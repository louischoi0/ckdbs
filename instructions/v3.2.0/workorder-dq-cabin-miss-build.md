# Work order DQ - a Cabin's miss answered from a map: the correlated Cabin probe's unobserved keys served by the statement-local build, one walk for the statement in place of one walk a miss

Status: **LANDED on `main` open at DQ-S5** on the operator's "push no
verify" (`raft-marks-2026-11-23.md` §4; the hook skipped, the suite 0 failed
of 5,159 at `7e31d272`, the same build inputs). **BUILT 2026-11-23 and STOPPED at DQ-S5 on a measured regression,
not closed** (DQ-Q13 (a), CLA's proposal as revised at DQ-S5's review,
not the operator's mark; `raft-marks-2026-11-23.md` §3; §7's DQ-S5). E1 is
built on top-level chains and sub-chains; E2 is not (DQ-S0). Everything is
reviewed, suite-green and committed on the work branch. The gate-declined
cabined join is 108x to 251x cheaper, and at the engine's defaults its
first statement is answered where it was refused; the recording
statement's refusal stays (DQ-Q11). Converged cabined-join statements read
7 % to 35 % dearer in cold cells, and join statements +0.2 to +0.5 us,
with the causes not isolated inside the spent measurement budget. The
operator's word either lifts the budget for a diagnosis and a remedy, or
accepts the residue and closes DQ. **On the lifted budget the residue was
diagnosed as placement** - memory (the sets a statement records land where
the previous statement's freed map was) and code (a new function moves the
code after it) - and each placement remedy measured trades the cost onto
another shape (`raft-marks-2026-11-23.md` §5). Before that: **OPENED 2026-11-23 on the operator's standing go-ahead**
(`/go-ahead-achieving-milestone DQ`, `raft-marks-2026-11-23.md` §1), at
`68e5a69f` (`v3.5.0-96-g68e5a69f`) on worktree `dq-cabin-miss-build`, branch
`worktree-dq-cabin-miss-build`. **DQ-Q0..Q8 adopted (a) on that go-ahead** -
CLA's proposals, not the operator's per-item mark (§4). Before that:
**WRITTEN 2026-11-22 (UTC), not opened.** Written in the main
checkout on `main` at `56b44964` (`v3.4.17-107-g56b44964` as a clone without
the local `v3.5.0` tag describes it) on the operator's words of that
conversation (`raft-marks-2026-11-22.md` §11). The letter DQ is CLA's
proposal. **Exploratory by CLA's proposal** (DQ-Q0): the operator's words ask
for a join method that works with the Cabin; that DQ-S0 measures the premise
before anything is built, and that DQ may close with nothing built, is CLA's.
**DQ-Q0..Q8 are unmarked.** No suite was run: the order changes no build
input.

Every `[source-read]` claim in §1 was read at `56b44964`. Nothing here is
measured: every cost in §1.3 and §2 is `[design]`, CLA's reasoning from the
source, and DQ-S0 is what turns it into a number or into E0.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` where a wrong choice turns a correct
answer into a wrong one with no error.

Related: `docs/spec/cabin.md` §4 (the read path), §4a (the correlated
probe), §5 (the witness), §6 (the announce), §6a (the banking gate), §8.1
(the per-column policy); `docs/spec/join-inner-build.md` §2, §4 (trust
class: none), §5 (the ladder's last arm), §6 (the stopping sub-chain), §7
(the cap), §8 (the exclusions); `docs/spec/parser-v2.md` I12 (the plan
surface); `docs/spec/cabin-controller.md` PO11;
`docs/inflight/known-gaps.md` ("A Cabin builds rarely on a busy instance");
`instructions/v3.2.0/workorder-cb-join-planner.md` (the cost model and the
build side); `workorder-cx-managed-plans.md` CX-S3a, CX-S5 (the `ACCESS` and
`BUILD` pins); `workorder-cu-key-set-access.md` (a value list over a Cabin);
`workorder-dl-statistics-queue-no-statement.md` (no record waits);
`instructions/v3.0.0/workorder-aq-expression-cabins.md`.

## 0. What DQ is

`[operator]` Three words in one conversation (given in Korean; recorded in
English):

1. *"Now, a way to add, for joins, a method that works together with the
   Cabin."*
2. After CLA's account of the join methods the tree has: *"How about
   unifying the Cabin and the hash map at the inner build?"*
3. After CLA's answer to that: *"Write the work order, then push."*

`[design]` **The gap, in one paragraph.** A join whose inner column carries
a Cabin compiles to the correlated Cabin probe, and that step never carries
the build annotation, which is the ladder's arm after it. So every outer row
whose key the Cabin has not observed walks the inner relation whole. The
same join on a column with **no** Cabin takes the statement-local build and
walks the inner relation once. Until its keys converge, and for as long as
the banking gate refuses, a declared Cabin therefore makes a join cost
O(missed outer rows x inner) where its absence costs O(inner). DQ lets the
Cabin probe's miss fall to the build the step would have carried, so the
worst case of a cabined join column is the uncabined one's.

**What DQ is not: one structure.** The operator's second word asked whether
the Cabin and the map could be unified. They stay two structures, for the
reasons in §1.4, and DQ joins them at the control flow: a hit is the
Cabin's, a miss is the map's, and the Cabin's observation of a key is the
recording walk it is today.

**What CLA told the operator, and what the tree corrects.**

- CLA proposed, as "M2", that the build's walk could **bank its buckets into
  the Cabin afterwards**, under conditions (the §6a view, no other conjunct,
  keys probed twice, the caps). **That is unsound as stated, and the
  conditions do not repair it.** A set is announced before its walk
  (`BeginRecording`, `src/exec/step_vm.cpp:1046`) so that every write from
  that instant, on any core, is appended by the witness before the walk's
  own matches are merged (`cabin.md` §6). A map built with no announce has a
  window - from its walk to the moment it would be banked - whose writes no
  witness appended. Only a key announced **before** the walk can be banked
  from it. What survives is narrower and is E2 below: the walk that records
  the one probed key may also bucket the map.
- CLA proposed a question on whether `BUILD <relation> OFF` should reach the
  fallback as a new piece of plan surface. **The clause already parses
  beside a Cabin pin**: only `BUILD ... ON` beside a keyed `ACCESS` is
  refused (`src/exec/plan_ddl.cpp:416`, `ArmPin::keyed()`
  `include/kds/exec/admin_plans.hpp:130-136`). DQ-Q5 is what `OFF` then
  means on a Cabin step, not whether it is spelled.
- CLA said the cost model prices a correlated Cabin probe as an index
  probe. That stands (`src/exec/join_order.cpp:238-243`).

## 1. The tree as read

### 1.1 What is measured `[measured]`

| measurement | reading |
|---|---|
| the build's constant (`join-inner-build.md` §5, against `bench/` at `1769487`) | 37.2 ns a bucketed inner row at 10,000 inner rows; the plain join's break-even under k = 2 outer rows |

**Not measured anywhere:** what a cabined join costs before its keys
converge; how many of a join's misses the banking gate refuses on an
instance with writes in flight; the miss walk against the build on one
shape. No results file under `bench/` sets a correlated Cabin probe beside
the build.

### 1.2 What the tree does `[source-read]` at `56b44964`

**The ladder, for a step's inner access** (`src/exec/step_compiler.cpp`,
the `else if` chain ending at `:3591`): the tuple key arms, the literal and
the correlated index arms, the Cabin arms (the column Cabin, the expression
Cabin, the predicate Cabin and the forms after them, the correlated one
among them), the filter scan, **and only then** the build annotation
(`:3578-3591`), taken where `walk` still holds - the step is still a bare
walk. A step the correlated Cabin arm claimed is `kCabinProbe`, so the
build's arm is never reached and `Step::build` stays empty
(`include/kds/exec/step_chain.hpp:786`).

**The correlated Cabin arm** (`CorrelatedCabinProbeOf`,
`step_compiler.cpp:1888-1910`): a residual conjunct that is a correlated
equality on a non-tuple-key own column, a column Cabin on that column, the
two sides of one descriptor. It makes a column probe only
(`ColumnProbeFor`), so no expression or predicate Cabin is probed per outer
row. **The build's key** (`BuildKeyOf`, `:1959-1985`) is the same conjunct
under one further rule: no other conjunct of the step may read a column
that is not the step's own.

**A probe at run time** (`RunCabinStep`, `src/exec/step_vm.cpp:928-992`):

| the key | what runs | line |
|---|---|---|
| observed | the set served, `cabin_hits` counted | `:955-959` |
| unobserved, not due to record (`n = 2`'s first touch, or the value cap) | `RunWalkStep`: the inner relation walked whole | `:984-990` |
| unobserved, due to record | `WalkAndRecord`: the same walk with a recording live | `:991` |
| due to record, but the view was minted beside an in-flight transaction or belongs to one | `NoteUnbankableView`, then `RunWalkStep` | `:1031-1034` |
| due to record, the announce or the instance's second answer refused | `RunWalkStep` | `:1046`, `:1095` |

The correlated form records at the second touch whatever the declaration
says (`declared_here`, `:982`; `kAutoRecordThreshold = 2`,
`include/kds/stats/cabin_store.hpp:298`), and the sighting table is reset
wholesale at `kMaxSightings = 4096` (`:283`).

**The build at run time**: a step carrying `Step::build` is dispatched to
`WalkAndBuild`, or to `WalkPrefixAndProbe` in a sub-chain runner
(`step_vm.cpp:651-656`); the first walk buckets every row passing the
non-correlated residual, and a later outer row probes the map
(`ProbeBuild`, `:1378`), each entry verified at its slot
(`join-inner-build.md` §2). The cap is `join_build_max_rows`, 65,536 rows
by default (`include/kds/exec/budget.hpp:80`); past it the step walks per
outer row for the rest of the statement, never an error (§7 there).

**The cost model** prices `kCabinProbe` with the index arms:
`set_values * kDescentCost + matches * (1 + kDescentCost)`
(`join_order.cpp:238-243`). A miss's walk is not in it. A built walk is
priced `relation_rows + own * kBuildInsertCost` once and
`kBuildProbeCost + matches` an execution (`:244-262`).

**The plan surface**: `ACCESS <relation> CABIN [(<column>)]` and
`BUILD <relation> ON | OFF` pin a step's arm and its build (CX-S3a, CX-S5);
`ON` beside a keyed `ACCESS` is refused, `OFF` is not
(`plan_ddl.cpp:416`). A Cabin the controller made is read only through a
plan that names it (PO11).

**The known gap this meets**: "A Cabin builds rarely on a busy instance"
(`docs/inflight/known-gaps.md`, the Cabin section): one autocommit write in
flight on any core declines every build on every core, so a Cabin may never
fill on a write-heavy instance.

### 1.3 What follows `[design]`

Outer rows `k`, inner rows `N`, distinct outer keys `d`, of which `r`
repeat inside the sighting window. Walks of the inner relation a statement
pays, before any key is observed:

| the inner column | walks |
|---|---|
| no Cabin, no index (the build) | 1 |
| a Cabin, keys never repeat | `k` |
| a Cabin, keys repeat, the gate open | about `2r` + the non-repeating keys' touches; later statements serve |
| a Cabin, the gate refusing (a write in flight at every mint) | `k`, in every statement, with no convergence |

The last row is the one the known gap makes reachable in production: on a
write-heavy instance a declared Cabin turns a join the build would answer
in one walk into `k` walks, permanently. Purpose 2 of the engine
(`CLAUDE.md`) is acceleration that can be dropped without changing a
result; it says nothing of cost, so this is not a contract break. It is the
opposite of what an operator declaring a Cabin expects. **Corrected by
DQ-S0** (§7): the cost is not a contract break, but the refusal it can
reach is one. At the engine's defaults, a join the build answers is
refused for its row-touch budget once a Cabin is declared, and `cabin.md`
§4a says §1 forbids an accelerator that turns a rows-returning statement
into a budget refusal.

### 1.4 Why the two structures stay two `[design]`

| | the build's map | a Cabin set |
|---|---|---|
| trust | none: the statement's own read under its snapshot (`join-inner-build.md` §4) | authoritative for an observed value (`cabin.md` §1) |
| completeness | the rows passing the step's non-correlated conjuncts | every tuple key of the value, a superset trimmed at the read |
| writes after it is made | none can matter: a SELECT writes nothing between its build and its probes | the witness appends every one (`cabin.md` §5) |
| life and reach | the statement, its core | the instance, every core appending |
| bound | `join_build_max_rows`, 65,536 rows | `cabin_max_values` 4,096 values, 4,096 entries a value (`include/kds/server/expeditor.hpp:409-410`) |
| declared | no | a catalog object |

`join-inner-build.md` §8 already rules the reuse of a map across statements
out by name: that is what the Cabin is. Three things make a single
structure wrong rather than merely unbuilt, each `[quiet-wrong]`:

1. **the announce's order** (§0's first correction): a set banked from a
   walk that was not announced misses the writes of the window;
2. **the map is a subset**: it holds the rows passing the step's other
   conjuncts, and a set authoritative for a value must hold every row of
   the value;
3. **the view**: the banking gate refuses a view minted beside an in-flight
   transaction or belonging to one (`cabin.md` §6a), and the map is built
   under any view.

The entry is already shared: the map's unit is the Cabin's 24-byte entry
(`join-inner-build.md` §7).

## 2. The candidates `[design]`

Each is priced at DQ-S0. None is chosen here, and none is built or
measured.

- **E0 - nothing.** The verdict if DQ-S0 finds the miss walks too few to
  matter on the shapes it measures, or the build's first-walk constant
  eating what the fallback saves.
- **E1 - the miss falls to the build.** A correlated Cabin step whose
  conjunct also passes `BuildKeyOf`'s rule carries the build annotation
  beside its probe. At run time a hit is served from the set as today. A
  miss that would end in `RunWalkStep` goes to the build's arm instead: the
  statement's first such miss walks and buckets, every later one probes the
  map. A miss due to record is `WalkAndRecord`, unchanged (DQ-Q2). The Cabin
  observes exactly what it observes today; only the non-recording walks
  change. **Made precise at DQ-S0's review**: the recording walk is
  unchanged, and a recording `WalkAndRecord` declines before its walk
  falls to the build too. That covers its four exits: the key column not
  decoded, an unbankable view, a refused announce, and the instance's
  second answer. Agate's misses from its second statement on are all
  declined there. A step that fails `BuildKeyOf`'s rule, and a declined or outgrown
  map, walk per miss as today.
- **E2 - the recording walk also buckets.** `WalkAndRecord`'s walk is a
  whole walk of the inner relation with one key announced. Where the map is
  not yet built it may bucket every row as the build does, so the
  statement's first miss due to record costs one walk for both. The set is
  banked by `WalkAndRecord`'s own rules, for its own announced key and no
  other. `[quiet-wrong]` only through the map's own contract: the recording
  walk in a sub-chain runs through the stop (`cabin.md` §4a), and the build
  in a sub-chain is a prefix map, positive-first (`join-inner-build.md`
  §6); the two rules must not be composed without DQ-S0 reading both.
- **E3 - a recording touch served from the map** (DQ-Q2 (b)). Once the map
  is built, a key at its recording touch probes the map instead of walking,
  so the statement pays one walk whatever its keys do. The Cabin then
  observes at most the statement's first missing key, and a join no longer
  converges its Cabin: the map answers every statement and the set never
  fills. Listed because it is the cheapest statement; not proposed.
- **E4 - the model prices the miss.** `kCabinProbe` with a `key_from`
  priced as the built walk where the step carries the annotation. This
  moves orders under `join_order = planned` and the captures made after it
  under `fixed`. Not proposed in DQ (DQ-Q6).

**Not candidates.** One structure for both (§1.4); a map kept across
statements (`join-inner-build.md` §8); banking more than the announced key
from any walk (§0); a Cabin made where none is declared (the plan is
`f(shape, catalog)`, and PO11); a Bound Cabin read by a SELECT (§6).

## 3. Rules

- **DQ-R1 - one answer, three configurations** `[quiet-wrong]`. A statement
  over a cabined join column returns the same rows, in the same order under
  the same contract, with the Cabin serving, with the fallback map serving,
  and with both taken away (`ACCESS ... SCAN`, `BUILD ... OFF`). The Cabin
  and inner-build contract suites, which compare configurations byte for
  byte (`tests/cabin_contract_test.cpp`,
  `tests/inner_build_contract_test.cpp`), are extended with the mixed
  statement: some outer rows served from a set, the rest from the map.
- **DQ-R2 - nothing is banked that was not announced** `[quiet-wrong]`. No
  code path writes a set from the map. A set's only source stays
  `WalkAndRecord`, under every rule it has (`step_vm.cpp:998`, "the only
  place a Recording is created").
- **DQ-R3 - the Cabin observes what it observes today.** Under E1 and E2 a
  key's sightings, its recording touch and its commit are unchanged; the
  cabin store's counters (`hits`, `misses`, `unbankable_views`, the cap
  refusals) read as before for the same statement. A change to that is E3
  and needs the operator's mark (DQ-Q2).
- **DQ-R4 - the map's rules are the build's.** `BuildKeyOf`'s one-conjunct
  rule, the same descriptor, no function or boolean conjunct on the step, a
  SELECT's chain only (no `CompileWhere` sub-chain, no scalar sub-chain),
  `VerifyTupleAt` at every probe, `join_build_max_rows` with its walk
  fallback and its `0`. DQ adds no second rule for when a map may be built
  and no second cap (`CLAUDE.md`: never a second name for a quantity).
- **DQ-R5 - the plan stays `f(shape, catalog)`.** The annotation is set at
  compile from the shape and the catalog. Which of the three arms a given
  outer row takes is run-time state inside one kind, as a Cabin's
  hit-or-miss branch already is. `AccessKind` gains no value (DQ-Q1), no
  fingerprint moves, no plan row and no superblock moves.
- **DQ-R6 - a step with no fallback pays nothing.** A statement with no
  correlated Cabin step, and a Cabin step whose every key is served,
  executes the instructions it executes today but for one test of the
  annotation on the miss path. Measured at the close, interleaved A/B.
- **DQ-R7 - no record waits** (DL, `docs/rules/rules.md` §3a). The fallback
  adds no latch to a statistics record and takes no latch of the cabin
  store that the miss path does not already take.
- **DQ-R8 - `ANALYZE` says which arm answered.** A step that fell back
  reports the existing fields together: `cabin_hits`, `cabin_misses`,
  `inner_built`, `build_rows`, `build_probes`. No new field unless DQ-S0
  finds a case the existing ones cannot tell apart.

## 4. Items for the operator (DQ-Q), CLA's proposals

(a) is CLA's proposal in each. **DQ-Q0..Q8 (a) adopted on the operator's
standing go-ahead of 2026-11-23 (go-ahead-achieving-milestone)**
(`raft-marks-2026-11-23.md` §1); none is the operator's own mark.

| | the question | (a) | other |
|---|---|---|---|
| DQ-Q0 | Is DQ exploratory? | **Yes**: DQ-S0 measures, and E0 closes DQ with nothing built | (b) build E1 without the premise |
| DQ-Q1 | A new `AccessKind`, or a branch inside `kCabinProbe`? | **A branch**: the step stays `kCabinProbe` and carries `Step::build` beside `Step::cabin`; the build's contract ("An annotation, never a kind", `step_compiler.cpp:3588`; `BuildKey`, `step_chain.hpp:400`) is amended to admit a Cabin step | (b) a kind of its own, which every `switch` over `AccessKind`, the statistics key and the plan printer then name |
| DQ-Q2 | A key at its recording touch, once the map is built | **It walks and records, as today** (DQ-R3): the Cabin converges as it does now, and a repeating key costs one walk in place of two | (b) E3: it probes the map, the statement pays one walk in all and the Cabin stops converging through joins; (c) a bounded number of recording walks a statement, a constant DQ-S0 would have to measure |
| DQ-Q3 | E2, the recording walk bucketing the map | **Build it only if DQ-S0 shows the first miss is often a recording one**, top-level chains only | (b) never; (c) sub-chains too |
| DQ-Q4 | Sub-chains (a correlated `EXISTS` over a cabined column) | **Top-level join steps first; sub-chains a stage of their own (DQ-S3), gated on DQ-S0's reading of the two stop rules** `[quiet-wrong]` | (b) both at once; (c) top-level only, sub-chains left to `known-gaps.md` |
| DQ-Q5 | What `BUILD <relation> OFF` means on a Cabin step | **It takes the fallback away**: the step is today's correlated probe, a miss walks. `ON` beside `ACCESS ... CABIN` stays refused | (b) `OFF` is ignored on a Cabin step and only `join_build_max_rows = 0` switches the fallback off |
| DQ-Q6 | The cost model | **Unchanged in DQ**: the fallback is a run-time guard, no order moves, no capture moves; the unpriced miss goes to `known-gaps.md` with DQ-S0's numbers | (b) E4, the miss priced, under `planned` only |
| DQ-Q7 | A value list over a Cabin with one value unobserved (CU-Q12 walks the relation whole) | **Out of DQ**: a set's all-or-nothing rule is CU's, and a literal list has no outer rows for a map to serve | (b) a stage of DQ |
| DQ-Q8 | The bar a built stage must meet | **At DQ-S0's gate-refusing cell, server CPU within 1.5x of the same join with the Cabin dropped; on a converged join, no cell slower than today outside the control's range; DQ-R6 met** - the 1.5 is provisional and is replaced by what DQ-S0 measures | the operator's number |

**Raised by DQ-S0 and DQ-S1, adopted (a) on the same standing go-ahead**
(`raft-marks-2026-11-23.md` §2); none is the operator's own mark. **DQ-Q13,
raised at DQ-S5, adopted (a) on the same go-ahead** (`raft-marks-2026-11-23.md`
§3), as revised at DQ-S5's review; it is not the operator's mark either.

| | the question | (a) | other |
|---|---|---|---|
| DQ-Q9 | A step an administrator's plan pins `ACCESS <relation> CABIN [(<column>)]`: the walk class is not admitted (`walk` is false, `step_compiler.cpp:3407`) | **It keeps the fallback**: the fallback is not a ladder rank but the Cabin step's own miss path, and a controller-made Cabin is read only through such a pin (PO11), so denying it there would leave exactly the pinned join with the per-miss walk. `BUILD OFF` alone takes it (DQ-Q5) | (b) a keyed pin keeps today's per-miss walk |
| DQ-Q10 | The plan statistics' decline record (`command_dispatcher.cpp`, the fold after a keyed run): a step carrying `Step::build` that opened and published no map is folded as a decline, which the cost model reads for a walked step | **A Cabin step's decline is a fallback that bucketed rows and published no map** (`build_rows > 0`, `inner_builds = 0`): an opening its set served built nothing and declined nothing. A walked step keeps its rule | (b) a Cabin step never records a decline |
| DQ-Q11 | DQ-S0 measured a refusal: at the engine's defaults (`max_rows_touched` 100,000,000, `join_build_max_rows` 65,536) a 60,000-row cabined join with 2,000 unobserved outer keys is refused where the uncabined one answers in 10.7 to 14.6 ms; the window is an inner relation within the cap and k x N above the budget, since past the cap the build declines and walks too. E1 removes it for every non-recording miss and every declined recording, but not for a statement whose misses are all recording touches (DQ-Q2 (a) keeps each a whole walk) | **Recorded in `known-gaps.md` (Cabin)**, with DQ-Q2 (b) and (c) the remedies the operator may take; DQ keeps DQ-Q2 (a) | (b) take DQ-Q2 (c), a bounded number of recording walks a statement, into DQ |
| DQ-Q12 | A correlated Cabin step that also carries an unindexed literal equality (`HasUnindexedEqualityFilter`): its uncabined twin is a filter scan with no build (`join-inner-build.md` §8) | **The fallback is declined there**, as the build is, so the miss's worst case is exactly the uncabined step's (§0) and the map keeps one rule (DQ-R4) | (b) annotate it anyway: cheaper than the uncabined step, and a second rule |
| DQ-Q13 | The close's residue (`bench/v3.5.0/results-dq-close-v3.5.0-101-g7e31d272b.md`). Converged cabined-join statements read 7 % to 35 % dearer on B than on A where the state is cold or in a run whose first statement the fallback answered: about 0.1 to 0.3 ms on a statement of about 1 ms, with the cause not isolated. Join statements read +0.2 to +0.5 us (0.5 % to 1.2 %), unattributed, within about 1 us of the noise. So DQ-Q8's converged clause is not met in those cells, and DQ-R6 is not shown met. The close's measurement budget is spent | **DQ stays open at DQ-S5, stopped** (revised at DQ-S5's review from closing it with the residue. That proposal set aside the bar CLA itself set, DQ-Q8 (a), and under the standing word a regression measured at the close is work, and a spent budget is a stop, not a close). Everything is built, reviewed, suite-green and committed on the work branch, and the residue is in `known-gaps.md`. The operator's word either **lifts the measurement budget** for the diagnosis and a remedy (per-thread CPU by thread id, the allocator's state after a freed map, a stage bisect of the join residue), or **accepts the residue and closes DQ**, which the push word would then land. Basis: the gain is bounded below by 108x on every gate-declined cell, with a refusal at the defaults answered at the first statement; the residue is bounded above at 0.1 to 0.3 ms on a converged statement and +0.5 us on a join; and the one remedy tried (B2) traded statement #3's binary part for 1 % to 2 % on whole-relation walks, so a remedy needs the cause first **Diagnosed on the operator's lifted budget** (`raft-marks-2026-11-23.md` §5): the residue is placement, not instructions. The sets a statement records land in the memory the previous statement's freed map left, and the code after the new `MissWalk` moves. Each placement remedy measured trades the cost onto another shape. **CLA's proposal for the operator's word: accept the residue and close DQ.** This is not adopted on the standing word, because it would set aside the bar CLA set | (b) close DQ with the residue recorded as a measured cost, not declared met (the proposal as first written); (c) withdraw E1 |

## 5. Stages

- **DQ-S0 - census and premise.** No engine change. (i) Re-read every
  `[source-read]` claim of §1.2 at the commit DQ opens from. (ii) Read the
  two stop rules side by side (`cabin.md` §4a's completion license,
  `join-inner-build.md` §6's prefix map) and write, for DQ-Q4, whether they
  compose. (iii) Measure, `build-release`, `cores = 1`, `relaxed`, every
  page resident, interleaved, inside the 20-minute stage budget: one join,
  inner 10,000 and 100,000 rows, outer 1,000 rows, the inner column (A)
  cabined, (B) the Cabin dropped, under three key distributions - no key
  repeats, every key repeats, a skewed mix - and for (A) a fourth arm with a
  second session holding a write in flight so the banking gate refuses.
  Read: server CPU a statement, walks of the inner relation
  (`correlated_scans`, `cabin_misses`), and the statement at which (A)
  first meets (B). **The verdict**: E0, or E1 with or without E2, with the
  1.5 of DQ-Q8 replaced by a measured number. A stop for the operator if
  the verdict is E0 or if (ii) finds the stop rules do not compose.
- **DQ-S1 - the annotation.** The compiler sets `Step::build` on a
  correlated Cabin step where `BuildKeyOf` accepts it (DQ-R4, DQ-R5), the
  plan pins read per DQ-Q5, `ANALYZE` and the plan printer show it. No
  executor change: the annotation is carried and unread, and the contract
  suites are byte-identical.
- **DQ-S2 - E1, top-level chains.** The miss path of `RunCabinStep` falls
  to the build's arm; the mixed-statement cells of DQ-R1; a crash-free
  feature, so no sim cell beyond the simulator drawing the shape.
- **DQ-S3 - sub-chains**, only past DQ-S0 (ii) and DQ-Q4.
- **DQ-S4 - E2**, only if DQ-Q3's condition is met.
- **DQ-S5 - the close.** The interleaved A/B over DQ's whole change
  (DQ-R6, DQ-Q8), DQ-S0's cells run again, `cabin.md` §4a and
  `join-inner-build.md` §2, §5 and §8 amended, `CLAUDE.md`'s Cabin and
  inner-build rows, `known-gaps.md`.

Every built stage is reviewed by `critics-developer` and gated by the full
suite; overhead is measured at the close.

## 6. What DQ does not do

- **A Bound Cabin as a join source** (CLA's "M3" in the conversation): a
  group-key join served with no miss branch, and `COUNT`/`SUM` answered
  from a group header. A Bound Cabin is the assertion's substrate and no
  SELECT reads one (`src/exec/bound_cabin.cpp`, `assertion_check.cpp`); its
  `RESERVED` entries against a reader's snapshot are a `[quiet-wrong]`
  question of their own. A separate order if the operator asks for one.
- **A hoisted hash join, a merge join, a multi-column join key, a
  non-equality join, a spilled map** (`join-inner-build.md` §8, CX-Q9).
- **The banking gate sharpened** (the known gap's own remedy, a per-value
  record of which unresolved transactions could contradict a build).
- **A Cabin created by the engine for a join it saw build repeatedly.** The
  controller builds and chooses no statement's arm (PO11); a repeated build
  as a signal to it is a question for that spec, not a stage here.
- **Any change to an index step, a tuple key step, or a literal Cabin
  probe.**

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-22 (UTC)

In the main checkout on `main` at `56b44964`, from a claude.ai conversation
session with no build tree: §1.2 read in that checkout, nothing built,
nothing run. Not reviewed by `critics-developer`. The premise is unmeasured
and the order says so in every place it states a cost.

### DQ-S0 - census and premise (2026-11-23)

On worktree `dq-cabin-miss-build` at `68e5a69f` (`v3.5.0-96-g68e5a69fd`).
No engine change. Results:
`bench/v3.5.0/results-dq-s0-premise-v3.5.0-96-g68e5a69fd.md`.

**(i) §1.2 re-read at `68e5a69f`: every claim holds, line for line.** No
build input changed between `56b44964` and `68e5a69f`. The census found
these places relevant to DQ. The first and last are not readers of
`Step::build` (one reads the plan pin, the other the `inner_build` flag);
the review corrected the list:

- `step_compiler.cpp:3407`: a keyed pin makes `walk` false and so takes
  the build's rank. This is DQ-Q9.
- `command_dispatcher.cpp`, the plan statistics' fold: a step that carries
  the annotation, opened, and published no map is folded as a decline. Read
  unchanged, every Cabin step its set served would be folded as a decline.
  This is DQ-Q10.
- `join_order.cpp:255`: the cost model reads the annotation only under the
  walked kinds, so DQ-Q6 (a) needs no code.
- `plan_printer.cpp:206` and `:549`: the plan and `ANALYZE` print the
  annotation and `inner_built=` for every step that carries one, so DQ-S1's
  display needs no new code (DQ-R8).
- `step_compiler.cpp:2296-2297` and `:2521`: `inner_build` is false under
  `CompileWhere` and for a scalar sub-chain. DQ-R4's SELECT-only rule holds
  by construction, provided the fallback is gated on the same flag, which
  DQ-S1 does.
- Readers that ignore an annotated Cabin step because each tests the kind
  first: `step_compiler.cpp:1480`, `:3595` and `:3771`. This supports
  DQ-Q1 (a) and DQ-R5.
- `BuildIsArmable` (`step_vm.cpp:1189`) is the run-time guard E1 passes
  through. It holds for a Cabin step, because the correlated conjunct stays
  in `Step::residual`.

One comment is stale: `WalkAndRecord`'s "the miss path and the heap
hint-failure path both end here" names two callers, and it has one
(`step_vm.cpp:991`).

**(ii) The two stop rules: E1 composes with both, E2 does not compose
with the prefix map.**

- E1 composes because the two rules never meet in one walk:
  - A recording walk (`WalkAndRecord`, `cabin.md` §4a's completion
    license) runs through the stop, touches no map, and leaves the prefix
    map's state and mark as they were.
  - A fallback walk is `WalkPrefixAndProbe`'s: it replays the bucket,
    resumes at the mark, and extends the map. No recording of this step
    is live in it: every decline leaves `WalkAndRecord` before
    `recording_` is set. An outer step's recording can be live, and the
    step-id checks make it harmless.
  - A hit serves the set and touches neither.
  - So each walk keeps its own rule whole, and interleaving them changes
    neither.
- E2 would put both rules in one walk. The completion license visits the
  rows after the stop for the recording alone and returns before the
  bucketing (`step_vm.cpp`, the recording block's `if (stopped_) return`).
  The walk still advances the prefix mark and, uncut, sets `complete`.
  - So a map bucketed only to the stop would read `kBuilt`, and a later
    key past the stop would answer an empty bucket as a conclusive
    absence. That is a wrong `EXISTS`.
  - Freezing the mark at the stop and withholding `complete`, as the cap's
    frozen map does, would compose. That is moot while E2 is not built.
- E2 is also not built (iii). DQ-Q4 (a) therefore stands, and **DQ-S3 runs**.

**(iii) The premise holds, and is worse than §1.3 stated.**

- Where the banking gate declines, the cabined join costs 116x to 159x
  (10,000 inner rows) and 183x to 249x (100,000 inner rows) what the
  uncabined one costs, statement by statement, in every statement.
- At the engine's defaults a 60,000-row cabined join with 2,000 outer keys
  is refused at both statements, and the uncabined one answers in 10.7 to
  14.6 ms (DQ-Q11). The 100,000-row census shows the same, with the build
  cap raised so that B could build.
- **The cap bounds E1.** Past `join_build_max_rows` (65,536 by default) the
  map declines and every miss walks, with or without E1.
- Converged, the Cabin is 3.9x to 16.9x cheaper than the build in wall
  time.
- **The verdict:** E1, without E2.
  - **DQ-Q3 (a)'s condition was read as whether E2 saves a walk, not as
    written.** As written ("the first miss is often a recording one"), it
    is met in `unique` #2 and `skewed` #2, where every miss is a recording
    touch.
  - But E2 saves at most one walk a statement, by construction: under
    DQ-Q2 (a) each later recording touch walks anyway. In these cells it
    saves none.
  - The rejection rests on that bound. DQ-S4 does not run.
- DQ-Q8's 1.5x stays provisional until DQ-S5 measures it. Agate under E1
  is predicted at 1.3x (10,000 rows) and 1.05x (100,000 rows, with the cap
  raised) of B, on the assumption that a miss's bookkeeping costs no more
  than a set's serve. No cell measured that bookkeeping.

**Raised:** DQ-Q9 and DQ-Q11 here, DQ-Q10 by (i), and DQ-Q12 at DQ-S1's
compile rule. All four are adopted (a) on the standing go-ahead
(`raft-marks-2026-11-23.md` §2).

Time: building 5 minutes, running 9, quiet hold 0 s. No suite: no build
input changed (`tools/`, `bench/`, `instructions/` only).

**Review** (`critics-developer`, at `f70d9f4f`).
- Applied:
  - two driver bugs, fixed by the reviewer: a CPU read that could miss up
    to a 4 ms tick (`settled_cpu_ns`), and a division by a 0 reading in the
    report;
  - F1, the refusal claim, which held at 100,000 rows only with the build
    cap the driver raised. It was measured again at the defaults (60,000
    rows, 2,000 outer keys, at 00:50 UTC) and is restated with the cap
    beside it everywhere;
  - F2, §2's E1 made precise;
  - F3 to F5, the ratios and figures, recomputed from the JSON;
  - F6, DQ-Q8 kept provisional;
  - F7, the driver's changes inside the stage, recorded in the results
    file;
  - F8, the plan lines' provenance;
  - F9, the reading of DQ-Q3 (a)'s condition;
  - F10, §1.3;
  - the census and (ii) wording;
  - the driver's simplifications: the unused summary, the copied `med`
    and `rotated`, and dead parameters.
- Rejected:
  - folding the driver's `Server` into `dk_close.Server`, which would
    change a driver DK's archived measurement was taken with;
  - one shared `quiet_gate`, left for when a third driver needs it.

### DQ-S1 - the annotation (2026-11-23)

On worktree `dq-cabin-miss-build`, over `60279878`.

**What the compiler does now:**
- **The rule is one function.** `BuildAnnotationOf` (`step_compiler.cpp`)
  is asked by the walked join's arm and by the correlated Cabin probe's
  arm, so DQ-R4's one rule holds by construction. The probe's arm sets
  `Step::build` beside `Step::cabin`, and the step stays `kCabinProbe`
  (DQ-Q1 (a)).
- **The walk class aside, the rule is the build's** (DQ-Q5, DQ-Q9, DQ-Q12):
  - `ACCESS <relation> CABIN` keeps the annotation;
  - `BUILD OFF` takes it;
  - `BUILD ON` beside a Cabin pin is refused, as before;
  - a literal equality that would make the walk a filter scan declines it,
    as do a function or boolean conjunct, a second correlated conjunct, a
    DML `WHERE` sub-chain and a scalar sub-chain.
- **The plan statistics' fold changed** (DQ-Q10): a Cabin step's decline is
  `build_rows > 0` with no map published. This was mutation-checked: the
  old rule fails `AdminPlanTest.ACabinStepThatBuiltNoMapFoldsNoDecline`.
- **Display:** the plan and `ANALYZE` print ` build on=colN key=...` and
  `inner_built=` on a Cabin step (DQ-R8), with no printer change.
- **Not read by the executor yet:** `RunStep` dispatches `kCabinProbe`
  before it reads `build`.
- **DQ-R1, every cap:** the inner-build contract suite gains a cabined join
  at every cap and every round of the Cabin's convergence, byte for byte
  against the uncabined walk.

**Review** (`critics-developer`): no correctness bug.
- Applied:
  - the rule folded into one function;
  - the Cabin cases of the scalar and DML sub-chain tests folded into the
    existing tests, run over both catalogs;
  - the plan-pin test made table-driven;
  - five comments corrected: the Cabin arm's ("a key the set has not
    observed costs what the uncabined join costs" was false for a
    recording touch), `BuildKey`'s, the printer's `inner_built=0`,
    `ArmPin`'s `OFF`, and `CorrelatedCabinProbeOf`'s.
- Rejected:
  - a `NOT SCAN` pin test, because `NOT SCAN` is not a pin the grammar
    admits (`admin_plans_test.cpp`, the refused `access=0:!SCAN`);
  - folding the five `CompileWhere` blocks into one helper, which is older
    than DQ;
  - annotating only top-level chains until DQ-S3. The sub-chain's
    annotation is carried and not read until then (spec §5a), and DQ-S3
    follows in this milestone.
- Two gaps the reviewer named are left for DQ-S2, where they become
  testable:
  - DQ-Q10's positive half (an outgrown fallback folds a decline);
  - the fold's walked branch, which has no dispatcher-level test anywhere.
    That gap is older than DQ.

Suite: 0 failed of 5,150 on this commit's build inputs. Overhead not measured; measured at the
milestone's close.

### DQ-S2 - E1 on top-level chains (2026-11-23)

On worktree `dq-cabin-miss-build`, over `46cd483c`.

**The executor reads the annotation.** `MissWalk` (`step_vm.cpp`) is not a
coroutine. It hands back `WalkAndBuild` where the step carries the
annotation on a top-level chain, and `RunWalkStep` otherwise.
- **Every miss path that records nothing calls it.** That is six paths:
  `RunCabinStep` with no key (no store, a NULL or unbindable value), a
  sighting not due to record, and `WalkAndRecord`'s four exits before a
  Recording exists: the key column not decoded, an unbankable view, a
  refused announce, and the instance's second answer.
- **The recording walk is unchanged.** Every Cabin-store call runs before
  the branch, as it did, so DQ-R2 and DQ-R3 hold by construction.
- **A miss without the annotation** pays a branch and no coroutine frame
  (DQ-R6).

**Docs:**
- the inner-build spec's new §5a;
- `cabin.md` §4a;
- the manual's Cabin section, its join paragraph and its `BUILD OFF`;
- the known-gaps entries, stated for the tree as it now is.

**Tests:**
- `tests/cabin_miss_build_test.cpp`:
  - a gate-declined join answered from the map in every statement, with
    the counters pinned;
  - the budget refusal it removes;
  - DQ-R1's mixed statement (sets, a recording walk and probes, one
    answer);
  - DQ-R3, the same Cabin report with and without the fallback: sets,
    entries, hits, misses, the gate's declines and the cap's refusals;
  - a NULL outer key and a switched-off store.
- `AdminPlanTest.AnOutgrownBuildFoldsADeclineWalkedOrAsACabinsFallback`:
  DQ-Q10's other half, with the walked branch beside it, which had no
  dispatcher-level test before.
- Mutation check: with `MissWalk` forced to walk, the four behaviour tests
  fail and the two invariance tests pass.
- **Where DQ-R1 is pinned:**
  - the mixed statement and the gate cells in the new file, as sorted rows;
  - `cabin_contract_test.cpp`'s correlated self-joins, which now go through
    the fallback and are compared byte for byte against the uncabined
    reference;
  - the inner-build contract suite's cabined join at every cap and every
    round, byte for byte (S1).
  - So DQ-R1's text holds, with the mixed statement in a file of its own.

**Review** (`critics-developer`): no correctness bug.
- Applied:
  - the known-gaps entry and `cabin.md` §4a stated for S2 (a correlated
    `EXISTS` still walks);
  - two test lines that pinned nothing, removed;
  - DQ-R3's comparison extended to the gate's declines and the cap's
    refusals;
  - the NULL-key and no-store paths given a test;
  - three comments corrected;
  - the manual given the cap, and the in-flight exception;
  - §5a told that the Cabin controller's `pages` signal falls where the map
    replaces walks.
- Deferred to DQ-S3: folding `MissWalk` and `RunStep`'s build dispatch
  into one function, once both have the sub-chain arm.
- Rejected: one shared `Rows()` helper for the four test files that copy
  it, which is older than DQ.

Suite: `DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter` failed
once at the first run, at a load average of 5.7. It is the known flake
(`known-gaps.md`, Testing), and it passed 8 of 8 repeats alone. The
reviewed tree's suite: 0 failed of 5,156. Overhead not
measured; measured at the milestone's close.

### DQ-S3 - sub-chains (2026-11-23)

On worktree `dq-cabin-miss-build`, over `910d8d63`.

**The executor:**
- **A sub-chain's miss that records nothing goes to §6's prefix map**
  (`WalkPrefixAndProbe`). DQ-S0 (ii) said this composes, and DQ-Q4 (a)
  gated the stage on it.
- **One dispatch.** `BuiltWalk` (`step_vm.cpp`, not a coroutine) is the
  build annotation's arm for both a walked join's step (`RunStep`) and a
  Cabin probe's miss (`MissWalk`).
- **One guard.** `BuildIsArmable` and the off-switch are asked there once,
  for both forms, where each form used to ask them for itself.
- **The recording walk is unchanged.** Its completion license runs through
  the stop in a walk of its own, which neither reads nor moves the map or
  its mark.

**Docs:** §5a's sub-chain paragraph, including the frozen form past the
cap; `cabin.md` §4a; the manual; known-gaps.

**Tests** (`tests/cabin_miss_build_test.cpp`):
- **A gate-declined `EXISTS` and `NOT EXISTS`, pinned exactly.**
  - 16 misses, 71 rows examined, 33 rows bucketed, the map complete, and
    15 replays.
  - The per-row walk examined 896. That figure is the mutation check: with
    the sub-chain fallback forced off, the test fails at it.
- **One `EXISTS` statement whose outer rows take every arm in turn:** a
  hit, two recording walks, nine first sightings through the prefix map,
  three hits, and a recording.
  - The map still buckets 33 rows and completes, so the recordings neither
    moved its mark nor filled it. That is DQ-S0 (ii)'s claim, run.
- **DQ-R3 with a sub-chain:** the same Cabin report with and without the
  fallback, now over the join and over a correlated `EXISTS`.
- **DQ-R1 for sub-chains:** the gate open, against the walk with neither
  structure and against the Cabin alone.
- **The inner-build contract suite** puts its `EXISTS` and a `NOT EXISTS`
  first, so round 0 meets the prefix map, frozen under caps 1 and 2.

**Review** (`critics-developer`): no correctness bug.
- Applied:
  - the interleaved statement, with its counters;
  - the exact pins in place of a bound with 9 rows of slack;
  - the contract suite's order;
  - the shared guard hoisted;
  - the duplicate counter helper removed;
  - `MissWalk`'s comment cut;
  - two spec sentences corrected: "every inner row once" holds within the
    cap only, and "a hit" there means a replayed row that qualifies;
  - known-gaps given its commit.
- Rejected:
  - merging `MissWalk` into `BuiltWalk`, since `RunStep` would then call a
    function named for a miss;
  - folding the five-field `BuildRecording` setups into a helper, which is
    older than DQ;
  - tests for a NULL key, `IN`'s per-row form, a Cabin step at index 1 or
    later, a nested sub-chain and a hoisted chain. The prefix map's own
    cases are the inner-build suites', and the fallback reaches them by the
    one dispatch they already pin.

Suite: 0 failed of 5,159 on this commit's build inputs. Overhead not measured;
measured at the milestone's close. DQ-S4 (E2) does not run (DQ-S0's verdict).

### DQ-S5 - the close's measurement, and the stop (2026-11-23)

**Stopped, not closed** (DQ-Q13 (a) as revised): the residue below is a
measured regression against DQ-Q8 (a)'s converged clause and DQ-R6, the
close's measurement budget is spent, and the operator's word either lifts
it for a diagnosis and a remedy or accepts the residue.

On worktree `dq-cabin-miss-build` at `7e31d272` (`v3.5.0-101-g7e31d272b`).
DQ-S4 (E2) did not run (DQ-S0's verdict).

**The measurement** (`ck-tester`, A = `68e5a69f` against B = `7e31d272`,
`build-release`, `bench/v3.5.0/results-dq-close-v3.5.0-101-g7e31d272b.md`):
- **DQ-Q8's gate cell is met at every size.** The Agate arm runs at 0.79x
  to 1.15x of the Cabin dropped, against 1.5x.
- **DQ-S0's cells, against A.** The gate-declined join is 108x to 133x
  cheaper at 10,000 inner rows and 186x to 251x at 100,000; the correlated
  `EXISTS` is 48x to 61x cheaper.
- **At the engine's defaults** (60,000 inner rows, 2,000 outer keys) the
  cabined join answers its first statement in 14.2 ms, where A refused
  both. The second statement is all recording touches and is still
  refused (DQ-Q11).
- **A reads the same as in DQ-S0.**
- **DQ-R6 (M1):**
  - a point read is level;
  - the walked join with the build is 1.3 % to 1.6 % cheaper;
  - the all-served Cabin join and a tuple-key join read +0.2 to +0.5 us
    (0.5 % to 1.2 %) in two readings of 10 to 15 lifetimes. The tuple-key
    join's inner probe takes no branch DQ changed; its compile
    (`BuildAnnotationOf` on the outer walk) and `RunStep` are DQ's changed
    code. DQ-R6 is not shown met.
- **The converged join's residue (DQ-Q13):**
  - the first converged statement of a run whose first statement the
    fallback answered (#3, after the recording statement) reads 10 % to
    35 % dearer in six readings;
  - with a 1.5 s idle gap, every converged statement reads 7 % to 10 %
    dearer.
  - What the three diagnostics found:
    - two thirds follows the fallback's first statement and one third the
      binary;
    - the binary third moves with placement;
    - it is not a periodic task's window.
  - The cause is not isolated.
- **One remedy was tried and withdrawn.** B2 kept `BuiltWalk` and
  `MissWalk` out of line. It removed the binary third of #3 but added 1 %
  to 2 % to whole-relation walks, and left M1's join residue as it was. It
  was reverted, so B is the closing code.
- **Budget:** about an hour of measurement, building excluded, with a quiet
  hold of 0 s. The 100,000-row A cell was cut to 1 round.

**The simulation corpus** at the closing code (release `ckdbs-sim` built from `7e31d272`, `scripts/sim.sh 20`: every committed seed and 20 of the day's fresh seeds): 754 runs, 0 failures.

**The docs moved:**
- `join-inner-build.md` §2, §5, §5a and §8, and `cabin.md` §4a;
- `CLAUDE.md`'s Cabin and inner-build rows;
- the manual's Cabin section, join paragraph and `BUILD OFF`;
- `known-gaps.md`: the busy-instance entry restated, the recording
  statement's refusal (DQ-Q11), and the close's residue (DQ-Q13).

**Sync:** `git fetch origin`. `origin/main` is still `68e5a69f`, the commit
DQ opened from, so the merge brings nothing.


**Review** (`critics-developer`).
- **The records under-reported the residue.** The reviewer corrected it in
  place and recomputed it from the archive:
  - the converged cells are 7 % to 35 % dearer, not 7 % to 23 %;
  - join statements are +0.2 to +0.5 us, not +0.3 to +0.5;
  - DQ-R6 is not shown met;
  - the claim that the tuple-key join runs no changed instruction is false,
    since its outer walk compiles through `BuildAnnotationOf` and its steps
    run through the changed `RunStep`;
  - the results file's header, time window, watchdog account and four
    statements its own data contradict are corrected. Three runs had no
    watchdog of their own; none stalled.
- Applied:
  - **DQ-Q13 revised from closing with the residue to stopping at DQ-S5.**
    Closing had set aside the bar CLA set, and a spent budget is a stop;
  - the two `CLAUDE.md` rows deduplicated;
  - a driver bug fixed (`dq_close.py` kept a block cut short on a busy host
    and pooled it; it now refuses). It did not affect these runs.
- Rejected:
  - de-duplicating `dq_close.py` against `dq_s0_premise.py`, because the
    driver is archived with the results it measured;
  - keying `thread_cpu` by thread id now. That is the diagnosis's first
    step, and the diagnosis waits for the operator's word.

### DQ-S5 - the diagnosis on the lifted budget (2026-11-23)

The operator's word "lift the measurement budget" (`raft-marks-2026-11-23.md`
§5) came with no figure, and CLA read it as one more hour. On worktree
`dq-cabin-miss-build` over `3e615f39`, with the binaries of the close, the
runs took 03:25 to 03:35 UTC, with a quiet hold of 0 s. Results:
`bench/v3.5.0/results-dq-close-v3.5.0-101-g7e31d272b.md`, last section.

- **The compile is level.** `sys.statement_stats`' `compile_us` reads the
  same on A, A2 and B for every M1 shape. The served Cabin join's +0.24 us is
  execution, and the served path's functions are the same size in both
  binaries. It is code placement.
- **Statement #3's excess is placement of two kinds.**
  - The sets the recording statement banks land in the memory the first
    statement's freed map left (74 page faults against 265).
  - The rest belongs to the binary with the fallback off (+100 us).
- **No remedy without a trade.** Keeping the helpers out of line, and
  fixing glibc's mmap threshold, each move the cost onto another shape.
- **DQ stays open at DQ-S5.** CLA proposes accepting the residue, for the
  operator's word. It is not adopted on the standing word, which would set
  aside the bar CLA set.

The driver changes are recorded in the results file: `thread_cpu` keyed by
thread id, `faults`, and the new `tools/dq_compile_probe.py`. No build input
changed, so no suite ran.

