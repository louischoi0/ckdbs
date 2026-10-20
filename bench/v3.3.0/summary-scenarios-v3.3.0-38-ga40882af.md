# Scenarios 1 to 4 at `v3.3.0-38-ga40882af`: summary

**One line.** On the same engine binary as this morning's run (`7532bd14...`, `9682b725`), a faster Python client decoder (`tools/kwp.py`, `tools/ckdbs_cli.py` at `a40882af`) moves exactly the shapes that return many rows, by a factor of 1.4x to 1.9x, and nothing else:
scenario 1's `bar-range`, `symbol-history` and `model-join` go from 0.49x, 0.43x and 0.56x of PostgreSQL 18.6 to 0.90x, 0.85x and 0.94x, scenario 3's `books-by-genre`, `loans-by-daterange` and `overdue` from 0.69x, 0.61x and 0.77x to 0.96x, 0.86x and 0.99x,
and scenario 4's `board` and `close` gain 10 % to 22 % and 17 % to 19 %. **The per-row cost of a returned row halves (6.9 and 9.5 µs to 3.6 and 4.8 µs on scenario 1) and what is left of the gap to PostgreSQL is 0.34 to 0.76 µs per row,
56 % of it (0.43 µs) engine-side and not CPU: KDS sends a reply only at statement completion, so the server's work is not overlapped with the client's.** The engine did not change and the writes, the commit, the walks and the Cabin controller did not move.
Where a comparison to this morning disagrees with the interleaved old/new client A/B, the A/B is the reading: the one-core scan shapes of scenarios 1 and 3 read 11 % to 16 % below the morning *with the old client too*, a host effect that is not the client's.

This file indexes the four results files and carries no number they do not. Measured by `ck-tester` from 2026-10-20 06:46 UTC to 09:12 UTC (the matrix 06:46 to 08:08 UTC, the join-order probe 08:08 to 08:18, scenario 2's A/B 08:18 to 08:23,
the row-cost attribution 08:23 to 09:12) on **one binary, the morning's, `sha256 7532bd14...`**. Budget lifted by the operator for this measurement; PostgreSQL reused from the same-day run at v3.3.0-33-g9682b725.

**Comparators.** KDS: this engine's own run of this morning, `v3.3.0-33-g9682b725` (2026-10-20 03:37 to 05:12 UTC), the **same binary** with the previous client, so every KDS delta here is a client change plus 3 h of host drift;
the A/B arms in the same window separate the two. PostgreSQL 18.6: the morning's fresh run (`/home/cdkbs/pg-bench-ba` port 15434, 04:22 to 04:33 UTC), **not re-run**, so a KDS/PG ratio carries the drift between its window and this one (it is nil
on the many-row shapes, where the client moved the number by a factor of two, and up to 11 % to 16 % on the one-core scan shapes). PostgreSQL is the floor, not the reference.

## Stamp (common to the four results files)

Each results file keeps its own date and time, driver and cell windows and points here for the rest. Everything the morning's summary stamps and this table does not repeat is unchanged (build type, host, device, server config, ports, quiet-host gate, monitor, PostgreSQL setup).

| Field | Value |
|---|---|
| Worktree / branch | `kwp-decoder-fast-path` / `kwp-decoder-fast-path` (the main checkout is not used) |
| HEAD | `a40882af`, `git describe --tags` = `v3.3.0-38-ga40882af` ("kwp: decode a row batch in one offset loop and resolve each column's renderer once"). It changes `tools/kwp.py` and `tools/ckdbs_cli.py` and nothing else (`git diff 9682b725 a40882af -- src include CMakeLists.txt` is empty), on a branch that carries the morning's results files |
| Tree cleanliness | Clean (`git status` empty) at the start of the measurement; the drivers are the unmodified `tools/` scripts; the results files under `bench/v3.3.0/` are additions committed after the measurement |
| Binary provenance | **not rebuilt.** `/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/kds_server` is a copy of the morning's `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/kds_server`; **sha256 `7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a`**, equal to the morning's, checked before the first cell and after the last; that binary was built at `9682b725` as Release, source mtime 2026-10-20 03:36:57.38 UTC, 3 min 17 s after its commit. Every server started from the copy; no build ran during the measurement |
| Client | the matrix cells: this worktree's `tools/` (`a40882af`). The A/B arm `old`: `a40882af^`'s (`f829964c`) `tools/`, extracted with `git archive` into `bench-runs/v3.3.0-38-ga40882af/oldtools/`, first on `PYTHONPATH`; the tree was not modified |
| Device | data files under `/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/data/`, `/dev/root`, `ext4` (`df -T`; `/` 95 % full, 13 GB free); never tmpfs. A fresh data file per cell (per server in scenario 4), deleted when the cell finished. The PostgreSQL cluster (`/home/cdkbs/pg-bench-ba`, on the same device) was neither started nor stopped nor queried |
| Build type | Release |
| Host | 8 logical CPUs, AMD EPYC 9V74 (1 socket x 4 cores x 2 threads), Linux 7.0.0-1014-azure, a shared development VM. Two unrelated `kds_server` processes (ports 15432 and a sandbox) and the PostgreSQL twin were running throughout (the twin received no query) |
| Time budget | **Budget lifted by the operator for this measurement; PostgreSQL reused from the same-day run at v3.3.0-33-g9682b725.** Every KDS cell of the morning's plan ran with its repeat counts: scenario 1 (3 x 3 matrix cells, 3 x 3 size-sweep cells), scenario 2 (the booker sweep and the saturation cells included), scenario 3 (36 cells), scenario 4 (two runs), the join-order probe, plus six A/B pairs per scenario 1, 3 and 4 and scenario 2's two A/B cells |
| Server config | `log_level = warn`, `buffer_pool_frames = 65536` (required since BE), `cores` and `durability` per cell, all else default. Ports 15604 to 15607, 15610, 15620, 15651 to 15653, 15690, never 15432 |
| Quiet host | before each cell a gate waited for no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process and at most 8 % busy CPU over 3 s (`lib.sh` in the archive; every gate wait was 0 s, 152 cells). **A 1-second monitor ran through the whole run** (`monitor.log`, 2,592 samples to 08:23, then `rowcost/monitor*.log`) and every cell was checked against it after it ended (`cellvalid.py`): a build/test process or a foreign process at 50 % CPU or more inside the window discards and re-runs the cell. The outcome is under Contamination |
| 60 s watchdog | `watchdog.sh`, `watchdog2.sh`, `watchdog3.sh` and the row-cost watchdogs poll a status or progress file's age and a recorded PID (never a process pattern); each found its master gone at the hand-over to the next stage and launched it, and none found a stall. No run was orphaned |
| PostgreSQL | 18.6, defaults (`shared_buffers = 128MB`, `synchronous_commit = on`, `fsync = on`, `max_wal_size = 1GB`), **not run**; the numbers are the morning's, `bench/v3.3.0/archive/scenarioN-v3.3.0-33-g9682b725/json/pg-*.json` |

## What counts as a finding

Stated once, for all four results files, and unchanged from the morning's summary. A cell is run three times (the booker sweep and the saturation cells once). **Spread = (max - min) / median over the runs of one cell.** A difference between two medians
that is smaller than the larger of the two spreads is not a finding. A cell whose spread is above 20 % is reported and carries no delta claim. A ratio against a control that cannot be affected (a one-row read, scenario 4's `pk` group) sets the floor of
a comparison that has no repeat: scenario 4's is about 1.10x. **The A/B pairs are the exception to "3 h of drift": old and new alternate within minutes, three pairs, and the rule applies to their own spreads.**

## Headlines

Warm qps (scenario 1), tps (2), derived qps (3, 4), median of three runs (scenario 4: run A, day 1). Delta = now over the morning; ratio = KDS over PostgreSQL (above 1.00x = KDS faster); the last column is the interleaved A/B of the same measure, where one exists
(scenario 2's eight-booker `cores = 8` cells have no A/B; its A/B ran at `cores = 1`):

| measure | KDS now | KDS morning (same binary, old client) | delta | PostgreSQL 18.6 (morning's run, reused) | KDS/PG now | KDS/PG morning | A/B: new over old client (same window) |
|---|---|---|---|---|---|---|---|
| 1 backtest, `day-slice` (warm, `cores = 1`) | 508.7 qps | 591.3 qps | -14.0 % | 413.5 qps | 1.23x | 1.43x | 1.023x |
| 1 backtest, `cross-join` (warm, `cores = 1`) | 496.9 qps | 575.8 qps | -13.7 % | 393.1 qps | 1.26x | 1.46x | 1.019x |
| 1 backtest, `point-join` (warm, `cores = 1`) | 10,589.7 qps | 12,674.6 qps | -16.4 % | 7,071.3 qps | 1.50x | 1.79x | 0.978x |
| 1 backtest, `bar-range` (warm, `cores = 1`) | 1,260.3 qps | 684.4 qps | +84.1 % | 1,399.8 qps | 0.90x | 0.49x | 1.831x |
| 1 backtest, `symbol-history` (warm, `cores = 1`) | 27.3 qps | 13.8 qps | +97.8 % | 32.3 qps | 0.85x | 0.43x | 1.923x |
| 1 backtest, `model-join` (warm, `cores = 1`) | 1,030.2 qps | 614.8 qps | +67.6 % | 1,092.2 qps | 0.94x | 0.56x | 1.753x |
| 2 freight, 8 bookers, `cores = 8`, `group` | 1,990.6 tps | 1,965.0 tps | +1.3 % | no counterpart (one booker) | - | - | - |
| 2 freight, 8 bookers, `cores = 8`, `strict` | 1,716.7 tps | 1,674.2 tps | +2.5 % | no counterpart (one booker) | - | - | - |
| 2 freight, 1 booker, durable, `group` | 425.4 tps | 414.8 tps | +2.6 % | 419.5 tps | 1.01x | 0.99x | 1.032x |
| 2 freight, 1 booker, durable, `strict` | 434.3 tps | 417.1 tps | +4.1 % | 419.5 tps | 1.04x | 0.99x | - |
| 3 library, `loans-by-user` (10,000 loans, unindexed), `cores = 1` | 2,449 qps | 2,779 qps | -11.9 % | 2,421 qps | 1.01x | 1.15x | 1.020x |
| 3 library, `join-no-literal` (10,000 loans, unindexed), `cores = 1` | 1,666 qps | 1,683 qps | -1.0 % | 1,504 qps | 1.11x | 1.12x | 1.133x |
| 3 library, `exists-correlated` (10,000 loans, unindexed), `cores = 1` | 1,608 qps | 1,596 qps | +0.7 % | 768 qps | 2.09x | 2.08x | 1.000x |
| 3 library, `books-by-genre` (10,000 loans, unindexed), `cores = 1` | 2,925 qps | 2,092 qps | +39.8 % | 3,041 qps | 0.96x | 0.69x | 1.500x |
| 3 library, `loans-by-daterange` (10,000 loans, unindexed), `cores = 1` | 913 qps | 652 qps | +40.0 % | 1,063 qps | 0.86x | 0.61x | 1.510x |
| 3 library, `overdue` (10,000 loans, unindexed), `cores = 1` | 1,126 qps | 873 qps | +29.0 % | 1,135 qps | 0.99x | 0.77x | 1.406x |
| 3 library, `loans-by-user` (10,000 loans, indexed), `cores = 1` | 11,682 qps | 11,001 qps | +6.2 % | 10,834 qps | 1.08x | 1.02x | - |
| 3 library, geometric mean over 12 shapes (10,000 loans, unindexed) | - | - | - | - | 1.07x | 1.06x | 1.121x |
| 3 library, geometric mean over 12 shapes (10,000 loans, indexed) | - | - | - | - | 1.14x | 0.98x | - |
| 4 cabinopt, `board` day 1, arm `off` (run A) | 2,562 qps | 2,344 qps | +9.3 % | 1,976 qps (no index) | 1.30x | 1.19x | 1.075x |
| 4 cabinopt, `board` day 1, arm `on` (run A) | 4,876 qps | 4,189 qps | +16.4 % | 1,976 qps (no index) | 2.47x | 2.12x | 1.214x |
| 4 cabinopt, `board` day 1, arm `declared` (run A) | 4,002 qps | 3,621 qps | +10.5 % | 1,976 qps (no index) | 2.03x | 1.83x | 1.172x |
| 4 cabinopt, `tape10k` day 1, arm `off` (run A) | 2,837 qps | 2,679 qps | +5.9 % | 2,100 qps (no index) | 1.35x | 1.28x | 1.023x |
| 4 cabinopt, `tape10k` day 1, arm `on` (run A) | 5,800 qps | 5,376 qps | +7.9 % | 2,100 qps (no index) | 2.76x | 2.56x | 1.081x |
| 4 cabinopt, `close` day 1, arm `off` (run A) | 776 qps | 671 qps | +15.6 % | 1,185 qps (no index) | 0.65x | 0.57x | 1.167x |

Files: `results-scenario1-backtest-v3.3.0-38-ga40882af.md`, `results-scenario2-freight-v3.3.0-38-ga40882af.md`, `results-scenario3-library-v3.3.0-38-ga40882af.md`, `results-scenario4-cabinopt-days-v3.3.0-38-ga40882af.md`.

## The movers, and what they are

- **Every shape that returns many rows is 1.4x to 1.9x faster, and the A/B says the client is all of it.** Interleaved old/new on scenario 1: `bar-range` 1.83x, `symbol-history` 1.92x, `model-join` 1.75x; scenario 3 unindexed 10,000 loans:
  `books-by-genre` 1.50x, `loans-by-daterange` 1.51x, `overdue` 1.41x, `join-no-literal` 1.13x; scenario 4 `board` 1.07x / 1.21x / 1.17x (`off` / `on` / `declared`)
  and `close` 1.17x to 1.21x. The cause is the client's: a returned row costs the Python decoder 5.28 µs of CPU and the new one 2.20 (`kwp._row_batch`) and the render to text 3.51 and 2.05 µs (`ckdbs_cli.send_command`).
- **What did not move.** One-row and few-row reads (the pk shapes, the folds, the Cabin-served statements of scenario 4 except the 8-row ones), the writes, the commit (scenario 2: the A/B reads 1.006x and 1.032x, inside
  their spreads), the unserved equality walk of scenario 4 (26.1 ns per row against 26.5 ns; server CPU 2.20 s against 2.22 s) and the Cabin controller's lifecycle (identical snapshot for snapshot).
- **A host effect on the one-core scans of the large integer relations, not the client's.** Scenario 1's `day-slice` and `cross-join` are -14 % against the morning and scenario 3's integer-column equalities are -12 % to -16 %; the old client reads the same shapes 17 % below the morning in the same window,
  the new one 2 % above the old. Scenario 2's cells read +1 % to +10 % against the morning with the old client reading the same gain in this window. The sign of the host's effect depends on the workload; none of it is separated into frequency, a neighbouring tenant or placement.
  **A comparison across hours that does not interleave its arms cannot read a 10 % difference on a scan shape in this environment.**
- **The join planner's payoff is the same 6 % to 7 %** as in the morning on the two reordered joins (scenario 1 `compare-one` 1.07x planned over written, spreads 1.9 % and 1.8 %), unmoved by a shorter statement. Scenario 3's three joins keep the written order and are within spread.

## Where KDS stands against PostgreSQL now, and what is left of the gap

PostgreSQL's numbers are the morning's, so the ratios below carry that window's difference.

- **Ahead**: the pk-anchored join (1.50x to 1.98x), the three-relation join (1.26x), the correlated `EXISTS` (2.09x at 10,000 loans), a Cabin-served equality (scenario 4 run A day 1: `board` 2.47x, `tape10k` 2.76x of a PostgreSQL seq scan),
  the write statements of a booking (p50 0.75x to 0.88x of PostgreSQL's), the big load phases of scenario 1 (1.47x to 1.65x), scenario 3's unindexed 10,000-loan equalities (1.00x to 1.10x, from 1.08x to 1.25x: the drift above).
- **Even**: one durable client (1.01x and 1.04x), bound by the device's fsync on both engines; the pk read round trip (0.98x to 1.00x at 10,000 loans).
- **Behind**: the many-row shapes by 6 % to 17 % where they were behind by 2x (scenario 1 `bar-range` 0.90x, `symbol-history` 0.85x, `model-join` 0.94x; scenario 3 `loans-by-daterange` 0.86x, `books-by-genre` 0.96x); whole-relation folds (scenario 1 `agg-global` and `agg-distinct` 0.68x, which return one row and
  are not the decoder's; scenario 4 `close` 0.65x in run A and 0.67x as the median of the two runs, from 0.57x); small scans (scenario 4 `tape200` 0.82x to 0.83x, from 0.73x).

**What part of the per-row gap remains, and what it is.** The corrected attribution (`bench/v3.3.0/results-row-cost-attribution-v3.3.0-37-gf829964c.md`, branch `row-cost-attribution`): about 84 % of the gap was the Python decoder, about 16 % (0.42 µs per row) engine-side, because
`WireResultSink::Emit` and `KwpSession::Deliver` (BN-R7) send the whole reply at statement completion and the server's work is never overlapped with the client's. This run re-measured KDS's side of that with the fix in (`rowcost/`, KDS only, PostgreSQL's figures the attribution's):

| component (µs per returned row, 12-column `daily_stats`, slope over 1 to 10,000 rows) | KDS, old client (this window) | KDS, new client | PostgreSQL 18.6 (attribution file, not re-measured) | new / old | new / PG |
|---|---|---|---|---|---|
| Wall slope, `values` (cells as Python ints) | 6.31 µs | 3.21 µs | 3.57 µs | 0.51x | 0.90x |
| Server CPU (`drain`, fixed 3 s windows) | 0.42 µs | 0.42 µs | 0.57 µs | 1.00x | 0.74x |
| Client CPU, total (`values`) | 5.89 µs | 2.79 µs | 3.55 µs | 0.47x | 0.79x |
| - receive (`drain` client CPU) | 0.02 µs | 0.02 µs | 0.27 µs | 1.00x | 0.07x |
| - split cells into `bytes` (`split` minus `drain`) | 5.28 µs | 2.20 µs | 2.59 µs | 0.42x | 0.85x |
| - convert to int (`values` minus `split`) | 0.59 µs | 0.57 µs | 0.69 µs | 0.96x | 0.82x |
| Client idle = wall - client CPU (`values`) | 0.43 µs | 0.42 µs | 0.01 µs | 0.98x | - |
| Wall slope, `drain` (decode removed) | 0.42 µs | 0.42 µs | 0.57 µs | 1.00x | 0.74x |
| **Wall slope, `send` (the scenario drivers' path: decode, render every cell to text, join)** | 9.25 µs | 4.70 µs | - | 0.51x | - |
| - client CPU, `send` | 8.80 µs | 4.27 µs | - | 0.49x | - |
| - of which render and join (`send` client CPU minus `split` client CPU) | 3.51 µs | 2.05 µs | - | 0.59x | - |
| - client idle, `send` (wall - client CPU) | 0.45 µs | 0.43 µs | - | 0.96x | - |

- **On a 12-column key-range read decoded to Python ints, KDS's wall slope is 3.21 µs per row against PostgreSQL's 3.57 µs (0.90x, a cross-window figure)**; the decode is no longer where KDS is behind (2.20 against 2.59 µs of client CPU).
- **On the scenario drivers' own path (decode, render every cell to text, join) it is 4.70 µs per row** (9.25 with the old client), matching the scenario's `symbol-history` figure (4.84 µs per row at 7,560 rows) against PostgreSQL's 4.08 µs. Of that
  0.76 µs gap, **0.43 µs (56 %) is the engine-side non-overlap** (the client idles while the server builds the reply; KDS's server CPU of 0.42 µs per row is never hidden behind the client's work) and
  **0.33 µs (44 %) is client work**, the render of binary cells to the newline protocol's text shape (2.05 µs of CPU per row) that PostgreSQL's text rows do not need. The split is by subtraction against PostgreSQL's
  attribution figures and was not measured on PostgreSQL's scenario path. For `bar-range` (nine columns, 0.34 µs per row) the engine-side term was not measured.
- Removing the non-overlap is an engine change (stream row batches as they are sealed rather than at completion, against BN-R7's one-write reply); the render step is a client-side choice. Neither is attempted here (rule: no engine edits to make a benchmark pass).

## What this run does not show

- **No limit was found for scenario 2.** The booker sweep reaches 3,930 tps at 32 bookers (`group`) with the host at most 65 % busy, driven from one Python process; the cells are 3,000 bookings. A saturation measurement needs more bookings and a second client process.
- **The host effect is not explained.** It is shown by the A/B (old client 493 qps on `day-slice` now, 591 qps this morning, same binary) and not separated into cause.
- **PostgreSQL was not re-measured.** A KDS/PG ratio on a one-core scan shape therefore has the drift in it; the row-cost comparison of the attribution table mixes a 06:19 PostgreSQL window with an 08:23 to 09:12 KDS window.
- **Scenario 3's 200-loan and 1,000-loan columns at `cores = 1` carry fewer spreads above 20 % than the morning's** (12 of 48 cells against 29 of 48) and more at `cores = 8` (10 of 48 against 6 of 48); no comparison on a cell above the line is read.
- **Scenario 4's PostgreSQL twin has no index**, runs day 1 only and is unpaced, as in the morning.
- The row-cost attribution's PostgreSQL leg is the attribution file's; its text-format caveat (PostgreSQL ships text, KDS binary) stands.

## Contamination

Five cells were discarded and re-run, none of them in the numbers reported:

| cell | window | what the monitor saw | action |
|---|---|---|---|
| `s2-c1-s` (attempt 1) | 06:54:33 to 06:54:52 | `claude --resume` (another agent session) at 52.5 % CPU in one sample of 11 | re-run; attempt kept in `contaminated/` |
| `pl-s3-written-none-r3` (attempts 1 and 2) | 08:14 to 08:15 | `python3` at 50 %+ CPU: the ck-tester's own table-generation scripts, run while the join-order probe was running | re-run twice more; both attempts kept; no analysis script was run during a cell afterwards |
| row-cost `values` 10,000 rows, repeat 2, new client | about 08:28:31 | `claude` at 89 % and a `node` bridge process at 62 % | the cell was moved to `rowcost/contaminated/` and re-run |
| row-cost `split` 1,000 rows, repeat 3, old client | about 08:44:02 | `claude` at 123 % (the ck-tester's own harness) | same |
| row-cost `send` 10,000 rows, repeat 1, new client | about 08:50:08 | kernel writeback workers (`kworker`, `cgwb_release`) at 99 %, 80 % and 53 %, probably the writeback after the previous server's data directory was deleted (not verified) | same |

The windows of the row-cost cells are reconstructed from the sweep's own order and durations (`rc_valid.py`), not timestamped, and the first seconds of the first row-cost sweep ran before its monitor was started. Of the 2,592 samples in `monitor.log`, 25 list a foreign process above 10 % CPU;
three of them (the `s2-c1-s` cell and the two `pl-s3-written-none-r3` attempts) were at 50 % or more inside a cell window, and the rest are minor (10 % to 50 %, mostly the ck-tester's own harness). The unrelated `kds_server` on port 15432 and the PostgreSQL twin on 15434 were not touched.

## Archive

- `bench/v3.3.0/archive/scenarioN-v3.3.0-38-ga40882af/`: each scenario's per-cell JSON, driver stdout, server configs, and for scenarios 1 and 3 the join-order probe's ANALYZE replies (`plans/`); the A/B cells are among them (`ab-old*`, `ab-new*`).
- `bench/v3.3.0/archive/run-v3.3.0-38-ga40882af/`: what the four scenarios share: the run scripts (`lib.sh`, `run_all.sh`, `run_plan.sh`, `run_post.sh`, watchdogs, monitors), the timeline, the monitor and watchdog logs, the validity log, the binary hashes, the contaminated attempts,
  the table generators under `analysis/`, and `rowcost/` (the KDS-only row-cost sweep: `kdsrow.py`, its `rowcost.py` import from the attribution branch, the cells as gzipped JSONL with every statement's latency, the monitor logs, the contaminated cells). `kdsrow.py` gained its `send` mode after the first two sweeps; both versions produce identical `values` and `split` cells.
  The old client's sources are `git show a40882af^:tools/kwp.py` and `:tools/ckdbs_cli.py`.
