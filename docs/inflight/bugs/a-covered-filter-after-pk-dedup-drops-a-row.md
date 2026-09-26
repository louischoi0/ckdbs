# A covering index drops a row whose stale entry is walked first

**Reproduced** at `4860e96` on `at-s15-index-leaf-coverage` (a scratch
cell, not committed), found by AT-S15's `critics-developer` pass. One core;
nothing concurrent about it.

## What is wrong

`step_vm.cpp`'s index probe (the `IndexVisitFrom` callback) records an
entry's pk in `seen_pks_` **before** the covering filter
(`CoveredRowSurvives`) runs. Maintenance is append-only, so one row can
own several entries; the first one the walk meets claims the pk. When that
entry is a **stale** one whose covered bytes fail the filter, the row is
filtered, and the current entry - which would pass - is then skipped as a
duplicate.

## Smallest reproduction

```sql
CREATE TABLE t (id int64, a int64, c int64) BTREE;
CREATE INDEX ix ON t (a) COVERING (c);
INSERT INTO t VALUES (2, 10);                 -- id 1: entry (2, 1, c=10)
UPDATE t SET a = 5, c = 20 WHERE id = 1;      -- entry (5, 1, c=20)
SELECT id FROM t WHERE a >= 1 AND a <= 10 AND c = 20;
```

With the index the reply is empty (`ANALYZE`: `index_scanned=2
index_filtered=1`); without it, row 1. The same happens to an older
snapshot when only `c` moved, because a new entry with the same `(key, pk)`
sorts to the front of its run.

## What it costs

A **quiet wrong answer**: a range probe through a covering index loses a
row whose covered column moved together with its key. `index.md` §1 says
an index may never change a result.

## The fix

Known and small, unscheduled: record the pk only once an entry survives the
filter ("keep the row if any of its entries survives"). That is still a
superset - every version that moved a covered column has an entry - and the
pk dedup still stops a row resolving twice. It changes
`index_entries_filtered` counts, which a contract cell may pin. No work
order carries it.
