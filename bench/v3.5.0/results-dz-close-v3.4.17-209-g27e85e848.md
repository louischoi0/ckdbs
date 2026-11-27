# DZ's close - what a cadence checkpoint holds its reactor for, before and after (`v3.4.17-209-g27e85e848` against `v3.4.17-202-g481d101aa`)

Work order: `instructions/v3.2.0/workorder-dz-checkpoint-reactor-residue.md`
DZ-S4. Measured 2026-11-27 17:17-17:41 UTC (the archive's `status` files:
17:17:24-17:40:32) on the worktree
`dz-checkpoint-reactor-residue`. **A** is `481d101aa`
(`v3.4.17-202-g481d101aa`), the commit DZ opened from; **B** is `27e85e848`
(`v3.4.17-209-g27e85e848`), DZ-S1 to DZ-S3 with their reviews.

## The answer

**DZ-R5's bar is met**: B held its reactor no longer than 9.6 ms in any of
12 census cells, at 64 Ki, 256 Ki and 1 Mi dirty frames and `cores` 1 and
4, and no poll passed 10 ms. A held it up to 1,064 ms, with 1,234-1,642
polls over 10 ms in every 1 Mi cell. **The run is not longer**: summed over
the cells, B's runs took 233 s against A's 245 s at 1 Mi and 41 s against
48 s at 256 Ki. **No server-level regression**: over 12 alternating pairs
at `insert1` `group` `cores = 2`, the median is 242.4k statements a run
against 240.5k (B/A 1.008, B higher in 6 of 12). **The early carve's change
is not shown to work at either shape**: at `group` the first three pairs
read the longest carve 12-440 ms against 9-12 ms, but over all twelve it is
1.7-440 ms against 0.8-273 ms (medians 11.0 and 10.4 ms; over 100 ms in two
of A's runs and one of B's); at `relaxed`, `cores = 1` it did not fall (A
50-459 ms, B 86-610 ms), where the window's last quarter drains before the
carve's sync is done.

## Setup

- **Binaries**, release builds copied and run from the copy (rule 3):

  | name | engine | sha256 (16) |
  |---|---|---|
  | census A | `bench/checkpoint_reactor_hold_bench.cpp` at `481d101aa`, DZ-S0's `hold-a` | `1e8b00fd8f97d9d4` |
  | census B | the same bench at `27e85e848` (forwarding the sliced collection, the staged writes and the tried maps) | `a7dbfe106b7a629b` |
  | server A | `kds_server` built from a `git archive` of `481d101aa` | `c537e1124590b823` |
  | server B | `kds_server` from `build-release` at `27e85e848` | `7a9ed5b56579f0b2` |

- **Device**: data file and log on `/dev/mapper/fedora-root`, xfs (rule 2).
- **Load** (rule 4): `/proc/loadavg` and the count of compiler and `ctest`
  processes before each cell are in each `status` in the archive; load
  1.0-9.6 (other sessions); no compiler ran during any census cell or any
  server cell but `A-insert1-relaxed-k1-r3` (2).
- **Census**: DZ-S0's grid, A then B in each cell, two rounds, pinned to
  CPUs 0-7. Its cuts are DZ-S0's: no page LSN, so no log gate or deferral; an
  in-memory anchor, so no anchor page write; nothing holding the map
  barrier.
- **Server**: `tools/ba_census.py`, 16 sessions, 8 s after 2 s, server on CPU
  0 (`cores = 1`) or 0-1 (`cores = 2`), clients on 4-7, port 15761 (rule 5),
  one run a cell, A then B: `insert1` `relaxed` `cores = 1` (DZ-S3's premise
  shape), `insert1` `group` `cores = 2`, `point` `group` `cores = 2`, three
  rounds; then three more rounds of `insert1` `group` (pairs 4-6), and six
  more (pairs 7-12) when its first six pairs disagreed.
- **Time**: 11 minutes census (17:17:24-17:28:18), 6 + 1.5 + 3 minutes
  server (17:29:00-17:34:58, 17:35:44-17:37:06, 17:37:49-17:40:32), 6
  minutes building -
  inside the milestone's hour. **What was cut**: no `cores = 4` server cell,
  no `strict` cell, and the point shape at three pairs.

Archive: `archive/dz-close-v3.4.17-209-g27e85e848/` (`census/` each cell's
output and status, `server/` each run's JSON, the status and the driver's
log, the drivers).

## The census: the longest `system` poll of one run

| frames | cores | A round 1 | A round 2 | B round 1 | B round 2 | A polls over 10 ms | B polls over 10 ms |
|---|---|---|---|---|---|---|---|
| 65,536 | 1 | 2.12 ms | 5.81 ms | 2.02 ms | 0.52 ms | 0, 0 | 0, 0 |
| 65,536 | 4 | 8.04 ms | 6.91 ms | 2.08 ms | 1.90 ms | 0, 0 | 0, 0 |
| 262,144 | 1 | 7.46 ms | 12.24 ms | 1.62 ms | 1.54 ms | 0, 1 | 0, 0 |
| 262,144 | 4 | 16.72 ms | 15.29 ms | 1.12 ms | 1.99 ms | 1, 1 | 0, 0 |
| 1,048,576 | 1 | 1,063.58 ms | 39.94 ms | 3.73 ms | 9.41 ms | 1,491, 1,234 | 0, 0 |
| 1,048,576 | 4 | 42.01 ms | 41.27 ms | 9.56 ms | 4.50 ms | 1,327, 1,642 | 0, 0 |

### By piece, the longest occurrence at 1 Mi (both rounds, both core counts)

| piece | A | B |
|---|---|---|
| the start's walk (B: one slice) | 32.4-37.8 ms | 3.2-3.5 ms |
| a step's writes (B: a stage or a finish) | 14.6-1,063.6 ms | 0.25-0.56 ms |
| the publish's walk (B: one slice) | 6.7-11.3 ms | 0.20-0.31 ms |
| a map half | 0.04-0.05 ms | 0.03-0.05 ms |

- **B's two 9.4-9.6 ms polls** at 1 Mi fall after the last step and before
  the publish's first batch, and none of the census's pieces is that long
  (the start phase's longest occurrence 3.2-3.6 ms, equal to the longest walk
  slice in three of the four cells; the census does not time the begin
  record's own pieces apart). Unattributed; under the bar.
- **Run length**, the sum of the four cells' runs: 1 Mi A 245.1 s, B 233.4 s
  (-4.7 %); 256 Ki A 47.8 s, B 40.8 s; 64 Ki A 13.7 s, B 9.4 s. A run's
  length is mostly its data-file `fsync` behind the host's other writeback,
  which moved by tens of seconds between rounds of one binary; DZ-R5's
  "+25 %" is not passed in any sum.

## The server: statements a run, and the carve

| shape | A | B |
|---|---|---|
| `insert1` `relaxed` `cores = 1`, statements a second | 90,647 / 106,219 / 160,707 | 100,272 / 93,251 / 162,054 |
| its longest carve | 451 / 459 / 50 ms | 610 / 474 / 86 ms |
| its longest reactor hold outside a run | 451 / 459 / 23 ms | 610 / 475 / 87 ms |
| `insert1` `group` `cores = 2`, statements a run, 12 pairs, median | 240,456 | 242,446 |
| its longest carve, first three pairs | 12 / 264 / 440 ms | 9 / 12 / 12 ms |
| its longest carve, 12 pairs, median (range) | 11.0 ms (1.7-440) | 10.4 ms (0.8-273) |
| its longest hold outside a run, first three pairs | 12 / 265 / 17 ms | 1 / 12 / 1 ms |
| `point` `group` `cores = 2`, statements a second | 143,189 / 148,809 / 146,124 | 141,808 / 140,555 / 143,868 |

- **`insert1` `group`**: the first six pairs read B lower in four (a -22 %
  median); the next six read B higher in four. Over twelve the pairs split
  6 to 6 and the medians agree within 1 %, so the first six were the
  host. The `sys.meta` deltas over the first six put B's WAL segment roll's
  create and header time, a run's sum and the six runs' mean, at 2.2-2.6 ms
  against 0.26-0.66 ms - one run carries B's mean (pair 5: 12.6 and 12.0 ms),
  the other five reading 0.13-1.04 ms - which the twelve pairs' throughput
  does not show.
- **`point` `group`**: B is 1.0-5.5 % lower in each of three pairs; the path
  reads no page DZ changed and appends nothing. Not resolved at three pairs.
- **The carve at `relaxed`, `cores = 1`**: the early carve stages its block
  at three-quarters of the window, which leaves 1,024 ids - about 10 ms at
  100k statements a second - and the longest carve was 0.47-0.61 s in two
  of B's three runs (86 ms in the third), read as the window draining first
  and the `BEGIN` that drains it persisting the pending block whole, as
  before. At `group` the statement rate is 15-37k a second against 91-162k,
  and the longest carve read 9-12 ms in B's first three runs, but 273 ms in
  its fourth, against A's 440 ms at most. DZ-S3's purpose is not shown met at
  either shape; the gap is stated in `known-gaps.md`.

## What this says about the engine

The cadence checkpoint is no longer a reason a session on its core waits:
at 8 GiB of dirty pages the worst it did to its reactor fell from a second
to under 10 ms, and the run itself did not get longer. What is left on that
core is the transaction-id carve - at `relaxed` in every run, at `group` in
one run of twelve - which no longer shares the reactor with the checkpoint
but still waits a data-file `fsync` when the window outruns the sync - a
block size question, not a checkpoint one.
