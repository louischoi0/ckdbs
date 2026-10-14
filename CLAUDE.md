# CLAUDE.md — KDS Storage Engine

Working guide for AI development agents. **This file is a guideline and a
milestone map, not a detail store**: every subsystem's decisions live in
the `docs/` file that owns them, and this file links there.
`docs/spec/heap-and-tuple.md` is the authoritative design spec; when this
file and a spec conflict, the spec wins — flag the conflict instead of
guessing.

**Compacted 2026-09-02 ahead of the big-bang change to the architecture,
rules and constraints.** This file and every document under `docs/` now
state the current contract only: what is built and how it behaves. The
history of how each rule was reached, every measurement, and every plan
for unbuilt work left the tree with `bench/`, `instructions/` and
`docs/inflight/`; the last commit holding all of it is `1769487`, and the
last commit holding the uncompacted docs is `7f0193b`.

## What This Project Is

KDS is a **userspace OLTP-specialized database storage engine written in
C++**, targeting financial systems. Its purpose is fast and *correct* OLTP
through two engine-native mechanisms:

1. **Engine-driven physical page optimization** — runtime statistics
   reorganize where tuples physically live, not just how queries plan.
2. **Waystone** — a record of where a previous execution of a query pattern
   found its rows, so a repeated pattern can look instead of searching.

Feature scope is deliberately narrow; do not add general-purpose DBMS
features unasked. **KDS is userspace software** — no kernel-module code,
kernel headers, or kernel-only APIs.

## Milestones

One line per subsystem: what is built and enforcing, then the owning docs.
Read the owning spec before touching a subsystem — each carries rules that
are correctness statements, not style. What a subsystem does *not* do is
stated in its spec as a fact, and nowhere here as a plan.

| Subsystem | Built | Owning docs |
|---|---|---|
| Pages, semi-sorted heap, Keystone, fixed-length tuples, var-heap | Built. **An UPDATE carries every cell it does not assign since BN-R3**, so versions of one row share a spilled value, and a value may be released only once the newest version naming it is reclaimed; **a var-heap append starts at the chain's last tail since BN-R8** (`instructions/v3.0.0/workorder-bn-opt-read-update-path.md`) | `docs/spec/heap-and-tuple.md` (authoritative), `docs/rules/rule-fixed-length-tuple.md`, `docs/spec/page.md` |
| Heap relations | **Suspended (SUS-1, 2026-09-05), not removed.** No new one is created — `CREATE TABLE … HEAP` is refused `Unsupported` and the storage default is `BTREE`; every existing heap relation mounts and serves, and no heap code or cell is deleted | `instructions/v3.0.0/workorder-as-sus1-heap-suspended.md`, `docs/spec/heap-and-tuple.md` §3.1b |
| Multi-page free map | Region-based, ceiling 2^31 pages / 16 TiB, no superblock bump. The map is unlogged; recovery repairs it. **A page is freed since BF** by one primitive that discards its frame and clears its bit in one hold, and a freed id returns through an in-memory free list ahead of the cursor; a map flush carries a write barrier | `docs/spec/page.md` §5 |
| Clustered B+ tree | Built. An append-split publishes the separator before the sibling link. **A split finds and holds every parent it writes before it writes anything since AT-S16**: each is asked whether it still routes the key to the node below, a miss re-descends from the root, and a path that cannot be revalidated is refused `TxnConflict` with nothing written. **A root a level grows over is marked** (the page's `flags`, 0 on older pages), so a core whose memo names a stale root is refused rather than growing a second root over it. **A leaf is in key order by placement since BD-S2**: a row takes the slot its key sorts to; a full leaf whose incoming key is above all its keys appends a new leaf and moves nothing, otherwise the rightmost leaf divides at the insertion point (the median when the key sorts below every key or the moving half fits no empty leaf) and any other leaf at the median, **a full leaf with a retired slot is compacted in place before it splits since BH-S5** (`CompactLeafAndInsert`, one image), and **a split is one `BTREE_SPLIT` record**, replayed whole or not at all (superblock 20 refused 19; 21 since BF, 22 since BK, 23 since BR). **A stale descent's `TxnConflict` is re-run, not answered, since BA-S14**: a statement that wrote nothing before the clustered tree refused it is run again from the top after a yield, under its deadline; the index tree's refusals still reach the client | `docs/spec/heap-and-tuple.md` §5, `docs/spec/wal.md` |
| WAL | **One stream for the instance**: core 0 holds the log, every other core appends under its latch, and the superblock's `log_topology` is the word `Decode` refuses a volume on — **a pre-M0 volume does not mount** (D14), and no branch below the door asks the question. Every data mutation logged (heap, undo, var-heap, index, assertions, catalog/DDL) except the free map, whose `ALLOC`/`FREE` kinds stay reserved and refused - a dropped relation's page is freed with no record, once the durable redo start is past the drop (BF) - and the advisory Waystone classes invariant 8 exempts. A page carries no stream stamp and the record envelope no core byte (BP-S3); the retired `TXN_PREPARE` and `PAGE_HANDOFF` kinds are refused `Corruption` (BP-S2). Recovery runs at mount, once per log — analysis/redo/high-water/undo, a completion checkpoint, `SHOW META`'s recovery block; a torn catalog page refuses the mount. **Every core checkpoints its own active transactions, one run at a time for the instance** (AT-S8, `wal::CheckpointGate`), and publishes into the one superblock anchor itself under the superblock latch. **An insert logs under the holds that placed it since AT-S21** - its leaf and a split's parents, each index entry under the index tree's hold, each spill under its var-heap page's - and the paths that still log after release are named in `records-appended-after-their-page-is-released.md`. **The log is recycled since BC**: segments wholly below the redo start of an anchor a sync made durable are removed - detached under the stream latch, unlinked on core 0's writer thread - the fold never decreases, redo never starts below the scan start, and a mount opens the log from the anchor's segment; no archive gates it and no retention setting exists. **A failed log write fail-stops the log since BC-S4, and a failed sync since `fsync-fail-stop`** - a device write the stream issues, a device sync on either thread that syncs the log (both go through one gate, one sync at a time; the directory's fsync is serialized and sticky), or a ring-full refusal once its drains are spent: every later write is refused and no page is written back until a restart. **Above one core a commit leaves its reactor since BA-S7**: a `group` sync is the WAL writer's, which kicks the parked committer's core, and a `strict` commit stages its record, parks with its marker held and publishes after its sync (`CommitDeferred`), a failed sync aborting it. **An append's I/O is off the stream latch since BA-S12** (the next segment built ahead on the device's own thread, a flush's `pwrite` unlatched over a second buffer), **but a segment roll syncs the segment it leaves under the latch, and BQ kept it so** (`wal.md` §6), and **a transaction-id carve persists page 0 alone since BA-S13**, the next block carved ahead on the `system` tick; persisting page 0 alone hands the pool's writeback to the checkpoint. **The cadence's checkpoint is a `system` task since BI**: one bounded call per poll, its log waits deferred and parked on the writer, every data-file sync on the instance's one data-sync thread (sticky: a failed one stops the instance), the publish paced with the anchor's page written after a sync of every page, a step writing no map, and the gate rotating over cores; its `pwrite`s stay on the reactor (`known-gaps.md`) | `docs/spec/wal.md` §8, §11, `docs/spec/sched.md` §4 |
| Transactions & MVCC | Built. **Every reader is a commit-LSN snapshot over the instance read view** (AN-S2): a window of commit LSNs above a floor, one for the instance, read by every core; no view is a bound on trx ids. Live views and reader leases bound the purges instance-wide (`ReadHorizon()` is the instance's oldest live snapshot); undo pages recycle into the log's next growth. `SnapshotTooOld` is never raised; a previous run's undo pages are not reclaimed at mount. **An explicit transaction is rolled back at `txn_lifetime_ceiling_ms` (60 s, no off-switch) since 2026-10-12** - AN-R14's instance-wide half, each listener's sweep, DDL inside it included, a running statement deferred and never cut short, the client told at its next statement (`COMMIT` refused); autocommit is not bounded, and a KWP load session is exempt by the operator's ruling (`txn.md` §1). **The engine has a lock family since M2** (AO): units, modes, a wait-for graph and a deadlock detector, one table for the instance in 64 partitions at every core count (BP-S6), nothing of it persisted — a writer meeting an undecided holder waits for its decide instead of being refused - **on any core since AX-S1**, which made the in-flight test the instance's (one table per core in `InstanceVisibility`, 1,024 ids, `Begin` refused past it); a refused ask waits on the refusing unit's slot and is kicked at its release - on the ask's own unit since AX-S2b, on another unit since AY-S2, and on a holder between its decide and its release since AY-S3 (B6); what still polls has no slot behind it (`known-gaps.md` Locks), and a waiter that would close a cycle is refused naming deadlock. **A read takes a borrow too since AO-S6e-b** — a positioned read declares the relation it walks and the slice it has reached, held for the statement, and it is what a `DROP TABLE` waits for; since AT-S1 it is declared **at the bind** for every relation a statement names — `FROM`, `JOIN`, subquery, a write's own — with the slice the outermost walk's; the defence it gives is one-directional and `txn.md` §5 says why | `docs/spec/txn.md` §5, `docs/rules/rules.md` §3 |
| Transactional DDL | Built and durable: catalog writes logged, DDL under a real transaction, losers rolled back at mount. `CREATE TABLE` is atomic, isolated and consistent. **One name, one row on every core since AT-S17**: every name-taking write checks and writes under one hold of its catalog relation's root page, and the check refuses a name another core's open drop has freed (`catalog.md` CT7); **a relation's namespace since AT-S17b** - a create re-checks the namespace live, and `DROP NAMESPACE` holds the same page across a RESTRICT that reads `sys.objects`, so no relation lands in a dropped namespace; `CREATE INDEX`/`DROP INDEX` atomic and isolated on every core, both taking the relation `X` since AT-S5e - they wait for open writers and positioned readers, a writer's `IX` waits on them from any core, and a wait at the 1 s fault net is refused `TxnConflict` - and `DROP INDEX` admitted inside an explicit transaction; **`DROP TABLE` is atomic but not isolated** (§5a). **A DDL runs where the session is**: every core writes the catalog's pages under the page latch and places a new one; the delete-mark purge runs on core 0's 50 ms tick, reading the instance's count and gated on the settlement's progress (BP-S4). **No core is told of a DDL**: every core's catalog asks the instance's schema version word at its task boundaries, and the relation `IS` — not the word — is what keeps a stale parse from executing | `docs/spec/ddl-transactional.md`, `docs/spec/catalog.md` |
| Namespaces | A namespace is a name and **declares the affinity** of the relations created in it, which nothing reads (D18: a weight-0 hint with no consumer until AS-E); the `placement` key is refused by name. `CREATE NAMESPACE` / `DROP NAMESPACE` (RESTRICT), `SHOW NAMESPACES`, `ns.table` everywhere a relation is named. **A qualifier declares grouping, never identity** — relation names are instance-global | `docs/spec/namespace.md` (NS10) |
| Query language, parser, step chains, joins, subqueries | Built. **`OR`, `NOT` and parentheses in a WHERE since BU-S1**: a group holding either is one conjunct lowered to `Step::bool_residual`, three-valued to its root and never a key, bound, probe or propagation source; **a subquery predicate under it since BU-S3**, a leaf naming an `in_tree` sub-chain of its step, never hoisted; **`IN`/`NOT IN` value lists, `NOT BETWEEN`, `LIKE` and a literal on the left since BU-S2**, each a boolean conjunct (`instructions/v3.0.0/workorder-bu-basic-query-surface.md`). **A btree walk owns its slot loop and a scan kernel rejects on the cell's bytes before any decode since BM**, and **a rejected row reads its tuple with no `Status` and UPDATE's and DELETE's WHERE is checked once since BT** (`parser-v2.md` §7, `instructions/v3.0.0/workorder-bt-accept-tuple-row.md`). `WITH (...)` table options are refused `NotImplemented`; `SET DURABILITY` sets the session rung (`protocol.md` §9). **Two scalar functions since AP-S4**, `DATE(timestamp)` and `NOW()`, in a WHERE comparison only and on either side: a function conjunct is a residual kind of its own and never a key, bound, index, join, build or Cabin probe; the function catalog declares purity, and a statement's determinism class withholds a D2 statement's trail (`exec/functions.hpp`) | `docs/spec/parser-v2.md` I10, `docs/spec/types.md` §3.2a |
| `UPDATE ... SET` expressions | **Built since BJ.** A SET value is an expression: arithmetic, `||`, the comparisons and `AND`/`OR`/`NOT`/`IS NULL` as bool values, `CASE`, `COALESCE`/`NULLIF`/`GREATEST`/`LEAST`, `CAST` and `::`, the scalar functions of `types.md` §3.2b, date +/- days, and a scalar subquery run once before the walk. **Every expression is typed at compile with no implicit conversion** (a mistyped one is refused whether or not a row matches) and **evaluated per row against the before-image** (`SET a = b, b = a` swaps), checked in its own type at every step; a foreign-key column takes a constant expression, folded, and refuses one that reads a column. NULL propagates; a division by zero, an overflow and a NULL into a NOT NULL column are the row's refusals. Not built, refused by name: `IN`, `BETWEEN`, `LIKE`, a correlated subquery, boolean literals, timestamp arithmetic | `docs/spec/types.md` §3.2b, `instructions/v3.0.0/workorder-bj-expression-update.md` |
| `ORDER BY` | Any columns in a non-aggregated statement, each `ASC`/`DESC`, up to 8 keys, `sort_max_rows`-capped, a top-N heap under `LIMIT`. Refused over aggregated output | `docs/spec/parser-v2.md` |
| Aggregation (GROUP BY, COUNT/SUM/MIN/MAX/AVG) | Built | `docs/spec/aggregate.md` |
| Types | DATE, TIMESTAMP, DECIMAL, DECIMAL128, `char(N)`, `varchar(N)`; `float` refused. `varchar(N)`'s N is that column's `kds.inline_cell_width` — a width in `[16, 4096]`, not a length cap; a longer value spills. A bare `varchar` reads as the instance width | `docs/spec/types.md`, `docs/rules/rule-fixed-length-tuple.md` §4 |
| NULL | A tail bitmap sized to nullable columns; an all-`NOT NULL` relation keeps a byte-identical layout. **NOT NULL is the default and `NULL` is opt-in**; nullable index keys refused; NULLs sort largest; the bitmap is sole authority; WHERE is three-valued | `docs/spec/null.md` |
| Waystone | Recording and replay, **keyed by `{fetch_id, arg_hash}` since AP-S2** - the statement's shape without its select list, so two select lists over one fetch share a trail and a `sys.patterns` row, and the fingerprint version is 2 - and **on every core**: each records trails and registers patterns into the instance's one `sys.patterns`. `Catalog::ClaimPatternWaystoneRoot` is a claim, not a store - two cores building one pattern's directory keep the first and the loser adopts it. Trails are never retired or decayed | `docs/spec/waystone-concpets.md` |
| CREATE PATTERN | Withdrawn and removed; `$name` parameters refused everywhere; `SysPatternRow`'s `origin`/`flags` stay on disk | `docs/spec/create-pattern-user-defined-patterns-v1.md` (withdrawn) |
| Cabin | Built, **one store for the instance**, partitioned by `cabin_id` (AR1 §11's second shape): every core observes into it and serves from it, and a reader holds a `CabinSet` handle rather than a pointer into the table. A set is authoritative for the observed value whole. **§6's announce**: a build installs an unservable empty set first, so a write on any core during the walk is in it; what the announce cannot cover the banking gate asks of the instance (§6a), and a Cabin therefore builds rarely while anything is unresolved anywhere (`known-gaps.md`). Entry sets are memory-resident, its latches armed above one core only (BP-S3); `CABIN AUTO` is the instance's one controller's, whatever core a relation's statements run on (AT-S8, AT-S9) | `docs/spec/cabin.md` |
| Secondary indexes (multi-column, covering) | Built. No index-only scan, no `UNIQUE`. **An insert re-checks its leaf's coverage since AT-S15** (a stale descent restarts, bounded, then `TxnConflict`), on the premise that no index entry is removed; **a divide finds and holds every parent it writes before it writes since AT-S16**, and marks a root a level grows over - the clustered tree's walk. **A run of equal sort keys - one row whose covered column is updated ~600 times - promotes one separator twice** and refuses that row's later covered updates - and a named `INSERT` of a rolled-back key (since BD-S3) or a purged one (since BH-S3) - for good (`a-run-of-equal-index-sort-keys-promotes-one-separator-twice.md`); **a covering probe keeps a row if any of its entries passes the filter since AT-S22** (a stale entry walked first no longer drops it) | `docs/spec/index.md` |
| Statement-local inner build | The walked-join shape takes a `BuildKey` annotation and buckets rows into a statement-scoped map; `join_build_max_rows` is the cap, `0` the off-switch; `ANALYZE` reports `inner_built=`/`build_rows=`/`build_probes=`. A built join owns no `sys.patterns` row and feeds no Waystone trail | `docs/spec/join-inner-build.md` |
| Foreign keys | Declared and enforced **where the statement runs**: the forward check descends every parent there and the reverse check walks every child whole there. The forward check is hoisted, for deduplication and because the wait has to happen before any row is written. **D9(a) is built since AY-S5**: the forward check takes `IS` on the parent relation and `S` on the parent row **before it descends**, held to the decide, so a parent `DELETE` waits for every open child writer on whichever core either runs - the check-to-write window across cores is closed. A refused `S` is the wait; the self-referencing arm takes the same pair per row. **A parent `DELETE` meeting an undecided child whose writer holds no `S`** - a child `DELETE`, an `UPDATE` of other columns, or one moving the fk column off (whose earlier version the walk reads through undo, under a view minted after the parent row's `X`) - parks mid-walk on that writer and re-checks at its decide. The costs are stated in `foreign-keys.md` §2c: a parent `UPDATE` waits for open child writers, two child writers that then update their parent deadlock, each parent's `S` counts against `max_locks_per_txn`, and a DDL taking the parent's `X` waits for child writers' `IS`. **A failed check gives back the parent's `S` since AZ-S5**, whoever took it and with every other absent parent the statement resolved since AZ-R5's amendment of 2026-10-02, keeping the `IS`. **A Cabin set finds a child and, drained, clears a parent since AY-S6**: sound because the store is the instance's (AT-S7), D9(a)'s `S` keeps every writer that sets the fk off the set while it is read, and the banking gate declines a set banked over an open child insert; a heap child's failed hint or a moved-off undecided row gives the set up and the walk answers. CASCADE/SET NULL refused | `docs/spec/foreign-keys.md` |
| Assertions | Enforcing on every core, including immediately after a restart. **One registry for the instance since AT-S5d**: `CREATE ASSERTION` builds where its session is, every core's writes check and reserve into the one directory each assertion has, under the registry's latch, and an admission holds its contribution until its reservation is logged; core 0's mount resumes it. An assertion the registry cannot enforce refuses the relation's writes on every core. **A build holds the relation `X` since AT-S5e**, under a transaction of its own, so it waits for open writers and every writer arriving during it waits for the adoption; a wait at the 1 s fault net is refused `TxnConflict`. **A snapshot run is a base only once its own chunk count is met since AY-S8**: a partial or torn run is discarded, never adopted, and an assertion left with no whole run in the scan comes up unenforcing until `DROP` and `CREATE ASSERTION` (superblock v19, 20 since BD-S2, 21 since BF, 22 since BK, 23 since BR; a v18 volume, whose snapshots carry no count, does not mount). **A create is adopted with its base under one hold since AZ-S2**, before its row, so every core-0 checkpoint after it names the assertion | `docs/spec/assertion.md` §6.1, §7, §8.1 |
| Transitions | **Built since BV (v3.2.0).** `CREATE TRANSITION <name> ON <table>(<column>) ON INSERT (...) ON UPDATE (a => b, ...) [ON DELETE FROM (...)]`: a column's initial states, `old => new` edges and deletable states, checked per row from the row alone - no registry, no derived state, no record kind - so a path property (`FINALIZED` only after `CHECKED`) follows by induction and no history is stored. One row per Transition in `sys.transitions` (oid 146, root page 16, superblock 24), the declaration verbatim, re-parsed into `TableAccess` by the catalog's transition loader; the fence is the relation `X` and the memo check (`TransitionMemoFenceTest`). A violation is `TRANSITION_VIOLATION` (wire category 18); `PURGE` is refused on a governed relation, so its keys stay bound for life; `DROP TABLE` takes its Transitions in autocommit and is refused inside an explicit transaction while one stands | `docs/spec/transition.md`, `instructions/v3.2.0/workorder-bv-transition-constraints.md` |
| Access statistics | `SHOW ACCESS`. **Written where the statement ran**: `Catalog::RecordAccess` holds `sys.access_stats`' root page exclusive so the fold and the admission of a new shape are one act across cores | `docs/spec/heap-and-tuple.md` §7, `docs/spec/crosscore.md` CC13 |
| ALTER TABLE | Catalog-only: RENAME TO / RENAME COLUMN; assertions RESTRICT. **`ADD COLUMN` / `DROP COLUMN` since BW (v3.2.0, closed 2026-10-14)**, writing no row: `CREATE TABLE ... SPARE n` reserves 16-byte slots in the fixed row size, `ADD` places a nullable column (declared `NULL`, cell <= 16 bytes, `varchar(16)` for strings) in the lowest free slot and older rows read NULL (vacant slots are written NULL), `DROP` flags the column and leaves its bytes and slot; both under the relation `X`, transactional, readers not isolated; dependants RESTRICT; on a changed relation a positional `INSERT` is full width (a stale key-omitted positional row after a `DROP` is a `[quiet-wrong]` known gap); **an `INSERT` column list since BW-S6** names its columns, so no column change shifts it; no cleaning; type changes and an `ADD` without a free slot refused | `docs/spec/alter.md` AL11, `instructions/v3.2.0/workorder-bw-instant-add-drop-column.md` |
| DROP TABLE | Catalog-scoped: oid tombstoned and never reissued; fkeys/assertions RESTRICT, asked again under the `X` since BF. **Its pages are reclaimed since BF**: the tombstone carries the anchor and var-heap roots (superblock 21), and an owner-checked walk frees what they reach once the durable redo start is past the drop and no statement that could have bound it still runs - at the next mount for a tombstone it finds, within the run for a drop committed there (a statement epoch per core); a crash mid-reclaim is re-driven, and pages no root reaches stay leaked (`known-gaps.md`). **Since AO-S6e-b the drop takes the relation in `X`** — it waits for a positioned reader and for an open writer of the relation, the wait is bounded by the lock family's 1 s fault net, and under a continuous stream of readers the drop can be refused `TxnConflict` where it used to succeed. Atomic still, isolated still not | `docs/spec/drop-table.md` |
| Bulk insert | Multi-row VALUES, the full pipeline per row. **The KWP load stream carries every column, the tuple key first, and every storable type** (BI15, BI16): a NULL tuple key field takes an issued id and a value names one, per row, so one chunk mixes them as one T1 statement does. **The sorted-fill fast path is heap-gated, so SUS-1 makes it unreachable for every new relation** — `bulkinsert.md` T3-2 applies only to relations created before the suspension | `docs/spec/bulkinsert.md`, `instructions/v3.0.0/workorder-as-sus1-heap-suspended.md` |
| Physical optimizer, Part I: relayout | Built, shadow-only: every move is blocked by a named §6 gate, which `SHOW RELAYOUT` reports. **The planner builds plans only for heap relations, so SUS-1 leaves it dark on a fresh volume** — `SHOW RELAYOUT` can propose nothing for a relation created since the suspension | `docs/spec/physical-optimizer.md`, `instructions/v3.0.0/workorder-as-sus1-heap-suspended.md` |
| Physical optimizer, Part II: Cabin controller | Built, with `SHOW CABIN_OPTIMIZER`; managed state is memory-resident. **One surface for the instance since AT-S8** (`OptimizerSurface`): every core's `SET CABIN_OPTIMIZER` flips the one switch, every core feeds the one latched collector and reads the controller under its view latch | same, Part II |
| Buffer-pool eviction | Every accessor returns a pinned `PageRef`. **A freed page's frame is discarded, dirty or not, since BF**, and `EvictClean` is deleted. **`buffer_pool_frames` is required and the pool's hard ceiling since BE** (the operator's ruling of 2026-10-07): a missing key, `0`, or a value below the volume's floor (resident-class pages plus 256) is refused at boot, and resident frames never exceed it. **Frames are slots** in chunks that never move, with a free list and a CLOCK hand over slots; a miss or a creation reserves its slot first and reclaims in bounded batches at the cap, core 0's 50 ms tick keeps a free reserve, and a full pool refuses `ResourceExhausted` - only outside a mutation's no-refuse window, since a refusal after a page write would leave it half done (BE-Q11; recovery, rollback and the assertion loops drain instead). A `SELECT`'s outermost walk faults cold (BE-R5). **Writeback copies each page under its page latch and cleans only what the copy covered since AT-S8 step 1b** (a per-frame dirty generation) — before it, one core's checkpoint could persist half of another core's write and drop the mark of one that had not gone out. **One pool for the instance** — every other core borrows core 0's frame table, so `buffer_pool_frames` is an undivided total and a page faulted on one core is served from the frame another loaded; the writeback gate is a property of the one log | `docs/spec/eviction.md`, `docs/spec/page.md` §3, `instructions/v3.0.0/workorder-be-bounded-pool.md` |
| Cross-core execution | **Every statement runs on the core its session is on**, and a transaction is one core's, whole. **A session's core is re-decided at every transaction's head since BL-S3** (`server/load_placer.hpp`): above one core a session with no transaction open moves whole - socket, protocol state, the statement that asked - to the least-busy core when the gap passes a margin, its own share of its core is under the gap, the target has published recently and it has stayed half a second; a transaction never crosses, and every core's load is read from `sched::CoreLoad` (BL-S1); BL-S4's level partition is stopped for a ruling on the WAL coupling that defeats it, whose read-path half (`Catalog::TouchPattern`'s logged write) is deleted, and **BL is paused on the operator's word of 2026-10-12** (`workorder-bl-session-balancer-priority-affinity.md` §6). The lock family's fault net is 1 s (`lock_wait_fault_net_ms`). Every core carves its own transaction-id window and bumps a relation's row-id mark in place, and **the one cross-core path is write-then-kick through the wake registry** - best-effort, a skipped kick costing one idle block. **A `SELECT`'s outermost walk yields every 64 pages since BA-S15** (`server::SelectRun`, `sched.md` §3), holding its view, its borrow, its core's statement epoch at its word and a pin on the catalog memo; nothing else running yields, a tuple key descent included | `docs/spec/crosscore.md`, `docs/spec/sched.md` |
| Task representation | C++20 stackless coroutines | `docs/spec/sched.md` §3 |
| Keystone id issue-once contract | A tuple key `UPDATE` is refused at compile; `sys.tables.next_id`'s bump logs and replays with every catalog write. **K1 has one named exception since BH-S3, `PURGE`** (`heap-and-tuple.md` §4.1c): `PURGE FROM t WHERE <tuple key comparisons>` - autocommit, btree, admin - retires keyless every committed delete-marked row of its window once `ResolvedForEveryReader` passes its deleter, logging `SLOT_RETIRE` and `VARHEAP_RELEASE` at `kNoTxnId` (no new record kind and no superblock bump), taking the relation's `IX` and no row unit, polling older readers and undecided writers for `kPurgeHorizonWaitNs` (1 s) under a fresh owned transaction per attempt; a named `INSERT` then places the key. Its horizon gate is `[quiet-wrong]`: every read view that can see a superseded user row must stay registered with the horizon | `docs/rules/keystoneid-invariant.md`, `docs/rules/keystoneid-k0-findings.md`, `docs/spec/heap-and-tuple.md` §4.1c |
| Caller-supplied tuple key | Per row, an `INSERT` names the tuple key or the engine issues one. **A btree leaf places a row at the slot its key sorts to since BD-S2** (BD-R1, BD-R2), so every keyed slot of every leaf ascends whatever order keys arrive in, at every core count, and **a named key on a btree is refused because of its tuple key for exactly two reasons since BD-S3** (BD-R5): a slot keyed `k` - live or delete-marked - is `AlreadyExists`, and a key outside `[1, 2^48 - 1]` is `OutOfRange` (48 bits since BK), each naming the tuple key token's byte - the one known exception being the equal-sort-keys index bug's `AlreadyExists`, reachable by naming a rolled-back or purged key again (`a-run-of-equal-index-sort-keys-promotes-one-separator-twice.md`); after `tuple key = 100`, `tuple key = 99` is placed. A rolled-back key is free, a committed key stays bound until a `PURGE` frees it (its delete-marked row is the tombstone, BD-R4; `PURGE` retires it, BH), and a named key meeting an undecided insert of the same key waits for its decide (BD-Q9). `sys.tables.next_id` is the issue cursor for omitted keys and K4's budget, and gates nothing on a btree (BD-R7). **A heap is untouched** (BD-Q4 (a)): it refuses a named key below its mark `OutOfRange` and fixes each id under its tail's hold (BB-R7). There is no key mode and no key-order state (BB-R10); the `sys.tables` byte stays, written 0 and read by nothing | `docs/spec/heap-and-tuple.md` §4.1, `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`, `instructions/v3.0.0/workorder-bk-keystone-48.md` |
| Wire protocol KWP/1 | **Every core accepts on the default port** (SO_REUSEPORT above one core; a session starts where it was accepted, under one TLS/SCRAM/limits setup, and may move between transactions (BL-S3); `peer_listeners` is refused by name). **Where the platform refuses SO_REUSEPORT (D19's fallback, AT-S10c)** core 0 binds alone, accepts every connection and hands each round-robin to a core by an inbox and a kick, and the session starts where it was handed (`include/kds/server/connection_handoff.hpp`). The default port speaks length-prefixed binary frames: handshake with capability intersection, SCRAM, PARSE/BIND/EXECUTE over statement and portal handles, typed row batches, portal suspension, transaction and durability frames, a pinned error registry. `kMaxFrame` 16 MiB, 64 KiB batch target, 60 s portal-idle timeout, 64 statements and 64 portals per session; `max_rows` bounds delivery, not execution; `C_DESCRIBE` of an unexecuted statement is refused. The newline protocol is `debug_text_port`'s loopback debug surface, off unless configured; `STOP` is reachable only there. TLS 1.3 and the statement-class roles are off by default; the `COMPRESSION` bit is reserved and never offered. **A pipelined statement's reply leaves in one write since BN-R7** (`workorder-bn-opt-read-update-path.md`). **`C_CANCEL` is answered since BA-S15** where a listener has an unguessable identity source and the instance's `CancelRegistry`, and only there is `CANCEL` offered: the flag is taken at a walking `SELECT`'s next slice boundary, ending it `CANCELLED` and failing its transaction, or else at the next frame | `docs/spec/protocol.md`, `docs/spec/client-manual.md` |
| Backup and restore | **Built since BR.** Two paths over one restore: **B**, `BACKUP START` / `COPY` / `STOP`, the server copying its data file through a device write fence (no page torn by its own write); **D**, an atomic snapshot between START and STOP. Either backup carries its own log `[start, stop]` and a checksummed manifest. **Restore is mount recovery from the backup's start, never the copied anchor, and does not open short of the stop** (superblock 23: anchor slot 0 rewritten, `restore_backup_id`); unlogged state (Waystone, `sys.access_stats`) is discarded at the restore's mount. The start is `D` pinned against the recycler and the BF reclaim; a WAL archive (`archive_dir` / `archive_command`) holds recycling and fails closed; PITR to an LSN; every restore takes a new lineage. `kds_server --restore-from`, `tools/kds_backup.py`. BR also fixed a rolled-back DDL's unlogged catalog compensation, which a crash replayed into a lost table | `docs/spec/backup.md`, `manual/server/backup-restore.md` |
| Simulation harness | A whole instance on crashable in-memory devices driven through `CommandDispatcher`, crashed at a seed-chosen op, restarted and reconciled against an oracle; `scripts/sim.sh` runs the corpus | `sim/`, `scripts/sim.sh` |
| Core placement on disk | **Nothing names a core on disk**: `sys.tables.owner_core` is a reserved word - written 0, never read - so the core count is not pinned and a mount at another `cores` records it. A relation is one structure headed by `desc_page_id`; `range_size_ids` is refused by name, and the superblock (25 since BW) refuses every older version | `docs/spec/crosscore.md` (CC7-CC10), `docs/spec/heap-and-tuple.md` §4.1a |
| Observability | Not a subsystem. Two `SHOW META` blocks: per-scheduling-group accounting against reactor wall time, the idle policy's counters | `docs/spec/sched.md` §4 |
| User manual | `manual/` — SQL surface, verified against code | `manual/sql/sql.md` |

## How `docs/` is organized

**Three trees left on 2026-09-02, ahead of the big-bang change: `bench/`,
`instructions/` and `docs/inflight/`.** The last commit holding them is
`1769487`; `git show 1769487:<path>` retrieves any file, and each directory
keeps a `README.md` saying what was there and why it went. **A citation to
one of those paths — in this file, a spec, the manual, a test or a source
comment — resolves against that commit *unless the path exists in the
working tree*.** All three have since reopened, so the qualifier is
load-bearing: `docs/inflight/in-progress/` is not a reopened bucket and a
source comment still cites it. `instructions/v3.0.0/` reopened
on 2026-09-02 with the change's own instructions — AR0 and one work order
per milestone (`instructions/v3.0.0/index.md`). **`bench/` reopened on
2026-09-03** with AL-R8 ratified: `bench/v3.0.0/` is a fresh series and
carries no delta against any v2.x number, and `bench/README.md` states the
five rules a run is invalid without. **`docs/inflight/` reopened on
2026-09-03 on the operator's word**, with a narrower job than it had —
`instructions/` took the plans, so this holds only what a plan is not.
**`docs/conceptnotes/` opened on 2026-09-08 on the operator's word**
(as `docs/blueprint/`, renamed 2026-09-26), with CN-1: concept notes,
which are neither built, nor a rule, nor a gap in what is built.
**`docs/pending/` opened on 2026-10-08 on the operator's word**: questions
the operator raised and deferred. Five buckets under `docs/`, one rule each:

- **`docs/spec/`** — what is confirmed and implemented. The authoritative
  specifications; when this file and a spec conflict, the spec wins.
- **`docs/rules/`** — concepts and constraints that hold across the whole
  codebase: `rules.md`, the fixed-length-tuple rule, the Keystone id
  invariant and its findings.
- **`docs/inflight/`** — `known-gaps.md` (what is missing, what the code
  does differently from a spec, what the suite does not cover), `bugs/`
  (a defect found and **not yet fixed** — one fixed in the session that
  found it gets none) and `blocked/`. Every entry names the commit it was
  verified at; an entry older than its subsystem's last change is a
  statement about an engine that no longer exists. Open work orders are
  **not** here — they are `instructions/<version>/`.
- **`docs/conceptnotes/`** — concept notes: an idea with its reasoning, its
  boundaries and the decisions it leaves open, recorded before anything is
  built and possibly before anything ever is. A note opens no stage and
  licenses no code; when one is taken up, the work order cites it and
  becomes the authority. Its own `README.md` carries the rule and the
  header every note must have.
- **`docs/pending/`** — a question the operator raised and deferred for
  later discussion: what prompted it, the tree as read, the options. It
  decides nothing; on the operator's ruling the entry is deleted and the
  ruling goes to the document that owns the question. Its own `README.md`
  carries the rule.

A closed workplan is deleted, not archived: the spec that owns the
subsystem carries everything durable. The plans closed before 2026-08-26
are at `925f483` (`git show 925f483:docs/workplan-index.md` lists them);
everything open after that was under `docs/inflight/` at `1769487`. A
citation to a `docs/workplan-*.md`, or to the superseded parser documents
`docs/spec/parser-v2.md` replaced, points at one of those two commits.

Numbering collides across docs (three `P`-schemes, two `R1`s): **cite the
file, never the bare number.**

## Hard Invariants — never violate, never "temporarily" bypass

Numbered to match `docs/spec/heap-and-tuple.md` §8.

1. 8192-byte pages; `uint32_t` page ids; `0xFFFFFFFF` reserved as invalid.
2. `min_key` is immutable after page creation, until the page is freed; a reused id is a new creation.
3. No tuple with `id < min_key(page)` in that page, ever — including by relayout — for the life of the page.
4. Tuples within a heap page are unordered. A btree leaf is sorted by placement: every keyed slot ascends by Keystone id, live and delete-marked alike, because a row takes the slot its key sorts to (BD-R1).
5. The Keystone column is exactly `id:48 | flags:8 | reserved:8`.
6. The Keystone word is read/written as an atomic `uint64_t` (CAS for updates); on-disk encodings use **explicit shift/mask helpers only — compiler bitfields are forbidden** for any persisted format.
7. Ids stored outside the tuple header are zero-extended `uint64_t`; the upper 16 bits are always 0.
8. Waystone is advisory: deleting it wholesale may cost performance but must never change a query result.
9. Waystone is never **authoritative**: a reader may consult it for *where to look* only, with the miss/Keystone/MVCC/fall-through discipline `docs/spec/heap-and-tuple.md` §8 spells out. It chooses where to look, never what is visible.
10. No single canonical in-memory tuple; consistency comes from page pin and latch discipline.
11. **Every** relation's tuple key is a unique 48-bit id, carried only by the Keystone word, never rebound except by `PURGE`, **never updatable**. First column must be integer-typed. Where the id comes from is a per-**row** fact — the `INSERT` names it or omits it — and there is no key mode. A Keystone id is bound to at most one committed tuple in the lifetime of a relation, except across a `PURGE` that freed it (K1, `docs/spec/heap-and-tuple.md` §4.1c). `sys.tables.next_id` is a high-water mark above every id placed: an omitted key is the mark, so issued keys are a sequence in issue order however many cores insert; an issued id can meet a key a concurrent statement named before the id was placed, and the placement's duplicate check then burns it and draws again, bounded at `kMaxIssueRounds`, so on a btree every key's uniqueness is the descent's (`docs/spec/heap-and-tuple.md` §4.1, §4.1a). Named keys carry no order: on a btree a named key's uniqueness is the descent's, and a leaf's slot order is its key order by placement; a heap admits a named key only at or above its mark. Monotonicity of issued keys is per-relation and per-history, never engine-wide.
12. The tuple MVCC header is exactly `trx_id:48 | undo_ptr | data_len | flags` = 20 bytes. There is no `xmax`.
13. **Every tuple is fixed-length**: row size is a schema constant, variable-width values occupy one tagged cell of `kds.inline_cell_width` bytes — the instance's, or the column's own where `varchar(N)` declared one (the width is per *column*, never per row). A disagreeing length is `Corruption`, never interpreted.
14. **Var-heap values are immutable per version** and `kVarHeap` pages are never relocated. Authoritative data — advisory rules do not apply to it.

## Working Rules

- Fresh codebase: idiomatic modern C++, not kernel-style C. RAII for every
  resource; no raw `new`/`delete` in engine logic; fixed-width integer types
  and `static_assert`ed layouts for on-disk structs; named `constexpr` for
  every size/offset with the derivation in a comment. Full rules:
  `docs/rules/rules.md`.
- Document the lock/atomic protocol at the top of each subsystem file.
- Tests accompany every subsystem; the contract suites that compare
  configurations byte-for-byte are **waystone, index, cabin and inner-build**
  — keep them green and extend them with the feature. The other contract
  suites are a different shape, pinning claims rather than configurations:
  types and aggregate one test per numbered spec item, scram and tls against
  their RFC vectors. **Assertion has no contract suite** —
  `tests/assertion_*_test.cpp` are what carry its checks.
- **Never push what you have not built — unless the operator says to.**
  `scripts/githooks/pre-push` is the gate; enable it once per clone with
  `git config core.hooksPath scripts/githooks`. On CLA's own initiative the
  gate runs, and `--no-verify` needs a reason CLA can state. When the
  operator asks for a push with the tests skipped, or for a force push, CLA
  does it — no re-litigating, no waiting for a suite the operator has
  waived — and still owes two things: **say plainly in the reply what was
  skipped** (the hook, the suite, or both) and **never report an unrun
  suite as a pass**. The Session Workflow's step 4 gate below is a gate on
  CLA proceeding unasked, never on the operator. **A suite already run is
  not run again by the hook** (operator, 2026-10-12: "if the tests have
  already run, push with no-verify to shorten the push"; widened
  2026-10-13: "from now on, if the whole test suite has been run by a
  review or a test, push with the --no-verify option, to cut development
  time and cost"): every push CLA makes goes out with `--no-verify` when
  the full suite - every test the hook runs, `scripts/test.sh`'s `ctest`
  less the labels it excludes, and not a narrower filter of cells (operator,
  2026-10-13: "treat the full suite as the range the hook runs"); a cell
  under an excluded label, such as one BS adds (`workorder-bs-test-hardening.md`
  §6.1), is not part of it - ran green
  on the build inputs being pushed, whoever ran it: a step's own test
  run, a review's, or a `ck-tester` run. Anything that changes a build
  input after that run voids it and the hook runs: a review's fix applied
  since, or a step 4 merge that brought source, tests, CMake files or
  `scripts/`. A merge that brought no build input - `docs/`, `bench/`,
  `instructions/`, `manual/`, `.claude/`, this file - does not. The reply
  names the hook as skipped and the commit the suite ran at, with its
  pass count.
- **Measure in `build-release`, never `./build` (Debug)** — Debug has
  reported the wrong sign. Per-statement fixed costs: server CPU,
  interleaved A/B. **Re-measure a premise before building the fix.**
- **A running measurement is watched every minute** (operator,
  2026-10-12: "once a benchmark or measurement is started, check its
  state every minute, and if it has fallen into an orphaned state, resume
  the measurement"). Every benchmark or measurement CLA starts gets a
  60 s watchdog - polled by a file it writes, never by a process pattern
  the watcher's own argv can match - and one found orphaned (stalled,
  its driver gone, its parent session gone) is resumed at once, not
  reported and left.
- **A measurement has a time budget** (operator, 2026-10-13: "unless the
  operator directs otherwise, a measurement runs within 20 minutes per
  stage and around 1 hour per milestone; keep this rule until directed
  otherwise"). A stage's measurement (a census, a premise check) is
  planned and run inside 20 minutes, and a milestone's close measurement
  inside about an hour, building included; a design that cannot fit is cut
  to what fits - fewer runs, fewer cells - and the results file says what
  was cut. Only the operator's word for that measurement lifts the budget.
- Every refusal carries the byte position of the offending token, and
  "understood and declined" is **two** codes: `Unsupported` is what the
  architecture cannot admit, `NotImplemented` is what nobody built, and
  each reaches a client as its own token. `InvalidArgument` stays "simply
  wrong". The test between the pair is in `include/kds/base/status.hpp`
  and `docs/spec/protocol.md` §11. Truthfulness beats convenience: never
  accept a spelling and enforce something other than what was written.
- **Every document is synced in English** (operator, 2026-10-07), whatever
  language the instruction came in: specs, work orders, rules, `docs/inflight/`
  entries, bench files, commit messages and code comments. A Korean
  instruction still gets a Korean reply. **A quotation is translated too**
  (operator, 2026-10-09): the operator's words are recorded in English, in
  quotation marks, and no Korean text enters the tree.
- Nothing new is reserved lightly: keywords hash as identifiers, and
  `kFingerprintVersion` moves only per `fingerprint.hpp`'s bump rule (the
  golden corpus pins it).
- Never add a second name for a quantity an existing setting expresses;
  re-scope the existing one.

## Version Management

The version of record is an **annotated git tag on `main`**. **The current
version of record is `v3.1.0`**, the merge of BU-S2 into `main`, operator-named
on 2026-10-13 ("we are in V3 now"); read its tag message.

- **The operator names the version; CLA may move only `z`.** A version is
  set by the operator saying it — *"it is version x.y.z"* — and nothing
  else. No number is ever inferred from a merged milestone, a green suite,
  or the size of a diff. A tag CLA creates on its own initiative differs
  from the last operator-named version in `z` alone, and CLA says plainly
  in the reply that it moved it. Pushing any tag waits for the word.
- **A tag message is a durability claim, so it carries what bounds it**:
  what is built and enforcing, then the gaps that limit it. A tag carrying
  only the first half overstates the engine.
- **Every measurement names its version, and `git describe --tags` is
  how** — `v2.0.0-37-gaa3e26c`. Binding on every results file and on any
  reply that quotes a number. A results file lives at
  `bench/<version>/<benchmark>-<describe>.md`; `bench/README.md` carries
  the rules, and a v3 number is never set beside a v2.x one.
- **Nothing is back-filled.** A result measured before a version existed
  keeps its bare commit id.

## Session Workflow — the loop every task runs

Four steps. The first three run unprompted; the fourth stops.

**1. Work in a worktree, named for the work.** Before touching the tree,
`EnterWorktree` with a name taken from the task, not from the date or a
counter — `rc04-high-water`, `fix-varheap-checksum`. Read-only questions
and pure doc lookups need no worktree.

**2. Every completed step gets a `critics-developer` review.** Per step,
not once at the end. Correctness first, then duplication,
over-engineering, bloat. Apply what it finds; say plainly which findings
were rejected and why — a review whose findings are all silently accepted
was not read.

**3. Every feature runs the full test suite; the overhead is measured once
per milestone.** The suite gates every step. The interleaved A/B overhead
measurement (`ck-tester`, `build-release`) runs **per milestone, not per
stage** (operator decision, 2026-09-30; `instructions/v3.0.0/raft-marks-2026-09-30.md`
§8): at the milestone's close, over its whole code change - the commit it
opened from against the commit that closes it. A stage lands carrying
"overhead not measured; measured at the milestone's close" as a stated
fact, never an implied pass, and a milestone does not close with it
unmeasured unless the report says so. **If the environment cannot build or run,
say so in the report** — an unrun measurement is never reported as a
pass, and a suite that was not executed is stated as "not executed".

**4. Land — sync unprompted, push only on the word.** Sync and resolve on
the work branch, never on `main`:

```
git fetch origin
git merge origin/main        # on the work branch; resolve conflicts here
```

Then **stop** and report: what the review found, what the suite showed,
what the merge resolved. On the go-ahead, and not before:

```
git checkout main
git merge --no-ff <branch>
git push origin main         # --no-verify when the suite already ran green on this tree
```

A failed review, a measured regression, or an unrun test suite stops the
chain before the merge. Say which, and do not offer the push as though
the gate had passed. This stops CLA, not the operator: an operator who
asks for the push anyway gets it, with what was skipped named in the reply
and never reported as a pass.

**Workflow mode** is an outer loop — `intermediary-agent` pulling tasks
from `cws` and `reporter-agent` syncing outcomes back — that wraps this
four-step process unmodified; it never relaxes a gate, and it activates
only on an explicit request. `docs/rules/rule-workflow-mode.md` is the
full rule.

## Every claim carries its worktree and commit — in the middle of the text

Not a header, not a footer, not a preamble: **the worktree name and the
short commit id go inside the sentence that makes the claim.** Reports
outlive the tree they were written against, and a finding quoted a week
later must still say what it was true of:

> On `rc04-high-water` at `f837e6a`, redo's `CreateAt` already covers the
> PAGE_INIT case, so the gap is the checkpoint's recLSN-of-zero entry.

- **When a step moves the commit, the next claim carries the new id.** One
  reply may name three, and should.
- **With no worktree in use, say so by name** — *"in the main checkout on
  `feat-wal-recovery` at `f837e6a`"*. Silence reads as "a worktree", which
  the Session Workflow requires and which would then be a false report of
  compliance.
- **A measurement names the version and the commit it was measured at** —
  `git describe --tags`, so `v2.0.0-37-gaa3e26c` rather than `aa3e26c`
  alone.
- **A completion report opens with the time, in UTC** (operator,
  2026-10-07): its first line is `date -u '+%Y-%m-%d %H:%M UTC'`'s output,
  read from the clock at the moment of writing. The commit says what tree a
  claim was true of; the time says when the report was made of it.

## Operator abbreviations

The operator uses shorthand when talking to CLA (operator, 2026-10-12).
**This table is the dictionary, and CLA keeps it**: an abbreviation the
operator defines is added here in the session it is defined, and one
missing from it is asked about, never guessed. An abbreviation is a
conversational token, not a document term — a document spells the word
out.

| Token | Meaning |
|---|---|
| `*M` | milestone (the asterisk is part of the token: `M0`, `M2` in AR0 are milestone names, not this) |
| `&*M` | the milestone currently in progress in this session |
| `*T` | a task inside a `*M` |
| `*I` | issue |
| `&*I` | the issue currently in progress in this session |
| `&*S` | the stage currently in progress in this session (operator, 2026-10-13) |
| `*B` | bug |
| `*P` | progress gate: what must be resolved before the named work can proceed. "Resolving the thread-contamination `*I` is the `*P` of the BK `*M`" means the BK milestone cannot be closed until that issue is resolved |
| `:>>` | the `*P` relation written as an arrow: `A :>> B` means A must be met before B can proceed (A is B's `*P`) |
| `<::` | grounded in: `A <:: B` means A rests on B - B is the ground, A the conclusion ("judged on the basis of B, A"; "A, because B"; "following B, A") |
| `->>` | causes: `A ->> B` means A brings about B - A triggers B, or B is an effect A reaches ("A brings about B"; "A induces B"; "A has a knock-on effect on B") |
| `{…}` | a set of items: `{A, B, C} :>> D` means A, B and C must all be met before D can proceed |
| `(…)` | any of a set of items: `(A, B, C) :>> D` means any one of A, B or C being met lets D proceed |
| `...[…]` | enumerate: `...[X]` asks CLA to list X item by item ("enumerate X"), e.g. `...[1 *I's options]` lists every option for issue 1 |
| `#n` | ordinal: the n-th item of what it qualifies - `#1 option` after an issue token is the first option (proposal, choice) CLA proposed for that issue |
| `$!` | a choice: when CLA has proposed options `a`, `b` and `c`, the operator's `$!a` chooses option `a`, and the decision is marked as option `a` |
| `%%%` | a request for the current progress: "tell me the current progress" (operator, 2026-10-13) - of the work running in this session, read off its files and logs at the moment of asking |

**A relation symbol mirrored reverses its direction** (operator,
2026-10-12): written back to front with each `<` and `>` swapped, a
relation reads from its other term and means the same thing, so either
term may stand on the left. `B <<: A` is `A :>> B` (B cannot proceed until
A is met), `B ::> A` is `A <:: B` (B is the ground of A), and `B <<- A` is `A ->> B`
(B is brought about by A). A set
`{…}` or `(…)` may stand on either side of a mirrored relation.

## Open Decisions — DO NOT assume or silently pick

None are recorded here or in the specs: the lists that stood here and in
each spec's open-decisions section were plans against the engine the
big-bang change replaces, and they went with `docs/inflight/` (at
`1769487`, and in this file at `7f0193b`). **The open list for v3 is
AR0's D1–D16**, in `instructions/v3.0.0/ar0-architecture-revision.md` §5,
each with CLA's proposal beside it and each unratified but what AR0-M
marks (D1(b), D2(a), D7, D8, D9(a), D11, D15; D7's last gate lifted in the
following letter, AT-0 item 6), what `raft-marks-2026-09-28.md` re-marks
(D1's levels, D8 as revised, D9(a) as a design) and D3 (AR0-5-R) — and three
of them (D7, D8, D9) are marked `[quiet-wrong]` by AR0 itself, meaning a
wrong choice converts a refusal into a wrong answer. `docs/inflight/known-gaps.md`
tracks what is *missing*, which is a different list and does not decide
anything. The rule stands whatever the lists say: **when work touches a
matter the owning spec does not decide, stop
and ask, or implement behind an interface that keeps every option
viable.** Never infer a decision from a green suite, a merged diff, or
the absence of a rule. The C++ standard, toolchain, build system and test
framework are proposed, never decided, by CLA.

## Maintenance rule for this file

When a decision lands: update the spec that owns it and flip the
milestone row — **do not** write the detail into this file. A finding
with no owning doc gets one (or a section in the nearest spec), never a
paragraph here. This file states what is; a plan for what will be lives
in the operator's instructions, and a plan's history lives in git.
