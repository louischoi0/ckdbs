# Page Eviction — Buffer Pool Replacement and Writeback

Status: **ADOPTED**
Related documents: `docs/spec/page.md` (S1 common header, S2 PageRef, S7 one
buffer pool, S9 checksums, S11 mmap rejection), `docs/spec/wal.md`
(WAL-before-data), `docs/spec/assertion.md` §5 (Bound Cabin pinned class),
`docs/spec/sched.md` (scheduling groups). No document owns ANALYZE; its
surface is described in `manual/sql/sql.md` §4.

---

## 1. Positioning

The buffer pool without an eviction path is a memory-bounded cache that
eventually exhausts its frames. This document closes that path with a design
that follows directly from standing engine contracts:

- **One pool for the instance** (EV4): one frame table serves every core,
  so a peer reads a page core 0 already faulted instead of faulting its own
  copy, and `buffer_pool_frames` is an undivided instance total. What
  synchronizes it is a structure latch on every frame-table operation and a
  per-frame latch word, both armed only at `cores > 1`. **The armed cost is
  not measured** (AM-S1 measured only the `cores = 1` compile-out).

  **G2 keeps a single core free of it**: unarmed, the structure latch is a
  null pointer and the guard is a branch, the latch word is never touched,
  and the sharing is not wired at all — so a single-core instance pays none
  of this.
- **Cooperative event loop** ⇒ blocking is not an available primitive.
  Exhaustion is handled by a bounded retry and a truthful error,
  never by waiting (consistent with fail-fast semantics elsewhere).
- **WAL-before-data** ⇒ extended to eviction as *flush-before-evict*: a
  dirty page may leave memory only after WAL is durable up to that page's
  last-modification LSN.

## 2. Decision Record

| ID | Decision |
|----|----------|
| EV1 | Replacement policy: **CLOCK (second-chance) with a per-frame usage counter**. Access bumps the counter (saturating); the sweep hand decrements and reclaims frames at zero. No LRU lists. A temperature-model variant reusing the physical-optimizer lazy-decay score (`docs/spec/physical-optimizer.md` R1) is an **experimental hook only**, gated behind the same experimental status as the physical optimizer itself. |
| EV2 | Dirty handling: a **background writeback task** (background scheduling group) keeps a supply of clean frames; eviction prefers clean frames. Forced synchronous writeback is the fallback only. **Flush-before-evict** is mandatory: WAL durable up to the page LSN before the frame is reused. Page checksums (S9) are computed at writeback. |
| EV3 | Pinning is a **page-class attribute**, not a per-page runtime flag. v1 pinned classes: **fixed catalog pages** and **Bound Cabin pages** (`docs/spec/assertion.md` §5). The fixed catalog pages are the system range below `kFirstUserPageId` - the superblock, region 0's bitmaps and the catalog relations' fixed roots - pinned by id, since they are formatted `kHeap` like any relation's page; **a page a catalog chain grows onto past its root is not pinned** (`catalog.md` CT5, superblock 31): it is a `CreateNew` page at or above 128, an ordinary CLOCK candidate reclaimed only clean, unpinned and unlatched and re-faulted with its checksum checked (D2 of the catalog page ceiling, under the operator's ruling of 2026-10-18, "$!recommended"). Its cost is cold faults under the catalog's holds; one of them spends an `INSERT`'s no-refuse window (`known-gaps.md`). Waystone/trail pages and meta-pool pages are evictable (Waystone is advisory — loss is a performance event, never a correctness event; the meta pool has its own entry-level eviction and is not double-pinned at page level). Debug builds assert on any eviction attempt against a pinned class. PageRef (S2) pins are, as always, absolute: a frame with a live pin is never a sweep candidate. |
| EV4 | **One pool for the instance** (AR0 M1): one frame table under a structure latch (§1), `kds.buffer_pool_frames` an undivided total rather than a per-core share, and no frame stealing or rebalancing because there is nothing to steal from. The sharing rests on the volume having one WAL stream, since the writeback gate is a property of the log; a volume with more than one is refused at mount, so it is unconditional. No core owns a relation's pages (`crosscore.md` CC7). |
| EV5 | Eviction trigger: **low-watermark background sweep with an on-demand fallback**. The background task keeps the free-frame reserve above a configured low watermark; foreground allocation takes frames from the free list in O(1). If allocation finds the free list empty, it runs the sweep inline (on-demand fallback). **Built at BE-S3**: the reserve is the instance's one pool, kept between capacity / 16 and capacity / 8 on core 0's writeback tick, and the inline fallback runs in bounded batches (§3.2, §4). |
| EV6 | Scan resistance: background bulk scans (the Cabin optimizer's build walk, §5) run through a **small dedicated ring buffer** of frames, cycling within it and **not bumping usage counters**, so foreground OLTP working sets are never displaced by a scan. |
| EV7 | No page-kind priorities in v1: **uniform CLOCK** across all evictable classes. B+tree inner nodes are protected naturally by their access frequency. No artificial weighting (e.g., elevated initial usage counts for index pages) without a measurement that justifies it. |
| EV8 | Pool exhaustion (every frame pinned or un-flushable): **bounded cooperative retry, then a truthful statement error.** The allocating step yields to the event loop up to a configured retry budget, giving writeback a chance to produce clean frames; on budget exhaustion the statement fails with `ResourceExhausted`. No waiting, ever. Occurrences are counted in production stats; the operational meaning is documented as "pool undersized for the workload." **Built at BE-S4, with three differences from this row** (§3.3): the retry re-walks without yielding, since a fault has no event loop to yield to; a refusal is admitted only before a mutation's first write (the no-refuse window, BE-Q11); and drain mode - recovery, rollback, the assertion loops - writes back on the fault path, waiting on the log's durability, rather than be refused. |
| EV9 | Observability: production counters for the pool (per store; `SHOW META`'s `pool_*`, BE-S3) — hits, misses, evictions, dirty writebacks, sweep rotations, ring-buffer scan frames served, `ResourceExhausted` occurrences. ANALYZE statements report page-cache hit/miss for their execution (per the standing ANALYZE goals). Sweep timing histograms are dev-mode only (dev/production profiling split). |
| EV10 | Deterministic testing: a **tiny-pool test profile** (built as `KDS_TEST_FRAME_BUDGET`, which lowers every debug store's capacity to its value but never below the 256-frame working minimum, and the mount floor raises it to the floor, BE-R3; the simulation harness picks a cap per seed) makes every CI run exercise eviction, writeback, ring-buffer, and exhaustion paths. Crash matrix gains "immediately before / after dirty-evict writeback" points. The integrity sweep gains a flush-before-evict oracle. |

---

## 3. Frame lifecycle

A frame is always in exactly one state:

```
FREE ──alloc──► ACTIVE(clean) ──write──► ACTIVE(dirty)
  ▲                  │                        │
  │                sweep                  writeback
  │                  │                        ▼
  └──────────────────┴──────────────── ACTIVE(clean)
```

- **FREE**: on the instance's free list; content undefined (poisoned in
  debug builds).
- **ACTIVE(clean)**: cached page, contents match disk (or superseded by WAL
  replay rules); evictable when unpinned, usage counter at zero.
- **ACTIVE(dirty)**: modified since load; never directly evictable — must
  transition to clean via writeback first.

Pinned-class frames (EV3) and frames with live PageRef pins are ACTIVE and
simply invisible to the sweep.

### 3.1 Access path (foreground, hot)

The frames live in a **slot array** (BE-R1): chunks of 1,024 slots whose
page bytes never move, a page table from page id to slot, and a free list.
The array grows a chunk at a time, cut at `buffer_pool_frames`, and a slot
is never returned to the allocator.

1. Page table lookup, under the structure latch where the store is shared
   (`page.md` §6).
2. Hit ⇒ saturating increment of the frame's usage counter (cap 5), return
   PageRef.
3. Miss or creation ⇒ **reserve a slot before filling it**, before the
   device read and before a creation claims its id, so every fill in flight
   counts against the capacity. Under the limit, the reservation pops a free
   slot or adds a chunk. At the limit, it runs §3.2's sweep inline in bounded
   batches, and then §3.3 applies.
4. A fault through `PageAccess::kScan` - the outermost read walk of a
   `SELECT` (BE-R5) - inserts its frame at usage 0, so a page a scan touches
   once is the hand's next victim. A hit through it bumps as any hit does.
   Every other fault inserts at usage 1. The scan ring (§5) inserts at 0 and
   never bumps.

### 3.2 CLOCK sweep

The hand is a slot index that walks the array circularly and keeps its
place between sweeps:

- skip: a free or reserved slot, a pinned class, a live PageRef pin, a frame
  whose page latch is held (armed stores only);
- usage > 0 ⇒ decrement, continue;
- usage == 0, clean ⇒ **reclaim**: remove from the page table, poison the
  slot in debug builds, push it on the free list;
- usage == 0, dirty ⇒ queue for writeback (§4) once (a `queued` bit); do not
  reclaim.

**A freed page's frame is discarded, dirty or not** (`page.md` §5,
`instructions/v3.0.0/workorder-bf-drop-table-page-reclaim.md` BF-R5). That
is not the sweep but the free primitive, which erases the frame in the hold
that clears the page's bit. It defers a pinned, latched or
writeback-claimed frame. No replay names a freed page, so its recLSN guards
nothing, and writing its dead bytes back could land them over a reuse.

**Every walk is bounded** (BE-R2):
- A batch reclaims `min(64, capacity / 16)` frames in at most 8 steps per
  frame, under one structure-latch hold, and the latch is released between
  batches.
- A walk of batches ends after a lap that freed and lowered nothing (on the
  §4 tick, also cleaned nothing) - every frame is pinned, latched,
  resident-class or dirty, and another lap cannot change that - or after
  `kClockUsageCap + 1` laps in all, because other cores' hits can keep
  raising the counters it lowers.

The sweep runs in two contexts: the background watermark loop (§4) and a
reservation at the limit. Both are the same code under the frame table's
structure latch, and `FreePage` (which replaced `EvictClean` at BF) erases
under it too. **Two cores' sweeps do not race because both run under
that latch**, not because they share an event loop: one pool serves every
core.

### 3.3 Exhaustion protocol (EV8)

When a reservation finds the pool at its limit and a walk reclaims nothing:

1. **Retry the walk**: it runs `kRefuseRetries` (8, the former
   `evict_retry_budget`) times in all. A drain on another core may have
   cleaned frames meanwhile. **Outside drain mode (below) nothing on this
   path writes back or waits**, and there is no yield: a fault is
   synchronous code with no event loop to yield to.
2. Then fail with `ResourceExhausted`, naming the capacity and how many
   frames are resident, pinned and dirty. **The pool never grows past
   `buffer_pool_frames`.**

**Where a refusal may happen** (BE-Q11). A refusal is safe only before a
mutation's first page write: after one, it would leave the write with
nothing that undoes it. So a mutation that writes and then fetches opens a
**no-refuse window** first:
- Each `INSERT` row, each `UPDATE` row, and the carved bulk fill (sized to
  its pages) open one.
- Opening the window promises it a share of the capacity (64 frames for a
  row). That is the refusal point, with nothing written.
- Fills inside the window spend the share. Outside a window, a fill's limit
  is the capacity less every open window's unspent share.
- A window that outgrows its share in a full pool **stops the process**
  rather than return an error into a half-done write. The log holds every
  committed change.

**Drain mode** covers the passes that are not a row's mutation and hold no
page latch between their steps: mount recovery, a rollback, and an
assertion's commit and abort loops. At the limit, a fill on such a thread,
holding no pin, writes the dirty queue back and walks again; a drain that
cleaned something does not count as a retry. A drain that cleans nothing,
or a thread holding a pin, falls through to step 1's retries and the
refusal.

**The refusal's scope is `txn.md`'s** (BE-Q10, `txn.md` §6). A refused
write in autocommit aborts its transaction. Inside `BEGIN` it puts the
session in `failed-txn` until its `ROLLBACK`. A refused read wrote nothing
and fails only its statement.

This bounds worst-case foreground latency and converts an undersized pool
into a visible, countable, truthful signal (`pool_refused` in `SHOW META`)
instead of a stall or an unbounded heap.

---

## 4. Writeback (EV2)

One background task for the instance, on core 0:

- **Runs on core 0's 50 ms writeback tick** (`MaintainFreeReserve`, BE-R2).
  When the pool's free count (the capacity less the slots in use and the
  open windows' promises) falls below `capacity / 16`, it reclaims in
  bounded batches, one per latch hold with the dirty queue drained between
  them, until the count reaches `capacity / 8`. It does at most one deficit
  per call and ends on the same walk bound as §3.2. The tick holds no page
  latch, which is what makes its drain sound. At `cores = 1` a long
  statement holds the reactor and the tick does not run, which is why the
  inline path is bounded on its own.
- Drains the dirty queue the sweeps populate (usage==0 dirty frames, each
  queued once), after the watermark loop on every tick. Nothing queues a
  frame by age.
- For each dirty page: **(1)** ensure WAL durable ≥ page LSN
  (flush-before-evict; usually a no-op because commit-path flushes run
  ahead), **(2)** compute checksum (S9), **(3)** write via IoBackend,
  **(4)** mark clean. Reclaim happens on the sweep's next visit, keeping the
  page cached until frames are actually needed.
- **The drain leaves what another holds** (`HeldFrames::kSkip`): a frame
  another core holds exclusive ends the run, and a frame another writeback
  has claimed between its copy and its clean (AT-S10e, `page.md` §8) is
  left to that writer. A flush, `Sync` and the checkpointer wait for both
  instead (`kWait`), since they owe a durability barrier.
- Batches contiguous page ids where possible (write coalescing) —
  best-effort, not a correctness property.

Checkpointing interaction: the checkpointer's page flushing and this
writeback share the same "durable-then-write-then-clean" primitive; the
checkpointer is a consumer of the writeback machinery, not a parallel
implementation (single code path, deterministic tests cover both callers).

## 5. Scan ring (EV6)

Bulk sequential readers declare ring mode on their scan handle:

- Frames come from a small ring, one per `OpenScanRing` call and not per
  core (`kScanRingFrames`, `PROPOSED` 32, `storage/page_store.hpp`), reused cyclically; pages read through the ring bypass the page table
  insert-for-retention path (they are mapped while in the ring, then
  dropped) and never bump usage counters.
- **The ring holds a shared pin, and the page latch, on exactly the page
  its last `Fetch` returned; every other slot is an ordinary evictable
  frame** (AM-R8, 2026-09-06). The pin makes "the span is valid until the
  next `Fetch`" true of a shared pool; it is dropped on the next `Fetch`,
  before the rotation, and in the ring's destructor. The latch keeps a scan
  from reading a page a writer on another core is mid-way through - a wrong
  answer, not a slow one - and is armed only at `cores > 1`. A foreground
  **exclusive** request on the ring's page waits until the ring's next
  `Fetch`. A consumer therefore finishes with the span before the next
  `Fetch` **and does not park while it holds one** (`heap_chain`'s rule): a
  ring's span is the one page handle in the tree that is not a `PageRef`.
- **A slot rotates only when the fetch faulted.** A page found resident —
  the foreground's frame, or one this ring already faulted — is used in
  place and costs no slot. The fetch itself reports which it was, rather
  than a residency probe taken before it, because on a shared pool such a
  probe is stale before it is acted on. The release therefore follows the
  fault instead of preceding it, so residency grows by at most the ring's
  size, plus one frame for the length of a `Fetch` that faults.
- Foreground point reads that hit a page currently held by the ring use it
  in place (it is a normal frame; only its lifecycle differs).
- The one consumer, as the tree has it: the Cabin optimizer's build walk
  (`src/exec/cabin_optimizer_exec.cpp`, where PO4 makes ring routing
  structural).
- A ring-mode dirty write is not expected in v1 (scans are read paths); a
  writer that needs ring mode must go through the standard dirty
  protocol — the ring never bypasses flush-before-evict.

## 6. Configuration surface

| Setting | Default (PROPOSED) | Notes |
|---|---|---|
| `buffer_pool_frames` | **required; no default** (ruled 2026-10-07, built at BE-S4) | The pool's **maximum**, in 8 KiB frames, for the whole instance: one pool every core shares (EV4). A config without it, a `0`, and a server started with no config are refused `InvalidArgument` naming the key. It must cover the volume's resident-class pages plus a working minimum of 256 frames (BE-Q6), checked before the mount and again after it; a value below is refused naming both numbers |
| free watermark | capacity / 16 (`low`), refilled to capacity / 8 (`high`) | §4. A constant function of the capacity since BE-Q5, not a key |
| retry bound | 8 | §3.3. A constant since BE-Q5, not a key |
| ring size | 32 per ring | EV6; `kScanRingFrames`, a parameter of `OpenScanRing` and not a config key; a ring is per call, not per core (§5) |
| usage counter cap | 5 | compile-time constant |

## 7. Non-goals (v1, documented)

- Page-kind priority weighting (EV7).
- Temperature-unified eviction via decay scores (`docs/spec/physical-optimizer.md` R1) — experimental hook only;
  the hook is a single policy seam in the sweep's victim test, nothing more.
- Prefetching — out of scope for this document (belongs to the scan/executor
  layer).
- Memory-pressure-driven pool resizing at runtime — pool size is boot-fixed
  in v1.
- Returning memory below the high-water mark: a slot, once allocated, stays.
  The capacity is the promise, not a shrink.
- Routing executor scans through the scan ring (§5). BE-R5's cold fault is
  the executor's scan resistance; `workorder-be-bounded-pool.md` §1.6 says
  why the ring would cost more there.
