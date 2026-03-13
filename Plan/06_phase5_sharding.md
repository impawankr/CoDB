# Phase 5 — Consistent Hashing & Sharding

### Horizontal Scaling: Beyond a Single Machine's Capacity

---

## The Problem

Even with 3 replicas, all nodes store **all the data**.

With 1TB of data and 3 nodes:

- Each node stores 1TB
- Adding more nodes does NOT increase capacity

This is **replication** — not **scaling**.

---

## The Pattern: Partitioning (Sharding)

Divide the key space across nodes so each node owns a subset:

```
Node A: keys 0x0000 - 0x3FFF  (25% of keyspace)
Node B: keys 0x4000 - 0x7FFF  (25% of keyspace)
Node C: keys 0x8000 - 0xBFFF  (25% of keyspace)
Node D: keys 0xC000 - 0xFFFF  (25% of keyspace)
```

Now with 1TB of data and 4 nodes:

- Each node stores ~250GB
- Adding more nodes → each stores less → capacity scales linearly

---

## Why Not Simple Hash Modulo?

```
node = hash(key) % N
```

**Problem:** When you add or remove a node, $N$ changes:

```
Before: hash("user:1") % 3 = 2  → Node C
After:  hash("user:1") % 4 = 1  → Node B
```

**Every key moves to a different node!**  
For 1TB of data, this means moving ~1TB on every scaling event.  
This is a total cluster disruption.

---

## The Pattern: Consistent Hashing

Map both **keys** and **nodes** onto the same circular ring (0 to $2^{32}$).

```
                   0
              ___/ | \___
          ___/     |     \___
      Node A (80)  |         Node D (300)
      /            |              \
     |             Ring           |
      \            |             /
      Node B (160) | Node C (240)
           \___    |    ___/
               \___|___/
```

**Rule:** A key is assigned to the **first node clockwise** from its hash position.

```
hash("user:1") = 100 → Node B (160) is first clockwise → owned by Node B
hash("user:2") = 200 → Node C (240) is first clockwise → owned by Node C
hash("user:3") = 350 → Node A (80) wraps around → owned by Node A
```

---

## Adding a Node (The Key Insight)

Add Node E at position 200:

```
Before: hash("user:2") = 200 → Node C (240)
After:  hash("user:2") = 200 → Node E (200) [Node E is now first clockwise]
```

Only keys between position 160 (Node B) and 200 (Node E) need to move.  
**This is O(K/N) data movement**, not O(K).

For 1TB across 10 nodes: adding 1 node moves only ~100GB (1/10), not 1TB.

---

## Virtual Nodes (vnodes)

**Problem with basic consistent hashing:**  
3 physical nodes land at random positions → very uneven load distribution.

**Solution:** Each physical node owns **multiple virtual positions** on the ring.

```
Physical Node A → virtual positions: 45, 170, 290
Physical Node B → virtual positions: 80, 210, 340
Physical Node C → virtual positions: 120, 250, 20
```

With 100+ virtual nodes per physical node, load distributes statistically evenly.

**Used in:** Apache Cassandra (default: 256 vnodes per node)

---

## Replication on the Ring

Quorum replication (Phase 4) + consistent hashing = full Dynamo-style partitioning.

Each key has N **consecutive** nodes on the ring as its replica set:

```
For key at hash position 100, with N=3:
  Primary   → Node B (160)   first clockwise
  Replica 1 → Node C (240)   second clockwise
  Replica 2 → Node D (300)   third clockwise
```

This means every node knows exactly who else holds each key — no lookup service needed.

---

## Directory Structure Changes

```
codb/
└── src/
    ├── partitioning/
    │   ├── hash_ring.h             ← NEW: consistent hash ring
    │   ├── hash_ring.cpp           ← NEW
    │   ├── partition_map.h         ← NEW: key → node mapping with vnodes
    │   ├── partition_map.cpp       ← NEW
    │   └── rebalancer.h            ← NEW: handles node join/leave
    │       rebalancer.cpp          ← NEW
    │
    └── router/
        ├── request_router.h        ← NEW: routes client requests to correct node
        └── request_router.cpp      ← NEW
```

---

## Core Implementation: Hash Ring

```cpp
class HashRing {
public:
    // vnodes_per_node: how many virtual positions per physical node
    explicit HashRing(int vnodes_per_node = 150);

    void add_node(const std::string& node_id);
    void remove_node(const std::string& node_id);

    // Returns the node responsible for this key
    std::string get_node(const std::string& key) const;

    // Returns N nodes for replication (primary + N-1 replicas)
    std::vector<std::string> get_nodes(const std::string& key, int n) const;

private:
    int vnodes_per_node_;
    // sorted map: ring_position → node_id
    std::map<uint32_t, std::string> ring_;

    uint32_t hash(const std::string& key) const;  // MurmurHash3
};
```

---

## Routing Layer

The router sits between the client and the storage nodes:

```
Client → RequestRouter → HashRing.get_nodes(key, N) → [Node1, Node2, Node3]
                       → forward request to correct nodes (using Phase 4 quorum)
```

```cpp
class RequestRouter {
public:
    RequestRouter(std::shared_ptr<HashRing> ring,
                  std::shared_ptr<QuorumCoordinator> coordinator);

    PutResponse  route_put(const PutRequest& req);
    GetResponse  route_get(const GetRequest& req);

private:
    std::shared_ptr<HashRing>          ring_;
    std::shared_ptr<QuorumCoordinator> coordinator_;
};
```

---

## Node Join Protocol

When a new node joins:

```
1. New node announces itself: "I am Node E at position [45, 170, 290]"

2. Cluster calculates: which keys does Node E now own?
   → Keys between Node D's last vnode and Node E's vnodes

3. Previous owners stream those keys to Node E

4. Once transfer complete, Node E marks itself ready

5. Ring is updated — new requests for those keys go to Node E
```

This is **live rebalancing** — the cluster never goes offline during scaling.

---

## Node Leave Protocol

```
1. Node B announces departure (or failure is detected)

2. Node B's keys are redistributed to the next node on the ring

3. If it was a planned departure: Node B streams its data first
   If it was a crash: replicas already exist (from N=3 replication)
                      → no data loss, just repair the replica count
```

---

## Hot Partition Problem

Even with consistent hashing, **keyspace skew** is possible:

```
Most accessed keys: user:1, user:2, user:3 → all hash near Node A
Node A gets 90% of traffic despite even key distribution
```

**Solutions:**

1. **Request routing with load awareness** — route away from hot nodes
2. **Key splitting** — split popular keys: `user:1_shard0`, `user:1_shard1`...
3. **Caching** — cache hot keys at the router layer
4. **Physical node capacity weighting** — give faster nodes more vnodes

---

## Testing Plan

### Hash Ring Unit Tests

```cpp
// Verify even distribution: 3 nodes, 1M keys
// Expect: each node gets ~333K keys (±5%)
HashRing ring(150);
ring.add_node("node1");
ring.add_node("node2");
ring.add_node("node3");
// Insert 1M random keys, count distribution
```

### Node Addition Test

```cpp
// Add 4th node to 3-node ring
// Expected: ~25% of keys migrate (not 75%)
// Verify: key assignments for sampled keys before/after
```

### Multi-Node Cluster Routing Test

```bash
# Start 4 nodes
./codb_node --id=n1 --port=50051
./codb_node --id=n2 --port=50052
./codb_node --id=n3 --port=50053
./codb_node --id=n4 --port=50054

# All requests go through router
./codb_router --nodes=n1:50051,n2:50052,n3:50053,n4:50054 --port=9000

# Client uses router
./codb_client localhost:9000 put key1 val1
./codb_client localhost:9000 get key1    # transparent routing
```

---

## Key Concepts After This Phase

- Why `hash(key) % N` is unsuitable for dynamic clusters
- How the ring structure limits data movement to $O(K/N)$ per node change
- Why virtual nodes are needed for load balancing
- How replication (Phase 4) and partitioning combine on the ring
- What "hot partition" means and how to detect it

---

## Real-World Comparison

| System        | Partitioning        | Virtual Nodes | Replication on Ring      |
| ------------- | ------------------- | ------------- | ------------------------ |
| Cassandra     | Consistent hashing  | Yes (256)     | N consecutive nodes      |
| DynamoDB      | Consistent hashing  | Yes           | N consecutive nodes      |
| Redis Cluster | Hash slots (16384)  | No (slots)    | Primary-replica per slot |
| MongoDB       | Range-based (zones) | No            | Replica sets per shard   |

---

## Git Commits for This Phase

```
feat(ring): implement consistent hash ring with MurmurHash3
feat(ring): add virtual node support with configurable count
feat(ring): implement get_nodes() for replication-aware routing
feat(partitioning): add PartitionMap with ring + node registry
feat(partitioning): implement node join with data transfer protocol
feat(partitioning): implement node leave with replica repair
feat(router): add RequestRouter that combines ring + quorum coordinator
test(ring): verify even key distribution across 3 nodes
test(ring): verify O(K/N) data movement on node addition
```

---

## Completion Criteria

- [ ] HashRing distributes 1M keys within 5% of even distribution
- [ ] Adding a 4th node moves ≈25% of keys (not all of them)
- [ ] `get_nodes(key, 3)` returns 3 distinct consecutive ring nodes
- [ ] RequestRouter transparently routes requests to correct nodes
- [ ] Node join and leave complete without cluster downtime

---

## Next → [Phase 6: Failure Detection](07_phase6_failure.md)
