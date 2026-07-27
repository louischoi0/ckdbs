# KDS Design Specification

**Status:** Living document — input for development agents. Sections marked `[CONFIRMED]` are settled design; `[OPEN]` items must not be assumed by implementers.
**Last updated:** 2026-07-27

---

## 1. Project Direction `[CONFIRMED]`

- KDS is an **OLTP-specialized database storage engine** targeting high performance for financial systems.
- **Kernel integration is on hold.** KDS is being redesigned as ordinary **userspace software**. Existing kernel-module code (C, Linux kernel style) serves as the reference/porting base.
- Differentiation strategy: runtime **statistics drive physical data placement** (not merely optimizer decisions), plus an **automatic hint index** for repeated query patterns. Feature scope is deliberately narrower than general-purpose DBMSs; the focus is speed.

## 2. Pages `[CONFIRMED]`

- Page size: **8 KB (8192 bytes)**.
- Page ID: **unsigned 32-bit** (`u32`). Target capacity 16 TB = 2^31 pages, using exactly half the u32 space. `0xFFFFFFFF` is available as `INVALID_PAGE_ID`. Page IDs must never be stored in signed types (2^31 overflows `s32`).
- Do **not** pack status flags into page-ID fields; status bits live in their own fields.

## 3. Heap Organization `[CONFIRMED]`

### 3.1 Semi-sorted heap with immutable `min_key`

- Heap pages form a **semi-sorted heap**: each heap page header carries status flags and an **immutable `min_key`** fixed at page creation.
- **Insertion rule (invariant):** no tuple whose PK is below the page's `min_key` may ever be inserted into that page.
- **Within a page, tuples are unordered** (normal heap append semantics; O(1) insert into free space).
- Because `min_key` never changes, readers can prune pages by key range **without locking**.
- Physical relayout must always honor the target page's `min_key` bound. Relocation across key ranges is done by writing tuples into **new pages with newly assigned `min_key` values**, never by mutating an existing page's `min_key`.

### 3.2 Page layout (carried over from existing implementation)

- Slot directory grows downward from the heap area offset; tuple data grows upward from the top; free space is the gap (`upper - lower`).
- The page tail permanently reserves `sizeof(page_id)` bytes for a `next_page_id` chain link, excluded from free-space accounting.
- Per-tuple MVCC header: `xmin`, `xmax`, `undo_ptr`, `data_len`, flags.
- Slot entries carry their own `flags` (e.g., `DEAD`) and `length`; retirement marks the slot dead rather than compacting eagerly.

## 4. Keystone Column — 64-bit Tuple Header Word `[CONFIRMED]`

Every tuple's **first column is mandatory**: a single 64-bit word (the "Keystone column" / "Keystone word"). This is a self-imposed constraint of KDS.

Bit layout (one `u64`):

| Field | Width | Purpose |
|---|---|---|
| `id` | 40 bits | Tuple primary key. Per-relation capacity ≈ 1.1 × 10^12 issued IDs. |
| `flags` | 8 bits | Transaction/status byte (Oracle lock-byte style; may reference a per-page transaction slot). Tuple-level status such as DEAD stays in the slot directory, not here. |
| `meta_handle` | 16 bits | **Temporary** identifier linking the tuple to its relation's metadata pool entry (Section 5). Not persistent in meaning. |

Implementation rules:

- Encode/decode with **explicit shift/mask** helpers only. **Never use C/C++ bitfields** for on-disk format (layout is implementation-defined; KDS must be portable across architectures).
- The whole word is updated with **atomic u64 operations (CAS)** under concurrency; fields must never be torn across writes.
- External structures (B-tree keys, `min_key`, hint index entries, metadata back-references) store the id as a **zero-extended `u64`**. Invariant: upper 24 bits are 0. Do not 5-byte-pack ids.

## 5. Per-Relation Metadata Pool `[CONFIRMED]`

Purpose: metadata for **physical relayout and statistics only** — never on the normal read path. Normal reads go through the B+ tree (Section 6).

- Each relation owns a metadata pool tracking **up to 65,536 tuples** (addressed by the 16-bit `meta_handle`).
- Pool entries are **fixed-length records** stored in standard **8 KB pages**, so a handle resolves to its entry with **O(1) arithmetic** (`page = handle >> k`, `slot = handle & mask`). Choose entries-per-page as a **power of two**; accept per-page slack rather than splitting entries across page boundaries.
- The pool is subject to **eviction**: only the current/hot subset of tuples has live metadata at any time. Entries record where a tuple is located (`page_id`), how it is referenced, and when.
- Metadata content is **transient and rebuildable**; it must never be required for correctness of query results.

## 6. Indexing `[CONFIRMED]`

- A **B+ tree** is the upper layer over heap pages and is the authoritative access path for normal reads.
- **Hint index** (new structure): key = `(query_pattern_id, argument values)` → the set of tuple locations previously read by that pattern, **across tables** (e.g., pattern A with `a=1` maps to tuple x in `table0` and tuple y in `table1`).
  - `query_pattern_id` identifies *identical queries*, not only stored procedures: queries are fingerprinted by normalized structure (FROM clause, join pattern, constants parameterized).
  - End goal: KDS **automatically creates and manages hint indexes** from observed query patterns. Zero user administration.
  - Hint entries are advisory: a consumer must validate the target tuple (PK identity + MVCC visibility) and fall back to the B+ tree on mismatch. Hints may be stale; they may never cause wrong results.

## 7. In-Memory Single-Copy Rule `[CONFIRMED]`

- An identical tuple is kept **at most once** in program memory at all times, managed through a hash-table-based lookup. This is a defensive design for data integrity: all in-memory references converge on the single canonical copy.

## 8. Statistics-Driven Physical Relayout `[CONFIRMED — direction]`

- KDS collects access statistics (via the metadata pool) and uses them to **physically optimize tuple placement**, starting with heap pages and expanding later.
- Relayout must respect: the `min_key` insertion invariant (3.1), index consistency (B+ tree entries updated or lazily repaired for moved tuples), and the advisory nature of hint entries (6).
- Rationale accepted during design: key-boundary re-partitioning mainly benefits range locality; for single-PK point lookups the hint index (6) is the primary latency weapon. Both mechanisms coexist.

## 9. Open Items `[OPEN — do not assume]`

- **Implementation language:** C vs C++ (userspace). Undecided.
- Metadata pool **eviction policy** (LRU vs frequency-based) and **invalidation mechanism** on eviction (clear-on-evict vs validate-on-use).
- **Persistence class of metadata pages** (WAL-logged vs unlogged/no-checkpoint).
- Heap **page split policy** when a page is full (how new `min_key` boundaries are chosen).
- Hint index **admission/eviction policy** and safety classification per query template (trusted for unique lookups vs prefetch-only).
- MVCC versioning semantics of the single-copy rule (identity per version vs per tuple) and memory reclamation scheme.
- Relations exceeding practical id issuance (per-relation u64 opt-in or history-table policy).

## 10. Invariant Summary (for implementers)

1. Page size is 8192 bytes; page IDs are unsigned 32-bit; `0xFFFFFFFF` reserved as invalid.
2. A heap page's `min_key` is immutable after creation.
3. No tuple with `id < min_key(page)` is ever placed in that page — including by relayout.
4. Tuples within a heap page are unordered.
5. Every tuple begins with the 64-bit Keystone column: `id:40 | flags:8 | meta_handle:16`.
6. The super-column word is read/written atomically as a `u64`; on-disk encoding uses explicit shift/mask, never compiler bitfields.
7. Ids are stored externally as zero-extended `u64` (upper 24 bits = 0).
8. The metadata pool and hint index are advisory: losing either entirely must not affect query correctness — only performance.
9. Normal reads are served by the B+ tree; metadata pages are never on the read path.
10. At most one in-memory copy of an identical tuple exists at any time.

---

*Maintenance note: when a `[OPEN]` item is decided, move it into the appropriate `[CONFIRMED]` section and record the date.*
