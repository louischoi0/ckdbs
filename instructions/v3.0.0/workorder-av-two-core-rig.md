# Work order AV — the deterministic two-core rig

Drafted 2026-09-07 by CLA against `origin/main` at `c9609c0`, as
`workorder-aw-m1-close.md`'s AW-S6. Letter **AV** per AR0-6 D26, which
rules the rig order is *not* `AS` — SUS-1 holds that letter — and proposes
this one. Every `path:line` is `[source-read]` at `c9609c0`; the rest is
`[design]`. **Nothing here is built**, which is AW-S6's own condition: the
stage's deliverable is this document. *(True at `c9609c0`. AV-S0 and AV-S1
are built since 2026-09-07 - §8 is the read and §9 the rows; §0-§7 stand
as drafted, with the corrections the build forced marked in place.)*

## 0. What gates on it, which is the reason it is drafted before it is needed

Two stages name the rig and neither can start without it:

- **AU-S2** (`workorder-au-ring-retirement.md:87`) — cross-core lock wake
  and victim notification as write-then-kick. Its gate is written as *"the
  rig, on `SimWaker`"*.
- **AO-S5** (`workorder-ao-m2-lock-family.md`) — cross-core waits. Its
  first deliverable **is** the rig: *"a deterministic two-`CoreRuntime` rig
  over a `SimWaker`, **first**"*.

`AW`'s corrected critical path is
`AM-S6 → AN-S2 → AO-S5 ← AU-S2 ← {AV, AU-S1c}`. AV is the only item on it
with no gate of its own, which is why AW-S6 is the one stage in that order
marked parallel.

## 1. Two citation corrections this draft has to make first

**AW-S6 asks the document to cite "AR0-6 D26 and AU-R6". There is no
AU-R6.** `workorder-au-ring-retirement.md` carries AU-R1..AU-R5 and stops.
The ruling AW meant is **AU-R3**, the only one whose subject is the rig's
waker: *"`SimWaker`. A deterministic waker for the rig: `Kick(core)`
records `(tick, dst)` and schedules the destination's idle block to end at
`tick + delay(seed)`. Replaces `SimRingTransport` for new consumers; the
two transport tests keep theirs until AT."* This order cites AU-R3 and
records the miss rather than inventing an AU-R6 to match the citation.

**AU-S1c is a stage, not a ruling, and it is unbuilt.**
`include/kds/sched/` holds `waker.hpp` and `waker_table.hpp` and nothing
else; there is no `SimWaker`. AU-S1 reported itself built while dropping
it (`1e4e446`), and AU-S1c carries it. AV's stages below assume AU-S1c
lands first and say so in their gates.

## 2. Rulings

**AV-R1 — The substitution seam, and AU-R3's stated one does not compile.
[design, needs the operator or a decision here]**

AU-S1c describes `SimWaker` as *"a `Waker`-shaped seam for a rig rather
than a second wake path — `WakerTable` holds `const Waker*`, so the sim one
substitutes where the table is built, and nothing in the engine learns a
new type"*. **That cannot work as the code stands.** `Waker::Wake()` is
`void Wake() const noexcept` (`include/kds/sched/waker.hpp:66`) — **not
virtual**, and `Waker` has no virtual destructor; `WakerTable::Register`
takes `const Waker*` and `Kick` calls `entry.waker->Wake()`
(`waker_table.hpp:52`, `:95`), a static call. A derived `SimWaker` passed
through that pointer would run `Waker::Wake()`, which writes to an
eventfd it does not own.

Four ways out, and CLA proposes the fourth:

1. **Make `Wake()` virtual.** One indirect call on the wake path, and a
   vtable on a class whose header argues it is "a single 8-byte write"
   (`waker.hpp:28`). Cheapest to write, and it puts a polymorphic type on
   the one path AR0-6 exists to keep cheap.
2. **`SimWaker` owns a real `Waker`** and delays the `Wake()` rather than
   replacing it. Keeps the engine's type, but the delay then lives in a
   thread or a timer, which is the non-determinism the rig exists to
   remove.
3. **Template `WakerTable` on the waker type.** No indirect call; every
   holder of a `WakerTable` becomes a template, and `Expeditor` and
   `CoreRuntime` both hold one.
4. **Substitute the table, not the waker** — a `SimWakerTable` with
   `Kick`'s signature, injected where `WakerTable` is built. The seam is
   already there (one table per instance, built in one place), the wake
   path keeps its static call, and "nothing in the engine learns a new
   type" stays true of `Waker` itself. What it costs is that
   `WakerTable`'s own `Kick` — the `sleeping` fence and the skip counter,
   `waker_table.hpp:86-97` — must be reproduced or shared, and a rig that
   reproduces the fence protocol is a rig that can disagree with it.

**Whichever is taken, AU-R3's sentence is rewritten by the same act**, and
that is this ruling's real content: the AU order states a mechanism that
its own code does not admit, and the rig cannot be built until that is
settled.

**AV-R1 marked — seam 4, the table. [operator, 2026-09-07, verbal]** In
the operator's own terms: *the attach point takes an interface
`WakerTable` implements; `SimWakerTable` wraps it, logs `(tick, dest)` per
`Kick`, forwards at the scheduled tick. `Wake()` stays non-virtual; AU-R3
rewritten to match.*

The mark is seam 4 as proposed **and it settles the one cost the proposal
left open**, which is worth stating because it is what makes seam 4 safe:
`SimWakerTable` **wraps** a real `WakerTable` rather than reproducing it,
so the `sleeping` fence and the skip counter (`waker_table.hpp:86-97`)
stay in one implementation and the rig cannot disagree with the protocol
it is testing. The rig's own content is the two things it adds around the
forward: the `(tick, dest)` log AV-R3 makes the determinism claim over,
and holding the kick until the scheduled tick.

What the mark obliges, and what it does not:

- **An interface at the attach point**, which `WakerTable` implements and
  `SimWakerTable` also implements by wrapping one. That is the production
  seam H4 asks about, and under the mark it is a virtual call **per
  `Kick`** rather than per `Wake()` - a cross-core wake, not the 8-byte
  write `waker.hpp` argues is cheap. Seam 1's cost was on the wrong path
  and this is why.
- **`Waker` is untouched.** `Wake()` stays non-virtual and `Waker` gains
  no vtable, so AU-S1c's "nothing in the engine learns a new type" stays
  literally true of the type it was said about.
- **AU-R3's sentence is rewritten**, per the ruling above and per the
  operator's own last clause. AU-S1c owes `SimWakerTable` rather than
  `SimWaker`, and the name in `include/kds/sched/` changes with it.
- **It does not settle H4's verdict.** Whether the interface can be
  injected without a production seam in `Expeditor` or `CoreRuntime` is
  AV-S0's read; the mark says which shape to build, not that the shape
  costs nothing. If a seam has to open there, `rules.md` still wants the
  justification.

**AV-R2 — The rig is the production arrangement, not a mock. [design]**
Two `CoreRuntime`s over **one** `DevicePageStore`, **one** WAL stream,
**one** `InstanceVisibility`, **one** `WakerTable` — which is what
`Expeditor` wires at `cores = 2` on a single-stream volume, and since
AM-S2 step 3 is the only arrangement a mountable volume has. A rig that
built two stores would be testing an engine that no longer exists, which
is the defect AM-S0(b) closed for `CoreRuntimeTest` and which this order
must not reintroduce.

**AV-R3 — Determinism is over the kick log, not over the schedule.
[design]** What a seed reproduces is the `(tick, dst)` sequence AU-R3
names, and nothing more: two reactors on two OS threads are not
deterministic in their instruction interleaving and this rig does not
pretend otherwise. A cell that needs a *state* to be reached
deterministically reaches it by a barrier, as the tree's threaded cells
already do; the seed is what makes **wake latency** reproducible, which is
the variable AO-S5 and AU-S2 are about.

**AV-R4 — The rig may not depend on machinery AW-S1b deletes. [design]**
`MayFault`, the extent lease, `LeasedIdSource`, the CC7 fault grants and
`TryClaimByStamp` are dead code awaiting deletion
(`workorder-aw-m1-close.md` §9). A rig that stands one of them up — a
leased peer store, a fault grant — is a rig that AW-S1b then has to
rewrite, and the last fixture that did this cost AM-S0(b) 123 cells. Two
`CoreRuntime`s over a shared store need none of it: `SetCoreOwnership` is
already skipped on a shared store (`core_runtime.cpp:348`).

**AV-R5 — One cell moves in from AM-S2-P. [cell]**
`ARingFetchWaitsForAPageAnotherCoreHoldsExclusive`
(`tests/am_s2_pin_protocol_test.cpp`) asserts that a scan ring waits for a
page another core holds exclusive. It takes the foreign hold through
`LatchFrameForTest(..., core=7)` **because it must**: two threads of one
store are one core to `PageLatch`, which admits a shared acquire under its
own core's exclusive (`page_latch.hpp`). The rig is where that cell stops
needing the hook, and AM-S2-P's §8.3 records it as "a cell with two
genuine cores is S3c's" — S3c became the port, so it lands here.

## 3. Hypotheses

| # | claim | how it fails if wrong |
|---|---|---|
| H1 | Two `CoreRuntime`s over one store, one stream and one waker table can be stood up outside `Expeditor` | `Expeditor::Open` does wiring no test can reach — the anchor, the transport, the catalog broadcast. AV-S0 is the read that settles it, and the existing two-core `core_runtime_test` fixture is the evidence it can |
| H2 | `SimWaker`'s delay reaches the reactor without a second thread | `Scheduler`'s idle block is an `IoBackend` wait; ending it early at a simulated tick means the backend has to be driven by the rig, not by wall time. If it cannot, AV-R3's determinism is over kick *order* only and the latency cells need a real `EpollIoBackend` |
| H3 | No cell AU-S2 or AO-S5 names needs a third core | each is stated over "core 0" and "core 1"; a third would make the rig a general N-core harness, which is a different order |
| H4 | The rig adds no production code | if a seam has to open in `Expeditor` or `CoreRuntime` to inject the table, that is production code with a test-only caller, and it needs its own justification under `rules.md` |

## 4. Measurement

None. The rig is a correctness instrument; its own cells are
mutation-checked (F3) like any other. **Overhead not measured** — the
interleaved A/B is suspended except for AM-S6 (`workorder-aw-m1-close.md`
§0 item 2).

## 5. Stages

Every stage: `critics-developer` review; suite plain, armed
(`KDS_TEST_PAGE_LATCH=1`), and armed with `KDS_TEST_FRAME_BUDGET=8`; stop.

| # | stage | cells (definition of done) | size | gate |
|---|---|---|---|---|
| AV-S0 | **The read.** What `Expeditor::Open` wires that a rig must reproduce, and whether the waker table interface can be injected without a production seam (H4). **AV-R1 is no longer this stage's to decide** - the operator marked seam 4 on 2026-09-07 - so what is left of that half is rewriting AU-R3's sentence to name `SimWakerTable` | a table appended here: every wiring step, reproduced-by-the-rig or not-needed, with the reason; AU-R3 rewritten | S | — |
| AV-S1 | **The rig itself**: two `CoreRuntime`s, one store, one stream, one visibility, one waker table; seam 4 per AV-R1's mark - an interface at the attach point, `SimWakerTable` wrapping a real `WakerTable` | the three cells §6 lists as the rig's own | M | AV-S0, AU-S1c |
| AV-S2 | **AV-R5's promotion**: the S-P2 cell over two genuine cores, the test hook gone from it | `ARingFetchWaitsForAPageAnotherCoreHoldsExclusive` passes with a real core-1 exclusive holder; **mutation**: drop the ring's page latch and it returns while the hold stands | S | AV-S1 |
| AV-S3 | **Hosting**: AU-S2's and AO-S5's cells by name (§6), each running on the rig | each named cell exists and is green; none uses `LatchFrameForTest` | M | AV-S1, and the owning stage's own gates |

## 6. The cells this rig must host, by name

AW-S6's condition is that they are listed by name rather than by
description, so a later reader can check the rig against the list.

**Its own three** (AR0-6 R1's cost is what they price):

1. **A kick wakes a parked peer.** Core 1's reactor is in its idle block;
   core 0 kicks; core 1 proceeds at the kick rather than at block expiry,
   and `sched_wakes_received` moves. (AU-S1 has this against real threads
   already; the rig's version is the deterministic one.)
2. **A lost kick costs one idle block and nothing else.** The publisher
   lands between the destination's last look and its raising of the
   `sleeping` flag, reads clear, skips the kick — `waker_table.hpp:86-97`'s
   stated best-effort case. The peer proceeds at block expiry, correct and
   slow, which is AR0-6-R1's stated cost rather than a defect.
3. **A page held `X` on core 0 blocks a `Fetch` on core 1** — AV-R5's
   promotion, and the first cell in the tree where two `PageLatch` holders
   are genuinely two cores.

**AU-S2's**, from `workorder-au-ring-retirement.md:87`:

4. **The waiter on core 1 proceeds at the kick rather than at idle-block
   expiry** (`sched_wakes_received` moves).
5. **The victim's refusal reaches the client** — `c168acb`'s lesson, that
   a report reaching nobody is the defect class.

**AO-S5's**, from `workorder-ao-m2-lock-family.md`:

6. **A waiter on core 1 woken by a decide on core 0 while core 1's reactor
   sleeps** (`sched_wakes_received` moves).
7. **With every kick delayed to the sim waker's maximum, the waiter still
   proceeds.**
8. **At the store, `MayWrite` admits a page its lease/grant arm refused at
   `9e5068c`, under the lock** — owner routing still in force (AO-1), so
   the cell is the store's rather than dispatch's.

**Note on cell 8, added 2026-09-07.** Its premise moved under it. AW-a
retired `MayWrite`'s grant arm and re-keyed the system-range arm
(`c9609c0`), and `MayWrite` no longer consults a lease or a grant at all:
it answers `false` only for a system page asked about by a core that is not
0. So "admits a page its lease/grant arm refused" now describes a refusal
that no longer exists. AO-S5 must restate the cell against what the
predicate does at the time it is written, and this order flags it rather
than restating it on AO's behalf. **Restated by AO-S5(b), 2026-09-07**: the
cell is absent. The refusal it named was retired, the one `MayWrite` keeps
(a system page asked about by a core that is not 0) is pinned by the
store's own cells, and no lock stands between a page and a write on a
peer - the page latch and the shared pool (M1) are what replaced the
write-right, and their cells are AM's.

## 7. What this order does not do

- It does not build anything. AW-S6's condition is the document.
- ~~It does not decide AV-R1.~~ **The operator marked it 2026-09-07**:
  seam 4, `SimWakerTable` wrapping a real `WakerTable`. Struck rather than
  deleted, because this list is what the order shipped without and the
  mark is what closed it.
- It does not build `SimWakerTable`. That is AU-S1c's, and AV-S1 gates on
  it - and the mark renames what AU-S1c owes, from `SimWaker` to the
  table.
- It does not widen to N cores. H3 is the claim that two is enough for
  every cell named in §6.
- It does not touch AU-S2's or AO-S5's own content. §6 lists their cells so
  the rig can be checked against them; restating them is theirs.

## 8. AV-S0 — the read: what `Expeditor` wires, and what the rig does with each

Source-read 2026-09-07 on `AU+AV-S0S1` from `8fa8e0a`, against
`src/server/expeditor.cpp` (`Open` at `:701`, `Start` at `:1384`) and
`src/server/core_runtime.cpp`. **The answer to H1 is yes and the answer to
H4 is "one interface, two runtime fields, no seam in `Expeditor`"**: the
rig stands two `CoreRuntime`s up outside `Expeditor`, exactly as
`tests/core_runtime_test.cpp`'s fixture does, and core 0 is a
`CoreRuntime` too. That is the one place the rig departs from AV-R2's
"production arrangement", and the table says per step what the departure
costs. Three dispositions:

- **rig** — the rig reproduces the step itself (`tests/two_core_rig.hpp`);
- **runtime** — `CoreRuntime::Open`/`AttachTransport`/`Run` does it, on
  every core, so the rig runs production's code by construction;
- **not needed** — with the reason, which is always one of: a fresh volume
  that is never remounted, no listener, no operator config, or advisory
  machinery nothing in §6 reads.

| `Expeditor` step | source | disposition |
|---|---|---|
| `CheckCoreCount`, `CheckFrameBudget`, `CheckPeerListenerConfig`, the hardware-core check | `Open` | not needed: config validation; the rig has no config file |
| `FilePageDevice::Open` | `Open` | rig, over `MemoryPageDevice`. AV-R2's claim is one *store*, not one file |
| `DevicePageStore::Open(kFirstUserPageId)`; `SetResidentLimit` (EV3's floor) | `Open` | rig |
| `SetFrameBudget(share)`, and `Start`'s `SetFrameBudget(total)` (EV4) | `Open`, `Start` | not needed: unbounded, the default. A cell about pressure sets `store().SetFrameBudget` itself |
| `OpenLog`, every `SetLogger` | `Open` | not needed: a null logger, which every runtime accepts |
| `BootstrapDatabase(cores)` | `Open` | rig, at `cores = 2` |
| `catalog.SetPlacementPolicy` | `Open` | rig, on **core 0's runtime catalog** (`Options::placement`). The bootstrap result's own catalog is unused after bootstrap: DDL runs through core 0's dispatcher, whose catalog is the runtime's |
| `FileLogDevice::Open`; `WalManager::Open(shared_stream = core_count > 1)`; `StartWriter` | `Open` | rig |
| `SetLatchArmed(core_count > 1, core_count)` | `Open` | rig. The store is shared, so `CoreRuntime::Open` skips it on both cores (its own arm is for an owned store) and this is the one arming site |
| `SetWalGate` | `Open` | rig |
| `catalog.SetWal` | `Open` | runtime, for each core's own catalog |
| `cabin_store_`, `trail_recorder_`, `undo_log_` | `Open` | runtime (Cabin on; no recorder on a runtime, which is the header's own rule) |
| `RecoverCoreAtMount`; `InvalidateFromPeer` after redo; `AuditCatalogAfterRecovery`; `FinalizeDeleteMarksAtMount`; `ResetAccessStatsIfDamaged`; `SweepUnownedSpills`; the `next_trx_id` raise | `Open` | not needed: a fresh volume, never remounted. **Stated rather than hidden**: the rig's log is never recovered, and a core-0 runtime runs no pass of its own ("one stream, core 0's mount pass covered this core" - which in the rig is nobody's). A cell about recovery is `sim/`'s or `core_runtime_test.cpp`'s, not this rig's |
| `trx_ids_` with `PersistSuperBlock` | `Open` | runtime, **and this is the first of the two runtime changes AV-S1 makes**: a core-0 `CoreRuntime` refused every carve `NotImplemented` (the arm was written for peers and applied to every core), so it could not open a writing transaction, which is why every two-core fixture in the tree built core 0 by hand. Core 0's arm now writes its image to page 0 and syncs the store - `Expeditor::PersistSuperBlock` on the runtime's own copy. Sound in the rig because nothing else writes page 0 (the peer's anchor is dropped, below); no production path builds a core-0 runtime (`grep -rn "CoreRuntime::Open" src/` finds `Expeditor::Start`'s peer loop from 1 and nothing else) |
| `visibility_.emplace()` | `Open` | rig, one for both cores, handed to each `Config::visibility` |
| `txn_manager_` | `Open` | runtime |
| `dispatcher_` and its limits (`aggregate_limits`, `sort_max_rows`, `join_build_max_rows`, `in_doubt_ceiling_ns`, `set_recovery`) | `Open` | runtime, at the runtime's defaults. The `Expeditor::Config` keys do not exist here; a cell that needs one sets it on `core(n).dispatcher()` |
| `ResumeAssertionsAfterRecovery` | `Open` | runtime, per core over `redo_start_lsn` of slot 0 |
| core 0's completion checkpoint (`checkpoint_target_`, `checkpoint_anchor_`, `CheckpointAfterRecovery`) | `Open` | not needed on core 0 (never remounted). The peer's runs at `AttachTransport` (runtime) and publishes its anchor to core 0 over the ring, where the rig **drops it** with a no-op `kAnchorWrite` handler - the same acknowledgement `core_runtime_test.cpp`'s rig gives |
| `set_relayout`, `optimizer_signals_`, the Cabin controller and executor, `checkpointer_`'s cadence | `Open` | not needed: advisory or cadence machinery nothing in §6 reads; SUS-1 leaves the planner dark on a fresh volume anyway |
| `Sync()` after mount | `Open` | rig (`store->Sync()` after bootstrap) |
| `EpollIoBackend` for core 0 | `Start` | runtime: every `CoreRuntime` opens its own, which is what `Start` says a reactor must have |
| TLS context, credential store, the three listeners, their `Attach`, the stop-signal fd | `Start` | not needed: no wire. A cell that wants one calls `core(n).ListenAndAttach`, production's peer listener, on either core |
| `live.scheduler`, `set_scheduler_view`, `ClearReactorBorrows` | `Start` | runtime (`Open` sets the view, `~CoreRuntime` withdraws every borrow) |
| `InstallSuspendAudit(store)` | `Start` | runtime, in `Run()`, with the shared-store arm (pin half off on a borrowed pool) |
| `RealRingTransport::Create(cores, kCoreRingSlots, kCoreRingPayloadBytes)` | `Start` | rig, same three numbers |
| `wakers_.emplace(cores)`; `transport_->AttachWakers(&*wakers_)` | `Start` | rig, **through the sim**: the real table is built, `SimWakerTable` wraps it, and the transport is attached to the sim - so a send's kick is logged and held like any other. This is H4's whole answer: the seam is `WakeRegistry` at the two attach points, which AV-R1's mark obliges anyway, and nothing in `Expeditor` opens for it |
| `scheduler.AttachWakerTable(&*wakers_, 0)`; `AttachTransport(&*transport_, 0)` | `Start` | rig, via `CoreRuntime::AttachTransport` and `scheduler().AttachWakerTable(&sim, id)` on both cores |
| `instance_stop_` and each peer's `set_instance_stop` | `Start` | not needed: no `STOP` reaches the rig; `TwoCoreRig::Stop()` is flag-then-kick through the **real** table, `BroadcastShutdown`'s shape, so a teardown never waits on the cell to advance the sim's tick |
| the `kAnchorWrite` handler writing through `SuperBlockCheckpointAnchor` | `Start` | rig, as a drop (above) |
| the peer loop's `CoreRuntime::Config` (checkpoint and drain intervals, widths, durability, isolation, budget, in-doubt ceiling, range size, access statistics, Cabin limits, `shared_store`, `buffer_pool_frames = 0`, slot-0 anchor, superblock, stream, writer, visibility) | `Start` | rig, for **both** cores, the config-derived fields at their runtime defaults and `wal_drain_interval_ns` from `Options` (off by default, so an idle reactor's block is the whole block). **The second runtime change**: `Config::scheduler`, a `SchedulerConfig`, so the rig can lengthen `max_idle_block_ms` from 10 ms to something a cell can distinguish from a wake - `scheduler_test.cpp`'s wake cells do the same on a bare `Scheduler`, and a `CoreRuntime` built its reactor with the default and no way to say otherwise. `Expeditor` leaves the field default-constructed |
| `ListenAndAttach` on peers under `peer_listeners` | `Start` | not needed (above) |
| `MakeStepSend`, `remote_reads_`, `remote_steps_`, `WireStepEndpoints`, `SetRemoteReads` on core 0 | `Start` | runtime: `AttachTransport` wires the step server and client on every core |
| core 0's `IndexBuildClient` and `AssertionBuildClient` | `Start` | **not reproduced**: `AttachTransport` wires the *servers* on peers only and no client on core 0, so a foreign `CREATE INDEX`/`CREATE ASSERTION` through core 0's dispatcher is refused in this rig. No §6 cell issues one; a cell that does wires the two clients as `core_runtime_test.cpp`'s `ForeignIndexRig` does, or lands them in `AttachTransport` for core 0 |
| statement shipping (both halves), 2PC (both halves, with the FK intent release on the decide), the foreign-key probe (both halves) on core 0 | `Start` | runtime: `AttachTransport` wires all three on every core, including 0 |
| `RegisterRowIdGrantHandler`, `RegisterTrxIdGrantHandler` | `Start` | rig, production's two functions over core 0's runtime catalog and sequence (`CoreRuntime::trx_ids()`, added for this). **And the peer's first block is carved by hand** from that same sequence, so a peer can write before its tick has asked - the tick is off by default |
| `RegisterAccessStatsBatchHandler`, `SetAccessStatsApplied` | `Start` | not reproduced: a peer's access-stat batches to core 0 are dropped. Advisory (CR7), and no §6 cell reads `sys.access_stats` |
| `catalog.SetInvalidationHook(BroadcastCatalogInvalidation)` | `Start` | rig: flush `kEveryCatalogPage`, then a `kCatalogInvalidate` to core 1 through a send-retry task on core 0's reactor - `BroadcastCatalogInvalidation`'s order, on the thread every DDL runs on |
| the worker spawn and `PinToCore` | `Start` | rig, without pinning. Two threads on an eight-CPU host need no affinity for anything §6 asserts |
| `SetPostTaskHook(drain)` and the three `SubmitEvery` cadences on core 0 | `RunUntilStopped` / `Run` | runtime, on each core's own **attached** manager. **The one open question this read leaves**: `Expeditor`'s core 0 drains the *owning* manager, and nothing in the rig calls the owner's `DrainOnce`. An attached drain flushes and asks the writer (`wal/manager.hpp`'s `RequestSyncNow`), which is production's peer path and is what a peer's commit already relies on; whether a commit on core 0 through an attached manager also becomes durable without the owner's drain is what AU-S2's committing cells will show, and this row is corrected there if it does not |

**H1 — yes.** The rig is the fixture `core_runtime_test.cpp` already
proved, with core 0 a runtime instead of a hand-built scheduler; nothing
`Expeditor::Open` wires is unreachable from outside it. **H2 — no, and the
rig does not need it**: the idle block is an `EpollIoBackend` wait, so a
held kick ends it only when the cell advances the tick and the forward
writes the eventfd, which is what `SimWakerTable` does. Determinism is
over the kick log and the latency in ticks (AV-R3), and the three cells
assert on counters, never on how long a kernel slept. **H3 — untested
here**: no §6 cell was written past cell 3 in this stage. **H4 — no seam
in `Expeditor`**: the production code the rig needed is the `WakeRegistry`
interface (the mark's own obligation), `CoreRuntime::Config::scheduler`,
`CoreRuntime::trx_ids()`, and core 0's persist arm - each a line with a
reason at its site, none a hook only a test calls into `Expeditor`.

## 9. Row status

| row | status |
|---|---|
| AV-S0 | **Done 2026-09-07** on `AU+AV-S0S1` from `8fa8e0a`: §8's table, and AU-R3 rewritten in `workorder-au-ring-retirement.md` (its AU-S2 gate, AU-5 and the AO-S5 row swept from `SimWaker` to `SimWakerTable` with it) |
| AV-S1 | **Built 2026-09-07** on `AU+AV-S0S1` from `8fa8e0a`, on AU-S1c built the same day. `tests/two_core_rig.hpp` (`TwoCoreRig`) and the three cells of §6 in `tests/two_core_rig_test.cpp`. **What the first run of the cells found**: the log holds the engine's own kicks beside the cell's - the peer's completion checkpoint publishes its anchor over the ring the moment its reactor starts, and that send kicks core 0 - so a cell reads the log per destination and asserts on core 1's own `wakes_received()`, never on the table's totals. Cell 2 reproduces the lost kick from the receiver's side (the sim holds it and the cell never advances) and says so; the publisher-side skip is `WakerTableTest`'s. The three cells and AU-S1c's four repeated ten times plain without a failure. **The `critics-developer` pass found six things, and CLA took five**: (C1) the sim test's parked reactor joined a five-second block at teardown instead of kicking it, ten seconds a suite - it kicks through the real table now, which is the pattern it cited; (C2) the determinism sentence was false for the rig - a shared RNG stream let one thread's kick shift every later kick's delay - so the draw is stateless over `(seed, dst, tick)` and `sched.md` §8 says what the seed fixes and what the interleaving owns; (C3) cell 2's headline assertion was a `>=` on a counter that only ever increases, and its twin repeated a line above it - now a bounded wait for a strict move; (C4) core 0's persist arm blanket-encoded a copy taken at `Open` over page 0, sound only while nothing else writes the page - now a read-modify-write of `next_trx_id` onto what the page holds, and its comment no longer claims to be `Expeditor::PersistSuperBlock`, whose `Sync()` also syncs the log; (C5) `forwarded()` counted a kick before it was forwarded - atomic now, counted after; (C6) `wakes_received()` is read cross-thread by a cell whose preamble claimed only admitted accessors - `scheduler.hpp` names three now, since the read is an atomic under a `waker_` engaged before any worker. Of the cuts: the transport is held by value rather than as an `optional<StatusOr>` and four accessors nobody called are gone; `FundPeerRelation`, `placement` and the drain interval stay for AU-S2 and AO-S5, which are the next commits; the re-told AU-R3 history left `sim_waker_table.hpp`; the `deque` is a `vector`. **Rejected**: factoring the volume setup out of `core_runtime_test.cpp`'s fixture (it reaches a 4,000-line file this stage has no other reason to touch, and the review's own words were "when someone is already inside it"). Noted and not changed: cell 1's `EXPECT_FALSE(done)` before the advance is racy in principle and microseconds against a five-second block in practice. After the fixes the seven cells passed five more times. Suites: plain 3306/3306 before the review's fixes; plain, armed and armed-with-budget-8 after them are in the landing commit's message |
| AV-S2 | **Built 2026-09-07** on `AU+AV-S0S1` from `8fa8e0a`, in the same commit as AV-S1: the hook cell `ARingFetchWaitsForAPageAnotherCoreHoldsExclusive` left `tests/am_s2_pin_protocol_test.cpp`, and its claim lives in the rig's cell 3 with a real core-1 holder. **The mutation ran**: with the hit path's shared latch dropped (`PinResidentAndRelease`'s `AcquirePageLatch` guarded on `mode != kShared` - a superset of "the ring takes no latch", since the ring's fetch of a resident page is that path's only shared caller in the cell) the cell fails at its named assertion, "the ring read a page another core held exclusive", within 310 ms; restored, it passes beside the four remaining pin cells. `LatchFrameForTest` stays: `page_latch_test.cpp` still takes the sweep's foreign hold through it. **Corrected 2026-09-07, after AU-S2's suites**: the budget-8 suite hung on this cell for 29 minutes, and the hang was the cell's, not the latch's. Core 1's fetcher parks on `held`, a flag core 0 sets with nothing kicking for it; under a frame budget core 0's `Get` faults slowly enough that core 1 has blocked by the time the flag flips, a five-second sleep against a two-second wait - so the wait failed, and the `ASSERT` left the cell without setting `release`, leaving core 0's holder spinning on it and core 1 spinning in `PageLatch::Acquire` behind the hold, with `Stop()` joining both forever. Both barriers kick now (`KickUntil`, the rule AU-S2 wrote into the rig header) and the holder is released by a scope guard on every exit. 20/20 under `KDS_TEST_PAGE_LATCH=1 KDS_TEST_FRAME_BUDGET=8` after |
| AV-S3 | **Hosting, cell by cell.** Cells 4, 6 and 7 are hosted since AU-S2 (2026-09-07, `tests/lock_wake_rig_test.cpp`), at the lock table's level - the waiter on core 1 proceeds at the kick with `sched_wakes_received` moving; with every kick delayed to the sim's maximum the waiter still proceeds - none of them through `LatchFrameForTest`. **Cell 5 is hosted since AO-S4b** (`tests/deadlock_rig_test.cpp`, the same day): a two-core cycle through shipped statements, the closer refused on core 1 naming deadlock, the refusal on the reply to core 0's coordinator in both the rendered line and the carried status - and that cell found the carried status was never set on a shipped refusal, which is `c168acb`'s class one seam over. **Cell 8 is restated as absent** (AO-S5(b)'s section, 2026-09-07): the refusal it named no longer exists, what replaced it is the page latch and the shared pool, and `device_page_store_test.cpp` pins what `MayWrite` still refuses - so there is nothing for the rig to host. What the hosting taught the rig: a cross-core barrier a cell builds for itself must kick until its condition holds, never once, because a single kick is best-effort by the registry's contract; and a fixture that owns coroutines must stop the rig before those coroutines' state dies |
