/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: LogReader class for reading Write-Ahead Log (WAL) entries.
 *********************************************************/

#pragma once
#include "log_entry.h"
#include <cstdint>
#include <string>
#include <vector>

/// LogReader - Helper for reading and replaying WAL entries.
/// Responsibilities:
///  - Open and read from the WAL file.
///  - Parse log entries and provide an interface to iterate over them.
///  - Handle end-of-file and partial reads gracefully.
class LogReader
{
public:
    explicit LogReader(const std::string &log_path);
    ~LogReader();

    // Returns entries in order until EOF or corruption
    std::vector<LogEntry> read_all();

private:
    std::string log_path_;
};