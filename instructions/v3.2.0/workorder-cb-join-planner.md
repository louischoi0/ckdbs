# Work order CB — a join planner: the join order, the build side, and range bounds through a join key

**CLOSED 2026-10-20 on the operator's word** (`instructions/v3.2.0/raft-marks-2026-10-20.md` §3).

Written and opened 2026-10-18 on `worktree-cb-join-planner` from `7e256216`
(`v3.2.0-172-g7e256216`), on the operator's words, recorded in English
(`raft-marks-2026-10-18.md` §1):

- **W0**: *"For 1, choose the smaller side; for 2, optimize the join order
  and carry the range condition to the key."* - 1 being the build side of
  the statement-local inner build, 2 being I12's written order and the range
  bound that equality propagation does not carry.
- **W1**: CB-Q1..Q4 (§2) marked (a), each CLA's recommendation.

**Claim tags**: `[source-read]` with `path:line` at `7e256216`; `[measured]`
for a number with its commit; `[design]` for reasoning about code not yet
written; `[quiet-wrong]` where a wrong choice turns a correct answer into a
wrong one with no error.

**The letter.** `[source-read]` CA is the last work order under
`instructions/v3.2.0/`; `origin/main` at `7e256216` names no CB. CLA's choice.

**CB against other orders and specs.** CB overrules, for inner joins of the
native step chain, three ratified statements of `docs/spec/parser-v2.md`:
I12's "written order is the plan" with the inner build's hard rule "never
the outer side", I11's defined prefix for `LIMIT` without `ORDER BY`, and
§5's "propagating range bounds through a join key" being out of scope by
decision. It leaves standing: the decorrelation prohibition (CB-Q4 (a)),
equality propagation's constraints, every trust-class rule
(`IsTrailReplayable`, invariants 8 and 9), and `join-inner-build.md`'s
build mechanics.

---

## 0. What CB is

The step chain runs its relations in the order they were written, so the
client chooses the join order and, through it, which relation the
statement-local inner build hashes. CB makes the engine choose both, from
the statement's shape and the catalog alone, and lets a range on one side
of a join-key equality bound the other side.

## 1. The tree as read

- `[measured]` On `build-release` at `dd3af538` (1 core, one run of 200,
  p50; not a measurement of record): scenario 3's `join-no-literal`
  (`users u JOIN loans l ON l.user_id = u.id WHERE u.id BETWEEN 1 AND 16`,
  10,000 loans, no secondary index) runs `Range users` then `Scan loans`
  with `inner_built=1 build_rows=10000 build_probes=15`, 1,090 µs, against
  348 µs for one filtered walk of `loans` alone; written the other way round
  it runs 4,845 µs (10,000 `Probe users`). PostgreSQL's plan is a hash join,
  0.57x of it at `cd7c7c4e` (`bench/v3.1.0/results-scenario3-library-v3.1.0-30-gcd7c7c4e.md` §8).
- `[measured]` Same build: scenario 1's `model-join` shape (8 models, 2,872
  results, `WHERE r.model_id = 3`) probes the one tuple key `m.id = 3` 359
  times (`memo_hits=358 pages=359`); 1,068 µs against 828 µs without the join.
- `[source-read]` Binding is one loop in written order
  (`src/exec/step_compiler.cpp` `CompileBlock` §1), a `ColumnRef`'s
  `rel_slot` is the step index (`include/kds/exec/step_chain.hpp:69`), and
  every later phase - lowering, propagation, placement, the access ladder,
  the build annotation - reads slots, so a chain compiled over a permuted
  binding is a correct chain of that order with nothing else changed.
- `[source-read]` Equality propagation is `PropagateEqualities`
  (`step_compiler.cpp`), appending `derived` conjuncts after the written
  ones; `TupleKeyRangeOf`, `IndexProbeOf` and the scan kernel read whatever
  the residual holds.
- `[source-read]` The inner build hashes the inner step of a walked join on
  its first outer row, bucketing only rows that pass the step's
  non-correlated residual (`src/exec/step_vm.cpp` `WalkAndBuild`, the
  bucketing at the build's residual split).

## 2. Decisions (W1: all (a))

| # | Question | Mark |
|---|---|---|
| CB-Q1 | Emission order once the plan reorders | **(a)** No order without `ORDER BY`; `LIMIT` without `ORDER BY` is some n rows. I11, I12 and the manual amended; a test that compares replies across configurations compares them as multisets where the statement has no `ORDER BY` |
| CB-Q2 | Where the join order's estimates come from | **(a)** A data-free cost model over the compiled chain: `f(shape, catalog)` stays true, so one catalog gives one plan |
| CB-Q3 | Waystone over a reordered chain | **(a)** A step's `step_id` is its relation's **written** position, whatever position it runs at; rule 0 turns any mismatch into a miss |
| CB-Q4 | Scope | **(a)** Inner joins of the native step chain only. Decorrelation stays forbidden; a block carrying a subquery predicate is not reordered |

## 3. Design `[design]`

### 3.1 Range bounds through a join key (CB-S1)

`PropagateEqualities` gains a second pass over the same equivalence classes:
for each member with no written equality-to-literal, and for each direction
(lower: `>`/`>=`; upper: `<`/`<=`), **the first written literal bound in that
direction on another member of the class, across an identical type
descriptor, is appended as a `derived` conjunct on the member** - unless the
member carries a written bound of that direction, or an equality-to-literal,
written or derived (the stronger conjunct). At most one derived bound per
direction per column. On a column its step already keys per outer row - the
own side of a correlated equality - a derived bound filters only and never
picks an index range, a predicate Cabin or a cover over the correlated probe
(`SteersAccess`, from CB-S1's review).

Results are unchanged for equality propagation's reason: inner joins only, a
NULL on either side makes the written equality unknown, and the derived
conjunct is appended, never replacing one. Plans may only be strengthened: a
derived tuple key bound can make a step a `kRange`, a derived column bound
an `kIndexRange`, and on a walked join it narrows what the build buckets.

### 3.2 The join order (CB-S2)

A top-level block of two or more relations, no subquery predicate and not
`SELECT *` is compiled over **every permutation of its relations up to 4**
(24 orders) - over 4, a greedy order: the cheapest driver, then repeatedly
the cheapest next relation - and the cheapest compiled chain runs. The cost
is a pure function of the compiled chain and its catalog:

- **Row estimates**: a relation is `kRelRows` rows; a tuple key equality to
  a literal is 1; every other conjunct to a literal multiplies by a
  selectivity constant (equality, a two-sided range, a one-sided range);
  a join on a tuple key yields at most one row per outer row, any other
  join-key equality `kJoinFanout` rows.
- **Step cost**: per execution of the step - a descent for `kLookup`/`kProbe`,
  a descent plus a descent per matched entry for an index or Cabin probe,
  the relation for a walk. A step past the driver executes once per row
  reaching it, except a step carrying the build annotation, which walks
  its relation once and then costs one map probe per row.
- **Ties** go to the earlier written order, so a statement whose written
  order is already cheapest compiles exactly as before.

The constants are named `constexpr` in one header with their derivation;
a non-tuple-key join keeps one row per outer row of an unfiltered relation,
the tuple key join's own rate, so a join's estimated size is the same from
either side. **A build the model expects past `join_build_max_rows` is priced
as the per-row walk the executor falls back to**, so the planner never
chooses an order for a build the cap would decline. **`join_order = planned |
written`** (default `planned`) is the one setting, added at CB-S2: the
tests that pin a walked join's mechanics run the written chain through it,
and it is the operator's way back to I12 as it stood. It is not a statistic
and does not touch CB-Q2 (a). After the choice,
step ids are rewritten to written positions (CB-Q3). `ANALYZE` prints
`reordered=1` on the class line when the order moved, and each step line
names its written id in execution order, so a plan names the reorder.

### 3.3 The build side (W0's 1)

**No new executor path.** The inner build hashes whichever relation runs as
the inner step of a walked join. Choosing which relation is the inner one is
§3.2's choice, and the cost model makes the smaller side the built one:
a build costs one walk plus one map insert per bucketed row, and its rows
are the inner relation's rows after its own literal conjuncts. In
`join-no-literal` with §3.1, the derived `l.user_id` bounds already cut
the bucketed rows from 10,000 to those of 16 users; where no bound crosses,
the order that hashes the filtered side wins on cost.

### 3.4 What does not change

Trust classes, `IsTrailReplayable`, every residual (a reordered chain's
residual is the same conjunct set placed by the same rule), the build's
mechanics and caps, sub-chain placement, nested blocks (only a top-level
block is reordered), the query layer (I19), and DML.

## 4. Stages

| Stage | What | Gate |
|---|---|---|
| CB-S1 | §3.1 range propagation; `ANALYZE` marks the derived bounds; tests: a derived bound keys a range, an index range and narrows a build, never changes rows (both writings, NULLs, mixed descriptors, strict ends) | suite green, review |
| CB-S2 | §3.2 the order and §3.3 the build side; the order contract amended (`parser-v2.md` I11, I12, `join-inner-build.md`, manual); tests comparing replies across configurations made order-free where no `ORDER BY` | suite green, review |
| CB-S3 | Close: owning specs and CLAUDE.md's row; scenarios 1 and 3's join shapes measured against the opening commit within the hour's budget | operator's word |

Overhead is measured at the close, over the whole change (CLAUDE.md step 3).

## 5. Stage log

- **CB-S1** landed at `bf0637da`: range bounds through a join-key class.
  The review found a derived range displacing a correlated index probe
  (three `IndexCompileTest` cells) and fixed it with `SteersAccess`; its
  per-member rule and one-pass merge adopted. Suite 4074/4074.
- **CB-S2**: the join order. The review found the KWP Keystone field flag
  following the executed driver instead of the FROM relation (fixed in
  `DescribeFields`), a cost double-count on a join column pinned to a
  literal (fixed), and proposed six cuts: planning opt-in at `Compile`, the
  cap read off the budget, the scope copied whole and the dead conditional
  dropped were taken; moving the cost model into `step_compiler.cpp` and
  costing the greedy prefix alone were not (the compiler is 2,700 lines and
  the model is one reader of the chain; the greedy path is for five or
  more relations). Two of its findings are known gaps (`known-gaps.md`,
  The join planner). `join_order` added (§3.2). Suite 4086/4086.
