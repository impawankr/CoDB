# Phase 6 — Failure Detection

### Knowing When a Node is Dead

---

## The Problem

Phases 3–5 all assume the system **knows** which nodes are alive.  
But how does a node know if another node is dead vs. slow vs. temporarily partitioned?

```
Node A sends a message to Node B.
No response.

Is Node B:
  (a) Dead?
  (b) Slow (high load, GC pause)?
  (c) Partitioned (network issue)?
  (d) Slow to respond (backpressure)?
```

**There is no way to distinguish these with certainty.**  
This is the **Failure Detection Problem** in distributed systems.

---

## Two Approaches

### Approach 1: Heartbeats (Centralized)

Each node periodically sends a "I am alive" signal to a **monitor** node.

```
Node A ──── heartbeat (every 1s) ────▶ Monitor
Node B ──── heartbeat (every 1s) ────▶ Monitor
Node C ──── heartbeat (every 1s) ────▶ Monitor

If Monitor receives no heartbeat from Node B for 5s:
   Monitor marks Node B as SUSPECTED_DEAD
```

**Problem:** The monitor is a **single point of failure**.  
**Problem:** Does not scale to hundreds of nodes.

---

### Approach 2: Gossip Protocol (Distributed)

No central monitor. Every node talks to random peers.

```
Each node maintains:
  - List of all known nodes
  - Per-node: { last_heartbeat_seq, last_seen_timestamp }

Every 1 second, each node:
  1. Increments its own heartbeat counter
  2. Picks 2-3 random peers
  3. Sends its full membership list
  4. Merges the received membership list (take max seq per node)
```

If a node's heartbeat counter hasn't increased in T seconds → **suspected dead**.

**Used in:** Apache Cassandra, Consul, Riak

---

## Gossip Protocol Design

### Membership Entry

```cpp
struct MemberEntry {
    std::string   node_id;
    std::string   address;
    uint64_t      heartbeat_seq;    // increments every gossip round
    uint64_t      last_updated_ms;  // local timestamp when we last saw an update
    NodeStatus    status;           // ALIVE, SUSPECTED, DEAD, LEFT
};
```

### Gossip Message

```proto
message GossipMessage {
  string sender_id = 1;
  repeated MemberEntry members = 2;
}

message MemberEntry {
  string node_id       = 1;
  string address       = 2;
  uint64 heartbeat_seq = 3;
  NodeStatus status    = 4;
}

enum NodeStatus {
  ALIVE     = 0;
  SUSPECTED = 1;
  DEAD      = 2;
  LEFT      = 3;
}
```

### Merge Rule

When receiving a gossip message, for each entry:

```
if received.heartbeat_seq > local.heartbeat_seq:
    update local entry with received
    reset last_updated_ms = now
```

If `last_updated_ms` is more than `SUSPECT_TIMEOUT` ago → mark SUSPECTED.  
If `last_updated_ms` is more than `DEAD_TIMEOUT` ago → mark DEAD.

---

## Phi Accrual Failure Detector

Simple timeout-based detection has a binary state: **alive or dead**.

**Problem:** Network jitter means a node might miss one heartbeat without being dead.  
Declaring it dead immediately causes unnecessary rebalancing.

**Better approach:** Compute a **suspicion score** (phi, φ) instead of a binary state.

$$\phi(t) = -\log_{10}(P_{later}(t - t_{last}))$$

Where $P_{later}$ is the probability that the next heartbeat arrives later than time $t$.

- $\phi < 1$: Node is healthy
- $\phi > 8$: Node is almost certainly dead (99.999999% probability)
- φ threshold is **configurable** based on your tolerance for false positives

**Used in:** Apache Cassandra (default threshold φ = 8)

For CoDB Phase 6, implement the simpler **timeout-based** detector first.  
Add phi accrual as an optional enhancement.

---

## Directory Structure Changes

```
codb/
└── src/
    ├── gossip/
    │   ├── membership.h            ← NEW: MemberEntry, NodeStatus, membership table
    │   ├── membership.cpp          ← NEW
    │   ├── gossip_service.h        ← NEW: gRPC service for receiving gossip
    │   ├── gossip_service.cpp      ← NEW
    │   ├── gossip_manager.h        ← NEW: periodic gossip sender + failure detector
    │   └── gossip_manager.cpp      ← NEW
    │
    └── failure/
        ├── failure_detector.h      ← NEW: interface for failure detection
        ├── timeout_detector.cpp    ← NEW: simple timeout-based implementation
        └── phi_detector.cpp        ← NEW (optional): phi accrual implementation
```

---

## Gossip Integration with Routing

Once failure detection works, connect it to the routing layer:

```
RequestRouter asks: "is Node B alive before sending request?"
GossipManager answers: "Node B is SUSPECTED — use Node C instead"
```

```cpp
class RequestRouter {
public:
    PutResponse route_put(const PutRequest& req) {
        auto nodes = ring_->get_nodes(req.key(), N);

        // Filter out suspected/dead nodes
        auto live_nodes = filter_alive(nodes);

        // Proceed with quorum on live nodes
        return coordinator_->put(req, live_nodes);
    }

private:
    std::vector<std::string> filter_alive(const std::vector<std::string>& nodes);
    std::shared_ptr<GossipManager> gossip_;
};
```

---

## Testing Plan

### Basic Gossip Convergence Test

```bash
# Start 5-node cluster
# Each node starts knowing only 1 peer (bootstrap)
# After 5 gossip rounds (5 seconds), ALL nodes should know about ALL nodes
# Measure: time to full convergence
```

### Failure Detection Test

```bash
# Start 3-node cluster, let gossip stabilize
# Kill node2 (SIGKILL — no graceful shutdown)
# Measure time until node1 and node3 mark node2 as SUSPECTED
# Measure time until they mark it as DEAD
# Verify: requests that would have gone to node2 now route to replicas
```

### False Positive Test

```bash
# Start 3-node cluster
# Pause node2 for 2 seconds (simulate GC pause): kill -STOP $PID2
# Verify: node2 is SUSPECTED but not immediately DEAD
# Resume node2: kill -CONT $PID2
# Verify: node2 recovers to ALIVE status after heartbeats resume
```

### Gossip Scale Test

```bash
# Start 20-node cluster
# Kill 5 nodes simultaneously
# Measure time for remaining 15 nodes to converge on DEAD status
```

---

## Convergence Time Mathematics

For gossip to propagate to all N nodes:

$$\text{rounds to converge} \approx \log_2(N)$$

With 3 peers per round and 1-second interval:

- 8 nodes → ~3 rounds = ~3 seconds
- 64 nodes → ~6 rounds = ~6 seconds
- 1024 nodes → ~10 rounds = ~10 seconds

This is **logarithmic scalability** — why gossip is preferred over central monitors.

---

## Graceful vs Crash Leave

| Scenario                                     | Behavior                                      |
| -------------------------------------------- | --------------------------------------------- |
| Node sends `LEFT` status (graceful shutdown) | Peers mark it LEFT immediately, stop routing  |
| Node crashes (no message)                    | Peers wait SUSPECT_TIMEOUT, then DEAD_TIMEOUT |

Always implement graceful shutdown with a `LEFT` gossip broadcast.

---

## Key Concepts After This Phase

- Why failure detection is fundamentally uncertain (cannot distinguish slow from dead)
- How gossip achieves O(log N) convergence time
- Why phi accrual is better than binary timeout for noisy networks
- How failure detection integrates with the routing layer
- The difference between SUSPECTED and DEAD states

---

## Tuning Parameters

| Parameter         | Default | Effect of Increase                        |
| ----------------- | ------- | ----------------------------------------- |
| `gossip_interval` | 1s      | Slower convergence, less bandwidth        |
| `suspect_timeout` | 5s      | Fewer false positives, slower detection   |
| `dead_timeout`    | 30s     | More time for recovery before rebalancing |
| `fanout`          | 3 peers | Faster convergence, more bandwidth        |

---

## Git Commits for This Phase

```
feat(gossip): add MemberEntry and membership table data structures
feat(gossip): implement gossip merge logic with max-seq-wins
feat(gossip): add GossipManager with periodic send to random peers
feat(gossip): implement timeout-based failure detector (ALIVE/SUSPECTED/DEAD)
feat(gossip): add gRPC GossipService for receiving gossip messages
feat(router): integrate failure detector into RequestRouter
feat(gossip): add graceful shutdown with LEFT status broadcast
test(gossip): verify convergence in 5-node cluster within 5 gossip rounds
test(failure): verify dead node detection within configured timeout
```

---

## Completion Criteria

- [ ] 5-node cluster reaches full membership knowledge within 5 gossip rounds
- [ ] Dead node (SIGKILL) detected within `dead_timeout` by all remaining nodes
- [ ] Paused node (GC simulation) moves to SUSPECTED but recovers to ALIVE
- [ ] RequestRouter stops routing to DEAD/SUSPECTED nodes
- [ ] Graceful shutdown broadcasts LEFT status and is reflected in all peers immediately

---

## Next → [Phase 7: Raft Consensus](08_phase7_consensus.md)
