# Work order CU - a value list as a key: `IN (...)` and an `OR` of equalities served by one descent per value

Status: **OPEN since 2026-11-16: CU-S0, CU-S1, CU-S2, CU-S3 and CU-S3b
are done on the work branch, CU-S4 (the close) is not opened.** CU-S0 was opened on the operator's word of
2026-11-16 (`raft-marks-2026-11-16.md` §1), on worktree
`cu-key-set-access` from `1edf5c54` (`v3.4.11-20-g1edf5c54`), with
**CU-Q0 to CU-Q7 each marked (a)**; CU-Q0's bar is met (§7). CU-S1 was
opened on the operator's second word of that day (§2 of that file), with
**CU-Q9 (c), CU-Q8 (a) and CU-Q1 (a) kept**: a list on the tuple key is a
key set (§7). **CU-S1's review found that CU-Q8 (a) was marked on a
bound that covers a one-step statement only** (§7); the operator re-read
it and **kept (a)** on the third word of that day, which **opened CU-S2**
(§3 of that file): a list on a column of an index's equality prefix and on
a cabined column is a key set (§7, `9ceb32ee`). **CU-S2 raised CU-Q10 to
CU-Q13, each built to CLA's proposal; the operator marked all four (a)**
on the fourth word of that day, which **opened CU-S3** (§4 of that file):
an `UPDATE` or `DELETE` whose `WHERE` is a list on the tuple key writes
by one descent per value (§7, `84bf6828`). **CU-S3's report raised
CU-Q14; the operator marked it (a) on the word of 2026-11-17, which
opened CU-S3b** (`raft-marks-2026-11-17.md` §1): a write descends for a
tuple key or a tuple key list beside any other conjunct, and a `WHERE`
holding a subquery is left pending
(`docs/pending/keyed-write-beside-a-subquery.md`). Every mark is the
operator's own. No word names CU-S4, so the close, with the overhead
measurement, waits for the operator's word. Nothing is merged or pushed.
Written
on `main` at `a61ab905` (`v3.4.11`) on the operator's word of 2026-11-15
(`raft-marks-2026-11-15.md` §6). Every `[source-read]` claim in §1 was
read at `a61ab905`; §7's census re-read each at `1edf5c54` and lists the
line numbers that differ.

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
- an `OR` nested under an `OR`: `a IN (1, 2) OR a = 3` and
  `(a = 1 OR a = 2) OR a = 3` are a `kOr` holding a `kOr`, which
  `ParseBoolChain` does not flatten, so they are not a `kOr` of leaves
  (CU-S0's census, §7; `a = 1 OR a = 2 OR a = 3` written flat is one
  `kOr` of three leaves and is the shape);
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

Every item is **the operator's own mark of 2026-11-16**: CU-Q0 to CU-Q7
on the word that opened CU-S0 (`raft-marks-2026-11-16.md` §1), and CU-Q8,
CU-Q9 and CU-Q1 again, which CU-S0 raised, on the word that opened CU-S1
(§2 of that file), and CU-Q8 a third time on the word that opened CU-S2
(§3). **CU-Q10 to CU-Q13 surfaced in CU-S2, were built to CLA's proposal
and were each marked (a)** on the word that opened CU-S3 (§4 of that
file); CU-Q12 and CU-Q13 depart from §2's "a set wins over every arm
ranked below it", and the mark is what amends it. **CU-Q14 surfaced in
CU-S3's report and is the operator's mark of 2026-11-17**
(`raft-marks-2026-11-17.md` §1).

| | item | options | CLA proposes | mark |
|---|---|---|---|---|
| CU-Q0 | Does CU-S0 gate the build? | (a) yes: if a tuple key list of 4 and of 64 values over a relation of 100k rows is not served faster than today's walk, CU stops. (b) built whatever S0 shows | (a) | **(a)**, the operator, 2026-11-16. The bar is met (§7) |
| CU-Q1 | Kind or annotation | (a) an annotation, `Step::key_set`, on the single-value kind. (b) new kinds `kLookupSet`, `kIndexSet`, `kCabinSet`, with `sys.access_stats` numbers 9..11 | (a). No persisted number moves, Waystone's line is untouched by construction, and `SHOW ACCESS` still names the structure that served | **(a)**, the operator, 2026-11-16. **One stated reason is wrong** (§7's census): under (a) Waystone's line is *not* untouched by construction, because `IsTrailReplayable` reads the kind and a set on `kLookup` would answer replayable. Four readers of the kind have to learn the annotation, and no compiler check enforces any of them; a missed `IsTrailReplayable` records a trail for a set step. Under (b) those readers stay right by construction and `EmitsTupleKeyOrder`'s `switch` refuses to compile until the new kinds are judged, at the price of three persisted numbers and a case in every other `switch`. CLA still proposed (a), on the other two reasons, with a test that a set step records and replays no trail. **(a) kept**, the operator, 2026-11-16, re-read against the census |
| CU-Q2 | Which structures | (a) tuple key, index leading column, Cabin column, in that order of stages. (b) tuple key only | (a), staged so the tuple key lands alone if S2 stalls | **(a)**, the operator, 2026-11-16 |
| CU-Q3 | A cap | (a) none beyond BU-Q2's 4,096 conditions: the ladder is `f(shape, catalog)` and a set is always taken. (b) a measured crossover above which the walk is kept | (a). (b) is the cardinality estimate IX9 refuses. The loss on a small relation is bounded by 4,096 descents, which S0 measures and states | **(a)**, the operator, 2026-11-16. The loss S0 states (§7): below about 100 rows on the tuple key and below a few hundred on an index a set is estimated slower than the walk, by at most 3.7 ms per statement, and by at most about 12 ms if CU-R1's re-check stays a test of the whole list (CU-Q9 (a)), in which case a list that matches most of a 1,000-row relation loses too |
| CU-Q4 | Waystone | (a) a step carrying a set records and replays no trail. (b) a trail per value | (a). A set is a search over `n` points, and invariant 9's line stays where it is | **(a)**, the operator, 2026-11-16 |
| CU-Q5 | Writes | (a) reads in CU-S1/S2; `UPDATE`/`DELETE ... WHERE <list>` in CU-S3, after S0's census of the point-write target and the lock window. (b) reads only, writes a later order | (a). A write by list walks and locks today what a read by list no longer walks | **(a)**, the operator, 2026-11-16 |
| CU-Q6 | The class | (a) a one-step tuple key set stays `kJoinSelect`. (b) `kPointSelect` | (a). `kPointSelect` names one row, and `sys.patterns.stmt_class` is a byte on disk | **(a)**, the operator, 2026-11-16 |
| CU-Q7 | Propagation across a join key | (a) not in CU. (b) in CU, as CB-S1 does for ranges | (a) | **(a)**, the operator, 2026-11-16 |
| CU-Q8 | Does a long set yield? A statement driven by a tuple key descent is run inline and never handed to the sliced walk (`SelectBody`); an index-driven one is handed on but reaches no boundary, since among a statement's steps only the outermost walk's page loop has one (`sched.md` §3). A set of 4,096 values is 1.6 to 4.2 ms of descents on the tuple key and 2.0 to 12.7 ms on an index, on top of about 4 ms for the list itself, which today's walk also spends unyielded; under CU-Q9 (a) the re-check adds up to about 84 ms (§7, derived) | (a) a set runs as a descent does today: inline, no yield, no cancel point inside it, the measured bound stated in `known-gaps.md`. (b) a set driving a `SELECT` is handed to the sliced run and yields, and takes `C_CANCEL`, every 64 values | (a) if CU-Q9 is (c) or (b): an index walk and a join's inner probes already run unyielded under `exec::Budget` (`sched.md` §3), the bound is then a few milliseconds, and (b) needs a resumable position inside the set and moves `sched.md` §3 and `protocol.md` §7 and §10, for a list length few statements reach. If CU-Q9 is (a) the bound is about 0.1 s and CLA proposes (b) | **(a)**, the operator, 2026-11-16; **kept** on the operator's re-read of the same day against the join bound CU-S1's review found (`raft-marks-2026-11-16.md` §3) |
| CU-Q9 | The cost of CU-R1's re-check `[quiet-wrong]`. CU-R1 keeps the list in the residual, and the residual is evaluated leaf by leaf in written order on every row a step emits, so a row found through the list's i-th value costs i comparisons: a set emitting h rows pays about h x (n + 1) / 2 of them, about 84 ms at 4,096 matching rows (§7, derived from the walk's measured 10 ns a comparison) | (a) as CU-R1 is written: the whole conjunct, in written order, on every emitted row. (b) the set step tests the row's column for membership in the set's sorted values, O(log n), and the conjunct leaves the residual for that step. (c) the conjunct stays in the residual and is evaluated whole, but for a row a set found the leaf whose value drove the descent is tested first; only if that leaf is not true is the rest of the tree evaluated in written order | (c). An `OR` is true as soon as one leaf is, so testing one leaf first changes no answer, three-valued included, and the conjunct that is evaluated is still the written one with its own literals: a wrong coercion or a stale index entry fails the leaf and falls to the whole tree, exactly as under (a). The cost is one comparison per emitted row where the row holds the value that found it. (b) tests against the coerced set, which is what CU-R1 exists to check | **(c)**, the operator, 2026-11-16 |
| CU-Q10 | A list's rank among indexes at one prefix length. §2 says a list column counts as an equality column and ties go to `index_oid` as today. With indexes on `(status)` and `(user_id)`, `status IN ('A', 'B') AND user_id = 5` is such a tie, and today it enters `(user_id)` | (a) at one equality prefix length single equalities outrank a prefix a list fills, before the range column and `index_oid` are asked: no statement leaves an index an equality put it on unless a list reaches a longer run of equality columns. (b) as §2 wrote: `index_oid` alone, so the statement above moves to `(status)` when that index is the older | (a). (b) changes the plan of statements that exist today, for the worse where the list's column is the less selective. **Built as (a)**; (b) is one term of `IndexChoiceOver`'s score | **(a)**, the operator, 2026-11-16 |
| CU-Q11 | An item an index's key encoder refuses `[quiet-wrong]`. CU-R3 says an item the descent would refuse to encode is dropped from the set. On the tuple key that is safe by invariant 7: a negative number is no key. For an index the encoder's refusal is not a proof that no row holds the value | (a) a NULL item is left out, since a key column is never NULL; any other refused item declines the list for that index, which is then scored without it. (b) as CU-R3 wrote: every refused item is dropped and the rest are keyed | (a). (b) answers a row short wherever the encoder refuses a value a row can hold. No such value was found to construct, since coercion refuses first, so (a) costs nothing known. **Built as (a)** | **(a)**, the operator, 2026-11-16 |
| CU-Q12 | The Cabin set's rank. §2 puts it directly after the literal `CabinProbeOf` and has it win over every arm below: the expression, predicate, correlated and cover Cabin arms, and a strict tuple key range. A set observes nothing, so while one of its values is unobserved it walks and no run of the statement changes that; each of those arms records what it misses, is keyed per outer row, or bounds its walk. With Cabins on `DATE(ts)` and on `sym`, `DATE(ts) = '2026-10-02' AND sym IN ('a', 'b')` was an expression probe that served from its second run, and under §2 it walks until other statements have observed `'a'` and `'b'` | (a) the Cabin set is the last of the Cabin arms and takes only a step that would otherwise walk its relation whole: above the filter scan, never on a step with a strict tuple key range. (b) as §2 wrote | (a). Under (b) a list turns statements that are served today into walks nothing in them ends. Under (a) a set replaces only a walk. **Built as (a)**; (b) is the arm moved back up in the ladder | **(a)**, the operator, 2026-11-16 |
| CU-Q13 | A list on a step a join key could enter. §2 puts the index set beside the literal `kIndexProbe`, which outranks the correlated index probe and the correlated Cabin probe (`index.md` §8a). `o.user_id = u.id AND o.status IN ('open', 'held')`, with indexes on `(user_id)` and `(status)` and `o` the inner step, was one descent into `(user_id)` per outer row; under §2 it walks every entry of both statuses per outer row | (a) no list is read on a step where a correlated index probe or a correlated Cabin probe stands: the step takes what it took before lists were read. The planner may still order the relation first, where the list is its set. (b) as §2 wrote: the set outranks both, as a single literal equality does today | (a). (b) multiplies by the list's length a trade the single-value rule already makes, on every outer row. (a) changes no plan that exists. A correlated probe that also carries the list on a later prefix column is the better plan than either and is not built. **Built as (a)**: `LiteralIndexChoiceOf` | **(a)**, the operator, 2026-11-16 |
| CU-Q14 | A write's key beside other conjuncts. CU-S3 kept the point write's gate: a write descends only where the tuple key's conjunct - a single equality, or since CU-S3 a list - is the clause's only one, so `UPDATE ... WHERE id = 1 AND v = 3` visits every leaf under a write latch, before CU and after. The row function asks the whole `WHERE` of the row a descent finds, so the gate is narrower than it need be, except for a `WHERE` holding a subquery, which a write must not evaluate under the leaf it holds exclusive | (a) in CU, as a stage before the close: a write descends for a key or a list beside any conjunct; a `WHERE` holding a subquery keeps its walks. (b) outside CU, a later order, and CU closes with the list's write "the whole `WHERE`" | (a), so the close's overhead measurement covers it | **(a)**, the operator, 2026-11-17, with the subquery case recorded as pending (`docs/pending/keyed-write-beside-a-subquery.md`) |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **CU-S0** | **The premise and the census.** Measured in `build-release`: `IN` lists of 1, 4, 64 and 1,024 tuple keys, and the same on an indexed column, against today's walk, at 1k and 100k rows, `cores = 1`. The census: every reader of `bool_residual`; every `switch` over `AccessKind`; the point-write target and the lock window of `UPDATE`/`DELETE`; whether the fingerprint reads the item count; the plan memo's literal key; `ANALYZE`'s step line | a results file under `bench/v3.4.11/`, the census in §7, CU-Q0's bar read |
| **CU-S1** | **Tuple key set.** The annotation, the ladder arm, the run, the elision, the pricing. Tests: duplicates, NULLs, negative and out-of-range items, a one-item list unchanged, a list under `NOT` and across columns not keyed, `ORDER BY <tuple key>` elided, every row byte for byte against `indexes = off` and `join_order = written`, a join's inner step keyed by a list, a sim cell | `parser-v2.md` I10, I11; the manual |
| **CU-S2** | **Index set and Cabin set.** CU-R2's shared seen set and its version-moving tests; IX9's prefix with a list column; the Cabin fallback for an unobserved value. The contract suites (index, cabin) extended byte for byte | `index.md` IX9, §8; `cabin.md` |
| **CU-S3** | **Writes**, as CU-Q5 is marked: the point-write target and the lock window over a set, a test per write route, FK and assertion paths unchanged | `parser-v2.md` I10's write half; `txn.md` §5 if the window moves |
| **CU-S3b** | **A write's key beside other conjuncts**, as CU-Q14 is marked: the single-key target and the list read off the compiled `WHERE` with any conjunct beside them but a subquery; the lock window unmoved | `parser-v2.md` I10; the manual; `docs/pending/keyed-write-beside-a-subquery.md` |
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

### CU-S0 - the premise and the census, 2026-11-16

On worktree `cu-key-set-access` at `1edf5c54` (`v3.4.11-20-g1edf5c54`).
**The bar of CU-Q0 (a) is met.** No engine code is changed; the stage adds
a driver (`tools/cu_s0_premise.py`), a results file and these documents.

**The premise** `[measured]`, Release at `1edf5c54`:
`bench/v3.4.11/results-cu-s0-premise-v3.4.11-20-g1edf5c54.md`. Server CPU
per list, `cores = 1`, one connection, a quiet host:

- **The bar's two cells**, tuple key, 100,000 rows. A list of 4 values:
  9,110 µs as today's walk, 98.4 µs as four separate statements, an
  estimated 30.9 µs as a key set. A list of 64 values: 73.3 ms, 1,479 µs,
  an estimated 113 µs. The indexed column reads the same: 9,359 µs
  against an estimated 37.4 µs, and 72.7 ms against 242 µs.
- **The walk is a product.** A list as a residual tests each row against
  each value, about 10 ns a comparison: 1,024 values over 100,000 rows
  cost 1.04 s of server CPU and 4,096 values 3.98 s.
- **A descent inside a statement** costs 0.4 to 1.1 µs per value on the
  tuple key and 2.6 to 3.2 µs on an index, at 1,000 rows and more. A
  separate statement costs 21 to 29 µs per value. The list itself costs
  about 0.5 to 1 µs per item to parse, type and compile, 4 ms at 4,096
  items, and the walk pays that too.
- **No arm is a key set**, which is unbuilt. The estimate is the list
  alone (the walk arm over a 1-row relation) plus the descents (the
  values stored as a relation's rows and joined to the table, which the
  engine runs as one probe per row). The bar does not rest on it: the
  separate statements, which overpay, already beat the walk 93 and 50
  times.
- **The estimate leaves out CU-R1's re-check.** Every row a set emits is
  tested against the whole list (`AcceptTupleAt`, `step_vm.cpp:2406`), and
  the `OR` stops at its first true leaf in written order
  (`chain_frame.cpp:278-288`), so a set emitting `h` rows pays about
  `h × (n + 1) / 2` comparisons at the walk's own 10 ns (derived from the
  walk cells, not measured). The bar's cells move by under 21 µs, and the
  separate statements stay above the estimate with it in every cell. The
  re-check reaches about 84 ms at 4,096 matching rows.
- **CU-Q3's loss, stated.** A set is estimated slower than the walk in 7
  of the 12 cells at 10 and 100 rows: on the tuple key at 10 rows from 64
  values up, on the index at 10 rows at every length and at 100 rows from
  64 values up. The largest loss is 3.7 ms, a 4,096-value index list over
  100 rows. At 1,000 rows a set is estimated 1.5 to 7.4 times faster in
  every cell. The crossover is between 10 and 100 rows on the tuple key
  and between 100 and 1,000 on an index. **That is without the re-check.**
  With it, 8 of the 12 small cells lose, and so do the four 1,000-row cells
  of 1,024 and 4,096 values, where every row of the relation matches the
  list and the walk's whole cost is the check a set repeats. The largest
  loss is then about 12 ms (index, 1,000 rows, 4,096 values).
- **Beyond the order's cells**: 4,096 values (BU-Q2's cap), and relations
  of 1, 10 and 100 rows, added to state CU-Q3's loss. **Cut to the
  budget**: 2 repeats of 2 s per arm (1 s on the small relations), no
  Cabin column, no PostgreSQL floor, no wait sampling beyond `ANALYZE`'s
  `DB time` line, which shows CPU only.

**The census** `[source-read]` (an `Explore` read at `1edf5c54`; CLA
re-read the lines marked *). Line numbers are `1edf5c54`'s.

- **§1's citations.** None of the files §1 cites changed between
  `a61ab905` and `1edf5c54` but `step_vm.cpp` (one rename). Three
  citations were off when written: the one-item collapse is
  `parser.cpp:1612`* (not `:1608-1609`), `ParseValueList` is `:1589-1614`,
  and `seen_tuple_keys_` is a runner member (`step_vm.cpp:2730`) cleared
  at the head of each `RunIndexStep` (`:1445-1447`), not a per-probe set.
  `bool_residual` has two feeders, not one: `CompileBound` and
  `CompileWhere`, the write path (`step_compiler.cpp:2448`, `:2013`).
- **§0.2's shape is narrower than its sentence.** A written
  `col = a OR col = b OR col = c` is one flat `kOr`. `a IN (1, 2) OR a = 3`
  and `(a = 1 OR a = 2) OR a = 3` are a `kOr` holding a `kOr`
  (`ParseBoolChain`, `parser.cpp:1526-1546`), so they are not the shape
  and keep the walk. §0.2's list now says so. `NOT IN` never reaches
  `bool_residual`: `Flatten` splits it into flat `!=` conjuncts.
- **Readers of `bool_residual`**: fourteen sites, all of which evaluate,
  type-check, print or collect columns from the conjuncts whatever the
  step's kind. `AcceptTupleAt` (`step_vm.cpp:2406`) evaluates them on
  every accepted row for every access kind, so CU-R1 holds with no change
  there. One site assumes a walk: the ladder's build arm (`:2648`), which
  is reached only while the kind is `kScan`.
- **Readers of the access kind that a set on a single-value kind would
  mislead** (this is what puts CU-Q1's reason in question):
  1. `IsTrailReplayable` (`step_chain.hpp:170-172`) answers true for
     `kLookup`, and through it `HasReplayableStep`, the trail recording in
     `AcceptTupleAt` (`step_vm.cpp:2513`), `ReadColumnsOf` (`step_compiler.cpp:1737`) and
     `trail_replay.cpp:44`. Under CU-Q4 (a) it has to read the annotation.
  2. The class (`step_compiler.cpp:2807-2815`*): a one-step `kLookup` is
     `kPointSelect`. Under CU-Q6 (a) it has to read the annotation.
  3. `RunStep`'s dispatch (`step_vm.cpp:629-637`) sends `kLookup` to
     `RunPointStep`, which dereferences `step.key` unconditionally
     (`:661`*). A set has no `step.key`.
  4. `Estimate` (`join_order.cpp:155-179`, `:216-250`): a set has no flat
     key equality, so its matches would be priced as a filtered walk's
     while its execution is priced as one descent.
  Readers that need no change: `EmitsTupleKeyOrder`, `StoredAccessKind`,
  `AccessKindName`, `RecordAccess`, the all-descents test that skips the
  order search (`:2162-2167`).
- **`SelectBody` runs a descent-driven statement inline**
  (`command_dispatcher.cpp:9074-9076`*), so a tuple key set would never
  reach the sliced walk. This is CU-Q8. An index-driven statement is
  handed on but never reaches a boundary, since only the walk's page loop
  has one. With the re-check, the unyielded bound at 4,096 values over
  100,000 rows is about 0.1 s, not a few milliseconds (derived). Today's
  walk of that list holds its core about 0.36 s per 64-page slice (3.98 s
  over 705 pages). Under (b), `sched.md` §3 and `protocol.md` §7 (what
  streams) and §10 (where a cancel is taken) move too.
- **`WAYSTONE ON`** over a list is refused today (`ShapeAdmitsDescent`
  reads `step.residual` only, `plan_printer.cpp:17-38`) and stays refused
  under CU-Q4 (a).
- **Writes** (CU-S3). The point-write target (`TupleKeyEqualityTarget`,
  `command_dispatcher.cpp:7600-7617`*), the lock window
  (`FoldTupleKeyWindow`, `:11725-11800`) and `PURGE`'s window all read
  `parser::Condition`, never a `Step`, so a `Step` annotation does not
  reach them and CU-S3 recognises the shape again at that level. Today
  `UPDATE ... WHERE id IN (1, 2, 3)` declares no range, visits every slot
  of every leaf under a write latch, and takes the relation `IX` and a
  tuple `U` on each match (`X` for `DELETE`). A write by descents takes
  the same units, so the lock window does not move. `PURGE` by list stays
  refused `NotImplemented`; CU-Q5 names `UPDATE` and `DELETE` only.
- **The fingerprint reads the item count** (CU-R4's item). It hashes the
  token stream: `IN (1, 2)` and `IN (1, 2, 3)` differ in `fetch_id`,
  `IN (1, 2)` and `IN (3, 4)` share it and differ in `arg_hash`, and
  `id IN (1, 2)` and `id = 1 OR id = 2` differ in `fetch_id` and share an
  `arg_hash` (`tests/testdata/parser_corpus.txt:232`*, `:244`*). So each
  list length is its own shape in `sys.statement_stats`, `sys.access_stats`
  and the plan statistics, today and after CU.
- **The plan memo** stores a join order only (`plan_memo.hpp:52-56`),
  keyed by `fetch_id` and `arg_hash` among others, and a hit recompiles
  the chain through the ladder. A key set built from the step's literals
  at each compile is correct under it.
- **`ANALYZE`**: a step's plan line is printed by `PrintStep`
  (`plan_printer.cpp:188-258`) and ends at `:235`. `key_set=<n>` goes
  there, beside `build on=`. Every test that pins a step line matches a
  substring that ends before that point or starts at `opens=` on the
  statistics line, so a token printed only on set steps moves none.
- **Tests that pin today's answer by name**:
  `where_bool_test.cpp`'s `OverTheTupleKeyItIsNotALookupOrARange` and
  `OverAnIndexedColumnItIsNotAnIndexProbe`, and
  `where_value_list_test.cpp`, whose header says a list is never a key.
  They pin rows, not plan lines. CU-S1 and CU-S2 keep their rows and
  rename them.

**What CU-S1 needs before it opens**: the operator's word, marks on
CU-Q8 and CU-Q9, and CU-Q1 re-read against the census.

**Review** (`critics-developer`, on the stage's diff at `1edf5c54`). The
bar stands. Taken:

- *The estimate left out CU-R1's re-check* (the reviewer's finding, not
  CLA's): the results file prices it, the loss and the unyielded bound
  above are restated with it, and it is CU-Q9.
- Seven wording and range errors in the results file, corrected there
  (`21 to 29 µs`; the per-descent range is by relation size, not hit
  ratio; the separate statements are 36 to 93 times cheaper than the
  walk, not orders of magnitude).
- One wrong path in this section (`ReadColumnsOf` is in
  `step_compiler.cpp`); the index-driven statement's missing boundary.
- CU-Q1's note: under (a) no compiler check enforces the four readers.

Stated, not fixed: with two repeats the driver's arm order starts with L
both times, and L and D share their draws. Neither moved a number (L's and L2's means differ by at most 5 % in
every 1,000- and 100,000-row cell of 4 values and more).
The driver is left byte-identical to the copy that ran.

Rejected: cutting the client-latency tables from the results file (a
results file carries the full percentile table, p0 and p25 included);
merging the archive's two table scripts (they are the evidence of how the
tables were made).

Test suite: not executed; the stage changes no build input (`tools/`,
`bench/` and `instructions/` only). Overhead not measured; measured at
the milestone's close.

### CU-S1 - the tuple key set, 2026-11-16

On worktree `cu-key-set-access`, from `c9eba484`, committed at `2066487c`.
**A value list on the
tuple key is served by one descent per value.**

**What is built** (`docs/spec/parser-v2.md` I10 owns the contract):

- **The annotation** (`step_chain.hpp`'s `KeySet`, `Step::key_set`): the
  conjunct's index in `Step::bool_residual` and the keys, ascending and
  distinct, each with the first leaf that names it. The step is `kLookup`
  with no `Step::key` (CU-Q1 (a)).
- **The compile** (`TupleKeySetOf`, `step_compiler.cpp`): the first
  conjunct whose root is `kOr` and whose every leaf is `<this step's
  tuple key> = <literal>`. An item is keyed by `TupleKeyBound`, the
  single-value arm's own test (CU-R3), so a NULL or a negative literal is
  left out and stays a leaf; a list with no key keeps its walk. The arm
  sits directly below the single-value descent and above the range, index,
  Cabin, filter-scan and build arms.
- **The run** (`RunKeySetStep`, `step_vm.cpp`): `RunPointStep`'s body
  after its key is derived became `AcceptByKey`, word for word, and a set
  calls it once per value. Not a coroutine; it stops at the sink's stop.
- **The re-check** (CU-R1, CU-Q9 (c)): the conjunct stays in the residual.
  `EvaluateBoolConjuncts` takes a `BoolLeafFirst` naming one leaf of one
  conjunct, asks it first where the root is an `OR`, and passes the
  conjunct on a true answer; any other answer falls to the whole tree in
  written order. The runner names the leaf of the value it descended for.
- **The readers of the kind** the census named, each now asking for the
  annotation: `IsTrailReplayable` takes a `Step`, and the kind's own line
  is renamed `IsTrailReplayableKind`, so asking the kind where a step was
  meant no longer compiles (the review's proposal; it makes CU-Q1's four
  readers one compiler-checked function); the class decision (CU-Q6 (a));
  the runner's dispatch; `Estimate`, which prices a set as its values'
  descents and keeps `values / rows` of the relation.
- **`ANALYZE`** prints `key_set=<n>` at the end of the step's plan line.
- **The sort elision** holds over a set by the ascending order of its
  items; `EmitsTupleKeyOrder` is unchanged.
- **`SelectBody`** is unchanged: a set-driven statement is `kLookup`-driven
  and runs inline (CU-Q8 (a)).

**What differs from §2 as written**: CU-Q9 (c)'s leaf-first test, which §2
predates. Nothing else.

**CU-R5 restated.** §3 says a statement with no list runs one new test at
the ladder. It runs more than that, each a test of one byte or pointer:
`key_set.has_value()` once per run of a lookup or probe step; one per
accepted row of a step that has a boolean conjunct, and a null test per
such conjunct in `EvaluateBoolConjuncts`, the write path's included; one
per accepted row of a step on a statement that records a trail; one per
step where a trail is built or a statement is asked for a replayable
step. `Step` grows by one `std::optional<KeySet>` at its end, below every
hot field. Measured at the close, as CU-R5 says.

**Tests.** `tests/key_set_test.cpp` (28 cells): the shape and what is not
it; CU-R3's items; the ladder; **CU-R1, every statement against the walk
of its own list** (the list in parentheses with one of its leaves written
again beside it, which is an `OR` holding an `OR` and so a scan) over
filters, orders, limits, groups, a driving and an inner step, correlated,
uncorrelated and hoisted subqueries, a query-layer statement, and under
transactions on a two-core rig (an undecided writer's insert, delete and
update seen by itself and by no one else; a `REPEATABLE READ` snapshot
across another session's committed writes); the written join order and
`indexes = off` answering the same bytes; the elision and the limit's
stop; CU-Q4 (`WAYSTONE ON` over a list alone refused; in an opted join
the probe step replays and the set step does not); CU-Q6; a 4,096-item
list keyed and 4,097 refused; a heap's declared column not keyed; a write
that reads a relation by list. `tests/step_compile_test.cpp` (4 cells):
the annotation's contents and leaf indices, the two readers, what is not
the shape, and **CU-Q9's fall-through, which no statement can reach** -
`EvaluateBoolConjuncts` asked for every leaf first, a leaf past the end,
a conjunct that is not there and a `NOT` root, each answering as with no
leaf named. Two cells that pinned BU-R5 by name are renamed; their rows
are unchanged.

**The sim cell.** With the shapes on, a range read's keys are named one
by one in an `IN` list, shuffled, one of them twice, 40 times in 100, on
a stream of its own (`key_list`). It stays a `kSelectRange` with the
range's `lo` and `hi`, so the oracle's expectation is the range's and
the shapes-off corpus is untouched.

**Review** (`critics-developer`, on the uncommitted tree over `c9eba484`):
no wrong-answer finding in the engine code. Taken:

- `IsTrailReplayable` collapsed to the `Step` form (above).
- **CU-Q8's bound was understated**, in `known-gaps.md`, `sched.md` §3
  and I10: 4,096 bounds the descents, and a set that drives a join runs
  the rest of the chain once per value, inline, with no yield, no cancel
  and no row sent before the end. Before CU that driver was a walk, which
  yielded every 64 of its own pages. The three documents now say so.
  **CU-Q8 (a) was marked on the one-step bound**, so this goes back to the
  operator; the code is as (a) says.
- The manual's promise of tuple key order without `ORDER BY`, withdrawn.
- "A write by list walks" narrowed to the write's own rows: a list on a
  relation a write only reads is a read step and is keyed.
- A plan out of `CLAUDE.md`'s row; two stale comments on what a lookup
  emits; two test names that claimed more than they pinned.
- Cells added for what had none: invisible versions under transactions,
  CU-Q9's fall-through, a hoisted subquery, the query layer, the cap, a
  heap, `UPDATE ... FROM`.

Not taken: dropping the `indexes = off` engine from the comparison (§5
names it for this stage, and it stops being vacuous at CU-S2); merging
the compile-level cells with the dispatcher-level ones (one pins the
structure, the other the answer); dispatching on `!step.key` to keep the
test on the kind's cache line (it reads as a lookup with a missing key).
**Not covered, stated in `known-gaps.md`**: that a recorded trail holds
no entry for a set step is pinned only through `IsTrailReplayable(const
Step&)` and the absence of `replays=`; a KWP portal read with `max_rows`
over a set-driven statement.

**A behaviour the ladder's order brings**: `id IN (...) AND sym = 'x'`
was a filter scan and a Cabin candidate for `sym`, and is a lookup now and
feeds none, as `id = 5 AND sym = 'x'` never did.

**Test suite**: 4513 of 4513 passed on the tree this section is committed
with (`ctest -LE bs-nightly`, the range the push hook runs, Debug), after
the review's changes; 4506 of 4506 before them.

Overhead not measured; measured at the milestone's close. A check of the
built path on the uncommitted tree, on a host that was running the test
suite, put the bar's two cells near CU-S0's estimate; it is not a
measurement and no number from it is recorded.

### CU-S2 - the index set and the Cabin set, 2026-11-16

On worktree `cu-key-set-access`, from `ffdcc6d5`, committed at `9ceb32ee`.
**A value list on one column of an index's equality prefix, and on a column with a column
Cabin, is a key set.**

**What is built** (`docs/spec/index.md` IX8b and §9, `docs/spec/cabin.md`
§4e and `docs/spec/parser-v2.md` I10 own the contract):

- **The shape, read once** (`ListConjunctOn`, `step_compiler.cpp`): the
  first boolean conjunct whose root is `kOr` and whose every leaf is
  `<this step's column> = <literal>` on one named column. The tuple key
  set of CU-S1 reads it too.
- **The index set** (`IndexChoiceOver`, `IndexProbeOf`): one list may
  stand as one equality column of an index's prefix - anywhere in it, and
  before the range column - where that column has no single equality.
  The probe's bounds are encoded at compile once per distinct value, in
  key order, into `KeySet::bounds`; the step is `kIndexProbe` or
  `kIndexRange`. A second list ends the prefix and filters.
- **Its run** (`RunIndexStep`, `step_vm.cpp`): the collect phase is asked
  once per value into **one seen set** (CU-R2), each new tuple key packed
  with the leaf of the value whose entry found it; one sort, one resolve.
  The covering filter judges an entry, and a row is kept if any entry of
  any value passes. A set whose bounds do not fit its index's entry width
  walks.
- **The Cabin set** (`CabinSetOf`): the first list on a non-key column
  that carries a column Cabin, one item per distinct Cabin key, NULLs
  left out; a list holding a value the Cabin cannot key is no set. The
  step is `kCabinProbe`.
- **Its run** (`RunCabinSetStep`, `ServeAllOrWalk`): every value's set
  looked up; all observed, the step is served from their union through
  `ServeFromCabin`, whose one seen set is CU-R2's; one unobserved, or more
  values than `cabin_max_values`, the whole step walks. **A list observes
  and banks nothing.** The cover's run and the set's are one function.
- **The re-check** (CU-R1, CU-Q9 (c)): the conjunct stays in the residual
  on both, and the leaf of the value that found the row is asked first.
- **The cost model** (`join_order.cpp`): a set's own conjunct is not
  counted twice; a set costs one descent per value and one resolve per
  match.
- **`ANALYZE`**: `key_set=<n>` on the step line, `n` the descents or sets
  asked.

**What differs from §2 as written**, each an item in §4 waiting for a
mark:

- **CU-Q10**: at one equality prefix length a single equality outranks a
  list, before the range column and `index_oid`.
- **CU-Q11** `[quiet-wrong]`: an item the index key encoder refuses
  declines the list for that index; only a NULL item is dropped.
- **CU-Q12**: the Cabin set is the last Cabin arm and takes only a step
  that would otherwise walk whole.
- **CU-Q13**: no list is read on a step a correlated index or Cabin probe
  could enter.

§2's "sorted, deduplicated" holds for the index set by encoded bytes and
for the Cabin set by Cabin key, in written order: a Cabin has no order.

**Tests.** `tests/key_set_test.cpp` (47 cells, 19 of them this stage's):
the index set alone, at each prefix position, before a range, two lists,
the ranking rules of CU-Q10 with and without a range, CU-Q13 on a join
key's index and on a join key's Cabin in the written order and the
planner's own, a tuple key set over an index set, the 4,096-value list on
an index, lists of dates, decimals and timestamps; CU-R2 on an index by a
row moved between listed values, in one session and under two snapshots;
the covering filter across two values' entries of one row, both ways; the
Cabin set served, unobserved, partly observed, an observed empty set,
duplicates and a NULL, CU-R2 on a row in two sets, in one session and
under two snapshots, CU-Q12 against a strict range, a predicate Cabin and
a filter scan, the sort not elided. Every keyed statement is compared
with the walk of the same list. `tests/index_contract_test.cpp` and
`tests/cabin_contract_test.cpp` carry list statements in the set every
configuration must answer byte for byte.

**Review** (`critics-developer`, on the uncommitted tree over
`ffdcc6d5`): no wrong answer found in either path; the encoding, the
packing, the shared seen sets, the covering filter across values and the
Cabin gates were each traced. Taken:

- **The Cabin set ranked above four arms that served better** - an
  expression, predicate, correlated or cover Cabin statement carrying a
  list on a cabined column became a walk nothing ended. Moved below them
  and kept off a strict range; raised as CU-Q12.
- **An index set took a join's inner step from its correlated probe**, a
  set of every listed value's entries per outer row. No list is read
  there now; raised as CU-Q13.
- **The score did not say what CU-Q10 says**: a list followed by a range
  outranked a single equality at one prefix length. The score is now
  equality columns, then no list, then the range.
- **A Cabin set counted duplicate values** in `key_set=<n>`, in the cost
  model and against `cabin_max_values`. Deduplicated at compile.
- `RunCabinSetStep` and `RunCoverStep` were one gate written twice: one
  function now. One constructor for a column Cabin's probe, where three
  sites filled it by hand. `IndexChoiceOver`'s score out-parameter became
  a field.
- Cells added: the transaction cell's `ANALYZE` after the move (it could
  pass on a walk), the covering filter across values, the list-then-range
  tie, CU-Q12, CU-Q13, the cap on an index, typed lists.
- Documents: the NULL rule's reason is that `col = NULL` is true of no
  row, not that a key column is never NULL.

Not taken: turning the runner's two defensive walks (a conjunct or leaf
index out of range) into `Corruption` - a walk answers such a chain
correctly and costs one compare; replacing the packed `uint64` scratch
with a pair vector, which the review itself advised against.

**Not covered, stated in `known-gaps.md`**: the decline of a set whose
bounds do not fit its index (no compile produces one); a Cabin set asked
while a recording of one of its values is in flight; an item an index's
key encoder refuses (none was found to construct). Also stated there: an
index set displaces a literal Cabin probe and a strict range, as a single
index probe does; a list that misses is counted against its Cabin and
moves a filter scan's controller candidacy to the list's column.

**Test suite**: 4532 of 4532 passed on the tree this section is
committed with (`ctest -LE bs-nightly`, the range the push hook runs,
Debug), after the review's changes; 4525 of 4525 before them.

Overhead not measured; measured at the milestone's close.

### CU-S3 - writes, 2026-11-16

On worktree `cu-key-set-access`, from `f1213c77`, committed at
`84bf6828`. **An `UPDATE` or a `DELETE` whose `WHERE` is a list on the
tuple key, and nothing else, writes by one descent per value.**

**What is built** (`docs/spec/parser-v2.md` I10 owns the contract;
`docs/spec/txn.md` §5 states that the window does not move):

- **The shape is read off the compiled `WHERE`, not off
  `parser::Condition`** (`exec::WriteKeySetOf`, `step_compiler.cpp`). The
  census above expected the write to recognise the shape again at the
  dispatcher's level, because the point-write target and the lock window
  read the raw conjuncts. A write compiles its `WHERE` through
  `CompileWhere` before it touches a row, so CU-S1's `TupleKeySetOf` is
  asked of that step and there is one recogniser. The gate is the point
  write's own (`TupleKeyEqualityTarget`): the list is the clause's only
  conjunct.
- **The loop** (`WriteWalk::WriteKeySet`, `command_dispatcher.cpp`): the
  statement's point arm once per distinct value, ascending. The point
  arm's body is one function per statement now (`write_at` in
  `UpdateParsed`, `mark_at` in `DeleteInner`), called by the single-key
  arm and by the loop, so a list's row and a single key's row are written
  by the same code, UPDATE's unique-key round (BX-R4) included.
- **The row** goes through `WriteWalk::Qualify` and `Claim` as a walked
  row does, the list its `WHERE` (CU-R1), with the leaf whose value made
  the descent asked first (CU-Q9 (c); `EvaluateConjuncts` takes the
  `BoolLeafFirst` the step VM already passed).
- **The position is the walk's** (`WalkCursor`, `session.hpp`). A walk
  resumes by key and the values ascend, so a held row parks the statement
  at its value and the resume starts at the first value at or above the
  cursor's key; a descent that declines (`TupleKeyLookup::Kind::kScan`)
  hands the rest of the list to the walk from that key. Nothing new is
  carried across a park.
- **The lock window does not move**, as the census read: a list declares
  no window (`DeclaredWriteBorrow` reads no bound off a `kBoolTree`,
  before CU and after) and each row written takes its row unit, `U` or
  `X`, in key order. No change to `PURGE`, which refuses a list
  `NotImplemented`.
- **The sim** (`sim/workload.cpp`): with the shapes on, a by-key UPDATE's
  or DELETE's `id = k` is spelled `id IN (k, 0, k)` 40 times in 100, on a
  stream of its own (`write_list`). The op's kind and key are unchanged,
  so the oracle's expectation is the single key's and the shapes-off
  corpus is untouched.

**The foreign-key, unique, assertion and Transition paths are
unchanged**: each is the row function's, which the descent and the walk
share. A cell pins each by a refusal on a list's later row that unwinds
its earlier ones.

**Tests.** `tests/key_set_test.cpp`: every write leaves what the walk of
its list leaves, compared across two engines - rows, what each index
answers and what the Cabin answers - for duplicates, a gap, a key past
the relation, a NULL and a negative item, a list that names no key, and a
`SET` that reads the row; **the descents are seen by their holds**, since
a write has no `ANALYZE`: a store that records the leaves of one relation
pinned exclusive shows one leaf for three neighbouring keys where the
walk of the same list holds every leaf, for UPDATE and DELETE; a list
beside another conjunct, a list on another column and two lists hold
every leaf, as a single key beside a conjunct does; a failed fetch makes
one descent decline and the walk takes over from its key, each listed row
written once (`v = v + 1`); a heap's declared first column walks. Under
transactions, on the two-core rig: a write by list is its transaction's
until it decides, `RETURNING` answers its rows, the four checks above,
`PURGE` by list refused. `tests/lock_family_test.cpp`: a list parks at a
held value with the values below it written and resumes there, the trail
holding each row once; a list whose first value is held waits having
written nothing and is run again; the commit arm refuses and unwinds; a
DELETE parks too; and under a cap of three, three listed rows reach the
cap where a window over them would not. `tests/index_unique_rig_test.cpp`:
a list write that has written a row parks on a busy unique key and
resumes at its value, on both of the holder's decides.
`tests/sim_loop_test.cpp`: the
shapes spell a range read and a by-key write as a key list, and without
them never. A shapes-on sweep of the sim, 8 seeds by 3 modes by 2
profiles at 1,500 ops and 2 iterations, passed 48 of 48 (Debug).

**Review** (`critics-developer`, on the uncommitted tree over
`f1213c77`): no correctness finding, and no edit of its own. It traced
each interleaving the position argument rests on - a park in the loop, a
declined descent, a walk that took over and then parked, the count
carried across a park, a first value held, and UPDATE's unique-key round
inside a list, a park inside its check included - and found the
single-key arm's replies unchanged line for line, the leaf-first hint
safe (a stack local cleared before the status is read, one conjunct by
the gate), and the gate closed to a heap's hidden key, a parameter, a
subquery leaf and a derived conjunct. Taken:

- **`WriteKeySet`'s own end enum is gone**: a park is already
  `parked_on_row`, which is how the walk beside it tells one, so the loop
  answers whether every value is done and there is one source for
  "parked".
- **A cell for the unique-key round inside a list** (BX-R4), the one
  interleaving no cell reached: `tests/index_unique_rig_test.cpp`'s
  mid-statement park, run by `id IN (2, 1, 7)` as well as by the walk,
  on both of the holder's decides.
- Wording: a phrase of history out of a code comment and I10; the
  `CLAUDE.md` row no longer reads as though a write's list on an indexed
  column were keyed; the cap sentence in `known-gaps.md`.

Not taken: folding the single-key arm and the list arm of both
statements into one `WriteWalk` member taking the verb, the counter and
the oid. It would save about twenty lines for a seven-argument helper
that formats replies, and the two arms answer differently on purpose - a
single absent key is `UPDATED 0` with no relation named, a list is the
walk's reply - with the `ON CONFLICT` arm's refusal standing between
them in `UpdateParsed`.

**Not covered, stated in `known-gaps.md`**: a write by list while
another core divides the leaf a descent lands in; a declining descent
under a `DELETE`; a list write resumed after its walk took over. Also
stated there: a list beside any other conjunct walks, because the point
write's gate is "the only conjunct" - `WHERE id = 1 AND v = 3` walks the
relation too, before CU and after, though the row function evaluates the
whole `WHERE` on the row a descent finds; lifting that gate is the point
write's question and outside CU. A write finds its rows through no index
and no Cabin, for a list as for a single value.

**Test suite**: 4550 of 4550 passed on the tree this section is
committed with (`ctest -LE bs-nightly`, the range the push hook runs,
Debug), after the review's changes; `build-release` compiles. The suite
was not run on the tree before the review's changes: the review ran the
cells nearest the change, 64 of 64.

**Synced with `origin/main`** at `d9f89624` (the unique check's busy key
parking on the named row's wake), merged into the work branch at
`5007ef48` with no conflict. The merge brought build inputs
(`command_dispatcher.cpp`, two rig tests), so the suite was run again on
the merged tree: 4554 of 4554 at `5007ef48` (the same range, Debug),
main's four new cells among them. CU-S3's unique-key cell runs under
main's check, which now also asks that no wake registration outlives the
statement.

Overhead not measured; measured at the milestone's close.


### CU-S3b - a write's key beside other conjuncts, 2026-11-17

On worktree `cu-key-set-access`, from `94f3c4a3`, committed at
the commit this section arrives in. **An `UPDATE` or a `DELETE` descends for a tuple key
equality or a tuple key list beside any other conjunct but a subquery**
(CU-Q14 (a), the operator's mark of 2026-11-17).

**What is built** (`docs/spec/parser-v2.md` I10 owns the contract):

- **Both targets are read off the compiled `WHERE`** (`step_compiler.cpp`).
  `exec::WriteKeyOf` is the single key: a conjunct `<tuple key> =
  <literal>` in the step's residual, keyed by `TupleKeyBound`, which is
  the rule a read's step becomes a lookup by. `exec::WriteKeySetOf` no
  longer asks that the list be the clause's only conjunct. Each names
  nothing for a `WHERE` with a sub-chain. The single key is asked first,
  so `id = 5 AND id IN (5, 6)` is one descent.
- **The dispatcher's raw-condition gate is deleted**
  (`CommandDispatcher::TupleKeyEqualityTarget`). It answered for exactly
  one condition on the stated ground that the probe could not evaluate a
  second; the row function has evaluated the whole `WHERE` on the row a
  descent finds since the point arm and the walk came to share it, and
  CU-S3's list already relied on that.
- **Nothing else moves.** The point arm's body, the list loop, the park
  and the resume position are CU-S3's. The lock window is read off the
  raw conjuncts as before (`DeclaredWriteBorrow`): a key or a list beside
  a conjunct on another column takes a row unit per row written, and a
  bound on the tuple key beside a list declares its window, under which
  the descents write with no row unit - what the walk of the same `WHERE`
  takes in both cases.
- **The subquery case is not built** (the operator's word): a `WHERE`
  holding a subquery collects its rows in a read walk and writes them in
  a write walk, whatever key it names
  (`docs/pending/keyed-write-beside-a-subquery.md` holds the options).
- **The sim** (`sim/workload.cpp`): with the shapes on, 30 in 100 by-key
  writes carry a second conjunct, `id != 0`, beside the single key or the
  list, on a stream of its own (`write_conjunct`). It holds of every row
  and bounds no window, so the oracle's expectation is unchanged. The
  first sweep refused every run at its first such write - the conjunct
  was spelled `<>`, which the grammar does not read - and was run again
  with `!=`.

**Tests.** `tests/key_set_test.cpp`: the two-engine cell compares a key
and a list beside conjuncts against the walk of the same `WHERE` - a
conjunct that leaves a listed row out, conjuncts before and after the
list, a bound on the key, a single key whose row passes its conjunct,
one whose row fails it and a second key that excludes it, for UPDATE and
DELETE; **the holds**: a key or a list beside other conjuncts takes the
leaf its keys are in where CU-S3's cell showed every leaf, a row that
fails the other conjunct is not written, a single key beside a list is
one descent, and a `WHERE` with a subquery (`IN (SELECT ...)`,
`EXISTS`), a bound on the tuple key and a list on another column still
take every leaf; a single key's descent that declines, beside a conjunct
and beside a list, is answered by the walk and by the list's loop, the
row written once; a function conjunct (`DATE(ts) = ...`) beside a key and
beside a list is asked of the row found; on a heap whose rows went in
out of id order, a key beside a conjunct and a list on the declared
first column write the rows the walk writes. `tests/lock_family_test.cpp`:
under a cap of three, a list of three rows beside a bound on the key is
written under the declared window where the bare list reaches the cap;
a list beside a conjunct parks at a held value whose head fails the
conjunct and whose snapshot version passes it (F4), and resumes there
with each row written once; a single key beside a conjunct waits on its
held row having written nothing and is run again, the conjunct asked of
what the holder left on either decide. `tests/sim_loop_test.cpp`: the
shapes spell a by-key write with a second conjunct beside a key and
beside a list, and without them never. A shapes-on sweep of the sim, 8
seeds by 3 modes by 2 profiles at 1,500 ops and 2 iterations, passed 48
of 48 (Debug), on the tree before the review's changes, which moved no
behaviour.

**Review** (`critics-developer`, on the uncommitted tree over
`94f3c4a3`): no correctness finding, and no edit of its own. It traced
that `sub_chains.empty()` is "no subquery anywhere" - a top-level
subquery, a leaf under a boolean conjunct, and `UPDATE ... FROM` and
`DELETE ... USING`, which lower to one, are each a sub-chain of the step,
and a `SET` subquery is answered before any leaf is taken; that
`CompileWhere` runs no equality propagation, so no derived conjunct names
a key; that no conjunct beside a key has a per-row refusal the walk of
the other rows would have raised; that whenever `WriteKeyOf` names a key
the raw fold's window is one key wide or empty, so nothing is declared,
and that every row a descent writes under a declared window lies inside
it; that the single-key arm cannot be reached on a resume that has
written a row; and that the leaf-first hint indexes the vector the
evaluator indexes however many boolean conjuncts stand. Against the
deleted gate it found nothing that descended and now walks; a
single-conjunct `WHERE` whose literal the raw test did not read as an
integer and the compile coerces to one now descends, and is asked its
coerced conjunct on the row found. Taken:

- **The heap cell could not tell a wrong descent from the walk**: its
  rows were inserted in id order, so each declared id equalled its hidden
  key. The rows now go in out of order and the cell runs a single key
  beside a conjunct as well as a list.
- **A cell for a single key's descent that declines**, beside a conjunct
  (the walk answers) and beside a list (the list's loop answers): the
  two fall-throughs it found with no cell.
- **`TupleKeyLiteral` is a file-local helper** beside its one remaining
  caller, the lock window's fold, and is off the dispatcher's header.
- The two-engine cell's cases say which are a single key instead of the
  cell reading it off the SQL text.
- Wording: the manual read as though a list beside a range walked; a test
  comment and a header comment described the deleted gate;
  `docs/rules/rules.md` §3 names both recognisers; a long line in
  `txn.md`.

Not taken: nothing was rejected. The review advised against one merge it
considered itself - `WriteKeyOf` with the read compiler's key loop, which
also picks column probes in written order - and the two stay apart.

**Test suite**: 4560 of 4560 passed on the tree this section is
committed with (`ctest -LE bs-nightly`, the range the push hook runs,
Debug), after the review's changes; `build-release` compiles. The suite
was not run on the tree before the review's changes: the cells nearest
the change passed there, 187 of 187, and 188 of 188 after.

**Synced with `origin/main`**: fetched on 2026-11-17 and still at
`d9f89624`, which the work branch holds since `5007ef48`. Nothing to
merge.

Overhead not measured; measured at the milestone's close.
