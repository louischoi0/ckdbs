# Scenario 1 (backtest) at `v3.5.0`

**One line.** At the tag `v3.5.0` (`07f4552a`) scenario 1 is where `v3.4.14` left it: every warm shape of the main cell is within its spread of the `v3.4.14` run on the same host (-1.6 % to +4.2 %), and the same-host interleaved A/B against the `v3.4.14` binary reads 0.98x to 1.02x on every shape and phase it can resolve. Nothing in DI, DH, DJ or the two audits' fixes has a price scenario 1 can read. Verification passed 18 of 18 runs.

Driver: `tools/scenario1_backtest.py` of the tag, unmodified, with `--seed 1 --bars-clustered btree --verify --sync --analyze`. Measured directly by CLA from the scheduled session (no `ck-tester` agent was spawned; the tester's rules in `.claude/agents/ck-tester.md` were followed through the run scripts). Comparator: `results-scenario1-backtest-v3.4.14.md` (2026-11-19, **this host**, the `v3.4.14` binary).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-20; main cells 18:03:19 to 18:09:53 UTC; A/B cells 18:28 to 18:36 UTC |
| Version, commit, binary, host, device, budget | stamped once in `summary-scenarios-v3.5.0.md`, section Stamp |
| Server config | the common config; `cores` 1 or 8, `durability` group or strict per cell; port 15610 |
| Driver | exit 0 in every cell; `verify_problems` empty in 18 of 18 runs |

## 2. What was run

The `v3.4.14` run's cells, unchanged: three cells (`c1-g`, `c8-g`, `c1-s`: cores and durability) three times each, the size sweep (252, 1,008 and 10,080 bars at `c1-g`) three times each, and the engine A/B (`c1-g`, three interleaved pairs). Cut: the join-order probe (as at `v3.4.14`) and PostgreSQL (no server on this host).

## 3. Results

Warm and cold qps from the driver's sweep, median of three runs; spread = (max - min) / median; a delta smaller than the larger spread is not a finding, and a cell above 20 % carries no claim.

#### cell c1-g: cores = 1, group

| shape | cold now | warm now | spread | warm v3.4.14 | spread | delta | read | cabin now | cabin v3.4.14 |
|---|---|---|---|---|---|---|---|---|---|
| bar-lookup | 13,027.8 qps | 13,523.8 qps | 4.2 % | 13,605.2 qps | 1.8 % | -0.6 % | inside the spread | - | - |
| bar-range | 1,125.2 qps | 1,160.4 qps | 4.9 % | 1,114.0 qps | 2.9 % | +4.2 % | inside the spread | - | - |
| day-slice | 666.2 qps | 677.6 qps | 2.6 % | 668.7 qps | 3.9 % | +1.3 % | inside the spread | 7,558.8 qps | 7,811.8 qps |
| symbol-history | 27.1 qps | 27.1 qps | 1.1 % | 27.1 qps | 1.5 % | +0.0 % | inside the spread | 26.5 qps | 26.5 qps |
| cross-join | 635.4 qps | 640.7 qps | 5.4 % | 650.8 qps | 7.3 % | -1.6 % | inside the spread | 6,316.1 qps | 6,301.8 qps |
| point-join | 12,805.6 qps | 13,782.7 qps | 24.7 % | 13,692.0 qps | 7.0 % | +0.7 % | spread > 20 %, no claim | - | - |
| model-join | 874.9 qps | 898.4 qps | 5.5 % | 905.4 qps | 4.9 % | -0.8 % | inside the spread | 897.7 qps | 897.6 qps |

#### cell c8-g: cores = 8, group

| shape | cold now | warm now | spread | warm v3.4.14 | spread | delta | read | cabin now | cabin v3.4.14 |
|---|---|---|---|---|---|---|---|---|---|
| bar-lookup | 12,203.3 qps | 13,019.7 qps | 3.4 % | 13,400.3 qps | 6.0 % | -2.8 % | inside the spread | - | - |
| bar-range | 1,054.0 qps | 1,074.7 qps | 4.4 % | 1,113.4 qps | 2.8 % | -3.5 % | inside the spread | - | - |
| day-slice | 601.9 qps | 606.2 qps | 3.9 % | 623.6 qps | 2.2 % | -2.8 % | inside the spread | 7,287.9 qps | 7,578.1 qps |
| symbol-history | 26.9 qps | 26.7 qps | 0.7 % | 26.8 qps | 0.7 % | -0.4 % | inside the spread | 26.0 qps | 26.0 qps |
| cross-join | 588.6 qps | 588.0 qps | 5.2 % | 593.9 qps | 1.3 % | -1.0 % | inside the spread | 5,966.0 qps | 6,270.1 qps |
| point-join | 13,029.8 qps | 13,678.4 qps | 5.2 % | 13,514.6 qps | 6.9 % | +1.2 % | inside the spread | - | - |
| model-join | 824.6 qps | 839.5 qps | 3.0 % | 833.0 qps | 5.4 % | +0.8 % | inside the spread | 817.4 qps | 819.9 qps |

#### cell c1-s: cores = 1, strict

| shape | cold now | warm now | spread | warm v3.4.14 | spread | delta | read | cabin now | cabin v3.4.14 |
|---|---|---|---|---|---|---|---|---|---|
| bar-lookup | 13,160.1 qps | 13,119.0 qps | 38.4 % | 13,169.9 qps | 5.7 % | -0.4 % | spread > 20 %, no claim | - | - |
| bar-range | 1,122.9 qps | 1,144.2 qps | 3.5 % | 1,142.3 qps | 4.9 % | +0.2 % | inside the spread | - | - |
| day-slice | 660.5 qps | 649.3 qps | 2.9 % | 671.5 qps | 4.4 % | -3.3 % | inside the spread | 7,651.6 qps | 8,064.8 qps |
| symbol-history | 27.0 qps | 27.2 qps | 1.1 % | 27.4 qps | 2.9 % | -0.7 % | inside the spread | 26.3 qps | 26.4 qps |
| cross-join | 639.4 qps | 650.4 qps | 2.5 % | 663.4 qps | 5.2 % | -2.0 % | inside the spread | 6,751.8 qps | 6,533.5 qps |
| point-join | 13,186.8 qps | 14,128.8 qps | 8.4 % | 13,737.7 qps | 7.7 % | +2.8 % | inside the spread | - | - |
| model-join | 855.3 qps | 918.8 qps | 2.8 % | 905.1 qps | 3.1 % | +1.5 % | inside the spread | 896.2 qps | 914.6 qps |

#### phases of `s1-c1-g`

| phase | ops | now | spread | v3.4.14 | spread | delta | read |
|---|---|---|---|---|---|---|---|
| load-exchanges | 2 | 1,651.6 qps | 21.3 % | 1,858.9 qps | 9.0 % | -11.2 % | spread > 20 %, no claim |
| load-symbols | 8 | 2,010.0 qps | 22.2 % | 2,153.4 qps | 13.9 % | -6.7 % | spread > 20 %, no claim |
| load-sessions | 7,560 | 21,771.0 qps | 12.1 % | 23,826.3 qps | 5.9 % | -8.6 % | inside the spread |
| load-bars | 60,480 | 21,266.2 qps | 4.8 % | 22,397.4 qps | 6.0 % | -5.1 % | inside the spread |
| load-stats | 60,480 | 22,215.4 qps | 4.5 % | 21,824.7 qps | 1.0 % | +1.8 % | inside the spread |
| load-models | 8 | 1,475.5 qps | 17.0 % | 1,647.0 qps | 17.5 % | -10.4 % | inside the spread |
| backtest-read | 360 | 609.0 qps | 1.2 % | 615.0 qps | 1.1 % | -1.0 % | inside the spread |
| result-insert | 2,872 | 1,798.6 qps | 3.4 % | 1,789.2 qps | 3.0 % | +0.5 % | inside the spread |
| backtest-replay | 360 | 634.7 qps | 6.5 % | 658.4 qps | 15.8 % | -3.6 % | inside the spread |
| compare-all | 1 | 91.3 qps | 0.5 % | 92.4 qps | 4.1 % | -1.2 % | inside the spread |
| compare-one | 32 | 925.1 qps | 4.0 % | 915.3 qps | 2.6 % | +1.1 % | inside the spread |
| read-bar-lookup | 200 | 13,238.3 qps | 38.7 % | 13,081.4 qps | 30.0 % | +1.2 % | spread > 20 %, no claim |
| read-bar-range | 200 | 1,138.2 qps | 2.9 % | 1,071.5 qps | 9.2 % | +6.2 % | inside the spread |
| read-symbol-history | 10 | 27.3 qps | 5.1 % | 27.3 qps | 0.4 % | +0.0 % | inside the spread |
| read-day-slice | 200 | 685.7 qps | 3.5 % | 666.4 qps | 1.2 % | +2.9 % | inside the spread |
| read-join-point | 200 | 14,018.5 qps | 6.8 % | 13,645.8 qps | 4.2 % | +2.7 % | inside the spread |
| read-join-exists | 10 | 195.6 qps | 1.4 % | 202.8 qps | 3.7 % | -3.6 % | inside the spread |
| agg-global | 10 | 172.7 qps | 2.7 % | 172.8 qps | 3.4 % | -0.1 % | inside the spread |
| agg-by-symbol | 10 | 135.4 qps | 2.2 % | 130.4 qps | 4.7 % | +3.8 % | inside the spread |
| agg-by-session | 10 | 40.6 qps | 1.7 % | 40.6 qps | 1.5 % | +0.0 % | inside the spread |
| agg-day-slice | 200 | 700.5 qps | 1.4 % | 688.1 qps | 2.8 % | +1.8 % | inside the spread |
| agg-distinct | 10 | 167.4 qps | 0.7 % | 167.9 qps | 4.8 % | -0.3 % | inside the spread |
| sync | 1 | 295.0 qps | 13.9 % | 281.3 qps | 10.3 % | +4.9 % | inside the spread |

## 4. Percentiles, run 1 of `s1-c1-g`

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load-exchanges | 2 | 492.6 µs | 492.6 µs | 492.6 µs | 555.3 µs | 555.3 µs | 555.3 µs | 0 |
| load-symbols | 8 | 395.9 µs | 440.7 µs | 476.5 µs | 650.3 µs | 650.3 µs | 650.3 µs | 0 |
| load-sessions | 7,560 | 37.6 µs | 39.9 µs | 41.2 µs | 57.3 µs | 89.0 µs | 269.0 µs | 0 |
| load-bars | 60,480 | 38.7 µs | 41.7 µs | 42.7 µs | 59.7 µs | 101.8 µs | 2,573.0 µs | 0 |
| load-stats | 60,480 | 29.9 µs | 41.9 µs | 42.9 µs | 58.9 µs | 94.7 µs | 429.2 µs | 0 |
| load-models | 8 | 508.4 µs | 520.1 µs | 627.4 µs | 1,096.7 µs | 1,096.7 µs | 1,096.7 µs | 0 |
| backtest-read | 360 | 1,502.9 µs | 1,605.3 µs | 1,644.2 µs | 1,773.3 µs | 2,145.4 µs | 3,588.6 µs | 0 |
| result-insert | 2,872 | 326.7 µs | 460.5 µs | 525.4 µs | 742.7 µs | 873.9 µs | 1,403.5 µs | 0 |
| backtest-replay | 360 | 1,412.7 µs | 1,550.4 µs | 1,574.2 µs | 1,641.0 µs | 2,085.3 µs | 2,626.9 µs | 0 |
| compare-all | 1 | 10,928.8 µs | 10,928.8 µs | 10,928.8 µs | 10,928.8 µs | 10,928.8 µs | 10,928.8 µs | 0 |
| compare-one | 32 | 992.3 µs | 1,035.1 µs | 1,062.6 µs | 1,191.2 µs | 1,258.5 µs | 1,258.5 µs | 0 |
| read-bar-lookup | 200 | 67.5 µs | 70.3 µs | 71.3 µs | 82.0 µs | 111.6 µs | 199.3 µs | 0 |
| read-bar-range | 200 | 797.4 µs | 833.3 µs | 854.1 µs | 982.8 µs | 1,038.9 µs | 1,074.8 µs | 0 |
| read-symbol-history | 10 | 35,139.5 µs | 35,500.4 µs | 35,636.8 µs | 38,499.7 µs | 38,499.7 µs | 38,499.7 µs | 0 |
| read-day-slice | 200 | 1,339.4 µs | 1,400.2 µs | 1,423.3 µs | 1,568.9 µs | 1,682.7 µs | 1,815.8 µs | 0 |
| read-join-point | 200 | 65.8 µs | 68.2 µs | 69.0 µs | 78.3 µs | 107.7 µs | 173.9 µs | 0 |
| read-join-exists | 10 | 4,646.3 µs | 4,966.9 µs | 5,034.3 µs | 5,734.4 µs | 5,734.4 µs | 5,734.4 µs | 0 |
| agg-global | 10 | 5,683.5 µs | 5,747.0 µs | 5,750.4 µs | 5,892.4 µs | 5,892.4 µs | 5,892.4 µs | 0 |
| agg-by-symbol | 10 | 7,280.2 µs | 7,296.1 µs | 7,316.4 µs | 7,586.3 µs | 7,586.3 µs | 7,586.3 µs | 0 |
| agg-by-session | 10 | 23,847.2 µs | 24,129.0 µs | 24,280.7 µs | 26,204.6 µs | 26,204.6 µs | 26,204.6 µs | 0 |
| agg-day-slice | 200 | 1,312.1 µs | 1,373.6 µs | 1,409.3 µs | 1,546.3 µs | 1,788.7 µs | 3,007.7 µs | 0 |
| agg-distinct | 10 | 5,785.3 µs | 5,829.1 µs | 5,880.2 µs | 6,458.3 µs | 6,458.3 µs | 6,458.3 µs | 0 |
| sync | 1 | 3,477.6 µs | 3,477.6 µs | 3,477.6 µs | 3,477.6 µs | 3,477.6 µs | 3,477.6 µs | 0 |

## 5. The size sweep

| bars | shape | warm now | spread | warm v3.4.14 | spread | delta | read |
|---|---|---|---|---|---|---|---|
| 252 | bar-lookup | 13,595.4 qps | 37.6 % | 13,555.0 qps | 41.3 % | +0.3 % | spread > 20 %, no claim |
| 252 | bar-range | 1,131.3 qps | 3.0 % | 1,103.2 qps | 3.3 % | +2.5 % | inside the spread |
| 252 | day-slice | 11,165.6 qps | 28.7 % | 11,151.9 qps | 41.2 % | +0.1 % | spread > 20 %, no claim |
| 252 | symbol-history | 735.9 qps | 2.3 % | 740.3 qps | 0.6 % | -0.6 % | inside the spread |
| 252 | cross-join | 9,726.7 qps | 6.5 % | 9,698.6 qps | 1.9 % | +0.3 % | inside the spread |
| 252 | point-join | 14,694.7 qps | 4.6 % | 13,579.0 qps | 47.5 % | +8.2 % | spread > 20 %, no claim |
| 252 | model-join | 9,241.5 qps | 4.9 % | 8,349.3 qps | 41.1 % | +10.7 % | spread > 20 %, no claim |
| 1,008 | bar-lookup | 13,339.1 qps | 41.3 % | 13,715.9 qps | 38.8 % | -2.7 % | spread > 20 %, no claim |
| 1,008 | bar-range | 1,152.3 qps | 3.6 % | 1,108.2 qps | 3.8 % | +4.0 % | **finding** |
| 1,008 | day-slice | 8,439.0 qps | 38.5 % | 7,920.5 qps | 38.5 % | +6.5 % | spread > 20 %, no claim |
| 1,008 | symbol-history | 703.4 qps | 3.4 % | 702.5 qps | 1.6 % | +0.1 % | inside the spread |
| 1,008 | cross-join | 7,203.7 qps | 6.6 % | 7,159.7 qps | 38.0 % | +0.6 % | spread > 20 %, no claim |
| 1,008 | point-join | 14,421.8 qps | 3.7 % | 14,380.3 qps | 21.5 % | +0.3 % | spread > 20 %, no claim |
| 1,008 | model-join | 9,265.1 qps | 2.2 % | 8,944.7 qps | 38.2 % | +3.6 % | spread > 20 %, no claim |
| 10,080 | bar-lookup | 12,419.0 qps | 39.4 % | 13,958.9 qps | 0.7 % | -11.0 % | spread > 20 %, no claim |
| 10,080 | bar-range | 1,134.7 qps | 2.8 % | 1,138.3 qps | 4.7 % | -0.3 % | inside the spread |
| 10,080 | day-slice | 2,534.6 qps | 16.6 % | 2,679.8 qps | 6.1 % | -5.4 % | inside the spread |
| 10,080 | symbol-history | 152.1 qps | 1.8 % | 153.6 qps | 2.8 % | -1.0 % | inside the spread |
| 10,080 | cross-join | 2,410.0 qps | 13.0 % | 2,423.0 qps | 18.0 % | -0.5 % | inside the spread |
| 10,080 | point-join | 10,552.9 qps | 55.8 % | 14,041.9 qps | 49.3 % | -24.8 % | spread > 20 %, no claim |
| 10,080 | model-join | 3,714.7 qps | 27.5 % | 4,093.3 qps | 25.6 % | -9.2 % | spread > 20 %, no claim |

One cell crosses its spread (1,008 bars, `bar-range`, +4.0 % against spreads of 3.6 % and 3.8 %); it is a cross-run reading at the floor, the A/B does not show it at 60,480 bars, and it is not read as an engine change.

## 6. The engine, isolated: `v3.4.14` against `v3.5.0`, interleaved on this host

The `v3.4.14` binary (the copy measured on 2026-11-19, sha256 `78b3aede...`) and the measured `v3.5.0` binary, one driver (the tag's), interleaved old, new, old, new, old, new, each on a fresh server and data file:

| measure | v3.4.14 engine | spread | v3.5.0 engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s1 bar-lookup warm | 13,648.6 qps | 7.4 % | 13,882.1 qps | 9.0 % | 1.02x | inside the spread |
| s1 bar-range warm | 1,149.7 qps | 2.3 % | 1,142.9 qps | 3.4 % | 0.99x | inside the spread |
| s1 day-slice warm | 668.5 qps | 2.4 % | 657.0 qps | 3.2 % | 0.98x | inside the spread |
| s1 symbol-history warm | 27.1 qps | 1.1 % | 27.3 qps | 2.6 % | 1.01x | inside the spread |
| s1 cross-join warm | 665.2 qps | 6.1 % | 651.5 qps | 3.9 % | 0.98x | inside the spread |
| s1 point-join warm | 13,567.9 qps | 36.0 % | 14,344.6 qps | 1.8 % | 1.06x | spread > 20 %, no claim |
| s1 model-join warm | 903.1 qps | 2.2 % | 906.4 qps | 3.1 % | 1.00x | inside the spread |
| s1 phase backtest-read | 610.1 qps | 2.2 % | 609.9 qps | 2.3 % | 1.00x | inside the spread |
| s1 phase result-insert | 1,840.8 qps | 1.3 % | 1,821.0 qps | 1.4 % | 0.99x | inside the spread |
| s1 phase compare-one | 894.3 qps | 2.2 % | 909.5 qps | 4.3 % | 1.02x | inside the spread |
| s1 phase load-bars | 21,876.0 qps | 1.3 % | 21,837.1 qps | 3.3 % | 1.00x | inside the spread |
| s1 phase agg-by-session | 41.2 qps | 0.7 % | 40.8 qps | 1.5 % | 0.99x | inside the spread |

## 7. What the run says about the engine

- **No change between `v3.4.14` and `v3.5.0` on scenario 1**: pk reads, ranges, walks, joins, aggregates, the durable insert phase and the bulk loads are inside their spreads in the cross-run reading and 0.98x to 1.02x in the A/B.
- The bulk-load phases of the main cell read 5 % to 11 % lower than the `v3.4.14` run (`load-bars` -5.1 %, inside the spread); the A/B's `load-bars` is 1.00x, so it is the run window, not the engine.

## 8. Noise floor, contamination and host

Every quiet-gate wait was 0 s. No foreign sample of 50 % or more of one CPU fell inside a scenario 1 cell (the summary's Contamination section lists every sample above 40 %). `point-join` and `read-bar-lookup` are bimodal across runs (spreads 24.7 % and 38.7 %) as at `v3.4.14`, and carry no claim.

## 9. Archive

`bench/v3.5.0/archive/scenario1-v3.5.0/` (per-cell JSON, driver stdout, prechecks, gate waits, server configs and logs) and `bench/v3.5.0/archive/run-v3.5.0/` (the run scripts, watchdogs, monitor log, timeline, binary hashes, the table generator `gen.py` and its output under `tables/`). No data file is kept.
