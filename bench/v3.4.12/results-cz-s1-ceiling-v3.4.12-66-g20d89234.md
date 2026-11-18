# CZ-S1, the ceiling: with the checkpoint's stream-latch sites trying and parking, no checkpoint run holds a read core 10 ms and no reader sample reaches 10 ms; C2 alone leaves the stalls and does not meet the bar

**Thesis.** Measured on worktree `cz-checkpoint-roll-wait`, branch
`worktree-cz-s1-spike`, at `20d89234` (`v3.4.12-66-g20d89234`): the
rung-3 probe's `levels` cell under a measurement-only spike, 5 interleaved
runs of five arms. This is CZ-S1 of
`instructions/v3.2.0/workorder-cz-checkpoint-roll-wait.md`. **The spike is
not landed, has no test and carries no proof**; what it shows is the most a
checkpoint that does not block on the stream latch returned in this cell.

- **C1, against CZ-Q1 (a)'s bar: met as the bar is written, on a host
  builds reached.** With the cadence checkpoint's latch sites trying and
  parking (`c1`) the reader schedule p99 is **1.4 ms** (median of 5;
  1.3-3.5 ms) against **1.5 ms** isolated (1.1-2.9 ms), **0.95x**, and 5 of
  5 runs are within 2x of the isolated run interleaved with them (0.95,
  1.26, 1.99, 0.85, 1.07).
  - Two of those five pairs rest on an isolated cell that ran under
    another session's build (r3 at a load of 7.4, r4 at 3.4; §0). The
    three pairs with no build seen on either side are 0.95x, 1.26x and
    1.07x.
  - Against the median of the three isolated cells no build reached
    (1.2 ms) `c1` is 1.21x, with 3 of 5 runs within 2x (§1).
  - This is not V1. V1 is the same bar on the built change (CZ-S4).
- **The stream-latch stalls go.** `levels` took **174** reader samples at
  or above 10 ms in 5 runs (12-56 a run, 13 episodes); `c1` took **0** in
  every run. A read core's checkpoint run held its reactor over 10 ms 14
  times in `levels`, 5 of them over 100 ms, and **not once in `c1`** (the
  scheduler's own counters, §3). The p99.9 is 138.8 ms in `levels` and
  4.4 ms in `c1`, against 4.5 ms isolated. The log rolls as before: 56
  rolls in each arm, hold median 250.9 ms against 251.4 ms.
  - Holds that are not the latch remain possible and C1 does not remove
    them; none fell inside a `c1` window (§2, "What C1 leaves").
- **Where the wait was: the log park's flush, then the begin record.** Of
  the read cores' 1,733 ms of latch wait in `levels`, counted site by
  site, 1,240.7 ms was at the park's flush, 556.1 ms at the begin record's
  append and 7.8 ms at the end record's. The recycle and the inline gate
  took none. The flush is the site CZ-S0 could not time.
- **C2 alone does not meet the bar, and it changes the workload.** Without
  the zero prewrite (`c2`) the roll hold's median falls from 250.9 ms to
  5.7 ms. The reader p99 is 3.2 ms, 2.11x, with 2 of 5 pairs within 2x,
  and 46 stall samples remain (§2). The inserter is closed-loop and runs
  53 % faster, so the C2 arms are a heavier cell, not `levels` with one
  change (§4).
- **C1 with C2 is not better than C1 at p99 here**: 3.4 ms, 2.29x, 2 of 5
  pairs within 2x. Builds reached 3 of its 5 cells, so the size of that is
  not established.

**CLA's reading, for the operator.** C1, alone, as CZ-Q3 (a) has it. C0 is
ruled out by the stall count and by the read cores' holds. C2 does not
earn a place in CZ on this cell; its effect on the roll and on the bulk
writer is large and is a different question from CZ's (§4, §8).

- **Order**: `instructions/v3.2.0/workorder-cz-checkpoint-roll-wait.md`
  §5 (CZ-S1); the operator's word, `raft-marks-2026-11-18.md` §35.
- **The spike**: `archive/cz-s1-ceiling-v3.4.12-66-g20d89234/cz-s1-spike.patch`,
  the diff of branch `worktree-cz-s1-spike` (`455342ad`, `20d89234`)
  against the work branch at `bf8d7a56`. §7 says what it does.
- **Driver**: `tools/bl_mixed.py` **with the spike's change**, the
  `--stall-cores` flag: a reader sends one `SHOW META` after each kept
  sample of 10 ms or more and keeps the core that answered. Every arm ran
  with it. It fired after 229 samples in all and never in `isolated` or
  `c1`, so the bar's two arms are untouched by it. Where it fires it adds
  a statement inside a stall episode and can only add stall samples, so
  `levels`' 174 is not like for like with CZ-S0's 75. `bench/README.md`
  says a driver change inside a measurement stage measures the driver;
  CZ-S0's §7 asked for this one.
- **Reviewed by `critics-developer`** against the raw data and the
  spike's source before the commit; what it corrected is in the order's
  §7.
- **Raw output**: `archive/cz-s1-ceiling-v3.4.12-66-g20d89234/`.
  - `out/r<run>/<cell>/` holds each cell's driver JSON and server log;
    `out/console-*.log` the consoles.
  - `cz-s1.sh` is the run script, `cz-s1.progress` its record of each
    cell's start, end, load and every gate wait; `watch.sh` and
    `watch.log` the 60 s watchdog and its output; `build.log` the two
    builds' times.
  - `cz_s1.py` writes `tables.md`, which holds every run's rows; "`tables.md`
    §n" below is that file's numbering.

| Item | Value |
|---|---|
| Executed | 2026-11-18 UTC. Release builds 06:39:47-06:40:45 and 06:43:16-06:43:59. Matrix 06:44:17-07:15:07, 25 cells, every cell rc=0 and 0 errors |
| Budget | The standing one, 20 minutes a stage (CZ-Q8 (a)). Building took 1.7 minutes, one smoke cell 0.3 minutes and the 25 cells 8.0 minutes of running: 10.0 minutes. The run script's quiet gate held the matrix 14.2 minutes and the driver's own wait for load another 8.7 minutes; those holds are outside the budget (`raft-marks-2026-11-18.md` §35). Nothing was cut |
| Binary | Release, built in this worktree at `20d89234`, run from a copy: `kds_server.20d89234`, sha256 `ebaf66561336c35e8a69c9e1cbe096e7ee7b373eb8f1df4496b59eb15762d7c7`. One binary for every arm; `KDS_CZ_SPIKE` chooses the arm and each run line carries it as `cz_mode` (`tables.md` §0) |
| Host / device | 8 vCPU Intel Xeon Platinum 8581C, Linux 6.12.116+deb13-cloud-amd64; data and WAL on `/dev/nvme0n1p1`, ext4, not tmpfs, 44 % full. CZ-S0's host. A sandbox `kds_server`, an Oracle container and other agent sessions were resident |
| CPUs | Servers on CPUs 0,2,4,6, clients on 1,3,5,7, as in CZ-S0: the four reactors sit on two physical cores |
| Quiet gate | Before every cell: no `kds_tests`, no `cc1plus`, 1-minute load under 2. It waited 170 times 5 s. The driver's own wait (load under 1.5, at most 120 s) ran after it and gave up twice, so a build that began in between reached cells: §0 |
| Server configuration | `cores = 4`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = info`, `reactor_cpus = 0,2,4,6`; a fresh server and data file per cell; port 15597 |
| Cell | The rung-3 probe's, as in CZ-S0: 6 open-loop pk point readers at 400/s in total (`transactional`); a 10,000-row unindexed walker (`analytic`); a 1,024-row inserter (`background`); 10 s window after 2 s warm-up; `--placement kernel`, `--ctl-core -1`, `--meta-cores`, `--stall-cores` |
| Arms, interleaved, 5 runs | `isolated`: readers only, the spike off. `levels`: + walker + inserter, the spike off. `c1`: `levels` with `KDS_CZ_SPIKE=c1`. `c2`: `levels` with `KDS_CZ_SPIKE=c2`. `c1c2`: both. `isolated` ran first in every run; the four loaded arms rotated their order by one a run |
| Baseline | This engine's previous numbers for the cell: `results-cz-s0-premise-v3.4.12-42-ga4d2fb3b.md` (`levels` 3.2 ms against 1.4 ms isolated) and `results-bl-rung3-probe-v3.4.12-24-g67973ebc.md` (12.2 ms against 649 µs). The PostgreSQL floor is not measured: it has no core placement or session level to compare |
| Suite | Not executed. The spike is not landed, and the work branch changes no build input |
| Overhead | Not measured; measured at the milestone's close |

## 0. The host, cell by cell

`tables.md` §0 has every cell's load and competing processes at its two
ends. Six cells had a `cc1plus`, `cmake --build` or `ctest` process at one
end or began under a load of 2 or more:

| cell | run | load at start | load at end | competing at start | competing at end | its p99 |
|---|---|---|---|---|---|---|
| isolated | r3 | 7.41 | 7.54 | 13 processes | 12 processes | 1.7 ms |
| isolated | r4 | 3.38 | 2.78 | 3 processes | 0 processes | 2.9 ms |
| c2 | r5 | 1.43 | 1.48 | 0 processes | 4 processes | 19.7 ms |
| c1c2 | r1 | 1.33 | 3.03 | 0 processes | 12 processes | 3.8 ms |
| c1c2 | r4 | 1.50 | 1.63 | 0 processes | 7 processes | 3.6 ms |
| c1c2 | r5 | 1.44 | 2.10 | 3 processes | 0 processes | 3.4 ms |

- The run script's gate passed `isolated` r3 and r4 at a load of 1.98;
  the driver's wait then gave up in both: it waits at most 120 s for a
  load under 1.5 and then starts.
- No cell of `levels` or `c1` is in the table. `c1` r3 and r4 ran within
  minutes of a build's end: the run script read a 5-minute load of 3.58
  and 2.86 at their starts, against a 1-minute load under 2
  (`cz-s1.progress`).
- "Reached" means a process was seen at one of a cell's two ends; a build
  that began and ended inside a cell is not seen. `levels` r2 and r4 ended
  at loads of 2.25 and 2.10 with none seen.
- An isolated cell that a build inflates flatters the arm paired with it
  (`isolated` r4's 2.9 ms is the arm's highest). §1 gives each arm against
  the three isolated cells no build reached as well.
- The arms' order is not balanced: `c1` followed `levels` in 4 of 5 runs.
  Every cell has a fresh server and data file.

## 1. The reader's schedule latency

Per run: `tables.md` §1.1. Medians of 5, and CZ-Q1 (a)'s two clauses (the
median against the isolated median; each run against the isolated run
interleaved with it, at least 4 of 5 within 2x), with CZ-Q11 (a)'s p99.9
and stall count beside them:

| cell | p50 (median) | p99 (median) | p99 range | p99 against `isolated`'s median | pairs within 2x | p99.9 (median) | p99.9 range | samples at or above 10 ms, 5 runs | a run |
|---|---|---|---|---|---|---|---|---|---|
| isolated | 323 µs | 1.5 ms | 1.1 ms - 2.9 ms | 1.00x | 5 of 5 | 4.5 ms | 4.0 ms - 5.0 ms | 0 samples | 0, 0, 0, 0, 0 samples |
| levels | 323 µs | 5.1 ms | 2.3 ms - 102.2 ms | 3.42x | 2 of 5 | 138.8 ms | 39.0 ms - 376.4 ms | 174 samples | 12, 38, 35, 33, 56 samples |
| c1 | 321 µs | 1.4 ms | 1.3 ms - 3.5 ms | 0.95x | 5 of 5 | 4.4 ms | 3.8 ms - 5.4 ms | 0 samples | 0, 0, 0, 0, 0 samples |
| c2 | 321 µs | 3.2 ms | 2.1 ms - 19.7 ms | 2.11x | 2 of 5 | 5.9 ms | 5.3 ms - 205.3 ms | 46 samples | 0, 1, 0, 0, 45 samples |
| c1c2 | 327 µs | 3.4 ms | 2.2 ms - 3.8 ms | 2.29x | 2 of 5 | 6.2 ms | 6.0 ms - 63.5 ms | 9 samples | 0, 1, 0, 0, 8 samples |

Each run against the isolated run interleaved with it:

| run | `isolated` p99 | `levels` p99 | `levels` / `isolated` | `c1` p99 | `c1` / `isolated` | `c2` p99 | `c2` / `isolated` | `c1c2` p99 | `c1c2` / `isolated` |
|---|---|---|---|---|---|---|---|---|---|
| r1 | 1.5 ms | 2.3 ms | 1.55x | 1.4 ms | 0.95x | 2.1 ms | 1.37x | 3.8 ms | 2.55x |
| r2 | 1.1 ms | 7.7 ms | 7.21x | 1.3 ms | 1.26x | 3.2 ms | 2.97x | 2.2 ms | 2.06x |
| r3 | 1.7 ms | 5.1 ms | 2.94x | 3.5 ms | 1.99x | 3.8 ms | 2.16x | 2.8 ms | 1.60x |
| r4 | 2.9 ms | 4.4 ms | 1.54x | 2.4 ms | 0.85x | 2.2 ms | 0.78x | 3.6 ms | 1.25x |
| r5 | 1.2 ms | 102.2 ms | 86.34x | 1.3 ms | 1.07x | 19.7 ms | 16.64x | 3.4 ms | 2.89x |

Against the median of the isolated cells no build reached (r1, r2 and r5:
1.2 ms):

| cell | p99 (median) | against the clean isolated median | runs within 2x of it | each run's p99 (ratio; `build` where one was seen) |
|---|---|---|---|---|
| levels | 5.1 ms | 4.33x | 1 of 5 | r1 2.3 ms (1.96x); r2 7.7 ms (6.49x); r3 5.1 ms (4.33x); r4 4.4 ms (3.71x); r5 102.2 ms (86.34x) |
| c1 | 1.4 ms | 1.21x | 3 of 5 | r1 1.4 ms (1.21x); r2 1.3 ms (1.14x); r3 3.5 ms (2.92x); r4 2.4 ms (2.04x); r5 1.3 ms (1.07x) |
| c2 | 3.2 ms | 2.67x | 2 of 5 | r1 2.1 ms (1.73x); r2 3.2 ms (2.67x); r3 3.8 ms (3.17x); r4 2.2 ms (1.89x); r5 19.7 ms (16.64x, build) |
| c1c2 | 3.4 ms | 2.89x | 1 of 5 | r1 3.8 ms (3.22x, build); r2 2.2 ms (1.85x); r3 2.8 ms (2.36x); r4 3.6 ms (3.02x, build); r5 3.4 ms (2.89x, build) |

- The pairs clause is met as CZ-Q1 (a) writes it, 5 of 5. On the stricter
  denominator it is 3 of 5, and that reading is CLA's, not the order's.
  Neither is a clean reading of r3 and r4: their isolated cells ran under
  a build, and their `c1` cells within minutes of one. The pairs with no
  build seen on either side are r1, r2 and r5: 0.95x, 1.26x, 1.07x.
- `levels` is 3.42x here against 2.27x in CZ-S0, two hours earlier on the
  same host; the driver differs by `--stall-cores`.
- `c1` r3's 3.5 ms comes from samples of 1 ms to 10 ms (153 of them
  against 51-78 in the other four runs), not from a stall. What raised
  them was not read.

## 2. The stall samples

`tables.md` §2 and §2.1 have every sample's overlap, its core and every
episode against the checkpoint run it falls in.

| cell | stall samples | overlap a checkpoint run | overlap a roll hold | overlap neither | episodes, by CZ-S0's reading |
|---|---|---|---|---|---|
| levels | 174 samples | 174 samples | 154 samples | 0 samples | begin 7 (89 samples); inside the run 4 (48); run's end 1 (22); none 1 (15) |
| c1 | 0 samples | - | - | - | none |
| c2 | 46 samples | 21 samples | 20 samples | 25 samples | inside the run 3 (22 samples); none 1 (24) |
| c1c2 | 9 samples | 9 samples | 8 samples | 0 samples | inside the run 1 (8 samples); publish write 1 (1) |

- **The core, as far as the driver can give it.** Every stall sample of
  `levels` names a read core (1 to 3). In 11 of its 13 episodes the core
  most samples name is a core whose checkpoint run the episode overlaps
  (`tables.md` §2.1); in one more (r3, 2 samples) one sample names that
  core and one another. A sample's core is read after the stall, and the
  placer may have moved the session by then: samples naming another core
  are read as moved sessions (inferred).
- **One `levels` episode is the flush's own time, not the latch.** In r2 a
  225 ms episode has no roll beside it. The one run it overlaps, core 3's,
  has 2.1 ms of latch wait and a flush call of 208.4 ms. Inside `Flush`
  that is either the spin on another thread's write in flight or this
  thread's own write of the staged ring; the spike does not tell them
  apart. The spike's try answers a flush in flight as busy, and `c1`'s
  longest flush call is 1.5 ms.
  - This is the thirteenth episode: its 15 samples name core 2, not core
    3. The placer moved 2 sessions out of core 3 in that window; whether
    these sessions were on core 3 during the stall is not known.
- **`c2` r5's 24-sample episode overlaps no logged run and is still a
  checkpoint's** (inferred). It falls 9.7 s into the window of a cell a
  build reached, and 16 of its samples name core 2.
  - Core 2's scheduler counts one hold over 100 ms inside a checkpoint run
    in that window, and its longest in-run hold is 116.8 ms; the episode's
    first sample is 115.1 ms late.
  - No run line exists for core 2 after the window's start: a run in
    flight at the server's stop is abandoned and logs none
    (`Checkpointer::Abandon`). The engine counted 25,380 checkpoint pages
    written in the window against 9,416 in the runs that logged
    (`tables.md` §3.2).
  - Core 2 waited 3.5 ms on the stream latch in the window, so the hold
    was not the latch. What it was is not known.
  - The twenty-fifth sample that overlaps neither a run nor a roll is the
    last of that cell's 283 ms episode, due 3 ms after its run ended.
- **`c1c2` r5's episode of 8 samples is a foreground statement, not a
  checkpoint site** (inferred). The samples name core 2, are 15 ms apart
  and end together at the end of a roll hold of 119.1 ms.
  - Core 2 counts one foreground poll over 10 ms in that window, of
    117.5 ms: the only one in the 25 cells.
  - The cell's page-latch spin turns are 425,990; the next highest of the
    25 cells is 103,415.
  - Read as a statement on core 2 spinning on a page latch whose holder
    waited out the roll. C1 does not touch that. The cell began with a
    build running.

### What C1 leaves on a read core

- **Holds of about 100 ms that are not the latch**, each seen once:
  - the 208.4 ms flush call in `levels` r2 above. Under C1 the wait on
    another thread's flush in flight becomes a yield and a retry; this
    thread's own write of the staged ring stays;
  - the 116.8 ms hold of `c2` r5's unlogged run above;
  - a publish-phase call of 96.6 ms in `c1` r5, on core 2, with no inline
    gate in it and a longest page write of 495 µs. Its run began 879 ms
    before the window and the call fell before it: that core counts no
    system poll over 10 ms inside the window;
  - a publish-phase call of 28.7 ms in `levels` r3 with no latch wait,
    whose 2 stall samples the episode rule tallies as "inside the run".
- **The inline WAL gate**, blocking as CZ-Q10 (a) keeps it: 41 calls in
  `c1`'s 13 logged read-core runs, 58.7 ms of reactor time, the longest
  3.8 ms. In `c1c2` one of 16.1 ms inside a publish-phase call of 22.3 ms
  made the arm's other stall sample.
- **The checkpoint's page writes** (CZ-Q12): the longest single write
  6.2 ms and a step's longest hold 6.6 ms in `c1`.
- **Samples of 1 ms to 10 ms**: §6.

## 3. The read cores and the stream latch

`tables.md` §3 and §3.2. The read cores' stream-latch wait, a window,
summed over cores 1 to 3 (the engine's own counter), and what their
scheduler counted:

| cell | stream-latch wait, 5 runs | a run, from the least | system polls over 10 ms | holds over 10 ms inside a checkpoint run | of them over 100 ms | foreground polls over 10 ms |
|---|---|---|---|---|---|---|
| isolated | 0 µs | 0 µs, 0 µs, 0 µs, 0 µs, 0 µs | 0 polls | 0 holds | 0 holds | 0 polls |
| levels | 1,732.8 ms | 65.3 ms, 189.2 ms, 195.0 ms, 476.9 ms, 806.4 ms | 14 polls | 14 holds | 5 holds | 0 polls |
| c1 | 0 µs | 0 µs, 0 µs, 0 µs, 0 µs, 0 µs | 0 polls | 0 holds | 0 holds | 0 polls |
| c2 | 303.5 ms | 2.4 ms, 2.5 ms, 7.3 ms, 19.5 ms, 271.8 ms | 7 polls | 7 holds | 2 holds | 0 polls |
| c1c2 | 6 µs | 0 µs, 0 µs, 0 µs, 0 µs, 6 µs | 1 poll | 1 hold | 0 holds | 1 poll |

- **`c1`'s zero latch wait is by construction, and is not the evidence.**
  A tried latch charges no wait: `Latch::try_lock` reads no clock and the
  spike's try path feeds neither the engine's counter nor its own. The
  evidence that nothing held a read core is the scheduler's: no system
  poll and no in-run hold over 10 ms in any of `c1`'s 5 windows, against
  14 in `levels`.
- In `levels` over 99.9 % of the wait is counted as roll-blocked, as in
  CZ-S0. `c1c2`'s one long system poll is the 22.3 ms publish-phase call
  of §2, and its one long foreground poll the 117.5 ms one.

## 4. The rolls, the log and the inserter

`tables.md` §4. Roll holds pooled over a cell's 5 windows:

| cell | rolls | hold p50 | hold p90 | hold max | holds over 300 ms | sync-gate share of the hold | body ready at the roll | log rate (median) | inserter rate (median) | against `levels` |
|---|---|---|---|---|---|---|---|---|---|---|
| levels | 56 rolls | 250.9 ms | 353.7 ms | 794.3 ms | 14 rolls | 98.6 % | 39 of 56 rolls | 73.5 MB/s | 158,412 rows/s | - |
| c1 | 56 rolls | 251.4 ms | 369.0 ms | 743.8 ms | 18 rolls | 98.6 % | 39 of 56 rolls | 74.1 MB/s | 161,644 rows/s | +2.0 % |
| c2 | 83 rolls | 5.7 ms | 7.3 ms | 618.9 ms | 2 rolls | 75.4 % | 83 of 83 rolls | 113.4 MB/s | 242,968 rows/s | +53.4 % |
| c1c2 | 76 rolls | 5.7 ms | 9.2 ms | 530.7 ms | 1 roll | 63.0 % | 76 of 76 rolls | 102.5 MB/s | 219,518 rows/s | +38.6 % |

- **C1 does not touch the roll**, as CZ-R1 requires: the same count, the
  same median, the same tail.
- **C2 removes the common roll's hold and keeps a tail.** With no zero
  fill the next segment's body is always ready, and the roll no longer
  waits out a writer's sync, which the fill is read as having slowed
  (inferred; BQ-S2 inferred the same). Holds over 100 ms remain:
  2 of 83 in `c2` (609.0 ms in r1, which no build reached, where no
  reader stalled, and which is unexplained; 618.9 ms in r5, which one did) and 2 of 76 in `c1c2`
  (530.7 and 119.1 ms, both in r5). This is BQ-S2's shape for H4 option 3:
  the median falls, a tail stays.
- **The cost C2 moves is not measured here.** The prewrite exists so that
  a commit's `fsync` does not pay an extent conversion
  (`src/wal/file_log_device.cpp`, `Prewrite`, whose comment prices it on
  xfs). The cell runs at `relaxed`, where no commit waits on a sync, on
  ext4.
- **The inserter's gain is about what the shorter holds predict.** Roll
  holds cover 28.8 % of `levels`' windows and 2.5 % of `c2`'s, which alone
  gives 1.40x against the 1.53x measured.
- **`c1c2` r1 is not a heavier cell**: 72.8 MB/s and 156,246 rows/s, as
  `levels`, with the arm's worst p99 (3.8 ms) and a build at its end.
- A checkpoint run is shorter under C2 (median 225 ms against 744 ms) and
  there are more of them (35 and 40 in 5 windows against 20 and 15).
- **CZ-Q2's guards under C1, reported with no bar.** The inserter does not
  lose (+2.0 %). A checkpoint run's longest wall time is 1,676 ms against
  1,678 ms. Live segments at the window's end are 11-14 against 7-14. Of
  13 commits on read cores, one found the latch busy and skipped its
  recycle.

## 5. The checkpoint's stream-latch sites, counted

`tables.md` §5.1 has every arm and §5.2 core 0. The read cores' checkpoint
runs that overlap a window, 5 runs of a cell:

| cell | runs | site | calls | answered busy | parked on a roll | bound reached | reactor time in calls | longest call | stream-latch wait | longest latch wait | parked for the latch |
|---|---|---|---|---|---|---|---|---|---|---|---|
| levels | 16 runs | begin record | 16 calls | 0 tries | 0 parks | 0 times | 556.4 ms | 158.1 ms | 556.1 ms | 158.0 ms | 0 µs |
| levels | 16 runs | park's flush | 512 calls | 0 tries | 0 parks | 0 times | 1,493.8 ms | 433.3 ms | 1,240.7 ms | 433.3 ms | 0 µs |
| levels | 16 runs | end record | 16 calls | 0 tries | 0 parks | 0 times | 7.8 ms | 7.8 ms | 7.8 ms | 7.8 ms | 0 µs |
| levels | 16 runs | recycle | 16 calls | 0 tries | 0 parks | 0 times | 6.3 ms | 6.1 ms | 0 µs | 0 µs | 0 µs |
| levels | 16 runs | inline gate, step | 28 calls | 0 tries | 0 parks | 0 times | 38.9 ms | 2.3 ms | 0 µs | 0 µs | 0 µs |
| levels | 16 runs | inline gate, publish | 22 calls | 0 tries | 0 parks | 0 times | 37.5 ms | 2.8 ms | 0 µs | 0 µs | 0 µs |
| c1 | 13 runs | begin record | 19 calls | 6 tries | 5 parks | 0 times | 268 µs | 43 µs | 0 µs | 0 µs | 462.2 ms |
| c1 | 13 runs | park's flush | 7,849 calls | 7,401 tries | 10 parks | 0 times | 39.3 ms | 1.5 ms | 0 µs | 0 µs | 1,011.2 ms |
| c1 | 13 runs | end record | 16 calls | 3 tries | 3 parks | 0 times | 39 µs | 6 µs | 0 µs | 0 µs | 23.1 ms |
| c1 | 13 runs | recycle | 13 calls | 1 try | 0 parks | 0 times | 37 µs | 5 µs | 0 µs | 0 µs | 0 µs |
| c1 | 13 runs | inline gate, step | 22 calls | 0 tries | 0 parks | 0 times | 31.8 ms | 2.0 ms | 0 µs | 0 µs | 0 µs |
| c1 | 13 runs | inline gate, publish | 19 calls | 0 tries | 0 parks | 0 times | 26.9 ms | 3.8 ms | 0 µs | 0 µs | 0 µs |

- **The assertion bases' appends** were called in every run and took 0 µs:
  the cell declares no assertion. The site is not exercised.
- **The sites account for the wait.** The engine's counter gives the read
  cores 1,732.8 ms in `levels`; the sites sum to 1,804.6 ms over the runs
  that overlap a window. The difference is the runs that straddle a
  window's edge.
- **The rows are lower bounds.** A run in flight at the server's stop logs
  no line, so its sites are not in them: `c2` r5 has one (§2), and `c1` r1
  most likely another (the engine counted 17,798 checkpoint pages written
  against 7,378 in its logged runs).
- **Checkpoints under C1 do not do less.** Every logged run is
  `completed=1` and every one published its anchor. By the engine's
  counter `c1` wrote 68,565 pages in its 5 windows against 72,541 in
  `levels`. The count of runs (15 against 20) follows the rotation's mix
  of long and short runs, which both arms have.
- **The flush call is more than its latch wait**: 1,493.8 ms of reactor
  time against 1,240.7 ms of latch wait in `levels`. The other 253 ms is
  the spin on a flush in flight and this thread's own write of the staged
  ring (§2's 208.4 ms call is most of it).
- **Under C1 the task waited off the reactor where it had blocked**:
  1,496.5 ms in 18 parks on a roll (5 at the begin record, 10 at the
  flush, 3 at the end record) and in the yields below. No bound was
  reached, so the blocking call past 16 parks was never exercised.
- **7,391 busy answers at the flush were not a roll** and cost a yield and
  a retry each: 39.3 ms of reactor time inside the calls in 5 windows, the
  yields' own turns not counted. In `c1c2` there are 52,264, of which one
  run (r5, core 3) made 49,805: about 61 ms of continuous spin behind a
  holder that was not a roll.
- **Core 0's checkpoint never found the latch busy** in any arm
  (`tables.md` §5.2): its tries and the inserter's appends are one
  thread's.

What held a read core's reactor longest in a checkpoint run (`tables.md`
§5.4; the count is the runs in which the call reached 10 ms):

| cell | runs | `start` (the snapshots and the begin append) | page write | publish-phase call | a step's whole hold | flush call | inline gate (step / publish) |
|---|---|---|---|---|---|---|---|
| levels | 16 runs | 159.2 ms (7 runs) | 5.9 ms (0 runs) | 28.7 ms (1 run) | 6.2 ms (0 runs) | 433.3 ms (6 runs) | 2.3 ms (0 runs) / 2.8 ms (0 runs) |
| c1 | 13 runs | 1.5 ms (0 runs) | 6.2 ms (0 runs) | 96.6 ms (1 run) | 6.6 ms (0 runs) | 1.5 ms (0 runs) | 2.0 ms (0 runs) / 3.8 ms (0 runs) |
| c2 | 26 runs | 1.6 ms (0 runs) | 3.3 ms (0 runs) | 10.2 ms (2 runs) | 7.8 ms (0 runs) | 253.8 ms (3 runs) | 4.3 ms (0 runs) / 3.7 ms (0 runs) |
| c1c2 | 30 runs | 1.4 ms (0 runs) | 2.4 ms (0 runs) | 22.3 ms (1 run) | 8.9 ms (0 runs) | 1.3 ms (0 runs) | 4.8 ms (0 runs) / 16.1 ms (1 run) |

## 6. The samples of 1 ms to 10 ms

`tables.md` §6. A sample is inside when its interval overlaps a checkpoint
run of a read core.

| cell | samples of 1-10 ms, 5 runs | inside a run | window under such runs | rate inside | rate outside |
|---|---|---|---|---|---|
| isolated | 324 samples | 3 samples | 0.5 s of 50 s | 6.4 /s | 6.5 /s |
| levels | 522 samples | 175 samples | 12.3 s of 50 s | 14.2 /s | 9.2 /s |
| c1 | 397 samples | 96 samples | 11.4 s of 50 s | 8.4 /s | 7.8 /s |
| c2 | 557 samples | 139 samples | 6.8 s of 50 s | 20.4 /s | 9.7 /s |
| c1c2 | 775 samples | 172 samples | 7.9 s of 50 s | 21.7 /s | 14.3 /s |

Under C1 a read core's checkpoint run barely raises the rate (8.4 against
7.8 a second), and the loaded cell's rate outside a run stays above the
isolated one (7.8 against 6.5). Under C2 the rate inside a run is higher
than in `levels`: the logged read-core runs are shorter, write 38 % more
pages in all (79,245 against 57,348) and take the inline gate 155 times
against 50.

## 7. What the spike does

One binary; with `KDS_CZ_SPIKE` unset the engine behaves as at `bf8d7a56`
and the sites are only counted, on the `[checkpoint] run:` line.

- **`c1`, at the cadence task only** (`Checkpointer::RunTask`); the
  synchronous checkpoint (mount, shutdown) is untouched.
  - **The begin record (C1b).** `Start`'s append tries the latch. Busy,
    nothing is appended, the task parks and calls `Start` again, which
    takes the active-transaction and dirty-page snapshots again (CZ-R3's
    retake). The assertion bases' appends stay blocking.
  - **The park's flush (C1a).** `RequestSyncNow`'s flush tries the latch,
    and answers busy for another thread's flush in flight too. Busy, the
    writer is asked for what is already flushed, the task parks, and it
    asks again unless the log is durable past its LSN by then.
  - **The end record (C1b).** As the begin record, with no snapshot.
  - **The recycle (C1c).** `DetachBelow` tries; busy, the recycle is
    skipped. It is retried only when a later commit advances the durable
    redo start again: the anchor advances it before it calls the recycler
    (`src/server/superblock_checkpoint_anchor.cpp:197-205`).
  - **The park.** While a roll holds the latch the task waits on a flag
    the roll sets, read at its reactor's next turn with no wake asked for;
    then one yield. 16 parks on a roll, then the blocking call (CZ-Q4 (a),
    CZ-R4). A busy answer with no roll running is one yield and a retry,
    not counted against the 16.
  - **The inline WAL gate** is untouched and counted (CZ-Q10 (a)).
- **`c2`.** `BuildBody` skips `Prewrite` where `posix_fallocate`
  succeeded and keeps its `fsync`.
- **A defect in the spike, with no effect on these runs.** `tl_busy` is
  not cleared when a site's call ends, and `RecycleBelow` and
  `RequestSyncNow` read it: after a busy try, a later real failure on that
  thread would be taken as busy and neither counted nor logged. No cell
  logged an error.
- **What it leaves undone**: the order's §7, "CZ-S1", lists what CZ-S2
  owes. No test was written and no suite was run on the spike.

## 8. What this file does not establish

- **V1.** The bar on the built change, with its tests, at the close.
- **That C1 is safe.** The spike has no proof; CZ-R2 and CZ-R3 are
  `[quiet-wrong]` and are CZ-S2's to argue.
- **A clean margin for C1.** One host, one filesystem, 5 windows of 10 s,
  with builds on the host for two of the five isolated cells.
- **The engine at `bf8d7a56`.** `levels` and `isolated` ran the
  instrumented binary with the spike off, not the unpatched one.
- **What the non-latch holds were**: the 208.4 ms flush call, the
  116.8 ms hold of an unlogged run and the 96.6 ms publish-phase call.
- **C2's cost.** A commit's `fsync` on unwritten extents at `group` or
  `strict`, another filesystem, and recovery's reading of a segment that
  was never zero-filled were not looked at. C2's gain on the bulk writer
  is measured on one closed-loop inserter at `relaxed`.
- **Anything at `cores = 1`** (no stream latch) **or at `group`** (CZ-Q5
  is the close's).
- **The probe's 19x.** `levels` is 3.4x here and was 2.3x in CZ-S0.
