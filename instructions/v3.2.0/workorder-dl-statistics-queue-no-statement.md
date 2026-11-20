# Work order DL - statistics queue no statement, and a switched-off consumer collects nothing: the two rules established over every recording path, every statistics read a statement makes and every collection task, and checked

Status: **WRITTEN 2026-11-20, not opened.** Written in the main checkout on
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

(a) is CLA's proposal in each. Only DL-Q1 is marked.

| | item | (a) | other |
|---|---|---|---|
| DL-Q0 | What is a statistic, for the rule? | every count, sum, sample or timing the engine keeps about what ran, whose loss changes no answer: the `sys` statistics stores, the wait statistics and ASH, the planner's observations, priors and managed objects, the controller's signals and the Cabin store's counters, the contention tallies, the plan tables' counters; **and the controller's view (P5)**. Not: a Waystone trail or a Cabin's entry set - structures a statement reads through, whose writes are their feature - nor the lock table, the frame table or the log | (b) Waystone trail recording is in; (c) P5 is out and stays `cabin-controller.md`'s gap |
| DL-Q1 | The per-core parts: may a record wait for one reader's copy? | **no - marked, the operator's word**: the end state is strict (`raft-marks-2026-11-20.md` §9) | |
| DL-Q2 | The strict shape. | W1 wherever a row is fixed counters, W2 for structure, W3 for every reader; DL-S0 names each store's | (b) W2 everywhere, the simplest, losing counts while a reader copies; (c) a sequence-lock a store, no loss, a reader that retries |
| DL-Q3 | A statistic whose only consumer is switched off. | **it is not collected - marked, the operator's word** (`raft-marks-2026-11-20.md` §10), recorded as `rules.md` §3a's second rule; the collector while `cabin_optimizer` is off is its first case | |
| DL-Q4 | A wait-free word every core writes (§1.2's list). | within the rule - nothing waits; the census lists them, DL-S0 prices the ones a statement writes every time, and one priced above the cell's floor moves to a per-core word | (b) outside the rule: per-core words only, all moved; (c) left alone, unlisted |
| DL-Q5 | A view's row whose columns differ by a run in flight. | accepted for every store, as `IndexStats` states it today | (b) a row is read whole (then DL-Q2 (c) for that store) |
| DL-Q6 | Collection tasks (the steward's sample, `GATHER STATISTICS`, the ASH sampler, the controller's tick) under clause (e). | a task reads data as a plain read does - one page at a time under its shared latch, a slice's `IS` for a gather - and is paced; a writer may meet it as it meets any reader | (b) a task also gives way: it tries a page and skips or retries on a busy one, and a gather's slice ends when a stronger lock waits |
| DL-Q7 | A statistics reader's own waits (one view behind another, a view behind a tick's W3 copy). | outside the rule: the statement reading the report is not the statement reported on, and its wait is one bounded copy | (b) inside: readers try too, and report busy |
| DL-Q8 | What checks the rule (DL-S5). | both: a contract suite that, a store at a time, holds every latch the store owns from another thread and runs each record entry point under a deadline; and a debug-build scope around every record in which a blocking `Latch` acquisition or a page fetch aborts naming the caller | (b) the suite only; (c) the guard only |
| DL-Q9 | The close's bar. | the suite and the guard green; no cell slower outside its spread (DL-R7); under a reader storm (M3) the described statements' p99 and p99.9 inside the no-reader arm's spread; the drop rate reported, no bar on it | (b) a bar on the drop rate too; (c) the operator's numbers |
| DL-Q11 | Does the second rule add switches? | no: it binds where a setting already turns a consumer off. A store a `sys` view reports keeps BO-Q8's "no switch" and counts as consumed | (b) every store gets a switch, off meaning not recorded; (c) only the stores DL-S0 prices above the cell's floor get one |
| DL-Q12 | The planner's observations and wanted priors under `join_order`. | collected under every value: `ADVISE PLAN`, a capture's first compile and the views read them under `fixed` and `written`, so no value turns the consumer off | (b) not folded while `join_order` is `written`; (c) not folded under `fixed` either once a shape is captured |
| DL-Q13 | Evidence already collected when a consumer is switched off. | kept, decaying by the clock, with no new note; a switch-on then reads what is left of it | (b) cleared at the switch-off, so "from no evidence" is exact |
| DL-Q10 | The measurement budget. | the standing one: 20 minutes a stage, about an hour at the close, cuts named in the results file | (b) lifted for DL-S0 |

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
