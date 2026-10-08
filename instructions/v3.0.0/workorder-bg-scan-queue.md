# Work order BG — scan resistance: a scan's frames are reclaimed before the hand laps the working set

Written 2026-10-07 on `worktree-bg-scan-ring-order` from `cd8ca91e`
(`v2.7.0-678-gcd8ca91e`). It follows the operator's word, verbatim:

- *"write the scan ring work order"*
- **W1:** *"mark BG-Q0..Q8 as proposed"*
- **W2** (2026-10-08): *"mark BG-Q9 as proposed"*

**Opened 2026-10-07 by W1** (`raft-marks-2026-10-07.md` §24): BG-Q0..Q8 are
marked as proposed, so BG-R1..R5 are rulings and BG-S1 is the next stage.
**BG-Q9 marked as proposed by W2** (`raft-marks-2026-10-08.md` §1): no
floor under the queue, with §5's repeated-probe cell as the gate. Every item
is marked.

**Why this order exists.** BE closed on 2026-10-07 with two findings, and
this order answers the second:
- `bench/v3.0.0/results-be-close-v2.7.0-665-geef442cf.md` §3 measured a
  hot set of half a 16,384-frame pool, warmed once or three times. A scan
  of about four times the pool re-faulted 92 % of it afterwards (one warm
  pass) and all of it (three).
- BE-Q7, marked as proposed, says what follows: *"(b) becomes its own order
  if BE-S6 measures displacement anyway."* BE-S6 did.

The other finding, resident scans 2.5-3.7 % slower, is not this order's.

**What the operator asked for, and what CLA proposes.** The words are "the
scan ring". There are two ways to give an executor scan the ring's
guarantee, and BG-Q1 decides between them:
- **(b)** route the executor through the ring that exists, EV6 in full;
- **(a)** keep the guarantee inside the store, as a queue the reclaim reads
  first. **CLA proposes (a).** §1.3 says why (b) costs what BE-S0's survey
  said it would, and §1.1 says why (a) closes the measured cause.

**BG neither depends on another order nor blocks one.** It shares one
surface with BF: BF deletes `EvictClean` and adds a discard eraser
(`workorder-bf-drop-table-page-reclaim.md`, BF-R5, BF-Q14). BG's queue
tolerates any eraser (BG-R1), so the two land in either order (BG-Q7).

## 0. What BG is

The CLOCK hand reclaims a scan's cold frames only when it reaches them, and
it reaches them by walking the whole pool, lowering every hot frame's
counter as it passes. A scan larger than the pool's free part therefore
drives the hand round the pool many times, and a hot frame survives at most
`kClockUsageCap` (5) laps. BG gives a scan's frames a queue of their own,
and reclaim takes from it before the hand moves.

**BG's rules, one line each:**

- **BG-R1.** A frame a scan faults in (`FetchHeat::kScan` or `kRing`) joins
  the **scan queue**, a FIFO in fault order. Any eraser may remove a
  queued frame; the queue skips stale entries rather than being told.
- **BG-R2.** Every reclaim batch, inline or on the tick, **takes from the
  queue's head before the hand moves**:
  - a frame a hit has warmed (usage > 0) is **promoted**: it leaves the
    queue and is an ordinary CLOCK frame;
  - a resident-class frame leaves the queue as a promoted one does;
  - a pinned, latched or dirty frame goes to the queue's tail, a dirty one
    queued for writeback first;
  - a cold, clean, unheld frame is reclaimed.

  Each entry is visited once per batch; the hand runs only for what the
  queue could not give, under BE-R2's step bound.
- **BG-R3.** **The slot ring retires**: `ScanRing`, `ReleaseScanSlot`,
  `DevicePageStore::OpenScanRing` and `kScanRingFrames`. Its two
  consumers keep `OpenScanRing`, which now returns the base class's plain
  fetcher, fetching through `FetchHeat::kRing` into the queue.
- **BG-R4.** `kRing` stays distinct from `kScan`. A ring fetch never bumps
  a hit, so a Cabin build reading a hot page does not warm it, and its
  frames are promoted only by a foreground touch.
- **BG-R5.** No new keys. The queue is bounded by the pool, not by a
  setting, and `eviction.md` §6's `kds.scan_ring_frames` row, which names
  no key the config reads, is deleted.

**BG does not do:**

- **Route executor scans through a fetcher.** That is BG-Q1 (b), with
  §1.3's costs.
- **Bound a scan's residency below the pool's free part.** Under no
  pressure, a scan's pages stay resident until something needs their
  frames: they are the first out, not kept out. The ring's tighter bound
  (32 frames per scan) is what BG-R3 gives up, deliberately (BG-Q2).
- **The resident-scan regression** BE closed with. It is a different cause,
  `known-gaps.md`'s first Eviction entry.
- **Change anything on disk.**

## 1. Survey at `cd8ca91e`

This section was read, not run, except where it quotes BE's measurements.

### 1.1 Why the hot set goes: laps

- **The mechanism.** A reservation at the cap runs `InlineBatchLocked`
  (`device_page_store.cpp:800`), which walks the hand over slots from
  where it stopped (`SweepLocked`, `:2784`). The tick does the same in
  `MaintainFreeReserve` (`:1906`). Every warm frame the hand passes loses
  one usage point. A cold frame, such as a `kScan` fault (BE-R5), is
  reclaimed when the hand reaches it, and not before.
- **The arithmetic.** With capacity `C` and `H` hot frames, one lap of the
  hand can free at most `C − H` scan frames. A scan of `S` pages past the
  pool therefore drives about `S / (C − H)` laps, and each lap lowers every
  hot frame by one. A hot frame read `u` times through a warm path (a fault
  enters at 1, each hit adds 1) survives `min(u, 5)` laps. The tick's free
  reserve (`C/16`..`C/8`) shrinks `C − H` a little; "about" covers it.
- **BE's cell, in those terms.** BE-S6's cell had `C` = 16,384,
  `H` = 8,192 and `S` ≈ 65,536, so about eight laps. Its hot set was pk
  point lookups, warmed one or three passes and then read once more
  (`before`), so `u` = 2 or 4: neither survives eight laps. 7,510 of 8,192
  (one pass) and 8,243 misses (three passes, internal pages included) were
  re-faulted; the model predicts all 8,192 for both, and the 682 one-pass
  survivors are BG-S1's to explain.
  `ColdScanTest.AHotWorkingSetSurvivesAScanFourTimesThePool`
  passes because its numbers keep the laps under five: `C` = 256, `H` = 32,
  `S` = 1,024 gives 4.6 laps, against hot frames read five times (`kRead`,
  `u` = 5).
- **What closes it.** The hand must not move while a scan's own frames can
  be reclaimed. Lowering the cap, raising `kClockUsageCap`, or a per-scan
  budget each change the arithmetic. None of them removes the laps.

### 1.2 The ring that exists

- **`DevicePageStore::ScanRing`** (`device_page_store.cpp:1061`): a cyclic
  set of `kScanRingFrames` (32, `page_store.hpp:99`) slots per
  `OpenScanRing` call. On every fault it rotates a slot and releases the
  previous occupant through `ReleaseScanSlot` (`:1025`), unless that frame
  is dirty, pinned, warmed, latched or of a resident class. It holds one
  pin and a shared page latch on the page its last `Fetch` returned
  (AM-R8).
- **Two consumers:** the Cabin build (`cabin_optimizer_exec.cpp:140`) and
  the relayout survey (`relayout_planner.cpp:255`). SUS-1 leaves the
  relayout survey dark for every relation created since 2026-09-05.
- **The base class's `PlainScanFetcher`** (`page_store.hpp:518`) is
  `GetForRead`, holding a `PageRef` until the next `Fetch`: the same
  contract the ring offers its caller, with one difference. It fetches the
  next page *before* it drops the last (`last_ = std::move(...)`), so for
  the length of a `Fetch` it holds two pins and two shared page latches,
  where the ring drops first (AM-R8b: "the ring never holds two page
  latches at once"; `kPinCeiling`'s note counts a ring as 1). BG-R3 fixes
  the order.
- **`kds.scan_ring_frames`** is named by `eviction.md` §6 and by three
  comments, and by no config key: nothing parses it.
- **The cells that rely on the ring's own lifecycle** are ten:
  - `eviction_test.cpp`: `:814`, `:862`, `:898`, `:939`,
    `:978` and `SlotArrayTest` `:1586`;
  - `am_s2_pin_protocol_test.cpp`: `:264` and `:360`;
  - `page_latch_test.cpp`: `:418`;
  - `two_core_rig_test.cpp`: `:181`, through `FetchThroughRing` (`:173`,
    `frames=1`): a ring fetch waits for another core's exclusive hold.

  `assertion_recover_test.cpp:434` and the two `wal_manager_test.cpp`
  cells name a different ring (the WAL's and the assertion stage's) and
  are not touched.

### 1.3 Why the executor did not take the ring, restated

BE-S0's read-only analysis (`workorder-be-bounded-pool.md` §1.6) holds at
`cd8ca91e`. Routing `RunWalkStep` (`step_vm.cpp:1958-1971`) through a
fetcher costs:
- **a fetcher on the btree leaf walk's signatures**, which have no fetcher
  seam (`btree.hpp:255`);
- **a ban on parking inside a walk, or a `ReleaseHeld` on the fetcher.** A
  fetcher holds its last page's pin and shared latch until the next
  `Fetch`, and `heap_chain.hpp:265-274` promises the between-pages
  suspension point to the no-fetcher forms only;
- **a shared page latch held across row emission and nested joins at
  `cores > 1`**, where today each page's pin lives for one visit call;
- **a re-fault on every repeat of one range.** The ring drops a slot on
  rotation however hot the page is about to be: xrock's per-day probes are
  exactly that shape, and BE's §2 cell measured them at 1.43 s a pass
  because they hit.

### 1.4 What BE built that BG stands on

- **`FetchHeat`** (`page_store.hpp:51`): `kWarm`, `kScan` (cold fault,
  bumps a hit) and `kRing` (cold fault, never bumps). `PageStore::Fetch`
  (`:214`) maps `PageAccess::kScan` to `kScan`, and `RunWalkStep` passes
  it for the outermost walk only (`step_vm.cpp:1958`).
- **Every CLOCK reclaim goes through `SweepLocked`**, under the structure
  latch: inline and on the tick in batches bounded by `kBatchStepsPerFrame`
  (BE-R2) and ended by `ReclaimWalk`, and `EvictColdFrames` with a
  six-lap bound. `pool_batch_steps` counts the inline batches' steps only;
  the tick adds none.
- **The `Frame` is the page table's node** (BE-S6), with spare bytes in
  its padding: the queue's membership bit costs no size.
- **The erasers share `ReleaseFrameLocked`.** BF adds a discard and deletes
  `EvictClean`, so "every eraser" is not a fixed list; BG-R1's stale-entry
  rule is what makes that not matter.

## 2. Rulings — CLA's proposals, marked as proposed (W1)

### BG-R1 — The scan queue

- **Membership.** `InsertFrame` with `FetchHeat::kScan` or `kRing` pushes
  the page id on the queue and sets `Frame::scan_queued`, a bool in the
  node's padding. Nothing else joins.
- **Storage.** A FIFO of page ids under the structure latch, O(1) at both
  ends. An entry is **stale** when its page is no longer resident, or is
  resident without `scan_queued` (released and re-faulted warm). A stale
  entry is skipped at the head and costs one step. **The test alone misses
  one case:** a page reclaimed and re-faulted by a scan is flagged again,
  so its old entry looks live beside its new one. Either each entry
  carries a fault stamp the frame also holds, or two live-looking entries
  for one page are declared harmless (the first to act clears the flag or
  frees the frame) and compaction keeps the newest. BG-S2 picks one and
  says which.
- **Bounded.** Each fault pushes one entry. When stale entries pass the
  number of live ones, the queue is compacted in one pass. That pass is
  amortised against the pushes that made them, and it runs only inside a
  batch's step bound.
- **Erasers do not touch the queue.** Releasing a frame destroys its node
  and the flag with it, which leaves its entry stale. That is what makes
  BF's discard, or any later eraser, safe without a hook.
- **Partition-ready (BA-R5):** a partition can own a queue as it owns a
  slot range.

### BG-R2 — The queue before the hand

A batch (`SweepLocked`) first walks the queue's head, under the same step
bound, **visiting each entry at most once per batch**:
1. a stale entry is popped;
2. **usage > 0**: popped and promoted. The flag is cleared, the usage
   counter is left as it is, and the frame is an ordinary CLOCK frame;
3. **resident-class**: popped and unflagged, as a promotion. It never
   leaves the pool, so a tail move would keep it in the queue for good (a
   `SELECT` over a catalog relation faults its pages `kScan`);
4. **dirty at usage zero**: queued for writeback exactly as the hand's
   branch does (`queued`, `dirty_eviction_queue_`), and moved to the tail.
   Without the writeback queuing, nothing cleans it while the queue
   supplies and the hand does not run;
5. **pinned or latched**: moved to the tail;
6. otherwise the frame is **reclaimed**.

The steps the queue leaves, when it reclaimed fewer than the batch wants,
go to the hand. The once-per-batch visit is what keeps a queue of held
frames from spending a whole batch rotating. **`ReclaimWalk`'s idle bound
counts the hand's steps only**: counting queue rotations would end a walk,
and refuse `ResourceExhausted`, before the hand had walked a lap. The
total bound counts both. So a scan of any length reclaims its own frames,
and the hand runs only for what the queue cannot give.

### BG-R3 — The slot ring retires

Deleted:
- `DevicePageStore::ScanRing` and `ReleaseScanSlot`;
- `DevicePageStore::OpenScanRing`;
- `kScanRingFrames`, and `OpenScanRing`'s `frames` parameter.

`PageStore::OpenScanRing` keeps its contract, a fetcher holding its last
page until the next `Fetch`, and its `PlainScanFetcher` fetches through
`FetchHeat::kRing` (it calls `GetForRead` today, which is `kWarm`, so this
is a new path, not a flag). **It drops its last `PageRef` before it
fetches the next**, as the ring does (AM-R8b), so a fetcher still holds
one pin and one page latch and `kPinCeiling`'s "a ring adds 1" stays
true. The two consumers do not change.

**AM-R8's latch pair outlives the ring.** The store header's page-against-
page paragraph names the ring's `S(leaf)` held across the Cabin build's
`S(var-heap)` fetches. `PlainScanFetcher`'s held `PageRef` takes the same
pair, so BG-S4 restates that paragraph against the fetcher; it does not
delete it.

What this gives up, stated: a ring scan's residency was bounded to 32
frames. Under BG it is bounded by the pool, and its frames are the first
reclaimed, in FIFO order with every other scan's. The pool is a hard
ceiling since BE, so the bound no longer protects the warm set. It did
also keep a Cabin build from crowding a concurrent `SELECT`'s queued
frames, and that is given up too.

### BG-R4 — Two heats, one queue

`kScan` and `kRing` both fault cold into the queue. They differ only on a
hit:
- **`kScan` bumps**, so a range a `SELECT` reads twice is promoted;
- **`kRing` does not**, so a Cabin build reading a hot page leaves its
  counter alone, as `ScanRing` did. Cells pin both.

### BG-R5 — No keys

No setting sizes the queue. `eviction.md` §6's `kds.scan_ring_frames` row
and the three comments that name it are deleted with `kScanRingFrames`.

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BG-S0 | **The order** | <ul><li>This file</li><li>Its review (§6)</li><li>The index row</li><li>`raft-marks-2026-10-07.md`'s section for the word</li></ul> | S |
| BG-S1 | **The premise, measured** | <ul><li>**The laps.** Re-run BE-S6's scan-resistance cell (`archive/be-close-v2.7.0-664-g2f08b71c/drivers/scanres.py`) at `cd8ca91e`, `build-release`, with the hand's laps counted. `pool_batch_steps` counts inline batches only (§1.4), so the tick's steps are added to a counter first - the stage's one code change - and laps are all steps over `pool_slots`. The order's arithmetic predicts about 8 laps for `S` = 4C, `H` = C/2. If the laps are fewer than the hot frames' usage (`u` = 2 for one warm pass plus `before`) and the hot set still goes, §1.1 is wrong and BG stops for a ruling. The 682 one-pass survivors (§1.1) are explained or reported as unexplained.</li><li>**The census** of `FetchHeat::kScan`/`kRing` producers and of every eraser at the tip of main, BF's discard included if BF has landed, **and the pages a scan faults outside its own walk**: a var-heap spill read during a scan faults warm and never joins the queue, so a scan over spilled columns still drives the hand (BG-S0's review).</li></ul> | S |
| BG-S2 | **The scan queue** (BG-R1, BG-R2, BG-R4) | <ul><li>**Code:** the queue, `scan_queued`, the queue-first batch, promotion, compaction, and `SHOW META`'s `pool_scan_reclaimed`, `pool_scan_promoted` and `pool_scan_queued`, and the hand's steps, inline and on the tick.</li><li>**Cells:**<ul><li>A hot set of half the pool, read once, survives a scan eight times the pool, with zero re-faults.</li><li>A range scanned twice is promoted and survives a later scan, with the rest of the pool full of warm frames.</li><li>A `kRing` hit does not promote; a `kScan` hit does.</li><li>A queued frame released by every eraser and re-faulted warm leaves a stale entry, skipped: the warm frame survives the next batch.</li><li>The reclaim-and-re-fault case of BG-R1, as BG-S2 resolved it.</li><li>A pinned or latched head goes to the tail and is not reclaimed; a dirty one is queued for writeback too; a resident-class one leaves the queue.</li><li>A queue whose every entry is held does not refuse a fill the hand can serve.</li><li>Compaction keeps the queue within twice the live entries.</li><li>On the two-core rig, a scan on one core and point writes on the other, with no lost dirty mark (BE-S3's cell, re-run).</li></ul></li><li>**Mutations, each repeated:**<ul><li>the hand run before the queue (killed by the hot-set cell);</li><li>promotion skipped (killed by the repeated-range cell);</li><li>a stale entry reclaimed (killed by the eraser cell);</li><li>a dirty head moved without the writeback queuing (killed by the dirty-head cell);</li><li>queue rotations counted toward the idle bound (killed by the held-queue cell).</li></ul></li><li>**The suite green**, plain, at the floor and armed, and the sim corpus.</li></ul> | M |
| BG-S3 | **The slot ring retired** (BG-R3, BG-R5) | <ul><li>**Code:** `ScanRing`, `ReleaseScanSlot`, the override, `kScanRingFrames` and the `frames` parameter deleted; `PlainScanFetcher` fetches through `kRing` and drops its last page before the next fetch.</li><li>**Cells:** §1.2's ten ring cells, each rewritten to the queue's property it stood for (a scan's frames are the first reclaimed, a ring fetch never bumps, a held page is never dropped) or deleted where it pinned only the ring's slot lifecycle, each decision listed.</li><li>**The Cabin build's cells** (`cabin_*`) and the relayout cells green, byte-identical where they are contract suites.</li><li>**The suite green**, three ways.</li></ul> | M |
| BG-S4 | **The close** | <ul><li>**The measurement** (§5).</li><li>**Text:**<ul><li>`eviction.md` EV6, §3.1, §3.2, §5, §6's ring row and §7's ring non-goal;</li><li>`page.md` §7;</li><li>the store's header, AM-R8's latch-pair paragraph restated (BG-R3);</li><li>`page_store.hpp`'s `ScanFetcher` and `OpenScanRing` comments;</li><li>`physical-optimizer.md` PO4 and its BUILDING state, and `cabin_optimizer_exec.hpp`/`.cpp`'s PO4 comments;</li><li>`manual/physical-optimizer/physical-optimizer.md`'s EVT06 line;</li><li>`CLAUDE.md`'s eviction row.</li></ul></li><li>**`known-gaps.md`:** the partial-scan-resistance entry deleted if §5 meets its bar, or restated with the number; the "executor does not scan through EV6's ring" entry deleted with the ring.</li></ul> | S |

## 4. Items for the operator

| item | question | kind | CLA's proposal | mark |
|---|---|---|---|---|
| BG-Q0 | **Open BG**, with BG-S0..S4 and BG-R1..R5 as written | process | Yes || **as proposed**, W1 |
| BG-Q1 | **How a scan's frames are kept off the working set.**<br>(a) A scan queue the reclaim reads before the hand (BG-R1, BG-R2).<br>(b) The executor's outermost walk through the slot ring (EV6 in full).<br>(c) A per-statement cap on a scan's resident frames | design | (a). It removes the laps, which are §1.1's measured cause, and costs none of §1.3's first three. The fourth comes back in one state: when warm frames fill the rest of the pool, the queue holds about one reclaim batch (≤ 64 frames), so a range repeated past that re-faults, and the warm frames do not age while the queue supplies. §5's probe cell measures it. (b) pays all four of §1.3's costs, among them a re-fault per repeated range. (c) bounds residency but still laps the hand, because the frames it drops are reclaimed by the same walk || **as proposed: (a)**, W1 |
| BG-Q2 | **The slot ring.**<br>(a) Retired, its consumers on the queue (BG-R3).<br>(b) Kept beside the queue | scope | (a). Two scan-resistance mechanisms in one store would have to agree on every frame both can hold, and the ring's one remaining property - 32 frames per scan - buys nothing under a hard ceiling || **as proposed: (a)**, W1 |
| BG-Q3 | **Promotion**: a queued frame with usage > 0 at the head leaves the queue and keeps its counter | design | Yes. A second probation list (2Q's A1out) would remember evicted ids, and §1.1 has no case that needs it || **as proposed**, W1 |
| BG-Q4 | **`kRing` stays distinct from `kScan`** (BG-R4) | design | Yes. A Cabin build's touch is not heat; a `SELECT`'s second touch is || **as proposed**, W1 |
| BG-Q5 | **No keys**; `kds.scan_ring_frames` deleted from `eviction.md` §6 | user-visible | Yes. It names no key the config reads, and the queue has no number to set || **as proposed**, W1 |
| BG-Q6 | **The bar for BG-S4.** A hot set of half the pool, read once, keeps at least 99 % of its leaf pages through a scan four times the pool; the OLTP A/B within its own run-to-run noise; §5's past-the-cap cells, the per-day probes included, no slower in B than in A | measurement | As written || **as proposed**, W1 |
| BG-Q7 | **Order against BF.** BF deletes `EvictClean` and adds a discard eraser | sequencing | Either order. BG-R1's stale-entry rule needs no hook from any eraser, and BG-S1's census names the erasers at the tip of main || **as proposed**, W1 |
| BG-Q8 | **The measurement** (§5), at BG's close, per `CLAUDE.md`'s Session Workflow step 3 | process | As written || **as proposed**, W1 |
| BG-Q9 | **A floor under the queue.** When warm frames fill the rest of the pool, the queue holds about one reclaim batch (at most 64 frames), so a range repeated past that re-faults - §1.3's fourth cost back in a smaller form - and warm frames stop aging while the queue supplies. (a) No floor (BG-R5), measured by §5's repeated-probe cell before anything is added. (b) A floor as a function of the capacity (2Q's `Kin`, e.g. capacity / 4), below which the hand runs | design, **[quiet-wrong] if unmeasured** | (a), with the measurement as the gate: §5's per-day probes over a pool whose rest is warm decide it, and (b) is written only if they re-fault. A floor is a number; BG-R5 asks that none be added unmeasured || **as proposed: (a)**, W2 |

## 5. Measurement

Once, at BG's close, in `build-release`. It compares `cd8ca91e` (A) with
the commit that closes BG (B). Each is named by `git describe --tags`, the
results go in `bench/v3.0.0/`, and `bench/README.md`'s rules apply. Rule
5's drivers are the BE close's archived ones, stated as such, unless a
`tools/` driver has taken them by then.

- **Scan resistance**, BE-S6's cell. Hot sets of a quarter, a half and
  three quarters of a 16,384-frame pool, read 1 and 3 times, then a scan of
  four and eight times the pool. Report the hot set's re-faults, the hand's
  laps, and `pool_scan_*`. The bar is BG-Q6.
- **Past the cap**, BE's §2 cells: one `COUNT(*)`, and the per-day probes
  twice, at 16,384 and 65,536 frames. B must not be slower than A.
- **The OLTP overhead A/B**, interleaved, with a pool larger than the
  working set, using BE's shapes. BG adds a flag test per fault and
  nothing per hit, so the expectation is flat.
- **A Cabin build** over a relation larger than the pool, A against B.
  Report the build time and what it displaced. It is the one consumer
  whose fetcher changes underneath it.

## 6. Row status

### BG-S0 — written 2026-10-07

Written on `worktree-bg-scan-ring-order` from `cd8ca91e`. No file under
`src/`, `include/` or `tests/` moved, and no suite ran.

**Its review** checked §1 against the tree and corrected it in place:
- **Citations:** a tenth ring cell (`two_core_rig_test.cpp:181`) and
  `heap_chain.hpp`'s lines.
- **BE's numbers:** the hot set was warmed and then read once more, so
  `u` = 2 or 4, and 92-100 % of it was re-faulted.
- **BG-S1's lap counter:** it now counts the tick's steps, and its gate is
  the hot frames' usage.
- **BG-R2:**
  - resident-class heads leave the queue;
  - dirty heads are queued for writeback;
  - each entry is visited once per batch;
  - only the hand's steps count toward the idle bound, which keeps the
    queue from starving the hand into a false refusal.
- **BG-R1:** a reclaimed-then-re-faulted page leaves a live-looking stale
  entry, and BG-S2 picks a stamp or tolerates the duplicate.
- **BG-R3:** `PlainScanFetcher` must release before it fetches, or it holds
  two pins and two latches. The 32-frame bound's loss is stated.
- **Cells, mutations and BG-S4's file list:** added to.

Two of its findings went past the text:
- **BG-Q9, the queue's floor,** is added unmarked.
- **Var-heap spills that bypass the queue** are in BG-S1's census.

Declined: the three cuts the review suggested for length. The order states
its rules once in §0, again with their reasons in §2, and as questions in
§4, which is the house shape for every order.
