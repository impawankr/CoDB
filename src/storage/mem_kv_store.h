#pragma once
#include "kv_store.h"
#include <unordered_map>
#include <shared_mutex>
#include <string>
#include <optional>

/**
 * MemKVStore — Phase 1: in-memory storage backend.
 *
 * Thread-safe via shared_mutex:
 *   - Reads use shared lock (multiple concurrent readers allowed)
 *   - Writes use exclusive lock (one writer at a time)
 *
 * Limitations (addressed in future phases):
 *   - Data is lost on process restart (Phase 2 adds WAL)
 *   - Cannot scale beyond a single machine (Phase 5 adds sharding)
 *   - Not optimized for range queries (Phase 9 adds LSM tree)
 */
class MemKVStore : public IKVStore {
public:
    bool put(const std::string& key, const std::string& value) override;
    std::optional<std::string> get(const std::string& key) override;
    bool remove(const std::string& key) override;

    // Utility: return number of stored keys (for testing)
    size_t size() const;

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::string> store_;
};
