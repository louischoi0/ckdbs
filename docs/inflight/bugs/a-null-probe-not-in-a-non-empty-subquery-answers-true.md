# A NULL probe NOT IN a non-empty subquery answers TRUE

**Reproduced** at `ac8159ae` plus the uncommitted review changes on
`worktree-fix-nested-uncorrelated-subquery`, by
`CommandDispatcherTest.DISABLED_ANullProbeIsUnknownAgainstANonEmptySubquery`
(`tests/command_dispatcher_test.cpp`), which fails as written and is disabled
until the fix lands. Found by the source review of the nested-hoist fix; it
is older than that fix and unrelated to it. One core, nothing concurrent.

## What is wrong

`x NOT IN (SELECT ...)` with `x` NULL and a subquery that returns at least
one row is UNKNOWN in SQL, so the row is not admitted. The engine answers
TRUE and admits it.

`ChainRunner::EvaluateSubChain` (`src/exec/step_vm.cpp`) compares the probe
only when it is not NULL - `probe != nullptr && !IsNull(*probe) && ...` in
the `kInSubquery`/`kNotInSubquery` arm - so a NULL probe never sets
`saw_match`. A NULL *candidate* sets `saw_null`, but a NULL *probe* sets
nothing, and the verdict reads "no match, no NULL": `kFalse` for IN, which
happens to be right in a WHERE, and `kTrue` for NOT IN, which is not.

Observed, on `n (id int64, v int64 NULL)` holding `(1, NULL), (2, 1)` and
`s (id int64, x int64)` holding `(1, 2)`:

| statement | answer | right answer |
|---|---|---|
| `SELECT id FROM n WHERE v NOT IN (SELECT x FROM s)` | `1`, `2` | `2` |
| `SELECT id FROM n WHERE v IN (SELECT x FROM s)` | none | none |

## What it costs

A **quiet wrong answer**: every row whose tested column is NULL is admitted
by `NOT IN` over a non-empty result, in a SELECT and in a write's WHERE
(`EvaluateConjuncts` reaches the same arm), so an `UPDATE`/`DELETE ... WHERE
c NOT IN (SELECT ...)` also touches rows it must not. An empty result is
right as it stands: `NULL NOT IN (empty)` is TRUE.

## The fix

Known and small: when the probe is NULL and the walk saw a row, the verdict
is `kUnknown` for both kinds (IN's verdict changes nothing in a WHERE, but
is then right where a three-valued result is read). The walk can stop at the
first row in that case, since no later row can change the answer. Enable
the disabled cell with the fix. No work order carries it yet.
