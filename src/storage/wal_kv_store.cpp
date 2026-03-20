/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: WalKVStore — a WAL-backed implementation of IKVStore.
 *********************************************************/

#include "wal_kv_store.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
//
// Creates LogManager → creates data_dir if needed, opens data_dir/wal.log.
// Does NOT replay the WAL here — caller must invoke recover() explicitly
// after construction (see main.cpp) so recovery happens at a predictable
// point before the gRPC server starts accepting requests.
// ─────────────────────────────────────────────────────────────────────────────
WalKVStore::WalKVStore(const std::string &data_dir)
    : writer_(std::make_unique<LogManager>(data_dir))
{
}

// ─────────────────────────────────────────────────────────────────────────────
// put
//
// WAL-first write protocol:
//   1. Append PUT entry to WAL and fsync (durable on disk).
//   2. Only then update the in-memory map.
// If step 1 fails we return false immediately — store_ is not modified,
// so the client gets an error and can retry.  No partial state is exposed.
// ─────────────────────────────────────────────────────────────────────────────
bool WalKVStore::put(const std::string &key, const std::string &value)
{
    if (!writer_->append(Optype::PUT, key, value))
        return false;

    std::unique_lock lock(mu_);
    store_[key] = value;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// get
//
// Pure in-memory read — zero disk I/O.  This is the payoff for maintaining
// store_ as a fully-replayed shadow of the WAL.
// ─────────────────────────────────────────────────────────────────────────────
std::optional<std::string> WalKVStore::get(const std::string &key)
{
    std::shared_lock lock(mu_);
    auto it = store_.find(key);
    if (it == store_.end())
        return std::nullopt;
    return it->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// remove
//
// Same WAL-first protocol as put().  A DELETE entry is appended so that on
// recovery the key is erased even if it was PUT in a prior session.
// ─────────────────────────────────────────────────────────────────────────────
bool WalKVStore::remove(const std::string &key)
{
    if (!writer_->append(Optype::DELETE, key, ""))
        return false;

    std::unique_lock lock(mu_);
    store_.erase(key);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// recover (public)
//
// Delegates to LogManager::recover() which constructs a LogReader, reads all
// valid entries (stopping at the first CRC mismatch / partial write), and
// returns them.  We then replay each entry into store_.
//
// Why does WalKVStore apply the entries instead of LogManager?
//   LogManager owns the raw log file.  It has no knowledge of the in-memory
//   map.  WalKVStore owns both and decides how log entries map to store_ state.
//   This keeps the two layers independent — LogManager can be swapped for a
//   segmented WAL without touching WalKVStore at all.
// ─────────────────────────────────────────────────────────────────────────────
void WalKVStore::recover()
{
    // LogManager reads the WAL file and returns valid entries in order.
    auto entries = writer_->recover();

    std::unique_lock lock(mu_);
    for (const auto &entry : entries)
    {
        if (entry.op == Optype::PUT)
            store_[entry.key] = entry.value;
        else if (entry.op == Optype::DELETE)
            store_.erase(entry.key);
    }
}
