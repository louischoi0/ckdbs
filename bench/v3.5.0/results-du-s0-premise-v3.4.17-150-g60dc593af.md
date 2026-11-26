# DU-S0 premise: an `UPDATE` by a non-unique index key walks 1,000,000 rows for 33 ms of server CPU, and the same rows found through the index and written by tuple key cost 0.11 ms (10 rows) to 2.7 ms (1,000 rows)

**Thesis.** At `60dc593af` (`v3.4.17-150-g60dc593af`) a write whose `WHERE`
is an equality on an indexed column reads no index: it takes every leaf of
the relation exclusive and tests every row. On a 1,000,000-row relation
that is 33.1-35.2 ms of server CPU a statement, whatever the statement
writes. The mechanism DU-S2 proposes - read the index, collect the tuple
keys, write each by the point arm - is runnable today as two statements,
and costs 107 µs for a key of 10 rows and 2,738 µs for a key of 1,000:
**a saving of 308x and 12.9x**, each above DU-Q1 (a)'s bar of 5x. The
two-statement form is an upper bound on the one-statement form: it pays a
second parse, a reply carrying the ids and a second round trip.

**DU-Q1 (a) passes: DU does not stop at the bounded walk.**

**The saving is not unconditional, and the cells say where it ends.** The
walk costs 33 ns a row visited; the keyed form 2.7 µs a row written at
1,000 rows a key (10.7 µs a row at 10, where the two statements' fixed
costs dominate). The two meet where the statement writes about 1 row in 80
of the relation - 1.2 % - and past that the walk is the cheaper arm. The
upper bound overstates the keyed form's cost a row, so the true crossing is
at a larger share; DU-S4's close measures it on the built arm.

| field | value |
|---|---|
| date executed | 2026-11-26, about 19:03-19:05 UTC |
| branch / commit | `worktree-du-keyed-write` at `60dc593af`, `git describe --tags` = `v3.4.17-150-g60dc593af`; the tree clean but for the driver below |
| work order | `instructions/v3.2.0/workorder-du-keyed-write.md` §5, DU-S0; DU-Q1 (a) |
| build | Release (`cmake -DCMAKE_BUILD_TYPE=Release`, target `kds_server`), built in this worktree at the measured commit; the binary's mtime 19:03:30 UTC |
| binary | a copy, `build-release/du-s0-run/kds_server.measured`, sha256 `d9e683d9b312c7e2f3aa4ddf26d81a1b215afc10ce840ae7abe8ac65c22da9cc` |
| device | `/dev/mapper/fedora-root`, xfs (`df -T` at run time) |
| server | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, pinned to CPU 2 (`taskset`), port 31990; one fresh server and data file a cell |
| driver | `tools/du_s0_premise.py` (written for this stage), one connection |
| host | **not quiet**: load 8.25 and 7.83 at the two cells, and another session's `cmake --build -j24` and `ctest -j 12` (worktree `dt-write-path-yield`) ran through every cell. The unit below is on-CPU time, which a contended CPU does not inflate as it does a latency; the control arm states what it did move |
| prior number | none: no file measures a write by a non-unique index key. CU-S0 (`bench/v3.4.3/`) priced a value list on the tuple key |
| PostgreSQL floor | not measured for this shape |
| raw output | `bench/v3.5.0/archive/du-s0-premise-v3.4.17-150-g60dc593af/out.json` |

## 1. The cells

`du_t (id int64, owner int64, v int64) BTREE`, 1,000,000 rows, a secondary
index on `owner`, `owner = id % (1,000,000 / m)`: a value's `m` rows are
spread over the whole relation, one every `1,000,000 / m` keys. A group is
one owner value's rows updated (`SET v = v + 1`). 36 groups an arm a cell (3
repeats of 12, the arm order rotated), the owner drawn afresh per group.

- **W** - `UPDATE du_t SET v = v + 1 WHERE owner = K`, one statement.
- **K** - `SELECT id FROM du_t WHERE owner = K`, then `UPDATE du_t SET v = v
  + 1 WHERE id IN (...)` over the ids returned.
- **W2** - W again: the noise control.

| rows a key | arm | groups/s (1 / p50) | server CPU a group | rows written a group | saving against W |
|---|---|---|---|---|---|
| 10 rows | W | 30.1 groups/s | 33,092 µs | 10 rows | 1.0x |
| 10 rows | K | 10,917 groups/s | 107 µs | 10 rows | 308x |
| 10 rows | W2 | 30.0 groups/s | 33,198 µs | 10 rows | 1.00x (the floor: 0.3 %) |
| 1,000 rows | W | 29.4 groups/s | 35,212 µs | 1,000 rows | 1.0x |
| 1,000 rows | K | 365.5 groups/s | 2,738 µs | 1,000 rows | 12.9x |
| 1,000 rows | W2 | 29.6 groups/s | 33,702 µs | 1,000 rows | 1.04x (the floor: 4.3 %) |

Every group of every arm wrote exactly `m` rows, checked per group, and the
relation's `SUM(v)` after the cell is the rows all three arms wrote (1,110
and 111,000, the warm pass's three groups included).

**The group's latency**, client-side, one connection:

| rows a key | arm | p0 | p25 | p50 | p75 | p99 | max |
|---|---|---|---|---|---|---|---|
| 10 rows | W | 32,676 µs | 32,969 µs | 33,184 µs | 33,282 µs | 33,761 µs | 33,761 µs |
| 10 rows | K | 77 µs | 84 µs | 92 µs | 109 µs | 610 µs | 610 µs |
| 10 rows | W2 | 32,938 µs | 33,153 µs | 33,298 µs | 33,616 µs | 33,999 µs | 33,999 µs |
| 1,000 rows | W | 33,661 µs | 33,867 µs | 33,995 µs | 34,217 µs | 80,715 µs | 80,715 µs |
| 1,000 rows | K | 2,347 µs | 2,555 µs | 2,736 µs | 3,033 µs | 8,291 µs | 8,291 µs |
| 1,000 rows | W2 | 33,596 µs | 33,727 µs | 33,803 µs | 33,947 µs | 64,317 µs | 64,317 µs |

With 36 groups an arm the p99 is the maximum. The W and W2 maxima of the
1,000-row cell (80.7 ms and 64.3 ms against a 34 ms median) are single
groups the other session's build took the CPU from; the on-CPU unit above
does not carry them.

## 2. What the two statements of K are

`ANALYZE SELECT id FROM du_t WHERE owner = 7`, as the engine printed it in
each cell:

```
step 0 IndexProbe du_t opens=1 examined=10 matched=10 sel=100% pages=11 index_scanned=10 index_resolved=10
step 0 IndexProbe du_t opens=1 examined=1000 matched=1000 sel=100% pages=1001 index_scanned=1000 index_resolved=1000
```

The read ladder already takes the index for this `WHERE`; the write of the
same `WHERE` does not ask it. K's `UPDATE` is CU-S3's key set write: one
descent a listed id.

## 3. Waits

Not applicable beyond the split above: one connection, `relaxed`
durability, no lock wait and no device read in either arm (the relation is
resident: 82.8 MB of data file under a 1,024 MB pool). A group's latency is
its server CPU plus the client's round trips - one for W, two for K.

## 4. What was cut, and what this file does not measure

- Two selectivities, one relation size, one core. No cell at `cores > 1`
  and none under a concurrent writer: the premise is a cost, not a
  concurrency claim.
- No `DELETE` cell. Its walk is `UPDATE`'s (`WriteWalk`).
- The crossing in the thesis is computed from the two cells, not measured:
  no cell writes more than 0.1 % of the relation.
- The host was not quiet (the stamp). The bar is a ratio of 5 and the
  measured ratios are 12.9 and 308, against a control that moved 4.3 % at
  most; the verdict does not depend on the contention, and the absolute
  microseconds may.
- Time spent: about 2 minutes of building and 1 minute of running, inside
  the stage's 20-minute budget; no quiet-gate hold was taken.
