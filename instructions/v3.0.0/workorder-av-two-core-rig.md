# Work order AV — the deterministic two-core rig

Drafted 2026-09-07 by CLA against `origin/main` at `c9609c0`, as
`workorder-aw-m1-close.md`'s AW-S6. Letter **AV** per AR0-6 D26, which
rules the rig order is *not* `AS` — SUS-1 holds that letter — and proposes
this one. Every `path:line` is `[source-read]` at `c9609c0`; the rest is
`[design]`. **Nothing here is built**, which is AW-S6's own condition: the
stage's deliverable is this document.

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
| AV-S0 | **The read, and AV-R1's decision.** What `Expeditor::Open` wires that a rig must reproduce; whether the waker table can be injected without a production seam (H4); which of AV-R1's four seams is taken, and AU-R3's sentence rewritten to match | a table appended here: every wiring step, reproduced-by-the-rig or not-needed, with the reason | S | — |
| AV-S1 | **The rig itself**: two `CoreRuntime`s, one store, one stream, one visibility, one waker table; the seam from AV-R1 | the three cells §6 lists as the rig's own | M | AV-S0, AU-S1c |
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
than restating it on AO's behalf.

## 7. What this order does not do

- It does not build anything. AW-S6's condition is the document.
- It does not decide AV-R1. Four seams are set out with a proposal; the
  choice rewrites AU-R3's sentence and belongs to the operator or to
  AV-S0's read.
- It does not build `SimWaker`. That is AU-S1c's, and AV-S1 gates on it.
- It does not widen to N cores. H3 is the claim that two is enough for
  every cell named in §6.
- It does not touch AU-S2's or AO-S5's own content. §6 lists their cells so
  the rig can be checked against them; restating them is theirs.
