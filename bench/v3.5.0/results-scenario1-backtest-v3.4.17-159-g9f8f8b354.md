# Scenario 1 (backtest) at `v3.4.17-159-g9f8f8b354`

**One line.** On scenario 1 the engine at `9f8f8b354` is the engine of `07f4552a` (`v3.5.0`): in the same-host A/B with the server pinned to the P-cores (five interleaved pairs), every warm, cold and Cabin shape that resolves reads 0.98x to 1.02x and every phase that resolves 0.95x to 1.02x, all inside their spreads. Nothing in DL, DK, DD, DN, DO, DP, DQ, DR or DU has a price scenario 1 can read. The warm shapes read 1.5x to 2.5x the `v3.5.0` run's (the phases 1.3x to 3.2x), which is this host against that one. Unpinned, the A/B on this host is not usable: 15 of its 18 sweep rows and 14 of its 23 phases fail the spread rule, and the six rows the rule marks (1.05x to 1.51x) read 0.95x to 1.02x pinned, inside their spreads. Verification passed 34 of 34 runs (main, A/B and pinned A/B).

Driver: `tools/scenario1_backtest.py` of the measured commit, unmodified (byte-identical to `07f4552a`'s), with `--seed 1 --bars-clustered btree --verify --sync --analyze`. Measured directly by CLA from a scheduled session (no `ck-tester` agent was spawned; the tester's rules in `.claude/agents/ck-tester.md` were followed through the run scripts). Comparator run: `results-scenario1-backtest-v3.5.0.md` (2026-11-20, `07f4552a`, **another host**: an 8-CPU Xeon Platinum 8581C VM), so a ratio against it is host plus engine and is not read as an engine change. The engine reading is the interleaved A/B on this host against a `07f4552a` binary built here (sections 6 and 7).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-26; main cells 21:05:49 to 21:09:38 UTC; unpinned A/B cells inside 21:19:44 to 21:41:59 UTC; pinned A/B cells inside 21:52:11 to 22:07:44 UTC |
| Version, commit, binary, host, device, budget | stamped once in `summary-scenarios-v3.4.17-159-g9f8f8b354.md`, section Stamp |
| Server config | the common config; `cores` 1 or 8, `durability` group or strict per cell; port 15710 |
| Driver | exit 0 in every cell; `verify_problems` empty in 34 of 34 runs (18 main, 6 unpinned A/B, 10 pinned A/B) |

## 2. What was run

The `v3.5.0` run's cells, unchanged in arguments and suffixes: three cells (`c1-g`, `c8-g`, `c1-s`: cores and durability) three times each, the size sweep (252, 1,008 and 10,080 bars at `c1-g`) three times each, and the engine A/B (`c1-g`, three interleaved pairs, old first). **Added**: the same A/B again with the server pinned to the P-cores and the driver to the E-cores (five interleaved pairs), because this host is a hybrid CPU and the unpinned cells are bimodal across runs (section 8). Cut: the join-order probe (as at `v3.5.0`) and PostgreSQL (no server on this host).

## 3. Results

Warm and cold qps from the driver's sweep, median of three runs; spread = (max - min) / median. **The last column is this host over another host**: no cell of it is a finding about the engine.

#### cell c1-g: cores = 1, group

| shape | cold now | warm now | spread | cabin now | warm v3.5.0 run (other host) | spread | now over v3.5.0 run (host + engine) |
|---|---|---|---|---|---|---|---|
| bar-lookup | 19,260.6 qps | 25,652.3 qps | 17.0 % | - | 13,523.8 qps | 4.2 % | 1.90x |
| bar-range | 1,815.0 qps | 1,939.4 qps | 28.8 % | - | 1,160.4 qps | 4.9 % | 1.67x |
| day-slice | 1,072.6 qps | 1,079.9 qps | 35.9 % | 12,976.6 qps | 677.6 qps | 2.6 % | 1.59x |
| symbol-history | 45.8 qps | 48.8 qps | 14.1 % | 45.6 qps | 27.1 qps | 1.1 % | 1.80x |
| cross-join | 1,019.6 qps | 1,053.7 qps | 22.7 % | 11,733.8 qps | 640.7 qps | 5.4 % | 1.64x |
| point-join | 23,493.2 qps | 24,100.1 qps | 79.1 % | - | 13,782.7 qps | 24.7 % | 1.75x |
| model-join | 1,649.7 qps | 1,622.1 qps | 12.5 % | 1,687.1 qps | 898.4 qps | 5.5 % | 1.81x |

#### cell c8-g: cores = 8, group

| shape | cold now | warm now | spread | cabin now | warm v3.5.0 run (other host) | spread | now over v3.5.0 run (host + engine) |
|---|---|---|---|---|---|---|---|
| bar-lookup | 19,259.7 qps | 26,627.8 qps | 5.2 % | - | 13,019.7 qps | 3.4 % | 2.05x |
| bar-range | 1,711.7 qps | 1,711.3 qps | 39.4 % | - | 1,074.7 qps | 4.4 % | 1.59x |
| day-slice | 1,364.0 qps | 1,400.5 qps | 5.2 % | 15,496.4 qps | 606.2 qps | 3.9 % | 2.31x |
| symbol-history | 42.8 qps | 45.3 qps | 9.1 % | 41.8 qps | 26.7 qps | 0.7 % | 1.70x |
| cross-join | 1,257.5 qps | 1,331.0 qps | 4.0 % | 12,574.1 qps | 588.0 qps | 5.2 % | 2.26x |
| point-join | 29,458.8 qps | 30,605.1 qps | 5.1 % | - | 13,678.4 qps | 5.2 % | 2.24x |
| model-join | 1,847.6 qps | 1,928.8 qps | 27.5 % | 1,941.3 qps | 839.5 qps | 3.0 % | 2.30x |

#### cell c1-s: cores = 1, strict

| shape | cold now | warm now | spread | cabin now | warm v3.5.0 run (other host) | spread | now over v3.5.0 run (host + engine) |
|---|---|---|---|---|---|---|---|
| bar-lookup | 11,489.7 qps | 26,545.5 qps | 11.5 % | - | 13,119.0 qps | 38.4 % | 2.02x |
| bar-range | 1,744.1 qps | 1,947.9 qps | 25.5 % | - | 1,144.2 qps | 3.5 % | 1.70x |
| day-slice | 1,089.1 qps | 1,092.3 qps | 1.3 % | 12,904.8 qps | 649.3 qps | 2.9 % | 1.68x |
| symbol-history | 42.9 qps | 42.8 qps | 0.7 % | 44.8 qps | 27.2 qps | 1.1 % | 1.57x |
| cross-join | 900.7 qps | 967.0 qps | 16.3 % | 10,657.1 qps | 650.4 qps | 2.5 % | 1.49x |
| point-join | 19,540.4 qps | 23,114.6 qps | 37.5 % | - | 14,128.8 qps | 8.4 % | 1.64x |
| model-join | 1,649.0 qps | 1,688.4 qps | 1.9 % | 1,721.7 qps | 918.8 qps | 2.8 % | 1.84x |

#### phases of `s1-c1-g`

| phase | ops | now | spread | v3.5.0 run (other host) | spread | now over v3.5.0 run (host + engine) |
|---|---|---|---|---|---|---|
| load-exchanges | 2 | 2,728.8 qps | 41.8 % | 1,651.6 qps | 21.3 % | 1.65x |
| load-symbols | 8 | 6,510.0 qps | 66.7 % | 2,010.0 qps | 22.2 % | 3.24x |
| load-sessions | 7,560 | 68,740.8 qps | 49.8 % | 21,771.0 qps | 12.1 % | 3.16x |
| load-bars | 60,480 | 44,817.5 qps | 12.6 % | 21,266.2 qps | 4.8 % | 2.11x |
| load-stats | 60,480 | 45,144.6 qps | 15.6 % | 22,215.4 qps | 4.5 % | 2.03x |
| load-models | 8 | 2,240.7 qps | 35.4 % | 1,475.5 qps | 17.0 % | 1.52x |
| backtest-read | 360 | 1,096.2 qps | 5.2 % | 609.0 qps | 1.2 % | 1.80x |
| result-insert | 2,872 | 4,621.7 qps | 6.1 % | 1,798.6 qps | 3.4 % | 2.57x |
| backtest-replay | 360 | 1,062.1 qps | 30.1 % | 634.7 qps | 6.5 % | 1.67x |
| compare-all | 1 | 157.7 qps | 11.7 % | 91.3 qps | 0.5 % | 1.73x |
| compare-one | 32 | 1,559.5 qps | 18.0 % | 925.1 qps | 4.0 % | 1.69x |
| read-bar-lookup | 200 | 23,031.6 qps | 40.1 % | 13,238.3 qps | 38.7 % | 1.74x |
| read-bar-range | 200 | 1,924.5 qps | 5.6 % | 1,138.2 qps | 2.9 % | 1.69x |
| read-symbol-history | 10 | 45.5 qps | 23.5 % | 27.3 qps | 5.1 % | 1.67x |
| read-day-slice | 200 | 912.8 qps | 17.4 % | 685.7 qps | 3.5 % | 1.33x |
| read-join-point | 200 | 23,106.5 qps | 27.5 % | 14,018.5 qps | 6.8 % | 1.65x |
| read-join-exists | 10 | 337.6 qps | 2.6 % | 195.6 qps | 1.4 % | 1.73x |
| agg-global | 10 | 277.4 qps | 14.7 % | 172.7 qps | 2.7 % | 1.61x |
| agg-by-symbol | 10 | 211.6 qps | 26.6 % | 135.4 qps | 2.2 % | 1.56x |
| agg-by-session | 10 | 71.8 qps | 5.8 % | 40.6 qps | 1.7 % | 1.77x |
| agg-day-slice | 200 | 1,111.6 qps | 17.0 % | 700.5 qps | 1.4 % | 1.59x |
| agg-distinct | 10 | 252.8 qps | 10.5 % | 167.4 qps | 0.7 % | 1.51x |
| sync | 1 | 457.3 qps | 24.6 % | 295.0 qps | 13.9 % | 1.55x |

## 4. Percentiles, run 1 of `s1-c1-g`

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load-exchanges | 2 | 340.7 µs | 340.7 µs | 340.7 µs | 916.9 µs | 916.9 µs | 916.9 µs | 0 |
| load-symbols | 8 | 235.2 µs | 280.8 µs | 311.5 µs | 428.3 µs | 428.3 µs | 428.3 µs | 0 |
| load-sessions | 7,560 | 21.7 µs | 27.5 µs | 27.7 µs | 29.5 µs | 46.0 µs | 195.9 µs | 0 |
| load-bars | 60,480 | 13.9 µs | 15.4 µs | 21.2 µs | 29.9 µs | 39.9 µs | 734.5 µs | 0 |
| load-stats | 60,480 | 14.1 µs | 14.8 µs | 28.0 µs | 29.4 µs | 37.2 µs | 1,033.2 µs | 0 |
| load-models | 8 | 320.0 µs | 325.7 µs | 341.1 µs | 744.3 µs | 744.3 µs | 744.3 µs | 0 |
| backtest-read | 360 | 731.3 µs | 761.0 µs | 959.8 µs | 1,021.6 µs | 1,304.2 µs | 3,910.9 µs | 0 |
| result-insert | 2,872 | 179.8 µs | 200.8 µs | 206.8 µs | 255.6 µs | 432.1 µs | 834.4 µs | 0 |
| backtest-replay | 360 | 719.4 µs | 726.1 µs | 728.8 µs | 735.7 µs | 743.1 µs | 760.8 µs | 0 |
| compare-all | 1 | 5,680.9 µs | 5,680.9 µs | 5,680.9 µs | 5,680.9 µs | 5,680.9 µs | 5,680.9 µs | 0 |
| compare-one | 32 | 570.4 µs | 636.9 µs | 734.0 µs | 809.9 µs | 848.3 µs | 848.3 µs | 0 |
| read-bar-lookup | 200 | 27.0 µs | 27.6 µs | 27.9 µs | 30.2 µs | 250.0 µs | 298.2 µs | 0 |
| read-bar-range | 200 | 399.8 µs | 418.6 µs | 529.9 µs | 671.2 µs | 697.9 µs | 716.4 µs | 0 |
| read-symbol-history | 10 | 18,276.3 µs | 18,529.8 µs | 18,665.7 µs | 20,859.2 µs | 20,859.2 µs | 20,859.2 µs | 0 |
| read-day-slice | 200 | 919.6 µs | 1,078.9 µs | 1,102.7 µs | 1,230.9 µs | 1,290.3 µs | 1,592.5 µs | 0 |
| read-join-point | 200 | 34.7 µs | 35.9 µs | 36.5 µs | 74.4 µs | 328.1 µs | 347.2 µs | 0 |
| read-join-exists | 10 | 2,831.8 µs | 2,875.4 µs | 2,903.5 µs | 3,344.3 µs | 3,344.3 µs | 3,344.3 µs | 0 |
| agg-global | 10 | 3,735.6 µs | 3,760.8 µs | 3,804.9 µs | 4,012.5 µs | 4,012.5 µs | 4,012.5 µs | 0 |
| agg-by-symbol | 10 | 4,992.8 µs | 5,023.6 µs | 5,050.2 µs | 5,191.0 µs | 5,191.0 µs | 5,191.0 µs | 0 |
| agg-by-session | 10 | 12,531.0 µs | 12,740.1 µs | 12,797.7 µs | 18,345.7 µs | 18,345.7 µs | 18,345.7 µs | 0 |
| agg-day-slice | 200 | 930.6 µs | 1,033.8 µs | 1,058.3 µs | 1,181.7 µs | 1,284.6 µs | 1,564.5 µs | 0 |
| agg-distinct | 10 | 4,053.8 µs | 4,096.9 µs | 4,101.0 µs | 4,202.8 µs | 4,202.8 µs | 4,202.8 µs | 0 |
| sync | 1 | 1,893.7 µs | 1,893.7 µs | 1,893.7 µs | 1,893.7 µs | 1,893.7 µs | 1,893.7 µs | 0 |

## 5. The size sweep

| bars | shape | warm now | spread | warm v3.5.0 run (other host) | spread | now over v3.5.0 run (host + engine) |
|---|---|---|---|---|---|---|
| 252 | bar-lookup | 32,922.9 qps | 29.2 % | 13,595.4 qps | 37.6 % | 2.42x |
| 252 | bar-range | 2,447.3 qps | 24.3 % | 1,131.3 qps | 3.0 % | 2.16x |
| 252 | day-slice | 27,362.2 qps | 36.4 % | 11,165.6 qps | 28.7 % | 2.45x |
| 252 | symbol-history | 1,570.2 qps | 19.7 % | 735.9 qps | 2.3 % | 2.13x |
| 252 | cross-join | 16,967.0 qps | 27.7 % | 9,726.7 qps | 6.5 % | 1.74x |
| 252 | point-join | 31,852.3 qps | 35.9 % | 14,694.7 qps | 4.6 % | 2.17x |
| 252 | model-join | 19,701.5 qps | 28.9 % | 9,241.5 qps | 4.9 % | 2.13x |
| 1,008 | bar-lookup | 33,034.2 qps | 41.3 % | 13,339.1 qps | 41.3 % | 2.48x |
| 1,008 | bar-range | 2,426.6 qps | 25.8 % | 1,152.3 qps | 3.6 % | 2.11x |
| 1,008 | day-slice | 18,999.0 qps | 33.4 % | 8,439.0 qps | 38.5 % | 2.25x |
| 1,008 | symbol-history | 1,535.9 qps | 20.8 % | 703.4 qps | 3.4 % | 2.18x |
| 1,008 | cross-join | 13,206.9 qps | 27.5 % | 7,203.7 qps | 6.6 % | 1.83x |
| 1,008 | point-join | 31,652.7 qps | 36.7 % | 14,421.8 qps | 3.7 % | 2.19x |
| 1,008 | model-join | 19,911.9 qps | 30.6 % | 9,265.1 qps | 2.2 % | 2.15x |
| 10,080 | bar-lookup | 23,317.8 qps | 54.7 % | 12,419.0 qps | 39.4 % | 1.88x |
| 10,080 | bar-range | 1,926.3 qps | 12.5 % | 1,134.7 qps | 2.8 % | 1.70x |
| 10,080 | day-slice | 5,209.0 qps | 32.3 % | 2,534.6 qps | 16.6 % | 2.06x |
| 10,080 | symbol-history | 255.4 qps | 11.3 % | 152.1 qps | 1.8 % | 1.68x |
| 10,080 | cross-join | 4,574.0 qps | 1.9 % | 2,410.0 qps | 13.0 % | 1.90x |
| 10,080 | point-join | 24,029.8 qps | 62.1 % | 10,552.9 qps | 55.8 % | 2.28x |
| 10,080 | model-join | 7,366.4 qps | 30.9 % | 3,714.7 qps | 27.5 % | 1.98x |

## 6. The engine, isolated, unpinned: `07f4552a` against `9f8f8b354`, interleaved on this host

The `07f4552a` binary built on this host for this run (Release, sha256 `03c8c2e9...`) and the measured binary (sha256 `a333ee48...`), one driver, interleaved old, new, old, new, old, new, each on a fresh server and data file. A row reads `spread > 20 %, no claim` when either arm's three runs spread over 20 %, and on this host most rows do: nothing fixes which class of core the server's thread runs on (section 9), and section 7 repeats the series with that fixed.

| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s1 bar-lookup cold | 12,955.0 qps | 74.2 % | 15,548.3 qps | 41.1 % | 1.20x | spread > 20 %, no claim |
| s1 bar-lookup warm | 26,430.7 qps | 28.5 % | 23,177.1 qps | 38.7 % | 0.88x | spread > 20 %, no claim |
| s1 bar-range cold | 1,932.9 qps | 27.2 % | 1,940.2 qps | 10.8 % | 1.00x | spread > 20 %, no claim |
| s1 bar-range warm | 1,948.9 qps | 28.7 % | 1,950.0 qps | 19.4 % | 1.00x | spread > 20 %, no claim |
| s1 day-slice cold | 1,105.7 qps | 44.4 % | 1,075.6 qps | 13.0 % | 0.97x | spread > 20 %, no claim |
| s1 day-slice warm | 1,106.6 qps | 46.9 % | 1,097.0 qps | 1.3 % | 0.99x | spread > 20 %, no claim |
| s1 day-slice cabin | 12,700.1 qps | 45.0 % | 12,989.3 qps | 9.2 % | 1.02x | spread > 20 %, no claim |
| s1 symbol-history cold | 43.2 qps | 20.8 % | 43.2 qps | 10.2 % | 1.00x | spread > 20 %, no claim |
| s1 symbol-history warm | 45.1 qps | 9.5 % | 45.1 qps | 12.2 % | 1.00x | inside the spread |
| s1 symbol-history cabin | 46.0 qps | 7.2 % | 45.1 qps | 14.0 % | 0.98x | inside the spread |
| s1 cross-join cold | 1,013.2 qps | 4.1 % | 1,066.2 qps | 4.1 % | 1.05x | **finding** |
| s1 cross-join warm | 1,073.4 qps | 2.9 % | 1,081.4 qps | 36.0 % | 1.01x | spread > 20 %, no claim |
| s1 cross-join cabin | 11,991.8 qps | 1.7 % | 11,738.9 qps | 30.8 % | 0.98x | spread > 20 %, no claim |
| s1 point-join cold | 21,467.4 qps | 13.6 % | 23,134.1 qps | 48.5 % | 1.08x | spread > 20 %, no claim |
| s1 point-join warm | 23,679.3 qps | 15.2 % | 23,801.1 qps | 49.7 % | 1.01x | spread > 20 %, no claim |
| s1 model-join cold | 1,626.1 qps | 1.8 % | 1,640.2 qps | 23.2 % | 1.01x | spread > 20 %, no claim |
| s1 model-join warm | 1,618.8 qps | 5.3 % | 1,683.4 qps | 26.6 % | 1.04x | spread > 20 %, no claim |
| s1 model-join cabin | 1,723.1 qps | 1.9 % | 1,705.2 qps | 22.6 % | 0.99x | spread > 20 %, no claim |

| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s1 phase load-exchanges | 4,739.1 qps | 15.1 % | 1,633.6 qps | 101.9 % | 0.34x | spread > 20 %, no claim |
| s1 phase load-symbols | 6,183.6 qps | 28.9 % | 4,254.1 qps | 44.7 % | 0.69x | spread > 20 %, no claim |
| s1 phase load-sessions | 37,463.8 qps | 81.3 % | 50,040.1 qps | 47.8 % | 1.34x | spread > 20 %, no claim |
| s1 phase load-bars | 38,634.4 qps | 21.0 % | 43,287.3 qps | 28.1 % | 1.12x | spread > 20 %, no claim |
| s1 phase load-stats | 39,379.0 qps | 23.2 % | 43,717.2 qps | 29.3 % | 1.11x | spread > 20 %, no claim |
| s1 phase load-models | 2,136.7 qps | 18.0 % | 2,001.7 qps | 149.2 % | 0.94x | spread > 20 %, no claim |
| s1 phase backtest-read | 1,107.2 qps | 13.3 % | 1,190.1 qps | 1.6 % | 1.07x | inside the spread |
| s1 phase result-insert | 4,264.8 qps | 53.4 % | 4,185.3 qps | 47.4 % | 0.98x | spread > 20 %, no claim |
| s1 phase backtest-replay | 1,088.1 qps | 34.1 % | 1,404.4 qps | 38.4 % | 1.29x | spread > 20 %, no claim |
| s1 phase compare-all | 157.6 qps | 19.9 % | 178.4 qps | 18.2 % | 1.13x | inside the spread |
| s1 phase compare-one | 1,565.3 qps | 33.6 % | 1,173.1 qps | 33.3 % | 0.75x | spread > 20 %, no claim |
| s1 phase read-bar-lookup | 22,801.7 qps | 30.9 % | 26,033.5 qps | 5.3 % | 1.14x | spread > 20 %, no claim |
| s1 phase read-bar-range | 1,927.5 qps | 25.2 % | 2,339.8 qps | 7.2 % | 1.21x | spread > 20 %, no claim |
| s1 phase read-symbol-history | 42.2 qps | 25.8 % | 52.6 qps | 18.8 % | 1.25x | spread > 20 %, no claim |
| s1 phase read-day-slice | 1,084.8 qps | 18.4 % | 1,343.6 qps | 4.4 % | 1.24x | **finding** |
| s1 phase read-join-point | 22,398.6 qps | 17.7 % | 30,313.5 qps | 19.2 % | 1.35x | **finding** |
| s1 phase read-join-exists | 353.2 qps | 4.0 % | 488.3 qps | 1.2 % | 1.38x | **finding** |
| s1 phase agg-global | 269.7 qps | 6.4 % | 406.7 qps | 8.6 % | 1.51x | **finding** |
| s1 phase agg-by-symbol | 207.8 qps | 15.0 % | 292.9 qps | 11.2 % | 1.41x | **finding** |
| s1 phase agg-by-session | 76.7 qps | 10.8 % | 75.6 qps | 3.2 % | 0.99x | inside the spread |
| s1 phase agg-day-slice | 1,144.4 qps | 40.7 % | 1,101.5 qps | 16.0 % | 0.96x | spread > 20 %, no claim |
| s1 phase agg-distinct | 260.2 qps | 46.7 % | 253.1 qps | 4.6 % | 0.97x | spread > 20 %, no claim |
| s1 phase sync | 429.4 qps | 8.1 % | 439.5 qps | 15.6 % | 1.02x | inside the spread |

## 7. The engine, isolated, pinned: the same A/B with the server on the P-cores

Server under `taskset -c 0-7` (the eight P-cores), driver under `taskset -c 8-23` (the sixteen E-cores), five interleaved pairs, old first; everything else as section 6.

| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s1 bar-lookup cold | 12,865.4 qps | 68.5 % | 11,870.1 qps | 66.9 % | 0.92x | spread > 20 %, no claim |
| s1 bar-lookup warm | 26,242.5 qps | 10.4 % | 25,695.8 qps | 28.9 % | 0.98x | spread > 20 %, no claim |
| s1 bar-range cold | 1,735.9 qps | 6.9 % | 1,743.0 qps | 7.6 % | 1.00x | inside the spread |
| s1 bar-range warm | 1,739.0 qps | 6.4 % | 1,737.3 qps | 4.9 % | 1.00x | inside the spread |
| s1 day-slice cold | 1,387.1 qps | 7.5 % | 1,393.3 qps | 8.4 % | 1.00x | inside the spread |
| s1 day-slice warm | 1,376.2 qps | 3.5 % | 1,377.9 qps | 8.7 % | 1.00x | inside the spread |
| s1 day-slice cabin | 12,505.1 qps | 20.5 % | 11,435.2 qps | 52.7 % | 0.91x | spread > 20 %, no claim |
| s1 symbol-history cold | 42.4 qps | 1.2 % | 42.5 qps | 1.2 % | 1.00x | inside the spread |
| s1 symbol-history warm | 41.7 qps | 1.0 % | 41.9 qps | 1.2 % | 1.00x | inside the spread |
| s1 symbol-history cabin | 41.4 qps | 0.7 % | 41.7 qps | 0.7 % | 1.01x | inside the spread |
| s1 cross-join cold | 1,257.4 qps | 9.3 % | 1,285.4 qps | 6.0 % | 1.02x | inside the spread |
| s1 cross-join warm | 1,342.0 qps | 6.7 % | 1,373.5 qps | 4.2 % | 1.02x | inside the spread |
| s1 cross-join cabin | 11,764.8 qps | 10.5 % | 10,020.9 qps | 20.3 % | 0.85x | spread > 20 %, no claim |
| s1 point-join cold | 22,690.8 qps | 7.6 % | 22,107.6 qps | 36.1 % | 0.97x | spread > 20 %, no claim |
| s1 point-join warm | 29,364.6 qps | 5.4 % | 27,974.1 qps | 33.3 % | 0.95x | spread > 20 %, no claim |
| s1 model-join cold | 1,657.3 qps | 3.5 % | 1,632.1 qps | 11.3 % | 0.98x | inside the spread |
| s1 model-join warm | 1,515.3 qps | 1.8 % | 1,517.4 qps | 3.2 % | 1.00x | inside the spread |
| s1 model-join cabin | 1,537.6 qps | 2.2 % | 1,531.3 qps | 2.1 % | 1.00x | inside the spread |

| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s1 phase load-exchanges | 3,429.5 qps | 34.8 % | 1,596.3 qps | 50.7 % | 0.47x | spread > 20 %, no claim |
| s1 phase load-symbols | 5,041.7 qps | 18.8 % | 5,332.1 qps | 43.7 % | 1.06x | spread > 20 %, no claim |
| s1 phase load-sessions | 47,507.1 qps | 3.1 % | 48,153.7 qps | 2.9 % | 1.01x | inside the spread |
| s1 phase load-bars | 45,136.1 qps | 1.3 % | 44,837.0 qps | 3.0 % | 0.99x | inside the spread |
| s1 phase load-stats | 46,749.7 qps | 1.4 % | 46,695.8 qps | 3.3 % | 1.00x | inside the spread |
| s1 phase load-models | 3,732.1 qps | 17.3 % | 4,474.1 qps | 28.1 % | 1.20x | spread > 20 %, no claim |
| s1 phase backtest-read | 1,309.2 qps | 4.3 % | 1,317.9 qps | 9.8 % | 1.01x | inside the spread |
| s1 phase result-insert | 4,805.1 qps | 8.2 % | 4,759.9 qps | 50.3 % | 0.99x | spread > 20 %, no claim |
| s1 phase backtest-replay | 1,324.4 qps | 6.1 % | 1,348.6 qps | 3.1 % | 1.02x | inside the spread |
| s1 phase compare-all | 139.2 qps | 13.4 % | 135.0 qps | 15.5 % | 0.97x | inside the spread |
| s1 phase compare-one | 1,504.3 qps | 7.1 % | 1,488.7 qps | 5.5 % | 0.99x | inside the spread |
| s1 phase read-bar-lookup | 24,336.0 qps | 4.9 % | 24,272.6 qps | 13.3 % | 1.00x | inside the spread |
| s1 phase read-bar-range | 1,732.0 qps | 3.9 % | 1,704.4 qps | 2.8 % | 0.98x | inside the spread |
| s1 phase read-symbol-history | 41.7 qps | 1.9 % | 41.7 qps | 1.7 % | 1.00x | inside the spread |
| s1 phase read-day-slice | 1,346.6 qps | 7.7 % | 1,342.4 qps | 1.9 % | 1.00x | inside the spread |
| s1 phase read-join-point | 24,736.9 qps | 8.8 % | 23,581.2 qps | 17.3 % | 0.95x | inside the spread |
| s1 phase read-join-exists | 479.9 qps | 3.3 % | 478.0 qps | 2.5 % | 1.00x | inside the spread |
| s1 phase agg-global | 388.8 qps | 7.3 % | 384.7 qps | 6.4 % | 0.99x | inside the spread |
| s1 phase agg-by-symbol | 255.1 qps | 7.9 % | 253.8 qps | 13.9 % | 0.99x | inside the spread |
| s1 phase agg-by-session | 79.7 qps | 1.5 % | 79.3 qps | 3.3 % | 0.99x | inside the spread |
| s1 phase agg-day-slice | 1,389.7 qps | 8.0 % | 1,378.7 qps | 4.2 % | 0.99x | inside the spread |
| s1 phase agg-distinct | 371.7 qps | 4.5 % | 359.8 qps | 3.1 % | 0.97x | inside the spread |
| s1 phase sync | 527.9 qps | 35.6 % | 440.6 qps | 31.0 % | 0.83x | spread > 20 %, no claim |

## 8. What the run says about the engine

- **No change between `07f4552a` and `9f8f8b354` on scenario 1 where the pinned A/B resolves**: ranges, walks, the three-relation join, the model join, the Cabin-served `symbol-history` and `model-join`, the aggregates and the bulk loads read 0.95x to 1.02x, 12 of the 18 sweep rows and 18 of the 23 phases inside their spreads and none marked.
- **Not resolved, and not read**: the statements under about 100 µs (`bar-lookup` and `point-join`, cold and warm, new-arm spreads 28.9 % to 66.9 %) and the two Cabin-served shapes above 10,000 qps (`day-slice` cabin 0.91x, `cross-join` cabin 0.85x, new-arm spreads 52.7 % and 20.3 %; the unpinned series reads them 1.02x and 0.98x). A row over 20 % carries no claim by this directory's rule; five pairs do not settle the two Cabin rows either way.
- Also not resolved: `result-insert` (0.99x, the new arm's five runs spread 50.3 %), and the phases of one to eight statements (`load-exchanges`, `load-symbols`, `load-models`, `sync`).
- The ratios against the `v3.5.0` run (1.49x to 2.48x on the warm shapes, 2.57x on `result-insert`, 2.11x on `load-bars`) price this workstation against that VM, not the 142 commits between the two.
- **The unpinned series marks six rows, and the pinned series confirms none.** `cross-join` cold 1.05x, `read-day-slice` 1.24x, `read-join-point` 1.35x, `read-join-exists` 1.38x, `agg-global` 1.51x and `agg-by-symbol` 1.41x unpinned read 1.02x, 1.00x, 0.95x, 1.00x, 0.99x and 0.99x pinned, each inside its spread. Three runs an arm do not sample where an unpinned server's thread runs on this host, so a tight unpinned spread is not a small error; the marks are reported and not read as engine changes.

## 9. Noise floor, contamination and host

- **The host is a hybrid CPU** (Intel Core Ultra 7 270K Plus: 8 P-cores at up to 5.4 to 5.5 GHz, 16 E-cores at up to 4.7 GHz, `intel_pstate` active, governor `powersave`), and nothing in the unpinned cells pins a thread. At `c1-g` the unpinned warm spreads are 2 to 14 times the `v3.5.0` run's (median 22.7 % here against 4.9 % there; `point-join` 79.1 %); at `c1-s` and `c8-g` some shapes spread no more than they did there.
- **Another session was building and testing in this repository throughout** (worktree `dt-write-path-yield`: `cmake`, `cc1plus`, `ld`, `ctest`, `kds_tests`). The quiet gate held a cell's start while such a process ran; a process that started inside a cell is in the monitor log. Two scenario 1 cells carry foreign samples of 50 % or more of one CPU and are kept and disclosed: `s1-c8-g` run 1 (`cc1plus` and `ld`, 5 of its 8 samples, 21:06:08 to 21:06:26 UTC) and `ab-new1-s1-c1-g` (`cc1plus`, 2 of its 9 samples, 21:20:34 to 21:20:53 UTC). No pinned A/B cell of scenario 1 carries such a sample.
- Gate waits on scenario 1 cells: `s1-c1-g` 9 s, `s1-c1-s` 34 s, `ab-old2-s1-c1-g` 315 s (the hold that prompted the gate's change, summary section Quiet gate); every other scenario 1 gate 0 s; pinned A/B: `pin-ab-old1-s1-c1-g` 40 s, the rest 0 s.

## 10. Archive

`bench/v3.5.0/archive/scenario1-v3.4.17-159-g9f8f8b354/` (per-cell JSON, driver stdout, prechecks, gate waits, start and end times, server configs and logs; the A/B cells `ab-*` and the pinned ones `pin-ab-*` among them) and `bench/v3.5.0/archive/run-v3.4.17-159-g9f8f8b354/` (the run scripts, watchdogs, monitor log, status timeline, binary hashes, the table generator `gen.py` and its output under `tables/`). No data file is kept.
