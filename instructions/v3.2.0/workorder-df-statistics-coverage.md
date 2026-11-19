# Work order DF - the statistics the engine records, widened: what a statement shape costs, transactions and the read horizon, index and relation usage, space

Status: **OPENED 2026-11-18** on the operator's word
(`raft-marks-2026-11-18.md` §36), from `57d7c67f`
(`v3.4.12-67-g57d7c67f`). **DF-S0 is done** (§7): the census, the spike's price, and four items
it took back, DF-Q11..Q14, **each marked (a) on the operator's word**
(`raft-marks-2026-11-18.md` §37). **DF-S1 is done and pushed** (§7):
part A, started on the operator's word (`raft-marks-2026-11-18.md` §39),
reviewed, the full suite 0 failed of 4700 at `ee89842e`, overhead not
measured, pushed with the hook skipped on the operator's word (§40).
**DF-S2 is done** (§7): part B, started on the operator's word
(`raft-marks-2026-11-19.md` §5), reviewed, the full suite 0 failed of 4766,
overhead not measured, not pushed. **DF-S3 is done** (§7): part C - index
usage and a relation's lock waits; the access shape's two columns owed to
DA - reviewed, the full suite 0 failed of 4773, overhead not measured, not
pushed. **DF-S4 is done** (§7): part D - a relation's delete-marked
estimate and its leaf divisions and compactions - reviewed, the full suite
0 failed of 4777, overhead not measured, not pushed. **DF-S5 ran and DF
is not closed** (§7): the close measured DF-S2..S4 at +0.07 us of server
CPU on a tuple key read and +0.18 us on an index-served one after three
remedies (one reverted, one an attribution, one kept), which with DF-S1's
+0.44 us puts a point read at about +0.51 us - **not resolved against
DF-Q10 (a)'s 0.5 us, and the operator's to read** (DF-Q24); the full suite
0 failed of 4778; not pushed. **The rest of DF ran
on the operator's
standing go-ahead of 2026-11-19** (§6 there, go-ahead-achieving-milestone):
an item a stage raises is adopted as CLA proposes it and recorded in §4 as
adopted on that word, never as the operator's own mark. Written on
2026-11-18 on worktree
`da-q8-memory-bound-key`, branch `worktree-df-statistics-coverage`, from
`56d20394` (`v3.4.12-53-g56d20394`) on the operator's word of 2026-11-18
(`raft-marks-2026-11-18.md` §33). The session was isolated in that worktree
and could not open one named for this order; the branch carries the name.

**DF-Q0..Q10 each marked (a), as CLA proposed, on the operator's word of
2026-11-18** (`raft-marks-2026-11-18.md` §34), a word that marked the
items and opened nothing; §36 opened DF. **DF moves no format**: no superblock bump, no catalog relation,
no WAL record kind, no page byte (DF-R1). Every `[source-read]` claim below
was read at `56d20394`, and the order was reviewed by `critics-developer`
before it was committed (§7).

**The motive is a question the engine cannot answer today, four times.**
Which statement shape reads the pages, writes the log and waits; how many
transactions decide each way and what holds the read horizon back; which
index nothing uses; and how much of a relation is delete-marked. Each is
answered by the commercial engines the operator asked CLA to compare with,
and each is absent from the views as read (§1). **DF is not a performance
order**: every number it adds is paid for on a statement's path or a
steward's step, and its bar is a ceiling on that cost (DF-Q10), not a gain.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[external]` CLA's knowledge of another engine,
looked up nowhere for this order.

Related: `docs/spec/sys-views.md` §5-§8 (the registry, the memory-resident
views, SV3, what is not here); `docs/spec/wait-events.md` (CJ-Q8);
`docs/spec/plan-stats.md` PS1, PS4, PS9 (the steward and its sample);
`docs/spec/txn.md` §1, §5; `docs/spec/index.md`;
`docs/spec/heap-and-tuple.md` §4.1c, §7;
`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md` (BO-Q5 (b),
BO-Q6, BO-Q7, BO-Q8, BO-Q13, BO-Q15: the shape DF widens and the item DF
reopens); `instructions/v3.2.0/workorder-cj-wait-events-ash.md` (the wait
accounting DF reads); `instructions/v3.2.0/workorder-da-access-stats-memory-resident.md`
(DA-S1, the store one part of DF waits for; DA-Q8, DA-R6);
`instructions/v3.2.0/workorder-cx-managed-plans.md` (the captured order,
DF-Q5); `instructions/v3.2.0/workorder-dd-index-entry-purge.md` (an index's
dead entries, not DF's).

## 0. What DF is

`[operator]` Asked what kinds of statistics would be worth adding beside
what a commercial database records, CLA listed nine; the operator took the
first four as one work order (`raft-marks-2026-11-18.md` §33).

`[design]` Four parts, each a stage that lands alone:

- **A - what a statement shape costs.** `sys.statement_stats` says how often
  a shape ran and how long. DF adds what it read and wrote, where its time
  went, how its latency is distributed, and how it was refused; and gives
  `DELETE` a shape.
- **B - transactions and the read horizon.** How many transactions decided
  each way, how long explicit ones live, and how old the oldest live read
  view is and where it is held.
- **C - usage.** Per index: how often it served a read. Per access shape:
  the rows it examined against the rows it kept. Per relation: its lock
  waits, cumulative.
- **D - space.** Per relation: how much is delete-marked, and how often its
  leaves divide. This reopens BO-Q13, adopted (a), "not in BO", on the
  operator's standing go-ahead of 2026-10-13 (BO §5) (DF-Q9).

Nothing DF records is read by the engine (DF-R3).

## 1. The tree as read

### 1.1 What is recorded today `[source-read]`

| store | key | what a row holds | bound |
|---|---|---|---|
| `stats::StatementStats` (`include/kds/stats/statement_stats.hpp:41-51`) | `(fetch_id, kind)` | `calls`, `refused`, `total_us`, `max_us`, `first_seen`, `last_seen`, `shape_text` (256 bytes, `:61`) | 4,096 shapes a core (`:60`), a node map (`:91`) |
| `stats::RelationStats` (`include/kds/stats/relation_stats.hpp:52-57`) | relation oid | `inserts`, `updates`, `deletes`, `purged` | 4,096 relations a core (`:61`) |
| `sys.access_stats` (`include/kds/stats/access_stats.hpp:28`) | `(kind, rel_id, column_mask)` | `uses`, `last_seen` | 4,096 shapes (`include/kds/catalog/rows.hpp:781`); a catalog relation until DA-S1 |
| `stats::LockWaitLog` (`include/kds/stats/lock_wait_log.hpp:44-60`) | a ring | one 64-byte record a parked wait: unit, mode, `rel_oid`, `key`, both transactions, `waited_us`, outcome | 4,096 a core (`:64`), overwritten oldest first |
| wait accounting (`include/kds/sched/wait_state.hpp:93-99`, `:105-127`) | core and event; session and event | count and total; a core's also max and a log2 histogram | fixed by the event registry |
| `sys.meta` (`CommandDispatcher::MetaSections`, `src/server/command_dispatcher.cpp:1820`) | section and field | the pool's hits and misses, the log's syncs and group commits, the scheduler, recovery, backup, `undo_pages_live` (`:1990`) | - |

**What `sys.statement_stats` cannot say** (`docs/spec/sys-views.md` §6.1):
rows, pages, log bytes, the split of its time, any percentile, why a call
was refused. `RecordStatement` is handed the line, the outcome and the
instant the statement began, and nothing else
(`src/server/command_dispatcher.cpp:1250-1259`).

**What is already counted and thrown away at the statement's end.**
`exec::StepStats` holds, per step, `rows_examined`, `rows_matched`,
`pages_fetched`, `spill_fetches`, `index_entries_scanned` and
`index_rows_resolved` (`include/kds/exec/step_vm.hpp:75-161`); `ANALYZE`
prints them and the plan statistics fold `rows_matched` from a planned
block (`docs/spec/plan-stats.md` PS2). `pages_fetched` is exact on a walk
and counts one a keyed access, whatever the tree's height
(`step_vm.hpp:106-115`). `ANALYZE`'s DB-time line is the session's wait
statistics after the statement less before
(`include/kds/sched/wait_state.hpp:200-209`;
`src/server/command_dispatcher.cpp:1614-1621`): a whole copy of the
session's `SessionWaitStats`, about 0.4 KB at 20 events, before and after,
each behind a `FlushOnCpu`. It holds only when the statement runs inside a
task of its own session (`wait_state.hpp:210-212`).
`wal::WalStats` counts records and bytes appended for the manager whole
(`include/kds/wal/manager.hpp:126-128`; `src/wal/manager.cpp:410-411`), not
by transaction.

**`DELETE` has no shape.** The fingerprint's allow-list is `select`,
`insert`, `update` (`src/parser/fingerprint.cpp:130-132`), so every
`DELETE` is one row at `fetch_id = 0` (`statement_stats.hpp:19-24`). Adding
a leading word needs no `kFingerprintVersion` bump: "making a statement
fingerprintable that previously was not - adding `DELETE` to the
patternable leading words, say - does not require a bump"
(`include/kds/parser/fingerprint.hpp:172-177`).

**Transactions.** `sys.meta`'s `txn` section carries two fields, both of
`BEGIN ... READ ONLY` (`command_dispatcher.cpp:1998-2002`). No rollback,
abort or lifetime-ceiling count was found in `include/kds/txn/manager.hpp`,
the dispatcher's members or the listener. The lifetime sweep and a
connection's close end a transaction by dispatching `ROLLBACK` itself, the
client's statement word for word (`src/server/tcp_server.cpp:943-945`); the
sweep logs a line and counts nothing (`:960-963`). **Commits are
counted, by the log**: `wal::WalStats` counts every commit record a core's
manager appends, by rung - `strict_commits`, `group_commits`,
`relaxed_commits` (`include/kds/wal/manager.hpp:133-147`;
`src/wal/manager.cpp:439-462`, a staged `strict` commit counted before its
sync) - and `sys.meta` shows `wal_group_commits` alone
(`command_dispatcher.cpp:2065`). A mount's losers are `recovery_rolled_back`
(`:2266`). A deadlock's victim is refused
`TxnConflict` with a message naming deadlock (`command_dispatcher.cpp:527-533`),
so the code alone does not tell a deadlock from a conflict; `sys.lock_waits`'
`outcome` does, in a ring that overwrites.

**The read horizon.** `TransactionManager::ReadHorizon()` is the oldest
snapshot LSN any live reader on any core holds, the minimum over every
core's published bound (`include/kds/txn/manager.hpp:722-734`;
`include/kds/txn/instance_visibility.hpp:247-252`). It bounds every purge.
A core publishes an LSN and no time, so the horizon has no age to read. No
view shows its value, its age or the core that holds it;
`undo_pages_live` is the one number that moves when it is held back. The
lifetime sweep knows each connection's `txn_began_at_ns`
(`src/server/tcp_server.cpp:938`).

**Indexes.** `sys.indexes` shows an index's keys, widths, `height`,
`entries` and `unique` (`src/server/sys_views.cpp:527-539`) and no use. A
step that an index serves is `kIndexProbe` or `kIndexRange`
(`include/kds/exec/step_chain.hpp:118`, `:130`) and names its index
(`step_chain.hpp:308`). `sys.access_stats` records such a step as a shape of
columns, not of an index, and only for a successful `SELECT` and a foreign
key's checks (`command_dispatcher.cpp:9221`, `:9271-9280`, `:4364-4377`;
`workorder-da-access-stats-memory-resident.md` §1.3), and both behind the
`access_statistics` key (`:9279`, `:4371`).

**Space.** `sys.relation_stats`' `leaves` is a walk of the clustered tree
at every read of the view (`sys_views.cpp:698`;
`src/storage/btree/btree.cpp:1355`). Leaf divisions, spills, page counts and
delete-marked slots are recorded nowhere: "each is a hook in the storage
layer for a number no reader has asked for"
(`workorder-bo-sys-statistics-show-sugar.md:405-407`, BO-Q13 (a);
`relation_stats.hpp:26-27`; `sys-views.md` §8). The plan statistics'
steward already reads sampled leaves for their live slots, within 16 page
reads a step (`plan-stats.md` PS1, PS4; `include/kds/stats/plan_stats.hpp:94`),
and skips a delete-marked one by its flag (`src/stats/plan_stats.cpp:79-84`).
The page epoch a division or compaction moves (`heap-and-tuple.md` §3.1a) is
a word on one page, not a count of a relation's divisions.

### 1.2 What is measured `[measured]`

One number, and it is BO's: the statement-path recording of
`StatementStats::Record` and `RelationStats::Record` costs 0.35-0.52 us of
server CPU a point statement at `cores = 1`, `relaxed`, inside the
measurement's spread
(`bench/v3.3.0/results-bo-close-v3.3.0-23-g86d1fe1a.md`, cited by
`sys-views.md` §6). **Nothing DF proposes is measured.** DF-S0 prices it
before anything is built.

### 1.3 What the other engines record `[external]`

From CLA's knowledge, looked up nowhere for this order, and binding on
nothing: PostgreSQL's `pg_stat_statements` (rows, block hits and reads, WAL
records and bytes, plan time, min/mean/stddev), `pg_stat_database`
(commits, rollbacks, deadlocks), `pg_stat_user_indexes` (`idx_scan`) and
`pg_stat_user_tables` (`n_dead_tup`); Oracle's `V$SQL` (buffer gets, disk
reads, CPU and wait-class time) and segment statistics (row lock waits,
leaf node splits); SQL Server's Query Store and index-usage views; MySQL's
statement digest summaries and histograms. They are the reason the four
questions are asked, not a specification of the answers.

## 2. The design `[design]`

**Part A - the statement shape.** `StatementShape` gains, per
`(fetch_id, kind)`:

- `rows`: the rows a `SELECT` delivered or a write affected.
- `rows_examined`, `pages`: `ExecStats::Total()`'s, where the statement ran
  a step chain; NULL-valued for a shape that never did (DF-R7).
- `log_bytes`, `log_records`: what the statement's transaction appended
  while the statement ran, from a count carried on the transaction
  (DF-Q7); a per-core or per-manager delta is not used, because another
  statement of the core can run between this statement's head and its end
  (`command_dispatcher.hpp:327-338`).
- `compile_us`: head of the statement to its chain compiled.
- the time split: `cpu_us`, `sched_us` and one column a wait class a
  statement's task can wait in, from the session's wait statistics after
  less before - what `ANALYZE` prints - with `timed_calls`, the calls the
  split covers (DF-Q6).
- `page_reads`: the statement's own blocking reads, the counts of
  `io.page_read`, `io.scan_ring_read` and `io.var_heap_read` in that same
  delta, covered by `timed_calls` as the split is (§4's closing paragraph).
- `conflicts`: the calls refused `TxnConflict`, a subset of `refused`.
- a log2 latency histogram, 32 buckets of the wait histogram's bounds,
  shown as `sys.statement_histogram`: one row a non-empty bucket of a shape
  (DF-Q2).

`sys.refusal_stats` is new: one row a pinned error code a core, `count`
and `last_seen`, bounded by the registry and keyed by nothing a client can
mint (DF-Q3). `delete` joins the fingerprint's leading words (DF-Q4).

**Part B - transactions and the horizon.** Per core, in `sys.meta`'s `txn`
section: `commits`, `rollbacks` (the client's `ROLLBACK`), `aborts` (a
transaction the engine rolled back: a failed statement's, a failed sync's,
a closed connection's), `ceiling_rollbacks`. The listener's `ROLLBACK` is
the client's spelling (§1.1), so a site in its handler would count all
three; each is counted where it is decided. `commits` is not a second name
for the log's three commit counts (DF-R5): DF-S0 says whether it is their
sum shown, or a count at the decide that differs from it by the staged
`strict` commits a failed sync aborts. A log2 histogram of an explicit
transaction's life, begin to decide, as `sys.txn_histogram`, read-only
transactions apart. The
horizon, instance-wide: `read_horizon_lsn`, `read_horizon_age_us` (the age
of the oldest live view) and `read_horizon_core` (the core whose bound is
the minimum). `sys.session_state` gains `txn_age_us` and `view_age_us`, so
the session holding the horizon can be found (DF-Q8).

**Part C - usage.**

- `stats::IndexStats`, a store the instance owns (DA-R6's reason), one part
  a core keyed by index oid: `probes`, `ranges`, `entries_scanned`,
  `rows_resolved`, `last_used`. Shown as columns of `sys.indexes`. Folded
  from `StepStats` where a chain's run is recorded (`RecordExecution`): a
  native `SELECT`'s, each query-layer walk's and a scalar subquery's; a
  write's `WHERE` subquery runs a chain that is not recorded (DF-S0, §7).
- `sys.access_stats`' shape gains `rows_examined` and `rows_matched`: the
  shape that reads a hundred rows to keep one is then a row of a view.
  **This part waits for DA-S1**: today the row is a 33-byte persisted
  struct, and widening it is a format event DF does not make (DF-R1).
- `sys.relation_stats` gains `lock_waits`, `lock_wait_us`,
  `lock_deadlocks`, `lock_fault_nets`, written where a `LockWaitRecord` is
  (`command_dispatcher.cpp:444`, `:712`; `src/stats/lock_wait_log.cpp:29`),
  from the record's `rel_oid` and outcome. `lock_deadlocks` summed is the
  deadlocks that wrote a `sys.lock_waits` row, not every deadlock refusal:
  a victim whose slot had already flipped writes none (`:440`, `:709`), nor
  does an online index build's drain (`:2880-2884`). No instance counter is
  added (DF-R5).

**Part D - space** (under DF-Q9's mark).

- `est_delete_marked` on `sys.relation_stats`: the steward's sample, the
  delete-marked slots of the leaves it already reads, scaled as PS1 scales
  live slots. Kept in a side table of the steward's that only the view
  reads: **it never enters the planner's snapshot and never moves its
  generation** (DF-R8).
- `leaf_divisions` and `leaf_compactions` on `sys.relation_stats` and
  `sys.indexes`: counted in memory where the division or the in-place
  compaction is done, since the instance started. This is the hook in the
  storage layer BO-Q13 declined.

## 3. Rules

Binding once DF opens.

- **DF-R1 - no format moves.** Every number is memory-resident, emptied by
  a restart and by a restore: no catalog relation, no page byte, no WAL
  record kind, no superblock bump. A part that cannot be built without one
  is not DF's and goes back to the operator.
- **DF-R2 - a record never fails or reorders what it describes, and waits
  for at most one copy of its own part** by a reader. No allocation is
  added to a statement's path beyond what the store it widens makes today
  and, for `stats::IndexStats`, its part's first sight of an index.
- **DF-R3 - nothing in the engine reads a number DF records.** Not the
  planner, the Cabin controller, a step, a purge or a refusal. A lost or
  wrong number costs a reader's insight and never an answer.
- **DF-R4 - a record writes its own core's part and nothing shared.** The
  one exception is the steward's side table (Part D), one for the instance
  as the planner's snapshot is, written by core 0's steward alone.
- **DF-R5 - no second name.** A quantity a view or a field already carries
  is not recorded again: `refused` stays the total, `sys.wait_stats` the
  authority for an event, `undo_pages_live` the undo backlog,
  `sys.access_stats`' `uses` the count of a shape's runs.
- **DF-R6 - no value enters a statistic.** No literal, bind value, key or
  argument hash is added (WD11); no column DF adds holds a statement's
  text.
- **DF-R7 - a column the engine could not fill is NULL, never 0.** A shape
  no chain ran for has no `pages`; a call outside a task of its own session
  adds nothing to the time split and is not in `timed_calls`.
- **DF-R8 - the space estimate is not a planner fact.** Nothing DF adds
  enters the planner's snapshot or moves its generation; PS9's one reader
  for a plan is unchanged.
- **DF-R9 - no switch** (BO-Q8), unless DF-S0's price sends the question
  back (DF-Q10). The access shape's two columns ride the existing
  `access_statistics` key (DA-Q4), as its rows do.

## 4. Items for the operator (DF-Q), CLA's proposals

**Every item is marked (a), the operator's mark of 2026-11-18**
(`raft-marks-2026-11-18.md` §34: *"Follow the proposals"*). (a) was CLA's
proposal in each. Three of the options are conditional on DF-S0, as each
states: DF-Q4 (a) only if the read of a fingerprint's consumers finds no
behaviour moved but the statistics key; DF-Q6 (a) the wait-statistics
delta only if its two copies price inside DF-Q10's bar, else per-statement
accumulators; DF-Q10 (a) sends a part over its bar back to the operator.
**DF-Q9 (a) lifts BO-Q13's exclusion** for delete-marked slots and leaf
divisions alone; spills and page counts stay excluded.

| | item | (a) | other |
|---|---|---|---|
| DF-Q0 | One order or four. | one order, four parts, each stage landing alone and each closable without the next | (b) four orders; (c) part A alone, the rest re-asked after its close |
| DF-Q1 | The bound of the widened statement store. A shape grows by the columns of §2 and a 32-bucket histogram - some hundreds of bytes, unmeasured - in a node map of 4,096 shapes a core. | the count kept, the slot's bytes and the worst case over `cores` stated in `sys-views.md` | (b) a boot key in bytes a core and the store rebuilt as a flat table, as DA-Q8 (a) marks for DA's store - a rebuild of a BO store, which DA §4 leaves "not settled" |
| DF-Q2 | The latency distribution. | a log2 histogram a shape, `sys.wait_histogram`'s buckets, shown as `sys.statement_histogram` | (b) `min_us` and a sum of squares, a mean and a deviation and no percentile; (c) none |
| DF-Q3 | Refusals by cause. | `conflicts` a shape, and `sys.refusal_stats` by pinned code a core | (b) a view keyed `(fetch_id, kind, code)`, bounded by shapes times codes; (c) `conflicts` alone |
| DF-Q4 | `DELETE`'s shape. | `delete` added to the fingerprint's leading words, after DF-S0 reads every consumer of a fingerprint - the pattern system, ASH, the Cabin controller's count, the plan memo, captured plans and PS2's posteriors (each keyed by `fetch_id`, 0 meaning none: `src/exec/step_compiler.cpp:2410`, `:2479`), the `pattern_id` a KWP `S_PARSE_OK` returns (`src/server/kwp_session.cpp:552-562`, client-visible), the golden corpus, and `tests/fingerprint_test.cpp:162`, which pins `DELETE` unpatternable - and finds no behaviour moved but the statistics key | (b) left one row at `fetch_id = 0` |
| DF-Q5 | The identity of the order a shape ran. CLA's list named it. | **not built**: `sys.plans` owns a shape's order and, since CX-S1a, shows it - `fetch_id` and `join_order` as run, kept across a restart (`sys-views.md` §5, read at `db1880da`) - so a column here would be a second name for it (DF-R5), and one a restart empties | (b) an `order_hash` and a count of changes a shape, of use under `join_order = planned` alone |
| DF-Q6 | The time split's source. | the session's wait statistics after less before, as `ANALYZE` reads them, if DF-S0 prices the two copies inside DF-Q10's bar; else per-statement accumulators on the task | (b) `cpu_us` alone; (c) no split, `sys.session_waits` and ASH stay the only source |
| DF-Q7 | Log bytes a statement. | a count on the transaction, bumped where an append names its transaction; a record that names none (`PURGE`'s) is no statement's | (b) not recorded by shape; the instance's total added to `sys.meta` |
| DF-Q8 | Who holds the horizon. `sys.sessions` is excluded (BO's W2, `workorder-bo-sys-statistics-show-sugar.md:21`; `sys-views.md` §8). | the three instance fields, and `txn_age_us` and `view_age_us` on `sys.session_state`, which exists | (b) the instance fields alone, the session found from ASH - which shows no `trx_id` (`src/server/sys_views.cpp:1172-1173`) |
| DF-Q9 | **BO-Q13, reopened**: delete-marked slots and leaf divisions. Adopted (a) "not in BO", on the ground that no reader had asked. | the steward's sampled estimate for delete-marked slots, and in-memory counts of divisions and compactions at their sites | (b) an exact count by a walk of one relation, a view with a required key as `sys.page_slots` has - a whole walk on the reactor unless it is sliced; (c) (a) and (b); (d) BO-Q13 (a) stands, part D not built |
| DF-Q10 | The measurement and its bar. | DF-S0 prices each part's statement-path cost by a spike, interleaved A/B, a point `SELECT` and a point `UPDATE` at `cores` 1 and 8; the close repeats the cells over the whole change. **The bar: all of DF adds no more server CPU a point statement than BO's recording did, 0.5 us at `cores = 1`**; a part over it goes back to the operator with the column that costs it. Inside the standing time budget (20 minutes a stage, about an hour at the close) | (b) the close alone; (c) another bar, the operator's |

**What CLA's list said that the tree corrects.** Item 1 named the executed
join order (DF-Q5 (a) withdraws it) and "page hits and misses a shape".
**Hits are not countable**: `pages` is the logical count, one a keyed
access whatever the height (§1.1), so `pages` less misses is not hits.
**Misses are the faulting statement's**: a miss is a blocking `pread` on
the reactor inside a `WaitScope` (`src/storage/device_page_store.cpp:1114-1116`),
booked to the event and to the faulting task's session
(`src/sched/wait_state.cpp:191`), so a statement's own misses are its
session's `io.page_read`, `io.scan_ring_read` and `io.var_heap_read` counts
after less before - DF-Q6's source, under its condition. What is no one
statement's is the stall the read puts on every other task of the core
(`sys-views.md:238-239`, `wait-events.md:73`, CJ-Q8). §2's `page_reads`
is that delta, so it stands or falls with DF-Q6 (a): under (b) or (c) it
is not built. DF-S0 re-reads this.

**Items the stages raised after the operator's standing go-ahead of
2026-11-19** (`raft-marks-2026-11-19.md` §6, go-ahead-achieving-milestone).
**Each is adopted as CLA proposed it, on that word, and is not the
operator's own mark.** (a) is CLA's proposal in each.

| | item | (a), adopted | other |
|---|---|---|---|
| DF-Q15 | Where a view's age is read from (DF-S2). DF-S0 named a mint stamp and "the statement's head instant handed to the mint". | the head of the statement that minted the view, carried as a stamp on every held view and published beside its core's bound: no clock is read on a statement's path, and the age overstates by the head-to-mint time | (b) a clock read at every mint, exact, about three reads a point `SELECT`; (c) the time since the core's bound last changed, no stamp a view, which calls an idle instance's horizon old |
| DF-Q16 | What the horizon's fields and a session's ages show with nothing held or nothing measured (DF-S2). | `none` in `sys.meta` and NULL in `sys.session_state` (DF-R7): no view held, or a view nobody stamped | (b) 0, which reads as "held, and new" |
| DF-Q17 | Which transactions the six counts hold (DF-S2). DF-Q13 (a) leaves a `READ ONLY` `COMMIT` out of `commits` and is silent on the rest. | read-write transactions alone: a `READ ONLY` one is in none of the six, whichever way it ends, a sweep's and a close's included; its life is in `sys.txn_histogram` under its own access mode | (b) a `READ ONLY` transaction's rollbacks counted by cause |
| DF-Q18 | What `wait_aborts` takes beside a wait (DF-S2). DF-S0's census says a structural re-run ends the same way; DF-Q13 (a) names a wait. | the transaction an autocommit write gives up to run again after a stale descent is a `wait_abort` too: the statement did not fail | (b) it is an `abort` |
| DF-Q19 | Where an explicit transaction's life ends (DF-S2). | where its end is handled - the `COMMIT`, the `ROLLBACK`, the listener's rollback - so a parked `strict` commit's wait for its sync is not in it | (b) at the commit's publish, a second site and a stamp carried across the park |
| DF-Q20 | A KWP load stream's ends (DF-S2). Its listener dispatches `ROLLBACK` itself at five sites. | its own abort is `rollbacks`; a failed chunk and a frame not legal inside a load are `aborts`; a closed connection and a detaching listener are `close_rollbacks` | (b) all five the client's `rollbacks` |
| DF-Q21 | What `probes` and `ranges` count (DF-S3). §2 names the columns and not their unit. | the times a step walked its index between one pair of bounds, counted where the index is read (`StepStats::index_walks`): once a row of the outer for a step under another, once a value for a value list, and nothing for a step that named an index and walked its relation instead | (b) the step's openings, which credit an index the run never read (`indexes = off`, an index dropped under a compiled chain) and count a value list of N values as one; (c) recorded statements, one a statement whatever it did |
| DF-Q22 | The allocation a leaf count makes (DF-S4). DF-R2 admits no allocation on a statement's path but a store's own and `stats::IndexStats`' first sight of an index; the leaf counts' store is new, and its first division of a tree on a core allocates under the divided leaf's hold. | DF-R2's first-sight exception is read to reach this store: one allocation a tree a core, at an act that already claims and formats a page, and a failure drops the count, never the split | (b) a relation's counts folded into `stats::RelationStats`, whose entry a write statement makes anyway, an index's kept apart; (c) a fixed table of words, no allocation, a bound that drops sooner |
| DF-Q23 | What `leaf_divisions` counts, and on which views (DF-S4). §2 names divisions and compactions for both `sys.relation_stats` and `sys.indexes`. | a division is a full leaf giving half its rows to a new one; a clustered tree's append of a fresh leaf, nothing moved, is not one and is counted nowhere; `sys.indexes` has `leaf_divisions` alone, an index leaf being never compacted - a column that could only read 0 is not shown | (b) appends counted as divisions; (c) a `leaf_appends` column beside them; (d) `leaf_compactions` on `sys.indexes`, always 0 |
| DF-Q24 | The close measured DF at or over DF-Q10 (a)'s bar on a point read (DF-S5, §7): DF-S1 +0.44 us, DF-S2..S4 +0.17 us on a tuple key `SELECT` and +0.35 us on an index-served one. DF-Q10 (a) sends a part over the bar back to the operator; the standing go-ahead asks for a remedy within the milestone first. | one remedy, then the measurement again and whatever it shows reported as it is: the session cost sample of DF-S1, which the bisect of 2026-11-19 named as the likeliest carrier and whose cheaper form was CLA's proposal there, folded where a wait is booked. Nothing is reverted, no switch is added (DF-R9), and **whether DF closes over an unmet or unresolved bar stays the operator's** | (b) no remedy, the bar's reading straight to the operator; (c) the stamps of DF-S2 or the fold of DF-S3 behind a boot key, against DF-R9; (d) a stage reverted |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DF-S0 - census and premise** | No engine code on `main`. §1 re-read at the opening commit. **Read**: which statements end with an `ExecStats` in reach of `RecordStatement`, and which run a chain at all; every consumer of a fingerprint (DF-Q4); whether every log append names its transaction (DF-Q7); every site that decides a transaction, for part B's four counts; what would publish a view's mint time beside its core's LSN (`read_horizon_age_us`, `view_age_us`) and a session's transaction start where `sys.session_state` reads (`txn_age_us`; the listener holds it, `tcp_server.cpp:938`); every test that pins a widened view's column list. **DF-Q10's spike and cells.** | the lists, the prices, and any item back to the operator |
| **DF-S1 - part A** | `StatementShape` widened; `sys.statement_histogram`; `sys.refusal_stats`; `delete` a leading word, in a commit of its own. | review, the full suite; at the push gate |
| **DF-S2 - part B** | the `txn` section's counts; `sys.txn_histogram`; the horizon's fields; `sys.session_state`'s two columns under DF-Q8 (a). | review, the full suite; at the push gate |
| **DF-S3 - part C** | `stats::IndexStats` and `sys.indexes`' columns; `sys.relation_stats`' lock columns. **The access shape's two columns only once DA-S1 has landed**; else the stage closes saying they are owed. | review, the full suite; at the push gate |
| **DF-S4 - part D** | under DF-Q9's mark. | review, the full suite; at the push gate |
| **DF-S5 - the close** | DF-Q10's A/B over the whole change; the documents below amended; the milestone row flipped. | closed |

**Tests DF owes.** Per part, one cell a column that the number is what a
fixed workload makes it; two cores running one shape answer one row with
the sums, and a histogram's buckets sum to `calls`; a refused call is in
`refused`, and in `conflicts` only when its code is `TxnConflict`; two
`DELETE`s of one shape are one row, apart from their `SELECT` twin's; a statement outside a
task of its own session leaves the split NULL (DF-R7); a commit, a client
rollback, an engine abort and a ceiling rollback each move one count; the
horizon's age grows while a `BEGIN ... READ ONLY` is held and returns when
it ends; an index a read never names stays at 0 and one it names moves; a
parked wait moves its relation's lock columns and a deadlock its
`lock_deadlocks`; a relation with delete-marked rows answers an estimate
above 0 and a `PURGE` brings it down at a later step, and the planner's
generation does not move for it (DF-R8); every store empty after a process
restart and a restore (an in-process restart keeps the BO stores,
`known-gaps.md` BO); `show_sugar_contract_test`'s cells for each widened view; an
armed two-core rig reading each view while both cores record, under `tsan`.

**Documents DF-S5 owes**, each read at the close: `sys-views.md` §5, §6.1,
§6.2, §6.6, §7, §8 (BO-Q13's line); `plan-stats.md` (the steward's side
table, PS9 restated); `txn.md` §1; `index.md` (`sys.indexes`);
`wait-events.md` where a column cites an event; `known-gaps.md` (the BO and
CJ sections, and what DF leaves owed); `manual/sql/sql.md` and the server
manual's views; `CLAUDE.md`'s observability row.

## 6. What DF does not do

- **Items 5-9 of CLA's list**: a history or snapshot of counters over
  time, a reset statement, a memory view, I/O and log detail by file or
  record kind, progress of long operations, and column statistics for the
  planner. None is opened by DF.
- No `sys.sessions` (BO's W2), and no client-visible session identity
  (`known-gaps.md`, CJ).
- No hit column a shape (§4).
- Nothing for an index's dead entries: no entry is removed today, and what
  is dead is DD's to define.
- No persistence. A restart and a restore empty every number, as they do
  today's.
- No fix to what today's counts mean: `ON CONFLICT ... DO UPDATE`'s rows
  stay counted as `known-gaps.md` states, and a rolled-back statement's
  rows stay counted (BO-S6).
- No key for the four BO stores (DF-Q1 (a)).
- Nothing reads what DF records (DF-R3): no advisor, no automatic index
  drop, no purge trigger.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-18

Written on worktree `da-q8-memory-bound-key`, branch
`worktree-df-statistics-coverage`, from `56d20394`
(`v3.4.12-53-g56d20394`), documents only. The letter DF is CLA's proposal.
No suite was executed: the commit changes no build input. Nothing was
measured for it; §1.2's number is BO's, at the version named.

**Reviewed by `critics-developer` before the commit.** It opened every
cited line; seven ranges were corrected in place and three statements given
the source they lacked. **Five claims of CLA's draft were false or
overstated, and each is corrected above**:

- "no commit count exists" - the log counts commits by rung
  (`wal::WalStats`), and the listener ends a transaction at the ceiling and
  at a close by dispatching `ROLLBACK`, so one site in its handler would
  count three causes as one (§1.1, part B);
- "a page miss is not attributable to a statement" - the read is booked to
  the faulting task's session; what is no one statement's is the stall on
  the core's other tasks. What cannot be counted is a hit. §4's closing
  paragraph and §6 now say so, and CLA added `page_reads` to part A on that
  ground, the review having left the column to CLA;
- "the lock columns summed are the instance's deadlocks" - two deadlock
  paths write no `sys.lock_waits` row (part C);
- the horizon's age was assumed readable - a core publishes an LSN and no
  time, so DF-S0 reads what would publish one (§1.1, DF-S0);
- BO-Q13 was written as the operator's adoption - it was CLA's proposal
  adopted on the standing go-ahead (§0).

Also applied: four consumers of a fingerprint the draft's list missed, one
of them client-visible (DF-Q4); `access_statistics` named as the key the
access shape's columns ride (DF-R9); DF-R4, DF-R6 and DF-R8 reworded where
each contradicted the design or misquoted PS9; "BO's W2" named, the bare
token colliding with this session's W2; a DF-S0 read no part used, cut.
**Not taken**: dropping DF-Q0 (b), four orders - the review found it sits
awkwardly beside the word "a work order", and it stays because how the
parts are split is the operator's to say. **Left to DF-S0 by the review**:
whether a `DELETE`'s write path hands a `fetch_id` to a planned compile.

### Merged with `origin/main` at `db1880da`, 2026-11-18

`origin/main` had moved from `56d20394` to `db1880da` (CX-S1a, CZ-S0) when
the order was committed, and was merged on the work branch. The record of
the operator's word took §33, §30-§32 having gone to those sessions.
**DF-Q5 (a)'s ground was re-read and rewritten**: as drafted it cited the
gap "captured orders do not survive a restart", which CX-S1a closed by
persisting them in `sys.plans`; the proposal stands on that view instead.
**No other `[source-read]` claim was re-read at the merge**: every cited
line is `56d20394`'s, and the merge moved `src/server/command_dispatcher.cpp`
and `src/server/sys_views.cpp`, so the line numbers cited in those two
files are stale on the merged tree and DF-S0's re-read at the opening
commit is owed as §5 says.

### DF-Q0..Q10 marked (a), 2026-11-18

On worktree `da-q8-memory-bound-key`, branch
`worktree-df-statistics-coverage`, from `84b29eea`, documents only. The
operator's word on CLA's report of the order (`raft-marks-2026-11-18.md`
§34) marks every item (a) and asks for the push with the hook skipped. The
header and §4 carry the marks; no design text changed. DF is not opened by
the word. No suite was executed: the commit changes no build input.

### DF-S0 - the census and the premise, 2026-11-18

On worktree `da-q8-memory-bound-key`, branch
`worktree-df-statistics-coverage`, from `6e7f7095` (the opening's record;
its source is `57d7c67f`'s). Every line below is `6e7f7095`'s. No engine
code is on this branch: the spike is commit `b2a4f70a` on branch
`df-s0-spike`, unpushed, and never lands. **No suite was executed**: the
work branch changes no build input, and the spike was built and measured,
not tested.

**1. The citations.** Every `path:line` of §1, §2 and §4 was re-read. In
`src/server/command_dispatcher.cpp`, `include/kds/server/command_dispatcher.hpp`,
`src/server/sys_views.cpp` and `docs/spec/sys-views.md` the merge moved the
numbers and no cited code or sentence changed in substance; the other
files keep theirs. One sentence of §1.1 is loose: a step names its index
through `Step::index` (`include/kds/exec/step_chain.hpp:666`), an optional
`IndexProbe` that carries the oid (`:308`), and `ExecStats` is indexed by
`step_id`, not by executed position.

**2. Which statements end with executor counters.** A native `SELECT`
alone: its run borrows a `SelectScratch` whose `stats` the executor fills
and `RecordExecution` reads (`command_dispatcher.hpp:2670`,
`command_dispatcher.cpp:6271`). An `UPDATE` and a `DELETE` do not run the
step VM over their own relation: each compiles its `WHERE` with
`exec::CompileWhere` and walks it with the dispatcher's `WriteWalk`
(`command_dispatcher.cpp:10616`, `:13326`, `:10178`), which reaches a row
by a tuple key descent, a key set or a walk, never by a secondary index or
a Cabin, and counts nothing. **A subquery in that `WHERE` is the
exception**: `WriteWalk::MatchesWhere` evaluates it through
`exec::EvaluateConjuncts` with no `ExecStats` (`:10489-10492`), whose
sub-chain runs with indexes on and a local `ExecStats` it drops
(`src/exec/step_vm.cpp:3048-3059`), so an index can serve it and nothing
records it. The query layer holds no statement-wide `ExecStats`, but each
relation it walks is a native `SelectRun` that reaches `RecordExecution`
with its own (`WalkForLayer`, `:9924-9936`) - an `INSERT ... SELECT`'s
source (`:6373`) among them - and an `UPDATE`'s scalar SET subquery runs
through `HandleSelect` (`:9787`). A plain `INSERT` runs no chain. So under
DF-R7 `rows_examined` and `pages` are filled for a native `SELECT` and
NULL for every other kind, a layer `SELECT` included unless its walks are
summed; and `stats::IndexStats`, folded at `RecordExecution`, sees every
read an index serves but a write's `WHERE` subquery's. A unique index's
check descents and a foreign key's are not a step and are not counted;
`sys.indexes`' `unique` column is what says such an index is in use.

**3. The fingerprint's consumers (DF-Q4).** Adding `delete` to the
allow-list alone gives a `DELETE` no shape: `DeleteInner` parses with the
free `parser::Parse` (`command_dispatcher.cpp:13294`), and only
`InsertInner` and `UpdateInner` call `ParseKeepingShape` (`:6310`,
`:10157`). With both changed, a `DELETE` reaches no planner, memo,
captured plan, posterior, Waystone or Cabin path: it compiles through
`CompileWhere`, which takes no `PlanOptions`, and its `USING` becomes an
`EXISTS` lowered without a plan. **Three behaviours move besides the
statistics key**: KWP's `S_PARSE_OK` returns a prepared `DELETE` a
non-zero `pattern_id` (`src/server/kwp_session.cpp:552-561`, read by
`tools/kwp.py:519-533`); `ANALYZE DELETE ...`, which is refused, is
keyed by the `DELETE`'s shape before its refusal
(`command_dispatcher.cpp:8917-8932`); and ASH samples a `DELETE` under
its `fetch_id` instead of 0 (`:854-856`, `sys.ash`'s `fingerprint`,
`sys_views.cpp:1241`). The Cabin controller's wait count
(`src/sched/ash_sampler.cpp:149-152`) does not move: it touches only a
`pattern_id` a `SELECT`'s execution registered
(`src/stats/optimizer_signals.cpp:83-89`). Three tests pin today's answer:
`tests/fingerprint_test.cpp:162`, `tests/testdata/parser_corpus.txt:481`
and `tests/show_sugar_contract_test.cpp:411-423`. **DF-Q4 (a)'s condition
is therefore not met as written, and the item is back at the operator as
DF-Q11.**

**4. Log appends (DF-Q7).** Every append passes
`WalManager::AppendThrough` (`src/wal/manager.cpp:367-411`), which has the
record's transaction id and size and no `Transaction` object; no thread
holds a current transaction. A count on the transaction therefore needs a
lookup of the id in the core's live transactions at every append, or the
object threaded through about fifteen logging helpers. Either way it
misses what a statement appends under no transaction: one
`HEAP_OVERWRITE` for each key an `INSERT` is issued, a burned redraw's
included (`Catalog::AllocateRowId`, `src/catalog/catalog.cpp:2903`), a
`PURGE`'s records, a rename's. A
checkpoint's `ASSERT_PENDING` carries a transaction's id and is not that
transaction's statement's (`src/wal/checkpointer.cpp:161-163`). An
autocommit statement's commit record is appended before
`RecordStatement`, so the window is whole. **The mechanism DF-Q7 (a)
names costs more and counts less than booking to the polled task's
session, as a wait is booked; back at the operator as DF-Q12.**

**5. The sites that decide a transaction (part B).** One site commits
(`TransactionManager::FinishCommitBody`, `src/txn/manager.cpp:289`) and
one rolls back (`Abort`, `:519`), and neither knows why. A `READ ONLY`
transaction reaches neither: its `COMMIT` and `ROLLBACK` end at
`Session::Finish` (`command_dispatcher.cpp:11435-11438`,
`:11541-11544`). A failed explicit `COMMIT` rolls back through
`RollbackLocal` (`:11459-11483`). The dispatcher's
`ROLLBACK` handler takes no cause: the lifetime sweep and a closed
connection dispatch the client's own text (`src/server/tcp_server.cpp:945`,
`:1113`), and `Session::Expire` runs after the rollback. A failed
statement inside an explicit transaction rolls nothing back - it poisons
the session and the client's `ROLLBACK`, the sweep or the close decides
later. **An autocommit write that parks before it has written a row
aborts its own transaction and begins another when it re-runs**: the
park is refused as the statement's verdict (`WriteWalk::ParkedOutcome`,
`command_dispatcher.cpp:10448-10451`) and `EndWrite` aborts every failed
autocommit write (`:12434-12436`); a relation-lock wait and a structural
re-run end the same way. One that parks after writing keeps its
transaction across the park and resumes it (`RunWalkWrite`,
`:9629-9640`). So one client statement can make several aborts, and the
lock-holder transaction a `CREATE ASSERTION`, `CREATE TRANSITION`,
`DROP TRANSITION` or a rename takes is always rolled back
(`DdlBuildLock::End`, `:4155-4160`). The log's three commit counts are
per core; a staged `strict` commit a failed sync aborts
(`FinishDeferredCommits`, `:1223`, not `EndWrite`) stays in
`strict_commits`. No
transaction, session or dispatcher holds a begin time: the listener
stamps one when `BEGIN` completes (`tcp_server.cpp:976-980`), and the
dispatcher can stamp its own at `HandleBegin` from the statement's head.
**The four counts of §2 do not partition these; back at the operator as
DF-Q13.**

**6. The read horizon (part B).** A core's bound is recomputed whole at
every publication: `PublishCoreBounds` scans the core's live transactions
and its 256 reader slots (`src/txn/manager.cpp:632-673`), and the word
has two writers, `PublishSnapshotBound` and `LowerSnapshotBound`
(`src/txn/instance_visibility.cpp:55-72`). It is written up to three
times a point `SELECT` and about five a point `UPDATE`. No view, lease or
transaction records when it was minted, and the manager has no clock. An
age therefore needs a mint stamp on every held view (a field beside
`ReadView`'s LSN, an array beside the reader slots), the scan returning
the oldest stamp, a store beside each of the two writes, and the
statement's head instant handed to the mint. `HorizonLsn`'s loop can name
the core of the minimum, which is briefly a core mid-mint. The session's
two words are reachable wherever a transaction opens or a view is minted,
and `Session::Finish` and `Expire` clear them. This is how DF-S2 builds
DF-Q8 (a); the spike priced the stores and not the scan.

**7. The tests that pin a widened view.** None pins a column list, count,
position or whole row of the five views or of the `txn` section: every
reader projects, looks a column up by name or compares `SHOW` with its
own `SELECT *`. No test lists every view. **New columns of
`sys.statement_stats` go before `shape_text`**, which is last because it
holds commas (`sys_views.cpp:1001-1016`). `sys.access_stats`' row is the
persisted 33-byte struct, which is why its two columns wait for DA-S1.
`StatementStats::Record`'s callers in `tests/statement_stats_test.cpp`
break only if its signature does.

**8. The price `[measured]`**
(`bench/v3.4.12/results-df-s0-spike-v3.4.12-69-gb2a4f70a.md`). One binary,
the spike's paths behind a mask read from the environment: A and A2 with
none, B with all, C with all but the wait-statistics delta. `cores = 1`,
`relaxed`, one connection, a point `SELECT` and a point `UPDATE` over the
literal and the prepared path, 15-17 paired runs a cell, server CPU a
statement:

| cell | A | B - A, every spiked path | B - C, the wait delta alone | largest A2 - A |
|---|---|---|---|---|
| `SELECT` literal | 20.46 us | +0.18 us (B above A in 15 of 17) | +0.27 us | 0.70 us |
| `SELECT` prepared | 19.78 us | +0.35 us (13 of 15) | +0.23 us | 0.69 us |
| `UPDATE` literal | 22.05 us | +0.23 us (15 of 16) | +0.07 us | 0.41 us |
| `UPDATE` prepared | 21.27 us | +0.11 us (12 of 17) | +0.05 us | 1.00 us |

**Every median is under DF-Q10 (a)'s bar of 0.5 us and none is resolved
from the run's own noise**: the largest gap between the two identical
arms is as large as the distance from a median to the bar. The cost is
there by sign and not placed by size. The wait-statistics delta is most of the cost
on a read and inside the bar by its median, so DF-Q6 (a)'s first branch
stands; it was spiked as one copy at the head and a diff at the end, not
two copies.

What the number is not. A is the spike's binary with its mask at 0, so
the residual branches and the larger shape and session are in A as well,
and B - A under-prices against a build with no spike by a residual that
was not measured. The spike prices the work and not the design's
attribution (a thread-local for the log bytes, dispatcher members for the
executor's totals). **Part C's per-index fold ran in no cell**: the
driver's relation has no secondary index, the `SELECT` is a tuple key
lookup and the `UPDATE` runs no chain, so bit 1 priced the executor's
totals on the `SELECT` and the fold's latch and map not at all. The latch
of a part was never contended. **`cores = 8`
was not run**: `tools/bn_census.py` writes `cores = 1` and no unmodified
tool runs these arms at eight cores with the server's CPU accounted
(DF-Q14). A fourth invocation was cut to hold the budget, the
measurement ran 14 s past 20 minutes, and seven cells that a foreign
build or host load made unclean were dropped from the paired tables. Not
priced because they are off a point statement's path: part C's lock
columns, written where a wait is already recorded, and part D, the
steward's and the division sites'.

**9. Taken back to the operator, and marked.** (a) was CLA's proposal in
each; **each is marked (a), the operator's mark of 2026-11-18**
(`raft-marks-2026-11-18.md` §37). DF-Q12 (a) replaces DF-Q7 (a)'s
mechanism and its scope - a record under no transaction is then the
statement's own - DF-Q13 (a) replaces the four counts of §2's part B, and
DF-Q11 (a) discharges DF-Q4 (a)'s condition by accepting what failed it;
§2 and §4 keep their text as written and are read with this table.

| | item | (a) | other |
|---|---|---|---|
| DF-Q11 | `DELETE`'s shape, DF-Q4 (a)'s condition having failed (3). | built with the three movements accepted and stated in `protocol.md` and `wait-events.md`: a prepared `DELETE`'s `S_PARSE_OK` carries its `pattern_id` as an `UPDATE`'s does, `ANALYZE DELETE` is counted under the shape it names, and `sys.ash` samples a `DELETE` under its `fetch_id`; `DeleteInner` keys through `ParseKeepingShape`; the three pinning tests move with it | (b) DF-Q4 (b): `DELETE` stays one row at `fetch_id = 0` and no fingerprint moves |
| DF-Q12 | Where a statement's log bytes are counted, DF-Q7 (a)'s mechanism having been priced by the census (4). | booked at `AppendThrough` to the session of the task being polled, as a wait is, and read as part of DF-Q6's delta: no lookup, no helper changed, and what the statement appends under no transaction is its own. A statement outside a task of its own session records NULL, as its time split does | (b) DF-Q7 (a) as marked, by a lookup of the id at every append, `TXN_BEGIN` counted at `Begin` and `ASSERT_PENDING` excluded; (c) the object threaded through the logging helpers |
| DF-Q13 | Part B's counts (5). | six, each where its cause is known: `commits` at `FinishCommitBody` (a `READ ONLY` transaction's `COMMIT` decides nothing and is not in it); `rollbacks`, the client's `ROLLBACK`; `ceiling_rollbacks` and `close_rollbacks`, the listener naming its cause on the session before it dispatches; `aborts`, a transaction rolled back because a statement or a commit failed - an autocommit statement's, an autocommit or explicit `COMMIT`'s, a deferred `strict` commit's whose sync never came; `wait_aborts`, the transaction an autocommit write gives up to wait before it has written a row, counted when it is given up, whether the wait ends in a re-run or in the fault net's refusal. The lock-holder transaction is in none | (b) the four of §2, `aborts` then counting every re-run and every close; (c) `commits` and one `rollbacks`, no cause |
| DF-Q14 | What DF-Q10 (a) asked priced and DF-S0 has no data for: `cores = 8`, and part C's per-index fold, which no cell of the driver reaches (8). | accepted for DF-S0; the close measures both, with a `cores` flag and an index-served read added to the census driver in a commit of its own before the close, outside any measurement stage | (b) the driver changed and the spike re-measured, both cells, before DF-S1 opens; (c) `cores = 1` and a tuple key read are the bar's only cells |

**What DF-S0 did not do.** It did not read the leaf-division and
compaction sites part D counts at, nor whether a statement outside a task
of its own session occurs on the KWP path - both are DF-S4's and DF-S1's
first reads. It did not measure a contended part, a scan, an `INSERT` or
a `DELETE`. For DF-Q12 (a) it read only this much: the WAL already opens
`sched::WaitScope` (`src/wal/checkpointer.cpp:606`, `:648`) and the
engine is one library, so no layering moves; an append runs on its
caller's thread under the stream latch (`src/wal/stream.cpp:390-394`),
where `WaitContext().current` is the polled task, the session's for a
statement the listener submitted (`tcp_server.cpp:738`) and asked by
`sched::RunningTaskBooksTo` (`src/sched/wait_state.cpp:87-90`). The
sweep's and the close's `ROLLBACK` are dispatched outside that session's
task (`tcp_server.cpp:945`, `:1113`); what their appends are booked to is
not read.

**Reviewed by `critics-developer` before the commit.** It opened every
cited line of this entry, read the spike and re-derived the table from
the run's analysis. **What it found wrong in CLA's draft, each corrected
above**: a write's `WHERE` subquery, and a `DELETE ... USING`, can be
served by an index through a chain whose counters are thrown away, so
"never by a secondary index" and "every read an index serves" were false;
the query layer's walks, an `INSERT ... SELECT`'s source and an `UPDATE`'s
scalar subquery do reach `RecordExecution`, so only a statement-wide
`ExecStats` is missing; a `DELETE`'s shape moves three behaviours, not
two, `sys.ash`'s `fingerprint` among them; a parked write gives its
transaction up only if it has written no row; **and the spike's per-index
fold ran in no measured cell**, so the draft's "all of DF" over-claimed -
the table, the results file's title and thesis and DF-Q14 now say so. In
the results file it corrected four numbers quoted from the wrong analysis
table, completed the account of host load, and put the unit into every
cell. From its reading of DF-Q13, CLA rewrote option (a): `commits` does
not hold a `READ ONLY` commit, `aborts` takes a failed explicit `COMMIT`
and a deferred commit's abort, and `rerun_aborts` became `wait_aborts`,
since the site sees a wait begin and not a re-run. **Not taken**: dropping
the build's log from the archive - it is a log, not data, and says what
was built. **Left unverified by the review, and so by this entry**: the
count of writes of a core's bound a statement (6), "about fifteen" helpers
(4), that no test pins a view's columns (7), and that `HorizonLsn` can
name the core (6); each is one census's reading.

### DF-Q11..Q14 marked (a); merged with `origin/main` at `5038f999`; handed off, 2026-11-18

On the same worktree and branch, from `011d1609`, documents only. The
operator's word on DF-S0's report (`raft-marks-2026-11-18.md` §37) marks
the four items (a) and asks for the push with the hook skipped and a
handoff. `origin/main` had moved to `5038f999` (CX-S1b, CX-S2) and was
merged on the work branch; the opening's record took §36, §35 having gone
to that session. **That merge brought source this session did not build
or test**: `src/server/command_dispatcher.cpp`,
`include/kds/server/command_dispatcher.hpp`, `src/server/sys_views.cpp`
and `src/exec/step_compiler.cpp` among it, so the line numbers this order
cites in those files are `6e7f7095`'s and DF-S1 re-reads them, and `sys.statement_stats` gained `plan_source`,
`plan_name` and `variant` from CX-S1b, which DF-S1's widened shape sits
beside. No suite was executed. **Next: DF-S1**, part A - not started.

### DF-S1 - part A, 2026-11-18

On worktree `da-q8-memory-bound-key`, branch
`worktree-df-statistics-coverage`, from `6f52c652`, on the operator's word
(`raft-marks-2026-11-18.md` §39). Three commits and a merge: `9f9b207b`
(`DELETE`'s shape, a commit of its own as §5 asks), `44dddd27` (the widened
shape and the two views), `63395374` (the merge of `origin/main` at
`2ca42fea`, CX-S3a, clean) and `ee89842e` (the review applied).

**What is built**, as `docs/spec/sys-views.md` §6.1, §6.1a and §6.1b now
state it. `delete` is a patternable leading word and `DeleteInner` parses
through `ParseKeepingShape` (DF-Q11 (a)); its three movements are in
`protocol.md` §5 and `sys-views.md` §6.1 and §6.7 - **not in
`wait-events.md`**, which DF-Q11 (a) names: `sys.ash`'s `fingerprint`
column is described in `sys-views.md` and nowhere in that file.
`sys.statement_stats` carries sixteen columns between `variant` and
`shape_text`; `sys.statement_histogram` and `sys.refusal_stats` are new
views with no `SHOW` spelling. A shape's slot is 624 bytes, from 184, in
the node map BO built, the count bound unchanged (DF-Q1 (a)).

**How, where §2 left it open.**

- **The split's source** (DF-Q6 (a)): one folded sample of the session's
  statistics at the statement's head and one at its end
  (`sched::SampleSessionCost`: CPU, time by wait class, the three read
  counts, the log's two words - 120 bytes, a local of `Dispatch`'s and
  `DispatchAsync`'s own frame), each behind a `FlushOnCpu`, taken only
  where `RunningTaskBooksTo` holds; the difference is the call's.
- **The log** (DF-Q12 (a)): `sched::BookLogAppend`, called once in
  `WalManager::AppendThrough`, adds to two words on the polled task's
  `SessionWaitStats`. No view shows the pair.
- **`rows` and the chain's counts ride `DispatchOutcome`**
  (`rows_returned`, `examined`, `compile_ns`), set by the handler that
  returns the statement's outcome, so a nested `SELECT`'s - a scalar
  subquery's, a query-layer walk's - are dropped with its outcome.
  `compile_ns` goes through a dispatcher member reset and exchanged in
  `StageStatement`, as `fetch_id` does, and is kept across the two places
  that rebuild an outcome (`SelectRun::Execute`, a refused group commit).
- **A refusal's code** is read with no `Status` built (`RefusalCodeOf`),
  so the record allocates nothing (DF-R2).

**What was built differently from §2's text, or decided where it was
silent - each CLA's, none marked by the operator, each stated in
`sys-views.md` §6.1:**

1. **The split is seven columns that sum to DB time, not "one column a
   wait class a statement's task can wait in"**: `cpu_us`, `sched_us`,
   `commit_us`, `io_us`, `net_us`, `app_us`, and `other_us` for every
   other class. Three classes have no site in a statement's task today
   (System I/O, CrossCore, Resource; Concurrency is reserved), and a
   column each would read 0 for ever; `other_us` keeps the sum true if one
   gains a site.
2. **`compile_us` is measured from the run's head, not the statement's**,
   by one more clock read a run: a re-run's waits would otherwise be in a
   compile time.
3. **`rows` of a `SELECT` is what it gave its sink**, not what a KWP
   `max_rows` let leave; a refused call adds none.
4. **`rows_examined` and `pages` include a fold's and an `ANALYZE`'s
   chain**, beside a native `SELECT`'s, and are NULL for every write and
   for a query-layer `SELECT` (DF-S0's census, item 2; the layer's walks
   are not summed).
5. **`sys.refusal_stats` is keyed by the wire's pinned category**, the
   code the client was told, in a latch-free table of single-writer words
   - the one statistics store with no latch. The registry has no count to
   assert against, so the bound (64) is asserted above its last entry
   today.
6. **No count column beside `rows_examined` or `compile_us`**: the store
   keeps `executed_calls` and `compiled_calls` to decide NULL, and the
   view shows only `timed_calls`, which §2 names.

**DF-S0's two first reads.** A statement outside a task of its own
session does not occur on a listener's path: the listener hands the
session's statistics to every statement's task at its submit
(`src/server/tcp_server.cpp:738`), KWP and the newline protocol alike. It
occurs for a synchronous `Dispatch` from no task - tests, and whatever
calls it off a reactor. The listener's sweep and close `ROLLBACK` go
through `DispatchInternal` (`tcp_server.cpp:945`, `:1113`), are recorded
as no statement, and what they append is inside no statement's two
samples: no shape's.

**Reviewed by `critics-developer`** at `63395374`; it ran 59 cells and
edited nothing, on CLA's instruction (a build was running). **Applied**
(`ee89842e`): **one wrong number** - an `INSERT ... ON CONFLICT DO UPDATE`
took a `compile_us`, its arm's `WHERE` compiling through `UpdateParsed`
once a conflicting row; a refused group commit dropped the statement's
compile; `rows` of a refused call was 0 by every refusal path's luck and
is now so by the record; the five timed fields, kept three times and
walked by three loops, folded into `SessionCostSample` with an
`operator-` and an `operator+=`; `ExaminedBy` made `ExecStats::Total()`'s;
the head carries the statistics pointer. **Cells added on its reading**:
the conflict arm; a `RETURNING` write's rows counted once; every split
column against a value of its own; a handed-on walk (`DispatchAsync`, a
scan) keeping its compile and its counts; two sessions of one core, their
`group` commits parking across each other, whose logs add up to what the
core appended. Two mutations were run against the new cells and each
failed its cell: the conflict arm's guard removed, and the walk's
`compile_ns` not carried across its reply. **Not taken**: a count on
`wire::ErrorCategory` to assert the store's bound against - the header
states the limit instead, and a registry of 64 categories is not near;
`WaitBucketOf(elapsed_us * 1000)`, cosmetic; a narrower copy for
`sys.statement_histogram`'s read, which is off a statement's path and is
in `known-gaps.md`. **Left unread by the review**: CX-S3a, the KWP portal's
`max_rows` path, the query layer beyond `WalkForLayer`, a `tsan` run.

**Tests §5 owes for part A and DF-S1 did not build.** Every store empty
after a process restart and a restore: the stores are process-wide
statics as BO's are, no cell starts a process, and an in-process restart
keeps them (`known-gaps.md`, BO). A deadlock's victim in `conflicts` and a
KWP `max_rows` portal's `rows`: no cell. **The two-core cell was not run
under `tsan`**: the nightly's clang is not on this host
(`scripts/bs/config.sh`'s `KDS_LLVM`), so it runs there first under the
nightly.

**Suite.** The full suite - `ctest -LE bs-nightly` - 0 failed of 4700
at `ee89842e`, on the merged tree (two disabled and two skipped, as before
the stage); before the review, 0 failed of 4696 at `63395374`.
**Overhead not measured; measured at the milestone's close** (DF-Q10 (a),
DF-Q14 (a)); DF-S0's spike priced one cost-faithful way to do this stage
and is not this code. **Documents**: `sys-views.md`, `protocol.md`,
`manual/sql/sql.md`'s statistics reports, `known-gaps.md` and
`CLAUDE.md`'s observability row are amended with the stage; the rest of
§5's list is DF-S5's. **Next: DF-S2**, part B - not started.

### DF-S1 pushed; handed off, 2026-11-18

On the operator's word (`raft-marks-2026-11-18.md` §40) DF-S1 went to
`main` with `--no-verify` from `0e6605ce` and this record: the hook was
skipped, the suite that stands behind the push is the full suite at
`ee89842e` (0 failed of 4700), and nothing after that commit changes a
build input. The operator was shown the six things above that were built
differently from §2 and marked none; they stand as CLA's until one is.
The cells §5 owes for part A and the two-core cell's `tsan` run are still
owed (`known-gaps.md`). **The session handed off here. Next: DF-S2**, part
B - not started, and it starts on the operator's word.

### DF-S1's overhead measured, 2026-11-19

DF-S1 landed with its overhead unmeasured. It is measured now, by the
bisect of CX's close regression
(`bench/v3.4.13/results-cx-close-bisect-v3.4.13-1-g11f72ce9.md`, worktree
`cx-close-regression-bisect` at `11f72ce9`): **that regression enters at
DF-S1's step**, in the one server lifetime measured with an arm at every
stage (`58d258a3` to `ee89842e`: +0.25, +0.57 and +0.79 µs of server CPU on
a point read and a two- and three-relation tuple key join, `cores = 1`).
Pooled over four lifetimes, `main` at `11f72ce9` costs +0.44, +0.31 and
+0.39 µs more than `58d258a3`, 0.7 to 0.8 % of throughput against CX's
opening commit. That is at and above the top of DF-S0's forecast (+0.11 to
+0.35 µs, which priced no join cell). **Against DF-Q10 (a)'s 0.5 µs it is
under by the pooled median and not resolved from it**: the pooled intervals
reach +0.54 and +0.57 µs, and the single lifetime's step is over the bar on
both joins. **The operator's reading, 2026-11-19
(`raft-marks-2026-11-19.md` §4): DF-S1 stands as built with this cost and
is not reverted.**

A spike that compiles the per-statement pieces out (`spike.diff` in that
file's archive) says: the compile-time clock (`NoteStatementCompiled`) costs
nothing measurable; the session cost sample at the head and the end
(`BeginStatement`, and `RecordStatement`'s second `FlushOnCpu` and
`SampleSessionCost`) is the likeliest carrier, at a plausible +0.1 to
+0.3 µs, and is not separated from what a binary's layout moves (0.2 to
0.3 µs); what no knob removes - the wider `DispatchOutcome`, the latency
bucket, the emit loop's count, `ExaminedBy` - is not resolved. **Not
measured**: a write's share (`BookLogAppend` is reached by no `SELECT`),
`cores` above 1, and any remedy. A cheaper sample - class totals kept
where a wait is booked - is CLA's proposal in that file and nothing is built
on it: it is the operator's to open, and §4 of that day's marks does not
open it.

### DF-S2 - part B, 2026-11-19

On worktree `da-q8-memory-bound-key`, branch
`worktree-df-statistics-coverage`, fast-forwarded from `3adf8810` to
`origin/main` at `8e7c820f`, on the operator's word
(`raft-marks-2026-11-19.md` §5); the operator's standing go-ahead for the
rest of DF came while the stage's first build ran (§6 there).

**What is built**, as `docs/spec/sys-views.md` §6.1c, §6.6 and §7 now
state it. `sys.meta`'s `txn` section carries six counts of how the core's
read-write transactions ended and the instance's read horizon in three
fields; `sys.txn_histogram` is a new view with no `SHOW` spelling;
`sys.session_state` has `txn_age_us` and `view_age_us`. One store,
`stats::TxnStats`: 560 bytes a core, single-writer relaxed words, no latch.

**How.**

- **The counts** (DF-Q13 (a)). `commits` is counted in
  `TransactionManager::FinishCommitBody`, the one site that commits. The
  other five are counted by the dispatcher where the cause is known:
  `RollbackLocal` takes the cause - the client's by default, the cause the
  listener named on the session (`Session::set_rollback_cause`) before it
  dispatched its own `ROLLBACK`, or `aborts` from a failed `COMMIT` - and
  `AbortOwnedScope` takes `aborts` or `wait_aborts` from `EndWrite`, which
  is where a statement that wrote nothing and is about to wait or run again
  is told from one that failed. A deferred commit a stopped log aborts is
  counted in `FinishDeferredCommits`. `DdlBuildLock::End` counts nothing.
- **The life** is recorded by `FinishExplicitTxn`, the one call through
  which an explicit transaction leaves its session, from the head of its
  `BEGIN` kept on the `Session`.
- **The horizon's age** (DF-Q8 (a), DF-Q15 (a)). Every held view carries the
  instant its minter stamped it with: `now_ns` on `Begin`,
  `StartStatement`, `RegisterReader`, `RefreshReader` and
  `AutocommitSnapshot`, kept on the `Transaction` and beside each reader
  slot. `LocalSnapshotBound` returns the bound with the stamp of the view
  that holds it, `PublishSnapshotBound` stores the stamp beside the bound -
  the bound's own store is as it was - and
  `InstanceVisibility::HorizonHeldBy` names the core. The stamp is the
  dispatcher's `statement_head_ns_`, which DF-S1 already reads: **no clock
  read is added to a statement's path**. The two reads that are added are
  off it: one a slice of an online index build and of a declaration's
  build, one a KWP load chunk.
- **A session's two ages** are two words on its `SessionWaitStats`, written
  by `Session` where its transaction begins, where its statement mints a
  view, and cleared where the transaction ends and where an autocommit
  statement is recorded.

**DF-S0's census, corrected by the build.** Its item 6 has a view's stamp
as "a field beside `ReadView`'s LSN"; the stamp is on the holder instead -
the transaction, the reader slot - so `ReadView`, which is copied into
every snapshot, is the size it was. Its count of the writes of a core's
bound a statement stays unverified; the stage adds one relaxed store to
each.

**Six items the stage raised are DF-Q15..DF-Q20** (§4), each adopted as
CLA proposed on the operator's standing go-ahead of 2026-11-19, none the
operator's own mark.

**Reviewed by `critics-developer`**, report only, on the uncommitted tree:
it walked every caller of `Commit`, `CommitDeferred`,
`FinishDeferredCommit` and `Abort` and found each end in exactly one count
and each life recorded once, the bound's store and the horizon unchanged,
and no race beyond the documented pairing of two stores. **Applied**:
**one wrong number** - a KWP load chunk is no staged statement, so the view
it re-mints was stamped with whichever statement its core staged last, and
a long load, the transaction likeliest to hold the horizon, could read as
old as its whole run; `ExecuteInsert` takes a head instant of its own. The
ranking of "who holds the bound", written once for a core's views and
again for the instance's cores, is one type (`txn::HeldBound`); the record
of a life and `Session::Finish`, paired at five sites, are one call;
`NoteTxnBegan` stamps the view it always came with; the rollback cause is
read, not exchanged, `Finish` and `Expire` already dropping it; the view's
word is set once its lease is held. In the documents: a caveat in
`known-gaps.md` that named waits which are before a statement's head, the
three places that said a reader could see `none`, a sentence that read as
"the store is not emptied", and the load stream's two ends no document
listed. **Not taken**: reading the lifetime sweep's stamp from the session
in place of the listener's own - it moves the ceiling from `BEGIN`'s
completion to its head and is not DF's; an opaque declaration of `TxnEnd`
in `session.hpp`, whose default needs the enumerator; one helper for three
saturating subtractions. **Read by the review and not reproduced**: a
statement whose descent noted a stale path and which then failed for
another reason, having written nothing, is a `wait_abort`
(`known-gaps.md`). **Seen beside the stage and not DF's**: a load chunk
does not reset the dispatcher's statement-boundary flag, so whether it
re-mints its READ COMMITTED view depends on the statement its core ran
before it; read by the review, not verified, no entry filed. **Left unread
by the review**: the cells in `tests/txn_manager_test.cpp` and
`tests/show_sugar_contract_test.cpp`, the two CMake lists, DF-R1..R9 one
by one, and whether a `ROLLBACK` can be refused before its handler.

**Tests.** `tests/txn_stats_test.cpp` (the store);
`tests/txn_stats_rig_test.cpp` on the two-core rig - a commit, a client
rollback and an engine abort each moving one count of their core, a
`READ ONLY` transaction moving none, a named cause counted once, a write
that waits counted a `wait_abort` and not an `abort` across two reactors,
a life recorded once by access mode, the horizon's age growing while a
`READ ONLY` transaction is held on the other core and returning when it
ends, and a READ COMMITTED transaction's view as old as its last
statement; cells in `tests/txn_manager_test.cpp`,
`tests/instance_visibility_test.cpp` and
`tests/show_sugar_contract_test.cpp`; and one on a whole instance,
`ExpeditorTest.TheSweepAClosedConnectionAndTheClientEachCountTheirOwnRollback`,
the listener's sweep and close through the debug text port. **Owed**
(`known-gaps.md`): a failed `COMMIT`, a deferred commit whose sync never
comes, a load stream's ends and its chunk's stamp, a structural re-run;
the two-core cells under `tsan`, which this host cannot build; the store
empty after a process restart and a restore.

**Suite.** The full suite - `ctest -LE bs-nightly` - 0 failed of 4766 on
the tree this stage's commit holds (two disabled and two skipped, as
before the stage); before the review's fixes, 0 failed of 4766 too.
**Overhead not measured; measured at the milestone's close** (DF-Q10 (a)):
the stage adds to every statement's path a stamp where a view is minted
and published, two words on the session, and a count where a transaction
is decided. **Documents**: `sys-views.md`, `txn.md` §1,
`manual/sql/sql.md`, `known-gaps.md` and `CLAUDE.md`'s observability row
are amended with the stage. **Next: DF-S3**, part C.

### DF-S3 - part C, 2026-11-19

On the same worktree and branch, from `c2925b6e`, on the operator's
standing go-ahead (`raft-marks-2026-11-19.md` §6).

**What is built**, as `docs/spec/sys-views.md` §6.2a and §6.2b now state
it. `sys.indexes` has five usage columns - `probes`, `ranges`,
`entries_scanned`, `rows_resolved`, `last_used` - and `sys.relation_stats`
four cumulative lock columns - `lock_waits`, `lock_wait_us`,
`lock_deadlocks`, `lock_fault_nets`. **The access shape's two columns are
not built and are owed to DA**: DA-S1 has not landed, the row is still a
33-byte persisted struct, and widening it is a format event (DF-R1), as §5
provides.

**How.**

- **Index usage** is folded in `CommandDispatcher::RecordIndexUse`, called
  from `RecordExecution`, over every step of the chain `exec::ForEachStep`
  visits, from the step's `StepStats`. The store, `stats::IndexStats`, is
  the instance's: `Expeditor` owns one and hands it to every core through
  `OptimizerSurface`, and a dispatcher handed none uses its own (DA-R6's
  reason, as §2 asks). A chain with no index step pays the walk of its
  steps and nothing else; the wall clock and the part's latch are touched
  at the first index step that walked.
- **`probes` and `ranges` are the index's walks** (DF-Q21 (a)), a new
  per-step counter bumped where the index is sought,
  `StepStats::index_walks`, printed nowhere.
- **The lock columns** are counted in `LockWaitLog::Record`, the one place
  a `sys.lock_waits` row is written, into `stats::RelationStats`, so they
  keep what that ring overwrites.
- **One store shape, once**: `stats::OidParts` is the latched, bounded,
  per-core map by oid that `RelationStats` had and `IndexStats` needed; both
  are built on it.

**Reviewed by `critics-developer`**, report only, on the uncommitted tree.
It checked that step ids and the `ExecStats` are the statement's at all
three call sites, that only a completed run folds, that every lock wait
row goes through the one call with no latch nested, and the store's
lifetime against the cores that hold it. **Applied**: **one wrong count** -
`probes` and `ranges` were the step's openings, and a step that names an
index can walk its relation instead (the `indexes` key off, an index
dropped under a compiled chain, a correlated probe that could not key),
which credited an index the run never read; and a value list of N values
counted one. Both are DF-Q21 (a)'s counter. `IndexStats` re-wrote the
find-or-insert this stage had just factored out of `RelationStats`: one
template, the second copy and the eight hand-listed merges deleted. In the
documents: "a refused statement folds nothing" holds a run and not a
statement, since each walk of a query-layer statement is a run; "a write
uses none" was too strong for `INSERT ... SELECT`'s source; `ANALYZE`'s run
is a use; `relation_stats.hpp`'s header still described writes alone.
**Not taken**: a null pointer in place of each dispatcher's own store,
6 KB a core the instance never uses - it is `own_plan_advices_`'s idiom.
**Left unread by the review**: `SelectRun::Execute` whole and a KWP portal
closed mid-run, the `UPDATE ... FROM`, `DELETE ... USING` and `SET`
subquery paths, the allocation sweep's failure model
(`tests/bs/alloc_sweep_test.cpp` reads `sys.indexes`, and
`IndexStats::Snapshot` can throw there as `RelationStats::Snapshot` can),
and the compiler's key set on an index.

**Tests.** `tests/index_stats_test.cpp` (the store);
`tests/index_usage_rig_test.cpp` on the two-core rig - an index no read
names at 0 with `last_used` NULL, an equality read on each core summed in
one row read from either, a value list one probe a value, a strict range
walked and counted nowhere, an inclusive one a `range`, `ANALYZE`'s run a
use, a write maintaining both indexes and using neither, a rebuilt index
at 0; and the lock columns on `LockWaitsViewTest`'s granted, fault-net and
deadlock cells, the first read again after the ring is emptied. **Owed**
(`known-gaps.md`): a step under an outer step, a sub-chain's and a hoisted
one's, a step that walked instead of reading its index; the two-core cell
under `tsan`.

**Suite.** The full suite - `ctest -LE bs-nightly` - 0 failed of 4773 on
the tree this stage's commit holds (two disabled and two skipped, as
before); before the review's fixes, 0 failed of 4773 too. **Overhead not
measured; measured at the milestone's close** (DF-Q10 (a), DF-Q14 (a)): an
index-served read pays the fold's latch once an index step, and every
recorded `SELECT` the walk of its steps. **Documents**: `sys-views.md`,
`index.md`, `manual/sql/sql.md`, `known-gaps.md` and `CLAUDE.md`'s
observability row. **Next: DF-S4**, part D.

### DF-S4 - part D, 2026-11-19

On the same worktree and branch, from `83aad532`, on the operator's
standing go-ahead (`raft-marks-2026-11-19.md` §6), under DF-Q9 (a).

**What is built**, as `docs/spec/sys-views.md` §6.2c and
`docs/spec/plan-stats.md` §6 now state it. `sys.relation_stats` has
`est_delete_marked`, `leaf_divisions` and `leaf_compactions`;
`sys.indexes` has `leaf_divisions` (DF-Q23 (a)). §8 of `sys-views.md` keeps
BO-Q13's exclusion for page counts and spills alone.

**How.**

- **The estimate.** The steward's read of a leaf for a prior (`SlotsOf`)
  counts its delete-marked slots beside its live ones;
  `ReadRelationPrior` hands the scaled total out beside the prior, and
  `PlanStats::Seed` keeps it in a side table of its own under a latch of its
  own, which `PlanStats::EstDeleteMarked` and the view alone read. The seed
  reads no page it did not read before. **DF-R8 holds by construction**:
  the table is written after the prior's own entry and beside it, sets no
  `dirty_`, is in no snapshot, export or load, and no plan can reach it.
- **The counts.** `stats::LeafStats`, the process's, on `stats::OidParts`,
  keyed by the oid that owns the tree's pages and counted in the storage
  layer: a clustered division and a compaction in `PlaceUnderHold`
  (`btree.cpp`) once the act returned, an index's leaf split in
  `IndexInsert` (`index_tree.cpp`) once its entries moved. The append path
  counts nothing. A mount's replay applies images and reaches neither site.

**Reviewed by `critics-developer`**, report only, on the uncommitted tree.
It proved DF-R8 path by path - the one writer, the one reader, every
publish, export and load - that the prior's value is unchanged in every
branch and `GATHER`'s leaf count the same number, the scaling in each
branch of the read, the two latches never nested, and each count site
against its callers, the redo and the undo. **It found no defect in the
code.** **Applied**: the store's header claimed a division that fails
after it began is counted, which is true of an index's and false of a
clustered one's - reworded to what each site does; the freshness the
documents claimed ("about every 10 s", "a few seconds old") is the
steward's floor and not its period - restated with the budget that sets
it; "the process's, as §6's others are" was false of the instance-owned
index store; the manual said "since the instance started"; the estimate is
now written after the prior's entry, so an allocation failure between them
cannot leave an estimate no erase finds; the out-parameter is an optional
and not a sentinel; `NodeFacts` holds the leaf's counts once. **DF-Q22**,
which the review raised, is in §4. **Not taken**: folding a relation's
counts into `RelationStats` (DF-Q22 (b)). **Left unread by the review**:
`TryReadTupleIn`'s reading of a retired slot, `PromoteSeparator` and
`SecureParents`, the `REINDEX` and online-build paths beyond their use of
`IndexInsert`, and whether `InitTableAccess` fails for a tombstoned
relation, which the drop's cell asserts.

**Tests.** `tests/space_stats_test.cpp`: a relation's estimate NULL before
a seed, exact for one leaf, 3 after three rows are delete-marked with the
planner's generation unmoved (the cell fails if the estimate sets
`dirty_`), 0 after a `PURGE` and a later seed, gone with a dropped
relation; a relation of 600 rising keys at 0 divisions and 1 after a key
arrives inside its full first leaf; a full leaf with a purged slot
compacted and not divided; an index's leaf splits its own and not its
relation's, a rebuild's counted to the new oid. **Owed** (`known-gaps.md`):
an estimate over a tree of height 2 or more, a division on a second core,
a division that fails, the store's bound.

**Suite.** The full suite - `ctest -LE bs-nightly` - 0 failed of 4777 on
the tree this stage's commit holds (two disabled and two skipped). **The
run before the review's fixes failed one cell of 4777**,
`RowWaitCeilingRigTest.AParentDeleteWaitsForTheLiftOfAMarkerOnItsOwnCore`,
at its 1 s bound on the marker's lift while the review ran beside the
suite; it passed 30 of 30 alone on the same binary and in the run after.
It reads no statistic this milestone adds; no entry is filed, and it is
named here so a second failure is not met as a first. **Overhead not
measured; measured at the milestone's close** (DF-Q10 (a)): the stage adds
nothing to a point statement's path - a latch at a division, a count in
the steward's leaf read. **Documents**: `sys-views.md`, `plan-stats.md`,
`manual/sql/sql.md`, `known-gaps.md`, `CLAUDE.md`'s observability row.
**Next: DF-S5**, the close.

### DF-S5 - the close's measurement and its three remedies; the bar not resolved, 2026-11-19

On the same worktree and branch, from `29810582`, on the operator's
standing go-ahead (`raft-marks-2026-11-19.md` §6). **Every stage of DF is
on the work branch. DF is not closed**: DF-Q10 (a)'s bar is not resolved
on a point read with its central estimate at the bar, and DF-Q10 (a) sends
that to the operator (DF-Q24). Nothing is pushed.

**The driver first** (DF-Q14 (a)), a commit of its own, `ec5ddfe8`:
`tools/bn_census.py` takes `--cores` and has `select-index`, an equality a
secondary index serves.

**The measurement** `[measured]`, by `ck-tester`, four files under
`bench/v3.4.13/`, each with its archive. Release `kds_server` from
`git archive` exports, `cores = 1`, `relaxed`, one connection, 1,000 rows,
ten server lifetimes a file with the baseline's binary run twice in every
lifetime (A and A') as that lifetime's floor, server CPU a statement, both
driver paths pooled, medians of block-paired deltas with 95 % intervals. A
is `8e7c820f`, `main` before DF-S2 with DF-S1 in it.

| file | arm against A | tuple key `SELECT` | index-served `SELECT` | `UPDATE` | `INSERT` |
|---|---|---|---|---|---|
| `results-df-close-v3.4.13-14-gec5ddfe8.md` | B = `ec5ddfe8`, DF-S2..S4 | +0.17 us [+0.13, +0.22] | +0.35 us [+0.30, +0.38] | -0.03 us | +0.07 us |
| `results-df-close-remedy-v3.4.13-15-g9f588fa2.md` | C = `9f588fa2`, the first remedy | +0.16 us [+0.13, +0.20] | +0.36 us [+0.31, +0.41] | -0.03 us | +0.02 us |
| `results-df-close-knobs-v3.4.13-14-gec5ddfe8.md` | B with the fold, the session words and the stamps compiled out together | +0.07 us [+0.00, +0.09] | +0.15 us [+0.11, +0.22] | - | - |
| `results-df-close-final-v3.4.13-17-gc16b4fc2.md` | D = `c16b4fc2`, the code as it stands | **+0.07 us [+0.02, +0.10]** | **+0.18 us [+0.12, +0.25]** | +0.02 us | +0.04 us |

B - A replicated in every later file within 0.07 us (+0.11 to +0.18 us on
the tuple key read, +0.33 to +0.35 us on the index-served one). The floor
A' - A pooled is -0.15 to +0.07 us by file and cell, and a single
lifetime's reaches 0.45 us: only ten lifetimes pooled resolve a read.

**Against the bar.** DF-S1's measured cost on a tuple key read is +0.44 us
[+0.27, +0.54] (above). With D - A the whole of DF is **about +0.51 us on a
tuple key read, a naive interval of +0.29 to +0.64 us: not resolved against
0.5 us, its centre at the bar**. On an index-served read DF-S2..S4 cost
+0.18 us and DF-S1 was never measured on that shape; on a write DF-S2..S4
are inside the floor and DF-S1's share was never measured. The two sources
are different drivers and durability classes and are not strictly
additive. **`cores = 8` was run for one lifetime and did not resolve**: two
servers of one binary stood 9 us apart and all four cells were unclean, as
AM-S6 found of this host; no delta is claimed.

**Three remedies, as the standing go-ahead asks of a regression at a close
(DF-Q24 (a)), and no fourth.**

1. **The session cost sample folded where a wait is booked** (`9f588fa2`),
   the cheaper sample the bisect of 2026-11-19 proposed for DF-S1's cost.
   C - B is +0.00 to +0.04 us on the reads; a saving above about 0.03 us on
   a tuple key read is ruled out. **It bought nothing and is reverted**
   (`35f8cce4`): the walk of the wait registry was not the carrier of
   DF-S1's cost, which stays unattributed.
2. **An attribution by knob binaries**, no engine change: B with the index
   fold, the session's two words and the mint stamps compiled out, each
   alone and all together. Together they remove about three quarters of
   B - A (+0.14 of +0.18 us, +0.24 of +0.33 us); **no single knob is
   attributed** - the removals sum to more than the whole, and a knob
   binary differs from its parent in layout by as much as the floor. The
   largest was the index fold's on an index-served read, +0.21 us
   [+0.18, +0.25].
3. **The index fold without a latch, a lookup or a clock** (`c16b4fc2`): an
   index's counts are single-writer relaxed words reached through a pointer
   the dispatcher keeps in an eight-slot cache, the part's latch guards its
   map alone, and `last_used` is the statement head's monotonic instant
   shown as a wall instant at the read. **D - B is -0.18 us [-0.22, -0.13]
   on an index-served read**, below zero in 8 of 10 lifetimes; -0.04 us,
   not resolved, on a tuple key read. Kept.

What no remedy reached: +0.07 us on a tuple key read that the knobs do not
remove either - the wider `StepStats`, the per-seek counter, the larger
`Transaction` and manager, a binary's layout - and all of DF-S1.

**Reviews.** `critics-developer`, report only, on each remedy's commit.
Of `9f588fa2` it found no defect and one test the fold had weakened; both
went with the revert. Of `c16b4fc2` it found no defect: the pointer's
stability across a rehash, that nothing erases or resets an index store,
the one swap of the store clearing the cache, the destruction order in
`Expeditor`, the rig and a dispatcher's own store, the words the two sides
share being atomics, and the stamp never 0 where a clock exists.
**Applied**: the template's contract that an address it hands out holds
until `Reset`, written where `Reset` is; a test helper reduced to what its
callers pass; a read on a second core through the kept pointer added to
the rig cell. **Not taken**: an unlatched `find` in the store in place of
the dispatcher's cache - it keeps a hash lookup a read and turns two
threads recording as one core from lost counts into a race on the map.
**Left unread**: which clock core 0's dispatcher holds, where oids are
issued (the cache reads oid 0 as empty), `sim/instance.cpp`'s wiring.

**Suite.** The full suite - `ctest -LE bs-nightly` - 0 failed of 4778 on
the tree this entry's commit holds (two disabled and two skipped); 0 failed
of 4778 at `c16b4fc2`, 0 failed of 4777 at `9f588fa2` and at `29810582`,
whose tree `35f8cce4` is again.

**Documents.** §5's list, each read: `sys-views.md` §5, §6 (the measured
cost beside BO's), §6.1c, §6.2a-§6.2c, §6.6, §7, §8; `plan-stats.md` §6;
`txn.md` §1; `index.md`; `known-gaps.md`, where "not measured" now carries
the numbers; `manual/sql/sql.md`; `CLAUDE.md`'s observability row.
`wait-events.md` names no column DF added and is unchanged. **The index's
DF row is not flipped to closed.**

**Time.** The four measurement rounds took about 44, 10, 20 and 15 minutes
wall, building included, of which the quiet gate held about 16 minutes in
the first and 1.5 in the third; together past the hour a close is given,
because three rounds followed the first's finding. Cut: `cores = 8` after
one lifetime of six; no `strict` class, no scan, no multi-statement
transaction, no per-stage binaries.

**Owed, beside each stage's list**: DF-S1's share on an index-served read
and on a write; `cores` above 1; the cells no stage built
(`known-gaps.md`); `sys.access_stats`' two columns, DA's.

**Next: the operator's reading of DF-Q24** - close DF with its measured
cost, as DF-S1 was kept on 2026-11-19, or ask for more; and the push.
