# Workplan: secondary indexes

Spec: `docs/feat-index.md` (decisions `IX1`-`IX14`).
Tasks `IX01`-`IX16`, in five milestones. **IX01-IX14 are built** (IX-M1 to
IX-M4 in full, plus the switch and the benchmark); IX15 and IX16 remain.

Read `feat-index.md` §1 before touching anything on the write path: the
superset invariant is what makes every maintenance action an append, and §2's
"nothing removes an entry" is a correctness statement, not an optimization
note.

---

## Where to pick this up

**At `IX15`** — the documentation sweep. IX01-IX14 are built as of 2026-08-08,
the whole suite is green at **1,694 tests**, and the feature is measured in
`bench/results-index.md`.

The headline: **9.7× on a selective equality over 10,000 rows**, 1.9× over
1,000, 1.11× over 200 — and an **11% loss** on a range at 200 rows, which is
the crossover IX9's `f(shape, catalog)` rule deliberately cannot see. §7's
covering claim survived in both directions, and the *absence* of an index-only
scan was measured rather than assumed.

`tests/index_contract_test.cpp` is what keeps the feature honest, and it was
**mutation-tested rather than assumed**: deleting IX8a's pk-order sort fails
two of its tests, and letting the covered filter decide a spilled value fails
a third. The third case existed only because the first mutation run showed the
suite could not see it - a suite that passes against a broken engine is not a
suite.

**The feature works end to end.** A probe over a 60-row relation examines 10;
a `BETWEEN` over 100 rows examines 10 and stops; a `COVERING` clause drops 30
of 40 descents before touching the base relation. `ANALYZE` reports all three
as `index_scanned` / `index_filtered` / `index_resolved`.

The finding that mattered, now spec **IX8a**: **an index step must emit in
primary-key order.** The walk collects pks in *index key* order and a scan
emits them in pk order, so without a sort between the two phases, creating an
index reorders a reply. "An accelerator may cost performance and must never
change a query result" is invariant 8's standard for Waystone, and an
authoritative structure does not get to fall below it. The byte-for-byte
equivalence test is what caught it.

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

### IX10 — `kIndexProbe` / `kIndexRange` in the compiler — **built**

`AccessKind` gained both, placed after `kCabinProbe` and before
`kFilterScan` - the accelerated non-pk kinds together. The persisted numbers
(7 and 8) come from `StoredAccessKind`'s explicit mapping, which is why a kind
could be inserted where it reads best without re-labelling any
`sys.access_stats` row already on disk.

Selection is `IndexProbeOf`: longest usable key prefix, ties broken by lowest
`index_oid` - free, because `TableAccess::indexes` is sorted by it and the
scan keeps the first index to reach a score. Tried **after** the pk kinds and
**before** the Cabin, each for a stated reason.

`IsTrailReplayable` did not move, and `cabin_contract_test.cpp` now asserts
both new kinds against it - an index is authoritative, like a Cabin, and
invariant 9's line is lookup versus search.

Three things worth knowing.

**The plan's instruction about `HasUnindexedEqualityFilter` was backwards.**
It said to widen it to a multi-column check. `FindIndexOnColumn` had already
been widened by IX03; what the function needed was to stop asking the catalog
at all - it was a `sys.indexes` scan per equality per compile - and read the
cached `index_mask` IX04 put on the relation.

**The bounds are encoded at compile time**, not per row: coercion is a
compile-time act and so is the encoding that follows it. A value the encoder
refuses declines the index rather than failing the statement, and the walk
returns identical rows.

**`waystone_contract_test.cpp` gained an index in its shared `Load`**, so all
five configurations cover indexed statements. That moved one existing test:
`ANonPkPredicateStillSearchesAndSaysSo` asserted the literal word "Scan", and
`b.v` now compiles to an IndexProbe - which is authoritative, faster, and
still a *search*. It reads the trust line off `IsTrailReplayable` now, which
is what keeps it a test about invariant 9 rather than about whichever
accelerator exists this month; its old form survives as
`AnUnindexedNonPkPredicateStillScans` over the un-indexed heap relation.

### IX11 — `RunIndexStep` in the step VM — **built**

Two phases, as `ServeFromCabin` is and for a related reason: phase 1 walks the
index between the compiler's bounds and collects pks; phase 2 resolves each
through the clustered tree and emits. `AcceptTupleAt` descends into the next
step and anything below it may fetch, so an index-leaf span held across
emission is exactly the span I15's R1 forbids.

It does neither of the two things the plan warned about. It does not decide
visibility - every located row goes through `AcceptTupleAt` like any other
kind - and it does not emit from an entry, because there is no visibility
witness outside the tuple.

**Spec IX8a came out of this task**: the phase-1 pks are **sorted** before
resolution, because the walk collects them in index-key order while a scan
emits in pk order, and an index that reorders a reply fails the standard
invariant 8 sets. It buys leaf locality too.

Three smaller things. A **dropped or redefined** index between compile and
execution takes the walk - the chain is stale, not wrong. The **live** root
is read off the cached `IndexRef` rather than the compiled copy, since a
split republishes it in place. And `DecodeOneValueInto` moved out of
`row_codec.cpp`'s anonymous namespace and into the header: an entry carries
covered columns concatenated in the index's order rather than at the row
layout's offsets, and re-deriving what a cell means would have been a second
decoder for the same bytes.

The covered-column filter is **conservative by construction** - a predicate
it cannot decide keeps the row, including a spilled covered value, because
resolving one would be a page fetch under the leaf's span. Wrong in that
direction costs a wasted descent; wrong in the other costs a row.

### IX12 — Equivalence tests — **built**

`tests/index_contract_test.cpp`, modelled on `waystone_contract_test.cpp` and
holding an index to a **higher** bar than that file holds a trail to. A trail
is advisory, so invariant 8 lets it be deleted wholesale; an index is
authoritative and cannot be. What it *can* be held to is the other half of the
same standard - an accelerator may cost performance and must never change a
query result - and that is what this suite asserts.

Four configurations, one query set, replies compared byte for byte:

  1. indexed, filled by the **write hook**   the ordinary one
  2. indexed, filled by the **backfill**     declared after the rows existed
  3. `indexes = off`                          the index exists and is ignored
  4. no index at all                          the baseline

Configuration 2 is what says the backfill and the write hook agree about what
an entry is; 3 and 4 together say the index changed neither plan nor answer.
`TheIndexedRunActuallyUsedTheIndex` is the control every equivalence suite
needs, without which all four could agree because none of them used one.

Five cases the shared set cannot express get their own tests: an old snapshot
reading its version through a superseded entry, a delete whose entry survives
and still returns nothing, two string keys sharing a truncated prefix, a
**spilled covered value** that the entry-side filter must not decide, and a
deliberately corrupted index page - which must **fail** rather than
mis-answer, the opposite outcome from the waystone suite's corrupted trail,
because an index has no fall-through that could be correct.

**It was mutation-tested, and that is why the fifth case exists.** Removing
IX8a's pk-order sort fails two tests. Letting the covered filter decide a
spilled value failed *nothing* on the first run - the suite had no case where
a covered column spilled - so the case was written and the mutation now fails
it. A suite that passes against a broken engine is not a suite.

---

## Milestone IX-M5 — measurement and documentation

### IX13 — Switches — **built**

`indexes`, default on, a **read-path** switch: off makes an index step take
the walk it would have taken had the index not existed.

**It does not change the compiled chain**, and that is the whole design. The
kind stays `kIndexProbe` and ANALYZE still says so; the switch steers the
branch inside `RunIndexStep`, exactly as `cabins` steers the branch inside
`RunCabinStep`. That keeps the plan `f(shape, catalog)` - and it makes the A/B
comparison sharper than a compiler switch would, because it holds the plan
fixed and varies only the work.

Maintenance is **not** switchable, as planned: an index that stops being
maintained is wrong rather than slow, and turning the write cost off is a
catalog act (`DROP INDEX`).

`IndexSwitchTest` is the pair the switch exists for - one instance on, one
off, replies compared byte for byte over probes, ranges, covering, an updated
key, a deleted row, an aggregate and a miss.

### IX14 — Benchmark — **built**

`bench/results-index.md`, from `tools/index_benchmark.py` and its PostgreSQL
twin `tools/pg_index_benchmark.py` (which imports its schema, row generator
and shape list from the ckdbs driver, so the two cannot drift into measuring
different questions). Release build, NVMe, data files under `$HOME`, twelve
runs, `--verify` comparing every indexed reply against the unindexed walk row
for row and in order.

What it settled, beyond the headline:

- **A base descent costs 0.58-0.76 µs**, rising with relation size. That makes
  `index_filtered × ~0.7 µs` the price of a COVERING clause, reported by
  `ANALYZE` before anyone commits to the write cost.
- **The marginal index costs half the first one** - +4.5 µs for the first,
  +2.5 for the second, flat across a 50× row range at `relaxed` durability.
  Two hypotheses fit (a per-index difference, or a fixed cost of entering the
  hook) and this run separates neither; the document says so.
- **IX2's rule is confirmed by a counter, not a latency.** After 10,000
  inserts and 600 updates, the index on the un-updated column holds exactly
  10,000 entries. A violation would cost ~2.5 µs - inside the harness floor -
  so latency could never have caught it.
- **`indexes = off` is a faithful proxy for no index** (+0.4% at the small
  sizes, straddling zero at 10K), which is what licenses the contract suite's
  A/B.

Two things worth carrying forward. **The write cost is 0.9% of a default
INSERT only because a batch of one is a batch**: the run is single-connection,
so the fsync does not amortize, and under concurrency 4.5 µs becomes visible.
That measurement does not exist. And **PostgreSQL never chose a plain
`Index Scan`** on these shapes - it preferred `Bitmap Heap Scan`, which sorts
tuple ids before heap access. That is the same reordering IX8a mandates here,
arrived at independently for a locality reason where this engine needs it for
a correctness one.

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
