# Phase 4 — Quorum Replication (Dynamo-Style)

### Leaderless Availability: Survive Any Node Failure

---

## The Problem with Leader–Follower

Phase 3 works, but has one critical limitation:

```
Leader is the bottleneck:
  - All writes go through it
  - All reads (for consistency) go through it
  - If it crashes → cluster is unavailable until failover completes
```

For systems that must always be writable (e-commerce, IoT, financial logging),  
a leader failure — even briefly — is unacceptable.

**Amazon Dynamo solved this in 2007 by removing the leader entirely.**

---

## The Pattern: Leaderless Quorum Replication

```
         Client
            │
       Coordinator           ← any node can be coordinator
      ╱     │     ╲
   Node1   Node2  Node3      ← all nodes are equal
   (W)     (W)    (skip)
```

There is no leader. Any node can accept a write.  
The client (or a coordinator node) **sends the write to multiple nodes in parallel**.

**The quorum condition:**

$$R + W > N$$

Where:

- $N$ = total replicas (usually 3)
- $W$ = nodes that must acknowledge a write
- $R$ = nodes that must respond to a read

This guarantees at least one node in any read set has seen the latest write.

---

## Quorum Math (Intuition)

With $N = 3$:

| $W$ | $R$ | Guarantee               | Tradeoff                    |
| --- | --- | ----------------------- | --------------------------- |
| 3   | 1   | Strong write durability | Writes require all nodes up |
| 2   | 2   | Balanced ($R+W=4 > 3$)  | Industry default            |
| 1   | 3   | Fast writes             | Reads require all nodes     |
| 1   | 1   | Fastest                 | No consistency guarantee    |

**Default in Cassandra and Dynamo:** $N=3$, $W=2$, $R=2$

```
Read quorum  [Node1, Node2] must overlap with
Write quorum [Node2, Node3]
→ Node2 is in both → has latest value
```

---

## Versioning: The New Problem

Without a leader to order writes, **concurrent writes to the same key can conflict**:

```
Client A writes: key=X, value="red"   → goes to Node1, Node2
Client B writes: key=X, value="blue"  → goes to Node2, Node3

Node2 now has two versions: "red" and "blue"
Which one is correct?
```

**Solution:** Attach a **version number** to every write.

For Phase 4, we use a simple monotonic counter.  
Phase 8 (Vector Clocks) will handle true concurrent conflict detection.

---

## Proto Changes

```proto
message VersionedValue {
  string   value   = 1;
  uint64   version = 2;   // logical version (monotonic counter)
  string   node_id = 3;   // which node last wrote this
}

message PutRequest {
  string key     = 1;
  string value   = 2;
  uint64 version = 3;   // client provides expected version (optimistic locking)
}

message GetResponse {
  bool           found = 1;
  VersionedValue vv    = 2;
}
```

---

## Directory Structure Changes

```
codb/
└── src/
    ├── quorum/
    │   ├── quorum_config.h         ← NEW: N, W, R values
    │   ├── coordinator.h           ← NEW: orchestrates quorum writes/reads
    │   ├── coordinator.cpp         ← NEW
    │   └── version_tracker.h       ← NEW: per-key version management
    │
    └── storage/
        └── versioned_kv_store.h    ← NEW: stores VersionedValue per key
```

---

## Write Path (Quorum)

```cpp
// Coordinator::put(key, value):
1. Generate new version = current_version + 1
2. Send PutRequest to all N nodes in parallel (async)
3. Wait for W acknowledgments
4. Return success to client

// If fewer than W acks arrive within timeout:
   Return error: "write quorum not reached"
```

---

## Read Path (Quorum)

```cpp
// Coordinator::get(key):
1. Send GetRequest to all N nodes in parallel
2. Wait for R responses
3. Among R responses, return the one with the HIGHEST version number
4. If lower-versioned nodes exist → trigger READ REPAIR

// Read Repair:
   Send the highest-version value back to the lagging nodes
   (background write, does not block the client)
```

**Read Repair** is how leaderless systems converge without a master.

---

## Sloppy Quorum + Hinted Handoff (Dynamo Pattern)

**Strict quorum:** Must write to the $W$ specific "home" nodes for that key.

**Problem:** What if 2 of 3 home nodes are down?

**Sloppy quorum:** Write to any $W$ available nodes (including "non-home" nodes).  
Tag the entry: "this belongs to Node2, please deliver when Node2 recovers."

When the original node comes back: the substitute node forwards the data.  
This is called **hinted handoff**.

```
Normal:     Write to [Node1, Node2, Node3]
Node2 down: Write to [Node1, Node3, Node4]  ← Node4 holds a "hint" for Node2
Node2 back: Node4 sends the hinted entry to Node2
```

**Why this matters:** Sloppy quorum ensures writes **never fail** due to node failures.  
The price: temporary inconsistency (mitigated by read repair + hinted handoff).

This is how **Amazon Dynamo and Cassandra achieve 99.999% write availability**.

---

## Anti-Entropy (Background Sync)

What if hinted handoff fails? What if nodes diverge silently?

**Anti-entropy** is a background process that compares data between nodes and repairs differences.

Implementation: **Merkle Trees**

```
Node1 builds a Merkle tree of its key-value pairs (hash of all data)
Node2 builds its own Merkle tree
Compare root hashes:
  - Same → in sync
  - Different → drill down to find which subtree differs
                → sync only the differing keys
```

This is far more efficient than comparing all keys.  
Used in Cassandra (`nodetool repair`) and Amazon Dynamo.

---

## Testing Plan

### Basic Quorum Test (N=3, W=2, R=2)

```bash
# Start 3 nodes (all equal, no leader)
./codb_node --id=n1 --port=50051 --peers=n2:50052,n3:50053
./codb_node --id=n2 --port=50052 --peers=n1:50051,n3:50053
./codb_node --id=n3 --port=50053 --peers=n1:50051,n2:50052

# Write via any node (it becomes coordinator)
./codb_client localhost:50051 put color blue

# Read from any node
./codb_client localhost:50052 get color    # blue
./codb_client localhost:50053 get color    # blue
```

### Quorum Under Failure

```bash
# Kill node 3 (1 out of 3 is down)
kill $NODE3_PID

# Write should still succeed (W=2, only 2 nodes needed)
./codb_client localhost:50051 put status "degraded-but-alive"

# Read should still succeed (R=2)
./codb_client localhost:50052 get status    # degraded-but-alive

# Kill node 2 as well (2 out of 3 are down)
kill $NODE2_PID

# Write SHOULD FAIL (cannot reach W=2)
./codb_client localhost:50051 put x y    # error: quorum not reached
```

### Read Repair Test

```bash
# Manually create a stale entry on node3 (lower version)
# Read via quorum → should return latest version
# Verify node3 was updated via read repair
```

---

## Comparison: Leader-Follower vs Quorum

| Property          | Leader–Follower       | Quorum (Dynamo-style)           |
| ----------------- | --------------------- | ------------------------------- |
| Write bottleneck  | Leader only           | Any node (coordinator)          |
| Read consistency  | Strong (from leader)  | Tunable via R and W             |
| Availability      | Down during failover  | Always writable (sloppy quorum) |
| Complexity        | Lower                 | Higher (conflict resolution)    |
| Conflict handling | Leader serializes all | Need versioning + read repair   |
| Examples          | PostgreSQL, MySQL     | Cassandra, Amazon Dynamo        |

---

## Key Concepts After This Phase

- Why $R + W > N$ guarantees at least one node has the latest write
- The difference between strict quorum and sloppy quorum
- How read repair converges eventual consistency
- How hinted handoff enables writes during node failures
- Why conflict resolution is unavoidable in leaderless systems

---

## Git Commits for This Phase

```
feat(proto): add VersionedValue and version field to Put/Get
feat(quorum): add QuorumConfig with N, W, R parameters
feat(quorum): implement Coordinator with parallel put to N nodes
feat(quorum): implement quorum read with highest-version selection
feat(quorum): add read repair on stale quorum responses
feat(quorum): implement sloppy quorum with hinted handoff
feat(storage): add VersionedKVStore with per-key version tracking
test(quorum): verify quorum write survives 1-of-3 node failure
test(quorum): verify read repair corrects stale replica
```

---

## Completion Criteria

- [ ] Write with W=2 succeeds even when 1 of 3 nodes is down
- [ ] Read with R=2 returns the latest version
- [ ] Read repair is triggered and fixes stale replicas in background
- [ ] Hinted handoff delivers writes to recovered nodes
- [ ] Write correctly fails when fewer than W nodes are available

---

## Next → [Phase 5: Consistent Hashing & Sharding](06_phase5_sharding.md)
