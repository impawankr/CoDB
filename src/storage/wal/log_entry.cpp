/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: LogEntry struct for Write-Ahead Log (WAL) implementation.
 *********************************************************/

#include "log_entry.h"
#include <optional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// CRC32 (IEEE 802.3 polynomial, no external deps)
//
// Why CRC32?
//   If the process crashes mid-write, the last bytes of the WAL file may be
//   partial garbage. CRC lets the reader detect and discard that corrupt tail
//   instead of replaying garbage data into the in-memory store.
//
// How it works:
//   We compute CRC32 over all bytes of the entry (header + key + value).
//   That 4-byte checksum is appended as the very last field.
//   On read, we recompute CRC over all bytes except the last 4, then compare.
//   Mismatch → treat entry as corrupt → stop recovery at that point.
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t crc32_compute(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
        {
            // IEEE 802.3 reversed polynomial
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

// ─────────────────────────────────────────────────────────────────────────────
// On-disk binary layout (all multi-byte fields are big-endian):
//
//  ┌──────────┬────┬─────────┬──────────┬─────────┬──────────┬────────┐
//  │ seq (8B) │op  │key_len  │  key     │ val_len │  value   │ crc32  │
//  │          │(1B)│  (4B)   │(variable)│  (4B)   │(variable)│  (4B)  │
//  └──────────┴────┴─────────┴──────────┴─────────┴──────────┴────────┘
//  Total fixed overhead = 21 bytes. Total size = 21 + key_len + val_len.
// ─────────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> LogEntry::serialize() const
{
    // Reserve exact capacity: 21 fixed bytes + variable key + variable value
    std::vector<uint8_t> data;
    data.reserve(21 + key.size() + value.size());

    // seq_num (8 bytes, big-endian)
    for (int i = 0; i < 8; ++i)
        data.push_back((seq_num >> (56 - i * 8)) & 0xFF);

    // op (1 byte)
    data.push_back(static_cast<uint8_t>(op));

    // key_len (4 bytes, big-endian) + key bytes
    uint32_t key_len = static_cast<uint32_t>(key.size());
    for (int i = 0; i < 4; ++i)
        data.push_back((key_len >> (24 - i * 8)) & 0xFF);
    data.insert(data.end(), key.begin(), key.end());

    // val_len (4 bytes, big-endian) + value bytes
    uint32_t val_len = static_cast<uint32_t>(value.size());
    for (int i = 0; i < 4; ++i)
        data.push_back((val_len >> (24 - i * 8)) & 0xFF);
    data.insert(data.end(), value.begin(), value.end());

    // BUG 1 FIX: CRC32 over all bytes written so far (everything except the checksum)
    // Appended as the last 4 bytes so the reader can verify before applying the entry.
    uint32_t crc = crc32_compute(data.data(), data.size());
    for (int i = 0; i < 4; ++i)
        data.push_back((crc >> (24 - i * 8)) & 0xFF);

    return data;
}

std::optional<LogEntry> LogEntry::deserialize(const uint8_t *data, size_t len)
{
    // Minimum valid entry: 21 bytes (all fixed fields, zero-length key and value)
    if (len < 21)
        return std::nullopt;

    LogEntry entry;

    // seq_num (8 bytes)
    entry.seq_num = 0;
    for (int i = 0; i < 8; ++i)
        entry.seq_num = (entry.seq_num << 8) | data[i];

    // op (1 byte)
    entry.op = static_cast<Optype>(data[8]);

    // key_len (4 bytes) → need 13 + key_len bytes available before val_len field
    uint32_t key_len = 0;
    for (int i = 0; i < 4; ++i)
        key_len = (key_len << 8) | data[9 + i];
    if (len < 17 + key_len) // 13(header so far) + key_len + 4(val_len field)
        return std::nullopt;

    entry.key = std::string(reinterpret_cast<const char *>(data + 13), key_len);

    // val_len (4 bytes) → starts at offset 13 + key_len
    uint32_t val_len = 0;
    for (int i = 0; i < 4; ++i)
        val_len = (val_len << 8) | data[13 + key_len + i];

    // Total entry size = 21 fixed bytes + key_len + val_len
    size_t total = 21 + key_len + val_len;
    if (len < total)
        return std::nullopt; // Partial write — treat as corrupt tail

    entry.value = std::string(reinterpret_cast<const char *>(data + 17 + key_len), val_len);

    // BUG 1 FIX: Verify CRC32 — compute over all bytes except the last 4 (checksum field)
    // If they don't match, this entry was partially written (crash mid-write). Discard it.
    uint32_t expected_crc = crc32_compute(data, total - 4);
    uint32_t stored_crc = 0;
    for (int i = 0; i < 4; ++i)
        stored_crc = (stored_crc << 8) | data[17 + key_len + val_len + i];
    if (expected_crc != stored_crc)
        return std::nullopt; // CRC mismatch → corrupt entry, stop recovery here

    return entry;
}