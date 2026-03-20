/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: FollowerService — server-side mirror of LeaderReplicator.
 *   Implements codb::ReplicationService::Service (generated from kvstore.proto).
 *   The leader calls AppendEntries on this service after every write.
 *   A lagging follower calls RequestEntries on the leader's instance of this
 *   service to catch up (leader also registers FollowerService so it handles
 *   incoming RequestEntries RPCs from other nodes).
 *********************************************************/

#pragma once
#include <memory> // shared_ptr
#include <string>
#include "kvstore.grpc.pb.h" // codb::ReplicationService::Service
#include "kv_store.h"        // IKVStore
#include "replication_log.h" // ReplicationLog

class FollowerService final : public codb::ReplicationService::Service
{
public:
    // store : the node's own WalKVStore — entries are applied here after WAL write.
    // log   : the node's own ReplicationLog — WAL write happens here first.
    //         Non-owning raw pointer; main() owns the unique_ptr<ReplicationLog>.
    explicit FollowerService(std::shared_ptr<IKVStore> store,
                             ReplicationLog *log);

    // Receives a replicated entry from the leader: deserializes, idempotency-checks,
    // writes to follower WAL, applies to in-memory store, then acks with follower_seq.
    grpc::Status AppendEntries(grpc::ServerContext *ctx,
                               const codb::AppendEntriesRequest *req,
                               codb::AppendEntriesResponse *resp) override;

    // Streams all WAL entries >= req->from_seq() back to a lagging follower in
    // length-prefixed batches of up to 1000 entries each.
    grpc::Status RequestEntries(grpc::ServerContext *ctx,
                                const codb::FetchLogRequest *req,
                                grpc::ServerWriter<codb::FetchLogResponse> *writer) override;

private:
    std::shared_ptr<IKVStore> store_; // apply the entry to in-memory map here
    ReplicationLog *log_;             // non-owning; write to this node's WAL first
};
