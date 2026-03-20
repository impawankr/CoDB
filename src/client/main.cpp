// -----------------------------------------------------------------------------
// codb_client — command-line client with automatic node failover
//
// Usage:
//   ./codb_client <addr>[,<addr2>,<addr3>] <cmd> [args...]
//
//   Addresses are tried left-to-right:
//     GET    — uses the first node that responds (reads work on any replica)
//     PUT    — skips followers that return FAILED_PRECONDITION (not the leader)
//     DELETE — same as PUT
//
// Examples:
//   ./codb_client localhost:50051 get city
//   ./codb_client localhost:50051,localhost:50052,localhost:50053 get city
//   ./codb_client localhost:50051,localhost:50052,localhost:50053 put city mars
// -----------------------------------------------------------------------------
#include "kvstore.grpc.pb.h"
#include "kvstore.pb.h"
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Split "a,b,c" → ["a", "b", "c"]
static std::vector<std::string> split_addrs(const std::string &s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ','))
        if (!token.empty())
            out.push_back(token);
    return out;
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::cerr << "Usage:\n"
                  << "  " << argv[0] << " <host:port>[,<host:port>,...] put <key> <value>\n"
                  << "  " << argv[0] << " <host:port>[,<host:port>,...] get <key>\n"
                  << "  " << argv[0] << " <host:port>[,<host:port>,...] delete <key>\n\n"
                  << "Separate multiple addresses with commas for automatic failover.\n"
                  << "GET tries each address until one responds.\n"
                  << "PUT/DELETE skips followers (FAILED_PRECONDITION) and retries on the next address.\n";
        return 1;
    }

    auto addrs = split_addrs(argv[1]);
    if (addrs.empty())
    {
        std::cerr << "Error: no addresses provided\n";
        return 1;
    }

    std::string cmd = argv[2];
    std::string key = argv[3];

    for (const auto &addr : addrs)
    {
        auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
        auto stub = codb::KVStore::NewStub(channel);

        // ── GET ────────────────────────────────────────────────────────────────
        // Any node (leader or follower) can answer a GET from its in-memory map.
        // Try each address until one is reachable.
        if (cmd == "get")
        {
            codb::GetRequest req;
            req.set_key(key);
            codb::GetResponse resp;
            grpc::ClientContext ctx;
            auto status = stub->Get(&ctx, req, &resp);

            if (!status.ok())
            {
                std::cerr << "[" << addr << "] unreachable: " << status.error_message()
                          << " — trying next...\n";
                continue; // try next address
            }

            if (resp.found())
                std::cout << resp.value() << "\n";
            else
                std::cout << "(not found)\n";
            return 0;
        }

        // ── PUT ────────────────────────────────────────────────────────────────
        else if (cmd == "put")
        {
            if (argc < 5)
            {
                std::cerr << "Error: 'put' requires a value\n";
                return 1;
            }
            codb::PutRequest req;
            req.set_key(key);
            req.set_value(argv[4]);
            codb::PutResponse resp;
            grpc::ClientContext ctx;
            auto status = stub->Put(&ctx, req, &resp);

            if (!status.ok())
            {
                // FAILED_PRECONDITION = this node is a follower, not the leader
                if (status.error_code() == grpc::StatusCode::FAILED_PRECONDITION)
                {
                    std::cerr << "[" << addr << "] not the leader — trying next...\n";
                    continue;
                }
                std::cerr << "[" << addr << "] unreachable: " << status.error_message()
                          << " — trying next...\n";
                continue;
            }

            if (resp.success())
                std::cout << "ok\n";
            else
                std::cerr << "Error: " << resp.error() << "\n";
            return 0;
        }

        // ── DELETE ─────────────────────────────────────────────────────────────
        else if (cmd == "delete")
        {
            codb::DeleteRequest req;
            req.set_key(key);
            codb::DeleteResponse resp;
            grpc::ClientContext ctx;
            auto status = stub->Delete(&ctx, req, &resp);

            if (!status.ok())
            {
                if (status.error_code() == grpc::StatusCode::FAILED_PRECONDITION)
                {
                    std::cerr << "[" << addr << "] not the leader — trying next...\n";
                    continue;
                }
                std::cerr << "[" << addr << "] unreachable: " << status.error_message()
                          << " — trying next...\n";
                continue;
            }

            if (resp.success())
                std::cout << "ok\n";
            else
                std::cerr << "Error: " << resp.error() << "\n";
            return 0;
        }

        else
        {
            std::cerr << "Unknown command: " << cmd
                      << "  (expected: get | put | delete)\n";
            return 1;
        }
    }

    // Exhausted all addresses
    std::cerr << "Error: all nodes unreachable or rejected the request.\n";
    if (cmd == "put" || cmd == "delete")
        std::cerr << "  Writes require the leader to be online.\n";
    return 1;
}
