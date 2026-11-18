# Work order DF - the statistics the engine records, widened: what a statement shape costs, transactions and the read horizon, index and relation usage, space

Status: **WRITTEN 2026-11-18, not opened.** Written on worktree
`da-q8-memory-bound-key`, branch `worktree-df-statistics-coverage`, from
`56d20394` (`v3.4.12-53-g56d20394`) on the operator's word of 2026-11-18
(`raft-marks-2026-11-18.md` §33). The session was isolated in that worktree
and could not open one named for this order; the branch carries the name.

**DF-Q0..Q10 each marked (a), as CLA proposed, on the operator's word of
2026-11-18** (`raft-marks-2026-11-18.md` §34). No stage is started: the
word marks the items and does not open DF; the opening is the operator's.
**DF moves no format**: no superblock bump, no catalog relation,
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
  from `StepStats` where a statement's chain ends, for every statement that
  runs a chain; which statements those are is DF-S0's read.
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
