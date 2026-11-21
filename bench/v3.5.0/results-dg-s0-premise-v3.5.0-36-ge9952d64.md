# DG-S0, a WAL segment's body priced: under a log that rolls, the fill's `fsync` stalls the commits due inside it, about 280 ms a segment; no fill raises a commit's median 76-94 %; written extents with no fill in the window are best at the commit and the bulk rate, not at every tail

**Thesis.** Measured on worktree `dg-wal-segment-body` at `e9952d64`
(`v3.5.0-36-ge9952d64`), whose source is `origin/main`'s `46ee8a6e`, with
a measurement-only spike over it (`archive/dg-s0-premise-v3.5.0-36-ge9952d64/dg-s0-spike.patch`,
`KDS_DG_SPIKE`). The spike is not landed and has no test or proof. The
order is `instructions/v3.2.0/workorder-dg-wal-segment-body.md` §5, DG-S0,
opened on the operator's standing go-ahead (`raft-marks-2026-11-21.md` §3).
There are 54 cells, 18 shapes × 3 runs, all on ext4. **No bar is set
(DG-Q1 (a)): V0 is the operator's reading of §1.**

- **The fill as it is (P0) stalls one-row commits whenever the log rolls**
  (§2). One-row commits at `group` beside a 24 MB/s log: p50 976 µs,
  **p95 171 ms, p99 264 ms**. At `strict`: p50 936 µs, p95 174 ms. With 16
  writers: p95 204 ms.
  - Of the writers' samples of 10 ms or more, 79 % at `group`, 76 % at
    `strict` and 68 % with 16 writers are due inside a fill's `fsync`; the
    rest are due while the queue that sync leaves behind drains (inferred).
    None at `group` is due inside a checkpoint run, and 6 of 2,792 at
    `strict` are.
  - **M4 times the fill.** The 64 writes take 35-50 ms and the `fsync` that
    follows takes **287-503 ms**. That `fsync` is the disturbance.
  - CZ-S1b's commit cells never reached half a segment, so they never met
    this. Its p95 (985 µs of service, 1.2 ms from the schedule) was a log
    that did not roll.
- **No fill (P1) moves the conversion onto every commit when the log moves
  fast** (§2). At `group`: p50 1.9 ms against 976 µs (+94 %), p95 2.9 ms.
  At `strict`: p50 1.6 ms (+76 %), p95 2.6 ms. A 128-row commit's p50 is
  2.6 ms against 1.6 ms (+66 %).
  - With no fill held to 400 a second the inserter did not keep up: 3,584
    statements against 4,000, and 3,011 with 16 writers (P0 reached 3,886,
    M2 4,000). One serial session falls behind once its mean statement
    exceeds the 2.5 ms interval, and P1's p50 is 2.6 ms. Under `group` and
    `strict` the log ran slower without the fill than with it.
  - The bulk writer gains 18 % at `group` and 49 % at `relaxed`, and the
    roll's hold falls from 263 ms to 5.5 ms at `relaxed` (§3, §4).
- **Written extents with no fill in the window (M2, the ceiling for P3, P4
  and P5) beat both arms at every commit percentile from p25 to p99.9, at
  both commit sizes, and at the bulk rate, but not at every tail.** P1 beats it at the bulk writer's
  p99.9 and max at `group`, its max at `relaxed`, its long ms/s at
  `group` (44 against 56), the roll's longest hold at `relaxed` (6.5 ms
  against 390 ms), the walker's p99 at `relaxed` and both commit sizes' max at
  `strict`; P0 beats it at the bulk writer's p50.
  - A one-row commit's p50 is 872 µs and its p95 1.2 ms at `group`; 881 µs
    and 1.3 ms at `strict`.
  - The bulk writer runs at 167,044 rows/s at `group` (+36 % over P0) and
    239,802 at `relaxed` (+53 %).
  - With the bodies dropped from the page cache, the bulk writer is
    7-9 % slower than warm and still 26 % and 39 % above P0.
- **The fill paced (P2), at 1 MiB per 8 ms, works at `group` and fails at
  `relaxed`.**
  - At `group` the bulk writer runs at 156,895 rows/s (+27 %) and every
    body was ready at its roll.
  - At `relaxed` every one of 33 rolls waited for the paced body under the
    stream latch. The roll's hold is 302 ms at p50 and 691 ms at its
    longest, worse than P0's 325 ms.
  - A pace is a rate, and the log outran this one (a segment fills in
    0.5-0.6 s, the paced body takes 0.8-0.9 s from its ask). This is the
    `[quiet-wrong]` candidate's other face: a body not ready when the roll
    needs it.
- **What is left of the roll's tail once no fill runs is a checkpoint
  run's** (DG-Q6, §5). All 6 rolls of 80 ms or more in the warm `relaxed`
  M2 cell (305-414 ms, 203-343 ms of each at the sync gate) lie inside a
  cadence checkpoint run, of 0.8-1.0 s writing 11,400-12,800 pages, on
  core 0, 1 or 2. So does every inserter statement of 100 ms or more that
  no fill overlaps, in every cell. Checkpoint runs cover 5-31 % of a
  window, so the coincidence is not chance; that the run causes the stall
  is inferred. P1's arm had none of 80 ms at `relaxed`.
- **P3 had no supply**: no segment was removed inside any window
  (§6). Its fallback, the fill, would have carried the whole load.

## 0. The run

| Item | Value |
|---|---|
| Executed | 2026-11-21 UTC. Build 05:21:28-05:22:50; smoke cells about 1.5 minutes; the 54 cells 05:25:45-05:49:42. Every cell rc=0, 0 errors |
| Budget | The standing 20 minutes (DG-Q7 (a)). **Overrun**: the cells ran 23.9 minutes, and 28.2 minutes from the build's start to the last cell's end. The cut was made before the run, in DG-Q7 (a)'s order, keeping M4 on every arm and M2 first: 3 runs a cell, not 5; M1 without the cold M2 arm and without P2; the 16-writer cell with P0 and P1 only; 10 s windows; ext4 only (DG-Q4 (a)). The quiet gate held nothing (0 holds) |
| Binary | `kds_server` built Release from `e9952d64` plus the spike patch, sha256 `70dd07d450de678054aece68b095ac7822ada4f3f6f3fcf5dfc9af81760fad58`. One binary; `KDS_DG_SPIKE` picks the arm, and an empty value is the tree's behaviour plus M4's stamps |
| Spike arms | `nofill` (P1): no zero fill where `posix_fallocate` reserved, the `fsync` kept. `pool=N` (M2): N bodies reserved, filled and synced at the log device's open, before the listener, each later body taken from them by a rename. `cold`: those bodies' pages dropped (`POSIX_FADV_DONTNEED` after the `fsync`). `pace=8000` (P2): a body built ahead is filled 1 MiB at a time, each piece written back by `sync_file_range` and followed by an 8 ms sleep, and the next body is asked for at the segment's start instead of its half. M4 is on every arm: `KDS_DG_FILL_LOG` receives each body's reserve, write and `fsync` times, each ask, each pooled take and each removed segment, on CLOCK_MONOTONIC |
| Spike review | `critics-developer` before the tables were read. No arm invalid: no race on the pool, DG-R3's order kept for a pooled body, `cold` uncached, the paced arithmetic right, one ask a segment. Its three reading checks are applied: no paced roll at `group` waited, and at `relaxed` all did (§4); every M2 run kept bodies in hand (6-17 left, none built inline); the paced arm's write time is given less its 512 ms of sleep, and its "in a fill" shares are not read |
| Driver | `tools/bl_mixed.py` at `e9952d64`, with `--insert-rate` (DG-Q8, landed at `e9952d64`) |
| Host / device | 8 vCPU Intel Xeon Platinum 8581C, Linux 6.12.116+deb13-cloud-amd64; data and WAL on `/dev/nvme0n1p1`, **ext4**. Load at a cell's start 0.96-1.49 by the driver (`host_before`, after its own load wait) and 0.96-1.91 at the run script's gate (`dg-s0.progress`); the gate saw no compiler, linker or test binary at any cell's start (DG-Q9 (a): shared host, gate stated) |
| Server configuration | `cores = 4`, `buffer_pool_frames = 65536`, `log_level = info`, `reactor_cpus = 0,2,4,6`, clients on CPUs 1,3,5,7, placement `kernel`; a fresh server and data file a cell; port 15611 |
| Shapes | **Bulk** (`bg` at `group`, `br` at `relaxed`): 6 readers at 400/s in all, the walker, the closed-loop 1,024-row inserter; `reader=transactional,walker=analytic,inserter=background`. **M1** (`mg` at `group`, `ms` at `strict`): 6 readers at 400/s, 4 open-loop one-row `UPDATE` writers at 800/s in all, the inserter held to 400 statements a second of 128 rows (a commit of about 60 KB, a block or more; about 24 MB/s of log). **Grouping** (`gg`, `group`): the same with 16 writers at 4,000/s |
| Arms | `base` P0, `nofill` P1, `pw` M2 warm (32 bodies for bulk, 12 for M1), `pc` M2 cold (bulk only), `pace` P2 (bulk only) |
| Order | 3 runs; the 18 cells rotate by one a run |
| Baseline | The engine's own previous numbers for these shapes: `bench/v3.4.12/results-cz-s1b-c2-priced-v3.4.12-66-g20d89234.md` (another build, the CZ-S1 spike, before CZ's C1 landed). The PostgreSQL floor is not measured |
| Suite | Not executed: no build input lands (the spike is reverted, and the driver is `tools/`) |
| Raw output | `archive/dg-s0-premise-v3.5.0-36-ge9952d64/`: `out/r<run>/<cell>/` (JSON, server log, `fill.log`), the run script `dg-s0.sh`, its progress file, the watchdog `watch.sh`, the spike patch, and `dg_s0.py` (`tables.md`), `dg_s0_attr.py` (`attribution.md`), `dg_s0_pct.py` (`percentiles.md`) |

## 1. V0's table

The median of 3 runs. A one-row commit's latency is from its schedule; the
128-row commit is the held inserter's service latency (from its send),
which no schedule stamps. "Long" is an inserter statement of 30 ms or
more, as ms of them a second. DG-Q1 (a) asked for the bulk writer's
longest run of long statements; the scripts do not compute it, and long
ms/s stands in. Cells not run are "-".

| candidate | arm | 1-row commit p50 / p95, `group` | the same, `strict` | 128-row commit p50 / p95 / max, `group` | bulk rows/s `group` / `relaxed` | long ms/s `group` / `relaxed` | roll hold p50 / max, `relaxed` |
|---|---|---|---|---|---|---|---|
| P0 the fill | base | 976 µs / **171 ms** | 936 µs / **174 ms** | 1.6 ms / 2.0 ms / 279 ms | 123,223 / 156,921 rows/s | 305 / 369 ms/s | **263 / 325 ms** |
| P1 no fill | nofill | **1.9 ms** / 2.9 ms | **1.6 ms** / 2.6 ms | **2.6 ms** / 3.7 ms / 7.6 ms | 145,563 / 234,331 rows/s | 44 / 71 ms/s | 5.5 / 6.5 ms |
| P2 the fill paced | pace | - | - | - | 156,895 / 161,569 rows/s | 81 / 380 ms/s | **302 / 691 ms** |
| M2, warm (ceiling of P3, P4, P5) | pw | 872 µs / 1.2 ms | 881 µs / 1.3 ms | 1.5 ms / 1.9 ms / 6.2 ms | 167,044 / 239,802 rows/s | 56 / 70 ms/s | 4.0 / 390 ms |
| M2, cold | pc | - | - | - | 155,199 / 218,614 rows/s | 51 / 60 ms/s | 3.9 / 5.8 ms |

16 one-row writers at 4,000/s, `group`: P0 1.3 ms / 204 ms, P1 2.2 ms /
3.3 ms (p50 / p95).

## 2. The commit (M1)

The writers' latency from the schedule, in µs, as the median of runs
(`percentiles.md`):

| cell | p0 | p25 | p50 | p95 | p99 | p99.9 | max | n |
|---|---|---|---|---|---|---|---|---|
| mg-base | 544 | 850 | 976 | 171,358 | 264,357 | 280,360 | 284,155 | 7,787 |
| mg-nofill | 611 | 1,412 | 1,898 | 2,897 | 3,576 | 5,143 | 7,849 | 8,002 |
| mg-pw | 492 | 780 | 872 | 1,230 | 1,651 | 3,314 | 5,763 | 8,004 |
| ms-base | 528 | 810 | 936 | 174,090 | 264,279 | 281,335 | 284,307 | 7,787 |
| ms-nofill | 548 | 1,085 | 1,645 | 2,562 | 3,352 | 5,570 | 7,976 | 8,003 |
| ms-pw | 540 | 790 | 881 | 1,281 | 1,677 | 4,222 | 9,836 | 8,003 |
| gg-base | 511 | 1,039 | 1,265 | 203,782 | 267,648 | 282,240 | 285,803 | 40,011 |
| gg-nofill | 549 | 1,766 | 2,180 | 3,341 | 4,682 | 17,422 | 23,622 | 40,008 |

- **P0's stall is a segment's.** Each of the 4 rolls in a window is
  preceded by a fill whose `fsync` takes 287 ms. A commit's `fdatasync`
  due in it waits for most of it. With 4 stalls of about 280 ms in 10 s,
  11 % of the open-loop schedule falls inside one, which is why the p95
  lands there. Writers' samples of 10 ms or more, over the 3 runs: 2,702 at
  `group`, 2,126 of them (79 %) due inside a fill's `fsync`; 2,792 and
  2,116 (76 %) at `strict`; 20,158 and 13,790 (68 %) with 16 writers
  (`attribution.md`). That the `fsync` holds the commit's sync is
  inferred from the timing: both write one ext4 filesystem on one device,
  and nothing else is timed.
- **P1's price is the median, not only the tail**, when the log rolls.
  CZ-S1b read a non-rolling log as meeting a never-written block on one
  commit in sixteen (+1.3 % at p50; inferred there). Here the p50 is
  +94 % at `group` and +76 % at `strict`. Read as nearly every commit's
  sync covering blocks written for the first time since the reservation,
  with the inserter writing 60 KB a statement (inferred: no conversion is
  counted).
  The roll's header sync is 1.1-1.2 ms against 0.4-0.5 ms (§4).
- **The inserter could not hold its rate without the fill**: 3,584
  statements of 4,000 at `group`, 3,352 at `strict`, 3,011 with 16
  writers. Its schedule fell behind, so its log rate is lower: 21.6
  against 23.5 MB/s at `group`. P0 reached 3,886 (its stalls), M2 4,000.
  One serial session falls behind once its mean statement exceeds the
  2.5 ms interval, so P1's commits were measured beside about 8-23 % less
  inserter load than P0's and M2's.
- **M2 is better than P0 even at p50** (872 µs against 976 µs at
  `group`; the p0 and p25 are lower too). M2 never syncs a fill at all;
  P0 also pays a fill's write-back between stalls (inferred).
- **The readers are not this order's.** Their p99 from the schedule is
  435-535 µs in every M1 cell, and 971 µs with 16 writers and no fill.

## 3. The bulk writer

The inserter's service latency, in µs, as the median of runs:

| cell | p0 | p25 | p50 | p95 | p99 | p99.9 | max | rows/s | long ms/s | long share inside a fill |
|---|---|---|---|---|---|---|---|---|---|---|
| bg-base | 4,866 | 5,289 | 5,457 | 7,889 | 130,132 | 281,737 | 283,313 | 123,223 | 305 | 85 % |
| bg-nofill | 5,670 | 6,284 | 6,490 | 7,172 | 10,802 | 97,418 | 179,137 | 145,563 | 44 | 0 % |
| bg-pw | 4,823 | 5,348 | 5,544 | 6,200 | 8,487 | 114,876 | 267,463 | 167,044 | 56 | - |
| bg-pc | 5,206 | 5,818 | 6,033 | 6,705 | 9,193 | 68,504 | 257,096 | 155,199 | 51 | - |
| bg-pace | 4,889 | 5,407 | 5,654 | 7,037 | 9,109 | 155,781 | 190,414 | 156,895 | 81 | not read |
| br-base | 3,227 | 3,506 | 3,671 | 7,590 | 13,496 | 329,539 | 386,764 | 156,921 | 369 | 85 % |
| br-nofill | 3,225 | 3,546 | 3,704 | 4,344 | 9,186 | 94,950 | 304,976 | 234,331 | 71 | 0 % |
| br-pw | 3,246 | 3,555 | 3,691 | 4,252 | 7,128 | 17,742 | 394,350 | 239,802 | 70 | - |
| br-pc | 3,264 | 3,613 | 4,017 | 5,189 | 9,258 | 46,061 | 377,405 | 218,614 | 60 | - |
| br-pace | 3,225 | 3,454 | 3,590 | 4,200 | 8,807 | 662,458 | 695,805 | 161,569 | 380 | not read |

- Against CZ-S1b's same shape at `group` (121,469 rows/s with the fill,
  146,073 without), this run's 123,223 and 145,563 agree, on another
  build, with C1 landed.
- The walker at `relaxed` (`tables.md` §4): p99 264 ms with the fill,
  14 ms with P1, 18 ms with M2 warm, 26 ms cold, 251 ms with P2, and 969
  (P0) and 983 (P2) against 1,254-1,515 statements a window. At `group`
  the walker ran fewer statements where the inserter ran faster (11,966
  with P0, 9,041 with M2 warm), because the two share core 0 under the
  partition (inferred). This is the trade CZ-S1b saw. The cold arm does
  not fit it: its inserter is slower than warm and its walker ran fewer
  still (6,898); that is not explained.

## 4. The roll

| cell | rolls | hold p50 | hold max | holds ≥ 80 ms | sync gate's share | header sync | body at the roll (3 runs) |
|---|---|---|---|---|---|---|---|
| bg-base | 8 | 3.1 ms | 3.7 ms | 0 | 0 % | 0.42 ms | hit 24 |
| bg-nofill | 10 | 4.4 ms | 5.1 ms | 0 | 0 % | 1.16 ms | hit 30 |
| bg-pw | 12 | 3.0 ms | 4.1 ms (285 ms once) | 0 (1 once) | 0 % | 0.43 ms | hit 35 |
| bg-pc | 11 | 3.5 ms | 4.5 ms | 0 | 0 % | 0.45 ms | hit 33 |
| bg-pace | 11 | 3.0 ms | 3.7 ms | 0 | 0 % | 0.41 ms | hit 33 |
| br-base | 11 | 263 ms | 325 ms | 11 | 99 % | 0.43 ms | hit 27, waited 6 |
| br-nofill | 16 | 5.5 ms | 6.5 ms | 0 | 7 % | 1.14 ms | hit 48 |
| br-pw | 16 | 4.0 ms | 390 ms | 2 | 81 % | 0.42 ms | hit 48 |
| br-pc | 16 | 3.9 ms | 5.8 ms | 0 | 2 % | 0.44 ms | hit 47 |
| br-pace | 11 | 302 ms | 691 ms | 11 | 13 % | 0.51 ms | **waited 33** |

- **At `relaxed` P0's every roll is held** (11 of 11 at 80 ms or more,
  99 % of it at the sync gate, 10-11 of them overlapping a fill), as
  CZ-S1's `c1` cell showed. Without the fill the hold is 4-6 ms.
- **P1 lengthens the header's sync under the latch** by about 0.7 ms
  (1.13-1.21 ms against 0.39-0.49 ms), DG-R5's named cost, and the
  median hold by 1.0-1.6 ms where P0's is not held (`bg`, `mg`, `ms`,
  `gg`).
- **P2 at `relaxed` waited on its body at every roll**: the paced fill
  takes about 0.84 s from its ask (512 ms of sleep, 326 ms of writing),
  and the ask is at the segment's start, but the segment fills in
  0.5-0.6 s from the previous roll's end. Each roll waits the difference,
  `body_wait_us` 153-630 ms in the raw lines; the 73 MB/s is the rate
  with those waits in it. A wait on the body is a wait under the stream
  latch.

## 5. M4 and the tail that remains (DG-Q6)

| arm | fill write | fill `fsync` | where measured |
|---|---|---|---|
| P0 | 35-50 ms | 287 ms (M1), 294 ms (`bg`), 503 ms (`br`) | every body built ahead |
| P1 | 0.1 ms | 0.8-1.0 ms | |
| P2 | 206 ms (`bg`), 326 ms (`br`), less 512 ms of sleep | 1.1-1.2 ms | its write-back is inside the paced loop |

- **The long statements are the fill's.** At `group` and `relaxed` 85 %
  of P0's long-statement time lies inside a fill built ahead, and 100 % in
  every M1 cell. In P1's and M2's cells, none does.
- **The tail without the fill.** Every roll of 80 ms or more that no fill
  overlaps lies inside a checkpoint run: 6 in `br-pw`, 305-414 ms, 203-343
  ms of each at the sync gate, plus 1 in `bg-pw` and 1 in `br-base`. So
  does every inserter statement of 100 ms or more that no fill overlaps,
  in every cell (1-7 a cell over 3 runs). Each lies wholly inside the
  run, and the runs cover only 5-31 % of a window (1-3 % in M1), so the
  coincidence is measured; the cause is inferred.
  - The runs in question are cadence runs on cores 0, 1 and 2. Under
    `br-pw` they last 0.8-1.0 s and write 11,400-12,800 pages, with
    `log_park_us` 426-657 ms and `data_park_us` 30-88 ms; under `bg` they
    are 0.21-0.5 s and 5,400-9,800 pages.
  - Most of such a run is parked on the log, so the direction is not
    established: the run may stall the log's sync, or wait on it.
  - The P1 16-writer cell's commit tail is the same: 327 of its 344
    writers' samples of 10 ms or more are due inside a checkpoint run,
    which covers 3 % of its window (its p99.9 17 ms).
  - This agrees with the hypothesis of the order's §1.1: the log's sync is
    tied to whatever else syncs on the filesystem. Here that is read as
    the checkpoint's data-file writeback (inferred), as the fill's was the
    fill's.
  - Eight rolls and some 40 statements, all at one `checkpoint_interval_ms`
    on one host and filesystem, do not settle it.
  - P1's cells had no such roll. Why that differs from M2 warm is not
    resolved.

## 6. Supply for P3, and P5

- **No segment was removed inside any window** in any of the 54 cells. In
  ten-second windows at these rates a recycled body was never available.
  P3's supply here is nil, as the order's §2 warned for a bulk load.
- **P5 is not available on this host**: `/usr/include/linux/falloc.h`
  (linux-libc-dev 6.12.117-1) defines no `FALLOC_FL_WRITE_ZEROES`, and
  neither does the tree. Which kernels offer it was not established on
  this host. `FALLOC_FL_ZERO_RANGE` is not a substitute: by the header it
  may leave unwritten extents. M2 is its ceiling only.

## 7. The census (by reading, DG-S0's second half)

On worktree `dg-wal-segment-body` at `2c2ed8dd`, source `46ee8a6e`. It is
recorded in the order's §7, "DG-S0". In short:
- P1 and P5 keep every tail reader's bytes; P2 and P4 keep them only
  while no zero write lands after a body is adopted; P3 breaks the clean
  end.
- No backup, archive or restore path in the tree preserves sparseness.
- P1, P2, P4 and P5 are not format events. P3 is, by DG-R4 and `wal.md`
  §4.2.

## 8. What this does not measure

- Another filesystem, or a device of its own (DG-Q4 (a)).
- Windows longer than 10 s, or 5 runs.
- P4's rule for "quiet", and P3's own price (a scrub, a read before a
  partial write, its supply over a log that does recycle).
- A pace other than 8 ms per MiB.
- M1's cold M2 arm.
- Why the warm M2 arm at `relaxed` meets a checkpoint run's stall that P1
  does not.
