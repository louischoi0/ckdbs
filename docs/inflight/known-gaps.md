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

- **No test and no `sim/` cell constructs an `Expeditor`.** Verified at
  `f6ed10c`: the only construction in the tree is production's
  `src/server/main.cpp:344`, and the only Expeditor surface under test is
  `Expeditor::Config`'s parse-and-validate overlay
  (`tests/config_file_test.cpp`, ~30 cells over the config keys and the
  core-count and frame-budget refusals). Every other reference in `tests/`
  and `sim/` is a *comment* saying "as `Expeditor::Open` does it", with the
  test reimplementing the wiring it describes. Nothing reaches `Open` or
  `Serve`.

  So the multi-core **assembly** — who opens the log, who attaches, what a
  peer is handed, what runs before the listener binds — is covered only by
  hand-built approximations that can agree with each other while all
  disagreeing with the real thing.

  **What this gap is not.** It did not cause M0's two cutover defects, and
  saying so would point at the wrong fix. Both sat in `CoreRuntime::Open`,
  which `tests/core_runtime_test.cpp` does exercise directly at
  `cores = 2`; both were pinned by cells in that file and neither needed an
  `Expeditor` to catch. AL-7c's own record says why the null deref escaped
  — *"no test opened a single-stream peer that had one"* — a missing
  fixture state, not a missing layer. The honest statement is that this is
  **the layer above the one both defects were found in, and it is still
  untested.**

  Owner: no spec claims this ground. `docs/spec/sched.md` §8 and
  `docs/spec/wal.md` §16 each test their own half.

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
