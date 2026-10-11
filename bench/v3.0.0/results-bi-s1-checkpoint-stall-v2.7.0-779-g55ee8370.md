# BI-S1 — what a checkpoint run does to its core's reactor (`v2.7.0-779-g55ee8370`)

**A**, the comparator BI-Q10 names: the build-release of `55ee8370`
(`v2.7.0-779-g55ee8370`), BI-S1's instrumentation over `main` at
`0e6e7d8d`, with no behaviour change. Measured 2026-10-11 12:48-15:11 UTC,
by `instructions/v3.0.0/workorder-bi-checkpoint-off-the-reactor.md` §5.1.

## How it ran

- **Binary**: a copy, `/home/cdkbs/bench-runs/bi-s1-v2.7.0-779-g55ee8370/kds_server`,
  sha256 `bd8ffa1fa32b12e731eb346cdc54943a4e7158d5dd6ccd168a0a2abc7254ce18`,
  every server started from it.
- **Device**: the data file and the log on `/dev/root`, ext4 (`df -T` per
  cell, in each cell's JSON).
- **Config**: `checkpoint_interval_ms = 5000`, `buffer_pool_frames =
  786432` (the bug entry's), `reactor_cpus` = CPUs `0..cores-1`, everything
  else default; a fresh data file per cell.
- **Driver**: `tools/bi_checkpoint_stall.py` (BI-S1's own; no `tools/`
  driver had this shape). Four load processes, each a loop of 1024-row
  named-key `INSERT`s into one btree over its own ascending key range. One
  statement probe (a pk point `SELECT` on a separate warm 1,000-row
  relation) and one `C_PING` probe per core, open loop at 20 ms: latency
  counts from the scheduled send, so a probe that falls behind accumulates.
  A sample is *in* a run when its scheduled time falls inside one of its own
  core's runs, by the server's per-run Info line (CLOCK_MONOTONIC on both
  sides).
- **Cells**: `cores` {1, 4} x `durability` {`relaxed`, `group`}, 600 s
  each, three runs per cell, interleaved r1 then r2 then r3 over the four
  shapes. Invocation: `archive/bi-s1-checkpoint-stall-v2.7.0-779-g55ee8370/run.sh`.
  Raw cell JSON (72 MB, every sample) stays at
  `/home/cdkbs/bench-runs/bi-s1-v2.7.0-779-g55ee8370/cells/`; the per-core
  tables are `archive/.../summary.md`.

**What departs from the rules, stated:**

- **The driver changed during the measurement stage**, three times, each to
  fix the driver and never the shape: the run-line regex missed a field
  (the first, aborted pass parsed 0 runs; restarted from scratch); a load
  worker died on a `retryable=1` refusal instead of retrying (fixed before
  the restart); and a retried batch refused as a duplicate killed a worker
  (fixed after `c4-relaxed-r1` lost one, from `c1-group-r1`'s successor on).
  The last is an engine defect, below.
- **`c4-relaxed-r1` was run last** (`rerun.sh`), after the twelve-cell pass,
  because its first attempt lost a load worker; the interleave is broken for
  that one cell.
- **`c4-relaxed-r3` overlapped another session's load**: 1-minute load 2.06
  at its start, 13.02 at its end (`/proc/loadavg` per cell). Its numbers are
  reported and not used for a conclusion. The host carries other sessions;
  `pgrep` of build processes was not recorded per cell, only the load
  average and `/proc/pressure/io`.

## The run matrix (cores pooled per cell)

| cell | rows inserted | runs | run p50 / max | longest iteration hold in / out of a run | iterations > 100 ms in / out | iterations > 1 s in a run | statement probe p50 in / out | statement probe p99 in / out | C_PING p99 in / out | worst core's anchor gap | time charged to no group / reactor wall |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c1-relaxed-r1 | 54.5 M rows | 120 runs | 828 ms / 1404 ms | 1404 ms / 521 ms | 120 / 639 iterations | 27 iterations | 646.6 ms / 189.45 ms | 1253 ms / 1259.1 ms | 1242 ms / 1233.2 ms | 5.68 s | 145 s / 601 s |
| c1-relaxed-r2 | 54.1 M rows | 120 runs | 826 ms / 1421 ms | 1421 ms / 647 ms | 120 / 638 iterations | 24 iterations | 617.8 ms / 185.76 ms | 1269 ms / 1266.6 ms | 1257 ms / 1237.0 ms | 5.68 s | 142 s / 601 s |
| c1-relaxed-r3 | 42.4 M rows | 120 runs | 656 ms / 2005 ms | 2005 ms / 812 ms | 120 / 489 iterations | 23 iterations | 644.3 ms / 409.17 ms | 1568 ms / 1407.1 ms | 1458 ms / 1321.6 ms | 5.95 s | 125 s / 601 s |
| c4-relaxed-r1 | 45.5 M rows | 68 runs | 9264 ms / 17697 ms | 17697 ms / 2338 ms | 69 / 2219 iterations | 68 iterations | 141039.3 ms / 532.08 ms | 428024 ms / 260443.0 ms | 428015 ms / 57442.1 ms | 114.49 s | 656 s / 2419 s |
| c4-relaxed-r2 | 57.8 M rows | 107 runs | 5388 ms / 11106 ms | 11106 ms / 1452 ms | 108 / 2613 iterations | 107 iterations | 172995.8 ms / 250.36 ms | 384796 ms / 286989.7 ms | 384226 ms / 70648.0 ms | 82.65 s | 644 s / 2417 s |
| c4-relaxed-r3 (contended) | 28.5 M rows | 52 runs | 7560 ms / 78946 ms | 78969 ms / 18482 ms | 54 / 1507 iterations | 53 iterations | 127504.3 ms / 2058.43 ms | 241168 ms / 310718.3 ms | 148224 ms / 152246.3 ms | 288.66 s | 861 s / 2438 s |
| c1-group-r1 | 43.9 M rows | 120 runs | 555 ms / 1126 ms | 1126 ms / 459 ms | 120 / 530 iterations | 2 iterations | 485.9 ms / 90.64 ms | 938 ms / 891.0 ms | 911 ms / 860.8 ms | 5.66 s | 457 s / 601 s |
| c1-group-r2 | 43.5 M rows | 120 runs | 685 ms / 1130 ms | 1141 ms / 543 ms | 120 / 579 iterations | 2 iterations | 578.5 ms / 153.22 ms | 983 ms / 1079.6 ms | 942 ms / 1053.5 ms | 5.56 s | 459 s / 601 s |
| c1-group-r3 | 26.7 M rows | 119 runs | 690 ms / 12753 ms | 12753 ms / 1188 ms | 116 / 736 iterations | 40 iterations | 44750.6 ms / 36936.36 ms | 73902 ms / 74499.7 ms | 70793 ms / 64594.4 ms | 17.07 s | 503 s / 602 s |
| c4-group-r1 | 48.9 M rows | 476 runs | 563 ms / 1921 ms | 1921 ms / 456 ms | 476 / 72 iterations | 32 iterations | 314.6 ms / 0.51 ms | 1000 ms / 33.2 ms | 1000 ms / 29.2 ms | 5.62 s | 347 s / 2404 s |
| c4-group-r2 | 41.5 M rows | 384 runs | 784 ms / 3668 ms | 3668 ms / 426 ms | 384 / 271 iterations | 145 iterations | 498.3 ms / 0.54 ms | 1902 ms / 178.3 ms | 1902 ms / 175.6 ms | 25.55 s | 478 s / 2406 s |
| c4-group-r3 | 34.6 M rows | 405 runs | 752 ms / 12150 ms | 12150 ms / 1193 ms | 404 / 809 iterations | 84 iterations | 502.3 ms / 0.62 ms | 35010 ms / 347.7 ms | 34966 ms / 334.3 ms | 55.61 s | 642 s / 2404 s |

**Where a run's time goes**, summed over the cell's runs (each phase timed
on the reactor; `data_sync` is a step's data-file `fsync`, `log_gate_*` the
log waits, `publish_*` the anchor publish's whole-store flush and its
`fsync`):

| cell | run time s | start | log_gate_batch | log_gate_run | page_write | map_write | claim_wait | data_sync | end_wait | publish_writeback | publish_sync |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c1-relaxed-r1 | 109 s | 2.0 s | 8.9 s | 0.0 s | 2.3 s | 0.0 s | 0.0 s | 91.4 s | 0.3 s | 1.9 s | 0.5 s |
| c1-relaxed-r2 | 107 s | 1.9 s | 7.9 s | 0.0 s | 2.4 s | 0.0 s | 0.0 s | 90.6 s | 0.3 s | 1.9 s | 0.4 s |
| c1-relaxed-r3 | 96 s | 1.7 s | 16.1 s | 0.0 s | 1.8 s | 0.0 s | 0.0 s | 73.0 s | 0.3 s | 1.7 s | 0.5 s |
| c4-relaxed-r1 | 582 s | 4.0 s | 18.3 s | 87.2 s | 1.3 s | 0.1 s | 0.0 s | 243.6 s | 12.9 s | 125.1 s | 85.9 s |
| c4-relaxed-r2 | 578 s | 4.9 s | 22.6 s | 98.0 s | 1.8 s | 0.1 s | 0.0 s | 212.0 s | 13.4 s | 129.3 s | 92.6 s |
| c4-relaxed-r3 (contended) | 590 s | 3.1 s | 33.9 s | 68.1 s | 2.8 s | 0.0 s | 0.0 s | 216.1 s | 9.7 s | 168.9 s | 82.4 s |
| c1-group-r1 | 68 s | 1.8 s | 0.0 s | 0.0 s | 1.9 s | 0.0 s | 0.0 s | 60.3 s | 0.6 s | 1.8 s | 0.3 s |
| c1-group-r2 | 84 s | 1.8 s | 0.3 s | 0.0 s | 1.8 s | 0.0 s | 0.0 s | 75.9 s | 0.4 s | 1.8 s | 0.5 s |
| c1-group-r3 | 139 s | 1.1 s | 0.0 s | 0.0 s | 1.8 s | 0.0 s | 0.0 s | 123.3 s | 3.4 s | 1.2 s | 7.0 s |
| c4-group-r1 | 290 s | 7.3 s | 64.8 s | 9.9 s | 2.5 s | 0.1 s | 0.0 s | 152.8 s | 5.0 s | 27.4 s | 17.4 s |
| c4-group-r2 | 339 s | 5.1 s | 65.3 s | 14.9 s | 2.2 s | 0.1 s | 0.0 s | 176.0 s | 4.0 s | 31.0 s | 38.5 s |
| c4-group-r3 | 358 s | 4.6 s | 71.5 s | 27.0 s | 4.2 s | 0.1 s | 0.0 s | 174.4 s | 8.4 s | 33.7 s | 30.4 s |

**Runs per core in ten minutes at `cores = 4`** (the gate's fairness,
BI-Q7): `c4-group` 120/118/119/119, 76/118/71/119, 100/97/102/106;
`c4-relaxed` 4/20/2/42, 10/71/23/3, 13/18/0/21.

## What it says

1. **A run holds its core's reactor for its whole length, and the core's
   sessions wait it out.** At `cores = 4`, `group`, the same-core statement
   probe answers in 0.51-0.62 ms at p50 outside a run and in 315-502 ms
   inside one; the longest single reactor iteration inside a run is 1.9-12 s
   against 0.4-1.2 s outside. Every run is at least one iteration over
   100 ms. W1 and W2 are aimed at exactly this.
2. **The data file's `fsync` is most of a run.** It is 60-89 % of the run
   time at `cores = 1` and 37-52 % at `cores = 4`: one `fsync` per 64-page
   step, on the reactor (`data_sync`). The log gates come next (the batch
   gate 65-72 s per `cores = 4`, `group` cell). Page writes themselves are
   1.3-4.2 s per cell - under 1 % - which is BI-R5's first remainder priced:
   leaving `pwrite` on the reactor costs little.
3. **`cores = 4`, `relaxed` is the bug entry's shape, and it is worse than
   the other three put together.** Runs take 5.4-9.3 s at p50 and up to
   17.7 s; per-core anchor gaps reach 82-114 s, the bug entry's 83-110 s
   reproduced; the gate starves cores (4/20/2/42 runs). The per-run gate
   (`log_gate_run`, 68-98 s per cell) and the publish's whole-store flush
   (`publish_writeback` 125-169 s, `publish_sync` 82-93 s) are what it adds
   over `group`: under `relaxed` nothing else syncs the log, so another
   core's re-dirtied pages meet the run's gate, and a long run leaves a
   large pool for the publish to flush. The statement probe's numbers in
   these cells are a backlog, not a latency: at `cores = 4`, `relaxed` a
   probe answers more slowly than every 20 ms for minutes at a time, and an
   open-loop probe that cannot keep up accumulates, so its p50 inside runs
   is minutes.
4. **`cores = 1` gains less from BI than `cores = 4` does.** Its probe's
   p99 is about the same in and out of a run (1.25 s against 1.26 s at
   `relaxed`): the four load sessions alone keep a single reactor busy for
   hundreds of milliseconds per insert batch, so the run's stall is one
   stall among many. `group`'s time charged to no group, 457-503 s of
   601 s, is core 0's drain syncing commits inline, which W4 keeps out of
   BI.
5. **BI-Q9's gate**: (i) does not hold (every cell shows runs holding the
   reactor for hundreds of milliseconds); (ii) holds by its letter, in a
   handful of runs per cell, as single device writes stalled in the kernel.
   The ruling is the order's BI-Q9-R.

## A defect this run found, not BI's

At `cores = 4` an `INSERT` refused `TXN_CONFLICT retryable=1` mid-statement
left its keys bound: the retry the reply invites was refused `duplicate
primary key` at row 1. The driver recorded six retried refusals across the
`cores = 4` cells (`c4-group-r1` 1, `c4-group-r3` 1, `c4-relaxed-r1`'s re-run
2, `c4-relaxed-r2` 1, `c4-relaxed-r3` 1) and three of them met the duplicate
(`c4-group-r1`, `c4-relaxed-r2`, `c4-relaxed-r3`); the first attempt of
`c4-relaxed-r1` met it once more, before the driver counted it. None at
`cores = 1`.
`docs/inflight/bugs/a-rollback-whose-pk-descent-is-refused-leaves-its-keys-bound.md`.

## Against what

There is no earlier number for this driver and shape: this file is BI's A,
the comparator BI-S5's B is measured against. The bug entry's
measurement (`v2.7.0-688-g19843fee`, xrock's load and `C_PING` probes)
is the nearest earlier reading, and its 83-110 s anchor gaps and 5.7 s
stalls agree with this run's `cores = 4`, `relaxed` cells in kind. A
PostgreSQL twin has no counterpart to this measurement.
