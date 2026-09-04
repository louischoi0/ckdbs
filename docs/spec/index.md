# Secondary indexes: multi-column and covering

Status: **built** — the storage layer, the catalog, the grammar, the write
hook, its WAL records, the backfill, the compiler, the read path and the
`indexes` switch. A statement on an indexed column **descends the index**,
and `tests/index_contract_test.cpp` is the suite that keeps it honest.
Documented in `CLAUDE.md`, `docs/spec/client-manual.md` and
`docs/spec/heap-and-tuple.md` §7. §13 names what this spec does **not**
settle.

This document owns the secondary index. It does **not** own the clustered
primary-key tree (`include/kds/storage/btree/`), which is a relation's
*storage* and is a different thing wearing a similar shape — §4 says why
that module is not reused.

---

## 1. What an index is here, and which trust class it lands in

A secondary index is a durable, always-maintained, ordered map from an
**index key** — one or more non-pk columns — to a **primary key**, over one
relation.

The engine already has three trust classes. This is not a fourth; it is the
second entry in the first one, and stating exactly where it lands is the
whole of the design:

| structure | authoritative for | maintained on |
|---|---|---|
| clustered pk tree | every pk | every write |
| **secondary index** | **every key value** | **every write** |
| Cabin | values queries have observed | every write, for observed values |
| Waystone trail | nothing — advisory always | nothing |

An index is a Cabin that observed everything. That is not a metaphor: the
invariant is `cabin.md` §1's, verbatim, with the observation
precondition struck out.

> **IX1 — observed ⇒ complete becomes simply complete, superset form, per
> snapshot.** An index's entry set for a key value is a **superset** of the
> pks of the visible rows qualifying on that key, and the surplus is
> subtracted at read time by verification — MVCC visibility, plus a re-check
> of the key predicate against the resolved version.

*Complete* because maintenance runs on every write, with no threshold and no
cap that could decline. *Superset* because entries are appended per version
and never removed, so an entry may name a row whose current version no
longer carries that key.

What that buys is the same thing a Cabin buys and more of it: an index probe
that finds no entries is an authoritative **"no rows" without opening the
relation** — for *every* value, not only the observed ones. Absence has a
witness, and the witness is that the write path could not have skipped it.

### 1.1 Why the surplus is not a defect

Work the two-version case through, because it is the case that makes the
design sound rather than merely convenient. Version identity is per logical
tuple (`docs/spec/txn.md`): `OverwriteTuple` keeps `(page_id, slot)`, and undo
versions have no address. So an UPDATE of key `v → v′` leaves two entries,
both naming the same pk.

- Old-snapshot reader probing `v`: finds the entry, resolves the pk to the
  version its snapshot sees — the old one — key matches. Correct.
- New-snapshot reader probing `v`: finds the same entry, resolves to the new
  version, key re-check fails, row dropped. Correct.
- Either reader probing `v′`: symmetric.

No entry has to be removed for any of those four to be right, which is the
same reason `cabin.md` §5 calls removal *incorrect* rather than merely
unnecessary. An older snapshot may still match through the undo chain.

---

## 2. Maintenance

> **IX2 — every maintenance action is an append.** INSERT appends one entry
> per index on the relation. UPDATE appends an entry for the new key and
> leaves the old one. DELETE does nothing. Nothing on the write path removes
> an entry, ever.

Two rules ride along, and both are load-bearing rather than tidy:

- **An UPDATE that touches no key or covered column of an index must not
  append to it.** Otherwise the entry set grows by one per write forever:
  correct by IX1's superset rule, and useless. This is `cabin.md` §5's
  third row, and the index carries the same contract test.
- **Nothing reclaims.** A superseded entry costs memory, a page, and a
  read-time skip; there is no purge of index entries: readers are
  registered per core and `ReadHorizon()` bounds a purge (`docs/spec/txn.md`
  §4.1), but only the undo purge and the catalog's delete-mark purge
  consume it, and no index purge was built. Same bargain the
  var-heap and the undo log strike, stated here so it is not discovered.

### 2.1 Failure on the write path

Unlike a Cabin — where un-observing is always legal, so any failure in the
hook is absorbed — an index **cannot** absorb a failed append: an index
missing an entry is not slower, it is wrong. So a failed index append fails
the statement, and inside a transaction poisons the session exactly as any
other statement failure does (`docs/spec/txn.md`: failure atomicity is per
transaction, not per statement).

This is the one place an index is more expensive to be correct about than a
Cabin, and it is why the index is not also a constraint (IX11).

---

## 3. Which clustered types may carry one

> **IX3 — a secondary index is built on a btree-clustered relation only.**
> `CREATE INDEX` on a heap-clustered relation is refused
> (`Catalog::CheckIndexDef`, `InvalidArgument`, naming this rule).

An entry's payload is the pk. Resolving a pk costs one descent on a btree
relation and a **chain scan** on a heap one, so an index over a heap
relation would turn one full scan into N partial ones — the identical
argument as `foreign-keys.md` F1's refusal of a heap parent. A heap
relation's banked acceleration on a non-pk column is its Cabin
(`cabin.md` §4a), whose hint-plus-walk fallback exists precisely because a
heap has no pk descent to heal with.

Three consequences of the entry format:

1. **The entry carries no location hint** — no `(page_id, slot)`, no
   epoch — so nothing here can dangle and nothing needs healing.
2. **Relayout cannot invalidate an index.** A pk is stable for life
   (invariant 11, K1), so moving a tuple moves nothing an index knows about.
3. **An index step grants no key order.** Phase 1 collects in index-key
   order and sorts into **pk** order before resolving (IX8a), so an
   `ORDER BY` on the key column keeps its sort step, and a planner that
   reads sortedness out of a `kIndexProbe` is reading something it never
   promised.

---

## 4. The pages, and why `btree/` is not reused

> **IX4 — an index gets its own page classes: `PageType::kIndexInternal =
> 11`, `PageType::kIndexLeaf = 12`, and they split by dividing their
> contents.**

`storage/btree/btree.hpp` cannot host this, and not by accident: an index
page is a different *kind* of page. The clustered tree's leaf is a heap page
holding tuples addressed by one Keystone id; an index page holds entries
keyed by `key || pk`, with no Keystone word, no MVCC header and no
`PageView`. Teaching the clustered module to also store entries would be the
merge that costs both, and **a secondary key is not monotonic** —
arbitrary-order arrival is the defining property of the thing, so its
splits were mandatory from the first line.

The escape is that an index page is **not a heap page**:

- It holds **entries**, not tuples. No Keystone word, no MVCC header, no slot
  directory, no `PageView`.
- It has no `min_key`, so **invariant 2** (min_key immutability) has nothing
  to be about here.
- It contains no tuple, so **invariant 3** (no tuple with `id < min_key`) has
  nothing to be about here either.

Dividing a full index leaf therefore decides exactly one thing — how *index*
pages split — and that decision is made here, scoped to this page class:

> **IX4a — an index leaf splits at its entry midpoint**; the new right
> sibling takes the upper half, and the separator copied into the parent is
> the **first key of the right sibling**. `[PROPOSED]: the midpoint.` The
> fill target is a parameter of the split function, never a constant a caller
> may assume.

Same separator convention as `btree_page.hpp`: a separator is the **low key
of the subtree it points at**, so a separator and a page's own first key can
never disagree about where a descent lands.

### 4.1 Leaf layout

```
[ common page header    ]  offset 0, 32 bytes (page_header.hpp)
[ IndexLeafHeader       ]  nr_entries, entry_width, right_sibling, level=0
[ entry array           ]  nr_entries * entry_width, sorted ascending, packed
[ ... free space ...    ]
```

The entry array is **fixed-width and sorted**, so a leaf is binary-searched
directly — no slot directory, because there is nothing of variable length to
address. A leaf carries a `right_sibling` link so a range walk crosses leaves
without re-descending, which is the clustered tree's `next_page_id` under a
different name.

Internal nodes are `btree_page.hpp`'s shape with the key widened from
`uint64` to the **sort key** of §4.2 — separator array, `leftmost_child`,
`level`, binary-searched, no right-link.

### 4.2 Entry layout

```
[ key : key_width bytes ]  the encoded composite key (§5)
[ pk  : 8 bytes         ]  zero-extended Keystone id (invariant 7)
[ covered cells         ]  ncovered * inline_cell_width, or 0 bytes (§7)
```

`entry_width` is a **schema constant of the index**, computed once at
`CREATE INDEX` and stored on the catalog row — the same relationship
`RowLayout::row_size` has to a relation. An entry whose length disagrees is
`Corruption`, never interpreted, exactly as invariant 13 requires of a tuple.

Duplicate keys are ordinary: entries sort by `(key, pk)`, so the pk breaks
the tie and the total order is strict. That also makes an entry's position
deterministic, which a split's arithmetic wants.

> **IX4b — the tree routes on the whole `(key, pk)` sort key, and a probe
> that knows less is zero-padded rather than compared short.** A
> correctness rule, not a convenience.

A secondary key is not unique, so a run of duplicates can span leaves. If a
separator carried only the key, a probe for a duplicated key would compare
*equal* to it and route right — past the entries in the left subtree carrying
that same key with a lower pk. Half the duplicates would be unreachable by
descent while still being reachable by walk, which is the shape of bug a walk
test cannot see.

So separators are full sort keys, and a probe for key `K` seeks `(K, 0)`.
Padding is the correct floor for free: §5's discriminator byte is `1` for
every value that exists, so a zero byte in that position sorts below all of
them — which makes a partial composite-key probe (`WHERE a = 3` on an index
over `(a, b)`) encode `a`, leave the rest zero, and land on the first entry
that can match.

Two entries may also share a `(key, pk)` pair — an UPDATE that changes a
covered column appends one — and that is information, so it is kept. A
**byte-identical** entry is not, and `IndexInsert` reports it rather than
storing it twice: nothing reclaims an index entry, and a probe that resolved
one pk twice would emit its row twice. Deduplicating *by pk* remains the read
path's job (IX11).

---

## 5. The composite key encoding

> **IX5 — a key is one order-preserving byte string, compared with
> `memcmp`.**

One function, `exec::EncodeIndexKey`, walks the index's key columns in
declared order and appends per column:

| column type | encoding | bytes |
|---|---|---|
| int32 / date | sign bit flipped, big-endian | 4 |
| int64 / timestamp | sign bit flipped, big-endian | 8 |
| uint64 | big-endian, no flip | 8 |
| decimal | unscaled int64, sign bit flipped, big-endian | 8 |
| decimal128 | unscaled int128, sign bit flipped, big-endian | 16 |
| char / varchar | value bytes, zero-padded, **truncated** (§6) | `kIndexStringKeyBytes` |

Each column is preceded by **one discriminator byte**: `0x00` for NULL,
`0x01` for a value. A nullable key column is refused at `CREATE INDEX`
(`docs/spec/null.md`), so the byte is always `0x01` and always costs —
deliberately, so that admitting a NULL key would be a semantic change and
not a **format** event.

Big-endian with the sign bit flipped is the whole trick: `memcmp` over the
concatenation then reproduces the tuple-wise ordering of the columns, so the
descent has **no per-type dispatch at all** and cross-scale or cross-width
disagreement is impossible — a column's scale and width are schema constants,
fixed at `CREATE TABLE`.

Note this is a *different* encoding from `MakeCabinKey` and from the
aggregate group key, and the difference is the point: those encode
**identity** (two NULLs are one group), this encodes **order**. Merging them
would give one function two jobs and one of them would lose.

---

## 6. Truncation: how a string is a key under a fixed-width entry

> **IX6 — a string key column contributes a fixed prefix of its value.
> Truncation costs precision and can never cost correctness.**

A string lives in a tagged cell of `inline_cell_width` bytes and may spill.
Storing it whole would make an entry variable-width and a leaf slotted, which
is the fixed-length rule surrendered for one column type. So the key stores
the first `kIndexStringKeyBytes` bytes, zero-padded.

That makes an equality on the encoded key a **prefix test**, i.e. a superset
of the true match set — false positives only, never a false negative. And a
superset is exactly what IX1 already hands to the reader, which already
re-checks the predicate against the resolved base row. **Truncation adds
nothing to the read path**, because the verification it would need is
unconditional anyway.

- The re-check is unconditional. There is no "was this key truncated?" flag
  and no branch, because a branch is a thing that can be wrong.
- A **spilled** value has no inline bytes, so the write hook resolves it
  through the var-heap to take its prefix: one page fetch on the write path
  for a long key, and never on the read path. The var-heap value is immutable
  per version (invariant 14), so the prefix cannot go stale.
- A range on a truncated key is correct for the same reason and less precise
  in the same way: the walk returns a superset, the residual subtracts it.

`kIndexStringKeyBytes` is `[PROPOSED] 32` (`include/kds/exec/index_key.hpp`)
and nothing may depend on the number: it trades entry width against how
often a probe pays a base descent it did not have to.

---

## 7. Covering, and why there is no index-only scan

Covered columns are stored in the entry after the pk, as their **inline cell
bytes verbatim** — tag included, so a spilled covered value stores its spill
pointer and reading it costs the var-heap fetch it would have cost anyway.

> **IX7 — there is no index-only scan, and there cannot be one. A covering
> index avoids the *base descent for rows that will be filtered out*, not
> the base read for rows that are returned.**

Visibility is decided at exactly one site, `ChainRunner::AcceptTupleAt`, from
the tuple's MVCC header and its undo chain. There is **no visibility witness
outside the tuple** — no purge, no commit watermark, no visibility map — so
an index entry cannot say whether its version is visible to the reader
holding it. Emitting a row from the entry alone would be answering a
visibility question with a structure that has never seen a snapshot.

So the base row is always fetched before a row is emitted, and even
`COUNT(*)` over an index is not servable from the index.

What covering does buy, and it is the larger half of the win on a selective
statement: **residual predicates on covered columns are evaluated from the
entry**, so a row that will not survive the filter never costs a pk descent.
On a probe returning 10 rows from 10,000 entries, that is the whole cost.

`ANALYZE` prices it directly: `index_filtered` counts descents the covered
columns avoided, and nothing else. If it is zero, a `COVERING` clause bought
exactly the write cost it added.

The entry-side test is **conservative by construction**: it answers "drop
this row" only when a residual predicate the entry's own values can decide
says so. A predicate on an uncovered column, an operand that is not a
literal, or a **spilled** covered value all keep the row and let the base
read filter it — the last because resolving a spill would be a page fetch
under the index leaf's span, which `parser-v2.md` I15's R1 forbids, and
because the spill pointer an entry carries may name a var-heap slot a
rollback has since released: entries are never compensated on rollback, and
`CoveredRowSurvives` (`step_vm.cpp`) never follows the pointer. Getting that
direction wrong is the difference between a lost row and a wasted descent.

---

## 8. Two access kinds, both search-class

> **IX8 — `AccessKind` gains `kIndexProbe` and `kIndexRange`. Both are
> search-class; `IsTrailReplayable` does not move.**

- `kIndexProbe` — equalities covering a prefix of the index's key columns.
- `kIndexRange` — equalities covering a prefix, plus an inclusive range on
  the next key column; or a range on the first.

> **IX8a — an index step emits its rows in primary-key order.**

The walk collects pks in *index key* order; a scan of the same relation emits
them in pk order. Without a sort between the two phases, creating an index
would **reorder a reply** — and "an accelerator may cost performance and must
never change a query result" is the standard invariant 8 holds Waystone to,
which an authoritative structure does not get to fall below. The equivalence
test compares byte for byte precisely so this cannot be missed.

It costs one sort of the matched set against one descent per element of it,
and it buys locality as well: on a btree relation pk order *is* leaf order,
so the descents walk the tree forwards instead of jumping.

Invariant 9's line is **lookup versus search**, not authoritative versus
advisory. An index probe returns a *set*, and a set missing a row inserted
since a trail was recorded is wrong in a way no per-tuple validation can
detect, because absence has no witness *in the trail*. That is exactly why
`kCabinProbe` is search-class despite the Cabin being authoritative, and the
index inherits the reasoning unchanged. A trail may prefetch for an index
step; it may never replace one. There is no unique index (IX11), so no
index step is lookup-class.

### 8a. The correlated probe

> **IX17 — a `kIndexProbe` may be keyed by an earlier step's row.** When an
> index's **leading** key column is bound by equality to a column of an
> earlier step or an enclosing chain — a join key — the step compiles to a
> `kIndexProbe` whose leading key value is encoded **per outer row** from
> the chain frame (`IndexProbe::key_from`), instead of the relation being
> walked once per outer row.

This is what an index-served inner join side is, and it serves the shape
equality propagation cannot reach: a join with **no literal** to propagate
— `ON l.user_id = u.id WHERE u.id BETWEEN ? AND ?`, a correlated `EXISTS`.

What it deliberately does not change:

- **Trust class.** Still search-class; `IsTrailReplayable` does not move.
  The set-completeness argument above is unchanged by where the key value
  comes from.
- **The per-row-encoding rule, by one priced exception.** §9's "no per-row
  key building" stands for every literal-keyed step; the correlated form
  encodes exactly one fixed-width column per outer row, priced against the
  full inner walk it replaces. The compile-time `low`/`high` stay as pure
  padding templates the executor copies and fills.
- **Selection purity.** First index in oid order whose leading column
  carries such an equality, first qualifying conjunct in residual order —
  no score, since the pinned prefix is always exactly 1. A literal equality
  that `IndexProbeOf` can encode at compile time always wins over the
  deferred form. Deeper prefixes mixing deferred and literal columns are
  out of scope by decision.
- **The decline rule.** The outer column must share the key column's exact
  type descriptor (`type_val`, `len`) — the executor encodes the outer
  row's decoded value into this column's key format, and only an identical
  descriptor makes that the encoding the index was built from. Any decline,
  at compile or per row, takes the walk and returns identical rows by the
  residual.
- **Cross-core — shipped as its walk.** An index step cannot cross the
  descriptor (core-local structure state), so the session applies a
  **ship-time downgrade** (`ShippedForm`, `step_descriptor.cpp`) at every
  encode seam: the shipped copy becomes the walk the step would fall back
  to anyway — `kScan`, aux dropped, residual intact — which cannot change
  a result, and the local half of the statement still takes the
  structure. The same route carries the `kCabinProbe` case. The peer
  therefore pays *walk* cost, not local cost: a downgraded correlated
  probe runs O(outer × inner) where the local form runs
  O(outer × log inner), and the consuming stage's row-touch budget can
  refuse a large enough shipped join that the local side answers. The
  descriptor's refusal stays as the backstop for callers that skip the
  sanctioned route.

---

## 9. How the compiler picks one

> **IX9 — index selection is `f(shape, catalog)`. No statistics, no data, no
> cardinality estimate.**

Among the relation's indexes, take the one matching the **longest usable key
prefix** of the step's residual equalities; break a tie by **lowest
`index_oid`**, which is creation order.

Deterministic and stable is not a preference here: a recorded pattern must
not compile differently as the data changes, or `pattern_id` stops naming a
plan. It is the same argument `CabinProbeOf` gives for taking the *first*
cabined equality rather than the most selective one.

There is a crossover this rule cannot see, and below it the index is a loss:
measured against the `bench/` tree at `1769487`, a range at 200 rows ran 11%
slower with the index than without it, while a selective equality was 9.7×
faster at 10,000 rows. KDS cannot decline its own index at that size,
because declining needs the cardinality estimate IX9 refuses. That is the
price of a stable plan, and it is small and bounded: the loss is one page's
worth of rows, and the win grows without limit.

Two rules that keep every existing proof standing:

- **The key equalities stay in `Step::residual`.** Exactly as a lookup's key
  and a `BETWEEN`'s bounds do. So "downgrading any step to a plain `kScan`
  cannot change the result" holds with no new proof — and it is what makes
  IX1's surplus subtraction free rather than a new mechanism.
- **An index beats a Cabin on the same column.** The index is complete for
  every value; a Cabin is authoritative only for observed ones. `CREATE
  INDEX` does not drop an existing Cabin — the Cabin simply stops being
  probed and its memory is the operator's to reclaim with `DROP CABIN`.

The compiler reads the relation's cached `index_mask` (§12), which names an
index's **leading** key column only — exactly the right question.

The two kinds **execute identically** — both walk the entries between two
encoded bounds — so the split is a statistics distinction, the same one
`kFilterScan` draws against `kScan`. The bounds are encoded **at compile
time**: coercion is a compile-time act (`types.md` §3.1) and so is the
encoding that follows it, so no per-row key building happens on the read path
— except the correlated form (§8a), which prices its one per-row encode there.
`low` pads its unpinned tail with `0x00` and `high` with `0xFF`, which are the
true bounds because a key column's discriminator byte is 1 for every value
that exists.

A value the key encoder refuses — an integer wider than its column —
**declines the index** rather than failing: the step falls through to the
walk, which returns the identical rows because the residual is untouched.

---

## 10. Grammar

```
CREATE INDEX <name> ON <table> (<col> [, <col>]...) [COVERING (<col> [, <col>]...)]
DROP INDEX <name>
SHOW INDEXES
```

`UNIQUE` parses and is refused with an exact byte (IX11).

> **IX10 — nothing is reserved.** `index` and `covering` follow the rule the
> aggregate function names settled: a keyword hashes exactly as the
> identifier it used to be, so a column may still be named `index`, and
> `kFingerprintVersion` does not move. The golden corpus is the evidence, and
> every pre-existing line must pass unchanged.

`CREATE INDEX` and `DROP INDEX` run under a transaction;
`docs/spec/ddl-transactional.md` owns what each guarantees.

An index's **name is unique instance-wide**, so `DROP INDEX` names only it.
That is where an index and a Cabin differ and why: `(relation, column)`
identifies a Cabin uniquely because C3 keeps it to one column, while two
indexes on one relation may share a leading column and differ after it — so
there has to be something to point `DROP` at.

`SHOW INDEXES` prints the declared column order, not a sorted set: that order
*is* what §5 concatenates, so a probe must match a prefix of what is printed.
It also prints `height` and `entries`, walked from the tree, because the
catalog can say an index exists and never what is in it — and prints `-` for
both rather than zeros when the tree cannot be walked, since an unreadable
tree is unknown and zeros would read as "empty".

### 10.1 Building over existing rows

`CREATE INDEX` on a populated relation backfills, and the backfill has one
non-obvious requirement:

> **IX10a — the backfill walks each tuple's undo chain and appends an entry
> per distinct key value across its versions**, not merely for the current
> one — and it runs **before** the `sys.indexes` row exists.

A reader holding an older snapshot must find its version through the new
index. Every version of a logical tuple shares one pk, so the walk is bounded
by the chain and the entries are the same shape. Omitting it would make an
old-snapshot read silently return fewer rows — the failure `cabin.md` §5
calls invisible without a baseline. A **delete-marked** row is walked like
any other: gone for newer readers, still there for older ones, which is
exactly the case the undo chain exists for.

*Distinct* needs no bookkeeping: a version that did not move the key produces
a byte-identical entry, and `IndexInsert` already reports one rather than
storing it twice (IX4b).

Building before publishing is what makes an index **complete or absent, never
partial** — a failed build leaves an unreachable tree and no catalog row,
where the reverse order leaves a declared index missing rows, which is a wrong
answer with a right answer's shape. Nothing can observe the half-built tree:
DDL is one statement on one cooperative thread. A split during the build moves
the root, so the root written into the catalog row is the one the build ended
at, not the page first allocated.

Two consequences worth stating. The walk runs **two phases per leaf** — copy
the page's tuples out, drop the span, then append — because appending fetches
pages and `parser-v2.md` I15's R1 forbids a fetch under a live span; that also
bounds the memory at one page rather than at one relation. And every refusal
runs **before** the walk, through `Catalog::CheckIndexDef` — the same checks
`CreateIndex` makes, factored out rather than copied, so a heap relation is
refused by name instead of surfacing as a page-type error from inside the
build.

---

## 11. What is refused, each with an exact byte

> **IX11 — `UNIQUE` is refused.**

Enforcing uniqueness would make the index a **constraint**, which needs the
visibility question `foreign-keys.md` §4 settled — latest state, never
walks undo, an in-flight writer of the same key is *busy* rather than a
violation — and a second write-conflict path on the insert hook. The index
is a read accelerator that cannot fail a write for a reason of its own.
`kIndexFlagUnique` is defined and never written, exactly as
`UndoRecordType::kInsert` is.

Also refused, each naming the reason:

- **A heap-clustered relation** (IX3, `InvalidArgument`).
- **A relation of two or more ranges** (`RefuseAuxiliaryOnSplitRelation`,
  `NotImplemented`); a split relation never gains an index.
- **A nullable key column** (`NotImplemented`, `docs/spec/null.md`).
- **An index on the primary key** — the clustered tree already is one.
- **A `float` column** — nothing settled its encoding, so it is not
  declarable in the first place.
- **More than `kMaxIndexKeyColumns` key or `kMaxIndexCoveredColumns` covered
  columns.** A cap **refuses**, never truncates: a truncated index declared
  complete is a wrong answer with a right answer's shape. Same rule
  `aggregate_max_groups` follows, and the opposite of a Cabin cap, which may
  fall back because a Cabin is only ever a shortcut.
- **`ORDER BY` served from an index.** An output sort exists
  (`docs/spec/parser-v2.md`), and an index is not it, for four independent
  reasons. **IX8a** has `RunIndexStep` sort its matched pks *back* into pk
  order on purpose, because creating an index must not reorder a reply.
  **IX2's append-only maintenance** leaves an updated row with entries at
  two keys, and the dedup keeps the lowest rather than the visible
  version's — which today only picks an entry and would then pick a row's
  *position*. **§6's 32-byte truncation** makes index order a prefix order,
  not a total order on values, and IX6's "truncation cannot cost
  correctness" is an argument about superset-plus-recheck that does not
  carry here. **IX9's crossover** would be walked into deliberately by an
  index chosen for its order rather than its selectivity, with no
  cardinality estimate to see it coming.
- **Expression, partial and descending indexes.** The key encoding of §5 is
  ascending by construction and reversing a column is a format-visible
  change, so this is a decision and not an omission.

---

## 12. Catalog, format and WAL

> **IX12 — `SysIndexRow` is 116 bytes.** Its introduction was a superblock
> format bump (`11 → 12`); a data file older than that does not mount.

The row (`include/kds/catalog/rows.hpp`):

```
index_oid, table_oid, root_page_id, key_width, entry_width,
name[kCatalogNameMax], nkeys, ncovered, flags, reserved0,
key_cols[kMaxIndexKeyColumns], covered_cols[kMaxIndexCoveredColumns]
```

`kMaxIndexKeyColumns = 4` and `kMaxIndexCoveredColumns = 8` `[PROPOSED]`,
chosen to keep the row inside one catalog slot; both are refusals, never
truncations (§11). The column arrays are in **declared index order**, which
is the order §5 concatenates them in — part of the format, not a
presentation detail. `Decode` refuses a count past its array, so one corrupt
byte cannot make every later reader index out of bounds.

`Catalog::FindIndexOnColumn` answers for an index's **leading** key column
only. Not "contains": an index on `(a, b)` can serve an equality on `a` and
cannot serve one on `b`, so answering yes for `b` would stop the compiler
calling that step a filter scan while leaving it exactly as slow — a lie to
the access statistics, which is that function's only consumer.

`Catalog::UpdateIndexRoot` is how a root split is recorded, the counterpart
of `UpdateRelationDescPage` and for the same reason: the storage layer has no
catalog, so it reports a new root and something above it writes one down.
`DropIndex` **retires** the row rather than delete-marking it (a catalog read
has no snapshot to filter a mark against) and **frees no page**: a dropped
index's tree is never reclaimed.

`TableAccess` carries `index_mask` plus a `std::vector<IndexRef>`, built at
`InitTableAccess()` on the same pattern as `cabin_mask` / `cabin_ids` — the
compiler needs the bit, the executor needs the root page and the widths. The
list is **sorted by `index_oid`**, so §9's lowest-oid tie-break is a property
of the list rather than of how rows happened to land on the catalog page. The
mask names **leading** key columns only, for the reason `FindIndexOnColumn`
does.

> **IX12a — `IndexRef::root_page_id` is the one field on `TableAccess` that
> changes without DDL, so `Catalog::UpdateIndexRoot` updates the cached entry
> **in place** rather than invalidating it.**

A root split republishes the root from inside an ordinary INSERT. Bumping the
catalog version there would drop every cached relation and dangle the
`const TableAccess*` the running statement is holding — and a multi-row
UPDATE would be holding it across every later row. The fact qualifies by the
same test `catalog_cache.hpp`'s other in-place updates pass: **a root belongs
to one index and is read by nothing else**, so a global drop would be damage
for nothing. So the pointer stays valid across the write hook, and every
holder sees the new root immediately.

`desc_page_id` keeps the older, harsher arrangement — a relation's root move
*does* bump — and `InsertInner` handles it by relinking last and using only
plain ids afterwards; changing it would be a change to the clustered tree's
contract, not this feature's.

Building the list **fails shut**, on the foreign-key argument rather than the
Cabin one: an index the *compiler* cannot see costs only speed, but an index
the *write hook* cannot see is an entry never appended — and an index missing
an entry is a row lost to every later probe. Both halves read this one list.

`CREATE INDEX` and `DROP INDEX` both bump the catalog version
(`BumpVersion()`), because the cached index list is derived from
`sys.indexes`.

### 12.1 WAL

**One record type, `kIndexInsert = 17`**, carrying the entry's slot and its
bytes. The record's `page_id` names the leaf and the leaf's own header carries
the widths, so redo needs neither the index's oid nor its layout — there is no
second place for either to be wrong.

> **IX13 — `INDEX_INSERT` is logged *before* the `HEAP_INSERT` or
> `HEAP_OVERWRITE` it points at.**

The direction is forced, not stylistic. If the index record is durable and the
row's is not, redo produces a **dangling entry**, which verification drops on
sight (IX1) — harmless. The reverse produces a row with no index entry, which
is a **lost row** the moment anything probes for it. Same reasoning that puts
`kVarHeapAppend` before its `HEAP_INSERT`, arriving at the same order from the
opposite pointer direction.

> **There is no `INDEX_PAGE_INIT` record, by decision.**

A new index page cannot be described by its header the way a new heap page
is: a **dividing** split leaves the new sibling already holding half the
entries, so only a full page image describes it. So a split takes a
`kFullPageImage` per page it created or rewrote — exactly what the clustered
tree's internal nodes do, and for the identical reason (no record type
describes an entry-array division).

That gives one rule with no exceptions: **an append that split nothing logs an
`INDEX_INSERT`; an append that split logs images and no `INDEX_INSERT`.** The
images are taken after the entry is in, so emitting both would apply it twice.

The hook **reports** its writes and the dispatcher emits them, which is
`btree.cpp`'s division ("it mutates pages; it does not know about the WAL").
Collection happens only when there is a log to write to, so the unlogged path
is the code it always was. Redo of these records is `docs/spec/wal.md`'s.

### 12.2 Cross-core

> **IX14 — nothing.** An index hangs off its relation's catalog row and has
> no owner field, so `crosscore.md` M1's co-location rule is structural here
> exactly as it is for the Cabin, the var-heap and Waystone pages. A peer core
> cannot `CREATE INDEX` for the same reason it cannot `CREATE TABLE`.

---

## 12.3 The switch

`indexes` (default on) is a **read-path** switch. Off makes an index step take
the walk it would have taken had the index not existed, and **the compiled
chain is unchanged either way** — the kind is still `kIndexProbe` and `ANALYZE`
still says so. That is the same arrangement `cabins` has, and it keeps the plan
`f(shape, catalog)`: the switch steers the branch inside a kind, never which
kind was compiled. It also makes the A/B comparison sharper than a compiler
switch would — identical plan, identical rows, different work.

> **There is deliberately no switch for index maintenance.**

An index that stops being maintained is *wrong* rather than slow, and a config
key that can produce a wrong answer is not a config key. Turning the write cost
off is a catalog act: `DROP INDEX`.

---

## 13. Open decisions this spec deliberately does not settle

The decisions this spec leaves unsettled — `kIndexStringKeyBytes`, the leaf
split point and fill factor, the column caps, reclamation of superseded
entries, an index-only scan, `UNIQUE`, descending/partial/expression
indexes, index cost as a `CABIN AUTO` input, and per-range against global
indexes under range ownership — are unrecorded here; the refusals that stand
in their place are §11's.
