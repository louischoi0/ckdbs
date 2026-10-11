# BA's `relaxed` carve and checkpoint, diagnosed (`v2.7.0-736-ge5a7e1b7`)

**Thesis.** BA-S17's census (`results-ba-s17-census-v2.7.0-736-ge5a7e1b7.md`) read a
`relaxed` carve of 226-258 ms against BA-S4's 20-46 ms and a longer `relaxed` checkpoint, and
named BA-S13's unconditional data-file sync as the carve's candidate cause. Bisected over the
stage commits and over two reverts of B, **the checkpoint's regression is BA-S13's and the
carve's is not**. With BA-S13's carve persist reverted to the whole-pool sync, the checkpoint's
longest stall returns from 87-124 ms to 13-19 ms, and the carve's does not move (277-620 ms).
At BA-S12 (`7fa2bd96`), before BA-S13, the carve's longest stall already reads 258-886 ms in
every clean run. **At B a carve's time is its data-file `fsync`**: page 0's writeback takes
13 µs at the median, the `fsync` 3.2 ms at the median and up to 252 ms, and in the three `diag`
runs the longest persist equals `carve_longest_us` to within 1 ms. `carve_longest_us` is a
maximum over the server's whole life, setup included, so at B and at S4 it reads a 240-300 ms
data-file `fsync` stall whenever one lands inside a carve. **Maxima of 400-886 ms** (S7p2, S12,
B under strace) exceed every slow data sync traced in the same run and are not explained:
before BA-S13 a carve also wrote back the whole pool, which strace does not trace, and the
carve's timer also covers the wait for the superblock latch. All of this is `relaxed` at
`cores = 1`. Whether carves *inside the measured span* got slower was not measured; the
operator ruled the record corrected without it (2026-10-11, translated: *"record correction
only"*).

## Setup

- **Shape**: `insert1`, `relaxed`, `cores = 1`, 16 sessions; 8 s measured after 2 s of warm-up,
  server on CPU 0, clients on CPUs 4-7; port 15651. Driver: `tools/ba_census.py` at
  `e5a7e1b7`, copied so that it keeps the server's stderr (`--max-load 2` gates each run's
  start, `--max-wait 300`); `run.sh` (B's and `noprep`'s first runs) called the uncopied
  driver in the `e5a7e1b7` worktree instead, whose state at that time is not recorded. Data on
  `/dev/root`, ext4, named and not archived as `df -T` output.
- **Binaries**, each a release build copied before running (`binaries.sha256` in the archive):

  | name | engine | what it isolates |
  |---|---|---|
  | `66bb11b2` | `v2.7.0-726-g66bb11b2`, BA-S4 | the census' comparator |
  | `91a14db0` | `v2.7.0-728-g91a14db0`, BA-S7 part 2 | after S7 |
  | `7fa2bd96` | `v2.7.0-729-g7fa2bd96`, BA-S12 | after S12, before S13 |
  | B | `v2.7.0-736-ge5a7e1b7`, the census' B (sha256 `d2a6977f...`) | BA's close |
  | `diag` | B with stderr timing around the carve's persist, `PersistPage` and the checkpoint | where a carve's time goes |
  | `oldsync` | `diag` with the carve's `PersistPage` replaced by the whole-pool `store_->Sync()` | BA-S13's persist reverted |
  | `noprep` | B with the WAL segment build-ahead disabled | BA-S12's build-ahead removed |

  The three stage binaries were built from `git archive` exports with
  `-DCMAKE_BUILD_TYPE=Release`; `diag`, `oldsync` and `noprep` from `build-release` of a
  worktree at `e5a7e1b7` with the edit applied. The archive's `scripts/oldsync.patch` builds
  `oldsync`; `diag` is that patch without its `PersistTrxIdCeiling` hunk, and
  `scripts/noprep.patch` is `noprep`'s one-line edit.
  `A` (`d43845a0`) did not start: the driver writes `reactor_cpus`, a key A does not know.
- **Host**: shared, with other sessions building. Each run's load at start and end is in the
  archive's `run*.log`; `pgrep` for compilers was not recorded per run, which is
  `bench/README.md` rule 4 met only by the load half. **A run that started at a load of 2 or
  more is excluded below** (`noprep` 2, `66bb11b2` in `out5` rounds 1 and 4, B in `out5` rounds 1
  and 4, `7fa2bd96` in `out4` round 2). The first three `diag` runs (`out-diag*`) have no log and
  are excluded too.
- **strace** (`out4` and `out5`): every `fsync`/`fdatasync` with its duration, through a wrapper the
  driver starts. It slows the server; its runs are read for the syncs, not for the stalls.

Archive: `archive/ba-carve-diag-v2.7.0-736-ge5a7e1b7/` (per-run JSON, `server.stderr` for the
`diag`/`oldsync` runs, logs, scripts, and the three tables below as text).

## The stall counters, clean runs

`*_longest_us` are maxima since the server started, as in the census.

| binary | runs | carve longest (ms) | checkpoint longest (ms) |
|---|---|---|---|
| `66bb11b2` (S4) | 5 | 21.4, 22.9, 50.5, 297.5, 334.6 | 10.1-16.4 |
| `91a14db0` (S7p2) | 6 | 26.1, 28.5, 340.7, 406.1, 413.2, 646.8 | 9.8-18.0 |
| `7fa2bd96` (S12) | 5 | 258.5, 276.1, 408.6, 469.0, 886.0 | 9.4-15.8 in 4; 306.2 in 1 |
| B | 8 | 252.0-273.9 in 6; 493.6, 503.2 | 87.3-124.1 in 7; 563.3 in 1 |
| `diag` | 3 | 244.3, 246.0, 252.8 | 100.8, 118.1, 120.7 |
| `oldsync` | 3 | 276.7, 282.5, 620.5 | 13.3, 16.3, 19.0 |
| `noprep` | 2 | 12.6, 357.6 | 84.5, 122.3 |

- **The checkpoint**: 9.4-18.0 ms in 15 of 16 clean runs before BA-S13, 87-124 ms in 10 of 11
  clean runs of B and `diag`, and 13-19 ms in 3 of 3 with BA-S13's persist reverted.
  Disabling BA-S12's build-ahead leaves it long. **BA-S13 moved the pool's writeback from the
  carve, which synced the whole pool, to the checkpoint, which now finds it dirty.** Of the two
  outliers, the 306 ms run's strace shows four slow data `fsync`s; the 563 ms run
  (`out3-B-3`) was not traced and is unexplained.
- **The carve**: in hundreds of ms in 2 of 5 runs at S4, 4 of 6 at S7p2, 5 of 5 at S12, 8 of 8
  at B and 3 of 3 with BA-S13 reverted. **Reverting BA-S13 does not shorten it, and S12, which
  precedes BA-S13, already reads 258 ms and more in every run.** These samples are too small to
  say which of S7 and S12 moved it from S4's typical 21-51 ms.

## Where a carve's time goes (`diag`, 3 runs, 308 carve persists)

| part | median | p90 | max |
|---|---|---|---|
| page 0's writeback | 13 µs | | 29 µs |
| the data file's `fsync` | 3.2 ms | 7.9 ms | 251.8 ms |

11 of 308 syncs exceed 50 ms. At B nothing in a carve but the `fsync` is ever long. This
breakdown was not taken for the earlier binaries, whose carve also wrote back the pool.

## The slow data-file `fsync`s (strace)

Offsets are seconds from the server's first traced sync; a run's life is about 40 s, setup
first. Syncs of the data file over 50 ms in the clean `out5` runs; the carve maximum is set
beside them for orientation only, since strace slows the server:

| run | start load | slow data syncs (offset s / ms) | carve longest |
|---|---|---|---|
| `66bb11b2` 2 | 1.93 | 0.6 / 297 | 297.5 ms |
| `66bb11b2` 3 | 1.92 | 20.5 / 274 | 22.9 ms |
| B 2 | 1.77 | 4.8 / 290 | 493.6 ms |
| B 3 | 1.95 | 0.7 / 242 | 272.0 ms |

- **BA-S4's own binary meets the same 240-300 ms data `fsync` about once a run.** When it
  lands outside a carve (round 3, 20 s in), `carve_longest` reads 23 ms. When it lands in a
  carve (round 2, during setup), it reads 297 ms.
- **B meets it early, during the setup load (0.7-4.8 s).** In round 3 the carve maximum
  matches it; in round 2 the carve maximum (494 ms) exceeds it and is unexplained. Why B's
  stall lands in a carve where S4's often does not is not established.
- **Slow data syncs per clean traced run** (`out4` and `out5`): S4 1 and 1, S7p2 2, 4 and 1,
  S12 4 and 3, B 1 and 1. By this count B does not meet the device's stall more often than
  S4 does. In `out4`, carve maxima of 406-647 ms (S7p2) and 409 ms (S12) stand beside a
  largest slow data sync of 90-364 ms.
- The loaded runs (`66bb11b2` 1 and 4, B 1 and 4) are in `slow-data-fsyncs.txt` and are not
  read. Round 1's B shows 13 slow syncs under a load of 9.4.

## What this does not establish

- **Carves inside the measured span.** No run marked the span's start in the server's
  stderr, so the carve p50, p99 and count over 50 ms were not compared inside the span.
- **Any in-span mechanism.** If in-span carves did get slower, the candidate is that a carve's
  `fsync` now flushes the page writes made by the checkpoint and by eviction. It is untested.
- **Other durabilities and core counts.** Only `relaxed` at `cores = 1` was run. The census'
  `group` and `strict` carve improvement and its `cores` 2 and 4 rows are not re-read.
- **The census' median shift itself** (20-46 ms to 226-258 ms over 75 runs a row) stands as
  measured. This file attributes it to where a once-a-run device stall lands, not to added
  carve work, and the stage that moved the landing is not isolated.
