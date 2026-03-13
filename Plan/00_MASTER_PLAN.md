# CoDB — Distributed Database Learning Roadmap

### (C++ · gRPC · First-Principles · Build-to-Understand)

---

## What Is CoDB?

**CoDB** is a distributed key-value database we build from scratch — step by step —
to understand how real systems like Amazon DynamoDB, CockroachDB, and Apache Cassandra work internally.

**Language:** C++17  
**Communication:** gRPC  
**Build System:** CMake  
**Goal:** Engineer-level understanding, not just API-user-level familiarity

---

## The Learning Philosophy

> Don't learn patterns. Learn why patterns exist.

Every pattern in distributed systems is an answer to a failure scenario.
We always start with:

1. **What breaks?** (the failure)
2. **Why does it break?** (the physics / constraint)
3. **What is the minimal fix?** (the pattern)
4. **What does the fix cost?** (the tradeoff)
5. **When should you NOT use it?** (the boundary)
6. **How does a real system implement it?** (the reference)
7. **Build it yourself** (the muscle)

---

## The 3 Physical Laws You Cannot Escape

```
1. Networks are unreliable         → messages get lost, delayed, duplicated
2. Nodes crash                     → memory vanishes, processes die
3. Clocks are not synchronized     → you cannot trust wall time
```

Every design pattern in this roadmap exists because of one of these three constraints.

---

## Roadmap Overview

| Phase | Name                          | Key Pattern                       | Real System Reference    |
| ----- | ----------------------------- | --------------------------------- | ------------------------ |
| 0     | Mental Models                 | First-principles thinking         | —                        |
| 1     | Single Node KV Store          | RPC Service Layer                 | etcd, Redis              |
| 2     | Write-Ahead Log (WAL)         | Durability & Crash Recovery       | PostgreSQL, RocksDB      |
| 3     | Leader–Follower Replication   | Log Shipping                      | MySQL, PostgreSQL        |
| 4     | Quorum Replication            | Dynamo-style R+W>N                | Amazon Dynamo, Cassandra |
| 5     | Consistent Hashing & Sharding | Partitioning                      | Cassandra, DynamoDB      |
| 6     | Failure Detection             | Heartbeats + Gossip               | Cassandra, Consul        |
| 7     | Raft Consensus                | Leader Election + Log Replication | etcd, CockroachDB        |
| 8     | Conflict Resolution           | Vector Clocks + CRDTs             | Riak, DynamoDB           |
| 9     | LSM Storage Engine            | MemTable + SSTables + Compaction  | RocksDB, LevelDB         |
| 10    | Distributed Transactions      | Saga Pattern                      | CockroachDB, TiDB        |

---

## CoDB Architecture Evolution

Each phase adds one layer. By Phase 9, you have a real distributed database.

```
Phase 1:
  Client → gRPC → KVService → In-Memory Store

Phase 2:
  Client → gRPC → KVService → WAL → In-Memory Store

Phase 3:
  Client → gRPC → Leader KVService → WAL → Followers

Phase 4:
  Client → Coordinator → Quorum Nodes (N=3, W=2, R=2)

Phase 5:
  Client → Router → Hash Ring → Shard Nodes

Phase 6:
  Cluster Monitor ← Heartbeats ← All Nodes
  Gossip Protocol → Membership List

Phase 7:
  Raft Leader Election → Metadata Consensus → Shard Map

Phase 9 (Final):
         Client
            |
       Query Router
            |
   ─────────────────────
   |          |         |
 Node1      Node2     Node3
   |          |         |
 WAL        WAL       WAL
 MemTable   MemTable  MemTable
 SSTables   SSTables  SSTables
 Repl Log   Repl Log  Repl Log
```

---

## Git Commit Strategy

Every phase has **named, purposeful commits**. Your repo becomes readable as a textbook.

```
feat(phase1): add gRPC proto definition for KVStore
feat(phase1): implement in-memory KVStore with mutex
feat(phase1): implement gRPC service layer
feat(phase1): add CLI client for manual testing

feat(phase2): add WAL log entry serialization
feat(phase2): implement log writer and flusher
feat(phase2): add crash recovery via log replay

feat(phase3): add replication log abstraction
feat(phase3): implement leader-follower RPC
feat(phase3): add follower catch-up mechanism

... and so on
```

---

## Time Estimates (Honest)

| Phase | Estimated Time | Difficulty |
| ----- | -------------- | ---------- |
| 0     | 1 day          | Conceptual |
| 1     | 2–3 days       | ★★☆☆☆      |
| 2     | 3–4 days       | ★★★☆☆      |
| 3     | 5–7 days       | ★★★★☆      |
| 4     | 4–5 days       | ★★★★☆      |
| 5     | 4–5 days       | ★★★☆☆      |
| 6     | 3–4 days       | ★★★☆☆      |
| 7     | 7–10 days      | ★★★★★      |
| 8     | 5–7 days       | ★★★★☆      |
| 9     | 4–5 days       | ★★★★☆      |
| 10    | 5–7 days       | ★★★★★      |

---

## Final Skill Outcome

After completing this roadmap you will deeply understand:

- Why Amazon Dynamo chose leaderless replication
- Why Google Spanner uses atomic clocks
- Why CockroachDB is built on Raft
- Why RocksDB uses LSM trees instead of B-trees
- Why Cassandra uses gossip for membership
- How to design systems that survive partial failures

These are the skills of a **senior distributed systems engineer**.

---

## Plan Files Index

```
Plan/
├── 00_MASTER_PLAN.md           ← You are here
├── 01_mental_models.md         ← Phase 0: First-principles thinking
├── 02_phase1_kv_store.md       ← Phase 1: Single node gRPC KV store
├── 03_phase2_wal.md            ← Phase 2: Write-Ahead Logging
├── 04_phase3_replication.md    ← Phase 3: Leader-Follower Replication
├── 05_phase4_quorum.md         ← Phase 4: Quorum Replication
├── 06_phase5_sharding.md       ← Phase 5: Consistent Hashing + Sharding
├── 07_phase6_failure.md        ← Phase 6: Failure Detection
├── 08_phase7_consensus.md      ← Phase 7: Raft Consensus
├── 09_phase8_conflicts.md      ← Phase 8: Conflict Resolution
└── 10_phase9_lsm.md            ← Phase 9: LSM Storage Engine
```

---

## Project Folder

All code lives in:

```
codb/
```

See `codb/README.md` for build instructions and project structure.

---

## Start Here

→ Read [01_mental_models.md](01_mental_models.md) before writing a single line of code.
