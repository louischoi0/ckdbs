# Work order DN - the heap redefined again: an Oracle-style heap, rows in data pages addressed by a computed rowid, every key served through an index that names the rowid, a full scan that reads the data pages and no index

Status: **WRITTEN 2026-11-20, not opened.** Written in the main checkout on
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

No stage is started. Every `[source-read]` claim below was read at
`d091d096`. No suite was run: the order changes no build input.

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
DD's census did not have, and DD-S0 should be re-read with it.

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
  `*` never includes it. **It is the tree's first word reserved against
  identifiers by the operator's word**: "nothing new is reserved lightly:
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

(a) is CLA's proposal in each. **Marked by the operator: DN-D1..D7 (§0).**
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

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DN-S0 - census and premise** | No engine code. **Read**: every path that finds a row by its Keystone (§1.3 completed); every path that assumes a relation's rows are a clustered tree - a walk, a key range, an `UPDATE`'s and `DELETE`'s locate, `ON CONFLICT`, the KWP load, recovery's redo of a tuple, backup's page classes, the sim; every site that reads invariants 2-4 and 11; what DD must provide for DN-R2, against DD's §4. **Measure**, `build-release`, inside the stage budget: a secondary index point read and an index range of 20 and 1,000 rows over a btree relation, against the same with the clustered descent replaced by a page fetch in a spike never committed - the most DN's address can save. | the census, the results file, DN-Q0..Q10 restated, and DD's re-read premise back to the operator |
| **DN-S1 - the rules written** | `heap-and-tuple.md` §3.1b and §8, `CLAUDE.md`'s invariants, `index.md`, `foreign-keys.md`, `keystoneid-invariant.md` amended under the marks (DN-R9). Documents only. | the operator's mark on the amended invariants |
| **DN-S2 - the heap** | After DD :>> DN-S2. The data pages, the rowid, the indexes naming it, the insert, the delete, the scan, the slot reuse under DN-R2; the superblock. | review, the full suite, `scripts/sim.sh` |
| **DN-S3 - the consumers** | §1.3's paths under DN-Q2, Q5, Q6, Q10. | review, the full suite |
| **DN-S4 - the close** | DN-S0's cells against the build; the btree standard cells (DN-R7); a heap against its btree twin on insert, point read, secondary-index read and scan. | closed |

## 6. What DN does not do

- No change to a btree relation, which stays the default (DN-Q8 (a)).
- No row migration or chaining: a row never moves (§2).
- No global insertion order (DN-D4).
- No foreign key to a non-`int64` PK (DN-Q5 (a)).
- No index-only scan.
- No claim against Oracle: no twin cell exists.

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
