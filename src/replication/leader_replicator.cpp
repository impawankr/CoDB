/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: LeaderReplicator implementation.
 *********************************************************/

#include "leader_replicator.h"
#include <grpcpp/grpcpp.h>
#include <iostream>

// Creates one long-lived gRPC channel + stub per peer in config.peer_addresses.
// Channels are kept alive for the process lifetime to avoid per-write handshake overhead.
LeaderReplicator::LeaderReplicator(const NodeConfig &config)
    : leader_id_(config.node_id)
{
    // Create one long-lived gRPC channel + stub per peer.
    // Channels survive for the lifetime of this object — opening a new channel
    // per replicate() call would add a full TCP + TLS handshake to every write.
    for (const auto &[id, addr] : config.peer_addresses)
    {
        auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
        stubs_[id] = codb::ReplicationService::NewStub(channel);
        std::cout << "[leader] connected to peer " << id << " at " << addr << "\n";
    }
}

// Serializes the entry and sends AppendEntries to every follower (synchronous).
// Blocks until all followers ack or any one fails/times out (500 ms deadline).
// Phase 4 will replace this with parallel fan-out + majority quorum.
bool LeaderReplicator::replicate(const LogEntry &entry)
{
    // Serialize once; send the same bytes to every follower.
    auto bytes = entry.serialize();

    for (auto &[id, stub] : stubs_)
    {
        codb::AppendEntriesRequest req;
        req.set_leader_id(leader_id_);
        req.set_log_entries(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        req.set_leader_seq(entry.seq_num);

        // 500 ms deadline per follower — prevents a slow/dead peer from
        // blocking the leader indefinitely.  Phase 4 replaces this with
        // parallel fan-out + majority-quorum so one slow follower can't stall writes.
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(500));

        codb::AppendEntriesResponse resp;
        grpc::Status status = stub->AppendEntries(&ctx, req, &resp);

        if (!status.ok())
        {
            std::cerr << "[leader] AppendEntries to " << id
                      << " RPC failed: " << status.error_message() << "\n";
            return false;
        }
        if (!resp.success())
        {
            std::cerr << "[leader] AppendEntries to " << id
                      << " rejected: " << resp.error() << "\n";
            return false;
        }
    }
    return true; // all followers acked
}

// Opens a RequestEntries server-streaming call to follower_id and drains it.
// Called from a background thread when a follower is detected to be lagging.
bool LeaderReplicator::sync_follower(const std::string &follower_id, uint64_t from_seq)
{
    auto it = stubs_.find(follower_id);
    if (it == stubs_.end())
    {
        std::cerr << "[leader] sync_follower: unknown follower '" << follower_id << "'\n";
        return false;
    }

    codb::FetchLogRequest req;
    req.set_follower_id(follower_id);
    req.set_from_seq(from_seq);

    grpc::ClientContext ctx;
    auto stream = it->second->RequestEntries(&ctx, req);

    codb::FetchLogResponse batch;
    while (stream->Read(&batch))
    {
        (void)batch; // entries are applied on the follower side; this call drives the stream
    }

    grpc::Status status = stream->Finish();
    if (!status.ok())
        std::cerr << "[leader] sync_follower to '" << follower_id
                  << "' stream error: " << status.error_message() << "\n";
    return status.ok();
}
