# KDS Design Specification — Heap & Tuple

The authoritative specification for how KDS stores a row. Companion specs own the layers above and beside it: `waystone-concpets.md` (pattern-keyed access trails), `txn.md` (transactions and MVCC), `wal.md` (logging and recovery), `page.md` (page management and buffering), `parser.md`, `protocol.md`, `rules.md` (C++ rules), `sched.md`.

`[OPEN]` marks a decision that has not been made. Implementers must not assume one; either ask, or build behind an interface that keeps every option viable.

---

## 1. What KDS Is

An **OLTP-specialized database storage engine** for financial systems, written as userspace C++. It is not a general-purpose DBMS and its feature scope is deliberately narrow.

Two mechanisms differentiate it, and both are engine-native rather than optimizer tricks:

- **Statistics drive physical placement.** Runtime access data reorganizes where tuples physically live, not merely how a query is planned.
- **Waystone** records where a previous execution of a query pattern found its rows, so a repeated pattern can look directly instead of searching.

## 2. Pages

- Page size is **8192 bytes**.
- Page ids are **unsigned 32-bit**. Capacity 16 TB = 2^31 pages, half the `uint32_t` space. `0xFFFFFFFF` is `kInvalidPageId`. Page ids are never stored in a signed type — 2^31 overflows `int32_t`.
- Status flags are never packed into a page-id field. Status bits get their own field.

## 3. Heap Organization

### 3.1 Semi-sorted heap

Each heap page header carries status flags and an **immutable `min_key`**, fixed when the page is created.

- **No tuple whose pk is below a page's `min_key` may ever be placed in it.** This holds for relayout as much as for insert.
- **Tuples within a page are unordered** — heap append semantics, O(1) insert into free space.
- Because `min_key` never changes, a reader can prune pages by key range **without locking**. That is the property the immutability exists to buy.
- Relayout honors the target page's `min_key`. Moving tuples across key ranges means writing them into **new pages with newly assigned `min_key` values**, never mutating an existing page's.

### 3.1a Per-page epoch counter

Every heap page header carries an **epoch counter**, bumped whenever the tuples physically on that page move. Unlike `min_key`, it is mutable by design.

Waystone records a page's epoch when it observes a tuple's location, and a consumer trusts that location only while the recorded epoch still matches the page's. This is how an advisory structure avoids becoming a second authoritative index: relayout bumps one counter instead of synchronously rewriting every entry that pointed into the page.

`[OPEN]` — where the epoch lives (a header field, which makes it on-disk format under `rules.md` §5, versus a core-local table keyed by page id), and its width and wraparound handling.

### 3.1b Chain growth by tail append

A relation is a **chain of heap pages** linked through the `next_page_id` tail reservation (§3.2), rooted at `sys.tables.desc_page_id` (`include/kds/storage/heap/heap_chain.hpp`).

- **Growth is tail append, never a split.** Every insert goes to the last page. When it has no room, a new page is allocated, the tuple is written into it, and only then is the page linked on — the link is what makes a page reachable, so publishing it first would expose an empty tail.
- **A new page's `min_key` is the id of the tuple that caused the growth**, the smallest id it can ever hold, since ids only increase (§4). No existing page's `min_key` is touched, so §3.1's immutability holds by construction.
- **Each page's ids lie entirely below the next page's `min_key`.** The chain is key-ordered page by page while tuples within a page stay unordered — "semi-sorted" holds across pages as well as within one. Two consequences are relied on in code: a duplicate incoming id can only be in the tail page, so the check is O(1) pages rather than O(chain); and an id below the tail's `min_key` has nowhere legal to go and is refused as a backwards sequence.
- **No free-space reuse.** A page that fills and then has rows deleted is never revisited; the chain only grows at the tail. A delete-heavy relation grows monotonically.
- Walks are bounded by `kMaxChainPages` (2^20 pages, 8 GiB per relation). Exceeding it is `Corruption` rather than a loop, since a cycle in the links would otherwise hang a request.

`[OPEN]` — the **heap page split policy**: dividing a full page's contents and choosing the new boundary. Tail append deliberately does not decide it, because it never moves a tuple off a page or assigns a `min_key` to a page that already holds tuples. Page compaction and free-space reuse are open with it, and both need the transaction manager to answer "does any snapshot still need these bytes" — which it cannot today, because readers are deliberately not registered (`txn.md` §4.1). Reader registration is the prerequisite.

### 3.2 Page layout

- The slot directory grows downward from the heap area offset; tuple data grows upward from the top; free space is the gap (`upper - lower`).
- The page tail permanently reserves `sizeof(PageId)` bytes for the `next_page_id` chain link, excluded from free-space accounting.
- The per-tuple MVCC header is **`trx_id` (48-bit writer, zero-extended to 8 bytes) + `undo_ptr` + `data_len` + flags — 20 bytes, with no `xmax`**. A version's death is the next version's birth: walking the undo chain already names the overwriting transaction, so storing that boundary a second time in the older version would be recording one fact twice. `trx_id` is whichever transaction last stamped the version — insert, overwrite, or delete-mark. The lock-slot role `xmax` plays in PostgreSQL belongs to the Keystone flags byte here (§4).
- **DELETE is a delete-mark**: the slot's `DELETED` flag plus the deleter's `trx_id`, with the tuple bytes left in place for snapshots that predate it. Physical reclamation is slot retirement (`DEAD`), a separate operation for a purge pass — hence two WAL records, `HEAP_DELETE_MARK` and `SLOT_RETIRE`.
- Slot entries carry their own `flags` (`DEAD`, `DELETED`) and `length`. Retirement marks a slot dead rather than compacting eagerly.

## 4. Keystone Column

Every tuple's **first column is mandatory**: one 64-bit word, the *Keystone word*. This is a self-imposed constraint of KDS and the tuple's identity lives in it.

| Field | Width | Purpose |
|---|---|---|
| `id` | 40 bits | Primary key. Per-relation capacity ≈ 1.1 × 10^12 issued ids. |
| `flags` | 8 bits | Transaction/status byte, Oracle lock-byte style; may reference a per-page transaction slot. Tuple status such as `DEAD` lives in the slot directory, not here. |
| `reserved` | 16 bits | Writers set 0, readers ignore. Repurposing is `[OPEN]`. |

**Every relation requires system-generated, autoincrement `id` values.** A caller-supplied pk on insert is a defect, not a feature. The id is the tuple's *identity*, and an identity the client chooses is one the client can duplicate, reuse, or run backwards.

- The sequence is **persistent, not derived**: `sys.tables.next_id`, issued by `Catalog::AllocateRowId()`. Deriving it as `max(id) + 1` would reissue an id after the highest tuple is deleted, handing a new tuple the identity of a retired one. The first id issued is 1; 0 stays reserved for "unset".
- Ids are unique and monotonic by construction, **not gapless** — an insert that fails after allocating burns one. Nothing depends on gaplessness.
- The pk is carried **only** by the Keystone word, never also as a body column: `EncodeRow()` writes `[Keystone word][columns 1..n-1]`, and `INSERT` supplies values for columns 1..n-1 only. Storing a key twice is how the two copies come to disagree.
- The pk **cannot be updated**. It is the tuple's identity, not a field of it.
- A relation's first column must be declared with an **integer type** (`catalog::CheckKeystoneColumn`), checked at `CREATE TABLE`. Its declared width is display metadata: the id lives in the 40-bit Keystone field regardless, so a narrow declared type does not cap the sequence.

Implementation rules:

- Encode and decode with **explicit shift/mask helpers only**. **Never use C/C++ bitfields** for an on-disk format — their layout is implementation-defined and KDS must be portable across architectures.
- The whole word is updated with **atomic `uint64_t` operations (CAS)**. Fields must never tear across writes.
- External structures — B+ tree keys, `min_key`, Waystone entries — store the id as a **zero-extended `uint64_t`** with the upper 24 bits zero. Ids are never 5-byte-packed.

`[OPEN]` — id-reuse and low-range reclamation policy. Sequence exhaustion is reported as `OutOfRange`, never wrapped.

## 5. Indexing

- A relation is stored either as a **heap chain** (§3.1b) or as a **clustered B+ tree** on the Keystone pk, chosen at `CREATE TABLE`. On a btree relation the tree *is* the storage, and a descent is authoritative: a miss means the row does not exist, and no scan follows. A heap relation has no pk index at all, so a point lookup scans the chain.
- A btree **leaf is a heap page** — same slot directory, same tuple format, same MVCC header, same `min_key` and `next_page_id`. A clustered-btree relation is therefore not a second storage engine; it is the heap with a directory over it.
- **Waystone** (`waystone-concpets.md`) is the engine's other access structure: `(pattern_id, arg_hash)` → the Keystones a previous execution of that pattern instance found, across relations. It is advisory and validated on use, and it may replace a *lookup* but never a *search*.

## 6. Page-Latch Consistency

There is no single canonical in-memory tuple and no hash table enforcing that an identical tuple exists at most once in program memory. Consistency is kept at the **page** level.

- A page frame is **pinned** for the duration of any access and **latched** — shared for reads, exclusive for structural mutation (slot directory changes, compaction, relayout). Tuple bytes are read and written directly within the pinned, latched frame; there is no tuple-identity cache to keep coherent with the page.
- Latching is **core-local**, consistent with thread-per-core/shared-nothing (`rules.md` §3): a page is owned by exactly one core, and its latch serializes cooperative tasks on that core across suspension points. It is not a cross-core lock — cross-core access goes through server-side forwarding (`protocol.md`), never shared-memory locking.
- Executors may copy tuple bytes into private working buffers. These are ephemeral projections; they compete with no canonical copy, because there isn't one.
- The Keystone word's atomic-CAS requirement (§4) is independent of latching: even under a latch, the word is read and written as a `std::atomic<uint64_t>` so fields never tear.

`[OPEN]` — buffer-pool page-frame reclamation policy (pin refcount versus epoch-based eviction) under this model.

## 7. Statistics-Driven Physical Relayout

KDS collects access statistics and uses them to **physically optimize tuple placement**, starting with heap pages.

Relayout must respect the `min_key` insertion rule (§3.1), keep the B+ tree consistent (entries updated or lazily repaired for moved tuples), and bump the page epoch (§3.1a) so every recorded location on that page becomes untrusted at once.

Key-boundary re-partitioning mainly benefits range locality; for single-pk point lookups the acceleration comes from Waystone instead. The two coexist and address different shapes.

*Nothing here is implemented — there is no physical optimizer, and consequently nothing bumps a page epoch.*

## 8. Invariants

Never violated, never "temporarily" bypassed.

1. Page size is 8192 bytes; page ids are `uint32_t`; `0xFFFFFFFF` is reserved as invalid.
2. A heap page's `min_key` is immutable after creation.
3. No tuple with `id < min_key(page)` is ever placed in that page, including by relayout.
4. Tuples within a heap page are unordered.
5. The Keystone column is exactly `id:40 | flags:8 | reserved:16`.
6. The Keystone word is read and written atomically as a `uint64_t`; on-disk encoding uses explicit shift/mask, never compiler bitfields.
7. Ids stored outside the tuple header are zero-extended `uint64_t` with the upper 24 bits zero.
8. Waystone is advisory: deleting it wholesale may cost performance and must never change a query result.
9. Waystone is never **authoritative**. A reader may consult it for *where to look*, provided it treats a missing or stale entry as a miss, checks the Keystone id of the tuple actually found at the reported location, applies MVCC visibility exactly as the authoritative path would, and falls through to that path — a btree descent on a btree relation, a chain scan on a heap one — on any mismatch. It chooses where to look, never what is visible.
10. No single canonical in-memory tuple is enforced; consistency comes from page pin and latch discipline (§6).
11. Every relation requires system-generated, autoincrement `id` values; a caller-supplied pk on insert is a defect (§4).
12. The tuple MVCC header is exactly `trx_id:48 (zero-extended to 64) | undo_ptr | data_len | flags` = 20 bytes. There is no `xmax`; a version's validity interval is reconstructed from the undo chain, and DELETE is the slot's `DELETED` mark plus the deleter's `trx_id`.

## 9. Open Decisions

Collected from the sections above, plus those owned by companion specs.

- Per-page epoch storage location, width, and wraparound (§3.1a).
- Heap page split policy; free-space reuse and page compaction, both gated on reader registration (§3.1b).
- Repurposing of the 16 reserved Keystone bits (§4).
- Id-reuse / low-range reclamation for high-churn relations (§4).
- Buffer-pool page-frame reclamation policy (§6).
- I/O backend abstraction: plain `O_DIRECT` versus `io_uring` versus pluggable.
- Waystone's own open items — retention and eviction, recording policy, page persistence class, `arg_hash` collision handling, and whether invariant 9 is ever amended to permit trusting a cached result set as complete (`waystone-concpets.md` §9).
- Undo retention and `SnapshotTooOld` surfacing; 48-bit `trx_id` wraparound; cross-core commit protocol (`txn.md`, `wal.md`).
