# KDS Design Specification

**Status:** Living document — input for development agents. Sections marked `[CONFIRMED]` are settled design; `[OPEN]` items must not be assumed by implementers. This file is the design spec CLAUDE.md and other docs refer to as "KDS-DESIGN.md" — that name is historical (pre-dates the move into `docs/`); this path is the actual, current location.
**Last updated:** 2026-07-27 (Keystone layout and metadata-pool sections amended 2026-07-28 — see §4, §5; in-memory single-copy rule retired 2026-07-28 in favor of page-latch consistency — see §7; tuple MVCC header amended 2026-07-29 to `trx_id` + `undo_ptr`, `xmax` removed — see §3.2)

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

### 3.1a Per-page epoch counter `[CONFIRMED, amended 2026-07-28]`

- Every heap page header carries an **epoch counter**, bumped whenever the tuples physically on that page move (relayout, page rebuild). Unlike `min_key`, the epoch is mutable by design.
- Purpose: Waystone (`docs/waystone-concpets.md` §3) records the page epoch at the moment it observes a tuple's location; a consumer trusts that location only while `entry.page_epoch == page's current epoch`. This is how an advisory structure avoids being a second authoritative index — no synchronous double-write on relayout, just an epoch bump.
- Storage location `[OPEN]`: header field (on-disk format, follows §5 of `docs/rules.md`) vs. a core-local epoch table keyed by `page_id`. Epoch width and wraparound handling are also `[OPEN]`. Do not assume either.

### 3.1b Chain growth by tail append `[CONFIRMED 2026-07-29]`

A relation is a **chain of heap pages** linked through the `next_page_id` tail reservation (§3.2), rooted at `sys.tables.desc_page_id`. Implemented in `include/kds/storage/heap/heap_chain.hpp`; before it, a relation was one page and an INSERT failed with `OutOfSpace` once 8 KB of tuples had landed.

- **Growth is tail append, never a split.** Every insert goes to the last page in the chain. When that page has no room, a new page is allocated, the tuple is written into it, and only then is the new page linked on — the link is what makes the page reachable, so publishing it before the tuple would expose an empty tail.
- **A new page's `min_key` is the id of the tuple that caused the growth** — the smallest id it can ever hold, since ids only increase (§4). No existing page's `min_key` is touched, so §3.1's immutability holds by construction.
- **Ordering property.** Because ids increase and every insert appends, each page's ids lie entirely below the next page's `min_key`. The chain is therefore key-ordered page by page, while tuples *within* a page stay unordered — the "semi-sorted" of §3.1 now holds across pages as well as within one. Two consequences are relied on in code: a duplicate of an incoming id can only be in the tail page (so the check is O(1) pages, not O(chain)), and an id below the tail's `min_key` cannot be placed anywhere legally and is refused as a backwards id sequence.
- **This does not decide the split policy.** Dividing a full page's contents and choosing a new boundary — the `[OPEN]` item in `CLAUDE.md` — is untouched: nothing here ever moves a tuple off a page or assigns a `min_key` to a page that already holds tuples. A split policy lands beside this, and pages produced by tail append stay valid under it.
- **No free-space reuse.** A page that fills and then has rows deleted is never revisited; the chain only grows at the tail. Reclaiming that space needs page compaction, which needs a transaction manager to know no snapshot still needs the bytes. A delete-heavy relation therefore grows monotonically. *(Update 2026-07-31: a transaction manager now exists (`docs/txn.md`), but it deliberately does **not** register readers (`docs/txn.md` §4.1), so it still cannot answer "does any snapshot need these bytes". Compaction stays open, and reader registration is the prerequisite.)*
- Walks are bounded by `kMaxChainPages` (2^20 pages = 8 GiB per relation); exceeding it is reported as `Corruption` rather than looping, since a cycle in the links would otherwise hang a request.

### 3.2 Page layout (carried over from existing implementation)

- Slot directory grows downward from the heap area offset; tuple data grows upward from the top; free space is the gap (`upper - lower`).
- The page tail permanently reserves `sizeof(page_id)` bytes for a `next_page_id` chain link, excluded from free-space accounting.
- Per-tuple MVCC header `[CONFIRMED, amended 2026-07-29]`: **`trx_id` (48-bit writer, zero-extended to 8 bytes) + `undo_ptr` + `data_len` + flags — 20 bytes, no `xmax`** (`docs/wal.md` §5.1, §14-1). A version's death is the next version's birth: walking the undo chain already names the overwriting transaction, so recording that boundary a second time in the older version is redundant. `trx_id` is whichever transaction last stamped the version — insert, overwrite, or delete-mark. The lock-slot role `xmax` plays in Postgres belongs to the Keystone lock byte here (§4).
- **DELETE is a delete-mark** `[CONFIRMED 2026-07-29]`: slot flag `DELETED` plus the deleter's `trx_id` in the writer field, tuple bytes left in place for snapshots that predate it. Physical reclamation is slot retirement (`DEAD`), a separate operation for a purge pass — hence two distinct WAL records, `HEAP_DELETE_MARK` and `SLOT_RETIRE`.
- Slot entries carry their own `flags` (`DEAD`, `DELETED`) and `length`; retirement marks the slot dead rather than compacting eagerly.

## 4. Keystone Column — 64-bit Tuple Header Word `[CONFIRMED, amended 2026-07-28]`

Every tuple's **first column is mandatory**: a single 64-bit word (the "Keystone column" / "Keystone word"). This is a self-imposed constraint of KDS.

Bit layout (one `u64`):

| Field | Width | Purpose |
|---|---|---|
| `id` | 40 bits | Tuple primary key. Per-relation capacity ≈ 1.1 × 10^12 issued IDs. |
| `flags` | 8 bits | Transaction/status byte (Oracle lock-byte style; may reference a per-page transaction slot). Tuple-level status such as DEAD stays in the slot directory, not here. |
| `reserved` | 16 bits | **Amendment 2026-07-28:** the former `meta_handle` field. Waystone (`docs/waystone-concpets.md` §4) addresses its per-tuple entries directly by `id`, not by a handle stored here, so this field has no current addressing purpose. Writers must set it to 0; readers must ignore it. Repurposing (e.g. a hot-tier accelerator handle) is `[OPEN]` — see spec §11. |

**Every relation** requires **system-generated, autoincrement `id` values** — callers must not supply their own `id`/pk on insert. Rationale: Waystone addressing is `entry_index = pk` (spec §4) directly off the issued id sequence; a user-supplied, non-monotonic, or reused pk would defeat the directory's dense/sparse-but-ordered growth assumption (spec §6) and could collide with an existing live entry.

**Amendment 2026-07-29 — scope widened from Waystone-enabled relations to all of them.** The rule previously bound only relations with `waystone_enabled` set, leaving a plain heap table free to keep any pk-assignment policy. That split is retired: `waystone_enabled` is a flag that can be turned *on* later, and a relation that spent its early life accepting caller-supplied pks cannot then be given a dense id-addressed structure without rewriting every key. Making the sequence universal costs a plain heap table nothing and keeps every relation eligible.

Consequences, as implemented:

- The id sequence is **persistent, not derived**: `sys.tables.next_id` (`include/kds/catalog/rows.hpp`), issued by `Catalog::AllocateRowId()`. Deriving it as `max(id) + 1` would reissue an id after the highest tuple is deleted, and a reissued id silently aliases a retired one in any structure that addresses by id. First id issued is 1; 0 stays reserved for "unset".
- Ids are unique and monotonic by construction, **not gapless** — an insert that fails after allocating burns one. Gaplessness is not a property anything depends on.
- The pk is carried **only** by the Keystone word, never also as a body column: `EncodeRow()` writes `[Keystone word][columns 1..n-1]` and `INSERT` supplies values for columns 1..n-1 only. Storing the key twice is how the two copies come to disagree.
- The pk **cannot be updated**: it is the tuple's identity, not a field of it.
- A relation's first column must be declared with an **integer type** (`catalog::CheckKeystoneColumn`), checked at `CREATE TABLE`. Its declared width is display metadata only — the id lives in the 40-bit Keystone field regardless, so a narrow declared type does not cap the sequence.
- Id-reuse / low-range reclamation remains `[OPEN]`; sequence exhaustion is reported (`OutOfRange`), never wrapped.

Implementation rules:

- Encode/decode with **explicit shift/mask** helpers only. **Never use C/C++ bitfields** for on-disk format (layout is implementation-defined; KDS must be portable across architectures).
- The whole word is updated with **atomic u64 operations (CAS)** under concurrency; fields must never be torn across writes.
- External structures (B-tree keys, `min_key`, hint index entries, metadata back-references) store the id as a **zero-extended `u64`**. Invariant: upper 24 bits are 0. Do not 5-byte-pack ids.

## 5. Per-Relation Metadata Pool — **superseded 2026-07-28**

> **Superseded by Waystone.** This bounded-pool design (65,536-entry cap, 16-bit `meta_handle` addressing, eviction management) is replaced in full by **Waystone**'s full-coverage, pk-direct model — see `docs/waystone-concpets.md`, decision recorded 2026-07-28. The `meta_handle` field is retired (§4). Kept below for history only; do not implement against this section.

Purpose (historical): metadata for **physical relayout and statistics only** — never on the normal read path. Normal reads go through the B+ tree (Section 6).

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

## 7. Page-Latch Consistency Model `[CONFIRMED, supersedes former "In-Memory Single-Copy Rule", 2026-07-28]`

- **The single canonical in-memory tuple concept is retired.** There is no hash-table-based convergence enforcing that an identical tuple exists at most once in program memory.
- Consistency is instead kept at the **page** level: a page frame is **pinned** for the duration of any access and **latched** — shared latch for reads, exclusive latch for structural mutation (slot directory changes, compaction, relayout). Tuple bytes are read/written directly within the pinned, latched frame; there is no separate tuple-identity cache to keep coherent with the page.
- Latching is **core-local**, consistent with the thread-per-core/shared-nothing rule (`docs/rules.md` §3): a page is owned by exactly one core, and its latch serializes cooperative tasks on that core across suspension points. It is not a cross-core lock — cross-core tuple access continues to go through forwarding (`docs/protocol.md`), not shared-memory locking.
- Query executors may still copy tuple bytes into private working buffers for processing (row buffers, projections, etc.). These are ephemeral and do not need to converge on, or compete with, any canonical copy — there isn't one.
- The Keystone word's atomic-CAS requirement (§4) is unaffected and applies independent of latching: even under a page latch, the 64-bit word is read/written via `std::atomic<uint64_t>` so fields never tear.

## 8. Statistics-Driven Physical Relayout `[CONFIRMED — direction]`

- KDS collects access statistics (via the metadata pool) and uses them to **physically optimize tuple placement**, starting with heap pages and expanding later.
- Relayout must respect: the `min_key` insertion invariant (3.1), index consistency (B+ tree entries updated or lazily repaired for moved tuples), and the advisory nature of hint entries (6).
- Rationale accepted during design: key-boundary re-partitioning mainly benefits range locality; for single-PK point lookups the hint index (6) is the primary latency weapon. Both mechanisms coexist.

## 9. Open Items `[OPEN — do not assume]`

- **Implementation language:** C vs C++ (userspace). Undecided.
- **Persistence class of metadata pages** (WAL-logged vs unlogged/no-checkpoint) — now framed as Waystone page persistence; see `docs/waystone-concpets.md` §11.
- Heap **page split policy** when a page is full (how new `min_key` boundaries are chosen).
- Hint index **admission/eviction policy** and safety classification per query template (trusted for unique lookups vs prefetch-only).
- ~~MVCC version identity semantics (identity per version vs per tuple)~~ — **decided 2026-07-31, `docs/txn.md` §2: identity is per logical tuple.** Forced by facts already confirmed: the pk cannot be updated (invariant 11), `OverwriteTuple` keeps `(page_id, slot)`, Waystone addresses by pk, and undo versions have no slot and therefore no address.
- Buffer-pool page-frame reclamation policy (pin refcount vs epoch-based eviction) under the page-latch model (§7).
- Relations exceeding practical id issuance (per-relation u64 opt-in or history-table policy) — now sharpened by Waystone's id-reuse/low-range reclamation open item, spec §11.
- Per-page epoch counter storage location and width/wraparound (`docs/waystone-concpets.md` §3, §11 — new as of the 2026-07-28 amendment).
- Repurposing of the freed 16 Keystone `reserved` bits (§4, new as of the 2026-07-28 amendment).

*(The former "metadata pool eviction policy" and "eviction-vs-validation invalidation" open items are removed: full-coverage Waystone has no admission/eviction to decide — see §5's supersession banner.)*

## 10. Invariant Summary (for implementers)

1. Page size is 8192 bytes; page IDs are unsigned 32-bit; `0xFFFFFFFF` reserved as invalid.
2. A heap page's `min_key` is immutable after creation.
3. No tuple with `id < min_key(page)` is ever placed in that page — including by relayout.
4. Tuples within a heap page are unordered.
5. Every tuple begins with the 64-bit Keystone column: `id:40 | flags:8 | reserved:16` (amended 2026-07-28; the field was `meta_handle:16`, now retired — see §4).
6. The super-column word is read/written atomically as a `u64`; on-disk encoding uses explicit shift/mask, never compiler bitfields.
7. Ids are stored externally as zero-extended `u64` (upper 24 bits = 0).
8. Waystone and the hint index are advisory: losing either entirely must not affect query correctness — only performance.
9. **(Amended 2026-07-30 — `docs/waystone-concpets.md` §3.1)** The B+ tree is the authoritative read path (the heap chain scan stands in until it exists). Waystone is never authoritative, but a pk point lookup **may** probe it: this previously read "Waystone pages are never on the read path", and the probe now owes a validate-and-fall-back contract instead — treat a missing/dead entry or an epoch mismatch as a miss, check the Keystone id of the tuple actually found at the reported location, apply MVCC visibility as the authoritative path would, and fall through on any mismatch. Invariant 8 above is what did not change.
10. **(Revised 2026-07-28)** No single canonical in-memory tuple is enforced; consistency comes from page pin + latch discipline (§7) — shared latch for reads, exclusive latch for structural mutation.
11. **(New 2026-07-28)** A relation with `waystone_enabled` set requires system-generated, autoincrement `id` values; callers must not supply their own pk on insert into such a relation (see §4).
12. **(New 2026-07-29)** The tuple MVCC header is exactly `trx_id:48 (zero-extended to 64) | undo_ptr | data_len | flags` = 20 bytes. There is no `xmax`; a version's validity interval is reconstructed from the undo chain, and DELETE is the slot's `DELETED` mark plus the deleter's `trx_id`.

---

*Maintenance note: when a `[OPEN]` item is decided, move it into the appropriate `[CONFIRMED]` section and record the date.*
