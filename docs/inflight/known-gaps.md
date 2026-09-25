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

- **EV8's exhaustion protocol is not built, and `eviction.md` describes it
  as though it were.** Verified at `dd0bfe9` and re-verified at `a140e8d`:
  nothing in the tree reads `kds.evict_retry_budget`, and no path returns
  `ResourceExhausted` for a full pool. The occurrences in `src/` belong to
  other subsystems - the lock table's, the row codec's, the sort's, the
  aggregate's and the lease services' among them - and `src/storage/`
  returns it in one place only, `anchor_page.cpp`'s slot cap, which is not
  the pool. **The scan ring's own retry was the fourth name on that list
  until 2026-09-06 and is gone**: `PinForScan`'s eight attempts and its
  `ResourceExhausted` left with the ring's port to the `loading_` protocol
  at `6cbd6f8` (AM-S2-P S-P1), so a ring fault waits for the loader like
  every other accessor. §3.3 spells out
  a three-step protocol (yield, retry to a budget, then a truthful statement
  error naming the core and pool size) and EV8's row promises "no waiting,
  ever" with occurrences counted. None of that exists.

  **What the code does instead**: `buffer_pool_frames` is a *soft* target.
  `InsertFrame` sweeps for the excess when a fault takes the pool past it
  (`device_page_store.cpp`, the `sweep` arm), and if the sweep reclaims
  nothing - every candidate pinned, dirty, or resident by class - the insert
  proceeds anyway and the pool grows past its budget. So an undersized pool
  is not a visible, countable, truthful signal; it is memory growth. That is
  a different operational answer from the one an operator reading
  `eviction.md` §3.3 would expect, and it is the one they get.

  **Found while sizing AM-S3**, whose ruling AM-R6 says to "keep the budget
  and the truthful error, and count the cross-core case separately" - a
  narrowing of something that is not there. The premise AM-R6 argues from is
  false in the same way: it says the retry budget "stops being a
  statement-local fact" under a shared pool, and there is no retry budget.
  What *is* newly true under sharing is that a frame a statement waits on
  may be held by another core's task, which matters exactly when an
  exhaustion path exists to be distorted by it.

  Owned by `docs/spec/eviction.md` (EV8, §3.3, and the
  `kds.evict_retry_budget` row in §7's settings table) and by
  `instructions/v3.0.0/workorder-am-m1-shared-pool.md` AM-R6, which needs
  re-scoping onto a stage that builds EV8 first.

## Testing

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

- **Three cuts AM-S4(d) made possible and did not take.** Named by that
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
  2. **`FrameBudgetShare`** (`expeditor.cpp`, declared in its header). Its
     only non-test call divides a total that nothing keeps divided: at one
     core the share is the total, and above one `Start()` overwrites it
     with the total. **One behavioural residue the reviewer did not test**:
     core 0's mount pass — recovery plus the completion checkpoint — runs
     on `total/cores` frames before `Start()` restores the total. The
     budget is an eviction target rather than a hard cap, so this is a
     warm-up cost and not a refusal, but the claim is untested and should
     be measured or pinned before the function goes.
  3. **`SuperBlock::wal_anchors()` and `MountAnchorOf`'s loop.** Only slot
     0 can be non-zero — `SetWalAnchor` refuses every other unconditionally
     and `Decode` refuses the volumes that could carry an old one — so
     `MountAnchorOf` collapses to `wal_anchor(0)` and the vector accessor
     loses its last caller. The loop is deliberately kept for now as a
     canary, and `superblock_checkpoint_anchor.hpp` says so at the
     declaration; cutting it means deciding the canary is not worth a pass
     over 64 entries once per mount.

  All three are dead code this stage *created*, which is the class AM-R4
  warns about: a field nothing writes and nothing reads is worse than no
  field, because the next reader assumes it means something. Owner:
  whichever stage next opens `core_runtime.cpp` — AM-S3 touches the same
  file.

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

- **The cabin optimizer's build does not announce, so a write during its
  walk is lost.** Verified at AT-S7 (2026-09-23) on
  `at-s7-one-cabin-store`. The serve path's build announces its set before
  it walks (`cabin.md` §6), so the write hook appends into it from any
  core and the commit merges; `CabinOptimizerExecutor::BuildSeededSets`
  walks and then `Commit`s, which with one store for the instance is the
  hazard §6 used to delete structurally — a write on another core between
  its walk and its commit is in neither, and the set is banked short.

  Not reachable today for two reasons that are both configuration rather
  than construction: the controller is **off by default**
  (`cabin_optimizer`), and its walks run on core 0's tick. Closing it is
  the same two calls the serve path makes. Owner:
  `src/exec/cabin_optimizer_exec.cpp`.

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

- **A child's forward check holds nothing, so a parent can be deleted
  between the check and the child's write — across cores.** Opened by
  AT-S5f (2026-09-23) and found by its own `critics-developer` pass;
  verified by source read at `f247c52` on `at-s5f-fk-local`.

  **Cost: a quiet wrong answer**, and the one `foreign-keys.md` §1 says a
  constraint may not have — a committed child referencing a deleted
  parent, both statements reporting success.

  `ResolveForeignKeyParents` asks the lock table only on `kBusy`
  (`src/server/command_dispatcher.cpp`, `WaitForParentRowWriter`'s one
  caller), so a parent that passes is unheld from the check to the row
  write. The parent's `DELETE` takes that row's `X` and walks the child
  before marking, but the child row does not exist yet, so the walk is
  honest and empty. **One core closes it by running to completion**
  between the fork and the write; two reactors do not.

  **What it replaced is wider, not narrower.** Until AT-S5f a passing
  forward probe left a row-scoped reference intent on the parent's owner,
  and that owner's `DELETE` answered busy while one was live — the
  interval `[check, decide]`. The intent went with the probe protocol
  (AT-R15's D5, the operator's ruling), and this window is what the
  ruling entails rather than an oversight in carrying it out.

  **The fix is D9(a)'s `S` fence** and needs nothing else: the parent's
  `DELETE` already takes the row's `X` ahead of its walk, so an `S` the
  child's check holds from the check to its decide is refused by it.
  Owner: the following letter (`workorder-at-m3-uniformity.md` AT-0
  item 6), with `docs/spec/foreign-keys.md` §3a stating the window.

The entry that stood here before it - a transaction whose
participant was deleting the parent answered `busy` across cores where one
core answered `violation`, and could not clear inside an explicit
transaction (AO-S5(b) C1, verified on `ao-s5b-c1c2` over `25c5849`) -
**closed at AT-S5f** with the two things it needed: a `DELETE` that
shipped to the parent's owner, which went at AT-S5, and
`FkPendingDeleteTable`'s pre-gate ahead of the visibility read, which went
with the probe protocol. Both checks read the rows themselves now, so
there is no second core's registration to be answered by.

## Multi-core state, continued

- **Two cores inserting omitted-pk rows into one unsplit heap relation can
  refuse the lower id `OutOfRange`.** By reading, on `at-s10-ring-consumers`
  at `59ed9c0` (found by AT-S10b's prose pass); no cell reproduces it. Since
  AT-S10b every core issues from the relation's one mark
  (`Catalog::AllocateRowId`), but issuing and placing are two steps under
  two latches: core A issues `n`, core B issues `n+1` and places it first,
  opening a tail page with `min_key = n+1`, and `ChainInsert` refuses A's
  `n` below it (invariant 3). The sorted fill's carve has the same window.
  A refusal, never a wrong answer. It narrows AT-S9's leased-block entry,
  which AT-S10b closed, to a one-id window; within one core the two steps
  cannot interleave. A heap relation is creatable only before SUS-1, and a
  btree relation - the default since - places each id by descent and is
  unaffected. Owner: `heap-and-tuple.md` §4.1a.

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

- **No cell drives a stale writeback against a re-faulted frame.** Verified
  at `aaf0f47`. AT-S8 step 1b draws every frame's dirty generation from the
  store's own counter, so a frame evicted and faulted back never repeats the
  value a concurrent writeback recorded at its copy; the interleaving needs
  two writebacks and an eviction inside one device write, and no cell forces
  it. Owner: `docs/spec/page.md` §6.

## Locks

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
  `instructions/v3.0.0/workorder-ao-m2-lock-family.md` AO-0 item 26, which
  is **undecided**, and `ar0-5-amendment-uniformity.md` §7 and §8.

  **Closed at AT-S1 on `m3-at`** for every shape above: the compiler
  declares each relation it binds (`step_compiler.cpp`), the three write
  verbs declare at resolve, and a remote-step producer took its own `IS`
  in its frame (`remote_step_service.cpp`, deleted at AT-S10 with the
  protocol). **What stays open is the
  direction, and it is a correction to AR0-5 §8 rather than a gap**: the
  ask is a non-blocking `TryAcquire`, so a reader arriving while a DDL holds
  `X` is refused and reads on - sound by DT1, catalog MVCC and catalog-only
  DDL, not by the lock (`read_borrow.hpp`; `workorder-at-m3-uniformity.md`
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
  proposal, and shipped at the cutover. It is not awaiting the word; it is
  awaiting someone noticing it was answered.

  The remaining twelve are CLA's proposals awaiting the word. **Three are
  marked `[quiet-wrong]` by AR0 itself — D7, D8, D9 — and only D7 is still
  unmarked by the operator**: AR0-M3 marks D8 and AR0-M4 marks D9, so the
  quiet-wrong item M3 opens against is D7 alone. *(Swept 2026-09-09 on
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
