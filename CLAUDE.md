# CLAUDE.md — KDS Storage Engine

Working guide for AI development agents. **This file is a guideline and a
milestone map, not a detail store**: every subsystem's decisions live in
the `docs/` file that owns them, and this file links there.
`docs/spec/heap-and-tuple.md` is the authoritative design spec; when this
file and a spec conflict, the spec wins — flag the conflict instead of
guessing.

**This file and every document under `docs/` state the current contract
only**: what is built and how it behaves. History, measurements and plans
are in `instructions/`, `bench/` and git:

- the trees as they stood before the 2026-09-02 compaction: `1769487`;
- the uncompacted docs: `7f0193b`;
- this file before its 2026-11-18 compaction, with each milestone row's
  history: `git show abe870fc:CLAUDE.md`.

## What This Project Is

KDS is a **userspace OLTP-specialized database storage engine written in
C++**, targeting financial systems. Its purpose is fast and *correct* OLTP,
where correctness is declared and the engine enforces it:

1. **Declared invariants, enforced on every write** — assertions,
   transitions, foreign keys (D9(a)), unique indexes, and a tuple identity
   that is never rebound (Keystone).
2. **Acceleration that can be dropped without changing a result** — Cabin
   value sets, authoritative for the values they observed and droppable
   per value or wholesale (`cabin.md` C1); and Waystone trails for the
   shapes an operator opts in, which are advisory (invariants 8 and 9).

This purpose was settled as CF-Q6 (b)
(`instructions/v3.2.0/raft-marks-2026-10-21.md` §10).

Feature scope is deliberately narrow; do not add general-purpose DBMS
features unasked. **KDS is userspace software** — no kernel-module code,
kernel headers, or kernel-only APIs.

## Milestones

One line per subsystem: what is built and enforcing, then the owning docs.
Read the owning spec before touching a subsystem — each carries rules that
are correctness statements, not style. What a subsystem does *not* do is
stated in its spec as a fact, and nowhere here as a plan. A row carries no
history: which stage built a behaviour, the superblock it moved and what it
replaced are in the owning docs and the work orders named beside them.

| Subsystem | Built | Owning docs |
|---|---|---|
| Pages, heap pages, Keystone, fixed-length tuples, var-heap | Built. An `UPDATE` carries every cell it does not assign, so versions of one row share a spilled value, and a value may be released only once the newest version naming it is reclaimed. A var-heap append starts at the chain's last tail. The var-heap, loser-undo, leaf, version-chain and Bound Cabin walks are bounded by a cycle check, not a length (`base/cycle_guard.hpp`) | `docs/spec/heap-and-tuple.md` (authoritative), `docs/rules/rule-fixed-length-tuple.md`, `docs/spec/page.md`, `instructions/v3.0.0/workorder-bn-opt-read-update-path.md` |
| Heap relations | Built (DN, superblock 39): **a heap is `kHeapData` pages in no key order, each row at a rowid (`page << 16 \| slot`) it keeps for life, found by key only through its Keystone index**, every index entry carrying the rowid and checked at the slot (DN-R3); a full scan reads the pages. A single `int64 PRIMARY KEY` declared first is the Keystone; otherwise the engine adds a hidden one (`keyidentity` reads either, `*` shows neither hidden), and another declared primary key is a unique index beside it. **A slot is reused only once no entry of any index names it** - an unasked release on core 0's tick (DD-S4's driver). A hidden Keystone refuses a named key below its mark; a declared one is an FK parent and is `PURGE`d by key. Only system relations are catalog chains | `docs/spec/heap-and-tuple.md` §3.1b, `instructions/v3.2.0/workorder-dn-heap-rowid.md` |
| Multi-page free map | Region-based, ceiling 2^31 pages / 16 TiB. The map is unlogged; recovery repairs it. One primitive frees a page: it discards the frame and clears the bit in one hold, and a freed id returns through an in-memory free list ahead of the cursor. A map flush carries a write barrier | `docs/spec/page.md` §5 |
| Clustered B+ tree | Built. **A leaf is in key order by placement**: a row takes the slot its key sorts to. An append-split publishes the separator before the sibling link. **A split finds and holds every parent it writes before it writes anything**, is one `BTREE_SPLIT` record replayed whole or not at all, and a path that cannot be revalidated is refused `TxnConflict` with nothing written. A full leaf with a retired slot is compacted in place before it splits. **A write descent takes its leaf exclusive at once below a level-1 node, and a leaf that no longer covers its key hands the descent its right sibling** (DK: up to 8 hops, then the restart; never a write from the named root). A root a level grows over is marked, so a core whose memo names a stale root is refused. **A stale descent's `TxnConflict` is re-run, not answered**, for a statement that wrote nothing before it, after a yield and under its deadline; the index tree's refusals still reach the client | `docs/spec/heap-and-tuple.md` §5, `docs/spec/wal.md` |
| WAL | **One stream for the instance**: core 0 holds the log and every other core appends under its latch. The superblock's `log_topology` is the word `Decode` refuses a volume on — **a pre-M0 volume does not mount** (D14), and no branch below the door asks the question. Every data mutation is logged except the free map — its `ALLOC`/`FREE` kinds reserved and refused; a dropped relation's page is freed with no record once the durable redo start is past the drop — and the advisory Waystone classes invariant 8 exempts. A page carries no stream stamp and the record envelope no core byte; `TXN_PREPARE` and `PAGE_HANDOFF` are refused `Corruption`. **A mount's log open zeroes an unsealed tail past its end and syncs what it scanned before claiming it durable** (`wal.md` §4.2). Recovery runs at mount, once per log (analysis, redo, high-water, undo, a completion checkpoint). **A torn page of a logged class is healed** (DB): a frame owes an image from its fault-in and each writeback copy and its first write fetch logs it - **packed** (DH: record kind 35, `PACKED_PAGE_IMAGE`, the page less its zero bytes, rebuilt byte for byte) where the page shrinks and a `FULL_PAGE_IMAGE` where it does not, no version moved for the kind and an older build refusing it at redo - a `PAGE_INIT` heals as an image does, and images are off through the mount; page 0, the maps, headerless and unlogged pages still refuse the mount torn (`wal.md` §10, `known-gaps.md`). **Every core checkpoints its own active transactions, one run at a time for the instance, and one cadence run a `checkpoint_interval_ms` at any `cores`, the cores in turn** (`wal::CheckpointGate`; DH: a core's own run comes every `cores` intervals, so the replay range and the log kept are up to about `cores + 1` intervals). **An insert logs under the holds that placed it**; the paths that still log after release are in `records-appended-after-their-page-is-released.md` (closed; `git show d83bd502^:docs/inflight/bugs/<name>`). **The log is recycled** below the redo start of an anchor a sync made durable — the fold never decreases and redo never starts below the scan start; no archive gates it and no retention setting exists. **A failed log write or sync fail-stops the log** (either syncing thread, or a ring-full refusal once its drains are spent): every later write is refused and no page is written back until a restart. Above one core a commit leaves its reactor (a `group` sync is the WAL writer's; a `strict` commit parks and publishes after its sync, `CommitDeferred`, a failed sync aborting it). An append's I/O is off the stream latch, **but a segment roll syncs the segment it leaves under the latch** (`wal.md` §6); **the cadence checkpoint tries that latch at its begin and end records, its log park's flush and its recycle, and parks instead of waiting a roll out on its reactor** - a skipped flush asks the writer for nothing above what is flushed, to 16 busy answers a site a run (`wal.md` §11, CZ). A transaction-id carve persists page 0 alone, which hands the pool's writeback to the checkpoint. The cadence's checkpoint is a `system` task: every data-file sync on the instance's one data-sync thread (sticky: a failed one stops the instance), the anchor's page written after a sync of every page, its `pwrite`s on the reactor (`known-gaps.md`) | `docs/spec/wal.md` §8, §11, `docs/spec/sched.md` §4, `instructions/v3.0.0/workorder-bq-wal-segment-roll.md`, `instructions/v3.2.0/workorder-cz-checkpoint-roll-wait.md`, `instructions/v3.2.0/workorder-dh-page-image-volume.md` |
| Transactions & MVCC | Built. **Every reader is a commit-LSN snapshot over the instance read view**; no view is a bound on trx ids, and `SnapshotTooOld` is never raised. Live views and reader leases bound the purges instance-wide (`ReadHorizon()`); a previous run's undo pages are not reclaimed at mount. **`BEGIN ... READ ONLY`** holds a view and a reader lease across statements — no id, no record, no in-flight slot — every write refused `READ_ONLY_TRANSACTION`, and a `readonly` connection's `BEGIN` is read-only by default. **`SELECT ... FOR UPDATE`** (DC), inside an explicit `READ WRITE` transaction, returns the rows an `UPDATE` with the same `WHERE` would write, each under that `UPDATE`'s `U` claim held to the decide - PostgreSQL's `FOR NO KEY UPDATE`, a foreign key child's share passing it - by the write's row walk with its leaves fetched for read: no row leaves before every row is claimed, a row newer than the view is refused and never returned, nothing is written or logged, and only an equality or a list on the tuple key descends (`txn.md` §5, "A locking read"). **An explicit transaction is rolled back at `txn_lifetime_ceiling_ms`** (60 s, no off-switch): a running statement is never cut short, and the client is told at its next statement (`COMMIT` refused); autocommit is not bounded and a KWP load session is exempt (`txn.md` §1). **The lock family**: units (relation, range, tuple, index key), modes (`IS`, `IX`, `S`, `X`, and `U`, an `UPDATE`'s version claim that a foreign key's share admits), a wait-for graph and a deadlock detector, one table for the instance in 64 partitions, nothing of it persisted. A writer meeting an undecided holder waits for its decide, on any core (the in-flight table holds 1,024 ids per core; a `BEGIN` or an autocommit write past it waits for a slot under `res.txn_slot`, refused at the fault net (DJ-S3)), and a waiter that would close a cycle is refused naming deadlock; what still polls is in `known-gaps.md` (Locks). **A read takes a borrow too**: every relation a statement names is declared at the bind and held for the statement, it is what a `DROP TABLE` waits for, and nothing finer is taken; the defence it gives is one-directional, and `txn.md` §5 says why | `docs/spec/txn.md` §5, `docs/rules/rules.md` §3, `instructions/v3.2.0/workorder-cs-read-only-transaction.md`, `instructions/v3.2.0/workorder-dc-select-for-update.md` |
| Transactional DDL | Built and durable: catalog writes logged, DDL under a real transaction, losers rolled back at mount. `CREATE TABLE` is atomic, isolated and consistent. `CREATE INDEX`/`DROP INDEX` are atomic and isolated on every core and take the relation `X` — but a plain autocommit `CREATE INDEX` builds online under `IS` (`index.md` §10.3) — waiting for open writers and positioned readers, a wait at the 1 s fault net refused `TxnConflict`; `DROP INDEX` is admitted inside an explicit transaction. **`DROP TABLE` is atomic but not isolated** (§5a). No catalog page budget: a catalog chain grows from the general page supply (`catalog.md` CT5). **One name, one row on every core** (`catalog.md` CT7): the check refuses a name another core's open drop has freed, and no relation lands in a dropped namespace. **A DDL runs where the session is, and no core is told of one**: every core's catalog asks the instance's schema version word at its task boundaries, and the relation `IS` — not the word — is what keeps a stale parse from executing | `docs/spec/ddl-transactional.md`, `docs/spec/catalog.md` |
| Namespaces | A namespace is a name-collision domain and **declares the affinity** of the relations created in it, which nothing reads (D18: a weight-0 hint with no consumer until AS-E); the `placement` key is refused by name. `CREATE NAMESPACE` / `DROP NAMESPACE` (RESTRICT), `SHOW NAMESPACES`, `ns.table` everywhere a relation is named. **A relation's name is unique within its namespace, and an unqualified name means the session's namespace only** - `public` until `USE <namespace>`, no search path, `sys` read only as `sys.<view>` (NS5, NS6, NS8); identity stays the oid | `docs/spec/namespace.md` |
| Query language, parser, step chains, joins, subqueries | Built. **The engine chooses an inner-join block's order**: every order up to four relations, greedy above, priced by a cost model (`exec/join_order.hpp`) over the join planner's statistics, so one catalog and one statistics generation give one plan; a step's id is its written position and a block with a subquery predicate is not reordered. **`join_order` says who may change that order, and a server's default, `fixed`, lets nothing** (`parser-v2.md` I12, `instructions/v3.2.0/workorder-cx-managed-plans.md`): a searched shape's order is captured - keyed by fetch id and every relation oid, no literal - once every relation of the block has a published prior, and every later compile of the shape runs it whatever the statistics do, from one instance table whose captures core 0's `system` task writes to the logged `sys.plans` (superblock 37, CX-S1a) and a mount loads back, so a restart, a crash or a restore searches no captured shape again; `DROP PLAN` re-arms one; `planned` searches at every compile, `written` keeps the written order, an admin's `SET JOIN_ORDER` flips the instance's one value for the run, and a dispatcher with no instance behind it runs `planned`. **An administrator's plan outranks every value** (CX-S1b): `CREATE PLAN <name> FOR <select> VARIANT ... WHEN <rule> ORDER (...) ... ELSE ORDER (...)`, or `AS SEARCHED`, attaches join orders to a shape - by example, with literals - as a logged `sys.plans` row; the variant is chosen at every compile by a total rule over the statement's arguments (`ARG(k)`, integers with `+ -`, strings by `=`/`!=`, `ELSE` mandatory), a variant may pin one access arm a relation (`ACCESS <relation> TUPLE_KEY | INDEX [<name>] | CABIN [(<column>)] | SCAN`, or `NOT` one), which restricts the ladder and never forces an arm (CX-S3a), a statement over one relation takes a plan for its arm (`ORDER (<the relation>) ACCESS ...`, CX-S3b), a variant may pin a relation's join method (`BUILD <relation> ON | OFF`: the statement-local build held to, or taken away; CX-S5), `ADVISE PLAN FOR <select>` runs the search once and proposes its order as `CREATE PLAN` text priced against the plan in force, applying nothing (memory-only `sys.plan_advice`, CX-S7), no compiled-chain cache is built (CX-S6), a variant that does not fit a statement is declined for it, `ALTER PLAN` enables, disables or replaces a variant, and an enabled plan RESTRICTs its relations' `DROP TABLE`, a pinned index's `DROP INDEX` and a named Cabin's drop. **A plan a rename put out of reach reads `invalid` and restricts no `DROP TABLE`** (DI): a renamed relation, or a column the statement writes by its binding, established from the plan's row alone, for a captured order as for an administrator's. A captured or administrator's order never refuses a statement its written order answers, and **without `ORDER BY` emission order is no contract**. The statistics are memory-resident but for a relation's prior, which outlives a clean shutdown (unlogged `sys.statistics`, `plan-stats.md` §8; a crash or a restore keeps none). Range bounds cross a join-key equality class as derived conjuncts. **In a WHERE**, `OR`, `NOT` and parentheses: a group holding `OR` or `NOT` is one three-valued conjunct (`Step::bool_residual`), a subquery predicate under it included, never a key, bound, probe or propagation source; `IN`/`NOT IN` lists, `NOT BETWEEN`, `LIKE` and a literal on the left are boolean conjuncts each. **A value list is a key set**: `col IN (...)` on the tuple key, on one column of an index's equality prefix or on a column with a column Cabin is served by one descent per distinct value, the list kept in the residual and checked on every row found, a Cabin set only once every value is observed; an `UPDATE` or `DELETE` whose list is on the tuple key writes the same way, under the units the walk of its `WHERE` takes; any other list, and a write whose `WHERE` holds a subquery, is walked. **A query layer** (`parser-v2.md` I19) answers a SELECT the native parse or compile declines over native single-relation walks at the statement's snapshot — expressions, `DISTINCT`, grouping and `HAVING` (folded by `aggregate.md`'s rules), outer, cross, comma, `USING` and condition joins, set operations, derived tables, subqueries as values — typed at compile with no implicit conversion, bounded by `sort_max_rows`, recording no Waystone trail; no `WHERE` term is carried by a walk on a nullable side (BU-Q7 `[quiet-wrong]`). **The DML forms** (`parser-v2.md` I20): expressions in `VALUES`, `INSERT ... SELECT` read whole before a row is placed, `ON CONFLICT ... DO NOTHING` on the tuple key and `DO UPDATE SET` on the tuple key or the one column of a unique index, `UPDATE ... FROM` refusing a row that matches two `FROM` rows (BU-Q16 `[quiet-wrong]`), `DELETE ... USING`, `RETURNING`. **`FOR UPDATE` is the one locking clause** (`parser-v2.md` I11a), read after the pagination tail of a `SELECT` of `*` or plain columns over one relation, nothing reserved; every other strength, option and shape is refused by name at its byte, and no chain, trail, plan, Cabin or query-layer walk answers a statement carrying it. A scan kernel rejects on the cell's bytes before any decode, and an `UPDATE`'s or `DELETE`'s `WHERE` is checked once (`parser-v2.md` §7). `WITH (...)` table options are refused `NotImplemented`; `SET DURABILITY` sets the session rung (`protocol.md` §9). Two scalar functions, `DATE(timestamp)` and `NOW()`, in a WHERE comparison only and on either side, a residual never a key, bound, index, join, build or Cabin probe; a D2 statement records no trail (`exec/functions.hpp`) | `docs/spec/parser-v2.md` I10, `docs/spec/types.md` §3.2a, `docs/spec/plan-stats.md`, `docs/pending/keyed-write-beside-a-subquery.md`, `instructions/v3.0.0/workorder-bu-basic-query-surface.md`, `instructions/v3.0.0/workorder-bt-accept-tuple-row.md`, `instructions/v3.2.0/workorder-cb-join-planner.md`, `instructions/v3.2.0/workorder-cc-plan-statistics.md`, `instructions/v3.2.0/workorder-cg-on-conflict-do-update.md`, `instructions/v3.2.0/workorder-cu-key-set-access.md` |
| `UPDATE ... SET` expressions | Built. A SET value is an expression: arithmetic, `||`, the comparisons and `AND`/`OR`/`NOT`/`IS NULL` as bool values, `CASE`, `COALESCE`/`NULLIF`/`GREATEST`/`LEAST`, `CAST` and `::`, the scalar functions of `types.md` §3.2b, date +/- days, and a scalar subquery run once before the walk. **Every expression is typed at compile with no implicit conversion** (a mistyped one is refused whether or not a row matches) and **evaluated per row against the before-image** (`SET a = b, b = a` swaps). A foreign-key column takes a constant expression only. NULL propagates; a division by zero, an overflow and a NULL into a NOT NULL column are the row's refusals. Not built, refused by name: `IN`, `BETWEEN`, `LIKE`, a correlated subquery, boolean literals, timestamp arithmetic | `docs/spec/types.md` §3.2b, `instructions/v3.0.0/workorder-bj-expression-update.md` |
| `ORDER BY` | Any columns in a non-aggregated statement, each `ASC`/`DESC`, up to 8 keys, `sort_max_rows`-capped, a top-N heap under `LIMIT`. **An order a structure already holds is read in it under a `LIMIT`, no sort** (CV): `<tuple key> DESC` by a reverse walk, and an index's keys after the ladder's equality prefix by an ordered index walk either way, a descending one returning ties by descending tuple key. Refused over aggregated output | `docs/spec/parser-v2.md` I11, I12, `docs/spec/index.md` IX8c, `instructions/v3.2.0/workorder-cv-ordered-index-walk.md` |
| Aggregation (GROUP BY, COUNT/SUM/MIN/MAX/AVG) | Built | `docs/spec/aggregate.md` |
| Types | DATE, TIMESTAMP, DECIMAL, DECIMAL128, `char(N)`, `varchar(N)`; `float` refused. `varchar(N)`'s N is that column's `kds.inline_cell_width` — a width in `[16, 4096]`, not a length cap; a longer value spills. A bare `varchar` reads as the instance width | `docs/spec/types.md`, `docs/rules/rule-fixed-length-tuple.md` §4 |
| NULL | A tail bitmap sized to nullable columns; an all-`NOT NULL` relation keeps a byte-identical layout. **NOT NULL is the default and `NULL` is opt-in**; nullable index keys refused; NULLs sort largest; the bitmap is sole authority; WHERE is three-valued | `docs/spec/null.md` |
| Waystone | Recording and replay on every core, keyed by `{fetch_id, arg_hash}` — the statement's shape without its select list — **for opted patterns only**: `WAYSTONE ON <select>` opts a shape in by example — compiled, never run, refused `Unsupported` where the shape admits no tuple key lookup or probe under any plan — and `WAYSTONE OFF <select>` or `<fetch_id>` opts it out, autocommit only, answered once durable at the session's rung. **An opt-in is the shape under the relations its example bound** (DI-S3, superblock 38): the same text under another namespace is another shape with its own trails, and a committed `DROP TABLE` ends the opt-ins naming the relation. No `SELECT` registers a row, claims a directory or writes the log. Trails are unlogged: every mount renews an opted row's directory empty after recovery and before the listener (`RenewPatternDirectories`) and keeps the opt-in. `waystone_recording`/`waystone_replay` are the ceilings over opted patterns. Trails are never retired or decayed | `docs/spec/waystone-concpets.md`, `instructions/v3.2.0/workorder-cf-waystone-premise.md` §8 |
| CREATE PATTERN | Withdrawn and removed; `$name` parameters refused everywhere. `SysPatternRow`'s `flags` bit 0 is the Waystone opt-in (`kPatternWaystone`) and its `origin` byte is reserved, written 0 | `docs/spec/create-pattern-user-defined-patterns-v1.md` (withdrawn) |
| Cabin | Built, **one store for the instance**, partitioned by `cabin_id`: every core observes into it and serves from it, through a `CabinSet` handle. A set is authoritative for the observed value whole. A build installs an unservable empty set first (§6's announce), so a write on any core during the walk is in it; what the announce cannot cover the banking gate asks of the instance (§6a), and a Cabin therefore builds rarely while anything is unresolved anywhere (`known-gaps.md`). Entry sets are memory-resident. `CABIN AUTO` is the instance's one controller's. **A Cabin is a key function `F`**: a column, an immutable expression (`DATE(ts)`) or a conjunction of literal comparisons, its canonical text its identity (`sys.cabin_exprs`); witnessed on INSERT and on an UPDATE that moves an input, a predicate served only on an exact match of every conjunct, and a two-sided range served from `DATE`'s observed days (cover) | `docs/spec/cabin.md`, `instructions/v3.0.0/workorder-aq-expression-cabins.md` |
| Secondary indexes (multi-column, covering, unique) | Built. No index-only scan. **A plain autocommit `CREATE INDEX` builds online** under the relation `IS` (`index.md` §10.3), its row flagged `kIndexFlagBuilding` — maintained by every write, chosen by no read; `UNIQUE`, `REINDEX`, a create inside `BEGIN` and a dispatcher that cannot park or has no lock table, transaction manager or clock keep the `X` build. **A unique index** refuses a write giving two live rows one key value, checked per row at latest state under a `kIndexKey` lock unit (`index.md` §10.2). An insert re-checks its leaf's coverage (a stale descent restarts, bounded, then `TxnConflict`), on the premise that no index entry is removed; a divide finds and holds every parent it writes before it writes, as the clustered tree's does. **An entry's sort key is the whole entry, covered bytes included** (IX4b), so no two entries share one; a covering probe keeps a row if any of its entries passes the filter. **`REINDEX INDEX`** is a drop and a create of one name in one autocommit transaction under the relation `X`, under a new oid; a dropped, rolled-back or rebuilt tree's pages are freed by an index job past the redo start and every older statement. **`PURGE INDEX <name>`** (DD) takes out of a live tree, under the relation `IX`, the entries a rebuild would not append and a deleted row's once every reader sees the delete, each row judged by the rebuild's own walk under its tuple `U` asked try-only, a leaf's first entry never removed, each rewritten leaf one `FULL_PAGE_IMAGE` at `kNoTxnId`, in 16-leaf slices that yield their core and hold nothing across a yield, no transaction held; nothing runs it unasked (`index.md` §2.2) | `docs/spec/index.md`, `instructions/v3.2.0/workorder-bx-unique-index.md`, `instructions/v3.2.0/workorder-by-reindex-drop-index-reclaim.md`, `instructions/v3.2.0/workorder-ci-online-index-build.md`, `instructions/v3.2.0/workorder-dd-index-entry-purge.md` |
| Statement-local inner build | The walked-join shape takes a `BuildKey` annotation and buckets rows into a statement-scoped map; `join_build_max_rows` is the cap, `0` the off-switch; `ANALYZE` reports `inner_built=`/`build_rows=`/`build_probes=`. A built join owns no `sys.patterns` row and feeds no Waystone trail | `docs/spec/join-inner-build.md` |
| Foreign keys | Declared and enforced **where the statement runs**: the forward check descends every parent there and the reverse check walks every child whole there. The forward check is hoisted, so its wait happens before any row is written. **D9(a)**: the forward check takes `IS` on the parent relation and `S` on the parent row **before it descends**, held to the decide, so a parent `DELETE` waits for every open child writer on whichever core either runs; a failed check gives the `S` back. A parent `DELETE` meeting an undecided child whose writer holds no `S` parks on that writer and re-checks at its decide. A parent `UPDATE` proceeds past open child writers (its `U` admits their `S`); the deadlock class that leaves (a parent updater passes a child's share, then waits on that child writer, which writes the parent) is accepted (CP-Q10 (a), `txn.md` §5). A Cabin set finds a child and, drained, clears a parent — sound because D9(a)'s `S` keeps every writer that sets the fk off the set while it is read and the banking gate declines a set banked over an open child insert; a moved-off undecided row gives the set up and the walk answers. The costs are stated in `foreign-keys.md` §2c. Refused: a self-referencing key (`NotImplemented`, so no parent is written by the statement it checks), a hidden-Keystone heap parent (a declared-Keystone heap is a parent, and any heap a child), CASCADE/SET NULL | `docs/spec/foreign-keys.md` |
| Assertions | Enforcing on every core, including immediately after a restart. **One registry for the instance**: every core's writes check and reserve into the one directory each assertion has, under the registry's latch, and an admission holds its contribution until its reservation is logged. An assertion the registry cannot enforce refuses the relation's writes on every core. A build holds the relation `X` under a transaction of its own (a wait at the 1 s fault net refused `TxnConflict`), and a create is adopted with its base under one hold, before its row, so every core-0 checkpoint after it names the assertion. A partial or torn snapshot run is discarded, never adopted, and an assertion left with no whole run comes up unenforcing until `DROP` and `CREATE ASSERTION`. A crash loser's reservations are given back at the mount | `docs/spec/assertion.md` §6.1, §6.2, §7, §8.1, `instructions/v3.2.0/workorder-cl-assertion-reservations-of-an-undecided-transaction.md` |
| Transitions | Built. `CREATE TRANSITION <name> ON <table>(<column>) ON INSERT (...) ON UPDATE (a => b, ...) [ON DELETE FROM (...)]`: a column's initial states, `old => new` edges and deletable states, checked per row from the row alone - no registry, no derived state, no record kind - so a path property follows by induction and no history is stored. One row per Transition in `sys.transitions`, the declaration verbatim; the fence is the relation `X` and the memo check (`TransitionMemoFenceTest`). A violation is `TRANSITION_VIOLATION`. `PURGE` is refused on a governed relation, so its keys stay bound for life; `DROP TABLE` takes its Transitions in autocommit and is refused inside an explicit transaction while one stands | `docs/spec/transition.md`, `instructions/v3.2.0/workorder-bv-transition-constraints.md` |
| Managed statistics (CY) | **Built since CY-S1b (2026-11-18)**: an operator's relation statistics object - published, pending, the last 8 replaced, a lock, a growing version - outranks the steward's sampled prior while published; `GATHER STATISTICS` (a leaf count sliced and paced to 16 leaves per 2 ms, under a per-slice `IS`), `LOCK`/`UNLOCK`/`PUBLISH`/`DISCARD PENDING`/`RESTORE`/`EXPORT`/`IMPORT`/`DELETE STATISTICS`, `sys.statistics`, `sys.statistics_history`; unlogged, kept across a clean shutdown only (CY-S1a's `sys.statistics`, superblock 36); nothing gathers on its own; no column statistics (the operator's ruling W13) | `docs/spec/plan-stats.md` §8, §9, `instructions/v3.2.0/workorder-cy-managed-statistics.md` |
| Access statistics | `SHOW ACCESS`. **Written where the statement ran, into memory** (DA): the instance's store, one part per core - a flat table sized by `access_stats_bytes_per_core`, 4,096 shapes a core by default - merged when read, emptied by a restart; no catalog page is written. The relation's dead bootstrap left at superblock 38 (DA-S3): oid 130 is retired and page 11 reserved and unused | `docs/spec/heap-and-tuple.md` §7, `docs/spec/sys-views.md` §6.2d, `instructions/v3.2.0/workorder-da-access-stats-memory-resident.md` |
| ALTER TABLE | Catalog-only: RENAME TO / RENAME COLUMN; assertions RESTRICT. **`ADD COLUMN` / `DROP COLUMN` write no row**: `CREATE TABLE ... SPARE n` reserves 16-byte slots in the fixed row size, `ADD` places a nullable column (cell <= 16 bytes) in a free or clean slot, older rows reading NULL, and `DROP` flags the column and leaves its bytes; both under the relation `X`, transactional, readers not isolated; dependants RESTRICT. **`PURGE DROPPED COLUMNS`** zeroes a dropped column's cells in place and releases their spills, row by row under the leaf hold once the writer is resolved for every reader and after a statement-epoch gate; an `ADD` with no vacant slot then reuses a clean one at its own position (oid kept, `generation` + 1; a KWP load checks a column-set digest). An `INSERT` column list is shifted by no column change; on a changed relation a positional `INSERT` is full width, and a stale key-omitted positional row after a `DROP` is a `[quiet-wrong]` known gap. Rows are never resized; type changes and an `ADD` without a free or clean slot are refused | `docs/spec/alter.md` AL11, `instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`, `instructions/v3.2.0/workorder-cd-dropped-column-cleaning.md` |
| DROP TABLE | Catalog-scoped: oid tombstoned and never reissued; fkeys/assertions RESTRICT, asked again under the `X`. **The drop takes the relation in `X`**: it waits for a positioned reader and for an open writer, bounded by the lock family's 1 s fault net, so under a continuous stream of readers it can be refused `TxnConflict`. **Its pages are reclaimed** once the durable redo start is past the drop and no statement that could have bound it still runs; a crash mid-reclaim is re-driven, and pages no root reaches stay leaked (`known-gaps.md`). Atomic, not isolated | `docs/spec/drop-table.md` |
| Bulk insert | Multi-row VALUES, the full pipeline per row. **The KWP load stream carries every column, the tuple key first, and every storable type** (BI15, BI16): a NULL tuple key field takes an issued id and a value names one, per row. On a heap the load's field 0 is the hidden key. There is no sorted fill | `docs/spec/bulkinsert.md` |
| Page epoch and decay score | The page epoch (R4) is a page-header word moved only by a leaf division or an in-place compaction, compared by Waystone replay and Cabin hints; the lazy-decay score (R1) is the one time-weighting, read by the Cabin controller. **No mover exists**: `SHOW RELAYOUT`, `sys.relayout` and the `physical_optimizer` key (refused at boot by name) are deleted | `docs/spec/heap-and-tuple.md` §3.1a, §7.1, `docs/spec/decay.md` |
| Cabin controller | Built, with `SHOW CABIN_OPTIMIZER`; managed state is memory-resident. **An execution signal is a fingerprint's and its candidate's together** (PO7c, DI): one text over two relations prices each relation's Cabin with its own executions. **One surface for the instance** (`OptimizerSurface`): every core's `SET CABIN_OPTIMIZER` flips the one switch; **a core notes into its own part of the collector, which the tick folds, and nothing at all while the switch is off** (DL: no identity built, no note); `SHOW CABIN_OPTIMIZER` reads the copy the tick last published. **It builds and chooses no statement's arm** (PO11, CX-Q6 (a)): a Cabin it made is read through only by a statement whose administrator's plan names it (`ACCESS <relation> CABIN (<column>)`), and a Cabin so named is not dropped by it | `docs/spec/cabin-controller.md`, `instructions/v3.2.0/workorder-cx-managed-plans.md` §8 (CX-S3b) |
| Buffer-pool eviction | Every accessor returns a pinned `PageRef`. **One pool for the instance**: `buffer_pool_frames` is required, an undivided total and the pool's hard ceiling; a missing key, `0`, or a value below the volume's floor (resident-class pages plus 256) is refused at boot, and resident frames never exceed it. **Frames are slots** in chunks that never move, with a free list and a CLOCK hand; a miss or a creation reserves its slot first. A full pool refuses `ResourceExhausted`, only outside a mutation's no-refuse window, since a refusal after a page write would leave it half done (recovery, rollback and the assertion loops drain instead). A `SELECT`'s outermost walk faults cold. A freed page's frame is discarded, dirty or not. Writeback copies each page under its page latch and cleans only what the copy covered. **The structure latch's holds** (DO): a read hit is two, a write hit three, the hit asking no free-map bit; the checkpoint's dirty tables hold it for their walk alone (sized before, sorted after) and the table never rehashes - the walk itself, 15 ms at 1 Mi frames, stays whole until the operator marks DO-S3's chunking (`page.md` §8, `known-gaps.md`). **A miss is a blocking read on its reactor for every reader but two**: a top-level `SELECT`'s descent and a `SELECT`'s walk awaited on a reactor ask the device first without waiting, and a read that would wait is handed to the page reader's threads while the statement parks under `io.page_read` (`page.md` §12) | `docs/spec/eviction.md`, `docs/spec/page.md` §3, §12, `instructions/v3.0.0/workorder-be-bounded-pool.md`, `instructions/v3.2.0/workorder-de-page-read-off-the-reactor.md` |
| Cross-core execution | **Every statement runs on the core its session is on**, and a transaction is one core's, whole. **A session's core is re-decided at every transaction's head** (`server/load_placer.hpp`): with no transaction open it may move whole to the least-busy core; a transaction never crosses. **`SET PRIORITY <level>`**: while an `analytic` or `background` session lives, those levels run on `max(1, cores / 4)` batch cores - core 0 first - and every other level off them (`server/priority.hpp`). The lock family's fault net is 1 s (`lock_wait_fault_net_ms`). Every core carves its own transaction-id window and bumps a relation's row-id mark in place, and **the one cross-core path is write-then-kick through the wake registry**, best-effort, a skipped kick costing one idle block. **A `SELECT`'s outermost walk yields every 64 pages** (16 for an `analytic` session), holding its view, its borrow, its core's statement epoch at its word and a pin on the catalog memo (`server::SelectRun`, `sched.md` §3); nothing else running yields, a tuple key descent included - but at a page read that would wait for the device, where a top-level `SELECT`'s descent is discarded and run again after a park, and its walk parks and fetches the leaf again (`page.md` §12, `sched.md` §3) | `docs/spec/crosscore.md`, `docs/spec/sched.md`, `instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md` §6 (closed with §5.2's bar (v) not met) |
| Task representation | C++20 stackless coroutines | `docs/spec/sched.md` §3 |
| Keystone id issue-once contract | A tuple key `UPDATE` is refused at compile; `sys.tables.next_id`'s bump logs and replays with every catalog write. **K1 has one named exception, `PURGE`** (`heap-and-tuple.md` §4.1c): `PURGE FROM t WHERE <tuple key comparisons>` - autocommit, a declared tuple key, admin - retires keyless every committed delete-marked row of its window once every reader is past its deleter, logging `SLOT_RETIRE` and `VARHEAP_RELEASE` at `kNoTxnId`, taking the relation's `IX` and no row unit, and waiting on older readers and undecided writers up to `kPurgeHorizonWaitNs` (1 s) per attempt; a named `INSERT` then places the key. Its horizon gate is `[quiet-wrong]`: every read view that can see a superseded user row must stay registered with the horizon | `docs/rules/keystoneid-invariant.md`, `docs/rules/keystoneid-k0-findings.md`, `docs/spec/heap-and-tuple.md` §4.1c |
| Caller-supplied tuple key | Per row, an `INSERT` names the tuple key or the engine issues one. **A btree leaf places a row at the slot its key sorts to**, whatever order keys arrive in. **A named key is refused because of its tuple key for exactly two reasons**: a slot keyed `k` - live or delete-marked - is `AlreadyExists`, and a key outside `[1, 2^48 - 1]` is `OutOfRange`. A rolled-back key is free, a committed key stays bound until a `PURGE` frees it, and a named key meeting an undecided insert of the same key waits for its decide. `sys.tables.next_id` is the issue cursor for omitted keys and K4's budget, and gates nothing. There is no key mode and no key-order state; the `sys.tables` byte stays, written 0 and read by nothing | `docs/spec/heap-and-tuple.md` §4.1, `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`, `instructions/v3.0.0/workorder-bk-keystone-48.md` |
| Wire protocol KWP/1 | **Every core accepts on the default port** (SO_REUSEPORT above one core, one TLS/SCRAM/limits setup; `peer_listeners` is refused by name; where the platform refuses SO_REUSEPORT, core 0 accepts and hands each connection to a core, `include/kds/server/connection_handoff.hpp`). The default port speaks length-prefixed binary frames: handshake with capability intersection, SCRAM, PARSE/BIND/EXECUTE over statement and portal handles, typed row batches, portal suspension, transaction and durability frames, a pinned error registry. `kMaxFrame` 16 MiB, 64 KiB batch target, 60 s portal-idle timeout, 64 statements and 64 portals per session; `max_rows` bounds delivery, not execution; `C_DESCRIBE` of an unexecuted statement is refused. The newline protocol is `debug_text_port`'s loopback debug surface, off unless configured; `STOP` as a line command is reachable only there, and over KWP it is an admin-classed statement. TLS 1.3 and the statement-class roles are off by default; the `COMPRESSION` bit is reserved and never offered. A pipelined statement's reply leaves in one write. **A plain `SELECT`'s rows leave while it runs**: rows followed by `S_ERROR` are no result, a statement whose rows have left is not re-run, and a client that stops reading parks the walk (`protocol.md` §7). **`C_CANCEL` is answered, and `CANCEL` offered, only where a listener has an unguessable identity source and the instance's `CancelRegistry`**: it is taken at a walking `SELECT`'s next slice boundary, ending it `CANCELLED` and failing its transaction, or else at the next frame. **Connections and sessions** (DJ, `protocol.md` §7, §10): `max_connections` caps the instance's sessions - a connection from the end of its handshake, so a `C_CANCEL` is never refused - with `reserved_admin_connections` for `admin`; `idle_session_timeout_s` closes a session holding no statement, transaction or live portal, told why; both off unless set. `RESET SESSION` returns a session's settings, statements and portals to a new connection's, and `S_READY` says so behind `SESSION_DEFAULTS`. A walk parked on a client that takes no byte for `txn_lifetime_ceiling_ms` is cancelled, and a capped portal past 16 MiB is refused by name | `docs/spec/protocol.md`, `docs/spec/client-manual.md`, `instructions/v3.2.0/workorder-ce-streaming-reply.md`, `instructions/v3.2.0/workorder-dj-connections-sessions-cursors.md` |
| Backup and restore | Built. Two paths over one restore: **B**, `BACKUP START` / `COPY` / `STOP`, the server copying its data file through a device write fence; **D**, an atomic snapshot between START and STOP. Either backup carries its own log `[start, stop]` and a checksummed manifest. **Restore is mount recovery from the backup's start, never the copied anchor, and does not open short of the stop**; unlogged state is discarded at the restore's mount. The start is pinned against the recycler and the dropped-relation reclaim; a WAL archive (`archive_dir` / `archive_command`) holds recycling and fails closed; PITR to an LSN; every restore takes a new lineage. `kds_server --restore-from`, `tools/kds_backup.py` | `docs/spec/backup.md`, `manual/server/backup-restore.md` |
| Simulation harness and test hardening | A whole instance on crashable in-memory devices driven through `CommandDispatcher`, crashed at a seed-chosen op, restarted and reconciled against an oracle; `scripts/sim.sh` runs the corpus. **BS is built and not closed until seven consecutive nightly reports**: torn and reordered power-loss writes, swept I/O and allocation failures, a PostgreSQL 18 differential (`scripts/bs/difftest.py`), `asan`, `tsan` and `noassert` builds, a coverage floor per subsystem, three libFuzzer targets and a list-append history checker. All of it is out of the push gate (label `bs-nightly`) and run by `scripts/bs/nightly.sh`; `scripts/bs/mutants.sh` is run by hand | `sim/`, `scripts/sim.sh`, `scripts/bs/`, `instructions/v3.0.0/workorder-bs-test-hardening.md` |
| Core placement on disk | **Nothing names a core on disk**: `sys.tables.owner_core` is a reserved word - written 0, never read - so the core count is not pinned and a mount at another `cores` records it. A relation is one structure headed by `desc_page_id`; `range_size_ids` is refused by name, and the superblock refuses every older version | `docs/spec/crosscore.md` (CC7-CC10), `docs/spec/heap-and-tuple.md` §4.1a |
| Observability | **Every engine report but `ANALYZE`'s plan and `BACKUP STATUS`'s line is a `sys` view read by `SELECT`**: `SHOW` and `DESCRIBE` are one `SELECT` over one view each, answering the same rows and refusals (SV1-SV5); schema-object views read under the session's view, engine-state views do not (BO-Q2). `sys.meta` carries the session's core's fields (`sched.md` §4). **No statistics record waits, and a switched-off consumer collects nothing** (DL, `docs/rules/rules.md` §3a, `instructions/v3.2.0/workorder-dl-statistics-queue-no-statement.md`): a record writes its own core's words or tries its part's latch and drops and counts (`sys.meta`'s `*_busy` and `*_dropped`), a reader copies and lets go, a compile and a view read a published copy, a reactor's statistics callback tries every latch, and a debug build aborts a record that would wait or fetch a page. The statistics views are memory-resident, per-core parts, never failing what they describe: `sys.access_stats`, `sys.statement_stats` - a shape's calls and what it cost, NULL where the engine could not count it, **one row a binding** (DI): a fetch id is the statement's text, so the row's key carries the relations the statement bound and the namespace its unqualified names meant, and one text under two namespaces or over a relation dropped and created again is two rows - with `sys.statement_histogram` and `sys.refusal_stats`, `sys.relation_stats`' write activity, cumulative lock waits and space (`est_delete_marked`, the planner's steward's sample, one table for the instance kept beside its prior and never in it; `leaf_divisions`, `leaf_compactions`), `sys.indexes`' leaf divisions and usage (`probes`, `ranges`, `entries_scanned`, `rows_resolved`, `last_used`; the instance's own store, read by nothing), `sys.lock_waits`, `sys.checkpoints`. **How transactions end and what holds the read horizon**: `sys.meta`'s `txn` section counts a core's read-write transactions by how each ended - `commits`, `rollbacks`, `ceiling_rollbacks`, `close_rollbacks`, `aborts`, `wait_aborts` - and carries the instance's read horizon (its LSN, the core holding it, the age of the view holding it); `sys.txn_histogram` holds explicit transactions' lives, `READ ONLY` apart; `sys.session_state`'s `txn_age_us` and `view_age_us` name the session. **Wait events and ASH**: every suspension is inside a `WaitScope`, the cumulative wait statistics are always on (`sys.wait_stats`, `sys.wait_histogram`, `sys.session_waits`, `sys.session_state`), and a per-core in-memory sampler fills `sys.ash`; memory only, never logged | `docs/spec/sys-views.md` §6.1-6.2c, §6.6-6.7, §7, `docs/spec/wait-events.md`, `instructions/v3.2.0/workorder-cj-wait-events-ash.md`, `instructions/v3.2.0/workorder-df-statistics-coverage.md`, `instructions/v3.2.0/workorder-di-shape-identity.md` |
| User manual | `manual/` — SQL surface, verified against code | `manual/sql/sql.md` |

## How `docs/` is organized

**`bench/`, `instructions/` and `docs/inflight/` were emptied on
2026-09-02 and have since reopened.** The last commit holding their old
contents is `1769487`; `git show 1769487:<path>` retrieves any file, and
each directory's `README.md` says what left. **A citation to one of those
paths — in this file, a spec, the manual, a test or a source comment —
resolves against that commit *unless the path exists in the working
tree*.** The qualifier is load-bearing: `docs/inflight/in-progress/` is not
a reopened bucket and a source comment still cites it. `bench/v3.0.0/` is a
fresh series and carries no delta against any v2.x number, and
`bench/README.md` states the five rules a run is invalid without. Work
orders are `instructions/<version>/`, one per milestone, listed in each
version's `index.md`. Five buckets under `docs/`, one rule each:

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
3. No tuple with `id < min_key(page)` in that page, ever — including by a leaf division — for the life of the page.
4. Tuples within a catalog chain page are unordered. **A heap data page is unordered**: a row takes a free slot of a page whose `min_key` admits its Keystone, and its slot says nothing of its key. A btree leaf is sorted by placement: every keyed slot ascends by Keystone id, live and delete-marked alike, because a row takes the slot its key sorts to (BD-R1); on a heap no reader reads slot order.
5. The Keystone column is exactly `id:48 | flags:8 | reserved:8`.
6. The Keystone word is read/written as an atomic `uint64_t` (CAS for updates); on-disk encodings use **explicit shift/mask helpers only — compiler bitfields are forbidden** for any persisted format.
7. Ids stored outside the tuple header are zero-extended `uint64_t`; the upper 16 bits are always 0.
8. Waystone is advisory: deleting it wholesale may cost performance but must never change a query result.
9. Waystone is never **authoritative**: a reader may consult it for *where to look* only, with the miss/Keystone/MVCC/fall-through discipline `docs/spec/heap-and-tuple.md` §8 spells out. It chooses where to look, never what is visible.
10. No single canonical in-memory tuple; consistency comes from page pin and latch discipline.
11. **Every** relation's tuple key is a unique 48-bit id, carried only by the Keystone word, never rebound except by `PURGE`, **never updatable**. A declared tuple key - the first column - must be integer-typed; on a heap it is the single `int64` primary key, declared first, and a heap with none carries an engine-added hidden `int64` column 0 that `*` never includes and `keyidentity` reads by name (`docs/spec/heap-and-tuple.md` §3.1b). **A rowid is not a tuple key**: a heap's slot is reused for a new row with a new Keystone once no index entry names it, so K1 is the Keystone's. On a heap a named key's uniqueness is the Keystone index's; a declared-PK heap keeps a deleted row's Keystone entry until `PURGE`, and a hidden-Keystone heap refuses a named key below its issue mark. Where the id comes from is a per-**row** fact — the `INSERT` names it or omits it — and there is no key mode. A Keystone id is bound to at most one committed tuple in the lifetime of a relation, except across a `PURGE` that freed it (K1, `docs/spec/heap-and-tuple.md` §4.1c). `sys.tables.next_id` is a high-water mark above every id placed: an omitted key is the mark, so issued keys are a sequence in issue order however many cores insert; an issued id can meet a key a concurrent statement named before the id was placed, and the placement's duplicate check then burns it and draws again, bounded at `kMaxIssueRounds`, so every key's uniqueness is the descent's (`docs/spec/heap-and-tuple.md` §4.1, §4.1a). Named keys carry no order: a named key's uniqueness is the descent's, and a leaf's slot order is its key order by placement; no relation but a hidden-Keystone heap refuses one for being below the mark. Monotonicity of issued keys is per-relation and per-history, never engine-wide.
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
  **The time a quiet gate holds a measurement is outside the budget**
  (operator, 2026-11-18): waiting for another session's build or suite to
  end is not counted, and the results file states the hold beside the time
  spent building and running.
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
version of record is `v3.5.0`**, taken on `main` at `07f4552a` (DI, DH,
the audits' contained fixes and DJ since `v3.4.16`, under one minor
version), operator-named on 2026-11-20 ("tag 3.5.0"); read its tag
message. **The tag is local and not yet pushed** (operator: "Do not push
the tag in the scheduled job"; `raft-marks-2026-11-20.md` §26).

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
