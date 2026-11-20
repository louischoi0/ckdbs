# Work order CV - `ORDER BY ... LIMIT` served by the order a structure already holds: the ordered index walk and the reverse walk

Status: **OPENED 2026-11-20 on the operator's standing go-ahead
(go-ahead-achieving-milestone)**, on worktree `cv-ordered-index-walk` from
`f4138860` (`v3.4.15-1-gf4138860`). Written 2026-11-15 on `main` at
`a61ab905` (`v3.4.11`) on the operator's word of 2026-11-15
(`raft-marks-2026-11-15.md` §7). **No CV-Q item is the operator's own
mark**: CV-Q0..Q6 are each adopted as CLA proposed them, and CV-Q7, raised
at CV-S0, is written into §4 with its options before it is adopted - each
**on the operator's standing go-ahead of 2026-11-20
(go-ahead-achieving-milestone)** (`raft-marks-2026-11-20.md` §3). The
`[source-read]` claims of §1 were read at `a61ab905` and read again at
`f4138860` (§7, CV-S0); where a line moved, §7 says so.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` marks an item where a wrong choice
turns a refusal into a wrong answer.

Related: `docs/spec/parser-v2.md` I11 (the sort and its elision), I12
(the tie order, CB-Q1); `docs/spec/index.md` IX1, IX4b, IX8a, IX9, §4
(the leaf and its `right_sibling`); `include/kds/exec/sort.hpp`
(`OutputSort`, the top-N heap); `instructions/v3.2.0/workorder-cu-key-set-access.md`
(the sibling order of the same survey).

CV was checked against every file name under `instructions/`, `docs/`,
`manual/` and `CLAUDE.md`, and every commit subject on the remote
branches, for a `CV-` token, a `CV<n>` token or a `workorder-cv` name at
`a61ab905`: no use found.

## 0. What CV is

### 0.1 Subject

`[operator]` Item 2 of CLA's optimizer survey of 2026-11-15: an
`ORDER BY` that a structure already holds in order is read in that order,
and a `LIMIT` stops the read, instead of reading every match and sorting
it.

`[design]` Take the OLTP "latest N" read,
`WHERE user_id = ? ORDER BY created_at DESC LIMIT 20` on an index over
`(user_id, created_at)`. Today it reads every row of the user, resolves
each one through the clustered tree, and keeps 20 in a top-N heap (§1.1).
The index already holds that user's entries in `created_at` order. CV
walks them in that order, backward for `DESC`, resolves each entry as it
goes, and stops at the twentieth row. The same applies to
`ORDER BY <tuple key> DESC LIMIT n`, which today pays a full walk and a
heap even though the clustered tree is that order reversed.

### 0.2 The shapes

- **O1 - ordered index walk.** The driving step (`rel_slot == 0`) has an
  index whose key columns are, in order: the step's equality prefix
  (zero or more columns bound by literal equalities), then the `ORDER BY`
  keys, all of one direction. A `LIMIT` is present (CV-Q2). The index
  must be chosen under IX9's rule as amended by CV-Q3.
- **O2 - reverse tuple key walk.** `ORDER BY <driving tuple key> DESC`,
  over a `kRange` or a walk of the driving step.
- **O3 - reverse ordered index walk.** O1 with `DESC` keys.

### 0.3 Not in CV

- Mixed directions (`ORDER BY a ASC, b DESC`). There are no descending
  indexes (`index.md` §11).
- An `ORDER BY` on a non-driving step, or across relations.
- An ordered walk without `LIMIT`, unless CV-Q2 says otherwise.
- `MIN`/`MAX` from an index end. It is the same mechanism, and a later
  order can take it.
- The key set of CU: a list prefix with an ordered tail would need a
  merge of `n` ordered runs, and is not built here.
- An index-only scan. IX7 stands, and every entry is still resolved.

## 1. The tree as read `[source-read]` at `a61ab905`

### 1.1 Today's order

- **Only one order is elided.** The compiler elides the sort only for one
  ascending key on the driving relation's tuple key, over a kind that
  `EmitsTupleKeyOrder` lists (`src/exec/step_compiler.cpp:2771-2796`,
  `include/kds/exec/step_chain.hpp:189-203`). Every other order is an
  `OutputSort`, bounded by `sort_max_rows`, and becomes a top-N heap under
  `LIMIT` (`parser-v2.md` I11; `include/kds/exec/sort.hpp:36`).
- **An index step does not emit in index order.** It collects tuple keys
  in index key order, then sorts them back into tuple key order before it
  resolves any (IX8a, `docs/spec/index.md:394`;
  `src/exec/step_vm.cpp:1512-1525`). IX8a gives its reason as "creating an
  index would reorder a reply".
- **That premise has since narrowed.** Since CB-Q1, emission order without
  `ORDER BY` is no contract (`parser-v2.md` I12). With `ORDER BY`, the
  contract is the sort's: "rows the clause does not distinguish come back
  in the order this contract gives them" (I12, the `ORDER BY` paragraph).
- **Phase 2 already stops on a quota.** The resolve loop breaks on
  `stopped_` (`src/exec/step_vm.cpp:1538-1540`). Phase 1, the collection,
  never stops early.

### 1.2 The structures

- **Index entries.** An index entry's sort key is the whole entry:
  `(key, tuple key, covered bytes)` (IX4b, `docs/spec/index.md:217`).
  Within one key value, entries therefore ascend by tuple key, and two
  entries for one `(key, tuple key)` are adjacent.
- **Stale entries stay.** Entries are append-only: an UPDATE that moves a
  key column leaves the old entry, and the read subtracts it (IX1).
- **A leaf links forward only.** `right_sibling` (`docs/spec/index.md:184-188`)
  and the clustered tree's `next_page_id` are the only leaf links. There
  is no backward link in either tree.
- **Index choice.** IX9 picks the longest usable equality prefix, ties to
  the lowest `index_oid`, and reads no sort key (`docs/spec/index.md:459-470`).

## 2. The mechanism `[design]`

**An ordered emission is an annotation on the driving step:**
`Step::emit_order { kTupleKey, kIndexKeyForward, kIndexKeyBackward,
kTupleKeyBackward }`. It is set by the compiler only where §0.2's shape
holds. When it is set, the chain's `sort_keys` are dropped, exactly as
today's elision drops them. Absent, every kind runs as it does today, so
IX8a stands for every statement CV does not touch.

**The ordered index run (O1, O3)** replaces IX8a's collect-sort-resolve
with a bounded leaf-at-a-time stream:

1. Seek the bound, in the walk's direction.
2. Collect the leaf's matching entries in entry order, then release the
   leaf.
3. Resolve each entry in that order. The descent re-enters `RunStep` for
   the inner steps, so a join keeps the driver's order.
4. Stop when the quota stops.
5. Otherwise seek the next leaf from the last entry seen. A split since
   the release moves entries right, never removes them (IX1), so a
   re-seek from the last entry neither skips nor repeats one.

**Two checks at each entry** `[quiet-wrong]`:
- **(i) The key check.** The row is emitted only if the **visible
  version's** key columns equal the entry's key. An entry left by an
  earlier version would otherwise emit the row out of order: a row moved
  from `created_at = 5` to `9` has an entry at 5 and one at 9, and must
  be emitted at 9 under a view that sees 9, and at 5 under a view that
  sees 5.
- **(ii) The seen set.** Each tuple key is emitted at most once, through
  a seen set over the walk. Two entries share `(key, tuple key)` when a
  covered column moved (IX4b).

Today the residual re-check makes (i) unnecessary, because the key is
bound by equality. In an ordered walk the order column is not bound, so
(i) is what keeps the order true.

**The reverse walk (O2, O3)** needs no on-disk change (CV-Q1). A backward
step from a leaf is a descent for the greatest entry below that leaf's
first entry, which is one root-to-leaf descent per leaf instead of a
sibling hop. The clustered tree's reverse walk is the same descent keyed
by tuple key.

**The tie order** under a reverse walk is descending tuple key, where a
sort over a forward emission gives ascending (CV-Q4).

**The ladder.** Under CV-Q3 (a), an index that serves the order is chosen
over one that only matches the equality prefix, and only when it covers
the whole equality prefix that IX9's choice would have used. It never
displaces a `kLookup`, a key set or a longer equality prefix. The choice
is still `f(shape, catalog)`.

**Pricing.** `EstimateChain` prices an ordered driver as
`min(matches, limit + offset)` resolutions, plus one descent per leaf for
a reverse walk. An order that moves the driver off the ordered step loses
the elision, and CC-Q10's rule ("never take an order that adds a sort
unless LIMIT bounds it") already prices that.

## 3. Rules

- **CV-R1. The reply under `ORDER BY` is the sort's reply**
  `[quiet-wrong]`. For every statement CV elides, the rows and their order
  equal what `OutputSort` over today's emission returns, except for the
  tie order CV-Q4 rules on. Pinned by an equivalence suite that runs each
  shape with `indexes = off`, which forces the sort, and compares byte for
  byte under CV-Q4's tie rule.
- **CV-R2. The key check and the seen set are both required**
  `[quiet-wrong]`. Tests:
  - a row whose order column moves forward;
  - the same row moved backward;
  - each of those read under a view older than the move;
  - a row whose covered column moves;
  - a row updated twice back to its first key.
- **CV-R3. A released leaf is re-sought, never revisited by pointer.**
  Each step to the next leaf is a seek from the last entry seen. Tested
  against a split and against a root growth on another core during the
  walk.
- **CV-R4. A yield is a page boundary.** An ordered walk yields where a
  `SELECT`'s outermost walk yields (`sched.md` §3), holding only its view,
  its borrow and its last entry, and no page.
- **CV-R5. Off the shape the cost is nil.** A step without `emit_order`
  runs no new instruction. This is measured at the close.
- **CV-R6. The spec moves with the code**: `parser-v2.md` I11 (the
  elided shapes), I12 (CV-Q4's tie rule); `index.md` IX8a (scoped to an
  unordered step), IX9 (CV-Q3), §8; `manual/sql/sql.md`. All of these move
  in the stage that lands the behaviour.
- The standing rules: each stage reviewed, the full suite per stage, the
  overhead measured once at the close in `build-release`, a stage's
  measurement inside 20 minutes under a 60 s watchdog, results named by
  `git describe --tags`, nothing pushed or merged without the operator's
  word.

## 4. Items for the operator (CV-Q), CLA's proposals

No item is the operator's own mark. Each is **adopted on the operator's
standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone)** as CLA
proposed it (the mark column); the options not taken stay open to the
operator's own mark.

| | item | options | CLA proposes | mark |
|---|---|---|---|---|
| CV-Q0 | Does CV-S0 gate the build? | (a) yes: if "latest 20 of a user with 10k rows" and `ORDER BY id DESC LIMIT 20` over 100k rows are not both faster than today's top-N, CV stops. (b) built whatever S0 shows | (a) | (a), adopted on the standing go-ahead |
| CV-Q1 | The reverse step | (a) one descent per leaf, with no on-disk change. (b) a `left_sibling` link in both trees, with a superblock bump | (a). (b) is a format change and adds a second link to keep true under every split. S0 measures (a)'s per-leaf cost, and (b) can follow if it shows | (a), adopted on the standing go-ahead |
| CV-Q2 | Without `LIMIT` | (a) not elided: an ordered walk of every match is `n` random resolutions against a walk and a sort, and IX9 refuses the estimate that would choose between them. (b) elided always | (a) | (a), adopted on the standing go-ahead |
| CV-Q3 | The index choice `[quiet-wrong]` only in its ties | (a) among indexes covering IX9's chosen equality prefix, prefer one that continues with the `ORDER BY` keys, when `LIMIT` is present; ties to the lowest `index_oid`. (b) IX9 unchanged: elide only when IX9's own choice happens to serve the order | (a). (b) leaves the common shape to index creation order | (a) as revised at CV-S0 (§7): a prefix a value list fills is no ordered candidate (§0.3); adopted on the standing go-ahead after its premise was re-read at `f4138860` |
| CV-Q4 | The tie order under a reverse walk | (a) descending tuple key, I12 amended: ties under an elided `DESC` return in the walk's order, as CB-Q1 already allows for every order not asked for. (b) ascending as today, by buffering each run of equal keys and reversing it. (c) elide `DESC` only when the order is total (the keys include the tuple key, or a unique index's every column) | (a), with the manual saying it. (b) costs an unbounded buffer for a long run of equal keys, and (c) excludes "latest N" on a non-unique timestamp, the shape CV exists for | (a), adopted on the standing go-ahead |
| CV-Q5 | `OFFSET` | (a) served by the same walk, counting skipped rows. (b) not elided under `OFFSET` | (a). The cost is what the top-N heap pays today, without the sort | (a), adopted on the standing go-ahead |
| CV-Q6 | Joins | (a) the driver's order is kept through nested inner steps, and an ordered driver is a planned block's candidate like any other. (b) a single-relation statement only | (a). The chain already nests in driver order (I12) | (a), adopted on the standing go-ahead |
| CV-Q7 | Which `ORDER BY` key types an ordered index walk serves `[quiet-wrong]` (raised at CV-S0) | (a) only a column whose index encoding is whole and order-preserving (IX5: the integers, `date`, `timestamp`, `decimal`, `decimal128`); a `char`/`varchar` order key keeps the sort. (b) every key type | (a). A string key is truncated to `kIndexStringKeyBytes` (IX6), so two values sharing the prefix sit in tuple key order, not their own: (b) would emit them out of order with no error. An equality prefix on a string column is unaffected - the residual drops the other values, and the matching ones keep the order column's order | (a), adopted on the standing go-ahead |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **CV-S0** | **The premise and the census.** Measured in `build-release`, `cores = 1`: "latest 20" for a user with 100, 10k and 100k rows on `(user_id, created_at)`; `ORDER BY id DESC LIMIT 20` and `ASC LIMIT 20 OFFSET 1000` on 100k rows; today's top-N against a spike of the reverse descent only. The census: every reader of `sort_keys` after the elision; every assumption of IX8a's order downstream of an index step (statistics, `ANALYZE`, the planner, Waystone recording); the yield and resume path of `SelectRun` | a results file under `bench/v3.4.11/`, the census in §7, CV-Q0's bar read |
| **CV-S1** | **O2, the reverse tuple key walk.** The annotation, the reverse descent, the elision of `DESC`, CV-Q4's tie rule (vacuous here: tuple keys are unique). Tests: equivalence against the sort, a split and a root growth during the walk, a yield mid-walk, `OFFSET`, a sim cell | `parser-v2.md` I11 |
| **CV-S2** | **O1, the forward ordered index walk.** The streamed run, CV-R2's two checks and their tests, CV-Q3's choice, the pricing. The index contract suite extended byte for byte | `index.md` IX8a, IX9; I12 |
| **CV-S3** | **O3, the reverse ordered index walk**, on S1's descent and S2's run; CV-Q4 in the manual | `index.md` §8; the manual |
| **CV-S4** | **The close.** The overhead A/B (CV-R5), S0's arms measured again, the known gaps, the report | the operator's word on landing |

## 6. What CV does not do

- No change to a reply without `ORDER BY`, and none to IX8a for a step
  without `emit_order`.
- No descending index, no mixed directions, no on-disk change under
  CV-Q1 (a).
- No index-only scan; every row is still resolved and checked for
  visibility.
- No statistics in the access choice.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-15

On `main` at `a61ab905`, on the operator's word. No stage is opened and
no code is written. Documents only.

### CV-S0 - the premise and the census (2026-11-20)

On worktree `cv-ordered-index-walk` at `f4138860`
(`v3.4.15-1-gf4138860`), before any CV code.

**The premise** (`bench/v3.4.15/results-cv-s0-premise-v3.4.15-1-gf4138860.md`,
`tools/cv_s0_premise.py`, `cores = 1`, `build-release`): a user's latest 20
of 10,000 rows costs 7,337.8 µs at p50 today and 101.0 µs for a statement
that reads only the 20 rows an ordered walk reads; `ORDER BY id DESC LIMIT
20` over 110,100 rows costs 38,987.6 µs against 88.3 µs. One tuple key point
descent, round trip included, is 54.7 µs at p50, an upper bound on a
re-descent per leaf. **CV-Q0's bar is met by more than an order of
magnitude on both cells, and CV proceeds.** No spike of the reverse descent
was built: the bounded statement and the point descent stand in for it, as
the results file says.

**§1 read again at `f4138860`.** Every claim holds; the lines moved:
the elision is `src/exec/step_compiler.cpp:3478-3500`, `EmitsTupleKeyOrder`
`include/kds/exec/step_chain.hpp:186-208`, IX8a's sort
`src/exec/step_vm.cpp:1650-1663`, phase 2's quota break `step_vm.cpp:1678`.
One fact §1 did not state: **a one-sided bound enters neither tree** - an
index range needs both ends on its column (`IndexChoiceOver`'s `ranged`)
and so does a tuple key range (`TupleKeyRangeOf`) - so `created_at > ?`
leaves an index step a probe of its equality prefix, and `id > ?` leaves a
walk.

**The census.**

- *Every reader of `sort_keys` after the elision*: `OutputSort::Reset`
  (`src/exec/sort.cpp:27`, empty keys arm no sort); `ANALYZE`'s `sort` line
  (`plan_printer.cpp:420`); `ReadColumnsOf` (`step_compiler.cpp:2031`), so a
  dropped key is no longer decoded for the sort - an ordered index step's
  key check reads its key columns, so CV-S2 adds them to that step's read
  set; the join planner's `EstimateChain` through `sorted()`
  (`join_order.cpp:313`), which prices an unsorted chain at `per_row *
  wanted / rows` - the ordered driver's `min(matches, limit + offset)` CV
  §2 asks for, with nothing added - and CC-Q10's `written_sorted`
  (`step_compiler.cpp:2575`), which an order that keeps the elision never
  meets, and one that loses it meets bounded, since every elided statement
  has a LIMIT (CV-Q2 (a)).
- *Every assumption of IX8a's order downstream of an index step*: the
  ascending tuple key elision (`EmitsTupleKeyOrder` lists both index kinds,
  `step_chain.hpp:195`). An ordered index step does not emit tuple key
  order, but the two elisions cannot meet: the ascending one fires only for
  a lone tuple key key, and an ordered index walk is taken only for keys
  that begin with an index column. The Cabin serve's sort into the walk's
  order (`step_vm.cpp:1863`) is the Cabin's and untouched: a Cabin arm
  keeps the sort. Access statistics key on `access_columns`, which CV does
  not move; Waystone records and replays only lookup-class steps
  (`IsTrailReplayable`), never a walk or an index step, so the driver's
  order reaches no trail.
- **The decline is where IX8a's order is assumed hardest**: `RunIndexStep`
  answers `indexes = off`, a dropped or redefined index and a refused
  encode with a walk (`step_vm.cpp:1466-1544`), which emits tuple key
  order. An elided `ORDER BY` over such a walk would be a wrong order with
  no error (`[quiet-wrong]`), and the index contract suite compares
  `indexes = off` byte for byte. **CV-S2 therefore gives an ordered index
  step an ordered decline**: the walk collects the matching rows' keys,
  sorts them into the walk's order, and resolves them in it.
- *The yield and resume path*: only `RunWalkStep` yields, at the bottom of
  its leaf loop, every `pages_per_slice` pages while it is the outermost
  walk (`step_vm.cpp:2241-2269`), and only its leaf fetch parks on a read
  that would wait (DE). An index step yields nowhere today. CV-R4's yield
  is therefore new code in the reverse walk and the ordered index run, at
  their leaf boundaries; the boundary itself is factored out of
  `RunWalkStep` into one function both call (`YieldAtSliceBoundary`).
- **A string order key is not served** (CV-Q7, raised here): IX6 truncates
  a string to `kIndexStringKeyBytes`, and two values sharing the prefix sit
  in tuple key order, not their own.

**CV-Q3's premise re-read** (`[quiet-wrong]` in its ties): IX9 still picks
the longest equality prefix, ties to the lowest `index_oid`
(`IndexProbeOf`, `step_compiler.cpp:1320`), and since CU a value list may
fill one prefix column. CV-Q3 (a) is adopted **as revised**: a prefix a
value list fills is no ordered candidate (§0.3 already keeps CU's key set
out), and the forward walk's ties are the sort's - ascending tuple key -
only when the `ORDER BY` keys are every index key column after the prefix,
so that is the shape served (an index entry sorts by its whole key, then
its tuple key, IX4b).

### CV-S1 - O2, the reverse tuple key walk (2026-11-20)

On worktree `cv-ordered-index-walk`, over `3ff0b377`.

- **The annotation**: `Step::emit_order` (`EmitOrder::kTupleKeyBackward`,
  `include/kds/exec/step_chain.hpp`), set by the compiler beside the
  ascending elision (`step_compiler.cpp`, "The reverse walk (CV O2)") for
  one descending key on the driving relation's tuple key, under a LIMIT,
  over a `kScan`, `kFilterScan` or `kRange` with no build and no key set.
  The chain's `sort_keys` are dropped there, so every candidate order the
  join planner compiles carries the elision only where its own first step
  is such a walk, and `EstimateChain` prices it as any unsorted chain under
  a LIMIT. `ANALYZE` prints `order=tuple_key_desc` on the step.
- **The walk**: `RunReverseWalkStep` (`step_vm.cpp`) reads a leaf's slots
  from the last down, passing over every key above the bound, and steps
  left with `btree::BtreeFetchLeafAtOrBelow(min_key - 1)`, which descends
  and then, under the leaf's hold, moves right while the right neighbour's
  `min_key` is not above the key (CV-R3: a seek by key, never a pointer).
  It ends at the first leaf whose `min_key` is at or below the range's
  low end (0 without a range): the leftmost leaf's `min_key` is always 0.
  It yields at the bottom of its leaf loop with no page
  held (CV-R4), through `YieldAtSliceBoundary`, the forward walk's boundary
  factored out so both call one definition.
- **CV-Q4** is vacuous here: tuple keys are unique.
- **Not given**: DE's park on a leaf read that would wait - a reverse
  walk's miss is a blocking read (`known-gaps.md`, "Ordered walks (CV)").
- **Tests**: `tests/ordered_walk_test.cpp` (CV-R1's equivalence against the
  sort's window over 75 leaves at ten windows, ranges at and past both
  ends, a filter, sparse named keys with deleted and updated rows, an empty
  and a one-leaf relation, a join, and what is not elided);
  `tests/walk_slice_rig_test.cpp`'s two reverse cells (a point read
  answered inside the walk's yield; splits and a root growth inside its
  suspensions, the reply checked in order). The sim's ordered read now
  takes `ORDER BY id DESC` for one limit in three, and `ORDER BY v DESC,
  id DESC` for another, from the limit it already drew, so no seed's
  stream moves (`sim/workload.cpp`, `sim/loop.cpp`).
- **Specs moved** (CV-R6): `parser-v2.md` I11 and its cost table;
  `manual/sql/sql.md`'s ordering notes; `known-gaps.md`.
- Overhead not measured; measured at the milestone's close.
- **Review** (`critics-developer`, CV-S0 and CV-S1): no correctness defect
  in S1 - splits, an append split's link order, a compaction and a root
  growth on another core each checked against the walk, the slice
  boundary's factoring behaviour-identical. **Applied**: the S0 results
  file's noise-floor and CPU sentences corrected to the archive's numbers;
  the reverse walk's four ends reduced to one (`min_key <= low`); the two
  tuple key elisions merged and a dead key-set test dropped; five cells
  added - updates and deletes inside a reverse walk's suspensions, purged
  slots, a cancel, a join the planner reorders, a key set; `known-gaps.md`
  given its commit, the stale-root cost and the unseeded one-sided bound.
  **Not applied**: seeding the reverse walk from a one-sided bound (keyset
  pagination) - a new access rule CV did not state, recorded as a gap;
  saving the descent's second fetch of the leaf - left to CV-S4's
  measurement; counting the move-right hops in `pages_fetched` - the hops
  are rare and the meter's unit is the leaf read.
- **Suite**: 0 failed of 4,880 (`ctest -LE bs-nightly`) on S1's tree.
