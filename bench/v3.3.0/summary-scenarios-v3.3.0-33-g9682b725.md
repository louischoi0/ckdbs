# Scenarios 1 to 4 at `v3.3.0-33-g9682b725`: summary

**One line.** The engine at `v3.3.0-33-g9682b725` is, within the run's spreads, the engine of v3.1.0 on most shapes this suite measures; the largest movers are two: the unserved equality **walk** (scenario 4's `off` arm is
1.18x to 1.20x faster on a 10,000-row board and tape) and the **inner-built two-relation join** (scenario 3's `join-no-literal` at 10,000 loans runs 1.98x). Smaller movers (eight-core one-connection reads, eight contended bookers) are listed below; reads, writes, the Cabin multiples and the controller lifecycle are inside the
run's own spreads. **Scenario 1's driver now runs unmodified** (BZ made its `HEAP` tables hidden-key btrees; no BTREE copy was needed).
**The join planner (CB/CC) reorders two of scenario 1's joins and moves neither beyond noise, and it changes nothing in scenario 3's joins**: A/B against `join_order = written` on the same binary.
One durable client is PostgreSQL's equal (0.99x), because both engines wait on the same fsync. PostgreSQL 18.6 was **re-measured fresh in this window**, so every ratio below is same-window.

This file indexes the four results files and carries no number they do not. Measured by `ck-tester` from 2026-10-20 03:37 UTC to 04:44 UTC on one `build-release` binary built at `9682b725` (sha256
`7532bd14...`, section 1 of each file). **The operator lifted the time budget for this run** ("lift the budget, re-measure PG too"): every cell of the v3.1.0 plan ran, with its repeat counts, nothing was cut,
and the size sweep (scenario 1) and booker sweep (scenario 2) that v3.1.0 left unfinished or unwritten were run and written up.

**Comparators.** KDS: v3.1.0 at `cd7c7c4e` (`v3.1.0-30-gcd7c7c4e`, measured 2026-10-13 to 14), 240 commits and 101 first-parent merges away, so every KDS delta is **cross-version** and attributable to no
single change; re-derived from that run's archived JSON. Scenario 1's v3.1.0 cells ran a BTREE-only copy of the driver. PostgreSQL 18.6: **fresh**, `/home/cdkbs/pg-bench-ba` port 15434 at its defaults, three runs
per cell wherever a twin exists, run after the KDS cells and never concurrent with one. PostgreSQL is the floor, not the reference.

## Headlines

Warm qps (scenario 1), tps (2), derived qps (3, 4), median of three runs (scenario 4: run A, day 1). Delta = now over v3.1.0 (cross-version); ratio = now over PostgreSQL (above 1.00x = KDS faster).

| measure | KDS now | KDS v3.1.0 | delta (cross-version) | PostgreSQL 18.6 (fresh) | KDS/PG |
|---|---|---|---|---|---|
| 1 backtest, `day-slice` (warm, `cores = 1`) | 591.3 qps | 550.9 qps | +7.3 % | 413.5 qps | 1.43x |
| 1 backtest, `cross-join` (warm, `cores = 1`) | 575.8 qps | 549.2 qps | +4.8 % | 393.1 qps | 1.46x |
| 1 backtest, `point-join` (warm, `cores = 1`) | 12,674.6 qps | 12,872.0 qps | -1.5 % | 7,071.3 qps | 1.79x |
| 1 backtest, `bar-range` (warm, `cores = 1`) | 684.4 qps | 670.1 qps | +2.1 % | 1,399.8 qps | 0.49x |
| 1 backtest, `symbol-history` (warm, `cores = 1`) | 13.8 qps | 14.1 qps | -2.1 % | 32.3 qps | 0.43x |
| 1 backtest, `model-join` (warm, `cores = 1`) | 614.8 qps | 569.5 qps | +8.0 % | 1,092.2 qps | 0.56x |
| 2 freight, 8 bookers, `cores = 8`, `group` | 1,965.0 tps | 1,902.7 tps | +3.3 % | no counterpart (one booker) | - |
| 2 freight, 8 bookers, `cores = 8`, `strict` | 1,674.2 tps | 1,584.9 tps | +5.6 % | no counterpart (one booker) | - |
| 2 freight, 1 booker, durable, `group` | 414.8 tps | 374.0 tps | +10.9 % | 419.5 tps | 0.99x |
| 2 freight, 1 booker, durable, `strict` | 417.1 tps | 382.0 tps | +9.2 % | 419.5 tps | 0.99x |
| 3 library, `loans-by-user` (10,000 loans, unindexed, `cores = 1`) | 2,779 qps | 2,779 qps | +0.0 % | 2,421 qps | 1.15x |
| 3 library, `join-no-literal` (10,000 loans, unindexed, `cores = 1`) | 1,683 qps | 851 qps | +97.8 % | 1,504 qps | 1.12x |
| 3 library, `exists-correlated` (10,000 loans, unindexed, `cores = 1`) | 1,596 qps | 1,586 qps | +0.7 % | 768 qps | 2.08x |
| 3 library, `loans-by-user` (10,000 loans, indexed, `cores = 1`) | 11,001 qps | 11,223 qps | -2.0 % | 10,834 qps | 1.02x |
| 3 library, geometric mean over 12 shapes (10,000 loans, unindexed / indexed) | - | - | 1.07x / 0.98x | - | 1.06x / 0.98x |
| 4 cabinopt, `board` day 1, arm `off` (run A) | 2,344 qps | 1,976 qps | +18.7 % | 1,976 qps (no index) | 1.19x |
| 4 cabinopt, `board` day 1, arm `on` (run A) | 4,189 qps | 3,858 qps | +8.6 % | 1,976 qps (no index) | 2.12x |
| 4 cabinopt, `board` day 1, arm `declared` (run A) | 3,621 qps | 3,506 qps | +3.3 % | 1,976 qps (no index) | 1.83x |
| 4 cabinopt, `tape10k` day 1, arm `off` (run A) | 2,679 qps | 2,238 qps | +19.7 % | 2,100 qps (no index) | 1.28x |
| 4 cabinopt, `tape10k` day 1, arm `on` (run A) | 5,376 qps | 4,808 qps | +11.8 % | 2,100 qps (no index) | 2.56x |

Files: `results-scenario1-backtest-v3.3.0-33-g9682b725.md`, `results-scenario2-freight-v3.3.0-33-g9682b725.md`, `results-scenario3-library-v3.3.0-33-g9682b725.md`, `results-scenario4-cabinopt-days-v3.3.0-33-g9682b725.md`.

## The movers, and what they are

- **The unserved equality walk is cheaper (scenario 4 `off`).** 1.18x on `board`, 1.20x on `tape10k`, every day of both runs, against a `pk` control that reads 0.85x to 1.07x; the per-row cost of an unserved equality falls from
  34.6 to 26.5 ns and the server CPU of an `off` run's boards from 2.80 s to 2.22 s. Scenario 3's integer-column equality walk shows **no** such gain (`loans-by-user` 2,779 qps in both versions), so the gain is not general; the filtered
  column here is a `varchar`, among other differences. Likely cause: not separated by this run, 240 commits.
- **`join-no-literal` at 10,000 loans is 1.98x (scenario 3).** The plan shows `inner_built=1` (a statement-local hash build of `loans`: one 10,074-row scan for a 16-row outer instead of 16 scans). The join planner is not the cause: with
  `join_order = written` the same cell is 5 % *faster*, and its plan carries the same build. Whether v3.1.0 already built cannot be read from its JSON (step kinds only). Likely cause: the inner build's reach (`join-inner-build.md`) or the scan beneath it.
- **`cores = 8` one-connection reads gained 28 % to 31 % (scenario 1).** At v3.1.0 a one-connection read on an eight-core instance ran 22 % slower than on one core; now 6 % slower. The one-core engine did not move (+7 %, inside its 7 % spread).
- **Eight contended bookers: +3.3 % `group`, +5.6 % `strict`** (scenario 2), above their spreads (0.5 %, 3.4 %), with 54 / 42 / 39 `strict` conflicts per run against 90 / 63 / 45. Small, real, and carried by the commit (its mean fell 5 % to 9 % at eight cores and 18 % with one booker), not by the statements: every statement p50 in scenario 2's comparison table is 1.01x to 1.11x of v3.1.0's, and scenario 4's `pk` control is +4.7 % (73.4 against 70.1 µs). A 3 % to 7 % per-statement slowdown is visible and is **not separated between engine and host** by this run.
- **One durable booker: +9 % to +11 % against v3.1.0, and equal to PostgreSQL.** That is the **device**: the commit mean fell from 1,646 to 1,344 µs while no statement moved, and PostgreSQL's commit in this window is 1,343 µs. A delta
  on a fsync-bound row is the sync's, not the engine's.
- **The join planner** (CB/CC): `compare-all` and `compare-one` are reordered (`reordered=1`) and read 14x fewer pages and 6 % to 11 % fewer examined rows, and run 1 % to 6 % faster, inside the noise; the three-relation join and scenario 3's joins keep the written order.
  The planner's price is not measurable in this suite and neither is its payoff.

## Where KDS stands against PostgreSQL

- **Ahead**: an unindexed non-pk equality walk (scenario 1 `day-slice` 1.43x, scenario 3 `loans-by-user` 1.15x, scenario 4 `board` 1.19x);
  the pk-anchored join (1.79x) and the three-relation join (1.46x); the correlated `EXISTS` (2.08x at 10,000 loans); a Cabin-served equality (scenario 4 `board` 2.1x, `tape10k` 2.5x of a PostgreSQL seq scan);
  the write statements of a booking (p50 0.73x to 0.84x of PostgreSQL's); loads (0.96x to 1.62x).
- **Even**: one durable client (0.99x), bound by the device's fsync on both engines; the round trip of a pk read at 10,000 loans (`pk-user` 0.98x).
- **Behind**: every shape that returns many rows (scenario 1 `bar-range` 0.49x, `symbol-history` 0.43x, `model-join` 0.56x; scenario 3 `books-by-genre`, `loans-by-daterange`, `overdue` 0.61x to 0.77x), whole-relation folds
  (scenario 1 `agg-*` 0.65x to 0.69x, scenario 4 `close` 0.57x), and the fixed round trip at small sizes (scenario 3 at 200 loans 0.62x to 0.99x on ten of twelve shapes; scenario 4 `tape200` 0.73x). The
  per-row result cost is about 2x PostgreSQL's (7.5 to 9.3 µs against 3.6 to 4.1 µs per returned row, at every size from 252 to 60,480 bars).

## What this run does not show

- **No limit was found for scenario 2.** The booker sweep reaches 3,768 tps at 32 bookers with the host at most 67 % busy; the cells are 3,000 bookings (under two seconds of booking time) driven from one Python process, so the client may be the bound.
  A saturation measurement needs more bookings and a second client process (rule 4b).
- **Scenario 3's cells at 200 and 1,000 loans carry 20 % to 43 % spreads** (one slow run in three of a 20 ms cell); no comparison there, cross-version or against PostgreSQL, is read.
- **Scenario 2's row-set axis is not swept** (rule 9): its sizes are fixed by the driver's arguments, as in v3.1.0.
- **Scenario 4's PostgreSQL twin has no index** (a Cabin's fair counterpart would be one on `symbol`), runs day 1 only, and is unpaced.
- Whether the factor of two in per-row result cost lives in the engine or in the two Python wire decoders (`tools/kwp.py` against `tools/pg_wire.py`) is not separated.
- None of the five files has had a `critics-developer` review.

## Contamination

No KDS cell was discarded: a 1-second monitor and a per-cell validity check found no build, `ctest` or foreign process at 50 % CPU in any window (the only foreign process above 10 % CPU was a `claude` process, 23 samples of 1,518). One batch of
six PostgreSQL scenario-3 attempts failed in seconds on a connection error (`role "ec2-user" does not exist`, the twin's default `--user`); they are kept in `contaminated/` of the run archive and the six cells were re-run with `--user cdkbs`.
The unrelated `kds_server` on port 15432 was not touched.

## Archive

- `bench/v3.3.0/archive/scenarioN-v3.3.0-33-g9682b725/`: each scenario's per-cell JSON (KDS and PostgreSQL), driver stdout, server configs. Scenarios 1 and 3 also hold `plans/` (the full ANALYZE replies of the join-order probe).
- `bench/v3.3.0/archive/run-v3.3.0-33-g9682b725/`: what the four scenarios share: the run scripts (`lib.sh`, `run_all.sh`, `run_plan.sh`, watchdog and monitor), the timeline, the monitor and watchdog logs, the validity log, the binary hash, the contaminated attempt,
  and the table generators under `analysis/`.
