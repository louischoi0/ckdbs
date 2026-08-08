# Workplan: secondary indexes

Spec: `docs/feat-index.md` (decisions `IX1`-`IX14`).
Tasks `IX01`-`IX16`, in five milestones. **IX01-IX09 are built** (IX-M1,
IX-M2 and IX-M3 in full); nothing else is.

Read `feat-index.md` §1 before touching anything on the write path: the
superset invariant is what makes every maintenance action an append, and §2's
"nothing removes an entry" is a correctness statement, not an optimization
note.

---

## Where to pick this up

**At `IX10`** — the read path, and the first task that makes the feature
visible to a query. IX01-IX09 are built as of 2026-08-08 and the whole suite is
green at **1,665 tests**. An index is declared, **built over whatever the
relation already holds**, maintained on every write and logged; **no statement
can use one**.

What building IX06 and IX09 changed, folded back into the spec:

- **The backfill runs before the catalog row, not after** (spec §10a). An
  index is then complete or absent, never partial: a failed build leaves an
  unreachable tree and no row. It also means the root written into the row is
  the one the build *ended* at, since a split moves it.
- **`Catalog::CheckIndexDef` was factored out of `CreateIndex`.** Building
  before publishing put the walk ahead of every refusal, so a heap relation
  failed as `page 129 has page_type 1, expected 2` from inside the build.
  One implementation, two callers - the DDL layer checks before it walks, and
  `CreateIndex` re-checks at the write because that is the door every other
  caller comes through.
- **`AppendIndexEntry` was extracted from `MaintainIndexes`.** The backfill
  builds a tree with no `sys.indexes` row, so it cannot go through the
  catalog-aware loop - and writing the append twice is how a backfilled entry
  and a written one come to disagree about what an entry is.
- **A test fixture without a `TransactionManager` writes no undo records**,
  so the version walk had nothing to find and the first backfill test passed
  by describing an engine that keeps no history. `IndexMaintainTest` now wires
  one; the IX06 tests gained real undo along with it and still pass.

- **IX12a is corrected, not merely qualified.** `Catalog::UpdateIndexRoot`
  updates the cached entry **in place** instead of bumping the catalog
  version. A split republishes a root from inside an ordinary INSERT, so a
  global drop would dangle the `const TableAccess*` the statement is holding
  — and a multi-row UPDATE holds one across every later row. The fact
  qualifies by the same test `catalog_cache.hpp`'s two pattern updates pass:
  a root belongs to one index and is read by nothing else. `desc_page_id`
  keeps the older, harsher arrangement, because changing it is a change to
  the clustered tree's contract rather than this feature's.
- **`CoerceLiteralToColumn` was not idempotent, and that was a live bug
  wider than the index.** Its date and timestamp arms have always accepted a
  value already in storage form; the two decimal arms refused one. A write
  hook re-coerces a *decoded* row — an UPDATE carries one — so a decimal
  column arrived as `kDecimal` and was rejected. **The Cabin's hook absorbs a
  coercion failure by un-observing, so a Cabin on a decimal column was
  silently destroyed by the first UPDATE that touched its relation.** Fixed
  at the one coercion site; the scale check is unchanged, so it is idempotent
  and not permissive.

What building them added to the design, each folded back into the spec:

- **The superblock bump 11 → 12 is justified, but not by the reason the spec
  gave.** The four bumps before it protected an existing file from a build
  that would misparse it; this one cannot, because nothing ever wrote a
  sys.indexes row, so a version-11 file would mount and run correctly. What
  it protects is an **older binary opening a newer file** — which finds rows
  its `Decode` rejects and fails on every SELECT compile, since
  `HasUnindexedEqualityFilter` asks sys.indexes per statement. The action did
  not change; the argument did, and the next bump should be argued rather
  than copied.
- **`FindIndexOnColumn` answers for the *leading* key column only.** "Contains"
  would stop the compiler calling a step a filter scan while leaving it
  exactly as slow — a lie to the access statistics, which is the function's
  only consumer today. `TableAccess::index_mask` follows the same rule.
- **`IndexRef::root_page_id` is the one field on `TableAccess` that can move
  without DDL** (spec IX12a). A root split republishes it and bumps the
  catalog version, so a caller holding a `const TableAccess*` across an index
  insert that grows a level is holding a **dangling pointer**. `desc_page_id`
  has had this property since btrees landed and `InsertInner` already handles
  it by relinking last; IX06 must do the same. Not caching the root would
  cost a sys.indexes scan per statement, which is what `TableAccess` exists
  to avoid.

- **The tree routes on `(key, pk)`, and a probe is zero-padded rather than
  shortened** (spec IX4b). A separator carrying only the key compares *equal*
  to a probe for a duplicated key and routes right, past the left subtree's
  entries for that same key. Those rows stay reachable by walk and become
  unreachable by descent — a bug no walk test can see, which is why
  `index_tree_test.cpp` probes as well as walks.
- **`IndexInsert` reports a byte-identical entry rather than storing it
  twice.** Nothing reclaims an index entry, and a probe resolving one pk twice
  emits its row twice. Entries differing only in covered bytes are both kept:
  that difference is information.
- **`kMaxIndexChanges` is `2 × kMaxBtreeDepth + 1`, not
  `storage::kMaxStructuralChanges`.** That constant is derived from the
  clustered tree's split, which creates one page per level and moves nothing;
  a dividing split touches two. Reusing it would silently overrun the array
  that tells the WAL what changed.
- **A layout is refused when a page cannot hold two entries** — which is
  where a large `COVERING` clause is refused, by arithmetic at declaration
  rather than by an insert failing much later.

Two facts to re-verify before writing code, because both are load-bearing and
both were read off the tree rather than measured:

1. `Catalog::InsertIndexRow` exists, is never called, and deliberately does
   not `BumpVersion()`. `IX03` makes that comment false and must correct it.
2. `HasUnindexedEqualityFilter` (`src/exec/step_compiler.cpp`) already calls
   `FindIndexOnColumn` and already declines to call an indexed column a filter
   scan. `IX10` widens it to a multi-column check; it does not invent it.

---

## Milestone IX-M1 — the page class — **built 2026-08-07**

Nothing above the storage layer changes. Ends with an index tree that can be
built, descended and walked by a test, holding keys nothing produces yet.

### IX01 — Key encoding — **built**

`include/kds/exec/index_key.hpp`, `src/exec/index_key.cpp`.
`IndexKeyColumnWidth` / `IndexKeyWidth` for the schema constant,
`EncodeIndexKeyColumn` / `EncodeIndexKey` for the bytes.

**One encoder, not two.** The plan expected a row-side and a literal-side
entry point; both would have been the same walk over the same storage values,
so there is one that takes `span<const AstValue>` and the caller supplies them
from a decoded row or from coerced literals. A second entry point is a second
place to get the coercion rule wrong, which is precisely the hazard below.

Per spec §5: one discriminator byte per column, sign-flipped big-endian for
signed integers and decimals, plain big-endian for `uint64`, zero-padded
truncated prefix for strings. `memcmp` is the only comparison.

The literal side goes through **`exec::CoerceLiteralToColumn`** and nowhere
else. `spec-types.md` §3.1 records what a second coercion path cost the Cabin
— an observed DATE value silently stopped seeing rows — and this is the same
shape of hazard with a wider blast radius.

**Enforced structurally**, not by convention: the encoder accepts only a
column's *storage form* and refuses a `kStr` against a `DATE` outright, so a
caller that skipped coercion gets an `InvalidArgument` rather than a key that
probes the wrong bucket.

Done: `tests/index_key_test.cpp` asserts `memcmp` agrees in sign with
`CompareValues` over every declarable key type, both directions, every pair —
including the width extremes (a sign flip that forgets to mask puts INT_MIN
above 0 for every width below 8), an int128 pair differing only in the high
half, and the full unsigned range through `raw_int_text`. Truncation's two
collapses are pinned as collapses: same prefix and an embedded NUL both encode
alike, and neither can invert an order.

### IX02 — Index page format and the split — **built**

`include/kds/storage/index/index_page.hpp` + `index_tree.hpp`.
`PageType::kIndexInternal = 11`, `kIndexLeaf = 12`, `kMaxAssignedPageType` → 12.

Leaf: header + sorted fixed-width entry array + `right_sibling`.
Internal: `btree_page.hpp`'s shape with the key widened to the **sort key**
(spec IX4b - `key || pk`, because a separator carrying only the key routes a
probe past a duplicated key's left half).
`static_assert` every offset and size, fixed-width integers, explicit
serialization — no bitfields anywhere near a persisted byte (invariant 6).

`IndexInsert`, `IndexSeekLeaf`, `IndexVisit`/`IndexVisitFrom`, `IndexHeight`,
`IndexLeafCount`, `IndexEntryCount`. Signatures mirror `btree.hpp`'s,
including `VisitControl` with the same meaning, so a caller can hand the same
lambda to either.

**This is where the split lives** (spec §4a): divide at the entry midpoint,
separator = the right sibling's first key, fill target a parameter and not a
constant. State in the file header why dividing here decides nothing about
the heap page split policy: an index page holds entries, has no `min_key`,
and contains no tuple, so invariants 2 and 3 have nothing to be about.

Done: `tests/index_tree_test.cpp`. 40,000 shuffled keys over a narrow key
exercise the leaf split; a **wide key** (1,000 bytes, 8 entries per page)
reaches height 4+ on 3,000 entries, which is what actually exercises the
internal-node split and the root growth above it - a 678-entry fan-out needs
~170,000 rows to get there, so the narrow test alone would have left the
push-up path untested. Every test probes as well as walks, because a
separator pointing one page off survives a walk. Plus: 2,000 duplicates of one
key spanning three leaves, a byte-identical entry deduplicated, two entries
differing only in covered bytes both kept, a corrupted entry count, a width
disagreeing with the index, and a clustered-tree page handed to the index
descent - each `Corruption` rather than a reinterpretation.

---

## Milestone IX-M2 — the catalog half

Ends with `CREATE INDEX` / `DROP INDEX` / `SHOW INDEXES` declaring an index
that no statement uses and no write maintains.

### IX03 — `SysIndexRow` and the format bump — **built**

The row rewritten per spec §12 (116 bytes, both column arrays packed at their
declared stride, a count past its array refused as `Corruption`).
`kSuperBlockVersion` 11 → 12, with the mount-time check naming both values -
**every pre-existing data file stops mounting**, the fifth such break, and the
one whose *reason* differs from its predecessors (see above).

`Catalog::CreateIndex` / `DropIndex` / `UpdateIndexRoot` / `ListIndexes` /
`FindIndexesForTable` / `FindIndexByName` / `FindIndexOnColumn`. All three
mutators go through `BumpVersion()`, and `InsertIndexRow` - whose comment
said "no version bump: nothing cached is derived from sys.indexes" - is gone,
since IX04 makes that false.

`CreateIndex` refuses, each naming the reason: a heap-clustered relation
(IX3), the primary key, a column the relation has not got, a repeated column,
an empty key, an over-cap key or covered list, `UNIQUE` (IX11), and a name
already in use. It does **not** build the tree or check key column types -
the root page is allocated and formatted by its caller, which keeps
`catalog/` free of the index page format.

### IX04 — `TableAccess::index_mask` and `IndexRef` — **built**

Built at `InitTableAccess()` in one `sys.indexes` scan, same pattern as
`cabin_mask` / `cabin_ids`. Carries root page id, key columns, covered
columns, `key_width`, `entry_width`; sorted by `index_oid` so §9's tie-break
is a property of the list; mask over **leading** key columns only.

**The plan's premise about the root was wrong, and the correction is the
task's finding.** It read "root page id cannot change without DDL - it is
allocated eagerly at CREATE INDEX and never moved by growth, the same
argument that lets the var-heap root be cached." The var-heap root really is
immutable; an index root is not - a split republishes it through
`UpdateIndexRoot`, which is reached from an ordinary INSERT. It is cached
anyway, because the bump that publishes it also drops the entry, and because
the alternative costs a catalog scan per statement. What that buys has a
price named in spec IX12a: **a `const TableAccess*` held across an index
insert that grows a level is dangling.** `desc_page_id` has had exactly this
property since btrees landed, and `InsertInner` handles it by relinking last;
IX06 does the same.

Building the list **fails shut** - the foreign-key argument, not the Cabin
one: an index the compiler cannot see costs speed, but an index the write
hook cannot see is a row lost to every later probe.

### IX05 — Grammar and DDL — **built**

`CREATE INDEX <name> ON <table> (...) [COVERING (...)]`, `DROP INDEX <name>`,
`SHOW INDEXES`, plus `exec/index_ddl.hpp` under them - which is where the
widths are computed and the root page formatted, because `catalog/` may know
neither the key encoding nor the page format.

**Done, and the corpus is the evidence: 26 lines added, 0 modified, and
`kFingerprintVersion` still 1.** `index`, `covering` and `unique` reach the
lexer as ordinary identifiers, exactly as the aggregate function names did,
so a column may still be named any of them - which the corpus now pins with
`SELECT index FROM t WHERE covering = 1`.

Refusals, each at the earliest layer that can name the byte. The parser
answers for `UNIQUE` (at that word's own offset) and for the over-cap column
lists; the catalog answers for the heap relation, the pk, the absent or
repeated column and the duplicate name, **passed through unrestated** so
there is one answer to "why not". The cap check exists in both, and that is
not duplication: only the parser knows where the column was written, and
`Catalog::CreateIndex` is the door every non-parser caller comes through.

Two decisions inside it. The **root page is allocated and formatted before
the catalog row that names it**, so a row can never point at a page that does
not exist - the reverse leak, an unreachable page, is the bargain every
allocation in this engine already strikes. And an index on a **cabined
column warns without refusing**: an index is complete where a Cabin is
authoritative only for observed values, so the Cabin becomes dead weight -
but dropping it is the operator's call.

---

## Milestone IX-M3 — maintenance

Ends with an index that is complete and that nothing reads.

### IX06 — The write hook — **built**

`include/kds/exec/index_maintain.hpp`, one implementation, called from
`InsertInner` and `UpdateInner` in `command_dispatcher.cpp`, beside the Cabin
witness and **before the log** — a Cabin that missed an append can be
un-observed; an index that missed one has lost a row.

`DeleteInner` deliberately does **not** call it, and says so in a comment
rather than calling it with nothing to do: maintenance there would be a
defect, not a no-op.

The touched-column rule is tested first, as planned
(`AnUpdateThatTouchesNoIndexedColumnAppendsNothing`), and its 900-row sibling
found the rule firing for real — a statement-wide `SET owner = 1` appends 899
entries over 900 rows, because one row already carried the value.

**Two ways a value reaches the hook, and both are deliberate.** Key columns
come from the statement's values, coerced through `CoerceLiteralToColumn` —
the one path from a written literal to a value the engine keys on. Covered
columns come from the **encoded tuple**, sliced at the layout's offsets, so
they are byte-identical to the page by construction, spill pointer included.

**IX07 turned out to be already satisfied**: the INSERT path holds the
literal and the UPDATE path calls `ResolveSpills` before the hook runs, so a
spilled key column's full value is in hand on both. No var-heap read was
needed on the write path.

### IX07 — Spilled key values on the write path — **built by IX06, and it
needed no code**

The premise was that a `kSpilled` key cell has no inline bytes and the hook
would have to fetch through `varheap::Fetch`. It does not: the hook takes
*values*, not cells, and both callers already hold resolved ones — INSERT has
the literal as written, and UPDATE calls `ResolveSpills` on its decoded row
well before the write. `AKeyLongerThanThePrefixIsStoredTruncatedAndStill
Maintained` covers a 200-byte value that spills.

Re-open this only if a caller appears that maintains an index from a tuple
rather than from values; the R1 warning above is the right one for it.

### IX08 — WAL records — **built**

`RecordType::kIndexInsert = 17`, `kMaxAssignedRecordType` → 17, emitted
**before** the `HEAP_INSERT` or `HEAP_OVERWRITE` it points at (spec §12.1,
with the argument).

**`kIndexPageInit` was not assigned, and the plan was wrong to reserve it.**
It assumed a new index page could be described by its header the way a new
heap page is, with the following record filling it. A dividing split does not
work that way - the new sibling leaves the operation already holding half the
entries - so only a full page image describes it, which is what the clustered
tree's internal nodes already take. A record type nothing can write is worse
than none.

That leaves one rule with no exceptions: **an append that split nothing logs
an `INDEX_INSERT`; an append that split logs images and no `INDEX_INSERT`**,
because the images are taken after the entry is in.

Two things it changed. `IndexInsert` now records **nothing** structural in the
no-split case - `changes()` is for pages no record type describes, and the
entry bytes describe a plain append completely, so a caller logs 4 bytes plus
an entry instead of an 8 KB image. And the UPDATE path's `HEAP_OVERWRITE`
moved to *after* the index hook, so the entries reaching the new version
precede it.

The hook **reports**, the dispatcher emits - `btree.cpp`'s division - and
collection happens only when `wal_` is non-null, so the unlogged path is
unchanged.

Verified by asserting the record sequence, as planned: `insert_wal_test.cpp`
checks the ordering, that the logged bytes match what landed on the page, that
a relation with no index logs nothing, and that a split takes images instead.

### IX09 — Backfill — **built**

`CREATE INDEX` on a populated relation, per spec §10a, and it runs **before**
the `sys.indexes` row exists - so an index is complete or absent, never
partial.

The test that would catch the omission was written first, as planned, and
**it failed for a reason worth recording**: the fixture had no
`TransactionManager`, so no undo record was ever written and the version walk
had nothing to find. A test that passes because the engine kept no history is
not a test of the walk.

The walk is **two phases per leaf** - copy the tuples out, drop the span, then
append - because appending fetches pages and I15's R1 forbids a fetch under a
live span. That bounds memory at one page rather than one relation.

Distinctness needs no bookkeeping: a version that did not move the key encodes
byte-identically and `IndexInsert` already deduplicates one (IX4b). A
delete-marked row is walked like any other, and a delete-mark's own undo
record carries an empty image - the version it supersedes is the one already
appended - so it is skipped.

---

## Milestone IX-M4 — the read path

Ends with the feature doing something.

### IX10 — `kIndexProbe` / `kIndexRange` in the compiler

`AccessKind` gains both. Selection per spec §9: longest usable key prefix,
tie broken by lowest `index_oid`, catalog only.

**Key equalities stay in `Step::residual`.** That is not an optimization to
revisit — it is what keeps "downgrading any step to a plain `kScan` cannot
change the result" true, and what makes the surplus subtraction of IX1 free.

`IsTrailReplayable` **must not move**, and `HasReplayableStep` with it.
Widen `HasUnindexedEqualityFilter` to ask about a multi-column index.

Done when: the contract test asserting the chain shape covers indexed and
unindexed forms, and the `waystone_contract_test.cpp` query set gains indexed
statements so all five of its configurations cover them.

### IX11 — `RunIndexStep` in the step VM

Descend, walk entries while the key prefix matches, and for each: evaluate
covered-column residuals from the entry, then `BtreeLookup` the pk, then
`AcceptTupleAt`.

Two things this must not do. It must not decide visibility itself — the
predicate lives at exactly one site and this is not it. And it must not skip
the base fetch for a surviving row, because there is no visibility witness
outside the tuple (spec §7); the covered columns are a **filter**, not a
projection source.

### IX12 — Equivalence tests

The suite that keeps this honest, modelled on `waystone_contract_test.cpp`:
the same statements over the same data, with the index used and with it
forced off, compared **byte for byte** — rows and order. Plus:

- an UPDATE that moves a key, read from both an old and a new snapshot;
- a DELETE, which must leave the entry and still return nothing;
- a truncated string key with two values sharing a prefix;
- a deliberately corrupted index page, which must fail rather than mis-answer.

---

## Milestone IX-M5 — measurement and documentation

### IX13 — Switches

`indexes` (default on) as the read-path switch, so IX12's A/B comparison has
something to flip. Maintenance is **not** switchable: an index that stops
being maintained is wrong, not slow, and a config key that can produce a wrong
answer is not a config key.

### IX14 — Benchmark

Via `bench/`'s owner. Wanted, and named here so the run is not a data dump:

- a selective secondary equality, indexed against `kFilterScan`, at 200 / 1K /
  10K rows;
- the same with and without `COVERING`, to price spec §7's claim that covering
  buys the *avoided* descents and nothing else;
- the write-path cost per index on INSERT and on a key-moving UPDATE;
- a PostgreSQL comparison on the same shapes.

Release build (`build-release`); a Debug measurement here is wrong in both
directions, which `workplan-aggregate-perf.md` established twice.

### IX15 — Documentation

`CLAUDE.md` gains a Core Architecture entry and loses the index items from
Open Decisions that this settles; §13's list moves in as the ones it does not.
`docs/client-manual.md` gains the three statements and the `indexes` key.
`docs/heap-and-tuple.md` §7's access-statistics note gains the two new kinds.

### IX16 — Access statistics

`kIndexProbe` and `kIndexRange` are recorded through the same single call with
no per-kind branch, so the numbers stay comparable across kinds. Free if IX10
is done right; listed so it is checked rather than assumed.

---

## Sequencing notes

- `IX01` and `IX02` are independent of everything above the storage layer and
  can land alone.
- `IX03`'s format bump is the disruptive one. Land it early in a session, not
  late — every existing data file dies with it, and discovering that at the
  end of a day of work is avoidable.
- `IX06` before `IX10`. An index the compiler emits and the write path does
  not maintain returns **wrong answers**, and it is the only ordering in this
  plan where the wrong sequence is dangerous rather than merely awkward.
- `IX09` (backfill) can trail `IX10` only if `CREATE INDEX` refuses a
  non-empty relation in the meantime, and that refusal must be a real refusal
  rather than an unchecked assumption.
