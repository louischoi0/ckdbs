# Scenarios 1 to 4 at `v3.1.0-30-gcd7c7c4e`: summary

**One line.** At `cd7c7c4e` the engine reads a walked row 2.4x to 2.8x more cheaply than at `9a0525d`.
That cost was its weakest side against PostgreSQL, and closing it moves the unindexed equality shapes from about 0.5x to 1.0-1.15x of PostgreSQL's seqscan.
Eight contended bookers under `strict` run +83 %. A single durable client is unchanged and is
PostgreSQL's equal (0.95x to 0.97x), because both engines spend 62 % of a booking in the commit fsync. Scenario 1's
driver is still refused at `CREATE TABLE ... HEAP` (SUS-1); a BTREE-only copy gives the scenario its first KDS number.

This file indexes the four results files and carries no number they do not. Measured by `ck-tester` from
2026-10-13 23:38 UTC to 2026-10-14 01:39 UTC on one `build-release` binary built at `cd7c7c4e` (sha256
`4752f562...`, section 1 of each file). The operator lifted the time budget for this run and **stopped it at
01:39 UTC** during scenario 1's size sweep (section 5 of that file). That sweep was not part of the rebaseline. Every rebaseline cell of the four scenarios had finished.

**Comparators.** KDS: the 2026-09-30 rebaseline at `9a0525d` (`v2.7.0-531-g9a0525d`, files held at
`36449d28`), 495 commits away, so every KDS delta is **cross-version** and attributable to no single change.
PostgreSQL 18.6: **reused, not re-measured**, from the files measured at `f2f1ee7` on 2026-09-30, 14 days before
this run, on a host that has since run other work. PostgreSQL is the floor, not the reference.

## Headlines

| scenario | KDS v3.1.0 | KDS `9a0525d` (cross-version) | PostgreSQL 18.6 (reused) | file |
|---|---|---|---|---|
| 1 backtest | driver refused (SUS-1); BTREE copy: `day-slice` 550.7 qps cold, 6,574.1 qps with a Cabin; `cross-join` 537.5 / 5,791.1 qps | refused at the same statement; no number | `day-slice` 1.33x, `cross-join` 1.43x, `point-join` 2.17x of PG; `bar-range` 0.49x, `symbol-history` 0.43x, `model-join` 0.57x | `results-scenario1-backtest-v3.1.0-30-gcd7c7c4e.md` |
| 2 freight, 8 bookers, `cores = 8` | 1,902.7 tps `group`, 1,584.9 tps `strict` | 1,519.0 / 864.2 tps (+25.3 %, +83.4 %) | no counterpart (the PG twin drives one booker) | `results-scenario2-freight-v3.1.0-30-gcd7c7c4e.md` |
| 2 freight, 1 booker, durable | 374.0 tps `group`, 382.0 tps `strict` | 378.2 / 377.7 tps (inside spread) | 392.2 tps; KDS 0.95x / 0.97x | same |
| 3 library, 10,000 loans, unindexed `loans-by-user` | 2,779 qps | 1,277 qps (2.18x) | KDS 1.12x (was about 0.53x) | `results-scenario3-library-v3.1.0-30-gcd7c7c4e.md` |
| 3 library, geometric mean over 12 shapes, 10,000 rows unindexed | 0.97x of PG | 0.62x of PG | - | same |
| 4 cabinopt, board day 1 (`off` / `on` / `declared`) | 1,976 / 3,858 / 3,506 qps | walk 921 to 960 qps | 1,994 qps seqscan; KDS 0.99x / 1.93x / 1.76x | `results-scenario4-cabinopt-days-v3.1.0-30-gcd7c7c4e.md` |

## Where KDS stands against PostgreSQL

- **Ahead**:
  - an equality filter answered by a Cabin: up to 2.28x on scenario 4's `tape10k`;
  - the pk-anchored join: 2.17x;
  - the correlated `EXISTS`: 1.13x to 2.09x;
  - the write statements of a booking: p50 0.71x to 0.86x of PostgreSQL's.
- **Even**:
  - one durable client, which is bound by the device's fsync on both engines;
  - the unindexed non-pk equality walk, now about 27 ns per row against PostgreSQL's 34 to 36 ns.
- **Behind**:
  - shapes that return many rows (`bar-range`, `symbol-history`, `books-by-genre`, `loans-by-daterange`: 0.43x to 0.77x);
  - whole-relation folds (scenario 1's `agg-*` and `compare-all`, 1.4x to 2.4x slower; scenario 4's `close` 0.56x);
  - the fixed round trip of a pk read (0.83x to 1.03x).

## What this run does not show

- No PostgreSQL number was re-measured. The 14 days and the host's other work between the two sides make a ratio inside 0.90x to 1.10x no finding.
- Scenario 1 measures a driver copy whose `daily_stats`, `model_results` and `write_probe` are BTREE relations where the driver
  and its PG twin use heap. Its write side therefore measures something other than what the driver intended.
- The unmodified driver has no number (SUS-1).
- Scenario 1's size sweep is partial: two runs at 252 and 1,008 bars, one at 10,080 bars.
- Scenario 2's booker sweep at `cores = 8` (`x-b1/2/4/16-s2-c8-*`) ran but is not written up. Its JSON is in the archive.
- Scenario 4 has no PostgreSQL run with an index on `symbol`, which would be the fair counterpart of a declared Cabin.
- Other sessions' builds and `ctest` overlapped the run. A 2-second monitor discarded and re-ran every cell they touched (`contaminated/` in each archive).
- None of the four files has had a `critics-developer` review.

## Archive

- `bench/v3.1.0/archive/scenarioN-v3.1.0-30-gcd7c7c4e/`: each scenario's per-cell JSON, logs, configs and contaminated attempts. Scenario 1 also holds the BTREE driver copy.
- `bench/v3.1.0/archive/run-v3.1.0-30-gcd7c7c4e/`: what the four scenarios share:
  - the run scripts (`run_all*.sh`, `lib.sh`), the timeline, the monitor and watchdog logs and the binary hash;
  - the generators under `analysis/`, including `build_s1_final.py`, which built scenario 1's file over the cells that existed at the stop.
