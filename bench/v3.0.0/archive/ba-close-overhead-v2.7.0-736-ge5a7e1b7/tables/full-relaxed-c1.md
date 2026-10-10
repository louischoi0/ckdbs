
**cores = 1, 200 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.00-1.85; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 40.7 µs | 48.3 µs | 49.2 µs | 50.1 µs | 62.5 µs | 78.5 µs | 50.8 µs (sd 0.3) | 19,422 qps |
| INSERT, pk omitted | B | 40.2 µs | 48.3 µs | 49.2 µs | 50.1 µs | 62.5 µs | 76.7 µs | 50.7 µs (sd 0.3) | 19,461 qps |
| INSERT again (replicate) | A | 39.9 µs | 48.2 µs | 49.1 µs | 50.0 µs | 62.4 µs | 77.1 µs | 53.4 µs (sd 1.1) | 18,551 qps |
| INSERT again (replicate) | B | 40.3 µs | 48.2 µs | 49.1 µs | 50.1 µs | 62.1 µs | 75.5 µs | 52.2 µs (sd 1.6) | 19,035 qps |
| SELECT by pk | A | 58.6 µs | 63.9 µs | 64.8 µs | 68.0 µs | 80.4 µs | 95.6 µs | 67.6 µs (sd 0.6) | 14,668 qps |
| SELECT by pk | B | 59.1 µs | 64.6 µs | 65.6 µs | 69.1 µs | 81.3 µs | 94.4 µs | 68.3 µs (sd 0.7) | 14,462 qps |
| UPDATE by pk | A | 39.5 µs | 49.7 µs | 51.0 µs | 52.4 µs | 65.1 µs | 79.1 µs | 55.8 µs (sd 1.7) | 17,924 qps |
| UPDATE by pk | B | 40.7 µs | 50.9 µs | 51.5 µs | 53.1 µs | 65.4 µs | 78.5 µs | 54.5 µs (sd 1.1) | 18,209 qps |
| DELETE by pk | A | 39.9 µs | 48.1 µs | 48.8 µs | 49.9 µs | 60.8 µs | 70.7 µs | 53.4 µs (sd 2.3) | 18,926 qps |
| DELETE by pk | B | 39.8 µs | 48.3 µs | 49.0 µs | 50.0 µs | 61.0 µs | 70.0 µs | 51.2 µs (sd 0.7) | 19,360 qps |
| bulk INSERT, 20 rows ascending | A | 88.7 µs | 92.2 µs | 94.0 µs | 104.7 µs | 119.8 µs | 167.2 µs | 102.7 µs (sd 1.2) | 9,172 qps |
| bulk INSERT, 20 rows ascending | B | 87.6 µs | 92.2 µs | 94.2 µs | 105.6 µs | 129.0 µs | 213.2 µs | 104.2 µs (sd 1.1) | 9,038 qps |
| COUNT/MIN/MAX over 100 ids (walks the relation) | A | 81.2 µs | 84.9 µs | 85.9 µs | 89.0 µs | 102.2 µs | 116.3 µs | 88.9 µs (sd 1.1) | 11,188 qps |
| COUNT/MIN/MAX over 100 ids (walks the relation) | B | 81.4 µs | 86.1 µs | 87.2 µs | 90.4 µs | 103.8 µs | 116.0 µs | 90.1 µs (sd 1.1) | 11,050 qps |
| SHOW META (control) | A | 35.4 µs | 44.3 µs | 45.1 µs | 45.9 µs | 54.0 µs | 62.0 µs | 45.8 µs (sd 0.5) | 21,716 qps |
| SHOW META (control) | B | 44.6 µs | 49.5 µs | 50.1 µs | 52.3 µs | 61.7 µs | 69.3 µs | 51.8 µs (sd 0.5) | 19,192 qps |
| SHOW NAMESPACES (control) | A | 31.1 µs | 37.3 µs | 37.6 µs | 38.1 µs | 47.1 µs | 56.1 µs | 38.8 µs (sd 0.3) | 25,630 qps |
| SHOW NAMESPACES (control) | B | 31.4 µs | 37.4 µs | 37.7 µs | 38.2 µs | 47.2 µs | 56.1 µs | 38.9 µs (sd 0.3) | 25,568 qps |
| SET ISOLATION LEVEL (control) | A | 31.5 µs | 37.4 µs | 37.7 µs | 38.1 µs | 46.6 µs | 54.4 µs | 38.7 µs (sd 0.3) | 25,713 qps |
| SET ISOLATION LEVEL (control) | B | 33.3 µs | 37.3 µs | 37.6 µs | 38.1 µs | 46.6 µs | 54.3 µs | 38.7 µs (sd 0.3) | 25,713 qps |

| shape (cores = 1, 200 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | +0.00 µs [-0.10, +0.10] | -0.06 µs [-0.19, +0.06] | +0.10 % [-0.26, +0.46] | 49.1 µs | no difference resolved |
| INSERT again (replicate) | +0.05 µs [-0.20, +0.15] | -1.15 µs [-2.19, -0.12] | +2.18 % [+1.89, +2.72] | 49.1 µs | no difference resolved |
| SELECT by pk | +0.50 µs [+0.40, +1.10] | +0.72 µs [+0.39, +1.06] | -1.02 % [-1.36, -0.42] | 64.8 µs | **B slower** (+0.8 % at p50) |
| UPDATE by pk | +0.55 µs [+0.30, +0.90] | -1.32 µs [-2.29, -0.36] | +1.80 % [-0.30, +4.60] | 50.9 µs | **B slower** (+1.1 % at p50) |
| DELETE by pk | +0.10 µs [+0.10, +0.30] | -2.16 µs [-3.42, -1.22] | +3.07 % [+1.70, +4.06] | 48.8 µs | **B slower** (+0.2 % at p50) |
| bulk INSERT, 20 rows ascending | +0.40 µs [-0.10, +0.60] | +1.45 µs [+0.81, +1.99] | -1.62 % [-2.03, -0.73] | 93.9 µs | no difference resolved |
| COUNT/MIN/MAX over 100 ids (walks the relation) | +1.40 µs [+0.80, +1.90] | +1.17 µs [+0.65, +1.71] | -1.39 % [-1.81, -0.54] | 85.7 µs | **B slower** (+1.6 % at p50) |
| SHOW META (control) | +4.90 µs [+4.80, +5.10] | +5.98 µs [+5.81, +6.13] | -11.48 % [-11.95, -11.13] | 45.2 µs | **B slower** (+10.8 % at p50) |
| SHOW NAMESPACES (control) | +0.10 µs [-0.10, +0.20] | +0.04 µs [-0.08, +0.17] | -0.24 % [-0.67, +0.38] | 37.5 µs | no difference resolved |
| SET ISOLATION LEVEL (control) | +0.00 µs [-0.10, +0.10] | -0.03 µs [-0.16, +0.14] | +0.16 % [-0.15, +0.59] | 37.7 µs | no difference resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -0.05 µs, sd 0.15 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -0.10 µs, sd 0.17 µs.

**cores = 1, 1,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.56-1.91; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 40.9 µs | 48.2 µs | 49.0 µs | 50.0 µs | 62.5 µs | 77.1 µs | 50.7 µs (sd 0.3) | 19,470 qps |
| INSERT, pk omitted | B | 40.6 µs | 48.2 µs | 49.0 µs | 50.0 µs | 62.7 µs | 78.3 µs | 50.9 µs (sd 0.9) | 19,436 qps |
| INSERT again (replicate) | A | 40.4 µs | 48.1 µs | 49.0 µs | 50.0 µs | 62.6 µs | 76.2 µs | 54.1 µs (sd 1.3) | 18,213 qps |
| INSERT again (replicate) | B | 40.8 µs | 48.2 µs | 49.1 µs | 50.1 µs | 62.9 µs | 76.6 µs | 51.4 µs (sd 1.0) | 19,365 qps |
| SELECT by pk | A | 58.0 µs | 64.2 µs | 66.4 µs | 71.9 µs | 83.7 µs | 98.4 µs | 69.8 µs (sd 1.8) | 14,327 qps |
| SELECT by pk | B | 59.7 µs | 64.7 µs | 67.1 µs | 73.0 µs | 84.4 µs | 97.2 µs | 70.2 µs (sd 0.8) | 14,168 qps |
| UPDATE by pk | A | 41.2 µs | 50.6 µs | 51.3 µs | 52.7 µs | 65.4 µs | 78.9 µs | 57.7 µs (sd 1.5) | 17,243 qps |
| UPDATE by pk | B | 39.4 µs | 50.8 µs | 51.5 µs | 52.9 µs | 65.6 µs | 78.4 µs | 54.0 µs (sd 0.8) | 18,311 qps |
| DELETE by pk | A | 39.5 µs | 48.1 µs | 48.8 µs | 49.9 µs | 61.3 µs | 70.7 µs | 53.6 µs (sd 2.1) | 18,789 qps |
| DELETE by pk | B | 39.9 µs | 48.2 µs | 49.0 µs | 50.0 µs | 61.8 µs | 71.0 µs | 51.0 µs (sd 0.8) | 19,574 qps |
| bulk INSERT, 20 rows ascending | A | 89.1 µs | 92.7 µs | 94.5 µs | 105.3 µs | 120.5 µs | 166.6 µs | 103.6 µs (sd 1.5) | 9,144 qps |
| bulk INSERT, 20 rows ascending | B | 88.1 µs | 92.4 µs | 94.5 µs | 105.7 µs | 128.9 µs | 214.8 µs | 104.5 µs (sd 2.7) | 9,115 qps |
| COUNT/MIN/MAX over 100 ids (walks the relation) | A | 133.8 µs | 140.8 µs | 144.0 µs | 151.7 µs | 165.5 µs | 188.9 µs | 147.7 µs (sd 1.2) | 6,743 qps |
| COUNT/MIN/MAX over 100 ids (walks the relation) | B | 137.8 µs | 145.5 µs | 148.8 µs | 156.0 µs | 170.4 µs | 190.0 µs | 152.4 µs (sd 2.1) | 6,541 qps |
| SHOW META (control) | A | 31.7 µs | 44.4 µs | 45.2 µs | 46.0 µs | 54.3 µs | 61.6 µs | 45.9 µs (sd 0.2) | 21,670 qps |
| SHOW META (control) | B | 43.4 µs | 49.5 µs | 50.3 µs | 52.6 µs | 61.6 µs | 69.4 µs | 51.9 µs (sd 0.5) | 19,190 qps |
| SHOW NAMESPACES (control) | A | 33.3 µs | 37.3 µs | 37.6 µs | 38.1 µs | 47.4 µs | 56.7 µs | 38.9 µs (sd 0.3) | 25,565 qps |
| SHOW NAMESPACES (control) | B | 33.5 µs | 37.3 µs | 37.6 µs | 38.1 µs | 46.9 µs | 56.1 µs | 38.8 µs (sd 0.5) | 25,650 qps |
| SET ISOLATION LEVEL (control) | A | 33.2 µs | 37.3 µs | 37.6 µs | 38.0 µs | 46.2 µs | 54.1 µs | 38.6 µs (sd 0.3) | 25,825 qps |
| SET ISOLATION LEVEL (control) | B | 30.2 µs | 37.3 µs | 37.5 µs | 38.0 µs | 46.0 µs | 54.2 µs | 38.5 µs (sd 0.4) | 25,863 qps |

| shape (cores = 1, 1,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | +0.05 µs [+0.00, +0.20] | +0.24 µs [-0.11, +0.69] | -0.18 % [-0.92, +0.20] | 49.0 µs | no difference resolved |
| INSERT again (replicate) | +0.10 µs [+0.00, +0.20] | -2.75 µs [-3.38, -2.07] | +5.29 % [+4.50, +6.14] | 48.9 µs | no difference resolved |
| SELECT by pk | +0.60 µs [+0.35, +1.00] | +0.39 µs [-0.32, +0.93] | -1.20 % [-1.49, -0.63] | 66.3 µs | **B slower** (+0.9 % at p50) |
| UPDATE by pk | +0.20 µs [-0.10, +0.40] | -3.72 µs [-4.43, -3.12] | +6.05 % [+5.00, +7.80] | 51.3 µs | no difference resolved |
| DELETE by pk | +0.20 µs [+0.00, +0.40] | -2.59 µs [-3.80, -1.59] | +4.19 % [+2.33, +5.67] | 48.8 µs | no difference resolved |
| bulk INSERT, 20 rows ascending | -0.10 µs [-1.00, +0.70] | +0.93 µs [-0.49, +2.62] | -0.37 % [-1.60, +0.64] | 94.5 µs | no difference resolved |
| COUNT/MIN/MAX over 100 ids (walks the relation) | +4.75 µs [+4.40, +5.50] | +4.73 µs [+3.70, +5.98] | -2.81 % [-3.53, -2.48] | 143.9 µs | **B slower** (+3.3 % at p50) |
| SHOW META (control) | +4.95 µs [+4.85, +5.20] | +5.98 µs [+5.75, +6.20] | -11.50 % [-12.31, -10.83] | 45.2 µs | **B slower** (+11.0 % at p50) |
| SHOW NAMESPACES (control) | +0.00 µs [-0.10, +0.10] | -0.11 µs [-0.29, +0.08] | +0.08 % [-0.19, +0.87] | 37.6 µs | no difference resolved |
| SET ISOLATION LEVEL (control) | +0.00 µs [-0.10, +0.00] | -0.04 µs [-0.14, +0.05] | -0.00 % [-0.12, +0.42] | 37.6 µs | no difference resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.05 µs, sd 0.12 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.18 µs.

**cores = 1, 10,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.35-1.86; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 40.6 µs | 48.1 µs | 48.9 µs | 49.9 µs | 62.5 µs | 79.3 µs | 50.7 µs (sd 0.4) | 19,462 qps |
| INSERT, pk omitted | B | 39.9 µs | 48.1 µs | 49.0 µs | 50.0 µs | 62.5 µs | 77.1 µs | 50.9 µs (sd 1.2) | 19,452 qps |
| INSERT again (replicate) | A | 39.5 µs | 48.1 µs | 49.0 µs | 50.0 µs | 63.3 µs | 84.8 µs | 57.0 µs (sd 8.0) | 18,017 qps |
| INSERT again (replicate) | B | 40.2 µs | 48.2 µs | 49.0 µs | 50.0 µs | 62.8 µs | 77.3 µs | 51.4 µs (sd 1.0) | 19,388 qps |
| SELECT by pk | A | 58.4 µs | 63.3 µs | 64.1 µs | 65.8 µs | 78.2 µs | 92.5 µs | 66.6 µs (sd 2.4) | 15,057 qps |
| SELECT by pk | B | 59.0 µs | 63.8 µs | 64.6 µs | 66.3 µs | 78.9 µs | 96.0 µs | 67.7 µs (sd 3.7) | 14,895 qps |
| UPDATE by pk | A | 41.8 µs | 50.7 µs | 51.4 µs | 53.0 µs | 65.5 µs | 78.3 µs | 55.7 µs (sd 0.9) | 17,751 qps |
| UPDATE by pk | B | 41.9 µs | 51.1 µs | 51.9 µs | 53.6 µs | 66.0 µs | 78.9 µs | 54.9 µs (sd 0.9) | 18,080 qps |
| DELETE by pk | A | 38.4 µs | 48.2 µs | 49.0 µs | 50.0 µs | 60.8 µs | 70.2 µs | 50.4 µs (sd 0.3) | 19,751 qps |
| DELETE by pk | B | 40.0 µs | 48.3 µs | 49.1 µs | 50.2 µs | 61.1 µs | 70.2 µs | 51.3 µs (sd 0.8) | 19,297 qps |
| bulk INSERT, 20 rows ascending | A | 87.7 µs | 92.9 µs | 94.9 µs | 105.3 µs | 120.9 µs | 169.1 µs | 107.5 µs (sd 6.6) | 9,021 qps |
| bulk INSERT, 20 rows ascending | B | 86.8 µs | 92.9 µs | 94.8 µs | 105.6 µs | 121.0 µs | 167.7 µs | 127.8 µs (sd 14.2) | 7,227 qps |
| COUNT/MIN/MAX over 100 ids (walks the relation) | A | 705.0 µs | 759.4 µs | 783.3 µs | 807.4 µs | 834.6 µs | 933.9 µs | 790.4 µs (sd 5.7) | 1,264 qps |
| COUNT/MIN/MAX over 100 ids (walks the relation) | B | 739.4 µs | 798.6 µs | 826.1 µs | 853.7 µs | 923.9 µs | 997.9 µs | 852.3 µs (sd 32.5) | 1,175 qps |
| SHOW META (control) | A | 31.2 µs | 44.1 µs | 45.0 µs | 45.7 µs | 53.2 µs | 61.0 µs | 45.6 µs (sd 0.3) | 21,808 qps |
| SHOW META (control) | B | 44.6 µs | 49.6 µs | 50.2 µs | 52.4 µs | 61.6 µs | 68.6 µs | 51.9 µs (sd 0.3) | 19,194 qps |
| SHOW NAMESPACES (control) | A | 31.6 µs | 37.2 µs | 37.6 µs | 38.1 µs | 47.2 µs | 56.2 µs | 38.8 µs (sd 0.3) | 25,583 qps |
| SHOW NAMESPACES (control) | B | 32.3 µs | 37.2 µs | 37.5 µs | 38.1 µs | 47.1 µs | 56.0 µs | 38.8 µs (sd 0.2) | 25,630 qps |
| SET ISOLATION LEVEL (control) | A | 28.6 µs | 37.3 µs | 37.6 µs | 38.0 µs | 46.4 µs | 54.7 µs | 38.6 µs (sd 0.3) | 25,820 qps |
| SET ISOLATION LEVEL (control) | B | 32.9 µs | 37.3 µs | 37.6 µs | 38.1 µs | 46.5 µs | 55.0 µs | 38.7 µs (sd 0.3) | 25,709 qps |

| shape (cores = 1, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | +0.00 µs [-0.10, +0.20] | +0.23 µs [-0.19, +0.89] | +0.18 % [-0.26, +0.57] | 48.9 µs | no difference resolved |
| INSERT again (replicate) | +0.00 µs [-0.10, +0.15] | -5.62 µs [-9.71, -3.23] | +7.80 % [+5.42, +8.92] | 49.0 µs | no difference resolved |
| SELECT by pk | +0.50 µs [+0.20, +0.80] | +1.17 µs [+0.24, +2.64] | -1.12 % [-1.46, -0.36] | 64.0 µs | **B slower** (+0.8 % at p50) |
| UPDATE by pk | +0.65 µs [+0.00, +0.80] | -0.87 µs [-1.46, -0.31] | +1.17 % [+0.57, +2.80] | 51.4 µs | no difference resolved |
| DELETE by pk | +0.10 µs [+0.00, +0.20] | +0.98 µs [+0.61, +1.35] | -2.11 % [-2.74, -0.81] | 49.0 µs | no difference resolved |
| bulk INSERT, 20 rows ascending | -0.10 µs [-0.90, +0.90] | +20.35 µs [+10.17, +29.16] | -19.53 % [-22.49, -17.56] | 94.6 µs | no difference resolved |
| COUNT/MIN/MAX over 100 ids (walks the relation) | +39.65 µs [+37.90, +41.10] | +61.96 µs [+49.72, +78.72] | -7.11 % [-7.89, -5.07] | 783.4 µs | **B slower** (+5.1 % at p50) |
| SHOW META (control) | +5.20 µs [+4.90, +5.30] | +6.29 µs [+6.14, +6.46] | -11.82 % [-12.41, -11.66] | 45.0 µs | **B slower** (+11.5 % at p50) |
| SHOW NAMESPACES (control) | +0.00 µs [-0.10, +0.10] | -0.07 µs [-0.18, +0.03] | +0.24 % [-0.15, +0.36] | 37.6 µs | no difference resolved |
| SET ISOLATION LEVEL (control) | +0.00 µs [+0.00, +0.10] | +0.06 µs [-0.08, +0.17] | -0.20 % [-0.46, +0.06] | 37.6 µs | no difference resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.10 µs, sd 0.17 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.10 µs, sd 0.15 µs.
