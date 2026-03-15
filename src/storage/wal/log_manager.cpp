/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: LogManager — single owner of the WAL file.
 *   WalKVStore talks only to LogManager; it never touches LogWriter or
 *   LogReader directly.  This lets us change the WAL strategy (rotation,
 *   segmentation, compression) entirely inside LogManager without touching
 *   any other layer.
 *********************************************************/

#include "log_manager.h"
#include <filesystem> // std::filesystem::create_directories
#include <stdexcept>  // std::runtime_error

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// Creates the data directory if it does not already exist, then opens (or
// creates) the WAL file at  data_dir/wal.log  via LogWriter.
//
// Why does LogManager own the path, not the caller?
//   WalKVStore only knows it wants "durable storage in some directory".
//   The naming convention (wal.log), any future segment naming, and rotation
//   policy all live here.  Changing to  wal_000001.log / wal_000002.log  in
//   Phase 9 is a one-file edit inside LogManager.
// ─────────────────────────────────────────────────────────────────────────────
LogManager::LogManager(const std::string &data_dir)
{
    // Create the data directory and any missing parents (like mkdir -p).
    // If it already exists this is a no-op.
    std::filesystem::create_directories(data_dir);

    // Canonical WAL path:  <data_dir>/wal.log
    wal_path_ = data_dir + "/wal.log";

    // Open (or create) the WAL file.  LogWriter holds the file descriptor for
    // the lifetime of this LogManager.
    writer_ = std::make_unique<LogWriter>(wal_path_);
}

// ─────────────────────────────────────────────────────────────────────────────
// append
//
// Write one logical operation to the WAL and force it to disk before
// returning.  The caller (WalKVStore) must call this BEFORE applying the
// change to the in-memory map.
//
// Durability contract:
//   If append() returns true  → the entry is safely on disk.
//   If append() returns false → the entry was NOT written; caller must
//                               propagate the error back to the client.
//
// Sequence numbers:
//   We pass seq_num = 0 here.  LogWriter stamps the real monotone sequence
//   number (next_seq_num_++) before serializing — so the caller does not need
//   to track sequence numbers at all.
// ─────────────────────────────────────────────────────────────────────────────
bool LogManager::append(Optype op,
                        const std::string &key,
                        const std::string &value)
{
    // Build the entry.  seq_num will be overwritten by LogWriter.
    LogEntry entry;
    entry.seq_num = 0; // placeholder — LogWriter stamps the real value
    entry.op = op;
    entry.key = key;
    entry.value = value; // empty string for DELETE operations

    // Write serialized bytes (with CRC) to the file descriptor
    if (!writer_->append(entry))
        return false;

    // Force kernel page-cache bytes to physical disk (fsync).
    // Without this, the OS may buffer the write — a crash before the buffer
    // flushes would lose the entry even though append() "succeeded".
    return writer_->sync();
}

// ─────────────────────────────────────────────────────────────────────────────
// recover
//
// Read all valid entries from the WAL file in sequence order.
// Called once at startup, before the server begins accepting new requests.
//
// LogReader is stateless and cheap to construct — we create one on demand
// rather than keeping it alive.  This also avoids the awkward question of
// whether an open-for-reading fd interferes with the open-for-writing fd
// held by LogWriter.
//
// What LogManager does NOT do here:
//   It does not apply entries to any in-memory map.
//   That is WalKVStore's job — it calls recover(), then iterates the returned
//   vector and builds its own store_.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<LogEntry> LogManager::recover()
{
    // LogReader opens the file read-only, reads all valid entries (stopping at
    // the first CRC mismatch / partial write), then closes the file.
    LogReader reader(wal_path_);
    return reader.read_all();
}

// ─────────────────────────────────────────────────────────────────────────────
// truncate   (Phase 9 stub)
//
// After a snapshot checkpoint, the WAL can be safely discarded — the snapshot
// already encodes the full in-memory state.  truncate() will:
//
//   Phase 9 implementation:
//     1. Verify a valid snapshot file exists for the current state.
//     2. Re-open the WAL file with O_TRUNC to reset it to zero bytes.
//     3. Construct a new LogWriter pointed at the same path so the fd is
//        fresh and next_seq_num_ continues from where it left off.
//
// For now it is a no-op stub that returns true so the build compiles and
// callers can write:  log_manager_->truncate();  without error.
// ─────────────────────────────────────────────────────────────────────────────
bool LogManager::truncate()
{
    // TODO (Phase 9): implement post-snapshot WAL truncation
    return true;
}
