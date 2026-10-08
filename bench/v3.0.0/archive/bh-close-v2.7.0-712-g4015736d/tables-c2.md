
**cores = 2, 200 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.41-1.59, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.8 µs | 48.6 µs | 49.3 µs | 50.9 µs | 66.0 µs | 75.8 µs | 51.6 µs (sd 0.5) | 19,141 qps |
| INSERT, pk omitted | B | 42.4 µs | 48.4 µs | 49.1 µs | 50.5 µs | 65.2 µs | 73.8 µs | 51.2 µs (sd 0.4) | 19,279 qps |
| INSERT again (replicate) | A | 43.2 µs | 48.7 µs | 49.3 µs | 50.8 µs | 65.9 µs | 76.6 µs | 53.8 µs (sd 0.8) | 18,374 qps |
| INSERT again (replicate) | B | 41.9 µs | 48.4 µs | 49.1 µs | 50.6 µs | 65.1 µs | 74.9 µs | 53.5 µs (sd 0.7) | 18,520 qps |
| SELECT by pk | A | 58.8 µs | 65.1 µs | 66.0 µs | 69.8 µs | 83.8 µs | 94.8 µs | 69.0 µs (sd 0.7) | 14,375 qps |
| SELECT by pk | B | 60.4 µs | 65.1 µs | 66.0 µs | 69.9 µs | 83.7 µs | 94.5 µs | 68.9 µs (sd 0.7) | 14,394 qps |
| UPDATE by pk | A | 46.5 µs | 51.8 µs | 52.5 µs | 54.3 µs | 68.3 µs | 77.2 µs | 57.6 µs (sd 1.9) | 17,325 qps |
| UPDATE by pk | B | 46.3 µs | 51.4 µs | 52.1 µs | 53.7 µs | 67.5 µs | 75.6 µs | 56.9 µs (sd 1.0) | 17,382 qps |
| DELETE by pk | A | 42.9 µs | 49.0 µs | 49.6 µs | 51.0 µs | 64.6 µs | 72.8 µs | 54.1 µs (sd 1.8) | 18,560 qps |
| DELETE by pk | B | 41.2 µs | 48.6 µs | 49.3 µs | 50.7 µs | 63.4 µs | 71.0 µs | 53.5 µs (sd 1.2) | 18,722 qps |
| bulk INSERT, 20 rows ascending | A | 96.0 µs | 99.3 µs | 101.6 µs | 113.0 µs | 129.5 µs | 166.3 µs | 110.4 µs (sd 1.0) | 8,596 qps |
| bulk INSERT, 20 rows ascending | B | 94.9 µs | 98.9 µs | 101.5 µs | 113.2 µs | 130.2 µs | 169.4 µs | 110.9 µs (sd 1.6) | 8,590 qps |
| COUNT/MIN/MAX over 100 ids | A | 83.0 µs | 86.7 µs | 87.6 µs | 91.0 µs | 107.6 µs | 116.3 µs | 90.8 µs (sd 0.7) | 10,946 qps |
| COUNT/MIN/MAX over 100 ids | B | 83.5 µs | 86.4 µs | 87.4 µs | 90.4 µs | 106.8 µs | 114.3 µs | 90.3 µs (sd 0.9) | 11,009 qps |
| SHOW META (control) | A | 34.0 µs | 44.7 µs | 45.7 µs | 46.4 µs | 56.4 µs | 64.2 µs | 46.4 µs (sd 0.6) | 21,362 qps |
| SHOW META (control) | B | 32.9 µs | 42.7 µs | 45.0 µs | 46.0 µs | 55.3 µs | 62.9 µs | 45.5 µs (sd 0.7) | 21,970 qps |

| shape (cores = 2, 200 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.50, -0.05] | -0.44 µs [-0.66, -0.22] | +1.07 % [+0.15, +1.35] | no cost resolved |
| INSERT again (replicate) | -0.15 µs [-0.40, +0.00] | -0.34 µs [-0.84, +0.16] | +0.23 % [-0.24, +1.35] | no cost resolved |
| SELECT by pk | -0.10 µs [-0.40, +0.40] | -0.06 µs [-0.40, +0.29] | +0.18 % [-0.46, +0.68] | no cost resolved |
| UPDATE by pk | -0.30 µs [-0.70, -0.10] | -0.70 µs [-1.76, +0.05] | +0.11 % [-0.28, +1.24] | no cost resolved |
| DELETE by pk | -0.20 µs [-0.50, +0.00] | -0.63 µs [-1.84, +0.51] | +0.84 % [-0.65, +2.50] | no cost resolved |
| bulk INSERT, 20 rows ascending | -0.45 µs [-0.90, +0.10] | +0.42 µs [-0.33, +1.22] | -0.29 % [-0.45, +0.41] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | -0.15 µs [-0.80, +0.00] | -0.53 µs [-0.98, -0.08] | +0.45 % [+0.05, +0.91] | no cost resolved |
| SHOW META (control) | -1.15 µs [-2.60, -0.10] | -0.92 µs [-1.42, -0.36] | +3.02 % [+0.25, +3.57] | B faster (-2.5 % at p50) |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.12 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.05 µs, sd 0.14 µs.

**cores = 2, 1,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.37-1.57, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.8 µs | 48.6 µs | 49.3 µs | 50.8 µs | 65.8 µs | 75.1 µs | 51.5 µs (sd 0.3) | 19,165 qps |
| INSERT, pk omitted | B | 42.6 µs | 48.4 µs | 49.1 µs | 50.5 µs | 65.5 µs | 74.7 µs | 51.1 µs (sd 0.3) | 19,301 qps |
| INSERT again (replicate) | A | 39.3 µs | 48.6 µs | 49.3 µs | 50.7 µs | 65.5 µs | 75.3 µs | 54.2 µs (sd 0.9) | 18,325 qps |
| INSERT again (replicate) | B | 40.6 µs | 48.4 µs | 49.1 µs | 50.4 µs | 64.8 µs | 74.4 µs | 54.5 µs (sd 2.2) | 18,411 qps |
| SELECT by pk | A | 60.8 µs | 65.6 µs | 68.0 µs | 73.8 µs | 86.8 µs | 99.3 µs | 71.0 µs (sd 0.5) | 13,958 qps |
| SELECT by pk | B | 61.2 µs | 65.3 µs | 67.6 µs | 73.5 µs | 86.3 µs | 98.0 µs | 70.7 µs (sd 0.4) | 14,017 qps |
| UPDATE by pk | A | 47.1 µs | 51.9 µs | 52.6 µs | 54.4 µs | 68.4 µs | 76.9 µs | 59.6 µs (sd 1.2) | 16,619 qps |
| UPDATE by pk | B | 46.8 µs | 51.4 µs | 52.1 µs | 53.8 µs | 67.8 µs | 75.4 µs | 59.4 µs (sd 1.3) | 16,700 qps |
| DELETE by pk | A | 43.5 µs | 48.9 µs | 49.5 µs | 51.1 µs | 64.9 µs | 72.4 µs | 54.0 µs (sd 0.7) | 18,450 qps |
| DELETE by pk | B | 42.9 µs | 48.5 µs | 49.2 µs | 50.6 µs | 63.1 µs | 70.8 µs | 53.2 µs (sd 0.8) | 18,764 qps |
| bulk INSERT, 20 rows ascending | A | 95.9 µs | 99.4 µs | 101.7 µs | 113.1 µs | 130.2 µs | 167.0 µs | 111.0 µs (sd 1.5) | 8,586 qps |
| bulk INSERT, 20 rows ascending | B | 95.3 µs | 98.9 µs | 101.3 µs | 113.1 µs | 129.3 µs | 171.7 µs | 110.9 µs (sd 1.5) | 8,542 qps |
| COUNT/MIN/MAX over 100 ids | A | 135.9 µs | 143.3 µs | 146.6 µs | 154.7 µs | 169.7 µs | 184.5 µs | 150.3 µs (sd 1.3) | 6,628 qps |
| COUNT/MIN/MAX over 100 ids | B | 136.7 µs | 144.3 µs | 147.7 µs | 154.9 µs | 169.8 µs | 180.8 µs | 151.0 µs (sd 1.1) | 6,595 qps |
| SHOW META (control) | A | 36.5 µs | 43.5 µs | 45.5 µs | 46.2 µs | 55.5 µs | 63.6 µs | 46.0 µs (sd 0.9) | 21,501 qps |
| SHOW META (control) | B | 31.5 µs | 42.8 µs | 45.2 µs | 46.1 µs | 54.8 µs | 62.3 µs | 45.6 µs (sd 0.8) | 21,808 qps |

| shape (cores = 2, 1,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.30 µs [-0.35, -0.10] | -0.39 µs [-0.58, -0.19] | +0.80 % [+0.39, +1.29] | no cost resolved |
| INSERT again (replicate) | -0.20 µs [-0.40, -0.10] | +0.34 µs [-0.57, +1.50] | +0.72 % [-0.86, +1.48] | no cost resolved |
| SELECT by pk | -0.40 µs [-0.60, +0.00] | -0.38 µs [-0.65, -0.11] | +0.35 % [+0.17, +1.00] | no cost resolved |
| UPDATE by pk | -0.55 µs [-0.90, -0.20] | -0.16 µs [-0.73, +0.41] | +0.64 % [-1.72, +1.57] | no cost resolved |
| DELETE by pk | -0.35 µs [-0.50, +0.00] | -0.80 µs [-1.42, -0.21] | +1.52 % [-0.05, +2.71] | no cost resolved |
| bulk INSERT, 20 rows ascending | -0.55 µs [-1.10, -0.15] | -0.09 µs [-1.05, +0.92] | +0.19 % [-0.82, +1.12] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | +0.85 µs [+0.00, +1.50] | +0.69 µs [-0.06, +1.60] | -0.35 % [-0.85, +0.14] | no cost resolved |
| SHOW META (control) | -0.10 µs [-1.35, +0.10] | -0.43 µs [-0.96, +0.11] | +0.48 % [-0.23, +2.55] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.16 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.20 µs.

**cores = 2, 10,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.27-1.59, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.8 µs | 48.6 µs | 49.4 µs | 51.0 µs | 67.2 µs | 83.7 µs | 51.9 µs (sd 1.0) | 19,105 qps |
| INSERT, pk omitted | B | 42.0 µs | 48.3 µs | 49.1 µs | 50.6 µs | 66.0 µs | 77.4 µs | 51.3 µs (sd 0.6) | 19,212 qps |
| INSERT again (replicate) | A | 42.4 µs | 48.5 µs | 49.2 µs | 50.6 µs | 65.6 µs | 76.4 µs | 57.1 µs (sd 1.0) | 17,368 qps |
| INSERT again (replicate) | B | 41.6 µs | 48.4 µs | 49.1 µs | 50.4 µs | 65.1 µs | 75.9 µs | 56.2 µs (sd 0.8) | 17,626 qps |
| SELECT by pk | A | 59.2 µs | 64.8 µs | 65.5 µs | 67.4 µs | 81.2 µs | 90.1 µs | 67.7 µs (sd 0.6) | 14,630 qps |
| SELECT by pk | B | 59.2 µs | 64.5 µs | 65.3 µs | 67.0 µs | 80.6 µs | 89.6 µs | 67.4 µs (sd 0.6) | 14,702 qps |
| UPDATE by pk | A | 47.6 µs | 52.3 µs | 53.2 µs | 55.2 µs | 69.2 µs | 78.0 µs | 57.7 µs (sd 0.8) | 17,205 qps |
| UPDATE by pk | B | 46.1 µs | 51.8 µs | 52.6 µs | 54.6 µs | 68.4 µs | 76.3 µs | 57.1 µs (sd 1.1) | 17,460 qps |
| DELETE by pk | A | 43.1 µs | 48.8 µs | 49.5 µs | 51.2 µs | 64.5 µs | 72.8 µs | 51.5 µs (sd 0.6) | 19,380 qps |
| DELETE by pk | B | 41.8 µs | 48.5 µs | 49.2 µs | 50.6 µs | 63.3 µs | 71.0 µs | 51.0 µs (sd 0.4) | 19,497 qps |
| bulk INSERT, 20 rows ascending | A | 95.9 µs | 99.5 µs | 102.1 µs | 112.7 µs | 129.1 µs | 168.3 µs | 111.0 µs (sd 1.4) | 8,557 qps |
| bulk INSERT, 20 rows ascending | B | 95.3 µs | 99.0 µs | 101.6 µs | 112.6 µs | 128.8 µs | 170.6 µs | 110.1 µs (sd 0.9) | 8,590 qps |
| COUNT/MIN/MAX over 100 ids | A | 718.1 µs | 778.5 µs | 803.0 µs | 828.0 µs | 860.2 µs | 919.6 µs | 807.7 µs (sd 10.6) | 1,237 qps |
| COUNT/MIN/MAX over 100 ids | B | 734.0 µs | 793.8 µs | 818.6 µs | 844.2 µs | 874.5 µs | 907.8 µs | 828.9 µs (sd 9.4) | 1,206 qps |
| SHOW META (control) | A | 36.7 µs | 43.0 µs | 45.4 µs | 46.2 µs | 55.9 µs | 64.1 µs | 45.9 µs (sd 0.8) | 21,500 qps |
| SHOW META (control) | B | 31.5 µs | 42.7 µs | 45.3 µs | 46.1 µs | 55.6 µs | 63.4 µs | 45.7 µs (sd 0.9) | 21,644 qps |

| shape (cores = 2, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.25 µs [-0.50, +0.00] | -0.63 µs [-0.99, -0.34] | +0.74 % [+0.25, +1.61] | no cost resolved |
| INSERT again (replicate) | -0.10 µs [-0.30, +0.10] | -0.90 µs [-1.31, -0.54] | +1.27 % [+0.60, +2.10] | no cost resolved |
| SELECT by pk | -0.30 µs [-0.90, +0.20] | -0.34 µs [-0.75, +0.03] | +0.45 % [-0.56, +1.59] | no cost resolved |
| UPDATE by pk | -0.70 µs [-0.90, -0.50] | -0.60 µs [-1.17, +0.05] | +1.07 % [+0.80, +2.01] | B faster (-1.3 % at p50) |
| DELETE by pk | -0.25 µs [-0.60, +0.00] | -0.54 µs [-0.93, -0.18] | +0.66 % [-0.04, +1.78] | no cost resolved |
| bulk INSERT, 20 rows ascending | -0.55 µs [-0.90, +0.10] | -0.86 µs [-1.72, -0.12] | +0.33 % [-0.23, +0.85] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | +17.80 µs [+10.60, +22.40] | +21.22 µs [+13.96, +27.91] | -2.77 % [-3.71, -2.09] | **B slower** (+2.2 % at p50) |
| SHOW META (control) | -0.10 µs [-1.50, +0.20] | -0.21 µs [-0.77, +0.41] | +0.68 % [-0.37, +2.73] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -0.15 µs, sd 0.23 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.20 µs.
