#include "mem_kv_store.h"

bool MemKVStore::put(const std::string& key, const std::string& value) {
    std::unique_lock lock(mu_);
    store_[key] = value;
    return true;
}

std::optional<std::string> MemKVStore::get(const std::string& key) {
    std::shared_lock lock(mu_);
    auto it = store_.find(key);
    if (it == store_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool MemKVStore::remove(const std::string& key) {
    std::unique_lock lock(mu_);
    return store_.erase(key) > 0;
}

size_t MemKVStore::size() const {
    std::shared_lock lock(mu_);
    return store_.size();
}
