# Work order DZ - what a checkpoint still does on its reactor: the dirty-table walk, the page writes, the map barrier

Status: **CLOSED 2026-11-27** at `27e85e848` (`v3.4.17-209-g27e85e848`),
its documents on top: DZ-R5's bar met at every size and core count
measured, the run not longer, no server-level regression; the early carve's
change not shown met at either measured shape (§7, "DZ-S4").
**Closed on CLA's reading of the close measurement**, adopted on the standing
go-ahead and not the operator's mark. Opened 2026-11-27 on worktree `dz-checkpoint-reactor-residue`
(branch `worktree-dz-checkpoint-reactor-residue`) from `481d101aa`
(`v3.4.17-202-g481d101aa`), under the operator's standing go-ahead of
2026-11-27 (`raft-marks-2026-11-27.md` §11): every DZ-Q item is **adopted
on that go-ahead, never marked by the operator item by item** (§4,
"Marks"). Written 2026-11-26 on worktree
`known-gaps-milestone-orders` (branch
`worktree-known-gaps-milestone-orders`) from `8274c951`
(`v3.4.17-148-g8274c951e`) on the operator's word of 2026-11-26
(`raft-marks-2026-11-26.md` §3). A `[source-read]` claim
with a `path:line` was read at `8274c951`; a claim taken from
`known-gaps.md` carries that entry's own verification commit and is not
re-verified here.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/page.md` §5, §8; `docs/spec/wal.md` §11, §11a;
`docs/spec/sched.md` §4, §9; `docs/inflight/known-gaps.md`, "Carve and
checkpoint stalls", "The frame table's structure latch (DO)";
`instructions/v3.0.0/workorder-bi-checkpoint-off-the-reactor.md` (BI,
closed 2026-10-11); `instructions/v3.2.0/workorder-do-frame-table-long-holds.md`
(DO, closed 2026-11-22; its §5a is the fuzzy table's order);
`instructions/v3.2.0/workorder-cw-optimistic-page-read.md` (CW,
abandoned).

## 0. What DZ is

`[source-read]` Since BI a checkpoint is a `system` task that parks for
every sync, and since DO no other core waits more than about 0.3 ms for
its dirty-table walk. What is left is on the **walking core's own
reactor** (`CLAUDE.md`, "WAL", "Buffer-pool eviction").
`[design]` DZ bounds each piece of that to one bar, so a session on the
core that checkpoints is not stalled by the checkpoint.

**This order replaces candidate 7 of CLA's list as first stated.** The
list named the per-fetch holds of the structure latch; CW-S0 priced them
at 2.8 % of a point read's server CPU and CW was abandoned on that
number, so they are §6's, not DZ's.

## 1. The tree as read

`[source-read]` from `docs/inflight/known-gaps.md` at `8274c951`:

- **The dirty-table walk** (`:1618`, verified at DO-S3): the walking
  core is stalled about 49 ms for H1 at 1 Mi dirty frames under three
  readers (26 ms before E2), and the publish's walk again - the sort, a
  table lookup a slot and a handover a chunk, all off the latch but on
  the walker's reactor. "A walk budget, or a handover every few chunks,
  is not built."
- **The fuzzy table's premise is stated and not enforced** (`:1630`):
  no core yields between an append and its stamp.
- **The writes** (`:1578`, restated at `ec827a19`): a step's and a
  publish batch's `pwrite`s stay on the reactor, single writes up to
  about 300 ms in the kernel's writeback throttle, and the inline log
  gate after 16 deferrals at `relaxed`.
- **The map barrier** (`:1590`, `[source-read]`, not measured): the run
  task's `WriteHeaderlessMaps` and `WriteFreeMaps` take
  `map_flush_latch_`, a plain mutex, on the reactor, and a blocking
  flush - `SYNC`, a reclaim's `PersistMaps`, the synchronous anchor
  publish - holds it across an `fsync`.
- **A paced publish's map write can carry the bit of a page another
  core created unlogged after the collection** (`:1603`), allocated and
  never written after a crash.
- **A `relaxed` carve's longest stall is 226-258 ms per server life**
  (`:1571`, `[measured]` at `e5a7e1b7`), read there as a data-file
  `fsync` stall; BA-S13's exit, a carve under 10 ms, is not met.
- **Every core checkpoints in its turn** (`:1660`).

`[source-read]` in the code at `8274c951`: `DevicePageStore::PersistMaps`
(`src/storage/device_page_store.cpp:745`). The run's own functions are
not located here; DZ-S0 lists them.

## 2. The mechanism `[design]`

1. **The walk yields.** The collect and the publish walk give the
   reactor back every K chunks, holding nothing. What the walk has
   already collected stays valid by DO's §5a order - a frame dirtied
   after its slot was passed is the next run's - and DZ-S0 re-reads that
   order with a yield placed between chunks, which it was not written
   for.
2. **The premise is enforced.** A debug build aborts a yield taken
   between a dirty-table append and its stamp (`:1630`).
3. **The page writes leave the reactor.** A step copies each page under
   its latch on the reactor, as writeback does, and hands the copies'
   `pwrite`s to the instance's data-sync thread or a writer beside it;
   the run parks until they return. The copy stays where the latch is.
4. **The map barrier is tried, not waited for.** The run's map halves
   try `map_flush_latch_` and park on a busy answer, as the cadence
   tries the stream latch since CZ.
5. **The carve** is measured again first (DZ-S0). If it is still a
   data-file `fsync` on a statement's path, it is DZ's only where the
   statement can park (DZ-Q4).

## 3. Rules

- **DZ-R1. A run that yields mid-walk still covers every page dirtied
  before its begin record's redo point** `[quiet-wrong]`: a page the
  fold drops is a page redo does not replay. The anchor's fold never
  decreases, as today; a crash cell cuts between two yields of one walk
  on every core of a two-core instance (built on one core, DZ-Q7 (a)).
  **And its begin record lists every transaction of its core undecided
  at the append** (added at DZ-S0, §7): the active-transaction table is
  snapshotted after the walk, with no suspension between it and the
  append.
- **DZ-R2. A page write off the reactor writes the bytes copied under
  the latch**, and only what the copy covered is cleaned - writeback's
  rule, unchanged.
- **DZ-R3. One checkpoint runs at a time for the instance**
  (`wal::CheckpointGate`), and a yielded run still holds the gate.
- **DZ-R4. A failed write or sync is sticky** as today: a failed
  data-file sync stops the instance.
- **DZ-R5. The bar**: no single hold of a reactor by a checkpoint above
  10 ms at 1 Mi dirty frames, at `group`; a run's whole length may grow
  by at most 25 %, since a longer run lengthens the replay range.
  Missed, DZ reports which piece missed.
- **DZ-R6. No thread is started after startup** (`sched.md` §9
  invariant 1); a writer thread, if one is added, starts with the
  instance and is the operator's item (DZ-Q2).
- The standing rules, as DT's order lists them.

## 4. Items for the operator (DZ-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| DZ-Q0 | Is DZ opened, and with what scope? | (a) §2's 1-4; (b) the walk only; (c) the writes only | (a) |
| DZ-Q1 | Does DZ-S0 gate? | (a) S0 measures each piece's reactor hold at 64 Ki, 256 Ki and 1 Mi dirty frames on the current tree; a piece already under DZ-R5's bar is left. (b) all built | (a) |
| DZ-Q2 | Who makes the page writes | (a) the existing data-sync thread, its queue ordered so a sync never waits behind a batch longer than S0 measures acceptable; (b) one more thread started with the instance | (a), with (b) if S0 shows syncs queueing |
| DZ-Q3 | The walk's yield interval | (a) derived from DZ-R5's bar and S0's per-chunk cost; (b) a boot key | (a): a key is a second name for the bar |
| DZ-Q4 | The carve at `relaxed` | (a) measured, and built only if it is the `fsync` the diagnosis read; (b) outside DZ | (a) |
| DZ-Q5 | The unlogged-page bit (`:1603`) | (a) outside DZ: it leaks a page and DV counts it; (b) inside | (a) |
| DZ-Q6 | The map barrier, which DZ-S0 could not price (raised by CLA at DZ-S0) | (a) built as §2 item 4 says: the halves' own holds are 0.01-0.18 ms, but the case §1 names - a wait on another caller's `fsync` under the barrier - needs a concurrent blocking flush and a changed headerless map, which S0's bench does not create, and when it happens it is one data-file `fsync` on the reactor (`[source-read]`; 3.2 ms median and up to 252 ms on this host, BA's diagnosis); (b) left, as DZ-Q1 (a) leaves a piece under the bar | (a): DZ-Q1's "under the bar" is a measurement S0 did not take for this piece |
| DZ-Q8 | What the premise check does when it fires (raised by CLA at DZ-S1) | (a) **records**, as the suspend audit's other three findings do (`sched/coro.cpp`: "recorded, not fatal"), read by a cell; §2 item 2's "aborts" is the audit's house rule amended; (b) aborts the debug build, the order's text | (a): one audit, one policy - an abort would make this finding the only fatal one, and the suite showed it firing on one kind that needs no stamp (`PAGE_INIT`), the case an abort turns into a crash |
| DZ-Q7 | DZ-R1's crash cell (raised by CLA at DZ-S0) | (a) on the one-core simulator, the seam at a slice boundary of the start's walk, for both covers and for the loser S0 found (§7, DZ-S0); the two-core form waits for a simulator with cores (EC, written); (b) wait for EC | (a): the loser is a one-core defect, and EC is not opened |

### Marks

**Every row is CLA's proposal adopted on the operator's standing go-ahead
of 2026-11-27 (go-ahead-achieving-milestone)** - *"go ahead until
achieving milestone, I will follow CLA proposal if decision needed"*
(`raft-marks-2026-11-27.md` §11) - and none is the operator's own mark on
the item. No item is tagged `[quiet-wrong]`; DZ-R1 is, and is a rule.

| | adopted | note |
|---|---|---|
| DZ-Q0 | (a) | §2's 1-4 |
| DZ-Q1 | (a) | DZ-S0 gates; a piece already under DZ-R5's bar is left |
| DZ-Q2 | **(b), revised by CLA at DZ-S2 from its own (a)** | (a)'s proposal named its trigger, "(b) if S0 shows syncs queueing"; DZ-S2 met its mirror on the tree: on the one data-sync thread a step's write queued behind a whole held `fsync` (`CheckpointDataSyncTest.TheRunParksWhileItsDataSyncIsHeld`), which BI-Q13 says no step waits for. The writes get a writer thread inside `DataSyncer`, started and stopped with the sync's (DZ-R6: at startup, none after) |
| DZ-Q3 | (a) | the yield interval derived, no key |
| DZ-Q4 | (a) | the carve measured first, built only if it is the `fsync` |
| DZ-Q5 | (a) | outside DZ: DV counts the page it leaks (`sys.volume_census`, landed at `8dd5ce8ea`) |
| DZ-Q6 | (a) | raised at DZ-S0; the map halves tried and parked on a busy barrier |
| DZ-Q7 | (a) | raised at DZ-S0; the crash cells on the one-core simulator |
| DZ-Q8 | (a) | raised at DZ-S1; the premise check records, never aborts |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DZ-S0** | The census of a run: every step on the reactor with its hold, at three dirty-set sizes and `cores` 1 and 4; DO's §5a re-read with a yield between chunks; the carve re-measured | a results file; DZ-Q1's bar read |
| **DZ-S1** | The walk's yield and the enforced premise | DZ-R1's crash cell; the rotated gate's cadence cells unchanged |
| **DZ-S2** | The page writes off the reactor; the map barrier tried | cells: a write failing off the reactor is sticky; a blocking flush beside a run |
| **DZ-S3** | The carve, as DZ-Q4 marks | its own cell or its recorded cut |
| **DZ-S4** | The close: DZ-R5 measured, a foreground A/B on the checkpointing core, `page.md` §8, `wal.md` §11, `known-gaps.md` | the operator's word on landing |

## 6. What DZ does not do

- Nothing for the structure latch's per-fetch holds or the mutex's
  unfairness (`known-gaps.md:1624`, `:1634`): CW's subject, abandoned.
- Nothing for a WAL segment roll (DY).
- No change to the cadence, the rotation or what a checkpoint records.

## 7. Stage log (CLA, appended as stages land)

### Opened 2026-11-27, on the standing go-ahead

On `dz-checkpoint-reactor-residue` from `481d101aa`
(`v3.4.17-202-g481d101aa`), the operator's word of 2026-11-27
(`raft-marks-2026-11-27.md` §11); DZ-Q0..Q5 adopted as proposed (§4,
"Marks"). The order was not reviewed when written (below); it is read
against the tree by DZ-S0, and its line references into `known-gaps.md`
are `8274c951`'s (about 230 lines lower at `481d101aa`, every entry
present).

### DZ-S0 - census, done 2026-11-27

`bench/v3.5.0/results-dz-s0-census-v3.4.17-202-g481d101aa.md`, on
`dz-checkpoint-reactor-residue` at `481d101aa`, a new bench
(`bench/checkpoint_reactor_hold_bench.cpp`).

- **Over the bar**: the start's walk at 256 Ki (6.8-14.3 ms, three of four
  cells over it) and 1 Mi (28-73 ms), the publish's walk at 1 Mi
  (9.9-10.9 ms), a step's writes at 1 Mi (35 ms to 1.26 s, the kernel's
  writeback throttle). **Under it**: the map halves' own holds
  (0.01-0.18 ms), every piece at 64 Ki (the longest one start's walk,
  8.5 ms).
- **DZ-Q1 (a) read**: the walks are DZ-S1's and the writes DZ-S2's; the
  map halves are built anyway on DZ-Q6 (a), raised here, because the case
  §1 names was not exercised.
- **§5a re-read with a yield** (`[quiet-wrong]`): the publish covers every
  window a yield opens, so redo needs nothing new; **the active-transaction
  table does**. `Start` takes it before the walk, so a transaction of the
  checkpointing core begun during a yield and undecided at the append is in
  no begin record: its change reaches the device by the publish, and a
  crash leaves it committed in effect. DZ-R1 now says so, and DZ-S1 takes
  the transactions after the walk.
- **Not measured**: the carve (DZ-S3 measures it first, with the server
  binary); the map barrier's wait (DZ-Q6); the log gate and the anchor
  page, which the bench cuts.
- No code beyond the bench; no review run on a results file and a bench
  (the bench is reviewed with DZ-S1, which keeps it).

### Written 2026-11-26

On `known-gaps-milestone-orders` from `8274c951`, on the operator's word.
No stage is opened and no code is written. **Not reviewed**: the
`critics-developer` review was started and stopped before it reported,
so no line reference or reading in this order has a second reader behind
it.

### DZ-S1 - the walks in slices and the premise checked, done 2026-11-27

On `dz-checkpoint-reactor-residue` at `78dfeb068`, its review's changes on
top.

- **Built**: `storage::DirtyTableCollector` - the store's slot walk
  (`WalkDirtySlice`, 16 chunks a slice, the hold and handover a chunk as
  DO's E2 takes them, armed or not), a two-pass LSD radix sort by page id and
  the merge to one entry a page at its lowest non-zero recLSN, 64 Ki entries
  a slice. The run task's start and publish advance it one slice a poll with
  a yield between (`CollectSliced`); the synchronous `Start` runs the same
  slices back to back. `BeginWith` takes the active transactions after the
  table, with no suspension before the append (DZ-R1's second half). The
  premise check (§2 item 2): `wal/stamp_audit.hpp` notes each appended
  record that names a page, `StampPageLsn` clears it, and the suspend audit
  records a suspension between the two (DZ-Q8 (a)); a suite run with the
  audit printing found one kind that never stamps, `PAGE_INIT` (31
  suspensions), exempt with its reason. The simulator gained
  `RunLiveCheckpoint`, a checkpoint whose begin record lists the live
  transactions (`RunCheckpoint` is the mount's, which lists none).
- **DZ-R1's cell**: `DirtyTableCrashTest.ATransactionBegunInsideTheWalkIsListedAndTakenBackAfterACrash`
  - a transaction begun at the walk's seam, its row updated and left
  undecided, a live checkpoint, a crash: the row reads its old value. **Shown
  to fail with the transactions taken before the walk** (the row read 7007).
  It drives the synchronous path; the task path takes the same calls in the
  same order with yields between.
- **Review** (`critics-developer`): no engine defect. **Fixed by the
  reviewer**: the census bench's timing wrapper did not forward
  `CollectDirty`, so it measured the whole walk S1 replaced. **Applied**: the
  walk timed again as the start phase, a slice an occurrence (P1); the
  collection's vector reserved to the store's capacity and the scatter buffer
  never zero-filled, which put about 5 ms in the poll that ended the walk
  (P2); one note a page in the premise check, so sixteen appends to one page
  cannot overflow it (P4); the dead accessor, the table taken as a span, no
  handover after a slice's last chunk, a forward declaration for the store
  header, two stale comments. **Rejected**: re-using a walked table across a
  busy begin append (P3) - sound by §5a but against CZ-R3's "collects both
  again", which is not CLA's to reword, and a busy answer needs a roll;
  folding `MergeDirtyTable` into the collector (the whole and the sliced
  merge take different shapes, and `DirtyTableCollectorTest` pins them
  equal); deriving the collector from the WAL seam (it keeps the storage
  class free of the checkpointer's header); a task-path loser cell (the
  order is one function's, `BeginWith`, on both paths).
- **Suite**: 5,346 of 5,348 at `78dfeb068` with the audit printing
  (`ctest -LE bs-nightly -j4`, 186 s); the two failures are memory's base
  flakes (`ContentionCountersTest.TwoCoresInsertingIntoOneRelationContendWhatAnInsertTakes`
  3 of 10 alone, `SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement`
  1 of 10). The review's changes ran S1's 86 cells green; the full suite
  runs again with DZ-S2.
- Overhead not measured; measured at the milestone's close.

### DZ-S2 - the writes off the reactor and the map barrier tried, done 2026-11-27

On `dz-checkpoint-reactor-residue` at `78cbbf464`.

- **Built**: `DevicePageStore::StageWriteBack` - `WriteBack`'s gate, claims
  and copies on the caller (the copy under each page's latch, as writeback
  copies), the device writes posted to `DataSyncer::Post`'s writer thread
  (DZ-Q2 revised to (b), §4). Armed, the writer cleans under the structure
  latch; unarmed, the store's one thread cleans - the run's finish, or a
  `kWait` writeback that meets the claim on that thread, which cleans for the
  parked run instead of waiting on itself. The clean is writeback's own
  (`CleanCopiedLocked`, DZ-R2). A failed write leaves its run and every later
  one dirty; only a failed sync is sticky (DZ-R4). The run task's every step
  and publish batch is staged, parked on and finished (`WriteOffReactor`);
  `Abandon` drains a batch in flight. The map halves try the barrier and park
  1 ms on a busy answer, to 16 a half, then block (DZ-Q6 (a)).
- **Cells** (`tests/checkpoint_offreactor_write_test.cpp`): the writer's
  order, a sync served beside a held job, `Stop` draining; a staged write done
  only once written, a page written after its copy left dirty; a blocking
  flush of a staged page on the store's own thread returning (armed and not);
  a failed staged write leaving its pages dirty for the next writeback; a run
  parked on its held step write while the reactor runs a foreground task,
  then publishing; the map halves parking on a held barrier.
  `CheckpointDataSyncTest.TheRunParksWhileItsDataSyncIsHeld` turns until the
  writer thread's writes land rather than a fixed 50 turns.
- **Checked against the bar before the suite** (one round, `hold-s2`, the
  census's bench forwarding the staged writes and tried maps): the longest
  `system` poll 1.05-1.12 ms at 256 Ki and 3.81-7.59 ms at 1 Mi, no poll over
  10 ms, against DZ-S0's 7.8-15.3 ms and 35-1,256 ms. Not the close's number.
- **Review** (`critics-developer`), at `78cbbf464`. **Fixed by the
  reviewer**, two defects: (B1) a staged run was written after the log
  fail-stopped - `WriteBack` asked the gate once, at its start, and the write
  came a whole park later - so each staged run now asks the same refusal
  (`WriteBackRefusal`) before its write, with a cell
  (`StagedWriteTest.ALogStoppedBetweenTheCopyAndTheWriteWritesNoLaterRun`,
  shown to fail without the check); (B2) a thrown allocation while staging
  could leave claims no thread gives back, so every later `kWait` writeback
  of those pages waited for good - the run is emplaced before its claims move,
  and a throw before the post gives every staged claim back dirty.
  **Applied**: the stage and the finish under the run's phase scope, so the
  gate and claim timers inside record as a step's did (P1); the store's
  destructor stops the data-sync threads before any member a queued job
  touches dies (P2); a refused stage gives its earlier runs back dirty
  instead of writing them on the reactor (P3); the map halves one virtual
  each with `busy`, null blocking, and one loop (`TryMapHalf`); `Drain` gone
  from the seam, `Abandon` waiting and finishing itself; three stale
  comments, and `WriteBack`'s claim comment naming the staged call's
  exception and why no cycle follows (DZ-R3). **Rejected**: one bookkeeping
  helper for `Step` and the task's step loop - the two differ in deferral and
  parking, and a shared helper would carry both. **Noted, unchanged**: a
  claimed frame answers `FreePage` with `kDeferred` for a step's park rather
  than a write's microseconds; the pages-written tally counts pages cleaned;
  an empty snapshot counts no step.

### DZ-S3 - the early carve's sync off the reactor, done 2026-11-27

- **The premise, measured first** (DZ-Q4 (a)), at `481d101aa`
  (`v3.4.17-202-g481d101aa`, release, copied, sha256 `c537e1124590b823...`):
  BA's diagnosis shape - `insert1`, `relaxed`, `cores = 1`, 16 sessions, 8 s
  after 2 s, server on CPU 0, clients on 4-7, port 15761, three runs,
  `tools/ba_census.py`, data on `/dev/mapper/fedora-root` (xfs); load
  3.6-4.5, no compiler running. **The longest carve is 437, 511 and 478 ms**,
  and in each run it equals the reactor's longest hold outside a checkpoint
  run (438, 511, 478 ms) to within 1 ms: the carve is the longest stall on
  the core. 184-231 carves a run. Archive:
  `bench/v3.5.0/archive/dz-s3-carve-premise-v3.4.17-202-g481d101aa/`. That
  the time is the data-file `fsync` is BA's diagnosis's reading (page 0's
  writeback 13 us at the median), not re-traced here.
- **Built**, on DZ-Q4 (a) - the early carve is a tick's, where nothing waits
  on its result, so it can be split without a statement parking: with a
  `StagedCeilingPersist` the tick raises the ceiling and writes page 0, asks
  the data-sync thread for the sync and holds the block pending; a later tick
  that finds the sync done makes it the block ahead. A `Begin` that drains
  the window with the block pending persists it whole, as a carve there
  always has; a failed sync drops it, its ids burned. Wired on core 0 and on
  every peer. The burn stays a whole carve.
- **Cells** (`tests/trx_id_test.cpp`): a staged block is ahead only once its
  sync is done and no second block is staged meanwhile; a window drained
  with the block pending persists it once and installs it; a failed sync
  drops it.
- **Suite**: 5,363 of 5,364 at `98ccf34e5` (`ctest -LE bs-nightly -j4`,
  174 s); the failure is memory's base flake
  `SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement`, 1 of 10
  alone at `75046f305`.
- **Review** (`critics-developer`), at `98ccf34e5`: no defect; "durable
  before issued" checked on every path (another core's page-0 write, a
  claimed page 0, a clean page 0, a sticky failure, a burn, a stopped
  syncer). **Added by the reviewer**: two cells, a drain over a failed staged
  sync issuing nothing from the block and a burn dropping a pending block.
  **Applied**: a dead guard removed. **Rejected**: folding the pending block
  into the block ahead (it keeps "ahead" meaning durable, which
  `has_block_ahead()` and BA-S13's readers rest on); one callback for `done`
  and `result` (they mirror `DataSyncer`'s own pair); the stage method folded
  into its lambda (it mirrors `PersistTrxIdCeiling`). **Recorded as gaps**
  (`known-gaps.md`, at the close): page 0's write at the carve can wait out
  a staged checkpoint batch that holds page 0's claim (`kSkip` would be
  unsound - the claimed copy may predate the raise); a block drained before
  its sync is done counts as two carves; no cell drives the expeditor's
  staged persist through a crash.

### DZ-S4 - the close, 2026-11-27; DZ closed

`bench/v3.5.0/results-dz-close-v3.4.17-209-g27e85e848.md`: `27e85e848`
against `481d101aa`, release, copied.

- **DZ-R5 met**: no poll of B's over 10 ms in 12 census cells (64 Ki, 256 Ki,
  1 Mi; `cores` 1 and 4; two rounds), the longest 9.56 ms; A's up to
  1,063.58 ms with 1,234-1,642 polls over 10 ms at 1 Mi. The run's length
  summed per size fell (1 Mi 245.1 s to 233.4 s), so "+25 %" is not passed.
  B's two 9.4-9.6 ms polls at 1 Mi are unattributed by the census's pieces
  and recorded in `known-gaps.md`.
- **The foreground A/B on the checkpointing core** (§5): BA's census driver,
  A and B alternating. `insert1` `group` `cores = 2`: 12 pairs, medians
  242,446 against 240,456 statements a run, B higher in 6 - the first six
  pairs read -22 % and the second six +15 % (medians); no regression. `point` `group`
  `cores = 2`: B 1.0-5.5 % lower in three pairs, unresolved, on a path DZ does
  not touch. `insert1` `relaxed` `cores = 1`: statements a second equal
  within the runs' spread.
- **The carve, read by CLA and not the operator**: at `group`, `cores = 2`
  DZ-S3's early carve read 9-12 ms against up to 440 ms over the first three
  pairs, but over all twelve 0.8-273 ms against 1.7-440 ms (medians 10.4
  and 11.0 ms), so no fall is shown; at `relaxed`, `cores = 1` it did not
  fall (86-610 ms against 50-459 ms), where the window's last quarter drains
  before the staged sync is done. **DZ-Q4 (a)'s split is built and its
  effect is not shown at either shape** (corrected after the close; the
  close first read it as met at `group` from the first three pairs): the
  remedy - a block or a trigger sized to the sync's length - changes
  `kTrxIdBlockSize`'s trade (ids a crash or a burn throws away), which is not
  DZ's and is not taken on the standing word. Recorded in `known-gaps.md`.
- **Documents**: `page.md` §8, `wal.md` §11, `txn.md` §4.2, `known-gaps.md`
  ("Carve and checkpoint stalls", "The frame table's structure latch"),
  `CLAUDE.md`'s WAL row, the index and the register.
- **The close's review** (`critics-developer`, documents only, against the
  code and the archives): **one reading corrected** - the carve's split "met
  at `group`" rested on the first three pairs; over twelve its effect is
  shown at neither shape (`raft-marks-2026-11-27.md` §14's correction row) -
  and counts, ranges and times recomputed in both results files; `page.md`
  §8's "one pass at one core", `wal.md` §11's missing DZ-R1 order, `txn.md`'s
  tick wording and `CLAUDE.md`'s "no longer than a slice" corrected to what
  the code does; the gaps given their commit. Its one code observation - the
  collector's slice comment against the measured slice - is recorded in
  `known-gaps.md`, the comment left unchanged so the suite run at
  `00ca380a9` stands for the pushed tree.

