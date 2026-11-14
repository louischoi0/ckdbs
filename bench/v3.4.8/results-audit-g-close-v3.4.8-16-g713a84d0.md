# Audit G1-G15 close measurement: the ordinary write and read paths and a spilling write sit inside the spread, and a server start costs 25-33 ms more

**Thesis.** The audit milestone G1-G15 (`2789a457`, `v3.4.8-13-g2789a457`, against `713a84d0`, `v3.4.8-16-g713a84d0`)
changed mount-time paths, a rollback codec and the var-heap append, and deleted dead code and the
queueing lock ask. Measured as an interleaved A/B of the two builds on one host, **one shape moved
and it is the one the change was written to move: a mount.** `WalStream::Open` now reads the last
segment's tail once more to scrub it, syncs the log, and syncs the log directory, and a server start-to-ready
costs **25.6-32.8 ms more (+40 % to +53 %)** than the baseline in every cell, with no overlap between the two
builds' distributions (every baseline mount is faster than every closing-build mount, 21 of 21, in
three of the four cells). It is a fixed per-mount cost: an empty log pays more than a log holding
4,000 spilled rows (+32.8 ms against +25.6 ms), so it scales with the 64 MiB segment and not with what was logged. **Everything else
sits inside this run's own spread**: a single-session point `UPDATE`/`INSERT`/`SELECT` is equal to
0.5 µs at p50 on the two placement-matched repetitions (M1); the var-heap spill, the cell that
carries the new Brent cycle check, reads +0.4 % to +0.8 % on inserts and -0.2 % to -3.1 % on updates against
inline controls that move by -1.9 % to +4.3 % (M2, at 200, 1,000 and 10,000 rows); scenario 2's
committed TPS is +0.6 % (M3); eight sessions on one hot row read the same refusals and the same
`U`-mode granted waits (M4). **No verdict of "improvement" is claimed anywhere**; the lock-table rewrite
(`lock_table.cpp` -203 lines) shows up in no cell as a gain this run can resolve.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-11-14 (UTC). Builds 02:05-02:09; main pass (M1, M4, scenario 2 x4, scenario 0 x2) 02:10:02-02:22:22; spill and mount cells 02:22:38-02:24:33; label-swapped M1 batch 02:24:33-02:28:37; mount-after-SIGKILL cells about 02:28:40-02:30. About 20 minutes of measurement and 26 minutes from the first command with the builds, inside the one-hour budget |
| Worktree / branch | measured from two detached `git worktree`s under the session scratchpad (`ckA` at `2789a457`, `ckB` at `713a84d0`, each `git status` clean, both removed afterwards); results committed on `worktree-audit-g1-g15-fixes` in `/home/louis/ckdbs/.claude/worktrees/audit-g1-g15-fixes`, whose HEAD at the time was `a8a73f44` (documents only over `713a84d0`: three files under `docs/pending/` and `instructions/`) |
| **A (baseline)** | `2789a457` = `v3.4.8-13-g2789a457`, committed 2026-11-14 01:07:39 UTC. Release (`-DCMAKE_BUILD_TYPE=Release`, `cmake --build --target kds_server`), binary mtime 02:09:00 UTC (after the commit). Copy `kds_server-A`, 7,609,464 bytes, sha256 `bb33f463ef4a0eb50a53b3e5422240f1c8dfa1d563ec12ac70f6a3899bdd4488` |
| **B (closing)** | `713a84d0` = `v3.4.8-16-g713a84d0`, committed 2026-11-14 02:03:38 UTC. Release, binary mtime 02:09:02 UTC (after the commit). Copy `kds_server-B`, 7,598,568 bytes, sha256 `072347a000f1dc851f835f2ac61f5f790c24b90ab5305054143e46c2996f4a5c` |
| Binary discipline | every server of every cell was started from the two copies under the scratchpad's `bin/` (on `/tmp`, tmpfs; the binary only); neither build tree was touched during the run. The two drivers that write their own config (`varheap_spill_benchmark.py`, `mount_cost_benchmark.py`) predate the required `buffer_pool_frames` key and a first attempt was refused by the server at boot (nothing measured); they were run **unmodified** through a one-line `exec` wrapper (`archive/.../scripts/wrap-A.sh`) that appends `buffer_pool_frames = 65536` to the config and then execs the measured copy, one wrapper per build |
| Device | data files and WALs under `/home/louis/bench-runs/audit-g-close/` on `/dev/nvme0n1p1`, ext4 (`df -T` read at the start); `/tmp` (tmpfs) held the binaries and the driver outputs only |
| Host | 8 vCPU Intel Xeon Platinum 8581C @ 2.10 GHz, Linux 6.12.116. The host sampler (every 2 s, 366 + 178 lines) counted `procs=0` - no compiler, `cmake --build` or test process - in every sample. A test suite ran on this host until about 02:04; the one-minute load average was 4.8 at the first sample and fell to 1.1 (reactors spin, so it does not read 0). **The run started after the load had left the CPUs**, and the first minutes are the decaying average, not CPU competition |
| Server config | M1, M4: `cores = 4` on CPUs 2-5, drivers on 6-7, `durability = relaxed`, `buffer_pool_frames = 65536`, `ash_enabled = on`. M3: `cores = 4` on 2-5, drivers on 0,1,6,7, `durability = group`, `ash_interval_ms = 100`. Spill: `durability = strict`, default `cores`, no pinning. Mount: `durability = relaxed`, default `cores`, no pinning. A fresh server and volume per cell |
| Build type | Release |
| Drivers | unmodified `tools/lock_contention_benchmark.py` (M1, M4), `tools/scenario2_freight.py`, `tools/scenario0_stockmarket.py` (M3), `tools/varheap_spill_benchmark.py` (M2), `tools/mount_cost_benchmark.py` (M5); scripts are the CP close's (`bench/v3.4.7/archive/cp-close-v3.4.7-11-g7d21468a/scripts/`, copied with paths edited) plus `scripts/` in this run's archive. The drivers come from the worktree's HEAD (`a8a73f44`); `tools/` is byte-identical between `713a84d0` and it |
| Prior number | **The CP close, `bench/v3.4.7/results-cp-close-v3.4.7-11-g7d21468a.md`**, for M1, M3 and M4 (same drivers, same flags). M2 (the spill driver) and M5 (the mount driver) have **no prior kdbs result file in the tree for this shape**: this run is the baseline the next one is read against. PostgreSQL floor: **not measured for any of these shapes** |
| Raw outputs | `bench/v3.4.8/archive/audit-g-close-v3.4.8-16-g713a84d0/` (driver JSON and text, views, status and host logs, scripts; no data file, WAL segment or server log) |
| Tests | no engine code was changed by this measurement; **no test suite was run by it** (the milestone's suite is the step before this cell) |

## 2. What was measured

Throughout, **A** is `2789a457` and **B** is `713a84d0`; a delta is B minus A. `lock_contention_benchmark.py`
calls its two labels `base` and `close`; in the first M1 batch `base` = A, in the swapped batch `base` = B, and every table here is
translated to A and B.

| Cell | Question | Driver and arms | Size |
|---|---|---|---|
| M1 | do the ordinary write and read paths regress | `lock_contention_benchmark.py --sessions 1`, A against B interleaved over 4 blocks, **6 reps**: 3 with A as the driver's primary label, 3 with the roles swapped | 256 rows; 40,000 ops/arm/rep |
| M2 | what a spilled `varchar` costs (the var-heap append, which now runs a Brent cycle check per hop) | `varheap_spill_benchmark.py --durability strict`, A and B alternated, 2 reps each (A,B then B,A), `--rows 200,1000,10000`; 1,600-byte value, one var-heap page per 5 rows; inline twins as controls | 200 / 1,000 / 10,000 rows; update phase runs 200 / 1,000 / 1,000 statements |
| M3 | throughput and waits under two scenarios | `scenario2_freight.py --bookers 8` x2 per build (A,B,B,A), `scenario0_stockmarket.py --fk --txn` x1 per build | scenario 2: 300 organizations, 30 ships, 300 operations, 24,000 cargos; scenario 0: 2,000 users, 2,000 assets, 8 traders |
| M4 | eight sessions on one hot row: refusals and waits (`lock_table.cpp` changed) | `lock_contention_benchmark.py --sessions 8`, A against B | 256 rows; 4,000 ops/session/arm |
| M5 | what a server start-to-ready costs on a volume with a non-empty, partly filled last segment | `mount_cost_benchmark.py --mounts 7 --durability relaxed`, A and B alternated, 3 reps each (A,B / B,A / A,B), three cells per build: empty log, 4,000 spilled rows (clean stop), 4,000 rows then SIGKILL | 64 MiB segment (not configurable); 4,000 rows is about 6.5 MB of log |

**What was cut.** The CP close's M2 (`bm_scan.py census`) and M5 (the range-fence driver) were not re-run: the scan path and
the fence are not what this milestone touched. The mount cell was not swept over log fill: every cell has a
last segment that is mostly empty (64 MiB, at most a few MB used), which is the **worst case for a scrub that reads to the end of the segment**;
a nearly full segment was not measured. M3 scenario 0 has one run per build (a delta there is read against the CP close's own 5.5 % spread). No PostgreSQL twin was run.

## 3. M5 - mount: +25 to +33 ms, in every cell, fixed per mount

A server start is `Popen` to the `listening on` line, 7 mounts per run on one data file, 3 runs per build and
cell. The last segment is the stream's tail; `WalStream::Open` in B reads the scanned tail's remainder
back in 1 MiB pieces (`ScrubTail`), writes only the non-zero span of a piece, calls `device->Sync()` before the
durable watermark claims the scan, and `FileLogDevice::OpenImpl` syncs the directory for the segments it adopted.
Wall times in ms (one-shot durations, not a rate; QPS does not apply). p99 equals the maximum at n = 21 and at n = 3 p25 equals p0.

| Cell | Build | Mounts | p0 | p25 | p50 | p95 | p99 | mean | `recovery_analysis_us` mean | `recovery_checkpoint_us` mean |
|---|---|---|---|---|---|---|---|---|---|---|
| empty log (rows 0) | A | 21 | 58.1 | 60.4 | 60.9 | 66.8 | 66.8 | 61.7 | 23.5 | 3.4 |
| empty log (rows 0) | B | 21 | 90.2 | 92.7 | 94.9 | 97.2 | 99.9 | 94.5 | 25.7 | 3.5 |
| 4,000 rows, clean stop | A | 21 | 56.0 | 58.5 | 60.3 | 62.9 | 65.1 | 60.3 | 18.0 | 3.6 |
| 4,000 rows, clean stop | B | 21 | 81.6 | 84.2 | 86.3 | 90.3 | 92.4 | 85.9 | 20.0 | 3.8 |
| 4,000 rows, SIGKILL: first mount (replays 29.2-29.5k records) | A | 3 | 103.2 | 103.2 | 106.9 | 112.4 | 112.4 | 107.5 | 26.8 | 14.1 |
| 4,000 rows, SIGKILL: first mount | B | 3 | 133.4 | 133.4 | 133.9 | 138.2 | 138.2 | 135.1 | 29.3 | 14.8 |
| 4,000 rows, SIGKILL: mounts 2-7 of each run | A | 18 | 49.7 | 53.9 | 54.2 | 58.5 | 58.5 | 55.0 | 15.8 | 3.7 |
| 4,000 rows, SIGKILL: mounts 2-7 | B | 18 | 75.0 | 77.3 | 78.0 | 82.2 | 82.2 | 78.4 | 17.3 | 3.7 |

| Cell | Δ mean (B - A) | Δ p50 | Share of A's mean | Baseline spread (A, p0 to max) | Overlap of the two distributions |
|---|---|---|---|---|---|
| empty log | **+32.8 ms** | +34.0 ms | +53 % | 8.7 ms | none (A max 66.8 < B min 90.2) |
| 4,000 rows, clean | **+25.6 ms** | +26.0 ms | +42 % | 9.1 ms | none (A max 65.1 < B min 81.6) |
| 4,000 rows, SIGKILL, first mount | **+27.6 ms** | +27.0 ms | +26 % | 9.2 ms | none (n = 3) |
| 4,000 rows, SIGKILL, mounts 2-7 | **+23.4 ms** | +23.8 ms | +43 % | 8.8 ms | none (A max 58.5 < B min 75.0) |

**Where it goes (what the run can and cannot say).** The server's own recovery counters rise by only 2.0-2.7 ms
(`recovery_analysis_us` +2.2, +2.0, +2.5, +1.5 ms by cell; `recovery_checkpoint_us` unchanged), so about 90 % of the delta is outside recovery, in the log's open - which is
exactly where the diff put the new work. Which of the three additions carries it (the scrub's read and its non-zero search over
about 64 MiB, the log `fsync`, the directory `fsync`) was **not measured**: the host has no `strace` or `perf`, and a black-box driver cannot split them.
The size is consistent with a byte-wise search over the 64 MiB remainder (about 0.4 ns per byte for 25-33 ms); that is a
hypothesis from the arithmetic, not a finding. The shape of the cost is a finding: it is per mount and per segment, not per record, so it does not grow with
log content (the empty log pays more than the 4,000-row log; why it pays 7 ms more was not found).
Wait breakdown: **not applicable** - the mount is a one-shot start with no concurrent waits the engine's `sys.wait_*` views could attribute, and they are zero at this point of boot.

## 4. M1 - the ordinary paths: equal where the placement is equal, and the apparent gaps follow the core

One session, autocommit statements on its own row, `relaxed`, so there is no commit wait in a statement. A
server's one-session pool lands on whichever core SO_REUSEPORT picks, independently per server. The six reps
show that **a pair is only comparable if both servers' sessions sit on cores 2-3**: in every one of the four reps that had a
server on core 0 or core 1, the slow arm was the one on that core, whichever build it was (1.1: B on core 0 slow; 1.2: A on core 1 slow; 1.3: A on core 0 slow; 2.2: B on core 1 slow), and the two reps that
mirror each other (1.2 and 2.2, A on 1 / B on 3 and B on 1 / A on 3) flip sign with the binary. Placement, not build.

| Rep | Core A / B | Placement | Arm | A p50 | B p50 | Δ p50 (B - A) | A QPS | B QPS | QPS, B vs A |
|---|---|---|---|---|---|---|---|---|---|
| 1.1 | 2 / 0 | core 0 involved | update-disjoint | 41.0 µs | 41.8 µs | +0.8 µs | 22,167 | 20,488 | -7.6 % |
| 1.1 | 2 / 0 | core 0 involved | update-hot | 41.0 µs | 52.5 µs | +11.5 µs | 21,695 | 17,980 | -17.1 % |
| 1.1 | 2 / 0 | core 0 involved | insert-omitted | 39.0 µs | 43.0 µs | +4.0 µs | 20,877 | 19,617 | -6.0 % |
| 1.1 | 2 / 0 | core 0 involved | select-hot | 52.0 µs | 62.1 µs | +10.1 µs | 18,201 | 15,054 | -17.3 % |
| 1.2 | 1 / 3 | clean, core 1 | update-disjoint | 41.4 µs | 40.9 µs | -0.5 µs | 20,911 | 22,230 | +6.3 % |
| 1.2 | 1 / 3 | clean, core 1 | update-hot | 51.4 µs | 40.9 µs | -10.5 µs | 19,557 | 22,070 | +12.8 % |
| 1.2 | 1 / 3 | clean, core 1 | insert-omitted | 48.6 µs | 39.3 µs | -9.3 µs | 18,605 | 20,339 | +9.3 % |
| 1.2 | 1 / 3 | clean, core 1 | select-hot | 71.1 µs | 52.5 µs | -18.6 µs | 14,773 | 17,430 | +18.0 % |
| 1.3 | 0 / 3 | core 0 involved | update-disjoint | 44.5 µs | 40.6 µs | -3.9 µs | 20,539 | 22,620 | +10.1 % |
| 1.3 | 0 / 3 | core 0 involved | update-hot | 42.7 µs | 40.8 µs | -1.9 µs | 20,884 | 22,484 | +7.7 % |
| 1.3 | 0 / 3 | core 0 involved | insert-omitted | 48.7 µs | 39.0 µs | -9.7 µs | 19,116 | 21,215 | +11.0 % |
| 1.3 | 0 / 3 | core 0 involved | select-hot | 66.5 µs | 52.2 µs | -14.3 µs | 14,991 | 18,178 | +21.3 % |
| **2.1** | 2 / 3 | **matched {2,3}** | update-disjoint | 40.5 µs | 40.9 µs | +0.4 µs | 22,945 | 22,424 | -2.3 % |
| **2.1** | 2 / 3 | **matched** | update-hot | 40.4 µs | 40.7 µs | +0.3 µs | 22,855 | 22,857 | +0.0 % |
| **2.1** | 2 / 3 | **matched** | insert-omitted | 38.9 µs | 39.1 µs | +0.2 µs | 21,128 | 21,100 | -0.1 % |
| **2.1** | 2 / 3 | **matched** | select-hot | 52.7 µs | 53.0 µs | +0.3 µs | 17,929 | 17,804 | -0.7 % |
| 2.2 | 3 / 1 | clean, core 1 | update-disjoint | 40.8 µs | 41.2 µs | +0.4 µs | 22,410 | 21,635 | -3.5 % |
| 2.2 | 3 / 1 | clean, core 1 | update-hot | 40.7 µs | 46.0 µs | +5.3 µs | 22,348 | 20,320 | -9.1 % |
| 2.2 | 3 / 1 | clean, core 1 | insert-omitted | 38.9 µs | 43.7 µs | +4.8 µs | 21,056 | 19,424 | -7.8 % |
| 2.2 | 3 / 1 | clean, core 1 | select-hot | 52.6 µs | 53.0 µs | +0.4 µs | 17,914 | 16,819 | -6.1 % |
| **2.3** | 3 / 2 | **matched {2,3}** | update-disjoint | 40.6 µs | 40.5 µs | -0.1 µs | 22,951 | 22,935 | -0.1 % |
| **2.3** | 3 / 2 | **matched** | update-hot | 40.7 µs | 40.7 µs | +0.0 µs | 22,894 | 22,714 | -0.8 % |
| **2.3** | 3 / 2 | **matched** | insert-omitted | 39.0 µs | 39.0 µs | +0.0 µs | 21,242 | 21,229 | -0.1 % |
| **2.3** | 3 / 2 | **matched** | select-hot | 52.8 µs | 52.3 µs | -0.5 µs | 17,971 | 18,086 | +0.6 % |

(Reps 1.x have A as the driver's primary label, 2.x have B; QPS is the driver's `ops / elapsed`.)
The two matched reps carry the comparison. Their full distributions:

| Rep | Arm | Build | ops | p0 | p25 | p50 | p95 | p99 | mean | QPS |
|---|---|---|---|---|---|---|---|---|---|---|
| 2.1 | update-disjoint | A | 40,000 | 37.8 µs | 39.7 µs | 40.5 µs | 49.9 µs | 61.3 µs | 42.8 µs | 22,945 QPS |
| 2.1 | update-disjoint | B | 40,000 | 36.8 µs | 40.1 µs | 40.9 µs | 50.2 µs | 61.9 µs | 43.8 µs | 22,424 QPS |
| 2.1 | update-disjoint-again | A | 40,000 | 38.1 µs | 39.7 µs | 40.5 µs | 50.7 µs | 63.6 µs | 43.2 µs | 22,733 QPS |
| 2.1 | update-disjoint-again | B | 40,000 | 38.1 µs | 40.0 µs | 40.9 µs | 50.4 µs | 69.6 µs | 43.5 µs | 22,578 QPS |
| 2.1 | update-hot | A | 40,000 | 35.5 µs | 39.7 µs | 40.4 µs | 49.9 µs | 62.5 µs | 43.0 µs | 22,855 QPS |
| 2.1 | update-hot | B | 40,000 | 38.1 µs | 39.9 µs | 40.7 µs | 49.5 µs | 60.1 µs | 43.0 µs | 22,857 QPS |
| 2.1 | insert-omitted | A | 40,000 | 35.2 µs | 38.2 µs | 38.9 µs | 49.3 µs | 66.4 µs | 46.7 µs | 21,128 QPS |
| 2.1 | insert-omitted | B | 40,000 | 34.7 µs | 38.3 µs | 39.1 µs | 49.0 µs | 62.8 µs | 46.8 µs | 21,100 QPS |
| 2.1 | select-hot | A | 40,000 | 45.0 µs | 51.5 µs | 52.7 µs | 63.8 µs | 75.7 µs | 55.1 µs | 17,929 QPS |
| 2.1 | select-hot | B | 40,000 | 49.2 µs | 51.8 µs | 53.0 µs | 64.4 µs | 74.6 µs | 55.5 µs | 17,804 QPS |
| 2.3 | update-disjoint | A | 40,000 | 38.2 µs | 39.9 µs | 40.6 µs | 49.4 µs | 59.0 µs | 42.8 µs | 22,951 QPS |
| 2.3 | update-disjoint | B | 40,000 | 37.7 µs | 39.8 µs | 40.5 µs | 49.8 µs | 61.1 µs | 42.8 µs | 22,935 QPS |
| 2.3 | update-disjoint-again | A | 40,000 | 37.2 µs | 40.0 µs | 40.8 µs | 50.5 µs | 67.4 µs | 43.6 µs | 22,524 QPS |
| 2.3 | update-disjoint-again | B | 40,000 | 38.1 µs | 39.8 µs | 40.6 µs | 51.5 µs | 105.3 µs | 43.7 µs | 22,468 QPS |
| 2.3 | update-hot | A | 40,000 | 38.2 µs | 39.9 µs | 40.7 µs | 49.9 µs | 60.5 µs | 43.0 µs | 22,894 QPS |
| 2.3 | update-hot | B | 40,000 | 38.1 µs | 39.9 µs | 40.7 µs | 50.3 µs | 62.6 µs | 43.3 µs | 22,714 QPS |
| 2.3 | insert-omitted | A | 40,000 | 35.0 µs | 38.2 µs | 39.0 µs | 48.4 µs | 59.8 µs | 46.4 µs | 21,242 QPS |
| 2.3 | insert-omitted | B | 40,000 | 34.6 µs | 38.2 µs | 39.0 µs | 49.0 µs | 62.9 µs | 46.5 µs | 21,229 QPS |
| 2.3 | select-hot | A | 40,000 | 49.1 µs | 51.7 µs | 52.8 µs | 63.8 µs | 73.8 µs | 54.9 µs | 17,971 QPS |
| 2.3 | select-hot | B | 40,000 | 47.0 µs | 51.2 µs | 52.3 µs | 63.2 µs | 74.3 µs | 54.6 µs | 18,086 QPS |

**Reading: inside the spread.** On the matched reps the p50 delta is -0.5 to +0.4 µs (about 1 %) on every arm, the p25 delta
-0.5 to +0.4 µs, and the noise floor of the run (the same statement measured twice, `update-disjoint` against `update-disjoint-again`) is
0.0-0.2 µs at p50. Whole-server `cpu.on_cpu` per statement (A minus B, derived over 240,000 statements) is +3.09, -2.07, -2.53, -1.30, +3.38 and +0.17 µs
over the six reps: a spread of about ±3 µs that changes sign, so no CPU delta is claimed. Wait breakdown: `relaxed` has no fsync in the statement and one session has no one to wait for; `sys.lock_waits`
is empty in every M1 run, so a statement's time is the client round trip plus the server's CPU, and the run can resolve ±0.5 µs of a 40 µs statement.
Spill and mount are the shapes that the milestone's code reaches; this cell is the regression check that nothing ordinary moved.

**Delta against the CP close** (`v3.4.7-11-g7d21468a`, the same driver, flags and shape): its closing build measured `update-disjoint` p50 52.5-59.3 µs in batch 1 and
its own text records that day's host reading the *unchanged baseline binary* 11-20 µs slower than the S0 day (39.1-39.7 µs). This run's A and B read 40.5-41.4 µs, i.e. the S0-day level. The cross-day gap is the host, as that document found,
and not an engine delta; the delta that counts is A against B in this run, which is nil.

## 5. M2 - the var-heap spill: inside the spread at 200, 1,000 and 10,000 rows

`varheap_spill_benchmark.py` times each statement on one connection with Python's socket round trip included. `ping` (`SHOW META`) reads p0 491-542 µs, p50 531-609 µs: **a statement's wall time is mostly the client round trip**,
so the driver resolves a few percent, not nanoseconds. Each row of the table is the mean of the two reps' percentiles (reps alternated A,B then B,A);
QPS is the driver's `ops / elapsed` averaged the same way. The inline phases are the control: they touch no var-heap page.
`WAL written` is byte-identical between the builds for every phase (819,368 bytes after 200 spilled inserts, 4,117,656 after 1,000, 67,108,864 = one full segment at 10,000), and `--verify` read every row back clean in all 12 runs.

| rows | phase | build | ops/rep | p0 | p25 | p50 | p95 | p99 | QPS | QPS, B vs A |
|---|---|---|---|---|---|---|---|---|---|---|
| 200 | insert-spill | A | 200 | 453 µs | 618 µs | 688 µs | 931 µs | 1144 µs | 1,244 QPS |  |
| 200 | insert-spill | B | 200 | 410 µs | 585 µs | 659 µs | 892 µs | 1043 µs | 1,309 QPS | +5.3 % |
| 200 | insert-inline | A | 200 | 309 µs | 492 µs | 564 µs | 816 µs | 1000 µs | 1,683 QPS |  |
| 200 | insert-inline | B | 200 | 336 µs | 474 µs | 547 µs | 764 µs | 957 µs | 1,754 QPS | +4.3 % |
| 200 | update-spill | A | 200 | 450 µs | 607 µs | 693 µs | 935 µs | 1106 µs | 1,270 QPS |  |
| 200 | update-spill | B | 200 | 428 µs | 607 µs | 696 µs | 1014 µs | 1313 µs | 1,231 QPS | -3.1 % |
| 200 | update-inline | A | 200 | 385 µs | 522 µs | 590 µs | 797 µs | 941 µs | 1,627 QPS |  |
| 200 | update-inline | B | 200 | 340 µs | 512 µs | 588 µs | 810 µs | 960 µs | 1,596 QPS | -1.9 % |
| 1000 | insert-spill | A | 1,000 | 380 µs | 589 µs | 669 µs | 908 µs | 1040 µs | 1,301 QPS |  |
| 1000 | insert-spill | B | 1,000 | 356 µs | 576 µs | 653 µs | 906 µs | 1611 µs | 1,307 QPS | +0.4 % |
| 1000 | insert-inline | A | 1,000 | 323 µs | 465 µs | 532 µs | 749 µs | 912 µs | 1,792 QPS |  |
| 1000 | insert-inline | B | 1,000 | 319 µs | 459 µs | 526 µs | 727 µs | 892 µs | 1,813 QPS | +1.2 % |
| 1000 | update-spill | A | 1,000 | 368 µs | 566 µs | 642 µs | 865 µs | 1013 µs | 1,348 QPS |  |
| 1000 | update-spill | B | 1,000 | 384 µs | 578 µs | 655 µs | 885 µs | 1049 µs | 1,322 QPS | -2.0 % |
| 1000 | update-inline | A | 1,000 | 333 µs | 478 µs | 542 µs | 737 µs | 871 µs | 1,770 QPS |  |
| 1000 | update-inline | B | 1,000 | 328 µs | 477 µs | 540 µs | 745 µs | 892 µs | 1,765 QPS | -0.3 % |
| 10000 | insert-spill | A | 10,000 | 348 µs | 600 µs | 682 µs | 925 µs | 1105 µs | 1,232 QPS |  |
| 10000 | insert-spill | B | 10,000 | 347 µs | 594 µs | 676 µs | 914 µs | 1086 µs | 1,242 QPS | +0.8 % |
| 10000 | insert-inline | A | 10,000 | 284 µs | 489 µs | 559 µs | 779 µs | 938 µs | 1,712 QPS |  |
| 10000 | insert-inline | B | 10,000 | 289 µs | 495 µs | 567 µs | 792 µs | 950 µs | 1,690 QPS | -1.3 % |
| 10000 | update-spill | A | 1,000 | 391 µs | 599 µs | 675 µs | 904 µs | 1064 µs | 1,272 QPS |  |
| 10000 | update-spill | B | 1,000 | 385 µs | 610 µs | 686 µs | 907 µs | 1058 µs | 1,270 QPS | -0.2 % |
| 10000 | update-inline | A | 1,000 | 329 µs | 486 µs | 555 µs | 768 µs | 966 µs | 1,716 QPS |  |
| 10000 | update-inline | B | 1,000 | 307 µs | 481 µs | 548 µs | 761 µs | 875 µs | 1,754 QPS | +2.2 % |

Per-rep QPS of the two spill phases (rep 1 / rep 2): 200 rows insert-spill A 1,201 / 1,286, B 1,346 / 1,272; update-spill A 1,229 / 1,311, B 1,266 / 1,196;
1,000 rows insert-spill A 1,314 / 1,288, B 1,295 / 1,318; update-spill A 1,347 / 1,350, B 1,322 / 1,321; 10,000 rows insert-spill A 1,254 / 1,210, B 1,258 / 1,226; update-spill A 1,261 / 1,282, B 1,224 / 1,316.

**Reading: inside the spread.** The spill phases move by +5.3 % to -3.1 %, and the inline controls, which cannot be touched by var-heap code, move by +4.3 % to -1.9 %
at the same 200-row size; two repetitions of A alone differ by 7 % there (1,201 against 1,286). The 1,000- and 10,000-row cells, where the spread is under 4 %, read
+0.4 % / +0.8 % (inserts) and -2.0 % / -0.2 % (updates). Nothing reads as a regression or an improvement. What this driver **cannot** resolve is a cost below
about 20-30 µs per statement (3-4 % of 660 µs); the cycle check is a per-hop comparison and, since a var-heap append starts at the chain's last tail (BN-R8, `CLAUDE.md`), a typical append walks few hops, so a cost of that size
would need a very long walk. The chain length at 10,000 rows (about 2,000 pages) is the longest this run exercised, and no per-statement cost showed at it. Not measured: server CPU per spill statement (this driver reads no views), so a sub-microsecond cost is not excluded.
Wait breakdown: the driver reads no wait views and `strict` adds the commit `fsync` inside every statement; the round trip floor is the `ping` p0 of 491-542 µs, the inline controls' p0 of 284-385 µs are below it (the ping reply is
larger) and the spill's p0 exceeds its inline control's by 37-144 µs. A finer split needs the views and was not taken.

## 6. M3 - the scenarios: equal throughput, waits granted, nothing refused that was not refused before

### 6.1 Scenario 2 (`--bookers 8`, `group`)

Each run ended early (the driver reports that the 24,000-cargo pool ran out before the 60 s allowance, as it did at the CP close), so TPS is an average over that window. Order of runs: A, B, B, A.

| Run | Build | committed | TPS | booking p0 / p25 / p50 / p95 / p99 | commit p0 / p25 / p50 / p95 / p99 | `--verify` failures of 400 | `TXN_CONFLICT` retried |
|---|---|---|---|---|---|---|---|
| a | A | 19,893 | 2,294.9 TPS | 428.8 / 1,274.7 / 2,579.2 / 3,421.9 / 4,598.1 µs | 388.0 / 748.2 / 896.1 / 1,367.0 / 1,843.0 µs | 111 | 5 |
| b | A | 19,898 | 2,274.3 TPS | 387.8 / 1,254.4 / 2,591.8 / 3,390.6 / 4,688.7 µs | 383.2 / 754.2 / 902.1 / 1,356.5 / 1,966.1 µs | 106 | 2 |
| a | B | 19,896 | 2,302.2 TPS | 413.5 / 1,304.8 / 2,577.8 / 3,401.8 / 4,533.7 µs | 395.6 / 755.3 / 903.9 / 1,363.9 / 1,893.3 µs | 114 | 5 |
| b | B | 19,890 | 2,293.4 TPS | 408.4 / 1,314.8 / 2,581.9 / 3,392.7 / 4,500.8 µs | 404.7 / 749.5 / 897.4 / 1,369.3 / 1,954.4 µs | 109 | 10 |

Mean TPS **A 2,284.6, B 2,297.8 (+0.6 %)**; the within-build spread is 0.9 % (A) and 0.4 % (B), and the delta is inside the spread of the pair. The `--verify` failures (106-114 of 400) appear at the same level on both builds (the CP close recorded 91-121 for the same driver); they were not analysed here. The statement phases of run `a` of each build:

| Phase | Build | ops | errors | p0 | p25 | p50 | p95 | p99 | rate |
|---|---|---|---|---|---|---|---|---|---|
| operation-update | A | 19,898 | 3 | 41.1 µs | 84.7 µs | 99.7 µs | 209.1 µs | 315.2 µs | 2,296/s |
| operation-update | B | 19,901 | 4 | 44.1 µs | 84.8 µs | 99.4 µs | 218.9 µs | 331.2 µs | 2,303/s |
| org-update | A | 19,895 | 2 | 38.7 µs | 74.7 µs | 87.9 µs | 224.8 µs | 322.7 µs | 2,295/s |
| org-update | B | 19,897 | 1 | 38.3 µs | 74.5 µs | 88.0 µs | 231.5 µs | 330.7 µs | 2,302/s |
| freight-insert | A | 19,898 | 0 | 44.8 µs | 99.6 µs | 123.2 µs | 259.1 µs | 403.3 µs | 2,296/s |
| freight-insert | B | 19,901 | 0 | 44.2 µs | 99.2 µs | 122.5 µs | 263.3 µs | 433.9 µs | 2,303/s |
| charge-insert | A | 111,932 | 0 | 35.9 µs | 67.7 µs | 83.2 µs | 197.5 µs | 311.1 µs | 12,913/s |
| charge-insert | B | 111,949 | 0 | 34.9 µs | 67.4 µs | 82.6 µs | 193.6 µs | 312.6 µs | 12,954/s |

**Wait breakdown (DB time, `ANALYZE`'s account, run `a`).** A statement is a sum: client round trip and the server's CPU, the group commit's `fsync`, the run queue, WAL writes, and lock waits. For scenario 2 over the run:

| Build | `cpu.on_cpu` | `commit.wal_flush` | `sched.run_queue` | `sysio.wal_write` | `sysio.checkpoint` | `app.lock_wait` | `sys.lock_waits` rows (all tuple `U`, granted; deadlock / `fault_net` / cancelled) |
|---|---|---|---|---|---|---|---|
| A, run a | 49.9 % | 46.4 % | 2.7 % | 0.75 % | 0.27 % | 0.027 % (19.5 ms, 44 rows, max 9.2 ms) | 0 / 0 / 0 |
| A, run b | 50.9 % | 46.3 % | 2.1 % | 0.35 % | 0.27 % | 0.053 % (39.3 ms, 50 rows, max 21.4 ms) | 0 / 0 / 0 |
| B, run a | 50.8 % | 46.4 % | 2.3 % | 0.31 % | 0.26 % | 0.017 % (12.8 ms, 58 rows, max 8.0 ms) | 0 / 0 / 0 |
| B, run b | 51.6 % | 46.0 % | 1.8 % | 0.36 % | 0.28 % | 0.007 % (5.1 ms, 58 rows, max 0.3 ms) | 0 / 0 / 0 |

Lock waits are a thousandth of DB time on both builds; the half of DB time that is not CPU is the group commit's fsync. The two builds' split is the same to a point, and the lock-wait totals (5-39 ms) are a few outliers (a 9-21 ms wait) that appear on both.
The only client-visible refusal is `TXN_CONFLICT` of the first-updater-wins rule, retried by the driver, at 2-10 per run on both.

### 6.2 Scenario 0 (`--fk --txn`, 8 traders, `group`)

One run per build, so no within-build spread of this run; the CP close's own spread (5.5 % over three runs) bounds what a delta here can mean.

| Build | committed | TPS | txn p0 / p25 / p50 / p95 / p99 | trade-insert p0 / p25 / p50 / p95 / p99 | account-update p0 / p25 / p50 / p95 / p99 | `app.lock_wait` | `commit.wal_flush` / `cpu.on_cpu` / run queue |
|---|---|---|---|---|---|---|---|
| A | 302,697 | 5,044.9 TPS | 620.6 / 1,291.5 / 1,479.5 / 2,004.3 / 2,512.3 µs | 38.6 / 86.5 / 109.8 / 209.7 / 358.2 µs | 37.1 / 75.5 / 91.4 / 179.2 / 354.3 µs | 0 | 85.0 % / 12.9 % / 1.7 % |
| B | 307,882 | 5,131.4 TPS | 577.9 / 1,267.2 / 1,458.2 / 1,959.8 / 2,452.1 µs | 38.1 / 84.6 / 106.6 / 203.2 / 350.7 µs | 37.5 / 74.6 / 89.8 / 172.6 / 348.8 µs | 0 | 85.2 % / 12.8 % / 1.7 % |

B reads +1.7 % TPS, inside that spread: not a finding. Scenario 0 is **85 % commit wait** (`commit.wal_flush`) in DB time and shares no parent between writers, so there is no lock wait to measure and the lock table is not stressed by it.
Balance verify (200 accounts) matched on both builds; 0 rolled back, 0 torn.

**Delta against the CP close** (`v3.4.7-11-g7d21468a`, same drivers and flags): scenario 2 close build 2,312.8 TPS (mean of three), now A 2,284.6 and B 2,297.8 (-1.2 % and -0.7 % of that number, against that run's own 2.6 % spread);
scenario 0 close build 5,199 TPS (mean of three), now A 5,045 and B 5,131 (one run each; -3.0 % and -1.3 %, against that run's 5.5 % spread). Both inside the CP close's spreads; across days the host is not controlled (M1's note), so these are context and the same-day A/B above is the comparison.

## 7. M4 - eight sessions on one hot row: the same refusals, the same waits

`lock_contention_benchmark.py --sessions 8`, 4,000 ops per session per arm (32,000 per arm). The servers' accept cores differ in this run (A: 0,1,2,3; B: 1,2,3), a placement difference of the kind M1 shows costs a few µs, so a small gap is not claimed.

| Arm | Build | ops | errors | p0 | p25 | p50 | p95 | p99 | max | QPS |
|---|---|---|---|---|---|---|---|---|---|---|
| update-hot | A | 32,000 | 3,130 | 32.8 µs | 198.6 µs | 315.3 µs | 791.9 µs | 1,088.4 µs | 3,240.3 µs | 21,390 QPS |
| update-hot | B | 32,000 | 2,901 | 37.3 µs | 193.1 µs | 303.0 µs | 762.5 µs | 1,067.9 µs | 5,730.6 µs | 22,153 QPS |
| update-disjoint | A | 32,000 | 0 | 38.8 µs | 196.2 µs | 303.9 µs | 762.0 µs | 1,042.2 µs | 3,083.5 µs | 22,196 QPS |
| update-disjoint | B | 32,000 | 0 | 34.3 µs | 191.6 µs | 298.7 µs | 745.1 µs | 1,040.0 µs | 5,294.5 µs | 22,631 QPS |
| update-disjoint-again | A | 32,000 | 0 | 33.2 µs | 187.6 µs | 298.3 µs | 754.3 µs | 1,041.0 µs | 6,046.4 µs | 22,398 QPS |
| update-disjoint-again | B | 32,000 | 0 | 38.9 µs | 182.3 µs | 292.4 µs | 741.5 µs | 1,036.5 µs | 5,268.3 µs | 22,861 QPS |
| insert-omitted | A | 32,000 | 0 | 37.2 µs | 187.5 µs | 296.0 µs | 747.2 µs | 1,042.1 µs | 6,674.3 µs | 22,408 QPS |
| insert-omitted | B | 32,000 | 0 | 29.5 µs | 129.7 µs | 222.5 µs | 664.4 µs | 945.7 µs | 269,377.3 µs | 21,358 QPS |
| select-hot | A | 32,000 | 0 | 49.8 µs | 265.0 µs | 417.6 µs | 1,058.8 µs | 1,443.5 µs | 6,591.4 µs | 16,055 QPS |
| select-hot | B | 32,000 | 0 | 49.4 µs | 266.6 µs | 416.9 µs | 1,056.1 µs | 1,446.3 µs | 6,246.0 µs | 16,156 QPS |

Both builds refuse the hot-row arm the same way (3,130 and 2,901 of 32,000 `TXN_CONFLICT retryable=1 row id=1 was written by transaction N`, the first-updater-wins rule; the QPS of that arm counts completed statements). `sys.lock_waits` on both builds is all tuple `U`, **all granted**: A 17 rows, 282 µs total, max 57 µs; B 30 rows, 535 µs total, max 94 µs;
0 deadlock, 0 `fault_net`, 0 cancelled. `app.lock_wait` events 2,298 (A, 976 µs) and 2,404 (B, 1,411 µs), +4.6 %, the same order of magnitude and inside what two identical runs of this driver differ by in the CP close; hot-minus-disjoint at p50 is +11.4 µs (A) and +4.3 µs (B), against noise floors (disjoint against its repeat) of 5.6 and 6.3 µs.
The single 269 ms `insert-omitted` max on B is one stall in 32,000 and is not repeated in any other arm or run.
Wait breakdown: lock waits are 976-1,411 µs against 20-21 s of `cpu.on_cpu` on each server (0.005-0.007 %); the arm's time is the eight sessions' queueing on four reactors and the round trip, not lock time.
**Delta against the CP close:** `app.lock_wait` events were 1,648 (base) and 1,634 (close) there and 2,298 and 2,404 here, a higher count on both builds that this run does not attribute (the arm is the same driver and flags); the same-day pair is the comparison. Mode `U` on every wait on both builds, as at the CP close.

## 8. Insights

1. **The one cost is fixed and sits on the restart path.** Mounting now takes 25-33 ms longer on a 64 MiB segment, whatever the log holds, and the recovery counters see only 2 ms of it. The fix is a durability one - a tail
   claimed durable only after it is synced, and what lies past the end zeroed so that a lost run's stale record cannot rejoin the stream (`wal.md` §4.2) - and the price is paid where the engine can best afford it: once per start, not per statement.
   What the result does say is that the price follows the *segment*, not the log content: the empty log paid more than the 4,000-row log, which is consistent with a scrub that reads to the end of the segment and finds less to skip in a fuller one (not tested). If a restart-heavy deployment (tests that start many servers; the simulation harness runs on in-memory devices and
   is not affected) finds 30 ms matters, the places to look are the scrub's byte-wise search and the two `fsync`s, in that order of suspicion, and a measurement with a profiler should split them before anything is changed; this run did not.
2. **A one-session pair's apparent 10-20 µs gaps are core placement, not build.** The reps where a server accepted on core 0 or core 1 read 5-19 µs slow on whichever build was there. Any future A/B on this driver should pin the placement (a driver `--pin-core` flag exists) or discard unmatched pairs, as this run did.
3. **Nothing in the ordinary paths moved.** The lock-table rewrite (the queueing ask removed), the unified row-compensation codec and the one undo tail codec show no effect on a point write, a point read, a spilling write, a 4-core booking scenario or eight sessions on one row, at the resolution above (about ±0.5 µs on a 40 µs statement; ±3-4 % on the spill driver). The wait picture is unchanged: scenario 2 is half CPU, half group-commit fsync, with lock waits at a thousandth of DB time on both builds; scenario 0 is 85 % commit wait.

## 9. Limits and what was not done

| Not measured | Why |
|---|---|
| Which part of the mount delta is the scrub, the log sync or the directory sync | no `strace` or `perf` on the host; a driver cannot split them, and no engine code may be edited by this measurement |
| Mount cost against log fill (a nearly full last segment) | cut for the budget; every cell's last segment is mostly empty, the likely worst case |
| Server CPU per spill statement | the spill driver reads no wait or CPU views |
| A rollback or crash-recovery path under load (the row-compensation and undo-tail codecs) | no driver for it in `tools/`; the mount-after-SIGKILL cell replays 29k records and rolls back nothing (0 losers) |
| The scan path (the CP close's M2) and the range fence (M5) | not touched by this milestone; not re-run |
| PostgreSQL floor | no twin run for these shapes (rule 4: a missing twin is not a hole) |

## 10. Reproduce

Scripts, in the archive's `scripts/`: `master.sh` (M1, M4, M3 in the order run), `master2.sh` (spill, mount, the swapped M1 batch), `spill_mount.sh`, `wrap-A.sh` (the config wrapper; `wrap-B.sh` is the same with B's path), the CP close's `run_m1m4.sh` and `run_m3.sh` with paths set through environment variables.

```
# builds: git worktree add --detach <dir> {2789a457|713a84d0}; cmake -DCMAKE_BUILD_TYPE=Release; cmake --build --target kds_server
# M1 (three reps; swapped batch exchanges BIN_BASE and BIN_CLOSE)
lock_contention_benchmark.py --port P --label base --ab-port Q --ab-label close --sessions 1 --rows 256 --ops 40000 --blocks 4
# M4
lock_contention_benchmark.py ... --sessions 8 --rows 256 --ops 4000 --blocks 4
# M3
scenario2_freight.py --suffix m3s2 --organizations 300 --ships 30 --operations 300 --cargos 24000 --verify 100 --seed 1 --sync --bookers 8 --seconds 60 --bookings 0
scenario0_stockmarket.py --suffix m3s0 --users 2000 --assets 2000 --traders 8 --seconds 60 --fk --txn --verify 200 --seed 1 --sync
# M2, rows in {200, 1000, 10000}
varheap_spill_benchmark.py --binary <wrapper> --label X --rows N --durability strict --port 15461 --scratch <ext4 dir> --json X.json --force
# M5, rows in {0, 4000}; the crash cell adds --crash
mount_cost_benchmark.py --binary <wrapper> --label X --rows N --mounts 7 --durability relaxed --port 15481 --scratch <ext4 dir> --json X.json --force
```

`--force` on the two drivers overrides their load-average guard, which read the decaying average of the suite that had just ended; `host2.log` and `pass1/host.log` show no competing process. Row-count mapping: M1/M4 `--rows` is the table size; M2 `--rows` is the spilled-row count and the chain length is rows / 5 pages; M5 `--rows` is the spilled rows loaded before the mounts.
