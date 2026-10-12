# BR's close: what backup, archive and restore cost (`v2.7.0-913-ga8110b05`)

**Thesis.** On the binary BR closed at, **the archive costs a third of the wide-`INSERT` throughput when it shares the log's
device and almost nothing when it does not; a base backup costs the workload 4-13 % of its service rate and, at `cores = 1`, four times its p99 for the
few seconds the copy runs; and a restore is 17-22 s for a 1.1 GiB volume with a 200-330 MiB log window and 2 min 18 s for the
same volume replayed through 5.9 GiB of archive past its stop. One restore did not open: a backup whose log window is 3.4 GiB
refuses at its completion checkpoint, and the same refusal reproduces on the commit BR opened from, so it is the engine's.**
All four numbers are bounded by one device that writes 165-173 MB/s (`dd`, fsynced): the archive's copy, the log's own writes and
the checkpoint's page writes share it, which is why the archive's price moves from -29 % to -3 % when its target is tmpfs, and
why the base copy's own speed is set by the throttle and not by the device.

| item | measured | against |
|---|---|---|
| archive on, wide `INSERT` (50 rows x 1.8 KB), `cores = 1` / `cores = 4` | 147 /s and 151 /s | 206 /s with archive off: -28.7 % [-28.9, -28.0] and -26.7 % [-28.5, -26.3], paired over 5 rounds |
| archive on, small statements (`INSERT`, `SELECT`, `UPDATE` by pk) | -2.0 % to +0.5 % (medians) | inside the spread of the rounds (up to +-11.7 % on `UPDATE`, whose rounds range 18,900-22,100 /s) |
| archive on a tmpfs target, wide `INSERT`, `cores = 1` | 200 /s | 207 /s off, 148 /s archive on the data device: the device is the cost (-3 % is the engine's) |
| base copy of 1.06 GiB under load | 4.2-5.3 s, 249 MiB/s median | throttle 256 MiB/s |
| workload during the copy, `cores = 1` | p50 `+0.9` µs, p99 83 to 334 µs (x4), service rate -8 % | `cores = 4`: p50 `+3.5` µs, p99 147 to 146 µs, rate -3.9 % |
| archive vs generation, 6 writers | archives 48.6 of 54.3 MiB/s in steady state; drains a 39-segment backlog at 83 MiB/s | generation mean 56.6, peak 5 s window 106.7 MiB/s |
| restore, 1.1 GiB volume, 200-330 MiB window | 17.1-22.0 s (layout 8.7-11.3 s, mount 8.4-10.7 s) | replay (analysis + redo) 376 and 405 MiB/s of log |
| restore through the archive past the stop (PITR to `latest`), 9.2 GiB of log replayed | 138.5 s (layout 93.5 s, mount 45 s) | replay 318 MiB/s |
| restore of a backup with a 3.4 GiB window | refused after 49 s: `completion checkpoint: WalStream: record of 1408648 bytes is larger than the ring` | the same refusal on crash recovery at the commit BR opened from |

- **A** = `fb953f07` (`v2.7.0-908-gfb953f07`). Used only for the diagnosis in section 6; every number above is B's.
- **B** = `a8110b05` (`v2.7.0-913-ga8110b05`), BR closed in code. Binary `af25c71f…`.
- The statement-path overhead A/B with backup and archive off is in `results-br-close-overhead-v2.7.0-913-ga8110b05.md`.
  This file is B alone (A has no archive and no backup), so its baseline is B with the feature off, in the same run.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-12, 16:17 to 17:30 UTC. M1a (archive) 16:17-16:23 and the tmpfs diagnostic 16:23-16:26; M1b (backup) 16:29-16:39; M2 16:39-16:43; M3 (restore) 16:44-16:58; the diagnosis of section 6 16:58-17:30 |
| Branch / worktree | `br-backup-restore`, worktree `br-backup-restore`; HEAD `a8110b05` |
| Tree | `git status --short` at 15:45 UTC, before anything was built: one modified tracked file, `instructions/v3.0.0/workorder-br-backup-restore.md`; nothing under `src/`, `include/` or `tools/` differed from `a8110b05`. The binaries were built from `git archive` exports of the commits, so the worktree's later state does not reach them (`tests/backup_archive_test.cpp` shows modified at 17:35 UTC, by the session that launched this run). No engine file was edited by this run |
| Binary | Release build of `git archive a8110b05` (`-DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF -DKDS_BUILD_BENCH=OFF`), mtime 2026-10-12 15:49:46 UTC, after the commit (15:39:32 UTC). Measured copy `bin/kds_server-B` sha256 `af25c71f63ca09ea60f6a66db89ca2d5664d20c5865cf18108b655fca6696757`, re-hashed at 17:20 UTC: unchanged. Every server, `--restore-from` and `--verify-backup` started from the copy. Section 6 also uses `bin/kds_server-A` (`09c82ae3…`, `fb953f07`) |
| Device | everything under `/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/work/`, `/dev/root` = `/dev/nvme0n1p1`, ext4, `rw,relatime,discard,errors=remount-ro,commit=30`; not tmpfs. **Sustained write 169 MB/s (`dd` 1 GiB `conv=fsync`) and 173 MB/s (`oflag=direct`)** at 16:26 UTC. The one exception, named where it occurs: the archive directory on `/dev/shm` in the diagnostic of section 2 |
| Host | 8 logical CPUs, AMD EPYC 9V74 (4 physical x 2 SMT; siblings 0/1, 2/3, 4/5, 6/7), 15 GiB, Linux 7.0.0-1014-azure |
| Pinning | server `taskset -c 0,2,4,6`, `reactor_cpus = 4` (`cores = 1`) or `4,2,6,0` (`cores = 4`); the M1b control keeps the server's other threads off the reactor's CPU (`taskset -c 0,2,6`); driver `taskset -c 7`; M2's six writers on CPUs 1,3,5,7 |
| Server config | `durability = relaxed`; `buffer_pool_frames = 65536` (M1a) or `262144` (2 GiB: M1b, M2, restores); `checkpoint_interval_ms = 5000`; `log_level = warn`; `archive_dir` set in the "archive on" cells and M2, unset otherwise; segments are the default 64 MiB. Ports 31821, 31831, 31841, 31861, 31871-31873, chosen |
| Suite | not executed (no code changed). `tools/br_drill.py` was not run here |
| PostgreSQL floor | not measured: the shapes are backup, archive and restore |
| Drivers | `archive/br-close-v2.7.0-913-ga8110b05/drivers/`: `br_cost.py` (M1a, M1b), `br_m2.py`, `br_restore.py`, `analyze_*.py`, `dbg_ckpt_ring.py`, `dbg_dlag.py`; the stage's own tools `tools/kds_backup.py` shapes (`BACKUP START / COPY TO / STOP TO / STATUS`) are driven directly. `bench/docs/` is closed |
| Series baseline | none: no earlier kdbs result prices archive, backup or restore (they did not exist). **This file is the baseline** the next one is read against |

## 2. M1a: the archive when on, nothing failing

Five rounds per core count, each round two fresh servers one after the other (archive off, archive on; the order alternates per
round), each running the same fixed work: 3,000 `INSERT`s, 3,000 `SELECT`s by pk and 3,000 `UPDATE`s by pk on a 10,000-row
relation, then 2,400 wide `INSERT`s of 50 rows (`id int64, v int64, payload varchar(1900)`, 1.8 KB a row, about 90 KB a
statement), which write about 0.9 GiB of log, 14 segments. Equal work, not equal time. Cells are medians with [min-max] over
the five rounds. The load on the host before each round was 1.7-3.4.

| cores | shape | archive | n | statements/s | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| 1 | INSERT hot (10,000 rows) | off | 5 rounds | 23189 [23111 to 23331] /s | 41.4 [41.2 to 41.5] µs | 61.4 [60.9 to 64.0] µs | 0.1 [0.1 to 0.1] ms | 0 [0 to 0] ms |
| 1 | INSERT hot (10,000 rows) | on | 5 rounds | 23194 [22988 to 23497] /s | 41.3 [40.7 to 41.6] µs | 62.9 [61.8 to 65.3] µs | 0.1 [0.1 to 0.2] ms | 0 [0 to 0] ms |
| 1 | INSERT hot (10,000 rows) | on vs off, paired | 5 rounds | 0.0 [-1.5 to 1.5] % | | | | |
| 1 | SELECT by pk | off | 5 rounds | 17173 [16686 to 17544] /s | 55.9 [55.2 to 56.3] µs | 77.9 [73.2 to 84.1] µs | 0.1 [0.1 to 0.2] ms | 0 [0 to 1] ms |
| 1 | SELECT by pk | on | 5 rounds | 17334 [17188 to 17686] /s | 55.6 [55.0 to 55.9] µs | 78.0 [73.3 to 84.3] µs | 0.1 [0.1 to 0.1] ms | 0 [0 to 0] ms |
| 1 | SELECT by pk | on vs off, paired | 5 rounds | 0.5 [-1.5 to 6.0] % | | | | |
| 1 | UPDATE by pk | off | 5 rounds | 21408 [19812 to 21806] /s | 43.5 [43.0 to 44.0] µs | 62.8 [61.0 to 68.9] µs | 0.1 [0.1 to 1.0] ms | 4 [3 to 17] ms |
| 1 | UPDATE by pk | on | 5 rounds | 20982 [18912 to 22081] /s | 43.1 [43.0 to 44.3] µs | 63.7 [58.6 to 84.1] µs | 0.1 [0.1 to 0.4] ms | 4 [3 to 7] ms |
| 1 | UPDATE by pk | on vs off, paired | 5 rounds | -2.0 [-11.7 to 10.0] % | | | | |
| 1 | wide INSERT, 50 rows x 1.8 KB | off | 5 rounds | 206 [205 to 207] /s | 1230.9 [1214.3 to 1234.5] µs | 5801.9 [5649.8 to 5969.7] µs | 586.3 [578.0 to 638.1] ms | 1077 [976 to 1140] ms |
| 1 | wide INSERT, 50 rows x 1.8 KB | on | 5 rounds | 147 [146 to 148] /s | 1224.1 [1183.0 to 1230.6] µs | 5385.9 [5309.4 to 5955.9] µs | 998.6 [976.5 to 1315.8] ms | 1379 [1331 to 1404] ms |
| 1 | wide INSERT, 50 rows x 1.8 KB | on vs off, paired | 5 rounds | -28.7 [-28.9 to -28.0] % | | | | |
| 4 | INSERT hot (10,000 rows) | off | 5 rounds | 22633 [22547 to 22805] /s | 42.3 [42.0 to 42.4] µs | 64.3 [62.4 to 65.0] µs | 0.1 [0.1 to 0.1] ms | 0 [0 to 1] ms |
| 4 | INSERT hot (10,000 rows) | on | 5 rounds | 22524 [22362 to 22565] /s | 42.3 [42.0 to 42.6] µs | 65.6 [63.9 to 67.0] µs | 0.1 [0.1 to 0.1] ms | 0 [0 to 0] ms |
| 4 | INSERT hot (10,000 rows) | on vs off, paired | 5 rounds | -0.9 [-1.4 to -0.1] % | | | | |
| 4 | SELECT by pk | off | 5 rounds | 16897 [16673 to 16902] /s | 57.0 [56.7 to 57.0] µs | 80.4 [79.0 to 85.3] µs | 0.1 [0.1 to 0.2] ms | 0 [0 to 0] ms |
| 4 | SELECT by pk | on | 5 rounds | 16728 [16533 to 16860] /s | 57.2 [56.6 to 57.6] µs | 83.4 [80.3 to 84.4] µs | 0.1 [0.1 to 0.1] ms | 0 [0 to 0] ms |
| 4 | SELECT by pk | on vs off, paired | 5 rounds | -0.9 [-2.2 to 1.1] % | | | | |
| 4 | UPDATE by pk | off | 5 rounds | 20918 [20298 to 20947] /s | 44.7 [44.4 to 45.0] µs | 67.5 [66.5 to 73.2] µs | 0.1 [0.1 to 0.1] ms | 4 [3 to 8] ms |
| 4 | UPDATE by pk | on | 5 rounds | 20578 [20311 to 20804] /s | 45.0 [44.3 to 45.1] µs | 69.7 [68.7 to 70.2] µs | 0.1 [0.1 to 0.1] ms | 4 [4 to 6] ms |
| 4 | UPDATE by pk | on vs off, paired | 5 rounds | -0.5 [-1.7 to 0.1] % | | | | |
| 4 | wide INSERT, 50 rows x 1.8 KB | off | 5 rounds | 206 [205 to 207] /s | 1262.1 [1253.1 to 1270.9] µs | 6079.4 [5938.2 to 6123.2] µs | 572.5 [558.9 to 579.9] ms | 1059 [805 to 1083] ms |
| 4 | wide INSERT, 50 rows x 1.8 KB | on | 5 rounds | 151 [147 to 152] /s | 1260.0 [1252.1 to 1287.0] µs | 5838.6 [4991.5 to 6059.3] µs | 1189.9 [973.0 to 1320.2] ms | 1323 [1125 to 1335] ms |
| 4 | wide INSERT, 50 rows x 1.8 KB | on vs off, paired | 5 rounds | -26.7 [-28.5 to -26.3] % | | | | |

The log and the archive's counters in the wide phase (median [min-max] over the five rounds):

| cores | archive | WAL written in the phase | WAL rate | segments archived by the end | wal_archive_lag_segments at the end of the phase | failures |
|---|---|---|---|---|---|---|
| 1 | off | 894 [892 to 896] MiB | 76.6 [76.4 to 77.1] MiB/s | 0 [0 to 0] | 0 [0 to 0] | 0 |
| 1 | on | 895 [892 to 901] MiB | 55.0 [54.6 to 55.1] MiB/s | 14 [14 to 14] | 2 [2 to 2] | 0 |
| 4 | off | 892 [892 to 898] MiB | 76.8 [76.3 to 77.1] MiB/s | 0 [0 to 0] | 0 [0 to 0] | 0 |
| 4 | on | 896 [891 to 901] MiB | 56.3 [54.8 to 57.2] MiB/s | 14 [14 to 14] | 3 [2 to 3] | 0 |


**The small statements do not see the archive.** Their p50 is unchanged to 0.3 µs, their p99 moves by -0.1 to +3.0 µs inside
the round-to-round range, and the paired throughput differences (median [min-max] over the five rounds) all span zero except
`INSERT` at `cores = 4` (`-0.9` % [`-1.4`, `-0.1`], 0.9 points). Each small arm lasts 0.1-0.2 s against a 64 MiB segment, so the
archiver has almost nothing to copy inside one; the cost is paid, if anywhere, in the wide phase.

**The wide `INSERT` pays `-28.7` % at `cores = 1` and `-26.7` % at `cores = 4`, with a spread of under a point between rounds.**
Its p50 is unchanged (1.22-1.23 ms), so the loss is not per statement but in the long stalls: p99.9 586 ms off, 999 ms on at
`cores = 1`; 573 ms off, 1,190 ms on at `cores = 4`. The log is written at 76.6 MiB/s with the archive off and 55.0 MiB/s with it
on, which is the same ratio as the statement rate (0.72 and 0.71), so the archive is not making each byte of log dearer; it is
taking time the log would have used.

**What bounds it is the device, not the engine.** One block device writes 165-173 MB/s fsynced. With the archive off the log
runs at 77 MiB/s with the checkpoint's page writes beside it; with the archive on, every segment is written a second time
(64 MiB, then `fsync` of the file and of the directory). The diagnostic below moves only the archive's target to tmpfs
(`/dev/shm`, so the archive's bytes no longer share the data device; this is a diagnostic of what bounds the cost and is not a
measurement of archiving, which on tmpfs would be fiction):

| archive target | rounds | wide `INSERT` | p50 | p99 | log rate |
|---|---|---|---|---|---|
| off | 3 | 206.9 [206.5-207.4] /s | 1,219.8 [1,174.9-1,224.5] µs | 5,957.6 [5,946.1-5,979.2] µs | 77.0 [76.9-77.2] MiB/s |
| `/dev/shm` | 3 | 200.3 [199.8-205.7] /s | 1,234.4 [1,220.6-1,240.1] µs | 5,901.1 [5,899.2-5,939.2] µs | 75.0 [74.6-76.9] MiB/s |
| data device | 3 | 147.8 [147.8-150.3] /s | 1,217.3 [1,180.5-1,219.6] µs | 5,599.1 [5,575.1-5,722.3] µs | 54.9 [54.9-56.1] MiB/s |

With the copy off the data device the loss falls from 28.6 % to 3.2 %: **what the engine itself spends on archiving (the archiver thread's CPU, the detached
segments it holds, its extra `fsync`s) is about 3 %; the rest is bandwidth the log and the archive fight for.** The first is
consistent with BR-R9 ("the archiver never runs under the stream latch", so a seal adds nothing to the hold) and is not a
measurement of the latch; the second is a deployment fact: `archive_dir`
belongs on a different device from `wal_dir`, and the manual should say so.

## 3. M1b: a base backup running

One server per core count (`durability = relaxed`, pool 2 GiB). A 10,000-row `hot` relation and a `ballast` relation of 540,000
wide rows (`id`, `v`, `payload varchar(1900)`), 1.06 GiB of pages, loaded in 54-55 s. The workload is one serial connection
cycling four statements, so each kind is a quarter of the mix: `INSERT` into `hot`, `UPDATE` of `hot` by pk, `UPDATE` of `ballast`
by a pk drawn uniformly over the whole ballast (the worst case for page touches), `SELECT` of `hot` by pk. Windows
alternate: a baseline window, then `BACKUP START`, `BACKUP COPY TO` (the workload runs until `BACKUP STATUS` reports `copying=0`),
then `BACKUP STOP TO` (the workload runs until the finish is done). Each baseline window is as long as the previous copy.
Five backups per cell; the last is kept for section 5. All five of each cell ended `outcome=done`; the host's load during the
three cells was 1.5-3.4 with no foreign process at 20 % of a CPU (the host record at the end).

**Throughput in these windows is dominated by multi-hundred-millisecond stalls that have nothing to do with the backup.** 11-87
% of a window, baseline included (median about half), is spent inside statements slower than 1 ms (the longest statements are 1-4.5 s). They are
the engine's checkpoint and log-sync stalls on a device that writes 165 MB/s while the workload dirties random ballast pages. A
raw statements-per-second figure therefore ranges 2,300-16,200 /s between identical baseline windows. **The service rate
excluding statements over 1 ms** (16,000-18,700 /s) is stable to about +-4 % and is what the table compares.


#### `cores = 1`, server on CPUs 0,2,4,6, reactor on 4: ballast 540,000 rows, loaded in 54.8 s; 5 backups

| window | n | duration (s) | statements/s, raw | statements/s excluding statements over 1 ms | share of the window inside statements over 1 ms | p50 | p99 | p99.9 | WAL bytes per statement |
|---|---|---|---|---|---|---|---|---|---|
| baseline (no backup) | 5 windows | 4.7 [4.2 to 7.0] s | 6946 [2323 to 8185] /s | 18271 [17447 to 18737] /s | 63 [55 to 87] % | 48.4 [48.3 to 48.5] µs | 83.4 [77.5 to 284.3] µs | 319.1 [178.9 to 352.4] µs | 1202 [1192 to 1212] B |
| BACKUP COPY running | 5 windows | 4.2 [4.2 to 4.8] s | 10925 [6117 to 14846] /s | 16801 [16318 to 16904] /s | 35 [12 to 63] % | 49.3 [49.1 to 49.4] µs | 334.1 [304.7 to 350.5] µs | 414.3 [406.1 to 530.5] µs | 1207 [1206 to 1212] B |
| BACKUP STOP finishing (data sync, log copy, manifest) | 5 windows | 10.1 [9.7 to 11.0] s | 5304 [5176 to 5962] /s | 18018 [17255 to 18250] /s | 70 [66 to 72] % | 48.6 [48.4 to 48.7] µs | 89.7 [84.3 to 192.5] µs | 379.0 [325.3 to 542.0] µs | 1842 [1277 to 2289] B |

5 backups, each copy window minus the baseline window just before it: p50 0.9 [0.8 to 1.0] µs; p99 240.2 [57.0 to 272.7] µs; service rate excluding statements over 1 ms -1470 [-2382 to -809] /s, -8.0 [-12.7 to -4.6] %.

| statement kind | baseline p50 | copy p50 | baseline p99 | copy p99 |
|---|---|---|---|---|
| ins-hot | 46.6 [46.5 to 46.8] µs | 47.2 [47.1 to 47.3] µs | 73.2 [69.7 to 268.7] µs | 330.3 [295.8 to 357.1] µs |
| upd-hot | 46.8 [46.6 to 46.9] µs | 47.4 [47.3 to 47.6] µs | 72.1 [66.8 to 285.1] µs | 292.1 [277.2 to 330.1] µs |
| upd-ballast | 48.5 [48.4 to 48.7] µs | 49.2 [49.1 to 49.4] µs | 74.4 [69.1 to 280.8] µs | 326.2 [290.7 to 341.0] µs |
| sel-hot | 60.9 [60.7 to 61.2] µs | 61.9 [61.7 to 62.9] µs | 87.9 [83.8 to 295.2] µs | 352.7 [318.8 to 369.1] µs |

The copy: 4.25 [4.21 to 4.82] s at 249.3 [222.8 to 253.6] MiB/s over 137152 [135552 to 137600] pages. STOP's finish: 10.25 [9.74 to 11.04] s. Writes that waited for the fence, in total at the end: 0 writes. Log copied per backup: 256, 320, 256, 320, 256 MiB. Backup size: 1067, 1070, 1072, 1074, 1080 MiB. Log span of the window (stop LSN - start LSN): 172, 255, 179, 306, 199 MiB.

#### `cores = 1`, the server's other threads kept off the reactor's CPU (mask 0,2,6, reactor on 4): ballast 540,000 rows, loaded in 54.8 s; 5 backups

| window | n | duration (s) | statements/s, raw | statements/s excluding statements over 1 ms | share of the window inside statements over 1 ms | p50 | p99 | p99.9 | WAL bytes per statement |
|---|---|---|---|---|---|---|---|---|---|
| baseline (no backup) | 5 windows | 4.8 [4.2 to 6.0] s | 9768 [6230 to 16213] /s | 18231 [17916 to 18461] /s | 46 [11 to 65] % | 49.0 [48.8 to 49.3] µs | 83.0 [80.6 to 103.4] µs | 302.9 [113.6 to 324.3] µs | 1207 [1205 to 1209] B |
| BACKUP COPY running | 5 windows | 4.2 [4.2 to 5.0] s | 7021 [4195 to 11441] /s | 15973 [15646 to 16695] /s | 56 [31 to 73] % | 49.8 [49.7 to 50.2] µs | 332.4 [316.1 to 348.9] µs | 568.6 [391.2 to 652.9] µs | 1205 [1176 to 1214] B |
| BACKUP STOP finishing (data sync, log copy, manifest) | 5 windows | 10.4 [9.3 to 11.3] s | 4506 [4108 to 5305] /s | 17871 [17075 to 18007] /s | 74 [70 to 77] % | 49.0 [48.9 to 49.2] µs | 86.0 [82.7 to 190.6] µs | 329.9 [314.8 to 971.6] µs | 1532 [1381 to 2731] B |

5 backups, each copy window minus the baseline window just before it: p50 0.9 [0.8 to 0.9] µs; p99 251.0 [220.1 to 268.3] µs; service rate excluding statements over 1 ms -2420 [-2585 to -1222] /s, -13.2 [-14.2 to -6.8] %.

| statement kind | baseline p50 | copy p50 | baseline p99 | copy p99 |
|---|---|---|---|---|
| ins-hot | 47.4 [47.2 to 47.6] µs | 47.9 [47.9 to 48.2] µs | 72.8 [71.0 to 108.4] µs | 329.1 [310.7 to 360.8] µs |
| upd-hot | 47.6 [47.2 to 47.7] µs | 48.1 [47.8 to 48.2] µs | 72.7 [70.2 to 86.5] µs | 326.6 [306.4 to 349.4] µs |
| upd-ballast | 49.1 [48.9 to 49.4] µs | 49.7 [49.6 to 50.1] µs | 74.7 [71.6 to 91.3] µs | 320.2 [318.2 to 345.8] µs |
| sel-hot | 62.2 [61.4 to 62.4] µs | 62.8 [62.3 to 63.6] µs | 91.0 [88.6 to 130.1] µs | 343.1 [325.8 to 358.7] µs |

The copy: 4.26 [4.19 to 4.98] s at 252.8 [215.4 to 254.6] MiB/s over 137280 [135552 to 138112] pages. STOP's finish: 10.40 [9.44 to 11.35] s. Writes that waited for the fence, in total at the end: 0 writes. Log copied per backup: 256, 256, 192, 192, 384 MiB. Backup size: 1059, 1061, 1072, 1074, 1081 MiB. Log span of the window (stop LSN - start LSN): 161, 231, 113, 181, 279 MiB.

#### `cores = 4`, server on CPUs 0,2,4,6, reactors on 4,2,6,0: ballast 540,000 rows, loaded in 54.3 s; 5 backups

| window | n | duration (s) | statements/s, raw | statements/s excluding statements over 1 ms | share of the window inside statements over 1 ms | p50 | p99 | p99.9 | WAL bytes per statement |
|---|---|---|---|---|---|---|---|---|---|
| baseline (no backup) | 5 windows | 7.1 [5.3 to 8.9] s | 7943 [6327 to 13932] /s | 16405 [16053 to 17285] /s | 53 [16 to 61] % | 50.9 [50.0 to 51.1] µs | 146.9 [128.4 to 172.9] µs | 215.2 [182.9 to 284.2] µs | 1211 [1208 to 1212] B |
| BACKUP COPY running | 5 windows | 4.4 [4.2 to 5.3] s | 8758 [1972 to 10068] /s | 15624 [15431 to 16620] /s | 45 [35 to 87] % | 54.4 [51.4 to 54.6] µs | 145.7 [132.3 to 170.6] µs | 235.0 [191.6 to 418.2] µs | 1210 [1206 to 1229] B |
| BACKUP STOP finishing (data sync, log copy, manifest) | 5 windows | 10.8 [10.5 to 11.3] s | 3623 [2651 to 4537] /s | 8091 [6384 to 11870] /s | 61 [55 to 62] % | 52.7 [51.6 to 54.3] µs | 185.9 [173.0 to 192.6] µs | 415.4 [259.7 to 446.2] µs | 2333 [2036 to 3377] B |

5 backups, each copy window minus the baseline window just before it: p50 3.5 [1.4 to 3.8] µs; p99 -1.2 [-11.9 to 23.9] µs; service rate excluding statements over 1 ms -665 [-898 to -606] /s, -3.9 [-5.4 to -3.7] %.

| statement kind | baseline p50 | copy p50 | baseline p99 | copy p99 |
|---|---|---|---|---|
| ins-hot | 48.7 [48.6 to 48.8] µs | 49.7 [49.0 to 50.0] µs | 76.5 [73.0 to 90.5] µs | 82.8 [77.8 to 110.2] µs |
| upd-hot | 48.5 [48.3 to 48.6] µs | 49.5 [48.8 to 49.8] µs | 74.6 [71.3 to 89.5] µs | 78.9 [77.7 to 116.6] µs |
| upd-ballast | 51.2 [49.9 to 51.3] µs | 54.5 [50.9 to 55.0] µs | 172.4 [142.5 to 195.9] µs | 163.8 [148.6 to 198.3] µs |
| sel-hot | 64.3 [63.7 to 64.3] µs | 66.0 [64.8 to 67.0] µs | 95.0 [90.6 to 108.3] µs | 104.7 [102.1 to 130.5] µs |

The copy: 4.41 [4.24 to 5.35] s at 249.4 [204.0 to 253.8] MiB/s over 139968 [137856 to 140608] pages. STOP's finish: 10.94 [10.52 to 11.45] s. Writes that waited for the fence, in total at the end: 0 writes. Log copied per backup: 256, 448, 384, 384, 384 MiB. Backup size: 1082, 1092, 1093, 1097, 1099 MiB. Log span of the window (stop LSN - start LSN): 166, 382, 309, 321, 329 MiB.

**What the copy costs, by cell.**

- **Copy speed.** 1.06 GiB in 4.2-5.3 s: median 249 MiB/s against the 256 MiB/s throttle, nine of 15 backups at 249-255 MiB/s
  (the other six at 204-237). The throttle binds. It can bind because the copy's writes land in the page cache and nothing fsyncs them
  until `STOP`; the device's 165 MiB/s sustained rate is not in the copy's path. `BACKUP STOP`'s finish (data file sync, the
  window's log copied, the manifest) took 9.4-11.5 s in every cell and is the device-bound part of a backup: 1.06 GiB plus
  190-450 MiB of log at 165 MB/s.
- **Latency.** The p50 of every statement kind rises by 0.5-1.0 µs at `cores = 1` (0.9 µs paired) and 1-3.3 µs at `cores = 4`
  (3.5 µs paired). **The p99 at `cores = 1` rises from 83 [77-284] µs to 334 [305-351] µs, a factor of four, in every one of the
  five backups, and the control moves nothing: with the server's other threads kept off the reactor's CPU (second table)
  it is 83 [81-103] to 332 [316-349] µs.** At `cores = 4` the p99 does not move: 147 [128-173] to 146 [132-171] µs. The p99.9
  is 319 to 414 µs at `cores = 1` and 215 to 235 µs at `cores = 4`.
- **Service rate** (median [min-max] over the five backups, each copy paired with the baseline just before it): `-8.0` %
  [`-12.7`, `-4.6`] at `cores = 1`, `-13.2` % [`-14.2`, `-6.8`] with the copier off the reactor's CPU, `-3.9` % [`-5.4`, `-3.7`] at
  `cores = 4`. The baseline windows themselves span 17,400-18,700 /s (`cores = 1`) and 16,050-17,300 /s (`cores = 4`), +-3.6 %, so
  the `cores = 4` figure is at the floor.
- **The fence.** `backup_fence_writes_waited` is 0 at the end of every backup in every cell: no write ever waited for the
  copier's 1 MiB read. The copier's cost to the workload is therefore not the fence's wait.
- **Write amplification.** The WAL per statement is 1,200-1,210 B in baseline and copy windows alike (the ballast `UPDATE`
  logs a 1.9 KB tuple); there is no full-page-image volume to price, because BR-R5's window images were refuted by the census and
  not built. The only extra log a backup writes is its own window copy: 192-448 MiB per backup.

**The cause of the `cores = 1` tail is not established.** A chunk is 128 pages (1 MiB) every 4 ms at 256 MiB/s; a statement
delayed by 250 µs once per chunk lands in about 1.2 % of statements, which is p99. That fits a delay of roughly the chunk's own
read-and-write time. It is not the copier sharing the reactor's CPU (the control), not the fence (zero waits), and it is absent
at `cores = 4` on the same CPUs. A sampler run of the reactor during a copy (`tools/kds_sampler.c`) would name it; it was not
run. The lever the data point to is the chunk size, not the rate.

### Are the stalls BR's? No: A has the same

Roughly half of every window in this workload, backup or not, sits inside statements over 1 ms, with single statements of 2-4
s. To see whether that is BR's (it changed the log device's segment reclaim and the stream's append path), the same workload
(`cores = 1`, the 540,000-row ballast, no backup, one 40 s window after 9 s of warm-up) ran on A and on B, in the order A, B, B, A,
each on a fresh server and data file. A has no `BACKUP`, so its windows are the baseline only.

| run (order) | binary | window | statements | statements/s, raw | statements/s excluding statements over 1 ms | share of the window inside statements over 1 ms | p50 | p99 | p99.9 | longest statement |
|---|---|---|---|---|---|---|---|---|---|---|
| A1 (1 of 4) | A `fb953f07` | 40.0 s | 372,597 statements | 9,315 /s | 18,106 /s | 49 % | 48.5 µs | 88.8 µs | 325.9 µs | 3,007 ms |
| B1 (2 of 4) | B `a8110b05` | 40.0 s | 384,295 statements | 9,607 /s | 18,135 /s | 47 % | 48.9 µs | 87.2 µs | 312.0 µs | 4,134 ms |
| B2 (3 of 4) | B `a8110b05` | 40.0 s | 371,521 statements | 9,288 /s | 18,004 /s | 48 % | 49.2 µs | 86.6 µs | 336.4 µs | 2,476 ms |
| A2 (4 of 4) | A `fb953f07` | 40.0 s | 364,723 statements | 9,118 /s | 17,920 /s | 49 % | 49.0 µs | 90.9 µs | 327.8 µs | 3,102 ms |

The share of a window inside statements over 1 ms is 49 % and 49 % on A, 47 % and 48 % on B; the service rate excluding them is
17,920-18,135 /s on both; the longest statement is 3.0-3.1 s on A and 2.5-4.1 s on B. **The stalls are in A and in B alike and
are not BR's.** They are the engine's, on a device that writes 165 MB/s, under a workload that dirties random pages of a
1 GiB relation: a candidate for its own work order, since half the wall time of a `relaxed` instance is spent inside
multi-second statements while its checkpoint falls behind. This run did not look inside them. For this file they set the
comparison: M1b's copy windows are read against baseline windows that stall as often.

## 4. M2: archive throughput against the log's peak generation

One server, `cores = 4`, `durability = relaxed`, `archive_dir` set, the same 540,000-row ballast loaded first, then an idle
base backup (section 5), then six client processes, each its own connection, `INSERT`ing 50 wide rows (about 90 KB) per
statement with the pk omitted, for 105 s: 45 s steady, then the archive directory made unwritable for 20 s (`chmod`; the
archiver fails, backs off, and holds the segments), then writable again for 40 s more. A sampler read `SHOW META` every 0.25 s
on its own connection (283 samples over 130 s, the last 25 s with the load stopped). The load's latency was p50 2.6 ms,
p95 255 ms, p99 1.44 s, longest 4.25 s over 11,132 statements, 0 errors.

| phase | span | duration | WAL generation (durable LSN) | archive rate (segments archived x 64 MiB) | max wal_archive_lag_segments | archive failures in the phase |
|---|---|---|---|---|---|---|
| steady (archive writable) | 0-45 s | 44.8 s | 54.3 MiB/s | 48.6 MiB/s | 5 segments | 0 |
| archive directory unwritable | 45-65 s | 20.2 s | 76.0 MiB/s | 0.0 MiB/s | 27 segments | 5 |
| archive writable again, load running | 65-105 s | 40.2 s | 49.3 MiB/s | 46.2 MiB/s | 39 segments | 0 |
| load stopped (tail) | 105-130 s | 24.8 s | 0.1 MiB/s | 77.6 MiB/s | 29 segments | 0 |
| whole load | 0-105 s | 105.2 s | 56.6 MiB/s | 38.3 MiB/s | 39 segments | 5 |

Peak over any 5 s window 106.7 MiB/s (from 30.8 s), median of the 5 s windows 64.7, lowest 19.1; 176 windows. Max `wal_archive_lag_segments`: 5 in the steady phase, 39 after the outage (at 76.3 s).

- **Generation.** 56.6 MiB/s over the whole load (54.3 steady); the highest any 5 s window reached is 106.7 MiB/s and the lowest
  19.1. (`wal_durable_lsn` moves in jumps under `relaxed`, a 1 s window reads up to 768 MiB/s; 5 s is the honest window.) The
  clients are not the bound: six writers with the archive on got 54 MiB/s, what one writer got with it on in M1a (55 MiB/s).
- **The archiver keeps up on average and not in the peaks.** Steady-state it archived 48.6 MiB/s against a generation of 54.3,
  the lag climbing to 5 segments (50 of the 176 five-second windows exceed 80 MiB/s, three exceed 100). Its own rate with the
  load stopped is about 170 MiB/s (the last 12 segments in 4.5 s, one every 0.25-0.5 s), the device's, since the copy is a read, a
  write and an `fsync`. With the load running it drained a backlog of 39 segments to 1 in 44.8 s: **58 segments, 82.8 MiB/s, while
  the log grew 27.2 MiB/s, a net 55.6 MiB/s**.
- **Margin.** Against the mean generation the archiver's loaded rate is 1.5x (82.8 / 56.6). Against the peak 5 s window it is
  0.78x: a burst at 107 MiB/s outruns it, and the lag is the integral of the excess. `wal_archive_lag_segments` peaked at 5 in
  the steady phase and at 39 after the outage.
- **The outage.** Five failures were counted by 62 s (`wal_archive_failures`: 2 by 47 s, 3 by 51 s, 4 by 56 s, 5 by 62 s, the backoff
  doubling from 1 s); 27 segments (1.7 GiB) were held at the moment the directory was repaired, and the first segment was archived **12.1
  s after the repair**, because the backoff had reached 16 s. The lag peaked at 39 segments (2.4 GiB of log held un-recycled in
  `wal_dir`) 76 s into the load. The recycler held every one: `wal_segments_removed` equals `wal_archived` (157) at the end, so
  nothing was removed that was not archived. `wal_archive_blocking` stayed 0 throughout (section 7).

## 5. M3: restore time

`kds_server --config <conf> --restore-from <backup> --restore-only` into a fresh directory, timed from the process start. The
restore prints two lines: the layout is done (`backup <id> laid out ... recovering`), and the mount recovered past the stop. The
wall time is split at them: **layout** (the backup's files verified and copied into place) and **recovery** (the mount that
replays, up to the second line; the exit afterwards is not included). The first restore of each backup reads its files cold
(`posix_fadvise(DONTNEED)` on the backup, and on the archive when one is read); the others read them warm. A fourth run per
backup serves instead of exiting, so `SHOW META`'s recovery block names what that mount did. `--verify-backup` is timed alone.

#### `cores = 1`, the backup taken under load (M1b, backup 5 of 5)

Backup 18355600794583972943: path B, start LSN 4,966,062,064, stop LSN 5,174,875,200, a window of 199 MiB of log (segments 74..77), 1,080 MiB on disk; `--verify-backup` 1.432 s.

| restore | backup files | total wall time | layout (copy + verify) | mount, replay (to the second line) |
|---|---|---|---|---|
| 1 | cold | 18.9 s | 10.5 s | 8.4 s |
| 2 | warm | 17.1 s | 8.7 s | 8.4 s |
| 3 | warm | 17.2 s | 8.8 s | 8.4 s |

`SHOW META` of the serving run's mount: 540,798 records scanned, 99,841 redo applied, 121,678 committed; analysis 0.16 s, redo 0.37 s, undo 0.00 s, completion checkpoint 0.01 s (sum 0.54 s).

#### `cores = 4`, the backup taken under load (M1b, backup 5 of 5)

Backup 14296509397782360924: path B, start LSN 4,907,477,288, stop LSN 5,252,946,384, a window of 329 MiB of log (segments 73..78), 1,099 MiB on disk; `--verify-backup` 1.601 s.

| restore | backup files | total wall time | layout (copy + verify) | mount, replay (to the second line) |
|---|---|---|---|---|
| 1 | cold | 22.0 s | 11.3 s | 10.7 s |
| 2 | warm | 20.4 s | 9.8 s | 10.6 s |
| 3 | warm | 20.3 s | 9.7 s | 10.5 s |

`SHOW META` of the serving run's mount: 849,326 records scanned, 41,606 redo applied, 191,095 committed; analysis 0.24 s, redo 0.58 s, undo 0.00 s, completion checkpoint 0.01 s (sum 0.82 s).

#### `cores = 4`, the idle backup of M2 restored through the archive to `latest`

Backup 15350963357412918531: path B, start LSN 618,004,624, stop LSN 4,256,699,640, a window of 3,470 MiB of log (segments 9..63), 1,075 MiB on disk; `--verify-backup` 5.668 s.

| restore | backup files | total wall time | layout (copy + verify) | mount, replay (to the second line) |
|---|---|---|---|---|
| 1 | cold | 138.5 s | 93.7 s | 44.7 s |
| 2 | warm | 138.7 s | 93.4 s | 45.2 s |

`SHOW META` of the serving run's mount: 2,617,179 records scanned, 1,647,436 redo applied, 15,742 committed; analysis 12.06 s, redo 17.67 s, undo 1.58 s, completion checkpoint 13.46 s (sum 44.78 s).

- **Layout is the data file's copy.** 8.7-9.8 s warm and 10.5-11.3 s cold for backups of 1,080-1,099 MiB (data file plus log
  window), 113-124 MiB/s warm; the device writes 165 MB/s. `--verify-backup` of the same backup is 1.4-1.6 s (the CRC pass over
  warm files).
- **Recovery is mostly not replay.** The mount's analysis, redo, undo and completion checkpoint sum to 0.54 s (window 199 MiB,
  `cores = 1`) and 0.82 s (329 MiB, `cores = 4`) inside a mount of 8.4 s and 10.5 s; the rest of the mount (about 7.8 and 9.7 s)
  is a fixed per-volume cost the counters do not name. **Replay, analysis plus redo, runs at 376 MiB/s of log at `cores = 1` and 405
  MiB/s at `cores = 4`** (199 MiB in 0.53 s; 329 MiB in 0.81 s), and at 318 MiB/s over the 9.2 GiB of the PITR run (12.1 s analysis
  plus 17.7 s redo for the 9.2 GiB between the redo start and the log's end).
- **A restore of the idle backup of this server does not open.** The idle backup (section 4) has a 3,470 MiB window (start LSN
  618,004,624, stop 4,256,699,640). Its restore ran 3 times (layout 38.1-38.4 s, mount 10.8-11.3 s) and each exited with status 1
  after 49 s; a debug rerun printed `startup failed: recovery of core 0: completion checkpoint: WalStream: record of 1408648
  bytes is larger than the ring`. Restored through the archive to `latest` the same backup opens (the last table above). **No wrong open occurred: the
  refusal is loud (BR-R2 holds).** Section 6 says why it happens and whose it is.

## 6. The window that cannot be restored, and why it is not BR's

A mount's completion checkpoint writes a `CHECKPOINT_BEGIN` whose payload carries the dirty-page table (`wal.md` §8: the active-transaction table and the dirty-page table with recovery LSNs). The log's
ring is 1 MiB (`kDefaultRingCapacity`), so a dirty-page table of more than the order of 100,000 pages is a record "larger than the ring" (the two refusals below are 1.38 and 1.41 MB). Redo of
a long window dirties every page the window touched: of the order of 115,000 pages for this ballast. Whether it is that long depends on how far
behind the log's end the anchor's redo start (`D`, the backup's start LSN) sits when the backup begins:

| after a 540,000-row ballast load | log end - `D` at the end of the load | `D` caught up |
|---|---|---|
| `cores = 4`, archive on | 4,032 MiB (`D` = 4,480, the log's first bytes) | 3,419 MiB at +10 s, 347 MiB at +16 s; not yet at +20 s |
| `cores = 4`, archive off | 4,032 MiB | 3,227 MiB at +14 s, 0 at +20 s |
| `cores = 1`, archive on | 2,048 MiB | 987 MiB at +8 s, 0 at +14 s |

(`dbg_dlag.py`, three servers loading at once, so the load took 240 s each instead of 55; the lags are in MiB, the first
column read at the end of the load.) A backup begun inside that interval inherits a 2-4 GiB window. The M1b backups were taken 15 s or more
after their loads' ends and had windows of 113-382 MiB; the M2 idle backup was taken 5 s after a 78 s load at `cores = 4` and
had 3,470 MiB.

**It reproduces on the commit BR opened from.** `dbg_ckpt_ring.py` on the binary A (`fb953f07`, no backup code), `cores = 4`,
the same 540,000-row ballast loaded alone, then `kill -9` at once and restart of the same files:

```
killed -9; restarting the same files
mount FAILED after 3.0 s: A: server exited 1: startup failed: recovery of core 0: completion checkpoint:
    WalStream: record of 1381352 bytes is larger than the ring
```

At `cores = 1`, A's checkpoint had caught up by the end of the same load (9 runs, 8,266 pages dirty at the last) and the same
kill recovered in 3.4 s (290,239 records). So **an instance at `fb953f07` that crashes at `cores = 4` within seconds of a bulk
load of this size cannot mount again**, and BR-R1 ("restore is mount recovery") makes BR's restore inherit that exactly. The
defect is the engine's and predates BR; BR is what exposes it to a planned operation. Two consequences for the order, stated
as findings and not decided here:

- BR-R3 says a forced checkpoint at `backup_start` "is not needed for correctness; it only shortens replay" (BR-Q3). For a
  window that dirties more pages than the ring holds records, the checkpoint is what makes the backup restorable; BR-Q3's
  answer should be recorded with this measurement beside it.
- A restore that goes through the archive past the stop opened here (section 5), probably because replaying 9.2 GiB filled the
  pool (262,144 frames) and the mount wrote pages back, shrinking the table below the ring; that is an inference and was not
  confirmed.

## 7. Constants

| constant (file) | value | what the numbers say | status |
|---|---|---|---|
| `BackupControl::kCopyBytesPerSecond` (`backup.hpp`) | 256 MiB/s | The copy runs at 249 MiB/s median (204-255), so the throttle binds on a device that writes 165-173 MB/s, because the copy's writes sit in the page cache until `STOP`. At that rate 1.06 GiB takes 4.2-5.3 s; the workload loses 4-13 % of its service rate and, at `cores = 1`, four times its p99 for those seconds. The data cannot say a different rate is better: one rate was run, and changing it is an engine edit. | stays `[provisional]`: measured at the one value |
| `BackupControl::kCopyChunkPages` (`backup.hpp`) | 128 pages (1 MiB) | The `cores = 1` p99 rises by about 250 µs, once per chunk by arithmetic (section 3). If the chunk's own read-and-write time is the cause, a smaller chunk is the lever. Not tested. | stays `[provisional]`; the direction to try is smaller |
| `WalArchiver::kReserveSegments` (`wal_archive.hpp`) | 4 segments (256 MiB) | **Not exercised**: the gate engages only when the WAL filesystem has under 256 MiB free while archiving fails, and this host has 98 GiB free, with no way to confine a filesystem without root. By arithmetic, 256 MiB is 2.4 s of the peak 5 s generation (106.7 MiB/s) and 4.5 s of the mean (56.6); the outage left 27 segments held after 20 s and 39 after the 12 s of backoff that followed the repair; whether 4 segments is the right reserve depends on how fast free space falls once archiving fails, which is the generation rate (106.7 MiB/s at the peak). `wal_archive_blocking` was 0 in every cell. | stays `[provisional]`: unmeasured |
| `kAbandonNs` (`backup.hpp`) | 1 h | The longest complete path B backup in this run took 38 s from `BACKUP START` to the manifest (the 3,470 MiB-window idle backup: copy 4.2 s, finish 33.9 s); the ones under load 14-17 s. 1 h is 95-250x. It bounds a path D snapshot taken between `START` and `STOP` by an operator and an abandoned session; neither was run. | stays `[provisional]`: no measurement can fix a policy; the data say only that nothing a path B backup does comes near it |
| `kIdleWait` 1 s, `kMaxBackoff` 30 s (`wal_archive.cpp`; the order's retry backoff) | 1 s, doubling to 30 s | After a 20 s outage the archiver resumed 12.1 s after the repair, with 39 segments held. At the 30 s cap the wait after a repair is up to 30 s, about 26 segments at the mean generation. | stays `[provisional]` |

## 8. Where the time goes

| unit | wait | share | how it is read |
|---|---|---|---|
| a base backup, path B, 1.06 GiB | the paced copy | 4.2-5.3 s (about 30 % of the 14-17 s) | `BACKUP COPY` to `copying=0` |
| | the finish: data file sync, log window copied, manifest | 9.4-11.5 s (about 70 %) | `BACKUP STOP` to `last_id` set |
| | write-statement, lock, client round trip | not applicable: the unit is a background operation, not a statement | |
| a workload statement during the copy | client and socket round trip | 28 of 48-62 µs p50, unchanged | M0's decomposition |
| | the copier's interference | +0.5 to +1.0 µs at p50, +250 µs at p99 (`cores = 1`) | paired against the baseline window |
| a restore, 1.1 GiB, 200-330 MiB window | layout (copy + verify) | 8.7-11.3 s (about half) | first stdout line |
| | the mount | 8.4-10.7 s (about half), of which analysis + redo + undo + checkpoint 0.5-0.8 s | second stdout line, `SHOW META` |
| | unattributed: the mount's time outside the recovery phases | 7.8-9.9 s | not measured: no sampler run |

## 9. What the run teaches

- **The archive's cost is the device's.** On the shared device a segment costs its bytes twice; the engine's own share is about
  3 %. Writing to a second device is the first thing a deployment should do, and a measurement on a one-device host prices the
  worst case.
- **A backup's expensive part is its end, not its copy.** The copy is about 30 % of a backup's time and is throttled to be quiet;
  the finish (the data sync and the window's log) is device-bound and is the same size whatever the throttle.
- **The throttle works and the chunk may not.** The rate holds the copy to 249 MiB/s and the workload to a 4-13 % lower service
  rate; at `cores = 1` a once-per-chunk delay of about 250 µs is visible at p99 and not at p50. That points to the chunk, and the
  chunk was not varied.
- **BR-R6's cost target, restated.** The overhead file prices the engine with the features off; this file prices them on. The
  difference between "the archive is free" and "the archive costs 29 % of a write-heavy workload" is entirely where its target
  directory lives.
- **A restore's window is set by a number nobody chose.** `D` can sit 4 GiB behind the log's end after a bulk load, and a
  window that long cannot be restored at the current ring size. The cadence's checkpoint, not BR, decides how far `D` lags; BR-Q3
  (a checkpoint at `backup_start`) has its first data point.

## 10. Not measured

- **Path D** (a volume snapshot): H9 left the LVM reference script unrun, and no snapshot device exists on this host.
- **The fail-closed gate engaging** (section 7, `kReserveSegments`): needs a nearly full WAL filesystem.
- **`archive_command` mode**: only `archive_dir`.
- **Archive on against the commit before BR**: A has no archive; the baseline is B with it off.
- **A different throttle or chunk size**: the constants are `static constexpr`; varying them is a code change.
- **Where the `cores = 1` tail and the mount's 8 s come from**: no CPU sampler was run (`tools/kds_sampler.c` exists).
- **`durability = strict` and `group`** in the backup and archive cells.
- **Equivalence of the restored instance** to the source: the drills and the simulation carry it; every restore here only
  proved it opened past its stop.

Raw numbers, drivers and logs: `bench/v3.0.0/archive/br-close-v2.7.0-913-ga8110b05/` (`out/m1a`, `out/m1a-diag`, `out/m1b`,
`out/m2`, `out/m3`, `out/stall`, `logs`, `drivers`).

## Host record

| samples file (cells it covers) | UTC window | samples | 1-min load, min-max | mean busy, all 8 CPUs | samples with a foreign process >= 20 % of a CPU | those processes |
|---|---|---|---|---|---|---|
| `host-samples-m1b.jsonl` (M1b cores 1 and cores 4) | 16:25:52-16:35:21 | 503 | 1.70-2.83 | 6 % | 0 | - |
| `host-samples-m1b-off.jsonl` (M1b cores 1, copier off the reactor CPU) | 16:35:53-16:38:41 | 167 | 1.52-3.43 | 6 % | 0 | - |
| `host-samples-m3.jsonl` (M3 restores) | 16:44:54-16:57:55 | 772 | 1.48-3.41 | 5 % | 0 | - |
| `host-samples-stall.jsonl` (stall census) | 17:15:42-17:29:22 | 793 | 2.17-4.06 | 8 % | 10 | /usr/bin/python3 /home/cdkbs/ckdbs/.claude/worktrees/bp-per-core-resid; python3 -; python3 -I mk3.py; python3 -I mk3c.py |

M1a and M2 were not sampled per second: their rounds recorded `/proc/loadavg` at the start and end (M1a 1.7-3.4, M2 1.61 to
2.88) and no compiler or test binary was live in any `pgrep` list. The stall census's sampler file covers both passes
(17:15:42-17:29:22): the first pass (17:15-17:21) was discarded because its driver failed on A, and eight sampled seconds in it
(17:15:44-17:18:05) show another session's `ugrep` and `python3` jobs at 60-100 % of a CPU; in the second pass (17:21-17:29),
which is the table in section 3, two sampled seconds show a foreign process at 20 % (`ps` reports a lifetime average, so a
short-lived job reads 20 %). The census interleaves A and B, so a foreign job lands on both. The M3 and M1b windows had none.
