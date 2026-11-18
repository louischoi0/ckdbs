# BL's rung-3 probe: the `transactional` reader's excess is the checkpoint on a roll, not a `normal` walker beside it - rung 3 is not opened; forced sharing is not a clean bound

**Thesis.** Measured on worktree `bl-rung3-probe` at `67973ebc`
(`v3.4.12-24-g67973ebc`; the server is `48857a5a`'s, the driver commit
changed no engine source). This is the probe that the BL close
(`bench/v3.4.0/results-bl-close-v3.4.1-10-gf4bad273.md` §1) cut: §5.2's
second 10,000-row walker at `normal`, beside the `transactional` readers.
The cell was placed by the balancer, with `cores = 4` and the levels
declared; every arm ran 5 times.

- **§5.2's condition for opening rung 3 is not met.** Rung 3 opens if "the
  reactor-hold distribution attributes the excess to a `normal` session
  sharing the reader's core" (§5.2, under bar (v)).
  - **Time-aligned evidence.** In the `levels` arm (the close's cell, no
    `normal` walker) the reader schedule p99 is **12.2 ms** (median of 5),
    against **649 µs** isolated. All 227 reader samples at or above 10 ms
    overlap a checkpoint run in the server log (`[checkpoint] run:`
    intervals), and 216 of them also overlap a WAL roll hold (`[wal]
    roll`).
  - **The hold counters agree.** `sched_hold_in_run_over_10ms` on the read
    cores equals the count of `system` polls over 10 ms.
    `sched_foreground_polls_over_10ms` is 0 on every read core in every
    arm, so no statement or walk slice held a read core for 10 ms.
  - **With the `normal` walker added (`probe`), the walker does not
    attract the excess.** The balancer moves the walker: its core changed
    inside the window in all 5 runs. A reader that connected on the
    walker's core takes 9.7 samples of 1 ms or more, against 8.2 for the
    other readers (11 and 19 readers over 5 runs). In `nwonly` (no batch
    session) the figures are 6.5 against 6.9.
  - **The end-of-run snapshot cannot rule out sharing.** No reader *ended*
    on the walker's core in any of the 10 runs. The walker moves mid-window,
    though, and in 3 runs a reader very likely shared its core for part of
    the window (§3.2). What the data supports is that sharing, where it
    happened, did not show in the readers' slow samples.
- **The `probe` arm's lower p99 (1.3 ms) is for a reason not rung 3's.**
  - With the walker added, the inserter writes less log: 59-65 MB/s
    against 73-75. It rolls 9 segments instead of 11-12.
  - The rolls are also much shorter. Their total hold falls from 2.6-3.6 s
    to 0.06-0.36 s, and the roll-blocked wait from 147-520 ms to 5-71 ms.
  - The walker is the one difference between the arms, so the slowdown is
    its doing, through the inserter, not through a reader's core. The path
    is not attributed (§4).
  - Within an arm, a run's p99 does not track its roll-blocked wait: probe
    r1 has the most wait (70 ms) and the lowest p99 (609 µs).
- **Forced sharing does not give a clean bound.** The balancer was
  switched off and every session forced onto core 1 (the measurement build
  of the close's `nobal.patch`), for 5 interleaved pairs.
  - On the medians, the reader schedule p99 is **535 µs** with the walker
    against **506 µs** without it (1.06x).
  - Paired run by run, the ratios are **2.15, 3.14, 1.06, 0.94 and 0.91**.
    2 of 5 pairs are above 2x, with 62 and 51 slow samples against 34 and 6.
  - The walk yields every 64 pages: 23,900 statements in about 123,000
    polls, about 5 polls per statement averaging ~60 µs. That is core 1's
    foreground time over its polls, with the readers' 4,015 taken out. The
    longest single poll is not measured over the window, because the
    `*longest*` counters run from the server's start and `same-iso`
    reaches 1.3 ms too.
  - So the median cost is about one slice, but 5 pairs do not show whether
    the two high pairs are the walker or run-to-run spread.
  - The p50 is lower with the walker (314 against 331 µs). Core 1
    idle-blocks about twice as often (about 24,400 against 12,000 times) but
    for less time in all (2.3 s against 9.6 s blocked). The mechanism is
    not attributed.

**What this settles, and what it does not.**
- **Rung 3 is not opened.** The bar-(v) excess stays BL-R7's remainder:
  the checkpoint holding a reader core on a roll, which is WAL work. BL-R7
  needs bar (v) missed (it is, at 19x) and the operator's ruling; §5.2
  narrows that to an attribution to a sharing `normal` session, which the
  timing does not give.
- **Whether rung 3 is ruled out is the operator's mark, not this file's.**
  Forced sharing costs a reader about one slice on the median, with 2 of
  5 pairs above 2x. More pairs would be needed to tell the walker from the
  spread.
- **Not measured:**
  - What rung 3 could do for a `normal` statement that does not yield. A
    two-tier pick cannot cut a running poll (BL-R7), so it could not help
    there either.
  - `cores = 1`, `group` durability and the readers-at-`normal` arm.

- **Order**: `instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`
  §5.2 and BL-R7; the close's cut, `bench/v3.4.0/results-bl-close-v3.4.1-10-gf4bad273.md`
  §1; the operator's word, `instructions/v3.2.0/raft-marks-2026-11-18.md` §11.
- **Driver**: `tools/bl_mixed.py` at `67973ebc`. It adds `--normal-walkers`
  (the probe's walker, placed like a reader) and `--meta-cores` (every
  interactive core's own SHOW META keys over the window). It was reviewed
  by `critics-developer` and two driver bugs were fixed before any run (the
  commit message lists them).
- **Raw output**: `archive/bl-rung3-probe-v3.4.12-24-g67973ebc/`.
  - `mixed/` and `same/` hold the driver JSON, server logs and consoles.
  - `r3.progress` and `r3same.progress` record each cell's start, end and
    load.
  - `r3-mixed.sh` and `r3-same.sh` are the run scripts; `watch.sh` is the
    60 s watchdog; `nobal.patch` is the measurement build's patch.
  - `tables.py` and its output `tables-mixed.md` hold every run's rows.
  - `ceiling.py` and `ceiling.md`, added on 2026-11-18 for work order CZ:
    the `levels` arm's slow samples against the roll holds and checkpoint
    runs, and the p99 each run keeps without them (arithmetic on samples).

| Item | Value |
|---|---|
| Executed | 2026-11-18 UTC. Release build 01:51:31-01:52:44. Balancer matrix 01:57:55-02:05:18 (20 cells). nobal build ended 02:05:37; forced-sharing cells 02:05:47-02:08:50 (10 cells). Every cell rc=0, errors 0 |
| Binaries | Release, built in this worktree, each run from a copy. `kds_server.48857a5a`: sha256 `d2b455a219f790f420ded446e912ffc64678ce92bb3c24879cf6b0cdd71ac1ee`. `kds_server.48857a5a-nobal` (+ `nobal.patch`, `KDS_BL_NO_BALANCE=1`, never committed, source restored after the build): `8a6c714fd03c6e924ce9511909df8c4a5f05140223637bc0ea87a95ae2212216` |
| Host / device | 8 vCPU Intel Xeon Platinum 8581C (2 threads per core), Linux 6.12.116+deb13-cloud-amd64; data and WAL on `/dev/nvme0n1p1`, ext4, not tmpfs, 42 % full. Servers on CPUs 0,2,4,6, clients on 1,3,5,7. **A different host from the BL close's** (an AMD EPYC 9V74 on Azure), so its numbers are context, not a delta. A sandbox `kds_server` (the web gateway's) was resident and idle throughout |
| Quiet gate | Before every cell: no `kds_tests`, no `cc1plus`, 1-minute load under 2. The gate never waited. Load at cell starts was 0.30-1.86 |
| Server configuration | `cores = 4`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = info`, `reactor_cpus = 0,2,4,6`; a fresh server and data file per cell |
| Cell | C1 of `results-bl-ruling2-remeasure-v3.4.0-22-g418670a3.md` §0: 6 open-loop pk point readers at 400/s in total (`transactional`); a 10,000-row unindexed walker (`analytic`); a 1,024-row inserter (`background`); 10 s window after 2 s warm-up; `--ctl-core -1` |
| Arms (balancer placing, `--placement kernel`), interleaved, 5 runs | `isolated`: readers only. `levels`: + walker + inserter (the close's cell). `probe`: `levels` + one 10,000-row walker at `normal`. `nwonly`: readers + the `normal` walker, no batch session (no partition) |
| Arms (forced, nobal, `--placement same --core 1`), interleaved, 5 runs | `same-iso`: readers only. `same-nw`: readers + the `normal` walker |
| Baseline | This engine's previous number for the cell, the BL close: `levels` reader schedule p99 1.9/6.5/9.6 ms against 0.47 ms isolated, on the other host. The PostgreSQL floor is not measured: it has no core placement or session level to compare |
| Suite | Not run: no engine source changed (the driver is a tool; `tools/` has no test of `bl_mixed.py`) |

## 1. What was run, what was cut

- **Run:**
  - §5.2's mixed cell with its second walker at `normal`, placed by the
    balancer. Beside it, the close's `levels` and `isolated` arms on the
    same binary and host.
  - A walker-only arm (`nwonly`), to see the `normal` walker without the
    partition.
  - Two forced-sharing arms with the balancer off, to price what a
    same-core `normal` walk costs a reader. That cost is the most that
    rung 3, which reorders polls on a core, could take back.
- **Cut, inside the stage's 20 minutes:**
  - the readers-at-`normal` arm (v0 places it as `transactional`);
  - `cores = 1`, where §5.2 reports without a bar;
  - `durability = group`;
  - a `normal` statement that does not yield;
  - BL-Q12's inversion counter (not built).
- **Batch cores are not read per core.** Under the partition an undeclared
  control session cannot be answered on core 0, so `meta_by_core` covers
  cores 1-3 in `levels` and `probe`, and all four cores elsewhere.
- **Four cells ran long.** nwonly r3-r5 and probe r4 took 33-38 s from
  start to end, against about 18 s for the rest (`r3.progress`). The
  window is fixed at 10 s, so this is setup and teardown time; it is not
  attributed.
- **"Longest" values are since the server's start.** That includes setup
  and warm-up, since each cell is a fresh server. Counts (`*_over_10ms`,
  roll-blocked waits) are over the window.

## 2. Reader latency

### 2.1 From the schedule (the instant the open loop said to send), median of 5 runs

| arm | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|
| isolated | 207 µs | 299 µs | 317 µs | 394 µs | **649 µs** | 3.6 ms | 5.0 ms |
| nwonly | 214 µs | 300 µs | 327 µs | 408 µs | 1.1 ms | 4.1 ms | 5.3 ms |
| levels | 206 µs | 303 µs | 328 µs | 436 µs | **12.2 ms** | 255.3 ms | 312.6 ms |
| probe | 194 µs | 281 µs | 312 µs | 420 µs | **1.3 ms** | 4.9 ms | 8.4 ms |
| same-iso (forced, core 1) | 207 µs | 308 µs | 331 µs | 402 µs | **506 µs** | 2.7 ms | 5.3 ms |
| same-nw (forced, core 1) | 164 µs | 275 µs | 314 µs | 413 µs | **535 µs** | 1.9 ms | 5.2 ms |

Per run, the reader schedule p99 is:

| arm | r1 | r2 | r3 | r4 | r5 |
|---|---|---|---|---|---|
| isolated | 1.3 ms | 476 µs | 494 µs | 963 µs | 649 µs |
| levels | 3.7 ms | 15.0 ms | 12.2 ms | 72.2 ms | 3.5 ms |
| probe | 609 µs | 1.3 ms | 1.0 ms | 2.6 ms | 1.4 ms |
| same-iso | 758 µs | 426 µs | 506 µs | 505 µs | 536 µs |
| same-nw | 1.6 ms | 1.3 ms | 535 µs | 477 µs | 490 µs |

`tables-mixed.md` has every run with all percentiles. Read it there if a
cell above differs from it: it is generated, and this table is copied
from it.

### 2.2 Service time (from the send), median of 5 runs

| arm | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|
| isolated | 132 µs | 211 µs | 223 µs | 284 µs | 326 µs | 1.5 ms | 4.0 ms |
| nwonly | 135 µs | 212 µs | 230 µs | 296 µs | 366 µs | 2.3 ms | 5.1 ms |
| levels | 60 µs | 214 µs | 230 µs | 303 µs | 605 µs | 79.5 ms | 312.5 ms |
| probe | 114 µs | 196 µs | 218 µs | 295 µs | 563 µs | 3.4 ms | 5.0 ms |
| same-iso | 127 µs | 216 µs | 231 µs | 297 µs | 323 µs | 828 µs | 2.7 ms |
| same-nw | 90 µs | 179 µs | 218 µs | 303 µs | 353 µs | 1.1 ms | 3.2 ms |

## 3. Where the excess comes from

### 3.1 The waits, by holder (the wait breakdown)

For each arm, these are the medians over its 5 runs of the windowed
totals. The roll-blocked wait is summed over every core's
`load_c<k>_wal_roll_blocked_wait_us`; the `system` poll is taken on the
read cores.

| arm | WAL rolls | roll-blocked wait, all cores | longest roll hold in the window (server log) | reader-core `system` polls > 10 ms | log MB/s | inserter rows/s |
|---|---|---|---|---|---|---|
| levels | 11-12 | 339 ms (147-520) | 349-761 ms | 2-4 per run | 73-75 | 158-168 k |
| probe | 9 | 14 ms (5-71) | 25-81 ms | 0-3 per run | 59-65 | 127-140 k |

### 3.2 The `normal` walker against the readers (balancer placing)

| arm | run | walker core (connect -> end) | readers ending on it | readers moved (any core) | samples >= 1 ms: readers ending on it / the rest | walker p50 / p99 |
|---|---|---|---|---|---|---|
| nwonly | r1-r5 | 0->3, 0->3, 0->3, 2->2, 1->2 | 0, 0, 0, 0, 0 | 1 each | 0 / 22-60 | 392-407 µs / 534-658 µs |
| probe | r1-r5 | 1->2, 1->3, 2->1, 3->2, 3->1 | 0, 0, 0, 0, 0 | 5, 4, 3, 3, 3 | 0 / 24-103 | 690-816 µs / 0.9-1.2 ms |

The end-core columns are snapshots. The walker moves between
interactive cores inside the window: by `load_c<k>_foreground_ppm`, its
core at the window's start differs from its core at the middle in all 10
runs. 17 of the 22 readers that connected on the walker's connect core
stayed on that core, and in 3 runs (nwonly r3, probe r3 and r4) a reader
very likely shared the walker's core for part of the window. A sharper
test groups readers by whether they *connected* beside the walker. Those
readers take as many slow samples as the rest: 9.7 per reader against 8.2
in `probe`, and 6.5 against 6.9 in `nwonly` (11 and 19 readers over 5
runs).

### 3.3 Each read core in the `levels` arm

| run | core: readers at end, longest `system` poll, roll-blocked wait |
|---|---|
| r1 | 1: 3, 67 µs, 0 · 2: 3, **322.1 ms, 321.8 ms** · 3: 0, 125.2 ms, 198.7 ms |
| r2 | 1: 2, 2.7 ms, 2.1 ms · 2: 3, 18.3 ms, 17.6 ms · 3: 1, **315.6 ms, 319.4 ms** |
| r3 | 1: 2, 3.3 ms, 1.8 ms · 2: 1, 5.4 ms, 3.7 ms · 3: 3, **120.0 ms, 141.8 ms** |
| r4 | 1: 3, 58 µs, 0 · 2: 3, 43.3 ms, 1.1 ms · 3: 0, **322.0 ms, 409.2 ms** |
| r5 | 1: 3, 87 µs, 0 · 2: 2, 16.6 ms, 2.2 ms · 3: 1, **163.4 ms, 255.9 ms** |

The bold cells are the close's signature (§3.3 there): the checkpoint
task, rotating over cores, waits on the stream latch while a roll holds
it, and the readers on that core wait with it. The two columns span
differently (the poll is since start, the wait is over the window) and
agree closely in 2 of the 5 bold cells. The end-core count is a weak
instrument here: r4's 322 ms poll was on core 3, which held no reader at
the end, and 38 of r4's 73 samples at or above 10 ms are on readers that
moved from core 3 to core 1. **The timing is the evidence**: all 227
reader samples at or above 10 ms over the 5 runs overlap a `[checkpoint]
run:` interval in the server log, and 216 overlap a `[wal] roll` hold. No
foreground poll on a read core went over 10 ms in any arm.

## 4. What is not attributed

- **Why the inserter is 12-25 % slower in `probe` than in `levels`**
  (run by run, 17 % the median).
  The partition keeps the `normal` walker off core 0, where the
  inserter runs. One candidate, not tested, is the ninth client process: it
  shares 4 client CPUs with the other eight, and those CPUs are SMT
  siblings of the reactors' CPUs. Either way, the `probe` arm's lower p99
  comes through less log, so the `levels` arm is the one that speaks to
  bar (v).
- **`nwonly` against `isolated`** (1.1 ms against 649 µs p99) moves 1
  reader per run, and its slow samples sit on readers that did not end on
  the walker's core. The difference is not attributed to the walker.

## 5. The bars

| bar | result |
|---|---|
| (v) `transactional` p99 at most 2x isolated, the balancer placing | **Not met** in `levels`: 12.2 ms against 649 µs (19x). In `probe` it is 1.3 ms (2.0x, on the line), for a reason not rung 3's (§4) |
| §5.2 under bar (v): rung 3 opens if the reactor-hold distribution attributes the excess to a `normal` session sharing the reader's core | **Not opened**: all 227 `levels` reader samples at or above 10 ms overlap a checkpoint run (216 also a roll hold); no foreground poll on a read core exceeded 10 ms; readers that connected beside the `normal` walker take no more slow samples than the rest |
| Forced sharing (no bar of §5.2's) | Median 535 µs against 506 µs reader p99 (1.06x), but paired ratios 2.15, 3.14, 1.06, 0.94, 0.91 - **not a clean bound**; 5 pairs cannot tell the walker from the spread |
