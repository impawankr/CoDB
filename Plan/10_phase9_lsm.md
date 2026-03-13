# Phase 9 — LSM Storage Engine

### High-Throughput Writes: Replace HashMap with a Real Storage Engine

---

## The Problem

All previous phases used an in-memory `unordered_map` backed by a WAL.

**Problems at scale:**

- Map must fit entirely in RAM
- No efficient range scans (keys are unordered in a hash map)
- WAL grows unbounded
- Recovery time grows linearly with WAL size
- No compression

**Goal of this phase:** Replace the in-memory map with a proper **Log-Structured Merge Tree** — the same storage engine used by RocksDB, LevelDB, Apache Cassandra, and HBase.

---

## Why LSM Tree?

### Naive approach: B-Tree (random writes)

B-Trees store data sorted on disk. Every write may require:

- Seeking to a random disk location
- Reading a page
- Modifying the page
- Writing it back

Random I/O on SSDs: ~100 μs per operation.  
At 100K writes/sec → 10 seconds of disk time → **bottleneck**.

### LSM Tree: Sequential writes only

```
Write path: always append → always sequential
Sequential SSD writes: ~10 μs per operation
10x throughput advantage over B-Trees for write-heavy workloads
```

---

## LSM Tree Architecture

```
                 Writes
                   ↓
┌──────────────────────────────────┐
│         MemTable (RAM)           │  ← sorted in-memory structure (std::map / skip list)
│    (e.g., 64 MB write buffer)    │
└──────────────────┬───────────────┘
                   │ when full → flush
                   ↓
┌──────────────────────────────────┐
│     Level 0: SSTable files       │  ← immutable sorted files on disk
│  [sst_0_001] [sst_0_002] ...    │
└──────────────────┬───────────────┘
                   │ compaction
                   ↓
┌──────────────────────────────────┐
│     Level 1: SSTable files       │  ← larger, sorted, non-overlapping
└──────────────────┬───────────────┘
                   │ compaction
                   ↓
         Level 2, Level 3, ...
```

---

## Component 1: MemTable

An **in-memory sorted map** of recent writes.

```cpp
class MemTable {
public:
    void put(const std::string& key, const std::string& value);
    void del(const std::string& key);
    std::optional<std::string> get(const std::string& key) const;

    bool is_full() const;   // e.g., size > 64MB
    size_t size_bytes() const;

    // Produce an immutable snapshot for flushing
    std::shared_ptr<ImmutableMemTable> freeze();

private:
    std::map<std::string, ValueOrTombstone> data_;  // sorted by key
    std::atomic<size_t> size_bytes_{0};
    mutable std::shared_mutex mu_;
};
```

**Why `std::map` and not `unordered_map`?**  
Keys must be **sorted** so the flush to SSTable produces a sorted file.  
In production: use a **skip list** for better concurrent performance.

---

## Tombstones (Delete Markers)

You cannot immediately remove a key from an LSM tree.  
The key may exist in older SSTables that haven't been compacted yet.

```
DELETE user:1
→ Write a tombstone: { key: "user:1", value: DELETE_MARKER }
→ On read: if tombstone is most recent version → return "not found"
→ Tombstones are physically removed only during compaction
```

```cpp
struct ValueOrTombstone {
    bool is_tombstone = false;
    std::string value;
};
```

---

## Component 2: SSTable (Sorted String Table)

When a MemTable fills up → it is **flushed** to an immutable SSTable file on disk.

### SSTable File Format

```
┌─────────────────────────────────────────────────────────┐
│                   Data Block (4KB pages)                 │
│  [key_len][key][val_len][val] [key_len][key]...          │
├─────────────────────────────────────────────────────────┤
│                   Index Block                            │
│  [first_key_in_page] → [file_offset] for each page      │
├─────────────────────────────────────────────────────────┤
│                   Bloom Filter                           │
│  Probabilistic set: "is this key probably in this file?" │
├─────────────────────────────────────────────────────────┤
│                   Footer                                 │
│  [index_offset] [bloom_offset] [magic_number]           │
└─────────────────────────────────────────────────────────┘
```

### Bloom Filter

Before reading an SSTable, check the bloom filter:

- If bloom says "NO" → key definitely not in this file → skip it
- If bloom says "YES" → key probably in this file → read and verify

Without bloom filters, a GET for a non-existent key reads every SSTable.  
With bloom filters, typically 0–1 SSTable reads for non-existent keys.

---

## Component 3: Compaction

Over time, Level 0 accumulates many SSTable files.  
Reading requires checking all of them.

**Compaction** merges multiple SSTables into one, sorted and deduplicated:

```
Before compaction:
  sst_001: { a:1, b:3, c:DELETE }
  sst_002: { a:2, d:1 }           ← newer

After compaction (merge + keep latest):
  sst_merged: { a:2, b:3, d:1 }  ← c is removed (tombstone collected)
```

### Leveled Compaction (RocksDB default)

```
Level 0: up to 4 SSTables (overlapping keys allowed)
Level 1: total size ~10MB, non-overlapping keys
Level 2: total size ~100MB, non-overlapping keys
Level N: 10x larger than Level N-1
```

When Level N exceeds its size limit → compact into Level N+1.

---

## Read Path

```
1. Check MemTable (most recent writes)
2. Check Immutable MemTables (being flushed)
3. For each Level, from L0 to Lmax:
   a. Check bloom filter → skip if definitely absent
   b. Binary search the index block → find page offset
   c. Read page → search for key
   d. Return first match found
```

---

## Write-Ahead Log Integration

The WAL from Phase 2 is still used — but now only for MemTable recovery:

```
Write flow:
  1. Append to WAL (crash safety)
  2. Insert into MemTable
  3. Return to client

If crash:
  Replay WAL → rebuild MemTable → continue

After MemTable flush to SSTable:
  WAL entries before the flush are no longer needed → truncate WAL
```

---

## Directory Structure Changes

```
codb/
└── src/
    └── storage/
        ├── lsm/
        │   ├── memtable.h              ← NEW: sorted in-memory table
        │   ├── memtable.cpp            ← NEW
        │   ├── sstable_writer.h        ← NEW: flush MemTable to SSTable file
        │   ├── sstable_writer.cpp      ← NEW
        │   ├── sstable_reader.h        ← NEW: read from SSTable file
        │   ├── sstable_reader.cpp      ← NEW
        │   ├── bloom_filter.h          ← NEW: probabilistic membership test
        │   ├── bloom_filter.cpp        ← NEW
        │   ├── compactor.h             ← NEW: leveled compaction
        │   ├── compactor.cpp           ← NEW
        │   └── lsm_engine.h            ← NEW: top-level LSM, implements IKVStore
        │       lsm_engine.cpp          ← NEW
        │
        └── wal/                        ← existing (Phase 2), now serves MemTable only
```

---

## Testing Plan

### MemTable Basic Test

```cpp
MemTable mt;
mt.put("apple", "1");
mt.put("cherry", "3");
mt.put("banana", "2");

// Verify iteration is in sorted order: apple, banana, cherry
```

### Flush and Read Test

```cpp
// Fill MemTable, flush to SSTable
// Create new empty MemTable
// Read keys from SSTable via SSTableReader
// Verify all keys present with correct values
```

### Bloom Filter Test

```cpp
BloomFilter bf(1000, 0.01);  // 1000 elements, 1% false positive rate
bf.add("key1"); bf.add("key2");
assert(bf.might_contain("key1") == true);
assert(bf.might_contain("key2") == true);
// "key3" was never added — should return false ~99% of the time
```

### Compaction Test

```cpp
// Create 3 SSTables with overlapping keys and different versions
// Run compaction
// Verify: output SSTable has no duplicates, only latest values
// Verify: tombstones are removed if no older versions exist
```

### Performance Benchmark

```bash
# Write 1M key-value pairs (1KB each)
# Measure: writes per second
# Compare: MemKVStore (Phase 1) vs LSMEngine (Phase 9)
# Expected: LSMEngine handles higher sustained throughput
```

---

## Key Concepts After This Phase

- Why sequential writes are faster than random writes on SSDs
- Why tombstones are needed instead of direct deletes
- How bloom filters reduce unnecessary disk reads
- Why compaction is necessary (space amplification without it)
- The tradeoff between write amplification (compaction cost) and read amplification (number of files to check)

---

## The Three Amplification Factors

| Factor              | Description                         | B-Tree | LSM Tree               |
| ------------------- | ----------------------------------- | ------ | ---------------------- |
| Write Amplification | Bytes written per byte of user data | ~2x    | 10-30x                 |
| Read Amplification  | Disk reads per user read            | ~2     | 1-10 (with bloom)      |
| Space Amplification | Disk space per byte of live data    | ~2x    | 1.1-2x (after compact) |

LSM trees trade **write amplification** (compaction rewrites data) for **write throughput** (sequential I/O).

---

## Git Commits for This Phase

```
feat(lsm): implement MemTable with sorted map and tombstone support
feat(lsm): implement SSTableWriter with index block and bloom filter
feat(lsm): implement SSTableReader with binary search and bloom check
feat(lsm): implement BloomFilter with configurable false positive rate
feat(lsm): implement leveled compaction with merge-sort of SSTables
feat(lsm): implement LSMEngine as IKVStore with MemTable + SSTable layers
feat(wal): integrate WAL truncation after MemTable flush
refactor(server): switch server to LSMEngine storage backend
bench(lsm): add write throughput benchmark comparing MemKVStore vs LSMEngine
```

---

## Completion Criteria

- [ ] MemTable correctly stores and retrieves keys in sorted order
- [ ] SSTable flush produces a valid file readable by SSTableReader
- [ ] Bloom filter rejects non-existent keys with < 5% false positive rate
- [ ] Compaction merges 3 overlapping SSTables into 1 correct output
- [ ] Tombstones are cleaned up during compaction
- [ ] LSMEngine survives crash and recovers via WAL replay
- [ ] Write throughput is measurably higher than Phase 1 MemKVStore under sustained load

---

## You Have Now Built a Distributed Database

After completing this phase, CoDB has:

```
✓ gRPC service layer         (Phase 1)
✓ Write-Ahead Log            (Phase 2)
✓ Leader-Follower Replication (Phase 3)
✓ Quorum Replication         (Phase 4)
✓ Consistent Hashing         (Phase 5)
✓ Gossip Failure Detection   (Phase 6)
✓ Raft Consensus             (Phase 7)
✓ Vector Clock Conflict Res  (Phase 8)
✓ LSM Storage Engine         (Phase 9)
```

This is the architecture of **Amazon DynamoDB, Apache Cassandra, and CockroachDB** — at a learning scale.

---

## Optional Phase 10 → [Distributed Transactions & Sagas](../README.md)
