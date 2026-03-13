#pragma once
#include <string>
#include <optional>

/**
 * IKVStore — Abstract storage interface.
 *
 * ALL storage backends (in-memory, WAL-backed, LSM) implement this.
 * The gRPC service layer only talks to this interface — never to a concrete class.
 *
 * This boundary is critical:
 *   Phase 2: swap MemKVStore → WalKVStore   (zero changes to service layer)
 *   Phase 9: swap WalKVStore → LSMEngine    (zero changes to service layer)
 */
class IKVStore {
public:
    virtual ~IKVStore() = default;

    /**
     * Store a key-value pair.
     * @return true on success, false on storage failure
     */
    virtual bool put(const std::string& key, const std::string& value) = 0;

    /**
     * Retrieve a value by key.
     * @return std::nullopt if key does not exist
     */
    virtual std::optional<std::string> get(const std::string& key) = 0;

    /**
     * Remove a key.
     * @return true if key existed and was removed, false if key not found
     */
    virtual bool remove(const std::string& key) = 0;
};
