# Work order CF — Waystone's premise: measure, then remove, default off, or opt in

Written 2026-10-21 on `cf-waystone-premise` from `23ea3e61`
(`v3.3.0-81-g23ea3e61`), on the operator's words, recorded in English
(`raft-marks-2026-10-21.md` §2):

- **W0**: *"Then what about turning Waystone off by default and having
  the DBA create it explicitly?"* - after CLA's direction analysis of the
  same day, which found the engine's two stated mechanisms (physical
  optimization, Waystone) hollowed in the built tree, and CLA's reading of
  when a trail is recorded (§1.1).
- **CLA's answer to W0, the order this work order follows**: (1) measure
  first, one stage inside 20 minutes, replay on against off on tuple-key
  join chains and repeated point reads; (2) if replay does not pay, remove
  it, or at the least turn it off by default; (3) only if it pays on
  particular shapes, consider an opt-in that adds no second model of a
  pattern.
- **W1**: *"For now, write a milestone in this order and push it, as an
  exploratory milestone."*
- **W2** (2026-10-21): *"Open with CF-Q0..Q7 $!a and proceed with S0"*
  (`raft-marks-2026-10-21.md` §3).

**Written 2026-10-21; opened 2026-10-21 on W2.** CF-Q0..Q5 are the
operator's marks, each (a) - CF-Q1 (a) against CLA's proposal (b) - and
CF-Q6 and CF-Q7 carry no lettered option, so W2 leaves them as they
stand (§4). **Exploratory since W1** (§3): CF-S0 decides which of
three branches runs, and a branch that is not taken is recorded, not
built.

**Claim tags**: `[operator]` for W0-W1; `[source-read]` with `path:line`
at `23ea3e61`; `[measured]` with its results file; `[design]` for
reasoning about code not yet written; `[quiet-wrong]` where a wrong choice
turns a correct answer into a wrong one with no error.

**The letter.** `[source-read]` CE is the last work order under
`instructions/v3.2.0/` on `origin/main` at `23ea3e61`, no branch or
worktree in `git worktree list` names a `CF`, and no commit subject on
`origin/main` carries the token. CLA's choice.

**CF against other orders.** CF re-scopes or removes Waystone
(`docs/spec/waystone-concpets.md`); what it leaves standing is §2.1's
"does not reach" list and §6. CA
(`workorder-ca-background-advisory-optimizer.md`) priced the advisory
work as a whole and is not reopened by CF. Not part of AR0 §8's chain.

---

## 0. What CF is

CLAUDE.md names Waystone as one of the engine's two mechanisms. The
built tree records a trail for a narrow class of statements (§1.1), no
measurement since v3 has priced a replay while one priced recording at
7-9 % of a point read (§1.2), and keeping it carries a set of standing
gaps (§1.3). CF settles which of
three things Waystone becomes:

- **Branch R - removed** (§2.1; CLA's proposal if S0 finds no gain).
- **Branch D - off by default** (§2.2).
- **Branch O - opt-in per pattern**, only if S0 finds a gain on
  particular shapes (§2.3).

---

## 1. The tree as read `[source-read]` at `23ea3e61`

### 1.1 When a trail is recorded

A trail is written only when every condition below holds:

1. **The switch.** `waystone_recording` (default `true`,
   `include/kds/server/core_runtime.hpp:165`, `expeditor.hpp:266`) builds
   a core's `TrailRecorder`; `waystone_replay` (`:166`, `:273`) reads
   trails. Both are boot-only config keys (`src/server/expeditor.cpp:358`,
   `:383`).
2. **A native `SELECT` that succeeded.** `RecordExecution` is called from
   the three SELECT exits only - `SelectRun`'s finish, `RunAggregated` and
   `RunAnalyze` (`src/server/command_dispatcher.cpp:5245`, `:7306`,
   `:7404`). No `INSERT`, `UPDATE` or `DELETE` records; a statement the
   query layer answers records nothing (`parser-v2.md` I19); a failed
   statement records nothing (`trail_recorder.hpp`, `OnPatternResult`).
3. **A lookup-class step.** Only `kLookup` and `kProbe` are recordable
   (`include/kds/exec/step_chain.hpp:171`); a chain with neither skips
   the Waystone lookup entirely (`waystone_usable`,
   `command_dispatcher.cpp:7643`). A built inner side feeds nothing
   (`join-inner-build.md`).
4. **Not D2.** A statement with a row-volatile function neither records
   nor replays (`command_dispatcher.cpp:7651`).
5. **The second sighting.** `kAutoRecordThreshold = 2`
   (`include/kds/stats/trail_recorder.hpp:70`), counted per instance
   `(fetch_id, arg_hash)` in a core-local table of `kMaxSightings = 4096`
   (`:67`) cleared wholesale on overflow. A pattern's `sys.patterns` row
   is registered at the same moment. **Every execution from the second
   re-records**: it walks the directory and reads the trail page, and
   writes it exclusive only when the entries changed
   (`src/stats/trail_store.cpp:105-108`).
6. **One page.** A collector that overflowed 253 entries records nothing
   (`src/stats/trail_recorder.cpp:106`); an empty one never reaches the
   recorder and is not counted as a sighting
   (`command_dispatcher.cpp:8040`).
7. **Only rows that passed the step's conjuncts** become entries
   (`src/exec/step_vm.cpp:2562`).

So a trail exists for a statement with a tuple-key lookup or probe step -
a point read, a tuple-key join chain, or a scan that probes by tuple key,
whose probe rows alone are recorded - touching at most 253 such rows,
repeated with the same arguments at least twice on one core since that
core's table last cleared, since the last mount; it is replayed from the
third execution on.

### 1.2 What it has been measured to buy `[measured]`

- **Before v3**: a replay was 3-7 % slower than a descent on a B+ tree
  relation (`waystone-concpets.md` §7, the bench tree at `1769487`).
- **BN-S1's census**
  (`bench/v3.0.0/results-bn-s1-census-v2.7.0-769-gb70f79d8.md:116`):
  recording and the replay attempt are 7-9 % of a point `SELECT` by tuple
  key's server CPU (`known-gaps.md:992`) - a cost, with no gain measured
  beside it.
- **CA-S0** (`bench/v3.2.0/results-ca-s0-premise-v3.2.0-152-ga9f0f5cd.md`):
  Waystone, access-statistics and optimizer-signal code is 0.2-0.6 % of
  sampled server CPU in scenarios 1-4, a lower bound (leaf functions only).
  With `waystone_recording`, `waystone_replay` and `access_statistics` all
  off, scenario 1's walked shapes ran 3.4-5.4 % slower and no other
  headline moved outside noise. **The file itself says it cannot separate
  the arm from its position**: the off arm always ran second in its pair.
  The three switches moved together, so replay was not isolated, and no
  cell checked that replay was exercised (`ANALYZE`'s `replays=`,
  `src/exec/plan_printer.cpp:460`).
- **None of those shapes could have replayed** (`[source-read]`,
  `tools/scenario1_backtest.py`): `read-day-slice` and `agg-day-slice` are
  a `FilterScan` with no lookup-class step (:1193-1199, :1316-1321; §1.1
  item 3); `backtest-read` reads each rebalance session once, its first
  sighting (:996-998); and `backtest-replay` at the driver's default
  `--replay 1` (:2027, which CA-S0's arguments did not override) is each
  session's second sighting, which records and does not replay (§1.1
  item 5). So CA-S0's scenario-1 movement is position or noise, not
  replay, and no measurement since v3 has priced a replay.
- **CA-S0's planned cells 1 and 2** - `select-pk` and a three-relation
  tuple-key join chain, each on against off
  (`workorder-ca-background-advisory-optimizer.md` §5) - were replaced by
  the scenario harness (§5a there) and never run as written.

### 1.3 What it costs to keep `[source-read]`

Standing gaps, by `docs/inflight/known-gaps.md` line at `23ea3e61`:

- **Every restart leaks the previous run's Waystone pages** (`:747`):
  trails are unlogged, every mount clears the roots
  (`server::DiscardAdvisoryState`, `src/server/restore.cpp:311`), and the
  pages they reached stay allocated.
- **Every mount dirties every `sys.patterns` page** (`:1403`), and
  **registering a pattern scans the whole of `sys.patterns`** (`:1390`);
  the relation has no cap.
- **Two cores recording one pattern can strand a trail and leak a page**
  (`:391`); **a directory cannot deepen** (`:407`).
- **The recording itself runs on the statement** (`:1341`): a pattern's
  first recording registers its row and claims a directory through the
  WAL, and every execution of an instance from its second walks the
  directory and reads its trail page, writing it exclusive when the
  entries changed (§1.1 item 5).
- **A contract suite** (`tests/waystone_contract_test.cpp`) and four more
  `waystone_*` test files are kept green for it (CLAUDE.md, Working
  Rules).

### 1.4 What was withdrawn before

`CREATE PATTERN` - a named, declared pattern with stored source text in
`sys.pattern_defs` - was withdrawn on 2026-08-31 and its code removed
(`docs/spec/create-pattern-user-defined-patterns-v1.md` §1): a pattern is
one fingerprint-identified concept, and the declaration was a second
model of it. `sys.patterns` keeps `flags` and `origin` on disk, written
`kOriginAuto` and never `kPatternPinned`.

---

## 2. The three branches `[design]`

### 2.1 Branch R - removed

What it reaches, each an item:

- `src/stats/trail_recorder.cpp`, `trail_store.cpp`, `waystone_dir.cpp`,
  `waystone_page.cpp` and their headers, with `include/kds/stats/waystone.hpp`.
- `include/kds/exec/trail_collector.hpp`, `trail_replay.hpp` and
  `src/exec/trail_replay.cpp`; the collector and replay arms of
  `StepVm` (`step_vm.cpp:2562`) and of `SelectRun`; `ExecStats::trail_replays`
  and `ANALYZE`'s `replays=`.
- `Catalog::RegisterPattern`, `FindPattern`, `GetSysPatternRow`,
  `ListPatterns`, `ClaimPatternWaystoneRoot`, `MutatePatternRow`,
  `ClearEveryPatternWaystoneRoot`, the catalog cache's pattern entries,
  and `DiscardAdvisoryState`'s trail half (`src/server/restore.cpp:311`).
- `include/kds/stats/instance_key.hpp` (only Waystone keys by it), the
  `waystone_usable` arm of the identity guard
  (`command_dispatcher.cpp:7661`) and `StatementIdentity`'s `trail` field,
  which `ANALYZE` reads `fetch_id` from.
- `sys.patterns` (CF-Q3), `PageType::kWaystone`, and the superblock
  version (bumped; every older volume refused, the standing order of
  2026-10-07).
- The headerless-page facility (`PageStore::CreateNewHeaderless`, the
  `kHeaderlessMap` bitmaps) loses its only user: the directory's interior
  pages are the one headerless class (`page.md` §1), and undo pages are
  forbidden it (`include/kds/txn/undo_log.hpp:49`). Whether it goes with
  them is not decided here.
- `waystone_recording` and `waystone_replay` refused at boot by name, as
  `physical_optimizer` is (`src/server/expeditor.cpp:198`) (CF-Q4).
- `tests/waystone_*_test.cpp`, `tests/trail_recorder_test.cpp`,
  `tests/trail_store_test.cpp`, and the Waystone cells in other test files;
  the byte-for-byte contract suites drop `waystone` (CLAUDE.md, Working
  Rules). The simulation harness's `waystone` toggle (`sim/loop.hpp:102`,
  `sim/instance.hpp:90`, `sim/sim_main.cpp`, the minimizer's plan encoding
  in `sim/minimize.cpp`), and `tools/bm_waystone_control.py`.
- `docs/spec/waystone-concpets.md` and `pattern-tracking-levels.md`
  deleted; `heap-and-tuple.md` §3.1a, §8 and the invariants re-worded
  (CF-Q5); `parser-v2.md` J5 and I17; `page.md` §1, `wal.md` and every
  other spec, manual page, `README.md` and `kds.conf.sample` line naming
  Waystone; CLAUDE.md's "What This Project Is" item 2, its Waystone row,
  invariants 8 and 9 and the rows that mention trails (CF-Q6).

What it does not reach: the fingerprint, `fetch_id` and `arg_hash` (CC's
order memo keys by it, `src/exec/step_compiler.cpp:2222`), the
informational `pattern_id` a KWP PARSE answers
(`src/server/kwp_session.cpp:546-553`), `ANALYZE`'s `pattern_id` and
`fetch_id` line (`command_dispatcher.cpp:7437`), the page epoch,
`tuple_verify`, the Cabin's location hint.

### 2.2 Branch D - off by default

The two keys default to `false` in `core_runtime.hpp` and `expeditor.hpp`;
`kds.conf.sample` and `manual/server/` say so; the contract suite keeps
running both arms. No other code moves. The costs of §1.3 stay for an
operator who turns it on.

### 2.3 Branch O - opt-in per pattern

Admissible only if S0 finds a gain on particular shapes, and only in a
form that is not a second model of a pattern (§1.4). The shape CLA would
propose is a flag on the pattern's existing identity - for example a bit
of `sys.patterns.flags` set by an admin statement naming a `fetch_id` -
with no name, no stored source text and no new relation.
`docs/spec/pattern-tracking-levels.md` already holds an unbuilt per-pattern
design of this kind (four tracking levels, selected only by the withdrawn
`CREATE PATTERN`), and an amendment starts from it. It leaves two
things CF-Q7 has to answer: with recording off no `sys.patterns` row is
registered, so the statement needs another source for the `fetch_id`
(`sys.statement_stats` holds one); and an opted-in pattern still loses
every trail at each mount (§1.3).

---

## 3. The exploratory method (W1) `[design]`

- **CF-S0 decides the branch.** It runs once CF is opened and CF-Q1 and
  CF-Q2 are marked, since its cells and its bar rest on them; it is the
  only stage that runs before the branch is marked (CF-Q0). Its results
  file says which branch the numbers point to and why.
- **A branch is built as its own commits on the work branch**, reviewed
  by `critics-developer`, gated by the full suite.
- **A branch not taken is recorded in §7 with the numbers that sank it**,
  never built.
- **Nothing merges to `main` without the operator's word**, and the
  positioning question (CF-Q6) is the operator's alone.

---

## 4. Questions - marked on W2

**Marks (W2, 2026-10-21)**: CF-Q0 (a), CF-Q1 (a), CF-Q2 (a), CF-Q3 (a),
CF-Q4 (a), CF-Q5 (a), each the operator's. CF-Q6 has no lettered option
and stays the operator's open question; CF-Q7 has none either and is
asked only if S0 points at Branch O. The questions as written follow.

**CF-Q0 - who picks the branch after S0.**
- (a) The operator marks it after S0's report.
- (b) S0's bar (CF-Q1) picks it, and the branch runs on the standing
  go-ahead.

**CLA proposes (a)**: Branch R changes what the project says it is
(CF-Q6), which a bar should not decide.

**CF-Q1 - the bar replay must clear.**
- (a) On at least one tuple-key chain cell, both conditions hold, each by
  more than the larger of the two arms' spreads, in throughput and in
  server CPU per statement, with `replays=` non-zero in the on arm, and
  no cell regresses outside its spread:
  - **replay's gain**: replay on beats replay off, recording on in both;
  - **the net**: both on beats both off, so recording's cost (§1.2's
    7-9 %, and §1.1 item 5's re-recording) is paid for.
- (b) (a), and the net gain at least 5 % of server CPU per statement.

**CLA proposes (b)**: a gain inside 5 % does not pay for §1.3. Branch R
or D follows from the net failing, whatever replay's gain alone shows.

**CF-Q2 - S0's arms.**
- (a) Three: both on; recording on, replay off; both off. Rotating order
  (ABC, BCA, CAB) so position is not confounded with arm.
- (b) Two: both on against both off, as CA-S0.

**CLA proposes (a)**: (b) is the confound §1.2 names.

**CF-Q3 - `sys.patterns` under Branch R.**
- (a) Deleted with its page, its view and `SHOW PATTERNS`; nothing else
  reads it as a store.
- (b) Kept as a view over `sys.statement_stats`.

**CLA proposes (a)**, with S1's source-read confirming that no reader
outside Waystone remains (the cabin optimizer counts by `pattern_id`
through its own collector).

**CF-Q4 - the config keys under Branch R.**
- (a) Refused at boot by name, as `physical_optimizer` is.
- (b) Accepted and ignored.

**CLA proposes (a)**: (b) accepts a spelling and enforces nothing
(CLAUDE.md, Working Rules).

**CF-Q5 - invariants 8 and 9 under Branch R.**
- (a) Kept, re-worded for "an advisory location structure", because the
  Cabin's location hint is one (`cabin.md` C6) and depends on the same
  epoch and verify discipline.
- (b) Deleted with Waystone, the Cabin hint's rules moved into `cabin.md`.

**CLA proposes (a)**: the discipline has a live subject.

**CF-Q6 - the project's stated purpose** `[operator]`. Under Branch R
(or D), CLAUDE.md's "What This Project Is" item 2 no longer describes
the engine. The operator's word decides what replaces it. CLA proposes
nothing here.

**CF-Q7 - Branch O's target identity**, asked only if S0 points at O:
where an opt-in names its pattern from, and whether an opted-in trail
is logged so it survives a mount.

---

## 5. Stages

The overhead is measured once, at the close, inside the budget of
2026-10-13 (about an hour, building included). Every stage is reviewed by
`critics-developer` and gated by the full suite.

**CF-S0 - the premise** (`build-release`, inside 20 minutes, results at
`bench/v3.3.0/results-cf-s0-premise-<describe>.md`). No engine code
moves. Arms per CF-Q2. Cells:

1. A three-relation tuple-key join chain (`kLookup` then two `kProbe`),
   a working set of instances each executed many times, under 4,096
   instances per core so the sighting table never clears (§1.1 item 5).
2. The same at four relations.
3. A repeated `select-pk` by tuple key, the same working-set bound.
4. Scenario 1's `backtest-replay` (its BTREE copy) at `--replay 2` or
   more, so the passes after the first replay (§1.2); `backtest-read`
   cannot replay and is not a cell.
5. A position control: scenario 1's `read-day-slice`, which cannot
   replay (§1.2), so any arm difference it shows is position or noise.

Each cell reports throughput and server CPU per statement (the server
process's CPU time over the statements it ran); the sampler
(`tools/kds_sampler.c`) gives the CPU split where a cell needs one.
`ANALYZE`'s `replays=` is read on one statement of the shape in the on
arm, and a cell whose on arm shows `replays=0` is void, not a result.
The first two passes of every cell are warm-up and excluded. The file
states `cores`, the connection count (sightings are per core, trails the
instance's) and every cut made to fit the budget.

**CF-S1 - the branch the operator marks** (CF-Q0).
- R: §2.1, in two commits - the code and the format (superblock bump),
  then the documents.
- D: §2.2, one commit.
- O: written as an amendment to this order first (CF-Q7), then built.

**CF-S2 - the close.** The close measurement, the order's stage log,
`known-gaps.md` (§1.3's entries struck or restated), the owning specs,
and CLAUDE.md's rows.

---

## 6. Out of scope

The Cabin and its controller, the page epoch, the fingerprint, CC's plan
statistics, `sys.access_stats`, and any new advisory structure. CA's
route and base-fetch hint (CA §8) stay a design with no measurement
asking for them.

---

## 7. Stage log

**Opened 2026-10-21 on W2**; CF-S0 started on the same word.
