# BI's close: the milestone's OLTP overhead A/B (`v2.7.0-784-gec827a19`)

**Thesis.** Over BI's whole code change one connection pays **nothing resolvable per statement**, and the one movement the
order expected, the checkpoint's hold on a reactor, is there wherever a run lands in the measurement. At `relaxed` 53 of the 54 p50
cells (nine statement kinds, three sizes, two core counts; `SHOW META` excluded) sit inside the run-to-run noise: the pk statements read
-0.55 to +0.10 µs on a 49-69 µs statement, the 20-row `INSERT` -0.65 to +0.10 µs, and the whole-relation walk +0.00 to +2.30 µs with every CI
containing zero (BA's close priced that walk at +20 to +46 µs at 10,000 rows). The one cell outside the noise is a control, `SHOW NAMESPACES`,
+0.30 µs [+0.20, +0.40]; no cell is resolved with B slower. A synced `INSERT` reads -10.75 to +3.45 µs on a 1,000 µs commit, resolved only on the negative
side (`cores = 2`, 10,000 rows, -9.80 µs [-23.45, -1.45]). **The cadence checkpoint, which in A is one reactor iteration (66.9 ms median and 106.5 ms worst at
`cores = 1`, 89.2 and 129.0 ms at `cores = 2`, 10,000 rows), is in B at most 1.4 ms median (7.0 and 17.3 ms worst)**. The statement in the arm where the
run lands falls from 45.8 to 2.1 ms (`cores = 1`); the longest statement of a `group` run at 10,000 rows falls from 36.3 to 19.4 ms and from 61.1 to 21.5 ms
(CIs exclude zero); at a 1,000 ms cadence and 200 or 1,000 rows it falls from 33.6-227.4 ms to 3.4-6.6 ms, where two copies of A read 250.1 and 284.7 ms. **At
`relaxed` and 10,000 rows the longest statement is 400-480 ms in most runs of A, of B and of two copies of A alike**: a reactor hold outside every
checkpoint run that tracks the WAL ring's inline drains, which no BI stage touches; that column cannot separate A from B. The checkpoint was absent from the
measured arms in 125 of 128 `relaxed` server-runs at 200 and 1,000 rows (section 2), so those cells price BI's per-tick test and task submit, and section 7
prices the run. **Verdict on BI-Q10 (iii): met, section 11.**

- **A** = `55ee8370` (`v2.7.0-779-g55ee8370`), BI-S1: the checkpoint instrumented and otherwise unchanged, so the
  cadence checkpoint is still one synchronous run inside the timer callback. Binary `bd8ffa1f…`.
- **B** = `ec827a19` (`v2.7.0-784-gec827a19`), BI closed in code: the cadence checkpoint is a `system`-group task that
  takes one step per poll and parks on the log writer and on the data file's sync thread, the gate rotates, the
  publish is paced (BI-S2 to BI-S5). Binary `2b45db5b…`. `git diff --stat 55ee8370 ec827a19 -- src include CMakeLists.txt`:
  19 files, 992 insertions, 148 deletions, five commits.
- This is the milestone-close overhead measurement of `instructions/v3.0.0/workorder-bi-checkpoint-off-the-reactor.md`
  section 5.2 (first bullet) and BI-Q10 (iii), "the OLTP overhead A/B within its own run-to-run noise". The idle-core run,
  the 12-cell stall matrix and core 0's loss window are in the BI-S5 stall file, not here.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-11, 20:05 to 20:55 UTC. `relaxed` `cores = 1` 20:05-20:11, `relaxed` `cores = 2` 20:11-20:19, `group` `cores = 1` 20:19-20:28, `group` `cores = 2` 20:28-20:38, dense-cadence cells 20:37-20:43 and 20:49-20:52, A/A controls 20:43-20:49 and 20:51-20:55. Per-cell windows and loads: section 2 |
| Branch / worktree | `worktree-bi-checkpoint-off-the-reactor`, worktree `bi-checkpoint-off-the-reactor`; HEAD `ec827a19` |
| Tree | `git status --short` at the start: no tracked file modified; two untracked paths (the BI-S5 stall results file and its archive, written by an earlier stage). At the end four documentation files were modified by the session that launched this run (`CLAUDE.md`, `docs/inflight/known-gaps.md`, the checkpoint bug entry, `instructions/v3.0.0/index.md`). Nothing under `src/`, `include/`, `tests/` or `tools/` changed at any point |
| A, provenance | Release build of an export of `55ee8370` in a session scratch directory (`CMAKE_BUILD_TYPE=Release`, `KDS_BUILD_TESTS=ON`, `KDS_BUILD_SIM=ON`, `KDS_BUILD_BENCH=OFF`). `diff -rq` of its `src/`, `include/` and `CMakeLists.txt` against `git archive 55ee8370` is empty. Source binary mtime 2026-10-11 12:12:22 UTC, after the commit (12:05:22 UTC) |
| B, provenance | The same flags, an export of `ec827a19`; the same `diff -rq` against `git archive ec827a19` is empty. Source binary mtime 2026-10-11 17:31:04 UTC, after the commit (17:30:13 UTC). **The worktree's own `build-release/kds_server` (sha256 `b855de3c…`, mtime 17:14:27 UTC) is older than three files `ec827a19` changed (`checkpointer.cpp`, `checkpointer.hpp`, `checkpoint_census.hpp`) and is not B; it was not used** |
| Measured copies | A `bd8ffa1fa32b12e731eb346cdc54943a4e7158d5dd6ccd168a0a2abc7254ce18`, B `2b45db5bd560e73133058bafc0a142d41737f8eee0d1d68f6b34704e0bba8ab5` (`sha256sum`, equal to the source binaries'; re-hashed after the last cell: unchanged). Every server of every cell started from `bin/kds_server-A` or `-B` in the run directory, never from a build tree (`archive/.../binaries.txt`) |
| Device | data files and WAL under `/home/cdkbs/bench-runs/bi-close-overhead-v2.7.0-784-gec827a19/w-*`, `/dev/root` (`/dev/nvme0n1p1`), ext4, mounted `rw,relatime,discard,errors=remount-ro,commit=30` (`df -T`, `findmnt`); not tmpfs. A fresh data file and server per (arm, run), deleted after |
| Host | 8 logical CPUs, AMD EPYC 9V74, 4 physical cores x 2 threads, 16 GB, Linux 7.0.0-1014-azure. Shared with other sessions (section 2) |
| Pinning | server on CPU 4 (`cores = 1`) or CPUs 4,5 (`cores = 2`), driver on CPU 7 (`taskset`), as BA's close |
| Server config | `cores` 1 or 2; `durability` `relaxed` or `group`; `buffer_pool_frames = 65536` (512 MiB, undivided); **`checkpoint_interval_ms = 5000`, written into every config** (the default; the dense cells use 1000); `log_level = warn`; everything else default. Ports 31811 and 31812, chosen, never the default 15432 |
| Pool against working set | `pool_resident` at the end of a run, maximum over every run of every cell: 1,498 frames of 65,536 (2.3 %), `pool_refused` 0 and no inline or background reclaim in any of the 296 run summaries (`drivers/poolcheck.py`), and 0 statement errors in all of them, so no read or write in this file waits on an eviction |
| Suite | not executed. No code changed in this measurement; the suite belongs to the stages |
| PostgreSQL floor | not measured for this shape. The A/B is B against A; the floor answers nothing about a change inside one engine |
| Drivers | `archive/.../drivers/bi_overhead_ab.py`, BA's `ba_overhead_ctrl.py` with four additions, none inside a timed region: the tools path, an explicit `checkpoint_interval_ms` (flag `--ckpt-ms`, default 5000), `SHOW META` snapshots of a few counters after every arm, and the checkpoint, scheduler and WAL counters at the end of each run. `bench/docs/` is closed (`bench/README.md`), so the commands are `drivers/run_all.sh` and `drivers/run_extra.sh`; `tools/` has no driver for these shapes, which breaks README rule 5 exactly as BA's close did |

## 2. Method, and the host each cell ran on

Method, unchanged from BA's close (`results-ba-close-overhead-v2.7.0-736-ge5a7e1b7.md` section 2): one client connection,
serial. Per (rows, run) a fresh A server and a fresh B server, six `(id, v, w) BTREE` relations loaded with `--rows` named
ascending keys in 500-row `INSERT`s, then arms of 3,000 statements each in 12 blocks of 250 that alternate which server goes
first; **the first server alternates per run, so the order is A,B,B,A,A,B,... within an arm and B,A,A,B across runs.**
**`--rows 200,1000,10000` is the relation's row count when an arm starts**, and the three sizes are three rows of every
table below. Equal work: 3,000 statements per arm per server; `relaxed` 16 runs per (cores, rows), 48,000 statements per arm
pooled; `group` 12 runs; dense cells 10 runs; A/A controls 8 runs. The delta is the median of the per-run p50 differences
B - A with a bootstrap 95 % CI (4,000 resamples of the runs, seed 1). Arms: the pk-omitted `INSERT` and its replicate, `SELECT`
by pk, `UPDATE` by pk, `DELETE` by pk, a 20-row ascending `INSERT`, a whole-relation walk (`SELECT COUNT(*), MIN(id), MAX(id)
... WHERE id >= lo AND id < lo + 100`, which walks the relation), and three statements that touch no engine path BI changed
(`SET ISOLATION LEVEL`, `SHOW NAMESPACES`, and `SHOW META`, which is not a control, below). The `group` cells run the two
`INSERT` arms and the `SET ISOLATION LEVEL` control only.

**The cadence is on, and what that means at these run lengths.** The checkpoint timer first fires 5 s after boot. At
200 or 1,000 rows the arms phase of a `relaxed` run ends at or before that first tick, so at `checkpoint_interval_ms = 5000`
the checkpoint is mostly absent from it:

#### relaxed-c1: checkpoint runs of the session's core during the arms, and the run record at the end of each run

| rows | server | session core | checkpoint runs landing in the arms: median [min-max] | runs with at least one | last run's wall time (`checkpoint_last_wall_us`): median / max | last run's steps: median | longest reactor iteration inside a run (`sched_hold_in_run_longest_us`): median / max | iterations over 10 ms inside runs, sum | `pool_resident` max / frames | `pool_refused`, sum |
|---|---|---|---|---|---|---|---|---|---|---|
| 200 | A | core 0 | 0 runs [0-0] | 0 of 16 runs | 0.0 ms / 0.0 ms | 0 steps | 0.0 ms / 0.0 ms | 0 iterations | 654 / 65,536 frames | 0 refusals |
| 200 | B | core 0 | 0 runs [0-0] | 0 of 16 runs | 0.0 ms / 0.0 ms | 0 steps | 0.0 ms / 0.0 ms | 0 iterations | 654 / 65,536 frames | 0 refusals |
| 1,000 | A | core 0 | 0 runs [0-1] | 1 of 16 runs | 0.0 ms / 122.0 ms | 0 steps | 0.0 ms / 122.1 ms | 1 iterations | 1,323 / 65,536 frames | 0 refusals |
| 1,000 | B | core 0 | 0 runs [0-0] | 0 of 16 runs | 0.0 ms / 0.0 ms | 0 steps | 0.0 ms / 0.0 ms | 0 iterations | 1,323 / 65,536 frames | 0 refusals |
| 10,000 | A | core 0 | 1 runs [1-1] | 16 of 16 runs | 66.9 ms / 106.5 ms | 13 steps | 66.9 ms / 106.5 ms | 16 iterations | 809 / 65,536 frames | 0 refusals |
| 10,000 | B | core 0 | 1 runs [1-1] | 16 of 16 runs | 25.8 ms / 37.6 ms | 13 steps | 1.4 ms / 7.0 ms | 0 iterations | 809 / 65,536 frames | 0 refusals |

#### relaxed-c2: checkpoint runs of the session's core during the arms, and the run record at the end of each run

| rows | server | session core | checkpoint runs landing in the arms: median [min-max] | runs with at least one | last run's wall time (`checkpoint_last_wall_us`): median / max | last run's steps: median | longest reactor iteration inside a run (`sched_hold_in_run_longest_us`): median / max | iterations over 10 ms inside runs, sum | `pool_resident` max / frames | `pool_refused`, sum |
|---|---|---|---|---|---|---|---|---|---|---|
| 200 | A | core 0/1 | 0 runs [0-0] | 0 of 16 runs | 0.0 ms / 0.0 ms | 0 steps | 0.0 ms / 0.0 ms | 0 iterations | 830 / 65,536 frames | 0 refusals |
| 200 | B | core 0/1 | 0 runs [0-0] | 0 of 16 runs | 0.0 ms / 0.0 ms | 0 steps | 0.0 ms / 0.0 ms | 0 iterations | 830 / 65,536 frames | 0 refusals |
| 1,000 | A | core 0/1 | 0 runs [0-1] | 2 of 16 runs | 0.0 ms / 131.5 ms | 0 steps | 0.0 ms / 131.5 ms | 2 iterations | 1,498 / 65,536 frames | 0 refusals |
| 1,000 | B | core 0/1 | 0 runs [0-0] | 0 of 16 runs | 0.0 ms / 0.0 ms | 0 steps | 0.0 ms / 0.0 ms | 0 iterations | 1,498 / 65,536 frames | 0 refusals |
| 10,000 | A | core 0/1 | 1 runs [1-1] | 16 of 16 runs | 89.2 ms / 129.0 ms | 18 steps | 89.2 ms / 129.0 ms | 12 iterations | 1,156 / 65,536 frames | 0 refusals |
| 10,000 | B | core 0/1 | 1 runs [1-1] | 16 of 16 runs | 23.6 ms / 48.2 ms | 18 steps | 1.4 ms / 17.3 ms | 1 iterations | 1,156 / 65,536 frames | 0 refusals |

**At 200 and 1,000 rows no cadence checkpoint run lands inside the measured arms in 125 of 128 `relaxed` server-runs**
(the three exceptions are A's, at 1,000 rows: one at `cores = 1`, two at `cores = 2`), and **at 10,000 rows exactly one
lands per server-run (64 of 64).** A `group` run lasts about 16 s, and 2-3 runs land per server and run at every size:

#### group-c1: checkpoint runs of the session's core during the arms, and the run record at the end of each run

| rows | server | session core | checkpoint runs landing in the arms: median [min-max] | runs with at least one | last run's wall time (`checkpoint_last_wall_us`): median / max | last run's steps: median | longest reactor iteration inside a run (`sched_hold_in_run_longest_us`): median / max | iterations over 10 ms inside runs, sum | `pool_resident` max / frames | `pool_refused`, sum |
|---|---|---|---|---|---|---|---|---|---|---|
| 200 | A | core 0 | 2 runs [2-2] | 12 of 12 runs | 8.4 ms / 13.3 ms | 1 steps | 10.4 ms / 13.4 ms | 9 iterations | 74 / 65,536 frames | 0 refusals |
| 200 | B | core 0 | 2 runs [2-2] | 12 of 12 runs | 13.1 ms / 26.6 ms | 1 steps | 1.9 ms / 7.7 ms | 0 iterations | 74 / 65,536 frames | 0 refusals |
| 1,000 | A | core 0 | 2 runs [2-3] | 12 of 12 runs | 10.4 ms / 14.5 ms | 1 steps | 15.5 ms / 20.4 ms | 20 iterations | 106 / 65,536 frames | 0 refusals |
| 1,000 | B | core 0 | 2 runs [2-3] | 12 of 12 runs | 12.0 ms / 16.3 ms | 1 steps | 1.9 ms / 3.1 ms | 0 iterations | 106 / 65,536 frames | 0 refusals |
| 10,000 | A | core 0 | 3 runs [3-3] | 12 of 12 runs | 8.2 ms / 10.5 ms | 1 steps | 35.7 ms / 65.7 ms | 20 iterations | 430 / 65,536 frames | 0 refusals |
| 10,000 | B | core 0 | 3 runs [2-3] | 12 of 12 runs | 10.2 ms / 11.8 ms | 1 steps | 3.0 ms / 7.9 ms | 0 iterations | 430 / 65,536 frames | 0 refusals |

#### group-c2: checkpoint runs of the session's core during the arms, and the run record at the end of each run

| rows | server | session core | checkpoint runs landing in the arms: median [min-max] | runs with at least one | last run's wall time (`checkpoint_last_wall_us`): median / max | last run's steps: median | longest reactor iteration inside a run (`sched_hold_in_run_longest_us`): median / max | iterations over 10 ms inside runs, sum | `pool_resident` max / frames | `pool_refused`, sum |
|---|---|---|---|---|---|---|---|---|---|---|
| 200 | A | core 0/1 | 2 runs [2-3] | 12 of 12 runs | 8.4 ms / 13.5 ms | 1 steps | 10.4 ms / 19.9 ms | 8 iterations | 102 / 65,536 frames | 0 refusals |
| 200 | B | core 0/1 | 2 runs [2-3] | 12 of 12 runs | 9.9 ms / 18.3 ms | 1 steps | 0.7 ms / 1.4 ms | 0 iterations | 102 / 65,536 frames | 0 refusals |
| 1,000 | A | core 0/1 | 2 runs [2-3] | 12 of 12 runs | 9.0 ms / 23.1 ms | 1 steps | 13.9 ms / 23.1 ms | 12 iterations | 158 / 65,536 frames | 0 refusals |
| 1,000 | B | core 0/1 | 2 runs [2-3] | 12 of 12 runs | 12.1 ms / 18.6 ms | 1 steps | 0.8 ms / 3.5 ms | 0 iterations | 158 / 65,536 frames | 0 refusals |
| 10,000 | A | core 0/1 | 3 runs [2-3] | 12 of 12 runs | 11.1 ms / 34.5 ms | 1 steps | 70.5 ms / 88.1 ms | 25 iterations | 774 / 65,536 frames | 0 refusals |
| 10,000 | B | core 0/1 | 2 runs [2-3] | 12 of 12 runs | 9.6 ms / 29.1 ms | 1 steps | 1.0 ms / 6.3 ms | 0 iterations | 774 / 65,536 frames | 0 refusals |

So the statement-level overhead of BI's per-tick test and task submit is measured in every cell, and the checkpoint run itself
is inside the window in the 10,000-row `relaxed` cells and in every `group` cell. **Because 200 and 1,000 rows at
`relaxed` never meet a checkpoint, section 7 adds the extreme: the same cells with `checkpoint_interval_ms = 1000`.**

**The host was shared, and this is the record.** The driver writes `/proc/loadavg` and `pgrep -a -f "cc1plus|cmake --build|ctest"`
into every run's JSON (`host_before`, `host_after`; in the archive). "Live program" counts runs whose lists hold a compiler,
build driver or test binary as the program (`cc1plus`, `cmake`, `ctest`, `kds_tests`); the pgrep also matches other sessions'
idle shell wait loops that merely name `ctest` and the driver's own shell, which are discarded before counting. The
1-minute load includes this run's own server and driver (roughly one CPU) and, in the `group` cells, the threads blocked in `fsync` (the load average counts them); `max wait` is the longest the driver held a run
back for a load above 2.0.

| cell (archive directory) | runs | UTC window of the run files | 1-min load before a run | 1-min load after a run | runs with a live program | runs with any competing process | longest hold-back | errors | largest per-run p50 gap on a control |
|---|---|---|---|---|---|---|---|---|---|
| `relaxed-c1` | 48 runs | 20:05:12-20:11:12 UTC | 0.51-2.00 | 0.63-2.16 | 0 of 48 runs | 0 of 48 runs | 10 s | 0 errors | 0.50 µs |
| `relaxed-c2` | 48 runs | 20:11:18-20:18:42 UTC | 1.60-1.99 | 1.60-2.35 | 0 of 48 runs | 0 of 48 runs | 30 s | 0 errors | 1.30 µs |
| `group-c1` | 36 runs | 20:18:58-20:28:04 UTC | 1.04-1.77 | 1.04-1.62 | 0 of 36 runs | 0 of 36 runs | 0 s | 0 errors | 0.30 µs |
| `group-c2` | 36 runs | 20:28:20-20:37:43 UTC | 1.07-1.65 | 1.07-1.65 | 0 of 36 runs | 0 of 36 runs | 0 s | 0 errors | 0.50 µs |
| `dense200-c1` | 10 runs | 20:49:48-20:50:42 UTC | 1.67-1.93 | 1.67-2.01 | 0 of 10 runs | 0 of 10 runs | 5 s | 0 errors | 0.50 µs |
| `dense200-c2` | 10 runs | 20:50:47-20:51:43 UTC | 1.50-1.87 | 1.50-2.02 | 0 of 10 runs | 0 of 10 runs | 5 s | 0 errors | 0.50 µs |
| `dense-c1` | 20 runs | 20:37:48-20:40:35 UTC | 1.46-1.99 | 1.52-2.16 | 0 of 20 runs | 0 of 20 runs | 5 s | 0 errors | 0.40 µs |
| `dense-c2` | 20 runs | 20:40:41-20:43:32 UTC | 1.56-1.95 | 1.56-2.21 | 0 of 20 runs | 0 of 20 runs | 10 s | 0 errors | 0.60 µs |
| `aa-c1` | 24 runs | 20:43:37-20:46:41 UTC | 1.61-1.96 | 1.61-2.19 | 0 of 24 runs | 0 of 24 runs | 10 s | 0 errors | 3.10 µs |
| `aa-c2` | 24 runs | 20:46:46-20:49:39 UTC | 1.53-1.95 | 1.53-2.08 | 0 of 24 runs | 0 of 24 runs | 5 s | 0 errors | 3.80 µs |
| `aadense-c1` | 20 runs | 20:51:49-20:54:51 UTC | 1.50-1.96 | 1.49-2.13 | 0 of 20 runs | 0 of 20 runs | 5 s | 0 errors | 0.40 µs |

The sampler (1 s, per-CPU busy time from `/proc/stat`, and any foreign process above 20 % of a CPU):

| cell | UTC window | samples | 1-min load, min-max | mean busy, server CPUs / driver CPU 7 | mean busy, the other CPUs (0,1,2,3,6; 5 at cores = 1) | samples with a foreign process >= 20 % of a CPU | those processes |
|---|---|---|---|---|---|---|---|
| `relaxed-c1` | 20:05:07-20:11:13 | 359 | 0.51-2.16 | 56 % / 34 % | 2 % | 0 | - |
| `relaxed-c2` | 20:11:13-20:18:42 | 444 | 1.60-2.77 | 15 % / 28 % | 6 % | 0 | - |
| `group-c1` | 20:18:42-20:28:04 | 555 | 1.04-1.77 | 9 % / 5 % | 2 % | 0 | - |
| `group-c2` | 20:28:04-20:37:43 | 573 | 1.06-1.70 | 4 % / 4 % | 2 % | 0 | - |
| `dense-c1` | 20:37:43-20:40:36 | 171 | 1.46-2.16 | 59 % / 30 % | 2 % | 0 | - |
| `dense-c2` | 20:40:36-20:43:32 | 174 | 1.56-2.21 | 16 % / 29 % | 8 % | 0 | - |
| `aa-c1` | 20:43:32-20:46:41 | 187 | 1.61-2.19 | 54 % / 33 % | 2 % | 0 | - |
| `aa-c2` | 20:46:41-20:49:39 | 176 | 1.53-2.08 | 18 % / 35 % | 7 % | 0 | - |
| `dense200-c1` | 20:49:43-20:50:42 | 58 | 1.67-2.01 | 45 % / 45 % | 2 % | 0 | - |
| `dense200-c2` | 20:50:42-20:51:43 | 61 | 1.50-2.02 | 10 % / 42 % | 7 % | 0 | - |
| `aadense-c1` | 20:51:43-20:54:51 | 186 | 1.49-2.23 | 54 % / 27 % | 2 % | 0 | - |

**No cell had a build or a test run live, and no foreign process held more than 20 % of a CPU in any sampled second.** The host's
I/O pressure (`/proc/pressure/io`) read `some` 36.0 / 37.0 / 46.8 % (10 s / 60 s / 300 s averages) at about 20:33 UTC, while
`group-c2` ran; synchronous commits in series are I/O-stalled by construction, so the figure is this run's own and is not separable
from it. It was read once; no per-cell pressure was recorded.

README's five rules: Release at the measured commit (above); data on a block device (`/dev/root` ext4); a hashed copy of
each binary; the load and the competing list written per run; the ports chosen.

### The noise floor, from inside the run

Three floors sit in every `relaxed` cell, and a fourth is built for this file.

1. **The controls** (`SET ISOLATION LEVEL`, `SHOW NAMESPACES`): nothing BI touches is on either path.
2. **The replicate** (`INSERT again`, a second relation, same arm): the same binary pair on a second relation of the same run.
3. **The sd of the per-run p50 delta**, 0.17-0.30 µs for the `relaxed` `INSERT`.
4. **The A/A cell** (`aa-c1`, `aa-c2`, 8 runs per size): the same A binary on both ports, the driver and the interleaving
   unchanged. Every delta there is the floor of that shape at that size.

| floor | `cores = 1` | `cores = 2` |
|---|---|---|
| control p50 delta B - A, `relaxed`, 16 runs | -0.05 to +0.10 µs, CIs inside [-0.15, +0.20] | 0.00 to +0.30 µs, CIs inside [-0.10, +0.40]; **one control cell excludes zero: `SHOW NAMESPACES`, 200 rows, +0.30 µs [+0.20, +0.40]** |
| A/A p50 delta, all pk shapes and `INSERT` | -0.15 to +0.45 µs, CIs inside [-0.50, +0.90] | -0.50 to +0.80 µs, CIs inside [-1.15, +1.20] |
| A/A p50 delta, 20-row `INSERT` and the walk | -0.10 to +0.20 µs, CIs inside [-7.90, +15.30] | -1.30 to +1.75 µs, CIs inside [-4.90, +9.90] |
| sd of the per-run p50 delta, `relaxed` `INSERT` | 0.21 / 0.22 / 0.18 µs at 200 / 1,000 / 10,000 rows | 0.17 / 0.30 / 0.29 µs |
| sd of the per-run p50 delta, `group` `INSERT` and its replicate | 3.4-5.8 µs | 11.5-18.1 µs |

**`SHOW META` is not a control.** B's reply carries the 13 fields of the checkpoint run record that BI-S2 to BI-S4
added (`checkpoint_last_gate_parks`, `checkpoint_last_end_parks`, `sched_system_longest_poll_us`, and the rest);
it reads +0.80 to +0.95 µs (about 1.7 %) slower in B in all six `relaxed` cells, and -0.45 to +0.20 µs in the A/A cells.
It is reported as that and carries no weight in the verdict.

## 3. Where a statement's time goes

A statement's latency is a sum. For one serial connection the parts are measurable from the controls; **a lock or conflict
wait is zero by construction** (one connection, no other writer). A's p50 in this run at `cores = 1`, 200 rows, with
`cores = 2` beside it:

| part | how it is read | A's p50, `cores = 1` | A's p50, `cores = 2` | share of the unsynced `INSERT` (`cores = 1`) | share of a `group` commit (`cores = 1`) |
|---|---|---|---|---|---|
| socket round trip, framing, parse, dispatch | `SET ISOLATION LEVEL` | 37.6 µs | 37.8 µs | 77 % | 3.8 % |
| write statement beyond that floor | pk `INSERT`, `relaxed`, minus the floor | 11.5 µs (49.1 - 37.6) | 11.4 µs (49.2 - 37.8) | 23 % | 1.2 % |
| read statement beyond the floor | pk `SELECT` minus the floor | 27.8 µs (65.4 - 37.6) | 28.6 µs (66.4 - 37.8) | - | - |
| durability wait (fsync of the commit) | `group` `INSERT` (996.8 µs, 1,023.0 µs) minus `relaxed` `INSERT` | 947.7 µs | 973.8 µs | - | 95.1 % |
| a cadence checkpoint run, per 5 s tick, A (one reactor hold), 10,000 rows | `sched_hold_in_run_longest_us` | 66.9 ms (1.3 % of the tick) | 89.2 ms (1.8 %) | - | - |
| the same, B (longest iteration the run overlapped) | same counter | 1.4 ms | 1.4 ms | - | - |

The commit is 95 % device in `group`, so a synced delta is a small share of a large number: **a delta under about 1 % of 1 ms
is under what one run's p50 can attribute (the sd of the per-run `group` p50 delta is 3.4-5.8 µs at `cores = 1` and 11.5-18.1
µs at `cores = 2`)**, and the 16-run median is what the CIs below average.

## 4. Result: `relaxed`, a single connection

**Nothing BI added is resolvable on any statement.** The p50 delta B - A with its 95 % CI, and the median per-run
throughput change, `cores = 1` then `cores = 2`. The A/A cell of the same shape is beside it in section 8. Every reading
below the floors of section 2 is not a finding.

#### relaxed-c1: p50 delta B - A (95 % CI of the median over runs) and tps change

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.30, +0.00]; +0.2 % tps [-0.1, +0.7] | -0.10 µs [-0.20, +0.10]; +0.4 % tps [-0.2, +0.9] | -0.15 µs [-0.30, -0.10]; +0.5 % tps [+0.2, +0.9] |
| INSERT again (replicate) | -0.10 µs [-0.20, +0.00]; +0.3 % tps [-1.8, +2.4] | +0.00 µs [-0.20, +0.20]; -0.1 % tps [-1.6, +3.1] | -0.20 µs [-0.30, +0.10]; -0.3 % tps [-2.1, +2.0] |
| SELECT by pk | -0.10 µs [-0.30, +0.20]; +0.3 % tps [+0.1, +0.9] | -0.25 µs [-0.80, +0.10]; +0.5 % tps [+0.2, +1.5] | -0.40 µs [-0.60, -0.10]; +0.4 % tps [-0.2, +1.2] |
| UPDATE by pk | +0.05 µs [-0.40, +0.40]; -0.2 % tps [-2.1, +2.6] | -0.55 µs [-0.80, -0.30]; +0.2 % tps [-1.6, +2.9] | -0.15 µs [-0.60, +0.05]; -1.4 % tps [-2.2, +2.8] |
| DELETE by pk | +0.00 µs [-0.10, +0.10]; -0.3 % tps [-1.7, +0.9] | -0.05 µs [-0.10, +0.10]; -1.0 % tps [-2.1, +0.7] | +0.00 µs [-0.05, +0.20]; -0.5 % tps [-1.6, -0.1] |
| bulk INSERT, 20 rows | -0.05 µs [-0.20, +0.30]; +0.2 % tps [-0.0, +0.8] | +0.10 µs [-0.20, +0.30]; +0.4 % tps [+0.0, +2.0] | -0.20 µs [-0.70, +0.40]; +23.0 % tps [-58.9, +131.1] |
| whole-relation walk (COUNT/MIN/MAX, 100-id window) | +0.00 µs [-0.60, +0.80]; +0.5 % tps [-0.9, +1.0] | +0.15 µs [-0.40, +1.00]; +0.5 % tps [-0.7, +0.9] | +2.30 µs [-0.50, +9.00]; +0.9 % tps [-0.2, +1.8] |
| SHOW META | +0.95 µs [+0.80, +1.10]; -2.0 % tps [-2.4, -1.5] | +0.85 µs [+0.60, +1.30]; -1.5 % tps [-2.4, -1.0] | +0.90 µs [+0.70, +1.10]; -1.8 % tps [-2.1, -1.3] |
| SHOW NAMESPACES (control) | +0.05 µs [+0.00, +0.15]; +0.1 % tps [-0.4, +0.4] | +0.05 µs [-0.10, +0.10]; -0.1 % tps [-0.3, +0.2] | +0.10 µs [+0.10, +0.20]; -0.3 % tps [-0.7, -0.1] |
| SET ISOLATION LEVEL (control) | +0.00 µs [-0.05, +0.00]; -0.1 % tps [-0.4, +0.3] | -0.05 µs [-0.15, +0.05]; +0.3 % tps [+0.0, +0.5] | +0.00 µs [-0.15, +0.10]; +0.0 % tps [-0.2, +0.3] |

#### relaxed-c1: throughput, A then B (median of the runs' ops / elapsed), and p50, A then B

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | 19,422 -> 19,482 tps; p50 49.1 -> 48.8 µs | 19,331 -> 19,414 tps; p50 49.0 -> 49.0 µs | 19,367 -> 19,446 tps; p50 49.0 -> 48.8 µs |
| INSERT again (replicate) | 19,228 -> 19,217 tps; p50 49.0 -> 48.9 µs | 18,981 -> 19,134 tps; p50 49.0 -> 48.9 µs | 19,119 -> 19,180 tps; p50 49.0 -> 48.8 µs |
| SELECT by pk | 14,453 -> 14,541 tps; p50 65.4 -> 65.5 µs | 14,090 -> 14,177 tps; p50 67.5 -> 67.2 µs | 14,764 -> 14,840 tps; p50 65.0 -> 64.7 µs |
| UPDATE by pk | 17,946 -> 18,148 tps; p50 52.1 -> 52.1 µs | 17,895 -> 18,140 tps; p50 52.5 -> 51.8 µs | 17,864 -> 17,732 tps; p50 52.6 -> 52.5 µs |
| DELETE by pk | 19,560 -> 19,474 tps; p50 48.8 -> 48.9 µs | 19,596 -> 19,418 tps; p50 48.8 -> 48.8 µs | 19,565 -> 19,407 tps; p50 48.9 -> 48.9 µs |
| bulk INSERT, 20 rows | 9,097 -> 9,120 tps; p50 94.0 -> 94.0 µs | 9,061 -> 9,100 tps; p50 93.8 -> 94.2 µs | 6,931 -> 6,556 tps; p50 94.5 -> 94.2 µs |
| whole-relation walk (COUNT/MIN/MAX, 100-id window) | 10,963 -> 10,925 tps; p50 86.8 -> 86.8 µs | 6,558 -> 6,578 tps; p50 146.9 -> 147.5 µs | 1,214 -> 1,230 tps; p50 805.5 -> 806.5 µs |
| SHOW META | 18,323 -> 17,940 tps; p50 52.5 -> 53.4 µs | 18,262 -> 17,957 tps; p50 52.5 -> 53.5 µs | 18,155 -> 17,995 tps; p50 52.7 -> 53.5 µs |
| SHOW NAMESPACES (control) | 25,613 -> 25,575 tps; p50 37.5 -> 37.5 µs | 25,332 -> 25,362 tps; p50 37.6 -> 37.6 µs | 25,542 -> 25,366 tps; p50 37.6 -> 37.7 µs |
| SET ISOLATION LEVEL (control) | 25,668 -> 25,640 tps; p50 37.6 -> 37.6 µs | 25,496 -> 25,593 tps; p50 37.6 -> 37.6 µs | 25,593 -> 25,664 tps; p50 37.6 -> 37.6 µs |

(runs per cell: [16]; sd of the per-run p50 delta, insert: 200 rows 0.21 µs, 1,000 rows 0.22 µs, 10,000 rows 0.18 µs)

#### relaxed-c2: p50 delta B - A (95 % CI of the median over runs) and tps change

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | -0.20 µs [-0.30, -0.10]; +0.5 % tps [+0.2, +0.8] | -0.20 µs [-0.30, +0.00]; +0.4 % tps [+0.0, +1.1] | -0.20 µs [-0.40, +0.00]; +0.5 % tps [-2.6, +2.3] |
| INSERT again (replicate) | -0.20 µs [-0.30, -0.10]; -0.1 % tps [-1.0, +0.7] | -0.15 µs [-0.40, +0.00]; -0.1 % tps [-0.9, +1.4] | -0.15 µs [-0.40, +0.00]; +0.5 % tps [-0.6, +1.5] |
| SELECT by pk | -0.30 µs [-0.50, +0.20]; +0.2 % tps [-0.4, +1.2] | -0.10 µs [-0.60, +0.20]; -0.1 % tps [-0.6, +0.9] | -0.25 µs [-1.00, +0.05]; +0.5 % tps [+0.0, +1.5] |
| UPDATE by pk | -0.40 µs [-0.50, +0.00]; -1.1 % tps [-3.0, +0.9] | -0.25 µs [-0.80, +0.00]; +2.6 % tps [+1.1, +3.7] | -0.45 µs [-0.90, +0.10]; +0.6 % tps [-0.6, +2.5] |
| DELETE by pk | +0.10 µs [-0.20, +0.20]; -1.3 % tps [-2.4, -0.1] | -0.10 µs [-0.25, +0.10]; -1.6 % tps [-2.1, +0.1] | +0.00 µs [-0.50, +0.20]; -0.3 % tps [-2.0, +1.1] |
| bulk INSERT, 20 rows | -0.15 µs [-2.00, +0.70]; +0.4 % tps [-1.1, +0.9] | -0.65 µs [-2.60, -0.20]; +1.7 % tps [+0.7, +4.9] | -0.60 µs [-2.30, +0.25]; -10.8 % tps [-40.0, +33.9] |
| whole-relation walk (COUNT/MIN/MAX, 100-id window) | +0.35 µs [-0.80, +1.10]; -0.0 % tps [-0.7, +1.3] | +0.40 µs [-0.70, +1.10]; +0.2 % tps [-0.4, +1.2] | +0.30 µs [-2.65, +5.20]; +0.5 % tps [-0.6, +3.4] |
| SHOW META | +0.95 µs [+0.80, +1.20]; -1.6 % tps [-2.3, -1.2] | +0.80 µs [+0.60, +1.30]; -1.4 % tps [-1.7, -0.2] | +0.80 µs [+0.70, +1.10]; -1.2 % tps [-2.0, -0.8] |
| SHOW NAMESPACES (control) | +0.30 µs [+0.20, +0.40]; -0.8 % tps [-1.3, +0.2] | +0.00 µs [-0.10, +0.20]; +0.2 % tps [-0.7, +0.6] | +0.10 µs [+0.00, +0.20]; -0.6 % tps [-1.2, +0.7] |
| SET ISOLATION LEVEL (control) | +0.00 µs [-0.10, +0.10]; -0.1 % tps [-0.6, +0.3] | +0.00 µs [-0.10, +0.05]; +0.4 % tps [-0.1, +0.8] | +0.00 µs [-0.10, +0.05]; -0.4 % tps [-0.9, +0.3] |

#### relaxed-c2: throughput, A then B (median of the runs' ops / elapsed), and p50, A then B

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | 19,169 -> 19,253 tps; p50 49.2 -> 49.0 µs | 19,057 -> 19,130 tps; p50 49.2 -> 49.0 µs | 18,658 -> 18,853 tps; p50 49.3 -> 49.0 µs |
| INSERT again (replicate) | 18,859 -> 18,800 tps; p50 49.2 -> 49.0 µs | 18,648 -> 18,737 tps; p50 49.3 -> 49.0 µs | 18,991 -> 19,135 tps; p50 49.3 -> 49.0 µs |
| SELECT by pk | 14,212 -> 14,235 tps; p50 66.4 -> 66.1 µs | 13,768 -> 13,746 tps; p50 68.8 -> 68.7 µs | 14,450 -> 14,542 tps; p50 65.8 -> 65.5 µs |
| UPDATE by pk | 17,462 -> 17,361 tps; p50 53.7 -> 53.4 µs | 17,177 -> 17,562 tps; p50 53.8 -> 53.2 µs | 17,274 -> 17,436 tps; p50 54.2 -> 53.9 µs |
| DELETE by pk | 19,055 -> 18,828 tps; p50 49.3 -> 49.5 µs | 19,229 -> 18,970 tps; p50 49.3 -> 49.2 µs | 19,023 -> 18,945 tps; p50 49.5 -> 49.4 µs |
| bulk INSERT, 20 rows | 8,443 -> 8,377 tps; p50 103.3 -> 103.1 µs | 8,272 -> 8,633 tps; p50 103.5 -> 102.2 µs | 6,417 -> 5,421 tps; p50 103.0 -> 102.4 µs |
| whole-relation walk (COUNT/MIN/MAX, 100-id window) | 10,716 -> 10,745 tps; p50 88.3 -> 88.2 µs | 6,463 -> 6,491 tps; p50 148.5 -> 148.6 µs | 1,203 -> 1,210 tps; p50 818.9 -> 818.0 µs |
| SHOW META | 18,126 -> 17,840 tps; p50 52.7 -> 53.5 µs | 18,061 -> 17,832 tps; p50 52.6 -> 53.6 µs | 18,066 -> 17,798 tps; p50 52.8 -> 53.6 µs |
| SHOW NAMESPACES (control) | 25,101 -> 24,776 tps; p50 37.8 -> 38.0 µs | 24,991 -> 25,022 tps; p50 38.0 -> 38.0 µs | 25,068 -> 24,963 tps; p50 38.0 -> 38.0 µs |
| SET ISOLATION LEVEL (control) | 25,446 -> 25,385 tps; p50 37.8 -> 37.8 µs | 25,300 -> 25,420 tps; p50 37.9 -> 37.8 µs | 25,242 -> 25,154 tps; p50 37.8 -> 37.8 µs |

(runs per cell: [16]; sd of the per-run p50 delta, insert: 200 rows 0.17 µs, 1,000 rows 0.30 µs, 10,000 rows 0.29 µs)

What the matrix says:

- **The pk `INSERT`, its replicate, `DELETE`, `SELECT` and `UPDATE` read between -0.55 and +0.10 µs at both core counts** (-0.55 µs is the
  `UPDATE` at 1,000 rows, `cores = 1`; -0.45 µs the `UPDATE` at 10,000 rows, `cores = 2`). Of the 30 cells (5 statements x 3 sizes x 2 core
  counts) B reads below A in 24, equal in 4 and above in 2 (+0.05 and +0.10 µs). **Five of the 30 CIs exclude zero, all with B the faster one**:
  the `INSERT` at 10,000 rows `cores = 1` -0.15 µs [-0.30, -0.10], the pk `SELECT` at 10,000 rows `cores = 1` -0.40 µs [-0.60, -0.10], the
  `UPDATE` at 1,000 rows `cores = 1` -0.55 µs [-0.80, -0.30], and the `INSERT` and its replicate at 200 rows `cores = 2` -0.20 µs [-0.30, -0.10]
  and -0.20 µs [-0.35, -0.10]. **A control that nothing in BI touches reads the other way by +0.30 µs [+0.20, +0.40]** (`SHOW NAMESPACES`, 200
  rows, `cores = 2`), and two copies of one binary read -0.50 to +0.80 µs on these statements (section 8): sub-microsecond deltas of either sign are this host's floor.
- **The 20-row `INSERT` reads -0.65 to +0.10 µs** (one CI excludes zero, B faster: 1,000 rows `cores = 2`, -0.65 µs [-2.60, -0.20]) and **the
  whole-relation walk +0.00 to +0.40 µs at 200 and 1,000 rows**, and +2.30 µs [-1.25, +9.20] (`cores = 1`) and +0.30 µs [-2.65, +5.20]
  (`cores = 2`) at 10,000 rows: every walk CI contains zero. A's walk costs about 81 ns a row (805.5 and 818.9 µs over 10,000 rows);
  +2.3 µs would be 0.23 ns a row.
- **The throughput columns at 10,000 rows for the 20-row `INSERT`** (+23.0 % with a CI of [-58.9, +131.1], -10.8 % [-40.0, +33.9])
  are one 400-480 ms stall per run in one arm of one server (section 6), not a rate; only their p50 is a reading.
- **`SHOW META`** reads +0.80 to +0.95 µs for the reason in section 2.

The shape of the latency (README rule 6), the median over the runs of each run's percentile, A then B, 3,000 statements per
run and arm (48,000 pooled), `cores = 1`:

#### relaxed-c1: latency distribution, median over runs of each run's percentile, A then B

| rows | statement | server | statements | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| 1,000 | INSERT, pk omitted | A | 48,000 statements | 42.0 µs | 48.2 µs | 49.0 µs | 63.8 µs | 78.5 µs |
| 1,000 | INSERT, pk omitted | B | 48,000 statements | 42.2 µs | 48.0 µs | 49.0 µs | 63.8 µs | 76.8 µs |
| 1,000 | SELECT by pk | A | 48,000 statements | 61.0 µs | 65.0 µs | 67.5 µs | 86.2 µs | 99.7 µs |
| 1,000 | SELECT by pk | B | 48,000 statements | 60.9 µs | 64.7 µs | 67.2 µs | 85.0 µs | 96.2 µs |
| 1,000 | UPDATE by pk | A | 48,000 statements | 45.1 µs | 51.7 µs | 52.5 µs | 66.7 µs | 78.0 µs |
| 1,000 | UPDATE by pk | B | 48,000 statements | 44.8 µs | 51.1 µs | 51.8 µs | 66.4 µs | 78.0 µs |
| 1,000 | DELETE by pk | A | 48,000 statements | 42.1 µs | 48.2 µs | 48.8 µs | 62.2 µs | 70.8 µs |
| 1,000 | DELETE by pk | B | 48,000 statements | 42.0 µs | 48.2 µs | 48.8 µs | 62.0 µs | 71.0 µs |
| 1,000 | bulk INSERT, 20 rows | A | 48,000 statements | 89.5 µs | 91.8 µs | 93.8 µs | 130.5 µs | 211.0 µs |
| 1,000 | bulk INSERT, 20 rows | B | 48,000 statements | 89.2 µs | 92.0 µs | 94.2 µs | 127.7 µs | 210.1 µs |
| 1,000 | whole-relation walk (COUNT/MIN/MAX, 100-id window) | A | 48,000 statements | 137.7 µs | 143.7 µs | 146.9 µs | 168.6 µs | 192.6 µs |
| 1,000 | whole-relation walk (COUNT/MIN/MAX, 100-id window) | B | 48,000 statements | 137.6 µs | 144.1 µs | 147.5 µs | 167.3 µs | 188.7 µs |
| 10,000 | INSERT, pk omitted | A | 48,000 statements | 42.2 µs | 48.2 µs | 49.0 µs | 63.6 µs | 79.2 µs |
| 10,000 | INSERT, pk omitted | B | 48,000 statements | 42.0 µs | 48.0 µs | 48.8 µs | 62.9 µs | 76.5 µs |
| 10,000 | SELECT by pk | A | 48,000 statements | 61.6 µs | 64.2 µs | 65.0 µs | 79.3 µs | 88.3 µs |
| 10,000 | SELECT by pk | B | 48,000 statements | 60.4 µs | 63.8 µs | 64.7 µs | 78.8 µs | 88.5 µs |
| 10,000 | UPDATE by pk | A | 48,000 statements | 45.4 µs | 51.8 µs | 52.6 µs | 67.1 µs | 80.8 µs |
| 10,000 | UPDATE by pk | B | 48,000 statements | 44.6 µs | 51.7 µs | 52.5 µs | 67.2 µs | 80.8 µs |
| 10,000 | DELETE by pk | A | 48,000 statements | 42.0 µs | 48.2 µs | 48.9 µs | 61.0 µs | 69.8 µs |
| 10,000 | DELETE by pk | B | 48,000 statements | 41.8 µs | 48.2 µs | 48.9 µs | 61.3 µs | 70.5 µs |
| 10,000 | bulk INSERT, 20 rows | A | 48,000 statements | 89.8 µs | 92.5 µs | 94.5 µs | 121.8 µs | 169.5 µs |
| 10,000 | bulk INSERT, 20 rows | B | 48,000 statements | 89.8 µs | 92.3 µs | 94.2 µs | 120.4 µs | 163.0 µs |
| 10,000 | whole-relation walk (COUNT/MIN/MAX, 100-id window) | A | 48,000 statements | 733.5 µs | 782.0 µs | 805.5 µs | 855.4 µs | 935.2 µs |
| 10,000 | whole-relation walk (COUNT/MIN/MAX, 100-id window) | B | 48,000 statements | 735.4 µs | 782.5 µs | 806.5 µs | 859.1 µs | 1064.0 µs |

The pk statements shift by the same fraction of a microsecond at every percentile, so what moved, if anything, is a fixed
cost, and it is below 1 µs. **The walk's p99 at 10,000 rows, `cores = 1`, reads 1,064.0 µs in B against 935.2 µs in A (+129 µs)**; at p0 to p95 the same walk is
flat (+1.9, +0.5, +1.0, +3.7 µs). **It does not reproduce**: `cores = 2` reads 975.0 against 959.8 µs (+15 µs), the dense cell 1,077.3 against
1,062.5 µs (+15 µs), and two copies of A read 979.0 against 956.2 µs (+23 µs) at `cores = 1` and 927.6 against 956.0 µs (-28 µs) at `cores = 2`.

### Against this engine's last result (rule 4)

The previous measurement of the same shapes and driver is the BA close, `results-ba-close-overhead-v2.7.0-736-ge5a7e1b7.md`
(`v2.7.0-736-ge5a7e1b7`; A is 43 commits and B 48 commits past it). Its B' column is its A p50 plus its median delta (derived). The
two runs share the pinning (server CPU 4 or 4,5, driver CPU 7), the driver's shapes and the device, and differ in the day:

#### cores = 1, relaxed: p50 of the BA close's B' (`e5a7e1b7`, v2.7.0-736, derived), of this run's A (`55ee8370`, v2.7.0-779) and B (`ec827a19`, v2.7.0-784)

| shape | rows | BA close B' | this run, A | this run, B | A - BA B' | B - BA B' |
|---|---|---|---|---|---|---|
| INSERT, pk omitted | 200 | 49.10 µs | 49.1 µs | 48.8 µs | +0.00 µs | -0.30 µs |
| INSERT, pk omitted | 1,000 | 49.05 µs | 49.0 µs | 49.0 µs | -0.05 µs | -0.05 µs |
| INSERT, pk omitted | 10,000 | 48.90 µs | 49.0 µs | 48.8 µs | +0.10 µs | -0.05 µs |
| SELECT by pk | 200 | 65.30 µs | 65.4 µs | 65.5 µs | +0.10 µs | +0.15 µs |
| SELECT by pk | 1,000 | 66.90 µs | 67.5 µs | 67.2 µs | +0.55 µs | +0.30 µs |
| SELECT by pk | 10,000 | 64.50 µs | 65.0 µs | 64.7 µs | +0.55 µs | +0.15 µs |
| UPDATE by pk | 200 | 51.45 µs | 52.1 µs | 52.1 µs | +0.65 µs | +0.65 µs |
| UPDATE by pk | 1,000 | 51.50 µs | 52.5 µs | 51.8 µs | +0.95 µs | +0.25 µs |
| UPDATE by pk | 10,000 | 52.05 µs | 52.6 µs | 52.5 µs | +0.55 µs | +0.40 µs |
| DELETE by pk | 200 | 48.90 µs | 48.8 µs | 48.9 µs | -0.05 µs | +0.00 µs |
| DELETE by pk | 1,000 | 49.00 µs | 48.8 µs | 48.8 µs | -0.20 µs | -0.20 µs |
| DELETE by pk | 10,000 | 49.10 µs | 48.9 µs | 48.9 µs | -0.20 µs | -0.20 µs |
| bulk INSERT, 20 rows | 200 | 94.30 µs | 94.0 µs | 94.0 µs | -0.35 µs | -0.30 µs |
| bulk INSERT, 20 rows | 1,000 | 94.40 µs | 93.8 µs | 94.2 µs | -0.60 µs | -0.15 µs |
| bulk INSERT, 20 rows | 10,000 | 94.50 µs | 94.5 µs | 94.2 µs | -0.05 µs | -0.30 µs |
| whole-relation walk | 200 | 87.10 µs | 86.8 µs | 86.8 µs | -0.25 µs | -0.25 µs |
| whole-relation walk | 1,000 | 148.65 µs | 146.9 µs | 147.5 µs | -1.75 µs | -1.15 µs |
| whole-relation walk | 10,000 | 823.05 µs | 805.5 µs | 806.5 µs | -17.55 µs | -16.50 µs |
| SET ISOLATION LEVEL (control) | 200 | 37.70 µs | 37.6 µs | 37.6 µs | -0.10 µs | -0.10 µs |
| SET ISOLATION LEVEL (control) | 1,000 | 37.60 µs | 37.6 µs | 37.6 µs | +0.00 µs | +0.00 µs |
| SET ISOLATION LEVEL (control) | 10,000 | 37.60 µs | 37.6 µs | 37.6 µs | +0.00 µs | +0.00 µs |

#### cores = 2, relaxed: p50 of the BA close's B' (`e5a7e1b7`, v2.7.0-736, derived), of this run's A (`55ee8370`, v2.7.0-779) and B (`ec827a19`, v2.7.0-784)

| shape | rows | BA close B' | this run, A | this run, B | A - BA B' | B - BA B' |
|---|---|---|---|---|---|---|
| INSERT, pk omitted | 200 | 49.65 µs | 49.2 µs | 49.0 µs | -0.45 µs | -0.65 µs |
| INSERT, pk omitted | 1,000 | 49.50 µs | 49.2 µs | 49.0 µs | -0.30 µs | -0.45 µs |
| INSERT, pk omitted | 10,000 | 49.60 µs | 49.3 µs | 49.0 µs | -0.30 µs | -0.60 µs |
| SELECT by pk | 200 | 67.35 µs | 66.4 µs | 66.1 µs | -0.95 µs | -1.25 µs |
| SELECT by pk | 1,000 | 69.55 µs | 68.8 µs | 68.7 µs | -0.75 µs | -0.90 µs |
| SELECT by pk | 10,000 | 66.55 µs | 65.8 µs | 65.5 µs | -0.70 µs | -1.05 µs |
| UPDATE by pk | 200 | 52.95 µs | 53.7 µs | 53.4 µs | +0.70 µs | +0.45 µs |
| UPDATE by pk | 1,000 | 53.05 µs | 53.8 µs | 53.2 µs | +0.70 µs | +0.20 µs |
| UPDATE by pk | 10,000 | 53.85 µs | 54.2 µs | 53.9 µs | +0.40 µs | +0.05 µs |
| DELETE by pk | 200 | 49.70 µs | 49.3 µs | 49.5 µs | -0.40 µs | -0.25 µs |
| DELETE by pk | 1,000 | 49.65 µs | 49.3 µs | 49.2 µs | -0.30 µs | -0.40 µs |
| DELETE by pk | 10,000 | 49.60 µs | 49.5 µs | 49.4 µs | -0.10 µs | -0.20 µs |
| bulk INSERT, 20 rows | 200 | 103.10 µs | 103.3 µs | 103.1 µs | +0.25 µs | +0.00 µs |
| bulk INSERT, 20 rows | 1,000 | 102.45 µs | 103.5 µs | 102.2 µs | +1.05 µs | -0.25 µs |
| bulk INSERT, 20 rows | 10,000 | 103.45 µs | 103.0 µs | 102.4 µs | -0.50 µs | -1.05 µs |
| whole-relation walk | 200 | 89.05 µs | 88.3 µs | 88.2 µs | -0.75 µs | -0.90 µs |
| whole-relation walk | 1,000 | 149.75 µs | 148.5 µs | 148.6 µs | -1.25 µs | -1.15 µs |
| whole-relation walk | 10,000 | 831.75 µs | 818.9 µs | 818.0 µs | -12.85 µs | -13.75 µs |
| SET ISOLATION LEVEL (control) | 200 | 38.00 µs | 37.8 µs | 37.8 µs | -0.20 µs | -0.20 µs |
| SET ISOLATION LEVEL (control) | 1,000 | 38.20 µs | 37.9 µs | 37.8 µs | -0.30 µs | -0.40 µs |
| SET ISOLATION LEVEL (control) | 10,000 | 38.10 µs | 37.8 µs | 37.8 µs | -0.30 µs | -0.30 µs |

**B reads within 1.25 µs of the BA close's B' in 40 of the 42 cells (7 statements x 3 sizes x 2 core counts), and 16.50 and 13.75 µs
below it on the walk at 10,000 rows** (806.5 and 818.0 µs against 823.05 and 831.75). A reads -17.55 and -12.85 µs against the same
cell, and A and B differ from each other by +1.0 and -0.9 µs there, so the shift from the BA close is common to both builds and is not
BI's. The BA file itself recorded six walk deltas at 10,000 rows spanning +19.7 to +46.3 µs and a 21 µs gap between its A and the
previous close's B'; the walk at 10,000 rows is the one cell in which this host and this engine move by tens of microseconds between days.

## 5. Result: `group`, a single connection

`group` durability, the `INSERT` and its replicate, 12 runs per cell. The control is the `SET ISOLATION LEVEL` row.

#### group-c1: synced INSERT, B - A p50 (95 % CI), tps change, and A then B throughput

| rows | shape | A p50 | B - A p50 (95 % CI) | B - A as share of A's p50 | tps change (95 % CI) | A -> B throughput | sd of per-run p50 delta |
|---|---|---|---|---|---|---|---|
| 200 | INSERT, pk omitted | 996.8 µs | -0.15 µs [-4.60, +1.85] | -0.02 % | +0.3 % [-1.5, +0.8] | 868 -> 866 tps | 3.57 µs |
| 200 | INSERT again (replicate) | 998.0 µs | -1.00 µs [-3.65, +6.20] | -0.10 % | +0.7 % [-2.3, +1.8] | 854 -> 857 tps | 5.83 µs |
| 200 | SET ISOLATION LEVEL (control) | 37.5 µs | +0.00 µs [-0.10, +0.05] | +0.00 % | -0.2 % [-0.7, +0.5] | 25,653 -> 25,622 tps | 0.13 µs |
| 1,000 | INSERT, pk omitted | 1001.5 µs | -0.05 µs [-2.85, +3.75] | -0.00 % | +1.0 % [-1.9, +2.0] | 856 -> 859 tps | 4.65 µs |
| 1,000 | INSERT again (replicate) | 998.1 µs | +2.20 µs [-0.80, +5.40] | +0.22 % | +0.8 % [-1.6, +1.4] | 857 -> 849 tps | 5.68 µs |
| 1,000 | SET ISOLATION LEVEL (control) | 37.5 µs | +0.10 µs [+0.00, +0.20] | +0.27 % | -0.3 % [-0.7, -0.2] | 25,568 -> 25,463 tps | 0.10 µs |
| 10,000 | INSERT, pk omitted | 999.5 µs | +1.05 µs [-1.90, +3.30] | +0.11 % | +0.7 % [-0.2, +1.8] | 851 -> 857 tps | 3.41 µs |
| 10,000 | INSERT again (replicate) | 999.2 µs | +3.45 µs [-0.95, +5.85] | +0.35 % | -0.9 % [-2.4, +0.1] | 863 -> 853 tps | 4.49 µs |
| 10,000 | SET ISOLATION LEVEL (control) | 37.6 µs | -0.05 µs [-0.10, +0.00] | -0.13 % | +0.1 % [-0.3, +0.4] | 25,709 -> 25,806 tps | 0.09 µs |

#### group-c2: synced INSERT, B - A p50 (95 % CI), tps change, and A then B throughput

| rows | shape | A p50 | B - A p50 (95 % CI) | B - A as share of A's p50 | tps change (95 % CI) | A -> B throughput | sd of per-run p50 delta |
|---|---|---|---|---|---|---|---|
| 200 | INSERT, pk omitted | 1023.0 µs | -2.80 µs [-12.35, +10.70] | -0.27 % | +1.8 % [-0.0, +3.1] | 824 -> 835 tps | 13.63 µs |
| 200 | INSERT again (replicate) | 1023.5 µs | -0.70 µs [-9.70, +8.75] | -0.07 % | -0.6 % [-1.3, +0.8] | 842 -> 836 tps | 15.32 µs |
| 200 | SET ISOLATION LEVEL (control) | 37.9 µs | +0.10 µs [+0.00, +0.25] | +0.26 % | -0.2 % [-0.8, +0.4] | 25,280 -> 25,218 tps | 0.17 µs |
| 1,000 | INSERT, pk omitted | 1023.5 µs | +3.15 µs [-8.45, +22.10] | +0.31 % | -1.7 % [-2.8, +0.4] | 837 -> 825 tps | 17.36 µs |
| 1,000 | INSERT again (replicate) | 1020.4 µs | -5.55 µs [-8.90, +19.45] | -0.54 % | +0.4 % [-2.1, +1.9] | 838 -> 838 tps | 18.09 µs |
| 1,000 | SET ISOLATION LEVEL (control) | 38.0 µs | +0.10 µs [+0.00, +0.10] | +0.26 % | +0.1 % [-0.3, +0.5] | 25,285 -> 25,189 tps | 0.10 µs |
| 10,000 | INSERT, pk omitted | 1042.2 µs | -9.80 µs [-23.45, -1.45] | -0.94 % | +1.3 % [+0.2, +3.4] | 819 -> 830 tps | 11.93 µs |
| 10,000 | INSERT again (replicate) | 1039.7 µs | -10.75 µs [-21.70, -1.50] | -1.03 % | +1.6 % [-0.2, +3.0] | 820 -> 832 tps | 11.52 µs |
| 10,000 | SET ISOLATION LEVEL (control) | 38.1 µs | +0.00 µs [-0.10, +0.05] | +0.00 % | -0.1 % [-1.0, +0.2] | 25,260 -> 25,122 tps | 0.11 µs |

- **At `cores = 1` the delta is zero within CIs 5-10 µs wide on a 1,000 µs commit**: -1.00 to +3.45 µs, no CI
  excludes zero (the six `INSERT` and replicate cells), the largest reading is the replicate at 10,000 rows, +3.45 µs [-0.95, +5.85] (0.35 %).
- **At `cores = 2` the readings are -10.75 to +3.15 µs, with one pair of CIs that excludes zero on the negative side:
  the `INSERT` at 10,000 rows, -9.80 µs [-23.45, -1.45] (-0.94 %, B faster), and its replicate, -10.75 µs [-21.70, -1.50].**
  The other four size/arm cells straddle zero with both signs. BA's close recorded +13 to +32 µs per commit at `cores = 2` for another
  pair of binaries (the hand-off of the sync to the writer thread); this pair reads no cost, and its only resolved cells are B the faster one.
- The commit's own cost sits at 997-1,002 µs (`cores = 1`) and 1,020-1,042 µs (`cores = 2`) in this run. The control
  stays within 0.1 µs.
- Throughput, A -> B: 868 -> 866, 856 -> 859 and 851 -> 857 tps (`cores = 1`) and 824 -> 835, 837 -> 825 and 819 -> 830 tps
  (`cores = 2`) for the `INSERT` at 200 / 1,000 / 10,000 rows, none resolved.

The shape of the `group` commit (the median over runs of each run's percentile; the p95 and p99 are device tail, not engine path):

#### group-c1: latency distribution, median over runs of each run's percentile, A then B

| rows | statement | server | statements | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| 200 | INSERT, pk omitted | A | 36,000 statements | 867.4 µs | 964.1 µs | 996.8 µs | 1731.9 µs | 4862.9 µs |
| 200 | INSERT, pk omitted | B | 36,000 statements | 865.0 µs | 963.5 µs | 996.8 µs | 1761.6 µs | 4789.2 µs |
| 1,000 | INSERT, pk omitted | A | 36,000 statements | 862.8 µs | 967.5 µs | 1001.5 µs | 1776.8 µs | 4786.6 µs |
| 1,000 | INSERT, pk omitted | B | 36,000 statements | 871.2 µs | 966.9 µs | 1001.2 µs | 1793.2 µs | 4811.2 µs |
| 10,000 | INSERT, pk omitted | A | 36,000 statements | 867.2 µs | 965.2 µs | 999.5 µs | 1729.0 µs | 4818.9 µs |
| 10,000 | INSERT, pk omitted | B | 36,000 statements | 869.0 µs | 966.0 µs | 999.9 µs | 1816.2 µs | 4840.5 µs |

#### group-c2: latency distribution, median over runs of each run's percentile, A then B

| rows | statement | server | statements | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| 200 | INSERT, pk omitted | A | 36,000 statements | 889.4 µs | 989.8 µs | 1023.0 µs | 1977.2 µs | 5097.0 µs |
| 200 | INSERT, pk omitted | B | 36,000 statements | 892.3 µs | 988.2 µs | 1024.8 µs | 1847.2 µs | 4973.8 µs |
| 1,000 | INSERT, pk omitted | A | 36,000 statements | 893.9 µs | 990.4 µs | 1023.5 µs | 1888.7 µs | 4938.4 µs |
| 1,000 | INSERT, pk omitted | B | 36,000 statements | 896.4 µs | 999.0 µs | 1036.2 µs | 1995.3 µs | 4939.6 µs |
| 10,000 | INSERT, pk omitted | A | 36,000 statements | 905.6 µs | 1004.5 µs | 1042.2 µs | 1881.7 µs | 4927.8 µs |
| 10,000 | INSERT, pk omitted | B | 36,000 statements | 897.9 µs | 993.4 µs | 1027.8 µs | 1947.9 µs | 4980.6 µs |

## 6. The longest statement

**This is the number the order named: A's checkpoint held a reactor for its whole run. B's does not, and the per-run longest
statement shows it in `group` and in the arm that holds the run; it does not show it at `relaxed` and 10,000 rows, where
a second stall, that belongs to neither binary, is larger.**

The longest statement of any timed arm per run (controls excluded), A and B, with the A/A cell as the floor of the same
column:

| cell | rows | runs | A: longest statement of a run, median [min-max] | A: runs over 10 ms / over 40 ms | B: median [min-max] | B: runs over 10 ms / over 40 ms | B - A per run: median (95 % CI) |
|---|---|---|---|---|---|---|---|
| `relaxed-c1` | 200 | 16 runs | 4.3 ms [2.4-11.4] | 1 of 16 runs / 0 of 16 runs | 3.1 ms [2.0-8.2] | 0 of 16 runs / 0 of 16 runs | -0.4 ms [-2.0, +0.5] |
| `relaxed-c1` | 1,000 | 16 runs | 4.0 ms [2.7-9.6] | 0 of 16 runs / 0 of 16 runs | 3.8 ms [2.4-9.1] | 0 of 16 runs / 0 of 16 runs | +0.5 ms [-1.5, +2.1] |
| `relaxed-c1` | 10,000 | 16 runs | 173.0 ms [3.6-453.4] | 12 of 16 runs / 12 of 16 runs | 211.5 ms [3.0-472.9] | 8 of 16 runs / 8 of 16 runs | +54.0 ms [-414.3, +407.4] |
| `relaxed-c2` | 200 | 16 runs | 3.7 ms [3.0-10.7] | 2 of 16 runs / 0 of 16 runs | 6.2 ms [3.3-12.1] | 2 of 16 runs / 0 of 16 runs | +2.0 ms [-0.1, +5.7] |
| `relaxed-c2` | 1,000 | 16 runs | 5.0 ms [3.0-21.5] | 3 of 16 runs / 0 of 16 runs | 4.3 ms [0.5-13.9] | 4 of 16 runs / 0 of 16 runs | -1.0 ms [-2.0, +0.4] |
| `relaxed-c2` | 10,000 | 16 runs | 184.6 ms [1.4-434.6] | 14 of 16 runs / 12 of 16 runs | 191.6 ms [19.8-274.4] | 16 of 16 runs / 13 of 16 runs | +13.1 ms [-180.3, +179.7] |
| `group-c1` | 200 | 12 runs | 21.8 ms [14.3-32.2] | 12 of 12 runs / 0 of 12 runs | 23.5 ms [14.6-29.4] | 12 of 12 runs / 0 of 12 runs | +0.4 ms [-2.7, +5.8] |
| `group-c1` | 1,000 | 12 runs | 21.2 ms [15.0-46.2] | 12 of 12 runs / 1 of 12 runs | 20.7 ms [15.2-29.4] | 12 of 12 runs / 0 of 12 runs | +0.1 ms [-6.7, +7.2] |
| `group-c1` | 10,000 | 12 runs | 36.3 ms [15.9-68.2] | 12 of 12 runs / 4 of 12 runs | 19.4 ms [11.3-26.9] | 12 of 12 runs / 0 of 12 runs | -18.1 ms [-30.9, -13.6] |
| `group-c2` | 200 | 12 runs | 22.4 ms [14.6-33.1] | 12 of 12 runs / 0 of 12 runs | 17.8 ms [15.0-28.0] | 12 of 12 runs / 0 of 12 runs | -0.6 ms [-10.4, +4.5] |
| `group-c2` | 1,000 | 12 runs | 16.2 ms [12.2-24.4] | 12 of 12 runs / 0 of 12 runs | 17.4 ms [14.6-36.6] | 12 of 12 runs / 0 of 12 runs | +0.2 ms [-2.3, +2.8] |
| `group-c2` | 10,000 | 12 runs | 61.1 ms [16.8-89.0] | 12 of 12 runs / 8 of 12 runs | 21.5 ms [13.5-32.6] | 12 of 12 runs / 0 of 12 runs | -39.4 ms [-57.9, -5.3] |
| `aa-c1` | 200 | 8 runs | 3.7 ms [3.0-6.5] | 0 of 8 runs / 0 of 8 runs | 4.7 ms [1.9-15.2] | 1 of 8 runs / 0 of 8 runs | +0.4 ms [-1.6, +2.6] |
| `aa-c1` | 1,000 | 8 runs | 4.8 ms [2.4-8.7] | 0 of 8 runs / 0 of 8 runs | 3.7 ms [1.9-6.8] | 0 of 8 runs / 0 of 8 runs | -1.2 ms [-3.0, +1.4] |
| `aa-c1` | 10,000 | 8 runs | 154.3 ms [2.9-436.0] | 6 of 8 runs / 5 of 8 runs | 204.6 ms [45.0-465.6] | 8 of 8 runs / 8 of 8 runs | +50.3 ms [-353.8, +445.0] |
| `aa-c2` | 200 | 8 runs | 4.0 ms [3.2-7.6] | 0 of 8 runs / 0 of 8 runs | 3.7 ms [2.8-5.5] | 0 of 8 runs / 0 of 8 runs | -0.2 ms [-2.9, +0.4] |
| `aa-c2` | 1,000 | 8 runs | 4.9 ms [2.9-12.5] | 1 of 8 runs / 0 of 8 runs | 4.4 ms [3.4-9.4] | 0 of 8 runs / 0 of 8 runs | -0.8 ms [-4.3, +0.3] |
| `aa-c2` | 10,000 | 8 runs | 186.5 ms [1.1-432.8] | 7 of 8 runs / 7 of 8 runs | 246.0 ms [16.2-459.7] | 8 of 8 runs / 7 of 8 runs | +59.1 ms [-132.8, +365.8] |

Three readings.

**(a) At 200 and 1,000 rows `relaxed` the longest statement is 0.5-21.5 ms in every server-run** (medians 3.1-6.2 ms), 0 of 128
server-runs over 40 ms, and no checkpoint run is inside the window in 125 of them (section 2). B - A is -1.0 to +2.0 ms, every CI
containing zero; two copies of A read -1.2 to +0.4 ms.

**(b) At 10,000 rows `relaxed` the longest statement is 400-480 ms in about two runs of three, in either binary,
and the A/A cell has the same spread.** A's median longest is 173.0 and 184.6 ms, B's 211.5 and 191.6 ms; B - A is
+54.0 ms [-414.3, +407.4] and +13.1 ms [-180.3, +179.7]; two copies of A read 154.3 and 204.6 ms (`cores = 1`) and 186.5 and
246.0 ms (`cores = 2`), with 5 of 8 and 8 of 8, 7 of 8 and 7 of 8 runs over 40 ms. **The per-run longest spans 1-473 ms, so a median over 8-16 runs is set by how many runs caught the stall, and this column is
not a result for or against BI.** The stall is a reactor iteration **outside every checkpoint run** (the run record reads
`sched_hold_out_run_longest_us` of 410-478 ms, grown during the arms in 14-16 of 16 runs in both binaries; the tables below), and it sits
in the 20-row `INSERT` arm in 8-16 of 16 runs (`cores = 2`: 16 of 16 B runs and 10 of 16 A runs). Its count tracks the WAL ring: at
`cores = 1`, `wal_ring_full` (drains the appender paid, `SHOW META`) is 15 per run at 10,000 rows `relaxed`, 2 at 1,000 and 0 at 200.
`[inferred]` from that correlation alone: the 60,000-row bulk burst fills the `relaxed` log ring and the appender drains it inline;
nothing in this run isolates it. A second reactor hold, 260-380 ms and outside any run, occurs while the *setup load* runs, in 3-5 of 16
runs of either binary at 200 and 1,000 rows (the "during setup" column); it is not attributed either.

#### relaxed-c1: the session core's longest reactor iteration (`SHOW META`, counters since boot), A then B

| rows | server | inside a checkpoint run, whole run: median / max | outside every run, during setup (load): median / max | outside every run, grown during the arms: runs of N / largest | statement-level: the arm holding the run (checkpoint_runs first increased), its max_us: median / max |
|---|---|---|---|---|---|
| 200 | A | 0.0 ms / 0.0 ms | 3.3 ms / 328.1 ms | 10 of 16 runs / 11.3 ms | - (0 runs) |
| 200 | B | 0.0 ms / 0.0 ms | 3.4 ms / 376.6 ms | 9 of 16 runs / 8.9 ms | - (0 runs) |
| 1,000 | A | 0.0 ms / 122.1 ms | 3.4 ms / 309.1 ms | 11 of 16 runs / 8.4 ms | 122.1 ms / 122.1 ms (1 runs) |
| 1,000 | B | 0.0 ms / 0.0 ms | 4.1 ms / 263.0 ms | 7 of 16 runs / 12.3 ms | - (0 runs) |
| 10,000 | A | 66.9 ms / 106.5 ms | 3.5 ms / 260.7 ms | 16 of 16 runs / 477.9 ms | 45.8 ms / 85.7 ms (16 runs) |
| 10,000 | B | 1.4 ms / 7.0 ms | 7.9 ms / 278.9 ms | 15 of 16 runs / 472.9 ms | 2.1 ms / 4.7 ms (16 runs) |

#### relaxed-c2: the session core's longest reactor iteration (`SHOW META`, counters since boot), A then B

| rows | server | inside a checkpoint run, whole run: median / max | outside every run, during setup (load): median / max | outside every run, grown during the arms: runs of N / largest | statement-level: the arm holding the run (checkpoint_runs first increased), its max_us: median / max |
|---|---|---|---|---|---|
| 200 | A | 0.0 ms / 0.0 ms | 3.0 ms / 260.4 ms | 12 of 16 runs / 10.7 ms | - (0 runs) |
| 200 | B | 0.0 ms / 0.0 ms | 3.2 ms / 259.0 ms | 11 of 16 runs / 12.1 ms | - (0 runs) |
| 1,000 | A | 0.0 ms / 131.5 ms | 3.4 ms / 257.7 ms | 11 of 16 runs / 21.4 ms | 107.6 ms / 126.5 ms (2 runs) |
| 1,000 | B | 0.0 ms / 0.0 ms | 3.4 ms / 257.5 ms | 9 of 16 runs / 13.9 ms | - (0 runs) |
| 10,000 | A | 89.2 ms / 129.0 ms | 3.2 ms / 289.0 ms | 14 of 16 runs / 434.3 ms | 9.5 ms / 115.5 ms (16 runs) |
| 10,000 | B | 1.4 ms / 17.3 ms | 3.1 ms / 266.8 ms | 14 of 16 runs / 409.9 ms | 1.8 ms / 4.3 ms (16 runs) |

**(c) The checkpoint run itself is where B differs, and it differs by the order's expected amount.** The first of the
two hold columns above is the counter BI-S1 added: A's cadence run is one reactor iteration, so its whole wall time is
its hold (**66.9 ms median, 106.5 ms worst at `cores = 1`; 89.2 and 129.0 ms at `cores = 2`**, 10,000 rows, 13 and 18 steps in
the run, 16 of 16 runs each); B's longest iteration overlapping the run is **1.4 ms median, 7.0 ms worst at `cores = 1`; 1.4
and 17.3 ms at `cores = 2`**, with the run's own wall time 25.8 ms (`cores = 1`) and 23.6 ms (`cores = 2`) median: the run is paced, and its reactor holds are short. The statement in the arm
where the run landed: **A 45.8 ms median (worst 85.7) and B 2.1 ms (worst 4.7) at `cores = 1`, A 9.5 ms (worst 115.5) and B 1.8 ms
(worst 4.3) at `cores = 2`**, 16 of 16 runs per cell. In `group`, where the run lands in every run and the 20 ms noise of a synced commit is
smaller than the hold:

#### group-c1: the session core's longest reactor iteration (`SHOW META`, counters since boot), A then B

| rows | server | inside a checkpoint run, whole run: median / max | outside every run, during setup (load): median / max | outside every run, grown during the arms: runs of N / largest | statement-level: the arm holding the run (checkpoint_runs first increased), its max_us: median / max |
|---|---|---|---|---|---|
| 200 | A | 10.4 ms / 13.4 ms | 10.4 ms / 278.4 ms | 7 of 12 runs / 32.1 ms | 15.4 ms / 32.2 ms (12 runs) |
| 200 | B | 1.9 ms / 7.7 ms | 6.3 ms / 273.6 ms | 10 of 12 runs / 29.2 ms | 19.9 ms / 27.9 ms (12 runs) |
| 1,000 | A | 15.5 ms / 20.4 ms | 8.2 ms / 299.3 ms | 9 of 12 runs / 46.1 ms | 16.1 ms / 30.4 ms (12 runs) |
| 1,000 | B | 1.9 ms / 3.1 ms | 8.9 ms / 278.9 ms | 9 of 12 runs / 28.4 ms | 15.4 ms / 22.1 ms (12 runs) |
| 10,000 | A | 35.7 ms / 65.7 ms | 94.8 ms / 306.5 ms | 6 of 12 runs / 28.3 ms | 36.3 ms / 68.2 ms (12 runs) |
| 10,000 | B | 3.0 ms / 7.9 ms | 18.7 ms / 279.8 ms | 6 of 12 runs / 25.7 ms | 14.5 ms / 25.8 ms (12 runs) |

#### group-c2: the session core's longest reactor iteration (`SHOW META`, counters since boot), A then B

| rows | server | inside a checkpoint run, whole run: median / max | outside every run, during setup (load): median / max | outside every run, grown during the arms: runs of N / largest | statement-level: the arm holding the run (checkpoint_runs first increased), its max_us: median / max |
|---|---|---|---|---|---|
| 200 | A | 10.4 ms / 19.9 ms | 2.9 ms / 4.5 ms | 5 of 12 runs / 5.4 ms | 17.7 ms / 27.0 ms (12 runs) |
| 200 | B | 0.7 ms / 1.4 ms | 2.9 ms / 256.5 ms | 9 of 12 runs / 11.6 ms | 17.2 ms / 28.0 ms (12 runs) |
| 1,000 | A | 13.9 ms / 23.1 ms | 2.8 ms / 252.7 ms | 7 of 12 runs / 7.7 ms | 15.6 ms / 24.4 ms (12 runs) |
| 1,000 | B | 0.8 ms / 3.5 ms | 3.0 ms / 5.6 ms | 8 of 12 runs / 12.3 ms | 16.1 ms / 36.6 ms (12 runs) |
| 10,000 | A | 70.5 ms / 88.1 ms | 7.1 ms / 299.1 ms | 4 of 12 runs / 6.5 ms | 61.1 ms / 89.0 ms (12 runs) |
| 10,000 | B | 1.0 ms / 6.3 ms | 3.2 ms / 10.2 ms | 7 of 12 runs / 31.2 ms | 16.5 ms / 22.0 ms (12 runs) |

**At 10,000 rows `group` the longest statement of a run is 36.3 ms (A) against 19.4 ms (B) at `cores = 1`, -18.1 ms
[-30.9, -13.6], and 61.1 against 21.5 ms at `cores = 2`, -39.4 ms [-57.9, -5.3], with 4 and 8 of 12 A runs over 40 ms
and none of B's.** At 200 and 1,000 rows `group` A's hold is 10-16 ms and B's 0.7-1.9 ms, and the per-run longest statement does
not separate (21.8 against 23.5 ms, 21.2 against 20.7 ms, 22.4 against 17.8 ms, 16.2 against 17.4 ms), because a synced
commit's own tail is 15-25 ms.

The longest statement of each arm (`max_us`, median over runs, A then B), `relaxed`:

#### relaxed-c1: the longest single statement of any timed arm, per run (controls excluded)

| rows | runs | A: median [min-max] | A: runs over 10 ms / over 40 ms | B: median [min-max] | B: runs over 10 ms / over 40 ms | B - A per run: median (95 % CI) | arm holding the longest, A / B |
|---|---|---|---|---|---|---|---|
| 200 | 16 runs | 4.3 ms [2.4-11.4] | 1 of 16 runs / 0 of 16 runs | 3.1 ms [2.0-8.2] | 0 of 16 runs / 0 of 16 runs | -0.4 ms [-2.0, +0.5] | bulk20 6, update-pk 4, insert-again 4 / update-pk 5, bulk20 5, delete-pk 4 |
| 1,000 | 16 runs | 4.0 ms [2.7-9.6] | 0 of 16 runs / 0 of 16 runs | 3.8 ms [2.4-9.1] | 0 of 16 runs / 0 of 16 runs | +0.5 ms [-1.5, +2.1] | bulk20 8, insert-again 6, range100 1 / bulk20 5, insert-again 4, update-pk 3 |
| 10,000 | 16 runs | 173.0 ms [3.6-453.4] | 12 of 16 runs / 12 of 16 runs | 211.5 ms [3.0-472.9] | 8 of 16 runs / 8 of 16 runs | +54.0 ms [-414.3, +407.4] | bulk20 8, insert-again 4, range100 4 / bulk20 8, insert-again 3, delete-pk 2 |

#### relaxed-c1: the longest statement of each arm (max_us), median over runs, A then B

| rows | INSERT, pk omitted | INSERT again | SELECT by pk | UPDATE by pk | DELETE by pk | bulk INSERT, 20 rows | whole-relation walk |
|---|---|---|---|---|---|---|---|
| 200 | 0.13 -> 0.13 ms | 1.78 -> 1.75 ms | 0.17 -> 0.16 ms | 2.54 -> 0.94 ms | 0.18 -> 0.51 ms | 3.31 -> 2.20 ms | 1.10 -> 1.01 ms |
| 1,000 | 0.13 -> 0.13 ms | 3.10 -> 3.13 ms | 0.16 -> 0.17 ms | 0.72 -> 2.98 ms | 0.10 -> 1.60 ms | 3.44 -> 3.33 ms | 1.54 -> 1.18 ms |
| 10,000 | 0.13 -> 0.13 ms | 2.32 -> 2.99 ms | 0.17 -> 0.16 ms | 1.82 -> 3.11 ms | 0.13 -> 2.08 ms | 140.32 -> 208.54 ms | 45.78 -> 2.10 ms |

## 7. The extreme: `checkpoint_interval_ms = 1000`

Rule 4b: a cell that never approached a limit measured the harness. At `checkpoint_interval_ms = 5000` and `relaxed`,
200 and 1,000 rows never meet a checkpoint; the dense cells run the same shapes with the cadence at 1000 ms, so 3-9
runs land per server and run. `relaxed`, 10 runs per cell, 200, 1,000 and 10,000 rows; the A/A cell is at
1,000 and 10,000 rows, `cores = 1`.

#### dense200-c1: checkpoint runs of the session's core during the arms, and the run record at the end of each run

| rows | server | session core | checkpoint runs landing in the arms: median [min-max] | runs with at least one | last run's wall time (`checkpoint_last_wall_us`): median / max | last run's steps: median | longest reactor iteration inside a run (`sched_hold_in_run_longest_us`): median / max | iterations over 10 ms inside runs, sum | `pool_resident` max / frames | `pool_refused`, sum |
|---|---|---|---|---|---|---|---|---|---|---|
| 200 | A | core 0 | 4 runs [4-5] | 10 of 10 runs | 26.6 ms / 293.0 ms | 3 steps | 163.0 ms / 495.8 ms | 40 iterations | 654 / 65,536 frames | 0 refusals |
| 200 | B | core 0 | 4 runs [4-4] | 10 of 10 runs | 151.8 ms / 459.5 ms | 4 steps | 2.4 ms / 126.7 ms | 1 iterations | 654 / 65,536 frames | 0 refusals |

#### dense200-c2: checkpoint runs of the session's core during the arms, and the run record at the end of each run

| rows | server | session core | checkpoint runs landing in the arms: median [min-max] | runs with at least one | last run's wall time (`checkpoint_last_wall_us`): median / max | last run's steps: median | longest reactor iteration inside a run (`sched_hold_in_run_longest_us`): median / max | iterations over 10 ms inside runs, sum | `pool_resident` max / frames | `pool_refused`, sum |
|---|---|---|---|---|---|---|---|---|---|---|
| 200 | A | core 0/1 | 4 runs [3-5] | 10 of 10 runs | 18.3 ms / 362.8 ms | 2 steps | 203.5 ms / 591.6 ms | 27 iterations | 830 / 65,536 frames | 0 refusals |
| 200 | B | core 0/1 | 3 runs [3-4] | 10 of 10 runs | 238.6 ms / 534.0 ms | 4 steps | 1.3 ms / 94.5 ms | 1 iterations | 830 / 65,536 frames | 0 refusals |

#### dense-c1: checkpoint runs of the session's core during the arms, and the run record at the end of each run

| rows | server | session core | checkpoint runs landing in the arms: median [min-max] | runs with at least one | last run's wall time (`checkpoint_last_wall_us`): median / max | last run's steps: median | longest reactor iteration inside a run (`sched_hold_in_run_longest_us`): median / max | iterations over 10 ms inside runs, sum | `pool_resident` max / frames | `pool_refused`, sum |
|---|---|---|---|---|---|---|---|---|---|---|
| 1,000 | A | core 0 | 4 runs [4-5] | 10 of 10 runs | 15.0 ms / 28.7 ms | 2 steps | 227.3 ms / 472.7 ms | 39 iterations | 1,323 / 65,536 frames | 0 refusals |
| 1,000 | B | core 0 | 4 runs [4-5] | 10 of 10 runs | 14.7 ms / 357.0 ms | 2 steps | 1.7 ms / 114.0 ms | 4 iterations | 1,323 / 65,536 frames | 0 refusals |
| 10,000 | A | core 0 | 9 runs [9-9] | 10 of 10 runs | 8.0 ms / 13.4 ms | 1 steps | 47.0 ms / 245.1 ms | 48 iterations | 809 / 65,536 frames | 0 refusals |
| 10,000 | B | core 0 | 9 runs [9-9] | 10 of 10 runs | 11.0 ms / 13.5 ms | 1 steps | 2.4 ms / 447.0 ms | 4 iterations | 809 / 65,536 frames | 0 refusals |

#### dense-c2: checkpoint runs of the session's core during the arms, and the run record at the end of each run

| rows | server | session core | checkpoint runs landing in the arms: median [min-max] | runs with at least one | last run's wall time (`checkpoint_last_wall_us`): median / max | last run's steps: median | longest reactor iteration inside a run (`sched_hold_in_run_longest_us`): median / max | iterations over 10 ms inside runs, sum | `pool_resident` max / frames | `pool_refused`, sum |
|---|---|---|---|---|---|---|---|---|---|---|
| 1,000 | A | core 0/1 | 4 runs [3-5] | 10 of 10 runs | 13.5 ms / 29.3 ms | 1 steps | 39.5 ms / 577.7 ms | 30 iterations | 1,498 / 65,536 frames | 0 refusals |
| 1,000 | B | core 0/1 | 4 runs [3-5] | 10 of 10 runs | 9.0 ms / 649.0 ms | 1 steps | 1.3 ms / 158.8 ms | 2 iterations | 1,498 / 65,536 frames | 0 refusals |
| 10,000 | A | core 0/1 | 9 runs [7-9] | 10 of 10 runs | 7.5 ms / 11.8 ms | 1 steps | 241.3 ms / 565.3 ms | 43 iterations | 1,156 / 65,536 frames | 0 refusals |
| 10,000 | B | core 0/1 | 9 runs [8-9] | 10 of 10 runs | 10.3 ms / 20.9 ms | 1 steps | 4.0 ms / 261.6 ms | 8 iterations | 1,156 / 65,536 frames | 0 refusals |

#### aadense-c1: checkpoint runs of the session's core during the arms, and the run record at the end of each run

| rows | server | session core | checkpoint runs landing in the arms: median [min-max] | runs with at least one | last run's wall time (`checkpoint_last_wall_us`): median / max | last run's steps: median | longest reactor iteration inside a run (`sched_hold_in_run_longest_us`): median / max | iterations over 10 ms inside runs, sum | `pool_resident` max / frames | `pool_refused`, sum |
|---|---|---|---|---|---|---|---|---|---|---|
| 1,000 | A | core 0 | 5 runs [5-5] | 10 of 10 runs | 7.4 ms / 12.0 ms | 1 steps | 284.9 ms / 590.5 ms | 39 iterations | 1,323 / 65,536 frames | 0 refusals |
| 1,000 | B | core 0 | 5 runs [4-5] | 10 of 10 runs | 11.4 ms / 32.6 ms | 1 steps | 304.0 ms / 644.3 ms | 42 iterations | 1,323 / 65,536 frames | 0 refusals |
| 10,000 | A | core 0 | 9 runs [9-9] | 10 of 10 runs | 8.0 ms / 18.1 ms | 1 steps | 130.1 ms / 497.2 ms | 56 iterations | 809 / 65,536 frames | 0 refusals |
| 10,000 | B | core 0 | 9 runs [9-9] | 10 of 10 runs | 7.6 ms / 18.7 ms | 1 steps | 45.5 ms / 525.0 ms | 55 iterations | 809 / 65,536 frames | 0 refusals |

| cell | rows | runs | A: longest statement of a run, median [min-max] | A: runs over 10 ms / over 40 ms | B: median [min-max] | B: runs over 10 ms / over 40 ms | B - A per run: median (95 % CI) |
|---|---|---|---|---|---|---|---|
| `dense200-c1` | 200 | 10 runs | 148.6 ms [21.7-467.4] | 10 of 10 runs / 6 of 10 runs | 3.4 ms [1.7-116.5] | 1 of 10 runs / 1 of 10 runs | -143.6 ms [-270.4, -21.9] |
| `dense200-c2` | 200 | 10 runs | 180.5 ms [12.2-591.7] | 10 of 10 runs / 5 of 10 runs | 5.2 ms [3.1-14.2] | 3 of 10 runs / 0 of 10 runs | -172.6 ms [-484.6, -18.3] |
| `dense-c1` | 1,000 | 10 runs | 227.4 ms [14.5-421.8] | 10 of 10 runs / 7 of 10 runs | 3.9 ms [3.1-114.1] | 3 of 10 runs / 3 of 10 runs | -186.4 ms [-396.2, +27.1] |
| `dense-c1` | 10,000 | 10 runs | 32.0 ms [10.2-475.1] | 10 of 10 runs / 4 of 10 runs | 395.9 ms [3.2-465.3] | 8 of 10 runs / 6 of 10 runs | +364.0 ms [-441.2, +437.1] |
| `dense-c2` | 1,000 | 10 runs | 33.6 ms [11.4-571.9] | 10 of 10 runs / 5 of 10 runs | 6.6 ms [3.3-103.7] | 4 of 10 runs / 1 of 10 runs | -23.6 ms [-425.7, -3.4] |
| `dense-c2` | 10,000 | 10 runs | 274.6 ms [64.5-515.2] | 10 of 10 runs / 10 of 10 runs | 8.8 ms [3.3-210.5] | 4 of 10 runs / 2 of 10 runs | -255.3 ms [-438.2, -90.0] |
| `aadense-c1` | 1,000 | 10 runs | 250.1 ms [37.4-590.8] | 10 of 10 runs / 9 of 10 runs | 284.7 ms [10.8-644.4] | 10 of 10 runs / 8 of 10 runs | +17.7 ms [-445.4, +430.4] |
| `aadense-c1` | 10,000 | 10 runs | 26.2 ms [10.3-499.9] | 10 of 10 runs / 3 of 10 runs | 291.3 ms [23.8-519.7] | 10 of 10 runs / 8 of 10 runs | +201.1 ms [-184.9, +482.6] |

**At the dense cadence the checkpoint is in every measured window, and there B and A separate.** `relaxed`, 10 runs per cell,
3-9 runs of the cadence checkpoint per server and run.

- **200 and 1,000 rows (four A/B cells).** A's per-run longest statement is 148.6, 180.5, 227.4 and 33.6 ms (median) with 6, 5, 7 and 5 of 10 runs
  over 40 ms; B's is 3.4, 5.2, 3.9 and 6.6 ms with 1, 0, 3 and 1 of 10. B - A is -143.6, -172.6, -186.4 and -23.6 ms; three of the four CIs exclude zero
  (the 1,000-row `cores = 1` interval reaches +27.1 ms). The reactor hold inside a run is A 163.0, 203.5, 227.3 and 39.5 ms (median), B 2.4, 1.3, 1.7 and
  1.3 ms. The statement in the arm where the run landed: A 10.5 / 10.8 / 12.0 / 12.7 ms median (worst 19.0-84.9 ms), B 0.2 / 0.2 / 0.3 / 0.9 ms.
- **The floor.** Two copies of A at the same cadence read 250.1 and 284.7 ms at 1,000 rows (9 and 8 of 10 runs over 40 ms) with an in-run hold of
  284.9 and 304.0 ms: an A-class binary stalls here in most runs and B does not. The CI of that pair, [-445.4, +430.4] ms, is as wide as the stall itself
  because the stall is on or off per run, so the evidence that B differs is the count: **5 of 40 B server-runs over 40 ms against 23 of 40 for A and
  17 of 20 for the two copies of A.**
- **10,000 rows: no reading.** The ring stall of section 6 is present at this cadence. A against B reads +364.0 ms [-441.2, +437.1] (`cores = 1`) and
  -255.3 ms [-438.2, -90.0] (`cores = 2`), and two copies of A read +201.1 ms [-184.9, +482.6].
- **The p50 is flat** at the dense cadence (tables below): the pk statements read -0.55 to +0.25 µs, the 20-row `INSERT` -1.80 to +1.05 µs. The one
  p50 CI that excludes zero with B slower is the walk at 1,000 rows, `cores = 2`, +0.95 µs [+0.20, +2.70] (0.6 %); there is no A/A cell at `cores = 2` and the
  A/A pair at `cores = 1` reads +0.40 µs [-0.50, +1.20] on the same cell and +3.40 µs [-1.50, +12.00] at 10,000 rows. A throughput column is not flat: the walk's rate at 200 rows is 8,178 tps in A and 10,961 tps in B
  (`cores = 1`, +48.9 % [-0.5, +99.8]) and 7,796 against 10,664 tps at `cores = 2` (+56.3 % [-0.1, +157.0]); B's rate equals the walk's rate with no checkpoint in
  the window (A's 10,963 and 10,716 tps at 5000 ms, section 4). A's stalls sit inside the arm's elapsed time.
- **B's runs are longer, as the order accepts (W1).** The last run's wall time (median) is 151.8 ms (`cores = 1`) and 238.6 ms (`cores = 2`) in B against 26.6 and
  18.3 ms in A at 200 rows, and 9.0-14.7 ms in B against 7.5-15.0 ms in A at 1,000 and 10,000 rows. BI-Q10 (iv) reports run length without a bar.
- **B's tail is not zero.** The worst iteration B's runs overlapped is 114.0 and 158.8 ms at 1,000 rows, 126.7 and 94.5 ms at 200 rows, 447.0 and 261.6 ms at 10,000
  rows. That counter takes every iteration the run overlaps, so a ring stall (section 6) that lands inside a run is in it; nothing in this data separates the two.

p50 at the dense cadence (checkpoint runs inside every run):

#### dense-c1: p50 delta B - A (95 % CI of the median over runs) and tps change

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | - | -0.05 µs [-0.20, +0.10]; +0.7 % tps [-0.0, +4.7] | -0.10 µs [-0.20, +0.10]; +10.3 % tps [-0.1, +22.7] |
| INSERT again (replicate) | - | -0.10 µs [-0.25, +0.15]; -0.8 % tps [-3.4, +7.7] | -0.10 µs [-0.25, +0.10]; +0.3 % tps [-0.2, +2.1] |
| SELECT by pk | - | -0.55 µs [-0.80, -0.10]; +1.4 % tps [+0.4, +29.4] | +0.05 µs [-0.30, +0.45]; +0.5 % tps [-0.7, +3.1] |
| UPDATE by pk | - | -0.40 µs [-0.90, +0.20]; +4.9 % tps [-1.1, +22.2] | +0.05 µs [-0.40, +0.40]; +1.0 % tps [-1.4, +5.8] |
| DELETE by pk | - | -0.05 µs [-0.20, +0.30]; +0.8 % tps [-0.4, +8.1] | +0.00 µs [-0.10, +0.15]; -1.6 % tps [-4.7, +0.9] |
| bulk INSERT, 20 rows | - | -0.50 µs [-1.10, +0.30]; +2.6 % tps [+1.5, +5.7] | +0.00 µs [-0.70, +0.45]; -55.7 % tps [-58.6, +140.5] |
| whole-relation walk (COUNT/MIN/MAX, 100-id window) | - | +0.10 µs [-0.80, +1.30]; +42.5 % tps [-11.6, +87.7] | +1.50 µs [-3.20, +8.80]; +1.4 % tps [+0.3, +2.2] |
| SHOW META | - | +0.75 µs [-0.55, +2.00]; -1.8 % tps [-3.1, +0.6] | +0.85 µs [+0.70, +1.25]; -1.2 % tps [-2.0, +2.2] |
| SHOW NAMESPACES (control) | - | +0.00 µs [-0.10, +0.20]; +0.2 % tps [-0.4, +0.5] | +0.15 µs [-0.05, +0.20]; +0.7 % tps [-1.3, +25.4] |
| SET ISOLATION LEVEL (control) | - | +0.00 µs [-0.10, +0.10]; -0.0 % tps [-0.3, +0.2] | +0.00 µs [-0.10, +0.05]; +0.1 % tps [-1.5, +1.3] |

#### dense-c2: p50 delta B - A (95 % CI of the median over runs) and tps change

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | - | -0.15 µs [-0.25, -0.05]; +0.8 % tps [+0.1, +6.4] | -0.25 µs [-0.50, +0.10]; +5.7 % tps [+0.4, +24.6] |
| INSERT again (replicate) | - | -0.15 µs [-0.30, -0.05]; +4.8 % tps [+0.6, +7.8] | -0.35 µs [-0.50, +0.00]; +3.0 % tps [+0.9, +7.1] |
| SELECT by pk | - | -0.25 µs [-1.00, +0.20]; +0.6 % tps [-0.1, +29.2] | -0.40 µs [-1.20, -0.10]; +1.3 % tps [+0.3, +3.2] |
| UPDATE by pk | - | -0.30 µs [-0.70, -0.05]; +0.8 % tps [+0.1, +11.8] | -0.30 µs [-0.55, +0.20]; +0.8 % tps [-0.6, +5.1] |
| DELETE by pk | - | -0.10 µs [-0.45, +0.30]; -0.3 % tps [-2.5, +1.1] | +0.10 µs [-0.10, +0.45]; -0.1 % tps [-2.3, +1.7] |
| bulk INSERT, 20 rows | - | -0.60 µs [-1.30, +0.00]; +2.0 % tps [+0.7, +5.4] | -1.80 µs [-2.90, +0.30]; +96.0 % tps [+7.2, +145.1] |
| whole-relation walk (COUNT/MIN/MAX, 100-id window) | - | +0.95 µs [+0.20, +2.70]; +0.6 % tps [-2.8, +94.7] | -2.45 µs [-25.15, +3.90]; +1.3 % tps [-0.1, +4.1] |
| SHOW META | - | +0.65 µs [+0.45, +0.80]; -1.3 % tps [-1.6, -1.0] | +0.90 µs [+0.75, +1.15]; -1.8 % tps [-2.6, +0.1] |
| SHOW NAMESPACES (control) | - | -0.10 µs [-0.15, +0.10]; +0.2 % tps [-0.3, +1.1] | -0.10 µs [-0.35, +0.15]; +0.8 % tps [-0.0, +1.5] |
| SET ISOLATION LEVEL (control) | - | +0.05 µs [-0.10, +0.10]; +0.0 % tps [-0.4, +0.5] | -0.05 µs [-0.20, +0.05]; +0.2 % tps [-0.3, +0.7] |

#### dense200-c1: p50 delta B - A (95 % CI of the median over runs) and tps change

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.30, -0.05]; +1.6 % tps [-0.0, +6.6] | - | - |
| INSERT again (replicate) | -0.20 µs [-0.30, -0.10]; +4.6 % tps [-1.6, +9.6] | - | - |
| SELECT by pk | +0.20 µs [-0.20, +0.60]; +0.1 % tps [-0.5, +0.9] | - | - |
| UPDATE by pk | +0.25 µs [-0.15, +0.70]; +3.2 % tps [+0.9, +10.4] | - | - |
| DELETE by pk | +0.00 µs [-0.30, +0.20]; +0.7 % tps [-1.1, +13.4] | - | - |
| bulk INSERT, 20 rows | +0.45 µs [+0.00, +0.80]; +3.0 % tps [-0.7, +7.8] | - | - |
| whole-relation walk (COUNT/MIN/MAX, 100-id window) | +0.35 µs [-0.50, +1.65]; +48.9 % tps [-0.5, +99.8] | - | - |
| SHOW META | +1.00 µs [+0.70, +1.45]; +7.5 % tps [-1.2, +11.2] | - | - |
| SHOW NAMESPACES (control) | +0.20 µs [+0.05, +0.35]; -0.4 % tps [-0.9, -0.2] | - | - |
| SET ISOLATION LEVEL (control) | +0.00 µs [+0.00, +0.10]; -0.2 % tps [-0.5, +0.6] | - | - |

#### dense200-c2: p50 delta B - A (95 % CI of the median over runs) and tps change

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | -0.25 µs [-0.50, -0.10]; +0.7 % tps [+0.4, +3.9] | - | - |
| INSERT again (replicate) | -0.25 µs [-0.50, -0.10]; -0.2 % tps [-2.4, +7.0] | - | - |
| SELECT by pk | -0.35 µs [-0.65, +0.10]; +1.2 % tps [+0.5, +6.4] | - | - |
| UPDATE by pk | -0.35 µs [-0.80, +0.10]; +0.5 % tps [-0.0, +3.6] | - | - |
| DELETE by pk | -0.05 µs [-0.40, +0.20]; -1.5 % tps [-4.6, +6.7] | - | - |
| bulk INSERT, 20 rows | +1.05 µs [-1.10, +1.80]; +1.0 % tps [-1.7, +6.3] | - | - |
| whole-relation walk (COUNT/MIN/MAX, 100-id window) | +0.40 µs [-0.50, +1.00]; +56.3 % tps [-0.0, +174.0] | - | - |
| SHOW META | +0.65 µs [+0.50, +0.80]; -1.3 % tps [-2.3, -0.1] | - | - |
| SHOW NAMESPACES (control) | +0.00 µs [-0.10, +0.20]; -0.5 % tps [-1.5, +0.4] | - | - |
| SET ISOLATION LEVEL (control) | -0.05 µs [-0.15, +0.05]; -0.5 % tps [-1.0, +0.4] | - | - |

#### aadense-c1: p50 delta B - A (95 % CI of the median over runs) and tps change

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | - | +0.05 µs [-0.20, +0.30]; -0.2 % tps [-4.1, +0.4] | +0.05 µs [-0.10, +0.20]; -2.5 % tps [-21.8, +21.1] |
| INSERT again (replicate) | - | +0.10 µs [-0.20, +0.30]; -5.5 % tps [-9.4, +9.6] | +0.00 µs [-0.20, +0.10]; -0.4 % tps [-1.3, +8.0] |
| SELECT by pk | - | +0.20 µs [-0.30, +0.35]; +1.3 % tps [-11.4, +16.4] | +0.20 µs [-0.20, +0.80]; -1.5 % tps [-4.3, +3.1] |
| UPDATE by pk | - | +0.10 µs [-0.40, +0.40]; +25.9 % tps [-29.6, +38.5] | +0.30 µs [+0.00, +1.05]; +0.7 % tps [-1.7, +4.2] |
| DELETE by pk | - | +0.05 µs [-0.05, +0.30]; +0.1 % tps [-1.8, +2.3] | -0.15 µs [-0.25, +0.00]; -1.1 % tps [-3.3, +0.8] |
| bulk INSERT, 20 rows | - | -0.20 µs [-0.70, +0.50]; +2.0 % tps [-2.0, +8.9] | -0.25 µs [-0.55, +0.65]; -13.8 % tps [-59.6, +61.2] |
| whole-relation walk (COUNT/MIN/MAX, 100-id window) | - | +0.40 µs [-0.50, +1.20]; +18.9 % tps [-52.0, +121.6] | +3.40 µs [-1.50, +12.00]; -1.4 % tps [-2.2, -0.0] |
| SHOW META | - | -0.20 µs [-0.40, -0.05]; +0.7 % tps [-1.9, +4.4] | +0.00 µs [-0.30, +0.45]; +0.0 % tps [-1.1, +0.4] |
| SHOW NAMESPACES (control) | - | -0.05 µs [-0.10, +0.10]; +0.2 % tps [-0.5, +0.5] | +0.00 µs [-0.20, +0.10]; -0.1 % tps [-20.5, +28.4] |
| SET ISOLATION LEVEL (control) | - | +0.00 µs [-0.10, +0.10]; +0.4 % tps [-0.4, +0.9] | +0.05 µs [-0.10, +0.15]; -0.3 % tps [-1.4, +1.4] |

## 8. The A/A cell: what two copies of one binary do

#### aa-c1: p50 delta B - A (95 % CI of the median over runs) and tps change

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | +0.10 µs [-0.10, +0.10]; +0.3 % tps [-0.5, +0.6] | +0.05 µs [-0.10, +0.30]; -0.6 % tps [-0.9, +0.4] | -0.10 µs [-0.20, +0.20]; +0.1 % tps [-0.4, +0.4] |
| INSERT again (replicate) | +0.05 µs [+0.00, +0.20]; -0.2 % tps [-4.2, +2.5] | +0.00 µs [-0.20, +0.30]; -1.0 % tps [-2.9, +1.9] | +0.00 µs [-0.20, +0.00]; +1.4 % tps [+0.3, +3.1] |
| SELECT by pk | -0.15 µs [-0.50, +0.30]; +0.1 % tps [-0.3, +0.6] | +0.45 µs [+0.00, +0.90]; -0.5 % tps [-1.2, +1.3] | +0.15 µs [-0.35, +0.60]; -0.1 % tps [-1.1, +0.7] |
| UPDATE by pk | -0.10 µs [-0.60, +0.20]; +2.2 % tps [-0.5, +3.1] | +0.30 µs [-0.30, +0.80]; -0.9 % tps [-1.5, +2.3] | +0.40 µs [+0.05, +0.60]; +0.1 % tps [-2.9, +1.9] |
| DELETE by pk | +0.00 µs [-0.20, +0.10]; -0.2 % tps [-2.9, +3.7] | -0.05 µs [-0.20, +0.20]; -1.7 % tps [-2.3, +1.2] | +0.05 µs [-0.20, +0.10]; -0.8 % tps [-1.7, +0.7] |
| bulk INSERT, 20 rows | -0.05 µs [-0.40, +0.50]; -0.6 % tps [-3.6, +0.2] | +0.20 µs [-0.90, +0.70]; +1.5 % tps [+0.7, +4.1] | -0.05 µs [-0.70, +0.80]; -21.8 % tps [-58.3, +132.6] |
| whole-relation walk (COUNT/MIN/MAX, 100-id window) | -0.05 µs [-0.50, +0.70]; +0.2 % tps [-1.5, +2.1] | +0.15 µs [-1.30, +1.00]; -0.5 % tps [-1.0, +0.3] | -0.10 µs [-7.90, +15.30]; -0.1 % tps [-0.5, +0.6] |
| SHOW META | +0.20 µs [-0.25, +0.40]; -0.4 % tps [-0.7, +0.8] | +0.20 µs [-0.20, +0.60]; -0.6 % tps [-1.0, +0.7] | -0.30 µs [-0.80, +0.60]; +0.3 % tps [-0.8, +1.9] |
| SHOW NAMESPACES (control) | +0.05 µs [-0.10, +0.20]; -0.1 % tps [-0.6, +1.0] | +0.15 µs [-0.10, +0.20]; +0.2 % tps [-0.3, +0.9] | -0.05 µs [-0.20, +0.05]; +0.7 % tps [-0.0, +2.0] |
| SET ISOLATION LEVEL (control) | +0.05 µs [-0.10, +0.10]; +0.6 % tps [-0.6, +1.4] | +0.10 µs [+0.00, +0.20]; -0.4 % tps [-0.6, +0.2] | +0.00 µs [-0.10, +0.10]; +0.0 % tps [-0.7, +0.4] |

#### aa-c2: p50 delta B - A (95 % CI of the median over runs) and tps change

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | -0.30 µs [-0.70, -0.20]; +0.9 % tps [+0.4, +1.5] | +0.05 µs [-0.20, +0.40]; -0.4 % tps [-1.2, +0.1] | -0.20 µs [-0.80, +0.30]; -0.1 % tps [-2.1, +4.4] |
| INSERT again (replicate) | -0.25 µs [-0.30, +0.10]; +1.0 % tps [-0.5, +2.5] | +0.00 µs [-0.30, +0.30]; +0.2 % tps [-1.9, +1.7] | +0.10 µs [-0.80, +0.20]; -1.1 % tps [-1.5, +0.5] |
| SELECT by pk | -0.50 µs [-1.00, -0.20]; +1.0 % tps [+0.7, +1.8] | +0.80 µs [-0.80, +1.20]; -0.9 % tps [-1.3, +0.8] | +0.05 µs [-0.70, +0.80]; -0.1 % tps [-1.8, +1.9] |
| UPDATE by pk | -0.30 µs [-0.80, +0.20]; +0.4 % tps [-0.6, +2.8] | +0.25 µs [-0.70, +0.60]; +1.4 % tps [-2.7, +1.7] | -0.15 µs [-0.60, +0.20]; +0.1 % tps [-2.2, +2.2] |
| DELETE by pk | -0.25 µs [-1.15, +0.00]; +0.7 % tps [-0.1, +1.8] | +0.30 µs [-0.20, +0.80]; -0.2 % tps [-2.8, +1.9] | -0.10 µs [-0.50, +0.20]; +0.3 % tps [-3.4, +0.9] |
| bulk INSERT, 20 rows | -0.40 µs [-3.40, +0.20]; +0.6 % tps [-0.6, +5.5] | -1.30 µs [-3.50, +3.60]; +2.9 % tps [-0.4, +6.8] | +0.55 µs [-4.60, +2.10]; -14.4 % tps [-56.5, +28.9] |
| whole-relation walk (COUNT/MIN/MAX, 100-id window) | -0.75 µs [-1.20, +0.10]; +1.3 % tps [-0.7, +2.3] | -0.15 µs [-2.20, +1.90]; -0.0 % tps [-2.8, +1.6] | +1.75 µs [-4.90, +9.90]; +0.4 % tps [-1.4, +2.9] |
| SHOW META | -0.45 µs [-0.70, -0.20]; +0.8 % tps [+0.1, +1.6] | -0.15 µs [-0.75, +0.30]; -0.1 % tps [-0.4, +1.3] | -0.20 µs [-0.50, +0.15]; +0.4 % tps [-0.6, +0.6] |
| SHOW NAMESPACES (control) | -0.10 µs [-0.20, +0.10]; +0.5 % tps [+0.3, +0.9] | +0.10 µs [-0.20, +0.20]; +0.0 % tps [-1.7, +0.9] | -0.15 µs [-0.30, +0.70]; -0.4 % tps [-2.6, +2.1] |
| SET ISOLATION LEVEL (control) | +0.00 µs [-0.10, +0.10]; +0.0 % tps [-0.2, +0.3] | +0.10 µs [+0.00, +0.10]; +0.1 % tps [-1.3, +1.0] | -0.15 µs [-0.30, +0.00]; +0.8 % tps [+0.6, +1.3] |

Every B - A of this section is the floor. **Two copies of A read -0.50 to +0.80 µs on the pk shapes**, with CIs excluding
zero in several cells (`cores = 1`: the `UPDATE` at 10,000 rows +0.40 µs [+0.05, +0.60]; `cores = 2`: the `INSERT` at 200 rows
-0.30 µs [-0.70, -0.20], the pk `SELECT` at 200 rows -0.50 µs [-1.00, -0.20]), and the 20-row `INSERT` and the walk
read -1.30 to +1.75 µs at `cores = 2` and -0.10 to +0.20 µs at `cores = 1`, with CIs up to 23 µs wide. 8 runs per cell are fewer than the 16 of the A/B cells and widen their CIs.

#### aa-c1: latency distribution, median over runs of each run's percentile, A then B

| rows | statement | server | statements | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| 10,000 | whole-relation walk (COUNT/MIN/MAX, 100-id window) | A | 24,000 statements | 734.3 µs | 785.2 µs | 808.4 µs | 858.7 µs | 956.2 µs |
| 10,000 | whole-relation walk (COUNT/MIN/MAX, 100-id window) | B | 24,000 statements | 733.1 µs | 783.9 µs | 807.0 µs | 864.5 µs | 979.0 µs |

### The reading used for BI-Q10 (iii)

A `relaxed` p50 delta is **inside the noise** when its median is 0.2 µs or less (BA's close floor) or its 95 % CI
overlaps the A/A cell's CI of the same shape, size and core count. The test is generous where the A/A CI is wide (8 runs), so the verdict (section 11) adds a stricter reading. Applied to every `relaxed` cell:

| cores | shape | rows | B - A p50 (95 % CI), 16 runs | A/A p50 delta (95 % CI), 8 runs | reading |
|---|---|---|---|---|---|
| 1 | INSERT, pk omitted | 200 | -0.10 µs [-0.30, +0.00] | +0.10 µs [-0.10, +0.10] | inside (median within 0.2 µs) |
| 1 | INSERT, pk omitted | 1,000 | -0.10 µs [-0.20, +0.10] | +0.05 µs [-0.10, +0.30] | inside (median within 0.2 µs) |
| 1 | INSERT, pk omitted | 10,000 | -0.15 µs [-0.30, -0.10] | -0.10 µs [-0.20, +0.20] | inside (median within 0.2 µs) |
| 1 | INSERT again (replicate) | 200 | -0.10 µs [-0.20, +0.00] | +0.05 µs [+0.00, +0.20] | inside (median within 0.2 µs) |
| 1 | INSERT again (replicate) | 1,000 | +0.00 µs [-0.20, +0.20] | +0.00 µs [-0.20, +0.30] | inside (median within 0.2 µs) |
| 1 | INSERT again (replicate) | 10,000 | -0.20 µs [-0.30, +0.10] | +0.00 µs [-0.20, +0.00] | inside (CI overlaps the A/A CI) |
| 1 | SELECT by pk | 200 | -0.10 µs [-0.30, +0.20] | -0.15 µs [-0.50, +0.30] | inside (median within 0.2 µs) |
| 1 | SELECT by pk | 1,000 | -0.25 µs [-0.80, +0.10] | +0.45 µs [+0.00, +0.90] | inside (CI overlaps the A/A CI) |
| 1 | SELECT by pk | 10,000 | -0.40 µs [-0.60, -0.10] | +0.15 µs [-0.50, +0.60] | inside (CI overlaps the A/A CI) |
| 1 | UPDATE by pk | 200 | +0.05 µs [-0.45, +0.40] | -0.10 µs [-0.60, +0.20] | inside (median within 0.2 µs) |
| 1 | UPDATE by pk | 1,000 | -0.55 µs [-0.80, -0.30] | +0.30 µs [-0.30, +0.80] | inside (CI overlaps the A/A CI) |
| 1 | UPDATE by pk | 10,000 | -0.15 µs [-0.60, +0.05] | +0.40 µs [-0.10, +0.60] | inside (median within 0.2 µs) |
| 1 | DELETE by pk | 200 | +0.00 µs [-0.10, +0.10] | +0.00 µs [-0.20, +0.10] | inside (median within 0.2 µs) |
| 1 | DELETE by pk | 1,000 | -0.05 µs [-0.10, +0.10] | -0.05 µs [-0.20, +0.20] | inside (median within 0.2 µs) |
| 1 | DELETE by pk | 10,000 | +0.00 µs [+0.00, +0.20] | +0.05 µs [-0.20, +0.10] | inside (median within 0.2 µs) |
| 1 | bulk INSERT, 20 rows | 200 | -0.05 µs [-0.20, +0.30] | -0.05 µs [-0.40, +0.50] | inside (median within 0.2 µs) |
| 1 | bulk INSERT, 20 rows | 1,000 | +0.10 µs [-0.20, +0.30] | +0.20 µs [-0.90, +0.70] | inside (median within 0.2 µs) |
| 1 | bulk INSERT, 20 rows | 10,000 | -0.20 µs [-0.70, +0.40] | -0.05 µs [-0.70, +0.80] | inside (CI overlaps the A/A CI) |
| 1 | whole-relation walk | 200 | +0.00 µs [-0.60, +0.80] | -0.05 µs [-0.50, +0.70] | inside (median within 0.2 µs) |
| 1 | whole-relation walk | 1,000 | +0.15 µs [-0.40, +1.00] | +0.15 µs [-1.00, +1.00] | inside (median within 0.2 µs) |
| 1 | whole-relation walk | 10,000 | +2.30 µs [-1.25, +9.20] | -0.10 µs [-7.90, +15.30] | inside (CI overlaps the A/A CI) |
| 1 | SHOW NAMESPACES (control) | 200 | +0.05 µs [+0.00, +0.15] | +0.05 µs [-0.10, +0.20] | inside (median within 0.2 µs) |
| 1 | SHOW NAMESPACES (control) | 1,000 | +0.05 µs [-0.10, +0.10] | +0.15 µs [-0.10, +0.20] | inside (median within 0.2 µs) |
| 1 | SHOW NAMESPACES (control) | 10,000 | +0.10 µs [+0.05, +0.20] | -0.05 µs [-0.20, +0.10] | inside (median within 0.2 µs) |
| 1 | SET ISOLATION LEVEL (control) | 200 | +0.00 µs [+0.00, +0.00] | +0.05 µs [-0.10, +0.10] | inside (median within 0.2 µs) |
| 1 | SET ISOLATION LEVEL (control) | 1,000 | -0.05 µs [-0.15, +0.00] | +0.10 µs [+0.00, +0.20] | inside (median within 0.2 µs) |
| 1 | SET ISOLATION LEVEL (control) | 10,000 | +0.00 µs [-0.15, +0.10] | +0.00 µs [-0.10, +0.05] | inside (median within 0.2 µs) |
| 1 | SHOW META | 200 | +0.95 µs [+0.80, +1.10] | +0.20 µs [-0.30, +0.40] | not an OLTP statement: B's reply carries 13 more fields |
| 1 | SHOW META | 1,000 | +0.85 µs [+0.60, +1.30] | +0.20 µs [-0.20, +0.60] | not an OLTP statement: B's reply carries 13 more fields |
| 1 | SHOW META | 10,000 | +0.90 µs [+0.70, +1.10] | -0.30 µs [-0.80, +0.60] | not an OLTP statement: B's reply carries 13 more fields |
| 2 | INSERT, pk omitted | 200 | -0.20 µs [-0.30, -0.10] | -0.30 µs [-0.70, -0.20] | inside (median within 0.2 µs) |
| 2 | INSERT, pk omitted | 1,000 | -0.20 µs [-0.30, +0.00] | +0.05 µs [-0.20, +0.40] | inside (median within 0.2 µs) |
| 2 | INSERT, pk omitted | 10,000 | -0.20 µs [-0.40, +0.05] | -0.20 µs [-0.80, +0.30] | inside (median within 0.2 µs) |
| 2 | INSERT again (replicate) | 200 | -0.20 µs [-0.35, -0.10] | -0.25 µs [-0.30, +0.10] | inside (CI overlaps the A/A CI) |
| 2 | INSERT again (replicate) | 1,000 | -0.15 µs [-0.40, +0.00] | +0.00 µs [-0.30, +0.30] | inside (median within 0.2 µs) |
| 2 | INSERT again (replicate) | 10,000 | -0.15 µs [-0.40, +0.00] | +0.10 µs [-0.80, +0.20] | inside (median within 0.2 µs) |
| 2 | SELECT by pk | 200 | -0.30 µs [-0.50, +0.20] | -0.50 µs [-1.00, -0.20] | inside (CI overlaps the A/A CI) |
| 2 | SELECT by pk | 1,000 | -0.10 µs [-0.60, +0.20] | +0.80 µs [-0.80, +1.20] | inside (median within 0.2 µs) |
| 2 | SELECT by pk | 10,000 | -0.25 µs [-1.00, +0.05] | +0.05 µs [-0.70, +0.80] | inside (CI overlaps the A/A CI) |
| 2 | UPDATE by pk | 200 | -0.40 µs [-0.50, +0.00] | -0.30 µs [-0.80, +0.20] | inside (CI overlaps the A/A CI) |
| 2 | UPDATE by pk | 1,000 | -0.25 µs [-0.80, +0.00] | +0.25 µs [-0.70, +0.60] | inside (CI overlaps the A/A CI) |
| 2 | UPDATE by pk | 10,000 | -0.45 µs [-0.90, +0.10] | -0.15 µs [-0.60, +0.20] | inside (CI overlaps the A/A CI) |
| 2 | DELETE by pk | 200 | +0.10 µs [-0.20, +0.20] | -0.25 µs [-1.30, +0.00] | inside (median within 0.2 µs) |
| 2 | DELETE by pk | 1,000 | -0.10 µs [-0.25, +0.10] | +0.30 µs [-0.20, +0.80] | inside (median within 0.2 µs) |
| 2 | DELETE by pk | 10,000 | +0.00 µs [-0.50, +0.20] | -0.10 µs [-0.50, +0.20] | inside (median within 0.2 µs) |
| 2 | bulk INSERT, 20 rows | 200 | -0.15 µs [-2.00, +0.75] | -0.40 µs [-2.50, +0.20] | inside (median within 0.2 µs) |
| 2 | bulk INSERT, 20 rows | 1,000 | -0.65 µs [-2.60, -0.20] | -1.30 µs [-3.50, +3.60] | inside (CI overlaps the A/A CI) |
| 2 | bulk INSERT, 20 rows | 10,000 | -0.60 µs [-2.30, +0.20] | +0.55 µs [-4.60, +2.10] | inside (CI overlaps the A/A CI) |
| 2 | whole-relation walk | 200 | +0.35 µs [-0.80, +1.10] | -0.75 µs [-1.20, +0.10] | inside (CI overlaps the A/A CI) |
| 2 | whole-relation walk | 1,000 | +0.40 µs [-0.70, +1.10] | -0.15 µs [-2.20, +1.90] | inside (CI overlaps the A/A CI) |
| 2 | whole-relation walk | 10,000 | +0.30 µs [-2.65, +5.20] | +1.75 µs [-4.90, +9.90] | inside (CI overlaps the A/A CI) |
| 2 | SHOW NAMESPACES (control) | 200 | +0.30 µs [+0.20, +0.40] | -0.10 µs [-0.20, +0.10] | OUTSIDE: B slower |
| 2 | SHOW NAMESPACES (control) | 1,000 | +0.00 µs [-0.10, +0.25] | +0.10 µs [-0.20, +0.20] | inside (median within 0.2 µs) |
| 2 | SHOW NAMESPACES (control) | 10,000 | +0.10 µs [+0.00, +0.25] | -0.15 µs [-0.30, +0.70] | inside (median within 0.2 µs) |
| 2 | SET ISOLATION LEVEL (control) | 200 | +0.00 µs [-0.10, +0.10] | +0.00 µs [-0.10, +0.10] | inside (median within 0.2 µs) |
| 2 | SET ISOLATION LEVEL (control) | 1,000 | +0.00 µs [-0.10, +0.05] | +0.10 µs [+0.00, +0.10] | inside (median within 0.2 µs) |
| 2 | SET ISOLATION LEVEL (control) | 10,000 | +0.00 µs [-0.10, +0.05] | -0.15 µs [-0.30, +0.00] | inside (median within 0.2 µs) |
| 2 | SHOW META | 200 | +0.95 µs [+0.80, +1.20] | -0.45 µs [-0.70, -0.20] | not an OLTP statement: B's reply carries 13 more fields |
| 2 | SHOW META | 1,000 | +0.80 µs [+0.60, +1.30] | -0.15 µs [-0.80, +0.30] | not an OLTP statement: B's reply carries 13 more fields |
| 2 | SHOW META | 10,000 | +0.80 µs [+0.70, +1.10] | -0.20 µs [-0.50, +0.20] | not an OLTP statement: B's reply carries 13 more fields |

**inside the noise: 53 cells (SHOW META excluded); outside: 1** (SHOW NAMESPACES (control), cores 2, 200 rows, +0.30 µs [+0.20, +0.40])

## 9. What is invalid, and what was not measured

- **The idle-core run, the 12-cell stall matrix, the same-core probe and core 0's loss window** are BI-S5's other
  measurements (`results-bi-s5-checkpoint-stall-v2.7.0-784-gec827a19.md`); this file does not repeat them.
- **`strict` was not run.** The order names `relaxed` and `group`.
- **`cores = 4` was not run**; `cores` 1 and 2 as BA's close. At `cores = 2` the single connection lands on core 0 or
  core 1 per run (the tables above list the cores seen); its counters are that core's.
- **The root cause of the 400-480 ms `relaxed` stall at 10,000 rows is not isolated.** The correlation with
  `wal_ring_full` is all the run has (section 6).
- **The group-mode p95 and p99** are device tail and are shown as they are; no claim rests on them.
- **The walk at 50,000 and 100,000 rows** (past BA-S15's 64-page slice) was not run; BA's close left that open too.
- **Raw per-statement files** (`*.raw.json.gz`, one per run) are not archived, to keep the archive small; the per-run
  summaries, the driver logs and the 1 s host samples (`host-samples*.jsonl`) are. Every table in this file regenerates
  from the summaries with `drivers/bi_analyze.py`, `verdict.py`, `baseline.py` and `hostwin.py`; the "longest statement"
  columns are the summaries' `max_us` and need nothing else.
- **Which core the single connection landed on** is recorded (`snaps.*.start.core`) and tabulated; it was not controlled.
- **One pass per cell.** There is no quiet-window repeat of any cell; the A/A cells and the replicate arm are the
  repeat evidence.

## 10. What this run teaches about the engine

- **BI costs nothing per statement and the order's expectation ("flat") holds.** A task submit per run, a test per tick and up to two parks per
  step do not show at p50 in 53 of 54 `relaxed` cells, in `group`, or (with one walk cell, section 7) at a 1,000 ms cadence. The only stable per-statement difference is `SHOW META`, +0.80 to
  +0.95 µs, the 13 run-record fields of its reply.
- **A 5000 ms cadence meets a polite foreground rarely.** A cadence run is 1.3-1.8 % of its tick as a reactor hold in A at 10,000 rows (66.9 and 89.2 ms), and
  at 200-1,000 rows a `relaxed` run of this driver ends before the first tick. The benefit of BI shows on the statement path where the run's hold is larger than
  the statement's own tail: 10,000 rows, `group`'s 15-25 ms commit tail against A's 36-70 ms hold, or any size at a 1,000 ms cadence, where A's hold is 39.5-227.3 ms
  (median) and B's 1.3-2.4 ms.
- **The checkpoint was not the largest stall in this workload.** At `relaxed` and 10,000 rows a 400-480 ms reactor iteration outside every checkpoint run occurs in most
  runs of A, B and two copies of A, ahead of the 67-89 ms run of A, and its count follows `wal_ring_full`. Taking the checkpoint off the reactor cannot remove the
  longest statement of that shape, and the shape's next bound is then the log appender's inline drain. This is `[inferred]` from a correlation; the run does not
  isolate it.
- **BI adds no walk cost.** A and B differ by +0.00 to +2.30 µs at 10,000 rows (CIs containing zero), and both read 13-17 µs below the BA close's B' (derived), so
  the per-row cost BA's close recorded between its own A and B (+20 to +46 µs at 10,000 rows) is not carried by this pair.
- **The `group` commit is 95 % device** (section 3), and B's `group` readings are the same as A's within the device's own spread: the synced cost of BI's
  parks and data-sync thread, which the work order priced as "nothing per statement", is not resolvable at one connection.

## 11. Verdict on BI-Q10 (iii)

**BI-Q10 (iii), "the OLTP overhead A/B within its own run-to-run noise": met.**

- **Per-statement p50, `relaxed`, `cores` 1 and 2, 200 / 1,000 / 10,000 rows:** 53 of 54 cells inside the noise (a median of 0.2 µs or less, or a CI
  overlapping the A/A cell of the same shape; section 8's table). The one outside, `SHOW NAMESPACES` at `cores = 2`, 200 rows, +0.30 µs [+0.20, +0.40], is a control
  that nothing in BI touches. The largest OLTP-shape reading is -0.65 µs (20-row `INSERT`, `cores = 2`, 1,000 rows) with B the faster one; the walk's is +2.30 µs
  [-1.25, +9.20] at `cores = 1`, 10,000 rows. `SHOW META` (+0.80 to +0.95 µs) is excluded as not an OLTP statement: B's reply carries 13 more fields.
- **Synced, `group`, `cores` 1 and 2:** -10.75 to +3.45 µs on 997-1,042 µs commits (-1.0 % to +0.35 %). No cell is resolved with B slower; two are resolved
  with B faster (10,000 rows, `cores = 2`: -9.80 and -10.75 µs) against a per-run sd of 11.5-18.1 µs, and their neighbours straddle zero with both signs.
- **A stricter reading, with no A/A allowance:** nine non-`SHOW META` cells have a median beyond +-0.3 µs (the largest a control reads). Six are B faster (-0.40 to -0.65 µs:
  the pk `SELECT` and `UPDATE`, the 20-row `INSERT`), three are the walk with B slower (+0.35 and +0.40 µs at 200 and 1,000 rows `cores = 2`, +2.30 µs at 10,000 rows `cores = 1`,
  0.3 % of its p50) and every one of those three CIs contains zero.
- **The longest statement, which the order said might move:** B's is below A's where A's hold exceeds the statement's own tail: `group` at 10,000 rows (-18.1 and -39.4 ms,
  CIs exclude zero), the arm that holds the run at `relaxed` and 10,000 rows (A 45.8 and 9.5 ms against B 2.1 and 1.8 ms, median) and the dense cadence at 200 and 1,000 rows
  (-23.6 to -186.4 ms; 5 of 40 B server-runs over 40 ms against 23 of 40 for A and 17 of 20 for two copies of A). It is equal within noise where no run lands (`relaxed` at 200 and
  1,000 rows, 3.1-6.2 ms) and at `group` 200 and 1,000 rows (-0.6 to +0.4 ms, CIs containing zero), where a synced commit's own 15-25 ms tail exceeds A's 10-16 ms hold. At `relaxed`
  and 10,000 rows the per-run column is a 400-480 ms stall that A, B and two copies of A all show, and it decides nothing about BI.
- **What bounds this verdict.** One pass per cell (16 / 12 / 10 / 8 runs), one host shared with other sessions (none of them a build or a test run in any cell, no
  foreign process above 20 % of a CPU), one connection (BI-Q10 (iii) is a per-statement reading; concurrency is in the BI-S5 stall file), `cores` 1 and 2 only, `strict`
  and `cores = 4` not run, A/A cells of 8 runs whose own CIs reach 1.2 µs (pk shapes) and 23 µs (walk), and the 400-480 ms stall unattributed. The synced deltas are
  read against a device whose commit cost differs between cells (997-1,042 µs).
