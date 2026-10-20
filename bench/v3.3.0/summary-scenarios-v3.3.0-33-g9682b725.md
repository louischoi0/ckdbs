# Scenarios 1 to 4 at `v3.3.0-33-g9682b725`: summary

**One line.** The engine at `v3.3.0-33-g9682b725` is, within the run's spreads, the engine of v3.1.0 on most shapes this suite measures; the largest movers are two: the unserved equality **walk** (scenario 4's `off` arm is
1.18x to 1.20x faster on a 10,000-row board and tape) and the **inner-built two-relation join** (scenario 3's `join-no-literal` at 10,000 loans runs 1.98x). Smaller movers (eight-core one-connection reads, scenario 1's one-core scan shapes, eight contended bookers) are listed below; the other reads, the writes, the Cabin-served rates and the controller lifecycle are inside the
run's own spreads. **Scenario 1's driver now runs unmodified** (BZ made its `HEAP` tables hidden-key btrees; no BTREE copy was needed).
**The join planner (CB/CC) reorders two of scenario 1's joins and moves neither beyond noise, and it changes nothing in scenario 3's joins**: A/B against `join_order = written` on the same binary.
One durable client is PostgreSQL's equal (0.99x), because both engines wait on the same fsync. PostgreSQL 18.6 was **re-measured fresh in this window**, so every ratio below is same-window.

This file indexes the four results files and carries no number they do not. Measured by `ck-tester` from 2026-10-20 03:37 UTC to 04:44 UTC (plus the three-cell `bar-lookup` re-run of scenario 1, 05:09 to 05:12 UTC) on one `build-release` binary built at `9682b725` (sha256
`7532bd14...`, the Stamp below). **The operator lifted the time budget for this run** ("lift the budget, re-measure PG too"): every cell of the v3.1.0 plan ran, with its repeat counts, nothing was cut,
and the size sweep (scenario 1) and booker sweep (scenario 2) that v3.1.0 left unfinished or unwritten were run and written up.

**Comparators.** KDS: v3.1.0 at `cd7c7c4e` (`v3.1.0-30-gcd7c7c4e`, measured 2026-10-13 to 14), 240 commits away (101 first-parent, 23 of them merges), so every KDS delta is **cross-version** and attributable to no
single change; re-derived from that run's archived JSON. Scenario 1's v3.1.0 cells ran a BTREE-only copy of the driver. PostgreSQL 18.6: **fresh**, `/home/cdkbs/pg-bench-ba` port 15434 at its defaults, three runs
per cell wherever a twin exists, run after the KDS cells and never concurrent with one. PostgreSQL is the floor, not the reference.

## Stamp (common to the four results files)

Each results file keeps its own date and time, driver, device and binary-hash line and points here for the rest.

| Field | Value |
|---|---|
| Worktree / branch | `bench-scenarios-1-4` / `bench-scenarios-1-4` (the main checkout is not used) |
| HEAD | `9682b725`, `git describe --tags` = `v3.3.0-33-g9682b725` ("Merge fix-fk-self-reference-refused: a self-referencing foreign key is refused NotImplemented at both doors"), committed 2026-10-20 03:33:40 UTC; equal to `origin/main` when the run began |
| Tree cleanliness | Clean (`git status` empty) at the start and at the end of the measurement; nothing under `src/`, `include/`, `tools/` or `CMakeLists.txt` was edited, and the drivers are the unmodified `tools/` scripts. `build-release/` is ignored by git. The results files under `bench/v3.3.0/` are additions committed after the measurement |
| Binary provenance | `build-release/kds_server`, built in this worktree at HEAD with `cmake -DCMAKE_BUILD_TYPE=Release` (`CMakeCache.txt`: `Release`), target `kds_server`, `-j4`, finished 2026-10-20 03:36:57 UTC, 3 min 17 s **after** the HEAD commit, so the binary is not older than the engine. Copied to `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/kds_server` before the first cell; **copy sha256 `7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a`** (equal to the source binary's, checked again after the run); source binary mtime 2026-10-20 03:36:57.38 UTC. Every server started from the copy; no build ran during any reported cell (the only build of the session finished before the first cell) |
| Device | data files under `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/data/`, `/dev/root`, `ext4` (`df -T`); never tmpfs. A fresh data file per cell (per server in scenario 4), deleted when the cell finished. The PostgreSQL cluster (`/home/cdkbs/pg-bench-ba`) is on the same device |
| Build type | Release |
| Host | 8 logical CPUs, AMD EPYC 9V74 (1 socket x 4 cores x 2 threads), Linux 7.0.0-1014-azure, a shared development VM; `/` was 95 % full (14 GB free) |
| Time budget | **Budget lifted by the operator for this run** (order of 2026-10-20: "lift the budget, re-measure PG too"). Every cell of the v3.1.0 rebaseline's plan ran, with its repeat counts, and PostgreSQL was re-measured fresh in the same window. Nothing was cut |
| Server config | `log_level = warn`, `buffer_pool_frames = 65536` (required since BE), `cores` and `durability` per cell, all else default. Ports 15604 to 15607, 15610, 15620, 15651 to 15653, never 15432 (held by an unrelated `kds_server`) |
| Quiet host | before each cell a gate waited for no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process and at most 8 % busy CPU over 3 s (`lib.sh` in the archive); `/proc/loadavg` and the process list are in each cell's precheck. **A 1-second monitor ran through the whole run** (`monitor.log`: 1,500+ samples of foreign processes above 10 % CPU) and every cell was checked against it after it ended (`cellvalid.py`): a build/test process or a foreign process at 50 % CPU or more inside the window discards and re-runs the cell. The outcome is under Contamination below |
| PostgreSQL | PostgreSQL 18.6 (`/home/cdkbs/pg-bench-ba`, port 15434), defaults (`shared_buffers = 128MB`, `synchronous_commit = on`, `fsync = on`, `max_wal_size = 1GB`), not started or stopped by this run. **Re-measured fresh in this window** (the twin runs follow the KDS cells, never concurrent with them) |

## What counts as a finding

Stated once, for all four results files. A cell is run three times (the booker sweep and the saturation cells once). **Spread = (max - min) / median over the runs of one cell**; `bench/README.md` names no spread, and this is the definition every file here uses, for this run's cells and for the v3.1.0 cells re-derived from its JSON. A difference between two medians that is smaller than the larger of the two spreads is not a finding. A cell whose spread is above 20 % is reported and carries no delta claim. A ratio
against a control that cannot be affected (a pk point read, a Cabin-free `pk` group) sets the floor of a comparison that has no repeat: scenario 4's is about 1.10x. Cross-version deltas are held to the same rule with v3.1.0's spread.

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
  34.6 to 26.5 ns and the server CPU of an `off` run's boards from 2.80 s to 2.22 s. Scenario 3's integer-column equality walk shows **no** such gain (`loans-by-user` 2,779 qps in both versions) and scenario 1's (`day-slice`, `int32`) a smaller one
  (+7.3 % `group`, +8.9 % `strict` at `cores = 1`), so the gain is uneven; the filtered column here is a `varchar`, among other differences. Likely cause: not separated by this run, 240 commits.
- **`join-no-literal` at 10,000 loans is 1.98x (scenario 3).** The plan shows `inner_built=1` (a statement-local hash build of `loans`, held to 79 rows by range bounds CB-S1 derives across the join key). The planner's
  order choice (CB-S2) is not the cause: with `join_order = written` the same cell is 5 % *faster*, and its plan carries the same build and the same derived bounds, so the A/B does not exclude CB-S1. Whether v3.1.0 built cannot be read
  from its JSON (step kinds only); its 1,175 µs mean is far below the ~3.9 ms that 16 full scans would cost, so it most likely did not scan per outer row. Candidate causes, none separated: CB-S1's derived bounds, the inner build's reach (`join-inner-build.md`), the scan beneath it.
- **Three `cores = 8` one-connection read shapes gained 28 % to 31 % (scenario 1:** `day-slice`, `cross-join`, `point-join`; the other four moved -1.4 % to +5.7 %). At v3.1.0 `day-slice` on an eight-core instance ran 22 % slower than on one core; now 6 % slower.
  The one-core scan shapes gained too: +7.3 % `day-slice` under `group` (at the edge of its 6.9 % spread) and +8.9 % under `strict` (spreads 1.0 % and 1.8 %).
- **Scenario 1 `bar-lookup` at `cores = 1` (-14.4 % in the matrix) is noise.** A re-run of the cell (three runs) shows the shape landing in a fast mode (12,515 to 12,657 qps) or a slow one (9,258 to 11,264 qps) at random, 3 of 6 runs each; the six-run spread is 28.6 %, and the fast-mode runs are -3.8 % against v3.1.0 within a 3.7 % spread. Not a finding; scenario 1, section 3.
- **Eight contended bookers: +3.3 % `group`, +5.6 % `strict`** (scenario 2), above their spreads (0.5 %, 3.3 %), with 54 / 42 / 39 `strict` conflicts per run against 90 / 63 / 45. Small, real, and carried by the commit (its mean fell 5 % to 9 % at eight cores and 18 % with one booker), not by the statements: every statement p50 in scenario 2's comparison table is 1.01x to 1.11x of v3.1.0's, and scenario 4's `pk` control is +4.7 % (73.4 against 70.1 µs). A 3 % to 7 % per-statement slowdown is visible and is **not separated between engine and host** by this run.
- **One durable booker: +9 % to +11 % against v3.1.0, and equal to PostgreSQL.** It is carried by the commit: its mean fell from 1,646 to 1,344 µs (its p50 only 7 %, so mostly the tail) while no statement got faster, and PostgreSQL's commit
  in this window is 1,343 µs. **Device or engine is not separated**: PostgreSQL was not measured in v3.1.0's window (that run reused 2026-09-30's), and scenario 1's durable autocommit insert did not get faster (744 against 773 rows/s).
- **The join planner** (CB/CC): `compare-all` and `compare-one` are reordered (`reordered=1`) and read 14x fewer pages and 6 % to 11 % fewer examined rows, and run 1 % to 6 % faster, inside the noise; the three-relation join and scenario 3's joins keep the written order.
  The planner's price is not measurable in this suite and neither is its payoff.

## Where KDS stands against PostgreSQL

- **Ahead**: an unindexed non-pk equality walk (scenario 1 `day-slice` 1.43x, scenario 3 `loans-by-user` 1.15x, scenario 4 `board` 1.19x);
  the pk-anchored join (1.79x) and the three-relation join (1.46x); the correlated `EXISTS` (2.08x at 10,000 loans); a Cabin-served equality (scenario 4 `board` 2.1x, `tape10k` 2.5x of a PostgreSQL seq scan);
  the write statements of a booking (p50 0.73x to 0.84x of PostgreSQL's); scenario 1's load phases (1.11x to 1.62x, `load-models` 0.96x).
- **Even**: one durable client (0.99x), bound by the device's fsync on both engines; the round trip of a pk read at 10,000 loans (`pk-user` 0.98x).
- **Behind**: every shape that returns many rows (scenario 1 `bar-range` 0.49x, `symbol-history` 0.43x, `model-join` 0.56x; scenario 3 `books-by-genre`, `loans-by-daterange`, `overdue` 0.61x to 0.77x), whole-relation folds
  (scenario 1 `agg-*` 0.65x to 0.69x on three of its four whole-relation folds, `agg-by-symbol` 0.91x; scenario 4 `close` 0.57x), and small scans (scenario 4 `tape200` 0.73x, where the `pk` round trip differs by only 4 µs, so the
  gap is not the round trip; scenario 3's 200-loan `cores = 1` cells are too noisy to read). The
  per-row result cost is about 2x PostgreSQL's (7.0 to 9.3 µs against 3.3 to 4.0 µs per returned row, each above its engine's own fixed round trip, 2.1x to 2.3x; the `bar-range` and `symbol-history` ratios are the same at 252, 1,008, 10,080 and 60,480 bars).

## What this run does not show

- **No limit was found for scenario 2.** The booker sweep reaches 3,768 tps at 32 bookers with the host at most 67 % busy; the cells are 3,000 bookings (under two seconds of booking time) driven from one Python process, so the client may be the bound.
  A saturation measurement needs more bookings and a second client process (a saturation cell, not a polite load).
- **Scenario 3's cells at 200 and 1,000 loans carry 20 % to 43 % spreads** (one slow run in three of a 20 ms cell); no comparison there, cross-version or against PostgreSQL, is read.
- **Scenario 2's row-set axis is not swept**: its sizes are fixed by the driver's arguments, as in v3.1.0.
- **Scenario 4's PostgreSQL twin has no index** (a Cabin's fair counterpart would be one on `symbol`), runs day 1 only, and is unpaced.
- Whether the factor of two in per-row result cost lives in the engine or in the two Python wire decoders (`tools/kwp.py` against `tools/pg_wire.py`) is not separated.
- The five files had a `critics-developer` review. Its corrections to the derived numbers and causal claims are commit `b3d2a2ae`; the remaining findings (a `bar-lookup` re-run, units, one spread definition, dangling rule references, the 2x per-row claim, and the cuts of the repeated stamp, contamination, finding-rule and insight text) are applied in the commit after it.

## Contamination

No KDS cell was discarded: a 1-second monitor and a per-cell validity check found no build, `ctest` or foreign process at 50 % CPU in any window (the only foreign process above 10 % CPU was a `claude` process, 23 samples of 1,518). One batch of
six PostgreSQL scenario-3 attempts failed in seconds on a connection error (`role "ec2-user" does not exist`, the twin's default `--user`); they are kept in `contaminated/` of the run archive and the six cells were re-run with `--user cdkbs`.
The three `bar-lookup` re-run cells were clean too (validity log). The unrelated `kds_server` on port 15432 was not touched.

## Archive

- `bench/v3.3.0/archive/scenarioN-v3.3.0-33-g9682b725/`: each scenario's per-cell JSON (KDS and PostgreSQL), driver stdout, server configs; scenario 1 also holds the three `rerun-s1-c1-g-r*` cells. Scenarios 1 and 3 also hold `plans/` (the full ANALYZE replies of the join-order probe).
- `bench/v3.3.0/archive/run-v3.3.0-33-g9682b725/`: what the four scenarios share: the run scripts (`lib.sh`, `run_all.sh`, `run_plan.sh`, watchdog and monitor), the timeline, the monitor and watchdog logs, the validity log, the binary hash, the contaminated attempt,
  and the table generators under `analysis/`.
