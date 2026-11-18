# CZ-S0, the premise: with the cadence checkpoint off no read core waits on the stream latch and the 10 ms stalls go; on CLA's proposed bar V0 passes, at a p99 excess far smaller than the probe's

**Thesis.** Measured on worktree `cz-checkpoint-roll-wait` at `a4d2fb3b`
(`v3.4.12-42-ga4d2fb3b`), the rung-3 probe's `levels` cell with the
cadence checkpoint on and off, 5 interleaved runs. This is CZ-S0 of
`instructions/v3.2.0/workorder-cz-checkpoint-roll-wait.md`: M1 to M4 and
the census, with no engine code.

- **V0, against CLA's proposed bar (CZ-Q1 (a), unmarked): passed.** With
  `checkpoint_interval_ms = 0` the reader schedule p99 is **2.3 ms**
  (median of 5; 1.6-2.6 ms) against **1.4 ms** isolated (1.4-2.4 ms),
  **1.60x**. Paired run by run the ratios are 0.97, 1.32, 1.61, 1.59 and
  1.12: 5 of 5 pairs within 2x. **The verdict is the operator's**; V0 is a
  necessary gate, and passing it does not prove C1.
- **At p99 the gate separated little this day.** With the cadence on,
  `levels` is **3.2 ms** (1.8-7.5 ms), 2.27x isolated, with 2 of 5 pairs
  above 2x. One of those five cells ran into another session's build
  (§0). Without it `levels` is 2.8 ms, 1.99x, with 1 of 4 pairs above 2x.
  The probe measured 12.2 ms against 649 µs, 19x, on this host three
  hours earlier.
  - The isolated floor is 2.2x higher here: 53-79 reader samples a run at
    or above 1 ms, against 17-51 in the probe.
  - `levels` took 75 stall samples (10 ms or more) over 5 runs, against
    227 in the probe. A run's p99 is its 41st slowest sample, and 0-38
    stall samples a run do not reach that rank.
  - Two other sessions built and ran test suites on the host throughout.
    The quiet gate held the matrix for 25.8 minutes in all.
- **The tail and the counters carry the premise.** The p99.9 is
  **71.1 ms** with the cadence on (5.2-85.8 ms; 4 of 5 runs above 40 ms)
  and **5.2 ms** with it off (4.7-5.7 ms), against 4.5 ms isolated.
  - All 75 stall samples of `levels` overlap a checkpoint run and a roll
    hold.
  - With the cadence off there is 1 stall sample in 5 runs (12.1 ms),
    overlapping neither. It was due 12.6 ms after the window's end, where
    the driver admits one more due instant.
- **No read core waits on the stream latch once the cadence is off.** The
  read cores' stream-latch wait is 19-702 ms a window with the cadence on,
  99.9 % of it counted as roll-blocked, and **0 µs in all 5 runs** with it
  off, while the log still rolls 12 times a window and a roll holds the
  latch 28-29 % of the window.
- **By timing, most stalls are the begin record's append.** Of the 6
  stall episodes in `levels`, 5 (62 of 75 samples) fall on a run's
  `start`, which took 30-116 ms; 1 lies inside a run on a roll. In `half`
  the 13 episodes are 6 at the begin record, 3 inside a run, 1 at a run's
  end, 1 on a page write, 1 on a publish-phase write and 1 unread (§4).
- **By latch time, most of the wait is at a site the run line does not
  time.** Summed over `levels`, the read cores waited 1,280 ms on the
  latch. Core by core, less what its runs' begin appends and inline gates
  took (the two latch sites the line times), at least 810 ms was taken at
  the park's flush, the end append or the recycle (758 ms if the publish's
  writeback phase, which can hide an inline gate, is taken out too). The largest
  item, a `system` poll of 623 ms on core 3 in r2, stalled no reader
  sample.
- **The cadence-off arm is confounded, as the order said it would be.**
  - The roll holds keep their median (240 ms against 247 ms) and lose
    their tail: 14 of 57 holds are over 300 ms with the cadence on (the
    longest 747 ms) and 0 of 60 with it off (the longest 260 ms).
  - The inserter gains 8 % (172,528 against 159,955 rows/s, medians).
  - The read cores also lose the checkpoint's page writes. One page write
    of 167 ms stalled readers in `half` with no roll beside it. C1 does
    not touch that.
  - Nothing is recycled: 15 segments are live at the window's end in every
    cadence-off run, against 6-14.
- **M4 did not vary what it was meant to.** Halving the inserter's batch
  left the roll count at 11 a window (against 10-12) and the log rate at
  72.1 MB/s (against 73.1). The inserter is closed-loop, so the batch
  changes the statement and not the rate. `half` says nothing on whether
  the excess follows the roll rate.

**What this settles.** In this cell, on a read core, nothing but the
cadence checkpoint waits on the stream latch, and a reader's stalls of
10 ms or more go when the cadence checkpoint goes. §8 lists what it does
not settle.

**For CZ-S2, from the census (§6.2):** CZ-R3's retake is required. What
covers the begin record's active-transaction snapshot today is that
`Start` is one poll, and a park between the snapshot and the append
removes exactly that. **No live defect was found** at `cores > 1`.

- **Order**: `instructions/v3.2.0/workorder-cz-checkpoint-roll-wait.md`
  §5 (CZ-S0, M1-M4); the operator's word, `raft-marks-2026-11-18.md`
  §31.
- **Driver**: `tools/bl_mixed.py`, unmodified, at `a4d2fb3b`.
- **Raw output**: `archive/cz-s0-premise-v3.4.12-42-ga4d2fb3b/`.
  - `out/r<run>/<cell>/` holds each cell's driver JSON and server log;
    `out/console-*.log` the consoles.
  - `cz-s0.sh` is the run script, `cz-s0.progress` its record of each
    cell's start, end, load and every gate wait; `watch.sh` and
    `watch.log` are the 60 s watchdog and its output.
  - `cz_s0.py` writes `tables.md`, which holds every run's rows; the
    section numbers cited below as "`tables.md` §n" are that file's.
- **Reviewed by `critics-developer`** against the raw data and the source
  before the commit; what it corrected is in the order's §7.

| Item | Value |
|---|---|
| Executed | 2026-11-18 UTC. Release build 04:46:40-04:48:00. Matrix 04:48:21-05:27:09, 25 cells, every cell rc=0 and 0 errors: 12.9 minutes in cells and 25.8 minutes held at the quiet gate |
| Budget | The standing one is 20 minutes a stage. Building and running the cells took 14.2 minutes; with the gate's waits the measurement spanned 40.5 minutes of wall time, over the budget on the clock. Whether a hold for another session's build counts is the operator's to say. Nothing was cut |
| Binary | Release, built in this worktree at `a4d2fb3b`, run from a copy: `kds_server.a4d2fb3b`, sha256 `34758fde7dcc570b4ba734be3ca70da2c09ec46042ea02ca20c14c4b8e5a3017` |
| Host / device | 8 vCPU Intel Xeon Platinum 8581C, Linux 6.12.116+deb13-cloud-amd64; data and WAL on `/dev/nvme0n1p1`, ext4, not tmpfs, 43 % full. The rung-3 probe's host. A sandbox `kds_server`, an Oracle container and other agent sessions were resident |
| CPUs | Servers on CPUs 0,2,4,6, clients on 1,3,5,7. `/sys` gives the sibling pairs as 0/4, 2/6, 1/5 and 3/7: **the four reactors sit on two physical cores**, and the clients on the other two. If reactor k is pinned to the k-th listed CPU, read core 2 shares a physical core with batch core 0, and read cores 1 and 3 share the other. This is the probe's layout too; that file's statement that the client CPUs are the reactors' siblings does not match `/sys` |
| Quiet gate | Before every cell: no `kds_tests`, no `cc1plus`, 1-minute load under 2. It waited 309 times 5 s. The driver's own wait (load under 1.5, at most 120 s) ran after it, so a build that began in between could reach a cell: §0 |
| Server configuration | `cores = 4`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = info`, `reactor_cpus = 0,2,4,6`; a fresh server and data file per cell; port 15597 |
| Cell | The rung-3 probe's: 6 open-loop pk point readers at 400/s in total (`transactional`); a 10,000-row unindexed walker (`analytic`); a 1,024-row inserter (`background`); 10 s window after 2 s warm-up; `--placement kernel`, `--ctl-core -1`, `--meta-cores` |
| Arms, interleaved, 5 runs | `isolated`: readers only. `levels`: + walker + inserter (M1's default arm, M2). `nockpt`: `levels` with `checkpoint_interval_ms = 0` (M1, V0's number, M3). `half`: `levels` with `--batch 512` (M4). `iso0`: `isolated` with `checkpoint_interval_ms = 0`, a control the order does not name |
| Baseline | This engine's previous number for the cell: `results-bl-rung3-probe-v3.4.12-24-g67973ebc.md`, `levels` 12.2 ms against 649 µs isolated, on this host at `48857a5a`'s server. The PostgreSQL floor is not measured: it has no core placement or session level to compare |
| Suite | Not run: no engine source changed |

## 0. The host, cell by cell

As the driver recorded it. `competing` counts processes matching
`cc1plus|cmake --build|ctest`; a bare `kds_tests` is not in that pattern,
and the run script's gate tests for it by name.

| cell | run | driver wait | load at start | load at end | competing at start | competing at end |
|---|---|---|---|---|---|---|
| `isolated` | r1 | 25 s | 1.45 | 1.11 | 0 processes | 0 processes |
| `isolated` | r2 | 5 s | 1.45 | 1.19 | 0 processes | 0 processes |
| `isolated` | r3 | 20 s | 1.46 | 1.04 | 0 processes | 0 processes |
| `isolated` | r4 | 15 s | 1.48 | 1.29 | 0 processes | 0 processes |
| `isolated` | r5 | 0 s | 1.40 | 1.36 | 0 processes | 0 processes |
| `iso0` | r1 | 20 s | 1.39 | 1.08 | 0 processes | 0 processes |
| `iso0` | r2 | 0 s | 1.28 | 1.21 | 0 processes | 3 processes |
| `iso0` | r3 | 120 s | 7.41 | 7.69 | 11 processes | 11 processes |
| `iso0` | r4 | 15 s | 1.44 | 1.10 | 0 processes | 0 processes |
| `iso0` | r5 | 15 s | 1.46 | 1.11 | 0 processes | 0 processes |
| `levels` | r1 | 0 s | 1.11 | 1.39 | 0 processes | 0 processes |
| `levels` | r2 | 0 s | 1.19 | 1.60 | 0 processes | 0 processes |
| `levels` | r3 | 0 s | 1.04 | 1.55 | 0 processes | 0 processes |
| `levels` | r4 | 0 s | 1.29 | 1.57 | 0 processes | 0 processes |
| `levels` | r5 | 0 s | 1.36 | 2.47 | 0 processes | 11 processes |
| `nockpt` | r1 | 0 s | 1.39 | 1.76 | 0 processes | 0 processes |
| `nockpt` | r2 | 5 s | 1.47 | 1.28 | 0 processes | 0 processes |
| `nockpt` | r3 | 5 s | 1.43 | 1.57 | 0 processes | 0 processes |
| `nockpt` | r4 | 5 s | 1.45 | 1.67 | 0 processes | 0 processes |
| `nockpt` | r5 | 20 s | 1.39 | 1.79 | 0 processes | 0 processes |
| `half` | r1 | 0 s | 1.08 | 1.58 | 0 processes | 0 processes |
| `half` | r2 | 0 s | 1.21 | 2.62 | 3 processes | 3 processes |
| `half` | r3 | 20 s | 1.44 | 1.88 | 0 processes | 0 processes |
| `half` | r4 | 0 s | 1.10 | 1.40 | 0 processes | 0 processes |
| `half` | r5 | 0 s | 1.11 | 1.81 | 0 processes | 0 processes |

**The 10 cells V0 is read from (`isolated`, `nockpt`) started at a load
of 1.39 to 1.48 with no competing process at either edge.** Four other
cells did not:

| cell | what the driver recorded | used for |
|---|---|---|
| `iso0` r3 | started after the driver's 120 s wait gave up, at load 7.41 with 11 competing processes, under a build for its whole length | the control only; its p99 (2.5 ms) is the highest of its arm |
| `iso0` r2 | 3 competing processes at its end | the control only |
| `half` r2 | 3 competing processes at both edges, load 1.21 to 2.62 | M4 |
| `levels` r5 | 11 competing processes at its end, load 1.36 to 2.47 | M1's default arm, M2 |

**`levels` r5 is contaminated beyond its end.** It has 239 slow samples
against 88-178 in the other four, an inserter at 136,332 rows/s against
156,150-169,798, and 10 rolls against 11-12. Its one stall episode, at
375-491 ms of the window, is the begin record's like r1's, r2's and r3's,
and is kept for M2. Its p99 (4.2 ms) and its pair (2.98x) are not clean,
and §2 gives `levels` with and without it.

**V0's pairs are not all adjacent.** r5's `nockpt` ran 15.9 minutes after
its `isolated` (05:25:36 against 05:09:43), across a 15-minute gate hold.

## 1. What was run, what was cut

- **Run:** M1 (the cadence off against the default, with the isolated arm
  interleaved), M2 (each run and each stall episode against the rolls,
  from the server log), M3 (the log and the inserter with no checkpoint),
  M4 (half the batch), and the census (§6).
- **Added:** `iso0`, the isolated cell with the cadence off, to see
  whether the setting moves the floor by itself. It does not show that it
  does: 1.7 ms (1.6 ms without r3) against 1.4 ms, inside the arm's spread.
- **Cut:** nothing.
- **Not measured:** `cores = 1`, `group` durability, a quiet host.

## 2. V0: the reader's p99 with the cadence checkpoint off (M1)

| cell | runs | p50 (median) | p99 (median) | p99 range | p99.9 (median) | p99.9 range | stall samples, 5 runs |
|---|---|---|---|---|---|---|---|
| `isolated` | 5 runs | 328 µs | 1.4 ms | 1.4-2.4 ms | 4.5 ms | 4.4-5.3 ms | 0 samples |
| `iso0` | 5 runs | 326 µs | 1.7 ms | 1.1-2.5 ms | 4.6 ms | 4.1-5.6 ms | 1 sample |
| `levels` | 5 runs | 331 µs | 3.2 ms | 1.8-7.5 ms | 71.1 ms | 5.2-85.8 ms | 75 samples |
| `nockpt` | 5 runs | 318 µs | 2.3 ms | 1.6-2.6 ms | 5.2 ms | 4.7-5.7 ms | 1 sample |
| `half` | 5 runs | 327 µs | 4.6 ms | 3.9-22.4 ms | 90.4 ms | 7.8-302.7 ms | 145 samples |

| run | `isolated` p99 | `iso0` p99 | `levels` p99 | `levels` / `isolated` | `nockpt` p99 | `nockpt` / `isolated` | `nockpt` / `iso0` |
|---|---|---|---|---|---|---|---|
| r1 | 2.4 ms | 1.1 ms | 3.2 ms | 1.37x | 2.3 ms | 0.97x | 2.03x |
| r2 | 1.4 ms | 1.5 ms | 2.4 ms | 1.71x | 1.9 ms | 1.32x | 1.24x |
| r3 | 1.4 ms | 2.5 ms | 7.5 ms | 5.27x | 2.3 ms | 1.61x | 0.92x |
| r4 | 1.6 ms | 1.7 ms | 1.8 ms | 1.12x | 2.6 ms | 1.59x | 1.47x |
| r5 | 1.4 ms | 1.9 ms | 4.2 ms | 2.98x | 1.6 ms | 1.12x | 0.84x |
| median of 5 | 1.4 ms | 1.7 ms | 3.2 ms | 2.27x | 2.3 ms | 1.60x | 1.30x |

**CLA's proposed V0** (CZ-Q1 (a)): the median within 2x the isolated
median, and at least 4 of 5 pairs within 2x. `nockpt` meets both: 1.60x,
and 5 of 5.

**How firm the pass is.**
- **The default arm nearly meets the same bar on its clean cells.**
  Without r5, `levels` is 2.8 ms (median of 4), 1.99x the isolated median
  of 5 and 1.86x that of the same four runs, with 1 of 4 pairs above 2x.
  At p99, this run's gate tells the two arms apart by 2.3 ms against
  2.8-3.2 ms. The p99.9 column and §3 tell them apart plainly.
- The bar is 2x an isolated p99 that is itself 2.2x the probe's. Against
  the probe's isolated median (649 µs) `nockpt`'s 2.3 ms would be 3.5x.
  That comparison crosses two runs three hours apart and is context, not a
  verdict; within this run the two arms share the host's floor.
- Against `iso0`, the control with the same setting, the pairs are 2.03x,
  1.24x, 0.92x, 1.47x and 0.84x: 4 of 5 within 2x, and r3's denominator
  is the contaminated cell.
- What `nockpt` keeps over `isolated` is in the samples between 1 and
  10 ms: 60-100 a run against 53-79. No stall is among them. The reactors
  share physical cores with one another (the CPUs row), so a read core's
  sibling is busy when the walker and the inserter run; that may be this
  remainder, and it was not measured.

## 3. The read cores: polls and the stream latch (M1)

Sums over the read cores (1, 2, 3 under the partition) of the differences
between the driver's two control reads, which fall 4-337 ms after the
window's start and 41-1,255 ms after its end.

| cell | run | `system` polls over 10 ms | foreground polls over 10 ms | stream-latch wait | of it roll-blocked | roll-blocked waits | checkpoint runs on read cores |
|---|---|---|---|---|---|---|---|
| `levels` | r1 | 2 polls | 0 polls | 219.0 ms | 218.9 ms | 3 waits | 2 runs |
| `levels` | r2 | 2 polls | 0 polls | 701.5 ms | 701.4 ms | 3 waits | 2 runs |
| `levels` | r3 | 4 polls | 0 polls | 218.6 ms | 218.4 ms | 5 waits | 6 runs |
| `levels` | r4 | 1 poll | 0 polls | 19.2 ms | 19.0 ms | 4 waits | 5 runs |
| `levels` | r5 | 1 poll | 0 polls | 121.3 ms | 121.2 ms | 3 waits | 6 runs |
| `nockpt` | r1-r5, each | 0 polls | 0 polls | 0 µs | 0 µs | 0 waits | 0 runs |
| `isolated`, `iso0` | r1-r5, each | 0 polls | 0 polls | 0 µs | 0 µs | 0 waits | 8 and 0 runs |

- No statement or walk slice held a read core for 10 ms in any cell.
- With the cadence off the read cores' longest `system` poll since the
  server's start is 18-109 µs.
- `levels` r4 has no stall sample: its read cores waited 19.2 ms in all.

## 4. Each stall episode against the run it falls in (M2)

An episode is the stall samples that lie within 50 ms of one another; they
end together, when the core runs again. The reading is from timing alone,
against the one checkpoint run each episode overlaps (`tables.md` §2.1
states the rule for each and lists all 20).

| cell | episodes | at the begin record | inside a run, on a roll | at a run's end, on a roll | a page write, no roll | a publish-phase write, no roll | unread |
|---|---|---|---|---|---|---|---|
| `levels` | 6 episodes, 75 samples | 5 episodes, 62 samples | 1 episode, 13 samples | 0 episodes | 0 episodes | 0 episodes | 0 episodes |
| `half` | 13 episodes, 145 samples | 6 episodes, 72 samples | 3 episodes, 44 samples | 1 episode, 4 samples | 1 episode, 22 samples | 1 episode, 1 sample | 1 episode, 2 samples |
| `nockpt` | 1 episode, 1 sample | 0 episodes | 0 episodes | 0 episodes | 0 episodes | 0 episodes | 1 episode, 1 sample |

- **The begin record.** In 11 episodes the stall starts within 10 ms of a
  run's start and ends 0.4-1.4 ms after that run's `start` does (26-116 ms
  against 30-116 ms), which is 0.4-1.5 ms after a roll hold of 70-355 ms
  ends.
- **Inside a run.** 4 episodes of 79-366 ms; the longest lies inside a
  727 ms roll hold. The site is the park's flush, the end append or the
  inline gate; the run line does not say which.
- **A run's end.** `half` r2: the episode ends 1 ms after the run does
  and 4 ms after a roll. The recycle at the anchor's commit is the run's
  last act, so this is most likely C1c's site (inferred).
- **A page write.** `half` r4: one page write of 167.0 ms in core 3's
  run, a 164.0 ms episode of 22 samples, no roll hold within 360 ms. Core
  0's run in `half` r2 has one of 226.4 ms. No run of `levels` has a page
  write of 10 ms.
- **A publish-phase write.** `half` r5: one 12.3 ms sample, in a run whose
  longest publish-phase call was 17.2 ms and whose longest page write was
  50 µs. That phase holds a batch of page writes, a map write or an inline
  gate; which one is not known.
- **Every run.** `tables.md` §5 lists each checkpoint run of `levels`
  with its wall time, parks, `start`, inline gate and the rolls it
  overlaps.
- **Whose core.** The driver keeps a reader's first and last core, and the
  balancer moves readers inside the window, so a stalled reader's core at
  the sample is not known (`tables.md` §2). The `system` polls over 10 ms
  are on the run's core in every case checked (`levels` r2, `half` r2 and
  r4). In `levels` r2 the only two session moves in the window are out of
  core 3, which fits two stalled readers leaving after the begin stall.

**A core's latch wait against the latch sites its runs time** (`tables.md`
§5.1, §5.2). The begin append is inside `start` and the inline gate is
`log_gate_*`, so their sums bound what those two sites waited. An inline
gate taken inside the publish is hidden in the publish's writeback phase,
so the last column takes that phase out too.

| cell | read cores' stream-latch wait, 5 runs | their runs' `start` sum | their inline-gate sum | not at a timed latch site, at least | the same, less the publish's writeback |
|---|---|---|---|---|---|
| `levels` | 1,279.5 ms | 449.1 ms | 42.5 ms | 810.4 ms | 757.7 ms |
| `half` | 1,121.3 ms | 454.3 ms | 68.4 ms | 643.0 ms | 604.4 ms |

The largest single item is core 3 in `levels` r2: 700.3 ms waited, 78.9 ms
of it in `start`, and one `system` poll of 623.1 ms - with no reader
sample stalled in it. A wait stalls a reader only if a reader is on that
core then.

## 5. The rolls, the log and the inserter (M1's confound, M3, M4)

| cell | rolls, 5 windows | hold p50 | hold p90 | hold max | holds over 300 ms | window under a roll hold | log rate (median) | inserter (median) | live segments at the window's end |
|---|---|---|---|---|---|---|---|---|---|
| `levels` | 57 rolls | 246.6 ms | 352.7 ms | 746.5 ms | 14 rolls | 14-34 % | 73.1 MB/s | 159,955 rows/s | 6-14 segments |
| `nockpt` | 60 rolls | 240.2 ms | 253.9 ms | 260.4 ms | 0 rolls | 28-29 % | 80.5 MB/s | 172,528 rows/s | 15 segments, every run |
| `half` | 55 rolls | 215.3 ms | 354.8 ms | 1,269.4 ms | 13 rolls | 22-26 % | 72.1 MB/s | 155,051 rows/s | 7-14 segments |

- **M1's confound.** The typical roll is as long without the checkpoint;
  the long ones are not there. The sync gate is 95.4 % of the hold in
  `levels` and 99.0 % in `nockpt`. So the reader's gain cannot be credited
  to shorter rolls at the median, and part of the tail's gain may be the
  long rolls' absence. The 11 begin-record episodes waited out holds of
  70-355 ms, a range `nockpt` has too.
- **M3.** With no cadence checkpoint the anchor stays at the mount's and
  no segment is removed: 15 segments (960 MiB) at the window's end,
  growing with the log. The cadence-off arm is a probe, not a setting to
  run with. `live segments` is derived from the log's lines, not read
  from the device (`tables.md` §4).

## 6. The census `[source-read]` at `a4d2fb3b`

Read on worktree `cz-checkpoint-roll-wait` at `a4d2fb3b`. No engine code
was changed.

### 6.1 Who takes the stream latch on a reactor thread, on a core with no write

The stream has four latched entries: `Append` and `AppendAndSeal`
(`src/wal/stream.cpp:392`, `:398`), `Flush` (`:476`), `Seal` (`:332`) and
`DetachBelow` (`:338`). `Flush` takes the latch even when the ring is
empty; only `WalManager::Flush` tests the ring first
(`src/wal/manager.cpp:113`). `Sync`, `RequestSyncNow` and an attached
manager's `Sync` call the stream's `Flush` with no such test (`:220`,
`:259`, `:183`).

| taker | reached on a core with no write? | read at |
|---|---|---|
| the cadence checkpoint task, at the order's five kinds of site | yes, on every core in rotation | `src/wal/checkpointer.cpp:46`, `:142`, `:163`, `:338`, `:640`, `:581`; `src/storage/device_page_store.cpp:1898`, `:2161` |
| a peer's drain tick | **no** at `relaxed` with no group commit parked: an attached manager returns before any flush (`DrainOnce`); with a group commit parked it asks through `RequestSyncNow`, which is a write's | `src/wal/manager.cpp:470-536`, `src/server/core_runtime.cpp:409` |
| a writeback's WAL gate outside a checkpoint: a page miss that evicts a dirty frame | yes in principle, on any core that faults into a full pool: `EnsureDurable` flushes under the latch and then blocks on the writer's condition variable. **Not reached in this cell**: the pool never filled (20,227 to 24,653 pages resident at the closing read, of 65,536 frames; the window's misses, 15,637 to 19,800, equal the growth) | `src/storage/device_page_store.cpp:1898`, `:2161`; `src/wal/manager.cpp:183-189` |
| a commit, a `SYNC`, a durability frame | a write's or an explicit statement's; none in a reader's path | `src/server/command_dispatcher.cpp:983`, `:1276`, `:1304`, `:1797`, `:5595`, `:12759` |
| the recycle | only from an anchor's commit, which is the checkpoint's | `src/server/expeditor.cpp:1257-1262`, `src/server/superblock_checkpoint_anchor.cpp:205` |

**Not read:** every caller of `WalManager::Append` outside `src/wal/`. A
plain `SELECT`'s path was not walked line by line for a logged write, nor
were the instance's background jobs (the index job, the dropped-relation
reclaim, the Cabin controller). What stands in for that reading is §3's
measurement: with the cadence off, the read cores waited 0 µs on the
stream latch in every run, with 12 rolls a window. That bounds waiting in
this cell. It does not show that nothing else takes the latch.

**Three blocking waits that are not the latch**, on the same paths:
- `Flush` spins on `std::this_thread::yield()` while another thread's
  unlatched write is in flight (`WaitOutFlushInFlight`,
  `src/wal/stream.cpp:512-514`, called at `:493`). The per-core
  stream-latch wait does not count it.
- The park's flush writes the instance's staged ring to the device on the
  read core's reactor (`src/wal/stream.cpp:499-500`).
- An attached manager's `Sync` blocks on the writer's condition variable
  until the sync lands (`src/wal/manager.cpp:189`). This is the inline
  gate, which CZ-R5 keeps.

**The recycle runs through core 0's manager on the publishing core's
thread.** The recycler is wired with `expeditor->wal_`
(`src/server/expeditor.cpp:1257`), so a peer's anchor commit calls core
0's `RecycleBelow` and takes the latch in `DetachBelow` on the peer's
reactor.

### 6.2 What covers the time between a begin record's snapshot and its append (CZ-R3)

`Start` takes the active-transaction and dirty-page snapshots and then
appends (`src/wal/checkpointer.cpp:195-198`). The two are covered
differently.

- **The active-transaction table is covered by the poll.** The table is
  the checkpointing core's own (`TransactionManager::Snapshot`,
  `src/txn/manager.cpp:675`; `wal.md` §11), and `Start` is one
  synchronous call in one poll of the run task
  (`src/wal/checkpointer.cpp:472-474`). A latch wait inside it blocks the
  thread and yields nothing, so no transaction of that core begins,
  writes or decides between the snapshot and the append. Another core's
  transactions are not this record's to carry: each is seeded by its own
  core's begin record.
- **The dirty table is not covered by the poll at `cores > 1`.** The pool
  is the instance's, so another core can dirty and log a page between the
  snapshot and the append. That page has a recLSN below `begin_lsn_` and
  is absent from the snapshot. The comment in `Start` ("Pages dirtied
  *after* this point need no special handling: their recLSNs are
  necessarily above begin_lsn_", `:221-223`) holds from the append on, and
  not for this interval. Two things cover it:
  1. **The fold.** The fold is the minimum redo start over every core's
     latest completed checkpoint, and the anchor on disk is the higher of
     the fold and the highest anchor already encoded
     (`include/kds/server/superblock_checkpoint_anchor.hpp:50-60`,
     `:97-101`; `src/server/superblock_checkpoint_anchor.cpp:100-105`).
     The core that appended in the gap last completed a checkpoint before
     this one began, so its own contribution lies below the record. Its
     next run snapshots on its own thread, after its stamp, and carries
     the page if it is still dirty. The clamp cannot lift the anchor past
     the record either: an earlier fold above it would need that core's
     own later snapshot to lack the page, which means the page was
     written. **This rests on no core yielding between an append and its
     stamp**, which `wal.md` §11-2 states as the frame holding a record's
     change when the record is appended, or a page hold kept to the stamp.
  2. **The publish.** Before the anchor's page is written, the cadence's
     publish writes every page dirty at that instant
     (`target_.DirtyPages()`, `src/wal/checkpointer.cpp:539`), and a
     synchronous publish flushes and syncs the whole store. A frame is
     dirty from its write fetch, before the append, not from the stamp
     (`src/storage/device_page_store.cpp:1079`).

  Analysis adds nothing of its own here: it enters every page a record in
  its scan names (`src/wal/analysis.cpp:156-176`), but it sees the gap's
  record only if the scan starts at or below it, which is cover 1.

**No live defect was found**, so no `docs/inflight/bugs/` entry is
written. The review tried four schedules against today's code (a record
logged in the gap on a clean page; a writeback cleaning the page between
the append and the stamp; the fold stepping back under the clamp; a commit
in the gap) and none loses a change. What was not read: whether any thread
other than a core's reactor appends a page record. Cover 1 argues from the
appending core's own checkpoint; cover 2 does not depend on who appended.

**`Start` has a third snapshot.** The assertion bases
(`LogAssertionSnapshots`, `src/wal/checkpointer.cpp:210`, `:174-183`) are
taken under the assertion directory's latch, which is held across their
appends (`src/exec/assertion_check.cpp:284-288`). A park at one of those
appends (`:142`, `:163`) would either hold that latch across the park,
which CZ-R9 forbids, or let the bases drift from the instant they
describe. CZ-S2 owes this site a design of its own.

**What this means for C1b.** A park between the snapshot and the append
lets the checkpointing core's own transactions act in the gap, and nothing
covers that:
- a transaction that begins and writes during the park is in no begin
  record, and analysis learns of it only if its records lie inside the
  scan;
- a transaction listed in the snapshot that commits during the park is
  the case `TransactionManager::Snapshot` already guards against for a
  deferred commit (`src/txn/manager.cpp:683-688`): a scan starting above
  its commit record reads the begin record's entry as a loser and undoes a
  committed transaction;
- cover 1 no longer covers that core's own page records, and the publish
  alone carries them.

So CZ-R3's retake is required and "unless CZ-S0 shows the gap is already
covered" does not apply: the cover is the poll, and C1b removes it. An
alternative for CZ-S2 to read is to try the latch **before** the snapshot
and append under the hold taken `[design]`. It is unread: `Append` takes
the latch itself, the latch is not recursive, and a ring-full retry calls
`Flush` (`src/wal/manager.cpp:389-396`), so it needs an entry the stream
does not have today.

**The comment in `Start`** is wrong for the interval above and no engine
code changes in S0. CZ-S3 corrects it when it touches the site; if the
order closes before S3, the close does, in a comment-only change.

### 6.3 Where a reader-only core's dirty pages come from

They are the instance's. `PageStoreCheckpointTarget::DirtyTable` walks
every frame of the one pool
(`include/kds/storage/page_store_checkpoint_target.hpp:35-41`,
`src/storage/device_page_store.cpp:2347-2356`), and `wal.md` §11 states it:
"the dirty set is the instance's; the active-transaction table is not." A
read core's cadence run therefore snapshots and writes the pages the
inserter dirtied on core 0. In `tables.md` §5, over the five runs of
`levels`, core 3's runs wrote 311 to 10,610 pages each and core 2's 993
to 2,640, with the `pwrite`s on the read core's reactor.

This matters for V0's reading. Turning the cadence off takes away the
latch wait **and** this writeback from the read cores, and §4 shows both
stall a reader: in `levels` no page write of a run reached 10 ms, and in
`half` one episode is a page write of 167 ms with no roll beside it.

### 6.4 BQ's rung-2 ceiling re-read against the order's §1.1

`bench/v3.0.0/results-bq-s1-segment-roll-v2.7.0-909-g4cf30270.md` §6 and
§10 ("Upper bounds these data put on each rung"). The two ceilings take
out different samples, and BQ's own text says what its remainder is.

| | BQ-S1's H5 bound | the order's §1.1 |
|---|---|---|
| taken out | reader stalls **aligned with a roll**: overlapping it and completing within -5 to +30 ms of its end | reader samples of 1 ms or more that overlap a checkpoint run (or, separately, a roll hold) at all |
| left in | stalls inside a checkpoint run that do not end with a roll. BQ: "57-90 % of the large reader stalls that do *not* line up with a roll fall inside a checkpoint run (1.1-4.8 s each at `relaxed`): a second holder, indicative only, which the H5 ceiling (2.6-145 ms p99) reflects" | whatever queued behind a sample taken out |
| what it bounds | a drain that does not block on a roll (rung 2). In BQ's cells core 0's drain carried 94-99 % of the hold | a checkpoint that does not block |
| placement | driver-forced (`segregated-no0`, `batch0`, `spread`), 3 runs of 30 s | the balancer's, under BL-S4's partition, 5 runs of 10 s |
| commit, host | `4cf30270`; an AMD EPYC 9V74 on Azure | `67973ebc`; an Intel Xeon Platinum 8581C |

The checkpoint was already a parking task at `4cf30270` (`RunTask` and
`ParkOnLog` are in that tree), so BQ's "second holder" is consistent with
the mechanism CZ names, seen there as a remainder. BQ marks it indicative
only, and its data cannot separate a latch wait from a page write. The two
numbers do not conflict: BQ's is what is left when the drain's stalls go
and the checkpoint's stay, and §1.1's is what is left when the
checkpoint's go. This is a reading of BQ's file, not a re-measurement of
its cells.

## 7. What CZ-S1's spike must count

The run line times the begin append (`start`) and the inline gate
(`log_gate_*`). It does not time three latch sites, and §4 shows at least
810 ms of latch wait at them in `levels`. The spike counts, per run and
per site:

1. the park's flush (`RequestSyncNow` in `ParkOnLog`): tries, busy
   answers, and the time each blocking call took;
2. the end append;
3. the recycle at the anchor's commit;
4. the inline gate's count and time, as today (CZ-Q10 (a)), and
   separately when it is taken inside the publish, where today's phase
   timer hides it;
5. a run's longest page write and publish write, as today, read beside
   the stalls: C1 leaves them.

And for the cell: the reader's core at each sample, which the driver does
not keep. Without it an episode is tied to a run by time only.

## 8. What this file does not establish

- That C1 returns what the cadence-off arm returned. The arm removes the
  checkpoint whole: its page writes go and the long rolls with them.
- The probe's 19x. `levels` is 2.0-2.3x here, on a host with a higher
  floor and fewer stalls a run, and whether the 19x would recur on a quiet
  host is not known.
- Which untimed site carries the 810 ms.
- Anything at `cores = 1` (no stream latch) or at `group`.
- That nothing else takes the stream latch on a read core: §3 shows that
  nothing else **waits** on it in this cell, and §6.1 says what was not
  read.
