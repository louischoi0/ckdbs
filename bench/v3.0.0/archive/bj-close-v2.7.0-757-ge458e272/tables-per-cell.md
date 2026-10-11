
#### cores = 1, 1,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 1.98-10.32, after 1.98-8.88; other-CPU busy over the run 2.1%-7.8% (median 2.4%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 39.9 µs | 50.8 µs | 51.5 µs | 66.9 µs | 83.9 µs | 54.1 µs (sd 1.5) | 18,264 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 42.6 µs | 51.2 µs | 52.0 µs | 66.9 µs | 81.1 µs | 54.3 µs (sd 0.7) | 18,090 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 39.9 µs | 50.9 µs | 51.6 µs | 67.3 µs | 81.9 µs | 55.1 µs (sd 1.6) | 17,873 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 43.2 µs | 51.2 µs | 52.1 µs | 67.4 µs | 81.4 µs | 55.4 µs (sd 1.2) | 17,700 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 42.9 µs | 51.5 µs | 52.3 µs | 67.9 µs | 83.7 µs | 55.5 µs (sd 1.1) | 17,546 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 43.8 µs | 52.2 µs | 53.1 µs | 68.2 µs | 82.9 µs | 56.5 µs (sd 1.1) | 17,410 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 905.2 µs | 942.5 µs | 966.3 µs | 1,110 µs | 1,302 µs | 991.4 µs (sd 18.6) | 1,011 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 921.5 µs | 966.2 µs | 983.2 µs | 1,139 µs | 1,412 µs | 1,014 µs (sd 16.7) | 988 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 900.1 µs | 935.0 µs | 951.1 µs | 1,081 µs | 1,642 µs | 985.1 µs (sd 35.6) | 1,019 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 910.7 µs | 958.4 µs | 980.5 µs | 2,363 µs | 3,514 µs | 1,255 µs (sd 286.0) | 848 stmt/s |
| INSERT, pk named | A | 48,000 | 35.0 µs | 48.5 µs | 49.2 µs | 64.2 µs | 78.9 µs | 104.5 µs (sd 71.5) | 18,103 stmt/s |
| INSERT, pk named | B | 48,000 | 40.0 µs | 48.6 µs | 49.4 µs | 63.8 µs | 77.7 µs | 146.4 µs (sd 60.8) | 6,078 stmt/s |
| SELECT by pk (control) | A | 48,000 | 59.3 µs | 65.1 µs | 67.6 µs | 88.4 µs | 109.0 µs | 71.4 µs (sd 1.1) | 13,869 stmt/s |
| SELECT by pk (control) | B | 48,000 | 58.0 µs | 64.7 µs | 67.3 µs | 87.7 µs | 106.3 µs | 70.7 µs (sd 1.2) | 14,032 stmt/s |
| DELETE by pk | A | 48,000 | 39.3 µs | 48.2 µs | 48.9 µs | 62.8 µs | 73.4 µs | 51.1 µs (sd 0.7) | 19,381 stmt/s |
| DELETE by pk | B | 48,000 | 39.8 µs | 48.2 µs | 48.9 µs | 62.9 µs | 74.4 µs | 51.6 µs (sd 1.2) | 19,261 stmt/s |
| SHOW META (control) | A | 48,000 | 42.4 µs | 49.7 µs | 50.3 µs | 62.1 µs | 70.6 µs | 52.1 µs (sd 0.3) | 19,018 stmt/s |
| SHOW META (control) | B | 48,000 | 43.2 µs | 49.6 µs | 50.2 µs | 62.1 µs | 70.5 µs | 52.1 µs (sd 0.3) | 19,032 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +0.25 µs [-0.10 µs, +0.80 µs] | +0.48 % [-0.19, +1.56] | +0.16 µs [-0.76 µs, +0.86 µs] | -0.28 % [-1.85, +0.57] | no change resolved |
| UPDATE SET v = lit, by pk (replicate) | +0.40 µs [+0.00 µs, +0.70 µs] | +0.78 % [+0.00, +1.36] | +0.34 µs [-0.53 µs, +1.24 µs] | -0.08 % [-2.39, +0.88] | no change resolved |
| UPDATE SET v = lit, w = lit, by pk | +0.65 µs [+0.45 µs, +1.05 µs] | +1.26 % [+0.95, +2.02] | +1.04 µs [+0.14 µs, +2.04 µs] | -0.76 % [-3.45, +0.09] | **B slower** (+1.3 % at p50, floor 0.6 %) |
| UPDATE SET v = lit, every row | +16.90 µs [+14.30 µs, +28.75 µs] | +1.77 % [+1.51, +3.02] | +23.08 µs [+15.76 µs, +30.67 µs] | -1.94 % [-3.37, -1.40] | CI excludes 0, inside the floor (3.0 %): not resolved |
| UPDATE SET v = lit, every row (replicate) | +29.45 µs [+24.10 µs, +36.60 µs] | +3.13 % [+2.56, +3.85] | +269.69 µs [+189.64 µs, +415.80 µs] | -17.22 % [-17.97, -16.13] | **B slower** (+3.1 % at p50, floor 3.0 %) |
| INSERT, pk named | +0.20 µs [+0.10 µs, +0.30 µs] | +0.40 % [+0.20, +0.61] | +41.86 µs [-20.74 µs, +100.44 µs] | -67.11 % [-72.75, +127.81] | CI excludes 0, inside the floor (0.6 %): not resolved |
| SELECT by pk (control) | -0.40 µs [-0.90 µs, +0.10 µs] | -0.60 % [-1.33, +0.15] | -0.72 µs [-1.44 µs, +0.01 µs] | +0.94 % [-0.08, +2.05] | no change resolved |
| DELETE by pk | +0.05 µs [-0.20 µs, +0.20 µs] | +0.10 % [-0.41, +0.41] | +0.49 µs [-0.11 µs, +1.12 µs] | -0.69 % [-2.16, +0.92] | no change resolved |
| SHOW META (control) | -0.05 µs [-0.20 µs, +0.10 µs] | -0.10 % [-0.40, +0.20] | +0.04 µs [-0.18 µs, +0.28 µs] | +0.13 % [-0.59, +0.71] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ -0.10 % [-0.40, +0.20]; replicate (second relation, same binary, per run, both arms pooled) point median +0.00 %, robust sd 0.29 % (plain sd 0.34 %); range median -0.74 %, robust sd 1.13 % (plain sd 1.30 %). Floor used: point 0.58 %, range 3.00 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 40.8 µs | 51.5 µs | 52.3 µs | 66.9 µs | 77.5 µs | 56.0 µs (sd 1.7) | 17,502 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 45.8 µs | 54.6 µs | 55.3 µs | 70.2 µs | 82.5 µs | 57.8 µs (sd 0.8) | 16,934 stmt/s | +2.90 µs [+2.75 µs, +3.10 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 43.5 µs | 54.1 µs | 54.7 µs | 68.8 µs | 78.6 µs | 61.4 µs (sd 8.8) | 16,855 stmt/s | +2.40 µs [+2.25 µs, +2.60 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 50.8 µs | 60.6 µs | 61.4 µs | 77.3 µs | 89.0 µs | 64.5 µs (sd 1.0) | 15,161 stmt/s | +9.15 µs [+8.70 µs, +9.35 µs] | - | - |

range (`WHERE id >= 1`, every one of the 1,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 909.2 µs | 947.5 µs | 966.5 µs | 1,797 µs | 3,252 µs | 1,503 µs (sd 1146.4) | 921 stmt/s = 921,050 rows/s | - | 966 ns | - |
| `SET v = v + 1` | 1,920 | 1,039 µs | 1,070 µs | 1,093 µs | 1,469 µs | 79.46 ms | 3,792 µs (sd 1359.9) | 250 stmt/s = 250,400 rows/s | +123.65 µs [+118.70 µs, +131.90 µs] | 1093 ns | +124 ns [+119, +132] |
| `SET v = w, w = v` | 1,920 | 1,003 µs | 1,036 µs | 1,054 µs | 1,286 µs | 6,718 µs | 1,333 µs (sd 311.5) | 839 stmt/s = 838,700 rows/s | +89.70 µs [+84.35 µs, +92.80 µs] | 1054 ns | +90 ns [+84, +93] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 1,212 µs | 1,241 µs | 1,266 µs | 1,872 µs | 4,219 µs | 1,601 µs (sd 869.0) | 721 stmt/s = 720,900 rows/s | +295.00 µs [+290.90 µs, +305.30 µs] | 1266 ns | +295 ns [+291, +305] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 1,062 µs | 1,092 µs | 1,113 µs | 1,475 µs | 3,889 µs | 1,312 µs (sd 285.3) | 830 stmt/s = 830,250 rows/s | +145.40 µs [+143.90 µs, +149.30 µs] | 1113 ns | +145 ns [+144, +149] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 45.6 µs | 54.4 µs | 55.1 µs | 69.9 µs | 80.2 µs | 61.6 µs (sd 17.5) | 17,209 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 114.3 µs | 121.2 µs | 123.6 µs | 149.0 µs | 169.2 µs | 128.3 µs (sd 1.2) | 7,743 /s | +68.50 µs [+68.20 µs, +68.70 µs] |
|   of which the `SELECT v` | 48,000 | 54.8 µs | 63.7 µs | 64.9 µs | 81.9 µs | 95.0 µs | 67.8 µs (sd 0.6) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 50.6 µs | 56.3 µs | 57.2 µs | 72.2 µs | 83.4 µs | 59.6 µs (sd 0.7) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 171.6 µs | 181.9 µs | 188.5 µs | 220.5 µs | 250.8 µs | 261.8 µs (sd 21.7) | 3,741 /s | +133.75 µs [+132.85 µs, +134.15 µs] |


#### cores = 1, 10,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 1.54-6.14, after 2.29-4.95; other-CPU busy over the run 2.2%-7.5% (median 3.7%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 40.2 µs | 51.0 µs | 51.8 µs | 67.3 µs | 83.2 µs | 56.2 µs (sd 7.2) | 18,042 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 41.4 µs | 51.7 µs | 52.7 µs | 67.9 µs | 82.8 µs | 65.5 µs (sd 29.6) | 17,674 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 43.6 µs | 51.1 µs | 51.9 µs | 67.1 µs | 81.2 µs | 56.5 µs (sd 8.5) | 18,005 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 41.9 µs | 51.7 µs | 52.7 µs | 67.8 µs | 82.4 µs | 55.3 µs (sd 1.1) | 17,776 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 43.0 µs | 51.7 µs | 52.6 µs | 68.9 µs | 85.2 µs | 56.4 µs (sd 2.3) | 17,612 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 43.1 µs | 52.8 µs | 53.8 µs | 69.8 µs | 85.5 µs | 57.2 µs (sd 2.0) | 17,412 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 9,292 µs | 9,581 µs | 9,719 µs | 18.66 ms | 761.51 ms | 24.43 ms (sd 7190.0) | 49 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 9,512 µs | 9,823 µs | 10.05 ms | 16.97 ms | 389.78 ms | 19.07 ms (sd 3606.4) | 57 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 9,341 µs | 9,562 µs | 9,700 µs | 19.48 ms | 838.75 ms | 41.34 ms (sd 11128.5) | 27 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 9,440 µs | 9,820 µs | 10.03 ms | 16.36 ms | 57.41 ms | 14.16 ms (sd 4823.8) | 87 stmt/s |
| INSERT, pk named | A | 48,000 | 41.6 µs | 48.7 µs | 49.5 µs | 66.4 µs | 83.3 µs | 56.1 µs (sd 9.0) | 18,440 stmt/s |
| INSERT, pk named | B | 48,000 | 39.6 µs | 48.9 µs | 49.7 µs | 68.0 µs | 85.5 µs | 104.0 µs (sd 136.4) | 18,711 stmt/s |
| SELECT by pk (control) | A | 48,000 | 55.1 µs | 64.3 µs | 65.2 µs | 80.7 µs | 95.2 µs | 68.2 µs (sd 1.7) | 14,505 stmt/s |
| SELECT by pk (control) | B | 48,000 | 58.8 µs | 64.0 µs | 64.9 µs | 80.1 µs | 92.7 µs | 67.7 µs (sd 1.2) | 14,642 stmt/s |
| DELETE by pk | A | 48,000 | 36.8 µs | 48.4 µs | 49.2 µs | 64.5 µs | 76.3 µs | 57.2 µs (sd 15.9) | 19,024 stmt/s |
| DELETE by pk | B | 48,000 | 38.5 µs | 48.4 µs | 49.3 µs | 64.2 µs | 76.7 µs | 52.4 µs (sd 3.3) | 19,227 stmt/s |
| SHOW META (control) | A | 48,000 | 43.9 µs | 49.8 µs | 50.4 µs | 62.8 µs | 73.0 µs | 52.6 µs (sd 1.0) | 18,857 stmt/s |
| SHOW META (control) | B | 48,000 | 42.4 µs | 49.8 µs | 50.4 µs | 62.7 µs | 71.3 µs | 52.3 µs (sd 0.7) | 19,028 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +0.65 µs [+0.30 µs, +1.15 µs] | +1.26 % [+0.58, +2.23] | +9.31 µs [-2.66 µs, +26.49 µs] | -1.39 % [-4.25, -0.70] | **B slower** (+1.3 % at p50, floor 0.8 %) |
| UPDATE SET v = lit, by pk (replicate) | +0.65 µs [+0.30 µs, +0.90 µs] | +1.26 % [+0.58, +1.72] | -1.21 µs [-5.71 µs, +1.35 µs] | -1.63 % [-2.37, -0.07] | **B slower** (+1.3 % at p50, floor 0.8 %) |
| UPDATE SET v = lit, w = lit, by pk | +0.95 µs [+0.70 µs, +1.35 µs] | +1.82 % [+1.34, +2.59] | +0.79 µs [-0.24 µs, +1.81 µs] | -1.31 % [-2.95, -0.30] | **B slower** (+1.8 % at p50, floor 0.8 %) |
| UPDATE SET v = lit, every row | +192.05 µs [+126.40 µs, +480.10 µs] | +1.97 % [+1.30, +4.98] | -5,365 µs [-8,561 µs, -2,710 µs] | +17.21 % [+13.80, +29.67] | **B slower** (+2.0 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, every row (replicate) | +216.20 µs [+149.80 µs, +482.00 µs] | +2.23 % [+1.54, +4.99] | -27.18 ms [-33.14 ms, -22.84 ms] | +219.76 % [+131.13, +235.39] | **B slower** (+2.2 % at p50, floor 1.1 %) |
| INSERT, pk named | +0.10 µs [+0.00 µs, +0.20 µs] | +0.20 % [+0.00, +0.40] | +47.89 µs [-4.61 µs, +121.11 µs] | +1.47 % [-3.76, +3.23] | no change resolved |
| SELECT by pk (control) | -0.15 µs [-0.90 µs, +0.15 µs] | -0.23 % [-1.37, +0.31] | -0.47 µs [-1.27 µs, +0.19 µs] | +0.65 % [-0.12, +1.07] | no change resolved |
| DELETE by pk | +0.10 µs [-0.00 µs, +0.20 µs] | +0.20 % [+0.00, +0.41] | -4.81 µs [-12.02 µs, -0.26 µs] | +1.00 % [-1.13, +3.79] | no change resolved |
| SHOW META (control) | -0.05 µs [-0.30 µs, +0.20 µs] | -0.10 % [-0.60, +0.49] | -0.33 µs [-0.75 µs, +0.08 µs] | +0.31 % [-0.36, +1.64] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ -0.10 % [-0.60, +0.49]; replicate (second relation, same binary, per run, both arms pooled) point median +0.19 %, robust sd 0.29 % (plain sd 0.48 %); range median +0.12 %, robust sd 0.50 % (plain sd 0.61 %). Floor used: point 0.77 %, range 1.12 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 43.3 µs | 52.1 µs | 53.2 µs | 68.9 µs | 82.4 µs | 58.3 µs (sd 5.3) | 17,283 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 46.7 µs | 55.2 µs | 55.9 µs | 71.6 µs | 85.0 µs | 67.1 µs (sd 14.4) | 16,557 stmt/s | +2.70 µs [+2.55 µs, +2.85 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 46.4 µs | 54.7 µs | 55.4 µs | 70.4 µs | 83.3 µs | 70.0 µs (sd 36.0) | 16,317 stmt/s | +2.20 µs [+2.10 µs, +2.45 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 51.2 µs | 61.2 µs | 62.1 µs | 79.5 µs | 94.8 µs | 90.6 µs (sd 48.9) | 14,194 stmt/s | +8.95 µs [+8.70 µs, +9.10 µs] | - | - |

range (`WHERE id >= 1`, every one of the 10,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 8,723 µs | 9,109 µs | 9,547 µs | 16.94 ms | 409.98 ms | 24.78 ms (sd 8559.2) | 51 stmt/s = 505,500 rows/s | - | 955 ns | - |
| `SET v = v + 1` | 1,920 | 10.01 ms | 10.42 ms | 10.76 ms | 20.73 ms | 416.16 ms | 28.62 ms (sd 9683.8) | 42 stmt/s = 416,500 rows/s | +1,263 µs [+1,214 µs, +1,320 µs] | 1076 ns | +126 ns [+121, +132] |
| `SET v = w, w = v` | 1,920 | 9,623 µs | 10.05 ms | 10.41 ms | 17.36 ms | 78.77 ms | 15.49 ms (sd 2781.3) | 69 stmt/s = 692,000 rows/s | +908.25 µs [+826.50 µs, +948.75 µs] | 1041 ns | +91 ns [+83, +95] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 11.52 ms | 11.95 ms | 12.34 ms | 20.55 ms | 448.61 ms | 35.58 ms (sd 11422.9) | 36 stmt/s = 361,500 rows/s | +2,839 µs [+2,800 µs, +2,904 µs] | 1234 ns | +284 ns [+280, +290] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 9,987 µs | 10.47 ms | 10.92 ms | 19.78 ms | 401.93 ms | 24.45 ms (sd 6040.2) | 48 stmt/s = 480,500 rows/s | +1,380 µs [+1,329 µs, +1,423 µs] | 1092 ns | +138 ns [+133, +142] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 45.9 µs | 54.9 µs | 55.7 µs | 77.4 µs | 173.4 µs | 79.9 µs (sd 42.5) | 15,467 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 110.1 µs | 121.4 µs | 123.5 µs | 154.6 µs | 184.5 µs | 132.3 µs (sd 8.4) | 7,647 /s | +67.75 µs [+67.45 µs, +68.30 µs] |
|   of which the `SELECT v` | 48,000 | 55.3 µs | 63.6 µs | 64.6 µs | 84.2 µs | 99.4 µs | 70.5 µs (sd 8.5) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 49.1 µs | 56.6 µs | 57.4 µs | 75.1 µs | 89.6 µs | 60.8 µs (sd 1.1) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 171.0 µs | 181.9 µs | 186.3 µs | 226.9 µs | 259.6 µs | 235.5 µs (sd 61.0) | 4,834 /s | +130.40 µs [+129.70 µs, +131.20 µs] |


#### cores = 2, 1,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 1.32-2.59, after 1.58-2.66; other-CPU busy over the run 2.2%-12.6% (median 5.3%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 46.6 µs | 52.0 µs | 52.9 µs | 70.4 µs | 82.9 µs | 55.5 µs (sd 0.7) | 17,662 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 47.3 µs | 52.8 µs | 53.7 µs | 70.6 µs | 81.2 µs | 56.1 µs (sd 0.9) | 17,508 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 44.4 µs | 51.9 µs | 52.8 µs | 69.4 µs | 80.7 µs | 57.8 µs (sd 7.3) | 17,609 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 43.6 µs | 52.8 µs | 53.6 µs | 69.9 µs | 81.6 µs | 57.0 µs (sd 1.3) | 17,278 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 45.8 µs | 52.7 µs | 53.5 µs | 69.7 µs | 79.0 µs | 56.6 µs (sd 0.8) | 17,256 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 47.3 µs | 53.8 µs | 54.5 µs | 70.8 µs | 80.5 µs | 57.5 µs (sd 0.9) | 17,093 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 1,197 µs | 1,278 µs | 1,301 µs | 1,411 µs | 1,774 µs | 1,316 µs (sd 12.7) | 762 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 1,205 µs | 1,287 µs | 1,314 µs | 1,437 µs | 1,683 µs | 1,334 µs (sd 20.8) | 753 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 1,261 µs | 1,292 µs | 1,309 µs | 1,493 µs | 2,066 µs | 1,482 µs (sd 525.0) | 742 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 1,266 µs | 1,311 µs | 1,340 µs | 2,318 µs | 3,462 µs | 1,682 µs (sd 634.8) | 690 stmt/s |
| INSERT, pk named | A | 48,000 | 39.6 µs | 49.0 µs | 49.8 µs | 68.3 µs | 89.0 µs | 103.2 µs (sd 44.3) | 9,526 stmt/s |
| INSERT, pk named | B | 48,000 | 40.0 µs | 49.2 µs | 50.1 µs | 68.3 µs | 86.9 µs | 134.4 µs (sd 49.3) | 8,035 stmt/s |
| SELECT by pk (control) | A | 48,000 | 60.7 µs | 65.5 µs | 68.4 µs | 88.6 µs | 102.1 µs | 71.8 µs (sd 0.8) | 13,814 stmt/s |
| SELECT by pk (control) | B | 48,000 | 58.4 µs | 65.6 µs | 68.4 µs | 89.4 µs | 103.1 µs | 72.4 µs (sd 2.2) | 13,748 stmt/s |
| DELETE by pk | A | 48,000 | 44.1 µs | 48.7 µs | 49.6 µs | 66.6 µs | 75.6 µs | 52.2 µs (sd 1.1) | 19,059 stmt/s |
| DELETE by pk | B | 48,000 | 42.8 µs | 48.6 µs | 49.3 µs | 65.1 µs | 73.8 µs | 51.9 µs (sd 0.8) | 19,174 stmt/s |
| SHOW META (control) | A | 48,000 | 42.1 µs | 49.9 µs | 50.6 µs | 64.7 µs | 73.3 µs | 52.8 µs (sd 0.6) | 18,862 stmt/s |
| SHOW META (control) | B | 48,000 | 43.6 µs | 49.9 µs | 50.6 µs | 65.8 µs | 74.5 µs | 52.9 µs (sd 0.7) | 18,811 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +0.70 µs [+0.00 µs, +1.50 µs] | +1.33 % [+0.00, +2.68] | +0.64 µs [+0.10 µs, +1.19 µs] | -1.42 % [-2.46, +0.40] | no change resolved |
| UPDATE SET v = lit, by pk (replicate) | +0.75 µs [+0.20 µs, +1.60 µs] | +1.42 % [+0.38, +3.03] | -0.86 µs [-4.96 µs, +1.64 µs] | -1.72 % [-3.21, +0.49] | **B slower** (+1.4 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, w = lit, by pk | +0.90 µs [+0.30 µs, +1.50 µs] | +1.68 % [+0.56, +2.84] | +0.86 µs [+0.26 µs, +1.50 µs] | -1.14 % [-2.91, +0.38] | **B slower** (+1.7 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, every row | +5.65 µs [-2.20 µs, +17.00 µs] | +0.44 % [-0.19, +1.56] | +17.76 µs [+7.56 µs, +28.26 µs] | -1.28 % [-2.69, -0.12] | no change resolved |
| UPDATE SET v = lit, every row (replicate) | +27.20 µs [+15.95 µs, +35.00 µs] | +2.08 % [+1.24, +2.69] | +199.06 µs [-195.04 µs, +618.43 µs] | -7.92 % [-10.02, -6.85] | CI excludes 0, inside the floor (3.0 %): not resolved |
| INSERT, pk named | +0.35 µs [+0.00 µs, +0.70 µs] | +0.70 % [+0.00, +1.41] | +31.20 µs [-10.87 µs, +71.53 µs] | -31.38 % [-54.45, +6.68] | no change resolved |
| SELECT by pk (control) | -0.05 µs [-0.50 µs, +0.40 µs] | -0.07 % [-0.73, +0.59] | +0.59 µs [-0.10 µs, +1.73 µs] | -0.41 % [-0.75, +0.25] | no change resolved |
| DELETE by pk | -0.20 µs [-0.40 µs, +0.10 µs] | -0.40 % [-0.81, +0.20] | -0.30 µs [-0.99 µs, +0.37 µs] | +0.76 % [-1.12, +1.76] | no change resolved |
| SHOW META (control) | +0.05 µs [-0.10 µs, +0.30 µs] | +0.10 % [-0.20, +0.60] | +0.11 µs [-0.39 µs, +0.61 µs] | -0.12 % [-1.26, +0.60] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ +0.10 % [-0.20, +0.60]; replicate (second relation, same binary, per run, both arms pooled) point median -0.28 %, robust sd 0.41 % (plain sd 0.38 %); range median +1.11 %, robust sd 0.95 % (plain sd 0.97 %). Floor used: point 1.11 %, range 3.01 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 45.5 µs | 52.9 µs | 53.7 µs | 70.8 µs | 80.5 µs | 65.0 µs (sd 22.9) | 17,237 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 48.4 µs | 55.5 µs | 56.2 µs | 73.7 µs | 82.6 µs | 64.0 µs (sd 17.9) | 16,371 stmt/s | +2.45 µs [+2.40 µs, +2.60 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 47.1 µs | 55.0 µs | 55.7 µs | 73.5 µs | 83.1 µs | 60.1 µs (sd 1.5) | 16,186 stmt/s | +2.00 µs [+1.90 µs, +2.15 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 56.3 µs | 61.4 µs | 62.3 µs | 82.3 µs | 92.0 µs | 71.4 µs (sd 19.2) | 14,878 stmt/s | +8.65 µs [+8.30 µs, +8.80 µs] | - | - |

range (`WHERE id >= 1`, every one of the 1,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 1,114 µs | 1,158 µs | 1,185 µs | 1,878 µs | 3,104 µs | 1,752 µs (sd 1218.0) | 772 stmt/s = 771,550 rows/s | - | 1185 ns | - |
| `SET v = v + 1` | 1,920 | 1,231 µs | 1,278 µs | 1,308 µs | 1,518 µs | 22.78 ms | 3,428 µs (sd 1360.7) | 270 stmt/s = 270,200 rows/s | +120.35 µs [+118.30 µs, +126.00 µs] | 1308 ns | +120 ns [+118, +126] |
| `SET v = w, w = v` | 1,920 | 1,196 µs | 1,242 µs | 1,272 µs | 1,489 µs | 6,863 µs | 1,920 µs (sd 1463.0) | 711 stmt/s = 710,800 rows/s | +83.90 µs [+76.00 µs, +90.70 µs] | 1272 ns | +84 ns [+76, +91] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 1,389 µs | 1,445 µs | 1,474 µs | 2,182 µs | 3,268 µs | 1,708 µs (sd 474.5) | 637 stmt/s = 636,750 rows/s | +291.55 µs [+285.90 µs, +301.00 µs] | 1474 ns | +292 ns [+286, +301] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 1,252 µs | 1,303 µs | 1,332 µs | 1,555 µs | 2,578 µs | 1,709 µs (sd 659.9) | 705 stmt/s = 704,800 rows/s | +146.45 µs [+142.35 µs, +153.50 µs] | 1332 ns | +146 ns [+142, +154] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 49.5 µs | 55.2 µs | 55.9 µs | 74.0 µs | 83.3 µs | 106.8 µs (sd 50.8) | 11,769 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 113.2 µs | 123.0 µs | 125.9 µs | 153.4 µs | 169.6 µs | 131.0 µs (sd 1.7) | 7,608 /s | +70.10 µs [+69.15 µs, +70.90 µs] |
|   of which the `SELECT v` | 48,000 | 57.8 µs | 64.6 µs | 66.0 µs | 85.2 µs | 95.9 µs | 69.1 µs (sd 0.8) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 50.3 µs | 57.2 µs | 58.1 µs | 75.6 µs | 85.2 µs | 60.9 µs (sd 0.9) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 170.9 µs | 184.3 µs | 191.5 µs | 223.4 µs | 248.7 µs | 231.3 µs (sd 30.2) | 4,231 /s | +135.50 µs [+134.90 µs, +136.25 µs] |


##### SUMMARY point: extra cost per statement over `SET v = lit` (B)

| | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 1,000 rows |
|---|---|---|---|
| `SET v = v + 1` | +2.90 µs [+2.75, +3.10] | +2.70 µs [+2.55, +2.85] | +2.45 µs [+2.40, +2.60] |
| `SET v = w, w = v` | +2.40 µs [+2.25, +2.60] | +2.20 µs [+2.10, +2.45] | +2.00 µs [+1.90, +2.15] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | +9.15 µs [+8.70, +9.35] | +8.95 µs [+8.70, +9.10] | +8.65 µs [+8.30, +8.80] |

##### SUMMARY range: extra cost per row over `SET v = lit` (B), ns per row

| | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 1,000 rows |
|---|---|---|---|
| `SET v = v + 1` | +124 ns [+119, +132] | +126 ns [+121, +132] | +120 ns [+118, +126] |
| `SET v = w, w = v` | +90 ns [+84, +93] | +91 ns [+83, +95] | +84 ns [+76, +91] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | +295 ns [+291, +305] | +284 ns [+280, +290] | +292 ns [+286, +301] |
| `SET v = (SELECT COUNT(*) FROM t)` | +145 ns [+144, +149] | +138 ns [+133, +142] | +146 ns [+142, +154] |

##### SUMMARY range: extra cost per statement over `SET v = lit` (B)

| | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 1,000 rows |
|---|---|---|---|
| `SET v = v + 1` | +123.65 µs [+118.70 µs, +131.90 µs] | +1,263 µs [+1,214 µs, +1,320 µs] | +120.35 µs [+118.30 µs, +126.00 µs] |
| `SET v = w, w = v` | +89.70 µs [+84.35 µs, +92.80 µs] | +908.25 µs [+826.50 µs, +948.75 µs] | +83.90 µs [+76.00 µs, +90.70 µs] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | +295.00 µs [+290.90 µs, +305.30 µs] | +2,839 µs [+2,800 µs, +2,904 µs] | +291.55 µs [+285.90 µs, +301.00 µs] |
| `SET v = (SELECT COUNT(*) FROM t)` | +145.40 µs [+143.90 µs, +149.30 µs] | +1,380 µs [+1,329 µs, +1,423 µs] | +146.45 µs [+142.35 µs, +153.50 µs] |

##### SUMMARY range: p50 per row, ns (B)

| | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 1,000 rows |
|---|---|---|---|
| `SET v = lit` | 966 ns | 955 ns | 1185 ns |
| `SET v = v + 1` | 1093 ns | 1076 ns | 1308 ns |
| `SET v = w, w = v` | 1054 ns | 1041 ns | 1272 ns |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1266 ns | 1234 ns | 1474 ns |
| `SET v = (SELECT COUNT(*) FROM t)` | 1113 ns | 1092 ns | 1332 ns |

##### SUMMARY point: p50 (B)

| | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 1,000 rows |
|---|---|---|---|
| `SET v = lit` | 52.3 µs | 53.2 µs | 53.7 µs |
| `SET v = v + 1` | 55.3 µs | 55.9 µs | 56.2 µs |
| `SET v = w, w = v` | 54.7 µs | 55.4 µs | 55.7 µs |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 61.4 µs | 62.1 µs | 62.3 µs |

##### SUMMARY round trip: p50, derived statements/s = 1e6 / p50

| | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 1,000 rows |
|---|---|---|---|
| rt-one | 55.1 µs = 18,149 /s | 55.7 µs = 17,953 /s | 55.9 µs = 17,889 /s |
| rt-pair | 123.6 µs = 8,091 /s | 123.5 µs = 8,097 /s | 125.9 µs = 7,943 /s |
| rt-txn | 188.5 µs = 5,305 /s | 186.3 µs = 5,368 /s | 191.5 µs = 5,222 /s |
| rt-pair minus rt-one | +68.5 µs [+68.2, +68.7] | +67.8 µs [+67.5, +68.3] | +70.1 µs [+69.2, +70.9] |
| rt-txn minus rt-one | +133.8 µs [+132.8, +134.2] | +130.4 µs [+129.7, +131.2] | +135.5 µs [+134.9, +136.2] |
