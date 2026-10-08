# KDS Design Specification — Heap & Tuple

The authoritative specification for how KDS stores a row. Companion specs own the layers above and beside it: `waystone-concpets.md` (pattern-keyed access trails), `txn.md` (transactions and MVCC), `wal.md` (logging and recovery), `page.md` (page management and buffering), `parser-v2.md`, `protocol.md`, `docs/rules/rules.md` (C++ rules), `sched.md`.

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
- **Tuples within a heap relation's page are unordered** — heap append semantics, O(1) insert into free space. A btree leaf, which is a heap page (§5), is sorted by placement instead: a row takes the slot its key sorts to, shifting the directory entries after it (invariant 4, §4.1).
- Because `min_key` never changes, a reader can prune pages by key range **without locking**. That is the property the immutability exists to buy.
- Relayout honors the target page's `min_key`. Moving tuples across key ranges means writing them into **new pages with newly assigned `min_key` values**, never mutating an existing page's.

### 3.1a Per-page epoch counter

Every heap page header carries an **epoch counter**, bumped whenever the tuples physically on that page move. Unlike `min_key`, it is mutable by design.

Waystone records a page's epoch when it observes a tuple's location, and a consumer trusts that location only while the recorded epoch still matches the page's. This is how an advisory structure avoids becoming a second authoritative index: relayout bumps one counter instead of synchronously rewriting every entry that pointed into the page.

The epoch lives in the **common page header** as `relayout_epoch` — `PageHeaderFields::reserved0`, offset 16, u64, durable by construction (`page.md` §2, `docs/spec/physical-optimizer.md` R4). A page that has never been relayouted carries 0. Wraparound is unreachable at u64 width rather than handled. **Pairing rule:** no consumer may accept a location on epoch equality alone — the epoch is a fast whole-page invalidation layered over the Keystone-id check (K1), never a substitute for it. The executor records it at access and `exec/tuple_verify.hpp` compares it for Waystone replay and Cabin hints alike. No mover exists, so no relayout bumps it; the one operation that does is the btree leaf division (§4.1).

### 3.1b Chain growth by tail append

> **SUSPENDED — SUS-1, operator, 2026-09-05.** No new heap relation can be
> created: `CREATE TABLE … HEAP` is refused `Unsupported` naming SUS-1, and
> the storage-form default is `BTREE`. **Everything in this section remains
> true and remains implemented.** An existing heap relation mounts, reads,
> writes, walks its chain, replays its redo and undo, and refuses a key
> below its high-water mark for exactly the reasons below. The suspension is
> a refusal at *creation* and a default, and nothing else — nothing here is
> deprecated and nothing is to be deleted. The order, the rulings and the
> resume condition are
> `instructions/v3.0.0/workorder-as-sus1-heap-suspended.md`.

A relation is a **chain of heap pages** linked through the `next_page_id` tail reservation (§3.2), rooted at `sys.tables.desc_page_id` (`include/kds/storage/heap/heap_chain.hpp`).

- **Growth is tail append, never a split.** Every insert goes to the last page. When it has no room, a new page is allocated, the tuple is written into it, and only then is the page linked on — the link is what makes a page reachable, so publishing it first would expose an empty tail.
- **A new page's `min_key` is the id of the tuple that caused the growth**, the smallest id it can ever hold, since ids only increase. No existing page's `min_key` is touched, so §3.1's immutability holds by construction.
- **Each page's ids lie entirely below the next page's `min_key`.** The chain is key-ordered page by page while tuples within a page stay unordered — "semi-sorted" holds across pages as well as within one. Two consequences are relied on in code: a duplicate incoming id can only be in the tail page, so the check is O(1) pages rather than O(chain); and an id below the tail's `min_key` has nowhere legal to go and is refused as a backwards sequence.
- **All three of the above rest on ids ascending, and that is the whole of what a heap relation refuses** - a heap alone since BD, which left the heap's protocol untouched (BD-Q4 (a)) while a btree took to placing a named key where it sorts (§4.1). A heap relation *may* be told its keys — what it may not be told is a key below its high-water mark, refused `OutOfRange` under the hold of the chain's tail (BB-R7, `heap::ChainInsertNamed`): below the tail's `min_key` by the chain itself, at or above it by `Catalog::AdmitExplicitRowId` (§4.1). The mark is this section's three properties written as one number: at or above it, the incoming id is above every id the relation has ever placed, so the tail is the only legal page, the new page's `min_key` is still the smallest id it can hold, and a duplicate can still only be on the tail. Below it, all three fail at once — and the second failure is the dangerous one, because a page opening below an id already on its predecessor makes the tail-only duplicate check admit a duplicate silently. The refusal is per *id*, never per relation, which is why nothing in this section is conditional.
- **No free-space reuse.** A page that fills and then has rows deleted is never revisited; the chain only grows at the tail. A delete-heavy relation grows monotonically.
- Walks are bounded by `kMaxChainPages` (2^20 pages, 8 GiB per relation). Exceeding it is `Corruption` rather than a loop, since a cycle in the links would otherwise hang a request.

No heap page is ever divided, compacted or reused, and no tuple moves off one. §4.1's btree leaf division is not a heap split and not a precedent for one: a divided leaf is re-routed to by the separator its division promotes, while a heap page is reachable only through the chain that precedes it, and a heap relation's ascending ids never sort inside a full page.

### 3.2 Page layout

- The slot directory grows downward from the heap area offset; tuple data grows upward from the top; free space is the gap (`upper - lower`).
- The page tail permanently reserves `sizeof(PageId)` bytes for the `next_page_id` chain link, excluded from free-space accounting.
- The per-tuple MVCC header is **`trx_id` (48-bit writer, zero-extended to 8 bytes) + `undo_ptr` + `data_len` + flags — 20 bytes, with no `xmax`**. A version's death is the next version's birth: walking the undo chain already names the overwriting transaction, so storing that boundary a second time in the older version would be recording one fact twice. `trx_id` is whichever transaction last stamped the version — insert, overwrite, or delete-mark. The lock-slot role `xmax` plays in PostgreSQL is covered by `trx_id` itself and by nothing on the page: it was specified as the Keystone flags byte's, and AO-R3 decided the lock family persists no bit at all (§4, `txn.md` §5).
- Under the fixed-length rule (§3.3), a relation's row size is a schema constant, so a slot's `length` and the header's `data_len` carry no new information; they are retained for format stability and treated as **checked redundancy** — a value disagreeing with the schema constant is `Corruption`, never interpreted.
- **DELETE is a delete-mark**: the slot's `DELETED` flag plus the deleter's `trx_id`, with the tuple bytes left in place for snapshots that predate it. Physical reclamation is slot retirement (`DEAD`), a separate operation for a purge pass — hence two WAL records, `HEAP_DELETE_MARK` and `SLOT_RETIRE`.
- Slot entries carry their own `flags` (`DEAD`, `DELETED`) and `length`. Retirement marks a slot dead rather than compacting eagerly.

### 3.3 Fixed-length tuples & the tagged cell

**Every tuple is fixed-length.** A relation's tuple layout is a sequence of fixed-size cells at offsets computable from the schema alone; row size is a per-relation constant, asserted in the row codec rather than policed by convention. Fixed-width types occupy their natural widths — `char(N)` among them, fixed by its declaration. Every variable-width type (`TEXT`, future blobs) occupies exactly **one tagged cell**, regardless of the value stored; that cell is `kds.inline_cell_width` bytes wide — the instance's, or the column's own when it was declared `varchar(N)`.

The rule exists for tuple mobility. An UPDATE that grows a row is what forces tuples to move in conventional engines (broken HOT chains, row migration) — and here it would additionally burn Waystone trail entries through epoch churn. With fixed cells **an UPDATE can never migrate a tuple**; combined with the immutable `min_key`, a tuple's address is stable for life until relayout moves it on purpose. Secondary gains: relayout is cell-`memcpy` with exact fill-factor math, in-page addressing is arithmetic, and the row codec reads static offsets. The accepted cost is stated plainly: variable-length management is *relocated* into the var-heap (§3.4), not eliminated, and fixed cells spend padding on short values.

Tagged cell layout, width `W = kds.inline_cell_width` (memcpy codec, `static_assert`ed, LE — `rules.md` §§2, 5):

| Tag (`u8` at offset 0) | Layout after tag | Meaning |
|---|---|---|
| `kNull` | zeros | SQL NULL; maps 1:1 to the wire NULL convention |
| `kInline` | `len u16`, then `len` bytes, zero padding | value fits: `len ≤ W − 3` |
| `kSpilled` | `len u32`, `varheap_ptr u64` (`page_id u32 · slot u16 · reserved u16`) | bytes live in the var-heap (§3.4) |

- The spill decision is a pure function of value length; an UPDATE crossing the boundary changes the cell's *tag*, never the tuple's size. A tag byte (rather than sentinels) is what lets NULL, empty, and spilled be distinguished without touching the var-heap, and is where future cell kinds land without a format bump.
- **`kds.inline_cell_width` is configuration-referenced but instance-pinned**: read once at bootstrap, written into the superblock, validated at every startup — a disagreement refuses to start, naming both values. On-disk layout depends on it, so it cannot be hot-changed; rewriting existing data for a new width is `Unsupported`.
- **It is the default, not the only width** (`docs/rules/rule-fixed-length-tuple.md` §4). `varchar(N)` declares `N` as *that column's* `kds.inline_cell_width` — the same number at a narrower scope, with the same `[16, 4096]` bounds from the same validator, and deliberately **no second name for it**. `N` is a width, not a length cap: a longer value spills as it always has. A bare `varchar` stores `len = 0`, which reads as the instance width. Widening a declared width is refused, because it rewrites every row.
- Default **64 bytes**. The semantics above hold regardless of the number.

### 3.4 Var-heap

The out-of-line store for spilled values. Its design goal is to be **boring**: the mobility problem was removed from the heap and must not reappear here.

- **Immutable per version.** Writing a spilled value appends `{len, bytes}` to a `kVarHeap` page and returns its pointer; values are never rewritten and never moved. Consequences, which are the rationale: an old-version reader follows the old pointer to bytes that cannot have changed, so MVCC correctness is free; pointers need no epoch, no validation, no forwarding; the var-heap is **relayout-exempt by construction**; and reclamation is a rider on purge — when a version dies, its values die with it. The accepted cost: churn-heavy string updates consume space until purge catches up, making purge cadence a sizing input.
- **Logged, headered, checksummed** — an ordinary authoritative page class, `wal.md`'s `VARHEAP_APPEND` record. Waystone/trail pages are advisory; a var-heap value is committed data — losing one loses a value, not a hint. Advisory rules do not apply here.
- Update ordering: `UNDO_WRITE{kVarHeapAppend}` (the append's own rollback) → `VARHEAP_APPEND` (new value) → cell overwrite (`HEAP_OVERWRITE`, old cell image into undo), in one transaction, replayed by ordinary winner/loser recovery. The undo record comes first because redo alone must never resurrect an append the undo phase has no record to release; the append is a link in the transaction's own undo chain, so undo reaches it like any other loser write (`rule-fixed-length-tuple.md` §5). No var-heap-specific recovery logic may exist.
- Storage is invisible on the wire: `TEXT` is length-prefixed bytes to clients regardless of inline or spilled, and must stay so.

*In code.* The `kVarHeap` page class, the per-relation chain rooted at `sys.tables.varheap_page_id`, the `VARHEAP_APPEND` record and the spill/fetch path (`include/kds/storage/varheap.hpp`, `rule-fixed-length-tuple.md` §8a). `varheap::PageRelease` tombstones a slot by zeroing its offset — writing no value byte, so invariant 14 holds to the letter — and a `VARHEAP_RELEASE` record makes it durable. What releases is **rollback**: a spill is a link in its transaction's undo chain, so both the live `Abort` and recovery's undo phase let a loser's values go. A value whose version merely *died* — superseded or deleted — is not released; its bytes stay. A value larger than one page (8144 bytes) is `Unsupported` rather than chained across pages.

## 4. Keystone Column

Every tuple's **first column is mandatory**: one 64-bit word, the *Keystone word*. This is a self-imposed constraint of KDS and the tuple's identity lives in it.

| Field | Width | Purpose |
|---|---|---|
| `id` | 40 bits | Primary key. Per-relation capacity ≈ 1.1 × 10^12 issued ids. |
| `flags` | 8 bits | Reserved, and **written as zero** — `row_codec.cpp`'s encode is the only writer and passes 0. It was specified "Oracle lock-byte style; may reference a per-page transaction slot", and AO-R3 decided against that: M2's lock family holds **no persisted bit**, a row's exclusive tenancy being the MVCC header's `trx_id` stamp plus an in-memory table (`txn.md` §5). Tuple status such as `DEAD` lives in the slot directory, not here. |
| `reserved` | 16 bits | Writers set 0, readers ignore. |

**Every relation's pk is a unique 40-bit id that is never rebound - except by an explicit `PURGE` (§4.1c) - and never updated.** *Where* the id comes from is a per-**row** choice — the `INSERT` names it or omits it (§4.1). An issued id ascends in issue order; a named key carries no order on a btree, which places it where it sorts, and continues the sequence on a heap, which admits it only at or above its mark (§3.1b). What is not a choice is uniqueness: **a Keystone id is bound to at most one committed tuple in the lifetime of a relation, except across a `PURGE` that freed it** (K1), and every consumer rests on that - each re-checks a stored pk against the row now there, which is what makes the exception safe for it (§4.1c). The provenance of the value does not matter to any of them.

Uniqueness is obtained two ways:

- **The mark, for an issued id.** `sys.tables.next_id` never moves backwards, so an id at or above it is above every id the relation has ever placed. An omitted pk is the mark itself, unique among issued ids. On a btree a named key equal to an id drawn but not yet placed can be placed first; the placement's duplicate check (`SearchLeaf`) then finds it, and the issue burns the id and draws again, bounded at `kMaxIssueRounds`. On a heap a named key is admitted only at or above the mark too, so the mark is a heap's whole proof.
- **The descent, for a named key on a btree.** It lands on the one leaf that may hold the key, and one binary search over its keyed slots (`btree::SearchLeaf`) answers whether a slot is keyed `k`, live or delete-marked; one is a duplicate, `AlreadyExists` (§4.1). The mark gates nothing there (BD-R7). An issued id that a named key equal to it placed first is burned, and the issue draws again (§4.1).

What holds regardless:

- The cursor is **persistent, not derived**: `sys.tables.next_id`, a **high-water mark on what has been placed**. `Catalog::AllocateRowId()` returns it and advances; `Catalog::AdmitExplicitRowId()` advances it past a supplied id at or above it; one below it is left where it is on a btree and refused on a heap (§4.1). Deriving it as `max(id) + 1` would reissue an id after the highest tuple is deleted, handing a new tuple the identity of a retired one. It is what `SHOW BUDGET` and `DESCRIBE` derive K4's lifetime budget from, so it must never fall behind what was placed. The first id issuable is 1 (`kFirstRowId`); 0 stays reserved for "unset".
- **The two id sources share the one mark.** An issued id clears every key named and admitted before its draw, and a named key at or above the mark clears every id the engine has issued. A *below-the-mark* named key can meet an issued one on a btree, and the descent's duplicate check answers it as it answers any present key; a heap refuses it.
- Ids are unique by construction, **not gapless** — an insert that fails after allocating burns one, and a caller may skip a range outright. Nothing depends on gaplessness.
- **Issued ids are monotonic in issue order on every relation; named keys carry no order on a btree** (§4.1a). A btree leaf's slot order is its key order by placement (BD-R1), not by issue order. Page-wise `min_key` ordering is preserved by a leaf division as well, so range pruning (`kRange`'s tail prune, `src/exec/step_vm.cpp`) does not depend on slot order.
- The pk is carried **only** by the Keystone word, never also as a body column: `EncodeRow()` writes `[Keystone word][columns 1..n-1]`. Storing a key twice is how the two copies come to disagree. A supplied id is *named* in the statement's first position and still lands only in the Keystone word.
- The pk **cannot be updated**. It is the tuple's identity, not a field of it.
- A relation's first column must be declared with an **integer type** (`catalog::CheckKeystoneColumn`), checked at `CREATE TABLE`. Its declared width is display metadata: the id lives in the 40-bit Keystone field regardless, so a narrow declared type does not cap the sequence.

Implementation rules:

- Encode and decode with **explicit shift/mask helpers only**. **Never use C/C++ bitfields** for an on-disk format — their layout is implementation-defined and KDS must be portable across architectures.
- The whole word is updated with **atomic `uint64_t` operations (CAS)**. Fields must never tear across writes.
- External structures — B+ tree keys, `min_key`, Waystone entries — store the id as a **zero-extended `uint64_t`** with the upper 24 bits zero. Ids are never 5-byte-packed.

**An id is consumed when a tuple carrying it commits** (BD-R4): the cursor never issues an id twice, and a committed key is never bound again unless a `PURGE` freed it (§4.1c). An id burned without placement, or placed and rolled back, is free, and a named key may take it. Sequence exhaustion is reported as `OutOfRange`, never wrapped.

### 4.1 Caller-supplied keys

> **Every relation takes a caller-supplied primary key or issues one when the `INSERT` omits it, per row. A btree places a named key where it sorts and refuses it because of its pk for exactly two reasons — a duplicate or an exhausted key (BD-R5). A heap admits a named key only at or above its high-water mark (§3.1b, kept by BD-Q4 (a)).**

There is no key mode and no `CREATE TABLE` declaration of who names the key: `INSERT` is the statement that names one, so the fact lives in the row's arity, and no relation refuses either arity. There is no per-relation key-order state either: **a btree leaf places a row at the slot index its key sorts to** (`heap::PageView::InsertTupleAt`, which moves the directory entries at and after that index up one and no tuple byte), so every keyed slot of every leaf ascends by Keystone id, live and delete-marked alike, whatever order keys arrive in and at every core count (BD-R1, BD-R2, `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`). A retired slot carries no key and sits anywhere. There is no exception for a relation to record (BB-R10).

*In code.* `catalog::KeyOrder`, its flip, `CatalogCache::MarkKeysUnordered`, `TableAccess::key_order`, the walk's per-page key-order emission and `DESCRIBE`'s `key_order=` are deleted (BB-R10, BB-Q11 (a)). The byte stays at its offset as `SysTableRow::retired_key_order` (`kRetiredKeyOrderOffset`, 101): every row writes it 0 and nothing reads it. **Superblock 20 refuses every version-19 volume** (BD-R8, no migration), because one an engine older than `1b5d252e` wrote at `cores > 1` can hold a leaf out of key order, which a fallback-free search reads wrong, and its log has no `BTREE_INSERT` or `BTREE_SPLIT` to replay. BB-R11's mount check, which read the byte and refused a relation still marked `kUnordered`, went with it. `Catalog::CreateTable` takes no key parameter.

**Syntax.** A trailing bare identifier in the same optional slot as the storage clause:

```sql
CREATE TABLE t (id int64, qty int64) BTREE EXPLICIT;
```

`EXPLICIT` is accepted and sets no field: it states what is true of every relation — the caller may name this relation's keys. `ASSIGNED` is `Unsupported` with its byte, because it means "the engine issues every id and supplying one is refused", and on the relation the statement would create, supplying one is admitted; accepting a spelling and enforcing something else is worse than refusing it. `HEAP BTREE` is an `InvalidArgument` repeat; `ASSIGNED EXPLICIT` is refused at the first word. All words are case-insensitive **identifiers, never reserved keywords**, so none of them moves a fingerprint hash. Storage does not follow the word: `CREATE TABLE t (...)` and `CREATE TABLE t (...) EXPLICIT` are both btree-clustered, the default since SUS-1.

**`default_key_mode` is not a config key.** It stays *known* so its absence can be reported: an instance file naming it is refused at startup with a message saying why, rather than falling to the generic unknown-key error.

**A heap relation refuses a key below its high-water mark**, `OutOfRange`, naming the mark, whether the key is present or not (BB-R12, kept for the heap by BD-Q4 (a)). A btree refused the same keys from BB-S3 until BD-S3 and places them now; the heap's refusal predates both and has a reason of its own. A chain grows only at its tail (§3.1b), so a key below the tail page's `min_key` has no legal page — inventing one would either mutate a `min_key` (invariant 2) or place a tuple below one (invariant 3). And the chain's duplicate check reads the **tail page alone**, which is sound only while every earlier page's ids sit below the tail's bound; a page opening below an id already on its predecessor breaks that quietly, and a duplicate pk is then admitted with no error at all. Both properties are the ascent, so refusing below the mark is what keeps §3.1b true rather than a second rule that could drift from it. At or above the mark the incoming key is above every id the relation has ever placed, so the tail is the only legal page *and* the only page a duplicate could be on — the two questions the descent answers on a btree, answered here by the tail's hold and one number: a key below the tail's `min_key` is refused by the chain before the mark is read, and one at or above it by the mark (BB-R7).

**A named key on a btree is refused because of its pk for exactly two reasons** (BD-R5), each carrying the pk token's byte as `(byte N)`, and `(row k)` in a multi-row statement:

1. **Exhausted — `OutOfRange`.** The key is outside `[kFirstRowId, kMaxKeystoneId]` = `[1, 2^40 − 1]`: zero, a negative number, one above 2^40 − 1, or a literal past 64 bits. It is judged from the literal's own digits, never from the lexer's wrapped value, so `2^64 + 5` is refused rather than landing as 5, and it is asked before the foreign-key and assertion steps. For an omitted key the same reason is the mark past 2^40 − 1, `OutOfRange` with its own text.
2. **Duplicate — `AlreadyExists`.** A slot keyed `k` in the leaf the descent holds, live (*"duplicate primary key k already present at page p slot s"*) or delete-marked and not purged (*"duplicate primary key k: a row with this key was deleted; PURGE frees its key"*, `btree::DuplicateKey`). A key named twice in one statement is a duplicate at its second occurrence.

Nothing else about the key refuses it on a btree: after `pk = 100`, `pk = 99` is placed, and a multi-row `VALUES` names its keys in any order. Refusals the pk does not cause — the parse errors, arity, a non-integer literal (`InvalidArgument`), `max_locks_per_txn`, a lock wait, a foreign key, an assertion, `OutOfSpace` — are unchanged. **The one known exception** is the equal-index-sort-keys defect, whose `AlreadyExists` is not a duplicate (`docs/inflight/bugs/a-run-of-equal-index-sort-keys-promotes-one-separator-twice.md`).

**The admission.** `Catalog::AdmitExplicitRowId(oid, id)` refuses an id outside the space `OutOfRange` before the catalog page is touched, for a caller that did not judge its literal already. Then:

- **At or above the mark**, either storage: the mark moves to `id + 1`, persisted before the caller places anything. On a btree this runs before the descent, under no page hold (BD-R6); on a heap under the exclusive hold of the chain's tail, held as the tail since BB-S4 (BB-R7, `heap::ChainInsertNamed`).
- **Below the mark**: on a btree the mark is left where it is and nothing is refused — advance or nothing (BD-R7). On a heap it is refused `OutOfRange` with nothing written, so a refused key burns no mark.

**Admitting before the descent is safe on a btree**: every placed id is below the mark, so a key at or above it cannot be a duplicate, and two statements naming one key serialise on its row unit.

**A refused borrow is judged once** (AO-S6c-c's rule: a key that can never be admitted never waits), so a granted borrow pays nothing. On a btree (BD-R5, BD-Q9 (a)) a slot keyed `k` whose latest writer has decided is a duplicate at once. Where that writer is in flight, its version chain is walked back while each earlier writer is in flight: a chain that ends is an **undecided insert**, not yet a duplicate because its rollback frees `k`, so the statement waits for its decide and the re-run answers by the outcome; a chain that reaches a decided writer is a committed row under an undecided update or delete, a duplicate at once. An absent key is a wait like any other, so **a key below the mark can meet a fence**: an open `DELETE … WHERE id < 50` makes a named insert of 10 wait. On a heap a refused borrow asks the mark (`Catalog::RowIdMark`), and a key below it — final, since the mark only rises — is refused there `OutOfRange`.

There is no mode check in this function, and none in `AllocateRowId` or `AllocateRowIdRange` either: all three run on every relation.

**The mark's advance outlives a rollback**, deliberately. It is made outside the caller's transaction (`wal::kNoTxnId`), so a `ROLLBACK` after a named key leaves the mark advanced. On a btree that costs nothing: both rollbacks retire the inserted row's slot keyless, the duplicate check does not see it, and **a rolled-back key named again is placed** (BD-R4, W12). On a heap the key is now below the mark, and naming it again is refused.

The mark is a **high-water mark** (BD-R7): the issue cursor for omitted keys, above every id ever placed, so an issued id needs no page read; K4's lifetime budget and the 40-bit exhaustion check; what `DESCRIBE` and `SHOW BUDGET` read. A named key at or above it moves it to `k + 1`, logged outside the transaction. It refuses nothing on a btree, and is the gate on a heap.

**On a btree relation the descent is exact, and one search answers three questions.** It lands on the one leaf whose key range covers the id; `btree::SearchLeaf`, one binary search over the leaf's keyed slots that steps over retired ones, answers whether `k` is present, the slot it sorts to, and whether that slot is past every key (an append, or an append split when the leaf is full) or not (a divide when it is full). It is the one leaf search - placement, lookup, `BTREE_INSERT`'s redo and recovery undo's re-find all use it - and it has no linear fallback, because no leaf is out of order. Consequences:

- **A committed key stays bound until a `PURGE` frees it** (BD-R4, §4.1c). A `DELETED` slot keeps its key, and a divide carries a delete-marked row with its key, so a `DELETE`d pk named again is refused as the duplicate it is. **The delete-marked row is the tombstone**, and `PURGE` is the one statement that retires it. An engine-internal purge still owes a keyed, never-visible tombstone in the row's key position - which the duplicate check and the placement order read, and which walks and `VerifyTupleAt` treat as absent - or reclaims nothing (`docs/inflight/known-gaps.md`).
- **A rolled-back key is free** (W12). Both rollbacks retire the slot keyless and a divide drops it, so the duplicate check does not see it. That is sound because the rolled-back tuple was never visible to another snapshot, and every structure that keeps `(oid, pk)` past a write verifies against the row now there.
- **`PlaceUnderHold` still refuses an id below its landing leaf's `min_key`** as `OutOfRange`. The descent makes that unreachable — a separator *is* a child's `min_key` — so it is a defensive check on the two ever disagreeing, not a policy about ordering.
- **A placement mid-leaf shifts the slots after it and bumps no epoch.** Every holder of a btree row's `(page, slot)` past its hold is shift-safe (BD-R3): a Waystone trail entry, a Cabin hint and the probe memo verify the Keystone id at the slot and miss to a descent; recovery undo re-finds its row by key (`txn.md`); the inner-build probe verifies (`join-inner-build.md` JB4); JB6's resume mark is a key. The `INSERTED … page= slot=` reply names the slot at placement. A placement whose log record is never written is taken back at every failure exit before the record (index maintenance, the assertion reservation, the undo append, the spill noting). Without a split, the still-held leaf loses the slot (its entry removed, the entries after it moved down) and gives back the tuple's bytes. A split cannot give back a rebuilt page, so the row's slot is retired keyless, the split is logged as one `BTREE_SPLIT` under `kNoTxnId`, and a root it grew is published.

**A full leaf divides** (`SplitLeafAndInsert`, `src/storage/btree/btree.cpp`). An id above everything in a full leaf is an *append*, which opens a fresh leaf and moves not one byte. An id that sorts *inside* a full leaf forces a division, and SQL reaches it whenever a named key sorts below a full leaf's highest key (BD). Why a division is legal inside the invariants, stated precisely because it is the one place tuples move between pages without a mover:

- **Invariant 2 holds** — the old leaf keeps its `min_key` **unchanged**. A division moves the *upper* half out, so the low bound a lock-free reader may already have pruned by never moves.
- **Invariant 3 holds on both sides** — everything that stays was at or above the old bound already, and the new leaf's `min_key` is the split key, which is by construction the smallest id moved into it. Neither page ends up holding a tuple below its own `min_key`.
- **One merge, one cut.** The keyed versions are copied out in slot order, which is key order (a leaf found out of order is `Corruption`), the incoming row is merged in where it sorts, and the run is cut once. **The rightmost leaf cuts at the insertion point** (BD-Q11 (a)): the incoming id opens the new leaf with every key above it and the left leaf stays full, so ids arriving slightly out of issue order at `cores > 1` leave no half-empty leaf behind; the median takes over for an id below every key or a moving half no empty leaf holds. **Any other leaf cuts at the median.** A named key far below the rightmost leaf's top leaves the left leaf sparse, which costs space, never correctness.
- **The old page is rebuilt, not edited.** `RetireSlot` marks a slot dead without reclaiming its bytes — reclamation is a purge pass's job and no purge exists. Retiring the moved half would therefore leave the page exactly as full as it was, and the division would make room for nothing, which is the entire point of it. So the page is reformatted and the staying half written back.
- **The old page's `relayout_epoch` is set to `old + 1`.** Every tuple on it changed slot and half of them changed page, which is a relayout in everything but name, so §3.1a's pairing rule applies: every Waystone trail entry and Cabin hint pointing into that page becomes untrusted at once. It is set to one past the old value rather than bumped from the zero the reformat left, because an epoch that went backwards would let an entry recorded at the old value compare equal again — the one thing the field exists to stop.
- **Delete marks travel with the version they belong to.** A moved version carries its deleter's `trx_id` and arrives still marked; re-inserting the payload alone would resurrect a row some snapshot has already been told is gone.
- **Secondary indexes need nothing.** An index entry's sort key is `key || pk` (`index_page.hpp`) and its payload is the pk — never a location — so a division is invisible to them. The undo chain is likewise addressed by `undo_ptr`, not by where a version sits.
- A leaf holding **fewer than two live tuples** cannot be divided: no boundary makes room, because the row is near page-sized. Reported as the `OutOfSpace` it is, naming the reason, rather than producing an empty leaf a descent would route to and never satisfy.
- **Every leaf's keyed slots are in key order**, through SQL and through `BtreeInsert`'s storage contract alike: both place where the key sorts, and each half of a division is written in key order. `SearchLeaf` relies on it and has no fallback.
- **A split is one log record.** Every page it writes - both leaves, every internal node created or amended, a new root - is imaged into one `BTREE_SPLIT`, whose CRC makes it replay whole or not at all, and no insert record follows it (BD-R12, `wal.md` §5.2).

**Promotion into a full internal node divides it** (`PromoteSeparator`). Two shapes, told apart rather than assumed. A separator sorting above every entry the node holds takes a right-split with no movement — a new node whose only child is the new subtree — which is the append case a monotonic sequence produces exclusively, and it is correct and free there. A separator sorting *inside* the entries, which a named key below the relation's highest reaches from SQL, divides them: the **median separator moves up** rather than being copied, its child becomes the new node's leftmost child, and the lower half is written back with the original leftmost child untouched. Copying the median instead — the leaf's rule — would route every key at exactly that value into a subtree that no longer holds it. Telling the two apart is not optional: promoting an interior separator by the cheap path would strand every subtree sorting above it, which is silent data loss rather than a wrong answer anyone would notice.

**`INSERT` arity is per-row and two-valued.** `ncols` values means the caller names the key and `values[0]` is it; `ncols - 1` means the caller omits it and the engine issues one. Both are legal on every relation, row by row, and a wrong length is refused naming **both** accepted counts — with two of them, a message naming one reads as an off-by-one against whichever the writer did not mean. The two counts cannot be confused: pk-plus-*n* columns is *n* + 1 values, pk-omitted is *n*; `INSERT` is positional with no column list (`parser/ast.hpp`'s `InsertStmt`) and no body column may be omitted individually, so a row's length names one reading and not the other, on every relation.

The supplied pk must be an **integer literal** — the gate runs before anything is placed and must not depend on evaluation — and a non-integer value is refused `InvalidArgument`, an integer outside the id space `OutOfRange` (exhausted, above), each carrying the offending token's byte. When the row names its key, the pk is split off `values[0]` once, so everything downstream (the FK forward check, assertion admission, `EncodeRow`, the Cabin witness, index maintenance) receives the shape it expects: the columns *after* the key. `AdmitExplicitRowId` and `AllocateRowId` both sit after `enforcer_.AdmitInsert` and the foreign-key check, so a row those refuse burns nothing. **The borrow always precedes the encode**, because a spill is a trail entry. The orders (BD-R6, `CommandDispatcher::InsertOneRow`):

- **A btree, an omitted pk:** issue (`AllocateRowId`), borrow, encode, descend, place - no page hold covers the issue. A refused borrow burns the id, which is then free (BD-R4). A placement that finds the drawn id present - a named key equal to an issued id not yet placed borrowed it first and placed it - burns the id and draws again, bounded at `kMaxIssueRounds`.
- **A btree, a named key:** the exhausted check, borrow, encode, admit (advance or nothing), descend, the duplicate check, place.
- **A heap** (BB-R2 and BB-R3's order, kept by BD-Q4 (a)): an omitted pk descends to the tail first, then is issued, borrowed and encoded under the tail's hold; a named key is borrowed and encoded first, outside any latch, and admitted under the tail's hold, so its spills precede its admission and a refusal there unwinds them as any refused insert does.

BB-R1, which fixed a btree row's id under the exclusive hold of the leaf it lands on so that placement order was issue order, was withdrawn on a btree by BD (W10): its one reason was a leaf's key order, which placement keeps. A heap keeps it (BB-R7).

**Bulk `INSERT`** runs every row through that same single-row pipeline in statement order, so a bulk statement may mix named and omitted rows, and each row is admitted, placed and indexed exactly as if it had arrived alone. On a btree a statement names its keys in any order, and a key it names twice is a duplicate at its second occurrence, with that row's ordinal. On a heap a named row must clear the mark every earlier row moved, so a statement naming keys out of order is refused at its first row below the mark, with that row's ordinal, and fails whole (BI4). The **sorted-fill fast path is engaged only when every row omits its key** (`SortedFillEligible` plus a per-statement check at the call site): the fill carves one contiguous id range up front and appends in order, which leaves no place for a key the caller chose. Ineligibility, never a refusal — a statement that names keys still runs, through the per-row path. The check is per statement rather than per relation because naming a key is a property of the row; what stays on `SortedFillEligible` is the relation-shaped half.

**A carve works on every relation.** `AllocateRowIdRange` refuses nothing for a key reason. Its one caller is the sorted fill, which is heap-gated (`bulkinsert.md`), and the one consequence is that a carve spends its block from the mark's point of view before those ids are placed. A *named* key cannot land inside a live carve: a carve runs only on a heap, where a named key must be at or above the mark, and the carve has already moved the mark past its own block.

**Every core issues and admits from the one mark.** Admitting a named key writes the relation's `sys.tables` row — the mark — and so does issuing an omitted one (`AllocateRowId`'s bump). On a heap both happen under the exclusive hold of the chain's tail (BB-R7), the user page outer to the `sys.tables` page (BB-R4, `page.md` §6); on a btree both happen before the descent, under no user page (BD-R6). That row was the system core's page until AT-S5 and a peer refused a named key per row; it is every core's now, written under the page latch with no task parking inside the span (`catalog.md` CT5). The omitted arity has bumped it on every core since AT-S10b (§4.1a).

**The pk is not updatable** (K2). `exec::CompileAssignments` refuses a pk `UPDATE` at compile time as `Unsupported` with the column's byte, regardless of provenance. Naming a key at insert and changing one afterwards are unrelated permissions; only the first is granted.

**What none of this touches:**

- **The heap chain's page layout.** `ChainInsert` refuses an id below the tail page's `min_key` as `OutOfRange` and checks duplicates on the tail page alone; the mark check in `AdmitExplicitRowId` is what keeps those two facts reachable. Since BB-S4 it runs inside the chain, under the tail's hold and after the tail's own check (`ChainInsertNamed`, BB-R7), rather than above it. Nothing divides a heap page or moves a tuple off one (§3.1b).
- **Waystone, Cabin, secondary indexes and foreign keys**, which key on the id's *value* and never on its provenance or its order.
- **The 40-bit budget (K4)** bounds the id *space*, not the insert count. A relation fed sparse named keys exhausts it after fewer rows; both places the budget is read — `DESCRIBE` and `SHOW BUDGET` — derive it from `next_id`, which the high-water advance keeps truthful.

**`DESCRIBE`**'s summary line carries no `key_order=` since BB-S3b (BB-Q11 (a)): a field that cannot vary reports nothing, and it went with the state it reported. The pk column reports `autoincrement=if-omitted` on every relation, and every other column `no`. Neither `yes` nor `no` is true of a pk — the sequence runs when the `INSERT` omits the key and does not when it names one, and both are legal everywhere — so printing either would be printing something untrue for a field's convenience.

**`ORDER BY <pk>` is discarded, with no per-page work.** `exec::CompileStepChain` accepts the driving relation's pk as an `ORDER BY` target and discards the clause outright: a walk emits pages in ascending `min_key` and a page's slots consecutively in slot order (`RunWalkStep`, `src/exec/step_vm.cpp`), and every page's slot order is its key order — a btree leaf's by placement (BD-R1, BD-R2), a heap tail's because each id is fixed under its hold above every id already there (BB-R7, since BB-S4). The Cabin serve's pk sort (`cabin.md` §4a) and the index step's (`index.md` IX8a) rest on the same premise. No relation records an exception for a reader to test (BB-R10), and superblock 20 refuses the version-19 volumes whose leaves need not hold it (§4.1).

**Recovery**: the mark's advance is one `OverwriteLogged` of one `sys.tables` row, logged before the row it covers is placed, so it redoes with every other catalog write and a crash between the two burns an id rather than reissuing one (K1, `keystoneid-invariant.md` §2). The retired key-order byte rides in the same row, written 0.

Tests: `tests/supplied_key_test.cpp` and `tests/sorted_leaf_named_keys_test.cpp` end to end (the latter's census pins every pk-caused refusal to the two reasons), the admission cases in `tests/catalog_test.cpp`, the placement and leaf-division cases in `tests/btree_test.cpp`, the waits in `tests/lock_family_test.cpp` (`NamedKeyWaitTest`), and the crash cases in `tests/sorted_leaf_crash_test.cpp` and `tests/recovery_undo_test.cpp`.

### 4.1a Monotonicity when more than one core inserts

**Every core issues from the one mark since AT-S10b.** A peer's omitted-pk
`INSERT` runs on the peer (AT-S5) and calls `Catalog::AllocateRowId`, the
in-place bump of the relation's `sys.tables.next_id` under that catalog
page's latch that core 0 always ran (`catalog.md` CT5). Two cores issuing
into one relation are serialised by the latch, so the ids are **one
sequence in issue order however many cores insert**, and §4.1's argument
holds unchanged: the mark stays one ceiling on what has been placed, and
issued ids are unique across cores by construction (K1). **Placement order
need not be issue order on a btree** since BD (BD-R6): the bump runs under
no page hold, so an id issued before a higher one that was placed first
lands below it, at the slot it sorts to, and the leaf stays in key order by
placement (BD-R2).

**No core caches a block, and that is deliberate.** AT-R4 and D20
(`instructions/v3.0.0/workorder-at-m3-uniformity.md`) name a per-core
row-id cache of 4,096; the build departs from that wording on hard
invariant 11's authority (`CLAUDE.md`): with insert spreading off, which is
every relation, an issued pk is an identity *and a sequence*, monotonic in
issue order (a named key carries no order on a btree, §4.1), and a
per-core block breaks issue order across cores - two rows
inserted a microsecond apart on different cores carry ids thousands apart,
and the later one may carry the lower id. That was the engine until
AT-S10b, when a peer issued from a block core 0 carved and leased to it over
the ring. The transaction-id sequence keeps its per-core block
(`txn.md` §4.2), because no reader decides by a transaction id's order
since AN-S2 (`txn.md` §4.1).

**Insert spreading is retired (AT-S9)**, with range ownership, and the
split relation it produced is retired with its directory (2026-09-30,
`crosscore.md` CC8): a relation is one structure headed by `desc_page_id`,
and `range_size_ids` is refused by name.

**What a client may rely on.** An issued pk is an identity **and a
sequence**, monotonic in issue order, on every relation and whichever cores
insert, and comparing two issued ids of one relation orders them in issue
order, never across relations or histories. **A named key carries no
order** on a btree: it is any id in the space not bound to a committed
tuple (§4.1), so its value says nothing about when it was inserted. On a
heap a named key is admitted only at or above the mark, so it continues the
sequence.

**On a heap, placement order is issue order at every core count (BB-R7,
kept by BD-Q4 (a)).** An omitted pk is issued, and a named key admitted,
under the exclusive hold of the chain's tail held **as** the tail
(`heap::ChainInsertIssued`, `heap::ChainInsertNamed`), so no other core can
issue, admit or place between the fix and the placement. A btree's
rightmost leaf held the issue the same way from BB-S3 (BB-R1) until BD
withdrew it there. The heap's walk takes each page exclusive while it reads the link
and stops only at a page whose link is still invalid under that hold, so a
page another core linked on meanwhile is walked on to, never linked over.
The sorted fill carves its block under the hold of the tail it starts from,
holds each fresh page from its creation through its fill, and links it only
once filled (`heap::ChainAppendCarved`). **Until BB-S4 a heap relation had a
window there**: the bump and the placement were two latched spans, so a core
could issue `n`, a second core issue `n + 1` and place it first - out of
order inside one tail page, and refused `OutOfRange` across a tail-page
boundary, where the second core's growth opened a page with `min_key = n +
1` - and two cores growing one chain at once could overwrite a link and
orphan a page. None of the three is reachable now. A heap relation is
creatable only before SUS-1
(`instructions/v3.0.0/workorder-as-sus1-heap-suspended.md`).

### 4.1c `PURGE` frees a deleted key

> **`PURGE FROM [ns.]t WHERE <pk comparisons>` retires, keyless, the slot of every committed delete-marked row its window names, once no reader can see the row it deleted. After it the key is free, and an `INSERT` that names it is judged as if the key had never been placed.** BH (`instructions/v3.0.0/workorder-bh-purge-key.md`, PU1-PU12) built it; K1's one named exception (`docs/rules/keystoneid-invariant.md`).

**The statement** (PU1). Each conjunct compares the pk with a non-negative integer literal: `=`, `<`, `<=`, `>`, `>=`, or `BETWEEN a AND b`. The conjuncts fold to one window `[lo, hi)` through `CommandDispatcher::FoldPkWindow` - `DeclaredWriteBorrow`'s fold, the one recognizer of a pk window - clamped to `[1, 2^40 − 1]`. A conjunct the fold cannot read is **refused**, never skipped, because skipping one would widen the window (`id > 0 AND id != 5` would free 5). `PURGE` is a head matched by text, never a reserved word. It runs in **autocommit only**, on a **btree user relation only**, with the **admin role** (`RequiredRole`'s unclassified default). It records no pattern, trail or access statistics.

**What is freed** (PU2, PU3). A slot is a target when it is keyed in the window, delete-marked, its deleter is not in flight, and `TransactionManager::ResolvedForEveryReader(deleter)` holds - every live and future reader sees the delete. A live key refuses a **window of one key**, however it is spelled (`id = 5`, `id BETWEEN 5 AND 5`), `InvalidArgument`; a wider window passes over live keys. An absent key - never placed, rolled back, already purged - purges nothing and is no refusal, so a `PURGE` can be run again.

**The waits** (PU4, PU5). A delete-marked key whose deleter is in flight, or the one key of a one-key window whose latest writer is in flight, waits for that writer's decide; a key whose deleter some reader cannot see yet waits for the horizon. Both are **polled**, bounded by `exec::kPurgeHorizonWaitNs` (1 s, no configuration key) - not by `lock_wait_fault_net_ms`, which bounds a lost wake and logs its firing as a fault - and refused `TxnConflict retryable=1` naming the key. **Each attempt is a whole statement under a fresh owned transaction**, ended before the wait: so the `PURGE`'s own view is minted after whatever it waited for and never holds its own horizon down, and between attempts it holds no view, no `IS` and no `IX` - a DDL during the wait proceeds and the re-run waits for it. The synchronous path cannot wait and answers with the refusal at once.

**Judged whole, then written key by key** (PU6, BH-R6). The attempt takes the relation's `IX` and **no row or range unit** (BH-R4), walks the window under shared holds and collects its targets, writing nothing; the first pending key ends the attempt. Then, in key order and in drain mode, each target goes through `exec::PurgeKey`: `btree::BtreeHoldTombstone` finds and verifies the slot under the leaf's exclusive hold - **skipping it unless it is still the tombstone the judged deleter stamped** (PU12: a named `INSERT`, another `PURGE`, or a purge and re-insert on another core may have reached it first) - the version's spills are read off the held page, the slot is retired and `SLOT_RETIRE` logged at `kNoTxnId` under that hold (AT-S21), and after the hold each spill is released with `VARHEAP_RELEASE` at `kNoTxnId`. **Retire first, release second**: the reverse could release a value a re-run would release again. The `IX` is held through the last release, so `CREATE INDEX`'s backfill and an assertion build, safe only under their `X`, never meet one. A refusal while writing keeps every key purged before it, and its text says how many and that a re-run finishes the window; a refusal between a key's retire and its release purges that key with its spills leaked, and says so.

**A purged leaf takes its keys back** (BH-S5). Retired slots keep their directory entries and bytes, so a leaf a purge emptied, or left with one key, is full with nothing to divide - and a leaf with no key below an incoming id would append a new leaf whose low key equals its own. So **a full leaf with a retired slot is compacted before it is split** (`btree::CompactLeafAndInsert`): rebuilt in place with its `min_key`, sibling link and `grown_over` kept, every keyed version (delete marks included) back in key order and the incoming row where it sorts, its relayout epoch moved past every renumbered slot, logged as the one image of a `BTREE_SPLIT`; no parent changes. A copy-free scan for a retired slot comes first, so a leaf with none splits as before.

**The race with an `INSERT` of the same key** (PU12). The leaf's exclusive latch orders them: an `INSERT` that reaches the leaf before the retire meets the tombstone and is refused `AlreadyExists` (*"… a row with this key was deleted; PURGE frees its key …"*, BH-Q13); one after it meets a keyless slot and is placed, its record after the retire's.

**Logging, recovery, durability** (PU8, BH-R7). No record kind is added and the superblock stays 21: both records are today's, at `kNoTxnId`, so analysis never counts them a loser's and undo never reverses them; redo applies each with the rollback's own applier. A loser `INSERT` of the purged key is undone by key at mount. The attempt's `TXN_COMMIT` follows every purge record, so `PURGED n` carries the session's durability class, and its count reaches KWP as `rows_affected` (PU7).

**What it leaves** (PU11, BH-R8, BH-R9). No assertion departure (`DELETE` wrote it), foreign-key check, Cabin or index hook, undo record or trail entry is written. Secondary-index entries, Cabin entries and Waystone trails stay: every consumer re-checks a stored pk against the row now there, so a purged and re-placed key reads as the new row, once (BH-S1's Census B). An index entry for the purged version is never removed, so the equal-index-sort-keys defect gains a path: a key purged and placed again with a different covered value lengthens a run of equal sort keys. The issue cursor never hands a purged key out; only an `INSERT` that names it takes it again.

**The horizon's obligation, for every later view.** The gate is sound because every view that can read a superseded version of a user row is registered with the horizon, or reads only latest state, or is excluded by a relation `X` (BH-S1's Census A, which also closed a resumed READ COMMITTED park that re-minted its registered view above its walk's snapshot). **A new kind of read view must be shown to be one of those, or a `PURGE` may retire a row it is still entitled to read** - a wrong answer with no error.

**Refusals** (PU10), each with its byte: inside `BEGIN`, `NotImplemented` at byte 0, before the transaction is touched; a system relation, `Unsupported` at the name (checked before the qualifier, whose check calls `InitTableAccess`, and before the heap check; BH-Q17); a heap, `Unsupported` at the name, because its chain grows only at its tail; no `WHERE`, `NotImplemented` past the name; a conjunct outside the six forms - another column, `!=`, `IS [NOT] NULL`, `IN`, a function, a subquery, column to column - `NotImplemented` at its function, column or (for a bare subquery) the `WHERE` token; a negative literal, `InvalidArgument` at it; a window of one live key, `InvalidArgument` at the literal; a wait at the bound, `TxnConflict retryable=1`; a refusal while writing, its own code with the count; a session below admin, the role refusal.

## 5. Indexing

- A relation is stored either as a **heap chain** (§3.1b) or as a **clustered B+ tree** on the Keystone pk, chosen at `CREATE TABLE` and by nothing else. On a btree relation the tree *is* the storage, and a descent is authoritative: a miss means the row does not exist, and no scan follows. That authority is what lets a btree place a caller-named key wherever it sorts and refuse it only as a duplicate (§4.1); a heap's tail-only duplicate check cannot, so a heap refuses a named key below its high-water mark. A heap relation has no pk index at all, so a point lookup scans the chain.
- A btree **leaf is a heap page** — same slot directory, same tuple format, same MVCC header, same `min_key` and `next_page_id`. A clustered-btree relation is therefore not a second storage engine; it is the heap with a directory over it.
- **A leaf grows two ways.** An id above everything the full leaf holds opens a fresh right leaf and moves nothing — the append shape a monotonic sequence produces. An id that sorts *inside* a full leaf makes the leaf **divide**: the keyed versions are cut once - the rightmost leaf at the insertion point, any other at the median key - the upper part moves to a new leaf whose `min_key` is the split key, and the old leaf keeps its own. SQL produces both shapes, the second whenever a named key sorts below a full leaf's highest; §4.1 carries the invariant argument, the epoch consequence and the internal-node case. Below a full leaf a row takes the slot its key sorts to (§4.1), and **a split is logged as one `BTREE_SPLIT` record** (`wal.md` §5.2).
- **A new leaf is published separator first, sibling link last.** A leaf is reachable two ways — a descent routed by its parent's separator, and a scan following the previous leaf's `next_page_id` — and a grow writes them separately, so one of the two half-applied states is always possible when a page allocation or a page read fails part-way through. Only one of them is survivable, and the order is chosen on that: *separator written, link not* leaves a page allocated and unreachable to a scan, which costs an allocation and nothing else, because a failed promotion fails the insert and no caller kept a row in it. *Link written, separator not* is unsurvivable — the leaf sits in the sibling chain routed by nothing, so the **old** leaf goes on taking the ids the new one holds, it is full, and the next such id appends another leaf *in front of* the unrouted one. The chain then descends, and §3.1b's page-wise ordering — which the descent's exactness, and so a btree's duplicate check, rests on — is false for a btree relation. The ordering, and the fact that the final link write goes through the descent's own pin so it cannot fail, are in `src/storage/btree/btree.cpp`. **The heap chain has no such question**: nothing routes to a heap page, so the link is its only publication and it is written last (§3.1b).
- **A descent is authoritative across cores too, and what makes it so is a coverage check rather than a lock** (AT-S5c). A leaf covers `[min_key, right sibling's min_key)`; both ends are immutable, so the interval changes in exactly one way - a page spliced in on the right, which is what both split shapes do. Since AT-S5 a write runs where the session is, so two cores reach one relation's leaves, and a descent has **no latch coupling**: it releases an internal node before asking for its child, and a write descent must additionally drop the leaf's shared hold before taking it exclusive, the page latch never being upgraded (`page.md` §6). In either gap the leaf can give the top of its range away. So the descent asks, of the chain as it stands, whether the leaf still covers the key, and starts over from the root when it does not - bounded, and a restart makes progress because a splitter holds the old leaf across its own separator promotion. **The write path pays this unconditionally and a lookup pays it only on a miss**, a hit being authoritative however the chain has moved; a monotonic pk lands on the rightmost leaf, which has no sibling to ask. `BtreeSeekLeaf` asks nothing, and the reason is directional: a split moves keys only to the right, so a seek that is outrun lands to the *left* of where the key went, and the scan it feeds walks forward onto that page next.
- **A split finds every parent it will write before it writes anything** (AT-S16, `SecureParents` in `btree.cpp`). The descent recorded its path holding none of it, so since AT-S5 another core can divide a recorded parent first and move the split leaf - or, one level up, the divided node - to the new half, where a separator put into the recorded parent is routed by nothing. This reaches the append path too: two appends serialise on the tail leaf, but a divide under the same parent - which a named key reaches from SQL - waits for neither. So once the leaf is known to split, the insert climbs while the nodes are full, taking each parent **exclusive** and asking whether it still routes the key to the node below - exact, because that node is held and every change to its range is a divide of it - and re-descends from the root it was given on a miss (the operator's Q1 ruling, 2026-09-26: re-validate and re-descend). **Nothing is written until every parent is held**, so a refusal - `TxnConflict`, retryable, naming a stale path, after `storage::kMaxDescentRestarts` - leaves the tree as it found it rather than a divided node no parent routes to. The holds cannot deadlock: they are taken bottom-up a level at a time, a re-descent reads only above everything the insert holds, and nothing holds a node and asks for one below it or to its left. A restart makes progress by the descent's argument one level up: a divider holds every node it writes until its insert returns, so a re-descent blocks on it or reads it after its separator is in place.
- **A root is marked when a level grows over it** (AT-S16; the operator's word of 2026-09-28). The old root - a leaf (`kHeaderFlagGrownOver` in the heap header's `flags`) or an internal node (`kInternalFlagGrownOver`) - is marked under the hold the split already has, as the last write of the growth, after every step that can fail: the mark is the only thing on the page that says it now has a parent. A core whose memo still names it - another core grew over it, published or not; a memo drops at the next task boundary - reads the mark before it would grow a level of its own over it and is refused before writing, instead of growing a second root whose publication drops the other one's half. A page written before the mark existed carries 0, which claims nothing, so its arrival is not a format event; the mark is never cleared and a divide's rebuild keeps it.
- **A lookup's answer is held, not only its status** (AT-0 item 12, marked (a); `workorder-at-m3-uniformity.md`). A hit's `(page, slot)` is true only while the leaf is: a division rebuilds the old leaf and moves its upper half, and a placement mid-leaf shifts every slot after it, so a slot read after the leaf is released can be another row. `BtreeLookup` therefore returns the leaf with the location - the descent's own `PageRef`, shared for a reader and exclusive for a writer that asks `kWrite` (a point `UPDATE`/`DELETE`, and rollback's relocation) - and **no caller re-fetches the page by id**. A location carried past its hold - the step runner's probe memo, a Cabin entry between its two serving phases - is a hint, and is read only through `VerifyTupleAt`, whose relayout-epoch and Keystone checks turn a moved row into a miss and a re-descent. Nothing reads under the hold across a suspension.
- **Waystone** (`waystone-concpets.md`) is the engine's other access structure: `(fetch_id, arg_hash)` → the Keystones a previous execution of that pattern instance found, across relations. It is advisory and validated on use, and it may replace a *lookup* but never a *search*.

## 6. Page-Latch Consistency

There is no single canonical in-memory tuple and no hash table enforcing that an identical tuple exists at most once in program memory. Consistency is kept at the **page** level.

- A page frame is **pinned** for the duration of any access and **latched** — shared for reads, exclusive for structural mutation (slot directory changes, compaction, relayout). Tuple bytes are read and written directly within the pinned, latched frame; there is no tuple-identity cache to keep coherent with the page.
- Latching is **a cross-core lock now, and the spec change came first** (`rules.md` §3). This read: latching is core-local, a page is owned by exactly one core, its latch serializes cooperative tasks on that core across suspension points, it is not a cross-core lock, and pages are not on `rules.md` §3's declared shared list — *adding them would be a spec change there before a code change here*. That is exactly the order it happened in: AM-S1 declared the page frame's latch word on `rules.md` §3's list, and AM-S2 built the shared pool under it. A frame's latch word is held by a **core**, re-entrant for that core and never upgraded, and it serializes holders on different cores as readily as tasks on one (`page.md` §6). **A write routed to the relation's owner until AT-S5**, with only the page cache beneath it shared; a write runs where the session is now (`crosscore.md` CC11), so the latch's cross-core half is load-bearing rather than latent, and `btree.cpp`'s descent is the first place that showed it (AT-S5c). What still goes through server-side forwarding is a *read* of a relation this core cannot serve (`protocol.md`, `crosscore.md`, D18).
- Executors may copy tuple bytes into private working buffers. These are ephemeral projections; they compete with no canonical copy, because there isn't one.
- The Keystone word's atomic-CAS requirement (§4) is independent of latching: even under a latch, the word is read and written as a `std::atomic<uint64_t>` so fields never tear.

Frame reclamation under this model is `docs/spec/eviction.md`'s: every accessor returns a pinned `PageRef`, and a CLOCK sweep reclaims only unpinned frames of evictable classes.

## 7. Statistics-Driven Physical Relayout

KDS collects access statistics and uses them to **physically optimize tuple placement**, starting with heap pages.

**Collection and the shadow planner are built; no mover is.** `sys.access_stats` records one row per access *shape* — `(kind, rel_id, column_mask)` — with how often it ran and when it last ran, written for every access kind through one call with no per-kind branch (`include/kds/stats/access_stats.hpp`). Distinct shapes are capped at `kMaxAccessShapes` (4096, `include/kds/catalog/rows.hpp`); past it the write fails `ResourceExhausted`. `SHOW ACCESS` reads it, and `SHOW RELAYOUT` weighs it with the decay score into candidate relayout plans (`docs/spec/physical-optimizer.md` §5).

The shape is keyed by **columns, never values**: `WHERE flag = 1` and `WHERE flag = 2` are one row. That is what bounds the relation by the schema rather than by the data, so it needs no eviction policy and no directory — the unbounded axis, *which arguments repeat*, is Waystone's and stays there (`waystone-concpets.md` §5). The two layers answer different questions and are deliberately not merged.

The kind split is what makes the data worth having: a walk driven by an equality on a non-pk unindexed column is `kFilterScan` rather than an undifferentiated `kScan`. The two cost the same and mean entirely different things — one is a statement that asked for everything, the other a statement that asked for a few rows and had to read all of them to find out which, which is exactly the case an index or a clustering decision would fix. `kIndexProbe` and `kIndexRange` (`docs/spec/index.md` §8) are counted through the same call, so a relation's history distinguishes "searched every row for a few" from "descended an index for them". A `kFilterScan` beside a `kIndexProbe` on the same relation names two columns with different treatment. Neither is trail-replayable — invariant 9's line is lookup versus search, and both are searches.

Relayout must respect the `min_key` insertion rule (§3.1), bump the page epoch (§3.1a) so every recorded location on that page becomes untrusted at once, and — **on a btree-clustered relation only** — keep the tree consistent, which is a tree restructure (`docs/spec/physical-optimizer.md` R8). A heap relation has no pk index, its Cabin is relocation-invariant by value = pk indirection, and secondary indexes exist only on btree relations — so a heap-relation mover maintains *nothing but the epoch*. Under the fixed-length rule (§3.3) a relayout is a copy of fixed cells — exact fill-factor math, no per-tuple size negotiation — and `kVarHeap` pages are outside its jurisdiction entirely (§3.4).

Key-boundary re-partitioning mainly benefits range locality; for single-pk point lookups the acceleration comes from Waystone instead. The two coexist and address different shapes.

No relayout is enacted: `SHOW RELAYOUT` reports every candidate plan with its predicted benefit and names the `docs/spec/physical-optimizer.md` §6 gate that blocks it, and no relayout bumps a page epoch.

## 8. Invariants

Never violated, never "temporarily" bypassed.

1. Page size is 8192 bytes; page ids are `uint32_t`; `0xFFFFFFFF` is reserved as invalid.
2. A heap page's `min_key` is immutable after creation, until the page is freed. A page id reused after a dropped relation's reclaim is a new creation, its `min_key` fixed again by that creation (`drop-table.md` DT1).
3. No tuple with `id < min_key(page)` is ever placed in that page, including by relayout - for the life of the page, which a free ends.
4. Tuples within a heap page are unordered by contract. A btree leaf is sorted by placement: every keyed slot ascends by Keystone id, live and delete-marked alike, whatever order rows arrive in and at every core count, because a row takes the slot index its key sorts to (BD-R1, BD-R2); a retired slot carries no key and sits anywhere. Readers rely on it through the `ORDER BY <pk>` elision, the Cabin serve's pk sort and the index step's sort (§4.1).
5. The Keystone column is exactly `id:40 | flags:8 | reserved:16`.
6. The Keystone word is read and written atomically as a `uint64_t`; on-disk encoding uses explicit shift/mask, never compiler bitfields.
7. Ids stored outside the tuple header are zero-extended `uint64_t` with the upper 24 bits zero.
8. Waystone is advisory: deleting it wholesale may cost performance and must never change a query result.
9. Waystone is never **authoritative**. A reader may consult it for *where to look*, provided it treats a missing or stale entry as a miss, checks the Keystone id of the tuple actually found at the reported location, applies MVCC visibility exactly as the authoritative path would, and falls through to that path — a btree descent on a btree relation, a chain scan on a heap one — on any mismatch. It chooses where to look, never what is visible.
10. No single canonical in-memory tuple is enforced; consistency comes from page pin and latch discipline (§6).
11. Every relation's pk is a **unique 40-bit `id`, never rebound except by `PURGE` (§4.1c), never updatable, and never carried outside the Keystone word**. Where the id comes from (§4.1) is a per-**row** fact — the `INSERT` names it or omits it — and there is no key mode, no `CREATE TABLE` declaration, and no relation that refuses either arity. **A Keystone id is bound to at most one committed tuple in the lifetime of a relation, except across a `PURGE` that freed it** (K1, BD-R4). `sys.tables.next_id` is a **high-water mark on what has been placed**: `AllocateRowId` draws from it for an omitted key, `AdmitExplicitRowId` advances it past a named one at or above it, and it never moves backwards. **Issued keys are a sequence in issue order**, unique among issued ids by the mark; on a btree the placement's duplicate check catches one a named key took first, and the issue draws again (§4.1). **Named keys carry no order**: on a btree a named key's uniqueness is the descent's - a slot keyed `k`, live, or delete-marked and not purged, is `AlreadyExists` - and it is otherwise refused only as exhausted, `OutOfRange` (BD-R5); a leaf's slot order is its key order by placement (invariant 4). On a heap a named key **below** the mark is refused `OutOfRange` (BB-R12, kept by BD-Q4 (a)), §3.1b's tail append, page-wise ordering and tail-page-only duplicate check being that ascent, and a row's id is fixed under the tail's hold (BB-R7). No relation records an exception: `kUnordered` is deleted (BB-R10). What nothing relaxes: the pk is not updatable, the cursor never issues an id twice, and a committed key is never bound again except after the `PURGE` that freed it.
12. The tuple MVCC header is exactly `trx_id:48 (zero-extended to 64) | undo_ptr | data_len | flags` = 20 bytes. There is no `xmax`; a version's validity interval is reconstructed from the undo chain, and DELETE is the slot's `DELETED` mark plus the deleter's `trx_id`.
13. **Every tuple is fixed-length.** A relation's row size is a schema constant; variable-width values occupy tagged cells of exactly `kds.inline_cell_width` bytes (§3.3), and that width is instance-pinned in the superblock. No code path produces a tuple whose size differs from its relation's constant.
14. **Var-heap values are immutable per version** and `kVarHeap` pages are never relocated; the class is logged, headered, and checksummed — authoritative data, not advisory (§3.4).

## 9. Open Decisions

The decisions this section once listed are not recorded here. Each section above states what holds today; a rule this file does not state is not made, and an implementer who needs one asks rather than assumes.
