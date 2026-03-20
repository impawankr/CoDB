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

## File-by-File Design — Intuition and Contents

### Mental model before reading any code

Think of the system in terms of **who owns what responsibility**:

```
┌─────────────────────────────────────────────────────────────────┐
│  src/server/          "the front door"                          │
│  Talks to clients. Does NOT know how storage or network works.  │
├─────────────────────────────────────────────────────────────────┤
│  src/replication/     "the postal service"                      │
│  Moves log entries between nodes. Does NOT know about clients.  │
├─────────────────────────────────────────────────────────────────┤
│  src/storage/         "the filing cabinet"                      │
│  Reads and writes data. Does NOT know about the network at all. │
└─────────────────────────────────────────────────────────────────┘
```

Each layer calls down, never up. `kv_service_impl` calls `LeaderReplicator`.
`LeaderReplicator` calls gRPC stubs. Nothing in `replication/` ever touches `kv_service_impl`.

---

### `src/server/node_config.h` — "who am I and who are my neighbours?"

This is the simplest file but the most load-bearing for Phase 3.
Every node needs answers to three questions before it can do anything:

```cpp
struct NodeConfig {
    std::string node_id;            // "node1" — unique name used in log messages and leader_id field
    std::string listen_addr;        // "0.0.0.0:50051" — what addr this process binds to
    bool        is_leader;          // true  → accept writes, replicate out
                                    // false → reject writes, receive replication in
    std::vector<std::string> peers; // ["localhost:50052", "localhost:50053"]
                                    // only used on the leader — the list of followers to ship to
    std::string data_dir;           // "./data/node1" — WAL lives here (each node has its own WAL)
};
```

**Why here and not in `main.cpp`?**
`main.cpp` is just a wiring point. If `NodeConfig` lives in its own header, both
`KVServiceImpl` and `LeaderReplicator` can include it without creating a circular dependency on `main`.
In Phase 7 (Raft) the config grows to include election timeouts, heartbeat intervals, and log
indexes — you want it isolated so that addition is a single-file change.

**How it gets populated in Phase 3:**
Command-line flags parsed in `main.cpp` fill a `NodeConfig` struct, which is then passed into
`KVServiceImpl` and `LeaderReplicator`. No global variables.

---

### `src/replication/replication_log.h/.cpp` — "the shared view of the log"

**Intuition:** Both the leader and followers need to answer the same question:
_"What is the highest sequence number I have committed?"_

`ReplicationLog` wraps `LogManager` and exposes two extra things the raw WAL doesn't:

```cpp
class ReplicationLog {
public:
    explicit ReplicationLog(const std::string& data_dir);

    // Write + fsync — same as LogManager::append but returns the seq that was assigned.
    // The caller (LeaderReplicator) needs the seq to fill AppendEntriesRequest.leader_seq.
    uint64_t append(Optype op, const std::string& key, const std::string& value = "");

    // Read all entries from a given seq forward.
    // Used by leader to answer FetchLog requests from lagging followers.
    std::vector<LogEntry> read_from(uint64_t from_seq);

    // Current highest committed sequence number.
    // Follower sends this in AppendEntriesResponse so leader knows how far behind it is.
    uint64_t last_seq() const;

    // Full recovery replay — same as LogManager::recover().
    std::vector<LogEntry> recover();

private:
    std::unique_ptr<LogManager> log_;
    uint64_t last_seq_ = 0;  // updated on every successful append or recover
};
```

**Why not use `LogManager` directly?**
`LogManager` only knows about append and recover. It does not track `last_seq_`.
`last_seq_` is a replication concept, not a storage concept — it belongs here.
`LogManager` remains unaware that replication exists. You can unit-test `LogManager`
without spinning up any network, and unit-test `ReplicationLog` without gRPC.

---

### `src/replication/leader_replicator.h/.cpp` — "ship the entry to every follower"

**Intuition:** The leader has just written to its own WAL. Now it must cause every follower
to write the same entry to _their_ WAL, in the same order, before acknowledging success to the client.

```cpp
class LeaderReplicator {
public:
    // peers: list of follower addresses e.g. ["localhost:50052", "localhost:50053"]
    explicit LeaderReplicator(const std::vector<std::string>& peers);

    // Called by KVServiceImpl::Put/Delete after writing to local WAL.
    // Sends AppendEntries RPC to all followers.
    // In sync mode: blocks until all followers ack (or timeout).
    // Returns false if any follower fails — caller should return error to client.
    bool replicate(const LogEntry& entry);

private:
    // One gRPC stub per peer, created at construction and reused.
    // Creating a channel per request is expensive — stubs are long-lived.
    std::vector<std::unique_ptr<codb::ReplicationService::Stub>> stubs_;
};
```

**What `replicate()` does step by step:**

1. Serialize the `LogEntry` into bytes (reuse `LogEntry::serialize()` from Phase 2)
2. Build an `AppendEntriesRequest`: set `leader_seq`, `serialized_entry`, `leader_id`
3. Call `stub->AppendEntries(...)` on each follower (in sync mode: sequentially or in parallel threads)
4. Check every `AppendEntriesResponse.success` — if any is false, return false

**Why one stub per peer at construction time?**
gRPC channels are TCP connections with TLS handshake. Opening one per `replicate()` call
would add hundreds of milliseconds to every write. Long-lived stubs keep the connection warm.

**Phase 4 preview:** `replicate()` will become `replicate_quorum()` — return true as soon as
a majority (not all) followers ack. Today it waits for all.

---

### `src/replication/follower_service.h/.cpp` — "receive the entry and write it locally"

**Intuition:** This is the server-side mirror of `LeaderReplicator`. It implements the
`ReplicationService` gRPC service that the leader calls into.

```cpp
// Implements codb::ReplicationService::Service (generated from proto)
class FollowerService final : public codb::ReplicationService::Service {
public:
    // store: the follower's own WalKVStore — entries are applied here
    explicit FollowerService(std::shared_ptr<IKVStore> store,
                             std::shared_ptr<ReplicationLog> log);

    // Called by leader for each new entry
    grpc::Status AppendEntries(grpc::ServerContext*,
                               const codb::AppendEntriesRequest*,
                               codb::AppendEntriesResponse*) override;

    // Called by a reconnecting follower to stream catch-up entries
    grpc::Status FetchLog(grpc::ServerContext*,
                          const codb::FetchLogRequest*,
                          grpc::ServerWriter<codb::LogEntryProto>*) override;

private:
    std::shared_ptr<IKVStore>       store_;  // apply the entry here
    std::shared_ptr<ReplicationLog> log_;    // write to our own WAL first
};
```

**`AppendEntries` does exactly what the leader's `put()` does, but without calling the replicator:**

1. Deserialize the entry from `request.serialized_entry()`
2. Write to follower's WAL via `log_->append()`
3. Apply to follower's in-memory map via `store_->put()` or `store_->remove()`
4. Return `success=true` and `follower_seq=log_->last_seq()`

**Why does the follower also have a WAL?**
If the follower crashes and restarts, it replays its own WAL to rebuild state — exactly like
the leader. Without its own WAL, a follower restart would require a full re-sync from the leader.
With WAL, it only needs entries it hasn't seen yet.

**`FetchLog` — catch-up for lagging followers:**
The leader's side calls `ReplicationLog::read_from(from_seq)` and streams the results back.
The follower applies them in order. This is identical to PostgreSQL's streaming replication catch-up.

---

### `src/server/kv_service_impl.cpp` — the only file that changes from Phase 2

Currently `Put()` looks like this:

```cpp
grpc::Status KVServiceImpl::Put(...) {
    bool ok = store_->put(req->key(), req->value());
    resp->set_success(ok);
    return grpc::Status::OK;
}
```

After Phase 3 — if this node is the leader, it must also replicate:

```cpp
grpc::Status KVServiceImpl::Put(...) {
    // 1. Write to local WAL + memory (WalKVStore handles both)
    bool ok = store_->put(req->key(), req->value());
    if (!ok) { resp->set_success(false); return grpc::Status::OK; }

    // 2. Ship to followers (only if we are the leader)
    if (replicator_) {
        // replicator_ is nullptr on followers
        LogEntry entry{ .op=Optype::PUT, .key=req->key(), .value=req->value() };
        ok = replicator_->replicate(entry);
    }

    resp->set_success(ok);
    return grpc::Status::OK;
}
```

`replicator_` is injected via the constructor — `nullptr` on followers.
This means `KVServiceImpl` needs zero `if (is_leader)` checks scattered through it —
the presence or absence of a replicator pointer encodes the role.

**On a follower:** If a client mistakenly sends a `Put` directly, `KVServiceImpl` should
return an error because the follower has no replicator. The cleanest way: add a role check
in `Put()/Delete()` and return `grpc::Status(grpc::FAILED_PRECONDITION, "not the leader")`.

---

## Call flow for a single PUT on a 3-node cluster

```
Client PUT "city" = "mars"
    │
    ▼
KVServiceImpl::Put()          [on leader, port 50051]
    │
    ├─▶ store_->put("city","mars")     ← WalKVStore
    │       ├─▶ LogManager::append(PUT, "city", "mars")   ← fsync to leader WAL
    │       └─▶ store_["city"] = "mars"                   ← leader RAM
    │
    └─▶ replicator_->replicate(entry)  ← LeaderReplicator
            ├─▶ AppendEntries RPC ──────────────────────────▶ FollowerService [node2]
            │                                                      ├─▶ log_->append()   ← fsync to node2 WAL
            │                                                      └─▶ store_->put()    ← node2 RAM
            │                          ◀── success, follower_seq ──┘
            │
            └─▶ AppendEntries RPC ──────────────────────────▶ FollowerService [node3]
                                                                   ├─▶ log_->append()   ← fsync to node3 WAL
                                                                   └─▶ store_->put()    ← node3 RAM
                                       ◀── success, follower_seq ──┘
    │
    ▼
resp.set_success(true)
    │
    ▼
Client ◀── OK
```

All three nodes have the entry on disk before the client sees "OK". That is synchronous replication.

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

## Implementation Status

### Implemented ✅

| File                                       | Status | Notes                                                                                                         |
| ------------------------------------------ | ------ | ------------------------------------------------------------------------------------------------------------- |
| `proto/kvstore.proto`                      | ✅     | `AppendEntries` (unary) + `RequestEntries` (server-streaming)                                                 |
| `src/server/node_config.h`                 | ✅     | `NodeRole` enum, `peer_addresses` map, `data_dir`                                                             |
| `src/server/kv_service_impl.h/.cpp`        | ✅     | Optional `LeaderReplicator*`; followers reject writes with `FAILED_PRECONDITION`                              |
| `src/server/main.cpp`                      | ✅     | Flag parsing (`--node-id`, `--port`, `--role`, `--peer`, `--data-dir`), wires all services                    |
| `src/replication/replication_log.h/.cpp`   | ✅     | Wraps `LogManager`; tracks `last_seq_`; `append()`, `recover()`, `read_from()`                                |
| `src/replication/leader_replicator.h/.cpp` | ✅     | Creates one stub per peer; synchronous `replicate()` with 500 ms deadline; `sync_follower()` via streaming    |
| `src/replication/follower_service.h/.cpp`  | ✅     | `AppendEntries`: idempotency check → WAL-first → apply to store. `RequestEntries`: batched streaming catch-up |

### Completion Criteria

- [x] 3-node cluster starts with one leader and two followers
- [ ] Write to leader replicates to both followers within 50 ms
- [ ] Follower that reconnects after downtime catches up via `RequestEntries` stream
- [x] Write to a follower directly returns `FAILED_PRECONDITION` (not the leader)
- [x] Leader crash preserves all replicated data on followers

### How to run a 3-node cluster

```bash
# Build
cd codb/build && cmake .. && make -j$(nproc)

# Terminal 1 — Leader
./codb_server --node-id node1 --port 50051 --role leader \
  --peer node2=localhost:50052 --peer node3=localhost:50053 \
  --data-dir ./data/node1

# Terminal 2 — Follower 1
./codb_server --node-id node2 --port 50052 --role follower \
  --peer node1=localhost:50051 \
  --data-dir ./data/node2

# Terminal 3 — Follower 2
./codb_server --node-id node3 --port 50053 --role follower \
  --peer node1=localhost:50051 \
  --data-dir ./data/node3

# Terminal 4 — Client
./codb_client localhost:50051 put city mars
./codb_client localhost:50052 get city   # returns "mars" after replication
./codb_client localhost:50053 get city   # returns "mars"
```

---

## Next → [Phase 4: Quorum Replication](05_phase4_quorum.md)
