/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: LeaderReplicator — ships a log entry to every follower.
 *   Called by KVServiceImpl::Put / Delete AFTER the local WAL write succeeds.
 *   Synchronous mode (Phase 3): blocks until ALL followers ack before returning.
 *   Phase 4 upgrade: replicate() becomes replicate_quorum() — returns true as
 *   soon as a majority (not all) followers respond.
 *********************************************************/

#pragma once
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "log_entry.h"             // LogEntry
#include "replication_log.h"       // ReplicationLog (for leader_seq)
#include "kvstore.grpc.pb.h"       // codb::ReplicationService::Stub
#include "../server/node_config.h" // NodeConfig, NodeRole

class LeaderReplicator
{
public:
    // Creates one long-lived gRPC channel + stub per peer in config.peer_addresses.
    explicit LeaderReplicator(const NodeConfig &config);

    // Serializes the entry and sends AppendEntries to every follower synchronously.
    // Blocks until all followers ack or any one times out (500 ms). Returns false on failure.
    // Phase 4 will change this to majority-quorum fan-out.
    bool replicate(const LogEntry &entry);

    // Opens a RequestEntries streaming call to follower_id starting at from_seq.
    // Used to catch up a lagging follower. Call from a background thread.
    bool sync_follower(const std::string &follower_id, uint64_t from_seq);

private:
    std::string leader_id_;

    // One long-lived stub per follower, keyed by node_id.
    // Using a map lets sync_follower() look up by id without a linear scan.
    // codb::ReplicationService::NewStub(channel) returns a unique_ptr<Stub>.
    std::map<std::string, std::unique_ptr<codb::ReplicationService::Stub>> stubs_;
};
