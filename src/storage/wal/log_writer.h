/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: LogWriter — durable append-only WAL writer.
 *********************************************************/

#pragma once // include guard must come before any #include
#include "log_entry.h"
#include <cstdint>
#include <string>

/// LogWriter - Append-only Write-Ahead Log (WAL) helper.
/// Responsibilities:
///  - Own and manage the WAL file descriptor lifetime.
///  - Generate monotonically increasing sequence numbers for entries.
///  - Provide durable append (append + sync) semantics.
///  - Be non-copyable to avoid accidental duplication of the file descriptor.

class LogWriter
{
public:
    explicit LogWriter(const std::string &log_path);
    ~LogWriter();

    // Append a log entry to the WAL file. Returns true on success.
    bool append(const LogEntry &entry);
    bool sync(); // Flush buffered data to disk. (fdatasync)

private:
    int fd_;                // File descriptor for the WAL file
    uint64_t next_seq_num_; // Sequence number for the next log entry
};