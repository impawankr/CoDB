/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: LogEntry struct for Write-Ahead Log (WAL) implementation.
 *********************************************************/

#pragma once
#include <cstdint>  // uint8_t, uint64_t
#include <optional> // std::optional used in deserialize()
#include <string>
#include <vector>

enum class Optype : uint8_t
{
    PUT = 0,
    DELETE = 1
};

struct LogEntry
{
    uint64_t seq_num;
    Optype op;
    std::string key;
    std::string value; // only used for PUT operations

    // Serialize LogEntry to a string for writing to WAL
    std::vector<uint8_t> serialize() const;
    // Deserialize a string read from WAL back into a LogEntry
    static std::optional<LogEntry> deserialize(const uint8_t *data, size_t len);
};
