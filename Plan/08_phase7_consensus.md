# Phase 7 — Raft Consensus

### Strong Agreement: Coordinating Cluster Metadata

---

## The Problem

Phases 4–6 give CoDB availability and partition tolerance.  
But some data cannot be eventually consistent:

```
Which node is the leader for a shard?
What is the current ring configuration?
Which nodes are voting members of the cluster?
```

If two nodes disagree on the ring configuration, they route requests differently.  
This causes **data corruption** — not just staleness.

**For metadata, you need a single agreed-upon truth.**

---

## The Pattern: Raft Consensus

Raft provides **replicated state machine** semantics:

> Given the same sequence of commands, all replicas will reach the same state.

It guarantees:

- Only one leader at a time
- All committed entries are durable (on a majority of nodes)
- All nodes apply entries in the same order

**Used for metadata coordination in:** etcd, CockroachDB, Consul, TiKV

---

## What Raft Does NOT Do

Raft is **expensive**. Each committed entry requires:

- Leader persists entry locally
- Leader sends to followers
- Followers persist + ack
- Leader commits + notifies followers

= **2 round trips** minimum

> Do NOT use Raft for every key-value operation.  
> Use Raft for **cluster metadata only**:
>
> - Ring configuration changes
> - Node membership changes
> - Shard ownership assignments

Use Phase 4 (quorum) for actual data reads and writes.

---

## Raft Algorithm Overview

### Three Node Roles

```
Follower  → passive, receives entries, votes in elections
Candidate → running for leader, solicits votes
Leader    → accepts client commands, replicates to followers
```

### State Transitions

```
         timeout (no leader heartbeat)          wins majority vote
Follower ─────────────────────────────▶ Candidate ────────────────▶ Leader
   ▲                                        │                           │
   │◀───────────────────────────────────────┘                          │
   │          discovers leader or higher term                          │
   │◀──────────────────────────────────────────────────────────────────┘
             discovers higher term
```

---

## Leader Election

### Terms

Raft uses **terms** — monotonically increasing integers — as logical clocks.

```
Term 1: Node A is leader
Term 2: Node A crashes, election → Node B wins
Term 3: Node B crashes, election → Node C wins
```

A node always rejects messages from lower terms. Higher term always wins.

### Election Process

```
1. Follower has not heard from leader for election_timeout (150–300ms, randomized)
2. Follower increments its term, transitions to Candidate
3. Candidate votes for itself, sends RequestVote to all peers:
   "I am in term 5, my log is up to index 42. Will you vote for me?"
4. Peer grants vote if:
   - Has not voted in this term yet
   - Candidate's log is at least as up-to-date as peer's log
5. Candidate receives majority → becomes Leader
   Candidate sees higher term → reverts to Follower
   Election timeout → starts new election with term+1
```

**Why randomized timeout?**  
Prevent all nodes starting elections simultaneously.  
Random range ensures one node usually starts first, wins before others begin.

---

## Log Replication

```
1. Client sends command to Leader: "Add node5 to ring at position 280"

2. Leader appends to its local log:
   { term: 3, index: 47, command: "ring_add_node5_280" }

3. Leader sends AppendEntries RPC to all followers:
   { term: 3, prevIndex: 46, prevTerm: 3, entries: [entry_47] }

4. Followers append to their logs, respond OK

5. Leader counts acks: majority received (e.g., 3 of 5 nodes)

6. Leader commits entry 47 (marks as committed)

7. Leader applies command to state machine (updates ring config)

8. Next AppendEntries informs followers of commit index

9. Followers apply command to their state machines
```

**Safety guarantee:** An entry is committed only after a majority has it.  
If leader crashes, the next leader will have all committed entries (guaranteed by vote log-completeness check).

---

## Raft Proto Messages

```proto
service RaftService {
  rpc RequestVote   (RequestVoteRequest)   returns (RequestVoteResponse);
  rpc AppendEntries (AppendEntriesRequest) returns (AppendEntriesResponse);
}

message RequestVoteRequest {
  uint64 term           = 1;
  string candidate_id   = 2;
  uint64 last_log_index = 3;
  uint64 last_log_term  = 4;
}

message RequestVoteResponse {
  uint64 term         = 1;
  bool   vote_granted = 2;
}

message AppendEntriesRequest {
  uint64 term          = 1;
  string leader_id     = 2;
  uint64 prev_log_idx  = 3;
  uint64 prev_log_term = 4;
  repeated LogEntry entries = 5;
  uint64 leader_commit = 6;
}

message AppendEntriesResponse {
  uint64 term    = 1;
  bool   success = 2;
  uint64 match_index = 3;  // highest index follower has
}

message LogEntry {
  uint64 term    = 1;
  uint64 index   = 2;
  bytes  command = 3;  // serialized RingConfig change or membership change
}
```

---

## Directory Structure Changes

```
codb/
└── src/
    ├── raft/
    │   ├── raft_node.h             ← NEW: core Raft state machine
    │   ├── raft_node.cpp           ← NEW
    │   ├── raft_log.h              ← NEW: persistent Raft log
    │   ├── raft_log.cpp            ← NEW
    │   ├── raft_service.h          ← NEW: gRPC service for Raft RPCs
    │   ├── raft_service.cpp        ← NEW
    │   └── state_machine.h         ← NEW: apply committed commands
    │       state_machine.cpp       ← NEW (ring config + membership)
    │
    └── metadata/
        ├── cluster_config.h        ← NEW: current ring + membership state
        └── cluster_config.cpp      ← NEW
```

---

## State Machine: What Raft Manages

The Raft state machine in CoDB manages:

```cpp
class ClusterStateMachine {
public:
    // Called when Raft commits a new entry
    void apply(const RaftCommand& cmd);

    // Current state (read by routing layer)
    HashRing     current_ring() const;
    MembershipTable members() const;

private:
    HashRing         ring_;
    MembershipTable  members_;
    mutable std::mutex mu_;
};

enum class RaftCommandType {
    ADD_NODE,
    REMOVE_NODE,
    UPDATE_VNODE_COUNT,
};
```

---

## How Raft Integrates with CoDB

```
Client wants to add Node5 to cluster
    ↓
Client sends request to Raft Leader
    ↓
RaftNode::propose("add_node5_at_280")
    ↓
Raft replicates to majority
    ↓
Raft commits → ClusterStateMachine::apply(ADD_NODE, node5, 280)
    ↓
HashRing updates → rebalancer migrates affected keys
    ↓
All Raft followers apply the same command → same ring state everywhere
```

---

## Leader Election Timing

```
Election timeout:   150ms – 300ms (randomized per node)
Heartbeat interval: 50ms (must be less than election timeout)
```

With 3 nodes, leader election typically completes in < 500ms.

---

## Log Compaction (Snapshots)

The Raft log grows unbounded. Solution: periodic snapshots.

```
State machine takes a snapshot: { ring: [...], members: [...] }
Raft discards all log entries before the snapshot index
New nodes bootstrap via snapshot + any entries after it
```

---

## Testing Plan

### Leader Election Test

```bash
# Start 3-node Raft cluster
# Wait for leader election to complete
# Verify: exactly 1 leader, 2 followers
# Kill leader
# Verify: new leader elected within 500ms
# Verify: old leader rejoins as follower
```

### Log Replication Test

```cpp
// Propose 100 ring config changes via leader
// Verify all 3 nodes apply all 100 in same order
// Compare final ring state across all 3 nodes → must be identical
```

### Split Vote Test

```bash
# 5-node cluster, partition into [2] and [3] groups
# Partition of 2 cannot elect a leader (no majority)
# Partition of 3 elects a leader (has majority)
# Verify: only 1 active leader across entire cluster
```

### Follower Catch-Up Test

```bash
# Start 3-node cluster
# Disconnect node3 for 30 seconds
# Propose 50 commands via leader
# Reconnect node3
# Verify: node3 catches up to entry 50 without full restart
```

---

## Key Concepts After This Phase

- Why randomized election timeouts prevent split votes
- What "committed" means in Raft (majority persistence)
- Why Raft log and WAL are different (Raft log is for commands, WAL is for storage)
- Why you should NOT use Raft for every key-value operation
- How log compaction (snapshots) bounds Raft log size

---

## Raft vs Paxos

| Property          | Raft                        | Paxos                    |
| ----------------- | --------------------------- | ------------------------ |
| Understandability | High (designed for clarity) | Low (very academic)      |
| Leader election   | Explicit, built-in          | Implicit, complex        |
| Log sequencing    | Strict, index-based         | Out-of-order possible    |
| Implementations   | etcd, CockroachDB, Consul   | Google Chubby, Zookeeper |
| Paper to read     | Ongaro & Ousterhout (2014)  | Lamport (1998/2001)      |

---

## Git Commits for This Phase

```
feat(raft): add RaftLog with persistent term/index/command storage
feat(raft): implement leader election with RequestVote RPC
feat(raft): implement log replication with AppendEntries RPC
feat(raft): add heartbeat timer and election timeout with jitter
feat(raft): implement commit index tracking and state machine application
feat(raft): add ClusterStateMachine for ring + membership commands
feat(raft): implement log compaction via snapshots
feat(metadata): add ClusterConfig backed by Raft state machine
test(raft): verify leader election within 500ms after leader crash
test(raft): verify 3-node log replication consistency
```

---

## Completion Criteria

- [ ] 3-node Raft cluster elects exactly one leader
- [ ] New leader elected within 500ms of leader crash
- [ ] All nodes apply the same commands in the same order
- [ ] Disconnected follower catches up on reconnect
- [ ] Ring config changes are consistent across all nodes via Raft
- [ ] Old leader reverts to follower when it rejoins (no split-brain)

---

## Next → [Phase 8: Conflict Resolution](09_phase8_conflicts.md)
