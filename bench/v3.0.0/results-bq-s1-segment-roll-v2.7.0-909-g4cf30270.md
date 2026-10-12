# BQ-S1: the WAL segment roll, measured — at `relaxed` its hold is a sync it waits out, and the log it sits in is never recycled

**Thesis.** On `v2.7.0-909-g4cf30270` (BQ-S1's instruments), a segment roll
at `relaxed` holds the WAL stream latch for a median **331 ms** (p99 788 ms,
max 1.21 s) and **95.6 % of that hold is one step: the roll waiting for the
WAL writer thread's `fdatasync` of the tail segment, already in flight.**
The roll's own sync is 1.0 % of its hold, a missed body 0.6 %. Rolls happen
every ~0.85 s at the bulk writer's 75-82 MiB/s, so **35-46 % of every 30 s
window is spent under a hold.** Every core that appends, and core 0 through
its group-commit drain, waits the hold out in full; a point reader or a
one-row writer that never touches the bulk core sees a schedule p99 of
**288-659 ms against 0.23-0.47 ms alone (1,030x to 2,000x)**, and BQ's bar
(p99 within 2x, p99.9 within 5x of isolated) is missed by two to three orders
of magnitude in every relaxed cell. At `group` the same roll holds a median
6.1 ms (p99 20.6 ms, 0.5-0.7 % of the window); readers stay within 1.1-2.5x
of isolated at p99 and the one cell that misses the bar by a wide margin,
C2's writers (29x), is not the roll's: none of its slow samples lines up
with one.

Two findings carry the insight, both about what the engine does rather than
what a roll costs in isolation:

- **(a) The relaxed hold is a queue behind a slow sync, not the roll's I/O.**
  A roll waits out about two writer-thread syncs (300 ms at the median
  between them); the 71-72 syncs per 30 s that rolls waited out add up to
  19-20 s of the window, so the one thread that syncs the log spends about
  two thirds of the run inside syncs that span a segment boundary, on a
  device that is also taking 75-80 MiB/s of log and a checkpoint's
  writeback. Syncing fewer segments does not remove the wait: with every
  non-tail sync subtracted from every roll, the hold's p99 is still
  **652-780 ms** (§4).
- **(b) At `relaxed` the log is not recycled during the load.** 13 of 15
  relaxed runs removed **0** segments (the other two removed 2 and 4) and held
  33-39 live segments at their last roll; every `group` run removed 20-22 and
  held 5-6. The fold's redo start stays in segment 0 because one page re-dirtied
  after its writeback copy keeps its first recLSN, and the checkpoint gate's
  rotation stretches each core's interval to ~25 s (§5). The unrecycled
  run is also what makes each sync's breadth grow, so (b) feeds (a).

**This is BQ's first measurement of the roll.** There is no earlier number
for this shape (cells, durabilities, core count, driver); this file is the
baseline BQ-S2 and later builds are read against. PostgreSQL is not part of
this file: the floor was not measured for this shape.

- **Order**: `instructions/v3.0.0/workorder-bq-wal-segment-roll.md` (§3
  hypotheses, §5.1 cells, §5.2 what a file carries, §5.3 the bar).
- **Drivers**: `tools/bl_mixed.py` at `4cf30270` (the matrix ran the copy in
  the measured worktree, which a `diff` against the commit's file shows
  identical today) driven by `bq_s1_matrix.py` (archived beside the raw
  output). Flag-by-flag documentation is the driver's own docstring;
  `bench/docs/` is closed (`bench/README.md`).
- **Raw output**: `archive/bq-s1-segment-roll-v2.7.0-909-g4cf30270/` — per
  run the driver JSON (gzip), the server log with every `[wal] roll` line,
  the console log; the matrix driver and `hoststamps.log`; the analysis scripts
  that produced the tables below.
- **Companion**: `results-bl-ruling1-remeasure-v2.7.0-909-g4cf30270.md`
  (BQ-Q1 (a): BL's owed ruling-1 re-measurement, same host session).

| Item | Value |
|---|---|
| Executed | 2026-10-12, **12:45-13:29 UTC** (BQ matrix, 54 cells; `hoststamps.log`, `bq.done` 13:29:24). BL's ruling-1 pair ran 12:38-12:45 UTC; a 6 s driver smoke at 12:27 UTC is excluded. **This file was written 2026-10-12 ~14:30 UTC from the existing data; no run, server, build or test was started in the session that wrote it** |
| Measured tree | worktree `wal-segment-roll-order`, branch `worktree-wal-segment-roll-order`, commit **`4cf30270`** = **`v2.7.0-909-g4cf30270`** (BQ-S1: the roll's instruments). It is an ancestor of `worktree-bp-per-core-residue` (merge `6139c2de`); this file is written on `bp-per-core-residue` at `8db9bb5b` (`v2.7.0-918-g8db9bb5b`) |
| Tree cleanliness | **Not recorded by the run.** Provenance instead: `build-release/kds_server` in that worktree has mtime 2026-10-12 10:33:54, 59 s after the commit (10:32:55), and its sha256 is the one every run's JSON records (below) |
| Binary | A copy, `/home/cdkbs/bench-runs/bq/bin/kds_server.4cf30270`, **sha256 `024ef4dc863ebc6b164dd5a585a9fd6ccbfcf12817be1a0e1d29ef492d2e9540`**; the driver copies it again into the run's work directory (`kds_server.blmixed`, same hash), and `bin_sha256` in all 54 BQ JSONs (and the 2 smoke JSONs) carries it. Source binary mtime 10:33:54.49, copy 10:33:54.52 |
| Build | Release (`CMAKE_BUILD_TYPE:STRING=Release` in that worktree's `build-release/CMakeCache.txt`) |
| Device | The WAL directory (`<workdir>/<run>/d.db.wal`) and the data file (`d.db`) are on the same filesystem: `/dev/root`, **ext4** (`/dev/nvme0n1p1` mounted on `/`, `rw,relatime,discard,errors=remount-ro,commit=30`), an Azure VM's NVMe disk, 126 GB free of 259 GB; not tmpfs (recorded per run as `data_device`). No dedicated WAL device (CN-9) |
| Host | 8 vCPU (AMD EPYC 9V74), Linux 7.0.0-1014-azure. **Servers pinned to CPUs 0,2,4,6, clients to 1,3,5,7 — the SMT siblings of the reactors** (`results-bl-s1-placement-premise-v2.7.0-857-g3552a03c.md`), so every four-core cell is indicative of the roll's cost, not of a clean machine |
| Server configuration | `cores = 4`, `durability = relaxed` or `group` (the cell's), `buffer_pool_frames = 131072`, `log_level = info` (the per-roll line is Info), `reactor_cpus = 0,2,4,6`; **every other key at its default**: 64 MiB WAL segments (`kDefaultSegmentSize`), 1 MiB stream ring (`kDefaultRingCapacity`), checkpoint cadence 5000 ms, BL-S3's session balancer live. One fresh server and one fresh data file per run |
| Cells | 9 cells x 2 durabilities x 3 runs (§0). 30 s measured after a 2 s warm-up |
| Baseline | **None: this is BQ's first measurement of the roll** (no earlier file measures this shape with this driver); it is the baseline BQ-S2 and later builds are read against. The PostgreSQL floor was not measured for this shape |
| Suite / overhead | **Suite not executed for this file** (analysis of existing files only); the instruments' own cells are `tests/wal_roll_instruments_test.cpp`. Overhead of the instruments: **not measured** (BQ-S5 measures it over BQ's whole change) |

## 0. What was measured, and how to read the tables

The cells are §5.1's. All run at `cores = 4`, once at `relaxed` and once at
`group`, three runs each, in one pass per run index (`relaxed` then `group`,
each in the order below), so the three passes are the interleave and a mixed
cell is immediately followed by its isolated arm. Flags below are
`tools/bl_mixed.py`'s; every cell adds `--cores 4 --seconds 30 --warmup 2
--runs 1 --frames 131072 --log-level info --ctl-core 0 --server-cpus 0,2,4,6
--client-cpus 1,3,5,7` and the matrix driver's `--durability`.

| Cell | Flags | What runs | Read |
|---|---|---|---|
| C3 | `--arm insert --readers 0 --writers 0 --placement segregated` | the bulk inserter alone (1,024-row `INSERT` statements into `bl_ins`, closed loop) | rows/s, log MiB/s |
| C2 | `--arm insert --readers 0 --writers 8 --writer-rate 800 --placement segregated` | inserter + 8 open-loop autocommit single-row pk `UPDATE` sessions (800/s in total) on cores 0-2 | writers' schedule and service latency |
| C2 isolated | `--arm isolated --readers 0 --writers 8 --writer-rate 800 --placement segregated` | the writers alone | baseline |
| C1 | `--arm mixed --readers 6 --rate 400 --placement P` | inserter + a walker (unindexed one-row `SELECT` over 10,000 wide rows) + 6 open-loop pk point readers (400/s in total), `P` in `segregated-no0`, `batch0`, `spread` | readers' schedule and service latency |
| C1 isolated | `--arm isolated --readers 6 --rate 400 --placement P` | the readers alone | baseline |

**Latency.** A reader or writer is open-loop: *schedule* latency is taken
from the instant the schedule said to send (a session held behind a stall is
charged for the wait it could not start in), *service* latency from the send.
The statement of an autocommit writer includes its commit, so C2's "commit
latency" is the writers' `writer_from_schedule`. Percentiles are
nearest-rank over every reader's or writer's samples in the 30 s window
(about 12,000 per C1 run, 24,000 per C2 run). **A table cell is the median of
the three runs' percentile**; every run's row is in Appendix A. Where this
file says *spread* it means the maximum minus the minimum of the three runs.

**Rolls.** Every figure about a roll is read from the server log's
`[wal] roll` lines whose `start_ns` lies in `[m0, m1]`, the `SHOW META`
window; the count equals the `wal_rolls` counter's delta in all 30 load-cell
runs (checked by the script). The checked analyses window slightly
differently (one roll fewer in C2 at `relaxed`: 523 rolls against this file's
524), which moves no verdict. A roll's step names are `stream.cpp`'s: **a**
seal, **b** flush, **c** the device sync (**c1** the gate: the wait on
`sync_mutex_`, i.e. on the writer thread's sync in flight; **c2** the roll's
own sync of the tail; **c3** its non-tail `fdatasync`s), **d** the segment
body (a wait on the preparer or an inline build; "hit" means the body was
ready), **e** the new segment's header write and `fdatasync`, **f** the
rename, **g** the directory `fsync`. `hold` is timed from after the roll's
append took the latch to its release; the residual (hold minus a-g) is at
most 0.2 % everywhere, so the split is complete.

**Counters** are `SHOW META` at `m1` minus `m0`, a key absent at `m0` read as
0 (a per-core field is absent while it is 0). The driver's own `meta_delta`
drops such keys and differences the two running maxima (`wal_roll_hold_max_us`),
and `analyze_s1.py` hard-codes the bulk core; **no figure here comes from
either** (§9).

**The row-set sweep (200 / 1K / 10K) was not run, and does not apply to
this measurement the way it does to a scan.** A roll's cost is a function of
log bytes (one segment per 64 MiB) and of the device's state, not of a
relation's cardinality. The relations here are fixed: `bl_point` and
`bl_walk` hold 10,000 rows each (`--point-rows`, `--walk-rows` defaults; C2's
writers each own a 1,250-key stripe of `bl_point`), and `bl_ins` grows by
about 5.1 million rows per `relaxed` run and 3.5 million per `group` run
(rows/s x 30 s). What the data does show is that the hold does not grow with how
much log the run has already written (its correlation with the live segment
count is 0.02-0.06, §3); what it cannot show is any effect at 200 or 1,000
rows, because none was measured.

## 1. The verdicts

Each is the checked analysis's (an analyst's recomputation from the raw
files, then one or two adversarial recomputations by other agents; where a
checker overturned or corrected the analyst, the corrected verdict is what is
written here and the correction is named). Every criterion is §3's text,
applied without adjustment. A "borderline in substance" note is the
analysts', never a change of the letter.

| Hypothesis | `relaxed` | `group` |
|---|---|---|
| **H0** — after `05cd011f` a roll still stalls cores with no bulk writer. Rejected only if in both C1 and C2 the largest hold is under 1 ms, **or** the stall-overlapped share of the non-bulk cores' latch wait is under 10 % **and** the schedule p99 is within 2x of isolated | **Not rejected**, by 100x or more on every clause: largest hold 587 ms to 1.21 s; core 0's drain is 100 % of its stream-latch wait; p99 ratios 1,032x / 1,036x / 1,996x (C1) and 1,399x (C2). 99.4-100 % of the non-bulk cores' stream-latch wait is roll-covered | **Not rejected by the letter**: C2 fails both clauses (largest hold 15-21 ms; roll-covered share 81-84 % of stream-latch wait, 41-62 % of all latch kinds; p99 27.7-29.9x). **Borderline in substance**: 0 of 241 / 239 / 240 C2 samples at or above p99 end within 20 ms of a roll's end (chance 1.5 %), and C2's whole distribution is shifted (p50 12.6-14.4 ms against 1.2 ms). The criterion cannot tell that cause from the roll's |
| **H1** — one or two steps carry most of the hold; rejected if no step reaches 40 % of the summed hold of the rolls at or above the hold's p99 | **Held**, robustly: step c is 89.8-99.5 % in every cell, almost all gate (73-99 %); c leads in 15 of 15 runs and in all 2,000 bootstrap draws. Prediction partly wrong: d is at most 10 % (0.6 % of all hold), e-g together 2.4 % | **Held by the letter, borderline in substance**: each cell's p99 bin is one roll, and the leading step flips between c (its own tail `fdatasync`) and g (the directory `fsync`) from run to run; over all rolls of the five load cells c is 40.7 %, g 39.8 %, e 18.6 % (the checked analysis, on the four H0 cells: 41.7 / 39.2 / 18.2). d is 0 (360 of 360 hits); e+f+g are 58.8 % of group hold (3.4 ms at p50, 14.8 ms at p99). Step c's own non-tail part is 1.3 % of the hold at or above p99, so group points at no rung below 4 |
| **H3, measurement half** — the non-tail `fdatasync`s are a material share of step c; rejected if under 10 % of step c's summed time over the rolls at or above the hold's p99 (the proof owed is BQ-S3's and is not judged) | **Not rejected**: share of step c at or above p99 is 39.8-57.0 % (C1 spread) and 54.8-72.0 % (C2); C1 batch0 5.1-21.5 %, C3 4.6-18.8 %, C1 segregated-no0 0.2-0.6 % (lower-upper bound; 2 rolls per cell). **The analyst's "rejected on substance" was refuted by two checkers** and is carried here only as a prediction (below) | **Marginal**: single-roll bins C1 batch0 14.5-16.6 %, C2 10.3-30.5 % (both bounds above 10 %), C1 segregated-no0 1.6 %, spread 1.9 %, C3 2.4 %; at or above p90 every cell is at most 10.5 %; the non-tail time per waited sync is 0.23 ms at the median, 0.95 ms at most. Not a group finding |
| **H4** — a missed body is material under a bulk writer; rejected if missed rolls' summed hold is under 10 % of all rolls' hold in C3 | **Not rejected on the letter**: 11.89 % pooled, but 8.26 / 16.33 / 11.12 % per run (spread 8.08 points, straddling 10 %). **In substance rejected**: step d's own time is 0.33-0.83 % of C3's hold; missed rolls hold as long as hit rolls (median 355 ms against 362 ms) because the sync gate, not the body, dominates them | **Rejected**: 0 misses in 75 windowed C3 rolls, 0 in every group cell |
| **H5, measurement half** — a drain meeting a roll is a material share of its core's stalls (rung 2's reject criterion is BQ-S3's) | **Held on core 0 only**: the drain is 100 % of core 0's stream-latch wait, covering 94-99 % of the hold (53 % in spread r2, where 15 of 35 rolls ran on core 0 itself); peers carry 0 drain. Rung 2's ceiling: with every roll-aligned stall removed, C1 reader p99 falls to 2.6-145 ms, never within 2x of isolated (0.21-0.56 ms) | **Not material**: core 0's drain wait is 15-78 ms per 30 s in C1/C3; in C2 the peers carry 83-153 ms each (51-74 % of their stream wait) but only 0.5-0.9 % of C2's 10 ms+ writer stalls line up with a roll |
| **§5.3 bar** (BQ-Q2 (a)) — on cores with no bulk writer, in C1 and C2: p99 within 2x and p99.9 within 5x of the isolated baseline; C3 loses at most 3 % | **Not met**: both terms missed in every C1 placement and in C2 by 1,000-2,000x (p99) and 200-640x (p99.9) on medians, and by at least 117x under the most favourable run pairing; the miss belongs to the roll (78-99 % of 50 ms+ stalls overlap a roll while rolls cover 35-42 % of the window in C1) | **Not met**: C2 misses at 29.3x / 19.0x, by a non-roll holder. C1 segregated-no0 (1.06x / 1.32x) and batch0 (1.48x / 1.95x) meet it on medians but the run pairing decides (ratio ranges 0.86-5.4x and 0.96-3.5x); spread misses p99.9 (8.95x; 9.34x on the non-bulk readers), its p99 (2.50x) is undetermined on the non-bulk readers (their p99 is under the driver's 1 ms recording floor). C3's base is in §7 |

**H3's prediction, carried forward, not a verdict.** At `relaxed` a waited-out
writer sync costs the same whether its non-tail part is under 5 % of it
(median 270 ms, n = 255 rolls) or over 30 % (266 ms, n = 251 rolls), the
correlation of a sync's tail time and non-tail time is negative (-0.56), and
one clean non-tail descriptor costs about 50-57 µs. The non-tail time looks
like a flush that moved to whichever descriptor is synced first, not a cost
added per segment; whether narrowing the sync moves it back onto the tail is
what BQ-S3's narrowing build answers. The bound in §4 does not need that
answer.

## 2. Readers and writers: the schedule latency, and the bar

The first tables are the headline. At `relaxed` C1's body is nearly
unchanged (p50 within 1.2x of isolated) and its tail is a stall of the
roll's length: p95 already 113-146 ms, because 35-42 % of the window is under
a hold, and the maximum (584-810 ms) is a hold. C2's body moves too (p50
2.4 ms against 159 µs, p95 459 ms), because a writer that appends meets the
latch itself; its maximum, 1.09 s, is the 1.21 s hold's.

Schedule latency (median of 3 runs per percentile; isolated arms are the
controls):

**`relaxed`**

| cell | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| c2 | median of 3 | 71754 | 99.2 µs | 169 µs | 2.4 ms | 459 ms | 659 ms | 878 ms | 1.09 s |
| c2-iso | median of 3 | 72021 | 119 µs | 154 µs | 159 µs | 184 µs | 471 µs | 2.9 ms | 10.4 ms |
| c1-segregated-no0 | median of 3 | 36018 | 108 µs | 185 µs | 206 µs | 113 ms | 288 ms | 496 ms | 584 ms |
| c1-iso-segregated-no0 | median of 3 | 36018 | 117 µs | 168 µs | 174 µs | 199 µs | 279 µs | 2.5 ms | 4.1 ms |
| c1-batch0 | median of 3 | 36018 | 121 µs | 188 µs | 211 µs | 145 ms | 366 ms | 624 ms | 713 ms |
| c1-iso-batch0 | median of 3 | 36018 | 120 µs | 171 µs | 177 µs | 199 µs | 354 µs | 1.6 ms | 3.8 ms |
| c1-spread | median of 3 | 36018 | 121 µs | 184 µs | 212 µs | 146 ms | 452 ms | 735 ms | 810 ms |
| c1-iso-spread | median of 3 | 36018 | 140 µs | 183 µs | 188 µs | 210 µs | 226 µs | 1.1 ms | 3.6 ms |

**`group`**

| cell | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| c2 | median of 3 | 71918 | 1.2 ms | 3.3 ms | 13.9 ms | 283 ms | 320 ms | 381 ms | 400 ms |
| c2-iso | median of 3 | 72020 | 984 µs | 1.1 ms | 1.2 ms | 5.1 ms | 10.9 ms | 20.1 ms | 29.4 ms |
| c1-segregated-no0 | median of 3 | 36018 | 119 µs | 183 µs | 197 µs | 241 µs | 409 µs | 3.3 ms | 38.2 ms |
| c1-iso-segregated-no0 | median of 3 | 36018 | 117 µs | 168 µs | 173 µs | 205 µs | 386 µs | 2.5 ms | 4.1 ms |
| c1-batch0 | median of 3 | 36018 | 127 µs | 182 µs | 193 µs | 257 µs | 748 µs | 5.1 ms | 38.9 ms |
| c1-iso-batch0 | median of 3 | 36018 | 120 µs | 171 µs | 177 µs | 209 µs | 504 µs | 2.6 ms | 4.5 ms |
| c1-spread | median of 3 | 36018 | 126 µs | 183 µs | 197 µs | 264 µs | 975 µs | 14.1 ms | 48.9 ms |
| c1-iso-spread | median of 3 | 36018 | 135 µs | 184 µs | 190 µs | 217 µs | 389 µs | 1.6 ms | 5.1 ms |

Service latency is taken from the send, so a statement sent into a reactor
that is blocked in a drain is charged for the wait (its request sits in the
socket); what it does not charge is the lateness of the send itself, which
is the difference to the schedule columns. The two tails differ by orders of
magnitude for a reason of arithmetic: at `relaxed` C1's service p99 is
1.1-3.6 ms and its p99.9 323-377 ms (between 0.1 % and 1 % of statements
met a blocked reactor), yet its schedule p99 is 288-452 ms, because each reader
sends one statement every ~15 ms (400/s over six readers) and a 300 ms stall
of one reader makes ~20 of its scheduled sends late. At `group` service p99
is 0.3-0.6 ms for C1 and 86 ms for C2.

**`relaxed`**

| cell | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| c2 | median of 3 | 71754 | 37.0 µs | 91.6 µs | 102 µs | 232 µs | 310 ms | 692 ms | 1.09 s |
| c2-iso | median of 3 | 72021 | 73.9 µs | 85.6 µs | 90.6 µs | 114 µs | 218 µs | 1.8 ms | 10.3 ms |
| c1-segregated-no0 | median of 3 | 36018 | 61.0 µs | 112 µs | 126 µs | 185 µs | 1.1 ms | 323 ms | 584 ms |
| c1-iso-segregated-no0 | median of 3 | 36018 | 85.4 µs | 99.7 µs | 105 µs | 127 µs | 160 µs | 1.0 ms | 3.0 ms |
| c1-batch0 | median of 3 | 36018 | 61.8 µs | 113 µs | 128 µs | 212 µs | 3.6 ms | 377 ms | 713 ms |
| c1-iso-batch0 | median of 3 | 36018 | 86.1 µs | 102 µs | 108 µs | 126 µs | 177 µs | 1.0 ms | 3.2 ms |
| c1-spread | median of 3 | 36018 | 53.9 µs | 112 µs | 131 µs | 202 µs | 2.3 ms | 325 ms | 810 ms |
| c1-iso-spread | median of 3 | 36018 | 106 µs | 115 µs | 120 µs | 141 µs | 155 µs | 1.1 ms | 3.5 ms |

**`group`**

| cell | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| c2 | median of 3 | 71918 | 1.1 ms | 3.0 ms | 3.5 ms | 12.7 ms | 86.3 ms | 303 ms | 339 ms |
| c2-iso | median of 3 | 72020 | 914 µs | 1.0 ms | 1.1 ms | 4.7 ms | 10.3 ms | 17.6 ms | 29.3 ms |
| c1-segregated-no0 | median of 3 | 36018 | 81.3 µs | 115 µs | 125 µs | 162 µs | 286 µs | 3.0 ms | 38.1 ms |
| c1-iso-segregated-no0 | median of 3 | 36018 | 86.1 µs | 99.7 µs | 105 µs | 133 µs | 190 µs | 1.0 ms | 3.0 ms |
| c1-batch0 | median of 3 | 36018 | 82.3 µs | 114 µs | 123 µs | 170 µs | 382 µs | 3.3 ms | 38.9 ms |
| c1-iso-batch0 | median of 3 | 36018 | 84.6 µs | 103 µs | 108 µs | 134 µs | 295 µs | 1.5 ms | 3.0 ms |
| c1-spread | median of 3 | 36018 | 76.8 µs | 116 µs | 126 µs | 168 µs | 582 µs | 3.3 ms | 48.8 ms |
| c1-iso-spread | median of 3 | 36018 | 107 µs | 117 µs | 122 µs | 143 µs | 229 µs | 960 µs | 3.5 ms |

### The bar, and the noise floor it is read against

The spread of the isolated arm is the noise floor: its p99 spreads by
173-345 µs across the three runs at `relaxed` and 161-412 µs at `group`
(columns below), and a mixed-over-isolated difference that sits inside that
spread is not a finding. At `relaxed` the median ratios are 200-2,000x, three
orders over the floor; at `group` C1 segregated-no0 and batch0 are inside it.
"Ratio range" is the pessimistic and optimistic pairings of the runs
(minimum mixed over maximum isolated, to maximum over minimum), the way the
checked analysis states them.

| durability | cell | figure | mixed (r1 / r2 / r3) | spread of mixed | isolated (r1 / r2 / r3) | spread of isolated | median ratio (bar: 2x p99, 5x p99.9) | ratio range (min mixed / max iso .. max mixed / min iso) | bar |
|---|---|---|---|---|---|---|---|---|---|
| relaxed | c1-segregated-no0 | schedule p99 | 306 ms / 211 ms / 288 ms | 95.8 ms | 279 µs / 216 µs / 561 µs | 345 µs | 1,031.90x (limit 2x) | 375.21x .. 1,419.8x | missed |
| relaxed | c1-segregated-no0 | schedule p99.9 | 626 ms / 296 ms / 496 ms | 330 ms | 2.5 ms / 803 µs / 2.5 ms | 1.7 ms | 200.69x (limit 5x) | 116.85x .. 778.9x | missed |
| relaxed | c1-batch0 | schedule p99 | 430 ms / 354 ms / 366 ms | 75.6 ms | 379 µs / 206 µs / 354 µs | 173 µs | 1,035.85x (limit 2x) | 933.96x .. 2,084.9x | missed |
| relaxed | c1-batch0 | schedule p99.9 | 593 ms / 674 ms / 624 ms | 81.1 ms | 2.3 ms / 1.0 ms / 1.6 ms | 1.3 ms | 401.36x (limit 5x) | 261.05x .. 668.5x | missed |
| relaxed | c1-spread | schedule p99 | 205 ms / 508 ms / 452 ms | 303 ms | 226 µs / 216 µs / 444 µs | 228 µs | 1,996.17x (limit 2x) | 461.56x .. 2,349.6x | missed |
| relaxed | c1-spread | schedule p99.9 | 475 ms / 1.00 s / 735 ms | 526 ms | 1.1 ms / 976 µs / 2.2 ms | 1.2 ms | 643.34x (limit 5x) | 220.61x .. 1,024.5x | missed |
| relaxed | c2 | schedule p99 | 658 ms / 1.30 s / 659 ms | 637 ms | 487 µs / 213 µs / 471 µs | 274 µs | 1,398.92x (limit 2x) | 1,351.88x .. 6,081.5x | missed |
| relaxed | c2 | schedule p99.9 | 778 ms / 1.72 s / 878 ms | 941 ms | 2.9 ms / 1.8 ms / 3.1 ms | 1.2 ms | 300.62x (limit 5x) | 251.52x .. 932.5x | missed |
| group | c1-segregated-no0 | schedule p99 | 409 µs / 408 µs / 1.1 ms | 700 µs | 204 µs / 474 µs / 386 µs | 270 µs | 1.06x (limit 2x) | 0.86x .. 5.4x | met on medians; pairing decides |
| group | c1-segregated-no0 | schedule p99.9 | 2.9 ms / 3.3 ms / 24.2 ms | 21.4 ms | 976 µs / 2.7 ms / 2.5 ms | 1.7 ms | 1.32x (limit 5x) | 1.07x .. 24.8x | met on medians; pairing decides |
| group | c1-batch0 | schedule p99 | 606 µs / 761 µs / 748 µs | 155 µs | 220 µs / 632 µs / 504 µs | 412 µs | 1.48x (limit 2x) | 0.96x .. 3.5x | met on medians; pairing decides |
| group | c1-batch0 | schedule p99.9 | 9.6 ms / 3.0 ms / 5.1 ms | 6.6 ms | 1.9 ms / 2.6 ms / 2.8 ms | 860 µs | 1.95x (limit 5x) | 1.09x .. 5.1x | met on medians; pairing decides |
| group | c1-spread | schedule p99 | 975 µs / 940 µs / 1.0 ms | 76.8 µs | 228 µs / 389 µs / 541 µs | 313 µs | 2.50x (limit 2x) | 1.74x .. 4.5x | missed |
| group | c1-spread | schedule p99.9 | 19.3 ms / 14.1 ms / 5.1 ms | 14.2 ms | 1.6 ms / 1.4 ms / 2.1 ms | 675 µs | 8.95x (limit 5x) | 2.47x .. 13.8x | missed |
| group | c2 | schedule p99 | 307 ms / 320 ms / 327 ms | 20.0 ms | 11.1 ms / 10.9 ms / 10.9 ms | 161 µs | 29.27x (limit 2x) | 27.70x .. 29.9x | missed |
| group | c2 | schedule p99.9 | 341 ms / 403 ms / 381 ms | 61.4 ms | 18.9 ms / 20.1 ms / 20.6 ms | 1.6 ms | 18.99x (limit 5x) | 16.61x .. 21.3x | missed |

**Which readers.** The table reads every reader (or writer) in a cell,
wherever the balancer put it: BL-S3's placer moved sessions in every mixed
run (4-92 moves in the window in C1, 0-2 in the isolated arms),
`per_core_valid` is false in all 54 runs, and a session is attributed to the
core it connected on. "Cores with no bulk writer" is therefore only
approximated, and the inserter itself ended on a reader core in most C1 runs.
The checked analyses recomputed C1 on the readers whose connect core is not
the inserter's: relaxed spread p99 215 / 568 / 497 ms (against 205 / 508 / 452
ms for all readers) and group spread p99.9 21.8 / 14.7 / 6.8 ms (all readers:
19.3 / 14.1 / 5.1 ms). No verdict changes. C2's population is clean at both
durabilities: all 101 in-window relaxed C2 rolls and all 69 in-window group C2
rolls ran on core 3, and all eight writers sat on cores 0-2.

## 3. The roll: how many, how long, from what

Rolls per run are 33-37 at `relaxed` (524 over the 15 load-cell runs) and
23-25 at `group` (360); the per-run counts are in Appendix B. The §5.1
floor of 50 rolls per arm per durability is met by pooling a cell's three
runs (101-108 and 69-75) and by no single run. **p99.9 is not reported for
holds: with about 100 rolls per cell it is the maximum.**

Hold (µs from the latch taken to its release; pooled over a cell's three
runs, rolls in the window):

| durability | cell | rolls (r1 / r2 / r3) | p0 | p25 | p50 | p95 | p99 | max | per-run p50 (r1 / r2 / r3) | per-run max (r1 / r2 / r3) |
|---|---|---|---|---|---|---|---|---|---|---|
| relaxed | c3 | 36 / 36 / 36 | 8.3 ms | 330 ms | 359 ms | 575 ms | 794 ms | 911 ms | 359 ms / 364 ms / 349 ms, spread 14.8 ms | 673 ms / 911 ms / 794 ms, spread 238 ms |
| relaxed | c2 | 34 / 33 / 34 | 64.3 ms | 332 ms | 367 ms | 707 ms | 789 ms | 1.21 s | 369 ms / 374 ms / 365 ms, spread 8.8 ms | 789 ms / 1.21 s / 746 ms, spread 464 ms |
| relaxed | c1-segregated-no0 | 35 / 35 / 37 | 47.0 ms | 274 ms | 297 ms | 472 ms | 697 ms | 710 ms | 297 ms / 283 ms / 331 ms, spread 48.0 ms | 710 ms / 678 ms / 587 ms, spread 123 ms |
| relaxed | c1-batch0 | 36 / 35 / 33 | 16.9 ms | 278 ms | 318 ms | 565 ms | 722 ms | 725 ms | 326 ms / 291 ms / 317 ms, spread 35.7 ms | 654 ms / 722 ms / 725 ms, spread 71.1 ms |
| relaxed | c1-spread | 35 / 35 / 34 | 9.9 ms | 272 ms | 307 ms | 655 ms | 788 ms | 828 ms | 271 ms / 308 ms / 326 ms, spread 54.3 ms | 668 ms / 788 ms / 828 ms, spread 160 ms |
| relaxed | all five load cells | 524 | 8.3 ms | 290 ms | 331 ms | 655 ms | 788 ms | 1.21 s | | |
| group | c3 | 25 / 25 / 25 | 4.5 ms | 5.2 ms | 5.8 ms | 13.4 ms | 22.7 ms | 22.7 ms | 6.6 ms / 5.8 ms / 5.6 ms, spread 1.1 ms | 15.2 ms / 22.7 ms / 11.1 ms, spread 11.6 ms |
| group | c2 | 23 / 23 / 23 | 4.6 ms | 6.0 ms | 6.6 ms | 17.2 ms | 21.3 ms | 21.3 ms | 6.6 ms / 7.5 ms / 6.1 ms, spread 1.4 ms | 15.2 ms / 19.4 ms / 21.3 ms, spread 6.1 ms |
| group | c1-segregated-no0 | 24 / 24 / 23 | 4.2 ms | 5.3 ms | 5.7 ms | 13.3 ms | 20.6 ms | 20.6 ms | 5.6 ms / 5.6 ms / 5.8 ms, spread 169 µs | 20.6 ms / 14.0 ms / 13.7 ms, spread 6.9 ms |
| group | c1-batch0 | 25 / 24 / 24 | 4.5 ms | 5.4 ms | 6.1 ms | 13.1 ms | 17.8 ms | 17.8 ms | 5.9 ms / 5.8 ms / 6.6 ms, spread 836 µs | 17.8 ms / 13.1 ms / 15.9 ms, spread 4.7 ms |
| group | c1-spread | 24 / 24 / 24 | 4.7 ms | 5.2 ms | 6.0 ms | 13.2 ms | 28.7 ms | 28.7 ms | 5.6 ms / 5.5 ms / 6.1 ms, spread 635 µs | 11.6 ms / 28.7 ms / 13.8 ms, spread 17.1 ms |
| group | all five load cells | 360 | 4.2 ms | 5.3 ms | 6.1 ms | 13.7 ms | 20.6 ms | 28.7 ms | | |

The hold is long at `relaxed` and short at `group`, and it is **not** a
function of how much log there is: at `relaxed` the correlation of a roll's
hold with the live segment count is 0.02 (analyst) / 0.06 (checker), with
the gap since the previous roll -0.17, and with the waited-out sync's tail
time 0.53-0.56. The time under a hold, per run:

| durability | cell | roll hold, summed per run (r1 / r2 / r3) | share of the 30 s window under a hold (r1 / r2 / r3) |
|---|---|---|---|
| relaxed | c3 | 13.5 s / 13.3 s / 12.8 s | 45.1 % / 44.2 % / 42.8 % |
| relaxed | c2 | 13.9 s / 13.8 s / 13.2 s | 45.4 % / 45.6 % / 44.2 % |
| relaxed | c1-segregated-no0 | 11.4 s / 10.4 s / 12.0 s | 38.4 % / 34.9 % / 39.9 % |
| relaxed | c1-batch0 | 12.1 s / 11.3 s / 11.2 s | 40.6 % / 37.8 % / 37.3 % |
| relaxed | c1-spread | 10.6 s / 11.7 s / 12.7 s | 35.1 % / 39.3 % / 42.3 % |
| group | c3 | 0.2 s / 0.2 s / 0.2 s | 0.6 % / 0.6 % / 0.5 % |
| group | c2 | 0.2 s / 0.2 s / 0.2 s | 0.6 % / 0.7 % / 0.6 % |
| group | c1-segregated-no0 | 0.2 s / 0.2 s / 0.2 s | 0.5 % / 0.6 % / 0.5 % |
| group | c1-batch0 | 0.2 s / 0.2 s / 0.2 s | 0.6 % / 0.5 % / 0.6 % |
| group | c1-spread | 0.2 s / 0.2 s / 0.2 s | 0.5 % / 0.6 % / 0.6 % |

### The step split

Shares of the summed hold. At `relaxed` step c1 — the gate — is 92-97 %;
at `group` the hold is the sum of three fixed device operations (the
roll's own tail sync, the header's `fdatasync`, the directory `fsync`) and
the gate is 12 %.

| durability | cell | rolls | a | b | c1 | c2 | c3 | d | e | f | g | residual |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| relaxed | c3 | 108 | 0.2 % | 0.0 % | 96.8 % | 0.0 % | 0.3 % | 0.6 % | 0.9 % | 0.0 % | 1.2 % | 0.0 % |
| relaxed | c2 | 101 | 0.0 % | 0.0 % | 92.4 % | 0.0 % | 2.6 % | 1.0 % | 2.7 % | 0.0 % | 1.2 % | 0.0 % |
| relaxed | c1-segregated-no0 | 107 | 0.0 % | 0.0 % | 96.9 % | 0.0 % | 0.4 % | 0.8 % | 0.9 % | 0.0 % | 1.0 % | 0.0 % |
| relaxed | c1-batch0 | 104 | 0.8 % | 0.0 % | 95.7 % | 0.2 % | 0.6 % | 0.3 % | 1.2 % | 0.0 % | 1.1 % | 0.0 % |
| relaxed | c1-spread | 104 | 0.0 % | 0.0 % | 96.8 % | 0.0 % | 0.7 % | 0.5 % | 0.9 % | 0.0 % | 1.1 % | 0.0 % |
| relaxed | all five | 524 | 0.2 % | 0.0 % | 95.6 % | 0.1 % | 1.0 % | 0.6 % | 1.3 % | 0.0 % | 1.1 % | 0.0 % |
| group | c3 | 75 | 0.3 % | 0.0 % | 4.0 % | 29.1 % | 3.4 % | 0.0 % | 20.2 % | 0.4 % | 42.3 % | 0.2 % |
| group | c2 | 69 | 0.1 % | 0.0 % | 39.0 % | 1.8 % | 2.8 % | 0.0 % | 16.8 % | 0.4 % | 38.9 % | 0.2 % |
| group | c1-segregated-no0 | 71 | 0.5 % | 0.0 % | 6.3 % | 30.9 % | 3.5 % | 0.0 % | 18.3 % | 0.6 % | 39.7 % | 0.2 % |
| group | c1-batch0 | 73 | 0.4 % | 0.0 % | 6.0 % | 28.9 % | 3.5 % | 0.0 % | 20.5 % | 0.4 % | 40.2 % | 0.2 % |
| group | c1-spread | 72 | 0.5 % | 0.0 % | 3.1 % | 36.8 % | 3.8 % | 0.0 % | 17.3 % | 0.4 % | 38.0 % | 0.2 % |
| group | all five | 360 | 0.4 % | 0.0 % | 12.2 % | 25.1 % | 3.4 % | 0.0 % | 18.6 % | 0.4 % | 39.8 % | 0.2 % |

Step names: a seal, b flush, c1 sync gate, c2 own sync tail, c3 own sync
non-tail, d body, e header, f rename, g directory fsync. **The rolls at or
above each cell's hold p99** (H1's own definition) are 2 per cell at
`relaxed` and 1 at `group`, so each cell's row is a handful of maxima, not a
distribution; the all-rolls table above is the stable one.

| durability | cell | rolls | a | b | c1 | c2 | c3 | d | e | f | g | residual |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| relaxed | c3 | 2 | 0.0 % | 0.0 % | 91.3 % | 0.0 % | 0.1 % | 0.0 % | 5.5 % | 0.0 % | 3.1 % | 0.0 % |
| relaxed | c2 | 2 | 0.0 % | 0.0 % | 73.2 % | 0.0 % | 17.1 % | 0.0 % | 6.9 % | 0.1 % | 2.7 % | 0.0 % |
| relaxed | c1-segregated-no0 | 2 | 0.0 % | 0.0 % | 92.9 % | 0.0 % | 0.2 % | 6.4 % | 0.1 % | 0.0 % | 0.4 % | 0.0 % |
| relaxed | c1-batch0 | 2 | 0.0 % | 0.0 % | 99.2 % | 0.0 % | 0.4 % | 0.0 % | 0.2 % | 0.0 % | 0.3 % | 0.0 % |
| relaxed | c1-spread | 2 | 0.0 % | 0.0 % | 89.7 % | 0.0 % | 0.1 % | 9.8 % | 0.1 % | 0.0 % | 0.2 % | 0.0 % |
| relaxed | all five | 6 | 0.0 % | 0.0 % | 84.0 % | 0.0 % | 6.5 % | 3.0 % | 4.4 % | 0.0 % | 2.1 % | 0.0 % |
| group | c3 | 1 | 0.1 % | 0.0 % | 0.0 % | 24.0 % | 0.6 % | 0.0 % | 9.9 % | 0.1 % | 65.1 % | 0.0 % |
| group | c2 | 1 | 0.1 % | 0.0 % | 9.3 % | 0.2 % | 1.1 % | 0.0 % | 4.4 % | 0.1 % | 84.7 % | 0.1 % |
| group | c1-segregated-no0 | 1 | 0.2 % | 0.0 % | 0.0 % | 75.9 % | 1.3 % | 0.0 % | 11.2 % | 0.2 % | 11.2 % | 0.1 % |
| group | c1-batch0 | 1 | 0.1 % | 0.0 % | 15.0 % | 0.3 % | 1.4 % | 0.0 % | 6.5 % | 0.2 % | 76.4 % | 0.1 % |
| group | c1-spread | 1 | 0.1 % | 0.0 % | 0.0 % | 74.1 % | 1.4 % | 0.0 % | 17.0 % | 0.1 % | 7.1 % | 0.0 % |
| group | all five | 4 | 0.1 % | 0.0 % | 2.1 % | 45.5 % | 1.1 % | 0.0 % | 11.1 % | 0.1 % | 39.8 % | 0.1 % |

At `group` the gate is 39 % of C2's hold against 3-6 % elsewhere: C2's
writers keep the writer thread syncing 325 times a second, so a roll there
waits out about two syncs (1.93 per roll, against 0.2 in C1/C3; §4's last
table).

Absolute time per step, pooled over the five load cells:

| durability | step | p0 | p50 | p95 | p99 | max | summed over all rolls |
|---|---|---|---|---|---|---|---|
| relaxed | a seal | 3.0 µs | 30.0 µs | 60.0 µs | 78.0 µs | 281 ms | 0.4 s |
| relaxed | b flush | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 s |
| relaxed | c1 sync gate (waits out the writer thread's sync) | 2.7 ms | 324 ms | 582 ms | 718 ms | 903 ms | 176.0 s |
| relaxed | c2 own sync, tail fdatasync | 46.0 µs | 51.0 µs | 66.0 µs | 2.0 ms | 65.7 ms | 0.1 s |
| relaxed | c3 own sync, non-tail fdatasyncs | 126 µs | 1.1 ms | 2.1 ms | 94.3 ms | 341 ms | 1.8 s |
| relaxed | d body (wait on the preparer + inline build) | 0.0 µs | 0.0 µs | 1.7 ms | 99.9 ms | 158 ms | 1.2 s |
| relaxed | e header write + fdatasync | 852 µs | 1.1 ms | 31.8 ms | 90.8 ms | 239 ms | 2.5 s |
| relaxed | f rename | 17.0 µs | 32.0 µs | 41.0 µs | 525 µs | 6.5 ms | 0.0 s |
| relaxed | g directory fsync | 1.3 ms | 2.1 ms | 8.3 ms | 47.4 ms | 103 ms | 2.1 s |
| relaxed | c (1+2+3) step c total | 4.4 ms | 325 ms | 584 ms | 739 ms | 1.07 s | 177.8 s |
| relaxed | e+f+g | 2.6 ms | 3.3 ms | 42.2 ms | 105 ms | 242 ms | 4.6 s |
| relaxed | hold | 8.3 ms | 331 ms | 655 ms | 788 ms | 1.21 s | 184.0 s |
| group | a seal | 7.0 µs | 19.0 µs | 36.0 µs | 199 µs | 740 µs | 0.0 s |
| group | b flush | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 s |
| group | c1 sync gate (waits out the writer thread's sync) | 0.0 µs | 0.0 µs | 3.4 ms | 9.6 ms | 14.1 ms | 0.3 s |
| group | c2 own sync, tail fdatasync | 47.0 µs | 1.7 ms | 4.2 ms | 9.0 ms | 21.3 ms | 0.7 s |
| group | c3 own sync, non-tail fdatasyncs | 64.0 µs | 244 µs | 317 µs | 481 µs | 878 µs | 0.1 s |
| group | d body (wait on the preparer + inline build) | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 s |
| group | e header write + fdatasync | 893 µs | 1.1 ms | 3.2 ms | 6.1 ms | 8.8 ms | 0.5 s |
| group | f rename | 23.0 µs | 30.0 µs | 35.0 µs | 42.0 µs | 834 µs | 0.0 s |
| group | g directory fsync | 1.7 ms | 2.2 ms | 6.2 ms | 13.6 ms | 18.1 ms | 1.0 s |
| group | c (1+2+3) step c total | 1.2 ms | 2.3 ms | 6.7 ms | 11.7 ms | 21.7 ms | 1.1 s |
| group | e+f+g | 2.7 ms | 3.4 ms | 8.2 ms | 14.8 ms | 19.0 ms | 1.5 s |
| group | hold | 4.2 ms | 6.1 ms | 13.7 ms | 20.6 ms | 28.7 ms | 2.6 s |

Step a is under 80 µs at p99 in both durabilities. Its 281 ms maximum at
`relaxed` is one roll (seg 16) in C1 batch0 r3 — the run §9 flags for a
concurrent build — whose seal waited out a flush in flight (`SealLocked`
includes that wait; 0.8 % of that cell's hold); the next largest seal is
86 ms (C3 r1).

The body (step d) is ready when the roll arrives in 484 of 523 rolls at
`relaxed` (the order's counts: 39 waited, 0 inline) and in all 360 at
`group`:

| durability | cell | run | hit | waited | inline | body wait + inline build (summed) | missed rolls' hold / all hold |
|---|---|---|---|---|---|---|---|
| relaxed | c3 | r1 | 33 | 3 | 0 | 87.8 ms | 8.26 % |
| relaxed | c3 | r2 | 31 | 5 | 0 | 110 ms | 16.33 % |
| relaxed | c3 | r3 | 32 | 4 | 0 | 42.0 ms | 11.12 % |
| relaxed | c3 | pooled | | | | | 11.89 % |
| relaxed | c2 | r1 | 32 | 2 | 0 | 138 ms | 7.81 % |
| relaxed | c2 | r2 | 29 | 4 | 0 | 241 ms | 15.27 % |
| relaxed | c2 | r3 | 32 | 2 | 0 | 33.2 ms | 5.02 % |
| relaxed | c2 | pooled | | | | | 9.42 % |
| relaxed | c1-segregated-no0 | r1 | 30 | 5 | 0 | 156 ms | 17.02 % |
| relaxed | c1-segregated-no0 | r2 | 32 | 3 | 0 | 104 ms | 12.35 % |
| relaxed | c1-segregated-no0 | r3 | 36 | 1 | 0 | 2.3 ms | 2.80 % |
| relaxed | c1-segregated-no0 | pooled | | | | | 10.54 % |
| relaxed | c1-batch0 | r1 | 34 | 2 | 0 | 3.4 ms | 7.27 % |
| relaxed | c1-batch0 | r2 | 32 | 3 | 0 | 7.9 ms | 11.99 % |
| relaxed | c1-batch0 | r3 | 30 | 3 | 0 | 88.1 ms | 10.50 % |
| relaxed | c1-batch0 | pooled | | | | | 9.85 % |
| relaxed | c1-spread | r1 | 35 | 0 | 0 | 0.0 µs | 0.00 % |
| relaxed | c1-spread | r2 | 33 | 2 | 0 | 159 ms | 9.05 % |
| relaxed | c1-spread | r3 | 34 | 0 | 0 | 0.0 µs | 0.00 % |
| relaxed | c1-spread | pooled | | | | | 3.03 % |
| group | c3 | r1 | 25 | 0 | 0 | 0.0 µs | 0.00 % |
| group | c3 | r2 | 25 | 0 | 0 | 0.0 µs | 0.00 % |
| group | c3 | r3 | 25 | 0 | 0 | 0.0 µs | 0.00 % |
| group | c3 | pooled | | | | | 0.00 % |
| group | c2 | r1 | 23 | 0 | 0 | 0.0 µs | 0.00 % |
| group | c2 | r2 | 23 | 0 | 0 | 0.0 µs | 0.00 % |
| group | c2 | r3 | 23 | 0 | 0 | 0.0 µs | 0.00 % |
| group | c2 | pooled | | | | | 0.00 % |
| group | c1-segregated-no0 | r1 | 24 | 0 | 0 | 0.0 µs | 0.00 % |
| group | c1-segregated-no0 | r2 | 24 | 0 | 0 | 0.0 µs | 0.00 % |
| group | c1-segregated-no0 | r3 | 23 | 0 | 0 | 0.0 µs | 0.00 % |
| group | c1-segregated-no0 | pooled | | | | | 0.00 % |
| group | c1-batch0 | r1 | 25 | 0 | 0 | 0.0 µs | 0.00 % |
| group | c1-batch0 | r2 | 24 | 0 | 0 | 0.0 µs | 0.00 % |
| group | c1-batch0 | r3 | 24 | 0 | 0 | 0.0 µs | 0.00 % |
| group | c1-batch0 | pooled | | | | | 0.00 % |
| group | c1-spread | r1 | 24 | 0 | 0 | 0.0 µs | 0.00 % |
| group | c1-spread | r2 | 24 | 0 | 0 | 0.0 µs | 0.00 % |
| group | c1-spread | r3 | 24 | 0 | 0 | 0.0 µs | 0.00 % |
| group | c1-spread | pooled | | | | | 0.00 % |

## 4. Finding (a): the relaxed hold is a sync waited out

A roll seals and flushes (a, b: microseconds), then needs the segment it is
leaving to be durable before it creates the next one. The sync mutex is held
by the WAL writer thread, which is in the middle of — or about to start — a
sync of the same tail. The roll takes the gate after it, and **the gate is
not first-in-first-out**: at `relaxed` every roll waited out one or two
writer syncs, two at p25 and above. Step c is then a wait on someone else's
`fdatasync`, and the roll's own is a no-op on a tail the writer just made
clean (p50 51 µs).

Per roll, pooled over the five load cells:

| durability | quantity (per roll) | rolls | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| relaxed | hold | 524 | 8.3 ms | 290 ms | 331 ms | 655 ms | 788 ms | 1.21 s |
| relaxed | sync gate wait (step c1) | 524 | 2.7 ms | 282 ms | 324 ms | 582 ms | 718 ms | 903 ms |
| relaxed | writer-sync tail fdatasync waited out | 524 | 2.4 ms | 256 ms | 302 ms | 660 ms | 896 ms | 957 ms |
| relaxed | writer-sync non-tail fdatasyncs waited out | 524 | 141 µs | 2.1 ms | 50.2 ms | 343 ms | 578 ms | 892 ms |
| relaxed | writer syncs waited out (count, not µs) | 524 | 1 | 2 | 2 | 2 | 2 | 2 |
| relaxed | live segments held at the roll's sync (count) | 524 | 3 | 12 | 20 | 36 | 38 | 39 |
| group | hold | 360 | 4.2 ms | 5.3 ms | 6.1 ms | 13.7 ms | 20.6 ms | 28.7 ms |
| group | sync gate wait (step c1) | 360 | 0.0 µs | 0.0 µs | 0.0 µs | 3.4 ms | 9.6 ms | 14.1 ms |
| group | writer-sync tail fdatasync waited out | 360 | 0.0 µs | 0.0 µs | 0.0 µs | 4.0 ms | 10.3 ms | 15.7 ms |
| group | writer-sync non-tail fdatasyncs waited out | 360 | 0.0 µs | 0.0 µs | 0.0 µs | 468 µs | 632 µs | 1.2 ms |
| group | writer syncs waited out (count, not µs) | 360 | 0 | 0 | 0 | 2 | 2 | 2 |
| group | live segments held at the roll's sync (count) | 360 | 2 | 5 | 5 | 6 | 6 | 6 |

The waited-out syncs' tail `fdatasync` is 256-660 ms per roll between p25
and p95 (302 ms at the median), over two syncs, so about 150 ms each: that
is the body of the hold. The non-tail descriptors the same syncs cover add a
median 50 ms (p95 343 ms); a sync already in flight when the wait began is
counted whole, so waited tail plus rest can exceed the gate wait (by 1.0-1.5x
in the rolls at or above p99).

**How slow a sync is, against the cadence.** The writer thread syncs 27-36
times per second in these `relaxed` runs, 2.1-2.8 MiB of log per sync. Of the
~900 syncs in a C3 run, the **71-72 that a roll waited out total 19.1-20.1 s
(269-279 ms each)** of the 30 s window. Syncs are serialized behind one
mutex, so the other ~820 syncs together took at most ~10-11 s: **a mean of
12-15 ms or less**. The slow ones are the ones that span a segment boundary.
The instrument counts only syncs a roll waited, so the duration of an
ordinary sync is bounded here, not measured. At `group` the writer syncs
138-152 times per second in C1/C3 (0.34-0.36 MiB each; 325-340 in C2, 0.15
MiB each), only 2-9 of a run's ~4,500 syncs are waited by a roll, and the
roll's own tail sync takes 1.7 ms.

| durability | cell | run | writer syncs/s (wal_syncs) | log MiB per sync | syncs inline | syncs by the writer thread |
|---|---|---|---|---|---|---|
| relaxed | c3 | r1 | 29.8 | 2.55 | 17 | 876 |
| relaxed | c3 | r2 | 31.3 | 2.40 | 8 | 931 |
| relaxed | c3 | r3 | 27.4 | 2.77 | 8 | 813 |
| relaxed | c2 | r1 | 26.7 | 2.65 | 12 | 808 |
| relaxed | c2 | r2 | 27.6 | 2.61 | 12 | 819 |
| relaxed | c2 | r3 | 28.1 | 2.64 | 11 | 827 |
| relaxed | c2-iso | r1 | 101.7 | 0.00 | 0 | 3060 |
| relaxed | c2-iso | r2 | 98.9 | 0.00 | 0 | 2974 |
| relaxed | c2-iso | r3 | 103.1 | 0.00 | 0 | 3101 |
| relaxed | c1-segregated-no0 | r1 | 36.1 | 2.09 | 4 | 1071 |
| relaxed | c1-segregated-no0 | r2 | 35.4 | 2.14 | 4 | 1051 |
| relaxed | c1-segregated-no0 | r3 | 32.0 | 2.45 | 15 | 947 |
| relaxed | c1-iso-segregated-no0 | r1 | 1.7 | 0.00 | 0 | 50 |
| relaxed | c1-iso-segregated-no0 | r2 | 1.6 | 0.00 | 0 | 48 |
| relaxed | c1-iso-segregated-no0 | r3 | 1.6 | 0.00 | 0 | 48 |
| group | c3 | r1 | 152.3 | 0.34 | 0 | 4572 |
| group | c3 | r2 | 150.8 | 0.34 | 0 | 4526 |
| group | c3 | r3 | 150.6 | 0.35 | 0 | 4520 |
| group | c2 | r1 | 339.5 | 0.15 | 32 | 10186 |
| group | c2 | r2 | 324.1 | 0.15 | 42 | 9792 |
| group | c2 | r3 | 325.9 | 0.15 | 32 | 9841 |
| group | c2-iso | r1 | 729.3 | 0.00 | 0 | 21945 |
| group | c2-iso | r2 | 728.1 | 0.00 | 0 | 21909 |
| group | c2-iso | r3 | 731.9 | 0.00 | 0 | 22025 |
| group | c1-segregated-no0 | r1 | 148.5 | 0.34 | 0 | 4469 |
| group | c1-segregated-no0 | r2 | 152.0 | 0.34 | 0 | 4575 |
| group | c1-segregated-no0 | r3 | 137.6 | 0.36 | 0 | 4141 |
| group | c1-iso-segregated-no0 | r1 | 1.6 | 0.00 | 0 | 48 |
| group | c1-iso-segregated-no0 | r2 | 1.6 | 0.00 | 0 | 48 |
| group | c1-iso-segregated-no0 | r3 | 1.6 | 0.00 | 0 | 48 |

**What the device was asked to do.** Per 30 s of a `relaxed` C3 run: 2.2 GiB
of log appended (75-76 MiB/s), one 64 MiB segment created and its header,
rename and directory entry fsynced per roll (36 rolls), and 4-5 checkpoint
runs writing 40,000 pages (314 MiB, 10.5 MiB/s of page writeback, 1.4-3.5 s
per run). The log's bytes are 7x the writeback's. A concurrent preparer
builds each next body ahead (step d); how many bytes its prewrite adds to a
device that is already taking the log was not measured (H4's third
re-exploration option is the experiment that bounds it). The checkpoint
runs in every load-cell run, `relaxed` then `group` (`log_gate_run waits` is
the number of runs that waited inline on the log gate; `data_park_us` the
longest wait on the data-file sync):

| durability | cell | run | checkpoint runs in the 30 s window | run wall (min / median / max) | pages written (sum) | MiB written (8 KiB pages) | MiB/s | log MiB/s | log_gate_run waits (runs with n>0) | data_park_us (max) |
|---|---|---|---|---|---|---|---|---|---|---|
| relaxed | c3 | r1 | 4 | 1.38 s / 2.06 s / 3.55 s | 40,176 | 314 | 10.5 | 75.8 | 3 of 4 | 651 ms |
| relaxed | c3 | r2 | 5 | 1.42 s / 2.60 s / 3.30 s | 40,546 | 317 | 10.6 | 75.2 | 4 of 5 | 746 ms |
| relaxed | c3 | r3 | 4 | 1.68 s / 2.73 s / 3.06 s | 40,145 | 314 | 10.4 | 75.8 | 2 of 4 | 922 ms |
| relaxed | c2 | r1 | 5 | 946 ms / 3.21 s / 4.76 s | 43,442 | 339 | 11.0 | 70.8 | 4 of 5 | 1.12 s |
| relaxed | c2 | r2 | 4 | 2.18 s / 3.38 s / 4.92 s | 35,069 | 274 | 9.1 | 72.1 | 4 of 4 | 1.02 s |
| relaxed | c2 | r3 | 4 | 2.94 s / 3.46 s / 4.87 s | 35,544 | 278 | 9.3 | 74.1 | 4 of 4 | 882 ms |
| relaxed | c1-segregated-no0 | r1 | 4 | 1.36 s / 1.80 s / 2.66 s | 39,330 | 307 | 10.3 | 75.3 | 3 of 4 | 702 ms |
| relaxed | c1-segregated-no0 | r2 | 4 | 1.50 s / 2.08 s / 2.61 s | 39,954 | 312 | 10.5 | 75.6 | 3 of 4 | 698 ms |
| relaxed | c1-segregated-no0 | r3 | 4 | 1.19 s / 1.69 s / 1.87 s | 25,905 | 202 | 6.7 | 78.3 | 3 of 4 | 111 ms |
| relaxed | c1-batch0 | r1 | 5 | 1.13 s / 1.59 s / 2.54 s | 27,353 | 214 | 7.2 | 77.9 | 4 of 5 | 630 ms |
| relaxed | c1-batch0 | r2 | 4 | 1.43 s / 1.95 s / 2.98 s | 40,403 | 316 | 10.6 | 75.6 | 3 of 4 | 584 ms |
| relaxed | c1-batch0 | r3 | 5 | 1.53 s / 2.05 s / 4.83 s | 36,381 | 284 | 9.4 | 70.4 | 3 of 5 | 656 ms |
| relaxed | c1-spread | r1 | 6 | 198 ms / 1.86 s / 2.11 s | 43,091 | 337 | 11.2 | 75.1 | 5 of 6 | 697 ms |
| relaxed | c1-spread | r2 | 4 | 1.45 s / 1.70 s / 2.69 s | 42,268 | 330 | 11.1 | 75.3 | 2 of 4 | 126 ms |
| relaxed | c1-spread | r3 | 4 | 1.49 s / 2.21 s / 2.32 s | 38,898 | 304 | 10.1 | 71.7 | 3 of 4 | 620 ms |
| group | c3 | r1 | 24 | 46.1 ms / 184 ms / 393 ms | 39,115 | 306 | 10.2 | 52.5 | 0 of 24 | 144 ms |
| group | c3 | r2 | 24 | 37.5 ms / 131 ms / 366 ms | 38,930 | 304 | 10.1 | 52.0 | 0 of 24 | 63.0 ms |
| group | c3 | r3 | 24 | 43.5 ms / 208 ms / 412 ms | 39,174 | 306 | 10.2 | 52.3 | 0 of 24 | 95.4 ms |
| group | c2 | r1 | 24 | 131 ms / 237 ms / 707 ms | 33,494 | 262 | 8.7 | 49.7 | 20 of 24 | 22.7 ms |
| group | c2 | r2 | 24 | 107 ms / 278 ms / 625 ms | 32,833 | 257 | 8.5 | 48.1 | 23 of 24 | 212 ms |
| group | c2 | r3 | 24 | 107 ms / 287 ms / 627 ms | 33,335 | 260 | 8.6 | 48.1 | 24 of 24 | 311 ms |
| group | c1-segregated-no0 | r1 | 25 | 31.1 ms / 45.0 ms / 82.5 ms | 41,816 | 327 | 10.9 | 50.4 | 0 of 25 | 66.0 ms |
| group | c1-segregated-no0 | r2 | 24 | 25.2 ms / 38.5 ms / 73.3 ms | 23,227 | 181 | 6.0 | 51.5 | 0 of 24 | 18.0 ms |
| group | c1-segregated-no0 | r3 | 25 | 30.8 ms / 45.1 ms / 341 ms | 40,551 | 317 | 10.5 | 49.3 | 0 of 25 | 53.9 ms |
| group | c1-batch0 | r1 | 25 | 33.5 ms / 45.4 ms / 84.3 ms | 42,676 | 333 | 11.1 | 51.3 | 0 of 25 | 31.8 ms |
| group | c1-batch0 | r2 | 25 | 33.0 ms / 51.1 ms / 61.9 ms | 42,475 | 332 | 11.0 | 51.4 | 0 of 25 | 24.9 ms |
| group | c1-batch0 | r3 | 24 | 38.3 ms / 46.8 ms / 398 ms | 39,691 | 310 | 10.3 | 50.1 | 0 of 24 | 57.3 ms |
| group | c1-spread | r1 | 24 | 30.1 ms / 47.9 ms / 111 ms | 40,008 | 313 | 10.4 | 50.6 | 0 of 24 | 38.7 ms |
| group | c1-spread | r2 | 25 | 32.1 ms / 44.6 ms / 86.1 ms | 41,982 | 328 | 10.9 | 50.8 | 0 of 25 | 23.1 ms |
| group | c1-spread | r3 | 25 | 34.3 ms / 44.8 ms / 66.5 ms | 42,261 | 330 | 11.0 | 51.0 | 0 of 25 | 30.8 ms |

**Why the sync is slow is not in the data.** The mechanism the numbers fit is
ext4's: an `fdatasync` of a file the bulk writer dirties at 75-80 MiB/s,
while another thread syncs the next segment's body and a checkpoint writes
pages, can wait on the journal commit or a device cache flush that all of them
share, and whichever descriptor is synced first pays it. No kernel trace was
taken, so this stays a reading of the evidence.

### The bound that matters for rung 3

If the breadth of the sync were the cost, narrowing it to the segments
written since the last sync would remove the non-tail share. Subtracting,
from each roll's hold, **both** the roll's own non-tail sync and the whole
non-tail part of every writer sync it waited out (an overcount, since a
waited sync counted whole may have begun before the wait) is the most
narrowing could take. `h3_bound.py` over every roll in the `relaxed` logs
(115, 112, 111, 110 and 114 rolls per cell, so including the few outside the
window):

| Cell (`relaxed`) | rolls | hold p50 | hold p99 | p50 with every non-tail sync removed | **p99 with every non-tail sync removed** |
|---|---|---|---|---|---|
| C1 segregated-no0 | 115 | 299 ms | 697 ms | 236 ms | **693 ms** |
| C1 batch0 | 112 | 318 ms | 722 ms | 272 ms | **717 ms** |
| C1 spread | 111 | 307 ms | 788 ms | 53 ms | **652 ms** |
| C2 | 110 | 369 ms | 789 ms | 322 ms | **673 ms** |
| C3 | 114 | 360 ms | 794 ms | 273 ms | **780 ms** |

The bar's p99 limit (2x the isolated p99) is 0.5-1.1 ms. With every
non-tail sync gone, rung 3 (H3) stays at least 590x above it; the tail
`fdatasync`s of the writer's syncs are what is left.

**The breadth is real, and small.** The roll's own non-tail sync grows from a
median 359-391 µs over the first quarter of rolls (4-5 live segments) to
1.7-1.8 ms over the last (35-37 live), about 50 µs per segment; at `group`
it stays 186-270 µs over 3-5 live segments:

| durability | cell | live segments at first / last roll (per run median) | own non-tail sync p50 first quarter | last quarter | waited-out writer syncs per roll | waited tail p50 | waited tail p99 | waited tail max | waited non-tail p50 | waited non-tail max |
|---|---|---|---|---|---|---|---|---|---|---|
| relaxed | c3 | 4 / 37 | 359 µs | 1.8 ms | 1.98 | 349 ms | 898 ms | 957 ms | 4.0 ms | 353 ms |
| relaxed | c2 | 5 / 36 | 391 µs | 1.8 ms | 1.99 | 483 ms | 802 ms | 896 ms | 4.4 ms | 813 ms |
| relaxed | c1-segregated-no0 | 5 / 37 | 366 µs | 1.7 ms | 1.96 | 301 ms | 742 ms | 887 ms | 4.1 ms | 308 ms |
| relaxed | c1-batch0 | 5 / 37 | 371 µs | 1.8 ms | 1.97 | 306 ms | 804 ms | 942 ms | 3.3 ms | 306 ms |
| relaxed | c1-spread | 4 / 35 | 368 µs | 1.8 ms | 1.98 | 263 ms | 913 ms | 945 ms | 235 ms | 892 ms |
| group | c3 | 3 / 5 | 205 µs | 245 µs | 0.19 | 0.0 µs | 4.2 ms | 4.2 ms | 0.0 µs | 238 µs |
| group | c2 | 3 / 5 | 186 µs | 224 µs | 1.93 | 3.2 ms | 15.7 ms | 15.7 ms | 433 µs | 1.2 ms |
| group | c1-segregated-no0 | 3 / 5 | 188 µs | 243 µs | 0.21 | 0.0 µs | 7.8 ms | 7.8 ms | 0.0 µs | 949 µs |
| group | c1-batch0 | 3 / 5 | 216 µs | 248 µs | 0.19 | 0.0 µs | 10.3 ms | 10.3 ms | 0.0 µs | 257 µs |
| group | c1-spread | 3 / 5 | 220 µs | 270 µs | 0.17 | 0.0 µs | 3.0 ms | 3.0 ms | 0.0 µs | 314 µs |

That growth is what finding (b) feeds.

## 5. Finding (b): the log is not recycled at `relaxed`

The recycler removes segments wholly below the durable redo start of an
anchor a sync made durable (BC). At `group` that works: **20-22 segments
removed per run, 5-6 live at the last roll, the fold advancing about one
segment per 1.25 s checkpoint run.** At `relaxed` under the same load it
does not: **0 removed in 13 of 15 runs, 33-39 live segments** at the last
roll, the sync breadth above growing with them. The two exceptions
removed 2 (C1 spread r1) and 4 (C1 batch0 r3) segments.

Per C3 run (the cell with no other load), counters over the window:

| durability | run | `wal_segments_removed` | `wal_first_segment` at the end | live segments at the last roll | checkpoint runs in the window | wall per run |
|---|---|---|---|---|---|---|
| relaxed | r1 / r2 / r3 | 0 / 0 / 0 | 0 / 0 / 0 | 38 / 38 / 38 | 4 / 5 / 4 | 1.4-3.5 s |
| group | r1 / r2 / r3 | 21 / 21 / 21 | 21 / 21 / 21 | 5 / 5 / 5 | 24 / 24 / 24 | 38-412 ms |

The causes, from the anchor lines and the code at `4cf30270` (a read of
`src/wal/checkpointer.cpp`, `src/storage/device_page_store.cpp`,
`include/kds/wal/checkpointer.hpp` and `src/server/superblock_checkpoint_anchor.cpp`
by an analyst, then two adversarial checkers; one link was corrected and is
stated as corrected):

1. **One page pins the redo start.** A page dirtied at LSN 5120 during the
   DDL phase, before the load, stays dirty with that recLSN through three or
   four full checkpoint runs. `StampPageLsn` sets `rec_lsn` only when it is
   unset (`device_page_store.cpp:1726-1730`); a writeback copies the page
   under its latch, writes the copy, and cleans the frame **only if nothing
   re-dirtied it since the copy** (`:2134-2136`), and otherwise leaves it
   dirty with its old `rec_lsn`. The step cursor still advances, so the run
   completes and the page reaches the next core's snapshot at recLSN 5120.
   `RedoStartFrom` takes the minimum recLSN of the dirty table, and the
   fold takes the minimum over every core's latest anchor, so
   `DetachBelow` sees a bound inside segment 0 and returns with nothing
   detached.
2. **`relaxed` widens the copy-to-clean window.** A hot page's `page_lsn` is
   almost never durable at `relaxed`, so the run defers 16 times
   (`kGateParks`) on a moving target and then waits inline (`log_gate_run`),
   during which the page is re-dirtied. Every pin release in the three C3
   runs came right after a run with `log_gate_run_n = 0`; every snapshot
   still pinned followed a run with `log_gate_run_n = 1`. At `group` every
   run has `log_gate_run_n = 0` and the page is cleaned by the second run.
   This is a correlation across three releases, not a proof, and which page
   it is was not identified (Info-level logs carry no page ids; the
   `sys.tables` page holding the `bl_*` rows, whose `next_id` is bumped on
   every insert, is the likeliest).
3. **The rotation stretches each core to ~25 s (the corrected link).** A run
   lasts 1.1-3.5 s at `relaxed`, longer than the 1.25 s stagger between
   cores. **The gate refuses a core whose turn it is not**
   (`CheckpointGate::Turn()`, BI-S2, `checkpointer.hpp:340-355`) as well as
   one that finds the gate held: when a run swallows the next core's tick,
   every other core's tick is refused until that core's next tick a full
   period later. Load-time runs are then `period + period/cores` = 6.25 s
   apart, and each core runs about every 25 s. `wal.md` §11 states the
   rotation and that a skip costs the core's contribution to the fold a later
   redo start; it does not state the 6.25 s / 25 s consequence.
   In r1 and r3 the pin had released by 18.75 s and the fold still waited on
   cores whose next turn fell after the 30 s window closed.

The timeline of one `relaxed` C3 run, and the same run's first checkpoint
runs at `group` for contrast (core, start, wall, dirty pages, gate waits
inside the run, the redo start that run published and the fold minimum
after it, in 64 MiB segments):

**`relaxed` C3** (all three runs; the last row of r1 and r3 is core 0 again)

| run | core | starts at | wall | dirty pages at start | log-gate waits inside the run | redo start published (segment) | instance redo start, the fold minimum (segment) |
|---|---|---|---|---|---|---|---|
| r1 | 0 | 0.00 s | 1.69 s | 3,793 | 1 | 0.00 | 0.00 |
| r1 | 1 | 6.25 s | 2.18 s | 11,472 | 1 | 0.00 | 0.00 |
| r1 | 2 | 12.50 s | 1.38 s | 10,298 | 1 | 0.00 | 0.00 |
| r1 | 3 | 18.75 s | 1.94 s | 11,311 | 0 | 0.00 | 0.00 |
| r1 | 0 | 25.19 s | 3.55 s | 7,095 | 3 | 26.00 | 0.00 |
| r2 | 0 | 0.00 s | 1.12 s | 3,491 | 1 | 0.00 | 0.00 |
| r2 | 1 | 1.25 s | 1.42 s | 1,153 | 1 | 0.00 | 0.00 |
| r2 | 2 | 7.50 s | 2.77 s | 12,097 | 1 | 0.00 | 0.00 |
| r2 | 3 | 13.75 s | 2.60 s | 9,592 | 0 | 0.00 | 0.00 |
| r2 | 0 | 20.11 s | 2.10 s | 9,852 | 1 | 20.00 | 0.00 |
| r2 | 1 | 26.25 s | 3.30 s | 7,852 | 3 | 20.38 | 0.00 |
| r3 | 0 | 0.00 s | 1.53 s | 3,796 | 1 | 0.00 | 0.00 |
| r3 | 1 | 6.25 s | 3.06 s | 11,502 | 1 | 0.00 | 0.00 |
| r3 | 2 | 12.50 s | 1.68 s | 10,320 | 0 | 0.00 | 0.00 |
| r3 | 3 | 18.75 s | 2.60 s | 11,206 | 0 | 17.52 | 0.00 |
| r3 | 0 | 25.19 s | 2.86 s | 7,117 | 1 | 26.00 | 0.00 |

**`group` C3 r1** (first 12 of 24 runs)

| run | core | starts at | wall | dirty pages at start | log-gate waits inside the run | redo start published (segment) | instance redo start, the fold minimum (segment) |
|---|---|---|---|---|---|---|---|
| r1 | 0 | 0.00 s | 0.06 s | 2,294 | 0 | 0.00 | 0.00 |
| r1 | 1 | 1.25 s | 0.05 s | 1,696 | 0 | 1.38 | 0.00 |
| r1 | 2 | 2.50 s | 0.39 s | 1,756 | 0 | 2.43 | 0.00 |
| r1 | 3 | 3.75 s | 0.39 s | 1,679 | 0 | 3.50 | 0.00 |
| r1 | 0 | 5.00 s | 0.34 s | 1,613 | 0 | 4.54 | 1.38 |
| r1 | 1 | 6.25 s | 0.29 s | 1,636 | 0 | 5.53 | 2.43 |
| r1 | 2 | 7.50 s | 0.25 s | 1,614 | 0 | 6.55 | 3.50 |
| r1 | 3 | 8.75 s | 0.25 s | 1,591 | 0 | 7.54 | 4.54 |
| r1 | 0 | 10.00 s | 0.20 s | 1,590 | 0 | 8.54 | 5.53 |
| r1 | 1 | 11.25 s | 0.19 s | 1,662 | 0 | 9.52 | 6.55 |
| r1 | 2 | 12.50 s | 0.16 s | 1,603 | 0 | 10.54 | 7.54 |
| r1 | 3 | 13.75 s | 0.16 s | 1,614 | 0 | 11.54 | 8.54 |
| r2 | 0 | 0.00 s | 0.06 s | 2,340 | 0 | 0.00 | 0.00 |
| r2 | 1 | 1.25 s | 0.06 s | 1,442 | 0 | 1.42 | 0.00 |
| r2 | 2 | 2.50 s | 0.05 s | 1,649 | 0 | 2.31 | 0.00 |
| r2 | 3 | 3.75 s | 0.06 s | 1,825 | 0 | 3.33 | 0.00 |
| r2 | 0 | 5.00 s | 0.04 s | 1,668 | 0 | 4.47 | 1.42 |
| r2 | 1 | 6.25 s | 0.37 s | 1,672 | 0 | 5.50 | 2.31 |
| r2 | 2 | 7.50 s | 0.31 s | 1,648 | 0 | 6.53 | 3.33 |
| r2 | 3 | 8.75 s | 0.26 s | 1,608 | 0 | 7.54 | 4.47 |
| r2 | 0 | 10.00 s | 0.25 s | 1,613 | 0 | 8.54 | 5.50 |
| r2 | 1 | 11.25 s | 0.21 s | 1,638 | 0 | 9.53 | 6.53 |
| r2 | 2 | 12.50 s | 0.21 s | 1,602 | 0 | 10.54 | 7.54 |
| r2 | 3 | 13.75 s | 0.21 s | 1,624 | 0 | 11.53 | 8.54 |
| r3 | 0 | 0.00 s | 0.07 s | 2,140 | 0 | 0.00 | 0.00 |
| r3 | 1 | 1.25 s | 0.05 s | 1,661 | 0 | 1.29 | 0.00 |
| r3 | 2 | 2.50 s | 0.05 s | 1,720 | 0 | 2.32 | 0.00 |
| r3 | 3 | 3.75 s | 0.05 s | 1,697 | 0 | 3.39 | 0.00 |
| r3 | 0 | 5.00 s | 0.04 s | 1,590 | 0 | 4.45 | 1.29 |
| r3 | 1 | 6.25 s | 0.41 s | 1,777 | 0 | 5.43 | 2.32 |
| r3 | 2 | 7.50 s | 0.41 s | 1,639 | 0 | 6.53 | 3.39 |
| r3 | 3 | 8.75 s | 0.35 s | 1,600 | 0 | 7.54 | 4.45 |
| r3 | 0 | 10.00 s | 0.31 s | 1,601 | 0 | 8.54 | 5.43 |
| r3 | 1 | 11.25 s | 0.26 s | 1,639 | 0 | 9.54 | 6.53 |
| r3 | 2 | 12.50 s | 0.25 s | 1,590 | 0 | 10.54 | 7.54 |
| r3 | 3 | 13.75 s | 0.22 s | 1,649 | 0 | 11.53 | 8.54 |

The pin is released only in a snapshot taken after a run with
`log_gate_run_n = 0`, and then only the minimum moves: at `relaxed` the
fold's last load-time value is [26.0, 0, 0, 0] in r1 (one core published a
redo start in segment 26, three still in segment 0).

**Classification, with the checker's reasoning.** The behaviour follows from
rules `wal.md` states (§11 step 2: a page is cleaned only if nothing
re-dirtied it after the copy; §11: the fold is the minimum over every core's
latest anchor; a skip costs a later redo start) and from what it does not
state: a frame left dirty by a generation mismatch keeps the recLSN from
before the copy indefinitely, and the rotation's cost once runs outlast
`period/cores`. §11-4's "when the bound does not move" list names only
interval 0, the warm-up and anchor 0. The retention is safe (a recLSN that is
too low replays more, never less), but its liveness has no bound in code: a
page re-dirtied inside every copy-to-clean window would pin the redo start
forever, and the log, each sync's breadth and a crash's replay range would
grow with it. No raise of the recLSN after a written copy exists, and the
obvious one - to just above the copy's `page_lsn` - is unsound, because a
record appended before its page is held (`AssertionEnforcer::AbortTxn`) can
be stamped below a later one; the bug entry gives a rule that does not lean
on stamp order. **`known-gaps.md`'s "Every
sync covers every live segment" entry says that since BC the run is
bounded; this measurement is the case where it is not.** The defect entry is
`docs/inflight/bugs/a-page-re-dirtied-after-its-writeback-copy-pins-the-redo-start.md`.

## 6. Who waits for a hold

One WAL stream, one latch: every core that appends waits out a roll, and core
0 waits through the drain that runs on its reactor. The table gives, for each
run, core 0's waits, the other non-roller cores' stream-latch waits and the
roller core's gate wait. **Roll-covered** is the part of a stream-latch wait
that the last roll's `[start, end]` covered (it tests only the last roll, so
a wait spanning two rolls is under-counted).

| durability | cell | run | window hold (all rolls, summed) | core 0: stream wait | core 0: drain wait | core 0: roll-covered | non-0 non-roller cores: stream wait (sum) | their roll-covered (sum) | roller core(s): sync-gate wait |
|---|---|---|---|---|---|---|---|---|---|
| relaxed | c3 | r1 | 13532 ms | 13354 ms | 13354 ms | 13353 ms | 326 ms | 325 ms | 13124 ms |
| relaxed | c3 | r2 | 13266 ms | 13109 ms | 13103 ms | 13107 ms | 731 ms | 731 ms | 12963 ms |
| relaxed | c3 | r3 | 12846 ms | 12635 ms | 12635 ms | 12634 ms | 1039 ms | 1039 ms | 12281 ms |
| relaxed | c2 | r1 | 13943 ms | 13832 ms | 5269 ms | 13829 ms | 26468 ms | 26458 ms | 13111 ms |
| relaxed | c2 | r2 | 13754 ms | 13011 ms | 4429 ms | 13007 ms | 25940 ms | 25929 ms | 12417 ms |
| relaxed | c2 | r3 | 13189 ms | 12349 ms | 4041 ms | 12345 ms | 24782 ms | 24770 ms | 12263 ms |
| relaxed | c1-segregated-no0 | r1 | 11439 ms | 11255 ms | 11255 ms | 11254 ms | 212 ms | 212 ms | 10982 ms |
| relaxed | c1-segregated-no0 | r2 | 10391 ms | 10174 ms | 10174 ms | 10173 ms | 386 ms | 385 ms | 10000 ms |
| relaxed | c1-segregated-no0 | r3 | 11995 ms | 11445 ms | 11441 ms | 11444 ms | 17.3 ms | 17.2 ms | 11793 ms |
| relaxed | c1-batch0 | r1 | 12128 ms | 11934 ms | 11934 ms | 11932 ms | 829 ms | 829 ms | 11820 ms |
| relaxed | c1-batch0 | r2 | 11252 ms | 11066 ms | 11066 ms | 11065 ms | 157 ms | 157 ms | 10971 ms |
| relaxed | c1-batch0 | r3 | 11250 ms | 10766 ms | 10766 ms | 10757 ms | 505 ms | 504 ms | 10362 ms |
| relaxed | c1-spread | r1 | 10555 ms | 10349 ms | 10349 ms | 10348 ms | 378 ms | 378 ms | 10213 ms |
| relaxed | c1-spread | r2 | 11717 ms | 6257 ms | 6257 ms | 6256 ms (core 0 also rolled) | 11.9 ms | 11.9 ms | 11359 ms |
| relaxed | c1-spread | r3 | 12745 ms | 12015 ms | 12015 ms | 12014 ms (core 0 also rolled) | 262 ms | 262 ms | 12317 ms |
| group | c3 | r1 | 184 ms | 15.2 ms | 15.2 ms | 4.5 ms | 0.01 ms | 0 ms | 12.9 ms |
| group | c3 | r2 | 180 ms | 77.9 ms | 77.9 ms | 67.3 ms | 0 ms | 0 ms | 4.7 ms |
| group | c3 | r3 | 161 ms | 37.3 ms | 37.3 ms | 34.1 ms | 0 ms | 0 ms | 3.5 ms |
| group | c2 | r1 | 176 ms | 181 ms | 98.6 ms | 147 ms | 321 ms | 270 ms | 68.9 ms |
| group | c2 | r2 | 200 ms | 182 ms | 113 ms | 158 ms | 445 ms | 353 ms | 82.7 ms |
| group | c2 | r3 | 182 ms | 195 ms | 145 ms | 160 ms | 345 ms | 291 ms | 66.1 ms |
| group | c1-segregated-no0 | r1 | 162 ms | 35.4 ms | 35.4 ms | 35.3 ms | 10.1 ms | 10.0 ms | 10.8 ms |
| group | c1-segregated-no0 | r2 | 170 ms | 23.5 ms | 23.5 ms | 23.4 ms | 1.9 ms | 1.9 ms | 9.4 ms |
| group | c1-segregated-no0 | r3 | 153 ms | 31.0 ms | 30.9 ms | 28.7 ms (core 0 also rolled) | 0.01 ms | 0 ms | 10.4 ms |
| group | c1-batch0 | r1 | 176 ms | 36.7 ms | 36.7 ms | 36.5 ms (core 0 also rolled) | 0 ms | 0 ms | 6.4 ms |
| group | c1-batch0 | r2 | 158 ms | 34.8 ms | 34.8 ms | 32.0 ms | 0 ms | 0 ms | 3.5 ms |
| group | c1-batch0 | r3 | 180 ms | 66.0 ms | 66.0 ms | 56.1 ms | 0.01 ms | 0 ms | 21.0 ms |
| group | c1-spread | r1 | 158 ms | 16.6 ms | 16.6 ms | 16.4 ms | 0.01 ms | 0 ms | 4.4 ms |
| group | c1-spread | r2 | 184 ms | 74.7 ms | 74.7 ms | 58.8 ms | 0 ms | 0 ms | 6.6 ms |
| group | c1-spread | r3 | 171 ms | 36.0 ms | 36.0 ms | 30.9 ms | 3.8 ms | 3.8 ms | 4.9 ms |

At `relaxed` in C1 and C3, **core 0's drain wait is the whole of its
stream-latch wait and covers 94-99 % of the summed hold** (53 % in C1 spread
r2, where 15 of its 35 rolls ran on core 0, which has no drain to block while
rolling). The drain runs in the reactor's post-task hook, so core 0 stops
for the roll's length:

| durability | cell | run | core 0 drain wait / window | core 0 scheduler stalls over 100 ms outside a poll (sched_hold_out_run_over_100ms, m1 - m0) | rolls in window | longest stall outside a poll (sched_hold_out_run_longest_us, m1) |
|---|---|---|---|---|---|---|
| relaxed | c3 | r1 | 13.4 s / 30.0 s = 44 % | 35 | 36 | 673 ms |
| relaxed | c3 | r2 | 13.1 s / 30.0 s = 44 % | 38 | 36 | 903 ms |
| relaxed | c3 | r3 | 12.6 s / 30.0 s = 42 % | 34 | 36 | 791 ms |
| relaxed | c1-segregated-no0 | r1 | 11.3 s / 29.8 s = 38 % | 34 | 35 | 689 ms |
| relaxed | c1-segregated-no0 | r2 | 10.2 s / 29.8 s = 34 % | 38 | 35 | 416 ms |
| relaxed | c1-segregated-no0 | r3 | 11.4 s / 30.1 s = 38 % | 37 | 37 | 579 ms |
| relaxed | c1-batch0 | r1 | 11.9 s / 29.9 s = 40 % | 36 | 36 | 646 ms |
| relaxed | c1-batch0 | r2 | 11.1 s / 29.8 s = 37 % | 36 | 35 | 720 ms |
| relaxed | c1-batch0 | r3 | 10.8 s / 30.2 s = 36 % | 32 | 33 | 716 ms |
| relaxed | c1-spread | r1 | 10.3 s / 30.1 s = 34 % | 30 | 35 | 659 ms |
| relaxed | c1-spread | r2 | 6.3 s / 29.8 s = 21 % | 36 | 35 | 796 ms |
| relaxed | c1-spread | r3 | 12.0 s / 30.1 s = 40 % | 32 | 34 | 824 ms |
| group | c3 | r1 | 0.0 s / 30.0 s = 0 % | 0 | 25 | 39.8 ms |
| group | c3 | r2 | 0.1 s / 30.0 s = 0 % | 1 | 25 | 240 ms |
| group | c3 | r3 | 0.0 s / 30.0 s = 0 % | 0 | 25 | 38.6 ms |
| group | c1-segregated-no0 | r1 | 0.0 s / 30.1 s = 0 % | 0 | 24 | 39.7 ms |
| group | c1-segregated-no0 | r2 | 0.0 s / 30.1 s = 0 % | 0 | 24 | 42.9 ms |
| group | c1-segregated-no0 | r3 | 0.0 s / 30.1 s = 0 % | 0 | 23 | 51.6 ms |
| group | c1-batch0 | r1 | 0.0 s / 30.1 s = 0 % | 0 | 25 | 58.4 ms |
| group | c1-batch0 | r2 | 0.0 s / 30.1 s = 0 % | 0 | 24 | 10.5 ms |
| group | c1-batch0 | r3 | 0.1 s / 30.1 s = 0 % | 0 | 24 | 42.9 ms |
| group | c1-spread | r1 | 0.0 s / 30.1 s = 0 % | 0 | 24 | 48.6 ms |
| group | c1-spread | r2 | 0.1 s / 30.1 s = 0 % | 0 | 24 | 69.6 ms |
| group | c1-spread | r3 | 0.0 s / 30.1 s = 0 % | 0 | 24 | 42.8 ms |

One scheduler stall of over 100 ms outside a poll per roll: 30-38 against
33-37 rolls. A reader parked on core 0 is stalled for a hold; the other
non-roller cores wait 0.02-1.0 s in total over the 30 s. In C2 the writers' own
appends wait: each of cores 0-2 waits 12.0-13.8 s of 30 s on the stream
latch, ~100 % roll-covered, and the drain is only 33-38 % of core 0's
(redundant with the writers' own wait). Which cores sessions were on when a
sample was taken is not recorded: the driver keeps only each session's first
and last core, and the stall attribution rests on three observations — one
reader stalled per roll in long contiguous stretches, exactly one reader on
core 0 at the end of the run in 8 of 9 runs, and none on the roller core.

**Alignment.** Of the C1 reader samples of 10 ms or more, 63-99 % of the stall
time is aligned with a roll (overlapping it and completing within -5..+30 ms
of its end), while rolls cover 35-42 % of the wall; relaxed C2 writers 91-97
%. At `group`, C2's aligned share is 0.5-0.9 % (64 / 121 / 77 of ~12,500
samples). The checked analyses also find that 57-90 % of the large reader
stalls that do *not* line up with a roll fall inside a checkpoint run
(1.1-4.8 s each at `relaxed`): a second holder, indicative only, which the
H5 ceiling (2.6-145 ms p99) reflects.

### Wait types, per 30 s window, summed over cores (mean of 3 runs)

| durability | cell | wal stream latch | WAL sync gate | frame table | window latch | lock partition | free map | roll-covered share of the stream-latch wait |
|---|---|---|---|---|---|---|---|---|
| relaxed | c3 | 13.73 s | 12.79 s | 71.0 ms | 41.2 ms | 0.0 µs | 33.0 µs | 100 % |
| relaxed | c2 | 38.80 s | 12.60 s | 210 ms | 44.0 ms | 565 µs | 913 µs | 100 % |
| relaxed | c1-segregated-no0 | 11.16 s | 10.93 s | 1.55 s | 805 ms | 728 µs | 251 µs | 100 % |
| relaxed | c1-batch0 | 11.75 s | 11.08 s | 1.52 s | 772 ms | 1.6 ms | 318 µs | 100 % |
| relaxed | c1-spread | 10.12 s | 11.30 s | 1.37 s | 709 ms | 697 µs | 350 µs | 100 % |
| relaxed | c2-iso | 0.7 µs | 0.0 µs | 726 µs | 73.0 µs | 204 µs | 0.0 µs | 0 % |
| relaxed | c1-iso-segregated-no0 | 0.0 µs | 0.0 µs | 99.7 µs | 3.3 µs | 0.0 µs | 0.0 µs | n/a (no stream wait) |
| group | c3 | 43.6 ms | 7.7 ms | 46.8 ms | 39.6 ms | 0.0 µs | 9.7 µs | 69 % |
| group | c2 | 560 ms | 350 ms | 122 ms | 11.9 ms | 1.1 ms | 56.0 µs | 82 % |
| group | c1-segregated-no0 | 34.1 ms | 10.2 ms | 1.00 s | 427 ms | 435 µs | 304 µs | 97 % |
| group | c1-batch0 | 46.2 ms | 10.3 ms | 524 ms | 812 ms | 2.3 ms | 326 µs | 91 % |
| group | c1-spread | 43.9 ms | 5.3 ms | 497 ms | 810 ms | 5.8 ms | 327 µs | 88 % |
| group | c2-iso | 20.0 µs | 0.0 µs | 1.0 ms | 178 µs | 15.7 µs | 0.3 µs | 0 % |
| group | c1-iso-segregated-no0 | 0.0 µs | 0.0 µs | 407 µs | 0.3 µs | 0.3 µs | 0.0 µs | n/a (no stream wait) |

## 7. The bulk writer: C3, and what the stalls cost it

C3 is the cell BQ-Q2's term (iii) is read against (a candidate may cost it
at most 3 %). Its base, with the spread of its three runs:

| durability | run | C3 rows/s | log MiB/s (append-LSN delta over m0..m1) | log MiB/s excluding the summed roll hold | rolls | hold total / window |
|---|---|---|---|---|---|---|
| relaxed | r1 | 169,643 | 75.76 | 138.0 | 36 | 45.1 % |
| relaxed | r2 | 168,427 | 75.24 | 134.8 | 36 | 44.2 % |
| relaxed | r3 | 169,766 | 75.83 | 132.6 | 36 | 42.8 % |
| relaxed | mean (spread = max - min) | 169,278 (spread 1,339 = 0.79 %) | 75.61 (spread 0.60) | | | |
| group | r1 | 117,421 | 52.46 | 52.8 | 25 | 0.6 % |
| group | r2 | 116,324 | 51.96 | 52.3 | 25 | 0.6 % |
| group | r3 | 117,150 | 52.33 | 52.6 | 25 | 0.5 % |
| group | mean (spread = max - min) | 116,965 (spread 1,098 = 0.94 %) | 52.25 (spread 0.50) | | | |

- **Term (iii)'s base.** `relaxed`: **169,278 rows/s, 75.61 MiB/s of log;
  spread 1,339 rows/s (0.79 %)**, so 3 % is 5,078 rows/s and four times the
  spread. `group`: **116,965 rows/s, 52.25 MiB/s; spread 1,098 rows/s
  (0.94 %)**, 3 % is 3,509 rows/s.
- **At `relaxed` C3 is roll-bound.** The rolls hold the latch for 43-45 % of
  the window, during which no append runs; the log rate over the window
  *outside* holds is 133-138 MiB/s against the achieved 75.6. A cheaper roll
  would more likely raise C3 than lower it, so term (iii) is a guard on a
  candidate that adds cost elsewhere (a changed sync, a changed flush), not on
  the latch itself.
- **At `group` C3 is commit-bound**, not roll-bound: holds are 0.5-0.6 % of
  the window and the rate is the same with them excluded (52.3-52.8 MiB/s);
  the writer syncs 150 times per second (§4).
- The inserter's statements (1,024 rows) have a p50 of 2.7-3.4 ms at
  `relaxed` and 5.2-5.9 ms at `group`, and a maximum of 0.59-1.21 s at
  `relaxed` (a hold) and 0.31-0.34 s at `group`; Appendix D has every run.
  Beside readers or writers the inserter's mean rate against C3's: `relaxed`
  C1 segregated-no0 +1.4 %, batch0 -1.0 %, spread -1.8 %, C2 -2.8 %
  (those cells' own run-to-run spreads are 3k-17k rows/s, 2-10 %, so
  none of these is a finding); `group` C1 -3.2 % / -2.3 % / -2.3 % and C2
  -6.4 %, each more than C3's 0.94 % spread, with spreads of their own of
  0.6k-5.1k rows/s (C2: 2.7k = 2.4 %), so C2's -6.4 % is the one delta here
  outside both.

## 8. Accounting for the waits (a latency is a sum)

For a `relaxed` C1 reader sitting at the p99 (288-452 ms schedule latency):

| Component | What the data says |
|---|---|
| Client and socket round trip, the read itself | the isolated arm's service p50 is 105-120 µs (p0 85-106 µs); the body of the mixed arm is the same (126-131 µs) |
| **Roll hold on its core's reactor** | the stall is the hold: the cell's hold has p50 297 ms, p95 472 ms, p99 697 ms (segregated-no0), and 63-99 % of the stall time of 10 ms+ samples is aligned with a roll. Reached through core 0's drain, which is 100 % of core 0's stream-latch wait |
| of which: the sync gate (a writer sync waited out) | 92-97 % of the hold (§3); two syncs, ~150 ms of tail `fdatasync` each (§4) |
| of which: the roll's own sync, body, header, rename, directory | 0.6-1.3 % each at `relaxed` (own sync c2+c3 1.1 %, body d 0.6 %, header e 1.3 %, directory g 1.1 %) |
| Schedule pile-up behind a stalled reader | a 300 ms stall makes ~20 of one reader's ~15 ms-spaced sends late (§2): schedule p99 288 ms against a service p99 of 1.1 ms |
| Lock or conflict wait | none: `errors` and `refusals` are 0 in all 54 runs; the lock-partition wait totals 0.7-1.6 ms in 30 s |
| Commit / fsync of the reader | not applicable: readers do not commit a log record since `05cd011f` |
| Other latches | frame table 1.4-1.6 s and window latch 0.7-0.8 s summed over all cores and statements in 30 s (C1), against 10-12 s of stream-latch wait |

For a `relaxed` C2 writer (p99 659 ms; autocommit, so its commit has no sync
of its own at `relaxed`): the wait is its own append on the stream latch,
**12.0-13.8 s of 30 s per writer core, ~100 % roll-covered**, and nothing
else is comparable (frame table 0.21 s, window latch 0.04 s, summed). At
`group` a writer's statement is dominated by its commit's sync: service p50
3.4-3.6 ms against 1.1 ms isolated, with the WAL writer syncing 325 times a
second against 729 isolated and 0.15 MiB per sync.

The same roll at `group`, as a sum: a median 6.1 ms = own tail sync 1.7
ms + header 1.1 ms + directory 2.2 ms + the rest. The columns of the wait
table (stream latch 34-46 ms in C1/C3 at `group`, 560 ms in C2 summed over
cores) are small beside `relaxed`'s 10-39 s because the roll is.

## 9. Host, noise floor and tool defects

**Host.** The gate before every cell required no compiler, `cmake --build`,
`ctest`, test binary or BP run and a 1-minute load under 2.5; it waited
6,790 s before the first (smoke) cell, 410 s before the BL pair, and 10-530 s
at 9 further cells (530 s once, 10 s eight times). Every cell's `rc` was 0.
Load before each run was 0.89-2.49, after each 0.89-3.16 (on 8 vCPUs; the
isolated arms start at the upper end because the 1-minute average still
holds the mixed arm that ran just before them). The 1-minute load is the only
load record besides the per-run `competing` list:

| durability | cell | 1-min load before the run (r1 / r2 / r3) | 1-min load after the run (r1 / r2 / r3) | competing processes listed by the run's two host stamps (r1 / r2 / r3) |
|---|---|---|---|---|
| group | c3 | 1.38 / 1.18 / 1.28 | 1.51 / 1.24 / 1.57 | 0 / 0 / 0 |
| group | c2 | 1.51 / 1.24 / 1.57 | 1.56 / 1.36 / 1.71 | 0 / 0 / 0 |
| group | c2-iso | 1.56 / 1.36 / 1.71 | 1.11 / 1.09 / 1.41 | 0 / 0 / 0 |
| group | c1-segregated-no0 | 1.11 / 1.09 / 1.41 | 2.02 / 1.65 / 1.65 | 0 / 0 / 0 |
| group | c1-iso-segregated-no0 | 2.02 / 1.65 / 1.65 | 1.23 / 0.92 / 0.95 | 0 / 0 / 0 |
| group | c1-batch0 | 1.23 / 0.92 / 0.95 | 1.54 / 1.70 / 1.66 | 0 / 0 / 0 |
| group | c1-iso-batch0 | 1.54 / 1.70 / 1.66 | 1.31 / 1.13 / 1.07 | 0 / 0 / 0 |
| group | c1-spread | 1.31 / 1.13 / 1.07 | 2.03 / 1.40 / 1.76 | 0 / 0 / 0 |
| group | c1-iso-spread | 2.03 / 1.40 / 1.76 | 1.10 / 0.89 / 0.97 | 0 / 0 / 0 |
| relaxed | c3 | 2.27 / 1.10 / 0.89 | 2.63 / 1.92 / 1.81 | 0 / 0 / 0 |
| relaxed | c2 | 2.31 / 1.92 / 1.81 | 2.82 / 2.33 / 2.20 | 0 / 0 / 0 |
| relaxed | c2-iso | 2.38 / 2.33 / 2.20 | 1.43 / 1.58 / 1.30 | 0 / 0 / 0 |
| relaxed | c1-segregated-no0 | 1.43 / 1.58 / 1.30 | 2.70 / 2.49 / 1.97 | 0 / 0 / 0 |
| relaxed | c1-iso-segregated-no0 | 2.17 / 2.49 / 1.97 | 1.27 / 1.58 / 1.01 | 0 / 0 / 0 |
| relaxed | c1-batch0 | 1.27 / 1.58 / 1.01 | 1.91 / 2.52 / 3.16 | 0 / 0 / 10 |
| relaxed | c1-iso-batch0 | 1.91 / 2.48 / 2.24 | 1.24 / 1.56 / 1.23 | 0 / 0 / 0 |
| relaxed | c1-spread | 1.24 / 1.56 / 1.23 | 2.37 / 2.31 / 2.06 | 0 / 0 / 0 |
| relaxed | c1-iso-spread | 2.37 / 2.31 / 2.06 | 1.38 / 1.18 / 1.28 | 0 / 0 / 0 |

**One run overlapped a build.** `relaxed` C1 batch0 r3 recorded, in its
post-run host stamp, a `cmake --build -j8` with eight `cc1plus` processes from
another session's worktree (`br-backup-restore`); its pre-run stamp was
clean, the start of the build inside the run is not recorded, and the gate
held the next cell for 530 s. That run has the matrix's lowest relaxed inserter
rate (158,427 rows/s; the next lowest is 160,996) and 33 rolls, the fewest.
It is kept in every table; without it the cell's relaxed schedule p99 is
354 / 430 ms (spread 76 ms, unchanged) and its p99.9 593 / 674 ms, and no
verdict moves.

**Noise floor, from inside the run.** Every cell has three runs and every mixed
arm an isolated control (the control's own spread is the floor):

- the isolated schedule p99 spreads 173-345 µs (`relaxed`) and 161-412 µs
  (`group`);
- the three runs of a `relaxed` mixed cell spread 76-637 ms at p99 (C2:
  637 ms), so *between-placement* differences in C1 (288 / 366 / 452 ms) are
  inside the runs' own spread and are not findings;
- `group` C1: segregated-no0 and batch0 differ from isolated by less than the
  isolated arm's own spread (p99 1.06x and 1.48x; spread 270 and 412 µs on
  204-632 µs), so **no group C1 delta is reported as a result** except
  spread's p99.9 (8.95x, outside the floor);
- C3's rows/s spread is 0.79 % (`relaxed`) and 0.94 % (`group`); the C1 and C2
  inserters differ from C3 by 1-3 % at `relaxed` (inside those cells' own
  spreads) and 2-6 % at `group` (§7).

**Tool defects found while reading the raw files** (they change what a reader
of the unfiltered output would conclude, not what is above):

1. `analyze_s1.py` hard-codes the bulk core (`bulk = 0 if 'batch0' in cell
   else 3`); BL-S3's placer moved the inserter in most runs (it rolled on
   core 1 in every relaxed segregated-no0 run, on core 1 / 3 / 3 in batch0, on
   core 2 / {0,1,3} / {0,3} in spread), so its "non-bulk stream wait" includes
   the bulk writer's core and drops a reader's. It reports 2.4 % roll-covered
   for `relaxed` C1 batch0 r1 where the recomputed figure is 100 %.
2. `bl_mixed.py`'s `meta_delta` is built only from keys present in `meta_m0`,
   and `SHOW META` omits a per-core field while it is 0, so a counter that
   was 0 at `m0` vanishes (core 0's roll-covered wait of 10.3 s in `relaxed`
   C1 spread r1 is dropped; group C2 r3's core 1, 150 ms). It also
   differences two running maxima: `wal_roll_hold_max_us` reads 241,615 in
   C1 batch0 r1's `meta_delta` where the run's maximum is 653,644. This file
   reads `meta_m1` and `meta_m0` itself.
3. `analyze_s1.py` groups steps as seal / flush / gate / sync / "create"
   (which merges d, e, f, g) and sums over every roll or only the p99 bin
   without saying so; its "H3 rest/stepc" adds waited syncs' rest time
   (whole syncs that may have begun before the wait) to a denominator of
   gate plus own sync and can pass 100 %.
4. Per-sample core placement is not recorded: the driver keeps each session's
   connect core and end core, and attributes latency by the connect core.

## 10. What the run teaches about the engine

1. **What the order called "the roll's I/O under the latch" is, at `relaxed`,
   a wait for someone else's I/O.** Steps c-g are 99 % of the hold, and 96 %
   of that is the gate. `wal.md` §6 step 5 ends "A sync can therefore wait out
   one already in flight; that cost is not measured": it is now: the gate is
   95.6 % of the summed `relaxed` hold (176.0 of 184.0 s over 524 rolls) and
   12.2 % of the `group` hold. At `group` the same roll is its own three
   device operations (tail sync 1.7 ms, header `fdatasync` 1.1 ms, directory `fsync` 2.2 ms at
   the median), about 6 ms, all three fixed costs of making a segment
   durable that no sync-narrowing removes (the directory `fsync` alone is
   39.8 % of the `group` hold).
2. **`relaxed` makes the roll expensive, not cheap.** Without commit syncs the
   log is made durable by the writer's interval sync, 27-36 per second of
   2.1-2.8 MiB, and the segment boundary lands among them; the syncs that
   span it take 270 ms each against a mean of at most 12-15 ms for the rest.
   At `group` a sync follows every 6.6 ms of log (0.34 MiB), the tail is
   nearly clean, and the boundary's sync is 1.7 ms. Whether it is the
   cadence (fewer, larger syncs) or the byte rate that makes the boundary
   slow is confounded in these runs: `group` also logs 52 MiB/s against
   `relaxed`'s 75. What the roll waits on at `relaxed` is that sync, not its
   own I/O, and a change to the latch's span does not shorten it.
3. **Releasing the latch (rung 4) is capped by the ring at `relaxed`.**
   At 70-78 MiB/s the 1 MiB stream ring fills in 12.8-14.2 ms (6.4-7.1 ms at
   the 512 KiB floor) against a median hold of 331 ms: 520 of 523 `relaxed`
   holds outlast the ring's fill time (523 of 523 at the floor). At `group`,
   48-52.5 MiB/s fills 1 MiB in 19-21 ms against a 6 ms hold: 4 of 360 holds
   outlast it (47 of 360 at the floor). This is the checked analysis's
   estimate from the achieved rate, a lower bound on the offered rate; no
   counter can see the ring fill during a latched roll.
4. **A pure reader is stalled by the drain, not by the latch.** Since
   `05cd011f` a point reader appends nothing, yet its schedule p99 is 288-452
   ms: the group-commit drain runs in core 0's post-task hook, takes the
   stream latch, and so blocks core 0's whole reactor for the hold (34-44 %
   of the window, 21 % in the one run below; one scheduler stall of 100 ms or
   more per roll), and a reader parked on core 0 waits with it. Peers carry no drain at `relaxed`.
   Where the roller itself sat on core 0 (15 of 35 rolls in C1 spread r2)
   there was no drain to block and core 0's drain wait was 6.3 s, not 10-12 s.
5. **At `group` the roll is invisible to readers and C2's tail belongs to
   something else.** C1 differs from isolated inside the noise floor; C2's
   29x has 0 of 240 slow samples ending within 20 ms of a roll (chance 1.5 %),
   and 24-40 % of its 50 ms+ samples overlap a checkpoint run. The shape of the
   whole distribution is shifted (schedule p50 13.9 ms vs 1.2 ms isolated,
   service p50 3.5 ms vs 1.1 ms; the writer syncs 325 times a second against
   729): commit syncs behind the bulk writer's device traffic, a standing
   open-loop backlog. Under H0's own rule it is a stall with another holder,
   to be named in `known-gaps.md`, not a roll finding.
6. **BC's recycling bound does not hold at `relaxed` under a bulk load**
   (§5): one page re-dirtied across its writeback copy pins the redo start in
   segment 0, and the checkpoint gate's rotation stretches each core to ~25 s.
   The live run is bounded by nothing the code states; it grows at the log's rate
   (about 2.4 GiB of live segments after 38 rolls), and every sync pays for it.
   That a `relaxed` instance under a bulk load keeps its whole log is, today,
   the engine's behaviour.
7. **The missed body is not the problem on this host.** The preparer is ahead
   of the roll in 484 of 523 rolls at `relaxed` and 360 of 360 at `group`;
   a miss costs 0.33-0.83 % of C3's hold (the body wait itself, 3-100 ms), and
   missed rolls hold as long as hit rolls (355 vs 362 ms) because the sync gate
   dominates them. H1's prediction that d carries a share is wrong at 64 MiB
   segments and 75-82 MiB/s. The preparer's zero prewrite and `fsync`
   compete with the log for the device, and whether that indirect cost is
   inside the sync gate is the one part of H4 these data cannot bound.
8. **The order's predictions that the data corrects.** H1's "steps e-g
   together are a few milliseconds": true at the median (3.3-3.4 ms at both
   durabilities), false in the tail (105 ms at p99 and 242 ms at the maximum
   at `relaxed`; 14.8 and 19.0 ms at `group`, 58.8 % of the group hold) — the
   header and directory syncs are fixed costs that grow when the device is busy.
   H3's "the share grows with the live run" is true (§4: 360 µs to 1.8 ms), and
   is small. `known-gaps.md` WAL ("since BC the run is bounded") is the entry
   finding (b) contradicts.

### Upper bounds these data put on each rung

Not verdicts on the rungs (their reject criteria run in BQ-S2 and BQ-S3); the
most a rung could take off the relaxed C1/C2 tails, from the checked
analyses. The bar's p99 limit (2x the isolated p99) is 0.5-1.1 ms.

| Rung | Bound from BQ-S1's data | Source (the checked analysis) |
|---|---|---|
| 1, H4 options 1-2 (body at a quarter segment, two bodies ahead) | removing **every** microsecond of step d from the p99 leaves 106-1,055 ms (C1 seg-no0 106-286 ms, batch0 278-426, spread 205-452; C2 521-1,055); step d is 0.3-1.8 % of hold | H4 |
| 1, H4 option 3 (skip the zero prewrite where `posix_fallocate` succeeds) | **not bounded**: acts through device contention on the gate, which these data cannot price | H4 caveat |
| 1, H2 (larger segment) | a projection, not measured (only 64 MiB was run): the hold does not follow the live segment count, so a larger segment makes rolls rarer, not shorter; at 1,024 MiB about 2.4-4.2 % of wall time would still sit under a hold, above the 1 % that p99 tolerates | the bar and H2-premise analysis |
| 2, H5 (non-blocking drain) | with every roll-aligned stall removed, C1 reader p99 is 2.6-145 ms, never within 2x of isolated; p99.9 56-220 ms | H5 |
| 3, H3 (narrow the sync) | p99 with every non-tail sync removed: 652-780 ms (§4) | `h3_bound.py` |
| 4, H6 (release the latch for steps c-g) | capped by the 1 MiB ring at `relaxed` (§10 item 3) | the bar and H6-cap analysis |

## 11. What this file does not establish

- **The three-size row-set sweep** (§0): not run; the roll's cost does not
  depend on a relation's rows, and nothing at 200 or 1,000 rows was measured.
- **`strict` durability** was not measured (§5.1 names `relaxed` and `group`);
  the deferred-commit path (`CommitDeferred`) and its parked committers
  across a roll are untested here.
- **One segment size (64 MiB), one ring (1 MiB), one core count (4), one
  filesystem (ext4 on one Azure NVMe disk, journal mode default), one host
  with the clients on the reactors' SMT siblings.** No other device was
  tried; whether a dedicated WAL device removes the sync wait is CN-9's
  question, not this file's.
- **The mechanism of the slow sync** (the journal commit or a device flush
  shared with the preparer's body and the checkpoint) is a reading of the
  evidence; no kernel trace was taken, and the duration of an *ordinary*
  sync is bounded by arithmetic (§4), not measured.
- **Which core a sample ran on** is not recorded; "cores with no bulk writer"
  is approximated by connect core, and the reader-on-core-0 attribution in §6 is
  an inference from three observations.
- **Which page pins the redo start** was not identified (Info logs carry no
  page ids); the `log_gate_run` correlation is across three releases.
- **The instrument's own limits.** `ChargeRollOverlap` tests only the last
  roll, so a wait spanning two rolls is under-counted (a group-commit
  drain's roll-covered share is 30-100 % of its wait for that reason);
  `wal_drain_latch_wait_us` charges any holder of the latch, not only a roll;
  slot 0 of the contention counters is shared with threads that declared no
  core. The tail/rest split follows the order the descriptors are synced in.
- **The hold's p99 bin is a handful of rolls:** 2 per cell at `relaxed`, 1 at
  `group`; H1 at `group` is borderline for that
  reason, and the leading step is not stable run to run.
- **Overhead** of the instruments (one `steady_clock` read per step, an Info
  line per roll, a mutex on the sync totals) is not measured here; BQ-S5 does.
- **Tree cleanliness** at the build, and `git describe` of the build's source
  as the build itself saw it, are not recorded; the binary's hash is.

## Appendix A. Every run's latency percentiles

Schedule latency, `relaxed`:

| cell | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| c2 | r1 | 23741 | 112 µs | 174 µs | 2.4 ms | 459 ms | 658 ms | 778 ms | 820 ms |
| c2 | r2 | 24007 | 92.7 µs | 169 µs | 7.1 ms | 666 ms | 1.30 s | 1.72 s | 1.78 s |
| c2 | r3 | 24006 | 99.2 µs | 167 µs | 291 µs | 412 ms | 659 ms | 878 ms | 1.09 s |
| c2-iso | r1 | 24007 | 90.8 µs | 152 µs | 158 µs | 184 µs | 487 µs | 2.9 ms | 13.8 ms |
| c2-iso | r2 | 24007 | 120 µs | 154 µs | 160 µs | 181 µs | 213 µs | 1.8 ms | 8.2 ms |
| c2-iso | r3 | 24007 | 119 µs | 154 µs | 159 µs | 187 µs | 471 µs | 3.1 ms | 10.4 ms |
| c1-segregated-no0 | r1 | 12006 | 121 µs | 181 µs | 206 µs | 145 ms | 306 ms | 626 ms | 697 ms |
| c1-segregated-no0 | r2 | 12006 | 105 µs | 186 µs | 208 µs | 57.0 ms | 211 ms | 296 ms | 360 ms |
| c1-segregated-no0 | r3 | 12006 | 108 µs | 185 µs | 202 µs | 113 ms | 288 ms | 496 ms | 584 ms |
| c1-iso-segregated-no0 | r1 | 12006 | 112 µs | 168 µs | 174 µs | 199 µs | 279 µs | 2.5 ms | 4.1 ms |
| c1-iso-segregated-no0 | r2 | 12006 | 120 µs | 168 µs | 174 µs | 198 µs | 216 µs | 803 µs | 4.1 ms |
| c1-iso-segregated-no0 | r3 | 12006 | 117 µs | 168 µs | 174 µs | 202 µs | 561 µs | 2.5 ms | 4.0 ms |
| c1-batch0 | r1 | 12006 | 109 µs | 188 µs | 211 µs | 239 ms | 430 ms | 593 ms | 645 ms |
| c1-batch0 | r2 | 12006 | 121 µs | 179 µs | 193 µs | 145 ms | 354 ms | 674 ms | 718 ms |
| c1-batch0 | r3 | 12006 | 128 µs | 194 µs | 221 µs | 133 ms | 366 ms | 624 ms | 713 ms |
| c1-iso-batch0 | r1 | 12006 | 118 µs | 171 µs | 177 µs | 213 µs | 379 µs | 2.3 ms | 3.8 ms |
| c1-iso-batch0 | r2 | 12006 | 120 µs | 168 µs | 174 µs | 190 µs | 206 µs | 1.0 ms | 2.1 ms |
| c1-iso-batch0 | r3 | 12006 | 120 µs | 171 µs | 177 µs | 199 µs | 354 µs | 1.6 ms | 4.1 ms |
| c1-spread | r1 | 12006 | 110 µs | 176 µs | 197 µs | 537 µs | 205 ms | 475 ms | 563 ms |
| c1-spread | r2 | 12006 | 125 µs | 184 µs | 212 µs | 146 ms | 508 ms | 1.00 s | 1.06 s |
| c1-spread | r3 | 12006 | 121 µs | 187 µs | 217 µs | 202 ms | 452 ms | 735 ms | 810 ms |
| c1-iso-spread | r1 | 12006 | 132 µs | 183 µs | 188 µs | 210 µs | 226 µs | 1.1 ms | 3.6 ms |
| c1-iso-spread | r2 | 12006 | 170 µs | 181 µs | 184 µs | 201 µs | 216 µs | 976 µs | 3.3 ms |
| c1-iso-spread | r3 | 12006 | 140 µs | 183 µs | 188 µs | 216 µs | 444 µs | 2.2 ms | 5.4 ms |

Schedule latency, `group`:

| cell | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| c2 | r1 | 24004 | 1.2 ms | 3.2 ms | 12.6 ms | 279 ms | 307 ms | 341 ms | 362 ms |
| c2 | r2 | 23976 | 1.2 ms | 3.4 ms | 13.9 ms | 283 ms | 320 ms | 403 ms | 428 ms |
| c2 | r3 | 23938 | 1.2 ms | 3.3 ms | 14.4 ms | 284 ms | 327 ms | 381 ms | 400 ms |
| c2-iso | r1 | 24006 | 991 µs | 1.1 ms | 1.2 ms | 5.1 ms | 11.1 ms | 18.9 ms | 29.4 ms |
| c2-iso | r2 | 24007 | 984 µs | 1.1 ms | 1.2 ms | 5.2 ms | 10.9 ms | 20.1 ms | 28.2 ms |
| c2-iso | r3 | 24007 | 972 µs | 1.1 ms | 1.2 ms | 5.1 ms | 10.9 ms | 20.6 ms | 42.0 ms |
| c1-segregated-no0 | r1 | 12006 | 120 µs | 183 µs | 197 µs | 241 µs | 409 µs | 2.9 ms | 32.4 ms |
| c1-segregated-no0 | r2 | 12006 | 119 µs | 182 µs | 193 µs | 238 µs | 408 µs | 3.3 ms | 38.2 ms |
| c1-segregated-no0 | r3 | 12006 | 114 µs | 187 µs | 200 µs | 264 µs | 1.1 ms | 24.2 ms | 51.7 ms |
| c1-iso-segregated-no0 | r1 | 12006 | 117 µs | 167 µs | 172 µs | 190 µs | 204 µs | 976 µs | 3.0 ms |
| c1-iso-segregated-no0 | r2 | 12006 | 118 µs | 168 µs | 173 µs | 208 µs | 474 µs | 2.7 ms | 6.3 ms |
| c1-iso-segregated-no0 | r3 | 12006 | 116 µs | 170 µs | 179 µs | 205 µs | 386 µs | 2.5 ms | 4.1 ms |
| c1-batch0 | r1 | 12006 | 127 µs | 180 µs | 195 µs | 249 µs | 606 µs | 9.6 ms | 56.9 ms |
| c1-batch0 | r2 | 12006 | 115 µs | 182 µs | 193 µs | 257 µs | 761 µs | 3.0 ms | 5.8 ms |
| c1-batch0 | r3 | 12006 | 128 µs | 182 µs | 192 µs | 267 µs | 748 µs | 5.1 ms | 38.9 ms |
| c1-iso-batch0 | r1 | 12006 | 126 µs | 168 µs | 174 µs | 192 µs | 220 µs | 1.9 ms | 3.3 ms |
| c1-iso-batch0 | r2 | 12006 | 116 µs | 171 µs | 177 µs | 209 µs | 632 µs | 2.6 ms | 4.9 ms |
| c1-iso-batch0 | r3 | 12006 | 120 µs | 172 µs | 178 µs | 215 µs | 504 µs | 2.8 ms | 4.5 ms |
| c1-spread | r1 | 12006 | 122 µs | 182 µs | 193 µs | 264 µs | 975 µs | 19.3 ms | 48.9 ms |
| c1-spread | r2 | 12006 | 128 µs | 183 µs | 197 µs | 258 µs | 940 µs | 14.1 ms | 66.0 ms |
| c1-spread | r3 | 12006 | 126 µs | 184 µs | 199 µs | 284 µs | 1.0 ms | 5.1 ms | 39.6 ms |
| c1-iso-spread | r1 | 12006 | 140 µs | 184 µs | 190 µs | 211 µs | 228 µs | 1.6 ms | 3.1 ms |
| c1-iso-spread | r2 | 12006 | 135 µs | 185 µs | 193 µs | 221 µs | 389 µs | 1.4 ms | 5.8 ms |
| c1-iso-spread | r3 | 12006 | 133 µs | 184 µs | 189 µs | 217 µs | 541 µs | 2.1 ms | 5.1 ms |

Service latency, `relaxed`:

| cell | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| c2 | r1 | 23741 | 36.7 µs | 92.5 µs | 106 µs | 262 µs | 317 ms | 692 ms | 820 ms |
| c2 | r2 | 24007 | 39.3 µs | 91.6 µs | 102 µs | 227 µs | 247 ms | 702 ms | 1.21 s |
| c2 | r3 | 24006 | 37.0 µs | 91.1 µs | 101 µs | 232 µs | 310 ms | 614 ms | 1.09 s |
| c2-iso | r1 | 24007 | 68.4 µs | 85.4 µs | 90.6 µs | 114 µs | 218 µs | 1.8 ms | 13.8 ms |
| c2-iso | r2 | 24007 | 73.9 µs | 85.6 µs | 90.1 µs | 111 µs | 137 µs | 1.0 ms | 8.1 ms |
| c2-iso | r3 | 24007 | 75.3 µs | 85.8 µs | 91.8 µs | 116 µs | 292 µs | 2.5 ms | 10.3 ms |
| c1-segregated-no0 | r1 | 12006 | 61.0 µs | 111 µs | 126 µs | 185 µs | 1.0 ms | 323 ms | 697 ms |
| c1-segregated-no0 | r2 | 12006 | 63.9 µs | 116 µs | 130 µs | 193 µs | 1.1 ms | 269 ms | 360 ms |
| c1-segregated-no0 | r3 | 12006 | 48.6 µs | 112 µs | 125 µs | 182 µs | 1.2 ms | 337 ms | 584 ms |
| c1-iso-segregated-no0 | r1 | 12006 | 87.3 µs | 99.7 µs | 105 µs | 127 µs | 160 µs | 1.0 ms | 3.6 ms |
| c1-iso-segregated-no0 | r2 | 12006 | 85.1 µs | 99.7 µs | 105 µs | 127 µs | 141 µs | 448 µs | 1.0 ms |
| c1-iso-segregated-no0 | r3 | 12006 | 85.4 µs | 100 µs | 105 µs | 130 µs | 382 µs | 1.5 ms | 3.0 ms |
| c1-batch0 | r1 | 12006 | 63.7 µs | 113 µs | 128 µs | 212 µs | 5.2 ms | 384 ms | 645 ms |
| c1-batch0 | r2 | 12006 | 59.4 µs | 108 µs | 119 µs | 179 µs | 870 µs | 325 ms | 718 ms |
| c1-batch0 | r3 | 12006 | 61.8 µs | 120 µs | 138 µs | 480 µs | 3.6 ms | 377 ms | 713 ms |
| c1-iso-batch0 | r1 | 12006 | 86.1 µs | 103 µs | 108 µs | 138 µs | 234 µs | 1.2 ms | 3.2 ms |
| c1-iso-batch0 | r2 | 12006 | 86.8 µs | 99.9 µs | 105 µs | 119 µs | 132 µs | 474 µs | 1.4 ms |
| c1-iso-batch0 | r3 | 12006 | 85.4 µs | 102 µs | 108 µs | 126 µs | 177 µs | 1.0 ms | 3.3 ms |
| c1-spread | r1 | 12006 | 63.2 µs | 109 µs | 125 µs | 187 µs | 632 µs | 205 ms | 563 ms |
| c1-spread | r2 | 12006 | 48.4 µs | 112 µs | 131 µs | 386 µs | 2.3 ms | 325 ms | 1.06 s |
| c1-spread | r3 | 12006 | 53.9 µs | 113 µs | 131 µs | 202 µs | 2.4 ms | 373 ms | 810 ms |
| c1-iso-spread | r1 | 12006 | 105 µs | 115 µs | 120 µs | 141 µs | 155 µs | 1.1 ms | 3.5 ms |
| c1-iso-spread | r2 | 12006 | 107 µs | 114 µs | 117 µs | 133 µs | 145 µs | 358 µs | 1.7 ms |
| c1-iso-spread | r3 | 12006 | 106 µs | 116 µs | 121 µs | 145 µs | 226 µs | 1.3 ms | 5.3 ms |

Service latency, `group`:

| cell | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| c2 | r1 | 24004 | 1.1 ms | 3.0 ms | 3.4 ms | 11.3 ms | 86.3 ms | 310 ms | 362 ms |
| c2 | r2 | 23976 | 1.1 ms | 3.0 ms | 3.6 ms | 12.9 ms | 71.3 ms | 303 ms | 339 ms |
| c2 | r3 | 23938 | 1.1 ms | 3.0 ms | 3.5 ms | 12.7 ms | 91.7 ms | 295 ms | 322 ms |
| c2-iso | r1 | 24006 | 920 µs | 1.0 ms | 1.1 ms | 4.7 ms | 10.6 ms | 16.5 ms | 29.3 ms |
| c2-iso | r2 | 24007 | 914 µs | 1.0 ms | 1.1 ms | 4.7 ms | 10.3 ms | 17.9 ms | 28.1 ms |
| c2-iso | r3 | 24007 | 905 µs | 1.0 ms | 1.1 ms | 4.7 ms | 10.2 ms | 17.6 ms | 41.9 ms |
| c1-segregated-no0 | r1 | 12006 | 89.6 µs | 115 µs | 125 µs | 162 µs | 284 µs | 2.2 ms | 32.3 ms |
| c1-segregated-no0 | r2 | 12006 | 81.3 µs | 114 µs | 124 µs | 156 µs | 286 µs | 3.0 ms | 38.1 ms |
| c1-segregated-no0 | r3 | 12006 | 68.2 µs | 118 µs | 128 µs | 172 µs | 601 µs | 3.7 ms | 51.6 ms |
| c1-iso-segregated-no0 | r1 | 12006 | 85.8 µs | 98.0 µs | 104 µs | 120 µs | 132 µs | 510 µs | 3.0 ms |
| c1-iso-segregated-no0 | r2 | 12006 | 86.1 µs | 99.7 µs | 105 µs | 133 µs | 247 µs | 1.7 ms | 3.0 ms |
| c1-iso-segregated-no0 | r3 | 12006 | 87.3 µs | 102 µs | 109 µs | 133 µs | 190 µs | 1.0 ms | 3.9 ms |
| c1-batch0 | r1 | 12006 | 75.3 µs | 112 µs | 122 µs | 161 µs | 356 µs | 3.3 ms | 56.8 ms |
| c1-batch0 | r2 | 12006 | 82.3 µs | 114 µs | 124 µs | 170 µs | 382 µs | 2.4 ms | 5.7 ms |
| c1-batch0 | r3 | 12006 | 96.3 µs | 114 µs | 123 µs | 173 µs | 481 µs | 3.3 ms | 38.9 ms |
| c1-iso-batch0 | r1 | 12006 | 84.6 µs | 99.9 µs | 104 µs | 121 µs | 142 µs | 890 µs | 1.9 ms |
| c1-iso-batch0 | r2 | 12006 | 86.5 µs | 103 µs | 108 µs | 134 µs | 295 µs | 1.5 ms | 3.0 ms |
| c1-iso-batch0 | r3 | 12006 | 83.9 µs | 103 µs | 108 µs | 138 µs | 298 µs | 1.7 ms | 3.1 ms |
| c1-spread | r1 | 12006 | 61.8 µs | 114 µs | 122 µs | 168 µs | 582 µs | 3.3 ms | 48.8 ms |
| c1-spread | r2 | 12006 | 76.8 µs | 116 µs | 126 µs | 168 µs | 426 µs | 2.6 ms | 66.0 ms |
| c1-spread | r3 | 12006 | 88.2 µs | 116 µs | 127 µs | 179 µs | 729 µs | 3.5 ms | 39.5 ms |
| c1-iso-spread | r1 | 12006 | 107 µs | 117 µs | 122 µs | 142 µs | 154 µs | 639 µs | 3.1 ms |
| c1-iso-spread | r2 | 12006 | 106 µs | 118 µs | 124 µs | 149 µs | 229 µs | 960 µs | 3.5 ms |
| c1-iso-spread | r3 | 12006 | 107 µs | 117 µs | 121 µs | 143 µs | 244 µs | 1.0 ms | 5.0 ms |

## Appendix B. Rolls, segments and the checkpoint counters, per run

`live segments at last roll` is the largest `sync_segs` among the window's
roll lines (the segments held at the roll's own sync). `wal_first_segment` and
`wal_segments_removed` are core 0's, answered by the control session.
`checkpoint runs` is core 0's counter delta, not the number of runs in the
window (those are in §4's table).

| durability | cell | run | rolls in window | rolls in log | roller core(s) | wal_segments_removed | wal_first_segment (end) | live segments at last roll | wal_append_lsn delta | checkpoint runs (core 0 view) |
|---|---|---|---|---|---|---|---|---|---|---|
| relaxed | c3 | r1 | 36 | 38 | c3x36 | 0 | 0 | 38 | 2274 MiB | 2 |
| relaxed | c3 | r2 | 36 | 38 | c3x36 | 0 | 0 | 38 | 2258 MiB | 2 |
| relaxed | c3 | r3 | 36 | 38 | c3x36 | 0 | 0 | 38 | 2276 MiB | 2 |
| relaxed | c2 | r1 | 34 | 37 | c3x34 | 0 | 0 | 37 | 2176 MiB | 1 |
| relaxed | c2 | r2 | 33 | 36 | c3x33 | 0 | 0 | 36 | 2172 MiB | 1 |
| relaxed | c2 | r3 | 34 | 37 | c3x34 | 0 | 0 | 37 | 2209 MiB | 1 |
| relaxed | c1-segregated-no0 | r1 | 35 | 38 | c1x35 | 0 | 0 | 38 | 2244 MiB | 1 |
| relaxed | c1-segregated-no0 | r2 | 35 | 38 | c1x35 | 0 | 0 | 38 | 2253 MiB | 1 |
| relaxed | c1-segregated-no0 | r3 | 37 | 39 | c1x37 | 0 | 0 | 39 | 2356 MiB | 1 |
| relaxed | c1-batch0 | r1 | 36 | 39 | c1x36 | 0 | 0 | 39 | 2326 MiB | 1 |
| relaxed | c1-batch0 | r2 | 35 | 38 | c3x35 | 0 | 0 | 38 | 2252 MiB | 1 |
| relaxed | c1-batch0 | r3 | 33 | 35 | c3x33 | 4 | 4 | 33 | 2124 MiB | 1 |
| relaxed | c1-spread | r1 | 35 | 37 | c2x35 | 2 | 2 | 35 | 2257 MiB | 2 |
| relaxed | c1-spread | r2 | 35 | 38 | c0x15, c1x1, c3x19 | 0 | 0 | 38 | 2243 MiB | 1 |
| relaxed | c1-spread | r3 | 34 | 36 | c0x1, c3x33 | 0 | 0 | 36 | 2158 MiB | 1 |
| group | c3 | r1 | 25 | 26 | c3x25 | 21 | 21 | 5 | 1575 MiB | 6 |
| group | c3 | r2 | 25 | 26 | c3x25 | 21 | 21 | 5 | 1560 MiB | 6 |
| group | c3 | r3 | 25 | 26 | c3x25 | 21 | 21 | 5 | 1571 MiB | 6 |
| group | c2 | r1 | 23 | 24 | c3x23 | 20 | 20 | 6 | 1497 MiB | 6 |
| group | c2 | r2 | 23 | 24 | c3x23 | 20 | 20 | 6 | 1459 MiB | 6 |
| group | c2 | r3 | 23 | 24 | c3x23 | 20 | 20 | 6 | 1458 MiB | 6 |
| group | c1-segregated-no0 | r1 | 24 | 25 | c3x24 | 20 | 20 | 6 | 1517 MiB | 6 |
| group | c1-segregated-no0 | r2 | 24 | 25 | c1x24 | 21 | 21 | 6 | 1551 MiB | 6 |
| group | c1-segregated-no0 | r3 | 23 | 24 | c0x6, c1x6, c2x11 | 21 | 21 | 5 | 1483 MiB | 6 |
| group | c1-batch0 | r1 | 25 | 26 | c0x1, c2x24 | 22 | 22 | 6 | 1544 MiB | 6 |
| group | c1-batch0 | r2 | 24 | 26 | c1x24 | 21 | 21 | 6 | 1547 MiB | 6 |
| group | c1-batch0 | r3 | 24 | 25 | c1x24 | 20 | 20 | 5 | 1507 MiB | 6 |
| group | c1-spread | r1 | 24 | 25 | c2x24 | 20 | 20 | 6 | 1523 MiB | 6 |
| group | c1-spread | r2 | 24 | 25 | c2x24 | 22 | 22 | 6 | 1530 MiB | 6 |
| group | c1-spread | r3 | 24 | 25 | c1x24 | 21 | 21 | 6 | 1536 MiB | 6 |

## Appendix C. Per-core stream-latch waits

Each core cell is `stream wait / drain wait / roll-covered wait / sync-gate
wait`, in ms over the run's window (`load_c<k>_waited_wal_stream_us`,
`load_c<k>_wal_drain_latch_wait_us`, `load_c<k>_wal_roll_blocked_wait_us`,
`load_c<k>_waited_wal_sync_gate_us`; m1 - m0, an absent field 0). The roller
column names the core of each in-window roll line and how many. Core 0's
column includes threads that declared no core.

| durability | cell | run | roller core(s) | core 0 | core 1 | core 2 | core 3 |
|---|---|---|---|---|---|---|---|
| relaxed | c3 | r1 | c3x36 | 13354 / 13354 / 13353 / 3.1 | 231 / 0 / 231 / 0 | 94.4 / 0 / 94.3 / 0 | 0.09 / 0 / 0 / 13124 |
| relaxed | c3 | r2 | c3x36 | 13109 / 13103 / 13107 / 0 | 353 / 0 / 353 / 0 | 379 / 0 / 378 / 0 | 0.10 / 0 / 0 / 12963 |
| relaxed | c3 | r3 | c3x36 | 12635 / 12635 / 12634 / 3.2 | 901 / 0 / 900 / 0 | 139 / 0 / 139 / 0 | 0.15 / 0 / 0 / 12281 |
| relaxed | c2 | r1 | c3x34 | 13832 / 5269 / 13829 / 0 | 12964 / 0 / 12957 / 0 | 13504 / 0 / 13500 / 0 | 2.0 / 0 / 0 / 13111 |
| relaxed | c2 | r2 | c3x33 | 13011 / 4429 / 13007 / 0 | 12278 / 0 / 12271 / 0 | 13661 / 0 / 13658 / 0 | 2.3 / 0 / 0 / 12417 |
| relaxed | c2 | r3 | c3x34 | 12349 / 4041 / 12345 / 0 | 12779 / 0 / 12773 / 0 | 12003 / 0 / 11997 / 0 | 1.8 / 0 / 0 / 12263 |
| relaxed | c1-segregated-no0 | r1 | c1x35 | 11255 / 11255 / 11254 / 0 | 0.10 / 0 / 0 / 10982 | 5.3 / 0 / 5.3 / 0 | 207 / 0 / 207 / 0 |
| relaxed | c1-segregated-no0 | r2 | c1x35 | 10174 / 10174 / 10173 / 3.5 | 0.09 / 0 / 0 / 10000 | 173 / 0 / 173 / 0 | 213 / 0 / 212 / 0 |
| relaxed | c1-segregated-no0 | r3 | c1x37 | 11445 / 11441 / 11444 / 0 | 0.07 / 0 / 0 / 11793 | 13.5 / 0 / 13.5 / 0 | 3.8 / 0 / 3.7 / 0 |
| relaxed | c1-batch0 | r1 | c1x36 | 11934 / 11934 / 11932 / 65.5 | 0.09 / 0 / 0 / 11820 | 19.9 / 0 / 19.8 / 0 | 809 / 0 / 809 / 0 |
| relaxed | c1-batch0 | r2 | c3x35 | 11066 / 11066 / 11065 / 11.9 | 148 / 0 / 148 / 0 | 8.3 / 0 / 8.2 / 0 | 0.10 / 0 / 0 / 10971 |
| relaxed | c1-batch0 | r3 | c3x33 | 10766 / 10766 / 10757 / 0 | 238 / 0 / 238 / 0 | 267 / 0 / 267 / 0 | 0.37 / 0 / 0 / 10362 |
| relaxed | c1-spread | r1 | c2x35 | 10349 / 10349 / 10348 / 3.9 | 361 / 0 / 361 / 0 | 0.07 / 0 / 0 / 10213 | 17.2 / 0 / 17.1 / 0 |
| relaxed | c1-spread | r2 | c0x15, c1x1, c3x19 | 6257 / 6257 / 6256 / 5136 | 347 / 0 / 347 / 324 | 11.9 / 0 / 11.9 / 0 | 750 / 0 / 750 / 5899 |
| relaxed | c1-spread | r3 | c0x1, c3x33 | 12015 / 12015 / 12014 / 535 | 10.5 / 0 / 10.4 / 0 | 252 / 0 / 252 / 0 | 0.08 / 0 / 0 / 11783 |
| group | c3 | r1 | c3x25 | 15.2 / 15.2 / 4.5 / 0 | 0.01 / 0 / 0 / 0 | 0.01 / 0 / 0 / 0 | 0.15 / 0 / 0 / 12.9 |
| group | c3 | r2 | c3x25 | 77.9 / 77.9 / 67.3 / 0 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 | 0.13 / 0 / 0 / 4.7 |
| group | c3 | r3 | c3x25 | 37.3 / 37.3 / 34.1 / 1.8 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 | 0.12 / 0 / 0 / 3.5 |
| group | c2 | r1 | c3x23 | 181 / 98.6 / 147 / 469 | 159 / 93.1 / 130 / 0 | 162 / 83.4 / 140 / 0 | 1.9 / 0.01 / 0 / 68.9 |
| group | c2 | r2 | c3x23 | 182 / 113 / 158 / 238 | 228 / 153 / 183 / 0 | 217 / 120 / 170 / 0 | 6.4 / 0.01 / 0 / 82.7 |
| group | c2 | r3 | c3x23 | 195 / 145 / 160 / 126 | 178 / 108 / 150 / 0 | 167 / 88.1 / 141 / 0 | 3.1 / 0.03 / 0 / 66.1 |
| group | c1-segregated-no0 | r1 | c3x24 | 35.4 / 35.4 / 35.3 / 0 | 4.7 / 0 / 4.6 / 0 | 5.4 / 0 / 5.4 / 0 | 0.14 / 0 / 0 / 10.8 |
| group | c1-segregated-no0 | r2 | c1x24 | 23.5 / 23.5 / 23.4 / 0 | 0.07 / 0 / 0 / 9.4 | 1.9 / 0 / 1.9 / 0 | 0 / 0 / 0 / 0 |
| group | c1-segregated-no0 | r3 | c0x6, c1x6, c2x11 | 31.0 / 30.9 / 28.7 / 0 | 0.04 / 0 / 0 / 3.1 | 0.06 / 0 / 0 / 7.3 | 0.01 / 0 / 0 / 0 |
| group | c1-batch0 | r1 | c0x1, c2x24 | 36.7 / 36.7 / 36.5 / 0 | 0 / 0 / 0 / 0 | 0.22 / 0 / 0 / 6.4 | 0 / 0 / 0 / 0 |
| group | c1-batch0 | r2 | c1x24 | 34.8 / 34.8 / 32.0 / 0 | 0.19 / 0 / 0 / 3.5 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| group | c1-batch0 | r3 | c1x24 | 66.0 / 66.0 / 56.1 / 0 | 0.81 / 0 / 0 / 21.0 | 0 / 0 / 0 / 0 | 0.01 / 0 / 0 / 0 |
| group | c1-spread | r1 | c2x24 | 16.6 / 16.6 / 16.4 / 0 | 0 / 0 / 0 / 0 | 0.16 / 0 / 0 / 4.4 | 0.01 / 0 / 0 / 0 |
| group | c1-spread | r2 | c2x24 | 74.7 / 74.7 / 58.8 / 0 | 0 / 0 / 0 / 0 | 0.19 / 0.00 / 0 / 6.6 | 0 / 0 / 0 / 0 |
| group | c1-spread | r3 | c1x24 | 36.0 / 36.0 / 30.9 / 0 | 0.23 / 0 / 0 / 4.9 | 0 / 0 / 0 / 0 | 3.8 / 0 / 3.8 / 0 |

## Appendix D. The inserter, per run

| durability | cell | run | inserter rows/s | log MiB/s | inserter statement p50 | p99 | max |
|---|---|---|---|---|---|---|---|
| relaxed | c3 | r1 | 169,643 | 75.8 | 2.7 ms | 10.6 ms | 677 ms |
| relaxed | c3 | r2 | 168,427 | 75.2 | 2.7 ms | 10.9 ms | 915 ms |
| relaxed | c3 | r3 | 169,766 | 75.8 | 2.7 ms | 42.0 ms | 798 ms |
| relaxed | c2 | r1 | 164,869 | 70.8 | 2.8 ms | 11.8 ms | 794 ms |
| relaxed | c2 | r2 | 162,743 | 72.1 | 2.7 ms | 14.4 ms | 1.21 s |
| relaxed | c2 | r3 | 165,837 | 74.1 | 2.8 ms | 12.2 ms | 750 ms |
| relaxed | c1-segregated-no0 | r1 | 169,106 | 75.3 | 3.3 ms | 7.3 ms | 715 ms |
| relaxed | c1-segregated-no0 | r2 | 169,783 | 75.6 | 3.3 ms | 10.4 ms | 682 ms |
| relaxed | c1-segregated-no0 | r3 | 175,835 | 78.3 | 3.1 ms | 7.3 ms | 592 ms |
| relaxed | c1-batch0 | r1 | 174,926 | 77.9 | 3.0 ms | 7.5 ms | 657 ms |
| relaxed | c1-batch0 | r2 | 169,634 | 75.6 | 3.2 ms | 10.2 ms | 728 ms |
| relaxed | c1-batch0 | r3 | 158,427 | 70.4 | 3.3 ms | 11.1 ms | 729 ms |
| relaxed | c1-spread | r1 | 168,483 | 75.1 | 3.4 ms | 7.8 ms | 673 ms |
| relaxed | c1-spread | r2 | 169,056 | 75.3 | 3.3 ms | 7.4 ms | 793 ms |
| relaxed | c1-spread | r3 | 160,996 | 71.7 | 3.3 ms | 7.4 ms | 832 ms |
| group | c3 | r1 | 117,421 | 52.5 | 5.2 ms | 51.8 ms | 335 ms |
| group | c3 | r2 | 116,324 | 52.0 | 5.3 ms | 77.5 ms | 317 ms |
| group | c3 | r3 | 117,150 | 52.3 | 5.3 ms | 66.7 ms | 325 ms |
| group | c2 | r1 | 111,225 | 49.7 | 5.7 ms | 92.9 ms | 332 ms |
| group | c2 | r2 | 108,489 | 48.1 | 5.7 ms | 67.4 ms | 338 ms |
| group | c2 | r3 | 108,642 | 48.1 | 5.7 ms | 86.1 ms | 322 ms |
| group | c1-segregated-no0 | r1 | 113,323 | 50.4 | 5.7 ms | 57.1 ms | 320 ms |
| group | c1-segregated-no0 | r2 | 115,661 | 51.5 | 5.5 ms | 90.2 ms | 319 ms |
| group | c1-segregated-no0 | r3 | 110,589 | 49.3 | 5.9 ms | 92.5 ms | 317 ms |
| group | c1-batch0 | r1 | 115,186 | 51.3 | 5.6 ms | 55.7 ms | 309 ms |
| group | c1-batch0 | r2 | 115,364 | 51.4 | 5.5 ms | 75.4 ms | 340 ms |
| group | c1-batch0 | r3 | 112,432 | 50.1 | 5.6 ms | 55.2 ms | 324 ms |
| group | c1-spread | r1 | 113,975 | 50.6 | 5.6 ms | 51.4 ms | 324 ms |
| group | c1-spread | r2 | 114,180 | 50.8 | 5.6 ms | 48.8 ms | 327 ms |
| group | c1-spread | r3 | 114,565 | 51.0 | 5.5 ms | 48.5 ms | 344 ms |
