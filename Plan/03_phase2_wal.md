# Phase 2 — Write-Ahead Log (WAL)

### Durability: Surviving a Crash

---

## The Problem

After Phase 1, restart the server. Every key you stored is gone.

This is not a bug. It is expected. Data was in RAM.

```
RAM  → volatile (lost on crash)
Disk → persistent (survives crash)
```

But naively writing to disk on every operation creates a new problem:
**random disk I/O is extremely slow**.

---

## The Pattern: Write-Ahead Logging

**Core rule:**

> Before any change takes effect, write what you are about to do to a log on disk.  
> If you crash, replay the log to restore state.

This is used in:

- PostgreSQL (`pg_wal/`)
- MySQL (InnoDB redo log)
- RocksDB (WAL)
- SQLite (journal file)

It solves the durability problem with **sequential writes** (fast) instead of random writes (slow).

---

## How WAL Works

```
Client: PUT user:1 alice

Step 1: Append to WAL on disk
        [PUT | user:1 | alice | seq=1 | crc=0xABCD]

Step 2: Apply to in-memory store
        store_["user:1"] = "alice"

Step 3: Return success to client

--- NODE CRASHES HERE ---

Step 4 (recovery): Open WAL file
Step 5 (recovery): Replay each entry in order
Step 6 (recovery): In-memory store is restored
```

If the node crashes **between Step 1 and Step 2** — the write is replayed on restart.  
If the node crashes **before Step 1** — the write never happened (acknowledged nothing).  
There is no unsafe middle state.

---

## Log Entry Format

Each log entry must contain:

1. **Sequence number** — for ordering and deduplication
2. **Operation type** — PUT, DELETE
3. **Key**
4. **Value** (empty for DELETE)
5. **CRC checksum** — detect partial writes / corruption

### Binary Format (on disk)

```
┌─────────────────────────────────────────────────────────────┐
│ SEQ (8 bytes) │ OP (1 byte) │ KEY_LEN (4B) │ VAL_LEN (4B) │
├─────────────────────────────────────────────────────────────┤
│               KEY (variable)                                 │
├─────────────────────────────────────────────────────────────┤
│               VALUE (variable)                               │
├─────────────────────────────────────────────────────────────┤
│               CRC32 (4 bytes)                                │
└─────────────────────────────────────────────────────────────┘
```

**Why binary and not text/JSON?**  
Binary is faster to parse, smaller on disk, and easier to detect partial writes.  
JSON logs are readable but 3–5x larger and slower to parse on recovery.

---

## The Checksum Problem

What if the process crashes **mid-write** to the log itself?

You could have a partial entry at the end of the WAL file:

```
[entry 1: complete] [entry 2: complete] [entry 3: PARTIAL]
```

The CRC detects this. On recovery:

- If CRC matches → apply the entry
- If CRC does not match → discard everything after the last valid entry

---

## Directory Structure Changes

```
codb/
└── src/
    └── storage/
        ├── kv_store.h              ← existing
        ├── kv_store.cpp            ← existing
        ├── wal/
        │   ├── log_entry.h         ← NEW: entry structure + serialization
        │   ├── log_writer.h        ← NEW: append to WAL file
        │   ├── log_writer.cpp      ← NEW
        │   ├── log_reader.h        ← NEW: iterate WAL entries
        │   ├── log_reader.cpp      ← NEW
        │   └── log_manager.h       ← NEW: coordinates write + recovery
        │   └── log_manager.cpp     ← NEW
        └── wal_kv_store.h          ← NEW: WAL-backed IKVStore implementation
            wal_kv_store.cpp        ← NEW
```

---

## Component Design

### `log_entry.h`

```cpp
enum class OpType : uint8_t {
    PUT    = 1,
    DELETE = 2,
};

struct LogEntry {
    uint64_t sequence_num;
    OpType   op;
    std::string key;
    std::string value;

    std::vector<uint8_t> serialize() const;
    static std::optional<LogEntry> deserialize(const uint8_t* data, size_t len);
};
```

### `log_writer.h`

```cpp
class LogWriter {
public:
    explicit LogWriter(const std::string& log_path);
    ~LogWriter();

    bool append(const LogEntry& entry);
    bool sync();   // fdatasync — guarantee bytes hit disk

private:
    int fd_;
    uint64_t next_seq_;
};
```

**Why `fdatasync`?**  
`fwrite` buffers in OS page cache. `fdatasync` forces bytes to physical disk.  
Without it, data can be lost even though we "wrote" it.

### `log_reader.h`

```cpp
class LogReader {
public:
    explicit LogReader(const std::string& log_path);

    // Returns entries in order until EOF or corruption
    std::vector<LogEntry> read_all();

private:
    std::string path_;
};
```

### `wal_kv_store.h` — the new `IKVStore` implementation

```cpp
class WalKVStore : public IKVStore {
public:
    explicit WalKVStore(const std::string& data_dir);

    // Writes to WAL first, then to memory
    bool put(const std::string& key, const std::string& value) override;
    std::optional<std::string> get(const std::string& key) override;
    bool remove(const std::string& key) override;

    // Call at startup to restore state from WAL
    void recover();

private:
    std::unique_ptr<LogWriter> writer_;
    std::unordered_map<std::string, std::string> store_;
    mutable std::shared_mutex mu_;
};
```

---

## Recovery Sequence

```
Server starts
    ↓
WalKVStore::recover()
    ↓
LogReader reads WAL file from beginning
    ↓
For each valid LogEntry:
    if PUT    → store_[key] = value
    if DELETE → store_.erase(key)
    ↓
Server begins accepting requests
```

**The in-memory state is now identical to what it was before the crash.**

---

## Wiring Into Phase 1

The only change to Phase 1 code:

```cpp
// Before (Phase 1):
auto store = std::make_shared<MemKVStore>();

// After (Phase 2):
auto store = std::make_shared<WalKVStore>("./data");
store->recover();   // replay WAL on startup
```

`KVServiceImpl` has **zero changes**. The interface contract holds.

---

## WAL Rotation and Compaction (Preview)

Over time the WAL grows unbounded. Two strategies:

### Strategy 1: Checkpointing

Periodically write the full current state as a **snapshot** file.  
Truncate the WAL.  
On recovery: load snapshot → replay only WAL entries after the snapshot.

### Strategy 2: Log Segmentation

Split WAL into segments (segment_000001.log, segment_000002.log, ...).  
Delete old segments once they are no longer needed.

> We will implement basic checkpointing in Phase 9 (LSM Tree), which is the industrial-strength version of this idea.

---

## Testing Plan

### Durability Test

```bash
./codb_server --data-dir ./data

# In another terminal:
./codb_client put name codb
./codb_client get name       # expect: codb

# Kill the server (Ctrl+C or kill signal)
# Restart the server
./codb_server --data-dir ./data

./codb_client get name       # expect: codb (restored from WAL!)
```

### Crash Mid-Write Simulation

```cpp
// Test: write 1000 entries, kill process at entry 500 via SIGKILL
// Restart: verify all 500 committed entries are present
// Verify no partial/corrupt entries exist
```

### Corruption Detection Test

```bash
# Manually corrupt last few bytes of WAL file
# Restart server
# Verify server starts (ignores corrupt tail)
# Verify all valid entries before corruption are recovered
```

---

## Key Concepts After This Phase

- Why sequential append is faster than random write
- What `fdatasync` does and why `fwrite` alone is not durable
- How CRC checksums detect partial writes
- The difference between a crash-safe and a non-crash-safe write
- Why WAL is the foundation of both replication logs and LSM compaction logs

---

## Performance Reality

WAL adds latency per write:

```
Without WAL:  PUT = RAM write          ≈ 100ns
With WAL:     PUT = disk write + RAM   ≈ 1ms (spinning disk) / 100μs (NVMe)
```

This is the **durability tax**. Every durable write has this cost.

Systems like Cassandra make this tunable:

```
consistency_level = ONE       → async replication, lowest latency
consistency_level = ALL       → wait for all replicas, highest durability
```

---

## Git Commits for This Phase

```
feat(wal): add LogEntry struct with binary serialization
feat(wal): implement LogWriter with fdatasync support
feat(wal): implement LogReader with CRC validation
feat(wal): implement WAL recovery sequence
feat(storage): add WalKVStore backed by write-ahead log
refactor(server): switch server to WalKVStore with data-dir flag
test(wal): add durability and crash recovery tests
```

---

## Completion Criteria

- [ ] Server survives restart with all written data intact
- [ ] WAL file is binary, contains sequence numbers and CRC checksums
- [ ] Partially written (corrupt) tail of WAL is safely ignored on recovery
- [ ] Recovery replays only entries not yet in memory (seq number tracking)
- [ ] `get` still has zero disk I/O (served from memory after recovery)

---

## Next → [Phase 3: Leader–Follower Replication](04_phase3_replication.md)
