### per-statement components, median of 3 runs (µs per statement)

| shape | rows returned | client path | stmts per run | wall | spread | client CPU | idle (wall - client CPU) | idle / wall | server CPU | idle / server CPU |
|---|---|---|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | send | 9451 | 85.1 µs | 3.6 % | 51.7 µs | 32.7 µs | 38.4 % | 32.3 µs | 1.01 |
| range-1 | 1 rows | split | 10521 | 76.7 µs | 2.2 % | 43.8 µs | 32.3 µs | 42.1 % | 32.3 µs | 1.00 |
| range-1 | 1 rows | drain | 13669 | 48.5 µs | 2.4 % | 16.9 µs | 31.8 µs | 65.7 % | 32.0 µs | 1.00 |
| range-100 | 100 rows | send | 3473 | 548.3 µs | 3.0 % | 464.1 µs | 81.3 µs | 14.8 % | 83.5 µs | 0.97 |
| range-100 | 100 rows | split | 5707 | 324.5 µs | 0.6 % | 245.2 µs | 79.3 µs | 24.4 % | 78.9 µs | 1.01 |
| range-100 | 100 rows | drain | 11360 | 93.8 µs | 3.6 % | 17.6 µs | 75.5 µs | 80.5 % | 75.6 µs | 1.00 |
| range-1000 | 1,000 rows | send | 603 | 4,684.5 µs | 0.7 % | 4,204.8 µs | 476.1 µs | 10.2 % | 495.0 µs | 0.96 |
| range-1000 | 1,000 rows | split | 1025 | 2,612.1 µs | 1.4 % | 2,150.7 µs | 456.1 µs | 17.5 % | 478.2 µs | 0.95 |
| range-1000 | 1,000 rows | drain | 4220 | 494.1 µs | 2.6 % | 39.0 µs | 455.1 µs | 92.1 % | 470.4 µs | 0.97 |
| range-10000 | 10,000 rows | send | 57 | 47,662.6 µs | 3.4 % | 43,415.4 µs | 4,247.2 µs | 8.9 % | 4,833.3 µs | 0.88 |
| range-10000 | 10,000 rows | split | 112 | 26,230.7 µs | 0.8 % | 22,118.1 µs | 4,167.6 µs | 15.9 % | 4,513.3 µs | 0.92 |
| range-10000 | 10,000 rows | drain | 637 | 4,275.1 µs | 1.0 % | 209.8 µs | 4,065.3 µs | 95.1 % | 4,250.4 µs | 0.96 |
| range-60480 | 60,480 rows | send | 9 | 318,902.8 µs | 0.6 % | 291,873.8 µs | 27,060.7 µs | 8.5 % | 30,000.0 µs | 0.90 |
| range-60480 | 60,480 rows | split | 16 | 186,440.3 µs | 1.7 % | 159,189.2 µs | 27,218.7 µs | 14.6 % | 29,375.0 µs | 0.93 |
| range-60480 | 60,480 rows | drain | 103 | 28,449.7 µs | 0.2 % | 1,216.9 µs | 27,236.7 µs | 95.7 % | 28,446.6 µs | 0.96 |
| filter-sym3 | 7,560 rows | send | 80 | 36,691.1 µs | 2.3 % | 31,913.4 µs | 4,782.6 µs | 13.0 % | 5,068.5 µs | 0.94 |
| filter-sym3 | 7,560 rows | split | 141 | 20,904.3 µs | 1.2 % | 16,287.6 µs | 4,616.7 µs | 22.1 % | 4,822.7 µs | 0.96 |
| filter-sym3 | 7,560 rows | drain | 562 | 4,546.5 µs | 1.6 % | 164.1 µs | 4,382.4 µs | 96.4 % | 4,531.0 µs | 0.97 |

### per returned row (µs per row), range shapes: marginal = (cost(size) - cost(1 row)) / (size - 1); slope = least-squares over the sizes named

| client path | quantity | marginal 100 | marginal 1,000 | marginal 10,000 | marginal 60,480 | slope, sizes 1..10,000 | slope, sizes 1..60,480 | intercept (fit 1..60,480) |
|---|---|---|---|---|---|---|---|---|
| send | wall | 4.679 µs/row | 4.604 µs/row | 4.758 µs/row | 5.272 µs/row | 4.762 µs/row | 5.283 µs/row | -1,258.7 µs |
| send | client CPU | 4.166 µs/row | 4.157 µs/row | 4.337 µs/row | 4.825 µs/row | 4.342 µs/row | 4.836 µs/row | -1,238.0 µs |
| send | idle | 0.491 µs/row | 0.444 µs/row | 0.421 µs/row | 0.447 µs/row | 0.421 µs/row | 0.447 µs/row | -23.9 µs |
| send | server CPU | 0.517 µs/row | 0.463 µs/row | 0.480 µs/row | 0.496 µs/row | 0.480 µs/row | 0.496 µs/row | -10.5 µs |
| split | wall | 2.503 µs/row | 2.538 µs/row | 2.616 µs/row | 3.081 µs/row | 2.618 µs/row | 3.092 µs/row | -1,125.3 µs |
| split | client CPU | 2.034 µs/row | 2.109 µs/row | 2.208 µs/row | 2.631 µs/row | 2.211 µs/row | 2.641 µs/row | -1,059.0 µs |
| split | idle | 0.475 µs/row | 0.424 µs/row | 0.414 µs/row | 0.450 µs/row | 0.413 µs/row | 0.450 µs/row | -54.3 µs |
| split | server CPU | 0.470 µs/row | 0.446 µs/row | 0.448 µs/row | 0.485 µs/row | 0.448 µs/row | 0.486 µs/row | -61.5 µs |
| drain | wall | 0.458 µs/row | 0.446 µs/row | 0.423 µs/row | 0.470 µs/row | 0.422 µs/row | 0.470 µs/row | -63.0 µs |
| drain | client CPU | 0.007 µs/row | 0.022 µs/row | 0.019 µs/row | 0.020 µs/row | 0.019 µs/row | 0.020 µs/row | 15.9 µs |
| drain | idle | 0.441 µs/row | 0.424 µs/row | 0.403 µs/row | 0.450 µs/row | 0.403 µs/row | 0.451 µs/row | -79.3 µs |
| drain | server CPU | 0.441 µs/row | 0.439 µs/row | 0.422 µs/row | 0.470 µs/row | 0.421 µs/row | 0.471 µs/row | -84.3 µs |

### the filtered walk: WHERE symbol_id = 3 (60,480 rows scanned, 7,560 returned)

| client path | wall | client CPU | idle | idle / wall | server CPU | idle / server CPU | server CPU per returned row | server CPU per scanned row | idle per returned row |
|---|---|---|---|---|---|---|---|---|---|
| send | 36,691.1 µs | 31,913.4 µs | 4,782.6 µs | 13.0 % | 5,068.5 µs | 0.94 | 0.670 µs/row | 0.084 µs/row | 0.633 µs/row |
| split | 20,904.3 µs | 16,287.6 µs | 4,616.7 µs | 22.1 % | 4,822.7 µs | 0.96 | 0.638 µs/row | 0.080 µs/row | 0.611 µs/row |
| drain | 4,546.5 µs | 164.1 µs | 4,382.4 µs | 96.4 % | 4,531.0 µs | 0.97 | 0.599 µs/row | 0.075 µs/row | 0.580 µs/row |

### per-statement latency distribution, `send` path (the scenario drivers' own), all statements of the 3 runs pooled

| shape | rows returned | statements | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| range-1 (send) | 1 rows | 28,030 | 78.3 µs | 81.2 µs | 82.5 µs | 99.3 µs | 111.1 µs |
| range-1 (drain) | 1 rows | 54,895 | 44.6 µs | 46.2 µs | 46.9 µs | 58.6 µs | 67.8 µs |
| range-100 (send) | 100 rows | 10,937 | 514.6 µs | 530.2 µs | 536.2 µs | 572.6 µs | 656.7 µs |
| range-100 (drain) | 100 rows | 45,013 | 85.3 µs | 89.0 µs | 91.8 µs | 108.7 µs | 122.3 µs |
| range-1000 (send) | 1,000 rows | 1,805 | 4,533.2 µs | 4,623.8 µs | 4,660.9 µs | 4,795.5 µs | 5,678.5 µs |
| range-1000 (drain) | 1,000 rows | 14,072 | 468.1 µs | 482.1 µs | 486.1 µs | 518.0 µs | 764.1 µs |
| range-10000 (send) | 10,000 rows | 172 | 45,561.8 µs | 46,729.6 µs | 47,523.6 µs | 49,639.8 µs | 51,152.8 µs |
| range-10000 (drain) | 10,000 rows | 1,879 | 4,115.2 µs | 4,200.9 µs | 4,218.4 µs | 4,486.5 µs | 5,147.7 µs |
| range-60480 (send) | 60,480 rows | 27 | 310,093.9 µs | 314,728.7 µs | 316,197.3 µs | 329,953.4 µs | 335,152.6 µs |
| range-60480 (drain) | 60,480 rows | 309 | 27,886.0 µs | 28,152.6 µs | 28,294.4 µs | 29,095.8 µs | 32,337.9 µs |
| filter-sym3 (send) | 7,560 rows | 234 | 35,340.1 µs | 36,008.2 µs | 36,361.7 µs | 38,414.5 µs | 40,731.3 µs |
| filter-sym3 (drain) | 7,560 rows | 1,679 | 4,414.2 µs | 4,472.1 µs | 4,534.1 µs | 4,634.9 µs | 4,835.3 µs |

### time to first byte, first S_ROW_BATCH and end of reply (µs from send; client pinned CPU 6, server CPU 2)

| shape | rows returned | reps | batches | bytes | event | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | 100 | 1 | 476 B | first_byte | 44.5 µs | 45.4 µs | 45.7 µs | 51.8 µs | 61.4 µs |
| | | | | | row_desc | 44.5 µs | 45.4 µs | 45.7 µs | 51.8 µs | 61.4 µs |
| | | | | | first_batch | 44.5 µs | 45.4 µs | 45.7 µs | 51.8 µs | 61.4 µs |
| | | | | | last_batch | 44.5 µs | 45.4 µs | 45.7 µs | 51.8 µs | 61.4 µs |
| | | | | | ready | 44.5 µs | 45.4 µs | 45.7 µs | 51.8 µs | 61.4 µs |
| range-100 | 100 rows | 100 | 1 | 14,336 B | first_byte | 84.8 µs | 86.6 µs | 89.5 µs | 102.9 µs | 108.6 µs |
| | | | | | row_desc | 84.8 µs | 86.6 µs | 89.5 µs | 102.9 µs | 108.6 µs |
| | | | | | first_batch | 84.8 µs | 86.6 µs | 89.5 µs | 102.9 µs | 108.6 µs |
| | | | | | last_batch | 84.8 µs | 86.6 µs | 89.5 µs | 102.9 µs | 108.6 µs |
| | | | | | ready | 84.8 µs | 86.6 µs | 89.5 µs | 102.9 µs | 108.6 µs |
| range-1000 | 1,000 rows | 100 | 3 | 140,356 B | first_byte | 452.6 µs | 457.0 µs | 465.0 µs | 480.0 µs | 493.1 µs |
| | | | | | row_desc | 452.6 µs | 457.0 µs | 465.0 µs | 480.0 µs | 493.1 µs |
| | | | | | first_batch | 468.4 µs | 473.2 µs | 482.1 µs | 496.7 µs | 508.0 µs |
| | | | | | last_batch | 471.8 µs | 483.7 µs | 489.8 µs | 506.5 µs | 513.4 µs |
| | | | | | ready | 471.8 µs | 483.7 µs | 489.8 µs | 506.5 µs | 513.4 µs |
| range-10000 | 10,000 rows | 100 | 22 | 1,400,546 B | first_byte | 3,926.4 µs | 3,944.0 µs | 3,953.9 µs | 4,012.5 µs | 4,066.5 µs |
| | | | | | row_desc | 3,926.4 µs | 3,944.0 µs | 3,953.9 µs | 4,012.5 µs | 4,066.5 µs |
| | | | | | first_batch | 3,944.3 µs | 3,962.4 µs | 3,972.1 µs | 4,025.6 µs | 4,087.5 µs |
| | | | | | last_batch | 4,133.0 µs | 4,159.6 µs | 4,171.3 µs | 4,269.3 µs | 4,547.6 µs |
| | | | | | ready | 4,133.0 µs | 4,159.6 µs | 4,171.3 µs | 4,269.3 µs | 4,547.6 µs |
| range-60480 | 60,480 rows | 100 | 130 | 8,468,826 B | first_byte | 25,945.7 µs | 26,242.5 µs | 26,356.5 µs | 27,222.9 µs | 28,670.5 µs |
| | | | | | row_desc | 25,945.7 µs | 26,242.5 µs | 26,356.5 µs | 27,222.9 µs | 28,670.5 µs |
| | | | | | first_batch | 25,945.7 µs | 26,242.5 µs | 26,356.5 µs | 27,226.1 µs | 28,670.5 µs |
| | | | | | last_batch | 27,482.3 µs | 27,742.9 µs | 27,849.2 µs | 28,998.6 µs | 30,294.0 µs |
| | | | | | ready | 27,482.3 µs | 27,742.9 µs | 27,849.2 µs | 28,998.6 µs | 30,294.0 µs |
| filter-symbol-3 | 7,560 rows | 100 | 17 | 1,058,896 B | first_byte | 4,258.0 µs | 4,276.5 µs | 4,295.2 µs | 4,369.3 µs | 4,618.8 µs |
| | | | | | row_desc | 4,258.0 µs | 4,276.5 µs | 4,295.2 µs | 4,369.3 µs | 4,618.8 µs |
| | | | | | first_batch | 4,277.6 µs | 4,297.1 µs | 4,315.1 µs | 4,384.0 µs | 4,639.2 µs |
| | | | | | last_batch | 4,406.6 µs | 4,430.3 µs | 4,448.9 µs | 4,510.6 µs | 4,793.1 µs |
| | | | | | ready | 4,406.6 µs | 4,430.3 µs | 4,448.9 µs | 4,510.6 µs | 4,793.1 µs |

### first batch against end of reply (medians)

| shape | rows returned | first byte | first batch | reply complete (ready) | first batch / ready | ready - first batch | recv calls |
|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | 45.7 µs | 45.7 µs | 45.7 µs | 100.0 % | 0.0 µs | 1 |
| range-100 | 100 rows | 89.5 µs | 89.5 µs | 89.5 µs | 100.0 % | 0.0 µs | 1 |
| range-1000 | 1,000 rows | 465.0 µs | 482.1 µs | 489.8 µs | 98.4 % | 7.7 µs | 2 |
| range-10000 | 10,000 rows | 3,953.9 µs | 3,972.1 µs | 4,171.3 µs | 95.2 % | 199.2 µs | 9 |
| range-60480 | 60,480 rows | 26,356.5 µs | 26,356.5 µs | 27,849.2 µs | 94.6 % | 1,492.7 µs | 58 |
| filter-symbol-3 | 7,560 rows | 4,295.2 µs | 4,315.1 µs | 4,448.9 µs | 97.0 % | 133.8 µs | 7 |

### scenario 1, cores = 1, group (driver-reported)

matrix (warm = same 8 arguments cycled, 100 statements; cold = each argument first seen):

| shape | rows returned | warm QPS run 1 | run 2 | run 3 | median | spread | cold QPS median | warm mean (median of runs) | warm p50 (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| bar-range | 200 rows | 1,257.2 qps | 1,263.3 qps | 1,239.6 qps | 1,257.2 qps | 1.9 % | 1,262.4 qps | 795.1 µs | 793.9 µs |
| symbol-history | 7,560 rows | 27.3 qps | 27.3 qps | 27.5 qps | 27.3 qps | 0.7 % | 27.7 qps | 36,598.0 µs | 36,343.2 µs |
| model-join | 359 rows | 1,051.0 qps | 1,042.8 qps | 1,059.3 qps | 1,051.0 qps | 1.6 % | 1,035.8 qps | 951.1 µs | 941.1 µs |
| day-slice | 8 rows | 589.6 qps | 590.1 qps | 602.6 qps | 590.1 qps | 2.2 % | 599.3 qps | 1,694.2 µs | 1,673.1 µs |

phase distributions (the driver's own phases; p0 p25 p50 p95 p99 over each run's statements, run 1 / 2 / 3 medians of each percentile):

| phase | rows returned | runs | ops per run | p0 | p25 | p50 | p95 | p99 | QPS from mean (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| read-bar-range | 200 rows | 3 | 200 | 763.9 µs | 778.1 µs | 784.1 µs | 817.7 µs | 829.8 µs | 1,267.1 qps |
| read-symbol-history | 7,560 rows | 3 | 10 | 35,570.0 µs | 35,909.6 µs | 36,108.9 µs | 39,414.5 µs | 39,414.5 µs | 27.1 qps |
| read-day-slice | 8 rows | 3 | 200 | 1,618.1 µs | 1,657.3 µs | 1,666.3 µs | 1,734.8 µs | 1,862.2 µs | 598.3 qps |

### scenario 3, 10,000 loans, unindexed, cores = 1, group (driver-reported)

| shape | runs | ops per run | QPS run 1 (derived 1e6 / mean) | run 2 | run 3 | median | spread | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| books-by-genre | 3 | 200 | 3,096 qps | 3,135 qps | 3,125 qps | 3,125 qps | 1.2 % | 285.3 µs | 306.0 µs | 316.4 µs | 348.0 µs | 441.7 µs |
| loans-by-daterange | 3 | 200 | 946 qps | 987 qps | 1,000 qps | 987 qps | 5.5 % | 394.2 µs | 993.2 µs | 1,021.4 µs | 1,095.4 µs | 1,157.7 µs |
| overdue | 3 | 200 | 1,176 qps | 1,232 qps | 1,241 qps | 1,232 qps | 5.4 % | 380.2 µs | 805.6 µs | 829.1 µs | 888.4 µs | 937.2 µs |
| loans-by-user | 3 | 200 | 2,875 qps | 2,846 qps | 2,950 qps | 2,875 qps | 3.6 % | 325.1 µs | 334.4 µs | 342.7 µs | 374.9 µs | 394.0 µs |
