/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: LogReader class coordinates write + recovery of the Write-Ahead Log (WAL).
 * Responsibilities:
 *  - Own and manage LogReader and LogWriter instances.
 *  - Provide a clean interface for the service layer to append entries and recover on startup.
 *  - Handle log file rotation and cleanup (not implemented in Phase 2, but designed for it).
 *********************************************************/

#pragma once
#include "log_entry.h"
#include "log_reader.h"
#include "log_writer.h"
#include <string>
#include <vector>
#include <memory>

class LogManager
{
public:
    // Opens (or creates) the WAL file at data_dir/wal.log
    explicit LogManager(const std::string &data_dir);

    // Append a PUT or DELETE entry durably (write + fsync)
    bool append(Optype op,
                const std::string &key,
                const std::string &value = "");

    // Read all valid entries from WAL (used during recovery)
    // Stops at first corrupt/partial entry — safe to call on truncated files
    std::vector<LogEntry> recover();

    // Truncate WAL after a snapshot checkpoint (Phase 9 preview)
    bool truncate();

private:
    std::string wal_path_;
    std::unique_ptr<LogWriter> writer_;
};