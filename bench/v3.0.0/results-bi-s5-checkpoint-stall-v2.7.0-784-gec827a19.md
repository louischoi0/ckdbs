# BI's close — the checkpoint off the reactor, A against B (`v2.7.0-784-gec827a19`)

**Thesis.** BI took the checkpoint off its core's reactor for most of a run:
the time a run spends *on* the reactor fell about tenfold in every shape (at
`cores = 4`, `group` from 290-358 s per ten-minute cell to 15-18 s), the
checkpoint's single longest hold fell from 1.1-79 s to 0.15-4.2 s, and its
holds over 100 ms from one per run to 1-12 per cell under `group`. **BI-Q10's
bar (i) is not met**: the checkpoint still holds a reactor past 10 ms -
a step's `pwrite` stalled in the kernel's writeback throttle (up to about
300 ms), the inline log-gate remainder after 16 deferrals at `relaxed`, and a
publish batch's writes - and under `cores = 1`, `relaxed` a run is 4-8x
longer than A's, its anchor gap 11-16 s against 6 s. Bar (ii) is met in 10 of
12 cells.

- **A** = the build-release of `55ee8370` (`v2.7.0-779-g55ee8370`), BI-S1's
  instrumented commit, as BI-Q10 defines it; sha256
  `bd8ffa1fa32b12e731eb346cdc54943a4e7158d5dd6ccd168a0a2abc7254ce18`. Its twelve
  stall cells are BI-S1's (`results-bi-s1-checkpoint-stall-v2.7.0-779-g55ee8370.md`),
  measured 12:48-15:11 UTC the same day; its two idle-core cells were measured
  with B's.
- **B** = the build-release of `ec827a19` (`v2.7.0-784-gec827a19`), BI closed in
  code; sha256 `2b45db5bd560e73133058bafc0a142d41737f8eee0d1d68f6b34704e0bba8ab5`.
  Later commits on the branch change text only. Measured 17:31-19:59 UTC.
- **Driver**: `tools/bi_checkpoint_stall.py` as at `ec827a19`, a frozen copy
  under the run directory (`archive/.../run.sh`), the same shape as BI-S1's:
  four load processes of 1024-row named-key `INSERT`s, an open-loop
  same-core statement probe and `C_PING` probe at 20 ms, 600 s per cell, three
  runs per shape interleaved. B's cells add a 30 s tail after the load
  (`--idle-seconds 30`) and a core-0 `SHOW META` probe for the loss window;
  the tail's runs are excluded from every load statistic.
- **Device**: `/dev/root`, ext4. **Host**: 1-minute load 1.1-6.4 at the
  start and end of B's cells (each cell's JSON), against BI-S1's 1.8-13
  (`c4-relaxed-r3` there was contaminated). Raw cells:
  `/home/cdkbs/bench-runs/bi-s5-v2.7.0-784-gec827a19/cells/`; per-core tables:
  `archive/bi-s5-checkpoint-stall-v2.7.0-784-gec827a19/summary.md`.

**How (i) is read** (BI-Q10-R): the checkpoint's own holds. For A every run
is one hold, its wall time, because A's run is one timer callback. For B
they are the `system` group's polls (`sched_system_longest_poll_us`,
`_polls_over_10ms`, `_polls_over_100ms`; cumulative over the server's life,
so a few seconds of probe fill and warm-up are in them), the checkpoint task
being the one `system` task where `SO_REUSEPORT` binds.

## A against B

| shape | run | runs A / B | run p50 A / B | the checkpoint's longest own hold A / B | its holds > 10 ms A / B | > 100 ms A / B | longest iteration in a run A / B | statement probe p50 in a run A / B | p99 in a run A / B | p99 out of a run A / B | worst anchor gap A / B |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c1-relaxed | r1 | 120 / 114 runs | 828 ms / 3507 ms | 1404 ms / 444 ms | 120 / 306 | 120 / 22 | 1404 ms / 1126 ms | 646.6 ms / 147.5 ms | 1253 ms / 726 ms | 1259 ms / 481 ms | 5.7 s / 11.2 s |
| c1-relaxed | r2 | 120 / 72 runs | 826 ms / 6092 ms | 1421 ms / 385 ms | 120 / 184 | 120 / 8 | 1421 ms / 1525 ms | 617.8 ms / 326.8 ms | 1269 ms / 1233 ms | 1267 ms / 894 ms | 5.7 s / 15.2 s |
| c1-relaxed | r3 | 120 / 66 runs | 656 ms / 6246 ms | 2005 ms / 635 ms | 120 / 181 | 120 / 8 | 2005 ms / 1745 ms | 644.3 ms / 361.1 ms | 1568 ms / 1346 ms | 1407 ms / 740 ms | 6.0 s / 16.3 s |
| c4-relaxed | r1 | 68 / 42 runs | 9264 ms / 11486 ms | 17697 ms / 1586 ms | 68 / 238 | 68 / 78 | 17697 ms / 2015 ms | 141039.3 ms / 380.5 ms | 428024 ms / 124570 ms | 260443 ms / 127083 ms | 114.5 s / 72.5 s |
| c4-relaxed | r2 | 107 / 32 runs | 5388 ms / 15599 ms | 11106 ms / 4195 ms | 107 / 145 | 107 / 53 | 11106 ms / 4195 ms | 172995.8 ms / 535.7 ms | 384796 ms / 137483 ms | 286990 ms / 158521 ms | 82.6 s / 112.7 s |
| c4-relaxed | r3 | 52 / 49 runs | 7560 ms / 9641 ms | 78946 ms / 2171 ms | 52 / 219 | 52 / 54 | 78969 ms / 2171 ms | 127504.3 ms / 442.6 ms | 241168 ms / 47978 ms | 310718 ms / 46742 ms | 288.7 s / 77.4 s |
| c1-group | r1 | 120 / 120 runs | 555 ms / 612 ms | 1126 ms / 262 ms | 120 / 271 | 120 / 1 | 1126 ms / 405 ms | 485.9 ms / 15.5 ms | 938 ms / 363 ms | 891 ms / 318 ms | 5.7 s / 5.5 s |
| c1-group | r2 | 120 / 120 runs | 685 ms / 720 ms | 1130 ms / 151 ms | 120 / 286 | 120 / 3 | 1141 ms / 565 ms | 578.5 ms / 24.7 ms | 983 ms / 486 ms | 1080 ms / 440 ms | 5.6 s / 5.6 s |
| c1-group | r3 | 119 / 120 runs | 690 ms / 712 ms | 12753 ms / 229 ms | 119 / 275 | 116 / 3 | 12753 ms / 598 ms | 44750.6 ms / 26.6 ms | 73902 ms / 498 ms | 74500 ms / 455 ms | 17.1 s / 5.6 s |
| c4-group | r1 | 476 / 469 runs | 563 ms / 570 ms | 1921 ms / 421 ms | 476 / 918 | 476 / 12 | 1921 ms / 421 ms | 314.6 ms / 0.6 ms | 1000 ms / 177 ms | 33 ms / 86 ms | 5.6 s / 15.5 s |
| c4-group | r2 | 384 / 401 runs | 784 ms / 628 ms | 3668 ms / 584 ms | 384 / 760 | 383 / 7 | 3668 ms / 615 ms | 498.3 ms / 0.6 ms | 1902 ms / 265 ms | 178 ms / 74 ms | 25.5 s / 20.8 s |
| c4-group | r3 | 405 / 425 runs | 752 ms / 668 ms | 12150 ms / 596 ms | 405 / 845 | 404 / 8 | 12150 ms / 596 ms | 502.3 ms / 0.6 ms | 35010 ms / 252 ms | 348 ms / 140 ms | 55.6 s / 20.8 s |

(The statement probe in the `c4-relaxed` cells is a backlog in both A and B:
the load keeps those reactors slower than one probe per 20 ms for minutes at
a time, and an open-loop probe that cannot keep up accumulates. Its ratios
there say little either way.)

## Where B's runs spend their time

Summed over the cell's runs; every phase is reactor time, the two parked
columns are wall time during which the reactor ran other work. A's runs had
no parked time: BI-S1's table puts 96-590 s per cell on the reactor.

| B cell | run time | on the reactor (the phases summed) | start | log_gate_batch | log_gate_run | page_write | claim_wait | data_sync | end_wait | publish_writeback | publish_sync | parked on the log | parked on the data file |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| B-c1-relaxed-r1 | 389 s | 13.9 s | 1.9 s | 3.1 s | 0.0 s | 6.0 s | 0.0 s | 0.0 s | 0.0 s | 2.9 s | 0.0 s | 272 s | 79 s |
| B-c1-relaxed-r2 | 447 s | 8.8 s | 1.0 s | 1.7 s | 0.0 s | 3.2 s | 0.0 s | 0.0 s | 0.0 s | 2.9 s | 0.0 s | 338 s | 82 s |
| B-c1-relaxed-r3 | 427 s | 9.1 s | 0.9 s | 1.6 s | 0.0 s | 3.8 s | 0.0 s | 0.0 s | 0.0 s | 2.7 s | 0.0 s | 325 s | 75 s |
| B-c4-relaxed-r1 | 505 s | 60.3 s | 0.9 s | 5.6 s | 19.7 s | 5.7 s | 0.0 s | 0.0 s | 0.0 s | 28.4 s | 0.0 s | 343 s | 60 s |
| B-c4-relaxed-r2 | 537 s | 50.3 s | 0.3 s | 4.9 s | 12.9 s | 3.7 s | 0.0 s | 0.0 s | 0.0 s | 28.6 s | 0.0 s | 413 s | 55 s |
| B-c4-relaxed-r3 | 487 s | 52.4 s | 0.6 s | 7.1 s | 20.1 s | 3.2 s | 0.0 s | 0.0 s | 0.0 s | 21.5 s | 0.0 s | 343 s | 69 s |
| B-c1-group-r1 | 83 s | 8.6 s | 1.8 s | 0.0 s | 0.0 s | 6.4 s | 0.0 s | 0.0 s | 0.0 s | 0.3 s | 0.0 s | 5 s | 5 s |
| B-c1-group-r2 | 97 s | 9.1 s | 1.8 s | 0.0 s | 0.0 s | 7.0 s | 0.0 s | 0.0 s | 0.0 s | 0.3 s | 0.0 s | 5 s | 5 s |
| B-c1-group-r3 | 99 s | 8.7 s | 1.8 s | 0.0 s | 0.0 s | 6.6 s | 0.0 s | 0.0 s | 0.0 s | 0.3 s | 0.0 s | 5 s | 5 s |
| B-c4-group-r1 | 269 s | 18.1 s | 7.0 s | 0.9 s | 1.0 s | 5.8 s | 0.0 s | 0.0 s | 0.0 s | 3.5 s | 0.0 s | 209 s | 26 s |
| B-c4-group-r2 | 270 s | 14.7 s | 6.1 s | 0.3 s | 0.7 s | 3.4 s | 0.0 s | 0.0 s | 0.0 s | 4.1 s | 0.0 s | 211 s | 31 s |
| B-c4-group-r3 | 288 s | 16.7 s | 6.5 s | 0.6 s | 1.6 s | 4.8 s | 0.0 s | 0.0 s | 0.0 s | 3.3 s | 0.0 s | 222 s | 36 s |

## The idle-core run and core 0's loss window

An idle-core run is one that started in the 30 s after the load stopped:
its wall time is what BI-Q6 (b) - no kick for a data-file park - costs on a
core with nothing else to do. The loss window is the longest interval, inside
one of core 0's runs, from the last `SHOW META` reading before
`wal_interval_syncs` moved to the reading that saw it move next: an
approximation from above, served by core 0's own reactor. Under `group` the
counter does not move (commits sync the log), so the column is meaningless
there and left out.

| cell | idle-core runs | idle run wall p50 / max | core 0's loss window, longest, in its runs |
|---|---|---|---|
| A-c4-relaxed-idle | 27 runs | 40 ms / 612 ms | 8347 ms |
| B-c4-relaxed-r1 | 23 runs | 38 ms / 1482 ms | 1850 ms |
| B-c4-relaxed-r2 | 21 runs | 37 ms / 114 ms | 2969 ms |
| B-c4-relaxed-r3 | 24 runs | 46 ms / 815 ms | 2054 ms |
| B-c1-relaxed-r1 | 7 runs | 62 ms / 76 ms | 1203 ms |
| B-c1-relaxed-r2 | 7 runs | 55 ms / 60 ms | 1635 ms |
| B-c1-relaxed-r3 | 7 runs | 52 ms / 57 ms | 1846 ms |
| A-c4-group-idle | 28 runs | 39 ms / 85 ms | - |
| B-c4-group-r1 | 28 runs | 52 ms / 257 ms | - |
| B-c4-group-r2 | 24 runs | 49 ms / 79 ms | - |
| B-c4-group-r3 | 28 runs | 49 ms / 99 ms | - |
| B-c1-group-r1..r3 | 7 runs each | 55-64 ms / 59-72 ms | - |

## BI-Q10, item by item

- **(i), as BI-Q10-R reads it - not met.** The checkpoint's longest own hold
  is 151-635 ms at `cores = 1` and 421-596 ms at `cores = 4`, `group`, and
  1.6-4.2 s at `cores = 4`, `relaxed`; its holds over 100 ms are 1-12 per
  cell under `group` and 8-78 under `relaxed`, where A had one per run. What
  is left, from B's run records: a step's page writes (`page_write`, single
  writes up to about 300 ms in the kernel's writeback throttle - BI-R5's
  first remainder, which W2 leaves on the reactor); the inline log gate
  after 16 deferrals (`log_gate_batch`/`log_gate_run`, 13-20 s per
  `cores = 4`, `relaxed` cell, hot pages re-dirtied by other cores faster
  than a sync lands); and a publish batch's writes (`publish_writeback`,
  21-29 s per `cores = 4`, `relaxed` cell). None of them is a sync: every
  `fsync` the checkpoint does is on a thread now.
- **(ii) - met in 10 of 12 cells.** The in-run p99 is within 2x the out-of-run
  p99 everywhere but `c4-group` r1 (177 ms against 86 ms, 2.06x) and r2
  (265 ms against 74 ms, 3.6x); A's ratios in that shape were 30x and 11x.
- **(iii) - the OLTP overhead A/B**: its own file,
  `results-bi-close-overhead-v2.7.0-784-gec827a19.md`.
- **(iv), reported, no bar.** Runs per ten minutes: `group` unchanged
  (120/120 at `cores = 1`, 384-476 against 401-469 at `cores = 4`);
  `relaxed` fewer - 66-114 against 120 at `cores = 1`, 32-49 against 52-107 at
  `cores = 4`. A run at `cores = 1`, `relaxed` takes 3.5-6.2 s at p50 against
  0.66-0.83 s, and the worst anchor gap is 11-16 s against 6 s; at `cores = 4`,
  `relaxed` it is 72-113 s against 83-289 s. The gate's rotation shows in the
  `relaxed` per-core run counts (the per-core tables).

## What it says

1. **BI moved the waits, which were the stall.** A run's reactor time is the
   phases, and they fell about tenfold; the run's length is mostly parked
   time now (210-413 s on the log and 26-82 s on the data file per cell at
   `relaxed` and `cores = 4`, `group`). The same-core statement probe at
   `cores = 4`, `group` answers in 0.6 ms at p50 inside a run, where A's
   answered in 315-502 ms.
2. **What is left on the reactor is writes, not waits.** The bar (i) asks for
   10 ms, and a single `pwrite` into a page cache under writeback pressure
   can block for a quarter-second. Meeting it needs the writes off the
   reactor too, which W2 does not cover (*"Hand the data file sync and
   EnsureDurable to the writer/I/O thread"*) and BI's §0 states as not done.
3. **`relaxed` pays for parking in run length.** Under `group` the log is
   always nearly durable and a run parks rarely; under `relaxed` a run's
   steps defer on pages its own and other cores keep re-dirtying, so the
   run takes many syncs' worth of wall time, and the anchor moves less often.
   W1 accepted longer runs; at `cores = 1`, `relaxed` the price is an anchor
   gap of 11-16 s against 6 s.
4. **An idle core's run costs what it did** (37-64 ms at p50 against 39-40 ms),
   so BI-Q6 (b)'s missing kick on a data-file park is not visible here.
5. **The rollback defect recurred** on both binaries
   (`docs/inflight/bugs/a-rollback-whose-pk-descent-is-refused-leaves-its-keys-bound.md`):
   a retried batch met a duplicate after a `retryable=1` refusal in
   `B-c4-group-r1`..`r3`, `B-c4-relaxed-r1`, `B-c4-relaxed-r3` and
   `A-c4-group-idle`.
