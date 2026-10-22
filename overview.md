# KDBS

**An OLTP database engine where correctness is declared and the engine enforces it.**

KDBS is a userspace database engine written in C++20 for online transaction processing. It does not try to be everything a traditional RDBMS is: it narrows its feature surface to what OLTP needs — keyed reads and writes, short transactions, joins along key chains, durability the caller chooses — and spends what that saves on two things:

> **Declared invariants, enforced on every write.**
> Assertions, transitions, foreign keys, unique indexes, and a tuple identity that is never rebound. Each is declared in the schema and checked by the engine on every write, on whichever core the write runs, so nothing that writes to the database — a service, a script, an agent — can break it.

> **Acceleration that can be dropped without changing a result.**
> Cabin value sets, and Waystone trails for the query shapes an operator opts in. A Waystone trail may replace a tuple key lookup or probe, never a search; a Cabin set may serve an observed value's predicate without a walk. Drop either — a Cabin set per value, Waystone wholesale — and every query returns the same rows.

## Design Philosophy

- **OLTP-first, not general-purpose.** Tuple key reads and writes, join chains, predicate-position subqueries, pagination, short transactions. A statement the native grammar reads compiles at parse time into a **step chain**; the rest of the supported SQL — outer joins, `HAVING`, set operations, derived tables, value expressions — is answered by a query layer over native single-relation walks (`docs/spec/parser-v2.md` I19, I20). No CTEs, no window functions, no dialect-compatibility shims.
- **One plan choice: inner-join order.** The one choice the engine makes about a plan is the order of an inner-join block's relations, priced by a cost model from the catalog and from plan statistics it learns as statements run (`docs/spec/parser-v2.md` I12, `docs/spec/plan-stats.md`). Every order returns the same rows; `join_order = written` keeps the written one. Without `ORDER BY`, the order rows arrive in is not a contract.
- **Declare, then enforce.** A rule the data must obey belongs in the schema, not in every service that writes. Each declared invariant is checked per write at the state it governs, and a violation is a refusal with its own error, never a repair.
- **Droppable without changing a result.** Everything the engine learns can be deleted without a query returning a different row, each for its own reason. Waystone trails choose *where to look*, never *what is visible* — a hard invariant (`docs/spec/heap-and-tuple.md` §8, invariants 8 and 9) with its own tests; against a trail the clustered B+ tree stays the authority. A Cabin is authoritative for the values it observed, and dropping any of its sets returns those queries to the tree walk (`docs/spec/cabin.md` C1). Plan statistics steer the join order alone (`docs/spec/plan-stats.md`); access statistics are reported by `SHOW ACCESS` and warn a `CREATE CABIN` on a column nothing filters (`docs/spec/heap-and-tuple.md` §7).
- **Reliability is a product feature.** Every authoritative mutation is logged — the free map (repaired by recovery), Waystone trails and `sys.access_stats` are not; pages carry CRC32C checksums; recovery runs at mount; a failed log write or sync stops the log rather than claim durability for a hole. Crash recovery is exercised — not assumed — by a deterministic simulation harness that runs one instance on in-memory devices through the command dispatcher, crashes it at a seed-chosen point and reconciles it against an oracle.
- **Mechanical sympathy everywhere.** Thread-per-core: a cooperative reactor per core runs every statement of a session on that session's core, over one buffer pool and one WAL stream for the instance; state shared across cores is declared in the spec that owns it. The storage, WAL, recovery and statement paths run under that harness; the reactor, listener and wire layer are outside it.

## Declared Invariants

| Invariant | What the engine enforces | Spec |
|---|---|---|
| **Assertion** | `CREATE ASSERTION` bounds a per-group `COUNT(*)` or `SUM(col)`; every write checks its group's running aggregate in O(1), on every core, immediately after a restart included | `docs/spec/assertion.md` |
| **Transition** | `CREATE TRANSITION` declares a column's initial states, its legal `old => new` edges and its deletable states, checked per row from the row alone; a path property follows by induction and no history is stored | `docs/spec/transition.md` |
| **Foreign key** | The parent is checked where the statement runs, under a parent-row lock held to the transaction's decide, so a parent `DELETE` waits for every open child writer on any core. `CASCADE` and `SET NULL` are refused | `docs/spec/foreign-keys.md` |
| **Unique index** | `CREATE UNIQUE INDEX` refuses a write that gives two live rows one key value, checked per row at the latest state | `docs/spec/index.md` §10.2 |
| **Tuple identity (Keystone)** | Every row's 48-bit tuple key is bound to at most one committed row in the life of its relation and is never updatable; only an explicit `PURGE` frees a key for reuse | `docs/rules/keystoneid-invariant.md`, `docs/spec/heap-and-tuple.md` §4.1 |

A refusal caused by a token names that token's byte position, and every refusal says whether the statement was simply wrong (`InvalidArgument`), understood but not built (`NotImplemented`), or outside what the architecture admits (`Unsupported`).

## Acceleration That Can Be Dropped

**Cabin.** A Cabin is a key function `F` over a relation — a column, an immutable expression such as `DATE(ts)`, or a conjunction of literal comparisons — and holds, for each value of `F` it has observed, the tuple keys of the rows carrying it. A set is complete for each value it observed (a superset the reader filters) and authoritative for it — an empty set is an authoritative "no rows" — maintained on every insert and on every update that moves an input; a predicate whose value was never observed walks as it would without the Cabin. Entry sets are memory-resident. A secondary index is a Cabin that has observed everything (`docs/spec/index.md`). On columns declared `CABIN AUTO` or left undeclared (an undeclared column reads as `AUTO`), the Cabin controller can create Cabins from runtime access statistics and drop the ones it created when `cabin_optimizer` is on; it is off by default and experimental (`docs/spec/cabin.md`, `docs/spec/cabin-controller.md`).

**Waystone.** For a query shape an operator opts in (`WAYSTONE ON <select>`), the engine records, per instance of the shape, the tuple keys the last execution found and where it found them, and the next execution reads those locations instead of descending the tree. A shape is identified by its `fetch_id` — the statement's shape with its select list left out and its literals parameterized — and an instance by `(fetch_id, arg_hash)`. A shape nobody opted records nothing and writes nothing; while nothing on the instance is opted, a `SELECT` pays one memory load. Trails are unlogged and every mount discards them; the opt-in itself is logged and survives (`docs/spec/waystone-concpets.md`).

Waystone's trust model is one sentence: **a trail may replace a lookup, never a search.** A step whose authoritative work is a tuple key lookup or probe can be served from a trail entry, which is validated first — the key re-derived from the current execution, the Keystone id and the page epoch checked, MVCC applied exactly as on the authoritative path, any miss falling through for that step alone. A step that must search still searches: a recorded set can never prove that nothing else qualifies, because absence has no witness.

## Architecture

```
                  clients (KWP/1 binary protocol)
                                │
              ┌─────────────────▼──────────────────┐
              │  listener on every core · sessions │  TLS · SCRAM · txn control
              └─────────────────┬──────────────────┘
              ┌─────────────────▼──────────────────┐
              │   parser · compiler · query layer  │
              │   join planner ◀── plan statistics │
              └─────────────────┬──────────────────┘
              ┌─────────────────▼──────────────────┐     ┌─────────────────────┐
              │      executor (step chains)        │◀───▶│ Cabin value sets    │
              │ assertions · transitions · FK      │     │ Waystone (opted in) │
              └─────────────────┬──────────────────┘     └─────────────────────┘
              ┌─────────────────▼──────────────────┐
              │ clustered B+ trees (tuple key)     │  secondary and unique indexes
              │ Keystone · MVCC undo · var-heap    │  lock family
              └─────────────────┬──────────────────┘
        ┌───────────────────────┼───────────────────────┐
┌───────▼────────┐     ┌────────▼────────┐      ┌────────▼────────┐
│ buffer pool    │     │ WAL             │      │ free map        │
│ one per        │     │ one stream per  │      │ region bitmaps  │
│ instance       │     │ instance        │      │ unlogged        │
└───────┬────────┘     └────────┬────────┘      └────────┬────────┘
        └───────────────────────┼───────────────────────┘
              ┌─────────────────▼──────────────────┐
              │ one growable data file             │  + WAL segment files
              └────────────────────────────────────┘
```

## Components

| Component | What it does |
|---|---|
| **KWP wire protocol** | Length-prefixed binary frames: handshake with capability intersection, SCRAM, optional TLS 1.3, PARSE/BIND/EXECUTE over statement and portal handles, typed row batches with portal suspension, transaction and durability frames, a pinned error registry, `C_CANCEL`. Every core accepts on the default port (`docs/spec/protocol.md`) |
| **Parser and compiler** | The native grammar parameterizes literals as it lexes (the shape fingerprint comes out of the parse), binds catalog names to oids, and compiles a step chain; what it declines, the query layer reads and answers over native single-relation walks at the statement's snapshot (`docs/spec/parser-v2.md`) |
| **Join planner** | Orders a top-level inner-join block — every order up to four relations, greedily above — by a cost model over the compiled chain, the catalog and the plan statistics: relation size estimates read from each tree, and the row counts a block's joins produced on earlier runs. The statistics are memory-resident and advisory to the order alone (`docs/spec/parser-v2.md` I12, `docs/spec/plan-stats.md`) |
| **Executor** | Runs a step chain front to back, nested-looping into each next step; a walked join may build a statement-local map of its inner side (`docs/spec/join-inner-build.md`); `ORDER BY` is an output sort bounded by `sort_max_rows`; `GROUP BY` folds `COUNT`/`SUM`/`MIN`/`MAX`/`AVG` (`docs/spec/aggregate.md`). A `SELECT`'s outermost walk yields every 64 pages |
| **Clustered B+ tree** | Every user relation is a B+ tree clustered on its tuple key; a `HEAP` relation is one with an engine-added hidden key column no SQL names. A leaf is in key order by placement, and a split is one log record, replayed whole or not at all. System relations are catalog chains (`docs/spec/heap-and-tuple.md` §3.1b, §5) |
| **Tuples and var-heap** | Every row is fixed-length: a 64-bit Keystone word (`id:48 \| flags:8 \| reserved:8`) and a 20-byte MVCC header (`trx_id:48`, undo pointer, length, flags; no `xmax`). An `UPDATE` overwrites the row in place, its old image going to undo. A variable-width value longer than its cell spills to the var-heap, immutable per version (`docs/rules/rule-fixed-length-tuple.md`) |
| **Transactions and MVCC** | `READ COMMITTED` (default) and `REPEATABLE READ`; every reader is a commit-LSN snapshot over one instance read view; write conflicts are first-updater-wins. A lock family — units, modes, a wait-for graph and a deadlock detector, nothing persisted — makes a writer meeting an undecided holder wait for its decide. An explicit transaction is rolled back at `txn_lifetime_ceiling_ms` (`docs/spec/txn.md`) |
| **Secondary indexes** | Multi-column, covering and unique; `REINDEX INDEX`; a dropped index's pages reclaimed. No index-only scan (`docs/spec/index.md`) |
| **Cabin** | Key-function value sets with tuple keys and verified location hints, one store for the instance; the `CABIN AUTO` controller (`docs/spec/cabin.md`, `docs/spec/cabin-controller.md`) |
| **Waystone** | The trail store for opted shapes, per `(fetch_id, arg_hash)`, reached through `sys.patterns`; unlogged and discarded at every mount (`docs/spec/waystone-concpets.md`) |
| **Catalog and DDL** | DDL is transactional and logged, rolled back at mount when its transaction lost. Namespaces; `ALTER TABLE` rename and `ADD`/`DROP COLUMN` into reserved spare slots without rewriting a row; `DROP TABLE` reclaims its pages. `SHOW` is sugar over a `SELECT` on a `sys` view (`docs/spec/ddl-transactional.md`, `docs/spec/catalog.md`, `docs/spec/alter.md`, `docs/spec/sys-views.md`) |
| **Buffer pool** | One for the instance, its frames shared by every core. `buffer_pool_frames` is required and is a hard ceiling; CLOCK over slots, a full pool refused `ResourceExhausted`; writeback copies a page under its latch and enforces the WAL-before-data order (`docs/spec/eviction.md`, `docs/spec/page.md`) |
| **WAL** | One stream for the instance. Every authoritative mutation is logged; the free map (repaired by recovery), Waystone trails and `sys.access_stats` are not. Recovery at mount: analysis, redo, high-water, undo. Durability per transaction: `strict`, `group`, `relaxed`. Checkpoints per core, one at a time; segments below a durable redo start are recycled; a failed write or sync stops the log until a restart (`docs/spec/wal.md`) |
| **Storage** | One growable data file, `offset = page_id × 8 KiB`, growth in 64-page extents, a region-based free map, CRC32C page checksums. `mmap` is rejected for data and WAL (`docs/spec/page.md`) |
| **Backup and restore** | `BACKUP START` / `COPY` / `STOP`, or an atomic snapshot between START and STOP; each backup carries its own log range and a checksummed manifest. Restore is mount recovery from the backup's start; a WAL archive and point-in-time recovery to an LSN (`docs/spec/backup.md`) |
| **Scheduler** | A cooperative reactor pinned per core running C++20 stackless coroutines, with three scheduling groups (`foreground`, `maintenance`, `system`) on static shares and no preemption. A session with no transaction open may move to a less busy core at a transaction's head (`docs/spec/sched.md`, `docs/spec/crosscore.md`) |
| **Simulation harness** | A whole instance on crashable in-memory devices, driven through the command dispatcher, crashed at a seed-chosen operation, restarted and reconciled against an oracle (`sim/`, `scripts/sim.sh`) |

## Glossary

KDBS names its own concepts; the stone metaphor is deliberate — a *keystone* holds the structure up, a *waystone* guides the traveler without being the road.

| Term | Meaning |
|---|---|
| **Tuple key** | A row's 48-bit identity in its relation — never updated, never rebound to another committed row except across a `PURGE`. Named by an `INSERT` or issued by the engine. Not a *primary key*: it is an identity, not a business key |
| **Keystone** | The 64-bit word every tuple carries: `id:48 \| flags:8 \| reserved:8`. Everything that names a tuple names it by its Keystone id |
| **Step chain** | The compiled form of a native statement: an ordered list of steps — lookups, probes, ranges, scans, nested sub-chains for subqueries — run front to back |
| **Assertion** | A declared bound on a per-group aggregate, enforced on every write |
| **Transition** | A declared set of a column's legal states and state changes, enforced per row |
| **Cabin** | A value set for a key function: for each observed value, the tuple keys of the rows carrying it. Complete for each value it observed (a superset the reader filters) and authoritative for it; droppable per value without changing a result |
| **Waystone** | The advisory store of trails for opted shapes. Strictly a marker beside the road: droppable wholesale without changing any result |
| **Trail** | The recorded path of one instance of an opted shape — the tuple keys a previous execution found, with step tags and last-seen locations. A trail may replace a lookup, never a search |
| **Pattern / `fetch_id`** | A pattern is a statement's shape, fingerprinted at parse time with literals parameterized, so inline values and bind parameters converge. `fetch_id` is the shape without its select list; an instance is `(fetch_id, arg_hash)` |
| **`min_key`** | A page's immutable key lower bound, stamped at creation; no tuple below it ever lives on the page |
| **Epoch** | A per-page counter in the page header, bumped when a leaf division or an in-place leaf compaction moves its tuples. Trail entries and Cabin hints record the epoch they observed; a mismatch is a miss and the authoritative path runs |
| **Advisory** | Waystone's invariant class: consulted for where to look, never authoritative, droppable wholesale without changing a query result. A Cabin is not advisory — it is authoritative for its observed values — but shares the property that dropping it changes no result |
| **Durability class** | Per-transaction acknowledgment: `strict` (after the log's sync), `group` (the same durability point, batched), `relaxed` (after the record enters the log ring, a loss window bounded by the flush interval) |
| **KWP** | The wire protocol: length-prefixed binary frames, capability handshake, extended PARSE/BIND/EXECUTE, chunked streaming, per-transaction durability selection |

## What KDBS is not

Not a data warehouse, not a general-purpose SQL database and not a dialect-compatibility layer. No CTEs, no window functions, no `SERIALIZABLE` isolation, no `float`, no index-only scan, no cascading foreign keys. Nothing moves a row for statistics' sake: there is no tuple mover and no relayout. If the workload is analytical scans over wide history, use a column store; if it is high-rate transactional access to living data with rules the data must obey, KDBS is built for that.

## Status

Under active development. The design is specification-first: every subsystem has a spec in [`docs/spec/`](docs/spec/) stating what is built and how it behaves — start with [`docs/rules/rules.md`](docs/rules/rules.md), then `heap-and-tuple.md`, `page.md`, `wal.md`, `txn.md`, `protocol.md`, `parser-v2.md` and `waystone-concpets.md`. What is missing is in [`docs/inflight/known-gaps.md`](docs/inflight/known-gaps.md). The binaries, settings and source still carry the `kds` prefix.

```bash
scripts/build.sh                                # build into ./build
scripts/test.sh                                 # build and run the test suite
./build/kds_server --config kds.conf.sample     # data file kds.db, port 15432
python3 tools/ckdbs_cli.py                      # interactive SQL client
```
