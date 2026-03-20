#include "kv_service_impl.h"

KVServiceImpl::KVServiceImpl(std::shared_ptr<IKVStore> store,
                             LeaderReplicator *replicator)
    : store_(std::move(store)), replicator_(replicator)
{
}

// ─────────────────────────────────────────────────────────────────────────────
// Put
//
// Follower guard: replicator_ is nullptr on followers — return FAILED_PRECONDITION
// so the client knows to retry against the leader.
//
// Leader path:
//   1. Write to local WAL + memory (WalKVStore::put handles both atomically).
//   2. Build a LogEntry and replicate to all followers via replicator_->replicate().
//      replicate() blocks until all followers ack (sync mode, Phase 3).
// ─────────────────────────────────────────────────────────────────────────────
grpc::Status KVServiceImpl::Put(grpc::ServerContext *,
                                const codb::PutRequest *req,
                                codb::PutResponse *resp)
{
    if (!replicator_)
    {
        resp->set_success(false);
        resp->set_error("not the leader — send writes to the leader node");
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "not the leader");
    }

    if (!store_->put(req->key(), req->value()))
    {
        resp->set_success(false);
        resp->set_error("local storage write failed");
        return grpc::Status::OK;
    }

    // Stamp a real monotonic seq so the follower's idempotency check
    // (leader_seq <= last_seq) works correctly. seq=0 would be treated as a
    // duplicate on a fresh follower whose last_seq is also 0.
    LogEntry entry;
    entry.seq_num = ++write_seq_;
    entry.op = Optype::PUT;
    entry.key = req->key();
    entry.value = req->value();

    bool ok = replicator_->replicate(entry);
    resp->set_success(ok);
    if (!ok)
        resp->set_error("replication to one or more followers failed");
    return grpc::Status::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Get — pure in-memory read, zero disk I/O, available on all nodes.
// ─────────────────────────────────────────────────────────────────────────────
grpc::Status KVServiceImpl::Get(grpc::ServerContext *,
                                const codb::GetRequest *req,
                                codb::GetResponse *resp)
{
    auto val = store_->get(req->key());
    if (val.has_value())
    {
        resp->set_found(true);
        resp->set_value(*val);
    }
    else
    {
        resp->set_found(false);
    }
    return grpc::Status::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Delete — same WAL-first + replicate pattern as Put.
// ─────────────────────────────────────────────────────────────────────────────
grpc::Status KVServiceImpl::Delete(grpc::ServerContext *,
                                   const codb::DeleteRequest *req,
                                   codb::DeleteResponse *resp)
{
    if (!replicator_)
    {
        resp->set_success(false);
        resp->set_error("not the leader — send writes to the leader node");
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "not the leader");
    }

    if (!store_->remove(req->key()))
    {
        resp->set_success(false);
        resp->set_error("local storage write failed");
        return grpc::Status::OK;
    }

    LogEntry entry;
    entry.seq_num = ++write_seq_;
    entry.op = Optype::DELETE;
    entry.key = req->key();
    // value is empty string for DELETE

    bool ok = replicator_->replicate(entry);
    resp->set_success(ok);
    if (!ok)
        resp->set_error("replication to one or more followers failed");
    return grpc::Status::OK;
}
