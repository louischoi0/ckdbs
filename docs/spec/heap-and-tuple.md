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
- **Tuples within a page are unordered** — heap append semantics, O(1) insert into free space.
- Because `min_key` never changes, a reader can prune pages by key range **without locking**. That is the property the immutability exists to buy.
- Relayout honors the target page's `min_key`. Moving tuples across key ranges means writing them into **new pages with newly assigned `min_key` values**, never mutating an existing page's.

### 3.1a Per-page epoch counter

Every heap page header carries an **epoch counter**, bumped whenever the tuples physically on that page move. Unlike `min_key`, it is mutable by design.

Waystone records a page's epoch when it observes a tuple's location, and a consumer trusts that location only while the recorded epoch still matches the page's. This is how an advisory structure avoids becoming a second authoritative index: relayout bumps one counter instead of synchronously rewriting every entry that pointed into the page.

The epoch lives in the **common page header** as `relayout_epoch` — `PageHeaderFields::reserved0`, offset 16, u64, durable by construction (`page.md` §2, `docs/spec/physical-optimizer.md` R4). A page that has never been relayouted carries 0. Wraparound is unreachable at u64 width rather than handled. **Pairing rule:** no consumer may accept a location on epoch equality alone — the epoch is a fast whole-page invalidation layered over the Keystone-id check (K1), never a substitute for it. The executor records it at access and `exec/tuple_verify.hpp` compares it for Waystone replay and Cabin hints alike. No mover exists, so no relayout bumps it; the one operation that does is the btree leaf division (§4.1).

### 3.1b Chain growth by tail append

A relation is a **chain of heap pages** linked through the `next_page_id` tail reservation (§3.2), rooted at `sys.tables.desc_page_id` (`include/kds/storage/heap/heap_chain.hpp`).

- **Growth is tail append, never a split.** Every insert goes to the last page. When it has no room, a new page is allocated, the tuple is written into it, and only then is the page linked on — the link is what makes a page reachable, so publishing it first would expose an empty tail.
- **A new page's `min_key` is the id of the tuple that caused the growth**, the smallest id it can ever hold, since ids only increase. No existing page's `min_key` is touched, so §3.1's immutability holds by construction.
- **Each page's ids lie entirely below the next page's `min_key`.** The chain is key-ordered page by page while tuples within a page stay unordered — "semi-sorted" holds across pages as well as within one. Two consequences are relied on in code: a duplicate incoming id can only be in the tail page, so the check is O(1) pages rather than O(chain); and an id below the tail's `min_key` has nowhere legal to go and is refused as a backwards sequence.
- **All three of the above rest on ids ascending, and that is the whole of what a heap relation refuses.** A heap relation *may* be told its keys — what it may not be told is a key below its high-water mark, refused as `OutOfRange` in `Catalog::AdmitExplicitRowId` before the chain is touched (§4.1). The mark is this section's three properties written as one number: at or above it, the incoming id is above every id the relation has ever placed, so the tail is the only legal page, the new page's `min_key` is still the smallest id it can hold, and a duplicate can still only be on the tail. Below it, all three fail at once — and the second failure is the dangerous one, because a page opening below an id already on its predecessor makes the tail-only duplicate check admit a duplicate silently. The refusal is per *id*, never per relation, which is why nothing in this section is conditional.
- **No free-space reuse.** A page that fills and then has rows deleted is never revisited; the chain only grows at the tail. A delete-heavy relation grows monotonically.
- Walks are bounded by `kMaxChainPages` (2^20 pages, 8 GiB per relation). Exceeding it is `Corruption` rather than a loop, since a cycle in the links would otherwise hang a request.

No heap page is ever divided, compacted or reused, and no tuple moves off one. §4.1's btree leaf division is not a heap split and not a precedent for one: a divided leaf is re-routed to by the separator its division promotes, while a heap page is reachable only through the chain that precedes it, and a heap relation's ascending ids never sort inside a full page.

### 3.2 Page layout

- The slot directory grows downward from the heap area offset; tuple data grows upward from the top; free space is the gap (`upper - lower`).
- The page tail permanently reserves `sizeof(PageId)` bytes for the `next_page_id` chain link, excluded from free-space accounting.
- The per-tuple MVCC header is **`trx_id` (48-bit writer, zero-extended to 8 bytes) + `undo_ptr` + `data_len` + flags — 20 bytes, with no `xmax`**. A version's death is the next version's birth: walking the undo chain already names the overwriting transaction, so storing that boundary a second time in the older version would be recording one fact twice. `trx_id` is whichever transaction last stamped the version — insert, overwrite, or delete-mark. The lock-slot role `xmax` plays in PostgreSQL belongs to the Keystone flags byte here (§4).
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
| `flags` | 8 bits | Transaction/status byte, Oracle lock-byte style; may reference a per-page transaction slot. Tuple status such as `DEAD` lives in the slot directory, not here. |
| `reserved` | 16 bits | Writers set 0, readers ignore. |

**Every relation's pk is a unique 40-bit id that is never rebound and never updated.** *Where* the id comes from is a per-**row** choice — the `INSERT` names it or omits it (§4.1) — and *whether it ascends* is a consequence of the storage type. What is not a choice is uniqueness: an id names exactly one tuple for the lifetime of the relation, and every consumer rests on that. The provenance of the value does not matter to any of them.

Uniqueness is obtained by two different means, and which one runs is decided by the storage and the id, never by a declaration:

- **The mark.** `sys.tables.next_id` never moves backwards, so an id at or above it is above every id the relation has ever placed. This covers every omitted-pk insert on any relation, and every supplied key on a **heap** relation — where it is the *only* available proof, because a chain has no descent. **Proved without reading a page.**
- **The descent.** On a **btree** relation a supplied id may sort anywhere, and there the mark proves nothing: a relation whose mark is 1000 may have had 500 since its first insert. Uniqueness is proved instead by the descent, which lands on the one leaf that may hold the key and finds the duplicate or does not.

What holds regardless:

- The cursor is **persistent, not derived**: `sys.tables.next_id`, a **high-water mark on what has been placed**. `Catalog::AllocateRowId()` returns it and advances; `Catalog::AdmitExplicitRowId()` advances it past a supplied id with `max()`. Deriving it as `max(id) + 1` would reissue an id after the highest tuple is deleted, handing a new tuple the identity of a retired one. It is what `SHOW BUDGET` and `DESCRIBE` derive K4's lifetime budget from, so it must never fall behind what was placed. The first id issuable is 1 (`kFirstRowId`); 0 stays reserved for "unset".
- **The two id sources share the one mark, and that is what keeps them apart.** An issued id always clears every key the caller has named, and a named key at or above the mark clears every id the engine has issued. Only a *below-the-mark* named key can meet an issued one, only a btree relation admits one, and there the descent is the answer.
- Ids are unique by construction, **not gapless** — an insert that fails after allocating burns one, and a caller may skip a range outright. Nothing depends on gaplessness.
- Ids are **monotonic until a below-the-mark key lands**, which only a btree relation permits and which the relation records (`sys.tables.key_order`, §4.1). Page-wise `min_key` ordering is preserved by a leaf division, so range pruning (`kRange`'s tail prune, `src/exec/step_vm.cpp`) does not depend on slot order.
- The pk is carried **only** by the Keystone word, never also as a body column: `EncodeRow()` writes `[Keystone word][columns 1..n-1]`. Storing a key twice is how the two copies come to disagree. A supplied id is *named* in the statement's first position and still lands only in the Keystone word.
- The pk **cannot be updated**. It is the tuple's identity, not a field of it.
- A relation's first column must be declared with an **integer type** (`catalog::CheckKeystoneColumn`), checked at `CREATE TABLE`. Its declared width is display metadata: the id lives in the 40-bit Keystone field regardless, so a narrow declared type does not cap the sequence.

Implementation rules:

- Encode and decode with **explicit shift/mask helpers only**. **Never use C/C++ bitfields** for an on-disk format — their layout is implementation-defined and KDS must be portable across architectures.
- The whole word is updated with **atomic `uint64_t` operations (CAS)**. Fields must never tear across writes.
- External structures — B+ tree keys, `min_key`, Waystone entries — store the id as a **zero-extended `uint64_t`** with the upper 24 bits zero. Ids are never 5-byte-packed.

Ids are never reused. Sequence exhaustion is reported as `OutOfRange`, never wrapped.

### 4.1 Caller-supplied keys

> **Every relation takes a caller-supplied primary key or issues one when the `INSERT` omits it, per row. On a btree relation the key may sort anywhere; on a heap relation it must not fall below the relation's high-water mark.**

There is no key mode and no `CREATE TABLE` declaration of who names the key: `INSERT` is the statement that names one, so the fact lives in the row's arity, and no relation refuses either arity. Whether a page's slot order is still its key order is an **observation**, not a declaration — `catalog::KeyOrder {kAscending = 0, kUnordered = 1}` on `sys.tables.key_order`, flipped once, ever, the first time `AdmitExplicitRowId` admits a key below the mark.

*In code.* `catalog::KeyOrder` (`include/kds/catalog/well_known.hpp`), persisted at `SysTableRow::kKeyOrderOffset` — the byte a former key-mode field held, whose on-disk 0 and 1 read as "every id here ascended" and "an id may have landed out of order" — cached on `TableAccess` (**the one cached field there that is not a DDL fact**) and published on the flip by `CatalogCache::MarkKeysUnordered`, an *in-place* update rather than a `BumpVersion`. In place because the flip happens inside a running `INSERT` that is holding a `const TableAccess*`: the same one-field/one-owner license the index root and the desc page carry, and for the same reason — a bump would leave that access dangling. `Catalog::CreateTable` takes no key parameter; `key_order` is set to `kAscending` by `InsertRelationRow`, because a relation holding no ids has had none land out of order.

**Syntax.** A trailing bare identifier in the same optional slot as the storage clause:

```sql
CREATE TABLE t (id int64, qty int64) BTREE EXPLICIT;
```

`EXPLICIT` is accepted and sets no field: it states what is true of every relation — the caller may name this relation's keys. `ASSIGNED` is `Unsupported` with its byte, because it means "the engine issues every id and supplying one is refused", and on the relation the statement would create, supplying one is admitted; accepting a spelling and enforcing something else is worse than refusing it. `HEAP BTREE` is an `InvalidArgument` repeat; `ASSIGNED EXPLICIT` is refused at the first word. All words are case-insensitive **identifiers, never reserved keywords**, so none of them moves a fingerprint hash. Storage does not follow the word: `CREATE TABLE t (...)` and `CREATE TABLE t (...) EXPLICIT` are both heap-clustered.

**`default_key_mode` is not a config key.** It stays *known* so its absence can be reported: an instance file naming it is refused at startup with a message saying why, rather than falling to the generic unknown-key error.

**A heap relation refuses a key below its high-water mark**, `OutOfRange`, naming the mark and pointing at `BTREE` as the storage that takes keys in any order. This is the one restriction. A chain grows only at its tail (§3.1b), so a key below the tail page's `min_key` has no legal page — inventing one would either mutate a `min_key` (invariant 2) or place a tuple below one (invariant 3). And the chain's duplicate check reads the **tail page alone**, which is sound only while every earlier page's ids sit below the tail's bound; a page opening below an id already on its predecessor breaks that quietly, and a duplicate pk is then admitted with no error at all. Both properties are the ascent, so refusing below the mark is what keeps §3.1b true rather than a second rule that could drift from it. At or above the mark the incoming key is above every id the relation has ever placed, so the tail is the only legal page *and* the only page a duplicate could be on — the two questions the descent answers on a btree, answered here by one number and no page read.

**The admission gate: spellability, then the storage's ordering rule.** `Catalog::AdmitExplicitRowId(oid, id)` first refuses an id outside `[kFirstRowId, kMaxKeystoneId]` as `InvalidArgument` — 0 is reserved for "unset" and a value ≥ 2^40 cannot be stored in the Keystone field by any path — before the catalog page is touched at all. Then:

- **At or above the mark**, either storage: the mark moves to `id + 1`, persisted before the caller places anything.
- **Below the mark, heap-clustered**: the `OutOfRange` above. Nothing is written, so a refused key burns no mark.
- **Below the mark, btree-clustered**: admitted on the strength of the descent that follows. The mark does not move — it is a ceiling on what has been placed and this id is under it — and `key_order` flips to `kUnordered` if it was not already. Guarded on the current value, so a backfill of ten thousand old ids writes the catalog page **once**, not once per row.

There is no mode check in this function, and none in `AllocateRowId` or `AllocateRowIdRange` either: all three run on every relation.

**Both writes outlive a rollback**, and deliberately. They are made outside the caller's transaction (`wal::kNoTxnId`), so a `ROLLBACK` after a named key leaves the mark advanced — burning an id, which K3 calls free, exactly as an issued id burns on an aborted insert — and leaves `key_order` flipped even though the key that flipped it is gone. The flag is safe in that direction and only that direction: a relation wrongly marked `kUnordered` pays a per-page sort it does not need, while one wrongly marked `kAscending` answers `ORDER BY <pk>` out of order. Un-flipping on rollback would have to prove no *other* below-mark key had landed meanwhile, which is a scan, to save a sort.

The mark is a **high-water mark, not a gate**: it exists to keep K4's lifetime budget and the 40-bit exhaustion check truthful about the id *space* a relation has consumed, and to keep the cursor from ever issuing an id already placed. A descending id on a btree relation costs no catalog write whatever.

**On a btree relation uniqueness is proved by the descent, not by the cursor.** Once ids may descend, `next_id` says nothing about what is in use: a relation whose mark is 1000 may have nothing at 500 or may have had 500 since its first insert. So `BtreeInsert` is the authority. It descends to the one leaf whose key range covers the id, scans that leaf's live slots, and returns `AlreadyExists` naming the page and slot on a hit. The check is complete rather than a sanity check, because the descent is exact — no other leaf may hold the key. Two consequences:

- **A delete-marked version still holds its key.** The scan reads slots, and a `DELETED` slot is still a live slot until retirement; nothing retires a slot today. So a pk that has been `DELETE`d cannot be re-supplied. That is issue-once (K1) holding for named keys by the same mechanism it holds for issued ones, and it is a restriction a caller will meet.
- **`BtreeInsert` still refuses an id below its landing leaf's `min_key`** as `OutOfRange`. The descent makes that unreachable — a separator *is* a child's `min_key` — so it is a defensive check on the two ever disagreeing, not a policy about ordering.

**A full leaf divides** (`SplitLeafAndInsert`, `src/storage/btree/btree.cpp`). A monotonic sequence never reaches it: an id above everything in a full leaf is an *append*, which opens a fresh leaf and moves not one byte. Only an id that sorts *inside* a full leaf forces a division. Why that is legal inside the invariants, stated precisely because it is the one place tuples move without a mover:

- **Invariant 2 holds** — the old leaf keeps its `min_key` **unchanged**. A division moves the *upper* half out, so the low bound a lock-free reader may already have pruned by never moves.
- **Invariant 3 holds on both sides** — everything that stays was at or above the old bound already, and the new leaf's `min_key` is the split key, which is by construction the smallest id moved into it. Neither page ends up holding a tuple below its own `min_key`.
- **The boundary is chosen by key, not by slot.** A leaf fed descending ids is not in slot order (invariant 4 always permitted that), so splitting at slot *n*/2 would divide it at an arbitrary key. The live versions are copied out, sorted by key, and cut at the median.
- **The old page is rebuilt, not edited.** `RetireSlot` marks a slot dead without reclaiming its bytes — reclamation is a purge pass's job and no purge exists. Retiring the moved half would therefore leave the page exactly as full as it was, and the division would make room for nothing, which is the entire point of it. So the page is reformatted and the staying half written back.
- **The old page's `relayout_epoch` is set to `old + 1`.** Every tuple on it changed slot and half of them changed page, which is a relayout in everything but name, so §3.1a's pairing rule applies: every Waystone trail entry and Cabin hint pointing into that page becomes untrusted at once. It is set to one past the old value rather than bumped from the zero the reformat left, because an epoch that went backwards would let an entry recorded at the old value compare equal again — the one thing the field exists to stop.
- **Delete marks travel with the version they belong to.** A moved version carries its deleter's `trx_id` and arrives still marked; re-inserting the payload alone would resurrect a row some snapshot has already been told is gone.
- **Secondary indexes need nothing.** An index entry's sort key is `key || pk` (`index_page.hpp`) and its payload is the pk — never a location — so a division is invisible to them. The undo chain is likewise addressed by `undo_ptr`, not by where a version sits.
- A leaf holding **fewer than two live tuples** cannot be divided: no boundary makes room, because the row is near page-sized. Reported as the `OutOfSpace` it is, naming the reason, rather than producing an empty leaf a descent would route to and never satisfy.
- **A leaf's slots are not necessarily in key order**, and the lookup path does not assume they are. `FindSlotForId` keeps its binary search as an optimization for the ordered case and falls through to a linear pass, which is what makes the answer correct in every case: a supplied id appended into a leaf can sort below its neighbours, and a division re-lays a page by key rather than by slot position. An unsorted leaf costs a wasted log2(n) probes and still returns the right answer.

**Promotion into a full internal node divides it** (`PromoteSeparator`). Two shapes, told apart rather than assumed. A separator sorting above every entry the node holds takes a right-split with no movement — a new node whose only child is the new subtree — which is the append case a monotonic sequence produces exclusively, and it is correct and free there. A separator sorting *inside* the entries, which only a caller-supplied id can produce, divides them: the **median separator moves up** rather than being copied, its child becomes the new node's leftmost child, and the lower half is written back with the original leftmost child untouched. Copying the median instead — the leaf's rule — would route every key at exactly that value into a subtree that no longer holds it. Telling the two apart is not optional: promoting an interior separator by the cheap path would strand every subtree sorting above it, which is silent data loss rather than a wrong answer anyone would notice.

**`INSERT` arity is per-row and two-valued.** `ncols` values means the caller names the key and `values[0]` is it; `ncols - 1` means the caller omits it and the engine issues one. Both are legal on every relation, row by row, and a wrong length is refused naming **both** accepted counts — with two of them, a message naming one reads as an off-by-one against whichever the writer did not mean. The two counts cannot be confused: pk-plus-*n* columns is *n* + 1 values, pk-omitted is *n*; `INSERT` is positional with no column list (`parser/ast.hpp`'s `InsertStmt`) and no body column may be omitted individually, so a row's length names one reading and not the other, on every relation.

The supplied pk must be an **integer literal** — the gate runs before anything is placed and must not depend on evaluation — and a non-integer or negative value is refused carrying the offending token's byte. When the row names its key, the pk is split off `values[0]` once, so everything downstream (the FK forward check, assertion admission, `EncodeRow`, the Cabin witness, index maintenance) receives the shape it expects: the columns *after* the key. `AdmitExplicitRowId` sits at exactly the position `AllocateRowId` occupies on the other arity — after `enforcer_.AdmitInsert`, before `EncodeRow` — so a refused row burns nothing.

**Bulk `INSERT`** runs every row through that same single-row pipeline in statement order, so a bulk statement may name keys in any order, mix named and omitted rows, and each row is admitted, placed and indexed exactly as if it had arrived alone. The **sorted-fill fast path is engaged only when every row omits its key** (`SortedFillEligible` plus a per-statement check at the call site): the fill carves one contiguous id range up front and appends in order, which leaves no place for a key the caller chose. Ineligibility, never a refusal — a statement that names keys still runs, through the per-row path. The check is per statement rather than per relation because naming a key is a property of the row; what stays on `SortedFillEligible` is the relation-shaped half.

**Row-id leases work on every relation.** `AllocateRowIdRange` refuses nothing for a key reason, which is what lets a peer core take the omitted-pk arity on any relation it owns. The one consequence: a carve spends its block from the mark's point of view before those ids are placed, so a *named* key landing inside a live carve meets the leased id when the peer places it. On a heap relation that cannot happen — a named key must be at or above the mark, which the carve has already moved past its own block. On a btree relation the descent reports it as the duplicate it is, `AlreadyExists`, to whichever of the two lands second.

**A peer core refuses a named key, per row.** Admitting one writes the relation's `sys.tables` row — the mark, or the `key_order` flip — and that page is the system core's. The refusal is in `InsertOneRow`, beside the admission it is about. A peer may write any relation it owns on the omitted arity, drawing from its own id lease and writing no catalog page at all.

**The pk is not updatable** (K2). `exec::CompileAssignments` refuses a pk `UPDATE` at compile time as `Unsupported` with the column's byte, regardless of provenance. Naming a key at insert and changing one afterwards are unrelated permissions; only the first is granted.

**What none of this touches:**

- **The heap chain's storage code.** `ChainInsert` refuses an id below the tail page's `min_key` as `OutOfRange` and checks duplicates on the tail page alone; the mark check in `AdmitExplicitRowId` is what keeps those two facts reachable, and it sits above them. Nothing divides a heap page or moves a tuple off one (§3.1b).
- **Waystone, Cabin, secondary indexes and foreign keys**, which key on the id's *value* and never on its provenance or its order.
- **The 40-bit budget (K4)** bounds the id *space*, not the insert count. A relation fed sparse named keys exhausts it after fewer rows; both places the budget is read — `DESCRIBE` and `SHOW BUDGET` — derive it from `next_id`, which the high-water advance keeps truthful.

**`DESCRIBE`** reports `key_order=ascending|unordered` after `clustered_type=` on the summary line — an observation, answering the question someone reads that line for: whether a walk's pk order can be trusted. The pk column reports `autoincrement=if-omitted` on every relation, and every other column `no`. Neither `yes` nor `no` is true of a pk — the sequence runs when the `INSERT` omits the key and does not when it names one, and both are legal everywhere — so printing either would be printing something untrue for a field's convenience.

**`ORDER BY <pk>` emits each page in key order once a relation is `kUnordered`.** `exec::CompileStepChain` accepts the driving relation's pk as an `ORDER BY` target, and while the sequence is monotonic it discards the clause outright: a walk emits a page's slots consecutively in slot order (`RunWalkStep`, `src/exec/step_vm.cpp`), and an id at or above the mark is appended above every id already on the page, so slot order *is* key order. A key admitted below the mark can be appended below them, so from then on the two diverge — **within one page only**, since page-wise `min_key` ordering is preserved by a division. So the clause sets `Step::emit_in_key_order` and the walk emits that page's live slots sorted by Keystone id.

The flag is read off `key_order` rather than off the storage type, and that is the point of keeping the byte: a btree relation fed only ascending keys is exactly as free here as one that never took a named key. Reading the storage type instead would charge a per-page sort to every btree relation in the engine for a divergence most of them never produce. The Waystone replay's ordering (`step_vm.cpp`) reads the same flag for the same reason.

**Recovery**: the `key_order` flip rides the **same logged catalog write** as the high-water advance — one `OverwriteLogged` of one `sys.tables` row — so it redoes with it and a crash cannot come back reading `kAscending` on a relation that took a below-mark key. The flip's failure mode is not the mark's: a lost mark burns or reissues ids (K1's class), while a lost flip would discard an `ORDER BY <pk>` that the relation now needs and answer it out of order — which is why it is in the row the mark already writes rather than a second, unlogged field beside it.

Tests: `tests/supplied_key_test.cpp` end to end, the admission cases in `tests/catalog_test.cpp`, the leaf-division cases in `tests/btree_test.cpp`.

### 4.1a Monotonicity is per **range** once inserts spread

§4.1's own argument one level down. Everything above holds; what changes is
the scope over which "ascending" is a claim.

**A relation's ids do not ascend in issue order once more than one core
inserts into it.** Under id-block-aligned insert spreading each core issues
from its own leased block, ranges align to block boundaries, and a core
appends to its own range's tail. So two rows inserted a microsecond apart on
different cores carry ids thousands apart, and the *later* one may carry the
*lower* id. Per **range**, ids still ascend exactly as §3.1b requires: a
range is one chain, its block is contiguous and issued in order, and a
higher block belongs to a different chain.

Three consequences, each a property of the mechanism rather than added by
this sentence:

- **Invariant 3 is satisfied per range, and structurally.** A range's head
  page is created with `min_key = lo` (`Catalog::CreateRangeEntryPage`), and
  every id the owning core issues into it comes from a block starting at
  `lo`. Nothing below the boundary can land in that chain even by mistake.
- **`sys.tables.next_id` stays one high-water mark for the relation.** It is
  what every block is carved from (`AllocateRowIdRange`), so ids remain
  globally unique across cores by construction — K1's issue-once contract —
  and the mark is still a ceiling on what has been placed. It is not, and
  never was, a statement about the order rows arrived in.
- **`key_order` is unaffected, and that is deliberate.** A block is issued
  ascending within its chain, so no page ever takes an id below one already
  on it, and slot order is still key order *within a page*. `kUnordered`
  records a below-mark key landing, which spreading never produces — every
  block is carved *above* the mark. `ORDER BY <pk>` over a spread relation
  is ordered by the fan-in's range-order concatenation, not by this flag
  (`crosscore.md` §2a).

**What a caller may not infer:** comparing two ids of a spread relation
orders them in the *id space*, never in time. That inference is unavailable
across relations and across histories (§4.1); it is unavailable within one
relation once a second core has taken a block of it.

**Spreading is off by default.** `range_size_ids` ships as `kRangeSizeOff`
(0); `kRangeSizeIdsDefault` (65,536) is the size a range measures once
spreading is on, not a default (`include/kds/server/range_alloc.hpp`). With
spreading off — every relation until an operator sets it — the pk is an
identity **and a sequence**, monotonic in issue order, and a client may rely
on it; with it on, the pk is an identity and nothing more. A single-core
instance never produces the spread case.

## 5. Indexing

- A relation is stored either as a **heap chain** (§3.1b) or as a **clustered B+ tree** on the Keystone pk, chosen at `CREATE TABLE` and by nothing else. On a btree relation the tree *is* the storage, and a descent is authoritative: a miss means the row does not exist, and no scan follows. That authority is what admits a caller-named key **below** the relation's high-water mark, which is the one thing a heap relation refuses. A heap relation has no pk index at all, so a point lookup scans the chain.
- A btree **leaf is a heap page** — same slot directory, same tuple format, same MVCC header, same `min_key` and `next_page_id`. A clustered-btree relation is therefore not a second storage engine; it is the heap with a directory over it.
- **A leaf grows two ways.** An id above everything the full leaf holds opens a fresh right leaf and moves nothing — the append shape a monotonic sequence produces. An id that sorts *inside* a full leaf makes the leaf **divide**: the live versions are cut at their median key, the upper half moves to a new leaf whose `min_key` is the split key, and the old leaf keeps its own. The second shape is produced by a caller-named key below the relation's high-water mark, which only a btree relation admits; §4.1 carries the invariant argument, the epoch consequence and the internal-node case.
- **A new leaf is published separator first, sibling link last.** A leaf is reachable two ways — a descent routed by its parent's separator, and a scan following the previous leaf's `next_page_id` — and a grow writes them separately, so one of the two half-applied states is always possible when a page allocation or a page read fails part-way through. Only one of them is survivable, and the order is chosen on that: *separator written, link not* leaves a page allocated and unreachable to a scan, which costs an allocation and nothing else, because a failed promotion fails the insert and no caller kept a row in it. *Link written, separator not* is unsurvivable — the leaf sits in the sibling chain routed by nothing, so the **old** leaf goes on taking the ids the new one holds, it is full, and the next such id appends another leaf *in front of* the unrouted one. The chain then descends, and §3.1b's page-wise ordering — the same ordering the tail-page-only duplicate check and invariant 11's below-the-mark refusal are justified by — is false for a btree relation. The ordering, and the fact that the final link write goes through the descent's own pin so it cannot fail, are in `src/storage/btree/btree.cpp`. **The heap chain has no such question**: nothing routes to a heap page, so the link is its only publication and it is written last (§3.1b).
- **Waystone** (`waystone-concpets.md`) is the engine's other access structure: `(pattern_id, arg_hash)` → the Keystones a previous execution of that pattern instance found, across relations. It is advisory and validated on use, and it may replace a *lookup* but never a *search*.

## 6. Page-Latch Consistency

There is no single canonical in-memory tuple and no hash table enforcing that an identical tuple exists at most once in program memory. Consistency is kept at the **page** level.

- A page frame is **pinned** for the duration of any access and **latched** — shared for reads, exclusive for structural mutation (slot directory changes, compaction, relayout). Tuple bytes are read and written directly within the pinned, latched frame; there is no tuple-identity cache to keep coherent with the page.
- Latching is **core-local**, consistent with thread-per-core/shared-nothing (`rules.md` §3): a page is owned by exactly one core, and its latch serializes cooperative tasks on that core across suspension points. It is not a cross-core lock — cross-core access goes through server-side forwarding (`protocol.md`), never shared-memory locking.
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
2. A heap page's `min_key` is immutable after creation.
3. No tuple with `id < min_key(page)` is ever placed in that page, including by relayout.
4. Tuples within a heap page are unordered.
5. The Keystone column is exactly `id:40 | flags:8 | reserved:16`.
6. The Keystone word is read and written atomically as a `uint64_t`; on-disk encoding uses explicit shift/mask, never compiler bitfields.
7. Ids stored outside the tuple header are zero-extended `uint64_t` with the upper 24 bits zero.
8. Waystone is advisory: deleting it wholesale may cost performance and must never change a query result.
9. Waystone is never **authoritative**. A reader may consult it for *where to look*, provided it treats a missing or stale entry as a miss, checks the Keystone id of the tuple actually found at the reported location, applies MVCC visibility exactly as the authoritative path would, and falls through to that path — a btree descent on a btree relation, a chain scan on a heap one — on any mismatch. It chooses where to look, never what is visible.
10. No single canonical in-memory tuple is enforced; consistency comes from page pin and latch discipline (§6).
11. Every relation's pk is a **unique 40-bit `id`, never rebound, never updatable, and never carried outside the Keystone word**. Where the id comes from (§4.1) is a per-**row** fact — the `INSERT` names it or omits it — and there is no key mode, no `CREATE TABLE` declaration, and no relation that refuses either arity. `sys.tables.next_id` is a **high-water mark on what has been placed**: `AllocateRowId` draws from it for an omitted key, `AdmitExplicitRowId` advances it past a named one, and it never moves backwards. Uniqueness follows from the mark with no page read for every omitted key and for every named key at or above it; a named key **below** the mark is admitted only on a btree relation, where the descent proves it instead, and is refused `OutOfRange` on a heap one because §3.1b's tail append, page-wise ordering and tail-page-only duplicate check are that ascent. A relation records whether it has ever taken one (`key_order`), which decides only whether a page's slot order is still its key order. What nothing relaxes: the pk is not updatable, and no id is ever issued twice.
12. The tuple MVCC header is exactly `trx_id:48 (zero-extended to 64) | undo_ptr | data_len | flags` = 20 bytes. There is no `xmax`; a version's validity interval is reconstructed from the undo chain, and DELETE is the slot's `DELETED` mark plus the deleter's `trx_id`.
13. **Every tuple is fixed-length.** A relation's row size is a schema constant; variable-width values occupy tagged cells of exactly `kds.inline_cell_width` bytes (§3.3), and that width is instance-pinned in the superblock. No code path produces a tuple whose size differs from its relation's constant.
14. **Var-heap values are immutable per version** and `kVarHeap` pages are never relocated; the class is logged, headered, and checksummed — authoritative data, not advisory (§3.4).

## 9. Open Decisions

The decisions this section once listed are not recorded here. Each section above states what holds today; a rule this file does not state is not made, and an implementer who needs one asks rather than assumes.
