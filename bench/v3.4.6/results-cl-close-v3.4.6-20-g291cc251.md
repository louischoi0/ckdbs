# CL close: what milestone CL's commit callback costs the commit of a transaction, against the commit it opened from

Milestone CL put an `after_logged` callback (a `std::function`) on every `TransactionManager::Commit` and `CommitDeferred`, and made every checkpoint write one `ASSERT_PENDING` record per open reservation. Neither is visible on the statement path in this run. On a Release build, with the driver on separate CPUs, the `COMMIT` statement at the closing commit is within 0.4 µs of the base at p50 in 17 of the 18 `relaxed` cells, and in the eighteenth (2 cores, four assertions, K = 100) the original run read 0.7 µs longer against a control that moved by -0.1 µs. Two fresh-server re-runs of the whole 2-core `relaxed` configuration read that cell at -0.1 and +0.1 µs against controls of -0.3 and -0.3 µs, and no 2-core `relaxed` cell exceeds +0.5 µs in them, so the +0.7 was server-to-server drift and not a cost: every `relaxed` cell is inside the floor. The control relation (no assertion, so the only thing CL can have added to it is the callback itself) moved by +0.1 to +0.3 µs at p50 on one core and -0.1 to +0.2 µs on two: that is H3, and it holds. Under `group` the commit waits 450 to 1,200 µs for the log sync and every p50 delta sits among deltas of both signs (the control alone moves between -11.6 and +10.3 µs). PostgreSQL is not part of this comparison: it parses `CREATE ASSERTION` in no release, so there is no twin and no floor for this shape.

## Run stamp

| Item | Value |
|---|---|
| Executed | 2026-11-06, `c1-relaxed` 05:14:49 to 05:16:53, `c2-relaxed` 05:17:08 to 05:19:06, `c1-group` 05:19:22 to 05:21:56, `c2-group` 05:22:11 to 05:24:54 UTC; one contiguous run, no cell repeated; then, 05:28:31 to 05:32:45 UTC, two re-runs of `c2-relaxed` (see "Re-runs of the 2-core `relaxed` configuration") |
| Version (closing side) | `v3.4.6-20-g291cc251`, branch `worktree-ck-assertion-commit-settle`, worktree `ck-assertion-commit-settle`. Tree: only documents (`CLAUDE.md`, `docs/`, `manual/`) were uncommitted when the run began. At 05:23:49 UTC, during the `c2-group` cell, a comment-only edit to `src/server/tcp_server.cpp` appeared in the worktree; the closing binary had been built at 05:13:10 UTC and was copied before the first cell, so it is unaffected (the run measured the copy) |
| Version (base side) | `v3.4.6-17-gb4a9e102`, the commit CL opened from, in the detached worktree `/home/louis/ck-tmp/cl-base`, `git status --short` empty |
| What differs between the two sides | `git diff --stat b4a9e102 291cc251 -- src include`: 19 files, 447 insertions, 140 deletions. `291cc251` adds only a test change and documents over `8751ec2f`, so the engine code is `8751ec2f`'s. The driver's own label for the second side is `spike`, kept in the raw JSON only; in this file it is `close` |
| Binary provenance | Measured from copies in `/home/louis/ck-tmp/run-cl-close/`. `kds_server.base` sha256 `6f10c92f879928fe6d7ee4afe3eb682816f010e78ce3073b0b77a4220a561bc1`, source mtime 2026-11-06 04:29:59 UTC (built in `cl-base/build-release`; `b4a9e102` is dated 2026-11-01 08:48 UTC). `kds_server.close` sha256 `84a1ef6732a99e5be71fd39611a619c50c54e4a34b2bb0e8901d5cca1eae8181`, source mtime 2026-11-06 05:13:10 UTC (the milestone worktree's `build-release`; `291cc251` committed 05:12:37 UTC). Neither binary is older than its commit |
| Build type | Release (`CMAKE_BUILD_TYPE:STRING=Release` in both caches) |
| Device | data files and logs under `/home/louis/ck-tmp/run-cl-close/w-*` on `/dev/nvme0n1p1`, ext4 (`df -T`); not tmpfs |
| Host | Intel Xeon Platinum 8581C, 8 logical CPUs, a virtual machine |
| CPU placement | server `taskset -c 0,1` with `reactor_cpus = 0` (1 core) or `0,1` (2 cores); driver `taskset -c 2,3`. Both servers of a configuration are up together, one driven at a time |
| Server configuration | `cores` 1 or 2, `durability` `relaxed` or `group`, `buffer_pool_frames = 65536`, `log_level = warn`, every other key default; ports 17301 (base) and 17302 (close); fresh server and data file per configuration |
| Host state | Before each cell the wrapper waited for 15 s with no `ctest`, `cc1plus`, `make`, `cmake` or `kds_tests` process and logged `busy_before=no` at every start; it polled every 3 s during each cell and logged no competitor (`competition.log` was never created). Each cell's JSON records `competing: []` before and after. Load average at the cell starts: 0.26, 0.96, 1.05 and 1.66 (1 min). No other `kds_server` was up. The watcher sees process names only |
| Baseline | This shape's previous measurement is `bench/v3.4.6/results-ck-close-v3.4.6-15-g59167d99.md`. CL's base descends from CK's closing commit, so that file's closing side is the ancestor of this file's base. Its numbers appear below as context only (see "Context"); the result of CL is the close-minus-base delta measured inside this run. PostgreSQL: no twin exists, no floor |

## What was measured

The unit is one explicit transaction, `BEGIN; K x INSERT; COMMIT`, on one connection, by `tools/assertion_settle_benchmark.py` unchanged from CK's close (its docstring is its manual). The grid is CK's close grid exactly, with the same flags, so the cells compare in shape.

| Axis | Values | Mapping to the driver |
|---|---|---|
| Assertions on the relation | 0 (control), 1, 4 | relations `stl_a0`, `stl_a1`, `stl_a4`; the ceilings are loose, so one INSERT makes 0, 1 or 4 reservations |
| INSERTs per transaction K | 1, 10, 100 | every INSERT names a fresh group, so each is one new reservation per assertion |
| `durability` | `relaxed`, `group` | `--durability` |
| `cores` | 1, 2 | `--cores`, `--reactor-cpus` |
| Preload rows | 1,000 | `--rows 1000`. **200 and 10,000 were not run** (see "What was cut") |
| Windows | one warm-up round discarded, five measured rounds, side order alternating | `--rounds 5`; `--window-stmts 30000` under `relaxed` (10,000 / 2,500 / 294 transactions at K = 1 / 10 / 100) and `12000` under `group` (4,000 / 1,000 / 117); equal work on both sides |

After each configuration the driver checked row counts and that no statement was refused: `verify_problems` is `[]` in all four JSONs. The latency percentiles pool the five rounds. The noise floor comes from inside the run: the control relation (`stl_a0`) is run at every K on both sides, and CL's change cannot do anything to it except through the callback, so its delta is the floor for the asserted cells at the same K and cores. The callback is exercised by the control too, which is why a control that moved would itself be the H3 finding.

## Finding 1: the control relation did not move beyond its floor, so H3 holds

The control's `COMMIT` p50, close minus base, over the pooled five rounds: +0.3, +0.1 and +0.2 µs on one core (K = 1, 10, 100) and +0.0, +0.2 and -0.1 µs on two, all under `relaxed`, against a base `COMMIT` p50 of 17.8 to 24.9 µs (a `COMMIT` that is mostly a socket round trip). Under `group` the control moves by +9.6, +10.3 and -11.3 µs on one core and -11.6, -3.2 and -1.4 µs on two, against a base p50 of 491 to 994 µs. Two servers running the same binary would differ by this much, and the asserted cells' deltas sit among those. A `std::function` call per commit is a few nanoseconds; no cell of this run shows more than the floor, so H3 ("neither fix changes a statement on a relation with no assertion") is not contradicted at any K, either durability or either core count.

The table is the closing commit's `COMMIT` latency minus the base's. The `relaxed` verdict reads the p50 delta against the control at the same K: "inside floor" is within 0.5 µs of it. The `group` verdict reads it against the control's range over K for that core count.

| cores | durability | assertions | K | close-base p0 | close-base p25 | close-base p50 | close-base p95 | control close-base p50 (same K) | verdict |
|---|---|---|---|---|---|---|---|---|---|
| 1 | relaxed | 0 | 1 | +1.4 µs | +0.2 µs | +0.3 µs | +0.3 µs | +0.3 µs | control |
| 1 | relaxed | 1 | 1 | -0.1 µs | +0.3 µs | +0.4 µs | +1.6 µs | +0.3 µs | inside floor |
| 1 | relaxed | 4 | 1 | +0.3 µs | +0.2 µs | +0.3 µs | -0.5 µs | +0.3 µs | inside floor |
| 1 | relaxed | 0 | 10 | +0.2 µs | +0.1 µs | +0.1 µs | +0.4 µs | +0.1 µs | control |
| 1 | relaxed | 1 | 10 | +1.2 µs | +0.1 µs | +0.0 µs | -0.2 µs | +0.1 µs | inside floor |
| 1 | relaxed | 4 | 10 | -0.3 µs | +0.1 µs | +0.1 µs | +0.1 µs | +0.1 µs | inside floor |
| 1 | relaxed | 0 | 100 | +0.3 µs | +0.2 µs | +0.2 µs | +1.2 µs | +0.2 µs | control |
| 1 | relaxed | 1 | 100 | +0.2 µs | +0.0 µs | -0.1 µs | -2.7 µs | +0.2 µs | inside floor |
| 1 | relaxed | 4 | 100 | +0.0 µs | +0.2 µs | +0.4 µs | +1.9 µs | +0.2 µs | inside floor |
| 2 | relaxed | 0 | 1 | +0.9 µs | +0.0 µs | +0.0 µs | -0.7 µs | +0.0 µs | control |
| 2 | relaxed | 1 | 1 | -0.6 µs | +0.0 µs | +0.1 µs | -1.2 µs | +0.0 µs | inside floor |
| 2 | relaxed | 4 | 1 | -1.0 µs | +0.1 µs | +0.0 µs | -0.4 µs | +0.0 µs | inside floor |
| 2 | relaxed | 0 | 10 | -0.4 µs | +0.2 µs | +0.2 µs | +0.2 µs | +0.2 µs | control |
| 2 | relaxed | 1 | 10 | +1.2 µs | +0.1 µs | +0.1 µs | -0.2 µs | +0.2 µs | inside floor |
| 2 | relaxed | 4 | 10 | -2.2 µs | +0.2 µs | +0.2 µs | -0.4 µs | +0.2 µs | inside floor |
| 2 | relaxed | 0 | 100 | +0.0 µs | +0.1 µs | -0.1 µs | -1.3 µs | -0.1 µs | control |
| 2 | relaxed | 1 | 100 | +0.1 µs | +0.1 µs | +0.2 µs | +0.7 µs | -0.1 µs | inside floor |
| 2 | relaxed | 4 | 100 | +0.2 µs | +0.6 µs | +0.7 µs | -0.3 µs | -0.1 µs | inside floor on re-run (original run: +0.8 µs over the control; see "Re-runs of the 2-core `relaxed` configuration") |
| 1 | group | 0 | 1 | -9.4 µs | +6.9 µs | +9.6 µs | -8.5 µs | +9.6 µs | control |
| 1 | group | 1 | 1 | +0.6 µs | +16.7 µs | +19.9 µs | +21.5 µs | +9.6 µs | outside the control's range, sign not consistent: noise |
| 1 | group | 4 | 1 | -7.6 µs | -22.3 µs | -21.0 µs | -22.2 µs | +9.6 µs | outside the control's range, sign not consistent: noise |
| 1 | group | 0 | 10 | -4.9 µs | +8.5 µs | +10.3 µs | +2.6 µs | +10.3 µs | control |
| 1 | group | 1 | 10 | +12.9 µs | -1.2 µs | -1.4 µs | -5.0 µs | +10.3 µs | inside noise |
| 1 | group | 4 | 10 | -10.6 µs | -6.7 µs | -9.6 µs | -36.5 µs | +10.3 µs | inside noise |
| 1 | group | 0 | 100 | +9.4 µs | +0.6 µs | -11.3 µs | +12.8 µs | -11.3 µs | control |
| 1 | group | 1 | 100 | +109.5 µs | -3.3 µs | -13.2 µs | -78.5 µs | -11.3 µs | outside the control's range, sign not consistent: noise |
| 1 | group | 4 | 100 | -215.9 µs | +8.9 µs | +17.0 µs | +18.0 µs | -11.3 µs | outside the control's range, sign not consistent: noise |
| 2 | group | 0 | 1 | -2.6 µs | -7.1 µs | -11.6 µs | -8.8 µs | -11.6 µs | control |
| 2 | group | 1 | 1 | +2.9 µs | +2.8 µs | +3.1 µs | -9.0 µs | -11.6 µs | inside noise |
| 2 | group | 4 | 1 | +18.5 µs | +10.8 µs | +6.9 µs | -2.1 µs | -11.6 µs | inside noise |
| 2 | group | 0 | 10 | +3.7 µs | -5.2 µs | -3.2 µs | +2.7 µs | -3.2 µs | control |
| 2 | group | 1 | 10 | -25.1 µs | -8.1 µs | -4.2 µs | +2.5 µs | -3.2 µs | inside noise |
| 2 | group | 4 | 10 | +3.9 µs | -4.8 µs | -7.9 µs | -21.6 µs | -3.2 µs | inside noise |
| 2 | group | 0 | 100 | +22.8 µs | -5.7 µs | -1.4 µs | -24.4 µs | -1.4 µs | control |
| 2 | group | 1 | 100 | +25.5 µs | -29.7 µs | -17.9 µs | -15.1 µs | -1.4 µs | outside the control's range, sign not consistent: noise |
| 2 | group | 4 | 100 | +45.8 µs | -25.8 µs | -6.2 µs | -3.1 µs | -1.4 µs | inside noise |


Reading it. All 18 `relaxed` cells but one are inside the floor at p50, p0 (which moves by up to 2.2 µs in both directions, the control's included) and p25 (+0.0 to +0.2 µs except one cell). The exception, 2 cores with four assertions at K = 100, is +0.7 µs at p50 and +0.6 µs at p25 against a control of -0.1 µs, a gap of 0.8 µs on a 26 µs statement. It is not called a regression, and the re-runs below settle it: the same cell on one core is +0.4 against a control of +0.2, the p95 delta of the cell is -0.3 µs, the cell's minimum-wall throughput moves by -2.4% (below) against +0.6% for the 2-core control at K = 100, and CK's close run saw the 2-core control alone drift by up to 1.5 µs between two servers of one binary. In two fresh-server re-runs the cell reads -0.1 and +0.1 µs at p50 (section below), so the original +0.7 µs is drift between the two servers of that run, and the verdict is inside floor.

Under `group` the sync (about 450 to 1,200 µs) is the commit, and the deltas swing in both directions. The cells outside the control's range come in opposite signs at one K (1 core, K = 1: +19.9 µs with one assertion and -21.0 µs with four; K = 100: -13.2 with one and +17.0 with four), which a cost proportional to reservations cannot produce. No `group` cell is a finding.

## Re-runs of the 2-core `relaxed` configuration

The original run's one marginal cell (2 cores, four assertions, K = 100) was re-run by repeating the whole `c2-relaxed` configuration twice, back to back, with the same two binary copies, the same flags and placement (`--cores 2 --reactor-cpus 0,1`, server on CPUs 0,1, driver on 2,3, `--window-stmts 30000`, five measured rounds plus a warm-up, side order alternating within each run), and a fresh server and data file for each. Re-run 1 ran 05:28:31 to 05:30:29 UTC and re-run 2 05:30:44 to 05:32:45 UTC, both on 2026-11-06, with `busy_before=no`, no competitor logged by the 3 s poll, empty `competing` lists in the JSON and `verify_problems` empty. The "original" rows are the numbers of the main run above (05:17:08 to 05:19:06), unchanged.

Close minus base `COMMIT`, the marginal cell and its control (the 2-core, K = 100 control), pooled over the five measured rounds of each run:

| assertions | run | close-base p0 | close-base p25 | close-base p50 | close-base p95 | base p50 | close p50 |
|---|---|---|---|---|---|---|---|
| 4 | original | +0.2 µs | +0.6 µs | +0.7 µs | -0.3 µs | 26.0 µs | 26.7 µs |
| 4 | re-run 1 | -0.2 µs | +0.0 µs | -0.1 µs | -1.6 µs | 26.4 µs | 26.3 µs |
| 4 | re-run 2 | +0.0 µs | +0.1 µs | +0.1 µs | -0.8 µs | 26.5 µs | 26.6 µs |
| 0 (control) | original | +0.0 µs | +0.1 µs | -0.1 µs | -1.3 µs | 24.9 µs | 24.8 µs |
| 0 (control) | re-run 1 | +0.0 µs | -0.2 µs | -0.3 µs | -0.9 µs | 25.0 µs | 24.7 µs |
| 0 (control) | re-run 2 | -0.2 µs | -0.3 µs | -0.3 µs | -2.7 µs | 25.4 µs | 25.1 µs |

The p50 delta of every 2-core `relaxed` cell in the three runs, so the reader can see whether the +0.7 recurs:

| assertions | K | original | re-run 1 | re-run 2 |
|---|---|---|---|---|
| 0 (control) | 1 | +0.0 µs | +0.1 µs | +0.4 µs |
| 1 | 1 | +0.1 µs | +0.1 µs | +0.5 µs |
| 4 | 1 | +0.0 µs | +0.2 µs | +0.3 µs |
| 0 (control) | 10 | +0.2 µs | +0.0 µs | +0.0 µs |
| 1 | 10 | +0.1 µs | +0.1 µs | +0.4 µs |
| 4 | 10 | +0.2 µs | +0.2 µs | +0.1 µs |
| 0 (control) | 100 | -0.1 µs | -0.3 µs | -0.3 µs |
| 1 | 100 | +0.2 µs | -0.5 µs | -0.8 µs |
| 4 | 100 | +0.7 µs | -0.1 µs | +0.1 µs |

The +0.7 does not recur: the same cell is -0.1 and +0.1 µs in the re-runs, against controls of -0.3 µs in both, a gap of +0.2 and +0.4 µs, inside the 0.5 µs floor. Across the 27 cell-runs the 2-core deltas span -0.8 to +0.7 µs with no sign tied to the number of assertions or to K (the largest in either direction are one assertion at K = 100, +0.2 / -0.5 / -0.8 µs, and four assertions at K = 100, +0.7 / -0.1 / +0.1 µs), which is the drift between two servers of one purpose that CK also saw. The verdict for the cell is **inside floor**; the original run's marginal reading is the run's own spread, reported as measured above and not a finding. The re-runs are not pooled with the original run; each stands as its own run of one engine state.

## Full COMMIT percentile table

The `COMMIT` statement's wall latency as the client saw it, all five measured rounds pooled, warm-up excluded. `commits` is the operation count. The `group` p95 and p99 carry single large outliers.

| cores | durability | assertions | K | side | commits | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | relaxed | 0 | 1 | base | 50000 | 13.6 µs | 17.6 µs | 17.8 µs | 21.3 µs | 25.7 µs |
| 1 | relaxed | 0 | 1 | close | 50000 | 15.0 µs | 17.8 µs | 18.1 µs | 21.6 µs | 26.3 µs |
| 1 | relaxed | 1 | 1 | base | 50000 | 14.0 µs | 17.6 µs | 17.8 µs | 20.7 µs | 24.9 µs |
| 1 | relaxed | 1 | 1 | close | 50000 | 13.9 µs | 17.9 µs | 18.2 µs | 22.3 µs | 25.6 µs |
| 1 | relaxed | 4 | 1 | base | 50000 | 13.6 µs | 17.7 µs | 18.0 µs | 22.5 µs | 26.9 µs |
| 1 | relaxed | 4 | 1 | close | 50000 | 13.9 µs | 17.9 µs | 18.3 µs | 22.0 µs | 29.2 µs |
| 1 | relaxed | 0 | 10 | base | 12500 | 17.2 µs | 18.1 µs | 18.3 µs | 21.9 µs | 26.6 µs |
| 1 | relaxed | 0 | 10 | close | 12500 | 17.4 µs | 18.2 µs | 18.4 µs | 22.3 µs | 26.8 µs |
| 1 | relaxed | 1 | 10 | base | 12500 | 16.0 µs | 18.3 µs | 18.7 µs | 23.0 µs | 29.6 µs |
| 1 | relaxed | 1 | 10 | close | 12500 | 17.2 µs | 18.4 µs | 18.7 µs | 22.8 µs | 28.4 µs |
| 1 | relaxed | 4 | 10 | base | 12500 | 17.1 µs | 18.3 µs | 18.6 µs | 22.8 µs | 27.8 µs |
| 1 | relaxed | 4 | 10 | close | 12500 | 16.8 µs | 18.4 µs | 18.7 µs | 22.9 µs | 28.4 µs |
| 1 | relaxed | 0 | 100 | base | 1470 | 21.4 µs | 22.6 µs | 23.2 µs | 29.6 µs | 36.2 µs |
| 1 | relaxed | 0 | 100 | close | 1470 | 21.7 µs | 22.8 µs | 23.4 µs | 30.8 µs | 41.5 µs |
| 1 | relaxed | 1 | 100 | base | 1470 | 21.6 µs | 23.0 µs | 23.8 µs | 33.6 µs | 41.5 µs |
| 1 | relaxed | 1 | 100 | close | 1470 | 21.8 µs | 23.0 µs | 23.7 µs | 30.9 µs | 37.4 µs |
| 1 | relaxed | 4 | 100 | base | 1470 | 22.0 µs | 23.3 µs | 24.1 µs | 31.4 µs | 40.4 µs |
| 1 | relaxed | 4 | 100 | close | 1470 | 22.0 µs | 23.5 µs | 24.5 µs | 33.3 µs | 43.4 µs |
| 2 | relaxed | 0 | 1 | base | 50000 | 13.8 µs | 17.7 µs | 18.0 µs | 22.2 µs | 26.3 µs |
| 2 | relaxed | 0 | 1 | close | 50000 | 14.7 µs | 17.7 µs | 18.0 µs | 21.5 µs | 25.2 µs |
| 2 | relaxed | 1 | 1 | base | 50000 | 14.4 µs | 17.7 µs | 17.9 µs | 22.2 µs | 25.5 µs |
| 2 | relaxed | 1 | 1 | close | 50000 | 13.8 µs | 17.7 µs | 18.0 µs | 21.0 µs | 25.4 µs |
| 2 | relaxed | 4 | 1 | base | 50000 | 15.5 µs | 17.7 µs | 18.0 µs | 21.8 µs | 25.6 µs |
| 2 | relaxed | 4 | 1 | close | 50000 | 14.5 µs | 17.8 µs | 18.0 µs | 21.4 µs | 25.5 µs |
| 2 | relaxed | 0 | 10 | base | 12500 | 15.9 µs | 18.2 µs | 18.5 µs | 22.2 µs | 26.5 µs |
| 2 | relaxed | 0 | 10 | close | 12500 | 15.5 µs | 18.4 µs | 18.7 µs | 22.4 µs | 26.4 µs |
| 2 | relaxed | 1 | 10 | base | 12500 | 15.9 µs | 18.4 µs | 18.7 µs | 22.8 µs | 26.8 µs |
| 2 | relaxed | 1 | 10 | close | 12500 | 17.1 µs | 18.5 µs | 18.8 µs | 22.6 µs | 26.4 µs |
| 2 | relaxed | 4 | 10 | base | 12500 | 17.4 µs | 18.5 µs | 18.8 µs | 23.4 µs | 28.3 µs |
| 2 | relaxed | 4 | 10 | close | 12500 | 15.2 µs | 18.7 µs | 19.0 µs | 23.0 µs | 26.9 µs |
| 2 | relaxed | 0 | 100 | base | 1470 | 23.1 µs | 24.2 µs | 24.9 µs | 32.1 µs | 37.5 µs |
| 2 | relaxed | 0 | 100 | close | 1470 | 23.1 µs | 24.3 µs | 24.8 µs | 30.8 µs | 36.5 µs |
| 2 | relaxed | 1 | 100 | base | 1470 | 23.3 µs | 24.6 µs | 25.2 µs | 32.1 µs | 38.7 µs |
| 2 | relaxed | 1 | 100 | close | 1470 | 23.4 µs | 24.7 µs | 25.4 µs | 32.8 µs | 40.3 µs |
| 2 | relaxed | 4 | 100 | base | 1470 | 23.7 µs | 25.1 µs | 26.0 µs | 34.3 µs | 43.1 µs |
| 2 | relaxed | 4 | 100 | close | 1470 | 23.9 µs | 25.7 µs | 26.7 µs | 34.0 µs | 41.8 µs |
| 1 | group | 0 | 1 | base | 20000 | 296.1 µs | 439.6 µs | 490.8 µs | 701.7 µs | 876.9 µs |
| 1 | group | 0 | 1 | close | 20000 | 286.7 µs | 446.5 µs | 500.4 µs | 693.2 µs | 841.4 µs |
| 1 | group | 1 | 1 | base | 20000 | 275.5 µs | 400.6 µs | 451.0 µs | 648.0 µs | 791.9 µs |
| 1 | group | 1 | 1 | close | 20000 | 276.1 µs | 417.3 µs | 470.9 µs | 669.5 µs | 819.6 µs |
| 1 | group | 4 | 1 | base | 20000 | 271.7 µs | 432.1 µs | 485.6 µs | 688.2 µs | 843.1 µs |
| 1 | group | 4 | 1 | close | 20000 | 264.1 µs | 409.8 µs | 464.6 µs | 666.0 µs | 834.4 µs |
| 1 | group | 0 | 10 | base | 5000 | 314.2 µs | 483.5 µs | 546.6 µs | 766.3 µs | 924.2 µs |
| 1 | group | 0 | 10 | close | 5000 | 309.3 µs | 492.0 µs | 556.9 µs | 768.9 µs | 895.6 µs |
| 1 | group | 1 | 10 | base | 5000 | 350.1 µs | 501.8 µs | 566.1 µs | 786.8 µs | 936.3 µs |
| 1 | group | 1 | 10 | close | 5000 | 363.0 µs | 500.6 µs | 564.7 µs | 781.8 µs | 928.0 µs |
| 1 | group | 4 | 10 | base | 5000 | 361.3 µs | 521.7 µs | 592.3 µs | 839.8 µs | 1089.4 µs |
| 1 | group | 4 | 10 | close | 5000 | 350.7 µs | 515.0 µs | 582.7 µs | 803.3 µs | 966.7 µs |
| 1 | group | 0 | 100 | base | 585 | 656.6 µs | 826.3 µs | 912.9 µs | 1141.6 µs | 1307.4 µs |
| 1 | group | 0 | 100 | close | 585 | 666.0 µs | 826.9 µs | 901.6 µs | 1154.4 µs | 1297.5 µs |
| 1 | group | 1 | 100 | base | 585 | 609.3 µs | 928.8 µs | 999.4 µs | 1287.0 µs | 27416.7 µs |
| 1 | group | 1 | 100 | close | 585 | 718.8 µs | 925.5 µs | 986.2 µs | 1208.5 µs | 1577.9 µs |
| 1 | group | 4 | 100 | base | 585 | 872.6 µs | 1072.2 µs | 1159.0 µs | 1404.9 µs | 2458.0 µs |
| 1 | group | 4 | 100 | close | 585 | 656.7 µs | 1081.1 µs | 1176.0 µs | 1422.9 µs | 3396.3 µs |
| 2 | group | 0 | 1 | base | 20000 | 288.4 µs | 462.7 µs | 523.4 µs | 737.0 µs | 896.2 µs |
| 2 | group | 0 | 1 | close | 20000 | 285.8 µs | 455.6 µs | 511.8 µs | 728.2 µs | 909.1 µs |
| 2 | group | 1 | 1 | base | 20000 | 290.7 µs | 436.6 µs | 491.4 µs | 698.0 µs | 834.2 µs |
| 2 | group | 1 | 1 | close | 20000 | 293.6 µs | 439.4 µs | 494.5 µs | 689.0 µs | 829.6 µs |
| 2 | group | 4 | 1 | base | 20000 | 286.7 µs | 452.5 µs | 516.3 µs | 737.4 µs | 907.4 µs |
| 2 | group | 4 | 1 | close | 20000 | 305.2 µs | 463.3 µs | 523.2 µs | 735.3 µs | 883.4 µs |
| 2 | group | 0 | 10 | base | 5000 | 354.6 µs | 529.6 µs | 593.3 µs | 817.7 µs | 995.5 µs |
| 2 | group | 0 | 10 | close | 5000 | 358.3 µs | 524.4 µs | 590.1 µs | 820.4 µs | 992.2 µs |
| 2 | group | 1 | 10 | base | 5000 | 390.3 µs | 545.7 µs | 610.0 µs | 830.4 µs | 1029.8 µs |
| 2 | group | 1 | 10 | close | 5000 | 365.2 µs | 537.6 µs | 605.8 µs | 832.9 µs | 1022.9 µs |
| 2 | group | 4 | 10 | base | 5000 | 390.9 µs | 587.7 µs | 659.8 µs | 902.5 µs | 1085.9 µs |
| 2 | group | 4 | 10 | close | 5000 | 394.8 µs | 582.9 µs | 651.9 µs | 880.9 µs | 1044.2 µs |
| 2 | group | 0 | 100 | base | 585 | 667.8 µs | 921.5 µs | 994.2 µs | 1256.7 µs | 8774.5 µs |
| 2 | group | 0 | 100 | close | 585 | 690.6 µs | 915.8 µs | 992.8 µs | 1232.3 µs | 1349.8 µs |
| 2 | group | 1 | 100 | base | 585 | 739.7 µs | 988.6 µs | 1050.4 µs | 1297.6 µs | 1563.9 µs |
| 2 | group | 1 | 100 | close | 585 | 765.2 µs | 958.9 µs | 1032.5 µs | 1282.5 µs | 1645.4 µs |
| 2 | group | 4 | 100 | base | 585 | 569.4 µs | 1120.3 µs | 1184.9 µs | 1458.9 µs | 7179.0 µs |
| 2 | group | 4 | 100 | close | 585 | 615.2 µs | 1094.5 µs | 1178.7 µs | 1455.8 µs | 1830.6 µs |


## Where the time goes

A transaction's wall time is a sum of statements; the table gives the p50 of each statement and their sum, for the control and four assertions at K = 1 and 100. (Means are not used: the pooled means carry the slow windows of this host, one window in five or six at up to twice the cost on either binary, and they invert signs between cells.) CL touches only the `COMMIT` column.

| Wait type | Measured here? |
|---|---|
| Client and socket round trip | Yes, indirectly: a statement's p0 to p50 on a nearly empty path is 14 to 20 µs and is nearly all round trip (`BEGIN` p50 and `COMMIT` p50 of the control under `relaxed`) |
| Write statement (INSERT) | Yes, the K x INSERT column |
| Durability/commit (fsync) | Yes, as the difference between a `group` and a `relaxed` COMMIT: about 450 to 500 µs at p50 at K = 1 against 18 µs |
| Commit callback (`after_logged`) | Not separable: it is a call of a few nanoseconds inside the commit, below the 0.1 µs resolution of the driver's clock; its bound is the control's delta, +0.3 µs or less at p50 under `relaxed` |
| Checkpoint `ASSERT_PENDING` records | Not on the statement path; nothing is open at a checkpoint in this driver, so none is written |
| Read | None, no SELECT is in the unit |
| Lock or conflict wait | None: one connection, and the driver counted zero refused statements |

| cores | durability | assertions | K | side | BEGIN p50 | INSERT p50 | K x INSERT p50 | COMMIT p50 | sum of p50s |
|---|---|---|---|---|---|---|---|---|---|
| 1 | relaxed | 0 | 1 | base | 18.2 µs | 27.9 µs | 27.9 µs | 17.8 µs | 63.9 µs |
| 1 | relaxed | 0 | 1 | close | 18.5 µs | 27.9 µs | 27.9 µs | 18.1 µs | 64.5 µs |
| 1 | relaxed | 4 | 1 | base | 18.3 µs | 32.3 µs | 32.3 µs | 18.0 µs | 68.6 µs |
| 1 | relaxed | 4 | 1 | close | 18.6 µs | 32.5 µs | 32.5 µs | 18.3 µs | 69.4 µs |
| 1 | relaxed | 0 | 100 | base | 19.0 µs | 27.3 µs | 2730.0 µs | 23.2 µs | 2772.2 µs |
| 1 | relaxed | 0 | 100 | close | 19.2 µs | 27.6 µs | 2760.0 µs | 23.4 µs | 2802.6 µs |
| 1 | relaxed | 4 | 100 | base | 19.5 µs | 31.4 µs | 3140.0 µs | 24.1 µs | 3183.6 µs |
| 1 | relaxed | 4 | 100 | close | 19.5 µs | 31.5 µs | 3150.0 µs | 24.5 µs | 3194.0 µs |
| 2 | relaxed | 0 | 1 | base | 18.5 µs | 28.4 µs | 28.4 µs | 18.0 µs | 64.9 µs |
| 2 | relaxed | 0 | 1 | close | 18.3 µs | 28.4 µs | 28.4 µs | 18.0 µs | 64.7 µs |
| 2 | relaxed | 4 | 1 | base | 18.4 µs | 33.0 µs | 33.0 µs | 18.0 µs | 69.4 µs |
| 2 | relaxed | 4 | 1 | close | 18.4 µs | 33.1 µs | 33.1 µs | 18.0 µs | 69.5 µs |
| 2 | relaxed | 0 | 100 | base | 19.3 µs | 28.0 µs | 2800.0 µs | 24.9 µs | 2844.2 µs |
| 2 | relaxed | 0 | 100 | close | 19.3 µs | 27.9 µs | 2790.0 µs | 24.8 µs | 2834.1 µs |
| 2 | relaxed | 4 | 100 | base | 19.6 µs | 32.2 µs | 3220.0 µs | 26.0 µs | 3265.6 µs |
| 2 | relaxed | 4 | 100 | close | 19.8 µs | 32.4 µs | 3240.0 µs | 26.7 µs | 3286.5 µs |
| 1 | group | 0 | 1 | base | 24.7 µs | 48.9 µs | 48.9 µs | 490.8 µs | 564.4 µs |
| 1 | group | 0 | 1 | close | 25.0 µs | 49.1 µs | 49.1 µs | 500.4 µs | 574.5 µs |
| 1 | group | 4 | 1 | base | 24.8 µs | 56.2 µs | 56.2 µs | 485.6 µs | 566.6 µs |
| 1 | group | 4 | 1 | close | 24.6 µs | 55.3 µs | 55.3 µs | 464.6 µs | 544.5 µs |
| 1 | group | 0 | 100 | base | 24.2 µs | 27.6 µs | 2760.0 µs | 912.9 µs | 3697.1 µs |
| 1 | group | 0 | 100 | close | 24.8 µs | 27.9 µs | 2790.0 µs | 901.6 µs | 3716.4 µs |
| 1 | group | 4 | 100 | base | 25.8 µs | 31.9 µs | 3190.0 µs | 1159.0 µs | 4374.8 µs |
| 1 | group | 4 | 100 | close | 26.0 µs | 32.1 µs | 3210.0 µs | 1176.0 µs | 4412.0 µs |
| 2 | group | 0 | 1 | base | 29.5 µs | 51.6 µs | 51.6 µs | 523.4 µs | 604.5 µs |
| 2 | group | 0 | 1 | close | 30.2 µs | 51.9 µs | 51.9 µs | 511.8 µs | 593.9 µs |
| 2 | group | 4 | 1 | base | 28.7 µs | 59.0 µs | 59.0 µs | 516.3 µs | 604.0 µs |
| 2 | group | 4 | 1 | close | 29.2 µs | 59.3 µs | 59.3 µs | 523.2 µs | 611.7 µs |
| 2 | group | 0 | 100 | base | 31.8 µs | 28.4 µs | 2840.0 µs | 994.2 µs | 3866.0 µs |
| 2 | group | 0 | 100 | close | 32.1 µs | 28.4 µs | 2840.0 µs | 992.8 µs | 3864.9 µs |
| 2 | group | 4 | 100 | base | 31.0 µs | 33.0 µs | 3300.0 µs | 1184.9 µs | 4515.9 µs |
| 2 | group | 4 | 100 | close | 30.2 µs | 33.0 µs | 3300.0 µs | 1178.7 µs | 4508.9 µs |


## Throughput

QPS here is transactions per second. The pooled means and the per-round medians are unusable (the windows that ran in the slow state, on either side, move a cell by tens of percent; per-round close-vs-base ranges below span -74% to +261%). The stable form is each side's best round: the table is `1,000,000 / min wall-per-transaction` (derived, not reported by the driver), and the CPU column is the server's CPU per transaction at its best round (a 10 ms tick over a window: 3.3 to 85 µs of rounding). The last column is how many of the five rounds sit within 1.15x of the best.

| cores | durability | assertions | K | base CPU/txn min | close CPU/txn min | delta | base CPU med | close CPU med | base wall/txn min | close wall/txn min | txn/s from min wall (base -> close) | rounds within 1.15x min (base,close) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | relaxed | 0 | 1 | 29.0 | 29.0 | +0.0 | 30.0 | 30.0 | 66.5 | 67.0 | 15034 -> 14917 | 3,3 |
| 1 | relaxed | 1 | 1 | 30.0 | 31.0 | +1.0 | 31.0 | 32.0 | 67.8 | 68.8 | 14753 -> 14533 | 4,5 |
| 1 | relaxed | 4 | 1 | 33.0 | 34.0 | +1.0 | 35.0 | 72.0 | 71.4 | 71.9 | 13997 -> 13910 | 3,2 |
| 1 | relaxed | 0 | 10 | 144.0 | 148.0 | +4.0 | 148.0 | 148.0 | 321.2 | 324.5 | 3113 -> 3082 | 5,5 |
| 1 | relaxed | 1 | 10 | 160.0 | 160.0 | -0.0 | 280.0 | 164.0 | 342.1 | 341.8 | 2923 -> 2926 | 2,3 |
| 1 | relaxed | 4 | 10 | 188.0 | 188.0 | +0.0 | 200.0 | 204.0 | 371.9 | 370.3 | 2689 -> 2701 | 4,2 |
| 1 | relaxed | 0 | 100 | 1292.5 | 1360.5 | +68.0 | 1428.6 | 1496.6 | 2865.0 | 2971.1 | 349 -> 337 | 3,3 |
| 1 | relaxed | 1 | 100 | 1462.6 | 1462.6 | +0.0 | 2108.8 | 1530.6 | 3048.4 | 3052.0 | 328 -> 328 | 2,5 |
| 1 | relaxed | 4 | 100 | 1734.7 | 1734.7 | +0.0 | 1802.7 | 1802.7 | 3342.6 | 3333.9 | 299 -> 300 | 4,4 |
| 2 | relaxed | 0 | 1 | 31.0 | 31.0 | +0.0 | 33.0 | 33.0 | 67.8 | 67.0 | 14751 -> 14917 | 3,4 |
| 2 | relaxed | 1 | 1 | 33.0 | 33.0 | +0.0 | 34.0 | 34.0 | 68.9 | 69.1 | 14520 -> 14471 | 4,5 |
| 2 | relaxed | 4 | 1 | 36.0 | 36.0 | -0.0 | 39.0 | 37.0 | 72.1 | 72.5 | 13869 -> 13791 | 2,5 |
| 2 | relaxed | 0 | 10 | 156.0 | 160.0 | +4.0 | 164.0 | 172.0 | 325.1 | 330.3 | 3076 -> 3028 | 3,4 |
| 2 | relaxed | 1 | 10 | 180.0 | 184.0 | +4.0 | 188.0 | 184.0 | 347.1 | 345.9 | 2881 -> 2891 | 3,3 |
| 2 | relaxed | 4 | 10 | 224.0 | 220.0 | -4.0 | 276.0 | 224.0 | 391.3 | 385.5 | 2556 -> 2594 | 3,3 |
| 2 | relaxed | 0 | 100 | 1428.6 | 1462.6 | +34.0 | 1496.6 | 1462.6 | 2972.8 | 2956.5 | 336 -> 338 | 5,4 |
| 2 | relaxed | 1 | 100 | 1598.6 | 1598.6 | -0.0 | 1700.7 | 1632.7 | 3126.1 | 3133.6 | 320 -> 319 | 3,5 |
| 2 | relaxed | 4 | 100 | 1938.8 | 2108.8 | +170.1 | 2006.8 | 2619.0 | 3447.1 | 3535.3 | 290 -> 283 | 4,2 |
| 1 | group | 0 | 1 | 70.0 | 72.5 | +2.5 | 72.5 | 77.5 | 580.6 | 597.3 | 1722 -> 1674 | 4,5 |
| 1 | group | 1 | 1 | 72.5 | 72.5 | +0.0 | 72.5 | 92.5 | 528.1 | 561.3 | 1894 -> 1781 | 5,4 |
| 1 | group | 4 | 1 | 77.5 | 72.5 | -5.0 | 82.5 | 100.0 | 606.7 | 537.0 | 1648 -> 1862 | 4,2 |
| 1 | group | 0 | 10 | 190.0 | 190.0 | -0.0 | 190.0 | 190.0 | 885.6 | 877.2 | 1129 -> 1140 | 4,5 |
| 1 | group | 1 | 10 | 210.0 | 200.0 | -10.0 | 220.0 | 210.0 | 923.0 | 937.0 | 1083 -> 1067 | 4,5 |
| 1 | group | 4 | 10 | 240.0 | 240.0 | -0.0 | 290.0 | 250.0 | 1010.2 | 955.9 | 990 -> 1046 | 3,4 |
| 1 | group | 0 | 100 | 1367.5 | 1282.1 | -85.5 | 1367.5 | 1367.5 | 3774.7 | 3775.3 | 265 -> 265 | 5,5 |
| 1 | group | 1 | 100 | 1538.5 | 1453.0 | -85.5 | 1623.9 | 1538.5 | 4077.5 | 4047.9 | 245 -> 247 | 3,4 |
| 1 | group | 4 | 100 | 1794.9 | 1880.3 | +85.5 | 2136.8 | 1965.8 | 4529.1 | 4618.5 | 221 -> 217 | 2,3 |
| 2 | group | 0 | 1 | 100.0 | 90.0 | -10.0 | 120.0 | 130.0 | 615.8 | 586.2 | 1624 -> 1706 | 4,3 |
| 2 | group | 1 | 1 | 95.0 | 95.0 | +0.0 | 125.0 | 97.5 | 602.5 | 572.6 | 1660 -> 1747 | 5,5 |
| 2 | group | 4 | 1 | 112.5 | 105.0 | -7.5 | 135.0 | 110.0 | 678.5 | 599.7 | 1474 -> 1668 | 3,5 |
| 2 | group | 0 | 10 | 240.0 | 240.0 | -0.0 | 240.0 | 240.0 | 964.2 | 966.2 | 1037 -> 1035 | 4,4 |
| 2 | group | 1 | 10 | 250.0 | 260.0 | +10.0 | 260.0 | 290.0 | 970.3 | 966.0 | 1031 -> 1035 | 3,2 |
| 2 | group | 4 | 10 | 300.0 | 290.0 | -10.0 | 340.0 | 310.0 | 1136.6 | 1073.2 | 880 -> 932 | 5,4 |
| 2 | group | 0 | 100 | 1453.0 | 1538.5 | +85.5 | 1623.9 | 1623.9 | 4029.8 | 3975.5 | 248 -> 252 | 4,5 |
| 2 | group | 1 | 100 | 1709.4 | 1709.4 | +0.0 | 1709.4 | 1709.4 | 4268.7 | 4201.0 | 234 -> 238 | 4,4 |
| 2 | group | 4 | 100 | 2136.8 | 2051.3 | -85.5 | 2222.2 | 2136.8 | 4858.4 | 4733.8 | 206 -> 211 | 3,3 |


Per-round throughput as the driver reports it (median of rounds, close vs base, and the range of the per-round delta), for completeness; it is dominated by the slow windows and resolves nothing:

| cores | durability | assertions | K | base txn/s | close txn/s | close vs base | per-round delta range % |
|---|---|---|---|---|---|---|---|
| 1 | relaxed | 0 | 1 | 14869 | 14529 | -2.3% | -74.2% .. +134.8% |
| 1 | relaxed | 1 | 1 | 14608 | 14323 | -2.0% | -3.1% .. +36.1% |
| 1 | relaxed | 4 | 1 | 13392 | 7990 | -40.3% | -69.0% .. +102.5% |
| 1 | relaxed | 0 | 10 | 3021 | 3036 | +0.5% | -3.8% .. +3.4% |
| 1 | relaxed | 1 | 10 | 1779 | 2877 | +61.7% | -54.6% .. +124.6% |
| 1 | relaxed | 4 | 10 | 2567 | 2255 | -12.2% | -31.7% .. +101.5% |
| 1 | relaxed | 0 | 100 | 325 | 294 | -9.5% | -48.8% .. +128.6% |
| 1 | relaxed | 1 | 100 | 228 | 316 | +38.5% | -0.1% .. +123.4% |
| 1 | relaxed | 4 | 100 | 295 | 285 | -3.1% | -55.5% .. +38.9% |
| 2 | relaxed | 0 | 1 | 14685 | 14457 | -1.5% | -2.0% .. +156.4% |
| 2 | relaxed | 1 | 1 | 14480 | 14294 | -1.3% | -5.4% .. +261.1% |
| 2 | relaxed | 4 | 1 | 11857 | 13670 | +15.3% | -2.1% .. +142.8% |
| 2 | relaxed | 0 | 10 | 3024 | 2972 | -1.7% | -14.6% .. +167.4% |
| 2 | relaxed | 1 | 10 | 2725 | 2792 | +2.5% | -18.4% .. +54.5% |
| 2 | relaxed | 4 | 10 | 2270 | 2491 | +9.7% | -16.9% .. +146.8% |
| 2 | relaxed | 0 | 100 | 332 | 333 | +0.5% | -59.8% .. +3.7% |
| 2 | relaxed | 1 | 100 | 308 | 314 | +1.8% | -0.2% .. +128.9% |
| 2 | relaxed | 4 | 100 | 286 | 246 | -14.1% | -50.0% .. +1.2% |
| 1 | group | 0 | 1 | 1660 | 1537 | -7.4% | -7.4% .. +8.3% |
| 1 | group | 1 | 1 | 1766 | 1664 | -5.8% | -38.6% .. +6.8% |
| 1 | group | 4 | 1 | 1627 | 1611 | -0.9% | -24.7% .. +33.4% |
| 1 | group | 0 | 10 | 1111 | 1082 | -2.6% | -2.6% .. +112.3% |
| 1 | group | 1 | 10 | 1055 | 1053 | -0.2% | -1.5% .. +11.2% |
| 1 | group | 4 | 10 | 924 | 998 | +7.9% | -50.4% .. +123.2% |
| 1 | group | 0 | 100 | 258 | 256 | -0.7% | -4.0% .. +1.8% |
| 1 | group | 1 | 100 | 240 | 243 | +1.1% | -1.3% .. +234.3% |
| 1 | group | 4 | 100 | 181 | 189 | +4.6% | -14.7% .. +115.7% |
| 2 | group | 0 | 1 | 1516 | 1526 | +0.6% | -29.2% .. +26.0% |
| 2 | group | 1 | 1 | 1578 | 1649 | +4.5% | -3.8% .. +13.1% |
| 2 | group | 4 | 1 | 1417 | 1483 | +4.7% | -0.8% .. +46.6% |
| 2 | group | 0 | 10 | 990 | 989 | -0.1% | -5.5% .. +4.2% |
| 2 | group | 1 | 10 | 947 | 782 | -17.4% | -17.8% .. +16.7% |
| 2 | group | 4 | 10 | 861 | 878 | +2.0% | -42.7% .. +8.7% |
| 2 | group | 0 | 100 | 244 | 240 | -1.7% | -1.8% .. +248.7% |
| 2 | group | 1 | 100 | 233 | 233 | +0.1% | -0.4% .. +3.9% |
| 2 | group | 4 | 100 | 205 | 198 | -3.7% | -3.8% .. +2.7% |


At the best round, close and base throughput agree within 2% in every `relaxed` cell except two: the control at K = 100 on one core (349 to 337 txn/s, -3.4%, while the same K with one and four assertions moves by 0.0% and +0.3%), which sets the throughput floor at about 3%, and 2 cores with four assertions at K = 100 (290 to 283, -2.4%), inside that floor. The `group` cells move by up to 13% in both directions (best-round txn/s, sync-bound) and are not read. Server CPU per transaction resolves nothing: the deltas are zero or one to a few ticks of the 10 ms CPU clock and carry both signs (the one cell with +170 µs, 2 cores with four assertions at K = 100, is the same cell as the marginal p50 above, and the cell's median CPU is +612 µs higher, which the slow-state rounds produce; the 1-core cell is +0.0). It is mentioned here, not claimed.

## Context: CL's base against CK's close

The previous measurement of this shape is `bench/v3.4.6/results-ck-close-v3.4.6-15-g59167d99.md`. Its closing side, `59167d99`, is an ancestor of CL's base. The table sets this run's base `COMMIT` p50 beside that file's closing side p50 for the same cell. It is context only and not CL's result: two runs on two days differ by the host's state, and under `relaxed` this run's base is 2.0 to 8.3 µs shorter than CK's close in every cell, much more than any delta between the two sides inside either run. That alone is why a cross-run comparison is not used for CL; only the interleaved delta is.

| cores | durability | assertions | K | CL base p50 | CK close p50 | CL base - CK close |
|---|---|---|---|---|---|---|
| 1 | relaxed | 0 | 1 | 17.8 µs | 20.1 µs | -2.3 µs |
| 1 | relaxed | 1 | 1 | 17.8 µs | 20.1 µs | -2.3 µs |
| 1 | relaxed | 4 | 1 | 18.0 µs | 20.0 µs | -2.0 µs |
| 1 | relaxed | 0 | 10 | 18.3 µs | 20.7 µs | -2.4 µs |
| 1 | relaxed | 1 | 10 | 18.7 µs | 20.9 µs | -2.2 µs |
| 1 | relaxed | 4 | 10 | 18.6 µs | 21.1 µs | -2.5 µs |
| 1 | relaxed | 0 | 100 | 23.2 µs | 26.8 µs | -3.6 µs |
| 1 | relaxed | 1 | 100 | 23.8 µs | 27.4 µs | -3.6 µs |
| 1 | relaxed | 4 | 100 | 24.1 µs | 28.8 µs | -4.7 µs |
| 2 | relaxed | 0 | 1 | 18.0 µs | 22.1 µs | -4.1 µs |
| 2 | relaxed | 1 | 1 | 17.9 µs | 24.1 µs | -6.2 µs |
| 2 | relaxed | 4 | 1 | 18.0 µs | 24.1 µs | -6.1 µs |
| 2 | relaxed | 0 | 10 | 18.5 µs | 23.3 µs | -4.8 µs |
| 2 | relaxed | 1 | 10 | 18.7 µs | 25.1 µs | -6.4 µs |
| 2 | relaxed | 4 | 10 | 18.8 µs | 24.8 µs | -6.0 µs |
| 2 | relaxed | 0 | 100 | 24.9 µs | 32.0 µs | -7.1 µs |
| 2 | relaxed | 1 | 100 | 25.2 µs | 33.5 µs | -8.3 µs |
| 2 | relaxed | 4 | 100 | 26.0 µs | 33.7 µs | -7.7 µs |
| 1 | group | 0 | 1 | 490.8 µs | 463.0 µs | +27.8 µs |
| 1 | group | 1 | 1 | 451.0 µs | 476.6 µs | -25.6 µs |
| 1 | group | 4 | 1 | 485.6 µs | 459.8 µs | +25.8 µs |
| 1 | group | 0 | 10 | 546.6 µs | 537.8 µs | +8.8 µs |
| 1 | group | 1 | 10 | 566.1 µs | 571.5 µs | -5.4 µs |
| 1 | group | 4 | 10 | 592.3 µs | 630.1 µs | -37.8 µs |
| 1 | group | 0 | 100 | 912.9 µs | 900.1 µs | +12.8 µs |
| 1 | group | 1 | 100 | 999.4 µs | 1002.6 µs | -3.2 µs |
| 1 | group | 4 | 100 | 1159.0 µs | 1146.8 µs | +12.2 µs |
| 2 | group | 0 | 1 | 523.4 µs | 550.7 µs | -27.3 µs |
| 2 | group | 1 | 1 | 491.4 µs | 523.3 µs | -31.9 µs |
| 2 | group | 4 | 1 | 516.3 µs | 507.8 µs | +8.5 µs |
| 2 | group | 0 | 10 | 593.3 µs | 637.4 µs | -44.1 µs |
| 2 | group | 1 | 10 | 610.0 µs | 647.2 µs | -37.2 µs |
| 2 | group | 4 | 10 | 659.8 µs | 672.0 µs | -12.2 µs |
| 2 | group | 0 | 100 | 994.2 µs | 941.4 µs | +52.8 µs |
| 2 | group | 1 | 100 | 1050.4 µs | 977.7 µs | +72.7 µs |
| 2 | group | 4 | 100 | 1184.9 µs | 1134.1 µs | +50.8 µs |


## What this teaches about the engine

The callback added to every commit is free at the resolution this driver has. The p50 `COMMIT` of the control relation, the only relation on which CL can have added a cost without removing one, moved by at most +0.3 µs, and the asserted relations, which CL's change also touches (dropping the pending set after the record rather than before), moved by no more than the control did, the one original-run exception having read inside the floor in two re-runs. That follows from where the work is: a `COMMIT` under `relaxed` is 18 to 26 µs of socket round trip and a log append, and a `std::function` dispatch is a different order of magnitude. Under `group` the commit is the sync (450 to 1,200 µs at p50), and this run shows it unaffected beyond its noise; the strict and deferred-strict paths were not run.

The checkpoint's `ASSERT_PENDING` records are priced at zero here because the driver keeps nothing open across a checkpoint; a workload with long transactions holding many reservations would write one record per reservation at every checkpoint, and this run does not speak to that. The mount's assertion pass is not on the statement path and is not measured.

CK's result stays as it was: the commit-time settle's removal (a 15 to 18 µs gain at K = 100 with four assertions) is still in place at the base, so CL's base `COMMIT` p50 for four assertions at K = 100 is 24.1 µs on one core, one microsecond over the control's 23.2 µs, and CL leaves that gap as it found it (+0.4 against +0.2 µs).

## What was cut, and what could not be measured

- **Nothing was cut from the grid** (the re-runs of `c2-relaxed` were added afterwards): all four configurations ran in full (cores 1 and 2, `relaxed` and `group`, 0/1/4 assertions, K = 1/10/100, one warm-up and five measured rounds). The whole run took 10 minutes 5 seconds (05:14:49 to 05:24:54).
- **Row counts of 200 and 10,000 were not run**; the preload is 1,000 rows only. The commit callback and the checkpoint records do not scale with relation size, but this run did not show that.
- **`strict` durability was not run.** The deferred-strict path (`CommitDeferred`, where the callback runs once staged) is the one CL's text names beside `group` and is not exercised here.
- **No cell with a long transaction left open across a checkpoint** (the `ASSERT_PENDING` writes), and not the mount's assertion pass.
- **Throughput and server CPU resolve nothing**, for the reasons above.
- The p99 columns carry single large outliers and are read for no delta.

## What made the numbers unreliable

- One window in five or six, on either binary, runs in a slow state (up to twice the per-transaction cost); it is why pooled means and per-round medians are not used. The 10 ms CPU clock quantizes server CPU per transaction.
- The 2-core `group` control moves by up to 11.6 µs at p50 between two servers of unrelated purpose, which is the width a `group` verdict carries.
- `src/server/tcp_server.cpp` was edited (a comment) during the last cell; it does not touch the measured binaries.

## Commands

```
export TMPDIR=/home/louis/ck-tmp            # /tmp is a nearly-full tmpfs
# base: detached worktree at b4a9e102 (cl-base); close: the milestone worktree at 291cc251
cmake --build build-release --target kds_server -j8       # in each tree
cp build-release/kds_server /home/louis/ck-tmp/run-cl-close/kds_server.{base,close}
# per configuration, e.g. cores 1, relaxed (group used --window-stmts 12000; cores 2 used --reactor-cpus 0,1):
taskset -c 2,3 python3 -I tools/assertion_settle_benchmark.py \
    --base-bin /home/louis/ck-tmp/run-cl-close/kds_server.base \
    --spike-bin /home/louis/ck-tmp/run-cl-close/kds_server.close \
    --workdir /home/louis/ck-tmp/run-cl-close/w-c1-relaxed --cores 1 --reactor-cpus 0 \
    --durability relaxed --window-stmts 30000 --rounds 5 --json /home/louis/ck-tmp/run-cl-close/c1-relaxed.json
```

The driver's flag `--spike-bin` names the second side, which in this file is the closing commit's binary. The raw JSON of the four configurations is left in `/home/louis/ck-tmp/run-cl-close/` and is not archived under `bench/` (a narrower measurement archives nothing, `bench/README.md`).
