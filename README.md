# KDS

**A fast, reliable database system specialized for OLTP.**

KDS does not try to be everything a traditional RDBMS is. It deliberately narrows its feature surface to what online transaction processing actually needs — point lookups, short transactions, tight tail latency, real durability — and delivers those extremely fast. In exchange for that focus, KDS does something most databases don't:

> **KDS treats physical data placement as a first-class optimization target.**
> Alongside the query optimizer every database has, KDS has a **physical optimizer** of equal rank: runtime access-pattern statistics don't just steer query plans — they periodically **rearrange the data itself** so that the pages your workload touches become fewer, denser, and hotter in cache.

## Design Philosophy

- **OLTP-first, not general-purpose.** Primary-key point reads/writes, equi-join chains, pagination, short transactions. No subqueries, no window functions, no dialect-compatibility shims. The grammar is small enough that *the query is the plan* — execution shape is classified at parse time and dispatched without plan search.
- **The engine observes, then reorganizes.** Every tuple access feeds a statistics layer (**Waystone**). Time-decayed scores classify data hot/warm/cold; the physical optimizer clusters hot tuples, compacts dead ones, and co-locates rows that recurring patterns (e.g. joins) touch together. Statistics change *where bytes live*, not just how queries run.
- **Reliability is a product feature.** Write-ahead logging with per-transaction durability classes, page checksums, full-page-image torn-write recovery, and crash-recovery paths that are exercised — not assumed — by deterministic fault-injection tests.
- **Mechanical sympathy everywhere.** Thread-per-core, shared-nothing: each core owns its data, its buffer pool, and its WAL stream. The buffer-cache hit path is a hash probe and an integer increment — no locks, no atomics. All I/O is asynchronous behind an injectable seam, so the entire engine runs under deterministic simulation.

## Architecture

```
                        clients (KWP binary protocol)
                                    │
                        ┌───────────▼───────────┐
                        │   TCP server / KWP    │  frames · sessions · txn control
                        └───────────┬───────────┘
                        ┌───────────▼───────────┐    pattern fingerprints
                        │  Parser (extended)    │──────────────┐
                        └───────────┬───────────┘              │
                        ┌───────────▼───────────┐   access     ▼
                        │       Executor        │─ events ─▶ ┌─────────────────┐
                        └───────────┬───────────┘            │ Waystone stats  │
                                    │                        └────────┬────────┘
                        ┌───────────▼───────────┐    scores/patterns  │
                        │    B+ tree (pk)       │◀── validated by ────┤
                        └───────────┬───────────┘                     ▼
                                    │                     ┌───────────────────────┐
                                    │      relayout ◀──── │  Physical optimizer   │
                                    ▼                     │  (peer of query opt.) │
                        ┌───────────────────────┐         └───────────────────────┘
                        │   Semi-sorted heap    │
                        │ (min_key pages,       │
                        │  Keystone tuples)     │
                        └───────────┬───────────┘
              ┌─────────────────────┼─────────────────────┐
   ┌──────────▼──────────┐ ┌────────▼─────────┐ ┌─────────▼─────────┐
   │ Buffer pool (1/core)│ │ WAL (1/core)     │ │  Space manager    │
   │ clock · bg writer   │ │ D1/D2/D3 classes │ │  extents · freemap│
   └──────────┬──────────┘ └────────┬─────────┘ └─────────┬─────────┘
              └─────────────────────┼─────────────────────┘
                        ┌───────────▼───────────┐
                        │ Single growable file  │  + per-core WAL segment streams
                        └───────────────────────┘
```

## Components

| Component | What it does |
|---|---|
| **KWP wire protocol** | Custom length-prefixed binary protocol: version/capability handshake, extended PARSE/BIND/EXECUTE, chunked result streaming with explicit flow control, per-transaction durability selection, structured retryable errors |
| **Parser** | Small OLTP grammar. Parameterizes literals during the parse (query fingerprints come out for free), tags each statement with an execution class, binds catalog names to oids at parse time — the executor never re-analyzes shape or resolves names |
| **Executor** | Switch-dispatch on statement classes; nested-loop equi-joins in written order ("the query is the plan"); emits access events as it touches tuples |
| **Semi-sorted heap** | 8 KiB pages with an immutable per-page key lower bound (`min_key`): pages are unordered inside, ordered between — range pruning without full sorting. Each tuple carries a 64-bit **Keystone** word (40-bit id · flags/lock byte · reserved) as its identity |
| **B+ tree** | The authoritative pk → location index. One tree core, thin facades; append-optimized for monotonic engine-issued ids (rightmost fast path, asymmetric splits). Core-local — no latching protocols at all |
| **Waystone** | Full-coverage access statistics: one 256-bit entry per tuple, O(1) pk-addressed through a per-relation page directory. Tracks decayed heat and last-known location; strictly advisory — droppable wholesale without changing any result. Toggleable per table |
| **Physical optimizer** *(experimental)* | The headline: threshold-triggered relayout that clusters hot tuples and compacts pages, plus advisory "bundle pages" co-locating the row locations that recurring multi-table patterns touch together. Ships behind flags with a shadow mode for measured, gradual rollout |
| **Buffer pool** | One per core over core-owned pages. RAII pinned-page handles, clock eviction, background writer, WAL-ordering gate enforced in code. Hit path: zero locks, zero atomics, zero allocation |
| **WAL** | Per-core append-only streams. Physiological redo + undo-chain MVCC (writer trx-id + undo pointer; no xmax). Durability classes per transaction: `strict` / `group` / `relaxed`. Fuzzy checkpoints, full-page images, point-in-time-recovery-ready archives |
| **Storage** | One growable data file, pure arithmetic page addressing (`offset = page_id × 8 KiB`), extent-based crash-safe growth, bitmap free-space management, CRC32C page checksums. mmap deliberately rejected — explicit async I/O only |
| **Scheduler** | Cooperative reactor pinned per core: run-to-completion tasks, scheduling groups (foreground / system / maintenance) with SLO-based throttling instead of preemption |
| **Deterministic testing** | Clock, randomness, and all I/O are injected. The whole engine runs single-threaded under a simulated scheduler with crash and torn-write injection — durability claims are tested, not asserted |

## What KDS is not

No subqueries or CTEs, no window functions, no cross-dialect SQL compatibility, no attempt to be a data warehouse. Aggregations and secondary indexes are on the roadmap, not in the core promise. If your workload is analytical scans over wide history, use a column store; if it is high-rate transactional access to living data, KDS is built for exactly that.

## Status

Under active development. The design is specification-first: every subsystem has a confirmed spec with explicit open decisions and required tests in [`docs/`](docs/) — start with the design spec, then `storage-layout.md`, `wal.md`, `wire-protocol.md`, `waystone-concept.md`, and `physical-optimizer-blueprint.md`.

```bash
./build.sh        # build
./test.sh         # run the full deterministic test suite
tools/ckdbs_cli.py --port 15432   # talk to a running instance
```
