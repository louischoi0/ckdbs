# The `sys` views, and `SHOW` as sugar over `SELECT`

**Owns** SV1-SV5, the registry of `sys` views, each view's filter class and
key, the four memory-resident statistics views and `sys.meta`. Built by BO
(`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md`, the work
order that holds the reasoning and the stage logs). The registry is
`include/kds/server/sys_views.hpp`; every builder is a `CommandDispatcher`
member in `src/server/sys_views.cpp`; the `SHOW` rewrite is
`CommandDispatcher::HandleShow`'s table in `src/server/command_dispatcher.cpp`.

## 1. The rules

- **SV1 - one `SELECT`, one view.** Every `SHOW <target> [<argument>]`, and
  `DESCRIBE`/`DESC <t>`, names exactly one view `sys.<v>` and is rewritten
  by the dispatcher into one `SELECT` over it - its AST, built from the
  line with no parse, not its text, so a refusal
  on the argument names the byte the client wrote: a fixed column list (or
  `*`) and, where the target takes an argument, a fixed `WHERE` on the
  view's key. It runs through `HandleCatalogView`, the function a `SELECT
  ... FROM sys.<v>` reaches, and no other.
- **SV2 - the same result.** The `SHOW` spelling and the `SELECT` it stands
  for answer the same rows, the same columns in the same order and the same
  values, and refuse with the same code at the same byte. The `show_sugar`
  contract suite (`tests/show_sugar_contract_test.cpp`) pins it through
  both protocols.
- **SV3 - nothing only `SHOW` can read.** A value a `SHOW` reports is a
  column of a view. A counter with no row of its own is a `sys.meta` field.
- **SV4 - a view is the only place the data logic lives.** A `SHOW` target
  is a view plus one rewrite entry; no `SHOW` handler reads a source.
- **SV5 - a view answers as an ordinary table** (the operator's W4: *"Every
  sys-related table must be output in the form of an ordinary table."*). A
  view's result is described and emitted through the statement's
  `ResultSink` as a relation's `SELECT` is: on KWP a typed `S_ROW_DESC` and
  typed row batches, never one `TEXT` column; on the newline protocol the
  header line and rows a relation's `SELECT` prints. **`ANALYZE` is the one
  text-lines reply left**: a plan, not a table.

## 2. What a view admits

A view is materialized by its builder and then filtered, so it admits less
than a relation:

- **Admitted:** a projection, `*`, a qualified column (`FROM sys.tables AS
  t ... WHERE t.oid = 100`), `WHERE` conjuncts `column op literal` and
  `column BETWEEN literal AND literal`,
  `ORDER BY` under each column's declared type, `LIMIT` and `OFFSET`, and a
  view in a `SET` value's subquery.
- **Typed.** A column is `uint64`, `varchar` or `timestamp` (UTC
  microseconds, `types.md` TY4). A `WHERE` literal is coerced to the
  column's type before any row is read (`types.md` §3.1a): `name = 5`,
  `oid = 'x'`, a negative integer and one past 2^64 - 1 are refused with the
  literal's byte. Integers compare unsigned; `sys.patterns.fetch_id` uses
  the full 64-bit range.
- **NULL** is a value a row does not have - an unreadable index's height, a
  Cabin's counters where no store runs, a relation's name for an oid the
  catalog no longer lists - never a 0 that reads as one.
- **Refused:** aggregation (AG12: a view's rows come from no step chain), a
  join, a subquery predicate, a boolean conjunct (`OR`, `NOT`, an `IN`
  list, `LIKE`, `NotImplemented`), a function call (`NotImplemented`), a
  column-to-column comparison, and `ANALYZE` over a view. A panel that needs
  counts reads the rows and counts them (`tools/ash_panel.py`). A form only the
  query layer reads (`parser-v2.md` I19: a value expression, an alias,
  `DISTINCT`, a set operation) is refused `NotImplemented` at the view's
  byte. The aggregation, join, `ANALYZE`, subquery-predicate and
  column-to-column refusals carry no byte (`known-gaps.md`).

## 3. Filter class (BO-Q2) `[quiet-wrong]`

A view that reports **schema objects** answers under the session's read
view, as `ddl-transactional.md` §5a requires: `tables`, `objects`,
`columns`, `namespaces`, `indexes`, `fkeys` (a foreign key is listed where
its child relation is visible). Every other view reports **engine state**
and reads no session view, so it neither filters nor ends a statement
boundary under an open DDL. A view on the wrong side of the line either
leaks an uncommitted `CREATE TABLE` or hides engine state a diagnostic
needs, with no error.

## 4. A view's key (BO-Q12)

A view may declare one key column (`sys_views.hpp`):

- **`kRequired`** - `sys.page_slots` by `page_id`: it cannot be
  materialized whole (it would read every page), so a `SELECT` without
  `page_id = <literal>` is refused `InvalidArgument` naming the key.
- **`kChecked`** - `sys.columns` by `rel_name`: optional; when written, a
  name that resolves to nothing is refused, never answered with no rows
  (`[quiet-wrong]` otherwise: a refusal turned into an empty answer). A
  bare name takes its namespace from a `namespace = '<literal>'` conjunct
  when there is one, else from the session (NS5).

The builder takes the first key conjunct as a bound on what it reads and
returns only what it names, so that conjunct is not compared again.

## 5. The registry

| view | filter | key | what a row is |
|---|---|---|---|
| `tables` | schema | - | a relation: oid, namespace, name, descriptor and root page ids, `clustered_type` (`HEAP`, `BTREE`, `CHAIN`), `next_id`, spare slots and how many are free |
| `columns` | schema | `rel_name`, checked | a live, visible column of a relation (no dropped column, no heap's hidden key) |
| `objects` | schema | - | a table object of `sys.objects` |
| `types` | engine | - | a registered type |
| `patterns` | engine | - | a `sys.patterns` row - a shape ever opted in to Waystone (`waystone-concpets.md` §4): `oid`, `fetch_id` (decimal), `fingerprint_version`, `stmt_class`, `waystone`, `use_count`, `last_seen`, `waystone_root`, `dir_depth`. `waystone` (1 or 0) replaced `origin` and `pinned` at CF-S1b, and is 1 only for a current-version row with the opt-in bit set, so a lapsed row reads 0 and its `fingerprint_version` says why; `use_count`/`last_seen` frozen at 0 since BL-S4; root and depth NULL where the row has no directory |
| `namespaces` | schema | - | a namespace, `sys` and `public` first |
| `indexes` | schema | - | a secondary index, its keys, covered columns, height and entries |
| `fkeys` | schema | - | a declared foreign key, listed where its child is visible |
| `access_stats` | engine | - | an access shape `(rel, kind, columns)` of `sys.access_stats` |
| `relation_stats` | engine | - | a relation's Keystone budget, its tree's shape, the join planner's prior, and its write activity (§6.2) |
| `cabins` | engine | - | a Cabin and its store counters |
| `assertions` | engine | - | an assertion, whether it is enforcing, and its counters |
| `transitions` | engine | - | a declared Transition, verbatim |
| `cabin_optimizer` | engine | - | a managed entry of the Cabin controller; refused where no controller runs |
| `lock_waits` | engine | - | one lock wait (§6.3) |
| `statement_stats` | engine | - | one statement shape (§6.1) |
| `checkpoints` | engine | - | one checkpoint run (§6.4) |
| `wait_event_names` | engine | - | one wait event of the registry (§6.5) |
| `wait_stats` | engine | - | one event on one core, plus a NULL-core total row per event (§6.6) |
| `wait_histogram` | engine | - | one non-empty log2 bucket of one event on one core (§6.6) |
| `session_waits` | engine | - | one event a session has waited on, cumulative (§6.6) |
| `session_state` | engine | - | one session's statement task as last published (§6.6) |
| `ash` | engine | - | one active-session sample (§6.7) |
| `page_slots` | engine | `page_id`, required | one page's header row, then one row per slot - or, on an internal node, its leftmost child, then one row per entry |
| `meta` | engine | - | one `sys.meta` field: `(section, field, value)`, the session's core's (§7) |

| spelling | answers with |
|---|---|
| `SHOW META` | `SELECT * FROM sys.meta` |
| `SHOW TABLES`, `NAMESPACES`, `PATTERNS`, `ACCESS`, `CABINS`, `INDEXES`, `FKEYS`, `ASSERTIONS`, `TRANSITIONS`, `CABIN_OPTIMIZER` | `SELECT * FROM sys.tables`, `sys.namespaces`, `sys.patterns`, `sys.access_stats`, `sys.cabins`, `sys.indexes`, `sys.fkeys`, `sys.assertions`, `sys.transitions`, `sys.cabin_optimizer` |
| `SHOW BUDGET` | `SELECT oid, namespace, name, next_id, ids_issued, ids_remaining, budget_used_ppm, budget_warning, budget_exhausted FROM sys.relation_stats` |
| `SHOW PAGE <id>` | the header and slot columns of `sys.page_slots WHERE page_id = <id>`; `SHOW PAGE <id> VALUES` adds `value`, the payload in hex |
| `DESCRIBE <t>`, `DESC <t>` | `SELECT * FROM sys.columns WHERE rel_name = <t>` |

`lock_waits`, `statement_stats`, `checkpoints` and `relation_stats`'
activity have no `SHOW` spelling (BO-Q10), and neither do `types`,
`objects` or an unkeyed `columns`. `SHOW TRACES`, `SHOW TRACE` and
`TRACE ON` are retired with the trace sink nothing ever installed (BO-Q9);
an unknown target is refused at its byte.

## 6. The statistics views

**Memory-resident** (BO-Q5 (b)): no oid, no root page, no format change;
emptied by a restart and by a restore. Each keeps **one part per core**
(`stats::kStatsParts`, asserted equal to `kMaxCores`) under a latch of its
own (`LatchKind::kStatsRing`): the core a row describes is its part's only
writer, and a builder on any core copies each part under its latch. **A
write never fails, delays or reorders what it describes**: a row past a
bound or an allocation that fails is dropped and counted in `sys.meta`
(SV3). No row is read by the engine, so none can change a result - a lost
or wrong row costs a reader's insight, never an answer. **No switch**
(BO-Q8): recording is always on. At BO's close it cost 0.35-0.52 us of
server CPU per point statement, inside the measurement's spread
(`bench/v3.3.0/results-bo-close-v3.3.0-23-g86d1fe1a.md`).

### 6.1 `sys.statement_stats` (`stats/statement_stats.hpp`)

One row per `(fetch_id, kind)` for the instance, the cores' parts merged at
read: `calls`, `refused`, `total_us`, `max_us`, `first_seen`, `last_seen`
and `shape_text`, the first statement seen, cut to 256 bytes at a character
boundary. `kind` is the first whitespace-separated token, upper-cased, when
it is a verb the dispatcher routes, else `OTHER`; only `SELECT`, `INSERT` and `UPDATE`
carry a fingerprint, so every other kind is one row at `fetch_id = 0`.
Recorded once per statement at its end, timed whole - a park, a re-run and
the group commit are one call; a refused statement is a call. A statement
the engine issues itself (a listener's `ROLLBACK`, a KWP load's
`BEGIN`/`COMMIT`) and a load chunk are not counted. 4096 shapes per core;
`statement_stats_dropped`.

### 6.2 `sys.relation_stats`' activity (`stats/relation_stats.hpp`)

`inserts`, `updates`, `deletes`, `purged`: the rows a relation's write
statements reported since the instance started, every core's summed. A
statement is counted when it answers, not when its transaction decides -
rows an explicit transaction rolls back, or an autocommit statement whose
deferred commit a fail-stopped log aborts, stay counted. A KWP load chunk's
rows are inserts; a `PURGE`'s retired keys are counted whatever its decide
does (PU6). The budget columns are exact, through `BudgetOf`; `height` and
`leaves` walk the clustered tree; `est_rows` and `generation` are the join
planner's published prior (`plan-stats.md` §6). **The bound, 4096 relations
per core, counts every relation a core has written since the instance
started, dropped ones included** (BO-Q15 (b)): past it, that core's writes
of a relation new to it are dropped, the view under-counts the relation,
and `relation_stats_dropped` above 0 says so.

### 6.3 `sys.lock_waits` (`stats/lock_wait_log.hpp`)

One row per lock wait that parked, written when it ends: `seq`, `core`,
`started`, `waited_us`, `unit`, `mode`, `rel_oid`, `key`, `waiter_trx`,
`holder_trx` and `outcome` (`granted`, `deadlock`, `fault_net`,
`cancelled`). `unit`, `rel_oid` and `key` name the unit that refused, where
the wait is registered; `mode` is what was asked - since AY-S2 they can be
two units. `unit` is one of `relation`, `range`, `tuple`, `index key`
(the slice unit left at CP-S2a); `mode` one of `IS`, `IX`, `S`, `U`, `X`
(`U` since CP-S2b: an `UPDATE`'s version claim, and a parent `DELETE`'s
wait on a child row's writer, whose probe asks `U`, `txn.md` §5). `granted` is a flipped slot, not a grant, so a re-run that parks
again is a second row. A waiter refused at the edge for closing a cycle is
a `deadlock` row with `waited_us` 0. A path that polls a holder with no slot
behind it is no wait here (`known-gaps.md`, Locks). A ring of 4096 per
core; `lock_waits_recorded`, `_overwritten`, `_dropped`.

### 6.4 `sys.checkpoints` (`stats/checkpoint_log.hpp`)

One row per checkpoint run, a projection of BI's `CheckpointRunReport`
handed over by `Checkpointer::EndRun`; the checkpointer writes nothing else
(BI-S2's rule: no catalog page). `seq`, `core`, `started` (the wall instant
at the end less the run's length on the injected clock), `duration_us`,
`steps`, `pages_flushed`, `dirty_at_start`, `log_wait_us`, `data_wait_us`,
`resumed` and `outcome` (`completed`, `failed`). A `resumed` run took up a
failed run's snapshot and took none of its own (`dirty_at_start` 0); it can
itself fail, which is why `resumed` is a column and not an outcome. The
waits are a task run's park times plus a synchronous call's time inside the
phases that wait on the log (the gates, the end record) and on the data
file's sync (a step's, the publish's); a log gate inside the publish's
writeback and a claim wait are inside the phases that hold them and in
neither column. An abandoned task writes no row. The runs before a crash
are gone with the process; the mount's recovery block stays in `sys.meta`.
A ring of 1024 per core; `checkpoints_recorded`, `_overwritten`,
`_dropped`.

### 6.5 `sys.wait_event_names` (`stats/wait_event.hpp`)

The wait-event registry, one row per event (CJ's WE01,
`instructions/v3.2.0/workorder-cj-wait-events-ash.md`): `event_id`, `name`
(`class.event`, the only public contract), `class`, `kind` (`timed`,
`counter`), `in_db_time` (0 for `idle.*` alone) and `p1`-`p3`, what the
event's parameters mean, NULL where it has none. `event_id` is the table
index, internal and not stable across versions; 0 is the reserved
`cpu.on_cpu`. A compile-time table: no ring, no counters, nothing to
overflow. An engine-state view, reading no session view.

### 6.6 Wait statistics (`sched/wait_state.hpp`)

Cumulative, always on, memory only (CJ's WE05, WD4, `docs/spec/wait-events.md`
names every event). A core's counters have one writer, that core, and are
relaxed atomics any core reads without a latch, so a row is a moment's view:
two columns of one row may differ by a wait in flight.

- `wait_stats`: `core`, `event`, `class`, `count`, `total_us`, `max_us`. The
  `cpu.on_cpu` row is the core's ON_CPU time (count 0). `core` is NULL on the
  total rows (the sum over cores, `max_us` the largest). A synchronous read's
  time is the core's stall, not one task's (CJ-Q8).
- `wait_histogram`: `core`, `event`, `bucket`, `lo_us`, `count`; bucket k holds
  waits of [2^k, 2^(k+1)) microseconds, 32 buckets, bucket 0 everything under
  2 us.
- `session_waits`: `session_id`, `event`, `count`, `total_us`; a session's
  `cpu.on_cpu` row is its ON_CPU time and its RUNNABLE time is `sched.run_queue`.
  A session's rows leave with it; a background task is no session.
- `session_state`: `session_id`, `state`, `event`, `p1`-`p3`, `elapsed_us`. A
  session between statements shows IDLE on `idle.client`. `session_id` is the
  engine's private counter, not the KWP handshake id.

**DB time** (I3) is ON_CPU + `sched.run_queue` + every timed event outside
`idle.*`; `ANALYZE` prints a statement's as `DB time 4.21 ms = CPU 1.02 +
commit.wal_flush 2.95 + ...` when the statement runs inside a task of its own
session (the sum of the parts as printed, each rounded to 10 us). A cross-core
step has no site today (every statement runs on its session's core, AT-S9), so
no cross-core line prints.

### 6.7 `sys.ash` (`sched/ash_sampler.hpp`, `stats/ash_ring.hpp`)

The active-session history (CJ's WE06-WE09). Each core's reactor samples its
own tasks once per `ash_interval_ms` (default 1000) on a drift-free grid: a
task that is not IDLE, background tasks included (`session_kind` =
`background`). A long step yields one batch at the real time with `late` = 1
(more than a tenth of an interval after its due time), nothing interpolated.
The sampler runs between polls, so `state` is never ON_CPU: CPU time reads as
RUNNABLE (CJ-Q12); `sys.wait_stats` carries the exact figure.

Columns: `sample_ts_us` (the instance's monotonic clock, not wall time), `core`,
`session_id`, `session_kind`, `state`, `event`, `class`, `sched_group`, `late`,
`arg_hash_valid` (1: `p1` is the statement's argument hash), `fingerprint` (the
statement's `fetch_id`, 0 before a statement names one), `p1`-`p3`,
`wait_elapsed_us`. The 64-byte record also holds `step_id`, `origin_session`
and `trx_id`, which nothing fills yet and the view does not show.

A view read copies each core's ring under that ring's own latch, so a query
during sampling sees whole records, and merges by time (ties by core, then
ring order). The ring is memory only, a fixed `ash_ring_bytes_per_core`
(default 4 MiB, 1-64 MiB, restart-only) overwritten oldest-first; a restart
begins empty. Filters are `WHERE` on the columns above.

**Settings** (`kds.conf` keys, runtime change by `SET ASH`, admin): `ash_enabled`
(on), `ash_interval_ms` (1000, 100-10000), `ash_capture_arg_hash` (off),
`ash_ring_bytes_per_core`. Off keeps the ring; on continues it. The settings are
process-wide, so two instances in one process share them.

## 7. `sys.meta`

**Tall** (BO-Q4): `(section, field, value)`, every value text, so a field
added or removed needs no schema change. The fields are the session's
core's, as `SHOW META`'s always were; the instance-wide ones read the same
on every core - among them the contention census, the four statistics
stores' counts, the backup and archive blocks, the log's durable and append
positions and roll totals, and the `load` block, which carries every
core's load. A field whose writer emits no
`=` is refused `Corruption`, never shown without a value. The field list
lives with the code that writes each section (`CommandDispatcher::MetaSections`).

**The `waystone` section** (CF-S1b, CF-S1c) carries `waystone_recording`
and `waystone_replay`, the instance's boot ceilings under their keys' own
names (`on`/`off`), and `waystone_opted`, the instance's opted count (one
load, `waystone-concpets.md` §4b). Nothing records until an operator opts
a shape in, so a config reading `waystone_recording = on` means less than it
did, and this section, the boot log line and `WAYSTONE ON`'s reply say so.

## 8. What is not here

- `sys.sessions` (W2: excluded by the operator).
- Per-relation page counts, leaf splits, spills and delete-marked slots
  (BO-Q13).
- A cross-core `sys.meta`: every core's fields readable from one session
  needs a field-by-field census of what is readable across cores
  (`known-gaps.md`).
