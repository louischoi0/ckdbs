# Known Gaps

What is missing, what does not survive a restart, and what the code does
differently from what a spec claims. **Opened fresh for v3.0.0 on
2026-09-03** — the v2 list is at `git show 1769487:docs/inflight/known-gaps.md`
and none of it is carried across, because every entry described the engine
AR0 replaces.

Every entry names the commit it was verified at and the doc that owns the
fix. An entry whose verification predates its subsystem's last change is a
statement about an engine that no longer exists; re-verify or strike it.

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

- **The read view and the read horizon are both per-core, and one spec
  sentence asserts the opposite.** Verified at `004f949`. `ReadView::Visible`
  (`include/kds/txn/read_view.hpp:81-90`) decides visibility from a bound on
  transaction ids, and `TransactionManager::ReadHorizon()`
  (`src/txn/manager.cpp:559-579`) walks this core's live set and reader slots
  only. Both are sound only while a reader reads its own core's versions —
  which `txn.md` §4.1 states of the horizon and states nowhere of the
  predicate — and that holds today only because a peer reaches another core's
  rows by shipping the statement. A shared buffer pool ends it, and the
  failure is a dirty read and a purge that outruns a reader, not an error.

  **`docs/spec/crosscore.md:288-290` states the false premise as a fact**:
  *"the trx-id domain is global, so ids compare cleanly"*. Ids are leased to
  each core in disjoint blocks (`include/kds/txn/trx_id.hpp:74`,
  `:153-155`), so issue order across cores is not id order and no bound on
  ids orders commits between them. That sentence is the gap, written down as
  a guarantee.

  Owner: `docs/spec/txn.md` §4.1. The source read and the mechanism that
  closes it are `instructions/v3.0.0/workorder-an-read-view.md` AN-3 E.

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
