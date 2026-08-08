# Page Eviction — Workplan (EVT01–EVT08)

Status: **READY FOR EXECUTION**
Spec: `eviction.md` (normative). Related: `storage.md`, `wal.md`,
`assertion-workplan.md` (AST06 depends on EVT06), `testing-workplan.md`.

Execution order is the numbering order unless a dependency says otherwise.
All new code follows the engine rules: explicit Status error types (no
throw), thread-per-core with core-local state, deterministic tests,
field-wise memcpy page access. Every mechanism in this plan is core-local;
any patch introducing cross-core synchronization into these paths is a
design violation, not an implementation detail.

---

## EVT01 — Frame metadata and state machine

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

## EVT02 — Free list and CLOCK sweep core

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

## EVT03 — Background writeback task and watermark maintenance

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

## EVT06 — Scan ring (EV6)

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
- Docs cross-check: `storage.md` gains a pointer to `eviction.md`;
  `assertion-workplan.md` AST06 gains the EVT06 dependency note.

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
