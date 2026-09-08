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

- **`PAGE_HANDOFF` is written and never read.** Verified at AM-S4(d),
  2026-09-07. The record's consumer was the receiving core's write grant,
  struck at AW-S1b; analysis neither erases the page from the dirty table
  nor seeds a recLSN for it, and redo skips it — so nothing acts on one.
  `range_alloc.cpp` is the single remaining site that appends one, and it
  pays PL §9 rule 1's durability ordering for it: **one `FlushPages` and
  one device sync per range opening**, for a record with no reader. The
  ordering is kept whole rather than half-kept, which is the right state to
  leave it in, but the cost is real and attributable.

  Not fixed here because retiring a record type is a format decision of its
  own: the kind stays in `ring_message.hpp`'s frozen enum, AU-R4's count
  freeze is unbuilt, and AU-R5 is where struck kinds are meant to go.
  Owner: `docs/spec/wal.md` §5.2, and `docs/spec/crosscore.md` CC7 for why
  the handoff exists at all.

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

## Foreign keys

- **A transaction whose participant is deleting the parent is answered
  busy across cores where one core answers violation, and inside an
  explicit transaction that busy cannot clear.** Verified on
  `ao-s5b-c1c2` over `25c5849` (AO-S5(b) C1), by reading: the forward
  probe's check view now carries the requester's own participant as its
  writer, so `CheckParentPresent` would answer that participant's own
  delete-mark `kViolation` as it does locally - but `FkProbeServer::Answer`
  consults `FkPendingDeleteTable::Pending` first, and that table is keyed
  on the deleting session with no coordinator identity to exclude the
  asker by (`fk_intent.hpp`, the `Pending` comment). So a `BEGIN`, a
  `DELETE` of the parent shipped to its owner, then a child `INSERT`
  naming that parent at home is refused `TxnConflict retryable=1` on
  every retry until the transaction ends - a retry loop that cannot
  succeed, the shape F1 named. Pre-existing (before C1 the same probe was
  answered busy by the delete-mark itself); C1 made it the one asymmetry
  left between one core and two. The fix is an own-aware pre-gate, which
  needs the coordinator's identity in the registration, or the probe
  running the pre-gate after the visibility read for a writer view. Owner:
  `docs/spec/foreign-keys.md` §5, which states the asymmetry;
  `instructions/v3.0.0/workorder-ao-m2-lock-family.md` AO-S6's units are
  where the delete side's waits belong.

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

  The remaining twelve are CLA's proposals awaiting the word, and **three**
  are marked `[quiet-wrong]` by AR0 itself — D7, D8, D9 — meaning a wrong
  choice converts a refusal into a wrong answer rather than an error. D1
  carries no such tag; its proposal argues *about* a quiet-wrong surface
  (write skew under SI) without AR0 classing the item as one.

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
