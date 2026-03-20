/*********************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: ReplicationLog — the replication layer's view of the WAL.
 *   Wraps LogManager and adds the two things raw WAL does not track:
 *     1. last_seq_  — the highest sequence number committed on this node.
 *     2. read_from  — read entries starting at a given seq (for catch-up).
 *   Both leader and follower own a ReplicationLog pointing at their own data_dir.
 *********************************************************/

#pragma once
#include <cstdint> // uint64_t
#include <memory>  // unique_ptr
#include <string>
#include <vector>
#include "log_entry.h"   // LogEntry, Optype
#include "log_manager.h" // LogManager

class ReplicationLog
{
public:
    // Opens (or creates) the WAL via LogManager(data_dir). last_seq_ starts at 0.
    explicit ReplicationLog(const std::string &data_dir);

    // Writes the entry to WAL (fsynced) and returns the stamped seq number.
    // Returns 0 on WAL failure — last_seq_ is not advanced.
    uint64_t append(Optype op,
                    const std::string &key,
                    const std::string &value = "");

    // Replays the WAL and restores last_seq_ to the highest committed seq.
    // Returns all valid entries for the caller to rebuild in-memory state.
    std::vector<LogEntry> recover();

    // Returns all entries with seq_num >= from_seq.
    // Used by the leader to answer a follower's RequestEntries catch-up RPC.
    std::vector<LogEntry> read_from(uint64_t from_seq);

    // Returns the highest seq this node has committed.
    // Follower puts this into AppendEntriesResponse.follower_seq so the leader
    // can calculate lag:  lag = leader.last_seq() - follower.last_seq()
    uint64_t last_seq() const;

private:
    std::unique_ptr<LogManager> log_;
    uint64_t last_seq_ = 0; // updated on every successful append() and recover()
};
