# Work order EH - what the native fold pays for a row: an aggregate over a whole relation

Status: **OPENED 2026-11-28** on worktree `eh-native-fold-per-row`
(branch `worktree-eh-native-fold-per-row`) from `01c6c741c`
(`v3.5.1-35-g01c6c741c`), the close of EG's work branch, which is not
landed; under the operator's standing go-ahead of 2026-11-28
(`raft-marks-2026-11-28.md` §14): every EH-Q item is **adopted on that
go-ahead, never marked by the operator item by item** (§4, "Marks").
Written on worktree
`pg-kds-comparison-orders` (branch `worktree-pg-kds-comparison-orders`)
from `b95be2e86` (`v3.4.17-255-gb95be2e86`) on the operator's word of
2026-11-28 (`raft-marks-2026-11-28.md` §4, §5). No stage is started; EH-S0
waits for the operator's word and for the marks on §4's items. A
`[source-read]` claim with a `path:line` was read at `b95be2e86`. **Two
readers stand behind them**: the lines marked "spot-read" were re-read by
CLA; the rest was read by a research pass of this session and not
re-read.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/aggregate.md` (AG1-AG15); `docs/spec/parser-v2.md`
§7, I19; `docs/spec/sched.md` §3; `docs/spec/index.md` IX7;
`bench/v3.2.0/results-pg-parity-v3.2.0-171-ge81ac7cd.md` (the fold's cost
on equal clients);
`bench/v3.5.0/comparison-pg-kds-scenarios-v3.4.17-166-g6c98f7c32.md`;
`bench/v3.5.0/results-pg-control-scenarios-v3.4.17-202-g481d101aa.md`;
`git show 7f0193bac^:docs/inflight/in-progress/workplan-aggregate-perf.md`
(the closed plan whose AP05 was not built); EF and EG, written the same
day.

## 0. What EH is

`[measured]` Through the scenario drivers, scenario 4's `close` -
`COUNT`, `SUM` and a `GROUP BY` over whole relations - reads 1,011.4 µs
on KDS at `6c98f7c32` against 474.4 µs on PostgreSQL 18.7, 0.47x, the
largest gap in the comparison. The drivers' unequal clients are in that
figure (one of the four statements returns 1,000 rows; EF §1.1), so it
does not size the engine's part.

`[measured]` **The engine's part was sized once, on equal clients**: at
`e81ac7cd`, `agg-global` over 60,480 rows ran at 0.61x of PostgreSQL's
rate on 1.63x its server CPU - 5,776 µs against 3,549 µs, about 95 ns
against 59 ns per row folded - and `agg-by-session` used 1.21x
PostgreSQL's server CPU while leading on the wall clock
(`bench/v3.2.0/results-pg-parity-v3.2.0-171-ge81ac7cd.md:14-22`,
`:68-80`). Of the three leads the comparison gave PostgreSQL, this is the
one the tree supports as the engine's. Nothing since is known to have
moved it, and it is not re-measured on `main`.

`[design]` EH lowers what the native fold pays for a row, **without
changing a result, a refusal or the order groups are emitted in**. S0
measures the premise on `main` and ranks §1.2's costs before anything is
built.

## 1. The tree as read

### 1.1 The path `[source-read]`

- **All four `close` statements are native.** `SELECT COUNT(*) FROM
  board`, `SELECT SUM(qty) FROM board`, `SELECT symbol, COUNT(*) FROM
  board GROUP BY symbol` and `SELECT COUNT(*) FROM tape_10k`
  (`tools/scenario4_cabinopt_days.py:434-443`), three rounds, 12
  operations a phase; the board holds 10,240 rows and 1,000 symbols, the
  tape 10,000 rows. None reaches the query layer, which folds what the
  native parse or compile declines (`HAVING`, an order over aggregates,
  expressions; `aggregate.md` §2).
- **The fold is outside the executor** (AG1, `aggregate.md:17`,
  spot-read): the dispatcher wraps the statement's `RowSink` in an
  `Aggregator` and the compiled chain is byte-identical to the same
  statement without the fold.
- **It runs whole, without a slice.** `RunAggregated` calls the
  synchronous `exec::Execute` with a lambda that calls
  `Aggregator::Accumulate`
  (`src/server/command_dispatcher.cpp:11037-11043`, spot-read). A plain
  `SELECT`'s outermost walk is a `SelectRun` that yields every 64 pages
  (`sched.md` §3); an aggregated one holds its core from its first leaf
  to its last. `sched.md` does not name the aggregate either way
  (spot-read: no line of it mentions one).

### 1.2 What a row costs on that path `[source-read]`, none of it timed

- **Visibility**: `txn::Classify` per row (`src/exec/step_vm.cpp:2970`).
- **A copy of the payload**, whatever the fold reads: the visible row's
  bytes are assigned into a buffer before any decode
  (`src/exec/step_vm.cpp:3005-3006`, spot-read), for `COUNT(*)` as for
  anything else.
- **No scan kernel**: the kernel is compiled from the `WHERE` residual
  and is null without one (`src/exec/step_vm.cpp:2956-2962`, spot-read;
  `:3358-3385`).
- **The decode** of what the statement reads: nothing for `COUNT(*)`,
  the `int64` for `SUM(qty)`, and for the `GROUP BY` the `varchar` key
  copied into a string (`src/exec/row_codec.cpp:559-566`).
- **A `std::function` call per row**: `RowSink`
  (`include/kds/exec/step_vm.hpp:58`, spot-read).
- **The accumulation** (`src/exec/aggregate.cpp:337-379`): without
  `GROUP BY`, one group and no hashing; with it, the key encoded into a
  reused scratch buffer and probed in a `std::unordered_map` keyed by
  `std::string` under `std::hash<std::string_view>`
  (`include/kds/exec/aggregate.hpp:145-150`, spot-read; `:243-244`).
- **Founding a group allocates**: the group's item vector, a copy of its
  key and a map node (`src/exec/aggregate.cpp:188-201`, spot-read;
  `:371-377`), with nothing reserved and `Reset` a `clear()`
  (`:52-64`). The closed plan's AP05 measured about 1 µs a group founded
  in its own section and about 0.5 µs in the plan's last state table,
  both pre-AP02 figures the plan itself calls stale, and was not built.
- **`SUM` over `int64`** is one overflow-checked add
  (`src/exec/aggregate.cpp:281-310`); 128-bit arithmetic is
  `DECIMAL128`'s alone.
- **Bounds**: `aggregate_max_groups`, 65,536 by default (AG11,
  `aggregate.md:28`, spot-read); `sort_max_rows` does not apply.

### 1.3 What the fold does not do, as the specs state it `[source-read]`

- No index-only scan: a `COUNT(*)` walks the relation's leaves
  (`index.md` IX7).
- No parallelism: the statement runs whole on its session's core
  (`aggregate.md` §7).
- No spill and no partial answer: a cap refuses (AG11).
- **Groups are emitted in first-seen order** (AG6, `aggregate.md:23`,
  spot-read), which is a contract the group table must keep.

### 1.4 What was measured `[measured]`, each at its commit

- `e81ac7cd`: §0's figures; the fold adds about 71 ns a row to the walk
  (the research pass's reading of the parity file).
- `6c98f7c32`, this host: `close` 1,011.4 µs on the `off` arm, 773.7 µs
  on `on`, 749.8 µs on `declared`
  (`bench/v3.5.0/interim-scenarios-v3.4.17-166-g6c98f7c32.md:151-153`);
  no file says why the arms differ.
- `9f8f8b354`, this host, the archived JSON as the research pass read
  it (`s4`, `r2-s4` and `cd2-s4`, recomputed by the review of
  `a7dce4487`): over nine `off`
  day-runs `close` has a p50 of 467 µs to 625 µs and a mean of 774 µs to
  1,211 µs, with a maximum of 1,967 µs to 4,222 µs, so its mean is carried
  by a few large operations - which statement they are is not recorded.
- PostgreSQL 18.7, this host: `close` mean 474.3 µs, p50 206.6 µs (the
  median run's percentile row; §0's 474.4 µs is 1,000,000 / its median
  qps, the comparison file's form).
- `9682b725` and `a40882af`: `close` at 0.57x, then 0.67x once the
  client's decoder changed and nothing else did
  (`bench/v3.3.0/results-scenario4-cabinopt-days-v3.3.0-38-ga40882af.md:9`,
  `:346`).
- **Not measured anywhere**: a per-statement split of `close` on either
  engine; the fold's server CPU on `main`; the share of any item of
  §1.2.

## 2. The mechanism `[design]`

Candidates, named and not chosen; S0's ranking picks.

- **(a) A row the fold reads in place.** Where the fold reads no cell
  (`COUNT(*)`) or fixed-width cells only, the payload is not copied: the
  cells are read under the leaf's hold after the row is classified. The
  copy stays wherever a value must outlive the hold.
- **(b) The sink called directly**, not through `std::function`, on the
  aggregated path.
- **(c) The group table**: reserved from the planner's prior where one
  is published, keyed by the encoded bytes in one arena, with the
  first-seen order kept by position as now.
- **(d) The fold given slices** as a `SelectRun` has them. This is a
  cost to the fold and a gain to every other session on its core; it is
  EH-Q2's.

AG1 says the chain is byte-identical with and without the fold. (a) and
(b) change how a row reaches the fold, not the chain; whether that is
inside AG1 as written is EH-Q3's.

## 3. Rules

- **EH-R1. Every answer is byte-identical**, groups in first-seen order
  (AG6): the aggregate contract suite stays green, one test per numbered
  item, and the PostgreSQL differential (`scripts/bs/difftest.py`) is
  run over the folds. **The waystone and cabin contract suites stay
  green too**: a change to how a row reaches the fold is a change beside
  the recording and the probes AG10 calls unchanged.
- **EH-R2. Visibility is decided per row at the statement's snapshot,
  before any cell is read.** A fold that counts a version its snapshot
  does not see, or skips one it does, is `[quiet-wrong]`.
- **EH-R3. A cell read in place is read under the page's pin and latch
  and never after** (invariant 10); nothing of a row is held by pointer
  across a leaf. A value read after its hold is `[quiet-wrong]`.
- **EH-R4. Every refusal stays where it is**: a `SUM` overflow on the
  row that overflows, a cap at the group that exceeds it, a decode's
  `Corruption` on the row that carries it.
- **EH-R5. The unaggregated path is not made slower**: a point `SELECT`
  and a walked `SELECT` are measured interleaved at each stage's end.
- **EH-R6. Measured in server CPU, through equal clients**
  (`kwp.Connection.query`), interleaved A/B in `build-release`, the
  server pinned.
- **EH-R7. A fold that yields owns its accumulator.** The dispatcher has
  one `Aggregator`, reused across statements and pointed at each
  statement's spec by `Reset`
  (`include/kds/server/command_dispatcher.hpp:3292-3300`, spot-read); it
  is safe today because a fold never gives its core back. Once one does
  (EH-Q2 (a)), another session's aggregate on that core would reset it
  mid-fold: `[quiet-wrong]`. The stage that adds the slice gives each
  running fold its own state and measures what that costs a point
  statement (about 4 µs when built per statement, by that comment).
- The standing rules, as DT's order lists them.

## 4. Items for the operator (EH-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| EH-Q0 | Is EH opened, and with what scope? | (a) S0, then the candidates S0 ranks, one a stage; (b) S0 alone | (a) |
| EH-Q1 | The bar | (a) a global fold's server CPU per row at or below PostgreSQL's on S0's host, each shape stated, and EH closes on what was measured whether or not the bar is met; (b) a fixed cut of S0's figure; (c) no bar | (a) |
| EH-Q2 | Does an aggregated walk yield? | (a) inside EH as its last stage, after the per-row work, with its cost to the fold measured and a cancel taken at its slice boundary as a `SelectRun`'s is; (b) a separate order; (c) not built, and `sched.md` §3 says an aggregate holds its core | (a) |
| EH-Q3 | AG1 | (a) read as "the chain's steps and its row stream are identical": a fold-aware row delivery is inside it, and `aggregate.md` says so; (b) AG1 kept to the letter, which leaves (c) of §2 alone | (a) |
| EH-Q4 | `COUNT(*)` from a structure's counts | (a) not built: a count that is not per row at the snapshot is not the answer; (b) considered | (a) |
| EH-Q5 | The query layer's fold | (a) outside EH; (b) inside | (a) |
| EH-Q6 | Scenario 4's `close` | (a) the driver times each of its four statements as its own phase from S0 on, on EF's equal client if EF-S1 has landed and on `kwp.Connection.query` directly if not; (b) left as one phase | (a) |

### Marks

**Every row is CLA's proposal adopted on the operator's standing go-ahead
of 2026-11-28 (go-ahead-achieving-milestone)** (`raft-marks-2026-11-28.md`
§10, §14), and none is the operator's own mark on the item. No EH-Q item
is tagged `[quiet-wrong]` itself; three rules are (EH-R2, EH-R3, EH-R7),
and the premise of each was re-read against the tree at `01c6c741c`
before the opening: visibility is decided per row by `txn::Classify`
under the leaf's hold (`src/exec/step_vm.cpp`), a cell is read today only
from the copied payload, and the dispatcher has one `Aggregator` reused
across statements (`include/kds/server/command_dispatcher.hpp`).

| | adopted | note |
|---|---|---|
| EH-Q0 | (a) | S0, then the candidates S0 ranks, one a stage |
| EH-Q1 | (a) | the bar: a global fold's server CPU a row at or below PostgreSQL's on S0's host; EH closes on what was measured either way |
| EH-Q2 | (a) | the slices as EH's last stage, under EH-R7 |
| EH-Q3 | (a) | AG1 read as "the chain's steps and its row stream are identical" |
| EH-Q4 | (a) | no `COUNT(*)` from a structure's counts |
| EH-Q5 | (a) | the query layer's fold outside EH |
| EH-Q6 | (a) | S0 measures the fold shape by shape through counting clients; scenario 4's `close` split is left to the close |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **EH-S0** | The premise on `main`: a global `COUNT(*)`, a global `SUM` and a `GROUP BY` at 1,000 and at 60,000 groups' worth of rows, server CPU per row beside PostgreSQL's in one window; a profile that ranks §1.2 | the ranking in §7; EH-Q1's figure |
| **EH-S1..** | One candidate a stage, in S0's order | each stage's server CPU per row; the suite; EH-R5's pair |
| **EH-Sy** | The slices (EH-Q2 (a)) | a fold that yields and is cancelled at a boundary; its cost |
| **EH-Sz** | The close: `aggregate.md`, `sched.md` §3, `known-gaps.md`, the overhead measurement | the operator's word on landing |

## 6. What EH does not do

- No parallel fold, no batch or vector executor, no SIMD kernel.
- No index-only scan (IX7), no stored or incremental aggregate.
- No native `HAVING`, no order over aggregates, no expression in a
  grouping key.
- No spill: AG11's caps refuse as now.
- Nothing of a read's fixed cost (EF) or a commit's (EG).

## 7. Stage log (CLA, appended as stages land)

### EH-S2 (2026-11-28): the group table, on `eh-native-fold-per-row` from `b36deb576`

- **The change** (§2's (c), less the reserve): `Aggregator`'s
  `std::unordered_map<std::string, size_t>` is replaced by an
  open-addressing table of `(hash, group + 1)` slots over one arena of key
  bytes, kept at most half full; founding a group appends its key to the
  arena and fills one slot, so no map node and no key string is allocated a
  group, and the table allocates only when it doubles. The hash is a 64-bit
  multiply-and-shift over the encoded key eight bytes a step, replacing
  `std::hash<std::string_view>` (11 % of a 7,560-group fold at EH-S0); a
  probe compares the hash and then the bytes, so no answer depends on its
  quality. `Accumulate` and `Merge` find and file groups through the same two
  helpers. **The emit order is untouched** (AG6): groups stay in `groups_`
  in the order they were founded, and nothing walks the table. **Not built**:
  the reserve from the planner's prior - it needs a prior plumbed to the
  aggregator for a saving the doubling already amortises.
- **Cells**: the aggregate contract suite and every cell naming an
  aggregate, `GROUP BY` or `DISTINCT`, 211 of 211, unchanged - they pin
  first-seen order, the cap, merge and every grouped answer.
- **The suite at this tree**: 5,433 of 5,435 (`ctest -LE bs-nightly -j4`,
  219.3 s). The two failures are the host's two-core flakes
  (`ContentionCountersTest.TwoCoresInsertingIntoOneRelationContendWhatAnInsertTakes`,
  `DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`); run 20 times
  each, interleaved with EG's build of the same window, which has no
  aggregate change, they failed 3 and 6 times here against 5 and 8 there.
  The suite run before it was **voided**: CLA rebuilt the test binary while
  it ran.
- Overhead not measured; measured at the milestone's close.

### EH-S1 (2026-11-28): a global fold from the row's bytes, on `eh-native-fold-per-row` from `1f7ef7c84`

- **The change.** `exec/bytes_fold.hpp` is the seam: a consumer the
  executor may hand a visible version's payload under the leaf's hold.
  `ChainRunner::AcceptTupleAt` hands it, in the `kVisible` arm, when the
  statement has a fold, the chain is one step that keeps every visible row
  (`KeepsEveryVisibleRow`: no conjunct of any kind, no sub-chain, no index
  order, no trail-replayable step; no Cabin recording live, tested per row),
  the fold admitted the relation at the step's first visible row - after
  BM-R6's checks, as the kernel is armed - and the payload is the layout's
  length. The row is examined, charged and matched in the full path's
  order. `Aggregator` is the consumer: `PrepareBytesFold` admits a global
  fold with no `DISTINCT` whose arguments are columns of the step's
  relation other than `varchar`, and `AccumulateBytes` decodes those cells
  with `DecodeColumnsInto` into its own slots and folds each through
  `FoldValue`, the per-value body `FoldInto` now calls too. The dispatcher
  passes its aggregator to `exec::Execute`. §2's (a) and (b) together: no
  copy, no frame decode, no `std::function` call, for that shape.
- **The `[quiet-wrong]` rules, read against the code as built**:
  EH-R2 - the arm is inside `txn::Classify`'s `kVisible` case; a
  `kNeedsUndoWalk` version takes the copied path and the sink. EH-R3 -
  `AccumulateBytes` decodes into the aggregator's own slots inside the call
  and keeps no span; no `varchar` is admitted, so no spill is pending and no
  fetch happens under the hold. EH-R7 does not arise: nothing yields.
- **Cells.** In `scan_row_walk_test.cpp`, the executor's contract with a
  consumer of the test's own: every row but the first is handed over and
  counted as the sink's would be; a conjunct, a second step or a declining
  fold hands nothing; the row-touch budget refuses on the same row with the
  same words; a fold's refusal ends the statement on its row. In the new
  `bytes_fold_equivalence_test.cpp`, the fold's contract (EH-R1, EH-R4):
  42 select lists over ten column types - every fixed-width type, `char`,
  `varchar` and `uint64` among them - answer byte for byte the same bare and
  behind a conjunct that keeps every row, empty, at one row, over 700 rows
  with every third row NULL, after deletes and updates, over a column of
  NULLs, beside an undecided writer's update and delete and after its
  commit, and with an overflowing `SUM` refused in the same words.
- **The suite**: 5,431 of 5,432 (`ctest -LE bs-nightly -j4`); the failure
  is `SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement`,
  the two-core rig `known-gaps.md` names as intermittent here (3 of 20
  alone at this tree), which folds nothing.
- **`docs/spec/aggregate.md` §1** states the bytes path and what AG1 still
  promises (EH-Q3 (a)).
- **After the review** (`6e55671ef`, `e2f13279d`, and this commit): **one
  defect, fixed by the reviewer** - a step whose decode is every column
  (`read_columns` is `kAllColumns`: a chain with a hoisted sub-chain, a
  relation over 64 columns) refuses a corrupt `varchar` cell on the frame's
  path even where the statement reads no such column, and the bytes path
  answered instead (EH-R4); the fold is no longer armed there, and a cell
  pins it by mutation. Two cells added: the remaining fixed-width types
  (`int8`, `bool`, `decimal(30,4)`, `uint64` above `INT64_MAX`) and a
  writer's own uncommitted rows and a `REPEATABLE READ` view across a
  commit, both paths. The arm returns from inside the span's scope. **Not
  taken from the review**: `ANALYZE` passing the fold - its sink counts and
  caps the rows it reports, so a fold that bypassed it would change what
  `ANALYZE` prints; `aggregate.md` says `ANALYZE` runs the frame's path.
  The shared item loop between `FoldInto` and `AccumulateBytes` (four lines
  twice, against a template for both) and the probably-dead clauses
  (`!recording_here`, the trail test), kept as the cheap guard each is.
- **Measured** (`bench/v3.5.1/results-eh-s1-in-place-v3.5.1-41-g6142f32f1.md`,
  interleaved against the engine EH opened from): a global fold of four
  aggregates is 29.1 ns of server CPU a row against 38.8 ns before and
  PostgreSQL's 28.0 ns; `COUNT(*)` 11.4 ns against 19.2 ns and 12.1 ns.
  **EH-Q1's bar is met within the spread** (1.04x on PostgreSQL's 5.1 %).
  Every grouped shape is inside its spread of the old binary.
- Overhead not measured; measured at the milestone's close.

### EH-S0 (2026-11-28): the premise and the ranking, on `eh-native-fold-per-row` from `01c6c741c`

`[measured]` on `main`'s engine (EG's closing binary; no engine source
differs), counting clients on both sides,
`bench/v3.5.1/results-eh-s0-premise-v3.5.1-35-g01c6c741c.md`:

- **The premise holds on `main`**: a global fold of four aggregates is
  39.0 ns of server CPU a row against PostgreSQL 18.7's 28.0 ns (1.39x;
  0.71x of its rate). EH-Q1's figure is 28.0 ns a row for that shape and
  12.1 ns for a bare `COUNT(*)`.
- **The ranking, by subtraction between eleven shapes and by 150 stack
  samples a fold**:
  1. **the path from a visible row to the fold**: a rejected row costs
     10.9 ns (PostgreSQL 17.5 ns), a counted one 19.2 ns (12.1 ns) - the
     copy, the general row's checks, the `std::function`, the fold's loop;
  2. **decode and fold of a fixed-width cell**: 8.4 ns for the first
     cell, 19.8 ns for four aggregates over two; `FoldInto` 34.5 % and
     `DecodeAndResolve` 30.3 % of a global fold's samples;
  3. **founding a group**: about 230 ns against about 160 ns, in the
     `std::string`-keyed map and the allocator (52 % of a 7,560-group
     fold's samples between them);
  4. the probe is no deficit (23.5 ns against 26.2 ns a row).
- **So S1 is §2's (a) and (b) together, for the fold the ranking's first
  two items decide**: a global fold with no predicate, accumulated from
  the visible row's bytes under the leaf's hold. §2's (c), the group
  table, is S2's if S1 lands.
- **No instruction-level profile**: the host has neither `perf` nor
  `valgrind`.
- The suite was not executed: no build input changed.

### Written 2026-11-28

On `pg-kds-comparison-orders` from `b95be2e86`, on the operator's word.
No stage is opened and no code is written. The 0.47x EH was asked for is
the scenario drivers' figure and carries their unequal clients; the
order's premise is the equal-client measurement at `e81ac7cd`, which S0
repeats on `main`.
