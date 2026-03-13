# Phase 8 — Conflict Resolution

### Vector Clocks & CRDTs: Handling Concurrent Writes

---

## The Problem

In Phase 4 (quorum replication), we used a simple version counter.  
This works when writes are sequential. But what about **concurrent writes**?

```
Client A reads key=X (version=5, value="red")
Client B reads key=X (version=5, value="red")

Client A writes X="blue"  → goes to [Node1, Node2]
Client B writes X="green" → goes to [Node2, Node3]

Node2 receives both writes.
Which one wins?
```

The version counter says both are version 6. It cannot tell them apart.

---

## Approach 1: Last-Write-Wins (LWW)

The simplest strategy: attach a timestamp, highest timestamp wins.

```
Client A writes X="blue"  at timestamp 1000
Client B writes X="green" at timestamp 1001
→ "green" wins
```

**Problem:** Clocks are not synchronized (Law 3 from Phase 0).  
Client A's write may have a higher physical timestamp even if it happened "after" Client B.

LWW causes **silent data loss** — a valid write is discarded without any error.

**Used in:** Cassandra (as default, with explicit acknowledgment of this tradeoff)  
**Not used in:** Systems that require correct concurrent update semantics

---

## Approach 2: Vector Clocks

Vector clocks track **causal relationships** between writes.

### Structure

A vector clock is a map from node ID to counter:

```
VC = { node1: 3, node2: 1, node3: 2 }
```

It means: "This write has seen 3 events from node1, 1 from node2, 2 from node3."

### Update Rule

When a node performs a write:

```
Increment its own counter:
VC_new = { node1: 4, node2: 1, node3: 2 }
```

### Merge Rule (on receive)

```
VC_merged[i] = max(VC_local[i], VC_received[i])
```

### Comparison Rule

```
VC_a < VC_b (a "happened before" b):
   All VC_a[i] <= VC_b[i]
   At least one VC_a[i] < VC_b[i]

VC_a || VC_b (concurrent — no causal relationship):
   VC_a[i] > VC_b[i]  for some i
   VC_a[j] < VC_b[j]  for some j
```

### Concurrent = Conflict

```
Client A writes X="blue":   VC = { A: 1, B: 0 }
Client B writes X="green":  VC = { A: 0, B: 1 }

Compare: A's VC is not < or > B's VC
→ These are CONCURRENT writes → CONFLICT
```

Instead of discarding one, CoDB stores both versions and lets the application decide.

---

## How DynamoDB Handles Conflicts

```
GET key → returns list of conflicting versions (siblings)
Client application sees siblings and resolves them
PUT key with merged value → resolved version
```

This means **the application must implement conflict resolution logic**.  
For a shopping cart: "union of both carts"  
For a counter: "sum of both values"  
For a string: "show both versions to user for manual resolution"

---

## Approach 3: CRDTs (Conflict-Free Replicated Data Types)

What if we design data structures where **all concurrent operations always merge correctly?**

These are CRDTs. They trade expressiveness for automatic conflict resolution.

### G-Counter (Grow-Only Counter)

```
Each node maintains its own counter slot:
  { node1: 5, node2: 3, node3: 7 }
  Total = 5 + 3 + 7 = 15

Increment: only increment your own slot
  node1 increments: { node1: 6, node2: 3, node3: 7 }

Merge: take max of each slot
  Merge { node1: 6, ... } and { node1: 5, ... }
  Result: { node1: 6, ... }

This is always correct — there are no conflicts to resolve.
```

### PN-Counter (Positive-Negative Counter)

```
Increment → add to P counter
Decrement → add to N counter
Value     = sum(P) - sum(N)
```

### OR-Set (Observed-Remove Set)

```
Add element: tag it with a unique ID { element: "x", tag: "uuid1" }
Remove element: record which tags are removed

Merge: union of add-sets minus union of remove-sets
```

**Key property:** Add and Remove can happen concurrently — no conflict.

---

## Proto Changes

```proto
message VectorClock {
  map<string, uint64> clocks = 1;  // node_id → counter
}

message VersionedValue {
  string       value    = 1;
  VectorClock  vc       = 2;
  string       node_id  = 3;
}

message GetResponse {
  bool                   found    = 1;
  repeated VersionedValue siblings = 2;  // may be > 1 if conflict
}

message PutRequest {
  string      key      = 1;
  string      value    = 2;
  VectorClock context  = 3;  // client sends back the VC it read
}
```

---

## Write Path with Vector Clocks

```
1. Client reads key X → receives VersionedValue { value="red", vc={A:1} }

2. Client wants to update: sends PUT { key=X, value="blue", context={A:1} }

3. Coordinator receives write:
   - Checks if context VC is causally consistent with stored VC
   - If stored VC is higher: detect concurrent write → create sibling
   - If stored VC <= context VC: safe overwrite

4. New VC = merge(context, {coordinator: current_counter + 1})
```

---

## Directory Structure Changes

```
codb/
└── src/
    ├── versioning/
    │   ├── vector_clock.h          ← NEW: VC data structure + operations
    │   ├── vector_clock.cpp        ← NEW
    │   ├── sibling_manager.h       ← NEW: stores multiple concurrent versions
    │   └── sibling_manager.cpp     ← NEW
    │
    └── crdt/
        ├── g_counter.h             ← NEW: grow-only counter CRDT
        ├── pn_counter.h            ← NEW: increment/decrement counter CRDT
        ├── or_set.h                ← NEW: observed-remove set CRDT
        └── lww_register.h          ← NEW: LWW register (for comparison)
```

---

## Vector Clock Operations

```cpp
class VectorClock {
public:
    void increment(const std::string& node_id);
    void merge(const VectorClock& other);      // take max of each component

    // Returns: BEFORE, AFTER, CONCURRENT, EQUAL
    CausalOrder compare(const VectorClock& other) const;

    uint64_t get(const std::string& node_id) const;

private:
    std::unordered_map<std::string, uint64_t> clocks_;
};

enum class CausalOrder {
    BEFORE,     // this VC happened before other
    AFTER,      // this VC happened after other
    CONCURRENT, // neither happened before the other → conflict
    EQUAL,
};
```

---

## Testing Plan

### Vector Clock Causality Test

```cpp
VectorClock vc_a, vc_b;
vc_a.increment("node1");            // A = {node1:1}
vc_b = vc_a;
vc_b.increment("node2");            // B = {node1:1, node2:1}

assert(vc_a.compare(vc_b) == CausalOrder::BEFORE);  // A happened before B
assert(vc_b.compare(vc_a) == CausalOrder::AFTER);

VectorClock vc_c;
vc_c.increment("node3");            // C = {node3:1}

assert(vc_a.compare(vc_c) == CausalOrder::CONCURRENT);  // A || C
```

### Conflict Detection Test

```bash
# Read key=X → { value="red", vc={n1:2} }
# Without updating VC, write X="blue" from two clients simultaneously
# Both writes arrive at coordinator
# Verify: both versions stored as siblings
# Read key=X → returns 2 siblings
```

### G-Counter Test

```cpp
GCounter c;
c.increment("node1");
c.increment("node1");
c.increment("node2");

GCounter c2;
c2.increment("node2");
c2.increment("node3");

c.merge(c2);
assert(c.value() == 4);  // 2 + max(1,1) + 1 = 4
// No conflicts — always deterministic
```

---

## Conflict Resolution Strategies Summary

| Strategy       | Data Loss?              | App Complexity      | Use Case                        |
| -------------- | ----------------------- | ------------------- | ------------------------------- |
| LWW            | Yes                     | None                | Metrics, logs (loss acceptable) |
| Vector Clocks  | No (stores siblings)    | High (app resolves) | Shopping carts, documents       |
| CRDTs          | No                      | Low (automatic)     | Counters, sets, flags           |
| Raft (Phase 7) | No (strong consistency) | None                | Metadata, config                |

---

## Key Concepts After This Phase

- Why physical timestamps are unsafe for conflict detection
- How vector clocks capture causality without synchronized clocks
- What "concurrent" means in terms of causal order
- Why CRDTs are conflict-free by design (not by luck)
- When to use LWW vs vector clocks vs CRDTs vs strong consistency

---

## Git Commits for This Phase

```
feat(vc): implement VectorClock with increment, merge, compare
feat(vc): add causal order comparison (BEFORE/AFTER/CONCURRENT)
feat(storage): add sibling storage for concurrent version tracking
feat(proto): add VectorClock message and context field to PutRequest
feat(crdt): implement G-Counter CRDT
feat(crdt): implement PN-Counter CRDT
feat(crdt): implement OR-Set CRDT
feat(client): add sibling display in CLI client GET response
test(vc): verify causality detection in concurrent write scenario
test(crdt): verify G-Counter merge is commutative and associative
```

---

## Completion Criteria

- [ ] Vector clock correctly identifies BEFORE, AFTER, CONCURRENT relationships
- [ ] Concurrent writes to the same key are stored as siblings (not silently discarded)
- [ ] Read returns all siblings when conflicts exist
- [ ] G-Counter, PN-Counter, and OR-Set merge correctly with no conflicts
- [ ] CRDTs produce same result regardless of merge order (commutativity)

---

## Next → [Phase 9: LSM Storage Engine](10_phase9_lsm.md)
