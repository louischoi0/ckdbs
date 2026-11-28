# EB-S0 premise: every key-form family costs 2.6x to 20,000x its proxy today, and an embedded key's join cannot run at 1,000,000 rows

**Thesis.** At `b761306e0` (`v3.4.17-225-gb761306e0`) each shape EB names
was run against a **proxy**: the cheapest statement or schema that returns
the same rows through an arm the engine already has. The proxy stands in
for the arm EB would add, the same way DU-S0's two-statement form did. It
is an estimate of the gain, not a measurement of the built arm. Every
range, order and whole-key join shape costs **13x to 20,422x** its proxy in
server CPU a statement. The nearest the bar is **J3, an embedded key's
literal prefix with a joined column: 2.77x at 100,000 rows, and 1.54x at
1,000,000 as measured, about 2.6x once a cost added to every arm alike is
taken out** (below). The join planner serves it today by reordering the
block (a tuple key range on the embedded relation, then a build over the
outer side).

**EB-Q1 (a) as adopted is a family bar.** Read against it:

- **S1 ranges** pass (13.5x to 391x).
- **S2 joins** pass on J1 and J2. J1 is 1,286x at 100,000 rows and 20,422x
  at 1,000,000. J2 is 4,554x at 100,000 rows and is **refused** at
  1,000,000 by the row-touch budget. **J3 is built with S2**: as measured
  at 1,000,000 rows it read under the bar, but the reading did not survive
  the review - the 1.54x carries about 95 us added to every arm, and the
  estimate without it is 2.6x - and the bar is the family's, which S2
  passes. J3's shape pins every column of the embedded key, one by a
  literal and one by the outer row, so S2's computed id serves it once the
  whole key outranks its literal prefix's window.
- **S3 order** passes (263x and 2,546x).
- **S4 upserts** is a refusal family, which the 2x bar cannot price. The
  shapes are refused today, and what an operator writes instead is an
  explicit transaction that reads and then writes. **CLA's reading: S4 is
  built.**

| field | value |
|---|---|
| date executed | 2026-11-27, part 1 21:51:47-21:58:36 UTC, part 2 about 21:58:50-21:59:47 UTC |
| branch / commit | `worktree-eb-key-forms-in-the-executor` at `b761306e0`, `git describe --tags` = `v3.4.17-225-gb761306e0`. The build inputs are `9e7597d5d`'s; `b761306e0` adds only the driver |
| work order | `instructions/v3.2.0/workorder-eb-key-forms-in-the-executor.md` §5, EB-S0; EB-Q1 (a) |
| build | Release (`cmake -DCMAKE_BUILD_TYPE=Release`), built in this worktree at 21:49:44 UTC before any EB code was written |
| binary | a copy, `build-release/eb-s0-run/kds_server.measured`, sha256 `c4ad5c210a12130abd264a5e7efa8750056be9a207a595377a60f76d48a34c84`; one binary for both parts |
| server | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 262144`, `join_order = planned`, pinned to CPU 2 (`taskset`), port 31993; one fresh server and data file per row count |
| driver | `tools/eb_s0_premise.py`, one connection. Part 2 re-ran J2, J3 and O1 at 1,000,000 rows (`--cells`) after part 1 stopped on J2's refusal; the refusal-capture and `--cells` edits are in the commit that carries this file |
| unit | server on-CPU time a statement, summed over the server's threads (`/proc/<pid>/task/*/schedstat`); client latency percentiles beside it |
| design | per cell: T (today's shape), B (the proxy), T2 (T again, the noise floor); 3 repeats, the order reversed on alternate repeats; 40 statements an arm a repeat, or fewer where an arm passed 3 s (never under 3); one draw sequence for every arm, and the rows a statement returned asserted equal between arms |
| host | **not quiet**: load average 1.34 to 3.04 at the arms, the host shared with other sessions; this session's Debug build ran on CPUs 4-23, off the server's CPU, from part 2's load on. The unit is on-CPU time, which a contended CPU inflates less than a latency, and T2 bounds the noise between runs of one statement. **Not recorded, against `bench/README.md` rules 2 and 4**: `df -T` of the data file at run time and `pgrep -a -f "cc1plus\|cmake --build\|ctest"` per cell (the driver writes `/proc/loadavg` only). At review, `df -T build-release/eb-s0-run` reports `/dev/mapper/fedora-root`, xfs |
| what was cut | the hashed key form and a declared-Keystone heap: no family's mechanism branches on them at S0 (S1-S4 add their cells to the contract suites instead); under the 3 s budget the T and T2 arms of J1 ran 39 statements at 100,000 rows and 9 at 1,000,000, and J2's ran 42 at 100,000, not 120. B ran 120 everywhere |
| prior number | DR-S0 (`results-dr-s0-premise-v3.5.0-114-ge9df68d02.md`, M3): `key >= x ORDER BY key LIMIT 100` at 100,000 rows was 5,821 us on a heap and 6,730-9,041 us on an index, against 2,340 us and 2,767 us here. Those are other shapes (a heap of other columns, at another commit), so no delta is claimed |
| PostgreSQL floor | not measured |
| raw output | `bench/v3.5.0/archive/eb-s0-premise-v3.4.17-225-gb761306e0/out-part1.json`, `out-part2.json` (the plans `ANALYZE` named for every arm are in them) |

## Relations

`n` rows each, keys dense `1..n`:

| relation | definition |
|---|---|
| `bt` | `(id int64, k int64, v int64)` BTREE, index `bt_k (k)`, `k` a permutation of `id` |
| `hp` | `(k int64, v int64)` HEAP, a hidden Keystone, `keyidentity` `1..n` |
| `e` | `(a int32, b int16, v int64, PRIMARY KEY (a, b))`, the embedded form, `b` in `0..999` |
| `c` | `(id, a, b, v)` with index `c_ab (a, b)`, `a = id % 10` |
| `c1` | the twin of `c` with index `c1_ba (b, a)`, its selective column leading |
| `o` | 100 rows, the outer side |

## Cells

Server CPU a statement, in us; the ratio is T over B. T2 was within 1.31 %
of T in every cell (the widest, R3 at 1,000,000 rows). T2 repeats T, so it
bounds noise that differs between two runs of one statement, not a cost
added to every arm alike (J3 at 1,000,000 rows, below).

| cell | family | T, today | B, the proxy | 100,000 rows | 1,000,000 rows |
|---|---|---|---|---|---|
| R1 page forward, btree tuple key | S1 | `id >= x ORDER BY id LIMIT 100`: a filtered walk from the head | `... AND id <= 2^48-1`: a range | 502 us / 16.3 us = **30.8x** | 5,070 us / 20.7 us = **245x** |
| R2 page backward, btree tuple key | S1 | `id < x ORDER BY id DESC LIMIT 20`: a reverse walk from the top | `id >= 1 AND id <= x-1`: a seeded reverse range | 529 us / 10.5 us = **50.4x** | 5,985 us / 15.3 us = **391x** |
| R3 page forward, heap Keystone | S1 | `keyidentity >= x ORDER BY keyidentity LIMIT 100`: a walk and a sort | `keyidentity IN (x .. x+99) ORDER BY keyidentity`: a key set | 2,340 us / 122 us = **19.3x** | 25,257 us / 154 us = **164x** |
| R3s strict window, heap Keystone | S1 | `keyidentity > x-1 AND keyidentity < x+100`: a walk | the inclusive window: a Keystone index range | 1,053 us / 29.9 us = **35.2x** | 11,415 us / 34.8 us = **328x** |
| R4s strict window, index leading key | S1 | `k > x-1 AND k < x+100`: a walk | the inclusive window: an index range | 1,468 us / 55.8 us = **26.3x** | 15,437 us / 97.8 us = **158x** |
| R4 page forward, index leading key | S1 | `k >= x ORDER BY k LIMIT 100`: a walk and a sort | `k IN (x .. x+99) ORDER BY k`: an index key set | 2,767 us / 206 us = **13.5x** | 28,080 us / 236 us = **119x** |
| J1 join by a whole composite index key | S2 | `o JOIN c ON c.a = o.a WHERE c.b = o.b`: a probe on `a` alone, its run of `n/10` filtered | `c1` entered by `b`, then `a` filtered | 240,430 us / 187 us = **1,286x** | 2,767,186 us / 136 us = **20,422x** |
| J2 join by a whole embedded key | S2 | `o JOIN e ON e.a = o.ea WHERE e.b = o.eb`: a walk of `e` per outer row, no build (two correlated conjuncts) | `o JOIN bt ON bt.id = o.x`: a tuple key probe | 223,136 us / 49.0 us = **4,554x** | **refused**: past the 100,000,000-tuple row-touch budget |
| J3 an embedded key's literal prefix and a joined column | S2 | `o JOIN e ON e.b = o.eb WHERE e.a = A`: reordered, a range of `e` then a build | as J2's B | 127 us / 45.9 us = **2.77x** | 221 us / 144 us = **1.54x** |
| O1 order by an embedded key | S3 | `SELECT a, b, v FROM e ORDER BY a, b LIMIT 100`: a walk and a top-N | `bt ORDER BY id LIMIT 100`: elided | 3,887 us / 14.8 us = **263x** | 39,467 us / 15.5 us = **2,546x** |

Client latency p50 followed the CPU in every cell, for example R1 at
1,000,000 rows was 5,431 us against 55 us, and J1 at 1,000,000 rows was
2.77 s against 162 us.

**J3 at 1,000,000 rows carries a cost added to every arm.** Its plans are
the 100,000-row plans page for page (`ANALYZE`: T examined 1,238 rows over
105 pages, B 200 over 101, at both counts), yet T rose 127 us to 221 us and
B 45.9 us to 144 us, a rise of 94 us and 98 us: the T - B gap stayed 81 us
and 78 us. All three arms' latencies are bimodal at 1,000,000 rows: T p25 166 us and p50 299 us, B p25 88 us and p50 224 us, T2 p25
168 us and p50 299 us, against 161 us / 163 us, 80 us / 80 us and
161 us / 163 us at 100,000 rows. The lower mode matches 100,000 rows.
Part 2 ran at load average 3.04, the highest of the run. The 1.54x is
therefore not a statement cost. With the added 95 us taken out of both arms
the ratio is about 2.6x. Estimate: 2.6x (basis: the constant T - B gap and
the lower latency mode, confidence: medium). Unverified: what the added
cost is. A re-run of J3 alone on a quiet host would check it.

## S4: what is refused today

| statement | answer at `b761306e0` |
|---|---|
| `INSERT INTO c VALUES (...) ON CONFLICT (a, b) DO NOTHING` (no unique index on `(a, b)`) | refused: "a conflict target of more than one column is not supported (byte 48); the tuple key is the one conflict there is" |
| `INSERT INTO e VALUES (...) ON CONFLICT (a, b) DO NOTHING` | the same refusal (byte 45) |
| `INSERT INTO e VALUES (...) ON CONFLICT DO NOTHING` | refused: "ON CONFLICT at byte 31 on a relation whose primary key is its Keystone id is not built" |
| `INSERT INTO hp (keyidentity, k, v) VALUES (1, ...) ON CONFLICT (keyidentity) DO NOTHING` | **admitted**: `INSERTED ... rows=0`, against `heap-and-tuple.md` §3.1b (EB-Q4) |

## What the numbers say about the engine

- **A bound with an open end costs the whole relation today, and the
  cost grows linearly with it.** R1, R2, R3 and R4 each grew about 10x
  between 100,000 and 1,000,000 rows. Their proxies stayed between 10 us
  and 240 us, because a statement that names its end walks only the rows
  it returns.
- **A strict end is the same cost as no end.** R3s and R4s write a window
  bounded at both ends and pay the walk. Their inclusive twins already
  take the index range, so the only gap is the spelling, which is the
  thing `TupleKeyRangeOf`'s comment says an optimizer must not reward.
- **The worst cells are joins: each reads an inner run per outer row.**
  J1 descends by the composite key's first column and filters a run of
  `n/10` entries per outer row. Over `o`'s 100 rows that is 1,000,000
  index entries resolved a statement at 100,000 rows and 10,000,000 at
  1,000,000 rows (`ANALYZE`'s `index_resolved`), where entering by the
  whole key resolves one per outer row. J2 has no index at all: the embedded key is the
  tuple key, and nothing computes the id from the outer row. At 1,000,000
  rows it is refused, so the statement **cannot be run**, not merely run
  slowly.
- **The planner already rescues J3.** It reorders the block so that the
  embedded relation's literal prefix is read once and the outer side is
  bucketed. That is 2.77x a tuple key probe per outer row at 100,000
  rows. The 1.54x at 1,000,000 rows carries a cost added to both arms
  (above), and about 2.6x is the estimate without it.

## Not measured

- The arms EB builds. The proxies estimate them, and S1-S4 measure
  nothing of their own. The milestone's close measures fixed costs, per
  `CLAUDE.md`'s Session Workflow.
- The hashed key form, a declared-Keystone heap, and more than one core.
