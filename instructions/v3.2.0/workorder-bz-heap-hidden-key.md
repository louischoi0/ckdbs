# Work order BZ — the heap redefined: a btree relation with no tuple key column

Written 2026-10-17 on `worktree-by-heap-redefine` from `e1333a77`
(`v3.2.0-150-ge1333a77`), on the operator's words, recorded in English:

- **W0**: "Now the heap table SUS will be lifted. But the heap table
  concept is redefined: every optimization behaviour for heap tables is
  discarded, and it simply means a table with no identity column defined -
  pages go in insertion order or arbitrary order - but instead of a linear
  search through the tail, it is managed by a btree keyed on the time a row
  was most recently added."
- **W1**, CLA's four questions of the same exchange, answered:
  - identity: **a hidden Keystone** - the engine still issues the 48-bit id,
    no user column carries it;
  - the btree key: **the issued id, which is insertion order** - the heap
    is the clustered B+ tree under a hidden issued key;
  - what is discarded: **the heap chain and tail append**;
  - existing volumes: **superblock bump, older volumes refused**.
- **W2**: "And the behaviour must not carry the constraint that a key that
  does not exist but has a low id cannot be inserted because of the high
  key mark, even with a hidden Keystone."
- **W3** *"$!a"*: the hidden key is **not nameable from SQL**; only the
  paths that already carry a key - the KWP load stream's tuple key field
  (`bulkinsert.md` BI15) - supply one, and it is placed where it sorts.
- **W4** *"$!a"*: the bulk sorted-fill (`bulkinsert.md` T3-2) and the
  physical optimizer's Part I (relayout, `SHOW RELAYOUT`) are **deleted
  with the chain**, not rebuilt over the btree.
- **W5**: "Write the work order first."
- **W6** (2026-10-17): `/go-ahead-achieving-milestone` - BZ opened;
  BZ-Q1 (b) and BZ-Q2 (a) adopted on the standing go-ahead
  (`raft-marks-2026-10-17.md` §2), not marked by the operator item by item.

**Claim tags**: `[source-read]` with `path:line` at `e1333a77`; `[design]`
for reasoning about code not yet written; `[quiet-wrong]` where a wrong
choice turns a correct answer into a wrong one with no error.

**The letter.** `[source-read]` BY is the last work order under
`instructions/v3.2.0/` (`workorder-by-reindex-drop-index-reclaim.md`). BZ is
unused. CLA's choice.

**BZ against other orders.** BZ lifts SUS-1
(`instructions/v3.0.0/workorder-as-sus1-heap-suspended.md`) and replaces
`heap-and-tuple.md` §3.1b. BY plans superblock 29; BZ takes the next
version at its landing, whichever lands first. Not part of AR0 §8's chain.

---

## 0. What BZ is

A **heap relation** is a relation whose user declares no tuple key column.
Physically it is the clustered B+ tree every other relation is: rows sit
in leaves by Keystone id, and the Keystone id is the one the engine issues
at insert, so leaf order is issue order is insertion order, and every
insert takes the existing append-split path. Nothing about it is
heap-specific below the catalog: no chain, no tail, no mark refusal, no
fast path, no relayout.

What the user sees: the relation's columns are exactly the ones declared.
`SELECT *` does not show the key, a positional `INSERT` does not supply it,
and no statement can name it.

What it is not: the semi-sorted heap of `heap-and-tuple.md` §3.1 and §3.1b.
That structure leaves the tree for user relations entirely (§2, BZ-R5).

---

## 1. The tree as read `[source-read]` at `e1333a77`

- **SUS-1's refusal is one parse branch.** `HEAP` after `CREATE TABLE`
  is refused `Unsupported` unless `HeapStorageAllowedForTest()`
  (`src/parser/parser.cpp:1851-1859`), and sets `ClusteredType::kHeap`
  otherwise (`:1862-1863`). The default is `kBtree`
  (`src/server/command_dispatcher.cpp:4303`).
- **Column 0 is the tuple key by position, everywhere.** The catalog
  refuses a relation whose first column is not integer-typed
  (`CheckKeystoneColumn`, `src/catalog/schema.cpp:141-151`, called from
  `Catalog::CreateTable`, `src/catalog/catalog.cpp:1334`); the row layout
  puts column 0 in the Keystone word and starts the body after it
  (`src/catalog/row_layout.cpp:62-75`); the surface refuses a nullable
  column 0 with its byte (`command_dispatcher.cpp:4034-4041`).
- **The mark refuses only on a heap.** `Catalog::AdmitExplicitRowId`
  places a btree key below `next_id` and refuses a heap's
  (`src/catalog/catalog.cpp:2901`, the branch at `:2919-2926`;
  `RefuseRowIdBelowMark` at `:2877`); the heap
  insert additionally refuses below its tail's `min_key`
  (`heap::ChainInsertNamed`, `src/storage/heap/heap_chain.cpp:180`, the
  refusal at `:192`, called at
  `command_dispatcher.cpp:6112`). A btree refuses a named key for two reasons
  only, `AlreadyExists` and `OutOfRange` of `[1, 2^48-1]` (BD-R5).
- **The chain is shared with the system catalog.** `heap::ChainVisit` and
  `heap::ChainInsert` walk and grow the bootstrap catalog relations and the
  declaration relations (`src/catalog/catalog.cpp:84,1096,1612,3615,4016`,
  `src/exec/declaration_relation.cpp:92,217,246`,
  `src/exec/catalog_spills.cpp:43`). **The user-relation arms are
  separate**: `ChainInsertIssued` (`command_dispatcher.cpp:6168`),
  `ChainInsertNamed` (`:6112`), `ChainAppendCarved` (`:5684`, the sorted
  fill), `ChainVisitOnePage` in the dispatcher (`:6443`) and the step VM
  (`src/exec/step_vm.cpp:2189`), and the FK walks, which call the kept
  `heap::ChainVisit` on a user relation (`src/exec/fk_check.cpp:153,393`).
- **`ClusteredType::kHeap` is named in** `src/catalog/catalog.cpp` (3),
  `src/stats/relayout_planner.cpp` (2), `src/server/command_dispatcher.cpp`
  (2), `src/parser/parser.cpp` (1), `include/kds/server/command_dispatcher.hpp`
  (1, a comment), and 49 test sites, `tests/catalog_test.cpp` (28) and
  `tests/core_runtime_test.cpp` (14) the most. **Two of catalog.cpp's three
  are the system relations' own `sys.tables` rows**: the bootstrap tables
  (`:736`) and the declaration relations (`:862`) are written `kHeap`, the
  on-disk value 0 (`include/kds/catalog/well_known.hpp:530`).
- **The sorted fill** is `SortedFillEligible` (`command_dispatcher.cpp:5543`)
  and its call (`:5485`), heap-gated. **Relayout Part I** is
  `src/stats/relayout_planner.cpp` (281 lines) and
  `include/kds/stats/relayout_planner.hpp` (187), plans only for a heap,
  and reports through `SHOW RELAYOUT` and the `sys.relayout` view
  (`src/server/sys_views.cpp:70,230`, `RelayoutView` at `:788`).
- **The `heap-suspended` label** is excluded by `scripts/test.sh:17`;
  nine source, test and script files name it or the test bypass.
- **A secondary index on a heap is refused** (`docs/spec/index.md` §3 IX3,
  `:111-123`, listed in §11 at `:679`), and **a heap FK parent is refused** (`foreign-keys.md` F1),
  both because a heap had no tuple key descent.
- **`sys.tables` carries a retired byte**: `retired_key_order`, written 0
  and read by nothing (`include/kds/catalog/rows.hpp:117`).
- **The superblock is 28**; BY plans 29.

---

## 2. Rulings `[design]`

- **BZ-R1 — a heap is a btree relation with a hidden tuple key.**
  `CREATE TABLE t (...) HEAP` creates a `kBtree` relation whose column 0 the
  engine adds: `int64 NOT NULL`, named `$tuple_key` (the lexer reads `$` +
  identifier as a parameter, refused everywhere, so no statement spells
  it), flagged hidden. The declared columns follow at positions 1..n. The
  row layout, the Keystone word, MVCC, undo, the WAL, Waystone, Cabin,
  assertions and Transitions see an ordinary btree relation and do not
  change. `CheckKeystoneColumn` runs on the engine's column 0, so the
  user's first column takes any type and may be `NULL`.
- **BZ-R2 — the marker is a `sys.columns` flag** (BZ-Q1, revised).
  `SysColumnRow::kColumnHidden = 0x04` on column 0, beside BW's
  `kColumnDropped` and `kColumnInSlot`. `ClusteredType`'s on-disk value 0
  stays, renamed `kCatalogChain`: the system relations' own `sys.tables`
  rows carry it (`catalog.cpp:736`, `:862`), and no user `CREATE TABLE`
  writes it again. `kBtree` is the only clustered type a user relation has.
- **BZ-R3 — the surface does not show the hidden column.** One resolver,
  `Schema::FindColumn`, skips it, so no name reaches it. Every site that
  already skips a dropped column for the user (BW-R6) skips a hidden one:
  `SELECT *` and `rel.*` (native, query layer, `DescribeStar`),
  `RETURNING *`, `DESCRIBE` and the `sys.columns` view, a positional
  `INSERT`'s live column list. Hidden is not dropped: storage, row codec
  and catalog sites that skip a dropped column keep reading the hidden one.
  A positional `INSERT` on a heap supplies exactly the declared columns
  and is never read as naming the key; its arity refusal names no hidden
  column. A heap result carries no `kFieldFlagKeystone` field. CREATE
  TABLE's position checks (a nullable or Cabin "tuple key", an FK's
  `column_no`) read the engine's positions, so the user's first column is
  position 1. **The KWP load stream is exempt** (W3, BI15): `S_LOAD_READY`
  describes field 0, the hidden key, and a NULL there takes an issued id.
  Column numbers inside the engine keep the hidden column at 0.
  `[quiet-wrong]`: a surface that counts the hidden column shifts every
  positional value one column, and columns of one type absorb it with no
  error.
- **BZ-R4 — no SQL statement names the key** (W3). `PURGE` on a heap is
  refused `Unsupported` with the relation token's byte: its window is
  written in tuple key comparisons. `ON CONFLICT (<tuple key>)` cannot name
  it and gets the unknown-column refusal; `ON CONFLICT DO NOTHING` and
  `RETURNING` are admitted, as on any btree. The load stream's named key is
  placed by BD-R5 - **no mark refusal on any path** (W2), since the only
  one, `RefuseRowIdBelowMark`, goes with the chain.
- **BZ-R5 — the user-relation heap is deleted, the catalog's chain is
  kept** (W1, W4). Deleted: `ChainInsertIssued`, `ChainInsertNamed`,
  `ChainAppendCarved`, the user-relation chain arms in the dispatcher
  (`VisitRelation`'s, `LocateByTupleKey`'s fallback, `LogChainGrowth` and
  the `kHeapInsert` logging arm), the step VM (the heap ordinal, the heap
  walk, the Cabin hint-failure fallback), `fk_check`'s heap walks, the
  DROP TABLE reclaim's `Expect::kHeapChain`, `ta.heap_tail_hint`,
  `RefuseRowIdBelowMark` and `AdmitExplicitRowId`'s heap branch, the
  `ON CONFLICT`/`RETURNING` heap refusals, `HeapStorageAllowedForTest`,
  the `heap-suspended` label and every cell only it carries, the sorted
  fill (`SortedFillEligible`, `bulkinsert.md` T3-2), the relayout planner,
  `SHOW RELAYOUT` and the `sys.relayout` view (Part I of
  `physical-optimizer.md`; Part II, the Cabin controller, stays). Kept:
  `heap::ChainInsert`, `ChainVisit`, `ChainTail`, `ChainLength` and the
  `kHeap` page class, which the system catalog and the declaration
  relations are built on - moving them is its own format change, outside
  W0. Operator precedent: removal over a flag.
- **BZ-R6 — superblock bump, older volumes refused** (W1; standing order
  of 2026-10-07). A volume before it may hold a pre-SUS-1 chain relation;
  `Decode` refuses it, so no code reads a user chain again. BZ takes the
  next version at its landing (BY plans 29).
- **BZ-R7 — what a heap admits.** A secondary index: yes - IX3's reason,
  no tuple key descent, is gone. An FK parent: no - a child's FK column
  holds the parent's tuple key (F1), which a heap never shows; refused
  `Unsupported` at `REFERENCES` with the parent token's byte. An FK child:
  yes. Assertions, Transitions, Cabins, `ALTER TABLE`: as on any btree.

---

## 3. What does not move

The clustered B+ tree, its split protocol and the issue cursor
(`next_id`, `kMaxIssueRounds`); `HEAP` as the storage word that declares a
heap and `BTREE` as the default; every btree relation's surface;
invariants 1, 2, 5-10, 12-14 as written; the system catalog's chain
relations; the var-heap. A heap's scan order without `ORDER BY` is key
order, as any btree's - the engine promises no order, as before.

---

## 4. Questions

- **BZ-Q1 — where the hidden-key marker lives.** (a) the retired
  `sys.tables` byte as `key_hidden`; (b) a `sys.columns` flag on column 0;
  (c) `ClusteredType::kHeap` over btree storage. CLA proposed (a) at the
  writing; **revised to (b)** against the tree before adoption: every
  surface site BZ-R3 names already reads a column row and skips a flagged
  one (BW-R6's `dropped()`, 26 sites), so a column flag is one predicate at
  existing skips, where (a) threads a table byte to each of them, and (c)
  makes every `kBtree` comparison admit a second value. **Adopted (b)**
  on the operator's standing go-ahead of 2026-10-17
  (go-ahead-achieving-milestone).
- **BZ-Q2 — the FK parent refusal.** (a) refused (BZ-R7); (b) admitted,
  with child values no client can obtain. **Adopted (a)** on the
  operator's standing go-ahead of 2026-10-17 (go-ahead-achieving-milestone).

The close measurement is not a question (Session Workflow step 3): one
interleaved A/B in `build-release`, from `e1333a77` to the close, btree
`INSERT` and point `SELECT`, plus a heap relation against its btree twin
at the close, inside the one-hour budget.

---

## 5. Stages

- **BZ-S1 — the hidden key, and SUS-1 lifted.** Parser: `HEAP` no longer
  refused. Dispatcher: a heap's `CREATE TABLE` builds the engine's column
  0 and the declared columns after it. Catalog: `kColumnHidden`,
  `kCatalogChain`, `CheckKeystoneColumn` on column 0, the superblock bump.
  Surface: every BZ-R3 site. Refusals: `PURGE` on a heap, a heap FK
  parent. Index DDL admits a heap. Tests: a `[quiet-wrong]` cell per
  BZ-R3 site with two same-typed columns, so a one-column shift fails; a
  load-stream cell placing a named key below `next_id` on a heap (W2);
  restart and crash-recovery cells over a heap.
- **BZ-S2 — the user-relation chain, the sorted fill and relayout Part I
  deleted.** Every BZ-R5 deletion; tests naming the old heap rewritten
  onto BZ-S1's heap or deleted where their subject (the chain) is gone;
  `scripts/test.sh` loses `-LE heap-suspended`.
- **BZ-S3 — the text.** `heap-and-tuple.md` §3.1b rewritten to BZ's
  heap; §3.1 and invariants 2-4 scoped to the pages that still are heap
  pages (the catalog's); invariant 11's "first column must be
  integer-typed" stated for a declared tuple key; `index.md` IX3 and §11,
  `foreign-keys.md` F1 per BZ-R7; `protocol.md` on `kFieldFlagKeystone`;
  `bulkinsert.md` T3-2 and `physical-optimizer.md` Part I removed;
  `manual/`; the CLAUDE.md rows; SUS-1's work order marked lifted.
- **BZ close** - the measurement above.

Each stage: `critics-developer` review, full suite.

**S1 and S2 are built as one stage** (revised during BZ-S1, adopted on
the operator's standing go-ahead of 2026-10-17): `HEAP` now names the
hidden-key btree, so every cell and the sim workload that created a chain
through `HEAP` either moves to its new meaning or loses its subject in the
same commit - a green S1 alone would have meant rewriting those cells onto
a chain only S2 then deletes. The debug text port's `INSERTED ... id=`
reply stays on a heap: it reports where a row was placed (id, page, slot),
as for every relation, and is no column.

---

## 6. Review of this order

`critics-developer` at `e1333a77`: §1's line numbers and counts corrected
in place; three gaps applied - `kHeap`'s on-disk value kept for the
system relations (BZ-R2), the load stream exempted (BZ-R3), the missed
position sites and heap arms listed (BZ-R3, BZ-R5); its "no marker, the
name marks it" cut rejected in favour of BZ-Q1 (b) - a flag is read where
`dropped()` already is, a name compare is a string test at every one of
them; the strawman questions and the separate deletion stages cut.

---

## 7. Stage log

Nothing landed.
