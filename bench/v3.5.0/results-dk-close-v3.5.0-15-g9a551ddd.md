# DK-S4 close: D1 and D2 are invisible in server CPU and in multi-core throughput, and D2 did not lower the refusals

**Thesis.** Interleaved against the opening commit `2c2639df` (A, `v3.5.0-12-g2c2639df`), the engine at `9a551ddd` (B, `v3.5.0-15-g9a551ddd`) is **level with A in every cell measured**: at `cores = 1` no B - A difference of the 15 cells (5 shapes x 200, 1,000 and 10,000 rows) lies outside the same-binary control's own spread (largest |B - A| median 1.77 us of 35 us; the control A2 - A reaches 1.18 us), and at `cores` 2 and 4 no throughput delta lies outside the control either. So **D1's one fetch fewer per write descent is real by the code (DK-S0 counted it) but is below what this method resolves** - under about 1 us of 23-35 us a statement, i.e. under 4 %. **D2 does not reduce the refusals**: over 32 multi-core cells an arm, A took 24 `refused_*` events, its control A2 24, and B 25; B's stale-descent hop fired 373 times in 1,152,000 statements yet B's refusals stayed where A's were. **No client saw an error in any of the 96 multi-core cells** - every refusal was re-run inside the engine. No cell is slower beyond its control.

| field | value |
|---|---|
| date executed | 2026-11-21, build 01:56:42-01:59:12 UTC; M2 and a first M1 pass 01:59:37-02:06:26 UTC; the M1 pass reported here 02:06:44-02:08:04 UTC |
| branch | `worktree-dk-descent-low-key` (worktree `dk-descent-low-key`); tree clean at `9a551ddd` before this file and `tools/dk_close.py` were written (both untracked) |
| commits | **B** = `9a551ddd`, `git describe --tags` = `v3.5.0-15-g9a551ddd`; **A** = `2c2639df`, `v3.5.0-12-g2c2639df` (DK's opening commit; DK-S0..S3 are the three between) |
| work order | `instructions/v3.2.0/workorder-dk-descent-low-key.md` section 5, DK-S4 |
| build | Release (`CMAKE_BUILD_TYPE=Release` read from both `CMakeCache.txt`), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, each from an archive export of its commit extracted under `/home/louis/dk-close-run/{A,B}-src` (no VCS directory), not the shared `./build` |
| **A**, **A2** | `kds_server_A`, sha256 `63dfe5835dc6e11ac3090bad3bf1fe2b521841b0f1c3a42cdd9562d84dbc2f56`; source binary mtime 2026-11-21 01:59:12 UTC, after `2c2639df`'s commit time (00:48:26 UTC). **A2 is the same file started as a second server** (M1: a third concurrent server; M2: a further fresh server per cell): the in-run noise floor. (That hash equals the A binary of DN-S0's file at `v3.4.18-9-g34958393`: the two source trees compile to the same bytes.) |
| **B** | `kds_server_B`, sha256 `ababc74a67254cbb06d254082276ac51811e2920c0d67a28c2989254d0a914df`; source binary mtime 01:59:12 UTC, after `9a551ddd`'s commit time (01:55:58 UTC) |
| binaries run from | copies in `/home/louis/dk-close-run/`; every server was started from a copy |
| device | data files and WAL under `/home/louis/dk-close-run`, `/dev/nvme0n1p1` (ext4, `df -T` at run time); each server's data directory was removed at its stop |
| server | `durability = relaxed`, `buffer_pool_frames` 65,536 (M1) / 131,072 (M2), `log_level = warn`; M1 `cores = 1`, M2 `cores` 2 and 4; ports 15761-15764 (never 15432) |
| host | 8 CPUs shared with other sessions; **no `cc1plus` in any of the 96 M2 cells or the 15 M1 cells**; load average (1 min) 1.9-3.9 over the M1 cells, 2.7-3.9 over M2 cells (`host_before`/`host_after` per cell are in the archive JSON) |
| PostgreSQL floor | not measured for these shapes |
| prior number for this shape | none for DK's shapes: DK-S0 counted fetches, restarts and refusals, not timings, so this file is the timing baseline. As a scale only, DN-S0's tuple-key point read at `v3.4.18-9-g34958393` was 23.7 us (10,000 rows) and 24.7 us (100,000 rows) server CPU; the `point` rows below read 22.9-23.6 us on a different day and host load |

## 1. Method, and what was cut

**Driver** `tools/dk_close.py` (new in this worktree, untracked; `bench/docs/` not yet updated for it). Archive: `bench/v3.5.0/archive/dk-close-v3.5.0-15-g9a551ddd/`.

**M1, `cores = 1`.** Three servers (A, A2, B) up at once, one KWP connection each, their threads pinned to CPUs 4, 5, 6 and **the arm-to-CPU mapping rotated every round** (a first M1 pass with fixed CPUs read A about 6 us above A2 on three cells; one of the three CPUs is slower by that much, so the pass was discarded and re-run with the rotation; its raw JSON stays in the archive beside M2's). Per size N (200, 1,000, 10,000: `--sizes`; a relation's preloaded ids are 2, 4, ... 2N), per shape, **24 rounds**: each arm gets a fresh `BTREE` relation `(id int64, v int64, w int64)` preloaded with N rows (untimed), then runs the same K statements (equal work); the arm order is rotated and reversed per round. **Primary number: server CPU per statement**, the sum of `/proc/<pid>/task/*/schedstat` run time over the server's threads, over the timed block, median of 24 rounds; the bracket after a difference is the min..max over rounds (it is wide, +-7 us, because a round's CPU-slot effect is in it - read the median, not the bracket). **The floor is A2 - A.**

| shape | statement | K a block | what it exercises |
|---|---|---|---|
| `point` | `SELECT * FROM t WHERE id = <even k>` | 2,000 | clustered read lookup (D2's miss path; D1 not involved) |
| `insert1` | `INSERT INTO t (v, w) VALUES (...)` | 400 | issued key, rightmost leaf, write descent |
| `named` | `INSERT INTO t VALUES (<odd k>, 0, 0)` into a relation preloaded with even ids | 400 (200 at N = 200: every odd slot) | named key mid-chain |
| `ixinsert` | `INSERT INTO t (v, w) VALUES (<new v>, 0)`, index `ix` on `v` | 400 | clustered and secondary write descents |
| `update1` | `UPDATE t SET w = <x> WHERE id = <even k>` | 400 | the write lookup (`LocateByTupleKey`) |

At N = 200 the clustered tree is one leaf (a root that is a leaf has no level-1 node), so D1 cannot apply there; at 1,000 rows it is a root over a few leaves and at 10,000 over a few dozen (inferred from row width and page size, not read from the engine).

**M2, `cores` 2 and 4.** One fresh server per cell (relation empty at the cell's start, so the window where the root grows a level is inside every cell), 4 sessions a core (8 and 16 writers), **6,000 statements per writer** (48,000 and 96,000 per cell; equal work, wall time is the throughput), 8 rounds, arm order rotated and reversed per round, servers pinned to CPUs 4..4+cores-1, the driver to CPUs 0-3. `ins1_shared`: every session issues-key `INSERT`s into one relation. `named_ix`: every session inserts a random named key and a random `v` into one relation with an index on `v`, plus one reader a core doing point lookups of random keys (the lookup-miss path) until the writers finish. The contention tallies (`SHOW META`, instance-wide) are read before and after each cell. Throughput is the Python client's, 8-16 threads under one GIL, so it is client-bound; **server CPU per committed write** (from `/proc`) is given beside it as the server's own figure.

**Cut.** `cores = 8` (the host has 8 CPUs shared; 4 is the largest clean cell), `group`/`strict` durability (every cell `relaxed`; the claim is a descent's, not a commit's), 100,000 rows, `DELETE` and `UPDATE` with an index, a PostgreSQL twin, the sampled profile DK-S0 could not take, B's `descent_hops` at `cores = 1` (not read in M1). **Time:** building 2.5 min (both builds in parallel, `-j4` each), M2 plus the discarded M1 pass 6.8 min, the M1 pass reported 1.3 min, a smoke run 0.2 min; **no time was spent waiting on a quiet host** (no compilation ran). Well inside the one-hour milestone budget.

## 2. `cores = 1`: nothing separates B from A

Server CPU per statement, medians of 24 rounds. The control column is A2 - A; the delta is B - A. **Derived throughput** is `1,000,000 / CPU us`, the server-CPU bound on a serial connection (a client's wall rate is lower; derived, not reported by the driver).

| rows N | shape | K | A CPU | A2 CPU | B CPU | A2 - A (floor) | B - A | B - A / A | derived A / B |
|---|---|---|---|---|---|---|---|---|---|
| 200 | `point` | 2000 | 22.89 us | 23.10 us | 22.91 us | +0.20 us [-6.8, +6.9] | -0.28 us [-7.2, +6.7] | -1.2 % | 43,693 / 43,656 /s |
| 200 | `insert1` | 400 | 23.34 us | 23.72 us | 23.29 us | +0.40 us [-6.4, +6.5] | -0.34 us [-6.9, +6.4] | -1.5 % | 42,840 / 42,934 /s |
| 200 | `named` | 200 | 24.03 us | 24.01 us | 23.75 us | -0.19 us [-6.5, +6.2] | -0.17 us [-6.2, +5.4] | -0.7 % | 41,620 / 42,109 /s |
| 200 | `ixinsert` | 400 | 25.91 us | 26.08 us | 25.65 us | -0.25 us [-7.3, +7.5] | -0.47 us [-7.2, +7.0] | -1.8 % | 38,597 / 38,990 /s |
| 200 | `update1` | 400 | 23.96 us | 24.02 us | 24.02 us | +0.17 us [-6.1, +6.5] | +0.21 us [-7.4, +6.0] | +0.9 % | 41,741 / 41,624 /s |
| 1,000 | `point` | 2000 | 23.35 us | 23.67 us | 23.65 us | +0.20 us [-7.0, +6.3] | +0.11 us [-6.9, +6.7] | +0.5 % | 42,831 / 42,288 /s |
| 1,000 | `insert1` | 400 | 24.62 us | 24.76 us | 24.49 us | +0.03 us [-7.0, +8.3] | +0.13 us [-6.9, +7.6] | +0.5 % | 40,615 / 40,831 /s |
| 1,000 | `named` | 400 | 25.06 us | 26.19 us | 25.89 us | +1.18 us [-8.4, +8.2] | +0.78 us [-8.8, +8.4] | +3.1 % | 39,900 / 38,623 /s |
| 1,000 | `ixinsert` | 400 | 27.46 us | 27.71 us | 27.67 us | +0.35 us [-13.0, +10.2] | -0.18 us [-11.5, +14.2] | -0.7 % | 36,418 / 36,139 /s |
| 1,000 | `update1` | 400 | 24.20 us | 23.84 us | 23.75 us | -0.27 us [-18.7, +14.3] | -0.23 us [-18.9, +6.1] | -0.9 % | 41,331 / 42,107 /s |
| 10,000 | `point` | 2000 | 23.44 us | 23.82 us | 23.57 us | +0.12 us [-7.0, +7.8] | +0.04 us [-7.0, +7.5] | +0.2 % | 42,669 / 42,426 /s |
| 10,000 | `insert1` | 400 | 26.39 us | 26.31 us | 25.92 us | +0.45 us [-8.1, +29.2] | +0.01 us [-8.3, +7.3] | +0.0 % | 37,892 / 38,575 /s |
| 10,000 | `named` | 400 | 35.36 us | 34.46 us | 32.85 us | +1.00 us [-12.8, +29.5] | -1.77 us [-11.2, +30.4] | -5.0 % | 28,281 / 30,442 /s |
| 10,000 | `ixinsert` | 400 | 29.98 us | 29.20 us | 30.93 us | -0.24 us [-9.6, +9.0] | +0.58 us [-10.1, +48.2] | +1.9 % | 33,359 / 32,335 /s |
| 10,000 | `update1` | 400 | 25.47 us | 24.72 us | 25.43 us | -0.77 us [-6.9, +10.3] | +0.37 us [-7.0, +12.3] | +1.4 % | 39,264 / 39,329 /s |

How to read it. **No B - A median exceeds the control's range across the 15 cells.** The largest B - A is `named` at 10,000 rows (-1.77 us, -5.0 %, B faster), against a control of +1.00 us and a B - A2 of -1.29 us; its per-round bracket runs to +30 us, which is the host. The nearest thing to a slowdown, `named` at 1,000 rows (+0.78 us, +3.1 %), has a control of +1.18 us (A2 slower than A by more), so it is the floor and not a regression. The 15 medians of B - A scatter around zero (seven negative, eight positive), which is what noise does. **`point` is nil as the work order claims** (-0.28, +0.11, +0.04 us at the three sizes), and D1's claim on the write cells (`insert1`, `named`, `ixinsert`, `update1`) is **not resolvable**: a fetch of a page already resident costs a fraction of the 23-35 us a statement, and the run's floor (about 1 us on medians of 24 rounds) is larger than that fraction. The data neither confirms nor refutes a saving of a few hundred ns; it shows there is none above about 1 us and no cost.

The sweep: statement cost is flat in N from 200 to 10,000 for `point`, `insert1` and `update1` (about 23-26 us) and rises for `named` at 10,000 rows (35 us against 24 us at 200), which this run does not attribute (a mid-chain insert's leaf split and slot shift are the candidates; not measured); a pk point lookup does not scale with rows, and the three sizes show it.

## 3. Client latency distributions

Wall time around one statement over the loopback with the Python KWP client (so it carries the client; **not** a server cost), pooled over the 24 rounds, same runs as section 2. Medians differ between arms by up to 2-5 us (`named` at 10,000 rows: A 48.8, A2 54.2, B 45.9 us), the spread of the same binary against itself being as large; no percentile separates B from A beyond A2.

| rows N | shape | arm | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| 200 | `point` | A | 48,000 | 46.2 us | 48.8 us | 49.8 us | 68.8 us | 77.7 us |
| 200 | `point` | A2 | 48,000 | 46.1 us | 48.9 us | 50.0 us | 69.0 us | 78.4 us |
| 200 | `point` | B | 48,000 | 45.4 us | 48.7 us | 49.6 us | 68.1 us | 75.6 us |
| 200 | `insert1` | A | 9,600 | 38.3 us | 40.8 us | 42.5 us | 57.4 us | 78.4 us |
| 200 | `insert1` | A2 | 9,600 | 38.4 us | 41.1 us | 43.2 us | 58.0 us | 82.3 us |
| 200 | `insert1` | B | 9,600 | 37.9 us | 40.6 us | 41.9 us | 57.1 us | 79.9 us |
| 200 | `named` | A | 4,800 | 37.9 us | 40.4 us | 42.9 us | 58.1 us | 112.7 us |
| 200 | `named` | A2 | 4,800 | 37.7 us | 40.5 us | 42.9 us | 58.3 us | 112.7 us |
| 200 | `named` | B | 4,800 | 37.7 us | 40.4 us | 42.5 us | 58.8 us | 112.5 us |
| 200 | `ixinsert` | A | 9,600 | 40.1 us | 42.6 us | 44.7 us | 61.4 us | 90.5 us |
| 200 | `ixinsert` | A2 | 9,600 | 40.1 us | 42.8 us | 45.4 us | 61.7 us | 94.7 us |
| 200 | `ixinsert` | B | 9,600 | 40.1 us | 42.7 us | 44.4 us | 61.1 us | 94.8 us |
| 200 | `update1` | A | 9,600 | 38.0 us | 41.1 us | 42.7 us | 57.5 us | 76.6 us |
| 200 | `update1` | A2 | 9,600 | 36.9 us | 41.2 us | 42.7 us | 57.6 us | 76.3 us |
| 200 | `update1` | B | 9,600 | 38.3 us | 41.0 us | 43.6 us | 58.0 us | 77.6 us |
| 1,000 | `point` | A | 48,000 | 46.1 us | 48.9 us | 51.0 us | 71.0 us | 79.2 us |
| 1,000 | `point` | A2 | 48,000 | 46.1 us | 48.9 us | 50.5 us | 70.5 us | 78.6 us |
| 1,000 | `point` | B | 48,000 | 46.1 us | 49.0 us | 50.8 us | 71.2 us | 79.1 us |
| 1,000 | `insert1` | A | 9,600 | 37.1 us | 41.7 us | 43.1 us | 58.7 us | 84.2 us |
| 1,000 | `insert1` | A2 | 9,600 | 36.4 us | 41.9 us | 44.4 us | 59.8 us | 93.7 us |
| 1,000 | `insert1` | B | 9,600 | 37.0 us | 41.8 us | 43.6 us | 59.6 us | 91.1 us |
| 1,000 | `named` | A | 9,600 | 37.5 us | 41.3 us | 42.3 us | 59.8 us | 128.0 us |
| 1,000 | `named` | A2 | 9,600 | 38.8 us | 41.7 us | 44.8 us | 62.8 us | 138.7 us |
| 1,000 | `named` | B | 9,600 | 38.4 us | 41.4 us | 43.2 us | 61.6 us | 126.2 us |
| 1,000 | `ixinsert` | A | 9,600 | 41.3 us | 44.0 us | 45.8 us | 66.4 us | 154.6 us |
| 1,000 | `ixinsert` | A2 | 9,600 | 41.3 us | 44.1 us | 45.8 us | 67.0 us | 151.3 us |
| 1,000 | `ixinsert` | B | 9,600 | 41.0 us | 43.6 us | 44.9 us | 70.5 us | 174.0 us |
| 1,000 | `update1` | A | 9,600 | 33.7 us | 41.0 us | 42.4 us | 57.8 us | 97.5 us |
| 1,000 | `update1` | A2 | 9,600 | 35.6 us | 40.8 us | 41.8 us | 56.1 us | 88.1 us |
| 1,000 | `update1` | B | 9,600 | 34.4 us | 40.8 us | 42.4 us | 57.4 us | 80.2 us |
| 10,000 | `point` | A | 48,000 | 46.3 us | 49.2 us | 50.4 us | 70.4 us | 82.1 us |
| 10,000 | `point` | A2 | 48,000 | 46.3 us | 49.4 us | 50.9 us | 71.9 us | 87.4 us |
| 10,000 | `point` | B | 48,000 | 46.3 us | 49.4 us | 51.0 us | 71.4 us | 83.5 us |
| 10,000 | `insert1` | A | 9,600 | 38.0 us | 42.9 us | 43.9 us | 59.4 us | 95.7 us |
| 10,000 | `insert1` | A2 | 9,600 | 37.8 us | 43.0 us | 44.0 us | 60.0 us | 105.8 us |
| 10,000 | `insert1` | B | 9,600 | 36.8 us | 42.8 us | 43.7 us | 59.2 us | 105.2 us |
| 10,000 | `named` | A | 9,600 | 40.3 us | 43.6 us | 48.8 us | 108.5 us | 193.9 us |
| 10,000 | `named` | A2 | 9,600 | 40.7 us | 43.7 us | 54.2 us | 107.9 us | 197.4 us |
| 10,000 | `named` | B | 9,600 | 40.5 us | 43.3 us | 45.9 us | 102.8 us | 187.0 us |
| 10,000 | `ixinsert` | A | 9,600 | 42.4 us | 45.3 us | 47.2 us | 66.5 us | 115.1 us |
| 10,000 | `ixinsert` | A2 | 9,600 | 42.7 us | 45.4 us | 47.1 us | 68.2 us | 127.5 us |
| 10,000 | `ixinsert` | B | 9,600 | 40.7 us | 45.4 us | 48.9 us | 68.9 us | 120.8 us |
| 10,000 | `update1` | A | 9,600 | 36.2 us | 41.6 us | 43.7 us | 60.0 us | 117.0 us |
| 10,000 | `update1` | A2 | 9,600 | 35.4 us | 41.7 us | 43.7 us | 59.8 us | 97.4 us |
| 10,000 | `update1` | B | 9,600 | 35.4 us | 41.5 us | 46.6 us | 60.4 us | 106.4 us |

## 4. Multi-core: throughput is level, refusals did not fall, no client saw one

Eight rounds a cell, arm order rotated. Throughput is the median of 8 cells (min..max) of **committed writes per second of wall time**; the control is A2. Server CPU per write is `/proc` CPU over the writers' interval divided by committed writes (in `named_ix` the readers' statements are in the numerator too). Paired deltas are taken within a round (B/A - 1, A2/A - 1; median, min..max over the 8 rounds).

| cores | cell | arm | throughput | server CPU a write | refusals in 8 cells | structural_reruns | cells with a refusal | `descent_hops` | client errors |
|---|---|---|---|---|---|---|---|---|---|
| 2 | `ins1_shared` | A | 28,430 /s [28,217, 28,714] | 31.04 us | 0 | 0 | 0 of 8 | n/a (field absent in A) | 0 |
| 2 | `ins1_shared` | A2 | 28,441 /s [27,153, 28,628] | 30.82 us | 4 | 4 | 4 of 8 | n/a | 0 |
| 2 | `ins1_shared` | B | 28,410 /s [28,040, 28,789] | 31.07 us | 3 | 3 | 3 of 8 | 8 | 0 |
| 2 | `named_ix` | A | 19,594 /s [19,402, 19,841] | 46.84 us | 4 | 3 | 4 of 8 | n/a | 0 |
| 2 | `named_ix` | A2 | 19,649 /s [19,284, 19,826] | 46.08 us | 2 | 2 | 2 of 8 | n/a | 0 |
| 2 | `named_ix` | B | 19,510 /s [19,036, 19,833] | 46.67 us | 6 | 4 | 6 of 8 | 2 | 0 |
| 4 | `ins1_shared` | A | 26,818 /s [26,622, 27,106] | 36.43 us | 4 | 4 | 4 of 8 | n/a | 0 |
| 4 | `ins1_shared` | A2 | 26,900 /s [22,980, 27,047] | 36.36 us | 4 | 4 | 4 of 8 | n/a | 0 |
| 4 | `ins1_shared` | B | 26,748 /s [24,774, 27,054] | 36.05 us | 5 | 5 | 5 of 8 | 36 | 0 |
| 4 | `named_ix` | A | 18,058 /s [17,167, 18,321] | 56.81 us | 16 | 14 | 6 of 8 | n/a | 0 |
| 4 | `named_ix` | A2 | 18,136 /s [17,984, 18,495] | 55.81 us | 14 | 12 | 7 of 8 | n/a | 0 |
| 4 | `named_ix` | B | 18,171 /s [17,957, 18,378] | 55.51 us | 11 | 9 | 6 of 8 | 327 | 0 |

"Refusals" is the sum of `refused_btree_descend`, `refused_btree_lookup`, `refused_btree_parent`, `refused_index_descend` and `refused_index_parent`; every one that occurred was `refused_btree_descend`, `refused_index_descend` or `refused_btree_lookup` (the two `parent` tallies stayed 0). Totals over the 32 cells an arm: **A 24 refusals, A2 24, B 25**; `structural_reruns` A 21, A2 22, B 21; cells with any refusal A 14, A2 17, B 20 of 32.

| cores | cell | B/A - 1 throughput | A2/A - 1 (floor) | B/A - 1 server CPU a write |
|---|---|---|---|---|
| 2 | `ins1_shared` | -0.06 % [-2.04, +1.67] | +0.17 % [-5.14, +1.16] | +0.1 % |
| 2 | `named_ix` | -0.11 % [-4.06, +1.58] | +0.28 % [-2.10, +2.03] | -0.4 % |
| 4 | `ins1_shared` | -0.39 % [-7.28, +0.79] | +0.07 % [-14.58, +1.60] | -1.0 % |
| 4 | `named_ix` | +0.37 % [-0.72, +6.07] | +0.62 % [-1.46, +5.65] | -2.3 % |

(The last column compares medians of the two arms' per-cell CPU figures, not paired.)

Findings. **(1) No cell is slower beyond its control**: all four paired B/A medians lie within 0.4 % of zero and inside the A2/A bracket. The one low B cell (`cores` 4 `ins1_shared`, 24,774 /s) is matched by a 22,980 /s excursion in A2: host. **(2) The refusals did not fall.** DK-S0 saw stale-descent refusals in 5 of 12 census cells; here every arm, A2 included, took refusals in 14-20 of 32 cells (more cells, longer cells). B's total (25) equals A's (24) and A2's (24); at `cores` 4 `named_ix` B is lower (11 against 16 and 14) but at `cores` 2 `named_ix` it is higher (6 against 4 and 2), and a gap of 3 between A and A2 on a count of 24 says none of these differences is a result. A refusal is rare either way: 24 over 1,152,000 statements an arm. **(3) D2 did fire**: B's `descent_hops` counted 8, 2, 36 and 327 hops in the four cells' 8 rounds (373 hops over 1,152,000 statements, 0.03 %; most at `cores` 4 `named_ix`, the cell that also has the most refusals), so some stale descents that A restarted from the root B answered with a hop; **but the refusals still taken in B are ones a hop does not answer** (more than 8 hops, or a write from the named root, which D2 excludes, DK-Q10) - the run does not attribute each refusal, and DK-S0's reading that they are the stale root's window remains unconfirmed. **(4) No client saw an error**: all 96 cells recorded zero client-visible errors by code; the refused tallies count the engine's internal refusals, which its re-run (`structural_reruns`, `index_maintain.cpp`'s retry) answered. A refusal is a cost - 24 re-runs in 1.15 million statements - not a failure, here.

## 5. What this teaches about the engine

1. **A resident-page fetch is cheap enough that D1 does not show.** Every write descent used to take its leaf twice (DK-S0: 1.00 second fetches a descent) and now takes it once, and the CPU per `insert1`, `named`, `ixinsert` and `update1` statement is unchanged within about 1 us. A statement is 23-35 us, of which the descent, the row write, the log record and the KWP frame are all parts; the second fetch is a latch and a hash probe on a page already in the pool. D1's value is the exclusive-at-once ordering (no release between the shared look and the exclusive take, which is where a stale descent could slip in), not a measurable CPU saving; that is a statement about this method's resolution (a floor of about 1 us on 24 rounds), not a proof of zero.
2. **D2 shortens some restarts but does not remove the refusals.** The expectation recorded in DK-Q3 was that a move right in place of a restart would lower the refusals; 373 hops happened and the refusal count did not move. Either the refusals are of the hop-excluded kind (the named root, a root that grew a level) or a hop that follows a restart's cause does not end it; the engine's counters do not say which. If the refusals matter, the next measurement is a per-cause tally, not another throughput run.
3. **Refusals cost nothing a client can see here.** 96 cells, 1.15 million statements an arm in the multi-core cells, zero client errors: the re-run layer swallows every refusal. The risk a refusal carries is to a statement that cannot be re-run (one that wrote before the descent failed), which these single-statement autocommit inserts never are.
4. **The multi-core cells are client-bound.** 26,000-28,000 inserts /s on 2 and on 4 cores (and 18,000-19,600 /s for `named_ix`) at 31-37 us (47-57 us for `named_ix`) of server CPU a write is a Python client under one GIL; server CPU a write rises from 31 to 36 us going from 2 to 4 cores, the shared relation's contention, and is the same in A and B. Nothing here approached a server limit; DK's descent cost cannot show in a client-bound throughput (rule 4b: this run found the client's limit, not the engine's).

## 6. Reproduce

`python3 -I tools/dk_close.py --binA <A> --binB <B> --workdir <dir on a block device> --out <json> --rounds 24 --k 400 --k-point 2000 --writes-per-session 6000 --m2-rounds 8` (the reported M1 pass is `--skip m2`; M2 ran in the first invocation, which used `--rounds 16` and whose M1 half is the discarded fixed-CPU pass; that run predates the driver's CPU rotation). Binaries must be absolute paths. Archive files: `m1-rotated-cpus.json` (reported M1), `run1-m2-and-m1-fixed-cpus.json` (reported M2, discarded M1), their logs and heartbeats.
