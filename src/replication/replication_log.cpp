/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: ReplicationLog implementation.
 *********************************************************/

#include "replication_log.h"

ReplicationLog::ReplicationLog(const std::string &data_dir)
    : log_(std::make_unique<LogManager>(data_dir))
{
    // last_seq_ starts at 0; recover() or append() will update it.
}

// Writes the entry durably to WAL (fsynced), then advances last_seq_.
// Returns the stamped sequence number; returns 0 on WAL failure.
uint64_t ReplicationLog::append(Optype op,
                                const std::string &key,
                                const std::string &value)
{
    // Write durably to WAL first (LogManager fsyncs internally).
    if (!log_->append(op, key, value))
        return 0; // WAL write failed — do NOT advance last_seq_

    // Stamp our own monotonic counter.  Only one writer thread calls append()
    // in Phase 3 (the leader's Put/Delete handler), so no mutex is needed yet.
    // Phase 4 note: if concurrent appends are added, guard last_seq_ with a mutex.
    return ++last_seq_;
}

// Replays the WAL and restores last_seq_ to the highest committed sequence number.
// Returns all valid entries so the caller can rebuild in-memory state.
std::vector<LogEntry> ReplicationLog::recover()
{
    auto entries = log_->recover();
    // Take the max seq_num seen — WAL may have a corrupt tail entry that was
    // partially written before a crash, so we cannot just use entries.back().
    for (const auto &e : entries)
        if (e.seq_num > last_seq_)
            last_seq_ = e.seq_num;
    return entries;
}

// Returns all WAL entries with seq_num >= from_seq.
// Used by the leader to answer a follower's RequestEntries catch-up RPC.
// Returns empty if from_seq > last_seq_ (follower is already up to date).
std::vector<LogEntry> ReplicationLog::read_from(uint64_t from_seq)
{
    if (from_seq > last_seq_)
        return {}; // follower is already caught up

    // Re-read from disk.  Phase 9 optimisation: maintain an in-memory index
    // by seq_num to avoid a full WAL scan on every catch-up request.
    auto all = log_->recover();
    std::vector<LogEntry> result;
    for (auto &e : all)
        if (e.seq_num >= from_seq)
            result.push_back(std::move(e));
    return result;
}

uint64_t ReplicationLog::last_seq() const
{
    return last_seq_;
}
