## Row-cost sweep, per statement, median of the runs (A = base, B = CE)

Runs per binary: {'A': 3, 'B': 3} 

### `send` path

| shape | rows returned | quantity | A | B | B over A | spread A / B | a finding? |
|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | wall | 83.8 µs | 84.3 µs | +0.6 % | 2.9 % / 2.3 % | no (inside the spread) |
| range-1 | 1 rows | client CPU | 51.0 µs | 51.7 µs | +1.3 % | 4.7 % / 1.2 % | no (inside the spread) |
| range-1 | 1 rows | idle (wall - client CPU) | 32.6 µs | 32.6 µs | +0.2 % | 0.9 % / 4.2 % | no (inside the spread) |
| range-1 | 1 rows | server CPU | 32.8 µs | 31.0 µs | -5.7 % | 4.3 % / 6.6 % | no (inside the spread) |
| range-100 | 100 rows | wall | 541.5 µs | 537.5 µs | -0.7 % | 1.4 % / 1.5 % | no (inside the spread) |
| range-100 | 100 rows | client CPU | 459.7 µs | 456.5 µs | -0.7 % | 0.3 % / 0.9 % | no (inside the spread) |
| range-100 | 100 rows | idle (wall - client CPU) | 82.3 µs | 81.0 µs | -1.6 % | 9.6 % / 5.2 % | no (inside the spread) |
| range-100 | 100 rows | server CPU | 85.1 µs | 81.6 µs | -4.2 % | 8.8 % / 6.2 % | no (inside the spread) |
| range-1000 | 1,000 rows | wall | 4,684.1 µs | 4,743.5 µs | +1.3 % | 2.6 % / 2.1 % | no (inside the spread) |
| range-1000 | 1,000 rows | client CPU | 4,239.9 µs | 4,267.1 µs | +0.6 % | 2.4 % / 2.2 % | no (inside the spread) |
| range-1000 | 1,000 rows | idle (wall - client CPU) | 451.2 µs | 472.4 µs | +4.7 % | 6.2 % / 1.7 % | no (inside the spread) |
| range-1000 | 1,000 rows | server CPU | 488.8 µs | 485.8 µs | -0.6 % | 8.9 % / 10.4 % | no (inside the spread) |
| range-10000 | 10,000 rows | wall | 48,421.1 µs | 45,506.3 µs | -6.0 % | 3.0 % / 0.2 % | **yes** |
| range-10000 | 10,000 rows | client CPU | 44,030.9 µs | 43,467.2 µs | -1.3 % | 3.0 % / 0.2 % | no (inside the spread) |
| range-10000 | 10,000 rows | idle (wall - client CPU) | 4,232.6 µs | 1,964.8 µs | -53.6 % | 7.8 % / 5.0 % | **yes** |
| range-10000 | 10,000 rows | server CPU | 4,680.9 µs | 4,893.6 µs | +4.5 % | 6.6 % / 8.2 % | no (inside the spread) |
| range-60480 | 60,480 rows | wall | 316,102.5 µs | 293,937.1 µs | -7.0 % | 1.3 % / 1.2 % | **yes** |
| range-60480 | 60,480 rows | client CPU | 289,330.6 µs | 291,624.2 µs | +0.8 % | 0.9 % / 1.3 % | no (inside the spread) |
| range-60480 | 60,480 rows | idle (wall - client CPU) | 26,831.6 µs | 2,274.7 µs | -91.5 % | 6.6 % / 6.5 % | **yes** |
| range-60480 | 60,480 rows | server CPU | 28,571.4 µs | 28,750.0 µs | +0.6 % | 15.0 % / 4.3 % | no (inside the spread) |
| filter-sym3 | 7,560 rows | wall | 36,239.9 µs | 31,877.6 µs | -12.0 % | 2.1 % / 2.2 % | **yes** |
| filter-sym3 | 7,560 rows | client CPU | 31,472.0 µs | 31,434.7 µs | -0.1 % | 1.9 % / 2.2 % | no (inside the spread) |
| filter-sym3 | 7,560 rows | idle (wall - client CPU) | 4,783.6 µs | 442.9 µs | -90.7 % | 3.8 % / 3.4 % | **yes** |
| filter-sym3 | 7,560 rows | server CPU | 5,074.6 µs | 5,063.3 µs | -0.2 % | 6.0 % / 2.6 % | no (inside the spread) |

### `split` path

| shape | rows returned | quantity | A | B | B over A | spread A / B | a finding? |
|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | wall | 75.2 µs | 76.6 µs | +1.8 % | 2.6 % / 3.8 % | no (inside the spread) |
| range-1 | 1 rows | client CPU | 42.7 µs | 43.8 µs | +2.6 % | 3.1 % / 2.4 % | no (inside the spread) |
| range-1 | 1 rows | idle (wall - client CPU) | 32.5 µs | 32.8 µs | +0.9 % | 3.7 % / 5.6 % | no (inside the spread) |
| range-1 | 1 rows | server CPU | 32.0 µs | 32.0 µs | -0.3 % | 5.8 % / 11.0 % | no (inside the spread) |
| range-100 | 100 rows | wall | 325.0 µs | 325.4 µs | +0.1 % | 4.3 % / 0.4 % | no (inside the spread) |
| range-100 | 100 rows | client CPU | 244.3 µs | 244.0 µs | -0.1 % | 1.5 % / 1.7 % | no (inside the spread) |
| range-100 | 100 rows | idle (wall - client CPU) | 81.2 µs | 81.4 µs | +0.2 % | 13.5 % / 3.6 % | no (inside the spread) |
| range-100 | 100 rows | server CPU | 80.6 µs | 80.3 µs | -0.4 % | 8.9 % / 6.9 % | no (inside the spread) |
| range-1000 | 1,000 rows | wall | 2,659.2 µs | 2,641.5 µs | -0.7 % | 1.9 % / 1.3 % | no (inside the spread) |
| range-1000 | 1,000 rows | client CPU | 2,196.1 µs | 2,186.7 µs | -0.4 % | 2.5 % / 1.8 % | no (inside the spread) |
| range-1000 | 1,000 rows | idle (wall - client CPU) | 440.1 µs | 458.5 µs | +4.2 % | 6.9 % / 1.7 % | no (inside the spread) |
| range-1000 | 1,000 rows | server CPU | 464.6 µs | 480.5 µs | +3.4 % | 7.4 % / 1.1 % | no (inside the spread) |
| range-10000 | 10,000 rows | wall | 26,605.8 µs | 23,871.6 µs | -10.3 % | 1.2 % / 0.8 % | **yes** |
| range-10000 | 10,000 rows | client CPU | 22,309.0 µs | 21,993.7 µs | -1.4 % | 1.0 % / 0.9 % | **yes** |
| range-10000 | 10,000 rows | idle (wall - client CPU) | 4,132.6 µs | 1,878.3 µs | -54.5 % | 6.6 % / 0.7 % | **yes** |
| range-10000 | 10,000 rows | server CPU | 4,456.5 µs | 4,455.4 µs | -0.0 % | 7.2 % / 1.2 % | no (inside the spread) |
| range-60480 | 60,480 rows | wall | 187,348.7 µs | 160,841.3 µs | -14.1 % | 1.3 % / 1.7 % | **yes** |
| range-60480 | 60,480 rows | client CPU | 159,042.4 µs | 158,644.7 µs | -0.3 % | 1.8 % / 1.7 % | no (inside the spread) |
| range-60480 | 60,480 rows | idle (wall - client CPU) | 27,374.3 µs | 2,196.6 µs | -92.0 % | 6.4 % / 1.9 % | **yes** |
| range-60480 | 60,480 rows | server CPU | 30,000.0 µs | 28,000.0 µs | -6.7 % | 7.7 % / 0.4 % | no (inside the spread) |
| filter-sym3 | 7,560 rows | wall | 20,884.4 µs | 16,565.5 µs | -20.7 % | 1.7 % / 0.4 % | **yes** |
| filter-sym3 | 7,560 rows | client CPU | 16,203.1 µs | 16,147.1 µs | -0.3 % | 1.6 % / 0.5 % | no (inside the spread) |
| filter-sym3 | 7,560 rows | idle (wall - client CPU) | 4,586.3 µs | 401.5 µs | -91.2 % | 4.3 % / 5.5 % | **yes** |
| filter-sym3 | 7,560 rows | server CPU | 4,827.6 µs | 4,802.6 µs | -0.5 % | 5.2 % / 1.3 % | no (inside the spread) |

## Hidden fraction against CE-S0's ceiling

Ceiling = A's idle x (1 - 1 / slices) (CE-S0 section 6), as a share of A's wall. Measured = (A wall - B wall) / A wall, medians. Idle removed = A idle - B idle.

| path | shape | rows returned | slices (CE-S0 fractional) | A wall | B wall | measured hidden | measured, share of A wall | ceiling, share of A wall | wall hidden / ceiling | A idle | B idle | idle removed | idle removed / ceiling | spread of wall A / B |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| send | range-1 | 1 rows | 1 | 83.8 µs | 84.3 µs | -0.5 µs | -0.6 % | 0.0 % | n/a (no boundary) | 32.6 µs | 32.6 µs | -0.1 µs | n/a | 2.9 % / 2.3 % |
| send | range-100 | 100 rows | 1 | 541.5 µs | 537.5 µs | 4.0 µs | +0.7 % | 0.0 % | n/a (no boundary) | 82.3 µs | 81.0 µs | 1.3 µs | n/a | 1.4 % / 1.5 % |
| send | range-1000 | 1,000 rows | 1 | 4,684.1 µs | 4,743.5 µs | -59.4 µs | -1.3 % | 0.0 % | n/a (no boundary) | 451.2 µs | 472.4 µs | -21.2 µs | n/a | 2.6 % / 2.1 % |
| send | range-10000 | 10,000 rows | 2.28 | 48,421.1 µs | 45,506.3 µs | 2,914.8 µs | +6.0 % | 4.9 % | 1.23 | 4,232.6 µs | 1,964.8 µs | 2,267.8 µs | 0.95 | 3.0 % / 0.2 % |
| send | range-60480 | 60,480 rows | 13.7 | 316,102.5 µs | 293,937.1 µs | 22,165.4 µs | +7.0 % | 7.9 % | 0.89 | 26,831.6 µs | 2,274.7 µs | 24,556.9 µs | 0.99 | 1.3 % / 1.2 % |
| send | filter-sym3 | 7,560 rows | 13.7 | 36,239.9 µs | 31,877.6 µs | 4,362.3 µs | +12.0 % | 12.2 % | 0.98 | 4,783.6 µs | 442.9 µs | 4,340.7 µs | 0.98 | 2.1 % / 2.2 % |
| split | range-1 | 1 rows | 1 | 75.2 µs | 76.6 µs | -1.4 µs | -1.8 % | 0.0 % | n/a (no boundary) | 32.5 µs | 32.8 µs | -0.3 µs | n/a | 2.6 % / 3.8 % |
| split | range-100 | 100 rows | 1 | 325.0 µs | 325.4 µs | -0.4 µs | -0.1 % | 0.0 % | n/a (no boundary) | 81.2 µs | 81.4 µs | -0.2 µs | n/a | 4.3 % / 0.4 % |
| split | range-1000 | 1,000 rows | 1 | 2,659.2 µs | 2,641.5 µs | 17.7 µs | +0.7 % | 0.0 % | n/a (no boundary) | 440.1 µs | 458.5 µs | -18.4 µs | n/a | 1.9 % / 1.3 % |
| split | range-10000 | 10,000 rows | 2.28 | 26,605.8 µs | 23,871.6 µs | 2,734.2 µs | +10.3 % | 8.7 % | 1.18 | 4,132.6 µs | 1,878.3 µs | 2,254.3 µs | 0.97 | 1.2 % / 0.8 % |
| split | range-60480 | 60,480 rows | 13.7 | 187,348.7 µs | 160,841.3 µs | 26,507.4 µs | +14.1 % | 13.5 % | 1.04 | 27,374.3 µs | 2,196.6 µs | 25,177.7 µs | 0.99 | 1.3 % / 1.7 % |
| split | filter-sym3 | 7,560 rows | 13.7 | 20,884.4 µs | 16,565.5 µs | 4,319.0 µs | +20.7 % | 20.4 % | 1.02 | 4,586.3 µs | 401.5 µs | 4,184.8 µs | 0.98 | 1.7 % / 0.4 % |

## Per-statement latency distribution (all statements of the runs pooled)

| path | shape | rows returned | binary | statements | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|
| send | range-1 | 1 rows | A | 31,430 | 77.9 µs | 80.5 µs | 81.7 µs | 99.0 µs | 107.3 µs |
| send | range-1 | 1 rows | B | 30,051 | 77.5 µs | 80.1 µs | 81.0 µs | 97.9 µs | 109.8 µs |
| send | range-100 | 100 rows | A | 8,326 | 517.0 µs | 531.2 µs | 536.1 µs | 565.2 µs | 709.7 µs |
| send | range-100 | 100 rows | B | 8,471 | 513.3 µs | 528.6 µs | 533.1 µs | 554.2 µs | 601.8 µs |
| send | range-1000 | 1,000 rows | A | 1,480 | 4,566.2 µs | 4,639.5 µs | 4,675.9 µs | 4,858.1 µs | 6,271.2 µs |
| send | range-1000 | 1,000 rows | B | 1,455 | 4,589.6 µs | 4,658.4 µs | 4,691.8 µs | 4,956.5 µs | 6,491.1 µs |
| send | range-10000 | 10,000 rows | A | 142 | 45,990.1 µs | 46,991.3 µs | 47,845.6 µs | 50,446.7 µs | 51,432.5 µs |
| send | range-10000 | 10,000 rows | B | 147 | 43,781.5 µs | 44,888.4 µs | 45,267.3 µs | 47,238.2 µs | 47,958.6 µs |
| send | range-60480 | 60,480 rows | A | 21 | 308,948.4 µs | 312,946.2 µs | 315,451.6 µs | 326,046.8 µs | 329,798.6 µs |
| send | range-60480 | 60,480 rows | B | 24 | 285,066.8 µs | 290,790.9 µs | 292,606.2 µs | 304,195.2 µs | 305,776.3 µs |
| send | filter-sym3 | 7,560 rows | A | 199 | 35,275.0 µs | 35,801.9 µs | 36,104.3 µs | 37,920.0 µs | 38,467.5 µs |
| send | filter-sym3 | 7,560 rows | B | 231 | 31,016.3 µs | 31,491.8 µs | 31,882.4 µs | 33,419.8 µs | 34,364.5 µs |
| split | range-1 | 1 rows | A | 26,683 | 68.0 µs | 72.2 µs | 73.0 µs | 87.6 µs | 98.0 µs |
| split | range-1 | 1 rows | B | 27,189 | 69.6 µs | 72.0 µs | 72.9 µs | 90.7 µs | 103.0 µs |
| split | range-100 | 100 rows | A | 14,078 | 306.6 µs | 315.8 µs | 321.0 µs | 342.6 µs | 431.5 µs |
| split | range-100 | 100 rows | B | 13,979 | 306.7 µs | 316.3 µs | 321.7 µs | 341.0 µs | 374.6 µs |
| split | range-1000 | 1,000 rows | A | 2,546 | 2,518.2 µs | 2,599.2 µs | 2,628.3 µs | 2,696.8 µs | 3,053.7 µs |
| split | range-1000 | 1,000 rows | B | 2,535 | 2,541.7 µs | 2,592.8 µs | 2,612.1 µs | 2,683.4 µs | 3,334.5 µs |
| split | range-10000 | 10,000 rows | A | 278 | 25,263.6 µs | 26,077.3 µs | 26,387.9 µs | 28,196.2 µs | 29,387.0 µs |
| split | range-10000 | 10,000 rows | B | 306 | 22,706.3 µs | 23,426.5 µs | 23,651.0 µs | 25,469.6 µs | 25,945.6 µs |
| split | range-60480 | 60,480 rows | A | 39 | 179,120.2 µs | 182,755.0 µs | 184,609.4 µs | 197,341.2 µs | 197,714.5 µs |
| split | range-60480 | 60,480 rows | B | 46 | 153,796.0 µs | 156,207.2 µs | 158,618.3 µs | 169,519.2 µs | 171,234.0 µs |
| split | filter-sym3 | 7,560 rows | A | 354 | 19,932.3 µs | 20,438.5 µs | 20,692.5 µs | 22,359.4 µs | 23,157.9 µs |
| split | filter-sym3 | 7,560 rows | B | 452 | 15,928.9 µs | 16,272.8 µs | 16,429.0 µs | 18,111.6 µs | 18,611.9 µs |

## Time to first byte, first S_ROW_BATCH and end of reply (µs from send)

Each run's percentile over its own reps (100, fewer at 60,480 rows), then the median over the runs of that binary. Runs: {'A': 3, 'B': 3}

| shape | rows returned | batches (A / B) | event | binary | p0 | p25 | p50 | p95 | p99 | spread of p50 over runs |
|---|---|---|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | 1 / 1 | first_byte | A | 43.1 µs | 43.9 µs | 44.3 µs | 55.5 µs | 66.5 µs | 3.6 % |
| | | | first_byte | B | 43.1 µs | 43.9 µs | 44.5 µs | 56.1 µs | 66.6 µs | 1.9 % |
| | | | first_batch | A | 43.1 µs | 43.9 µs | 44.3 µs | 55.5 µs | 66.5 µs | 3.6 % |
| | | | first_batch | B | 43.1 µs | 43.9 µs | 44.5 µs | 56.1 µs | 66.6 µs | 1.9 % |
| | | | ready | A | 43.1 µs | 43.9 µs | 44.3 µs | 55.5 µs | 66.5 µs | 3.6 % |
| | | | ready | B | 43.1 µs | 43.9 µs | 44.5 µs | 56.1 µs | 66.6 µs | 1.9 % |
| range-100 | 100 rows | 1 / 1 | first_byte | A | 85.1 µs | 87.1 µs | 90.1 µs | 106.7 µs | 112.6 µs | 2.7 % |
| | | | first_byte | B | 85.1 µs | 86.9 µs | 89.8 µs | 109.6 µs | 113.0 µs | 0.9 % |
| | | | first_batch | A | 85.1 µs | 87.1 µs | 90.1 µs | 106.7 µs | 112.6 µs | 2.7 % |
| | | | first_batch | B | 85.1 µs | 86.9 µs | 89.8 µs | 109.6 µs | 113.0 µs | 0.9 % |
| | | | ready | A | 85.1 µs | 87.1 µs | 90.1 µs | 106.7 µs | 112.6 µs | 2.7 % |
| | | | ready | B | 85.1 µs | 86.9 µs | 89.8 µs | 109.6 µs | 113.0 µs | 0.9 % |
| range-1000 | 1,000 rows | 3 / 3 | first_byte | A | 434.1 µs | 439.8 µs | 454.2 µs | 469.6 µs | 485.5 µs | 5.6 % |
| | | | first_byte | B | 453.5 µs | 461.7 µs | 473.5 µs | 490.7 µs | 499.0 µs | 3.3 % |
| | | | first_batch | A | 449.3 µs | 454.6 µs | 470.9 µs | 485.7 µs | 503.6 µs | 5.4 % |
| | | | first_batch | B | 468.8 µs | 478.0 µs | 489.6 µs | 507.3 µs | 516.0 µs | 3.0 % |
| | | | ready | A | 453.1 µs | 460.6 µs | 472.3 µs | 491.3 µs | 503.6 µs | 5.7 % |
| | | | ready | B | 471.1 µs | 483.6 µs | 493.3 µs | 512.0 µs | 519.5 µs | 2.7 % |
| range-10000 | 10,000 rows | 22 / 22 | first_byte | A | 3,998.6 µs | 4,014.5 µs | 4,023.5 µs | 4,124.8 µs | 4,181.2 µs | 5.1 % |
| | | | first_byte | B | 1,788.7 µs | 1,806.3 µs | 1,834.0 µs | 1,867.4 µs | 1,904.6 µs | 1.9 % |
| | | | first_batch | A | 4,015.1 µs | 4,030.3 µs | 4,040.8 µs | 4,128.3 µs | 4,200.4 µs | 5.2 % |
| | | | first_batch | B | 1,803.4 µs | 1,820.5 µs | 1,847.5 µs | 1,882.4 µs | 1,917.6 µs | 2.0 % |
| | | | ready | A | 4,206.6 µs | 4,226.3 µs | 4,238.2 µs | 4,343.2 µs | 4,398.8 µs | 4.9 % |
| | | | ready | B | 4,200.5 µs | 4,242.3 µs | 4,299.8 µs | 4,368.5 µs | 4,426.2 µs | 1.7 % |
| range-60480 | 60,480 rows | 130 / 130 | first_byte | A | 25,929.9 µs | 26,535.0 µs | 26,673.7 µs | 27,033.2 µs | 29,561.4 µs | 8.3 % |
| | | | first_byte | B | 1,854.2 µs | 1,877.8 µs | 1,897.2 µs | 1,937.8 µs | 1,973.7 µs | 0.4 % |
| | | | first_batch | A | 25,929.9 µs | 26,535.0 µs | 26,673.7 µs | 27,033.2 µs | 29,561.4 µs | 8.3 % |
| | | | first_batch | B | 1,869.1 µs | 1,893.0 µs | 1,912.5 µs | 1,953.5 µs | 1,988.6 µs | 0.4 % |
| | | | ready | A | 27,408.7 µs | 28,091.4 µs | 28,259.3 µs | 28,717.4 µs | 30,838.1 µs | 7.8 % |
| | | | ready | B | 26,000.4 µs | 26,240.7 µs | 26,520.7 µs | 26,994.0 µs | 27,115.9 µs | 0.3 % |
| filter-symbol-3 | 7,560 rows | 17 / 17 | first_byte | A | 4,269.6 µs | 4,316.8 µs | 4,341.6 µs | 4,427.6 µs | 4,482.2 µs | 6.4 % |
| | | | first_byte | B | 371.5 µs | 377.9 µs | 387.5 µs | 412.7 µs | 422.8 µs | 3.1 % |
| | | | first_batch | A | 4,287.3 µs | 4,332.3 µs | 4,346.6 µs | 4,437.4 µs | 4,482.2 µs | 6.3 % |
| | | | first_batch | B | 371.5 µs | 378.1 µs | 387.6 µs | 412.7 µs | 422.8 µs | 3.1 % |
| | | | ready | A | 4,397.3 µs | 4,441.0 µs | 4,462.5 µs | 4,561.1 µs | 4,576.1 µs | 6.5 % |
| | | | ready | B | 4,610.2 µs | 4,662.2 µs | 4,698.5 µs | 4,753.3 µs | 4,777.5 µs | 1.2 % |

### First-batch time and the share of the reply it has reached (medians of the runs' p50)

| shape | rows returned | A first batch | B first batch | B over A | spread A / B | a finding? | A first batch / A ready | B first batch / B ready | A ready | B ready | ready B over A |
|---|---|---|---|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | 44.3 µs | 44.5 µs | +0.3 % | 3.6 % / 1.9 % | no (inside the spread) | 100.0 % | 100.0 % | 44.3 µs | 44.5 µs | +0.3 % |
| range-100 | 100 rows | 90.1 µs | 89.8 µs | -0.3 % | 2.7 % / 0.9 % | no (inside the spread) | 100.0 % | 100.0 % | 90.1 µs | 89.8 µs | -0.3 % |
| range-1000 | 1,000 rows | 470.9 µs | 489.6 µs | +4.0 % | 5.4 % / 3.0 % | no (inside the spread) | 99.7 % | 99.3 % | 472.3 µs | 493.3 µs | +4.4 % |
| range-10000 | 10,000 rows | 4,040.8 µs | 1,847.5 µs | -54.3 % | 5.2 % / 2.0 % | **yes** | 95.3 % | 43.0 % | 4,238.2 µs | 4,299.8 µs | +1.5 % |
| range-60480 | 60,480 rows | 26,673.7 µs | 1,912.5 µs | -92.8 % | 8.3 % / 0.4 % | **yes** | 94.4 % | 7.2 % | 28,259.3 µs | 26,520.7 µs | -6.2 % |
| filter-symbol-3 | 7,560 rows | 4,346.6 µs | 387.6 µs | -91.1 % | 6.3 % / 3.1 % | **yes** | 97.4 % | 8.2 % | 4,462.5 µs | 4,698.5 µs | +5.3 % |

### Reply shape check (first frame types, batches, bytes, errors), A and B

| shape | binary | first_types of rep 0 | batches | bytes | rows | error flags in any rep |
|---|---|---|---|---|---|---|
| range-1 | A | [3, 4, 5, 6, 8, 2] | 1 | 476 B | 1 | 0 |
| range-1 | B | [3, 4, 5, 6, 8, 2] | 1 | 476 B | 1 | 0 |
| range-100 | A | [3, 4, 5, 6, 8, 2] | 1 | 14,336 B | 100 | 0 |
| range-100 | B | [3, 4, 5, 6, 8, 2] | 1 | 14,336 B | 100 | 0 |
| range-1000 | A | [3, 4, 5, 6, 6, 6] | 3 | 140,356 B | 1,000 | 0 |
| range-1000 | B | [3, 4, 5, 6, 6, 6] | 3 | 140,356 B | 1,000 | 0 |
| range-10000 | A | [3, 4, 5, 6, 6, 6] | 22 | 1,400,546 B | 10,000 | 0 |
| range-10000 | B | [3, 4, 5, 6, 6, 6] | 22 | 1,400,546 B | 10,000 | 0 |
| range-60480 | A | [3, 4, 5, 6, 6, 6] | 130 | 8,468,826 B | 60,480 | 0 |
| range-60480 | B | [3, 4, 5, 6, 6, 6] | 130 | 8,468,826 B | 60,480 | 0 |
| filter-symbol-3 | A | [3, 4, 5, 6, 6, 6] | 17 | 1,058,896 B | 7,560 | 0 |
| filter-symbol-3 | B | [3, 4, 5, 6, 6, 6] | 17 | 1,058,896 B | 7,560 | 0 |

## Scenario 1 (60,480 bars), c1-g, warm QPS (driver matrix)

| shape | rows returned | A, runs | B, runs | A median | B median | B over A | spread A / B | a finding? |
|---|---|---|---|---|---|---|---|---|
| bar-range | 200 rows | 1,257.1 / 1,265.6 / 1,234.2 qps | 1,269.8 / 1,262.6 / 1,211.7 qps | 1,257.1 qps | 1,262.6 qps | +0.4 % | 2.5 % / 4.6 % | no (inside the spread) |
| symbol-history | 7,560 rows | 27.2 / 27.2 / 27.4 qps | 28.9 / 28.8 / 29.1 qps | 27.2 qps | 28.9 qps | +6.2 % | 0.7 % / 1.0 % | **yes** |
| model-join | 359 rows | 1,073.4 / 1,033.2 / 1,028.1 qps | 1,065.6 / 1,040.8 / 1,029.9 qps | 1,033.2 qps | 1,040.8 qps | +0.7 % | 4.4 % / 3.4 % | no (inside the spread) |
| day-slice | 8 rows | 614.5 / 586.6 / 599.9 qps | 591.4 / 604.1 / 588.8 qps | 599.9 qps | 591.4 qps | -1.4 % | 4.7 % / 2.6 % | no (inside the spread) |

Cold QPS (each argument first seen):

| shape | A median | B median | B over A | spread A / B | a finding? |
|---|---|---|---|---|---|
| bar-range | 1,246.9 qps | 1,273.2 qps | +2.1 % | 5.8 % / 1.2 % | no (inside the spread) |
| symbol-history | 27.7 qps | 28.3 qps | +2.2 % | 0.7 % / 6.4 % | no (inside the spread) |
| model-join | 1,016.7 qps | 1,033.9 qps | +1.7 % | 6.1 % / 2.6 % | no (inside the spread) |
| day-slice | 601.0 qps | 595.4 qps | -0.9 % | 0.6 % / 3.6 % | no (inside the spread) |

Phase distributions (each run's percentile, median over the runs):

| phase | rows returned | binary | ops per run | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| read-bar-range | 200 rows | A | 200 | 767.1 µs | 791.6 µs | 800.0 µs | 816.3 µs | 836.6 µs |
| read-bar-range | 200 rows | B | 200 | 767.3 µs | 786.5 µs | 793.3 µs | 819.4 µs | 866.0 µs |
| read-symbol-history | 7,560 rows | A | 10 | 35,570.5 µs | 35,762.1 µs | 35,908.6 µs | 39,780.1 µs | 39,780.1 µs |
| read-symbol-history | 7,560 rows | B | 10 | 32,768.0 µs | 33,103.2 µs | 33,423.2 µs | 38,200.8 µs | 38,200.8 µs |
| read-day-slice | 8 rows | A | 200 | 1,625.3 µs | 1,652.8 µs | 1,660.2 µs | 1,714.8 µs | 1,829.9 µs |
| read-day-slice | 8 rows | B | 200 | 1,627.4 µs | 1,646.3 µs | 1,656.2 µs | 1,727.6 µs | 1,803.6 µs |

## Scenario 3 (10,000 loans, unindexed), c1-g, QPS derived as 1,000,000 / mean µs

| shape | A, runs | B, runs | A median | B median | B over A | spread A / B | a finding? |
|---|---|---|---|---|---|---|---|
| books-by-genre | 2,918 / 3,135 / 2,966 qps | 3,102 / 3,197 / 3,120 qps | 2,966 qps | 3,120 qps | +5.2 % | 7.3 % / 3.1 % | no (inside the spread) |
| loans-by-daterange | 997 / 1,003 / 999 qps | 994 / 1,007 / 989 qps | 999 qps | 994 qps | -0.6 % | 0.6 % / 1.8 % | no (inside the spread) |
| overdue | 1,192 / 1,239 / 1,192 qps | 1,251 / 1,261 / 1,248 qps | 1,192 qps | 1,251 qps | +5.0 % | 3.9 % / 1.0 % | **yes** |
| loans-by-user | 2,660 / 2,886 / 2,651 qps | 2,930 / 2,904 / 2,894 qps | 2,660 qps | 2,904 qps | +9.1 % | 8.8 % / 1.3 % | **yes** |

Phase distributions (each run's percentile, median over the runs):

| phase | binary | ops per run | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| books-by-genre | A | 200 | 290.8 µs | 320.9 µs | 337.2 µs | 372.8 µs | 390.5 µs |
| books-by-genre | B | 200 | 288.1 µs | 310.9 µs | 319.2 µs | 344.4 µs | 357.4 µs |
| loans-by-daterange | A | 200 | 392.2 µs | 982.9 µs | 1,012.7 µs | 1,068.3 µs | 1,117.0 µs |
| loans-by-daterange | B | 200 | 415.2 µs | 985.9 µs | 1,017.8 µs | 1,082.6 µs | 1,117.0 µs |
| overdue | A | 200 | 406.2 µs | 834.0 µs | 855.7 µs | 914.4 µs | 946.0 µs |
| overdue | B | 200 | 371.8 µs | 794.5 µs | 816.7 µs | 871.7 µs | 887.2 µs |
| loans-by-user | A | 200 | 331.8 µs | 367.0 µs | 373.9 µs | 397.9 µs | 407.2 µs |
| loans-by-user | B | 200 | 321.9 µs | 334.0 µs | 341.2 µs | 372.6 µs | 380.9 µs |

## Overhead control: point statements, server CPU per statement (bn_census, A and B side by side)

6 runs x 6 blocks, 1,998 statements per arm per run. Unclean cells: 0 of 84. Server CPU = the sum of /proc/<pid>/task/*/schedstat over every server thread; spread = (max - min) / median over the runs.

| shape | path | A server CPU | B server CPU | B over A | spread A / B | a finding? | A wall mean | B wall mean | wall B over A | wall spread A / B |
|---|---|---|---|---|---|---|---|---|---|---|
| noop | literal | 12.13 µs | 12.18 µs | +0.4 % | 2.9 % / 2.9 % | no (inside the spread) | 27.25 µs | 27.23 µs | -0.1 % | 2.3 % / 1.3 % |
| ping | literal | 108.88 µs | 107.68 µs | -1.1 % | 2.9 % / 5.0 % | no (inside the spread) | 292.52 µs | 290.03 µs | -0.9 % | 2.1 % / 4.3 % |
| ping | prepared | 108.55 µs | 106.83 µs | -1.6 % | 4.8 % / 4.2 % | no (inside the spread) | 289.11 µs | 286.99 µs | -0.7 % | 2.1 % / 0.8 % |
| select-pk | literal | 31.20 µs | 31.21 µs | +0.0 % | 20.0 % / 14.8 % | no (inside the spread) | 61.97 µs | 62.30 µs | +0.5 % | 10.8 % / 6.9 % |
| select-pk | prepared | 29.89 µs | 30.47 µs | +1.9 % | 14.2 % / 13.7 % | no (inside the spread) | 58.59 µs | 60.32 µs | +3.0 % | 8.1 % / 8.3 % |
| update-lit | literal | 26.64 µs | 26.59 µs | -0.2 % | 9.3 % / 5.1 % | no (inside the spread) | 51.33 µs | 51.58 µs | +0.5 % | 5.8 % / 9.8 % |
| update-lit | prepared | 26.48 µs | 25.78 µs | -2.7 % | 7.4 % / 6.9 % | no (inside the spread) | 48.67 µs | 48.04 µs | -1.3 % | 6.5 % / 4.3 % |

Wall of one statement (each run's percentile, median over the runs; the driver reports p90, not p95):

| shape | path | binary | statements per run | p0 | p25 | p50 | p90 | p99 |
|---|---|---|---|---|---|---|---|---|
| noop | literal | A | 1,998 | 22.89 µs | 26.16 µs | 26.42 µs | 27.60 µs | 39.95 µs |
| noop | literal | B | 1,998 | 22.64 µs | 26.15 µs | 26.43 µs | 27.36 µs | 38.90 µs |
| ping | literal | A | 1,998 | 273.06 µs | 281.25 µs | 287.17 µs | 304.17 µs | 367.36 µs |
| ping | literal | B | 1,998 | 272.48 µs | 280.84 µs | 285.97 µs | 301.51 µs | 348.61 µs |
| ping | prepared | A | 1,998 | 269.39 µs | 278.54 µs | 284.31 µs | 300.19 µs | 363.47 µs |
| ping | prepared | B | 1,998 | 269.20 µs | 277.94 µs | 283.92 µs | 299.00 µs | 329.33 µs |
| select-pk | literal | A | 1,998 | 54.13 µs | 56.56 µs | 59.40 µs | 69.73 µs | 87.29 µs |
| select-pk | literal | B | 1,998 | 55.11 µs | 57.11 µs | 60.25 µs | 69.45 µs | 88.54 µs |
| select-pk | prepared | A | 1,998 | 51.20 µs | 53.28 µs | 56.57 µs | 64.88 µs | 81.02 µs |
| select-pk | prepared | B | 1,998 | 52.15 µs | 54.08 µs | 57.11 µs | 65.92 µs | 85.92 µs |
| update-lit | literal | A | 1,998 | 40.42 µs | 47.12 µs | 47.74 µs | 57.00 µs | 76.08 µs |
| update-lit | literal | B | 1,998 | 42.85 µs | 47.29 µs | 48.01 µs | 56.45 µs | 74.99 µs |
| update-lit | prepared | A | 1,998 | 39.41 µs | 44.43 µs | 45.03 µs | 53.41 µs | 72.38 µs |
| update-lit | prepared | B | 1,998 | 40.48 µs | 44.18 µs | 44.82 µs | 53.18 µs | 74.08 µs |
