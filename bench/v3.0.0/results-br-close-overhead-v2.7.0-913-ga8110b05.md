# BR's close: the milestone's OLTP overhead A/B (`v2.7.0-913-ga8110b05`)

**Thesis.** With backup off and archive off (the defaults), BR's whole code change costs **nothing resolvable per statement**
on one serial connection. Over 30 `relaxed` cells (five statement shapes, three sizes, two core counts) the p50 of B (BR closed)
minus A (the commit BR opened from) reads `-0.20` to `+0.25` µs on the four pk shapes at 41-60 µs a statement (at most 0.5 %),
the p99 and the throughput of every cell sit inside the run-to-run noise, and the one cell whose p50 and server-CPU intervals
miss the A/A control's (UPDATE by pk, `cores = 1`, 10,000 rows: `+0.20` µs [`+0.10`, `+0.50`]) moved less than the A/A control
itself did on the same shape (`-0.30` µs [`-0.60`, `-0.20`]). The data-file write path, the only place BR-R6 says a cost sits
(the fence's three atomics in `FilePageDevice::WriteAt`), prices at `1.990` µs a page in A and `1.994` µs in B over eight
alternating pairs, the page-write phase of an 8,546-page checkpoint run. **BR-R6 holds as measured**, with one correction to its
wording from the source read below: the statement path did gain code (a seven-name compare chain and an
`ArchiveRefusal()` test per statement), which the claim "three atomics per page write" does not list; whatever it costs is
under the noise floor of this run.

- **A** = `fb953f07` (`v2.7.0-908-gfb953f07`), the commit BR opened from. Binary `09c82ae3…`.
- **B** = `a8110b05` (`v2.7.0-913-ga8110b05`), BR closed in code (S0 to S9, five commits). Binary `af25c71f…`.
  `git diff --stat fb953f07 a8110b05 -- src include CMakeLists.txt`: 39 files, 2,950 insertions, 49 deletions.
- This is the milestone-close overhead A/B of `CLAUDE.md` step 3 and `instructions/v3.0.0/workorder-br-backup-restore.md`
  BR-S10 (last bullet) and section 6 item 10. What backup and archive cost **when on**, the archive's throughput and the
  restore time are in `results-br-close-backup-restore-v2.7.0-913-ga8110b05.md`.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-12, 15:55 to 16:17 UTC. `relaxed` `cores = 1` 15:55-15:59, `relaxed` `cores = 4` 15:59-16:03, A/A `cores = 1` 16:03-16:04, A/A `cores = 4` 16:04-16:06, `group` `cores = 1` 16:06-16:11, `group` `cores = 4` 16:11-16:15, page-write cells 16:15-16:17. Per-cell windows and loads: section 2 |
| Branch / worktree | `br-backup-restore`, worktree `br-backup-restore`; HEAD `a8110b05` |
| Tree | `git status --short` at 15:45 UTC, before anything was built: one modified tracked file, `instructions/v3.0.0/workorder-br-backup-restore.md`. Nothing under `src/`, `include/` or `tools/` differed from `a8110b05`, and both binaries were built from `git archive` exports of the two commits, so the worktree's later state does not reach them (at 17:35 UTC `tests/backup_archive_test.cpp` also shows modified, by the session that launched this run; this run did not touch it). No engine file was edited by this run |
| A, provenance | Release build of `git archive fb953f07` in a scratch directory: `-DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF -DKDS_BUILD_BENCH=OFF`, target `kds_server`. Binary mtime 2026-10-12 15:48:18 UTC, after the commit (13:04:43 UTC) |
| B, provenance | The same flags, `git archive a8110b05`; `diff -rq` of its `src/`, `include/` and `CMakeLists.txt` against the worktree is empty. Binary mtime 15:49:46 UTC, after the commit (15:39:32 UTC). **The worktree's own `build/` is Debug and was not used** |
| Measured copies | A `09c82ae38fd1ae4124b39263be51db23cb3322c97b75434aa41aa4cc5ee32892`, B `af25c71f63ca09ea60f6a66db89ca2d5664d20c5865cf18108b655fca6696757` (`sha256sum`, equal to the source binaries'; re-hashed after the last cell: unchanged). Every server of every cell started from `bin/kds_server-A` or `-B` in the run directory (`archive/br-close-v2.7.0-913-ga8110b05/binaries.txt`). The exports carry no `.git`, so the binaries' own version string reads `unknown`; the commits are named here |
| Device | data files and WAL under `/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/work/`, `/dev/root` = `/dev/nvme0n1p1`, ext4, `rw,relatime,discard,errors=remount-ro,commit=30` (`findmnt`); not tmpfs. The device writes 169 MB/s and 173 MB/s on a 1 GiB `dd conv=fsync` and `dd oflag=direct`, measured at 16:26 UTC. A fresh data file and server per (arm set, run), deleted after |
| Host | 8 logical CPUs, AMD EPYC 9V74, 4 physical cores x 2 threads (siblings 0/1, 2/3, 4/5, 6/7), 15 GiB, Linux 7.0.0-1014-azure. Shared with other sessions (section 2) |
| Pinning | server `taskset -c 4` with `reactor_cpus = 4` (`cores = 1`), or `taskset -c 0,2,4,6` with `reactor_cpus = 4,2,6,0` (`cores = 4`); driver `taskset -c 7`. At `cores = 4` the client session is made to land on core 0 in even runs and core 1 in odd runs (the driver reconnects until `SHOW META` says so), the same for A and B |
| Server config | `cores` 1 or 4; `durability` `relaxed` (`group` in the group cells); `buffer_pool_frames = 65536` (512 MiB); `checkpoint_interval_ms = 5000`; `log_level = warn`; everything else default, so **`archive_dir` unset and no `BACKUP` issued**. Ports 31811 (A) and 31812 (B), chosen, never the default 15432 |
| Pool against the working set | `pool_refused` 0 and 0 statement errors in all 276 runs; the volume is held whole in the pool, so no statement here waits on an eviction |
| Suite | not executed. No code changed in this measurement; the suite belongs to the stages |
| PostgreSQL floor | not measured for this shape. The A/B is B against A; a floor answers nothing about a change inside one engine |
| Drivers | `archive/br-close-v2.7.0-913-ga8110b05/drivers/br_overhead_ab.py` (the BI close's `bi_overhead_ab.py` with the arm list trimmed to the five shapes, the session pinned to a chosen core at `cores = 4`, and `SHOW META` read around every timed block for `sched_foreground_polled_us`, none of it inside a timed region), `br_ckpt.py` for section 5, `run_m0.sh` for the cells. `bench/docs/` is closed (`bench/README.md`) and `tools/` has no driver for these shapes, so the commands are the scripts in the archive |
| Series baseline | `results-bi-close-overhead-v2.7.0-784-gec827a19.md` measured the relaxed `INSERT` at 49.1 µs p50 (`cores = 1`, 200 rows). This run's A reads 41.5 µs. The two runs are 129 commits apart (`v2.7.0-784` to `v2.7.0-913`), were built with different flags (tests and simulation on there) and ran on a host in a different state, so no delta is claimed; this file is the baseline the next BR-era close is read against |

## 2. Method, and the host each cell ran on

One client connection, serial. Per (rows, run) a fresh A server and a fresh B server, four `(id, v, w) BTREE` relations loaded
with `--rows` named ascending keys in 500-row `INSERT`s, then arms of 3,000 statements (1,500 in the `group` cells) in 12
blocks that alternate which server goes first; **the first server alternates per run**, so the order is A,B,B,A,A,B,... within an
arm and B,A,A,B across runs. **`--rows 200,1000,10000` is the row count of the relation when an arm starts**, and the three sizes
are three rows of every table. Equal work: every run executed the same statement count on both servers (checked over all 276
runs: the row counts and the operation counts of A and B are equal in every one). Arms: `SET ISOLATION LEVEL` (a control: no
BR path is on it), the pk-omitted `INSERT` and its replicate on a second relation (the floor from inside the run), `SELECT` by
pk, `UPDATE` by pk, and `SHOW META` (not a control, section 3). `relaxed` runs 24 per (cores, rows), 72 per cell; the A/A
controls run the same binary A on both ports, 12 per (cores, rows); `group` runs 10 per (cores, rows) and carries the two
`INSERT` arms and the control only. Each delta is the median of the per-run differences B - A with a bootstrap 95 % interval
(4,000 resamples of the runs, seed 1). Server CPU per statement is the delta of `sched_foreground_polled_us` across each timed
block divided by the statements in it (the session's core; a block in which the session changed core would be dropped, and none
was).

**The checkpoint cadence is on and does not land in the arms.** `checkpoint_runs` read 0 at the end of all 192 `relaxed`
server-runs and exactly 1 at the end of all 80 `group` server-runs (the first tick is 5 s after boot; a `relaxed` run ends
before it). So the `relaxed` cells never write a data page in the measured region: the statement path is priced here, and the
page-write path, where the fence is, is priced separately in section 5.

**The host was shared, and this is the record.** The driver writes `/proc/loadavg` and `pgrep -a -f "cc1plus|cmake --build|ctest"`
into every run's JSON (`host_before`, `host_after`). Another session ran a `ctest -j8` and a build on this host until about
15:50 UTC, while A and B were built and before any cell; no cell overlapped it. "Competing" below counts runs whose pgrep lists
hold anything outside this run's directory.

| cell | runs | UTC window of the run files | 1-min load before a run | 1-min load after a run | runs with a live compiler/build/test program | runs with any competing process | longest hold-back | errors |
|---|---|---|---|---|---|---|---|---|
| `relaxed-c1` | 72 runs | 15:55:27-15:58:58 UTC | 1.43-2.28 | 1.43-2.28 | 0 of 72 runs | 0 of 72 runs | 0 s | 0 errors |
| `relaxed-c4` | 72 runs | 15:59:01-16:02:43 UTC | 1.53-2.12 | 1.53-2.12 | 0 of 72 runs | 0 of 72 runs | 0 s | 0 errors |
| `aa-c1` | 36 runs | 16:02:46-16:04:29 UTC | 1.55-2.07 | 1.55-2.07 | 0 of 36 runs | 0 of 36 runs | 0 s | 0 errors |
| `aa-c4` | 36 runs | 16:04:32-16:06:19 UTC | 1.63-1.99 | 1.63-1.99 | 0 of 36 runs | 0 of 36 runs | 0 s | 0 errors |
| `group-c1` | 30 runs | 16:06:28-16:10:48 UTC | 1.52-2.54 | 1.52-2.54 | 0 of 30 runs | 0 of 30 runs | 0 s | 0 errors |
| `group-c4` | 30 runs | 16:10:57-16:15:14 UTC | 1.54-2.36 | 1.54-2.36 | 0 of 30 runs | 0 of 30 runs | 0 s | 0 errors |

Sampler (1 s, per-CPU busy from /proc/stat; foreign process = not under /home/cdkbs/bench-runs, >= 20 % of a CPU):

| cell | UTC window | samples | 1-min load, min-max | mean busy, server CPUs | mean busy, driver CPU 7 | mean busy, the other CPUs | samples with a foreign process >= 20 % | those processes |
|---|---|---|---|---|---|---|---|---|
| `relaxed-c1` | 15:55:25-15:58:58 | 210 | 1.43-2.28 | 39 % | 38 % | 4 % | 1 | [kds_server-A] <defunct> |
| `relaxed-c4` | 15:58:58-16:02:44 | 224 | 1.53-2.12 | 13 % | 39 % | 3 % | 4 | [kds_server-A]; [kds_server-B] <defunct> |
| `aa-c1` | 16:02:44-16:04:29 | 105 | 1.55-2.07 | 38 % | 38 % | 3 % | 3 | [kds_server-A]; python3 drivers/analyze_m0.py out/m0 relaxed-c1 relaxed-c4 |
| `aa-c4` | 16:04:29-16:06:19 | 110 | 1.63-1.99 | 11 % | 38 % | 1 % | 1 | [kds_server-A] <defunct> |
| `group-c1` | 16:06:19-16:10:48 | 267 | 1.52-2.59 | 9 % | 4 % | 3 % | 2 | python3 drivers/analyze_m0.py out/m0 relaxed-c1 relaxed-c4 a |
| `group-c4` | 16:10:48-16:15:14 | 263 | 1.52-2.36 | 6 % | 6 % | 4 % | 0 | - |

The 1-minute load includes this run's own server, idle twin and driver (about two CPUs). In the sampler column, the entries
under "those processes" are this run's own: `[kds_server-A] <defunct>` is a zombie of a pilot pass that was killed and discarded
at 15:55 (the pilot's results were deleted, not used), and `python3 drivers/analyze_m0.py` is the analysis script, run on CPU 1
for about two seconds at 16:03 and 16:07 (3 samples inside `aa-c1`, 2 inside `group-c1`). No foreign process held 20 % of a CPU
in any sampled second of any cell. README's five rules: Release at the measured commit (section 1); data on a block device;
hashed copies of both binaries; the load and the competing list written per run; the ports chosen.

## 3. Results: the statement path

### The noise floor, from inside the run

Four floors sit in every `relaxed` cell: the **control** (`SET ISOLATION LEVEL`), the **replicate** (`INSERT again`), the **sd of
the per-run p50 delta**, and the **A/A cell** (the same binary on both ports, driver and interleaving unchanged: every delta there
is the floor of that shape at that size).

| floor | `cores = 1` | `cores = 4` |
|---|---|---|
| control p50 delta B - A, 3 sizes | +0.00 to +0.10 µs, intervals inside [-0.10, +0.10] µs | +0.00 to +0.10 µs, intervals inside [-0.10, +0.20] µs |
| replicate INSERT p50 delta, 3 sizes | +0.00 to +0.10 µs, intervals inside [-0.30, +0.20] µs | -0.20 to +0.05 µs, intervals inside [-0.40, +0.25] µs |
| A/A p50 delta, the four pk shapes x 3 sizes | -0.30 to +0.10 µs, intervals inside [-0.65, +0.55] µs | -0.70 to +0.15 µs, intervals inside [-0.85, +0.50] µs |
| A/A server CPU per statement delta, the same cells | -0.41 to +0.13 µs, intervals inside [-0.69, +0.43] µs | -0.55 to +0.04 µs, intervals inside [-0.77, +0.48] µs |
| A/A throughput delta, the same cells | -427 to +393 /s, intervals inside [-805, +1,109] /s | -622 to +452 /s, intervals inside [-994, +881] /s |
| sd of the per-run p50 delta, `relaxed` INSERT, 200 / 1,000 / 10,000 rows | 0.49 / 0.43 / 0.37 µs | 0.41 / 0.39 / 0.55 µs |

The A/A cells are not centred on zero: at `cores = 4` the UPDATE shape at 10,000 rows reads `-0.70` µs [`-0.85`, `-0.50`] with
the same binary on both sides, a port/position effect of a few tenths of a microsecond on a 45 µs statement. **Any delta below
about 0.3 µs at `cores = 1` and 0.7 µs at `cores = 4` is not a finding.**

### What B - A reads

Every table row is a cell: B - A of the per-run median, with its 95 % interval, beside the A/A control of the same shape and
size where the cell has one. Throughput is `ops / elapsed` of the arm, measured, not derived.


#### relaxed, cores = 1

| shape | rows | runs | A QPS | B QPS | B - A QPS (95 % CI) | A p50 | B p50 | B - A p50 (95 % CI) | A/A p50 delta (95 % CI) | A p99 | B p99 | B - A p99 (95 % CI) | B - A server CPU per statement (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| SET ISOLATION LEVEL (control) | 200 | 24 runs | 26,910 /s | 26,903 /s | +22 /s [-105, +122] | 35.8 µs | 35.8 µs | +0.00 µs [-0.10, +0.10] | +0.00 µs [-0.10, +0.05] | 52.7 µs | 51.9 µs | -0.55 µs [-1.30, +0.40] | -0.00 µs [-0.03, +0.03] |
| SET ISOLATION LEVEL (control) | 1,000 | 24 runs | 27,042 /s | 27,027 /s | -38 /s [-91, +68] | 35.6 µs | 35.6 µs | +0.00 µs [+0.00, +0.00] | +0.10 µs [-0.10, +0.10] | 51.8 µs | 51.7 µs | -0.40 µs [-0.80, +0.40] | -0.00 µs [-0.06, +0.04] |
| SET ISOLATION LEVEL (control) | 10,000 | 24 runs | 27,099 /s | 27,058 /s | +1 /s [-122, +103] | 35.5 µs | 35.6 µs | +0.10 µs [+0.00, +0.10] | +0.05 µs [-0.05, +0.10] | 51.8 µs | 51.4 µs | -0.35 µs [-0.90, +0.50] | +0.02 µs [-0.02, +0.08] |
| INSERT, pk omitted | 200 | 24 runs | 22,448 /s | 22,533 /s | +68 /s [-208, +254] | 41.5 µs | 41.5 µs | +0.10 µs [-0.30, +0.30] | +0.05 µs [-0.25, +0.25] | 69.3 µs | 69.2 µs | +0.15 µs [-1.30, +1.70] | +0.12 µs [-0.22, +0.35] |
| INSERT, pk omitted | 1,000 | 24 runs | 22,600 /s | 22,552 /s | -75 /s [-180, +65] | 41.3 µs | 41.5 µs | +0.15 µs [-0.30, +0.30] | -0.10 µs [-0.35, +0.25] | 68.5 µs | 67.6 µs | +0.00 µs [-1.80, +0.80] | +0.21 µs [-0.07, +0.39] |
| INSERT, pk omitted | 10,000 | 24 runs | 22,518 /s | 22,603 /s | -26 /s [-95, +51] | 41.5 µs | 41.5 µs | +0.05 µs [-0.10, +0.20] | -0.10 µs [-0.35, +0.35] | 68.2 µs | 67.8 µs | +0.10 µs [-1.00, +0.90] | +0.10 µs [-0.03, +0.22] |
| INSERT again (replicate) | 200 | 24 runs | 22,325 /s | 22,262 /s | -73 /s [-355, +474] | 41.5 µs | 41.5 µs | +0.10 µs [-0.20, +0.20] | +0.05 µs [-0.30, +0.20] | 68.0 µs | 69.3 µs | +1.65 µs [-0.90, +2.50] | +0.19 µs [-0.06, +0.35] |
| INSERT again (replicate) | 1,000 | 24 runs | 22,331 /s | 22,388 /s | +119 /s [-142, +541] | 41.5 µs | 41.5 µs | +0.00 µs [-0.30, +0.20] | -0.05 µs [-0.35, +0.30] | 68.2 µs | 66.0 µs | -0.80 µs [-3.00, +0.70] | +0.02 µs [-0.13, +0.33] |
| INSERT again (replicate) | 10,000 | 24 runs | 22,212 /s | 22,280 /s | +232 /s [-243, +371] | 41.5 µs | 41.5 µs | +0.00 µs [-0.10, +0.20] | +0.00 µs [-0.30, +0.30] | 67.3 µs | 68.6 µs | +1.20 µs [-0.60, +1.80] | +0.11 µs [-0.01, +0.30] |
| SELECT by pk | 200 | 24 runs | 16,752 /s | 16,694 /s | -1 /s [-177, +90] | 56.2 µs | 56.4 µs | +0.20 µs [-0.10, +0.40] | -0.05 µs [-0.30, +0.10] | 84.9 µs | 85.5 µs | -0.95 µs [-4.30, +3.20] | +0.36 µs [+0.13, +0.55] |
| SELECT by pk | 1,000 | 24 runs | 16,212 /s | 16,197 /s | -38 /s [-149, +80] | 58.1 µs | 58.1 µs | +0.10 µs [-0.20, +0.25] | -0.15 µs [-0.50, +0.55] | 87.1 µs | 86.6 µs | -1.00 µs [-2.70, +0.90] | +0.14 µs [-0.16, +0.32] |
| SELECT by pk | 10,000 | 24 runs | 16,938 /s | 16,770 /s | -53 /s [-240, +100] | 56.3 µs | 56.6 µs | +0.05 µs [-0.10, +0.50] | -0.20 µs [-0.65, +0.20] | 81.8 µs | 82.4 µs | -0.20 µs [-2.75, +1.95] | +0.28 µs [-0.03, +0.62] |
| UPDATE by pk | 200 | 24 runs | 21,066 /s | 21,329 /s | +112 /s [-403, +500] | 44.0 µs | 44.0 µs | +0.10 µs [-0.20, +0.30] | +0.10 µs [-0.45, +0.35] | 71.1 µs | 71.5 µs | -1.45 µs [-3.60, +1.40] | +0.15 µs [-0.11, +0.33] |
| UPDATE by pk | 1,000 | 24 runs | 21,075 /s | 21,192 /s | +3 /s [-295, +417] | 44.1 µs | 44.0 µs | +0.00 µs [-0.30, +0.25] | -0.05 µs [-0.55, +0.20] | 70.6 µs | 69.7 µs | -1.65 µs [-2.60, +1.10] | +0.02 µs [-0.31, +0.40] |
| UPDATE by pk | 10,000 | 24 runs | 20,946 /s | 21,028 /s | +61 /s [-363, +376] | 44.2 µs | 44.5 µs | +0.20 µs [+0.10, +0.50] | -0.30 µs [-0.60, -0.20] | 70.2 µs | 70.1 µs | -0.10 µs [-1.40, +1.00] | +0.41 µs [+0.19, +0.55] |

#### relaxed, cores = 4

| shape | rows | runs | A QPS | B QPS | B - A QPS (95 % CI) | A p50 | B p50 | B - A p50 (95 % CI) | A/A p50 delta (95 % CI) | A p99 | B p99 | B - A p99 (95 % CI) | B - A server CPU per statement (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| SET ISOLATION LEVEL (control) | 200 | 24 runs | 26,402 /s | 26,398 /s | -5 /s [-95, +140] | 35.9 µs | 36.0 µs | +0.00 µs [+0.00, +0.10] | +0.00 µs [-0.15, +0.10] | 54.5 µs | 54.8 µs | -0.35 µs [-0.90, +0.40] | +0.05 µs [+0.00, +0.10] |
| SET ISOLATION LEVEL (control) | 1,000 | 24 runs | 26,600 /s | 26,544 /s | -7 /s [-80, +96] | 35.8 µs | 35.9 µs | +0.10 µs [+0.00, +0.20] | +0.00 µs [-0.10, +0.15] | 53.7 µs | 53.5 µs | -0.20 µs [-0.70, +0.50] | +0.04 µs [-0.02, +0.07] |
| SET ISOLATION LEVEL (control) | 10,000 | 24 runs | 26,620 /s | 26,756 /s | +35 /s [-59, +164] | 35.8 µs | 35.7 µs | +0.00 µs [-0.10, +0.05] | +0.00 µs [-0.05, +0.10] | 53.4 µs | 53.1 µs | -0.40 µs [-1.10, +0.20] | -0.00 µs [-0.06, +0.11] |
| INSERT, pk omitted | 200 | 24 runs | 21,874 /s | 21,892 /s | +1 /s [-118, +64] | 42.5 µs | 42.4 µs | -0.15 µs [-0.30, +0.10] | -0.10 µs [-0.25, +0.20] | 69.4 µs | 68.5 µs | -0.15 µs [-0.70, +1.20] | +0.08 µs [-0.14, +0.31] |
| INSERT, pk omitted | 1,000 | 24 runs | 21,958 /s | 22,074 /s | +110 /s [-27, +176] | 42.4 µs | 42.3 µs | -0.10 µs [-0.30, +0.00] | -0.25 µs [-0.55, +0.00] | 68.7 µs | 67.3 µs | -1.40 µs [-1.90, -0.10] | -0.09 µs [-0.16, +0.04] |
| INSERT, pk omitted | 10,000 | 24 runs | 21,806 /s | 21,917 /s | -33 /s [-144, +193] | 42.4 µs | 42.4 µs | +0.10 µs [-0.20, +0.25] | -0.20 µs [-0.35, +0.05] | 69.5 µs | 68.2 µs | -1.15 µs [-1.40, -0.35] | +0.11 µs [-0.07, +0.28] |
| INSERT again (replicate) | 200 | 24 runs | 21,207 /s | 21,439 /s | +20 /s [-160, +428] | 42.6 µs | 42.3 µs | -0.20 µs [-0.40, +0.20] | -0.20 µs [-0.25, +0.10] | 69.5 µs | 70.0 µs | +1.25 µs [-0.40, +2.30] | +0.04 µs [-0.16, +0.24] |
| INSERT again (replicate) | 1,000 | 24 runs | 21,537 /s | 21,771 /s | +162 /s [+17, +586] | 42.5 µs | 42.2 µs | -0.20 µs [-0.35, +0.00] | -0.30 µs [-0.45, +0.00] | 68.8 µs | 68.4 µs | -0.50 µs [-1.90, +1.20] | -0.11 µs [-0.29, -0.01] |
| INSERT again (replicate) | 10,000 | 24 runs | 21,664 /s | 21,791 /s | +48 /s [-121, +420] | 42.3 µs | 42.4 µs | +0.05 µs [-0.20, +0.25] | -0.20 µs [-0.40, +0.00] | 68.8 µs | 69.2 µs | -0.70 µs [-1.90, +0.70] | +0.05 µs [-0.13, +0.27] |
| SELECT by pk | 200 | 24 runs | 16,216 /s | 16,113 /s | -82 /s [-142, +20] | 57.5 µs | 57.8 µs | +0.25 µs [-0.30, +0.70] | +0.00 µs [-0.65, +0.45] | 88.9 µs | 88.7 µs | -0.10 µs [-1.60, +1.70] | +0.16 µs [+0.03, +0.73] |
| SELECT by pk | 1,000 | 24 runs | 15,735 /s | 15,688 /s | -55 /s [-146, +72] | 60.2 µs | 60.3 µs | +0.20 µs [-0.10, +0.60] | +0.15 µs [-0.35, +0.50] | 91.4 µs | 90.6 µs | -0.90 µs [-2.10, +0.40] | +0.30 µs [-0.17, +0.50] |
| SELECT by pk | 10,000 | 24 runs | 16,327 /s | 16,246 /s | -109 /s [-186, +52] | 57.5 µs | 57.6 µs | +0.20 µs [+0.00, +0.40] | -0.40 µs [-0.75, -0.10] | 84.6 µs | 85.6 µs | +0.15 µs [-1.00, +2.00] | +0.18 µs [-0.03, +0.47] |
| UPDATE by pk | 200 | 24 runs | 20,467 /s | 20,221 /s | -49 /s [-360, +134] | 44.9 µs | 45.1 µs | +0.15 µs [-0.20, +0.60] | -0.25 µs [-0.70, +0.20] | 70.0 µs | 69.8 µs | +0.65 µs [-0.55, +0.90] | +0.22 µs [+0.00, +0.59] |
| UPDATE by pk | 1,000 | 24 runs | 20,196 /s | 20,420 /s | +85 /s [-271, +368] | 45.0 µs | 45.2 µs | -0.15 µs [-0.30, +0.30] | -0.10 µs [-0.25, +0.20] | 68.7 µs | 67.9 µs | +0.00 µs [-0.80, +0.50] | +0.05 µs [-0.10, +0.39] |
| UPDATE by pk | 10,000 | 24 runs | 20,372 /s | 20,377 /s | -60 /s [-244, +176] | 45.2 µs | 45.4 µs | +0.00 µs [-0.30, +0.30] | -0.70 µs [-0.85, -0.50] | 68.8 µs | 69.4 µs | +0.75 µs [-1.00, +1.20] | +0.11 µs [-0.06, +0.35] |

#### group, cores = 1 (1,500 statements per arm)

| shape | rows | runs | A QPS | B QPS | B - A QPS (95 % CI) | A p50 | B p50 | B - A p50 (95 % CI) | A p99 | B p99 | B - A p99 (95 % CI) | B - A server CPU per statement (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| SET ISOLATION LEVEL (control) | 200 | 10 runs | 26,881 /s | 26,752 /s | -125 /s [-381, +12] | 35.8 µs | 36.0 µs | +0.10 µs [+0.00, +0.25] | 52.5 µs | 53.0 µs | +0.65 µs [-1.05, +1.90] | +0.10 µs [+0.03, +0.12] |
| SET ISOLATION LEVEL (control) | 1,000 | 10 runs | 27,048 /s | 27,026 /s | -19 /s [-82, +117] | 35.8 µs | 35.8 µs | +0.05 µs [-0.10, +0.10] | 51.5 µs | 50.8 µs | -0.50 µs [-1.30, +0.20] | +0.03 µs [-0.06, +0.08] |
| SET ISOLATION LEVEL (control) | 10,000 | 10 runs | 26,892 /s | 26,818 /s | -13 /s [-177, +161] | 35.9 µs | 36.0 µs | +0.00 µs [-0.05, +0.10] | 52.6 µs | 52.6 µs | +0.70 µs [-0.70, +1.80] | +0.03 µs [-0.01, +0.10] |
| INSERT, pk omitted | 200 | 10 runs | 802 /s | 794 /s | -1 /s [-28, +42] | 1042.9 µs | 1044.4 µs | -0.70 µs [-9.50, +8.40] | 5263.9 µs | 5255.4 µs | -128.20 µs [-674.40, +1133.90] | +0.12 µs [-0.22, +0.44] |
| INSERT, pk omitted | 1,000 | 10 runs | 801 /s | 778 /s | -22 /s [-42, +9] | 1064.6 µs | 1069.5 µs | -1.15 µs [-4.25, +5.80] | 5029.9 µs | 6048.1 µs | +745.45 µs [-172.80, +2252.70] | +0.14 µs [+0.06, +0.51] |
| INSERT, pk omitted | 10,000 | 10 runs | 792 /s | 792 /s | +5 /s [-29, +48] | 1065.5 µs | 1071.7 µs | -1.50 µs [-8.70, +8.10] | 5328.6 µs | 5166.1 µs | -916.80 µs [-1620.00, +1069.80] | +0.30 µs [-0.09, +0.48] |
| INSERT again (replicate) | 200 | 10 runs | 809 /s | 801 /s | +19 /s [-26, +51] | 1039.0 µs | 1041.5 µs | -6.30 µs [-7.80, +4.20] | 5403.4 µs | 5017.0 µs | -311.20 µs [-1444.40, +734.30] | +0.31 µs [+0.05, +0.51] |
| INSERT again (replicate) | 1,000 | 10 runs | 776 /s | 791 /s | +14 /s [-52, +33] | 1070.5 µs | 1064.3 µs | +0.00 µs [-7.30, +8.20] | 4818.6 µs | 5335.7 µs | +252.55 µs [-562.45, +1561.70] | +0.18 µs [-0.02, +0.45] |
| INSERT again (replicate) | 10,000 | 10 runs | 788 /s | 777 /s | +2 /s [-26, +31] | 1067.2 µs | 1065.6 µs | +0.00 µs [-8.20, +7.80] | 5696.8 µs | 5331.7 µs | -172.40 µs [-1392.10, +613.10] | +0.27 µs [+0.04, +0.56] |

#### group, cores = 4 (1,500 statements per arm)

| shape | rows | runs | A QPS | B QPS | B - A QPS (95 % CI) | A p50 | B p50 | B - A p50 (95 % CI) | A p99 | B p99 | B - A p99 (95 % CI) | B - A server CPU per statement (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| SET ISOLATION LEVEL (control) | 200 | 10 runs | 26,140 /s | 25,950 /s | +11 /s [-347, +259] | 36.1 µs | 36.2 µs | +0.00 µs [-0.10, +0.10] | 55.7 µs | 56.8 µs | -0.20 µs [-1.80, +3.35] | +0.07 µs [-0.08, +0.12] |
| SET ISOLATION LEVEL (control) | 1,000 | 10 runs | 26,051 /s | 26,317 /s | +164 /s [-8, +560] | 36.2 µs | 36.1 µs | -0.10 µs [-0.20, +0.00] | 56.2 µs | 54.9 µs | -0.15 µs [-1.40, +0.90] | +0.02 µs [-0.07, +0.06] |
| SET ISOLATION LEVEL (control) | 10,000 | 10 runs | 26,369 /s | 26,366 /s | -15 /s [-166, +194] | 36.0 µs | 36.1 µs | +0.00 µs [+0.00, +0.15] | 54.9 µs | 55.2 µs | +0.30 µs [-0.10, +1.40] | -0.02 µs [-0.04, +0.02] |
| INSERT, pk omitted | 200 | 10 runs | 768 /s | 771 /s | -16 /s [-29, +16] | 1092.0 µs | 1091.0 µs | +3.55 µs [-4.50, +8.70] | 5422.8 µs | 5171.3 µs | -226.55 µs [-872.40, +572.85] | -0.16 µs [-0.30, +0.63] |
| INSERT, pk omitted | 1,000 | 10 runs | 816 /s | 825 /s | +3 /s [-42, +27] | 1030.2 µs | 1032.1 µs | +3.15 µs [-4.45, +7.10] | 4974.3 µs | 5079.2 µs | +326.90 µs [-752.30, +1005.35] | +0.22 µs [-0.22, +0.82] |
| INSERT, pk omitted | 10,000 | 10 runs | 836 /s | 843 /s | -0 /s [-25, +48] | 1017.0 µs | 1014.7 µs | -2.20 µs [-5.85, +2.65] | 5109.2 µs | 4977.2 µs | -47.50 µs [-1003.30, +423.30] | +0.28 µs [+0.01, +0.70] |
| INSERT again (replicate) | 200 | 10 runs | 790 /s | 768 /s | +6 /s [-21, +20] | 1091.5 µs | 1098.3 µs | +2.65 µs [-8.50, +5.60] | 5194.5 µs | 5082.1 µs | -282.40 µs [-587.60, +352.55] | +0.40 µs [-0.02, +0.49] |
| INSERT again (replicate) | 1,000 | 10 runs | 814 /s | 838 /s | +11 /s [-28, +58] | 1023.8 µs | 1022.3 µs | -1.10 µs [-10.90, +2.10] | 5289.7 µs | 4785.8 µs | -366.10 µs [-1008.00, +730.95] | +0.20 µs [-0.51, +0.45] |
| INSERT again (replicate) | 10,000 | 10 runs | 843 /s | 823 /s | -16 /s [-62, +26] | 1018.4 µs | 1018.2 µs | -1.25 µs [-4.60, +7.30] | 4664.9 µs | 5172.1 µs | +673.80 µs [-440.15, +874.35] | +0.36 µs [+0.18, +0.48] |

**Verdict**, by the rule "inside the noise when the interval of B - A contains zero or overlaps the A/A cell's interval of the
same cores, shape, size and metric" (`archive/.../drivers/verdict_m0.py`), 30 `relaxed` cells per metric:

```
relaxed cells: 30 (shape x rows x cores)
- p50: 29 inside the noise, 1 resolved
    - cores 1, UPDATE by pk, 10,000 rows: B - A +0.20 µs [+0.10, +0.50]; A/A -0.30 [-0.60, -0.20]
- p99: 30 inside the noise, 0 resolved
- QPS: 30 inside the noise, 0 resolved
- server CPU per statement: 29 inside the noise, 1 resolved
    - cores 1, UPDATE by pk, 10,000 rows: B - A +0.41 µs [+0.19, +0.55]; A/A -0.41 [-0.69, +0.17]
```

- **Latency.** The four pk shapes at `cores = 1` read B - A p50 `+0.00` to `+0.20` µs, non-negative in all 12 cells; at
  `cores = 4` `-0.20` to `+0.25` µs. Against 41-60 µs a statement that is at most 0.5 %. Their intervals reach `-0.40` to
  `+0.70` µs, which bounds the cost at the 95 % level at 1.2 % of a statement (the SELECT at `cores = 4`, 200 rows, upper edge
  `+0.70` µs on 57.5 µs). The p99 deltas are `-1.65` to `+1.65` µs on 68-91 µs and every interval contains zero except two
  (INSERT at `cores = 4`, 1,000 and 10,000 rows: `-1.40` [`-1.90`, `-0.10`] and `-1.15` [`-1.40`, `-0.35`] µs, B faster, and
  inside the A/A cell's).
- **Throughput.** `-109` to `+232` /s on 15,700-22,600 /s (at most 1.1 %), every interval inside the A/A cell's; one interval
  excludes zero (the replicate INSERT, `cores = 4`, 1,000 rows, `+162` /s [`+17`, `+586`], B faster).
- **Server CPU.** The delta of `sched_foreground_polled_us` per statement is `+0.02` to `+0.41` µs at `cores = 1` and `-0.11` to
  `+0.30` µs at `cores = 4`, on 13.4-20.4 µs of server CPU. At `cores = 1` the sign is non-negative in all 12 pk cells, which is
  worth saying and not worth more: the A/A cells reach `-0.41` µs on the same measure, and the one resolved cell is the UPDATE
  at 10,000 rows (`+0.41` [`+0.19`, `+0.55`] against `-0.41` [`-0.69`, `+0.17`] in A/A: the two intervals do not overlap, but the
  same shape with the same binary on both sides moves as far, in the other direction).
- **`group` `INSERT`**, where the commit is 1,000 µs: B - A p50 `-6.30` to `+3.55` µs on 1,015-1,100 µs, every interval contains
  zero, throughput `-22` to `+19` /s on 770-845 /s (every interval contains zero), p99 `-917` to `+745` µs on 4,700-5,700 µs
  with intervals of the same width. The server-CPU delta of the INSERT arms is `+0.12` to `+0.31` µs at `cores = 1` and `-0.16`
  to `+0.40` µs at `cores = 4`; five of the 12 INSERT-arm intervals exclude zero, all positive, and there is no `group` A/A
  cell to bound them (the `group` control reads `+0.10` [`+0.03`, `+0.12`] µs at 200 rows, `cores = 1`). Against a commit that
  takes 1,000 µs of wall time it is 0.03 %.
- **`SHOW META` is not a control.** B's reply carries seven more fields (`wal_lineage`, `wal_durable_lsn`, `backup_state`,
  `backup_log_bytes_copied`, `backup_fence_writes_waited`, `backups_completed`, `backups_failed`), and it reads `+0.40` to `+0.60` µs
  slower at `cores = 1`, `+0.45` to `+0.65` µs at `cores = 4` (every interval excludes zero); in the A/A cells it reads `-0.20` to `+0.20` µs. It carries no weight in the verdict.

**No cell is resolved with B slower beyond the A/A floor**, one is resolved by the interval rule and sits below the floor, and
no cell is resolved with B faster beyond it. The statement "BR costs nothing on the statement path" is therefore what the
data support up to about 0.3 µs a statement at `cores = 1` and 0.7 µs at `cores = 4`.

## 4. Where a statement's time goes

A latency is a sum. For one serial connection the parts are readable from the controls and the counters; **a lock or conflict
wait is zero by construction** (one connection, no other writer; the lock family never has a holder to wait for). A's values at
`cores = 1`, 200 rows, in this run:

| part | how it is read | `relaxed` INSERT | `relaxed` SELECT by pk | `relaxed` UPDATE by pk | `group` INSERT |
|---|---|---|---|---|---|
| p50 of the statement | the arm | 41.5 µs | 56.2 µs | 44.0 µs | 1,042.9 µs |
| server CPU (`sched_foreground_polled_us` per statement) | `SHOW META` around the blocks | 13.7 µs (33 %) | 17.4 µs (31 %) | 15.5 µs (35 %) | 28.1 µs (2.7 %) |
| client, socket round trip, framing | p50 minus server CPU | 27.8 µs (67 %) | 38.8 µs (69 %) | 28.5 µs (65 %) | 27.8 µs (2.7 %, the relaxed INSERT's) |
| of which the floor every statement pays | `SET ISOLATION LEVEL`: p50 35.8 µs, server CPU 7.9 µs | 35.8 µs | 35.8 µs | 35.8 µs | 35.8 µs |
| durability wait (the commit's `fsync`) | what is left of the `group` INSERT p50 (1,001 µs is its excess over the `relaxed` INSERT) | - | - | - | 987 µs (94.6 %) |
| lock or conflict wait | one connection | 0 µs | 0 µs | 0 µs | 0 µs |
| a checkpoint run inside the arm | `checkpoint_runs` | none in 192 of 192 server-runs | none | none | exactly one per server-run; the arm's p99 is 5,264 µs against its p50 of 1,043 µs, the `fsync` tail, not shown to be the checkpoint |

So BR's possible per-statement cost lives in the 13-17 µs of server CPU, a third of the statement. The measured deltas
(`+0.02` to `+0.41` µs of server CPU at `cores = 1`) would be 0.2-3 % of that; the A/A cells put the floor of the same measure
at about 0.4 µs. A cost larger than that would have shown.

## 5. The data-file write path, where BR-R6 puts the fence

The `relaxed` cells never reach a checkpoint (section 2), so they cannot price `FilePageDevice::WriteAt`, where
`PageWriteFence::WriteScope` adds three atomic operations per write call when disarmed. `br_ckpt.py` prices it directly: A and
B in alternating order (eight pairs), each a fresh server (`cores = 1`, `relaxed`, `checkpoint_interval_ms = 3000`), a wide
relation of 40,000 rows (`id int64, v int64, payload varchar(1900)`, 100 rows an `INSERT`), then `SHOW META`
polled until a checkpoint run that began after the load had completed. The run's page-write phase sum over its pages is the
per-page cost of the write path. Equal work: the run read flushed exactly 8,546 pages in all sixteen.

| pair | order | A page-write phase | B page-write phase | B - A | A checkpoint run | B checkpoint run | pages flushed |
|---|---|---|---|---|---|---|---|
| 0 | A,B | 1.902 µs/page | 2.021 µs/page | +0.119 µs/page | 1155 ms | 1155 ms | 8,546 pages |
| 1 | B,A | 2.095 µs/page | 1.891 µs/page | -0.204 µs/page | 1105 ms | 1162 ms | 8,546 pages |
| 2 | A,B | 1.982 µs/page | 1.994 µs/page | +0.012 µs/page | 1158 ms | 1139 ms | 8,546 pages |
| 3 | B,A | 2.025 µs/page | 1.916 µs/page | -0.109 µs/page | 1122 ms | 1116 ms | 8,546 pages |
| 4 | A,B | 1.982 µs/page | 2.012 µs/page | +0.030 µs/page | 1123 ms | 1151 ms | 8,546 pages |
| 5 | B,A | 1.846 µs/page | 1.993 µs/page | +0.147 µs/page | 1133 ms | 1146 ms | 8,546 pages |
| 6 | A,B | 1.998 µs/page | 2.037 µs/page | +0.039 µs/page | 1182 ms | 1142 ms | 8,546 pages |
| 7 | B,A | 2.010 µs/page | 1.971 µs/page | -0.039 µs/page | 1104 ms | 1155 ms | 8,546 pages |
| median [min-max] | | 1.990 [1.846 to 2.095] µs/page | 1.994 [1.891 to 2.037] µs/page | +0.021 [-0.204 to +0.147] µs/page | 1128 [1104 to 1182] ms | 1149 [1116 to 1162] ms | 8,546 pages |

The page write costs about 2.0 µs a page (a page-cache `pwrite` of 8 KiB) and the fence's three atomics add `+0.02` µs a page
at the median, inside a spread of `-0.20` to `+0.15` µs between pairs. **The fence is not measurable at its site.** The page-write
phase is 17 ms of a 1.13 s checkpoint run (the rest of the run is not broken down here).

## 6. What the run teaches

- **BR-R6 is true where it is measurable and incomplete in its wording.** `git diff fb953f07 a8110b05 -- src/server/command_dispatcher.cpp`
  adds, on every statement, a chain of up to seven `IEquals(cmd, ...)` compares and an `ArchiveRefusal()` call (a pointer test
  while archiving is off) before the failed-transaction gate, and a `BACKUP` verb routed past the SQL parser. None of it is a
  fence atomic. It prices at no more than the floor above (the p50 of the four pk shapes at `cores = 1` is `+0.00` to `+0.20` µs,
  consistent with zero). BR-R6 should say "the statement path gained a verb test and a
  pointer test", not "nothing".
- **The sign is the only thing the data could be hiding.** At `cores = 1` 12 of 12 pk cells have a non-negative p50 delta and a
  non-negative server-CPU delta, and at `group` five of 12 server-CPU intervals are positive. Each is inside the floor and
  together they are a pattern worth one more A/B if BR's fixed costs are ever the question; this run neither resolves it nor can
  rule it out.
- **Nothing in the `relaxed` arms exercised BR's mechanisms.** No checkpoint ran, no backup was open and no archive was
  configured, so this file prices the *absence* of BR: the dispatcher, the superblock version (22 to 23), the stream's
  `AppendThrough` split and the fence's disarmed atomics. The cost of the mechanisms when on is a separate file.
- **Open decisions.** None of the ones recorded in `CLAUDE.md` or the specs acquired a data point here; BR-R6 is the only claim
  this file speaks to.

## 7. Not measured

- The suite was not executed (no code changed). The BR drills (`tools/br_drill.py`) were not run here; equivalence of restored
  instances is the drills' and the simulation's claim, not this file's.
- No PostgreSQL floor.
- The `group` cells have no A/A control, so a `group` server-CPU interval that excludes zero cannot be set against a measured
  floor of that shape.
- `cores` above 4 and `durability = strict` were not run.

Raw numbers, drivers and logs: `bench/v3.0.0/archive/br-close-v2.7.0-913-ga8110b05/` (`out/m0`, `out/m0b`, `logs`, `drivers`).
