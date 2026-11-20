# Work order DL - statistics queue no statement, and a switched-off consumer collects nothing: the two rules established over every recording path, every statistics read a statement makes and every collection task, and checked

Status: **OPENED 2026-11-20 on the operator's standing go-ahead
(go-ahead-achieving-milestone, `raft-marks-2026-11-20.md` §11)**, on
worktree `dl-statistics-queue-no-statement` from `b0a824db`
(`v3.4.16-2-gb0a824db`). **No DL-Q item but DL-Q1 and DL-Q3 is the
operator's own mark**: every other one is CLA's proposal, adopted on that
standing word as §4's mark column records it, DL-Q12 as revised against
the tree, and DL-Q14..Q18 raised by DL-S0's census and written into §4
with their options before they were adopted. The stage log is §7.

Written in the main checkout on
`main` at `1aaafe1b` (`v3.4.16-1-g1aaafe1b`) on the operator's words of
2026-11-20 (`raft-marks-2026-11-20.md` §9, §10). **Four things are the
operator's word**: the rule (`docs/rules/rules.md` §3a), that its final
form is the strict one (DL-Q1), that a milestone of its own establishes
it - this order - and, **added the same day (§10), a second rule the
milestone establishes with it: a switched-off consumer collects nothing**
(DL-Q3). The letter DL, the clauses the first rule is read as, the
census, the shapes and the stages are CLA's proposals. No DL-Q item but
DL-Q1 and DL-Q3 is marked. No stage is started. Every `[source-read]` claim below was
read at `1aaafe1b`. No suite was run: the order changes no build input.

**Not exploratory.** The rule is not a speed-up to be priced and dropped:
DL-S0 measures so that the build is ordered and its cost known, and no
measurement closes the order with a path left queueing. What a measurement
can change is which shape a path takes (DL-Q2), never whether it complies.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` where a wrong choice turns a
correct answer into a wrong one with no error.

Related: `docs/rules/rules.md` §3 (the Cabin controller's surface and the
Cabin store rows), §3a (the rule); `docs/inflight/known-gaps.md`
("Statistics that can queue a statement", "The `sys` views and the
statistics stores (BO)", the `SHOW CABIN_OPTIMIZER` entry under
"Multi-core state, continued"); `docs/spec/sys-views.md` §6;
`docs/spec/plan-stats.md`; `docs/spec/cabin-controller.md` (R2, PO4, PO8,
PO10); `docs/spec/wait-events.md`;
`instructions/v3.2.0/workorder-da-access-stats-memory-resident.md` (the
case that prompted the rule; DA-R3, DA-R4);
`instructions/v3.2.0/workorder-df-statistics-coverage.md` (DF-R2, DF-R4);
`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md` (the
per-core parts); `instructions/v3.2.0/workorder-ca-background-advisory-optimizer.md`
(CA-Q6, the fold moved off the statement, not built);
`instructions/v3.2.0/workorder-cw-optimistic-page-read.md` (the frame
table's latch, which is not a statistic and is CW's).

## 0. What DL is

`[operator]` *"It seems we have to set the principle that statistics must
not, in any case, line up a write or a read (query execution)."* and, after
CLA's reading of the tree and a five-clause draft, *"For now record it as
a rule; in the end strictly, and for now establish this rule through a
separate milestone. Write the work order of the milestone that will
establish this rule."* Then, the order written: *"Add the principle 'a
switched-off consumer collects nothing' to that milestone too."* (given in
Korean; recorded in English.)

`[measured]` What prompted it. `sys.access_stats` was a catalog relation
whose root, page 11, every successful `SELECT` held exclusive to count
itself. DA-S0 priced that hold at +8.19 us of server CPU a point `SELECT`
(+24 %) and -11.6 % of throughput at `cores = 8`, against +0.60 us at
`cores = 1` (`bench/v3.4.14/results-da-s0-premise-v3.4.14-30-g673f025d.md`);
DA's close, with the count moved to a per-core memory part, recovered it:
+13.5 % throughput in 6 of 6 paired lifetimes, a p99 of 513 us against
399.5 us, the counting itself not measurable
(`bench/v3.4.14/results-da-close-v3.4.14-32-g89c26252.md`). The same
optimization had been proposed and discarded twice on a `cores = 1` number
(BN-R6, CA-Q6; DA's §1.1). **The cost was the queue, not the count**, and a
one-core measurement could not see it.

`[design]` DL does three things:

1. **finds every place a statistic can make a statement wait** - DL-S0's
   census, of which §1.2 is the first reading and not the whole;
2. **removes each one** (DL-S1 to DL-S4), the strict form for the per-core
   stores included;
3. **adds what keeps the rule true** after DL closes (DL-S5): a contract
   suite and a debug guard, so that the next page 11 is an abort in a test
   and not a number found at a close.

**The rule as recorded** (`rules.md` §3a). The operator's sentence, read by
CLA as five clauses; the clauses are open to the operator's amendment
through §4's items, and the rule's text moves with a mark:

- (a) a record on a statement's path writes its own core's state and takes
  nothing a statement on another core can hold;
- (b) a record waits for no reader of the statistic; it is lost and counted
  instead (**strict - the operator's word**);
- (c) what a record does under anything it holds is bounded by a constant;
- (d) a statement that consults a statistic reads a published copy and
  waits for no writer of it;
- (e) a task that gathers a statistic off the statement's path takes
  nothing a plain read of the same data does not take, and is paced.

**The second rule** (`rules.md` §3a, its second entry; `[operator]`).
Where a setting turns off the only consumer of a statistic, a statement's
path records nothing for it; turning the consumer on starts it from no
evidence. CLA's draft carried it as a clause, the order as first written
took it out of the rule as a cost matter and left it an open item, and
the operator's word of §10 puts it in the milestone as a principle. It
is recorded beside the first rule, not inside it: the first is about a
wait, this one about work done for nobody. P1 breaks both (§1.2).

**What else CLA said in the conversation, and what the word did with it.**
CLA asked whether the per-core stores' wait for one reader's copy should be
allowed (DF-R2's and DA-R3's standing wording); the word is that the end
state is strict.

## 1. The tree as read

### 1.1 What is measured `[measured]`

| measurement | reading |
|---|---|
| DA-S0 and DA's close (above) | one exclusive hold every statement takes: -11.6 % at `cores = 8`, not visible at `cores = 1` |
| BO's close, `v3.3.0-23-g86d1fe1a`, `cores = 1` (`bench/v3.3.0/results-bo-close-v3.3.0-23-g86d1fe1a.md`) | `StatementStats::Record` and `RelationStats::Record` together cost 0.35-0.52 us a point statement, inside the largest same-binary gap |
| CA-S0, `v3.2.0-152-ga9f0f5cd`, `cores = 8` (`bench/v3.2.0/results-ca-s0-premise-v3.2.0-152-ga9f0f5cd.md`) | Waystone, access-statistics and optimizer-signal code together 0.2-0.6 % of sampled server CPU, a lower bound; the three keys off moved no headline outside noise. DA-S0 later found -11.6 % in a cell CA-S0 did not run, so "outside noise" there is not evidence for §1.2's paths |

**Not measured, and DL-S0 measures it:**

- the collector's latch (§1.2, P1) at any core count. Both arms of DA's
  `select8` cells took it on every statement, the control included, so its
  price is inside every number of that file and separated in none;
- the waits on `kOptimizer`, `kCabinStats` and `kStatsRing`: the contention
  instrument counts each (`include/kds/base/contention.hpp:51`, `:54`,
  `:56`), and no results file under `bench/` names any of the three;
- the tail a statistics reader puts on the statements it reads about:
  `known-gaps.md` (BO) says so in its own words - "unmeasured as a tail on
  the writing core".

### 1.2 The paths `[source-read]` at `1aaafe1b`

A first reading: `src/stats/`, `include/kds/stats/` and their call sites.
Statistics written outside them were not all opened; DL-S0 does.

**P1 - the Cabin controller's signal collector: one latch for the
instance, on every statement.** `OptimizerSignals` is "one collector for
the instance" (`include/kds/stats/optimizer_signals.hpp`, its header),
latched above one core (`src/server/expeditor.cpp:1342-1346`).
`RecordOptimizerSignals` calls `NoteExecution` at the successful end of
every `SELECT` that carries an identity
(`src/server/command_dispatcher.cpp:10681`, `:10719`), and `NoteExecution`
holds the latch for its body (`src/stats/optimizer_signals.cpp:68-81`).

- *It does not ask the switch.* `cabin_optimizer` is off by default
  (`cabin-controller.md`, the settings table); the wait sample alone is
  gated on it (`optimizer_signals.cpp:84`). The dispatcher's comment prices
  an unset collector at "one predicate per successful SELECT"
  (`include/kds/server/command_dispatcher.hpp:2184-2187`), and the server
  always sets one.
- *Its work under the latch is not bounded by a constant.* A new
  fingerprint past `kMaxTrackedFingerprints` (4,096,
  `optimizer_signals.hpp:61`) evicts the coldest by a scan of the whole
  map, a decay evaluation an entry (`optimizer_signals.cpp:23-41`,
  `:47-53`).
- *Its reader holds the same latch for a whole copy.* The controller's
  tick takes `Snapshot()` (`src/exec/cabin_optimizer_exec.cpp:44`,
  `optimizer_signals.cpp:116`) - an aggregation of every entry, sorted,
  under the latch.
- `rules.md` §3 declares the structure and its latch ("held for each
  call's body and innermost"). Declared, and a queue.

**P2 - the Cabin store's counters.** `NoteHit` and `NoteMiss` take the
cabin's partition latch to add one, then the instance's one `stats_latch_`
to add one, then forward to P1's latch (`src/stats/cabin_store.cpp:131-145`,
`:147-158`); `NoteHint` forwards a failure to P1 (`:160-162`,
`optimizer_signals.cpp:98-102`). Reached by a statement whose plan names a
Cabin (`cabin-controller.md` PO11). The partition latch is the store's own
and serves the entry reads; taking it a second time for a counter, and the
two instance latches, are the statistic's.

**P3 - the per-core parts: a record waits for a reader's copy.** One part a
core under a `kStatsRing` latch, the core its only writer, a reader on any
core taking each part's latch in turn:

| store | the record | the reader's hold |
|---|---|---|
| `StatementStats` | once a statement, a hash lookup by `(fetch_id, kind)` (`src/stats/statement_stats.cpp:58-67`) | `Snapshot()` **merges under the latch**: every shape of the part folded into a map that allocates, up to 4,096 a part (`:135-138` onward); `Dropped()`, a `sys.meta` field, takes each part's latch (`:180-183`) |
| `RelationStats`, `LeafStats` (`OidParts`) | `With` under the part's latch (`include/kds/stats/oid_parts.hpp:35-43`); a leaf division or compaction counts **under its page hold** (`include/kds/stats/leaf_stats.hpp`, "Where the counts live") | a sum by oid under each part's latch (`oid_parts.hpp:64`, `:74`, `:84`) |
| `AccessStats` (DA) | a flat-table probe under the latch (`src/stats/access_stats.cpp:47-50`) | `:86`, `:125`, `:160` |
| `LockWaitLog`, `CheckpointLog` (`PerCoreRing`) | a ring append (`include/kds/stats/per_core_ring.hpp:53-55`) | a copy (`:79`, `:91`, `:102`) |
| `AshRing` | the sampler's append (`include/kds/stats/ash_ring.hpp:38`) | `:55`, `:64`, `:68` |
| `PlanStats`' parts | `Fold` at a planned statement's end and `WantPriors` at a compile (`src/stats/plan_stats.cpp:332-335`, `:361-364`) | the steward's `Merge`: a copy of the part's entries, **allocated under the latch**, over the whole 1,024-slot table (`:382-400`) |

The latch is a `std::mutex` (`include/kds/base/latch.hpp:59-83`), so a
record that meets a reader sleeps its reactor thread, and every session on
that core, for the copy. The standing rules allow it: "waits for at most
one copy of its own part by a reader" (DF-R2, DA-R3). The specs say
otherwise in their own words - "a write never fails, delays or reorders
what it describes" (`docs/spec/sys-views.md` §6; the same sentence heads
five store headers) - and `known-gaps.md` (BO) records the delay.

**P4 - a statement's own read of a statistic.** A compile pins the
planner's published snapshot: one acquire load, and "the publish latch and
a reference copy only when the generation moved"
(`include/kds/stats/plan_stats.hpp:271-273`; `PlanStats::Current`,
`src/stats/plan_stats.cpp:593-596`). `publish_latch_` is one for the
instance, and an install holds it across the snapshot's construction
(`:498-513`), so every core's first compile after a publish can wait for
an install or for each other.

**P5 - a statistics report that stops a core.** `SHOW CABIN_OPTIMIZER`
takes the controller's view latch, which core 0's tick holds across a
Cabin's creation and its whole seeded build, and sleeps its reactor and
every session on it for the build (`known-gaps.md`, "A `SHOW
CABIN_OPTIMIZER` can stall its core for a whole Cabin build"; `rules.md`
§3, the controller's row). Whether the controller's view is a statistic is
DL-Q0.

**What already meets the rule**, and is the shape to copy:

- `RefusalStats`, `TxnStats`: "a fixed table per core, no latch and no
  allocation" (`include/kds/stats/refusal_stats.hpp:28`,
  `include/kds/stats/txn_stats.hpp:55`).
- `IndexStats`: the part's latch guards the part's *map* - a core's first
  sight of an index and a reader's walk - and "the counts themselves are
  not under the latch": relaxed single-writer words the core adds through
  with no latch, no hash and no allocation
  (`include/kds/stats/index_stats.hpp:45-57`).
- The wait statistics: relaxed single-writer words
  (`include/kds/sched/wait_state.hpp:61-73`, `:95-98`).
- The contention instrument: one cache-line block a core, written by its
  own thread (`contention.hpp`, "Per core, summed when read").
- The per-core load slots (`rules.md` §3, their row).

**Consumers with a switch, a first reading** (the second rule):

| statistic | its consumer and the switch | on a statement's path while off |
|---|---|---|
| the controller's signals S1-S3 (P1, and P2's forwards) | the Cabin controller; `cabin_optimizer`, off by default | **collected**: `NoteExecution`, `NoteCabinLookup` and `NoteCabinHint` ask no switch, and the collector being set is itself why every fingerprinted `SELECT` builds a `StatementIdentity` (`command_dispatcher.cpp:10261-10265`) and walks its chain for a candidate (`:10683-10718`) |
| the controller's S4, the wait sample | the same | not collected (`optimizer_signals.cpp:84`) - the shape the rule asks for |
| the access statistics | `SHOW ACCESS` and `CREATE CABIN`'s warning; `access_statistics` | not collected (`command_dispatcher.cpp:10507`) |
| ASH | `sys.ash`; `kds.ash.enabled` | not sampled (`src/sched/ash_sampler.cpp:102`) |
| the planner's observations and wanted priors | a `planned` search, a `fixed` capture's first compile, `ADVISE PLAN`, the `sys` views; `join_order` | collected under every value (`command_dispatcher.cpp:10454-10496`); whether any value turns the consumer off is DL-Q12 |

A store a `sys` view reports and nothing switches - `StatementStats`,
`RelationStats`, `IndexStats`, `LeafStats`, the wait statistics - has "no
switch" by BO-Q8 (`sys-views.md` §6) and is consumed by whoever reads the
view; the rule adds no switch (DL-Q11).

**Wait-free words every core writes.** Not a wait, and listed because the
census has to decide what they are (DL-Q4): `CapturedPlans::CountServed`
and its four siblings (`include/kds/exec/captured_plans.hpp:177-181`),
`AdminPlans::CountServed` and `CountDeclined`
(`include/kds/exec/admin_plans.hpp:269-270`), `PlanStats::part_drops_`
(`plan_stats.cpp:358`).

**The tools the tree already has.** A try-form guard that never waits
(`LatchGuard(Latch*, std::try_to_lock_t)`, `latch.hpp:120-128`, CZ-S3); a
per-kind wait count and wait time for every `Latch`
(`Latch::LockReportingWait`, `latch.hpp:68-76`); `sched::RelaxedU64`.

### 1.3 The census (DL-S0) `[source-read]` at `b0a824db`

On worktree `dl-statistics-queue-no-statement` at `b0a824db`. §1.2 stands
as the first reading; this is the list. `CD` is
`src/server/command_dispatcher.cpp`. Classes: **C0** own-core words, **C1**
own-core part under a latch, **C2** a latch or hold another core's
statement takes, **C3** a wait-free word every core writes, **C4** taken
under a page hold or a lock.

**Where everything runs.** Every statement, and so every record a
statement makes, runs on its session's core's reactor thread
(`CommandDispatcher::DispatchAsync`, `CD:924`); no record site runs on the
WAL writer, the data syncer, the page readers, the archiver or the backup
copier. **The steward, the controller's tick and the captured-plan
persisting step are not tasks: they are `Scheduler::SubmitEvery` timer
callbacks run inline in core 0's reactor loop** (`src/sched/scheduler.cpp:127-170`),
and the ASH sampler is a phase of every core's loop (`:441-448`) - so a
latch one of them waits for stops every statement on that core (DL-Q17).
A `Latch` is a `std::mutex` (`include/kds/base/latch.hpp:59-83`).

**Records on a statement's path.**

| store | the record and its sites | class today | what it can wait for | shape (DL-Q2) | stage |
|---|---|---|---|---|---|
| the controller's collector (P1) | `NoteExecution`, every successful fingerprinted `SELECT` (`CD:10719`); `NoteCabinLookup` / `NoteCabinHint` from a Cabin probe (`src/stats/cabin_store.cpp:144`, `:157`, `:161`); `NoteWaitSample` from each core's sampler (`src/sched/ash_sampler.cpp:151`) | **C2**, one latch for the instance, and unbounded under it (the eviction scan) | every other core's note; the tick's `Snapshot`; a `SHOW CABIN_OPTIMIZER`'s `QualityOf` | second rule, then W4 | S1 |
| the Cabin store's totals (P2) | **nine sites take `stats_latch_`**, not two: `NoteHit`, `NoteMiss`, `NoteCapRefusal`, `NoteUnbankableView`, `NoteEntryCapRefusal`, `Observe` (a sighting clear), `Commit`, `Unobserve` and **`NoteWrite`** (`cabin_store.cpp:131-195`, `:235`, `:353`, `:430`, `:561`) | **C2**; **`NoteWrite` is also C4**: an `INSERT` or `UPDATE` of a relation with an observed value takes the instance's latch holding its leaf exclusive (`CD:8514`, `:12218`, `:10537`) | every other core's count; `sys.meta`'s copy | W1: a block of words a core | S1 |
| a Cabin's `hits` and `misses` (P2) | `NoteHit`, `NoteMiss`: the cabin's partition latch taken a second time for one add | **C2** (a partition is the instance's, 16 by `cabin_id`) | any core's probe, write or build of a cabin in the partition | W1: `CounterTable` a core, by `cabin_id` (never reissued: `AllocateRowId`) | S1 |
| a Cabin's `recordings`, `appends`, `values`, `entries` | under the partition latch the set's own change already holds (`Commit`, `NoteWrite`, `AddSetLocked`) | within the rule: no latch is taken for the count, and `values` is read by `MayObserve` | - | unchanged | - |
| `StatementStats` | once a statement, at its end (`CD:1475`) | C1 | a reader's merge of the part (allocating) | W2, W3 | S2 |
| `RelationStats` | a write statement's end (`CD:6758`, `:15002`); a lock wait's end (`src/stats/lock_wait_log.cpp:24`) | C1 | a reader's merge | W1 (`CounterTable`) | S2 |
| `LeafStats` | a leaf division or compaction (`src/storage/btree/btree.cpp:1030`, `:1075`; `src/storage/index/index_tree.cpp:446`) | C1 and **C4**: under the leaf's exclusive hold and every secured parent's | a reader's merge - with pages held exclusive | W1 (`CounterTable`) | S2 |
| `IndexStats` | the counts are W1 already; a core's first sight of an index takes the part's latch (`CD:10443`) | C1 at the first sight | a reader's merge | W1 (`CounterTable`) | S2 |
| `AccessStats` | a successful `SELECT`'s steps (`CD:10508`); a foreign key check (`CD:5310`) | C1 | a reader's walk of the whole table; **`Forget`, a column change on any core, takes every part's latch in turn** (`CD:4702`) - a C2 path of its own | W1: slots claimed and marked by compare-exchange (DL-Q15) | S2 |
| `LockWaitLog` | a lock wait's end, or its deadlock refusal (`CD:480`, `:521`, `:837`, `:878`) | C1 | a reader's copy of the ring (allocating) | W2, W3 | S2 |
| `CheckpointLog` | a checkpoint run's end, on its core's reactor (`src/wal/checkpointer.cpp:867`) | C1 | a reader's copy | W2, W3 | S2 |
| `AshRing` | the sampler's append, once a task a sample (`ash_sampler.cpp:169`) | C1 | a reader's decode of the whole ring under the latch (65,536 records by default) | W2, W3 | S2 |
| `PlanStats`' parts | `WantPriors` and `Fold` at a planned statement's end (`CD:10469`, `:10496`) | C1 | the steward's copy (allocating) | W2, W3 | S2 |
| the session registry (P6, new) | **a session's creation and its end take one process-wide `std::mutex`**, the end with a linear erase (`src/sched/wait_state.cpp:27-48`) | **C2** | any core's connect or close; a `sys.session_waits` or `sys.session_state` reader's copy of every session (`:52-58`) | W1: slots, no latch (DL-Q14) | S2 |
| `RefusalStats`, `TxnStats`, the wait totals, the contention block, the core load slots, a statement's `ExecStats`, the WAL manager's counts, the scheduler's | per-core or per-run words | C0 | nothing | unchanged | - |

**A statement's own read of a statistic.**

| read | sites | what it can wait for | shape | stage |
|---|---|---|---|---|
| the planner's snapshot pin (P4) | `PlanStatsSource::Pin` at every planned compile (`src/exec/step_compiler.cpp:2845`) and at a planned statement's end (`CD:10460`): `PlanStats::Current` under `publish_latch_` once a generation | an install: the snapshot is built **under** the latch - three sorts over up to 16,384 posteriors and the managed store's latch nested (`src/stats/plan_stats.cpp:498-513`) | W5: built outside, swapped under a hold of one pointer; a pin that finds it busy keeps the snapshot it has | S3 |

**Readers of a statistic** (each a client statement on its own core's
reactor, through `ReadSysView`, `src/server/sys_views.cpp:256`; DL-Q7 (a)
puts their own waits outside the rule, and W3 shortens what they cost).

| reader | what it holds, and for what |
|---|---|
| `sys.statement_stats`, `sys.statement_histogram` | each part's latch across a merge into a map that allocates, strings copied (`src/stats/statement_stats.cpp:135-178`) |
| `sys.relation_stats`, `sys.indexes` | each `OidParts` part's latch across an allocating merge (`include/kds/stats/oid_parts.hpp:61-68`); `PlanStats`' `space_latch_` a relation |
| `sys.access_stats`, and `CREATE CABIN`'s warning (`src/exec/cabin_ddl.cpp:72`) | each part's latch across a walk of every slot, allocating (`src/stats/access_stats.cpp:122-155`) |
| `sys.lock_waits`, `sys.checkpoints` | each part's latch across a copy with no reserve (`include/kds/stats/per_core_ring.hpp:76-86`) |
| `sys.ash` | a core's ring latch across the decode of the whole ring (`include/kds/stats/ash_ring.hpp:53-61`) |
| `sys.session_waits`, `sys.session_state` | the process-wide session mutex across a copy of every session |
| **`sys.meta`** | every `Dropped()` and `Read()` takes each part's latch in turn (`CD:2661-2694`); the Cabin store's `stats_latch_` twice (`CD:2230`); **and the controller's view latch** (`CD:2148`) - so a `SHOW META` on any core, not only a `SHOW CABIN_OPTIMIZER`, sleeps its reactor for a Cabin build (P5, wider than §1.2 read it) |
| `sys.cabin_optimizer` (P5) | the view latch across the whole view, `QualityOf` nesting the collector's latch an entry (`sys_views.cpp:1174-1234`) |
| `sys.wait_stats`, `sys.wait_histogram`, `sys.refusal_stats`, `sys.txn_histogram`, the contention block | nothing: relaxed loads |

**Collection tasks** (clause (e), DL-Q6 (a), DL-Q17).

| task | what it takes | paced | finding |
|---|---|---|---|
| the planner's steward (`PlanStats::StewardStep`, `plan_stats.cpp:548`; core 0, 1 s) | each part's latch for a copy; pages one at a time under a shared latch, the anchor let go before the root; `space_latch_`; `publish_latch_` across the snapshot's construction; no lock unit | 16 pages a step | a plain read of the data, paced: meets (e). Its three latches are waited for on core 0's reactor: DL-Q17, S2 and S3 |
| `GATHER STATISTICS` (`CD:3528-3599`) | a slice's `IS` by a non-queueing ask, 16 leaves a slice one at a time, a 2 ms floor a slice; the borrow and the page let go before each park | yes | meets (e). A dispatcher that cannot park runs it whole, unpaced (`BuildRun::HandOn`, `CD:3091`): the synchronous `Dispatch`, which nothing in `src/` calls |
| the ASH sampler | its own ring's latch; the collector's latch a waiting sample while the switch is on | 1 s by default | its two latches: S1 and S2 |
| the controller's tick (`src/exec/cabin_optimizer_exec.cpp:39-96`; core 0, 10 s) | the view latch, taken **before the switch is asked**; the collector's latch for `Snapshot`; then what `Apply` does | 10 s between ticks, nothing inside one | the statistic's part - the snapshot - is S1's. **`Apply` is not a statistic's**: a created or extended Cabin's seeded build walks the whole relation inside one callback with no yield, core 0's reactor stopped for the walk, and a create holds `sys.cabins`' root exclusive across a scan and a log append. An entry set is the Cabin's feature (DL-Q0 (a)), so this is `cabin-controller.md`'s gap, recorded in `known-gaps.md` and not closed here |

**Wait-free words every core writes** (C3, DL-Q4 (a): within the rule,
listed, the ones a statement writes every time priced - §7's DL-S0 entry).

| word | written | read by |
|---|---|---|
| the buffer pool's `hits_` (`src/storage/device_page_store.cpp:1084`) | every page fetch that hits, above one core under the frame table's latch | `sys.meta` `pool` |
| the pool's `misses_`, `would_wait_`, `fetch_images_logged_` and its batch counts | a miss, a refused read, an image, a fill at the limit | the same |
| `CapturedPlans`' `served_`, `deferred_`, `unkeyed_`, `over_ceiling_`, `declined_` (`src/exec/step_compiler.cpp:2811-3000`) | every compile of a join block under `fixed` - no chain is cached, so every such statement | `sys.meta` `join_order` |
| `AdminPlans`' `served_`, `declined_` (`:2699`, `:2725`) | a compile whose shape has an enabled plan | the same |
| `PlanStats::part_drops_`, `WakerTable`'s kicks, `PageReader::submitted_`, the logger's line counts, the reclaim counts | a full part, a cross-core wake, a handed-off read, a log line, a drop | `sys.meta` |
| the contention block's slot 0 | core 0's reactor and every thread that declared no core (the WAL writer, the data syncer), by read-modify-write | `sys.meta` `contention` |

**Outside the rule by DL-Q0 (a), and listed so nobody reads them as
missed**: the captured plans' and the administrator's plans' partition
latches, which every keyed compile takes for a lookup (`step_compiler.cpp:2698`,
`:2831`) - plan tables a statement reads through, `rules.md` §3's rows,
carrying the `kStatsRing` kind by an accident of naming (DL-Q18); a Cabin
partition's latch for an entry read or append; a Waystone trail; the frame
table's latch (CW's).

**Consumers with a switch** (the second rule; §1.2's table confirmed, with
what a statement still does while each is off).

| statistic | switch, default | on a statement's path while off | finding |
|---|---|---|---|
| the collector's S1-S3 | `cabin_optimizer`, off | all of it: the identity (`CD:10261-10265`), the candidate walk and the note (`CD:10681-10719`), the Cabin store's forwards. The collector is always built and handed to every dispatcher and to the store (`src/server/expeditor.cpp:1342-1370`), and the switch is read in three places only - the tick, the wait sample's gate, `sys.meta` | **breaks the rule**: S1 |
| the collector's S4 | the same | nothing (`src/stats/optimizer_signals.cpp:84`) | meets it |
| the access statistics | `access_statistics`, on | nothing (`CD:5309`, `:10507`) | meets it |
| ASH | `ash_enabled`, on | no sample (`ash_sampler.cpp:101-104`). A statement still publishes its fingerprint and pattern id and its wait state to its session's words (`CD:946-947`, `:983-984`) - read by `sys.session_state` and `sys.session_waits`, which no switch turns off | meets it: ASH is not their only consumer |
| the planner's observations and wanted priors | `join_order` | under `written`: nothing - `planned_relations` is filled only where a block is searched or run under `fixed` (`step_compiler.cpp:2729-2738`, `:2764-2776`; `CD:10456`) | meets it (DL-Q12 as revised) |
| a Waystone trail | `waystone_recording`, `waystone_replay` | not a statistic (DL-Q0 (a)) | - |

A store a `sys` view reports and no setting switches - `StatementStats`,
`RefusalStats`, `RelationStats`, `TxnStats`, `LockWaitLog`, `IndexStats`,
`LeafStats`, the wait statistics, every word above - is consumed by
whoever reads the view (DL-Q11 (a)).

**Spec and header sentences the change makes false or true** (DL-R9; each
is amended in the stage that changes its store): "waits for at most one
reader's copy of its own part" (`include/kds/stats/access_stats.hpp`, DA-R3;
DF-R2 in `sys-views.md` §6 and the store headers); "a reader on any core
takes each part's latch in turn" (six store headers); "A forget deletes by
backward shift, so no tombstone counts against the bound"
(`access_stats.hpp`); "One collector for the instance ... latched above one
core" (`optimizer_signals.hpp`, `rules.md` §3's controller row,
`cabin-controller.md`); "a separate stats latch for the instance's
counters" (`rules.md` §3's Cabin store row); "one predicate per successful
SELECT" for an unset collector (`command_dispatcher.hpp:2184-2187`); "the
publish latch and a reference copy only when the generation moved"
(`plan_stats.hpp`). True once DL lands, and false today: "a write never
fails, delays or reorders what it describes" (`sys-views.md` §6, five
store headers).

## 2. The shapes `[design]`

None is chosen here but where a mark says so; DL-S0 orders them.

- **W1 - single-writer words read with no latch** (`IndexStats`' and
  `RefusalStats`' shape). A row of fixed counters is `RelaxedU64` words its
  core alone writes; a reader reads them as they stand, so a row's columns
  may differ by a run in flight. No count is lost and nothing waits. The
  part's latch stays for the part's *structure* only - a first sight, a
  reader's walk of the map - and W2 covers that.
- **W2 - try, or drop and count.** Where a record must change structure (a
  shape's, relation's or index's first sight; a ring's append; a table's
  slot), it takes its part's latch by the try form; a failed try drops the
  record and adds one to the part's drop count, itself a W1 word. Nothing
  waits; a reader's copy costs the records that met it.
- **W3 - the reader copies and lets go.** A reader holds a part's latch for
  a bounded copy into memory it reserved before the hold, and merges,
  sorts and allocates after it. It shortens what W2 drops; it is owed
  whatever W1 and W2 do.
- **W4 - per-core parts folded by the consumer's own task** (`PlanStats`'
  shape, for P1). Each core notes into its own fixed table; core 0's tick
  takes each part by W3 and folds into the controller's state, which the
  tick alone touches. Eviction and decay run in the fold, never on a
  statement. `Snapshot()` reads the folded state under nothing a statement
  takes, and stays one deterministic by-value aggregation (PO10): parts
  folded in core order, entries by id.
- **W5 - a published immutable copy read with no latch** (for P4 and P5).
  The writer builds the copy outside any latch and publishes one pointer;
  a statement loads it. For P5 the tick publishes the view at its end and
  `SHOW CABIN_OPTIMIZER` reads the copy - the fix `known-gaps.md` already
  names.

## 3. Rules

Binding from DL-S1 on. `rules.md` §3a is the authority for the rule itself;
these are the order's.

- **DL-R1 - no answer moves.** Nothing DL changes alters a statement's
  rows, refusals or order. A statistic the engine reads - the planner's
  observations and priors, the controller's signals - stays advisory to a
  choice and never to an answer (`plan-stats.md`; CY-R1; `cabin.md` C1).
- **DL-R2 - a lost record is counted where a reader can see it.** Every
  drop a failed try causes adds to a `sys.meta` field of its store
  (`sys-views.md` SV3), beside the bound drops that exist today and apart
  from them, so a reader can tell "the part was full" from "a reader was
  copying".
- **DL-R3 - a view keeps its columns, types, keys and refusals.** What a
  row may now show is columns that differ by a run in flight (W1); where a
  spec promises more than that for a view, DL-S0 lists it and the item
  goes to the operator before the store changes (DL-Q5).
- **DL-R4 - no allocation is added to a statement's path**, and none stays
  under a latch a record takes but a stated first sight.
- **DL-R5 - the controller's decisions stay deterministic** (PO10): the
  same notes in the same per-core order fold to the same snapshot.
- **DL-R6 - no format moves**: no page byte, no record kind, no superblock
  bump. Every structure DL touches is memory.
- **DL-R7 - nothing is made slower to be made compliant.** The close's bar
  is no cell slower outside its own spread at `cores = 1` and at the
  largest clean cell (DL-Q9).
- **DL-R8 - the armed rigs carry the proof.** The simulator runs unarmed
  and single-threaded and shows nothing here; the two-core rigs and the
  `tsan` build do. A W1 read beside a writer is an atomic relaxed load, so
  it is no data race by the language's rule.
- **DL-R9 - the standing wording goes when the stores do.** DF-R2's and
  DA-R3's "waits for at most one copy" is struck from the specs and store
  headers that repeat it in the stage that changes each store, and
  `known-gaps.md`'s entries are struck as each path lands, never before.
- **DL-R10 - a switch is read, never waited for.** What a statement pays
  for a consumer that is off is one relaxed load of the switch; no
  identity, candidate, sample or note is built on its behalf. A switch
  flipped while statements run is safe at any instant: a note taken a
  moment after an off, or missed a moment after an on, is a count's
  difference and nothing else.
- **DL-R11 - a switch-on starts from no evidence.** Nothing is
  back-filled or estimated for the time the consumer was off (PO8; the
  collector's own "eviction restarts counting, it never fabricates it").
  What the switch-off does with evidence already collected is DL-Q13.

## 4. Items for the operator (DL-Q)

(a) is CLA's proposal in each. **DL-Q1 and DL-Q3 are the operator's own marks; every
other mark is CLA's proposal adopted on the operator's standing go-ahead of 2026-11-20
(go-ahead-achieving-milestone, `raft-marks-2026-11-20.md` §11), never the operator's
per-item mark.** None is tagged `[quiet-wrong]`. The options not taken stay open to the
operator's own mark.

| | item | (a) | other | mark |
|---|---|---|---|---|
| DL-Q0 | What is a statistic, for the rule? | every count, sum, sample or timing the engine keeps about what ran, whose loss changes no answer: the `sys` statistics stores, the wait statistics and ASH, the planner's observations, priors and managed objects, the controller's signals and the Cabin store's counters, the contention tallies, the plan tables' counters; **and the controller's view (P5)**. Not: a Waystone trail or a Cabin's entry set - structures a statement reads through, whose writes are their feature - nor the lock table, the frame table or the log | (b) Waystone trail recording is in; (c) P5 is out and stays `cabin-controller.md`'s gap | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone) |
| DL-Q1 | The per-core parts: may a record wait for one reader's copy? | **no - marked, the operator's word**: the end state is strict (`raft-marks-2026-11-20.md` §9) | | **(a)**, the operator's own mark (`raft-marks-2026-11-20.md` §9) |
| DL-Q2 | The strict shape. | W1 wherever a row is fixed counters, W2 for structure, W3 for every reader; DL-S0 names each store's | (b) W2 everywhere, the simplest, losing counts while a reader copies; (c) a sequence-lock a store, no loss, a reader that retries | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone); each store's shape is named in §1.3 |
| DL-Q3 | A statistic whose only consumer is switched off. | **it is not collected - marked, the operator's word** (`raft-marks-2026-11-20.md` §10), recorded as `rules.md` §3a's second rule; the collector while `cabin_optimizer` is off is its first case | | **(a)**, the operator's own mark (`raft-marks-2026-11-20.md` §10) |
| DL-Q4 | A wait-free word every core writes (§1.2's list). | within the rule - nothing waits; the census lists them, DL-S0 prices the ones a statement writes every time, and one priced above the cell's floor moves to a per-core word | (b) outside the rule: per-core words only, all moved; (c) left alone, unlisted | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone); priced in DL-S0 (§7) |
| DL-Q5 | A view's row whose columns differ by a run in flight. | accepted for every store, as `IndexStats` states it today | (b) a row is read whole (then DL-Q2 (c) for that store) | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone) |
| DL-Q6 | Collection tasks (the steward's sample, `GATHER STATISTICS`, the ASH sampler, the controller's tick) under clause (e). | a task reads data as a plain read does - one page at a time under its shared latch, a slice's `IS` for a gather - and is paced; a writer may meet it as it meets any reader | (b) a task also gives way: it tries a page and skips or retries on a busy one, and a gather's slice ends when a stronger lock waits | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone) |
| DL-Q7 | A statistics reader's own waits (one view behind another, a view behind a tick's W3 copy). | outside the rule: the statement reading the report is not the statement reported on, and its wait is one bounded copy | (b) inside: readers try too, and report busy | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone) |
| DL-Q8 | What checks the rule (DL-S5). | both: a contract suite that, a store at a time, holds every latch the store owns from another thread and runs each record entry point under a deadline; and a debug-build scope around every record in which a blocking `Latch` acquisition or a page fetch aborts naming the caller | (b) the suite only; (c) the guard only | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone) |
| DL-Q9 | The close's bar. | the suite and the guard green; no cell slower outside its spread (DL-R7); under a reader storm (M3) the described statements' p99 and p99.9 inside the no-reader arm's spread; the drop rate reported, no bar on it | (b) a bar on the drop rate too; (c) the operator's numbers | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone) |
| DL-Q11 | Does the second rule add switches? | no: it binds where a setting already turns a consumer off. A store a `sys` view reports keeps BO-Q8's "no switch" and counts as consumed | (b) every store gets a switch, off meaning not recorded; (c) only the stores DL-S0 prices above the cell's floor get one | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone) |
| DL-Q12 | The planner's observations and wanted priors under `join_order`. | collected under every value: `ADVISE PLAN`, a capture's first compile and the views read them under `fixed` and `written`, so no value turns the consumer off | (b) not folded while `join_order` is `written`; (c) not folded under `fixed` either once a shape is captured | **(a) as revised at DL-S0**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone): the tree gathers under `planned` and `fixed` and nothing under `written` (CX-Q2, `src/exec/step_compiler.cpp:2729-2738`), not "under every value" as (a) was written; DL changes none of it - `written` already records nothing for the planner, and under `fixed` a capture's first compile, `ADVISE PLAN`, a `DROP PLAN`'s re-arm and the views still read what is gathered |
| DL-Q13 | Evidence already collected when a consumer is switched off. | kept, decaying by the clock, with no new note; a switch-on then reads what is left of it | (b) cleared at the switch-off, so "from no evidence" is exact | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone) |
| DL-Q10 | The measurement budget. | the standing one: 20 minutes a stage, about an hour at the close, cuts named in the results file | (b) lifted for DL-S0 | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone) |
| DL-Q14 | The session registry of the wait statistics: one process-wide mutex a session's creation and its end take, and a `sys.session_waits` or `sys.session_state` reader holds for a copy of every session (`src/sched/wait_state.cpp:27-58`; found by DL-S0). | the registry takes no latch: a session's statistics live in a slot of chunks that are never moved or freed, claimed and given back by compare-exchange, and a reader walks the slots and reads each as its words stand, leaving out one that changed hands under the read | (b) a registry a core under a latch, the reader's hold cut to a copy of pointers - a creation still waits for it; (c) outside the rule: a connection's setup is not a statement | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone); raised by DL-S0's census |
| DL-Q15 | `AccessStats::Forget` - a column change's removal of the shapes naming a column, from every core's part (`src/server/command_dispatcher.cpp:4702`) - once a record takes no latch (found by DL-S0). | it waits for nothing: a shape's slot is marked dead where it stands and its own key takes it again from zero; a dead slot keeps its place and counts toward its part's bound until a restart (DA's "no tombstone counts against the bound" goes) | (b) the parts keep a latch and the column change waits a bounded copy for each (breaks clause (a)); (c) the forget is queued to each owning core and a reader filters until it is applied | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone); raised by DL-S0's census |
| DL-Q16 | A statement whose subject is the statistic - the operator's `STATISTICS` statements (a `GATHER`'s store and republish, `PUBLISH`, `RESTORE`, ...), `ADVISE PLAN`'s row, a relation drop's `Forget` of a managed object (found by DL-S0). | outside the rule, as DL-Q7's readers are: it is not a statement the statistic reports on, and what it may wait for is one bounded install or copy by another such statement - never one a statement's record or a compile holds, since those take none of these latches | (b) inside: it tries, and a busy latch is refused `TxnConflict` | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone); raised by DL-S0's census |
| DL-Q17 | A statistics task that runs as a timer callback on a reactor (the steward, the controller's tick, the ASH sampler): a latch it waits for stops every statement on its core (found by DL-S0; the callbacks are `Scheduler::SubmitEvery`'s, run inline). | every latch such a callback takes that another thread can hold is taken by the try form; a busy one is left to the next period (a part not merged this second, a publish left dirty, a sample not taken) and counted where a count exists | (b) the callbacks move to a thread of their own; (c) they may wait a bounded copy | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone); raised by DL-S0's census |
| DL-Q18 | The latch kinds the contention block counts by: the plan tables (captured plans, administrator's plans, plan advice) carry `kStatsRing` and are not statistics (DL-Q0 (a)); `kCabinStats` names a latch DL-S1 removes (found by DL-S0). | the plan tables take a kind of their own (`plan_table`), `cabin_stats` goes with its latch, and `stats_ring` counts the statistics stores alone - so the wait the rule forbids has one name in `sys.meta` and DL-S5's guard has one kind to refuse | (b) left as they are, the guard listing latches by address | **(a)**, adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone); raised by DL-S0's census |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DL-S0 - census and price** | No engine code on `main`; a measurement-only patch stays on its branch. **Census**: every statistics write on a statement's path, classed C0 own-core words / C1 own-core part under a latch / C2 a latch or hold another core's statement takes / C3 a wait-free word every core writes / C4 taken under a page hold or a lock; every reader of each store with what it holds, for how long and from which task; every statement-path read of a statistic (P4's class); every collection task with what it takes; **every statistic's consumers and the setting, if any, that turns each off, with what a statement still does for it while off**; every spec and header sentence the change makes false or true. **Measurement** M1-M4 below, `build-release`. | the list §1.2 is a first reading of; each store's shape named (DL-Q2); the order of DL-S1..S4 by price; items back to the operator |
| **DL-S1 - the collector (P1) and the Cabin store's counters (P2)** | **The second rule first**: with `cabin_optimizer` off no statement builds an identity or a candidate for the collector, notes an execution or forwards a Cabin lookup or hint (DL-R10, DL-R11), and whatever else DL-S0's list finds collected for a consumer that is off is brought in here. Then W4 for the signals while it is on; the store's counters as W1 words a core; `rules.md` §3's two rows and `cabin-controller.md` amended. | a branch at the push gate |
| **DL-S2 - the per-core parts (P3)** | The strict form, a store at a time in DL-S0's order, `OidParts` and `PerCoreRing` once for the stores that share them; W3 for every reader; DL-R2's drop fields; `sys-views.md` §6, `plan-stats.md` and each store's header amended (DL-R9). | a branch at the push gate |
| **DL-S3 - a statement's read (P4) and the report (P5)** | W5 for the planner's snapshot pin and, if DL-Q0 keeps it, the controller's view. | a branch at the push gate |
| **DL-S4 - the collection tasks** | Clause (e) against the steward, the gather, the sampler and the tick as DL-S0 found them; built only where one takes more than a plain read, or where DL-Q6 is marked (b). | a branch at the push gate, or "nothing to change" with the census as its record |
| **DL-S5 - the check** | DL-Q8: the contract suite, extended with every store DL touched; the debug scope; **for the second rule, a test per switch that runs the statement shapes with the consumer off and finds its store untouched and its entry points uncalled**; `btree`- and page-level rigs untouched. `rules.md` §3a loses its "does not yet meet them" paragraph and `known-gaps.md` its section. | a branch at the push gate |
| **DL-S6 - the close** | Interleaved A/B over the whole change, the commit DL opened from against the closing one: DA's four cells, a plan-named Cabin probe cell, M3's reader storm; the drop rates; what was cut for the budget. | closed, or closed with what is left named |

Each of DL-S1 to DL-S5 carries its `critics-developer` review and the full
suite, the armed rigs and the `tsan` cell beside it; overhead is measured
at the close (the Session Workflow's step 3).

**DL-S0's cells.**

- **M1** - the waits as counted: `kOptimizer`, `kCabinStats` and
  `kStatsRing` wait counts and wait time a statement, from the contention
  block, at `cores` 1 and 8 on DA's `select8` shape and on `insert1`.
  `kOptimizer` is two latches (`contention.hpp:51`); S0 reads which.
- **M2** - P1's price: the same binary with the dispatcher's collector
  unset against set (a measurement-only patch), interleaved, `select1` and
  `select8`. This is the DA-S0 measurement for the latch DA left, and the
  unset arm is what the second rule makes of the default configuration,
  so it is also that rule's expected gain.
- **M3** - the reader's tail: 16 clients on `select8` beside one session
  that reads `sys.statement_stats`, `sys.relation_stats` and `sys.meta` in
  a loop, against the same load with no reader; the described statements'
  p50, p99, p99.9 and `kStatsRing` waits. Then the same beside a
  controller tick with `cabin_optimizer` on.
- **M4** - P2's price: a statement whose plan names a Cabin, at `cores` 8,
  the counters as built against compiled out.

## 6. What DL does not do

- No statistic is removed or renamed and no view loses a column; no
  switch is added (DL-Q11 (a)), and none changes its meaning or default.
- No change to the frame table's latch, a page latch, the lock family or
  the log: none is a statistic. The frame table's latch is CW's.
- No Waystone trail or Cabin entry-set change (DL-Q0 (a)).
- No new statistic, no history, no persistence of one.
- No change to what the planner or the controller decides from a
  statistic, only to how the statistic reaches them.
- No format event (DL-R6).

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-20

Written in the main checkout on `main` at `1aaafe1b`
(`v3.4.16-1-g1aaafe1b`), with the rule's entry in `docs/rules/rules.md`
§3a and the list of what does not meet it in
`docs/inflight/known-gaps.md`; documents only. The letter DL is CLA's
proposal; the rule, its strict end state and the milestone are the
operator's word. No review was run and no suite was executed: the commit
changes no build input.

### Amended 2026-11-20: the second rule

On the operator's word of the same day (`raft-marks-2026-11-20.md` §10),
before the order was committed: "a switched-off consumer collects
nothing" is recorded as `rules.md` §3a's second rule and is established
by this order. DL-Q3 is marked; DL-R10, DL-R11, DL-Q11..Q13, §1.2's
table of consumers, DL-S0's census line, DL-S1's first half and DL-S5's
test are added. Documents only.

### Opened 2026-11-20, and DL-S0: the census

Opened on the operator's standing go-ahead (go-ahead-achieving-milestone,
`raft-marks-2026-11-20.md` §11) on worktree
`dl-statistics-queue-no-statement` from `b0a824db` (`v3.4.16-2-gb0a824db`).
The marks are §4's column: DL-Q1 and DL-Q3 the operator's own, every other
adopted as CLA proposed it on that standing word.

**The census is §1.3**, read at `b0a824db` by three sweeps of `src/` and
`include/` - every record entry point's call sites, every reader and
collection task, every counter outside `src/stats/` and every switch -
each finding CLA cites re-read in the tree before it was written down.
What it found beyond §1.2's first reading:

- **the Cabin store counts under `stats_latch_` at nine sites, not two**,
  and one of them, a write's append to an observed set, takes it holding
  the leaf exclusive;
- **`AccessStats::Forget` is a cross-core path of its own** (a column
  change takes every part's latch in turn): DL-Q15;
- **the session registry of the wait statistics** - one process-wide mutex
  at a session's first statement and at its end, held by a reader for a
  copy of every session: DL-Q14;
- **`sys.meta` takes the controller's view latch**, so `SHOW META`, not
  only `SHOW CABIN_OPTIMIZER`, sleeps its core for a Cabin build;
- **the steward, the tick and the persisting step are timer callbacks on
  core 0's reactor, and the sampler a phase of every reactor's loop**, so a
  latch one of them waits for stops its core's statements: DL-Q17;
- **a statement publishes its pattern id for the controller's wait signal
  alone**, whatever the switch says - a second piece of work done for a
  consumer that is off;
- **the plan tables' partition latches carry the statistics' latch kind**
  and are not statistics: DL-Q18;
- **DL-Q12's proposal did not match the tree**: nothing is gathered for the
  planner under `written` already (CX-Q2). Revised and adopted as revised.

Not DL's and recorded in `known-gaps.md`: the controller's seeded build
stops core 0 for a whole-relation walk - an entry set's build, not a
statistic's (DL-Q0 (a)).

**Each store's shape is named in §1.3's table** (DL-Q2 (a)): W1 for every
row of fixed counters - the Cabin store's counts, `RelationStats`,
`LeafStats`, `IndexStats`, `AccessStats` and the session registry, the
first four over one latch-free table (`stats/counter_table.hpp`); W2 and
W3 for the stores whose row or structure is more than counters -
`StatementStats`, the two rings, the ASH ring and the planner's parts; W4
for the collector; W5 for the planner's snapshot and the controller's
view.

No engine code is in this stage. No suite was executed for it: it changes
no build input. The premise measurement is the next entry.
