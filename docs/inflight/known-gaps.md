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
  `2f08b71c`: access statistics, Waystone trails, the delete-mark purge,
  Cabin and assertion builds, and DDL page creation open no window. Census
  B classified each as refusing before its first write. No cell drives one
  into a full pool. Owned by `docs/spec/eviction.md` (§3.3).

## Testing

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

- **An expeditor's `Start()` failed once under `-j8` and did not
  reproduce.** On `ay-s2-containment-wake` at `33b9433`, one full Debug
  suite failed `ExpeditorTest.AtOneCoreTheDispatcherHoldsTheInstancesLockTable`
  at `ASSERT_TRUE(db.Start().ok())` after 1.8 s, with the status not
  printed, because the assertion carries no message. The cell then passed 5/5
  alone. `ExpeditorTest.*` passed in eight parallel copies × 5 repeats, and
  the next full suite was green. The fixture's ports come from
  `TwoFreeLoopbackPorts()`, which probes and releases before the instance
  binds, so a parallel test can take one in between; that reading is not
  confirmed. **Owner: none.** Since AZ-S4 (`worktree-az-s4-instrument-failures`
  from `cd433ea`) every one of the file's fourteen `Start()` assertions - ten
  on `db`, four on `opened.value()` - prints the status through `Started`,
  so the next failure names its cause - for a port clash, `bind() failed`
  with the errno, though not which of the two ports. The failed cell runs
  one core, where no listener uses `SO_REUSEPORT`, so a clash there fails
  `Start()`; above one core it would bind. Neither the port probe nor any
  bound was changed, since no cause has been observed.

- **AX-S2b's release-kick cell failed once under `-j8` and did not
  reproduce.** On `worktree-ay-s0-order` at `14cfdfa` (sources as
  `58198cb`), one full Debug suite failed
  `RowWaitWakeRigTest.AWriterParkedOnAnotherCoresRowProceedsAtTheReleaseKick`
  with its assertion message not captured; the cell then passed 20/20
  alone, 40/40 across eight parallel copies, 40/40 under a concurrent full
  suite, and two more full suites were green. Its bounds are wall-clock
  (`Within(2000ms)` for core 1's first idle block, `Within(1000ms)` after
  the kick), which a loaded host can exceed. **Owner: none.** Since AZ-S4
  (`worktree-az-s4-instrument-failures` from `cd433ea`) the file's timed
  waits print what they saw, read only through what is safe from the test
  thread:
  - every first idle-block wait, and the holder's `COMMIT`, print whether
    the statement finished and, only if it did, its response;
  - the wait after the kick prints, from a baseline taken before the sim
    releases it, the kicks still held in the sim, the kicks the real table
    skipped on a clear sleep flag, the wakes core 1 received and its idle
    blocks - which tells a kick that never reached core 1 from a reactor
    that woke and a statement that was slow.
  What a first idle-block timeout cannot say is whether the reactor was busy
  or the statement never parked; no counter safe to read across threads
  separates the two. The bounds were not widened. Keep the next failure with
  `--output-on-failure` before it is attributed.

- **A rollback's schema-word move has no cell that kills its removal since
  AX-S2.** Verified on `ax-s3-inflight-prose` from `b54a769` (AX-S3):
  `MoveSchemaWordIfCatalogWriter` removed from `TransactionManager::Abort`
  survives `DdlFenceRigTest.ADropIndexRolledBackOnCoreZeroKeepsTheRowAPeerWroteWhileItWasOpen`
  5/5 and the full Debug suite, 3015/3015 (one pre-existing disabled cell). The cell
  was written for a peer whose memo left out an index an open `DROP INDEX`
  had marked - DT9's predicate was core-local - so after the rollback only
  a re-resolution put the index back into its writes. Since AX-S2 the peer
  resolves the index while the drop is open, and the cell passes without
  the move. **The move was nearly inert already**: the rolling-back core
  moves the word again at `EndDdlScope` (`InvalidateAfterCompensation`),
  synchronously after `Abort`, so this move only orders the bump before the
  release - the window the cell's rounds raced. By reading, not by a cell:
  the one shape it still orders is a rolled-back `CREATE INDEX` whose
  index a peer's memo holds (§5b's asymmetry), where a re-run inside that
  window maintains the orphaned tree once; a rolled-back `DROP TABLE`
  bumps the word at its own write. No shape found loses an entry, so the
  move stays. Owner: none named; `txn.md` §4.1 and `ddl-transactional.md`
  §5e. Carried by AX's close
  (`instructions/v3.0.0/workorder-ax-inflight-publication.md` §7 item 4).

- **The assertion scan's floor is a fixed defect with no regression test
  under it.** Verified at AM-S0(a) by reverting the fix (every cell in
  `tests/expeditor_test.cpp` stayed green); re-read at `f3da0413`,
  2026-10-13. The defect (AL-7c, `workorder-al-m0-single-wal.md`) started
  the assertion resume at the anchor fold's `checkpoint_lsn`, which can sit
  past a core's own `ASSERT_SNAPSHOT`, so the instance came up counting
  those assertions unenforceable and refusing their relations' writes for
  the mount (`assertion.md` §6.1). The fix passes the fold's
  `redo_start_lsn` (`Expeditor::Open` and `CoreRuntime::Open`'s fixture
  arm, both calling `ResumeAssertionsAfterRecovery`).
  - **Why no instance cell reaches it**: each core's `ShutdownCheckpoint`
    syncs the store first (`core_runtime.cpp`), so its redo start equals its
    `CHECKPOINT_BEGIN` and the fold's winner is at or before every
    snapshot. Removing that sync "because the tail already syncs" would make
    the defect reachable silently.
  - **What reaches it**: a fold won by a cadence checkpoint (old recLSN,
    recent `checkpoint_lsn`) beside another core's older last
    `ASSERT_SNAPSHOT` - a core whose shutdown checkpoint never published.
  - **The cheap harness** is a unit cell over
    `SuperBlockCheckpointAnchor::Publish` and the floor choice: two
    synthetic records `{redo=1000, ckpt=1000}` and `{redo=100, ckpt=5000}`,
    asserting the floor handed to `ResumeAssertionsAfterRecovery` is `100`.
    Since BC the fold never decreases (`superblock_checkpoint_anchor.hpp`),
    which the cell's record order must respect.

  Owner: `docs/spec/wal.md` §16.

- **`wal_ring_full_refusals` is proved zero where it must be and unproved
  where it fires.** Verified at `f6ed10c`; re-read at `f3da0413`,
  2026-10-13. Reaching it needs an append to lose the drained ring space to
  another core `kRingDrainAttempts` (4) times running (`src/wal/manager.cpp`),
  which one thread cannot stage. `tests/wal_manager_test.cpp` and
  `client-manual.md`'s ring-counter row state the gap. Owner:
  `docs/spec/wal.md` §16.

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

- **A refused catalog report on a new page spends one reserved page.** The
  page is left allocated, empty and unlinked. The catalog range is pages
  16..127, below the system floor `FreePage` refuses, and no reclaim walk
  reaches an unlinked page (`page.md` §5, `drop-table.md` DT1). This
  predates AZ-S1: the page used to hold the row. **No owner.**

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
  Re-verified at `1fb71d1a` by reading, 2026-10-12; priced by BQ at
  `4cf30270` (`instructions/v3.0.0/workorder-bq-wal-segment-roll.md` §7,
  `bench/v3.0.0/results-bq-s1-segment-roll-v2.7.0-909-g4cf30270.md`,
  `bench/v3.0.0/results-bq-s2-rung1-v2.7.0-909-g4cf30270.md`).
  - `WalStream::Append` (`src/wal/stream.cpp:359`) rolls under the stream
    latch when the record does not fit: `Roll` (`:235-243`) reaches
    `StartSegment` (`:140-150`), which syncs the segment it leaves through
    `SyncDevice` - waiting out any sync in flight, the writer thread's
    included - and creates the next. Deliberate, and stated as built in
    `include/kds/wal/stream.hpp`'s protocol header and `wal.md` §6 step 1,
    whose self-contradiction BQ-S5 resolved.
  - **Measured**: at `relaxed` under one bulk writer (~75 MiB/s of log) a
    roll holds the latch p50 ~330 ms, p99 ~790 ms, max 1.2 s, 95 % of
    it the gate wait - the writer's tail `fdatasync`, slow on a device also
    taking the log, the checkpoint's writeback and the preparer's zero
    fill. Rolls every ~0.85 s put 35-46 % of a window under a hold; cores
    without the bulk writer see a schedule p99 of ~290-660 ms against
    ~0.23-0.47 ms isolated (medians), and core 0's drain carries ~100 % of its
    stream-latch wait. At `group` the hold is ~6 ms p50 / ~20 ms p99.
  - **What BQ tried, cheapest first** (BQ-R2): H2's larger segment makes
    rolls rarer and longer, not cheaper (~5 ms more hold per MiB, the
    total held time flat; at 1,024 MiB BQ's probe saw the restart 1.1-1.4 s
    longer and one `INSERT` stalled up to 12.8 s on the preparer's build). Rungs 2 and 3 (H5, H3) are recorded unable
    from BQ-S1's bounds (BQ-Q6 (a)): with every non-tail sync removed the
    hold p99 stays 652-780 ms, and with every roll-aligned stall removed C1
    reader p99 stays 2.6-145 ms. BQ closed on §0.4's row 4: nothing below
    rung 4 met the bar, and the latch is kept with this cost named.
  - **Open leads for a successor order**: (i) H4 option 3 (no zero
    prewrite where `posix_fallocate` reserved, the body's `fsync` kept) cut
    the hold's p50 to ~16 ms, but its tail stays (p99 ~1.1 s, bimodal) and
    its group guard (C2's `group` commit p99) is unevaluated, so it is a
    measurement build, not a fix; (ii) rung 4, H6 - releasing the latch for
    steps c-g - is `[quiet-wrong]` and unlicensed (BQ-Q5 (b)): it needs the
    operator's own word.
  - **Beside it, not BQ's**: at `relaxed` the log was almost never recycled
    during a bulk load, because a re-dirtied page kept its first recLSN;
    fixed on `fix-redirtied-page-rec-lsn` at `0e88060a` (`wal.md` §11-2),
    not re-measured.

  Owner: `docs/spec/wal.md` §6, `wal/stream.hpp`'s latch protocol.

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
  commit-order floor for good.** Verified at AN-S2 on
  `an-s2-read-view-cutover`; re-read at `f3da0413`, 2026-10-13. The floor
  (`include/kds/txn/instance_visibility.hpp`) is bounded by the minimum
  issue cursor over attached cores, and an *idle* core is unpinned by
  burning its block on its own tick (`PinsFloor`, `txn/manager.cpp`). A
  core that wedges or shuts down takes no tick and nothing republishes its
  slot, so the floor stays and the window grows one entry per commit for
  the life of the process. Not reachable today: the expeditor stops every
  core together. Owner: `instructions/v3.0.0/workorder-an-read-view.md`
  AN-R14. (The per-core read view closed at AN-S2; pinned by
  `VisibilityWiringTest.ACommitOnAHigherBlockIsVisibleToALowerCoresNextView`.)

- **Two cores recording one pattern can strand a trail in a directory
  slot.** Verified at AT-S7 (2026-09-23) on `at-s7-one-cabin-store`.
  `LookupOrCreateWaystonePage` (`src/stats/waystone_dir.cpp`) reads a
  child slot under one page hold, **releases it**, allocates, then
  re-acquires to link. Two cores whose instances collide on one
  `DirIndexAt` can both read `kEmptyDirSlot`, both allocate, and the
  second link overwrites the first - stranding a trail the first has
  already written, and leaking a page. Newly reachable because AT-S7
  turns recording on for every core.

  Invariant 8 prices it exactly: a lost trail is a lost replay and never
  a result. Not closed because the obvious fix - hold the parent across
  `CreateNew` - introduces a page latch held across a durability wait on
  a path `device_page_store.hpp` permits it on only for a fault. Owner:
  `docs/spec/waystone-concpets.md`.

- **`ClaimPatternWaystoneRoot` has no path for deepening a directory.**
  Verified at AT-S7 (2026-09-23) on `at-s7-one-cabin-store`. The claim
  refuses to replace a root that is set, which is what keeps two cores
  building one directory from keeping two; `GrowPatternDirectory` exists
  to deepen one and would be handed back the *old* pair, leaving the row
  pointing at the old root at the old depth while a new level exists - a
  miss for every key, not an error.

  Not live: `GrowPatternDirectory` has no caller outside its own cells, so
  every directory is depth 1. Closing it means a compare-and-set contract
  the claim does not have. `waystone_dir.hpp`'s header carries the trap;
  owner: `include/kds/catalog/catalog.hpp`.

- **A Cabin builds rarely on a busy instance, and that is the price of
  one store.** Verified at AT-S7 (2026-09-23) on `at-s7-one-cabin-store`.
  §6a's banking gate refuses while **anything** is unresolved anywhere —
  `ReadView::in_flight_at_mint` became the instance's fact, and the
  announce asks `InstanceVisibility::AnyUnresolved` again — so one
  autocommit write in flight on any core declines every build on every
  core. It was one core's transactions declining one core's builds, which
  §6a already called wider than it sounds; the store's topology made it
  the instance's.

  Correct, and conservative in the only legal direction: an unobserved
  value is answered by the authoritative scan. What it costs is that a
  Cabin may never fill on a write-heavy instance, which `SHOW CABINS`
  reports as `unbankable_views=` and nothing else distinguishes from "the
  column is never probed". Sharpening it means a per-value record of which
  unresolved transactions could contradict a build, which is a structure
  §6a does not have. Owner: `docs/spec/cabin.md` §6a.

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
  intention test scans the transaction's holdings, beside the row `X`s the
  writes already took - so a transaction's asks are quadratic in its
  borrows. Found by the same review. **Measured at AY's close** at
  `v2.7.0-530-g0552d55`, one core only
  (`results-ay-s11-overhead-v2.7.0-530-g0552d55.md`, cell 3, at `fc2d3437^`):
  about 0.2 ns x K a row for K distinct parents in one transaction -
  unresolved up to K = 1,024, marginal at 4,096, +3.43 µs a row (~6 %) at
  16,384 - **and ~2.1 µs of that 3.43 is paid at the decide, not by the
  inserts**, whose own excess is not resolvable. So what was measured is
  not this scan: its hit here, the relation intention, sits among
  `held_`'s first entries. By source read, not profiled, it is
  `LockTable::Release`: per borrow, `ReleaseHeld` erases from the front of
  its partition's vector and `WakeWaiters` then scans that partition for
  the key just erased - O(n²/64) a decide, which predates AY and which
  D9(a)'s doubled borrows took from 14 ms to 49 ms at K = 16,384. A hashed
  holdings set would not remove
  it; a keyed partition would. Neither is built.

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
  engine only.** Measured by AT-S13 on `at-s13-prices` at
  `v2.7.0-391-gf6f2073` (2026-09-26, `results-at-s13-prices-v2.7.0-391-gf6f2073.md`
  cell 2, at `fc2d3437^`): in three of the measured commit's seven runs one
  `update-disjoint` arm took ~0.37-0.41 s longer than its siblings (max
  latency 371-413 ms), inside `sched_foreground_polled_us`, and in one run on
  every core at once; `df8cc5f`'s three runs show none. An instance-wide
  hold would produce it, and so would a WAL append held behind device
  writeback under the log's latch. Three against zero is too few to call
  it AT's and too many to call it noise; not investigated. Owner: whoever
  measures next, starting from `wal.md` §3 and `page.md` §6's writeback
  claim.

- **A `SHOW CABIN_OPTIMIZER` can stall its core for a whole Cabin build.**
  Verified at `4bf80fa`, 2026-09-23. AT-S8 put the controller behind a view
  latch that core 0's cadence holds across a tick, and a tick may create a
  Cabin and walk its relation for the seeded build; the latch is a
  `std::mutex`, so a peer's `SHOW` sleeps that peer's reactor - and every
  session on it - for the build. Liveness, not correctness. The fix is for
  the tick to publish a copy of the view at its end and the `SHOW` to read
  the copy. Owner: `docs/spec/physical-optimizer.md` Part II.

- **A multi-core test instance binds its probed port with SO_REUSEPORT.**
  Verified at `aaf0f47`. `ExpeditorTest`'s fixtures probe a free loopback
  port and then bind it; above one core every listener sets SO_REUSEPORT
  since AT-S8, so two concurrent ctest processes that probe the same port
  both bind it and exchange connections silently, where before the second
  bind failed loudly. Unobserved, and a flake rather than an engine defect.
  Owner: the test fixture.

- **An eight-core append workload reaches the clustered tree's bounded
  re-descent refusal, and the cause is not established.** Measured by the
  scenario rebaseline on `bench-rerun-scenarios` at `v2.7.0-531-g9a0525d`
  (2026-09-30, `results-scenario0-stockmarket-…` §6 and
  `results-scenario2-freight-…`, both at `36449d28`): a write descent
  restarted `storage::kMaxDescentRestarts` (4) times and was refused
  `TXN_CONFLICT retryable=1 ... gave up after 5 attempts` (`btree.cpp`) -
  scenario0 `cores = 8` `strict` on one `trades` insert in 3 of 3 runs
  (key 113, root page 138 each time), scenario2 `cores = 8` `group` three
  times; all on fresh relations under eight appending sessions. The message
  names two causes, split churn or a descent from a memoised root that has
  since grown a level; the same key and page in every repeat fit the stale
  root, unproven. **Since BA-S14 (`6aa76ec7`, after the measurement)** a
  statement that wrote nothing before the refusal is re-run inside the
  engine after a yield, crossing the task boundary where `Revalidate()`
  drops the memo, so that case no longer reaches the client; not
  re-measured. The engine still does not tell the causes apart: since AT-S16
  a grown-over root carries a mark `SecureParents` reads and `DescendTo`
  does not, and from a stale root every attempt fails the same way
  (`insert_placement.hpp`). No cell isolates which cause fires. Owner:
  `docs/spec/heap-and-tuple.md` §5.

- **No cell drives a stale writeback against a re-faulted frame.** Verified
  at `aaf0f47`. AT-S8 step 1b draws every frame's dirty generation from the
  store's own counter, so a frame evicted and faulted back never repeats the
  value a concurrent writeback recorded at its copy; the interleaving needs
  two writebacks and an eviction inside one device write, and no cell forces
  it. Owner: `docs/spec/page.md` §6.

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
  - **A purged version's secondary-index entries are never removed** (BH-Q8
    (a)); a key placed again appends beside them (`index.md` IX4b).
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

- **A write refused by a lock unit waits on that unit's slot, and a wake
  is not a grant.** A refusal on the ask's own unit waits on its slot since
  AX-S2b, one in another unit since AY-S2 (the containment wake), and a
  first encounter with a holder between its decide and its release since
  AY-S3 (B6; `row_wait_wake_rig_test.cpp`). Background: AT-S13's review
  (`at-s13-prices` at `896af54`, 2026-09-26) found cross-core holders read
  as decided and writers refused `TXN_CONFLICT retryable=1` where AO-S3
  promised a wait; AX-S1 made the in-flight test the instance's. What stays
  open, retryable and never a wrong answer:
  - **A wake is not a grant.** `TryAcquire` does not queue, so a third
    writer can take the unit between the flip and the re-run; under
    sustained contention the waiter ends at the 1 s fault net naming the
    last holder (AX's item 3, no action proposed).
  - **What still polls** has no slot behind it: no lock table, a holder only
    the header names, an assertion's group, and a `PURGE` waiting on a
    deleter's decide or the read horizon (`heap-and-tuple.md` §4.1c).
  - A holder that never decides costs a cross-core writer the 1 s fault net
    and its defect warning, as a same-core writer pays.

  Spec: `docs/spec/txn.md` §5.

- **The relation `IS` is declared at the bind for every relation a
  statement names** (closed at AT-S1, 2026-09-09; `step_compiler.cpp`). What
  stays is one-directional by design: the ask is a non-queueing
  `TryAcquire`, so a reader arriving while a DDL holds `X` reads on, sound by
  catalog MVCC and the statement epoch rather than by the lock
  (`include/kds/server/read_borrow.hpp`, `txn.md` §5, `drop-table.md` DT1).

- **`CompileWhere` resolves an UPDATE's or DELETE's subquery relations
  under no view.** Verified 2026-09-09 on `m3-at` at `4b06051` by AT-S1's
  review; re-read at `f3da0413`, 2026-10-13. `UpdateInner` and
  `DeleteInner` resolve their own relation under the session's view (DT3c)
  but call `exec::CompileWhere` with `view = nullptr`
  (`command_dispatcher.cpp`), so `UPDATE t SET v = 1 WHERE id IN (SELECT id
  FROM u)` resolves a `u` another transaction created and has not
  committed. The fix is one argument at each site and turns a shape that
  works today into a refusal, so it is recorded rather than applied. Owner:
  `docs/spec/ddl-transactional.md` DT3c.

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
- **Every restart leaves the previous run's Waystone pages allocated.**
  Every mount clears each pattern's directory root and empties
  `sys.access_stats` (`server::DiscardAdvisoryState`, `waystone-concpets.md`),
  because after a crash a directory page can name a child whose allocation
  bit never reached the device. The directory and trail pages under a
  cleared root stay allocated, reached by nothing and freed by nothing, so
  the volume grows by every run's Waystone pages, restart after restart,
  clean or not, and a later run builds its trails on new pages. Verified on
  `fix-sync-publish-and-waystone-crash` at `ec440c2d`.
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

- **A backfill can be refused, or index a stranger, through a recycled
  undo record.** Between `ResolvedForEveryReader`'s check and the read,
  another core's purge can recycle the record, and a new record holding
  another relation's row with the same tuple key passes the backfill's
  identity check: its append is refused `Corruption` or leaves a surplus
  entry readers re-check (IX2). Never a wrong answer; no cell forces the
  window (`index.md` §10.1). Verified on `reindex-drop-index-reclaim` at
  `83204ebc` with the backfill fix merged.

## Statement scheduling

Verified at `0064d8bd` (BA-S15). Owned by `docs/spec/sched.md` §3 and
`docs/spec/protocol.md` §10.

- **Only a `SELECT`'s outermost walk yields.** It yields every
  `exec::SlicePolicy::pages_per_slice` (64) pages. Nothing else running
  gives its core back:
  - a write;
  - a tuple key descent, run inline by design;
  - an index walk (`RunIndexStep`);
  - a join's inner walk under one outer page.

  Each holds its core until it ends or `exec::Budget` stops it. 64 is
  BA-Q11's value, not re-measured in BA-S15.
- **A cancel is observed only where a walk yields, or at the next
  frame.** A write that `C_CANCEL` or a closing connection asks to stop
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
  - **The teardown cell kills its mutant only under AddressSanitizer.**
    `WalkSliceRigTest.ACoreTornDownWithAWalkBetweenSlicesReleasesItFirst`
    passes in Debug with `Scheduler::DiscardTasks` removed. The tree has
    no sanitizer build.

## Placement per transaction (BL)

Verified on `worktree-bl-session-balancer` at BL-S3's commit, measured in
`bench/v3.0.0/results-bl-s1-placement-premise-v2.7.0-857-g3552a03c.md`;
the first bullet re-read on `worktree-bl-touchpattern-delete` at `05cd011f`
and re-priced at BQ's close (2026-10-12, from `4cf30270`'s measurement).
BL is paused (the order's §6, "BL paused 2026-10-12").
Owned by `instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`
until its close; the mechanisms by `docs/spec/crosscore.md` CC1 and
`docs/spec/sched.md` §5.

- **A bulk writer stalls point readers on every core, and no placement
  isolates them; what is left of the stall at `05cd011f` is the segment
  roll's.** Two couplings through the one WAL were found at BL-S3's commit,
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
  stays unmarked.
- **`TRACE ON` is per core, not per session** (`CommandDispatcher::tracing_`).
  It always was; since BL-S3 moves sessions, a session's trace stops when it
  moves and another session landing on the traced core is traced instead.
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
- **BL-R5 is not built**: under D19's fallback core 0 still hands accepted
  sockets round-robin, not to the least busy core. The balancer corrects
  the placement at a session's first head, so the accept-time choice was
  not separately measured.
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
- **Waystone records a trail for a point `SELECT` by tuple key**: recording and the
  replay attempt are 7-9 % of its server CPU (BN-S1's census), for a lookup
  the descent already answers directly. No BN proposal named it.
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
- **A torn page from the device is refused, not healed.** A snapshot that
  does not freeze the filesystem (a cloud disk snapshot) is a crash image,
  and the per-checkpoint image cadence of `wal.md` §10 is not built. Every
  data page's CRC32C is checked when the backup stops, at `verify` and at the
  restore's layout, so the torn page is named there rather than at the
  mount; it is not repaired. Path B's copy cannot tear (the fence); path D
  needs a freezing snapshot.
- **A restore leaks**: a page the source freed after the copier read its
  map region stays allocated in the restore, as after a crash; Waystone
  pages and `sys.access_stats`' chain, made unreachable at the restore's
  mount (`DiscardAdvisoryState`), stay allocated where they were written.
- **PITR targets an LSN, not a time**: the commit record carries no time
  (BR-Q5 (b)). An operator notes `SHOW META`'s `wal_durable_lsn`.
- **`kds_backup` reaches the admin role only on an `auth = off` server**:
  `tools/kwp.py` has no SCRAM or TLS.
- **The LVM reference script is not run here**: the development VM grants no
  root (`br-s0-census.md` H9); path D's drills use the freeze script.
- **An `archive_command` that hangs holds the archive**: it is not timed out,
  only ended at shutdown; the archive's lag and `wal_archive_blocking` are
  what an operator watches.
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

- **A dropped column's slot and bytes are never reclaimed.** A drop flags the
  column's `sys.columns` row and leaves its bytes in every row, a dropped
  varchar's spills included until purge reclaims the row; a dropped slot is
  never taken again. Cleaning - resetting the flags and rewriting rows to a
  smaller size - is D-e (a)'s separate milestone, not built.
- **Readers see an open `ADD`/`DROP COLUMN`** (BW-Q8 (a)): the catalog row
  is rewritten in place under the DDL's transaction, so another session's
  read can see the undecided column set - a column missing from `SELECT *`,
  a name refused, an added column reading NULL - before the decide. Writers
  are fenced by the relation `X`; no value a reader shows is wrong.
- **An automatic `CreateCabin` takes no relation lock**, so the Cabin
  controller can land a Cabin on a column between `DROP COLUMN`'s restrict
  check and its write. The Cabin is never served - no statement can name
  the column - and `DROP CABIN` is refused only by the name it cannot
  resolve; harmless, and not fenced.

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
- **`ON CONFLICT DO NOTHING` skips a tuple-key conflict only** (BU-Q18,
  met by BX at the merge of `origin/main` after `d152a715`): a row whose
  unique-index key is held is refused `AlreadyExists`, where PostgreSQL's
  target-less form skips it; and `ON CONFLICT (<col>)` naming a
  uniquely indexed column is refused `NotImplemented` with BU's reason,
  "the tuple key is the one conflict there is", which BX made untrue. A
  refusal, never a wrong row. Owner: none - a decision, not a defect.

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
  directory walk and `sys.access_stats`' exclusive page are written by the
  statement that executed, as §1.1 of the order reads them. CA did not move
  them: CA-S0 measured them at 0.2-0.6 % of sampled server CPU on scenarios 1-4,
  and switching all three keys off moved no throughput outside noise
  (`bench/v3.2.0/results-ca-s0-premise-*.md`). The order's note, drain, steward and
  published route table (CA-R2..R4) are not built. Neither is the base-fetch hint
  (CA-S3).
- **Scenario 2's booking rate is the hot row's lock held across the group
  fsync.** Each booking updates one of a few hot `operations` rows, and keeps
  the row's `X` until its commit is durable. So eight bookers serialize on
  `fsync` latency, and a read path made 6 % cheaper moved the committed TPS by
  nothing (`bench/v3.2.0/results-ca-r1-visibility-*.md`). Releasing a lock before
  the commit is durable, with the dependency tracking that makes it sound, is a
  commit-protocol decision nobody has taken.
- **A pinned floor still costs a latched lookup per writer, per statement.** A
  memo removes the per-row cost (`txn.md` §4.4). Lowering the idle burn's
  threshold so the floor would follow was measured and reverted: it added
  nothing on scenarios 1-4 (candidate C2, `results-ca-r1-*`).
- **Scenario 2's `--verify` reports 8-12 % failed I1 checks in every arm,
  the base included**, and 13-28 of 400 at `v3.1.0-30`. Nobody has determined
  whether this is the driver's read skew or an invariant slip in the engine.

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
