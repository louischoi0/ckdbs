# A write walk's subquery over its own relation sees the statement's own writes

**Found** 2026-10-13 by BU-S3's `critics-developer` review
(`instructions/v3.0.0/workorder-bu-basic-query-surface.md` §6). Reproduced on
`worktree-bu-basic-query-surface` at `e702dca3` by
`tests/where_bool_subquery_test.cpp`'s
`DISABLED_AWritesSubqueryOverItsOwnRelationSeesNoneOfItsWrites`.
**Not fixed.** It predates BU: the flat `WHERE` takes the same path. BU-S3
makes it reachable through `OR` and `NOT` as well.

**Cost: a quiet wrong answer.** An `UPDATE` or `DELETE` writes rows its
`WHERE` does not select, or skips rows it does select, and reports the
count as success.

## The wrong behaviour

`docs/spec/parser-v2.md` R2 says the Halloween problem is unreachable. It is
reachable when a write's `WHERE` holds a subquery over the relation being
written. The subquery is a sub-chain that re-runs for every row
(`exec::EvaluateConjuncts`, `src/exec/step_vm.cpp`). Each run reads the
relation as it stands at that moment, so it sees the rows the same
statement has already deleted or updated. A `SELECT` with the same `WHERE`
answers correctly.

## Reproduction

Over a relation `t` with ids 1..6:

```
SELECT id FROM t WHERE NOT EXISTS (SELECT id FROM t AS x WHERE x.id < t.id)
  -> id 1
DELETE FROM t WHERE NOT EXISTS (SELECT id FROM t AS x WHERE x.id < t.id)
  -> DELETED 6      (should be DELETED 1: every delete makes the next row the first)
DELETE FROM t WHERE id = 99 OR NOT EXISTS (SELECT id FROM t AS x WHERE x.id < t.id)
  -> DELETED 6      (should be DELETED 1)
UPDATE t SET n = 7 WHERE NOT EXISTS (SELECT id FROM t AS x WHERE x.id < t.id AND x.n = 7)
  -> UPDATED 1      (should be UPDATED 6)
```

The `OR` form was reproduced by the cell. The flat `DELETE` and the `UPDATE`
results come from the review's probes at the same commit and were not
re-run when this entry was written.

## The fix

The fix is known in shape but not decided. The subquery's reads on the write
path must not see the statement's own writes: either a view taken before the
statement's first write, or the statement's own writes excluded from what the
subquery sees. That choice is the transaction layer's (`docs/spec/txn.md`),
so it waits for the operator's ruling. No work order carries it yet.
