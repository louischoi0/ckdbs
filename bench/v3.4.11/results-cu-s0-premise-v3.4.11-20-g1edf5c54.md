# CU-S0 premise: a value list as one walk against the same values as descents

A value list on the tuple key or on an indexed column is served today by a walk of the whole relation, and its server CPU grows with rows times list length. The same values run as single-value descents inside one statement cost two to three orders of magnitude less on a 100,000-row relation, and as separate statements 36 to 93 times less. **CU-Q0's bar (`instructions/v3.2.0/workorder-cu-key-set-access.md` section 4) is met**: at 100,000 rows a tuple key list of 4 values costs 9,110 µs of server CPU as today's walk, against 98.4 µs as four separate statements and an estimated 30.9 µs as a key set; a list of 64 values costs 73.3 ms, against 1,479 µs and an estimated 113 µs. No arm is a key set, which is unbuilt: the estimate is built from two measured arms and is described below. On relations of 10 and 100 rows the estimate says a key set is **slower** than the walk in 7 of 12 cells, by up to 3.7 ms on a 4,096-value list. The estimate leaves out CU-R1's check of the list on every row a set emits; with that check added at the walk's own measured cost per comparison, a set is also slower at 1,000 rows with 1,024 and 4,096 values, and the largest loss is about 12 ms (the re-check, below). Neither changes the bar's cells.

| field | value |
|---|---|
| date/time executed | 2026-11-16 UTC: 00:28-00:35 (1,000 and 100,000 rows), 00:35-00:37 (10 and 100 rows), 00:40-00:41 (1 row) |
| branch / commit | `worktree-cu-key-set-access` at `1edf5c54`; `git describe --tags` = `v3.4.11-20-g1edf5c54` |
| engine source | the worktree at `1edf5c54`. Its only uncommitted file at build time was the driver, `tools/cu_s0_premise.py`, which is no build input. Not a `git archive` export |
| binary | Release (`-DCMAKE_BUILD_TYPE=Release`), target `kds_server`, built 2026-11-16 00:25:22 UTC (after HEAD's commit time 00:19:04 UTC); measured copy `/home/louis/bench-runs/cu-s0/run/kds_server`, sha256 `d3d7f0940286de466bb02fab561e4bfa61addaafa629e6056bdc1547284a86cd`, the same copy in all three runs |
| device | data files and WAL under `/home/louis/bench-runs/cu-s0/data`, `data-small` and `data-one`, ext4 on `/dev/nvme0n1p1` (`df -T`), not tmpfs |
| server config | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, `join_order = written` (read by the J arm only), port 15631, defaults otherwise; one fresh server and data file per relation size; server pinned to CPU 4, the client to CPUs 0-3 |
| driver | `tools/cu_s0_premise.py`, new in this stage, archived as run under `scripts/`; one KWP connection, literal-text statements |
| host state | quiet. 8 CPUs (Xeon Platinum 8581C); 1-minute load average 0.35 to 1.34 over the three runs, 0 `cc1plus` processes in every arm run. Recorded per arm run in the archive's `result*.json` |
| archive | `bench/v3.4.11/archive/cu-s0-premise-v3.4.11-20-g1edf5c54/` (`result.json`, `result-small.json`, `result-one.json`, the logs, the driver, `tables.py` and `estimate.py`) |

## What was measured

Relation `cu_t (id int64, k int64, a int64, b int64) BTREE` with `k = 3 * id + 1` and a secondary index on `k`. A **group** is one list answered. Four arms per cell (column, rows, list length n):

- **L**: one statement, `SELECT * FROM cu_t WHERE <col> IN (v1, ..., vn)`. Today this is `Scan cu_t` with the list as a boolean residual (Plans, below).
- **D**: the same n values as n statements `SELECT * FROM cu_t WHERE <col> = v`, one after another: `Lookup` on `id`, `IndexProbe` on `k`. It pays n parses, n replies and n round trips that a key set would not.
- **J**: one statement that runs n descents. The n values are the rows of `cu_v_<n> (id, rid, rk)`, joined to `cu_t` on the column, which runs as a walk of those n rows and one `Probe` (tuple key) or `IndexProbe` of `cu_t` per row, the join equality kept as the probe's filter. It has no list to parse, type or sort, and its values are one fixed draw per cell.
- **L2**: L again, the noise control.

L's and D's values are drawn afresh per group, distinct, from `[1, max(rows, n)]`, so every value hits unless the list is longer than the relation. Each arm ran 2 repeats of 2.0 s (1.0 s in the 10-, 100- and 1-row runs), the arm order rotated and alternately reversed. Once per cell L's rows were compared with D's for one draw and with J's for the stored draw, row for row: equal in all 42 cells.

**Server CPU per list** is `utime + stime` of the server process (`/proc/<pid>/stat`, 10 ms ticks) over an arm run, divided by its groups. The bar is read on it: client latency carries D's n round trips, which are not the engine's.

**Noise floor.** `spread` = (max - min) / mean over the four L and L2 arm runs of a cell. An arm, or the estimate, is read faster or slower than L only when its difference from L's mean exceeds that spread.

**The key set estimate.** `estimate(rows, n) = P(n) + max(0, J(rows, n) - J(1))`, every term measured server CPU:

- `P(n)` is the L arm over a 1-row relation: one statement carrying the n-item list with next to nothing to walk. It prices the list itself (parsed, typed, compiled) on top of one statement.
- `J(rows, n) - J(1)` is the n descents with the statement's own cost taken off. `J(1)` is the mean of the column's n = 1 J cells.

It leaves out what a key set adds that neither arm runs: sorting and deduplicating the values, and CU-R1's check of the n-item list on every row the set emits, which J does not run (its residual is one equality). That check is priced separately below. It keeps J's walk of the n stored rows, which a key set does not run. It is an estimate of an unbuilt path, and CU-S4 measures the built one.

## Server CPU per list, measured

| column | rows | n | L walk | L2 walk | spread | D n statements | J n descents | L / J | L / D | reading |
|---|---|---|---|---|---|---|---|---|---|---|
| `id` | 1,000 rows | 1 value | 25.1 µs | 22.9 µs | 13 % | 24.2 µs | 29.6 µs | 0.8 times | 0.99 times | J slower, D inside noise |
| `id` | 1,000 rows | 4 values | 123 µs | 125 µs | 5 % | 105 µs | 33.2 µs | 3.7 times | 1.19 times | J faster, D faster |
| `id` | 1,000 rows | 64 values | 805 µs | 846 µs | 8 % | 1,434 µs | 86.5 µs | 9.5 times | 0.58 times | J faster, D slower |
| `id` | 1,000 rows | 1,024 values | 6,078 µs | 6,030 µs | 2 % | 24.5 ms | 887 µs | 6.8 times | 0.25 times | J faster, D slower |
| `id` | 1,000 rows | 4,096 values | 25.3 ms | 25.2 ms | 1 % | 87.0 ms | 2,202 µs | 11.5 times | 0.29 times | J faster, D slower |
| `id` | 100,000 rows | 1 value | 23.9 µs | 23.7 µs | 3 % | 24.1 µs | 30.2 µs | 0.8 times | 0.99 times | J slower, D inside noise |
| `id` | 100,000 rows | 4 values | 9,099 µs | 9,121 µs | 1 % | 98.4 µs | 37.9 µs | 240.7 times | 92.58 times | J faster, D faster |
| `id` | 100,000 rows | 64 values | 73.7 ms | 72.9 ms | 3 % | 1,479 µs | 88.0 µs | 832.5 times | 49.53 times | J faster, D faster |
| `id` | 100,000 rows | 1,024 values | 1.05 s | 1.04 s | 1 % | 23.9 ms | 1,135 µs | 919.9 times | 43.69 times | J faster, D faster |
| `id` | 100,000 rows | 4,096 values | 3.98 s | 3.98 s | 1 % | 102.1 ms | 4,208 µs | 946.4 times | 39.01 times | J faster, D faster |
| `k` | 1,000 rows | 1 value | 25.2 µs | 25.5 µs | 2 % | 25.4 µs | 30.9 µs | 0.8 times | 1.00 times | J slower, D inside noise |
| `k` | 1,000 rows | 4 values | 127 µs | 129 µs | 11 % | 114 µs | 44.6 µs | 2.9 times | 1.12 times | J faster, D inside noise |
| `k` | 1,000 rows | 64 values | 786 µs | 793 µs | 2 % | 1,728 µs | 203 µs | 3.9 times | 0.46 times | J faster, D slower |
| `k` | 1,000 rows | 1,024 values | 5,978 µs | 5,989 µs | 1 % | 25.8 ms | 2,675 µs | 2.2 times | 0.23 times | J faster, D slower |
| `k` | 1,000 rows | 4,096 values | 25.5 ms | 25.3 ms | 1 % | 112.6 ms | 12.8 ms | 2.0 times | 0.23 times | J faster, D slower |
| `k` | 100,000 rows | 1 value | 26.8 µs | 26.4 µs | 2 % | 26.2 µs | 29.2 µs | 0.9 times | 1.01 times | J slower, D inside noise |
| `k` | 100,000 rows | 4 values | 9,359 µs | 9,359 µs | 0 % | 103 µs | 38.5 µs | 242.8 times | 90.86 times | J faster, D faster |
| `k` | 100,000 rows | 64 values | 72.7 ms | 72.7 ms | 0 % | 1,745 µs | 204 µs | 355.9 times | 41.65 times | J faster, D faster |
| `k` | 100,000 rows | 1,024 values | 1.04 s | 1.04 s | 0 % | 27.7 ms | 3,085 µs | 337.9 times | 37.60 times | J faster, D faster |
| `k` | 100,000 rows | 4,096 values | 3.98 s | 4.05 s | 4 % | 111.1 ms | 11.8 ms | 339.5 times | 36.16 times | J faster, D faster |

The `reading` column compares J and D with L as measured, so J's reading leaves the list's own cost out; the estimate table below puts it back.

The n = 1 rows are the control: a one-item list is folded to the plain comparison by the parser, so L, L2 and D run the same statement. J there costs 2 to 7 µs more than L, which is its second step and its walk of one row.

At 100,000 rows the walk costs 91 ns per row walked at 4 values, 733 ns at 64, 10.4 µs at 1,024 and 39.8 µs at 4,096: about 10 ns per comparison, every row tested against every value it does not match. That is why a 1,024-value list takes 1.05 s of server CPU and a 4,096-value list 3.98 s.

Small relations, measured:

| column | rows | n | L walk | L2 walk | spread | D n statements | J n descents | L / J | L / D | reading |
|---|---|---|---|---|---|---|---|---|---|---|
| `id` | 10 rows | 4 values | 27.8 µs | 28.6 µs | 8 % | 93.7 µs | 28.5 µs | 1.0 times | 0.30 times | J inside noise, D slower |
| `id` | 10 rows | 64 values | 66.0 µs | 66.6 µs | 13 % | 1,340 µs | 57.7 µs | 1.1 times | 0.05 times | J inside noise, D slower |
| `id` | 10 rows | 4,096 values | 4,660 µs | 4,599 µs | 3 % | 86.7 ms | 1,617 µs | 2.9 times | 0.05 times | J faster, D slower |
| `id` | 100 rows | 4 values | 36.7 µs | 37.6 µs | 14 % | 86.1 µs | 30.0 µs | 1.2 times | 0.43 times | J faster, D slower |
| `id` | 100 rows | 64 values | 120 µs | 120 µs | 1 % | 1,370 µs | 68.1 µs | 1.8 times | 0.09 times | J faster, D slower |
| `id` | 100 rows | 4,096 values | 6,008 µs | 5,977 µs | 1 % | 88.0 ms | 1,824 µs | 3.3 times | 0.07 times | J faster, D slower |
| `k` | 10 rows | 4 values | 27.2 µs | 29.4 µs | 10 % | 95.8 µs | 36.2 µs | 0.8 times | 0.30 times | J slower, D slower |
| `k` | 10 rows | 64 values | 68.2 µs | 67.7 µs | 11 % | 1,544 µs | 73.2 µs | 0.9 times | 0.04 times | J inside noise, D slower |
| `k` | 10 rows | 4,096 values | 4,627 µs | 4,627 µs | 1 % | 91.0 ms | 2,041 µs | 2.3 times | 0.05 times | J faster, D slower |
| `k` | 100 rows | 4 values | 45.0 µs | 36.1 µs | 45 % | 110 µs | 37.5 µs | 1.1 times | 0.37 times | J inside noise, D slower |
| `k` | 100 rows | 64 values | 128 µs | 124 µs | 12 % | 1,724 µs | 130 µs | 1.0 times | 0.07 times | J inside noise, D slower |
| `k` | 100 rows | 4,096 values | 6,000 µs | 5,993 µs | 3 % | 113.8 ms | 5,744 µs | 1.0 times | 0.05 times | J faster, D slower |

One row (the L arm here is `P(n)`):

| column | rows | n | L walk | L2 walk | spread | D n statements | J n descents | L / J | L / D | reading |
|---|---|---|---|---|---|---|---|---|---|---|
| `id` | 1 rows | 1 value | 23.7 µs | 27.4 µs | 20 % | 24.4 µs | 37.8 µs | 0.7 times | 1.05 times | J slower, D inside noise |
| `id` | 1 rows | 4 values | 24.9 µs | 26.4 µs | 11 % | 88.9 µs | 35.0 µs | 0.7 times | 0.29 times | J slower, D slower |
| `id` | 1 rows | 64 values | 57.7 µs | 56.8 µs | 4 % | 1,339 µs | 49.7 µs | 1.2 times | 0.04 times | J faster, D slower |
| `id` | 1 rows | 1,024 values | 611 µs | 637 µs | 7 % | 22.0 ms | 409 µs | 1.5 times | 0.03 times | J faster, D slower |
| `id` | 1 rows | 4,096 values | 4,029 µs | 4,011 µs | 4 % | 85.0 ms | 1,532 µs | 2.6 times | 0.05 times | J faster, D slower |
| `k` | 1 rows | 1 value | 27.9 µs | 26.6 µs | 16 % | 26.7 µs | 33.9 µs | 0.8 times | 1.02 times | J slower, D inside noise |
| `k` | 1 rows | 4 values | 30.6 µs | 29.7 µs | 8 % | 95.0 µs | 34.6 µs | 0.9 times | 0.32 times | J slower, D slower |
| `k` | 1 rows | 64 values | 73.3 µs | 65.7 µs | 23 % | 1,418 µs | 55.8 µs | 1.2 times | 0.05 times | J inside noise, D slower |
| `k` | 1 rows | 1,024 values | 666 µs | 646 µs | 4 % | 25.3 ms | 451 µs | 1.5 times | 0.03 times | J faster, D slower |
| `k` | 1 rows | 4,096 values | 3,955 µs | 3,929 µs | 2 % | 91.0 ms | 1,618 µs | 2.4 times | 0.04 times | J faster, D slower |

## The key set estimate against the walk

| column | rows | n | L walk | spread | P list alone | descents, J - J(1) | key set estimate | L / estimate | reading |
|---|---|---|---|---|---|---|---|---|---|
| `id` | 10 rows | 4 values | 28.2 µs | 8 % | 25.6 µs | 0.0 µs | 25.6 µs | 1.10 times | set faster |
| `id` | 10 rows | 64 values | 66.2 µs | 13 % | 57.2 µs | 25.1 µs | 82.4 µs | 0.80 times | set slower |
| `id` | 10 rows | 4,096 values | 4,630 µs | 3 % | 4,020 µs | 1,584 µs | 5,604 µs | 0.83 times | set slower |
| `id` | 100 rows | 4 values | 37.1 µs | 14 % | 25.6 µs | 0.0 µs | 25.6 µs | 1.45 times | set faster |
| `id` | 100 rows | 64 values | 120 µs | 1 % | 57.2 µs | 35.5 µs | 92.8 µs | 1.29 times | set faster |
| `id` | 100 rows | 4,096 values | 5,992 µs | 1 % | 4,020 µs | 1,791 µs | 5,811 µs | 1.03 times | set faster |
| `id` | 1,000 rows | 4 values | 124 µs | 5 % | 25.6 µs | 0.6 µs | 26.3 µs | 4.73 times | set faster |
| `id` | 1,000 rows | 64 values | 826 µs | 8 % | 57.2 µs | 54.0 µs | 111 µs | 7.42 times | set faster |
| `id` | 1,000 rows | 1,024 values | 6,054 µs | 2 % | 624 µs | 855 µs | 1,479 µs | 4.09 times | set faster |
| `id` | 1,000 rows | 4,096 values | 25.2 ms | 1 % | 4,020 µs | 2,170 µs | 6,190 µs | 4.08 times | set faster |
| `id` | 100,000 rows | 4 values | 9,110 µs | 1 % | 25.6 µs | 5.3 µs | 30.9 µs | 294.58 times | set faster |
| `id` | 100,000 rows | 64 values | 73.3 ms | 3 % | 57.2 µs | 55.4 µs | 113 µs | 650.16 times | set faster |
| `id` | 100,000 rows | 1,024 values | 1.04 s | 1 % | 624 µs | 1,102 µs | 1,726 µs | 604.77 times | set faster |
| `id` | 100,000 rows | 4,096 values | 3.98 s | 1 % | 4,020 µs | 4,176 µs | 8,196 µs | 485.93 times | set faster |
| `k` | 10 rows | 4 values | 28.3 µs | 10 % | 30.2 µs | 4.9 µs | 35.1 µs | 0.81 times | set slower |
| `k` | 10 rows | 64 values | 67.9 µs | 11 % | 69.5 µs | 41.9 µs | 111 µs | 0.61 times | set slower |
| `k` | 10 rows | 4,096 values | 4,627 µs | 1 % | 3,942 µs | 2,010 µs | 5,952 µs | 0.78 times | set slower |
| `k` | 100 rows | 4 values | 40.5 µs | 45 % | 30.2 µs | 6.2 µs | 36.3 µs | 1.12 times | inside spread |
| `k` | 100 rows | 64 values | 126 µs | 12 % | 69.5 µs | 98.3 µs | 168 µs | 0.75 times | set slower |
| `k` | 100 rows | 4,096 values | 5,996 µs | 3 % | 3,942 µs | 5,713 µs | 9,655 µs | 0.62 times | set slower |
| `k` | 1,000 rows | 4 values | 128 µs | 11 % | 30.2 µs | 13.3 µs | 43.5 µs | 2.95 times | set faster |
| `k` | 1,000 rows | 64 values | 789 µs | 2 % | 69.5 µs | 172 µs | 241 µs | 3.27 times | set faster |
| `k` | 1,000 rows | 1,024 values | 5,983 µs | 1 % | 656 µs | 2,643 µs | 3,299 µs | 1.81 times | set faster |
| `k` | 1,000 rows | 4,096 values | 25.4 ms | 1 % | 3,942 µs | 12.7 ms | 16.7 ms | 1.52 times | set faster |
| `k` | 100,000 rows | 4 values | 9,359 µs | 0 % | 30.2 µs | 7.2 µs | 37.4 µs | 250.07 times | set faster |
| `k` | 100,000 rows | 64 values | 72.7 ms | 0 % | 69.5 µs | 173 µs | 242 µs | 299.83 times | set faster |
| `k` | 100,000 rows | 1,024 values | 1.04 s | 0 % | 656 µs | 3,054 µs | 3,710 µs | 281.01 times | set faster |
| `k` | 100,000 rows | 4,096 values | 4.02 s | 4 % | 3,942 µs | 11.8 ms | 15.7 ms | 255.16 times | set faster |

At 1,000 rows and above the estimate is faster than the walk in every cell: 1.5 to 7.4 times at 1,000 rows, 250 to 650 times at 100,000. That is without CU-R1's re-check, which turns the 1,000-row cells of 1,024 and 4,096 values to slower (below).

Below that it is not. On the tuple key the estimate loses at 10 rows from 64 values up (16 µs at 64 values, about 1.0 ms at 4,096) and wins or ties at 100 rows. On the indexed column it loses at 10 rows at every length and at 100 rows from 64 values up; the largest loss is 3.7 ms, a 4,096-value list over 100 rows (9,655 µs against 5,996 µs). The 4-value cells at 10 and 100 rows differ from the walk by 3 to 12 µs either way, on statements of 25 to 40 µs whose own spread is 8 to 45 %, and their descent term is J at 4 values minus J(1) from other servers' runs (floored at 0 on the tuple key), so they place the crossover no finer than that.

### The re-check the estimate leaves out (derived, not measured)

CU-R1 keeps the list in the residual, and `AcceptTupleAt` evaluates `bool_residual` on every row a step emits (`src/exec/step_vm.cpp:2406`). The `OR` is evaluated leaf by leaf in written order and stops at the first true (`EvaluateTri`, `src/exec/chain_frame.cpp:278-288`), so a matching row costs its value's position in the list, `(n + 1) / 2` comparisons on average, and a set emitting `h` rows pays about `h × (n + 1) / 2` comparisons. The walk's own cells give the cost of one: `(L − P(n))` divided by the comparisons the walk makes (`n` for each row that misses and `(n + 1) / 2` for each that matches) is 9.6 to 10.6 ns in every cell of 1,024 values and more from 100 rows up (11 to 14 ns at 64 values, where the walk's own per-row cost still shows). At 1,000 rows with 1,024 and 4,096 values every row matches (the list is all of `[1, n]`), so the walk's cost there *is* this check: 1,000 rows each tested against the list, which a set pays as well.

Adding `h × (n + 1) / 2 × 10 ns` to the estimate:

| column | rows | n | L walk | estimate | + re-check | L / (estimate + re-check) |
|---|---|---|---|---|---|---|
| `id` | 1,000 rows | 1,024 values | 6,054 µs | 1,479 µs | 6,604 µs | 0.92 times, slower |
| `id` | 1,000 rows | 4,096 values | 25.2 ms | 6,190 µs | 26.7 ms | 0.95 times, slower |
| `k` | 1,000 rows | 1,024 values | 5,983 µs | 3,299 µs | 8,424 µs | 0.71 times, slower |
| `k` | 1,000 rows | 4,096 values | 25.4 ms | 16.7 ms | 37.2 ms | 0.68 times, slower |
| `id` | 100 rows | 4,096 values | 5,992 µs | 5,811 µs | 7,860 µs | 0.76 times, slower |
| `k` | 100 rows | 4,096 values | 5,996 µs | 9,655 µs | 11.7 ms | 0.51 times, slower |
| `id` | 100,000 rows | 64 values | 73.3 ms | 113 µs | 134 µs | 549 times |
| `k` | 100,000 rows | 64 values | 72.7 ms | 242 µs | 263 µs | 276 times |
| `id` | 100,000 rows | 4,096 values | 3.98 s | 8,196 µs | 92.1 ms | 43 times |
| `k` | 100,000 rows | 4,096 values | 4.02 s | 15.7 ms | 99.7 ms | 40 times |

With the re-check a set is estimated slower in 8 of the 12 cells at 10 and 100 rows (the 100-row 4,096-value tuple key cell turns), and in the four 1,000-row cells of 1,024 and 4,096 values; the largest loss is 11.8 ms (`k`, 1,000 rows, 4,096 values), and at 100,000 rows a set is 40 to 550 times faster. The bar's cells move by under 21 µs. D stays above the estimate with its re-check in every cell measured (at 100,000 rows and 4,096 values, 102 ms against 92 ms on the tuple key and 111 ms against 100 ms on the index), so it remains an upper bound up to BU-Q2's 4,096 values, narrowly at the top.

What the list itself costs:

| column | n | P list alone (L over 1 row) | per item over one value's statement |
|---|---|---|---|
| `id` | 1 value | 25.6 µs | the single-value statement |
| `id` | 4 values | 25.6 µs | 0.02 µs |
| `id` | 64 values | 57.2 µs | 0.50 µs |
| `id` | 1,024 values | 624 µs | 0.58 µs |
| `id` | 4,096 values | 4,020 µs | 0.98 µs |
| `k` | 1 value | 27.2 µs | the single-value statement |
| `k` | 4 values | 30.2 µs | 0.98 µs |
| `k` | 64 values | 69.5 µs | 0.67 µs |
| `k` | 1,024 values | 656 µs | 0.61 µs |
| `k` | 4,096 values | 3,942 µs | 0.96 µs |

From 64 items up a list costs about 0.5 to 1 µs of server CPU per item before any row is read, and at 4,096 items that is 4 ms, as much as the 4,096 tuple key descents it names. The walk pays it too.

## Per descent

| column | rows | n | J CPU per value | D CPU per value | L CPU per row walked |
|---|---|---|---|---|---|
| `id` | 1,000 rows | 1 value | 29.65 µs | 24.25 µs | no walk (one value is keyed) |
| `id` | 1,000 rows | 4 values | 8.30 µs | 26.14 µs | 124 ns |
| `id` | 1,000 rows | 64 values | 1.35 µs | 22.40 µs | 826 ns |
| `id` | 1,000 rows | 1,024 values | 0.87 µs | 23.94 µs | 6,054 ns |
| `id` | 1,000 rows | 4,096 values | 0.54 µs | 21.24 µs | 25,225 ns |
| `id` | 100,000 rows | 1 value | 30.25 µs | 24.05 µs | no walk (one value is keyed) |
| `id` | 100,000 rows | 4 values | 9.46 µs | 24.60 µs | 91 ns |
| `id` | 100,000 rows | 64 values | 1.38 µs | 23.11 µs | 733 ns |
| `id` | 100,000 rows | 1,024 values | 1.11 µs | 23.33 µs | 10,438 ns |
| `id` | 100,000 rows | 4,096 values | 1.03 µs | 24.92 µs | 39,825 ns |
| `k` | 1,000 rows | 1 value | 30.85 µs | 25.40 µs | no walk (one value is keyed) |
| `k` | 1,000 rows | 4 values | 11.15 µs | 28.59 µs | 128 ns |
| `k` | 1,000 rows | 64 values | 3.17 µs | 27.00 µs | 789 ns |
| `k` | 1,000 rows | 1,024 values | 2.61 µs | 25.17 µs | 5,983 ns |
| `k` | 1,000 rows | 4,096 values | 3.12 µs | 27.50 µs | 25,385 ns |
| `k` | 100,000 rows | 1 value | 29.20 µs | 26.25 µs | no walk (one value is keyed) |
| `k` | 100,000 rows | 4 values | 9.64 µs | 25.75 µs | 94 ns |
| `k` | 100,000 rows | 64 values | 3.19 µs | 27.26 µs | 727 ns |
| `k` | 100,000 rows | 1,024 values | 3.01 µs | 27.08 µs | 10,425 ns |
| `k` | 100,000 rows | 4,096 values | 2.89 µs | 27.13 µs | 40,175 ns |

| column | rows | n | J CPU per value | D CPU per value | L CPU per row walked |
|---|---|---|---|---|---|
| `id` | 10 rows | 4 values | 7.11 µs | 23.41 µs | 2,823 ns |
| `id` | 10 rows | 64 values | 0.90 µs | 20.93 µs | 6,625 ns |
| `id` | 10 rows | 4,096 values | 0.39 µs | 21.16 µs | 462,962 ns |
| `id` | 100 rows | 4 values | 7.50 µs | 21.51 µs | 372 ns |
| `id` | 100 rows | 64 values | 1.06 µs | 21.40 µs | 1,197 ns |
| `id` | 100 rows | 4,096 values | 0.45 µs | 21.48 µs | 59,924 ns |
| `k` | 10 rows | 4 values | 9.06 µs | 23.95 µs | 2,832 ns |
| `k` | 10 rows | 64 values | 1.14 µs | 24.13 µs | 6,792 ns |
| `k` | 10 rows | 4,096 values | 0.50 µs | 22.22 µs | 462,735 ns |
| `k` | 100 rows | 4 values | 9.36 µs | 27.51 µs | 406 ns |
| `k` | 100 rows | 64 values | 2.03 µs | 26.94 µs | 1,260 ns |
| `k` | 100 rows | 4,096 values | 1.40 µs | 27.77 µs | 59,965 ns |

A tuple key descent inside a statement settles at 0.4 to 1.1 µs of server CPU per value once the list is long enough to drown the statement's fixed cost, and an index descent with its resolve at 2.6 to 3.2 µs at 1,000 rows and more (0.5 to 1.4 µs at 10 and 100 rows). The difference is not the hit ratio: the 1,000-row 4,096-value index cell is three quarters misses and costs 3.1 µs a value. A separate statement costs 21 to 29 µs per value whatever the column: almost all of D is the statement, not the descent.

## Latency per list, client side

One connection. p25 to p99 are the mean of the two repeats' percentiles; p0 and max are the extremes over both. The group count is the sample size: L at 100,000 rows has 4 groups at 1,024 values and 2 at 4,096, so its percentiles there are those few samples.

| column | rows | n | arm | groups | lists/s | p0 | p25 | p50 | p75 | p99 | max | rows/list |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `id` | 1,000 rows | 1 value | L | 58,685 groups | 15,808.7 lists/s | 49.5 µs | 53.5 µs | 56.4 µs | 74.2 µs | 102 µs | 3,389 µs | 1 rows |
| `id` | 1,000 rows | 1 value | L2 | 65,780 groups | 17,681.3 lists/s | 49.0 µs | 52.5 µs | 53.5 µs | 56.2 µs | 82.3 µs | 4,388 µs | 1 rows |
| `id` | 1,000 rows | 1 value | D | 61,020 groups | 16,429.8 lists/s | 48.3 µs | 51.9 µs | 53.8 µs | 71.5 µs | 98.3 µs | 8,283 µs | 1 rows |
| `id` | 1,000 rows | 1 value | J | 50,284 groups | 15,061.0 lists/s | 53.2 µs | 56.5 µs | 57.9 µs | 67.1 µs | 118 µs | 4,534 µs | 1 rows |
| `id` | 1,000 rows | 4 values | L | 23,574 groups | 6,110.9 lists/s | 144 µs | 152 µs | 155 µs | 164 µs | 254 µs | 1,038 µs | 4 rows |
| `id` | 1,000 rows | 4 values | L2 | 22,288 groups | 5,788.4 lists/s | 145 µs | 153 µs | 157 µs | 190 µs | 272 µs | 2,030 µs | 4 rows |
| `id` | 1,000 rows | 4 values | D | 14,081 groups | 3,631.8 lists/s | 195 µs | 211 µs | 262 µs | 299 µs | 643 µs | 9,142 µs | 4 rows |
| `id` | 1,000 rows | 4 values | J | 44,598 groups | 13,359.2 lists/s | 58.0 µs | 62.2 µs | 64.2 µs | 85.8 µs | 132 µs | 1,740 µs | 4 rows |
| `id` | 1,000 rows | 64 values | L | 3,990 groups | 1,030.0 lists/s | 834 µs | 907 µs | 941 µs | 980 µs | 1,516 µs | 6,997 µs | 64 rows |
| `id` | 1,000 rows | 64 values | L2 | 3,701 groups | 956.2 lists/s | 833 µs | 916 µs | 958 µs | 1,028 µs | 2,270 µs | 8,344 µs | 64 rows |
| `id` | 1,000 rows | 64 values | D | 1,151 groups | 290.1 lists/s | 3,192 µs | 3,280 µs | 3,318 µs | 3,368 µs | 4,744 µs | 5,358 µs | 64 rows |
| `id` | 1,000 rows | 64 values | J | 17,797 groups | 5,165.5 lists/s | 148 µs | 157 µs | 165 µs | 227 µs | 323 µs | 6,532 µs | 64 rows |
| `id` | 1,000 rows | 1,024 values | L | 538 groups | 139.7 lists/s | 6,704 µs | 6,981 µs | 7,067 µs | 7,179 µs | 9,266 µs | 11.6 ms | 1,000 rows |
| `id` | 1,000 rows | 1,024 values | L2 | 539 groups | 139.9 lists/s | 6,751 µs | 6,979 µs | 7,058 µs | 7,189 µs | 8,556 µs | 11.1 ms | 1,000 rows |
| `id` | 1,000 rows | 1,024 values | D | 67 groups | 16.6 lists/s | 51.7 ms | 53.0 ms | 55.8 ms | 69.7 ms | 74.5 ms | 75.9 ms | 1,000 rows |
| `id` | 1,000 rows | 1,024 values | J | 1,905 groups | 553.2 lists/s | 1,633 µs | 1,748 µs | 1,790 µs | 1,841 µs | 2,220 µs | 2,511 µs | 1,000 rows |
| `id` | 1,000 rows | 4,096 values | L | 144 groups | 37.4 lists/s | 25.4 ms | 26.4 ms | 26.7 ms | 27.0 ms | 28.5 ms | 29.5 ms | 1,000 rows |
| `id` | 1,000 rows | 4,096 values | L2 | 145 groups | 37.4 lists/s | 25.7 ms | 26.4 ms | 26.7 ms | 27.0 ms | 30.2 ms | 32.1 ms | 1,000 rows |
| `id` | 1,000 rows | 4,096 values | D | 20 groups | 4.9 lists/s | 201.6 ms | 202.9 ms | 204.1 ms | 205.6 ms | 219.0 ms | 222.7 ms | 1,000 rows |
| `id` | 1,000 rows | 4,096 values | J | 949 groups | 315.6 lists/s | 2,906 µs | 3,078 µs | 3,141 µs | 3,208 µs | 4,170 µs | 6,593 µs | 1,000 rows |
| `id` | 100,000 rows | 1 value | L | 65,016 groups | 17,432.1 lists/s | 49.2 µs | 53.8 µs | 54.8 µs | 56.9 µs | 83.7 µs | 6,363 µs | 1 rows |
| `id` | 100,000 rows | 1 value | L2 | 65,434 groups | 17,533.0 lists/s | 48.5 µs | 53.9 µs | 54.8 µs | 56.8 µs | 77.6 µs | 6,589 µs | 1 rows |
| `id` | 100,000 rows | 1 value | D | 64,429 groups | 17,281.0 lists/s | 49.1 µs | 52.6 µs | 53.8 µs | 61.5 µs | 82.0 µs | 6,025 µs | 1 rows |
| `id` | 100,000 rows | 1 value | J | 50,019 groups | 14,970.4 lists/s | 53.5 µs | 57.0 µs | 58.6 µs | 78.5 µs | 113 µs | 3,604 µs | 1 rows |
| `id` | 100,000 rows | 4 values | L | 433 groups | 108.3 lists/s | 8,961 µs | 9,131 µs | 9,191 µs | 9,298 µs | 9,786 µs | 10.1 ms | 4 rows |
| `id` | 100,000 rows | 4 values | L2 | 432 groups | 108.0 lists/s | 8,922 µs | 9,108 µs | 9,190 µs | 9,348 µs | 10.5 ms | 12.2 ms | 4 rows |
| `id` | 100,000 rows | 4 values | D | 16,396 groups | 4,221.9 lists/s | 199 µs | 211 µs | 249 µs | 256 µs | 323 µs | 6,557 µs | 4 rows |
| `id` | 100,000 rows | 4 values | J | 40,267 groups | 11,958.6 lists/s | 59.0 µs | 65.5 µs | 84.8 µs | 94.7 µs | 134 µs | 6,476 µs | 4 rows |
| `id` | 100,000 rows | 64 values | L | 55 groups | 13.6 lists/s | 72.4 ms | 72.9 ms | 73.3 ms | 75.0 ms | 77.9 ms | 81.0 ms | 64 rows |
| `id` | 100,000 rows | 64 values | L2 | 56 groups | 13.6 lists/s | 72.2 ms | 72.7 ms | 73.0 ms | 73.4 ms | 75.6 ms | 76.1 ms | 64 rows |
| `id` | 100,000 rows | 64 values | D | 1,150 groups | 289.5 lists/s | 3,246 µs | 3,336 µs | 3,367 µs | 3,411 µs | 4,322 µs | 5,131 µs | 64 rows |
| `id` | 100,000 rows | 64 values | J | 18,332 groups | 5,345.5 lists/s | 152 µs | 162 µs | 196 µs | 201 µs | 291 µs | 4,198 µs | 64 rows |
| `id` | 100,000 rows | 1,024 values | L | 4 groups | 0.9 lists/s | 1.04 s | 1.04 s | 1.04 s | 1.06 s | 1.06 s | 1.07 s | 1,024 rows |
| `id` | 100,000 rows | 1,024 values | L2 | 4 groups | 1.0 lists/s | 1.04 s | 1.04 s | 1.04 s | 1.04 s | 1.04 s | 1.04 s | 1,024 rows |
| `id` | 100,000 rows | 1,024 values | D | 72 groups | 17.6 lists/s | 53.5 ms | 54.1 ms | 54.5 ms | 56.0 ms | 75.3 ms | 76.6 ms | 1,024 rows |
| `id` | 100,000 rows | 1,024 values | J | 1,657 groups | 476.0 lists/s | 1,877 µs | 2,041 µs | 2,087 µs | 2,138 µs | 2,467 µs | 5,890 µs | 1,024 rows |
| `id` | 100,000 rows | 4,096 values | L | 2 groups | 0.3 lists/s | 3.97 s | 3.98 s | 3.98 s | 3.98 s | 3.98 s | 3.99 s | 4,096 rows |
| `id` | 100,000 rows | 4,096 values | L2 | 2 groups | 0.2 lists/s | 3.98 s | 3.99 s | 3.99 s | 3.99 s | 3.99 s | 4.00 s | 4,096 rows |
| `id` | 100,000 rows | 4,096 values | D | 17 groups | 4.2 lists/s | 215.5 ms | 222.9 ms | 239.1 ms | 254.2 ms | 272.1 ms | 292.8 ms | 4,096 rows |
| `id` | 100,000 rows | 4,096 values | J | 442 groups | 126.3 lists/s | 7,508 µs | 7,739 µs | 7,818 µs | 7,910 µs | 10.1 ms | 11.1 ms | 4,096 rows |
| `k` | 1,000 rows | 1 value | L | 64,349 groups | 17,206.5 lists/s | 51.7 µs | 55.5 µs | 56.7 µs | 58.3 µs | 79.5 µs | 1,691 µs | 1 rows |
| `k` | 1,000 rows | 1 value | L2 | 64,025 groups | 17,124.7 lists/s | 51.5 µs | 55.7 µs | 56.8 µs | 58.5 µs | 79.4 µs | 2,923 µs | 1 rows |
| `k` | 1,000 rows | 1 value | D | 62,970 groups | 16,878.2 lists/s | 50.8 µs | 55.0 µs | 56.3 µs | 59.2 µs | 84.8 µs | 3,697 µs | 1 rows |
| `k` | 1,000 rows | 1 value | J | 52,863 groups | 15,804.3 lists/s | 56.2 µs | 59.7 µs | 60.7 µs | 62.6 µs | 101 µs | 4,756 µs | 1 rows |
| `k` | 1,000 rows | 4 values | L | 21,996 groups | 5,707.8 lists/s | 147 µs | 155 µs | 158 µs | 192 µs | 260 µs | 2,859 µs | 4 rows |
| `k` | 1,000 rows | 4 values | L2 | 21,660 groups | 5,622.4 lists/s | 146 µs | 156 µs | 187 µs | 191 µs | 261 µs | 1,891 µs | 4 rows |
| `k` | 1,000 rows | 4 values | D | 14,530 groups | 3,741.0 lists/s | 206 µs | 221 µs | 265 µs | 311 µs | 396 µs | 5,211 µs | 4 rows |
| `k` | 1,000 rows | 4 values | J | 37,097 groups | 10,855.9 lists/s | 65.1 µs | 69.6 µs | 97.9 µs | 104 µs | 139 µs | 5,075 µs | 4 rows |
| `k` | 1,000 rows | 64 values | L | 4,084 groups | 1,054.8 lists/s | 831 µs | 905 µs | 932 µs | 971 µs | 1,162 µs | 2,987 µs | 64 rows |
| `k` | 1,000 rows | 64 values | L2 | 4,075 groups | 1,053.0 lists/s | 837 µs | 905 µs | 934 µs | 976 µs | 1,154 µs | 1,639 µs | 64 rows |
| `k` | 1,000 rows | 64 values | D | 1,028 groups | 259.0 lists/s | 3,380 µs | 3,495 µs | 3,570 µs | 4,243 µs | 5,152 µs | 9,876 µs | 64 rows |
| `k` | 1,000 rows | 64 values | J | 11,124 groups | 3,058.1 lists/s | 260 µs | 310 µs | 331 µs | 338 µs | 432 µs | 4,100 µs | 64 rows |
| `k` | 1,000 rows | 1,024 values | L | 542 groups | 140.7 lists/s | 6,744 µs | 6,965 µs | 7,046 µs | 7,145 µs | 8,613 µs | 14.1 ms | 1,000 rows |
| `k` | 1,000 rows | 1,024 values | L2 | 541 groups | 140.4 lists/s | 6,767 µs | 6,989 µs | 7,065 µs | 7,186 µs | 8,876 µs | 11.3 ms | 1,000 rows |
| `k` | 1,000 rows | 1,024 values | D | 71 groups | 17.6 lists/s | 55.3 ms | 56.2 ms | 56.8 ms | 57.3 ms | 58.3 ms | 58.3 ms | 1,000 rows |
| `k` | 1,000 rows | 1,024 values | J | 1,017 groups | 273.9 lists/s | 3,400 µs | 3,563 µs | 3,627 µs | 3,700 µs | 4,252 µs | 6,892 µs | 1,000 rows |
| `k` | 1,000 rows | 4,096 values | L | 142 groups | 36.8 lists/s | 25.8 ms | 26.6 ms | 27.0 ms | 27.3 ms | 37.2 ms | 44.2 ms | 1,000 rows |
| `k` | 1,000 rows | 4,096 values | L2 | 144 groups | 37.2 lists/s | 25.7 ms | 26.4 ms | 26.7 ms | 27.1 ms | 32.5 ms | 36.8 ms | 1,000 rows |
| `k` | 1,000 rows | 4,096 values | D | 17 groups | 4.1 lists/s | 217.5 ms | 222.0 ms | 227.4 ms | 264.5 ms | 281.2 ms | 305.9 ms | 1,000 rows |
| `k` | 1,000 rows | 4,096 values | J | 271 groups | 72.6 lists/s | 13.5 ms | 13.6 ms | 13.7 ms | 13.9 ms | 15.1 ms | 15.5 ms | 1,000 rows |
| `k` | 100,000 rows | 1 value | L | 63,225 groups | 16,861.5 lists/s | 52.2 µs | 56.8 µs | 58.0 µs | 59.8 µs | 78.0 µs | 954 µs | 1 rows |
| `k` | 100,000 rows | 1 value | L2 | 62,125 groups | 16,607.0 lists/s | 52.6 µs | 57.3 µs | 58.5 µs | 60.2 µs | 81.9 µs | 1,957 µs | 1 rows |
| `k` | 100,000 rows | 1 value | D | 64,701 groups | 17,252.5 lists/s | 51.1 µs | 55.3 µs | 56.5 µs | 58.2 µs | 77.4 µs | 1,656 µs | 1 rows |
| `k` | 100,000 rows | 1 value | J | 54,468 groups | 16,344.8 lists/s | 54.9 µs | 58.5 µs | 59.5 µs | 61.2 µs | 81.1 µs | 1,919 µs | 1 rows |
| `k` | 100,000 rows | 4 values | L | 421 groups | 105.4 lists/s | 9,060 µs | 9,353 µs | 9,428 µs | 9,530 µs | 10.7 ms | 15.9 ms | 4 rows |
| `k` | 100,000 rows | 4 values | L2 | 421 groups | 105.3 lists/s | 9,077 µs | 9,316 µs | 9,422 µs | 9,571 µs | 10.6 ms | 18.9 ms | 4 rows |
| `k` | 100,000 rows | 4 values | D | 17,085 groups | 4,388.9 lists/s | 209 µs | 221 µs | 224 µs | 230 µs | 291 µs | 844 µs | 4 rows |
| `k` | 100,000 rows | 4 values | J | 45,922 groups | 13,572.8 lists/s | 66.7 µs | 70.1 µs | 70.9 µs | 73.0 µs | 114 µs | 2,230 µs | 4 rows |
| `k` | 100,000 rows | 64 values | L | 56 groups | 13.7 lists/s | 72.1 ms | 72.6 ms | 72.7 ms | 73.0 ms | 76.2 ms | 77.5 ms | 64 rows |
| `k` | 100,000 rows | 64 values | L2 | 56 groups | 13.7 lists/s | 71.8 ms | 72.3 ms | 72.6 ms | 73.4 ms | 75.7 ms | 76.5 ms | 64 rows |
| `k` | 100,000 rows | 64 values | D | 1,056 groups | 265.8 lists/s | 3,408 µs | 3,511 µs | 3,553 µs | 4,248 µs | 5,056 µs | 5,544 µs | 64 rows |
| `k` | 100,000 rows | 64 values | J | 11,421 groups | 3,153.6 lists/s | 261 µs | 275 µs | 309 µs | 329 µs | 542 µs | 4,377 µs | 64 rows |
| `k` | 100,000 rows | 1,024 values | L | 4 groups | 1.0 lists/s | 1.04 s | 1.04 s | 1.04 s | 1.05 s | 1.05 s | 1.05 s | 1,024 rows |
| `k` | 100,000 rows | 1,024 values | L2 | 4 groups | 1.0 lists/s | 1.04 s | 1.04 s | 1.04 s | 1.05 s | 1.05 s | 1.05 s | 1,024 rows |
| `k` | 100,000 rows | 1,024 values | D | 66 groups | 16.6 lists/s | 55.7 ms | 57.3 ms | 57.9 ms | 59.9 ms | 80.6 ms | 80.9 ms | 1,024 rows |
| `k` | 100,000 rows | 1,024 values | J | 914 groups | 245.6 lists/s | 3,778 µs | 3,988 µs | 4,052 µs | 4,134 µs | 4,667 µs | 5,502 µs | 1,024 rows |
| `k` | 100,000 rows | 4,096 values | L | 2 groups | 0.3 lists/s | 3.98 s | 3.99 s | 3.99 s | 3.99 s | 3.99 s | 3.99 s | 4,096 rows |
| `k` | 100,000 rows | 4,096 values | L2 | 2 groups | 0.2 lists/s | 3.98 s | 4.06 s | 4.06 s | 4.06 s | 4.06 s | 4.13 s | 4,096 rows |
| `k` | 100,000 rows | 4,096 values | D | 18 groups | 4.1 lists/s | 228.2 ms | 231.4 ms | 234.2 ms | 241.8 ms | 289.1 ms | 297.0 ms | 4,096 rows |
| `k` | 100,000 rows | 4,096 values | J | 240 groups | 64.2 lists/s | 15.2 ms | 15.4 ms | 15.5 ms | 15.6 ms | 17.6 ms | 17.8 ms | 4,096 rows |

Small relations:

| column | rows | n | arm | groups | lists/s | p0 | p25 | p50 | p75 | p99 | max | rows/list |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `id` | 10 rows | 4 values | L | 27,744 groups | 14,890.8 lists/s | 56.6 µs | 60.0 µs | 61.0 µs | 64.1 µs | 119 µs | 5,495 µs | 4 rows |
| `id` | 10 rows | 4 values | L2 | 27,974 groups | 15,031.5 lists/s | 56.7 µs | 60.1 µs | 61.3 µs | 64.7 µs | 111 µs | 1,249 µs | 4 rows |
| `id` | 10 rows | 4 values | D | 8,561 groups | 4,396.0 lists/s | 191 µs | 202 µs | 208 µs | 249 µs | 328 µs | 3,020 µs | 4 rows |
| `id` | 10 rows | 4 values | J | 26,353 groups | 15,833.5 lists/s | 57.8 µs | 60.7 µs | 61.7 µs | 63.2 µs | 82.9 µs | 470 µs | 4 rows |
| `id` | 10 rows | 64 values | L | 14,584 groups | 8,414.0 lists/s | 104 µs | 109 µs | 111 µs | 117 µs | 182 µs | 1,259 µs | 10 rows |
| `id` | 10 rows | 64 values | L2 | 14,208 groups | 8,174.4 lists/s | 104 µs | 108 µs | 111 µs | 141 µs | 178 µs | 4,851 µs | 10 rows |
| `id` | 10 rows | 64 values | D | 627 groups | 315.2 lists/s | 3,028 µs | 3,092 µs | 3,117 µs | 3,144 µs | 4,355 µs | 8,820 µs | 10 rows |
| `id` | 10 rows | 64 values | J | 15,625 groups | 9,890.0 lists/s | 88.0 µs | 91.8 µs | 93.2 µs | 97.5 µs | 155 µs | 420 µs | 10 rows |
| `id` | 10 rows | 4,096 values | L | 324 groups | 192.7 lists/s | 4,930 µs | 5,100 µs | 5,158 µs | 5,239 µs | 5,935 µs | 6,848 µs | 10 rows |
| `id` | 10 rows | 4,096 values | L2 | 324 groups | 193.6 lists/s | 4,895 µs | 5,081 µs | 5,147 µs | 5,216 µs | 5,547 µs | 6,320 µs | 10 rows |
| `id` | 10 rows | 4,096 values | D | 12 groups | 5.0 lists/s | 194.9 ms | 196.5 ms | 197.9 ms | 198.7 ms | 199.7 ms | 200.8 ms | 10 rows |
| `id` | 10 rows | 4,096 values | J | 730 groups | 577.0 lists/s | 1,611 µs | 1,676 µs | 1,719 µs | 1,769 µs | 2,019 µs | 3,010 µs | 10 rows |
| `id` | 100 rows | 4 values | L | 24,889 groups | 13,299.5 lists/s | 65.5 µs | 68.5 µs | 69.4 µs | 72.4 µs | 115 µs | 1,790 µs | 4 rows |
| `id` | 100 rows | 4 values | L2 | 24,599 groups | 13,152.9 lists/s | 65.4 µs | 68.8 µs | 70.0 µs | 74.3 µs | 116 µs | 1,491 µs | 4 rows |
| `id` | 100 rows | 4 values | D | 9,409 groups | 4,838.1 lists/s | 193 µs | 202 µs | 204 µs | 208 µs | 249 µs | 570 µs | 4 rows |
| `id` | 100 rows | 4 values | J | 25,042 groups | 15,033.0 lists/s | 57.9 µs | 60.9 µs | 61.8 µs | 64.2 µs | 108 µs | 2,197 µs | 4 rows |
| `id` | 100 rows | 64 values | L | 8,527 groups | 4,637.7 lists/s | 200 µs | 206 µs | 210 µs | 215 µs | 332 µs | 584 µs | 64 rows |
| `id` | 100 rows | 64 values | L2 | 8,517 groups | 4,628.8 lists/s | 200 µs | 206 µs | 209 µs | 214 µs | 343 µs | 3,213 µs | 64 rows |
| `id` | 100 rows | 64 values | D | 606 groups | 304.7 lists/s | 3,151 µs | 3,231 µs | 3,263 µs | 3,297 µs | 3,760 µs | 4,025 µs | 64 rows |
| `id` | 100 rows | 64 values | J | 10,868 groups | 6,379.3 lists/s | 145 µs | 150 µs | 152 µs | 156 µs | 246 µs | 1,996 µs | 64 rows |
| `id` | 100 rows | 4,096 values | L | 263 groups | 151.3 lists/s | 6,129 µs | 6,477 µs | 6,598 µs | 6,709 µs | 7,169 µs | 8,260 µs | 100 rows |
| `id` | 100 rows | 4,096 values | L2 | 261 groups | 150.8 lists/s | 6,263 µs | 6,491 µs | 6,600 µs | 6,714 µs | 7,760 µs | 8,629 µs | 100 rows |
| `id` | 100 rows | 4,096 values | D | 10 groups | 4.9 lists/s | 195.9 ms | 198.7 ms | 199.3 ms | 204.2 ms | 217.0 ms | 233.0 ms | 100 rows |
| `id` | 100 rows | 4,096 values | J | 658 groups | 499.1 lists/s | 1,850 µs | 1,944 µs | 1,994 µs | 2,052 µs | 2,251 µs | 2,433 µs | 100 rows |
| `k` | 10 rows | 4 values | L | 28,642 groups | 15,405.0 lists/s | 57.2 µs | 60.4 µs | 61.3 µs | 64.5 µs | 104 µs | 500 µs | 4 rows |
| `k` | 10 rows | 4 values | L2 | 26,828 groups | 14,415.5 lists/s | 57.2 µs | 60.8 µs | 62.0 µs | 66.2 µs | 129 µs | 4,740 µs | 4 rows |
| `k` | 10 rows | 4 values | D | 8,581 groups | 4,405.4 lists/s | 199 µs | 209 µs | 214 µs | 232 µs | 340 µs | 2,636 µs | 4 rows |
| `k` | 10 rows | 4 values | J | 21,706 groups | 12,820.9 lists/s | 60.3 µs | 63.7 µs | 68.8 µs | 95.3 µs | 116 µs | 440 µs | 4 rows |
| `k` | 10 rows | 64 values | L | 14,238 groups | 8,193.5 lists/s | 106 µs | 111 µs | 113 µs | 120 µs | 193 µs | 536 µs | 10 rows |
| `k` | 10 rows | 64 values | L2 | 13,949 groups | 8,018.6 lists/s | 107 µs | 111 µs | 114 µs | 144 µs | 177 µs | 941 µs | 10 rows |
| `k` | 10 rows | 64 values | D | 555 groups | 278.8 lists/s | 3,059 µs | 3,182 µs | 3,758 µs | 3,798 µs | 4,464 µs | 5,223 µs | 10 rows |
| `k` | 10 rows | 64 values | J | 13,200 groups | 8,113.4 lists/s | 98.9 µs | 104 µs | 136 µs | 139 µs | 178 µs | 488 µs | 10 rows |
| `k` | 10 rows | 4,096 values | L | 322 groups | 191.7 lists/s | 4,981 µs | 5,141 µs | 5,200 µs | 5,263 µs | 5,635 µs | 7,068 µs | 10 rows |
| `k` | 10 rows | 4,096 values | L2 | 322 groups | 191.5 lists/s | 4,932 µs | 5,125 µs | 5,188 µs | 5,269 µs | 6,247 µs | 7,207 µs | 10 rows |
| `k` | 10 rows | 4,096 values | D | 10 groups | 4.8 lists/s | 198.9 ms | 201.2 ms | 202.8 ms | 204.9 ms | 233.5 ms | 265.2 ms | 10 rows |
| `k` | 10 rows | 4,096 values | J | 632 groups | 464.1 lists/s | 2,020 µs | 2,103 µs | 2,145 µs | 2,196 µs | 2,373 µs | 2,462 µs | 10 rows |
| `k` | 100 rows | 4 values | L | 20,808 groups | 11,105.1 lists/s | 66.2 µs | 98.2 µs | 100 µs | 102 µs | 121 µs | 5,471 µs | 4 rows |
| `k` | 100 rows | 4 values | L2 | 25,728 groups | 13,760.9 lists/s | 65.7 µs | 69.3 µs | 70.4 µs | 72.8 µs | 114 µs | 659 µs | 4 rows |
| `k` | 100 rows | 4 values | D | 7,610 groups | 3,912.5 lists/s | 202 µs | 257 µs | 262 µs | 272 µs | 341 µs | 796 µs | 4 rows |
| `k` | 100 rows | 4 values | J | 22,161 groups | 13,091.4 lists/s | 62.9 µs | 66.0 µs | 67.7 µs | 96.3 µs | 113 µs | 388 µs | 4 rows |
| `k` | 100 rows | 64 values | L | 7,733 groups | 4,181.9 lists/s | 201 µs | 211 µs | 244 µs | 253 µs | 367 µs | 1,592 µs | 64 rows |
| `k` | 100 rows | 64 values | L2 | 8,253 groups | 4,480.6 lists/s | 202 µs | 209 µs | 213 µs | 225 µs | 382 µs | 1,901 µs | 64 rows |
| `k` | 100 rows | 64 values | D | 494 groups | 248.3 lists/s | 3,308 µs | 3,389 µs | 4,053 µs | 4,752 µs | 4,979 µs | 5,438 µs | 64 rows |
| `k` | 100 rows | 64 values | J | 8,176 groups | 4,608.4 lists/s | 199 µs | 205 µs | 208 µs | 214 µs | 384 µs | 578 µs | 64 rows |
| `k` | 100 rows | 4,096 values | L | 260 groups | 149.6 lists/s | 6,278 µs | 6,574 µs | 6,670 µs | 6,772 µs | 7,464 µs | 8,035 µs | 100 rows |
| `k` | 100 rows | 4,096 values | L2 | 262 groups | 150.1 lists/s | 6,258 µs | 6,491 µs | 6,598 µs | 6,706 µs | 8,433 µs | 10.2 ms | 100 rows |
| `k` | 100 rows | 4,096 values | D | 8 groups | 3.8 lists/s | 230.4 ms | 251.9 ms | 256.2 ms | 273.5 ms | 281.6 ms | 282.3 ms | 100 rows |
| `k` | 100 rows | 4,096 values | J | 289 groups | 168.8 lists/s | 5,812 µs | 5,881 µs | 5,912 µs | 5,944 µs | 6,422 µs | 6,720 µs | 100 rows |

One row:

| column | rows | n | arm | groups | lists/s | p0 | p25 | p50 | p75 | p99 | max | rows/list |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `id` | 1 rows | 1 value | L | 32,087 groups | 17,149.6 lists/s | 48.7 µs | 51.9 µs | 53.0 µs | 64.3 µs | 90.0 µs | 1,522 µs | 1 rows |
| `id` | 1 rows | 1 value | L2 | 26,704 groups | 14,337.0 lists/s | 49.1 µs | 63.0 µs | 68.2 µs | 75.1 µs | 103 µs | 440 µs | 1 rows |
| `id` | 1 rows | 1 value | D | 31,347 groups | 16,789.0 lists/s | 47.2 µs | 60.0 µs | 61.6 µs | 63.1 µs | 82.5 µs | 403 µs | 1 rows |
| `id` | 1 rows | 1 value | J | 19,187 groups | 11,234.1 lists/s | 53.2 µs | 76.2 µs | 95.9 µs | 98.9 µs | 125 µs | 419 µs | 1 rows |
| `id` | 1 rows | 4 values | L | 32,193 groups | 17,345.5 lists/s | 52.6 µs | 55.3 µs | 56.2 µs | 57.5 µs | 76.8 µs | 558 µs | 1 rows |
| `id` | 1 rows | 4 values | L2 | 31,114 groups | 16,741.7 lists/s | 52.7 µs | 55.6 µs | 56.8 µs | 61.4 µs | 87.8 µs | 6,929 µs | 1 rows |
| `id` | 1 rows | 4 values | D | 8,943 groups | 4,599.9 lists/s | 185 µs | 195 µs | 230 µs | 237 µs | 273 µs | 867 µs | 1 rows |
| `id` | 1 rows | 4 values | J | 22,730 groups | 13,599.6 lists/s | 54.4 µs | 77.5 µs | 78.9 µs | 80.9 µs | 102 µs | 426 µs | 1 rows |
| `id` | 1 rows | 64 values | L | 17,517 groups | 10,297.4 lists/s | 90.5 µs | 94.0 µs | 95.2 µs | 97.5 µs | 125 µs | 464 µs | 1 rows |
| `id` | 1 rows | 64 values | L2 | 17,427 groups | 10,254.5 lists/s | 90.8 µs | 94.3 µs | 95.5 µs | 97.8 µs | 127 µs | 458 µs | 1 rows |
| `id` | 1 rows | 64 values | D | 635 groups | 318.9 lists/s | 2,976 µs | 3,035 µs | 3,065 µs | 3,106 µs | 3,896 µs | 4,323 µs | 1 rows |
| `id` | 1 rows | 64 values | J | 18,516 groups | 12,038.3 lists/s | 76.1 µs | 79.7 µs | 80.8 µs | 83.0 µs | 118 µs | 502 µs | 1 rows |
| `id` | 1 rows | 1,024 values | L | 1,851 groups | 1,227.8 lists/s | 707 µs | 774 µs | 800 µs | 839 µs | 1,046 µs | 1,600 µs | 1 rows |
| `id` | 1 rows | 1,024 values | L2 | 1,821 groups | 1,200.7 lists/s | 709 µs | 767 µs | 805 µs | 870 µs | 1,144 µs | 5,574 µs | 1 rows |
| `id` | 1 rows | 1,024 values | D | 41 groups | 20.0 lists/s | 48.0 ms | 48.6 ms | 49.3 ms | 49.9 ms | 57.8 ms | 63.3 ms | 1 rows |
| `id` | 1 rows | 1,024 values | J | 2,639 groups | 2,042.1 lists/s | 425 µs | 454 µs | 474 µs | 506 µs | 675 µs | 2,542 µs | 1 rows |
| `id` | 1 rows | 4,096 values | L | 360 groups | 219.8 lists/s | 4,304 µs | 4,459 µs | 4,509 µs | 4,589 µs | 5,541 µs | 6,309 µs | 1 rows |
| `id` | 1 rows | 4,096 values | L2 | 364 groups | 222.1 lists/s | 4,277 µs | 4,418 µs | 4,461 µs | 4,519 µs | 5,843 µs | 6,462 µs | 1 rows |
| `id` | 1 rows | 4,096 values | D | 12 groups | 5.0 lists/s | 194.8 ms | 196.3 ms | 196.5 ms | 200.0 ms | 200.3 ms | 201.3 ms | 1 rows |
| `id` | 1 rows | 4,096 values | J | 764 groups | 617.2 lists/s | 1,497 µs | 1,560 µs | 1,604 µs | 1,659 µs | 1,942 µs | 2,701 µs | 1 rows |
| `k` | 1 rows | 1 value | L | 27,770 groups | 14,877.4 lists/s | 50.2 µs | 54.3 µs | 65.5 µs | 76.8 µs | 110 µs | 1,884 µs | 1 rows |
| `k` | 1 rows | 1 value | L2 | 28,964 groups | 15,507.7 lists/s | 49.7 µs | 53.7 µs | 56.9 µs | 76.2 µs | 104 µs | 4,886 µs | 1 rows |
| `k` | 1 rows | 1 value | D | 28,883 groups | 15,473.8 lists/s | 49.4 µs | 52.8 µs | 64.2 µs | 70.0 µs | 122 µs | 4,944 µs | 1 rows |
| `k` | 1 rows | 1 value | J | 22,826 groups | 13,500.5 lists/s | 54.2 µs | 58.4 µs | 78.9 µs | 82.4 µs | 126 µs | 2,231 µs | 1 rows |
| `k` | 1 rows | 4 values | L | 25,159 groups | 13,629.8 lists/s | 53.0 µs | 56.8 µs | 75.5 µs | 82.1 µs | 116 µs | 2,023 µs | 1 rows |
| `k` | 1 rows | 4 values | L2 | 25,953 groups | 14,046.0 lists/s | 53.0 µs | 56.8 µs | 69.4 µs | 81.8 µs | 120 µs | 5,805 µs | 1 rows |
| `k` | 1 rows | 4 values | D | 8,554 groups | 4,398.8 lists/s | 187 µs | 200 µs | 214 µs | 244 µs | 339 µs | 4,061 µs | 1 rows |
| `k` | 1 rows | 4 values | J | 23,080 groups | 13,735.2 lists/s | 56.0 µs | 59.4 µs | 61.9 µs | 83.3 µs | 129 µs | 3,717 µs | 1 rows |
| `k` | 1 rows | 64 values | L | 12,724 groups | 7,420.9 lists/s | 92.1 µs | 111 µs | 138 µs | 156 µs | 182 µs | 3,149 µs | 1 rows |
| `k` | 1 rows | 64 values | L2 | 14,397 groups | 8,406.2 lists/s | 91.4 µs | 95.9 µs | 124 µs | 128 µs | 193 µs | 4,370 µs | 1 rows |
| `k` | 1 rows | 64 values | D | 600 groups | 301.7 lists/s | 3,046 µs | 3,106 µs | 3,136 µs | 3,565 µs | 4,385 µs | 5,217 µs | 1 rows |
| `k` | 1 rows | 64 values | J | 17,586 groups | 11,297.9 lists/s | 79.0 µs | 82.6 µs | 83.7 µs | 86.5 µs | 149 µs | 740 µs | 1 rows |
| `k` | 1 rows | 1,024 values | L | 1,728 groups | 1,126.1 lists/s | 740 µs | 822 µs | 865 µs | 935 µs | 1,220 µs | 3,405 µs | 1 rows |
| `k` | 1 rows | 1,024 values | L2 | 1,749 groups | 1,142.7 lists/s | 743 µs | 810 µs | 854 µs | 923 µs | 1,163 µs | 1,639 µs | 1 rows |
| `k` | 1 rows | 1,024 values | D | 35 groups | 16.9 lists/s | 49.4 ms | 59.0 ms | 59.3 ms | 60.0 ms | 66.7 ms | 68.7 ms | 1 rows |
| `k` | 1 rows | 1,024 values | J | 2,482 groups | 1,863.8 lists/s | 458 µs | 492 µs | 516 µs | 558 µs | 816 µs | 1,216 µs | 1 rows |
| `k` | 1 rows | 4,096 values | L | 359 groups | 218.3 lists/s | 4,405 µs | 4,491 µs | 4,536 µs | 4,615 µs | 5,341 µs | 5,960 µs | 1 rows |
| `k` | 1 rows | 4,096 values | L2 | 364 groups | 221.8 lists/s | 4,394 µs | 4,457 µs | 4,489 µs | 4,542 µs | 4,859 µs | 5,990 µs | 1 rows |
| `k` | 1 rows | 4,096 values | D | 10 groups | 4.8 lists/s | 198.6 ms | 201.8 ms | 202.8 ms | 213.8 ms | 217.7 ms | 232.5 ms | 1 rows |
| `k` | 1 rows | 4,096 values | J | 742 groups | 586.5 lists/s | 1,598 µs | 1,665 µs | 1,697 µs | 1,731 µs | 1,977 µs | 2,257 µs | 1 rows |

## Waits

None of the arms waits. `ANALYZE`'s last line for every statement kept is CPU only (`DB time 9.89 ms = CPU 9.89` for the 4-value walk at 100,000 rows; Plans, below): one connection on one core, reads only, every page resident in a 65,536-frame pool. `sys.wait_stats` was not sampled.

## Plans

`ANALYZE`'s reply at 100,000 rows; the archive's `result.json` keeps it for every cell.

- `id`, 100,000 rows, n = 1, arm L: `SELECT * FROM cu_t WHERE id IN (55108)`
    - `analyze rows=1 class=PointSelect steps=1 examined=1 pages=1 opens=1 pattern_id=0x652dc280105f7aea fetch_id=0xec07565feed1404d`
    - `class=PointSelect steps=1 hoisted=0`
    - `step 0 Lookup cu_t key=55108`
    - `  filter 0:0.0 = 55108`
    - `step 0 Lookup cu_t opens=1 examined=1 matched=1 sel=100% pages=1`
    - `DB time 0.13 ms = CPU 0.13`
- `id`, 100,000 rows, n = 4, arm L: `SELECT * FROM cu_t WHERE id IN (9228, 89109, 49376, 11113)`
    - `analyze rows=4 class=JoinSelect steps=1 examined=100000 pages=705 opens=1 pattern_id=0x37dac98f166831de fetch_id=0x69d4d7cd6d180d15`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 Scan cu_t`
    - `  filter bool (0:0.0 = 9228 OR 0:0.0 = 89109 OR 0:0.0 = 49376 OR 0:0.0 = 11113)`
    - `step 0 Scan cu_t opens=1 examined=100000 matched=4 sel=0% pages=705`
    - `DB time 9.89 ms = CPU 9.89`
- `id`, 100,000 rows, n = 4, arm J: `SELECT t.id, t.k, t.a, t.b FROM cu_v_4 AS v JOIN cu_t AS t ON t.id = v.rid`
    - `analyze rows=4 class=JoinSelect steps=2 examined=8 pages=5 opens=5 pattern_id=0xc054b33f51ca54bc fetch_id=0x133730dc035e9b4`
    - `class=JoinSelect steps=2 hoisted=0`
    - `step 0 Scan cu_v_4 AS v`
    - `step 1 Probe cu_t AS t key=0:0.1`
    - `  filter 0:1.0 = 0:0.1`
    - `step 0 Scan cu_v_4 AS v opens=1 examined=4 matched=4 sel=100% pages=1`
    - `step 1 Probe cu_t AS t opens=4 examined=4 matched=4 sel=100% pages=4`
    - `DB time 0.13 ms = CPU 0.13`
- `id`, 100,000 rows, n = 4, arm D: `SELECT * FROM cu_t WHERE id = 77`
    - `analyze rows=1 class=PointSelect steps=1 examined=1 pages=1 opens=1 pattern_id=0x207dc3e32ec5053b fetch_id=0xd8b45cb18d55b324`
    - `class=PointSelect steps=1 hoisted=0`
    - `step 0 Lookup cu_t key=77`
    - `  filter 0:0.0 = 77`
    - `step 0 Lookup cu_t opens=1 examined=1 matched=1 sel=100% pages=1`
    - `DB time 0.03 ms = CPU 0.03`
- `k`, 100,000 rows, n = 1, arm L: `SELECT * FROM cu_t WHERE k IN (165325)`
    - `analyze rows=1 class=JoinSelect steps=1 examined=1 pages=2 opens=1 pattern_id=0xa13419a9207fb103 fetch_id=0xdfa3e8ae1b49af3e`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 IndexProbe cu_t`
    - `  filter 0:0.1 = 165325`
    - `step 0 IndexProbe cu_t opens=1 examined=1 matched=1 sel=100% pages=2 index_scanned=1 index_resolved=1`
    - `DB time 0.11 ms = CPU 0.11`
- `k`, 100,000 rows, n = 4, arm L: `SELECT * FROM cu_t WHERE k IN (27685, 267328, 148129, 33340)`
    - `analyze rows=4 class=JoinSelect steps=1 examined=100000 pages=705 opens=1 pattern_id=0x1310e3352f39eb6b fetch_id=0x6b650e973563bea`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 Scan cu_t`
    - `  filter bool (0:0.1 = 27685 OR 0:0.1 = 267328 OR 0:0.1 = 148129 OR 0:0.1 = 33340)`
    - `step 0 Scan cu_t opens=1 examined=100000 matched=4 sel=0% pages=705`
    - `DB time 10.08 ms = CPU 10.08`
- `k`, 100,000 rows, n = 4, arm J: `SELECT t.id, t.k, t.a, t.b FROM cu_v_4 AS v JOIN cu_t AS t ON t.k = v.rk`
    - `analyze rows=4 class=JoinSelect steps=2 examined=8 pages=9 opens=5 pattern_id=0xb569831e9ad879b0 fetch_id=0x6890c43fabf52fc8`
    - `class=JoinSelect steps=2 hoisted=0`
    - `step 0 Scan cu_v_4 AS v`
    - `step 1 IndexProbe cu_t AS t key=0:0.2`
    - `  filter 0:1.1 = 0:0.2`
    - `step 0 Scan cu_v_4 AS v opens=1 examined=4 matched=4 sel=100% pages=1`
    - `step 1 IndexProbe cu_t AS t opens=4 examined=4 matched=4 sel=100% pages=8 index_scanned=4 index_resolved=4`
    - `DB time 0.10 ms = CPU 0.10`
- `k`, 100,000 rows, n = 4, arm D: `SELECT * FROM cu_t WHERE k = 232`
    - `analyze rows=1 class=JoinSelect steps=1 examined=1 pages=2 opens=1 pattern_id=0xe94e9ed3346cd4a2 fetch_id=0xa647081d39bbe5b7`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 IndexProbe cu_t`
    - `  filter 0:0.1 = 232`
    - `step 0 IndexProbe cu_t opens=1 examined=1 matched=1 sel=100% pages=2 index_scanned=1 index_resolved=1`
    - `DB time 0.09 ms = CPU 0.09`

## Delta against an earlier number

None exists. No results file under `bench/` measures a value list, and this is the first number for the shape. PostgreSQL was not run (cut, below).

## What bounds the reading

- **No arm is a key set.** The estimate adds two measured arms and is described above. The bar does not rest on it: D, which overpays by n statements, already beats the walk 93 times at 4 values and 50 times at 64 on the tuple key at 100,000 rows, and stays above the estimate with CU-R1's re-check added in every cell measured.
- **The estimate leaves out CU-R1's re-check**, priced from the walk's own per-comparison cost rather than measured. With it, a set loses at 1,000 rows from 1,024 values up, and the largest loss is about 12 ms, not 3.7 ms.
- **J's values are fixed per cell** and L's and D's are drawn per group, so J's descents are cache-warm. On a 100,000-row relation whose pages are all resident this can understate a descent; it cannot account for a difference of two orders of magnitude.
- **Arm order.** With two repeats the driver's rotation starts both with L, and L and D share their draws while L2 draws its own. L's and L2's means differ by at most 5 % in every 1,000- and 100,000-row cell of 4 values and more, so no order effect shows.
- **Hit ratio.** Every value hits unless the list is longer than the relation. Lists of mostly absent values were measured only as the cells whose list is longer than the relation.
- **A Cabin column was not measured.** CU-S2's Cabin set has no premise cell here.
- **`cores = 1`, one connection.** Nothing here says what a long list costs other sessions on its core, as a walk that yields every 64 pages or as a run of descents that does not.
- **Cut to the stage's budget** (20 minutes, building included; about 16 minutes of building and running were spent over the three runs): 2 repeats of 2 s per arm (1 s at 1, 10 and 100 rows); 1,024 values not run at 10 and 100 rows; two columns; no PostgreSQL floor; no wait sampling beyond `ANALYZE`'s line; no profile of the walk's per-comparison cost.

## What this says about the engine

The walk's cost is a product and the descents' cost is a sum. A list as a residual tests each row against each value, so server CPU is rows times list length at about 10 ns a comparison: 64 values over 100,000 rows cost 73 ms and 4,096 values cost 4 s, on a relation whose 4-value walk takes 9 ms. As descents the list costs about 1 µs a value on the tuple key and 3 µs on an index at 1,000 rows and more, less on the smaller relations. A set still tests every row it emits against the whole list (CU-R1), about `(n + 1) / 2` comparisons a row, so a list that matches most of the relation saves nothing over the walk.

The crossover sits where rows times 10 ns equals one descent, and it measures there: between 10 and 100 rows on the tuple key and between 100 and 1,000 on an index, about the same at every list length because both sides scale with it. That holds while a list matches few of the relation's rows: the re-check scales with the rows matched times the list length, so a list that matches most of a relation crosses higher, past 1,000 rows at 1,024 values and more. Under CU-Q3 (a) a set is taken at every size, so a list over a relation below the crossover is served slower than today. The loss is bounded by the descents and the re-check: 4,096 descents cost 1.6 to 4.2 ms on the tuple key and 2.0 to 12.7 ms on an index (J − J(1)), and the re-check of every row emitted up to about 84 ms at 4,096 matching rows (derived). The largest loss the estimate shows is 3.7 ms per statement without the re-check and about 12 ms with it, at 1,000 rows, where every row matches.

The statement's fixed cost is the third number. A client that works around the walk today by sending one `WHERE id = v` per value pays 21 to 29 µs of server CPU per value: 20 to 40 times the tuple key descent it asked for, and about 10 times an index descent. A key set gives that client one statement.
