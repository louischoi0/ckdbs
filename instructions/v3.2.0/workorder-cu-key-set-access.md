# Work order CU - a value list as a key: `IN (...)` and an `OR` of equalities served by one descent per value

Status: **WRITTEN 2026-11-15, not opened.** Written on `main` at
`a61ab905` (`v3.4.11`) on the operator's word of 2026-11-15
(`raft-marks-2026-11-15.md` §6). No stage is started. CU-S0 waits for the
operator's word and for the marks on §4's items. Every `[source-read]`
claim below was read at `a61ab905`.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` marks an item where a wrong choice
turns a refusal into a wrong answer.

Related: `docs/spec/parser-v2.md` I10 (BU-R5, the rule CU amends), I11
(the sort elision), I12; `docs/spec/index.md` IX1, IX8a, IX9;
`docs/spec/cabin.md`; `docs/spec/plan-stats.md`;
`instructions/v3.0.0/workorder-bu-basic-query-surface.md` §0.2 (the keyed
disjunction left for "a later order", whose answers BU's tests pin);
`instructions/v3.2.0/workorder-cv-ordered-index-walk.md` (the sibling
order of the same survey).

**The letter.** CT is passed over: `docs/spec/catalog.md` numbers its
rules CT1..CT7, and a work order named CT would make `CT-S1` and `CT5`
read as one scheme. CU was checked against every file name under
`instructions/`, `docs/`, `manual/` and `CLAUDE.md`, and every commit
subject on the remote branches, for a `CU-` token, a `CU<n>` token or a
`workorder-cu` name at `a61ab905`: no use found.

## 0. What CU is

### 0.1 Subject

`[operator]` Item 1 of CLA's optimizer survey of 2026-11-15: a value list
on a column that has an access path is served by that path, one descent
per value, rather than by a walk of the relation.

`[design]` Today `WHERE id IN (1, 7, 42)` walks the whole relation, and so
does `WHERE id = 1 OR id = 7` (§1.1). The tuple key, a secondary index's
leading column and a Cabin's column each have a descent for one value.
CU gives each of them a **key set**: the list's literals coerced to the
column's type, with NULL and unencodable items dropped, deduplicated,
sorted, and served by the single-value descent once per value. The
conjunct stays in the step's residual, so the proof that "a step
downgraded to a plain walk returns the same rows" holds unchanged
(BU-R5's premise). What CU amends is BU-R5's "never a key" for exactly
one shape.

### 0.2 The shape

One top-level conjunct that is a `kOr` of two or more `kCompare` leaves,
every leaf `col = <literal>` on **one and the same column** of **this
step's** relation. This is what `ParseValueList` builds for `col IN (...)`
(`src/parser/parser.cpp:1588-1611`), and also what a written
`col = a OR col = b` lowers to. Anything else stays a residual and keeps
today's walk:
- an `OR` across columns (an index union);
- an `OR` under `NOT` or nested under an `AND` inside the tree;
- a leaf that is not `=`;
- a column from another step, which would be a correlated list;
- `NOT IN`;
- `IN (SELECT ...)`, which is §2's sub-chain.

### 0.3 Not in CU

- **An index union** (`a = 1 OR b = 2`), or any keyed `OR` of two
  columns.
- **Propagating a key set across a join's equality class**
  (`o.user_id = u.id AND u.id IN (...)` keying `o`). CB-S1 does this for
  ranges, and CU-Q7 leaves it to a later order.
- **A multi-column list**, `(a, b) IN ((1, 2), ...)`. The grammar does not
  read it.
- **A cost-based choice between a key set and a walk.** IX9 stays
  `f(shape, catalog)` (CU-Q3).
- **The query layer.** A layer walk carries a term only when the native
  `WHERE` grammar reads it (`parser-v2.md` I19), so a list the native
  parse reads reaches a native step. CU changes nothing above that.

## 1. The tree as read `[source-read]` at `a61ab905`

### 1.1 A value list is a residual, never a key

- `parser-v2.md` I10: a multi-item `IN` is a `kBoolTree` conjunct and
  "never a key". No tuple key bound, index range, Cabin probe, join key,
  build key, write lock window, point-write target or PURGE window is
  read off it (BU-R5). The tree is capped at 64 levels and 4,096
  conditions (BU-Q2).
- `ParseValueList` builds `kOr` over `kEq` leaves on one `ColumnName`. A
  one-item list collapses to the plain comparison and is keyed today
  (`src/parser/parser.cpp:1608-1609`).
- The compiler pushes such a conjunct into `Step::bool_residual`
  (`src/exec/step_compiler.cpp:497`). The ladder's arms read
  `step.residual` only (`:2520-2668`), and the last arm refuses the build
  annotation to a step with any `bool_residual` (`:2648`).

### 1.2 The single-value descents CU reuses

- **Tuple key.** `kLookup` is a literal equality on the tuple key,
  `TupleKeyBound`-checked (`src/exec/step_compiler.cpp:2541-2554`), and
  runs in `RunPointStep` (`src/exec/step_vm.cpp:629-631`, `:659`).
- **Index.** `kIndexProbe` walks the entries between two encoded bounds
  (`include/kds/exec/step_chain.hpp:105-130`). `RunIndexStep` collects
  tuple keys through a per-probe `seen_tuple_keys_` set
  (`src/exec/step_vm.cpp:1445-1490`), sorts them back into tuple key order
  (IX8a, `:1525`), then resolves each one through the clustered tree.
- **Cabin.** `kCabinProbe` takes the first cabined equality
  (`CabinProbeOf`, `src/exec/step_compiler.cpp:2599-2605`), and
  `RunCabinStep` serves its set (`src/exec/step_vm.cpp:837`).

### 1.3 Where a new kind would be seen

- **`sys.access_stats` persists the kind.** `StoredAccessKind` appends
  numbers and never reuses one: 1..8 are taken
  (`src/exec/plan_printer.cpp:40-54`).
- **The sort elision.** `EmitsTupleKeyOrder` lists every kind but the
  Cabin probe (`include/kds/exec/step_chain.hpp:189-203`). The elision of
  `ORDER BY <driving tuple key>` reads it (`src/exec/step_compiler.cpp:2771-2796`).
- **The class.** A one-step chain is `kPointSelect` only for `kLookup`
  (`src/exec/step_compiler.cpp:2807-2815`).
- **The join cost model.** `EstimateChain` prices each kind
  (`src/exec/join_order.cpp:215-240`). A chain of tuple key descents alone
  skips the search (`src/exec/step_compiler.cpp:2161-2166`).
- **Waystone.** `IsTrailReplayable` admits `kLookup` and `kProbe` only
  (`include/kds/exec/step_chain.hpp`).

## 2. The mechanism `[design]`

**A key set is an annotation, and the access kind stays the
single-value kind.** `Step` gains a `std::optional<KeySet>` holding the
column, the coerced values (sorted, deduplicated, NULLs and unencodable
items dropped) and the index of the `bool_residual` conjunct it was read
from. The kind stays `kLookup`, `kIndexProbe` or `kCabinProbe`, so
`sys.access_stats`, `EmitsTupleKeyOrder`, Waystone's line and every
`switch` over kinds keep their meaning (CU-Q1). `ANALYZE` prints
`key_set=<n>` on the step.

**The ladder.** Each set arm sits directly after its single-value arm:
- the tuple key set after `kLookup`/`kProbe`;
- the index set beside the literal `kIndexProbe`, under IX9's longest
  usable prefix, with a list column counting as one equality column of
  the prefix and ties broken by `index_oid` as today;
- the Cabin set after the literal `CabinProbeOf`.

A single-value equality on the same structure always wins over a set. A
set wins over every arm ranked below it. This is a pure function of
`(shape, catalog)`, as IX9 requires.

**The runs.**
- **Tuple key set:** one `RunPointStep` descent per value, in ascending
  order. A value with no visible row emits nothing.
- **Index set:** phase 1 walks each value's bounds into **one shared
  `seen_tuple_keys_`**, then one sort and one resolve, as IX8a does for a
  single probe (CU-R2).
- **Cabin set:** one served set per value, unioned and deduplicated by
  tuple key before the resolve. An unobserved value falls back as the
  single probe does: the whole step takes the walk, because a partial
  union is no answer.

Every emitted row passes the whole residual, the list conjunct included.

**Order.** A tuple key set emits ascending, so `ORDER BY <tuple key>`
stays elided. An index set and a Cabin set emit in tuple key order after
their sort, which is exactly IX8a's guarantee. A Cabin set keeps the
Cabin probe's exclusion from the elision.

**Pricing.** `EstimateChain` prices a set as `n` executions of its
single-value kind. A chain whose steps are all tuple key descents or
tuple key sets skips the order search as today.

## 3. Rules

- **CU-R1. The list conjunct stays in the residual** `[quiet-wrong]`.
  Every row a set emits is checked against it. Dropping it would let a
  wrong coercion or a stale index entry answer as the list.
- **CU-R2. A row is emitted once** `[quiet-wrong]`.
  - Index set: an entry left by an earlier version (IX1's surplus) can
    resolve a row whose visible version holds another value in the same
    list. The residual passes at both entries, so without a seen set
    shared across the values, the row would come back twice.
  - Cabin set: the same holds for a row witnessed under two values.
  - A test for each: move a row's column from one list value to another,
    then read it under a snapshot that sees the newer version and under
    one that sees the older version.
- **CU-R3. Coercion is the single-value arm's.** An item the descent would
  refuse to encode (a negative tuple key, an out-of-range literal, a
  descriptor mismatch) is dropped from the set. The residual still sees
  it. If no item remains, the step takes the walk and answers empty
  through the residual. No new refusal is added: a list that types today
  types after CU.
- **CU-R4. No new reserved word, no fingerprint change.** The list's
  values are literals, so `IN (1,2)` and `IN (3,4)` share a fetch shape
  exactly as they do today. Whether the item count is part of the shape
  is CU-S0's census item, and CU changes nothing there.
- **CU-R5. Off the set's shape the cost is nil.** A statement with no list
  conjunct runs no new instruction beyond one test of
  `bool_residual.empty()` at the ladder. This is measured at the close.
- **CU-R6. The spec moves with the code**: `parser-v2.md` I10 (BU-R5
  amended for the one shape), I11 (the elision over a tuple key set),
  `index.md` IX9 (a list column in the prefix) and §8,
  `cabin.md`'s ladder, `plan-stats.md` if pricing moves, and
  `manual/sql/sql.md`. All of these move in the stage that lands the
  behaviour.
- The standing rules: each stage reviewed, the full suite per stage, the
  overhead measured once at the close in `build-release`, a stage's
  measurement inside 20 minutes under a 60 s watchdog, results named by
  `git describe --tags`, nothing pushed or merged without the operator's
  word.

## 4. Items for the operator (CU-Q), CLA's proposals

None is assumed; each waits for a mark.

| | item | options | CLA proposes |
|---|---|---|---|
| CU-Q0 | Does CU-S0 gate the build? | (a) yes: if a tuple key list of 4 and of 64 values over a relation of 100k rows is not served faster than today's walk, CU stops. (b) built whatever S0 shows | (a) |
| CU-Q1 | Kind or annotation | (a) an annotation, `Step::key_set`, on the single-value kind. (b) new kinds `kLookupSet`, `kIndexSet`, `kCabinSet`, with `sys.access_stats` numbers 9..11 | (a). No persisted number moves, Waystone's line is untouched by construction, and `SHOW ACCESS` still names the structure that served |
| CU-Q2 | Which structures | (a) tuple key, index leading column, Cabin column, in that order of stages. (b) tuple key only | (a), staged so the tuple key lands alone if S2 stalls |
| CU-Q3 | A cap | (a) none beyond BU-Q2's 4,096 conditions: the ladder is `f(shape, catalog)` and a set is always taken. (b) a measured crossover above which the walk is kept | (a). (b) is the cardinality estimate IX9 refuses. The loss on a small relation is bounded by 4,096 descents, which S0 measures and states |
| CU-Q4 | Waystone | (a) a step carrying a set records and replays no trail. (b) a trail per value | (a). A set is a search over `n` points, and invariant 9's line stays where it is |
| CU-Q5 | Writes | (a) reads in CU-S1/S2; `UPDATE`/`DELETE ... WHERE <list>` in CU-S3, after S0's census of the point-write target and the lock window. (b) reads only, writes a later order | (a). A write by list walks and locks today what a read by list no longer walks |
| CU-Q6 | The class | (a) a one-step tuple key set stays `kJoinSelect`. (b) `kPointSelect` | (a). `kPointSelect` names one row, and `sys.patterns.stmt_class` is a byte on disk |
| CU-Q7 | Propagation across a join key | (a) not in CU. (b) in CU, as CB-S1 does for ranges | (a) |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **CU-S0** | **The premise and the census.** Measured in `build-release`: `IN` lists of 1, 4, 64 and 1,024 tuple keys, and the same on an indexed column, against today's walk, at 1k and 100k rows, `cores = 1`. The census: every reader of `bool_residual`; every `switch` over `AccessKind`; the point-write target and the lock window of `UPDATE`/`DELETE`; whether the fingerprint reads the item count; the plan memo's literal key; `ANALYZE`'s step line | a results file under `bench/v3.4.11/`, the census in §7, CU-Q0's bar read |
| **CU-S1** | **Tuple key set.** The annotation, the ladder arm, the run, the elision, the pricing. Tests: duplicates, NULLs, negative and out-of-range items, a one-item list unchanged, a list under `NOT` and across columns not keyed, `ORDER BY <tuple key>` elided, every row byte for byte against `indexes = off` and `join_order = written`, a join's inner step keyed by a list, a sim cell | `parser-v2.md` I10, I11; the manual |
| **CU-S2** | **Index set and Cabin set.** CU-R2's shared seen set and its version-moving tests; IX9's prefix with a list column; the Cabin fallback for an unobserved value. The contract suites (index, cabin) extended byte for byte | `index.md` IX9, §8; `cabin.md` |
| **CU-S3** | **Writes**, as CU-Q5 is marked: the point-write target and the lock window over a set, a test per write route, FK and assertion paths unchanged | `parser-v2.md` I10's write half; `txn.md` §5 if the window moves |
| **CU-S4** | **The close.** The overhead A/B over the whole change (CU-R5), S0's arms measured again, the known gaps, the report | the operator's word on landing |

## 6. What CU does not do

- No change to what a residual means or to three-valued evaluation.
- No statistics in an access choice, and no change to IX9 beyond a list
  column counting in the prefix.
- No index union, no propagation across join keys, no multi-column list.
- No new persisted kind number, no superblock bump, no record kind.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-15

On `main` at `a61ab905`, on the operator's word. No stage is opened and
no code is written. Documents only.
