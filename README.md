# CoDB — Distributed Key-Value Database

> A distributed database built from scratch for learning, following production-grade design patterns.

---

## What is CoDB?

CoDB is an incrementally-built distributed key-value store.  
Each phase adds exactly one distributed systems capability.

By the end, CoDB implements the same core patterns found in:

- **Amazon DynamoDB** — quorum replication, consistent hashing, vector clocks
- **Apache Cassandra** — gossip protocol, leveled compaction, leaderless replication
- **CockroachDB / etcd** — Raft consensus, strong metadata coordination

---

## Current Status

| Phase | Feature                     | Status         |
| ----- | --------------------------- | -------------- |
| 1     | Single Node KV (gRPC)       | ✅ Done        |
| 2     | Write-Ahead Log             | ✅ Done        |
| 3     | Leader–Follower Replication | ✅ Done        |
| 4     | Quorum Replication          | 🔲 Not Started |
| 5     | Consistent Hashing          | 🔲 Not Started |
| 6     | Failure Detection (Gossip)  | 🔲 Not Started |
| 7     | Raft Consensus              | 🔲 Not Started |
| 8     | Vector Clocks / CRDTs       | 🔲 Not Started |
| 9     | LSM Storage Engine          | 🔲 Not Started |

---

## Project Structure

```
codb/
│
├── CMakeLists.txt              ← Root build file
├── README.md                   ← This file
│
├── proto/
│   └── kvstore.proto           ← gRPC service definitions
│
├── src/
│   ├── server/                 ← gRPC server entry point
│   ├── client/                 ← CLI client tool
│   ├── storage/                ← Storage abstractions
│   │   └── wal/                ← Write-Ahead Log
│   │   └── lsm/                ← LSM Storage Engine (Phase 9)
│   ├── replication/            ← Leader-Follower replication (Phase 3)
│   ├── quorum/                 ← Quorum coordinator (Phase 4)
│   ├── partitioning/           ← Consistent hash ring (Phase 5)
│   ├── router/                 ← Request routing layer
│   ├── gossip/                 ← Membership and failure detection (Phase 6)
│   ├── raft/                   ← Raft consensus for metadata (Phase 7)
│   ├── versioning/             ← Vector clocks (Phase 8)
│   └── crdt/                   ← CRDT implementations (Phase 8)
│
├── tests/
│   ├── unit/                   ← Unit tests per component
│   └── integration/            ← Multi-node integration tests
│
├── bench/
│   └── write_bench.cpp         ← Write throughput benchmarks
│
├── data/                       ← Runtime data directory (gitignored)
│
└── scripts/
    ├── run_cluster.sh           ← Start a 3-node local cluster
    └── run_tests.sh             ← Run all tests
```

---

## Prerequisites

### macOS

```bash
brew install cmake grpc protobuf abseil googletest
```

### Ubuntu / Debian

```bash
apt install cmake libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev
```

---

## Build

```bash
cd codb
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

---

## Run (Phase 1)

```bash
# Terminal 1: Start server
./build/codb_server --port=50051

# Terminal 2: Use client
./build/codb_client localhost:50051 put hello world
./build/codb_client localhost:50051 get hello
./build/codb_client localhost:50051 delete hello
```

---

## Run 3-Node Cluster (Phase 3+)

```bash
./scripts/run_cluster.sh
# Starts node1:50051 (leader), node2:50052, node3:50053
```

---

## Run Tests

```bash
cd build && ctest --output-on-failure
```

---

## Learning Plan

All design documents are in `../Plan/`.

Start with: [Plan/00_MASTER_PLAN.md](../Plan/00_MASTER_PLAN.md)

---

## Design Principles

1. **Each phase adds exactly one capability** — no big-bang rewrites
2. **IKVStore interface is sacred** — storage layer is always replaceable
3. **Failure first** — every feature is designed for the failure case first
4. **Measure everything** — benchmarks before and after each phase
5. **Sequential commits** — Git history reads like a textbook

---

## License

MIT — Built for learning purposes.
