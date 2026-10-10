
**group, cores = 2, 200 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.31-1.76; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 1,010.0 µs | 1,211.0 µs | 1,272.6 µs | 1,347.6 µs | 2,272.0 µs | 5,474.5 µs | 1450.9 µs (sd 34.3) | 690 qps |
| INSERT, pk omitted | B | 1,030.3 µs | 1,227.4 µs | 1,290.1 µs | 1,364.4 µs | 2,122.0 µs | 5,320.9 µs | 1454.0 µs (sd 34.5) | 688 qps |
| INSERT again (replicate) | A | 992.4 µs | 1,209.1 µs | 1,272.2 µs | 1,349.3 µs | 2,237.1 µs | 5,375.4 µs | 1448.7 µs (sd 31.7) | 689 qps |
| INSERT again (replicate) | B | 1,024.2 µs | 1,230.9 µs | 1,293.7 µs | 1,370.2 µs | 2,315.6 µs | 5,488.6 µs | 1473.2 µs (sd 29.6) | 676 qps |
| SET ISOLATION LEVEL (control) | A | 33.9 µs | 37.7 µs | 38.0 µs | 38.6 µs | 47.8 µs | 57.3 µs | 39.4 µs (sd 0.4) | 25,306 qps |
| SET ISOLATION LEVEL (control) | B | 34.8 µs | 37.7 µs | 38.0 µs | 38.6 µs | 47.5 µs | 57.0 µs | 39.4 µs (sd 0.6) | 25,394 qps |

| shape (group, cores = 2, 200 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | +14.05 µs [+8.80, +23.40] | +3.09 µs [-14.26, +21.64] | -0.19 % [-1.09, +1.47] | 1265.2 µs | **B slower** (+1.1 % at p50) |
| INSERT again (replicate) | +20.05 µs [+10.20, +32.00] | +24.50 µs [+13.41, +37.65] | -1.61 % [-2.29, -0.12] | 1266.1 µs | **B slower** (+1.6 % at p50) |
| SET ISOLATION LEVEL (control) | +0.00 µs [-0.10, +0.05] | +0.00 µs [-0.12, +0.13] | +0.19 % [-0.23, +0.44] | 38.0 µs | no difference resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -1.65 µs, sd 25.10 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -0.40 µs, sd 20.11 µs.

**group, cores = 2, 1,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.35-1.91; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 989.0 µs | 1,202.8 µs | 1,266.8 µs | 1,342.6 µs | 2,295.1 µs | 5,694.3 µs | 1457.2 µs (sd 47.6) | 689 qps |
| INSERT, pk omitted | B | 968.9 µs | 1,215.6 µs | 1,277.6 µs | 1,353.4 µs | 2,267.9 µs | 5,376.9 µs | 1460.0 µs (sd 51.8) | 690 qps |
| INSERT again (replicate) | A | 1,010.7 µs | 1,206.6 µs | 1,268.2 µs | 1,344.6 µs | 2,242.0 µs | 5,545.4 µs | 1452.7 µs (sd 62.3) | 694 qps |
| INSERT again (replicate) | B | 1,021.4 µs | 1,221.6 µs | 1,285.2 µs | 1,361.1 µs | 2,253.2 µs | 5,540.3 µs | 1464.8 µs (sd 47.5) | 682 qps |
| SET ISOLATION LEVEL (control) | A | 33.5 µs | 37.7 µs | 38.0 µs | 38.6 µs | 48.0 µs | 57.4 µs | 39.4 µs (sd 0.3) | 25,295 qps |
| SET ISOLATION LEVEL (control) | B | 33.6 µs | 37.7 µs | 38.0 µs | 38.7 µs | 48.4 µs | 57.9 µs | 39.5 µs (sd 0.6) | 25,296 qps |

| shape (group, cores = 2, 1,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | +12.95 µs [+2.10, +20.75] | +2.79 µs [-18.06, +24.61] | -0.08 % [-1.64, +1.22] | 1259.0 µs | **B slower** (+1.0 % at p50) |
| INSERT again (replicate) | +15.55 µs [+6.40, +22.40] | +12.14 µs [-6.26, +29.44] | -1.42 % [-2.27, +0.30] | 1265.8 µs | **B slower** (+1.2 % at p50) |
| SET ISOLATION LEVEL (control) | +0.00 µs [-0.10, +0.10] | +0.07 µs [-0.11, +0.29] | -0.08 % [-0.34, +0.27] | 38.0 µs | no difference resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +2.00 µs, sd 13.41 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +7.15 µs, sd 23.02 µs.

**group, cores = 2, 10,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.39-1.96; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 996.9 µs | 1,201.2 µs | 1,261.4 µs | 1,335.1 µs | 2,225.1 µs | 5,549.2 µs | 1446.5 µs (sd 41.6) | 694 qps |
| INSERT, pk omitted | B | 1,029.1 µs | 1,222.7 µs | 1,284.2 µs | 1,359.5 µs | 2,206.3 µs | 5,339.4 µs | 1463.4 µs (sd 32.4) | 685 qps |
| INSERT again (replicate) | A | 984.8 µs | 1,199.3 µs | 1,263.1 µs | 1,339.8 µs | 2,196.0 µs | 5,566.1 µs | 1440.7 µs (sd 46.2) | 700 qps |
| INSERT again (replicate) | B | 1,018.6 µs | 1,221.6 µs | 1,284.2 µs | 1,361.9 µs | 2,308.4 µs | 5,798.3 µs | 1474.8 µs (sd 43.0) | 679 qps |
| SET ISOLATION LEVEL (control) | A | 32.1 µs | 37.7 µs | 38.0 µs | 38.5 µs | 47.7 µs | 57.7 µs | 39.3 µs (sd 0.3) | 25,320 qps |
| SET ISOLATION LEVEL (control) | B | 34.6 µs | 37.7 µs | 38.0 µs | 38.6 µs | 47.6 µs | 57.1 µs | 39.3 µs (sd 0.3) | 25,350 qps |

| shape (group, cores = 2, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | +19.15 µs [+11.10, +32.40] | +16.97 µs [-3.52, +37.70] | -1.15 % [-2.57, +0.06] | 1258.0 µs | **B slower** (+1.5 % at p50) |
| INSERT again (replicate) | +21.30 µs [+12.00, +29.20] | +34.11 µs [+18.29, +48.58] | -2.45 % [-3.20, -1.82] | 1256.7 µs | **B slower** (+1.7 % at p50) |
| SET ISOLATION LEVEL (control) | +0.00 µs [-0.10, +0.10] | -0.02 µs [-0.21, +0.16] | +0.17 % [-0.19, +0.53] | 38.0 µs | no difference resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.95 µs, sd 21.70 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +6.55 µs, sd 19.47 µs.
