# Work order AM-S2 step 3 — Sharing the store: the scan ring pins what it hands out

Written 2026-09-05 by CLA against `103a6b8` on `worktree-ar2-borrow-model-2`,
under `workorder-am-m1-shared-pool.md` (AM). This is not a new letter: it is
the ruling AM-S2's step-3 row said had to exist before the pool is shared
(`src/storage/device_page_store.cpp:703-712`, "neither is decided"), together
with the three review items that row left open (R3, R6, R8), written as one
sub-order because step 3 cannot land without all four. Every `path:line` is
`[source-read]` at `103a6b8`; the rest is `[design]`. Nothing is `[measured]`,
and nothing here may be tagged so while the interleaved A/B is suspended.

**Operator's mark, 2026-09-05 (verbal):** the pending decision list follows
CLA's proposals. `raft-marks-2026-09-05.md` records the mark item by item;
this order records what the mark means for AM-S2 step 3 and builds on it.

**Status: S3a and S3b built 2026-09-05 on `worktree-am-s2` from
`ce4d27d`.** §8 records what landed, what the `critics-developer` pass
changed, and the three places the build differs from what §2 and §5 wrote.
S3c is not started.

---

## 1. Background

### 1.1 The one place a reader holds bytes with nothing keeping the frame alive

AM-S2 steps 1, 2a, 2b closed every accessor that could hand a `PageRef` a
`data_` into a freed `Page`: `FetchPinned` (`device_page_store.cpp:1466-`)
and `CreatePinned` take the pin under the structure latch and wait for the
page latch after it, and `InsertFrame`, `ReleaseScanSlot`, and the two
sweeps all reach `frames_` under the same latch. What is left is
`DevicePageStore::ScanRing::Fetch` (`device_page_store.cpp:692-741`), and
it is left because it is not an accessor defect: it is a *model* whose
premise stops being true the moment a second thread can evict.

The model is *no pin, drop on rotation* (`eviction.md` §5 / EV6). Both
arms of `Fetch` return a span into a frame with `pins == 0`:

- **hit arm** (`:715-717`): `frames_.find` outside any latch, span
  returned whether the frame is the foreground's or a ring slot's;
- **miss arm** (`:721-727`): `ReleaseScanSlot(slots_[hand_])`, then
  `ResidentBytes(page_id, mark_dirty=false, bump_usage=false)`, span
  returned, slot recorded.

The contract the consumer relies on is *valid until the next `Fetch`*
(`page_store.hpp:63-69`; `storage/heap/heap_chain.hpp:221`). Today that
contract is discharged by one fact — one store, one thread — and by nothing
in the code. Under a shared store `EvictClean` (`:1322`),
`EvictColdFrames` (`:1638`) and a peer ring's `ReleaseScanSlot` (`:666`)
each see a clean, unpinned, usage-0 frame and erase it, correctly by their
own rule (EV4: `pins > 0` is the only absolute), while the scan reader is
inside the span. `live_pins()` and `pinned_frames()` balance perfectly
throughout. That is the same quiet-failure class the 2a review found in
`Get` (AM-6, H1), and it is the last instance of it in the store.

A latch inside `Fetch` does not help, as the site's comment says: the
exposure begins when the latch would drop and lasts as long as the caller
holds the span.

### 1.2 Who consumes the ring

| caller | why it rings | state under SUS-1 |
|---|---|---|
| `src/exec/cabin_optimizer_exec.cpp:113` | PO4, structurally: the Cabin build's walk must not displace the foreground working set (`cabin_optimizer_exec.hpp:71`) | **live** — "a btree leaf is a heap page, so one loop serves both clustered forms" (`:115-116`) |
| `src/stats/relayout_planner.cpp:255` | EVT06: a census of a relation larger than memory must not flood the pool it surveys | dark for every new relation (AS-Q5); mounts and runs for pre-SUS-1 heap relations |

So the decision is, in effect, the Cabin build's. It is the automatic
optimizer path (SB3 admitted `CREATE CABIN` on split relations, the
automatic path included), which is why "refuse the ring" is not a corner.

### 1.3 The three review items step 3 inherits

From AM-6's AM-S2 row (2a/2b review, "reported and open"):

- **R3** — armed and unarmed disagree on CLOCK usage after a fault: the
  armed loop's re-check from the top runs the raw fetch twice on a wake,
  and the raw fetch bumps usage on the hit branch.
- **R6** — the hit path's `loading_` test (`:1521-1522`) is load-bearing
  and untested, because `am_s2_pin_protocol_test.cpp`'s fixture leaves
  `frame_budget_` at 0 and the inline sweep never runs.
- **R8** — no `PageDevice` declares itself thread-safe, and 2b makes
  concurrent reads of different pages possible for the first time. Partly
  moved by `589f169`: `MemoryPageDevice` now latches its mutating surface,
  and the finding was corrected to "existing defect surfaced" — but the
  *declaration* the row asked for is still absent, and the file-backed
  device has not been read.

---

## 2. Conclusions — rulings, marked

**AM-R8 — The scan ring pins the one page it has handed out, and nothing
else. [spec change, marked 2026-09-05]** `Fetch` takes a shared pin on
the page it returns and drops it on the next `Fetch` (before rotation) or
in the destructor. The other `frames - 1` slots stay at `pins == 0`, as
today, and any core may evict them; no outstanding span points into them,
by the contract. Every other §5 property survives unchanged: no usage
bump, drop on rotation, abandon to the pool when the foreground claimed
the frame, `kds.scan_ring_frames` untouched.

Why this and not the other two:

1. **It is the contract the tree already ships.** `PlainScanFetcher`
   (`page_store.hpp:437-450`) holds the previous fetch's `PageRef` and
   releases it on the next `Fetch`, and its comment calls that the *exact*
   implementation of the lifetime contract. AM-R8 makes `ScanRing` say the
   same thing with a pin instead of a thread.
2. **"The pin the ring exists to avoid" was an overstatement, and CLA
   made it.** EV6's stated purpose is to bypass the retention insert and
   the usage bump — displacement. A pin is not on that list. Under arming
   the fetch already holds the structure latch, so the pin is a counter
   increment inside a hold that exists anyway; at `cores = 1` it is two
   plain integer increments per page, no atomic (G1), against a device
   read.
3. **Refusing the ring (option B) is a second quiet-wrong.** It would
   route the Cabin build through `GetForRead`, which bumps usage and
   inserts for retention, at `cores > 1` only — the PO4 guarantee
   `cabin_optimizer_exec.hpp:71` calls structural would hold on a
   single-core instance and silently lapse on every other. Pinning all 32
   slots (option A) deletes drop-on-rotation and turns `ReleaseScanSlot`
   into a no-op, which is half the ring.

**AM-R8a — One pin path, armed or not.** The ring's fetch goes through the
same `loading_` protocol as `FetchPinned` (`:1504-`): structure latch, the
resident-and-not-loading test, `CountPin`, unlock, `AcquirePageLatch`
(shared). It is *not* a copy of that body with `bump_usage = false`
threaded through by hand — F1 was exactly that shape. `FetchPinned` gains
the one parameter the ring needs, the usage bump as a caller's choice with
today's default, and the ring calls it. This also closes the hit arm's
unlatched `frames_.find`, the half of R2 the third-eraser commit
(`cdc2d46`) left. `cores = 1` takes the unarmed branch of the same
function: fetch, then `PinFrame`, plain increments.

**AM-R8b — Unpin precedes release, always.** `ReleaseScanSlot` refuses a
frame with `pins > 0` (`:679`), so the rotation is `UnpinFrame(prev)` then
`ReleaseScanSlot(prev)`, and the destructor the same per slot. The
reverse order silently keeps every slot resident for the life of the
process — the ring would "work" and the pool would fill.

**AM-R8c — The shared page latch is the wait.** A foreground writer
holding the page `X` blocks the scan on that page, exactly as
`GetForRead` blocks. This is the page-latch family's ordinary rule (AR2
R2), not a new one, and it is the reason the pin is `kShared` rather than
a pin-without-latch: a pin alone would let the scan read a page mid-write
on another core.

**AM-R9 — R3 closes by making the wake path re-check without
re-fetching. [quiet-wrong, marked]** The armed loop's second raw fetch
after a wake is a second usage bump, so a page faulted under contention
warms twice. The re-check on wake tests residency and `loading_` only;
the raw fetch runs once per `FetchPinned` call on the branch that
succeeds. The cell is the disagreement itself: usage after one contended
fault equals usage after one uncontended fault, armed, and equals the
unarmed value.

**AM-R10 — R6 closes with a budgeted fixture. [cell, marked]** The
`loading_` half of the hit test cannot be proved by a functional cell
(`:1507-1520` says why: the race is a data race, not a wrong answer).
What *can* be covered is that the sweep runs at all under the cell:
`am_s2_pin_protocol_test.cpp` gains a variant with `frame_budget_` set
below the working set so the inline sweep executes, and asserts the
sweep's counter moved. The comment stays; the fixture stops being the
reason the comment is the only evidence.

**AM-R11 — R8 closes with a declaration, not a latch. [spec change,
marked]** `PageDevice` (`include/kds/storage/page_device.hpp`) states its
threading contract in one sentence: concurrent `ReadPage*` of *distinct*
pages from distinct threads is permitted; concurrent access to the same
page, and any `WritePage*` concurrent with anything, is the caller's to
serialize (the frame table does, under the page latch). `MemoryPageDevice`
already meets it (`589f169`). The file-backed device is read at this step
against that sentence — `pread`/`pwrite` at distinct offsets are
independent by the kernel's contract, so CLA expects it to meet it
without change, and says so as `[design]` until the read.

---

## 3. Hypotheses

| # | claim | how it fails if wrong |
|---|---|---|
| H1 | Pinning the current slot is sufficient: no span into an unpinned slot is ever outstanding | a consumer keeps a span across `Fetch` — the contract already forbids it; a grep of both callers confirms neither does |
| H2 | The pin's cost at `cores = 1` is below the A/B's noise floor | unmeasurable while the A/B is suspended; recorded as `[design]`, and AM-S6 is where it would show if it shows |
| H3 | `ReleaseScanSlot`'s `usage > 0` abandon rule still discriminates once the ring pins | a pinned fetch that bumped usage would make every ring slot look foreground-claimed; AM-R8a's no-bump is what keeps the rule meaningful, and the cell checks usage stays 0 on ring slots |
| H4 | The file-backed device needs no change under AM-R11 | a shared `fd` offset (`read`/`lseek` rather than `pread`) — the source read decides |

---

## 4. Measurement

Nothing in this sub-order is `[measured]`. The interleaved A/B is
suspended by operator decision and AM-S6 is the milestone's measurement.
The cells below are correctness cells; a run that finishes is the
evidence where the property is structural, and every cell is
mutation-checked before it counts (F3).

---

## 5. Improvement — stages

Sizes: S ≤ ½ day, M ≤ 2 days. Every stage: `critics-developer` review,
the full suite plain **and** armed (`KDS_TEST_PAGE_LATCH=1`), sync with
`origin/main`, stop.

| # | Stage | Cells (definition of done) | Size | Gate |
|---|---|---|---|---|
| S3a | **The ring pins** (AM-R8, R8a, R8b, R8c): `FetchPinned`'s usage parameter; `ScanRing::Fetch` through it; unpin-then-release on rotation and in the destructor; the site comment at `:703-712` rewritten to say what is decided | (1) two threads, one armed store, a start barrier: the scan reader holds a span while the other thread forces `EvictColdFrames` over the ring's page — the frame survives, `pins == 1`; **mutation**: with the pin compiled out the frame is erased and the cell fails (an ASan-visible freed read is acceptable evidence; a green run with the pin removed is not). (2) rotation drops the previous slot: after `frames + 1` fetches the first page is gone and the last is pinned. (3) ring slots report `usage == 0` throughout (H3). (4) a page held `X` by a second thread blocks `Fetch` until release (AM-R8c), bounded. (5) `cores = 1`: `live_pins()` reads 1 during a ring scan and 0 after it — the ring now shows in the gauge, and `page_latch_test.cpp`'s ring assertions are re-read against that | M | — |
| S3b | **R3, R6, R8 closed** (AM-R9, R10, R11): the wake path's single fetch; the budgeted fixture; the `PageDevice` sentence and the file-backed device's source read | usage-equality cell (AM-R9); sweep-counter cell (AM-R10); the declaration with the read's finding recorded as `[source-read]` (AM-R11) | S | S3a |
| S3c | **The store is shared** — the original step 3 and 4 together (AM-6: "steps 3 and 4 cannot be split"): `CoreRuntime` holds a reference to one `DevicePageStore`; `SetLatchArmed(core_count)` at construction; `MayFault` removed (AM-R2); `buffer_pool_frames` an undivided total; the `frames < cores` boot refusal goes | AM-S2's original cell: a page faulted on core 1 is served from the frame core 0 loaded; the census at `cores = 2` and `cores = 4` armed; the Cabin build at `cores = 2` walks a relation larger than the pool under the ring and the foreground's resident set is unchanged before and after (PO4, now a cell rather than a comment) | L | S3a, S3b |

Steps 5 and 6 of AM-S2 (the CLOCK hand and the budget as instance totals;
the boot refusal) fold into S3c or stand as the row says once S3c is read.

## 6. What this order does not do

- It does not measure. H2 is the one claim a measurement would settle,
  and it waits for AM-S6.
- It does not touch `kds.scan_ring_frames` or EV6's numbers.
- It does not change either consumer. If S3a's grep (H1) finds a span
  held across `Fetch`, that is a consumer defect filed in `bugs/`, not a
  reason to widen the pin.

## 7. Prose owed

`eviction.md` §5 gains one sentence under the first bullet: *the ring
holds a shared pin, and the page latch, on exactly the page its last
`Fetch` returned; every other slot is an ordinary evictable frame.* The
"per-core" in "a small per-core ring" stays true — a ring is per scan, and
a scan is on one core — but AM-S5's pass should re-read it against a
shared pool. `page_store.hpp:63-69`'s contract text is unchanged; it was
right and is now implemented.

## 8. S3a and S3b as built — 2026-09-05, `worktree-am-s2` from `ce4d27d`

**H1 verified by source read, not assumed.** Both ring consumers finish a
page before fetching the next: the Cabin build stages every row out of the
page inside one scope and its spill fetches run after
(`cabin_optimizer_exec.cpp:163-199`), and the relayout survey walks through
`ChainVisitOnePage`, which holds the ring's span for exactly one page's slot
loop (`heap_chain.cpp:278-325`). No consumer defect to file.

**H4 verified by source read.** `FilePageDevice` needed no change: every
transfer is a `pread`/`pwrite` at an offset computed from the page id
(`file_page_device.cpp:97`, `:129`), never `lseek` plus `read`, and the
loop's `offset`, `buffer` and `remaining` are locals. Its *own* header's
"Concurrency: core-local, like every PageDevice" was the thing that was
wrong, and it had been wrong since one device began serving every core's
store; corrected, and `page_device.hpp`'s AM-R11 sentence now cites the
read rather than asserting the conclusion.

### 8.1 Three departures from what §2 and §5 wrote

1. **AM-R9's letter is "the raw fetch runs once per call"; what shipped
   suppresses the second fetch's *charge* instead.** The loader still calls
   `Resolve` a second time on the hit branch it rounds into, with
   `bump_usage` false. The reason is the branch that made the letter unsafe:
   between this call's load and its re-check another core can evict and
   re-fault the page, so the frame the call returns need not be the one it
   loaded, and skipping the second fetch would skip that frame's dirty mark
   on a `Get`. That is a lost write, a worse defect than the one AM-R9
   closes.
2. **`FetchPinned` gained one parameter as §2 said; `FetchAndPin` beside it
   gained a second.** The first draft told an in-place hit from a fault with
   a residency probe of the ring's own (`ResidentNow`, under the structure
   latch), which forced §5's bound to be weakened to "the ring's size, plus
   what a concurrent eviction made it miss" — the probe is stale before the
   fetch acts on it. The review's largest finding was that the fetch already
   knows: `charged` **is** "this call ran `InsertFrame`". So the private
   `FetchAndPin` carries a `bool* faulted`, `FetchPinned` is a four-argument
   forwarder that passes null, and the probe, its eight-line header block,
   one latch round-trip per page and the spec weakening all go. The trade,
   taken deliberately: the slot release now follows the fault instead of
   preceding it, so residency peaks at `frames + 1` for the length of one
   `Fetch`. That is a change to §5's bound and is written into the spec as
   one.
3. **S3b latched the last two erasers, which §5's table assigns to S3c.**
   AM-R10's budgeted rig is the first cell in the tree ever to enter the
   fault path's inline sweep, and its first run was a `double free or
   corruption` abort: `EvictColdFrames` walks and erases `frames_` with no
   latch, and the hand-pin that guards the just-inserted frame was three
   unlatched reads of the same table beside it. `EvictClean` is the same
   shape. Both take the structure latch now, as `ReleaseScanSlot` did at
   `cdc2d46` — whose comment already read "like the other two **will**".
   The cell could not otherwise run, so this is not scope taken, it is
   scope the cell forced.

### 8.2 What S3a closes that it was not written to close

**R2.** A ring fetch reached `ResidentBytes` directly, outside the
`loading_` set, so `InsertFrame`'s `insert_or_assign` could replace a
`Frame` — latch word and pin count with it — under a concurrent loader.
Routing the ring through `FetchPinned` (AM-R8a) removes the only caller that
could. It also puts ring fetches behind `Resolve`'s allocation gate for the
first time.

**A page-against-page pair nobody had listed.** The ring drops its hold at
the *next* `Fetch`, so the Cabin build's phase-2 spill fetches run inside
it: `S(heap leaf)` then `S(var-heap page)`. Shares never block shares, so
nothing waits today, and owner routing puts the writer of both pages on the
ring's own core — but the pair is on `device_page_store.hpp`'s order list
now, because S3c is exactly when that list has to be complete. A
`ScanFetcher::Release()` for the consumer to call before phase 2 was
considered and declined: it widens a base-class interface for one consumer's
shape, and §6 of this order says the consumers are not changed.

### 8.3 Cells — six mutants, six kills

| mutation | cells that failed |
|---|---|
| the ring's pin dropped immediately after `FetchPinned` | `ARingSpanSurvivesAConcurrentSweepBecauseTheRingPinsIt`, `TheRingHoldsExactlyOnePinAndItTravelsWithTheFetch` |
| the destructor releases slots before it unpins (AM-R8b reversed) | `TheRingHoldsExactlyOnePinAndItTravelsWithTheFetch`, `RingFetchesNeverBumpUsage` |
| the ring fetch passes `bump_usage = true` | `ARingBoundsAScansResidencyAndSparesTheWorkingSet`, `RingFetchesNeverBumpUsage`, `RotationAbandonsASlotTheForegroundTouchedAndDropsAnUntouchedOne` |
| `AcquirePageLatch` dropped from the hit path | `ARingFetchWaitsForAPageAnotherCoreHoldsExclusive` |
| `bump_usage && !charged` → `bump_usage` (AM-R9 undone) | `AFaultChargesTheClockCounterOnceArmedAsUnarmed` |
| the inline sweep block never runs | `ConcurrentFaultsPastTheBudgetRunTheInlineSweep` |

`EvictColdFrames`'s structure latch removed is a seventh: the budgeted cell
segfaults. That is the mutation that found the defect in the first place,
recorded as evidence rather than run afterwards for the table.

**And one existing cell discriminated nothing.** `RingFetchesNeverBumpUsage`
ended the scan and asserted `EvictColdFrames` reclaimed the page in one
pass — but that sweep makes `size × (kClockUsageCap + 1)` steps in a single
call, so it reclaims the frame whether or not a fetch bumped it. Rewritten
to read the counter where it is read directly: `ReleaseScanSlot` drops a
slot at usage 0 and abandons it above, so the ring's own destructor is the
discriminator, and a device-read count in the same cell pins "in place when
resident" against the trace rather than against inference.

**AM-R8c's cell needs the test hook, and that is a fact about the word.**
Two threads of one store are one *core* to `PageLatch`, which admits a
shared acquire under its own core's exclusive hold (`page_latch.hpp`), so
the foreign hold is taken with `LatchFrameForTest(..., core=7)` — the same
shape `page_latch_test.cpp` uses for the sweep's refusal. A cell with two
genuine cores is S3c's.

### 8.4 What the `critics-developer` pass changed

Every finding was taken; none was rejected. The three that changed code
rather than prose: the `faulted` out-parameter above (its S1), the fatal
`ASSERT_TRUE` taken while a thread is parked forever in `PageLatch::Acquire`
— which would `std::terminate` the binary instead of failing one cell — and
the device-read assertion that pins §5's in-place rule. Three comments the
change had falsified were fixed in the pass itself: `InsertFrame`'s claim
that the `insert_or_assign` clobber "is still reachable by a ring fetch
racing a load", the header's list of sites that "still touch frames
unlatched", and `heap_chain.cpp`'s "the ring branch holds no pin at all".
Two more were falsified by S3b and fixed here: `PinFrame`'s "written in the
voice of the shared future" paragraph, whose counterexample — all three
erasers reading `pins` unlatched — is what S3b removed, and the `loading_`
test's own note that no cell in the file ever entered the inline sweep.

The pass also measured two properties as *uncovered* by the full armed
suite before the fixes: AM-R9's charge-once and the ring's in-place rule.
Both have cells now. Until they did, "the armed suite is green" was not
evidence for either, and a row that said so would have been overstating.

### 8.5 Suite

**3354/3354 plain and 3354/3354 armed** (`KDS_TEST_PAGE_LATCH=1`, plus the
two by-design skips and one disabled cell). The threaded budgeted cell was
run eight times rather than once — one green run of a concurrency cell
proves nothing. **Overhead not measured**: the interleaved A/B is suspended
by operator decision, and AM-S6 is where H2 would show if it shows.
