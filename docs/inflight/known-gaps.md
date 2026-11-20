# Known Gaps

What is missing, what does not survive a restart, and what the code does
differently from what a spec claims. Opened fresh for v3.0.0 on 2026-09-03;
the v2 list is at `git show 1769487:docs/inflight/known-gaps.md` and none of
it carried across.

Every entry names the commit it was verified at and the doc that owns the
fix. An entry whose verification predates its subsystem's last change is a
statement about an engine that no longer exists; re-verify or strike it.
A bug entry `d83bd502` closed unreproduced is cited here as
`git show eeb88f47:<path>`.

## Eviction

- **A resident scan is 2.5-3.7 % slower than before BE, and the rest of
  the cause is not found.** Measured at `eef442cf`
  (`bench/v3.0.0/results-be-close-v2.7.0-665-geef442cf.md`):
  - a full scan of 100,000 resident rows takes +274 µs at p50;
  - `range100` at 10,000 rows takes +28 µs.

  The neighbouring-stage bisection places it at BE-S2's slot array. Its
  larger part, cache-set aliasing from an exact 8 KiB slot stride, is fixed
  at BE's close. The rest is about 270 ns per page, and the store's hit
  path, within 1 ns of `e4b107af` on a microbenchmark, does not account for
  it. Owned by `docs/spec/eviction.md` (§3.1).

- **The cold scan protects only a hot set touched more often than the scan
  laps the pool.** Measured at `2f08b71c` (the same results file, §3): a
  hot set of half a 16,384-frame pool, read once or three times, is 92 %
  re-faulted after a scan four times the pool. The scan drives about eight
  laps of the hand, and each lap lowers every hot counter. BE-Q7 names this
  the case for the scan ring's own order, which is not written. Owned by
  `docs/spec/eviction.md` (EV6, §3.1).

- **The executor does not scan through EV6's ring, by decision.** Re-read
  at `0205444b`: `OpenScanRing`'s only caller is `cabin_optimizer_exec.cpp`
  (the relayout planner, its other caller, is deleted since BZ). A `SELECT`'s outermost walk faults cold
  instead (BE-R5, `eviction.md` §3.1), which keeps a scan larger than the
  pool from displacing the working set without the ring's costs
  (`workorder-be-bounded-pool.md` §1.6). What the ring would still add - a
  scan's residency bounded to its slots rather than to the pool - is not
  measured. Owned by `docs/spec/eviction.md` (EV6, §5, §7).

- **Three of BE's rules are argued, not tested.** Verified at `2f08b71c`:
  - **The reclaim walk's total bound** of `kClockUsageCap + 1` laps, which
    ends a reservation that other cores' hits keep warm. A two-core cell
    could not form the spin, and forcing it needs a seam between batches.
  - **`FailStop`**: a window that outgrows its share in a full pool.
  - **Drain mode under recovery and `Abort` at the cap.** It is reached
    only through `BoundedPoolTest.AnInsertRefusedForAFullPoolLeavesNoRowBehind`'s
    rollback and reboot, which may not fill the pool while they run.

  The `UPDATE` and carved-fill windows also have no cell of their own. Owned
  by `docs/spec/eviction.md` (§3.2, §3.3).

- **A rollback compensation that meets a full pool while its thread holds
  a pin is refused, and that leaves the write un-undone.** Verified at
  `2f08b71c`: drain mode writes back only when the thread holds no pin
  (`DevicePageStore::ReserveFrame`), because a writeback's shared try
  re-enters the thread's own exclusive hold. No path into
  `TransactionManager::Abort` holding a pin was found, and nothing
  enforces that either. The damage is Census B's #11
  (`bugs/a-fetch-refused-after-a-page-write-leaves-the-mutation-half-done.md`,
  closed unreproduced at `d83bd502`).
  Owned by `docs/spec/eviction.md` (§3.3) and `docs/spec/txn.md`.

- **Writers outside a statement meet the ordinary refusal.** Verified at
  `2f08b71c` (access statistics, then a page writer, write no page since
  DA): Waystone trails, the delete-mark purge,
  Cabin and assertion builds, and DDL page creation open no window. Census
  B classified each as refusing before its first write. No cell drives one
  into a full pool. **A declaration insert does write before its refusal**
  (verified at `cbe61e87`): `CREATE ASSERTION`, `CREATE TRANSITION` and a
  Cabin expression spill their text before the row is placed; each spill is
  logged at its append since the audit's G3, so a refusal after it leaves an
  unreferenced value the mount sweep collects, not a slot redo never built.
  Owned by `docs/spec/eviction.md` (§3.3).

## Testing

- **DH's close measured five cells and one recovery round; the controls
  were not measured.** On `dh-page-image-volume`, 2026-11-20
  (`bench/v3.4.15/results-dh-close-v3.4.15-5-g22818e44.md`): the operator
  stopped the measurement part-way, after a host shared with other
  sessions' builds had let 30 clean runs through in about three and a half
  hours. Not measured, A against B: `select-pk` at one and eight cores,
  `insert` at four cores, scenario 2, the four-core pressure cell (where
  DH-S0 read the image's build faster than the no-image one and left it
  unexplained), `strict`; the 10K-row update control has one round and no
  floor, and the recovery cell one round. "No cell slower for DH" is
  known for the five cells in the file and for no other. **The tree
  that merged DH with `origin/main` at `18f2e339` (CV, the audit's
  dead-code removals) was not suite-run before it was pushed**; DH's own
  tree ran 0 failed of 4,883 before that merge. **Owner: none.**
- **No cell crashes a multi-core instance under the rotated checkpoint
  gate and mounts it.** On `dh-page-image-volume` over `76fe6777`
  (DH-S2), 2026-11-20. Since DH-S1a a core's own checkpoint comes every
  `cores` periods, and recovery finds a transaction that core left open
  through that core's `CHECKPOINT_BEGIN`, which the anchor's fold keeps
  in the replay range (`wal.md` §11). That chain is pinned in pieces -
  the gate's turns (`CheckpointGateCadenceTest`), the fold
  (`SuperBlockFoldTest`), analysis seeding from every begin and keeping
  what a later begin does not list
  (`AnalysisTest.ALaterBeginThatDoesNotListATransactionDoesNotClearIt`) -
  and by no cell end to end: the simulator is one core, the two-core rig's
  gate is not rotated, and `SetRotation` is reached only by a running
  server. The order's "a crash between every pair of runs with a live
  transaction on each core" was not built; it needs a multi-core crash
  harness that does not exist. **Owner: none.**
- **Restore and PITR over a log of packed images are covered only
  implicitly.** Same commit. The simulator's backup cell
  (`SimLoop.ABackupTakenAcrossTheRunRestoresToItsStopOnEveryCommittedSeed`)
  and `PointInTimeTest.ARestoreToAnLsnPastTheStopEndsJustBeforeIt` run
  with the write fetch's image on, so their logs can hold
  `PACKED_PAGE_IMAGE` records; neither asserts that one lies in the
  window it restores over. **Owner: none.**
- **`DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter` fails about
  one run in ten on a loaded host.** On `dh-page-image-volume` over
  `8fc41252`, 2026-11-20, at a load average of 15 to 19: once in DH-S1b's
  suite, then 22 of 200 repeats, and 16 of 200 with DH's packing switched
  off in the same tree - the same rate, so not the packed image's. The
  cell gives each of five `group` commits on a peer core 2 s to be woken
  by the WAL writer's kick, in a rig whose idle block is 5 s; the kick is
  best-effort (`sched/waker_table.hpp`: a kick that reads the
  destination's flag before it is raised is skipped, priced at one idle
  block), and the failures print `kicks skipped 1`. The cell asserts a
  bound the kick does not promise. Not measured: the rate on a quiet host,
  or how often a server's peer commit pays the 10 ms block for the same
  race. **Owner: none.**

- **The sim's constraint stream cannot kill a mutant that ignores held
  assertion contributions.** On `bs-test-hardening` at `76a2c08c`,
  `scripts/bs/mutants.sh`'s `holds-ignored` (every held contribution
  skipped in `assertion_check.cpp`'s admission) survived 9 of 9
  `ckdbs-sim` runs. The sim writes `p0`, `c0` and `a0` from one session
  in autocommit (BS-A12), so two admissions are never in flight at once.
  Two cells kill it, 3 of 3 rounds:
  `AssertionRaceTest.AHoldCountsInEveryAdmissionAndInNoSnapshotAndIsRoomAgainOnceGivenBack`,
  which holds on one thread, and
  `AssertionRaceTest.TwoCoresAdmittingIntoOneGroupAdmitExactlyOneAndRefuseTheOtherAtAdmission`,
  on two cores. The other three mutants, which admit every row, loosen
  the bound by one, and never refuse a parent `DELETE`, die in both the
  sim and the cells. **Owner: none.**

- **A split page created after the split's first write is guarded by one
  unit cell, not the sim.** Found 2026-10-16 on
  `worktree-fix-h9-chain-ascent` off `8cacbb3d`, closing H9. Two shapes
  create a page after writing one: a full parent's divide or right-split,
  and a root grown over a leaf `SplitLeafAndInsert` divided (the old leaf
  is rebuilt before the root is created). Both are safe because the split
  claims every page, device room included, before it writes. A mutant that
  grows the device at a page's creation instead of at its claim passed all
  1,200 runs of `--faults grow` (200 seeds x uniform/zipfian x
  clean/sync-crash/crash, 1,500 ops x 2 iterations): at those sizes no
  parent fills, and a first split of a rising key stream is an append,
  which writes nothing before its root. The mutant is killed by
  `BtreeTest.ADivideWhoseSecondPageTheDeviceRefusesWritesNothing` alone.
  Whether the sim ever grows a root over a divided leaf has not been
  counted. **Owner: none.**

- **`DropTableReclaimServingTest.ATwoCoreMountReclaimsOnceEveryCoreHasCheckpointedWhileServing`
  failed once under host load.** On `worktree-bm-scan-kernel-open` at
  `6b771ec1`'s tree, one full Debug `-j8` suite failed its
  `IsAllocated(id)` expectation (`tests/drop_table_reclaim_test.cpp:684`)
  at a host loadavg of 30 from other sessions; the stage (BM-S4, the scan
  kernel's strings) touches no reclaim path. It then passed 6 of 6 alone
  and 12 of 12 under `ctest --repeat`. **Owner: none**, no cause observed.
  It failed once more on `worktree-bx-unique-index` at BX-S1 (`a994ecc3`'s
  parent tree), 10 of 10 alone.

- **`ContentionCountersTest.APageLatchWaitCountsItsSpinTurns` failed once
  under `-j8` load** on `worktree-bx-unique-index` at BX-S3 (on top of
  `e542bb7d`), a stage that changed tests only; 10 of 10 alone. **Owner:
  none**, no cause observed.

- **`SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement`
  timed out once under `-j8`** (its `KickUntil(..., 60000ms)`,
  `tests/set_expr_two_core_rig_test.cpp:90`) in a full Debug suite on
  `worktree-fix-ddl-build-fault-net` at `c8a1e45c`, with another session's
  suite on the host (loadavg ~14 on 8 CPUs). 5 of 5 alone (about 2 s);
  under 16 concurrent copies, 32 of 32 at `c8a1e45c` (12-21 s) and 32 of 32
  at the base `4f175ba9` (20-26 s). The change under test is not on the
  cell's path: two `UPDATE`s' relation `IX` are compatible, and a row wait
  goes through `AwaitWriteBlock`, which it did not touch. The wait prints
  no per-writer progress, so slow cannot be told from stopped. **Owner:
  none**.

- **`IdAllocationAcrossCores.TwoCoresWritingOneRelationIssueOneSequence`
  times out under `-j8`.** It failed its `KickUntil(..., 20000ms)`
  (`tests/id_allocation_across_cores_test.cpp`) in full Debug suites on
  `worktree-ap-s2-trail-on-fetch-id` at `7fc2c57` and
  `worktree-ap-s3-rule-0-prime` at `0fcdfcc`, then passed 10/10 alone.
  Reproduced 2026-10-09 (BA-S3, `worktree-ba-open-marks` at `2f8f7c42`):
  2 of 4 plain `-j8` suites; under `-j8` plus eight concurrent copies, 9 of
  80 runs failed there and 10 of 72 at the base `d43845a0`, so BA-S2 did not
  change the rate. The wait prints no per-writer progress, so a slow host
  cannot be told from a stopped writer. The sibling
  `ContentionCountersTest.TwoCoresInsertingIntoOneRelationContendWhatAnInsertTakes`
  prints it: 14 of 32 runs under the same load expired with both writers
  part-way (52-400 of 400 rows after 20 s, core 0 committing `strict`
  against a real log file) - slow, not a lost wake, which for this cell is
  a reading by analogy. The sibling now commits `relaxed` and `group` only,
  with fewer rows and a longer bound. **Owner: none**; the bound is not
  widened.

- **An expeditor's `Start()` failed once under `-j8` and did not reproduce.**
  Seen once, on `ay-s2-containment-wake` at `33b9433`, in
  `ExpeditorTest.AtOneCoreTheDispatcherHoldsTheInstancesLockTable`; green
  since. The suspected cause is unconfirmed: `TwoFreeLoopbackPorts()` probes
  and releases before the instance binds, and at one core no listener uses
  `SO_REUSEPORT`, so a clash fails `Start()`. Since AZ-S4 the file's
  `Start()` assertions print the status through `Started`, this cell's
  included (`tests/expeditor_test.cpp:533`); one in another cell,
  `TheTickRetiresADropsMarksWithNoFurtherDdl` (`:403`), is a bare `.ok()`. Re-verified at
  `7dd9b789` by reading. Owner: none.

- **AX-S2b's release-kick cell failed once under `-j8` and did not
  reproduce.** Seen once, on `worktree-ay-s0-order` at `14cfdfa`, in
  `RowWaitWakeRigTest.AWriterParkedOnAnotherCoresRowProceedsAtTheReleaseKick`
  (`tests/row_wait_wake_rig_test.cpp`), message not captured. Its bounds are
  wall-clock (`Within(2000ms)`, `Within(1000ms)`), which a loaded host can
  exceed. Since AZ-S4 its timed waits print what is safe to read across
  threads; that cannot separate a busy reactor from a statement that never
  parked. Bounds not widened; keep the next failure with
  `--output-on-failure`. Re-verified at `7dd9b789` by reading. Owner: none.

- **A `strict` park cell failed once under `-j8` and did not reproduce.**
  Seen once, on `worktree-fk-walk-ceiling-wait` at `448c35c1`, in
  `StrictParkRigTest.AnExplicitStrictCommitParksAndPublishesAtItsSync`
  (`tests/strict_park_rig_test.cpp:200`): after the gate's release the
  `COMMIT` did not answer inside `Within(2000ms)`. Thirty repeats of the
  file alone passed at the same commit, and the same `-j8` run passed at
  `941d05e8`. The suspected cause is unconfirmed: nothing kicks core 1
  after the release but the WAL writer's own kick, which is best-effort,
  and a skipped one costs the rig's 5 s idle block. The commit it was seen
  at changes no commit or park path. Bounds not widened. Owner: none.
  **Seen a second time**, on `dc-select-for-update` at `f6a12f5d`'s tree,
  at the same line, in a `-j8` run beside another session's build (load
  average 13): eight repeats of the cell alone passed, and the same `-j8`
  run passed whole when repeated. That tree changes no commit or park path
  either.

- **A rollback's schema-word move has no cell that kills its removal since
  AX-S2.** Removing `MoveSchemaWordIfCatalogWriter` from
  `TransactionManager::Abort` leaves
  `DdlFenceRigTest.ADropIndexRolledBackOnCoreZeroKeepsTheRowAPeerWroteWhileItWasOpen`
  and the full Debug suite green (verified at `b54a769`, AX-S3), because a
  peer resolves the index while the drop is open. The move is nearly inert
  (`EndDdlScopeById` moves the word again through `InvalidateAfterCompensation`);
  by reading, the one shape it orders is a rolled-back `CREATE INDEX` whose
  index a peer's memo holds (`ddl-transactional.md` §5b). Re-read at
  `7dd9b789`: the call is still in `Abort`. Owner: `txn.md` §4.1 and
  `ddl-transactional.md` §5e.

- **The assertion scan's floor is a fixed defect with no regression test
  under it.** The defect (AL-7c) started the assertion resume at the anchor
  fold's `checkpoint_lsn`, which can sit past a core's own `ASSERT_SNAPSHOT`
  (`assertion.md` §6.1). Both callers now pass the fold's `redo_start_lsn`
  (`Expeditor::Open`, `CoreRuntime::Open`); reverting that leaves
  `tests/expeditor_test.cpp` green. No instance cell reaches it, because
  each core's `ShutdownCheckpoint` syncs the store first, so removing that
  sync would make the defect reachable silently. The cheap harness is a unit
  cell over the anchor fold and the floor choice with records
  `{redo=1000, ckpt=1000}` and `{redo=100, ckpt=5000}` expecting floor `100`
  (the fold never decreases, so record order matters). Re-verified at
  `7dd9b789` by reading. Owner: `docs/spec/wal.md` §16.

- **`wal_ring_full_refusals` is proved zero where it must be and unproved
  where it fires.** A nonzero value needs an append to lose the drained ring
  space to another core `kRingDrainAttempts` (4) times running
  (`src/wal/manager.cpp`), which one thread cannot stage. The only statement
  of the gap is the comment in `tests/wal_manager_test.cpp`; it says
  `client-manual.md` repeats it, which `7dd9b789` does not (no ring-counter
  row there). Owner: `docs/spec/wal.md` §16.

- **A stale headerless bit an older build left is not repaired.** Verified
  at `f9014108` by reading: a region whose first flush lost its free-map
  write, re-created at the next mount by a build before the audit's G18,
  wrote its new free map beside the old headerless map, so both landed and
  the region now loads; an id the cursor reissued there as a headered page
  stays marked headerless - written unstamped, read unverified, past both
  WAL gates. Since G18 the state is never made, but nothing detects one
  already made: that would take reading each such page. Owner:
  `docs/spec/page.md` §5.

- **A split-sized record can still lose its drains, and fail-stop the log.**
  Verified at `f9014108` by reading: the largest `BTREE_SPLIT`, a
  `CHECKPOINT_BEGIN` and, since the audit's G17, every assertion snapshot
  chunk are cut to `kLargestSplitRecord` (about 272 KiB), which leaves the
  ring room beside other cores' records - but if other cores refill more
  than `ring_capacity` less that size during each of the
  `kRingDrainAttempts` (4) drains, the append is refused and the log
  fail-stops (`wal.md` §6-5). At the default 1 MiB ring that needs about
  752 KiB of other cores' records per drain; at the 512 KiB floor, 240 KiB.
  `ASnapshotRunBesideAnotherCoresAppendsDoesNotStopTheLog` shows the common
  case only. Owner: `docs/spec/wal.md` §6.

- **Two drivers' text still describes the old heap.** Verified at
  `18e7b511` by grep: the drivers that declared `(id int64, ...) HEAP` and meant `id`
  as the tuple key (`aggregate_benchmark.py`, `kwp_load_benchmark.py`,
  `bulk_insert_benchmark.py`, `scenario1_backtest.py`) declare `BTREE` since
  BZ, the storage BZ's heap has with the key still declared. The prose of
  `cabin_scope_ab_benchmark.py` (its `HEAP` relations were created with no
  storage word, so BTREE, since SUS-1) and of `scenario1_backtest.py`'s
  summary line still calls a relation a heap with no index; no measurement
  reads that text. Owner: `instructions/v3.2.0/workorder-bz-heap-hidden-key.md`.

## WAL

- **The server manual still says WAL recovery is not implemented.**
  `manual/server/server.md` §6 ("What a restart loses") reads "WAL recovery
  is not implemented" and "`enforcing=0` after a restart until recovery
  exists", against §5 of the same file, `wal.md` §8 and `assertion.md`:
  recovery runs at every mount and assertions enforce immediately after a
  restart. Read on `de-page-read-off-the-reactor` at `1942cb9a`,
  2026-11-19; DE's close corrected the same claim in §5 ("Durability
  points") and left §6 as found - it is not DE's.
- **A log this build wrote is not one a pre-DH build of the same
  superblock can always replay.** Since DH-S1b (on `dh-page-image-volume`
  over `8fc41252`) the write fetch's image is record kind 35,
  `PACKED_PAGE_IMAGE`, wherever the page packs to under a page (`wal.md`
  §10). No superblock or segment version moved (DH-Q11 (a)): a build that
  predates DH mounts a cleanly shut volume as before (read, not tested: it
  holds when the shutdown checkpoint completed), and refuses a crashed one,
  one whose shutdown checkpoint failed, a backup or a PITR whose replay
  range holds a packed image, at redo, as `no applier for record type
  UNKNOWN` - the older build has no name for kind 35 - after its own redo has applied the records
  before it, which a later mount on this build applies again. Nothing at
  the door says which build a volume needs; the refusal is the statement.
  **Not executed**: an older binary against a newer log - the claim rests on
  reading redo's refusal of a page record it has no applier for, at
  `f4138860` (`src/wal/redo.cpp`), and on
  `RedoTest.APageRecordWithNoApplierEndsRecoveryByName`, which drives that
  branch with a reserved kind, not with kind 35 on an older build. Owner:
  `docs/spec/wal.md` §10.
- **Only the write fetch's image is packed.** The whole pages an index
  build, a catalog chain's link edit, an assertion build and a
  `BTREE_SPLIT` log stay 8,192 bytes each (DH-Q13 (a)); their volume was
  not measured by DH. A page that does not pack to under a page - random
  or compressed bytes in wide columns - is logged verbatim and costs what
  it did before DH; DH measured only tables of integers and scenario 2's
  schema. Owner: `docs/spec/wal.md` §10.
- **Where pages are faulted in often, a write still pays most of the
  image's price.** On `dh-page-image-volume`, closing code `22818e44`,
  measured 2026-11-20 (`bench/v3.4.15/results-dh-close-v3.4.15-5-g22818e44.md`).
  With a table three times the pool (1M rows, 2,048 frames, one core) a
  statement logs 0.81 images, each owed by the page's fault-in, which the
  instance's cadence does not reduce; the pack halves their bytes (about
  6,900 B to about 3,080 B a statement) and the build is 14.8 % faster
  than before DH and still 19.5 % under the same build with no image
  debt. With the table resident the price is 2 % at four cores and inside
  the floor at eight. What reduces it there is the pool's size, not a
  setting DH added; conditioning the fault-in's debt on an image already
  in the replay range was noted and not proposed (the order, §2.1).
  Accepted by the operator at DH's close (`raft-marks-2026-11-20.md`
  §18). Owner: `docs/spec/wal.md` §10.
- **Above one core the replay range is up to `cores` checkpoint
  intervals, and one more for each turn a core loses.** Since DH-S1a
  (on `dh-page-image-volume` over `df0dcd70`, read and unit-tested there)
  the cadence is the instance's: one run starts each
  `checkpoint_interval_ms` and the cores take turns (`wal.md` §11), so a
  core's contribution to the anchor's fold comes from a run up to `cores`
  intervals old, its redo start about one interval below that run, where
  the run was one interval old - about 45 s at eight cores and the
  default. Until every core has run once after a mount the fold stays
  where the mount's completion checkpoints put it, now for up to `cores`
  intervals. What follows the fold follows it there: a crash replays that much log, the log keeps that
  much from recycling, a dropped relation's pages and a dropped index's
  wait that long to be reclaimed, and a backup's start is that far behind
  its `BACKUP START`. **Measured once** at DH's close (eight cores, 50 s of
  100,000-row updates, `kill -9`; one round, no floor): recovery read
  1,456,727 records against 161,207 and the mount took 0.72 s against
  0.19 s; the WAL directory held 268.4 MB against 201.3 MB. Accepted by
  the operator (`raft-marks-2026-11-20.md` §18). A core whose tick meets a run in progress, or the
  spacing rule, waits a whole period for its next ask, and its turn stays
  owed; nothing retries sooner. A cadence checkpoint that outlasts the
  period costs the turns its run spans: 1.2 periods halves the rate
  (`CheckpointGateCadenceTest`). Not measured: the
  mount time over a `cores`-interval replay range, and the log directory's
  size at eight cores. **Not covered by the suite**: that the cores' ticks
  are armed in phase (`src/server/core_runtime.cpp`) - the gate's cases
  drive the gate with in-phase asks, and a stagger put back would pass
  them, giving one run every interval x (1 + 1/`cores`); a test of two
  reactors' timers at a 20 ms period was judged a flake on a shared host
  and not written. Owner: `docs/spec/wal.md` §11.
- **A refused catalog report on a new page, or a catalog row no empty page
  holds, spends one page.** Re-read on `worktree-fix-catalog-page-ceiling`
  at `0e539d46`, 2026-10-18. `InsertRow` leaves the page allocated, empty
  and unlinked: a free needs BF-R4's replay gate, and no reclaim walk
  reaches an unlinked page (`page.md` §5, `drop-table.md` DT1), the trade
  `heap::ChainInsert` makes. Since superblock 31 the page is a `CreateNew`
  page from the general supply, not one of a finite reserved range, so the
  leak costs file space and no catalog capacity. This predates AZ-S1: the
  page used to hold the row. **No owner.**

- **`MountAnchorOf` still loops over 64 anchor slots where only slot 0 can
  be non-zero.** Named by AM-S4(d)'s review; re-read at `f3da0413`,
  2026-10-13. `SuperBlock::SetWalAnchor` refuses every slot but 0 and
  `Decode` refuses volumes that could carry another, so
  `SuperBlockCheckpointAnchor::MountAnchorOf` could be `wal_anchor(0)` and
  `SuperBlock::wal_anchors()` would lose its last caller. Kept as a canary,
  as `superblock_checkpoint_anchor.hpp` says. Owner: whichever stage next
  opens the anchor. (AM-S4(d)'s other cut, `CoreRuntime`'s unshared-store
  arm, is no longer dead: it is the documented fixture shape
  `core_runtime_test.cpp` opens.)

- **Every sync covers every live segment, so its cost grows with the
  checkpoint interval's worth of log.** Restated at BC-S5 on
  `worktree-wal-recycling`, 2026-10-07 (CN-9 §4 C2); first verified at
  `8f9a887` by reading.
  - `FileLogDevice::Sync` `fdatasync`s every segment of the live run
    (`src/wal/file_log_device.cpp`, `include/kds/wal/stream.hpp`). Its
    comment forbids narrowing that to the tail, because a roll can land
    between the stream capturing its watermark and the device sync.
  - **Since BC the run is bounded only while the fold moves**: segments
    wholly below the durable redo start are removed (`wal.md` §11-4). Under
    a bulk load at `relaxed` at `4cf30270` it did not: a page re-dirtied
    after its writeback copy kept its first recLSN and pinned the fold in
    segment 0, and 13 of 15 BQ-S1 runs removed 0 segments, with 33-39 live
    (`bench/v3.0.0/results-bq-s1-segment-roll-v2.7.0-909-g4cf30270.md` §5).
    **Since `fix-redirtied-page-rec-lsn` (`0e88060a`) the recLSN moves up to the lowest
    LSN stamped since the copy** (`wal.md` §11-2); the relaxed cell has not
    been re-measured, and the gate's rotation - each core one run about
    every `cores × (period + period / cores)`, 25 s there - still bounds how
    often the fold can move.
  - Syncing only the segments *written* since the last sync would be
    enough: the previous tail plus any segment created since. Nothing
    records which those are.

  Cost: one `fdatasync` per live segment per durable-point advance, about
  50 us per live segment (measured at `4cf30270`, BQ-S1); a segment roll at
  `relaxed` is held mostly by the tail's sync, not by this breadth (BQ-S1's
  H3 bound). **Re-priced at BQ's close, 2026-10-12**: the non-tail syncs are
  39.8-72.0 % of step c over the rolls at or above the hold's p99 in BQ-S1's
  C1-spread and C2 cells, but with
  every one of them removed the roll's hold p99 stays 652-780 ms at
  `relaxed`, so narrowing the sync (BQ's rung 3, H3, `[quiet-wrong]`, its
  proof unwritten) was recorded unable to meet BQ's bar and not built
  (BQ-Q6 (a), `instructions/v3.0.0/workorder-bq-wal-segment-roll.md` §7).
  The breadth stays and so does the comment forbidding the narrowing.
  Owner: `docs/spec/wal.md` §3,
  `wal/file_log_device.cpp`.

- **A segment roll does I/O inside the instance's append latch, and at
  `relaxed` under a bulk writer its hold is hundreds of milliseconds.**
  `WalStream::Append` rolls under the stream latch; `Roll` reaches
  `StartSegment` (`src/wal/stream.cpp`), which syncs the segment it leaves
  through `SyncDevice` and creates the next. Deliberate (`wal.md` §6 step 1,
  `include/kds/wal/stream.hpp`). Priced by BQ at `4cf30270`: p50 ~330 ms,
  p99 ~790 ms, max 1.2 s at `relaxed` under one bulk writer; ~6 ms p50 at
  `group` (numbers in `wal.md` §6 step 5). BQ closed with the latch kept
  (BQ-Q6 (a)); the open leads are H4 option 3 (no zero prewrite, tail
  unchanged) and rung 4, H6 - releasing the latch for the roll's I/O,
  `[quiet-wrong]` and unlicensed without the operator's word
  (`instructions/v3.0.0/workorder-bq-wal-segment-roll.md` §7). The
  re-dirtied-page recLSN fix that kept the log from recycling is at
  `0e88060a`, not re-measured. Re-verified at `7dd9b789` by reading.
  **Since CZ (closed 2026-11-18, on worktree `cz-checkpoint-roll-wait` at
  `74861601`) the cadence checkpoint no longer waits a roll out on its
  reactor**: it tries the latch and parks (`wal.md` §11). Every other
  taker still waits a roll out - a commit, a drain, `EnsureDurable` and a
  foreground statement on a page latch a roll's appender holds - and the
  roll's own length is unchanged (hold median about 250 ms in CZ's close
  cell, `bench/v3.4.12/results-cz-close-v3.4.12-113-g74861601.md`). The zero prewrite's cost is DG's,
  unopened. Owner: `docs/spec/wal.md` §6.

## Multi-core state

- **A peer's superblock is the volume's whole image** (closed 2026-09-03,
  AL-S9's review): `CoreRuntime::Open` refuses a config without it, so no
  field is carried across one at a time. Owner:
  `include/kds/server/core_runtime.hpp`.

- **The transaction lifetime ceiling bounds explicit transactions only, and
  defers to a statement that is running.** Verified on
  `worktree-txn-lifetime-ceiling` from `1aebdfe8`, 2026-10-12. The sweep
  (`txn_lifetime_ceiling_ms`, `docs/spec/txn.md` §1) rolls back an explicit
  transaction, idle or busy, DDL inside it included, between 1x and 1.25x
  its lifetime. What it leaves:
  - **An autocommit statement is not bounded.** It is its own transaction
    and the sweep reaches only a session's explicit one; a long autocommit
    `SELECT` holds its view for as long as it walks.
  - **A statement in flight defers the sweep for as long as it runs.** It
    is never interrupted, and a write does not yield (Statement
    scheduling, below), so one that never ends holds its transaction past
    the ceiling.
  - **Background read views are not reached** (`txn.md` §1, exemption 2,
    `[PROPOSED]`): the sweep walks connections, and the re-mint it would
    need is unbuilt.
  - **A swept KWP session's portals are kept.** A portal bound inside the
    transaction keeps its sealed batches after the rollback, as it does
    after a client's own `ROLLBACK`; the rows it delivers were read under a
    transaction that no longer exists.

  Owner: `docs/spec/txn.md` §1.

- **A core that stops is not an idle core, and it pins the instance's
  commit-order floor for good.** The floor
  (`include/kds/txn/instance_visibility.hpp`) is bounded by the minimum
  issue cursor over attached cores; an idle core is unpinned by burning its
  block on its own tick (`PinsFloor`, `txn/manager.cpp`). A core that wedges
  or shuts down takes no tick, so the floor stays and the window grows one
  entry per commit for the life of the process. Not reachable today: the
  expeditor stops every core together. Re-verified at `7dd9b789` by reading.
  Owner: `instructions/v3.0.0/workorder-an-read-view.md` AN-R14.

- **Two cores recording one pattern can strand a trail in a directory
  slot.** `LookupOrCreateWaystonePage` (`src/stats/waystone_dir.cpp`) reads
  a child slot under one page hold, releases it, allocates, then re-acquires
  to link, so two cores colliding on one `DirIndexAt` can both read
  `kEmptyDirSlot`, both allocate, and the second link overwrites the first:
  a stranded trail and a leaked page. Invariant 8 prices it: a lost trail is
  a lost replay, never a result. Not closed because holding the parent
  across `CreateNew` would put a page latch across a durability wait.
  **Still reachable since CF-S1b, for an opted pattern**: every core records
  an opted pattern, and while `WAYSTONE ON` or the mount makes the root, the
  recorder still links the child slots below it. Re-verified at `7dd9b789`
  by reading and on `cf-waystone-premise` at `46c632aa`. Owner:
  `docs/spec/waystone-concpets.md`.

- **Nothing deepens a Waystone directory, and a deepening would be a third
  root writer.** `ClaimPatternWaystoneRoot` is deleted (CF-S1b). The root and
  its depth have two writers: `Catalog::SetPatternWaystone` (`WAYSTONE ON`,
  which roots only a row with no directory, under page 9's hold) and
  `Catalog::RenewPatternDirectories` (the mount). Neither replaces a root a
  run is recording into. `GrowPatternDirectory` has no caller outside
  `tests/waystone_dir_test.cpp`, so every directory is depth 1 and a
  colliding instance displaces. Handing either writer a deepened root would
  leave the row at the old root and depth: a miss for every key, not an
  error. So a deepening needs a replace under page 9's hold and a move of the
  schema word. Not live; `waystone_dir.hpp` carries the trap. Restated on
  `cf-waystone-premise` at `46c632aa`. Owner:
  `docs/spec/waystone-concpets.md` §5.

- **A Cabin builds rarely on a busy instance, and that is the price of one
  store.** §6a's banking gate refuses while anything is unresolved anywhere
  (`TransactionManager` sets `in_flight_at_mint` from
  `InstanceVisibility::AnyUnresolved`), so one autocommit write in flight on
  any core declines every build on every core. Correct, and conservative in
  the only legal direction (an unobserved value is answered by the
  authoritative scan), but a Cabin may never fill on a write-heavy instance;
  `SHOW CABINS` reports `unbankable_views=` and nothing else tells that from
  "never probed". Sharpening it needs a per-value record of which unresolved
  transactions could contradict a build, a structure §6a does not have.
  Re-verified at `7dd9b789` by reading. Owner: `docs/spec/cabin.md` §6a.

## Foreign keys

- **A statement that fails for any reason but a foreign-key violation keeps
  every absent parent's `S` it resolved.** Found by the review of AZ-R5's
  second amendment on `worktree-az-q3b-release-every-absent-parent` at
  `e2340c4`; read, not run. The hoist takes the parents' `S` before any row
  is written, and only a failed foreign-key check gives the absent ones back
  (`foreign-keys.md` §2c). In `BEGIN; INSERT INTO c VALUES (5, 10), (6, 98)`
  where `c` row 5 exists, row 1 fails on its key and `S(98)` stays to the
  rollback; an assertion refusal or the cap mid-hoist does the same. **Cost:
  a refusal, bounded by the client's rollback**, never a wrong answer.
  Giving them back at the statement's failure exit is the same argument,
  and **the operator's** to mark. **Owner: none.**
- **The borrow ledger's `Holds` is a linear scan, and D9(a) asks it more.**
  Every parent `S` and the `IS` above it go through `BorrowChain`, whose
  intention test scans the transaction's holdings, so a transaction's asks
  are quadratic in its borrows. Measured at AY's close (`v2.7.0-530-g0552d55`,
  one core): ~0.2 ns x K a row for K distinct parents, +3.43 us a row
  (~6 %) at K = 16,384, of which ~2.1 us is paid at the decide. By source
  read, not profiled, that part is `LockTable::Release` (`ReleaseHeld` erases
  from the front of a partition vector, `WakeWaiters` rescans it: O(n^2/64)
  a decide). A keyed partition would remove it; a hashed holdings set would
  not. Neither is built; the numbers are not re-measured and their results
  file is not in the working tree. Source structure re-read at `7dd9b789`.

**The check-to-write window closed at AY-S5** (D9(a), `foreign-keys.md`
§2a, §3a), pinned on one thread by
`FkParentHoldTest.AParentDeletedBetweenAChildsCheckAndItsWriteIsRefused`.

## Multi-core state, continued

**A volume written before `1b5d252e` at `cores > 1` no longer mounts**
(closed at BD-S2, `62470f56`): superblock 20 and later refuse version 19
(BD-R8, `heap-and-tuple.md` §4.1).

- **Two concurrent `CREATE ASSERTION`s can place `sys.assertions` rows out
  of issue order.** By reading, on `bb-s0-order` at `bddd450c` (BB §1.8),
  re-read on `worktree-bb-issue-under-the-leaf` at `6dc792c9`; no cell
  reproduces it. `sys.assertions` is the one system relation whose rows carry
  a Keystone word. Its id is issued before the build
  (`src/exec/assertion_catalog.cpp`, `AllocateRowId(kSysAssertionsTable)`) and
  placed after it, by `EncodeRow` and `ChainInsert` under the
  root page's hold (`catalog.md` CT7) - two latched spans, so two
  creates on two cores can place out of issue order: defect A's shape, on a
  system relation. Across a tail-page boundary the one issued first and
  placed second is refused `OutOfRange` after its build. **Not read**:
  whether any reader depends on `sys.assertions`' key order. BB fixed the
  user relations only (BB-R9, on BB-Q5: system relations out). No orphaned
  page was ever reachable here: the insert holds the chain's root exclusive
  across `ChainInsert` (CT7, since AT-S17), so two cores never grow this
  chain at once. Owner: `assertion.md` §7.

- **An unexplained ~0.4 s stall inside foreground time, on the post-AT
  engine only.** AT-S13 (`v2.7.0-391-gf6f2073`, 2026-09-26): in three of
  seven runs of the measured commit one `update-disjoint` arm took
  ~0.37-0.41 s longer than its siblings, inside `sched_foreground_polled_us`
  and once on every core at once; the three `df8cc5f` runs show none. An
  instance-wide hold or a WAL append behind device writeback under the log's
  latch would produce it. Not investigated, not re-measured. Owner: whoever
  measures next, from `wal.md` §3 and `page.md` §6.

- **A `SHOW CABIN_OPTIMIZER` can stall its core for a whole Cabin build.**
  Core 0's cadence tick (`src/server/expeditor.cpp`) holds the controller's
  view latch, a `std::mutex` (`include/kds/base/latch.hpp`), across a tick
  that may create a Cabin and walk its relation; a peer's `SHOW` takes the
  same latch (`src/server/sys_views.cpp`) and sleeps that reactor, and every
  session on it, for the build. Liveness, not correctness. The fix is for
  the tick to publish a copy of the view and the `SHOW` to read the copy.
  Re-verified at `7dd9b789` by reading. Owner:
  `docs/spec/cabin-controller.md`.

- **A multi-core test instance binds its probed port with SO_REUSEPORT.**
  `ExpeditorTest`'s fixtures probe a free loopback port and then bind it;
  above one core every listener sets `SO_REUSEPORT`, so two concurrent ctest
  processes that probe the same port both bind it and exchange connections
  silently (`tests/expeditor_test.cpp`'s `FreePorts` comment states it).
  Unobserved; a flake, not an engine defect. Re-verified at `7dd9b789` by
  reading. Owner: the test fixture.

- **An eight-core append workload reaches the clustered tree's bounded
  re-descent refusal, and the cause is not established.** The scenario
  rebaseline (`bench-rerun-scenarios`, `v2.7.0-531-g9a0525d`, 2026-09-30)
  saw a write descent restart `storage::kMaxDescentRestarts` (4) times and
  refuse `TXN_CONFLICT retryable=1 ... gave up after 5 attempts`
  (`src/storage/btree/btree.cpp`): scenario0 `cores = 8` `strict` on one
  `trades` insert in 3 of 3 runs (key 113, root page 138 each time) and
  scenario2 `cores = 8` `group` three times, all on fresh relations under
  eight appending sessions. The message names split churn or a descent from
  a memoised root that has since grown a level; the repeated key and page
  fit the stale root, unproven. Since BA-S14 (`6aa76ec7`) a statement that
  wrote nothing before the refusal is re-run inside the engine, so that case
  no longer reaches the client; not re-measured. No cell isolates which
  cause fires (a grown-over root carries a mark `SecureParents` reads and
  `DescendTo` does not). The cited results files are not in the working
  tree. Constant and message re-verified at `7dd9b789`. Owner:
  `docs/spec/heap-and-tuple.md` §5.

- **No cell drives a stale writeback against a re-faulted frame.** AT-S8
  step 1b draws every frame's dirty generation from the store's own counter
  (`++dirty_gens_`, `src/storage/device_page_store.cpp`), so a frame evicted
  and faulted back never repeats the value a concurrent writeback recorded at
  its copy; the interleaving needs two writebacks and an eviction inside one
  device write, and no cell forces it. Re-verified at `7dd9b789` by reading.
  Owner: `docs/spec/page.md` §6.

## Keystone ids

- **An engine-internal purge of a user relation still owes a keyed
  tombstone, and none is built; `PURGE` frees a key by design.** Verified at
  `26766699` (BH-S3). A committed key stays bound because its delete-marked
  row stays in its leaf - the tombstone BD-R4 rests on - until the operator's
  `PURGE` retires it (`heap-and-tuple.md` §4.1c), K1's one named exception.
  Nothing else retires a delete-marked user row (`Catalog::RetireDeleteMarks`
  walks catalog chains only). **What an engine-internal purge would owe**: a
  keyed, never-visible tombstone in the row's key position, which the
  duplicate check and the placement order read and every walk and
  `VerifyTupleAt` treat as absent - or it reclaims nothing. Owner:
  `docs/rules/keystoneid-invariant.md` K1, `heap-and-tuple.md` §4.1.

- **What bounds `PURGE`** (BH, `instructions/v3.0.0/workorder-bh-purge-key.md`
  §0), verified at `469e0b92` (BH-S4):
  - **One open older snapshot blocks every `PURGE` of a key deleted after
    it.** An idle REPEATABLE READ session refuses each such `PURGE` at
    `exec::kPurgeHorizonWaitNs` (1 s), `TxnConflict retryable=1`.
  - **Nothing wakes its wait.** Neither a horizon advance nor a deleter's
    decide kicks the waiting core: the wait polls, one of "what still
    polls" (Locks, below), and a wide window is re-walked from its low key
    on every attempt.
  - **A refusal or a crash in the middle of a window leaves it partly
    purged**, up to some key in key order (each key whole); the refusal's
    text says how many, and a re-run finishes the window
    (`PurgeKeySqlCrashTest`, `PurgeKeyTest.ARefusalWhileWriting...`).
  - **A crash or a refusal between a key's retire and its spill release
    leaks that row's spills**, which no sweep reclaims (the mount sweep
    covers `sys.assertions` only). Spills of versions older than the
    tombstone, in the undo chain's keeping, were already leaked before BH.
  - **The leaf space a purge frees comes back only when the leaf fills**:
    a retired slot keeps its directory entry until a full leaf is compacted
    before it would split (`btree::CompactLeafAndInsert`, BH-S5) or
    divided, so a thinned leaf holds its slots until an insert needs them.
  - **A wide window collects every target before it writes**, so its
    target list grows with the window.
  - **A row `PURGE` removes no secondary-index entry** (BH-Q8 (a)); a key
    placed again appends beside them (`index.md` IX4b), and `PURGE INDEX`
    (`index.md` §2.2) takes the purged version's out.
  - **The audit claim carries the exception**: a key is never reissued
    except by an explicit, logged `PURGE`, and an external system that
    captured a Keystone id must treat a purged key as a new identity.
  - **Every future read view inherits the horizon obligation**: a view kind
    that can read a superseded user row must be registered with the horizon,
    read latest state only, or be excluded by a relation `X` - or a `PURGE`
    may retire a row it is entitled to read, a wrong answer with no error
    (`heap-and-tuple.md` §4.1c; BH-S1's Census A).
  Owner: `heap-and-tuple.md` §4.1c.

- **`DELETE` refuses a system relation only by accident.** Verified at
  `80a0c223` (BH-S1, Census D). `DELETE FROM sys.tables` resolves the name,
  and `InitTableAccess` answers `NotFound` *"no columns for this rel_id"*,
  because a bootstrap relation has no `sys.columns` rows (`catalog.cpp`).
  The refusal would change the day the catalog learns its own columns, and
  `NotFound` says the relation is absent when it is not. `PURGE` refuses one
  explicitly, `Unsupported` (BH-Q17); `DELETE` and `UPDATE` do not.
  `PurgeKeyTest.CensusDADeleteOfASystemRelationIsRefusedOnlyByAccident`
  pins the accident. Owner: `heap-and-tuple.md` §4.

- **An omitted tuple key on a btree can be refused `AlreadyExists`.** Verified at
  `91c998a3` (BD-S5), by reading `CommandDispatcher::InsertOneRow`. The id
  is issued before the descent, under no leaf's hold (BD-R6), so a named key
  equal to it can be borrowed, placed and committed first; the placement's
  duplicate check then burns the id and the issue draws again, at most
  `kMaxIssueRounds` (8) times. A statement losing every round is refused
  with the leaf's bare duplicate text and no byte - an `INSERT` that named
  no key. Pinned for one round by
  `IssueUnderTheLeafRig.AnIssuedIdANamedKeyPlacedFirstIsDrawnAgain`; eight
  in a row needs a client naming each next id as it is issued. Owner:
  `heap-and-tuple.md` §4.1.

- **The whole-relation walk is 1.2-1.8 % slower at 48 bits between fair
  placements of the two binaries, and no call accounts for it.** Verified
  at `efa6adba` (BK-S2) against `5979b5e7`,
  `bench/v3.0.0/results-bk-close-v2.7.0-767-gefa6adba.md` §3:
  `COUNT/MIN/MAX` over a 100-id window, which walks every row, reads
  +1.7 µs [+1.5, +2.2] at 1,000 rows and +12.1 µs [+10.6, +16.0] at 10,000
  between fresh copies of A and B - 1.2 ns a row of a 74 ns walk, server
  work - and up to +5.3 % from B's file in its worst placement, while a
  byte-identical copy of B read 2.1 % faster than B's file and two copies
  of A agree to 0.05 µs. No write or point read moved (-0.85 to +0.10 µs at
  p50). B executes one call fewer per decoded row than A (`RowKeystoneId`
  deleted), so the increment is not the code BK-S2 wrote; the hypothesis
  that fits is code placement (B's binary is 16 KB larger), which a stage
  build would price and `perf` (locked out, `perf_event_paranoid` = 4)
  would attribute - neither was run. Reported, not resolved, and gating
  nothing (BK-Q4). The next walk measurement needs fresh copies of both
  sides and a same-bytes copy as the null (the file's "Baseline for the
  next run"). The merged tree (`a45311b5`, BJ's review code over B) is not
  measured. Owner: `docs/spec/heap-and-tuple.md` §4 (the Keystone word)
  and the results file above; BK's order
  (`instructions/v3.0.0/workorder-bk-keystone-48.md` §6, BK-S4) records how
  it was found.

## Locks

- **A named key meeting another transaction's undecided insert past 2^16
  versions is refused rather than waited on** (a review finding next to the
  audit's G21, verified by reading at `0304a822`). `UndecidedInsert`
  (`command_dispatcher.cpp`) asks down a chain of in-flight versions at most
  `kMaxUndoChainLength` steps before answering "not an insert", so a
  transaction that inserts key `k` and then updates its own row 2^16 times
  leaves a chain that a second session's `INSERT` naming `k` reads as a
  committed row: refused `AlreadyExists` at once, where BD-Q9 (a) says it
  waits for the decide - and wrongly if the first transaction rolls back.
  Never a wrong row. The operator's G21 ruling kept the cap ("as
  proposed"); a cycle check would close it, every record of such a chain
  being an in-flight writer's and so never recycled.

- **A write refused by a lock unit waits on that unit's slot, and a wake is
  not a grant.** A refusal on the ask's own unit waits on its slot since
  AX-S2b, one in another unit since AY-S2, and a first encounter with a
  holder between its decide and its release since AY-S3
  (`tests/row_wait_wake_rig_test.cpp`). What stays open, retryable and never
  a wrong answer:
  - **A wake is not a grant.** `TryAcquire` does not queue, so a third
    writer can take the unit between the flip and the re-run; under
    sustained contention the waiter ends at the 1 s fault net naming the
    last holder.
  - **What still polls** has no slot behind it: no lock table, a holder only
    the header names, an assertion's group, and a `PURGE` waiting on a
    deleter's decide or the read horizon (`heap-and-tuple.md` §4.1c). The
    unique check's busy holder left this list at `f1ce9953`: its busy arm
    probes the named row in `U` (`WaitForRowWriter`, the reverse foreign-key
    walk's probe), so it parks on the wake of the unit the holder writes the
    row under (`index.md` §10.2). A holder that writes the row under no
    unit, which only a dispatcher with no lock table does, still polls.
    **A granted probe - the holder has released - waits for the snapshot
    ceiling** where the holder committed above it, kicked at the marker's
    lift (`foreign-keys.md` §3a; on `worktree-fk-walk-ceiling-wait` from
    `d9f89624`, `RowWaitCeilingRigTest`'s four cells that end at the lift,
    red before it: the three foreign-key cells at the 1 s net, the unique
    cell refused at once; a fifth pins the refusal a ceiling wait gets at
    the net). A holder still in flight when its row's probe is granted
    polls `IsInFlight`; no cell reaches that arm and no writer is known to
    take it.
  - A holder that never decides costs a cross-core writer the 1 s fault net
    and its defect warning, as a same-core writer pays.

  Re-verified at `f5d310ec` by reading (`TryAcquire` non-queueing, the rig
  cells exist); the unique check's `kBusy` arm at `f1ce9953`, by the four
  `RowWaitWakeRigTest.ABusyUniqueKey*` cells, red before it. Spec:
  `docs/spec/txn.md` §5.

- **The decide slot is designed and not built** (CP's C4,
  `instructions/v3.2.0/workorder-cp-lock-model-exploration.md` §3, §8.4).
  One slot per in-flight transaction, flipped where `Release` runs today
  (`FinishCommitBody` after `RetireInFlight` and the schema word; `Abort`),
  would put a wake behind every entry of "what still polls" but the
  no-table case. No number asked for it: at CP-S0 the family's whole share
  of a point `UPDATE` was about 0.4 µs of 39 µs, every parked ask in
  scenario 2, scenario 0 and the hot-row cell was granted, and no wait
  ended at the fault net. Its second half - the row's table entry
  published only while a fence stands - carries a `[loud]` risk: a fenced
  bulk statement that today waits before its walk would meet an undecided
  row mid-walk and be refused retryably. CP-Q4 (a) marks the form that
  avoids it, (α): the fence-taker waits out the statements that began
  before its publish, so no wait becomes a refusal. It is a design for its
  own order. Read at `f5d310ec`; nothing of it is in the tree.

- **The per-relation lock header is not built, and its eight-core number
  is not measured** (CP's C3, the order's §3, §8.4). A standing range
  fence costs every point write of the relation one walk of the 64
  partitions: about 1 µs a statement at four cores at CP-S0
  (`bench/v3.4.7/results-cp-s0-premise-v3.4.7-1-gcc54a296.md` §7), against
  a point write whose own floor is about 36 µs (CP-Q12 (a) reads the rule's
  "floor" as the write's). BP-H10's +12 % of a statement's CPU at eight
  cores was not reproduced because eight cores were not run, and the
  four-core cells barely exercised cross-core partition contention. A
  relation header holding its entry and its fences under one latch is the
  design; it is re-proposed as its own item only if an eight-core run
  reproduces the figure. Read at `f5d310ec`.

- **A parent `UPDATE` no longer waits for child writers, and one class of
  deadlock replaced the one that removed** (CP-S2b, `766f6a6d`; `txn.md`
  §5, `foreign-keys.md` §2c). **Closed**: the parent `UPDATE`'s wait on
  every open child writer's `S`, and the deadlock of two transactions that
  each wrote a child of P and then updated P - they serialise on P's `U`
  (`FkCrossCoreRigTest.TwoChildWritersThatThenUpdateTheirParentSerialise`).
  **Open by the operator's acceptance** (CP-Q10 (a), `[loud]`): a
  transaction granted `U` on a parent row over a child writer's share,
  that then waits on that child writer for any reason, deadlocks with it
  if the child writer then writes the parent row; one of the two is
  refused retryably naming deadlock, where before CP-S2b the updater
  waited at the parent row first. Pinned by
  `FkCrossCoreRigTest.AnUpdateThenADeleteOfAParentUnderAChildsShareWaitsAtTheDeleteAndDeadlocks`
  (the same row) and
  `FkCrossCoreRigTest.AParentUpdaterThatThenWaitsOnTheChildWriterElsewhereDeadlocks`
  (another row). Two child writers that then `DELETE` their parent still
  deadlock (`...TwoChildWritersThatThenDeleteTheirParentDeadlock`): `X`
  keeps that shape. Cells ran green in the suite at `766f6a6d`.

- **A hot row's client-visible refusal is not a lock's.** At CP-S0, eight
  sessions on one row had 7.9 % of their `UPDATE`s refused `TXN_CONFLICT`
  by `txn.md` §5's first-updater-wins - a version committed after the
  statement's snapshot, READ COMMITTED's no-re-read window - while the
  family parked 17 asks and granted every one. No lock cell reaches it and
  the decide slot would not either; removing it is a re-read loop, an
  isolation-level change no order has taken (the CP order's §8.5).

- **The relation `IS` is declared at the bind for every relation a
  statement names** (closed at AT-S1, 2026-09-09; `step_compiler.cpp`). What
  stays is one-directional by design: the ask is a non-queueing
  `TryAcquire`, so a reader arriving while a DDL holds `X` reads on, sound by
  catalog MVCC and the statement epoch rather than by the lock
  (`include/kds/server/read_borrow.hpp`, `txn.md` §5, `drop-table.md` DT1).

- **A resolution `ResolveNames` discards keeps its `IS`.** Read 2026-10-14
  at `7ad21023` by the reviews of the instance-wide open-DDL gate; still so
  on `worktree-fix-names-resolved-with-no-view` off `01d04fa8`. When the
  generation moved and the name is resolved again under a view, whatever
  `IS` the first compile declared on the relation it bound is held until
  the statement ends. That relation was never bound for use, so no answer
  is wrong; a DDL wanting its `X` waits for the statement. Owner:
  `docs/spec/ddl-transactional.md` §4.

- **A DDL that opens and closes inside one statement leaves that
  statement's cache window open.** Read 2026-10-16 on
  `worktree-fix-names-resolved-with-no-view` off `01d04fa8` by the
  `critics-developer` review. The head records the open-DDL count before it
  revalidates (`ViewFor`), which covers a DDL that closes across the head.
  A DDL that opens after the head and closes before a later resolution in
  the same statement leaves both readings zero. A cache entry filled while
  it was open - the duplicate check, the name renderer, or a resolution
  `ResolveNames` discarded - is then served. Not reproduced; the cross-head
  form of the same window, run by
  `CatalogNameRigTest.AStatementHeadThatRevalidatedBeforeARollbackDoesNotBindItsRelation`
  with the head's reading removed, answered
  `ERR no sys.tables row for this oid`: a refusal with a wrong
  reason, not a wrong row, because oids are never reissued and a reclaim is
  gated on the statement epoch. The full close is `ViewFor` taking the fast
  path only while `Catalog::MemoIsCurrent()` holds. It went red on
  `InRunReclaimTest.ABindPastAGateDefectIsRefusedByTheRootCheck`, which
  binds through a stale memo to reach BF-R10's root check, so taking it
  needs that cell to reach the check another way. Owner:
  `docs/spec/ddl-transactional.md` §4.

- **A KWP load chunk refused by a relation `X` rolls back the whole
  load.** Accepted by the operator on 2026-10-21. A chunk runs
  `ExecuteInsert` outside `DispatchAsync`, so it cannot park: a relation
  `IX` refused by an index, assertion or transition build's `X` - on an
  index build's own core too, between its slices, since A1 stage 2 - or by
  a rename's (`alter.md` AL4a) is refused `TxnConflict`, and
  `HandleLoadChunk`'s `fail_load` rolls back the load's one transaction,
  chunks already acknowledged included (`kwp_load_server.cpp`). Once a
  chunk has written, the load holds the `IX` until it decides, so a DDL
  arriving later waits for the load instead. Owner: `docs/spec/txn.md` §5.

## Page reclamation

What BF (`instructions/v3.0.0/workorder-bf-drop-table-page-reclaim.md`)
leaves, each verified on `worktree-drop-table-page-reclaim` at `f2de416c`. A
dropped relation's pages are reclaimed (`drop-table.md` DT1); these are the
pages and the cases it does not reach.

- **Pages no root reaches stay leaked.** A reclaim frees what a tombstone's
  roots reach, so these stay allocated for the life of the volume:
  - a page left unlinked by a failed heap growth, btree split or var-heap
    growth;
  - a failed `CREATE INDEX`'s page that its freeing does not reach (BX-Q6
    (b), `index.md` §12): one a failed divide left unlinked, one whose
    frame a writeback held, every page of a tree the guarded walk refused,
    and every page after a crash before the map was written. A create
    refused **after** its tree is logged is not here since BY: its slot is
    owed and an index job reclaims it (BY-R1, BY-R2) - unless the slot's
    best-effort seed itself failed, which leaves the tree no slot names.
    Verified on `reindex-drop-index-reclaim` at `4f55b8a2`;
  - a failed or rolled-back `CREATE TABLE`'s pages, which no tombstone
    names;
  - a refused catalog report's page (WAL, above);
  - a previous run's undo pages (UP4, `txn.md` §4.1);
  - a dropped assertion's Bound Cabin chain;
  - Waystone pages.
- **Every restart, and every `WAYSTONE OFF`, leaves Waystone pages
  allocated.** Narrowed by CF-S1b; verified on `cf-waystone-premise` at
  `46c632aa` (first verified on `fix-sync-publish-and-waystone-crash` at
  `ec440c2d`). Every mount clears every directory root and gives each opted
  pattern a fresh one (`server::DiscardAdvisoryState`,
  `waystone-concpets.md` §4c), because
  after a crash a directory page can name a child whose allocation bit never
  reached the device. Only opted patterns have directories now, but the leak
  is not bounded per mount: each mount strands every opted row's previous
  directory (a root and up to 2048 trail pages), and each `OFF` strands that
  row's, so the volume grows with mounts and with toggles, clean or not. A
  page `WAYSTONE ON` made and did not use (a concurrent `ON` rooted the row
  first, the row was found opted with a root, or the write failed) is
  logged `leaked by WAYSTONE ON`, and a page the renewal's second pass did
  not use `leaked by the mount's renewal`; neither is freed.
- **At `cores > 1`, a reclaim waits for every core's first checkpoint.**
  The durable redo start does not move until every core has published, so
  with `checkpoint_interval_ms = 0` nothing a mount found is freed until a
  clean shutdown's checkpoints. This is BF-R4's stated cost, and BF-S1's
  premise cell is what showed the arm is needed.
- **A crash forgets the free list.** After a crash the allocation floor
  hides the run's freed ids below it for the next run; a clean restart
  finds them again. The undo log's recycle list states the same loss.
- **A drop committed during a run is freed at the next checkpoints, not at
  once.** Its gate is the durable redo start past its commit, which takes
  a checkpoint that finds the dropped relation's dirty frames written back
  - about two cadence intervals.
- **A refused reclaim stays pending.** A reclaim refused - a page reached
  twice by the walk, an unreadable page, a failed free or map sync - leaves
  the tombstone owing for the rest of the run. It is driven again at the
  next mount, and each refusal is counted `reclaim_refused`.
- **`SHOW PAGE` of a reused id shows its new owner's bytes.** It reads any
  page id a client names.
- **BF-R10's per-page owner checks inside a descent, a chain walk and a
  var-heap fetch are not built.** The check is made once per bind instead:
  a relation's anchor and first page must still carry its oid. The
  verifier also checks every remembered location. Neither is the
  authority, which is the replay gate and the statement epoch.
- **An orphan Cabin set lives until a restart.** A probe compiled before a
  drop may bank a set of the dropped relation's locations after it; only a
  stale memo naming that `cabin_id` can reach it, and the statement epoch
  covers that. A rolled-back `CREATE TABLE` whose `CABIN` column observed
  values leaves its set the same way: the rollback retires the
  `sys.cabins` row and nothing forgets the set, but no row names the
  `cabin_id` afterwards and the id is never reissued. Read on
  `fix-rolled-back-cabin` off `f2ef45c1`.
- **A tree freed in one pass would pass every cell.** On a volume of one
  map region, a map flush writes the region's one page whole, so the
  per-level syncs matter only to a tree spanning regions - past 65,280
  pages - and no cell builds one.
- **A `REINDEX` on a relation whose anchor is full is refused.** The new
  tree needs a slot while the old one's is held until the reclaimer frees
  the old tree (BY-R5), so at `kAnchorMaxIndexEntries` slots the rebuild
  is refused `ResourceExhausted` before any page is built, until the
  reclaimer's jobs remove dropped indexes' slots (`index.md` §12). Verified
  on `reindex-drop-index-reclaim` at `9da66f0e`
  (`AtTheSlotCapARebuildIsRefusedAndTheOldIndexStands`).
- **A `REINDEX` whose backfill fails is not tested on its own.** It goes
  through `CREATE INDEX`'s build (BY-R7), whose BX-S6 free is tested
  through a unique build over duplicates; a `REINDEX` cannot meet that
  failure - the old unique index already refused the duplicates - and no
  other backfill failure is reachable by a cell without exhausting the
  pool or the device. Verified on
  `reindex-drop-index-reclaim` at `9da66f0e`.

- **A backfill can be refused, or index a stranger, through a recycled undo
  record.** Between `ResolvedForEveryReader`'s check and the read, another
  core's purge can recycle the record, and a new record holding another
  relation's row with the same tuple key passes the backfill's identity
  check: its append is refused `Corruption` or leaves a surplus entry
  readers re-check (IX2). Never a wrong answer; no cell forces the window.
  Re-verified at `7dd9b789` (`index.md` §10.1 states the same).

## Statement scheduling

Verified at `0064d8bd` (BA-S15). Owned by `docs/spec/sched.md` §3 and
`docs/spec/protocol.md` §10.

- **Only a `SELECT`'s outermost walk and a relation-`X` build (or an online `CREATE INDEX`) yield.**
  The walk yields every `exec::SlicePolicy::pages_per_slice` (64) pages, a
  `CREATE INDEX`, `REINDEX INDEX`, `CREATE ASSERTION` or `CREATE
  TRANSITION` build every 64 relation leaves (`CREATE ASSERTION` and
  `CREATE TRANSITION` since A1 stage 2b, `IndexBuildSliceRigTest`).
  Nothing else running gives its core back:
  - a write;
  - a tuple key descent, run inline by design, and a key set's descents
    (CU-Q8; "Key sets (CU)" below states the bound);
  - an index walk (`RunIndexStep`);
  - a join's inner walk under one outer page.

  Each holds its core until it ends or `exec::Budget` stops it. 64 is
  BA-Q11's value, not re-measured in BA-S15.
- **A page read still stops its core for every reader but two** (DE,
  verified at `1942cb9a`; `page.md` §12). A read that would have to wait
  for the device leaves the reactor only for a statement that is itself a
  plain `SELECT` whose head is a descent run inline, and for a `SELECT`'s
  outermost walk awaited on a reactor. A blocking `pread` on the reactor,
  for the device's whole time, is still what these make:
  - a write and its read phase - an `UPDATE`'s or `DELETE`'s locate, an
    `INSERT`'s descent, a foreign-key or unique check (DE-Q7 (a): an issued
    key's first page write comes before its descent, and a write path's
    callers recover from a failed fetch in ways a refusal cannot pass
    through);
  - a locking read, a `SELECT` run inside another statement, a traced
    (`ANALYZE`) `SELECT`, a walk the query layer or a write's subquery
    drives to its end where it stands;
  - a walk's own descent to its first leaf (`BtreeSeekLeaf`,
    `BtreeLeftmostLeaf`), and every fetch under a walk's leaf - an undo
    chain, a spilled value, an index walk's base fetch, a join's inner step;
  - a build's walk, `GATHER STATISTICS`, recovery and every background
    task.
- **What the read that leaves costs, and where it stops** (DE, verified at
  `1942cb9a`):
  - a descent's statement is **run again from its head** for each page it
    is refused, at most eight times (`CommandDispatcher::kMaxMissReruns`)
    and inside the fault net; after either, and after a cancel, it reads
    the rest synchronously. A head with more cold pages than eight - a long
    key set, a probe into a cold inner relation - blocks its core for the
    remainder, as before DE. Each run is parsed and compiled again, and
    `sys.statement_stats`' per-shape page reads and rows count the
    discarded runs, as BA-S14's re-runs do;
  - **the page parks and every other wait of the statement share one
    deadline**: a `SELECT` that spent the fault net parked on pages and then
    meets a relation wait (a DDL's `X`) is refused `TxnConflict` at once,
    sooner than before DE;
  - a park's deadline, like every wait's, is read when the reactor turns:
    on an otherwise idle core it is late by up to `max_idle_block_ms`;
  - the path rests on the operating system's page cache: the reader's
    thread leaves the page there and the statement's next fetch copies it.
    Under memory pressure that drops the page in between, that fetch waits
    for the device on the reactor after all - a slow read, never a wrong
    one. `O_DIRECT` is not built, and with it this path would leave nothing
    behind;
  - on a file system whose `RWF_NOWAIT` can answer "would wait" for a lock
    rather than for the device (XFS's inode lock), a statement is parked
    that need not have been. ext4 is what was measured;
  - `PageReader::kPageReadThreads` is 4, `[provisional]`: no cell has
    priced another number, and reads past four in flight queue behind
    them;
  - at `cores = 1` the instance now runs the reader's threads and core 0
    has a wake descriptor in its readiness set, the two departures from
    "build nothing at one core" DE-Q14 names.

- **What DE costs work that never misses** (measured at the close and its
  spike, `bench/v3.4.14/results-de-close-v3.4.14-6-g1942cb9a.md`; A is
  `35086008`, the commit DE opened from):
  - a resident point `SELECT` at `cores = 1` costs about 0.2 to 0.4 us more
    server CPU a statement at the closing code than at A (medians of four
    server lifetimes, 20 to 24 us a statement; +0.33 to +0.66 us at
    `1942cb9a`), where two servers of A's own binary differ by -0.18 to
    +0.02 us and a single lifetime by up to 0.6 us. A build with no page
    reader, no arm and no one-core wake table carries the same, so it is
    not those; it is not separated from DE's per-statement code - the
    refusal's admission test, the note's read, a larger `DispatchOutcome`
    and `SelectRun` - or from binary layout, on a host with no PMU;
  - a resident walk read 8 % fewer pages a second at `1942cb9a` than at A;
    at the closing code it is within about 1 % (two runs each, ranges
    overlapping). What the 8 % was is not isolated;
  - a cold point `SELECT` that parks is itself 2 to 3 % slower at its p50
    (+13 to +21 us of about 640 us) than when it blocked its core, and
    spends 41 to 49 us more CPU: the refused run, the wake and the run
    after it;
  - `pool_misses` counts a refused fetch and the fetch after it, so it is
    about two a cold read where a read parks, not one; `io.page_read` is
    still one a device read.

  Not covered: a `tsan` build of `tests/de_page_read_rig_test.cpp`; the
  park's own `io.page_read` count on a reactor (the synchronous loop's is
  pinned); a re-run over a real KWP sink (a counting session sink pins the
  rewind); a session closed while its read is in flight.
- **A yielding build still stalls its core at a WAL segment roll.** A
  roll holds the append latch across its sync (the WAL entry above), and a
  build that logs per row - `CREATE ASSERTION`'s `ASSERT_BUILD`, about 129 B
  a row - crosses segments inside a slice: at 1,000,000 rows on `cores = 1`
  its two rolls held a same-core point read 529 and 540 ms, where every
  other probe was answered within a slice (`CREATE INDEX` logs its tree
  only at the end; `bench/v3.4.0/results-a1-close-v3.4.0-11-gf53e62db.md`
  §7). The slice bounds everything but the roll.
- **A live build's waiters can still meet the fault net.** A relation-`X`
  build's waiters on other cores are not refused at the fault net while
  it advances (`txn.md` §5, `holder_progress.hpp`).
  A build advances only while its core runs it, so a co-located statement
  that does not yield, or many sliced statements queued on that core, can
  leave it silent past the net, and its waiters on other cores are then
  refused `TXN_CONFLICT` though the build is live. A rename registers and
  never advances, so its waiters always have the plain net.
  Not covered: `CREATE ASSERTION`'s and `CREATE TRANSITION`'s waiters
  have no fault-net cell of their own - they share `DdlBuildLock`'s
  registration, `IndexBuildSliceRigTest`'s stage 2b cells drive a peer
  writer that waits for the publish but not past the net, and
  `DdlFenceRigTest.AWriterOnAPeerOutlivesTheFaultNetBehindAnAutocommitBuildOnCoreZero`
  drives `CREATE INDEX` only. Nor is an assertion adopted after a real
  yield with a checkpoint between its slices: `AssertionPublishRigTest`'s
  relations fit one slice, so its publish seam runs in a tail that never
  yielded.
- **A cancel is observed only where a walk or a relation-`X` build
  yields, or at the next frame.** A write that `C_CANCEL` or a closing connection asks to stop
  runs to its end, and the cancel then refuses the session's next frame.
  So a dropped connection can still find its write applied.
- **A core holds at most 256 suspended walks.** Each holds one of its
  manager's `txn::kMaxRegisteredReaders` reader-lease slots across its
  suspensions. The 257th walk suspended at once on one core is refused
  `OutOfSpace` at its head.
- **What the suite does not cover:**
  - **A `strict` commit beside a suspended walk.** A deferred commit now
    names its `DispatchAsync` call, and that fix is covered by reading
    only.
  - **`C_CANCEL` end to end through `TcpServer`.** Registration,
    unregistration at close and at `Detach`, and capability negotiation
    are covered by reading only. The cells drive `KwpSession` and the
    registry directly.
  - **The teardown cell kills its mutant only under AddressSanitizer**, so
    only outside the push gate.
    `WalkSliceRigTest.ACoreTornDownWithAWalkBetweenSlicesReleasesItFirst`
    passes in Debug with `Scheduler::DiscardTasks`'s call removed; under
    BS-S1's `asan` configuration, which the nightly runs (`scripts/bs/`),
    the mutant dies of a heap-use-after-free 5 runs in 5 (verified on
    `bs-test-hardening` at `0e9ea02e`).

## Placement per transaction (BL)

Verified on `worktree-bl-session-balancer` at BL-S3's commit, measured in
`bench/v3.0.0/results-bl-s1-placement-premise-v2.7.0-857-g3552a03c.md`;
the first bullet re-read on `worktree-bl-touchpattern-delete` at `05cd011f`
and re-priced at BQ's close (2026-10-12, from `4cf30270`'s measurement);
the first bullet re-measured and the second added on `bl-ruling2-remeasure` at `418670a3`
(2026-10-21, `bench/v3.4.0/results-bl-ruling2-remeasure-v3.4.0-22-g418670a3.md`).
BL closed on 2026-10-22 (the order's §6, "BL closed 2026-10-22"), with
ruling 2 marked (a): core 0 is the first batch core.
Owned here since the close; the order (`instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`)
holds the history, and the mechanisms are `docs/spec/crosscore.md` CC1 and
`docs/spec/sched.md` §5.

- **A bulk writer stalls point readers through the segment roll; at
  `418670a3` a held placement that keeps readers off core 0 spares them
  most of it, and the shipped balancer does not hold one.** Two couplings through the one WAL were found at BL-S3's commit,
  where readers stalled 0.25-0.8 s. **The first is gone**: BL-S4's ruling 1
  (`raft-marks-2026-10-12.md` §11) deleted `Catalog::TouchPattern`, whose
  logged rewrite of a pattern's heat every repeated point `SELECT` made.
  **Re-measured 2026-10-12** by BQ-S1 under BQ-Q1 (a), `05cd011f` against
  `3338cf0c` in BL's C1 cell (`relaxed`, four cores, three placements,
  three runs each;
  `bench/v3.0.0/results-bl-ruling1-remeasure-v2.7.0-909-g4cf30270.md`):
  reader service p99 fell from 262-283 ms to 1.7-3.0 ms (medians), and the
  reader cores' stream-latch wait from 2.3-3.5 s per 10 s to 0-0.31 s; the
  schedule p99 stays 277-295 ms (medians) and the p99.9 and maximum do not
  move. **The second stands, and is what is left**:
  - **a segment roll holds the stream latch across device I/O**:
    `WalStream::Append`'s roll seals, flushes and runs `StartSegment`'s
    device sync, which waits out the writer thread's sync of every live
    segment, all under the latch (`wal.md` §6 step 1). Core 0's 10 ms
    group-commit drain takes the same latch, and core 0's stream-latch wait
    is 3.0-3.9 s of every 10 s at both commits.

  `Latch::lock` blocks the reactor, so every session on a core whose
  reader or drain meets a roll waits the roll out. **BL-S4's partition
  waits on the roll**, whose fix was the order's §6 ruling 3. BQ took it
  as its subject and answered it on 2026-10-12: the latch is kept, with its
  cost named (this file's WAL entry on the roll; `wal.md` §6 step 5; the
  first trace is in BL-S1's results archive, `wal-coupling.md`),
  so no fix to the roll is coming from BQ, and releasing the latch for the
  roll (BQ's rung 4) needs the operator's own word (BQ-Q5 (b)). Ruling 2
  is marked (a): core 0 is the first batch core while the partition is active.

  **At `418670a3` the stall lands mostly on core 0**: readers held off
  core 0 escape most of it, and readers on core 0 do not (the results file
  above).
- **BL-S3's balancer places readers on core 0 under a bulk writer.**
  `LoadPlacer::Decide` reads each core's load words (`busy_ppm`,
  `active_sessions`, the window end) and nothing else, so it does not know
  that core 0's reactor runs the group-commit drain. It separates two
  saturating batch sessions onto their own cores, and readers fill what is
  left, core 0 among it. 8 of 9 balanced runs at `418670a3` ended with a
  reader on core 0. **Since BL-S4 (`402317a2`) this holds only where the
  bulk writer declares no batch level**: a session that declares
  `analytic` or `background` makes core 0 a batch core and keeps every
  interactive session off it from its next head, but a writer that declares
  nothing leaves the partition inactive and the placement BL-S3's. Not
  re-measured since BL-S4.
- **Tools that pin a session by reconnecting until `SHOW META` names a
  core** (`tools/multicore_benchmark.py`, `lock_contention_benchmark.py`,
  `bi_checkpoint_stall.py`, `bb_concurrent_benchmark.py`, `bl_mixed.py`'s
  placements, `ba_census.py`'s arms) measure the balancer's placement on a
  BL-S3 binary: a pinned session may move at a head whenever
  `LoadPlacer::Decide`'s conditions hold (`load_placer.hpp`). No setting
  turns the balancer off; against a pre-BL-S3 binary (`3552a03c`) they
  measure what their docstrings say.
- **The staleness test assumes an idle reactor publishes often**:
  `LoadPlacer::kStaleNs` (four windows) holds while `max_idle_block_ms`
  stays under three windows (150 ms); the default is 10. A larger idle
  block makes an idle core read as held, and it draws no session.
- **BL-R5 is built only in part**: under D19's fallback core 0 still hands
  accepted sockets round-robin, not to the least busy core; since BL-S4 the
  round-robin passes over the batch cores while the partition is active.
  The balancer corrects the placement at a session's first head, so the
  accept-time choice was not separately measured.
- **A session's level is not observable per session** (BL-R8, BL-S4):
  `sys.meta` describes the instance and the asking session's core, not the
  session, so only `SET PRIORITY`'s reply names the level; the partition's
  instance numbers (`batch_sessions`, `batch_cores`,
  `load_c<k>_partition_moves_out`) are printed. Verified on
  `worktree-bl-s4-partition` at `402317a2`.
- **BL-R7's remainder: a declared partition does not bring a
  `transactional` reader inside 2x its isolated p99** (§5.2 bar (v),
  measured at BL's close on `worktree-bl-s4-partition` at `f4bad273`,
  `bench/v3.4.0/results-bl-close-v3.4.1-10-gf4bad273.md`): 6.5 ms median
  against 0.47 ms isolated. Where it was read, the excess is the checkpoint
  task, rotating over cores, holding a reader core's reactor on the stream
  latch during a segment roll. **Rung 3's probe was run on 2026-11-18**
  (`bench/v3.4.12/results-bl-rung3-probe-v3.4.12-24-g67973ebc.md`, on
  worktree `bl-rung3-probe` at `67973ebc`) and **rung 3 is not opened**
  under §5.2's condition:
  - The `levels` excess (12.2 ms against 649 µs isolated, on that host)
    is the checkpoint on a roll: all 227 reader samples at or above 10 ms
    overlap a checkpoint run, and 216 also overlap a roll hold.
  - Under the balancer, readers that connected beside the `normal` walker
    took no more slow samples than the others.
  - Forced onto one core with the balancer off, the walker costs a reader
    535 µs against 506 µs p99 on the medians. The paired runs, though,
    range from 0.91x to 3.14x, so this is not a clean bound.
  Whether rung 3 is ruled out is the operator's mark. **CZ's close
  re-measured bar (v) on 2026-11-18** (CZ-Q6 (a); worktree
  `cz-checkpoint-roll-wait` at `74861601`, `bench/v3.4.12/results-cz-close-v3.4.12-113-g74861601.md`):
  in the probe's `levels` cell with the cadence checkpoint parking on a
  busy stream latch the reader p99 is 1.55x isolated with 5 of 5 pairs
  within 2x, against 3.17x and 2 of 5 without it. **Bar (v) is met in
  that cell on that build.** BL's other mixed cells and placements are
  not re-run, and the cell was read on one host. What else v0 does not
  separate:
  - a `normal` session's statement that does not yield holds an
    interactive core from a `transactional` one, and a `background` write
    batch holds a batch core from an `analytic` read - the pick inside
    `foreground` is FIFO (rung 3 is unbuilt; BL-Q13 keeps yielding writes
    out of BL);
  - at `cores = 1` there is no partition, so a level acts by its walk slice
    alone;
  - a session waiting on a lock held by another level's session waits for
    it (inversion; BL-Q12's counter is unbuilt);
  - buffer frames, the log's sync order and the lock table ignore the
    level;
  - **the batch sessions share the batch cores**: an `analytic` walker
    beside a `background` inserter on one batch core completes about a
    twentieth of the walks it does with a core of its own.
- **What the suite does not cover:** a TLS or SCRAM session moving (the
  channel and gate travel inside the `Connection`, covered by reading only),
  a cancel sent to a session after it moved (the registry entry is keyed by
  session id and holds the session's flag; reading only), and bytes that
  arrive in the kernel between the source's unregister and the
  destination's register (level-triggered registration; reading only).

## Expressions in `UPDATE ... SET` (BJ)

Written at BJ's close on `worktree-bj-expression-update`. Owned by
`docs/spec/types.md` §3.2b and `instructions/v3.0.0/workorder-bj-expression-update.md`.

- **Refused by name at their byte, never parsed and dropped:** `IN`, `BETWEEN`,
  `LIKE`, `EXISTS`, `IS TRUE` and `IS DISTINCT FROM`; a boolean literal
  (`TRUE`/`FALSE`); `<>` (inequality is `!=`); a correlated subquery; a
  subquery over a catalog view; the row form `SET (a, b) = (SELECT ...)`;
  `DEFAULT` and `UPDATE ... FROM`; timestamp arithmetic (there is no
  `INTERVAL`); a negative count of places in `ROUND`/`TRUNC`; an expression
  that reads a column or the clock assigned to a foreign-key column.
- **An expression in `INSERT ... VALUES`, a select list or a WHERE** is not
  built (BJ-Q8): each is its own order on the same grammar.
- **Where it differs from the reference dialect, on purpose:**
  - **Exact types** (BJ-Q2 (b)): `int32 + int64` needs a `CAST`. The one
    departure is a scaled decimal (`ROUND`/`TRUNC`/`CEIL`/`FLOOR` over a
    decimal), which takes its context's precision.
  - `CAST('1.5' AS int64)` is 2; PostgreSQL refuses it. `CAST(18446744073709551615
    AS uint64)` is refused (a bare integer literal resolves to int64 first); `-128::int8`
    is admitted through the fused minus where PostgreSQL refuses both spellings.
  - Strings are bytes: `LENGTH` counts them and `UPPER`/`LOWER` change ASCII
    letters only.
  - **A decimal quotient has more places than its operands** and is never
    narrowed implicitly; whether PostgreSQL's own division rounds twice was
    not checked, and this engine's does not.
  - **A scalar subquery is run once, before the walk, wherever it is
    written**, so one in a `CASE` branch no row takes still runs and its
    error stands; and a WHERE-position subquery in the same statement is
    re-run per row and sees the statement's earlier rows.
  - A simple `CASE x WHEN a` is the searched form with `x` repeated, so a
    nested one multiplies its operand's size; the parser bounds an
    expression at 4,096 nodes counting a shared child per reference. Holding
    the operand once on the node is open.
- **What the suite does not cover:**
  - `NOW()` carried across a parked SET (the WHERE-position cell exists; the
    SET one is covered by reading, the same `ParkedWrite::context`).
  - **An autocommit UPDATE's two subqueries reading one snapshot.** The fix
    pins the UPDATE's snapshot; the race it closes needs a commit between two
    subqueries of one statement and has no cell.
  - A function's purity is declared nowhere: nothing reads it until AQ
    evaluates a function on the write path.
- **What it costs, measured at `e458e272`** (`bench/v3.0.0/results-bj-close-v2.7.0-757-ge458e272.md`):
  a literal point `UPDATE` is **+0.25 to +0.70 µs (0.5-1.3 %)** and a literal
  every-row `UPDATE` about +6 to +19 ns a row slower than before BJ; the share
  between the parser and `UpdateInner` is not attributed (`perf` is locked out).
  A constant-folding or lazier-allocation second attempt was not made.

## The read and update paths (BN)

Written at BN's close on `worktree-bk-read-update-paths`, verified at `8679367e`. Owned by
`instructions/v3.0.0/workorder-bn-opt-read-update-path.md` and the specs each
stage changed (`heap-and-tuple.md` §3.4, `types.md` §3.2b, `protocol.md` §5).

- **A prepared statement is still substituted into its text and parsed again
  at every execution** (BN-R1 not built, BN-Q13). Parse and compile are
  1.1-4.5 µs of a point statement's server CPU after BN (5-15 % of it,
  `bench/v3.0.0/archive/bn-close-v2.7.0-787-g8679367e/partsB.md`), and a
  client that prepares is the only one a cached parse would help; no driver
  in the tree prepares. A text parameter containing a single quote stays
  unbindable at `C_BIND` for the same reason.
- **Waystone records a trail for a point `SELECT` by tuple key only if an
  operator opted the shape in.** Closed for every shape not opted by CF-S1b
  and CF-S1c (verified on `cf-waystone-premise` at `46c632aa`): a `SELECT`
  nobody opted pays one atomic load while nothing is opted, or one hash miss
  while something is, and is never sighted (`waystone-concpets.md` §4b).
  BN-S1's census put recording and the replay attempt at 7-9 % of a point
  read's server CPU, and CF-S0 at 2.7-3.9 %
  (`bench/v3.3.0/results-cf-s0-premise-v3.3.0-97-g3d628df4.md`); an opted
  point read still pays it. The gate's own saving is unmeasured until
  CF-S2's close.
- **The request path's floor is the kernel's**: a `C_PING` round trip costs
  12 µs of server CPU and 26 µs at p50 on a quiet host at BN's close, 64 % of
  the CPU in its one `send()` and 33 % in the read and the reactor's wake
  (BN-Q8: no engine change in BN reaches it).
- **A spilled value an UPDATE re-assigns is still never released**: BN-R3
  stopped the copy of a value the SET list does not name, not the
  superseded one it does. The deferred-release drain stays unbuilt, and
  `heap-and-tuple.md` §3.4 states the rule it must take: a value goes only
  once the newest version naming it is reclaimed.
- **What the suite does not cover:**
  - the race BN-S3's review found and the order fixed - an assertion adopted
    on another core between a statement's refused `IS` and its row's `IX` -
    has no cell; the ask sits after the conflict check, where it always did;
  - the var-heap tail hint across a `DROP TABLE` reclaim that reuses the
    dropped chain's root id: the cell (`ARootIsBornWithNoHint`) uses the
    in-memory store's id order, not a reclaim on `DevicePageStore`.

## The scan row (BM)

Written at BM's close on `worktree-bm-scan-kernel-open`, verified at `952c67e5`; the first and
last entries restated at BT's close, verified at `4274ed81`. Owned by
`instructions/v3.0.0/workorder-bm-scan-row-kernel.md`,
`instructions/v3.0.0/workorder-bt-accept-tuple-row.md` and `parser-v2.md` §7.

- **A rejected scan row costs 27.4 ns after BT, and what is left of its
  fixed cost is what BT-R3 keeps**: `bn`'s net row at `4274ed81` is 27.4 ns
  of server CPU against 31.0 at `eeb88f47` in the same run
  (`bench/v3.0.0/results-bt-close-v2.7.0-997-g4274ed81.md`), 0.94 times
  PostgreSQL 18's 29.1 ns from BM's close (not re-measured). BT-S1's
  ablations (`bench/v3.0.0/results-bt-s1-premise-v2.7.0-992-geeb88f47.md`)
  priced what remains: the rejected row's two counters and its row charge
  about 3 ns together (kept: `ANALYZE`'s counts and the budget's refusal
  row are BM-R2's), and at most 0.7-1.0 ns for `AcceptTupleAt`'s
  `Status` return - an upper bound that also holds its `PageSpanGuard` and
  the call itself (kept: BT-R1 keeps visibility in that one call). No profile was taken after BT;
  BM's (29.3 ns on the frame-pointer build) put 7.4 ns in the kernel's
  compare and 6.2 in the slot loop, which BT did not touch.
- **A recent row's visibility is not memoized** (BM-R7, BM-S5 struck at
  BM-S1, `bench/v3.0.0/results-bm-s1-scan-row-v2.7.0-942-gf5a7efb2.md`).
  Beside a point writer, as BM-Q8's cell specifies, `LookupCommit` and the
  window latch were 3.8 % of the scan core's CPU against the 10 % gate. The
  share rises with the rows on the window path: 10.7 % beside an every-row
  writer, 15.6 % with every row on it and no writer, a lookup costing
  12-15 ns. Neither of those two shapes is the gate's cell, and a
  statement scanning rows written since the floor pays them.
- **A step's first visible row of a statement takes the full path**: the
  kernel is armed only once that step's checks have run (BM-R6), so a
  statement whose step sees one row gains nothing from it.
- **The kernel holds a conjunct against a literal only**: a column on the
  right, a function conjunct, a spilled `varchar`, a `uint64` tuple key and
  `UPDATE` / `DELETE`'s own row loops are filtered as before.
- **The every-row `UPDATE` may still pay a few µs for BM-S2** (verified
  at `952c67e5`, `bench/v3.0.0/results-bm-s6-remedy-v2.7.0-948-g952c67e5.md`).
  S2 put a `Status`-building check on every row of `UPDATE`'s path, +4.2 to
  +4.7 % at the close; the remedy asks each as a predicate first, and the
  pooled re-measure reads +8.9 µs literal and +3.2 prepared over A of
  ~740, within the spread - but B2 is slower than A in 12 and 11 of 15
  runs, and one batch of three reads +16.9 and +14.8. Measured at 1,000
  rows, `cores = 1`, `relaxed` only. BT-S4 deleted the per-row conjunct
  check (UPDATE's and DELETE's walks check once, BT-R5), and BT's close
  reads the every-row `UPDATE` 3.1 / 3.5 % under BT's opening - inside the
  largest A-against-A' gap, so whether BM-S2's residual is gone is not
  resolved at 1,000 rows; at 200 and 10,000 rows B is below A in 6 of 6
  runs literal and 5 of 6 prepared (`bench/v3.0.0/results-bt-close-v2.7.0-997-g4274ed81.md`).

## Carve and checkpoint stalls

Verified at `e5a7e1b7` (BA's close,
`bench/v3.0.0/results-ba-s17-census-v2.7.0-736-ge5a7e1b7.md` and
`results-ba-close-overhead-v2.7.0-736-ge5a7e1b7.md`; diagnosed in
`results-ba-carve-diag-v2.7.0-736-ge5a7e1b7.md`). Owned by `docs/spec/wal.md`
§11a and, for the checkpoint, BI (`workorder-bi-checkpoint-off-the-reactor.md`,
closed 2026-10-11 at `ec827a19`).

- **A `relaxed` carve's longest stall is 226-258 ms per server life**
  (median), against 20-46 ms at BA-S4; under `group` and `strict` it fell
  to 3.6-5.9 ms. At `cores = 1` it is not BA-S13's, and at B it reads a
  data-file `fsync` stall BA-S4's binary also meets; maxima above 300 ms,
  `cores` 2 and 4, and carves inside the measured span are unexplained or
  unmeasured (the diagnosis file). BA-S13's exit, a carve under 10 ms, is
  not met.
- **The checkpoint still holds its reactor past 10 ms, for writes** -
  restated at BI's close (`ec827a19`,
  `results-bi-s5-checkpoint-stall-v2.7.0-784-gec827a19.md`). Since BI a run
  is a `system` task that parks for every sync, and its reactor time fell
  about tenfold; what remains on the reactor is a step's and a publish
  batch's `pwrite`s (single writes up to about 300 ms in the kernel's
  writeback throttle) and the inline log gate after 16 deferrals at
  `relaxed`. Under `relaxed` a run is 4-8x longer than before at `cores = 1`.
  `bugs/a-checkpoint-run-holds-its-cores-reactor-for-its-whole-length.md`
  (closed unreproduced at `d83bd502`) carried the numbers and what a fix
  needs.
- **A run's map halves can wait on its reactor for another caller's
  data-file sync** (`[source-read]`, on `fix-map-flush-order` over
  `27e9d03a`). The map write order (`page.md` §5) puts a sync between the
  headerless and free maps whenever a headerless map changed. A blocking
  flush - `SYNC`, a reclaim's `PersistMaps`, the synchronous anchor publish
  - takes that sync inside `FlushMaps` under the map write barrier
  (`map_flush_latch_`), a plain mutex, and the run task's
  `WriteHeaderlessMaps` and `WriteFreeMaps` take the same barrier on the
  reactor, so a run that meets one waits out its `fsync` there. Before the
  map write order the barrier covered `pwrite`s only. It needs a pending
  headerless change and a concurrent blocking flush at once; not measured.
  Releasing the barrier across the wait would keep BF-R5 only with the run
  re-parking on a newer ticket, a change to the halves' contract.
- **A paced publish's map write can carry the bit of a page another core
  created unlogged after the collection** (BI-S4, `[source-read]`). The
  publish collects the dirty pages, writes them, then writes the maps; a
  Waystone page another core creates between the collection and the map
  write has its bit on the device without its bytes, and a crash before the
  next flush leaves it allocated and never written. The whole-store `Flush`
  had the same window before BI; BI-Q12 closed the step's, which was a
  whole run long. Not measured.

## Per-core structures kept (BP)

Verified at `ccf9cfc1` (`v2.7.0-909-gccf9cfc1`; the census,
`bench/v3.0.0/results-bp-s1-census-v2.7.0-909-gccf9cfc1.md`). Each was
measured under BP-R2's bar and kept; the cost stays and is stated here.
Owned by `instructions/v3.0.0/workorder-bp-per-core-residue.md` §7.

- **Every core checkpoints in its turn** (AT-S8, BP-H7): runs per 5 s period
  are 1 / 2 / 3.9 at cores 1 / 2 / 4, and pages written per 1,000 rows rise
  11-18 % from one core to four. Under BP-R2's throughput and p99 terms;
  one instance-wide run would need every live transaction's undo head
  published, a recovery-correctness change (BP-Q5 (b), not taken).
- **A whole catalog memo drops when the schema word moves for a root a
  level grew over** (BP-H6): at most 2.7e-4 drops per statement, 0.08 % of
  statement CPU at worst.
- **The per-core id window** (AT-R4, BP-H8) is unmeasured: `SHOW META`
  carries no window-size or burn counter, and no skew cell has run. A
  `LookupCommit` count exists since BM-S1 (`visibility_window_lookups`).
- **The per-core in-flight table** (AX, BP-H9): a miss reads one id; its
  1,024-slot cap per core was not approached, and the skew case that would
  settle whether one core can reach it while others have room has not run.
- **A `cores = 1` every-row `UPDATE` runs 3-6 % slower since BQ joined BP's
  branch, cause not found** (carried open at BP's and BQ's close,
  2026-10-13). `ccf9cfc1` is the one fast build; every build from the merge
  `6139c2de` on is slower, about 1 point of it the harness's position
  offset. About half the gap sits in `WalManager::Append`'s own frame
  (1.05 % of samples to 2.97-3.60 %), its callee `WalStream::Append` flat.
  Three bounded attempts (a bisect, aligned builds, a lazy `RollRecord`)
  did not find it. Point statements and `cores = 4` are within noise
  (`bench/v3.0.0/results-bp-bq-close-v2.7.0-924-g3e77e7d1.md`).
- **BP-H10's fence premium is +5.1 %** after BP-S6 at `cores = 8`; its next
  cut, BP-Q15 (b), is a protocol change for its own order.

## Backup and restore (BR)

Verified on `br-backup-restore` (BR's close). Owned by `docs/spec/backup.md`;
the procedures are `manual/server/backup-restore.md`.

- **Not built** (BR §0.2): incremental backup, compression and encryption of
  a backup or the archive, a native object-storage target, a parallel copy,
  concurrent backups, a logical dump, a streaming standby.
- **A torn page in a path D copy is refused, not healed** (verified on
  `db-torn-page-healing` at `c802fac9`; DB-Q7 (a)). A snapshot that does not
  freeze the filesystem (a cloud disk snapshot) is a crash image. Every data
  page's CRC32C is checked when the backup stops, at `verify` and at the
  restore's layout, so the torn page is named there rather than at the
  mount; it is not repaired, though since DB the restore's mount would heal
  one whose image lies in `[start, stop]` - no cell shows it, and relaxing
  the three checks is a successor's. Path B's copy cannot tear (the fence);
  path D needs a freezing snapshot.
- **A restore leaks**: a page the source freed after the copier read its
  map region stays allocated in the restore, as after a crash; Waystone
  pages, made unreachable at the restore's mount (`DiscardAdvisoryState`),
  stay allocated where they were written, and so do the growth pages a
  pre-DA build gave `sys.access_stats`' chain, which nothing reaches since DA.
- **PITR targets an LSN, not a time**: the commit record carries no time
  (BR-Q5 (b)). An operator notes `SHOW META`'s `wal_durable_lsn`.
- **`kds_backup` reaches the admin role only on an `auth = off` server**:
  `tools/kwp.py` has no SCRAM or TLS.
- **The LVM reference script is not run here**: the development VM grants no
  root (`br-s0-census.md` H9); path D's drills use the freeze script.
- **An `archive_command` that hangs holds the archive**: it is not timed out,
  only ended at shutdown; the archive's lag and `wal_archive_blocking` are
  what an operator watches.
- **A backup starts a thread after startup, which `sched.md` §9's invariant
  1 forbids and no document declares.** Read at `a4d2fb3b` while DE was
  reviewed: `BACKUP COPY` starts the copier's thread and `BACKUP STOP` the
  finisher's (`src/server/backup.cpp:166`, `:340`), each joined by the next
  call. The invariant reads "no thread creation after startup"
  (`docs/spec/sched.md:260`), and its declared exceptions - the WAL writer,
  the data-sync thread - start at boot. `backup.md` names the archiver's
  thread (`:116`) and not these two. Nothing is known to go wrong; the
  spec and the code disagree, and neither `sched.md` nor `backup.md` says
  which is meant.
- **What BR costs when on** (BR-S10, `bench/v3.0.0/results-br-close-backup-restore-v2.7.0-913-ga8110b05.md`):
  the archive shares the WAL device's bandwidth - wide `INSERT`s lose 27-29 %
  with the archive on the data device, 3 % with it elsewhere; the archiver
  keeps 1.5x the mean log rate and 0.78x a 5 s peak, so it falls behind at a
  peak and drains after it (39 segments held once). A base copy runs at its
  256 MiB/s throttle and, at `cores = 1`, quadruples the workload's p99 for
  the seconds it runs (`cores = 4`: at the floor).
- **The copier's rate (256 MiB/s) and chunk (128 pages), the fail-closed
  reserve (4 segments) and the hour that ends an idle backup stay
  `[provisional]`**: BR-S10 measured each at one value only (its results
  file says why each stays).

## Transitions (BV)

Verified on `cn4-transition-refresh` (BV's close). Owned by
`docs/spec/transition.md`.

- **Not built** (BV §0.2, CN-4 §7):
  - a trigger or any action on a transition;
  - a stored history or audit log;
  - a state machine spanning columns or relations;
  - guarded edges;
  - freezing a row in a terminal state.
- **A crash between a `DROP TABLE`'s commit and the retirement of its
  Transitions leaves an inert row.** The row names a relation that is gone
  and can never return, because an oid is never reissued. It keeps its name
  taken until `DROP TRANSITION`, and `SHOW TRANSITIONS` prints it with
  `rel=oid=<n>`. Nothing at mount sweeps it.
- **The sim harness has no Transition op.** BV-S3 replaced it with
  exhaustive cut-everywhere crash cells (`tests/transition_crash_test.cpp`)
  for `CREATE`, an `UPDATE` along an edge, and `DROP`. A seeded workload
  interleaving state moves with transactions, rollbacks and faults is not
  generated.
- **The sim harness generates no heap relation.** Verified at
  `0205444b` (`sim/workload.cpp`): every relation the workload creates is a
  btree with a declared tuple key, because the oracle reconciles rows by
  tuple key and a heap's is hidden (`heap-and-tuple.md` §3.1b). A heap's
  storage is the btree's, which the corpus exercises; its surface - the
  hidden column skipped by `*`, `RETURNING *`, `DESCRIBE`, positional
  `INSERT` - and its crash recovery are covered at unit level only
  (`tests/heap_hidden_key_test.cpp`), never by a seeded crash workload.
- **`CREATE`/`DROP TRANSITION` inside an explicit transaction commit at the
  statement**, as `CREATE`/`DROP ASSERTION` do. A `ROLLBACK` does not undo
  them.

## Column slots and `ADD`/`DROP COLUMN` (BW)

Verified on `worktree-bw-instant-add-drop-column` over `bc42a011`
(`instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`).

- **A positional `INSERT` written before a column change is caught by its
  count only in part** `[quiet-wrong]` (`alter.md` AL11, BW-Q9); a
  column-listed one is not exposed (BW-S6). After an
  `ADD` every stale row is refused. After a `DROP`, a row written for the
  old columns with its key left out has the new full form's length and is
  admitted with its first value as the key and every other value one column
  over; after one `DROP` and one `ADD`, an old full row has the new full
  length. Positional SQL text carries no described shape, unlike a KWP load
  (BW-Q10). The column list closes it for a writer who uses it: built in
  BW-S6 (BW-Q11 (a), BW-R13), a column-listed `INSERT` names its columns,
  so no column change can shift it. Verified for BW-S6 on
  `worktree-bw-instant-add-drop-column` over `e3450498`.
- **A value's type refusal at encode carries no byte** (predates BW): an
  `INSERT` value of the wrong type - positional or column-listed - is
  refused by `exec::EncodeRow` (`row_codec.cpp`, "column 'a' expects an
  integer"), which `InsertOneRow` passes through as it is; a multi-row
  statement adds only `(row k)`. Every other `INSERT` refusal names the
  value's byte. Found by BW-S6's review on
  `worktree-bw-instant-add-drop-column` at `e1e17abc`; not fixed there, as
  the codec has no `AstValue` and threading the byte is outside BW.

- **Rows are never resized after a `DROP COLUMN`** (CD-OI-1 (a), verified on
  `cd-column-slot-cleaning` at `0badb7d6`). `PURGE DROPPED COLUMNS` zeroes a
  dropped column's cells, releases their spills and frees a slot an `ADD`
  had placed, but the bytes stay inside the row size: a dropped declared
  column's are never reusable, and a `SPARE 0` relation gains no `ADD`
  capacity. Resizing (a `REBUILD` into a new tree) needs the invariant-13
  ruling and is not built.
- **`PURGE DROPPED COLUMNS` does not yield its core** (CD-OI-11 (a)): an
  O(rows) walk on one core; its stall is CD's close measurement's cell 4. It holds no
  page across leaf windows, so a yield could be added without a redesign.
- **The CABIN AUTO controller's memory-resident entry is keyed by
  (relation, position)** and survives a drop and a reuse: a candidate
  parked in BUILDING after a refused create on a dropped position stays
  parked for the column that reuses the position until a restart (CD-S3's
  departure). Performance only; an operator's `DROP CABIN` of an auto Cabin
  leaves the same stale entry.
- **Readers see an open `ADD`/`DROP COLUMN`** (BW-Q8 (a)): the catalog row
  is rewritten in place under the DDL's transaction, so another session's
  read can see the undecided column set - a column missing from `SELECT *`,
  a name refused, an added column reading NULL - before the decide. Writers
  are fenced by the relation `X`; no value a reader shows is wrong.
- **A column `CREATE CABIN` takes no relation lock**, the controller's or
  an operator's, so a Cabin can land on a column between `DROP COLUMN`'s
  restrict check and its write, or on an open `ADD`'s column and outlive
  its rollback. It is never served - no statement can name the column -
  and `DROP CABIN` cannot reach it by name; `PURGE DROPPED COLUMNS` drops
  one on a dropped position and an `ADD` one on the position it takes (CD,
  verified at `0badb7d6`), so no column inherits it.

## Expression and predicate Cabins (AQ)

Verified on `worktree-aq-expression-cabins` at AQ-S1
(`instructions/v3.0.0/workorder-aq-expression-cabins.md`).

- **A `DROP TABLE` inside an explicit transaction leaves its expression
  Cabins' texts in `sys.cabin_exprs`** (AQ-Q11). The drop can still roll
  back its Cabins' rows, so the texts are removed only by an autocommit
  drop, after its commit; once the transactional drop commits they are inert
  - nothing reads a text no `sys.cabins` row names - and take a little
  catalog space for good. The same holds for a crash between an autocommit
  drop's commit and the removal, and for a `DROP CABIN` whose text delete
  failed after its row was gone.
- **A column Cabin's `CREATE` and `DROP` take no relation lock**, as before
  AQ; only the expression and predicate shapes take the relation `X`
  (AQ-Q13), because their text names columns by position and a create
  racing `DROP COLUMN` would leave one naming a dropped column.

## Unique indexes (BX)

Verified on `worktree-bx-unique-index` at BX-S2
(`instructions/v3.2.0/workorder-bx-unique-index.md`).

- **A unique build over string keys sharing their 32-byte prefix costs
  O(n²) entry visits**: each row's check walks the whole run (BX-R7 as
  built).
- **A busy verdict counts any undecided writer of a row the run names**,
  whether or not its pending version carries the key (`CheckUniqueKey`'s
  conservative busy): a write can wait on a transaction that only touched a
  row whose stale entry names the key. A wait it did not need, never a
  duplicate admitted.
- **Row 2 onward of a multi-row `INSERT`, and a KWP load row past its
  chunk's first, meeting an undecided writer of its key is refused
  `TxnConflict`** rather than waited for (BX-Q3 (a)): `ExecuteInsert`
  cannot park mid-statement.
- **`ON CONFLICT` reacts to one arbiter** (BU-Q18, CG-Q1 (b)): the tuple
  key, or the column named in `(<col>)` when it is the one column of a
  unique index. A conflict on any other unique index is refused
  `AlreadyExists`, where PostgreSQL's target-less form skips or updates on
  every one, and a multi-column or expression target is refused
  `NotImplemented`. A refusal, never a wrong row. Re-verified on
  `cg-on-conflict-do-update` at CG-S1 (`tests/dml_forms_test.cpp`).
  Owner: CG (`instructions/v3.2.0/workorder-cg-on-conflict-do-update.md`).
- **`ON CONFLICT DO UPDATE`'s unique-index re-read has no forced-interleaving
  cell** (CG-S0 reading 1b): the arm reads its holder again under the
  holder's `X`, and a holder moved off the key between the check and the
  `X` takes the insert arm. No cell puts a writer in that gap - it lies
  inside one statement - so the re-read is covered by reading only.
- **Under READ COMMITTED inside an explicit transaction, `ON CONFLICT DO
  UPDATE`'s arm reads at the statement's view**, not one minted after the
  holder's `X`: a holder committed by another session after the statement
  began is refused `TxnConflict` (retryable) where PostgreSQL would update
  it. A refusal, never a wrong row; in autocommit each arm row takes a new
  view. Re-minting a view inside a statement is a decision the order did
  not take. Verified on `cg-on-conflict-do-update` at CG-S1's review.
- **`sys.relation_stats` counts `ON CONFLICT DO UPDATE`'s updated rows as
  inserts**: the statement finishes as an `INSERT` and hands the one count,
  inserted plus updated, to the insert column. Observability only.
- **An `UPDATE` that walks its relation costs more server CPU since
  CG-S1** (`6775aec1` against `v3.4.3`). Sources:
  `bench/v3.4.3/results-cg-close-v3.4.3-8-g6775aec1.md` and the attribution
  run `bench/v3.4.6/results-cg-slow-mode-v3.4.6-3-g1011c2f3.md`.
  - **At 10,000 rows it is bimodal.** `UPDATE u SET v = x WHERE name =
    '...'` costs a median of about 825 us of server CPU per statement in
    two runs of three, against about 762 us in the rest and in the
    `v3.4.3` runs (about +8 %, about 6 ns per walked row).
  - **At 1,000 rows it is in 19 runs of 20**: a median of 110 us against
    100.5 us (+9.5 %). The exception sat at `v3.4.3`'s level in a run
    whose 10,000-row walk was slow, so the mode is not fixed per server
    process.
  - **Ruled out by the attribution run**, made on a quiet host with the
    server pinned:
    - the host: the slow mode follows the binary (13 of 20 runs on
      `6775aec1`, 0 of 10 on `v3.4.3`, interleaved within seconds); no
      build or test process ran, run-queue wait is 3-6 us in both modes,
      and the SMT sibling is at most 2.4 % busy in 12 of the 13 slow runs.
      The CPU-time column alone could not exclude a slower CPU;
    - the close's phase-order confound: B's binary through A's phases is
      slow in 7 of 10 runs.
  - **The point `UPDATE` and `INSERT` are within 5 % of `v3.4.3`.**
  - **Open:** why it switches off in some 10,000-row runs (an
    address-dependent state chosen per relation is the candidate), and which code in
    the walk pays it. CG-S1 split `UpdateInner` into its parse and
    `UpdateParsed`, so the compiled walk can differ though its loop and
    `apply` are unchanged.
  - **Next:** a sampler profile, an ASLR-off arm, an `objdump` diff of the
    walk. Measured on the binaries of `f44846ef` and `6775aec1`, with the
    harness at `1011c2f3`; the engine at `1011c2f3`, which has changed the
    dispatcher and index maintenance since `6775aec1`, is not measured.
- **Each `ON CONFLICT DO UPDATE` arm row re-runs `UpdateParsed`'s statement
  setup** (resolution, `InitTableAccess`, `CompileAssignments`): with the
  arbiter's lock and lookup, about 5 us a row over a point `UPDATE` at
  `6775aec1`'s close; the setup's own share is not measured. Compiling the
  assignments once per statement, `EXCLUDED` as a per-row slot, is the cut
  if it matters.
- **An `ON CONFLICT DO UPDATE` wait after its first write is a refusal**
  (CG-S0 reading 1): a later row meeting an open writer of its key, its
  unique value or its parent is refused `TxnConflict` once an earlier row
  of the statement is written, as any `INSERT` is - only a statement that
  wrote nothing is waited for and run again.

## The query layer and the DML forms (BU)

Verified on `worktree-bu-basic-query-surface-s4` at `a13ceb49`
(`instructions/v3.0.0/workorder-bu-basic-query-surface.md` §6, the review
rows of BU-S4 to S8). Each is a refusal or a cost, never a wrong answer.

- **The query grammar's refusals by name reach no client** (BU-Q9): when it
  refuses a statement too, the native refusal is the reply, so `OVER`,
  `NATURAL JOIN`, `DISTINCT ON` and `NULLS FIRST` answer the native parse's
  "expected ..." (`InvalidArgument`) rather than `NotImplemented` by name;
  `WITH` and `LIKE ... ESCAPE` happen to be refused by name natively.
- **A statement opening with `(`** - `(SELECT ...) UNION ...` - is routed by
  the dispatcher as an unknown command and reaches neither grammar.
- **An aggregate reading an enclosing query's column inside a subquery** is
  refused "an aggregate is not allowed here" where PostgreSQL answers the
  mixed form; the pure-outer form is refused rightly, by the wrong message.
- **A lone `NULL` is `varchar`** (BU-Q11): `HAVING NULL`, `WHERE NULL`,
  `SELECT NULL UNION SELECT 1` and `INSERT ... SELECT NULL` into a non-text
  column are refused; `CAST` is the spelling. Likewise a bare `5` from an
  `INSERT ... SELECT` source into an `int32` column.
- **`UPDATE ... FROM` reads its `FROM` row only in its WHERE**: a SET value
  or a RETURNING item naming it is refused `NotImplemented`. One relation
  after `FROM` or `USING`; a comma then a `LEFT` or inner join whose `ON`
  reads a relation before the comma is answered where PostgreSQL refuses it.
- **Costs measured at BU's close, not cut:** a layer statement holds every
  relation's rows it walks (bounded by `sort_max_rows`), copies each row per
  evaluation stage, returns a cached subquery result by value per outer row,
  and probes `IN (subquery)` linearly; `UPDATE ... FROM`'s EXISTS carries the
  target's own conjuncts, so even a point update walks the target.
- **Without a transaction manager** (a test configuration; production always
  builds one) a RETURNING is refused after its rows are written.
- **`ON CONFLICT`'s pre-check** keeps the units it took across a wait and
  re-run inside an explicit transaction, and takes a key's unit before the
  foreign-key parent's - the reverse of the row path, a cycle the deadlock
  detector breaks; `DmlFormsTest` has no lock table, so its wait has no cell.

## The advisory optimizer, in the background (CA)

Verified on `worktree-bz-background-advisory-optimizer` at `954abc20`
(`instructions/v3.2.0/workorder-ca-background-advisory-optimizer.md` §7).

- **The advisory work still runs on the statement.** Trail recording, replay's
  directory walk and the access statistics are written by the statement that
  executed, as §1.1 of the order reads them; the access statistics are a
  memory add to the core's own part since DA, where they were page 11's
  exclusive hold. Narrowed by
  CF-S1b (on `cf-waystone-premise` at `46c632aa`): the Waystone half runs
  only for a shape an operator opted in. CA did not move
  them: CA-S0 measured them at 0.2-0.6 % of sampled server CPU on scenarios 1-4,
  and switching all three keys off moved no throughput outside noise
  (`bench/v3.2.0/results-ca-s0-premise-*.md`). The order's note, drain, steward and
  published route table (CA-R2..R4) are not built. Neither is the base-fetch hint
  (CA-S3).
- **Scenario 2's booking rate is not a hot row's lock held across the group
  fsync (lock waits are 0.03 % of its DB time); a booker's time is CPU and
  its own commit acknowledgement.** Corrected 2026-11-08
  on `worktree-cp-lock-model-exploration` at `cc54a296` (CP-S0, cell M6,
  `bench/v3.4.7/results-cp-s0-premise-v3.4.7-1-gcc54a296.md`); the earlier
  text here, verified at `954abc20`, said the row's `X` is kept "until its
  commit is durable", and the tree says otherwise: under `group`
  `WalManager::Commit` returns after the append (`src/wal/manager.cpp:441-445`),
  `FinishCommitBody` publishes, retires and releases every borrow
  (`src/txn/manager.cpp:306-336`), and only the acknowledgement waits for the
  batch (`txn.md` §4: "a reader sees a commit as soon as the record is
  appended, which under `group` is before the platter has it"). Measured: a
  second session's `UPDATE` of the row a first session had just committed
  answered before the first's `COMMIT` in 300 of 300 iterations, by a margin
  of 78 % of the acknowledgement's latency (p50 423 us of 539 us), and parked
  on no lock; under `strict` above one core, where the deferred commit does
  hold its borrows across the sync by design (BA-S7), the second waited the
  whole sync (305 `sys.lock_waits` rows, p50 440 us) and answered first in
  1.3 % of iterations. `tests/lock_release_before_sync_rig_test.cpp` pins
  the `group` order on the two-core rig. So eight bookers each wait for their
  own `commit.wal_flush` (44 % of the run's DB time in CP-S0's scenario 2
  run, beside 53 % on CPU; lock waits 0.03 %), which is consistent with a
  read path made 6 % cheaper moving the committed TPS by nothing
  (`bench/v3.2.0/results-ca-r1-visibility-*.md`).
  Releasing a lock before durability is not a decision left to take: the
  engine does it, and CP withdrew the candidate (its order §3).
- **A pinned floor still costs a latched lookup per writer, per statement.** A
  memo removes the per-row cost (`txn.md` §4.4). Lowering the idle burn's
  threshold so the floor would follow was measured and reverted: it added
  nothing on scenarios 1-4 (candidate C2, `results-ca-r1-*`).
- **Scenario 2's `--verify` reports 8-12 % failed I1 checks in every arm,
  the base included**, and 13-28 of 400 at `v3.1.0-30`. Nobody has determined
  whether this is the driver's read skew or an invariant slip in the engine.

## The Waystone opt-in (CF)

Verified on `cf-waystone-premise` at `46c632aa`
(`instructions/v3.2.0/workorder-cf-waystone-premise.md` §8.6; owner
`docs/spec/waystone-concpets.md` §4a-§4e). What Branch O closed is restated
in the entries above (the point-read recording cost, the registration scan,
the claim's deepening trap, the leak per mount); these are what it leaves.

- **Warm-up after every mount.** Trails are unlogged and discarded at every
  mount (CF-O-Q2 (a)), so after any restart, clean or not, an opted instance
  records again on its second execution on one core and replays only from
  its third. The opt-in survives; its trails do not.
- **An opt-in lapses at a `kFingerprintVersion` bump.** An opted row of the
  old version records nothing, keeps no root and reads `waystone = 0`; the
  mount logs `INFO [waystone] <n> opt-ins lapsed at fingerprint version <v>` while
  no current row of its `fetch_id` is opted. Nothing carries the bit
  forward, since that would need stored text (CF-O-Q12 (a)): the operator
  re-issues `WAYSTONE ON`.
- **The refill: a whole-chain scan of `sys.patterns` per core per schema
  word move, while anything is opted.** Every core drops its opted map at a
  word move and refills it at its next opted-candidate `SELECT`. The word
  moves on DDL, on a clustered or index root growth (which DML causes, on
  every core but the growing one), on a `CABIN AUTO` create or drop, on a
  catalog-writing resolve and on an opt-in, so it is not "per DDL". Bounded
  by the rows ever opted; not measured.
- **An opt-in is a shape under what it bound** (DI-S3, superblock 38;
  verified on `di-shape-identity` at DI-S3's commit): the row carries the
  binding, a committed `DROP TABLE` ends the opt-ins naming the relation,
  and two bindings of one text keep two directories. What is left:
  - **A dispatcher with a transaction manager and no log never sees a
    drop commit** (`EndDdlScopeById` reads the commit's LSN), so there the
    opt-in outlives the drop until a mount; no server is built that way.
  - **An ended row stays**, its bit and root cleared, naming a dropped oid:
    one 178-byte row a dropped relation's opt-in, never reclaimed.
  - **A renamed relation's opt-in is inert, not ended**: its text no longer
    compiles, and the row waits for `OFF <fetch_id>`.
  - **`OFF <select>` over an example that binds only part of its
    relations** (a join whose second relation is gone) names a binding no
    row has and answers `unchanged`.
  - **A `SELECT` over more than 16 distinct relations cannot be opted in**
    (`NotImplemented`).
- **The opted path's own costs are left as built** (CF-O-Q13 (a)): every
  re-record takes the directory page exclusive, which dirties it
  (`src/stats/waystone_dir.cpp`, `LookupOrCreateWaystonePage`'s `Get`), and
  every replayed statement builds `TrailReplay::Build`'s per-statement step
  map (`src/exec/trail_replay.cpp`). CF-S0 measured no cost there to
  remove. CF-S2's close prices the opted path whole: `scanprobe` opted is
  level with the engine before O, which recorded it anyway (-0.3 % server
  CPU, inside the spread; `bench/v3.4.0/results-cf-s2-close-v3.4.0-9-g46c632aa.md`).
- **The mount's per-opted-row directory create is unmeasured.** Each opted
  row costs the mount, before the listener binds, one device read of the new
  page's id and, on an id the device still holds, a zero write and a sync on
  the data-sync thread, plus one logged overwrite. The cursor restarts at
  128 at a clean mount, so the zero-write arm is the common one after a run
  that reclaimed pages.
- **Measured at CF-S2's close, at `cores = 1`** (`v3.4.0-9-g46c632aa`,
  `bench/v3.4.0/results-cf-s2-close-v3.4.0-9-g46c632aa.md`): the steady
  state. With nothing opted, the lookup cells land on CF-S0's arm C (both
  keys off). An opted `scanprobe` keeps its gain, and another shape's opt-in
  costs the rest nothing measurable. **Still unmeasured:**
  - the refill under a moving word, and the mount's added cost. The harness
    has no cell for either: the refill needs root growth during a read cell,
    and the mount cost needs many opted rows.
  - a cold pool and a size sweep.
  - `cores > 1` beyond the one `scanprobe` cell the close ran (the results
    file).

## Catalog chains past the old ceiling

Verified on `worktree-fix-catalog-page-ceiling` at `0e539d46`, 2026-10-18.
Since superblock 31 every page a catalog chain grows onto comes from the
general supply and the reserved range 17..127 is deleted (`catalog.md` CT5,
`crosscore.md` CC12 CR3). The operator ruled "$!recommended" on 2026-10-18,
taking CLA's recommendation on each of the change's nine decisions; D4 and
D9 were to record these costs and fix nothing, and the rest are costs the
change's stages found and left.

- **A cold `sys.tables` walk can spend an `INSERT`'s no-refuse window.**
  (D4.) A grown catalog page is an ordinary evictable frame (`eviction.md`
  EV3). An `INSERT` opens its `NoRefuseWindow` (`command_dispatcher.cpp`
  :5804) before `AdmitExplicitRowId` (:5842) or `AllocateRowId` (:5864)
  walks `sys.tables` from its root to the relation's row, so every cold page
  of that walk faults inside the window and spends one frame of its
  64-frame share (`page_store.hpp`'s `kWindowFrames`), which is never
  returned (`DevicePageStore::ReserveFrame`). A walk past 64 cold pages, on
  a pool that is full with nothing reclaimable, reaches `FailStop` instead
  of a `ResourceExhausted` refusal. That needs a `sys.tables` chain of more
  than 64 pages, about 3,900 relations (61 `sys.tables` rows per page),
  cold, with a full pool. Not
  reproduced. Moving the first row-id call ahead of `NoRefuseWindow::Open`,
  or memoising each relation's `sys.tables` row address (which also removes
  the O(chain) walk per `INSERT`), would close it; the second was CLA's
  proposed follow-up. **No owner.**
- **`WAYSTONE ON` scans the whole of `sys.patterns` under page 9's
  exclusive hold.** Restated on `cf-waystone-premise` at `46c632aa`; the
  statement-path registration scan this entry named (D9.) is gone with
  `Catalog::RegisterPattern` (CF-S1b), so no `SELECT` scans or writes the
  chain. `Catalog::SetPatternWaystone` holds the root exclusive across the
  call: `ON` walks the chain for a current row and, finding none, inserts
  through a second walk; `OFF` walks it whole, since it clears every row of
  the `fetch_id` of any version. Each costs O(pages) with every other core's
  `WAYSTONE` waiting behind it, and the chain only grows: `OFF` keeps its
  row, no row is ever retired, and a fingerprint version bump leaves the old
  rows in place. On a superblock-33 volume the chain holds only the shapes
  ever opted, so its length is what operators opted, not what traffic
  repeated. The gated cells, `AMountClearsEveryWaystoneRootOfALongPatternChain`
  (1,500 opted rows) and, with `KDS_TEST_LARGE_CATALOG` set,
  `CatalogPageCeilingTest.AMountClearsEveryWaystoneRootOfAPatternChainPastTheOldBound`
  (17,000 past 128 pages, 212 s at CF-S1b), build their chains through it.
  **No owner.**
- **Every mount dirties every `sys.patterns` page.** Narrowed by CF-S1b
  (on `cf-waystone-premise` at `46c632aa`): on a superblock-33 volume the
  chain holds only the rows ever opted. The advisory discard
  (`server::DiscardAdvisoryState`, run at every mount) renews every opted
  row's root and clears every other in one `kWrite` pass,
  `Catalog::RenewPatternDirectories`' second, whose walk fetches each page
  exclusive, and an exclusive fetch marks the frame dirty (`device_page_store.hpp`, "Get() marks the
  frame dirty"), so the mount's completion checkpoint writes the whole
  chain back whether a row changed or not. A shared walk plus an upgrade
  would cost a second fetch per page that holds a directory; nobody has
  measured either. **No owner.**
- **The delete-mark purge walks the whole catalog with no yield.**
  `Catalog::RetireDeleteMarks` visits every page of every catalog chain
  exclusive (`kWrite`) in one call on core 0's 50 ms tick
  (`CommandDispatcher::SweepSettledDeleteMarks`, gated on the mark count
  and the settlement's progress), so its cost is the whole catalog's size,
  which the old 128-step bound used to cap. Performance only: what it
  retires is correct. **No owner.**
- **A cycle among `sys.statistics` growth pages that all carry its oid is
  re-formatted for the whole walk budget before it is cut.**
  `Catalog::DiscardUnloggedChain` cuts at once a link back to page 17 (its
  owner, bootstrap's 0, is not the relation's) and at any foreign,
  non-heap, unallocated or unreadable page, but a cycle such as 200 -> 201
  -> 200 between pages it owns is walked, re-formatting a page per step, until
  `kMaxPageWalkLength` (2^20 steps) is spent: bounded, but seconds of a
  mount. Reachable only through damage that passes the checksum, since
  nothing frees or relinks such a page. A visited set would cut it at the
  first repeat. `sys.access_stats`' chain, where this was first found, is
  walked by nothing since DA. **No owner.** (Re-read on
  `da-access-stats-memory-resident` at DA-S1.)
- **The sim's restore with `skip_recovery` runs no advisory discard.**
  `SimInstance::CreateFromBackup` leaves the discard to `Boot`, which runs
  it only when it runs recovery, so a restored image booted with
  `skip_recovery` keeps its Waystone roots (its access statistics start
  empty at every boot since DA, being the boot's dispatcher's memory),
  and since CF-S1b skips the directory renewal too; it sets the opted count
  by `RecountOptedPatterns` instead (on `cf-waystone-premise` at
  `46c632aa`).
  Before the change `CreateFromBackup` discarded after `Boot` whatever the
  option said. A restore that skips recovery ships in no configuration;
  only the harness can build one. **No owner.**

## The join planner (CB)

- **An aggregated join's `SUM` can overflow in one join order and not in
  another.** Read at CB-S2's review on `worktree-cb-join-planner`: the int64
  accumulator checks every partial sum (`src/exec/aggregate.cpp`, the
  `SUM` fold), so for one set of rows `MAX, 1, -1` refuses `OutOfRange`
  where `MAX, -1, 1` does not. Since CB-S2 the join planner chooses the
  order rows reach the fold, and a catalog change (`CREATE INDEX`, a Cabin)
  can change it, so the same statement over the same rows can move between
  an answer and a refusal. Never a wrong answer. Owned by
  `docs/spec/aggregate.md`.

- **A reordered plan's `ANALYZE` numbers steps one way and columns
  another.** A step line prints the step's id, its relation's written
  position (CB-Q3), while a `ColumnRef` prints `up:slot.col` with `slot` the
  execution position (`src/exec/plan_printer.cpp`). Correct, and easy to
  misread on a `reordered=1` plan. Owned by `docs/spec/parser-v2.md` I12.

- **The planner prices a relation at `kAssumedRelationRows` until its prior
  is published.** Since CC-S3 a relation's prior replaces the constant
  (`docs/spec/plan-stats.md`), but a mount or a restore starts empty: until
  the steward's first publish - a second after a planned statement first
  meets the relation - the plan is data-free. Owned by `plan-stats.md`.

- **A join subset no run measured is estimated from priors and constant
  selectivities.** `card(T)` exists only for the prefixes of orders that
  ran; another order's prefix is priced from the relations' priors, the
  literal constants and `1 / rows` per join key, which can be wrong by
  orders of magnitude for a skewed join. The 10 % switch margin and the
  build's quarter-cap guard bound how far a wrong estimate moves a plan,
  not whether it does. Owned by `plan-stats.md`.

- **The prior samples at most four root children and their leaves**, so a
  wide root's subtrees and a tree whose leaf fill varies a great deal
  (mid-splits beside full appended leaves) are estimated from a small
  sample; above height 2 the stratified estimate has no unit test of its
  own (it needs about 130,000 rows). Verified at the stratified prior's
  commit on `worktree-cc-stats-planner`; CC's close re-measures it against
  true counts. Owned by `plan-stats.md` PS1.

- **A relation created and loaded after mount runs some join plans 12-30 %
  slower than one restored from a copy of the same data**, on the opening
  binary as well: found at CC's close on scenario 2's customer-statement
  (`bench/v3.2.0/results-cc-close-v3.2.0-184-gc547ef14.md`), cause not
  diagnosed. Not the planner's: both binaries show it with the order
  forced. It makes the 200,000-cargo flip measure +7-10 % on such servers
  where it measures -4.6 to +4.1 % on clean ones, and the close's clean
  -44.7 % at 10,000 cargos was not reproduced by the re-measure at
  `1a9168d3` (-28.8 % on both binaries,
  `bench/v3.2.0/results-cc-close-prior-v3.2.0-186-g1a9168d3.md`) - the estimate's saving
  (`switch=0.68`) is not realised at that size, since a walk rejecting rows
  on their bytes is cheaper per row than the model's walk. Owned by
  `docs/spec/eviction.md` until diagnosed.

- **A block of more than four relations is ordered greedily from priors
  alone**: it keys no posterior, so no observed cardinality reaches it.
  Owned by `parser-v2.md` I12.

- **Which kind serves a step is still chosen by rank, not by statistics.**
  An index is taken ahead of a walk however unselective its key
  (`index.md` IX9's crossover); CC-Q1 (a) put statistics into the order
  only. Owned by `index.md` §9.

## The `sys` views and the statistics stores (BO)

Verified on `bo-relation-stats-checkpoints` at `86d1fe1a` (BO-S7). Owned by
`docs/spec/sys-views.md`.

- **Six refusals over a view name no byte**: aggregation (AG12), a
  subquery predicate and a column-to-column comparison (`HandleCatalogView`),
  a join and `ANALYZE` over a view, and an unknown `sys.<name>`
  (`SelectBody`, both `command_dispatcher.cpp`) answer a bare `ERR`, against
  the rule that every refusal carries the offending token's byte. The
  boolean-conjunct and function-call refusals beside them do carry it.
- **`SHOW PAGE` names every page that is not a btree page `HEAP`**
  (`PageSlotsView`, `src/server/sys_views.cpp`): an anchor, var-heap, map
  or undo page is labelled `HEAP` and its contents read as heap slots. A
  diagnostic's mislabel, not an answer; the page type byte is the
  authority, and the view's `page_type` should spell it.
- **A view's read delays the writer of each part it copies** by the copy: a
  reader takes each core's statistics latch in turn. Bounded by the part
  (4096 shapes, 4096 relations, 4096 waits, 1024 runs), unmeasured as a
  tail on the writing core.
- **An in-process restart keeps the statistics stores**: they are
  process-wide, so the sim harness's restart (`sim/`) and a test's second
  instance in one process see the rows the first wrote. A real restart
  empties them.
- **A checkpoint task's park reads its status before its generation**
  (`Checkpointer::RunTask`, `src/wal/checkpointer.cpp`; found by BO-S7's
  review, pre-existing): an abandoned frame polled again with a failed park
  would end a newer task's run, or, with none running, dereference the null
  report `Abandon` left. Unreachable today - `Abandon` runs only at
  shutdown, after the reactors stop - and so not a defect entry; owned by
  `docs/spec/wal.md` §11. Checking the generation first at each park closes
  it.

### What a statement shape costs (DF-S1)

Verified on `da-q8-memory-bound-key` (branch
`worktree-df-statistics-coverage`) at `ee89842e`. Owned by
`docs/spec/sys-views.md` §6.1-§6.1b.

- **A write's read cost is in no column.** `rows_examined` and `pages` are
  NULL for an `UPDATE` and a `DELETE` - their walk is the dispatcher's and
  counts nothing - and for a query-layer `SELECT`, whose walks are not
  summed; a subquery's chain is counted nowhere. `page_reads` and the time
  split do cover them.
- **The cost columns cost a point read +0.44 us of server CPU** at
  `cores = 1` (+0.27 to +0.54;
  `bench/v3.4.13/results-cx-close-bisect-v3.4.13-1-g11f72ce9.md`), kept on
  the operator's word of 2026-11-19 and **not attributed**: folding the
  session's sample where a wait is booked recovered nothing
  (`bench/v3.4.13/results-df-close-remedy-v3.4.13-15-g9f588fa2.md`). Their
  share of an index-served read and of a write is not measured, nor
  anything at `cores` above 1.
- **`sys.statement_stats` and `sys.statement_histogram` copy every shape
  under each part's latch**, and a shape is 624 bytes, from 184: the delay
  a read puts on a part's writer (above) grew with it, unmeasured.
- **`sys.refusal_stats` assumes one writer a core** and checks nothing: two
  threads recording as one core lose counts. Its bound, 64 codes, is
  asserted against the wire registry's last entry as it stands, not against
  a count, which the registry does not have.
- **No cell** empties the stores across a process restart or a restore
  (an in-process restart keeps them, above), counts a deadlock's victim in
  `conflicts`, or reads `rows` under a KWP `max_rows`; the two-core cell has
  not run under `tsan`.

### Transactions and the read horizon (DF-S2)

Verified on `da-q8-memory-bound-key` (branch
`worktree-df-statistics-coverage`) at the commit that adds this section.
Owned by `docs/spec/sys-views.md` §6.1c, §6.6 and §7.

- **A view's age is its minter's statement head, not a clock read at the
  mint.** `read_horizon_age_us` and `view_age_us` overstate a view's age by
  the time from the statement's head to the mint: its parse and its
  catalog revalidation. The waits a statement takes before it runs - the
  commit ceiling, its core's run queue - are before the head, and a re-run
  after a lock wait takes a new one. No clock is read on a statement's path
  for the stamp; a KWP load chunk, which is no staged statement, reads one a
  chunk. A statement resumed after a park mints, if it mints then, under
  the head of whichever statement its core staged last.
- **The horizon's three fields are not one snapshot.** Each core publishes
  its bound and its stamp in two stores, and a reader pairs them unguarded:
  a core in the middle of a mint answers the bound it lowered with the
  stamp its slot held before, `none` where it held no view.
- **A reader of the fields reads its own view** where nothing older is
  held: no `SELECT` can see `none` for the horizon's LSN or its core. The
  age reads `none` on an instance with no clock.
- **A view the engine holds outside a session has no row in
  `sys.session_state`**: a declaration build's lock-holder transaction is
  in the horizon's fields and in no session's `view_age_us`, and a session
  the listener has not run a statement for has no row at all.
- **The counts assume one writer a core** and check nothing, as
  `sys.refusal_stats` does. `commits` is counted by the transaction
  manager, so a fixture's manager that names no core counts into core 0.
- **Measured with DF-S3 and DF-S4, not apart**: the three stages cost a
  tuple key `SELECT` +0.07 us of server CPU at `cores = 1` and a write
  nothing the floor shows
  (`bench/v3.4.13/results-df-close-final-v3.4.13-17-gc16b4fc2.md`); a knob
  run could attribute none of it to the stamps or to the session's words
  alone. `cores` above 1 did not resolve.
- **`wait_aborts` takes a statement whose descent noted a stale path**
  and which then failed, having written nothing, for another reason: the
  site tests the note, not the refusal's code, and such a statement is not
  run again. Read, not reproduced.
- **No cell** drives a failed `COMMIT`, a parked `strict` commit whose sync
  never comes, a load stream's failed chunk or a structural re-run into
  `aborts` and `wait_aborts`, or reads a load chunk's stamp; the two-core
  cells have not run under `tsan`;
  and no cell checks that a process restart or a restore empties the store
  (an in-process restart keeps it, as it keeps the BO stores above).

### Index and relation usage (DF-S3)

Verified on `da-q8-memory-bound-key` (branch
`worktree-df-statistics-coverage`) at the commit that adds this section.
Owned by `docs/spec/sys-views.md` §6.2a and §6.2b.

- **`sys.access_stats` has no `rows_examined` and `rows_matched`.** DF's
  part C names them. The format event that stood in the way went with DA-S1,
  which moved the store to memory, but DA changes nothing that is counted
  (its §6, DA-R1), so it did not add them either. **No owner.** (Re-read on
  `da-access-stats-memory-resident` at DA-S1.)
- **An index a write's `WHERE` subquery reads through is counted nowhere**:
  that chain's counters are dropped where it runs. An index that reads 0
  is therefore not proven unused by the count alone; neither is a unique
  index, nor a foreign key's.
- **`lock_deadlocks` is not every deadlock**: the two paths that write no
  `sys.lock_waits` row (a victim whose slot had flipped, an online build's
  drain) are in no column.
- **`last_used` is a statement's head, not the run's own instant**, and a
  statement resumed after a park stamps the head of whichever statement its
  core staged last. An index whose slot in the dispatcher's eight-slot cache
  another index took pays the part's latch and a lookup at its next read.
  No cell puts two indexes in one slot, or swaps the store under a cache.
- **An index-served read pays +0.18 us of server CPU** for DF-S2..S4 at
  `cores = 1`, +0.11 us over a tuple key read's share
  (`bench/v3.4.13/results-df-close-final-v3.4.13-17-gc16b4fc2.md`): what is
  left of the fold after its latch, lookup and clock read went (-0.18 us),
  the per-seek counter and the wider per-step counters, not told apart.
- **No cell** counts an index step under an outer step (one walk a row of
  the outer), a sub-chain's or a hoisted step's, or a step that named an
  index and walked its relation instead (`indexes = off`, an index dropped
  under a compiled chain); the two-core cell has not run under `tsan`.

### Space (DF-S4)

Verified on `da-q8-memory-bound-key` (branch
`worktree-df-statistics-coverage`) at the commit that adds this section.
Owned by `docs/spec/sys-views.md` §6.2c and `docs/spec/plan-stats.md` §6.

- **`est_delete_marked` exists only for a relation the steward seeds**: one
  a searched join block named, or whose prior a clean shutdown carried. A
  relation read and written by single-relation statements alone reads NULL
  for good. Nothing gathers it on request; `GATHER STATISTICS` counts live
  rows and does not fill it.
- **It is a sample of at most four leaves** above height 1, scaled by the
  prior's leaf count: deletes clustered in leaves the sample missed read 0,
  and ones clustered in a sampled leaf read many times their number.
- **An append is counted nowhere**, so `leaf_divisions` says nothing of how
  fast a tree grows; `leaves` does.
- **The count is taken under the divided leaf's exclusive hold**: a latch
  of the store's and, at a tree's first division on a core, an allocation
  (DF-Q22 (a)). A reader of the view holds that latch for its copy of the
  part, which allocates, and a division on that core waits for it. Not
  measured.
- **`est_delete_marked` can be older than the relation's last seed**: a
  seed that read no leaf keeps the estimate the one before it made.
- **`leaf_divisions` and `leaf_compactions` are the process's**, not the
  instance's: an in-process restart keeps them, as it keeps the BO stores.
- **No cell** samples a tree of height 2 or more for delete-marked rows,
  counts a division on a second core, drives a division that fails, or
  reaches the store's bound.

### A row is one binding's (DI-S1)

Verified on `di-shape-identity` (branch `worktree-di-shape-identity`) at
DI-S1's commit. Owned by `docs/spec/sys-views.md` §6.1, §6.1a and
`include/kds/stats/statement_binding.hpp`.

- **A `SELECT` the native grammar does not parse has no fetch id.** A parse
  that stops in the middle answers no fingerprint (`parser.hpp`), so a set
  operation and every other form only the query layer reads is kind
  `SELECT` at `fetch_id = 0`. Since DI-S1 that is one row a binding, so no
  row sums statements over different relations; it still sums every such
  text over the same relations, and its `shape_text` is the first one
  seen. Giving such a statement its own fetch id is not built.
- **A statement refused part way through its bind is the row of what it
  had declared by then**, not the row its text has once it binds whole: a
  join whose second relation does not exist is counted under its first.
- **More than 16 distinct relations are not told apart**: one `over` row a
  shape.
- **A dropped relation's rows stay until a restart** and count against
  the part's 4,096 (DI-Q7 (c)): a workload that drops and creates relations
  fills a part with rows no call can reach, and the shapes after them are
  dropped and counted. A reclaim was built at DI-S1 and taken out at the
  close: run where a part needs room - a statement's own record - it
  scanned the part and read the catalog on that statement's path, which
  `rules.md` §3a forbids. A reclaim off the statement's path is not built.
- **DI's cost on a write is not resolved.** At the close
  (`bench/v3.4.15/results-di-close-v3.4.15-12-g871845e4.md`) no cell was
  slower outside its own spread, the point reads were level, and a keyed
  `INSERT` and the FK insert read about 0.4 us dearer at `cores = 1` in
  10 and 9 of 12 lifetimes - inside the spread, on a host with no counter
  to place it. Not chased.
- **A shape's slot grew to 768 bytes from 624 and its key to 168 from 24**,
  so the copy a view read makes under each part's latch grew with them
  (above), unmeasured.
- **`sys.ash`'s `fingerprint` is the text's** (DI-Q9 (a)): a reader who
  joins a sample to `sys.statement_stats` by it meets every binding's row.
- **No cell** drives `ResolveNames`' second resolve through a statement
  (the rewind is pinned on the binding alone), or a refused commit's and a
  refused page wait's rebuilt reply carrying the binding; the walk's
  rebuilt reply is pinned on the two-core rig.

### The Cabin controller's signal is a candidate's (DI-S2)

Verified on `di-shape-identity` (branch `worktree-di-shape-identity`) at
DI-S2's commit. Owned by `docs/spec/cabin-controller.md` PO7c.

- **A candidate is a relation and a column position, not a column.**
  Nothing tells the signals of a `DROP COLUMN`, so a position `ADD COLUMN`
  reuses after `PURGE DROPPED COLUMNS` inherits the dropped column's
  decayed heat until it decays. Found at DI-S0; not a text identity and
  not fixed by DI.
- **A fingerprint whose candidacy moves between two columns of one
  relation is two signals**: the old one decays where it was overwritten
  whole before DI-S2. No decision is priced with more heat than before the
  move.
- **One fingerprint is now as many signals as the relations it targets,
  plus one for its executions with no candidacy, under the same 4,096
  cap** (`kMaxTrackedFingerprints`, `[PROPOSED]`, unmeasured): 41 texts
  under 100 namespaces pass it where 41 entries stood before. Past the cap
  every new signal evicts the coldest - commonly another newcomer, so
  neither builds a count - and pays the 4,096-entry scan under the
  collector's latch, on the success path of every `SELECT`. The eviction
  rule is unchanged; its reach is what moved.
- **The execution feed runs with the controller off**, under its latch
  above one core (`RecordOptimizerSignals`): only `Tick` and the wait
  samples follow the switch. Unchanged by DI.
- **A dropped relation's signals are not forgotten at the drop**: they
  name an oid no statement can bind again, decay, and are evicted when the
  table is full.

## Wait events and ASH (CJ)

Verified on `cj-wait-events-ash` at the milestone's close
(`instructions/v3.2.0/workorder-cj-wait-events-ash.md` §8).

- **`sys.ash` never shows ON_CPU** (CJ-Q12). The sampler runs between polls, so a
  task that is using CPU reads RUNNABLE; the exact figure is `sys.wait_stats`'
  `cpu.on_cpu`. Weaker than Oracle's ASH for CPU attribution.
- **`ash_capture_arg_hash` does nothing outside its unit test.** The only hash
  the engine has, `fingerprint->arg_hash`, is taken after a KWP bind is
  substituted into the text, so it covers bind values, which WD11 forbids
  recording. Publishing it waits for the operator's ruling (a hash of a
  low-entropy value can be reversed).
- **Records not filled**: `trx_id`, `step_id`, `origin_session` and the
  cross-core flag are always 0 and `sys.ash` does not show them. `xcore.*`,
  `res.eviction_retry` and `sysio.writeback` have no booking site; several
  events have unfilled parameters (`docs/spec/wait-events.md` marks each).
- **An unscoped park** on a core that is not strict is counted
  (`unscoped_parks`), booked to no event, and leaves the task WAITING on
  `cpu.on_cpu`.
- **A reactor's idle block is not capped at the next sample time**, so a batch can
  read as late by up to `max_idle_block_ms` (10 ms), which at the 100 ms minimum
  interval is exactly the late threshold.
- **`session_id`** is a private counter, not the KWP handshake id; a session's
  rows in `sys.session_waits` do not join to a client-visible id.
- **The settings are process-wide** (`SET ASH`, the `ash_*` keys): two instances
  in one process share one kill switch.
- **A `sys` view answers no aggregate** (AG12), so a panel counts client-side.
  KDS Studio is not in this tree; `tools/ash_panel.sql` is the data contract.
- **`sim/` drives no core runtime**, so the I2 assertion is checked over the unit
  and integration suite, not the simulation.
- **S4**, the Cabin controller's wait signal (WE13, `[EXPERIMENTAL]`), is counted
  and shown in the snapshot and read by no decision.

## Streamed replies (CE)

Verified on `ce-streaming-reply` at `9e568b44` (CE-S1..S4). Owned by
`docs/spec/protocol.md` §7; the order is
`instructions/v3.2.0/workorder-ce-streaming-reply.md`.

- **A parked walk is bounded only by its socket.** A client that stops
  reading holds the walk's read view (the purge horizon), its borrow (every
  DDL taking the relation `X` waits, refused `TxnConflict` at the 1 s fault
  net) and its core's reader-lease slot for as long as its socket lives -
  about twice `tcp_keepalive_s`, and without bound with keepalive off and a
  live client. Two consequences a yield of one turn never had:
  - **the transaction lifetime ceiling does not reach it** (CE-A7):
    `ExpireOverdueTransactions` skips a connection with a statement running
    (`tcp_server.cpp`), so an explicit transaction outlives the 60 s
    ceiling with it;
  - **256 parked walks fill their core's reader-lease slots**
    (`txn::kMaxRegisteredReaders`, `sched.md` §3), and a further walk's
    head on that core is refused `OutOfSpace` until one ends.
- **Only a walk longer than one slice streams.** Rows leave at the walk's
  slice boundaries (64 pages) and only once a 64 KiB batch has sealed, so a
  reply whose walk ends inside its first slice - the scenarios' shapes of
  1,000 rows or fewer (`bar-range`, `model-join`, `books-by-genre`; of the
  measured many-row shapes only `symbol-history` and the filtered walk
  cross a boundary) - is sent at completion as before. CE-S0 bounds what
  streaming can hide at 5-12 % of the wall time of a statement of 10,000
  rows or more on the scenario client
  (`bench/v3.3.0/results-ce-s0-premise-v3.3.0-50-g1f57f37b.md`).
- **A cancel of a parked walk is read on its core's next poll**, bounded by
  the idle block (`max_idle_block_ms`); nothing kicks the core. No cell
  covers `C_CANCEL` against a parked walk; the hangup cell covers the
  closing path, which the same predicate reads.
- **The two park cells assume the host's send buffer** (`tcp_wmem`'s
  maximum, 4 MiB here) is well under their 12.8 MB reply; on a host tuned
  above it the walk ends without parking and the cells fail.
- **What `ce_streaming_reply_test.cpp` does not cover**: the keepalive user
  timeout ending a park and the horizon held while parked (both CE-S4
  asked for; the hangup cell covers the closing path and the borrow's
  release); a join, an index or Cabin fallback walk, or `LIMIT` streaming;
  and CE-S1's byte-identical reply for every shape that does not stream -
  only the capped portal has a cell.

## `READ ONLY` transactions (CS)

Verified on `cs-read-only-transaction` at `adbea55b` (CS-S1, CS-S2). Owned
by `docs/spec/txn.md` §1 ("The access mode"); the order is
`instructions/v3.2.0/workorder-cs-read-only-transaction.md`.

- **A `READ ONLY` transaction holds a reader-lease slot for its whole
  life**, idle time included, and shares the core's 256 with every
  suspended `SELECT` walk (CS-R6, CS-Q5 (a)). 256 idle read-only
  transactions on one core refuse that core's next walk head and next
  `BEGIN READ ONLY` `OutOfSpace` until one ends - at the latest at the 60 s
  lifetime ceiling. No measurement has shown starvation; none was taken.
- **Only the declared form is cheap.** An undeclared `BEGIN` that only reads
  still draws an id, logs `TXN_BEGIN`/`TXN_COMMIT` and, under `strict`,
  syncs at `COMMIT` (CS-S0: `bench/v3.4.11/results-cs-s0-premise-v3.4.11-3-gbbaff33a.md`).
  Lazy id assignment is not built; its census is
  `docs/pending/lazy-id-assignment-census.md` (CS-Q9).
- **KWP has no access byte.** `C_TXN_BEGIN{durability}` opens a `READ WRITE`
  transaction (`READ ONLY` on a `readonly` connection), and a KWP client
  asks for `READ ONLY` by sending the statement text (CS-Q4 (a)).
- **The refusal is on the routing token, not the parse.** An admitted verb
  that a later grammar let carry a write (a data-modifying `WITH`, say)
  would pass the head gate. `BeginWrite`'s second line refuses it there,
  but only on a route that opens a write scope.
- **What the cells do not cover**: a read-only transaction across a
  session move. None can happen: the placer moves only a session with no
  transaction. Also uncovered: `C_CANCEL` of a walk inside one, and a
  `READ COMMITTED` read-only transaction whose lease is re-pointed while
  another core's undo purge runs. The purge cells cover `PURGE`'s horizon
  gate, not the undo purge's.

## Plan statistics (CY)

- **Relation priors and managed objects are kept across a clean shutdown
  only.** Verified at CY-S1b on `cy-managed-statistics`. After a crash or a
  restore `sys.statistics` is empty: the planner prices every relation at
  `kAssumedRelationRows` until the steward's first publish, as before CY-S1
  (`docs/spec/plan-stats.md` PS16), and every gathered, imported, pending or
  locked object and its history is gone - `EXPORT`/`IMPORT` is how a model
  is carried across either. Posteriors and decline facts are never kept.
  Owned by `plan-stats.md` §8, §9.

- **A `GATHER` is behind OLTP by its slicing only.** Verified at CY-S1b on
  `cy-managed-statistics`. Its walk yields every 16 leaves and holds the
  relation's `IS` a slice at a time (PS20), but it stays on the session's
  core - a session moves only at a transaction head (BL-S3), so it is not
  put on a batch core unless the session declared `SET PRIORITY
  background` first - and its page reads come through the buffer pool, so a
  gather of a cold relation larger than the pool turns the pool over (CY-R10
  asks a page budget that does not). Its pace (CY-Q31 (a), 16 leaves per
  2 ms at most) is a floor and not a period: nothing wakes the park at its
  deadline, so on an idle core a slice waits for the reactor's idle-block
  ceiling (10 ms) and a gather runs at about a fifth of its ceiling; the
  fix is a scheduler timer the dispatcher has no handle to arm. No cell runs
  the paced branch - every gather fixture has no clock - so it is covered by
  CY-S5's measurement alone. Owned by `plan-stats.md` §9.

- **No column or column-group statistics** (the operator's W13, CY-S2 and
  CY-S3 not built), so selectivity is still the three constants of
  `exec/join_order.hpp`; and **no `ADVISE PLAN ... USING PENDING`**
  (CY-Q30 (a)): a pending version's effect on a plan is not shown before it
  is published. Owned by `plan-stats.md` §9.

- **The shutdown's write is covered by hand-driven cells only.** Every
  restart cell calls `Expeditor::WriteRelationPriors` itself, then the
  clean stop's checkpoint; none runs `RunUntilStopped` to its tail, so
  removing the call there would pass the suite. No cell crashes a device
  between the shutdown's page writes and its map write - the damaged-chain
  cells build that state on the in-memory store, and one tears the head on
  a file. The simulation harness's shutdown does not call the write, so
  the corpus does not exercise the relation. Owned by `plan-stats.md` §8.

- **The closing mark is a count, not a checksum.** A chain that mixes
  pages of two shutdowns and still reads whole needs a cut-off page handed
  back to the chain and a power loss between two of its page writes; it
  would seed stale priors, which price an order and never change a row.
  Owned by `plan-stats.md` PS16.

## Captured plans (CX)

- **A capture is lost if the instance crashes within about a second of
  it.** Verified at CX-S1a on `cx-plan-change-option`. A capture is in
  memory at the compile and written to `sys.plans` by core 0's `system`
  task, once a second (CX-Q14 (a): a statement writes no log). A crash in
  between loses it, and the shape is captured again at its next compile,
  from the statistics of that run - so it may run one order before the
  crash and another after. The row is logged but its write is not synced
  on its own: it is durable with the next commit's sync or the next
  checkpoint, so under an instance that only reads, the window is the
  checkpoint interval. `captured_pending` counts what is waiting. Owned
  by `parser-v2.md` I12.

- **A captured plan has no state of its own.** `DISABLED` is not built
  for one, and `INVALID` (CX-Q16) only as DI-S4 derives it - a rename of a
  relation, or of a column written by its binding, read off the row. A
  captured order that stops compiling for another reason
  is declined statement by statement and counted, its row unchanged, and
  the only acts on it are `DROP PLAN` and a `CREATE PLAN` over its shape
  (`ALTER PLAN` on a captured row is refused `NotImplemented`). Verified
  at CX-S1b on `cx-plan-change-option`. Owned by
  `instructions/v3.2.0/workorder-cx-managed-plans.md`.

- **An administrator's plan is checked against one statement, once.**
  Verified at CX-S1b on `cx-plan-change-option`. `CREATE PLAN` compiles
  each variant's order for the statement as written, with the example's
  literals; another binding of the shape can still meet a variant that
  does not fit it (a `LIMIT` that no longer bounds an added sort is the
  case with a cell), and that statement runs its written order, counted in
  `plan_declined`. **`ALTER PLAN ... REPLACE VARIANT` compiles nothing**:
  the row holds the shape and no literal, so the new order is checked only
  as a permutation of the plan's bindings and the rule only against the
  argument count. A replaced order that fits no statement is declined at
  every compile and visible only there. Owned by `parser-v2.md` I12.

- **A declined variant marks nothing.** CX-Q4 (b) reads "mark it
  `INVALID` and run the default plan". Built: the statement runs its
  **written** order - not the shape's captured order or the search - the
  decline is counted and named by `ANALYZE` and `sys.statement_stats`
  (`admin_declined`), and the row's state does not move, because a decline
  can be one binding's (CX-Q26's reasoning). `invalid` is derived: a
  relation the plan names is gone, or its fetch id is another fingerprint
  version's. A plan every statement declines therefore reads `enabled`.
  Owned by `parser-v2.md` I12.

- **Plan DDL does not wait for plan DDL.** `CREATE PLAN`, `ALTER PLAN` and
  `DROP PLAN` take one claim for the instance and a second one meanwhile
  is refused `TxnConflict` at once, where every other DDL waits up to the
  lock family's fault net. And they hold their relations' `IS` by a
  non-queueing ask: beside a `DROP TABLE` that holds the `X` they are
  refused `TxnConflict` rather than waiting for its decide. No cell runs
  plan DDL from two cores. Owned by `exec/admin_plans.hpp`.

- **Every `ALTER PLAN` leaves its old row's text in `sys.plans`' var-heap
  until the next mount's sweep**, as a dropped declaration's does: an
  `ALTER` is a new row and the old one's removal, so an agent that tunes a
  plan's thresholds in a loop grows the var-heap through the run. Not
  measured. Owned by `exec/plan_catalog.hpp`.

- **A multi-variant plan lexes the statement a second time at every
  compile**, to read its arguments (`parser::ArgumentsOf`); a one-variant
  plan does not. And while any plan is enabled, every top-level join
  block's compile on every core probes the administrator's table - one
  partition latch - a tuple key chain's included, which the captured table
  is never asked of (CX-R6 as amended in the stage log). Neither is
  measured; the overhead measurement is owed at CX's close. Owned by
  `exec/admin_plans.hpp`.

- **A block run in an order other than its written one compiles twice,
  and there is no plan cache.** Verified at CX-S6 (`29401d0f`) on
  `cx-plan-change-option` (`bench/v3.4.12/results-cx-s6-compile-share-v3.4.12-112-g9712f5f0.md`
  §7). `CompileBlock` compiles the written order first - its refusal is
  the statement's and it is the decline's fallback - and then the order in
  force, so a captured order or a plan's variant that is not the written
  one pays two compiles: 5.3 µs against 2.5 µs for a three-relation join
  in release, in-process; a two-variant plan pays 4.2 µs against 1.6 µs,
  the second lex included. **S6 built no compiled-chain cache** (CX-Q37
  (a)), and in the server the plan cells are where that costs: the
  server's own `exec::Compile`, the most a cache could remove, is 5.7 %
  (`cores = 1`) and 6.3 % (`cores = 2`) of a two-variant plan's client p50
  and 5.3 % of a one-relation plan's at `cores = 2`, against 2.8 % to 4.1 %
  for every plain shape; against the server's own time every cell is over
  5 % (§8 of that file, a spike). Skipping the first compile is not built
  and is not free: the written order's refusal is the statement's. Owned
  by `instructions/v3.2.0/workorder-cx-managed-plans.md`.

- **With the cabin controller on and no plan naming its Cabins, the
  controller accelerates nothing.** Verified at CX-S3b on
  `cx-plan-change-option`. Since CX-Q6 (a) a controller-made Cabin is read
  through only by a statement whose administrator's plan names it
  (`cabin-controller.md` PO11), so a `cabin_optimizer = on` that used to
  turn a hot filter scan into a Cabin probe on its own now builds the
  Cabin and leaves the scan: someone - a DBA, an agent - has to issue
  `CREATE PLAN ... ACCESS <relation> CABIN (<column>)` for each shape that
  should read through it, and nothing proposes that plan (`ADVISE PLAN`
  is CX-S7). An unnamed Cabin is probed by no statement's step, so it
  observes no value, HEAL never fires for it, and an EXTEND - which a
  foreign key reverse check's misses on a child's Cabin can still
  trigger - finds nothing sighted to seed; it holds
  a `sys.cabins` row and a budget page while its fingerprint stays hot.
  An instance that ran with the controller on before this stage loses
  the serve at the upgrade, with no refusal: the statements walk. And a
  DROP a plan RESTRICTs is decided again at every tick while its
  condition holds - each one a skipped action, a `drops_restricted` count
  and a decision-log record - since the decision core is not told why it
  did not land; noise, not a wrong state. Owned by
  `docs/spec/cabin-controller.md`.

- **A one-relation plan puts every one-relation `SELECT` through the plan
  table, a point read included.** Verified at CX-S3b on
  `cx-plan-change-option`. While at least one enabled plan is for a
  statement over one relation, every top-level one-relation `SELECT` with
  a fetch id probes the administrator's table - one partition latch - and
  a statement the plan governs is compiled twice, unpinned and pinned.
  With none enabled the cost is one relaxed load. CX-R6's "zero cost for
  the unmanaged point path" therefore holds only while no one-relation
  plan is enabled. Measured at CX's close (`a32528a4`,
  `bench/v3.4.12/results-cx-close-v3.4.12-115-ga32528a4.md`): +0.17 to
  +0.25 µs of server CPU on a point read against a control's +0.06 to
  +0.11 µs, 0.5 % to 1 % of its throughput; an enabled join plan costs it
  nothing resolvable. Owned by `exec/admin_plans.hpp`.

- **Point statements are 1.2 % to 2.0 % slower at CX's close than where
  CX opened, and the cause is not attributed.** Measured at `a32528a4` on
  `cx-plan-change-option` against `8b584cf6`, `cores = 1`
  (`bench/v3.4.12/results-cx-close-v3.4.12-115-ga32528a4.md`): a point
  read and a two- and three-relation tuple key join spend +0.4 to +0.8 µs
  more server CPU, under `fixed` and `planned` alike, above the noise
  floor in two runs; join blocks are unchanged. CX-S0..S3a are within the
  floor; DF-S1's statement statistics, inside the same span, carry part;
  the rest falls in CX-S3b..S7, whose point-path additions read as
  nanoseconds. A bisect of `ee89842e..a32528a4` is owed. Owned by
  `instructions/v3.2.0/workorder-cx-managed-plans.md`.

- **A named Cabin pin is by the column's name, and its drop RESTRICT has
  a window.** Verified at CX-S3b on `cx-plan-change-option`. `ACCESS t
  CABIN (c)` stores `c` as written: after `ALTER TABLE ... RENAME COLUMN`
  the pin names no column and the RESTRICT no longer finds the Cabin, so
  it can be dropped under the enabled plan. No statement is moved by that:
  a pin is met only on a column the statement writes, so a statement of
  the plan's shape names the old column and no longer compiles, and the
  new spelling is another shape (the entry below). Only a variant `REPLACE
  VARIANT` gave a pin on a column its statement does not write - it checks
  the Cabin exists, not that the pin is met - is declined statement by
  statement (counted, `plan_declined`), before the rename as after. A column Cabin's `DROP
  CABIN` and the controller's DROP hold no relation lock, so a `CREATE
  PLAN` naming the Cabin that lands between their check and the row's
  removal is not seen: its variant is declined (CX-Q4 (b)), no result
  changes. An expression or predicate Cabin cannot be named
  (`NotImplemented`) and its drop is not RESTRICTed. Not reached by a
  cell: a pinned `CABIN` arm on an expression, predicate or cover Cabin;
  plan DDL racing a Cabin's drop; a one-relation plan under a streamed
  `SELECT` or across a restart. Owned by
  `instructions/v3.2.0/workorder-cx-managed-plans.md`.

- **A plan a rename put out of reach reads `invalid`, where the rename is
  established from its row** (DI-S4, `exec::PlanRowCanMatch`; verified on
  `di-shape-identity` at DI-S4's commit): a renamed relation, and a renamed
  column the statement writes by its binding (`b.col`). It then restricts
  no `DROP TABLE`. **Not established, so read as before** (DI-R9): a
  renamed column the statement writes **unqualified** - the plan reads
  `enabled`, still RESTRICTs, and matches nothing; a statement stored cut
  short; a captured order that stops compiling for any reason but those
  two. Nothing is written at the rename: the state is derived at each read
  of `sys.plans` and at the drop's RESTRICT, and the plan stays in the
  administrator's table and in the captured table, where no statement
  reaches it. A rename back puts it in force again. Also not established:
  **a statement the native grammar does not read with its literals as
  `0`** - any `LIKE`, whose pattern must be a string - and **a dropped
  column**, since a `DROP COLUMN` undecided would read as a rename. And
  what the derived state costs:
  - **`invalid` hides `enabled` and `disabled`** while the rename stands.
  - **A rename back after the drop's check orphans the plan**: over
    `(t, u)` with `u` renamed away, `DROP TABLE t` is let through; a
    `RENAME` of `u` back before the drop decides puts the plan in force
    over a relation about to be gone, where it then reads `invalid` for
    the gone relation. `CREATE PLAN` could not do that before DI-S4 - it
    waits for the drop's `X`.
  - **A pinned index's `DROP INDEX` and a named Cabin's drop are still
    restricted** by a plan that can match nothing.
  Owned by `parser-v2.md` I12.

- **What CX-S1b's cells do not reach.** A variant whose order does not
  compile for a statement (the decline has one cell, by the sort rule); a
  `CREATE PLAN` or `ALTER PLAN` refused because a `DROP TABLE` holds a
  relation's `X`, and a `CREATE PLAN` that meets a drop committed between
  its compile and its hold (`HoldPlanRelations` and the catalog check
  after it are read, not run: the rig's lock table is not driven from two
  sessions there, and the existence check reads this core's catalog memo,
  whose refresh inside a statement was not verified); `ReplaceAdminPlan`'s failure between its insert and its removal
  (the state it leaves is built by hand in a cell, and a mount's handling
  of it pinned); a plan over more than 16 relations (`Unsupported`); a
  mount that meets an administrator's row of another fingerprint version
  (no second version exists to write one). The simulation harness's
  instance neither loads nor makes plans. Owned by
  `tests/admin_plans_test.cpp`.

- **A row naming a dropped relation can outlive the drop until the next
  mount.** The persisting task removes such rows in the step after the
  drop commits; if that step's scan or its delete fails, the oids it took
  are not asked again in the run and the rows stay, inert - an oid is
  never reissued - until a mount removes them. **And a removed row's text
  stays in the relation's var-heap until the next mount's sweep**, as a
  dropped declaration's does: here the removal is the engine's, so a
  workload that creates, joins and drops relations grows `sys.plans`'
  var-heap through the run. A dispatcher with no log
  (a test rig) forgets nothing at a drop inside a transaction, its commit
  naming no LSN - in the captured table or the administrator's. Owned by
  `exec/plan_catalog.hpp`.

- **A mount refuses on a `sys.plans` it cannot scan or cannot delete
  from**, as it does for any logged catalog relation, although the state
  lost would cost only a recapture. A row it can scan and cannot read is
  skipped. Not ruled on by the operator. Owned by `exec/plan_catalog.hpp`.

- **The persisting task's failure path has no cell.** A step whose insert
  fails puts the rest back on the queue; the table's own cell pins the
  requeue, and no cell makes `InsertDeclaration` fail under the step. No
  cell runs the step on its `system` tick or at the shutdown tail - every
  cell calls `Expeditor::PersistCapturedPlans` itself - so removing either
  call would pass the suite. The writeback run's pin allowance has no cell
  of its own: four simulation cells abort without it. The simulation harness's instance neither
  loads nor persists plans. Owned by `exec/plan_catalog.hpp`.

- **A shape whose relation never gets a prior is searched at every
  compile, with no memo.** A capture waits for every relation of the block
  to have a published prior; until then the block is searched as under
  `planned` and, by CX-Q3, without the order memo, so each compile pays up
  to 24 compiles of the block. The steward seeds a wanted relation within
  its next steps, so the window is seconds - but a relation whose seed
  keeps being discarded (a page whose type or owner does not match) keeps
  its shapes there for good, and a shape first compiled while a DDL is
  held open stays there until it closes, both counted in
  `capture_deferred`. Not measured.
  Owned by `parser-v2.md` I12.

- **Four of the capture's paths have no cell that reaches them through a
  compile.** A captured order that does not compile for a statement (no
  statement that produces it was found; the other cause of
  `capture_declined`, a sort a larger `LIMIT` no longer bounds, has its
  cell), a block of more than 16 relations (the unkeyed path is reached by
  a missing fetch id only), a compile that loses the install to another
  core between its search and its capture, and one that loses the table's
  last free slot there: the table's own cells pin first-install-wins and
  the ceiling, and the compile's handling of each answer is read, not run.
  No cell runs two reactors' statements under `fixed`, and the simulation
  harness's instance runs `planned`, a bare dispatcher's value. Owned by
  `parser-v2.md` I12.

- **A capture can be made by a compile the statement then discards.**
  `ResolveNames` compiles once unfiltered and, if a DDL opened meanwhile,
  again under a minted view; the first pass may capture before its answer
  is thrown away, so the order was priced against a catalog that may hold
  an uncommitted DDL's rows - an index that rolls back, say. Oids are never
  reissued and every order returns the same rows, so no result is at risk;
  the shape keeps an order chosen in that window until a restart. Owned by
  `parser-v2.md` I12.

- **A shape running uncaptured is visible only in aggregate.** `SHOW
  META` counts deferred, unkeyed, over-ceiling and declined blocks for the
  instance and `ANALYZE` names the reason for one statement; no view lists
  the shapes. Owned by `instructions/v3.2.0/workorder-cx-managed-plans.md`
  CX-R11.

## Key sets (CU)

Verified on `cu-key-set-access` at `04d9afe9` (CU-S3b); the measured
numbers at `70a307ea` (CU-S4, the close). Owned by `docs/spec/parser-v2.md` I10; the order is
`instructions/v3.2.0/workorder-cu-key-set-access.md`.

- **A Cabin set is served only when every value is already observed, and
  a list observes nothing** (`cabin.md` §4e). `sym IN ('a', 'b')` walks
  until single-value statements have banked both sets, and walks again
  whenever one is dropped. An expression or predicate Cabin, and a
  correlated list, take no set.
- **A list on a join's inner relation is a filter wherever the join key
  enters that relation by an index or a Cabin** (CU-Q13, `index.md` §9).
  `o.user_id = u.id AND o.status IN ('open', 'held')` enters `(user_id)`
  per outer row as before and filters by the list; an index on
  `(user_id, status)` is entered by its leading column only. A correlated
  probe carrying a list on a later prefix column is not built.
- **An index set displaces a literal Cabin probe and a strict tuple key
  range**, as a single index probe does (`index.md` §9: an index ahead of
  the Cabin). `sym = 'a' AND v IN (1, 2)` with a Cabin on `sym` and an
  index on `v` took the Cabin before CU and takes `(v)` by set now.
- **A list that misses is counted against its Cabin and can never be the
  statement that ends the miss.** Each run adds a `cabin_misses` and, on a
  managed Cabin, a coverage miss the controller reads toward EXTEND
  (`cabin-controller.md`); the walk's pages count toward that Cabin's
  benefit; and a shape that was a filter scan before CU-S2
  (`sym IN (...) AND region = 'x'`) now names `sym`'s Cabin as its
  candidate, so the controller no longer prices a Cabin on `region` from
  it.
- **Two declines have no cell**: a set step whose bounds do not match its
  index's entry width (`RunIndexStep` walks; no compile produces one), and
  a Cabin set asked while a recording of one of its values is in flight
  (the value reads as unobserved and the step walks).
- **One list an index probe.** `a IN (...) AND b IN (...)` on an index
  over `(a, b)` enters it by one list and filters by the other; no cross
  product of the two is formed.
- **A list that reaches a longer prefix displaces a single probe** (IX9).
  With indexes on `(v)` and `(g, v)`, `v = 50 AND g IN (0, 2)` enters
  `(g, v)` once per value of `g`, where before CU it entered `(v)` once.
  The longest-prefix rule is unchanged; a long list makes it cost more
  descents than the probe it replaced. At one equality prefix length a
  list never displaces a single equality (CU-Q10).
- **No item an index's key encoder refuses was found to construct**, so
  the rule that such an item declines the list for that index
  (`IndexChoiceOver`) has no cell. Coercion refuses a literal its column
  cannot hold before the encoder is asked.
- **A write whose `WHERE` holds a subquery walks its relation twice,
  whatever key the clause names** (CU-S3b). `UPDATE` and `DELETE ...
  WHERE id = 1 AND v = 3` and `... WHERE id IN (1, 2) AND v = 3` descend;
  `... WHERE id = 1 AND v IN (SELECT ...)` reads every row in a
  collection pass and then visits every slot of every leaf under a write
  latch (`WriteWalk::CollectTargets`), because a write evaluates no
  subquery under the leaf it holds exclusive. Keying either pass is
  deferred on the operator's word of 2026-11-17
  (`docs/pending/keyed-write-beside-a-subquery.md`).
- **A write over a bound on the tuple key walks the relation.** `UPDATE
  ... WHERE id BETWEEN 10 AND 20` declares its lock window off the bound
  and finds its rows by visiting every leaf: a write's walk has a resume
  position and no end. Before CU and after; in no stage.
- **A list on a relation the write only reads** (`UPDATE ... FROM`,
  `DELETE ... USING`, a subquery, `INSERT ... SELECT`) is keyed.
  `PURGE` by list is refused `NotImplemented` and is in no stage.
- **A write finds its rows through no index and no Cabin**, for a list as
  for a single value: `UPDATE ... WHERE owner IN (1, 9)` walks the
  relation whatever index `owner` has. CU-Q5 names the point-write
  target; an index-driven write is no stage of CU.
- **A write by list holds its core for all its descents**, as every write
  does for its whole walk: no write yields (`sched.md` §3). It takes a
  row unit per row written and no window, so a list naming more rows than
  `max_locks_per_txn` admits is refused at the cap, as the walk of it was;
  a bound on the tuple key beside the list declares a window, and the
  list is then written under it.
- **What CU-S3's cells do not cover**: a write by list while another
  core's writer divides the leaf a descent lands in (the descent and the
  row function are the point write's, whose cells cover it); a descent
  that declines on a `DELETE` (the cell fails a fetch under an `UPDATE`,
  and the loop is one function for both); and a list write resumed after
  its walk took over from a declined descent. **CU-S3b adds none of its
  own for** UPDATE's unique-key round (BX-R4) under a list beside a
  conjunct, which is the bare list's loop and has that cell.
- **The shape is exact.** `id IN (1, 2) OR id = 3`,
  `(id = 1 OR id = 2) OR id = 3` and a list under `NOT` or mixed with
  another column are not keyed and walk. A client that wants the set
  writes one flat list.
- **A set is taken at every relation size** (CU-Q3 (a)), and on a small
  relation the walk it replaces is cheaper. Measured at the close
  (`bench/v3.4.11/results-cu-close-v3.4.11-40-g70a307ea.md`):
  on the tuple key a set is slower at 1 and 10 rows from 64 values up (the
  10-row 64-value cell inside its spread), by up to 1.1 ms of server CPU
  per statement at 4,096 values, and faster at 100 rows; on an index it is
  slower at 1 and 10 rows from 4 values up (the 1-row 64-value cell inside
  its spread) and
  at 100 rows from 64 values up, by up to about 3.6 ms (100 rows, 4,096
  values; CU-S0 estimated 3.7 ms). At 1,000 rows a set is faster in every
  cell measured.
- **A statement driven by a set never yields, takes no cancel and
  streams no row before its end** (CU-Q8 (a)). It is run inline, like one
  driven by a single descent, and a set on an inner step runs whole per
  outer row. For a one-step statement CU-S0's estimate for the largest set
  the grammar admits, 4,096 values, is 1.6 to 4.2 ms of descents on top of
  about 4 ms for parsing the list, which the walk also spent before its
  first slice. **For a join the bound is the values times the rest of the
  chain**: each value's row runs every later step before the next descent,
  so a set of 4,096 rows over an inner step that walks 100,000 rows (one
  with a boolean conjunct, which takes no build) holds its core for about
  4,096 walks. Before CU the same statement's driver was a walk, which
  yielded and took a cancel every 64 of its own pages and sent its rows at
  those boundaries (CE), so the worst case lost its yields, though they
  were already as far apart as the matches in 64 pages. `exec::Budget`
  bounds both. CU-Q8 (a) was marked on the one-step bound; the join bound
  was found by CU-S1's review. **Measured at the close, one step**: a
  4,096-value set holds its core for 8.3 ms of server CPU on the tuple key
  over 100,000 rows and 20.0 ms on an index, the list's own 4.5 to 5.3 ms
  included (`bench/v3.4.11/results-cu-close-v3.4.11-40-g70a307ea.md`).
  The join bound is not measured.
- **What the close did not measure** (CU-S4): a Cabin set; a set on a
  join's inner step; `cores` above 1 or a second session, so the leaf
  holds a keyed write no longer takes have no number beside them; a
  `DELETE` by key or by list on its own (it is measured with the `INSERT`
  that feeds it). The overhead off the set's shape is "none found", not
  "none": the largest cost the run does not exclude (the pooled delta
  plus two standard errors) is 0.2 to 4.8 % of a statement's server CPU,
  4.8 % on a tuple key lookup, and 3.0 % on the statements that reach
  CU-R5's per-row tests (a boolean conjunct on a walked step, a recorded
  and replayed trail).
- **Each list length is its own statement shape.** The fingerprint hashes
  the token stream, so `IN (1, 2)` and `IN (1, 2, 3)` have different
  `fetch_id`s and their own rows in `sys.statement_stats`,
  `sys.access_stats` and the plan statistics. CU did not change this; a
  client that sends lists of many lengths fills those stores faster than
  one that sends a few.
- **The Cabin controller no longer sees a filter beside a list.**
  `id IN (...) AND sym = 'x'` was a filter scan and a Cabin candidate for
  `sym`; its step is a lookup now and feeds no candidate, as `id = 5 AND
  sym = 'x'` never did.
- **What the cells do not cover**: a set read while a writer on another
  core divides the leaf it descends to (the descent and `AcceptTupleAt`
  are the single lookup's, whose cells cover it); a KWP portal read with
  `max_rows` over a set-driven statement; and whether a recorded trail
  holds no entry for a set step, which is pinned only through
  `IsTrailReplayable(const Step&)` and the absence of `replays=`.

## Ordered walks (CV)

Verified on `cv-ordered-index-walk` over `3ff0b377` (CV-S1). Owned by
`docs/spec/parser-v2.md` I11; the order is
`instructions/v3.2.0/workorder-cv-ordered-index-walk.md`.

- **A root growth on another core makes the rest of a reverse walk pay
  hops**: the statement's memo names the root it bound, so each later step
  descends from that root into what is now the left subtree and moves
  right, under the leaf's hold, to the leaf it wants - one hop per leaf of
  the subtree's right part, per step. Correct (the rig cell grows a level
  under the walk), not measured; a LIMIT keeps it short, and a filter that
  matches nothing walks it whole.
- **A one-sided bound does not seed the reverse walk**: `WHERE id < ?
  ORDER BY id DESC LIMIT 20` - keyset pagination - starts at the relation's
  top and filters every row above the bound, because a one-sided bound is
  no tuple key range (`TupleKeyRangeOf`). `BETWEEN` seeds it.

- **A reverse walk's leaf miss is a blocking read on its reactor.** DE
  gives the forward walk's leaf fetch a read off the reactor and a park
  (`page.md` §12); the reverse walk (O2) fetches each leaf, and makes each
  step's descent, as a blocking read. A resident relation pays nothing for
  it; a cold one stops its core for each leaf it misses, as every walk did
  before DE.
- **A reverse walk makes one descent per leaf** (CV-Q1 (a)): no left
  sibling link exists, so each step left is a seek from the last leaf's
  `min_key`. The join planner prices an elided chain at the rows its LIMIT
  lets out and does not add those descents.
- **The reverse walks are priced without their descents**: CV §2 asked
  `EstimateChain` to add one descent per leaf for a reverse walk (O2, O3);
  it adds none, pricing an elided chain at the rows its LIMIT lets out, as
  a forward one. And a reverse run's `pages_fetched` counts the leaf it
  reads, not those its move right passes over.
- **`ORDER BY <tuple key> DESC` without a `LIMIT` is still a sort**
  (CV-Q2 (a)), and so is one over a key set, a build or a Cabin arm.
- **An ordered index step's decline holds every match** (CV-S2, `index.md`
  IX8c): under `indexes = off`, or an index gone under the chain, the walk
  collects each matching row's key and tuple key, sorts them and resolves
  each one - memory in the matches, as IX8a's collection, and a descent per
  row where the walk would have emitted in place. Bounded by nothing but
  `exec::Budget`, which it charges twice for each row (the walk's decode
  and the resolve's), so under `indexes = off` a budget is met at about
  half the rows `plain` reaches - a refusal that depends on the switch.
- **An ordered index walk resolves entry by entry**: a row its prefix
  admits and a residual rejects still costs its descent, so `WHERE user_id
  = ? AND kind = ? ORDER BY created_at LIMIT 20` over `(user_id,
  created_at)` can resolve every one of the user's entries to find 20 of a
  rare kind - no more than IX8a's step resolves, and the order no sort. Its
  index leaf reads are blocking reads, as every index step's are.
- **A reverse walk across many leaves is not measured**: CV's close
  (`bench/v3.4.15/results-cv-close-v3.4.15-5-g7d11ba44.md`) measured
  `LIMIT 20` windows, served from one to a few leaves, where the descent
  per leaf does not show; what it costs over hundreds of leaves - a
  filter that rejects most rows, a deep `OFFSET` - is not known.

## Locking reads (DC)

Verified on `dc-select-for-update` at `7e18598d` (DC's close). Owned by
`docs/spec/txn.md` §5 ("A locking read") and `docs/spec/parser-v2.md` I11a;
the order is `instructions/v3.2.0/workorder-dc-select-for-update.md`.

- **`FOR UPDATE` takes `U`, which is PostgreSQL's `FOR NO KEY UPDATE`**
  (DC-Q1 (a), `[quiet-wrong]` for a client that assumes the other). A
  foreign key child's `INSERT` naming a locked parent proceeds and commits
  while the claim stands, so a count of the parent's children taken under
  the lock is stale with no refusal, and the holder's later `DELETE` of the
  parent can be refused RESTRICT. No strength that keeps a key share out
  is built: `X` under this spelling is DC-Q1 (b), and `FOR SHARE`, `FOR KEY
  SHARE` and the spelling `FOR NO KEY UPDATE` are refused by name.
- **Only an equality or a list on the tuple key descends.** Every other
  predicate - a range on the tuple key included, and every predicate on a
  `HEAP` relation, whose key no statement names - walks the whole
  relation, whatever index or Cabin the column has, without yielding its
  core and without reading the cancel flag: the write walk's gaps ("Key
  sets (CU)", "Statement scheduling"), inherited whole (DC-Q0 (a)). A
  cancel or a closed connection is seen at the session's next frame.
  DC-Q0 (b), the step chain with a claim at its driving step, is not
  built.
- **A `READ COMMITTED` locking read can be refused in the window between
  its view and its claim** (DC-Q3 (a), DC-Q4 (a)): a row whose newest
  version committed inside it is `TxnConflict`, the transaction is failed
  and the row's unit stays held until the `ROLLBACK`, so the client
  retries the whole transaction. There is no re-read loop; CR
  (`workorder-cr-read-committed-rerun.md`, written and not opened) is the
  order that would re-run it. Measured once, not a bound: eight sessions
  on one row at `cores = 4` had 2.70 % of their locking reads refused so
  (`bench/v3.4.13/results-dc-close-v3.4.13-14-gcd80d31d.md`).
- **A wake is not a grant** for a locking read either ("Locks", above):
  under sustained contention on one row a waiter can be re-run into a
  third session's claim until the 1 s fault net refuses it, which fails
  its transaction.
- **A range on the tuple key, and a read with no `WHERE`, declare a
  window held to the decide** (DC-Q8 (a)): the relation's fence counter
  stays raised for the transaction's length, not a statement's, so every
  writer of the relation pays the all-partition probe for that long, and a
  `WHERE`-less locking read holds every writer of the relation out until
  it decides.
- **Nothing bounds the reply** (DC-Q12 (a)): every row is encoded before
  the statement ends - none leaves while it runs (DC-R4) - and a KWP
  session holds the sealed batches in memory, as it does for a write's
  `RETURNING`, which has no bound either. A row-unit read stops at the
  lock cap's 65,536 rows; a window's is unbounded.
- **Rows are claimed that are not returned, and nothing gives one back
  before the decide**: every key of a declared window; a row an earlier
  run claimed that the re-run no longer matches; a row refused after its
  unit was granted. There is no unlock statement and no lock timeout
  setting.
- **Not built, refused by name**: autocommit (DC-Q7 (a)); `NOWAIT`, `SKIP
  LOCKED`, `OF <relation>` (DC-Q6 (a)); `ORDER BY`, `LIMIT`, `OFFSET` -
  the job-queue shape, DC-Q5 (b) - a join, an aggregate, expressions in the
  select list and every query-layer form.
- **`sys.statement_stats` counts a locking read's calls, time and rows and
  not what it examined**: `rows_examined` and `pages` are NULL for its
  shape, the write walk counting neither.
- **A point `SELECT` without the clause reads 0.10 to 0.13 us slower at
  DC's close than at its opening**, about 0.5 % of its server CPU, inside
  the 0.3 us floor between two servers of one binary and with an interval
  that barely clears zero: a cost of that size is neither shown nor
  excluded, and the host has no PMU to separate it from a binary's layout
  (`bench/v3.4.13/results-dc-close-v3.4.13-16-g7e18598d.md`).
- **What the cells do not cover**: a locking read under `tsan` (the
  `bs-nightly` build runs the same cells; no `tsan` run was made for DC); a
  capped KWP portal (`max_rows`) over a locking read's rows, which is the
  session's code for any described reply; a locking read whose leaf
  another core divides between its claim pass and its emit pass (the emit
  descends by key, as a write's `RETURNING` does); a cancel while a
  locking read is parked on a held row, which the wait does not read.

## The online index build (CI)

Verified on `worktree-online-index-build` at `614864c4` (CI-S1, CI-S2).
Owned by `docs/spec/index.md` §10.3. The order is
`instructions/v3.2.0/workorder-ci-online-index-build.md`.

- **The backfill's skip of a row `PURGE` retired under it has no test**
  (CI-S4's review): a delete-marked row whose deleter every reader sees,
  copied before a `PURGE` released its spilled values, fails its decode, and
  is skipped as one no reader reaches - or read again, if its deleter was in
  flight at the copy. Without the skip the build was refused, never wrong.
  Showing it needs the same seam CI-R7 does. **Neither has the re-read of a
  row whose head writer's spill - the head version's or an older one's it
  wrote - a rollback released under the copy** (the audit's G24, verified
  at `030b5bd9`): the same seam, and a writer on another core rolling back
  between the copy and the decode. Without it the build was refused
  `NotFound`, never wrong.
- **CI-R7 has no test that fails without it** `[quiet-wrong]`. A row whose
  head writer was in flight at the backfill's copy, and rolled back with
  its id below the floor before the walk, is read again instead of being
  trusted resolved. Showing it needs a seam between the backfill's copy and
  its append (one synchronous slice), and a writer whose id sorts below the
  build's. Neither exists, so the rule is held by review alone.
- **A writer transaction open past the fault net refuses the build.** The
  drain's wait is the lock family's 1 s net, restarted each time a writer it
  waits on decides. A transaction granted the relation's `IX` before the
  announce and open longer than that refuses the build `TxnConflict`, after
  H3 - which spends an index oid, owes a root page until the reclaimer
  frees it, and moves the schema word twice. Today's `X` build is refused by
  the same writer.
- **A build holds every core's undo for its length.** Its in-flight id holds
  the floor, so no undo page with a later writer is recycled and the commit
  window keeps every entry above it until the build decides. The view is
  re-minted per slice, which frees the horizon's consumers only. The
  lifetime ceiling never ends an online build: autocommit is not bounded.
- **Every core pays the open-DDL resolution for a build's length.** The
  build counts as an open DDL instance-wide, so every statement head on
  every core resolves names under a minted view and bypasses the shared
  name cache until it decides (`ddl_open_at_head_`). Under the `X` build the
  relation's own writers were parked instead.
- **A refusal between H2 and H3 leaks the root page**: the image is logged
  and no slot names it yet. A crash there leaks it too.
- **The drain draws one wait-for edge**, to the front of its set. A cycle
  through another member is found once the front decides, or by the net.
- **A stream of online builds can keep an `X`-taker waiting**, since a
  waiting `X` has no queue position (`lock_table.cpp`), as writers already
  can.

## Access statistics in memory (DA)

Verified on `da-access-stats-memory-resident` at DA-S1
(`instructions/v3.2.0/workorder-da-access-stats-memory-resident.md`; owner
`docs/spec/sys-views.md` §6.2d, `docs/spec/heap-and-tuple.md` §7).

- **DA-S3 is done at superblock 38** (on `di-shape-identity`, carried by
  DI's format event): bootstrap makes no page 11 and no `sys.objects` or
  `sys.tables` row for oid 130, which is retired; page 11 is reserved and
  unused, as page 10. A version-37 volume does not mount.
- **A forget orders the writes, not the statements** (DA-R5): a `SELECT`
  whose borrow was refused and that records after an `ADD COLUMN`'s forget
  can re-admit the reused position's shape, as page 11's hold allowed.
- **A part's bound is per core**: a schema whose shapes pass 4,096 a core
  drops on the cores that run them and keeps counting the known ones; the
  merged view may then hold more than 4,096 rows. `access_stats_dropped` says
  how many were lost; the key raises the bound.
- **A dropped relation's shapes stay until a restart** and count against
  their cores' bounds, as they stayed until a mount before.

## Statistics that can queue a statement (DL)

Verified in the main checkout on `main` at `1aaafe1b` by reading; nothing
here is measured. The rules are `docs/rules/rules.md` §3a's two; the milestone that
closes these is `instructions/v3.2.0/workorder-dl-statistics-queue-no-statement.md`,
whose §1.2 carries the `path:line` of each. A first reading of `src/stats/`
and its call sites, not a census: DL-S0 is the census.

- **Every successful `SELECT` with an identity takes one latch for the
  instance to note itself to the Cabin controller's collector**
  (`OptimizerSignals::NoteExecution`), on every core, whether or not
  `cabin_optimizer` is on - it is off by default, and only the wait sample
  asks the switch. Under that latch a new fingerprint past 4,096 evicts by
  a scan of the whole table, and the controller's tick copies and sorts
  every entry. Breaks clauses (a) and (c), and the second rule: the consumer is
  off and the statement still builds an identity and notes for it. Its cost is inside every
  `cores = 8` number of DA's results files and separated in none.
  **DI-S2 changed this path's key and nothing else of it** (on
  `di-shape-identity`): the table holds a signal a `(fingerprint,
  candidate)` and a second map a fingerprint's waits, so the 4,096 is
  reached sooner and the eviction's scan with it; the latch, the switch
  and the scan are as they were, and are DL's.
- **A Cabin probe's hit or miss takes three shared latches to count
  itself**: the cabin's partition latch a second time, the instance's one
  `stats_latch_`, and the collector's latch above (`CabinStore::NoteHit`,
  `NoteMiss`). Reached by a statement whose plan names a Cabin. Breaks (a).
- **A record into a per-core statistics part waits for a reader's copy of
  that part**, and the latch is a `std::mutex`, so the wait sleeps the
  core's reactor and every session on it: `StatementStats` (whose
  `Snapshot` merges and allocates under the latch, up to 4,096 shapes a
  part, and whose `sys.meta` drop count takes every part's latch),
  `RelationStats`, `LeafStats` (counted under a leaf's page hold),
  `AccessStats`, `LockWaitLog`, `CheckpointLog`, `AshRing`, and
  `PlanStats`' parts (the steward's `Merge` allocates under the latch over
  a 1,024-slot table). Allowed by DF-R2 and DA-R3 as written; breaks (b),
  the strict end state, and for the two allocating readers (c). The BO
  entry above ("A view's read delays the writer of each part it copies")
  is the same gap, older.
- **A core's first compile after a plan-statistics publish takes the
  instance's `publish_latch_`**, which an install holds across the
  snapshot's construction (`PlanStats::Current`, `InstallLocked`). Breaks
  (d).
- **`SHOW CABIN_OPTIMIZER` can stall its core for a whole Cabin build**:
  the entry under "Multi-core state, continued". Within the rule if the
  controller's view is a statistic (DL-Q0).

What already meets the rule: `RefusalStats`, `TxnStats`, `IndexStats`'
counts, the wait statistics, the contention instrument and the per-core
load slots - single-writer words a core, read with no latch.

## Assertion admission

- **A departure that raises a SUM is never admitted** `[quiet-wrong]` (a
  review finding next to the audit's G26, verified by reading at
  `0304a822`; the pending entry's G26-R). A `DELETE`'s departure, and a
  group-moving `UPDATE`'s, is reserved with no admission check, on AS11's
  premise that a departure cannot violate an upper bound (`assertion.md`).
  For `SUM` over a column holding negative values, removing a negative
  contribution raises the group's sum: under `CHECK SUM(x) <= 100 GROUP BY
  grp`, `(1, 1, -50)` then `(2, 1, 140)` is admitted (sum 90), and deleting
  row 1 is accepted with the group then summing to 140. The declared
  invariant is broken with no refusal. Admitting such a departure narrows
  AS11, a contract change that waits for the operator. `COUNT` and `SUM`
  over non-negative values are unaffected.

## Assertion reservations (CK, CL)

Verified on `ck-assertion-commit-settle` at `291cc251`. A crash loser's
reservations are given back at the mount, and a failed commit - always behind
a stopped log (CL-S0) - is given back by the next one (`assertion.md` §6.2,
§7); what that leaves:

- **An abort's unwind refused part-way stops the instance's log** (CL-Q7 (b)):
  a page read error on a Bound Cabin page, or a directory refusal, during a
  `ROLLBACK` or a statement's abort is a fail-stop until a restart, where it
  used to leave the total wrong. Availability for correctness, as a refused
  log write already was.
- **The sim checks a loser only after a restart**: its `a0` writes inside
  explicit transactions are judged `kUnknown` at the statement (the oracle's
  committed rows are not what the assertion reads there) and checked by the
  after-restart re-summation alone.
- **A reservation under `kBootstrapXid`** - a dispatcher with no transaction
  manager - is never given back at a mount: that id writes no `TXN_COMMIT`, so
  the mount cannot tell its statements apart and treats it as committed. No
  production configuration reaches it.
- **A checkpoint's `ASSERT_PENDING` list is as long as the open reservations,
  and is written under the registry's one directory latch.** Undecided
  transactions holding R reservations in all (`pending_` and `aborting_`)
  cost every checkpoint they span R records, appended under that latch -
  with any ring-full flush or segment-roll sync the appends incur - so every
  assertion's admissions and reservations, on every core, wait for them.
  Nothing bounds R but the transactions' size: not `AssertionSnapshotFits`
  (groups only), not `max_locks_per_txn` (a bulk write declares one coarse
  unit), and an autocommit statement or a KWP load has no lifetime ceiling.
  Seen in `AssertionRaceTest.EverySnapshotEqualsTheFoldOfTheRecordsBeforeIt`
  at `8751ec2f` (800 open reservations against a back-to-back snapshot loop,
  killed past 80 minutes); the cell now commits each row, as the dispatcher
  does. Not measured on a server.
- **`kEntryReserved` no longer means "in flight"** (CK-R1): no reader may
  trust it as such (CK-R4, `[quiet-wrong]`); the cell
  `AssertionRecoverTest.ACommittedReservationsEntryKeepsItsBitAndEveryConsumerAnswersAsBefore`
  pins it.

## Decisions the revision has not taken

- **AR0's D1-D16: what is taken.** `workorder-al-m0-single-wal.md` took D3,
  D4, D14 and D15 for M0's scope; only D15 was stamped (AL-R8, 2026-09-03).
  D3 was built against AR0's proposal - every core appends under one latch
  with one writer thread, not (a) a dedicated log core - and was ratified
  2026-09-28 with AR0-5-R's R10 (`raft-marks-2026-09-28.md` §7). AR0-M and
  the later marks take D1(b), D2(a), D7, D8, D9(a) and D11; the rest await
  the operator's word. The three AR0 marks `[quiet-wrong]` - D7, D8, D9 -
  are all marked by the operator (AR0-M8, AR0-M3, AR0-M4;
  `raft-marks-2026-09-29.md` §3). Owner: `ar0-architecture-revision.md` §5.

- **AR0 §6's prerequisite is answered only for the engine AR0 replaces.**
  AR0-V1 answers RW-C1 attribution from three v2.x results files at
  `1769487` (94-98 % of unattributed reactor wall time in the WAL drain's
  `fdatasync`); the v3 number - AL-S8's `fdatasync`-share cell - was not
  run. Owner: `instructions/v3.0.0/ar0-architecture-revision.md` §6 and
  AR0-V1.

## Power cuts (BS-S4)

Verified on `bs-test-hardening` from `5d58f3ca`
(`instructions/v3.0.0/workorder-bs-test-hardening.md` BS-S4, BS-A8, BS-A9).

- **A torn page of a logged class is healed; four tears are still refused**
  (DB, verified on `db-torn-page-healing` at `c802fac9`,
  `instructions/v3.2.0/workorder-db-torn-page-healing.md`). Since DB a page
  a power cut tore at a sector boundary is healed at the mount from the
  image its first write fetch after a fault-in or a writeback logged, or
  from its `PAGE_INIT` (`wal.md` §10), and the sim fails a run that refuses
  one (DB-Q6). Still refused - the mount, the reclaim or the integrity sweep
  naming `Corruption` - and lost to the instance until a restore:
  - **page 0 and the two map regions** (DB-Q3): read before recovery, never
    imaged, so a tear refuses the mount; no second copy exists;
  - **a page of an unlogged class** (`sys.statistics`, and a fenced index
    build's tree before its `LogBuiltTree`), whose content no record
    describes;
  - **a tear of the mount's own write**: images are off through the mount
    (DB-R2), so a page redo or the mount wrote back and a crash tore before
    the completion checkpoint is refused when its last image is below the
    scan start;
  - **a headerless page** - a Waystone directory page - carries no
    checksum, so a tear there is served, and only invariant 9's
    fall-through stands between it and an answer.

  Over `scripts/bs/io_sweep.py --kinds power-loss-at-page-write --ops 300`
  at `c802fac9`, seeds 1-5 and 20260826003, page writes 1-14 in both modes
  and both phases: 28 tears, 23 healed, 1 refused (a mount-phase tear), 4
  left nothing a read reached; no run failed.
- **Unverified: an index job's `ANCHOR_SLOT_REMOVE` beside a `DROP TABLE`
  reclaim** (`[source-read]`, found at DB-S0 on `db-torn-page-healing` at
  `734484ca`). The index job removes a slot from a relation's anchor page
  (`src/catalog/catalog.cpp:990`) while a concurrent `DROP TABLE` of that
  relation may free the anchor's page (`src/server/page_reclaim.cpp:122-127`);
  read, not reproduced, and independent of DB.
- **The simulated cuts are narrower than a disk's.** The log is cut as a
  suffix of its unsynced run, never scattered; the page device keeps one
  pending image a page, so a page written twice since its sync persists as
  its latest image or not at all; and a page past the durable capacity is
  lost with the growth. A disk can do each of these differently. The
  scattered log cut is one the engine answers since the audit's G4 - the
  open zeroes what lies past the tail (`wal.md` §4.2) - but the sim cannot
  produce it; `WalStreamTest.AnIntactRecordAboveAHoleIsNeverReadBackIntoTheStream`
  plants the image by hand (verified at `cbe61e87`).

## The audit's logic defects (G27-G42)

Verified on `worktree-audit-g27-g42-fixes` at `ef0e3664`
(`docs/pending/whole-engine-audit-second-run-twelve-slices.md` §12). What
the thirteen fixes leave, each `[source-read]` unless marked.

- **A `CREATE TABLE` of tens of thousands of columns is refused late.**
  `RowLayout::Build` now refuses a row past a page before its width sum can
  wrap (G29), which bounds a relation near 8,100 columns - but the
  dispatcher's and the catalog's two column-name checks run first, each
  comparing every pair, unyielding: at the 65,538 columns one 1.3 MB
  statement can spell that is about 2 x 10^9 comparisons apiece before the
  refusal. No column-count cap is stated or checked ahead of them. Traced by
  the audit's verifier, not run.
- **The Cabin controller parks a candidate on a failed catalog read it
  cannot tell from a permanent refusal.** `CreateCabin` now returns its
  duplicate scan's failure (G40). `cabin_optimizer_exec.cpp` retries on
  `ResourceExhausted` and treats `NotFound` as permanent, and the store can
  answer `NotFound` for a page of a damaged chain; that candidate is parked
  until the controller is reset. Read by the step's reviewer.
- **`RENAME TO` refuses a name too long with no byte.** The catalog's
  refusal reaches the client as it is (`command_dispatcher.cpp`, the
  rename's `Refused(renamed)`); CREATE TABLE's, CREATE INDEX's and a
  column's carry theirs. It predates the fixes.
- **A volume that already holds a truncated name is not repaired.** Two
  relations, or two indexes, created under one 64-byte-or-longer spelling
  before G27/G28 keep their two rows under one stored name; the 63-byte
  prefix reaches one (`FindIndexByName`: the larger oid; a relation: the
  first in scan order), and dropping by it twice removes both.
- **No cell covers a restore's unreadable archive or unreadable archived
  segment** (`IoError`, G41's two other answers): the cells have a missing
  archive and an existing empty one. A cell would need a directory the test
  cannot search, which a test run as root can.
- **`IsPinnedClassFrame` takes the map latch for every Bound Cabin frame it
  is asked about, on a volume with a Waystone directory** (G34): once per
  frame per sweep visit, under the frame table's latch, for frames the sweep
  never evicts. Free where no pattern is opted in. Not measured.
- **`FindIndexByName` has no caller in the engine**: the statements ask
  `LookupIndexByName`, and the `NotFound`-shaped form is kept for the cells
  that open an index by its name (about 25 call sites in 9 test files).

## Index entry purge (DD)

Verified on worktree `dd-index-entry-purge` (DD-S2, 2026-11-20).

- **Nothing drives it**: `PURGE INDEX` runs when an administrator runs it
  (DD-Q4 (a)); DD-S4, a background driver, is put to the operator. Under
  DN's heap a slot is freed only by entry removal, so DN needs a driver
  that runs unasked.
- **A statement does not yield between attempts**: its attempts run back
  to back on its core's reactor, each bounded to 64 leaves under the
  relation `IX`, and the core's other sessions wait for the whole
  statement.
- **A leaf's first entry is never removed** (`index.md` IX2c), so a dead
  first entry survives every purge; on a DN heap that would be one slot a
  leaf of each index never freed. DM's U2b (a bound kept on the page) is
  what would lift it.
- **Spilled covered churn under one key is never purged**
  (`SameOwedEntry`'s spilled-cell rule, DD-S0's finding).
- **Not tested**: a divide forced between a leaf's copy and its removal,
  and two `PURGE INDEX` runs of one index on two cores at once; the
  descent's coverage check and the tuple unit are what make both safe.
- **The simulator draws it on one seed in four with a lock table**; the
  other three answer it with the refusal by name.

