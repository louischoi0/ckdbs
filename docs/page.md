# KDS Page Layout & Buffer Management — Technical Specification

**Status:** **Official specification**, decisions confirmed 2026-07-28. Extends and partially supersedes `docs/page-management.md`: that document's `PageStore` contract (§3) and allocation policy (§6) are amended here; its BufferPool description (§5) is replaced by this document's buffer sections. Markers: `[CONFIRMED]` — decided; `[PROPOSED]` — default within a confirmed decision, adopt or amend before implementing the affected part; `[OPEN]` — do not assume. Consistent with `docs/rules.md`, `docs/wal.md`, `docs/waystone-concept.md`, `docs/sched.md`.

## 0. Decision Record `[CONFIRMED 2026-07-28]`

| # | Decision | Choice |
|---|---|---|
| S1 | Page layout abstraction | **Common 32-byte page header** at offset 0 for all header-bearing page classes |
| S2 | Store interface | **`PageRef`** — RAII pinned-page handle replaces raw spans in the `PageStore` contract |
| S5 | Disk layout | **Single data file**; `offset = page_id × 8192` |
| S7 | Multi-core ownership | **Per-core buffer pools** over core-owned pages (shared-nothing preserved) |
| S9 | Page checksums | **Adopted** — CRC32C in the common header, computed at flush, verified at load |
| S3/S4/S6/S8/S10 | Eviction, dirty/checkpoint, SpaceManager detail, frame memory, config/observability | Defaults specified below as `[PROPOSED]` |

## 1. Page Classes `[CONFIRMED]`

Two page classes exist, and the distinction is load-bearing:

- **Headered pages** — heap, B+ tree, undo, catalog, superblock, free-map: carry the common header (§2), are checksummed, carry `page_lsn`, and participate in WAL and recovery.
- **Headerless pages** — **Waystone entry and directory pages only.** Rationale: both tilings are exact powers of two (256 × 32 B entries; 2048 × 4 B directory children = 8192 exactly), so any header steals a slot and forces division-based addressing, violating the confirmed shift/mask derivations in `docs/waystone-concept.md` §5–6. The exemption is safe *because of* Waystone's advisory contract: these pages are unlogged by default, wholly rebuildable via backfill, and self-verifying at the entry level (each entry stores its `pk`; a mismatch or garbage read is treated as absent, which is always correct). The rejected alternative — halving fanout to 2⁷ to make room for a header — would double Waystone's space cost for integrity it does not need.
- Consequences: WAL `FULL_PAGE_IMAGE` and checksum verification apply to headered pages only; recovery never replays onto headerless pages; the buffer pool records the class per frame (§7) so instrumentation can enforce both this rule and Waystone invariant 8.

## 2. Common Page Header `[CONFIRMED layout]`

Fixed 32 bytes at offset 0 of every headered page. Type-specific content begins at offset 32.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `page_type` | frozen append-only enum: `heap`, `btree_internal`, `btree_leaf`, `undo`, `catalog`, `superblock`, `freemap`, … (`0` = invalid/unformatted) |
| 1 | 1 | `format_version` | per-type layout version; bumps are format events |
| 2 | 2 | `flags` | per-type; 0 unless specified |
| 4 | 4 | `checksum` | CRC32C over the full 8 KiB with this field zeroed (§10) |
| 8 | 8 | `page_lsn` | LSN of the last WAL record applied (wal.md §9); 0 = never logged |
| 16 | 8 | `reserved0` | 0; candidates: epoch (heap), owner core, compaction cursor |
| 24 | 8 | `reserved1` | 0 |

Codec rules as everywhere (rules.md §2/§5): field-wise memcpy helpers, mirror struct + `offsetof` `static_assert`s, fixed-width LE, no bitfields. A shared `page_header` codec module owns this layout; type-specific codecs (heap_page, btree, …) compose it and must not re-implement it.

**Amendment consequence:** `heap_page`'s current layout shifts by 32 bytes (its existing ad-hoc fields fold into the common header where equivalent). No shipped format exists, so this is a code change, not a migration. The design spec's heap section gains the header reference (§12).

## 3. `PageRef` — the Pinned-Page Handle `[CONFIRMED]`

Raw spans are unsafe the moment eviction exists; the interface fixes it structurally now, while callers are few:

- `PageRef` is a move-only RAII guard: construction pins the frame, destruction unpins. It exposes `page_id()`, `bytes()` (fixed-extent span, valid exactly as long as the ref lives), `MarkDirty()`, and `page_class()`.
- `PageStore` v2 contract: `CreateAt(page_id) → StatusOr<PageRef>`, `CreateNew() → StatusOr<PageRef>`, `Get(page_id) → StatusOr<PageRef>`. The three-operation shape survives; only the return type changes.
- Pin discipline: holding a `PageRef` across a task suspension point is legal but metered (§11 pin-residency metric) — suspended portals and long scans are the known holders; leaks show up in the metric, unbalanced unpins become impossible by construction.
- `MarkDirty()` on a ref records the frame's `recLSN` (first-dirty LSN) if unset — the hook that feeds the checkpoint dirty-page table (§8).
- Migration: `InMemoryPageStore`, `BufferPool`, catalog, bootstrap, and tests move to `PageRef` in one change; `Frame` becomes an implementation detail no caller names.

## 4. Single-File Store `[CONFIRMED]`

- One data file per KDS instance. Mapping is pure arithmetic: `file_offset = page_id × 8192`. No file/segment indirection, no mapping table.
- Capacity: `page_id` is u32 with `2³¹` target pages ⇒ 16 TiB file ceiling (design constant, asserted).
- Growth: the file extends by **extents** (§5) via the `IoBackend` (allocation syscalls behind the seam); sparse regions are permitted — a `page_id` may be allocated logically before its neighborhood is dense.
- Well-known pages (superblock, catalog bootstrap) keep their fixed low ids; the superblock at page 0 anchors everything else (WAL anchors per wal.md §14-3, free-map root, high-water).
- Future segmentation (splitting the namespace across files) remains possible behind the same arithmetic contract without a format change; it is `[OPEN]` and explicitly *not* planned — recorded only so nobody designs against its impossibility.

## 5. SpaceManager `[PROPOSED]`

Owns "which page_ids exist / are free" inside the disk-backed store, behind the unchanged `PageStore` seam (page-management.md §6.2, now concretized):

- **Free map:** bitmap pages (`page_type = freemap`). One headered free-map page covers `(8192 − 32) × 8 = 65,280` pages (~510 MiB of data file); free-map pages sit at computable interval positions in the id space so locating the bitmap for a page_id is arithmetic, not lookup.
- **Extent = 64 pages** `[OPEN: size]` — the unit of file growth and (future) per-core prealloc batching. Per-core prealloc remains deferred exactly as decided (page-management §6.3); reconfirmed here.
- **Durability:** allocation state changes emit the reserved `ALLOC`/`FREE` WAL records (wal.md §5); the free map is a headered, logged page class, so recovery replays it like any other page. Crash between extent growth and first use is benign: unreferenced allocated pages are re-freed by a recovery-time sweep of `ALLOC` without matching object linkage `[OPEN: exact reclamation rule — tied to wal.md's reserved-page recovery item]`.
- High-water mark and free-map root live in the superblock.

## 6. Per-Core Buffer Pools `[CONFIRMED]`

- One `BufferPool` instance per core, caching only pages that core owns. Pin counts and frame state stay plain non-atomic fields — the current single-core implementation *is* the per-core implementation; multi-core adds instances, not synchronization.
- Cross-core page access does not exist: work moves to the owning core over the message interface (rules.md §3), consistent with wire-protocol D3 (server-side forwarding).
- The **ownership partition function** (which core owns which pages/relations) is `[OPEN]` until multi-core lands; nothing below depends on its choice.

## 7. Eviction `[PROPOSED]`

- **Clock** (second-chance) over unpinned frames; reference bit set on hit.
- Clean-preferred: clean victims are reclaimed first; a dirty victim is legal but must flush under §8 rules before reuse (flush-on-evict through the same path the checkpointer uses).
- **Resident classes:** superblock, catalog bootstrap pages, and Waystone upper directory levels are pinned-resident (never eviction candidates). The set is small, enumerable, and asserted at startup.
- Frames carry a **usage tag** (normal / waystone / system) enabling the instrumented tests that prove Waystone invariant 8 (no waystone-page touches on the normal read path) and §1's class rules.
- Pool-full with zero evictable frames remains `OutOfSpace` — now a genuine "all pins held" condition rather than the current unconditional cap; it is an overload signal, metered, never silently waited on.

## 8. Dirty Tracking, Flush & Checkpoint Integration `[PROPOSED]`

The pool enforces the WAL contract in code, not by caller discipline:

- Each frame tracks `dirty`, `recLSN` (LSN when first dirtied since last clean), and `page_lsn` mirror.
- The pool holds a `WalDurability` seam (`durable_lsn()` — injected; stub until WAL lands). **`Flush(frame)` refuses (suspends the flushing system task) until `durable_lsn() ≥ page_lsn`** — wal.md §8-1 becomes unbypassable.
- Checksum is computed inside `Flush` immediately before write-out (§10); `MarkClean` exists only as the completion step of the flush path — no caller can "clean" a page without the bytes being durable.
- `DirtyTable()` exports `{page_id → recLSN}` for `CHECKPOINT_BEGIN` (wal.md §11); the checkpointer drives flushes through the same path under the `system` scheduling group and the SLO controller.

## 9. Frame Memory `[PROPOSED]`

- One preallocated, **4 KiB-aligned slab** of `nr_frames × 8 KiB` per pool, carved at startup — O_DIRECT-compatible regardless of the open I/O-backend decision; zero steady-state allocation.
- At the disk transition, frames become owning copies (real read-into / write-from), and `page-management.md` §5.1's "frames are views" note is retired; with `PageRef` (§3) no caller observes the difference.

## 10. Checksums `[CONFIRMED adopted; parameters PROPOSED]`

- Algorithm CRC32C; field per §2; scope: all headered pages; computed at flush (§8), verified on every load from disk (not on buffer hits).
- Verification failure ⇒ `Status(DataCorruption)` to the requester; during recovery, a checksum-failed page with an available `FULL_PAGE_IMAGE` is restored from it (wal.md §10) — the two mechanisms compose: FPI heals torn writes, the checksum is what *detects* them.
- Headerless (Waystone) pages are exempt by class (§1); their integrity story is entry self-identification + rebuildability.

## 11. Configuration & Observability `[PROPOSED]`

- Config surface: `nr_frames` (per core), extent size, checkpoint cadence (owned by wal.md), resident-class list (build-time).
- Metrics, product-grade like wal.md §13: hit ratio, eviction rate (clean vs dirty-flush), dirty fraction, pin-residency time distribution, `OutOfSpace` incidents, checksum failures (always zero in health), flush stall time attributable to WAL waits.

## 12. Required Amendments (gate for implementation)

1. **Design spec — heap page section:** heap layout begins at offset 32 atop the common header; fold duplicated fields; reference this doc for the header. Note the headered/headerless class split. Stamp dates.
2. **`docs/waystone-concept.md`:** record the headerless-class exemption (§1 here) against its §5/§6 — the tilings stand *because* of it; add the entry-self-verification integrity note.
3. **`docs/page-management.md`:** mark §3 (interface) superseded by `PageRef` v2, §5 by this doc's buffer sections, §6 by §5 here; keep with banners per repo convention.
4. **`docs/wal.md`:** cross-ref — `page_lsn`/checksum fields land via this header (its §14-1 satisfied); free-map is a logged page type; note §5 ALLOC/FREE emitter is §5 here.
5. **`CLAUDE.md`:** architecture summary (common header, PageRef, single file, per-core pools, checksums); refresh open list from §13.
6. **Header/code:** `common.hpp` page-class enum; new `page_header` codec module; `heap_page`, catalog, bootstrap, superblock rebased onto it; `PageStore` v2 migration.

## 13. Open Decisions — do not assume

- Extent size; `nr_frames` defaults; clock vs refinements if profiling demands (policy is behind the pool API).
- Core-ownership partition function (multi-core milestone).
- Reserved-page reclamation rule after crash (shared with wal.md's open).
- Reserved header fields' assignment (heap epoch is the leading candidate for `reserved0` — decide with Waystone T-series epoch work).
- Future namespace segmentation (recorded non-goal).
- Direct-I/O specifics (inherited I/O-backend open).

## 14. Testing Requirements

1. **Header codec:** round-trips; offset asserts; unknown `page_type` on load ⇒ error, never interpretation; version-bump gate test.
2. **PageRef:** pin/unpin balance by construction (leak test via pool stats); ref outliving pool ⇒ defined failure; span validity bounded by ref lifetime under eviction pressure.
3. **Eviction:** pinned never evicted; resident classes never candidates; clean-preferred order; dirty eviction flushes first; usage-tag instrumentation drives the Waystone invariant-8 test.
4. **WAL ordering:** with a scripted `WalDurability` stub, `Flush` provably waits for `durable_lsn ≥ page_lsn` under randomized schedules (deterministic sim); `MarkClean` unreachable except via flush completion.
5. **Checksum:** flush-computed, load-verified; injected corruption ⇒ `DataCorruption`; recovery + FPI restoration path (with wal.md's crash matrix).
6. **Single-file store:** arithmetic mapping property tests; sparse growth; extent-boundary allocation; free-map bit accounting vs actual allocations under fault injection.
7. **Per-core:** two pools over disjoint ownership run the full suite independently with zero shared state (asserted by instrumentation).
