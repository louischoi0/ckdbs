# CZ-S4, the close: with the cadence checkpoint parking on a busy stream latch, the reader's p99 is 1.55x isolated with 5 of 5 pairs within 2x; the bulk writer loses nothing; the overhead is inside the floor

**Thesis.** Measured on worktree `cz-checkpoint-roll-wait` at `74861601`
(`v3.4.12-113-g74861601`, arm B, CZ-S3) against `origin/main` at
`9712f5f0` (`v3.4.12-112-g9712f5f0`, arm A, the commit CZ-S3 branched from;
no CZ engine code is in it). This is CZ-S4 of
`instructions/v3.2.0/workorder-cz-checkpoint-roll-wait.md`.

- **V1, CZ-Q1 (a)'s bar on the built change with the checkpoint on: met.**
  In the rung-3 probe's `levels` cell at `relaxed` the reader schedule p99
  is **673 µs** on B (median of 5; 569-849 µs) against **435 µs**
  isolated (406-745 µs): **1.55x**. **5 of 5 runs are within 2x** of the
  isolated run interleaved with them: 1.09, 1.55, 1.56, 1.98 and 0.81. A,
  the same cell without CZ, is 3.17x with 2 of 5 pairs within 2x.
  - The closest pair is r4's, 1.98x. Its read cores held one system poll
    of 56.2 ms with no stream-latch wait (§2), and it took the 3 stall
    samples B took in all.
- **BL's bar (v), re-measured (CZ-Q6 (a)): met in this cell.** It is the
  same reading as V1, at the same placement (`--placement kernel`).
- **Beside the bar (CZ-Q11 (a), no bar):** p99.9 median 4.3 ms on B
  against 63.2 ms on A and 2.7 ms isolated. Reader samples at or above
  10 ms: **3** on B against **114** on A over 5 runs. B's read cores
  waited **0 µs** on the stream latch in every run, and A's 154-757 ms;
  a tried entry charges nothing to that counter, so B's evidence is the
  run line's blocking time, 0 µs at every site, not the counter.
  The rolls are alike: 11-12 a window, hold median 249-254 ms on B and
  231-296 ms on A.
- **The bulk cell, CZ-Q2 (a)'s 3 %: met.** BQ's C3, the bulk writer
  alone: 159,301 rows/s on B against 159,481 on A, median of 5, **0.999**.
  The runs range from 0.972 to 1.035.
- **The checkpoint, CZ-Q2 (a), reported with no bar:** a run's wall time
  in the window is 1.0-1.6 s median on B and 0.7-1.5 s on A. The live
  segments at the window's end are 14 on B in every run, every segment
  the run wrote (the oldest core's anchor is still in segment 0), and 7-14
  on A.
  The bound of 16 was never reached at any site. One recycle was skipped,
  in r4, and none was taken blocking.
- **The `group` cell, CZ-Q5 (a), no bar:** 1.31x on B and 1.36x on A,
  3 of 3 pairs each, with no stall sample on either. At `group` a roll
  holds about 3 ms, and A's read cores waited at most 8 µs on the latch.
- **The overhead, interleaved A/B over the whole change at `cores = 1`:
  inside the floor.** Every shape's B - A is within the A2 - A spread or
  below zero. The largest cost not excluded is about +3.8 % on CU's
  `update-in64` at 100,000 rows (B - A +3.7 us, SE 2.4, on 224.4 us; B is
  above A at both sizes there, A2 is not), and +3.2 % on `select-pk` among
  the pooled shapes (B - A -2.2 %, 2 SE 5.4 %). At `cores = 4` the overhead is the C3 cell
  and the inserter's rate in V1 (159,268 against 159,910 rows/s).

**CLA's reading.** Every gate of the close is met: V1, the bulk cell's
3 %, and no measured regression. The order closes with C1 built. BL's
bar (v) is met in the rung-3 probe's cell on this build. That is a record
for BL's texts (CZ-Q6 (a)), not a reopening of BL.

- **Order**: `instructions/v3.2.0/workorder-cz-checkpoint-roll-wait.md`
  §5 (CZ-S4); the operator's word, `raft-marks-2026-11-18.md` §50.
- **Driver**: `tools/bl_mixed.py` as on `main`, without CZ-S1's
  `--stall-cores`, which never fired in an `isolated` or `c1` cell, and
  `tools/cu_close_overhead.py` for the overhead.
- **Raw output**: `archive/cz-close-v3.4.12-113-g74861601/`.
  - `out/v1/`, `out/c3/` and `out/g/` hold each cell's driver JSON and
    server log, and `out/overhead/q1.json` the overhead's rows.
  - `cz-close.sh` is the run script, `cz-close.progress` its record of
    each cell, and `watch.sh` and `watch.log` the 60 s watchdog.
  - `cz_close.py` writes `tables.md`. `q1_tables.py`, CU's close script,
    writes `q1_tables.md`.
  - `binaries.sha256` lists the three binaries.

| Item | Value |
|---|---|
| Executed | 2026-11-18 UTC. Release builds: A 16:15:13-16:16:32, B 16:29:35-16:30:33. Cells 16:32:12-16:44:57, 34 cells, every one rc=0 with 0 errors. Overhead 16:44:57-16:45:57 |
| Budget | About an hour for a milestone's close (CZ-Q8 (a)). Building took 2.3 minutes and the cells and the overhead 13.8 minutes: 16.1 minutes. The script's quiet gate held the matrix 20 s and the driver's own load gate (`--max-load 1.5`) 120 s over 11 cells (`tables.md` §0, "driver wait"); both are inside the 13.8 minutes and outside the budget. Cut against S1's design: the `group` cell has 3 runs, as CZ-Q5 (a) asks for one cell; nothing else was cut |
| Binaries | Release. A `kds_server.9712f5f0`, sha256 `adaec037...f54d0`, and A2 a byte-identical copy. B `kds_server.74861601`, sha256 `7c0addd4...b931c` (`binaries.sha256`) |
| Host / device | 8 vCPU Intel Xeon Platinum 8581C, Linux 6.12.116+deb13-cloud-amd64. Data and WAL on `/dev/nvme0n1p1`, ext4. CZ-S0's host. The sandbox `kds_server` was resident. **No cell found a competing build or test**, and every cell started at a 1-minute load at or under 1.5 (`tables.md` §0) |
| CPUs | Servers on CPUs 0,2,4,6 and clients on 1,3,5,7, as in CZ-S0 and S1 |
| Quiet gate | Before every cell: no `kds_tests`, no `cc1plus`, and a 1-minute load under 2. It waited twice, 5 s and 15 s. The driver then waits for a 1-minute load at or under 1.5: 120 s over 11 cells |
| Server configuration | `cores = 4`, `buffer_pool_frames = 65536`, `log_level = info`, `reactor_cpus = 0,2,4,6`, a fresh server and data file per cell, port 15597. `durability = relaxed` for V1 and C3, `group` for G. The overhead ran at `cores = 1`, `relaxed`, as CU's close did |
| Cells | **V1**: CZ-S0's `levels` cell. 6 open-loop pk point readers at 400/s in total (`transactional`), a 10,000-row walker (`analytic`) and a 1,024-row inserter (`background`). 10 s window after 2 s of warm-up, `--placement kernel`, `--meta-cores`. Arms `isolated-B` (readers only), `levels-A` and `levels-B`, 5 runs; `isolated-B` first in every run, the two loaded arms alternating. **C3**: the inserter alone (`--arm insert --readers 0`), A and B, 5 runs, alternating. **G**: V1's three arms at `group`, 3 runs |
| The bar's denominator | `isolated-B`. The isolated arm carries no checkpoint latch wait to remove, and the bar is B against itself, as V1 states it |
| Baseline | This engine's earlier numbers for the cell: CZ-S1 (`results-cz-s1-ceiling-v3.4.12-66-g20d89234.md`, the spike's `c1` 0.95x) and CZ-S0 (`results-cz-s0-premise-v3.4.12-42-ga4d2fb3b.md`). This file's arm A is the like-for-like baseline. The PostgreSQL floor is not measured: it has no core placement or session level to compare |
| Suite | Full Debug suite, 0 failed of 4727, on CZ-S3's reviewed tree (`74861601`'s build inputs) |
| Overhead | Measured here (§4) |

## 1. V1: the reader's schedule latency, relaxed

| run | isolated-B p99 | levels-A p99 | levels-B p99 | A / isolated | B / isolated | isolated p99.9 | A p99.9 | B p99.9 | stall samples A | stall samples B |
|---|---|---|---|---|---|---|---|---|---|---|
| r1 | 745 µs | 1.4 ms | 811 µs | 1.85x | 1.09x | 3.0 ms | 31.3 ms | 4.3 ms | 8 samples | 0 samples |
| r2 | 435 µs | 25.5 ms | 673 µs | 58.65x | 1.55x | 2.7 ms | 141.3 ms | 4.3 ms | 50 samples | 0 samples |
| r3 | 406 µs | 4.1 ms | 633 µs | 10.05x | 1.56x | 1.6 ms | 382.8 ms | 3.5 ms | 30 samples | 0 samples |
| r4 | 429 µs | 1.2 ms | 849 µs | 2.71x | 1.98x | 1.4 ms | 63.2 ms | 7.4 ms | 15 samples | 3 samples |
| r5 | 704 µs | 881 µs | 569 µs | 1.25x | 0.81x | 3.6 ms | 59.4 ms | 3.8 ms | 11 samples | 0 samples |

| reading | isolated-B | levels-A | levels-B |
|---|---|---|---|
| p99, median of 5 | 435 µs | 1.4 ms | 673 µs |
| against the isolated median | 1.00x | 3.17x | **1.55x** |
| pairs within 2x | - | 2 of 5 | **5 of 5** |
| p99.9, median | 2.7 ms | 63.2 ms | 4.3 ms |
| stall samples (>= 10 ms), 5 runs | 0 samples | 114 samples | 3 samples |
| inserter rows/s, median | - | 159,910 | 159,268 |

**A's excess is a little narrower than CZ-S1's `levels`** (3.17x here,
3.42x there), and its runs range from 1.25x to 58.65x. That is the
checkpoint-on-a-roll stall the order is about: every one of A's 114 stall
samples falls inside a read core's checkpoint run and 112 inside a roll,
and the two worst runs, r2 and r3, are the ones whose read cores ran 6
and 5 checkpoints in the window against 2.

## 2. V1: the read cores

| arm | run | system polls > 10 ms | longest system poll (since start) | in-run holds > 10 ms | stream-latch wait, read cores | rolls in window | roll hold median |
|---|---|---|---|---|---|---|---|
| A | r1 | 2 polls | 110.4 ms | 2 holds | 161.0 ms | 12 rolls | 251.0 ms |
| A | r2 | 5 polls | 163.1 ms | 5 holds | 154.0 ms | 11 rolls | 296.2 ms |
| A | r3 | 2 polls | 450.3 ms | 2 holds | 478.3 ms | 11 rolls | 231.4 ms |
| A | r4 | 2 polls | 675.3 ms | 2 holds | 757.0 ms | 12 rolls | 250.9 ms |
| A | r5 | 3 polls | 164.8 ms | 3 holds | 274.4 ms | 12 rolls | 249.7 ms |
| B | r1 | 0 polls | 3.7 ms | 0 holds | 0 µs | 12 rolls | 250.4 ms |
| B | r2 | 0 polls | 4.9 ms | 0 holds | 0 µs | 12 rolls | 253.9 ms |
| B | r3 | 0 polls | 3.6 ms | 0 holds | 0 µs | 12 rolls | 253.6 ms |
| B | r4 | 1 poll | 56.2 ms | 1 hold | 0 µs | 11 rolls | 248.7 ms |
| B | r5 | 0 polls | 3.5 ms | 0 holds | 0 µs | 12 rolls | 252.9 ms |

- **B's read cores wait 0 µs on the stream latch.** B's longest
  `system` poll on a read core is 3.5-4.9 ms in four runs. CZ-S1 found a
  tried latch charges no wait by construction, so the evidence here is
  the polls and holds, not the counter.
- **B r4's one 56.2 ms system poll is not the latch.** It is on core 2,
  inside core 2's checkpoint run (0.89 s before the window to 0.257 s into
  it); the 3 stall samples end at 0.257-0.258 s on the driver's clock, at
  the run's end, and no roll overlaps them. The run line blocked 0 µs at
  every site and parked 1.4 ms at the flush. **It is not a page write**:
  the run's longest step is 632 µs and its longest page write 500 µs
  (CZ-Q12 (a)'s reading). **It is not the inline gate**: the run took it
  0 times. Of CZ-S1's list, the park's flush writing the staged ring
  (CZ-Q21 (a)) is left, or a part of the run's completion no phase times.
  Nothing here separates them.

## 3. The checkpoint (CZ-Q2 (a), no bar)

B's cadence runs inside the window, every core:

| run | runs | wall, median | wall, max | busy answers begin/end/flush | of them on a roll | bound reached | parked begin/end/flush | recycle skipped | live segments at window end (A, B) |
|---|---|---|---|---|---|---|---|---|---|
| r1 | 2 runs | 1.48 s | 1.61 s | 1/1/4 answers | 1/1/3 answers | 0/0/0 reached | 50.5/111.9/303.1 ms | 0 skipped | 14 segments, 14 segments |
| r2 | 2 runs | 1.59 s | 1.64 s | 1/2/6 answers | 1/2/2 answers | 0/0/0 reached | 126.4/118.1/231.1 ms | 0 skipped | 7 segments, 14 segments |
| r3 | 2 runs | 1.58 s | 1.64 s | 1/2/4 answers | 1/2/2 answers | 0/0/0 reached | 128.8/212.5/343.6 ms | 0 skipped | 11 segments, 14 segments |
| r4 | 4 runs | 1.02 s | 1.28 s | 0/0/7 answers | 0/0/3 answers | 0/0/0 reached | 0/0/6.4 ms | 1 skipped | 14 segments, 14 segments |
| r5 | 2 runs | 1.56 s | 1.61 s | 2/2/3 answers | 1/2/2 answers | 0/0/0 reached | 130.7/47.3/346.8 ms | 0 skipped | 14 segments, 14 segments |

A's runs in the window: wall median 0.68-1.50 s and max 1.48-1.83 s
(`tables.md` §2).

- **The flush is the most-busy site**, as CZ-S1 found. 3-7 busy answers
  a window (its 2-4 checkpoint runs; at most 4 in one run), 12 of the 24
  on a roll.
- **The bound of 16 was never reached**, at any site in any B server log,
  so the blocking entries took no time. The question §8.3 left open, how
  often a try is busy, is answered for this cell: 3-7 times a window at
  the flush and 0-2 at each record.
- **The recycle was skipped once in the 12 B runs inside the windows**
  (r4; C3's bulk-B r5 skipped once more) and never reached its bound.
  Live segments at the window's end are 14 on B in every run: segment 13
  is current and the oldest core's anchor is still in segment 0, so the
  recycle freed nothing in any B window. A's 7 and 11 are runs in which
  the checkpoint went more often (8 and 6 runs in the window against 2).
  In A r1, r4 and r5, as in B r1-r3 and r5, the first run in the window
  was over 1.25 s (the slot to the next core) and no run started for
  about 5 s after it; why is not read here. B's first run parked
  0.04-0.23 s on the latch in r2, r3 and r5 and was over 1.25 s without
  the parks, so these counts do not show CZ costing a run; it does
  lengthen runs.

## 4. The overhead at `cores = 1` (`tools/cu_close_overhead.py`, 5 runs)

Server CPU per unit, pooled over the sizes. A2 is a byte-identical copy
of A, the floor:

| shape | A | B - A (SE) | A2 - A (SE) | B - A % | largest cost not excluded |
|---|---|---|---|---|---|
| select-pk | 22.03 us | -0.48 us (0.59) | -0.64 us (0.42) | -2.2 % | +3.2 % |
| select-range | 29.63 us | -0.22 us (0.12) | +0.40 us (0.32) | -0.7 % | +0.1 % |
| select-idx | 24.23 us | -0.27 us (0.11) | -0.11 us (0.11) | -1.1 % | -0.2 % |
| select-scan | 41.85 us | -0.46 us (0.16) | -0.18 us (0.21) | -1.1 % | -0.3 % |
| select-join | 44.56 us | -0.50 us (0.44) | -0.55 us (0.36) | -1.1 % | +0.8 % |
| update-pk | 22.82 us | -0.01 us (0.05) | -0.10 us (0.08) | -0.0 % | +0.4 % |
| update-scan | 95.90 us | +0.16 us (0.25) | +0.46 us (0.41) | +0.2 % | +0.7 % |
| ins-del | 42.67 us | -0.15 us (0.13) | -0.13 us (0.15) | -0.4 % | +0.3 % |

CU's moved shapes, run as plain statements here (`q1_tables.md`), are
within 2 SE of A. The largest is `update-in64` at 100,000 rows, +3.7 us
(SE 2.4) on 224.4 us. At one core the stream has no latch and the tried
entries are the blocking ones (CZ-R6). What B adds on an append is one
branch on a null pointer, and on a roll one atomic flag.

## 5. The `group` cell (CZ-Q5 (a), no bar)

| reading | isolated-B | levels-A | levels-B |
|---|---|---|---|
| p99, median of 3 | 422 µs | 575 µs | 555 µs |
| against the isolated median | 1.00x | 1.36x | 1.31x |
| pairs within 2x | - | 3 of 3 | 3 of 3 |
| p99.9, median | 1.1 ms | 3.2 ms | 2.9 ms |
| stall samples | 0 samples | 0 samples | 0 samples |

At `group` a roll holds about 3 ms (8 rolls a window) and A's read
cores waited at most 8 µs on the latch, as BQ found. CZ changes nothing that
this cell can see.

## 6. What this file does not establish

- **One cell, one host.** V1 is the rung-3 probe's cell with the
  balancer placing, on the host CZ-S0 and S1 used. BL's other mixed
  cells and placements are not re-run.
- **B r4's 56.2 ms hold** is not attributed (§2).
- **C3 is one writer.** A load whose busy answers reach the bound of 16
  is not built here: the bound was never reached.
- **The overhead's cores = 4 reading** is C3 and V1's inserter rate, not
  a per-statement A/B at four cores.
- **CZ-S3's tests run on in-memory devices.** This file is the only
  measurement of the built change on a real device.
