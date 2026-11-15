# Audit F4 overhead: the common write path costs nothing; the new snapshot-WHERE path costs about 120 ns per skipped row, and only where an undecided writer holds the row

**Thesis.** `4f5b2e17` (`v3.4.10-1-g4f5b2e17`) makes an UPDATE/DELETE row walk read the statement snapshot's version, through the undo chain,
when the row's page version was written by a transaction the view cannot see, is undecided (or the session is REPEATABLE READ), and fails the WHERE.
Measured as an interleaved A/B in Release against `8d3adccc` (`v3.4.10`), **the common path is unchanged**: a non-key `UPDATE ... WHERE w = k` and a
point `UPDATE ... WHERE id = k`, at 200, 1,000 and 10,000 rows, differ by at most 0.9 µs of server CPU per statement (1.4 %), inside the 1.9 %
spread of a second server running A's own binary; one cell (the 10,000-row scan) reads B 3.3 % *faster*, which is not a cost. **The new path costs
+120 to +129 ns of server CPU per row the walk reaches whose current version is an open transaction's**: a statement over 10,000 such rows goes
from 0.78 ms to 2.01 ms of server CPU (1,149 to 465 QPS), the same for UPDATE and DELETE, at all three sizes. The extra is a fixed per-row cost
(it does not grow with the row count), which is what the structure predicts: one `IsInFlight` query, one undo-page read under the leaf `X`, one tuple decode and one WHERE
evaluation per row. Both builds answer `UPDATED 0` / `DELETED 0` in every statement of every cell (0 mismatches in about 131,000 statements per arm), so the
difference is cost only.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-11-15, 05:42:09 to 05:43:18 UTC (the 10-run measurement; about 70 s). Builds 05:38-05:40. About 10 minutes in all from the first command, inside the one-hour budget |
| Worktree / branch | `/home/louis/ckdbs/.claude/worktrees/audit-f4-where-on-snapshot` on `worktree-audit-f4-where-on-snapshot`; builds from `git archive` exports of the two exact commits into `/home/louis/bench-runs/f4/srcA` and `srcB` (no `.git`, so neither binary can be dirty). Tree clean at the time of the run |
| **A (baseline)** | `8d3adccc` = `v3.4.10`, committed 2026-11-15 04:54:00 UTC. Release (`-DCMAKE_BUILD_TYPE=Release`, `--target kds_server`), binary mtime 05:40:11 UTC (after the commit). Copy `kds_server-A`, 7,574,704 bytes, sha256 `9094294ea6298307b30a91bc0c261cef9d3a6f1ebadf80c2b61d1868e5edabe6` |
| **B** | `4f5b2e17` = `v3.4.10-1-g4f5b2e17`, committed 2026-11-15 05:37:19 UTC. Release, binary mtime 05:40:12 UTC (after the commit). Copy `kds_server-B`, 7,578,800 bytes, sha256 `93bfb49cee75c27de2fb1a54c99a4b7cdd1bc228446390ee4bb5b431bfebeae2`. The diff A to B in `src/` and `include/` is `command_dispatcher.cpp` (`WriteWalk::Qualify`, `Claim`) plus comment-only edits to two headers |
| Noise control | **A2**: a third server started from a byte-identical copy of A's binary (same sha256), one per run, taking the same blocks in the rotation. A2 minus A prices process placement and host drift; it cannot price code layout, which differs between any two builds |
| Binary discipline | every server of every cell started from the copies in `/home/louis/bench-runs/f4/run/` (on `/dev/nvme0n1p1`, ext4 as it happens); the build trees were not touched after the copy |
| Device | data files and WAL under `/home/louis/bench-runs/f4/run/w/` on `/dev/nvme0n1p1`, ext4 (`df -T`) |
| Host | 8 vCPU Intel Xeon Platinum 8581C @ 2.10 GHz, Linux 6.12.116. One-minute load before the run 1.52 (the decaying average of the builds, which had exited); `/proc/loadavg` and `pgrep cc1plus` sampled every 60 s by the watchdog (`hostload.log`: no compiler process in any sample; loads 1.2-1.5). Server pinned to CPU 2, driver to CPU 4 |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, everything else default. Fresh server and data file per run per arm; three servers side by side, one busy at a time, the order of every block rotating one place per block and per run. Ports 31970-31972 |
| Driver | `bench/v3.4.10/archive/f4-overhead-v3.4.10-1-g4f5b2e17/scripts/f4_ab.py`, a scratch driver importing `tools/bn_census.py` (server, block runner, server on-CPU time from `/proc/<pid>/task/*/schedstat`) and `tools/kwp.py` unmodified; `analyze.py` and `tables.py` made this file's numbers. KWP, the literal (unnamed PARSE) path only |
| Prior numbers | **none for these shapes**: no earlier results file in `bench/` measures a non-key scan `UPDATE`, or a `WHERE`-failing write over an undecided writer's rows. This run is the baseline the next one is read against. PostgreSQL floor: **not measured for this shape** |
| Raw outputs | `bench/v3.4.10/archive/f4-overhead-v3.4.10-1-g4f5b2e17/` (`full.json`, `analysis.txt`, progress and host logs, scripts; no data file, WAL segment or server log) |
| Tests | no engine code was changed by this measurement. **No test suite was run by it**; the commit's own tests (`tests/lock_family_test.cpp`) are the coordinator's |

## 2. What was measured

`UPDATE t SET v = v + 1`, `UPDATE ... SET v = 0` and `DELETE` over `BTREE` relations of `id, v, w` int64, loaded `v = w = id`. The row count is the table's
(200, 1,000, 10,000; the mapping from the flag is simply the relation loaded: `t<N>`, `u<N>`, `d<N>`). Every statement is one autocommit statement
over one KWP connection; the unit measured is the statement.

| Cell | Statement | What the walk does per row | Expected reply |
|---|---|---|---|
| 1, scan | `UPDATE t<N> SET v = v + 1 WHERE w = ?` (`w` unindexed, one row matches) | the per-row path over the whole relation, no tuple-key window; every row but one fails the WHERE on a version the view can see | `UPDATED 1` |
| 1, point | `UPDATE t<N> SET v = v + 1 WHERE id = ?` | tuple-key descent to one row (size-independent; the three sizes are the evidence of it) | `UPDATED 1` |
| 2, update | `UPDATE u<N> SET v = 0 WHERE v = -1` while a second session, in `BEGIN` and never committed during the cell, has run `UPDATE u<N> SET v = -5` over all N rows | every row's page version is the open transaction's (`v = -5`, fails) and so is its snapshot version (`v = i`, fails): A skips at the page version, B also reads the undo version | `UPDATED 0` |
| 2, delete | `DELETE FROM d<N> WHERE v = -1`, the same holder | the same | `DELETED 0` |

Each (cell, size) is measured with 6 blocks per arm per run, the same statement numbers in every arm, 10 runs. The holder transaction is opened in each of the
three servers before the cell, is checked to be still open after the cell (the update holder reads its own `v = -5`; the delete holder is only checked to answer), and is rolled back; the cells fit in about 1 s, far inside the 60 s `txn_lifetime_ceiling_ms`
(the driver aborts if one takes over 45 s). `bad` counts a reply with the wrong tag: **0 in every cell, both builds**.

## 3. Cell 1: the common path is unchanged

The finding first: in five of the six shapes B and A differ by less than 1 µs, and the A2 control, the same binary on another server, differs from A by as much
(+0.71 µs on the 200-row scan, against B's +0.53 µs). The one row that stands apart is the 10,000-row scan: B is 23 µs (3.3 %) *cheaper* than A in all ten runs,
where A2 is 4.6 µs cheaper (0.6 %). It is not a cost and I do not attribute it to a change: the diff adds one stored enum per row and a branch, which cannot remove 2.3 ns per
row, so a code-layout difference between the two binaries is the likelier reading, and this run does not isolate it (the 200- and 1,000-row scans, which run the same
per-row code, show no such gap). The point `UPDATE` is constant at 22 µs across the three table sizes on both builds, which is the evidence that it does not scale with rows.

Server on-CPU time is the sum over all server threads per statement; the client-observed latency adds the socket round trip (about 20 µs for a 22 µs statement). No further wait
decomposition applies: with one session, `relaxed` durability and no open writer, there is no fsync, lock or conflict wait in these statements.

## Results

### Cell 1: server CPU per statement

| Shape | Rows | Stmts per run per arm | A CPU, median [min, max] over 10 runs | A2 (control) | B | B minus A | A2 minus A (noise) | Runs with B < A |
|---|---|---|---|---|---|---|---|---|
| c1-scan-update-200 | 200 | 1,500 | 38.06 µs [37.60, 40.38] | 38.77 µs [37.52, 39.39] | 38.60 µs [38.32, 39.31] | +0.53 µs (+1.4 %, +2.7 ns/row) | +0.71 µs (+1.9 %) | 4 / 10 |
| c1-point-update-200 | 200 | 3,000 | 22.02 µs [21.41, 23.14] | 22.12 µs [21.49, 23.17] | 22.20 µs [21.38, 23.09] | +0.18 µs (+0.8 %, +0.9 ns/row) | +0.10 µs (+0.4 %) | 4 / 10 |
| c1-scan-update-1000 | 1,000 | 600 | 94.43 µs [91.72, 100.62] | 94.22 µs [92.10, 100.15] | 94.17 µs [93.14, 100.06] | -0.25 µs (-0.3 %, -0.3 ns/row) | -0.21 µs (-0.2 %) | 6 / 10 |
| c1-point-update-1000 | 1,000 | 3,000 | 22.23 µs [21.62, 22.99] | 21.94 µs [21.61, 23.22] | 22.07 µs [21.57, 23.12] | -0.17 µs (-0.7 %, -0.2 ns/row) | -0.29 µs (-1.3 %) | 7 / 10 |
| c1-scan-update-10000 | 10,000 | 120 | 712.45 µs [703.83, 742.33] | 707.84 µs [703.87, 743.03] | 689.18 µs [682.10, 719.92] | -23.28 µs (-3.3 %, -2.3 ns/row) | -4.61 µs (-0.6 %) | 10 / 10 |
| c1-point-update-10000 | 10,000 | 3,000 | 22.28 µs [21.85, 23.68] | 22.33 µs [21.85, 23.67] | 22.44 µs [21.92, 22.76] | +0.15 µs (+0.7 %, +0.0 ns/row) | +0.04 µs (+0.2 %) | 3 / 10 |

### Cell 1: client-observed latency and throughput (median over 10 runs; throughput derived as 1,000,000 / mean µs)

| Shape | Arm | p0 | p25 | p50 | p75 | p99 | Throughput |
|---|---|---|---|---|---|---|---|
| c1-scan-update-200 | A | 52.8 µs | 56.9 µs | 57.8 µs | 60.8 µs | 99.4 µs | 16,075 QPS |
| c1-scan-update-200 | B | 54.7 µs | 57.2 µs | 58.2 µs | 61.9 µs | 98.0 µs | 15,911 QPS |
| c1-point-update-200 | A | 36.2 µs | 41.9 µs | 42.6 µs | 44.7 µs | 66.1 µs | 21,900 QPS |
| c1-point-update-200 | B | 36.7 µs | 42.1 µs | 42.7 µs | 45.0 µs | 66.3 µs | 21,771 QPS |
| c1-scan-update-1000 | A | 106.4 µs | 110.9 µs | 113.5 µs | 119.2 µs | 200.4 µs | 8,339 QPS |
| c1-scan-update-1000 | B | 104.8 µs | 109.9 µs | 113.2 µs | 119.2 µs | 217.4 µs | 8,295 QPS |
| c1-point-update-1000 | A | 35.7 µs | 42.0 µs | 42.7 µs | 44.6 µs | 67.1 µs | 21,687 QPS |
| c1-point-update-1000 | B | 36.4 µs | 41.9 µs | 42.6 µs | 44.5 µs | 66.9 µs | 21,842 QPS |
| c1-scan-update-10000 | A | 698.2 µs | 744.8 µs | 773.6 µs | 814.0 µs | 1032.5 µs | 1,259 QPS |
| c1-scan-update-10000 | B | 671.7 µs | 716.0 µs | 749.5 µs | 789.3 µs | 1036.9 µs | 1,295 QPS |
| c1-point-update-10000 | A | 36.1 µs | 42.3 µs | 43.0 µs | 44.9 µs | 66.4 µs | 21,770 QPS |
| c1-point-update-10000 | B | 36.5 µs | 42.3 µs | 43.1 µs | 45.1 µs | 65.3 µs | 21,789 QPS |

### Cell 2: server CPU per statement

| Shape | Rows | Stmts per run per arm | A CPU, median [min, max] over 10 runs | A2 (control) | B | B minus A | A2 minus A (noise) | Runs with B < A |
|---|---|---|---|---|---|---|---|---|
| c2-update-200 | 200 | 600 | 35.64 µs [34.92, 42.07] | 35.84 µs [34.73, 37.54] | 61.49 µs [60.72, 64.88] | +25.85 µs (+72.5 %, +129.2 ns/row) | +0.20 µs (+0.6 %) | 0 / 10 |
| c2-delete-200 | 200 | 600 | 34.29 µs [33.79, 35.33] | 33.96 µs [33.53, 35.07] | 59.80 µs [59.46, 64.63] | +25.51 µs (+74.4 %, +127.5 ns/row) | -0.33 µs (-1.0 %) | 0 / 10 |
| c2-update-1000 | 1,000 | 300 | 99.14 µs [97.06, 103.96] | 98.74 µs [97.02, 104.33] | 222.13 µs [219.79, 243.48] | +122.99 µs (+124.1 %, +123.0 ns/row) | -0.39 µs (-0.4 %) | 0 / 10 |
| c2-delete-1000 | 1,000 | 300 | 95.91 µs [94.11, 100.56] | 96.03 µs [93.46, 98.67] | 216.65 µs [213.71, 221.55] | +120.74 µs (+125.9 %, +120.7 ns/row) | +0.12 µs (+0.1 %) | 0 / 10 |
| c2-update-10000 | 10,000 | 60 | 778.34 µs [768.32, 791.82] | 775.40 µs [765.15, 799.47] | 2007.16 µs [1984.47, 2047.93] | +1228.82 µs (+157.9 %, +122.9 ns/row) | -2.93 µs (-0.4 %) | 0 / 10 |
| c2-delete-10000 | 10,000 | 60 | 755.76 µs [742.37, 779.02] | 757.45 µs [751.99, 786.22] | 1969.31 µs [1941.68, 2013.19] | +1213.55 µs (+160.6 %, +121.4 ns/row) | +1.69 µs (+0.2 %) | 0 / 10 |

### Cell 2: client-observed latency and throughput (median over 10 runs; throughput derived as 1,000,000 / mean µs)

| Shape | Arm | p0 | p25 | p50 | p75 | p99 | Throughput |
|---|---|---|---|---|---|---|---|
| c2-update-200 | A | 52.0 µs | 54.7 µs | 55.6 µs | 58.3 µs | 106.0 µs | 16,930 QPS |
| c2-update-200 | B | 77.0 µs | 79.4 µs | 80.8 µs | 84.8 µs | 151.8 µs | 11,347 QPS |
| c2-delete-200 | A | 51.0 µs | 53.0 µs | 54.0 µs | 57.0 µs | 99.1 µs | 17,426 QPS |
| c2-delete-200 | B | 75.1 µs | 77.6 µs | 79.0 µs | 83.2 µs | 134.4 µs | 11,979 QPS |
| c2-update-1000 | A | 108.6 µs | 114.5 µs | 117.7 µs | 123.9 µs | 244.6 µs | 7,960 QPS |
| c2-update-1000 | B | 236.9 µs | 251.4 µs | 267.4 µs | 302.5 µs | 418.6 µs | 3,537 QPS |
| c2-delete-1000 | A | 105.8 µs | 111.4 µs | 114.3 µs | 121.0 µs | 223.7 µs | 8,132 QPS |
| c2-delete-1000 | B | 231.7 µs | 246.6 µs | 262.8 µs | 297.1 µs | 397.7 µs | 3,603 QPS |
| c2-update-10000 | A | 754.9 µs | 804.9 µs | 840.8 µs | 898.1 µs | 1366.9 µs | 1,149 QPS |
| c2-update-10000 | B | 1961.8 µs | 2032.8 µs | 2077.9 µs | 2155.9 µs | 3811.2 µs | 465 QPS |
| c2-delete-10000 | A | 735.3 µs | 789.3 µs | 829.4 µs | 872.4 µs | 1086.7 µs | 1,190 QPS |
| c2-delete-10000 | B | 1938.0 µs | 2002.8 µs | 2043.1 µs | 2104.6 µs | 2446.9 µs | 480 QPS |

## 4. Cell 2: the new path costs about 120 ns per row, linear in rows, the same for UPDATE and DELETE

B minus A server CPU is +25.9 / +123.0 / +1,228.8 µs for UPDATE at 200 / 1,000 / 10,000 rows, which is +129 / +123 / +123 ns per row, and
+25.5 / +120.7 / +1,213.6 µs for DELETE (+128 / +121 / +121 ns per row). B is slower than A in **10 of 10 runs in all six rows**, the spread of either build across runs
is under 5 % of its median (A at 10,000 rows 768-792 µs; B 1,984-2,048 µs), and the A2 control sits within 0.4 % of A. The delta is therefore far outside the noise floor.
Per row the A side costs 78 ns (the walk, its page-version decode and the WHERE), so B takes a row of this kind from 78 ns to about 200 ns (2.6 times). Read as a fraction of
the statement it is +72 % at 200 rows, where the fixed cost of the statement (about 20 µs of request handling) dilutes it, and +158 % at 10,000 rows, where it does not.

What the 120 ns buys is the correctness fix: a `WHERE` that fails on the page version is no longer skipped when the snapshot's version of that row might pass. The statement
pays it only for rows whose current writer is undecided (or at every row under REPEATABLE READ, which this run did not measure), and the cost is not per statement but per such row: a hot table with a
long-lived open writer over most of its rows makes every non-key write scan of it about 2.6 times as expensive in server CPU, here 1,149 to 465 QPS at 10,000 rows. A decomposition by wait type does not
apply: the statement waits on nothing in either build (both answer `UPDATED 0` without a lock wait); the whole delta is CPU. Splitting the 120 ns among `IsInFlight`, the undo read, the decode and the re-evaluation
needs a profile this run did not take, so the share of each is **not measured**.

## 5. What this says about the engine

- The F4 change keeps the per-row hot path as it was: one stored enum is not measurable (cell 1, against a control that sees the same ±0.7 µs).
- The cost of reading through undo during a write walk is about 120 ns a row on this host, about 1.6 times the cost of the walk of that row without it. That is the price of the snapshot read the work order
  adds, and it is paid only where an undecided writer holds the rows, so it scales with write contention over a relation, not with its size.
- A caution about what this does not show: the cell chose rows that fail on **both** versions, so B never writes. A workload in which the snapshot version passes the WHERE changes behaviour (B updates or waits where A skipped)
  and is a different measurement. The row counts ran to 10,000; no larger table, no REPEATABLE READ session, no cores above one and no `strict` durability were measured.

## 6. What was cut

PostgreSQL floor (no twin for the shape). REPEATABLE READ (the path is entered for every row there, so it is a second cell). Prepared-statement path (literal only). More than one core and a second session on another core.
A profile splitting the 120 ns. The pilot of this driver (5 runs, A and B only) agreed with these numbers and is not kept.

Reproduce: `KDS_TOOLS=<repo>/tools python3 -I bench/v3.4.10/archive/f4-overhead-v3.4.10-1-g4f5b2e17/scripts/f4_ab.py --arm A=<A binary> --arm B=<B binary> --arm A2=<copy of A> --workdir <dir on ext4> --runs 10 --out full.json --progress progress`.
