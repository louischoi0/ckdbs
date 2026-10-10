
**strict, cores = 2, 200 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.14-1.43; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 854.2 µs | 977.7 µs | 1,013.4 µs | 1,069.1 µs | 1,822.7 µs | 5,166.4 µs | 1202.0 µs (sd 47.8) | 839 qps |
| INSERT, pk omitted | B | 868.6 µs | 994.0 µs | 1,031.9 µs | 1,090.9 µs | 1,930.0 µs | 5,288.7 µs | 1220.3 µs (sd 52.9) | 825 qps |
| INSERT again (replicate) | A | 851.2 µs | 980.0 µs | 1,017.6 µs | 1,077.1 µs | 1,952.9 µs | 5,306.2 µs | 1222.8 µs (sd 91.3) | 832 qps |
| INSERT again (replicate) | B | 875.0 µs | 996.0 µs | 1,033.3 µs | 1,092.1 µs | 1,985.1 µs | 5,272.0 µs | 1231.3 µs (sd 68.1) | 826 qps |
| SHOW META (control) | A | 36.7 µs | 42.9 µs | 45.2 µs | 46.3 µs | 57.3 µs | 65.8 µs | 46.5 µs (sd 1.3) | 21,407 qps |
| SHOW META (control) | B | 43.8 µs | 49.7 µs | 50.4 µs | 52.6 µs | 64.1 µs | 72.5 µs | 52.5 µs (sd 0.8) | 18,993 qps |

| shape (strict, cores = 2, 200 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | +17.50 µs [+11.50, +29.20] | +18.31 µs [-17.76, +51.43] | -2.85 % [-4.56, +2.39] | 1012.2 µs | **B slower** (+1.7 % at p50) |
| INSERT again (replicate) | +13.05 µs [+6.10, +22.40] | +8.46 µs [-13.25, +33.30] | -0.12 % [-1.39, +0.64] | 1014.2 µs | **B slower** (+1.3 % at p50) |
| SHOW META (control) | +5.05 µs [+4.90, +6.70] | +5.97 µs [+5.23, +6.71] | -11.41 % [-13.27, -10.08] | 45.2 µs | **B slower** (+11.2 % at p50) |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -2.85 µs, sd 21.23 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -3.05 µs, sd 21.95 µs.

**strict, cores = 2, 1,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.12-1.48; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 856.1 µs | 985.6 µs | 1,020.5 µs | 1,072.4 µs | 1,836.6 µs | 4,931.2 µs | 1188.5 µs (sd 32.4) | 843 qps |
| INSERT, pk omitted | B | 883.7 µs | 1,006.0 µs | 1,044.1 µs | 1,098.5 µs | 1,969.8 µs | 5,279.8 µs | 1226.4 µs (sd 29.5) | 813 qps |
| INSERT again (replicate) | A | 873.6 µs | 984.9 µs | 1,019.9 µs | 1,070.7 µs | 1,921.4 µs | 5,062.9 µs | 1207.0 µs (sd 44.5) | 836 qps |
| INSERT again (replicate) | B | 887.2 µs | 1,005.4 µs | 1,042.2 µs | 1,094.7 µs | 1,882.8 µs | 5,115.9 µs | 1223.6 µs (sd 50.8) | 817 qps |
| SHOW META (control) | A | 31.9 µs | 44.1 µs | 45.2 µs | 46.2 µs | 56.6 µs | 65.5 µs | 46.2 µs (sd 1.1) | 21,466 qps |
| SHOW META (control) | B | 41.8 µs | 49.8 µs | 50.4 µs | 52.6 µs | 64.0 µs | 72.0 µs | 52.9 µs (sd 1.3) | 19,011 qps |

| shape (strict, cores = 2, 1,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | +25.45 µs [+12.45, +34.60] | +37.82 µs [+19.72, +56.54] | -2.33 % [-6.02, -0.74] | 1016.9 µs | **B slower** (+2.5 % at p50) |
| INSERT again (replicate) | +26.65 µs [+7.70, +34.25] | +16.51 µs [-1.19, +33.54] | -1.73 % [-3.77, +0.32] | 1017.9 µs | **B slower** (+2.6 % at p50) |
| SHOW META (control) | +5.00 µs [+4.90, +5.30] | +6.72 µs [+5.92, +7.58] | -11.66 % [-13.75, -11.13] | 45.3 µs | **B slower** (+11.0 % at p50) |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -2.05 µs, sd 14.95 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -1.75 µs, sd 15.66 µs.

**strict, cores = 2, 10,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.12-1.48; 16 runs with a ctest/cc1plus/kds_tests entry in the competing list; 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 863.0 µs | 983.7 µs | 1,019.7 µs | 1,074.5 µs | 1,980.6 µs | 5,197.8 µs | 1207.2 µs (sd 26.1) | 831 qps |
| INSERT, pk omitted | B | 866.1 µs | 1,001.6 µs | 1,038.9 µs | 1,092.9 µs | 1,869.1 µs | 4,921.5 µs | 1214.2 µs (sd 23.1) | 821 qps |
| INSERT again (replicate) | A | 864.0 µs | 983.1 µs | 1,019.2 µs | 1,074.1 µs | 1,855.3 µs | 5,051.0 µs | 1194.3 µs (sd 34.1) | 839 qps |
| INSERT again (replicate) | B | 863.7 µs | 999.5 µs | 1,037.3 µs | 1,089.9 µs | 1,862.4 µs | 5,161.1 µs | 1211.4 µs (sd 28.5) | 824 qps |
| SHOW META (control) | A | 31.6 µs | 44.4 µs | 45.4 µs | 46.3 µs | 56.6 µs | 65.2 µs | 46.4 µs (sd 0.5) | 21,414 qps |
| SHOW META (control) | B | 43.3 µs | 49.8 µs | 50.6 µs | 53.0 µs | 64.4 µs | 72.4 µs | 52.6 µs (sd 0.4) | 18,912 qps |

| shape (strict, cores = 2, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |
|---|---|---|---|---|---|
| INSERT, pk omitted | +21.80 µs [+7.20, +30.60] | +6.99 µs [-4.33, +18.49] | -0.77 % [-1.49, +0.05] | 1022.3 µs | **B slower** (+2.1 % at p50) |
| INSERT again (replicate) | +23.35 µs [+1.70, +30.00] | +17.13 µs [-2.12, +36.62] | -1.58 % [-3.59, +1.62] | 1016.9 µs | **B slower** (+2.3 % at p50) |
| SHOW META (control) | +5.00 µs [+4.80, +5.50] | +6.15 µs [+5.91, +6.39] | -11.55 % [-12.06, -10.92] | 45.3 µs | **B slower** (+11.0 % at p50) |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -2.80 µs, sd 11.52 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -0.40 µs, sd 12.46 µs.
