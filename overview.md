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
| **Physical optimizer** *(shadow mode built)* | The shadow half exists: `SHOW RELAYOUT` reports every candidate relayout plan with its lazy-decay-weighted benefit and the gate blocking it, and the page epoch that makes moving tuples safe is live at every validation site. The mover does not exist yet, so nothing moves — deliberately: the promotion gate applied to the optimizer itself, with the shadow report as the evidence that opening a gate pays |
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

## Status

Under active development. The design is specification-first: every subsystem has a spec with explicit open decisions and required tests in [`docs/`](docs/) — start with `rules.md`, then `page.md`, `wal.md`, `txn.md`, `protocol.md`, `parser.md`, `waystone-concpets.md`, and `step-chain.md`.

```bash
./build.sh        # build
./test.sh         # run the full deterministic test suite
tools/ckdbs_cli.py --port 15432   # talk to a running instance
```
