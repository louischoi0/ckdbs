# Page Eviction — Buffer Pool Replacement and Writeback

Status: **ADOPTED**
Related documents: `docs/spec/page.md` (S1 common header, S2 PageRef, S7 per-core
buffer pool, S9 checksums, S11 mmap rejection), `docs/spec/wal.md`
(WAL-before-data), `docs/spec/assertion.md` §5 (Bound Cabin pinned class),
`docs/spec/sched.md` (scheduling groups). No document owns ANALYZE; its
surface is described in `manual/sql/sql.md` §4.

---

## 1. Positioning

The buffer pool without an eviction path is a memory-bounded cache that
eventually exhausts its frames. This document closes that path with a design
that follows directly from standing engine contracts:

- **This design was built on per-core pools, and AR0 M1 gave that up.** The
  argument is kept rather than deleted, because it is the price now being
  paid and a spec that quietly drops an argument it used to make cannot be
  audited (AM-R7). It read: per-core pools plus thread-per-core ⇒ the entire
  replacement mechanism is core-local, no latches, no lock-free tricks, no
  cross-core coordination anywhere, which is the structural advantage over
  shared-pool engines — PostgreSQL's buffer mapping and clock sweep contend
  on locks; KDS simply had nothing to contend on.

  **What replaced it, and what it bought.** Since AM-S2 step 3 (`2663001`)
  one frame table serves every core, and the contention the argument
  avoided is now real: a structure latch on every frame-table operation and
  a per-frame latch word, both armed only at `cores > 1`. What that buys is
  the thing per-core pools could not do — a peer reads a page core 0 already
  faulted instead of faulting its own copy, so `buffer_pool_frames` is an
  undivided instance total rather than `total / cores` per core, and the
  asymmetry the old split had (core 0 alone carrying the listener, the
  catalog and every session, on `1/cores` of the pool) is gone. **The cost
  is not yet a number**: AM-S1's `cores = 1` A/B bounded the compile-out at
  −1.9% group and +2.4% strict, and the armed cost has been carried as "not
  measured" since. AM-S6 measures it against AL-S8's baseline on AL-S8's
  host, and until that file exists this bullet states a trade whose second
  column is empty.

  **G2 is what keeps the trade honest at one core**: unarmed, the structure
  latch is a null pointer and the guard is a branch, the latch word is never
  touched, and the sharing is not wired at all — so a single-core instance
  pays none of this and reads the paragraph above as history.
- **Cooperative event loop** ⇒ blocking is not an available primitive.
  Exhaustion is handled by bounded cooperative retry and a truthful error,
  never by waiting (consistent with fail-fast semantics elsewhere).
- **WAL-before-data** ⇒ extended to eviction as *flush-before-evict*: a
  dirty page may leave memory only after WAL is durable up to that page's
  last-modification LSN.

## 2. Decision Record

| ID | Decision |
|----|----------|
| EV1 | Replacement policy: **CLOCK (second-chance) with a per-frame usage counter**. Access bumps the counter (saturating); the sweep hand decrements and reclaims frames at zero. No LRU lists. A temperature-model variant reusing the physical-optimizer lazy-decay score (`docs/spec/physical-optimizer.md` R1) is an **experimental hook only**, gated behind the same experimental status as the physical optimizer itself. |
| EV2 | Dirty handling: a **background writeback task** (background scheduling group) keeps a supply of clean frames; eviction prefers clean frames. Forced synchronous writeback is the fallback only. **Flush-before-evict** is mandatory: WAL durable up to the page LSN before the frame is reused. Page checksums (S9) are computed at writeback. |
| EV3 | Pinning is a **page-class attribute**, not a per-page runtime flag. v1 pinned classes: **fixed catalog pages** and **Bound Cabin pages** (`docs/spec/assertion.md` §5). Waystone/trail pages and meta-pool pages are evictable (Waystone is advisory — loss is a performance event, never a correctness event; the meta pool has its own entry-level eviction and is not double-pinned at page level). Debug builds assert on any eviction attempt against a pinned class. PageRef (S2) pins are, as always, absolute: a frame with a live pin is never a sweep candidate. |
| EV4 | **One pool for the instance** (AR0 M1, AM-S2 step 3, `2663001`), where this row read *strict per-core pools, no cross-core frame stealing, no rebalancing in v1* — the rationale being that any stealing path reintroduces cross-core synchronization and forfeits the lock-free property. That property was given up deliberately (§1): the pool is one frame table under a structure latch, `kds.buffer_pool_frames` is an undivided total rather than a per-core share, and "stealing" is not a mechanism because there is nothing to steal from. Sharing **was** conditional on the volume having one WAL stream, since the writeback gate is a property of the log; AM-S4(d) refused the volume that had more than one, so it is unconditional and this row's original text describes no arrangement the engine can be in. What is still per core: the *owner* of a relation's pages (`crosscore.md` CC7) and the write routing that follows from it — M1 shared the cache, not the ownership. |
| EV5 | Eviction trigger: **low-watermark background sweep with an on-demand fallback**. The background task keeps the per-core free-frame reserve above a configured low watermark; foreground allocation takes frames from the free list in O(1). If allocation finds the free list empty, it runs the sweep inline (on-demand fallback). |
| EV6 | Scan resistance: bulk sequential scans in the background group (CREATE ASSERTION builder, aggregate full scans, maintenance scans) run through a **small dedicated ring buffer** of frames, cycling within it and **not bumping usage counters**, so foreground OLTP working sets are never displaced by a scan. |
| EV7 | No page-kind priorities in v1: **uniform CLOCK** across all evictable classes. B+tree inner nodes are protected naturally by their access frequency. No artificial weighting (e.g., elevated initial usage counts for index pages) without a measurement that justifies it. |
| EV8 | Pool exhaustion (every frame pinned or un-flushable): **bounded cooperative retry, then a truthful statement error.** The allocating step yields to the event loop up to a configured retry budget, giving writeback a chance to produce clean frames; on budget exhaustion the statement fails with `ResourceExhausted`. No waiting, ever. Occurrences are counted in production stats; the operational meaning is documented as "pool undersized for the workload." |
| EV9 | Observability: production counters per core — hits, misses, evictions, dirty writebacks, sweep rotations, ring-buffer scan frames served, `ResourceExhausted` occurrences. ANALYZE statements report page-cache hit/miss for their execution (per the standing ANALYZE goals). Sweep timing histograms are dev-mode only (dev/production profiling split). |
| EV10 | Deterministic testing: a **tiny-pool test profile** (e.g., 8 frames per core) makes every CI run exercise eviction, writeback, ring-buffer, and exhaustion paths. Crash matrix gains "immediately before / after dirty-evict writeback" points. The integrity sweep gains a flush-before-evict oracle. |

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

- **FREE**: on the per-core free list; content undefined.
- **ACTIVE(clean)**: cached page, contents match disk (or superseded by WAL
  replay rules); evictable when unpinned, usage counter at zero.
- **ACTIVE(dirty)**: modified since load; never directly evictable — must
  transition to clean via writeback first.

Pinned-class frames (EV3) and frames with live PageRef pins are ACTIVE and
simply invisible to the sweep.

### 3.1 Access path (foreground, hot)

1. Page table lookup (per-core map, no lock).
2. Hit ⇒ saturating increment of the frame's usage counter (cap: small
   constant, PROPOSED 5), return PageRef.
3. Miss ⇒ pop a frame from the free list (O(1)), read the page, insert into
   the map, return PageRef. Free-list-empty ⇒ §3.3.

### 3.2 CLOCK sweep

The sweep hand walks the frame array circularly:

- skip: pinned class, live PageRef pin;
- usage > 0 ⇒ decrement, continue;
- usage == 0, clean ⇒ **reclaim**: remove from page table, push to free
  list;
- usage == 0, dirty ⇒ schedule for writeback (§4); do not reclaim yet.

The sweep runs in two contexts: the background watermark task (EV5 primary)
and the on-demand fallback inside an allocating step (EV5 fallback). Both
execute on the owning core's event loop, so they never race each other —
they are the same code path invoked from two places.

### 3.3 Exhaustion protocol (EV8)

When allocation finds the free list empty **and** an inline sweep rotation
produces no reclaimable frame:

1. Yield cooperatively (re-enqueue the current step; writeback and other
   tasks run).
2. Retry allocation. Repeat up to `kds.evict_retry_budget` (PROPOSED 8)
   times.
3. On budget exhaustion: fail the statement with `ResourceExhausted`,
   message naming the core and pool size. The transaction survives (a
   statement error).

This bounds worst-case foreground latency and converts a pathological
configuration into a visible, countable, truthful signal instead of a stall.

---

## 4. Writeback (EV2)

A background-group task per core:

- Maintains the free-frame reserve above `kds.free_watermark` (PROPOSED:
  1/16 of the pool, which is the instance's since AM-S2 step 3 and was the
  core's share before it) by running sweep rotations.
- Drains a dirty queue populated by the sweep (usage==0 dirty frames) and,
  opportunistically, by age.
- For each dirty page: **(1)** ensure WAL durable ≥ page LSN
  (flush-before-evict; usually a no-op because commit-path flushes run
  ahead), **(2)** compute checksum (S9), **(3)** write via IoBackend,
  **(4)** mark clean. Reclaim happens on the sweep's next visit, keeping the
  page cached until frames are actually needed.
- Batches contiguous page ids where possible (write coalescing) —
  best-effort, not a correctness property.

Checkpointing interaction: the checkpointer's page flushing and this
writeback share the same "durable-then-write-then-clean" primitive; the
checkpointer is a consumer of the writeback machinery, not a parallel
implementation (single code path, deterministic tests cover both callers).

## 5. Scan ring (EV6)

Bulk sequential readers declare ring mode on their scan handle:

- Frames come from a small ring, one per `OpenScanRing` call and not per
  core — the wording said "per-core" until 2026-09-06, and the pool it
  drew that from stopped being per-core when the frame table became one
  table for the instance (`kds.scan_ring_frames`, PROPOSED
  32), reused cyclically; pages read through the ring bypass the page table
  insert-for-retention path (they are mapped while in the ring, then
  dropped) and never bump usage counters.
- **The ring holds a shared pin, and the page latch, on exactly the page
  its last `Fetch` returned; every other slot is an ordinary evictable
  frame** (AM-R8, 2026-09-06). That is what makes "the span is valid until
  the next `Fetch`" a statement about the pool rather than about there
  being one thread: the pin is dropped on the next `Fetch`, before the
  rotation, and in the ring's destructor. Bypassing the retention insert
  and the usage bump is what a ring is for; a pin was never on that list.
  The latch is the other half — without it a scan reads a page a writer on
  another core is in the middle of, which is a wrong answer rather than a
  slow one. It is armed only where the page latch is, at `cores > 1`,
  which is the only configuration where that writer can exist. It runs in
  both directions: a foreground **exclusive** request on the page the ring
  is holding waits until the ring's next `Fetch`, where the read below
  does not. A consumer therefore finishes with the span before the next
  `Fetch` **and does not park while it holds one**, which is `heap_chain`'s
  rule about which walks may take a fetcher, stated once more at the seam
  because a ring's span is the one page handle in the tree that is not a
  `PageRef`.
- **A slot rotates only when the fetch faulted.** A page found resident —
  the foreground's frame, or one this ring already faulted — is used in
  place and costs no slot. The fetch itself reports which it was, rather
  than a residency probe taken before it, because on a shared pool such a
  probe is stale before it is acted on. The release therefore follows the
  fault instead of preceding it, so residency grows by at most the ring's
  size, plus one frame for the length of a `Fetch` that faults.
- Foreground point reads that hit a page currently held by the ring use it
  in place (it is a normal frame; only its lifecycle differs).
- Consumers, as the tree has them: the Cabin optimizer's build walk
  (`src/exec/cabin_optimizer_exec.cpp`, where PO4 makes ring routing
  structural) and the relayout planner's survey
  (`src/stats/relayout_planner.cpp`). This line read "the CREATE ASSERTION
  builder and aggregate full scans" until 2026-09-06; neither has ever
  opened a ring.
- A ring-mode dirty write is not expected in v1 (scans are read paths); a
  writer that needs ring mode must go through the standard dirty
  protocol — the ring never bypasses flush-before-evict.

## 6. Configuration surface

| Setting | Default (PROPOSED) | Notes |
|---|---|---|
| `kds.buffer_pool_frames` | 0 = unbounded until sized | **An undivided instance total** since AM-S2 step 3 (EV4): one frame table holds it and every core draws from it, so the number an operator sets is the number of frames the instance has. It read *total, divided equally per core* — `total / cores` via `FrameBudgetShare`, the remainder undistributed, a nonzero total below `cores` refused at boot — and carried a known asymmetry with it: the even split handed most of the pool to peers while core 0 alone carried the listener, the catalog and every session. Sharing removes the split and the asymmetry together. On a volume with per-core streams the old division is still what runs |
| `kds.free_watermark` | pool/16 | background sweep target; the pool is the instance's (§1) |
| `kds.evict_retry_budget` | 8 | EV8 bounded retry |
| `kds.scan_ring_frames` | 32 per ring | EV6; a ring is per `OpenScanRing` call, not per core (§5) |
| usage counter cap | 5 | compile-time constant |

## 7. Non-goals (v1, documented)

- Cross-core frame stealing / dynamic rebalancing (EV4).
- Page-kind priority weighting (EV7).
- Temperature-unified eviction via decay scores (`docs/spec/physical-optimizer.md` R1) — experimental hook only;
  the hook is a single policy seam in the sweep's victim test, nothing more.
- Prefetching — out of scope for this document (belongs to the scan/executor
  layer).
- Memory-pressure-driven pool resizing at runtime — pool size is boot-fixed
  in v1.
