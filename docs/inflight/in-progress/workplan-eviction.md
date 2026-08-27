# Page Eviction — Workplan (EVT01–EVT08)

Status: **READY FOR EXECUTION**
Spec: `docs/spec/eviction.md` (normative). Related: `docs/spec/page.md`,
`docs/spec/wal.md`, `docs/workplan-assertion.md` (AST06 depends on EVT06),
`docs/inflight/in-progress/workplan-testing.md`. *(Filenames corrected 2026-08-10 — none of
`eviction.md`, `storage.md`, `assertion-workplan.md` or
`testing-workplan.md` has ever existed under those names.)*

Execution order is the numbering order unless a dependency says otherwise.
All new code follows the engine rules: explicit Status error types (no
throw), thread-per-core with core-local state, deterministic tests,
field-wise memcpy page access. Every mechanism in this plan is core-local;
any patch introducing cross-core synchronization into these paths is a
design violation, not an implementation detail.

---

## EVT01 — Frame metadata and state machine  **[PARTLY BUILT 2026-08-09]**

**Built:** `Frame::pins` and `Frame::usage` (saturating, cap
`DevicePageStore::kClockUsageCap`, `[PROPOSED] 5`); the move-only
`DevicePageStore::PageRef` with `PinnedGet`/`PinnedGetForRead`;
`IsPinnedClass()` + `SetResidentLimit()`; `pinned_frames()`.

**Not built:** the FREE/ACTIVE state machine and its edge asserts - there is
no free list yet, so a frame has no FREE state to be in (that is EVT02); and
frame *content poisoning*.

**A finding against EV3, and it changes how the rule can be written.** EV3
says pinning is a page-class attribute "resolved from page kind at load".
That works for a Bound Cabin, which gets a page type of its own. It does
**not** work for the fixed catalog pages: they are formatted
`PageType::kHeap`, exactly as a user relation's pages are, so the page kind
cannot tell them apart. Their reserved ids can, and do
(`catalog/well_known.hpp`). So the implementation is *id-range-or-kind*, and
EV3's "resolved from page kind" holds for one of its two v1 classes rather
than both. The id range is adopted from `system_page_limit_`, the boundary
`SetCoreOwnership` already draws over exactly this set of pages, rather than
a second one being invented beside it.

**Scope.** Extend the per-core frame descriptor with the eviction fields and
enforce the lifecycle of spec §3.

**Deliverables.**
- Frame fields: state (FREE / ACTIVE), dirty bit, usage counter (saturating,
  cap constant), page LSN (last-modification), pinned-class bit resolved
  from page kind at load, PageRef pin count (existing, S2).
- Page-class → pinned mapping table (v1: fixed catalog, Bound Cabin) with a
  single lookup point so future classes are one-line additions.
- Debug asserts: sweep never visits a pinned-class or pin-held frame's
  reclaim branch; state transitions only via the defined edges.

**Acceptance.** Unit tests over the state machine; pinned-class table test;
pin-count interaction test (PageRef alive ⇒ frame untouchable).

---

## EVT02 — Free list and CLOCK sweep core  **[PARTLY BUILT 2026-08-09]**

**Built:** `EvictColdFrames(budget)` - the sweep rotation with §3.2's four
branches in the specified order (skip pinned / skip pinned-class /
decrement / reclaim-clean / queue-dirty), and `TakeDirtyEvictionQueue()`,
which is §4's queue for EVT03 to drain. The pre-existing `EvictClean()` -
the peer cache-invalidation path - now refuses a **pinned** page as it
already refused a dirty one.

**Not built:** the free list and therefore the whole allocation side - there
is no bounded pool, so a miss still creates a frame rather than popping one,
and the on-demand fallback has nothing to fall back to. Frame poisoning.

**Nothing calls the sweep**, and that is a sequencing constraint rather than
an oversight: `page.md` §3's first line is that raw spans are unsafe the
moment eviction exists, and ~257 call sites still take one from
`Get`/`GetForRead`/`CreateAt`/`CreateNew`. **The `PageRef` migration (S2) is
a hard prerequisite for enabling any of this**, and it is not in this
workplan - it belongs to `page.md` §16-7's "PageStore v2 migration". It must
not be staged behind an implicit `PageRef → span` conversion: that would make
`auto s = store.Get(id).value();` compile and dangle, which is the exact bug
eviction introduces.

The sweep exists ahead of the migration for one reason: it makes the
pinned-class guarantee testable *now*, so the Bound Cabin can be built
against it. `tests/eviction_test.cpp` asserts every refusal against a sweep
that reclaims a victim in the same pass, so none of them is a tautology.

**Scope.** Spec §3.2–§3.3 mechanism, single implementation invoked from
both trigger contexts (EV5).

**Deliverables.**
- Per-core free list (O(1) push/pop) feeding the allocation path.
- CLOCK hand + sweep rotation: skip / decrement / reclaim-clean /
  queue-dirty branches exactly as specified.
- On-demand fallback: allocation with empty free list runs an inline
  rotation before entering the retry protocol.
- Page-table removal on reclaim; frame content poisoning in debug builds.

**Acceptance.** Deterministic tests: reclaim ordering matches usage-counter
semantics under a scripted access sequence; dirty frames are never
reclaimed directly; sweep visits skip pinned frames; on-demand fallback
reclaims when a clean zero-usage frame exists.

---

## EVT03 — Background writeback task and watermark maintenance  **[BUILT 2026-08-09]**

**Built.** `DevicePageStore::WriteBack()` is §4's primitive - durable →
checksum → write → clean, in that order, failure leaving the frame dirty
with its recLSN intact so the next pass retries - and it is **the single
code path**: `Flush()`, `FlushPages()` (the checkpointer's route) and the
new `DrainDirtyEvictionQueue()` all run through it, so the checkpointer is
a consumer of the machinery rather than a parallel implementation, proven
by its suites passing unchanged through the refactor. Coalescing is real:
ascending contiguous runs go out as one `WritePageRun` of at most
`kWritebackRunPages` (8, `[PROPOSED]`) through a bounded scratch copy -
best-effort by spec, the zero-copy run arriving with page.md §9's slab -
and a four-page run is pinned by test as exactly one device call.
`MaintainFreeReserve(pool_frames, watermark)` is §4's loop with the pool
size and watermark as **parameters, not fields**, because the bounded pool
is EVT02's unbuilt half and this layer owns the loop's shape while
EVT02/EVT04 own its numbers; each rotation drains what it queued so the
next lap can reclaim - "reclaim happens on the sweep's next visit" -
and it terminates on "a full rotation yielded nothing", tested against an
unsatisfiable watermark. The background task is registered in the
expeditor at a 50 ms `[PROPOSED]` cadence, one bounded batch per tick as
the cooperative-yield boundary - **idle today by construction**, since the
queue only fills when the sweep runs and nothing calls the sweep until the
PageRef migration lands; the same built-ahead-of-its-work stance the sweep
itself takes. The acceptance oracle is structural: a gate probe plus a
counting device prove **no page write ever precedes its WAL durability
point** by a violation counter that must read zero, not by inspection.

**Scope.** Spec §4: the background-group task keeping the free reserve
above `kds.free_watermark`, draining the dirty queue.

**Deliverables.**
- Task registered in the background scheduling group with cooperative
  yielding between batches.
- Writeback primitive: WAL-durable-≥-page-LSN check (flush-before-evict) →
  checksum (S9) → IoBackend write → mark clean. Single code path exposed
  for the checkpointer to reuse.
- Watermark loop: sweep rotations until the free reserve meets target or a
  full rotation yields nothing.
- Write coalescing for contiguous page ids (best-effort).

**Acceptance.** Deterministic tests with a scripted IoBackend: no page
write ever precedes its WAL durability point (oracle-checked); watermark is
restored after a dirty burst; checkpointer reuse compiles against the same
primitive (integration stub).

**Dependency.** Requires WAL flush-to-LSN query/request API — if `wal.md`
implementation lacks "flush up to LSN X and report durable LSN", add it
here as a sub-item (small, but it is the correctness hinge of EV2).

---

## EVT04 — Exhaustion protocol and `ResourceExhausted`

**Scope.** Spec §3.3 bounded cooperative retry + Status addition.

**Deliverables.**
- Retry loop: yield via re-enqueue, `kds.evict_retry_budget` bound, then
  `ResourceExhausted` statement error (transaction survives) with core id
  and pool size in the message.
- Status catalog + KWP wire mapping + KDS Studio display (D9 coherence,
  same checklist as AssertionViolation/AST08).
- Production counter increments on every occurrence.

**Acceptance.** Tiny-pool test that pins all frames and proves: bounded
retries, truthful error, transaction usable afterwards, counter
incremented. Golden-message test.

---

## EVT05 — Configuration surface

**Scope.** Spec §6 settings.

**Deliverables.** `kds.buffer_pool_frames`, `kds.free_watermark`,
`kds.evict_retry_budget`, `kds.scan_ring_frames` wired through boot-time
configuration into per-core pool construction; PROPOSED defaults recorded;
rejection of invalid combinations (watermark ≥ pool, ring ≥ pool, zero
budget) at boot with truthful errors.

**Acceptance.** Boot-validation tests; settings visible via the existing
introspection path (SHOW META or successor).

---

## EVT06 — Scan ring (EV6)  **[BUILT 2026-08-09]**

**Built.** The seam is `storage::ScanFetcher` with a virtual
`PageStore::OpenScanRing()` factory, because every consumer (the relayout
planner's survey, the cabin optimizer's builds, aggregate scans) holds a
`PageStore&` and must not know the concrete store: the base default is the
plain pass-through - what ring mode *means* on a store with no pool to
protect, so `InMemoryPageStore` and every existing test behave
byte-identically - and `DevicePageStore` overrides with the real cyclic
ring. §5's rules land as one drop predicate: rotation (and ring
destruction) drops a slot's frame **unless the foreground claimed it** - a
dirty write, a live pin, a usage bump (only foreground accessors bump;
ring fetches never do, which is what makes usage the claim signal), or
pinned-class membership each abandon the frame to ordinary pool life. A
resident page is used in place with no rotation and no bump, which is the
foreground-hit rule in one direction and "a scan is not heat" in the
other. `heap::ChainVisit` gained the optional fetcher (read walks only -
the ring never bypasses the dirty protocol), and the walk's per-page
discipline is what makes the ring's stricter lifetime safe: each page is
finished before the next fetch can rotate its frame away. **First live
consumer: the relayout planner's survey census.** Acceptance held: a
twelve-page scan through a four-slot ring grows residency by ≤ 4 and the
usage-raised working set survives the following sweep; five ring fetches
leave a page reclaimable in one pass; a mid-scan pin survives rotation
with its bytes intact while the cold slot drops. **Deferred, stated:** the
step VM's aggregate full-scan switch needs the ring threaded through
`Execute` plus a which-steps-qualify policy, and belongs beside EVT07's
observability so the switch is measurable; AST06's builder predates the
ring and adopts it as a follow-up. PHY04's builds are ring consumers from
birth. **This unblocks workplan-physical-optimizer PHY04.**

**Scope.** Spec §5. **Blocks AST06** (assertion builder is the first
consumer) — schedule accordingly.

**Deliverables.**
- Ring-mode scan handle: frame acquisition from the per-core ring, cyclic
  reuse, no usage-counter bumps, page-table visibility while resident.
- Interaction rule implementation: foreground hit on a ring-resident page
  uses it in place; ring rotation with a live foreign pin skips that slot
  (pin-safety preserved).
- Consumer integration: aggregate full-scan path switched to ring mode;
  builder integration lands with AST06.

**Acceptance.** Deterministic test: a full scan over a relation larger than
the pool leaves the pre-scan foreground working set resident (hit-rate
oracle); pin-during-ring-rotation test; usage counters unchanged by scans.

---

## EVT07 — Observability (EV9)

**Scope.** Counters and ANALYZE integration.

**Deliverables.**
- Per-core production counters: hits, misses, evictions, dirty writebacks,
  sweep rotations, ring frames served, exhaustion events.
- ANALYZE per-statement page-cache hit/miss line (hooks into the standing
  ANALYZE work).
- Dev-mode sweep/writeback timing histograms (dev/production split).

**Acceptance.** Counter correctness under EVT02/EVT03 test scenarios;
ANALYZE snapshot tests.

---

## EVT08 — Tiny-pool profile, crash matrix, and close-out (EV10)

**Scope.** Testing integration and documentation hygiene.

**Deliverables.**
- Tiny-pool test profile (PROPOSED 8 frames/core) as a harness
  configuration; CI job running the standard workload suite under it.
- Crash-matrix points: immediately before and after the writeback of a
  dirty evicted page; recovery must show the flush-before-evict invariant
  held (no page on disk newer than durable WAL). **[GATED on the S-2
  recovery loop, same gate as testing-workplan; land the oracle and
  matrix registration now if the loop is not ready.]**
- Integrity-sweep oracle: on-disk page LSN ≤ durable WAL LSN at all times.
- Benchmark note: INSERT/SELECT throughput with eviction active vs the
  pre-eviction baseline at standard pool size (regression budget: noise
  level; eviction must be free when the working set fits).
- Docs cross-check: `docs/spec/page.md` gains a pointer to
  `docs/spec/eviction.md`; `docs/workplan-assertion.md` AST06 gains the
  EVT06 dependency note.

**Acceptance.** Green CI on the tiny-pool job; benchmark recorded in the
perf log; oracle wired into the harness integrity sweep.

---

## Dependency graph

```
EVT01 ──► EVT02 ──► EVT03 ──► EVT04 ──► EVT08
                       │
EVT05 ────────────────┤
EVT06 (needs EVT02) ──┼──► AST06 (assertion builder)
EVT07 (needs EVT03) ──┘
```

EVT05 can land any time after EVT01. EVT06 unblocks AST06 and should be
prioritized if the assertion track is active in parallel.
Gated items: EVT08 crash matrix (S-2 harness).
