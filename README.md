# KDS

**A fast, reliable database system specialized for OLTP.**

KDS does not try to be everything a traditional RDBMS is. It deliberately narrows its feature surface to what online transaction processing actually needs — point lookups, short transactions, equi-join chains, tight tail latency, real durability — and delivers those extremely fast. In exchange for that focus, KDS does two things most databases don't:

> **KDS treats physical data placement as a first-class optimization target.**
> Alongside the query optimizer every database has, KDS has a **physical optimizer** of equal rank: runtime access-pattern statistics don't just steer query plans — they periodically **rearrange the data itself** so that the pages your workload touches become fewer, denser, and hotter in cache.

> **KDS indexes query patterns, not relations.**
> The index you did not create: the engine fingerprints every statement's shape at parse time, watches which pattern instances recur, and records **Waystone trails** — where each recurring `pattern(args)` actually found its rows, across every relation it touched. `CREATE INDEX` exists for the searches a trail may never replace; the recurring lookups teach the database how to serve them for free.

## Design Philosophy

- **OLTP-first, not general-purpose.** Primary-key point reads/writes, inner equi-join chains, predicate-position subqueries, pagination, short transactions. Every statement compiles to a **step chain** executed in written order — *the query is the plan*: execution shape is classified at parse time and dispatched without plan search. No CTEs, no window functions, no dialect-compatibility shims.
- **The engine observes, then reorganizes.** Executions feed a statistics layer (**Waystone**). Time-decayed scores classify data hot/warm/cold; the physical optimizer clusters hot tuples, compacts dead ones, and co-locates rows that recurring patterns touch together. Statistics change *where bytes live*, not just how queries run.
- **Advisory by construction.** Everything learned — trails, scores, placement hints — is structurally advisory: delete all of it and every query still returns the same rows, just slower. The B+ tree stays the sole authority. This is a hard invariant with its own test family, not a design preference.
- **Reliability is a product feature.** Write-ahead logging with per-transaction durability classes, page checksums, full-page-image torn-write recovery, and crash-recovery paths that are exercised — not assumed — by deterministic fault-injection tests.
- **Declarative group constraints.** SQL-92 `CREATE ASSERTION`, restricted to the class that can be checked incrementally — `COUNT(*)`/`SUM(col)` upper bounds per group — enforced lock-free at admission time on the relation's home core. A violating write is refused before it happens; the check is O(1) against a running aggregate, never a re-evaluation.
- **Mechanical sympathy everywhere.** Thread-per-core, shared-nothing: each core owns its data, its buffer pool, and its WAL stream. The buffer-cache hit path is a hash probe and an integer increment — no locks, no atomics. All I/O is asynchronous behind an injectable seam, so the entire engine runs under deterministic simulation.

## Indexing the Workload

A *pattern* is a statement's shape, fingerprinted as it is parsed (`WHERE id = 42` and `WHERE id = ?` converge). A *pattern instance* is that shape with its arguments bound. When an instance recurs (from its second execution on), KDS records its **trail**: the Keystones of every tuple that execution touched — possibly spanning several relations, tagged with the step of the chain that produced each one. Patterns themselves are catalog objects (`sys.patterns`): a durable, inspectable statement of what this database is actually asked to do.

The trust model is one sentence: **a trail may replace a lookup, never a search.** A step whose authoritative work is a keyed descent can be served from a validated trail entry (identity + epoch checked, MVCC applied as ever, per-entry fallback on any miss). A step that must search still searches — a recorded set can never prove that nothing else qualifies. Negation (`NOT IN`, `NOT EXISTS`) is search-class by definition: absence has no witness.

What this buys: a three-relation join served from a trail is three direct page reads instead of three index descents — and a heap relation gets keyed acceleration for its observed patterns *without carrying a single secondary index*.

## Roadmap

The learned layer grows in deliberate steps:

1. **Advisory acceleration** *(current work)* — trails skip descents for recurring patterns; the physical optimizer's shadow report prices the reshaping before any page moves.
2. **Secondary indexes tamed** — declared `CREATE INDEX` shipped first (an index is a Cabin that observed everything, so its correctness argument was already proved); making most declarations unnecessary for recurring patterns remains the trail's ambition.
3. **Bounded set caching** — with a commit-time change stamp per relation, "nothing changed under this result" becomes provable, and search-class steps join the party *(open design)*.
4. **Hands-off operation** — everything needed to run KDS is exposed as data and levers, not intuition: the workload is inspectable (`sys.patterns`, metrics), every optimization can be evaluated before it acts (the physical optimizer's **shadow mode** reports predicted benefit first), and every action is a flag or a threshold with a promotion metric to verify it. The control loop closes without anyone in it; who — or what — sits in the operator seat is deliberately left open.

One property makes the last step sane rather than reckless, and it is structural: the entire learned layer is advisory by invariant, so the worst mistake any operator — scripted, automated, or human — can make through these surfaces costs performance, never correctness.

## Architecture

```
                        clients (KWP binary protocol)
                                    │
                        ┌───────────▼───────────┐
                        │   TCP server / KWP    │  frames · sessions · txn control
                        └───────────┬───────────┘
                        ┌───────────▼───────────┐    pattern fingerprints
                        │        Parser         │──────────────┐
                        └───────────┬───────────┘              ▼
                        ┌───────────▼───────────┐      ┌─────────────────┐
                        │  Executor (step VM)   │◀────▶│ Waystone trails │
                        └───────────┬───────────┘      │  + sys.patterns │
                                    │    record/replay └────────┬────────┘
                        ┌───────────▼───────────┐               │ hot sets
                        │    B+ tree (pk)       │◀─ validates ──┤
                        └───────────┬───────────┘               ▼
                                    │                ┌───────────────────────┐
                                    │   relayout ◀── │  Physical optimizer   │
                                    ▼                │  (peer of query opt.) │
                        ┌───────────────────────┐    └───────────────────────┘
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
| **Parser** | Small OLTP grammar — joins and predicate-position subqueries included. Parameterizes literals during the parse (pattern fingerprints come out for free), tags each statement with an execution class, binds catalog names to oids at parse time — the executor never re-analyzes shape or resolves names |
| **Executor (step VM)** | Every statement is a **step chain** — lookups, probes, scans, nested sub-chains — run in written order. Replay-eligible steps consult the instance's trail first (validated per entry); the rest run authoritatively. Records trails from an instance's second execution |
| **Waystone** | The trail store: per pattern instance `(pattern_id, arg_hash)`, the recorded Keystones of the rows it touched, with last-seen locations and step tags. Reached through `sys.patterns` and a per-pattern directory. Strictly advisory — droppable wholesale without changing any result |
| **Semi-sorted heap** | 8 KiB pages with an immutable per-page key lower bound (`min_key`): pages are unordered inside, ordered between — range pruning without full sorting. Each tuple carries a 64-bit **Keystone** word (40-bit id · flags/lock byte · reserved) as its identity |
| **B+ tree** | The authoritative pk → location index. One tree core, thin facades; append-optimized for monotonic engine-issued ids (rightmost fast path, asymmetric splits). Core-local — no latching protocols at all |
| **Physical optimizer** *(shadow mode built)* | The shadow half exists: `SHOW RELAYOUT` reports every candidate relayout plan with its lazy-decay-weighted benefit and the gate blocking it, and the page epoch that makes moving tuples safe is live at every validation site. The mover does not exist yet, so nothing moves — deliberately: the promotion gate applied to the optimizer itself, with the shadow report as the evidence that opening a gate pays. Beside it, an experimental **cabin optimizer** (off by default) self-manages advisory Cabins from workload statistics — create, extend, heal, drop, under a page budget with a kill switch — where a wrong decision costs performance only, never correctness; `SHOW CABIN_OPTIMIZER` shows every decision with the scores that produced it |
| **Buffer pool** | One per core over core-owned pages. RAII pinned-page handles, clock eviction, background writer, WAL-ordering gate enforced in code. Hit path: zero locks, zero atomics, zero allocation |
| **WAL** | Per-core append-only streams. Physiological redo + undo-chain MVCC (writer trx-id + undo pointer; no xmax). Durability classes per transaction: `strict` / `group` / `relaxed`. Fuzzy checkpoints, full-page images, point-in-time-recovery-ready archives |
| **Storage** | One growable data file, pure arithmetic page addressing (`offset = page_id × 8 KiB`), extent-based crash-safe growth, bitmap free-space management, CRC32C page checksums. mmap deliberately rejected — explicit async I/O only |
| **Scheduler** | Cooperative reactor pinned per core: run-to-completion tasks, scheduling groups (foreground / system / maintenance) with SLO-based throttling instead of preemption |
| **Deterministic testing** | Clock, randomness, and all I/O are injected. The whole engine runs single-threaded under a simulated scheduler with crash and torn-write injection — durability claims are tested, not asserted |

## Glossary

KDS names its own concepts; the stone metaphor is deliberate — a *keystone* holds the structure up, a *waystone* guides the traveler without being the road.

| Term | Meaning |
|---|---|
| **Keystone** | The 64-bit identity word every tuple carries: 40-bit id · 8-bit flags/lock byte · 16 reserved bits. Everything that names a tuple names it by its Keystone id |
| **Waystone** | The advisory store of trails, reached per pattern instance through `sys.patterns`. Strictly a marker beside the road: droppable wholesale without changing any result |
| **Trail** | The recorded path of one pattern instance — the Keystones a previous execution touched, in execution order, with step tags and last-seen locations. A trail may replace a lookup, never a search |
| **Pattern / pattern instance** | A pattern is a statement's *shape*, fingerprinted at parse time as `pattern_id` (literals parameterized, so inline values and bind parameters converge). An instance is that shape with arguments bound: `(pattern_id, arg_hash)`. Patterns are catalog objects in `sys.patterns` |
| **Step chain** | The compiled form of every statement: an ordered list of steps — lookups, probes, ranges, scans, nested sub-chains for subqueries — executed in written order. "The query is the plan" |
| **KWP** | The KDS Wire Protocol: length-prefixed binary frames, version/capability handshake, extended PARSE/BIND/EXECUTE, chunked streaming, per-transaction durability selection |
| **Semi-sorted heap / `min_key`** | The heap layout: pages are unordered inside but ordered between, via an immutable per-page key lower bound (`min_key`) — range pruning without full sorting |
| **Epoch** | A per-heap-page counter bumped whenever its tuples move. Trail entries record the epoch they observed; a mismatch means the location is no longer trusted and the authoritative path runs |
| **Advisory** | The invariant class every learned structure belongs to: deleting it may cost performance but can never change a query result. Enforced by a dedicated test family, not by convention |
| **Shadow mode** | The physical optimizer's evaluation mode: plans are produced and their predicted benefit reported, but nothing moves. The promotion gate between observing an optimization and enacting it |
| **Durability class** | Per-transaction WAL acknowledgment semantics: `strict` (ack after fsync), `group` (same durability point, batched), `relaxed` (bounded loss window, for reconstructible data) |

## What KDS is not

No CTEs or derived tables, no window functions, no cross-dialect SQL compatibility, no attempt to be a data warehouse. `GROUP BY` with `COUNT`/`SUM`/`MIN`/`MAX`/`AVG` is built; `HAVING` and sorted aggregate output are not. Secondary indexes exist (`CREATE INDEX`, multi-column and covering, on clustered relations) for the searches a trail may never replace — an index answers with a *set*, a trail only ever replaces a *lookup* — while recurring point access is served from Waystone trails without one. If your workload is analytical scans over wide history, use a column store; if it is high-rate transactional access to living data, KDS is built for exactly that.

## Benchmarks

Every number in this section comes from a results file under [`bench/`](bench/),
each of which records the commit it was measured at, the exact environment, the
binary's provenance, and its own caveats. Nothing here is quoted from memory or
averaged across runs that used different code. Where a comparison flatters KDS,
the results file says why; where it does not, that is stated with the same
prominence. **Read the caveats section at the bottom before quoting anything.**

### Environment and method

All KDS-vs-PostgreSQL runs share one method, established in
[`bench/results.md`](bench/results.md) and kept by every later file:

| | |
|---|---|
| Host | AWS EC2, AMD EPYC 7571, 2 vCPU, 7 GiB RAM |
| Storage | NVMe-backed EBS volume (ext4/xfs) — never tmpfs, so every fsync is real |
| KDS | `build-release` (`-O3 -DNDEBUG`), newline text protocol, defaults unless the run says otherwise |
| PostgreSQL | **17.10, packaged build, untuned defaults** (`shared_buffers = 128MB`, `synchronous_commit = on`), scratch cluster from `tools/pg_setup.sh` |
| Client | Python 3.9, single connection, one request at a time, same harness on both sides (`tools/bench_common.py`) |
| Statements | Inline-literal text, parsed per execution **on both sides** — no prepared statements anywhere, because KDS has no PARSE/BIND path yet and giving PostgreSQL one would flatter it |
| Verification | Where a workload has a correct answer, `--verify` compares the two engines' replies row for row before any number is kept |

PostgreSQL is left at stock defaults on purpose: a baseline tuned by hand is
not a baseline — and "stock" cuts both ways, which the per-file caveats price.
Both engines share the same 2-vCPU box with the Python client, sequentially,
never concurrently. Client-visible latencies carry a ~90-210 µs Python socket
floor on both sides; server-side numbers are quoted where a file records them.

### Headline summary

One line per benchmark area. "Verdict" is the results file's own conclusion,
not a re-interpretation.

| Area | Workload | KDS | PostgreSQL 17 | Verdict | Source |
|---|---|---|---|---|---|
| pk point access | `WHERE id = <n>`, 50k rows, server p50 | **12 µs** (Waystone) | 74 µs (btree pk) | ~6× faster, both flat in row count | `results-waystone-vs-pg.md` |
| pk-chain join | 2-relation join by pk, client p50 | ~143 µs | ~296-313 µs | ~2.1× faster at every tested size | `results-scenario1-vs-pg.md` |
| `EXISTS` subquery | uncorrelated, hoisted | ~132 µs | ~239 µs | ~1.8× faster, flat | `results-scenario1-vs-pg.md` |
| pk `BETWEEN` range | 200 rows returned | ~388 µs | ~2,254 µs | ~5.8× faster | `results-scenario1-vs-pg.md` |
| Non-pk filter scan | day-slice, 252 → 10,080 rows | 158 → 1,418 µs | 224 → 1,293 µs | **crossover**: KDS wins small, PG wins large | `results-scenario1-vs-pg.md` |
| OLTP txn mix, 1 conn | group commit vs `synchronous_commit=on` | 200.8 TPS | 212.9 TPS | within 6%; PG's tail is tighter (1.2× vs 1.4×) | `results-latency-matrix.md` |
| OLTP txn mix, 4 conns | same | 213.7 TPS | **491.5 TPS** | **PG 2.3× ahead** — its group commit amortizes across connections, KDS's does not yet | `results-latency-matrix.md` |
| Relaxed durability | D3 loss window vs PG sync=on | 1,610 / 2,875 TPS (1/4 conn) | 212.9 / 491.5 | not the same guarantee — stated, not hidden | `results-latency-matrix.md` |
| Secondary index read | selective equality, 10k rows | 9.7× vs own walk | PG picks the same plan shape | index pays where selectivity does | `results-index.md` |
| Index build | `CREATE INDEX`, 10k rows | 8.68 ms | 4.06 ms | **PG ~2× faster** at scale | `results-index.md` |
| Index INSERT overhead | one index, `relaxed` | +0.6-2.1% | +0.8-1.2% | comparable, both small | `results-index.md` |
| Aggregation scaling | 1 → thousands of groups | +46% | +454% (HashAggregate) | fold cost tracks group count better | `docs/workplan-aggregate-perf.md` |
| Aggregation, no index-only scan | `COUNT(*)` over indexed col | costs a full resolve | PG Index Only Scan ~10% cheaper | honest structural gap, gated on a visibility witness | `results-index.md` §7 |
| Multi-core | 4 isolated relations, cores=1 vs 2 | 1.05× | — | parity, as designed — core 0 serves everything until the pipeline lands | `results-multicore.md` |
| Bulk INSERT, durable | 1,000-row `VALUES`, rows/s | **210,165** | 81,400 (`sync=on`) | 2.6× at the widest batch, 1.07× at batch 1 | `results-bulk-insert.md` |
| Freight booking | whole transaction, mean | **3,663 µs** | 4,072 µs | KDS ahead 10%; the *decomposition* is the finding | `results-scenario2-freight.md` |
| Non-pk equality | 15 shape×size cells, p50 ratio | 1.14×-1.99× faster | — | KDS ahead in every cell, walk or index | `results-scenario3-library.md` |
| Cabin controller | 3 business days, day-1 TPS | 1,680 (auto) vs 608 (off) | twin in `results/cabinopt-days-pg.json` | **2.8×**, matching declared-by-hand (1,724) | `results-cabin-optimizer-days.md` |

### 1. Point access by primary key — the headline result

[`bench/results-waystone-vs-pg.md`](bench/results-waystone-vs-pg.md), 2026-07-30.
The comparison that matters: PostgreSQL doing what it actually does for real
users (`PRIMARY KEY`, btree index scan) against a Waystone-enabled KDS
relation. Server-side p50 — each engine's own measurement of the whole
statement, excluding the Python round trip both sides pay:

| rows | KDS scan (no Waystone) | PostgreSQL btree pk | KDS Waystone |
|---:|---:|---:|---:|
| 1,000 | 562 µs | 69 µs | **11 µs** |
| 10,000 | 5,177 µs | — | **12 µs** |
| 50,000 | 28,044 µs | 74 µs | **12 µs** |

Client-visible throughput for the same runs:

| rows | KDS scan | PG seqscan | PG btree pk | KDS Waystone |
|---:|---:|---:|---:|---:|
| 1,000 | 1,331 qps | 2,945 | 4,299 | **8,547** |
| 10,000 | 167 | 701 | 3,840 | **8,784** |
| 50,000 | 33 | 168 | 4,221 | **8,261** |

KDS is ~6× faster than PostgreSQL's btree on this one operation, and both are
flat in row count, as any real index must be. The results file is explicit
that this is a narrow, expected result rather than a general claim: a
validated recorded location replaces a descent, which is precisely and only
what Waystone is for. The two linear columns (KDS's own scan, PG's seqscan)
are there to show what the probe replaced, not to be compared.

### 2. Joins, subqueries and ranges — the backtest workload

[`bench/results-scenario1-vs-pg.md`](bench/results-scenario1-vs-pg.md),
2026-08-07, at 252 / 1,008 / 10,080 bar rows. Both engines produced
**identical model scores to the basis point across all eight models** before
any latency was recorded. Client-side mean/p50 in µs, 200 ops per cell:

| shape | rows | KDS p50 | PostgreSQL p50 | ratio |
|---|---:|---:|---:|---:|
| pk lookup (`read-bar-lookup`) | 252 | ~127 | 224 | 1.8× |
| pk lookup | 10,080 | 126 | 205 | 1.6× |
| pk-chain join (`read-join-point`) | 252 | 142 | 312 | 2.2× |
| pk-chain join | 10,080 | 142 | 295 | 2.1× |
| `EXISTS` subquery | 252 | 132 | 239 | 1.8× |
| `EXISTS` subquery | 10,080 | 127 | 240 | 1.9× |
| pk `BETWEEN`, 200 rows back | 252 | 382 | 2,254 | **5.9×** |
| pk `BETWEEN`, 200 rows back | 10,080 | 388 | 2,286 | **5.9×** |
| non-pk filter (`read-day-slice`) | 252 | **158** | 224 | 1.4× |
| non-pk filter | 1,008 | **256** | 307 | 1.2× |
| non-pk filter | 10,080 | 1,418 | **1,293** | 0.91× |

The file's thesis, which the last three rows are the point of: **KDS's
per-statement cost is a small fixed number and PostgreSQL's is a larger one,
while KDS's per-row cost is slightly higher — so which engine wins is decided
by row count, and the crossover sits inside the measured range.** The file
fits both terms per shape (e.g. day-slice: KDS ≈ 35.2 µs fixed + 128.1 ns/row,
fit error +1.8%) rather than quoting either endpoint as "the" result. The
joins and ranges stay flat because they are pk-keyed: written order is the
plan, each step probes by primary key, and the reply size is constant across
tiers by construction.

### 3. Transactions and durability — where PostgreSQL wins

[`bench/results-latency-matrix.md`](bench/results-latency-matrix.md). A mixed
transactional workload (BEGIN / writes / COMMIT), KDS `durability = group`
against PostgreSQL `synchronous_commit = on` — the equivalent
acknowledgment point. TPS and transaction-latency percentiles:

| configuration | conns | TPS | txn p50 | p99 | tail ratio |
|---|---:|---:|---:|---:|---:|
| KDS `group` | 1 | 200.8 | 5,145 µs | 7,406 µs | 1.4× |
| PostgreSQL `sync=on` | 1 | 212.9 | 4,641 µs | 5,353 µs | **1.2×** |
| KDS `group` | 4 | 213.7 | 14,710 µs | 32,580 µs | 2.2× |
| PostgreSQL `sync=on` | 4 | **491.5** | 8,036 µs | 9,854 µs | **1.2×** |
| KDS `relaxed` | 1 | 1,610.0 | 504 µs | 2,752 µs | 5.5× |
| KDS `relaxed` | 4 | 2,874.9 | 926 µs | 6,333 µs | 6.8× |

Read the two engines' single-connection rows first: within 6% on TPS, and
PostgreSQL's tail is tighter. Then the four-connection rows, which are the
honest part: **PostgreSQL more than doubles and KDS does not**, because PG's
group commit amortizes one fsync across concurrent committers and KDS's
equivalent — real, but serving a cooperative single statement stream — has
nobody to batch with yet. The `relaxed` rows are a different guarantee
(bounded loss window, D3), included because reconstructible data is a real
workload, never as a like-for-like number.

The write path's own ladder, single connection, from `docs/wal.md`'s
measurements: 802 inserts/s `strict`, 798 `group` (a batch of one is a
batch), 6,332 `relaxed`, 7,673 unlogged. And one self-inflicted number worth
keeping visible: sharing one undo page across transactions
([`bench/results-txn-layer-budget.md`](bench/results-txn-layer-budget.md))
took the same 45-second workload from 716 TPS with 97,826 failures to
**1,344 TPS with none** — the failure mode was the engine exhausting its own
page-id space, not a competitor.

### 4. Secondary indexes — honest in both directions

[`bench/results-index.md`](bench/results-index.md), 2026-08-08, twelve
configurations, every read verified row-for-row against the unindexed
relation first. The headline: a selective equality over 10,000 rows is
**9.7× faster** through the index than the walk, 1.9× over 1,000, 1.11× over
200 — and an **11% loss** on a range at 200 rows, the crossover KDS's
stable-plan rule deliberately cannot act on (PostgreSQL declines its own
index at that size; KDS cannot, because declining needs the cardinality
estimate a stable plan refuses — `pattern_id` names a plan forever).

What an index costs to write through, INSERT overhead at `relaxed` (so the
fsync does not mask it):

| rows | KDS no index | KDS 1 index | KDS covering | PostgreSQL 1 index |
|---:|---:|---:|---:|---:|
| 200 | +0.38% | +0.77% | +0.84% | +0.46% |
| 1,000 | +0.99% | +0.28% | +0.42% | +1.16% |
| 10,000 | +0.89% | +1.65% | +1.82% | +0.83% |

Per-statement: +4.5 µs for the first index, +2.5 for the second, ~0.9% of a
default INSERT — with the file's standing caveat that a batch of one is a
batch, and under real concurrency the fsync amortizes and the cost becomes
visible. Build cost is where PostgreSQL is simply better today:

| rows | KDS `CREATE INDEX` | PostgreSQL | KDS per row |
|---:|---:|---:|---:|
| 200 | 209 µs | 597 µs | 1.04 µs |
| 1,000 | 1.13 ms | 807 µs | 1.13 µs |
| 10,000 | 8.68 ms | 4.06 ms | 0.87 µs |

Linear at ~1 µs/row and **about 2× slower than PostgreSQL's** at scale.
Covering columns buy 0.58-0.76 µs per avoided base descent and nothing else,
because **there is no index-only scan and cannot be one** until a visibility
witness exists outside the tuple — measured honestly: `COUNT(*)` over an
indexed column costs the same as fetching the rows, where PostgreSQL's
`Index Only Scan` is ~10% cheaper.

### 5. Aggregation — the fold scales by groups, not rows

[`bench/results-aggregate.md`](bench/results-aggregate.md) (KDS internals) and
[`docs/workplan-aggregate-perf.md`](docs/workplan-aggregate-perf.md) (the
PostgreSQL comparison). The fold's cost tracks **group count**, not row
count: over a 20,000-row scan, 2 groups cost −3.7% (returning 2 rows instead
of 20,000 pays for the fold), 64 groups −3.1%, 1,024 groups +5.7%; the global
form (`COUNT(*), SUM(qty)`, one row back) is **−21%** against the plain scan.
Where the fold buys least is a grouped point/range shape: +29-30% on a
101-row range producing 64 groups, which is a fixed cost against a small
denominator, not a per-row cost.

Against PostgreSQL the scaling claim is the one that matters: from 1 group to
several thousand, KDS's fold degrades **+46%** where PostgreSQL's
HashAggregate degrades **+454%** on the same host — the fold is not the
bottleneck, and the aggregate-perf workplan's later finding was that what
*was* slow was the walk underneath decoding columns nothing reads. Two caps
(`aggregate_max_groups`, `aggregate_max_distinct`) fail a statement rather
than truncate it, so no aggregation number here can come from a silently
partial answer.

### 6. Advisory structures — what "free" costs

The learned structures are advisory (deleting them may cost performance,
never correctness), so their benchmarks price both directions:

- **Waystone** ([`results-waystone-v2.md`](bench/results-waystone-v2.md)):
  22-31× faster repeated point access on a heap relation; a 13-15% *loss* on
  a btree one, cut to **3-6%** by folding the fingerprint into the parse and
  zero-copy tokens — which also made every parse 3-8% faster.
- **Cabin** ([`results-cabin.md`](bench/results-cabin.md)): on the
  disk-backed 10,000-user run, a wash — TPS +3.2%, profit-scan mean −20% at a
  23.9% hit rate. The file's own warning is the part worth quoting: an
  earlier tmpfs run showed +27% TPS, and **that number is a tmpfs artifact**
  — with fsync free the scan was the bottleneck, and on a real device it is
  not. The hit rate is a property of the workload, not the structure.
- **Access statistics**: +1-2% on a point lookup, unmeasurable on anything
  slower. **Physical-optimizer shadow mode**
  ([`results-physical-optimizer-shadow.md`](bench/results-physical-optimizer-shadow.md)):
  zero idle cost at exact noise; the report priced at ~60 µs + 24 ns/slot.
- **Assertions** ([`results-assertion.md`](bench/results-assertion.md)) and
  **bulk INSERT** ([`results-bulk-insert.md`](bench/results-bulk-insert.md)):
  see the files; each records its own INSERT-path overhead.

### 7. Multi-core — parity, stated before it is fixed

[`bench/results-multicore.md`](bench/results-multicore.md), 2026-08-10: four
non-interfering relations, one connection each, `cores=1` vs `cores=2`:

| | cores=1 | cores=2 |
|---|---:|---:|
| wall clock | 11.09 s | 10.56 s |
| aggregate throughput | 2,524 stmt/s | 2,651 stmt/s |
| ratio | — | 1.05× (1.13× on repeat) — **noise, not scaling** |

Every relation reports `owner_core=0` in both configurations: core 0 serves
every statement until the cross-core pipeline lands (`docs/crosscore.md` —
the CC7 ownership decision and its P6b handoff are in; dispatch is not), so
parity is the *correct* current result and this file is the baseline the
pipeline will be measured against.

### 8. Bulk INSERT — the batch ladder against PostgreSQL

[`bench/results-bulk-insert.md`](bench/results-bulk-insert.md), 2026-08-10.
Multi-row `INSERT ... VALUES (...), (...)` at batch sizes 1 / 10 / 100 /
1,000, 100,000 rows per configuration, three durability classes, against
PostgreSQL 17.10 with `synchronous_commit` as the equivalent per-transaction
knob (`off` twins `relaxed`; `on` twins `group`/`strict`). Rows per second,
post-fix engine state (the file's Part I records the slower pre-fix run and
what the fix was — the history is kept, not overwritten):

| rows/statement | KDS `relaxed` | KDS `group` | KDS `strict` | PG `sync=off` | PG `sync=on` | KDS/PG (relaxed) | KDS/PG (durable) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 9,328 | 943 | 951 | 5,503 | 878 | **1.70×** | **1.07×** |
| 10 | 63,369 | 9,116 | — | 36,450 | 8,080 | **1.74×** | **1.13×** |
| 100 | 209,876 | 64,614 | — | 87,920 | 45,630 | **2.39×** | **1.42×** |
| 1,000 | 278,180 | 210,165 | 170,894 | 94,600 | 81,400 | **2.94×** | **2.58×** |

Two things the file is careful about, kept here: the batch-1 durable column
is the fsync ladder both engines pay (943 vs 878 — the device's ~940 µs
write+fsync p50 is measured in the same file, so neither engine has room to
win there), and the run was made under a load gate on a shared box — every
configuration started only below a load threshold, contaminated attempts
were discarded *and listed*, and the noise floor was established from
interleaved A/B repeats (0.06-3.5% by batch size).

### 9. The freight booking — where a transaction spends its time

[`bench/results-scenario2-freight.md`](bench/results-scenario2-freight.md),
2026-08-07. A booking is one transaction: four reads, ~6.6 writes, one
COMMIT, FK-checked throughout. 1,500 bookings per engine, `--verify` on 25.
The wait breakdown — the table this results format exists for:

| wait type | KDS | share | PostgreSQL | share |
|---|---:|---:|---:|---:|
| durability wait (COMMIT, one fsync) | **1,836 µs** | **50.1%** | 1,135 µs | 27.9% |
| write statements (~6.6 inserts, 2 updates) | 967 µs | 26.4% | 1,717 µs | 42.2% |
| reads (4 statements) | 598 µs | 16.3% | 967 µs | 23.7% |
| client, framing, `BEGIN` (residual) | 262 µs | 7.1% | 253 µs | 6.2% |
| **whole booking** | **3,663 µs** | 100% | **4,072 µs** | 100% |

KDS ends ~10% ahead, but the decomposition is the useful part: KDS spends
**half the booking waiting on one fsync** where PostgreSQL spends 28% —
PostgreSQL's write *statements* cost 1.8× more, and its commit costs 1.6×
less. A single-connection run cannot amortize a group commit on either side
(a batch of one is a batch), so the fsync share is the ceiling story for
both engines, told at different points of the transaction.

### 10. Non-pk equality — walk, index and Cabin against PostgreSQL

[`bench/results-scenario3-library.md`](bench/results-scenario3-library.md),
2026-08-08. Five query shapes over a library schema at 200 / 1,000 / 10,000
loans — **442,847 operations across 44 cells, zero errors**, every KDS reply
verified against a client-side-filtered full scan (the check that would
catch an index or Cabin serving an incomplete set), and every indexed cell's
*plan* asserted through `ANALYZE`, so a silent regression to a scan fails
the run rather than passing as a flat number. p50 ratios, above 1.00 = KDS
faster:

| shape | rows | KDS walk | PG seqscan | ratio | KDS index | PG index | ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| `pk-user` | 10,000 | 128 µs | 187 µs | 1.46× | 122 µs | 194 µs | 1.59× |
| `loans-by-user` | 200 | 153 | 255 | 1.66× | 132 | 263 | **1.99×** |
| `loans-by-user` | 10,000 | 1,420 | 1,671 | 1.18× | 141 | 237 | 1.68× |
| `loans-by-book` | 10,000 | 1,415 | 1,671 | 1.18× | 140 | 228 | 1.63× |
| `resv-by-user` | 10,000 | 773 | 885 | 1.14× | 137 | 215 | 1.57× |
| `books-by-author` | 10,000 | 425 | 538 | 1.27× | 136 | 224 | 1.65× |

KDS is ahead in every cell, walking or indexed — the walk ratios narrow with
size (a per-row race KDS wins by less than it wins the fixed cost), the
indexed ratios do not, because both engines are flat and KDS's constant is
smaller. Two findings the file insists on: **KDS chooses the same plan at
every cardinality** (the plan table shows `IndexProbe` at 200 rows where
PostgreSQL declines its own index — the stable-plan trade, visible in data),
and the **Cabin's hit-rate cliff**: (90.6% hits, 14.3% faster than the walk)
at 200 rows becomes (9.1% hits, 172.3% *slower*) at 10,000 — a Cabin pays in
proportion to how often a value is probed again, and this workload's uniform
draw stops re-probing as the value space grows. The file also carries a
status header stating exactly which commit the numbers describe and why a
same-evening refresh was aborted (a concurrent build held the machine) —
the provenance discipline every file in `bench/` follows.

### 11. The business stress mix — the honesty benchmark

[`bench/results-business-stress.md`](bench/results-business-stress.md).
10,000 users, 20,012 accounts, 4 trader processes over disjoint partitions,
120 s measured. Its first table is the most important number in `bench/`
because of what it *removes*:

| | tmpfs (`/tmp`) | real disk (xfs) |
|---|---:|---:|
| TPS | **1,730.6** | **166.8** |
| committed in 120 s | 207,670 | 20,015 |
| txn p50 | 1.78 ms | 11.80 ms |
| INSERT p50 (server-side) | 129 µs | 987 µs |

A 10× swing from the filesystem alone. This is why every results file above
states its device and refuses tmpfs: any number measured where fsync is free
describes a different engine. The same file prices the reporting job (TPS
166.8 with the reporter on, 321.7 without; p99 211 ms vs 49 ms) — the
scan-vs-writes interference that the Cabin and index work exists to reduce.

### 12. The Cabin controller over three business days

[`bench/results-cabin-optimizer-days.md`](bench/results-cabin-optimizer-days.md),
2026-08-10 — the newest file, measuring PHY01-PHY08 end to end: three
compressed business days, hot symbol sets rotating nightly, three arms on
identical data — `off` (no Cabin ever), `on` (`SET CABIN_OPTIMIZER ON`,
the controller decides), `declared` (an operator hand-declares Cabins on
all five symbol columns). Day-1 TPS: **608 off, 1,680 controller (2.8×),
1,724 declared** — the controller reaches parity with a human who knew the
answer in advance, and the file's thesis is the nightly pattern: retirement
(the hot set moving on) is the norm the controller must handle, not an edge
case. A PostgreSQL twin ran in the same session
(`bench/results/cabinopt-days-pg.json`); its comparison lives in the file.
Two runs with different seeds; the quiet-machine run is primary and the
file says which and why.

### 13. Assertions — measured without a twin, and why

[`bench/results-assertion.md`](bench/results-assertion.md), 2026-08-09.
Nine configurations across three durability classes, enforcement stamped in
every run (`SHOW ASSERTIONS` must report `enforcing=on` or the driver
refuses to measure), the violation phase refusing **200/200** attempts with
the exact wire error, and the assertion's own aggregate verified identical
across five comparison relations. **No PostgreSQL twin exists** — the file's
§6 says why rather than leaving it implied: PostgreSQL has no enforced
`CREATE ASSERTION`, and the honest equivalents (triggers, materialized
counters) each measure a different mechanism with different guarantees, so a
"comparison" would be a chart of unlike things. The INSERT-path overhead of
an enforcing assertion is the file's own number; it is a KDS-vs-KDS
measurement by construction.

### Caveats — what these numbers do not show

Stated once, applying to everything above:

1. **Single-connection bias.** Almost every number is one connection, one
   request at a time. That flatters KDS's fsync ladder (a batch of one is a
   batch) and hides PostgreSQL's concurrency strengths — which is why the
   latency matrix's four-connection row, where PG wins 2.3×, is quoted in
   the summary table with the same prominence as the 6× pk result.
2. **PostgreSQL is untuned.** Stock 17.10, 128 MB `shared_buffers`. The
   per-file caveats say where that costs it (e.g. large scans).
3. **Durability is not symmetric.** PostgreSQL fsyncs *and can replay its
   WAL after a crash*. KDS logs INSERT/UPDATE/DELETE at the same
   acknowledgment points but **recovery is not implemented** — nothing reads
   the log back (`docs/known-gaps.md`). A durability number bought without
   recovery is cheaper; keep that in mind wherever the two engines tie.
4. **No prepared statements on either side**, no connection pooling, no
   parallel query on the PG side (untuned defaults on a 2-vCPU box).
5. **The client floor.** ~90-210 µs of Python socket cost rides on every
   client-visible latency on both sides; sub-10 µs client-side differences
   are noise. Server-side numbers are quoted where files record them.
6. **Shared 2-vCPU host.** Engines ran sequentially, never concurrently,
   and each results file records load averages and compiler activity.

### The provenance discipline

Every results file in `bench/` carries the same evidence block, and it is
worth knowing what to expect before opening one, because it is what makes
these numbers quotable at all:

- **The commit measured, and the binary's provenance** — mtime against
  HEAD's commit time, with the delta spelled out when they differ. Where a
  binary predates HEAD, the file proves no engine source differs
  (`results-scenario3-library.md` and `results-bulk-insert.md` both do this
  explicitly, diff by diff); where the tree was dirty, the dirt is listed
  file by file with why none of it is on the measured path.
- **The device, stated and refused when wrong** — every file names the block
  device and states "not tmpfs"; §11's 10× swing is why.
- **Machine state** — load averages before and after every cell, `pgrep`
  for compilers, and in the newest files a hard load gate: a configuration
  starts only below a load threshold, and discarded attempts are listed
  rather than silently retried.
- **A noise floor from inside the run** — interleaved A/B repeats or
  replicate cells, so "X% faster" claims can be read against the spread the
  same harness produced on the same day (0.06-3.5% for bulk insert,
  0.6-8.7% median for scenario3's replicates).
- **Verification before measurement** — `--verify` comparing engines
  row-for-row, plan assertions through `ANALYZE`, enforcement stamps; a run
  that cannot prove it measured the right thing is aborted, and two files
  (`results-scenario3-library.md`'s refresh, a scenario2 contaminated run)
  record aborts as findings rather than deleting them.
- **Supersession is recorded, never overwritten** — `results.md` opens with
  which of its own premises expired and where the replacement lives;
  `results-bulk-insert.md` keeps its pre-fix Part I beside the post-fix
  numbers. A results file is history, and history does not get edited.

### Reproducing

Each comparison has a driver pair under [`tools/`](tools/), sharing one
harness (`tools/bench_common.py`) so the two sides measure identically;
[`bench/docs/README.md`](bench/docs/) records exact invocations per results
file. The pattern:

```bash
tools/pg_setup.sh init                      # scratch PostgreSQL 17 cluster, port 15433
build-release/kds_server --config kds.conf  # KDS on 15432 — Release build, never ./build

tools/benchmark.py            # the four-phase client path (KDS side)
tools/pg_benchmark.py         # its PostgreSQL twin
tools/scenario1_backtest.py   # joins/ranges ladder     (+ pg_scenario1_backtest.py)
tools/scenario2_freight.py    # FK-checked bookings     (+ pg_scenario2_freight.py)
tools/scenario3_library.py    # non-pk equality matrix  (+ pg_scenario3_library.py)
tools/index_benchmark.py      # secondary indexes       (+ pg_index_benchmark.py)
tools/bulk_insert_benchmark.py    # batch ladder        (+ pg_bulk_insert_benchmark.py)
tools/multicore_benchmark.py  # the 4-relation isolation baseline (no PG twin)
```

Ground rules, learned the hard way and enforced by the files above: measure
`build-release` only (`./build` is Debug and has reported the wrong *sign*
twice — `docs/workplan-aggregate-perf.md`); put data files on a real
filesystem, never `/tmp`; run the engines sequentially, never concurrently,
on this 2-vCPU class of box; and re-measure a premise before building on it.

### Full results index

| File | What it measures |
|---|---|
| [`bench/results.md`](bench/results.md) | The original four-phase client-path comparison (partly superseded — its own header says which parts) |
| [`bench/results-waystone-vs-pg.md`](bench/results-waystone-vs-pg.md) | pk point access vs PostgreSQL's btree — the 6× table above |
| [`bench/results-waystone-v2.md`](bench/results-waystone-v2.md) / [`-waystone.md`](bench/results-waystone.md) | Waystone's win and its overhead, heap and btree |
| [`bench/results-scenario1-vs-pg.md`](bench/results-scenario1-vs-pg.md) | The backtest workload at three sizes — joins, subqueries, ranges, the crossover |
| [`bench/results-scenario2-freight.md`](bench/results-scenario2-freight.md) | The freight workload — FK-checked writes and mixed reads |
| [`bench/results-scenario3-library.md`](bench/results-scenario3-library.md) | What a non-pk equality costs: Cabin vs index vs walk vs PostgreSQL |
| [`bench/results-latency-matrix.md`](bench/results-latency-matrix.md) | TPS and tail latency, 1 and 4 connections, vs `synchronous_commit=on` |
| [`bench/results-index.md`](bench/results-index.md) | Secondary indexes: read win, write cost, build cost, the crossover |
| [`bench/results-aggregate.md`](bench/results-aggregate.md) | The fold's cost by group count |
| [`bench/results-cabin.md`](bench/results-cabin.md) | Cabin on a real device, and the tmpfs warning |
| [`bench/results-cabin-optimizer.md`](bench/results-cabin-optimizer.md) / [`-days.md`](bench/results-cabin-optimizer-days.md) | The Cabin controller end to end, and the multi-day workload vs PostgreSQL |
| [`bench/results-txn.md`](bench/results-txn.md) / [`-txn-layers.md`](bench/results-txn-layers.md) / [`-txn-layer-budget.md`](bench/results-txn-layer-budget.md) | MVCC's read-path cost; the durability ladder; the undo-page fix |
| [`bench/results-assertion.md`](bench/results-assertion.md) | Assertion admission checks on the INSERT path |
| [`bench/results-bulk-insert.md`](bench/results-bulk-insert.md) | Multi-row VALUES against the per-row pipeline |
| [`bench/results-business-stress.md`](bench/results-business-stress.md) | The four-table business mix under sustained load |
| [`bench/results-keystone-alloc.md`](bench/results-keystone-alloc.md) | What the id allocator costs, and why the block size is 4096 |
| [`bench/results-access-stats.md`](bench/results-access-stats.md) | The statistics feed's overhead |
| [`bench/results-physical-optimizer-shadow.md`](bench/results-physical-optimizer-shadow.md) | Shadow mode's idle cost (zero) and report cost |
| [`bench/results-multicore.md`](bench/results-multicore.md) | The multi-core parity baseline above |

## Status

Under active development. The design is specification-first: every subsystem has a spec with explicit open decisions and required tests in [`docs/`](docs/) — start with `heap-and-tuple.md` (the authoritative spec) and `rules.md`, then `page.md`, `wal.md`, `txn.md`, `protocol.md`, `parser-v2.md`, and `waystone-concpets.md`. [`docs/known-gaps.md`](docs/known-gaps.md) is the engine-wide list of what is missing and what a restart loses; [`manual/`](manual/) is the user-facing surface, verified against code rather than against specs.

Note `parser.md` and `step-chains.md` are **superseded** by `parser-v2.md` and kept only as history — this list named the first and a `step-chain.md` that never existed.

```bash
./build.sh        # build
./test.sh         # run the full deterministic test suite
tools/ckdbs_cli.py --port 15432   # talk to a running instance
```
