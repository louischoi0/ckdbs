# Work order BK — four statistics relations, and `SHOW` as sugar over `SELECT`

Written 2026-10-11 **in the main checkout on `main` at `ab2c46d0`**
(`v2.7.0-771-gab2c46d0`), with no worktree: the operator directed that the
documents of this session be pushed to `main` directly (W0 below). It
follows the operator's words of 2026-10-11, recorded in English
(`raft-marks-2026-10-11.md` §1-§3):

- **W0:** *"In this session it will be almost only documents, so push
  directly."*
- **W1:** *"Right now ckdbs has only the tables the sys catalog strictly
  needs, so the meta-information tables for analysis, operations and
  profiling are limited. Pick about five tables worth adding."*
- **W2:** *"Exclude `sys.sessions`."* (the operator's answer to CLA's
  question of which of its five proposed tables to leave out; the five
  were `sys.statement_stats`, `sys.relation_stats`, `sys.lock_waits`,
  `sys.checkpoints` and `sys.sessions`)
- **W3:** *"Write a workplan that adds four tables, and it must contain
  this: keywords such as `SHOW META` and `BUDGET` are only syntactic
  sugar, and the same result must be referenceable through `SELECT` as
  well. And push it."*

**Written, not opened.** W2 and W3 are the operator's decisions and stand
here as BK-R1 and BK-R2. What they leave open is §5, BK-Q0..Q13, each with
CLA's proposal and none marked. **Not reviewed**: BK-S0's
`critics-developer` review of this order is owed before BK-Q0 is put to
the operator (§7).

**Claim tags:**
- `[source-read]` with `path:line` at `ab2c46d0`, read and not run;
- `[design]` for CLA's reasoning about code not yet written;
- `[measured]` with the invocation (none in this order);
- `[quiet-wrong]` marks an item where a wrong choice turns a refusal or a
  correct answer into a wrong one with no error.

The operator's words, and a rule that restates them, are decisions rather
than claims, so they carry no tag.

**The letter.** `[source-read]` BJ is the last order on `main`
(`workorder-bj-expression-update.md`); no branch on `origin` and no file
under `instructions/` or `docs/` uses BK. BK is the next free letter.

**BK against other orders.** No order on `main` touches the `SHOW`
surface or `src/exec/catalog_view.cpp`. **BI**
(`workorder-bi-checkpoint-off-the-reactor.md`, not opened) changes how a
checkpoint run is stepped and where its syncs wait; `sys.checkpoints`
(§3.4) records a run's outcome and must be written by whatever shape BI
leaves, so BK-S7 follows BI's S2 if BI is opened first. **AQ** and **BJ**
share the expression evaluator; BK reads no expression. Not part of AR0
§8's chain.

---

## 0. What BK is

Two things, one dependent on the other.

1. **`SHOW` becomes sugar** (BK-R2). Every `SHOW` target is one `SELECT`
   over one `sys` view, rewritten at parse and run through the `SELECT`
   path, so nothing an operator can read through `SHOW` is out of reach
   of `SELECT`, `WHERE` and `LIMIT`. Today that holds for none of the 15
   targets (§1.1).
2. **Four new relations** (BK-R1): `sys.statement_stats`,
   `sys.relation_stats`, `sys.lock_waits` and `sys.checkpoints`, each
   readable by `SELECT` from the stage that builds it. They are what the
   engine does not record anywhere today: the cost of a statement shape,
   a relation's activity, a lock wait, and a checkpoint run's history
   (§1.4).

The first is the frame and lands first, so the four relations are built
as views from the start and never gain a `SHOW` handler of their own.

---

## 1. Survey at `ab2c46d0`

### 1.1 The `SHOW` surface

`[source-read]` `DispatchInner` routes 15 `SHOW` targets by their second
token (`src/server/command_dispatcher.cpp:1278-1297`). Each handler reads
its own sources and writes its own `key=value` text inline; no handler
reads a view, and none is reachable through `SELECT`:

| target | handler | reads | `SELECT` today |
|---|---|---|---|
| `META` | `HandleShowMeta` `:1469-1861` | superblock, free-map residency, allocation and reclaim counters, pool, undo, `WalStats`, scheduler, `MountRecovery`, `Contention`. **The field list exists only as this function's `ostream` writes** | none |
| `TABLES` | `HandleListTables` `:2078` | `ListTables(view)`, filtered under the session's view (DT4) | `sys.tables`, unfiltered (§1.3) |
| `NAMESPACES` | `HandleShowNamespaces` `:3034` | filtered under the session's view | none |
| `PAGE <id> [VALUES]` | `HandleShowPage` `:2099` | one page's header and slots | none |
| `PATTERNS` | `HandleShowPatterns` `:1887-1943` | `ListPatterns()` | `sys.patterns`, **rendered differently**: `fetch_id` in hex with `origin`/`pinned`/`class`/`uses`/`waystone` against the view's decimal `fetch_id` with `fingerprint_version`/`dir_depth` |
| `ACCESS` | `HandleShowAccess` `:1945-2003` | relation `sys.access_stats` | refused: not a view |
| `BUDGET` | `HandleShowBudget` `:2020-2077` | `sys.tables.next_id` through `catalog::BudgetOf` (`src/catalog/keystone_budget.cpp:5-37`), a summary line from constants | `next_id` only, through `sys.tables` |
| `CABINS` | `HandleShowCabins` `:3315-3408` | `sys.cabins` and the in-memory store's stats | none |
| `INDEXES` | `HandleShowIndexes` `:2581` | filtered under the session's view | none |
| `FKEYS` | `HandleShowFkeys` `:4035` | `sys.fkeys` | none |
| `ASSERTIONS` | `HandleShowAssertions` `:3174-3227` | `sys.assertions` and the enforcer's in-memory counters | none |
| `RELAYOUT [<table>]` | `HandleShowRelayout` `:3229-3313` | the shadow planner over `ListTables()` and `ListAccessStats()` | none |
| `CABIN_OPTIMIZER` | `HandleShowCabinOptimizer` `:3466-3553` | the controller, executor counters, decision log, signals, under the view latch | none |
| `TRACES`, `TRACE <id>` | `HandleShowTraces` `:3439-3442`, `HandleShowTrace` `:3444-3464` | the trace sink | none |

`DESCRIBE <table>` (`HandleDescribe` `:2275-2442`) is not a `SHOW` target
but is the same kind of surface: column rows and table-level fields,
among them `next_id` and the budget fields `BudgetOf` derives
(`:2338`, `:2349-2357`).

**Two findings on this surface.** `[source-read]`
- **`SHOW TRACES` and `SHOW TRACE` always answer `ERR no trace sink is
  installed`.** `SetTraceSink` (`include/kds/server/command_dispatcher.hpp:1746`)
  has no caller in `src/`, `tests/`, `sim/` or `tools/`, so `traces_`
  stays null, and `TRACE ON` is refused for the same reason (`:3415-3437`).
  The surface is reachable and does nothing (BK-Q9).
- **`manual/sql/sql.md:975` states `catalog_recovered=0`; the code writes
  `catalog_recovered=1`** (`command_dispatcher.cpp:1810`). The manual's
  introspection table (`sql.md:23`, `:975-990`) also omits `TRACES`,
  `TRACE` and `CABIN_OPTIMIZER`. BK-S8 rewrites the section, so both are
  corrected there rather than separately.

### 1.2 What `SELECT ... FROM sys.<name>` reaches

`[source-read]`
- **Five views**: `tables`, `columns`, `objects`, `types`, `patterns`
  (`src/exec/catalog_view.cpp:181-184`), each a builder that reads the
  catalog's typed readers and **materializes** a `CatalogView{column_names,
  rows}` (`include/kds/exec/catalog_view.hpp:51-54`). The header says why
  they are views and not relations (`:13-39`): a catalog row is a
  fixed-offset struct, not a row-codec tuple, so no schema can be attached
  to it.
- **Every other `sys.<name>` is refused** with `ERR no catalog view named
  ...` (`command_dispatcher.cpp:7287`), so the five relations with no view
  - `sys.access_stats`, `sys.indexes`, `sys.cabins`, `sys.fkeys`,
  `sys.assertions` - are unreadable by `SELECT`, although the manual's
  table pairs `SHOW ACCESS` with `sys.access_stats` (`sql.md:980`).
- **A view refuses** aggregation (AG12, `:6801`), `ORDER BY` (`:6823`), a
  join (`:7266`), a subquery predicate, a function call and a
  column-to-column comparison (all in `HandleCatalogView`, `:6800` on);
  `LIMIT`/`OFFSET` apply. Every integer is a `uint64` and compares
  unsigned (`catalog_view.cpp:20-29`).
- **Both forms reach a client the same way**: KWP sends a reply with no
  described sink as a one-column `TEXT` result set named `line`
  (`src/server/kwp_session.cpp:831-836`, `:885-889`). A view's reply is a
  CSV header line and CSV rows; a `SHOW` reply is `key=value` lines.

### 1.3 The filter split `[quiet-wrong]`

`[source-read]` `docs/spec/ddl-transactional.md` §5a ("Which reads
filter") puts `SHOW TABLES` and `SHOW INDEXES` among the routes that
answer under the session's view, and the diagnostics (`SHOW ACCESS`,
`BUDGET`, `ASSERTIONS`, `CABINS`) among those that deliberately do not.
Its line between the classes: a surface reporting **schema objects** must
filter; one reporting **engine state** must not. The views are not
classified there, and they do not filter: `TablesView`, `ObjectsView` and
`ColumnsView` call `ListTables()` with no view (`catalog_view.cpp:56`,
`:84`, `:102`), and `HandleCatalogView` takes no `Session` at all
(`command_dispatcher.cpp:6800`).

So, by reading and not by a run, `SHOW TABLES` and `SELECT * FROM
sys.tables` already answer differently inside a transaction while another
transaction's `CREATE TABLE` is open, and §5a's own opening sentence
names the one that answers differently as the leak. BK-R2 cannot hold
while the two disagree, so BK must pick one answer for every view
(BK-Q2).

### 1.4 What the four relations would draw on

`[source-read]`
- **Statements.** A statement's shape is already computed:
  `parser::Fingerprint{pattern_id, arg_hash, literal_count, param_count,
  fetch_id}` (`include/kds/parser/fingerprint.hpp:248-267`,
  `kFingerprintVersion = 2` at `:222`). **Nothing measures a statement's
  cost per shape**: `started_ns` is read only when a logger is attached
  (`command_dispatcher.cpp:940`) and survives only in the log line
  (`:1053-1076`); `SelectRun` (`:4910-4982`) keeps no time; the per-step
  `exec::ExecStats` (`include/kds/exec/step_vm.hpp:217-226`) feeds
  `ANALYZE` and the optimizer's signals. The only per-shape persisted
  numbers are `sys.patterns`' `use_count`/`last_seen` (`TouchPattern`,
  `src/catalog/catalog.cpp:2914-2924`), for patterns only.
- **Relations.** `BudgetOf(next_id)` gives `ids_issued`, `ids_remaining`,
  `budget_used` and the two flags exactly. The survey found no
  per-relation counter of inserts, updates, deletes, purges, pages or
  delete-marked slots in `include/kds` or `src`; the allocation and
  reclaim counters `SHOW META` prints are instance-wide.
- **Lock waits.** The lock family is `include/kds/txn/lock_table.hpp`:
  units `kRelation`, `kRange`, `kSlice`, `kTuple` (`:275-280`), modes
  `IS`, `IX`, `S`, `X` (`:282-287`), the wait-for graph and its cycle check
  (`src/txn/lock_table.cpp:374-432`), the 1 s fault net
  (`kLockWaitFaultNetNs`, `lock_table.hpp:440`). A wait is parked and
  ended in two places: `AwaitWriteBlock` (`command_dispatcher.cpp:268-529`:
  the edge `:391-392`, the park `:412`, the clear `:449`, the deadlock
  victim `:451-475`, the fault net `:476-526`) and `AwaitRelationLock`
  (`:564-688`). **No counter of waits or waited time exists**: the
  `contention_lock_partition_*` and `contention_lock_wait_for_*` fields
  (`include/kds/base/contention.hpp:43-44`) count latch contention on the
  table's partitions, not lock waits.
- **Checkpoints.** `wal::CheckpointStats{started, completed,
  pages_flushed, steps}` (`include/kds/wal/checkpointer.hpp:280-285`) is
  kept and never printed; its accessor is core 0's alone
  (`include/kds/server/expeditor.hpp:640-642`). `SHOW META` prints
  `contention_checkpoint_runs`, `contention_checkpoint_longest_us` and the
  mount's `recovery_checkpoint_us`. `MountRecovery`
  (`include/kds/server/mount_recovery.hpp:84-203`) is memory-resident.
  Nothing records one run against another.

### 1.5 The rule a persisted statistic falls under

`[source-read]` `docs/rules/rules.md` CR6 (line 67): a catalog relation
whose content is only a statistic is written **unlogged**, never redone or
undone, and **discarded** by a mount that finds it damaged. It is the
**sole exception** to logged catalog writes; `sys.access_stats` is the
only relation that qualifies, and **adding a second requires showing it
meets the same test** - the content's class, not its cost: *"a relation a
reader may act on is not a statistic"*. `sys.access_stats` is written
under its root page held exclusive for the whole call
(`catalog.cpp:2926-3029`), unlogged (`:2989-2997`), and reset at mount if
damaged (`ResetAccessStatsIfDamaged`, `:3247-3291`). A failed write is
counted in `AccessStatsCounters::write_failures`
(`include/kds/stats/access_stats.hpp:48-51`) and fails no statement;
nothing prints the counter.

### 1.6 What a persisted relation costs the format

`[source-read]` A new `sys` relation takes an oid (134 is the next; 133
is retired, `include/kds/catalog/well_known.hpp:246-248`) and a fixed
root page. Pages 4-9 and 11-14 are taken and 10 and 15 retired
(`:334-354`, `:380-382`); `kCatalogOverflowFirst = 16` (`:437`) must move
past a new root, as `:460-462`'s assertion message says. That is a
superblock bump from 21 (`include/kds/server/superblock.hpp:266`), and
every bump since AY-S8 has refused the version before it. A
memory-resident view costs no format change.

---

## 2. Rulings

### BK-R1 — Four relations (W2, W3)

BK adds `sys.statement_stats`, `sys.relation_stats`, `sys.lock_waits` and
`sys.checkpoints`. `sys.sessions` is not part of BK.

### BK-R2 — `SHOW` is sugar over `SELECT` (W3)

Restated as four rules, which BK-S8 lands in the spec that owns the
`sys` views:

- **SV1 — one `SELECT`, one view.** Every `SHOW <target> [<argument>]`
  names exactly one view `sys.<v>` and is rewritten at parse into one
  `SELECT` over it: a fixed column list (or `*`) and, where the target
  takes an argument, a fixed `WHERE` on the view's key column. It runs
  through the `SELECT` path and no other.
- **SV2 — the same result.** The `SHOW` spelling and the `SELECT` it is
  rewritten into answer with the same rows, the same columns in the same
  order and the same values, and refuse with the same code at the same
  byte position.
- **SV3 — nothing only `SHOW` can read.** No value is reported by a
  `SHOW` that `SELECT` over a view cannot read. A counter a `SHOW` prints
  today is a column of a view after BK, or it is deleted.
- **SV4 — a view is the only place the data logic lives.** A `SHOW`
  target is added by adding a view and one rewrite entry; no `SHOW`
  handler reads a source. The handlers of §1.1 are deleted as their
  targets move.

How a reply is rendered under SV2 is BK-Q1; what a view filters is BK-Q2.

---

## 3. The four relations — CLA's proposal `[design]`

Common to all four: each is a view in the registry of §1.2 from the
stage that builds it; its columns are typed (BK-Q3); its timestamps are
UTC wall-clock nanoseconds, because an operator correlates these rows
with events outside the engine (the logical `last_seen` of
`sys.access_stats` and `sys.patterns` is not reused); a write into any of
them **never fails, delays or reorders the statement or run it
describes** - a failed or dropped write is counted, and the count is a
column of `sys.meta` (SV3).

### 3.1 `sys.statement_stats` — what a statement shape costs

- **Answers:** which shapes run most, cost most in total, and cost most
  per call; which are refused and how.
- **Key:** `(fetch_id, kind)`, where `kind` is the statement class
  (`SELECT`, `INSERT`, `UPDATE`, `DELETE`, `PURGE`, DDL). BK-Q6.
- **Columns:** `fetch_id`, `kind`, `calls`, `ok`, `refused_conflict`
  (`TxnConflict`, deadlock, fault net), `refused_other`, `rows_returned`,
  `rows_written`, `pages_touched` (from `ExecStats`), `total_ns`,
  `max_ns`, `first_seen`, `last_seen`, and `shape_text`, the first
  statement seen of the shape with its literals masked, cut to a fixed
  width.
- **Writer:** each core accumulates its own statements in memory and
  folds them into the relation on its own tick, under the relation's
  root page held exclusive - the `sys.access_stats` shape (CC13) at a
  tick's rate instead of a statement's, because a per-statement hold of
  one page from every core is the contention point this table exists to
  find. The view therefore lags by at most one tick, stated in the spec.
- **Bound:** `kMaxStatementShapes` rows (4096 proposed, `access_stats`'
  number); a new shape past it is dropped and counted (BK-Q7).
- **Persistence and CR6:** persisted, unlogged (BK-Q5). The test: no
  engine reader acts on it - the relayout planner reads
  `sys.access_stats` and Waystone reads `sys.patterns`, and nothing would
  read this - so its content is
  only a statistic. If a reader ever acts on it, it leaves CR6.
- **Switch:** `statement_statistics`, a new key, since no existing
  setting expresses this quantity (BK-Q8).

### 3.2 `sys.relation_stats` — a relation's activity and budget

- **Answers:** how much each relation is written, and how much of its
  40-bit id space is spent.
- **Key:** `oid`; `name` and `namespace` are resolved at read, because a
  view cannot be joined (§1.2).
- **Exact columns, derived at read:** `next_id`, `ids_issued`,
  `ids_remaining`, `budget_used`, `budget_warning`, `budget_exhausted`,
  through the same `BudgetOf` as today. `SHOW BUDGET` becomes a projection
  of this view (SV1).
- **Statistic columns:** `inserts`, `updates`, `deletes`, `purged`,
  `refused`, `leaf_splits`, `var_heap_spills`, and `stats_since`, the time
  the counters started from (a format, a discarded damaged page, or a
  reset). The counters say *since `stats_since`* and nothing more.
- **Writer:** the same per-core accumulation and tick fold as §3.1.
- **Not in BK:** page counts and delete-marked slot counts, which would
  have to be exact to be acted on (a `PURGE` decision) and therefore could
  not be a statistic under CR6 (BK-Q13).
- **Persistence and CR6:** the statistic columns persisted, unlogged; the
  exact columns are never stored.

### 3.3 `sys.lock_waits` — every lock wait, one row

- **Answers:** what waits on what, for how long, and how it ends -
  including the costs `docs/spec/foreign-keys.md` §2c states and nothing
  measures.
- **Row:** one per wait that parked, written when it ends: `seq`,
  `core`, `started`, `waited_ns`, `unit`, `mode`, `rel_oid`, `key` (the
  slice or tuple id, 0 for a relation unit), `waiter_trx`, `holder_trx`
  (the refusing holder at the park), `fetch_id` of the waiting statement,
  and `outcome`: `granted`, `deadlock_victim`, `fault_net`, `cancelled`.
- **Writer:** the two park sites of §1.4, at the wait's end, into one ring
  for the instance under a latch of its own. A path that polls with no
  slot behind it (`docs/inflight/known-gaps.md`, Locks) is not a wait here
  and the spec names each one.
- **Bound:** a ring of `kLockWaitRingRows` (4096 proposed), the oldest
  overwritten, overwrites counted.
- **Persistence:** **memory-resident.** The lock family persists nothing
  (`docs/spec/txn.md` §5), and writing a catalog page at the end of every
  wait would add a hold exactly where the instance is already waiting. A
  restart empties it, and the spec says so. CR6 is not invoked.

### 3.4 `sys.checkpoints` — the history of checkpoint runs

- **Answers:** how long each run took, what it wrote, where it moved the
  redo start, and which runs the gate skipped - across restarts, so the
  runs before a crash can be read after it.
- **Row:** one per run, written after its anchor publish or its skip:
  `seq`, `core`, `kind` (`periodic`, `shutdown`, `mount`), `started`,
  `duration_ns`, `steps`, `pages_flushed`, `redo_start_lsn` (the durable
  redo start after the publish), `outcome` (`published`, `skipped`,
  `failed`), and `wal_first_segment` at the run's end.
- **Writer:** the checkpointer, after `Complete()` or the gate's skip,
  under the relation's root page held exclusive. The row of run `k` lands
  on a page that run `k + 1` writes back, so a row is durable one run
  later; the spec says so.
- **Bound:** a ring of `kCheckpointHistoryRows` (1024 proposed) in the
  relation, a row's slot being `seq mod N`.
- **Persistence and CR6:** persisted, unlogged (BK-Q5). The test: no
  reader acts on it - recovery reads the anchor and the log, never this.
  The mount's recovery block stays in `sys.meta`, which is where `SHOW
  META` prints it today.

### 3.5 The views the existing `SHOW` targets need (SV1)

| `SHOW` spelling | rewritten into |
|---|---|
| `META` | `SELECT * FROM sys.meta` (BK-Q4) |
| `TABLES` | `SELECT * FROM sys.tables` |
| `NAMESPACES` | `SELECT * FROM sys.namespaces` (new) |
| `PAGE <id> [VALUES]` | `SELECT <cols> FROM sys.page_slots WHERE page_id = <id>`; `VALUES` adds the payload column (BK-Q12) |
| `PATTERNS` | `SELECT * FROM sys.patterns`; the view's rendering is the one kept |
| `ACCESS` | `SELECT * FROM sys.access_stats` (new view over the existing relation) |
| `BUDGET` | `SELECT oid, name, next_id, ids_issued, ids_remaining, budget_used, budget_warning, budget_exhausted FROM sys.relation_stats`; the summary line becomes a query the operator writes (BK-Q3) |
| `CABINS` | `SELECT * FROM sys.cabins` (new view: the relation and the store's counters) |
| `INDEXES` | `SELECT * FROM sys.indexes` (new view) |
| `FKEYS` | `SELECT * FROM sys.fkeys` (new view) |
| `ASSERTIONS` | `SELECT * FROM sys.assertions` (new view: the relation and the enforcer's counters) |
| `RELAYOUT [<table>]` | `SELECT * FROM sys.relayout [WHERE name = <table>]` (new view, planned at read) |
| `CABIN_OPTIMIZER` | `SELECT * FROM sys.cabin_optimizer` (new view) |
| `TRACES`, `TRACE <id>` | BK-Q9 |
| `DESCRIBE <table>` | BK-Q11 |

The four relations of §3.1-§3.4 get no `SHOW` spelling (BK-Q10).

---

## 4. Stages

- **BK-S0** — this order, `raft-marks-2026-10-11.md` and `index.md`'s BK
  row. Its `critics-developer` review, then BK-Q0..Q13 put to the
  operator.
- **BK-S1** — red first: a contract suite, `show_sugar`, that runs every
  `SHOW` spelling and the `SELECT` it is rewritten into on one instance
  and compares the two replies byte for byte (SV2), failing for every
  target at the start. Then the view registry gains typed columns (BK-Q3)
  and the filter class of each view (BK-Q2).
- **BK-S2** — the rewrite at parse, and the existing targets moved onto
  views one at a time, each handler deleted as its target turns green in
  `show_sugar` (SV4). `sys.meta` last, as the largest. Every test and
  tool that parses a `SHOW` reply is migrated in the stage that changes
  it (BK-Q1).
- **BK-S3** — `sys.lock_waits`: the ring and the two park sites. No
  format change.
- **BK-S4** — the format, if BK-Q5 is (a): oids and root pages for the
  three persisted relations, `kCatalogOverflowFirst` moved, superblock 22
  refusing 21, the mount's discard of a damaged statistic page
  generalized from `sys.access_stats` to the four, and CR6 amended to
  name them.
- **BK-S5** — `sys.statement_stats`: the per-core accumulator, the tick
  fold, the switch.
- **BK-S6** — `sys.relation_stats`.
- **BK-S7** — `sys.checkpoints` (after BI-S2 if BI is opened first).
- **BK-S8** — the close: the overhead measured once (§6); the spec
  landed - a new `docs/spec/sys-views.md` owning SV1-SV4, every view and
  its filter class, and the four relations; `docs/rules/rules.md` CR6;
  `docs/spec/ddl-transactional.md` §5a; `manual/sql/sql.md` §6
  rewritten; CLAUDE.md's Observability row flipped.

Each stage runs the full suite and gets its own `critics-developer`
review, per the Session Workflow.

---

## 5. Items for the operator

None is marked. The proposal is CLA's.

| item | question | options | CLA proposes |
|---|---|---|---|
| **BK-Q0** | Open BK? | (a) open as written; (b) open with items changed; (c) hold | - |
| **BK-Q1** | How is a `SHOW` reply rendered? | (a) it **is** the `SELECT`'s reply, byte for byte (CSV header and rows); the `key=value` form retires and every parser of it migrates; (b) a renderer keeps `key=value` over the same rows - same data, a second rendering | **(a)**: W3 says only sugar, and a second renderer is a second code path SV2 would have to police |
| **BK-Q2** `[quiet-wrong]` | What does each view filter? | (a) §5a's line applied to views: schema-object views (`tables`, `objects`, `columns`, `namespaces`, `indexes`, `fkeys`) answer under the session's view, engine-state views do not; (b) none filters, and `SHOW TABLES`/`INDEXES`/`NAMESPACES` lose their filter; (c) every view filters | **(a)**: (b) removes the filter from three surfaces that have it, making §1.3's disagreement the rule; (c) hides an uncommitted relation from the diagnostics that exist to show what the instance holds |
| **BK-Q3** | Do views get typed columns, admitting `ORDER BY` and aggregation? | (a) yes, both admitted; a join stays refused; (b) no, AG12 and the `ORDER BY` refusal stand | **(a)**: the refusal's stated reason is that a view has no schema to resolve a key against (`catalog_view.hpp`); a typed column list is that schema, and `ORDER BY total_ns DESC` is the first query anyone runs on §3.1 |
| **BK-Q4** | `sys.meta`'s shape | (a) tall: `(core, section, field, value)`, one row per field per core, every core; (b) wide: one row per core, one column per field | **(a)**: the field list changes often and a tall row needs no schema change for it; (a) also makes `SHOW META` report every core, where today it reports the session's own. Reading a peer's counters needs each to be readable from another core, which the survey did not check field by field |
| **BK-Q5** | What is persisted? | (a) `statement_stats`, `relation_stats`' statistic columns and `checkpoints` persisted unlogged under CR6, superblock 22 refusing 21; `lock_waits` memory-resident; (b) all four memory-resident, no format change, all history lost at a restart | **(a)**: the history most worth reading is the one before a crash |
| **BK-Q6** | A statement's identity in §3.1 | (a) `fetch_id` with `kind`, so one key names a shape across `sys.patterns`, its trails and this; (b) `pattern_id`, which also splits by select list | **(a)** |
| **BK-Q7** | Bounds and saturation | (a) 4096 shapes, new ones dropped and counted; rings of 4096 lock waits and 1024 checkpoint runs, overwritten and counted; (b) other numbers | **(a)** |
| **BK-Q8** | Switches | (a) `statement_statistics`, default on; `lock_waits` and `checkpoints` always on (a wait already parks; a run is rare); (b) one switch for all four | **(a)** |
| **BK-Q9** | `SHOW TRACES` / `SHOW TRACE` / `TRACE ON` | (a) retired with the sink, since nothing installs one (§1.1); (b) the sink installed and two views, `sys.traces` and `sys.trace_spans` | **(a)**: a surface that has answered `ERR` since it was wired off is not one anyone reads; §3.1 answers most of what a trace was for |
| **BK-Q10** | `SHOW` spellings for the four relations | (a) none: they are read by `SELECT`; (b) `SHOW STATEMENTS`, `LOCK_WAITS`, `CHECKPOINTS` | **(a)**: sugar adds a name and nothing else |
| **BK-Q11** | `DESCRIBE` | (a) in scope: `SELECT * FROM sys.columns WHERE table_name = <t>`, its table-level fields moving to `sys.tables` and `sys.relation_stats`; (b) out of scope, not being a `SHOW` | **(a)**: W3 says keywords *such as* these, and `DESCRIBE` is the same kind of surface |
| **BK-Q12** | `SHOW PAGE` | (a) `sys.page_slots`, one row per slot with the page header repeated on each, refusing a read with no `page_id = <id>`; (b) `SHOW PAGE` kept as a named exception, a debugging surface over raw bytes | **(a)** |
| **BK-Q13** | `relation_stats`' page and delete-marked counts | (a) not in BK; (b) maintained exactly and logged, outside CR6; (c) filled by an explicit walk statement | **(a)**: (b) puts a logged write on every data write, and (c) needs a statement nobody has asked for |

---

## 6. Measurement

The overhead is measured once, at BK-S8, per the Session Workflow:
interleaved A/B in `build-release`, server CPU per statement, the commit
BK opened from against the commit that closes it, and
`statement_statistics` on against off at the close. The shapes are those
of the last close (`bench/v3.0.0/`), single-row point reads and writes
first, because §3.1's two clock reads and one table update per statement
are a fixed cost that shows most on the cheapest statement. A stage
lands with "overhead not measured; measured at the milestone's close".

---

## 7. Row status

### BK-S0 — written 2026-10-11

Written in the main checkout on `main` at `ab2c46d0`
(`v2.7.0-771-gab2c46d0`) and pushed to `main` on the operator's W0 and
W3, with the pre-push hook skipped because the change is documents only
(`scripts/githooks/pre-push`'s own exemption); no build or suite ran.
**Not reviewed**: the `critics-developer` review of this order is owed.
**Not opened**: BK-Q0 is unmarked. The survey (§1) is read, not run;
§1.3's filter split in particular is a reading of three call sites and
has no reproduction yet.
