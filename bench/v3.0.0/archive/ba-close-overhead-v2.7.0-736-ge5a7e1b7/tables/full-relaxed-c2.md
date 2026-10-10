
**cores = 2, 200 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 0.59-1.68; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.2 µs | 48.6 µs | 49.5 µs | 50.9 µs | 66.2 µs | 77.1 µs | 51.7 µs (sd 0.4) | 19,079 qps |
| INSERT, pk omitted | B | 43.2 µs | 48.9 µs | 49.6 µs | 51.2 µs | 66.1 µs | 76.6 µs | 51.9 µs (sd 0.5) | 19,037 qps |
| INSERT again (replicate) | A | 42.2 µs | 48.6 µs | 49.4 µs | 50.9 µs | 66.4 µs | 78.0 µs | 54.1 µs (sd 1.2) | 18,412 qps |
| INSERT again (replicate) | B | 43.0 µs | 48.8 µs | 49.6 µs | 51.3 µs | 66.0 µs | 78.5 µs | 53.2 µs (sd 1.7) | 18,695 qps |
| SELECT by pk | A | 60.2 µs | 65.5 µs | 66.4 µs | 71.4 µs | 85.1 µs | 97.4 µs | 69.6 µs (sd 0.6) | 14,244 qps |
| SELECT by pk | B | 57.7 µs | 66.3 µs | 67.3 µs | 72.7 µs | 86.5 µs | 98.6 µs | 70.6 µs (sd 0.8) | 14,027 qps |
| UPDATE by pk | A | 44.8 µs | 51.6 µs | 52.2 µs | 54.1 µs | 68.2 µs | 78.1 µs | 57.8 µs (sd 1.7) | 17,303 qps |
| UPDATE by pk | B | 47.5 µs | 52.3 µs | 53.2 µs | 55.4 µs | 69.3 µs | 79.3 µs | 56.5 µs (sd 1.0) | 17,560 qps |
| DELETE by pk | A | 42.4 µs | 48.7 µs | 49.4 µs | 50.8 µs | 65.3 µs | 74.9 µs | 54.3 µs (sd 1.1) | 18,358 qps |
| DELETE by pk | B | 44.1 µs | 49.1 µs | 49.8 µs | 51.8 µs | 66.1 µs | 75.5 µs | 53.0 µs (sd 0.8) | 18,782 qps |
| bulk INSERT, 20 rows ascending | A | 94.7 µs | 99.5 µs | 102.1 µs | 114.3 µs | 134.0 µs | 175.4 µs | 111.6 µs (sd 2.2) | 8,461 qps |
| bulk INSERT, 20 rows ascending | B | 96.9 µs | 100.4 µs | 103.2 µs | 115.4 µs | 138.6 µs | 195.2 µs | 112.1 µs (sd 1.0) | 8,442 qps |
| COUNT/MIN/MAX over 100 ids (walks the relation) | A | 83.2 µs | 86.5 µs | 87.8 µs | 94.2 µs | 110.5 µs | 122.9 µs | 91.9 µs (sd 2.7) | 10,861 qps |
| COUNT/MIN/MAX over 100 ids (walks the relation) | B | 84.4 µs | 87.8 µs | 89.1 µs | 94.4 µs | 110.9 µs | 123.2 µs | 92.9 µs (sd 1.3) | 10,698 qps |
| SHOW META (control) | A | 33.6 µs | 44.1 µs | 45.6 µs | 46.4 µs | 56.5 µs | 65.5 µs | 46.3 µs (sd 0.8) | 21,440 qps |
| SHOW META (control) | B | 42.9 µs | 49.8 µs | 50.6 µs | 53.1 µs | 64.3 µs | 73.1 µs | 52.7 µs (sd 0.7) | 18,931 qps |
| SHOW NAMESPACES (control) | A | 31.1 µs | 37.7 µs | 38.1 µs | 38.8 µs | 51.3 µs | 60.1 µs | 39.8 µs (sd 0.9) | 25,072 qps |
| SHOW NAMESPACES (control) | B | 32.7 µs | 37.8 µs | 38.1 µs | 38.8 µs | 50.7 µs | 59.7 µs | 39.8 µs (sd 0.4) | 25,030 qps |
| SET ISOLATION LEVEL (control) | A | 33.9 µs | 37.8 µs | 38.1 µs | 38.6 µs | 48.1 µs | 57.3 µs | 39.5 µs (sd 0.3) | 25,227 qps |
| SET ISOLATION LEVEL (control) | B | 34.0 µs | 37.8 µs | 38.1 µs | 38.7 µs | 48.9 µs | 58.1 µs | 39.6 µs (sd 0.7) | 25,224 qps |

| shape (cores = 2, 200 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | +0.15 µs [+0.00, +0.30] | +0.27 µs [-0.01, +0.57] | -0.37 % [-0.97, -0.06] | 49.5 µs | no difference resolved |
| INSERT again (replicate) | +0.15 µs [-0.10, +0.30] | -0.99 µs [-1.64, -0.29] | +1.71 % [+0.64, +3.16] | 49.4 µs | no difference resolved |
| SELECT by pk | +0.95 µs [+0.65, +1.10] | +1.04 µs [+0.73, +1.36] | -1.49 % [-1.92, -0.86] | 66.4 µs | **B slower** (+1.4 % at p50) |
| UPDATE by pk | +0.85 µs [+0.60, +1.40] | -1.28 µs [-2.11, -0.54] | +1.90 % [+0.34, +3.17] | 52.1 µs | **B slower** (+1.6 % at p50) |
| DELETE by pk | +0.30 µs [+0.20, +0.80] | -1.30 µs [-1.84, -0.71] | +2.56 % [+1.34, +3.67] | 49.4 µs | **B slower** (+0.6 % at p50) |
| bulk INSERT, 20 rows ascending | +1.00 µs [+0.10, +1.90] | +0.46 µs [-0.69, +1.56] | -0.45 % [-1.22, +0.50] | 102.1 µs | **B slower** (+1.0 % at p50) |
| COUNT/MIN/MAX over 100 ids (walks the relation) | +1.35 µs [+0.30, +2.10] | +0.98 µs [-0.36, +1.95] | -1.33 % [-2.62, -0.15] | 87.7 µs | **B slower** (+1.5 % at p50) |
| SHOW META (control) | +4.90 µs [+4.80, +5.30] | +6.34 µs [+5.90, +6.84] | -11.78 % [-13.11, -10.88] | 45.6 µs | **B slower** (+10.7 % at p50) |
| SHOW NAMESPACES (control) | +0.00 µs [-0.10, +0.05] | -0.07 µs [-0.42, +0.22] | -0.12 % [-0.28, +0.29] | 38.1 µs | no difference resolved |
| SET ISOLATION LEVEL (control) | +0.00 µs [-0.10, +0.10] | +0.11 µs [-0.09, +0.35] | +0.09 % [-0.59, +0.41] | 38.0 µs | no difference resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -0.05 µs, sd 0.15 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -0.10 µs, sd 0.15 µs.

**cores = 2, 1,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.60-1.96; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.7 µs | 48.7 µs | 49.5 µs | 51.1 µs | 66.5 µs | 76.9 µs | 51.8 µs (sd 0.4) | 19,106 qps |
| INSERT, pk omitted | B | 43.0 µs | 48.8 µs | 49.5 µs | 51.1 µs | 66.3 µs | 76.4 µs | 51.8 µs (sd 0.5) | 18,990 qps |
| INSERT again (replicate) | A | 42.3 µs | 48.6 µs | 49.3 µs | 50.8 µs | 66.0 µs | 76.3 µs | 54.7 µs (sd 1.5) | 18,188 qps |
| INSERT again (replicate) | B | 42.4 µs | 48.7 µs | 49.4 µs | 51.0 µs | 66.1 µs | 77.8 µs | 52.4 µs (sd 0.8) | 18,950 qps |
| SELECT by pk | A | 59.7 µs | 65.9 µs | 68.7 µs | 74.4 µs | 88.8 µs | 102.7 µs | 72.2 µs (sd 1.9) | 13,824 qps |
| SELECT by pk | B | 60.8 µs | 66.7 µs | 69.6 µs | 75.4 µs | 89.7 µs | 102.3 µs | 72.7 µs (sd 0.5) | 13,627 qps |
| UPDATE by pk | A | 44.6 µs | 51.7 µs | 52.4 µs | 54.4 µs | 68.7 µs | 78.2 µs | 59.8 µs (sd 1.6) | 16,720 qps |
| UPDATE by pk | B | 45.7 µs | 52.4 µs | 53.2 µs | 55.5 µs | 69.2 µs | 78.8 µs | 56.4 µs (sd 0.5) | 17,571 qps |
| DELETE by pk | A | 43.0 µs | 48.7 µs | 49.4 µs | 50.9 µs | 64.7 µs | 73.4 µs | 53.7 µs (sd 0.8) | 18,584 qps |
| DELETE by pk | B | 43.5 µs | 49.0 µs | 49.7 µs | 51.6 µs | 65.5 µs | 74.5 µs | 52.6 µs (sd 1.3) | 18,968 qps |
| bulk INSERT, 20 rows ascending | A | 94.4 µs | 99.0 µs | 101.7 µs | 113.6 µs | 131.7 µs | 171.2 µs | 113.4 µs (sd 4.7) | 8,420 qps |
| bulk INSERT, 20 rows ascending | B | 96.6 µs | 100.5 µs | 103.4 µs | 115.6 µs | 139.6 µs | 195.1 µs | 112.7 µs (sd 2.7) | 8,456 qps |
| COUNT/MIN/MAX over 100 ids (walks the relation) | A | 135.6 µs | 143.0 µs | 146.4 µs | 155.0 µs | 170.8 µs | 186.2 µs | 151.1 µs (sd 3.1) | 6,602 qps |
| COUNT/MIN/MAX over 100 ids (walks the relation) | B | 139.6 µs | 146.6 µs | 149.8 µs | 157.9 µs | 175.5 µs | 193.3 µs | 154.0 µs (sd 1.3) | 6,475 qps |
| SHOW META (control) | A | 36.1 µs | 43.0 µs | 45.5 µs | 46.5 µs | 56.8 µs | 65.4 µs | 47.6 µs (sd 3.6) | 21,280 qps |
| SHOW META (control) | B | 43.3 µs | 50.2 µs | 50.9 µs | 53.6 µs | 64.5 µs | 73.3 µs | 59.3 µs (sd 11.5) | 18,745 qps |
| SHOW NAMESPACES (control) | A | 34.2 µs | 37.8 µs | 38.2 µs | 38.9 µs | 50.1 µs | 60.2 µs | 39.9 µs (sd 0.2) | 24,930 qps |
| SHOW NAMESPACES (control) | B | 30.9 µs | 37.9 µs | 38.2 µs | 39.1 µs | 51.0 µs | 60.0 µs | 40.0 µs (sd 0.4) | 24,893 qps |
| SET ISOLATION LEVEL (control) | A | 33.8 µs | 37.8 µs | 38.1 µs | 38.7 µs | 47.7 µs | 56.8 µs | 39.4 µs (sd 0.2) | 25,230 qps |
| SET ISOLATION LEVEL (control) | B | 30.8 µs | 37.8 µs | 38.1 µs | 38.6 µs | 47.6 µs | 57.4 µs | 39.3 µs (sd 0.2) | 25,223 qps |

| shape (cores = 2, 1,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | +0.10 µs [-0.10, +0.25] | +0.03 µs [-0.21, +0.26] | -0.19 % [-0.36, +0.35] | 49.4 µs | no difference resolved |
| INSERT again (replicate) | +0.00 µs [-0.05, +0.20] | -2.34 µs [-3.14, -1.60] | +4.02 % [+2.63, +5.18] | 49.3 µs | no difference resolved |
| SELECT by pk | +0.85 µs [+0.50, +1.20] | +0.55 µs [-0.46, +1.22] | -1.13 % [-1.56, -0.73] | 68.7 µs | **B slower** (+1.2 % at p50) |
| UPDATE by pk | +0.75 µs [+0.50, +1.20] | -3.46 µs [-4.20, -2.79] | +5.26 % [+4.47, +7.26] | 52.3 µs | **B slower** (+1.4 % at p50) |
| DELETE by pk | +0.25 µs [+0.15, +0.40] | -1.11 µs [-1.86, -0.39] | +2.51 % [+0.97, +3.32] | 49.4 µs | **B slower** (+0.5 % at p50) |
| bulk INSERT, 20 rows ascending | +1.25 µs [+0.90, +2.70] | -0.70 µs [-3.41, +1.88] | -0.58 % [-1.56, +3.16] | 101.2 µs | **B slower** (+1.2 % at p50) |
| COUNT/MIN/MAX over 100 ids (walks the relation) | +3.35 µs [+2.90, +4.20] | +2.83 µs [+0.78, +4.25] | -2.48 % [-3.17, -1.39] | 146.4 µs | **B slower** (+2.3 % at p50) |
| SHOW META (control) | +5.50 µs [+4.90, +7.50] | +11.62 µs [+5.98, +18.19] | -11.80 % [-25.34, -10.20] | 45.5 µs | **B slower** (+12.1 % at p50) |
| SHOW NAMESPACES (control) | +0.00 µs [+0.00, +0.15] | +0.10 µs [-0.08, +0.29] | +0.17 % [-0.71, +0.47] | 38.2 µs | no difference resolved |
| SET ISOLATION LEVEL (control) | +0.00 µs [-0.10, +0.10] | -0.04 µs [-0.19, +0.11] | +0.15 % [-0.37, +0.59] | 38.2 µs | no difference resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -0.05 µs, sd 0.24 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -0.10 µs, sd 0.19 µs.

**cores = 2, 10,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.45-1.95; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.0 µs | 48.7 µs | 49.5 µs | 50.9 µs | 66.5 µs | 79.7 µs | 51.7 µs (sd 0.3) | 19,050 qps |
| INSERT, pk omitted | B | 42.2 µs | 48.8 µs | 49.5 µs | 51.1 µs | 66.8 µs | 78.2 µs | 52.3 µs (sd 0.3) | 18,907 qps |
| INSERT again (replicate) | A | 42.6 µs | 48.6 µs | 49.4 µs | 50.8 µs | 65.8 µs | 78.7 µs | 57.4 µs (sd 1.4) | 17,225 qps |
| INSERT again (replicate) | B | 42.7 µs | 48.8 µs | 49.5 µs | 51.2 µs | 66.0 µs | 77.1 µs | 52.4 µs (sd 1.8) | 19,011 qps |
| SELECT by pk | A | 60.5 µs | 64.9 µs | 65.7 µs | 68.1 µs | 82.6 µs | 92.4 µs | 68.2 µs (sd 0.7) | 14,500 qps |
| SELECT by pk | B | 61.4 µs | 65.7 µs | 66.5 µs | 69.1 µs | 84.2 µs | 93.8 µs | 69.2 µs (sd 0.6) | 14,381 qps |
| UPDATE by pk | A | 45.9 µs | 51.9 µs | 52.7 µs | 54.9 µs | 69.3 µs | 79.2 µs | 57.7 µs (sd 1.5) | 17,368 qps |
| UPDATE by pk | B | 47.0 µs | 52.8 µs | 53.8 µs | 55.8 µs | 69.8 µs | 78.9 µs | 56.7 µs (sd 1.0) | 17,570 qps |
| DELETE by pk | A | 39.2 µs | 48.8 µs | 49.4 µs | 50.9 µs | 64.9 µs | 73.5 µs | 51.5 µs (sd 0.4) | 19,299 qps |
| DELETE by pk | B | 41.7 µs | 49.0 µs | 49.7 µs | 52.0 µs | 67.2 µs | 77.8 µs | 52.3 µs (sd 0.5) | 18,967 qps |
| bulk INSERT, 20 rows ascending | A | 95.0 µs | 99.5 µs | 102.3 µs | 114.1 µs | 133.0 µs | 170.7 µs | 112.1 µs (sd 2.5) | 8,482 qps |
| bulk INSERT, 20 rows ascending | B | 96.8 µs | 100.5 µs | 103.5 µs | 115.5 µs | 133.5 µs | 160.3 µs | 152.9 µs (sd 22.8) | 6,326 qps |
| COUNT/MIN/MAX over 100 ids (walks the relation) | A | 714.4 µs | 773.3 µs | 797.3 µs | 821.4 µs | 850.8 µs | 975.9 µs | 803.7 µs (sd 5.2) | 1,240 qps |
| COUNT/MIN/MAX over 100 ids (walks the relation) | B | 748.5 µs | 808.2 µs | 831.6 µs | 855.6 µs | 884.3 µs | 973.5 µs | 844.6 µs (sd 14.0) | 1,183 qps |
| SHOW META (control) | A | 31.3 µs | 43.0 µs | 45.4 µs | 46.3 µs | 56.6 µs | 65.8 µs | 46.0 µs (sd 0.9) | 21,547 qps |
| SHOW META (control) | B | 44.9 µs | 50.0 µs | 50.7 µs | 53.4 µs | 64.6 µs | 72.6 µs | 52.7 µs (sd 0.4) | 18,869 qps |
| SHOW NAMESPACES (control) | A | 33.8 µs | 37.8 µs | 38.1 µs | 38.8 µs | 50.2 µs | 59.9 µs | 39.8 µs (sd 0.3) | 24,981 qps |
| SHOW NAMESPACES (control) | B | 33.9 µs | 37.7 µs | 38.1 µs | 38.8 µs | 50.3 µs | 60.1 µs | 39.7 µs (sd 0.4) | 25,041 qps |
| SET ISOLATION LEVEL (control) | A | 34.8 µs | 37.7 µs | 38.1 µs | 38.6 µs | 48.7 µs | 57.8 µs | 39.5 µs (sd 0.6) | 25,209 qps |
| SET ISOLATION LEVEL (control) | B | 31.7 µs | 37.8 µs | 38.1 µs | 38.7 µs | 47.7 µs | 57.6 µs | 39.7 µs (sd 1.4) | 25,225 qps |

| shape (cores = 2, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | +0.10 µs [+0.00, +0.10] | +0.53 µs [+0.31, +0.76] | -0.88 % [-1.45, -0.41] | 49.5 µs | no difference resolved |
| INSERT again (replicate) | +0.05 µs [-0.10, +0.25] | -5.05 µs [-6.21, -3.67] | +10.44 % [+8.04, +10.85] | 49.5 µs | no difference resolved |
| SELECT by pk | +0.85 µs [+0.10, +1.40] | +1.01 µs [+0.66, +1.39] | -1.39 % [-2.48, -0.52] | 65.7 µs | **B slower** (+1.3 % at p50) |
| UPDATE by pk | +1.15 µs [+0.50, +1.40] | -1.06 µs [-1.85, -0.36] | +1.68 % [+0.64, +2.42] | 52.7 µs | **B slower** (+2.2 % at p50) |
| DELETE by pk | +0.20 µs [+0.20, +0.50] | +0.82 µs [+0.53, +1.09] | -1.70 % [-2.28, -0.88] | 49.4 µs | **B slower** (+0.4 % at p50) |
| bulk INSERT, 20 rows ascending | +0.95 µs [+0.35, +2.80] | +40.90 µs [+29.94, +51.50] | -25.84 % [-34.32, -17.88] | 102.5 µs | **B slower** (+0.9 % at p50) |
| COUNT/MIN/MAX over 100 ids (walks the relation) | +33.25 µs [+32.30, +36.20] | +40.89 µs [+34.94, +46.85] | -4.73 % [-5.64, -3.91] | 798.5 µs | **B slower** (+4.2 % at p50) |
| SHOW META (control) | +5.25 µs [+4.90, +6.90] | +6.73 µs [+6.26, +7.21] | -12.45 % [-14.02, -11.60] | 45.5 µs | **B slower** (+11.5 % at p50) |
| SHOW NAMESPACES (control) | +0.05 µs [-0.10, +0.10] | -0.06 µs [-0.34, +0.17] | +0.10 % [-0.34, +0.62] | 38.1 µs | no difference resolved |
| SET ISOLATION LEVEL (control) | +0.00 µs [-0.10, +0.20] | +0.22 µs [-0.25, +0.83] | +0.13 % [-1.02, +0.42] | 38.1 µs | no difference resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -0.10 µs, sd 0.16 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -0.10 µs, sd 0.13 µs.
