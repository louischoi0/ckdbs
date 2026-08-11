# Workplan — general `ORDER BY`

Owner doc for the output sort. Amends `docs/parser-v2.md` I11 and I12, and
supersedes the `ORDER BY` half of `docs/parser-v2-workplan.md` V09. Built
2026-08-11 in the `feat-order-by` worktree.

## What it is

`ORDER BY <col> [ASC|DESC] [, <col> [ASC|DESC]]...` on any column or
columns of any relation in a non-aggregated top-level statement — pk or
not, driving relation or joined, each key with its own direction. Up to
`kMaxSortKeys` (8) keys.

Before this, `ORDER BY` was one accepted form and six refusals: the driving
relation's pk, ascending, one key. Every refusal message in the tree ended
with the same clause — *"needs an output sort this engine does not have"*.
This is that sort.

## What stays refused, and why each is a decision rather than a gap

| Form | Answer | Why |
|---|---|---|
| `ORDER BY 1` | `Unsupported` + byte | An ordinal names a select-list position, which is a second spelling of a thing that already has one. Nothing about a sort existing changes that. |
| `ORDER BY count(x)` | `Unsupported` + byte | The grammar has no expressions here, AG9's rule one clause over. |
| A ninth key | `Unsupported` + byte of the ninth | A cap refuses; it never sorts by a prefix of what was written. |
| Over aggregated output | `Unsupported` + byte | **Deliberately left open.** `feat-aggregate.md` §10 pairs this with `HAVING` as post-fold consumers to be decided together; answering half of it here would settle that decision by accident. |
| Inside a subquery | `Unsupported` + byte | A subquery's rows feed a predicate. No order of them is observable. |
| `NULL` in a sort key | `Corruption` | No NULL is storable, and whether one sorts first or last is the open decision `EncodeIndexKeyColumn` already declines to prejudge. Refusing keeps it open. |

## OB1 — the parser

`src/parser/parser.cpp` `ParsePaginationTail`, `include/kds/parser/ast.hpp`.

`std::optional<ColumnName> order_by` became `std::vector<SortKey>`, where a
`SortKey` is a `ColumnName` and a `descending` bit. The clause reads shape
only: which relation a name belongs to, and whether the column exists, are
catalog questions answered in OB3.

**`DESC` reaches the AST now.** It used to be refused on the ground that
"every chain links forward only" — true of the executor it was written
for, where the only thing that could serve an order was a walk and no walk
runs backwards. An output sort does not walk; it orders what the walk
emitted. The reason is gone rather than outvoted, which is the standard
I11 set when it kept the word in the grammar.

**Nothing was reserved and `kFingerprintVersion` did not move.** `ORDER`,
`BY`, `ASC`, `DESC`, `LIMIT`, `OFFSET` are still ordinary identifiers
matched by text at clause position, and `fingerprint.cpp` has no
clause-aware code at all. Three corpus lines were affected and **the two
that flipped kept both hashes** — `parser_corpus.txt` `ORDER BY id DESC`
and `ORDER BY id, name` went `Unsupported` → `ok` with `pattern_id` and
`arg_hash` untouched, the V08/V09 precedent.

One consequence worth stating because it differs from `LIMIT`: a sort
column is an **identifier**, so it is part of the pattern shape, not a
slot. `ORDER BY a` and `ORDER BY b` are different patterns, where
`LIMIT 10` and `LIMIT 20` share one.

## OB2 — one ordering, in one place

`include/kds/exec/row_codec.hpp`, `src/exec/row_codec.cpp`.

`OrderKeyOf(type_val, value)` normalizes a decoded value to an `OrderKey`:
an `Int128 num` for every numeric type, a `std::string str` for text.
`OrderKey::Compare` is then one branch and no type dispatch.

**Why a normalized key rather than a three-way comparator.** A sort
compares each row O(log n) times and decodes it once, so the type dispatch
belongs on the once side. Normalizing also removes the trap that makes
`CompareValues` unusable as a sort predicate: it answers "no match" to
anything it cannot compare, so `<` and `>` can both be false — not a strict
weak ordering, and `std::sort` handed one may read off the end of its
range.

`Int128` holds an int64, a uint64 and a wide decimal's unscaled value
exactly, so one field covers every numeric arm. DATE and TIMESTAMP need no
case of their own, for TY5's reason: an ordering on the encoded integer *is*
the ordering on the value.

## OB3 — the compiler, and the elision

`src/exec/step_compiler.cpp` §5a and `ReadColumnsOf`,
`include/kds/exec/step_chain.hpp`.

Keys resolve through the existing `ResolveColumn`, and land on
`StepChain::sort_keys` as a `ColumnRef`, a `type_val` and a direction. The
header paragraph claiming "`ORDER BY` deliberately does not survive
compilation … a field here would be a fact nothing may read" is retired
in place, with the reason it stopped being true.

**The elision.** One ascending key on the driving relation's pk compiles to
*no sort at all*. That is exactly V09's accepted form and V09's argument
still holds for it: it names the order the chain already emits. Dropping
the keys rather than sorting-and-noticing-it-was-already-sorted is what
keeps that path at literally zero cost.

The premise is that step 0 emits in pk order. A walk or range does
(page-wise `min_key`, which a division preserves); an index step does,
because IX8a sorts its pks back into that order deliberately; a lookup or
probe emits one row. **A Cabin probe does not** — its entry set is
insertion-ordered — so `kCabinProbe` is excluded by name. That exclusion is
a fix and not a precaution: the discarding version of this clause answered
`ORDER BY <pk>` over a Cabin-probed relation with whatever order the entry
set happened to hold.

**`read_columns` is the one field a sorted chain does not share with its
unsorted twin**, because a key must be decoded to be compared. Keys are
filed against their *owning* step, so a join ordered by the inner
relation's column decodes it on the inner step. Steps, kinds, keys,
residuals and class are unchanged — which is what the identity test
asserts, and AP01's pass has the same shape.

## OB4 — `exec::OutputSort`, at the AG1 seam

`include/kds/exec/sort.hpp`, `src/exec/sort.cpp`,
`src/server/command_dispatcher.cpp`.

A sink decorator, the third thing to take AG1's seam after the fold and the
quota and for their reason: a sort consumes rows and has no opinion about
where they came from. The step VM is untouched, so trail recording, Cabin
probes, access statistics and probe equivalence hold for a sorted statement
with no new proof — all of them happen inside `AcceptTupleAt`, upstream of
the sink.

That split is also the answer to "does sorting change what Waystone
records": no. A trail records **execution** order and the client sees
**emission** order, and those were already two different objects — `LIMIT`
proved it first. The sort widens the gap without moving either definition.
Pinned by `waystone_contract_test.cpp`, which now compares sorted replies
across all five recording/replay configurations, `DESC` and multi-key
included.

**The quota moved downstream of the sort.** Rows [m, m+n) of the *sorted*
reply are not rows [m, m+n) of the emitted one, so a sorted statement
cannot stop its walk when the quota fills. Two consequences, both stated
rather than discovered:

- **A sorted statement's `LIMIT` no longer bounds work.** The row-touch
  budget still does, which is the division `pagination.hpp` already draws:
  the quota bounds output, the budget bounds work.
- ANALYZE's `examined=` and `pages=` for a sorted statement are the
  unlimited statement's, however small the `LIMIT`.

**Stability comes from the comparator, not the algorithm.** Arrival order
(`seq`) is the last sort key and always ascending. That makes the order
**total** — so `std::sort` and OB5's heap are safe without proving the
client's keys unique — and makes ties resolve to the order the chain
emitted them in. So `ORDER BY` **refines** I12's emission contract rather
than replacing it: rows the clause does not distinguish come back in the
order they would have had without it.

**Memory.** Rows are buffered as rendered text plus their normalized keys.
This is not a new class of memory: the dispatcher already renders every row
of a reply into one stream and answers in a single write, so a full result
set was always resident. What the sort adds is the keys and the per-row
split of a buffer that already existed.

`sort_max_rows` (`[PROPOSED]` 1048576) refuses when a sort would hold more
rows than that. It never truncates and never spills — a sort has no
fallback to decline to, the way a Cabin does, so it belongs on the
aggregate side of that line.

## OB5 — top-N: the quota bounds memory

With a `LIMIT`, only the first `offset + limit` rows of the sorted order can
reach the client, so the buffer keeps a bounded max-heap of that many and
discards the rest as they arrive. `ORDER BY ... LIMIT 20` over a large
relation holds twenty rows, not the relation — measured through ANALYZE's
`sorted=` against its `examined=`.

The addition saturates rather than wrapping: both halves are client-supplied
uint64s, and a wrap would turn a huge `OFFSET` into a tiny buffer that
silently drops rows. The target is clamped to the cap rather than refused
against it — a `LIMIT` larger than `sort_max_rows` is not itself an error,
and a statement returning ten rows must succeed however large its limit was
written. The cap still fires if that many rows actually arrive.

## OB6 — observability

- `FormatPlan` prints `sort <up>:<slot>.<col> asc|desc, ...` between
  `project` and `quota` — execution order, the placement rule that
  comment already fixes. **Absent when the clause was elided**, and seeing
  that absence is most of why the line is worth printing.
- ANALYZE reports `sorted=N` beside `groups=`: the rows the sort *held*,
  which under a `LIMIT` is the retained count and not the count that
  arrived. It runs the sort for the same reason it runs the fold and the
  quota — the run it describes must be the run that happened.

## Finding: an index cannot serve the order, and this is not a gap

Recorded here rather than left as an omission, because the obvious
optimization is wrong for four independent reasons.

`RunIndexStep` walks its leaf chain in key order and then **sorts its
matched pks back into pk order** (`src/exec/step_vm.cpp`). That `std::sort`
is a correctness property, not locality: IX8a — creating an index must not
reorder a reply, because invariant 8's standard is one an authoritative
structure does not get to fall below.

Three more things would have to be true and none is:

1. **Index maintenance is append-only** (IX2), so an updated row has entries
   at both keys. The dedup keeps the *first* entry encountered — the lowest
   key across versions, not the visible version's. Today that only decides
   which entry survives and the pk sort erases the difference; under an
   ordered emission it would decide the row's output position, and a bare
   `ORDER BY col` has no residual on `col` to catch it.
2. **A string key is truncated** to `kIndexStringKeyBytes` (32) and
   zero-padded, so index order is a *prefix* order and not a total order on
   values. Two strings sharing a 32-byte prefix encode identically. IX6's
   "truncation can never cost correctness" is an argument about a
   superset-plus-recheck read path and does not carry over to serving an
   order. Pinned by `StringsSharingALongPrefixStillOrder`.
3. **There is no cardinality estimate.** Choosing an index for its order
   rather than its selectivity walks straight into IX9's measured
   crossover — a range at 200 rows is 11% *slower* with the index — with
   nothing available to see it coming.

`docs/feat-index.md` §11's line "There is no output sort at all yet; an
index that could feed one is not the missing half" is now half false and
half more true than when it was written: the sort exists, and the index
still is not the missing half.

## Cross-core

The sort is a **session-core sink**. No ordered merge across cores is
introduced, deliberately: `workplan-crosscore.md` forbids creating a
cross-stream ordering dependency, and `crosscore.md` §7's
cancel-at-the-`LIMIT`-th-framed-row trick is streaming early termination
that a `LIMIT`-aware ordered merge could not offer, since no producer could
know it was done. A blocking sink-side sort consumes whatever arrives and
orders locally, which leaves that door shut rather than painted over — and
leaves §7's trick intact on the elided pk path, which is the path that has
it.

## Open

- `sort_max_rows`'s default, and whether the cap should count bytes rather
  than rows. Rows follow `aggregate_max_groups`'s precedent: an entry count
  is the number an operator can reason about against a row width they
  already know. A relation of wide varchars makes that less true than it is
  for groups.
- `kMaxSortKeys = 8`.
- Where a NULL sorts, when NULLs become storable. Joined to
  `EncodeIndexKeyColumn`'s identical open decision — they must be answered
  together or the index and the sort will disagree.
- `ORDER BY` over aggregated output, and `HAVING` beside it —
  `feat-aggregate.md` §10, untouched by this work.
- Whether the top-N heap should also bound the *unlimited* case by spilling.
  It refuses today. Spilling is a different feature and needs a temp-file
  story this engine does not have.
