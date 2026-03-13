# Phase 0 — Mental Models

### Think Like a Distributed Systems Architect

---

## Objective

Before writing a single line of code, you need to **rewire how you think about correctness**.

In single-node systems, correctness is the default — you must work hard to break it.  
In distributed systems, **failure is the default** — you must work hard to achieve correctness.

This phase has no code. Only thinking exercises.

---

## 0.1 — The 3 Physical Laws

These are not engineering opinions. These are physics.

### Law 1: Networks are unreliable

A message sent between two machines **may**:

- Never arrive
- Arrive twice
- Arrive out of order
- Arrive after an arbitrarily long delay

> There is no way to distinguish between "the other node is slow" and "the other node is dead."
> This is the core of the **Two Generals Problem**.

**Implication:** You can never know with certainty if a remote operation succeeded.

---

### Law 2: Nodes crash

A running process may halt at any point:

- Mid-write
- Between two operations
- While holding a lock
- While replicating to followers

> There is no "safe moment" to crash. If a node can crash, it will crash at the worst possible moment.

**Implication:** All state changes must be designed to be **recoverable** or **idempotent**.

---

### Law 3: Clocks are not synchronized

No two machines share a global clock. Each machine has a local clock that:

- Drifts over time (NTP only corrects it approximately)
- Can jump backwards (clock adjustments)
- Can disagree by milliseconds to seconds

> You cannot use wall-clock timestamps to determine the order of events across machines.

**Implication:** Event ordering in distributed systems requires **logical clocks**, not physical ones.

---

## 0.2 — CAP Theorem (Correctly Understood)

**Common misconception:** "You must choose 2 out of 3."  
**Reality:** During normal operation, you have all three. CAP only applies **during a network partition**.

```
During a network partition, you must choose:

  Consistency  (all nodes see the same data at the same time)
      OR
  Availability  (every request gets a response, possibly stale)
```

You cannot have both during a partition. Partition tolerance is not optional — networks always partition.

### The Real-World Spectrum

| System           | Choice during partition |
| ---------------- | ----------------------- |
| Apache Cassandra | Availability (AP)       |
| Amazon DynamoDB  | Availability (AP)       |
| Google Spanner   | Consistency (CP)        |
| CockroachDB      | Consistency (CP)        |
| Etcd / ZooKeeper | Consistency (CP)        |

Neither choice is wrong. It depends on what your system **cannot afford to lose**.

---

## 0.3 — PACELC Model (CAP's More Honest Extension)

CAP only describes behavior under partition. But partitions are rare.  
PACELC describes the **always-present tradeoff**:

```
If Partition:   choose between Availability vs Consistency
Else (normal):  choose between Latency vs Consistency
```

```
PA/EL  →  Cassandra (highly available, low latency, weaker consistency)
PC/EC  →  Spanner   (strongly consistent, higher latency)
PA/EC  →  DynamoDB  (available under partition, consistent in normal operation — tunable)
```

**Lesson:** Strong consistency always costs latency. There is no free lunch.

---

## 0.4 — The Consistency Spectrum

This is not binary. It is a spectrum with quantifiable tradeoffs.

```
← weaker                                                      stronger →

Eventual     Causal     Read-Your-Writes    Monotonic    Linearizable
Consistency  Consistency  Consistency       Reads
   (fastest)                                              (most correct, slowest)
```

### Definitions you must internalize

| Level                | Guarantee                                                                                |
| -------------------- | ---------------------------------------------------------------------------------------- |
| **Eventual**         | Writes will propagate to all nodes _eventually_. No timing guarantee.                    |
| **Causal**           | Operations that are causally related appear in order. Unrelated ops may diverge.         |
| **Read-Your-Writes** | After a write, the same client always reads the new value.                               |
| **Monotonic Reads**  | Once you read a value, you never read an older one.                                      |
| **Linearizable**     | Operations appear instantaneous. There is a single total order. The strongest guarantee. |

---

## 0.5 — The Coordination Tax

Every time multiple nodes must **agree** on something, you pay:

```
1 coordination round = 1 network round-trip
                     = ~0.5ms locally
                     = ~80ms cross-region
```

In a high-throughput system doing millions of ops/sec — this is not free.

### How real systems minimize the coordination tax

| System      | Strategy                                                   |
| ----------- | ---------------------------------------------------------- |
| Dynamo      | Leaderless quorum — no global coordinator in write path    |
| Spanner     | TrueTime — uses atomic clocks to reduce uncertainty window |
| CockroachDB | Raft only for metadata — data writes via MVCC              |
| Cassandra   | Gossip for membership — no central metadata server         |

> **Principle:** Move coordination off the hot path. Use it for metadata, not data.

---

## 0.6 — Failure Taxonomy

When you design a system, you must design for **each of these failure types explicitly**:

| Failure Type          | Description                            | Example                    |
| --------------------- | -------------------------------------- | -------------------------- |
| **Crash failure**     | Node stops and stays stopped           | OOM kill, power loss       |
| **Omission failure**  | Node sends/receives no messages        | Network partition          |
| **Timing failure**    | Node responds but too slowly           | GC pause, CPU saturation   |
| **Byzantine failure** | Node sends incorrect or malicious data | Bit flip, compromised node |

> In CoDB, we will handle crash, omission, and timing failures.
> Byzantine failures require cryptographic consensus (out of scope).

---

## 0.7 — The 5 Questions to Ask for Every Design Decision

Before adding any feature to CoDB, answer these 5 questions:

1. **What problem does this solve?**  
   Name the specific failure mode or constraint.

2. **What does it cost?**  
   Latency? Memory? Complexity? Network bandwidth?

3. **What does it break?**  
   Which consistency/availability property gets weaker?

4. **Can we make it optional?**  
   Can callers tune the behavior (e.g., choose quorum size)?

5. **When should we NOT use it?**  
   What is the boundary condition where this pattern fails?

---

## 0.8 — Mental Model Exercise

Before moving to Phase 1, think through this scenario:

### Scenario

You have 3 nodes. A client writes `key=X, value=42` to Node 1.

Node 1 writes the value locally, then starts replicating to Node 2 and Node 3.
**Before replication completes, Node 1 crashes.**

Now answer:

1. What does Node 2 return when queried for `X`?
2. What does Node 3 return when queried for `X`?
3. How does the client know the write succeeded or failed?
4. How does the cluster recover?
5. What would you need to add to prevent this inconsistency?

> Write your answers before reading Phase 3 (Replication).
> Compare after.

---

## 0.9 — Key References to Study (Optionally)

These are the original papers behind every major pattern we will implement.

| Paper / Resource                                                         | Why It Matters                                            |
| ------------------------------------------------------------------------ | --------------------------------------------------------- |
| _Dynamo: Amazon's Highly Available Key-Value Store_ (2007)               | The foundation of quorum replication + consistent hashing |
| _In Search of an Understandable Consensus Algorithm_ — Raft paper (2014) | The most readable consensus paper                         |
| _Bigtable: A Distributed Storage System_ (2006)                          | LSM tree application at Google scale                      |
| _Spanner: Google's Globally Distributed Database_ (2012)                 | TrueTime + external consistency                           |
| _Designing Data-Intensive Applications_ — Martin Kleppmann               | Best practical reference book                             |

---

## Completion Criteria

You are ready to move to Phase 1 when you can answer these without notes:

- [ ] What is the difference between availability and consistency during a network partition?
- [ ] Why can't you use wall-clock time to order events across nodes?
- [ ] Name 3 failure types and one design response to each.
- [ ] What is the coordination tax and why does it matter for throughput?
- [ ] What does "eventual consistency" actually guarantee?

---

## Next → [Phase 1: Single Node KV Store](02_phase1_kv_store.md)
