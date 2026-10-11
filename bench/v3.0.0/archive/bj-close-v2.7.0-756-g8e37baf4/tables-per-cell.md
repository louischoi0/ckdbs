
#### cores = 1, 200 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 2.06-6.72, after 1.98-6.00; other-CPU busy over the run 2.3%-9.0% (median 3.3%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 40.0 µs | 50.5 µs | 51.4 µs | 67.6 µs | 85.9 µs | 54.2 µs (sd 1.6) | 18,271 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 43.3 µs | 52.3 µs | 53.2 µs | 68.2 µs | 84.0 µs | 55.8 µs (sd 0.9) | 17,595 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 39.6 µs | 50.6 µs | 51.3 µs | 66.6 µs | 81.9 µs | 54.2 µs (sd 1.6) | 18,188 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 43.4 µs | 52.4 µs | 53.1 µs | 67.7 µs | 82.8 µs | 56.2 µs (sd 1.1) | 17,451 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 43.0 µs | 51.3 µs | 52.1 µs | 67.6 µs | 83.7 µs | 55.1 µs (sd 1.2) | 17,874 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 44.4 µs | 53.5 µs | 54.1 µs | 69.4 µs | 85.3 µs | 57.6 µs (sd 1.2) | 17,128 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 223.6 µs | 230.4 µs | 238.4 µs | 300.4 µs | 347.6 µs | 246.1 µs (sd 1.8) | 4,055 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 225.1 µs | 232.2 µs | 240.6 µs | 319.8 µs | 384.7 µs | 252.5 µs (sd 8.8) | 3,960 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 220.7 µs | 232.3 µs | 240.9 µs | 333.3 µs | 404.0 µs | 253.1 µs (sd 9.1) | 3,984 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 224.7 µs | 232.5 µs | 241.2 µs | 319.1 µs | 371.3 µs | 250.6 µs (sd 6.3) | 4,013 stmt/s |
| INSERT, pk named | A | 48,000 | 41.1 µs | 48.5 µs | 49.2 µs | 64.8 µs | 83.3 µs | 52.3 µs (sd 1.5) | 18,887 stmt/s |
| INSERT, pk named | B | 48,000 | 40.8 µs | 48.6 µs | 49.3 µs | 64.8 µs | 80.7 µs | 52.3 µs (sd 0.8) | 18,819 stmt/s |
| SELECT by pk (control) | A | 48,000 | 58.0 µs | 64.5 µs | 65.5 µs | 84.4 µs | 102.2 µs | 69.3 µs (sd 1.6) | 14,317 stmt/s |
| SELECT by pk (control) | B | 48,000 | 57.6 µs | 64.4 µs | 65.4 µs | 84.5 µs | 102.1 µs | 69.2 µs (sd 1.8) | 14,302 stmt/s |
| DELETE by pk | A | 48,000 | 38.5 µs | 48.0 µs | 48.8 µs | 63.7 µs | 75.0 µs | 51.4 µs (sd 1.6) | 19,413 stmt/s |
| DELETE by pk | B | 48,000 | 39.1 µs | 48.0 µs | 48.8 µs | 62.8 µs | 73.0 µs | 51.8 µs (sd 1.3) | 19,131 stmt/s |
| SHOW META (control) | A | 48,000 | 44.0 µs | 49.6 µs | 50.3 µs | 63.0 µs | 72.7 µs | 52.4 µs (sd 0.8) | 18,934 stmt/s |
| SHOW META (control) | B | 48,000 | 42.3 µs | 49.5 µs | 50.2 µs | 62.7 µs | 72.0 µs | 52.4 µs (sd 0.5) | 18,945 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.80 µs [+1.50 µs, +2.00 µs] | +3.52 % [+2.91, +3.88] | +1.64 µs [+0.87 µs, +2.25 µs] | -3.86 % [-4.33, -1.81] | **B slower** (+3.5 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.80 µs [+1.50 µs, +2.00 µs] | +3.51 % [+2.91, +3.88] | +1.93 µs [+0.68 µs, +2.96 µs] | -4.99 % [-5.62, -1.83] | **B slower** (+3.5 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, w = lit, by pk | +2.15 µs [+1.60 µs, +2.30 µs] | +4.13 % [+3.08, +4.48] | +2.51 µs [+1.83 µs, +3.18 µs] | -4.76 % [-5.81, -2.75] | **B slower** (+4.1 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, every row | +1.90 µs [-0.10 µs, +4.20 µs] | +0.79 % [-0.04, +1.72] | +6.46 µs [+2.94 µs, +11.37 µs] | -1.87 % [-3.04, -0.20] | no change resolved |
| UPDATE SET v = lit, every row (replicate) | -0.25 µs [-1.70 µs, +2.40 µs] | -0.10 % [-0.71, +0.99] | -2.52 µs [-5.52 µs, +0.43 µs] | +0.86 % [-0.53, +2.21] | no change resolved |
| INSERT, pk named | +0.15 µs [-0.10 µs, +0.30 µs] | +0.30 % [-0.20, +0.61] | +0.01 µs [-0.97 µs, +0.86 µs] | -1.18 % [-2.72, +1.62] | no change resolved |
| SELECT by pk (control) | -0.10 µs [-0.60 µs, +0.30 µs] | -0.15 % [-0.92, +0.46] | -0.09 µs [-0.62 µs, +0.43 µs] | +0.15 % [-1.19, +0.67] | no change resolved |
| DELETE by pk | +0.00 µs [-0.30 µs, +0.10 µs] | +0.00 % [-0.61, +0.21] | +0.36 µs [-0.59 µs, +1.13 µs] | -1.79 % [-2.39, +0.37] | no change resolved |
| SHOW META (control) | -0.10 µs [-0.30 µs, +0.05 µs] | -0.20 % [-0.60, +0.10] | -0.01 µs [-0.34 µs, +0.31 µs] | -0.15 % [-0.49, +0.73] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ -0.20 % [-0.60, +0.10]; replicate (second relation, same binary, per run, both arms pooled) point median -0.19 %, robust sd 0.28 % (plain sd 0.29 %); range median +0.44 %, robust sd 1.52 % (plain sd 1.56 %). Floor used: point 0.74 %, range 3.48 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 44.1 µs | 52.5 µs | 53.3 µs | 68.2 µs | 80.2 µs | 60.2 µs (sd 14.5) | 17,452 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 45.0 µs | 54.4 µs | 55.0 µs | 69.8 µs | 81.5 µs | 57.5 µs (sd 0.6) | 17,062 stmt/s | +1.75 µs [+1.60 µs, +1.90 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 45.0 µs | 54.1 µs | 54.7 µs | 69.8 µs | 81.1 µs | 58.8 µs (sd 1.7) | 16,790 stmt/s | +1.50 µs [+1.30 µs, +1.60 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 50.6 µs | 60.3 µs | 61.0 µs | 77.7 µs | 90.0 µs | 65.1 µs (sd 1.3) | 15,139 stmt/s | +7.85 µs [+7.70 µs, +8.00 µs] | - | - |

range (`WHERE id >= 1`, every one of the 200 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 223.6 µs | 231.7 µs | 241.8 µs | 364.8 µs | 905.4 µs | 270.2 µs (sd 12.9) | 3,727 stmt/s = 745,380 rows/s | - | 1209 ns | - |
| `SET v = v + 1` | 1,920 | 241.4 µs | 255.3 µs | 266.7 µs | 396.9 µs | 1,057 µs | 297.8 µs (sd 14.3) | 3,395 stmt/s = 679,010 rows/s | +25.95 µs [+24.70 µs, +28.10 µs] | 1334 ns | +130 ns [+123, +140] |
| `SET v = w, w = v` | 1,920 | 239.1 µs | 247.7 µs | 257.4 µs | 349.0 µs | 458.8 µs | 273.0 µs (sd 26.0) | 3,774 stmt/s = 754,730 rows/s | +15.00 µs [+13.30 µs, +18.40 µs] | 1287 ns | +75 ns [+66, +92] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 282.8 µs | 293.4 µs | 306.9 µs | 502.0 µs | 1,157 µs | 351.7 µs (sd 21.0) | 2,880 stmt/s = 575,960 rows/s | +65.90 µs [+64.40 µs, +67.70 µs] | 1534 ns | +330 ns [+322, +338] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 259.6 µs | 274.4 µs | 286.9 µs | 410.4 µs | 1,531 µs | 324.7 µs (sd 24.7) | 3,137 stmt/s = 627,330 rows/s | +47.40 µs [+44.40 µs, +48.20 µs] | 1434 ns | +237 ns [+222, +241] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 44.0 µs | 54.3 µs | 54.9 µs | 70.9 µs | 87.9 µs | 77.5 µs (sd 31.1) | 16,671 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 112.2 µs | 122.0 µs | 124.2 µs | 156.8 µs | 193.4 µs | 134.9 µs (sd 7.8) | 7,589 /s | +69.10 µs [+68.80 µs, +69.40 µs] |
|   of which the `SELECT v` | 48,000 | 55.9 µs | 63.9 µs | 64.8 µs | 85.4 µs | 106.8 µs | 69.0 µs (sd 2.6) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 48.7 µs | 57.1 µs | 57.9 µs | 75.3 µs | 93.7 µs | 65.1 µs (sd 6.9) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 172.2 µs | 181.8 µs | 186.9 µs | 226.8 µs | 271.7 µs | 243.4 µs (sd 27.3) | 4,045 /s | +131.25 µs [+130.70 µs, +132.00 µs] |


#### cores = 1, 1,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 1.53-9.10, after 1.64-7.93; other-CPU busy over the run 2.2%-21.6% (median 2.8%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 40.0 µs | 50.5 µs | 51.4 µs | 66.9 µs | 83.7 µs | 53.9 µs (sd 1.0) | 18,275 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 43.3 µs | 52.3 µs | 53.2 µs | 68.4 µs | 85.2 µs | 56.0 µs (sd 1.9) | 17,656 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 41.1 µs | 50.6 µs | 51.4 µs | 66.2 µs | 81.3 µs | 54.4 µs (sd 1.4) | 18,121 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 43.8 µs | 52.3 µs | 53.1 µs | 68.2 µs | 82.7 µs | 56.5 µs (sd 0.9) | 17,357 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 42.9 µs | 51.3 µs | 52.0 µs | 67.1 µs | 81.5 µs | 54.9 µs (sd 1.0) | 17,879 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 45.5 µs | 53.6 µs | 54.2 µs | 68.8 µs | 84.4 µs | 57.3 µs (sd 1.0) | 17,070 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 903.1 µs | 938.0 µs | 964.0 µs | 1,120 µs | 1,460 µs | 993.6 µs (sd 27.2) | 1,016 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 914.0 µs | 947.1 µs | 964.8 µs | 1,106 µs | 1,399 µs | 998.1 µs (sd 14.1) | 1,006 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 900.5 µs | 937.2 µs | 959.1 µs | 1,099 µs | 1,500 µs | 993.2 µs (sd 22.3) | 1,009 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 910.0 µs | 946.8 µs | 972.0 µs | 2,330 µs | 3,374 µs | 1,164 µs (sd 19.4) | 861 stmt/s |
| INSERT, pk named | A | 48,000 | 37.9 µs | 48.5 µs | 49.2 µs | 64.2 µs | 78.1 µs | 79.9 µs (sd 55.2) | 18,738 stmt/s |
| INSERT, pk named | B | 48,000 | 36.6 µs | 48.7 µs | 49.4 µs | 64.0 µs | 77.3 µs | 164.2 µs (sd 58.2) | 5,289 stmt/s |
| SELECT by pk (control) | A | 48,000 | 57.6 µs | 64.9 µs | 67.8 µs | 88.1 µs | 108.6 µs | 75.6 µs (sd 16.1) | 13,935 stmt/s |
| SELECT by pk (control) | B | 48,000 | 59.5 µs | 64.9 µs | 67.9 µs | 88.6 µs | 107.1 µs | 71.3 µs (sd 1.7) | 13,959 stmt/s |
| DELETE by pk | A | 48,000 | 38.4 µs | 48.1 µs | 48.8 µs | 64.7 µs | 85.7 µs | 52.5 µs (sd 3.6) | 19,302 stmt/s |
| DELETE by pk | B | 48,000 | 39.8 µs | 48.2 µs | 48.9 µs | 64.3 µs | 80.7 µs | 51.6 µs (sd 2.8) | 19,433 stmt/s |
| SHOW META (control) | A | 48,000 | 44.5 µs | 49.5 µs | 50.2 µs | 62.9 µs | 74.8 µs | 52.4 µs (sd 0.9) | 19,069 stmt/s |
| SHOW META (control) | B | 48,000 | 44.2 µs | 49.7 µs | 50.4 µs | 63.2 µs | 75.0 µs | 52.6 µs (sd 1.0) | 18,955 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.60 µs [+1.40 µs, +2.20 µs] | +3.12 % [+2.71, +4.26] | +2.17 µs [+1.40 µs, +3.17 µs] | -3.79 % [-4.77, -2.26] | **B slower** (+3.1 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.55 µs [+1.40 µs, +2.10 µs] | +3.02 % [+2.62, +4.17] | +2.07 µs [+1.27 µs, +2.83 µs] | -4.15 % [-5.59, -1.47] | **B slower** (+3.0 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, w = lit, by pk | +2.15 µs [+1.70 µs, +2.60 µs] | +4.15 % [+3.28, +5.09] | +2.34 µs [+1.60 µs, +3.02 µs] | -4.71 % [-5.53, -2.28] | **B slower** (+4.1 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, every row | +8.60 µs [+0.45 µs, +11.60 µs] | +0.90 % [+0.05, +1.22] | +4.54 µs [-7.46 µs, +15.34 µs] | -0.56 % [-1.59, -0.16] | CI excludes 0, inside the floor (2.3 %): not resolved |
| UPDATE SET v = lit, every row (replicate) | +16.80 µs [+6.80 µs, +23.70 µs] | +1.77 % [+0.72, +2.52] | +171.33 µs [+160.93 µs, +181.76 µs] | -14.70 % [-16.52, -13.36] | CI excludes 0, inside the floor (2.3 %): not resolved |
| INSERT, pk named | +0.25 µs [+0.00 µs, +0.40 µs] | +0.51 % [+0.00, +0.81] | +84.31 µs [+26.06 µs, +132.44 µs] | -69.96 % [-72.54, -65.61] | no change resolved |
| SELECT by pk (control) | +0.00 µs [-0.20 µs, +0.50 µs] | +0.00 % [-0.30, +0.74] | -4.22 µs [-12.31 µs, +0.29 µs] | +0.01 % [-0.24, +0.17] | no change resolved |
| DELETE by pk | +0.00 µs [-0.10 µs, +0.20 µs] | +0.00 % [-0.20, +0.41] | -0.86 µs [-1.71 µs, -0.14 µs] | +0.80 % [-0.77, +2.93] | no change resolved |
| SHOW META (control) | +0.20 µs [+0.15 µs, +0.40 µs] | +0.40 % [+0.30, +0.80] | +0.23 µs [+0.03 µs, +0.43 µs] | -0.46 % [-0.89, -0.01] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ +0.40 % [+0.30, +0.80]; replicate (second relation, same binary, per run, both arms pooled) point median +0.00 %, robust sd 0.56 % (plain sd 0.49 %); range median +0.12 %, robust sd 1.11 % (plain sd 1.07 %). Floor used: point 1.12 %, range 2.33 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 43.9 µs | 52.6 µs | 53.3 µs | 68.0 µs | 80.9 µs | 58.8 µs (sd 9.7) | 17,448 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 45.7 µs | 54.4 µs | 55.0 µs | 70.5 µs | 84.2 µs | 59.5 µs (sd 7.1) | 16,960 stmt/s | +1.80 µs [+1.65 µs, +1.90 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 45.7 µs | 54.0 µs | 54.7 µs | 70.7 µs | 85.6 µs | 60.4 µs (sd 7.1) | 16,865 stmt/s | +1.40 µs [+1.20 µs, +1.60 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 50.7 µs | 60.3 µs | 61.1 µs | 77.3 µs | 90.2 µs | 65.4 µs (sd 2.3) | 15,177 stmt/s | +7.70 µs [+7.50 µs, +8.00 µs] | - | - |

range (`WHERE id >= 1`, every one of the 1,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 902.0 µs | 939.7 µs | 959.0 µs | 1,495 µs | 2,923 µs | 1,104 µs (sd 186.9) | 938 stmt/s = 937,600 rows/s | - | 959 ns | - |
| `SET v = v + 1` | 1,920 | 1,020 µs | 1,049 µs | 1,068 µs | 1,316 µs | 71.36 ms | 3,882 µs (sd 1221.4) | 244 stmt/s = 244,350 rows/s | +108.35 µs [+106.40 µs, +113.40 µs] | 1068 ns | +108 ns [+106, +113] |
| `SET v = w, w = v` | 1,920 | 983.3 µs | 1,012 µs | 1,031 µs | 1,229 µs | 6,404 µs | 1,315 µs (sd 622.3) | 883 stmt/s = 883,350 rows/s | +68.25 µs [+65.00 µs, +71.50 µs] | 1031 ns | +68 ns [+65, +72] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 1,177 µs | 1,219 µs | 1,243 µs | 1,754 µs | 4,114 µs | 1,621 µs (sd 858.6) | 738 stmt/s = 737,700 rows/s | +283.90 µs [+277.60 µs, +289.80 µs] | 1243 ns | +284 ns [+278, +290] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 1,044 µs | 1,075 µs | 1,095 µs | 1,394 µs | 3,761 µs | 1,490 µs (sd 972.8) | 843 stmt/s = 843,100 rows/s | +136.55 µs [+133.20 µs, +138.95 µs] | 1095 ns | +137 ns [+133, +139] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 44.9 µs | 54.3 µs | 55.0 µs | 72.2 µs | 88.7 µs | 77.7 µs (sd 34.8) | 17,183 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 114.7 µs | 122.4 µs | 125.3 µs | 163.6 µs | 200.1 µs | 132.5 µs (sd 10.0) | 7,656 /s | +69.80 µs [+69.50 µs, +70.00 µs] |
|   of which the `SELECT v` | 48,000 | 56.1 µs | 64.1 µs | 65.5 µs | 88.0 µs | 107.7 µs | 69.7 µs (sd 5.0) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 50.4 µs | 57.1 µs | 58.1 µs | 77.9 µs | 102.2 µs | 61.8 µs (sd 4.8) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 171.5 µs | 182.9 µs | 190.7 µs | 241.1 µs | 287.8 µs | 255.5 µs (sd 39.6) | 3,730 /s | +134.95 µs [+134.90 µs, +135.30 µs] |


#### cores = 1, 10,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 1.94-6.32, after 2.44-5.09; other-CPU busy over the run 2.9%-20.2% (median 17.3%); 0 errors; gate wait median 21 s; processes matching cc1plus|cmake --build|ctest: 2 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 40.1 µs | 50.9 µs | 51.6 µs | 67.1 µs | 83.8 µs | 55.2 µs (sd 3.4) | 18,073 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 43.6 µs | 52.7 µs | 53.6 µs | 68.8 µs | 85.5 µs | 56.9 µs (sd 1.6) | 17,281 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 43.1 µs | 51.0 µs | 51.8 µs | 70.4 µs | 98.7 µs | 55.6 µs (sd 5.1) | 18,064 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 44.0 µs | 52.8 µs | 53.7 µs | 73.5 µs | 111.6 µs | 59.0 µs (sd 10.5) | 17,424 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 43.1 µs | 51.6 µs | 52.5 µs | 68.9 µs | 85.7 µs | 55.3 µs (sd 1.3) | 17,842 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 44.9 µs | 53.8 µs | 54.5 µs | 72.5 µs | 92.1 µs | 58.7 µs (sd 1.9) | 16,865 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 9,308 µs | 9,638 µs | 9,794 µs | 19.07 ms | 462.62 ms | 21.35 ms (sd 2359.8) | 48 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 9,496 µs | 9,754 µs | 9,877 µs | 16.83 ms | 394.77 ms | 17.55 ms (sd 673.1) | 58 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 9,275 µs | 9,618 µs | 9,779 µs | 19.43 ms | 808.29 ms | 35.67 ms (sd 3870.8) | 28 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 9,432 µs | 9,745 µs | 9,882 µs | 16.98 ms | 54.56 ms | 13.01 ms (sd 3107.0) | 87 stmt/s |
| INSERT, pk named | A | 48,000 | 40.1 µs | 48.6 µs | 49.4 µs | 70.6 µs | 94.1 µs | 54.6 µs (sd 5.2) | 18,662 stmt/s |
| INSERT, pk named | B | 48,000 | 41.8 µs | 48.6 µs | 49.5 µs | 71.5 µs | 92.6 µs | 54.9 µs (sd 8.1) | 18,824 stmt/s |
| SELECT by pk (control) | A | 48,000 | 59.0 µs | 64.0 µs | 64.9 µs | 81.9 µs | 99.6 µs | 68.7 µs (sd 2.5) | 14,552 stmt/s |
| SELECT by pk (control) | B | 48,000 | 57.5 µs | 64.2 µs | 65.0 µs | 84.1 µs | 102.7 µs | 69.1 µs (sd 3.1) | 14,568 stmt/s |
| DELETE by pk | A | 48,000 | 39.9 µs | 48.1 µs | 48.9 µs | 64.4 µs | 75.9 µs | 51.6 µs (sd 0.4) | 19,168 stmt/s |
| DELETE by pk | B | 48,000 | 38.0 µs | 48.2 µs | 49.0 µs | 64.5 µs | 76.3 µs | 51.8 µs (sd 1.1) | 19,167 stmt/s |
| SHOW META (control) | A | 48,000 | 42.1 µs | 49.5 µs | 50.2 µs | 63.9 µs | 73.4 µs | 52.7 µs (sd 1.0) | 18,882 stmt/s |
| SHOW META (control) | B | 48,000 | 42.0 µs | 49.5 µs | 50.2 µs | 63.0 µs | 71.6 µs | 52.9 µs (sd 3.1) | 18,981 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.90 µs [+1.65 µs, +2.40 µs] | +3.68 % [+3.18, +4.67] | +1.72 µs [-0.51 µs, +3.27 µs] | -4.07 % [-5.33, -3.17] | **B slower** (+3.7 % at p50, floor 0.6 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.80 µs [+1.50 µs, +2.40 µs] | +3.46 % [+2.92, +4.67] | +3.38 µs [+1.70 µs, +6.23 µs] | -3.99 % [-4.90, -2.90] | **B slower** (+3.5 % at p50, floor 0.6 %) |
| UPDATE SET v = lit, w = lit, by pk | +2.00 µs [+1.85 µs, +2.40 µs] | +3.83 % [+3.51, +4.59] | +3.41 µs [+2.36 µs, +4.54 µs] | -5.24 % [-7.53, -2.53] | **B slower** (+3.8 % at p50, floor 0.6 %) |
| UPDATE SET v = lit, every row | +96.55 µs [+45.80 µs, +153.40 µs] | +0.99 % [+0.73, +1.56] | -3,801 µs [-5,176 µs, -2,804 µs] | +20.76 % [+17.89, +23.17] | CI excludes 0, inside the floor (1.0 %): not resolved |
| UPDATE SET v = lit, every row (replicate) | +134.00 µs [+50.10 µs, +163.50 µs] | +1.37 % [+0.51, +1.66] | -22.66 ms [-25.46 ms, -19.65 ms] | +215.84 % [+130.46, +232.48] | **B slower** (+1.4 % at p50, floor 1.0 %) |
| INSERT, pk named | +0.05 µs [-0.10 µs, +0.20 µs] | +0.10 % [-0.20, +0.32] | +0.29 µs [-1.56 µs, +2.40 µs] | +0.75 % [-0.71, +2.01] | no change resolved |
| SELECT by pk (control) | +0.00 µs [-0.10 µs, +0.60 µs] | +0.00 % [-0.15, +0.93] | +0.46 µs [-0.73 µs, +1.53 µs] | -0.62 % [-1.75, +0.45] | no change resolved |
| DELETE by pk | +0.10 µs [-0.10 µs, +0.25 µs] | +0.20 % [-0.20, +0.51] | +0.21 µs [-0.41 µs, +0.87 µs] | -0.78 % [-1.54, +1.81] | no change resolved |
| SHOW META (control) | -0.05 µs [-0.20 µs, +0.20 µs] | -0.10 % [-0.40, +0.40] | +0.17 µs [-0.89 µs, +1.81 µs] | +0.34 % [+0.04, +1.27] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ -0.10 % [-0.40, +0.40]; replicate (second relation, same binary, per run, both arms pooled) point median +0.00 %, robust sd 0.28 % (plain sd 5.40 %); range median +0.04 %, robust sd 0.48 % (plain sd 0.54 %). Floor used: point 0.56 %, range 1.00 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 43.0 µs | 53.0 µs | 53.9 µs | 71.1 µs | 85.8 µs | 62.6 µs (sd 8.5) | 16,748 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 46.1 µs | 54.8 µs | 55.5 µs | 73.0 µs | 89.4 µs | 65.3 µs (sd 10.1) | 15,979 stmt/s | +1.65 µs [+1.50 µs, +1.80 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 46.0 µs | 54.5 µs | 55.2 µs | 72.6 µs | 86.9 µs | 65.7 µs (sd 13.7) | 16,207 stmt/s | +1.30 µs [+1.20 µs, +1.40 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 51.1 µs | 60.8 µs | 61.7 µs | 81.6 µs | 97.1 µs | 67.3 µs (sd 1.8) | 14,448 stmt/s | +7.80 µs [+7.70 µs, +7.90 µs] | - | - |

range (`WHERE id >= 1`, every one of the 10,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 8,721 µs | 8,956 µs | 9,221 µs | 16.20 ms | 406.08 ms | 19.46 ms (sd 3651.2) | 51 stmt/s = 513,500 rows/s | - | 922 ns | - |
| `SET v = v + 1` | 1,920 | 9,768 µs | 10.16 ms | 10.36 ms | 19.76 ms | 402.56 ms | 23.81 ms (sd 1411.0) | 42 stmt/s = 415,500 rows/s | +1,167 µs [+1,150 µs, +1,191 µs] | 1036 ns | +117 ns [+115, +119] |
| `SET v = w, w = v` | 1,920 | 9,407 µs | 9,689 µs | 10.04 ms | 16.61 ms | 69.35 ms | 14.19 ms (sd 1222.4) | 72 stmt/s = 723,500 rows/s | +737.65 µs [+673.50 µs, +808.70 µs] | 1004 ns | +74 ns [+67, +81] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 11.23 ms | 11.56 ms | 11.81 ms | 18.53 ms | 419.68 ms | 27.94 ms (sd 1453.1) | 36 stmt/s = 362,500 rows/s | +2,624 µs [+2,590 µs, +2,662 µs] | 1181 ns | +262 ns [+259, +266] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 9,848 µs | 10.25 ms | 10.53 ms | 18.48 ms | 405.35 ms | 21.06 ms (sd 1393.9) | 48 stmt/s = 483,500 rows/s | +1,283 µs [+1,250 µs, +1,318 µs] | 1053 ns | +128 ns [+125, +132] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 45.1 µs | 54.6 µs | 55.5 µs | 77.6 µs | 178.0 µs | 84.4 µs (sd 37.4) | 15,350 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 115.2 µs | 121.9 µs | 125.0 µs | 159.1 µs | 184.6 µs | 133.9 µs (sd 7.5) | 7,570 /s | +68.85 µs [+68.55 µs, +69.50 µs] |
|   of which the `SELECT v` | 48,000 | 55.1 µs | 63.7 µs | 65.0 µs | 86.1 µs | 101.3 µs | 69.7 µs (sd 3.1) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 51.7 µs | 57.0 µs | 58.1 µs | 77.2 µs | 90.3 µs | 63.3 µs (sd 5.3) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 172.0 µs | 182.1 µs | 188.5 µs | 236.3 µs | 273.3 µs | 207.7 µs (sd 24.4) | 5,069 /s | +132.15 µs [+131.10 µs, +133.00 µs] |


#### cores = 2, 200 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 2.04-3.00, after 1.96-2.92; other-CPU busy over the run 2.9%-18.4% (median 8.5%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 43.3 µs | 51.8 µs | 52.8 µs | 71.2 µs | 88.0 µs | 57.5 µs (sd 7.6) | 17,634 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 48.1 µs | 53.8 µs | 54.5 µs | 72.8 µs | 85.8 µs | 57.7 µs (sd 1.2) | 17,124 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 46.0 µs | 51.7 µs | 52.6 µs | 70.0 µs | 81.0 µs | 56.3 µs (sd 1.8) | 17,560 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 47.2 µs | 53.7 µs | 54.4 µs | 71.7 µs | 83.4 µs | 58.3 µs (sd 1.4) | 16,949 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 44.7 µs | 52.7 µs | 53.5 µs | 70.4 µs | 79.5 µs | 57.0 µs (sd 1.2) | 17,199 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 48.7 µs | 54.6 µs | 55.2 µs | 72.6 µs | 82.6 µs | 58.7 µs (sd 1.0) | 16,639 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 276.5 µs | 296.9 µs | 308.6 µs | 359.7 µs | 409.9 µs | 313.9 µs (sd 2.9) | 3,170 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 280.5 µs | 301.6 µs | 314.4 µs | 371.5 µs | 413.1 µs | 319.6 µs (sd 4.6) | 3,124 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 282.1 µs | 303.4 µs | 317.2 µs | 365.9 µs | 418.5 µs | 321.6 µs (sd 6.8) | 3,116 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 287.5 µs | 305.5 µs | 318.1 µs | 376.1 µs | 471.2 µs | 325.8 µs (sd 8.8) | 3,076 stmt/s |
| INSERT, pk named | A | 48,000 | 45.0 µs | 49.0 µs | 49.7 µs | 68.2 µs | 83.0 µs | 53.8 µs (sd 1.6) | 18,381 stmt/s |
| INSERT, pk named | B | 48,000 | 43.7 µs | 49.1 µs | 49.9 µs | 68.9 µs | 83.7 µs | 53.2 µs (sd 1.2) | 18,538 stmt/s |
| SELECT by pk (control) | A | 48,000 | 60.2 µs | 65.6 µs | 66.7 µs | 86.6 µs | 99.3 µs | 70.3 µs (sd 0.8) | 14,068 stmt/s |
| SELECT by pk (control) | B | 48,000 | 60.4 µs | 65.6 µs | 66.6 µs | 86.1 µs | 100.0 µs | 70.7 µs (sd 2.9) | 14,148 stmt/s |
| DELETE by pk | A | 48,000 | 42.9 µs | 48.6 µs | 49.3 µs | 65.0 µs | 73.5 µs | 51.5 µs (sd 0.4) | 19,196 stmt/s |
| DELETE by pk | B | 48,000 | 43.9 µs | 48.6 µs | 49.4 µs | 65.4 µs | 74.0 µs | 52.0 µs (sd 0.7) | 19,046 stmt/s |
| SHOW META (control) | A | 48,000 | 45.0 µs | 49.7 µs | 50.4 µs | 64.2 µs | 72.8 µs | 52.7 µs (sd 1.0) | 18,875 stmt/s |
| SHOW META (control) | B | 48,000 | 42.4 µs | 49.8 µs | 50.5 µs | 65.2 µs | 73.7 µs | 53.0 µs (sd 0.9) | 18,797 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.65 µs [+1.35 µs, +2.20 µs] | +3.11 % [+2.55, +4.25] | +0.23 µs [-3.55 µs, +2.49 µs] | -2.80 % [-3.89, -1.99] | **B slower** (+3.1 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.80 µs [+1.30 µs, +2.20 µs] | +3.41 % [+2.46, +4.28] | +2.08 µs [+0.97 µs, +3.09 µs] | -3.14 % [-5.82, -2.14] | **B slower** (+3.4 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, w = lit, by pk | +1.90 µs [+1.30 µs, +2.30 µs] | +3.56 % [+2.41, +4.35] | +1.69 µs [+0.91 µs, +2.48 µs] | -3.16 % [-4.44, -1.02] | **B slower** (+3.6 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, every row | +4.70 µs [+3.50 µs, +6.70 µs] | +1.53 % [+1.14, +2.16] | +5.66 µs [+3.02 µs, +8.34 µs] | -1.77 % [-2.59, -0.59] | CI excludes 0, inside the floor (4.6 %): not resolved |
| UPDATE SET v = lit, every row (replicate) | +4.70 µs [-1.40 µs, +6.20 µs] | +1.50 % [-0.45, +1.98] | +4.22 µs [-0.64 µs, +8.89 µs] | -1.74 % [-2.60, +0.12] | no change resolved |
| INSERT, pk named | +0.25 µs [-0.05 µs, +0.40 µs] | +0.50 % [+0.00, +0.81] | -0.58 µs [-1.22 µs, +0.13 µs] | +1.33 % [-0.25, +2.75] | no change resolved |
| SELECT by pk (control) | +0.05 µs [-0.50 µs, +0.10 µs] | +0.08 % [-0.75, +0.38] | +0.34 µs [-0.44 µs, +1.61 µs] | +0.13 % [-0.14, +1.06] | no change resolved |
| DELETE by pk | +0.10 µs [-0.10 µs, +0.30 µs] | +0.20 % [-0.20, +0.61] | +0.50 µs [+0.13 µs, +0.89 µs] | -0.59 % [-2.06, +0.11] | no change resolved |
| SHOW META (control) | +0.05 µs [-0.20 µs, +0.30 µs] | +0.10 % [-0.39, +0.59] | +0.31 µs [-0.33 µs, +0.94 µs] | -0.62 % [-1.19, +0.28] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ +0.10 % [-0.39, +0.59]; replicate (second relation, same binary, per run, both arms pooled) point median -0.19 %, robust sd 0.27 % (plain sd 0.89 %); range median +1.75 %, robust sd 1.43 % (plain sd 2.12 %). Floor used: point 0.73 %, range 4.61 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 47.1 µs | 53.7 µs | 54.3 µs | 70.9 µs | 80.6 µs | 57.4 µs (sd 1.3) | 17,172 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 49.0 µs | 55.2 µs | 55.8 µs | 73.1 µs | 82.5 µs | 59.0 µs (sd 0.9) | 16,722 stmt/s | +1.55 µs [+1.40 µs, +1.60 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 47.7 µs | 54.8 µs | 55.5 µs | 73.0 µs | 82.4 µs | 60.0 µs (sd 1.6) | 16,270 stmt/s | +1.15 µs [+1.00 µs, +1.20 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 54.8 µs | 60.9 µs | 61.7 µs | 81.1 µs | 90.3 µs | 66.1 µs (sd 2.2) | 15,123 stmt/s | +7.35 µs [+7.20 µs, +7.50 µs] | - | - |

range (`WHERE id >= 1`, every one of the 200 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 269.4 µs | 278.9 µs | 292.6 µs | 484.0 µs | 723.4 µs | 318.3 µs (sd 9.5) | 3,136 stmt/s = 627,220 rows/s | - | 1463 ns | - |
| `SET v = v + 1` | 1,920 | 290.8 µs | 302.0 µs | 316.0 µs | 479.6 µs | 645.4 µs | 339.1 µs (sd 7.9) | 2,959 stmt/s = 591,770 rows/s | +23.60 µs [+21.30 µs, +26.00 µs] | 1580 ns | +118 ns [+107, +130] |
| `SET v = w, w = v` | 1,920 | 282.8 µs | 291.7 µs | 304.5 µs | 374.6 µs | 448.6 µs | 310.8 µs (sd 4.2) | 3,215 stmt/s = 642,940 rows/s | +11.25 µs [+9.90 µs, +14.40 µs] | 1522 ns | +56 ns [+50, +72] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 324.0 µs | 335.7 µs | 351.5 µs | 467.2 µs | 1,003 µs | 373.0 µs (sd 14.9) | 2,726 stmt/s = 545,140 rows/s | +59.15 µs [+56.80 µs, +60.50 µs] | 1758 ns | +296 ns [+284, +302] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 310.6 µs | 322.6 µs | 338.3 µs | 470.5 µs | 555.4 µs | 356.4 µs (sd 5.6) | 2,808 stmt/s = 561,640 rows/s | +45.80 µs [+43.90 µs, +48.30 µs] | 1692 ns | +229 ns [+219, +242] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 48.3 µs | 55.1 µs | 55.8 µs | 73.6 µs | 82.7 µs | 93.5 µs (sd 30.0) | 9,218 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 118.4 µs | 124.1 µs | 126.6 µs | 154.3 µs | 173.0 µs | 137.7 µs (sd 12.7) | 7,523 /s | +70.55 µs [+70.50 µs, +71.10 µs] |
|   of which the `SELECT v` | 48,000 | 59.5 µs | 64.9 µs | 65.9 µs | 85.5 µs | 95.6 µs | 73.3 µs (sd 11.3) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 53.0 µs | 58.1 µs | 59.0 µs | 77.1 µs | 85.8 µs | 63.6 µs (sd 6.8) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 173.7 µs | 184.6 µs | 190.3 µs | 221.6 µs | 248.6 µs | 226.4 µs (sd 32.6) | 4,485 /s | +134.10 µs [+133.60 µs, +135.00 µs] |


#### cores = 2, 1,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 1.81-3.72, after 2.08-3.62; other-CPU busy over the run 1.9%-28.3% (median 5.6%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 43.6 µs | 51.7 µs | 52.7 µs | 70.6 µs | 82.3 µs | 55.4 µs (sd 0.5) | 17,699 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 45.3 µs | 53.8 µs | 54.5 µs | 72.1 µs | 83.4 µs | 57.3 µs (sd 0.7) | 17,125 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 43.8 µs | 51.6 µs | 52.5 µs | 70.3 µs | 82.4 µs | 64.5 µs (sd 32.0) | 17,494 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 48.4 µs | 53.8 µs | 54.4 µs | 71.6 µs | 84.5 µs | 58.8 µs (sd 2.3) | 16,845 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 44.8 µs | 52.5 µs | 53.4 µs | 70.7 µs | 82.5 µs | 57.4 µs (sd 2.0) | 17,182 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 48.9 µs | 54.6 µs | 55.3 µs | 73.2 µs | 84.1 µs | 63.3 µs (sd 16.0) | 16,651 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 1,200 µs | 1,287 µs | 1,313 µs | 1,448 µs | 1,746 µs | 1,334 µs (sd 25.1) | 751 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 1,230 µs | 1,305 µs | 1,332 µs | 1,456 µs | 1,661 µs | 1,348 µs (sd 24.6) | 745 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 1,256 µs | 1,301 µs | 1,326 µs | 1,527 µs | 2,139 µs | 1,626 µs (sd 693.3) | 727 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 1,278 µs | 1,324 µs | 1,352 µs | 2,360 µs | 3,353 µs | 1,600 µs (sd 533.1) | 681 stmt/s |
| INSERT, pk named | A | 48,000 | 39.6 µs | 48.8 µs | 49.7 µs | 69.4 µs | 92.0 µs | 101.4 µs (sd 41.0) | 9,463 stmt/s |
| INSERT, pk named | B | 48,000 | 39.6 µs | 49.1 µs | 50.3 µs | 69.5 µs | 92.4 µs | 127.7 µs (sd 46.4) | 7,984 stmt/s |
| SELECT by pk (control) | A | 48,000 | 57.4 µs | 65.7 µs | 69.1 µs | 91.8 µs | 108.2 µs | 72.6 µs (sd 1.8) | 13,740 stmt/s |
| SELECT by pk (control) | B | 48,000 | 59.9 µs | 66.0 µs | 69.4 µs | 93.1 µs | 112.1 µs | 73.3 µs (sd 3.2) | 13,699 stmt/s |
| DELETE by pk | A | 48,000 | 41.2 µs | 48.6 µs | 49.3 µs | 66.7 µs | 76.7 µs | 51.9 µs (sd 1.2) | 19,121 stmt/s |
| DELETE by pk | B | 48,000 | 42.4 µs | 48.6 µs | 49.4 µs | 66.4 µs | 76.3 µs | 52.4 µs (sd 1.2) | 18,897 stmt/s |
| SHOW META (control) | A | 48,000 | 42.2 µs | 49.9 µs | 50.6 µs | 67.6 µs | 83.1 µs | 58.2 µs (sd 21.7) | 18,835 stmt/s |
| SHOW META (control) | B | 48,000 | 43.3 µs | 49.8 µs | 50.6 µs | 69.0 µs | 88.8 µs | 54.3 µs (sd 5.6) | 18,782 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.70 µs [+1.30 µs, +2.40 µs] | +3.22 % [+2.63, +4.59] | +1.92 µs [+1.59 µs, +2.24 µs] | -3.64 % [-4.08, -2.31] | **B slower** (+3.2 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.90 µs [+1.40 µs, +2.50 µs] | +3.62 % [+2.61, +4.83] | -5.69 µs [-21.92 µs, +3.24 µs] | -2.39 % [-6.34, -0.72] | **B slower** (+3.6 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, w = lit, by pk | +2.10 µs [+1.50 µs, +2.50 µs] | +3.91 % [+2.82, +4.70] | +5.85 µs [+1.15 µs, +13.90 µs] | -3.17 % [-6.46, -1.76] | **B slower** (+3.9 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, every row | +24.60 µs [+17.20 µs, +29.25 µs] | +1.86 % [+1.18, +2.26] | +13.89 µs [+2.74 µs, +24.23 µs] | -1.28 % [-2.06, -0.35] | CI excludes 0, inside the floor (2.9 %): not resolved |
| UPDATE SET v = lit, every row (replicate) | +22.35 µs [+9.80 µs, +44.80 µs] | +1.69 % [+0.74, +3.35] | -25.46 µs [-283.62 µs, +118.18 µs] | -4.66 % [-7.46, -3.34] | CI excludes 0, inside the floor (2.9 %): not resolved |
| INSERT, pk named | +0.40 µs [+0.10 µs, +0.90 µs] | +0.81 % [+0.20, +1.81] | +26.30 µs [-12.31 µs, +64.47 µs] | -15.33 % [-57.85, +29.35] | CI excludes 0, inside the floor (1.1 %): not resolved |
| SELECT by pk (control) | +0.35 µs [-0.20 µs, +0.80 µs] | +0.51 % [-0.29, +1.17] | +0.72 µs [-0.11 µs, +2.02 µs] | -0.20 % [-0.85, +0.27] | no change resolved |
| DELETE by pk | +0.05 µs [-0.35 µs, +0.40 µs] | +0.10 % [-0.61, +0.82] | +0.51 µs [+0.11 µs, +0.89 µs] | -1.22 % [-2.41, +0.05] | no change resolved |
| SHOW META (control) | +0.00 µs [-0.40 µs, +0.40 µs] | +0.00 % [-0.79, +0.79] | -3.87 µs [-12.11 µs, +0.43 µs] | -0.21 % [-0.71, +0.77] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ +0.00 % [-0.79, +0.79]; replicate (second relation, same binary, per run, both arms pooled) point median -0.28 %, robust sd 0.41 % (plain sd 0.58 %); range median +1.02 %, robust sd 0.96 % (plain sd 1.03 %). Floor used: point 1.11 %, range 2.95 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 48.9 µs | 53.9 µs | 54.6 µs | 83.8 µs | 107.0 µs | 74.1 µs (sd 30.9) | 16,896 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 48.7 µs | 55.5 µs | 56.2 µs | 85.1 µs | 107.9 µs | 61.7 µs (sd 9.0) | 16,491 stmt/s | +1.70 µs [+1.50 µs, +1.70 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 48.5 µs | 55.1 µs | 55.8 µs | 85.3 µs | 109.1 µs | 73.3 µs (sd 31.5) | 16,196 stmt/s | +1.25 µs [+1.10 µs, +1.40 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 53.9 µs | 61.4 µs | 62.3 µs | 95.4 µs | 120.5 µs | 78.2 µs (sd 27.9) | 14,856 stmt/s | +7.75 µs [+7.60 µs, +7.90 µs] | - | - |

range (`WHERE id >= 1`, every one of the 1,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 1,133 µs | 1,176 µs | 1,202 µs | 2,076 µs | 2,995 µs | 1,715 µs (sd 1214.1) | 773 stmt/s = 772,850 rows/s | - | 1202 ns | - |
| `SET v = v + 1` | 1,920 | 1,241 µs | 1,287 µs | 1,316 µs | 1,926 µs | 8,715 µs | 3,358 µs (sd 1492.2) | 267 stmt/s = 267,300 rows/s | +109.15 µs [+106.40 µs, +118.50 µs] | 1316 ns | +109 ns [+106, +118] |
| `SET v = w, w = v` | 1,920 | 1,192 µs | 1,243 µs | 1,269 µs | 1,941 µs | 7,289 µs | 2,097 µs (sd 1394.8) | 689 stmt/s = 688,750 rows/s | +65.30 µs [+60.00 µs, +75.20 µs] | 1269 ns | +65 ns [+60, +75] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 1,380 µs | 1,434 µs | 1,468 µs | 2,559 µs | 3,769 µs | 1,938 µs (sd 1241.5) | 641 stmt/s = 640,850 rows/s | +265.85 µs [+256.80 µs, +273.80 µs] | 1468 ns | +266 ns [+257, +274] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 1,270 µs | 1,315 µs | 1,340 µs | 1,562 µs | 2,410 µs | 1,562 µs (sd 495.3) | 717 stmt/s = 717,400 rows/s | +135.60 µs [+129.20 µs, +144.00 µs] | 1340 ns | +136 ns [+129, +144] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 46.8 µs | 55.3 µs | 56.1 µs | 85.1 µs | 109.3 µs | 86.9 µs (sd 43.9) | 16,539 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 116.1 µs | 124.5 µs | 128.4 µs | 188.7 µs | 237.7 µs | 138.8 µs (sd 17.7) | 7,462 /s | +71.50 µs [+71.20 µs, +72.10 µs] |
|   of which the `SELECT v` | 48,000 | 59.5 µs | 65.1 µs | 66.8 µs | 99.5 µs | 124.3 µs | 72.2 µs (sd 8.8) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 50.9 µs | 58.1 µs | 59.3 µs | 87.4 µs | 114.2 µs | 65.6 µs (sd 9.6) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 173.4 µs | 185.8 µs | 193.9 µs | 281.7 µs | 340.7 µs | 249.4 µs (sd 29.7) | 3,879 /s | +137.00 µs [+136.60 µs, +137.80 µs] |


#### cores = 2, 10,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 2.30-3.31, after 2.39-3.57; other-CPU busy over the run 2.4%-23.3% (median 4.9%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 46.2 µs | 52.2 µs | 53.3 µs | 71.7 µs | 88.3 µs | 57.5 µs (sd 3.1) | 17,342 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 47.8 µs | 54.1 µs | 54.8 µs | 73.7 µs | 90.1 µs | 58.7 µs (sd 1.4) | 16,854 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 46.7 µs | 52.1 µs | 53.1 µs | 70.8 µs | 84.8 µs | 56.1 µs (sd 1.3) | 17,533 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 48.6 µs | 54.0 µs | 54.7 µs | 72.8 µs | 85.8 µs | 57.9 µs (sd 0.9) | 16,992 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 47.3 µs | 52.9 µs | 53.7 µs | 70.2 µs | 80.1 µs | 56.8 µs (sd 1.1) | 17,265 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 49.4 µs | 54.8 µs | 55.6 µs | 73.3 µs | 82.3 µs | 58.9 µs (sd 0.9) | 16,710 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 11.85 ms | 12.91 ms | 13.32 ms | 24.13 ms | 383.77 ms | 25.00 ms (sd 2342.9) | 40 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 12.15 ms | 13.04 ms | 13.47 ms | 23.21 ms | 334.98 ms | 25.80 ms (sd 4176.4) | 40 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 12.37 ms | 12.91 ms | 13.39 ms | 93.65 ms | 396.24 ms | 29.72 ms (sd 3791.3) | 34 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 12.63 ms | 13.18 ms | 13.42 ms | 43.10 ms | 272.69 ms | 23.64 ms (sd 2433.2) | 44 stmt/s |
| INSERT, pk named | A | 48,000 | 39.6 µs | 49.1 µs | 50.3 µs | 73.1 µs | 112.6 µs | 73.5 µs (sd 33.2) | 17,762 stmt/s |
| INSERT, pk named | B | 48,000 | 42.0 µs | 49.4 µs | 50.8 µs | 74.4 µs | 111.6 µs | 97.0 µs (sd 90.0) | 17,697 stmt/s |
| SELECT by pk (control) | A | 48,000 | 61.4 µs | 65.6 µs | 66.7 µs | 88.7 µs | 116.8 µs | 70.8 µs (sd 4.0) | 14,150 stmt/s |
| SELECT by pk (control) | B | 48,000 | 59.0 µs | 65.7 µs | 66.7 µs | 88.1 µs | 112.4 µs | 70.5 µs (sd 3.7) | 14,141 stmt/s |
| DELETE by pk | A | 48,000 | 41.0 µs | 48.5 µs | 49.5 µs | 69.3 µs | 87.4 µs | 53.3 µs (sd 4.4) | 18,862 stmt/s |
| DELETE by pk | B | 48,000 | 41.6 µs | 48.7 µs | 49.7 µs | 69.3 µs | 86.8 µs | 53.2 µs (sd 4.2) | 18,977 stmt/s |
| SHOW META (control) | A | 48,000 | 42.6 µs | 49.7 µs | 50.4 µs | 68.1 µs | 84.1 µs | 53.4 µs (sd 4.1) | 18,929 stmt/s |
| SHOW META (control) | B | 48,000 | 42.8 µs | 49.7 µs | 50.5 µs | 67.2 µs | 80.9 µs | 53.4 µs (sd 4.0) | 18,887 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.60 µs [+1.30 µs, +1.80 µs] | +3.02 % [+2.43, +3.59] | +1.27 µs [-0.00 µs, +2.21 µs] | -2.41 % [-5.11, -1.34] | **B slower** (+3.0 % at p50, floor 1.2 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.55 µs [+1.30 µs, +2.20 µs] | +2.94 % [+2.43, +4.22] | +1.73 µs [+1.24 µs, +2.26 µs] | -2.54 % [-4.42, -1.93] | **B slower** (+2.9 % at p50, floor 1.2 %) |
| UPDATE SET v = lit, w = lit, by pk | +1.90 µs [+1.40 µs, +2.30 µs] | +3.55 % [+2.62, +4.25] | +2.11 µs [+1.36 µs, +2.85 µs] | -3.13 % [-5.01, -1.64] | **B slower** (+3.6 % at p50, floor 1.2 %) |
| UPDATE SET v = lit, every row | +261.75 µs [-15.35 µs, +427.90 µs] | +1.97 % [-0.11, +3.44] | +790.53 µs [-1,784 µs, +3,787 µs] | +1.96 % [-10.60, +13.78] | no change resolved |
| UPDATE SET v = lit, every row (replicate) | +92.60 µs [-67.40 µs, +315.70 µs] | +0.70 % [-0.50, +2.35] | -6,073 µs [-8,386 µs, -3,646 µs] | +32.07 % [+9.89, +39.79] | no change resolved |
| INSERT, pk named | +0.60 µs [-0.10 µs, +1.10 µs] | +1.19 % [-0.20, +2.23] | +23.52 µs [-18.60 µs, +79.21 µs] | -1.46 % [-27.34, +26.43] | no change resolved |
| SELECT by pk (control) | -0.05 µs [-0.40 µs, +0.50 µs] | -0.07 % [-0.60, +0.76] | -0.24 µs [-0.69 µs, +0.25 µs] | +0.49 % [-0.17, +1.27] | no change resolved |
| DELETE by pk | +0.10 µs [-0.30 µs, +0.60 µs] | +0.20 % [-0.61, +1.23] | -0.05 µs [-0.51 µs, +0.42 µs] | +0.16 % [-0.92, +1.69] | no change resolved |
| SHOW META (control) | +0.15 µs [-0.40 µs, +0.40 µs] | +0.30 % [-0.79, +0.79] | +0.02 µs [-0.24 µs, +0.26 µs] | -0.31 % [-0.65, +0.18] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ +0.30 % [-0.79, +0.79]; replicate (second relation, same binary, per run, both arms pooled) point median -0.36 %, robust sd 0.42 % (plain sd 0.58 %); range median +1.06 %, robust sd 1.15 % (plain sd 1.34 %). Floor used: point 1.21 %, range 3.36 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 48.4 µs | 54.2 µs | 55.1 µs | 76.1 µs | 102.5 µs | 59.5 µs (sd 4.4) | 16,762 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 52.0 µs | 55.8 µs | 56.7 µs | 77.2 µs | 101.2 µs | 62.4 µs (sd 5.9) | 16,264 stmt/s | +1.65 µs [+1.55 µs, +1.75 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 50.1 µs | 55.4 µs | 56.3 µs | 76.5 µs | 101.6 µs | 62.7 µs (sd 5.2) | 16,054 stmt/s | +1.30 µs [+1.20 µs, +1.40 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 55.7 µs | 61.6 µs | 62.6 µs | 84.5 µs | 107.9 µs | 70.9 µs (sd 5.0) | 14,102 stmt/s | +7.50 µs [+7.35 µs, +7.70 µs] | - | - |

range (`WHERE id >= 1`, every one of the 10,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 11.05 ms | 11.37 ms | 11.50 ms | 17.71 ms | 327.54 ms | 19.60 ms (sd 1586.7) | 51 stmt/s = 510,000 rows/s | - | 1150 ns | - |
| `SET v = v + 1` | 1,920 | 12.07 ms | 12.52 ms | 12.63 ms | 18.24 ms | 339.75 ms | 23.57 ms (sd 1351.5) | 43 stmt/s = 427,000 rows/s | +1,153 µs [+1,074 µs, +1,194 µs] | 1263 ns | +115 ns [+107, +119] |
| `SET v = w, w = v` | 1,920 | 11.66 ms | 12.06 ms | 12.18 ms | 15.64 ms | 61.52 ms | 16.22 ms (sd 1912.7) | 65 stmt/s = 650,000 rows/s | +706.20 µs [+666.70 µs, +718.30 µs] | 1218 ns | +71 ns [+67, +72] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 13.45 ms | 13.91 ms | 14.03 ms | 56.61 ms | 346.54 ms | 29.58 ms (sd 4099.1) | 35 stmt/s = 348,500 rows/s | +2,557 µs [+2,525 µs, +2,580 µs] | 1403 ns | +256 ns [+252, +258] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 12.19 ms | 12.62 ms | 12.73 ms | 18.72 ms | 319.95 ms | 20.43 ms (sd 1452.9) | 48 stmt/s = 483,000 rows/s | +1,229 µs [+1,208 µs, +1,264 µs] | 1273 ns | +123 ns [+121, +126] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 46.0 µs | 55.5 µs | 56.3 µs | 74.6 µs | 86.1 µs | 92.7 µs (sd 64.8) | 16,538 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 116.8 µs | 123.7 µs | 126.3 µs | 157.5 µs | 191.0 µs | 170.4 µs (sd 47.7) | 7,405 /s | +70.10 µs [+69.60 µs, +70.50 µs] |
|   of which the `SELECT v` | 48,000 | 57.1 µs | 64.8 µs | 65.9 µs | 86.1 µs | 99.2 µs | 80.5 µs (sd 29.1) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 51.0 µs | 57.8 µs | 58.7 µs | 78.1 µs | 92.7 µs | 89.1 µs (sd 34.8) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 174.9 µs | 184.0 µs | 188.8 µs | 225.4 µs | 273.4 µs | 217.5 µs (sd 30.0) | 4,999 /s | +132.30 µs [+131.80 µs, +133.40 µs] |


##### SUMMARY point: extra cost per statement over `SET v = lit` (B)

| | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| `SET v = v + 1` | +1.75 µs [+1.60, +1.90] | +1.80 µs [+1.65, +1.90] | +1.65 µs [+1.50, +1.80] | +1.55 µs [+1.40, +1.60] | +1.70 µs [+1.50, +1.70] | +1.65 µs [+1.55, +1.75] |
| `SET v = w, w = v` | +1.50 µs [+1.30, +1.60] | +1.40 µs [+1.20, +1.60] | +1.30 µs [+1.20, +1.40] | +1.15 µs [+1.00, +1.20] | +1.25 µs [+1.10, +1.40] | +1.30 µs [+1.20, +1.40] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | +7.85 µs [+7.70, +8.00] | +7.70 µs [+7.50, +8.00] | +7.80 µs [+7.70, +7.90] | +7.35 µs [+7.20, +7.50] | +7.75 µs [+7.60, +7.90] | +7.50 µs [+7.35, +7.70] |

##### SUMMARY range: extra cost per row over `SET v = lit` (B), ns per row

| | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| `SET v = v + 1` | +130 ns [+123, +140] | +108 ns [+106, +113] | +117 ns [+115, +119] | +118 ns [+107, +130] | +109 ns [+106, +118] | +115 ns [+107, +119] |
| `SET v = w, w = v` | +75 ns [+66, +92] | +68 ns [+65, +72] | +74 ns [+67, +81] | +56 ns [+50, +72] | +65 ns [+60, +75] | +71 ns [+67, +72] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | +330 ns [+322, +338] | +284 ns [+278, +290] | +262 ns [+259, +266] | +296 ns [+284, +302] | +266 ns [+257, +274] | +256 ns [+252, +258] |
| `SET v = (SELECT COUNT(*) FROM t)` | +237 ns [+222, +241] | +137 ns [+133, +139] | +128 ns [+125, +132] | +229 ns [+219, +242] | +136 ns [+129, +144] | +123 ns [+121, +126] |

##### SUMMARY range: extra cost per statement over `SET v = lit` (B)

| | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| `SET v = v + 1` | +25.95 µs [+24.70 µs, +28.10 µs] | +108.35 µs [+106.40 µs, +113.40 µs] | +1,167 µs [+1,150 µs, +1,191 µs] | +23.60 µs [+21.30 µs, +26.00 µs] | +109.15 µs [+106.40 µs, +118.50 µs] | +1,153 µs [+1,074 µs, +1,194 µs] |
| `SET v = w, w = v` | +15.00 µs [+13.30 µs, +18.40 µs] | +68.25 µs [+65.00 µs, +71.50 µs] | +737.65 µs [+673.50 µs, +808.70 µs] | +11.25 µs [+9.90 µs, +14.40 µs] | +65.30 µs [+60.00 µs, +75.20 µs] | +706.20 µs [+666.70 µs, +718.30 µs] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | +65.90 µs [+64.40 µs, +67.70 µs] | +283.90 µs [+277.60 µs, +289.80 µs] | +2,624 µs [+2,590 µs, +2,662 µs] | +59.15 µs [+56.80 µs, +60.50 µs] | +265.85 µs [+256.80 µs, +273.80 µs] | +2,557 µs [+2,525 µs, +2,580 µs] |
| `SET v = (SELECT COUNT(*) FROM t)` | +47.40 µs [+44.40 µs, +48.20 µs] | +136.55 µs [+133.20 µs, +138.95 µs] | +1,283 µs [+1,250 µs, +1,318 µs] | +45.80 µs [+43.90 µs, +48.30 µs] | +135.60 µs [+129.20 µs, +144.00 µs] | +1,229 µs [+1,208 µs, +1,264 µs] |

##### SUMMARY range: p50 per row, ns (B)

| | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| `SET v = lit` | 1209 ns | 959 ns | 922 ns | 1463 ns | 1202 ns | 1150 ns |
| `SET v = v + 1` | 1334 ns | 1068 ns | 1036 ns | 1580 ns | 1316 ns | 1263 ns |
| `SET v = w, w = v` | 1287 ns | 1031 ns | 1004 ns | 1522 ns | 1269 ns | 1218 ns |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1534 ns | 1243 ns | 1181 ns | 1758 ns | 1468 ns | 1403 ns |
| `SET v = (SELECT COUNT(*) FROM t)` | 1434 ns | 1095 ns | 1053 ns | 1692 ns | 1340 ns | 1273 ns |

##### SUMMARY point: p50 (B)

| | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| `SET v = lit` | 53.3 µs | 53.3 µs | 53.9 µs | 54.3 µs | 54.6 µs | 55.1 µs |
| `SET v = v + 1` | 55.0 µs | 55.0 µs | 55.5 µs | 55.8 µs | 56.2 µs | 56.7 µs |
| `SET v = w, w = v` | 54.7 µs | 54.7 µs | 55.2 µs | 55.5 µs | 55.8 µs | 56.3 µs |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 61.0 µs | 61.1 µs | 61.7 µs | 61.7 µs | 62.3 µs | 62.6 µs |

##### SUMMARY round trip: p50, derived statements/s = 1e6 / p50

| | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| rt-one | 54.9 µs = 18,215 /s | 55.0 µs = 18,182 /s | 55.5 µs = 18,018 /s | 55.8 µs = 17,921 /s | 56.1 µs = 17,825 /s | 56.3 µs = 17,762 /s |
| rt-pair | 124.2 µs = 8,052 /s | 125.3 µs = 7,981 /s | 125.0 µs = 8,000 /s | 126.6 µs = 7,899 /s | 128.4 µs = 7,788 /s | 126.3 µs = 7,918 /s |
| rt-txn | 186.9 µs = 5,350 /s | 190.7 µs = 5,244 /s | 188.5 µs = 5,305 /s | 190.3 µs = 5,255 /s | 193.9 µs = 5,157 /s | 188.8 µs = 5,297 /s |
| rt-pair minus rt-one | +69.1 µs [+68.8, +69.4] | +69.8 µs [+69.5, +70.0] | +68.8 µs [+68.6, +69.5] | +70.6 µs [+70.5, +71.1] | +71.5 µs [+71.2, +72.1] | +70.1 µs [+69.6, +70.5] |
| rt-txn minus rt-one | +131.2 µs [+130.7, +132.0] | +134.9 µs [+134.9, +135.3] | +132.2 µs [+131.1, +133.0] | +134.1 µs [+133.6, +135.0] | +137.0 µs [+136.6, +137.8] | +132.3 µs [+131.8, +133.4] |
