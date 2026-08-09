# KDS Design Specification — Heap & Tuple

The authoritative specification for how KDS stores a row. Companion specs own the layers above and beside it: `waystone-concpets.md` (pattern-keyed access trails), `txn.md` (transactions and MVCC), `wal.md` (logging and recovery), `page.md` (page management and buffering), `parser.md`, `protocol.md`, `rules.md` (C++ rules), `sched.md`.

`[OPEN]` marks a decision that has not been made. Implementers must not assume one; either ask, or build behind an interface that keeps every option viable.

---

## 1. Scope

This document specifies row storage only: heap organization, page layout, the tuple format, and the structures that address a tuple. What KDS *is* — positioning, differentiators, feature scope — lives in the project `README.md` and is deliberately not restated here.

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

**Decided 2026-08-09 (`docs/feat-physical-optimizer.md` R4):** the epoch lives in the **common page header** — `PageHeaderFields::reserved0` (offset 16), the slot the header comment had already nominated — as `relayout_epoch`, u64, durable by construction. Every existing page carries 0 there, so the decision costs **no format bump**: a zero reads as epoch 0. Wraparound is unreachable at u64 width rather than handled. The pairing rule is part of the decision: no consumer may accept a location on epoch equality alone — the epoch is a fast whole-page invalidation layered over the Keystone-id check (K1), never a substitute for it. The field and the consumers' comparisons are in code (`docs/workplan-physical-optimizer.md` PX03/PX04, 2026-08-09) — recorded at access by the executor, compared at `exec/tuple_verify.hpp` for Waystone replay and Cabin hints alike, with a bump API called by nothing — and nothing bumps it until a mover exists.

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
- Under the fixed-length rule (§3.3), a relation's row size is a schema constant, so a slot's `length` and the header's `data_len` carry no new information; they are retained for format stability and treated as **checked redundancy** — a value disagreeing with the schema constant is `Corruption`, never interpreted.
- **DELETE is a delete-mark**: the slot's `DELETED` flag plus the deleter's `trx_id`, with the tuple bytes left in place for snapshots that predate it. Physical reclamation is slot retirement (`DEAD`), a separate operation for a purge pass — hence two WAL records, `HEAP_DELETE_MARK` and `SLOT_RETIRE`.
- Slot entries carry their own `flags` (`DEAD`, `DELETED`) and `length`. Retirement marks a slot dead rather than compacting eagerly.

### 3.3 Fixed-length tuples & the tagged cell

**Every tuple is fixed-length.** A relation's tuple layout is a sequence of fixed-size cells at offsets computable from the schema alone; row size is a per-relation constant, asserted in the row codec rather than policed by convention. Fixed-width types occupy their natural widths. Every variable-width type (`TEXT`, future blobs) occupies exactly **one tagged cell** of `kds.inline_cell_width` bytes, regardless of the value stored.

The rule exists for tuple mobility. An UPDATE that grows a row is what forces tuples to move in conventional engines (broken HOT chains, row migration) — and here it would additionally burn Waystone trail entries through epoch churn. With fixed cells **an UPDATE can never migrate a tuple**; combined with the immutable `min_key`, a tuple's address is stable for life until relayout moves it on purpose. Secondary gains: relayout is cell-`memcpy` with exact fill-factor math, in-page addressing is arithmetic, and the row codec reads static offsets. The accepted cost is stated plainly: variable-length management is *relocated* into the var-heap (§3.4), not eliminated, and fixed cells spend padding on short values.

Tagged cell layout, width `W = kds.inline_cell_width` (memcpy codec, `static_assert`ed, LE — `rules.md` §§2, 5):

| Tag (`u8` at offset 0) | Layout after tag | Meaning |
|---|---|---|
| `kNull` | zeros | SQL NULL; maps 1:1 to the wire NULL convention |
| `kInline` | `len u16`, then `len` bytes, zero padding | value fits: `len ≤ W − 3` |
| `kSpilled` | `len u32`, `varheap_ptr u64` (`page_id u32 · slot u16 · reserved u16`) | bytes live in the var-heap (§3.4) |

- The spill decision is a pure function of value length; an UPDATE crossing the boundary changes the cell's *tag*, never the tuple's size. A tag byte (rather than sentinels) is what lets NULL, empty, and spilled be distinguished without touching the var-heap, and is where future cell kinds land without a format bump.
- **`kds.inline_cell_width` is configuration-referenced but instance-pinned**: read once at bootstrap, written into the superblock, validated at every startup — a disagreement refuses to start, naming both values. On-disk layout depends on it, so it cannot be hot-changed; rewriting existing data for a new width is `Unsupported`. A global constant was chosen over per-column declared widths deliberately: one number instead of a schema decision users can get wrong, one codec path, and no `VARCHAR(n)`/`ALTER WIDEN` surface at all. The recorded cost is uniform padding where a per-column width would have been tighter.
- Default **64 bytes** `[OPEN: value]` — sized so common OLTP strings never spill; to be settled against measured string-length distributions of target schemas. The semantics above hold regardless of the number.

### 3.4 Var-heap

The out-of-line store for spilled values. Its design goal is to be **boring**: the mobility problem was removed from the heap and must not reappear here.

- **Immutable per version.** Writing a spilled value appends `{len, bytes}` to a `kVarHeap` page and returns its pointer; values are never rewritten and never moved. Consequences, which are the rationale: an old-version reader follows the old pointer to bytes that cannot have changed, so MVCC correctness is free; pointers need no epoch, no validation, no forwarding; the var-heap is **relayout-exempt by construction**; and reclamation is a rider on purge — when a version dies, its values die with it. The accepted cost: churn-heavy string updates consume space until purge catches up, making purge cadence a sizing input.
- **Logged, headered, checksummed** — an ordinary authoritative page class, `wal.md`'s `VARHEAP_APPEND` record. Stated explicitly because the recent reflex runs the other way: waystone/trail pages are advisory, but a var-heap value is committed data — losing one loses a value, not a hint. Advisory rules do not apply here.
- Update ordering: `VARHEAP_APPEND` (new value) → cell overwrite (`HEAP_OVERWRITE`, old cell image into undo), in one transaction, replayed by ordinary winner/loser recovery. A crash between them leaves an unreferenced value for purge's sweep. No var-heap-specific recovery logic may exist.
- Storage is invisible on the wire: `TEXT` is length-prefixed bytes to clients regardless of inline or spilled, and must stay so.

*In code.* The `kVarHeap` page class, the per-relation chain rooted at `sys.tables.varheap_page_id`, the `VARHEAP_APPEND` record and the spill/fetch path all exist (`include/kds/storage/varheap.hpp`, `rule-fixed-length-tuple.md` §8a). Two limits remain: a value larger than one page (8144 bytes) is `Unsupported` rather than chained across pages, and **nothing reclaims** — purge does not exist, so a superseded value's bytes stay until it does.

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

**Collection landed 2026-08-03; the shadow planner that consumes it landed 2026-08-09; the optimization itself — a mover — has not.** `sys.access_stats` records one row per access *shape* — `(kind, rel_id, column_mask)` — with how often it ran and when it last ran, written for every access kind through one call with no per-kind branch (`include/kds/stats/access_stats.hpp`). `SHOW ACCESS` reads it, and `SHOW RELAYOUT` weighs it with the R1 decay score into candidate relayout plans (`docs/feat-physical-optimizer.md` §5).

The shape is keyed by **columns, never values**: `WHERE flag = 1` and `WHERE flag = 2` are one row. That is what bounds the relation by the schema rather than by the data, so it needs no eviction policy and no directory — the unbounded axis, *which arguments repeat*, is Waystone's and stays there (`waystone-concpets.md` §5). The two layers answer different questions and are deliberately not merged.

What makes the data worth having is the kind split that arrived with it: a walk driven by an equality on a non-pk unindexed column is now `kFilterScan` rather than an undifferentiated `kScan`. The two cost the same and mean entirely different things — one is a statement that asked for everything, the other is a statement that asked for a few rows and had to read all of them to find out which, which is exactly the case an index or a clustering decision would fix. Measured cost of collecting: +1-2% on a point lookup, unmeasurable on anything slower.

Since 2026-08-08 the same split records what happened when that case *was* fixed: `kIndexProbe` and `kIndexRange` (`docs/feat-index.md` §8) are counted through the same call with no per-kind branch, so a relation's history now distinguishes "searched every row for a few" from "descended an index for them". A `kFilterScan` sitting beside a `kIndexProbe` on the same relation names two columns with different treatment, which is the first shape a physical optimizer could act on that this file's §7 does not already describe. Neither is trail-replayable — invariant 9's line is lookup versus search, and both are searches.

Relayout must respect the `min_key` insertion rule (§3.1), bump the page epoch (§3.1a) so every recorded location on that page becomes untrusted at once, and — **on a btree-clustered relation only** — keep the tree consistent, which is a tree restructure and out of the first mover's scope (`docs/feat-physical-optimizer.md` R8). A heap relation has no pk index, its Cabin is relocation-invariant by value = pk indirection, and secondary indexes exist only on btree relations — so a heap-relation mover maintains *nothing but the epoch*, which is why the first mover targets heap relations. Under the fixed-length rule (§3.3) a relayout is a copy of fixed cells — exact fill-factor math, no per-tuple size negotiation — and `kVarHeap` pages are outside its jurisdiction entirely (§3.4).

Key-boundary re-partitioning mainly benefits range locality; for single-pk point lookups the acceleration comes from Waystone instead. The two coexist and address different shapes.

*No mover is implemented, and consequently nothing bumps a page epoch.* **The shadow half is built (2026-08-09)**: `docs/feat-physical-optimizer.md` (R1-R12) — the decay score, the epoch field with real comparisons at both validation sites, the planner, and `SHOW RELAYOUT`, which reports every candidate plan with its predicted benefit and the §6 gate blocking it. v1 is deliberately shadow-only: every enactment is gated, and the report exists to price opening the gates.

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
13. **Every tuple is fixed-length.** A relation's row size is a schema constant; variable-width values occupy tagged cells of exactly `kds.inline_cell_width` bytes (§3.3), and that width is instance-pinned in the superblock. No code path produces a tuple whose size differs from its relation's constant.
14. **Var-heap values are immutable per version** and `kVarHeap` pages are never relocated; the class is logged, headered, and checksummed — authoritative data, not advisory (§3.4).

## 9. Open Decisions

Collected from the sections above, plus those owned by companion specs.

- ~~Per-page epoch storage location, width, and wraparound (§3.1a)~~ — **decided 2026-08-09 (`docs/feat-physical-optimizer.md` R4)**: common header `reserved0` → `relayout_epoch`, u64, no format bump. What remains true: a tuple's address is stable for life, so until a mover bumps a page every comparison is between two zeros; the field landed at workplan PX03 and the real comparisons at PX04 (both 2026-08-09), ahead of any mover — the hand-bumped-epoch contract tests in both suites are what prove they would fire.
- `kMaxAccessShapes` (`[PROPOSED]` 4096) — the cap on distinct rows in `sys.access_stats` (§7). The population is (kind × relation × column combination), which in a real schema is dozens; the cap exists because "in a real schema" is an assumption and an unbounded catalog relation written from the statement path is where that assumption would fail quietly.
- Whether access statistics ever *drive* anything (§7). Collection is built; no policy consumes it, and choosing one is a separate decision with its own blast radius — relayout has to respect `min_key` (invariant 3), keep a btree-clustered relation's tree consistent, and bump an epoch that is decided (§3.1a) but not yet in code. `docs/feat-physical-optimizer.md` §5-§6 is where the driving policy now lives, shadow-first.
- Heap page split policy; free-space reuse and page compaction, both gated on reader registration (§3.1b).
- `kds.inline_cell_width` default value (§3.3) — settle against measured target-schema string-length distributions.
- Spilled-value size cap; prefix-inline revisit trigger (adopt only if string-equality steps become a measured cost) (§§3.3–3.4).
- Purge-cadence sizing metric for var-heap headroom (§3.4).
- Repurposing of the 16 reserved Keystone bits (§4).
- Id-reuse / low-range reclamation for high-churn relations (§4).
- Buffer-pool page-frame reclamation policy (§6).
- I/O backend abstraction: plain `O_DIRECT` versus `io_uring` versus pluggable.
- Waystone's own open items — retention and eviction, recording policy, page persistence class, `arg_hash` collision handling, and whether invariant 9 is ever amended to permit trusting a cached result set as complete (`waystone-concpets.md` §9).
- Undo retention and `SnapshotTooOld` surfacing; 48-bit `trx_id` wraparound; cross-core commit protocol (`txn.md`, `wal.md`).
