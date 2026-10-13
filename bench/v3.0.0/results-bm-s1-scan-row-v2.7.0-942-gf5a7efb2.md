# BM-S1: the scan row, priced - 65 ns a row, 92-96 % of it in the five parts BM replaces

**Thesis.** On `worktree-bm-scan-kernel-open` at `f5a7efb2` (`v2.7.0-942-gf5a7efb2`, BM-S1's instrumented commit,
`cores = 1`, `relaxed`, every page resident), an unindexed scan row costs **65.5 ns of server CPU** (literal path;
65.3 ns prepared) net of a point statement on BN's shape - `SELECT * FROM s WHERE w = ?` over 10,000 rows of three
`int64` columns, 681 µs a scan, 15.3 million rows per second. That is **1.7 ns (2.5 %) below BN's close** (67.2 ns literal,
67.0 prepared, `v2.7.0-787-g8679367e`) and **2.24 times PostgreSQL 18.6's 29.3 ns** on a vacuumed table (1.88 times its
34.9 ns on the un-vacuumed table BN's twin measured; BN's close put that ratio at 1.96). The cost is a per-row cost and
nothing else: 66.2, 65.5 and 65.0 ns at 1,000, 10,000 and 100,000 rows, and the literal and prepared paths agree to 0.2 ns,
so neither the statement's fixed cost nor its parse and plan is in the row. A 13-column relation costs 82-102 ns a row
whichever type the filter reads, and PostgreSQL's twin spans 36-71 ns over the same types: where PostgreSQL pays for
`numeric` KDS is 1.27 times it, where it compares integers and dates KDS is 1.9-2.4 times.

- **BM-Q1 passes by a wide margin.** The five parts BM-S2..S4 replace - loop and callback, per-row checks and counters,
  copy, decode, residual - are **96.3 % of the net row on BN's shape, 91.6 % on the wide shape's `h = ?` and 93.2 % on
  `h = ? AND k = ?`** (sampler, frame-pointer build, each against the net row of the same profiled server), against a stop
  gate at one third. The gate falls above, by a factor of 2.7-2.9.
- **BM-Q8 does not open BM-S5 on the cell as specified.** With a point writer on the other core of a two-core `group`
  instance, `LookupCommit` and its latch are **3.8 % of the scan core's CPU** by the sampler (28.9 µs of a scan's 766 µs,
  plus 0.085 µs of latch wait), against a gate at 10 %. The gate falls below. The share reaches 15.6 % when every row of the
  table takes the window path with no writer beside it, and 10.7 % beside an every-row writer; it is the *fraction of rows*
  on the window path that decides it, and in a two-core instance the floor's rule (the minimum over the cores' cursors)
  put every row there in 6 of 45 point-writer segments (section 6).

Each section leads with its finding; the drivers are documented in their own docstrings (`bench/README.md`: `bench/docs/`
stays closed), so this file states findings and links there.

| Item | Value |
|---|---|
| Executed | 2026-10-13, UTC, from the output files and the run scripts' time records. **First round (every table below):** 8-run KDS census 01:53:53-01:56:22; its replicate (an earlier 8-run census of the same binary, used for the noise floor only) 01:40:20-01:42:47; PostgreSQL twin, vacuumed, 01:56:22-01:57:05; PostgreSQL twin, not vacuumed, 02:15:21-02:16:10 (a first not-vacuumed pass, 01:57:05-01:58:47, overlapped another session's `cmake --build` and is archived, not used); writers cell 02:07:32-02:13:31; sampler profile 02:13:31-02:15:01; source-line profile 02:20:12-02:20:51. One control run after the first round (section 1), 03:02:22-03:03:16. A second round is described under "The second round" below |
| Worktree / branch | `bm-scan-kernel-open`, branch `worktree-bm-scan-kernel-open` |
| Commit | `f5a7efb2065c7f67e0f2928913acebf841fe7b59`, committed 2026-10-13 01:22:00 UTC; `git describe --tags f5a7efb2` = `v2.7.0-942-gf5a7efb2` |
| Tree | The worktree was **dirty** while the runs were taken: nine tracked files under `src/` and `include/`, `tests/CMakeLists.txt`, a new `tests/scan_row_walk_test.cpp` and the drivers named below, all edited by the session that launched this measurement (BM-S2 work in progress). **None of it is in a measured number**: every binary was built from `git archive f5a7efb2`, an export of the commit, into the scratchpad, never from the worktree |
| Binary provenance | Plain Release build of the export: source binary mtime 2026-10-13 01:24:13 UTC (2 min 13 s **after** the commit, so it is not older than HEAD), copied before the first cell, **sha256 `ac007610a1222fd66accbd20e9c8772aa2939d3a5955a49db45935fe74d30c8d`**, every server started from the copy and the source binary re-hashed equal at the end. Frame-pointer build (`-DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer`) mtime 01:25:46, sha256 `7d86d0ed7354327ad87a84d4182f7132444e85a49f37e930c45ed2bad1cc2cdb`; the same with `-g` (mtime 02:19:07, sha256 `8c00fee223a50d36734400246e0fe0d217ea14c5930dc21f7cd9753f3c33b07e`) for source lines only; sampler `kds_sampler.so` (`gcc -O2 -shared -fPIC`) sha256 `df1e67bae6a60f47ef05cd916c94938215287195bdc802b04730c414b787379f`. All in `archive/.../binaries.txt` |
| Build | `CMAKE_BUILD_TYPE=Release` (read from each build's `CMakeCache.txt`), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server` |
| Host / device | 8 vCPU AMD EPYC 9V74 (4 cores x 2 threads; siblings 0-1, 2-3, 4-5, 6-7), Linux 7.0.0-1014-azure, 16 GB. **Data files and WAL under `/home/cdkbs/bench-runs/bm-s1/` on `/dev/root`, ext4, not tmpfs** (`df -T`, `archive/.../host.txt`); the binaries' copies are on the same device |
| Pinning | Census and PostgreSQL: server (backend) on CPU 2, driver on CPU 4. Writers cell: reactors pinned by `reactor_cpus = 2,6`, the server's other threads on CPUs 2, 3, 6, 7, the scan driver on CPU 4, the writer session's process on CPU 0 |
| Server config | Census and profile: `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, `waystone_recording`/`waystone_replay` at their defaults (on). Writers cell and its profile: `cores = 2`, `durability = group`, the rest as above. A fresh server and data file per run. Ports 32101, 32111, 32121, 32131, 32141; PostgreSQL 15434 - none the default 15432 |
| PostgreSQL | 18.6, `/home/cdkbs/pg-bench-ba` (the PG18 cluster; `~/pg-bench` is PG16), defaults except `port` and logging; `shared_buffers` 128 MB, `max_parallel_workers_per_gather` 2 (every plan is a plain `Seq Scan`, section 1), `jit` on (no plan is costly enough to use it) |
| Host load | 1-minute load average per run and per cell in Appendix B. **Not idle, and not busy with builds**: 1.4-2.0 during the census, which is the measurement's own two busy tasks plus the 5-minute residue of other sessions; no `cc1plus`, `cmake --build` or `ctest` process in any recorded cell of the census, the vacuumed twin or the writers cell. The not-vacuumed twin that did overlap a build is not used |
| Suite | **Not run in this session.** No engine code changed for this measurement; the commit message of `f5a7efb2` reports the Debug suite at 3,637 of 3,637 (`ctest -j8`), which this file did not re-check |

## The second round

A second round (same binary, same host) was started at 02:26:00 UTC to settle three things the first left open: the
**200-row size** that `bench/README.md`'s sweep rule asks for (the first round swept 1,000 / 10,000 / 100,000 rows); a
`pool_misses` reading per arm and an untimed 400-statement prelude, so that a cold first arm cannot reach a number; and a
**second pass of the writers cell and of the profile** under the same driver revision as the census. **It was not completed -
it was stopped, finally on the operator's word, and not resumed - and no number in this file is taken from it.** Its census
(02:26:00-02:28:34) and both PostgreSQL twins (02:28:34-02:30:19) did finish, on a quiet host; its writers pass overlapped
another session's builds and test runs (the first attempt, 02:44:39-02:48:59, carries a competing build in the host record of
6 of its 16 variant-runs; the retry was killed at about 02:58 with 10 variant-runs written), and its profile pass was never
started. The scripts were stopped by their process ids, not by name. BM-S1 is written from the first round's data, as the
operator ruled. The second round's files are kept under `archive/.../second-round-stopped/` and are not read by any table
here.

## What was run, and what was not

- **Cells** (`tools/bm_scan.py`'s docstring has the list; one relation per row of the table below). Each cell is run on a
  fresh server and data file per run, 8 runs, over two driver paths - **literal** (an unnamed `C_PARSE` of the text per
  execution) and **prepared** (one named `C_PARSE`, then BIND/EXECUTE) - cut into 12 blocks taken alternately by the two
  paths, the first path alternating by block and by run. Per block: the SHOW META counters before and after
  (`contention_visibility_window_lookups`, the window latch wait), the server's on-CPU time (the sum over every server thread of
  `/proc/<pid>/task/*/schedstat`) and the client's wall time per statement.
- **Flags to row counts.** `--scan-ops 192` is the scans per path per run on a 10,000-row cell; `rows-1k` runs 4x that (768),
  `rows-100k` a quarter (48), `sel-100` half (96). `--point-ops 3000` point statements per path per relation. So `bn` has
  8 x 192 = 1,536 statements per path in the pooled tables, and every cell the same count on both paths. The PostgreSQL twin
  runs one path (simple query protocol, a literal per statement) with the KDS per-path count. Relations: `s1k` 1,000 rows,
  `s` 10,000, `s100k` 100,000, `wd` 10,000, `nul0`/`nul50` 10,000, `srec` 10,000 (rows `(i, i, i)` for the three-column ones).
- **The net row** is `(scan CPU - point CPU) / rows scanned`, per run, the point statement being
  `SELECT * FROM <rel> WHERE id = ?` on the same relation and path in the same run. It is a fitted per-row cost, so it is
  reported in nanoseconds with its derived rows per second; every table of statements carries statements per second.
- **Width relation** `wd` (13 columns): `id int64, a int32, b int64, c uint64, d bool, e date, f timestamp, g decimal(12,2),
  h decimal(30,4), k char(16), m varchar, n int64 NULL, p varchar`; `m` is an inline value (9 bytes), `p` a 301-byte value
  that spills, `n` is NULL on no row; `d` is true on exactly one row (a bool equality otherwise matches half the table).
  `h` is the last fixed-width non-string column, so cell (i) is `WHERE h = ?`, (ii) `WHERE h = ? AND k = ?`, (iii) one
  equality per family `a, c, d, e, f, g, h, k, m` (`h` is cell (i) again). Literals: dates and timestamps as quoted strings,
  decimals as quoted spellings, a bool as `1`; every family could be expressed.
- **PostgreSQL type map**: `int32` -> `integer`, `int64` -> `bigint`, `uint64` -> `bigint` (an 8-byte integer compare;
  `numeric(20,0)` would be dearer and was not measured), `bool` -> `boolean`, `date`, `timestamp`, `decimal(12,2)`/`decimal(30,4)`
  -> `numeric(12,2)`/`numeric(30,4)`, `char(16)` -> `char(16)` (blank-padded), `varchar` -> `varchar`. **PostgreSQL keeps the
  301-byte `p` in the heap row** (it moves a value out of line only past about 2 KB), so the twin has no spilled column. Two
  states are reported because they differ by 16 %: **vacuumed** (`VACUUM (ANALYZE)` after the load, as autovacuum would do within
  a minute: pages marked all-visible skip the per-tuple visibility check) and **not vacuumed** (`ANALYZE` only, plus two
  warm-up scans, which is what BN's twin measured).
- **Not run**, and why: the 200-row size as a census cell (it belonged to the second round, which was stopped; the control of
  section 1 gives its net row only); a PostgreSQL twin for `bn-recent` and for the writers cell
  (neither has an analogue, and the brief asked for none); PostgreSQL's prepared path (simple query only, as BN); the census at
  `cores > 1`; `strict` durability in the writers cell (a read-only statement has no commit wait; `strict` would only slow the
  writer, which changes how many rows are recent and was not varied); cold reads (a BM non-goal); heap relations
  (suspended by SUS-1); the sampler on the other census cells; a second profile of any cell (one per cell, sampling error in
  section 5); the suite (see the stamp).
- **Drivers.** Written under `tools/`, uncommitted when run and committed with this file: `bm_scan.py` and `bm_scan_table.py`
  (new), `bn_pg_scan.py` (`--shapes bm`, `--no-vacuum`), `bn_census.py` (`cores`, `durability` and `reactor_cpus` knobs on
  `Server`, defaults unchanged), `bn_parts.json` (the `chain:` parts of `f5a7efb2`, whose copy and per-row-check regexes were
  amended before any fold in this file because a frameless `memcpy` or string destructor shows its caller's caller, so the
  copy part had matched nothing). **The first round was measured with `bm_scan.py` sha256 `6ad769310207...`, `bn_pg_scan.py`
  `2dab29ea...`, `bn_census.py` `9f8822f5...` and `bn_parts.json` `6ed4e590...`; the committed `bm_scan.py` and `bn_pg_scan.py`
  differ from those only by the `rows-200` cell, the `s200` relation, the `pool_misses` field and the prelude of the stopped
  round.** `tools/bm_waystone_control.py` is the control of section 1. Hashes of the committed files: `archive/.../host.txt`.

## 1. The scan row on BN's shape is 65.5 ns, and the row is all there is

The net row is flat from 1,000 to 100,000 rows and equal on the two driver paths, so the cost the kernel work has to cut is
per row; a statement's fixed part (parse, compile, the point-statement cost the net row subtracts) is not in it. On BN's
shape the engine is 1.7 ns below BN's close; PostgreSQL is 2.24 times cheaper per row vacuumed, 1.88 times not vacuumed.

| cell | relation | rows scanned | rows out | KDS net row, literal | KDS net row, prepared | KDS scan rows/s (derived, literal) | lookups per scanned row | PostgreSQL net row, vacuumed | PostgreSQL net row, not vacuumed | KDS literal / PG vacuumed | KDS literal / PG not vacuumed |
|---|---|---|---|---|---|---|---|---|---|---|---|
| rows-1k | s1k | 1,000 | 1 | 66.2 [64.7, 71.0] ns | 66.9 [65.1, 71.9] ns | 15.1 M rows/s | 0.0000 | 25.3 [21.9, 26.0] ns | 31.2 [25.7, 35.7] ns | 2.62 | 2.13 |
| bn | s | 10,000 | 1 | 65.5 [64.4, 70.2] ns | 65.3 [64.6, 69.4] ns | 15.3 M rows/s | 0.0000 | 29.3 [28.6, 32.2] ns | 34.9 [33.8, 43.1] ns | 2.24 | 1.88 |
| rows-100k | s100k | 100,000 | 1 | 65.0 [64.2, 69.8] ns | 65.1 [64.3, 70.4] ns | 15.4 M rows/s | 0.0000 | 29.2 [28.9, 33.0] ns | 34.7 [34.2, 37.0] ns | 2.23 | 1.88 |
| bn-recent | srec | 10,000 | 1 | 66.5 [65.1, 70.1] ns | 65.9 [65.1, 70.4] ns | 15.0 M rows/s | 0.0500 | not measured | not measured | - | - |
| sel-0 | s | 10,000 | 0 | 65.0 [64.6, 70.2] ns | 65.1 [64.6, 69.2] ns | 15.4 M rows/s | 0.0000 | 29.0 [28.5, 29.5] ns | 34.7 [32.7, 36.3] ns | 2.24 | 1.87 |
| sel-1 | s | 10,000 | 100 | 66.8 [66.1, 71.7] ns | 66.8 [66.3, 72.3] ns | 15.0 M rows/s | 0.0000 | 30.4 [30.0, 31.1] ns | 35.2 [34.1, 39.0] ns | 2.19 | 1.90 |
| sel-10 | s | 10,000 | 1000 | 79.1 [77.7, 84.8] ns | 79.5 [78.0, 84.0] ns | 12.6 M rows/s | 0.0000 | 43.6 [43.1, 45.5] ns | 48.5 [46.8, 55.0] ns | 1.81 | 1.63 |
| sel-100 | s | 10,000 | 10000 | 210.5 [201.6, 214.1] ns | 209.0 [200.9, 218.3] ns | 4.8 M rows/s | 0.0000 | 171.3 [169.3, 184.3] ns | 174.0 [169.2, 186.9] ns | 1.23 | 1.21 |
| wide-h | wd | 10,000 | 1 | 87.4 [85.1, 91.7] ns | 87.2 [85.0, 92.2] ns | 11.4 M rows/s | 0.0000 | 68.7 [67.6, 74.1] ns | 72.3 [69.9, 89.4] ns | 1.27 | 1.21 |
| wide-h-k | wd | 10,000 | 1 | 98.0 [95.4, 102.3] ns | 98.1 [94.7, 101.7] ns | 10.2 M rows/s | 0.0000 | 70.6 [69.1, 89.4] ns | 76.6 [72.4, 137.6] ns | 1.39 | 1.28 |
| wide-a | wd | 10,000 | 1 | 84.7 [83.1, 89.7] ns | 85.7 [82.7, 88.2] ns | 11.8 M rows/s | 0.0000 | 36.1 [35.7, 37.4] ns | 41.0 [40.0, 42.3] ns | 2.35 | 2.07 |
| wide-c | wd | 10,000 | 1 | 89.4 [87.6, 92.9] ns | 89.6 [87.3, 93.3] ns | 11.2 M rows/s | 0.0000 | 40.5 [39.4, 41.9] ns | 44.6 [43.3, 47.3] ns | 2.21 | 2.00 |
| wide-d | wd | 10,000 | 1 | 83.4 [81.9, 95.5] ns | 84.0 [81.4, 93.6] ns | 12.0 M rows/s | 0.0000 | 38.8 [37.5, 40.3] ns | 43.5 [42.6, 53.7] ns | 2.15 | 1.91 |
| wide-e | wd | 10,000 | 1 | 82.4 [80.7, 94.8] ns | 82.1 [81.1, 93.3] ns | 12.1 M rows/s | 0.0000 | 42.8 [41.3, 44.3] ns | 47.9 [46.2, 65.6] ns | 1.93 | 1.72 |
| wide-f | wd | 10,000 | 1 | 84.1 [82.7, 91.7] ns | 84.6 [82.6, 90.2] ns | 11.9 M rows/s | 0.0000 | 44.4 [43.1, 55.4] ns | 49.1 [47.2, 60.8] ns | 1.89 | 1.71 |
| wide-g | wd | 10,000 | 1 | 84.2 [82.9, 90.1] ns | 84.3 [82.8, 88.9] ns | 11.9 M rows/s | 0.0000 | 66.1 [65.2, 71.1] ns | 69.1 [67.5, 102.8] ns | 1.27 | 1.22 |
| wide-k | wd | 10,000 | 1 | 91.2 [90.0, 102.1] ns | 90.5 [89.9, 101.3] ns | 11.0 M rows/s | 0.0000 | 63.7 [62.4, 65.9] ns | 66.6 [65.2, 73.0] ns | 1.43 | 1.37 |
| wide-m | wd | 10,000 | 1 | 101.7 [98.7, 107.8] ns | 102.3 [98.1, 109.3] ns | 9.8 M rows/s | 0.0000 | 61.1 [59.6, 62.8] ns | 64.3 [63.1, 71.9] ns | 1.67 | 1.58 |
| nul-0 | nul0 | 10,000 | 1 | 67.1 [66.0, 70.4] ns | 67.9 [66.1, 70.7] ns | 14.9 M rows/s | 0.0000 | 27.1 [26.6, 28.5] ns | 32.2 [27.3, 33.0] ns | 2.47 | 2.09 |
| nul-50 | nul50 | 10,000 | 1 | 64.8 [62.7, 69.1] ns | 64.8 [62.6, 70.4] ns | 15.4 M rows/s | 0.0000 | 28.3 [27.5, 29.6] ns | 33.8 [32.9, 57.3] ns | 2.29 | 1.91 |

- **Against this engine's last number** (`bench/v3.0.0/results-bn-close-v2.7.0-787-g8679367e.md` section 4, the same shape,
  `cores = 1`, `relaxed`, 10,000 rows): 67.2 ns literal and 67.0 ns prepared there, 65.5 and 65.3 here, a fall of 1.7 and
  1.7 ns (-2.5 %); the scan's server CPU 699.8 -> 681.3 µs, the point statement's 28.1 -> 27.7 µs. The replicate census
  of the same binary reads 65.3 and 65.7 (delta -1.9 and -1.3 ns). The two are 155 commits apart (`git log
  8679367e..f5a7efb2 -- src/exec src/storage` lists BI, BP, BR, a NULL-probe fix and merges; none is titled for the scan
  walk); **which of them moved the row, if any did, was not isolated**, and BM-S1's own commit is instrumentation only. The
  host moved too: the same driver's PostgreSQL number on the not-vacuumed table is 34.9 ns here against BN's 34.3.
- **Against the floor**: PostgreSQL 18.6 costs 29.3 ns [28.6, 32.2] a row vacuumed and 34.9 ns [33.8, 43.1] not
  vacuumed, so KDS is 2.24 and 1.88 times it. The vacuumed figure is the harder floor: an all-visible page lets PostgreSQL
  skip the visibility check per tuple, 5.6 ns of its row. KDS's floor path (a row below the visibility floor) pays
  2.2 ns for the same decision (section 5). PostgreSQL is a floor, not a reference (`bench/README.md`).
- **Both paths, one number**: the paired literal-minus-prepared difference of the net row is +0.53 ns [-0.79, +1.59] over the
  8 runs on `bn`, and within +-0.6 ns (median over the runs) on every 10,000-row cell except `sel-100` (+2.6 ns, where
  the result path dominates). The walk is the same code after the parse; the statements/s of the two paths differ by the
  parse and bind only (Appendix A: 1,401 and 1,410 scans/s at p50).
- **PostgreSQL's plan** is a plain `Seq Scan` with the filter in every cell (`EXPLAIN` of run 0, Appendix A), so the
  backend's own CPU is the whole cost: no parallel worker is missed.

**The small relations carry a bias this table does not remove.** A Waystone page is allocated for each argument value that
executes twice (`kds.conf.sample`, "waystone_recording"), and the point statement runs 6,000 times over 1,000 keys on `s1k`,
so its arm allocates a trail page per key, while the 10,000-row relations' 6,000 keys never repeat. The first round's `s1k`
point CPU (26.6 µs [25.4, 29.6]) does not stand out of the other relations' (24.8-27.9 µs) at that spread, so the table's
`rows-1k` figure is **uncorrected**, not shown to be wrong. A control run after the first round, taken with the second round's
driver revision (`tools/bm_waystone_control.py`, 6 runs, `waystone_recording` on and
off on otherwise identical fresh servers, alternating; `archive/.../waystone-control.*`) put the point statements on `s200`,
`s1k` and `s` at 24.5, 26.1 and 24.4 µs with recording on against 24.1, 24.3 and 24.7 µs off; the scans did not move (37.8,
90.8 and 680.4 µs on; 37.8, 91.0 and 677.3 µs off). The net rows it gives, recording off, are **68.3 ns at 200 rows [65.6, 72.4],
66.7 ns at 1,000 [65.4, 69.9] and 65.3 ns at 10,000 [64.9, 69.3]** (on: 66.2, 64.9, 65.6). Read with the 100,000-row 65.0 ns,
the small relations show a fixed part of about 0.6 µs a scan beyond a point statement (a fit of `c + f / rows` gives
c = 65.2 ns, f = 0.6 µs); it is 0.9 % of the 10,000-row scan and does not change the finding. The 200-row size is therefore
a control number here, not a census cell.

## 2. Selectivity: an emitted row costs 141-146 ns, twice the scanned row, and the kernel cannot touch it

The 0 %, 1 % and 10 % cells add the emission of 0, 100 and 1,000 rows to the same scan; the 100 % cell emits all 10,000.
(The `rows out` column of the table above checks each count; the run-0 `ANALYZE` of the four read `examined=10000` and
`matched=` 0, 100, 1,000 and 10,000, and no reply of any cell had the wrong row count: 0 bad replies in 8 runs.)

| cell | rows out | KDS net row, literal | KDS extra over `sel-0` per emitted row | PostgreSQL net row, vacuumed | PostgreSQL extra per emitted row |
|---|---|---|---|---|---|
| `sel-0` | 0 | 65.0 ns | - | 29.0 ns | - |
| `sel-1` | 100 | 66.8 ns | 180 ns (100 rows: the difference is 1.8 ns [noise 1 ns] of a row) | 30.4 ns | 140 ns |
| `sel-10` | 1,000 | 79.1 ns | 141 ns | 43.6 ns | 146 ns |
| `sel-100` | 10,000 | 210.5 ns | 145.5 ns | 171.3 ns | 142.3 ns |

**At 100 % the row is emission-bound**: 145 of its 210 ns are the result path (encode three `int64` columns into a row batch
and the socket), 69 % of the cell. That is the all-pass cost BM-Q2 (a) asks the 100 % cell to price: a pre-filter that only
rejects adds its own comparison on top of 210 ns and removes nothing from this cell, and the guard BM-Q12 sets on it is a
guard of a path the kernel does not shorten. KDS and PostgreSQL emit a row for the same ~142 ns, so at 100 % the ratio is
1.23. The 100 % cell's **wall time is the client's**, not the server's: 15.6 ms at p50 against 2.1 ms of server CPU,
because the Python driver decodes 10,000 rows a reply (Appendix A) - a statement-rate comparison of that cell measures the
driver.

## 3. Width: the 13-column row costs 17-26 ns more than `bn`'s for any single type, an inline `varchar` 36 ns more

Every cell below returns one row; the filter column is the only thing that changes.

| filter | KDS net row, literal | over `bn` | PostgreSQL net row, vacuumed | KDS / PG |
|---|---|---|---|---|
| `e = ?` (date) | 82.4 ns | +16.9 ns | 42.8 ns | 1.93 |
| `d = ?` (bool) | 83.4 ns | +17.9 ns | 38.8 ns | 2.15 |
| `f = ?` (timestamp) | 84.1 ns | +18.6 ns | 44.4 ns | 1.89 |
| `g = ?` (decimal(12,2)) | 84.2 ns | +18.7 ns | 66.1 ns | 1.27 |
| `a = ?` (int32) | 84.7 ns | +19.2 ns | 36.1 ns | 2.35 |
| `h = ?` (decimal(30,4)), cell (i) | 87.4 ns | +21.9 ns | 68.7 ns | 1.27 |
| `c = ?` (uint64) | 89.4 ns | +23.9 ns | 40.5 ns | 2.21 |
| `k = ?` (char(16)) | 91.2 ns | +25.7 ns | 63.7 ns | 1.43 |
| `h = ? AND k = ?`, cell (ii) | 98.0 ns | +32.5 ns | 70.6 ns | 1.39 |
| `m = ?` (inline varchar) | 101.7 ns | +36.2 ns | 61.1 ns | 1.67 |

- **Row width, not type, is most of the difference to `bn`.** The seven integer-encoded families (the date, bool, timestamp,
  both decimals, the int32 and the uint64) sit in a 7 ns band, 82.4-89.4 ns. The extra 17-19 ns of the cheapest of them is
  spread over the walk, not in one part (sampler, `wide-h` against `bn`, ns a row: per-row checks +6.2, copy +4.0, decode
  +2.7, the borrow's lock traffic +2.3, page access +1.5). It follows the row's size: a rejected tuple (about 245 bytes on
  the page here, 50 on `bn`: 304 pages against 61 for 10,000 rows) is copied whole before it is tested, and the per-page and
  per-64-page-slice work lands on fewer rows (point 7 of section 9).
- **The string families are where decode, not compare, is dear**: `k` is +6.5 ns over `a` and `m` +17.0 ns over it - the NUL
  trim and a `std::string` per row for `char(N)`, the tagged cell and a string for an inline `varchar`. BM-Q6's order (integer
  types in BM-S3, strings in BM-S4) puts the smaller removable cost first; the data do not argue against the order, they
  say BM-S4 carries the larger per-row cell.
- **A second conjunct costs 10.6 ns even though it never runs on a rejected row**: `h = ? AND k = ?` is +10.6 ns over `h = ?`
  while the first conjunct rejects 9,999 of 10,000 rows. The filter decodes every column it reads before it evaluates the first
  conjunct (`Step::filter_columns`), so `k` is decoded and trimmed on every row. A kernel that rejects on `h` before touching
  `k` removes this cost; nothing in the residual path does.
- **PostgreSQL pays by type, KDS does not**: PostgreSQL's row is 36-44 ns for integers, dates and timestamps and 61-69 ns
  for `numeric`, `char` and `varchar`, so KDS is 1.9-2.4 times it on the former and 1.27-1.67 times on the latter. KDS's
  spread over the ten cells is 19 ns, PostgreSQL's 34.

## 4. Nullable columns cost nothing extra, and a NULL row is cheaper than a non-NULL one

| cell | filter | NULLs | KDS net row, literal | PostgreSQL net row, vacuumed |
|---|---|---|---|---|
| `bn` (control: all `NOT NULL`) | `w = ?` | none declared | 65.5 ns | 29.3 ns |
| `nul-0` | `n = ?` | 0 % | 67.1 ns | 27.1 ns |
| `nul-50` | `n = ?` | 50 % | 64.8 ns | 28.3 ns |

Declaring the column nullable adds 1.6 ns to a row with no NULL in it (a bitmap bit tested per row; about 1.5 noise floors).
At 50 % NULL the net row falls 2.3 ns, which is **4.6 ns per NULL row**: a NULL operand skips the decode of the value and the
typed compare and goes straight to the three-valued collapse. BM-Q3 asks what passing a NULL row to the residual costs
against rejecting it in the kernel (its option (a) against (b)); this is the residual's own price for a NULL row today,
4.6 ns below a non-NULL one, and the number option (a) keeps paying per NULL row. PostgreSQL is flat (27.1 and 28.3 ns).

## 5. Where the row goes: the five parts BM replaces are 92-96 % of it (BM-Q1)

The sampler (`tools/kds_sampler.c`, 1 kHz of process CPU, the frame-pointer build, 20,000 literal scans per cell) folded into
`tools/bn_parts.json`, in nanoseconds per scanned row. `chain:` parts match the sample's whole chain, leaf first.

| part (`tools/bn_parts.json`, first match) | bn: ns per scanned row (share) | wide-h: ns per scanned row (share) | wide-h-k: ns per scanned row (share) |
|---|---|---|---|
| socket send | 1.1 ns (1.5 %) | 1.2 ns (1.4 %) | 1.4 ns (1.3 %) |
| socket read and wait | 0.6 ns (0.9 %) | 0.8 ns (0.9 %) | 0.8 ns (0.8 %) |
| kwp frames (decode, PARSE scan and fingerprint, BIND substitute, encode) | 0.2 ns (0.3 %) | 0.3 ns (0.4 %) | 0.5 ns (0.4 %) |
| parse | 0.3 ns (0.4 %) | 0.3 ns (0.3 %) | 0.3 ns (0.3 %) |
| compile | 0.2 ns (0.3 %) | 0.3 ns (0.3 %) | 0.4 ns (0.4 %) |
| waystone and access statistics | 0.1 ns (0.1 %) | 0.1 ns (0.1 %) | 0.1 ns (0.1 %) |
| var-heap append | 0.0 ns (0.0 %) | 0.0 ns (0.0 %) | 0.0 ns (0.0 %) |
| undo and WAL records | 0.0 ns (0.0 %) | 0.0 ns (0.0 %) | 0.0 ns (0.0 %) |
| transaction begin, commit, locks | 0.8 ns (1.1 %) | 3.1 ns (3.4 %) | 2.8 ns (2.8 %) |
| result encoding | 0.1 ns (0.1 %) | 0.1 ns (0.1 %) | 0.1 ns (0.1 %) |
| row walk: residual | 13.0 ns (18.0 %) | 12.7 ns (14.1 %) | 13.1 ns (12.7 %) |
| row decode and encode | 19.7 ns (27.3 %) | 22.4 ns (24.9 %) | 32.3 ns (31.4 %) |
| row walk: visibility | 0.0 ns (0.0 %) | 0.0 ns (0.0 %) | 0.0 ns (0.0 %) |
| row walk: copy | 1.2 ns (1.6 %) | 5.2 ns (5.8 %) | 5.8 ns (5.7 %) |
| row walk: per-row checks and counters | 21.0 ns (29.0 %) | 27.2 ns (30.2 %) | 28.0 ns (27.2 %) |
| row walk: loop and callback | 12.3 ns (17.0 %) | 12.5 ns (13.9 %) | 13.1 ns (12.8 %) |
| descent and page access | 0.3 ns (0.4 %) | 1.8 ns (2.0 %) | 2.0 ns (2.0 %) |
| statement scaffolding | 1.2 ns (1.6 %) | 1.5 ns (1.7 %) | 1.5 ns (1.5 %) |
| scheduler and checkpoint | 0.3 ns (0.3 %) | 0.5 ns (0.6 %) | 0.6 ns (0.5 %) |
| other | 0.0 ns (0.0 %) | 0.0 ns (0.0 %) | 0.0 ns (0.0 %) |
| samples | 14,454 | 18,144 | 20,506 |
| server CPU per scan, sampled | 723.5 µs | 902.2 µs | 1029.3 µs |
| server CPU per point statement, unsampled, same server | 25.6 µs | 28.1 µs | 38.0 µs |
| net row on the profiled build, (scan - point) / 10,000 | 69.8 ns | 87.4 ns | 99.1 ns |
| parts BM-S2..S4 replace (loop and callback, per-row checks and counters, copy, decode, residual) | 67.2 ns | 80.1 ns | 92.4 ns |
| **replaced parts / net row (BM-Q1)** | **96.3 %** | **91.6 %** | **93.2 %** |
| net row on the plain build (census, literal median) | 65.5 ns | 87.4 ns | 98.0 ns |

**BM-Q1 reading.** Gate: stop if the parts BM-S2..S4 replace are under one third of the net row. On `bn` they are
**67.2 ns of a 69.8 ns net row on the profiled build, 96.3 %** (92.9 % of the whole sampled statement); on the width
shape's cell (i) 80.1 of 87.4 ns, **91.6 %**; on cell (ii) 92.4 of 99.1 ns, **93.2 %**. The gate falls above by a factor of
2.7-2.9, so BM is not stopped; the margin is wide enough that the sampler's error does not matter (a part of 29 % of 14,454
samples is +-0.4 points; the net row on the profiled build is 6.6 % above the plain build's 65.5 ns, which is the frame
pointers and the sampling handler).

- **What the 65 ns is made of on `bn`**: per-row checks and counters 21.0 ns, decode of the filter column 19.7 ns,
  residual 13.0 ns, loop and callback 12.3 ns, copy 1.2 ns. The decode and the residual - the `int64` column turned
  into an `AstValue` and compared - are 32.7 ns for a comparison that is one integer compare; the loop, the callback and the
  per-row checks are 33.3 ns that no row's data enters. The work order's section 1.1 priced the callback, decode and
  residual at 38 of 67 ns; the same three are 45 ns here (the loop-and-callback part here also holds the leaf's slot loop).
- **What is inside `AcceptTupleAt`'s 10.5 ns of self time** (a `-g` build, the same 20,000 scans, source lines by `addr2line
  -i`; `archive/.../accepttupleat-lines.txt`): `std::string`/`std::vector`/`StatusOr` bookkeeping of the `AstValue` slots
  and the span 2.6 ns, **the visibility decision (`txn::Classify` and `ReadView::Visible`, both inline) 2.2 ns**, the
  `EvaluateAll` call and the build test 1.5 ns, the `DecodeAndResolve` call and `ExecStats::For` 1.3 ns, the prologue and
  epilogue 1.1 ns, the frame slot lookup 0.6 ns, the budget charge 0.4 ns, two flag tests 0.7 ns. The floor-path visibility decision, which the
  kernel keeps, is 2.2 ns of 65.5 (3.4 %). Next to it `PageView::ReadTuple` is 4.8 ns, `ExecStats::For` 1.7 ns,
  `DecodeColumnsInto` 9.1 ns, `DecodeOneValueInto` 5.1 ns, `BtreeVisitLeafPage` 7.7 ns, the callback thunk 6.5 ns.
- **What the profile does not price**: what replaces these parts. BM-Q12 says the same of G3; the parts table prices the
  cost BM removes, and the kernel's own compare, the slot read and the visibility decision stay in the row.
- **Parts BM does not replace**: socket 1.7 ns, parse/compile/KWP frames/Waystone/scaffolding/scheduler 2.4 ns, and the
  borrow's lock traffic (`transaction begin, commit, locks`) 0.8 ns here and 3.1 ns on the wide relation, because
  `ReadBorrow::Position` takes and releases a lock-table entry every 64 pages (point 7 of section 9).

## 6. The visibility window: 12-15 ns a lookup, 3.8 % beside a point writer, 16 % when every row is on it (BM-Q8)

**On the floor path a row costs nothing in `LookupCommit`: 19 of the 20 census cells read 0 lookups.** `bn-recent` is the same
shape on a relation loaded last, whose final 500-row `INSERT` (5 % of the rows) is above the floor because the floor rises
when the commit window reaches 1,024 entries, at mount, on an undo growth or at an idle core's burn, not per commit
(`InstanceVisibility::Reclaim`, `kReclaimFloor`): it reads 500 lookups a scan and a net row of 66.5 ns against `bn`'s 65.5
(+1.0 ns, or about 20 ns a lookup; the replicate reads 65.4, +0.1). At `cores = 1`, where the window latch is unarmed since
BP-H2, a lookup cannot be resolved below the 1 ns a row floor of this host: **0-20 ns**.

**The writers cell** (`cores = 2`, `group`; the scan session on core 1, a second process holding a session on core 0;
the scan is `bn`; 3 segments of 1,500 scans with the writer going and 3 without, alternating, per run, 8 runs, two writers):
`point` is the cell as specified - one session updating `s.v` by primary key back to back, each update waiting for its group
commit, about 724 updates a second - and `range` the extreme, `UPDATE s SET v = v + 1` over every row, 27 a second. Before
each no-writer segment 2,200 one-row relaxed transactions on a relation nobody scans pass the floor over the writer's last
commit. A segment is kept when the scan session was on core 1 and the writer's on core 0 before and after it (the load
placer may move a session between statements; 3 of 48 point segments were dropped, none of the range ones).

`point` writer:

| segments | scans | lookups per scanned row | scan core CPU per scan (median over segments) | all-server CPU per scan | window latch waits per scan | window latch wait per scan | writer updates/s | p0 | p25 | p50 | p90 | p95 | p99 | scans/s at p50 (derived) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| no writer: 22 of 24 | 33,000 | 0.1509 | 698.1 µs | 703.4 µs | 0.000 | 0.000 µs | 0 | 689.6 µs | 710.8 µs | 720.8 µs | 871.9 µs | 912.7 µs | 1207.6 µs | 1,387 /s |
| writer going: 23 of 24 | 34,500 | 0.1825 | 727.2 µs | 790.2 µs | 0.298 | 0.171 µs | 724 /s | 693.9 µs | 728.2 µs | 760.7 µs | 908.1 µs | 954.4 µs | 1300.8 µs | 1,315 /s |

`range` writer:

| segments | scans | lookups per scanned row | scan core CPU per scan (median over segments) | all-server CPU per scan | window latch waits per scan | window latch wait per scan | writer updates/s | p0 | p25 | p50 | p90 | p95 | p99 | scans/s at p50 (derived) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| no writer: 24 of 24 | 36,000 | 0.2083 | 703.6 µs | 711.1 µs | 0.000 | 0.000 µs | 0 | 690.3 µs | 712.7 µs | 729.2 µs | 869.7 µs | 970.9 µs | 1359.0 µs | 1,371 /s |
| writer going: 24 of 24 | 36,000 | 1.0668 | 1465.2 µs | 2112.1 µs | 6.111 | 11.979 µs | 27 /s | 778.8 µs | 830.9 µs | 840.5 µs | 1323.6 µs | 2038.8 µs | 16535.6 µs | 1,190 /s |

`point`, paired by run, writer going minus no writer: scan core CPU +29.1 [+7.4, +52.3] µs, p50 +36.2 [+9.5, +89.4] µs, p99 +60 [-39, +87] µs (8 runs)
segments dropped for a session off its core: 3 of 48; bad scans 0; bad updates 0

`range`, paired by run, writer going minus no writer: scan core CPU +734.6 [+648.3, +1045.7] µs, p50 +116.3 [+69.7, +127.2] µs, p99 +15124 [+14617, +19876] µs (8 runs)
segments dropped for a session off its core: 0 of 48; bad scans 0; bad updates 0

- **Beside a point writer the scan costs +29.1 µs [+7.4, +52.3] of its core's CPU (+4.2 %) and +36 µs at p50 (+5.0 %)**,
  8 of 8 runs positive. Only part of it is `LookupCommit`: segments with fewer than 500 lookups a scan (0.05 a row) still
  cost 20 µs more than the no-writer segments with none (table below), which is the writer's presence - shared cache lines,
  the page latches armed above one core, the frame table - and was not isolated further.
- **The every-row writer is the stress case**: the scan core spends 735 µs more per scan, p50 +116 µs, p99 +15.1 ms (the
  scans that meet the writer's statement mid-flight walk undo chains and wait on its page latches), 6.1 window latch waits
  and 12.0 µs of wait a scan.

`point`: scan core CPU by the share of rows that took the window path:

| lookups per scanned row | segments (no writer / writer going) | scan core CPU per scan, median [min, max] | no writer, median | writer going, median | extra over the zero-lookup median, per lookup, no-writer segments only |
|---|---|---|---|---|---|
| 0 | 17 / 0 | 690.5 [678.4, 759.9] µs | 690.5 µs | - | - |
| 0 - 0.05 | 0 / 15 | 710.7 [688.6, 805.5] µs | - | 710.7 µs | - |
| 0.05 - 0.5 | 2 / 4 | 764.5 [741.4, 796.9] µs | 794.9 µs | 750.3 µs | +65.3 ns |
| 0.5 - 0.95 | 0 / 1 | 788.1 [788.1, 788.1] µs | - | 788.1 µs | - |
| about 1 (every row) | 3 / 3 | 857.4 [830.9, 922.9] µs | 835.7 µs | 879.4 µs | +14.5 ns |

`range`: scan core CPU by the share of rows that took the window path:

| lookups per scanned row | segments (no writer / writer going) | scan core CPU per scan, median [min, max] | no writer, median | writer going, median | extra over the zero-lookup median, per lookup, no-writer segments only |
|---|---|---|---|---|---|
| 0 | 19 / 0 | 701.5 [676.2, 895.3] µs | 701.5 µs | - | - |
| about 1 (every row) | 5 / 24 | 1431.8 [799.5, 1981.5] µs | 824.0 µs | 1465.2 µs | +12.3 ns |

**The floor decides how many rows are on the window path, and it is the minimum over the cores.** In 6 of the 45 kept
point-writer segments every row took the window path (`lookups per scanned row` about 1.00, 3 with the writer going and 3
without it); 29 of the 48 range segments did. The series by segment (below) shows the pattern, on the point writer: in 4 of the 8 runs (0, 2, 3, 6) the opening segment reads
0.96-1.00, in three of them (2, 3, 6) it holds for the next segment or two - a no-writer segment included, after its flood of
2,200 commits - and then the reading is 0.00 after a later flood. `FloorCandidate` is the minimum over the
attached cores of their issue cursors and oldest unresolved ids (`src/txn/instance_visibility.cpp:180-195`), the cores carve
their transaction ids in disjoint blocks of 4,096 (`include/kds/txn/trx_id.hpp:73`), and an idle core pins the floor until the
window reaches 4,096 entries and its tick burns its cursor (`include/kds/txn/manager.hpp:813`). A scan on a quiet core with a
lower block therefore sees the busy core's recent commits above the floor. The data are consistent with that mechanism; the
id-block geometry that produced each pinned segment was not read out.

`point`, lookups per scanned row by segment, run by run (a = no writer, w = writer going):

- run 0: w 0.96 a 0.00 a 0.00 w 0.01 w 0.01 a 0.00
- run 1: a 0.00 w 0.05 w 0.15 a 0.20 a 0.00 w 0.00
- run 2: w 1.00 a 1.00 a 0.00 w 0.01 w 0.01 a 0.00
- run 3: a 1.00 w 1.00 w 0.54 a 0.00 a 0.00 w 0.03
- run 4: w 0.02 a 0.00 a 0.00 w 0.01 w 0.01 a 0.00
- run 5: a 0.00 w 0.04 w 0.09 a 0.12 a 0.00 w 0.00
- run 6: w 1.00 a 1.00 a 0.00 w 0.00 w 0.01 a 0.00
- run 7: a 0.00 w 0.05 w 0.15 a 0.00 a 0.00 w 0.01

`range`, lookups per scanned row by segment, run by run (a = no writer, w = writer going):

- run 0: w 1.20 a 0.00 a 0.00 w 0.98 w 0.99 a 0.00
- run 1: a 1.00 w 1.24 w 1.13 a 0.00 a 0.00 w 0.99
- run 2: w 1.03 a 1.00 a 0.00 w 0.99 w 0.98 a 0.00
- run 3: a 1.00 w 1.05 w 1.20 a 0.00 a 0.00 w 0.99
- run 4: w 1.12 a 0.00 a 0.00 w 0.99 w 0.99 a 0.00
- run 5: a 0.00 w 1.13 w 1.16 a 1.00 a 0.00 w 1.00
- run 6: w 1.02 a 0.00 a 0.00 w 0.99 w 0.98 a 0.00
- run 7: a 1.00 w 1.20 w 1.26 a 0.00 a 0.00 w 0.99

**Cost of one lookup**, from the no-writer segments with every row on the window path against those with none: **+14.5 ns
[point segments] and +12.3 ns [range segments]** (835.7 and 824.0 µs against 690.5 and 701.5 µs), latch armed, uncontended.
The sampler agrees:

| dump | scans | scan-core reactor CPU per scan | samples | LookupCommit frames, all | LookupCommit under the scan walk | samples on the scan walk | LookupCommit / scan-walk samples | LookupCommit per scan (share x server CPU) | as a share of the scan core's CPU | window latch wait per scan | lookups per scanned row |
|---|---|---|---|---|---|---|---|---|---|---|---|
| writers-point, writer going | 4,000 | 766.0 µs | 3,148 | 112 (3.56 %) | 110 (3.49 %) | 2,856 | 3.85 % | 28.9 µs | 3.77 % | 0.085 µs | 0.108 |
| writers-point, no writer | 4,000 | 706.0 µs | 2,809 | 0 (0.00 %) | 0 (0.00 %) | 2,676 | 0.00 % | 0.0 µs | 0.00 % | 0.000 µs | 0.000 |
| writers-range, writer going | 4,000 | 1588.5 µs | 8,226 | 600 (7.29 %) | 543 (6.60 %) | 7,902 | 6.87 % | 154.4 µs | 9.72 % | 16.047 µs | 1.160 |
| writers-range, no writer | 4,000 | 813.6 µs | 3,245 | 501 (15.44 %) | 499 (15.38 %) | 3,103 | 16.08 % | 126.7 µs | 15.58 % | 0.000 µs | 1.000 |

**BM-Q8 reading.** Gate: BM-S5 is built only if `LookupCommit` and its latch exceed 10 % of the scan's server CPU in the writers
cell, by the sampler (`LookupCommit` frames inclusive plus the window latch wait from `contention_window_wait_us`). **In the
cell as specified (the point writer going), 28.9 µs a scan (110 of 2,856 scan-walk samples, 3.85 %) plus 0.085 µs of latch wait
is 3.8 % of the scan core's 766.0 µs: the gate falls below, by a factor of 2.6, and BM-S5 is not opened by it.** The same
reading at the extremes: every row on the window path with no writer, 126.7 µs of 813.6 µs, **15.6 %**; beside the every-row
writer 154.4 µs plus 16.0 µs of latch wait of 1,588.5 µs, **10.7 %**. At 14.5 ns a lookup the share passes 10 % of a 690 µs
scan when more than 0.48 of the rows take the window path (derived). The point-writer cell's sampled window had 0.108 lookups a
row (its writer-going segments average 0.18); 13 % of its kept segments were at 1.00.

## 7. Waits: where the 713.6 µs of a `bn` statement goes

A scan statement is a read: it holds no commit wait, writes no record and, with every page resident, does no I/O. Its latency
is a sum of the following, from the census, the profile and the SHOW META deltas.

| unit measured: one `bn` scan, literal, p50 wall 713.6 µs | measured | share of p50 |
|---|---|---|
| durability / commit (fsync) | not applicable: read-only statement, no WAL record | 0 |
| write statement | not applicable | 0 |
| read: the row walk (sampled parts `residual`, `decode`, `copy`, `per-row checks`, `loop and callback`, `descent`) | 93.3 % of server CPU in the profile (675 of 723.5 µs); scaled to the plain build's 681.3 µs of server CPU, about 636 µs | about 89 % |
| server, other than the walk: socket send and read, KWP frames, parse, compile, Waystone, locks, scaffolding, scheduler | 6.7 % of server CPU in the profile; about 46 µs on the plain build | about 6 % |
| client and socket round trip, not on the server's CPU | wall p50 713.6 - server CPU 681.3 = 32.3 µs (the driver's own CPU is 47.3 µs a statement; the two overlap) | 4.5 % |
| lock or conflict wait | window latch waits: 0 at `cores = 1` (the latch is unarmed); the borrow's lock-table traffic is CPU (0.8 ns a row, 8 µs a scan) and is inside the walk's row | 0 |
| I/O wait | none by construction: every relation is resident (65,536 frames against at most 3,000 pages); `pool_misses` was not sampled per arm in the first round | 0 |

At `cores = 2` beside a point writer the scan adds window latch waits: 0.30 a scan and 0.17 µs of wait (0.02 % of the
scan); beside the every-row writer 6.1 waits and 12.0 µs a scan (1.7 % of the 713 µs baseline).

## 8. The noise floor

| source | what it says |
|---|---|
| Two 8-run censuses of the same binary, about 13 minutes apart | the medians of the 20 cells' literal net rows differ by a median of 0.6 ns (largest: `sel-100` +4.1 ns, `wide-h-k` +2.5, `wide-m` +2.2, `wide-h` +1.6; `bn` +0.22 literal, -0.40 prepared). The table below |
| Literal against prepared on one run (same walk) | median difference within +-0.6 ns on every 10,000-row cell except `sel-100` (+2.6 ns); per run, within [-3, +5] ns |
| Spread across the 8 runs of one cell | `bn`: [64.4, 70.2] ns literal - run 3 read 70.2 against 64.4-66.7 for the other seven, with nothing in its host record to explain it (gate load 1.81, no competing process) |

So **a difference between two cells' medians under about 1 ns (1.5 %) is not a finding, and under 3 ns on `sel-100` and
`wide-m`**. The figures that rest on a single such difference are marked as such where they appear (`bn-recent` against
`bn`, +1.0 ns; the nullable bitmap, +1.6 ns).

| cell | net row, this set, literal | net row, replicate set, literal | this - replicate | this - replicate, prepared | paired literal - prepared, per run |
|---|---|---|---|---|---|
| rows-1k | 66.2 ns | 65.7 ns | +0.47 ns | +1.59 ns | -0.43 [-1.18, +0.89] ns |
| bn | 65.5 ns | 65.3 ns | +0.22 ns | -0.40 ns | +0.53 [-0.79, +1.59] ns |
| rows-100k | 65.0 ns | 65.4 ns | -0.37 ns | -0.04 ns | -0.01 [-0.75, +0.48] ns |
| bn-recent | 66.5 ns | 65.4 ns | +1.16 ns | +0.27 ns | +0.32 [-2.30, +1.80] ns |
| sel-0 | 65.0 ns | 65.1 ns | -0.13 ns | -0.25 ns | +0.08 [-1.00, +1.91] ns |
| sel-1 | 66.8 ns | 66.6 ns | +0.10 ns | -0.03 ns | -0.14 [-0.91, +0.10] ns |
| sel-10 | 79.1 ns | 79.6 ns | -0.47 ns | +0.59 ns | -0.14 [-1.11, +0.78] ns |
| sel-100 | 210.5 ns | 206.4 ns | +4.09 ns | +1.24 ns | +2.64 [-5.81, +4.64] ns |
| wide-h | 87.4 ns | 85.8 ns | +1.63 ns | +1.14 ns | +0.19 [-0.45, +2.53] ns |
| wide-h-k | 98.0 ns | 95.6 ns | +2.48 ns | +2.53 ns | -0.11 [-0.43, +1.58] ns |
| wide-a | 84.7 ns | 85.3 ns | -0.60 ns | +1.08 ns | -0.11 [-2.14, +1.45] ns |
| wide-c | 89.4 ns | 89.2 ns | +0.12 ns | +0.75 ns | +0.06 [-0.37, +1.18] ns |
| wide-d | 83.4 ns | 82.3 ns | +1.10 ns | +1.92 ns | +0.52 [-1.78, +1.93] ns |
| wide-e | 82.4 ns | 81.9 ns | +0.54 ns | +0.28 ns | +0.28 [-0.43, +1.53] ns |
| wide-f | 84.1 ns | 83.9 ns | +0.23 ns | +0.75 ns | +0.12 [-0.72, +1.51] ns |
| wide-g | 84.2 ns | 84.9 ns | -0.72 ns | +0.41 ns | +0.04 [-2.31, +1.17] ns |
| wide-k | 91.2 ns | 91.5 ns | -0.36 ns | -0.73 ns | +0.37 [-0.18, +1.23] ns |
| wide-m | 101.7 ns | 99.5 ns | +2.23 ns | +3.70 ns | -0.22 [-3.09, +0.97] ns |
| nul-0 | 67.1 ns | 66.5 ns | +0.62 ns | +1.49 ns | -0.16 [-2.36, +0.86] ns |
| nul-50 | 64.8 ns | 63.7 ns | +1.11 ns | +1.60 ns | +0.04 [-1.32, +3.05] ns |

The idle server burns 6.2-6.9 ms of CPU a second (`idle_cpu_us_per_s`), not subtracted: it adds under 5 ns to a 700 µs
statement.

## 9. What the run teaches

1. **The scan row is entirely a per-row cost, and 92-96 % of it sits in the parts BM's stages replace.** Nothing fixed hides in
   it (65.0-66.2 ns across 1,000 to 100,000 rows; 0.6 µs a scan beyond a point statement at 200 rows by the control), the two
   driver paths agree, and the five parts BM-S2..S4 name are 67 of 70 ns. The order's premise - a distributed cost is cut by
   changing the unit of per-row work - has its first measured support: no single function is above 16 % of the statement, and
   the five parts together are the row.
2. **What the kernel must keep is small and measured**: the floor-path visibility decision is 2.2 ns of the 65.5 and
   `PageView::ReadTuple`'s slot read 4.8 ns, so 7.0 ns of the row are parts BM keeps by construction; the typed compare and
   the kernel's own loop are the unknowns, and the profile prices only what is removed, not what replaces it (BM-Q12, G3).
3. **A second conjunct costs a column decode on every row** (`h = ? AND k = ?`: +10.6 ns), and a wide row costs 17 ns more
   whatever the type, spread over copy, checks, decode and page work. A kernel that rejects before copying and before decoding
   is aimed at both; a cheaper compare touches neither. The string families carry the most decode per row (+6.5 and +17.0 ns
   over `int32`).
4. **The 100 % cell is the result path** (145 of 210 ns), identical in KDS and PostgreSQL to within 3 ns: BM-Q2 (a)'s
   all-pass cost is a kernel compare on top of an emission the kernel cannot shorten, and BM-Q12's guard on that cell guards
   the path BM does not touch.
5. **`LookupCommit` costs 12-15 ns and is rare on a one-core or quiet instance, but in a two-core instance the floor can sit
   behind a quiet core's cursor**, and then it is 15.6 % of a scan's CPU with no writer at all. This is the first measured
   input to BM-R7: the work order's premise (section 1.6) that BN's shape never left the floor path holds (0 lookups in
   every census cell); its worry that a scan over recently written rows pays a shared latch per row is true but small
   uncontended (0.3 waits a scan beside a point writer). What decides the cost is how many rows the floor leaves above it
   (a floor question as much as a scan-kernel one), and BM-R7's memo, if it is ever built, is priced by the 14.5 ns a lookup
   and the fraction of rows measured here.
6. **PostgreSQL's per-tuple visibility check is 5.6 ns of its row (16 %) until its pages are all-visible; KDS's floor path is
   2.2 ns.** A twin measured un-vacuumed (BN's) overstates PostgreSQL's row by that much; both are reported.
7. **The borrow's per-slice lock traffic is a fixed cost of about 5.8 µs per 64-page slice** (derived from the profile: 159
   samples on `bn`'s one slice and 624 on the 304-page wide relation's five, 20,000 scans each; `LockTable::AcquireInner` and
   `ReleaseOne` under `ReadBorrow::Position`, with an `operator new` and a `free` each). It is 0.8 ns of `bn`'s row and
   3.1 ns of the wide relation's, and no BM stage touches it.
8. **Operational**: this host is shared. The first not-vacuumed twin and both writers attempts of the second round overlapped
   another session's builds or test suites; the host record per run is what separated them. A cell that must be quiet needs
   a gate on *running compilers and test binaries*, not on the load average alone, because the measurement's own two busy
   tasks hold the 1-minute load near 1.5-2.0.

## 10. Not run

See the list under "What was run, and what was not", and the second round. In addition: the sampler's 1 kHz resolves a part
of 1 % to about 0.1 points at the sample counts used (14,454-20,506) and a profile is one pass per cell, so a part
under 0.5 % is not read; the writers cell's `range` variant was run only at `group`; the pinned-floor mechanism of section 6
is read from the code and consistent with the series, not isolated by a probe.

## 11. Baseline for the next run

This file is the baseline for `bn`, the rows, selectivity, width and nullable cells, the visibility cells and the profile, at
`v2.7.0-942-gf5a7efb2`. The numbers to hold the next run against (literal path, net row, median of 8): `bn` **65.5 ns**,
`rows-1k` 66.2, `rows-100k` 65.0, `sel-10` 79.1, `sel-100` 210.5, `wide-h` 87.4, `wide-h-k` 98.0, `wide-m` 101.7, `nul-0` 67.1,
`nul-50` 64.8; scan CPU on `bn` 681.3 µs; the writers cell's scan-core CPU 698.1 µs without and 727.2 µs with the point writer;
`LookupCommit` 14.5 ns a lookup and 3.8 % of the scan core's CPU beside the point writer. The PostgreSQL floor on the same
shapes: `bn` 29.3 ns vacuumed, 34.9 ns not vacuumed. A run with no prior number for a shape (the 200-row size) says so.

## Appendix A. Statement wall time, pooled over the 8 runs, and server CPU

Wall time per statement as the client sees it (`time.perf_counter()` around the call). `n` is the pooled statement count.
Percentiles are of the pooled statements, not medians of per-run percentiles; the statements per second are `1,000,000 / p50`.

| cell | path | n | p0 | p25 | p50 | p90 | p95 | p99 | statements/s at p50 (derived) |
|---|---|---|---|---|---|---|---|---|---|
| point s1k | literal | 24,000 | 51.5 µs | 54.3 µs | 55.4 µs | 66.4 µs | 73.3 µs | 85.4 µs | 18,051 /s |
| point s1k | prepared | 24,000 | 48.9 µs | 51.1 µs | 52.1 µs | 62.1 µs | 68.4 µs | 80.1 µs | 19,194 /s |
| rows-1k | literal | 6,144 | 116.5 µs | 119.7 µs | 121.8 µs | 139.9 µs | 147.3 µs | 176.9 µs | 8,210 /s |
| rows-1k | prepared | 6,144 | 113.1 µs | 116.6 µs | 118.5 µs | 136.7 µs | 145.2 µs | 181.8 µs | 8,439 /s |
| point s | literal | 24,000 | 51.3 µs | 53.6 µs | 54.4 µs | 64.0 µs | 70.5 µs | 109.4 µs | 18,382 /s |
| point s | prepared | 24,000 | 44.1 µs | 50.4 µs | 51.2 µs | 60.4 µs | 67.0 µs | 86.0 µs | 19,531 /s |
| bn | literal | 1,536 | 688.5 µs | 706.4 µs | 713.6 µs | 760.5 µs | 795.9 µs | 901.7 µs | 1,401 /s |
| bn | prepared | 1,536 | 683.5 µs | 702.3 µs | 709.3 µs | 747.7 µs | 770.9 µs | 849.7 µs | 1,410 /s |
| point s100k | literal | 24,000 | 52.0 µs | 54.3 µs | 55.1 µs | 64.4 µs | 71.0 µs | 84.3 µs | 18,149 /s |
| point s100k | prepared | 24,000 | 48.5 µs | 51.2 µs | 52.0 µs | 62.6 µs | 69.9 µs | 87.0 µs | 19,231 /s |
| rows-100k | literal | 384 | 6431.1 µs | 6497.1 µs | 6550.9 µs | 7022.0 µs | 7105.5 µs | 7311.0 µs | 153 /s |
| rows-100k | prepared | 384 | 6430.7 µs | 6497.3 µs | 6539.2 µs | 7038.5 µs | 7140.8 µs | 8162.3 µs | 153 /s |
| point srec | literal | 24,000 | 51.5 µs | 53.8 µs | 54.5 µs | 65.0 µs | 70.9 µs | 84.9 µs | 18,349 /s |
| point srec | prepared | 24,000 | 48.5 µs | 50.6 µs | 51.3 µs | 60.9 µs | 67.2 µs | 78.8 µs | 19,493 /s |
| bn-recent | literal | 1,536 | 689.8 µs | 710.5 µs | 717.9 µs | 771.4 µs | 813.0 µs | 1032.5 µs | 1,393 /s |
| bn-recent | prepared | 1,536 | 685.0 µs | 706.6 µs | 714.5 µs | 756.6 µs | 802.7 µs | 1042.9 µs | 1,400 /s |
| sel-0 | literal | 1,536 | 685.0 µs | 702.7 µs | 709.5 µs | 751.6 µs | 793.7 µs | 1134.3 µs | 1,409 /s |
| sel-0 | prepared | 1,536 | 682.6 µs | 698.8 µs | 704.8 µs | 746.5 µs | 782.7 µs | 974.8 µs | 1,419 /s |
| sel-1 | literal | 1,536 | 834.0 µs | 852.3 µs | 860.0 µs | 933.1 µs | 991.4 µs | 1215.0 µs | 1,163 /s |
| sel-1 | prepared | 1,536 | 830.6 µs | 848.7 µs | 857.3 µs | 936.0 µs | 1004.9 µs | 1216.0 µs | 1,166 /s |
| sel-10 | literal | 1,536 | 2107.2 µs | 2162.3 µs | 2181.7 µs | 2316.0 µs | 2384.2 µs | 2656.9 µs | 458 /s |
| sel-10 | prepared | 1,536 | 2103.0 µs | 2159.0 µs | 2179.4 µs | 2324.0 µs | 2397.7 µs | 4920.6 µs | 459 /s |
| sel-100 | literal | 768 | 14758.1 µs | 15396.7 µs | 15619.8 µs | 19530.1 µs | 21382.8 µs | 26938.6 µs | 64 /s |
| sel-100 | prepared | 768 | 14777.2 µs | 15406.8 µs | 15594.8 µs | 18375.3 µs | 20862.6 µs | 26527.9 µs | 64 /s |
| point wd | literal | 24,000 | 70.7 µs | 73.2 µs | 74.2 µs | 87.9 µs | 96.6 µs | 114.1 µs | 13,477 /s |
| point wd | prepared | 24,000 | 67.8 µs | 70.0 µs | 71.0 µs | 83.8 µs | 92.9 µs | 107.9 µs | 14,085 /s |
| wide-h | literal | 1,536 | 907.9 µs | 933.7 µs | 952.7 µs | 1024.0 µs | 1073.1 µs | 1291.5 µs | 1,050 /s |
| wide-h | prepared | 1,536 | 904.4 µs | 929.2 µs | 948.6 µs | 1016.0 µs | 1054.6 µs | 1171.3 µs | 1,054 /s |
| wide-h-k | literal | 1,536 | 1009.6 µs | 1037.7 µs | 1059.6 µs | 1140.1 µs | 1182.3 µs | 1492.4 µs | 944 /s |
| wide-h-k | prepared | 1,536 | 1005.4 µs | 1032.0 µs | 1054.4 µs | 1130.8 µs | 1174.5 µs | 1311.2 µs | 948 /s |
| wide-a | literal | 1,536 | 877.7 µs | 911.7 µs | 927.4 µs | 998.7 µs | 1038.3 µs | 1373.6 µs | 1,078 /s |
| wide-a | prepared | 1,536 | 880.3 µs | 907.1 µs | 924.1 µs | 991.2 µs | 1030.1 µs | 1478.9 µs | 1,082 /s |
| wide-c | literal | 1,536 | 928.2 µs | 961.3 µs | 978.6 µs | 1028.5 µs | 1073.0 µs | 1364.1 µs | 1,022 /s |
| wide-c | prepared | 1,536 | 920.5 µs | 957.2 µs | 974.1 µs | 1031.9 µs | 1071.0 µs | 1303.1 µs | 1,027 /s |
| wide-d | literal | 1,536 | 864.3 µs | 895.5 µs | 909.2 µs | 1021.3 µs | 1054.6 µs | 1335.9 µs | 1,100 /s |
| wide-d | prepared | 1,536 | 862.9 µs | 890.7 µs | 903.3 µs | 1013.8 µs | 1056.0 µs | 1369.0 µs | 1,107 /s |
| wide-e | literal | 1,536 | 861.6 µs | 893.0 µs | 907.6 µs | 1001.8 µs | 1123.1 µs | 1537.2 µs | 1,102 /s |
| wide-e | prepared | 1,536 | 861.6 µs | 889.1 µs | 902.9 µs | 979.5 µs | 1091.2 µs | 1461.7 µs | 1,108 /s |
| wide-f | literal | 1,536 | 875.0 µs | 906.7 µs | 923.0 µs | 1018.2 µs | 1062.0 µs | 1526.6 µs | 1,083 /s |
| wide-f | prepared | 1,536 | 875.7 µs | 902.9 µs | 918.1 µs | 1014.4 µs | 1058.7 µs | 1526.8 µs | 1,089 /s |
| wide-g | literal | 1,536 | 876.5 µs | 910.4 µs | 926.4 µs | 985.1 µs | 1034.6 µs | 1285.9 µs | 1,079 /s |
| wide-g | prepared | 1,536 | 876.0 µs | 907.0 µs | 922.7 µs | 989.8 µs | 1041.9 µs | 1195.2 µs | 1,084 /s |
| wide-k | literal | 1,536 | 952.4 µs | 981.3 µs | 994.9 µs | 1082.0 µs | 1145.4 µs | 1347.2 µs | 1,005 /s |
| wide-k | prepared | 1,536 | 949.3 µs | 976.9 µs | 989.1 µs | 1088.6 µs | 1126.0 µs | 1181.6 µs | 1,011 /s |
| wide-m | literal | 1,536 | 1038.3 µs | 1065.5 µs | 1086.4 µs | 1183.3 µs | 1242.7 µs | 1696.5 µs | 920 /s |
| wide-m | prepared | 1,536 | 1031.9 µs | 1061.5 µs | 1081.8 µs | 1181.8 µs | 1240.7 µs | 1697.3 µs | 924 /s |
| point nul0 | literal | 24,000 | 48.0 µs | 54.2 µs | 54.9 µs | 64.2 µs | 70.7 µs | 82.7 µs | 18,215 /s |
| point nul0 | prepared | 24,000 | 49.2 µs | 51.0 µs | 51.7 µs | 60.6 µs | 67.0 µs | 78.4 µs | 19,342 /s |
| nul-0 | literal | 1,536 | 700.3 µs | 721.5 µs | 733.3 µs | 792.9 µs | 817.2 µs | 914.4 µs | 1,364 /s |
| nul-0 | prepared | 1,536 | 697.3 µs | 716.9 µs | 729.4 µs | 793.7 µs | 823.9 µs | 1042.5 µs | 1,371 /s |
| point nul50 | literal | 24,000 | 52.2 µs | 54.4 µs | 55.1 µs | 67.2 µs | 74.2 µs | 89.1 µs | 18,149 /s |
| point nul50 | prepared | 24,000 | 48.5 µs | 51.1 µs | 51.8 µs | 62.2 µs | 68.8 µs | 81.1 µs | 19,305 /s |
| nul-50 | literal | 1,536 | 668.9 µs | 688.0 µs | 702.3 µs | 782.5 µs | 834.5 µs | 1133.3 µs | 1,424 /s |
| nul-50 | prepared | 1,536 | 662.7 µs | 684.5 µs | 698.8 µs | 781.7 µs | 824.5 µs | 1054.2 µs | 1,431 /s |

PostgreSQL 18.6, vacuumed, simple query protocol, pooled:

| cell | n | p0 | p25 | p50 | p90 | p95 | p99 | statements/s at p50 (derived) |
|---|---|---|---|---|---|---|---|---|
| point:s1k | 24,000 | 52.8 µs | 56.6 µs | 57.2 µs | 67.4 µs | 74.4 µs | 86.0 µs | 17,483 /s |
| rows-1k | 6,144 | 79.7 µs | 81.5 µs | 82.2 µs | 96.2 µs | 102.8 µs | 117.1 µs | 12,165 /s |
| point:s | 24,000 | 53.7 µs | 56.8 µs | 57.4 µs | 67.8 µs | 74.8 µs | 86.6 µs | 17,422 /s |
| bn | 1,536 | 336.2 µs | 342.5 µs | 351.0 µs | 380.9 µs | 410.8 µs | 593.4 µs | 2,849 /s |
| point:s100k | 24,000 | 53.6 µs | 57.6 µs | 58.3 µs | 69.4 µs | 76.2 µs | 88.3 µs | 17,153 /s |
| rows-100k | 384 | 2913.5 µs | 2943.1 µs | 2968.9 µs | 3126.9 µs | 3296.3 µs | 5039.7 µs | 337 /s |
| sel-0 | 1,536 | 334.1 µs | 340.7 µs | 348.3 µs | 365.8 µs | 373.0 µs | 422.2 µs | 2,871 /s |
| sel-1 | 1,536 | 442.5 µs | 452.0 µs | 458.8 µs | 484.2 µs | 517.0 µs | 592.4 µs | 2,180 /s |
| sel-10 | 1,536 | 1061.0 µs | 1082.9 µs | 1093.6 µs | 1147.9 µs | 1515.7 µs | 1684.1 µs | 914 /s |
| sel-100 | 768 | 9647.0 µs | 9902.8 µs | 9990.3 µs | 10507.4 µs | 10838.0 µs | 13541.5 µs | 100 /s |
| point:wd | 24,000 | 62.0 µs | 64.6 µs | 65.4 µs | 77.3 µs | 83.8 µs | 95.2 µs | 15,291 /s |
| wide-h | 1,536 | 719.0 µs | 741.5 µs | 752.1 µs | 805.0 µs | 887.7 µs | 1259.2 µs | 1,330 /s |
| wide-h-k | 1,536 | 741.6 µs | 762.0 µs | 771.5 µs | 861.0 µs | 1044.2 µs | 1388.2 µs | 1,296 /s |
| wide-a | 1,536 | 409.8 µs | 421.9 µs | 430.8 µs | 456.2 µs | 492.0 µs | 628.9 µs | 2,321 /s |
| wide-c | 1,536 | 447.4 µs | 463.2 µs | 472.3 µs | 496.0 µs | 519.5 µs | 751.3 µs | 2,117 /s |
| wide-d | 1,536 | 428.9 µs | 444.1 µs | 454.5 µs | 483.3 µs | 513.6 µs | 669.4 µs | 2,200 /s |
| wide-e | 1,536 | 469.2 µs | 484.4 µs | 493.2 µs | 524.6 µs | 555.1 µs | 707.7 µs | 2,028 /s |
| wide-f | 1,536 | 486.7 µs | 501.3 µs | 510.7 µs | 584.6 µs | 672.7 µs | 874.4 µs | 1,958 /s |
| wide-g | 1,536 | 694.2 µs | 717.5 µs | 733.3 µs | 802.2 µs | 847.6 µs | 1022.4 µs | 1,364 /s |
| wide-k | 1,536 | 670.4 µs | 691.3 µs | 703.9 µs | 763.4 µs | 809.5 µs | 972.6 µs | 1,421 /s |
| wide-m | 1,536 | 646.1 µs | 665.8 µs | 676.1 µs | 724.4 µs | 764.7 µs | 981.6 µs | 1,479 /s |
| point:nul0 | 24,000 | 53.6 µs | 57.0 µs | 57.8 µs | 73.5 µs | 81.6 µs | 97.5 µs | 17,301 /s |
| nul-0 | 1,536 | 313.9 µs | 321.9 µs | 330.9 µs | 358.7 µs | 394.7 µs | 481.5 µs | 3,022 /s |
| point:nul50 | 24,000 | 54.8 µs | 57.1 µs | 57.7 µs | 71.8 µs | 79.6 µs | 91.1 µs | 17,331 /s |
| nul-50 | 1,536 | 325.5 µs | 333.9 µs | 343.2 µs | 376.4 µs | 402.1 µs | 462.4 µs | 2,914 /s |

Server CPU per statement (median [min, max] over the 8 runs of each run's mean):

| cell | KDS scan CPU, literal | KDS scan CPU, prepared | KDS point CPU, literal | PG scan CPU, vacuumed | PG point CPU, vacuumed |
|---|---|---|---|---|---|
| rows-1k | 92.8 [90.3, 100.6] µs | 92.7 [90.0, 100.3] µs | 26.6 [25.4, 29.6] µs | 66.5 [65.6, 68.1] µs | 41.5 [40.9, 43.7] µs |
| bn | 681.3 [671.8, 731.3] µs | 678.0 [671.4, 720.0] µs | 27.7 [24.7, 28.9] µs | 335.6 [326.7, 363.3] µs | 41.9 [40.8, 44.1] µs |
| rows-100k | 6526.7 [6443.6, 7007.6] µs | 6536.4 [6449.9, 7061.6] µs | 25.4 [24.5, 27.8] µs | 2960.3 [2928.7, 3342.7] µs | 42.6 [41.6, 45.6] µs |
| bn-recent | 690.1 [675.9, 728.0] µs | 682.7 [675.3, 729.9] µs | 24.8 [24.3, 27.2] µs | - | - |
| sel-0 | 677.3 [673.4, 729.0] µs | 675.6 [672.9, 717.2] µs | 27.7 [24.7, 28.9] µs | 331.7 [326.4, 339.3] µs | 41.9 [40.8, 44.1] µs |
| sel-1 | 693.3 [688.7, 744.3] µs | 694.3 [687.4, 747.0] µs | 27.7 [24.7, 28.9] µs | 346.4 [341.8, 352.8] µs | 41.9 [40.8, 44.1] µs |
| sel-10 | 817.1 [804.6, 875.5] µs | 821.4 [804.4, 870.3] µs | 27.7 [24.7, 28.9] µs | 478.0 [473.1, 499.2] µs | 41.9 [40.8, 44.1] µs |
| sel-100 | 2132.5 [2043.3, 2167.1] µs | 2118.5 [2033.3, 2208.9] µs | 27.7 [24.7, 28.9] µs | 1755.5 [1734.3, 1884.5] µs | 41.9 [40.8, 44.1] µs |
| wide-h | 903.1 [877.8, 948.2] µs | 900.4 [876.4, 951.9] µs | 27.9 [26.5, 30.8] µs | 736.2 [724.3, 788.3] µs | 48.7 [47.7, 49.4] µs |
| wide-h-k | 1008.0 [981.6, 1051.4] µs | 1008.0 [973.3, 1047.0] µs | 27.9 [26.5, 30.8] µs | 755.1 [738.6, 943.5] µs | 48.7 [47.7, 49.4] µs |
| wide-a | 875.4 [858.3, 927.3] µs | 883.7 [853.2, 912.0] µs | 27.9 [26.5, 30.8] µs | 409.9 [405.6, 423.6] µs | 48.7 [47.7, 49.4] µs |
| wide-c | 922.4 [902.5, 960.0] µs | 923.8 [898.5, 963.0] µs | 27.9 [26.5, 30.8] µs | 453.7 [442.4, 467.2] µs | 48.7 [47.7, 49.4] µs |
| wide-d | 862.4 [846.4, 982.3] µs | 869.1 [840.4, 962.3] µs | 27.9 [26.5, 30.8] µs | 437.3 [424.3, 450.7] µs | 48.7 [47.7, 49.4] µs |
| wide-e | 851.3 [834.2, 976.5] µs | 848.8 [837.2, 960.8] µs | 27.9 [26.5, 30.8] µs | 475.9 [462.2, 492.2] µs | 48.7 [47.7, 49.4] µs |
| wide-f | 869.2 [854.8, 948.2] µs | 873.6 [853.0, 932.3] µs | 27.9 [26.5, 30.8] µs | 492.7 [479.2, 603.3] µs | 48.7 [47.7, 49.4] µs |
| wide-g | 869.8 [856.4, 928.5] µs | 869.7 [853.9, 919.4] µs | 27.9 [26.5, 30.8] µs | 709.7 [700.4, 760.3] µs | 48.7 [47.7, 49.4] µs |
| wide-k | 939.3 [927.2, 1051.7] µs | 932.6 [925.0, 1043.3] µs | 27.9 [26.5, 30.8] µs | 685.7 [672.1, 708.8] µs | 48.7 [47.7, 49.4] µs |
| wide-m | 1045.6 [1013.0, 1109.2] µs | 1050.7 [1007.0, 1122.9] µs | 27.9 [26.5, 30.8] µs | 659.1 [644.1, 677.3] µs | 48.7 [47.7, 49.4] µs |
| nul-0 | 696.6 [684.2, 729.4] µs | 703.7 [684.6, 733.5] µs | 25.5 [24.5, 27.0] µs | 314.6 [307.3, 328.9] µs | 43.1 [41.1, 43.8] µs |
| nul-50 | 673.7 [651.2, 721.1] µs | 672.5 [650.4, 733.2] µs | 26.0 [24.5, 30.2] µs | 327.0 [316.1, 340.1] µs | 43.1 [41.5, 44.4] µs |

PostgreSQL's plan for each cell:

| cell | plan (first EXPLAIN, vacuumed run 0) |
|---|---|
| rows-1k | `Seq Scan on bm_s1k  (cost=0.00..19.50 rows=1 width=24) /   Filter: (w = 596)` |
| bn | `Seq Scan on bm_s  (cost=0.00..189.00 rows=1 width=24) /   Filter: (w = 9596)` |
| rows-100k | `Seq Scan on bm_s100k  (cost=0.00..1887.00 rows=1 width=24) /   Filter: (w = 39596)` |
| sel-0 | `Seq Scan on bm_s  (cost=0.00..189.00 rows=1 width=24) /   Filter: (w < 1)` |
| sel-1 | `Seq Scan on bm_s  (cost=0.00..189.00 rows=100 width=24) /   Filter: (w < 101)` |
| sel-10 | `Seq Scan on bm_s  (cost=0.00..189.00 rows=1000 width=24) /   Filter: (w < 1001)` |
| sel-100 | `Seq Scan on bm_s  (cost=0.00..189.00 rows=9999 width=24) /   Filter: (w < 10001)` |
| wide-h | `Seq Scan on bm_wd  (cost=0.00..681.00 rows=1 width=393) /   Filter: (h = 9596.7172)` |
| wide-h-k | `Seq Scan on bm_wd  (cost=0.00..706.00 rows=1 width=393) /   Filter: ((h = 9596.7172) AND (k = 'k00009596'::bpchar))` |
| wide-a | `Seq Scan on bm_wd  (cost=0.00..681.00 rows=1 width=393) /   Filter: (a = 9596)` |
| wide-c | `Seq Scan on bm_wd  (cost=0.00..681.00 rows=1 width=393) /   Filter: (c = 9596)` |
| wide-d | `Seq Scan on bm_wd  (cost=0.00..656.00 rows=1 width=393) /   Filter: d` |
| wide-e | `Seq Scan on bm_wd  (cost=0.00..681.00 rows=1 width=393) /   Filter: (e = '2052-04-10'::date)` |
| wide-f | `Seq Scan on bm_wd  (cost=0.00..681.00 rows=1 width=393) /   Filter: (f = '2026-01-01 02:39:56'::timestamp without time zone)` |
| wide-g | `Seq Scan on bm_wd  (cost=0.00..681.00 rows=1 width=393) /   Filter: (g = 95.96)` |
| wide-k | `Seq Scan on bm_wd  (cost=0.00..681.00 rows=1 width=393) /   Filter: (k = 'k00009596'::bpchar)` |
| wide-m | `Seq Scan on bm_wd  (cost=0.00..681.00 rows=1 width=393) /   Filter: ((m)::text = 'm00009596'::text)` |
| nul-0 | `Seq Scan on bm_nul0  (cost=0.00..189.00 rows=1 width=24) /   Filter: (n = 9596)` |
| nul-50 | `Seq Scan on bm_nul50  (cost=0.00..184.00 rows=1 width=24) /   Filter: (n = 9192)` |

## Appendix B. Host state

Per run of the first-round census (the 1-minute load average includes the measurement's own tasks):

| run | 1-min load at the gate | load before / after the run | cell loads min - max | competing processes recorded |
|---|---|---|---|---|
| 0 | 1.99 | 1.99 / 1.65 | 1.71 - 1.83 | 0 |
| 1 | 1.65 | 1.65 / 1.59 | 1.55 - 1.65 | 0 |
| 2 | 1.59 | 1.59 / 1.81 | 1.49 - 1.81 | 0 |
| 3 | 1.81 | 1.81 / 1.67 | 1.64 - 1.69 | 0 |
| 4 | 1.67 | 1.67 / 1.45 | 1.45 - 1.53 | 0 |
| 5 | 1.45 | 1.45 / 1.53 | 1.49 - 1.54 | 0 |
| 6 | 1.53 | 1.53 / 1.59 | 1.57 - 1.64 | 0 |
| 7 | 1.59 | 1.59 / 1.43 | 1.42 - 1.47 | 0 |

bad replies: 0; idle CPU per second per run (µs): [6580, 6627, 6853, 6314, 6783, 6207, 6532, 6348]
meta self lookups per run: [0, 0, 0, 0, 0, 0, 0, 0]

The not-vacuumed twin's clean pass (02:15:21-02:16:10) read a constant 1-minute load of 2.05-2.09 with no competing process in
any run's before and after record; the writers cell's runs read 1.2-2.6 with none (`archive/.../writers.json.gz`, `gates.txt`).

## Appendix C. Reproducing

All from `/home/cdkbs/ckdbs/.claude/worktrees/bm-scan-kernel-open` with the copies named in the stamp; the build is
`git archive f5a7efb2 | tar -x -C <dir>` then `cmake -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF
[-DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer]` and `cmake --build --target kds_server`.

    python3 tools/bm_scan.py census --bin <copy> --workdir <dir> --port 32101 --runs 8 --pin-server 2 --pin-driver 4 \
        --quiet-load 2 --quiet-wait 900 --out census.json
    python3 tools/bn_pg_scan.py --shapes bm --port 15434 --runs 8 --scan-ops 192 --blocks 12 --point-ops 3000 \
        --pin-backend 2 --pin-driver 4 --out pg.json            # add --no-vacuum for the not-vacuumed twin
    python3 tools/bm_scan.py writers --bin <copy> --workdir <dir> --port 32111 --runs 8 --blocks 3 --writer-block-ops 1500 \
        --variant point,range --pin-driver 4 --pin-writer 0 --reactor-cpus 2,6 --pin-server 2,3,6,7 \
        --quiet-load 2 --quiet-wait 900 --out writers.json
    python3 tools/bm_scan.py profile --bin <fp copy> --sampler <kds_sampler.so> --sampler-dir <dir> --workdir <dir> \
        --port 32121 --pin-server 2 --pin-driver 4 --profile-cells bn,wide-h,wide-h-k --profile-scans 20000 \
        --point-ops 3000 --writers --variant point,range --profile-writer-scans 4000 --reactor-cpus 2,6 --out profile.json
    (the source-line pass: the -g copy, --profile-cells bn,wide-h, --out profile-g.json)
    python3 tools/bm_waystone_control.py --bin <copy> --workdir <dir> --port 32141 --runs 6 --out waystone-control.json
    python3 tools/bm_scan_table.py census census.json --pg pg.json --pg2 pg-novacuum.json --replicate census-first.json \
        --section net|cpu|pct|plans|noise|hosts
    python3 tools/bm_scan_table.py writers writers.json --clean --section summary|paired|bins|series
    python3 tools/bm_scan_table.py profile profile.json --census census.json --section compact|lookup|full

Raw output: `bench/v3.0.0/archive/bm-s1-scan-row-v2.7.0-942-gf5a7efb2/` - the census, its replicate, both twins, the writers
cell and the profile JSON (gzip where large), the run logs, the time and gate records, `binaries.txt`, `host.txt`
(device, PostgreSQL settings, server configs, driver hashes), `profile-full.txt` (every part of every dump),
`accepttupleat-lines.txt`, the control and `second-round-stopped/`. The sampler's raw dumps (2.8-4.0 MB each) stay in
`/home/cdkbs/bench-runs/bm-s1/prof/` and `prof-g/` and are named in `profile.json`; folding them needs the profiled
binary of the same sha256.
