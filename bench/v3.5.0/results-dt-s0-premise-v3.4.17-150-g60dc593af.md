# DT-S0 premise: what a write that never gives its core back costs the sessions that never miss

Measured on `v3.4.17-150-g60dc593af` (Release), the premise stage of
`instructions/v3.2.0/workorder-dt-write-path-yield.md` (§0-§2, DT-Q1, §5's
DT-S0 row). **Finding, in one paragraph.** Two mechanisms were measured as
the effect on four hot sessions that never miss (1,000 point reads/s, keys
1..700, one core). (a) **Cold point `UPDATE`s** (4 sessions, 2,000/s in all,
uniform keys over a relation 4x the pool, OS page cache kept from answering):
the reactor spends **26.8-27.9 % of the window blocked inside `io.page_read`**
(61,360-61,373 reads, 1.53 per `UPDATE`; mean 87.2-91.0 µs, p50 in
[64, 128) µs, p99 in [256, 512) µs, log2 buckets), and the hot sessions'
service p99 is **5,106 µs and 4,790 µs** (two runs), against 530-553 µs alone,
5,260-5,437 µs with the same writers and the page cache answering, and
322-336 µs beside cold point reads (the read side with DE built). The hot
p99 from the schedule is **431,822 µs in one run and 6,224 µs in the other**
of the same cell: that spread is larger than any difference this table could
rank, so the schedule-based p99 of this cell is not a finding. (b) **One
whole-relation walked `UPDATE`** (matches nothing, writes nothing, pool
holding the relation, cache warm, no device in it) runs **119-127 ms per walk
(p50 122-123 ms) without yielding**; the hot sessions' service p50 is
121.8-122.9 ms, p99 241.5-245.0 ms and max 242.1-247.4 ms, and they complete
**34.9 and 34.5 reads/s of the 1,000 offered**. A whole-relation `SELECT`
scanner in the same shape takes 44-45 ms a walk and leaves the hot sessions a
p99 of 288.0-351.7 µs. DT-Q1's bar reads "under 1 ms at p99": the numbers it
is read from are in sections 4 and 5; the reading is the operator's.

## 1. Stamp

| | |
|---|---|
| Executed | 2026-11-26, 17:05-17:16 UTC (first attempt 17:05-17:09, see section 2; the cells that count 17:10-17:15; `summary-*.json` `started` fields). Written 17:18 UTC |
| Branch / worktree | `worktree-dt-write-path-yield`, worktree `dt-write-path-yield` |
| Commit | `60dc593af1b0cedcc128ba756a93527e15c25790`, **`git describe --tags` = `v3.4.17-150-g60dc593af`**. The version of record is `v3.5.0` (taken at `07f4552a`, an ancestor of this commit, 133 commits back), but that tag is not in this clone, so describe prints against `v3.4.17`; the file is named by the string as printed |
| Tree | tracked files clean. Untracked: `tools/dt_s0_premise.py` (the driver, new), this file and its archive directory |
| Binary | `build-release/kds_server`, Release, mtime 2026-11-26 12:00:57 -0500 (17:00:57 UTC), **after** the commit's own timestamp (09:57:44 -0500), so it is not older than HEAD. Measured from a copy (`~/bench-runs/dt/work/kds_server.dt`), **sha256 `d9e683d9b312c7e2f3aa4ddf26d81a1b215afc10ce840ae7abe8ac65c22da9cc`**, equal to the source binary's hash and to `bin_sha256` in every JSON. The Release build the stage waited on ended `exit=0` |
| Host | 24 logical CPUs, 1 thread a core (Intel Core Ultra 7 270K Plus: CPUs 0-7 P-cores at up to 5.4-5.5 GHz, 8+ E-cores at 4.7 GHz); 30 GiB; Linux 7.3.4-300.fc45. Reactor pinned to CPU 1 (`reactor_cpus = 1`); the 8 client processes pinned round-robin over CPUs 2-7 (`--client-cpus`, a P-core each) |
| Data device | `/dev/mapper/fedora-root`, **xfs**, on `nvme0n1` (Sandisk Optimus 5100 500 GB, per `lsblk`); `df -T` of the work directory read `/dev/mapper/fedora-root xfs`; `data_device` in every JSON agrees. **Not tmpfs** (`/tmp` here is). Data file `~/bench-runs/dt/work/cell/dt.db`, 1,056,440,320 B = 128,960 pages of 8 KiB at load |
| Port | 32761, chosen on the command line, checked free |
| Server configuration | `durability = relaxed`; `cores = 1`; `buffer_pool_frames` 32,768 (cells i, iiw, iiiw, iiir) or 160,000 (ivw, ivr); `log_level = warn`; no other non-default key |
| Driver | `tools/dt_s0_premise.py`, adapted from `tools/de_premise.py` (which is unmodified); docstring is its documentation (`bench/docs/` is closed by `bench/README.md`). Invocations are `driver-r0b.log`, `driver-r1.log`, `driver-r1b.log` in the archive; `argv` is in each `summary-*.json` |
| Raw output | `bench/v3.5.0/archive/dt-s0-premise-v3.4.17-150-g60dc593af/`: `dt-<cell>-r<run>.json` (12), `dt-<cell>-r<run>.s.log` (12), `summary-*.json`, the driver logs, `heartbeat` |
| Baseline | this engine's own nearest earlier shape is DE-S0's (section 8), measured on another host; for the write shapes there is no earlier number, so this file becomes the baseline |

## 2. What was measured, and what was cut

One relation `dt_big (id int64, c0 int64, pad char(1000))`, 900,000 rows,
BTREE, loaded once (9.1 s) into a template directory. **Every cell mounts a
fresh copy of the template** (these cells write rows; DE's did not). Pool
32,768 frames is 25.4 % of the 128,960 pages (derived); 160,000 holds all.
Window 20 s after a 4 s warm-up; cells interleaved by round; one control
session walks the relation once before each window (the pre-warm) and reads
`sys.wait_stats`, `sys.wait_histogram` and `sys.meta` at both ends of the
window. Page cache: `posix_fadvise(DONTNEED)` before the mount and `mincore`
(read before the mount and again after the window), the server in
`systemd-run --user --scope -p MemorySwapMax=0` with `memory.max` = its
`RssAnon` after the pre-warm + 64 MiB (DE-Q2 (a)'s method, unchanged).

| cell | hot | co-located load | pool | OS page cache | runs |
|---|---|---|---|---|---|
| i | 4 sessions, 1,000 reads/s total | none | 32,768 frames | warm (`mincore` 1.00) | 2 |
| iiw | same | 4 cold writers: `UPDATE dt_big SET c0 = c0 + 1 WHERE id = <uniform key 1..900,000>`, autocommit, open-loop, 2,000/s in all (500/s each) | 32,768 frames | warm (1.00) | 2 |
| iiiw | same | same as iiw | 32,768 frames | kept from answering (`mincore` 0.00 before the mount, 0.04 after) | 2 |
| iiir | same | 4 cold point `SELECT id, c0 ... WHERE id = <uniform key>`, 2,000/s in all | 32,768 frames | kept from answering (0.00 / 0.06) | 2 |
| ivw | same | 1 walker, closed-loop: `UPDATE dt_big SET c0 = c0 WHERE c0 = -1` (whole-relation write walk, matches no row) | 160,000 frames | warm (1.00) | 2 |
| ivr | same | 1 scanner, closed-loop: `SELECT id FROM dt_big WHERE c0 = -1` | 160,000 frames | warm (1.00) | 2 |

Reproduction mapping: `--rows 900000` gives the 128,960 pages above;
`--frames-quarter 32768`, `--frames-all 160000`; `--hot 4 --hot-rate 1000
--hot-keys 700`; `--cold 4`; the cold rate is the driver's rule (0.3 of a
core over the device probe's p50, capped at `--cold-rate-cap 2000`): the probe's
p50 was 46.6-48.1 µs (O_DIRECT, 8 KiB, queue depth 1, 2,000 reads), 0.3 over it
is about 6,400/s, **so the cap, 2,000/s, is what set the rate** (`--cold-rate
2000` was passed after the first round so every round shares it).
`--seconds 20 --warmup 4 --mem-slack-mb 64`.

**What was cut, and what did not hold.**

- **Cell ivwc (the walker with a quarter pool and the cache kept from
  answering) is not measured.** Its server was killed by the kernel's OOM killer
  inside the scope's `memory.max` seconds into the window (`journalctl --user`:
  "The kernel OOM killer killed some processes in this unit ... Failed with
  result 'oom-kill'", 1.3 G peak), before the first snapshot; the driver's
  traceback is `driver-r0-first-attempt.log`. Before it died, the server's log
  (not archived; its directory was removed with the work directory) showed
  the walked `UPDATE` refused five times with `ERR buffer pool full:
  buffer_pool_frames 32768, 32768 resident (0 pinned, 32753 dirty), nothing
  reclaimable; raise buffer_pool_frames or split the statement`, each after
  284-698 ms. That is one observation of one attempt; it was not re-run and its
  cause is not established here.
- **First attempt of ivw, replaced.** The first driver recorded a hot
  sample only when its due instant fell in the window, so in the cell that
  starves the hot sessions it recorded none. The driver now records by
  completion instant and ivw was re-run (both runs below). **Cells i, iiw, iiiw
  and iiir run 0 were taken before that change** and used the due-instant rule;
  their counts (20,002-20,005 of an offered 20,000) show the schedule never fell
  behind enough for the two rules to differ by more than a handful of samples.
  Cells i r1, iiw r1, iiiw r1, ivw, iiir r1 and ivr r1 used the new rule.
- Two runs of every cell; none cut for time. Wall time of the measurement:
  about 13 minutes from first driver edit to last cell, no hold for any
  competing build or suite.
- **Not done in this stage**: the work order's §7 census (each write path's
  fetches, holds and where the trail first moves); `PURGE`, `INSERT` and
  `DELETE` as cold writers; `cores > 1`; `strict` durability; a heap relation.

## 3. The host while it ran

`competing` (`pgrep -a -f "cc1plus|cmake --build|ctest"`) was **empty before
and after all 12 cells.** One-minute load average before a cell was 0.39-1.42,
15-minute 1.84-2.75 (it falls through the run from the build that ended
minutes earlier). The driver waited for the 1-minute load to fall under 1.5 for
`wait_s` = 0 s before every cell but iiir r0 (5.0 s). Other processes resident
on the host and not part of the run: a podman SQL Server and a PostgreSQL 17
container (idle, a health check each 30 s), `btop`, other `claude` sessions.

| cell | run | loadavg before (1 / 5 / 15 min) | loadavg after (1 / 5 / 15 min) |
|---|---|---|---|
| i | r0 | 1.02 / 4.04 / 2.75 | 0.70 / 3.67 / 2.67 |
| iiw | r0 | 0.70 / 3.67 / 2.67 | 0.65 / 3.38 / 2.60 |
| iiiw | r0 | 0.65 / 3.38 / 2.60 | 1.41 / 3.35 / 2.61 |
| iiir | r0 | 1.42 / 3.29 / 2.60 | 1.22 / 3.10 / 2.56 |
| ivr | r0 | 1.15 / 2.88 / 2.51 | 1.24 / 2.72 / 2.46 |
| ivw | r0 | 0.60 / 2.21 / 2.31 | 0.73 / 2.09 / 2.26 |
| i | r1 | 0.73 / 2.09 / 2.26 | 0.52 / 1.90 / 2.19 |
| iiw | r1 | 0.52 / 1.90 / 2.19 | 0.54 / 1.78 / 2.14 |
| iiiw | r1 | 0.54 / 1.78 / 2.14 | 0.51 / 1.66 / 2.09 |
| ivw | r1 | 0.51 / 1.66 / 2.09 | 0.71 / 1.60 / 2.05 |
| iiir | r1 | 0.39 / 1.27 / 1.90 | 0.52 / 1.21 / 1.86 |
| ivr | r1 | 0.52 / 1.21 / 1.86 | 0.81 / 1.22 / 1.84 |

(run 0 of ivw was taken after ivr r0; the rows are in time order.)

## 4. The cold-write stall (a)

Hot sessions, from the schedule's due instant (what an open-loop user sees,
queueing included). `n` is the operation count in the 20 s window; offered is
1,000 reads/s, so 20,000.

| cell/run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| i r0 | 20,002 | 90.4 µs | 440.1 µs | 544.3 µs | 660.2 µs | 727.7 µs | 1,538.0 µs | 2,566.6 µs |
| i r1 | 20,004 | 82.7 µs | 509.3 µs | 576.0 µs | 692.1 µs | 781.3 µs | 1,673.7 µs | 3,604.2 µs |
| iiw r0 | 20,004 | 78.4 µs | 256.5 µs | 311.4 µs | 754.6 µs | 8,300.1 µs | 13,823.7 µs | 16,503.3 µs |
| iiw r1 | 20,004 | 52.2 µs | 274.9 µs | 336.6 µs | 595.6 µs | 9,452.3 µs | 16,853.1 µs | 21,558.3 µs |
| iiiw r0 | 20,003 | 58.2 µs | 191.0 µs | 268.7 µs | 105,753.2 µs | 431,822.1 µs | 542,743.9 µs | 573,907.4 µs |
| iiiw r1 | 20,005 | 70.8 µs | 192.4 µs | 274.7 µs | 2,117.6 µs | 6,223.9 µs | 12,072.6 µs | 17,932.2 µs |
| iiir r0 | 20,004 | 75.1 µs | 158.3 µs | 183.6 µs | 336.6 µs | 433.0 µs | 740.5 µs | 2,793.1 µs |
| iiir r1 | 20,005 | 56.3 µs | 166.9 µs | 201.0 µs | 350.5 µs | 464.9 µs | 1,077.7 µs | 3,147.4 µs |
| ivw r0 | 698 | 3,891,777.0 µs | 8,815,085.9 µs | 13,683,554.6 µs | 22,320,007.6 µs | 23,131,426.8 µs | 23,275,361.3 µs | 23,275,361.3 µs |
| ivw r1 | 690 | 3,956,441.4 µs | 8,932,852.7 µs | 13,763,515.2 µs | 22,314,778.6 µs | 23,158,944.6 µs | 23,321,773.1 µs | 23,321,773.1 µs |
| ivr r0 | 20,004 | 103.0 µs | 237.5 µs | 257.3 µs | 370.5 µs | 430.3 µs | 1,081.7 µs | 2,722.0 µs |
| ivr r1 | 20,005 | 83.9 µs | 251.8 µs | 277.0 µs | 441.1 µs | 510.7 µs | 867.8 µs | 2,503.9 µs |

(The ivw rows are the walker's cell: the hot sessions there complete 698 and
690 operations, not 20,000, and are read in section 5.)

Hot sessions, service time (from the send):

| cell/run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| i r0 | 20,002 | 35.8 µs | 263.7 µs | 364.3 µs | 467.1 µs | 530.5 µs | 1,376.2 µs | 2,416.1 µs |
| i r1 | 20,004 | 29.1 µs | 337.8 µs | 390.3 µs | 485.7 µs | 553.1 µs | 1,524.9 µs | 2,505.8 µs |
| iiw r0 | 20,004 | 25.5 µs | 179.3 µs | 228.9 µs | 404.6 µs | 5,259.5 µs | 9,804.5 µs | 12,574.0 µs |
| iiw r1 | 20,004 | 30.8 µs | 156.4 µs | 229.6 µs | 401.5 µs | 5,436.9 µs | 11,164.7 µs | 15,379.0 µs |
| iiiw r0 | 20,003 | 30.3 µs | 95.6 µs | 119.7 µs | 2,012.0 µs | 5,106.4 µs | 58,044.2 µs | 66,443.2 µs |
| iiiw r1 | 20,005 | 29.6 µs | 103.5 µs | 197.9 µs | 1,534.5 µs | 4,789.6 µs | 7,530.7 µs | 9,864.6 µs |
| iiir r0 | 20,004 | 29.1 µs | 93.7 µs | 111.1 µs | 251.5 µs | 322.3 µs | 542.9 µs | 2,009.9 µs |
| iiir r1 | 20,005 | 28.6 µs | 99.2 µs | 117.1 µs | 259.2 µs | 336.4 µs | 555.8 µs | 2,773.0 µs |
| ivw r0 | 698 | 65.3 µs | 120,963.6 µs | 121,847.2 µs | 130,412.3 µs | 241,518.5 µs | 242,147.0 µs | 242,147.0 µs |
| ivw r1 | 690 | 56.0 µs | 121,591.8 µs | 122,901.9 µs | 134,920.1 µs | 245,024.7 µs | 247,432.0 µs | 247,432.0 µs |
| ivr r0 | 20,004 | 53.9 µs | 112.1 µs | 124.2 µs | 236.5 µs | 288.0 µs | 680.2 µs | 2,605.4 µs |
| ivr r1 | 20,005 | 62.9 µs | 119.7 µs | 133.0 µs | 291.1 µs | 351.7 µs | 633.7 µs | 2,389.9 µs |

**Reading the two bases.** In service time the hot p99 of iiiw is 5,106 and
4,790 µs; of iiw (the same writers, page cache answering, so no device wait) it
is 5,260 and 5,437 µs; of i, 530 and 553 µs; of iiir (cold reads, DE built),
322 and 336 µs. So with the writers present the hot p99 sits near 5 ms whether
or not the writers wait for a device, and what the device adds is visible
above it, at p95 (iiiw 2,012 and 1,535 µs against iiw's 405 and 402 µs) and at
p99.9 and the max in run 0 (58,044 and 66,443 µs against 9,805 and 12,574 µs).
Run 1's p99.9 and max (7,531 and 9,865 µs) are *below* iiw's, so those two
columns do not separate the cells reliably. From the schedule, iiiw's p99 is
431,822 µs in run 0 and 6,224 µs in run 1: **a 69x spread between two runs
of one cell** (derived). The service p99 of the same two runs agrees (5,106 vs
4,790 µs, 1.07x), so the difference is a few long stalls early or late in
run 0's window that queued the open-loop schedule, not a difference in the
body. **The noise floor for this cell's schedule-based tail is therefore
larger than any delta between cells in it**, and no delta between iiiw and
iiw at p99 is claimed. The floor cell i is itself not a quiet reference at
p50: its p50 (544 and 576 µs from the schedule) is **higher** than every loaded
cell's (183-337 µs), so p50 is not compared across cells in this file; the
cause (a deeper idle state of a lightly used pinned CPU is the candidate) was
not tested.

The cold sessions themselves, service time:

| cell/run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| iiw r0 | 40,002 | 17.6 µs | 98.5 µs | 173.1 µs | 352.4 µs | 3,353.6 µs | 8,512.7 µs | 12,757.5 µs |
| iiw r1 | 40,001 | 22.2 µs | 164.7 µs | 225.3 µs | 380.3 µs | 2,906.1 µs | 9,583.5 µs | 14,783.9 µs |
| iiiw r0 | 40,001 | 28.4 µs | 212.0 µs | 292.8 µs | 1,508.7 µs | 3,988.7 µs | 51,929.7 µs | 66,986.8 µs |
| iiiw r1 | 40,003 | 34.3 µs | 209.1 µs | 280.4 µs | 1,005.9 µs | 3,855.0 µs | 7,073.9 µs | 10,006.2 µs |
| iiir r0 | 40,001 | 33.4 µs | 156.9 µs | 268.7 µs | 460.1 µs | 533.1 µs | 1,202.1 µs | 3,131.2 µs |
| iiir r1 | 40,002 | 35.5 µs | 153.1 µs | 260.1 µs | 453.5 µs | 531.7 µs | 862.1 µs | 2,327.7 µs |

### 4.1 What the reactor does while a write is cold

`sys.wait_stats` over the window, as a share of the 20 s window (derived:
`total_us / window`), core 0 of one core. `io.page_read` is where a write's
miss waits **on the reactor itself** (a blocking `pread`; no write statement
parks, DT-§1); in iiir the same event is time a statement is **parked**, since
DE, so it is not the reactor's stall there. `on_cpu` and `sched.run_queue` are
sums over sessions, so `run_queue` can pass the window.

| cell | run | `on_cpu` | `io.page_read` | `sysio.wal_write` | `sysio.checkpoint` | `sched.run_queue` (sum of sessions) | idle remainder of the core (derived) |
|---|---|---|---|---|---|---|---|
| i | r0 / r1 | 4.5 % / 4.7 % | 0.0 % | 0.0 % | 0.0 % / 0.1 % | 0.2 % / 0.2 % | about 95 % |
| iiw | r0 / r1 | 10.9 % / 11.1 % | 0.6 % / 0.6 % | 0.2 % / 0.2 % | 0.4 % / 0.6 % | 15.0 % / 13.4 % | about 88 % |
| iiiw | r0 / r1 | 8.0 % / 7.9 % | 27.9 % / 26.8 % | 0.8 % / 0.8 % | 0.4 % / 0.4 % | 29.7 % / 23.3 % | about 63 % |
| iiir | r0 / r1 | 4.7 % / 4.6 % | 19.3 % / 17.3 % (parked) | 0.0 % | 0.0 % | 0.2 % / 0.2 % | (the reads are not the reactor's) |

The `io.page_read` events of the write cells, per read:

| cell | run | reads in window | total wait | mean per read | p50 bucket | p99 bucket | device bytes per read (`/proc/<pid>/io read_bytes` over reads) | `pool_would_wait` |
|---|---|---|---|---|---|---|---|---|
| iiw | r0 / r1 | 61,387 / 61,386 | 0.118 s / 0.120 s | 1.9 µs / 2.0 µs | under 2 µs | [2, 4) µs | 0.0 B (answered by the page cache) | 0 / 0 |
| iiiw | r0 / r1 | 61,360 / 61,373 | 5.586 s / 5.351 s | 91.0 µs / 87.2 µs | [64, 128) µs | [256, 512) µs | 18,957 B / 18,979 B | 0 / 0 |
| iiir | r0 / r1 | 30,812 / 30,833 | 3.869 s / 3.470 s | 125.6 µs / 112.5 µs | [128, 256) µs | [128, 256) µs | 8,323 B / 8,257 B | 30,794 / 30,825 |

Buckets are `sys.wait_histogram`'s log2 buckets ([2^k, 2^(k+1)) µs, read
from the cumulative counts; a bucket is the resolution). In iiw the page cache
answers in under 4 µs and the 0.6 % share is the CPU cost of a `pread`, not a
stall. **A write misses 1.53 times** (61,360 reads for 40,000 `UPDATE`s) where
a point read misses 0.77 times (30,812 for 40,000): the extra is whatever
page a write needs beside its leaf and was not decomposed here. The kernel
reads 18,957 bytes from the device for each write-path read against 8,257-8,323
for a read: readahead on the write path's buffered `pread`s, a factor of 2.3
(derived) on bytes, not measured on time. The scope's `memory.max` was reached
8,949 and 8,814 times (`memory.events` `max`) in iiiw against 1,242 and 1,237 in
iiir: the writers' dirty file pages press the scope's limit, which the read
cell does not, **so iiiw's device path includes a memory-limited writeback
that iiir's does not**, and this is a confound of the method, not of the engine.

## 5. The long-walk stall (b)

The walker's `UPDATE` matches no row and writes none, but is a write
statement: it commits once a walk (161 and 159 commits in the window), takes
the write path's holds and logs. The hot sessions are starved:

| cell | run | hot reads completed (derived: `n / 20 s`) | hot service p0 | p25 | p50 | p95 | p99 | max | hot from schedule p50 | schedule max |
|---|---|---|---|---|---|---|---|---|---|---|
| ivw | r0 | 698 ops = 34.9 reads/s | 65.3 µs | 120,963.6 µs | 121,847.2 µs | 130,412.3 µs | 241,518.5 µs | 242,147.0 µs | 13,683,554.6 µs | 23,275,361.3 µs |
| ivw | r1 | 690 ops = 34.5 reads/s | 56.0 µs | 121,591.8 µs | 122,901.9 µs | 134,920.1 µs | 245,024.7 µs | 247,432.0 µs | 13,763,515.2 µs | 23,321,773.1 µs |
| ivr | r0 | 20,004 ops = 1,000.2 reads/s | 53.9 µs | 112.1 µs | 124.2 µs | 236.5 µs | 288.0 µs | 2,605.4 µs | 257.3 µs | 2,722.0 µs |
| ivr | r1 | 20,005 ops = 1,000.2 reads/s | 62.9 µs | 119.7 µs | 133.0 µs | 291.1 µs | 351.7 µs | 2,389.9 µs | 277.0 µs | 2,503.9 µs |
| i | r0 | 20,002 ops = 1,000.1 reads/s | 35.8 µs | 263.7 µs | 364.3 µs | 467.1 µs | 530.5 µs | 2,416.1 µs | 544.3 µs | 2,566.6 µs |
| i | r1 | 20,004 ops = 1,000.2 reads/s | 29.1 µs | 337.8 µs | 390.3 µs | 485.7 µs | 553.1 µs | 2,505.8 µs | 576.0 µs | 3,604.2 µs |

p0 is a request that found the core between two walks. A request that arrives
during a walk waits for it: p25-p95 are one walk (121-135 ms), p99 and max are
two (242-247 ms: a request met the walker's next walk before its own turn).
**The walk holds the core for its whole length**: ON_CPU is 100.5 % of the
window in both runs (0.5 % over, a bookkeeping difference between the counter
and the driver's clock, not examined), and 4 hot sessions each get one request between two walks, which is
4 / 0.1215 s = about 33 requests/s (derived), as measured (34.5-34.9). The
hot sessions' schedule falls up to 23.3 s behind at the end of the window.

The walks themselves (from the walker's own clock; the first 0.8 s walk in
each run is the one in progress when the window ended):

| cell | run | walks completed (all / inside the window) | per walk: min | p50 | p95 | what a walk is |
|---|---|---|---|---|---|---|
| ivw | r0 | 190 / 165 | 119 ms | 122 ms | 127 ms | write walk, no yield, commits each time |
| ivw | r1 | 188 / 163 | 120 ms | 123 ms | 133 ms | same |
| ivr | r0 | 532 / 454 | 44 ms | 45 ms | 46 ms | read walk, yields every 64 pages |
| ivr | r1 | 534 / 456 | 44 ms | 45 ms | 46 ms | same |

A walked `UPDATE` takes 2.7x a `SELECT` walk's time over the same pages
(122 / 45, derived), and **is not free when it writes nothing**: the window
shows 174.2 MB (run 0) and 167.3 MB (run 1) of WAL appended, 21,248 and 20,416
page images fetched (`pool_fetch_images`), 161 and 159 commits and 161 and
159 WAL syncs, and `wal_ring_full` 109 and 93: about 1.05 MB of log a walk
(derived, over 165 walks). Whether the 2.7x is the walk's per-page cost, the
images, or the commit's sync was not separated.

## 6. Waits and decomposition

A hot read is the sum of its waits. The hot sessions' own `io.page_read` is
zero in every cell (their leaves stay resident). What a hot read waits behind
is the reactor's time spent in someone else's statement, and `sys.wait_stats`
names it per cell: in iiiw the reactor's own blocking read, 26.8-27.9 % of the
window; in ivw the walker's CPU, 100.5 %; in iiw 10.9-11.1 % CPU of the writers
plus WAL and checkpoint system I/O (1.0-1.2 % together). **What is not
decomposed:** the hot p99 of about 5 ms in iiw has no device wait in it, and
no `WaitScope` of this run carries it. The window's counters give candidates
(a log of 406 MB in 20 s from 38,553 page images, 6 segment rolls, about 100
interval syncs a second, 40,000 commits) but this run did not separate them, so
no cause is stated. The client and socket round trip is inside every latency
above (the hot service p0 of 25-65 µs is the least a round trip cost); lock
and conflict waits did not occur (`lock.*` events absent from every window's
`waits_delta`, 0 errors in all 12 cells).

## 7. What the run says about the engine

1. **The reactor's stall under a cold write is a device wait of about 90 µs
   a read, 1.5 reads an `UPDATE`, taking 27 % of the core at 2,000
   `UPDATE`/s.** It is the same event DE's premise measured on reads, now on the
   path DE-Q7 (a) left blocking, and `pool_would_wait` is 0 in iiiw: the write
   path never asks the device without waiting, so nothing leaves the reactor.
2. **The cold-write cell's hot p99 does not separate from the warm-cache
   writers' at this resolution** (service p99 4.8-5.1 ms against 5.3-5.4 ms), and
   **the writers' non-device cost is itself a stall of about 5 ms at p99**.
   Before DT can claim a device-wait win at p99, the 5 ms the warm writers
   already carry has to be attributed; this run does not.
3. **The long walk is the larger stall by two orders of magnitude**: 242-247 ms
   at p99 and 3.5 % of offered hot throughput, against 288-352 µs for the same
   walk as a `SELECT`. It involves no device at all (`io.page_read` 0, mincore
   1.00). It follows from no yield in the write walk, as §1 of the work order
   says; the number it adds is that a **single** closed-loop walker is enough.
4. **A walked write that writes nothing still logs and syncs** (1.05 MB and a
   commit a walk). If DT's yield makes the walk shorter on the core, the
   commit and log cost per walk are what remain.
5. The `ivwc` observation, refusal as `buffer pool full` with 32,753 of 32,768
   frames dirty and nothing reclaimable, then an OOM kill under the scope's
   limit, is a single unrepeated attempt. If it reproduces it is a statement
   about a walked write over a relation larger than the pool, which DT's
   walk park would meet; it is flagged for DT-S0's census, not concluded.

## 8. Against this engine's own earlier number (DE-S0)

The nearest earlier shape is DE-S0's cells iii (cold point reads, cache kept
from answering, pre-DE engine) and vscan (scanner, pool holds the relation)
in `bench/v3.4.13/results-de-s0-premise-v3.4.13-19-g35086008.md` (measured at
`v3.4.13-19-g35086008`, 131 commits earlier, another host: Intel Xeon
Platinum 8581C, 8 logical CPUs, Linux 6.12, a cloud persistent disk as NVMe
with a probe p50 of 462.9 µs). **No delta is computed**: the host, the device
and the client placement differ, and the write cells have no earlier number.
For the record of where each file stood:

| | DE-S0 (other host) | this file |
|---|---|---|
| cold reads, hot service p99 | iii: 985.4 µs and 1,095.1 µs (reads blocked on the reactor) | iiir: 322.3 µs and 336.4 µs (reads parked since DE) |
| scanner beside hot, hot service p99 | vscan: 265.6 µs and 276.8 µs | ivr: 288.0 µs and 351.7 µs |
| scanner, per 64-page slice / per walk | vscan: not stated per walk | 44-45 ms per whole walk |
| reactor share in the cold reads' `io.page_read` | iii: 27.0-27.5 %, mean 540.2-550.9 µs | iiir: 17.3-19.3 % parked, mean 112.5-125.6 µs |
| reactor share in cold writes' `io.page_read` | (no cell) | iiiw: 26.8-27.9 %, mean 87.2-91.0 µs |

Both files' cold cells show about 27 % of the core inside a blocking read,
but the rate that produced it differs by design (DE's cold rate was 648/s over
a 463 µs device; here the cap, 2,000/s, over a device 10x quicker), so the equal
shares say nothing about the engine.

## 9. What was not measured

`INSERT` and `DELETE` as cold writers; any write by key list; `cores > 1`;
`strict` durability (every cell is `relaxed`); the walker with a pool smaller
than the relation (cell ivwc, section 2); a write walk that writes rows (the
walker here matches none); a hot read of a row a write holds; the time a
cancel takes; the census of §7 of the work order; the debug build's aborts.
No cell's cold rate was swept (one rate, 2,000/s, set by the cap), so the
dependence of the hot p99 on the cold-write rate is unknown.
