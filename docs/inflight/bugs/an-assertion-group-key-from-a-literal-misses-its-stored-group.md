# An assertion's group key built from a literal misses the group its stored rows are counted in

**Reproduced.** Verified at `bb754d06` on `worktree-t2-load-all-types-named-pk`
(`v2.7.0-679` line, base `cd8ca91e`). Found by the critics-developer review of
the T2 load change (BI15, BI16), then confirmed by a throwaway cell, which was
not kept.

## What is wrong

An insert's admission keys its group from the row's values **as they
arrived**: `AssertionEnforcer::AdmitInsert` → `KeyFor` → `EncodeGroupKey`
(`src/exec/assertion_check.cpp`). The build's backfill
(`src/exec/assertion_build.cpp`) and the `DELETE`/`UPDATE` paths key from
**decoded** rows. `EncodeGroupKey` tags each value kind differently (`kInt`,
`kStr`, `kDecimal`), so one stored value has two keys whenever its literal form
and its decoded form are different kinds. This holds for `date` and
`timestamp`, where the literal is a `kStr` and the decoded value is a `kInt`,
and for `decimal`, where the literal is a `kStr` and the decoded value is a
`kDecimal`.

`NoteCabinWrite` documents and fixes the same mistake for Cabins
(`src/server/command_dispatcher.cpp`, its `CoerceLiteralToColumn` call). The
assertion path does not do the same.

## What it costs

**A quiet wrong answer.** A bound the assertion exists to keep is broken. This
cell, on the tree above:

```
CREATE TABLE t (id int64, d date)
INSERT INTO t VALUES ('2026-01-01')
INSERT INTO t VALUES ('2026-01-01')
CREATE ASSERTION cap ON t GROUP BY (d) CHECK COUNT(*) <= 2
INSERT INTO t VALUES ('2026-01-01')   -> INSERTED (a third row; must be refused)
```

The same happens with a `decimal(10,2)` group column and the value `'1.50'`.

**Rows admitted after the create collide among themselves.** All of them are
`kStr` keys, so their own count is enforced. What escapes is the backfill's
count. For the same reason a `DELETE` gives back a reservation under a key
that no insert used.

**The T2 load stream:** it sends `date` and `timestamp` as their decoded
`kInt`, so they agree with the backfill. It sends `decimal` as literal text
(BI16, so its precision is checked), so decimals inherit the T1 behaviour.

## The fix

Not settled. The proposal is to coerce each group value with
`CoerceLiteralToColumn` in `KeyFor`, as `NoteCabinWrite` does. That needs each
group column's `len` (the decimal scale) in `LiveAssertion`, which today
carries only `group_type_vals`. No test groups an assertion by a `date`,
`timestamp` or `decimal` column. The fix opens with that cell, red first.
