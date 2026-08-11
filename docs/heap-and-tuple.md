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
- **A new page's `min_key` is the id of the tuple that caused the growth**, the smallest id it can ever hold, since ids only increase. No existing page's `min_key` is touched, so §3.1's immutability holds by construction.
- **Each page's ids lie entirely below the next page's `min_key`.** The chain is key-ordered page by page while tuples within a page stay unordered — "semi-sorted" holds across pages as well as within one. Two consequences are relied on in code: a duplicate incoming id can only be in the tail page, so the check is O(1) pages rather than O(chain); and an id below the tail's `min_key` has nowhere legal to go and is refused as a backwards sequence.
- **All three of the above rest on ids ascending, and that is why a heap relation may only be `ASSIGNED`.** §4.1's `EXPLICIT` key mode — where the caller names the id and it may sort anywhere — is refused on a heap-clustered relation for exactly this reason, at `Catalog::CreateTable` and again at the statement layer. So every relation that can be a heap chain still issues its ids from the cursor, and nothing in this section is conditional.
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

**Every relation's pk is a unique 40-bit id that is never rebound and never updated.** *Where* the id comes from — and, since 2026-08-11, *whether it ascends* — is a per-relation choice, the **key mode** (§4.1). What is not a choice is uniqueness: an id names exactly one tuple for the lifetime of the relation, and every consumer rests on that. The provenance of the value does not matter to any of them.

The two modes obtain uniqueness by different means, and that is the whole of the difference:

- **`ASSIGNED`** (the default) draws the id from a persisted per-relation cursor, so ids ascend and uniqueness follows from the cursor never moving backward — **proved without reading a page**.
- **`EXPLICIT`** takes the id from the caller, in any order, so the cursor proves nothing. Uniqueness is proved instead by the clustered btree's **descent**, which lands on the one leaf that may hold the key. That is why an `EXPLICIT` relation must be btree-clustered (§4.1).

What holds regardless of mode:

- The cursor is **persistent, not derived**: `sys.tables.next_id`. On an `ASSIGNED` relation it is the smallest id never yet issued, returned and advanced by `Catalog::AllocateRowId()`; deriving it as `max(id) + 1` would reissue an id after the highest tuple is deleted, handing a new tuple the identity of a retired one. On an `EXPLICIT` relation it is a **high-water mark** — `Catalog::AdmitExplicitRowId()` moves it with `max()` and never reads it as a gate. In both modes it is what `SHOW BUDGET` and `DESCRIBE` derive K4's lifetime budget from, so it must never fall behind what was placed. The first id issuable is 1 (`kFirstRowId`); 0 stays reserved for "unset".
- Ids are unique by construction, **not gapless** — an insert that fails after allocating burns one, and an explicit relation may skip a range outright. Nothing depends on gaplessness.
- Ids are **monotonic on an `ASSIGNED` relation only.** Three things rested on issuance order rather than on value uniqueness and had to be paid for: the btree leaf that used to refuse a division, the full-internal-node promotion that assumed a rightmost split, and the leaf slot search that assumed slots were in key order (all §4.1). Page-wise `min_key` ordering did **not** — a division preserves it — so range pruning (`kRange`'s tail prune, `src/exec/step_vm.cpp`) is untouched.
- The pk is carried **only** by the Keystone word, never also as a body column: `EncodeRow()` writes `[Keystone word][columns 1..n-1]`. Storing a key twice is how the two copies come to disagree. This holds in both key modes — an explicit id is *supplied* in the statement's first position and still lands only in the Keystone word.
- The pk **cannot be updated**. It is the tuple's identity, not a field of it.
- A relation's first column must be declared with an **integer type** (`catalog::CheckKeystoneColumn`), checked at `CREATE TABLE`. Its declared width is display metadata: the id lives in the 40-bit Keystone field regardless, so a narrow declared type does not cap the sequence.

Implementation rules:

- Encode and decode with **explicit shift/mask helpers only**. **Never use C/C++ bitfields** for an on-disk format — their layout is implementation-defined and KDS must be portable across architectures.
- The whole word is updated with **atomic `uint64_t` operations (CAS)**. Fields must never tear across writes.
- External structures — B+ tree keys, `min_key`, Waystone entries — store the id as a **zero-extended `uint64_t`** with the upper 24 bits zero. Ids are never 5-byte-packed.

`[OPEN]` — id-reuse and low-range reclamation policy. Sequence exhaustion is reported as `OutOfRange`, never wrapped.

### 4.1 Key mode (amended 2026-08-11; built 2026-08-11)

Until this amendment §4 opened "**Every relation requires system-generated, autoincrement `id` values.** A caller-supplied pk on insert is a defect, not a feature." That sentence bound three rules together — *the engine issues the id*, *ids ascend*, and *the pk is never updated* — and charged all three at one price. They are separable, and only the third is an identity rule. The decision, in one sentence:

> **A relation's primary key cannot be updated, but the caller may supply it, and it need not be monotonic.**

An earlier draft of this section relaxed only the first of the three and kept the ascent as an invariant, on the argument that monotonicity *is* the uniqueness proof and dropping it would force a search. That argument was right about `ASSIGNED` relations and wrong about the engine: on a clustered btree the search already exists and is already paid for. A descent lands on the one leaf that may hold the key, so uniqueness is a page read the insert was making anyway. The ascent was never load-bearing for the btree — it was load-bearing for the *heap chain*, which is why the relaxation is scoped to btree relations rather than granted engine-wide.

A relation has one **key mode**, fixed at `CREATE TABLE` and never altered afterwards — `ALTER TABLE` accepts only `RENAME TO` and `RENAME COLUMN` (`docs/spec-alter.md`), so this needs no new refusal, only that none is ever added:

- **`ASSIGNED`** — the default, and what every relation built before this amendment is. The engine issues the id from the cursor; `INSERT` supplies values for columns 1..n-1 and supplying the pk is refused. Ids ascend.
- **`EXPLICIT`** — the caller supplies the id. `INSERT` supplies values for columns 0..n-1, the first being the pk, and omitting it is refused. Ids may arrive in any order.

*In code.* `catalog::KeyMode {kAssigned = 0, kExplicit = 1}` (`include/kds/catalog/well_known.hpp`), persisted on `SysTableRow::key_mode` at `kKeyModeOffset` after `owner_core`, cached on `TableAccess` as a DDL-only fact like `clustered_type`. The one byte it added grew `kOnDiskSize`, which is a **format-version event** — superblock **13 → 14** — because `SysTableRow::Decode` refuses any size but the exact one, so without the bump a pre-existing file would mount cleanly and fail on its first catalog read naming a size rather than a version. `Catalog::CreateTable` takes the mode as a required parameter and every caller passes it by name: a defaulted mode is how the wrong one reaches a relation without anyone reading the line.

**Syntax.** A trailing bare identifier in the same optional slot as the storage clause:

```sql
CREATE TABLE t (id int64, qty int64) BTREE EXPLICIT;
```

The two categories are order-free and each may appear at most once — a repeat is `InvalidArgument` with the byte, because `HEAP BTREE` names two different relations and `ASSIGNED EXPLICIT` two different insert arities, and picking one of them would be the parser deciding what the writer meant. All four words are matched case-insensitively **as identifiers and never reserved as keywords** (CLAUDE.md's "nothing new is reserved lightly"); anything else in the slot still falls to the top-level trailing-garbage check. Because these words were previously rejected there, this is new syntax rather than a re-spelling of accepted syntax: no statement that hashes now hashes differently, and `kFingerprintVersion` therefore did **not** move (`include/kds/parser/fingerprint.hpp`'s bump rule). A column-level marker (`id int64 EXPLICIT`) reads better and is rejected anyway: it needs a per-column option grammar that does not exist, in order to state a fact that is true of the relation rather than of the column.

**An `EXPLICIT` relation must be `BTREE`-clustered.** This is the load-bearing restriction and everything else follows from it. A heap chain grows only at its tail (§3.1b), so an id below the tail page's `min_key` has no legal page to go to — inventing one would either mutate a `min_key` (invariant 2) or place a tuple below one (invariant 3). And proving a supplied id unused would mean scanning every page in the chain, since §3.1b's "a duplicate can only be in the tail page" argument is exactly the argument the ascent bought. The btree descent answers both questions in one walk: it names the only page the id may legally occupy *and* the only page a duplicate could be on. Refused in `Catalog::CreateTable` as `Unsupported` — understood and declined — and again at the statement layer carrying the offending word's byte. Both, because the catalog is where a relation comes into being and a relation that can accept no `INSERT` should not be creatable through any path; and `HEAP` being the default is why a bare `EXPLICIT` lands in the same refusal.

**The admission gate checks spellability and nothing else.** `Catalog::AdmitExplicitRowId(oid, id)` refuses an id outside `[kFirstRowId, kMaxKeystoneId]` as `InvalidArgument` — 0 is reserved for "unset" and a value ≥ 2^40 cannot be stored in the Keystone field by any path — and refuses a `kAssigned` relation as `Unsupported`, the mirror of `AllocateRowId`'s refusal of a `kExplicit` one. There is **no ordering check.** What the function then does with `sys.tables.next_id` is advance it with `max()`: at or above the mark, the mark moves to `id + 1` and is persisted before the caller places anything; below the mark, it returns having written nothing at all.

That last clause is the correction to make loudly, because the earlier draft of this section and `docs/keystoneid-invariant.md` §2 both describe a gate that requires `id >= next_id` and rejects anything below it as `OutOfRange`. **That is not what shipped.** The mark is a **high-water mark**, not a gate: it exists to keep K4's lifetime budget and the 40-bit exhaustion check truthful about the id *space* a relation has consumed, and to keep the `ASSIGNED` cursor — should a mode ever be reachable both ways — from later issuing an id already placed. A descending id costs no catalog write whatever, which is what keeps a backfill of old ids from touching the catalog page once per row.

**Uniqueness is proved by the descent, not by the cursor.** Once ids may descend, `next_id` says nothing about what is in use: a relation whose mark is 1000 may have nothing at 500 or may have had 500 since its first insert. So `BtreeInsert` is the authority. It descends to the one leaf whose key range covers the id, scans that leaf's live slots, and returns `AlreadyExists` naming the page and slot on a hit. The check is complete rather than a sanity check, because the descent is exact — no other leaf may hold the key. Two honest consequences:

- **A delete-marked version still holds its key.** The scan reads slots, and a `DELETED` slot is still a live slot until retirement; nothing retires today (`docs/known-gaps.md`, reclamation). So a pk that has been `DELETE`d cannot be re-supplied. That is issue-once (K1) holding for explicit relations by the same mechanism it holds for assigned ones, and it is a restriction a caller will meet.
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

**`INSERT` arity is per-relation and single-valued.** `kAssigned` takes `ncols - 1` values, `kExplicit` takes `ncols` with the pk first, and each refuses the other's shape with a message that names the rule rather than the count. Mixing within a relation is refused outright: `INSERT` is positional with no column list (`parser/ast.hpp`'s `InsertStmt`), so a relation accepting both counts makes `VALUES (1, 2)` on a three-column table ambiguous between "explicit pk plus one column" and "assigned pk plus two columns", and leaves the arity refusal unable to say which was meant. One arity per relation is what keeps the message truthful. The supplied pk must be an **integer literal** — the gate runs before anything is placed and must not depend on evaluation — and a non-integer or negative value is refused carrying the offending token's byte. On the explicit path the pk is split off `values[0]` once, so everything downstream (the FK forward check, assertion admission, `EncodeRow`, the Cabin witness, index maintenance) keeps receiving the shape it already expected: the columns *after* the key. `AdmitExplicitRowId` sits at exactly the position `AllocateRowId` occupies on the assigned path — after `enforcer_.AdmitInsert`, before `EncodeRow` — so a refused row still burns nothing (BI9).

**Bulk `INSERT`** runs every row through that same single-row pipeline in statement order, so a bulk statement may name keys in any order and each row is admitted, placed and indexed exactly as if it had arrived alone (BI2 and BI4 unchanged). The **sorted-fill fast path is excluded** for explicit relations (`SortedFillEligible`): it carves one contiguous id range up front and appends in order, which is the wrong shape for ids the caller names. The exclusion is stated on the mode rather than inherited from `kHeap` — an explicit relation cannot reach that path through DDL anyway, since it must be btree — so the coupling cannot be broken silently from the other end.

**Row-id leases are not used for explicit relations.** `AllocateRowIdRange` refuses a `kExplicit` relation (`Unsupported`), and a lease is a carve, so no lease can ever be granted for one. The explicit insert path never calls `AllocateRowId` at all. Leases exist to let a peer core issue ids without writing the shared catalog page (P5); here the core issues nothing, and the only catalog write left is the high-water advance, which the relation's owning core makes.

**The pk is still not updatable, in either mode** (K2). `exec::CompileAssignments` refuses a pk `UPDATE` at compile time as `Unsupported` with the column's byte (K-M3), regardless of provenance. Naming a key at insert and changing one afterwards are unrelated permissions; only the first was granted.

**What this did not change:**

- **Heap relations, entirely.** Every heap-clustered relation is `ASSIGNED`, so §3.1b's tail append, its page-wise ordering and its tail-page-only duplicate check all keep the ascent they were built on. The **heap page split policy stays open and untouched** (§3.1b, §9).
- **`ChainInsert`**, down to its refusal messages: it never sees an explicit relation.
- **Waystone, Cabin, secondary indexes and foreign keys**, which key on the id's *value* and never on its provenance or its order.
- **The 40-bit budget (K4)** bounds the id *space*, not the insert count. An `EXPLICIT` relation fed sparse ids exhausts it after fewer rows; both places the budget is read — `DESCRIBE` and `SHOW BUDGET` — derive it from `next_id`, which the high-water advance keeps truthful.

**`DESCRIBE`** reports `key_mode=ASSIGNED|EXPLICIT` after `clustered_type=` on the summary line, and a pk column on an `EXPLICIT` relation reports `autoincrement=no` — the mode is read rather than `is_pk` alone, because saying `yes` would describe a sequence that never runs.

**Known gap — `ORDER BY <pk>` is a weaker statement on an `EXPLICIT` relation than the compiler assumes.** `exec::CompileStepChain` accepts the driving relation's pk as an `ORDER BY` target and then *discards* the clause, on the argument that "pk order is the order the chain already emits". A walk emits a page's slots consecutively in slot order (`RunWalkStep`, `src/exec/step_vm.cpp`), and until this amendment slot order was key order on every relation, because ids were appended in ascending order. On an `EXPLICIT` relation it is not: a supplied id lands at the end of the slot directory wherever it sorts. **Rows are still emitted in key order *across* pages** — page-wise `min_key` ordering is preserved by a division — so the disorder is bounded by one page, but it is real, and it is not what a client reading `ORDER BY id` expects. Nothing tests emission order on an explicit relation; the suite compares sets and probes by key. Recorded in `docs/known-gaps.md`; closing it needs either a per-page sort at emission or a refusal of `ORDER BY` on `EXPLICIT` relations, and picking one is not a decision this amendment makes. `LIMIT`/`OFFSET`'s own contract is unaffected — it is a prefix of the emitted order, whatever that order is.

**Recovery**: an `EXPLICIT` relation inherits the K1 gap and narrows what it can cost. Catalog writes are unlogged, so a crash can roll the high-water mark back below ids that were durably placed — but on an explicit relation the mark issues nothing, so a rolled-back mark cannot hand out an id already in use; the worst it produces is an understated budget and a `DESCRIBE` that reads low until the next high insert. On an `ASSIGNED` relation the same rollback reissues, which is the K1 break `docs/keystoneid-k0-findings.md` §4 describes and K-M2a closes.

**Implementation** — built 2026-08-11, PK01-PK07 in `docs/workplan-key-mode.md`, with `tests/key_mode_test.cpp` as the end-to-end cover and the leaf-division cases in `tests/btree_test.cpp`. The one piece deliberately left unbuilt is the internal-node division above.

## 5. Indexing

- A relation is stored either as a **heap chain** (§3.1b) or as a **clustered B+ tree** on the Keystone pk, chosen at `CREATE TABLE` — and forced to `BTREE` when the key mode is `EXPLICIT` (§4.1). On a btree relation the tree *is* the storage, and a descent is authoritative: a miss means the row does not exist, and no scan follows. That authority is what proves an `EXPLICIT` relation's keys unique, and it is why the mode is btree-only. A heap relation has no pk index at all, so a point lookup scans the chain.
- A btree **leaf is a heap page** — same slot directory, same tuple format, same MVCC header, same `min_key` and `next_page_id`. A clustered-btree relation is therefore not a second storage engine; it is the heap with a directory over it.
- **A leaf grows two ways.** An id above everything the full leaf holds opens a fresh right leaf and moves nothing — the append shape a monotonic sequence produces. An id that sorts *inside* a full leaf makes the leaf **divide**: the live versions are cut at their median key, the upper half moves to a new leaf whose `min_key` is the split key, and the old leaf keeps its own. Built 2026-08-11 with the `EXPLICIT` key mode, which is the only thing that can produce the second shape; §4.1 carries the invariant argument, the epoch consequence, and the unimplemented internal-node case.
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
11. Every relation's pk is a **unique 40-bit `id`, never rebound, never updatable, and never carried outside the Keystone word**. **Amended 2026-08-11 (§4.1):** where the id comes from is the relation's key mode, fixed at `CREATE TABLE`. Under `ASSIGNED` (the default) the engine issues it from `sys.tables.next_id`, the cursor only moves forward, and uniqueness follows from that with no page read. Under `EXPLICIT` the caller supplies it, **it need not ascend**, and uniqueness is proved by the clustered btree descent instead — which is why an `EXPLICIT` relation must be btree-clustered, and why heap chains (§3.1b) are `ASSIGNED`-only and keep the ascent unconditionally. `next_id` on an `EXPLICIT` relation is a high-water mark for the K4 budget, not a gate. What no mode relaxes: the pk is not updatable, and no id is ever issued twice.
12. The tuple MVCC header is exactly `trx_id:48 (zero-extended to 64) | undo_ptr | data_len | flags` = 20 bytes. There is no `xmax`; a version's validity interval is reconstructed from the undo chain, and DELETE is the slot's `DELETED` mark plus the deleter's `trx_id`.
13. **Every tuple is fixed-length.** A relation's row size is a schema constant; variable-width values occupy tagged cells of exactly `kds.inline_cell_width` bytes (§3.3), and that width is instance-pinned in the superblock. No code path produces a tuple whose size differs from its relation's constant.
14. **Var-heap values are immutable per version** and `kVarHeap` pages are never relocated; the class is logged, headered, and checksummed — authoritative data, not advisory (§3.4).

## 9. Open Decisions

Collected from the sections above, plus those owned by companion specs.

- ~~Per-page epoch storage location, width, and wraparound (§3.1a)~~ — **decided 2026-08-09 (`docs/feat-physical-optimizer.md` R4)**: common header `reserved0` → `relayout_epoch`, u64, no format bump. What remains true: a tuple's address is stable for life, so until a mover bumps a page every comparison is between two zeros; the field landed at workplan PX03 and the real comparisons at PX04 (both 2026-08-09), ahead of any mover — the hand-bumped-epoch contract tests in both suites are what prove they would fire.
- `kMaxAccessShapes` (`[PROPOSED]` 4096) — the cap on distinct rows in `sys.access_stats` (§7). The population is (kind × relation × column combination), which in a real schema is dozens; the cap exists because "in a real schema" is an assumption and an unbounded catalog relation written from the statement path is where that assumption would fail quietly.
- Whether access statistics ever *drive* anything (§7). Collection is built; no policy consumes it, and choosing one is a separate decision with its own blast radius — relayout has to respect `min_key` (invariant 3), keep a btree-clustered relation's tree consistent, and bump an epoch that is decided (§3.1a) but not yet in code. `docs/feat-physical-optimizer.md` §5-§6 is where the driving policy now lives, shadow-first.
- Heap page split policy — **still open and still untouched** by the 2026-08-11 key-mode work, which changed only btree leaves; free-space reuse and page compaction, both gated on reader registration (§3.1b).
- `kds.inline_cell_width` default value (§3.3) — settle against measured target-schema string-length distributions.
- Spilled-value size cap; prefix-inline revisit trigger (adopt only if string-equality steps become a measured cost) (§§3.3–3.4).
- Purge-cadence sizing metric for var-heap headroom (§3.4).
- Repurposing of the 16 reserved Keystone bits (§4).
- Id-reuse / low-range reclamation for high-churn relations (§4).
- ~~Whether a caller may ever supply the pk, and whether a supplied id may descend (invariant 11)~~ — **both decided and built 2026-08-11 (§4.1)**: yes to each, as a per-relation `EXPLICIT` key mode, restricted to btree-clustered relations. The second question was briefly recorded here as open and owned by the heap page split policy; that framing was wrong. It is owned by *where uniqueness is proved*, and on a clustered btree the descent already proves it — so the answer cost a leaf division and nothing the heap chain rests on. What stays open beside it: **whether `ALTER TABLE` may ever change a relation's key mode** (today it refuses, with everything else data-shaped), and **whether a heap relation may ever be `EXPLICIT`**, which is the heap page split policy below and not a pk question.
- ~~**Dividing a full btree internal node**~~ — **decided and built 2026-08-11** (`docs/workplan-key-mode.md` PK09). A separator promoted into a full node divides that node when it sorts inside its entries: the median separator **moves** up rather than being copied, its child becomes the new node's leftmost, and the lower half is written back — the leaf division one level up, and simpler, since an internal entry is a fixed pair with no payload to carry. The right-split-with-no-movement stays for the append case, where it is correct and free. Nothing about the feature is now refused for being unbuilt.
- Buffer-pool page-frame reclamation policy (§6).
- I/O backend abstraction: plain `O_DIRECT` versus `io_uring` versus pluggable.
- Waystone's own open items — retention and eviction, recording policy, page persistence class, `arg_hash` collision handling, and whether invariant 9 is ever amended to permit trusting a cached result set as complete (`waystone-concpets.md` §9).
- Undo retention and `SnapshotTooOld` surfacing; 48-bit `trx_id` wraparound; cross-core commit protocol (`txn.md`, `wal.md`).
