# Work order AM-S2-P — Porting `c7c3a67`'s scan ring onto `main`

Written 2026-09-06 by CLA against `origin/main` at `16e6c5c` and
`origin/worktree-am-s2` at `272a46e` (= `c7c3a67` over `5080615`; the merge
commit adds nothing from main past `5080615`). Under
`workorder-am-m1-shared-pool.md` (AM); no new letter. Supersedes both
`workorder-am-s2-step3-scan-ring.md` (2026-09-05) and
`workorder-am-s2-merge.md` (2026-09-06, S-M0..S-M5), which were written
before `c7c3a67` was read. Every `path:line` is `[source-read]` at the
named commit.

**Operator's decision, 2026-09-06 (verbal):** the two sessions' work is
combined; the better code wins per item. This order records the per-item
verdict (§2) and the port that follows from it (§5).

**Status: S-P0, S-P1 and S-P2 done 2026-09-06 on `worktree-am-s2-port`
from `16e6c5c`.** §7 is S-P0's table and §8 is what S-P1 and S-P2 built.
Between them they move four of this order's own rulings: `main` closed R3
and the 2a/2b R6 with better mechanisms than `c7c3a67`'s (§7 rows 6-9),
and F-1 — the defect S-P0 filed against `main`'s R3 fix and S-P1 was to
close — turned out to be closed by AM-R8a itself, so the fix CLA built for
it was reverted (§8.2). S-P3 and S-P4 are not started.

---

## 1. Background

### 1.1 What is being compared

Not two branches. `main` carries AM-S2 step 3 in full (3a..3g, `2663001`,
`dd0bfe9`), AM-S3, AM-S4, AM-S4a, AU-S1b and `d8574e0`; `worktree-am-s2`
carries one commit, `c7c3a67`, on top of `5080615`. **`main` is the base**
and this order does not revisit that. What `c7c3a67` has that `main` lacks
is a different scan ring, the `PageDevice` threading declaration, four
cells, and a corrected `eviction.md` §5 — and on every axis where the two
rings differ, `c7c3a67`'s is the better code.

### 1.2 The two rings, axis by axis

| axis | `main` (`a21fe3f`, `device_page_store.cpp:721-748`, `:828-860`) | `c7c3a67` (`:720-800`) |
|---|---|---|
| page latch on the handed-out page | none | shared, via `FetchAndPin(kShared)` |
| fault path | `ResidentBytes` direct, then re-find under the latch, 8 attempts, `ResourceExhausted` | the `loading_` protocol every other accessor takes |
| pins per scanning core | one per occupied slot, up to `kScanRingFrames` = 32 | 1 |
| resident fast path | linear walk of all slots under the structure latch, per fetch | `FetchAndPin`'s hit branch |
| rotation | on every fetch not already in a slot — a foreground-resident page consumes a slot and takes the ring's pin | only when the fetch *faulted* (`faulted` out-parameter); residency bound `frames + 1`, exact |
| cells | two existing cells edited | four new, six mutants killed; found `RingFetchesNeverBumpUsage` discriminated nothing |

### 1.3 Why the latch is a correctness item and not a preference

`main`'s ring reads a page while another core may hold it `X`. At
`cores = 1` nothing ever wrote concurrently, so the ring's "unlatched
bytes are the scan's model" was never tested against a writer; at
`cores > 1` the Cabin build computes statistics from a page mid-write —
the quiet-wrong class this milestone exists to close. `a21fe3f`'s reason
for omitting the latch ("would deadlock against the foreground") is
answered by two facts already in `main`:

- `UnpinFrame` (`:2203-2221`) releases the page latch with the pin, so
  every `PageRef` already holds a page latch across arbitrary caller code
  and `PlainScanFetcher` holds one across exactly the ring's span.
- `heap/heap_chain.hpp:220-223` already forbids passing a fetcher to a
  caller that suspends between pages. The latch-across-park hazard the
  argument reached for is closed by a contract older than both sessions.

### 1.4 Why the fault path is a correctness item

`PinForScan`'s miss branch faults outside `loading_`. Two cores faulting
one page issue two device reads and the second `InsertFrame` loses — the
race 2b closed for every other accessor (`:1249`, `:1304`) — and the
8-attempt loop is the symptom dressed as a remedy. Under the `loading_`
protocol the pin is taken under the hold that made the frame resident,
and the loop has nothing left to retry.

### 1.5 The pin ceiling

`pin_ceiling_ = kPinCeiling × concurrent_pinners` (`device_page_store.hpp:718`),
armed at `core_count` (`core_runtime.cpp:252`) — `16` at `cores = 2`.
`main`'s ring can hold 32 pins on one store. `a21fe3f`'s comment (`:831`)
says the ceiling "has to admit N per scanning core"; the code does not
do it, and whether the debug abort at `:2094` is reachable with a
production-size ring is S-P0's question. Under `c7c3a67`'s ring the
question disappears: one pin per scan.

### 1.6 Two corrections `c7c3a67` made to CLA's 2026-09-05 rulings

- **AM-R9** is not "skip the second fetch on wake". Between the load and
  the re-check another core can evict and re-fault the page; skipping the
  fetch would skip that frame's dirty mark on a `Get` — a lost write. The
  fix is a suppressed *charge* (`charged` = "this call ran
  `InsertFrame`"), which also becomes the ring's `faulted` answer.
- **AM-R11** closed by source read: `FilePageDevice` transfers at
  `pread`/`pwrite` offsets with locals only; no change. Its header's
  "core-local, like every PageDevice" was the only defect.

**S-P0 moved the first of these.** `main` closed R3 at `dd833d6` by a
third mechanism neither ruling considered — pinning at the miss branch, so
the second fetch never runs — and that is better than a suppressed charge.
The hazard `c7c3a67` named is real and survives in `main`'s version; §7's
F-1 is that defect and S-P1 owns it. AM-R11 stands as written.

### 1.7 What `main` has that `c7c3a67` predates, and the port must respect

`FetchPinned` on `main` was refactored after the branch point:
`PinResidentAndRelease` (`device_page_store.hpp:1129`) is the shared pin
tail with `hold` held on entry. `c7c3a67`'s `FetchAndPin` was written
against the pre-refactor body. **The port re-derives `FetchAndPin` on
`main`'s shape** — the `faulted`/`charged` answer and the `bump_usage`
parameter added to the tail `main` has — rather than pasting the branch's
function over it. S-P1 is that derivation and nothing else.

---

## 2. Conclusions — rulings

**AM-R8 (final) — The ring holds one shared, page-latched pin on the page
its last `Fetch` returned; every other slot is an ordinary evictable
frame; a slot rotates only on a fault.** `c7c3a67`'s model, taken whole.
`main`'s `PinForScan` and its retry loop go. `a21fe3f`'s slot-pin model is
recorded in the AM-S2 row as built and replaced, with §1.3–1.5 as the
reason, so it is not proposed again.

**AM-R8a — One fetch protocol.** The ring fetches through the same
`loading_` path as every accessor, with the usage bump refused, and gets
its fault answer from that path. No second body.

**AM-R8b — Release order:** the ring's page latch and pin drop first
(`DropHeld`), then `ReleaseScanSlot`. `main`'s "the ring's own pin comes
off inside `ReleaseScanSlot`" block (`:761-769`) goes with the slot-pin
model; `ReleaseScanSlot` returns to refusing any pinned frame, which is
what makes "unpin first" load-bearing.

**AM-R8c — The consumer contract** (`page_store.hpp`, `heap_chain.cpp`,
`cabin_optimizer_exec.cpp` comments as in `c7c3a67`): a `ScanFetcher`
consumer finishes with the span before the next `Fetch` and does not
suspend while holding one. The rule is `heap_chain.hpp:220-223`'s,
restated at the ring.

**AM-R9 / AM-R10 / AM-R11** — as `c7c3a67` built them (§1.6). AM-R10's
budgeted fixture (`BudgetedPinProtocolTest`) is taken; `main`'s
`KDS_TEST_FRAME_BUDGET=8` run is the same rig by environment and the
fixture is what makes it a cell.

**AM-R9 / AM-R10 superseded, 2026-09-06 (S-P0).** Both are closed on
`main` by better mechanisms and neither is ported; §7 rows 6–9 carry the
verdicts and the one defect the read found. AM-R11 stands.

**AM-R13 — The pin ceiling is not widened for the ring.** With one pin
per scan, `pin_ceiling_ = kPinCeiling × core_count` stands unchanged. If
S-P0 finds `main` widened it in code, the widening is reverted here.
**S-P0: `main` did not widen it** — the comment claims a scaling the code
does not do (§7, F-2), and the port removes the need rather than the
comment's premise.

---

## 3. Hypotheses

| # | claim | how it fails if wrong | S-P0 verdict |
|---|---|---|---|
| H1 | `FetchAndPin` can be expressed as `main`'s `FetchPinned` + `PinResidentAndRelease` with two additions (`bump_usage`, `faulted`) and no duplicated arm | the tail needs the charge state the loop owns; then the tail takes it as a parameter, still one copy | **holds, and is simpler than expected** — `faulted` is not a charge state on `main`: the miss branch *is* the fault, so the branch sets it. The tail needs `mark_dirty` for F-1 |
| H2 | `main`'s ring cells in `eviction_test.cpp` either pass under the ported ring or were asserting the slot-pin model | a cell asserting `pinned_frames() == 3` for two slots plus a handle is the model, not the property; it is rewritten to `c7c3a67`'s form | **two of four are the model** (§7 rows 11–14); four cells, not three — the order missed `:705` |
| H3 | Nothing on `main` past `a21fe3f` depends on `PinForScan` or on slots being pinned | `grep PinForScan` on `main` returns one definition and one caller — the ring; S-P0 confirms | **holds**: `device_page_store.cpp:721`, `:855`, `device_page_store.hpp:1082`, nothing else |
| H4 | The page latch on a ring fetch is unmeasurable at `cores = 1` | `[design]` until AM-S6; the A/B is suspended | untouched |

Also confirmed for AM-R8c: `src/exec/cabin_optimizer_exec.cpp` contains no
`co_await`, `co_yield` or park of any kind, so no ring consumer suspends
inside its walk. PO6's budget at `:324` is in the apply path, not the
walk.

---

## 4. Measurement

None. Correctness cells only, each mutation-checked before it counts
(F3). `c7c3a67`'s six mutants are re-run on the ported code, not
inherited as a claim.

---

## 5. Improvement — stages

Branch `worktree-am-s2-port` from `origin/main` at `16e6c5c`.
`origin/worktree-am-s2` is a read-only reference and is deleted after
S-P4 lands; the AM-S2 row is the record of what was taken. No rebase of
`272a46e`: its one substantive commit is ported by hand against `main`'s
refactored `FetchPinned` (§1.7).

Every stage: `critics-developer` review; suite plain, armed
(`KDS_TEST_PAGE_LATCH=1`), and armed with `KDS_TEST_FRAME_BUDGET=8`; stop.

| # | Stage | Cells (definition of done) | Size |
|---|---|---|---|
| S-P0 | **Read.** Output: §7's table | S — **done** |
| S-P1 | **The fetch protocol** (AM-R8a, F-1): `bump_usage` and `faulted` on `main`'s `FetchPinned` shape; the dirty mark re-applied in the pin tail (F-1); `PinForScan` and its retry loop removed | F-1's cell: a `Get` whose frame is evicted and re-faulted clean between the load and the pin still leaves the frame dirty; **mutation**: drop the tail's mark and the write is lost. Two rings on two threads faulting one page with a start barrier issue **one** device read; **mutation**: bypass `loading_` on the ring's path and the count is 2 | M |
| S-P2 | **The ring** (AM-R8, R8b, R8c): `c7c3a67`'s `ScanRing` replaces `a21fe3f`'s; `ReleaseScanSlot`'s own-pin block removed; the contract comments in `page_store.hpp`, `heap_chain.cpp`, `cabin_optimizer_exec.cpp` | `ARingSpanSurvivesAConcurrentSweepBecauseTheRingPinsIt` and `ARingFetchWaitsForAPageAnotherCoreHoldsExclusive` ported; **mutations**: pin removed → the sweep erases the frame under the span; latch removed → `Fetch` returns while `X` is held. `main`'s four ring cells per §7 rows 11–14. `live_pins()` reads 1 during a scan and 0 after it. `ReleaseScanSlot` refuses a pinned frame again and a cell says so | M |
| S-P3 | **Declarations and the ceiling** (AM-R11, AM-R13): `page_device.hpp` and `file_page_device.hpp` as in `c7c3a67`; `a21fe3f`'s ceiling comment (F-2) corrected or removed with the slot-pin model | the declaration with the `[source-read]` finding | S |
| S-P4 | **Documents.** `eviction.md` §5 as `c7c3a67` wrote it (the AM-R8 paragraph and the corrected consumer line); the AM-S2 row: one entry naming `a21fe3f`'s model as built-and-replaced with §1.3–1.5 as the reason, and `c7c3a67` as the source of the port; `index.md` | no document under `instructions/v3.0.0/` describes a ring `main` does not have; `origin/worktree-am-s2` deleted after the push | S |

## 6. What this order does not do

- It does not touch AM-S3, AM-S4, AM-S4a, AU-S1b or `d8574e0`.
- It does not measure; H4 waits for AM-S6.
- It does not reopen one-pin-versus-N. AM-R8 is final; the record of the
  alternative is §1.2 and the AM-S2 row entry S-P4 writes.
- It does not assign sessions. One branch, worked in sequence, is the
  whole of the concurrency policy from here; if two sessions must run,
  the second one does not touch `device_page_store.*` or
  `instructions/v3.0.0/` until this order's S-P4 has landed.

---

## 7. S-P0 — the read, 2026-09-06 on `worktree-am-s2-port` from `16e6c5c`

Every `c7c3a67` hunk, classified. **Take** = ported as written or
re-derived on `main`'s shape. **Superseded** = `main` already has the
property by a different and better mechanism. **Drop** = neither, with
the reason.

| # | `c7c3a67` hunk | verdict | why |
|---|---|---|---|
| 1 | `ScanRing` — one page-latched pin, `DropHeld`, fault-only rotation | **take** | AM-R8 final; §1.3–1.5 |
| 2 | `FetchAndPin`'s `bump_usage` parameter, `Resolve` gaining it | **take, re-derived** | `main`'s `FetchPinned` still calls `GetUnpinned`/`GetForReadUnpinned`, which are `Resolve(id, mark_dirty)`; the parameter goes on `Resolve` exactly as on the branch |
| 3 | `FetchAndPin`'s `faulted` out-parameter | **take, simplified** | on `main` the miss branch *is* the fault, so `faulted` is set there and cleared on the hit branch — no `charged` needed to carry it |
| 4 | `PinForScan` + its 8-attempt loop + `ResourceExhausted` | **removed** (from `main`) | AM-R8a/§1.4; H3 confirms one caller |
| 5 | `ReleaseScanSlot`'s own-pin block (`main`'s `:761-769`) | **removed** (from `main`) | goes with the slot-pin model; AM-R8b |
| 6 | `charged` (AM-R9's suppressed charge) | **superseded** | `main` closed R3 at `dd833d6` by pinning where the load finished (`device_page_store.cpp:1932`), so the second `Resolve` never runs at all. Strictly better than suppressing its charge |
| 7 | `frame_usage_for_test` + `AFaultChargesTheClockCounterOnceArmedAsUnarmed` | **superseded** | `main`'s `SweepVictimAfterAFault(bool armed)` equivalence rig (`eviction_test.cpp:789`) reads the counter through the sweep's victim choice instead of adding an accessor, and asserts the two arms genuinely differ |
| 8 | `inline_sweeps` counter + `BudgetedPinProtocolTest` | **superseded** | `main` has `EvictionInsertSweepTest.AFaultPastTheBudgetSweepsUnderTheInsertsOwnHold` — a budget and more distinct pages than it holds, which is AM-R10's rig, plus `KDS_TEST_FRAME_BUDGET` as a suite-wide arm |
| 9 | the two erasers taking the structure latch | **superseded** | `main` moved MG06's sweep *inside* `InsertFrame`'s own hold (`dd833d6`), which removes the unlatched window rather than covering it, and split `EvictColdFrames` into a public door and a `Locked` body so the in-hold call cannot self-deadlock |
| 10 | `page_device.hpp` citation + `file_page_device.hpp` correction (AM-R11) | **take** | `main`'s `file_page_device.hpp:40` still reads "Concurrency: core-local, like every PageDevice", untrue since one device began serving every core's store |
| 11 | `eviction_test.cpp` `ARingBoundsAScansResidencyAndSparesTheWorkingSet` | **keep `main`'s** | a property, not a model: residency ≤ before + 4 holds under one pin, and the post-scan `EvictColdFrames(16) > 0` gets *more* reclaimable frames, not fewer |
| 12 | `RingFetchesNeverBumpUsage` | **replace with `c7c3a67`'s** | `main`'s asserts `EvictColdFrames(8) == 0` "a pinned ring slot was reclaimed" — that is the slot-pin model stated as a property. `c7c3a67`'s reads the counter where `ReleaseScanSlot` reads it and adds a device-read count for the in-place rule |
| 13 | `RotationSparesAPinnedPageAndDropsAColdOne` | **rewrite** | `main` asserts `pinned_frames() == 3` (foreground + two slots) and `== 1` after `ring.reset()`. Under AM-R8 it is 2 and 1; `c7c3a67`'s edit is exactly that |
| 14 | `TheRingNeverDropsADirtyFrameOrAResidentClassPage` | **keep `main`'s** | traced under the ported ring: `dirty` in place (no slot), `other` faults into slot 0, `dirty` in place again, destructor drops `other` and refuses `dirty`. Same end state |
| 15 | `eviction.md` §5 AM-R8 paragraph + consumer line | **take** | `main`'s §5 is untouched: no pin sentence, and "Consumers: the CREATE ASSERTION builder and aggregate full scans" — neither has ever opened a ring |
| 16 | `heap_chain.cpp` and `cabin_optimizer_exec.cpp` pin/latch comments, the page-against-page pair | **take** | both describe the ported ring; `main`'s say the ring branch holds no pin, which the port makes false |

### Findings the read produced

**F-1 — `main`'s R3 fix can lose a write, and it is the hazard `c7c3a67`
named against a different fix.** `FetchPinned`'s miss branch loads, then
pins whatever is resident when the guard re-takes the latch
(`device_page_store.cpp:1932`). Between `InsertFrame` and that moment the
frame can be written back, swept, and re-faulted by another core with
`mark_dirty = false`. The pin then lands on a **clean** frame and the
call returns without ever marking it dirty, so a `Get`'s write never
reaches the device. Narrow — it needs a writeback and two sweep laps
inside one window — and it is the same class as everything else this
milestone closes: every gauge balances. The fix is one line in
`PinResidentAndRelease`, which already holds the latch and the frame:
re-apply the dirty mark when the caller asked for one. S-P1 owns it, with
its own cell and mutation.

**F-2 — `a21fe3f`'s pin-ceiling comment claims a scaling the code does not
do.** `device_page_store.cpp:831` says the ceiling "has to admit N per
scanning core"; `pin_ceiling_` is `kPinCeiling × concurrent_pinners` and
nothing widens it for a ring (`device_page_store.hpp:718`,
`core_runtime.cpp:252`). At `cores = 2` the ceiling is 16 and a
production-size ring holds up to 32, so a debug build aborts at
`CountPin` once a scan walks more than 16 distinct pages. Unreached by the
suite — no cell rings more than a handful of pages at `cores > 1` — and
reachable in production the moment the Cabin build walks a 17-page
relation on a two-core instance. The port removes the condition rather
than the comment: one pin per scan, and AM-R13 stands as written.

---

## 8. S-P1 and S-P2 as built — 2026-09-06 on `worktree-am-s2-port` from `75f81a8`

**One commit, not two, and the reason is that S-P1 alone is broken.** S-P1
removes `PinForScan`, so the ring must fetch through `FetchAndPin` — which
takes the page latch. With the slot-pin model still in place that is a
shared latch on every occupied slot for the life of a scan, and the armed
suite aborted on `RotationSparesAPinnedPageAndDropsAColdOne`: the
foreground asks for `X` on a page an open ring is holding `S`, which is
the never-upgrade check firing on entirely correct traffic. §1.3 argued
the latch was safe and it is — under **one** pin. S-P2's `DropHeld()`
before the fetch is what makes it so, and a tree that stopped between the
two stages would not build a story anyone should bisect to.

### 8.1 What landed

- `Resolve` gains `bump_usage`; `FetchPinned` gains it and becomes a
  four-argument forwarder; the private `FetchAndPin` carries the body and
  the `bool* faulted` answer. The three `for_read ? GetForReadUnpinned :
  GetUnpinned` ternaries collapse to `Resolve(page_id, !for_read,
  bump_usage)` — a net subtraction, since those two accessors *are*
  `Resolve` with the bump fixed at true.
- `PinForScan`, its eight-attempt loop and its `ResourceExhausted` are
  gone. The ring is inside `loading_` like every other accessor.
- `ScanRing` is `c7c3a67`'s: one shared, page-latched pin on the page its
  last `Fetch` returned, `DropHeld()` first on every fetch and in the
  destructor, and a slot rotated **only** when the fetch faulted.
- `ReleaseScanSlot` no longer owns a pin; the ring unpins through
  `UnpinFrame`, which is what takes the page latch off with it.
- AM-R8c's sentence at the `ScanFetcher` seam, and the consumer comments
  in `heap_chain.cpp` and `cabin_optimizer_exec.cpp`.

### 8.2 F-1 was withdrawn, and the withdrawal is the finding

S-P0 filed F-1 as a live defect in `main`'s R3 fix and S-P1 was to close
it with a `mark_dirty` re-application in the pin tail. **CLA built that,
then found it guards nothing and reverted it.** `InsertFrame` has four
call sites; the three `Create*` paths insert **dirty**, and
`ResidentBytes`' miss is reached from `FetchAndPin`, which publishes to
`loading_` before it loads — so a concurrent fault of the same page waits
instead of inserting. `LoadingGuard`'s destructor re-locks and *leaves*
the structure latch held into `PinResidentAndRelease`, so the gap between
the erase and the pin is zero rather than small. And `InsertFrame`'s
lost-race arm already carries a loser's dirty flag to the winner.

The one path that inserted without publishing was `PinForScan` — the
ring's. **So AM-R8a is the fix for F-1**, and a guard in the tail would
have been a third instance of the pattern two earlier reviews caught in
this milestone: a fix whose cell passes with the fix reverted. The
invariant is recorded at `InsertFrame`, where it is kept. A debug
assertion there was proposed by the review and **declined**: the
migration-era `*Unpinned` accessors reach that arm from
`mount_recovery_test.cpp` and `btree_test.cpp` with no loading entry, and
`KDS_TEST_PAGE_LATCH` arms those stores, so it would fire on correct
single-threaded traffic.

### 8.3 Cells — six mutants, six kills

| mutation | cell that failed |
|---|---|
| the ring points back at `ResidentBytes` (outside `loading_`) | `TwoRingsFaultingOnePageIssueOneDeviceRead` — 400 reads over 200 rounds against 200 |
| the ring's pin dropped immediately after the fetch | `ARingSpanSurvivesAConcurrentSweepBecauseTheRingPinsIt`, `RotationSparesAPinnedPageAndDropsAColdOne` |
| `AcquirePageLatch` dropped from the pin tail | `ARingFetchWaitsForAPageAnotherCoreHoldsExclusive` |
| the destructor releases slots before it unpins | `RingFetchesNeverBumpUsage` |
| the ring fetch passes `bump_usage = true` | `ARingBounds…`, `RingFetchesNeverBumpUsage`, `RotationSparesAPinnedPage…` |
| rotate on every fetch, not only on a fault | `AnInPlaceHitCostsTheRingNoSlot` |
| `ReleaseScanSlot` ignores the latch word | `ARingSlotIsNotDroppedWhileAnotherCoreHoldsIt` |

**Two of those cells exist because a mutation survived first.**
`always-rotate` passed every ring cell in the tree: the page being fetched
is pinned by the time the rotation runs, so `ReleaseScanSlot` refuses it
and the slot is simply re-recorded. The difference only shows when the hit
is a page the ring does *not* hold — a foreground frame — which is what
`AnInPlaceHitCostsTheRingNoSlot` sets up. And the latch mutant passed once
because the mutation itself was a no-op comment rather than a removed
acquire; corrected, it kills.

### 8.4 What the `critics-developer` pass changed, and what it refuted

The tree moved under it — it was launched against S-P1 and read S-P2 —
which it says plainly rather than reporting stale line numbers as current.

- **Confirmed, independently:** the S-P1 latch shape had both a same-thread
  `S`-then-`X` abort *and* a cross-core ABBA against the chain-growth path,
  which holds two exclusive latches. `DropHeld()` before the fetch is what
  removes both, and that ordering is load-bearing rather than tidy.
- **Refuted, and CLA's own claim:** §1.6's reason for the `Create*` paths
  not producing F-1 — "they take ids nothing else holds" — is *not*
  strictly true. `FetchAndPin` publishes to `loading_` before `Resolve`
  runs the `IsAllocated` check, so a page can be in `loading_` with its
  free-map bit clear and `CreateNew` can claim it. That race is real; it
  cannot lose a write, because the insert it makes is dirty. The record
  now says the load-bearing fact rather than the convenient one.
- **F-2 confirmed as stated and closed by the port:** the ceiling is
  `kPinCeiling × core_count`, nothing widens it for a ring, and the S-P1
  shape could abort a debug build once a Cabin build walked a 17-page
  relation at `cores = 2`. One pin per scan closes it. AM-R13 stands.
- **Applied:** twelve dead lines of `PinForScan`'s doc block that argued
  the ring must *never* take the latch it now takes; `was_resident`
  computed only when a caller asked for it (it was a second hash and probe
  on the single-core hot path); the `faulted`-on-error-return caveat; the
  cell's stale message and its non-vacuity note; the ring's open site in
  `cabin_optimizer_exec.cpp`; and the invariant paragraph at `InsertFrame`.
- **Recorded, not fixed (C-7):** the eight-attempt cap left with
  `PinForScan`, so a ring fault that loses its frame to a sweep now rounds
  `FetchAndPin`'s unbounded loop. The ring is structurally the most exposed
  caller, because its fault inserts at usage 0 and is the first victim of
  the next lap where a foreground fault inserts warm and survives one. Each
  turn re-reads the device, so progress is probabilistically fine, and the
  armed budgeted suite is green — but if it ever hangs, this is the first
  place to look. Re-introducing a cap would restore a truthful error in
  place of a correct answer, which is the trade S-P1 deliberately took.
- **Also recorded:** the pair `S(heap leaf) → S(var-heap page)` is safe
  today only because the Cabin build runs on the relation's owner core and
  every write to that relation routes to the same owner — one thread. The
  page-against-page bullet now says so, and says that nothing enforces it.

### 8.5 Suite

**3363/3363 plain, 3363/3363 armed (`KDS_TEST_PAGE_LATCH=1`), and
3363/3363 armed with `KDS_TEST_FRAME_BUDGET=8`**, plus three by-design
skips and one disabled cell. **Overhead not measured**: the interleaved
A/B is suspended by operator decision, and H4 waits for AM-S6.
