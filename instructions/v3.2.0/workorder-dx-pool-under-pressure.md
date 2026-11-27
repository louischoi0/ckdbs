# Work order DX - the buffer pool under pressure: a full pool that waits for its writeback, and an abort that cannot be refused a frame

Status: **CLOSED 2026-11-27 on the work branch, not landed** (`raft-marks-2026-11-27.md` §13): DX-S0..S3 at `8167fa139`, `7271241e4`, `5e25f1d26` and the close commit; a merge, a push, a tag and a version wait for the operator. **OPENED 2026-11-27** on worktree `dx-pool-under-pressure`
(branch `worktree-dx-pool-under-pressure`) from `481d101aa`
(`v3.4.17-202-g481d101aa`), under the operator's standing go-ahead of
2026-11-27 (`raft-marks-2026-11-27.md` §11): every DX-Q item is **adopted
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

Related: `docs/spec/eviction.md` §3.1-§3.3, EV6, EV8; `docs/spec/page.md`
§3, §8; `docs/spec/txn.md` (abort); `docs/inflight/known-gaps.md`,
"Eviction"; `instructions/v3.0.0/workorder-be-bounded-pool.md` (BE);
`instructions/v3.0.0/workorder-bg-scan-queue.md` (BG, closed narrow on
2026-11-23 with its scan queue not built).

## 0. What DX is

`[design]` Three behaviours of a pool that is full, in the order of what
each can cost:

1. **A rollback's compensation is never refused a frame.** Today one
   path is argued safe and not enforced.
2. **A pool full of dirty frames makes a statement wait for writeback**
   where it has nothing to undo, instead of refusing it.
3. **A scan larger than the pool** and the hot set it displaces - only
   if the operator reopens what BG closed (DX-Q4).

## 1. The tree as read

`[source-read]` from `docs/inflight/known-gaps.md` at `8274c951`:

- **A rollback compensation that meets a full pool while its thread
  holds a pin is refused, and that leaves the write un-undone** (`:112`,
  verified at `2f08b71c`): drain mode writes back only when the thread
  holds no pin (`DevicePageStore::ReserveFrame`), because a writeback's
  shared try re-enters the thread's own exclusive hold. "No path into
  `TransactionManager::Abort` holding a pin was found, and nothing
  enforces that either."
- **A mostly dirty pool refuses point updates** (`:40`, `[measured]`
  `bench/v3.5.0/results-dm-s1-ceiling-v3.5.0-89-gc003e7fe.md` §3): on
  2,048 frames over 1,000,000 rows, four sessions at `cores = 1`,
  143-220 `ResourceExhausted` in 6 s against about 40,000 updates, and
  557-596 with 78 % of the right sibling's reads removed. The likely
  cause as written there: a dirty frame is queued for writeback by a
  reclaim walk that passes over it, and the built retry re-walks without
  yielding. Any change that removes reads from a write path meets this
  cell.
- **Writers outside a statement meet the ordinary refusal** (`:122`),
  and no cell drives one into a full pool.
- **Three of BE's rules are argued, not tested** (`:99`): the reclaim
  walk's total bound, `FailStop`, drain mode under recovery and `Abort`
  at the cap.
- **The cold scan protects only a hot set touched more often than the
  scan laps the pool** (`:29`, `[measured]`): a hot set of half a
  16,384-frame pool is 92 % re-faulted after a scan four times the pool.
- **Writeback stamps the checksum into the live frame under a shared
  page latch** (`:68`): value-benign, a data race by the language's
  rule.

`[source-read]` in the code at `8274c951`: the reservation is
`DevicePageStore::ReserveFrame`
(`src/storage/device_page_store.cpp:901`, declared
`include/kds/storage/device_page_store.hpp:1498`); the CLOCK cap is
`kClockUsageCap = 5` (`device_page_store.hpp:856`); `OpenScanRing`
(`device_page_store.cpp:1376`) has one caller, the Cabin controller
(`src/exec/cabin_optimizer_exec.cpp:165`).

## 2. The mechanism `[design]`

1. **The abort.** `TransactionManager::Abort` and every compensation
   under it assert, in every build, that the thread holds no pin at
   entry; a path found holding one is reordered. Beside the assertion, a
   reservation made for a compensation drains by a writeback that does
   not need the caller's own page - the copy-under-latch writeback of a
   frame the thread does not hold - so the refusal has no path left.
   DX-S0 decides whether the assertion alone closes it.
2. **The wait.** A reservation refused outside a no-refuse window, by a
   statement that has written nothing or can park, parks under a wait
   event until writeback has cleaned a batch, then asks again, inside
   the statement's deadline. Writeback under pressure is driven by the
   pressure, not only by the reclaim walk's passing: a refused
   reservation hands a batch of dirty frames to the writeback path
   before it parks.
3. **The refusal stays** where a park is impossible - a synchronous
   dispatcher, a statement past its deadline - with the same code.
4. **The checksum stamp** moves to the scratch copy (`:68`'s named
   remedy), one line of §2.2's stage.

## 3. Rules

- **DX-R1. No refusal and no park after a mutation's first page write**
  `[quiet-wrong]`: EV8's no-refuse window is unchanged, and the wait is
  taken only before it or outside it.
- **DX-R2. An abort's compensation is never refused a frame**, and a
  cell fills the pool with dirty frames under a rollback and reboots.
- **DX-R3. A waiting reservation holds no latch and no pin**, is inside
  a `WaitScope`, and ends at the statement's deadline with today's
  `ResourceExhausted`.
- **DX-R4. `buffer_pool_frames` stays the hard ceiling**: resident
  frames never exceed it, and no reserve is carved that a setting does
  not name.
- **DX-R5. A pool that is not full pays nothing**: the hit path and the
  free-list path execute no new instruction. Measured at the close.
- The standing rules, as DT's order lists them.

## 4. Items for the operator (DX-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| DX-Q0 | Is DX opened, and with what scope? | (a) §2's 1, 2 and 4; (b) the abort only; (c) with the scan queue | (a) |
| DX-Q1 | Does DX-S0 gate? | (a) S0 reproduces `:40`'s cell on the current tree and searches every path into `Abort` for a pin; the wait is built only if the refusals reproduce. The abort's assertion is built either way. (b) all built regardless | (a) |
| DX-Q2 | The abort's guarantee | (a) the assertion, plus the drain that needs no page of the caller's; (b) the assertion only; (c) a per-transaction frame reserve | (a); (c) spends frames a setting does not name |
| DX-Q3 | What the wait is bounded by | (a) the statement's existing deadline (`lock_wait_fault_net_ms`); (b) its own key | (a) |
| DX-Q4 | The scan queue (BG-S2..S4) | (a) stays closed, as the operator closed it on 2026-11-23; (b) reopened inside DX | (a) |
| DX-Q5 | BE's three untested rules | (a) each gets a cell in the stage that touches it, with a seam between reclaim batches; (b) outside DX | (a) |
| DX-Q6 | Raised at DX-S2's review (B1, `[quiet-wrong]`): a `DELETE` reserves its row's assertion departure before its trail entry, so a refused statement of an explicit transaction may leave a reservation the trail does not show | (a) an explicit transaction holding any pending reservation keeps the refusal, as before DX-S2; (b) a per-statement mark of the pending set, unwound before a re-run | (a); (b) adds an unwind path to the assertion registry for a refusal that (a) leaves as it was |

### Marks

**Every row is CLA's proposal adopted on the operator's standing go-ahead
of 2026-11-27 (go-ahead-achieving-milestone)** - *"go ahead until
achieving milestone, I will follow CLA proposal if decision needed"*
(`raft-marks-2026-11-27.md` §11) - and none is the operator's own mark on
the item. Of DX-Q0..Q5 no item is tagged `[quiet-wrong]`; **DX-Q6 is**, written at DX-S2's review and adopted under the same word.

| | adopted | note |
|---|---|---|
| DX-Q0 | (a) | §2's 1 (the abort), 2 (the wait) and 4 (the stamp); not the scan queue |
| DX-Q1 | (a) | DX-S0 gates the wait on the refusals reproducing; the abort's assertion is built either way. The bar is read in §7 |
| DX-Q2 | (a) | the assertion, plus the drain that needs no page of the caller's |
| DX-Q3 | (a) | the statement's existing deadline (`lock_wait_fault_net_ms`) |
| DX-Q4 | (a) | the scan queue stays closed, as the operator closed BG on 2026-11-23 |
| DX-Q5 | (a) | each of BE's three untested rules gets a cell in the stage that touches it |
| DX-Q6 | (a) | written at DX-S2 and adopted on the same go-ahead; `AssertionEnforcer::HasPending` in `EndWrite`'s test |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DX-S0** | The census: every caller of `Abort` and of a compensation, with what it pins; the dirty-pool cell re-measured at the current tree; where writeback is queued from. No behaviour change | a results file; DX-Q1's bar read |
| **DX-S1** | The abort: the assertion, any reorder S0 finds, the drain as DX-Q2 marks | DX-R2's cell; the sim's allocation-failure sweep over rollbacks |
| **DX-S2** | The wait, pressure-driven writeback, the scratch-copy stamp | cells: the dirty-pool cell refuses nothing inside its deadline; DX-R1's window; a `tsan` cell over the stamp |
| **DX-S3** | The close: the dirty-pool cell and a resident control A/B, `eviction.md` §3.3, `known-gaps.md` | the operator's word on landing |

## 6. What DX does not do

- No change to the CLOCK hand, the usage cap or the cold-fault rule.
- No second pool, no per-core pool, no background writer thread beyond
  what the checkpoint and writeback already run (DX-S0 states if one is
  needed, and it is then the operator's item).
- Nothing for the 2.5-3.7 % resident-scan cost (`known-gaps.md:17`),
  whose cause is not found.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-26

On `known-gaps-milestone-orders` from `8274c951`, on the operator's word.
No stage is opened and no code is written. **Not reviewed**: the
`critics-developer` review was started and stopped before it reported,
so no line reference or reading in this order has a second reader behind
it.

### DX-S0 (2026-11-27): the census, on `dx-pool-under-pressure` from `481d101aa`

The results file is `bench/v3.5.0/results-dx-s0-census-v3.4.17-202-g481d101aa.md`.
No behaviour changed. The probe was a stderr line at each site, run once
over the suite and reverted. Every `path:line` below was read at
`481d101aa`, not at this order's `8274c951`.

- **Every abort is entered with no pin** `[measured]`. Over the whole
  suite, `bs-nightly` included, every entry read
  `PinsHeldOnThisThread() == 0`: 20,522 into `TransactionManager::Abort`,
  20,711 into `AssertionEnforcer::AbortTxn`, 143 into recovery's
  `RollBackOne`. `[source-read]` Each caller runs at a statement's or a
  transaction's boundary.
- **But a compensation can reserve with a pin held** `[source-read]`.
  - The rollback's row locator descends for write and keeps the leaf while
    `LeafStillCoversKey` fetches the right sibling
    (`src/storage/btree/btree.cpp:68-70`, `:109`).
  - A miss there, in a full pool, is refused with the drain skipped
    (`device_page_store.cpp:941`). The locator turns that into NotFound,
    and the compensation's write is left un-undone: `known-gaps.md:112`'s
    hazard, on a real path.
  - No measured run reached it. **So DX-S1 builds DX-Q2 (a)'s drain for a
    thread holding a pin**; no reorder is needed once it does.
- **The dirty-pool refusal reproduces** `[measured]`: 1,683-5,515
  refusals in each 6 s lifetime, 2.26-2.49 % of statements, against
  DM-S1's 143-220 (0.37-0.55 %).
  - **77-91 % of them are refused at `BeginNoRefuseWindow`'s open**
    (`device_page_store.cpp:1051`), which §2.2 did not name. The rest are
    refused at `ReserveFrame` (`:961`).
  - **Both refuse with the row's leaf held exclusive.** The point update
    opens its window inside the row visit (`command_dispatcher.cpp:14636`,
    called at `:14870`).
  - So DX-R3's wait cannot be taken where the refusal is raised. **DX-S2
    takes it after the statement has let go of everything**, as BA-S14's
    structural re-run does: it drains the queue holding nothing, then
    runs the statement again, only if the statement wrote nothing and under
    its one deadline.
- **Where writeback is queued from** `[source-read]`. Only the CLOCK walk
  (`SweepLocked`) queues a dirty frame. The queue is drained only on core
  0's 50 ms tick and in drain mode, and at `cores = 1` the tick does not
  run while a statement holds the reactor. So an ordinary statement's
  reservation queues the frames, retries 8 times without yielding, and
  refuses.
- **DX-Q1's bar, CLA's reading: met.** The refusals reproduce, so the wait
  is built in DX-S2. The abort's assertion is built in DX-S1 either way.
  This reading is CLA's on the standing go-ahead, not the operator's.
- **The suite at `481d101aa`**: 5,341 of 5,342 passed (`-j8`,
  `-LE bs-nightly`). The one failure was a tmpfs `Disk quota exceeded`
  while copying a WAL. It passed 3 of 3 times alone.
- **Reviewed** (`critics-developer`).
  - Applied:
    - H1, the locator's pinned sibling fetch the first draft missed;
    - H2, the refusal sites hold the leaf, so the wait moves to the
      statement's boundary;
    - three wrong figures (M1-M3);
    - L1-L5.
  - Rejected: H2's probe of the pins at each refusal site. The wait is
    not taken there, so what the sites hold does not decide the design.
- Overhead not measured; measured at the milestone's close.

### DX-S1 (2026-11-27): the abort, on `dx-pool-under-pressure` from `8167fa139`

Begun in one session, carried by its handoff into the next, and closed
there. Every `path:line` below was read on the worktree's tree.

- **Built** `[source-read]`:
  - `HeldFrames::kSkipPinned` (`include/kds/storage/device_page_store.hpp`):
    a writeback that claims no frame pinned at claim time. One file-local
    test, `LeftPinned` (`src/storage/device_page_store.cpp`), serves the
    claim, the run extension and the batch's log gate
    (`HighestDirtyPageLsn`), so the gate leaves out exactly what no run
    claims.
  - `ReserveFrame`'s drain in drain mode runs with pins held, in
    `kSkipPinned` (DX-Q2 (a)). The rollback locator's write descent is the
    path DX-S0 found holding one: it keeps the leaf while
    `LeafStillCoversKey` fetches the right sibling. No reorder was needed.
  - `storage::RequireNoPinsHeld(site)` fail-stops, in every build, at the
    entry of `TransactionManager::Abort`, `AssertionEnforcer::AbortTxn` and
    `RecoveryUndo::RollBackOne` (DX-R2).
  - Docs: `eviction.md` §3.3, the header's durability-wait bullet,
    `known-gaps.md` (the refused-compensation entry narrowed, not deleted;
    the BE-rules entry down to two).
- **Cells**, each killed by the mutation named `[measured]`:
  - `BoundedPoolTest.ADrainHoldingPinsWritesBackTheQueueButNeverAPinnedFrame`:
    by the old `t_pins_here == 0` gate, by `kSkip` at the drain, by the
    claim's pinned test dropped (page 16 written) and by the run's (page
    21 written inside a run);
  - `EvictionWritebackTest.ADrainLeavingAPinnedFrameAsksTheLogOnlyForWhatItWrites`:
    by the gate's pinned test dropped;
  - `BoundedPoolDeathTest.ARollbackEnteredHoldingAPinStopsTheInstance`
    (a `ROLLBACK` reaches `AbortTxn` first) and
    `...ATransactionAbortEnteredHoldingAPinStopsTheInstance`: each by
    dropping its site's call;
  - `PoolOfDirtyFramesTest.ARollbackFaultingIntoItUndoesEveryRow`: by
    dropping `Abort`'s drain mode (refused 77 against 1);
    `...AMountUndoingTheLoserInItUndoesEveryRow`: by dropping the sim
    mount's drain (before the handoff);
  - `BsAllocSweep.ARollback*`, four kinds, the assertion's included.
- **"The sim's allocation-failure sweep"** of §5 is BS's sweep
  (`tests/bs/alloc_sweep_test.cpp`, label `bs-nightly`): `sim/` has no
  allocation fault injection.
- **Not built: a cell for the locator's pinned fetch.** It needs the
  trail's page resident, the moved row's leaf and its sibling not, and no
  clean frame when the sibling is fetched. A rollback's earlier drains
  leave clean frames behind, so the SQL surface cannot form it
  deterministically. `known-gaps.md` records it.
- **Reviewed twice** (`critics-developer`).
  - The first review: H1, the gate leaving pinned frames out, and H2, a
    stronger store cell and a `ROLLBACK` death cell, applied. Rejected:
    folding the pinned test into `kSkip`, because it changes the tick's
    drain, which is unmeasured; `CountOld`'s -1, because a refused read
    already fails the cell.
  - The re-review found no correctness bug. Applied: a cell that kills
    H1, one predicate in place of two, the claim's skip merged, a rewrap,
    §3.3's opening. Rejected: `kRows` cut from 3,000 to about 1,800. Both
    dirty-pool kills were measured at 3,000, and 26 s of Debug time does
    not pay for measuring them again.
  - The re-review named one effect a release build now has: a pin count
    left too high on a reactor, which used to switch drain mode off
    silently, now stops the process at the next abort there. DX-S0 counted
    zero pins at all 41,376 entries.
- **The suite** `[measured]`: 5,346 of 5,348 (`ctest -LE bs-nightly -j4`,
  190 s, on the tree committed). The two failures are the host's recorded
  two-core flakes. `ContentionCountersTest.TwoCoresInsertingIntoOneRelationContendWhatAnInsertTakes`
  then failed 1 of 5 alone, at its 60 s expiry, which is its rate at base.
  `DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter` passed 5 of 5
  alone. Neither cell runs in drain mode or an abort holding a pin. The
  four `BsAllocSweep.ARollback*` kinds (`bs-nightly`) passed on the same
  build.
- Overhead not measured; measured at the milestone's close.

### DX-S2 (2026-11-27): the wait, on `dx-pool-under-pressure` from `7271241e4`

- **Built** `[source-read]`, as DX-S0 §4 designed it:
  - the pool notes its two refusals on the thread
    (`storage::NotePoolRefusal`, `page_store.hpp`): a reservation refused
    and a window's open refused;
  - `EndWrite` turns the note into `pool_refused_`, and `StageStatement`
    into `DispatchOutcome::pool_wait`, only under `may_park_`, only for a
    statement whose trail did not grow (DX-R1), and - in an explicit
    transaction - only where the transaction holds no assertion
    reservation (below);
  - `DispatchAsync`'s BA-S14 loop asks `PoolRerunDue` after
    `StructuralRerunDue`. It drains the dirty queue holding nothing
    (`PageStore::DrainForPressure`, `kSkip`, every build asking
    `RequireNoPinsHeld`) under `res.eviction_retry`, and re-runs only if
    the drain wrote something, the outcome's code is `ResourceExhausted`,
    and the statement's one deadline (DX-Q3 (a)) is not past - before and
    after the drain;
  - a re-run due withholds an explicit transaction's poison as a
    structural one does, and an autocommit scope is aborted as one that
    waits;
  - **the stamp** (§2.4): writeback stamps the checksum into the scratch
    copy after the `memcpy`, never the frame.
  - Docs: `eviction.md` §3.3 and EV8, `page.md` §8 (the durability-wait
    clause DX-S1 made false) and §10, `wait-events.md`'s
    `res.eviction_retry` (its site; `p2` unused, as it is booked),
    `known-gaps.md` (the dirty-pool entry rewritten as what is still
    refused; the stamp's entry deleted).
- **The golden log is re-pinned** (`wal_golden_log_test.cpp`): a page image
  logged from a frame after a writeback now carries the frame's checksum
  field, not the last write's CRC. `[measured]` The old pin passes with the
  frame stamp put back, so the stamp move alone moved the bytes. Redo never
  verifies an image's checksum, and an image reaches the device only
  through a writeback, which stamps it.
- **Cells**, each killed by the mutation named `[measured]`:
  - `DxPoolRerunRigTest.PointUpdatesIntoAPoolOfDirtyFramesAreNeverRefused`
    (1,200 point updates, 320 frames, no writeback tick; `pool_refused`
    rises, no client refusal, `SUM` exact): by `EndWrite` never marking the
    re-run;
  - `...AnUpdateRefusedAfterItWroteRowsKeepsTheRefusal` (DX-R1): by
    dropping the trail test - which first passed anyway, because the
    re-runs end at the deadline and the rollback hides them, so the cell
    reads the pool's counters per line;
  - `...ADeleteRefusedAfterItsAssertionReservationKeepsTheRefusal`: by
    dropping the reservation test;
  - `...AStatementFailingForAnotherReasonAfterARecoveredRefusalIsNotRunAgain`:
    by dropping the code test;
  - `EvictionWritebackTest.AWritebackStampsItsCopyAndLeavesTheFramesChecksumAlone`:
    by stamping the frame again;
  - `...AReaderOfTheChecksumFieldBesideAWritebackIsNoRace`, the `tsan`
    cell: a reader of the field under a shared hold beside a writer that
    dirties and flushes, on two cores. **Not run under TSan**: no TSan
    build exists on the host, and the nightly's `tsan` step is what runs
    it. In this build it checks only that the device's page verifies.
- **Reviewed** (`critics-developer`). Two correctness bugs, both fixed by the
  review and kept:
  - **B1 `[quiet-wrong]`**: a `DELETE` reserves its row's assertion
    departure outside any no-refuse window and before its trail entry. A
    refusal at its undo append, inside `BEGIN`, was re-run and reserved
    the departure twice, and the `COMMIT` kept both, so the assertion's
    total understated the group by one. Fixed by
    `AssertionEnforcer::HasPending`: an explicit transaction holding a
    reservation keeps the refusal, its earlier statements' reservations
    included, as it did before DX-S2.
  - **B2**: a refusal a caller recovered from left the note set, and a
    statement that then failed for another reason was re-run. Fixed by
    the code test.
  - Applied: `RewindForRerun` folded into `RerunAdmitted`, the
    end-of-statement clear removed, a stale comment on the headerless
    mark, `RequireNoPinsHeld` in place of an `assert`, the deadline asked
    again after the drain, and cells for B1 and B2.
  - Rejected: a counter for pool re-runs. `res.eviction_retry` in
    `sys.wait_stats` counts each re-run's drain already, and a second name
    for that quantity is what the project's rules forbid.
- **The suite** `[measured]`: 5,353 of 5,354 (`ctest -LE bs-nightly -j4`,
  189 s, on the tree committed); `BsAllocSweep.*` 14 of 14. The failure is
  `ContentionCountersTest.TwoCoresInsertingIntoOneRelationContendWhatAnInsertTakes`,
  the host's recorded two-core flake. A run before the golden's re-pin also
  failed `DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`, which
  then failed 54 of 200 runs alone on this tree against 45 of 200 at
  `481d101aa`, interleaved (the last 120: 33 and 33) - the flake, not this
  change.
- **The dirty-pool cell in Release** `[measured]`
  (`bench/v3.5.0/results-dx-s2-dirty-pool-v3.4.17-205-g5e25f1d26.md`,
  `5e25f1d26` against `481d101aa`, interleaved, `cores = 1`):
  - the client's refusals fell from 746-2,050 a 6 s lifetime (2.00-2.24 %
    of statements) to 0-1;
  - `pool_refused` fell from 1,120-2,902 to 6-27, because a drain writes
    the queue before the next walk asks;
  - no answer moved. Throughput is not compared: the cell's
    lifetime-to-lifetime variance is 3.1 times on one binary.
  - **DX-S2's bar is met on CLA's reading**: the cell refuses no client
    inside its deadline but 2 statements in 173,915, each a window's open
    whose drain wrote nothing or whose deadline passed, which
    `known-gaps.md` records.
- Overhead not measured; measured at the milestone's close.

### DX-S3 (2026-11-27): the close, on `dx-pool-under-pressure` at `8765d2d34`

- **The dirty-pool cell**: DX-S2's, `results-dx-s2-dirty-pool-v3.4.17-205-g5e25f1d26.md`
  (above).
- **The resident control A/B** `[measured]`
  (`bench/v3.5.0/results-dx-close-v3.4.17-206-g8765d2d34.md`, `ck-tester`,
  Release, `5e25f1d26` against `481d101aa`, 24 interleaved rounds with a
  second copy of the base as control, `cores = 1`, server on CPUs 0-7):
  - server CPU per unit, DX / base, 0.985 over 24 cells (0.973-0.994),
    none at or above 1; the control 0.999 (0.993-1.005);
  - `pool_refused` 0 and no reclaim in all 72 arm runs, and the same
    pool-hit count in every arm;
  - the 1.5 % saving is not attributed - DX's diff only adds work - and is
    read as the binary pair's, as the DW close read the same sign.
  - **DX-R5, CLA's reading: holds** to the harness's 1-2 % resolution.
    `[source-read]` The hit path (`FetchPinned`'s hit) and the free-list
    path of `ReserveFrame` are unchanged; what DX adds per statement is
    outside them (two thread-local clears in `StageStatement`, one read in
    `EndWrite` on a failure, a pin-count test at each abort's entry).
  - Cut: `strict` durability, `cores > 1`, a committing explicit
    transaction, relations above 10,000 rows. The measurement took 6
    minutes, about 25 with its driver.
- **Not executed**: the `tsan` cell under TSan (no TSan build on the host;
  the nightly's `tsan` step runs it).
- **Durable content moved**: `eviction.md` §3.3 and EV8, `page.md` §8 and
  §10, `wait-events.md`, `known-gaps.md`, `CLAUDE.md`'s buffer-pool row.
  The SQL surface did not change, so the manual did not.
- **Synced**: `git fetch origin` found `origin/main` at `481d101aa`, DX's
  base; nothing to merge.
- **The suite** at `5e25f1d26`: 5,353 of 5,354 (DX-S2's entry). The close
  commits change no build input.

