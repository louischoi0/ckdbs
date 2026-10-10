
**strict, cores = 1, 200 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.34-2.00; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 863.5 µs | 984.3 µs | 1,024.2 µs | 1,086.4 µs | 2,089.0 µs | 5,181.0 µs | 1247.5 µs (sd 108.7) | 818 qps |
| INSERT, pk omitted | B | 859.7 µs | 986.4 µs | 1,026.8 µs | 1,091.8 µs | 2,116.8 µs | 5,283.5 µs | 1270.9 µs (sd 128.2) | 814 qps |
| INSERT again (replicate) | A | 863.9 µs | 990.0 µs | 1,033.8 µs | 1,112.4 µs | 2,243.5 µs | 5,186.3 µs | 1249.6 µs (sd 115.3) | 828 qps |
| INSERT again (replicate) | B | 874.7 µs | 990.0 µs | 1,034.6 µs | 1,114.3 µs | 2,188.2 µs | 5,201.7 µs | 1261.8 µs (sd 100.8) | 810 qps |
| SET ISOLATION LEVEL (control) | A | 33.3 µs | 37.4 µs | 37.7 µs | 38.6 µs | 51.1 µs | 63.1 µs | 40.2 µs (sd 1.7) | 25,186 qps |
| SET ISOLATION LEVEL (control) | B | 33.3 µs | 37.4 µs | 37.8 µs | 38.6 µs | 52.0 µs | 61.8 µs | 40.2 µs (sd 1.9) | 25,226 qps |

| shape (strict, cores = 1, 200 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | +4.95 µs [-1.20, +7.30] | +23.42 µs [-15.50, +79.83] | +0.33 % [-3.18, +1.93] | 1022.0 µs | no difference resolved |
| INSERT again (replicate) | +1.50 µs [-4.50, +3.80] | +12.20 µs [-17.29, +44.81] | -0.41 % [-1.47, +2.03] | 1026.0 µs | no difference resolved |
| SET ISOLATION LEVEL (control) | +0.10 µs [+0.00, +0.10] | +0.01 µs [-0.22, +0.30] | +0.05 % [-0.16, +0.72] | 37.7 µs | no difference resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +4.35 µs, sd 62.20 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +5.35 µs, sd 55.37 µs.

**strict, cores = 1, 1,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.23-4.16; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 869.9 µs | 983.0 µs | 1,022.5 µs | 1,090.6 µs | 2,595.4 µs | 6,176.8 µs | 1497.1 µs (sd 1208.6) | 836 qps |
| INSERT, pk omitted | B | 861.4 µs | 983.1 µs | 1,022.6 µs | 1,090.7 µs | 2,823.0 µs | 9,431.0 µs | 2354.6 µs (sd 4537.7) | 834 qps |
| INSERT again (replicate) | A | 857.5 µs | 986.8 µs | 1,028.2 µs | 1,097.6 µs | 2,325.9 µs | 5,713.5 µs | 1584.4 µs (sd 1356.6) | 825 qps |
| INSERT again (replicate) | B | 870.5 µs | 987.6 µs | 1,029.1 µs | 1,100.2 µs | 2,397.9 µs | 6,007.4 µs | 1741.7 µs (sd 1905.7) | 821 qps |
| SET ISOLATION LEVEL (control) | A | 33.0 µs | 37.4 µs | 37.8 µs | 38.9 µs | 52.3 µs | 62.4 µs | 40.4 µs (sd 3.5) | 25,366 qps |
| SET ISOLATION LEVEL (control) | B | 31.0 µs | 37.4 µs | 37.8 µs | 38.8 µs | 51.2 µs | 61.9 µs | 40.1 µs (sd 2.8) | 25,264 qps |

| shape (strict, cores = 1, 1,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | +0.35 µs [-1.70, +3.90] | +857.50 µs [+4.81, +2533.99] | -0.71 % [-1.94, +0.42] | 1017.4 µs | no difference resolved |
| INSERT again (replicate) | +2.85 µs [-3.90, +7.80] | +157.28 µs [-19.61, +446.71] | -0.07 % [-3.28, +1.10] | 1021.0 µs | no difference resolved |
| SET ISOLATION LEVEL (control) | +0.05 µs [+0.00, +0.10] | -0.32 µs [-1.19, +0.22] | +0.10 % [-0.69, +0.30] | 37.7 µs | no difference resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +6.15 µs, sd 81.87 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +5.45 µs, sd 50.10 µs.

**strict, cores = 1, 10,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.23-1.93; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 861.8 µs | 990.2 µs | 1,029.1 µs | 1,088.7 µs | 2,062.5 µs | 5,173.8 µs | 1288.7 µs (sd 161.3) | 807 qps |
| INSERT, pk omitted | B | 864.8 µs | 990.1 µs | 1,027.1 µs | 1,082.9 µs | 1,966.0 µs | 5,033.7 µs | 1226.1 µs (sd 77.9) | 826 qps |
| INSERT again (replicate) | A | 873.4 µs | 992.0 µs | 1,030.2 µs | 1,088.2 µs | 1,934.6 µs | 5,087.0 µs | 1231.4 µs (sd 80.8) | 826 qps |
| INSERT again (replicate) | B | 858.5 µs | 993.9 µs | 1,033.2 µs | 1,092.6 µs | 1,999.3 µs | 5,139.9 µs | 1229.7 µs (sd 56.2) | 815 qps |
| SET ISOLATION LEVEL (control) | A | 33.2 µs | 37.4 µs | 37.8 µs | 38.5 µs | 48.5 µs | 59.4 µs | 41.2 µs (sd 5.4) | 25,383 qps |
| SET ISOLATION LEVEL (control) | B | 33.4 µs | 37.4 µs | 37.8 µs | 38.4 µs | 48.4 µs | 57.6 µs | 39.7 µs (sd 1.2) | 25,265 qps |

| shape (strict, cores = 1, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | -2.25 µs [-4.10, -0.10] | -62.54 µs [-147.03, -10.83] | +2.92 % [+0.22, +5.22] | 1026.3 µs | B faster (-0.2 % at p50) |
| INSERT again (replicate) | +3.00 µs [-0.65, +7.00] | -1.76 µs [-34.96, +24.47] | -0.22 % [-1.24, +0.94] | 1030.0 µs | no difference resolved |
| SET ISOLATION LEVEL (control) | -0.05 µs [-0.10, +0.10] | -1.49 µs [-4.06, +0.49] | +0.07 % [-0.55, +1.25] | 37.8 µs | no difference resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.95 µs, sd 11.87 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +4.70 µs, sd 13.19 µs.
