# An inner build buckets a conjunct on the outer row by the first outer row alone `[quiet-wrong]`

**Reproduced.** Found on `worktree-aq-expression-cabins` over `1099ad88`,
2026-10-15, by the cell AQ-S3's review asked for
(`tests/cabin_expr_test.cpp`, `CabinPredicateTest.AnEnclosingRowsConjunctIsNotTheStepsOwn`).
The wrong answer comes with no Cabin in reach: the cell drops its Cabin and
gets the same answer. Not caused by AQ. AQ's arms run before the build arm in
the ladder, and for a step with no Cabin match they return nothing.

## What is wrong

A correlated `EXISTS` whose sub-query filters on the **enclosing** row's
column against a literal, beside the correlated equality, compiles the inner
step to the statement-local inner build (`docs/spec/join-inner-build.md`):

```
step 1 Scan t build on=col5 key=1:0.5
  filter 1:0.1 = 'closed'      -- twin.state, the outer row's
  filter 1:0.2 > 1000          -- twin.amount, the outer row's
  filter 0:0.5 = 1:0.5
```

The build fills its map once, with the rows that pass the non-correlated
conjuncts. It evaluates `twin.state = 'closed'` and `twin.amount > 1000`,
which read the outer row and are not constant per statement, as if they were
constant, against the first outer row. Every later outer row probes that one
map.

## Reproduction

`twin (id, state, amount, sym)` holds ids 1..5:

| id | state | amount |
|---|---|---|
| 1 | closed | 500 |
| 2 | closed | 1500 |
| 3 | open | 2000 |
| 4 | closed | 1001 |
| 5 | closed | 1000 |

`sym` is distinct per row. `t` holds the same rows.

- `SELECT id FROM twin WHERE EXISTS (SELECT id FROM t WHERE twin.state = 'closed' AND t.sym = twin.sym)`
  answers `1, 2`. The right answer is `1, 2, 4, 5`.
- `... WHERE twin.amount > 1000 AND t.sym = twin.sym)` answers nothing. The
  right answer is `2, 3, 4`.

## What it costs

Rows silently missing from a correlated `EXISTS` (or `IN`) reply, whenever
the sub-query also filters on the enclosing row by a literal and the inner
side has no index or Cabin. No error is raised.

## The likely fix (not built)

`BuildKeyOf`'s gate (`src/exec/step_compiler.cpp`) declines a step whose
residual holds a conjunct reading an enclosing chain's column (`up > 0`) other
than the one correlated equality. Such a conjunct is a property of the outer
row: it can be hoisted to the outer step, or the build declined.
`join-inner-build.md` §5 owns the decision.
