/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: LogReader class for reading Write-Ahead Log (WAL) entries.
 *********************************************************/

// iterate WAL entries

#include "log_reader.h"
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

LogReader::LogReader(const std::string &log_path) : log_path_(log_path) {}

LogReader::~LogReader() {}

std::vector<LogEntry> LogReader::read_all()
{
    std::vector<LogEntry> entries;

    int fd = open(log_path_.c_str(), O_RDONLY);
    if (fd == -1)
    {
        // WAL file doesn't exist yet → this is a fresh server start, not an error.
        // Return empty list; the server will create the file on the first write.
        return entries;
    }

    // BUG 4 FIX: Use lseek to find exact file size instead of a fixed 1MB buffer.
    //   A fixed buffer silently truncates any WAL larger than that size, causing
    //   data loss on recovery. lseek(SEEK_END) gives us the precise byte count.
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0)
    {
        close(fd);
        return entries; // Empty WAL file — nothing to replay
    }
    lseek(fd, 0, SEEK_SET); // Rewind to beginning before reading

    // Allocate a buffer exactly as large as the file and read it in one call
    std::vector<uint8_t> buffer(static_cast<size_t>(file_size));
    ssize_t bytes_read = read(fd, buffer.data(), buffer.size());
    close(fd);

    if (bytes_read <= 0)
        return entries;

    // Parse entries sequentially until we reach the end or hit a corrupt entry
    size_t offset = 0;
    while (offset < static_cast<size_t>(bytes_read))
    {
        auto entry_opt = LogEntry::deserialize(buffer.data() + offset,
                                               bytes_read - offset);
        if (!entry_opt.has_value())
        {
            // CRC mismatch or partial entry — this is the corrupt tail left by a
            // mid-write crash. Stop here; everything before this offset is valid.
            break;
        }

        // BUG 3 FIX: Advance by the ACTUAL serialized size of this entry.
        //   Old code used: 13 + key.size() + value.size()  ← missing val_len(4B) + crc(4B)
        //   Correct formula: 21 + key_len + val_len
        //     breakdown: seq(8) + op(1) + key_len(4) + key + val_len(4) + val + crc(4)
        //   Off-by-8 caused every subsequent entry to be parsed at the wrong offset.
        size_t entry_bytes = 21 + entry_opt->key.size() + entry_opt->value.size();
        offset += entry_bytes;

        entries.push_back(std::move(*entry_opt));
    }

    return entries;
}
