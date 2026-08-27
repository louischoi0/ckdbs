# KDS Page Management

Page allocation, the buffer pool, the file layout, and the I/O path. `[PROPOSED]` marks a default to confirm or amend before the affected part is built; `[OPEN]` must not be assumed. Consistent with `docs/rules/rules.md`, `docs/spec/wal.md`, `docs/spec/waystone-concpets.md`, `docs/spec/sched.md`.

## 0. Decision Record

| # | Decision | Choice |
|---|---|---|
| S1 | Page layout abstraction | **Common 32-byte page header** at offset 0 for all header-bearing page classes |
| S2 | Store interface | **`PageRef`** — RAII pinned-page handle replaces raw spans in the `PageStore` contract |
| S5 | Disk layout | **Single data file**; `offset = page_id × 8192`; extent-based growth |
| S7 | Multi-core ownership | **Per-core buffer pools** over core-owned pages (shared-nothing preserved) |
| S9 | Page checksums | **Adopted** — CRC32C in the common header, computed at flush, verified at load |
| S11 | Paging mechanism | **Explicit buffer pool with asynchronous I/O (Postgres-style frames). mmap is rejected for data and WAL** (§15) |
| S12 | Page → relation resolution | **`owner_oid` in the common header** (`reserved1`, §2a) — the page is the mapping, no auxiliary structure. Confirmed and built 2026-08-13 |
| S3/S4/S6/S8/S10 | Eviction, dirty/checkpoint, SpaceManager detail, frame memory, config/observability | Defaults specified below as `[PROPOSED]` |

## 1. Page Classes

Two page classes exist, and the distinction is load-bearing:

- **Headered pages** — heap, B+ tree, undo, catalog, superblock, free-map: carry the common header (§2), are checksummed, carry `page_lsn`, and participate in WAL and recovery.
- **Headerless pages** — **Waystone entry and directory pages only.** Both tilings are exact powers of two (256 × 32 B entries; 2048 × 4 B directory children = 8192 exactly), so any header would steal a slot and force division-based addressing, violating the confirmed shift/mask derivations in `docs/spec/waystone-concpets.md` §5–6. The exemption is safe *because of* Waystone's advisory contract: these pages are unlogged by default, wholly rebuildable via backfill, and self-verifying at the entry level (each entry stores its `pk`; a mismatch or garbage read is treated as absent — always correct). The rejected alternative — halving fanout to 2⁷ to make header room — would double Waystone's space cost for integrity it does not need.
- Consequences: WAL `FULL_PAGE_IMAGE` and checksum verification apply to headered pages only; recovery never replays onto headerless pages; the buffer pool records the class per frame (§7) so instrumentation can enforce both this rule and Waystone invariant 8.

## 2. Common Page Header

Fixed 32 bytes at offset 0 of every headered page. Type-specific content begins at offset 32.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `page_type` | frozen append-only enum: `heap`, `btree_internal`, `btree_leaf`, `undo`, `catalog`, `superblock`, `freemap`, … (`0` = invalid/unformatted) |
| 1 | 1 | `format_version` | per-type layout version; bumps are format events |
| 2 | 2 | `flags` | per-type; 0 unless specified |
| 4 | 4 | `checksum` | CRC32C over the full 8 KiB with this field zeroed (§10) |
| 8 | 8 | `page_lsn` | LSN of the last WAL record applied (wal.md §9); 0 = never logged |
| 16 | 8 | `relayout_epoch` | **built** — bumped when tuples on the page move (`docs/spec/physical-optimizer.md` R4); 0 = never relayouted. Its arrival consumed `reserved0` without a format event, the precedent §2a reuses |
| 24 | 8 | `owner_oid` | **built 2026-08-13** — oid of the owning object, 0 = unattributed (§2a); pre-§2a pages carry 0, which reads correctly |

Codec rules as everywhere (rules.md §2/§5): field-wise memcpy helpers, mirror struct + `offsetof` `static_assert`s, fixed-width LE, no bitfields. A shared `page_header` codec module owns this layout; type-specific codecs compose it and must not re-implement it.

**Amendment consequence:** `heap_page`'s current layout shifts by 32 bytes (existing ad-hoc fields fold into the common header where equivalent). No shipped format exists, so this is a code change, not a migration.

## 2a. Page Ownership — `owner_oid` — confirmed and built 2026-08-13

The engine has forward mappings only (relation → descriptor page, var-heap
root, index roots); nothing resolves a page back to its relation. Two
consumers name that absence as their blocker: **page reclamation**
(`docs/spec/physical-optimizer.md` §6 gate 3 — retired and DROP-TABLE-orphaned
pages are quarantined forever because no structure can prove an owner) and
**the uncountable half of catalog recovery** (`docs/inflight/known-gaps.md`, RV3 —
rows whose relation the catalog lost cannot be attributed). WAL recovery
itself is *not* a consumer: redo is page-id-physical and recovery undo
resolved its one relation-shaped need by refusing the mount
(`docs/workplan-wal-recovery.md`, RC05); nothing here reopens that.

**The design is the field and nothing else.** `owner_oid` — the catalog
`Oid` (u64) of the owning relation — occupies the `reserved1` word at
offset 24 of the common header. No map pages, no in-memory reverse index,
no free-list extension: the page itself is the mapping, and the reverse
query is a scan (below). Any consumer that needs the reverse direction hot
is a new proposal, not this one.

- **Arrival is not a format event**, by the same rule that let
  `relayout_epoch` consume `reserved0`: every page ever written carries 0
  at offset 24, and 0 reads as the correct default — **unattributed**. No
  `format_version` bump; the layout did not change, a reserved word gained
  meaning.
- **Stamping:** written once by `FormatPage` at page initialization
  (signature gains an owner parameter, default 0) and **immutable until
  the page is re-initialized** — the `min_key` discipline. Reuse re-stamps
  through re-initialization, never in place. Written under the
  initializer's exclusive access, read under an ordinary shared pin;
  inside the checksummed span like every header field, so flush/verify
  need nothing new.
- **Per-class semantics:** heap pages and the relation's `kVarHeap` chain
  carry the relation's oid. System classes — undo, catalog, superblock,
  freemap — carry 0; `page_type` already identifies them. Headerless
  Waystone pages are exempt by class (§1). B+ tree pages carry their
  **immediate owner** (decided 2026-08-13): clustered-tree pages the
  relation's oid — the clustered tree *is* the relation's storage — and
  secondary-index pages the index's own oid. One uniform rule: the object
  whose structure the page is. **Corrected at implementation, same day**:
  the confirmation text claimed relations and indexes share the
  `sys.objects` oid space — false. An index oid is a `sys.indexes` row id
  (`AllocateRowId(kSysIndexesTable)`), a separate issue-once sequence, and
  `DropIndex` *retires* the row rather than tombstoning it. So owner
  resolution is discriminated by `page_type`, which the header carries
  right next to the oid: `kIndexLeaf`/`kIndexInternal` owners resolve
  against `sys.indexes` (orphan = no row carries the id, sound because row
  ids are never reissued), every other class against `sys.objects`
  (orphan = the DT2 tombstone). A relation-level query over index pages
  takes the catalog's index → relation edge.
- **WAL:** redo must reproduce the stamp, so `PAGE_INIT`'s payload gains
  `owner_oid` (12 → 24 bytes: `owner_oid` at offset 16, four reserved
  bytes at 12 keeping the codec's mirror struct naturally aligned per
  rules.md §2, the 12-byte prefix unchanged). The decoder accepts both forms,
  the 12-byte legacy one decoding as owner 0 — the same compatible-zero rule
  as the on-page arrival. **Discriminated by a length *floor*, never by
  equality** (corrected by review, same day): `DecodeRecord` hands a payload
  codec the record's 8-byte-aligned tail rather than the exact payload, so a
  12-byte payload arrives as 16 bytes of payload-plus-zero-padding and a
  24-byte one as exactly 24 — an equality test would refuse every legacy
  record read through the envelope, which is the sole case the compatibility
  exists for. The payload change and its versioning mechanics
  are recorded in `docs/spec/wal.md` §5.2 (amended at confirmation).

What it answers, and at what cost:

1. **Owner of page P:** read the header. O(1) with the page in hand.
2. **Is P orphaned:** its `owner_oid` resolves to a `kTypeDroppedTable`
   tombstone row. DT2's retype-never-retire (`docs/spec/drop-table.md`)
   and the never-reissued oid floor make the test ABA-proof — a stamped
   oid can never come to mean a *different* live relation. This is gate
   3's missing ownership proof. An **unattributed page is never
   reclaimable** — pre-feature pages are safe by default, not at risk.
3. **Pages of relation X:** a sequential full-file header scan,
   O(all pages). Deliberately unindexed: the only consumers — background
   reclamation and RV3's lost-relation census — are rare and sequential,
   and the single-file arithmetic layout (§4) makes the scan literally
   one forward read of the file.

What it does not give, stated so nothing is conflated:

- **No free/allocated state.** The SpaceManager's free map (§5) remains
  the `[PROPOSED]` owner of "is this page free"; `owner_oid` says who a
  formatted page belongs to, never whether it may be allocated. The two
  answer different questions and neither substitutes for the other.
- **No change to recovery's refusal branch.** RC05's mount refusal on an
  identity mismatch stands. Noted for its owner to argue: a page's
  `owner_oid` would hand recovery undo the `rel_oid` half of a
  `RowLocator(rel_oid, pk)` call without touching the undo record format
  — a cheaper lifting route than the format-version event
  `docs/workplan-wal-recovery.md` names — but that is RC05's decision,
  not a consequence of this field.

All three `[OPEN]` items decided 2026-08-13: **B+ tree pages carry their
immediate owner** (above); **no backfill** — pre-feature pages stay
permanently unattributed, which is safe by default (owner 0 is never
reclaimable) and costless because no shipped format exists to migrate;
**oid 0 verified rather than assumed** — it is *not* free catalog-wide
(`kNamespaceSys = 0` is a live persisted oid) but it names an object that
owns no pages, and no page-owning object can carry it (system relations
sit at oid 115+, user objects from `kUserOidStart` = 4000), so 0 is
unambiguous as "unattributed" in this field.

Amendments applied at confirmation (2026-08-13): `docs/spec/wal.md` §5.2
(`PAGE_INIT` payload growth, length-discriminated decode),
`docs/spec/physical-optimizer.md` §6 gate 3 (names this as the confirmed
ownership check), `docs/inflight/known-gaps.md` (RV3's uncountable half).

**Built the same day.** What landed: the header field and `GetOwnerOid`;
`FormatPage` and every creation wrapper take the owner (heap
`CreateEmpty`/`CreateEmptyAs`, var-heap `FormatPage`/`CreateChain`/
`ChainAppend`, btree `FormatRoot`/`BtreeInsert` and both index
`CreateEmpty`s, `index::FormatRoot`/`IndexInsert` — the entry points
non-defaulted so no caller can silently skip stamping); rebuilds
(`SplitLeafAndInsert`, `DivideInternalNode`) re-read the page's own stamp;
`PAGE_INIT` is 24 bytes, decoded against a length floor (above), and redo
re-stamps from it; `CREATE TABLE` and `CREATE INDEX` issue the oid *before*
the first page so roots and backfill-split pages stamp from birth (the
index oid is pre-issued via `IndexDef::index_oid`); FPI-created pages carry
the stamp inside the image for free. Catalog-core fixed pages and their
overflow stay 0 (the catalog class); `sys.pattern_defs`/`sys.assertions`
chains stamp their well-known oids because `ChainInsert` grows them. Tests:
header round-trip and stamp, payload owner / legacy-through-the-envelope /
shorter-than-legacy, redo stamp, chain-growth stamp, split stamp.

**Not built, deliberately**: every *consumer* — the RV3 census scan, the
gate-3 orphan test and reclamation — and any backfill (pre-§2a pages stay
0 forever, unreclaimable by construction).

## 3. `PageRef` — the Pinned-Page Handle

Raw spans are unsafe the moment eviction exists; the interface fixes it structurally now, while callers are few:

- `PageRef` is a move-only RAII guard: construction pins the frame, destruction unpins. It exposes `page_id()`, `bytes()` (fixed-extent span, valid exactly as long as the ref lives), `MarkDirty()`, and `page_class()`.
- `PageStore` v2 contract: `CreateAt(page_id) → StatusOr<PageRef>`, `CreateNew() → StatusOr<PageRef>`, `Get(page_id) → StatusOr<PageRef>`. The three-operation shape survives; only the return type changes.
- Pin discipline: holding a `PageRef` across a task suspension point is legal but metered (§11) — suspended portals and long scans are the known holders; unbalanced unpins become impossible by construction.
- `MarkDirty()` records the frame's `recLSN` (first-dirty LSN) if unset — the hook feeding the checkpoint dirty-page table (§8).
- Migration: `InMemoryPageStore`, `BufferPool`, catalog, bootstrap, and tests move to `PageRef` in one change; `Frame` becomes an implementation detail no caller names.

## 4. Single-File Store

- One data file per KDS instance. Mapping is pure arithmetic: `file_offset = page_id × 8192`. No file/segment indirection, no mapping table.
- Capacity: `page_id` is u32 with `2³¹` target pages ⇒ 16 TiB file ceiling (design constant, asserted).
- Growth is extent-based and crash-safe (§14); sparse regions are permitted.
- Well-known pages (superblock, catalog bootstrap) keep their fixed low ids; the superblock at page 0 anchors everything else (WAL anchors per wal.md §14-3, free-map root, high-water).
- Future namespace segmentation across files remains possible behind the same arithmetic contract without a format change; `[OPEN]`, explicitly not planned.

## 5. SpaceManager `[PROPOSED]`

Owns "which page_ids exist / are free" inside the disk-backed store, behind the unchanged `PageStore` seam:

- **Free map:** bitmap pages (`page_type = freemap`). One headered free-map page covers `(8192 − 32) × 8 = 65,280` pages (~510 MiB of data file); free-map pages sit at computable interval positions in the id space — locating the bitmap for a page_id is arithmetic, not lookup.
- **Extent = 64 pages** `[OPEN: size]` — the unit of file growth and, later, per-core prealloc batching. Per-core prealloc is deferred.
- **Durability:** allocation changes emit the reserved `ALLOC`/`FREE` WAL records (wal.md §5); the free map is a headered, logged page class replayed like any other. Crash between extent growth and first use is benign (§14). Reserved-page reclamation rule `[OPEN]`, shared with wal.md.
- High-water mark and free-map root live in the superblock.

## 5a. The var-heap page class

`kVarHeap` (`page_type = 10`, `include/kds/storage/varheap.hpp`): the out-of-line store for values too long for a tuple's fixed-width tagged cell (`docs/spec/heap-and-tuple.md` §3.4).

- **Headered, checksummed, logged** — an ordinary authoritative page class, with a `page_lsn` and a `VARHEAP_APPEND` record (`wal.md` §5.2). Stated explicitly because the recent additions around it (waystone pages, the trail directory) are *advisory*, and those rules must not be pattern-matched onto this one: losing a var-heap value loses a committed value, not a hint.
- **Layout:** the same slotted shape as a heap page — common header, an 8-byte page header (`flags`, `nr_slots`, `lower`, `upper`), a slot directory of `{offset u16, length u16}` growing down, values growing up from the tail `next_page_id` reservation. What it is *not* is a heap page: no MVCC tuple header, no delete-mark, no slot retirement, because a value has no lifetime of its own — it lives and dies with the version pointing at it.
- **Chain:** one per relation, rooted at `sys.tables.varheap_page_id` and allocated at `CREATE TABLE` for any schema that can spill, grown by tail append. The root never moves, which is what keeps it a cacheable fact (`catalog_cache.hpp`'s rule). No `min_key` and no ordering: values are reached only through the pointers in the tuples that own them, so a walk is never a search.
- **Relayout-exempt by construction.** Values are immutable per version, so the physical optimizer has no reason to touch a `kVarHeap` page and must not.
- **Max value = 8144 bytes**, one page's worth. The spilled-value size cap is an `[OPEN]` decision (`rule-fixed-length-tuple.md` §9) and this is not it: a larger value would need a multi-page representation, so it is refused with `Unsupported` rather than answered by inventing one. A future cap can be lower (a policy check above the layer) or higher (a chained representation behind the same `Append`/`Fetch` pair).
- **Reclamation rides on purge**, which does not exist — so nothing is reclaimed yet, and churn-heavy string updates consume space until it does.

## 6. Per-Core Buffer Pools

- One `BufferPool` instance per core, caching only pages that core owns. Pin counts and frame state stay plain non-atomic fields — the current single-core implementation *is* the per-core implementation; multi-core adds instances, not synchronization.
- Cross-core page access does not exist: work moves to the owning core over the message interface (rules.md §3), consistent with wire-protocol D3.
- The ownership partition function is `[OPEN]` until multi-core lands; nothing below depends on it.

## 7. Eviction `[PROPOSED]`

- **Clock** (second-chance) over unpinned frames; reference bit set on hit; the sweep hand advances O(1) amortized.
- Clean-preferred: clean victims reclaimed first; a dirty victim is legal but must flush under §8 before reuse. The background writer (§13) exists precisely to make dirty victims rare.
- **Resident classes:** superblock, catalog bootstrap pages, and Waystone upper directory levels are pinned-resident (never candidates). Small, enumerable, asserted at startup.
- Frames carry a **usage tag** (normal / waystone / system) enabling the instrumented tests for Waystone invariant 8 and §1's class rules.
- Pool-full with zero evictable frames remains `OutOfSpace` — a genuine all-pins-held overload signal, metered, never silently waited on.

## 8. Dirty Tracking, Flush & Checkpoint Integration `[PROPOSED]`

The pool enforces the WAL contract in code, not by caller discipline:

- Each frame tracks `dirty`, `recLSN` (LSN when first dirtied since last clean), and a `page_lsn` mirror.
- The pool holds a `WalDurability` seam (`durable_lsn()` — injected; stub until WAL lands). **`Flush(frame)` refuses (suspends the flushing system task) until `durable_lsn() ≥ page_lsn`** — wal.md §8-1 becomes unbypassable.
- Checksum is computed inside `Flush` immediately before write-out (§10); `MarkClean` exists only as the completion step of the flush path.
- `DirtyTable()` exports `{page_id → recLSN}` for `CHECKPOINT_BEGIN` (wal.md §11); checkpointer and background writer drive flushes through the same path under the `system` scheduling group and the SLO controller.

## 9. Frame Memory `[PROPOSED]`

- One preallocated, **4 KiB-aligned slab** of `nr_frames × 8 KiB` per pool, carved at startup — O_DIRECT-compatible regardless of the open I/O-backend decision; zero steady-state allocation.
- At the disk transition, frames become owning copies (real read-into / write-from); with `PageRef` no caller observes the difference.

## 10. Checksums

- CRC32C (hardware-accelerated where available — SSE4.2 `crc32` on x86-64; the software fallback is a correctness twin used by the deterministic sim); field per §2; scope: all headered pages; computed at flush (§8), verified on every load from disk — never on buffer hits.
- Verification failure ⇒ `Status(DataCorruption)`; during recovery, a checksum-failed page with an available `FULL_PAGE_IMAGE` is restored from it (wal.md §10) — checksum *detects*, FPI *heals*.
- **A never-written page is not a checksum failure (2026-08-24, `docs/inflight/in-progress/workplan-peer-writer.md` PW1c-7).** A page the free map calls allocated that the device cannot address or holds as all zeros reads `NotFound` ("allocated but was never written"), not `Corruption` — the same reading `DevicePageStore::Open` gives an all-zero free map. The state is ordinary once extents are reserved ahead of their pages (a peer's lease is allocated whole in the map core 0 flushes; the peer writes lazily), and the distinction is what recovery needs: redo *creates* a page from `NotFound` under a `PAGE_INIT`, where a `Corruption` it could only poison and wait for an FPI. `CreateAt` accepts such an id after proving the device holds nothing. A torn page with a zero header and a nonzero body is not all zero and still fails verification. One case narrows (the PW1c-7 review's C5): a page that was flushed and later zeroed whole by device damage, whose first in-range record is ordinary and whose `FULL_PAGE_IMAGE` follows, is now refused at redo where it used to be poisoned and healed — reachable only by damage between a checkpoint and a crash, named so it is not rediscovered as a regression.
- Headerless (Waystone) pages are exempt by class (§1).

## 11. Configuration & Observability `[PROPOSED]`

- Config: `nr_frames` per core, extent size, background-writer watermarks (§13), checkpoint cadence (owned by wal.md), resident-class list (build-time).
- Metrics, product-grade like wal.md §13: hit ratio, eviction rate (clean vs dirty-flush split), dirty fraction vs watermarks, pin-residency time distribution, prefetch issued/used/wasted, read/write batch sizes, `OutOfSpace` incidents, checksum failures (zero in health), flush stall time attributable to WAL waits.

## 12. Read Path & Prefetch `[PROPOSED]`

Performance target: **a buffer hit is a hash probe, a pin increment, and a reference-bit set — nothing else.** No locks, no atomics (core-local), no allocation, no syscalls.

- **Hit path:** core-local open-addressing hash `page_id → frame_index` (power-of-two capacity, shift/mask probing), sized with the pool at startup. This replaces the current `unordered_map` — the mapping structure is on the hottest path in the engine and must not chase nodes or allocate.
- **Miss path:** the requesting task suspends on an async read future submitted through `IoBackend`; the core keeps executing other tasks (reactor phases, docs/spec/sched.md). A miss costs the *task* latency, never the *core* throughput. Duplicate misses for the same page coalesce onto one in-flight read (a per-pool in-flight table).
- **Batched submission:** reactor phase 5 drains pending reads as vectored/batched submissions — the seam is shaped for io_uring-style multi-op submission without committing to it (backend `[OPEN]`).
- **Prefetch** — three sources, all advisory, all bounded, all metered (issued/used/wasted):
  1. *Sequential:* per-scan-cursor detection (N consecutive page requests) triggers readahead of the next extent(s); depth adaptive up to a cap `[OPEN: depth]`.
  2. *Structural:* semi-sorted heap range scans know their qualifying page set up front from `min_key` pruning — the executor hands the page list to the pool as a prefetch batch before consuming it. B+ tree upper levels stay hot via clock naturally; leaf-chain scans prefetch like sequential.
  3. *Waystone-driven:* trusted probe locations (epoch-valid) for an upcoming pattern-correlated group are batch-prefetched before execution — the hint index's cheapest win, and advisory twice over (wrong prefetch wastes I/O, never correctness).
- Checksum verification runs on the miss path only (§10), after the read completes, in the task's own context — the hit path never touches it.

## 13. Write Path & Background Writing `[PROPOSED]`

Principle: **foreground tasks never write data pages.** All page write-out is background, in the `system` scheduling group, SLO-throttled:

- **Background writer:** a periodic task that keeps the dirty fraction between low/high watermarks `[OPEN: defaults]` by flushing coldest-dirty frames ahead of demand — its whole purpose is that eviction almost always finds clean victims (§7), converting flush latency spikes into smooth background bandwidth (the Postgres bgwriter role, reshaped for the reactor).
- **Checkpoint spreading:** the checkpointer paces its dirty-table flushes across a fraction of the checkpoint interval rather than bursting, under the same SLO controller (wal.md §11 cadence stays the RTO knob).
- **Write coalescing:** flush batches sort by `page_id`; the single-file arithmetic mapping (§4) makes id-order literally file-order, so sorted batches become sequential writes, and adjacent dirty pages merge into single vectored writes. This is the concrete payoff of decision S5.
- **Ordering:** every write-out passes the §8 WAL gate; commit latency itself is WAL group-commit territory (wal.md §6), not this path.
- **No double caching:** with an O_DIRECT-class backend, the pool is the only cache — no kernel page cache shadow copy, no read-modify-write amplification from cache misalignment (slab alignment per §9 exists for this).

## 14. Growth `[PROPOSED]`

The file is growable to the 16 TiB ceiling with amortized, crash-safe extension:

- Growth unit is the extent (§5), allocated via the `IoBackend`'s allocate verb (fallocate-class: real block reservation, not just size). Under allocation pressure the SpaceManager grows **multiple extents at once** (doubling batch up to a cap `[OPEN]`) so growth syscalls amortize away from the steady state.
- Crash-safe ordering: `ALLOC` WAL record first, then file extension, then first use. File extension is idempotent on replay; an extension without surviving `ALLOC` linkage is re-absorbed by the recovery sweep (§5 open item). File-metadata durability (size/blocks) is folded into the same flush discipline as data (backend-specific verb, defined with the I/O backend decision).
- Shrinking/truncation is a **non-goal** for v1 (`FREE`d space is reused, not returned to the filesystem); recorded so nothing accidentally depends on it.

## 15. mmap Evaluation — Rejected for Data & WAL

mmap was evaluated as the paging mechanism (map the single file, let the kernel page it). It is attractive on the surface — the arithmetic single-file layout is exactly mmap-shaped, and the kernel page cache comes free. It is rejected, for reasons that are well documented across DBMS engineering literature and are *worse* than usual under KDS's architecture:

1. **Write-ordering control is lost.** The kernel may flush a dirty mapped page at any moment. That silently violates WAL-before-data (§8, wal.md §8-1) and destroys torn-page/FPI guarantees — `msync` granularity and timing cannot reconstruct the ordering contract. This alone is disqualifying for a durability-bearing engine.
2. **Page faults stall the whole core.** A fault blocks the faulting *thread*. In a thread-per-core cooperative reactor, that is not one slow query — it freezes every task on the core for an unbounded device-latency window, gutting the SLO model (docs/spec/sched.md). The buffer pool's miss path suspends one task and keeps the core running (§12); mmap has no equivalent.
3. **Error handling is incompatible.** I/O errors surface as `SIGBUS` mid-instruction — irreconcilable with the no-exceptions/`Status` error rules and with any notion of a clean failure path.
4. **It breaks deterministic testing outright.** Kernel paging cannot be injected, scheduled, or fault-injected through the `IoBackend` seam; rules.md §4 (whole-engine simulation with torn-write injection) would be unenforceable. In KDS this is not a nice-to-have — it is how every guarantee in wal.md §16 is proven.
5. **Performance at scale is worse, not better:** TLB shootdown storms on eviction, kernel reclaim contention, 4 KiB kernel granularity vs 8 KiB engine pages, and no interposition point for checksums (§10) or the flush gate (§8).

**Verdict:** explicit per-core buffer pool with asynchronous, seam-injected I/O (decision S11). mmap may appear in offline tooling (e.g. read-only backup inspection utilities) but never inside the engine's data or WAL paths.

## 16. Required Amendments (gate for implementation)

1. **Design spec — heap page section:** heap layout begins at offset 32 atop the common header; fold duplicated fields; note the headered/headerless split. Stamp dates.
2. **`docs/spec/waystone-concpets.md`:** record the headerless-class exemption (§1) against its §5/§6; add the entry-self-verification integrity note; note Waystone-driven prefetch (§12-3) as a consumer.

4. **`docs/spec/wal.md`:** cross-refs — `page_lsn`/checksum land via the common header (§14-1 satisfied); free-map is a logged page type; ALLOC-before-extend ordering (§14 here) referenced from its recovery section.
5. **`docs/spec/sched.md`:** note the background writer and prefetch drain as `system`-group residents.
6. **`CLAUDE.md`:** architecture summary (common header, PageRef, single growable file, per-core pools + clock + bgwriter, checksums, mmap rejected); refresh opens from §17.
7. **Code:** `common.hpp` page-class enum; `page_header` codec module; `heap_page`/catalog/bootstrap/superblock rebase; `PageStore` v2 migration; `unordered_map` → open-addressing mapping table.

## 17. Open Decisions — do not assume

- Extent size; growth-batch cap; `nr_frames` defaults; bgwriter watermarks; prefetch depth caps.
- Core-ownership partition function (multi-core milestone).
- Reserved-page reclamation rule after crash (shared with wal.md).
- Reserved header field assignment: both resolved — `reserved0` became `relayout_epoch` (§2), `reserved1` is `owner_oid` (§2a, built; its `[OPEN]` items decided 2026-08-13 — immediate owner for B+ tree pages, no backfill, oid 0 verified unambiguous).
- I/O backend (inherited); the backend's allocate/metadata-durability verbs are defined with it.
- Future namespace segmentation (recorded non-goal); file shrink (non-goal v1).

## 18. Testing Requirements

1. **Header codec:** round-trips; offset asserts; unknown `page_type` ⇒ error; version-bump gate.
2. **PageRef:** pin balance by construction; span validity bounded by ref lifetime under eviction pressure; ref-across-suspension metering.
3. **Eviction & residency:** pinned never evicted; resident classes never candidates; clean-preferred order; usage-tag instrumentation drives the Waystone invariant-8 test.
4. **WAL gate:** scripted `WalDurability` stub proves `Flush` waits for `durable_lsn ≥ page_lsn` under randomized deterministic schedules; `MarkClean` unreachable outside flush completion.
5. **Checksum:** flush-computed, load-verified, hit path untouched (instrumented); injected corruption ⇒ `DataCorruption`; FPI restoration with wal.md's crash matrix; hardware/software CRC32C equivalence.
6. **Read path:** hit path syscall/allocation-free (instrumented); duplicate-miss coalescing; prefetch never changes results (advisory test in the Waystone style) and its issued/used/wasted accounting is exact; sequential detection triggers at the documented threshold.
7. **Write path:** bgwriter holds dirty fraction within watermarks under simulated load; flush batches are id-sorted and coalesced (assert on the I/O trace); checkpoint spreading paces within its window.
8. **Growth:** ALLOC-before-extend ordering under crash injection at each step; extension idempotent on replay; growth batching amortization visible in the syscall trace; 16 TiB ceiling asserted.
9. **Single-file & per-core:** arithmetic mapping property tests; sparse growth; two pools over disjoint ownership run the suite with zero shared state.
