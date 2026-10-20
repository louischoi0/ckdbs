# Scenario 1 (backtest) at `v3.3.0-33-g9682b725`

**One line.** `tools/scenario1_backtest.py`, **unmodified**, now runs end to end: the `HEAP` relations of its schema (`daily_stats`,
`model_results`, `write_probe`) are btrees with a hidden tuple key since BZ, so the SUS-1 refusal that stopped it at v3.1.0 is gone and
the BTREE copy that run needed is not used. On 60,480 bars an unindexed one-session equality (`day-slice`) runs
591.3 qps and a Cabin on its filter column lifts it 11.1x to
6,557.7 qps; the backtest's three-relation join runs 575.8 qps warm (563.7 qps cold) and
5,570.3 qps with the Cabin; a durable autocommit insert is 744 rows/s and a 1,000-row batch
21,023 rows/s. Against the v3.1.0 BTREE-copy numbers (cross-version) the one-core scan and join shapes gained 5 % to 9 % (above both spreads
under `strict`; under `group` from inside them, `cross-join`, to just above them, `day-slice` and `model-join`), and three `cores = 8` shapes (`day-slice`, `cross-join`, `point-join`) gained 28 to 31 %:
the eight-core penalty `day-slice` paid at v3.1.0 (22 %) is 6 % now; the other four `cores = 8` shapes moved -1.4 % to +5.7 %
(section 9). The one apparent regression, `bar-lookup` at `cores = 1` (-14.4 %), re-ran as noise (section 3). Against PostgreSQL 18.6, **re-measured in the same window**, KDS is ahead on the non-index equality scan
(1.43x), the join (1.46x) and the pk-anchored join
(1.79x), and behind on every shape that returns many rows or folds a whole relation
(`bar-range` 0.49x, `symbol-history` 0.43x,
`model-join` 0.56x). **The engine's join planner (CB/CC) reorders two of the backtest's joins and it does not move them**: section 6.

Driver: `tools/scenario1_backtest.py`, unmodified, with `--seed 1 --bars-clustered btree --verify --sync --analyze`, driver
documentation: `bench/docs/` is closed and the tool's own `--help` carries the flags (`bench/README.md`). Measured by `ck-tester`.
Comparator for the KDS deltas (cross-version): `bench/v3.1.0/results-scenario1-backtest-v3.1.0-30-gcd7c7c4e.md`, whose cells ran the
BTREE-only **copy** of the driver (three `HEAP` keywords changed); this run is the first with the driver as shipped, so the write-side
phases compare a hidden-key btree against an explicit one, which is the same structure. PostgreSQL 18.6: `tools/pg_scenario1_backtest.py`,
**fresh**, three runs at the driver's defaults and three at each sweep size.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; KDS matrix cells 03:37:30 to 03:46:03 UTC (nine cells); size sweep 04:18:34 to 04:20:48 UTC (nine cells); PostgreSQL cells 04:25:05 to 04:33:44 UTC (twelve cells); bar-lookup re-run 05:09 to 05:12 UTC (three cells, section 3); join-order probe 04:34:37 to 04:42:59 UTC (six cells). Per-cell windows: section 10 |
| Device | data files under `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/data/`, `/dev/root`, `ext4` (`df -T`); never tmpfs. A fresh data file per cell (per server in scenario 4), deleted when the cell finished. The PostgreSQL cluster (`/home/cdkbs/pg-bench-ba`) is on the same device |
| Binary | `kds_server` copy in `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/`, **sha256 `7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a`** (equal to the source binary's), source mtime 2026-10-20 03:36:57.38 UTC, built at `9682b725` as Release; provenance in the summary's Stamp |
| Everything else | commit `9682b725` (`v3.3.0-33-g9682b725`), tree clean, Release build, host, server config, quiet-host gate, PostgreSQL setup and the v3.1.0 comparator are stamped once in the summary (`summary-scenarios-v3.3.0-33-g9682b725.md`, "Stamp"). **Budget lifted by the operator for this run**; nothing was cut |
| Driver | **unmodified** `tools/scenario1_backtest.py`; exit code 0 and no verify problem in every KDS cell (`verify_problems` empty in 18 of 18) |

## 2. What was run

The v3.1.0 plan, every cell, with the driver as shipped:

| cell | cores | durability | runs | arguments |
|---|---|---|---|---|
| `s1-c1-g`, `r2-`, `r3-` | 1 core | group | 3 runs | `--seed 1 --bars-clustered btree --verify --sync --analyze` (60,480 bars) |
| `s1-c8-g`, `r2-`, `r3-` | 8 cores | group | 3 runs | same |
| `s1-c1-s`, `r2-`, `r3-` | 1 core | strict | 3 runs | same |
| `s1s-n252-c1-g` | 1 core | group | 3 runs | same plus `--years 1 --symbols 1 --exchanges 1` (252 bars) |
| `s1s-n1008-c1-g` | 1 core | group | 3 runs | same plus `--years 1 --symbols 4 --exchanges 2` (1,008 bars) |
| `s1s-n10080-c1-g` | 1 core | group | 3 runs | same plus `--years 5 --symbols 8 --exchanges 2` (10,080 bars) |
| `pg-s1`, `r2-`, `r3-` | - | `synchronous_commit = on` | 3 runs | `pg_scenario1_backtest.py --seed 1 --verify` (60,480 bars) |
| `pg-s1s-n252` / `n1008` / `n10080` | - | same | 3 runs each | same plus the sweep's `--years/--symbols/--exchanges` |
| `pl-s1-planned-r1..3`, `pl-s1-written-r1..3` | 1 core | group | 3 runs each | the `s1-c1-g` arguments, the server started with `join_order = written` for the second arm, ANALYZE replies dumped |

**Row-count axis.** Bars = 252 sessions per year x years x symbols; `daily_stats` has one row per bar and `daily_bars` one per bar.
The driver's flags map to 252, 1,008, 10,080 and (default `--years 30 --symbols 8`) 60,480 bars, 128,536 rows over seven relations at the default size.
The sweep cells run the same driver at the smaller sizes and are shown in section 7. A cell is one fresh server and a fresh data file,
deleted afterwards. The size is a row label in every sweep table.

Cut: nothing. The operator lifted the budget for this run, so the size sweep that the v3.1.0 run was stopped in (two runs at 252 and 1,008 bars, one at 10,080)
ran with three runs at every size here.

## 3. Results

The QPS sweep (the driver's own controlled matrix, 100 statements per cell, 8 distinct arguments in the warm cases): median of three
runs per cell. `cold` = every argument seen for the first time, `warm` = the same few arguments cycled, `cabin` = warm with a Cabin
declared on the filter column and observed. Qps is the driver's own figure. The v3.1.0 columns are the BTREE-copy cells of
`bench/v3.1.0/results-scenario1-backtest-v3.1.0-30-gcd7c7c4e.md`, re-derived from its archived JSON (three runs), **cross-version**.

Findings are read by the rule stated once in the summary (`summary-scenarios-v3.3.0-33-g9682b725.md`, "What counts as a finding"); a cell whose spread is above 20 % is reported and carries no delta claim.

#### cell c1-g: cores = 1, group (3 runs now, 3 runs at v3.1.0)

| shape | cold | warm | spread of warm | cabin | cabin over warm | warm v3.1.0 (cross-version) | warm delta | cabin v3.1.0 | cabin delta |
|---|---|---|---|---|---|---|---|---|---|
| bar-lookup | 11,982.6 qps | 11,263.5 qps | 13.0 % | n/a | - | 13,152.9 qps | -14.4 % | n/a | - |
| bar-range | 684.1 qps | 684.4 qps | 5.7 % | n/a | - | 670.1 qps | +2.1 % | n/a | - |
| day-slice | 588.9 qps | 591.3 qps | 6.9 % | 6,557.7 qps | 11.09x | 550.9 qps | +7.3 % | 6,574.1 qps | -0.2 % |
| symbol-history | 14.3 qps | 13.8 qps | 6.5 % | 13.6 qps | 0.99x | 14.1 qps | -2.1 % | 13.7 qps | -0.7 % |
| cross-join | 563.7 qps | 575.8 qps | 1.8 % | 5,570.3 qps | 9.67x | 549.2 qps | +4.8 % | 5,791.1 qps | -3.8 % |
| point-join | 12,444.5 qps | 12,674.6 qps | 2.4 % | n/a | - | 12,872.0 qps | -1.5 % | n/a | - |
| model-join | 605.9 qps | 614.8 qps | 6.5 % | 618.0 qps | 1.01x | 569.5 qps | +8.0 % | 577.5 qps | +7.0 % |

#### cell c8-g: cores = 8, group (3 runs now, 3 runs at v3.1.0)

| shape | cold | warm | spread of warm | cabin | cabin over warm | warm v3.1.0 (cross-version) | warm delta | cabin v3.1.0 | cabin delta |
|---|---|---|---|---|---|---|---|---|---|
| bar-lookup | 12,433.4 qps | 12,349.9 qps | 0.2 % | n/a | - | 12,389.8 qps | -0.3 % | n/a | - |
| bar-range | 666.4 qps | 665.2 qps | 1.8 % | n/a | - | 663.1 qps | +0.3 % | n/a | - |
| day-slice | 548.7 qps | 554.4 qps | 2.3 % | 6,248.3 qps | 11.27x | 430.0 qps | +28.9 % | 6,181.5 qps | +1.1 % |
| symbol-history | 14.0 qps | 13.8 qps | 0.7 % | 13.4 qps | 0.97x | 14.0 qps | -1.4 % | 13.6 qps | -1.5 % |
| cross-join | 534.1 qps | 548.1 qps | 1.2 % | 5,271.2 qps | 9.62x | 427.6 qps | +28.2 % | 5,535.2 qps | -4.8 % |
| point-join | 11,634.7 qps | 11,776.5 qps | 1.1 % | n/a | - | 8,966.9 qps | +31.3 % | n/a | - |
| model-join | 576.9 qps | 583.3 qps | 0.9 % | 579.3 qps | 0.99x | 552.1 qps | +5.7 % | 557.0 qps | +4.0 % |

#### cell c1-s: cores = 1, strict (3 runs now, 3 runs at v3.1.0)

| shape | cold | warm | spread of warm | cabin | cabin over warm | warm v3.1.0 (cross-version) | warm delta | cabin v3.1.0 | cabin delta |
|---|---|---|---|---|---|---|---|---|---|
| bar-lookup | 12,762.8 qps | 12,867.1 qps | 28.5 % | n/a | - | 9,434.4 qps | +36.4 % | n/a | - |
| bar-range | 667.3 qps | 678.9 qps | 3.3 % | n/a | - | 667.4 qps | +1.7 % | n/a | - |
| day-slice | 597.7 qps | 599.1 qps | 1.0 % | 6,512.2 qps | 10.87x | 550.1 qps | +8.9 % | 6,443.6 qps | +1.1 % |
| symbol-history | 14.2 qps | 14.2 qps | 4.9 % | 13.8 qps | 0.97x | 14.3 qps | -0.7 % | 13.8 qps | +0.0 % |
| cross-join | 573.1 qps | 578.0 qps | 3.3 % | 5,453.6 qps | 9.44x | 547.6 qps | +5.6 % | 5,782.2 qps | -5.7 % |
| point-join | 12,194.3 qps | 12,216.4 qps | 31.2 % | n/a | - | 13,228.9 qps | -7.7 % | n/a | - |
| model-join | 595.9 qps | 605.8 qps | 4.8 % | 606.2 qps | 1.00x | 571.1 qps | +6.1 % | 584.8 qps | +3.7 % |

**A Cabin lifts the two scan shapes by an order of magnitude and nothing else.** `day-slice` 11.1x and
`cross-join` 9.7x at `cores = 1`; `symbol-history` returns 7,560 rows and gains nothing, and `model-join` is a
probe-per-row join over 359 rows (1.01x). Cold and warm agree within 3 % on the walk shapes: the data is in the pool.

**`bar-lookup` at `cores = 1`: a re-run.** The three `c1-g` runs of the matrix gave a warm `bar-lookup` of 11,052.4 / 11,263.5 / 12,514.6 qps (median 11,263.5), 14.4 % below v3.1.0's 13,152.9 qps, against spreads of 13.0 % now and 3.7 % then. That is below the 20 % exclusion line and larger than both spreads, so by the rule it was a candidate regression and the cell was repeated: `rerun-s1-c1-g-r1..3`, the same binary copy and arguments, 05:09 to 05:12 UTC, one at a time, no build, the monitor on, all three windows clean (`validity.log`).

| run | warm qps | warm p50 | cold qps | phase `read-bar-lookup` qps | mode |
|---|---|---|---|---|---|
| matrix run 1 (`s1-c1-g`) | 11,052.4 qps | 86.9 µs | 11,014.4 qps | 9,781.6 qps | slow |
| matrix run 2 (`r2-s1-c1-g`) | 11,263.5 qps | 88.7 µs | 11,982.6 qps | 12,551.5 qps | slow |
| matrix run 3 (`r3-s1-c1-g`) | 12,514.6 qps | 74.9 µs | 12,801.0 qps | 12,572.2 qps | fast |
| re-run 1 | 9,257.6 qps | 103.3 µs | 9,114.4 qps | 12,857.1 qps | slow |
| re-run 2 | 12,649.6 qps | 74.0 µs | 12,666.3 qps | 12,894.4 qps | fast |
| re-run 3 | 12,657.4 qps | 74.0 µs | 12,618.6 qps | 12,891.6 qps | fast |
| v3.1.0, three runs (cross-version) | 13,484.7 / 13,152.9 / 12,992.6 qps | - | - | 12,761.6 / 9,485.2 / 13,188.5 qps | fast x3 in the sweep, one slow in the phase |
| PostgreSQL 18.6 (fresh), three runs | 14,533.8 / 14,673.3 / 14,777.3 qps | - | - | - | - |

**Verdict: noise, not a regression.** The 100-statement sweep of this shape falls into one of two modes, a fast one (p50 74.0 to 74.9 µs, 12,515 to 12,657 qps) and a slow one (p50 86.9 to 103.3 µs, 9,258 to 11,264 qps), and which one a run lands in is not controlled: the slow mode took 3 of the 6 `c1-g` runs here (2 of the first 3, 1 of the 3 re-runs), and one of v3.1.0's three runs hit it in its separate 200-statement `read-bar-lookup` phase (9,485 qps), and the `c1-s` cell here has a slow run too (9,286 qps). Over all six runs the median is 11,889 qps and the spread 28.6 % (the re-runs alone: 12,650 qps, 26.9 %), above the 20 % line, so no delta is claimed for the shape at `cores = 1`. The fast-mode runs agree to 1.1 % (12,515 to 12,657 qps, median 12,650 qps) and sit 3.8 % under v3.1.0's three fast runs (13,153 qps, spread 3.7 %): that equals the larger spread, so it is not a finding by the rule either, though it has the sign of the 1 % to 11 % per-statement increase that scenario 2 shows (summary). `cores = 8` is steady where `cores = 1` is not (12,354 / 12,333 / 12,350 qps, spread 0.2 %, -0.3 % against v3.1.0), so the shape is not slower where the sweep always landed in the fast mode. Why `cores = 1` mixes the two modes is not explained by this run.

**Writes: the durability wait is the cost of a row.** Rows per `BEGIN`/`COMMIT`; median of three runs with the spread in brackets:

| rows per BEGIN/COMMIT | c1-g now | c8-g now | c1-s now | c1-g v3.1.0 | c8-g v3.1.0 | c1-s v3.1.0 |
|---|---|---|---|---|---|---|
| 1 row | 744 rows/s (5.8 %) | 730 rows/s (2.8 %) | 755 rows/s (1.1 %) | 773 rows/s (25.9 %) | 720 rows/s (4.9 %) | 771 rows/s (39.9 %) |
| 10 rows | 5,551 rows/s (3.3 %) | 5,396 rows/s (4.9 %) | 5,401 rows/s (9.4 %) | 5,327 rows/s (9.0 %) | 4,634 rows/s (6.6 %) | 5,130 rows/s (14.3 %) |
| 100 rows | 15,434 rows/s (6.9 %) | 13,152 rows/s (18.3 %) | 15,933 rows/s (3.5 %) | 15,379 rows/s (6.1 %) | 13,095 rows/s (9.5 %) | 14,922 rows/s (23.9 %) |
| 1000 rows | 21,023 rows/s (2.6 %) | 19,758 rows/s (16.0 %) | 21,171 rows/s (11.9 %) | 18,295 rows/s (23.3 %) | 18,826 rows/s (26.7 %) | 22,060 rows/s (3.9 %) |

**Concurrency: KDS scales with cores, not with connections.** One join (`cross-join`) issued over 1, 2, 4 and 8 connections:

| connections | c1-g now | c8-g now | c1-s now | c1-g v3.1.0 | c8-g v3.1.0 | c1-s v3.1.0 |
|---|---|---|---|---|---|---|
| 1 | 561.3 qps (3.5 %) | 540.6 qps (1.0 %) | 568.9 qps (1.4 %) | 536.1 qps (8.0 %) | 520.1 qps (1.4 %) | 542.7 qps (2.0 %) |
| 2 | 631.0 qps (1.4 %) | 1,020.6 qps (1.1 %) | 629.1 qps (4.5 %) | 582.5 qps (4.7 %) | 970.5 qps (3.4 %) | 596.7 qps (2.3 %) |
| 4 | 634.9 qps (2.3 %) | 1,131.8 qps (41.9 %) | 633.7 qps (6.5 %) | 601.9 qps (3.1 %) | 1,255.7 qps (21.0 %) | 594.2 qps (1.5 %) |
| 8 | 633.6 qps (2.0 %) | 1,588.5 qps (22.0 %) | 636.7 qps (0.9 %) | 599.6 qps (2.6 %) | 1,514.4 qps (35.6 %) | 596.6 qps (1.8 %) |

At `cores = 1` the join does not scale with connections (1.13x at eight): one core's statements run on one thread, so a rising
aggregate is only round-trip overlap. At `cores = 8` it reaches
2.94x at eight connections, with a 22 % to 42 % run-to-run spread at four and eight connections: the sessions are placed on cores as they arrive
(BL-S3) and the eight clients share eight CPUs with the reactors.

Every phase of the driver, median over the three `c1-g` runs, against v3.1.0 (the load phases are single-connection insert loops, the folds are
whole-relation reads):

| phase (cores = 1, group) | ops | now: median qps | spread | v3.1.0 median qps (cross-version) | delta |
|---|---|---|---|---|---|
| load-exchanges | 2 ops | 847.4 qps | 22.3 % | 783.0 qps | +8.2 % |
| load-symbols | 8 ops | 899.1 qps | 58.0 % | 789.8 qps | +13.8 % |
| load-sessions | 7,560 ops | 21,542.0 qps | 16.6 % | 22,947.6 qps | -6.1 % |
| load-bars | 60,480 ops | 19,575.2 qps | 3.4 % | 20,133.8 qps | -2.8 % |
| load-stats | 60,480 ops | 19,921.2 qps | 3.4 % | 20,478.1 qps | -2.7 % |
| load-models | 8 ops | 765.9 qps | 12.2 % | 829.1 qps | -7.6 % |
| backtest-read | 360 ops | 531.7 qps | 1.0 % | 501.9 qps | +5.9 % |
| result-insert | 2,872 ops | 742.1 qps | 1.7 % | 749.1 qps | -0.9 % |
| backtest-replay | 360 ops | 575.4 qps | 3.8 % | 529.5 qps | +8.7 % |
| compare-all | 1 ops | 57.8 qps | 6.7 % | 56.9 qps | +1.6 % |
| compare-one | 32 ops | 603.6 qps | 4.8 % | 562.5 qps | +7.3 % |
| read-bar-lookup | 200 ops | 12,551.5 qps | 22.2 % | 12,761.6 qps | -1.6 % |
| read-bar-range | 200 ops | 669.5 qps | 6.8 % | 666.8 qps | +0.4 % |
| read-symbol-history | 10 ops | 14.2 qps | 5.6 % | 13.9 qps | +2.2 % |
| read-day-slice | 200 ops | 589.5 qps | 3.4 % | 555.8 qps | +6.1 % |
| read-join-point | 200 ops | 12,339.9 qps | 31.4 % | 13,097.7 qps | -5.8 % |
| read-join-exists | 10 ops | 167.8 qps | 1.7 % | 165.8 qps | +1.2 % |
| agg-global | 10 ops | 166.4 qps | 0.5 % | 163.1 qps | +2.0 % |
| agg-by-symbol | 10 ops | 132.4 qps | 1.6 % | 127.7 qps | +3.7 % |
| agg-by-session | 10 ops | 32.7 qps | 3.7 % | 32.0 qps | +2.2 % |
| agg-day-slice | 200 ops | 615.8 qps | 2.0 % | 572.1 qps | +7.6 % |
| agg-distinct | 10 ops | 169.8 qps | 7.4 % | 162.3 qps | +4.6 % |
| sync | 1 ops | 219.8 qps | 30.4 % | 71.7 qps | +206.6 % |

## 4. Percentiles

Every phase of run 1 of each default-size cell, in the driver's own order (the unit is the statement's round trip; `ops` is the count in the
cell). The five percentiles are the driver's; `max` and `errors` are beside them.

#### phases, run 1 of `s1-c1-g` (cores = 1, group)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load-exchanges | 2 ops | 1,230.6 µs | 1,230.6 µs | 1,230.6 µs | 1,593.2 µs | 1,593.2 µs | 1,593.2 µs | 0 errors |
| load-symbols | 8 ops | 1,138.4 µs | 1,141.0 µs | 1,337.5 µs | 10,093.1 µs | 10,093.1 µs | 10,093.1 µs | 0 errors |
| load-sessions | 7,560 ops | 41.8 µs | 43.0 µs | 43.4 µs | 55.1 µs | 65.5 µs | 103.7 µs | 0 errors |
| load-bars | 60,480 ops | 37.0 µs | 44.8 µs | 45.4 µs | 66.4 µs | 80.6 µs | 3,889.3 µs | 0 errors |
| load-stats | 60,480 ops | 38.0 µs | 45.0 µs | 45.5 µs | 64.0 µs | 74.9 µs | 1,424.0 µs | 0 errors |
| load-models | 8 ops | 1,213.1 µs | 1,260.3 µs | 1,376.8 µs | 1,642.2 µs | 1,642.2 µs | 1,642.2 µs | 0 errors |
| backtest-read | 360 ops | 1,811.1 µs | 1,851.2 µs | 1,878.0 µs | 2,033.9 µs | 2,544.9 µs | 2,665.3 µs | 0 errors |
| result-insert | 2,872 ops | 961.0 µs | 1,199.0 µs | 1,271.7 µs | 1,673.3 µs | 2,285.8 µs | 12,217.3 µs | 0 errors |
| backtest-replay | 360 ops | 1,698.3 µs | 1,752.7 µs | 1,786.6 µs | 1,921.1 µs | 1,993.9 µs | 2,043.8 µs | 0 errors |
| compare-all | 1 ops | 17,104.1 µs | 17,104.1 µs | 17,104.1 µs | 17,104.1 µs | 17,104.1 µs | 17,104.1 µs | 0 errors |
| compare-one | 32 ops | 1,610.4 µs | 1,620.7 µs | 1,632.9 µs | 1,777.7 µs | 1,929.0 µs | 1,929.0 µs | 0 errors |
| read-bar-lookup | 200 ops | 73.8 µs | 103.3 µs | 105.4 µs | 127.1 µs | 131.1 µs | 152.5 µs | 0 errors |
| read-bar-range | 200 ops | 1,424.0 µs | 1,446.8 µs | 1,490.0 µs | 1,535.1 µs | 2,188.0 µs | 2,239.7 µs | 0 errors |
| read-symbol-history | 10 ops | 68,707.1 µs | 69,294.9 µs | 69,709.7 µs | 72,947.6 µs | 72,947.6 µs | 72,947.6 µs | 0 errors |
| read-day-slice | 200 ops | 1,693.9 µs | 1,720.3 µs | 1,734.2 µs | 1,767.3 µs | 1,878.5 µs | 2,354.6 µs | 0 errors |
| read-join-point | 200 ops | 108.0 µs | 109.8 µs | 110.7 µs | 134.4 µs | 136.9 µs | 154.8 µs | 0 errors |
| read-join-exists | 10 ops | 5,528.2 µs | 5,921.8 µs | 5,926.7 µs | 5,972.6 µs | 5,972.6 µs | 5,972.6 µs | 0 errors |
| agg-global | 10 ops | 5,944.9 µs | 5,950.5 µs | 5,964.4 µs | 6,207.1 µs | 6,207.1 µs | 6,207.1 µs | 0 errors |
| agg-by-symbol | 10 ops | 7,438.1 µs | 7,445.0 µs | 7,464.4 µs | 7,527.1 µs | 7,527.1 µs | 7,527.1 µs | 0 errors |
| agg-by-session | 10 ops | 29,904.7 µs | 29,956.7 µs | 30,079.2 µs | 33,071.5 µs | 33,071.5 µs | 33,071.5 µs | 0 errors |
| agg-day-slice | 200 ops | 1,621.7 µs | 1,633.0 µs | 1,637.9 µs | 1,673.9 µs | 1,766.9 µs | 2,688.2 µs | 0 errors |
| agg-distinct | 10 ops | 5,742.8 µs | 5,751.2 µs | 5,773.3 µs | 6,059.0 µs | 6,059.0 µs | 6,059.0 µs | 0 errors |
| sync | 1 ops | 6,054.7 µs | 6,054.7 µs | 6,054.7 µs | 6,054.7 µs | 6,054.7 µs | 6,054.7 µs | 0 errors |

#### phases, run 1 of `s1-c8-g` (cores = 8, group)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load-exchanges | 2 ops | 1,092.6 µs | 1,092.6 µs | 1,092.6 µs | 1,222.8 µs | 1,222.8 µs | 1,222.8 µs | 0 errors |
| load-symbols | 8 ops | 1,064.9 µs | 1,088.4 µs | 1,101.8 µs | 1,460.6 µs | 1,460.6 µs | 1,460.6 µs | 0 errors |
| load-sessions | 7,560 ops | 41.6 µs | 43.9 µs | 44.5 µs | 61.5 µs | 72.5 µs | 701.7 µs | 0 errors |
| load-bars | 60,480 ops | 40.8 µs | 46.2 µs | 48.4 µs | 74.3 µs | 89.2 µs | 4,608.5 µs | 0 errors |
| load-stats | 60,480 ops | 41.0 µs | 46.2 µs | 47.2 µs | 69.1 µs | 83.4 µs | 1,991.0 µs | 0 errors |
| load-models | 8 ops | 1,099.8 µs | 1,131.1 µs | 1,176.0 µs | 1,610.6 µs | 1,610.6 µs | 1,610.6 µs | 0 errors |
| backtest-read | 360 ops | 1,912.1 µs | 1,954.2 µs | 1,969.3 µs | 2,161.3 µs | 2,513.8 µs | 2,583.9 µs | 0 errors |
| result-insert | 2,872 ops | 1,002.7 µs | 1,222.1 µs | 1,288.8 µs | 1,689.1 µs | 3,008.7 µs | 15,165.6 µs | 0 errors |
| backtest-replay | 360 ops | 1,792.7 µs | 1,820.9 µs | 1,834.1 µs | 1,885.6 µs | 2,015.4 µs | 2,288.5 µs | 0 errors |
| compare-all | 1 ops | 18,222.1 µs | 18,222.1 µs | 18,222.1 µs | 18,222.1 µs | 18,222.1 µs | 18,222.1 µs | 0 errors |
| compare-one | 32 ops | 1,694.4 µs | 1,704.8 µs | 1,717.0 µs | 1,777.8 µs | 1,802.4 µs | 1,802.4 µs | 0 errors |
| read-bar-lookup | 200 ops | 73.4 µs | 76.0 µs | 78.0 µs | 96.6 µs | 108.8 µs | 129.7 µs | 0 errors |
| read-bar-range | 200 ops | 1,467.5 µs | 1,500.1 µs | 1,516.3 µs | 1,552.6 µs | 1,596.4 µs | 1,934.0 µs | 0 errors |
| read-symbol-history | 10 ops | 70,971.6 µs | 71,341.3 µs | 71,462.0 µs | 74,604.6 µs | 74,604.6 µs | 74,604.6 µs | 0 errors |
| read-day-slice | 200 ops | 1,746.9 µs | 1,780.1 µs | 1,824.2 µs | 1,873.2 µs | 1,930.5 µs | 2,545.6 µs | 0 errors |
| read-join-point | 200 ops | 73.6 µs | 76.4 µs | 77.7 µs | 100.6 µs | 108.4 µs | 126.6 µs | 0 errors |
| read-join-exists | 10 ops | 4,894.5 µs | 4,901.4 µs | 4,918.5 µs | 5,941.4 µs | 5,941.4 µs | 5,941.4 µs | 0 errors |
| agg-global | 10 ops | 6,062.8 µs | 6,078.9 µs | 6,080.1 µs | 6,192.9 µs | 6,192.9 µs | 6,192.9 µs | 0 errors |
| agg-by-symbol | 10 ops | 7,602.5 µs | 7,611.4 µs | 7,613.8 µs | 7,659.8 µs | 7,659.8 µs | 7,659.8 µs | 0 errors |
| agg-by-session | 10 ops | 30,525.1 µs | 30,688.6 µs | 30,856.3 µs | 33,735.5 µs | 33,735.5 µs | 33,735.5 µs | 0 errors |
| agg-day-slice | 200 ops | 1,682.7 µs | 1,706.6 µs | 1,718.4 µs | 1,797.1 µs | 2,005.2 µs | 3,874.3 µs | 0 errors |
| agg-distinct | 10 ops | 5,892.1 µs | 5,912.2 µs | 5,917.1 µs | 6,253.6 µs | 6,253.6 µs | 6,253.6 µs | 0 errors |
| sync | 1 ops | 2,738.1 µs | 2,738.1 µs | 2,738.1 µs | 2,738.1 µs | 2,738.1 µs | 2,738.1 µs | 0 errors |

#### phases, run 1 of `s1-c1-s` (cores = 1, strict)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load-exchanges | 2 ops | 1,149.1 µs | 1,149.1 µs | 1,149.1 µs | 1,418.7 µs | 1,418.7 µs | 1,418.7 µs | 0 errors |
| load-symbols | 8 ops | 1,160.2 µs | 1,185.8 µs | 1,219.6 µs | 1,541.9 µs | 1,541.9 µs | 1,541.9 µs | 0 errors |
| load-sessions | 7,560 ops | 41.7 µs | 43.0 µs | 43.4 µs | 56.1 µs | 65.1 µs | 113.8 µs | 0 errors |
| load-bars | 60,480 ops | 37.0 µs | 45.0 µs | 46.0 µs | 70.3 µs | 87.0 µs | 3,887.0 µs | 0 errors |
| load-stats | 60,480 ops | 37.4 µs | 45.0 µs | 45.5 µs | 64.0 µs | 75.4 µs | 447.9 µs | 0 errors |
| load-models | 8 ops | 1,187.9 µs | 1,194.7 µs | 1,222.8 µs | 1,755.0 µs | 1,755.0 µs | 1,755.0 µs | 0 errors |
| backtest-read | 360 ops | 1,818.4 µs | 1,862.2 µs | 1,887.3 µs | 2,462.0 µs | 2,770.0 µs | 4,041.2 µs | 0 errors |
| result-insert | 2,872 ops | 1,032.1 µs | 1,206.1 µs | 1,267.3 µs | 1,630.7 µs | 3,084.3 µs | 16,940.1 µs | 0 errors |
| backtest-replay | 360 ops | 1,709.1 µs | 1,742.5 µs | 1,761.1 µs | 1,805.8 µs | 2,042.6 µs | 2,467.0 µs | 0 errors |
| compare-all | 1 ops | 17,867.2 µs | 17,867.2 µs | 17,867.2 µs | 17,867.2 µs | 17,867.2 µs | 17,867.2 µs | 0 errors |
| compare-one | 32 ops | 1,651.1 µs | 1,672.6 µs | 1,683.6 µs | 1,782.5 µs | 1,847.2 µs | 1,847.2 µs | 0 errors |
| read-bar-lookup | 200 ops | 73.8 µs | 75.6 µs | 76.6 µs | 98.8 µs | 103.4 µs | 125.0 µs | 0 errors |
| read-bar-range | 200 ops | 1,488.5 µs | 1,508.5 µs | 1,517.5 µs | 1,545.7 µs | 1,615.6 µs | 1,632.9 µs | 0 errors |
| read-symbol-history | 10 ops | 71,318.0 µs | 71,576.9 µs | 71,748.6 µs | 74,743.2 µs | 74,743.2 µs | 74,743.2 µs | 0 errors |
| read-day-slice | 200 ops | 1,644.3 µs | 1,660.4 µs | 1,670.4 µs | 1,719.7 µs | 2,038.9 µs | 2,287.4 µs | 0 errors |
| read-join-point | 200 ops | 73.4 µs | 75.4 µs | 76.0 µs | 92.7 µs | 104.2 µs | 140.1 µs | 0 errors |
| read-join-exists | 10 ops | 5,861.9 µs | 6,146.1 µs | 6,177.8 µs | 6,706.3 µs | 6,706.3 µs | 6,706.3 µs | 0 errors |
| agg-global | 10 ops | 5,900.4 µs | 5,923.4 µs | 5,961.1 µs | 6,081.1 µs | 6,081.1 µs | 6,081.1 µs | 0 errors |
| agg-by-symbol | 10 ops | 7,456.5 µs | 7,463.1 µs | 7,475.1 µs | 7,598.6 µs | 7,598.6 µs | 7,598.6 µs | 0 errors |
| agg-by-session | 10 ops | 30,209.2 µs | 30,395.8 µs | 30,490.1 µs | 33,376.3 µs | 33,376.3 µs | 33,376.3 µs | 0 errors |
| agg-day-slice | 200 ops | 1,572.2 µs | 1,591.1 µs | 1,602.2 µs | 1,670.1 µs | 1,896.2 µs | 2,666.1 µs | 0 errors |
| agg-distinct | 10 ops | 5,708.4 µs | 5,734.5 µs | 5,752.1 µs | 6,070.4 µs | 6,070.4 µs | 6,070.4 µs | 0 errors |
| sync | 1 ops | 2,783.0 µs | 2,783.0 µs | 2,783.0 µs | 2,783.0 µs | 2,783.0 µs | 2,783.0 µs | 0 errors |

## 5. Where the time goes

The matrix is serial reads on one connection, so a statement is a fixed cost plus a per-row cost, and a write is a statement plus the durability wait. What
the cells measure of each, `c1-g`:

| Component | This run | How derived |
|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **102.2 µs** mean, p0 73.8 µs | `read-bar-lookup`, run 1, 200 ops; PostgreSQL's same phase: 70.9 µs mean, p0 65.0 µs |
| Per-row cost of a returned row | **7.0 µs per row** (`read-bar-range`: (1,493.6 - 102.2) µs / 200 rows) and 9.3 µs per row (`read-symbol-history`: (70,055.3 - 102.2) µs / 7,560 rows) | PostgreSQL: 3.3 µs and 4.0 µs per row, after its own 70.9 µs fixed cost (2.1x and 2.3x) |
| Per-row cost of an examined, unreturned row | about 27 ns | `read-day-slice` examines all 60,480 `daily_stats` rows and returns 8 (the ANALYZE table in the stdout of each cell): (mean - fixed) / 60,480 |
| Durability / commit | **1,296 µs per commit** | autocommit insert 1,343 µs per row, a 1,000-row batch 47.6 µs per row; the difference is one durable commit. PostgreSQL's: 1,292 µs |
| Lock or conflict wait | none | one connection (the concurrency sweep reads only) |
| Write statement | 47.6 µs per row at batch 1,000 | the insert path with the sync amortised away |

The fixed round trip is the one place PostgreSQL is cheaper per statement (1.44x of its mean for the same phase, KDS slower by that factor);
the per-row cost of a returned row is about twice PostgreSQL's, which is why every shape that returns many rows sits at 0.43x to 0.56x of it. The
engine is cheap where it examines and discards (a walked unindexed equality beats PostgreSQL's seq scan by 1.4x) and dear where it must build and send
a row: a result row costs 7.0 to 9.3 µs above the fixed round trip against PostgreSQL's 3.3 to 4.0 µs.

## 6. Joins and the plan the engine chose

The engine chooses an inner-join block's order since CB-S2 and prices it from the join planner's statistics since CC. Scenario 1 has three join
statements: `backtest-read` (three relations), `compare-all` and `compare-one` / `model-join` (two). ANALYZE of each, on a server in this run's
state (`pl-s1-planned-r1`, the full reply is in `bench/v3.3.0/archive/scenario1-v3.3.0-33-g9682b725/plans/`):

| statement | plan (default `join_order = planned`) | `reordered` | examined | with `join_order = written` | examined |
|---|---|---|---|---|---|
| `backtest-read` (`cross-join`) | `FilterScan daily_stats` -> `Probe daily_bars` -> `Probe symbols` (order = memo, est 8 / 8 / 8) | no | 60,496 rows | same chain | 60,496 rows |
| `compare-all` | `Scan models` (8 rows) -> `Scan model_results`, **`build on=col1`, `inner_built=1`, `build_rows=2,872`, `build_probes=7`** | **yes** (`reordered=1`, switch 0.19) | 5,393 rows | `Scan model_results` -> `Probe models` per row (2,872 opens, 2,898 pages) | 5,744 rows |
| `compare-one` (`model-join`) | `Lookup models key=1` -> `FilterScan model_results` (est 1 / 362) | **yes** (`reordered=1`, switch 0.84) | 2,873 rows | `FilterScan model_results` -> `Probe models` (359 opens, `memo_hits=358`) | 3,231 rows |

The planner does what it was built to do on the two joins whose written order is the dear one, and **it moves their throughput by 1 % to 6 %, which is inside the noise**.
Three runs each of the default and of the `join_order = written` arm (same binary, same cell shape, same window):

| measure | planned | spread | written | spread | planned over written |
|---|---|---|---|---|---|
| phase backtest-read | 531.0 qps | 1.5 % | 536.2 qps | 1.5 % | 0.99x |
| phase backtest-replay | 567.0 qps | 1.6 % | 568.3 qps | 1.4 % | 1.00x |
| phase compare-all | 57.7 qps | 0.9 % | 57.0 qps | 2.3 % | 1.01x |
| phase compare-one | 609.9 qps | 2.7 % | 573.3 qps | 3.6 % | 1.06x |
| sweep cross-join cold | 556.4 qps | 2.1 % | 580.2 qps | 3.1 % | 0.96x |
| sweep cross-join warm | 567.4 qps | 2.0 % | 576.2 qps | 3.3 % | 0.98x |
| sweep cross-join cabin | 4,845.6 qps | 23.7 % | 5,667.5 qps | 20.1 % | 0.85x |
| sweep point-join cold | 8,778.5 qps | 48.2 % | 12,626.9 qps | 31.3 % | 0.70x |
| sweep point-join warm | 8,787.9 qps | 45.2 % | 12,869.6 qps | 32.5 % | 0.68x |
| sweep model-join cold | 586.0 qps | 4.1 % | 577.1 qps | 4.3 % | 1.02x |
| sweep model-join warm | 594.4 qps | 3.9 % | 582.9 qps | 4.5 % | 1.02x |
| sweep model-join cabin | 594.9 qps | 3.3 % | 585.3 qps | 3.1 % | 1.02x |


`compare-one` is the only phase that reads above its spreads (2.7 % and 3.6 %), by 6 % (1.06x), and the `model-join` sweep row
moves 2 % with spreads of 4 %. The join is a 359-row result out of 2,872 examined rows; its time is the walk of `model_results` (26 pages) and the
round trip, not the join order. The `point-join` rows (0.68x to 0.70x for the planned arm) are the bimodal row of section 3 (spreads up to 31 % there) in a different mood
in each arm (spreads 45 % to 48 % planned, 31 % to 33 % written), not a finding. **Why `model-join` stays at 0.56x of PostgreSQL**: both engines examine the 2,872 rows of
`model_results` for one `model_id`; PostgreSQL's 1,092 qps against KDS's 615 qps is the per-row result cost of section 5 (359 rows returned),
not a plan difference: the KDS plan is the one the planner picked, and the alternative order is the same speed within 6 %.

## 7. The size sweep

`c1-g`, 252 / 1,008 / 10,080 bars (60,480 is section 3), three runs each; v3.1.0 columns are its sweep, which had two runs at 252 and 1,008 bars and one at
10,080 (cross-version, fewer runs, so a delta of 10 % or less is not read):

| bars | cell | runs | shape | cold | warm | cabin | spread of warm | v3.1.0 warm (runs) | warm delta |
|---|---|---|---|---|---|---|---|---|---|
| 252 bars | s1s-n252 | 3 runs | bar-lookup | 12,392.2 qps | 13,184.7 qps | - | 3.2 % | 12,948.0 qps (2 runs) | +1.8 % |
| 252 bars | s1s-n252 | 3 runs | bar-range | 679.4 qps | 680.9 qps | - | 2.3 % | 687.2 qps (2 runs) | -0.9 % |
| 252 bars | s1s-n252 | 3 runs | day-slice | 10,855.5 qps | 10,915.6 qps | 11,684.4 qps | 3.7 % | 11,078.8 qps (2 runs) | -1.5 % |
| 252 bars | s1s-n252 | 3 runs | symbol-history | 415.5 qps | 431.0 qps | 418.3 qps | 0.8 % | 431.9 qps (2 runs) | -0.2 % |
| 252 bars | s1s-n252 | 3 runs | cross-join | 7,000.7 qps | 8,601.1 qps | 8,485.8 qps | 25.0 % | 8,794.8 qps (2 runs) | -2.2 % |
| 252 bars | s1s-n252 | 3 runs | point-join | 12,034.5 qps | 12,630.9 qps | - | 3.0 % | 11,211.6 qps (2 runs) | +12.7 % |
| 252 bars | s1s-n252 | 3 runs | model-join | 6,506.3 qps | 7,288.3 qps | 6,860.1 qps | 2.9 % | 6,908.6 qps (2 runs) | +5.5 % |
| 1,008 bars | s1s-n1008 | 3 runs | bar-lookup | 12,659.0 qps | 12,862.6 qps | - | 1.3 % | 13,374.6 qps (2 runs) | -3.8 % |
| 1,008 bars | s1s-n1008 | 3 runs | bar-range | 673.6 qps | 664.4 qps | - | 4.2 % | 695.4 qps (2 runs) | -4.5 % |
| 1,008 bars | s1s-n1008 | 3 runs | day-slice | 7,091.0 qps | 7,140.7 qps | 8,708.1 qps | 0.8 % | 7,098.4 qps (2 runs) | +0.6 % |
| 1,008 bars | s1s-n1008 | 3 runs | symbol-history | 406.1 qps | 420.1 qps | 412.6 qps | 2.8 % | 425.8 qps (2 runs) | -1.3 % |
| 1,008 bars | s1s-n1008 | 3 runs | cross-join | 5,306.8 qps | 6,004.4 qps | 7,208.3 qps | 2.7 % | 6,156.2 qps (2 runs) | -2.5 % |
| 1,008 bars | s1s-n1008 | 3 runs | point-join | 12,452.2 qps | 12,918.0 qps | - | 0.8 % | 12,952.5 qps (2 runs) | -0.3 % |
| 1,008 bars | s1s-n1008 | 3 runs | model-join | 6,574.8 qps | 7,323.9 qps | 7,347.7 qps | 8.3 % | 7,721.4 qps (2 runs) | -5.1 % |
| 10,080 bars | s1s-n10080 | 3 runs | bar-lookup | 12,917.0 qps | 12,970.4 qps | - | 2.5 % | 9,620.0 qps (1 runs) | +34.8 % |
| 10,080 bars | s1s-n10080 | 3 runs | bar-range | 669.3 qps | 685.8 qps | - | 1.9 % | 634.7 qps (1 runs) | +8.1 % |
| 10,080 bars | s1s-n10080 | 3 runs | day-slice | 2,420.3 qps | 2,432.1 qps | 6,550.4 qps | 7.9 % | 2,211.1 qps (1 runs) | +10.0 % |
| 10,080 bars | s1s-n10080 | 3 runs | symbol-history | 85.8 qps | 84.9 qps | 85.5 qps | 1.5 % | 85.8 qps (1 runs) | -1.0 % |
| 10,080 bars | s1s-n10080 | 3 runs | cross-join | 2,144.3 qps | 2,252.0 qps | 5,561.3 qps | 2.0 % | 2,101.4 qps (1 runs) | +7.2 % |
| 10,080 bars | s1s-n10080 | 3 runs | point-join | 12,547.7 qps | 12,657.7 qps | - | 13.9 % | 13,419.2 qps (1 runs) | -5.7 % |
| 10,080 bars | s1s-n10080 | 3 runs | model-join | 2,779.9 qps | 2,900.2 qps | 2,896.0 qps | 10.3 % | 2,815.5 qps (1 runs) | +3.0 % |

**Fixed costs and per-row costs separate cleanly.** The pk shapes are flat: `bar-lookup` 12,863 to 13,185 qps across 252, 1,008 and 10,080 bars (11,264 qps at 60,480 with a 13 % spread) and `point-join`
12,631 to 12,918 qps across all four sizes, as a descent should be. The scan shapes scale with rows: `day-slice` goes 10,916 -> 7,141 -> 2,432 -> 591 qps from
252 to 60,480 bars, and the Cabin makes it **flat** (11,684 -> 8,708 -> 6,550 -> 6,558 qps): the Cabin's lift is 1.1x at 252 bars and 11.1x at 60,480,
so a Cabin on a relation that fits in one or two pages buys nothing (the walk is already a round trip).

## 8. Against PostgreSQL 18.6 (fresh, same window)

PostgreSQL is the floor: it says whether the engine is in the right league and nothing finer. The twin ran three times at the driver's defaults
(`pg-s1`, `r2-pg-s1`, `r3-pg-s1`; the twelve PostgreSQL cells ran 04:25:05 to 04:33:44 UTC, after every KDS cell of the matrix and sweep and never concurrent with one) and three times at each sweep size.
PG's `index built` column is a btree index; KDS's Cabin is the counterpart, and they are different structures. Ratios are KDS `c1-g` (median of three)
over PostgreSQL (median of three), above 1.00x = KDS faster.

| shape | KDS cold | PG cold | KDS/PG cold | KDS warm | PG warm | KDS/PG warm | KDS Cabin | PG index built | KDS/PG indexed-or-cabin |
|---|---|---|---|---|---|---|---|---|---|
| bar-lookup | 11,982.6 qps | 14,177.0 qps | 0.85x | 11,263.5 qps | 14,673.3 qps | 0.77x | - | - | - |
| bar-range | 684.1 qps | 1,370.1 qps | 0.50x | 684.4 qps | 1,399.8 qps | 0.49x | - | - | - |
| day-slice | 588.9 qps | 418.6 qps | 1.41x | 591.3 qps | 413.5 qps | 1.43x | 6,557.7 qps | 8,722.0 qps | 0.75x |
| symbol-history | 14.3 qps | 33.0 qps | 0.43x | 13.8 qps | 32.3 qps | 0.43x | 13.6 qps | 33.8 qps | 0.40x |
| cross-join | 563.7 qps | 390.7 qps | 1.44x | 575.8 qps | 393.1 qps | 1.46x | 5,570.3 qps | 4,594.5 qps | 1.21x |
| point-join | 12,444.5 qps | 7,009.7 qps | 1.78x | 12,674.6 qps | 7,071.3 qps | 1.79x | - | - | - |
| model-join | 605.9 qps | 1,078.6 qps | 0.56x | 614.8 qps | 1,092.2 qps | 0.56x | 618.0 qps | 1,124.4 qps | 0.55x |

| phase (median over runs) | KDS `c1-g` | PG18 | KDS/PG |
|---|---|---|---|
| load-exchanges | 847.4 qps | 700.0 qps | 1.21x |
| load-symbols | 899.1 qps | 806.7 qps | 1.11x |
| load-sessions | 21,542.0 qps | 13,292.7 qps | 1.62x |
| load-bars | 19,575.2 qps | 13,336.9 qps | 1.47x |
| load-stats | 19,921.2 qps | 12,948.5 qps | 1.54x |
| load-models | 765.9 qps | 797.3 qps | 0.96x |
| backtest-read | 531.7 qps | 367.8 qps | 1.45x |
| result-insert | 742.1 qps | 779.8 qps | 0.95x |
| backtest-replay | 575.4 qps | 387.8 qps | 1.48x |
| compare-all | 57.8 qps | 120.0 qps | 0.48x |
| compare-one | 603.6 qps | 1,091.5 qps | 0.55x |
| read-bar-lookup | 12,551.5 qps | 14,120.8 qps | 0.89x |
| read-bar-range | 669.5 qps | 1,383.6 qps | 0.48x |
| read-symbol-history | 14.2 qps | 32.7 qps | 0.43x |
| read-day-slice | 589.5 qps | 415.1 qps | 1.42x |
| read-join-point | 12,339.9 qps | 6,924.8 qps | 1.78x |
| read-join-exists | 167.8 qps | 123.9 qps | 1.35x |
| agg-global | 166.4 qps | 244.4 qps | 0.68x |
| agg-by-symbol | 132.4 qps | 145.5 qps | 0.91x |
| agg-by-session | 32.7 qps | 50.1 qps | 0.65x |
| agg-day-slice | 615.8 qps | 413.4 qps | 1.49x |
| agg-distinct | 169.8 qps | 244.6 qps | 0.69x |

| rows per BEGIN/COMMIT | KDS c1-g | PG18 | KDS/PG |
|---|---|---|---|
| 1 | 744 rows/s | 739 rows/s | 1.01x |
| 10 | 5,551 rows/s | 5,248 rows/s | 1.06x |
| 100 | 15,434 rows/s | 12,149 rows/s | 1.27x |
| 1000 | 21,023 rows/s | 16,505 rows/s | 1.27x |

| connections | KDS c1-g | KDS c8-g | PG18 | KDS c1-g/PG | KDS c8-g/PG |
|---|---|---|---|---|---|
| 1 | 561.3 qps | 540.6 qps | 456.4 qps | 1.23x | 1.18x |
| 2 | 631.0 qps | 1,020.6 qps | 870.2 qps | 0.73x | 1.17x |
| 4 | 634.9 qps | 1,131.8 qps | 892.5 qps | 0.71x | 1.27x |
| 8 | 633.6 qps | 1,588.5 qps | 874.6 qps | 0.72x | 1.82x |

Ratios across the sweep sizes, KDS `c1-g` warm over PostgreSQL warm (and indexed):

| bars | shape | KDS warm | PG warm | KDS/PG warm | KDS Cabin | PG index | KDS/PG indexed |
|---|---|---|---|---|---|---|---|
| 252 | bar-lookup | 13,184.7 qps | 9,989.7 qps | 1.32x | - | - | - |
| 252 | bar-range | 680.9 qps | 1,318.8 qps | 0.52x | - | - | - |
| 252 | day-slice | 10,915.6 qps | 13,721.8 qps | 0.80x | 11,684.4 qps | 12,191.6 qps | 0.96x |
| 252 | symbol-history | 431.0 qps | 927.4 qps | 0.46x | 418.3 qps | 958.5 qps | 0.44x |
| 252 | cross-join | 8,601.1 qps | 5,666.7 qps | 1.52x | 8,485.8 qps | 4,765.0 qps | 1.78x |
| 252 | point-join | 12,630.9 qps | 6,353.6 qps | 1.99x | - | - | - |
| 252 | model-join | 7,288.3 qps | 6,596.8 qps | 1.10x | 6,860.1 qps | 6,855.1 qps | 1.00x |
| 1,008 | bar-lookup | 12,862.6 qps | 11,445.5 qps | 1.12x | - | - | - |
| 1,008 | bar-range | 664.4 qps | 1,278.9 qps | 0.52x | - | - | - |
| 1,008 | day-slice | 7,140.7 qps | 7,652.4 qps | 0.93x | 8,708.1 qps | 9,010.9 qps | 0.97x |
| 1,008 | symbol-history | 420.1 qps | 879.8 qps | 0.48x | 412.6 qps | 901.7 qps | 0.46x |
| 1,008 | cross-join | 6,004.4 qps | 3,398.9 qps | 1.77x | 7,208.3 qps | 3,745.8 qps | 1.92x |
| 1,008 | point-join | 12,918.0 qps | 7,119.6 qps | 1.81x | - | - | - |
| 1,008 | model-join | 7,323.9 qps | 8,067.8 qps | 0.91x | 7,347.7 qps | 8,313.6 qps | 0.88x |
| 10,080 | bar-lookup | 12,970.4 qps | 14,170.7 qps | 0.92x | - | - | - |
| 10,080 | bar-range | 685.8 qps | 1,355.2 qps | 0.51x | - | - | - |
| 10,080 | day-slice | 2,432.1 qps | 2,058.3 qps | 1.18x | 6,550.4 qps | 8,742.6 qps | 0.75x |
| 10,080 | symbol-history | 84.9 qps | 194.7 qps | 0.44x | 85.5 qps | 204.8 qps | 0.42x |
| 10,080 | cross-join | 2,252.0 qps | 1,681.7 qps | 1.34x | 5,561.3 qps | 4,569.8 qps | 1.22x |
| 10,080 | point-join | 12,657.7 qps | 7,010.1 qps | 1.81x | - | - | - |
| 10,080 | model-join | 2,900.2 qps | 4,189.7 qps | 0.69x | 2,896.0 qps | 4,227.3 qps | 0.69x |

**The league reading.** KDS is in the same league on pk reads (0.77x to 1.32x of PG, within the round-trip gap), ahead on the unindexed equality scan at 60,480 bars
and on both joins that start from a pk or a scan with probes (1.3x to 2.0x at every size), and **behind by half on result-heavy shapes at every size**
(`bar-range` 0.51x to 0.52x at 252, 1,008 and 10,080 bars, `symbol-history` 0.42x to 0.48x): that ratio does not change with the row count, which marks it as a per-row,
not a fixed, cost. At 252 bars `day-slice` is 0.80x; the pk round trip is not the reason (`bar-lookup` at 252 bars is 1.32x, KDS faster), and this run does not separate what is.

## 9. Delta against the previous run of this shape (cross-version)

The comparator is v3.1.0's BTREE-copy cells (section 3 carries every shape; section 3's last table every phase). 240 commits apart, so nothing here is attributable to one change.
Read against each side's spread:

| measure | now | v3.1.0 | delta | read |
|---|---|---|---|---|
| `c1-g` `day-slice` warm | 591.3 qps | 550.9 qps | +7.3 % | just above the larger spread (6.9 % now, 6.5 % then): a finding by section 3's rule, at its edge |
| `c1-s` `day-slice` warm | 599.1 qps | 550.1 qps | +8.9 % | **a finding**: spreads 1.0 % and 1.8 % |
| `c1-g` `bar-lookup` warm | 11,263.5 qps | 13,152.9 qps | -14.4 % | **noise**: bimodal, six-run spread 28.6 %; fast-mode runs -3.8 % against a 3.7 % spread (section 3, re-run) |
| `c1-g` `cross-join` warm | 575.8 qps | 549.2 qps | +4.8 % | inside the spread (9.5 % at v3.1.0) |
| `c1-g` `model-join` warm | 614.8 qps | 569.5 qps | +8.0 % | above both spreads (6.5 % now, 3.8 % then), so a finding by section 3's rule; the reordered plan of section 6 accounts for 2 % of it at most |
| `c8-g` `day-slice` warm | 554.4 qps | 430.0 qps | +28.9 % | **a finding**: spreads 2.3 % and 0.5 % |
| `c8-g` `cross-join` warm | 548.1 qps | 427.6 qps | +28.2 % | **a finding**: spreads 1.2 % and 0.6 % |
| Cabin lifts (`day-slice` / `cross-join`, `c1-g`) | 11.1x / 9.7x | 11.9x / 10.5x | - | the Cabin arms moved -0.2 % and -3.8 % (inside spread) |
| autocommit insert | 744 rows/s | 773 rows/s | -3.7 % | inside the spread (26 % then) |

At v3.1.0 a one-connection read on an eight-core instance ran 22 % slower than on a one-core instance (430 against 551 qps for `day-slice`); in this run the same pair is
554 against 591 qps (6 % slower). Most of the +28.9 % is therefore the eight-core penalty shrinking; the rest is a one-core gain of 7 % to 9 % on the
scan shapes (+7.3 % `c1-g`, at the edge of its spreads; +8.9 % `c1-s`, spreads 1.0 % and 1.8 %). This run does not say which commits did either. Every rate of the phases the run shares with v3.1.0 (section 3, last table)
moved between -6.1 % and +8.7 % except the phases of 1 to 8 operations (`sync`, `load-exchanges`, `load-symbols`, `load-models`), whose spreads are 12 % to 58 %.

## 10. Noise floor, contamination and host

The noise floor is the 3-run spread of one cell: 6.9 % for `day-slice`, 1.8 % for `cross-join`, 6.5 % for `model-join`,
13.0 % for `bar-lookup` over its first three runs and 28.6 % over six (section 3). Two shapes are bimodal between runs (the sweep lands in a fast or a slow mode): `point-join`
(spread 1 % to 48 % depending on the cell, section 3 and section 6) and `bar-lookup` (28.6 % at `c1-g`, 28.5 % under `strict`). Medians of three damp it and the table shows the spread beside it; no delta is
claimed on them.

Contamination: none in this scenario's cells (summary, "Contamination"); the validity verdict of every cell is in the host table below.

Per-cell host stamps (load average before the cell, gate wait, and what the 1-second monitor saw inside the window):

| cell | start (UTC) | end (UTC) | load average before (1 / 5 / 15 min) | gate wait | build or test process | foreign samples >= 10 % CPU | exit |
|---|---|---|---|---|---|---|---|
| `s1-c1-g` | 03:37:33 | 03:38:23 | 1.48 / 1.41 / 2.74 | 0 s | none | 0 of 24 samples | exit 0 |
| `s1-c8-g` | 03:38:30 | 03:39:21 | 1.31 / 1.40 / 2.66 | 0 s | none | 0 of 25 samples | exit 0 |
| `s1-c1-s` | 03:39:28 | 03:40:18 | 1.19 / 1.37 / 2.57 | 0 s | none | 1 of 25 samples | exit 0 |
| `r2-s1-c1-g` | 03:40:26 | 03:41:17 | 1.17 / 1.36 / 2.49 | 0 s | none | 0 of 26 samples | exit 0 |
| `r2-s1-c8-g` | 03:41:24 | 03:42:15 | 1.16 / 1.32 / 2.41 | 0 s | none | 0 of 26 samples | exit 0 |
| `r2-s1-c1-s` | 03:42:22 | 03:43:11 | 1.20 / 1.31 / 2.34 | 0 s | none | 0 of 25 samples | exit 0 |
| `r3-s1-c1-g` | 03:43:18 | 03:44:09 | 1.00 / 1.24 / 2.25 | 0 s | none | 0 of 25 samples | exit 0 |
| `r3-s1-c8-g` | 03:44:16 | 03:45:06 | 0.99 / 1.21 / 2.18 | 0 s | none | 0 of 24 samples | exit 0 |
| `r3-s1-c1-s` | 03:45:13 | 03:46:03 | 1.03 / 1.18 / 2.11 | 0 s | none | 0 of 25 samples | exit 0 |
| `s1s-n252-c1-g` | 04:18:37 | 04:18:44 | 0.21 / 0.41 / 0.79 | 0 s | none | 0 of 5 samples | exit 0 |
| `s1s-n1008-c1-g` | 04:18:51 | 04:18:57 | 0.30 / 0.43 / 0.79 | 0 s | none | 2 of 5 samples | exit 0 |
| `s1s-n10080-c1-g` | 04:19:04 | 04:19:16 | 0.30 / 0.42 / 0.78 | 0 s | none | 1 of 8 samples | exit 0 |
| `r2-s1s-n252-c1-g` | 04:19:23 | 04:19:29 | 0.38 / 0.43 / 0.78 | 0 s | none | 1 of 5 samples | exit 0 |
| `r2-s1s-n1008-c1-g` | 04:19:37 | 04:19:43 | 0.50 / 0.46 / 0.79 | 0 s | none | 0 of 5 samples | exit 0 |
| `r2-s1s-n10080-c1-g` | 04:19:50 | 04:20:02 | 0.53 / 0.47 / 0.78 | 0 s | none | 0 of 7 samples | exit 0 |
| `r3-s1s-n252-c1-g` | 04:20:09 | 04:20:15 | 0.71 / 0.52 / 0.80 | 0 s | none | 1 of 5 samples | exit 0 |
| `r3-s1s-n1008-c1-g` | 04:20:22 | 04:20:28 | 0.68 / 0.52 / 0.79 | 0 s | none | 0 of 5 samples | exit 0 |
| `r3-s1s-n10080-c1-g` | 04:20:35 | 04:20:48 | 0.59 / 0.51 / 0.79 | 0 s | none | 0 of 8 samples | exit 0 |
| `pg-s1` | 04:25:08 | 04:25:48 | 0.76 / 0.68 / 0.80 | 0 s | none | 0 of 20 samples | exit 0 |
| `pg-s1s-n252` | 04:25:55 | 04:26:00 | 0.97 / 0.75 / 0.82 | 0 s | none | 0 of 4 samples | exit 0 |
| `pg-s1s-n1008` | 04:26:07 | 04:26:12 | 0.89 / 0.74 / 0.82 | 0 s | none | 0 of 4 samples | exit 0 |
| `pg-s1s-n10080` | 04:26:19 | 04:26:29 | 0.83 / 0.73 / 0.81 | 0 s | none | 0 of 7 samples | exit 0 |
| `r2-pg-s1` | 04:28:48 | 04:29:26 | 0.87 / 0.78 / 0.82 | 0 s | none | 1 of 20 samples | exit 0 |
| `r2-pg-s1s-n252` | 04:29:33 | 04:29:38 | 0.86 / 0.79 / 0.82 | 0 s | none | 1 of 4 samples | exit 0 |
| `r2-pg-s1s-n1008` | 04:29:45 | 04:29:50 | 1.03 / 0.84 / 0.84 | 0 s | none | 0 of 4 samples | exit 0 |
| `r2-pg-s1s-n10080` | 04:29:57 | 04:30:07 | 1.02 / 0.84 / 0.84 | 0 s | none | 0 of 7 samples | exit 0 |
| `r3-pg-s1` | 04:32:24 | 04:33:04 | 1.01 / 0.95 / 0.88 | 0 s | none | 1 of 20 samples | exit 0 |
| `r3-pg-s1s-n252` | 04:33:11 | 04:33:15 | 0.96 / 0.96 / 0.89 | 0 s | none | 0 of 4 samples | exit 0 |
| `r3-pg-s1s-n1008` | 04:33:22 | 04:33:27 | 0.89 / 0.94 / 0.89 | 0 s | none | 0 of 5 samples | exit 0 |
| `r3-pg-s1s-n10080` | 04:33:35 | 04:33:44 | 0.90 / 0.94 / 0.89 | 0 s | none | 0 of 6 samples | exit 0 |
| `pl-s1-planned-r1` | 04:34:40 | 04:35:30 | 0.65 / 0.86 / 0.87 | 0 s | none | 0 of 24 samples | exit 0 |
| `pl-s1-written-r1` | 04:36:10 | 04:37:00 | 0.96 / 0.95 / 0.90 | 0 s | none | 0 of 25 samples | exit 0 |
| `pl-s1-planned-r2` | 04:37:40 | 04:38:30 | 1.03 / 0.98 / 0.92 | 0 s | none | 0 of 25 samples | exit 0 |
| `pl-s1-written-r2` | 04:39:09 | 04:40:00 | 0.84 / 0.95 / 0.92 | 0 s | none | 0 of 26 samples | exit 0 |
| `pl-s1-planned-r3` | 04:40:40 | 04:41:29 | 0.92 / 0.98 / 0.93 | 0 s | none | 0 of 24 samples | exit 0 |
| `pl-s1-written-r3` | 04:42:09 | 04:42:59 | 1.14 / 1.05 / 0.97 | 0 s | none | 0 of 25 samples | exit 0 |

## 11. What the run says about the engine

1. **A heap-by-design append loses nothing to a btree here.** The driver's heap-shaped tables are hidden-key btrees now, the hidden key is issued in order, so every insert is a tail append: `result-insert` runs 742.1 qps against 749 qps for the BTREE copy at v3.1.0 (`heap-and-tuple.md` §3.1b).
2. **Where KDS loses to PostgreSQL is the row, not the plan.** A returned row costs 7.0 to 9.3 µs above the fixed round trip against 3.3 to 4.0 µs (2.1x to 2.3x), which puts the cost in result delivery (tuple decode, projection, KWP row-batch encoding) rather than in the examined-row walk. The client is `tools/kwp.py` for KDS and `tools/pg_wire.py` for PostgreSQL, each a Python
   decoder, so whether the factor of two lives in the engine or in the two decoders is **not separated by this run**.
3. **The join planner has nothing to win here** (section 6): `compare-all` reads 14x fewer pages after the reorder and runs at the same 57.7 qps, because its 17 ms is 2,872 returned rows at about 6 µs each.

## 12. Archive

`bench/v3.3.0/archive/scenario1-v3.3.0-33-g9682b725/`: per-cell JSON (KDS and PostgreSQL), the driver stdout of each cell, the server configs, the join-order probe's ANALYZE replies
(`plans/`). What the four scenarios share is in `bench/v3.3.0/archive/run-v3.3.0-33-g9682b725/`: the run scripts (`lib.sh`, `run_all.sh`, `run_plan.sh`), the timeline, the monitor and
watchdog logs, the binary hash, and the table generators under `analysis/`. No data file is kept.
