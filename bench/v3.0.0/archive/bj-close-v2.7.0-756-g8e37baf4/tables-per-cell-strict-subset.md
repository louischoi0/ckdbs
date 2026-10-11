
#### cores = 1, 200 rows

16 runs used of 16 (0 discarded: other-CPU busy > 10% or verify/bad flag); loadavg(1 min) before each run 2.06-6.72, after 1.98-6.00; other-CPU busy over the run 2.3%-9.0% (median 3.3%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

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

12 runs used of 16 (4 discarded: other-CPU busy > 10% or verify/bad flag); loadavg(1 min) before each run 2.01-9.10, after 2.03-7.93; other-CPU busy over the run 2.2%-3.3% (median 2.6%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 36,000 | 40.0 µs | 50.6 µs | 51.4 µs | 66.7 µs | 83.8 µs | 53.8 µs (sd 1.0) | 18,335 stmt/s |
| UPDATE SET v = lit, by pk | B | 36,000 | 43.3 µs | 52.3 µs | 53.2 µs | 68.1 µs | 85.0 µs | 56.1 µs (sd 2.3) | 17,695 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 36,000 | 41.1 µs | 50.7 µs | 51.4 µs | 66.2 µs | 80.2 µs | 53.9 µs (sd 0.7) | 18,203 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 36,000 | 43.8 µs | 52.4 µs | 53.2 µs | 68.1 µs | 82.3 µs | 56.5 µs (sd 0.9) | 17,357 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 36,000 | 43.8 µs | 51.3 µs | 52.1 µs | 67.1 µs | 82.0 µs | 54.8 µs (sd 0.8) | 17,887 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 36,000 | 45.5 µs | 53.6 µs | 54.2 µs | 68.6 µs | 84.5 µs | 57.3 µs (sd 1.1) | 17,049 stmt/s |
| UPDATE SET v = lit, every row | A | 1,440 | 903.1 µs | 936.3 µs | 956.3 µs | 1,094 µs | 1,374 µs | 984.5 µs (sd 12.2) | 1,017 stmt/s |
| UPDATE SET v = lit, every row | B | 1,440 | 914.0 µs | 947.3 µs | 964.6 µs | 1,100 µs | 1,400 µs | 995.6 µs (sd 13.5) | 1,006 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,440 | 900.5 µs | 935.0 µs | 951.7 µs | 1,101 µs | 1,518 µs | 992.3 µs (sd 23.7) | 1,013 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,440 | 910.0 µs | 945.3 µs | 970.1 µs | 2,329 µs | 3,322 µs | 1,162 µs (sd 19.1) | 865 stmt/s |
| INSERT, pk named | A | 36,000 | 37.9 µs | 48.5 µs | 49.2 µs | 64.0 µs | 78.8 µs | 87.4 µs (sd 62.3) | 18,738 stmt/s |
| INSERT, pk named | B | 36,000 | 38.8 µs | 48.7 µs | 49.4 µs | 63.6 µs | 77.6 µs | 158.2 µs (sd 66.5) | 5,249 stmt/s |
| SELECT by pk (control) | A | 36,000 | 59.8 µs | 64.9 µs | 67.5 µs | 86.6 µs | 102.3 µs | 71.7 µs (sd 3.5) | 13,998 stmt/s |
| SELECT by pk (control) | B | 36,000 | 59.5 µs | 64.9 µs | 67.7 µs | 87.1 µs | 102.9 µs | 70.8 µs (sd 0.8) | 13,998 stmt/s |
| DELETE by pk | A | 36,000 | 40.3 µs | 48.1 µs | 48.8 µs | 62.8 µs | 73.3 µs | 51.5 µs (sd 1.7) | 19,399 stmt/s |
| DELETE by pk | B | 36,000 | 39.8 µs | 48.2 µs | 48.9 µs | 61.9 µs | 72.3 µs | 50.7 µs (sd 0.5) | 19,471 stmt/s |
| SHOW META (control) | A | 36,000 | 44.5 µs | 49.5 µs | 50.1 µs | 61.9 µs | 70.0 µs | 52.0 µs (sd 0.3) | 19,083 stmt/s |
| SHOW META (control) | B | 36,000 | 44.2 µs | 49.7 µs | 50.3 µs | 62.4 µs | 71.0 µs | 52.3 µs (sd 0.3) | 18,971 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.60 µs [+1.30 µs, +2.40 µs] | +3.12 % [+2.53, +4.69] | +2.32 µs [+1.36 µs, +3.58 µs] | -3.89 % [-4.93, -1.99] | **B slower** (+3.1 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.60 µs [+1.35 µs, +2.40 µs] | +3.13 % [+2.61, +4.69] | +2.52 µs [+1.85 µs, +3.17 µs] | -4.36 % [-5.95, -3.30] | **B slower** (+3.1 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, w = lit, by pk | +2.00 µs [+1.65 µs, +2.90 µs] | +3.85 % [+3.15, +5.61] | +2.52 µs [+1.75 µs, +3.29 µs] | -4.87 % [-6.04, -2.20] | **B slower** (+3.8 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, every row | +11.40 µs [+2.15 µs, +14.95 µs] | +1.19 % [+0.22, +1.57] | +11.06 µs [+2.02 µs, +21.42 µs] | -0.86 % [-1.79, -0.35] | CI excludes 0, inside the floor (2.2 %): not resolved |
| UPDATE SET v = lit, every row (replicate) | +19.10 µs [+7.60 µs, +25.65 µs] | +2.03 % [+0.80, +2.70] | +169.30 µs [+157.48 µs, +180.35 µs] | -14.70 % [-16.02, -13.25] | CI excludes 0, inside the floor (2.2 %): not resolved |
| INSERT, pk named | +0.30 µs [+0.05 µs, +0.40 µs] | +0.61 % [+0.10, +0.82] | +70.82 µs [-2.85 µs, +135.83 µs] | -71.17 % [-73.67, +82.91] | CI excludes 0, inside the floor (1.1 %): not resolved |
| SELECT by pk (control) | +0.00 µs [-0.20 µs, +0.40 µs] | +0.00 % [-0.30, +0.59] | -0.90 µs [-3.12 µs, +0.38 µs] | +0.05 % [-0.20, +0.20] | no change resolved |
| DELETE by pk | +0.10 µs [+0.00 µs, +0.30 µs] | +0.21 % [+0.00, +0.62] | -0.83 µs [-1.82 µs, +0.02 µs] | +0.34 % [-0.85, +3.12] | no change resolved |
| SHOW META (control) | +0.20 µs [+0.15 µs, +0.30 µs] | +0.40 % [+0.30, +0.60] | +0.26 µs [+0.09 µs, +0.42 µs] | -0.46 % [-0.76, -0.12] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ +0.40 % [+0.30, +0.60]; replicate (second relation, same binary, per run, both arms pooled) point median +0.00 %, robust sd 0.56 % (plain sd 0.51 %); range median -0.10 %, robust sd 1.07 % (plain sd 1.07 %). Floor used: point 1.12 %, range 2.24 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 36,000 | 43.9 µs | 52.6 µs | 53.3 µs | 67.2 µs | 77.9 µs | 56.6 µs (sd 1.9) | 17,448 stmt/s | - | - | - |
| `SET v = v + 1` | 36,000 | 46.1 µs | 54.4 µs | 55.1 µs | 69.8 µs | 80.9 µs | 59.9 µs (sd 8.3) | 16,970 stmt/s | +1.80 µs [+1.60 µs, +2.00 µs] | - | - |
| `SET v = w, w = v` | 36,000 | 46.2 µs | 54.1 µs | 54.7 µs | 70.0 µs | 82.1 µs | 58.3 µs (sd 2.0) | 16,990 stmt/s | +1.35 µs [+1.20 µs, +1.50 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 36,000 | 50.8 µs | 60.3 µs | 61.1 µs | 76.8 µs | 87.8 µs | 65.0 µs (sd 2.0) | 15,201 stmt/s | +7.70 µs [+7.45 µs, +8.00 µs] | - | - |

range (`WHERE id >= 1`, every one of the 1,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,440 | 902.0 µs | 938.7 µs | 956.7 µs | 1,460 µs | 2,895 µs | 1,056 µs (sd 21.0) | 944 stmt/s = 944,400 rows/s | - | 957 ns | - |
| `SET v = v + 1` | 1,440 | 1,020 µs | 1,048 µs | 1,066 µs | 1,316 µs | 71.36 ms | 3,778 µs (sd 1379.4) | 232 stmt/s = 232,500 rows/s | +109.60 µs [+105.15 µs, +114.50 µs] | 1066 ns | +110 ns [+105, +114] |
| `SET v = w, w = v` | 1,440 | 983.3 µs | 1,011 µs | 1,030 µs | 1,221 µs | 6,404 µs | 1,368 µs (sd 717.5) | 886 stmt/s = 885,950 rows/s | +67.50 µs [+64.80 µs, +71.35 µs] | 1030 ns | +68 ns [+65, +71] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,440 | 1,180 µs | 1,219 µs | 1,241 µs | 1,622 µs | 4,176 µs | 1,694 µs (sd 989.8) | 740 stmt/s = 739,600 rows/s | +286.60 µs [+278.35 µs, +292.00 µs] | 1241 ns | +287 ns [+278, +292] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,440 | 1,048 µs | 1,074 µs | 1,093 µs | 1,419 µs | 4,167 µs | 1,601 µs (sd 1111.9) | 841 stmt/s = 841,050 rows/s | +136.90 µs [+133.55 µs, +141.20 µs] | 1093 ns | +137 ns [+134, +141] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 36,000 | 44.9 µs | 54.3 µs | 54.9 µs | 69.9 µs | 82.0 µs | 83.4 µs (sd 38.7) | 17,168 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 36,000 | 114.7 µs | 122.3 µs | 124.6 µs | 151.0 µs | 171.9 µs | 129.7 µs (sd 0.7) | 7,670 /s | +69.65 µs [+69.40 µs, +69.85 µs] |
|   of which the `SELECT v` | 36,000 | 56.1 µs | 64.0 µs | 65.2 µs | 82.2 µs | 95.5 µs | 68.3 µs (sd 0.4) | - | - |
|   of which the `UPDATE SET v = <lit>` | 36,000 | 50.4 µs | 57.0 µs | 57.9 µs | 73.0 µs | 85.2 µs | 60.5 µs (sd 0.3) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 36,000 | 174.4 µs | 182.7 µs | 189.7 µs | 221.5 µs | 251.3 µs | 244.9 µs (sd 36.5) | 3,738 /s | +134.90 µs [+134.70 µs, +135.05 µs] |


#### cores = 1, 10,000 rows

5 runs used of 16 (11 discarded: other-CPU busy > 10% or verify/bad flag); loadavg(1 min) before each run 2.25-6.32, after 2.98-5.09; other-CPU busy over the run 2.9%-5.4% (median 4.7%); 0 errors; gate wait median 6 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 15,000 | 40.1 µs | 50.8 µs | 51.5 µs | 66.8 µs | 83.6 µs | 54.4 µs (sd 1.2) | 18,261 stmt/s |
| UPDATE SET v = lit, by pk | B | 15,000 | 43.6 µs | 52.9 µs | 53.7 µs | 69.1 µs | 85.1 µs | 56.5 µs (sd 0.5) | 17,310 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 15,000 | 43.3 µs | 50.8 µs | 51.5 µs | 66.5 µs | 80.4 µs | 53.7 µs (sd 0.5) | 18,318 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 15,000 | 45.5 µs | 52.9 µs | 53.6 µs | 68.7 µs | 85.7 µs | 56.1 µs (sd 0.4) | 17,517 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 15,000 | 43.1 µs | 51.3 µs | 52.1 µs | 67.4 µs | 83.0 µs | 54.6 µs (sd 0.7) | 17,980 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 15,000 | 46.5 µs | 53.8 µs | 54.4 µs | 70.1 µs | 86.3 µs | 58.0 µs (sd 1.0) | 16,993 stmt/s |
| UPDATE SET v = lit, every row | A | 600 | 9,308 µs | 9,615 µs | 9,742 µs | 18.96 ms | 462.62 ms | 21.08 ms (sd 551.0) | 48 stmt/s |
| UPDATE SET v = lit, every row | B | 600 | 9,534 µs | 9,775 µs | 9,911 µs | 16.72 ms | 387.67 ms | 17.33 ms (sd 557.4) | 58 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 600 | 9,275 µs | 9,597 µs | 9,758 µs | 18.47 ms | 807.83 ms | 36.17 ms (sd 2255.3) | 27 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 600 | 9,501 µs | 9,766 µs | 9,914 µs | 16.52 ms | 51.74 ms | 11.89 ms (sd 1609.3) | 89 stmt/s |
| INSERT, pk named | A | 15,000 | 42.7 µs | 48.4 µs | 49.2 µs | 68.3 µs | 87.2 µs | 53.1 µs (sd 1.3) | 18,669 stmt/s |
| INSERT, pk named | B | 15,000 | 41.9 µs | 48.6 µs | 49.5 µs | 70.8 µs | 91.4 µs | 54.1 µs (sd 4.3) | 18,845 stmt/s |
| SELECT by pk (control) | A | 15,000 | 59.0 µs | 63.8 µs | 64.6 µs | 81.8 µs | 100.1 µs | 68.7 µs (sd 2.7) | 14,515 stmt/s |
| SELECT by pk (control) | B | 15,000 | 60.5 µs | 64.2 µs | 65.0 µs | 85.4 µs | 103.4 µs | 68.3 µs (sd 1.7) | 14,580 stmt/s |
| DELETE by pk | A | 15,000 | 40.4 µs | 48.0 µs | 48.9 µs | 64.6 µs | 75.6 µs | 51.5 µs (sd 0.4) | 19,229 stmt/s |
| DELETE by pk | B | 15,000 | 41.4 µs | 48.3 µs | 49.1 µs | 64.9 µs | 76.6 µs | 52.8 µs (sd 0.8) | 18,829 stmt/s |
| SHOW META (control) | A | 15,000 | 44.5 µs | 49.5 µs | 50.2 µs | 64.0 µs | 72.6 µs | 52.4 µs (sd 0.2) | 18,909 stmt/s |
| SHOW META (control) | B | 15,000 | 45.4 µs | 49.5 µs | 50.1 µs | 63.1 µs | 72.3 µs | 52.2 µs (sd 0.3) | 18,960 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +2.30 µs [+1.70 µs, +2.50 µs] | +4.47 % [+3.29, +4.88] | +2.06 µs [+1.20 µs, +2.82 µs] | -3.70 % [-5.33, -0.96] | **B slower** (+4.5 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, by pk (replicate) | +2.10 µs [+1.70 µs, +2.40 µs] | +4.08 % [+3.31, +4.69] | +2.32 µs [+1.92 µs, +2.66 µs] | -4.19 % [-4.90, -2.90] | **B slower** (+4.1 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, w = lit, by pk | +2.40 µs [+1.80 µs, +2.80 µs] | +4.59 % [+3.42, +5.41] | +3.38 µs [+2.22 µs, +4.60 µs] | -5.49 % [-9.35, -2.53] | **B slower** (+4.6 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, every row | +160.10 µs [+89.20 µs, +253.90 µs] | +1.65 % [+0.91, +2.62] | -3,756 µs [-4,058 µs, -3,400 µs] | +20.95 % [+17.89, +25.58] | **B slower** (+1.6 % at p50, floor 0.5 %) |
| UPDATE SET v = lit, every row (replicate) | +152.10 µs [+133.60 µs, +238.40 µs] | +1.57 % [+1.37, +2.44] | -24.28 ms [-26.82 ms, -20.85 ms] | +222.83 % [+121.90, +257.20] | **B slower** (+1.6 % at p50, floor 0.5 %) |
| INSERT, pk named | +0.00 µs [-0.20 µs, +1.20 µs] | +0.00 % [-0.41, +2.43] | +1.02 µs [-0.72 µs, +3.86 µs] | +0.72 % [-10.77, +2.36] | no change resolved |
| SELECT by pk (control) | +0.20 µs [+0.00 µs, +0.90 µs] | +0.31 % [+0.00, +1.41] | -0.34 µs [-3.14 µs, +1.66 µs] | -0.10 % [-3.78, +8.33] | no change resolved |
| DELETE by pk | +0.20 µs [-0.20 µs, +0.40 µs] | +0.41 % [-0.41, +0.82] | +1.32 µs [+0.72 µs, +2.34 µs] | -1.54 % [-5.98, -1.28] | no change resolved |
| SHOW META (control) | -0.10 µs [-0.20 µs, +0.10 µs] | -0.20 % [-0.40, +0.20] | -0.20 µs [-0.32 µs, -0.08 µs] | +0.31 % [+0.20, +0.78] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ -0.20 % [-0.40, +0.20]; replicate (second relation, same binary, per run, both arms pooled) point median -0.19 %, robust sd 0.28 % (plain sd 0.18 %); range median +0.14 %, robust sd 0.19 % (plain sd 0.35 %). Floor used: point 0.74 %, range 0.52 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 15,000 | 44.8 µs | 53.3 µs | 54.1 µs | 72.8 µs | 90.7 µs | 63.6 µs (sd 8.8) | 15,761 stmt/s | - | - | - |
| `SET v = v + 1` | 15,000 | 46.7 µs | 54.8 µs | 55.6 µs | 74.6 µs | 90.9 µs | 67.4 µs (sd 11.3) | 16,031 stmt/s | +1.60 µs [+1.20 µs, +1.80 µs] | - | - |
| `SET v = w, w = v` | 15,000 | 46.9 µs | 54.6 µs | 55.3 µs | 73.1 µs | 87.5 µs | 69.8 µs (sd 23.0) | 16,290 stmt/s | +1.20 µs [+0.80 µs, +1.40 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 15,000 | 51.8 µs | 60.9 µs | 61.8 µs | 81.4 µs | 94.6 µs | 66.6 µs (sd 1.6) | 14,743 stmt/s | +7.70 µs [+7.30 µs, +7.90 µs] | - | - |

range (`WHERE id >= 1`, every one of the 10,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 600 | 8,748 µs | 8,996 µs | 9,339 µs | 16.44 ms | 414.15 ms | 20.59 ms (sd 5761.3) | 50 stmt/s = 504,000 rows/s | - | 934 ns | - |
| `SET v = v + 1` | 600 | 9,768 µs | 10.20 ms | 10.54 ms | 19.96 ms | 402.56 ms | 23.72 ms (sd 1352.3) | 42 stmt/s = 417,000 rows/s | +1,169 µs [+1,114 µs, +1,310 µs] | 1054 ns | +117 ns [+111, +131] |
| `SET v = w, w = v` | 600 | 9,423 µs | 9,750 µs | 10.19 ms | 16.91 ms | 69.35 ms | 14.49 ms (sd 1430.8) | 72 stmt/s = 720,000 rows/s | +697.50 µs [+613.90 µs, +1,045 µs] | 1019 ns | +70 ns [+61, +105] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 600 | 11.32 ms | 11.62 ms | 11.89 ms | 18.57 ms | 423.27 ms | 27.96 ms (sd 1283.6) | 36 stmt/s = 362,000 rows/s | +2,590 µs [+2,514 µs, +2,721 µs] | 1189 ns | +259 ns [+251, +272] |
| `SET v = (SELECT COUNT(*) FROM t)` | 600 | 9,924 µs | 10.32 ms | 10.72 ms | 18.39 ms | 419.70 ms | 21.41 ms (sd 1780.2) | 47 stmt/s = 472,000 rows/s | +1,265 µs [+1,121 µs, +1,470 µs] | 1072 ns | +127 ns [+112, +147] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 15,000 | 45.9 µs | 54.8 µs | 56.1 µs | 79.0 µs | 176.7 µs | 98.5 µs (sd 48.3) | 14,237 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 15,000 | 115.9 µs | 122.5 µs | 127.2 µs | 162.5 µs | 190.3 µs | 140.2 µs (sd 11.5) | 7,480 /s | +69.10 µs [+68.30 µs, +82.30 µs] |
|   of which the `SELECT v` | 15,000 | 56.1 µs | 63.9 µs | 65.7 µs | 87.9 µs | 103.3 µs | 71.9 µs (sd 5.2) | - | - |
|   of which the `UPDATE SET v = <lit>` | 15,000 | 51.7 µs | 57.3 µs | 58.9 µs | 78.3 µs | 91.3 µs | 67.4 µs (sd 8.5) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 15,000 | 173.7 µs | 182.9 µs | 191.8 µs | 238.0 µs | 273.3 µs | 200.3 µs (sd 12.3) | 5,069 /s | +131.90 µs [+129.60 µs, +152.70 µs] |


#### cores = 2, 200 rows

11 runs used of 16 (5 discarded: other-CPU busy > 10% or verify/bad flag); loadavg(1 min) before each run 2.04-3.00, after 1.96-2.92; other-CPU busy over the run 2.9%-9.8% (median 5.8%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 33,000 | 46.6 µs | 51.6 µs | 52.6 µs | 70.8 µs | 83.1 µs | 55.6 µs (sd 0.8) | 17,648 stmt/s |
| UPDATE SET v = lit, by pk | B | 33,000 | 48.1 µs | 53.7 µs | 54.4 µs | 72.5 µs | 85.8 µs | 57.7 µs (sd 1.2) | 17,166 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 33,000 | 46.8 µs | 51.6 µs | 52.5 µs | 70.3 µs | 82.0 µs | 56.0 µs (sd 2.1) | 17,632 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 33,000 | 47.8 µs | 53.7 µs | 54.3 µs | 71.6 µs | 83.1 µs | 58.2 µs (sd 1.1) | 16,948 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 33,000 | 44.7 µs | 52.6 µs | 53.4 µs | 70.9 µs | 80.2 µs | 57.0 µs (sd 1.5) | 17,310 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 33,000 | 49.2 µs | 54.5 µs | 55.2 µs | 72.8 µs | 83.1 µs | 58.6 µs (sd 1.1) | 16,745 stmt/s |
| UPDATE SET v = lit, every row | A | 1,320 | 276.5 µs | 296.3 µs | 307.2 µs | 364.2 µs | 417.2 µs | 313.5 µs (sd 3.3) | 3,192 stmt/s |
| UPDATE SET v = lit, every row | B | 1,320 | 281.4 µs | 301.7 µs | 314.7 µs | 375.8 µs | 419.1 µs | 321.1 µs (sd 4.2) | 3,112 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,320 | 282.1 µs | 304.4 µs | 318.9 µs | 375.2 µs | 425.6 µs | 324.0 µs (sd 6.8) | 3,097 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,320 | 288.2 µs | 306.1 µs | 318.8 µs | 384.6 µs | 491.4 µs | 328.4 µs (sd 8.8) | 3,071 stmt/s |
| INSERT, pk named | A | 33,000 | 45.2 µs | 49.0 µs | 49.7 µs | 68.1 µs | 81.7 µs | 53.5 µs (sd 1.6) | 18,416 stmt/s |
| INSERT, pk named | B | 33,000 | 43.7 µs | 49.1 µs | 50.0 µs | 68.7 µs | 83.6 µs | 53.3 µs (sd 1.4) | 18,686 stmt/s |
| SELECT by pk (control) | A | 33,000 | 60.2 µs | 65.5 µs | 66.4 µs | 86.6 µs | 99.6 µs | 70.2 µs (sd 0.6) | 14,086 stmt/s |
| SELECT by pk (control) | B | 33,000 | 60.9 µs | 65.4 µs | 66.4 µs | 85.9 µs | 99.8 µs | 69.8 µs (sd 0.8) | 14,172 stmt/s |
| DELETE by pk | A | 33,000 | 42.9 µs | 48.5 µs | 49.2 µs | 64.2 µs | 72.6 µs | 51.4 µs (sd 0.3) | 19,275 stmt/s |
| DELETE by pk | B | 33,000 | 43.9 µs | 48.6 µs | 49.3 µs | 65.2 µs | 73.7 µs | 52.1 µs (sd 0.7) | 18,981 stmt/s |
| SHOW META (control) | A | 33,000 | 45.0 µs | 49.7 µs | 50.4 µs | 64.3 µs | 72.8 µs | 52.8 µs (sd 1.1) | 18,864 stmt/s |
| SHOW META (control) | B | 33,000 | 45.1 µs | 49.8 µs | 50.5 µs | 65.4 µs | 73.9 µs | 53.2 µs (sd 1.1) | 18,787 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.90 µs [+1.40 µs, +2.30 µs] | +3.63 % [+2.65, +4.40] | +2.12 µs [+1.44 µs, +2.96 µs] | -2.91 % [-4.78, -1.83] | **B slower** (+3.6 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, by pk (replicate) | +2.10 µs [+1.30 µs, +2.30 µs] | +4.04 % [+2.46, +4.41] | +2.11 µs [+0.76 µs, +3.34 µs] | -3.35 % [-6.54, -2.36] | **B slower** (+4.0 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, w = lit, by pk | +2.10 µs [+1.30 µs, +2.40 µs] | +3.95 % [+2.41, +4.50] | +1.63 µs [+0.62 µs, +2.65 µs] | -2.39 % [-5.25, -0.97] | **B slower** (+4.0 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, every row | +5.70 µs [+3.90 µs, +10.10 µs] | +1.84 % [+1.26, +3.27] | +7.61 µs [+4.96 µs, +10.45 µs] | -2.09 % [-3.56, -0.63] | CI excludes 0, inside the floor (4.4 %): not resolved |
| UPDATE SET v = lit, every row (replicate) | +4.70 µs [-6.40 µs, +5.60 µs] | +1.49 % [-1.98, +1.76] | +4.35 µs [-2.55 µs, +10.98 µs] | -1.60 % [-2.90, +1.23] | no change resolved |
| INSERT, pk named | +0.30 µs [-0.30 µs, +0.40 µs] | +0.60 % [-0.60, +0.81] | -0.23 µs [-0.99 µs, +0.63 µs] | +1.05 % [-2.08, +2.57] | no change resolved |
| SELECT by pk (control) | +0.10 µs [-0.50 µs, +0.40 µs] | +0.15 % [-0.75, +0.60] | -0.35 µs [-0.69 µs, +0.04 µs] | +0.90 % [-0.22, +1.18] | no change resolved |
| DELETE by pk | +0.10 µs [+0.00 µs, +0.30 µs] | +0.20 % [+0.00, +0.61] | +0.75 µs [+0.35 µs, +1.18 µs] | -1.22 % [-2.24, -0.20] | no change resolved |
| SHOW META (control) | +0.20 µs [-0.20 µs, +0.30 µs] | +0.40 % [-0.40, +0.60] | +0.43 µs [-0.52 µs, +1.28 µs] | -0.91 % [-2.29, +0.69] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ +0.40 % [-0.40, +0.60]; replicate (second relation, same binary, per run, both arms pooled) point median -0.18 %, robust sd 0.27 % (plain sd 0.28 %); range median +2.05 %, robust sd 1.19 % (plain sd 2.27 %). Floor used: point 0.73 %, range 4.43 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 33,000 | 47.1 µs | 53.6 µs | 54.3 µs | 70.5 µs | 79.5 µs | 57.6 µs (sd 1.5) | 17,181 stmt/s | - | - | - |
| `SET v = v + 1` | 33,000 | 49.0 µs | 55.1 µs | 55.8 µs | 73.1 µs | 82.8 µs | 58.8 µs (sd 0.8) | 16,765 stmt/s | +1.60 µs [+1.40 µs, +1.70 µs] | - | - |
| `SET v = w, w = v` | 33,000 | 49.6 µs | 54.7 µs | 55.4 µs | 72.8 µs | 81.8 µs | 59.9 µs (sd 1.8) | 16,458 stmt/s | +1.20 µs [+1.00 µs, +1.30 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 33,000 | 54.8 µs | 60.9 µs | 61.7 µs | 81.1 µs | 90.4 µs | 66.4 µs (sd 2.4) | 15,109 stmt/s | +7.50 µs [+7.30 µs, +7.60 µs] | - | - |

range (`WHERE id >= 1`, every one of the 200 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,320 | 269.4 µs | 278.7 µs | 292.5 µs | 487.9 µs | 624.8 µs | 317.2 µs (sd 8.7) | 3,149 stmt/s = 629,760 rows/s | - | 1462 ns | - |
| `SET v = v + 1` | 1,320 | 290.8 µs | 301.6 µs | 316.1 µs | 494.8 µs | 566.4 µs | 338.7 µs (sd 8.8) | 2,965 stmt/s = 592,960 rows/s | +22.60 µs [+21.30 µs, +26.40 µs] | 1580 ns | +113 ns [+107, +132] |
| `SET v = w, w = v` | 1,320 | 282.8 µs | 291.8 µs | 304.7 µs | 373.0 µs | 432.6 µs | 310.7 µs (sd 4.7) | 3,218 stmt/s = 643,700 rows/s | +11.30 µs [+9.90 µs, +14.60 µs] | 1523 ns | +57 ns [+50, +73] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,320 | 324.0 µs | 335.4 µs | 351.7 µs | 454.5 µs | 796.6 µs | 367.3 µs (sd 11.0) | 2,738 stmt/s = 547,660 rows/s | +59.40 µs [+56.20 µs, +61.00 µs] | 1758 ns | +297 ns [+281, +305] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,320 | 310.6 µs | 322.3 µs | 338.7 µs | 492.6 µs | 555.2 µs | 354.6 µs (sd 3.9) | 2,820 stmt/s = 564,080 rows/s | +45.80 µs [+43.50 µs, +50.20 µs] | 1694 ns | +229 ns [+218, +251] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 33,000 | 48.3 µs | 55.1 µs | 55.8 µs | 73.6 µs | 82.0 µs | 83.9 µs (sd 31.7) | 16,728 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 33,000 | 118.4 µs | 123.9 µs | 126.3 µs | 154.0 µs | 174.3 µs | 140.1 µs (sd 14.8) | 7,528 /s | +70.50 µs [+70.30 µs, +71.10 µs] |
|   of which the `SELECT v` | 33,000 | 59.5 µs | 64.8 µs | 65.8 µs | 85.4 µs | 95.2 µs | 75.0 µs (sd 13.4) | - | - |
|   of which the `UPDATE SET v = <lit>` | 33,000 | 53.0 µs | 58.0 µs | 58.9 µs | 77.0 µs | 86.0 µs | 64.3 µs (sd 8.2) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 33,000 | 173.7 µs | 184.4 µs | 190.0 µs | 221.7 µs | 251.3 µs | 237.9 µs (sd 32.8) | 4,072 /s | +134.20 µs [+133.90 µs, +135.00 µs] |


#### cores = 2, 1,000 rows

10 runs used of 16 (6 discarded: other-CPU busy > 10% or verify/bad flag); loadavg(1 min) before each run 2.18-3.62, after 2.08-3.44; other-CPU busy over the run 1.9%-5.7% (median 4.1%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 30,000 | 46.4 µs | 51.6 µs | 52.6 µs | 70.6 µs | 82.5 µs | 55.4 µs (sd 0.6) | 17,724 stmt/s |
| UPDATE SET v = lit, by pk | B | 30,000 | 46.9 µs | 53.8 µs | 54.5 µs | 71.9 µs | 82.1 µs | 57.3 µs (sd 0.8) | 17,125 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 30,000 | 43.8 µs | 51.5 µs | 52.3 µs | 70.0 µs | 82.5 µs | 56.1 µs (sd 1.4) | 17,651 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 30,000 | 48.6 µs | 53.7 µs | 54.4 µs | 71.7 µs | 84.5 µs | 58.7 µs (sd 2.7) | 16,974 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 30,000 | 45.4 µs | 52.3 µs | 53.2 µs | 70.2 µs | 80.8 µs | 57.3 µs (sd 2.2) | 17,161 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 30,000 | 48.9 µs | 54.6 µs | 55.2 µs | 72.4 µs | 82.2 µs | 59.1 µs (sd 1.6) | 16,738 stmt/s |
| UPDATE SET v = lit, every row | A | 1,200 | 1,208 µs | 1,289 µs | 1,313 µs | 1,432 µs | 1,746 µs | 1,331 µs (sd 22.1) | 753 stmt/s |
| UPDATE SET v = lit, every row | B | 1,200 | 1,234 µs | 1,305 µs | 1,333 µs | 1,441 µs | 1,503 µs | 1,342 µs (sd 8.9) | 745 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,200 | 1,263 µs | 1,305 µs | 1,330 µs | 1,530 µs | 2,137 µs | 1,572 µs (sd 616.5) | 727 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,200 | 1,290 µs | 1,320 µs | 1,343 µs | 2,274 µs | 2,509 µs | 1,447 µs (sd 35.8) | 695 stmt/s |
| INSERT, pk named | A | 30,000 | 39.6 µs | 48.8 µs | 49.5 µs | 67.7 µs | 86.6 µs | 100.1 µs (sd 44.3) | 9,463 stmt/s |
| INSERT, pk named | B | 30,000 | 39.6 µs | 49.1 µs | 50.2 µs | 68.3 µs | 89.4 µs | 127.9 µs (sd 46.8) | 8,354 stmt/s |
| SELECT by pk (control) | A | 30,000 | 57.4 µs | 65.6 µs | 68.8 µs | 90.6 µs | 104.7 µs | 72.3 µs (sd 1.6) | 13,749 stmt/s |
| SELECT by pk (control) | B | 30,000 | 59.9 µs | 65.9 µs | 68.9 µs | 89.8 µs | 105.1 µs | 72.4 µs (sd 1.2) | 13,712 stmt/s |
| DELETE by pk | A | 30,000 | 43.2 µs | 48.5 µs | 49.2 µs | 65.6 µs | 73.9 µs | 51.4 µs (sd 0.3) | 19,207 stmt/s |
| DELETE by pk | B | 30,000 | 44.8 µs | 48.6 µs | 49.4 µs | 64.6 µs | 72.2 µs | 52.2 µs (sd 0.7) | 18,910 stmt/s |
| SHOW META (control) | A | 30,000 | 45.0 µs | 50.0 µs | 50.6 µs | 64.4 µs | 72.8 µs | 52.7 µs (sd 0.2) | 18,818 stmt/s |
| SHOW META (control) | B | 30,000 | 44.7 µs | 49.8 µs | 50.5 µs | 64.3 µs | 72.0 µs | 52.5 µs (sd 0.5) | 18,964 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.80 µs [+1.40 µs, +2.50 µs] | +3.40 % [+2.64, +5.09] | +1.95 µs [+1.48 µs, +2.41 µs] | -3.64 % [-4.43, -2.08] | **B slower** (+3.4 % at p50, floor 1.0 %) |
| UPDATE SET v = lit, by pk (replicate) | +2.25 µs [+1.45 µs, +2.70 µs] | +4.31 % [+3.04, +5.22] | +2.58 µs [+0.82 µs, +4.40 µs] | -3.25 % [-7.47, -0.72] | **B slower** (+4.3 % at p50, floor 1.0 %) |
| UPDATE SET v = lit, w = lit, by pk | +2.20 µs [+1.50 µs, +2.65 µs] | +4.12 % [+2.82, +5.04] | +1.79 µs [+0.12 µs, +3.39 µs] | -2.11 % [-6.46, -1.13] | **B slower** (+4.1 % at p50, floor 1.0 %) |
| UPDATE SET v = lit, every row | +23.70 µs [+1.45 µs, +38.05 µs] | +1.82 % [+0.02, +2.93] | +10.58 µs [-5.33 µs, +24.66 µs] | -1.24 % [-2.08, +1.64] | CI excludes 0, inside the floor (2.4 %): not resolved |
| UPDATE SET v = lit, every row (replicate) | +19.85 µs [-7.35 µs, +27.60 µs] | +1.49 % [-0.22, +2.15] | -124.94 µs [-511.20 µs, +78.10 µs] | -4.09 % [-5.37, -2.84] | no change resolved |
| INSERT, pk named | +0.40 µs [+0.00 µs, +1.05 µs] | +0.81 % [+0.00, +1.83] | +27.81 µs [-21.94 µs, +78.32 µs] | -18.83 % [-70.33, +38.68] | no change resolved |
| SELECT by pk (control) | +0.35 µs [-0.20 µs, +0.70 µs] | +0.51 % [-0.29, +1.03] | +0.12 µs [-0.22 µs, +0.42 µs] | -0.20 % [-0.83, +0.27] | no change resolved |
| DELETE by pk | +0.35 µs [-0.30 µs, +0.60 µs] | +0.72 % [-0.61, +1.22] | +0.72 µs [+0.36 µs, +1.07 µs] | -1.43 % [-2.41, -0.13] | no change resolved |
| SHOW META (control) | -0.20 µs [-0.40 µs, +0.40 µs] | -0.39 % [-0.79, +0.79] | -0.17 µs [-0.42 µs, +0.07 µs] | +0.28 % [-0.47, +0.96] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ -0.39 % [-0.79, +0.79]; replicate (second relation, same binary, per run, both arms pooled) point median -0.39 %, robust sd 0.30 % (plain sd 0.61 %); range median +1.01 %, robust sd 0.70 % (plain sd 0.57 %). Floor used: point 0.99 %, range 2.41 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 30,000 | 49.0 µs | 53.7 µs | 54.4 µs | 70.7 µs | 79.2 µs | 80.4 µs (sd 36.9) | 16,922 stmt/s | - | - | - |
| `SET v = v + 1` | 30,000 | 48.7 µs | 55.4 µs | 56.1 µs | 73.0 µs | 80.3 µs | 59.1 µs (sd 2.0) | 16,714 stmt/s | +1.70 µs [+1.50 µs, +1.75 µs] | - | - |
| `SET v = w, w = v` | 30,000 | 48.5 µs | 54.9 µs | 55.6 µs | 72.8 µs | 79.9 µs | 68.1 µs (sd 29.7) | 16,742 stmt/s | +1.30 µs [+1.05 µs, +1.40 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 30,000 | 53.9 µs | 61.3 µs | 62.1 µs | 81.4 µs | 89.6 µs | 81.6 µs (sd 33.6) | 14,892 stmt/s | +7.80 µs [+7.60 µs, +7.90 µs] | - | - |

range (`WHERE id >= 1`, every one of the 1,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,200 | 1,133 µs | 1,178 µs | 1,203 µs | 1,951 µs | 2,439 µs | 1,780 µs (sd 1518.4) | 776 stmt/s = 776,300 rows/s | - | 1203 ns | - |
| `SET v = v + 1` | 1,200 | 1,241 µs | 1,285 µs | 1,312 µs | 1,481 µs | 4,020 µs | 2,811 µs (sd 1313.8) | 378 stmt/s = 377,500 rows/s | +108.50 µs [+103.15 µs, +112.10 µs] | 1312 ns | +109 ns [+103, +112] |
| `SET v = w, w = v` | 1,200 | 1,192 µs | 1,244 µs | 1,269 µs | 1,477 µs | 7,516 µs | 2,445 µs (sd 1683.9) | 675 stmt/s = 674,900 rows/s | +64.25 µs [+58.85 µs, +75.20 µs] | 1269 ns | +64 ns [+59, +75] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,200 | 1,380 µs | 1,434 µs | 1,467 µs | 2,209 µs | 2,622 µs | 2,049 µs (sd 1582.9) | 646 stmt/s = 645,650 rows/s | +267.85 µs [+254.40 µs, +273.80 µs] | 1467 ns | +268 ns [+254, +274] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,200 | 1,270 µs | 1,317 µs | 1,342 µs | 1,518 µs | 2,164 µs | 1,575 µs (sd 615.9) | 724 stmt/s = 723,550 rows/s | +138.20 µs [+133.10 µs, +145.30 µs] | 1342 ns | +138 ns [+133, +145] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 30,000 | 46.8 µs | 55.2 µs | 55.9 µs | 73.9 µs | 83.1 µs | 58.9 µs (sd 0.8) | 16,732 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 30,000 | 116.1 µs | 124.2 µs | 127.5 µs | 156.9 µs | 177.1 µs | 132.9 µs (sd 0.9) | 7,468 /s | +71.50 µs [+71.25 µs, +72.10 µs] |
|   of which the `SELECT v` | 30,000 | 60.2 µs | 65.0 µs | 66.5 µs | 86.1 µs | 97.6 µs | 69.9 µs (sd 0.5) | - | - |
|   of which the `UPDATE SET v = <lit>` | 30,000 | 50.9 µs | 58.1 µs | 59.1 µs | 77.4 µs | 87.2 µs | 62.1 µs (sd 0.4) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 30,000 | 173.4 µs | 185.6 µs | 193.1 µs | 225.5 µs | 251.3 µs | 259.8 µs (sd 10.0) | 3,865 /s | +137.05 µs [+136.60 µs, +137.90 µs] |


#### cores = 2, 10,000 rows

9 runs used of 16 (7 discarded: other-CPU busy > 10% or verify/bad flag); loadavg(1 min) before each run 2.31-3.31, after 2.39-3.31; other-CPU busy over the run 2.4%-5.0% (median 2.8%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 27,000 | 47.1 µs | 52.0 µs | 53.2 µs | 71.4 µs | 87.0 µs | 56.4 µs (sd 1.1) | 17,368 stmt/s |
| UPDATE SET v = lit, by pk | B | 27,000 | 47.9 µs | 54.2 µs | 54.9 µs | 73.6 µs | 88.5 µs | 58.3 µs (sd 1.2) | 16,966 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 27,000 | 47.0 µs | 51.9 µs | 53.0 µs | 70.1 µs | 82.7 µs | 55.8 µs (sd 0.8) | 17,622 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 27,000 | 49.9 µs | 54.1 µs | 54.8 µs | 72.3 µs | 82.9 µs | 57.9 µs (sd 0.4) | 16,973 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 27,000 | 47.3 µs | 52.8 µs | 53.7 µs | 69.9 µs | 79.6 µs | 56.7 µs (sd 0.9) | 17,224 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 27,000 | 49.4 µs | 54.9 µs | 55.7 µs | 73.3 µs | 80.9 µs | 58.5 µs (sd 0.7) | 16,793 stmt/s |
| UPDATE SET v = lit, every row | A | 1,080 | 11.85 ms | 12.80 ms | 13.20 ms | 24.91 ms | 388.96 ms | 25.80 ms (sd 2176.2) | 38 stmt/s |
| UPDATE SET v = lit, every row | B | 1,080 | 12.61 ms | 13.12 ms | 13.54 ms | 20.14 ms | 298.08 ms | 23.87 ms (sd 2401.9) | 40 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,080 | 12.37 ms | 12.88 ms | 13.21 ms | 72.47 ms | 384.32 ms | 28.85 ms (sd 3338.2) | 36 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,080 | 12.63 ms | 13.18 ms | 13.57 ms | 47.68 ms | 270.62 ms | 23.40 ms (sd 2398.5) | 44 stmt/s |
| INSERT, pk named | A | 27,000 | 39.6 µs | 48.9 µs | 50.0 µs | 68.7 µs | 97.6 µs | 71.3 µs (sd 28.4) | 17,830 stmt/s |
| INSERT, pk named | B | 27,000 | 42.0 µs | 49.5 µs | 50.8 µs | 69.8 µs | 96.3 µs | 83.0 µs (sd 45.3) | 17,806 stmt/s |
| SELECT by pk (control) | A | 27,000 | 61.8 µs | 65.4 µs | 66.4 µs | 84.5 µs | 97.0 µs | 69.4 µs (sd 1.0) | 14,172 stmt/s |
| SELECT by pk (control) | B | 27,000 | 59.0 µs | 65.7 µs | 66.6 µs | 84.3 µs | 93.8 µs | 69.2 µs (sd 0.8) | 14,252 stmt/s |
| DELETE by pk | A | 27,000 | 44.8 µs | 48.5 µs | 49.4 µs | 65.6 µs | 74.2 µs | 52.3 µs (sd 1.0) | 18,865 stmt/s |
| DELETE by pk | B | 27,000 | 43.6 µs | 48.8 µs | 49.7 µs | 66.7 µs | 75.6 µs | 52.0 µs (sd 0.6) | 19,009 stmt/s |
| SHOW META (control) | A | 27,000 | 42.6 µs | 49.8 µs | 50.4 µs | 64.1 µs | 72.2 µs | 52.3 µs (sd 0.5) | 18,922 stmt/s |
| SHOW META (control) | B | 27,000 | 43.5 µs | 49.8 µs | 50.4 µs | 63.6 µs | 70.8 µs | 52.3 µs (sd 0.6) | 18,956 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.60 µs [+1.20 µs, +2.50 µs] | +3.02 % [+2.24, +4.74] | +1.78 µs [+0.97 µs, +2.54 µs] | -3.35 % [-5.20, -1.09] | **B slower** (+3.0 % at p50, floor 1.6 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.80 µs [+1.30 µs, +3.10 µs] | +3.36 % [+2.43, +5.93] | +2.10 µs [+1.33 µs, +2.80 µs] | -4.24 % [-5.57, -2.43] | **B slower** (+3.4 % at p50, floor 1.6 %) |
| UPDATE SET v = lit, w = lit, by pk | +1.90 µs [+1.40 µs, +2.70 µs] | +3.57 % [+2.62, +5.09] | +1.80 µs [+1.10 µs, +2.54 µs] | -3.08 % [-5.96, -1.14] | **B slower** (+3.6 % at p50, floor 1.6 %) |
| UPDATE SET v = lit, every row | +279.00 µs [+150.40 µs, +656.60 µs] | +2.09 % [+1.13, +5.08] | -1,925 µs [-3,742 µs, +475.11 µs] | +13.78 % [-0.50, +18.43] | CI excludes 0, inside the floor (2.3 %): not resolved |
| UPDATE SET v = lit, every row (replicate) | +307.20 µs [+42.00 µs, +487.00 µs] | +2.29 % [+0.31, +3.73] | -5,450 µs [-8,385 µs, -2,590 µs] | +24.54 % [+2.79, +46.94] | **B slower** (+2.3 % at p50, floor 2.3 %) |
| INSERT, pk named | +1.00 µs [+0.30 µs, +1.30 µs] | +2.02 % [+0.60, +2.63] | +11.74 µs [-24.81 µs, +51.33 µs] | -1.06 % [-57.51, +69.30] | **B slower** (+2.0 % at p50, floor 1.6 %) |
| SELECT by pk (control) | +0.10 µs [-0.40 µs, +1.10 µs] | +0.15 % [-0.60, +1.67] | -0.17 µs [-0.63 µs, +0.33 µs] | +0.22 % [-0.44, +1.67] | no change resolved |
| DELETE by pk | +0.40 µs [-0.30 µs, +0.70 µs] | +0.81 % [-0.61, +1.43] | -0.29 µs [-0.83 µs, +0.28 µs] | +0.64 % [-1.41, +1.97] | no change resolved |
| SHOW META (control) | +0.30 µs [-0.80 µs, +0.40 µs] | +0.60 % [-1.58, +0.80] | -0.03 µs [-0.40 µs, +0.29 µs] | -0.09 % [-0.86, +1.70] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ +0.60 % [-1.58, +0.80]; replicate (second relation, same binary, per run, both arms pooled) point median -0.36 %, robust sd 0.54 % (plain sd 0.41 %); range median +0.76 %, robust sd 0.75 % (plain sd 1.17 %). Floor used: point 1.58 %, range 2.26 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 27,000 | 48.7 µs | 54.3 µs | 55.1 µs | 73.4 µs | 85.1 µs | 58.6 µs (sd 1.1) | 16,764 stmt/s | - | - | - |
| `SET v = v + 1` | 27,000 | 52.2 µs | 56.0 µs | 56.8 µs | 74.5 µs | 85.9 µs | 60.1 µs (sd 1.3) | 16,305 stmt/s | +1.70 µs [+1.60 µs, +1.80 µs] | - | - |
| `SET v = w, w = v` | 27,000 | 51.1 µs | 55.5 µs | 56.3 µs | 74.2 µs | 85.1 µs | 61.0 µs (sd 1.7) | 15,922 stmt/s | +1.20 µs [+1.10 µs, +1.40 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 27,000 | 55.7 µs | 61.7 µs | 62.7 µs | 82.4 µs | 94.8 µs | 69.5 µs (sd 1.8) | 14,069 stmt/s | +7.50 µs [+7.40 µs, +7.80 µs] | - | - |

range (`WHERE id >= 1`, every one of the 10,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,080 | 11.08 ms | 11.35 ms | 11.48 ms | 17.46 ms | 330.84 ms | 19.88 ms (sd 367.8) | 50 stmt/s = 505,000 rows/s | - | 1148 ns | - |
| `SET v = v + 1` | 1,080 | 12.07 ms | 12.53 ms | 12.64 ms | 17.70 ms | 336.89 ms | 23.17 ms (sd 767.2) | 43 stmt/s = 427,000 rows/s | +1,170 µs [+1,150 µs, +1,246 µs] | 1264 ns | +117 ns [+115, +125] |
| `SET v = w, w = v` | 1,080 | 11.66 ms | 12.04 ms | 12.17 ms | 15.51 ms | 295.83 ms | 15.84 ms (sd 1445.9) | 66 stmt/s = 659,000 rows/s | +716.30 µs [+668.10 µs, +742.20 µs] | 1217 ns | +72 ns [+67, +74] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,080 | 13.45 ms | 13.89 ms | 14.00 ms | 57.52 ms | 338.94 ms | 28.19 ms (sd 1981.0) | 36 stmt/s = 362,000 rows/s | +2,549 µs [+2,509 µs, +2,585 µs] | 1400 ns | +255 ns [+251, +259] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,080 | 12.19 ms | 12.62 ms | 12.71 ms | 18.38 ms | 323.93 ms | 20.26 ms (sd 1284.1) | 48 stmt/s = 483,000 rows/s | +1,240 µs [+1,208 µs, +1,277 µs] | 1271 ns | +124 ns [+121, +128] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 27,000 | 46.0 µs | 55.7 µs | 56.5 µs | 74.4 µs | 83.8 µs | 91.4 µs (sd 37.9) | 16,225 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 27,000 | 116.8 µs | 124.0 µs | 126.6 µs | 156.7 µs | 185.3 µs | 141.2 µs (sd 24.3) | 7,473 /s | +70.40 µs [+69.60 µs, +70.50 µs] |
|   of which the `SELECT v` | 27,000 | 57.1 µs | 64.9 µs | 66.1 µs | 85.6 µs | 97.5 µs | 69.6 µs (sd 0.4) | - | - |
|   of which the `UPDATE SET v = <lit>` | 27,000 | 51.0 µs | 57.9 µs | 58.8 µs | 78.0 µs | 91.5 µs | 70.7 µs (sd 23.9) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 27,000 | 176.9 µs | 184.2 µs | 189.3 µs | 225.0 µs | 264.3 µs | 225.7 µs (sd 34.1) | 4,998 /s | +132.80 µs [+131.80 µs, +133.90 µs] |

