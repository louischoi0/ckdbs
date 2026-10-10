
**group, cores = 1, 200 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.23-10.43; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 863.3 µs | 1,003.2 µs | 1,070.4 µs | 1,266.0 µs | 2,639.7 µs | 7,198.3 µs | 2137.5 µs (sd 3309.2) | 814 qps |
| INSERT, pk omitted | B | 869.6 µs | 1,002.0 µs | 1,070.6 µs | 1,271.7 µs | 2,750.1 µs | 9,169.8 µs | 2179.2 µs (sd 3465.8) | 824 qps |
| INSERT again (replicate) | A | 849.2 µs | 996.9 µs | 1,059.9 µs | 1,282.3 µs | 2,376.6 µs | 5,436.5 µs | 1320.5 µs (sd 205.2) | 808 qps |
| INSERT again (replicate) | B | 864.6 µs | 997.7 µs | 1,060.1 µs | 1,283.0 µs | 2,407.9 µs | 5,650.4 µs | 1535.1 µs (sd 883.1) | 829 qps |
| SET ISOLATION LEVEL (control) | A | 33.2 µs | 37.4 µs | 37.8 µs | 42.8 µs | 92.3 µs | 258.1 µs | 50.9 µs (sd 37.7) | 25,328 qps |
| SET ISOLATION LEVEL (control) | B | 33.6 µs | 37.5 µs | 37.9 µs | 43.4 µs | 95.5 µs | 318.7 µs | 52.6 µs (sd 37.2) | 25,191 qps |

| shape (group, cores = 1, 200 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | +0.90 µs [-3.15, +5.35] | +41.67 µs [-8.33, +123.69] | -0.53 % [-1.75, +0.97] | 1032.8 µs | no difference resolved |
| INSERT again (replicate) | -2.25 µs [-5.20, +3.20] | +214.63 µs [-7.54, +620.32] | -0.08 % [-3.65, +2.31] | 1034.0 µs | no difference resolved |
| SET ISOLATION LEVEL (control) | +0.10 µs [+0.00, +0.20] | +1.61 µs [+0.05, +3.67] | -0.50 % [-2.28, -0.10] | 37.7 µs | no difference resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -8.05 µs, sd 96.88 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -1.70 µs, sd 141.13 µs.

**group, cores = 1, 1,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.18-1.98; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 842.2 µs | 977.2 µs | 1,015.8 µs | 1,074.1 µs | 2,069.1 µs | 5,780.9 µs | 1578.6 µs (sd 729.0) | 845 qps |
| INSERT, pk omitted | B | 843.9 µs | 976.2 µs | 1,015.0 µs | 1,073.7 µs | 2,075.2 µs | 5,736.8 µs | 1508.3 µs (sd 739.9) | 837 qps |
| INSERT again (replicate) | A | 855.6 µs | 979.6 µs | 1,018.2 µs | 1,076.8 µs | 2,083.9 µs | 5,610.6 µs | 1474.3 µs (sd 638.0) | 835 qps |
| INSERT again (replicate) | B | 854.2 µs | 978.4 µs | 1,016.6 µs | 1,073.9 µs | 2,074.9 µs | 5,370.8 µs | 1442.1 µs (sd 584.6) | 836 qps |
| SET ISOLATION LEVEL (control) | A | 30.6 µs | 37.5 µs | 37.9 µs | 38.7 µs | 50.8 µs | 60.7 µs | 40.5 µs (sd 1.8) | 25,125 qps |
| SET ISOLATION LEVEL (control) | B | 33.3 µs | 37.5 µs | 37.9 µs | 38.6 µs | 49.9 µs | 59.4 µs | 39.7 µs (sd 1.4) | 25,289 qps |

| shape (group, cores = 1, 1,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | -1.20 µs [-5.50, +3.20] | -70.34 µs [-221.71, +73.24] | -0.79 % [-2.76, +4.67] | 1005.2 µs | no difference resolved |
| INSERT again (replicate) | -2.55 µs [-5.40, +2.70] | -32.12 µs [-262.63, +130.18] | -0.28 % [-2.69, +4.20] | 1016.1 µs | no difference resolved |
| SET ISOLATION LEVEL (control) | -0.05 µs [-0.20, +0.10] | -0.81 µs [-1.37, -0.29] | +1.06 % [-0.06, +3.87] | 37.9 µs | no difference resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +3.90 µs, sd 18.29 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +2.85 µs, sd 16.95 µs.

**group, cores = 1, 10,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.62-2.00; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 861.4 µs | 988.1 µs | 1,025.0 µs | 1,083.0 µs | 2,021.9 µs | 5,354.0 µs | 1380.2 µs (sd 367.9) | 784 qps |
| INSERT, pk omitted | B | 872.6 µs | 986.1 µs | 1,023.1 µs | 1,079.7 µs | 2,003.6 µs | 5,239.6 µs | 1324.1 µs (sd 303.4) | 811 qps |
| INSERT again (replicate) | A | 861.0 µs | 986.8 µs | 1,024.9 µs | 1,086.3 µs | 2,208.1 µs | 6,754.5 µs | 1854.9 µs (sd 2254.3) | 753 qps |
| INSERT again (replicate) | B | 864.0 µs | 986.1 µs | 1,024.2 µs | 1,083.8 µs | 2,006.0 µs | 5,301.7 µs | 1454.1 µs (sd 536.7) | 760 qps |
| SET ISOLATION LEVEL (control) | A | 31.6 µs | 37.4 µs | 37.8 µs | 38.6 µs | 48.8 µs | 60.6 µs | 39.9 µs (sd 1.8) | 25,262 qps |
| SET ISOLATION LEVEL (control) | B | 32.8 µs | 37.4 µs | 37.8 µs | 38.7 µs | 49.5 µs | 60.1 µs | 39.9 µs (sd 1.2) | 25,127 qps |

| shape (group, cores = 1, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | -1.00 µs [-3.40, +1.30] | -56.06 µs [-125.37, +18.76] | +2.27 % [-0.62, +12.47] | 1025.9 µs | no difference resolved |
| INSERT again (replicate) | -0.60 µs [-4.10, +3.80] | -400.82 µs [-1332.37, +117.20] | -0.79 % [-4.84, +5.27] | 1025.0 µs | no difference resolved |
| SET ISOLATION LEVEL (control) | +0.05 µs [+0.00, +0.10] | +0.07 µs [-0.42, +0.46] | -0.18 % [-1.04, +0.58] | 37.8 µs | no difference resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -2.15 µs, sd 18.42 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -0.75 µs, sd 17.04 µs.
