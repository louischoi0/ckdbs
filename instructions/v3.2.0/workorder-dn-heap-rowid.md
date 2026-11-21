# Work order DN - the heap redefined again: an Oracle-style heap, rows in data pages addressed by a computed rowid, every key served through an index that names the rowid, a full scan that reads the data pages and no index

Status: **OPENED 2026-11-20 on the operator's standing go-ahead
(go-ahead-achieving-milestone), linked from DD** (`raft-marks-2026-11-20.md` §28), on
worktree `dn-heap-rowid` at `34958393`. **DN-S0 and DN-S1 done; stopped at DN-S1's gate,
the operator's mark on the amended invariants (§8)**. DN-Q0..Q14 adopted on that
go-ahead, none the operator's own mark, DN-Q1, DN-Q9 and DN-Q13 revised against the tree;
DN-Q12, DN-Q13 and DN-Q14 added at DN-S0 (§7). **DD-S4, the unasked
driver, ruled into DN-S2 by the operator on 2026-11-21** (`$!b`,
`raft-marks-2026-11-21.md` §6); taken up again on the standing go-ahead the
same day and stopped at the same gate. **DN-S1's gate passed on 2026-11-21**:
the operator marked §8 as staged, DN-Q13 (c) and DN-Q14 (a) with it
(`raft-marks-2026-11-21.md` §8), and DN-S2 onward runs on the standing
go-ahead of that day. **Written 2026-11-20.** Written in the main checkout on
`main` at `d091d096` (`v3.4.15-19-gd091d096`) on the operator's words of
2026-11-20 (`raft-marks-2026-11-20.md` §13). The letter DN is CLA's
proposal. **Exploratory by CLA's proposal** (DN-Q0): the operator's words
fix the shape; that DN-S0 measures its premise before anything is built is
CLA's. **Two more marks the same day** (`raft-marks-2026-11-20.md` §14):
the hidden Keystone's name is a reserved word (DN-D6, which settles half of
DN-Q1) and invariants 2 to 4 are the btree's alone (DN-D7) - **revised by
the operator's next word (§15 there): the `min_key` invariants, 2 and 3,
bind a heap data page too, and only 4's order is the btree's** (DN-D7 as
revised, DN-R10).

**DN rests on DD.** The operator's mark DN-D1 is that a slot is reused only
once every index entry naming it is removed, and DD
(`workorder-dd-index-entry-purge.md`) is the order that would remove one.
DD is written, not opened, exploratory, and may close with nothing built
(its own §0); DN gives it a consumer it did not have (§1.4). **Order:
DD :>> DN-S2.** DN-S0 and DN-S1 do not wait for it.

Every `[source-read]` claim in §1-§3 was read at `d091d096`; DN-S0's
(§7) at `34958393`. No suite was run: the order changes no build input.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` where a wrong choice turns a
correct answer into a wrong one with no error.

Related: `docs/spec/heap-and-tuple.md` §3.1b (the heap today), §4 (the
Keystone), §4.1c (`PURGE`), §5, §8 (the hard invariants);
`docs/rules/keystoneid-invariant.md` (K1-K5); `docs/spec/index.md` (IX1,
IX2, IX4b, IX6, §10.2); `docs/spec/foreign-keys.md` F1;
`docs/spec/cabin.md` C2, C6; `docs/spec/waystone-concpets.md` §2, §6;
`docs/spec/page.md` §5 (the free map, the free list);
`instructions/v3.2.0/workorder-bz-heap-hidden-key.md` (the definition DN
replaces); `workorder-dd-index-entry-purge.md` (the prerequisite);
`workorder-cv-ordered-index-walk.md` (an entry resolved per row);
`workorder-dk-descent-low-key.md`, `workorder-dm-leaf-upper-bound.md` (the
descents an index step pays); `workorder-di-shape-identity.md`.

## 0. What DN is

`[operator]` *"I am going to redefine the heap table concept once more. I
intend to name an Oracle-style table a heap table. That is, rows are stored
at random by key but in order by rowid. Range search by key is supported
through an intermediate index. A key access always goes through the index
and then a lookup happens, but a full table scan scans the data pages
directly with no index."* (given in Korean; recorded in English.)

CLA read that the heap today is already a relation whose pages are in
insertion order and whose full scan reads them (BZ), and that the
difference is one thing: **an index entry names a physical address (a
rowid) instead of a logical one (a tuple key)**, so a lookup is one page,
not a second btree descent. CLA laid out five questions. The operator's
answers, each recorded as a mark:

| | the operator's word (given in Korean; recorded in English) | the mark |
|---|---|---|
| DN-D1 | *"1. (b), assuming DD comes first."* | **a slot is reused only once every index entry naming it is removed**; DD is the prerequisite |
| DN-D2 | *"2. Where there is no PK, the engine issues a Keystone internally to tell rows apart (the rowid a value computed from the data page, so that it can be computed back in one step)."* | **identity and address are two things**: the Keystone tells rows apart; the rowid is computed from `(page, slot)` and back in one step |
| DN-D3 | *"2. The key's identity: a multi-column PK, a PK of other types supported, and where no PK is created, the engine-issued Keystone in its place (in that case not seen by `SELECT *` in SQL, but its value can be read by naming the column, e.g. `keyidentity`)."* | **a PK of any column count and type**; with none, the issued Keystone, hidden from `*` and **readable by an explicit name** |
| DN-D4 | *"3. No global order guaranteed."* | **rowid order is not insertion order across cores** |
| DN-D6 | *"The name for reading it (`keyidentity` or the like) as a reserved word."* | **the hidden Keystone's name is a reserved word**; its spelling is not fixed by the word ("or the like") |
| DN-D7 | *"Invariants 2 to 4 are btree-only."* then *"But even for a heap, the `min_key` invariant is kept (applied)."* | **as revised: invariants 2 and 3, the `min_key` pair, bind a heap data page as they bind every page; invariant 4's order is the btree leaf's alone, and a heap data page's slots are in no key order.** CLA reads "the `min_key` invariant" as both 2 (immutable) and 3 (no tuple below it), which are one statement about one field |
| DN-D5 | *"If the user sets no PK or makes it an int64, the engine-issued Keystone is used."* and, asked whether a single `int64` PK **is** the Keystone - a value named by the `INSERT` or issued when omitted - *"(a) is right, write it."* | **a single `int64` PK column is the Keystone** (named or issued, as a btree relation's tuple key is today); any other PK is a unique index beside a hidden issued Keystone |

`[design]` **Why the two words fit.** Keeping the Keystone apart from the
rowid is what keeps K1: a reused slot (DN-D1) gets a new row with a new
Keystone, so no Keystone is ever bound to two committed rows, though one
rowid is, at different times (DN-R1).

**What DN reverses.** BZ's definition, the operator's word of 2026-10-17:
"every optimization behaviour for heap tables is discarded" and the hidden
key "not nameable from SQL", `$!a` (`raft-marks-2026-10-17.md` §1). DN-D3
reverses the second; the heap's storage is replaced. A BZ heap on an
existing volume is DN-Q9's.

## 1. The tree as read `[source-read]`

### 1.1 The heap today

- "A heap relation is a btree relation whose tuple key is hidden"
  (`docs/spec/heap-and-tuple.md:43`), column 0 named `$tuple_key`
  (`include/kds/catalog/schema.hpp:63`), which "`$` and an identifier lex
  as a parameter, refused everywhere, so no statement spells it".
- "Below the catalog nothing is heap-specific" (`heap-and-tuple.md:45`):
  every subsystem sees a btree relation.
- A heap refuses `PURGE` and being a foreign-key parent
  (`heap-and-tuple.md:48`).

### 1.2 What an index entry names, and what a lookup costs

- A secondary index maps an index key "to a **tuple key**"
  (`docs/spec/index.md:22`); a probe resolves the tuple key through the
  clustered tree (`index.md:64-66`). So every index step pays its own
  descent and then the clustered descent.
- CV's ordered index walk resolves entry by entry, "a row its prefix admits
  and a residual rejects still costs its descent" (`known-gaps.md`, CV).
- **The tree's premise that no entry is removed**: "every maintenance
  action is an append" (`index.md` IX2, `:78`); an insert's coverage
  re-check and CV's reverse index walk rest on it
  (`include/kds/storage/index/index_tree.hpp`, `IndexFetchLeafAtOrBelow`'s
  comment). An entry naming a stale tuple key is harmless today because
  the resolve re-checks the key against the visible version (IX1's
  superset form, `index.md:40-44`).

### 1.3 Who finds a row by its Keystone

Not a complete census (DN-S0's):

- a foreign key: "FKs reference the parent's Keystone id"
  (`docs/spec/foreign-keys.md` F1);
- a Cabin entry: "Keystone ids (tuple keys), never tuple locations"
  (`cabin.md` C2), with a location hint verified against the slot (C6);
- a Waystone entry: a Keystone and a location, verified the same way
  (`waystone-concpets.md` §2);
- `PURGE`'s window, written in tuple key comparisons (§4.1c);
- the KWP load stream's field 0 (`bulkinsert.md` BI15);
- an `UPDATE`'s and `DELETE`'s locate by tuple key, and the arbiter of
  `ON CONFLICT`.

### 1.4 The invariants a heap page touches

`CLAUDE.md`'s hard invariants, numbered as `heap-and-tuple.md` §8:

- **2, 3** - `min_key` immutable, "no tuple with `id < min_key(page)`":
  stated for every page that holds tuples, and **kept for a heap data
  page (DN-D7 as revised)**. They are not free there. A heap page holds
  Keystones in no order, and two things can bring a Keystone below a
  page's `min_key`: a named `int64` key (DN-D5) lower than the page's, and
  nothing else - an issued Keystone is the relation's mark, above every
  key placed (invariant 11), so a reused slot (DN-D1) always takes a
  higher one. So 3 is a **rule of placement**: a row goes only to a page
  whose `min_key` is at or below its Keystone (DN-R10).
- **4** - "Tuples within a catalog chain page are unordered. A btree leaf
  is sorted by placement": a heap data page is a third kind, unordered
  (DN-D7). 4 gains a sentence; 2 and 3 keep their text.
- **What `min_key` is on a heap page** is not said by 2 or 3: only that
  it never changes and nothing sits below it. DN-Q11.
- **11** - "every relation's tuple key is a unique 48-bit id, carried only
  by the Keystone word, never rebound except by `PURGE`": holds by DN-D2;
  "a declared tuple key - the first column - must be integer-typed" becomes
  "a single `int64` PK", which need not be the first column.
- **13, 14** - fixed-length tuples, var-heap immutable: **unchanged, and
  what makes a rowid stable** (§2).

### 1.5 What DD gives DN

DD's premise is that "the one measurement the tree holds on a churned
index argues against part of it" (DD §0, §1.1). Under DN-D1 the reason to
remove an entry is no longer an index's size: **a slot cannot be reused
until it is**, so without DD a heap never reuses space. That is a consumer
DD's census did not have, and DD-S0 should be re-read with it. **The note
is in DD's own §0a** (2026-11-20): eight items, among them that DD-Q1 (a)
keeps a delete-marked row's head entry, DD-Q3 (a) pins one slot a leaf,
DD-Q4 (a) leaves reuse to an administrator's statement, and nothing in DD
says who knows that no entry of any index names a slot.

## 2. The design `[design]`

None is built or measured.

- **A heap relation** is a set of data pages, each holding fixed-length
  slots in no key order, plus **at least one index**: the PK's, or, where
  the Keystone is hidden, the Keystone's (DN-Q2). There is no clustered
  tree.
- **The rowid** is `(page_id, slot)` packed into a word and unpacked in
  one step - e.g. `page_id << 16 | slot`, 32 + 16 bits in 48 (DN-Q4). It
  names where a row is. It is stable for the row's life: a row never
  grows (invariant 13), an `UPDATE` is in place with its older versions
  in undo (invariant 12), and no split moves it, since no data page
  divides.
- **The Keystone** is the row's identity (DN-D2): issued, or the
  single `int64` PK's value (DN-D5). K1 holds as today.
- **An index entry** is `(index key, rowid)`, and DN-Q3 asks whether it
  carries the Keystone too, for a check at the slot.
- **A key access** is the index's descent, then the rowid's page. **A full
  scan** walks the relation's data pages in page order, with no index.
- **An insert** takes a slot on a page of its core's (DN-D4), then writes
  every index's entry. A unique PK is checked through its index (BX's
  latest-state check).
- **A delete** delete-marks the slot as today; the slot is reused only
  after DD has removed every entry naming it (DN-D1).
- **Range search by key** is the index's range walk, each entry resolved
  by its rowid; CV's ordered walk applies unchanged in shape.

What is expected to change in cost, as CLA reasons it, not measured:

- a secondary index access loses one descent (index descent + clustered
  descent becomes index descent + one page);
- a PK access is about even with a btree relation's (one descent each,
  plus one page for the heap);
- a full scan is about even with today's leaf walk;
- an insert pays a page of its core's in place of the btree's append, and
  every index entry is written as today.

## 3. Rules

Binding once DN opens.

- **DN-R1 - K1 holds** `[quiet-wrong]`. A Keystone is bound to at most one
  committed row in a relation's life, whatever a rowid is reused for.
- **DN-R2 - no index entry outlives its slot** `[quiet-wrong]`. A slot is
  reused only once no entry of any index of the relation names its rowid,
  as of every reader that could still resolve one (DN-D1, DD's rules).
- **DN-R3 - a rowid is resolved, never trusted**: what a resolve reads at
  a rowid is checked against what the entry was for - the index key
  against the visible version (IX1 kept), and the Keystone if DN-Q3 marks
  it - so a defect in DN-R2 is a refusal, not another row's answer.
- **DN-R4 - one answer.** A statement over a heap returns what the same
  statement returns over a btree relation with the same rows, but for
  emission order without `ORDER BY`, which is no contract.
- **DN-R5 - the hidden Keystone is named by one reserved word** (DN-D6),
  which no relation, column, index or alias may take on any relation, and
  `*` never includes it. **It is the first word reserved by the operator's word**
  (the lexer's keyword table already reserves `JOIN`, `ON`, `AS` and
  others, DN-S0): "nothing new is reserved lightly:
  keywords hash as identifiers" (`CLAUDE.md`, Working Rules) stands for
  every other word, and what the reservation does to a volume that
  already holds the name, and to a fingerprint (`fingerprint.hpp`'s bump
  rule), is stated before it is built (DN-Q1).
- **DN-R6 - a format event is said** (a superblock bump; DN-Q9 for a BZ
  heap on an older volume).
- **DN-R7 - a btree relation pays nothing.** No standard cell over a btree
  relation moves outside its floor.
- **DN-R8 - no constant without a measurement**: a rowid's bit split, a
  per-core page count, a fill threshold.
- **DN-R9 - the invariants are amended before the code**: 4 gains "a heap
  data page is unordered" (DN-D7 as revised) and 11 is restated for a
  heap, in `heap-and-tuple.md` §8 and `CLAUDE.md`, before DN-S2 writes a
  heap page; 2 and 3 are not reworded. **Not amended at the mark**: both
  files state what is built, and no heap data page exists; DN-S1 writes
  them.
- **DN-R10 - a heap page's `min_key` is immutable and nothing sits below
  it** (invariants 2 and 3, DN-D7 as revised). An insert places a row
  only on a page whose `min_key` is at or below the row's Keystone; a
  slot's reuse is under the same test; a page that cannot take a row is
  passed over, never rewritten. Checked where a row is placed and again
  by redo.

## 4. Items for the operator (DN-Q)

(a) is CLA's proposal in each. **Marked by the operator: DN-D1..D7 (§0);
§8 as staged, with DN-Q13 (c) and DN-Q14 (a), on 2026-11-21**
(`raft-marks-2026-11-21.md` §8).
Every item below is unmarked but the half of DN-Q1 that DN-D6 settles.

| | item | (a) | other |
|---|---|---|---|
| DN-Q0 | The order's standing. | **exploratory**: DN-S0 is census and premise, and may narrow or end DN | (b) a build order |
| DN-Q1 | The hidden Keystone's name. **That it is a reserved word is the operator's (DN-D6)**; open are its spelling and its edges. | **`keyidentity`**; refused by name wherever an identifier is declared, on every relation; on a relation whose Keystone is a declared `int64` PK it reads that column, so one spelling reads any relation's Keystone; a mount that finds the name already declared on a volume refuses by name; the fingerprint's bump rule read at DN-S0 and a bump said if it is owed | (b) another spelling the operator names; (c) on a relation with a declared Keystone the word is refused, not an alias; (d) an existing declaration of the name is kept and shadows the word |
| DN-Q2 | The index a hidden-Keystone heap always has. | **a Keystone index**, built with the relation and dropped with it, so every path of §1.3 finds a row by its Keystone | (b) none: a heap with no PK has no Keystone path, and §1.3's consumers refuse it |
| DN-Q3 `[quiet-wrong]` | Whether an index entry carries the Keystone beside the rowid. | **yes**, checked at the slot (DN-R3): 8 bytes an entry, and a reused slot that DD missed answers "no row" instead of another row | (b) the rowid alone, DD trusted |
| DN-Q4 | The rowid's encoding and whether SQL sees it. | **`page_id << 16 \| slot` in 48 bits; not visible to SQL** | (b) a `ROWID` pseudo-column, read-only; (c) another split, measured |
| DN-Q5 | A foreign key to a heap. | **a heap with a single `int64` PK is a parent as today (F1: the Keystone); a heap with another PK, or none, is not a parent** in DN | (b) a foreign key to any unique PK - a new FK form, its own order |
| DN-Q6 | `PURGE` on a heap. | **`PURGE` by Keystone where the Keystone is the PK; refused elsewhere, as today** | (b) by PK |
| DN-Q7 | Where an insert goes. | **a page of its core's whose `min_key` admits the row (DN-R10), a free slot on it first, a new page when none** | (b) the free map's first free slot, any core, under the same test |
| DN-Q11 | **What a heap data page's `min_key` is** (raised by DN-D7 as revised). An issued Keystone always passes DN-R10; a named `int64` key below every open page's `min_key` does not. | **the Keystone of the first row placed on the page.** A named key below every page with room opens a new page, whose `min_key` is that key. **The cost is stated**: keys named in descending order open a page a row, and DN-S0 measures a descending named load before DN-S2. What the field then gives a reader - a lower bound a scan by Keystone can skip a page on - has no consumer in DN and none is built | (b) a page opened by a named key takes the lowest valid key, 1, as its `min_key`: no page a row, and the invariant holds on that page with nothing to say; (c) named `int64` keys on a heap are placed through a second set of pages, apart from the issued ones |
| DN-Q8 | What `CREATE TABLE` without `HEAP` makes. | **a btree relation, as today** | (b) a heap |
| DN-Q9 | A BZ heap on an existing volume. | **the mount refuses the volume by name**, as every earlier format change did | (b) a mount converts it; (c) BZ's heap kept under another keyword |
| DN-Q10 | Cabin and Waystone over a heap. | **their entries keep the Keystone (C2) and their location hint becomes the rowid**, verified as today | (b) neither serves a heap in DN |
| DN-Q12 | **Where the amended invariants are written before the heap exists** (raised at DN-S0: DN-R9 writes them into `heap-and-tuple.md` §8 and `CLAUDE.md` before DN-S2, and both files state only what is built). | **staged in this order** (§8), word for word as they will read, marked there by the operator, and moved into the two files with DN-S2's code - so neither file states an invariant no code holds, and the text the code is written against is fixed first | (b) written into the two files at DN-S1, as DN-R9 reads, each marked "binds from DN-S2" |
| DN-Q13 `[quiet-wrong]` | **Where a declared single `int64` PK that is not the first column sits** (raised at DN-S0: column 0 is the tuple key at every site that checks one - `CheckKeystoneColumn`, `DecodeShapeHolds`, the not-null, Cabin and drop checks, and some nine readers that take `columns.front()`). | **the Keystone is column 0 of the stored row on every relation**: a PK declared elsewhere is placed at position 0, and the declared order is a presentation order `*`, `DESCRIBE` and a positional `INSERT` follow - one map, kept beside the schema - so no reader of column 0 changes | (b) the Keystone at its declared position, every column-0 site generalized; (c) a PK that is the Keystone must be declared first, as today |
| DN-Q14 `[quiet-wrong]` | **How a deleted row's Keystone stays bound once its slot is reused** (raised by DN-S0's review). On a btree relation a delete-marked slot keeps its key bound until `PURGE` (K1). On a heap, DN-D1 reuses the slot once DD has removed every entry naming it - the Keystone index's included - and a key named again then passes that index's uniqueness check: one Keystone bound to two committed rows, DN-R1 broken with no error. | **by how the heap gets its keys.** A heap whose Keystone is a declared `int64` PK - keys a user names - keeps a deleted row's **Keystone-index entry** until a row `PURGE` frees the key (DD's judge owes it, DD-Q1 (a) for that one index), so its slot waits for the `PURGE` (DN-R2). A heap with the hidden Keystone - keys the engine issues - refuses a named key at or below `sys.tables.next_id` (the KWP load is the one door that names one), so no deleted key can be named again and its slot is reused as soon as DD has cleared it. **The cost, stated at the mark**: the mark is above every key placed (invariant 11), so a KWP load into a hidden-Keystone heap names its keys in ascending order - a key below one already placed is refused | (b) every heap keeps the Keystone entry until `PURGE`, and a hidden-Keystone heap admits `PURGE` by `keyidentity` - one rule, slot reuse waiting on a statement everywhere; (c) a Keystone tombstone outside every index (a per-relation set of retired keys) - a new persisted structure |
| DN-Q15 | **A data page's type** (raised at DN-S2). | **a new `PageType::kHeapData` (15)**: the slotted body, logged and imaged as a btree leaf is; slot release and reuse are its alone | (b) `kHeap`, the catalog chain's: its pages only append, and a catalog page's checks would read a user row |
| DN-Q16 | **The head data page's `min_key`** (raised at DN-S2: the head is created with the relation, before any row). | **1, the lowest valid key**, so invariant 3 admits every row on it; every other page takes the Keystone of the row it is opened for (DN-Q11 (a)) | (b) no head page until the first insert, `desc_page_id` set then: a root that changes after CREATE, which `catalog_cache.hpp` forbids |
| DN-Q17 | **Where a grown page is linked, and how it is logged** (raised at DN-S2). | **after the page that filled** - one core's rows walked in the order it wrote them, no order promised (DN-D4) - **logged as a split is**: one BTREE_SPLIT imaging the new page and the page that links to it, so the page and the link replay whole or not at all | (b) after the head: every growth images the head; (c) a PAGE_INIT and a link record: a new record kind for what an existing one does |
| DN-Q18 | **The unasked driver's shape** (DD-S4 under the operator's `$!b`, raised at DN-S2). | **a job per relation in four phases** - collect retired slots (retiring a hidden-Keystone heap's resolved tombstones), purge every index (DD's attempt), verify no entry names a candidate, free - **stepped by core 0's `system` tick every 50 ms**, `analytic`'s slice a step, under the relation's `IX` as a read holder; the queue is noted by a heap `DELETE` and a taken-back insert | (b) every heap walked every period: an insert-only heap pays a walk a period for nothing; (c) a per-slot record of which indexes still name it: a new persisted structure |
| DN-Q19 | **How `keyidentity` is reserved** (DN-Q1 (a) revised, raised at DN-S2). | **refused by name at each declaring production** - a relation, a column (CREATE TABLE, ADD COLUMN, RENAME), an index, a namespace, an alias - **and resolved at every column reference** by the catalog's one resolver (`Schema::FindColumn`). Not a lexer keyword: a keyword is refused by `ParseIdent` at every reference too, the one place the word must be read | (b) a lexer keyword, every reference site taught to admit it |
| DN-Q20 `[quiet-wrong]` | **What "at or below its issue mark" reads** (DN-Q14 (a)'s text, raised at DN-S2). | **below `next_id`**: the mark is above every key placed (invariant 11), so a key below it is one that could have been placed and deleted; `next_id` itself was never placed. This is what the marked cost, an ascending load, requires | (b) at or below `next_id`, literally: an ascending load is then refused at its second key |
| DN-Q21 | **The `PRIMARY KEY` grammar** (DN-D3, DN-D5, raised at DN-S2: the tree had none). | **a column's `PRIMARY KEY` suffix, after the nullability, or one `PRIMARY KEY (<col>, ...)` in the column list**, both words unreserved; **a heap's alone**, refused `NotImplemented` on a btree, whose key is its first column with no keyword; a primary key column is never NULL | (b) accepted on a btree as a no-op on its first column: a spelling that enforces nothing new |
| DN-Q22 | **What the engine's indexes are named** (raised at DN-S2). | **`$keystone_<oid>` and `$pkey_<oid>`**: a parameter's spelling, which no statement writes, so neither is dropped, rebuilt or shadowed by name | (b) `<table>_pkey`: a name a user index can take first, refusing the CREATE TABLE |
| DN-Q23 | **Placement hints across a restart** (raised at DN-S2). | **memory only** (`HeapSpace`): a restart forgets each core's page and the free slots, a core with no page fills from the head, and a release finds the free slots again; up to `cores` partly filled pages wait for a release to be taken again | (b) persisted per relation: a catalog write per growth |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DN-S0 - census and premise** | No engine code. **Read**: every path that finds a row by its Keystone (§1.3 completed); every path that assumes a relation's rows are a clustered tree - a walk, a key range, an `UPDATE`'s and `DELETE`'s locate, `ON CONFLICT`, the KWP load, recovery's redo of a tuple, backup's page classes, the sim; every site that reads invariants 2-4 and 11; what DD must provide for DN-R2, against DD's §4. **Measure**, `build-release`, inside the stage budget: a secondary index point read and an index range of 20 and 1,000 rows over a btree relation, against the same with the clustered descent replaced by a page fetch in a spike never committed - the most DN's address can save. | the census, the results file, DN-Q0..Q10 restated, and DD's re-read premise back to the operator |
| **DN-S1 - the rules written** | `heap-and-tuple.md` §3.1b and §8, `CLAUDE.md`'s invariants, `index.md`, `foreign-keys.md`, `keystoneid-invariant.md` amended under the marks (DN-R9). Documents only. | the operator's mark on the amended invariants |
| **DN-S2 - the heap** | After DD :>> DN-S2 and the mark at DN-S1's gate. The data pages, the rowid, the indexes naming it, the insert, the delete, the scan, the slot reuse under DN-R2; **the driver that removes index entries unasked and the record that no entry of any index names a slot** (DD-S4, moved here by the operator's `$!b` of 2026-11-21); the superblock. | review, the full suite, `scripts/sim.sh` |
| **DN-S3 - the consumers** | §1.3's paths under DN-Q2, Q5, Q6, Q10. | review, the full suite |
| **DN-S4 - the close** | DN-S0's cells against the build; the btree standard cells (DN-R7); a heap against its btree twin on insert, point read, secondary-index read and scan. | closed |

## 6. What DN does not do

- No change to a btree relation, which stays the default (DN-Q8 (a)).
- No row migration or chaining: a row never moves (§2).
- No global insertion order (DN-D4).
- No foreign key to a non-`int64` PK (DN-Q5 (a)).
- No index-only scan.
- No claim against Oracle: no twin cell exists.

## 8. The amended text, staged (DN-S1, DN-Q12 (a))

`[design]` What `docs/spec/heap-and-tuple.md` §8, `CLAUDE.md`'s Hard
Invariants and the rules files will say once DN-S2's code holds it - word
for word, for the operator's mark before any of it is written into those
files (DN-R9). **A changed sentence only**; every invariant not named keeps
its text. Under DN-D7 as revised, 2 and 3 keep theirs.

**Invariant 4** (`heap-and-tuple.md` §8 and `CLAUDE.md`), in place of its
present text (in `heap-and-tuple.md` its clauses "whatever order rows arrive
in and at every core count", BD-R2 and "a retired slot carries no key and
sits anywhere" are kept):

> 4. Tuples within a catalog chain page are unordered by contract. **A heap
> data page is unordered**: a row takes a free slot of a page whose
> `min_key` admits its Keystone (invariant 3, DN-R10), and its slot says
> nothing of its key. A btree leaf is sorted by placement: every keyed slot
> ascends by Keystone id, live and delete-marked alike, because a row takes
> the slot its key sorts to (BD-R1). Readers rely on a leaf's order through
> the `ORDER BY <tuple key>` elision, the Cabin serve's tuple key sort and
> the index step's sort (§4.1), **and on a heap none of them reads slot
> order: the elision is off and both sorts take the Keystone from the row
> or the entry.**

**Invariant 9** (`heap-and-tuple.md` §8), its fall-through clause:

> ... and falls through to that path - a btree descent on a btree
> relation, the Keystone index's descent and the rowid's page on a heap
> (§3.1b) - on any mismatch.

**Invariant 11** (`heap-and-tuple.md` §8 and `CLAUDE.md`), its second
sentence, in place of "A declared tuple key is the relation's first column
and must be integer-typed; a heap's is the engine's hidden column 0, which
no SQL statement names (§3.1b)" (`heap-and-tuple.md`) and "A declared tuple
key - the first column - must be integer-typed; a heap's is an engine-added
hidden `int64` column no SQL names" (`CLAUDE.md`):

> A declared tuple key is an integer column, the relation's first; on a
> heap it is the single `int64` primary key, which is declared first
> (DN-Q13). A heap with no such key carries an
> engine-added hidden `int64` column 0, which `*` never includes and which
> one reserved word, `keyidentity`, reads by name on every relation (DN-D3,
> DN-D6). **A rowid is not a tuple key**: it names where a heap row is, `page
> << 16 | slot`, and a slot is reused for a new row with a new Keystone once
> no index entry names it (DN-D1, DN-R2) - so K1 is the Keystone's, and a
> rowid is bound to many rows over a relation's life, one at a time. **On a
> heap a named key's uniqueness is the Keystone index's, not a leaf's**: a
> key an entry of it names - live or delete-marked - is `AlreadyExists`. A
> declared-PK heap keeps a deleted row's Keystone entry until a `PURGE`
> frees the key; a hidden-Keystone heap refuses a named key at or below its
> issue mark, so a deleted key is never named again (DN-Q14).

**`heap-and-tuple.md` §3.1b**, its first sentence, in place of "A heap
relation is a btree relation whose tuple key is hidden":

> A heap relation is a set of data pages holding fixed-length rows in no key
> order, addressed by a rowid, and a Keystone index - its single `int64`
> primary key's where that is the Keystone, an engine-built one otherwise
> (DN-Q2), beside any other declared primary key's - through which every
> key access goes; a full scan reads the data pages and no index.

**`index.md` §1** (IX1), a sentence after "maps an index key to a tuple
key":

> On a heap an entry maps an index key to a **rowid and the Keystone**
> (DN-Q3): the rowid is fetched, and the Keystone at the slot is checked
> against the entry's before the key re-check, so an entry that outlived
> its slot (DN-R2's defect) is a miss, never another row.

**`foreign-keys.md` F1**, a sentence:

> A heap is a parent only where its Keystone is a declared single `int64`
> primary key; a heap with another key, or none, is refused as a parent by
> name (DN-Q5).

**`keystoneid-invariant.md` K3**, in place of "every user relation is a
btree, a heap under its hidden key included":

> Every user relation finds a row by its Keystone through one authoritative
> structure - a btree relation's tree, a heap's Keystone index (its single
> `int64` primary key's, where that is the Keystone) - so a miss there is
> the row's absence.

**`heap-and-tuple.md` §4.1c (`PURGE`)**, its heap refusal:

> A heap whose Keystone is a declared `int64` primary key is purged by
> Keystone as a btree relation is, its tombstone found through that key's
> index; any other heap refuses `PURGE` as today (DN-Q6).

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-20

In the main checkout on `main` at `d091d096`, documents only. The order
rests on a reading of the sites in §1; nothing was run. DD is a
prerequisite of DN-S2 and is not opened. Open with the operator: DN-Q0..Q10,
whether DN opens, and whether DD opens first with DN's consumer in its
premise.

### Two marks, 2026-11-20

In the main checkout on `main` at `4defff7a`, documents only
(`raft-marks-2026-11-20.md` §14). DN-D6: the hidden Keystone's name is a
reserved word; DN-R5 and DN-Q1 rewritten, the spelling and the edges left
open. DN-D7: invariants 2 to 4 are the btree's alone; §1.4 and DN-R9
rewritten, the two files that state the invariants not touched until
DN-S1. Nothing was run.

### DN-D7 revised, 2026-11-20

In the main checkout on `main` at `e3b770e6`, documents only
(`raft-marks-2026-11-20.md` §15). The `min_key` invariants bind a heap
data page; only 4's order is the btree's. §1.4 rewritten: on a heap,
invariant 3 is a rule of placement, and only a named `int64` key can meet
it. DN-R10 and DN-Q11 added, DN-Q7 and DN-R9 amended. The reading of a
catalog chain page that the first form of DN-D7 owed is dropped: 2 and 3
keep their scope. Nothing was run.

### DN-S0 - census and premise, 2026-11-20

On worktree `dn-heap-rowid` at `34958393` (`v3.4.18-9-g34958393`): DD's
closed work branch (`e0d43a79`) merged onto `origin/main`, opened on the
operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone),
linked from DD by the operator's word (`raft-marks-2026-11-20.md` §27). No
engine code; one spike, never committed.

**Read** (three censuses, `[source-read]` at `34958393`).

- **Who finds a row by its Keystone** (§1.3 completed). Everything leans
  on four things the clustered tree gives: (A) a miss is authoritative;
  (O) slot order is key order (BD-R1); (R) a key range is a leaf-chain
  walk from a seek; (H) a location hint is verified and falls back to a
  descent. **Needing the Keystone index authoritative for a hit and a
  miss** (DN-Q2): a point `UPDATE`/`DELETE` (`LocateByTupleKey`), `FOR
  UPDATE`'s claim and re-read, `RETURNING`, a foreign key's forward check,
  an insert's key uniqueness (today the tree's own placement search,
  `btree.cpp`'s `DuplicateKey`), `ON CONFLICT`'s arbiter, `PURGE`'s
  tombstone, `CleanDroppedCells`. **Key-order paths §1.3 did not list**:
  the `ORDER BY` elision on the tuple key (`step_compiler.cpp`'s
  `EmitsTupleKeyOrder` test and its reverse, `kTupleKeyBackward`; the
  query layer's `driving_keystone`) -
  `[quiet-wrong]` on a heap, whose scan is page order, so it is off there;
  `RunIndexStep`'s IX8a sort back to tuple key order, which needs the
  Keystone from the entry or the slot (DN-Q3); the Cabin serve's sort; the
  key set's ascending descent; the reverse walk; `PURGE`'s window; the
  online build's `HighWaterKey` (its A2 bound "every row above it" needs
  a fence that is not a key on a heap); recovery undo's rightward re-find;
  the simulator's order checks. **Simplified by a stable rowid**:
  rollback's `RowLocatorForRollback`, recovery undo (whose heap arm
  exists), redo, a walk's resume - each exists because a split moves rows.
  **A page scan serves**: assertions, Transitions, `GATHER STATISTICS`,
  `LeafCursor`'s users, backup.
- **What assumes a clustered tree.** `ClusteredType {kCatalogChain,
  kBtree}` is the discriminator (`well_known.hpp`); DN is a third value,
  branched on at `VisitRelation`, `CREATE INDEX`, foreign keys and the
  views. **The heap page format is reusable**: nothing in `heap::PageView`
  is key-ordered, `min_key` is a header field. **But there is no slot
  reuse primitive** (DN-D1 needs one): `InsertTuple` appends, `RetireSlot`
  keeps the bytes, `RedoWriteTuple` refuses a dead slot `Corruption` - a
  new write and a new redo case, DN-S2's. `kHeap` today is a catalog
  chain page, so DN-S2 decides between it and a new `PageType`. Tree-only
  and not needed on a heap: the leaf split and compaction, the epoch's
  two movers, `BTREE_INSERT` and `BTREE_SPLIT`. Needing a heap arm: the
  scan's seek and tail prune, the reverse walk, the DML walk's resume (by
  rowid), the dropped relation's reclaim walk (`page_reclaim.cpp`), the
  plan statistics' descent-sampled prior (a page-count prior instead), the
  online build's high water, the simulator's integrity sweep. No tree
  assumption: the checkpointer, backup, the free map, the page reader.
- **The invariants.** 2 and 3 are enforced at the btree door
  (`btree.cpp`, `OutOfRange` below `min_key`) and the chain's
  (`RefuseBelowTail`); a heap data page needs its own copy under DN-R10;
  redo already takes `min_key` from `PAGE_INIT` for every page type. 4 is
  enforced by `CollectKeyed`. **Invariant 9 also names the btree** - its
  fall-through is "a btree descent, every user relation being a btree" -
  **so DN-R9's list is short by one**: 9 is amended with 4 and 11. 11's
  "the first column, integer-typed" is hard-coded at column 0 across the
  tree (DN-Q13). K3 and the §1 premises of `keystoneid-invariant.md` say
  every user relation is a btree.
- **The reserved word. DN-R5's "the tree's first word reserved against
  identifiers" is false**: `lexer.cpp`'s keyword table already reserves
  `JOIN`, `ON`, `AS`, `IN`, `EXISTS`, `NOT`, `BETWEEN`, `LEFT`, `RIGHT`,
  `FULL`, `OUTER` (and `NULL`), each refused wherever an identifier is
  parsed. `keyidentity` would be the first reserved by the operator's word.
  **No fingerprint bump is owed**: a keyword and an identifier hash alike
  (`fingerprint.cpp`, `ShapeTag::kIdent` over the folded text), so a
  reservation moves no hash, and reading a pseudo-column by name is the
  catalog's resolution, not the token stream; no corpus line holds the
  word.
- **A BZ heap's volume.** `superblock.cpp` refuses any version but its own
  before the catalog is read, with "superblock version N is not this
  build's". A bump for DN refuses every older volume, a heap in it or not,
  and **cannot refuse "by name"** - it never reaches a relation's name. So
  DN-Q1 (a)'s "a mount that finds the name declared refuses by name" is
  moot under a bump, and DN-Q9 (a) is revised below.
- **What DN needs from DD** (DD's §0a, answered by DD's close). Built:
  entry removal under a row's unit, the rebuild's judge, DD-Q1 (b) - a
  deleted row's entries go once every reader sees the delete. **Not built,
  and DN-S2 needs them**: a driver that runs unasked (DD-S4, put to the
  operator) - without one a heap reuses a slot only after an
  administrator's `PURGE INDEX`; and a record that no entry of **any**
  index names a slot, which is DN's own (DN-R2). **A cost DD leaves**:
  DD-Q3 (a) never removes a leaf's first entry, so one slot a leaf of each
  index is never freed while the leaf stands; DM's U2b would lift it.

**Measured** (`bench/v3.4.18/dn-s0-premise-v3.4.18-9-g34958393.md`, by
`ck-tester`, A = `34958393` against B = a spike never committed - a
thread-local map from a tuple key to the (page, slot) of its first resolve,
so `RunIndexStep`'s resolve fetches that one page instead of descending;
`cores = 1`, `relaxed`, every page resident, a host busy with other
sessions' builds, waited out per block). **The descent costs about 0.3 to
0.6 us a resolved row**: an index range of 20 rows is 21-22 % cheaper
without it, one of 1,000 rows 31-40 %; **a one-row secondary point read
saves 3-7 %, at the noise floor** (28.8 against 27.7 us at 10,000 rows).
The spike prices only the saving: an entry's 8 more bytes (DN-Q3), the slot
check, DD's work and every other consumer of an address are not in it.
Cut: `cores > 1`, a cold device read, a PostgreSQL twin, the 1,000-row
cells' noise at 1,000 rows.

**DN-Q0's reading.** The premise holds for an index range and is weak for
a point read: the address saves a descent a resolved row, and a statement
that resolves one row barely sees it. DN stays exploratory, as DN-Q0 (a)
reads; nothing here ends it.

**Adopted on the operator's standing go-ahead of 2026-11-20
(go-ahead-achieving-milestone)**, none the operator's own mark, the marks
DN-D1..D7 untouched:

- **DN-Q3 `[quiet-wrong]` (a)**: an index entry carries the Keystone beside
  the rowid, checked at the slot. Its premise, read again: under DN-D1 a
  slot is reused, so a rowid alone names two rows over time; the census
  shows DD's judge (`OwedIndexEntries`) and IX8a's sort both need to know
  which row an entry is for, and the Keystone is the only identity that
  never moves (DN-R1). Holds.
- **DN-Q13 `[quiet-wrong]` (c), revised from (a) on DN-S0's review**: a
  heap's single `int64` PK, which is its Keystone (DN-D5), is declared
  first, and one declared elsewhere is refused at its byte. (a)'s premise -
  that no reader of column 0 changes - holds for storage only: a stored
  column map replaces BZ's "skip the hidden column" filter at every
  presentation site (`*`, `RETURNING *`, `DESCRIBE`, `sys.columns`, the
  wire's row description, a positional `INSERT`, the query layer), needs a
  catalog row change (`SysColumnRow` has only `pos`), and a missed site
  lines a positional `INSERT` up wrongly - as `[quiet-wrong]` as (b). (c)
  costs nothing, keeps every column-0 site, and keeps DN-D5's "a single
  `int64` PK is the Keystone" by refusing only a declaration order.
- **DN-Q14 `[quiet-wrong]` (a)**, written into §4 first: a declared-PK
  heap keeps a deleted row's Keystone-index entry until a row `PURGE`, and
  a hidden-Keystone heap refuses a named key at or below its issue mark.
  Its premise, read: the hidden Keystone is named only by the KWP load's
  field 0 (DN-S0's census A), and every issued key is below the mark
  (invariant 11), so no deleted key of such a heap can be named again.
- **DN-Q0 (a)**: exploratory, continuing.
- **DN-Q1 (a), revised against the tree**: `keyidentity`, reserved by a
  row in the lexer's keyword table, so it is refused at every declaration
  site by the parser's one door (`ParseIdent`), and a refusal names the
  word and its byte - **and admitted, by name, at every site that parses
  a column reference** (the select list, `WHERE`, `ORDER BY`,
  `RETURNING`, an `INSERT` column list), which is the cost the reserving
  takes on; on a relation whose Keystone is a declared `int64` PK it reads
  that column. **The mount check is dropped**: under DN's superblock bump
  no older volume reaches its catalog, so no declared `keyidentity` can be
  met. No fingerprint bump.
- **DN-Q2 (a)**, **DN-Q4 (a)**, **DN-Q5 (a)**, **DN-Q6 (a)**, **DN-Q7 (a)**,
  **DN-Q8 (a)**, **DN-Q10 (a)**, **DN-Q11 (a)** as written; DN-Q11's
  descending named load is measured before DN-S2.
- **DN-Q9 (a), revised against the tree**: the mount refuses the volume by
  its superblock version, **naming DN's format event** in the message
  (the version DN's bump takes, which DA-S3's owed bump may share) -
  not by a relation's name, which the refusal never reaches.
- **DN-Q12 (a)**, written into §4 first: the amended invariants are staged
  in this order (§8) and move into `heap-and-tuple.md` and `CLAUDE.md`
  with DN-S2's code.
- **DN-R5 corrected**: "the tree's first word reserved against identifiers"
  reads "the first word reserved by the operator's word".
- **DN-R9 widened**: invariant 9 is amended with 4 and 11.

Overhead not measured; measured at the milestone's close.

### DN-S1 - the rules written, staged, 2026-11-20

On worktree `dn-heap-rowid`, documents only: §8, under DN-Q12 (a). **This
is DN-S1's gate, and DN stops here**: the stage ends in the operator's mark
on the amended invariants (§5), DN-S2 writes against them, and a hard
invariant is amended only under a recorded mark of the operator's - DN-D7
is one for 4, nothing yet is for 9 or for 11's restatement, nor for
DN-Q13 (c) and DN-Q14 (a), which §8 carries. **Also owed before DN-S2**: a
driver that removes index entries unasked (DD-S4, with the operator; without
one a heap reuses a slot only after an administrator's `PURGE INDEX`), and
DN-Q11's descending named load.

**Reviewed by `critics-developer`** (DN-S0 and DN-S1 together). It read
every `[source-read]` claim at its line and found them true but one.
**Fixed by the reviewer**: the `ORDER BY` elision's citation (the ascending
arm is `EmitsTupleKeyOrder`, `kTupleKeyBackward` only the reverse); §8's
invariant 4, which dropped three clauses of the present text; 11's "in place
of", which quoted `CLAUDE.md` alone; §3.1b and K3, which left a heap with a
multi-column or non-`int64` PK no Keystone index. **Applied from its
findings**: **DN-Q14 written and adopted** - its most serious finding, a
deleted row's Keystone bound twice once its slot is reused; **DN-Q13
revised from (a) to (c)**; DN-Q1's read-side cost stated; DN-Q9's message
no longer names version numbers nobody has allocated; invariant 11's
duplicate clause restated for a heap; the order's read-at line names both
commits; this entry cut to its gate. **Rejected**: none. It recommended
DN-Q13 go back to the operator unmarked; it goes to the operator with §8,
adopted as (c) and open to the mark.

### DD-S4 ruled into DN-S2, 2026-11-21

On worktree `dn-dd-s4-into-dn-s2` from `dcab41e5` (`v3.5.0-39-gdcab41e5`),
documents only. The operator marked **DD-S4 option (b)**
(`raft-marks-2026-11-21.md` §6): the unasked driver is built in DN-S2, after
this order's DN-S1 mark, and is designed with DN-R2's record that no entry
of any index of the relation names a slot - DD's `PURGE INDEX` judges one
index (DD's §0a item 5), so the driver's unit is the relation, not an
index. §5's DN-S2 row now carries it. The design is DN-S2's and is not
written here.

Taken up on the operator's standing go-ahead of 2026-11-21
(go-ahead-achieving-milestone), and **stopped at once at DN-S1's gate**:
the mark on §8 (invariant 4, 9, 11's restatement; DN-Q13 (c), DN-Q14 (a),
both `[quiet-wrong]`) amends hard invariants, which only a recorded mark of
the operator's does. Owed before DN-S2, besides the mark: DN-Q11's
descending named load, measured. No item was adopted at this entry.

### DN-S1's gate passed, 2026-11-21

The operator's word, *"For now follow the proposal, and for what remains
/go-ahead-achieving-milestone"* (given in Korean; recorded in English),
given after CLA's account of §8 item by item. **The operator's own mark**:
§8 as staged - invariant 4 (under DN-D7), 9, 11's restatement, §3.1b,
IX1, F1, K3 and §4.1c - with **DN-Q13 (c)** and **DN-Q14 (a)**. CLA's
account named one cost of DN-Q14 (a) the order had not written: a KWP load
into a hidden-Keystone heap names its keys in ascending order, since the
mark is above every key placed; it is written into DN-Q14's row as a cost
of the marked option. "For now" is recorded as said: the mark stands until
the operator moves it. DN-S2, DN-S3 and DN-S4 run on the standing go-ahead
of 2026-11-21 (go-ahead-achieving-milestone), on worktree
`dn-dd-s4-into-dn-s2`, branch `worktree-dn-heap-s2`, from `3a07ad49`.

### DN-S2 - the heap, 2026-11-21

On worktree `dn-dd-s4-into-dn-s2`, branch `worktree-dn-heap-s2`, from
`b7262f9b` (`v3.5.0-41-gb7262f9b`), on the operator's standing go-ahead of
2026-11-21 (go-ahead-achieving-milestone). Three steps, each reviewed by
`critics-developer` and gated by the full suite.

- **S2a, `e0c34e20`: the page.** `PageType::kHeapData` (15, DN-Q15), a free
  slot (`kSlotFlagFree`) released by `PageView::FreeSlot` and taken again by
  `ReuseSlot`, SLOT_FREE (36), the rowid codec (`storage/heap/rowid.hpp`),
  superblock 39. **Review**: redo replayed a HEAP_OVERWRITE onto a free slot
  as a reuse - refused `Corruption` now; DN-R10 checked again at redo; a
  data page's slots never shift (`InsertTupleAt` mid-directory and
  `UnInsertTuple` but the last refused there); `ReuseSlot` bounds-checked;
  one tuple writer (`WriteNewTuple`), one slot-record logger. Rejected: none.
- **S2b, `9a69c21d`: the heap.** `ClusteredType::kHeap`, `CREATE TABLE ...
  HEAP` with its head page (`min_key` 1, DN-Q16) and Keystone index
  (DN-Q2 (a), named per DN-Q22), every index entry carrying the rowid
  (`IndexLayout::rowid_width`) and resolved at it with the Keystone check
  (DN-R3), `exec::LocateRow` for every key access, placement by space with a
  growth logged as a split (DN-Q17), the btree's walk reading a heap's chain
  while every descent refuses a data page, the compiler giving a heap no
  tuple key range, no `ORDER BY <tuple key>` elision and a build prefix by
  place, `PRIMARY KEY` (DN-Q21), `keyidentity` (DN-Q19), DN-Q14 (a)'s mark
  (DN-Q20). **Review**: an online `CREATE INDEX` over a heap was refused, and
  past that its A2 bound (a chain ordered by `min_key`) would have left older
  rows unindexed - fixed (the high water over the whole chain, a page past it
  skipped, not ending the walk); `DROP TABLE` of a heap leaked its data
  pages; `GATHER STATISTICS` and `sys.relation_stats` read no data page; a
  refusal named `$tuple_key`. Adopted from its proposals: a rowid naming
  another relation's page is a miss, not `Corruption` (a crash can leave an
  entry whose growth redo never created, and `Corruption` would refuse every
  judge that could remove it); a rowid on page 0 refused (`PutIndexRowid`);
  `TableAccess::heap()`; `HeapSpace::Forget`. Not applied: folding
  `WalkHeapPages`' cursor epilogue into `WalkBtreeLeaves`' (two shapes of
  resume), dropping `IndexRef::rowid` (it keeps `IndexLayoutOf` free of the
  access). The design questions it raised are `known-gaps.md`'s DN section.
- **S2c, `7ae0f800` and `bc8c9d97`: the slot release, unasked** (DD-S4 under
  the operator's `$!b`, DN-Q18): `exec::HeapSlotRelease`, stepped by
  `CommandDispatcher::StepHeapRelease` from core 0's `system` tick, queued by
  `HeapSpace::NoteDead`. **Review**: **`7ae0f800` did not compile** - a
  rename onto `SysTableRow` - and the suite CLA reported for it had run an
  older binary, CLA's build filter having missed GCC's `error:` lines; the
  build and the suite were re-run at `bc8c9d97`. **A stale catalog memo let
  the release free slots an index built afterwards still named** (DN-R2,
  `[quiet-wrong]`) - each step revalidates the catalog now; an online build's
  index can append behind the verify, so a job meeting one frees nothing and
  re-queues; a tombstone the job deferred was never asked again; the free
  was unbounded (DD-R10). Adopted from its proposals: a rollback's
  compensation queues the relation (`TransactionManager`'s heap-retire
  observer), so does every mount, and the queue is taken in arrival order.
  The PURGE INDEX judge groups a leaf's entries by `(tuple key, rowid)`.

**Gate**: the full suite 0 failed of 5090 at `bc8c9d97`; `scripts/sim.sh`
280 runs, 0 failures, at `7ae0f800` before heaps were drawn and at
`bc8c9d97` with them (DN-S3). Overhead not measured; measured at the
milestone's close. **Adopted on the standing go-ahead**, none the
operator's own mark: DN-Q15 to DN-Q23 (§4), DN-Q20 `[quiet-wrong]` among
them - "at or below its issue mark" read as below `next_id`, which the
marked cost, an ascending load, requires. **Moved into the owning documents
with the code** (DN-Q12 (a)): §8's text into `heap-and-tuple.md` §3.1b,
§4.1c and §8 and `CLAUDE.md`'s invariants 4 and 11, IX1, F1 and K3 - with
one sentence §8 did not stage, invariant 11's "no relation refuses a named
key for being below the mark", which DN-Q14 (a) makes false and which now
names the hidden-Keystone heap.

### DN-S3 - the consumers, 2026-11-21

On worktree `dn-dd-s4-into-dn-s2`, on the same standing go-ahead. §1.3's
paths under DN-Q2, Q5, Q6 and Q10:

- **DN-Q2 (a)**: every path that finds a row by its Keystone - a point
  read or write, `FOR UPDATE`, `RETURNING`, a foreign key's checks, an
  insert's uniqueness, `ON CONFLICT`'s arbiter, `PURGE`'s tombstone,
  `CleanDroppedCells`, the unique check, the backfill's re-read, the Cabin
  and Waystone fallbacks - goes through `exec::LocateRow`, the Keystone
  index on a heap (DN-S2). An index step and the ordered index walk resolve
  at the entry's rowid first (`ResolveIndexedRow`).
- **DN-Q5 (a)**: a declared-Keystone heap is a foreign key parent; a hidden
  Keystone's is refused `Unsupported` as before.
- **DN-Q6 (a)**: `PURGE` by Keystone on a declared-Keystone heap - the window
  walked in page order, every key judged, the tombstone held through the
  Keystone index (`exec::PurgeKey`'s one `HoldTombstone`, which replaced
  `btree::BtreeHoldTombstone`), the purged slots queued for the release.
- **DN-Q10 (a)**: no change was needed - a Cabin's and a Waystone's location
  hint is a `(page, slot)`, which on a heap is the rowid and never moves,
  verified against the Keystone at the slot (`VerifyTupleAt`), and the
  fallback is `LocateRow`.
- **The simulator draws heaps** (a declared-Keystone heap per table and as
  the foreign key parent, by its own stream) and steps the slot release
  beside the reclaim, so a slot freed and reused runs under its crashes.

**Review** (of `bc8c9d97`): no correctness defect; four regression cells
added (a window purged whatever the page order, a purged key named again
across the release, a heap parent under the foreign key's rules, a
Cabin-served reverse check over a heap child). Adopted: the two tombstone
holds merged; the simulator steps the release and draws the parent as a
heap; a stale comment, a long line, an include's order, an implicit
default. **Rejected**: an owner token on the heap-retire observer - two
dispatchers over one manager are a test fixture's shape, not production's,
and no cell trips it; `PURGE` served by the Keystone index's window - a
performance change, stated in `known-gaps.md`.

**Gate**: the full suite 0 failed of 5094; `scripts/sim.sh` 280 runs, 0
failures, heaps drawn and the release stepped. Overhead not measured;
measured at the milestone's close.
