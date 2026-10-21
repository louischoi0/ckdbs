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
- **W3** (2026-10-21): *"Proceed with $!O and push no verify"* - the
  branch, after CF-S0's report (`raft-marks-2026-10-21.md` §4).

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

**CF-S0: the premise, measured**
(`bench/v3.3.0/results-cf-s0-premise-v3.3.0-97-g3d628df4.md`; harness
`bench/v3.3.0/archive/cf-harness/`).

- **How it ran.** Three arms (CF-Q2 (a)) in rotating order, 6 reps each,
  `cores = 1`, `build-release`, at engine source `a1809988`.
  - Every cell's reply digest was identical across the 18 runs.
  - Arm A's `replays=` was non-zero on every recordable cell and zero in
    B and C. This is the first v3 measurement to show that a replay ran.
- **Point reads and tuple-key chains** (`pk`, `chain3`, `chain4`):
  - Replay's gain is nil, -0.2 % to -1.1 %, inside the spread.
  - The default (A) against both off (C) costs 2.7-3.9 % of server CPU
    per statement, outside the spread on `chain3` and `pk`.
  - The recording is that cost: about 1.2-1.5 us per statement.
- **A scan-driven chain with 200 tuple-key probes** (`scanprobe`,
  scenario 1's cross-section shape, reproduced synthetically):
  - Replay pays: +5.7 % throughput and -7.2 % server CPU against B.
  - The net against C is +5.0 % and -6.4 %, outside the spread.
  - Recording costs it nothing measurable.
- **The rotated position control** moved 0.1-0.6 %, inside its spread.
- **Against CF-Q1 (a): not met.**
  - Its two conditions hold on `scanprobe` alone, and on no `pk`,
    `chain3` or `chain4` cell. Whether `scanprobe` is "a tuple-key
    chain cell" is a reading: §1.1 names it a scan that probes by tuple
    key, a different shape from a tuple-key join chain.
  - Its "no cell regresses" clause fails: arm A regresses against C on
    `chain3` and `pk`, outside the spread.
  - So the bar does not support the present default and does not pick a
    branch. Replay's gain on `scanprobe` meets §2.3's condition for
    Branch O, if the operator counts that shape; Branch D keeps the gain
    only instance-wide, with the point-read cost back.
- **Cuts.** Scenario 1's driver was not run; its shapes were reproduced
  on synthetic relations. There was no size sweep, no cold pool and no
  `cores > 1`.
- **What happens next.** The branch is the operator's (CF-Q0 (a)).

**Branch O marked on W3** (`raft-marks-2026-10-21.md` §4). Branches R
and D are not taken; S0's numbers above are what sank them. CF-S1 opens
with Branch O's amendment to this order (§5, CF-Q7).

**CF-S1a: Branch O's amendment written** (§8). The amendment went through
three steps:
- **Drafting.** A workflow read the tree through four lenses, wrote three
  independent designs, and had three judges score them. The design
  ranked first was by-example opt-in on the fingerprint's own row; the
  minimal-surface design came second.
- **First verification.** The synthesized draft was checked adversarially
  and drew 38 findings (none fatal, 9 wrong). Every one was applied,
  eight of them by recorded CLA decisions. The most consequential:
  - superblock 33, as decided by the standing order;
  - the second-model test restated honestly;
  - the mount's cache drop `[quiet-wrong]`;
  - an instance-wide opted count.
- **Second review.** A second review drew 20 more (none fatal, 6 wrong),
  all applied. The record is in §8.7.

It is documents only, so no suite ran. CF-O-Q1..Q14 wait for the
operator's marks, and no code moves before them.

**W5** (*"push no verify and go ahead"*, `raft-marks-2026-10-21.md` §8):
the amendment was pushed, and CF-O-Q1..Q14 were adopted, each (a), on the
go-ahead. **CF-S1b started.**

**CF-S1b: storage, the statement, the mount and superblock 33 - built**
on `cf-waystone-premise`, branched from `26d973de`. It ran as a workflow:
one implementer, three `critics-developer` reviewers, then one fixer.

- **What it built** (§8.5 CF-S1b, as scoped):
  - `kPatternWaystone`, and the `origin` byte made reserved;
  - `Catalog::SetPatternWaystone`, which makes the directory page before
    taking page 9's hold;
  - OFF's walk over rows of every version;
  - the `WAYSTONE` prefix verb and its refusal table;
  - the view's `waystone` column;
  - `RenewPatternDirectories`: two passes, then a drop of the cache;
  - the boot log line and the `waystone` section of `MetaSections`;
  - `kSuperBlockVersion = 33`;
  - the deletion of auto-registration and the claim;
  - the recorder reduced to a lookup of opted rows;
  - the test reach, with `tests/waystone_opt_in_test.cpp` new;
  - CLAUDE.md's rows, flipped at this landing.
- **Where the tree made the code diverge from §8** (the implementer's
  report):
  1. A refused bind borrow never fails its statement (`read_borrow.hpp`),
     so the `TxnConflict` row is unreachable. A cell pins the behaviour
     instead.
  2. The park waits on `appended_lsn() - 1`, because `IsDurable` is strict.
  3. A `?` is refused before parsing, since the native grammar refuses it.
     OFF fingerprints the text and parses a copy with each `?` read as
     `0`.
  4. `waystone_opted` is computed by a walk for each `SHOW META`, and the
     boot line uses the renewal's own count. The atomic count is S1c's.
  5. ON creates a page whenever recording is on and the row has no root.
  6. `FindPattern` caches every row, opted or not, and the recorder checks
     the bit itself.
  7. The 1,500- and 17,000-row mount cells mount with recording off. The
     reclaim re-drive cell runs both ways.
  8. The built-join cell is now backed by a cell that opts a shape in.
  9. The cache-before-discard cell runs at the catalog and dispatcher
     level, because a mount offers no hook between its drop and the
     discard.
- **Review: 19 findings** (5 bugs, 1 spec divergence, 10 test gaps, 3
  cleanups). 18 were applied and one was applied in part:
  - **Bugs fixed:**
    - a directory page leaked unlogged when page 9 could not be pinned;
    - `ERR` was sent after the state had changed, so a failed sync request
      now says the state is in effect but not durable;
    - OFF failing part-way now bumps the word;
    - a `FindPattern` read error is counted, not treated as "not opted".
  - **The part rejected:** the interim `FindPattern` miss on a SELECT that
    is not opted (a `ScanAll` of page 9's chain on every such statement
    while a key is on) is S1c's to remove, as §8.5 states. Its comment now
    states the cost.
  - **Mutation check:** five mutants were applied, rebuilt and run, and
    each was caught:
    - the durable park;
    - ON always creating a page;
    - the shape filter;
    - the recording-site `IsTrailReplayable`;
    - the renewal's failure arm.
- **Suite:** the full Debug suite (`ctest -j8 -LE bs-nightly`) passed
  4250/4250 on the final tree. Two cells are disabled by design and two
  are skipped. The 17,000-row cell, run alone with
  `KDS_TEST_LARGE_CATALOG=1`, passed in 212 s on the implementer's tree.
- **Overhead:** not measured; it is measured at the milestone's close.

**CF-S1c: the count, the map and the gate - built** (`b0e6db12`, then
the fold below).

- **The opted count.** It is one atomic for the instance. It moves only
  after a successful write that changes the bit on a current-version row,
  and it saturates at 0. It is set at every boot, either by the renewal or
  by a recount on the sim's `skip_recovery` boot.
- **The opted map.** `FindOptedPattern` answers from a per-core map that is
  complete for each cache generation. The map fills only while the count
  is non-zero, and a miss scans nothing. `Catalog::FindPattern` is deleted,
  and `IsCurrentOptIn` is the one predicate.
- **The `SelectBody` gate.** `HasReplayableStep` runs first, then the
  lookup, so a scan-only statement never fills the map. Replay reuses the
  entry. The collector runs only when the statement is both recording and
  opted. The identity guard keeps ANALYZE's line. A lookup that cannot read
  is counted in `write_failures`.
- **The contract suite** compares opted against not-opted under every
  ceiling, with the trail deleted, the trail corrupted and the bit flipped
  mid-run. Every recordable opted query asserts `replays`.
- **Review: 17 findings, none of them bugs.** 16 were applied. The opted
  count made a fourth instance word, which crosses CT6's threshold; that is
  CF-O-Q15, settled (a) by the spec. Eight mutants were each killed.
- **Divergences:**
  - `FindPattern` is deleted rather than kept beside `FindOptedPattern`.
  - "A failed write" is tested at every write site, including a
    fail-stopped log.
- **The fold.** The four instance words (schema word, oid sequence, mark
  count, opted count) are now one `catalog::InstanceWords`, wired by one
  setter, `Catalog::SetInstanceWords`, and one `CoreRuntime::Config` field.
  The old setters and fields are deleted. `catalog.md` CT6 and `rules.md`
  are amended. The engine's behaviour is unchanged. Review: 6 findings, all
  applied.
- **Suite:** the full Debug suite passed 4263/4263 on the folded tree
  (`ctest -j8 -LE bs-nightly`).
- **Overhead:** not measured; it is measured at the milestone's close.

**CF-S1d: documents - written.** One writer, two read-only verifiers
(one checking truth against the code, one checking completeness of the
stale text), then one applier.

- **Specs:** `waystone-concpets.md` is rewritten around the opt-in, with
  new §4a-§4e. Also brought to Branch O:
  - `pattern-tracking-levels.md`;
  - `create-pattern-user-defined-patterns-v1.md` §1, §2 and §4.1;
  - `protocol.md`, `sys-views.md`, `backup.md`, `heap-and-tuple.md`,
    `parser-v2.md` J5, `catalog.md`;
  - `wal.md:39`, now matching `page.md` §1;
  - `alter.md`: a rename now ends an opt-in;
  - `cabin.md`, `aggregate.md`, `crosscore.md`, `namespace.md`,
    `bulkinsert.md` and `index.md`;
  - `rules.md` and `keystoneid-k0-findings.md`.
- **Manual and config:** `manual/sql/sql.md` gains a `WAYSTONE` section.
  `manual/server/server.md`, `manual/server/backup-restore.md`,
  `kds.conf.sample` and `README.md` are updated.
- **Known gaps:** `known-gaps.md` is updated per §8.6, with a new section
  for the Waystone opt-in. Each entry is verified at `46c632aa`.
- **Stale text:** CF-O-Q14's list is cleared, and the bench drivers' text
  under `tools/` is corrected.
- **Divergence carried into the documents:** §8.3's `TxnConflict`
  bind-borrow row is left out, because the code cannot produce it (S1b
  divergence 1).
- **Verification:** 17 findings, all applied.
- **Suite:** not re-run. Only comments changed in C++, as `git diff -U0`
  shows; the build inputs are those of `46c632aa`.

---

## 8. Branch O - the amendment (CF-S1)

Written 2026-10-21 on `cf-waystone-premise` at `722006ec`, on W3, and
revised the same day on `cf-waystone-premise` at `5404a95b` against a
verification of 38 findings and a second review of 20 (§8.7). The source tree at `5404a95b` is the
one at `722006ec`, since only documents moved, so every `[source-read]`
below is at `722006ec`. §1's `command_dispatcher.cpp` citations have moved
since `23ea3e61`:
- `RecordExecution`'s three exits, 5245/7306/7404, are now `:5454`/`:7526`/`:7624`.
- `waystone_usable`, the D2 guard and the identity guard, 7643/7651/7661, are now `:7867`/`:7875`/`:7885`.
- ANALYZE's `fetch_id` line, 7437, is now `:7658-7659`.
- The known-gaps entries §1.3 cites (747, 391, 407, 1341, 1390, 1403) are now `:761`, `:405`, `:421`, `:1359`, `:1408`, `:1421`.

### 8.1 What O is

Waystone records and replays only for the patterns an operator has opted
in. The opt-in is one bit on the row the pattern already has: its
`fetch_id` row in `sys.patterns`.

**The second-model test, stated honestly.** What `CREATE PATTERN` was
withdrawn for was a second identity beside the fingerprint (a name, stored
source text, a relation of its own), together with an `origin` that
changed recording policy (`create-pattern-user-defined-patterns-v1.md:16-21`).
The opt-in *is* a recording-policy bit. That is what W0 asked for ("having
the DBA create it explicitly") and what W3 marked ($!O). It is admissible
because it sits on the fingerprint's own row and carries no second
identity: `fetch_id` is still the only name a pattern has, and no text is
stored. What does change is what `sys.patterns` is. It stops being "every
shape that repeated" and becomes "the opted shapes": one row for each shape
ever opted, with the bit saying which shapes are opted now. S1d amends
`create-pattern-user-defined-patterns-v1.md` §1 (a pattern as "a
fingerprint-identified case tracked by statistics") and §4.1 to match.

The operator names a pattern by example. `WAYSTONE ON <select>` compiles
the statement without running it, refuses an example whose *shape* admits
no step Waystone records under any plan, stores the bit and discards the
text. The refusal is drawn at the shape and not at the compiled chain,
because whether the chain *as compiled now* has a `kLookup` or `kProbe`
step is not a property of the shape: join order is chosen per `arg_hash`
and per plan statistics (`src/exec/step_compiler.cpp:2201-2232`), so a
later index, Cabin or statistics change can turn a probe into a scan or
back, and a negative literal leaves a tuple key equality a scan
(`:2540-2551`) under a `fetch_id` that a positive one shares. A shape that
admits a descent but compiles to none now is accepted, and the gate's
per-execution `HasReplayableStep` (§8.3) is what keeps such a run from
recording.

Nothing registers a pattern on the `SELECT` path any more. Each core holds
a complete map of the opted patterns. An instance with none opted pays one
atomic load per `SELECT`, and a pattern that is not opted pays one hash
probe instead of CF-S0's 1.2-1.5 us. Trails stay unlogged and are still
discarded at every mount. The bit survives a mount. Superblock 33 (§8.3,
Format).

### 8.2 The tree as read `[source-read]`

- **The row has room.** `SysPatternRow.flags` is a u16 at offset 38. Bit 0 is `kPatternPinned`, which has had no writer since 2026-08-31, and every row is written `flags = 0` (`include/kds/catalog/rows.hpp:627-632,694-701`; `src/catalog/catalog.cpp:3131-3132`).
  - Decode validates no flag bit (`src/catalog/rows.cpp:256-264`). The row is 41 bytes (`rows.hpp:679`).
  - `pattern-tracking-levels.md:11-12`: a per-pattern level would *replace* the `pinned` boolean.
  - `origin` is only ever written `kOriginAuto` (`rows.hpp:689-692`; `catalog.cpp:3131`).
  - The view has one reader of each stored field. It prints `origin == kOriginUser` and `flags & kPatternPinned` from the row (`src/server/sys_views.cpp:483-484`), and every row carries 0 in both. `rows.hpp:694-696`'s "no reader" is stale.
- **Registration is on the `SELECT` path.**
  - `RegisterPattern`'s only engine caller is `TrailRecorder::EnsurePattern`, which runs on an instance's second sighting (`src/stats/trail_recorder.cpp:53`). Tests call it directly, and `ClaimPatternWaystoneRoot` too (`tests/catalog_test.cpp`, `tests/command_dispatcher_test.cpp:51-111,1018`, `tests/catalog_page_ceiling_test.cpp:491-596`). `tests/catalog_row_test.cpp:131-165` writes `kOriginUser` and `kPatternPinned`.
  - The run-time directory claim belongs to `EnsureDirectory` (`:87`), which goes through `ClaimPatternWaystoneRoot`. The claim updates only the claiming core's cache (`catalog.cpp:3203-3206`). A loser's page is leaked (`trail_recorder.cpp:81-86`), and no known-gaps entry records that leak.
  - The insert is logged at `kBootstrapXid` (`catalog.cpp:3134`), and the claim is a logged `HEAP_OVERWRITE` at `kNoTxnId` (`:3165`).
  - So a `SELECT` appends catalog records today. The archive's fail-closed gate lists no `SELECT` (`src/server/command_dispatcher.cpp:1303-1310`).
- **Absences are never cached.** A `FindPattern` miss is a `ScanAll` of the whole chain from page 9 (`catalog.cpp:3073-3086` with `:3055-3056`). With replay on, a shape that never registers scans `sys.patterns` on every execution. Point reads by ever-new keys are such a shape. No S0 cell exercised this.
- **The gate today** (`command_dispatcher.cpp:7867-7932`):
  - `HasReplayableStep` walks the chain on every `SELECT` while either key is on.
  - `replay_scratch.Clear()` runs unconditionally (`:7901`).
  - The fingerprint is already paid for, because `statement_stats` reads it (`:7705-7710`).
  - The identity also serves the cabin optimizer (`:7885`).
- **The schema word.** `BumpVersion` is the catalog's private choke point. It drops this core's cache and advances the instance word, and each peer drops its cache at its next statement head (`include/kds/catalog/catalog.hpp:216-237`; `catalog.cpp:1039-1052`, `:1070-1077`). The pattern map goes with it (`src/catalog/catalog_cache.cpp:157-171`).
  - The word moves on more than DDL:
    - a clustered or index root growth, which DML causes (`AdoptRoot` -> `BumpWord`, `catalog.cpp:2973-2995`; the growing core keeps its cache and every other core drops its own);
    - a Cabin create or drop (`:3475`, `:3505`);
    - the resolve of a transaction that wrote catalog rows (`:1083-1084`; `command_dispatcher.cpp:6847`).
  - `RegisterPattern` deliberately does not bump. A mid-statement bump could dangle the `TableAccess*` the statement holds (`catalog.hpp:820-829`).
- **Mount.**
  - On core 0, redo is followed by `DropCache` (`src/server/expeditor.cpp:1021`) and then `DiscardAdvisoryState`, which runs `ClearEveryPatternWaystoneRoot` (`:1045-1063`; `src/server/restore.cpp:311-318`), before the listener binds.
  - The sim's `Boot` makes the same two calls (`sim/instance.cpp:97,112`). `ForgetUnwrittenPages` runs only in a restore's preparation (`restore.cpp:305-309`, called at `:421` and `sim/instance.cpp:449`).
  - The clear rewrites only `waystone_root` and `dir_depth` (`catalog.cpp:3238-3239`). It skips rows of another fingerprint version even when they hold a root (`:3234-3237`), and it keeps core 0's cache coherent through `UpdatePatternWaystone` (`:3246`), which is that method's second caller after the claim's (`:3206`). Flags survive.
- **Superblock.** `kSuperBlockVersion = 32` (`include/kds/server/superblock.hpp:203`). `Decode` refuses every version but its own (`src/server/superblock.cpp:72-78`).
- **Durability primitives.**
  - `InsertRow` and `OverwriteLogged` return a `Status` and no LSN (`catalog.cpp:279`, `:471`). `WalManager::appended_lsn()` is "where the next record goes; every byte below it has been appended" (`include/kds/wal/manager.hpp:233-234`).
  - The dispatcher's post-statement park waits on `IsDurable(lsn) || stopped()` under a `WalManager::DurableWait`, then turns a stopped log into the refusal through `EnsureDurable` (`command_dispatcher.cpp:868-897`). A `group` commit enters it by setting its pending LSN (`:4979-4982`). `Checkpointer::ParkOnLog` requests the sync itself first (`src/wal/checkpointer.cpp:598-612`).
- **Naming sources.**
  - KWP PARSE answers `pattern_id`, never `fetch_id` (`src/server/kwp_session.cpp:546-560`).
  - ANALYZE prints `fetch_id` in hex (`command_dispatcher.cpp:7658-7659`). The views print it in decimal (`sys_views.cpp:84-91,466`). The lexer has no `0x` literal (`src/parser/lexer.cpp:139-169`).
  - `sys.statement_stats` is memory-only and emptied by a restart (`include/kds/stats/statement_stats.hpp:13-24`).
  - ANALYZE is a dispatcher prefix. Its remainder fingerprints exactly as the bare `SELECT` does (`command_dispatcher.cpp:1462-1477`).
- **Precedents and gates.**
  - `PURGE` and `BACKUP` are refused inside an explicit transaction, `NotImplemented` at byte 0 (`:10974-10979`, `:1588-1591`).
  - An unlisted verb is admin (`:1219-1259`). `WITH` stands beside `SELECT` at the readonly floor (`:1232`).
  - The failed-txn gate refuses everything off its whitelist before any handler runs (`:1318`).
  - `ArchiveRefusal` answers `ResourceExhausted` at byte 0 (`:5501-5510`).
  - The identity is built from `fingerprint->fetch_id` with no namespace or oid in it (`command_dispatcher.cpp:7886-7889`). One statement text under two session namespaces is therefore one `fetch_id`. Replay drops an entry recorded against another relation (`src/exec/trail_replay.cpp:38`).
- **Spec conflict.** `docs/spec/wal.md:39` says every Waystone page is headerless. `page.md` §1 and the code say only the directory's interior pages are (`src/stats/waystone_page.cpp:31-35`).

### 8.3 The design `[design]`

**Surface.**

```
WAYSTONE ON  <select-statement>
WAYSTONE OFF <select-statement>
WAYSTONE OFF <fetch_id>          -- decimal, or 0x + 1-16 hex digits
```

- **Parsing.** `WAYSTONE` is a dispatcher prefix verb, beside ANALYZE (`command_dispatcher.cpp:1462-1477`). It needs no grammar production and no reserved word. It is not a patternable leading word (`src/parser/fingerprint.cpp:123-131`), so `kFingerprintVersion` does not move.
- **Gates and role.** It is admin by default. It is added to the archive gate (`:1306-1308`) and to `kKinds` (`src/stats/statement_stats.cpp:14-20`). The gates run in the dispatcher's order: the role check (`RequiredRole`, `:1295`), then the archive gate (`:1306-1310`), then the failed-txn gate (`:1318`), then the handler.
- **What ON does:**
  1. It parses and compiles the example through the native path `HandleSelect` uses, under the session's namespace, and does not execute it.
  2. It takes `fetch_id` from the parser's fingerprint and `stmt_class` from `exec::StoredStatementClass(chain.klass)`.
  3. It releases the compiled chain and every catalog handle, then writes. The write bumps the schema word, and no `TableAccess*` may be held across that (`catalog.hpp:820-829`).
  4. It discards the text.
- **What OFF does.** `OFF <select>` only parses and fingerprints the example. It never compiles it, so it applies none of the recordability, D2, `?` or borrow refusals, and OFF can always clear what ON set, including after the relation is dropped. `OFF <fetch_id>` parses the number only. Both spellings reach the same write (§8.3 Storage, OFF).
- **Replies:**
  - `OK WAYSTONE ON fetch_id=<decimal> waystone_recording=<on|off> waystone_replay=<on|off>`. The last two are the instance's boot ceilings, printed under their keys' own names, as `sys.meta` prints `cabin_optimizer=` (`command_dispatcher.cpp:1707-1708`).
  - `OK WAYSTONE OFF fetch_id=<decimal>`.
  - `... unchanged` when nothing was written. That covers ON on a row that is already opted and has a directory (or needs none, §8.3 Storage), and OFF when no row carrying that `fetch_id`, of any version, has bit 0 set, under either spelling, whether or not a row exists.
- **Refusals.** Each carries the offending token's byte in the whole command. Parser bytes inside the example are shifted by the prefix length. Nothing is written on a refusal. `SET CABIN_OPTIMIZER`'s bare `ERR` strings (`:2184-2193`) are not copied.

| Case | Applies to | Code | Byte |
|---|---|---|---|
| A session below admin | both | the role gate's `ERR permission:` text (`:1295-1298`), which answers before every other gate | none |
| The archive gate engaged | both | `ResourceExhausted` | 0 |
| Inside a failed explicit transaction | both | the failed-txn gate's own refusal (`:1318`), which answers before any handler: the bare `ERR current transaction is aborted ...` text with no `Status` (`:1334-1335`), or `ResourceExhausted` where the lifetime ceiling rolled it back (`:1326-1332`) | none |
| Inside an explicit transaction | both | `NotImplemented` | 0 |
| A missing or unknown second word | both | `InvalidArgument` | that word, or the end |
| `ON` followed by a number (the grammar is `ON <select>`) | ON | `InvalidArgument` | the number |
| `OFF <fetch_id>` malformed, or overflowing 64 bits | OFF | `InvalidArgument` | the number |
| An example that does not parse | both | the parser's own code | its byte |
| `INSERT`, `UPDATE` or `DELETE` | both | `NotImplemented` | the leading word |
| `WITH`, or another read the query layer answers | both | `NotImplemented` | the leading word |
| Any other leading word, `ANALYZE` included | both | `InvalidArgument` | the leading word |
| An example that does not compile | ON | the compiler's own code | its byte |
| A `SELECT` the native path declines (query layer, `parser-v2.md` I19) | ON | `NotImplemented` | where the native path declined, else the example's first byte |
| A shape that admits no `kLookup`/`kProbe` step under any plan: no conjunct, in the example or a subquery of it, equates a relation's tuple key with a literal, with another relation's column or with an enclosing row's (`src/exec/step_compiler.cpp:2530-2563`, the only places either kind is assigned; `include/kds/exec/step_chain.hpp:171`) | ON | `Unsupported` | the example's `SELECT` (invariant 9) |
| A D2 statement | ON | `Unsupported` | the row-volatile function |
| A `?` placeholder | ON | `NotImplemented` | the `?` (CF-O-Q11) |
| A bind borrow refused at the 1 s fault net | ON | `TxnConflict` | the example's first byte |
| `CreateDirPage` refused (a full pool) | ON | `ResourceExhausted` | 0 |

Compiling the example declares the bind's relation borrows (AT-S1). ON can
therefore wait behind a DDL's `X` for up to the 1 s fault net. That is a
stated cost, not a defect.

The recordability row follows `status.hpp:43-52`'s test: `Unsupported`
promises that the statement fails the same way on this release and every
later one, so it is given only for a fact of the shape, which no index,
Cabin or statistics change can move (invariant 9 makes such a shape
unrecordable for good). A shape that admits a descent is never refused for
its current plan.

**The view.** In `sys.patterns` / `SHOW PATTERNS`, the `origin` and
`pinned` columns are replaced by one `waystone` bool. It is true for a
current-version row with bit 0 set. A row of another fingerprint version
reads false, and its `fingerprint_version` column says why. ANALYZE is
unchanged and keeps its hex. OFF accepts both forms.

**Storage.**

- **The bit and the byte.** Bit 0 is re-scoped from `kPatternPinned` to `kPatternWaystone = 0x1`. `kOriginAuto` and `kOriginUser` are deleted, and `origin` becomes a reserved byte, written 0 and read by nothing; `rows.hpp` says so. `SysCabinRow`'s own `kCabinOriginAuto`/`kCabinOriginUser` stay. The row stays 41 bytes and no offset moves.
  - **The cache entry follows.** `PatternAccess` drops its `origin` member, whose default is `kOriginAuto` and whose comment claims a statement-path reader that does not exist (`include/kds/catalog/schema.hpp:599-608`; a grep of `src`/`include` finds no reader), and `AccessOf` stops copying it (`src/catalog/catalog.cpp:3020`). `PatternAccess::flags` stays (`schema.hpp:609`, copied at `catalog.cpp:3021`), and the opted map and `EnsurePattern` read `flags & kPatternWaystone` from it.
- **The writer.** There is one new writer, `Catalog::SetPatternWaystone(fetch_id, stmt_class, on, root)`. It holds page 9 exclusive across the decision and the write, as `RegisterPattern` does (`catalog.cpp:3104-3136`). The existence check and the write are therefore one act across cores, and two concurrent ONs serialize on that hold, with no claim.
  - **The directory page is created before the hold, never under it.** `CreateDirPage` is `CreateNewHeaderless` (`src/stats/waystone_dir.cpp:43-53`), whose `MarkHeaderlessBeforeInsert` reads the device and, on an id the device still holds, writes zeros and blocks on the data-sync thread's `SyncNow` (`src/storage/device_page_store.cpp:376-400`; `src/storage/data_syncer.cpp:61-68`). Under page 9 that would be a page latch held across a durability wait, which `include/kds/storage/device_page_store.hpp:158` says no path in the tree does and `known-gaps.md:405-419` declined for this same directory. So ON first reads its row with no hold (`GetSysPatternRow`); if the row is absent or has no root and `waystone_recording` is on, it calls `CreateDirPage` and passes the page in. Today's create also runs with no latch held (`src/stats/trail_recorder.cpp:81-87`).
  - **ON with no row:** under the hold, `AllocateRowId`, then an insert with `flags = 0x1`, the passed root and depth 1 (no root, depth 0, when none was passed). The insert is logged at `kBootstrapXid`, as `RegisterPattern`'s is.
  - **ON with a row:** set the bit, and write the passed root if the row has none. This is one logged overwrite at `kNoTxnId` through `MutatePatternRow`, which stays private.
  - **A passed page the write does not use** - the row gained a root from a concurrent ON between the unheld read and the hold, the row was found opted with a root, or the write failed - is logged as leaked (`waystone: directory page <id> leaked by WAYSTONE ON`); a failed write also refuses ON with its status. A row that needs a root and gets none (the read raced the other way) is written without one, and the next mount renews it.
  - **`waystone_recording` off:** ON sets the bit and creates no directory, because there is no recorder (`src/server/expeditor.cpp:947`; the collector exists only when `recorder_ != nullptr`, `command_dispatcher.cpp:7927`), so nothing would record into it whatever `waystone_replay` says; replay against a row with no root is already a plain miss (`:7907-7908`). The mount creates the root once recording is on.
  - **OFF**, under either spelling: one whole-chain `kWrite` walk under the hold - the visitor that never accepts, as `ClearEveryPatternWaystoneRoot` walks (`catalog.cpp:3215-3250`) - clearing the bit and the root of **every** row carrying that `fetch_id`, of any fingerprint version. The rows are kept, and the directories and their trails become unreachable, as at a mount. The walk does not go through `MutatePatternRow`'s version filter (`catalog.cpp:3158`) or stop at the first match (`ForFirstRow`, `:3147-3168`). It must not: a `fetch_id` is not salted by `kFingerprintVersion` and a bump moves only some hashes (`include/kds/parser/fingerprint.hpp:162-176,189-191`; `tests/waystone_replay_test.cpp:341-345` plants exactly that collision), so after a bump a lapsed opted row and the current row a re-issued ON inserts commonly share one `fetch_id`, and a first-match OFF meeting the lapsed row first would clear it, answer `OK`, answer `unchanged` on every later call, and never reach the current row, which would go on recording. OFF answers `unchanged` only when none of the rows it met had bit 0 set.
  - **Then:** release page 9, and `BumpVersion("waystone opt-in")`.
- **Durability of `OK`.** The hold is released before any wait. The statement reads `WalManager::appended_lsn()` after its write, which covers its own record and possibly later ones (a later bound, never an earlier one), and parks in the dispatcher's existing post-statement park (`command_dispatcher.cpp:868-897`). It enters that park by setting its pending LSN as a `group` commit does (`:4979-4982`), after requesting a sync as `ParkOnLog` does. An `unchanged` answer writes nothing, so it reads `appended_lsn()` **under page 9's hold** instead, which bounds the record of whichever earlier statement set the state it found (every writer of the bit writes under that hold), and parks the same way. What `OK` and `unchanged` promise, by the session's rung:
  - `strict` and `group`: the state answered is durable. A fail-stopped log turns into the refusal.
  - `relaxed`: the answer comes at once, and the bit is durable within the relaxed interval, as a relaxed commit is. A crash inside that interval can lose the opt-in. That costs performance only (invariant 8).
- **Deleted:**
  - the recorder's `RegisterPattern` call and its `CreateDirPage`/claim arm (`trail_recorder.cpp:36-90`);
  - `Catalog::RegisterPattern`, whose body moves into `SetPatternWaystone`;
  - `ClaimPatternWaystoneRoot`;
  - `CatalogCache::UpdatePatternWaystone`. The mount keeps coherent by a drop instead (§8.3 Mount);
  - `PatternAccess::origin` and its copy in `AccessOf` (above);
  - `TrailRecorder::Stats::patterns_registered` ("auto-registrations performed", `include/kds/stats/trail_recorder.hpp:75`; incremented at `src/stats/trail_recorder.cpp:55`; asserted at `tests/trail_recorder_test.cpp:87`), which O would leave permanently 0.
- **Renamed:** `TrailRecorder::kAutoRecordThreshold` (`trail_recorder.hpp:70`, read at `trail_recorder.cpp:13`, cited at `tests/waystone_across_cores_test.cpp:71`) becomes `kRecordThreshold`, since no pattern is automatic; its value stays 2 (CF-O-Q8). `CabinStore`'s own `kAutoRecordThreshold` (`include/kds/stats/cabin_store.hpp:293`) stays.
- **The recorder after the deletion.** `EnsurePattern` becomes a lookup that answers only a row with bit 0 set, since OFF keeps its row. `EnsureDirectory` uses the row's root. With no root, it counts `write_failures` and returns. No `SELECT` writes the log.

**The gate and its hot-path cost.**

- **The opted count.** The instance keeps one atomic count of the current-version rows with bit 0 set. Like the schema word, it is owned by the instance and handed to every core's catalog. `NoOptedPatterns()` is a single load, with no fill.
  - **It moves only after the row's write succeeds**, under page 9's hold, and only on a 0->1 or 1->0 change of bit 0 on a **current-version** row. Clearing a lapsed row's bit (OFF's walk) leaves it alone, and the decrement saturates at 0.
  - **It is set from the chain at every boot**, after redo where redo runs, by one count of current-version rows with bit 0 set: Renew's walk sets it, and a boot that runs no renewal runs a counting walk of its own. The sim's `skip_recovery` boot skips `DiscardAdvisoryState` and with it the renewal (`sim/instance.cpp:74-113`; used by `sim/sim_main.cpp:152` and `tests/sim_loop_test.cpp:238,414`), and would otherwise leave the count 0 over opted rows for its whole run.
- **The map.** While the count is non-zero, the per-core pattern map is *complete per generation*: one `ScanAll` at the first ask after a cache drop, holding every current-version row with bit 0 set. A miss is NotFound, with no scan. While the count is zero, nothing fills.
- **Soundness rule**, stated in `catalog_cache.hpp` and pinned by a test: every write that changes the bit or an opted row's root either moves the schema word, or is followed by a drop of the writing catalog's cache before that catalog serves a statement.
  - A stale count costs performance only (invariant 8). Given the two rules above, it is stale for at most one statement after a successful ON or OFF: a zero read during a peer's ON misses one statement, and a non-zero read just after the last OFF pays one fill.
  - A peer that still holds a root that OFF cleared writes into a directory that is unreachable but still allocated. That is safe because no Waystone page is freed during a run (`known-gaps.md:761`).
- **The gate**, in `SelectBody`, ahead of today's `waystone_usable` line:

```
keys_on  = recorder_ != nullptr || replay_enabled_
opted    = keys_on && trailable && fingerprint && !catalog_.NoOptedPatterns()
             ? catalog_.FindOptedPattern(fetch_id) : nullptr      // no page outside a fill
waystone_usable = opted != nullptr && exec::HasReplayableStep(compiled)
```

- The identity guard becomes `waystone_usable || optimizer_signals_ != nullptr || analyze` (`SelectBody` already takes `analyze`, `command_dispatcher.cpp:7727-7729`). The engine always wires the signals (`src/server/expeditor.cpp:1283,1302`; `src/server/core_runtime.cpp:284`), so a served instance keeps ANALYZE's `pattern_id=`/`fetch_id=` line either way; the `analyze` term keeps it for every dispatcher built with no optimizer surface, the golden cell `AnalyzePrintsThePatternIdAndTheFetchIdItsTrailIsKeyedBy` among them (`tests/waystone_replay_test.cpp:329-339`), whose statement is not opted. It costs nothing off ANALYZE.
- Replay reuses `opted`, so there is no second `FindPattern`, and `replay_scratch.Clear()` moves inside it.
- The collector runs only when `recorder_ && waystone_usable`.
- `RecordTrail` is unchanged.

| Statement | Pays | No longer pays |
|---|---|---|
| Nothing opted on the instance | one atomic load | `HasReplayableStep`, `Clear()`, `FindPattern`, the map fill, the collector, `Observe`, the directory, the trail |
| Some opted, this one not | the load, one `unordered_map` miss, and a share of the refill | the same, less the fill |
| Opted | today's path, less the claim | - |

**The refill.** While the count is non-zero, a refill is a whole-chain
`ScanAll` of `sys.patterns` on every core, once per word move: a DDL, a
root growth (on every core but the growing one), a `CABIN AUTO` create or
drop, or a catalog-writing resolve. A root growth comes from DML, so this
is not "per DDL". On a v33 volume the chain holds only the rows ever opted
(OFF keeps its row) and no auto-registered row, so the scan is bounded by
what operators opted, not by what repeated.

The VM's per-row `trail_`/`replay_` null tests stay
(`src/exec/step_vm.cpp:718,2556`).

The expected result, to be measured at the close:
- `pk`, `chain3` and `chain4`, with nothing opted, return to CF-S0's arm C within the spread.
- `scanprobe` opted, against not opted, keeps CF-S0's net against arm C: +5.0 % throughput and 6.4 % less server CPU (`bench/v3.3.0/results-cf-s0-premise-v3.3.0-97-g3d628df4.md:135`).

**The defaults.**
- `waystone_recording` and `waystone_replay` stay boot-only booleans, still default `true` (`include/kds/server/expeditor.hpp:266,273`; parsed at `src/server/expeditor.cpp:358-387`; copied per core at `:1853-1854` into `include/kds/server/core_runtime.hpp:165-166`). They are re-scoped as the instance ceiling over opted patterns, and no new key is added.
- Nothing records until an operator opts in. A config line that reads `waystone_recording = on` now means less than it used to, so the instance says so in three places, each printing the switches under their keys' own names, as the `cabin_optimizer` section does (`command_dispatcher.cpp:1707-1708`), never as a second name:
  - the boot log carries `waystone: waystone_recording=<on|off> waystone_replay=<on|off> waystone_opted=<n>`;
  - `sys.meta` carries the same three fields in a new `waystone` section of `CommandDispatcher::MetaSections` (`src/server/command_dispatcher.cpp:1667`), which is where the fields are produced (`sys_views.cpp:1067-1093`'s `MetaView` only splits them) and where none exists today;
  - `kds.conf.sample`'s Waystone comment (`kds.conf.sample:323-355`) and the manual state the behaviour change.

**Mount and cross-core.**

- **The mount.** `ClearEveryPatternWaystoneRoot` becomes `RenewPatternDirectories`, at the same point: core 0, after recovery (and, on a restore, after the preparation's `ForgetUnwrittenPages`), inside `DiscardAdvisoryState` and before the listener binds. Its callers are `expeditor.cpp:1063` and the sim's `sim/instance.cpp:112`, both through `DiscardAdvisoryState`. It takes the `waystone_recording` ceiling as an argument; `waystone_replay` does not enter, for the reason ON's directory rule gives (§8.3 Storage).
  - **Two passes, so no page is created under a catalog page latch**, for the reason ON's is created before its hold. Pass 1 is a read-only walk that collects the current-version opted rows. Then, with no latch held and only if `waystone_recording` is on, one `CreateDirPage` per collected row. Pass 2 is today's `kWrite` walk (`catalog.cpp:3215-3250`), writing each collected row's fresh root at depth 1 as a logged overwrite. A fresh root names no stale child, so the discard's reason still holds (`expeditor.cpp:1045-1055`). With recording off, the root is cleared and none is created.
  - **The mount's added cost:** per opted row, one device read of the new page's id, and on an id the device still holds a zero write and a `SyncNow` on the data-sync thread (`device_page_store.cpp:376-400`), plus one logged overwrite. The cursor restarts at 128 at a clean mount, so the zero-write arm is the common one after a run that reclaimed pages. The mount pays it once per opted row, before the listener binds; it is unmeasured until the close.
  - Every other row that has a root is cleared in pass 2, *whatever its fingerprint version*. Today's skip of other-version rows (`catalog.cpp:3234-3237`) is dropped, so a lapsed row keeps no stale root.
  - If a creation fails, the row's root is cleared, the bit is kept, the failure is logged, and the mount still opens. A row that changed between the passes (none can, before the listener binds, but the walk does not rely on it) keeps its write and the unused page is logged as leaked. A later `WAYSTONE ON` re-creates the root.
  - Opted rows of another fingerprint version are counted and logged as `waystone: <n> opt-ins lapsed at fingerprint version <v>`, each only when no current-version row with the same `fetch_id` is opted, so a lapse the operator has already re-issued is not reported again at every mount.
  - The walk ends by setting the opted count and **dropping core 0's catalog cache**. Without the drop, a pattern entry filled before the walk would keep the previous run's root. That root's children can be ids the cursor has handed to logged pages, so a trail write would overwrite them with no record. `[quiet-wrong]`
- **Cross-core.** The bit reaches every core through the word. A peer answers from its previous generation for at most one statement, which costs performance only (invariant 8).
  - Every core's fill already sees the root, because the directory exists before the word moves. No core claims at run time, so the root-claim race and its leaked loser page (`trail_recorder.cpp:81-86`) are gone.
  - Sightings stay per core; trails stay the instance's.
  - An opt-in is by *text*. One text run under two namespaces is one opt-in. An opt-in survives `DROP TABLE` and a re-create under the same name, because no `sys.patterns` writer runs on a drop and `fetch_id` carries no oid. It has to be turned off explicitly. Replay stays correct through the `rel_oid` check (`trail_replay.cpp:38`), but recording costs come back on a shape nobody opted for the new relation. The manual says all of this. Renew cannot tell when an example no longer resolves, so it counts nothing for that.

**Format: superblock 33, decided.** `kSuperBlockVersion` moves from 32 to
33 (`superblock.hpp:203`), and every older volume is refused. This is
decided by the operator's standing order of 2026-10-07, recorded at
`instructions/v3.0.0/raft-marks-2026-10-07.md` §11: *"superblock backward
compatibility is not needed. Until I say otherwise, always apply this"*.
Bit 0 of `flags` and the `origin` byte change meaning, which is
the case the order covers, so no operator item is opened.
- **The cost, stated as a fact:** every v32 volume and every v32 backup stops mounting, and restoring one is refused.
- `Decode` refuses every version but its own (`superblock.cpp:72-78`), so a v32 binary refuses a v33 volume and never reads an opted row as pinned.
- **What the bump buys:** on a v33 volume every `sys.patterns` row was written by `WAYSTONE ON`, because auto-registration is gone. No row is legacy, nothing is migrated and nothing inert is kept.
- Row bytes, offsets and record kinds do not change.
- The bump lands in S1b, with the deletion of auto-registration, so that the first tree to read 33 writes rows only through `WAYSTONE ON`.

**Dropped from the candidate designs, one line each:**
- A numeric `ON`, even checked against `statement_stats`: it accepts a scan-only or query-layer shape that will never record, a spelling that enforces nothing.
- "Superblock 30": the tree has 32, and O moves it to 33.
- The opted path's steady-state skip, the read-first directory walk and the pooled `Build` index: CF-S0 measured no cost there for them to remove (CF-O-Q13).
- The claim kept as a pool-full fallback: it leaves a logged write and the claim race on the `SELECT` path.
- ANALYZE switched to decimal, and a `waystone=`/`trail_entries=` line: unasked surface that breaks the golden lines in `tests/waystone_replay_test.cpp`.
- "`OK` is durable through an owned transaction's commit": unsourced. The named park replaces it.

### 8.4 Questions

**Adopted on W5** (*"push no verify and go ahead"*, 2026-10-21,
`raft-marks-2026-10-21.md` §8): CF-O-Q1..Q14 each (a), as CLA proposed,
on the go-ahead and not marked by the operator item by item.

**CF-O-Q1 - where an opt-in names its pattern from** (CF-Q7, first half).
- (a) By example. `ON` compiles a `SELECT`, takes its `fetch_id` and discards the text. `OFF` takes an example, which it only fingerprints, or a decimal or hex `fetch_id`.
- (b) A numeric `fetch_id` for both, checked against `sys.statement_stats` (memory-only, empty after a restart).
- (c) A numeric `fetch_id`, unchecked.

**CLA proposes (a).** It is the only form that checks the example's shape
against recordability: a shape that admits no descent under any plan is
refused `Unsupported`, and one whose current plan merely has none is
accepted (§8.3's refusal table). Under (b), a `pattern_id` passed by
mistake is refused, but a scan-only shape is still accepted.

**CF-O-Q2 - whether an opted-in trail is logged so it survives a mount** (CF-Q7, second half).
- (a) No. Trails are discarded at every mount and each opted row gets a fresh empty directory. Warm-up is two executions of an instance on one core, then a replay on any core.
- (b) Unlogged, with each child validated at mount. This rests on an unstated rule that no `kWaystone` page is ever freed. `[quiet-wrong]`: a wrong check lets `WriteTrail`'s displacing format overwrite a logged page (`src/stats/trail_store.cpp:117-129`).
- (c) Logged trails and a headered directory. This needs a new record kind, and it brings back the `SELECT`-path WAL appends that BL-S4 removed.
- (d) Kept only after a clean stop. This needs a superblock field, and a restore still discards (`docs/spec/backup.md:60-68`).

**CLA proposes (a)**: it is the only option with no new correctness rule.

**CF-O-Q3 - the spelling.**
- (a) `WAYSTONE ON|OFF <select>`, a prefix verb.
- (b) `SET WAYSTONE ...`. `SET` is otherwise a memory switch (`command_dispatcher.cpp:1355-1363`).
- (c) `ALTER PATTERN ...`. This puts the noun `PATTERN` back beside the withdrawn grammar.

**CLA proposes (a)**. The word is the operator's.

**CF-O-Q4 - the bit, the byte and the view.**
- (a) Re-scope bit 0 as `kPatternWaystone`; delete `kOriginAuto` and `kOriginUser`; re-scope `origin` as reserved, written 0; replace the view's `origin` and `pinned` with `waystone`.
- (b) A new bit 1, keeping `kPatternPinned`, `pinned` and `origin`.
- (c) A new bit 1, deleting `pinned`, `origin` and their constants, with bit 0 reserved and written 0.

**CLA proposes (a)**. `pattern-tracking-levels.md:11-12` already retires
the pinned boolean. (a) removes dead constants rather than spending a
second bit, and the superblock 33 bump covers the change of meaning. No
stored value is left asserting a provenance it no longer has. The view
change is user-visible.

**CF-O-Q5 - auto-registration and the directory.**
- (a) Delete auto-registration and the run-time claim. The statement and the mount are the only writers of a directory root.
- (b) Keep the claim as a fallback.

**CLA proposes (a)**. (b) keeps a logged write and the race on the `SELECT` path.

**CF-O-Q6 - cross-core reach and the "not opted" answer.**
- (a) `BumpVersion`, a complete per-generation opted map, and an instance-wide opted count that makes "nothing opted" a single load.
- (b) A Waystone-only word.
- (c) No invalidation.

**CLA proposes (a)**. An opt-in is a rare admin act. (b) is a second
channel for a fact the word already covers. Under (c) a peer can stay
"off" indefinitely. The count is what keeps a word move from costing a
zero-opt instance a chain scan.

**CF-O-Q7 - transaction and durability.**
- (a) Autocommit only (`NotImplemented`, byte 0, with a failed transaction answered by its own gate first). `OK` is sent per §8.3's rung table, through the named park.
- (b) Transactional, under the DDL undo hook (`command_dispatcher.cpp:6632-6687`).

**CLA proposes (a)**. The writes are outside any user transaction: an
insert at `kBootstrapXid` (`catalog.cpp:3134`) or an overwrite at
`kNoTxnId` (`:3165`), and an advisory bit gains nothing from atomicity
with user writes.

**CF-O-Q8 - the threshold.**
- (a) Keep n = 2 (`include/kds/stats/trail_recorder.hpp:70`).
- (b) n = 1 for opted patterns.

**CLA proposes (a)**, on the evidence. CF-S0 measured no threshold effect:
its numbers are steady state, with the first two passes excluded as
warm-up (`results-cf-s0-premise-v3.3.0-97-g3d628df4.md:81`), so nothing it
found asks for n = 1. The bit already selects recording policy (§8.1), so
the objection to (b) is the evidence, not the bit.

**CF-O-Q9 - the boot keys.**
- (a) Keep them, default `true`, as the ceiling. `ON` under an off ceiling is accepted and its reply says so, and the boot log and `sys.meta` report the ceilings and the opted count.
- (b) Default `false`.
- (c) Refused at boot by name.

**CLA proposes (a)**. A `true` default costs one load. Under (b) an opt-in
does nothing until a restart. (c) loses the instance kill switch that the
contract suite and the sim use.

**CF-O-Q10 - what OFF does.**
- (a) Clear the bit and the root, and keep the row.
- (b) Clear the bit only.
- (c) Delete the row.

**CLA proposes (a)**. The view then reads true. (c) needs a delete path
that `sys.patterns` has never had.

**CF-O-Q11 - `?` in the example.**
- (a) Refused by ON, `NotImplemented` at its byte. Accepted by OFF, which only fingerprints.
- (b) Fingerprinted without a compile, for ON as well.

**CLA proposes (a)**. The literal and `?` forms share a `pattern_id`
(`include/kds/parser/fingerprint.hpp:24-34`). `fetch_id` is that hash with
the leading select list left out (`:123-142`), so they share a `fetch_id`
too. ON's compile is what makes its refusals truthful for the example.

**CF-O-Q12 - a `kFingerprintVersion` bump.**
- (a) Opt-ins lapse. The mount counts and logs them, and clears their roots. The view keeps the old row, with `waystone` false and its version naming the lapse. The operator re-issues `ON`, which inserts a current row that commonly shares the lapsed row's `fetch_id`; `OFF`, under either spelling, clears the bit of both (§8.3 Storage, OFF), and the mount stops reporting the lapse once the current row is opted.
- (b) A silent lapse.

**CLA proposes (a)**. Carrying the bit forward needs stored text, which is
excluded by design.

**CF-O-Q13 - the opted path's own costs.** These are the exclusive,
dirtying directory `Get` on every re-record
(`src/stats/waystone_dir.cpp:86`) and `TrailReplay::Build`'s per-statement
map (`src/exec/trail_replay.cpp:17`).
- (a) Leave them, and enter them in known-gaps, priced by the close.
- (b) Fix them in O.

**CLA proposes (a)**: no measurement asks for the fix.

**CF-O-Q14 - stale text and the spec conflict.**

The stale text, now or once O's deletions land:
- **Prose:** `include/kds/server/command_dispatcher.hpp:503-513`, `include/kds/parser/fingerprint.hpp:194-196`, `manual/sql/sql.md:661-669`.
- **Comments:**
  - `rows.hpp:627-650,683,689-701,819`;
  - `include/kds/catalog/schema.hpp:580-598`;
  - `include/kds/catalog/catalog.hpp:232,842-894,1021,1371,1436`;
  - `include/kds/catalog/catalog_cache.hpp:262`;
  - `include/kds/storage/page_latch.hpp:28`;
  - `include/kds/stats/waystone_dir.hpp:164-167`;
  - `include/kds/server/core_runtime.hpp:160-164`;
  - `src/catalog/catalog.cpp:3122-3124,3470`;
  - `src/server/command_dispatcher.cpp:7648-7652`, which says ANALYZE's hex is "the hex `SHOW PATTERNS` lists" while the view prints decimal.
- **Code and comment:** `include/kds/catalog/schema.hpp:599-609` (`PatternAccess::origin`, defaulted to `kOriginAuto`, and the comment claiming the recorder reads it on the statement path) with `src/catalog/catalog.cpp:3020-3021`; `include/kds/stats/trail_recorder.hpp:70,75` and `src/stats/trail_recorder.cpp:55` (the threshold's name and the `patterns_registered` counter). All are changed by S1b (§8.3 Storage, Deleted and Renamed), not left as comments.
- **Specs:**
  - `create-pattern-user-defined-patterns-v1.md` §1 and §4.1 (`:16-21`, `:51-60`), amended to the bit's new meaning and to `sys.patterns` as the opted set, and §2 (`:30`, "`TrailRecorder` has one threshold, `kAutoRecordThreshold = 2`, for every" pattern), amended to the opted patterns and the renamed constant;
  - `waystone-concpets.md:73` ("`flags` ... and `origin` ... retained from the withdrawn declared-pattern design ... every row is `kOriginAuto`"), restated as the opt-in bit and a reserved byte;
  - `pattern-tracking-levels.md:5-12`.

The conflict is `wal.md:39` against `page.md` §1.
- (a) Correct them in CF-S1d, with `wal.md:39` brought to `page.md`'s text, which the code follows.
- (b) Enter them in known-gaps.

**CLA proposes (a)**. The operator rules which text the conflict corrects.

**CF-O-Q15 - the opted count is the instance's fourth word.** Raised by
CF-S1c's review. `docs/spec/catalog.md` CT6 keeps three words as three
pointers "by threshold, not principle" and says "a fourth word tips it
(AT-0 item 11)"; item 11's row reads "three at three words, one at four".
CF-S1c wired the opted count as a fourth pointer
(as `Expeditor::waystone_opted_`, `Catalog::SetOptedCount` and
`CoreRuntime::Config::opted_count`), so the code has crossed the
threshold the spec states.
- (a) Fold the four words into one instance-words struct, set once per catalog and carried by one `CoreRuntime::Config` field, re-opening AT-S2's schema-word seam and the fixtures that wire the words (`two_core_rig.hpp`, `core_runtime_test.cpp`, `catalog_name_race_test.cpp`, `transition_test.cpp`, `lock_family_test.cpp` and the rest).
- (b) Keep four pointers and amend CT6 in CF-S1d: four words, the opted count's rule in its table, and the threshold restated.

**Settled by the spec, (a)**: CT6 already states the threshold ("a
fourth word tips it"), and a spec's rule is followed, not re-asked
(CLAUDE.md). CF-S1c folds the four words into one struct, as a commit of
its own after S1c's count, map and gate. Recorded under the go-ahead of
W5, not as an operator mark.

### 8.5 Stages

Every stage is reviewed by `critics-developer` and gated by the full suite
(`scripts/test.sh`'s `ctest`, less the excluded labels). The overhead is
measured once, at the close.

- **CF-S1a - this amendment.**
  - Documents only. No build input moves, so no suite is run. That is stated, not reported as a pass.
  - Stops for the operator's marks on CF-O-Q1..Q14. No code moves before them.
- **CF-S1b - storage, the statement, the mount and the bump.**
  - `kPatternWaystone` and the reserved `origin`, with `PatternAccess::origin` and its `AccessOf` copy deleted (`schema.hpp:599-609`, `catalog.cpp:3020-3021`) and `flags` kept for the opted lookup; `SetPatternWaystone` (the directory page created before the page 9 hold, the decision and the write under it, the leak log, `BumpVersion`, the named park, and the `unchanged` park); OFF's all-versions walk; the `WAYSTONE` verb and its refusal table; the archive-gate and `kKinds` entries; the view's `waystone` column; `RenewPatternDirectories` in two passes, with the lapse count, the `waystone_recording` argument and the closing cache drop; the boot log line and the `waystone` section of `MetaSections`; and `kSuperBlockVersion = 33` with its `superblock.hpp` entry.
  - The deletion of auto-registration, the claim, `UpdatePatternWaystone` and `patterns_registered`, the threshold's rename, and the recorder made a lookup of opted rows.
  - Until S1c, a not-opted `SELECT` with a key on pays a `FindPattern` miss over the short chain. That is an interim cost, not measured.
  - Test reach:
    - `trail_*` and `waystone_*` re-pointed to opt in first, and the golden lines in `tests/waystone_replay_test.cpp`;
    - `tests/catalog_test.cpp`, `tests/command_dispatcher_test.cpp`, `tests/catalog_page_ceiling_test.cpp` (the 1,500- and 17,000-row mount cells), and `tests/catalog_row_test.cpp`;
    - the tests that get a pattern, a trail or a replay by traffic through a `stats::TrailRecorder` dispatcher and so depend on auto-registration, each re-pointed to issue `WAYSTONE ON` for its statement first: `tests/alter_table_test.cpp:207-222` and `tests/drop_table_test.cpp:130-143` (two executions, then one `SHOW PATTERNS` row), `tests/purge_key_test.cpp:262-276` (`replays=` after three executions) and `tests/function_conjunct_test.cpp:268,279` (`replays=1`);
    - the suites whose fixture builds a recorder, so that their recording-on arms record nothing with nothing opted: `tests/aggregate_contract_test.cpp:460-463`, `tests/where_bool_test.cpp`, `tests/where_value_list_test.cpp`, `tests/where_bool_subquery_test.cpp`, `tests/query_layer_test.cpp` and `tests/types_e2e_test.cpp`. Each either opts its recordable statements in, or S1b states per file that its assertions do not depend on a trail (a reply compared with recording on and off is still compared, but a vacuous recording-on arm is named, not left silent);
    - the reach is the grep `TrailRecorder|replays=|SHOW PATTERNS|sys\.patterns` over `tests/` and `sim/` at `722006ec`; a file it reaches that is not listed here (`analyze_test`, `keystone_id_test`, `types_contract_test`, `assertion_ddl_test`, `bulk_insert_test`, `core_runtime_test`, `show_sugar_contract_test`, `parser_fuzz_test`, `fingerprint_test`, `sim_loop_test`) was read and names `sys.patterns` only in a comment, a fingerprint equality or a view alias, or asserts an absence of `replays=` under a fixture with no recorder (`analyze_test.cpp:144-152`), which O does not change;
    - every test that pins 32;
    - the sim's `waystone` toggle, which issues `WAYSTONE ON` for its `SELECT`s at a seed-chosen op;
    - `tools/bm_waystone_control.py`, re-pointed to opt its shapes in before the "on" arm. With nothing opted it would measure nothing.
  - Tests:
    - one cell per refusal, with its code and byte where the table gives one; the role gate's and the failed-txn gate's cells assert those gates' own text, which carries no code or byte, and pin the order role, archive, failed-txn, handler;
    - the recordability refusal on a scan-only shape, and acceptance of a shape that admits a descent but compiles to none now (a negative tuple key literal);
    - the ON/OFF round trip and `unchanged`, including OFF of a pattern that is not opted under both spellings;
    - hex and decimal OFF;
    - `OFF <select>` after the relation is dropped, and with a `?`;
    - `OFF <fetch_id>` on a lapsed row;
    - a lapsed opted row planted under a statement's own `fetch_id` (as `tests/waystone_replay_test.cpp:341-345` plants one), a re-issued `ON`, then `OFF <fetch_id>`: both rows' bits are clear, the view reads false and the opted count is back where it was;
    - ON and the mount with `waystone_recording` off and `waystone_replay` on, creating no directory;
    - ON with no catalog page latch held across `CreateDirPage` (a page created before page 9's hold), and a concurrent ON's unused page logged as leaked;
    - ANALYZE of a not-opted `SELECT` on a dispatcher with no optimizer surface still prints `pattern_id=` and `fetch_id=`;
    - the bit survives a crash at `group`, a clean restart and a restore, while the trail does not; an `unchanged` answer at `group` parks until the state it reports is durable;
    - a peer core sees the bit after one statement;
    - a refusal inside a transaction leaves the transaction unpoisoned;
    - a v32 volume and a v32 backup are refused;
    - a crash cell pinning the mount re-create after recovery and before the listener (`[quiet-wrong]` if ordered wrong);
    - a cell that fills core 0's pattern cache before the discard and asserts the post-mount root is the renewed one (`[quiet-wrong]` without the drop).
- **CF-S1c - the count, the map and the gate.**
  - The instance's opted count, the complete opted map and its soundness rule, the `SelectBody` gate, the moved `Clear()` and the re-keyed identity guard.
  - The contract suite compares opted against not-opted, byte for byte, under the ceilings, with these arms: trail deleted mid-run, corrupted trail, bit flipped mid-run. **Every recordable opted cell asserts `replays > 0`**, so a wrong fixture cannot pass vacuously.
  - Tests:
    - a not-opted `SELECT` leaves the pattern-cache fill and the recorder's sightings untouched;
    - with the count at zero, a word move causes no fill;
    - the count matches the view after ON, OFF, `OFF <fetch_id>` of a lapsed row (no move), a failed write (no move), a mount, and a `skip_recovery` boot over opted rows;
    - an OFF at count 0 leaves it 0 (the decrement saturates).
- **CF-S1d - documents.**
  - `waystone-concpets.md`: the opt-in, the gate, the count and the complete-map rule, mount renewal and the lapse, and the opt-in by text.
  - `pattern-tracking-levels.md`: the bit is the level field's two-state encoding (0 = untracked, 1 = level 3), which a level field would widen in place rather than sit beside.
  - `create-pattern-user-defined-patterns-v1.md` §1 and §4.1.
  - `protocol.md`; `manual/sql/sql.md`, with a `WAYSTONE` section covering the opt-in by text, its survival across `DROP TABLE`, the namespaces and the rung table; `manual/server/`; `kds.conf.sample`; and the expeditor and `core_runtime` key comments.
  - CF-O-Q14's text.
  - CF-O-Q15 as ruled: CT6 amended, or the words folded (a code change, so the suite runs).
  - The suite runs again only if a comment edit touched source.
- **CLAUDE.md at S1b's landing, not at the close.** S1b's code falsifies the Waystone row ("each records trails and registers patterns", "`Catalog::ClaimPatternWaystoneRoot` is a claim"), so the row is flipped in S1b's landing commit, per the maintenance rule ("When a decision lands: update the spec that owns it and flip the milestone row"). The same edit brings the superblock mentions to 33: the Heap relations row ("the superblock (32 since CD) refuses every older volume") and the Core placement row ("the superblock (32 since CD's clean flag)"); and the CREATE PATTERN row's "`SysPatternRow`'s `origin`/`flags` stay on disk" becomes the opt-in bit and the reserved byte.
- **CF-S2 - the close.** One interleaved, rotated A/B in `build-release` on CF-S0's harness (`bench/v3.3.0/archive/cf-harness/`), named by `git describe`, about an hour including the build.
  - Base `722006ec` against the close, nothing opted, on `pk`, `chain3`, `chain4` and `scanprobe`.
  - At the close, `scanprobe` opted against not opted, with `replays=` read, against the +5.0 % / 6.4 % net (§8.3).
  - `scanprobe` at `cores = 2` if the budget fits, and a refill cell (rows opted, the word moved by root growth) if it still fits. Each cut is stated.
  - The mount's added cost (§8.3 Mount), timed as a mount with rows opted against one with none, if the budget fits; a cut is stated.
  - Results go to `bench/<version>/results-cf-s2-close-<describe>.md`, `<version>` being the tag in the close's `git describe`: `bench/v3.4.0/` unless the operator names another version, since the version of record is `v3.4.0` (`5404a95b`). The base `722006ec` describes as `v3.3.0-104-g722006ec` and is named so in the file; the CF-S0 file stays under `bench/v3.3.0/`.
  - Then §7's stage log and `known-gaps.md` (§8.6). CLAUDE.md's rows were flipped at S1b's landing.

### 8.6 What O leaves

**Closes:**
- `known-gaps.md:1010`, recording's cost on a point read, for every pattern not opted.
- The root-claim race and its leaked loser page (`src/stats/trail_recorder.cpp:81-86`), which never had a known-gaps entry: no run-time claim exists.
- `:1408`'s registration scan on the statement path. ON keeps one chain scan per call under page 9, over the rows ever opted.
- The `SELECT` path's catalog log writes past the archive gate (§8.2).
- The per-execution `sys.patterns` scan of an unregistered shape under replay.

**Narrows:**
- `:1359`: advisory work on the statement now happens for opted shapes only.
- `:761`: the leak is limited to opted rows, but it is not bounded per mount. Each mount strands every opted row's previous directory (a root plus up to 2048 trail pages), and each OFF strands that row's directory, so the leak grows with mounts and with toggles. A page ON created and did not use (a concurrent ON, a row found opted, a failed write) is logged as a leak, and so is a page the mount's second pass did not use.
- `:421`, restated: with `ClaimPatternWaystoneRoot` deleted, `SetPatternWaystone` (under page 9) and `RenewPatternDirectories` (at mount) are the only root writers. `GrowPatternDirectory` still has no caller, so every directory is depth 1 with displacement. A deepening would be a third writer, needing the same hold and a word move.
- `:1421`: every mount still dirties every `sys.patterns` page it walks, but on a v33 volume the chain holds only rows ever opted.

**Keeps:**
- `:405`, whole: the child-slot race in `LookupOrCreateWaystonePage` can still strand a trail and leak a page, because every core still records an opted pattern.
- `:1105-1112`: a paced publish's map write.
- `:1447-1453`: the sim's `skip_recovery`, which now skips the renewal as well.
- `:1617-1625`: the torn headerless page, still bounded by the mount discard.
- An opt-in is by text. It survives `DROP TABLE` and a re-create, applies in every namespace with the same text, and must be turned off explicitly. Each mount renews a root for it.
- The refill: a whole-chain scan per core per word move while anything is opted.
- Warm-up after every mount, the lapse at a fingerprint bump, and the opted path's costs (CF-O-Q13).

**Unmeasured until the close:** `cores > 1`, a cold pool, a size sweep, the refill under a moving word, and the mount's per-opted-row directory create (a device read, and possibly a zero write and a data sync).

### 8.7 Review record

Each finding (0-37) was checked against the tree at `722006ec`.

- 0 - applied: `:405` moves wholly to Keeps; the root-claim leak is cited at `trail_recorder.cpp:81-86` (decision 7).
- 1 - applied (decision 5): every word mover is named, and the refill is restated as a whole-chain scan per core per move.
- 2 - applied: the view reads both stored fields; `rows.hpp:694-696` joins CF-O-Q14.
- 3 - applied (decision 4): `UpdatePatternWaystone` is deleted, and Renew ends with a drop of core 0's cache.
- 4 - applied (decision 4): Renew clears roots of every version; today's skip is cited.
- 5 - applied: "on a restore, after `ForgetUnwrittenPages`"; `sim/instance.cpp:112` is named as a caller.
- 6 - applied: "only engine caller"; the four test files are in S1b's reach.
- 7 - applied: the golden lines are cited in `tests/waystone_replay_test.cpp`; `bm_waystone_control.py` is re-pointed in S1b.
- 8 - applied: a failed transaction answers with the failed-txn gate's refusal first.
- 9 - applied: `catalog.cpp:3073-3086` with `:3055-3056`.
- 10 - applied: `rows.hpp:689-692` and `catalog.cpp:3131`.
- 11 - applied: `:1105-1112`, `:1447-1453`, `:1617-1625`.
- 12 - applied (decision 7): `:421` is restated against the two remaining root writers.
- 13 - applied: the listed lines join CF-O-Q14.
- 14 - applied: `fingerprint.hpp:24-34` with `:123-142`; the `fetch_id` equality is stated as derived.
- 15 - applied: the close expects the net against arm C (+5.0 % throughput, 6.4 % less server CPU).
- 16 - applied (decision 1): bump to 33, recorded as decided; CF-O-Q5 and the "Dropped: a bump to 33" line are deleted.
- 17 - applied (decision 2): the second-model test is restated; CF-O-Q8 (formerly Q9) is re-argued on evidence; the spec amendments go to S1d.
- 18 - applied (decision 3): `InvalidArgument` at the number.
- 19 - applied (decision 1): `origin` becomes reserved, written 0, and `kOriginAuto`/`kOriginUser` are deleted.
- 20 - applied (decision 6): the view's `waystone` is true only for current-version rows, and `OFF <fetch_id>` clears a lapsed bit.
- 21 - applied (decision 6): OFF of a pattern that is not opted answers `unchanged` under both spellings.
- 22 - applied: `WITH` and other query-layer reads are `NotImplemented` at the leading word.
- 23 - applied: archive `ResourceExhausted` at 0, pool-full `ResourceExhausted` at 0, fault-net `TxnConflict` at the example's first byte.
- 24 - applied: same correction as 2.
- 25 - applied: the listed comments and `create-pattern-user-defined-patterns-v1.md` §4.1 join CF-O-Q14 and S1d.
- 26 - applied: S1d states the bit as the level field's two-state encoding.
- 27 - applied (decision 8), with one correction: `sys.meta` has no Waystone line today, so its three fields are new, not an existing line re-worded.
- 28 - applied (decision 4): the drop, the restated rule and the S1b cell.
- 29 - applied (decision 5): the atomic count; no legacy-chain cell is added, because a v33 chain holds only rows ever opted; a refill cell is added to CF-S2 if it fits.
- 30 - applied (decision 6): "the example, as compiled now"; OFF applies parse and fingerprint refusals only.
- 31 - applied: the opt-in by text, its survival across `DROP TABLE` and the need to turn it off explicitly are in §8.3, Keeps and S1d's manual.
- 32 - applied (decision 7): same as 0.
- 33 - applied (decision 7): the leak grows per mount and per toggle; ON's failure after `CreateDirPage` is logged; ON and Renew make no root with both ceilings off.
- 34 - applied (decision 6): same as 4 and 20.
- 35 - applied: the park is named by source-read (`command_dispatcher.cpp:868-897`, `manager.hpp:233-234`), `OK` is stated per rung, and the crash cell pins `group`.
- 36 - applied (decision 1): the source-read is corrected, and the "pre-CF binary ignores the bit" sentence is replaced by `Decode`'s refusal.
- 37 - applied: the `ForgetUnwrittenPages` wording, and warm-up as "two executions of an instance on one core, then a replay on any core".

A second review reported 20 problems, numbered s0-s19 in the order
reported, each checked against the tree at `722006ec`. Every one held and
is applied, except where a line below says otherwise:
- second review: s0 (tests reached through a recorder), s1 (Q7's ground), s2 (`MetaSections`), s3 (the defaults' cites), s4 and s13 (`PatternAccess::origin`, the threshold's name, `patterns_registered`, the spec lines), s5 (the role gate and the order), s6 (`Unsupported` drawn at the shape), s7 and s16 (OFF clears every row of a `fetch_id`, any version; the lapse logged only while unreplaced), s8 and s18 (the count's rules), s9 (directories keyed on `waystone_recording`), s10 (the switches printed under their keys' names), s11 (the order quoted from `raft-marks-2026-10-07.md` §11), s12 (results under the close's version), s14 (CLAUDE.md at S1b), s15 (no page created under a catalog latch; Renew in two passes; the mount's cost priced), s17 (the identity guard takes `analyze`), s19 (`unchanged` parks) - applied.
- second review: s17's fix is applied, but its premise is rejected in part: "outside tests nothing calls `set_optimizer_signals`" is false. `set_optimizer_surface` calls it (`include/kds/server/command_dispatcher.hpp:1936-1937`), and the expeditor builds the collector unconditionally and hands it to core 0 and to every peer (`src/server/expeditor.cpp:1283,1302,1373`; `src/server/core_runtime.cpp:284`), so a served instance keeps ANALYZE's line under the old guard. The break is in dispatchers built with no optimizer surface - the golden cell among them - and the `|| analyze` term fixes that at no cost, so it is taken.
