# Mount start-to-ready after the scrub remedy: 4-6 ms over the baseline, 79-85 % of the +25-35 ms regression removed

**Thesis.** `bench/v3.4.8/results-audit-g-close-v3.4.8-16-g713a84d0.md` found that the audit milestone's log open (`713a84d0`) made a server start
25-33 ms slower, fixed per mount, because the tail scrub read the last 64 MiB segment a second time and tested it byte by byte. The remedy
(`7c2dec3b`, `4cf04d4c` = `v3.4.8-20-g4cf04d4c`) tests the scan's own buffer in 4 KiB `memcmp` blocks and reads nothing twice. Measured as an
interleaved three-way A/B/C on one host, **a start now costs +3.9 to +5.8 ms over the baseline `2789a457`** (+6 % to +9 %), against +25.0 to +34.6 ms for the previous
closing build measured alongside it: **79-85 % of the regression is gone, and 4-6 ms remain**: a mean shift smaller than the baseline's own p0-to-max spread (9-11 ms), with the ranges of A and C overlapping in every cell. Only the log open changed between `713a84d0` and `4cf04d4c`; nothing else was measured.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-11-14 (UTC): build 02:51-02:52, cells 02:52:22-02:53:00 (the host sampler runs to 02:54). About 3 minutes of measurement, 5 with the build |
| Worktree / branch | `/home/louis/ckdbs/.claude/worktrees/audit-g1-g15-fixes`, `worktree-audit-g1-g15-fixes`, HEAD `4cf04d4c` when this was written; **C** was built from a detached `git worktree` of `4cf04d4c`, removed afterwards |
| **A (baseline)** | `2789a457` = `v3.4.8-13-g2789a457`: the same copy `kds_server-A` as the first run, sha256 `bb33f463ef4a0eb50a53b3e5422240f1c8dfa1d563ec12ac70f6a3899bdd4488` (re-hashed before the first cell) |
| **B (control)** | `713a84d0` = `v3.4.8-16-g713a84d0`: the same copy `kds_server-B` as the first run, sha256 `072347a000f1dc851f835f2ac61f5f790c24b90ab5305054143e46c2996f4a5c`. Re-measured in this run's interleave so the comparison C against B is same-day |
| **C (remedy)** | `4cf04d4c` = `v3.4.8-20-g4cf04d4c`, committed 2026-11-14 02:48:02 UTC (Release, `CMAKE_BUILD_TYPE=Release`, `--target kds_server`), binary mtime 02:52:13 UTC (after the commit); copy `kds_server-C`, 7,603,928 bytes, sha256 `32f23732ce55f7ca8adfb9a06b812f8cf43b6053002edccbaf2044d66a771741`. Tree clean |
| Binary discipline | every server started from the three copies in the scratchpad (`/tmp`, tmpfs, binary only), through the same one-line `exec` wrapper per build as the first run (it appends the `buffer_pool_frames = 65536` key the unmodified driver does not write) |
| Device | data files and WALs under `/home/louis/bench-runs/audit-g-close/f2/` on `/dev/nvme0n1p1`, ext4; a fresh server and volume per run |
| Host | 8 vCPU Xeon Platinum 8581C, Linux 6.12.116; the sampler (51 samples, every 2 s) counted `procs=0` in every one (no compiler, test or `cmake` process; the one-minute load fell 5.8 to 1.4 from my own finished build's decaying average, and the cells started after the compilers had exited) |
| Server config | default `cores`, `durability = relaxed`, `buffer_pool_frames = 65536`, 64 MiB segment |
| Driver | unmodified `tools/mount_cost_benchmark.py --mounts 7 --durability relaxed`, 3 runs per build and cell, rotating order (A B C / C A B / B C A), 7 mounts per run; the crash cell adds `--crash` |
| Prior number | **the 713a84d0 run**, `bench/v3.4.8/results-audit-g-close-v3.4.8-16-g713a84d0.md` §3, and B re-measured here. PostgreSQL floor: not measured |
| Tests | no engine code changed by this measurement; none run by it (the coordinator reports the full suite green at `4cf04d4c`, 4439 / 4439) |
| Raw outputs | `archive/audit-g-close-v3.4.8-20-g4cf04d4c/` |

## 2. Result

Wall time from `Popen` to the `listening on` line, in ms (a one-shot duration; QPS does not apply). n = 21 per cell, except the first mount after a SIGKILL (n = 3, so p25 equals p0 and p99 equals the maximum).

| Cell | Build | Mounts | p0 | p25 | p50 | p95 | p99 | mean | `recovery_analysis_us` mean | `recovery_checkpoint_us` mean |
|---|---|---|---|---|---|---|---|---|---|---|
| empty log | A | 21 | 57.8 | 60.3 | 60.5 | 68.9 | 69.0 | 62.0 | 23.5 | 3.6 |
| empty log | B | 21 | 92.3 | 94.5 | 96.6 | 101.0 | 109.0 | 96.6 | 26.1 | 3.3 |
| empty log | **C** | 21 | 62.4 | 64.9 | 66.9 | 73.3 | 75.7 | **67.9** | 23.2 | 3.6 |
| 4,000 rows, clean stop | A | 21 | 55.9 | 57.9 | 58.2 | 64.7 | 67.0 | 59.6 | 17.6 | 3.5 |
| 4,000 rows, clean stop | B | 21 | 79.8 | 83.5 | 83.8 | 88.3 | 90.6 | 84.6 | 19.6 | 3.5 |
| 4,000 rows, clean stop | **C** | 21 | 60.3 | 62.3 | 62.9 | 68.7 | 68.8 | **64.4** | 17.5 | 3.4 |
| 4,000 rows, SIGKILL, first mount (about 29k records) | A | 3 | 111.4 | 111.4 | 114.1 | 120.6 | 120.6 | 115.4 | 28.4 | 14.3 |
| 4,000 rows, SIGKILL, first mount | B | 3 | 138.9 | 138.9 | 140.8 | 143.7 | 143.7 | 141.1 | 31.2 | 13.7 |
| 4,000 rows, SIGKILL, first mount | **C** | 3 | 119.7 | 119.7 | 120.4 | 122.1 | 122.1 | **120.7** | 28.9 | 13.5 |
| 4,000 rows, SIGKILL, mounts 2-7 | A | 18 | 53.7 | 57.9 | 58.2 | 62.4 | 62.4 | 58.5 | 17.0 | 3.5 |
| 4,000 rows, SIGKILL, mounts 2-7 | B | 18 | 81.5 | 83.7 | 83.8 | 86.5 | 86.5 | 84.4 | 19.5 | 3.4 |
| 4,000 rows, SIGKILL, mounts 2-7 | **C** | 18 | 57.8 | 60.4 | 62.5 | 67.1 | 67.1 | **62.5** | 16.7 | 3.4 |

| Cell | B - A mean | **C - A mean** | C - A p50 | C - B mean | Share of the B - A regression removed | B (713a84d0 run, first measurement) - A, for reference |
|---|---|---|---|---|---|---|
| empty log | +34.6 ms | **+5.8 ms** (+9 %) | +6.3 ms | -28.8 ms | 83 % | +32.8 ms |
| 4,000 rows, clean stop | +25.0 ms | **+4.8 ms** (+8 %) | +4.7 ms | -20.2 ms | 81 % | +25.6 ms |
| 4,000 rows, SIGKILL, first mount | +25.8 ms | **+5.4 ms** (+5 %) | +6.3 ms | -20.4 ms | 79 % | +27.6 ms |
| 4,000 rows, SIGKILL, mounts 2-7 | +25.9 ms | **+3.9 ms** (+7 %) | +4.3 ms | -22.0 ms | 85 % | +23.4 ms |

**Reading.** B reproduced the first run to within 2-4 ms in every cell (+25.0 to +34.6 ms against +23.4 to +32.8 ms), so the control did not drift and the 20-29 ms that C removes is a build effect.
What is left, 3.9-5.8 ms, is a mean shift that all four cells show in the same direction; the distributions of A and C overlap in all four cells (A's maximum exceeds C's minimum in each), and the baseline's own p0-to-max spread is 9-11 ms,
so no single mount can be called slower; what the run supports is a mean shift of 4-6 ms, the same sign in four cells and in 21-mount samples, small against the spread and not demonstrated beyond it. The server's recovery counters are equal (`recovery_analysis_us` -0.3 to +0.5 ms), as before.
The wait breakdown does not apply (a one-shot start with no concurrent waits the `sys.wait_*` views could attribute).

**Delta against the 713a84d0 numbers:** C is 20.2-28.8 ms faster than B on the same day (B's first-run means 94.5 / 85.9 / 135.1 / 78.4 ms, C's now 67.9 / 64.4 / 120.7 / 62.5).

## 3. What is left, and the best attribution

The diff between B and C removes the second 64 MiB read (the 1 MiB `ReadAt` loop) and the byte-wise `find_if`. It leaves three things on the open path that A did not have:
a 4 KiB `memcmp` pass over the scan's roughly 64 MiB buffer, `device->Sync()` before the watermark claims the scanned tail, and the directory `fsync` of the adopted segment names. **None of the three was timed separately**
(no `strace` or `perf` on this host; a driver cannot split them, and no engine code may be edited by a measurement). My best attribution of the 4-6 ms is the `memcmp` pass plus the two syncs: a pass over 64 MiB that was just written by a `pread` is bounded by memory bandwidth,
about 5-6 ms at 10-12 GB/s, which alone is the size of the residual. That is arithmetic, not a finding. It also fits the shape of the numbers: the residual is largest for the empty log (+5.8 ms), where the whole remainder is zero and so
every block is compared, and smaller where records occupy part of the buffer (+3.9 to +5.4 ms), but the differences between cells (1-2 ms) are inside the spread, so no weight is put on it.
The first-mount-after-crash cell (n = 3) adds nothing a larger sample would not: its +5.4 ms agrees with the clean cells'.

If the residual is worth removing, the candidate is not the scrub's test but whether a segment whose scan already saw it zero-filled to the end could skip the pass (the scan knows where the last record ended and a freshly created segment is zero by construction); that is a design question for the owner of `wal.md` §4.2, not a measured result, and it is not recommended on this evidence alone.

## 4. Not measured

| Not measured | Why |
|---|---|
| The split of the 4-6 ms between the `memcmp` pass, the log sync and the directory sync | no profiler or syscall tracer on the host; engine code may not be edited by a measurement |
| Mount cost against log fill (a nearly full last segment) | as in the first run, every cell's last segment is mostly empty |
| Any shape other than a mount | only the log open changed since `713a84d0`; the ordinary-path cells of the first file stand for `713a84d0` and were not re-run |

## 5. Reproduce

Archive `scripts/`: `mount2.sh` (the interleave, rotating order, status file for the watchdog), `wrap-{A,B,C}.sh`, `gen_mount2.py` (the tables above). Per cell: `mount_cost_benchmark.py --binary <wrapper> --label X --rows N --mounts 7 --durability relaxed [--crash] --port 15481 --scratch <ext4 dir> --json X.json --force`, N in {0, 4000}. `--force` overrides the driver's load-average guard, which read a decaying average; `host3.log` shows no competing process. `--rows` is the number of 1,600-byte spilled rows loaded before the mounts (about 6.5 MB of log).
