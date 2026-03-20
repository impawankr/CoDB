// -----------------------------------------------------------------------------
// codb_server — entry point
//
// Parses flags, wires storage + replication, and starts gRPC server.
//
// Usage:
//   ./codb_server \
//     --node-id   node1              \   # unique name for this node
//     --port      50051              \   # gRPC listen port
//     --role      leader             \   # "leader" | "follower"
//     --peer      node2=localhost:50052 \ # repeatable; id=addr pairs
//     --peer      node3=localhost:50053 \
//     --data-dir  ./data/node1           # WAL + storage directory
//
// Design:
//   - WalKVStore  : durable KV backend (WAL-backed)
//   - ReplicationLog : thin wrapper around LogManager for replication tracking
//   - LeaderReplicator : created only on the leader; pushes entries to peers
//   - FollowerService  : registered on ALL nodes so peers can call AppendEntries
//   - KVServiceImpl    : if replicator is nullptr → write RPCs are rejected
//                        (followers are effectively read-only via the KV API)
// -----------------------------------------------------------------------------

#include "kv_service_impl.h"
#include "wal_kv_store.h"
#include "node_config.h"
#include "leader_replicator.h"
#include "follower_service.h"
#include "replication_log.h"

#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

// ---------------------------------------------------------------------------
// Simple flag parser
//   --key value   or   --key=value
// Returns the string after "--" as the key.
// Caller iterates argv manually so we inline it here.
// ---------------------------------------------------------------------------
static NodeConfig parse_flags(int argc, char **argv)
{
    NodeConfig cfg;
    cfg.data_dir = "./data"; // sensible defaults
    cfg.role = NodeRole::FOLLOWER;
    std::string node_id;
    std::string port = "50051";

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        auto value_of = [&]() -> std::string
        {
            // Support both  --flag value  and  --flag=value
            auto eq = arg.find('=');
            if (eq != std::string::npos)
                return arg.substr(eq + 1);
            if (i + 1 < argc)
                return argv[++i];
            throw std::runtime_error("Missing value for flag: " + arg);
        };

        if (arg == "--node-id" || arg.rfind("--node-id=", 0) == 0)
        {
            node_id = value_of();
        }
        else if (arg == "--port" || arg.rfind("--port=", 0) == 0)
        {
            port = value_of();
        }
        else if (arg == "--data-dir" || arg.rfind("--data-dir=", 0) == 0)
        {
            cfg.data_dir = value_of();
        }
        else if (arg == "--role" || arg.rfind("--role=", 0) == 0)
        {
            std::string r = value_of();
            if (r == "leader")
                cfg.role = NodeRole::LEADER;
            else
                cfg.role = NodeRole::FOLLOWER;
        }
        else if (arg == "--peer" || arg.rfind("--peer=", 0) == 0)
        {
            // Expected format:  id=host:port
            std::string peer = value_of();
            auto sep = peer.find('=');
            if (sep == std::string::npos)
                throw std::runtime_error("--peer must be id=host:port, got: " + peer);
            std::string peer_id = peer.substr(0, sep);
            std::string peer_addr = peer.substr(sep + 1);
            cfg.peer_addresses[peer_id] = peer_addr;
        }
    }

    if (node_id.empty())
    {
        std::cerr << "[codb] WARNING: --node-id not set, defaulting to 'node0'\n";
        node_id = "node0";
    }

    cfg.node_id = node_id;
    cfg.listen_addr = "0.0.0.0:" + port;
    return cfg;
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    // ---- 1. Parse flags -------------------------------------------------------
    NodeConfig config = parse_flags(argc, argv);

    std::cout << "[codb] node_id    : " << config.node_id << "\n";
    std::cout << "[codb] listen_addr: " << config.listen_addr << "\n";
    std::cout << "[codb] role       : " << (config.role == NodeRole::LEADER ? "LEADER" : "FOLLOWER") << "\n";
    std::cout << "[codb] data_dir   : " << config.data_dir << "\n";
    for (auto &[id, addr] : config.peer_addresses)
        std::cout << "[codb] peer       : " << id << " -> " << addr << "\n";

    // ---- 2. Storage -----------------------------------------------------------
    // WalKVStore keeps both the in-memory map and the WAL file in sync.
    // recover() replays the WAL on startup to rebuild in-memory state.
    auto store = std::make_shared<WalKVStore>(config.data_dir);
    store->recover();
    std::cout << "[codb] WAL recovered from " << config.data_dir << "\n";

    // ---- 3. Replication log ---------------------------------------------------
    // ReplicationLog wraps LogManager and tracks the last sequence number
    // applied so followers can request catch-up entries efficiently.
    auto rep_log = std::make_unique<ReplicationLog>(config.data_dir);

    // ---- 4. Leader replicator (leaders only) ---------------------------------
    // LeaderReplicator holds one gRPC stub per peer and fans out writes.
    // Followers leave this as nullptr; KVServiceImpl will reject write RPCs.
    std::unique_ptr<LeaderReplicator> replicator;
    if (config.role == NodeRole::LEADER)
    {
        if (config.peer_addresses.empty())
        {
            std::cout << "[codb] Leader has no peers — single-node mode\n";
        }
        replicator = std::make_unique<LeaderReplicator>(config);
    }

    // ---- 5. gRPC services -----------------------------------------------------
    // KVServiceImpl  : client-facing (Put / Get / Delete)
    // FollowerService: peer-facing  (AppendEntries / RequestEntries)
    //   Both run on the same port — gRPC multiplexes by service name.
    KVServiceImpl kv_service(store, replicator.get());

    FollowerService follower_service(store, rep_log.get());

    // ---- 6. Build and start server --------------------------------------------
    grpc::ServerBuilder builder;
    builder.AddListeningPort(config.listen_addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&kv_service);
    builder.RegisterService(&follower_service);

    auto server = builder.BuildAndStart();
    std::cout << "[codb] Server listening on " << config.listen_addr << "\n";
    server->Wait();
    return 0;
}
