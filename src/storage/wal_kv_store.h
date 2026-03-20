/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: LogEntry struct for Write-Ahead Log (WAL) implementation.
 *********************************************************/

#pragma once
#include <memory>       // unique_ptr
#include <optional>     // optional
#include <shared_mutex> // shared_mutex
#include <string>
#include <unordered_map>
#include "kv_store.h"    // IKVStore
#include "log_manager.h" // LogManager (owns LogWriter + LogReader)

// WalKVStore — IKVStore backed by a Write-Ahead Log.
//
// Write path:  WAL append (durable) → in-memory map update
// Read path:   in-memory map only (zero disk I/O)
// Recovery:    LogManager::recover() replays WAL into store_ at startup
class WalKVStore : public IKVStore
{
public:
    // Opens (or creates) the WAL via LogManager and replays any existing log.
    explicit WalKVStore(const std::string &data_dir);

    bool put(const std::string &key, const std::string &value) override;
    std::optional<std::string> get(const std::string &key) override;
    bool remove(const std::string &key) override;

    // Replays the WAL into store_ on startup.
    // Must be called once from main() after construction.
    void recover();

private:
    std::unique_ptr<LogManager> writer_; // owns the WAL file
    std::unordered_map<std::string, std::string> store_;
    mutable std::shared_mutex mu_;
};