/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: FollowerService implementation.
 *********************************************************/

#include "follower_service.h"

FollowerService::FollowerService(std::shared_ptr<IKVStore> store,
                                 ReplicationLog *log)
    : store_(std::move(store)), log_(log)
{
}

// Called by the leader for every new write.
// Deserializes the entry, guards against duplicates, writes to the follower's
// WAL first, then applies to the in-memory store. Returns follower_seq so the
// leader can track replication lag.
grpc::Status FollowerService::AppendEntries(grpc::ServerContext *,
                                            const codb::AppendEntriesRequest *req,
                                            codb::AppendEntriesResponse *resp)
{
    // ── 1. Deserialize ────────────────────────────────────────────────────────
    // The leader serialized via LogEntry::serialize() — CRC is verified here.
    const std::string &raw = req->log_entries();
    auto entry_opt = LogEntry::deserialize(
        reinterpret_cast<const uint8_t *>(raw.data()), raw.size());

    if (!entry_opt)
    {
        resp->set_success(false);
        resp->set_error("corrupt entry: CRC mismatch or truncated bytes");
        return grpc::Status::OK;
    }

    // ── 2. Idempotency guard ──────────────────────────────────────────────────
    // If we already have this seq (e.g. leader retried after a network hiccup)
    // just ack — writing it again would create a duplicate WAL entry.
    if (req->leader_seq() <= log_->last_seq())
    {
        resp->set_success(true);
        resp->set_follower_seq(log_->last_seq());
        return grpc::Status::OK;
    }

    // ── 3. WAL-first write ────────────────────────────────────────────────────
    // Write to disk BEFORE updating the in-memory map.  If we crash between
    // the WAL append and the map update, recovery replays the WAL and fixes it.
    uint64_t seq = log_->append(entry_opt->op, entry_opt->key, entry_opt->value);
    if (seq == 0)
    {
        resp->set_success(false);
        resp->set_error("follower WAL write failed");
        return grpc::Status::OK;
    }

    // ── 4. Apply to in-memory store ───────────────────────────────────────────
    if (entry_opt->op == Optype::PUT)
        store_->put(entry_opt->key, entry_opt->value);
    else if (entry_opt->op == Optype::DELETE)
        store_->remove(entry_opt->key);

    // ── 5. Respond ────────────────────────────────────────────────────────────
    // Return follower_seq so the leader can measure replication lag:
    //   lag = req->leader_seq() - resp->follower_seq()
    resp->set_success(true);
    resp->set_follower_seq(log_->last_seq());
    return grpc::Status::OK;
}

// Called by a lagging follower to catch up. Streams all WAL entries >= from_seq
// back in length-prefixed batches of up to 1000 entries each.
grpc::Status FollowerService::RequestEntries(grpc::ServerContext *,
                                             const codb::FetchLogRequest *req,
                                             grpc::ServerWriter<codb::FetchLogResponse> *writer)
{
    auto entries = log_->read_from(req->from_seq());
    if (entries.empty())
        return grpc::Status::OK; // follower is already caught up

    // Send entries in batches of up to 1000 to bound individual message size.
    // Each FetchLogResponse contains length-prefixed serialized LogEntry records:
    //   [ len(4B) | entry_bytes | len(4B) | entry_bytes | ... ]
    // The follower decodes each entry by reading len then deserializing.
    const size_t BATCH_SIZE = 1000;
    for (size_t i = 0; i < entries.size(); i += BATCH_SIZE)
    {
        size_t end = std::min(i + BATCH_SIZE, entries.size());

        std::string packed;
        for (size_t j = i; j < end; ++j)
        {
            auto b = entries[j].serialize();
            uint32_t len = static_cast<uint32_t>(b.size());
            packed.append(reinterpret_cast<const char *>(&len), 4);
            packed.append(reinterpret_cast<const char *>(b.data()), b.size());
        }

        codb::FetchLogResponse batch;
        batch.set_log_entries(packed);
        batch.set_last_seq(entries[end - 1].seq_num);

        if (!writer->Write(batch))
        {
            // Client disconnected mid-stream — not an error on our side.
            return grpc::Status::OK;
        }
    }

    return grpc::Status::OK;
}
