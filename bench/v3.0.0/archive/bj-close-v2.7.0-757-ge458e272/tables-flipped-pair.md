
#### cores = 1, 1,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 0.42-2.28, after 0.66-2.47; other-CPU busy over the run 2.0%-3.1% (median 2.5%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 2 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 40.3 µs | 51.3 µs | 52.0 µs | 67.2 µs | 81.4 µs | 54.3 µs (sd 0.5) | 18,086 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 43.9 µs | 52.4 µs | 53.3 µs | 67.8 µs | 81.6 µs | 55.6 µs (sd 0.6) | 17,700 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 43.3 µs | 51.3 µs | 52.0 µs | 67.1 µs | 81.3 µs | 54.8 µs (sd 1.1) | 17,897 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 43.6 µs | 52.4 µs | 53.2 µs | 68.2 µs | 83.3 µs | 56.5 µs (sd 1.1) | 17,301 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 43.9 µs | 52.1 µs | 52.9 µs | 67.9 µs | 82.7 µs | 55.7 µs (sd 1.0) | 17,648 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 45.1 µs | 53.7 µs | 54.3 µs | 69.1 µs | 83.0 µs | 57.5 µs (sd 1.0) | 17,116 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 917.4 µs | 954.4 µs | 973.7 µs | 1,137 µs | 1,427 µs | 1,006 µs (sd 18.0) | 994 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 915.0 µs | 948.4 µs | 969.2 µs | 1,131 µs | 1,460 µs | 998.3 µs (sd 22.1) | 1,009 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 911.3 µs | 951.3 µs | 965.1 µs | 1,127 µs | 1,693 µs | 1,238 µs (sd 640.9) | 1,001 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 911.7 µs | 947.3 µs | 976.1 µs | 2,318 µs | 3,667 µs | 1,581 µs (sd 747.1) | 857 stmt/s |
| INSERT, pk named | A | 48,000 | 38.1 µs | 48.7 µs | 49.5 µs | 63.4 µs | 76.4 µs | 73.0 µs (sd 50.7) | 18,907 stmt/s |
| INSERT, pk named | B | 48,000 | 36.1 µs | 48.8 µs | 49.5 µs | 64.0 µs | 77.2 µs | 162.6 µs (sd 55.8) | 5,873 stmt/s |
| SELECT by pk (control) | A | 48,000 | 58.2 µs | 64.7 µs | 67.4 µs | 88.3 µs | 107.1 µs | 71.1 µs (sd 1.6) | 13,953 stmt/s |
| SELECT by pk (control) | B | 48,000 | 58.8 µs | 64.9 µs | 67.7 µs | 88.2 µs | 103.9 µs | 71.0 µs (sd 1.0) | 13,938 stmt/s |
| DELETE by pk | A | 48,000 | 40.6 µs | 48.3 µs | 49.1 µs | 62.5 µs | 72.4 µs | 51.4 µs (sd 0.9) | 19,275 stmt/s |
| DELETE by pk | B | 48,000 | 40.3 µs | 48.3 µs | 49.2 µs | 62.4 µs | 72.5 µs | 51.4 µs (sd 1.1) | 19,223 stmt/s |
| SHOW META (control) | A | 48,000 | 39.3 µs | 49.7 µs | 50.4 µs | 62.7 µs | 72.1 µs | 52.5 µs (sd 0.4) | 18,908 stmt/s |
| SHOW META (control) | B | 48,000 | 44.2 µs | 49.6 µs | 50.4 µs | 62.6 µs | 71.2 µs | 52.2 µs (sd 0.4) | 19,002 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.10 µs [+0.70 µs, +1.60 µs] | +2.12 % [+1.34, +3.11] | +1.30 µs [+0.93 µs, +1.67 µs] | -2.03 % [-3.34, -1.46] | **B slower** (+2.1 % at p50, floor 0.6 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.30 µs [+0.80 µs, +1.60 µs] | +2.50 % [+1.53, +3.08] | +1.72 µs [+0.97 µs, +2.37 µs] | -3.65 % [-4.40, -1.95] | **B slower** (+2.5 % at p50, floor 0.6 %) |
| UPDATE SET v = lit, w = lit, by pk | +1.30 µs [+1.10 µs, +1.80 µs] | +2.46 % [+2.07, +3.42] | +1.81 µs [+1.01 µs, +2.59 µs] | -3.15 % [-5.20, -1.53] | **B slower** (+2.5 % at p50, floor 0.6 %) |
| UPDATE SET v = lit, every row | -4.65 µs [-11.60 µs, +0.00 µs] | -0.48 % [-1.20, +0.02] | -7.49 µs [-18.49 µs, +2.83 µs] | +0.35 % [-0.37, +2.11] | no change resolved |
| UPDATE SET v = lit, every row (replicate) | +4.40 µs [-0.20 µs, +9.70 µs] | +0.46 % [-0.10, +0.90] | +342.63 µs [-187.84 µs, +883.75 µs] | -14.78 % [-39.80, -12.97] | no change resolved |
| INSERT, pk named | +0.00 µs [-0.15 µs, +0.15 µs] | +0.00 % [-0.20, +0.30] | +89.59 µs [+36.95 µs, +132.48 µs] | -69.48 % [-73.94, -58.76] | no change resolved |
| SELECT by pk (control) | +0.20 µs [+0.00 µs, +0.65 µs] | +0.30 % [-0.15, +0.89] | -0.16 µs [-0.83 µs, +0.31 µs] | -0.16 % [-0.51, +0.49] | no change resolved |
| DELETE by pk | +0.05 µs [-0.20 µs, +0.30 µs] | +0.10 % [-0.40, +0.61] | -0.01 µs [-0.54 µs, +0.55 µs] | +0.31 % [-1.91, +1.37] | no change resolved |
| SHOW META (control) | +0.00 µs [-0.30 µs, +0.20 µs] | +0.00 % [-0.59, +0.40] | -0.22 µs [-0.45 µs, -0.01 µs] | +0.41 % [-0.35, +1.11] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ +0.00 % [-0.59, +0.40]; replicate (second relation, same binary, per run, both arms pooled) point median +0.00 %, robust sd 0.28 % (plain sd 0.26 %); range median -0.36 %, robust sd 0.92 % (plain sd 1.40 %). Floor used: point 0.59 %, range 2.20 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 43.8 µs | 52.7 µs | 53.5 µs | 68.2 µs | 80.0 µs | 60.1 µs (sd 8.2) | 17,414 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 46.1 µs | 54.8 µs | 55.4 µs | 70.5 µs | 83.3 µs | 58.3 µs (sd 0.8) | 16,848 stmt/s | +1.90 µs [+1.70 µs, +2.10 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 46.4 µs | 54.4 µs | 55.0 µs | 70.7 µs | 85.7 µs | 60.6 µs (sd 7.2) | 16,752 stmt/s | +1.50 µs [+1.40 µs, +1.65 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 51.0 µs | 60.8 µs | 61.5 µs | 79.2 µs | 95.1 µs | 67.4 µs (sd 6.9) | 15,005 stmt/s | +8.00 µs [+7.80 µs, +8.15 µs] | - | - |

range (`WHERE id >= 1`, every one of the 1,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 900.4 µs | 939.4 µs | 958.1 µs | 1,611 µs | 3,184 µs | 1,496 µs (sd 1157.9) | 931 stmt/s = 931,050 rows/s | - | 958 ns | - |
| `SET v = v + 1` | 1,920 | 1,021 µs | 1,049 µs | 1,067 µs | 1,326 µs | 8,558 µs | 3,234 µs (sd 1696.0) | 262 stmt/s = 261,600 rows/s | +109.20 µs [+104.50 µs, +115.80 µs] | 1067 ns | +109 ns [+104, +116] |
| `SET v = w, w = v` | 1,920 | 980.4 µs | 1,007 µs | 1,022 µs | 1,215 µs | 2,713 µs | 1,221 µs (sd 267.8) | 896 stmt/s = 895,650 rows/s | +65.90 µs [+60.50 µs, +69.90 µs] | 1022 ns | +66 ns [+60, +70] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 1,173 µs | 1,209 µs | 1,229 µs | 2,131 µs | 3,859 µs | 1,612 µs (sd 921.8) | 735 stmt/s = 734,850 rows/s | +270.20 µs [+266.20 µs, +279.30 µs] | 1229 ns | +270 ns [+266, +279] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 1,045 µs | 1,079 µs | 1,096 µs | 1,312 µs | 2,957 µs | 1,856 µs (sd 1483.9) | 852 stmt/s = 852,500 rows/s | +138.15 µs [+136.10 µs, +141.45 µs] | 1096 ns | +138 ns [+136, +141] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 45.4 µs | 54.6 µs | 55.2 µs | 69.9 µs | 81.4 µs | 57.8 µs (sd 1.0) | 17,083 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 114.9 µs | 122.8 µs | 124.9 µs | 150.2 µs | 170.4 µs | 129.6 µs (sd 1.3) | 7,699 /s | +69.65 µs [+69.40 µs, +70.20 µs] |
|   of which the `SELECT v` | 48,000 | 55.3 µs | 64.3 µs | 65.4 µs | 81.8 µs | 94.9 µs | 68.3 µs (sd 0.7) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 52.0 µs | 57.3 µs | 58.1 µs | 73.0 µs | 83.7 µs | 60.4 µs (sd 0.6) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 173.5 µs | 183.8 µs | 190.5 µs | 221.9 µs | 255.4 µs | 268.4 µs (sd 10.6) | 3,689 /s | +135.15 µs [+134.60 µs, +135.80 µs] |


##### SUMMARY point: extra cost per statement over `SET v = lit` (B)

| | cores 1, 1,000 rows |
|---|---|
| `SET v = v + 1` | +1.90 µs [+1.70, +2.10] |
| `SET v = w, w = v` | +1.50 µs [+1.40, +1.65] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | +8.00 µs [+7.80, +8.15] |

##### SUMMARY range: extra cost per row over `SET v = lit` (B), ns per row

| | cores 1, 1,000 rows |
|---|---|
| `SET v = v + 1` | +109 ns [+104, +116] |
| `SET v = w, w = v` | +66 ns [+60, +70] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | +270 ns [+266, +279] |
| `SET v = (SELECT COUNT(*) FROM t)` | +138 ns [+136, +141] |

##### SUMMARY range: extra cost per statement over `SET v = lit` (B)

| | cores 1, 1,000 rows |
|---|---|
| `SET v = v + 1` | +109.20 µs [+104.50 µs, +115.80 µs] |
| `SET v = w, w = v` | +65.90 µs [+60.50 µs, +69.90 µs] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | +270.20 µs [+266.20 µs, +279.30 µs] |
| `SET v = (SELECT COUNT(*) FROM t)` | +138.15 µs [+136.10 µs, +141.45 µs] |

##### SUMMARY range: p50 per row, ns (B)

| | cores 1, 1,000 rows |
|---|---|
| `SET v = lit` | 958 ns |
| `SET v = v + 1` | 1067 ns |
| `SET v = w, w = v` | 1022 ns |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1229 ns |
| `SET v = (SELECT COUNT(*) FROM t)` | 1096 ns |

##### SUMMARY point: p50 (B)

| | cores 1, 1,000 rows |
|---|---|
| `SET v = lit` | 53.5 µs |
| `SET v = v + 1` | 55.4 µs |
| `SET v = w, w = v` | 55.0 µs |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 61.5 µs |

##### SUMMARY round trip: p50, derived statements/s = 1e6 / p50

| | cores 1, 1,000 rows |
|---|---|
| rt-one | 55.2 µs = 18,116 /s |
| rt-pair | 124.9 µs = 8,006 /s |
| rt-txn | 190.5 µs = 5,249 /s |
| rt-pair minus rt-one | +69.7 µs [+69.4, +70.2] |
| rt-txn minus rt-one | +135.2 µs [+134.6, +135.8] |
