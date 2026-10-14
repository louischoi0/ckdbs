# Work order BO — four statistics relations, and `SHOW` as sugar over `SELECT`

**Lettered BK at its writing and relettered BO on the operator's word of
2026-10-11, landed 2026-10-12** (`raft-marks-2026-10-11-keystone-48.md` §4), when three
orders of the day were found to carry BK: `workorder-bk-keystone-48.md`,
written first, keeps the letter; `workorder-bn-opt-read-update-path.md` is
BN. This order's items read BO-*, and its marks stay in
`raft-marks-2026-10-11.md` §1-§3 under the new letter.

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

**Opened 2026-10-13** on the operator's word *"Open the semantic sugar
milestone."* (`raft-marks-2026-10-13.md` §10), in the worktree
`bo-show-sugar` from `cd7c7c4e` (`origin/main`). BO-S0's owed
`critics-developer` review ran at the opening and found twelve defects in
§3 and §5 and four §1 claims no longer true; its corrections are applied
below and listed in §7 ("BO opened"). **W4, the same day**, is the
operator's own mark on BO-Q1 and BO-Q3: *"Every sys-related table must be
output in the form of an ordinary table."* (`raft-marks-2026-10-13.md`
§11), restated as BO-R3. **W5, the same day, is the standing go-ahead**
(`raft-marks-2026-10-13.md` §12, `go-ahead-achieving-milestone`): BO runs
to its close in this session, BO-Q2 and Q4..Q13 adopted as CLA proposed -
several revised against `cd7c7c4e` first - and recorded in §5's mark
column as adopted, never as the operator's marks.

**Claim tags:**
- `[source-read]` with `path:line` at `ab2c46d0` in §1.1-§1.6 and at
  `cd7c7c4e` in §1.7 and below, read and not run;
- `[design]` for CLA's reasoning about code not yet written;
- `[measured]` with the invocation (none in this order);
- `[quiet-wrong]` marks an item where a wrong choice turns a refusal or a
  correct answer into a wrong one with no error.

The operator's words, and a rule that restates them, are decisions rather
than claims, so they carry no tag.

**The letter.** `[source-read]` BJ was the last order on `main` at
`ab2c46d0`; BO was the next free letter then, and the relettering of
2026-10-11 confirmed it.

**BO against other orders.** At `cd7c7c4e`, BU (`workorder-bu-basic-query-surface.md`)
and the `sysview-strict-compare` change both edited `HandleCatalogView`:
a view's columns carry declared types and its WHERE literals are coerced
and compared under them, and a boolean conjunct over a view is refused
`NotImplemented`. **BI** is closed; its S2 rule - a checkpoint step writes
no map and allocates no page it does not write - binds §3.4, which is
written to it (no catalog page from the checkpointer). **BR** discards
`sys.access_stats` at a restore's mount; BO adds no persisted relation
(BO-Q5 as revised), so nothing more is discarded. Not part of AR0 §8's
chain.

---

## 0. What BO is

Two things, one dependent on the other.

1. **`SHOW` becomes sugar** (BO-R2). Every `SHOW` target is one `SELECT`
   over one `sys` view, rewritten at parse and run through the `SELECT`
   path, so nothing an operator can read through `SHOW` is out of reach
   of `SELECT`, `WHERE` and `LIMIT`. Today that holds for none of the 15
   targets (§1.1).
2. **Four new relations** (BO-R1): `sys.statement_stats`,
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
  The surface is reachable and does nothing (BO-Q9).
- **`manual/sql/sql.md:975` states `catalog_recovered=0`; the code writes
  `catalog_recovered=1`** (`command_dispatcher.cpp:1810`). The manual's
  introspection table (`sql.md:23`, `:975-990`) also omits `TRACES`,
  `TRACE` and `CABIN_OPTIMIZER`. BO-S8 rewrites the section, so both are
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
names the one that answers differently as the leak. BO-R2 cannot hold
while the two disagree, so BO must pick one answer for every view
(BO-Q2).

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

### 1.7 Re-read at `cd7c7c4e` (the opening review)

`[source-read]` §1.1-§1.6 were read at `ab2c46d0`; the opening review
re-read every claim on `bo-show-sugar` at `cd7c7c4e`. Every line number
above has moved; the claims hold except these four, which are corrected
here and govern where they disagree:

- **A view's columns are typed.** `CatalogView::column_types`
  (`include/kds/exec/catalog_view.hpp`) declares `uint64` or `varchar` per
  column, and a WHERE literal is coerced and compared under it
  (`src/server/command_dispatcher.cpp:6903-6961`). The reply still renders
  untyped, comma-joined, as one KWP `TEXT` column named `line`
  (`src/server/kwp_session.cpp:802-848`), so **a string holding a comma is
  ambiguous** - harmless for today's identifier columns and wrong for
  every string column §3 adds.
- **`TouchPattern` is deleted** (BL-S4); `sys.patterns`' `use_count` and
  `last_seen` are frozen at 0, and `SHOW PATTERNS` prints them as 0.
- **A checkpoint run is recorded, not kept**: BI's `CheckpointRunReport`
  (`include/kds/sched/checkpoint_census.hpp:60-86`) carries each run's
  start and end, steps, pages, `dirty_at_start`, `resumed`, `completed`
  and its park times, and is logged and dropped.
- **The superblock is 23** (`include/kds/server/superblock.hpp:139`).

Three facts §1 did not state:

- **Every catalog relation overflows into one shared range**, pages
  16..127 (`AllocateCatalogPage`, `src/catalog/catalog.cpp:352-361`); a
  full range refuses every catalog relation's growth `OutOfSpace`, and a
  4096-shape persisted `statement_stats` alone would need about 120 of its
  112 pages. This is why BO-Q5 is revised to memory-resident.
- **Only `select`, `insert` and `update` carry a fingerprint**
  (`src/parser/fingerprint.cpp:130-131`); `DELETE`, `PURGE` and DDL have no
  `fetch_id`.
- **A restore discards `sys.access_stats`** (`src/server/restore.cpp:328`)
  and a mount resets it when damaged (`src/server/expeditor.cpp:1054`).

---

## 2. Rulings

### BO-R1 — Four relations (W2, W3)

BO adds `sys.statement_stats`, `sys.relation_stats`, `sys.lock_waits` and
`sys.checkpoints`. `sys.sessions` is not part of BO.

### BO-R2 — `SHOW` is sugar over `SELECT` (W3)

Restated as four rules, which BO-S8 lands in the spec that owns the
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
  today is a column of a view after BO, or it is deleted.
- **SV4 — a view is the only place the data logic lives.** A `SHOW`
  target is added by adding a view and one rewrite entry; no `SHOW`
  handler reads a source. The handlers of §1.1 are deleted as their
  targets move.

How a reply is rendered under SV2 is BO-Q1; what a view filters is BO-Q2.

### BO-R3 — A `sys` view answers as an ordinary table (W4)

*"Every sys-related table must be output in the form of an ordinary
table."* Restated:

- **SV5 — one row path.** A `sys` view's result is described and emitted
  through the statement's `ResultSink` exactly as a relation's `SELECT`
  is: a typed field per column, a typed row per row. On KWP that is a
  `S_ROW_DESC` naming every column with its type and typed `S_ROW_BATCH`es,
  never the one `TEXT` column `line`; on the newline protocol it is the
  same header line and rows a relation's `SELECT` prints. A `SHOW` target,
  being one `SELECT` over one view (SV1), answers the same way.
- What does not reach a result set - a completion with no rows, a refusal
  - is unchanged.

`DESCRIBE`, `ANALYZE` and any reply left outside a view keep
`DeliverTextLines` only until the stage that moves them; at BO's close
`ANALYZE` (a plan, not a table) is the one text-lines reply left, and the
spec names it.

---

## 3. The four relations `[design]`

As revised at the opening (§7, "BO opened") and adopted under W5.

Common to all four: each is a view in the registry of §1.2 from the
stage that builds it, typed (SV5); **each is memory-resident** (BO-Q5
(b)) - no oid, no root page, no format change, emptied by a restart and
by a restore, and the spec says so; a point in time is a `TIMESTAMP`
(UTC microseconds, `docs/spec/types.md` TY4) and a duration an integer
`*_us`, so `WHERE started > '...'` works and no second time
representation enters; a write into any of them **never fails, delays or
reorders the statement or run it describes** - one that cannot be kept is
dropped and counted, and the count is a `sys.meta` field (SV3). None is a
statistic a reader acts on, so none can change a result: a wrong or lost
value costs a reader's insight, never an answer.

**Where the rows live.** Each core keeps its own part in memory, guarded
by a latch of the part's own: the core that owns it takes the latch
uncontended per write, and a view's builder, on whatever core the
`SELECT` runs, takes each core's latch in turn to copy its rows. No core
reads another's unguarded words (the opening review's finding 12), and no
cross-core write is added to a statement's path.

### 3.1 `sys.statement_stats` — what a statement shape costs

- **Answers:** which shapes run most, cost most in total and per call,
  and which are refused.
- **Key:** `(fetch_id, kind)`. `kind` is the statement's first keyword
  (`SELECT`, `INSERT`, `UPDATE`, `DELETE`, `PURGE`, `CREATE`, ...); a kind
  the parser gives no fingerprint (§1.7) is one row per kind with
  `fetch_id = 0` (BO-Q6 as revised).
- **Columns:** `fetch_id`, `kind`, `calls`, `refused`, `total_us`,
  `max_us`, `first_seen`, `last_seen`, `shape_text` - the first statement
  of the shape seen, cut to 256 bytes. A statement the engine re-runs
  (BA-S14's structural re-run, a `PURGE` retry) is one call, timed whole.
  Rows returned and pages touched are not columns: no one source counts
  them across the walk, the descent and the fold, and two sources would be
  a column that disagrees with itself.
- **Bound:** 4096 shapes per core; a new shape past it is dropped and
  counted.
- **Writer:** the dispatcher, once per statement, at its end.
- **No switch** (BO-Q8 as revised): a second key beside
  `access_statistics` would name a neighbouring quantity, and the close
  measures what the always-on recording costs; a cost that needs a switch
  goes back to the operator as a new item.

### 3.2 `sys.relation_stats` — a relation's activity and budget

- **Key:** `oid`, with `name`; a relation no longer in the catalog (a
  dropped one) has no row - its counters are dropped at read.
- **Exact columns, derived at read:** `next_id`, `ids_issued`,
  `ids_remaining`, `budget_used`, `budget_warning`, `budget_exhausted`,
  through the same `BudgetOf` as `SHOW BUDGET` today (48-bit, BK).
- **Statistic columns:** `inserts`, `updates`, `deletes`, `purged` (rows,
  since the instance started). Leaf splits, var-heap spills, page counts
  and delete-marked slots are not in BO (BO-Q13): each is a hook in the
  storage layer for a number no reader has asked for.
- **Writer:** the dispatcher, from the rows a write statement reports.

### 3.3 `sys.lock_waits` — every lock wait, one row

- **Row:** one per wait that parked, written when it ends: `seq`, `core`,
  `started`, `waited_us`, `unit`, `mode`, `rel_oid`, `key` (the slice or
  tuple id, 0 for a relation unit), `waiter_trx`, `holder_trx`, and
  `outcome`: `granted`, `deadlock`, `fault_net`, `cancelled`.
- **Writer:** the two park sites (`AwaitWriteBlock`, `AwaitRelationLock`),
  at the wait's end. A path that polls with no slot behind it
  (`known-gaps.md`, Locks) is not a wait here and the spec names it.
- **Bound:** a ring of 4096 per core, the oldest overwritten and counted.

### 3.4 `sys.checkpoints` — the recent checkpoint runs

- **Row:** a projection of BI's `CheckpointRunReport`, one per run, kept
  when the run ends: `seq`, `core`, `started`, `duration_us`, `steps`,
  `pages_flushed`, `dirty_at_start`, `log_wait_us`, `data_wait_us`, and
  `outcome`: `completed`, `resumed`, `failed`.
- **Writer:** the checkpointer hands its finished report to its core's
  ring and writes nothing else - no catalog page, per BI-S2's rule.
- **Bound:** a ring of 1024 per core.
- **Not across a restart**: the runs before a crash are gone with the
  process (BO-Q5 (b)); the mount's recovery block stays in `sys.meta`.

### 3.5 The views the existing `SHOW` targets need (SV1)

| `SHOW` spelling | answered by |
|---|---|
| `META` | `SELECT * FROM sys.meta`: `(section, field, value)`, `value` varchar, the session's core (BO-Q4) |
| `TABLES` | `sys.tables`, filtered (BO-Q2) |
| `NAMESPACES` | `sys.namespaces` (new), filtered |
| `PAGE <id> [VALUES]` | `sys.page_slots WHERE page_id = <id>`, a required key (BO-Q12) |
| `PATTERNS` | `sys.patterns`; the view's rendering is the one kept |
| `ACCESS` | `sys.access_stats` (new view over the existing relation) |
| `BUDGET` | the budget columns of `sys.relation_stats`; the summary line is deleted (aggregation over a view stays refused, BO-Q3) |
| `CABINS` | `sys.cabins` (new) |
| `INDEXES` | `sys.indexes` (new), filtered |
| `FKEYS` | `sys.fkeys` (new), filtered - **a filter `SHOW FKEYS` does not have today** |
| `ASSERTIONS` | `sys.assertions` (new) |
| `RELAYOUT [<table>]` | `sys.relayout`, `[WHERE name = <table>]` |
| `CABIN_OPTIMIZER` | `sys.cabin_optimizer` (new) |
| `TRACES`, `TRACE <id>`, `TRACE ON` | retired with the trace sink (BO-Q9) |
| `DESCRIBE <table>` | `sys.columns WHERE table_name = <table>` (BO-Q11) |

**A required key.** `sys.page_slots` cannot be materialized whole (it
would read every page of the volume), and `RELAYOUT <t>` walks the named
relation, which the bare form does not; an unknown name is refused today.
So a view may declare a **required key**: its builder receives the
conjunct `key = <literal>` and refuses a `SELECT` without one
`InvalidArgument` naming the key, and refuses an unknown key the way the
`SHOW` does today - never zero rows (`[quiet-wrong]` otherwise: a refusal
turned into an empty answer). `sys.relayout` with no key plans every
relation without the walk, as the bare `SHOW RELAYOUT` does.

**The `SHOW` path builds the `SELECT`'s AST, not its text.** The
argument token keeps the byte offset the client wrote, so a refusal on it
names the byte in the statement the client sent (SV2), and the AST goes
to `HandleCatalogView` - the function the `SELECT` path calls.

The four relations of §3.1-§3.4 get no `SHOW` spelling (BO-Q10).

---

## 4. Stages

As revised at the opening.

- **BO-S0** — this order, the marks and `index.md`'s row; the opening
  review (§7). Done at the opening.
- **BO-S1** — red first: the `show_sugar` contract suite - every `SHOW`
  spelling against its `SELECT` on one instance, compared row for row
  through both the newline protocol and KWP (SV2, SV5) - and cells that a
  view answers as a typed result set. Then SV5 for the existing views:
  `HandleCatalogView` emits through the `ResultSink`; `ORDER BY` over a
  view, compared under the declared types with `exec::CompareValues`;
  the filter class of each view (BO-Q2); the required key.
- **BO-S2** — the existing targets moved onto views one at a time, each
  handler deleted as its target turns green (SV4), `sys.meta` last; every
  test and tool that parses a `SHOW` reply migrated in the step that
  changes it; the trace sink retired (BO-Q9); `DESCRIBE` moved (BO-Q11).
- **BO-S3** — `sys.lock_waits`.
- **BO-S4** — **not run**: it was the format change for persisted
  statistics, and BO-Q5 is (b).
- **BO-S5** — `sys.statement_stats`.
- **BO-S6** — `sys.relation_stats`.
- **BO-S7** — `sys.checkpoints`.
- **BO-S8** — the close: the overhead measured once (§6); a new
  `docs/spec/sys-views.md` owning SV1-SV5, every view, its filter class
  and its required key; `docs/spec/ddl-transactional.md` §5a naming
  `SHOW FKEYS`, `SHOW NAMESPACES` and the views; `manual/sql/sql.md`'s
  introspection section rewritten (its `catalog_recovered` and missing
  targets with it); CLAUDE.md's Observability row flipped.

Each stage runs the full suite and gets its own `critics-developer`
review, per the Session Workflow.

---

## 5. Items

BO-Q0 is the operator's mark (W, `raft-marks-2026-10-13.md` §10), BO-Q1
and BO-Q3 the operator's W4 (§11); every other mark is **adopted on the
operator's standing go-ahead of 2026-10-13 (go-ahead-achieving-milestone)**
(§12), with the revision made against `cd7c7c4e` named.

| item | question | options | CLA proposes | mark |
|---|---|---|---|---|
| **BO-Q0** | Open BO? | (a) as written; (b) with items changed; (c) hold | - | **(a)**, operator, 2026-10-13 |
| **BO-Q1** | How is a `SHOW` reply rendered? | (a) it **is** the `SELECT`'s reply; (b) a renderer keeps `key=value` | (a), as an ordinary table's result set: typed on KWP, header and rows on the newline protocol (revised: the CSV-in-`TEXT` form is ambiguous on a comma, §1.7) | **(a)**, operator's W4 |
| **BO-Q2** `[quiet-wrong]` | What does each view filter? | (a) §5a's line: schema-object views (`tables`, `objects`, `columns`, `namespaces`, `indexes`, `fkeys`) under the session's view, engine-state views not; (b) none; (c) all | (a); premise re-read at `cd7c7c4e`: the views still call `ListTables()` with no view and `HandleCatalogView` takes no session, so the split stands. `SHOW FKEYS` gains a filter it lacks today, stated as a change | **(a)**, adopted |
| **BO-Q3** | Typed columns, `ORDER BY`, aggregation over a view? | (a) all admitted; (b) none | revised: typed (done) and `ORDER BY` admitted through `exec::CompareValues`; aggregation stays refused (AG12): a fold needs a compiled `AggregateSpec` over a schema, a view has none, and a second fold is what AG12 refuses | **typed and `ORDER BY`**, operator's W4 ("an ordinary table") read with the revision; aggregation adopted as refused |
| **BO-Q4** | `sys.meta`'s shape | (a) tall `(section, field, value)`; (b) wide | (a), revised: the session's own core only, as `SHOW META` is today - every core needs a field-by-field census of what is readable across cores, which is not in BO (`known-gaps.md`) | **(a)** as revised, adopted |
| **BO-Q5** | What is persisted? | (a) three relations unlogged, superblock 24; (b) all four memory-resident | revised to **(b)**: (a) puts three growing relations in the 112-page catalog range every relation shares (§1.7), has the checkpointer write a catalog page BI-S2 rules out, and is discarded by a restore anyway | **(b)**, adopted |
| **BO-Q6** | A statement's identity | (a) `(fetch_id, kind)`; (b) `pattern_id` | (a), revised: a kind with no fingerprint is one row per kind at `fetch_id = 0` | **(a)** as revised, adopted |
| **BO-Q7** | Bounds | (a) 4096 shapes, rings of 4096 waits and 1024 runs, overflow counted; (b) other | (a), per core | **(a)**, adopted |
| **BO-Q8** | Switches | (a) a new `statement_statistics`; (b) one switch | revised: **none** - a new key beside `access_statistics` is a parallel knob; the close measures the cost | **none**, adopted |
| **BO-Q9** | `SHOW TRACES` / `TRACE` / `TRACE ON` | (a) retired with the sink; (b) installed | (a): `src/stats/trace.cpp`, `include/kds/stats/trace.hpp`, `tests/trace_test.cpp`, `HandleTrace`, `SetTraceSink`, `traces_` deleted | **(a)**, adopted |
| **BO-Q10** | `SHOW` spellings for the four | (a) none; (b) three | (a) | **(a)**, adopted |
| **BO-Q11** | `DESCRIBE` | (a) in scope; (b) out | (a) | **(a)**, adopted |
| **BO-Q12** | `SHOW PAGE` | (a) `sys.page_slots` with a required key; (b) a named exception | (a), with the required key of §3.5, which `RELAYOUT <t>` needs anyway | **(a)**, adopted |
| **BO-Q13** | page and delete-marked counts | (a) not in BO; (b) logged; (c) a walk | (a) | **(a)**, adopted |

---

## 6. Measurement

The overhead is measured once, at BO-S8: interleaved A/B in
`build-release`, server CPU per statement, `cd7c7c4e` (where BO opened)
against the commit that closes it, inside the hour the standing budget
allows. Single-row point reads and writes first: §3.1's two clock reads
and one latched table update per statement are a fixed cost that shows
most on the cheapest statement (BN measured access statistics at 0.7 % of
a point statement, BN-S6). A stage lands with "overhead not measured;
measured at the milestone's close".

## 7. Row status

### BO-S0 — written 2026-10-11

Written in the main checkout on `main` at `ab2c46d0`
(`v2.7.0-771-gab2c46d0`) and pushed to `main` on the operator's W0 and
W3, with the pre-push hook skipped because the change is documents only
(`scripts/githooks/pre-push`'s own exemption); no build or suite ran.
**Not reviewed**: the `critics-developer` review of this order is owed.
**Not opened**: BO-Q0 is unmarked. The survey (§1) is read, not run;
§1.3's filter split in particular is a reading of three call sites and
has no reproduction yet.


### BO opened — 2026-10-13

BO was opened on the operator's word (`raft-marks-2026-10-13.md` §10) in
the worktree `bo-show-sugar`, from `cd7c7c4e`. BO-S0's owed
`critics-developer` review ran at the opening. It read the order and the
tree and ran nothing.

**Applied:**

- **§1.7.** Four claims are false at `cd7c7c4e`: the views are typed,
  `TouchPattern` is deleted, BI records each checkpoint run, and the
  superblock is 23. Three facts were unstated: the 112-page catalog range
  every relation shares, which statement kinds carry a fingerprint, and
  the restore's discard of `sys.access_stats`.
- **BO-Q5 revised to memory-resident** (findings 1, 3, 4): persisted
  relations would grow in the shared range, would have the checkpointer
  write a catalog page that BI-S2's rule forbids, and are discarded by a
  restore anyway. This deletes BO-S4.
- **The reply format** (finding 2): CSV in one `TEXT` column is ambiguous
  once a string holds a comma. W4 then decided it.
- **BO-Q6's key revised** (finding 5): a kind with no fingerprint is one
  row at `fetch_id = 0`, and a re-run counts as one call.
- **The `SHOW` path builds the AST** (finding 6), so a refusal names the
  byte the client wrote.
- **A required key for `PAGE` and `RELAYOUT <t>`** (finding 7): the
  statement is refused rather than answered with zero rows.
- **Time columns are `TIMESTAMP`** (finding 8).
- **§3's CR6 argument replaced** (finding 9) by "a wrong value costs
  insight, never an answer". Under (b) the point is moot.
- **The tick fold dropped** (finding 10) for a latched per-core part,
  written at the statement's end. This keeps CC13's "in the row before its
  statement returns", and does not bring back CR7's retired batch.
- **`SHOW FKEYS` gaining a filter** is stated as a change (finding 11).
- **BO-Q4 held to the session's core** (finding 12).
- **BO-Q8's new key withdrawn** as a parallel knob.
- **BO-Q9's deletions named.**
- **Stale text corrected**: the 40-bit id space, the superblock number,
  and BI's state.

**Rejected:** none. On the review's own reading, aggregation under BO-Q3
stays refused (AG12).

**Not cut, stated:** the migration's size. `SHOW META` is parsed in 15
test files and 24 tools.

### BO-S1 — 2026-10-13

On `bo-show-sugar`, `92c9bc47` to `ac3ec07c`: a `sys` view answers as an
ordinary table (SV5) - a typed `S_ROW_DESC` and typed row batches on KWP -
takes `ORDER BY` under its declared types, and the schema-object views read
under the session's view (BO-Q2 (a)); the `show_sugar` contract suite was
written red first, its SV2 cells red until BO-S2; a `SET` value's subquery
over a view runs as one over a table, and the refusal SV5 made dead is
deleted. Its `critics-developer` review was applied at `ac3ec07c` (a
view-column refusal names its byte; one keyed sort vector and one sink
selection). **Not recorded by that session**: the review's rejected
findings and the suite's pass count; the next session found BO-S2's work
uncommitted on top of it and no handoff.

### BO-S2 — 2026-10-14

On `bo-show-sugar`, `a8d1425e` (engine and tests), `3ac864f3` (tools) and
the review commit after them. Every `SHOW` target and `DESCRIBE` is one
`SELECT` over one view, built as its AST and answered by
`HandleCatalogView` (SV1, SV4); the fifteen handlers and the trace sink
(BO-Q9) are deleted; the views are `src/server/sys_views.cpp`. `sys.meta`
is `(section, field, value)` (BO-Q4), written per section. SV3 carried two
headers no view had: `SHOW CABIN_OPTIMIZER`'s controller and executor
numbers (`cabin_optimizer_*`) and `SHOW CABINS`' store counters
(`cabins_unbankable_views`, `cabins_cap_refusals`), both `sys.meta` fields.
Every test and tool that parsed a `key=value` reply reads the view
(`tests/view_text.hpp`, `tools/sys_meta.py`; the tool reader also reads a
pre-BO engine's line, which an A/B baseline still answers).

**Deleted, by SV3's second clause:** `SHOW BUDGET`'s summary line (BO-Q3:
a filter on `budget_warning` answers it); `SHOW RELAYOUT`'s per-shape lines
(summed into `shapes` and `walk_weight_q8`, the shapes being
`sys.access_stats`' rows); `DESCRIBE`'s `autoincrement=` (always
`if-omitted` on the pk, derivable from `pk`) and `columns=` (the row
count); `SHOW PAGE`'s internal-node `max_entries` (a constant);
`SHOW PATTERNS`' `stale=v<n>` (derivable from `fingerprint_version`).
**Behaviour changed, stated:** `SHOW RELAYOUT` with the optimizer off and
`SHOW CABIN_OPTIMIZER` with no controller are refusals naming the view's
byte, where the first answered a notice; `SHOW PAGE` refuses a page with an
unreadable slot rather than skipping it; `SHOW BUDGET` refuses where a
relation has no `sys.tables` row rather than printing an error row.

**Review** (`critics-developer`, on `a8d1425e`). Applied: a view's key
conjunct re-filtered the rows its builder had already resolved, so a
qualified `DESCRIBE ns.t` and `SHOW RELAYOUT ns.t` answered zero rows
(`[quiet-wrong]`; fixed, two new cells); `DESCRIBE` and a bare `SHOW` named
garbage bytes (fixed); `sys.patterns` decided a directory by its root, not
`HasWaystoneDirectory` (fixed); `DESCRIBE <unknown>` named no byte (fixed);
the two configuration refusals named no byte (fixed); a rate over no
lookups, an empty covering list and an unresolvable relation name read 0,
`''` and `oid=N` (now NULL); every view took the session's read view, which
ends a statement boundary under an open DDL (now only the schema-object
views); a dead `Spelling` struct; two tests reading by position or by a
substring the header also holds. **Rejected:** `Unsupported` for the two
configuration refusals - the architecture admits both surfaces, this
instance is configured without them, so `InvalidArgument` stays;
`MetaSections` pushing triples instead of a `key=value` line its view
parses back - the largest cut on offer, but the backup and archive blocks
write the same line through `AppendMeta(std::ostream&)` and share it with
`BACKUP STATUS`, so it is a change to those interfaces, not to this stage,
and is left as named.

Suite: 3759/3759 (`ctest -LE heap-suspended`, Debug). Overhead not
measured; measured at the milestone's close.

### BO-S3 — 2026-10-14

On `bo-show-sugar`, `5c086e29` and its review commit. `sys.lock_waits` as
§3.3 designs it, memory-resident (BO-Q5 (b)), a ring of 4096 per core
(BO-Q7 (a)) in `stats::LockWaitLog`, written by `AwaitWriteBlock` and
`AwaitRelationLock` through an RAII row whose destructor writes
`cancelled`; `sys.meta`'s `lock_waits` section carries recorded,
overwritten and dropped. **Decided in the building, stated:** a waiter
refused at the edge for closing a cycle is a `deadlock` row with
`waited_us` 0, although it never parked - §3.3 lists the outcome; a slot
already flipped before the park (a stale memo's re-run, an ask granted
after its refusal) is no wait; `unit`, `rel_oid` and `key` name the unit
that refused, where the wait is registered, and `mode` what was asked -
since AY-S2 they can be two units; `granted` means the slot flipped, not
a grant, so a re-run that parks again is a second row; a waiter or holder
that is no transaction reads NULL.

**Review** (`critics-developer`, on `5c086e29`): no engine defect found;
every exit checked, every site's mode checked against its ask, the
cancelled path proved by a throwaway cell. Applied: a vacuous
waiter-differs assertion pinned to the real value (NULL); a comment the
new `sys.meta` block displaced; the deadlock arms follow the park's
flipped-slot rule; the destructor's contract stated as the clock's; the
engine's `LatchGuard` for `std::lock_guard`; the part count tied to
`kMaxCores`; the record packed to 64 bytes; `unit`/`mode` and `granted`
documented as above. **Not applied:** `AwaitRelationLock`'s waiter ignores
`parked_write()` - an identity that predates this stage, inherited by the
row, left for the lock family; `LockWaitRow` as a free function - it reads
the core id and is one declaration; a shared per-core ring template, which
the reviewer itself proposed for when `sys.checkpoints` repeats the shape
(BO-S7).

Suite: 3767/3767 (`ctest -LE heap-suspended`, Debug). Overhead not
measured; measured at the milestone's close.

### BO-S4 — not run

BO-Q5 is (b): nothing is persisted, so the format change this stage was
is not made (§4).

### BO-S5 — 2026-10-14

On `bo-show-sugar`, `4a2b16e9` and its review commit. `sys.statement_stats`
as §3.1 designs it: `stats::StatementStats`, a part per core of up to 4096
shapes merged by `(fetch_id, kind)` at read; a new shape past the bound is
dropped and counted (`statement_stats_dropped`). Recorded once per
statement at its end by `Dispatch` and `DispatchAsync`, timed whole (a
park, a re-run and the group commit are one call); the `fetch_id` is the
parse's own - `SELECT`'s parser, `INSERT`'s and `UPDATE`'s through
`ParseKeepingShape` - so no statement is lexed twice, the first parse of a
statement keeping the key a nested `SELECT` would otherwise take.
**Decided in the building, stated:** a refused statement is a call and
counts in `refused`; a frame destroyed at a park never replied and is no
call; `kind` is the first whitespace-separated token, as `DispatchInner`
routes it, and a token that is no routed verb is `OTHER`, so a client mints
no row per misspelling; **a statement the engine issues itself is not
counted** - a listener's `ROLLBACK` at the lifetime ceiling or a client's
disconnect, a KWP load's own `BEGIN`/`COMMIT`/`ROLLBACK` go through
`DispatchInternal` - and a KWP load's rows, a protocol frame and not a
statement, are no row either; the cost per statement is **three clock
reads** (two monotonic, one wall), not §6's two, and one latched map
update.

**Review** (`critics-developer`, on `4a2b16e9`). Fixed by the review: an
`UPDATE` whose `SET` holds a subquery was keyed by the subquery's
`fetch_id`; `Dispatch`'s refused commit dropped the key; `shape_text`
could end inside a UTF-8 sequence, which a strict client decode refuses -
each with a cell that failed first. Applied after it: the engine's own
statements uncounted (above); `Record` derives the kind itself, so a key's
pointer-hashed kind is always the static one; the kind split as the
dispatcher splits. **Not applied:** one `IEquals` and one list of routed
verbs for the engine (eight copies, four lists) - a refactor across the
parser, exec and server, not this stage's; the per-core part and its
count repeat `LockWaitLog`'s (BO-S3's deferral stands until BO-S7); a
reader copying a part delays that core's next write by the copy - both
stores share it, and the close measures it; an in-process sim restart
keeps the store, which "emptied by a restart" means of a process.

Suite: 3777/3777 (`ctest -LE heap-suspended`, Debug). Overhead not
measured; measured at the milestone's close.
