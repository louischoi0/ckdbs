# BL's ruling 2, re-measured: a point reader's tail is decided by whether it shares core 0

**Thesis.** BL-S4 has been stopped since 2026-10-12 on ruling 2, which asks
what role core 0 takes under the interactive/batch partition. BL-R2 says
"core 0 is never a batch core", and CLA's proposal of 2026-10-12 was "core 0
first". This file re-measures BL's C1 cell at `v3.4.0-22-g418670a3`, on the
operator's choice on 2026-10-21 to re-measure before ruling. **With
placements held** (a measurement build with the balancer switched off, §0),
the readers' tail is set by one fact: whether any reader is on core 0.

| placement (held) | where the readers are | reader schedule p99, r1 / r2 / r3 | median | p99.9 median |
|---|---|---|---|---|
| `segregated-no0` | cores 1-2; batch on 3; **core 0 empty** | 0.9 / 1.4 / 48.2 ms | **1.4 ms** | 3.8 ms |
| `batch0` | cores 1-3; **batch on core 0** | 70.9 / 1.8 / 73.3 ms | **70.9 ms** | 207.6 ms |
| `segregated` | **cores 0-2**; batch on 3 | 461.8 / 478.0 / 501.3 ms | **478.0 ms** | 746.0 ms |
| isolated (no batch) | cores 1-2 | 0.72 / 0.48 / 0.56 ms | 0.56 ms | 2.3 ms |

- **A reader on core 0 waits out every roll.** Each run wrote 71-77 MB/s
  of log and rolled 10-12 segments in its 10 s window. A roll holds the
  stream latch for 4.0-4.8 s per 10 s, 97-99 % of it waiting at the sync
  gate. Whenever core 0 has no appender of its own, its group-commit drain
  waits on that latch for 4.0-4.7 s per 10 s. The readers sharing core 0's
  reactor pay for it: their schedule p99 is 462-501 ms, and the ranges do
  not touch either of the other two arms.
- **A bulk writer on another core costs readers off core 0 little by
  comparison.** In `segregated-no0` the walker and the inserter share
  core 3, no reader shares a core with either, and no reader is on core 0:
  the median p99 is 1.4 ms, 2.5x the isolated arm's. Two of three runs are
  within 2.5x, and r3 is 48 ms. No held arm puts a reader on a batch
  session's core, so that case is not measured here.
- **Batch on core 0 and an empty core 0 are not separated.** With the batch
  sessions on core 0, the inserter there is the session that rolls (core 0
  carries all of the sync-gate wait), so the drain on the same thread never
  waits for a roll. Cores 2 and 3, which hold readers and no batch session,
  still wait 0.17-0.45 s on the roll (§1.3). **The ranges of `batch0` and
  `segregated-no0` overlap at p99** (1.8-73 ms against 0.9-48 ms) **and at
  p99.9** (130-220 ms against 3.4 / 3.8 / 226.9 ms). The `segregated-no0`
  r3 outlier has the same signature as `batch0`'s tail: 0.25 s of roll wait
  on core 2, a reader-only core, with no drain wait there. So an empty core
  0 does not keep the roll off every reader core either. What takes the
  stream latch there is not identified (the checkpoint task, which rotates
  over cores, is one candidate).
- **The shipped balancer (BL-S3) undoes every placement.** With the
  balancer live, a placement is only a starting point: 9-83 moves per run.
  8 of 9 balanced runs ended with a reader on core 0. The balancer splits
  the two batch sessions onto separate cores, each reading 0.76-0.99 busy,
  and the readers fill the cores left over. Nothing in `LoadPlacer` knows
  that core 0's reactor runs the drain. The balanced schedule p99 is
  291-581 ms in every starting placement, except one run (`spread` r2, 1.0
  ms), **the only balanced run that ended with no reader on core 0**.
- **Holding the two batch sessions on one core costs the walker 13x.** It
  completed 1,535-1,575 statements beside the non-yielding inserter,
  against 17,874-21,453 when the balancer gave it a core of its own. This is
  BL-R2's batch partition as designed (a batch session runs on what the
  batch cores have), and it is priced here, not judged.

**What this means for ruling 2.** **BL-R2 as written is refuted.** It
keeps core 0 interactive, and readers on core 0 are the 478 ms arm, whose
range touches neither other arm. **"Core 0 first" (CLA's proposal of
2026-10-12) meets the condition that protects readers**, which is that
core 0 hosts no interactive session while a bulk writer runs. BL-S4's rule
that `decide` never moves a `normal` session onto a batch core enforces
that while a batch-level session is live. **Three runs do not show whether
an empty core 0 beats it.** An empty core 0 costs one core in four at
`cores = 4`; its medians are lower, but the ranges overlap at p99 and
p99.9.

- **Order**: `instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`
  §6 ("BL resumed 2026-10-21"); `instructions/v3.2.0/raft-marks-2026-10-21.md` §11.
- **Driver**: `tools/bl_mixed.py` at `418670a3`, unchanged; run by
  `matrix.sh` and `matrix2.sh` in the archive.
- **Raw output**: `archive/bl-ruling2-remeasure-v3.4.0-22-g418670a3/`. It
  holds the driver JSON and server log per run (`r1..r3/` for the shipped
  binary, `nobal/r1..r3/` for the measurement build), the console logs, the
  two matrix scripts, `nobal.patch`, and the table scripts (`tables.py`,
  `rolls.py`, `rolls_nobal.py`) with their rendered output (`tables-*.md`,
  `rolls-*.md`).

| Item | Value |
|---|---|
| Executed | 2026-10-21, **23:38:59-23:45:45 UTC** (shipped binary, 12 runs) and **23:47:18-23:50:40 UTC** (measurement build, 9 runs) |
| Version | `v3.4.0-22-g418670a3` (`418670a3`, `origin/main` at the time of the run), worktree `bl-ruling2-remeasure` |
| Binaries | Release (`CMAKE_BUILD_TYPE=Release`), each run started from a copy. **Shipped**: `kds_server.418670a3`, sha256 `979a4aae671c53d046dfea1e0d92d27ea37b9b2de8ef09bb222aba255ed86b59`. **Measurement build**: `kds_server.418670a3-nobal`, sha256 `08cebc4b41804c13be4a63c1ef6cd3302ebf73e1d49dba4783e00aee84036bdf`. It is `418670a3` plus `nobal.patch`: `LoadPlacer::Place` returns the session's own core when `KDS_BL_NO_BALANCE` is set. The patch was never committed, and the source was restored after the build |
| Device | `/dev/root`, ext4 (Azure VM NVMe), not tmpfs (recorded per run) |
| Host | 8 vCPU (AMD EPYC 9V74), Linux 7.0.0-1014-azure; servers on CPUs 0,2,4,6, clients on 1,3,5,7 (the reactors' SMT siblings). Before every run the driver gated on a 1-minute load at or under 1.5: before-run load 0.70-1.50, waits 0-75 s. Two unrelated long-lived `kds_server` processes (autotrade, sandbox) were resident and idle throughout. No compile or test run was active during either matrix |
| Server configuration | `cores = 4`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = info`, `reactor_cpus = 0,2,4,6`; every other key default (64 MiB segments) |
| Cell | C1, as in `results-bl-ruling1-remeasure-v2.7.0-909-g4cf30270.md` §0: a walker (an unindexed one-row `SELECT` over 10,000 wide rows, closed loop), an inserter (1,024-row `INSERT`, closed loop), six open-loop pk point readers at 400/s in total; 10 s measured after 2 s of warm-up; a fresh server and data file per run |
| Runs | Shipped: {isolated `segregated-no0`, mixed `segregated-no0`, `spread`, `batch0`} x 3, interleaved by pass. Measurement build: mixed {`segregated-no0`, `segregated`, `batch0`} x 3, interleaved by pass. **Cut to fit the 20-minute stage budget**: `durability = group` was not run, nor any core count other than 4, nor an isolated arm on the measurement build (the readers' isolated placement is the same `segregated-no0` either way, and the shipped isolated runs saw 0-1 moves) |
| Baseline | This engine's previous number for this cell: `results-bl-ruling1-remeasure-v2.7.0-909-g4cf30270.md` (`05cd011f`, balancer live). The PostgreSQL floor was not measured for this shape (no core placement exists to compare) |
| Suite / overhead | Suite not executed (no committed code changed). BL's overhead A/B is still owed at BL's close |

## 1. Placements held (measurement build)

### 1.1 Reader schedule latency, median of three runs

| placement | runs | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| segregated-no0 | 3 | 125 µs | 177 µs | 185 µs | 267 µs | 1.4 ms | 3.8 ms | 8.8 ms |
| batch0 | 3 | 126 µs | 182 µs | 192 µs | 286 µs | 70.9 ms | 207.6 ms | 237.4 ms |
| segregated | 3 | 137 µs | 195 µs | 207 µs | 268.2 ms | 478.0 ms | 746.0 ms | 768.4 ms |

### 1.2 Reader service latency, median of three runs

| placement | runs | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| segregated-no0 | 3 | 89 µs | 109 µs | 116 µs | 170 µs | 593 µs | 3.0 ms | 8.7 ms |
| batch0 | 3 | 55 µs | 113 µs | 121 µs | 184 µs | 714 µs | 7.7 ms | 237.3 ms |
| segregated | 3 | 73 µs | 118 µs | 130 µs | 184 µs | 1.4 ms | 448.9 ms | 768.3 ms |

The service p99 is under 1.5 ms in every arm: a reader's statement does not
wait in itself. The gap between service and schedule is the reactor the
reader shares. Every run is in `tables-nobal.md`.

### 1.3 Where the wait went

| placement | run | rolls | roll hold | hold max | sync-gate wait | roll-blocked wait c0 / c1 / c2 / c3 | session cores (6 readers, walker, inserter) | log MB/s | schedule p99 |
|---|---|---|---|---|---|---|---|---|---|
| segregated-no0 | r1 | 10 | 4.16 s | 171 ms | 4.09 s | 4.10 s / 0.00 s / 0.00 s / 0.00 s | 1, 2, 1, 2, 1, 2, 3, 3 | 77 | 0.9 ms |
| segregated-no0 | r2 | 12 | 4.74 s | 139 ms | 4.62 s | 4.69 s / 0.00 s / 0.01 s / 0.00 s | 1, 2, 1, 2, 1, 2, 3, 3 | 71 | 1.4 ms |
| segregated-no0 | r3 | 11 | 4.01 s | 0 ms | 3.93 s | 3.97 s / 0.00 s / 0.25 s / 0.00 s | 1, 2, 1, 2, 1, 2, 3, 3 | 75 | 48.2 ms |
| segregated | r1 | 12 | 4.75 s | 352 ms | 4.67 s | 4.67 s / 0.00 s / 0.30 s / 0.00 s | 0, 1, 2, 0, 1, 2, 3, 3 | 73 | 461.8 ms |
| segregated | r2 | 11 | 4.40 s | 352 ms | 4.37 s | 4.37 s / 0.00 s / 0.00 s / 0.00 s | 0, 1, 2, 0, 1, 2, 3, 3 | 76 | 478.0 ms |
| segregated | r3 | 11 | 4.38 s | 269 ms | 4.30 s | 4.34 s / 0.00 s / 0.19 s / 0.00 s | 0, 1, 2, 0, 1, 2, 3, 3 | 76 | 501.3 ms |
| batch0 | r1 | 11 | 4.54 s | 310 ms | 4.45 s | 0.00 s / 0.00 s / 0.05 s / 0.45 s | 1, 2, 3, 1, 2, 3, 0, 0 | 74 | 70.9 ms |
| batch0 | r2 | 11 | 4.48 s | 122 ms | 4.44 s | 0.00 s / 0.00 s / 0.00 s / 0.17 s | 1, 2, 3, 1, 2, 3, 0, 0 | 74 | 1.8 ms |
| batch0 | r3 | 11 | 4.60 s | 138 ms | 4.55 s | 0.00 s / 0.00 s / 0.24 s / 0.22 s | 1, 2, 3, 1, 2, 3, 0, 0 | 75 | 73.3 ms |

The roll itself does not depend on placement: 4.0-4.8 s of latched hold per
10 s in every row. **Placement decides only who waits for it.** In
`segregated-no0` the waiter is core 0's drain, with no reader behind it. In
`segregated` it is the same drain, with two readers behind it. The roll is
the inserter's own append: the sync-gate wait, 3.9-4.7 s per 10 s, is all
on the inserter's core in every row (core 3, or core 0 in `batch0`). A
core's roll-blocked wait counts any stream-latch wait on that core
(`ChargeRollOverlap`, `src/wal/stream.cpp`), so the 0.17-0.45 s on cores 2
and 3 in `batch0`, and the 0.25 s on core 2 in `segregated-no0` r3, are
waits on reader-only cores whose source this file does not identify.
`hold max` is the longest single roll. Its 0 ms in `segregated-no0` r3
is the counter as read and is not explained here: that run's total hold is
4.01 s.

### 1.4 Batch sessions

The inserter ran 158-170k rows/s in every arm, and the walker completed
1,535-1,575 statements: the two share one core in every held placement.

## 2. The shipped binary (balancer live)

| placement (start) | schedule p99 r1 / r2 / r3 | median | p99.9 median | moves (run) | session cores at end | readers on core 0 at end |
|---|---|---|---|---|---|---|
| segregated-no0 | 485.9 / 581.0 / 290.7 ms | 485.9 ms | 686.8 ms | 24 / 17 / 83 | e.g. 3,3,3,3,0,3,2,1 | 1 / 1 / 1 |
| spread | 358.4 / **1.0** / 426.8 ms | 358.4 ms | 691.8 ms | 9 / 13 / 28 | r2: 1,1,1,1,1,1,3,2 | 1 / **0** / 1 |
| batch0 | 433.0 / 327.8 / 433.1 ms | 433.0 ms | 632.7 ms | 13 / 19 / 20 | e.g. 3,3,3,0,3,3,1,2 | 1 / 1 / 1 |
| isolated (segregated-no0) | 0.72 / 0.48 / 0.56 ms | 0.56 ms | 2.3 ms | 1 / 0 / 0 | - | - |

At the mid-run sample, each core holding a batch session reads 762,127-994,212
`busy_ppm`, and the others read 1,730-47,973, except core 0 in
`segregated-no0` r3 (528,173, the run with 83 moves). The balancer separates the
two batch sessions, which sit on cores 1-3 at the end of every run, and the
readers spread over the rest, core 0 among them. Every run, with every
percentile, is in `tables-balanced.md`, and the roll table is in
`rolls-balanced.md`.

**Against this engine's previous number** (`05cd011f`, v2.7.0-909, the
same cell with the balancer live): schedule p99 medians 277 / 288 / 295 ms
(segregated-no0 / spread / batch0) then, 486 / 358 / 433 ms now. The run
ranges overlap in every placement (then 240-455, 248-290 and 293-337 ms;
now 291-581, 1.0-427 and 328-433 ms; batch0's by 9 ms), so **no change in
reader p99 is established**. The batch sessions did change, and their
ranges are disjoint: the inserter ran 171,929-176,142 rows/s then and
152,046-167,159 now (about -10 %), and the walker completed 6,668-10,274
statements then and 17,874-21,453 now. This file does not attribute either
change. Neither file holds the readers on a placement, so neither measures
one.

## 3. What is not measured

- `durability = group`, where BQ measured a roll's hold at ~6 ms p50 and
  ~20 ms p99: the core-0 effect there is expected to be small and was not
  run.
- `cores` 2 and 8. At `cores = 2`, keeping core 0 free of interactive
  sessions leaves one core for all of them.
- Whether `segregated-no0` and `batch0` differ at p99 or p99.9: their
  ranges overlap at three runs.
- A partition built in the engine (BL-S4): every held placement here is
  the driver's, with the balancer switched off by an uncommitted patch.
