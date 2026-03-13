# Phase 1 — Single Node KV Store (gRPC)

### The Foundation: RPC Service Layer

---

## Objective

Build a **single-node key-value database** accessible over gRPC.  
No replication. No durability. Just clean architecture that will scale.

> Getting architecture right here is critical.  
> Every future phase plugs into this layer. Mistakes now compound later.

---

## The Problem This Phase Solves

You need a stable **RPC control plane** before adding distributed logic.

All serious distributed systems (etcd, CockroachDB, Cassandra) start with a clean service abstraction:

```
Client
   ↓
Network (RPC)
   ↓
Service Handler
   ↓
Storage Abstraction
```

If these layers are tightly coupled, adding replication later becomes surgery.  
If they are **cleanly separated**, replication is just another caller of the storage abstraction.

---

## What You Will Build

```
Operations:
  PUT  key  value   → stores the value
  GET  key          → returns the value or "not found"
  DELETE key        → removes the key

Non-goals (deferred to later phases):
  × No persistence (data lost on restart)
  × No replication
  × No transactions
```

---

## Architecture

```
┌─────────────────────────────────────────┐
│                   Client                │
│            (kv_client CLI tool)         │
└──────────────────┬──────────────────────┘
                   │  gRPC (proto3)
┌──────────────────▼──────────────────────┐
│           KVServiceImpl                 │
│   (translates RPC ↔ storage calls)      │
└──────────────────┬──────────────────────┘
                   │
┌──────────────────▼──────────────────────┐
│               KVStore                   │
│   (in-memory, thread-safe, pure C++)    │
└─────────────────────────────────────────┘
```

**Key principle:** `KVServiceImpl` knows nothing about storage internals.  
`KVStore` knows nothing about gRPC.  
This boundary is sacred.

---

## Directory Structure

```
codb/
├── CMakeLists.txt
├── README.md
│
├── proto/
│   └── kvstore.proto           ← gRPC service definition
│
├── src/
│   ├── server/
│   │   ├── main.cpp            ← starts gRPC server
│   │   └── kv_service_impl.cpp ← RPC → storage bridge
│   │   └── kv_service_impl.h
│   │
│   ├── client/
│   │   └── main.cpp            ← CLI: put/get/delete
│   │
│   └── storage/
│       ├── kv_store.h          ← storage interface
│       └── kv_store.cpp        ← in-memory implementation
│
└── scripts/
    └── run_server.sh
```

---

## Step-by-Step Implementation

### Step 1 — Proto Definition (`proto/kvstore.proto`)

Define the gRPC contract. This is the public API of CoDB.

```proto
syntax = "proto3";
package codb;

service KVStore {
  rpc Put    (PutRequest)    returns (PutResponse);
  rpc Get    (GetRequest)    returns (GetResponse);
  rpc Delete (DeleteRequest) returns (DeleteResponse);
}

message PutRequest    { string key = 1; string value = 2; }
message PutResponse   { bool success = 1; string error = 2; }

message GetRequest    { string key = 1; }
message GetResponse   { bool found = 1; string value = 2; }

message DeleteRequest { string key = 1; }
message DeleteResponse{ bool success = 1; }
```

**Why proto3?** Forward/backward compatibility when we add replication fields later.

---

### Step 2 — Storage Abstraction (`src/storage/kv_store.h`)

Design this as an **interface**, not a concrete class.  
Later phases will swap the in-memory backend for WAL-backed storage.

```cpp
#pragma once
#include <string>
#include <optional>

// Pure abstract storage interface
// Phase 2 will provide a WAL-backed implementation
class IKVStore {
public:
    virtual ~IKVStore() = default;
    virtual bool put(const std::string& key, const std::string& value) = 0;
    virtual std::optional<std::string> get(const std::string& key) = 0;
    virtual bool remove(const std::string& key) = 0;
};
```

**Why `std::optional` for get?** Distinguishes between "key not found" and "value is empty string".

Concrete in-memory implementation:

```cpp
class MemKVStore : public IKVStore {
public:
    bool put(const std::string& key, const std::string& value) override;
    std::optional<std::string> get(const std::string& key) override;
    bool remove(const std::string& key) override;
private:
    std::unordered_map<std::string, std::string> store_;
    mutable std::shared_mutex mu_;   // readers don't block each other
};
```

**Why `shared_mutex`?**  
Reads are concurrent (many clients query at once).  
Writes are exclusive (only one writer at a time).  
`shared_mutex` gives `shared_lock` for reads, `unique_lock` for writes.

---

### Step 3 — Service Implementation (`src/server/kv_service_impl.cpp`)

This class bridges gRPC callbacks into storage calls.

```cpp
class KVServiceImpl final : public codb::KVStore::Service {
public:
    explicit KVServiceImpl(std::shared_ptr<IKVStore> store);

    grpc::Status Put(grpc::ServerContext*, const codb::PutRequest*,
                     codb::PutResponse*) override;

    grpc::Status Get(grpc::ServerContext*, const codb::GetRequest*,
                     codb::GetResponse*) override;

    grpc::Status Delete(grpc::ServerContext*, const codb::DeleteRequest*,
                        codb::DeleteResponse*) override;
private:
    std::shared_ptr<IKVStore> store_;
};
```

**Critical design note:** `IKVStore` is injected via constructor (dependency injection).  
When Phase 3 adds a replication layer, you inject `ReplicatedKVStore` instead — **zero changes to the service layer**.

---

### Step 4 — Server Entry Point (`src/server/main.cpp`)

```cpp
int main(int argc, char** argv) {
    std::string addr = "0.0.0.0:50051";
    if (argc > 1) addr = "0.0.0.0:" + std::string(argv[1]);

    auto store = std::make_shared<MemKVStore>();
    KVServiceImpl service(store);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    auto server = builder.BuildAndStart();
    std::cout << "[codb] Server listening on " << addr << "\n";
    server->Wait();
}
```

---

### Step 5 — CLI Client (`src/client/main.cpp`)

```
Usage:
  ./codb_client <host:port> put <key> <value>
  ./codb_client <host:port> get <key>
  ./codb_client <host:port> delete <key>

Examples:
  ./codb_client localhost:50051 put name "elon"
  ./codb_client localhost:50051 get name
  > elon
  ./codb_client localhost:50051 delete name
  ./codb_client localhost:50051 get name
  > (not found)
```

---

## CMake Build Setup

Key dependencies:

- `grpc`
- `protobuf`
- `absl` (comes with grpc)

Install via Homebrew on macOS:

```bash
brew install grpc protobuf cmake
```

CMakeLists.txt minimum viable structure:

```cmake
cmake_minimum_required(VERSION 3.20)
project(codb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

# Proto generation
# Storage library
# Server executable
# Client executable
```

---

## Testing Plan

### Manual Test Sequence

```bash
# Terminal 1
./codb_server 50051

# Terminal 2
./codb_client localhost:50051 put user:1 "alice"
./codb_client localhost:50051 put user:2 "bob"
./codb_client localhost:50051 get user:1    # expect: alice
./codb_client localhost:50051 get user:3    # expect: not found
./codb_client localhost:50051 delete user:2
./codb_client localhost:50051 get user:2    # expect: not found
```

### Failure Observation Test

```bash
# Kill the server while client is running
# Observe the error response
# Restart server
# Observe: all data is GONE (no persistence yet)
# This pain motivates Phase 2 (WAL)
```

---

## Key Concepts to Understand After This Phase

After completing Phase 1, you should understand:

- How gRPC stubs and services work in C++
- Why interface-based design (`IKVStore`) matters for testability and extensibility
- Why `shared_mutex` is better than `mutex` for read-heavy workloads
- What happens to in-memory data when a process crashes
- Why the RPC layer and storage layer must be completely decoupled

---

## What This Phase Does NOT Handle

| Problem                     | Solution Phase          |
| --------------------------- | ----------------------- |
| Data lost on crash          | Phase 2 (WAL)           |
| Cannot scale past 1 machine | Phase 5 (Sharding)      |
| No replication = no HA      | Phase 3 (Replication)   |
| No conflict resolution      | Phase 8 (Vector Clocks) |

---

## Git Commits for This Phase

```
feat(proto): define KVStore gRPC service and message types
feat(storage): add IKVStore interface and MemKVStore implementation
feat(server): implement KVServiceImpl with gRPC handlers
feat(server): add server main with configurable port
feat(client): add CLI client with put/get/delete commands
feat(build): configure CMakeLists with grpc and protobuf
docs(phase1): add README with build and run instructions
```

---

## Completion Criteria

- [ ] Server starts and accepts gRPC connections
- [ ] `put`, `get`, `delete` work correctly over the network
- [ ] Concurrent clients do not cause data races (verified with thread sanitizer)
- [ ] Getting a non-existent key returns a clear "not found" response
- [ ] Server restart clears all data (confirming no persistence — motivation for Phase 2)

---

## Next → [Phase 2: Write-Ahead Logging](03_phase2_wal.md)
