# Phase 3 — Leader–Follower Replication

### High Availability: Surviving a Node Failure

---

## The Problem

After Phase 2, CoDB survives a process crash — data is on disk.  
But what if the **disk itself fails**, or the entire **machine** dies?

Single node = Single point of failure.

```
Node 1 dies → entire database is offline
            → all data may be lost (if disk is destroyed)
```

**Solution:** Run multiple copies of the data across multiple machines.

---

## The Pattern: Leader–Follower Replication

```
          ┌──────────┐
Writes ──▶│  Leader  │──── replication log ────▶ Followers
Reads ◀──▶│          │                           │  │
          └──────────┘              ┌────────────┘  └────────────┐
                                    ▼                             ▼
                              ┌──────────┐               ┌──────────┐
                              │Follower 1│               │Follower 2│
                              └──────────┘               └──────────┘
```

**Rules:**

1. All **writes** go to the Leader
2. Leader applies the write locally, then ships it to Followers
3. Followers apply the same operations in the same order
4. Reads can go to Leader (consistent) or Followers (potentially stale)

Used in: **PostgreSQL, MySQL, MongoDB (primary-secondary)**

---

## The Replication Log

The WAL from Phase 2 becomes the **replication log**.  
This is not accidental — replication IS log shipping.

```
Leader WAL:
  [seq=1 | PUT  | user:1 | alice ]
  [seq=2 | PUT  | user:2 | bob   ]
  [seq=3 | DEL  | user:1 |       ]

Follower 1 applies these in order → identical state
Follower 2 applies these in order → identical state
```

The follower simply replays the leader's log.

---

## Replication Modes

### Synchronous Replication

```
Client                Leader              Follower
  │──── PUT ─────────▶│                      │
  │                   │──── replicate ───────▶│
  │                   │◀─── ack ─────────────│
  │◀──── success ─────│
```

**Pros:** Zero data loss. If leader crashes, follower has all data.  
**Cons:** Write latency includes one full network round-trip to the follower.

### Asynchronous Replication

```
Client                Leader
  │──── PUT ─────────▶│
  │◀──── success ─────│
  │                   │─────── replicate ──────▶ Follower (later)
```

**Pros:** Low write latency. Leader does not wait.  
**Cons:** Follower may lag. If leader crashes before replication, data is lost.

### Semi-Synchronous (Industry Standard)

```
Wait for at least 1 follower to acknowledge, not all.
```

This is what **MySQL semi-sync replication** uses.  
CoDB will implement this in Phase 4 (quorum) as a generalization.

---

## New Proto Messages

Extend `kvstore.proto` with replication RPC:

```proto
service ReplicationService {
  // Leader calls this on each follower
  rpc AppendEntries(AppendEntriesRequest) returns (AppendEntriesResponse);

  // Follower calls this to catch up after reconnect
  rpc FetchLog(FetchLogRequest) returns (stream LogEntryProto);
}

message AppendEntriesRequest {
  uint64 leader_seq     = 1;   // sequence number of this entry
  bytes  serialized_entry = 2; // serialized LogEntry
  string leader_id      = 3;
}

message AppendEntriesResponse {
  bool    success       = 1;
  uint64  follower_seq  = 2;   // highest seq the follower has
  string  error         = 3;
}

message FetchLogRequest {
  uint64 from_seq = 1;  // follower wants entries starting here
}
```

---

## Directory Structure Changes

```
codb/
└── src/
    ├── replication/
    │   ├── replication_log.h          ← NEW: wraps WAL for replication
    │   ├── replication_log.cpp        ← NEW
    │   ├── leader_replicator.h        ← NEW: ships log to followers
    │   ├── leader_replicator.cpp      ← NEW
    │   ├── follower_service.h         ← NEW: receives replicated entries
    │   └── follower_service.cpp       ← NEW
    │
    └── server/
        ├── kv_service_impl.cpp        ← MODIFY: route writes through replicator
        └── node_config.h              ← NEW: node role (leader/follower), peer list
```

---

## Node Configuration

Each node needs to know:

1. Its own identity (address + port)
2. Its role (leader or follower)
3. The addresses of all other nodes

```cpp
struct NodeConfig {
    std::string node_id;           // e.g. "node1"
    std::string listen_addr;       // e.g. "0.0.0.0:50051"
    bool is_leader;
    std::vector<std::string> peers; // e.g. ["node2:50052", "node3:50053"]
};
```

> Note: In Phase 7 (Raft), leader election will be automatic. For now, we configure it manually.

---

## Write Path (Leader)

```
Client PUT → KVServiceImpl::Put()
    ↓
Write to WAL (Phase 2)
    ↓
Apply to MemKVStore
    ↓
LeaderReplicator::replicate(entry)
    ├── send AppendEntries to Follower 1
    └── send AppendEntries to Follower 2
    ↓ (wait for ack if synchronous mode)
Return success to client
```

---

## Read Path

Two options — expose as configuration:

```
READ_MODE=leader_only   → always reads from leader (linearizable)
READ_MODE=any_replica   → may read stale data (eventual consistency)
```

For now, implement `leader_only`. Phase 4 introduces tunable consistency.

---

## Follower Catch-Up (Log Replay)

When a follower reconnects after being offline, it must catch up:

```
Follower: "My highest seq = 142"
Leader:   "I have entries up to seq = 289"
Leader:   streaming FetchLog(from_seq=143) → sends 143..289
Follower: applies each entry in order
Follower: "Now at seq = 289. Ready."
```

This is called **log-based replication** and is the foundation of Kafka, PostgreSQL streaming replication, and MySQL binlog.

---

## The Split-Brain Problem

If the network between leader and followers breaks:

```
                    ─ ─ ─ network partition ─ ─ ─

Leader ──writes──▶  ||  ◀──── Follower 1 (thinks leader is dead)
                    ||        Follower 2 (thinks leader is dead)
```

Followers may elect a new leader among themselves.  
Now you have **two leaders accepting writes** — split brain.

**How to prevent it:** A node can only become leader if it has acknowledgment from a **majority** of nodes (quorum).

- 3 nodes → need 2 to agree → one partition can have at most 1 quorum
- 5 nodes → need 3 to agree → only one partition can reach quorum

> This is why distributed systems typically use **odd numbers** of nodes.  
> Phase 7 (Raft) handles this automatically.

---

## Testing Plan

### 3-Node Cluster Test

```bash
# Terminal 1 (Leader)
./codb_server --id=node1 --port=50051 --leader --peers=node2:50052,node3:50053

# Terminal 2 (Follower 1)
./codb_server --id=node2 --port=50052 --leader-addr=localhost:50051

# Terminal 3 (Follower 2)
./codb_server --id=node3 --port=50053 --leader-addr=localhost:50051

# Terminal 4 (Client)
./codb_client localhost:50051 put city "mars"
./codb_client localhost:50052 get city    # Should return "mars" (after replication)
./codb_client localhost:50053 get city    # Should return "mars"
```

### Replication Lag Test

```bash
# Write 1000 keys rapidly to leader
# Query follower immediately → some may return "not found" (replication lag)
# Wait 100ms → query follower → all keys present
# Measure replication lag in milliseconds
```

### Leader Crash Test

```bash
# Write several keys to leader
# Kill leader process
# Query follower → all replicated data present
# Write to follower → SHOULD FAIL (follower is read-only in this phase)
# Phase 7 (Raft) will add automatic failover
```

---

## Key Concepts After This Phase

- Why the WAL is the natural replication log
- The difference between synchronous and asynchronous replication
- Why split-brain happens and why quorum prevents it
- How follower catch-up works via log replay
- Why reads from followers are potentially stale

---

## Tradeoffs Table

| Replication Mode     | Write Latency | Data Loss Risk | Read Freshness        |
| -------------------- | ------------- | -------------- | --------------------- |
| Async                | Lowest        | Yes (on crash) | Stale reads possible  |
| Sync (1 follower)    | +1 RTT        | No             | Leader reads: fresh   |
| Sync (all followers) | +N RTT        | No             | Fresh everywhere      |
| Semi-sync (quorum)   | +1 RTT        | Minimal        | Depends on which node |

---

## Git Commits for This Phase

```
feat(proto): add ReplicationService and AppendEntries RPC
feat(config): add NodeConfig with role and peer list
feat(replication): implement LeaderReplicator with sync mode
feat(replication): implement FollowerService for AppendEntries
feat(replication): implement follower catch-up via FetchLog stream
feat(server): route writes through LeaderReplicator
feat(server): add read mode configuration (leader_only/any)
test(replication): add 3-node cluster integration test
```

---

## Completion Criteria

- [ ] 3-node cluster starts with one leader and two followers
- [ ] Write to leader replicates to both followers within 50ms
- [ ] Follower that reconnects after downtime catches up via log replay
- [ ] Write to a follower directly returns an error
- [ ] Leader crash preserves all replicated data on followers

---

## Next → [Phase 4: Quorum Replication](05_phase4_quorum.md)
