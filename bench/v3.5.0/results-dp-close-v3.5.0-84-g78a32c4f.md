# DP-S4 close: a statement over no view pays nothing a method can resolve; a view costs about 3 us a level, linear to the bound, so depth 8 is affordable

**Thesis.** Measured at the DP close (B, `v3.5.0-84-g78a32c4f`) against the commit DP opened from (A, `v3.5.0-79-g30c47abf`), **the standard cells do not move outside their floors, with one cell at the edge of its own**: in server CPU per statement (24 rounds, same-binary control A2) the 15 cells of `tools/dk_close.py`'s M1 give B - A medians of +0.25 us on average against an A2 - A control of +0.20 us (the largest B - A, +1.16 us at 10,000 rows `insert1`, sits beside an A2 - A of +0.99 us); in `ba_census` throughput at `cores = 1` the paired B/A - 1 is -0.44 % (`point`), -0.27 % (`insert1`) and -0.79 % (`pkmix`) over 6 interleaved runs against a floor of +0.32 %, +0.58 % and +0.03 % - **`pkmix` is the one cell with all 6 paired runs negative (B lower, -0.27..-3.56 %) and a floor of -0.52..+1.02 %, about 0.3 us of 30 us a statement and the edge of what this method resolves**. At `cores = 2` every cell's own 3-run spread (3.5-9.8 %) exceeds any B - A. **A view that exists on the instance but is not read costs at most +0.4 us a point read (+1.7 %, 3 of 3 sizes positive, inside the +-0.5 us floor)**. **Reading through a view costs a fixed 3.0-3.7 us a level** (a point read 22.8-25.4 us by hand becomes 26.2-29.4 us, +13.8..+14.8 %; a two-relation key join +3.0..+3.2 us, +10..+11 %), **flat in the row count**, and linear in nesting: a view chain 8 deep adds +24.4 us to a 22.6 us point read (2.1x) and, where every level joins one more table, +32.4 us to a 54.8 us nine-way join (1.6x). **DP-R11's depth 8 is affordable on these numbers** (section 6).

| field | value |
|---|---|
| date executed | 2026-11-22; builds 09:28-09:31 UTC; Q1 first pass 09:31-09:40 UTC; Q2-Q4 09:40:41-09:42:11 UTC; A/A2/B server-CPU pass 09:42:22-09:43:44 UTC; Q4 join chain 09:43:46-09:43:54 UTC; Q1 control pass 09:44:16-09:56:48 UTC |
| branch | `worktree-dp-view` (worktree `sync-stale-records`), HEAD `78a32c4f`; **the checkout is dirty** (documents modified, `docs/spec/view.md` and `tools/dp_close.py` untracked) and that does not matter to the binaries: both were built from `git archive` exports of their commits (below), not from this tree |
| commits | **B** = `78a32c4f`, `git describe --tags` = `v3.5.0-84-g78a32c4f` (DP-S1..S3 are the commits between; DP-S4 is this file); **A** = `30c47abf`, `v3.5.0-79-g30c47abf` (the commit DP opened from) |
| work order | `instructions/v3.2.0/workorder-dp-view.md` section 5, DP-S4; DP-R10, DP-R11, DP-Q8, DP-Q18 |
| build | Release (`CMAKE_BUILD_TYPE=Release` read from both `CMakeCache.txt`), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, each from a `git archive` export under `/home/louis/bench-runs/dp-close/src-{A,B}` (no VCS directory; the shared `build-release` was not used) |
| **A** | copy `bin/kds_server_A`, sha256 `4d9e6c886986c662b91e16f4407c45010a77d09b684c7bda14eff97202f69be2`; source binary mtime 2026-11-22 09:30:48 UTC, after `30c47abf`'s commit time (01:46:45 UTC). **A2** is a second copy of the same bytes (same hash), started as a second server: the in-run floor |
| **B** | copy `bin/kds_server_B`, sha256 `765992266f50b7f0d7c3564bec9de99d00ef3528674ea84c2438d71d0f64d93c`; source binary mtime 09:30:51 UTC, after `78a32c4f`'s commit time (09:27:29 UTC) |
| binaries run from | the copies in `/home/louis/bench-runs/dp-close/bin/`; every server of every cell was started from a copy (`ba_census.py` makes its own copy of `--bin` besides) |
| device | data files and WAL under `/home/louis/bench-runs/dp-close/work-*`, `/dev/nvme0n1p1` (ext4, `df -T` at run time); `ba_census` records `data_device` per run (ext4 on the same device); every data directory removed at its server's stop |
| server | `durability = relaxed`, `buffer_pool_frames` 65,536, `cores` as stated per section, `log_level = warn`; default `join_order` (`fixed`); ports 15771-15773 (`dp_close.py`, `dk_close.py` 15761-15763 - never 15432), 15781-15783 (`ba_census`) |
| host | 8 CPUs, shared with other sessions; **no `cc1plus` at any cell's start** (`pgrep -c cc1plus` = 0 in all 90 `ba_census` start lines and before each driver run); load average (1 min) at the start of a `ba_census` run 1.42-5.18 (the 5.18 and the one 30 s wait are the first run, started 3 s after this session's own two builds ended); the driver cells record `host_before`/`host_after` in the archive JSON (1.0-2.7 at start). `ba_census` waits up to 30 s for load below 3 (`--max-load 3 --max-wait 30`) and then runs |
| PostgreSQL floor | not measured: the shapes are an engine's own compile-time costs, and no twin exists for a view's absorption |
| prior number for these shapes | Q1's cells: none at this commit's version for the `ba_census` shapes (the arm A of this file is the baseline); Q2-Q4: **no prior number** - a view did not exist before DP, so this file is the baseline the next DP-family run is read against |

## 1. Method, and what was cut

**Drivers.** `tools/ba_census.py` and `tools/dk_close.py` (M1 only), both **unmodified** at the measured commits; and the new `tools/dp_close.py` (documents its flags in `--help`; `bench/docs/` not updated for it). Archive: `bench/v3.5.0/archive/dp-close-v3.5.0-84-g78a32c4f/` (the JSON of every run, the driver logs, the heartbeat files, and the shell scripts that ran Q1 and its control pass).

| id | question | method | cells |
|---|---|---|---|
| Q1 | DP-R10, no view on the instance | **(a)** `ba_census.py` interleaved run by run, 3 runs an arm per cell, arm order flipped each run, fresh server and data file a run, 8 s measured after 2 s warm-up, 10,000 preloaded rows, `cores` 1 (2 sessions, server on CPU 2, clients on CPUs 4-5) and 2 (4 sessions, server CPUs 0,2, clients 4-7); **(b) control pass** at `cores` 1, 6 runs an arm, A, A2 and B interleaved (order rotated); **(c)** `dk_close.py` M1: server CPU per statement from `/proc` schedstat, A, A2, B up at once, 24 rounds, arm-to-CPU mapping rotated each round, 15 cells (5 shapes x 200, 1,000, 10,000 rows) | shapes `point`, `insert1`, `pkmix` |
| Q2 | DP-R10, a view exists | `dp_close.py q2`: three B servers up at once - N1 (no view has ever existed), N2 (same, the floor), V (one unrelated view `CREATE VIEW unrelated AS SELECT id FROM other`, so every FROM name is asked as a view first; DP-Q18); a point read `SELECT * FROM t<N> WHERE id = <even k>`, 2,000 statements a block, 24 rounds, CPU rotation per round | 200, 1,000, 10,000 rows |
| Q3 | the cost of reading through a view | `dp_close.py q3`: one B server, `cores = 1`; per size, hand-written (H), the same on an identical copy (H2, the floor), and through a view (VIEW), blocks interleaved, order rotated, 24 rounds; `point` = `SELECT v FROM t WHERE id = k` vs `... FROM v1 WHERE id = k` with `v1 (id, v) AS SELECT id, v FROM t`, 2,000 a block; `join` = `SELECT t.v, b.v FROM t JOIN b ON t.id = b.id WHERE t.id = k` vs `SELECT av, bv FROM vj WHERE id = k`, 1,000 a block; `range` = `WHERE id >= lo AND id < lo + 2 * R` with R = min(1,000, N), so 200, 1,000 and 1,000 rows returned at N = 200, 1,000 and 10,000, 100 a block | 200, 1,000, 10,000 rows; ids are even (2..2N) |
| Q4 | DP-R11, depth 8 | `dp_close.py q4` and `q4j`: one B server; **chain** `n1 (id, v) AS SELECT id, v FROM t`, `nK` over `n(K-1)`, a point read through n1, n2, n4, n8 against the table (H) and its copy (H2); **join chain** `m1 = j0 JOIN j1`, `mK = m(K-1) JOIN jK` (the absorbed statement at depth d is a (d+1)-way join) against the hand-written join (H_d); 24 rounds, order rotated | 1,000 rows (the chain does not scale with rows: Q3 shows its cost flat across 200-10,000) |

**Primary numbers.** Server CPU per statement (`/proc/<pid>/task/*/schedstat`, all server threads, compile and run together over the timed block), median of 24 rounds; the bracket after a difference is min..max over the rounds' paired differences (wide, +-7 us, because a round's CPU-slot effect is in it - read the median). The floor is the same-binary control (A2 - A in Q1, N2 - N1 in Q2, H2 - H in Q3, H1b - H1 in the join chain). Derived throughput is `1,000,000 / CPU us`, the server-CPU bound on a serial connection (derived, not reported by a driver). Latency columns are client wall time around one statement over loopback with the Python KWP client (it carries the client, **not a server cost**), pooled over the rounds. **Compile and run are not separable** on this engine's surface (a PARSE frame does not compile, `C_DESCRIBE` of an unexecuted statement is refused, `protocol.md`); the compile cost of absorption is the difference between a statement through a view and its hand-written expansion, which after absorption is the same block - **so the delta is the absorption (and the extra compile work it carries) and is attributed to nothing finer**; the run cost is the same on both sides by construction.

**Cut.** `cores` 4 and 8 (4 would overlap the server and client CPUs on this 8-CPU shared host), `group` and `strict` durability (every cell `relaxed`: DP adds no write or commit work, and a view is read-only, `NotImplemented` for a write), 100,000 rows, views whose body is not absorbable (the derived-table read, section 5's gap), a view over a subquery or an `IN` list, writes through a view (refused), a PostgreSQL twin, `ANALYZE`'s view of the plan. **Time:** two builds in parallel (`-j4` each) about 3 minutes (a warm compiler cache; both finished 09:30:51), Q1 first pass 9 minutes, Q2-Q4 1.5 minutes, the server-CPU pass 1.4 minutes, the control pass 12.5 minutes; about 30 minutes in all, inside the one-hour budget; no time spent waiting on a quiet host. A first-pass defect to name: the 3-run `ba_census` pass (section 2.1) had no same-binary control, which is why the 6-run pass with A2 (section 2.2) was added.

## 2. Q1 - DP-R10: the standard cells, no view on the instance

### 2.1 `ba_census`, first pass (3 runs an arm, A and B)

Throughput is the median of 3 runs (min..max); latencies are the medians of the 3 runs' own percentiles over every client statement; the spread is (max - min) / median. At `cores` 2 the cell's own spread (3.5-9.8 %) is larger than every B - A, so those rows are a bound, not a result; `cores` 1 has no control in this pass.

| cores | shape | arm | throughput | min..max | spread | p0 | p25 | p50 | p95 | p99 | statements | client errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `point` | A | 33,112 /s | 33,097..33,142 /s | 0.1 % | 46.6 us | 56.0 us | 57.1 us | 66.6 us | 76.6 us | 794,811 | 0 errors |
| 1 | `point` | B | 32,761 /s | 32,488..32,773 /s | 0.9 % | 47.3 us | 56.5 us | 57.6 us | 68.7 us | 78.4 us | 784,174 | 0 errors |
| 1 | `point` | **B - A** | -351 /s (-1.1 %) | | | | | | | | | |
| 1 | `insert1` | A | 41,253 /s | 41,085..41,340 /s | 0.6 % | 31.8 us | 40.0 us | 40.9 us | 50.5 us | 63.4 us | 989,426 | 0 errors |
| 1 | `insert1` | B | 40,967 /s | 40,886..41,014 /s | 0.3 % | 32.1 us | 40.2 us | 41.1 us | 51.2 us | 64.2 us | 982,936 | 0 errors |
| 1 | `insert1` | **B - A** | -286 /s (-0.7 %) | | | | | | | | | |
| 1 | `pkmix` | A | 33,446 /s | 33,250..33,572 /s | 1.0 % | 35.9 us | 47.6 us | 56.9 us | 72.4 us | 82.2 us | 802,147 | 0 errors |
| 1 | `pkmix` | B | 33,047 /s | 32,977..33,368 /s | 1.2 % | 36.2 us | 48.2 us | 57.3 us | 73.2 us | 82.5 us | 795,146 | 0 errors |
| 1 | `pkmix` | **B - A** | -398 /s (-1.2 %) | | | | | | | | | |
| 2 | `point` | A | 43,818 /s | 43,682..47,982 /s | 9.8 % | 49.1 us | 74.2 us | 88.6 us | 120.3 us | 140.4 us | 1,083,859 | 0 errors |
| 2 | `point` | B | 45,192 /s | 43,061..45,207 /s | 4.7 % | 54.2 us | 75.4 us | 85.5 us | 108.3 us | 137.1 us | 1,067,693 | 0 errors |
| 2 | `point` | **B - A** | +1,374 /s (+3.1 %) | | | | | | | | | |
| 2 | `insert1` | A | 56,743 /s | 55,224..57,223 /s | 3.5 % | 33.2 us | 46.3 us | 57.7 us | 76.1 us | 124.4 us | 1,362,550 | 0 errors |
| 2 | `insert1` | B | 57,062 /s | 51,905..57,327 /s | 9.5 % | 32.9 us | 45.6 us | 57.1 us | 76.6 us | 127.4 us | 1,330,358 | 0 errors |
| 2 | `insert1` | **B - A** | +319 /s (+0.6 %) | | | | | | | | | |
| 2 | `pkmix` | A | 46,829 /s | 44,641..47,143 /s | 5.3 % | 41.7 us | 66.7 us | 76.7 us | 120.8 us | 152.9 us | 1,108,912 | 0 errors |
| 2 | `pkmix` | B | 45,805 /s | 45,445..47,396 /s | 4.3 % | 42.1 us | 67.1 us | 76.8 us | 120.1 us | 152.6 us | 1,109,179 | 0 errors |
| 2 | `pkmix` | **B - A** | -1,023 /s (-2.2 %) | | | | | | | | | |

### 2.2 `ba_census`, control pass (`cores` = 1, 6 runs an arm, A, A2, B)

Added to put a same-binary floor beside the first pass's -0.7..-1.2 %. The `paired` rows are within a run triple (B/A - 1 and A2/A - 1: median, then [min, max] over the 6 runs).

| shape | arm | throughput | min..max | spread | p0 | p25 | p50 | p95 | p99 | statements | client errors |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `point` | A | 32,789 /s | 32,270..33,073 /s | 2.5 % | 46.8 us | 56.3 us | 57.5 us | 68.4 us | 78.7 us | 1,572,036 | 0 errors |
| `point` | A2 | 32,814 /s | 32,403..33,055 /s | 2.0 % | 46.6 us | 56.3 us | 57.5 us | 67.7 us | 78.0 us | 1,574,410 | 0 errors |
| `point` | B | 32,626 /s | 32,467..32,903 /s | 1.3 % | 47.5 us | 56.4 us | 57.7 us | 68.2 us | 78.3 us | 1,566,939 | 0 errors |
| `point` | paired B/A - 1 | -0.44 % [-1.14, +0.61] | | | | | | | | 4 of 6 runs negative | |
| `point` | paired A2/A - 1 (floor) | +0.32 % [-0.99, +1.52] | | | | | | | | 2 of 6 runs negative | |
| `insert1` | A | 40,970 /s | 40,595..41,090 /s | 1.2 % | 32.2 us | 40.2 us | 41.0 us | 51.4 us | 64.7 us | 1,963,323 | 0 errors |
| `insert1` | A2 | 41,161 /s | 40,857..41,366 /s | 1.2 % | 31.9 us | 40.0 us | 40.9 us | 50.7 us | 63.8 us | 1,974,082 | 0 errors |
| `insert1` | B | 40,779 /s | 40,351..41,003 /s | 1.6 % | 32.2 us | 40.3 us | 41.2 us | 51.2 us | 64.8 us | 1,956,160 | 0 errors |
| `insert1` | paired B/A - 1 | -0.27 % [-1.51, +0.38] | | | | | | | | 5 of 6 runs negative | |
| `insert1` | paired A2/A - 1 (floor) | +0.58 % [-0.57, +1.40] | | | | | | | | 1 of 6 runs negative | |
| `pkmix` | A | 33,269 /s | 33,096..33,321 /s | 0.7 % | 36.2 us | 47.8 us | 57.1 us | 73.0 us | 83.5 us | 1,594,875 | 0 errors |
| `pkmix` | A2 | 33,279 /s | 33,082..33,595 /s | 1.5 % | 35.8 us | 47.8 us | 57.0 us | 72.8 us | 82.8 us | 1,598,148 | 0 errors |
| `pkmix` | B | 32,980 /s | 32,102..33,044 /s | 2.9 % | 36.0 us | 48.1 us | 57.5 us | 73.4 us | 83.9 us | 1,576,099 | 0 errors |
| `pkmix` | paired B/A - 1 | -0.79 % [-3.56, -0.27] | | | | | | | | 6 of 6 runs negative | |
| `pkmix` | paired A2/A - 1 (floor) | +0.03 % [-0.52, +1.02] | | | | | | | | 3 of 6 runs negative | |

At `cores` 1 B's throughput is below A's by 0.27-0.79 % in the paired median, and **A2 is above A by 0.03-0.58 %**: the same bytes in the A2 slot read higher than the A slot, so a part of the sign is the slot. `point` (-0.44 % against a floor range of -0.99..+1.52 %) and `insert1` (-0.27 % against -0.57..+1.40 %) are inside the floor. **`pkmix` is the exception**: B is lower in 6 of 6 paired runs (-0.27..-3.56 %) while A2 is lower in 3 of 6 (-0.52..+1.02 %); the median shift of -0.79 % is about 0.25 us of the 30 us a statement and is not separated from the floor by more than the floor's own width, but its sign does not scatter. The latency percentiles agree in direction and size (p50 +0.2..+0.4 us in `point`, `insert1`, `pkmix`, B over A, against A2 over A -0.1..0.0 us; p0 -0.2..+0.7 us).

### 2.3 Server CPU per statement, 24 rounds, A, A2, B (`dk_close.py` M1, `cores` = 1)

The control column is A2 - A and the delta is B - A; brackets are min..max over 24 rounds. The statement shapes are `dk_close.py`'s (`point` = `SELECT * ... WHERE id = k`, `insert1` an issued-key insert, `named` a named mid-chain insert, `ixinsert` an insert with a secondary index, `update1` a point UPDATE); at N = 200 `named` runs 200 statements, every other cell the stated K.

| rows | shape | K | A CPU | A2 CPU | B CPU | A2 - A (floor) | B - A | B - A / A | derived A / B |
|---|---|---|---|---|---|---|---|---|---|
| 200 | `point` | 2,000 | 23.63 us | 23.02 us | 23.79 us | -0.63 us [-7.2, +6.2] | +0.29 us [-6.5, +7.2] | +1.2 % | 42,319/s / 42,036/s |
| 200 | `insert1` | 400 | 23.01 us | 23.12 us | 23.44 us | +0.19 us [-6.8, +9.3] | +0.11 us [-6.4, +6.5] | +0.5 % | 43,468/s / 42,667/s |
| 200 | `named` | 200 | 23.20 us | 23.71 us | 23.13 us | +0.75 us [-7.9, +6.7] | +0.12 us [-7.0, +6.6] | +0.5 % | 43,099/s / 43,235/s |
| 200 | `ixinsert` | 400 | 25.61 us | 25.27 us | 26.05 us | -0.17 us [-7.7, +6.6] | -0.12 us [-7.6, +7.4] | -0.5 % | 39,044/s / 38,392/s |
| 200 | `update1` | 400 | 23.74 us | 23.52 us | 23.82 us | -0.43 us [-7.2, +6.6] | +0.22 us [-6.9, +7.1] | +0.9 % | 42,124/s / 41,983/s |
| 1,000 | `point` | 2,000 | 23.70 us | 23.97 us | 23.97 us | +0.07 us [-7.7, +6.8] | +0.38 us [-6.8, +8.7] | +1.6 % | 42,201/s / 41,718/s |
| 1,000 | `insert1` | 400 | 24.53 us | 24.52 us | 25.07 us | +0.60 us [-7.4, +8.4] | +0.11 us [-6.7, +7.5] | +0.5 % | 40,767/s / 39,886/s |
| 1,000 | `named` | 400 | 24.67 us | 24.91 us | 24.96 us | +0.11 us [-7.7, +6.7] | +0.31 us [-7.7, +7.3] | +1.2 % | 40,540/s / 40,068/s |
| 1,000 | `ixinsert` | 400 | 27.39 us | 28.29 us | 28.36 us | -0.76 us [-9.8, +9.0] | +0.40 us [-9.2, +8.1] | +1.4 % | 36,509/s / 35,259/s |
| 1,000 | `update1` | 400 | 23.62 us | 24.16 us | 24.19 us | -0.02 us [-7.5, +6.2] | +0.46 us [-6.6, +6.3] | +1.9 % | 42,329/s / 41,346/s |
| 10,000 | `point` | 2,000 | 23.52 us | 25.21 us | 23.67 us | +1.36 us [-7.1, +7.8] | +0.00 us [-6.8, +7.3] | +0.0 % | 42,512/s / 42,244/s |
| 10,000 | `insert1` | 400 | 25.85 us | 27.92 us | 26.75 us | +0.99 us [-6.3, +8.8] | +1.16 us [-8.9, +9.4] | +4.5 % | 38,691/s / 37,384/s |
| 10,000 | `named` | 400 | 31.61 us | 32.12 us | 32.24 us | +0.44 us [-40.4, +10.2] | +0.57 us [-35.3, +22.2] | +1.8 % | 31,632/s / 31,017/s |
| 10,000 | `ixinsert` | 400 | 30.39 us | 29.94 us | 30.08 us | -0.65 us [-57.9, +8.8] | -0.48 us [-56.3, +9.8] | -1.6 % | 32,900/s / 33,245/s |
| 10,000 | `update1` | 400 | 24.09 us | 25.14 us | 24.50 us | +1.13 us [-9.0, +41.0] | +0.29 us [-7.4, +5.5] | +1.2 % | 41,506/s / 40,810/s |

**B-A medians: mean +0.25 us, positive in 13 of 15, max +1.16, min -0.48; A2-A medians: mean +0.20 us, positive in 9 of 15, max +1.36, min -0.76.** No B - A median lies outside the control's range in any of the 15 cells; the same-binary control moves by as much as B does (its mean shift is +0.20 us, B's +0.25 us). The sign of B - A is positive in 13 of 15 cells while A2 - A is positive in 9 of 15: a shift of 0.05 us in the mean between B and the control, which 24-round medians cannot resolve (the floor on this host is about 1 us per cell, +-7 us per round). The two methods agree on the size - a shift of at most about 0.3 us (about 1 %) of a 23-32 us statement - and disagree on whether it is there: the CPU method cannot see it and the `ba_census` control pass sees it, in `pkmix` only.

### 2.4 Verdict on DP-R10 (no view on the instance)

**Met within this method's resolution, with one cell at the edge of its floor.** Of the 15 CPU cells and the 6 `ba_census` cells none lies outside its floor by more than the floor's own width; `pkmix` at `cores` 1 reads -0.79 % (6 of 6 paired runs negative) in the control pass and -1.2 % in the first pass, against a floor of about +-1 %. What would settle it is a per-statement profile of the compile head with and without the view check (the absorption step is one type check on each resolved `FROM` oid, `workorder-dp-view.md` DP-R10), which this run did not take; nothing in the data says the check costs more than 0.3 us a statement (0.3 us is 0.9 % of a `pkmix` statement), and a statement over no view runs the same compile it ran at A in every other respect.

## 3. Q2 - DP-R10 with a view on the instance (an unrelated view exists)

Point read of a plain table, B, three servers up at once, 24 rounds, 2,000 statements a block. N1 is the base; N2 is its same-binary control; V holds one view over a different table, so DP-Q18's word is set and each `FROM` name is asked as a view first. Brackets are min..max of the paired differences over 24 rounds.

| rows | arm | server CPU | derived throughput | CPU vs N1 (median [min, max]) | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|---|
| 200 | N1 | 23.07 us | 43,354/s | - | 48,000 | 46.3 us | 49.7 us | 52.1 us | 72.2 us | 93.7 us |
| 200 | N2 | 24.28 us | 41,179/s | +0.01 us [-8.9, +7.5] | 48,000 | 45.9 us | 49.7 us | 52.2 us | 72.8 us | 94.1 us |
| 200 | V | 23.48 us | 42,588/s | +0.37 us [-9.6, +7.4] | 48,000 | 46.3 us | 49.5 us | 51.0 us | 71.6 us | 84.0 us |
| 1,000 | N1 | 23.57 us | 42,432/s | - | 48,000 | 46.5 us | 49.8 us | 52.3 us | 72.0 us | 79.9 us |
| 1,000 | N2 | 23.02 us | 43,435/s | -0.49 us [-7.0, +7.5] | 48,000 | 46.0 us | 49.6 us | 51.6 us | 71.6 us | 79.5 us |
| 1,000 | V | 23.73 us | 42,132/s | +0.40 us [-6.8, +7.6] | 48,000 | 46.0 us | 50.0 us | 52.0 us | 72.8 us | 80.4 us |
| 10,000 | N1 | 23.14 us | 43,206/s | - | 48,000 | 45.9 us | 48.8 us | 50.7 us | 70.9 us | 86.3 us |
| 10,000 | N2 | 23.48 us | 42,599/s | +0.47 us [-6.8, +8.2] | 48,000 | 45.3 us | 49.1 us | 51.0 us | 73.7 us | 122.8 us |
| 10,000 | V | 23.63 us | 42,316/s | +0.27 us [-9.3, +6.8] | 48,000 | 46.1 us | 49.0 us | 50.1 us | 70.0 us | 105.4 us |

**The unrelated view costs at most +0.4 us a point read (+1.7 % of 23.5 us), and the measurement cannot tell it from nothing.** V - N1 is +0.37, +0.40 and +0.27 us at 200, 1,000 and 10,000 rows (all three positive); the floor N2 - N1 is +0.01, -0.49 and +0.47 us. A delta the size of the floor is not a finding; what the three signs say is that +0.4 us is an upper bound and 0 is not excluded. The latency percentiles do not separate the arms (p50 52.1, 52.2, 51.0 us at 200 rows; p99 84.0 us for V is below N1's 93.7 us). **DP-Q18's word is untested against its alternative**: no arm asks nothing on an instance with a view, and no arm asks every name on an instance with none, so this run says what the asked-name path costs (at most 0.4 us), not what the word saves.

## 4. Q3 - the cost of reading through a view

B, `cores = 1`, server CPU per statement over compile and run, 24 rounds, H and H2 hand-written on two identical tables, VIEW through an absorbable view equivalent to H. `rows` is what a statement returns. **The throughput column is derived from CPU.**

| rows | shape | arm | server CPU | derived throughput | CPU vs H (median [min, max]; share of H) | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 200 | `point` (1 row) | H | 25.40 us | 39,367/s | - | 48,000 | 42.4 us | 45.3 us | 51.1 us | 65.5 us | 71.4 us |
| 200 | `point` (1 row) | H2 | 24.98 us | 40,030/s | +0.02 us [-6.7, +5.3] (+0.1 %) | 48,000 | 42.5 us | 45.2 us | 48.7 us | 65.4 us | 72.1 us |
| 200 | `point` (1 row) | VIEW | 29.41 us | 33,996/s | +3.72 us [-1.2, +10.7] (+14.6 %) | 48,000 | 45.6 us | 48.5 us | 53.3 us | 71.9 us | 78.4 us |
| 200 | `join` (1 row) | H | 29.06 us | 34,416/s | - | 24,000 | 50.2 us | 52.9 us | 54.3 us | 77.7 us | 84.9 us |
| 200 | `join` (1 row) | H2 | 29.22 us | 34,223/s | +0.11 us [-8.4, +6.6] (+0.4 %) | 24,000 | 50.4 us | 53.1 us | 54.3 us | 78.2 us | 87.7 us |
| 200 | `join` (1 row) | VIEW | 32.50 us | 30,770/s | +3.19 us [-3.6, +14.9] (+11.0 %) | 24,000 | 52.8 us | 55.7 us | 57.3 us | 84.5 us | 95.1 us |
| 200 | `range` (200 rows) | H | 57.34 us | 17,440/s | - | 2,400 | 126.3 us | 130.1 us | 132.1 us | 152.2 us | 170.7 us |
| 200 | `range` (200 rows) | H2 | 57.66 us | 17,342/s | +0.36 us [-1.9, +4.8] (+0.6 %) | 2,400 | 126.2 us | 130.4 us | 132.3 us | 156.3 us | 189.4 us |
| 200 | `range` (200 rows) | VIEW | 61.34 us | 16,302/s | +4.14 us [+1.9, +7.0] (+7.2 %) | 2,400 | 129.3 us | 133.7 us | 136.2 us | 165.6 us | 234.7 us |
| 1,000 | `point` (1 row) | H | 24.81 us | 40,310/s | - | 48,000 | 42.4 us | 45.2 us | 50.2 us | 64.9 us | 70.6 us |
| 1,000 | `point` (1 row) | H2 | 25.46 us | 39,282/s | +0.08 us [-6.9, +4.3] (+0.3 %) | 48,000 | 42.5 us | 45.3 us | 49.5 us | 65.2 us | 74.5 us |
| 1,000 | `point` (1 row) | VIEW | 27.92 us | 35,820/s | +3.41 us [-2.5, +9.5] (+13.8 %) | 48,000 | 40.4 us | 48.2 us | 52.5 us | 70.9 us | 78.2 us |
| 1,000 | `join` (1 row) | H | 29.34 us | 34,080/s | - | 24,000 | 50.1 us | 52.9 us | 53.8 us | 75.0 us | 86.6 us |
| 1,000 | `join` (1 row) | H2 | 29.53 us | 33,863/s | +0.32 us [-6.2, +10.5] (+1.1 %) | 24,000 | 50.4 us | 53.0 us | 54.0 us | 77.2 us | 88.0 us |
| 1,000 | `join` (1 row) | VIEW | 32.21 us | 31,045/s | +2.96 us [-1.5, +13.9] (+10.1 %) | 24,000 | 52.9 us | 55.5 us | 56.4 us | 81.4 us | 90.0 us |
| 1,000 | `range` (1000 rows) | H | 205.95 us | 4,856/s | - | 2,400 | 458.6 us | 488.4 us | 511.5 us | 660.9 us | 747.7 us |
| 1,000 | `range` (1000 rows) | H2 | 209.62 us | 4,770/s | +3.29 us [-25.3, +19.9] (+1.6 %) | 2,400 | 457.8 us | 489.7 us | 513.4 us | 661.8 us | 756.5 us |
| 1,000 | `range` (1000 rows) | VIEW | 214.99 us | 4,651/s | +9.52 us [-17.0, +32.2] (+4.6 %) | 2,400 | 466.0 us | 498.0 us | 521.9 us | 673.4 us | 758.5 us |
| 10,000 | `point` (1 row) | H | 22.82 us | 43,819/s | - | 48,000 | 42.7 us | 44.9 us | 45.6 us | 61.1 us | 67.1 us |
| 10,000 | `point` (1 row) | H2 | 22.83 us | 43,808/s | +0.06 us [-5.1, +1.7] (+0.3 %) | 48,000 | 42.4 us | 44.9 us | 45.6 us | 59.8 us | 65.8 us |
| 10,000 | `point` (1 row) | VIEW | 26.17 us | 38,206/s | +3.38 us [-2.7, +6.3] (+14.8 %) | 48,000 | 45.8 us | 48.2 us | 49.0 us | 66.1 us | 73.8 us |
| 10,000 | `join` (1 row) | H | 29.62 us | 33,766/s | - | 24,000 | 50.9 us | 53.5 us | 54.4 us | 74.4 us | 81.5 us |
| 10,000 | `join` (1 row) | H2 | 29.83 us | 33,519/s | +0.08 us [-9.1, +1.5] (+0.3 %) | 24,000 | 50.7 us | 53.7 us | 54.5 us | 63.7 us | 76.5 us |
| 10,000 | `join` (1 row) | VIEW | 32.68 us | 30,596/s | +3.03 us [+1.7, +7.9] (+10.2 %) | 24,000 | 53.6 us | 56.4 us | 57.3 us | 80.5 us | 88.3 us |
| 10,000 | `range` (1000 rows) | H | 212.34 us | 4,709/s | - | 2,400 | 462.3 us | 500.2 us | 522.8 us | 664.5 us | 764.4 us |
| 10,000 | `range` (1000 rows) | H2 | 215.54 us | 4,640/s | -2.48 us [-35.8, +14.8] (-1.2 %) | 2,400 | 462.2 us | 497.3 us | 520.4 us | 667.1 us | 748.9 us |
| 10,000 | `range` (1000 rows) | VIEW | 222.32 us | 4,498/s | +6.73 us [-25.5, +24.1] (+3.2 %) | 2,400 | 471.4 us | 505.9 us | 532.5 us | 681.0 us | 763.7 us |

**A view adds a fixed 3.0-4.1 us to a statement, and that cost does not grow with the rows read.** The point read costs +3.72, +3.41 and +3.38 us at 200, 1,000 and 10,000 rows (+13.8..+14.8 %; floor +0.02..+0.08 us); the two-relation key join +3.19, +2.96 and +3.03 us (+10.1..+11.0 %; floor +0.08..+0.32 us); the range returning 200 rows +4.14 us [+1.9, +7.0] (+7.2 %; floor +0.36 us). **The 1,000-row range through a view reads +9.5 us (N = 1,000) and +6.7 us (N = 10,000), +3.2..+4.6 %, but the brackets (-17..+32 us, -26..+24 us) and the floor (+3.3 us at 1,000, -2.5 us at 10,000) say the extra over the fixed 3.4 us is not resolved**: a range statement runs 206-222 us and its CPU is noisier by an order of magnitude. A per-row cost of absorption is not shown and is not excluded; the absorbed block is the hand-written one (`view.md` VW3), and a per-row cost would be a defect. Client latency follows CPU: the view's p50 is +2.2..+3.4 us over H for point and +2.6..+3.0 us for the join, with p0 +2.6..+3.2 us in five of six rows (the fixed cost shows in the best case, as a fixed cost should; the sixth, the point read at 1,000 rows, has a p0 of 40.4 us below H's 42.4 us, one outlier) and **p99 +6.7..+7.6 us for the point read**.

## 5. Q4 - DP-R11: a compile at the depth bound

### 5.1 A chain of single-relation views (1,000 rows)

`nK (id, v) AS SELECT id, v FROM n(K-1)`, `n1` over the table `t`; a point read `SELECT v FROM nK WHERE id = k`, 2,000 a block. A ninth-level view `n9` over `n8` was refused at its create: `0x90000 views nest deeper than 8 (byte 12): this one would be level 9` (the bound is enforced as DP-Q8 states).

| statement | server CPU | derived throughput | CPU vs H (median [min, max]) | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|
| H | 22.65 us | 44,156/s | - | 48,000 | 42.4 us | 44.7 us | 45.5 us | 61.2 us | 66.4 us |
| H2 | 22.57 us | 44,312/s | +0.06 us [-5.7, +5.8] | 48,000 | 42.4 us | 44.8 us | 45.5 us | 60.8 us | 66.0 us |
| n1 | 25.77 us | 38,800/s | +3.19 us [-2.5, +7.3] | 48,000 | 45.2 us | 47.7 us | 48.5 us | 66.8 us | 72.3 us |
| n2 | 28.39 us | 35,221/s | +5.64 us [-0.1, +7.6] | 48,000 | 47.9 us | 50.2 us | 51.0 us | 70.6 us | 76.3 us |
| n4 | 34.17 us | 29,268/s | +11.48 us [+6.0, +21.9] | 48,000 | 53.3 us | 56.0 us | 56.8 us | 80.8 us | 87.7 us |
| n8 | 47.21 us | 21,183/s | +24.36 us [+21.6, +43.7] | 48,000 | 65.7 us | 68.7 us | 69.7 us | 116.1 us | 123.2 us |

### 5.2 A chain where every level joins one more table (1,000 rows)

`m1 = j0 JOIN j1`, `mK = m(K-1) JOIN jK` on the key; the absorbed block at depth d is the (d+1)-way join H_d written by hand. 500 statements a block, 24 rounds. H1b is H1 on an identical copy of `j0`.

| depth | statement | absorbed shape | server CPU | derived throughput | m_d - H_d (median [min, max]; share of H_d) | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | H1 | 2-way | 27.71 us | 36,091/s | - | 12,000 | 47.8 us | 50.6 us | 51.4 us | 62.1 us | 81.2 us |
| 1 | m1 | 2-way | 30.11 us | 33,206/s | +2.32 us [+0.5, +11.0] (+8.4 %) | 12,000 | 50.3 us | 53.1 us | 54.0 us | 64.9 us | 78.2 us |
| 2 | H2 | 3-way | 31.26 us | 31,992/s | - | 12,000 | 51.9 us | 54.4 us | 55.2 us | 63.8 us | 80.3 us |
| 2 | m2 | 3-way | 36.91 us | 27,096/s | +5.59 us [+3.9, +7.3] (+17.9 %) | 12,000 | 56.5 us | 59.7 us | 60.6 us | 70.6 us | 84.4 us |
| 4 | H4 | 5-way | 39.36 us | 25,408/s | - | 12,000 | 59.0 us | 62.0 us | 63.0 us | 79.6 us | 110.7 us |
| 4 | m4 | 5-way | 51.23 us | 19,519/s | +12.06 us [+10.2, +16.0] (+30.6 %) | 12,000 | 70.5 us | 73.6 us | 74.7 us | 93.3 us | 131.7 us |
| 8 | H8 | 9-way | 54.82 us | 18,242/s | - | 12,000 | 73.5 us | 76.9 us | 78.0 us | 91.8 us | 121.9 us |
| 8 | m8 | 9-way | 86.70 us | 11,535/s | +32.44 us [+29.1, +35.3] (+59.2 %) | 12,000 | 102.7 us | 107.6 us | 109.6 us | 135.8 us | 181.2 us |
| 1 | H1b (floor, copy of H1's table) | 2-way | 27.55 us | 36,292/s | -0.10 us [-2.7, +2.7] vs H1 | 12,000 | 48.1 us | 50.6 us | 51.4 us | 60.5 us | 73.7 us |

**The compile cost of a view chain is linear in depth: about 3 us a level for single-relation views, about 4 us a level where each level adds a relation.** Single-relation chain: +3.19, +5.64, +11.48, +24.36 us at depth 1, 2, 4, 8 (3.2, 2.8, 2.9, 3.0 us a level); join chain: +2.32, +5.59, +12.06, +32.44 us (2.3, 2.8, 3.0, 4.1 us a level - the last level steps up slightly, and the nine-way join the statement becomes is itself 54.8 us by hand, so the join chain's relative cost is lower: +59 % against the chain's +108 %). **At the bound a point read through eight views costs 47.2 us of server CPU, 2.1 times the 22.6 us by hand, and a statement that is a nine-way join costs 86.7 us against 54.8 us.** Client p50 follows: 45.5 us (H) to 69.7 us (n8); p99 66.4 us to 123.2 us. The floors are inside +-2.7 us (the join chain's H1b - H1 reads -0.10 us median, [-2.7, +2.7]) and the brackets of the deep arms (n8 +21.6..+43.7 us; m8 +29.1..+35.3 us) lie wholly above them: **the delta at depth 8 is not noise**.

## 6. What this teaches about the engine

1. **DP-R11: depth 8 is affordable, and the bound is a policy, not a cost cliff.** The measured cost at the bound is +24.4 us a statement for a chain of plain views and +32.4 us for a chain of joins, linear in depth (no superlinear term between 1, 2, 4 and 8), on every statement since no compiled-chain cache exists (CX-S6) - a hot point read through an 8-deep chain runs 21,183 /s of server CPU against 44,156 /s by hand. That is a cost an operator who nests eight views pays and sees; it is not one the depth bound protects anyone from, since the bound's job (DP-Q8) is a finite walk and the walk is cheap. **Recommendation: lift `[provisional]` from DP-Q8's 8 on this measurement - Basis: linear cost, +24..+32 us at the bound, nothing near a limit; Trade-off: what was measured is one chain shape (a column-forwarding view per level, a point read) and a join chain at 1,000 rows, not a wide select list, a view with subquery predicates at depth, or a depth-8 chain under a query-layer body, and the bound's number is still the operator's mark (DP-Q8 is (a) as marked 2026-11-22).** No depth above 8 was measured (a ninth level is refused), so whether 16 would also be affordable is a prediction the data does not support; the per-level slope is the only thing it gives.
2. **Absorption costs 3 us a level because it happens at every compile.** The absorbed block is the statement's hand-written expansion (VW3), so the run costs the same; the 3.0-3.7 us is the absorption and the compile work it carries, per level, paid per statement; this run does not attribute it finer (a profile of the compile head would). It is flat in rows (3.7, 3.4, 3.4 us for a point read at 200, 1,000, 10,000 rows) and flat in shape (3.0-3.2 us for a join, 3.4-3.7 for a point), which is what a compile-time constant looks like. That is 14 % of a 23-25 us point read - the cost an OLTP user who wraps a plain table in a view will see - and the lever, if it matters, is a rewrite cache keyed by statement and view oids, which does not exist (CX-S6: no compiled-chain cache is built); this run does not say whether it is worth building (a cache lookup costs something too, and the point read is the cheapest statement there is).
3. **DP-R10's no-view claim holds to about +-0.3 us a statement (1 %), and no better is resolvable here.** Server CPU per statement on this box has a floor near 1 us on 24-round medians (+-7 us per round, `dk_close`'s own range); the throughput method's 6-run floor is +-1 %. A shift of 0.3 us was seen once (`pkmix`, `cores` 1) and not seen in the 15 CPU cells; the honest summary is that DP did not move the standard cells by more than about 1 % and that this run cannot call it zero.
4. **The unrelated view's cost (V) is the first data point for DP-Q18's measured fast path.** At most +0.4 us (+1.7 %) a statement on an instance where a view exists and is never read; whether the `a view has existed` word is worth its complexity over always asking (DP-Q18 (b)) is not answered, since neither side of the choice was run without the other.
5. **Not covered, and a gap rather than a pass.** A non-absorbable body read as a derived table (VW4) is a different cost, one the query layer pays by running the whole body; no cell here runs one (`DISTINCT`, `GROUP BY`, `ORDER BY ... LIMIT` bodies). The `range` rows through a view at 1,000 rows return a different number of rows from the 200-row case, which this file reads as noise and a per-row cost not shown. A view under a Waystone opt-in, a captured plan or an administrator's plan (the plan keys carry the views, DP-S2) was not measured.

## 7. Reproduce

Builds: `git archive <commit> | tar -x -C <dir>`, `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, `cmake --build build --target kds_server`; copy the binary, hash it. Q1: `archive/.../q1.sh` (first pass) and `q1b.sh` (control pass), each calling `tools/ba_census.py --engine kds --bin <copy> --port <P> --shape <s> --durability relaxed --cores <k> --sessions <n> --seconds 8 --warmup 2 --runs 1 --rows 10000 --server-cpus <..> --client-cpus <..> --workdir <..> --max-load 3 --max-wait 30 --out <dir>`. Server CPU: `python3 -I tools/dk_close.py --binA <A> --binB <B> --workdir <dir> --out <json> --skip m2 --rounds 24 --k 400 --k-point 2000`. Q2-Q4: `python3 -I tools/dp_close.py --bin <B copy> --workdir <dir> --out <json> --rounds 24 --port 15771` (Q2-Q4 chain pass: default `--q q2,q3,q4`; the join chain: `--q q4j --depths 1,2,4,8 --k-join 500`). Binaries must be absolute paths. The `q4j` cell was added to the driver after the first Q2-Q4 pass ran and is archived as its own JSON (`q234/dp_close_q4j.json`); `q2`, `q3`, `q4` were not changed by that edit. `*_report.py` and `frag.py` in the archive print every table of this file from the JSON.
