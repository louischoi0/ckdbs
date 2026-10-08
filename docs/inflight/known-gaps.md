# Known Gaps

What is missing, what does not survive a restart, and what the code does
differently from what a spec claims. **Opened fresh for v3.0.0 on
2026-09-03** — the v2 list is at `git show 1769487:docs/inflight/known-gaps.md`
and none of it is carried across, because every entry described the engine
AR0 replaces.

Every entry names the commit it was verified at and the doc that owns the
fix. An entry whose verification predates its subsystem's last change is a
statement about an engine that no longer exists; re-verify or strike it.

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

- **The executor does not scan through EV6's ring, by decision.** Verified
  at `2f08b71c`: `OpenScanRing`'s only callers are `relayout_planner.cpp`
  and `cabin_optimizer_exec.cpp`. A `SELECT`'s outermost walk faults cold
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
  (`docs/inflight/bugs/a-fetch-refused-after-a-page-write-leaves-the-mutation-half-done.md`).
  Owned by `docs/spec/eviction.md` (§3.3) and `docs/spec/txn.md`.

- **Writers outside a statement meet the ordinary refusal.** Verified at
  `2f08b71c`: access statistics, Waystone trails, the delete-mark purge,
  Cabin and assertion builds, and DDL page creation open no window. Census
  B classified each as refusing before its first write. No cell drives one
  into a full pool. Owned by `docs/spec/eviction.md` (§3.3).

## Testing

- **`IdAllocationAcrossCores.TwoCoresWritingOneRelationIssueOneSequence`
  times out under `-j8`, twice in one day.**
  - **The failures.** On `worktree-ap-s2-trail-on-fetch-id` at `7fc2c57`'s
    tree, and again on `worktree-ap-s3-rule-0-prime` at `0fcdfcc` plus
    comment edits, one full Debug suite each failed the cell's
    `KickUntil(..., 20000ms)` (`tests/id_allocation_across_cores_test.cpp:74`)
    after about 21 s. Neither stage touches the insert path the cell drives.
  - **The reruns.** The cell then passed 10/10 alone, and the next full
    suite was green both times.
  - **What a failure cannot say.** The wait's message does not say which
    writer had not finished, or how far it got. So a slow host cannot be
    told apart from a writer that stopped.
  - **Owner: none.** The bound was not widened, since no cause has been
    observed.

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
  under it.** Verified at AM-S0(a) by reverting the fix: every cell in
  `tests/expeditor_test.cpp` stays green. The defect
  (`instructions/v3.0.0/workorder-al-m0-single-wal.md` AL-7c) is the scan
  starting at the anchor fold's `checkpoint_lsn`, which can sit *past* a
  core's own `ASSERT_SNAPSHOT` — the fold carries the record of whichever
  core had the lowest `redo_start_lsn`, and that core's checkpoint can be
  later than everyone else's. A peer then comes up counting what it owns
  unenforceable, and refuses that relation's writes for the life of the
  mount (`docs/spec/assertion.md` §6.1).

  **Why no cell reaches it, stated precisely because the obvious version of
  the argument cites the wrong call.** It is *not* the shutdown tail's
  `core->Sync()` that empties a dirty table — that is `wal_->SyncAll()`, the
  log alone. What does it is each core's `ShutdownCheckpoint`, whose first
  act is its own `store_->Sync()`; so its `RedoStartFrom` yields exactly its
  `CHECKPOINT_BEGIN` LSN, which is also its `checkpoint_lsn`, and the
  `ASSERT_SNAPSHOT` records follow inside the same checkpoint. The fold is a
  true min over `redo_start_lsn` and the tail walks the cores ascending, so
  the winner is core 1's — the earliest BEGIN of the run, at or before every
  core's snapshot, its own included. The property therefore rests on
  **every** shutdown checkpoint publishing, and on that `store_->Sync()`
  staying where it is: remove it on the grounds that "the tail already
  syncs", and this goes silently.

  **How the state is reached**, which is what a harness for it would need:
  the fold's winner has to be a **cadence** checkpoint record — the cadence
  path does not sync the store first, so its `redo_start_lsn` is an old
  recLSN while its `checkpoint_lsn` is recent — with some other core's last
  `ASSERT_SNAPSHOT` older than that `checkpoint_lsn`. That needs a core
  whose shutdown checkpoint never published: a crash, or a
  `ShutdownCheckpoint` that failed and was logged past. At `cores = 2` it is
  masked besides, because core 0 is the only core that could win the min
  with a stale record and it syncs its store before its own checkpoint.

  **So the cheap harness is not an instance-level one.** It is a unit cell
  over `SuperBlockCheckpointAnchor::Publish` and the floor choice in
  `CoreRuntime::Open`: publish two synthetic anchor records —
  `{core 1, redo=1000, ckpt=1000}` and `{core 2, redo=100, ckpt=5000}` —
  assert the fold carries `{redo=100, ckpt=5000}`, and assert the floor
  handed to `ResumeAssertionsAfterRecovery` is `100` and not `5000`. That
  pins the field the fold does not bound, in a few dozen lines with no
  instance, no threads and no crash. Owner: `docs/spec/wal.md` §16.

  The entry this replaces — *no test and no `sim/` cell constructs an
  `Expeditor`* — closed at AM-S0(a): `Serve()` split into `Start()` +
  `RunUntilStopped()` (`expeditor.hpp`) and `tests/expeditor_test.cpp` runs
  a real instance at `cores = 2` over real loopback sockets, asserting the
  assembly between the halves. Two of AL-7c's three defects have a
  reproduction there; this is the one that does not.

- **`wal_ring_full_refusals` is proved zero where it must be and unproved
  where it fires.** Verified at `f6ed10c`. Reaching it needs an append to
  lose the drained ring space to another core `kRingDrainAttempts` (4)
  times running, which cannot be staged from one thread; a multi-threaded
  cell that sometimes trips is worse than none. `tests/wal_manager_test.cpp`
  states the gap; `docs/spec/client-manual.md`'s ring-counter row now does
  too. Owner: `docs/spec/wal.md` §16.

- **AL-S8's scenario matrix cannot be re-run on a post-SUS-1 engine, so
  no measurement stage can produce the delta it was written to
  produce.** Verified at `13b6b55` on `an-s3-snapshot-adoption`, 2026-09-07,
  by attempting AN-S5's cells: `tools/scenario0_stockmarket.py` and
  `tools/scenario2_freight.py` declare part of their schema `HEAP` with no
  override, and `CREATE TABLE … HEAP` is refused `Unsupported` since SUS-1
  (2026-09-05). `s0-c1-g` and `s2-c1-g` were refused at schema creation
  (`bench/v3.0.0/results-an-s5-scenario0-v2.7.0-265-g13b6b55.md`,
  `…scenario2…`); every other cell of the matrix declares the same tables.
  AN-S5's scenario half, AM-S6 (AW-S5) and any future delta against
  `results-scenario0-stockmarket-v2.7.0-157-gf6ed10c.md` /
  `results-scenario2-freight-…` are unreachable until the drivers stop
  emitting the word - `workorder-as-sus1-heap-suspended.md` AS-Q6, the
  operator's, which named these tools on the day. Not a defect in either
  driver or in SUS-1; a measurement the tree can no longer take. Owner:
  `instructions/v3.0.0/workorder-as-sus1-heap-suspended.md` AS-Q6.
  **Closed 2026-09-08** (`raft-marks-2026-09-08-as-q6.md`): the drivers'
  four heap relations are `BTREE`, and `f6ed10c` was re-measured with the
  changed drivers the same day — eight cells, AL-S8's own arguments and
  cell order, all committing their full targets
  (`results-scenario0-stockmarket-btree-v2.7.0-157-gf6ed10c.md`,
  `results-scenario2-freight-btree-v2.7.0-157-gf6ed10c.md`). AM-S6 and
  AN-S5's scenario half have a comparator again, and it is that pair; the
  AL-S8 heap files stay as history and no delta against them is ever
  valid. What the entry leaves behind rather than closes: measurement is
  BTREE only until the operator says otherwise, so no v3 number prices a
  heap relation, and four tools still emit an explicit `HEAP` and are
  refused (AS-S3).

## WAL

- **A `BTREE_SPLIT` of 8 or more pages exceeds a 64 KiB ring or segment.**
  Verified at `07e822a6` (BD-S3), by reading. A split's record carries a
  full image of every page it writes (8,200 bytes each), so 8 pages need
  more than 64 KiB, and `kMinRingCapacity` and the sim's segments are both
  64 KiB. The `static_assert` in `storage/log_page_image.hpp` holds the
  deepest split (`kMaxStructuralChanges`) under the *default* ring only.
  Unreachable in practice - a split writes two pages per level it climbs,
  so 8 pages need three full internal levels to divide at once - but where
  it happens the append is refused `InvalidArgument` after the split's
  pages changed in memory. Raising the minimum past the deepest split was
  declined at BD-S2's review: it moves the premise of the cells that open
  at the minimum ring and of the sim's segment rolls. Owner:
  `docs/spec/wal.md` §5.2.

- **Whether an index split's records replay as one is unchecked below the
  root leaf.** Verified at `4debe8d9` (BD-S1). The secondary index tree
  logs a split as separate page images and no `INDEX_INSERT`, the shape the
  clustered tree's append split had when a log cut between its parent and
  its link left a leaf off the walk (`workorder-bd-sorted-leaf-named-keys.md`
  §1.8, fixed by `BTREE_SPLIT`).
  BD-S1 cut the log after every record of the root leaf's split and it did
  not reproduce; a leaf split under an internal node and an internal node's
  divide were not cut. Owner: `docs/spec/index.md`; a fix, if one
  reproduces, is an order of its own.

- **A heap placement whose record is never written is not taken back.**
  Verified at `07e822a6` (BD-S3), by reading; BD-S1's
  `SortedLeafCrashTest.ARowPlacedAndNeverLoggedLeavesNoHoleForTheNextRecordsRedo`
  reproduced the shape on a btree leaf at `ec3acda5`. When index
  maintenance, the assertion reservation, the undo append or the spills'
  noting fails after a placement and before its record, BD-S2 removes a
  btree row from its still-held leaf; a heap row stays on its page with no
  record. A later logged insert on the same page then names a slot redo has
  never seen, and the mount is refused (the btree cell's refusal read
  *"redo names slot 4 on a page holding 3"*). BD left the heap's protocol untouched (BD-Q4 (a)), and
  no volume this engine mounts holds a heap user relation, so only the
  parser's test seam reaches it. Owner: `heap-and-tuple.md` §3.1b.

- **A refused catalog report on a new page spends one reserved page.** The
  page is left allocated, empty and unlinked. The catalog range is pages
  16..127, below the system floor `FreePage` refuses, and no reclaim walk
  reaches an unlinked page (`page.md` §5, `drop-table.md` DT1). This
  predates AZ-S1: the page used to hold the row. **No owner.**

- **`PAGE_HANDOFF` is neither written nor read, and its record type
  stays.** Verified at AT-S9, 2026-09-24. Its reader was the receiving
  core's write grant, struck at AW-S1b, and since AM-S4(d) analysis neither
  erases nor seeds on it and redo skips it. Its last writer was
  `range_alloc.cpp`'s range opening, retired at AT-S9 with insert
  spreading, so the durability cost the entry used to price (one
  `FlushPages` and one device sync per range opening) is gone too. What
  remains is the record type, which a pre-AT log can still carry and
  recovery must still decode, and `wal::LogPageHandoff`, whose only callers
  are the cells that build such a log. Retiring the type is a format
  decision of its own. Owner: `docs/spec/wal.md` §5.2.

- **Two cuts AM-S4(d) made possible and did not take.** Named by that
  stage's `critics-developer` pass, verified by it by trace and exhaustive
  grep, and deferred because the operator called for the push before they
  could be made and verified on their own:

  1. **`CoreRuntime`'s unshared-store arm** (`core_runtime.cpp`'s
     `else { DevicePageStore::Open … SetWalGate }`, the `owned_store_`
     member, its budget site, and `Config::buffer_pool_frames`). Every
     caller now sets `shared_store` unconditionally, and AM-S4(d) removed
     the last syntactic path to null when `share_pool ? store_.get() :
     nullptr` became `store_.get()`. The one cell touching the budget field
     asserts it is *ignored*.
  2. **`SuperBlock::wal_anchors()` and `MountAnchorOf`'s loop.** Only slot
     0 can be non-zero — `SetWalAnchor` refuses every other unconditionally
     and `Decode` refuses the volumes that could carry an old one — so
     `MountAnchorOf` collapses to `wal_anchor(0)` and the vector accessor
     loses its last caller. The loop is deliberately kept for now as a
     canary, and `superblock_checkpoint_anchor.hpp` says so at the
     declaration; cutting it means deciding the canary is not worth a pass
     over 64 entries once per mount.

  A third, `FrameBudgetShare`, was deleted at BE-S4 with the per-core
  share it computed. Both remaining are dead code this stage *created*,
  which is the class AM-R4 warns about: a field nothing writes and nothing reads is worse than no
  field, because the next reader assumes it means something. Owner:
  whichever stage next opens `core_runtime.cpp` — AM-S3 touches the same
  file.

- **Every sync covers every live segment, so its cost grows with the
  checkpoint interval's worth of log.** Restated at BC-S5 on
  `worktree-wal-recycling`, 2026-10-07 (CN-9 §4 C2); first verified at
  `8f9a887` by reading.
  - `FileLogDevice::Sync` `fdatasync`s every segment of the live run
    (`src/wal/file_log_device.cpp`, `include/kds/wal/stream.hpp`). Its
    comment forbids narrowing that to the tail, because a roll can land
    between the stream capturing its watermark and the device sync.
  - **Since BC the run is bounded**: segments wholly below the durable redo
    start are removed (`wal.md` §11-4), so the calls per sync are the
    segments since the anchor's, not every segment the instance ever wrote.
    The cost no longer grows with the instance's age.
  - Syncing only the segments *written* since the last sync would be
    enough: the previous tail plus any segment created since. Nothing
    records which those are.

  Cost: one `fdatasync` per live segment per durable-point advance, bounded
  by the checkpoint cadence. Not measured. Owner: `docs/spec/wal.md` §3,
  `wal/file_log_device.cpp`.

- **A segment roll does I/O inside the instance's append latch, which
  `wal.md` §6 both says and denies, and its cost was never priced.**
  Verified at `8f9a887`, 2026-09-28, by reading (CN-9 §4 C4).
  - `WalStream::Append` takes the stream latch and calls `Roll` under it
    when the record does not fit (`src/wal/stream.cpp:210-219`).
  - `Roll` reaches `CreateSegment` (`:142-148`, `:63-64`). That runs
    `posix_fallocate`, zero-fills 64 MiB, writes the header, `fsync`s the
    file and then `fsync`s the directory (`src/wal/file_log_device.cpp`).
  - **Since the unheadered-tail fix (2026-10-07, `worktree-fix-roll-header`)
    the roll first syncs the device** through the stream's sync gate, so the
    segment it leaves is sealed durably before the next one exists - one
    more `fdatasync` per roll, and a wait for any sync already in flight.
  - **The behaviour is deliberate.** The authority for it as built is
    `include/kds/wal/stream.hpp:35-43`, which makes the latch a mutex for
    exactly this wait.
  - **The spec contradicts itself here.** `docs/spec/wal.md:114`, in the
    `[PROPOSED]` §6, puts *"any segment roll"* inside the latched append,
    and in the same sentence calls an append *"no allocation and no I/O"*.
  - **The follow-on was never priced.** `stream.hpp` names it: *"if AL-S8
    prices this as material"*. AL-S8's files measure the single stream, not
    the roll.

  Cost: once per 64 MiB of log, every core's append waits through a
  latched device sync, a `posix_fallocate`, a 64 MiB prewrite and two
  `fsync`s. What that
  means for a device is CN-9 §4 C4's. Owner: `docs/spec/wal.md` §6 for the contradiction, and
  `wal/stream.hpp`'s latch protocol for the cost. Price it first: the
  CLAUDE.md rule is to re-measure the premise before building the fix.

## Multi-core state

- **Closed 2026-09-03, recorded because the closure is the interesting
  part.** A peer's `superblock_` was a default-constructed copy, and zero
  is a legal value of most of its fields. Three fields had been carried
  across one at a time after each was caught answering a silent, legal,
  wrong zero — the WAL anchor, `next_trx_id` (PW1), `log_topology`
  (AL-7e) — and the AL-S9 review found **the next three already live**:
  `version`, `create_time` and `last_mount_time`, which a peer's
  `SHOW META` printed as `0` and as the epoch under `peer_listeners = on`,
  against `docs/spec/crosscore.md` CC11's *every core reads with the same
  authority*.

  Field-by-field was never going to end, because nothing listed which
  fields a peer was entitled to answer and nothing checked. `Open` now
  takes the volume's whole image and **refuses a config without one**, so
  there is no list because there is no choice. Owner:
  `include/kds/server/core_runtime.hpp`.

  **The closure immediately paid for itself**, which is why it is recorded
  rather than dropped: handing over the real image broke 123 tests that had
  been running against a volume they contradicted. See the fixture entry
  under Testing.

- **No transaction has a lifetime ceiling any more, and the sweep that
  enforced one is gone.** Verified at AT-S6 (2026-09-23) on
  `at-s6-2pc-retired`. AN-R14 ruled a 10 s envelope and a 60 s ceiling for
  every transaction, and what was built enforced it over **cross-owner
  participant contexts alone** (`ShippedStatementExecutor::ExpireEnrolled`)
  - AW-S3's record already said the instance-wide half was unbuilt and
  unlettered. AT-S6 retired the executor with the protocol, so the narrow
  half went too: `txn/manager.hpp` has no clock, no transaction carries a
  start time, and **an abandoned explicit transaction holds the instance's
  read horizon and its undo for the life of the process**.

  A client that opens a transaction and stops talking is what reaches it,
  and nothing refuses or reclaims. It is a widening of AW-S3's gap rather
  than a new one, and it is now the whole of that rule. Owner:
  `instructions/v3.0.0/workorder-aw-m1-close.md` §11.2, which sized the
  instance-wide half and ruled it its own letter.

- **A core that stops is not an idle core, and it pins the instance's
  commit-order floor for good.** Verified at AN-S2 on
  `an-s2-read-view-cutover`, recorded here per AN-R14's instruction that
  it belongs in this file once the window is load-bearing — which AN-S2
  made it. The floor (`include/kds/txn/instance_visibility.hpp`) is bounded
  by the minimum issue cursor over attached cores, and AN-S1b unpins an
  *idle* core by burning its block on that core's own tick. A core that
  wedges or shuts down takes no tick, and nothing else ever republishes its
  slot: its cursor stays where it stopped, the floor with it, and the
  window grows by one entry per commit for the life of the process. Not
  reachable today — the expeditor stops every core together — and stated
  so AN-S1b's landing note is not read as closing AN-R13 entirely. Owner:
  `instructions/v3.0.0/workorder-an-read-view.md` AN-R14, first bullet.

  *(The entry that stood here — "the read view and the read horizon are
  both per-core, and one spec sentence asserts the opposite" — closed at
  AN-S2: `ReadView::Visible` decides by the instance's commit-LSN window
  and floor, `ReadHorizon()` answers the instance-wide oldest snapshot, and
  `crosscore.md`'s "the trx-id domain is global, so ids compare cleanly" was
  rewritten at AN-S4. The two cells that fail on the old predicate and
  pass on the new one are
  `VisibilityWiringTest.ACommitOnAHigherBlockIsVisibleToALowerCoresNextView`
  and `…ATransactionBegunAfterTheMintFromALowerBlockStaysInvisible`.)*

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
  (`bench/v3.0.0/results-ay-s11-overhead-v2.7.0-530-g0552d55.md`, cell 3):
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

**The check-to-write window closed at AY-S5** (on
`ay-s5-d9a-parent-fence`): a child's forward check holds the parent row's
`S` from before its descent to its decide (D9(a), `foreign-keys.md` §2a,
§3a), so a parent `DELETE` on any core waits for it. The entry that stood
here recorded the window from AT-S5f (verified at `f247c52`), and AY-S4
reproduced it at `64b97e7` (a two-core rig cell, an orphan 5/5). That cell
went at BB-S3's review - BB-R3 then refused the below-mark key it wrote
behind the walk, so it passed with nothing tested. BD-S3 withdrew that
refusal on a btree (BD-R5), so the shape is reachable again through SQL. The
window stays pinned on one thread by `FkParentHoldTest.AParentDeletedBetweenAChildsCheckAndItsWriteIsRefused`.
A second orphaning shape AY-S4 found - a parent `DELETE`
answered "no children" over a child an undecided `UPDATE` had moved off it,
the rollback then restoring the reference - closed in the same stage, the
reverse check reading such a row's earlier version (AY-Q8); its bug entry
went with it.

The entry before that - a transaction whose
participant was deleting the parent answered `busy` across cores where one
core answered `violation`, and could not clear inside an explicit
transaction (AO-S5(b) C1, verified on `ao-s5b-c1c2` over `25c5849`) -
**closed at AT-S5f** with the two things it needed: a `DELETE` that
shipped to the parent's owner, which went at AT-S5, and
`FkPendingDeleteTable`'s pre-gate ahead of the visibility read, which went
with the probe protocol. Both checks read the rows themselves now, so
there is no second core's registration to be answered by.

## Multi-core state, continued

**A volume an engine older than `1b5d252e` wrote at `cores > 1` no longer
mounts** (closed at BD-S2, `62470f56`, verified at `07e822a6`): superblock
20 refuses every version-19 volume (BD-R8, no migration), so a btree leaf
two cores filled out of key order before BB-S3 can no longer reach the
fallback-free leaf search or the `ORDER BY <pk>` elision. The entry that
stood here recorded that such a volume mounted under BB-R11's byte check,
which went with the bump (`heap-and-tuple.md` §4.1).

- **Two concurrent `CREATE ASSERTION`s can place `sys.assertions` rows out
  of issue order.** By reading, on `bb-s0-order` at `bddd450c` (BB §1.8),
  re-read on `worktree-bb-issue-under-the-leaf` at `6dc792c9`; no cell
  reproduces it. `sys.assertions` is the one system relation whose rows carry
  a Keystone word. Its id is issued before the build
  (`assertion_catalog.cpp:493`, `AllocateRowId(kSysAssertionsTable)`) and
  placed after it, by `EncodeRow` and `ChainInsert` (`:247-250`) under the
  root page's hold (`:214`, `catalog.md` CT7) - two latched spans, so two
  creates on two cores can place out of issue order: defect A's shape, on a
  system relation. Across a tail-page boundary the one issued first and
  placed second is refused `OutOfRange` after its build. **Not read**:
  whether any reader depends on `sys.assertions`' key order. BB fixed the
  user relations only (BB-R9, on BB-Q5: system relations out). No orphaned
  page was ever reachable here: the insert holds the chain's root exclusive
  across `ChainInsert` (CT7, since AT-S17), so two cores never grow this
  chain at once. Owner: `assertion.md` §7.

- **The Cabin store's partition latches are taken at `cores = 1`.** By
  reading, on `at-s12-prose-sweep` at `2b20369` (found by AT-S12's
  `rules.md` §3 sweep). AT-S7 made the store the instance's with a
  `std::mutex` per partition (`stats/cabin_store.hpp`'s `Partition`), and
  unlike every other latch `rules.md` §3 indexes it is not null-armed, so a
  single-core instance pays an uncontended lock and unlock on every Cabin
  probe, append and serve. G1's compile-out clause
  (`ar0-architecture-revision.md` §3) does not admit it; G2 says cores = 1
  pays nothing. Cost, not correctness, and unmeasured. Owner:
  `docs/spec/cabin.md`.

- **An unexplained ~0.4 s stall inside foreground time, on the post-AT
  engine only.** Measured by AT-S13 on `at-s13-prices` at
  `v2.7.0-391-gf6f2073` (2026-09-26, `bench/v3.0.0/results-at-s13-prices-v2.7.0-391-gf6f2073.md`,
  cell 2): in three of the measured commit's seven runs one
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
  scenario rebaseline on `bench-rerun-scenarios` at
  `v2.7.0-531-g9a0525d` (2026-09-30,
  `bench/v3.0.0/results-scenario0-stockmarket-v2.7.0-531-g9a0525d.md` §6,
  `results-scenario2-freight-v2.7.0-531-g9a0525d.md`). A write descent
  whose leaf no longer covers its key restarts from the root
  `storage::kMaxDescentRestarts` (4) times and is then refused
  `TXN_CONFLICT retryable=1 btree descent for key K from page P gave up
  after 5 attempts` (`btree.cpp`). Scenario0's `cores = 8`, `strict` cell
  hit it on one `trades` insert in 3 of 3 runs, the same key 113 from the
  same root page 138 each time, and in none of its nine other cells.
  Scenario2's `cores = 8`, `group` cell hit it three times: run 2 on
  `freights` (key 107, page 145) and `charges` (key 135, page 147), run 3
  on `charges` at the same key and page. All were on fresh
  relations under eight appending sessions. The message names two causes
  and does not choose: split churn outrunning the descent, or a core
  descending from a memoised root that has since grown a level. The source
  comment calls the stale root the everyday one. The same key and page in
  every repeat, and a `catalog changed ... runs again` re-run logged in the
  same second of one run, fit that reading but do not prove it. A refusal,
  never a wrong answer, and retryable by design: the client's retry crosses
  a task boundary, where `Revalidate()` drops the memo. Scenario2's driver
  retried and lost nothing. Scenario0's driver does not retry, so each such
  cell tears one transaction and exits 1. The engine could tell the two
  causes apart and does not: since AT-S16 a root a level grows over carries
  the grown-over mark, which `SecureParents` reads and `DescendTo` does not,
  though it reads that root page on every attempt - the comment saying a
  stale root "does not look different from here" predates the mark (AT-S5c).
  And §5's "a restart makes progress" holds under split churn only: from a
  stale root every attempt fails the same way (`insert_placement.hpp` says
  so). What is open is whether a stale-root miss should re-read the anchor
  inside the statement rather than cost the client a round trip. No cell
  isolates which cause fires. Owner: `docs/spec/heap-and-tuple.md` §5.

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
  §0), verified at `26766699`:
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
  - **The leaf space a purge frees comes back only at a dividing split**,
    never at an append split: a retired slot keeps its directory entry.
  - **A wide window collects every target before it writes**, so its
    target list grows with the window.
  - **A purged version's secondary-index entries are never removed** (BH-Q8
    (a)), so the equal-index-sort-keys defect gains a path
    (`docs/inflight/bugs/a-run-of-equal-index-sort-keys-promotes-one-separator-twice.md`).
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

- **An omitted pk on a btree can be refused `AlreadyExists`.** Verified at
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

## Locks

- **A write refused by a lock unit waits on that unit's slot, and a wake
  is not a grant.** A refusal on the ask's own unit has waited on its slot
  since AX-S2b, one found in another unit since AY-S2, and a first encounter
  with a holder between its decide and its release since AY-S3. What still
  polls is a refusal with no slot behind it - no lock table, a holder only
  the header names, an assertion's group.
  Found by AT-S13's review, measured on `at-s13-prices` at `896af54`
  (2026-09-26) as a *refusal*: `CommandDispatcher::NoteBlockingWriter` parks
  only when `txn_->IsInFlight(trx)`, and until AX-S1 that walked **this
  core's** `live_`, so a holder on another core read as decided and the
  writer was refused `TXN_CONFLICT retryable=1` where AO-S3 promised a wait.
  AT-S13's cell 1 server log
  (`bench/v3.0.0/archive/at-s13-prices-v2.7.0-391-gf6f2073/s8-kds.log`)
  carries 7 refusals of that shape, part of cell 1's 4-6% hot-row refusal
  price. AX-S1 made the predicate the instance's, so the site parks on a
  peer's holder; **AX-S2b** made `BorrowChain`'s refused tuple ask register
  a wake, so that wait is on the unit's slot - flipped at the holder's
  release, after its retire, with a kick to the waiter's core - and its
  re-run comes after the release (`row_wait_wake_rig_test.cpp`). Verified by
  reading on `ax-s2b-row-wait-wake` at `043aee7`, the stage's review.
  **B6 is closed at AY-S3** (on `ay-s3-b6-and-q9-cells` from `e187b2b`,
  `raft-marks-2026-09-29.md` §13): `NoteBlockingWriter` records the block
  whenever the refusing unit's wake names the holder, in flight or not, with
  the repeatable-read guard unchanged beneath it
  (`row_wait_wake_rig_test.cpp`'s first-encounter cell). What stays open,
  retryable, never a wrong answer:

  - **A wake is not a grant.** `TryAcquire` does not queue, so a third
    writer can take the unit between the flip and the re-run; the re-run
    parks again under the same deadline, and under sustained contention
    ends at the 1 s fault net naming whichever holder came last.

  A holder that never decides costs a cross-core writer the 1 s fault net
  and its defect warning, as a same-core writer pays. AX closed carrying
  three (`instructions/v3.0.0/workorder-ax-inflight-publication.md` §7
  items 1-3): the cross-unit refusal (item 2), which handed back no slot,
  **is closed by AY-S2** (on `ay-s2-containment-wake` at `7d90ca7`, the
  containment wake; `txn.md` §5), the first encounter (item 1) **by
  AY-S3** as a wait (§13), and a wake not being a grant (item 3) proposes
  no action. Spec: `docs/spec/txn.md` §5.

- **The relation `IS` covers a statement's outermost walk and nothing else,
  and AT's quiet-wrong defence is sequenced as though it covered every
  read.** Verified 2026-09-09 on `ao-m2-close` at `cf3d0d0` by reading the
  sites: `src/exec/step_vm.cpp:1960` and `:2030` guard the position report on
  `index == 0`, and `src/server/remote_step_service.cpp` passes no
  `PositionSink` at any of its three execution sites (`:458`, `:886`, `:1080`).
  So a nested step's walk — a join's inner relation — and every remote step
  declare no position and hold no relation `IS`.

  **A point, index or Cabin read declares nothing only where its own path
  serves it**, which is narrower than `txn.md` §5 and AO-S6e-b's list both
  say and is corrected in the spec with this entry: each falls through to
  `RunWalkStep` carrying the same `index` (`step_vm.cpp:615` for a heap point
  read, `:1210`, `:1223` and `:1233` for an index probe with no usable index,
  `:760`, `:776` and `:812` for a Cabin miss), and a fallback at `index == 0`
  reports like any other walk.

  **Why this reaches past `DROP TABLE`.**
  `instructions/v3.0.0/ar0-5-amendment-uniformity.md` §7 sequences M3's
  schema word on *"the relation `IS`/`X` already in from AO-S6"*, and its §8
  makes that `IS` the whole defence of the one quiet-wrong surface M3 opens:
  *"DDL's `X` cannot be granted while it is held, so a stale parse cannot be
  executed, only rejected at the version check."* Where no `IS` is held the
  `X` **is** granted, so the defence holds on the shape that declares and on
  no other. AT's first cell — the schema word deliberately not bumped,
  asserting that the lock alone still blocks the DDL — is written against
  the same assumption and would pass only on that shape.

  `docs/spec/drop-table.md` DT7 carries the same conditionality in the
  narrower form it is visible in, and does not say which readers are
  positioned.

  **Not a defect in AO-S6e-b**, whose own "what it does not do" list states
  every one of these omissions; a gap between what M2 built and what AT is
  sequenced against. Owner:
  `instructions/v3.0.0/workorder-ao-m2-lock-family.md` AO-0 item 26, closed as
  mooted at AT's close (`workorder-at-m3-uniformity.md` AT-9, verified at
  `e6d9098`), and `ar0-5-amendment-uniformity.md` §7 and §8.

  **Closed at AT-S1 on `m3-at`** for every shape above: the compiler
  declares each relation it binds (`step_compiler.cpp`), the three write
  verbs declare at resolve, and a remote-step producer took its own `IS`
  in its frame (`remote_step_service.cpp`, deleted at AT-S10 with the
  protocol). **What stays open is the
  direction, and it is a correction to AR0-5 §8 rather than a gap**: the
  ask is a non-blocking `TryAcquire`, so a reader arriving while a DDL holds
  `X` is refused and reads on - sound by catalog MVCC, catalog-only DDL and,
  for a drop, the statement epoch that holds its pages' reclaim until every
  statement that could have bound the relation has ended (`drop-table.md`
  DT1), not by the lock (`read_borrow.hpp`; `workorder-at-m3-uniformity.md`
  AT-7 item 10, AT-0 item 10).

- **`CompileWhere` resolves an UPDATE's or DELETE's subquery relations
  under no view.** Verified 2026-09-09 on `m3-at` at `4b06051` by AT-S1's
  review: `UpdateInner` and `DeleteInner` resolve their own relation under
  the session's view (DT3c) and then call `exec::CompileWhere` with
  `view = nullptr`, so `UPDATE t SET v = 1 WHERE id IN (SELECT id FROM u)`
  resolves a `u` the session cannot see - a relation another transaction
  created and has not committed. Pre-existing; AT-S1 takes an `IS` on that
  relation, which changes nothing about the visibility. The fix is one
  argument at each site and changes a refusal for a shape that works today,
  so it is recorded rather than applied. Owner: `docs/spec/ddl-transactional.md`
  DT3c.

## Page reclamation

What BF (`instructions/v3.0.0/workorder-bf-drop-table-page-reclaim.md`)
leaves, each verified on `worktree-drop-table-page-reclaim` at `f2de416c`. A
dropped relation's pages are reclaimed (`drop-table.md` DT1); these are the
pages and the cases it does not reach.

- **Pages no root reaches stay leaked.** A reclaim frees what a tombstone's
  roots reach, so these stay allocated for the life of the volume:
  - a page left unlinked by a failed heap growth, btree split or var-heap
    growth;
  - a failed `CREATE INDEX`'s tree, which no anchor slot names;
  - a failed or rolled-back `CREATE TABLE`'s pages, which no tombstone
    names;
  - a refused catalog report's page (WAL, above);
  - a previous run's undo pages (UP4, `txn.md` §4.1);
  - a dropped assertion's Bound Cabin chain;
  - Waystone pages.
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
  next mount, and each refusal is counted `reclaim_refused`. The equal-sort-keys index bug
  (`bugs/a-run-of-equal-index-sort-keys-promotes-one-separator-twice.md`),
  if it ever makes two descents reach one child, lands here: a refusal,
  never a page freed twice.
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
  covers that.
- **A tree freed in one pass would pass every cell.** On a volume of one
  map region, a map flush writes the region's one page whole, so the
  per-level syncs matter only to a tree spanning regions - past 65,280
  pages - and no cell builds one.
- **A root growth's anchor publish is logged after its split**
  (`bugs/a-root-growths-anchor-publish-is-logged-after-its-split.md`).
  Where that cut stands, a reclaim walks from the grown-over root: the
  leaf chain reaches every leaf, and the new root and its right half's
  internal nodes leak.

## Decisions the revision has not taken

- **AR0's D1–D16: four are taken, one of them against AR0's own
  proposal.** `instructions/v3.0.0/workorder-al-m0-single-wal.md` carries a
  table of the D-items *"taken as ratified for M0's scope"* — **D3, D4, D14
  and D15** — of which only D15 was explicitly stamped (as AL-R8, 2026-09-03).

  **D3 is the one to know about.** AR0 proposed *(a) a dedicated log core,
  other cores fan in via ring*. AL-R1 built something else — every core
  appends under one latch, with a single writer thread — and the work order
  says so outright: *"every core still appends, which is where AL-R1
  departs from D3(a)."* That is a D-item settled in practice, against the
  proposal, and shipped at the cutover. **Ratified 2026-09-28** with
  AR0-5-R, whose R10 takes R0's core-0 placements, the log stream among them,
  as D3 answered in practice (`raft-marks-2026-09-28.md` §7; verified at
  `5dc4081`).

  Of the remaining twelve, AR0-M and the later marks take D1(b), D2(a),
  D7, D8, D9(a) and D11 (`CLAUDE.md`'s Open Decisions paragraph lists
  them); the rest are CLA's proposals awaiting the word. **Three are
  marked `[quiet-wrong]` by AR0 itself — D7, D8, D9 — and all three are
  marked by the operator**: AR0-M3 marks D8, AR0-M4 marks D9, and AR0-M8
  marks D7 (2026-09-29, `raft-marks-2026-09-29.md` §3), its last gate
  lifted in the following letter. *(Swept 2026-09-09 on
  `ao-m2-close` at `cf3d0d0` at M2's close, which needed the same count; this
  paragraph read "three are marked … meaning a wrong choice converts a refusal
  into a wrong answer" and did not reach AR0-M.)* D1 carries no such tag; its
  proposal argues *about* a quiet-wrong surface (write skew under SI) without
  AR0 classing the item as one.

- **AR0 §6's prerequisite is answered on paper and not re-measured.** §6
  requires RW-C1 attribution — what the unattributed reactor wall clock
  actually is — before D6 and D10 go non-zero and before M0's baselines are
  interpreted. AR0-V1's own consequence line says the prerequisite **is**
  answered, by three named v2.x results files at `1769487`, and in the
  direction that *"a single sync point off the execution cores is supported
  and D2/D10's 'scheduler latency' premise is not"* — the figure being
  94–98%, which AR0-V1 corrects from the body's 92–98%, attributed there to
  the WAL drain's `fdatasync`.

  **The residue is that all of it prices the engine AR0 replaces.** What is
  missing is the v3 number: AL-S8's `fdatasync`-share-of-reactor-wall-time
  cell, which the work order's own row records as not run with no number
  claimed. Owner:
  `instructions/v3.0.0/ar0-architecture-revision.md` §6 and AR0-V1.
