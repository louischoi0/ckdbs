
#### cores = 1, 1,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 10% or verify/bad flag); loadavg(1 min) before each run 1.98-10.32, after 1.98-8.88; other-CPU busy over the run 2.1%-7.8% (median 2.4%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

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

16 runs used of 16 (0 discarded: other-CPU busy > 10% or verify/bad flag); loadavg(1 min) before each run 1.54-6.14, after 2.29-4.95; other-CPU busy over the run 2.2%-7.5% (median 3.7%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

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

14 runs used of 16 (2 discarded: other-CPU busy > 10% or verify/bad flag); loadavg(1 min) before each run 1.32-2.59, after 1.58-2.66; other-CPU busy over the run 2.2%-10.0% (median 5.1%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 42,000 | 46.6 µs | 51.9 µs | 52.8 µs | 70.3 µs | 81.2 µs | 55.4 µs (sd 0.7) | 17,692 stmt/s |
| UPDATE SET v = lit, by pk | B | 42,000 | 47.3 µs | 52.8 µs | 53.7 µs | 70.7 µs | 81.3 µs | 56.2 µs (sd 0.9) | 17,508 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 42,000 | 44.4 µs | 51.8 µs | 52.7 µs | 69.5 µs | 80.7 µs | 58.0 µs (sd 7.9) | 17,625 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 42,000 | 43.6 µs | 52.8 µs | 53.6 µs | 70.0 µs | 81.0 µs | 56.9 µs (sd 1.3) | 17,278 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 42,000 | 45.8 µs | 52.6 µs | 53.5 µs | 69.6 µs | 78.7 µs | 56.6 µs (sd 0.9) | 17,275 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 42,000 | 48.8 µs | 53.8 µs | 54.5 µs | 71.0 µs | 80.5 µs | 57.5 µs (sd 0.9) | 17,093 stmt/s |
| UPDATE SET v = lit, every row | A | 1,680 | 1,197 µs | 1,277 µs | 1,300 µs | 1,410 µs | 1,790 µs | 1,316 µs (sd 13.6) | 762 stmt/s |
| UPDATE SET v = lit, every row | B | 1,680 | 1,205 µs | 1,288 µs | 1,317 µs | 1,435 µs | 1,621 µs | 1,334 µs (sd 21.0) | 753 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,680 | 1,261 µs | 1,292 µs | 1,308 µs | 1,493 µs | 2,100 µs | 1,501 µs (sd 561.1) | 742 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,680 | 1,266 µs | 1,314 µs | 1,344 µs | 2,310 µs | 3,305 µs | 1,711 µs (sd 676.2) | 690 stmt/s |
| INSERT, pk named | A | 42,000 | 39.6 µs | 49.0 µs | 49.8 µs | 68.3 µs | 89.0 µs | 106.4 µs (sd 45.6) | 9,305 stmt/s |
| INSERT, pk named | B | 42,000 | 40.0 µs | 49.2 µs | 50.1 µs | 68.3 µs | 87.5 µs | 128.2 µs (sd 48.8) | 8,186 stmt/s |
| SELECT by pk (control) | A | 42,000 | 60.7 µs | 65.5 µs | 68.4 µs | 88.7 µs | 102.1 µs | 71.7 µs (sd 0.7) | 13,837 stmt/s |
| SELECT by pk (control) | B | 42,000 | 58.4 µs | 65.6 µs | 68.3 µs | 89.4 µs | 103.1 µs | 72.4 µs (sd 2.4) | 13,748 stmt/s |
| DELETE by pk | A | 42,000 | 44.1 µs | 48.7 µs | 49.6 µs | 66.7 µs | 76.0 µs | 52.2 µs (sd 1.2) | 19,059 stmt/s |
| DELETE by pk | B | 42,000 | 42.8 µs | 48.6 µs | 49.3 µs | 65.1 µs | 73.6 µs | 51.9 µs (sd 0.7) | 19,174 stmt/s |
| SHOW META (control) | A | 42,000 | 42.1 µs | 49.9 µs | 50.6 µs | 64.5 µs | 73.0 µs | 52.8 µs (sd 0.6) | 18,862 stmt/s |
| SHOW META (control) | B | 42,000 | 43.6 µs | 49.9 µs | 50.6 µs | 66.0 µs | 75.0 µs | 52.9 µs (sd 0.8) | 18,766 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +0.95 µs [+0.25 µs, +1.70 µs] | +1.80 % [+0.47, +3.08] | +0.81 µs [+0.26 µs, +1.35 µs] | -1.89 % [-2.55, +0.10] | **B slower** (+1.8 % at p50, floor 0.8 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.05 µs [+0.20 µs, +1.65 µs] | +2.00 % [+0.38, +3.28] | -1.08 µs [-5.54 µs, +1.74 µs] | -1.72 % [-4.30, +1.83] | **B slower** (+2.0 % at p50, floor 0.8 %) |
| UPDATE SET v = lit, w = lit, by pk | +1.15 µs [+0.70 µs, +1.60 µs] | +2.15 % [+1.30, +2.99] | +0.91 µs [+0.24 µs, +1.63 µs] | -1.14 % [-3.35, +0.50] | **B slower** (+2.1 % at p50, floor 0.8 %) |
| UPDATE SET v = lit, every row | +11.25 µs [-1.00 µs, +30.60 µs] | +0.87 % [-0.08, +2.34] | +17.86 µs [+6.86 µs, +29.14 µs] | -1.28 % [-2.45, -0.42] | no change resolved |
| UPDATE SET v = lit, every row (replicate) | +28.00 µs [+20.60 µs, +41.90 µs] | +2.15 % [+1.59, +3.22] | +209.63 µs [-254.96 µs, +653.65 µs] | -7.92 % [-9.93, -6.55] | CI excludes 0, inside the floor (3.0 %): not resolved |
| INSERT, pk named | +0.35 µs [+0.00 µs, +0.75 µs] | +0.70 % [+0.00, +1.51] | +21.87 µs [-24.01 µs, +63.70 µs] | -19.25 % [-52.19, +30.78] | no change resolved |
| SELECT by pk (control) | -0.05 µs [-0.40 µs, +0.40 µs] | -0.07 % [-0.59, +0.58] | +0.78 µs [+0.03 µs, +1.97 µs] | -0.52 % [-0.93, -0.11] | no change resolved |
| DELETE by pk | -0.10 µs [-0.40 µs, +0.10 µs] | -0.20 % [-0.80, +0.20] | -0.34 µs [-1.11 µs, +0.40 µs] | +0.76 % [-2.28, +2.06] | no change resolved |
| SHOW META (control) | +0.00 µs [-0.10 µs, +0.40 µs] | +0.00 % [-0.20, +0.79] | +0.14 µs [-0.44 µs, +0.71 µs] | -0.29 % [-1.47, +0.60] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ +0.00 % [-0.20, +0.79]; replicate (second relation, same binary, per run, both arms pooled) point median -0.19 %, robust sd 0.28 % (plain sd 0.37 %); range median +1.08 %, robust sd 0.96 % (plain sd 1.03 %). Floor used: point 0.79 %, range 3.01 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 42,000 | 45.5 µs | 52.9 µs | 53.7 µs | 70.8 µs | 80.4 µs | 66.3 µs (sd 24.4) | 17,237 stmt/s | - | - | - |
| `SET v = v + 1` | 42,000 | 48.4 µs | 55.4 µs | 56.1 µs | 73.7 µs | 82.5 µs | 64.6 µs (sd 19.1) | 16,446 stmt/s | +2.45 µs [+2.40 µs, +2.55 µs] | - | - |
| `SET v = w, w = v` | 42,000 | 47.1 µs | 55.0 µs | 55.7 µs | 73.7 µs | 83.4 µs | 60.2 µs (sd 1.6) | 16,156 stmt/s | +2.05 µs [+1.90 µs, +2.20 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 42,000 | 58.9 µs | 61.4 µs | 62.2 µs | 82.1 µs | 91.9 µs | 72.2 µs (sd 20.4) | 14,878 stmt/s | +8.50 µs [+8.30 µs, +8.80 µs] | - | - |

range (`WHERE id >= 1`, every one of the 1,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,680 | 1,114 µs | 1,159 µs | 1,186 µs | 1,866 µs | 2,858 µs | 1,800 µs (sd 1300.7) | 777 stmt/s = 777,250 rows/s | - | 1186 ns | - |
| `SET v = v + 1` | 1,680 | 1,231 µs | 1,279 µs | 1,308 µs | 1,483 µs | 23.63 ms | 3,422 µs (sd 1436.8) | 270 stmt/s = 270,200 rows/s | +121.50 µs [+118.30 µs, +126.50 µs] | 1308 ns | +122 ns [+118, +126] |
| `SET v = w, w = v` | 1,680 | 1,196 µs | 1,242 µs | 1,274 µs | 1,448 µs | 6,489 µs | 1,974 µs (sd 1563.2) | 718 stmt/s = 718,000 rows/s | +83.90 µs [+76.00 µs, +101.70 µs] | 1274 ns | +84 ns [+76, +102] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,680 | 1,389 µs | 1,443 µs | 1,474 µs | 2,169 µs | 3,138 µs | 1,699 µs (sd 506.1) | 641 stmt/s = 641,100 rows/s | +290.55 µs [+284.40 µs, +297.60 µs] | 1474 ns | +291 ns [+284, +298] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,680 | 1,252 µs | 1,303 µs | 1,332 µs | 1,521 µs | 2,091 µs | 1,603 µs (sd 543.2) | 723 stmt/s = 722,550 rows/s | +144.90 µs [+140.80 µs, +153.50 µs] | 1332 ns | +145 ns [+141, +154] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 42,000 | 49.5 µs | 55.2 µs | 55.9 µs | 74.1 µs | 83.0 µs | 102.2 µs (sd 52.9) | 16,434 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 42,000 | 113.2 µs | 122.8 µs | 125.7 µs | 153.2 µs | 169.9 µs | 130.9 µs (sd 1.8) | 7,622 /s | +69.85 µs [+69.00 µs, +70.80 µs] |
|   of which the `SELECT v` | 42,000 | 57.8 µs | 64.5 µs | 65.9 µs | 85.0 µs | 95.8 µs | 69.0 µs (sd 0.9) | - | - |
|   of which the `UPDATE SET v = <lit>` | 42,000 | 50.3 µs | 57.1 µs | 58.1 µs | 75.5 µs | 84.9 µs | 60.9 µs (sd 1.0) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 42,000 | 170.9 µs | 184.2 µs | 191.3 µs | 222.9 µs | 248.2 µs | 234.2 µs (sd 31.0) | 4,023 /s | +135.45 µs [+134.80 µs, +136.10 µs] |

