/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: LogEntry struct for Write-Ahead Log (WAL) implementation.
 *********************************************************/

#include "log_entry.h"
#include "log_writer.h"
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>
#include <optional>

// LogWriter constructor: opens the WAL file for appending and initializes sequence number
LogWriter::LogWriter(const std::string &log_path)
    : fd_(-1), next_seq_num_(0)
{
    fd_ = open(log_path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd_ == -1)
    {
        throw std::runtime_error("Failed to open WAL file: " + log_path);
    }
}

LogWriter::~LogWriter()
{
    if (fd_ != -1)
    {
        close(fd_);
    }
}

// Append a log entry to the WAL file. Returns true on success.
//
// BUG 2 FIX: next_seq_num_ was declared and initialised but never used.
//   We take a local copy of the entry so we can assign the sequence number
//   without mutating the caller's struct (the original is const&).
//   next_seq_num_++ assigns the current value then increments — every entry
//   gets a unique, monotonically increasing number, which is essential for:
//     - Deduplication during recovery (ignore already-applied seq numbers)
//     - Replication log ordering in Phase 3
bool LogWriter::append(const LogEntry &entry)
{
    // Stamp a unique sequence number onto a local copy before serializing
    LogEntry stamped = entry;
    stamped.seq_num = next_seq_num_++; // assign current, then increment

    std::vector<uint8_t> data = stamped.serialize();
    ssize_t written = write(fd_, data.data(), data.size());
    return written == static_cast<ssize_t>(data.size());
}

// Flush buffered data to disk.
//
// Why fsync instead of fdatasync?
//   On macOS with strict C++17 (-std=c++17, no extensions), Apple's SDK hides
//   fdatasync behind a _POSIX_C_SOURCE guard that isn't satisfied in strict mode.
//   fsync() is universally available and provides the same durability guarantee
//   we need: all written bytes are on persistent storage before this returns.
//   (On Linux, fdatasync is marginally faster because it skips flushing
//   metadata; we can add an #ifdef __linux__ optimisation later if needed.)
bool LogWriter::sync()
{
    return fsync(fd_) == 0;
}

// Note: In a production system, we would want to handle partial writes, retries, and error handling more robustly.