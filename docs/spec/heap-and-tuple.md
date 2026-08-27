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

**Decided 2026-08-09 (`docs/spec/physical-optimizer.md` R4):** the epoch lives in the **common page header** — `PageHeaderFields::reserved0` (offset 16), the slot the header comment had already nominated — as `relayout_epoch`, u64, durable by construction. Every existing page carries 0 there, so the decision costs **no format bump**: a zero reads as epoch 0. Wraparound is unreachable at u64 width rather than handled. The pairing rule is part of the decision: no consumer may accept a location on epoch equality alone — the epoch is a fast whole-page invalidation layered over the Keystone-id check (K1), never a substitute for it. The field and the consumers' comparisons are in code (`docs/workplan-physical-optimizer.md` PX03/PX04, 2026-08-09) — recorded at access by the executor, compared at `exec/tuple_verify.hpp` for Waystone replay and Cabin hints alike, with a bump API called by nothing — and nothing bumps it until a mover exists.

### 3.1b Chain growth by tail append

A relation is a **chain of heap pages** linked through the `next_page_id` tail reservation (§3.2), rooted at `sys.tables.desc_page_id` (`include/kds/storage/heap/heap_chain.hpp`).

- **Growth is tail append, never a split.** Every insert goes to the last page. When it has no room, a new page is allocated, the tuple is written into it, and only then is the page linked on — the link is what makes a page reachable, so publishing it first would expose an empty tail.
- **A new page's `min_key` is the id of the tuple that caused the growth**, the smallest id it can ever hold, since ids only increase. No existing page's `min_key` is touched, so §3.1's immutability holds by construction.
- **Each page's ids lie entirely below the next page's `min_key`.** The chain is key-ordered page by page while tuples within a page stay unordered — "semi-sorted" holds across pages as well as within one. Two consequences are relied on in code: a duplicate incoming id can only be in the tail page, so the check is O(1) pages rather than O(chain); and an id below the tail's `min_key` has nowhere legal to go and is refused as a backwards sequence.
- **All three of the above rest on ids ascending, and that is the whole of what a heap relation refuses.** Amended 2026-08-25 with §4.1's removal of the key mode. A heap relation *may* be told its keys — what it may not be told is a key below its high-water mark, refused as `OutOfRange` in `Catalog::AdmitExplicitRowId` before the chain is touched. The mark is this section's three properties written as one number: at or above it, the incoming id is above every id the relation has ever placed, so the tail is the only legal page, the new page's `min_key` is still the smallest id it can hold, and a duplicate can still only be on the tail. Below it, all three fail at once — and the second failure is the dangerous one, because a page opening below an id already on its predecessor makes the tail-only duplicate check admit a duplicate silently. The old refusal was per *relation*, at `CREATE TABLE`; it is per *id* now, which is why nothing in this section is conditional either way.
- **No free-space reuse.** A page that fills and then has rows deleted is never revisited; the chain only grows at the tail. A delete-heavy relation grows monotonically.
- Walks are bounded by `kMaxChainPages` (2^20 pages, 8 GiB per relation). Exceeding it is `Corruption` rather than a loop, since a cycle in the links would otherwise hang a request.

`[OPEN]` — the **heap page split policy**: dividing a full page's contents and choosing the new boundary. Tail append deliberately does not decide it, because it never moves a tuple off a page or assigns a `min_key` to a page that already holds tuples. Page compaction and free-space reuse are open with it, and both need the transaction manager to answer "does any snapshot still need these bytes" — which it cannot today, because readers are deliberately not registered (`txn.md` §4.1). Reader registration is the prerequisite.

**Still open after 2026-08-11, and not settled by the btree leaf division.** §4.1 divides a full *btree leaf*, which shows a division can be done inside invariants 2 and 3 — the old page keeps its `min_key`, the new page takes the split key. It does not carry over. A leaf is reachable through a descent, so a divided leaf is re-routed to by the separator the division promotes; a heap page is reachable only through the chain that precedes it, and nothing routes. A heap relation also has no shape that produces the need: its ids ascend, so no id ever sorts inside a full page. The division is a precedent for the *invariant argument*, never for the policy.

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

**Every relation's pk is a unique 40-bit id that is never rebound and never updated.** *Where* the id comes from is a per-**row** choice since 2026-08-25 — the `INSERT` names it or omits it (§4.1) — and *whether it ascends* is a consequence of the storage type. What is not a choice is uniqueness: an id names exactly one tuple for the lifetime of the relation, and every consumer rests on that. The provenance of the value does not matter to any of them.

Uniqueness is obtained by two different means, and which one runs is decided by the storage and the id, never by a declaration:

- **The mark.** `sys.tables.next_id` never moves backwards, so an id at or above it is above every id the relation has ever placed. This covers every omitted-pk insert on any relation, and every supplied key on a **heap** relation — where it is the *only* available proof, because a chain has no descent. **Proved without reading a page.**
- **The descent.** On a **btree** relation a supplied id may sort anywhere, and there the mark proves nothing: a relation whose mark is 1000 may have had 500 since its first insert. Uniqueness is proved instead by the descent, which lands on the one leaf that may hold the key and finds the duplicate or does not.

What holds regardless:

- The cursor is **persistent, not derived**: `sys.tables.next_id`, a **high-water mark on what has been placed**. `Catalog::AllocateRowId()` returns it and advances; `Catalog::AdmitExplicitRowId()` advances it past a supplied id with `max()`. Deriving it as `max(id) + 1` would reissue an id after the highest tuple is deleted, handing a new tuple the identity of a retired one. It is what `SHOW BUDGET` and `DESCRIBE` derive K4's lifetime budget from, so it must never fall behind what was placed. The first id issuable is 1 (`kFirstRowId`); 0 stays reserved for "unset".
- **The two id sources share the one mark, and that is what keeps them apart.** An issued id always clears every key the caller has named, and a named key at or above the mark clears every id the engine has issued. Only a *below-the-mark* named key can meet an issued one, only a btree relation admits one, and there the descent is the answer.
- Ids are unique by construction, **not gapless** — an insert that fails after allocating burns one, and a caller may skip a range outright. Nothing depends on gaplessness.
- Ids are **monotonic until a below-the-mark key lands**, which only a btree relation permits and which the relation records (`sys.tables.key_order`, §4.1). Three things rested on issuance order rather than on value uniqueness and had to be paid for: the btree leaf that used to refuse a division, the full-internal-node promotion that assumed a rightmost split, and the leaf slot search that assumed slots were in key order (all §4.1). Page-wise `min_key` ordering did **not** — a division preserves it — so range pruning (`kRange`'s tail prune, `src/exec/step_vm.cpp`) is untouched.
- The pk is carried **only** by the Keystone word, never also as a body column: `EncodeRow()` writes `[Keystone word][columns 1..n-1]`. Storing a key twice is how the two copies come to disagree. A supplied id is *named* in the statement's first position and still lands only in the Keystone word.
- The pk **cannot be updated**. It is the tuple's identity, not a field of it.
- A relation's first column must be declared with an **integer type** (`catalog::CheckKeystoneColumn`), checked at `CREATE TABLE`. Its declared width is display metadata: the id lives in the 40-bit Keystone field regardless, so a narrow declared type does not cap the sequence.

Implementation rules:

- Encode and decode with **explicit shift/mask helpers only**. **Never use C/C++ bitfields** for an on-disk format — their layout is implementation-defined and KDS must be portable across architectures.
- The whole word is updated with **atomic `uint64_t` operations (CAS)**. Fields must never tear across writes.
- External structures — B+ tree keys, `min_key`, Waystone entries — store the id as a **zero-extended `uint64_t`** with the upper 24 bits zero. Ids are never 5-byte-packed.

`[OPEN]` — id-reuse and low-range reclamation policy. Sequence exhaustion is reported as `OutOfRange`, never wrapped.

### 4.1 Caller-supplied keys (amended 2026-08-11; **key mode removed 2026-08-25**)

Until the 2026-08-11 amendment §4 opened "**Every relation requires system-generated, autoincrement `id` values.** A caller-supplied pk on insert is a defect, not a feature." That sentence bound three rules together — *the engine issues the id*, *ids ascend*, and *the pk is never updated* — and charged all three at one price. They are separable, and only the third is an identity rule. That amendment separated them and paid for the second with a per-relation **key mode**, `ASSIGNED` or `EXPLICIT`, fixed at `CREATE TABLE`, `EXPLICIT` btree-only.

**The mode is gone.** The decision, in one sentence:

> **Every relation takes a caller-supplied primary key or issues one when the `INSERT` omits it, per row. On a btree relation the key may sort anywhere; on a heap relation it must not fall below the relation's high-water mark.**

Three things the mode was doing, and what each turned into:

- **It declared who names the key.** That is a per-row fact and always was — `INSERT` is the statement that names one — so it moved into the arity. A relation no longer refuses half the statements a caller might write against it.
- **It gated the heap.** `EXPLICIT` was refused on a heap-clustered relation because a chain has no descent to prove uniqueness with and no legal page for a key below its tail. Both are true of a key *below the mark* and neither is true of a key at or above it, so the gate moved from the relation to the id: `AdmitExplicitRowId` refuses a below-mark key on a heap relation as `OutOfRange`, and a heap relation is otherwise an ordinary one. §3.1b's three properties are the ascent, and the mark is the ascent written as one number.
- **It told the compiler whether a page's slot order was its key order.** That was the one thing the mode knew that nothing else did, and it was *over*-stated: a btree relation fed only ascending keys is as ordered as an assigned one ever was. It became an **observation** rather than a declaration — `catalog::KeyOrder {kAscending = 0, kUnordered = 1}` on `sys.tables.key_order`, flipped once, ever, the first time `AdmitExplicitRowId` admits a key below the mark.

**No format bump came with the removal.** `KeyOrder` occupies the byte `KeyMode` held, at the same offset and the same width, and the two on-disk values carry over as the facts they already implied: `kAssigned`'s 0 as "every id here ascended" (true of an assigned relation by construction) and `kExplicit`'s 1 as "an id may have landed out of order" (conservative for an explicit one, and wrong that way costs a sort rather than an answer). `kOnDiskSize` did not move, so a file written before 2026-08-25 mounts and every row in it means what it meant. Superblock stays at 15; `superblock.hpp`'s version list records the non-bump and why, because that list is where a missed one would be found.

*In code.* `catalog::KeyOrder` (`include/kds/catalog/well_known.hpp`), persisted at `SysTableRow::kKeyOrderOffset`, cached on `TableAccess` — **the one cached field there that is not a DDL fact**, and published on the flip by `CatalogCache::MarkKeysUnordered`, an *in-place* update rather than a `BumpVersion`. In place because the flip happens inside a running `INSERT` that is holding a `const TableAccess*`: the same one-field/one-owner license the index root and the desc page already carry, and for the same reason. The first form of this work did bump, and the dangling access re-read the pk out of a freed vector — a second insert on one relation answered "tuple's Keystone id N does not match the id being inserted". `Catalog::CreateTable` lost its `KeyMode` parameter and gained no replacement: `key_order` is set to `kAscending` by `InsertRelationRow` and never passed in, because a relation holding no ids has had none land out of order.

**Syntax.** A trailing bare identifier in the same optional slot as the storage clause:

```sql
CREATE TABLE t (id int64, qty int64) BTREE EXPLICIT;
```

**`EXPLICIT` survives as a word that does nothing, and `ASSIGNED` is refused.** The storage word is what the slot still decides; the key-mode words are what the removal left behind, and the two get different answers on purpose. `EXPLICIT` is accepted and sets no field: it states what is true of every relation — the caller may name this relation's keys — so written SQL keeps working and nothing false is accepted. `ASSIGNED` is `Unsupported` with its byte, because it means "the engine issues every id and supplying one is refused", and on the relation the statement would create, supplying one is admitted. Ignoring it would be accepting a spelling and enforcing something else (CLAUDE.md's truthfulness rule), which is worse than refusing it. `HEAP BTREE` is still the `InvalidArgument` repeat it was; `ASSIGNED EXPLICIT` no longer reaches that check, since the first word is refused before there is a second to repeat. All words stay case-insensitive **identifiers, never reserved keywords**, so no hash moved in either direction and `kFingerprintVersion` stayed at 1 through both the addition and the removal — a statement that no longer parses no longer hashes at all.

Storage no longer follows the key mode anywhere: `CREATE TABLE t (...)` and `CREATE TABLE t (...) EXPLICIT` are both heap-clustered. The resolution that pulled a bare `EXPLICIT` to btree existed only to keep the heap refusal reachable from a written word alone, and went with it.

**`default_key_mode` is gone with the mode it defaulted.** The config key stays *known* so its removal can be reported: an instance file still naming it is refused at startup with a message saying why, rather than falling to the generic unknown-key error. Nothing replaced it — there is no default left to set, and an instance whose keys come from outside now simply supplies them.

**A heap relation refuses a key below its high-water mark**, `OutOfRange`, naming the mark and pointing at `BTREE` as the storage that takes keys in any order. This is the one restriction, and it is the whole of what `EXPLICIT ⇒ BTREE` was protecting. A chain grows only at its tail (§3.1b), so a key below the tail page's `min_key` has no legal page — inventing one would either mutate a `min_key` (invariant 2) or place a tuple below one (invariant 3). And the chain's duplicate check reads the **tail page alone**, which is sound only while every earlier page's ids sit below the tail's bound; a page opening below an id already on its predecessor breaks that quietly, and a duplicate pk is then admitted with no error at all. Both properties are the ascent, so refusing below the mark is what keeps §3.1b true rather than a second rule that could drift from it. At or above the mark the incoming key is above every id the relation has ever placed, so the tail is the only legal page *and* the only page a duplicate could be on — the two questions the descent answers on a btree, answered here by one number and no page read.

**The admission gate: spellability, then the storage's ordering rule.** `Catalog::AdmitExplicitRowId(oid, id)` first refuses an id outside `[kFirstRowId, kMaxKeystoneId]` as `InvalidArgument` — 0 is reserved for "unset" and a value ≥ 2^40 cannot be stored in the Keystone field by any path — before the catalog page is touched at all. Then:

- **At or above the mark**, either storage: the mark moves to `id + 1`, persisted before the caller places anything.
- **Below the mark, heap-clustered**: the `OutOfRange` above. Nothing is written, so a refused key burns no mark.
- **Below the mark, btree-clustered**: admitted on the strength of the descent that follows. The mark does not move — it is a ceiling on what has been placed and this id is under it — and `key_order` flips to `kUnordered` if it was not already. Guarded on the current value, so a backfill of ten thousand old ids writes the catalog page **once**, not once per row.

There is no mode check left in this function, and none in `AllocateRowId` or `AllocateRowIdRange` either: all three run on every relation.

**Both writes outlive a rollback**, and deliberately. They are made outside the caller's transaction (`wal::kNoTxnId`), so a `ROLLBACK` after a named key leaves the mark advanced — burning an id, which K3 calls free, exactly as an issued id burns on an aborted insert — and leaves `key_order` flipped even though the key that flipped it is gone. The flag is safe in that direction and only that direction: a relation wrongly marked `kUnordered` pays a per-page sort it does not need, while one wrongly marked `kAscending` answers `ORDER BY <pk>` out of order. Un-flipping on rollback would have to prove no *other* below-mark key had landed meanwhile, which is a scan, to save a sort.

That last clause is the correction to make loudly, because the earlier draft of this section and `docs/rules/keystoneid-invariant.md` §2 both describe a gate that requires `id >= next_id` and rejects anything below it as `OutOfRange`. **That is not what shipped.** The mark is a **high-water mark**, not a gate: it exists to keep K4's lifetime budget and the 40-bit exhaustion check truthful about the id *space* a relation has consumed, and to keep the `ASSIGNED` cursor — should a mode ever be reachable both ways — from later issuing an id already placed. A descending id costs no catalog write whatever, which is what keeps a backfill of old ids from touching the catalog page once per row.

**On a btree relation uniqueness is proved by the descent, not by the cursor.** Once ids may descend, `next_id` says nothing about what is in use: a relation whose mark is 1000 may have nothing at 500 or may have had 500 since its first insert. So `BtreeInsert` is the authority. It descends to the one leaf whose key range covers the id, scans that leaf's live slots, and returns `AlreadyExists` naming the page and slot on a hit. The check is complete rather than a sanity check, because the descent is exact — no other leaf may hold the key. Two honest consequences:

- **A delete-marked version still holds its key.** The scan reads slots, and a `DELETED` slot is still a live slot until retirement; nothing retires today (`docs/inflight/known-gaps.md`, reclamation). So a pk that has been `DELETE`d cannot be re-supplied. That is issue-once (K1) holding for explicit relations by the same mechanism it holds for assigned ones, and it is a restriction a caller will meet.
- **`BtreeInsert` still refuses an id below its landing leaf's `min_key`** as `OutOfRange`. The descent makes that unreachable — a separator *is* a child's `min_key` — so it is a defensive check on the two ever disagreeing, not a policy about ordering.

**A full leaf now genuinely divides** (`SplitLeafAndInsert`, `src/storage/btree/btree.cpp`), where it used to refuse citing the open split policy. A monotonic sequence never reaches it: an id above everything in a full leaf is an *append*, which opens a fresh leaf and moves not one byte. Only an id that sorts *inside* a full leaf forces a division. Why that is legal inside the invariants, stated precisely because it is the one place tuples move without a mover:

- **Invariant 2 holds** — the old leaf keeps its `min_key` **unchanged**. A division moves the *upper* half out, so the low bound a lock-free reader may already have pruned by never moves.
- **Invariant 3 holds on both sides** — everything that stays was at or above the old bound already, and the new leaf's `min_key` is the split key, which is by construction the smallest id moved into it. Neither page ends up holding a tuple below its own `min_key`.
- **The boundary is chosen by key, not by slot.** A leaf fed descending ids is not in slot order (invariant 4 always permitted that; only issuance order used to make it true anyway), so splitting at slot *n*/2 would divide it at an arbitrary key. The live versions are copied out, sorted by key, and cut at the median.
- **The old page is rebuilt, not edited.** `RetireSlot` marks a slot dead without reclaiming its bytes — reclamation is a purge pass's job and no purge exists. Retiring the moved half would therefore leave the page exactly as full as it was, and the division would make room for nothing, which is the entire point of it. So the page is reformatted and the staying half written back.
- **The old page's `relayout_epoch` is set to `old + 1`.** Every tuple on it changed slot and half of them changed page, which is a relayout in everything but name, so §3.1a's pairing rule applies: every Waystone trail entry and Cabin hint pointing into that page becomes untrusted at once. It is set to one past the old value rather than bumped from the zero the reformat left, because an epoch that went backwards would let an entry recorded at the old value compare equal again — the one thing the field exists to stop.
- **Delete marks travel with the version they belong to.** A moved version carries its deleter's `trx_id` and arrives still marked; re-inserting the payload alone would resurrect a row some snapshot has already been told is gone.
- **Secondary indexes need nothing.** An index entry's sort key is `key || pk` (`index_page.hpp`) and its payload is the pk — never a location — so a division is invisible to them. The undo chain is likewise addressed by `undo_ptr`, not by where a version sits.
- A leaf holding **fewer than two live tuples** cannot be divided: no boundary makes room, because the row is near page-sized. Reported as the `OutOfSpace` it is, naming the reason, rather than producing an empty leaf a descent would route to and never satisfy.
- **A leaf's slots are no longer in key order**, and the lookup path had to stop assuming they were. `FindSlotForId` keeps its binary search as an optimization for the ordered case and falls through to a linear pass, which is what makes the answer correct in every case: a supplied id appended into a leaf can sort below its neighbours, and a division re-lays a page by key rather than by slot position. An unsorted leaf costs a wasted log2(n) probes and still returns the right answer. Invariant 4 always permitted this — only issuance order used to make slot order true anyway.

**Promotion into a full internal node divides it** (`PromoteSeparator`, workplan PK09). Two shapes, told apart rather than assumed. A separator sorting above every entry the node holds takes a right-split with no movement — a new node whose only child is the new subtree — which is the append case a monotonic sequence produces exclusively, and it is correct and free there. A separator sorting *inside* the entries, which only a caller-supplied id can produce, divides them: the **median separator moves up** rather than being copied, its child becomes the new node's leftmost child, and the lower half is written back with the original leftmost child untouched. Copying the median instead — the leaf's rule — would route every key at exactly that value into a subtree that no longer holds it. Telling the two apart is not optional: promoting an interior separator by the cheap path would strand every subtree sorting above it, which is silent data loss rather than a wrong answer anyone would notice.

**`INSERT` arity is per-row and two-valued.** `ncols` values means the caller names the key and `values[0]` is it; `ncols - 1` means the caller omits it and the engine issues one. Both are legal on every relation, row by row, and a wrong length is refused naming **both** accepted counts — with two of them, a message naming one reads as an off-by-one against whichever the writer did not mean.

The two counts cannot be confused, which is what makes accepting both honest rather than ambiguous. The 2026-08-11 rule argued the opposite — that a relation taking both counts makes `VALUES (1, 2)` on a three-column table ambiguous between "explicit pk plus one column" and "assigned pk plus two columns" — and **that argument was wrong.** Those two readings have different lengths: pk-plus-two is three values, pk-omitted-plus-two is two. `INSERT` is positional with no column list (`parser/ast.hpp`'s `InsertStmt`) and no body column may be omitted individually, so a row's length names one reading and not the other. There is no relation on which the two coincide.

The supplied pk must be an **integer literal** — the gate runs before anything is placed and must not depend on evaluation — and a non-integer or negative value is refused carrying the offending token's byte. When the row names its key, the pk is split off `values[0]` once, so everything downstream (the FK forward check, assertion admission, `EncodeRow`, the Cabin witness, index maintenance) keeps receiving the shape it already expected: the columns *after* the key. `AdmitExplicitRowId` sits at exactly the position `AllocateRowId` occupies on the other arity — after `enforcer_.AdmitInsert`, before `EncodeRow` — so a refused row still burns nothing (BI9).

**Bulk `INSERT`** runs every row through that same single-row pipeline in statement order, so a bulk statement may name keys in any order, mix named and omitted rows, and each row is admitted, placed and indexed exactly as if it had arrived alone (BI2 and BI4 unchanged). The **sorted-fill fast path is engaged only when every row omits its key** (`SortedFillEligible` plus a per-statement check at the call site): the fill carves one contiguous id range up front and appends in order, which leaves no place for a key the caller chose. Ineligibility, never a refusal — a statement that names keys still runs, through the per-row path. The check is per statement rather than per relation because naming a key is a property of the row now; what stays on `SortedFillEligible` is the relation-shaped half.

**Row-id leases work on every relation.** `AllocateRowIdRange` refuses nothing for a key reason any more, which is what lets a peer core take the omitted-pk arity on any relation it owns. The one honest consequence: a carve spends its block from the mark's point of view before those ids are placed, so a *named* key landing inside a live carve meets the leased id when the peer places it. On a heap relation that cannot happen — a named key must be at or above the mark, which the carve has already moved past its own block. On a btree relation the descent reports it as the duplicate it is, `AlreadyExists`, to whichever of the two lands second.

**A peer core refuses a named key, per row.** Admitting one writes the relation's `sys.tables` row — the mark, or the `key_order` flip — and that page is the system core's. The refusal used to be PW1c-5's shape-gate arm, which refused the whole *relation* for having the wrong mode declared; it is in `InsertOneRow` now, beside the admission it is about. Strictly more is admitted: a peer may write any relation it owns, on the omitted arity, drawing from its own id lease and writing no catalog page at all.

**The pk is still not updatable** (K2). `exec::CompileAssignments` refuses a pk `UPDATE` at compile time as `Unsupported` with the column's byte (K-M3), regardless of provenance. Naming a key at insert and changing one afterwards are unrelated permissions; only the first was granted.

**What this did not change:**

- **The heap chain's storage code, entirely.** `ChainInsert` already refused an id below the tail page's `min_key` as `OutOfRange` and already checked duplicates on the tail page alone. Not one line of `heap_chain.cpp` moved: the mark check in `AdmitExplicitRowId` is what keeps those two facts reachable, and it sits above them. The **heap page split policy stays open and untouched** (§3.1b, §9) — nothing here divides a heap page or moves a tuple off one.
- **Waystone, Cabin, secondary indexes and foreign keys**, which key on the id's *value* and never on its provenance or its order.
- **The 40-bit budget (K4)** bounds the id *space*, not the insert count. A relation fed sparse named keys exhausts it after fewer rows; both places the budget is read — `DESCRIBE` and `SHOW BUDGET` — derive it from `next_id`, which the high-water advance keeps truthful.

**`DESCRIBE`** reports `key_order=ascending|unordered` after `clustered_type=` on the summary line — where `key_mode=` used to print a declaration, this prints an observation, and it answers the question someone reads that line for: whether a walk's pk order can be trusted. The pk column reports `autoincrement=if-omitted` on every relation, and every other column `no`. Neither `yes` nor `no` is true of a pk any more — the sequence runs when the `INSERT` omits the key and does not when it names one, and both are legal everywhere — so printing either would be printing something untrue for a field's convenience.

**`ORDER BY <pk>` emits each page in key order once a relation is `kUnordered`.** `exec::CompileStepChain` accepts the driving relation's pk as an `ORDER BY` target, and while the sequence is monotonic it discards the clause outright: a walk emits a page's slots consecutively in slot order (`RunWalkStep`, `src/exec/step_vm.cpp`), and an id at or above the mark is appended above every id already on the page, so slot order *is* key order. A key admitted below the mark can be appended below them, so from then on the two diverge — **within one page only**, since page-wise `min_key` ordering is preserved by a division. So the clause sets `Step::emit_in_key_order` and the walk emits that page's live slots sorted by Keystone id.

The flag is read off `key_order` rather than off the storage type, and that is the point of keeping the byte: a btree relation fed only ascending keys is exactly as free here as an assigned relation used to be. Reading the storage type instead would have charged a per-page sort to every btree relation in the engine for a divergence most of them never produce. The Waystone replay's ordering (`step_vm.cpp`) reads the same flag for the same reason.

**Recovery**: the `key_order` flip rides the **same logged catalog write** as the high-water advance — one `OverwriteLogged` of one `sys.tables` row, inside RV3's coverage since 2026-08-19 — so it redoes with it and a crash cannot come back reading `kAscending` on a relation that took a below-mark key. That mattered enough to state, because the flip's failure mode is not the mark's: a lost mark burns or reissues ids (K1's class), while a lost flip would discard an `ORDER BY <pk>` that the relation now needs and answer it out of order. Making it a second, unlogged field would have introduced a wrong-answer gap where the mark only had an id gap, which is why it went into the row the mark already writes rather than beside it.

**Implementation** — the key mode built 2026-08-11 (PK01-PK09, `docs/workplan-key-mode.md`, superseded) and removed 2026-08-25, with `tests/supplied_key_test.cpp` as the end-to-end cover, the admission cases in `tests/catalog_test.cpp` and the leaf-division cases in `tests/btree_test.cpp`.

## 5. Indexing

- A relation is stored either as a **heap chain** (§3.1b) or as a **clustered B+ tree** on the Keystone pk, chosen at `CREATE TABLE` and by nothing else — the key mode used to force `BTREE` and no longer exists (§4.1). On a btree relation the tree *is* the storage, and a descent is authoritative: a miss means the row does not exist, and no scan follows. That authority is what admits a caller-named key **below** the relation's high-water mark, which is the one thing a heap relation refuses. A heap relation has no pk index at all, so a point lookup scans the chain.
- A btree **leaf is a heap page** — same slot directory, same tuple format, same MVCC header, same `min_key` and `next_page_id`. A clustered-btree relation is therefore not a second storage engine; it is the heap with a directory over it.
- **A leaf grows two ways.** An id above everything the full leaf holds opens a fresh right leaf and moves nothing — the append shape a monotonic sequence produces. An id that sorts *inside* a full leaf makes the leaf **divide**: the live versions are cut at their median key, the upper half moves to a new leaf whose `min_key` is the split key, and the old leaf keeps its own. Built 2026-08-11 with the `EXPLICIT` key mode; since that mode's removal (2026-08-25) the second shape is produced by a caller-named key below the relation's high-water mark, which only a btree relation admits; §4.1 carries the invariant argument, the epoch consequence, and the unimplemented internal-node case.
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

**Collection landed 2026-08-03; the shadow planner that consumes it landed 2026-08-09; the optimization itself — a mover — has not.** `sys.access_stats` records one row per access *shape* — `(kind, rel_id, column_mask)` — with how often it ran and when it last ran, written for every access kind through one call with no per-kind branch (`include/kds/stats/access_stats.hpp`). `SHOW ACCESS` reads it, and `SHOW RELAYOUT` weighs it with the R1 decay score into candidate relayout plans (`docs/spec/physical-optimizer.md` §5).

The shape is keyed by **columns, never values**: `WHERE flag = 1` and `WHERE flag = 2` are one row. That is what bounds the relation by the schema rather than by the data, so it needs no eviction policy and no directory — the unbounded axis, *which arguments repeat*, is Waystone's and stays there (`waystone-concpets.md` §5). The two layers answer different questions and are deliberately not merged.

What makes the data worth having is the kind split that arrived with it: a walk driven by an equality on a non-pk unindexed column is now `kFilterScan` rather than an undifferentiated `kScan`. The two cost the same and mean entirely different things — one is a statement that asked for everything, the other is a statement that asked for a few rows and had to read all of them to find out which, which is exactly the case an index or a clustering decision would fix. Measured cost of collecting: +1-2% on a point lookup, unmeasurable on anything slower.

Since 2026-08-08 the same split records what happened when that case *was* fixed: `kIndexProbe` and `kIndexRange` (`docs/spec/index.md` §8) are counted through the same call with no per-kind branch, so a relation's history now distinguishes "searched every row for a few" from "descended an index for them". A `kFilterScan` sitting beside a `kIndexProbe` on the same relation names two columns with different treatment, which is the first shape a physical optimizer could act on that this file's §7 does not already describe. Neither is trail-replayable — invariant 9's line is lookup versus search, and both are searches.

Relayout must respect the `min_key` insertion rule (§3.1), bump the page epoch (§3.1a) so every recorded location on that page becomes untrusted at once, and — **on a btree-clustered relation only** — keep the tree consistent, which is a tree restructure and out of the first mover's scope (`docs/spec/physical-optimizer.md` R8). A heap relation has no pk index, its Cabin is relocation-invariant by value = pk indirection, and secondary indexes exist only on btree relations — so a heap-relation mover maintains *nothing but the epoch*, which is why the first mover targets heap relations. Under the fixed-length rule (§3.3) a relayout is a copy of fixed cells — exact fill-factor math, no per-tuple size negotiation — and `kVarHeap` pages are outside its jurisdiction entirely (§3.4).

Key-boundary re-partitioning mainly benefits range locality; for single-pk point lookups the acceleration comes from Waystone instead. The two coexist and address different shapes.

*No mover is implemented, and consequently nothing bumps a page epoch.* **The shadow half is built (2026-08-09)**: `docs/spec/physical-optimizer.md` (R1-R12) — the decay score, the epoch field with real comparisons at both validation sites, the planner, and `SHOW RELAYOUT`, which reports every candidate plan with its predicted benefit and the §6 gate blocking it. v1 is deliberately shadow-only: every enactment is gated, and the report exists to price opening the gates.

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
11. Every relation's pk is a **unique 40-bit `id`, never rebound, never updatable, and never carried outside the Keystone word**. **Amended 2026-08-11 and again 2026-08-25 (§4.1):** where the id comes from is a per-**row** fact — the `INSERT` names it or omits it — and there is no key mode, no `CREATE TABLE` declaration, and no relation that refuses either arity. `sys.tables.next_id` is a **high-water mark on what has been placed**: `AllocateRowId` draws from it for an omitted key, `AdmitExplicitRowId` advances it past a named one, and it never moves backwards. Uniqueness follows from the mark with no page read for every omitted key and for every named key at or above it; a named key **below** the mark is admitted only on a btree relation, where the descent proves it instead, and is refused `OutOfRange` on a heap one because §3.1b's tail append, page-wise ordering and tail-page-only duplicate check are that ascent. A relation records whether it has ever taken one (`key_order`), which decides only whether a page's slot order is still its key order. What nothing relaxes: the pk is not updatable, and no id is ever issued twice.
12. The tuple MVCC header is exactly `trx_id:48 (zero-extended to 64) | undo_ptr | data_len | flags` = 20 bytes. There is no `xmax`; a version's validity interval is reconstructed from the undo chain, and DELETE is the slot's `DELETED` mark plus the deleter's `trx_id`.
13. **Every tuple is fixed-length.** A relation's row size is a schema constant; variable-width values occupy tagged cells of exactly `kds.inline_cell_width` bytes (§3.3), and that width is instance-pinned in the superblock. No code path produces a tuple whose size differs from its relation's constant.
14. **Var-heap values are immutable per version** and `kVarHeap` pages are never relocated; the class is logged, headered, and checksummed — authoritative data, not advisory (§3.4).

## 9. Open Decisions

Collected from the sections above, plus those owned by companion specs.

- ~~Per-page epoch storage location, width, and wraparound (§3.1a)~~ — **decided 2026-08-09 (`docs/spec/physical-optimizer.md` R4)**: common header `reserved0` → `relayout_epoch`, u64, no format bump. What remains true: a tuple's address is stable for life, so until a mover bumps a page every comparison is between two zeros; the field landed at workplan PX03 and the real comparisons at PX04 (both 2026-08-09), ahead of any mover — the hand-bumped-epoch contract tests in both suites are what prove they would fire.
- `kMaxAccessShapes` (`[PROPOSED]` 4096) — the cap on distinct rows in `sys.access_stats` (§7). The population is (kind × relation × column combination), which in a real schema is dozens; the cap exists because "in a real schema" is an assumption and an unbounded catalog relation written from the statement path is where that assumption would fail quietly.
- Whether access statistics ever *drive* anything (§7). Collection is built; no policy consumes it, and choosing one is a separate decision with its own blast radius — relayout has to respect `min_key` (invariant 3), keep a btree-clustered relation's tree consistent, and bump an epoch that is decided (§3.1a) but not yet in code. `docs/spec/physical-optimizer.md` §5-§6 is where the driving policy now lives, shadow-first.
- Heap page split policy — **still open and still untouched** by the 2026-08-11 key-mode work, which changed only btree leaves; free-space reuse and page compaction, both gated on reader registration (§3.1b).
- `kds.inline_cell_width` default value (§3.3) — settle against measured target-schema string-length distributions.
- Spilled-value size cap; prefix-inline revisit trigger (adopt only if string-equality steps become a measured cost) (§§3.3–3.4).
- Purge-cadence sizing metric for var-heap headroom (§3.4).
- Var-heap partition under range ownership, and per-range sub-structures
  generally (added 2026-08-24): one `kVarHeap` page may hold values
  referenced from both sides of a pk-range boundary, so
  `docs/spec/crosscore.md` §6a gates a spilling relation from splitting until
  this doc designs the partition; `crosscore.md` CC8 likewise names
  per-range heap chains / btree subtree entries as R3's largest piece,
  owned structurally here.
- Repurposing of the 16 reserved Keystone bits (§4).
- Id-reuse / low-range reclamation for high-churn relations (§4).
- ~~Whether a caller may ever supply the pk, and whether a supplied id may descend (invariant 11)~~ — **both decided and built 2026-08-11 (§4.1)**: yes to each, as a per-relation `EXPLICIT` key mode, restricted to btree-clustered relations. The second question was briefly recorded here as open and owned by the heap page split policy; that framing was wrong. It is owned by *where uniqueness is proved*, and on a clustered btree the descent already proves it — so the answer cost a leaf division and nothing the heap chain rests on.
- ~~Whether a heap relation may ever be `EXPLICIT`~~, and ~~whether `ALTER TABLE` may ever change a relation's key mode~~ — **both closed 2026-08-25 by the key mode's removal (§4.1)**, and the second is closed by having nothing left to change. The first was recorded here as the heap page split policy's question; **that framing was wrong too**, in the same way the ascent question's was. The split policy answers *where does a tuple go when it sorts inside a full page*, and a heap relation never has to answer it: a key at or above the mark belongs at the tail, and a key below it is refused. So the heap took caller-named keys for the cost of one comparison, and the split policy below is untouched — no heap page divides, and no tuple moves off one. What a heap still cannot do is take a key that goes backwards, which is a statement about the chain and is stated in §3.1b rather than left open here.
- ~~**Dividing a full btree internal node**~~ — **decided and built 2026-08-11** (`docs/workplan-key-mode.md` PK09). A separator promoted into a full node divides that node when it sorts inside its entries: the median separator **moves** up rather than being copied, its child becomes the new node's leftmost, and the lower half is written back — the leaf division one level up, and simpler, since an internal entry is a fixed pair with no payload to carry. The right-split-with-no-movement stays for the append case, where it is correct and free. Nothing about the feature is now refused for being unbuilt.
- Buffer-pool page-frame reclamation policy (§6).
- I/O backend abstraction: plain `O_DIRECT` versus `io_uring` versus pluggable.
- Waystone's own open items — retention and eviction, recording policy, page persistence class, `arg_hash` collision handling, and whether invariant 9 is ever amended to permit trusting a cached result set as complete (`waystone-concpets.md` §9).
- Undo retention and `SnapshotTooOld` surfacing; 48-bit `trx_id` wraparound; cross-core commit protocol (`txn.md`, `wal.md`).
