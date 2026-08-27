# Page Eviction — Buffer Pool Replacement and Writeback

Status: **ADOPTED**
Related documents: `docs/spec/page.md` (S1 common header, S2 PageRef, S7 per-core
buffer pool, S9 checksums, S11 mmap rejection), `docs/spec/wal.md`
(WAL-before-data), `docs/spec/assertion.md` §5 (Bound Cabin pinned class),
`docs/spec/sched.md` (scheduling groups), `docs/inflight/in-progress/workplan-testing.md`.

*(Filenames corrected 2026-08-10: this list was written against names that
were never in this repository — `storage.md`, `assertion.md`,
`scheduler.md`, `testing-workplan.md`. `analyze.md` is dropped rather than
repointed: **no document owns ANALYZE**, and the surface is described in
`manual/sql/sql.md` §4.)*

---

## 1. Positioning

The buffer pool without an eviction path is a memory-bounded cache that
eventually exhausts its frames — a failure mode observed first-hand in the
kernel-era prototype. This document closes that path with a design that
follows directly from standing engine contracts:

- **Per-core pools (S7) + thread-per-core** ⇒ the entire replacement
  mechanism is core-local. There are no latches, no lock-free tricks, and no
  cross-core coordination anywhere in this design. This is the structural
  advantage over shared-pool engines (PostgreSQL's buffer mapping and clock
  sweep contend on locks; KDS simply has nothing to contend on).
- **Cooperative event loop** ⇒ blocking is not an available primitive.
  Exhaustion is handled by bounded cooperative retry and a truthful error,
  never by waiting (consistent with fail-fast semantics elsewhere).
- **WAL-before-data** ⇒ extended to eviction as *flush-before-evict*: a
  dirty page may leave memory only after WAL is durable up to that page's
  last-modification LSN.

## 2. Decision Record

| ID | Decision |
|----|----------|
| EV1 | Replacement policy: **CLOCK (second-chance) with a per-frame usage counter**. Access bumps the counter (saturating); the sweep hand decrements and reclaims frames at zero. No LRU lists. A temperature-model variant reusing the physical-optimizer lazy-decay score (`docs/spec/physical-optimizer.md` R1) is reserved as an **experimental hook only**, gated behind the same experimental status as the physical optimizer itself. |
| EV2 | Dirty handling: a **background writeback task** (background scheduling group) keeps a supply of clean frames; eviction prefers clean frames. Forced synchronous writeback is the fallback only. **Flush-before-evict** is mandatory: WAL durable up to the page LSN before the frame is reused. Page checksums (S9) are computed at writeback. |
| EV3 | Pinning is a **page-class attribute**, not a per-page runtime flag. v1 pinned classes: **fixed catalog pages** and **Bound Cabin pages** (AS6). Waystone/trail pages and meta-pool pages are evictable (Waystone is advisory — loss is a performance event, never a correctness event; the meta pool has its own entry-level eviction and is not double-pinned at page level). Debug builds assert on any eviction attempt against a pinned class. PageRef (S2) pins are, as always, absolute: a frame with a live pin is never a sweep candidate. |
| EV4 | Strict per-core pools. **No cross-core frame stealing, no rebalancing in v1.** Pool size is a boot-time setting, divided evenly across cores by default. (Rationale: any stealing path reintroduces cross-core synchronization, forfeiting the lock-free property. Skewed-placement rebalancing is a reserved future item.) |
| EV5 | Eviction trigger: **low-watermark background sweep with an on-demand fallback**. The background task keeps the per-core free-frame reserve above a configured low watermark; foreground allocation takes frames from the free list in O(1). If allocation finds the free list empty, it runs the sweep inline (on-demand fallback). This transplants the kernel-era prealloc ring design into the C++ engine. |
| EV6 | Scan resistance: bulk sequential scans in the background group (CREATE ASSERTION builder, aggregate full scans, future maintenance scans) run through a **small dedicated ring buffer** of frames, cycling within it and **not bumping usage counters**, so foreground OLTP working sets are never displaced by a scan. |
| EV7 | No page-kind priorities in v1: **uniform CLOCK** across all evictable classes. B+tree inner nodes are protected naturally by their access frequency. Artificial weighting (e.g., elevated initial usage counts for index pages) is deferred until measurements justify it (same posture as R5). |
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
3. On budget exhaustion: fail the statement with `ResourceExhausted`
   (Status catalog addition, D9 coherence), message naming the core and pool
   size. The transaction survives (statement error, AG3/AS9 precedent).

This bounds worst-case foreground latency and converts a pathological
configuration into a visible, countable, truthful signal instead of a stall.

---

## 4. Writeback (EV2)

A background-group task per core:

- Maintains the free-frame reserve above `kds.free_watermark` (PROPOSED:
  1/16 of the per-core pool) by running sweep rotations.
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

- Frames come from a small per-core ring (`kds.scan_ring_frames`, PROPOSED
  32), reused cyclically; pages read through the ring bypass the page table
  insert-for-retention path (they are mapped while in the ring, then
  dropped) and never bump usage counters.
- Foreground point reads that hit a page currently held by the ring use it
  in place (it is a normal frame; only its lifecycle differs).
- First consumers: CREATE ASSERTION builder (AST06), aggregate full scans.
  ANALYZE-driven maintenance scans join later.
- A ring-mode dirty write is not expected in v1 (scans are read paths); if a
  future writer needs ring mode, it must go through the standard dirty
  protocol — the ring never bypasses flush-before-evict.

## 6. Configuration surface

| Setting | Default (PROPOSED) | Notes |
|---|---|---|
| `kds.buffer_pool_frames` | sized at boot | total, divided evenly per core (EV4) — **built 2026-08-24, under the operator invariant of the same day**: every core's share is *equal* — `total / min(cores, hardware cores)` as ratified, which boot's overcommit refusal collapses to `total / cores` on every instance that exists (the code divides by `cores`; the refusal is what keeps the two formulas equal) — no remainder seat for core 0, the remainder undistributed and bounded by `cores` (`FrameBudgetShare`, tested); a nonzero total below `cores` refused at boot (`CheckFrameBudget`); before this the key reached core 0's pool only and every peer ran unbounded. **Known asymmetry, owned by the R2 arbiter** (`docs/inflight/in-progress/blueprint-range-ownership.md`): the even split hands most of the pool to peers while core 0 alone carries the listener, the catalog and every session - an operator budgeting a mostly-single-core instance should expect core 0's pool to shrink by the core count until shares rebalance by demand |
| `kds.free_watermark` | pool/16 per core | background sweep target |
| `kds.evict_retry_budget` | 8 | EV8 bounded retry |
| `kds.scan_ring_frames` | 32 per core | EV6 |
| usage counter cap | 5 | compile-time constant |

All PROPOSED defaults are to be validated by the standing benchmark and the
tiny-pool profile before promotion.

## 7. Non-goals (v1, documented)

- Cross-core frame stealing / dynamic rebalancing (EV4) — reserved.
- Page-kind priority weighting (EV7) — reserved pending measurements.
- Temperature-unified eviction via decay scores (`docs/spec/physical-optimizer.md` R1) — experimental hook only;
  the hook is a single policy seam in the sweep's victim test, nothing more.
- Prefetching — out of scope for this document (belongs to the scan/executor
  layer).
- Memory-pressure-driven pool resizing at runtime — pool size is boot-fixed
  in v1.
