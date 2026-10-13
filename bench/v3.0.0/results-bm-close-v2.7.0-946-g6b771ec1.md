# BM close: the scan row is 31.0 ns, 54 % under the opening commit and 1.07 times PostgreSQL 18, and the every-row `UPDATE` pays 4.7 % for it

**Thesis.** On `worktree-bm-scan-kernel-open`, B (`6b771ec1`, `v2.7.0-946-g6b771ec1`: BM-S1's instrument, S2's VM-owned slot
loop, S3's and S4's scan kernel) costs **31.0 ns of server CPU per scanned row** on BN's shape (`SELECT * FROM s WHERE w = ?`,
10,000 rows, `cores = 1`, `relaxed`, every page resident, literal path) against **70.0 ns for A** (`fa80da9d`,
`v2.7.0-940-gfa80da9d`, BM's opening commit) in the same interleaved run: 32.2 million rows/s against 14.3, a fall of 38.1 ns
(-54.4 %). PostgreSQL 18.6, vacuumed, on the same host and pinning, costs 29.1 ns, so the ratio the opening measured at 2.41 is
**1.07**. Switching the kernel off (`scan_kernel = off`, same B binary) gives 56.4 ns: the VM-owned slot loop alone (S2) is
13.4 ns of the 38.1, the kernel (S3 and S4) the other 25.2. The row no longer depends on the relation's width (35.6-44.3 ns on
the 13-column relation against 88.7-109.8 for A) or on a second conjunct (+0.4 ns against +11.6).

- **BM-Q12's guard, as the order set it.** Two of the three guard cells read **within the A-against-A' spread** and the
  third does not hold (section 3):
  - the 100 % output scan: B - A = -41 µs of 2,095 (-2.0 %), spread 63 µs; B is slower than A in 2 of 8 runs;
  - the point `SELECT`: B - A = +0.46 µs of 28.2 (+1.6 %) on `bn_census`'s `select-pk`, spread 0.89 µs, and +0.4 to +0.8 µs
    on the census's four relation sizes; B is slower than A in 6 of 8 runs, and `scan_kernel = off` reads the same;
  - the every-row `UPDATE` (1,000 rows): **not held. B - A = +34.5 µs of 734 (+4.7 %) in the 8-run cell and +31.4 µs of 743
    (+4.2 %) in an independent 5-run re-measure: B is slower than A in 13 of 13 runs**, the same +35 µs with the kernel off. The
    bar as written read "within" on the 8-run cell (its spread, 38.4 µs, is one run's gap; the typical gap is 9.7 µs) and
    "slower than the spread" on the re-measure (spread 10.7 µs). The layout probe moves the cell by -1.8 %, so layout alone
    does not account for +4.2 %.
- **A and A' are the same bytes.** The rebuild of A from a second export came out byte-identical (sha256 `c4f442ae...`
  for both), so the spread between them bounds the harness (two server instances of one executable), not code layout, which
  the order's BM-Q12 revision assumed it would include. A layout probe (A's source with `-falign-functions=64`) was added
  to bound it (section 4).
- **The run was cut to the operator's one-hour budget** (section 1): the PostgreSQL twin has 4 runs instead of 8 and no
  not-vacuumed pass, `bn_census` ran at 1,000 rows only, the profile covers `bn` alone. Nothing below is extrapolated across a cut.

Each section leads with its finding. The drivers are documented in their own docstrings (`bench/README.md`: `bench/docs/`
stays closed).

| Item | Value |
|---|---|
| Executed | 2026-10-13, UTC, from the drivers' output and the run scripts' time records. Builds 04:43:44-04:52:39. **Census** (the 20 cells of BM-S1 plus `rows-200`, `out-200`, `out-1k`; 5 servers; 8 runs): 05:04:50-05:27:56. PostgreSQL twin, vacuumed: run 0 05:27:56-05:29:5x (stopped when the plan was cut to the time budget; kept), runs 1-3 05:35:00-05:38:15. `bn_census` at 1,000 rows (4 servers; 8 runs): 05:30:11-05:35:00. Profile of B (frame-pointer build), `bn`: 05:38:15-05:38:26. Layout probe on the every-row `UPDATE`: 05:39:46-05:41:58 (a first attempt at 05:38:29 failed to connect a server and wrote no data) |
| Worktree / branch | `bm-scan-kernel-open`, branch `worktree-bm-scan-kernel-open`, HEAD `6b771ec1` |
| Commits measured | **A** = `fa80da9dbd654d3dfd5456d75c45adcf31d50712`, committed 2026-10-13 00:40:21 UTC, `v2.7.0-940-gfa80da9d` (BM's opening; its code is `d0022b19`'s). **B** = `6b771ec11701b394870cd60f269545a6a34976cb`, committed 2026-10-13 04:41:04 UTC, `v2.7.0-946-g6b771ec1`. Between them: `f5a7efb2` (BM-S1's `LookupCommit` count: one relaxed add per window lookup, zero on the floor path), `23dfdbb8` (BM-S1's results), `c5a724f4` (S2), `7f1d1f70` (S3), `6b771ec1` (S4). The A-to-B delta carries all of them |
| Tree | The worktree was **dirty** while the runs were taken: CLA's BM-S6 text edits (`CLAUDE.md`, `docs/inflight/known-gaps.md`, `docs/spec/parser-v2.md`, `include/kds/exec/step_vm.hpp`, `kds.conf.sample`, `manual/sql/sql.md`) and this stage's driver edits (`tools/bm_scan.py`, `tools/bn_census.py`, `tools/bn_pg_scan.py`, `tools/bn_parts.json`, new `tools/bm_close_table.py`). **None of it is in a measured number**: every binary was built from `git archive <commit>` into the scratchpad, never from the worktree |
| Binary provenance | Plain Release builds of three exports, each copied before the first cell, **hashed**, and every server started from the copy (`archive/.../binaries.txt`): **A** and **A'** both `c4f442aebe7e3a7d4ea504b94e16d757c1770ffb4ae71fcae09b075faaab45d6` (source binaries' mtimes 04:45:27 and 04:48:49, from separate exports `src-A` and `src-A2` and separate build trees: byte-identical); **B** `963894d08781bd2d37b682b1a453be210ba7590e91fc157129ae8b272ce069ca` (mtime 04:46:56, 5 min 52 s after its commit, so not older than HEAD); **B-off** is the B file with `scan_kernel = off` in its config; **A-al64** (layout probe, A's export with `-DCMAKE_CXX_FLAGS=-falign-functions=64`) `6bde0dde30bd66e928d75902b9fef97e141b29932a3434acffe528415ed2c8a5` (mtime 04:52:39); frame-pointer build of B (`-fno-omit-frame-pointer`, for the profile only) `1455b6f8597e9a00ac2a8b0962ab0041398ab0a0ffd57d63844dc3845dddb103`; sampler `kds_sampler.so` `df1e67bae6a60f47ef05cd916c94938215287195bdc802b04730c414b787379f`. A binary older than its commit: none |
| Build | `CMAKE_BUILD_TYPE=Release` (read from each `CMakeCache.txt`, `-O3 -DNDEBUG`), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`; gcc 13.3.0 |
| Host / device | 8 vCPU AMD EPYC 9V74 (4 cores x 2 threads; siblings 0-1, 2-3, 4-5, 6-7), Linux 7.0.0-1014-azure, 16 GB. **Data files and WAL under `/home/cdkbs/bench-runs/bm-s6/` on `/dev/root`, ext4, not tmpfs** (`df -T`, `archive/.../host.txt`); the binaries' copies are on the same device |
| Pinning | Servers (all of them, one process each, only one ever busy) on CPU 2, driver on CPU 4, PostgreSQL backend on CPU 2; the SMT siblings 3 and 5 and CPUs 0, 1, 6, 7 were left to whatever else ran |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, Waystone at its defaults (on); a fresh server and data file per run and arm; `scan_kernel = off` written for the B-off arm only (never to A, whose binary predates the key). Ports 32401-32405 (census), 32501-32504 (`bn_census`), 32421 (profile), 32851-32854 (probe); PostgreSQL 15434; none the default 15432 |
| PostgreSQL | 18.6, `/home/cdkbs/pg-bench-ba`, port 15434, defaults except port and logging (`shared_buffers` 128 MB, `max_parallel_workers_per_gather` 2, `jit` on, `autovacuum` on); every plan a plain `Seq Scan` (`archive/.../pg-b.json`, plans of run 0); `VACUUM (ANALYZE)` after each load |
| Host load | Shared host: other sessions' test suites kept the load average at 14-28 until about 05:02, which is why the measurement waited. **Every census cell started in a quiet window** (section 8): load 0.62-2.20, no other session's task on the server's or driver's CPU, SMT siblings under 20 % busy; 10 of 248 census cell-runs started above the 2.0 load gate (2.01-2.20, the measurement's own two busy tasks) and are listed in Appendix B. `bn_census` and PostgreSQL cells: Appendix B |
| Drivers | The measuring versions: `tools/bm_scan.py` sha256 `1896ef20...`, `tools/bn_census.py` `d7b80765...`, `tools/bn_pg_scan.py` `344342f5...` (all three unchanged from 05:02 to the end of the run); `tools/bm_close_table.py` (new, tables only) `736e0e66...`; `tools/bn_parts.json` `91b0c2c3...` (one regex alternative added after the profile ran, section 6). Full hashes: `archive/.../host.txt` |
| Suite | **Not run in this session.** No engine code changed |

## 1. What was run, and what was cut to the operator's time budget

**Standing rule, operator, 2026-10-13: a milestone's close measurement runs within about one hour, building included.** This one
began at 04:43 (builds) and was to end its measuring by 05:50. At 05:30 the plan was cut to fit; **what follows was cut to
that budget, and for no other reason**:

- the PostgreSQL twin was planned at 8 runs vacuumed and 8 not vacuumed; it ran **4 vacuumed runs** (run 0 from the first plan,
  stopped after its first complete run, plus 3) and **no not-vacuumed pass**. The ratios below are against the vacuumed table,
  the harder floor;
- `bn_census` (point statements, every-row `UPDATE`) was planned at 200, 1,000 and 10,000 rows; it ran **at 1,000 rows only**,
  with four servers (A, A', B, B-off; no layout-probe server), 8 runs. The every-row `UPDATE` therefore has one size;
- the layout probe on the every-row `UPDATE` ran as its own short cell: 5 runs, four servers (A, A', B, A-al64), after a
  first attempt at 05:38:29 that failed to connect a server and wrote no data; it doubles as an independent re-measure of A, A' and B;
- the profile was planned for `bn` and `wide-h`; it ran for **`bn` only** (20,000 literal scans of B's frame-pointer build);
- the writers cell was not repeated, as the brief says (BM-S5 was struck at BM-S1).

Cut: no cell of the census (all 23 ran, 8 runs, 5 servers). Runs were kept at 8 wherever the cell ran at all.

**The census** (`tools/bm_scan.py census --arm ...`): one fresh server per arm and run, five arms (A, A', B, B-off, A-al64) loaded
with the same relations, and every cell measured on all five **block by block**: 12 blocks, the five servers visited in an order
rotated by one place per block and per run (each takes each position equally often), the same statements on each, literal and
prepared paths alternating. Flags to counts: `--scan-ops 192` scans per path per run on a 10,000-row cell (1,536 pooled per
path and arm over 8 runs); `rows-200` runs x8 of that, `rows-1k` x4, `rows-100k` /4 (48 per run), `sel-100` /2, `out-200` x4,
`out-1k` x2; `--point-ops 3000` primary-key `SELECT`s per path per relation. **Cells:** BM-S1's 20 (`rows-1k`, `bn`, `rows-100k`,
`bn-recent`, `sel-0/1/10/100`, `wide-h`, `wide-h-k`, `wide-a/c/d/e/f/g/k/m`, `nul-0`, `nul-50`) and three the sweep rule asks
for and BM-S1 did not run as census cells: `rows-200`, `out-200`, `out-1k` (the 100 % output scan at 200 and 1,000 rows).
The net row is `(scan CPU - point CPU on the same relation, arm, path and run) / rows scanned`, per run, reported as the median
over 8 runs with [min, max]; deltas are **paired by run**. Every run checks every arm's `ANALYZE` of one statement per cell
and a digest of its rows (Appendix B): **all 23 cells, all five arms: identical rows, and identical `examined=`, `matched=`,
`sel=`, `pages=`, `spills=`**.

**The bar (BM-Q12), fixed in `tools/bm_close_table.py`'s docstring before the census was read.** For a cell and run `r`,
`S` = the largest `|A_r - A'_r|` over the runs. B is **within the spread** when `|median_r (B_r - A_r)| <= S`, slower or
faster when it falls outside. A second reading was **added after the census was read**, because one outlying run sets `S`
on some cells: the *typical gap*, the median over runs of `|A_r - A'_r|`, with the verdict also saying where the median `B - A`
stands against it. Only the 100 % output scan, the point `SELECT` and the every-row `UPDATE` are bars; every other cell carries
the same reading as information.

## 2. The scan row: 31.0 ns, flat in the row count, 54 % under A on BN's shape and on the cells that reject most rows

**On every cell that rejects most of what it scans, B costs 30-48 ns a row and A 69-110.** The saving is the same at 200,
1,000, 10,000 and 100,000 rows (`bn` shape: B 36.3, 32.3, 31.0, 30.2 ns; A 75.7, 70.8, 70.0, 69.3), so it is a per-row saving.
The statement's fixed part beyond a point `SELECT` is about 1.1 µs in both (5.3 ns x 200 rows for B, 5.7 x 200 for A), so the
once-per-statement kernel compile does not show at the net-row level. The literal and prepared paths agree to within 1.3 ns on
every delta (the prepared tables are in the census file; the tool prints both).

**literal path: net row, ns (median over 8 runs [min, max]); the net row is (scan CPU - point-select CPU on the same relation, arm, path and run) / rows scanned**

| cell | rows scanned | A | A' | B | B-off | A-al64 | BM-S1 file, f5a7efb2 (literal) |
|---|---|---|---|---|---|---|---|
| rows-200 | 200 | 75.7 [67.9, 87.0] ns | 74.7 [69.0, 81.2] ns | 36.3 [30.9, 38.6] ns | 60.0 [54.7, 69.8] ns | 70.1 [66.8, 81.9] ns | not in that file |
| rows-1k | 1,000 | 70.8 [69.4, 74.0] ns | 75.9 [70.5, 79.2] ns | 32.3 [29.8, 34.6] ns | 57.5 [56.4, 65.2] ns | 71.6 [68.2, 74.1] ns | 66.2 [64.7, 71.0] ns |
| bn | 10,000 | 70.0 [68.8, 71.8] ns | 73.3 [72.6, 76.2] ns | 31.0 [30.8, 32.6] ns | 56.4 [56.0, 58.9] ns | 68.5 [66.9, 69.5] ns | 65.5 [64.4, 70.2] ns |
| rows-100k | 100,000 | 69.3 [67.9, 70.4] ns | 72.1 [71.1, 80.6] ns | 30.2 [29.6, 32.0] ns | 55.3 [54.9, 57.5] ns | 66.8 [66.3, 68.2] ns | 65.0 [64.2, 69.8] ns |
| bn-recent | 10,000 | 70.7 [69.2, 73.0] ns | 74.5 [73.5, 76.6] ns | 31.6 [31.1, 33.9] ns | 57.1 [56.3, 59.3] ns | 68.7 [67.2, 72.2] ns | 66.5 [65.1, 70.1] ns |
| sel-0 | 10,000 | 70.3 [68.3, 75.3] ns | 73.5 [71.9, 75.9] ns | 31.5 [30.6, 33.7] ns | 56.5 [55.9, 58.3] ns | 67.8 [66.8, 71.5] ns | 65.0 [64.6, 70.2] ns |
| sel-1 | 10,000 | 72.0 [69.9, 75.4] ns | 74.8 [73.8, 77.2] ns | 32.7 [32.2, 35.9] ns | 58.5 [57.3, 63.0] ns | 69.7 [68.1, 71.6] ns | 66.8 [66.1, 71.7] ns |
| sel-10 | 10,000 | 83.0 [82.1, 87.2] ns | 86.9 [85.7, 88.8] ns | 48.3 [47.2, 54.2] ns | 69.6 [68.6, 72.7] ns | 81.2 [80.8, 83.1] ns | 79.1 [77.7, 84.8] ns |
| sel-100 | 10,000 | 206.8 [203.2, 211.9] ns | 207.3 [203.4, 209.9] ns | 203.1 [191.9, 232.5] ns | 195.0 [186.9, 222.6] ns | 210.5 [202.3, 215.3] ns | 210.5 [201.6, 214.1] ns |
| out-200 | 200 | 237.3 [229.4, 244.3] ns | 232.6 [229.6, 243.1] ns | 235.1 [228.4, 260.4] ns | 221.4 [214.5, 229.4] ns | 236.3 [218.5, 241.4] ns | not in that file |
| out-1k | 1,000 | 213.4 [206.6, 230.4] ns | 212.2 [203.2, 217.4] ns | 209.5 [202.7, 235.3] ns | 199.1 [193.1, 220.1] ns | 213.8 [210.0, 217.3] ns | not in that file |
| wide-h | 10,000 | 93.4 [91.8, 94.4] ns | 95.2 [92.7, 99.3] ns | 35.6 [34.9, 37.2] ns | 79.7 [77.0, 91.1] ns | 89.1 [88.5, 93.7] ns | 87.4 [85.1, 91.7] ns |
| wide-h-k | 10,000 | 105.0 [104.4, 108.5] ns | 109.7 [108.5, 113.8] ns | 36.0 [35.5, 37.0] ns | 91.0 [89.4, 103.5] ns | 101.4 [100.6, 102.6] ns | 98.0 [95.4, 102.3] ns |
| wide-a | 10,000 | 90.0 [88.6, 91.8] ns | 91.7 [91.2, 94.8] ns | 39.0 [38.5, 40.5] ns | 77.9 [75.6, 88.1] ns | 87.8 [86.0, 90.0] ns | 84.7 [83.1, 89.7] ns |
| wide-c | 10,000 | 99.9 [98.8, 103.9] ns | 100.3 [98.2, 105.6] ns | 36.7 [36.5, 41.7] ns | 84.9 [84.3, 90.5] ns | 95.4 [93.9, 97.4] ns | 89.4 [87.6, 92.9] ns |
| wide-d | 10,000 | 88.7 [88.2, 89.5] ns | 89.5 [87.4, 90.9] ns | 36.8 [36.2, 37.4] ns | 74.1 [72.5, 77.5] ns | 85.9 [85.3, 88.0] ns | 83.4 [81.9, 95.5] ns |
| wide-e | 10,000 | 90.0 [88.5, 95.6] ns | 89.6 [89.1, 91.6] ns | 38.5 [37.6, 39.9] ns | 74.9 [72.8, 75.8] ns | 87.0 [85.6, 88.0] ns | 82.4 [80.7, 94.8] ns |
| wide-f | 10,000 | 91.3 [90.5, 93.7] ns | 91.2 [90.4, 95.2] ns | 39.5 [38.7, 40.6] ns | 75.8 [75.0, 77.3] ns | 87.8 [85.8, 89.5] ns | 84.1 [82.7, 91.7] ns |
| wide-g | 10,000 | 92.6 [90.3, 95.1] ns | 93.3 [91.1, 98.2] ns | 39.3 [38.4, 41.3] ns | 76.7 [74.6, 77.9] ns | 87.6 [86.3, 89.9] ns | 84.2 [82.9, 90.1] ns |
| wide-k | 10,000 | 101.6 [99.5, 104.2] ns | 101.0 [97.9, 116.1] ns | 39.7 [39.1, 40.9] ns | 84.2 [82.7, 88.8] ns | 95.2 [93.6, 97.5] ns | 91.2 [90.0, 102.1] ns |
| wide-m | 10,000 | 109.8 [108.8, 115.3] ns | 108.7 [106.7, 114.2] ns | 44.3 [43.6, 47.4] ns | 90.8 [90.0, 92.7] ns | 110.4 [109.4, 112.7] ns | 101.7 [98.7, 107.8] ns |
| nul-0 | 10,000 | 70.2 [69.8, 72.1] ns | 74.4 [73.8, 76.0] ns | 32.0 [31.6, 34.0] ns | 58.1 [57.1, 60.3] ns | 69.0 [67.6, 71.6] ns | 67.1 [66.0, 70.4] ns |
| nul-50 | 10,000 | 69.1 [67.3, 73.4] ns | 70.0 [68.5, 71.1] ns | 43.9 [42.8, 44.9] ns | 54.4 [53.1, 56.8] ns | 66.1 [64.6, 67.5] ns | 64.8 [62.7, 69.1] ns |

**prepared path: net row, ns (median over 8 runs [min, max]); the net row is (scan CPU - point-select CPU on the same relation, arm, path and run) / rows scanned**

| cell | rows scanned | A | A' | B | B-off | A-al64 |
|---|---|---|---|---|---|---|
| rows-200 | 200 | 73.9 [69.9, 94.9] ns | 74.3 [56.1, 77.1] ns | 34.1 [24.8, 37.4] ns | 57.8 [53.1, 60.1] ns | 69.8 [64.0, 78.6] ns |
| rows-1k | 1,000 | 71.1 [69.8, 73.6] ns | 73.9 [70.5, 77.2] ns | 31.6 [30.0, 34.1] ns | 57.0 [54.5, 59.9] ns | 69.9 [66.6, 75.2] ns |
| bn | 10,000 | 69.2 [68.6, 71.9] ns | 72.9 [72.2, 76.0] ns | 31.2 [30.8, 32.5] ns | 56.1 [55.6, 57.0] ns | 68.0 [67.0, 69.6] ns |
| rows-100k | 100,000 | 68.4 [67.8, 69.6] ns | 72.0 [71.0, 80.7] ns | 30.2 [30.0, 32.3] ns | 55.2 [55.1, 56.0] ns | 66.6 [65.9, 68.1] ns |
| bn-recent | 10,000 | 70.4 [69.0, 73.8] ns | 74.1 [73.1, 75.5] ns | 31.3 [30.8, 33.8] ns | 57.2 [55.9, 60.1] ns | 68.9 [67.6, 70.6] ns |
| sel-0 | 10,000 | 70.0 [68.6, 76.6] ns | 73.8 [72.1, 75.1] ns | 31.5 [30.6, 33.7] ns | 56.8 [55.7, 59.8] ns | 67.6 [66.7, 71.3] ns |
| sel-1 | 10,000 | 72.3 [70.3, 76.3] ns | 75.8 [73.9, 78.4] ns | 33.2 [32.1, 36.2] ns | 57.8 [57.1, 60.2] ns | 69.1 [68.4, 69.4] ns |
| sel-10 | 10,000 | 82.9 [82.0, 86.9] ns | 86.1 [85.0, 87.8] ns | 48.7 [47.2, 53.5] ns | 69.8 [68.9, 73.4] ns | 81.1 [80.6, 82.8] ns |
| sel-100 | 10,000 | 206.7 [202.1, 212.5] ns | 205.1 [201.7, 211.3] ns | 202.7 [192.8, 234.4] ns | 194.5 [186.5, 222.0] ns | 210.4 [199.9, 217.2] ns |
| out-200 | 200 | 237.2 [223.7, 253.3] ns | 234.6 [211.3, 239.1] ns | 237.0 [217.5, 254.9] ns | 221.0 [214.4, 229.0] ns | 235.6 [225.6, 246.8] ns |
| out-1k | 1,000 | 211.9 [207.8, 216.9] ns | 211.4 [204.7, 214.3] ns | 207.3 [202.7, 232.1] ns | 198.3 [192.5, 220.1] ns | 215.1 [208.9, 217.4] ns |
| wide-h | 10,000 | 92.3 [90.9, 94.6] ns | 94.3 [92.6, 96.7] ns | 35.6 [34.7, 36.6] ns | 79.8 [77.3, 90.5] ns | 89.7 [88.8, 92.2] ns |
| wide-h-k | 10,000 | 106.2 [103.6, 106.8] ns | 110.2 [107.3, 114.1] ns | 36.4 [34.8, 37.3] ns | 90.3 [89.5, 102.3] ns | 101.8 [99.5, 106.6] ns |
| wide-a | 10,000 | 90.1 [88.3, 92.3] ns | 92.5 [90.3, 94.3] ns | 39.1 [38.1, 41.6] ns | 77.7 [75.7, 86.8] ns | 87.6 [86.3, 90.3] ns |
| wide-c | 10,000 | 99.5 [98.7, 106.8] ns | 100.4 [98.4, 105.1] ns | 37.3 [36.2, 38.4] ns | 85.0 [83.4, 89.8] ns | 94.5 [93.6, 96.1] ns |
| wide-d | 10,000 | 88.7 [88.1, 90.1] ns | 89.4 [87.9, 90.1] ns | 36.8 [36.0, 37.9] ns | 73.7 [72.7, 75.9] ns | 86.2 [84.6, 88.3] ns |
| wide-e | 10,000 | 89.9 [89.1, 96.4] ns | 90.0 [88.8, 93.2] ns | 38.4 [37.5, 39.6] ns | 74.7 [73.4, 75.6] ns | 86.5 [85.9, 91.2] ns |
| wide-f | 10,000 | 91.3 [90.9, 93.8] ns | 92.5 [89.7, 96.1] ns | 39.4 [38.7, 40.8] ns | 75.4 [74.7, 77.3] ns | 87.6 [86.1, 89.7] ns |
| wide-g | 10,000 | 93.3 [90.5, 96.0] ns | 93.0 [90.5, 98.9] ns | 39.0 [38.6, 42.4] ns | 76.6 [74.2, 78.0] ns | 88.8 [86.7, 94.4] ns |
| wide-k | 10,000 | 101.3 [99.2, 102.8] ns | 100.9 [96.9, 117.1] ns | 39.6 [39.2, 42.4] ns | 84.1 [82.6, 85.0] ns | 95.3 [93.3, 97.4] ns |
| wide-m | 10,000 | 109.7 [108.8, 115.5] ns | 108.7 [106.0, 113.8] ns | 44.2 [43.7, 48.2] ns | 90.6 [89.4, 92.7] ns | 110.8 [109.6, 113.6] ns |
| nul-0 | 10,000 | 70.3 [69.7, 73.8] ns | 74.4 [73.8, 75.5] ns | 32.1 [31.6, 34.1] ns | 58.1 [57.2, 58.4] ns | 68.6 [67.5, 71.5] ns |
| nul-50 | 10,000 | 70.1 [67.6, 73.3] ns | 70.0 [68.7, 74.3] ns | 44.0 [42.7, 46.2] ns | 54.8 [53.1, 55.6] ns | 66.2 [64.5, 67.7] ns |

Throughput form (rows per second, derived as 1e9 / the median net row of the table above):

**literal path, scan rate: rows per second derived from the median net row (1e9 / ns)**

| cell | rows scanned | A rows/s | A' rows/s | B rows/s | B-off rows/s | A-al64 rows/s | B / A | B-off / A |
|---|---|---|---|---|---|---|---|---|
| rows-200 | 200 | 13.21 M | 13.38 M | 27.55 M | 16.68 M | 14.26 M | 2.085 | 1.262 |
| rows-1k | 1,000 | 14.13 M | 13.17 M | 30.91 M | 17.39 M | 13.97 M | 2.188 | 1.231 |
| bn | 10,000 | 14.28 M | 13.65 M | 32.22 M | 17.73 M | 14.60 M | 2.256 | 1.241 |
| rows-100k | 100,000 | 14.42 M | 13.87 M | 33.15 M | 18.07 M | 14.97 M | 2.299 | 1.253 |
| bn-recent | 10,000 | 14.14 M | 13.42 M | 31.69 M | 17.50 M | 14.55 M | 2.241 | 1.237 |
| sel-0 | 10,000 | 14.22 M | 13.61 M | 31.75 M | 17.69 M | 14.75 M | 2.233 | 1.244 |
| sel-1 | 10,000 | 13.88 M | 13.37 M | 30.55 M | 17.10 M | 14.35 M | 2.201 | 1.232 |
| sel-10 | 10,000 | 12.05 M | 11.51 M | 20.69 M | 14.36 M | 12.31 M | 1.716 | 1.191 |
| sel-100 | 10,000 | 4.83 M | 4.82 M | 4.92 M | 5.13 M | 4.75 M | 1.018 | 1.061 |
| out-200 | 200 | 4.21 M | 4.30 M | 4.25 M | 4.52 M | 4.23 M | 1.010 | 1.072 |
| out-1k | 1,000 | 4.69 M | 4.71 M | 4.77 M | 5.02 M | 4.68 M | 1.018 | 1.072 |
| wide-h | 10,000 | 10.70 M | 10.50 M | 28.08 M | 12.55 M | 11.22 M | 2.624 | 1.173 |
| wide-h-k | 10,000 | 9.53 M | 9.11 M | 27.78 M | 10.99 M | 9.86 M | 2.916 | 1.154 |
| wide-a | 10,000 | 11.11 M | 10.91 M | 25.67 M | 12.84 M | 11.39 M | 2.311 | 1.155 |
| wide-c | 10,000 | 10.01 M | 9.97 M | 27.24 M | 11.77 M | 10.49 M | 2.720 | 1.176 |
| wide-d | 10,000 | 11.27 M | 11.17 M | 27.17 M | 13.49 M | 11.64 M | 2.410 | 1.197 |
| wide-e | 10,000 | 11.11 M | 11.16 M | 25.96 M | 13.35 M | 11.49 M | 2.336 | 1.202 |
| wide-f | 10,000 | 10.95 M | 10.97 M | 25.29 M | 13.20 M | 11.39 M | 2.309 | 1.206 |
| wide-g | 10,000 | 10.80 M | 10.72 M | 25.43 M | 13.04 M | 11.41 M | 2.354 | 1.207 |
| wide-k | 10,000 | 9.85 M | 9.90 M | 25.18 M | 11.88 M | 10.50 M | 2.558 | 1.206 |
| wide-m | 10,000 | 9.11 M | 9.20 M | 22.56 M | 11.02 M | 9.06 M | 2.477 | 1.209 |
| nul-0 | 10,000 | 14.24 M | 13.44 M | 31.24 M | 17.22 M | 14.49 M | 2.194 | 1.209 |
| nul-50 | 10,000 | 14.46 M | 14.28 M | 22.80 M | 18.38 M | 15.14 M | 1.577 | 1.271 |

**prepared path, scan rate: rows per second derived from the median net row (1e9 / ns)**

| cell | rows scanned | A rows/s | A' rows/s | B rows/s | B-off rows/s | A-al64 rows/s | B / A | B-off / A |
|---|---|---|---|---|---|---|---|---|
| rows-200 | 200 | 13.53 M | 13.47 M | 29.32 M | 17.31 M | 14.32 M | 2.166 | 1.279 |
| rows-1k | 1,000 | 14.06 M | 13.53 M | 31.67 M | 17.55 M | 14.30 M | 2.253 | 1.248 |
| bn | 10,000 | 14.44 M | 13.71 M | 32.03 M | 17.82 M | 14.71 M | 2.218 | 1.234 |
| rows-100k | 100,000 | 14.61 M | 13.89 M | 33.08 M | 18.13 M | 15.00 M | 2.264 | 1.241 |
| bn-recent | 10,000 | 14.21 M | 13.49 M | 32.00 M | 17.48 M | 14.51 M | 2.252 | 1.230 |
| sel-0 | 10,000 | 14.29 M | 13.54 M | 31.78 M | 17.59 M | 14.79 M | 2.223 | 1.231 |
| sel-1 | 10,000 | 13.83 M | 13.19 M | 30.16 M | 17.29 M | 14.47 M | 2.181 | 1.250 |
| sel-10 | 10,000 | 12.06 M | 11.62 M | 20.52 M | 14.32 M | 12.33 M | 1.701 | 1.187 |
| sel-100 | 10,000 | 4.84 M | 4.88 M | 4.93 M | 5.14 M | 4.75 M | 1.020 | 1.063 |
| out-200 | 200 | 4.22 M | 4.26 M | 4.22 M | 4.52 M | 4.24 M | 1.001 | 1.073 |
| out-1k | 1,000 | 4.72 M | 4.73 M | 4.82 M | 5.04 M | 4.65 M | 1.022 | 1.068 |
| wide-h | 10,000 | 10.83 M | 10.61 M | 28.05 M | 12.53 M | 11.14 M | 2.589 | 1.157 |
| wide-h-k | 10,000 | 9.42 M | 9.08 M | 27.46 M | 11.07 M | 9.83 M | 2.915 | 1.175 |
| wide-a | 10,000 | 11.09 M | 10.82 M | 25.54 M | 12.87 M | 11.42 M | 2.303 | 1.160 |
| wide-c | 10,000 | 10.05 M | 9.96 M | 26.82 M | 11.76 M | 10.58 M | 2.670 | 1.170 |
| wide-d | 10,000 | 11.28 M | 11.19 M | 27.14 M | 13.57 M | 11.60 M | 2.406 | 1.203 |
| wide-e | 10,000 | 11.13 M | 11.11 M | 26.01 M | 13.38 M | 11.56 M | 2.337 | 1.203 |
| wide-f | 10,000 | 10.95 M | 10.81 M | 25.36 M | 13.27 M | 11.41 M | 2.317 | 1.212 |
| wide-g | 10,000 | 10.71 M | 10.76 M | 25.66 M | 13.06 M | 11.27 M | 2.395 | 1.219 |
| wide-k | 10,000 | 9.87 M | 9.91 M | 25.26 M | 11.90 M | 10.49 M | 2.559 | 1.205 |
| wide-m | 10,000 | 9.12 M | 9.20 M | 22.62 M | 11.04 M | 9.03 M | 2.480 | 1.211 |
| nul-0 | 10,000 | 14.23 M | 13.44 M | 31.18 M | 17.22 M | 14.57 M | 2.191 | 1.210 |
| nul-50 | 10,000 | 14.27 M | 14.28 M | 22.72 M | 18.24 M | 15.11 M | 1.592 | 1.279 |

**Paired deltas, and what each stage bought.** `B-off - A` is S2 (the VM-owned slot loop and the once-per-statement checks)
with the kernel off; `B - B-off` is S3 and S4 (the kernel) on the same binary. The two are additive by construction (same
binary, one switch), and `B - A` also carries `f5a7efb2`'s instrument and the shared-code changes.

**literal path: paired deltas of the net row, ns (median over 8 runs [min, max]; per-run pairs); S is the largest |A - A'| in any run (BM-Q12's spread); only `sel-100` is a bar here**

| cell | B - A | B-off - A (S2 alone) | B - B-off (S3, S4) | A - A' (S) | B against S | B > A in k of 8 runs | A-al64 - A (layout probe) |
|---|---|---|---|---|---|---|---|
| rows-200 | -41.42 [-50.52, -31.78] (-54.7 %) | -15.93 [-23.29, -10.49] (-21.0 %) | -23.77 [-33.31, -20.68] (-31.4 %) | -1.34 [-13.29, +13.11] (S = 13.29) | faster than the spread | 0 | -4.74 [-16.80, +8.79] (-6.3 %) |
| rows-1k | -38.65 [-41.71, -37.55] (-54.6 %) | -13.21 [-17.58, -4.20] (-18.7 %) | -25.72 [-34.66, -22.50] (-36.3 %) | -4.56 [-8.25, +3.49] (S = 8.25) | faster than the spread | 0 | -1.24 [-3.52, +4.63] (-1.8 %) |
| bn | -38.08 [-40.74, -36.95] (-54.4 %) | -13.37 [-15.45, -10.59] (-19.1 %) | -25.24 [-27.11, -24.32] (-36.0 %) | -3.17 [-6.38, -1.50] (S = 6.38) | faster than the spread | 0 | -1.92 [-4.81, -0.25] (-2.7 %) |
| rows-100k | -38.49 [-39.70, -37.95] (-55.5 %) | -13.65 [-14.93, -11.66] (-19.7 %) | -25.05 [-26.48, -23.47] (-36.1 %) | -2.95 [-11.02, -2.05] (S = 11.02) | faster than the spread | 0 | -1.96 [-3.17, -1.15] (-2.8 %) |
| bn-recent | -38.49 [-41.68, -36.35] (-54.4 %) | -13.68 [-16.36, -9.89] (-19.4 %) | -25.24 [-28.23, -23.21] (-35.7 %) | -4.50 [-5.48, -1.18] (S = 5.48) | faster than the spread | 0 | -1.53 [-5.24, -0.58] (-2.2 %) |
| sel-0 | -39.10 [-44.30, -35.55] (-55.6 %) | -13.88 [-18.10, -10.97] (-19.7 %) | -25.23 [-26.20, -24.33] (-35.9 %) | -2.70 [-5.85, +1.55] (S = 5.85) | faster than the spread | 0 | -2.43 [-7.20, +2.27] (-3.5 %) |
| sel-1 | -38.90 [-42.99, -36.43] (-54.0 %) | -14.07 [-15.38, -8.85] (-19.5 %) | -25.12 [-27.61, -23.20] (-34.9 %) | -3.19 [-5.75, +1.40] (S = 5.75) | faster than the spread | 0 | -2.45 [-6.69, +0.03] (-3.4 %) |
| sel-10 | -34.59 [-39.63, -28.69] (-41.7 %) | -13.07 [-17.97, -11.21] (-15.8 %) | -21.85 [-23.37, -15.17] (-26.3 %) | -3.94 [-5.95, +1.23] (S = 5.95) | faster than the spread | 0 | -1.34 [-6.39, -0.36] (-1.6 %) |
| sel-100 | -4.19 [-16.79, +28.80] (-2.0 %) | -11.86 [-22.99, +10.68] (-5.7 %) | +6.05 [-18.11, +42.82] (+2.9 %) | +0.34 [-6.25, +4.86] (S = 6.25) | within the spread (B - A below minus the typical gap 2.2) | 2 | +1.25 [-6.22, +11.63] (+0.6 %) |
| out-200 | -0.13 [-14.64, +30.18] (-0.1 %) | -15.36 [-23.84, -4.39] (-6.5 %) | +15.23 [+5.34, +34.57] (+6.4 %) | +1.85 [-3.73, +12.41] (S = 12.41) | within the spread (inside the typical gap 3.55) | 4 | +0.98 [-22.55, +8.48] (+0.4 %) |
| out-1k | -4.28 [-14.43, +17.76] (-2.0 %) | -13.15 [-33.22, +3.82] (-6.2 %) | +10.55 [-10.94, +35.59] (+4.9 %) | +2.93 [-5.25, +12.96] (S = 12.96) | within the spread (B - A below minus the typical gap 3.99) | 3 | +0.24 [-14.98, +4.79] (+0.1 %) |
| wide-h | -57.70 [-59.44, -55.07] (-61.7 %) | -14.04 [-17.37, -1.66] (-15.0 %) | -44.18 [-55.27, -41.24] (-47.3 %) | -1.68 [-7.09, +1.44] (S = 7.09) | faster than the spread | 0 | -3.05 [-5.34, +0.10] (-3.3 %) |
| wide-h-k | -68.95 [-72.73, -67.45] (-65.7 %) | -14.47 [-17.20, -1.29] (-13.8 %) | -54.56 [-67.27, -53.70] (-52.0 %) | -4.71 [-6.08, -3.58] (S = 6.08) | faster than the spread | 0 | -3.72 [-5.92, -2.89] (-3.5 %) |
| wide-a | -50.92 [-51.90, -49.77] (-56.6 %) | -12.39 [-14.52, -1.49] (-13.8 %) | -38.26 [-49.67, -36.17] (-42.5 %) | -1.72 [-4.37, -0.15] (S = 4.37) | faster than the spread | 0 | -1.76 [-3.61, -0.95] (-2.0 %) |
| wide-c | -62.87 [-64.17, -60.73] (-63.0 %) | -14.85 [-16.46, -9.90] (-14.9 %) | -47.73 [-54.05, -45.93] (-47.8 %) | -1.16 [-1.94, +2.91] (S = 2.91) | faster than the spread | 0 | -4.65 [-7.43, -2.37] (-4.7 %) |
| wide-d | -52.32 [-53.27, -50.92] (-59.0 %) | -14.54 [-16.05, -12.04] (-16.4 %) | -37.32 [-40.32, -35.10] (-42.1 %) | -0.56 [-2.14, +0.82] (S = 2.14) | faster than the spread | 0 | -2.74 [-3.81, -1.22] (-3.1 %) |
| wide-e | -51.49 [-55.69, -50.04] (-57.2 %) | -15.68 [-20.51, -14.64] (-17.4 %) | -35.84 [-37.70, -34.63] (-39.8 %) | +0.49 [-1.00, +4.00] (S = 4.00) | faster than the spread | 0 | -3.55 [-7.95, -1.83] (-4.0 %) |
| wide-f | -51.77 [-54.27, -50.88] (-56.7 %) | -15.73 [-16.34, -14.53] (-17.2 %) | -36.05 [-37.93, -35.14] (-39.5 %) | +0.14 [-3.27, +2.08] (S = 3.27) | faster than the spread | 0 | -3.65 [-6.50, -1.49] (-4.0 %) |
| wide-g | -52.24 [-54.82, -51.05] (-56.4 %) | -15.98 [-18.32, -13.64] (-17.3 %) | -37.14 [-38.71, -34.79] (-40.1 %) | -0.96 [-5.50, +2.10] (S = 5.50) | faster than the spread | 0 | -4.14 [-6.97, -2.82] (-4.5 %) |
| wide-k | -61.65 [-63.32, -59.38] (-60.7 %) | -16.69 [-19.29, -11.95] (-16.4 %) | -44.38 [-49.21, -42.81] (-43.7 %) | -0.06 [-11.90, +3.21] (S = 11.90) | faster than the spread | 0 | -5.19 [-8.86, -4.26] (-5.1 %) |
| wide-m | -65.28 [-69.89, -62.33] (-59.5 %) | -18.64 [-23.04, -17.14] (-17.0 %) | -46.60 [-48.14, -45.19] (-42.4 %) | +1.27 [-4.57, +6.71] (S = 6.71) | faster than the spread | 0 | +0.62 [-2.86, +2.81] (+0.6 %) |
| nul-0 | -38.14 [-39.61, -37.80] (-54.3 %) | -12.19 [-14.73, -9.68] (-17.4 %) | -26.47 [-28.25, -23.14] (-37.7 %) | -3.83 [-5.84, -2.09] (S = 5.84) | faster than the spread | 0 | -1.63 [-2.52, +1.39] (-2.3 %) |
| nul-50 | -25.42 [-28.89, -23.77] (-36.8 %) | -14.89 [-18.66, -12.75] (-21.5 %) | -10.40 [-12.71, -9.52] (-15.0 %) | -0.85 [-2.36, +2.77] (S = 2.77) | faster than the spread | 0 | -3.19 [-7.17, -2.33] (-4.6 %) |

**prepared path: paired deltas of the net row, ns (median over 8 runs [min, max]; per-run pairs); S is the largest |A - A'| in any run (BM-Q12's spread); only `sel-100` is a bar here**

| cell | B - A | B-off - A (S2 alone) | B - B-off (S3, S4) | A - A' (S) | B against S | B > A in k of 8 runs | A-al64 - A (layout probe) |
|---|---|---|---|---|---|---|---|
| rows-200 | -41.98 [-59.16, -34.97] (-56.8 %) | -16.72 [-36.03, -10.68] (-22.6 %) | -23.86 [-28.28, -21.67] (-32.3 %) | -1.45 [-5.18, +25.01] (S = 25.01) | faster than the spread | 0 | -3.45 [-16.23, +2.63] (-4.7 %) |
| rows-1k | -39.79 [-42.89, -36.35] (-55.9 %) | -14.63 [-16.55, -10.29] (-20.6 %) | -25.92 [-26.95, -22.81] (-36.4 %) | -2.81 [-6.64, +0.91] (S = 6.64) | faster than the spread | 0 | -1.31 [-5.08, +4.73] (-1.8 %) |
| bn | -37.87 [-40.06, -36.05] (-54.7 %) | -12.95 [-15.95, -11.99] (-18.7 %) | -24.95 [-25.82, -23.10] (-36.0 %) | -3.41 [-6.42, -2.62] (S = 6.42) | faster than the spread | 0 | -1.32 [-3.98, +0.06] (-1.9 %) |
| rows-100k | -38.02 [-39.63, -36.61] (-55.6 %) | -13.30 [-14.51, -11.96] (-19.4 %) | -24.88 [-26.01, -22.81] (-36.4 %) | -3.36 [-12.93, -1.89] (S = 12.93) | faster than the spread | 0 | -1.97 [-2.92, -0.70] (-2.9 %) |
| bn-recent | -38.50 [-42.97, -36.83] (-54.7 %) | -13.45 [-14.98, -9.93] (-19.1 %) | -25.45 [-29.32, -23.55] (-36.2 %) | -3.16 [-5.67, +0.66] (S = 5.67) | faster than the spread | 0 | -1.32 [-6.22, +0.66] (-1.9 %) |
| sel-0 | -38.55 [-45.35, -35.97] (-55.1 %) | -12.92 [-19.16, -10.92] (-18.5 %) | -25.18 [-26.73, -24.90] (-36.0 %) | -3.29 [-5.44, +2.68] (S = 5.44) | faster than the spread | 0 | -2.07 [-8.19, +0.15] (-3.0 %) |
| sel-1 | -39.48 [-42.26, -36.60] (-54.6 %) | -14.58 [-19.12, -12.37] (-20.2 %) | -24.91 [-25.34, -20.90] (-34.4 %) | -2.88 [-5.21, +1.12] (S = 5.21) | faster than the spread | 0 | -3.60 [-7.34, -1.09] (-5.0 %) |
| sel-10 | -34.71 [-39.03, -28.66] (-41.9 %) | -13.20 [-16.98, -9.30] (-15.9 %) | -21.45 [-24.73, -19.36] (-25.9 %) | -2.80 [-5.81, +1.15] (S = 5.81) | faster than the spread | 0 | -1.59 [-4.12, -0.15] (-1.9 %) |
| sel-100 | -5.36 [-15.90, +30.33] (-2.6 %) | -10.62 [-21.82, +9.45] (-5.1 %) | +6.25 [-20.52, +40.13] (+3.0 %) | +1.34 [-7.23, +7.77] (S = 7.77) | within the spread (B - A below minus the typical gap 3.21) | 2 | +1.19 [-9.06, +13.11] (+0.6 %) |
| out-200 | -5.13 [-11.44, +20.71] (-2.2 %) | -11.72 [-33.58, -8.73] (-4.9 %) | +10.32 [-1.21, +38.40] (+4.4 %) | +2.47 [-7.16, +17.07] (S = 17.07) | within the spread (B - A below minus the typical gap 3.82) | 3 | -2.04 [-19.80, +15.03] (-0.9 %) |
| out-1k | -4.03 [-8.66, +15.80] (-1.9 %) | -13.41 [-19.30, +4.28] (-6.3 %) | +9.60 [-12.95, +31.15] (+4.5 %) | +1.63 [-5.17, +5.97] (S = 5.97) | within the spread (B - A below minus the typical gap 3.89) | 1 | +1.42 [-3.51, +8.04] (+0.7 %) |
| wide-h | -56.43 [-59.78, -54.31] (-61.1 %) | -13.39 [-14.57, -2.79] (-14.5 %) | -43.54 [-54.53, -42.05] (-47.2 %) | -1.88 [-5.82, +1.83] (S = 5.82) | faster than the spread | 0 | -2.80 [-4.86, +1.31] (-3.0 %) |
| wide-h-k | -69.46 [-71.41, -67.34] (-65.4 %) | -14.81 [-16.72, -4.04] (-14.0 %) | -54.47 [-65.31, -53.50] (-51.3 %) | -4.89 [-7.80, -0.98] (S = 7.80) | faster than the spread | 0 | -3.92 [-6.71, +0.39] (-3.7 %) |
| wide-a | -51.05 [-51.96, -49.47] (-56.6 %) | -12.72 [-14.57, -3.04] (-14.1 %) | -38.06 [-47.73, -36.84] (-42.2 %) | -2.22 [-3.64, -0.48] (S = 3.64) | faster than the spread | 0 | -2.74 [-3.59, +0.25] (-3.0 %) |
| wide-c | -63.16 [-69.18, -60.77] (-63.4 %) | -15.14 [-19.05, -11.60] (-15.2 %) | -47.75 [-52.16, -46.68] (-48.0 %) | -1.00 [-1.37, +2.39] (S = 2.39) | faster than the spread | 0 | -5.38 [-10.82, -3.74] (-5.4 %) |
| wide-d | -52.35 [-52.97, -50.16] (-59.0 %) | -15.16 [-15.93, -14.19] (-17.1 %) | -36.84 [-38.78, -35.28] (-41.5 %) | -0.15 [-1.34, +1.52] (S = 1.52) | faster than the spread | 0 | -2.74 [-4.00, -1.57] (-3.1 %) |
| wide-e | -51.86 [-57.81, -50.60] (-57.7 %) | -15.70 [-21.18, -14.40] (-17.5 %) | -36.19 [-36.74, -34.94] (-40.3 %) | -0.01 [-3.38, +6.52] (S = 6.52) | faster than the spread | 0 | -3.60 [-9.85, +1.43] (-4.0 %) |
| wide-f | -52.08 [-54.37, -50.95] (-57.0 %) | -16.10 [-16.49, -14.92] (-17.6 %) | -36.04 [-37.92, -34.53] (-39.5 %) | -0.58 [-4.36, +1.62] (S = 4.36) | faster than the spread | 0 | -3.67 [-6.09, -2.36] (-4.0 %) |
| wide-g | -53.26 [-57.17, -49.79] (-57.1 %) | -16.89 [-18.66, -14.32] (-18.1 %) | -36.42 [-39.15, -35.47] (-39.0 %) | -1.18 [-6.41, +3.97] (S = 6.41) | faster than the spread | 0 | -3.33 [-7.16, +2.13] (-3.6 %) |
| wide-k | -60.94 [-63.64, -59.03] (-60.2 %) | -17.21 [-19.42, -15.13] (-17.0 %) | -43.53 [-44.91, -42.54] (-43.0 %) | +0.69 [-15.17, +4.15] (S = 15.17) | faster than the spread | 0 | -6.03 [-6.78, -3.74] (-6.0 %) |
| wide-m | -65.62 [-69.44, -60.89] (-59.8 %) | -18.70 [-22.86, -17.46] (-17.1 %) | -46.47 [-48.11, -42.18] (-42.4 %) | +0.86 [-4.10, +6.90] (S = 6.90) | faster than the spread | 0 | +0.87 [-2.64, +4.49] (+0.8 %) |
| nul-0 | -38.20 [-41.50, -37.25] (-54.4 %) | -12.65 [-15.75, -11.32] (-18.0 %) | -25.84 [-26.59, -23.25] (-36.8 %) | -4.09 [-4.64, -0.70] (S = 4.64) | faster than the spread | 0 | -1.84 [-3.83, +1.15] (-2.6 %) |
| nul-50 | -25.69 [-28.70, -24.04] (-36.6 %) | -14.93 [-18.09, -14.28] (-21.3 %) | -10.54 [-11.73, -8.78] (-15.0 %) | -1.17 [-4.05, +2.90] (S = 4.05) | faster than the spread | 0 | -3.37 [-7.04, -2.24] (-4.8 %) |

- **S2 buys 12-19 ns a row on every cell (14-22 % where the row is cheap, 6-7 % on the emission-bound cells) and the kernel 10-55 ns
  more on the selective ones.** On `bn` they are 13.4 and 25.2 ns; the kernel is worth more where the row is wider or the filter
  column dearer to decode (`wide-h-k` 54.6 ns, `wide-m` 46.6, `wide-k` 44.4) and least on `nul-50` (10.4).
- **Width stopped mattering.** A's 13-column row cost 18.7-39.8 ns more than the 3-column `bn` row; B's costs 4.6-13.3 ns more
  (`wide-h` 35.6, `wide-m` 44.3 against `bn` 31.0), because a rejected row is neither copied nor decoded. A second conjunct,
  which A paid 11.6 ns for (`wide-h-k` over `wide-h`: it decoded `k` before it tried `h`), is free (+0.4 ns).
- **Selectivity.** B keeps its advantage at 0 % and 1 % rows out (31.5 and 32.7 ns against A's 70.3 and 72.0), loses most of
  it at 10 % (48.3 against 83.0: 1,000 emitted rows at about 168 ns each) and none of it is left at 100 %, where the cell is
  the result path.
- **A NULL operand is the one place the kernel gives back.** BM-Q3 (a) passes a row whose operand is NULL to the residual. `nul-0`
  (no NULL) costs 32.0 ns; `nul-50` (every second row NULL) 43.9 ns, **+11.9 ns a row, 23.8 ns per NULL row**, while A gets
  cheaper with NULLs (69.1 against 70.2). B still wins the cell (-36.8 %), but with half its rows NULL the kernel's gain is
  two fifths of what it is elsewhere. BM-Q3 said (b) would be taken only if this cost proved material; at 50 % NULL it is 12 ns of 44.
- **`bn-recent`** (5 % of the rows above the visibility floor, 0.0500 window lookups per scanned row in both B and B-off; A has no
  counter, Appendix B) costs +0.6 ns over `bn` in B (31.6 against 31.0) and +0.7 in A: a window lookup is lost in the noise at
  5 % of rows.
- **Against this engine's last file** (`results-bm-s1-scan-row-v2.7.0-942-gf5a7efb2.md`, the BM-S1 column above): A today reads
  70.0 ns on `bn` against that file's 65.5 (+4.5 ns, +6.9 %; the same code plus an instrument, another host day, another
  process mix). B is 34.5 ns under the BM-S1 number. The BM-S1 column is context, the A column is the comparator.

## 3. BM-Q12's guard: three cells, three readings

### 3.1 The 100 % output scan: within the spread, and the kernel's all-pass cost is priced

**B - A = -41 µs of 2,095 µs (-2.0 %) at 10,000 rows, spread 63 µs: within.** B is slower than A in 2 of 8 runs; the cell is
emission-bound (145 of its ~205 ns a row are the result path in BM-S1's reading) and its run-to-run range is wide (B alone spans
1,947-2,353 µs), so the guard resolves differences of about 3 % here and no finer. The same shape at 200 and 1,000 rows, which
the sweep rule asks for, is within too (+0.6 µs of 74.5, spread 1.6 µs; -3.6 µs of 241.7, spread 12.3 µs).

**Guard: the 100 % output scan (`sel-100`, and the same shape at 200 and 1,000 rows), literal path. Metric: the statement's server CPU; p50 wall beside it**


| cell | A server CPU | A' server CPU | B server CPU | B-off server CPU | A-al64 server CPU | A statements/s (CPU-derived) | B statements/s (CPU-derived) | B - A | B-off - A | A - A' (S = max abs) | B against S |
|---|---|---|---|---|---|---|---|---|---|---|---|
| out-200 | 74.5 [73.2, 75.8] µs | 74.3 [72.3, 76.2] µs | 74.9 [73.3, 78.7] µs | 71.9 [70.7, 73.8] µs | 74.4 [73.1, 76.3] µs | 13,422 /s | 13,359 /s | +0.6 [-1.9, +5.5] µs (+0.8 %) | -2.1 [-3.8, -0.9] µs (-2.8 %) | +0.5 [-1.6, +1.4] µs (S = 1.6 µs) | within the spread (inside the typical gap 0.744) |
| out-1k | 241.7 [235.4, 258.9] µs | 241.4 [234.8, 246.5] µs | 238.5 [232.6, 265.4] µs | 228.3 [223.2, 249.2] µs | 242.5 [240.0, 246.0] µs | 4,137 /s | 4,193 /s | -3.6 [-13.0, +19.0] µs (-1.5 %) | -12.3 [-31.6, +3.9] µs (-5.1 %) | +2.9 [-5.0, +12.3] µs (S = 12.3 µs) | within the spread (B - A below minus the typical gap 3.43) |
| sel-100 | 2095.3 [2062.4, 2146.4] µs | 2100.2 [2061.5, 2127.1] µs | 2059.1 [1946.6, 2352.8] µs | 1977.6 [1896.9, 2253.0] µs | 2132.5 [2050.6, 2180.4] µs | 477 /s | 486 /s | -41.0 [-167.0, +288.5] µs (-2.0 %) | -117.7 [-229.8, +106.6] µs (-5.6 %) | +3.5 [-62.8, +48.0] µs (S = 62.8 µs) | within the spread (B - A below minus the typical gap 21.6) |

wall p50 per statement (median over runs of each run's p50), statements/s derived:

| cell | A wall p50 | A' wall p50 | B wall p50 | B-off wall p50 | A-al64 wall p50 |
|---|---|---|---|---|---|
| out-200 | 365.7 µs (2,735 /s) | 364.9 µs (2,740 /s) | 365.8 µs (2,734 /s) | 364.0 µs (2,747 /s) | 366.0 µs (2,733 /s) |
| out-1k | 1615.9 µs (619 /s) | 1612.5 µs (620 /s) | 1608.7 µs (622 /s) | 1602.1 µs (624 /s) | 1615.6 µs (619 /s) |
| sel-100 | 15832.6 µs (63 /s) | 15836.7 µs (63 /s) | 15821.3 µs (63 /s) | 15708.1 µs (64 /s) | 15917.0 µs (63 /s) |

- `out-200`: B slower than A in 5 of 8 runs
- `out-1k`: B slower than A in 3 of 8 runs
- `sel-100`: B slower than A in 2 of 8 runs

**Guard: the 100 % output scan (`sel-100`, and the same shape at 200 and 1,000 rows), prepared path. Metric: the statement's server CPU; p50 wall beside it**


| cell | A server CPU | A' server CPU | B server CPU | B-off server CPU | A-al64 server CPU | A statements/s (CPU-derived) | B statements/s (CPU-derived) | B - A | B-off - A | A - A' (S = max abs) | B against S |
|---|---|---|---|---|---|---|---|---|---|---|---|
| out-200 | 73.7 [71.7, 76.9] µs | 73.3 [71.8, 74.8] µs | 74.5 [71.2, 78.5] µs | 71.9 [70.1, 72.7] µs | 73.5 [72.5, 75.8] µs | 13,573 /s | 13,417 /s | +0.1 [-2.7, +5.6] µs (+0.2 %) | -1.7 [-6.6, -0.6] µs (-2.3 %) | +0.2 [-1.4, +3.9] µs (S = 3.9 µs) | within the spread (inside the typical gap 1.03) |
| out-1k | 239.8 [235.9, 244.6] µs | 240.0 [235.0, 241.6] µs | 235.6 [231.9, 260.9] µs | 227.0 [223.2, 248.6] µs | 242.9 [237.9, 246.4] µs | 4,171 /s | 4,245 /s | -3.7 [-8.9, +16.5] µs (-1.5 %) | -13.1 [-17.7, +4.5] µs (-5.5 %) | +1.9 [-5.0, +5.7] µs (S = 5.7 µs) | within the spread (B - A below minus the typical gap 2.8) |
| sel-100 | 2093.7 [2050.3, 2151.4] µs | 2077.5 [2044.3, 2141.1] µs | 2054.4 [1954.5, 2371.7] µs | 1972.5 [1891.7, 2246.1] µs | 2130.9 [2026.2, 2198.3] µs | 478 /s | 487 /s | -52.6 [-158.2, +303.2] µs (-2.5 %) | -105.9 [-218.1, +94.7] µs (-5.1 %) | +13.1 [-72.6, +77.0] µs (S = 77.0 µs) | within the spread (B - A below minus the typical gap 31.3) |

wall p50 per statement (median over runs of each run's p50), statements/s derived:

| cell | A wall p50 | A' wall p50 | B wall p50 | B-off wall p50 | A-al64 wall p50 |
|---|---|---|---|---|---|
| out-200 | 361.0 µs (2,770 /s) | 360.6 µs (2,773 /s) | 362.6 µs (2,758 /s) | 358.9 µs (2,787 /s) | 361.1 µs (2,769 /s) |
| out-1k | 1604.1 µs (623 /s) | 1603.6 µs (624 /s) | 1601.2 µs (625 /s) | 1593.9 µs (627 /s) | 1606.2 µs (623 /s) |
| sel-100 | 15824.2 µs (63 /s) | 15818.9 µs (63 /s) | 15794.8 µs (63 /s) | 15653.2 µs (64 /s) | 15836.2 µs (63 /s) |

- `out-200`: B slower than A in 4 of 8 runs
- `out-1k`: B slower than A in 2 of 8 runs
- `sel-100`: B slower than A in 2 of 8 runs

**What the kernel costs a row it keeps.** `B - B-off` on the all-pass cells (net row, paired by run): +6.1 ns at 10,000 rows
([-18.1, +42.8]; +2.9 %), +10.6 ns at 1,000 ([-10.9, +35.6]; +4.9 %) and **+15.2 ns at 200 rows ([+5.3, +34.6], positive in
all 8 runs; +6.4 %)**. That is BM-Q2 (a)'s all-pass cost: the kernel's compare, then the whole residual path again. S2's saving
(-5 to -7 %) covers it, so B ends level with A on these cells, and **the 100 % scan gains nothing from the kernel**. BM-Q2 said
(b), the kernel as the authority, would be reconsidered if this cost proved material; it is 3-6 % of a cell the kernel cannot
shorten.

### 3.2 The point `SELECT`: within the spread by the bar, with a consistent small cost

`select-pk` on `bn_census`'s 1,000-row table, 8 runs x 3,000 statements per path (server CPU per statement; statements/s
CPU-derived; paired by run):

**`tools/bn_census.py`, `--rows 1000`: server CPU per statement (µs, median over 8 runs [min, max]); statements/s is CPU-derived (1e6 / CPU µs); deltas paired by run**

| shape | path | A | A' | B | B-off | A statements/s | B statements/s | B - A | B-off - A | A - A' (S = max abs) | B against S | A wall p50 | B wall p50 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| select-pk | literal | 28.19 [26.64, 30.36] µs | 28.47 [26.82, 30.36] µs | 28.86 [27.05, 30.65] µs | 28.64 [26.88, 31.09] µs | 35,477 /s | 34,647 /s | +0.46 [-0.55, +1.47] µs (+1.6 %) | +0.28 [-0.05, +1.27] µs (+1.0 %) | -0.15 [-0.89, +0.55] µs (S = 0.89 µs) | within the spread (B - A above the typical gap 0.462) | 56.8 µs | 57.3 µs |
| select-pk | prepared | 27.48 [26.26, 29.65] µs | 27.77 [25.94, 30.56] µs | 28.44 [25.96, 30.37] µs | 27.91 [26.45, 30.24] µs | 36,385 /s | 35,161 /s | +0.62 [-0.30, +2.19] µs (+2.3 %) | +0.57 [-0.16, +1.37] µs (+2.1 %) | -0.27 [-1.46, +0.36] µs (S = 1.46 µs) | within the spread (B - A above the typical gap 0.34) | 53.5 µs | 54.2 µs |
| update-lit | literal | 24.80 [24.59, 25.35] µs | 25.14 [24.67, 25.94] µs | 25.11 [24.64, 25.60] µs | 25.04 [24.66, 25.44] µs | 40,328 /s | 39,822 /s | +0.18 [-0.40, +0.93] µs (+0.7 %) | +0.14 [-0.25, +0.60] µs (+0.6 %) | -0.15 [-0.79, +0.24] µs (S = 0.79 µs) | within the spread (inside the typical gap 0.228) | 45.6 µs | 45.8 µs |
| update-lit | prepared | 24.05 [23.81, 24.43] µs | 24.04 [23.88, 24.95] µs | 24.15 [23.57, 25.13] µs | 24.39 [23.65, 24.70] µs | 41,574 /s | 41,410 /s | +0.10 [-0.46, +0.95] µs (+0.4 %) | +0.22 [-0.16, +0.65] µs (+0.9 %) | -0.14 [-0.61, +0.55] µs (S = 0.61 µs) | within the spread (inside the typical gap 0.34) | 42.7 µs | 42.7 µs |
| update-range | literal | 734.37 [720.43, 755.76] µs | 728.10 [716.13, 762.08] µs | 766.13 [755.11, 792.49] µs | 766.24 [755.60, 800.23] µs | 1,362 /s | 1,305 /s | +34.49 [+11.84, +44.23] µs (+4.7 %) | +35.43 [+3.67, +71.11] µs (+4.8 %) | +4.70 [-38.37, +25.32] µs (S = 38.37 µs) | within the spread (B - A above the typical gap 9.67) | 742.1 µs | 779.9 µs |
| update-range | prepared | 739.16 [724.52, 786.51] µs | 724.49 [719.47, 760.17] µs | 779.35 [754.49, 799.81] µs | 763.76 [754.83, 797.47] µs | 1,353 /s | 1,283 /s | +34.99 [+0.72, +48.50] µs (+4.7 %) | +32.80 [+2.74, +49.23] µs (+4.4 %) | +5.62 [-15.27, +60.03] µs (S = 60.03 µs) | within the spread (B - A above the typical gap 11.8) | 737.9 µs | 772.7 µs |
| scan | literal | 98.80 [97.46, 101.59] µs | 101.80 [100.86, 104.38] µs | 59.31 [57.91, 60.98] µs | 84.81 [84.01, 87.63] µs | 10,121 /s | 16,859 /s | -39.43 [-40.97, -38.40] µs (-39.9 %) | -13.68 [-14.39, -12.83] µs (-13.8 %) | -3.39 [-6.23, +0.72] µs (S = 6.23 µs) | faster than the spread | 127.0 µs | 87.1 µs |
| scan | prepared | 98.01 [96.65, 101.24] µs | 101.19 [99.93, 103.01] µs | 58.24 [57.31, 59.88] µs | 84.47 [83.61, 87.36] µs | 10,203 /s | 17,171 /s | -39.61 [-41.91, -38.60] µs (-40.4 %) | -13.49 [-14.08, -12.33] µs (-13.8 %) | -3.19 [-4.25, +0.12] µs (S = 4.25 µs) | faster than the spread | 125.2 µs | 84.3 µs |

B slower than A in k of 8 runs: select-pk/literal 6, select-pk/prepared 6, update-lit/literal 6, update-lit/prepared 5, update-range/literal 8, update-range/prepared 8, scan/literal 0, scan/prepared 0
end state and page growth: {"end_state": {"A": {"range_v_ok": true, "spill_txt_ok": true}, "A2": {"range_v_ok": true, "spill_txt_ok": true}, "B": {"range_v_ok": true, "spill_txt_ok": true}, "Boff": {"range_v_ok": true, "spill_txt_ok": true}}}

**B - A = +0.46 µs of 28.2 (+1.6 %) literal and +0.62 µs of 27.5 (+2.3 %) prepared; spread 0.89 and 1.46 µs: within.**
The sign is not noise, though: B is slower than A in 6 of 8 runs on both paths, B-off reads +0.28 and +0.57 µs, and the census's
primary-key arms give the same sign on seven of eight relations (literal: +0.8, +0.7, +0.5, +0.4 µs on 200, 1,000, 10,000 and
100,000 rows; `nul0` -0.1 µs; +1 % to +3 %), and every one is within its own spread:

**Point `SELECT * FROM <rel> WHERE id = ?` (the pk lookup; it does not scan), literal path, server CPU per statement over 8 runs of 3,000 statements**

| relation (rows) | A | A' | B | B-off | A-al64 | A statements/s (CPU-derived) | B statements/s (CPU-derived) | B - A | B-off - A | A - A' (S = max abs) | B against S |
|---|---|---|---|---|---|---|---|---|---|---|---|
| s200 (200) | 27.21 [26.51, 27.92] µs | 27.54 [26.33, 28.39] µs | 27.94 [26.58, 28.56] µs | 27.69 [27.05, 29.02] µs | 27.16 [26.57, 30.03] µs | 36,747 /s | 35,797 /s | +0.81 [-1.23, +1.73] µs (+3.0 %) | +0.54 [-0.07, +1.72] µs (+2.0 %) | -0.07 [-1.86, +0.79] µs (S = 1.86 µs) | within the spread (B - A above the typical gap 0.56) |
| s1k (1,000) | 28.82 [28.24, 29.92] µs | 29.07 [27.92, 31.67] µs | 29.69 [28.30, 30.22] µs | 29.92 [28.86, 31.21] µs | 29.03 [28.36, 30.19] µs | 34,702 /s | 33,682 /s | +0.67 [-0.20, +1.46] µs (+2.3 %) | +0.72 [-0.27, +2.97] µs (+2.5 %) | -0.15 [-3.43, +1.08] µs (S = 3.43 µs) | within the spread (B - A above the typical gap 0.568) |
| s (10,000) | 27.04 [26.59, 30.39] µs | 27.38 [26.44, 28.03] µs | 27.75 [26.79, 28.35] µs | 27.72 [26.89, 29.17] µs | 27.40 [26.39, 28.01] µs | 36,981 /s | 36,030 /s | +0.49 [-2.90, +1.55] µs (+1.8 %) | +0.43 [-1.90, +1.52] µs (+1.6 %) | -0.24 [-0.82, +3.09] µs (S = 3.09 µs) | within the spread (B - A above the typical gap 0.458) |
| s100k (100,000) | 27.97 [27.41, 29.04] µs | 28.28 [26.73, 29.10] µs | 28.65 [27.67, 29.00] µs | 28.32 [27.77, 29.53] µs | 27.94 [26.89, 29.96] µs | 35,751 /s | 34,900 /s | +0.37 [-0.49, +1.34] µs (+1.3 %) | +0.37 [-0.54, +1.15] µs (+1.3 %) | +0.09 [-0.92, +0.68] µs (S = 0.92 µs) | within the spread (inside the typical gap 0.436) |
| srec (10,000) | 27.25 [26.88, 29.32] µs | 27.47 [26.54, 29.23] µs | 27.85 [27.12, 28.52] µs | 27.38 [27.10, 28.60] µs | 27.07 [26.23, 27.90] µs | 36,700 /s | 35,912 /s | +0.47 [-0.80, +1.04] µs (+1.7 %) | +0.08 [-0.71, +1.14] µs (+0.3 %) | +0.12 [-0.65, +0.65] µs (S = 0.65 µs) | within the spread (B - A above the typical gap 0.301) |
| wd (10,000) | 30.03 [29.50, 31.61] µs | 30.36 [28.71, 30.68] µs | 30.98 [29.86, 31.67] µs | 30.95 [30.11, 32.26] µs | 30.35 [29.61, 31.34] µs | 33,298 /s | 32,276 /s | +0.36 [-0.44, +2.10] µs (+1.2 %) | +0.61 [+0.05, +1.57] µs (+2.0 %) | +0.50 [-1.10, +1.07] µs (S = 1.10 µs) | within the spread (inside the typical gap 0.788) |
| nul0 (10,000) | 28.03 [27.11, 29.05] µs | 27.62 [27.12, 29.84] µs | 28.14 [27.44, 29.11] µs | 28.22 [27.92, 29.89] µs | 27.75 [26.83, 29.48] µs | 35,681 /s | 35,540 /s | -0.11 [-0.92, +1.29] µs (-0.4 %) | +0.41 [-0.41, +2.08] µs (+1.4 %) | +0.12 [-1.61, +1.54] µs (S = 1.61 µs) | within the spread (inside the typical gap 0.7) |
| nul50 (10,000) | 27.86 [27.12, 28.27] µs | 27.87 [26.70, 28.77] µs | 28.29 [27.53, 29.64] µs | 28.31 [27.50, 29.31] µs | 27.77 [26.74, 29.16] µs | 35,899 /s | 35,346 /s | +0.27 [-0.38, +1.99] µs (+1.0 %) | +0.80 [-0.40, +1.13] µs (+2.9 %) | +0.15 [-0.96, +0.55] µs (S = 0.96 µs) | within the spread (inside the typical gap 0.462) |

**Point `SELECT * FROM <rel> WHERE id = ?` (the pk lookup; it does not scan), prepared path, server CPU per statement over 8 runs of 3,000 statements**

| relation (rows) | A | A' | B | B-off | A-al64 | A statements/s (CPU-derived) | B statements/s (CPU-derived) | B - A | B-off - A | A - A' (S = max abs) | B against S |
|---|---|---|---|---|---|---|---|---|---|---|---|
| s200 (200) | 26.36 [25.83, 27.41] µs | 26.95 [25.66, 29.60] µs | 27.43 [26.15, 28.86] µs | 27.18 [26.31, 28.11] µs | 26.37 [25.75, 27.37] µs | 37,930 /s | 36,451 /s | +0.92 [-0.43, +1.53] µs (+3.5 %) | +0.72 [+0.09, +1.64] µs (+2.7 %) | -0.54 [-2.18, +0.55] µs (S = 2.18 µs) | within the spread (B - A above the typical gap 0.708) |
| s1k (1,000) | 28.14 [27.47, 29.25] µs | 28.31 [27.01, 30.23] µs | 28.88 [27.88, 29.38] µs | 28.94 [28.12, 30.77] µs | 28.06 [27.56, 29.62] µs | 35,535 /s | 34,625 /s | +0.55 [-0.24, +1.75] µs (+1.9 %) | +0.47 [-0.02, +3.30] µs (+1.7 %) | -0.10 [-2.76, +1.50] µs (S = 2.76 µs) | within the spread (inside the typical gap 0.845) |
| s (10,000) | 26.26 [25.96, 29.29] µs | 26.92 [25.64, 27.86] µs | 27.35 [26.22, 28.00] µs | 26.81 [26.27, 28.43] µs | 26.42 [25.53, 27.02] µs | 38,083 /s | 36,558 /s | +0.31 [-1.88, +1.89] µs (+1.2 %) | +0.27 [-1.73, +1.29] µs (+1.0 %) | -0.23 [-0.92, +3.21] µs (S = 3.21 µs) | within the spread (inside the typical gap 0.527) |
| s100k (100,000) | 27.27 [26.46, 28.06] µs | 27.50 [26.33, 28.25] µs | 27.73 [26.96, 28.46] µs | 27.75 [27.05, 29.00] µs | 26.94 [26.26, 27.84] µs | 36,667 /s | 36,065 /s | +0.50 [-0.19, +1.12] µs (+1.8 %) | +0.56 [+0.03, +1.49] µs (+2.1 %) | -0.06 [-1.08, +1.23] µs (S = 1.23 µs) | within the spread (B - A above the typical gap 0.397) |
| srec (10,000) | 26.59 [25.83, 29.48] µs | 26.62 [25.42, 27.93] µs | 27.00 [26.22, 27.79] µs | 27.02 [26.12, 28.09] µs | 26.33 [25.49, 27.10] µs | 37,606 /s | 37,041 /s | +0.25 [-1.79, +1.33] µs (+0.9 %) | +0.30 [-1.89, +1.36] µs (+1.1 %) | +0.14 [-0.92, +1.55] µs (S = 1.55 µs) | within the spread (inside the typical gap 0.7) |
| wd (10,000) | 29.12 [28.45, 31.14] µs | 29.73 [27.99, 30.13] µs | 30.29 [29.18, 32.36] µs | 30.22 [29.23, 31.51] µs | 29.37 [28.70, 30.27] µs | 34,338 /s | 33,018 /s | +1.25 [+0.05, +1.93] µs (+4.3 %) | +1.12 [-0.08, +1.85] µs (+3.9 %) | +0.29 [-1.42, +1.17] µs (S = 1.42 µs) | within the spread (B - A above the typical gap 0.497) |
| nul0 (10,000) | 27.37 [26.24, 28.14] µs | 27.06 [26.28, 28.91] µs | 27.54 [26.78, 28.04] µs | 27.25 [26.73, 28.92] µs | 27.03 [25.90, 28.90] µs | 36,533 /s | 36,316 /s | +0.03 [-0.65, +1.39] µs (+0.1 %) | +0.60 [-1.34, +0.86] µs (+2.2 %) | +0.28 [-1.38, +1.38] µs (S = 1.38 µs) | within the spread (inside the typical gap 0.927) |
| nul50 (10,000) | 27.01 [26.50, 27.70] µs | 27.08 [25.90, 29.94] µs | 27.40 [26.79, 28.57] µs | 27.43 [26.99, 28.19] µs | 27.04 [25.82, 27.29] µs | 37,024 /s | 36,492 /s | +0.33 [-0.32, +1.71] µs (+1.2 %) | +0.37 [-0.71, +1.28] µs (+1.4 %) | -0.11 [-2.83, +0.97] µs (S = 2.83 µs) | within the spread (inside the typical gap 0.653) |

So a point statement in B costs about **0.3-1.3 µs (1-4 %) more than in A, with the kernel on or off**. The kernel is not
evaluated for it; what B added that every statement pays is `CheckStepShapeOnce` and its kernel compile at the step's first
visible row. That is read from the diff, not isolated: this run did not vary it. It stays inside the bar, and it is the
cell where the bar's spread (the largest run's gap) is wider than the effect.

### 3.3 The every-row `UPDATE`: slower by 4.2-4.7 % in 13 of 13 runs, and the bar read it two ways

`update-range` (`UPDATE r SET v = ? WHERE id >= 1`, 1,000 rows), server CPU per statement over 8 runs x 240 statements per path.
The rows are in the table of 3.2 (`update-range`); per run, literal path:

| run | A | A' | B | B-off | A - A' | B - A | B-off - A |
|---|---|---|---|---|---|---|---|
| 0 | 750.9 µs | 748.5 µs | 792.5 µs | 794.7 µs | +2.5 µs | +41.5 µs | +43.8 µs |
| 1 | 727.9 µs | 721.0 µs | 759.0 µs | 759.4 µs | +6.9 µs | +31.1 µs | +31.4 µs |
| 2 | 751.9 µs | 762.1 µs | 792.1 µs | 755.6 µs | -10.2 µs | +40.2 µs | +3.7 µs |
| 3 | 740.8 µs | 725.8 µs | 768.7 µs | 773.1 µs | +15.0 µs | +27.9 µs | +32.3 µs |
| 4 | 755.8 µs | 730.4 µs | 767.6 µs | 800.2 µs | +25.3 µs | +11.8 µs | +44.5 µs |
| 5 | 720.4 µs | 722.3 µs | 764.7 µs | 759.0 µs | -1.8 µs | +44.2 µs | +38.5 µs |
| 6 | 721.4 µs | 759.7 µs | 759.2 µs | 792.5 µs | -38.4 µs | +37.9 µs | +71.1 µs |
| 7 | 725.3 µs | 716.1 µs | 755.1 µs | 757.7 µs | +9.2 µs | +29.8 µs | +32.3 µs |

**B - A = +34.5 µs of 734 (+4.7 %) literal and +35.0 µs (+4.7 %) prepared, B slower than A in 8 of 8 runs on both paths.** By the
bar fixed in advance this is **within the spread**: S is 38.4 µs literal (60.0 prepared), and each is the gap of **one**
outlying run (run 6 literal, run 3 prepared). The typical gap is 9.7 and 11.8 µs, and the median `B - A` is 3.6 and 3.0
times it. Read the table: A and A' reach 716-730 µs in 9 of their 16 runs; **B never goes under 755 µs, and B-off never under 756**.

**The independent re-measure** (the layout probe's cell: four fresh servers A, A', B, A-al64, 5 runs, same flags, started after
the profile; it is not part of the 8-run table) reads the same constant and a tighter spread:

**`tools/bn_census.py`, `--rows 1000`: server CPU per statement (µs, median over 5 runs [min, max]); statements/s is CPU-derived (1e6 / CPU µs); deltas paired by run**

| shape | path | A | A' | B | A-al64 | A statements/s | B statements/s | B - A | A-al64 - A (layout probe) | A - A' (S = max abs) | B against S | A wall p50 | B wall p50 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| update-range | literal | 742.95 [742.66, 749.45] µs | 738.41 [732.29, 752.69] µs | 779.02 [767.19, 794.12] µs | 731.87 [712.11, 744.29] µs | 1,346 /s | 1,284 /s | +31.41 [+24.24, +51.40] µs (+4.2 %) | -13.02 [-30.84, -5.16] µs (-1.8 %) | +4.25 [-3.65, +10.67] µs (S = 10.67 µs) | slower than the spread | 762.7 µs | 798.7 µs |
| update-range | prepared | 737.93 [725.63, 775.87] µs | 741.18 [736.85, 798.05] µs | 772.31 [759.28, 787.05] µs | 722.91 [711.09, 765.90] µs | 1,355 /s | 1,295 /s | +31.80 [+11.14, +49.12] µs (+4.3 %) | -16.25 [-52.96, +27.97] µs (-2.2 %) | -13.62 [-22.18, +8.34] µs (S = 22.18 µs) | slower than the spread | 750.6 µs | 785.3 µs |

B slower than A in k of 5 runs: update-range/literal 5, update-range/prepared 5
end state and page growth: {"end_state": {"A": {"range_v_ok": true, "spill_txt_ok": true}, "A2": {"range_v_ok": true, "spill_txt_ok": true}, "B": {"range_v_ok": true, "spill_txt_ok": true}, "Apad": {"range_v_ok": true, "spill_txt_ok": true}}}

**B - A = +31.4 µs of 743 (+4.2 %) literal and +31.8 µs (+4.3 %) prepared, B slower than A in 5 of 5 runs, spread 10.7 and
22.2 µs: slower than the spread on both paths.** So the bar read "within" on one measurement and "slower" on the other, because
`S` is set by the worst A-against-A' run; the constant is the same in both (+31 to +35 µs, 13 of 13 runs).
`scan_kernel = off` does not change that (B-off - A = +35.4 and +32.8 µs), and an every-row `UPDATE` reaches neither the slot
loop nor the kernel, as the order itself says. So the cost is in something B shares with B-off; the candidates, read from the diff and not isolated, are the shared-code
changes the `UPDATE` path also runs (S2's split of `EvaluateConjunct`, `DecodeChecks`, `ReadTupleIn`, `f5a7efb2`'s instrument) and
code layout. The layout probe is on the same cell: A-al64 - A = -13.0 µs (-1.8 %) literal and -16.3 µs (-2.2 %) prepared, so a plain alignment change moves the cell about 2 % toward faster, and does not reproduce a 4 % cost. This run does not say which shared change it is.

## 4. The harness: A and A' are one executable, and the instance still moves the cell

**A rebuild of A from a second export is byte-identical to A.** The two binaries have one sha256, so the A-against-A' spread
BM-Q12 uses contains the host and the server *instance* (start order, port, data-file path, ASLR) and **no code layout**. It is
not small: on the scan row A' reads slower than A in every run (`bn` literal: A - A' = -3.2 ns, [-6.4, -1.5]; -4.5 % of A), and
on the every-row `UPDATE` single runs differ by up to 38 µs literal and 60 µs prepared (5-8 %). Rotating the visiting order balances the position within a
block, not the start order of the five servers, which is fixed (A first). A delta under about 5 % on a scan cell, or under
about 5 % on the `UPDATE`, is inside what two instances of one executable do here.

**The layout probe.** A's source built with every function aligned to 64 bytes (A-al64) reads **-1.9 ns on `bn`** ([-4.8, -0.3];
-2.7 %), -2 to -5 % on the wide cells, +0.6 % on `sel-100` and `wide-m`: a layout change alone moves a scan cell by up to
5 %, toward faster on all but those two, so a few percent on a cell is not a code finding either.
On the every-row `UPDATE` (3.3) the probe reads -13.0 µs (-1.8 %) literal, [-30.8, -5.2] over 5 runs.

## 5. Against PostgreSQL 18 (a floor, not a reference)

**On `bn`, B costs 1.07 times PostgreSQL's row (31.0 ns against 29.1 vacuumed); A cost 2.41 times.** The twin is `tools/pg_*`'s
cluster on port 15434, untuned, the same relations and filters, simple query protocol, 4 runs (cut to the time budget; no
not-vacuumed pass). B is **below** PostgreSQL on the cells where PostgreSQL pays for the type (`wide-h` 0.52, `wide-h-k` 0.51, `wide-g`
0.59, `wide-k` 0.62, `wide-m` 0.74: `numeric`, `char`, `varchar`), at parity on the integer, bool, date and timestamp families
(0.90-1.09) and **above** it where the cell is the result path (`out-200` 1.89, `out-1k` 1.28, `sel-100` 1.24, `sel-10` 1.12) and on
`nul-50` (1.57). `rows-200` has no ratio: PostgreSQL's primary-key lookup costs more than its 200-row scan, so its (scan - point)
is negative and is not a row cost. A result is not good because it is near PostgreSQL; the table says which cells are left.

**Against PostgreSQL 18.6 on the same host and pinning (simple query protocol, a literal per statement; KDS literal path); a floor, not a reference**

| cell | rows scanned | A | B | PG18 vacuumed | A / PG | B / PG | PG18 not vacuumed | A / PG | B / PG |
|---|---|---|---|---|---|---|---|---|---|
| rows-200 | 200 | 75.7 ns | 36.3 ns | -11.3 [-15.2, -7.9] ns (negative: not a row cost) | n/m | n/m | not measured | - | - |
| rows-1k | 1,000 | 70.8 ns | 32.3 ns | 26.0 [24.9, 27.8] ns | 2.73 | 1.25 | not measured | - | - |
| bn | 10,000 | 70.0 ns | 31.0 ns | 29.1 [28.6, 29.5] ns | 2.41 | 1.07 | not measured | - | - |
| rows-100k | 100,000 | 69.3 ns | 30.2 ns | 29.6 [29.0, 29.8] ns | 2.35 | 1.02 | not measured | - | - |
| bn-recent | 10,000 | 70.7 ns | 31.6 ns | not measured | - | - | not measured | - | - |
| sel-0 | 10,000 | 70.3 ns | 31.5 ns | 29.2 [28.6, 29.6] ns | 2.41 | 1.08 | not measured | - | - |
| sel-1 | 10,000 | 72.0 ns | 32.7 ns | 30.2 [30.1, 31.0] ns | 2.39 | 1.08 | not measured | - | - |
| sel-10 | 10,000 | 83.0 ns | 48.3 ns | 43.1 [42.4, 43.5] ns | 1.93 | 1.12 | not measured | - | - |
| sel-100 | 10,000 | 206.8 ns | 203.1 ns | 164.0 [162.6, 164.4] ns | 1.26 | 1.24 | not measured | - | - |
| out-200 | 200 | 237.3 ns | 235.1 ns | 124.4 [122.7, 134.1] ns | 1.91 | 1.89 | not measured | - | - |
| out-1k | 1,000 | 213.4 ns | 209.5 ns | 164.0 [160.4, 167.3] ns | 1.30 | 1.28 | not measured | - | - |
| wide-h | 10,000 | 93.4 ns | 35.6 ns | 68.5 [67.7, 75.8] ns | 1.36 | 0.52 | not measured | - | - |
| wide-h-k | 10,000 | 105.0 ns | 36.0 ns | 70.6 [70.5, 74.4] ns | 1.49 | 0.51 | not measured | - | - |
| wide-a | 10,000 | 90.0 ns | 39.0 ns | 35.7 [35.4, 36.5] ns | 2.52 | 1.09 | not measured | - | - |
| wide-c | 10,000 | 99.9 ns | 36.7 ns | 39.9 [39.4, 40.4] ns | 2.50 | 0.92 | not measured | - | - |
| wide-d | 10,000 | 88.7 ns | 36.8 ns | 38.3 [38.1, 38.7] ns | 2.32 | 0.96 | not measured | - | - |
| wide-e | 10,000 | 90.0 ns | 38.5 ns | 42.5 [42.0, 43.2] ns | 2.12 | 0.91 | not measured | - | - |
| wide-f | 10,000 | 91.3 ns | 39.5 ns | 43.7 [43.6, 44.2] ns | 2.09 | 0.90 | not measured | - | - |
| wide-g | 10,000 | 92.6 ns | 39.3 ns | 66.3 [65.3, 69.6] ns | 1.40 | 0.59 | not measured | - | - |
| wide-k | 10,000 | 101.6 ns | 39.7 ns | 63.8 [62.6, 64.5] ns | 1.59 | 0.62 | not measured | - | - |
| wide-m | 10,000 | 109.8 ns | 44.3 ns | 60.1 [59.8, 62.1] ns | 1.83 | 0.74 | not measured | - | - |
| nul-0 | 10,000 | 70.2 ns | 32.0 ns | 27.2 [26.6, 27.8] ns | 2.59 | 1.18 | not measured | - | - |
| nul-50 | 10,000 | 69.1 ns | 43.9 ns | 27.9 [27.5, 28.6] ns | 2.48 | 1.57 | not measured | - | - |

## 6. What the row costs now: 29.3 ns on the profiled build, 43 % of it per-row checks and counters

The sampler (`tools/kds_sampler.c`, 1 kHz of process CPU, the frame-pointer build of B, 20,000 literal scans of `bn`) folded into
`tools/bn_parts.json` (one change: `row walk: loop and callback` also matches the VM's own slot-loop lambda,
`RunWalkStep::{lambda`, which B has in place of `BtreeVisitLeafPage` and the `std::function` thunk; A's folds are unchanged).
6,366 samples, 319.5 µs of server CPU a scan, 26.1 µs a point statement on the same server, **net row 29.3 ns** (the plain build's
census reads 31.0 beside four idle servers on the same CPU; the profiled server ran alone, so the 1.7 ns is not the frame pointers or
the handler and is not separated here).

| part (`tools/bn_parts.json`, first match) | ns per scanned row (share of the statement) | what it holds in B |
|---|---|---|
| row walk: per-row checks and counters | **13.8 ns (43.3 %)** | `AcceptTupleAt`'s own body 9.1 ns (28.5 %), `PageView::ReadTupleIn` 3.4 ns (10.5 %), `ExecStats::For` 1.4 ns (4.3 %) |
| row walk: residual | 7.4 ns (23.3 %) | `ScanKernelRejects` alone: the typed compare |
| row walk: loop and callback | 6.2 ns (19.4 %) | the VM's slot loop and `visit_slot` (`RunWalkStep::{lambda#1}`) |
| socket send | 1.4 ns (4.3 %) | |
| transaction begin, commit, locks | 0.7 ns (2.1 %) | the borrow's lock-table traffic per 64-page slice |
| socket read and wait | 0.5 ns (1.4 %) | |
| statement scaffolding | 0.6 ns (2.0 %) | |
| kwp frames, parse, compile, Waystone, result encoding, scheduler | 1.1 ns (3.1 %) | |
| descent and page access | 0.3 ns (1.0 %) | |
| row decode and encode; row walk: copy; row walk: visibility | 0.0 ns (0.1 %) | the rejected row is neither decoded nor copied; the floor-path visibility decision is inlined into `AcceptTupleAt` |
| samples; server CPU per scan, sampled; per point statement; net row | 6,366; 319.5 µs; 26.1 µs; 29.3 ns | |
| parts BM-S2..S4 replaced (loop and callback, per-row checks and counters, copy, decode, residual) | 27.5 ns = 93.7 % of the net row | what is left is what BM kept by construction plus its own compare |

**The largest remaining part is `AcceptTupleAt`'s own body, 9.1 ns (28.5 %)**, and the largest part of the fold is per-row checks
and counters, 13.8 ns. BM-S1 priced `AcceptTupleAt`'s self time on A at 10.5 ns, of which the visibility decision was 2.2; the
kernel removed what ran after it (decode 19.7 ns, copy 1.2, residual 13.0, the thunk 12.3 in BM-S1's `bn` profile) and not this
frame. The kernel's compare (7.4 ns) and the slot loop (6.2 ns) are the next two. Together the three are 86 % of the statement.
`wide-h` was not profiled (time budget), and no profile was taken of the 100 % cell or of B-off.

## 7. Waits: where a `bn` scan statement goes

A scan statement is a read: it holds no commit wait, writes no record, and with every page resident does no I/O.

| unit measured: one `bn` scan, B, literal, p50 wall 378.5 µs | measured | share of p50 |
|---|---|---|
| durability / commit (fsync) | not applicable: read-only statement, no WAL record | 0 |
| write statement | not applicable | 0 |
| read: the row walk (profile: residual, per-row checks, loop and callback, decode, descent: 87 % of the sampled statement) | 87 % of 338 µs server CPU (31.0 ns x 10,000 + 27.7 µs point cost) ≈ 294 µs | about 78 % |
| server, other than the walk (socket send and read, KWP frames, parse, compile, locks, scaffolding, scheduler) | 13 % of 338 µs ≈ 44 µs | about 12 % |
| client and socket round trip, off the server's CPU | 378.5 - 338 ≈ 40 µs (the driver's own CPU is ~45 µs a statement; the two overlap) | about 10 % |
| lock or conflict wait | none: `cores = 1` (the window latch is unarmed), no writer; the borrow's lock-table traffic is CPU, inside the 0.7 ns | 0 |
| I/O wait | none by construction (65,536 frames against at most 3,000 pages; `pool_misses` is 0 in every census cell but `point:s1k`, see Appendix B) | 0 |

The shares are derived from the profile's part shares scaled to the plain build's server CPU, not separately sampled.

## 8. The noise floor, and the host

| source | what it says |
|---|---|
| A against A' (one executable, two instances) | the median A - A' is -0.1 to -4.7 ns on the selective cells, with a systematic sign on `bn` (A' slower in 8 of 8 runs, -3.2 ns, -4.5 %); on the every-row `UPDATE` single runs differ by up to 38 µs literal and 60 µs prepared. See 3.3 and 4 |
| Layout probe (A-al64 against A) | -1.9 ns on `bn` (-2.7 %), up to -6 ns on the string cells (-5 to -6 %); see 4 |
| Literal against prepared | the two paths' deltas agree to within 1.3 ns on every cell of the census |
| Host | quiet for the whole measurement: 248 of 248 census cell-runs clean (siblings of the server's and driver's CPUs under 20 % busy, no other session's task above 15 % of the pinned CPUs), 64 of 64 `bn_census` cell-runs clean, 116 of 116 PostgreSQL cell-runs clean (load at most 1.39), 10 of 10 probe cell-runs clean (two started at load 2.25 and 2.58, other sessions beginning to build), 0 cell-runs with a competing build or test run recorded; Appendix B |

**So a difference under about 3 % on a scan cell, or about 5 % on a single `UPDATE` run, is not a finding on its own.** Every
delta of section 2 is far outside it (-20 % to -66 %). The section 3.2 delta (+1.6 %) is inside it and is reported with its run
count; the section 3.3 delta (+4.2 to +4.7 %) is the same in 13 of 13 runs on two measurements, which is what makes it a
finding where one run would not.

## 9. What the run teaches

1. **BM's premise held, and what is left of the row is what BM kept by construction plus the kernel's own compare.** BM-Q1 said
   the parts S2..S4 replace were 96 % of A's row; the same five parts are 93.7 % of B's, but the row is 29-31 ns instead of 65-70,
   and decode and copy are zero on a rejected row. The largest left is the per-row frame (`AcceptTupleAt` 9.1 ns), then the kernel's own compare (7.4) and the slot loop (6.2).
   `known-gaps.md`'s BN entry (*"a scan row costs twice PostgreSQL's, and no single part of it dominates"*) no longer holds on
   BN's shape: 1.07 times, and one frame is 29 % of the statement. It still holds for the result path (1.24-1.89 times on the
   cells that emit most rows) and for 50 % NULL (1.57).
2. **The kernel is a pre-filter, and a pre-filter costs the rows it keeps.** +6 to +15 ns a row on the all-pass cells (3.1);
   S2 pays for it there, so the 100 % scan ends level with A. Nothing in this run says the cost is material enough to take
   BM-Q2 (b); it does say the kernel is worth nothing where selectivity is near 100 %.
3. **NULL is where BM-Q3 (a) costs:** 23.8 ns a NULL row to the residual (section 2). If a workload scans nullable columns that are
   mostly NULL, (b) is the lever, and I16 would be amended to one function and two call sites.
4. **The write path does not get the saving and pays a constant for the shared code.** The every-row `UPDATE` is +4.2 to +4.7 %
   in 13 of 13 runs with the kernel on or off (3.3); the point `SELECT` +1-4 % (3.2). Neither is the kernel. The `UPDATE`'s
   cost is the one guard that does not hold, and the order's bar read it two ways because its spread is one run's gap. The layout
   probe moves the cell -1.8 % and so does not reproduce it; which of S2's shared-code changes it is was not isolated here.
5. **A rebuild is not a layout probe.** The order's BM-Q12 revision took a rebuild of identical code as the layout component of
   the spread. On this toolchain the rebuild is byte-identical, so the spread has none. A probe that changes the layout (alignment)
   is needed to bound it, and the probe moves a cell up to 5 %.
6. **The all-pass 100 % cell and the result path are untouched, and unchanged against PostgreSQL's** (1.24 times on `sel-100`;
   BM-S1: 1.23): the next scan lever for those cells is the result path (a row emitted costs 136 ns over a rejected one in A and
   172 in B by the same subtraction, `sel-100` against `sel-0`), which is not BM's.

## 10. Not run

Cut to the one-hour budget (section 1): the not-vacuumed PostgreSQL twin; PostgreSQL runs 5-8; `bn_census` at 200 and 10,000
rows (so the every-row `UPDATE` and the point `SELECT` have one `bn_census` size; the point `SELECT` has four sizes in the census);
a layout-probe server in `bn_census`'s 8-run measurement (the probe ran as its own 5-run cell); the `wide-h` profile; a B-off profile; a profile of the 100 % cell.
Not run for other reasons: the writers cell (BM-S5 was struck at BM-S1); the census at `cores > 1`; `strict` durability (a read
has no commit wait); cold reads (a BM non-goal); heap relations (suspended); the suite (the stamp). The 200-row PostgreSQL ratio
does not exist (section 5). The profile's per-part shares carry the sampler's error: a part of 28 % of 6,366 samples is ±1.1 points.

## 11. Baseline for the next run

This file is the baseline for the scan cells, the three guards and the profile at `v2.7.0-946-g6b771ec1`. Hold the next run
against (literal, net row, median of 8): `bn` **31.0 ns** (A, the opening, 70.0), `rows-1k` 32.3, `rows-100k` 30.2, `sel-10`
48.3, `sel-100` 203.1, `wide-h` 35.6, `wide-h-k` 36.0, `wide-m` 44.3, `nul-0` 32.0, `nul-50` 43.9; the guards: `sel-100` scan
2,059 µs of server CPU, `select-pk` 28.9 µs, `update-range` 766 µs (1,000 rows); the profile's `bn` net row 29.3 ns with
`AcceptTupleAt` 9.1 ns; PostgreSQL 18.6 vacuumed `bn` 29.1 ns.

## Appendix A. Percentiles

Wall time per statement as the client sees it, pooled over the runs (the census: every statement of the 8 runs; `bn_census`: the
median over runs of each run's percentile, which has no p95). Percentiles of the 100 % cells are the Python driver's decode of
10,000 rows a reply, not the server's. `n` is the pooled statement count; statements/s is 1,000,000 / p50.

### A.1 The census (`tools/bm_scan.py`)

| cell | path | arm | n | p0 | p25 | p50 | p90 | p95 | p99 | statements/s at p50 (derived) |
|---|---|---|---|---|---|---|---|---|---|---|
| point:s200 | literal | A | 24,000 | 49.0 µs | 55.0 µs | 56.1 µs | 68.1 µs | 73.8 µs | 88.5 µs | 17,825 /s |
| point:s200 | literal | A' | 24,000 | 47.1 µs | 55.1 µs | 56.2 µs | 68.9 µs | 74.9 µs | 89.6 µs | 17,794 /s |
| point:s200 | literal | B | 24,000 | 47.9 µs | 55.5 µs | 56.6 µs | 68.9 µs | 74.0 µs | 88.1 µs | 17,668 /s |
| point:s200 | literal | B-off | 24,000 | 47.7 µs | 55.5 µs | 56.7 µs | 69.1 µs | 74.3 µs | 88.7 µs | 17,637 /s |
| point:s200 | literal | A-al64 | 24,000 | 51.6 µs | 55.1 µs | 56.2 µs | 68.8 µs | 74.9 µs | 92.6 µs | 17,794 /s |
| point:s200 | prepared | A | 24,000 | 46.6 µs | 51.6 µs | 52.7 µs | 64.9 µs | 70.1 µs | 84.7 µs | 18,975 /s |
| point:s200 | prepared | A' | 24,000 | 46.0 µs | 51.8 µs | 52.8 µs | 66.0 µs | 73.2 µs | 90.0 µs | 18,939 /s |
| point:s200 | prepared | B | 24,000 | 47.5 µs | 52.2 µs | 53.2 µs | 65.9 µs | 72.4 µs | 89.1 µs | 18,797 /s |
| point:s200 | prepared | B-off | 24,000 | 45.8 µs | 52.3 µs | 53.5 µs | 66.3 µs | 72.5 µs | 87.1 µs | 18,692 /s |
| point:s200 | prepared | A-al64 | 24,000 | 47.4 µs | 51.6 µs | 52.7 µs | 64.7 µs | 70.6 µs | 86.1 µs | 18,975 /s |
| rows-200 | literal | A | 12,288 | 65.0 µs | 68.9 µs | 70.7 µs | 86.2 µs | 93.6 µs | 119.8 µs | 14,144 /s |
| rows-200 | literal | A' | 12,288 | 66.7 µs | 69.6 µs | 71.2 µs | 86.2 µs | 92.7 µs | 119.8 µs | 14,045 /s |
| rows-200 | literal | B | 12,288 | 58.3 µs | 62.1 µs | 63.5 µs | 78.4 µs | 85.8 µs | 113.4 µs | 15,748 /s |
| rows-200 | literal | B-off | 12,288 | 63.5 µs | 66.8 µs | 68.4 µs | 85.2 µs | 92.9 µs | 120.1 µs | 14,620 /s |
| rows-200 | literal | A-al64 | 12,288 | 65.6 µs | 68.7 µs | 70.4 µs | 86.1 µs | 93.2 µs | 116.0 µs | 14,205 /s |
| rows-200 | prepared | A | 12,288 | 60.5 µs | 65.4 µs | 67.1 µs | 83.8 µs | 92.5 µs | 119.7 µs | 14,903 /s |
| rows-200 | prepared | A' | 12,288 | 61.9 µs | 66.1 µs | 67.6 µs | 82.1 µs | 88.7 µs | 108.0 µs | 14,793 /s |
| rows-200 | prepared | B | 12,288 | 54.3 µs | 58.7 µs | 59.9 µs | 73.8 µs | 81.0 µs | 103.0 µs | 16,694 /s |
| rows-200 | prepared | B-off | 12,288 | 59.9 µs | 63.4 µs | 64.9 µs | 79.1 µs | 85.8 µs | 104.5 µs | 15,408 /s |
| rows-200 | prepared | A-al64 | 12,288 | 61.4 µs | 65.1 µs | 66.6 µs | 81.0 µs | 87.8 µs | 108.4 µs | 15,015 /s |
| point:s1k | literal | A | 24,000 | 50.7 µs | 55.6 µs | 57.4 µs | 70.9 µs | 77.4 µs | 91.8 µs | 17,422 /s |
| point:s1k | literal | A' | 24,000 | 48.1 µs | 55.6 µs | 57.6 µs | 72.4 µs | 79.7 µs | 97.3 µs | 17,361 /s |
| point:s1k | literal | B | 24,000 | 49.5 µs | 56.0 µs | 58.0 µs | 72.2 µs | 79.8 µs | 100.0 µs | 17,241 /s |
| point:s1k | literal | B-off | 24,000 | 51.7 µs | 56.2 µs | 58.4 µs | 73.8 µs | 81.7 µs | 100.4 µs | 17,123 /s |
| point:s1k | literal | A-al64 | 24,000 | 50.7 µs | 55.6 µs | 57.6 µs | 73.5 µs | 81.6 µs | 99.9 µs | 17,361 /s |
| point:s1k | prepared | A | 24,000 | 45.5 µs | 52.2 µs | 54.0 µs | 67.1 µs | 73.8 µs | 87.4 µs | 18,519 /s |
| point:s1k | prepared | A' | 24,000 | 46.6 µs | 52.3 µs | 54.2 µs | 68.2 µs | 74.9 µs | 90.6 µs | 18,450 /s |
| point:s1k | prepared | B | 24,000 | 47.2 µs | 52.7 µs | 54.6 µs | 68.3 µs | 75.1 µs | 90.8 µs | 18,315 /s |
| point:s1k | prepared | B-off | 24,000 | 46.4 µs | 52.9 µs | 54.9 µs | 69.0 µs | 75.9 µs | 92.0 µs | 18,215 /s |
| point:s1k | prepared | A-al64 | 24,000 | 47.7 µs | 52.2 µs | 54.2 µs | 69.2 µs | 75.9 µs | 92.6 µs | 18,450 /s |
| rows-1k | literal | A | 6,144 | 119.1 µs | 124.3 µs | 128.9 µs | 152.0 µs | 163.1 µs | 193.4 µs | 7,758 /s |
| rows-1k | literal | A' | 6,144 | 121.4 µs | 127.6 µs | 132.5 µs | 156.6 µs | 169.4 µs | 209.2 µs | 7,547 /s |
| rows-1k | literal | B | 6,144 | 78.7 µs | 86.8 µs | 89.6 µs | 109.9 µs | 120.3 µs | 155.3 µs | 11,161 /s |
| rows-1k | literal | B-off | 6,144 | 107.1 µs | 111.8 µs | 115.4 µs | 139.0 µs | 151.7 µs | 183.9 µs | 8,666 /s |
| rows-1k | literal | A-al64 | 6,144 | 113.1 µs | 122.6 µs | 127.8 µs | 153.0 µs | 168.1 µs | 213.5 µs | 7,825 /s |
| rows-1k | prepared | A | 6,144 | 112.5 µs | 121.0 µs | 125.5 µs | 147.5 µs | 157.6 µs | 190.3 µs | 7,968 /s |
| rows-1k | prepared | A' | 6,144 | 118.0 µs | 124.2 µs | 128.4 µs | 150.8 µs | 160.9 µs | 190.5 µs | 7,788 /s |
| rows-1k | prepared | B | 6,144 | 76.4 µs | 83.4 µs | 85.8 µs | 104.5 µs | 111.7 µs | 149.8 µs | 11,655 /s |
| rows-1k | prepared | B-off | 6,144 | 102.6 µs | 108.5 µs | 112.0 µs | 132.7 µs | 142.2 µs | 169.0 µs | 8,929 /s |
| rows-1k | prepared | A-al64 | 6,144 | 115.0 µs | 119.1 µs | 123.8 µs | 147.7 µs | 163.2 µs | 199.1 µs | 8,078 /s |
| point:s | literal | A | 24,000 | 48.4 µs | 54.9 µs | 56.0 µs | 68.8 µs | 75.6 µs | 94.5 µs | 17,857 /s |
| point:s | literal | A' | 24,000 | 51.3 µs | 54.9 µs | 56.0 µs | 68.2 µs | 73.4 µs | 87.9 µs | 17,857 /s |
| point:s | literal | B | 24,000 | 51.3 µs | 55.3 µs | 56.5 µs | 68.4 µs | 73.6 µs | 87.1 µs | 17,699 /s |
| point:s | literal | B-off | 24,000 | 49.2 µs | 55.4 µs | 56.7 µs | 69.2 µs | 75.1 µs | 91.8 µs | 17,637 /s |
| point:s | literal | A-al64 | 24,000 | 48.5 µs | 54.9 µs | 56.1 µs | 68.9 µs | 74.5 µs | 89.5 µs | 17,825 /s |
| point:s | prepared | A | 24,000 | 44.4 µs | 51.6 µs | 52.7 µs | 65.3 µs | 71.8 µs | 94.3 µs | 18,975 /s |
| point:s | prepared | A' | 24,000 | 43.4 µs | 51.7 µs | 52.8 µs | 64.5 µs | 69.6 µs | 84.0 µs | 18,939 /s |
| point:s | prepared | B | 24,000 | 49.2 µs | 52.1 µs | 53.3 µs | 65.7 µs | 71.7 µs | 86.1 µs | 18,762 /s |
| point:s | prepared | B-off | 24,000 | 46.4 µs | 52.2 µs | 53.5 µs | 65.8 µs | 71.8 µs | 87.7 µs | 18,692 /s |
| point:s | prepared | A-al64 | 24,000 | 46.6 µs | 51.5 µs | 52.7 µs | 64.7 µs | 70.0 µs | 85.0 µs | 18,975 /s |
| bn | literal | A | 1,536 | 711.0 µs | 755.3 µs | 769.2 µs | 810.2 µs | 865.6 µs | 1163.5 µs | 1,300 /s |
| bn | literal | A' | 1,536 | 746.3 µs | 790.4 µs | 802.7 µs | 852.5 µs | 894.2 µs | 1106.0 µs | 1,246 /s |
| bn | literal | B | 1,536 | 345.0 µs | 366.2 µs | 378.5 µs | 417.3 µs | 457.2 µs | 544.2 µs | 2,642 /s |
| bn | literal | B-off | 1,536 | 589.9 µs | 621.3 µs | 634.2 µs | 674.9 µs | 719.6 µs | 1063.1 µs | 1,577 /s |
| bn | literal | A-al64 | 1,536 | 696.8 µs | 736.1 µs | 748.5 µs | 793.7 µs | 846.9 µs | 1099.1 µs | 1,336 /s |
| bn | prepared | A | 1,536 | 708.7 µs | 750.8 µs | 763.9 µs | 800.6 µs | 847.1 µs | 948.5 µs | 1,309 /s |
| bn | prepared | A' | 1,536 | 742.2 µs | 786.5 µs | 799.9 µs | 847.9 µs | 888.9 µs | 1078.0 µs | 1,250 /s |
| bn | prepared | B | 1,536 | 341.7 µs | 361.9 µs | 374.5 µs | 412.8 µs | 455.9 µs | 536.2 µs | 2,670 /s |
| bn | prepared | B-off | 1,536 | 584.7 µs | 616.6 µs | 628.9 µs | 662.8 µs | 688.0 µs | 781.4 µs | 1,590 /s |
| bn | prepared | A-al64 | 1,536 | 693.7 µs | 730.7 µs | 743.6 µs | 786.7 µs | 836.9 µs | 1102.2 µs | 1,345 /s |
| point:s100k | literal | A | 24,000 | 52.4 µs | 55.9 µs | 57.1 µs | 69.2 µs | 74.2 µs | 89.8 µs | 17,513 /s |
| point:s100k | literal | A' | 24,000 | 52.4 µs | 55.9 µs | 57.0 µs | 69.6 µs | 75.8 µs | 94.9 µs | 17,544 /s |
| point:s100k | literal | B | 24,000 | 53.0 µs | 56.4 µs | 57.5 µs | 69.1 µs | 73.8 µs | 87.7 µs | 17,391 /s |
| point:s100k | literal | B-off | 24,000 | 51.5 µs | 56.3 µs | 57.6 µs | 70.3 µs | 75.0 µs | 90.2 µs | 17,361 /s |
| point:s100k | literal | A-al64 | 24,000 | 51.7 µs | 55.7 µs | 56.9 µs | 68.5 µs | 73.9 µs | 89.4 µs | 17,575 /s |
| point:s100k | prepared | A | 24,000 | 48.1 µs | 52.5 µs | 53.5 µs | 64.7 µs | 70.2 µs | 89.0 µs | 18,692 /s |
| point:s100k | prepared | A' | 24,000 | 47.2 µs | 52.6 µs | 53.6 µs | 65.7 µs | 71.4 µs | 89.2 µs | 18,657 /s |
| point:s100k | prepared | B | 24,000 | 49.7 µs | 53.0 µs | 54.0 µs | 65.1 µs | 69.6 µs | 82.8 µs | 18,519 /s |
| point:s100k | prepared | B-off | 24,000 | 46.8 µs | 53.0 µs | 54.1 µs | 66.2 µs | 71.2 µs | 87.7 µs | 18,484 /s |
| point:s100k | prepared | A-al64 | 24,000 | 48.3 µs | 52.3 µs | 53.2 µs | 64.1 µs | 69.3 µs | 82.9 µs | 18,797 /s |
| rows-100k | literal | A | 384 | 6770.6 µs | 6911.2 µs | 7014.3 µs | 7347.8 µs | 7488.3 µs | 9952.4 µs | 143 /s |
| rows-100k | literal | A' | 384 | 7137.5 µs | 7273.8 µs | 7344.9 µs | 8194.0 µs | 8258.5 µs | 8560.8 µs | 136 /s |
| rows-100k | literal | B | 384 | 2987.3 µs | 3058.2 µs | 3097.7 µs | 3398.9 µs | 3459.0 µs | 4490.4 µs | 323 /s |
| rows-100k | literal | B-off | 384 | 5513.5 µs | 5603.4 µs | 5659.2 µs | 5942.4 µs | 6079.4 µs | 7582.3 µs | 177 /s |
| rows-100k | literal | A-al64 | 384 | 6654.4 µs | 6738.9 µs | 6789.3 µs | 7128.6 µs | 7310.7 µs | 8340.9 µs | 147 /s |
| rows-100k | prepared | A | 384 | 6781.0 µs | 6920.3 µs | 7014.9 µs | 7333.1 µs | 7429.3 µs | 7887.0 µs | 143 /s |
| rows-100k | prepared | A' | 384 | 7151.6 µs | 7258.6 µs | 7354.2 µs | 8231.1 µs | 8361.5 µs | 8959.1 µs | 136 /s |
| rows-100k | prepared | B | 384 | 2997.4 µs | 3064.3 µs | 3104.3 µs | 3409.6 µs | 3513.4 µs | 4697.6 µs | 322 /s |
| rows-100k | prepared | B-off | 384 | 5517.9 µs | 5597.7 µs | 5654.2 µs | 5898.4 µs | 5953.6 µs | 6368.7 µs | 177 /s |
| rows-100k | prepared | A-al64 | 384 | 6636.3 µs | 6731.4 µs | 6800.2 µs | 7131.3 µs | 7246.6 µs | 7902.9 µs | 147 /s |
| point:srec | literal | A | 24,000 | 48.8 µs | 55.2 µs | 56.4 µs | 70.1 µs | 77.8 µs | 96.5 µs | 17,730 /s |
| point:srec | literal | A' | 24,000 | 48.5 µs | 55.2 µs | 56.4 µs | 69.3 µs | 76.9 µs | 95.9 µs | 17,730 /s |
| point:srec | literal | B | 24,000 | 51.8 µs | 55.5 µs | 56.7 µs | 69.5 µs | 75.5 µs | 91.1 µs | 17,637 /s |
| point:srec | literal | B-off | 24,000 | 50.7 µs | 55.6 µs | 56.7 µs | 69.0 µs | 74.2 µs | 89.1 µs | 17,637 /s |
| point:srec | literal | A-al64 | 24,000 | 48.4 µs | 55.1 µs | 56.1 µs | 67.7 µs | 72.9 µs | 86.7 µs | 17,825 /s |
| point:srec | prepared | A | 24,000 | 43.2 µs | 51.8 µs | 52.8 µs | 66.1 µs | 73.3 µs | 90.8 µs | 18,939 /s |
| point:srec | prepared | A' | 24,000 | 45.9 µs | 51.8 µs | 52.9 µs | 65.6 µs | 71.5 µs | 86.5 µs | 18,904 /s |
| point:srec | prepared | B | 24,000 | 45.9 µs | 52.2 µs | 53.3 µs | 64.8 µs | 70.3 µs | 86.7 µs | 18,762 /s |
| point:srec | prepared | B-off | 24,000 | 47.4 µs | 52.3 µs | 53.4 µs | 65.5 µs | 71.2 µs | 87.4 µs | 18,727 /s |
| point:srec | prepared | A-al64 | 24,000 | 45.0 µs | 51.6 µs | 52.6 µs | 64.6 µs | 69.7 µs | 85.3 µs | 19,011 /s |
| bn-recent | literal | A | 1,536 | 721.5 µs | 759.9 µs | 774.8 µs | 820.4 µs | 876.9 µs | 1163.9 µs | 1,291 /s |
| bn-recent | literal | A' | 1,536 | 748.2 µs | 798.3 µs | 811.2 µs | 865.1 µs | 926.6 µs | 1132.4 µs | 1,233 /s |
| bn-recent | literal | B | 1,536 | 346.1 µs | 368.0 µs | 380.9 µs | 425.3 µs | 463.9 µs | 557.8 µs | 2,625 /s |
| bn-recent | literal | B-off | 1,536 | 589.9 µs | 626.3 µs | 638.4 µs | 680.6 µs | 723.7 µs | 979.0 µs | 1,566 /s |
| bn-recent | literal | A-al64 | 1,536 | 688.6 µs | 738.9 µs | 751.8 µs | 809.7 µs | 875.9 µs | 1245.7 µs | 1,330 /s |
| bn-recent | prepared | A | 1,536 | 714.7 µs | 755.9 µs | 770.8 µs | 818.4 µs | 866.1 µs | 1096.8 µs | 1,297 /s |
| bn-recent | prepared | A' | 1,536 | 746.2 µs | 793.5 µs | 806.6 µs | 845.9 µs | 898.5 µs | 1122.1 µs | 1,240 /s |
| bn-recent | prepared | B | 1,536 | 343.3 µs | 364.7 µs | 376.6 µs | 418.7 µs | 455.6 µs | 594.2 µs | 2,655 /s |
| bn-recent | prepared | B-off | 1,536 | 589.3 µs | 622.1 µs | 633.9 µs | 676.9 µs | 730.2 µs | 1017.7 µs | 1,578 /s |
| bn-recent | prepared | A-al64 | 1,536 | 697.9 µs | 734.3 µs | 746.5 µs | 804.0 µs | 868.5 µs | 1132.4 µs | 1,340 /s |
| sel-0 | literal | A | 1,536 | 703.5 µs | 752.9 µs | 768.1 µs | 840.1 µs | 889.5 µs | 1114.7 µs | 1,302 /s |
| sel-0 | literal | A' | 1,536 | 736.4 µs | 787.5 µs | 800.5 µs | 842.0 µs | 899.3 µs | 1174.1 µs | 1,249 /s |
| sel-0 | literal | B | 1,536 | 339.0 µs | 362.4 µs | 373.2 µs | 413.1 µs | 463.5 µs | 532.1 µs | 2,680 /s |
| sel-0 | literal | B-off | 1,536 | 583.7 µs | 617.9 µs | 629.5 µs | 668.7 µs | 701.2 µs | 949.8 µs | 1,589 /s |
| sel-0 | literal | A-al64 | 1,536 | 693.5 µs | 729.9 µs | 742.4 µs | 785.8 µs | 843.4 µs | 1179.7 µs | 1,347 /s |
| sel-0 | prepared | A | 1,536 | 701.5 µs | 749.0 µs | 765.4 µs | 840.6 µs | 899.3 µs | 1111.3 µs | 1,307 /s |
| sel-0 | prepared | A' | 1,536 | 736.1 µs | 785.0 µs | 797.4 µs | 843.2 µs | 900.0 µs | 1103.0 µs | 1,254 /s |
| sel-0 | prepared | B | 1,536 | 332.8 µs | 358.3 µs | 371.4 µs | 412.3 µs | 460.7 µs | 579.1 µs | 2,693 /s |
| sel-0 | prepared | B-off | 1,536 | 582.6 µs | 614.0 µs | 627.1 µs | 671.3 µs | 743.4 µs | 1011.4 µs | 1,595 /s |
| sel-0 | prepared | A-al64 | 1,536 | 688.8 µs | 727.0 µs | 738.8 µs | 780.6 µs | 837.0 µs | 1086.0 µs | 1,354 /s |
| sel-1 | literal | A | 1,536 | 856.1 µs | 905.1 µs | 921.7 µs | 986.8 µs | 1036.2 µs | 1302.2 µs | 1,085 /s |
| sel-1 | literal | A' | 1,536 | 888.3 µs | 936.1 µs | 949.0 µs | 1015.7 µs | 1069.2 µs | 1297.9 µs | 1,054 /s |
| sel-1 | literal | B | 1,536 | 487.9 µs | 511.1 µs | 527.0 µs | 569.2 µs | 613.1 µs | 722.6 µs | 1,898 /s |
| sel-1 | literal | B-off | 1,536 | 730.8 µs | 767.4 µs | 782.7 µs | 835.4 µs | 931.1 µs | 1159.6 µs | 1,278 /s |
| sel-1 | literal | A-al64 | 1,536 | 844.7 µs | 880.9 µs | 894.5 µs | 952.7 µs | 1004.3 µs | 1276.6 µs | 1,118 /s |
| sel-1 | prepared | A | 1,536 | 850.8 µs | 900.5 µs | 917.8 µs | 998.2 µs | 1090.5 µs | 1366.3 µs | 1,090 /s |
| sel-1 | prepared | A' | 1,536 | 884.7 µs | 931.7 µs | 945.6 µs | 1028.8 µs | 1176.5 µs | 1355.4 µs | 1,058 /s |
| sel-1 | prepared | B | 1,536 | 488.5 µs | 507.2 µs | 523.3 µs | 571.4 µs | 643.2 µs | 756.2 µs | 1,911 /s |
| sel-1 | prepared | B-off | 1,536 | 730.6 µs | 761.5 µs | 778.0 µs | 816.6 µs | 853.3 µs | 1068.4 µs | 1,285 /s |
| sel-1 | prepared | A-al64 | 1,536 | 835.5 µs | 875.3 µs | 889.2 µs | 921.9 µs | 974.0 µs | 1082.1 µs | 1,125 /s |
| sel-10 | literal | A | 1,536 | 2163.6 µs | 2233.0 µs | 2259.8 µs | 2345.9 µs | 2410.8 µs | 2913.0 µs | 443 /s |
| sel-10 | literal | A' | 1,536 | 2171.1 µs | 2260.4 µs | 2283.4 µs | 2385.7 µs | 2556.9 µs | 3031.2 µs | 438 /s |
| sel-10 | literal | B | 1,536 | 1811.7 µs | 1882.3 µs | 1908.4 µs | 1993.6 µs | 2088.1 µs | 5050.9 µs | 524 /s |
| sel-10 | literal | B-off | 1,536 | 2034.0 µs | 2092.0 µs | 2119.2 µs | 2206.0 µs | 2296.1 µs | 2758.3 µs | 472 /s |
| sel-10 | literal | A-al64 | 1,536 | 2147.8 µs | 2212.7 µs | 2236.7 µs | 2327.3 µs | 2438.1 µs | 3121.6 µs | 447 /s |
| sel-10 | prepared | A | 1,536 | 2157.9 µs | 2227.6 µs | 2252.1 µs | 2325.3 µs | 2408.8 µs | 3833.0 µs | 444 /s |
| sel-10 | prepared | A' | 1,536 | 2182.9 µs | 2251.6 µs | 2275.9 µs | 2375.6 µs | 2444.7 µs | 8434.5 µs | 439 /s |
| sel-10 | prepared | B | 1,536 | 1805.0 µs | 1871.3 µs | 1896.2 µs | 1984.2 µs | 2064.5 µs | 4988.9 µs | 527 /s |
| sel-10 | prepared | B-off | 1,536 | 2022.0 µs | 2084.4 µs | 2109.6 µs | 2210.5 µs | 2414.0 µs | 2915.5 µs | 474 /s |
| sel-10 | prepared | A-al64 | 1,536 | 2148.0 µs | 2203.9 µs | 2227.8 µs | 2319.1 µs | 2411.1 µs | 2945.3 µs | 449 /s |
| sel-100 | literal | A | 768 | 14943.0 µs | 15648.8 µs | 15838.0 µs | 19780.0 µs | 30634.7 µs | 39320.5 µs | 63 /s |
| sel-100 | literal | A' | 768 | 14937.5 µs | 15654.2 µs | 15838.1 µs | 20599.0 µs | 30945.2 µs | 38992.8 µs | 63 /s |
| sel-100 | literal | B | 768 | 14949.2 µs | 15625.2 µs | 15805.0 µs | 21682.3 µs | 30094.1 µs | 39363.3 µs | 63 /s |
| sel-100 | literal | B-off | 768 | 14916.4 µs | 15543.3 µs | 15738.8 µs | 21265.3 µs | 30360.7 µs | 39064.9 µs | 64 /s |
| sel-100 | literal | A-al64 | 768 | 14971.6 µs | 15672.0 µs | 15875.4 µs | 21256.4 µs | 30790.4 µs | 39843.0 µs | 63 /s |
| sel-100 | prepared | A | 768 | 14942.3 µs | 15614.0 µs | 15801.8 µs | 19554.4 µs | 29712.3 µs | 39295.3 µs | 63 /s |
| sel-100 | prepared | A' | 768 | 14872.4 µs | 15618.1 µs | 15813.7 µs | 19368.9 µs | 28723.1 µs | 38862.9 µs | 63 /s |
| sel-100 | prepared | B | 768 | 14817.8 µs | 15592.0 µs | 15795.1 µs | 20229.7 µs | 30362.1 µs | 39368.6 µs | 63 /s |
| sel-100 | prepared | B-off | 768 | 14854.9 µs | 15498.6 µs | 15688.5 µs | 19520.0 µs | 30231.2 µs | 38735.8 µs | 64 /s |
| sel-100 | prepared | A-al64 | 768 | 15024.3 µs | 15638.4 µs | 15822.1 µs | 20450.1 µs | 30165.5 µs | 39513.8 µs | 63 /s |
| out-200 | literal | A | 6,144 | 342.9 µs | 359.3 µs | 365.5 µs | 386.9 µs | 406.2 µs | 538.3 µs | 2,736 /s |
| out-200 | literal | A' | 6,144 | 344.3 µs | 358.9 µs | 365.5 µs | 389.2 µs | 426.0 µs | 555.0 µs | 2,736 /s |
| out-200 | literal | B | 6,144 | 346.0 µs | 359.6 µs | 366.5 µs | 392.3 µs | 418.5 µs | 538.4 µs | 2,729 /s |
| out-200 | literal | B-off | 6,144 | 343.2 µs | 357.1 µs | 363.6 µs | 385.7 µs | 399.2 µs | 501.9 µs | 2,750 /s |
| out-200 | literal | A-al64 | 6,144 | 344.1 µs | 359.3 µs | 366.0 µs | 387.6 µs | 399.1 µs | 469.8 µs | 2,732 /s |
| out-200 | prepared | A | 6,144 | 338.9 µs | 354.6 µs | 361.1 µs | 385.7 µs | 419.2 µs | 576.5 µs | 2,769 /s |
| out-200 | prepared | A' | 6,144 | 339.1 µs | 354.0 µs | 360.5 µs | 383.4 µs | 418.4 µs | 522.2 µs | 2,774 /s |
| out-200 | prepared | B | 6,144 | 341.5 µs | 355.4 µs | 362.3 µs | 389.3 µs | 426.8 µs | 521.0 µs | 2,760 /s |
| out-200 | prepared | B-off | 6,144 | 339.4 µs | 352.7 µs | 358.9 µs | 379.7 µs | 393.4 µs | 478.4 µs | 2,786 /s |
| out-200 | prepared | A-al64 | 6,144 | 339.9 µs | 354.6 µs | 361.1 µs | 382.7 µs | 397.3 µs | 468.1 µs | 2,769 /s |
| out-1k | literal | A | 3,072 | 1556.8 µs | 1597.8 µs | 1615.2 µs | 1690.4 µs | 1772.9 µs | 2326.6 µs | 619 /s |
| out-1k | literal | A' | 3,072 | 1539.2 µs | 1595.4 µs | 1612.3 µs | 1679.7 µs | 1754.3 µs | 2284.2 µs | 620 /s |
| out-1k | literal | B | 3,072 | 1552.2 µs | 1593.1 µs | 1611.0 µs | 1682.9 µs | 1741.4 µs | 2548.8 µs | 621 /s |
| out-1k | literal | B-off | 3,072 | 1533.5 µs | 1584.3 µs | 1601.1 µs | 1663.3 µs | 1718.2 µs | 2399.1 µs | 625 /s |
| out-1k | literal | A-al64 | 3,072 | 1551.3 µs | 1599.6 µs | 1617.1 µs | 1674.4 µs | 1732.9 µs | 2216.9 µs | 618 /s |
| out-1k | prepared | A | 3,072 | 1536.1 µs | 1587.6 µs | 1605.1 µs | 1674.1 µs | 1749.7 µs | 2339.2 µs | 623 /s |
| out-1k | prepared | A' | 3,072 | 1544.4 µs | 1588.4 µs | 1605.3 µs | 1664.6 µs | 1722.3 µs | 2212.4 µs | 623 /s |
| out-1k | prepared | B | 3,072 | 1538.7 µs | 1584.9 µs | 1602.5 µs | 1665.7 µs | 1736.8 µs | 2472.3 µs | 624 /s |
| out-1k | prepared | B-off | 3,072 | 1525.0 µs | 1576.0 µs | 1592.5 µs | 1655.1 µs | 1706.1 µs | 2340.8 µs | 628 /s |
| out-1k | prepared | A-al64 | 3,072 | 1543.9 µs | 1589.7 µs | 1607.1 µs | 1675.0 µs | 1759.9 µs | 2600.9 µs | 622 /s |
| point:wd | literal | A | 24,000 | 71.0 µs | 75.5 µs | 77.3 µs | 91.2 µs | 98.2 µs | 118.8 µs | 12,937 /s |
| point:wd | literal | A' | 24,000 | 71.3 µs | 75.4 µs | 77.0 µs | 89.7 µs | 96.4 µs | 114.8 µs | 12,987 /s |
| point:wd | literal | B | 24,000 | 70.6 µs | 76.1 µs | 77.8 µs | 92.1 µs | 99.6 µs | 122.2 µs | 12,853 /s |
| point:wd | literal | B-off | 24,000 | 72.1 µs | 76.4 µs | 78.1 µs | 91.4 µs | 97.7 µs | 115.2 µs | 12,804 /s |
| point:wd | literal | A-al64 | 24,000 | 71.1 µs | 75.7 µs | 77.3 µs | 91.5 µs | 98.4 µs | 118.8 µs | 12,937 /s |
| point:wd | prepared | A | 24,000 | 68.3 µs | 71.8 µs | 73.4 µs | 87.2 µs | 93.8 µs | 114.3 µs | 13,624 /s |
| point:wd | prepared | A' | 24,000 | 63.2 µs | 71.8 µs | 73.3 µs | 86.2 µs | 92.3 µs | 111.1 µs | 13,643 /s |
| point:wd | prepared | B | 24,000 | 67.8 µs | 72.5 µs | 74.1 µs | 88.0 µs | 94.9 µs | 119.4 µs | 13,495 /s |
| point:wd | prepared | B-off | 24,000 | 68.5 µs | 72.7 µs | 74.3 µs | 88.2 µs | 94.4 µs | 113.2 µs | 13,459 /s |
| point:wd | prepared | A-al64 | 24,000 | 68.5 µs | 71.8 µs | 73.4 µs | 86.8 µs | 92.9 µs | 109.0 µs | 13,624 /s |
| wide-h | literal | A | 1,536 | 932.8 µs | 983.5 µs | 995.6 µs | 1097.1 µs | 1370.9 µs | 1897.8 µs | 1,004 /s |
| wide-h | literal | A' | 1,536 | 951.4 µs | 1003.6 µs | 1016.1 µs | 1112.7 µs | 1516.6 µs | 1981.7 µs | 984 /s |
| wide-h | literal | B | 1,536 | 395.7 µs | 418.8 µs | 432.4 µs | 479.1 µs | 593.6 µs | 969.0 µs | 2,313 /s |
| wide-h | literal | B-off | 1,536 | 801.6 µs | 860.8 µs | 876.6 µs | 1095.4 µs | 1140.8 µs | 1581.4 µs | 1,141 /s |
| wide-h | literal | A-al64 | 1,536 | 905.7 µs | 960.6 µs | 973.1 µs | 1048.3 µs | 1348.8 µs | 1856.4 µs | 1,028 /s |
| wide-h | prepared | A | 1,536 | 925.8 µs | 979.7 µs | 990.9 µs | 1064.9 µs | 1319.0 µs | 1872.9 µs | 1,009 /s |
| wide-h | prepared | A' | 1,536 | 949.1 µs | 999.4 µs | 1012.4 µs | 1102.8 µs | 1269.5 µs | 1921.3 µs | 988 /s |
| wide-h | prepared | B | 1,536 | 391.9 µs | 415.1 µs | 429.2 µs | 475.4 µs | 575.0 µs | 963.1 µs | 2,330 /s |
| wide-h | prepared | B-off | 1,536 | 803.9 µs | 854.7 µs | 869.9 µs | 1093.3 µs | 1124.0 µs | 1579.0 µs | 1,150 /s |
| wide-h | prepared | A-al64 | 1,536 | 902.2 µs | 957.9 µs | 970.1 µs | 1031.4 µs | 1183.3 µs | 1842.5 µs | 1,031 /s |
| wide-h-k | literal | A | 1,536 | 1050.4 µs | 1107.0 µs | 1123.5 µs | 1233.4 µs | 1527.7 µs | 2019.0 µs | 890 /s |
| wide-h-k | literal | A' | 1,536 | 1108.3 µs | 1159.9 µs | 1176.7 µs | 1255.3 µs | 1498.0 µs | 2084.2 µs | 850 /s |
| wide-h-k | literal | B | 1,536 | 397.9 µs | 422.4 µs | 437.9 µs | 484.5 µs | 584.0 µs | 974.0 µs | 2,284 /s |
| wide-h-k | literal | B-off | 1,536 | 925.3 µs | 975.6 µs | 989.2 µs | 1220.0 µs | 1295.2 µs | 1715.7 µs | 1,011 /s |
| wide-h-k | literal | A-al64 | 1,536 | 1023.9 µs | 1078.7 µs | 1094.1 µs | 1143.9 µs | 1243.5 µs | 1969.1 µs | 914 /s |
| wide-h-k | prepared | A | 1,536 | 1048.8 µs | 1102.2 µs | 1118.6 µs | 1219.7 µs | 1554.4 µs | 2037.0 µs | 894 /s |
| wide-h-k | prepared | A' | 1,536 | 1093.7 µs | 1156.1 µs | 1172.2 µs | 1265.2 µs | 1556.4 µs | 2072.1 µs | 853 /s |
| wide-h-k | prepared | B | 1,536 | 393.7 µs | 418.8 µs | 432.7 µs | 472.6 µs | 545.8 µs | 974.4 µs | 2,311 /s |
| wide-h-k | prepared | B-off | 1,536 | 922.3 µs | 971.8 µs | 985.3 µs | 1214.3 µs | 1248.1 µs | 1686.2 µs | 1,015 /s |
| wide-h-k | prepared | A-al64 | 1,536 | 1022.4 µs | 1074.8 µs | 1089.2 µs | 1150.9 µs | 1358.8 µs | 2004.0 µs | 918 /s |
| wide-a | literal | A | 1,536 | 909.4 µs | 963.2 µs | 976.7 µs | 1028.3 µs | 1159.9 µs | 1865.1 µs | 1,024 /s |
| wide-a | literal | A' | 1,536 | 932.8 µs | 981.5 µs | 993.1 µs | 1058.0 µs | 1163.6 µs | 1878.8 µs | 1,007 /s |
| wide-a | literal | B | 1,536 | 423.1 µs | 445.9 µs | 461.5 µs | 503.3 µs | 581.2 µs | 1236.9 µs | 2,167 /s |
| wide-a | literal | B-off | 1,536 | 784.0 µs | 838.0 µs | 855.4 µs | 1059.5 µs | 1090.0 µs | 1580.7 µs | 1,169 /s |
| wide-a | literal | A-al64 | 1,536 | 887.4 µs | 940.5 µs | 955.1 µs | 1018.2 µs | 1134.9 µs | 1868.9 µs | 1,047 /s |
| wide-a | prepared | A | 1,536 | 903.4 µs | 959.9 µs | 974.2 µs | 1028.1 µs | 1127.9 µs | 1869.3 µs | 1,026 /s |
| wide-a | prepared | A' | 1,536 | 921.2 µs | 977.5 µs | 991.4 µs | 1060.2 µs | 1249.7 µs | 1857.3 µs | 1,009 /s |
| wide-a | prepared | B | 1,536 | 418.0 µs | 442.7 µs | 458.9 µs | 501.8 µs | 633.6 µs | 1222.3 µs | 2,179 /s |
| wide-a | prepared | B-off | 1,536 | 785.9 µs | 833.6 µs | 851.0 µs | 1055.5 µs | 1090.1 µs | 1606.1 µs | 1,175 /s |
| wide-a | prepared | A-al64 | 1,536 | 885.8 µs | 936.7 µs | 950.8 µs | 1017.1 µs | 1148.2 µs | 1835.0 µs | 1,052 /s |
| wide-c | literal | A | 1,536 | 1010.9 µs | 1058.5 µs | 1073.3 µs | 1161.2 µs | 1346.9 µs | 2004.6 µs | 932 /s |
| wide-c | literal | A' | 1,536 | 1017.6 µs | 1061.5 µs | 1077.7 µs | 1179.0 µs | 1443.7 µs | 1991.8 µs | 928 /s |
| wide-c | literal | B | 1,536 | 403.5 µs | 425.9 µs | 443.0 µs | 500.8 µs | 667.0 µs | 1228.8 µs | 2,257 /s |
| wide-c | literal | B-off | 1,536 | 863.8 µs | 916.7 µs | 929.7 µs | 1051.4 µs | 1187.1 µs | 1682.4 µs | 1,076 /s |
| wide-c | literal | A-al64 | 1,536 | 949.0 µs | 1008.2 µs | 1020.6 µs | 1109.8 µs | 1283.6 µs | 1941.3 µs | 980 /s |
| wide-c | prepared | A | 1,536 | 1003.9 µs | 1054.6 µs | 1070.5 µs | 1155.4 µs | 1481.7 µs | 1977.1 µs | 934 /s |
| wide-c | prepared | A' | 1,536 | 1004.0 µs | 1057.4 µs | 1072.7 µs | 1161.2 µs | 1472.8 µs | 2021.6 µs | 932 /s |
| wide-c | prepared | B | 1,536 | 398.3 µs | 421.2 µs | 437.8 µs | 496.3 µs | 612.5 µs | 1220.2 µs | 2,284 /s |
| wide-c | prepared | B-off | 1,536 | 858.7 µs | 912.0 µs | 923.7 µs | 1031.4 µs | 1193.7 µs | 1699.2 µs | 1,083 /s |
| wide-c | prepared | A-al64 | 1,536 | 942.8 µs | 1001.3 µs | 1014.8 µs | 1109.0 µs | 1294.6 µs | 1880.1 µs | 985 /s |
| wide-d | literal | A | 1,536 | 897.2 µs | 949.4 µs | 960.2 µs | 1010.8 µs | 1110.2 µs | 1847.1 µs | 1,041 /s |
| wide-d | literal | A' | 1,536 | 893.3 µs | 953.1 µs | 965.8 µs | 1034.1 µs | 1184.1 µs | 1849.7 µs | 1,035 /s |
| wide-d | literal | B | 1,536 | 396.1 µs | 422.2 µs | 436.6 µs | 476.2 µs | 538.2 µs | 1161.2 µs | 2,290 /s |
| wide-d | literal | B-off | 1,536 | 745.2 µs | 802.3 µs | 817.4 µs | 878.3 µs | 1009.2 µs | 1557.6 µs | 1,223 /s |
| wide-d | literal | A-al64 | 1,536 | 869.9 µs | 922.6 µs | 934.9 µs | 985.4 µs | 1052.8 µs | 1800.1 µs | 1,070 /s |
| wide-d | prepared | A | 1,536 | 892.4 µs | 945.2 µs | 957.2 µs | 1007.1 µs | 1150.2 µs | 1816.2 µs | 1,045 /s |
| wide-d | prepared | A' | 1,536 | 897.6 µs | 946.7 µs | 959.8 µs | 1009.1 µs | 1111.5 µs | 1851.8 µs | 1,042 /s |
| wide-d | prepared | B | 1,536 | 394.1 µs | 418.0 µs | 433.3 µs | 473.3 µs | 560.2 µs | 1154.8 µs | 2,308 /s |
| wide-d | prepared | B-off | 1,536 | 748.1 µs | 796.0 µs | 811.1 µs | 859.2 µs | 932.8 µs | 1547.7 µs | 1,233 /s |
| wide-d | prepared | A-al64 | 1,536 | 856.1 µs | 917.5 µs | 929.3 µs | 978.4 µs | 1056.5 µs | 1817.1 µs | 1,076 /s |
| wide-e | literal | A | 1,536 | 901.2 µs | 958.4 µs | 971.7 µs | 1074.3 µs | 1299.6 µs | 1917.8 µs | 1,029 /s |
| wide-e | literal | A' | 1,536 | 904.8 µs | 959.9 µs | 971.3 µs | 1044.6 µs | 1149.6 µs | 1868.9 µs | 1,030 /s |
| wide-e | literal | B | 1,536 | 418.8 µs | 439.1 µs | 455.1 µs | 499.1 µs | 599.1 µs | 1155.4 µs | 2,197 /s |
| wide-e | literal | B-off | 1,536 | 763.0 µs | 810.0 µs | 825.2 µs | 882.5 µs | 953.0 µs | 1563.7 µs | 1,212 /s |
| wide-e | literal | A-al64 | 1,536 | 876.0 µs | 929.9 µs | 942.4 µs | 1001.0 µs | 1129.1 µs | 1810.4 µs | 1,061 /s |
| wide-e | prepared | A | 1,536 | 901.8 µs | 955.7 µs | 969.0 µs | 1056.4 µs | 1267.4 µs | 1911.4 µs | 1,032 /s |
| wide-e | prepared | A' | 1,536 | 903.6 µs | 955.3 µs | 966.5 µs | 1034.6 µs | 1158.2 µs | 1939.3 µs | 1,035 /s |
| wide-e | prepared | B | 1,536 | 415.9 µs | 436.1 µs | 452.1 µs | 496.7 µs | 606.5 µs | 1147.3 µs | 2,212 /s |
| wide-e | prepared | B-off | 1,536 | 754.3 µs | 805.2 µs | 821.5 µs | 880.9 µs | 987.0 µs | 1551.6 µs | 1,217 /s |
| wide-e | prepared | A-al64 | 1,536 | 877.7 µs | 926.6 µs | 939.6 µs | 1007.6 µs | 1244.0 µs | 1903.0 µs | 1,064 /s |
| wide-f | literal | A | 1,536 | 929.6 µs | 978.2 µs | 990.1 µs | 1044.6 µs | 1279.6 µs | 1876.5 µs | 1,010 /s |
| wide-f | literal | A' | 1,536 | 927.7 µs | 979.8 µs | 992.1 µs | 1057.7 µs | 1165.8 µs | 1861.8 µs | 1,008 /s |
| wide-f | literal | B | 1,536 | 433.1 µs | 457.5 µs | 472.7 µs | 518.4 µs | 576.8 µs | 1050.8 µs | 2,116 /s |
| wide-f | literal | B-off | 1,536 | 778.4 µs | 823.8 µs | 839.4 µs | 889.5 µs | 1031.2 µs | 1568.4 µs | 1,191 /s |
| wide-f | literal | A-al64 | 1,536 | 893.6 µs | 941.8 µs | 953.5 µs | 1010.4 µs | 1178.5 µs | 1846.1 µs | 1,049 /s |
| wide-f | prepared | A | 1,536 | 920.5 µs | 975.5 µs | 986.0 µs | 1046.9 µs | 1244.3 µs | 1859.2 µs | 1,014 /s |
| wide-f | prepared | A' | 1,536 | 922.0 µs | 975.6 µs | 989.1 µs | 1072.9 µs | 1319.6 µs | 1878.8 µs | 1,011 /s |
| wide-f | prepared | B | 1,536 | 431.3 µs | 452.0 µs | 467.8 µs | 505.6 µs | 552.7 µs | 1051.3 µs | 2,138 /s |
| wide-f | prepared | B-off | 1,536 | 770.3 µs | 819.7 µs | 834.1 µs | 876.7 µs | 943.9 µs | 1557.8 µs | 1,199 /s |
| wide-f | prepared | A-al64 | 1,536 | 889.3 µs | 937.5 µs | 948.2 µs | 998.1 µs | 1132.1 µs | 1822.1 µs | 1,055 /s |
| wide-g | literal | A | 1,536 | 917.1 µs | 973.3 µs | 985.9 µs | 1093.2 µs | 1499.6 µs | 1980.3 µs | 1,014 /s |
| wide-g | literal | A' | 1,536 | 930.0 µs | 984.8 µs | 1000.0 µs | 1121.2 µs | 1469.8 µs | 1873.2 µs | 1,000 /s |
| wide-g | literal | B | 1,536 | 432.4 µs | 455.7 µs | 471.9 µs | 532.5 µs | 708.1 µs | 985.9 µs | 2,119 /s |
| wide-g | literal | B-off | 1,536 | 771.2 µs | 828.5 µs | 844.0 µs | 907.1 µs | 1057.4 µs | 1582.8 µs | 1,185 /s |
| wide-g | literal | A-al64 | 1,536 | 892.2 µs | 940.2 µs | 950.9 µs | 1018.4 µs | 1184.0 µs | 1866.8 µs | 1,052 /s |
| wide-g | prepared | A | 1,536 | 911.8 µs | 969.9 µs | 983.2 µs | 1136.6 µs | 1450.5 µs | 1873.4 µs | 1,017 /s |
| wide-g | prepared | A' | 1,536 | 922.8 µs | 981.6 µs | 996.0 µs | 1125.2 µs | 1515.0 µs | 1882.5 µs | 1,004 /s |
| wide-g | prepared | B | 1,536 | 429.7 µs | 451.5 µs | 466.4 µs | 524.5 µs | 703.9 µs | 995.6 µs | 2,144 /s |
| wide-g | prepared | B-off | 1,536 | 766.4 µs | 823.7 µs | 838.0 µs | 894.8 µs | 1102.7 µs | 1590.3 µs | 1,193 /s |
| wide-g | prepared | A-al64 | 1,536 | 883.3 µs | 937.4 µs | 949.7 µs | 1062.6 µs | 1340.9 µs | 1874.5 µs | 1,053 /s |
| wide-k | literal | A | 1,536 | 1006.0 µs | 1065.9 µs | 1081.3 µs | 1170.5 µs | 1476.3 µs | 1993.3 µs | 925 /s |
| wide-k | literal | A' | 1,536 | 1006.9 µs | 1061.0 µs | 1084.6 µs | 1245.8 µs | 1336.0 µs | 1975.3 µs | 922 /s |
| wide-k | literal | B | 1,536 | 440.4 µs | 461.8 µs | 478.5 µs | 523.7 µs | 612.2 µs | 982.0 µs | 2,090 /s |
| wide-k | literal | B-off | 1,536 | 850.6 µs | 909.4 µs | 921.4 µs | 978.3 µs | 1245.6 µs | 1676.0 µs | 1,085 /s |
| wide-k | literal | A-al64 | 1,536 | 966.8 µs | 1013.0 µs | 1023.4 µs | 1085.0 µs | 1302.0 µs | 1876.0 µs | 977 /s |
| wide-k | prepared | A | 1,536 | 1015.7 µs | 1062.0 µs | 1075.6 µs | 1135.0 µs | 1338.9 µs | 1992.5 µs | 930 /s |
| wide-k | prepared | A' | 1,536 | 997.8 µs | 1055.8 µs | 1080.1 µs | 1237.3 µs | 1324.9 µs | 1978.8 µs | 926 /s |
| wide-k | prepared | B | 1,536 | 438.9 µs | 459.3 µs | 473.2 µs | 511.6 µs | 645.1 µs | 969.7 µs | 2,113 /s |
| wide-k | prepared | B-off | 1,536 | 852.4 µs | 903.0 µs | 914.8 µs | 955.0 µs | 1028.1 µs | 1649.9 µs | 1,093 /s |
| wide-k | prepared | A-al64 | 1,536 | 960.9 µs | 1008.2 µs | 1019.6 µs | 1078.7 µs | 1269.9 µs | 1886.0 µs | 981 /s |
| wide-m | literal | A | 1,536 | 1136.8 µs | 1166.6 µs | 1186.6 µs | 1250.7 µs | 1332.9 µs | 2074.5 µs | 843 /s |
| wide-m | literal | A' | 1,536 | 1098.3 µs | 1142.8 µs | 1158.9 µs | 1277.7 µs | 1436.2 µs | 2032.4 µs | 863 /s |
| wide-m | literal | B | 1,536 | 487.7 µs | 513.5 µs | 528.9 µs | 562.5 µs | 648.3 µs | 987.5 µs | 1,891 /s |
| wide-m | literal | B-off | 1,536 | 924.9 µs | 981.1 µs | 993.9 µs | 1038.9 µs | 1151.6 µs | 1740.4 µs | 1,006 /s |
| wide-m | literal | A-al64 | 1,536 | 1143.6 µs | 1176.4 µs | 1190.9 µs | 1245.1 µs | 1320.7 µs | 2035.0 µs | 840 /s |
| wide-m | prepared | A | 1,536 | 1111.9 µs | 1162.6 µs | 1182.1 µs | 1247.3 µs | 1314.3 µs | 2075.2 µs | 846 /s |
| wide-m | prepared | A' | 1,536 | 1085.5 µs | 1140.3 µs | 1158.0 µs | 1280.1 µs | 1417.2 µs | 2048.7 µs | 864 /s |
| wide-m | prepared | B | 1,536 | 483.3 µs | 508.5 µs | 526.1 µs | 561.9 µs | 644.2 µs | 990.4 µs | 1,901 /s |
| wide-m | prepared | B-off | 1,536 | 916.0 µs | 977.3 µs | 989.6 µs | 1030.3 µs | 1117.0 µs | 1718.6 µs | 1,011 /s |
| wide-m | prepared | A-al64 | 1,536 | 1142.3 µs | 1171.8 µs | 1186.4 µs | 1239.4 µs | 1420.0 µs | 2007.8 µs | 843 /s |
| point:nul0 | literal | A | 24,000 | 51.9 µs | 55.5 µs | 56.8 µs | 70.7 µs | 77.6 µs | 95.5 µs | 17,606 /s |
| point:nul0 | literal | A' | 24,000 | 49.0 µs | 55.5 µs | 56.8 µs | 69.5 µs | 75.9 µs | 93.2 µs | 17,606 /s |
| point:nul0 | literal | B | 24,000 | 49.5 µs | 55.9 µs | 57.1 µs | 69.6 µs | 75.1 µs | 89.0 µs | 17,513 /s |
| point:nul0 | literal | B-off | 24,000 | 48.0 µs | 56.0 µs | 57.4 µs | 71.4 µs | 78.4 µs | 98.2 µs | 17,422 /s |
| point:nul0 | literal | A-al64 | 24,000 | 52.2 µs | 55.5 µs | 56.6 µs | 69.8 µs | 76.7 µs | 92.9 µs | 17,668 /s |
| point:nul0 | prepared | A | 24,000 | 44.8 µs | 52.1 µs | 53.3 µs | 67.1 µs | 74.3 µs | 97.7 µs | 18,762 /s |
| point:nul0 | prepared | A' | 24,000 | 46.1 µs | 52.2 µs | 53.3 µs | 65.6 µs | 71.9 µs | 87.2 µs | 18,762 /s |
| point:nul0 | prepared | B | 24,000 | 49.1 µs | 52.6 µs | 53.7 µs | 66.6 µs | 72.6 µs | 89.4 µs | 18,622 /s |
| point:nul0 | prepared | B-off | 24,000 | 48.9 µs | 52.7 µs | 53.9 µs | 66.3 µs | 71.8 µs | 84.8 µs | 18,553 /s |
| point:nul0 | prepared | A-al64 | 24,000 | 45.4 µs | 52.1 µs | 53.2 µs | 67.0 µs | 73.9 µs | 92.4 µs | 18,797 /s |
| nul-0 | literal | A | 1,536 | 722.5 µs | 766.1 µs | 780.5 µs | 817.3 µs | 859.9 µs | 1009.6 µs | 1,281 /s |
| nul-0 | literal | A' | 1,536 | 763.6 µs | 805.8 µs | 818.5 µs | 854.2 µs | 907.1 µs | 1140.6 µs | 1,222 /s |
| nul-0 | literal | B | 1,536 | 354.3 µs | 375.7 µs | 387.1 µs | 419.3 µs | 460.0 µs | 521.4 µs | 2,583 /s |
| nul-0 | literal | B-off | 1,536 | 600.8 µs | 636.5 µs | 650.1 µs | 688.2 µs | 742.0 µs | 1054.4 µs | 1,538 /s |
| nul-0 | literal | A-al64 | 1,536 | 707.9 µs | 748.6 µs | 761.7 µs | 813.4 µs | 856.9 µs | 1119.2 µs | 1,313 /s |
| nul-0 | prepared | A | 1,536 | 717.1 µs | 762.0 µs | 775.4 µs | 810.6 µs | 852.9 µs | 1042.3 µs | 1,290 /s |
| nul-0 | prepared | A' | 1,536 | 755.8 µs | 801.4 µs | 813.4 µs | 849.7 µs | 894.0 µs | 1077.3 µs | 1,229 /s |
| nul-0 | prepared | B | 1,536 | 350.9 µs | 371.8 µs | 383.1 µs | 416.9 µs | 455.8 µs | 562.7 µs | 2,610 /s |
| nul-0 | prepared | B-off | 1,536 | 599.0 µs | 633.6 µs | 646.8 µs | 680.5 µs | 712.5 µs | 922.8 µs | 1,546 /s |
| nul-0 | prepared | A-al64 | 1,536 | 703.4 µs | 743.4 µs | 757.2 µs | 799.3 µs | 838.6 µs | 952.9 µs | 1,321 /s |
| point:nul50 | literal | A | 24,000 | 50.7 µs | 55.5 µs | 56.7 µs | 69.1 µs | 75.1 µs | 95.3 µs | 17,637 /s |
| point:nul50 | literal | A' | 24,000 | 48.0 µs | 55.6 µs | 56.8 µs | 69.4 µs | 74.8 µs | 94.1 µs | 17,606 /s |
| point:nul50 | literal | B | 24,000 | 49.4 µs | 56.0 µs | 57.2 µs | 69.7 µs | 75.2 µs | 91.4 µs | 17,483 /s |
| point:nul50 | literal | B-off | 24,000 | 48.9 µs | 56.1 µs | 57.3 µs | 70.1 µs | 75.8 µs | 93.4 µs | 17,452 /s |
| point:nul50 | literal | A-al64 | 24,000 | 48.1 µs | 55.5 µs | 56.7 µs | 69.1 µs | 75.1 µs | 92.0 µs | 17,637 /s |
| point:nul50 | prepared | A | 24,000 | 48.6 µs | 52.2 µs | 53.3 µs | 65.8 µs | 71.9 µs | 87.7 µs | 18,762 /s |
| point:nul50 | prepared | A' | 24,000 | 44.2 µs | 52.2 µs | 53.3 µs | 65.7 µs | 72.6 µs | 95.0 µs | 18,762 /s |
| point:nul50 | prepared | B | 24,000 | 47.6 µs | 52.7 µs | 53.7 µs | 65.5 µs | 71.1 µs | 87.8 µs | 18,622 /s |
| point:nul50 | prepared | B-off | 24,000 | 47.7 µs | 52.7 µs | 53.8 µs | 66.2 µs | 72.5 µs | 89.7 µs | 18,587 /s |
| point:nul50 | prepared | A-al64 | 24,000 | 47.2 µs | 52.1 µs | 53.1 µs | 64.9 µs | 70.4 µs | 84.9 µs | 18,832 /s |
| nul-50 | literal | A | 1,536 | 704.7 µs | 748.7 µs | 766.2 µs | 820.9 µs | 843.3 µs | 1004.9 µs | 1,305 /s |
| nul-50 | literal | A' | 1,536 | 715.4 µs | 758.0 µs | 772.2 µs | 807.6 µs | 853.8 µs | 1006.0 µs | 1,295 /s |
| nul-50 | literal | B | 1,536 | 464.8 µs | 493.4 µs | 506.9 µs | 542.5 µs | 572.8 µs | 668.3 µs | 1,973 /s |
| nul-50 | literal | B-off | 1,536 | 565.2 µs | 599.2 µs | 612.9 µs | 646.8 µs | 670.7 µs | 863.4 µs | 1,632 /s |
| nul-50 | literal | A-al64 | 1,536 | 676.5 µs | 715.6 µs | 730.9 µs | 765.5 µs | 797.6 µs | 869.4 µs | 1,368 /s |
| nul-50 | prepared | A | 1,536 | 700.1 µs | 745.6 µs | 762.1 µs | 815.3 µs | 842.1 µs | 1060.8 µs | 1,312 /s |
| nul-50 | prepared | A' | 1,536 | 708.6 µs | 754.8 µs | 769.3 µs | 811.6 µs | 868.5 µs | 1078.4 µs | 1,300 /s |
| nul-50 | prepared | B | 1,536 | 459.2 µs | 487.6 µs | 503.0 µs | 539.5 µs | 572.7 µs | 703.1 µs | 1,988 /s |
| nul-50 | prepared | B-off | 1,536 | 563.7 µs | 596.5 µs | 609.5 µs | 641.7 µs | 668.1 µs | 896.9 µs | 1,641 /s |
| nul-50 | prepared | A-al64 | 1,536 | 666.8 µs | 711.3 µs | 726.3 µs | 766.1 µs | 804.3 µs | 977.2 µs | 1,377 /s |

### A.2 `tools/bn_census.py`, 1,000 rows

| rows | shape | path | arm | n | p0 | p25 | p50 | p90 | p95 | p99 | statements/s at p50 (derived) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1000 | select-pk | literal | A | 24,000 | 52.7 µs | 54.9 µs | 56.8 µs | 68.1 µs | - | 89.4 µs | 17,615 /s |
| 1000 | select-pk | literal | A' | 24,000 | 52.6 µs | 55.1 µs | 57.0 µs | 68.8 µs | - | 89.4 µs | 17,539 /s |
| 1000 | select-pk | literal | B | 24,000 | 53.0 µs | 55.4 µs | 57.3 µs | 68.6 µs | - | 87.1 µs | 17,441 /s |
| 1000 | select-pk | literal | B-off | 24,000 | 52.4 µs | 55.1 µs | 57.1 µs | 68.4 µs | - | 88.3 µs | 17,511 /s |
| 1000 | select-pk | prepared | A | 24,000 | 49.6 µs | 51.9 µs | 53.5 µs | 65.4 µs | - | 83.5 µs | 18,676 /s |
| 1000 | select-pk | prepared | A' | 24,000 | 49.4 µs | 52.2 µs | 53.8 µs | 64.7 µs | - | 84.1 µs | 18,599 /s |
| 1000 | select-pk | prepared | B | 24,000 | 49.7 µs | 52.4 µs | 54.2 µs | 65.2 µs | - | 85.4 µs | 18,438 /s |
| 1000 | select-pk | prepared | B-off | 24,000 | 49.8 µs | 52.2 µs | 53.9 µs | 65.4 µs | - | 84.6 µs | 18,549 /s |
| 1000 | update-lit | literal | A | 24,000 | 37.7 µs | 45.0 µs | 45.6 µs | 56.3 µs | - | 80.2 µs | 21,906 /s |
| 1000 | update-lit | literal | A' | 24,000 | 37.3 µs | 45.0 µs | 45.6 µs | 56.3 µs | - | 80.0 µs | 21,911 /s |
| 1000 | update-lit | literal | B | 24,000 | 38.5 µs | 45.1 µs | 45.8 µs | 55.9 µs | - | 77.2 µs | 21,815 /s |
| 1000 | update-lit | literal | B-off | 24,000 | 38.3 µs | 45.2 µs | 45.8 µs | 56.0 µs | - | 77.7 µs | 21,832 /s |
| 1000 | update-lit | prepared | A | 24,000 | 33.9 µs | 41.9 µs | 42.7 µs | 52.0 µs | - | 74.1 µs | 23,425 /s |
| 1000 | update-lit | prepared | A' | 24,000 | 34.8 µs | 41.8 µs | 42.6 µs | 52.8 µs | - | 77.2 µs | 23,472 /s |
| 1000 | update-lit | prepared | B | 24,000 | 34.6 µs | 42.1 µs | 42.7 µs | 52.8 µs | - | 77.1 µs | 23,433 /s |
| 1000 | update-lit | prepared | B-off | 24,000 | 35.1 µs | 42.2 µs | 42.8 µs | 53.1 µs | - | 74.1 µs | 23,359 /s |
| 1000 | update-range | literal | A | 1,920 | 692.6 µs | 728.6 µs | 742.1 µs | 860.4 µs | - | 1353.4 µs | 1,348 /s |
| 1000 | update-range | literal | A' | 1,920 | 689.5 µs | 724.4 µs | 739.0 µs | 849.4 µs | - | 1172.2 µs | 1,353 /s |
| 1000 | update-range | literal | B | 1,920 | 724.7 µs | 764.5 µs | 779.9 µs | 880.6 µs | - | 1351.1 µs | 1,282 /s |
| 1000 | update-range | literal | B-off | 1,920 | 724.9 µs | 764.3 µs | 778.8 µs | 877.9 µs | - | 1291.3 µs | 1,284 /s |
| 1000 | update-range | prepared | A | 1,920 | 686.1 µs | 723.6 µs | 737.9 µs | 853.7 µs | - | 1554.4 µs | 1,355 /s |
| 1000 | update-range | prepared | A' | 1,920 | 688.0 µs | 720.0 µs | 734.9 µs | 847.3 µs | - | 1249.8 µs | 1,361 /s |
| 1000 | update-range | prepared | B | 1,920 | 722.4 µs | 759.8 µs | 772.7 µs | 870.6 µs | - | 1625.7 µs | 1,294 /s |
| 1000 | update-range | prepared | B-off | 1,920 | 721.7 µs | 759.4 µs | 773.8 µs | 883.8 µs | - | 1253.4 µs | 1,292 /s |
| 1000 | scan | literal | A | 6,336 | 119.9 µs | 123.4 µs | 127.0 µs | 146.7 µs | - | 189.3 µs | 7,873 /s |
| 1000 | scan | literal | A' | 6,336 | 123.1 µs | 126.7 µs | 129.7 µs | 147.1 µs | - | 190.6 µs | 7,707 /s |
| 1000 | scan | literal | B | 6,336 | 83.0 µs | 85.1 µs | 87.1 µs | 103.4 µs | - | 147.7 µs | 11,477 /s |
| 1000 | scan | literal | B-off | 6,336 | 107.8 µs | 110.6 µs | 113.1 µs | 131.8 µs | - | 169.8 µs | 8,839 /s |
| 1000 | scan | prepared | A | 6,336 | 116.5 µs | 120.6 µs | 125.2 µs | 142.9 µs | - | 182.6 µs | 7,987 /s |
| 1000 | scan | prepared | A' | 6,336 | 119.9 µs | 123.9 µs | 127.9 µs | 147.6 µs | - | 188.7 µs | 7,818 /s |
| 1000 | scan | prepared | B | 6,336 | 79.9 µs | 82.0 µs | 84.3 µs | 99.0 µs | - | 139.2 µs | 11,856 /s |
| 1000 | scan | prepared | B-off | 6,336 | 104.9 µs | 107.7 µs | 110.6 µs | 128.2 µs | - | 163.6 µs | 9,046 /s |

## Appendix B. Host state, lookups, digests

**Rows and counters.** row digests of one statement per cell, run 0: 23 cells, all arms equal in every cell
ANALYZE of one statement per cell, run 0 (examined=, matched=, sel=, pages=, spills= tokens): equal across arms in every cell

sample ANALYZE (bn, B): SELECT | analyze rows=1 class=JoinSelect steps=1 examined=10000 pages=61 opens=1 pattern_id=0x4f7795b53e6c24fd fetch_id=0x3fb582106ed5b222 | class=JoinSelect steps=1 hoisted=0 | step 0 FilterScan s |   filter 0:0.2 = 5434 | project * (3 column(s) of step 0) | step 0 FilterScan s opens=1 examined=10000 matched=1 sel=0% pages=61

**`pool_misses`** (SHOW META delta around every block, summed): 0 in every census cell but `point:s1k`, where all five arms read the
same 6,672 over the 8 runs (about 834 a run): the Waystone trail pages that 6,000 point statements over 1,000 keys allocate, the bias BM-S1
named for the small relations and did not remove. It is equal across arms, so no delta is affected, and it inflates the `rows-1k` point
cost the net row subtracts (the `rows-1k` net row is therefore uncorrected, as in BM-S1). `bn_census`: not recorded.

| cell | path | A lookups per scanned row (median over runs) | A' lookups per scanned row (median over runs) | B lookups per scanned row (median over runs) | B-off lookups per scanned row (median over runs) | A-al64 lookups per scanned row (median over runs) |
|---|---|---|---|---|---|---|
| bn | literal | no counter in this engine | no counter in this engine | 0.0000 | 0.0000 | no counter in this engine |
| bn-recent | literal | no counter in this engine | no counter in this engine | 0.0500 | 0.0500 | no counter in this engine |
| rows-100k | literal | no counter in this engine | no counter in this engine | 0.0000 | 0.0000 | no counter in this engine |

### B.1 The census

| run | 1-min load at the run gate | gate wait | load before / after the run | arm order | competing builds or test runs recorded in the run's cells |
|---|---|---|---|---|---|
| 0 | 0.93 | 0 s | 0.93 / 1.01 | A A' B B-off A-al64 | 0 |
| 1 | 1.01 | 0 s | 1.01 / 1.16 | A A' B B-off A-al64 | 0 |
| 2 | 1.16 | 0 s | 1.16 / 1.12 | A A' B B-off A-al64 | 0 |
| 3 | 1.12 | 0 s | 1.12 / 2.01 | A A' B B-off A-al64 | 0 |
| 4 | 1.85 | 5 s | 1.85 / 0.71 | A A' B B-off A-al64 | 0 |
| 5 | 0.71 | 0 s | 0.71 / 1.16 | A A' B B-off A-al64 | 0 |
| 6 | 1.16 | 0 s | 1.16 / 1.04 | A A' B B-off A-al64 | 0 |
| 7 | 1.04 | 0 s | 1.04 / 1.21 | A A' B B-off A-al64 | 0 |

| cell | 1-min load at the cell start, min - max over runs | cell-runs above the load gate (2.0) | cell-runs clean (siblings <= 20 % busy; no other session's task above 15 % of the pinned CPUs) | attempts, max | quiet wait before the cell, max | sibling CPUs, busy % median [max] | other sessions' share of the server's / driver's CPU, % max | run-queue wait % of on-CPU time, median [max] (the server's own threads share its CPU) | cell-runs with a competing build or test run |
|---|---|---|---|---|---|---|---|---|---|---|
| point:s200 | 0.68 - 1.52 | 0 of 8 | 8 of 8 | 1 | 2 s | 1.6 [4.6] | 2.2 / 3.0 | 5.5 [6.1] | 0 |
| rows-200 | 0.70 - 1.52 | 0 of 8 | 8 of 8 | 1 | 4 s | 1.6 [8.6] | 3.0 / 3.5 | 5.7 [6.3] | 0 |
| point:s1k | 0.70 - 1.47 | 0 of 8 | 8 of 8 | 1 | 2 s | 4.0 [7.7] | 1.6 / 2.8 | 5.2 [6.6] | 0 |
| rows-1k | 0.89 - 1.58 | 0 of 8 | 8 of 8 | 1 | 2 s | 2.9 [7.1] | 2.6 / 2.3 | 9.7 [11.5] | 0 |
| point:s | 0.82 - 1.53 | 0 of 8 | 8 of 8 | 1 | 2 s | 1.8 [5.4] | 2.7 / 3.5 | 5.0 [5.7] | 0 |
| bn | 0.82 - 1.41 | 0 of 8 | 8 of 8 | 1 | 2 s | 2.2 [4.0] | 1.5 / 3.6 | 9.5 [11.2] | 0 |
| point:s100k | 0.83 - 1.49 | 0 of 8 | 8 of 8 | 1 | 10 s | 2.0 [5.1] | 3.4 / 2.9 | 5.0 [6.8] | 0 |
| rows-100k | 0.77 - 1.62 | 0 of 8 | 8 of 8 | 2 | 8 s | 2.0 [2.5] | 0.4 / 3.1 | 10.2 [10.6] | 0 |
| point:srec | 0.70 - 1.57 | 0 of 8 | 8 of 8 | 1 | 2 s | 1.8 [8.8] | 2.4 / 3.0 | 5.2 [5.8] | 0 |
| bn-recent | 0.65 - 1.62 | 0 of 8 | 8 of 8 | 1 | 2 s | 2.8 [5.6] | 1.4 / 3.9 | 9.4 [11.2] | 0 |
| sel-0 | 0.65 - 1.49 | 0 of 8 | 8 of 8 | 1 | 2 s | 2.7 [5.0] | 1.2 / 3.0 | 9.5 [11.1] | 0 |
| sel-1 | 0.68 - 1.48 | 0 of 8 | 8 of 8 | 1 | 8 s | 2.0 [7.6] | 1.1 / 1.1 | 12.4 [13.1] | 0 |
| sel-10 | 0.62 - 1.48 | 0 of 8 | 8 of 8 | 1 | 2 s | 2.1 [4.5] | 2.0 / 1.0 | 11.6 [12.3] | 0 |
| sel-100 | 0.65 - 1.42 | 0 of 8 | 8 of 8 | 1 | 2 s | 2.1 [5.7] | 2.2 / 0.3 | 23.1 [24.8] | 0 |
| out-200 | 0.83 - 1.85 | 0 of 8 | 8 of 8 | 1 | 2 s | 2.4 [4.2] | 2.4 / 1.1 | 3.3 [4.9] | 0 |
| out-1k | 0.95 - 1.86 | 0 of 8 | 8 of 8 | 1 | 2 s | 2.2 [4.0] | 1.5 / 0.5 | 6.3 [8.2] | 0 |
| point:wd | 0.95 - 1.88 | 0 of 8 | 8 of 8 | 1 | 8 s | 1.8 [4.4] | 2.8 / 2.6 | 4.7 [6.2] | 0 |
| wide-h | 0.96 - 1.89 | 0 of 8 | 8 of 8 | 1 | 2 s | 3.3 [8.8] | 0.9 / 1.9 | 12.1 [14.2] | 0 |
| wide-h-k | 0.96 - 1.98 | 0 of 8 | 8 of 8 | 1 | 2 s | 2.7 [4.9] | 0.9 / 3.9 | 12.2 [13.5] | 0 |
| wide-a | 0.88 - 1.98 | 0 of 8 | 8 of 8 | 1 | 2 s | 1.5 [6.3] | 1.0 / 4.0 | 12.5 [13.9] | 0 |
| wide-c | 0.81 - 1.98 | 0 of 8 | 8 of 8 | 1 | 8 s | 3.4 [18.3] | 1.4 / 2.7 | 13.1 [16.3] | 0 |
| wide-d | 0.81 - 2.06 | 1 of 8 | 8 of 8 | 1 | 6 s | 1.3 [2.6] | 2.0 / 2.1 | 12.2 [14.8] | 0 |
| wide-e | 0.83 - 2.06 | 1 of 8 | 8 of 8 | 1 | 2 s | 2.4 [6.1] | 1.2 / 3.3 | 13.5 [15.3] | 0 |
| wide-f | 0.84 - 2.05 | 1 of 8 | 8 of 8 | 1 | 2 s | 1.9 [4.1] | 1.0 / 3.0 | 11.8 [13.3] | 0 |
| wide-g | 0.77 - 2.05 | 1 of 8 | 8 of 8 | 1 | 2 s | 2.7 [7.2] | 1.2 / 2.9 | 13.0 [13.4] | 0 |
| wide-k | 0.77 - 2.13 | 1 of 8 | 8 of 8 | 1 | 2 s | 1.7 [4.4] | 1.0 / 1.1 | 12.4 [13.0] | 0 |
| wide-m | 0.79 - 2.20 | 1 of 8 | 8 of 8 | 1 | 2 s | 1.4 [3.8] | 0.7 / 2.0 | 11.4 [12.6] | 0 |
| point:nul0 | 0.81 - 2.18 | 1 of 8 | 8 of 8 | 1 | 2 s | 3.5 [5.8] | 3.4 / 3.0 | 5.5 [6.8] | 0 |
| nul-0 | 0.74 - 2.01 | 1 of 8 | 8 of 8 | 1 | 2 s | 2.1 [4.5] | 1.5 / 8.9 | 9.9 [12.7] | 0 |
| point:nul50 | 0.68 - 2.01 | 1 of 8 | 8 of 8 | 1 | 2 s | 2.2 [4.4] | 4.3 / 3.9 | 5.0 [8.6] | 0 |
| nul-50 | 0.68 - 2.01 | 1 of 8 | 8 of 8 | 1 | 2 s | 1.6 [6.3] | 1.9 / 2.4 | 9.9 [11.8] | 0 |

all cell-runs: 248; above the load gate: 10; clean: 248; with a competing build or test run: 0

bad replies: 0
idle CPU per second per run and arm (µs): [{"A": 5266, "A'": 4709, "B": 3901, "B-off": 3915, "A-al64": 3862}, {"A": 4650, "A'": 4826, "B": 4050, "B-off": 4084, "A-al64": 4314}, {"A": 3970, "A'": 5258, "B": 4544, "B-off": 4532, "A-al64": 4067}, {"A": 4149, "A'": 4187, "B": 4604, "B-off": 4612, "A-al64": 4859}, {"A": 4212, "A'": 4080, "B": 3612, "B-off": 4941, "A-al64": 4273}, {"A": 4475, "A'": 3976, "B": 3809, "B-off": 4102, "A-al64": 4510}, {"A": 4221, "A'": 5835, "B": 3741, "B-off": 4025, "A-al64": 5226}, {"A": 3922, "A'": 4081, "B": 5047, "B-off": 4278, "A-al64": 3899}]
SHOW META's own lookups per run and arm: [{"A": 0, "A2": 0, "B": 0, "Boff": 0, "Apad": 0}, {"A": 0, "A2": 0, "B": 0, "Boff": 0, "Apad": 0}, {"A": 0, "A2": 0, "B": 0, "Boff": 0, "Apad": 0}, {"A": 0, "A2": 0, "B": 0, "Boff": 0, "Apad": 0}, {"A": 0, "A2": 0, "B": 0, "Boff": 0, "Apad": 0}, {"A": 0, "A2": 0, "B": 0, "Boff": 0, "Apad": 0}, {"A": 0, "A2": 0, "B": 0, "Boff": 0, "Apad": 0}, {"A": 0, "A2": 0, "B": 0, "Boff": 0, "Apad": 0}]

### B.2 `bn_census`, 1,000 rows

`--rows 1000`

| run | 1-min load at the run gate | gate wait | load before / after the run | arm order | competing builds or test runs recorded in the run's cells |
|---|---|---|---|---|---|
| 0 | 0.76 | 0 s | 0.76 / 1.42 |  | 0 |
| 1 | 1.42 | 0 s | 1.42 / 1.54 |  | 0 |
| 2 | 1.54 | 0 s | 1.54 / 1.16 |  | 0 |
| 3 | 1.16 | 0 s | 1.16 / 1.05 |  | 0 |
| 4 | 1.05 | 0 s | 1.05 / 1.38 |  | 0 |
| 5 | 1.38 | 0 s | 1.38 / 1.76 |  | 0 |
| 6 | 1.76 | 0 s | 1.76 / 1.88 |  | 0 |
| 7 | 1.88 | 0 s | 1.88 / 1.39 |  | 0 |

| cell | 1-min load at the cell start, min - max over runs | cell-runs above the load gate (2.0) | cell-runs clean (siblings <= 20 % busy; no other session's task above 15 % of the pinned CPUs) | attempts, max | quiet wait before the cell, max | sibling CPUs, busy % median [max] | other sessions' share of the server's / driver's CPU, % max | run-queue wait % of on-CPU time, median [max] (the server's own threads share its CPU) | cell-runs with a competing build or test run |
|---|---|---|---|---|---|---|---|---|---|---|
| select-pk/literal | 0.74 - 1.64 | 0 of 8 | 8 of 8 | 1 | 4 s | 1.0 [2.0] | 3.1 / 1.4 | 4.3 [5.8] | 0 |
| select-pk/prepared | 0.68 - 1.61 | 0 of 8 | 8 of 8 | 1 | 2 s | 1.4 [17.2] | 7.3 / 3.9 | 3.9 [12.4] | 0 |
| update-lit/literal | 0.68 - 1.56 | 0 of 8 | 8 of 8 | 1 | 2 s | 1.2 [5.8] | 4.8 / 4.2 | 5.6 [5.8] | 0 |
| update-lit/prepared | 0.62 - 1.51 | 0 of 8 | 8 of 8 | 1 | 2 s | 0.5 [14.2] | 3.4 / 3.2 | 5.6 [6.0] | 0 |
| update-range/literal | 0.62 - 1.91 | 0 of 8 | 8 of 8 | 1 | 2 s | 0.6 [4.0] | 1.4 / 2.4 | 17.2 [33.1] | 0 |
| update-range/prepared | 0.97 - 1.86 | 0 of 8 | 8 of 8 | 1 | 2 s | 1.3 [4.6] | 1.2 / 1.6 | 18.7 [31.0] | 0 |
| scan/literal | 1.05 - 1.86 | 0 of 8 | 8 of 8 | 1 | 2 s | 0.6 [6.3] | 5.9 / 2.8 | 5.5 [10.1] | 0 |
| scan/prepared | 1.05 - 1.91 | 0 of 8 | 8 of 8 | 1 | 2 s | 2.0 [12.1] | 4.1 / 3.2 | 5.6 [7.0] | 0 |

all cell-runs: 64; above the load gate: 0; clean: 64; with a competing build or test run: 0

bad replies: 0
idle CPU per second per run and arm (µs): [{"A": 5108, "A'": 4585, "B": 4041, "B-off": 4328}, {"A": 4062, "A'": 3974, "B": 3999, "B-off": 3750}, {"A": 4702, "A'": 4581, "B": 4138, "B-off": 4130}, {"A": 4084, "A'": 3782, "B": 3781, "B-off": 3806}, {"A": 5110, "A'": 4450, "B": 4203, "B-off": 4143}, {"A": 4675, "A'": 4272, "B": 3771, "B-off": 4087}, {"A": 4450, "A'": 4096, "B": 4241, "B-off": 4177}, {"A": 5461, "A'": 4561, "B": 3675, "B-off": 4626}]

### B.3 The layout-probe cell (`update-range`, 5 runs)

`--rows 1000`

| run | 1-min load at the run gate | gate wait | load before / after the run | arm order | competing builds or test runs recorded in the run's cells |
|---|---|---|---|---|---|
| 0 | 0.57 | 0 s | 0.57 / 1.36 |  | 0 |
| 1 | 1.36 | 0 s | 1.36 / 2.25 |  | 0 |
| 2 | 1.91 | 10 s | 1.91 / 2.58 |  | 0 |
| 3 | 1.98 | 20 s | 1.98 / 1.87 |  | 0 |
| 4 | 1.87 | 0 s | 1.87 / 1.69 |  | 0 |

| cell | 1-min load at the cell start, min - max over runs | cell-runs above the load gate (2.0) | cell-runs clean (siblings <= 20 % busy; no other session's task above 15 % of the pinned CPUs) | attempts, max | quiet wait before the cell, max | sibling CPUs, busy % median [max] | other sessions' share of the server's / driver's CPU, % max | run-queue wait % of on-CPU time, median [max] (the server's own threads share its CPU) | cell-runs with a competing build or test run |
|---|---|---|---|---|---|---|---|---|---|---|
| update-range/literal | 0.52 - 2.25 | 1 of 5 | 5 of 5 | 1 | 2 s | 1.1 [2.5] | 1.6 / 3.4 | 24.6 [26.6] | 0 |
| update-range/prepared | 1.06 - 2.58 | 1 of 5 | 5 of 5 | 1 | 2 s | 0.6 [2.7] | 1.7 / 5.3 | 12.1 [47.6] | 0 |

all cell-runs: 10; above the load gate: 2; clean: 10; with a competing build or test run: 0

bad replies: 0
idle CPU per second per run and arm (µs): [{"A": 4802, "A'": 4253, "B": 4344, "A-al64": 4167}, {"A": 4552, "A'": 4239, "B": 4336, "A-al64": 3782}, {"A": 4557, "A'": 4308, "B": 3951, "A-al64": 3865}, {"A": 4188, "A'": 4422, "B": 4231, "A-al64": 3782}, {"A": 4792, "A'": 4115, "B": 3986, "A-al64": 5105}]

## Appendix C. Reproducing

All from `/home/cdkbs/ckdbs/.claude/worktrees/bm-scan-kernel-open` with the copies named in the stamp. Builds:
`git archive <commit> | tar -x -C <dir>` (A and A' from two directories), then `cmake -DCMAKE_BUILD_TYPE=Release
-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF [-DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer | -falign-functions=64]` and
`cmake --build --target kds_server`. The gate arguments used throughout: `GATE="--cell-quiet-pct 10 --cell-quiet-wait 60
--cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"`.

    python3 tools/bm_scan.py census --arm A=<A> --arm A2=<A copy> --arm B=<B> --arm Boff=<B>,scan_kernel=off --arm Apad=<A-al64> \
        --workdir <dir> --port 32401 --runs 8 --pin-server 2 --pin-driver 4 --quiet-load 2 --quiet-wait 900 $GATE --out census.json
    python3 tools/bn_census.py --arm A=<A> --arm A2=<A copy> --arm B=<B> --arm Boff=<B>,scan_kernel=off --workdir <dir> --port 32501 \
        --runs 8 --pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 3000 --scan-ops 800 --range-ops 240 --blocks 12 \
        --only select-pk,update-lit,update-range,scan --quiet-load 2 --quiet-wait 900 $GATE --out bn.json
    python3 tools/bn_census.py --arm A=<A> --arm A2=<A copy> --arm B=<B> --arm Apad=<A-al64> --workdir <dir> --port 32851 --runs 5 \
        --pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 3000 --scan-ops 200 --range-ops 240 --blocks 12 \
        --only update-range --quiet-load 2 --quiet-wait 900 $GATE --out bnpad.json
    python3 tools/bn_pg_scan.py --shapes bm --port 15434 --runs 3 --scan-ops 192 --blocks 12 --point-ops 3000 --pin-backend 2 \
        --pin-driver 4 $GATE --out pg-b.json          # plus run 0 of the same command, kept as pg-run0.json
    python3 tools/bm_scan.py profile --bin <B frame-pointer build> --sampler kds_sampler.so --sampler-dir <dir> --workdir <dir> \
        --port 32421 --pin-server 2 --pin-driver 4 --profile-cells bn --profile-scans 20000 --point-ops 3000 --out profile.json
    python3 tools/bm_close_table.py census census.json --pg pg-run0.json,pg-b.json --base <BM-S1 census.json.gz> --section net|delta|rate|pg|guard|point|pct|hosts|digest|lookups
    python3 tools/bm_close_table.py bn 1000=bn.json --section guard|pct|hosts
    python3 tools/bm_scan_table.py profile profile.json --section compact

Raw output: `bench/v3.0.0/archive/bm-close-v2.7.0-946-g6b771ec1/` - the census, `bn_census`, probe and PostgreSQL JSON (gzip),
their logs, the profile's JSON, log and fold, `binaries.txt`, `host.txt` (device, PostgreSQL settings, driver hashes), the run
scripts, and `hostwatch.log` (the host's load average and competing-process count every 10 s from 04:51 on). The sampler's raw
dump (3 MB) stays in `/home/cdkbs/bench-runs/bm-s6/prof/dump.0` and is named in `profile.json`; folding it needs the
frame-pointer binary of the sha256 above.
