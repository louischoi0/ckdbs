# KDS Page Management

Page allocation, the buffer pool, the file layout, and the I/O path. Consistent with `docs/rules/rules.md`, `docs/spec/wal.md`, `docs/spec/waystone-concpets.md`, `docs/spec/sched.md`; frame replacement is `docs/spec/eviction.md`'s.

## 0. Decision Record

| # | Decision | Choice |
|---|---|---|
| S1 | Page layout abstraction | **Common 32-byte page header** at offset 0 for all header-bearing page classes |
| S2 | Store interface | **`PageRef`** — RAII pinned-page handle; the `PageStore` contract hands out no raw span |
| S5 | Disk layout | **Single data file**; `offset = page_id × 8192`; extent-based growth |
| S7 | Multi-core ownership | **Per-core buffer pools** over core-owned pages. Unchanged by AR0 M0: the log is shared, the frames are not (§6) |
| S9 | Page checksums | **Adopted** — CRC32C in the common header, computed at flush, verified at load |
| S11 | Paging mechanism | **Explicit buffer pool. mmap is rejected for data and WAL** (§15) |
| S12 | Page → relation resolution | **`owner_oid` in the common header** (§2a) — the page is the mapping, no auxiliary structure |
| S3/S4/S6/S8/S10 | Eviction, dirty tracking, space management, frame memory, observability | §§5, 7-9 and 11 state what is built |

## 1. Page Classes

Two page classes exist, and the distinction is load-bearing:

- **Headered pages** — heap, B+ tree, undo, catalog, superblock, free-map, var-heap, Waystone entry pages: carry the common header (§2), are checksummed, carry `page_lsn`, and participate in WAL and recovery.
- **Headerless pages** — **the Waystone directory's interior pages only.** Their tiling is an exact power of two (2048 × 4 B directory children = 8192 exactly), so a header would steal a slot and force division-based addressing, violating the shift/mask derivations in `docs/spec/waystone-concpets.md` §5–6. The exemption is safe *because of* Waystone's advisory contract: a damaged interior page leads a walk to a page that is not the instance's waystone, which the headered target's own check turns into a miss — always correct. A page is marked headerless durably, in a bitmap page of the free map's shape (`kHeaderlessMap`, §5), because the store is opened before there is a catalog to ask and a verify on first touch would otherwise reject the page. The rejected alternative — halving fanout to make header room — would double the directory's space cost for integrity it does not need.
- Consequences: WAL `FULL_PAGE_IMAGE` and checksum verification apply to headered pages only; recovery never replays onto headerless pages; the buffer pool records the class per frame so instrumentation can enforce both this rule and Waystone invariant 8.

## 2. Common Page Header

Fixed 32 bytes at offset 0 of every headered page. Type-specific content begins at offset 32.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `page_type` | frozen append-only enum: `heap`, `btree_internal`, `btree_leaf`, `undo`, `catalog`, `superblock`, `freemap`, … (`0` = invalid/unformatted) |
| 1 | 1 | `format_version` | per-type layout version; bumps are format events |
| 2 | 2 | `flags` | the PL-C stream stamp: `core_id + 1` of the core that last claimed the page, 0 = never stamped (`page-lsn-cross-stream.md` §9 rule 4). **A claim of ownership, not a statement about which log the page's records are in** — under one WAL stream (`wal.md` §3) redo neither refuses a foreign value nor rewrites one |
| 4 | 4 | `checksum` | CRC32C over the full 8 KiB with this field zeroed (§10) |
| 8 | 8 | `page_lsn` | LSN of the last WAL record applied (wal.md §9); 0 = never logged |
| 16 | 8 | `relayout_epoch` | bumped when tuples on the page move (`docs/spec/physical-optimizer.md` R4, `heap-and-tuple.md` §3.1a); 0 = never relayouted |
| 24 | 8 | `owner_oid` | oid of the owning object, 0 = unattributed (§2a) |

Codec rules as everywhere (rules.md §2/§5): field-wise memcpy helpers, mirror struct + `offsetof` `static_assert`s, fixed-width LE, no bitfields. A shared `page_header` codec module owns this layout; type-specific codecs compose it and must not re-implement it.

**A reserved word that gains a meaning under which 0 is the correct default is not a format event.** Every page ever written carries 0 there and reads correctly; no `format_version` bump, no backfill. Offsets 2, 16 and 24 were all assigned that way.

## 2a. Page Ownership — `owner_oid`

The engine's other mappings run forward only (relation → descriptor page, var-heap root, index roots); `owner_oid` is the one thing that resolves a page back to its owner. It is the catalog `Oid` (u64) of the owning object, at offset 24 of the common header. No map pages, no in-memory reverse index, no free-list extension: the page itself is the mapping, and the reverse query is a scan (below). A consumer that needs the reverse direction hot is a new proposal, not this one.

- **Stamping:** written once by `FormatPage` at page initialization and **immutable until the page is re-initialized** — the `min_key` discipline. Reuse re-stamps through re-initialization, never in place. Written under the initializer's exclusive access, read under an ordinary shared pin; inside the checksummed span like every header field, so flush/verify need nothing new. Every page-creating entry point takes the owner non-defaulted — heap `CreateEmpty`/`CreateEmptyAs`, var-heap `FormatPage`/`CreateChain`/`ChainAppend`, btree `FormatRoot`/`BtreeInsert`, both index `CreateEmpty`s, `index::FormatRoot`/`IndexInsert` — so no caller can silently skip it; rebuilds (`SplitLeafAndInsert`, `DivideInternalNode`) re-read the page's own stamp; `CREATE TABLE` and `CREATE INDEX` issue the oid *before* the first page (the index oid via `IndexDef::index_oid`), so roots and backfill-split pages stamp from birth; an FPI-created page carries the stamp inside the image.
- **Per-class semantics:** heap pages and the relation's `kVarHeap` chain carry the relation's oid. System classes — undo, catalog, superblock, freemap — carry 0; `page_type` already identifies them. The catalog's fixed pages and their overflow are 0; the `sys.assertions` chain stamps its well-known oid because `ChainInsert` grows it. Headerless pages are exempt by class (§1). B+ tree pages carry their **immediate owner**: clustered-tree pages the relation's oid — the clustered tree *is* the relation's storage — and secondary-index pages the index's own oid. One uniform rule: the object whose structure the page is. Relations and indexes do **not** share an oid space: an index oid is a `sys.indexes` row id (`AllocateRowId(kSysIndexesTable)`), a separate issue-once sequence, and `DropIndex` *retires* the row rather than tombstoning it. Owner resolution is therefore discriminated by `page_type`, which the header carries next to the oid: `kIndexLeaf`/`kIndexInternal` owners resolve against `sys.indexes` (orphan = no row carries the id, sound because row ids are never reissued), every other class against `sys.objects` (orphan = the `kTypeDroppedTable` tombstone). A relation-level query over index pages takes the catalog's index → relation edge.
- **WAL:** redo reproduces the stamp. `PAGE_INIT`'s payload is 24 bytes — `owner_oid` at offset 16, four reserved bytes at 12 keeping the codec's mirror struct naturally aligned (rules.md §2), the 12-byte prefix unchanged — and the decoder accepts the 12-byte legacy form as owner 0, **discriminated by a length *floor*, never by equality**: `DecodeRecord` hands a payload codec the record's 8-byte-aligned tail rather than the exact payload, so a 12-byte payload arrives as 16 bytes of payload-plus-zero-padding and a 24-byte one as exactly 24, and an equality test would refuse every legacy record read through the envelope. `docs/spec/wal.md` §5.2 carries the payload.

What it answers, and at what cost:

1. **Owner of page P:** read the header. O(1) with the page in hand.
2. **Is P orphaned:** its `owner_oid` resolves to a `kTypeDroppedTable` tombstone row. The tombstone's retype-never-retire rule (`docs/spec/drop-table.md`) and the never-reissued oid floor make the test ABA-proof — a stamped oid can never come to mean a *different* live relation. An **unattributed page is never reclaimable**.
3. **Pages of relation X:** a sequential full-file header scan, O(all pages). Deliberately unindexed: the reverse query is rare and sequential, and the single-file arithmetic layout (§4) makes the scan one forward read of the file.

What it does not give:

- **No free/allocated state.** The free map (§5) owns "is this page free"; `owner_oid` says who a formatted page belongs to, never whether it may be allocated. Neither substitutes for the other.
- **Nothing in recovery.** Redo is page-id-physical, and recovery undo's mount refusal on an identity mismatch stands; neither reads this field.

**oid 0 is unambiguous as "unattributed."** It is *not* free catalog-wide (`kNamespaceSys = 0` is a live persisted oid) but it names an object that owns no pages, and no page-owning object can carry it: system relations sit at oid 115+, user objects from `kUserOidStart` = 4000. Pages written before the field existed carry 0 permanently and are never reclaimable.

No consumer reads the field today: there is no census scan, no orphan test and no reclamation.

## 3. `PageRef` — the Pinned-Page Handle

Raw spans are unsafe the moment eviction exists; the interface fixes it structurally:

- `PageRef` is a move-only RAII guard: construction pins the frame, destruction unpins. It exposes `page_id()`, `bytes()` (fixed-extent span, valid exactly as long as the ref lives), `MarkDirty()`, and `page_class()`.
- `PageStore` contract: `CreateAt(page_id) → StatusOr<PageRef>`, `CreateNew() → StatusOr<pair<PageId, PageRef>>`, `Get(page_id) → StatusOr<PageRef>`, `GetForRead(page_id) → StatusOr<PageRef>`. Unbalanced unpins are impossible by construction; `Frame` is an implementation detail no caller names.
- Pin discipline: holding a `PageRef` across a task suspension point is legal but metered (§11) — suspended portals and long scans are the known holders.
- `MarkDirty()` records the frame's `recLSN` (first-dirty LSN) if unset — the hook feeding the checkpoint dirty-page table (§8).

## 4. Single-File Store

- One data file per KDS instance. Mapping is pure arithmetic: `file_offset = page_id × 8192`. No file/segment indirection, no mapping table.
- Capacity: `page_id` is u32 with `2³¹` target pages ⇒ 16 TiB file ceiling (design constant, asserted).
- Growth is extent-based and crash-safe (§14); sparse regions are permitted.
- Well-known pages (superblock, free-map and headerless-map pages, catalog bootstrap) keep their fixed low ids; the superblock at page 0 anchors the WAL anchor table (wal.md §14-3). It holds no free-map root: the map's pages are found by arithmetic (§5).
- Segmentation across files is a non-goal; the contract is one file and one arithmetic mapping.

## 5. SpaceManager

Owns "which page_ids exist / are free" inside the disk-backed store, behind the unchanged `PageStore` seam (`DevicePageStore`, `include/kds/storage/free_map.hpp`):

- **Free map:** bitmap pages (`page_type = freemap`), one bit per page id, 1 = allocated, addressed by explicit shift/mask over the page body. One headered free-map page covers `(8192 − 32) × 8 = 65,280` pages (~510 MiB of data file). The map is **region-based**: region N covers ids `[N × 65,280, (N + 1) × 65,280)`, and its two bitmaps — the free map and the headerless map (§1) — are the first two ids inside it, so locating the bitmap for a page_id is arithmetic, not lookup, up to the 2^31-page ceiling. Region 0's pair is pages 1 and 2, beside the superblock.
- **Extent = 64 pages** (`kDefaultExtentPages`) — the unit of file growth and of the per-core extent lease (`include/kds/storage/extent_lease.hpp`).
- **The free map is not logged.** `ALLOC` and `FREE` are reserved in the WAL record enum and emitted by nothing; recovery repairs the map by raising the allocation floor (`RaiseAllocationFloor`). A crash between extent growth and first use is benign (§14). Nothing frees a page.
- A page the map calls allocated that the device holds as all zeros reads `NotFound`, not `Corruption` (§10).

## 5a. The var-heap page class

`kVarHeap` (`page_type = 10`, `include/kds/storage/varheap.hpp`): the out-of-line store for values too long for a tuple's fixed-width tagged cell (`docs/spec/heap-and-tuple.md` §3.4).

- **Headered, checksummed, logged** — an ordinary authoritative page class, with a `page_lsn` and a `VARHEAP_APPEND` record (`wal.md` §5.2). Waystone pages and the trail directory are *advisory*, and those rules must not be pattern-matched onto this one: losing a var-heap value loses a committed value, not a hint.
- **Layout:** the same slotted shape as a heap page — common header, an 8-byte page header (`flags`, `nr_slots`, `lower`, `upper`), a slot directory of `{offset u16, length u16}` growing down, values growing up from the tail `next_page_id` reservation. What it is *not* is a heap page: no MVCC tuple header, no delete-mark, no slot retirement, because a value has no lifetime of its own — it lives and dies with the version pointing at it.
- **Chain:** one per relation, rooted at `sys.tables.varheap_page_id` and allocated at `CREATE TABLE` for any schema that can spill, grown by tail append. The root never moves, which is what keeps it a cacheable fact (`catalog_cache.hpp`'s rule). No `min_key` and no ordering: values are reached only through the pointers in the tuples that own them, so a walk is never a search.
- **Relayout-exempt by construction.** Values are immutable per version, so the physical optimizer has no reason to touch a `kVarHeap` page and must not.
- **Max value = 8144 bytes**, one page's worth. A larger value would need a multi-page representation, so it is refused with `Unsupported` rather than answered by inventing one.
- **Release:** a rollback releases its own appends (`VARHEAP_RELEASE`, `heap-and-tuple.md` §3.4). A superseded or deleted version's value is not released; its bytes stay, so churn-heavy string updates consume space.

## 6. Per-Core Buffer Pools

- One `DevicePageStore` per core (`include/kds/server/core_runtime.hpp`), caching only pages that core owns. Pin counts and frame state are plain non-atomic fields; multi-core adds instances, not synchronization — **with one declared exception, the page latch word** (next bullet), landed by AR0 M1's AM-S1 ahead of the shared pool AM-S2 builds.
- **The page latch** (`include/kds/storage/page_latch.hpp`; `rules.md` §3's row): every resident frame carries one 32-bit word — an exclusive bit, the owning core, a count — operated on by CAS through `std::atomic_ref`, and **armed only at `cores > 1`** from the superblock's core count; an unarmed store never touches it. Taken with the pin and released with it: `Get` and the `Create*` accessors hold a frame exclusive, `GetForRead` shared. Re-entrant for the owning **core** — a task may hold one page twice, and shared under its own exclusive — where "the owning core is the running task" is a discipline, not a gate: no task parks holding a pin, the suspend audit *records* a violation in debug builds only (`sched/coro.hpp`'s `NoteSuspension`, not fatal), and a park under a latch would be a silent second exclusive grant to the next task on that core; a per-task owner is AM-S2's escalation if the audit ever records one. **Never upgraded**: a shared holder asking for exclusive is a self-deadlock, aborted in debug by a check that reads the store's own `pins != 0` as "the shares are mine" — a proxy sound only while pools are per core — and an unbounded spin on the reactor thread in release. Acquisition order: **outer** to the WAL stream latch — a task appends while holding its page latches, and no WAL path asks for a page latch while holding the stream latch (recovery's redo is the one place `wal/` touches a frame at all, and it runs on the mount thread holding no stream latch) — never nested with the visibility window latch, never held across a park; on the fault path a task may hold other frames latched while a writeback it triggered waits on the WAL gate, which is sound (the writer thread takes no page latch) and a latency cost AM-S3 prices. Page against page is **unordered through M1** — two page latches are held on every split and chain growth (a descent holds one at a time), and one core owns its pool, so no two holders can wait on each other — and AM-S2 owes the order for the shared pool (the tree's own shapes are parent before child, old before new), and with it three more items this stage left behind: the self-deadlock check's `pins` proxy above; a re-validation after the re-fetch the two S-then-X fixes in `btree.cpp` and `index_tree.cpp` introduced (each decides on a read handle and acts after dropping it, sound only while one core owns its pool); and starvation — waits spin, then yield, with no queue and no writer preference, so steady shared holders of a hot page can starve an exclusive request, inert through M1. Through M1 the pools stay per core and the latch is inert in production; the primitive, its order and its cells are AM-S1's, and `device_page_store.hpp`'s header carries the same statement.
- Cross-core page access does not exist: work moves to the owning core over the message interface (rules.md §3).
- Ownership: a relation's pages belong to the core `sys.tables.owner_core` names (`docs/spec/crosscore.md` CC7); a core writes only pages inside an extent it was granted or leased; a page changes hands only through the logged handoff of `docs/spec/page-lsn-cross-stream.md` §9. **The pools stay per core under one WAL stream**: the log is shared, the frames are not (`wal.md` §3).
- **The device under them is shared, and that is declared** (`rules.md` §3): one `PageDevice` serves every core's store, and core 0 alone grows it. A reader sees a capacity that only rises, which is what makes an unsynchronized `uint32_t` sound here and would not survive a shrink.

## 7. Eviction

`docs/spec/eviction.md` owns replacement. What runs:

- **CLOCK** (second-chance) over unpinned frames, with a per-frame usage counter capped at `kClockUsageCap` = 5; the sweep decrements and reclaims at zero.
- The sweep runs **inline on the fault path** whenever a fault pushes residency past the `buffer_pool_frames` budget (0 = unbounded). The page just faulted is never its own victim. There is no background sweep and no background writer.
- **Clean-preferred:** a dirty frame at usage zero is queued for writeback, never reclaimed before it is written clean under §8.
- **Resident classes** are pinned by class, never candidates: the fixed system pages below the resident limit, the catalog's fixed pages, and Bound Cabin pages (`eviction.md` EV3). A pinned frame is never reclaimed at any budget.
- Bulk scans in the background group run through a small dedicated ring of frames that does not bump usage counters (`eviction.md` EV6).
- Exhaustion — every frame pinned or un-flushable — is `eviction.md` EV8's bounded retry then a truthful error, never a wait.

## 8. Dirty Tracking, Flush & Checkpoint Integration

The pool enforces the WAL contract in code, not by caller discipline:

- Each frame tracks `dirty`, `recLSN` (LSN when first dirtied since last clean), and a `page_lsn` mirror. A mutation path appends its record, then `StampPageLsn()` with the record's LSN, which records the `page_lsn` the gate reads and captures the frame's `recLSN`; the frame keeps that value until it is written back.
- `SetWalGate()` installs a `WalDurability`, and every write path (`Flush`, `Sync`, `FlushPages`) first calls `EnsureDurable()` on the highest `page_lsn` among the pages it is about to write — wal.md §8-1 is unbypassable. One call per flush batch, not per page. With no gate installed the store writes ungated, which is sound only for a caller that logs nothing (WAL-free unit tests, the simulator).
- **One writeback path.** `WriteBack()` — durable → checksum → write → clean — serves `Flush()`, the checkpointer's `FlushPages()` and the dirty-eviction-queue drain; the checkpointer is a consumer of the writeback machinery, not a parallel implementation. Ascending contiguous runs are coalesced into one `WritePageRun` of at most `kWritebackRunPages` = 8 (64 KiB), best-effort, never a correctness property.
- Checksum is computed inside writeback immediately before write-out (§10); `MarkClean` exists only as the completion step of the flush path.
- The dirty table `{page_id → recLSN}` is exported for `CHECKPOINT_BEGIN` (wal.md §11).

## 9. Frame Memory

Frames are separate heap allocations; there is no preallocated slab and no O_DIRECT. A coalesced write-out run (§8) is a bounded copy into scratch.

## 10. Checksums

- CRC32C (hardware-accelerated where available — SSE4.2 `crc32` on x86-64; the software fallback is a correctness twin used by the deterministic sim, `include/kds/storage/crc32c.hpp`); field per §2; scope: all headered pages; computed at flush (§8), verified on every load from disk — never on buffer hits.
- Verification failure ⇒ `Status(DataCorruption)`; during recovery, a checksum-failed page with an available `FULL_PAGE_IMAGE` is restored from it (wal.md §10) — checksum *detects*, FPI *heals*.
- **A never-written page is not a checksum failure.** A page the free map calls allocated that the device cannot address or holds as all zeros reads `NotFound` ("allocated but was never written"), not `Corruption` — the same reading `DevicePageStore::Open` gives an all-zero free map. The state is ordinary because extents are reserved ahead of their pages (a peer's lease is allocated whole in the map core 0 flushes; the peer writes lazily), and the distinction is what recovery needs: redo *creates* a page from `NotFound` under a `PAGE_INIT`, where a `Corruption` it could only poison and wait for an FPI. `CreateAt` accepts such an id after proving the device holds nothing. A torn page with a zero header and a nonzero body is not all zero and still fails verification. One case is refused rather than healed: a page that was flushed and later zeroed whole by device damage, whose first in-range record is ordinary and whose `FULL_PAGE_IMAGE` follows, is refused at redo — reachable only by damage between a checkpoint and a crash.
- Headerless pages are exempt by class (§1).

## 11. Configuration & Observability

- Config: `buffer_pool_frames` (instance-wide, divided across cores, 0 = unbounded; a value below `cores` is refused at startup), the device's extent size (§5), checkpoint cadence (owned by wal.md), resident-class list (build-time).
- Counters per store: live pins and the pin high-water (`live_pins()`, `pin_high_water()`), resident pages. A failed checksum verify is logged at Error and names corruption; allocation and write-back are logged at Trace, batch write-back and sync at Debug, every device-level failure at Error.

## 12. Read Path

- **Hit path:** a probe of the core-local frame table (`std::unordered_map<PageId, Frame>`), a pin increment and a usage-counter bump. No locks, no atomics (core-local), no syscalls.
- **Miss path:** a synchronous `pread` through the `PageDevice` seam (`FilePageDevice`; `MemoryPageDevice` under the simulator, with fault injection), then checksum verification (§10) — the hit path never touches it.
- There is no prefetch, no read coalescing and no asynchronous submission.

## 13. Write Path

- Every write-out runs through §8's one `WriteBack()` path and passes the WAL gate; commit latency itself is WAL group-commit territory (wal.md §6), not this path.
- Write-out is driven by `Flush()`, the checkpointer's `FlushPages()` and the drain of the dirty-eviction queue. There is no background writer.
- Batches coalesce ascending contiguous runs (§8); the single-file arithmetic mapping (§4) makes id-order literally file-order.

## 14. Growth

The file is growable to the 16 TiB ceiling:

- Growth unit is the extent (§5). `FilePageDevice` grows by `extent_pages` via `posix_fallocate` — real block reservation, not a size change — and a file whose size is not a whole number of pages is `Corruption` rather than rounded. Extension is idempotent on replay.
- The free map is unlogged (§5), so no `ALLOC` record precedes an extension; a crash between extension and first use leaves pages the allocation floor re-absorbs at recovery.
- Shrinking/truncation is a **non-goal**; nothing frees a page and nothing returns space to the filesystem.

## 15. mmap Evaluation — Rejected for Data & WAL

mmap was evaluated as the paging mechanism (map the single file, let the kernel page it). It is attractive on the surface — the arithmetic single-file layout is exactly mmap-shaped, and the kernel page cache comes free. It is rejected, for reasons that are well documented across DBMS engineering literature and are *worse* than usual under KDS's architecture:

1. **Write-ordering control is lost.** The kernel may flush a dirty mapped page at any moment. That silently violates WAL-before-data (§8, wal.md §8-1) and destroys torn-page/FPI guarantees — `msync` granularity and timing cannot reconstruct the ordering contract. This alone is disqualifying for a durability-bearing engine.
2. **Page faults stall the whole core.** A fault blocks the faulting *thread*. In a thread-per-core cooperative reactor, that is not one slow query — it freezes every task on the core for an unbounded device-latency window, gutting the SLO model (docs/spec/sched.md).
3. **Error handling is incompatible.** I/O errors surface as `SIGBUS` mid-instruction — irreconcilable with the no-exceptions/`Status` error rules and with any notion of a clean failure path.
4. **It breaks deterministic testing outright.** Kernel paging cannot be injected, scheduled, or fault-injected through the `PageDevice` seam; rules.md §4 (whole-engine simulation with torn-write injection) would be unenforceable. In KDS this is not a nice-to-have — it is how every guarantee in wal.md §16 is proven.
5. **Performance at scale is worse, not better:** TLB shootdown storms on eviction, kernel reclaim contention, 4 KiB kernel granularity vs 8 KiB engine pages, and no interposition point for checksums (§10) or the flush gate (§8).

**Verdict:** explicit per-core buffer pool with seam-injected I/O (decision S11). mmap may appear in offline tooling (e.g. read-only backup inspection utilities) but never inside the engine's data or WAL paths.

## 16. Required Amendments

The amendments this section gated are applied: the common header (§2), the headerless-class exemption (`docs/spec/waystone-concpets.md` §5–6), the WAL cross-references (`docs/spec/wal.md`) and the `PageRef` contract (§3). The frame table is a hash map (§12).

## 17. Open Decisions

The decisions this section once listed are not recorded here. Each section above states what holds today; a rule this file does not state is not made.

## 18. Testing Requirements

1. **Header codec:** round-trips; offset asserts; unknown `page_type` ⇒ error; version-bump gate.
2. **PageRef:** pin balance by construction; span validity bounded by ref lifetime under eviction pressure; ref-across-suspension metering.
3. **Eviction & residency:** pinned never evicted; resident classes never candidates; clean-preferred order; the full suite runs under a tiny frame budget (`KDS_TEST_FRAME_BUDGET` in debug builds) so every run exercises the sweep.
4. **WAL gate:** scripted `WalDurability` stub proves `Flush` waits for `durable_lsn ≥ page_lsn` under randomized deterministic schedules; `MarkClean` unreachable outside flush completion.
5. **Checksum:** flush-computed, load-verified, hit path untouched (instrumented); injected corruption ⇒ `DataCorruption`; FPI restoration with wal.md's crash matrix; hardware/software CRC32C equivalence.
6. **Write path:** flush batches are id-sorted and coalesced (assert on the I/O trace).
7. **Growth:** extension idempotent on replay; 16 TiB ceiling asserted.
8. **Single-file & per-core:** arithmetic mapping property tests; sparse growth; two pools over disjoint ownership run the suite with zero shared state.
