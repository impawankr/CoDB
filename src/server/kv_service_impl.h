#pragma once
#include <atomic>
#include <memory>
#include <string>
#include "../storage/kv_store.h"
#include "kvstore.grpc.pb.h"
#include "leader_replicator.h" // LeaderReplicator — nullptr on followers

// KVServiceImpl — bridges gRPC client RPCs to the storage + replication layers.
//
// Role encoding via constructor injection (no is_leader flag needed in methods):
//   Leader:   KVServiceImpl(store, replicator)  — replicator_ != nullptr
//   Follower: KVServiceImpl(store, nullptr)     — replicator_ == nullptr
//
// Put/Delete check replicator_ first:
//   nullptr  → return FAILED_PRECONDITION ("not the leader")
//   non-null → write to WAL+memory via store_, then ship via replicator_->replicate()
//
// Get is always served from the in-memory map regardless of role — zero disk I/O.
class KVServiceImpl final : public codb::KVStore::Service
{
public:
    // replicator: pass nullptr for follower nodes.
    // Default arg keeps Phase 1/2 single-arg construction valid.
    explicit KVServiceImpl(std::shared_ptr<IKVStore> store,
                           LeaderReplicator *replicator = nullptr);

    grpc::Status Put(grpc::ServerContext *,
                     const codb::PutRequest *req,
                     codb::PutResponse *resp) override;
    grpc::Status Get(grpc::ServerContext *,
                     const codb::GetRequest *req,
                     codb::GetResponse *resp) override;
    grpc::Status Delete(grpc::ServerContext *,
                        const codb::DeleteRequest *req,
                        codb::DeleteResponse *resp) override;

private:
    std::shared_ptr<IKVStore> store_; // storage backend (WalKVStore)
    LeaderReplicator *replicator_;    // non-owning; nullptr on followers
    // Monotonically increasing counter stamped on every replicated entry.
    // seq_num 0 is reserved as "unset" — valid seqs start at 1.
    // The follower's idempotency check (leader_seq <= follower_last_seq) relies
    // on this being non-zero so the very first write isn't silently dropped.
    std::atomic<uint64_t> write_seq_{0};
};
