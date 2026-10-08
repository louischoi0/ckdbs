
**cores = 1 repeat, 1,000 rows** - 10 runs, 30,000 statements per arm per shape pooled (loadavg before each run 1.37-1.58, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 41.0 µs | 48.5 µs | 49.4 µs | 50.4 µs | 63.8 µs | 76.4 µs | 51.1 µs (sd 0.4) | 19,255 qps |
| INSERT, pk omitted | B | 41.1 µs | 48.5 µs | 49.3 µs | 50.3 µs | 63.1 µs | 74.8 µs | 50.8 µs (sd 0.6) | 19,411 qps |
| INSERT again (replicate) | A | 41.0 µs | 48.5 µs | 49.4 µs | 50.4 µs | 63.0 µs | 74.5 µs | 53.5 µs (sd 1.1) | 18,563 qps |
| INSERT again (replicate) | B | 41.2 µs | 48.5 µs | 49.3 µs | 50.2 µs | 62.4 µs | 73.9 µs | 53.0 µs (sd 0.8) | 18,595 qps |
| SELECT by pk | A | 58.4 µs | 64.7 µs | 67.0 µs | 72.9 µs | 84.7 µs | 99.7 µs | 70.0 µs (sd 0.5) | 14,169 qps |
| SELECT by pk | B | 58.3 µs | 64.7 µs | 67.1 µs | 72.6 µs | 84.7 µs | 97.2 µs | 69.8 µs (sd 0.5) | 14,218 qps |
| UPDATE by pk | A | 39.0 µs | 50.5 µs | 51.6 µs | 53.1 µs | 65.5 µs | 77.6 µs | 59.0 µs (sd 1.8) | 16,896 qps |
| UPDATE by pk | B | 40.7 µs | 50.8 µs | 51.7 µs | 53.2 µs | 65.9 µs | 77.3 µs | 58.4 µs (sd 0.8) | 16,984 qps |
| DELETE by pk | A | 41.0 µs | 48.5 µs | 49.3 µs | 50.4 µs | 62.4 µs | 70.7 µs | 53.0 µs (sd 0.5) | 18,761 qps |
| DELETE by pk | B | 40.6 µs | 48.6 µs | 49.4 µs | 50.3 µs | 61.9 µs | 70.2 µs | 52.5 µs (sd 1.1) | 19,035 qps |
| COUNT/MIN/MAX over 100 ids | A | 137.6 µs | 147.0 µs | 151.2 µs | 158.0 µs | 171.7 µs | 185.8 µs | 153.8 µs (sd 1.4) | 6,461 qps |
| COUNT/MIN/MAX over 100 ids | B | 135.2 µs | 141.7 µs | 144.8 µs | 151.3 µs | 164.6 µs | 179.6 µs | 147.9 µs (sd 1.0) | 6,723 qps |
| SHOW META (control) | A | 35.6 µs | 44.3 µs | 45.2 µs | 45.9 µs | 53.4 µs | 61.5 µs | 45.6 µs (sd 0.6) | 21,717 qps |
| SHOW META (control) | B | 32.2 µs | 44.2 µs | 45.3 µs | 45.9 µs | 53.6 µs | 61.2 µs | 45.6 µs (sd 0.7) | 21,750 qps |

| shape (cores = 1 repeat, 1,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.20, +0.05] | -0.27 µs [-0.50, -0.07] | +0.39 % [+0.05, +1.13] | no cost resolved |
| INSERT again (replicate) | -0.10 µs [-0.25, +0.10] | -0.47 µs [-1.40, +0.29] | +0.44 % [-0.45, +1.96] | no cost resolved |
| SELECT by pk | +0.05 µs [-0.30, +0.50] | -0.11 µs [-0.49, +0.27] | +0.27 % [-0.73, +0.87] | no cost resolved |
| UPDATE by pk | +0.15 µs [-0.35, +0.90] | -0.60 µs [-1.70, +0.46] | +0.55 % [-1.04, +3.15] | no cost resolved |
| DELETE by pk | +0.00 µs [-0.10, +0.15] | -0.43 µs [-1.01, +0.37] | +1.21 % [+0.45, +2.36] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | -6.70 µs [-7.05, -5.70] | -5.92 µs [-6.56, -5.26] | +3.94 % [+3.45, +4.54] | B faster (-4.4 % at p50) |
| SHOW META (control) | +0.00 µs [-0.30, +0.15] | +0.03 µs [-0.20, +0.28] | +0.01 % [-0.68, +0.73] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.13 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.14 µs.

**cores = 1 repeat, 10,000 rows** - 10 runs, 30,000 statements per arm per shape pooled (loadavg before each run 1.41-1.58, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 39.5 µs | 48.4 µs | 49.3 µs | 50.0 µs | 60.5 µs | 76.8 µs | 50.7 µs (sd 0.4) | 19,509 qps |
| INSERT, pk omitted | B | 41.1 µs | 48.4 µs | 49.3 µs | 50.0 µs | 59.5 µs | 72.5 µs | 50.3 µs (sd 0.4) | 19,565 qps |
| INSERT again (replicate) | A | 40.4 µs | 48.4 µs | 49.3 µs | 50.1 µs | 61.1 µs | 75.2 µs | 54.5 µs (sd 0.9) | 17,984 qps |
| INSERT again (replicate) | B | 40.5 µs | 48.4 µs | 49.3 µs | 50.0 µs | 60.4 µs | 73.3 µs | 53.9 µs (sd 0.5) | 18,362 qps |
| SELECT by pk | A | 57.3 µs | 63.3 µs | 64.3 µs | 65.8 µs | 76.6 µs | 87.8 µs | 66.0 µs (sd 1.3) | 15,054 qps |
| SELECT by pk | B | 58.5 µs | 63.7 µs | 64.5 µs | 65.8 µs | 76.0 µs | 82.2 µs | 65.9 µs (sd 0.4) | 15,022 qps |
| UPDATE by pk | A | 42.1 µs | 51.0 µs | 51.8 µs | 53.0 µs | 64.4 µs | 77.7 µs | 55.7 µs (sd 0.8) | 17,784 qps |
| UPDATE by pk | B | 44.4 µs | 50.9 µs | 51.7 µs | 52.9 µs | 64.5 µs | 76.8 µs | 55.4 µs (sd 0.9) | 17,934 qps |
| DELETE by pk | A | 40.2 µs | 48.4 µs | 49.3 µs | 50.1 µs | 59.9 µs | 68.3 µs | 50.4 µs (sd 0.2) | 19,730 qps |
| DELETE by pk | B | 39.7 µs | 48.4 µs | 49.3 µs | 50.1 µs | 59.3 µs | 68.1 µs | 50.3 µs (sd 0.2) | 19,740 qps |
| COUNT/MIN/MAX over 100 ids | A | 745.4 µs | 814.7 µs | 851.6 µs | 909.4 µs | 947.1 µs | 1,019.8 µs | 863.0 µs (sd 4.8) | 1,158 qps |
| COUNT/MIN/MAX over 100 ids | B | 709.5 µs | 757.2 µs | 781.4 µs | 805.0 µs | 829.9 µs | 924.1 µs | 785.9 µs (sd 3.4) | 1,271 qps |
| SHOW META (control) | A | 32.2 µs | 44.7 µs | 45.4 µs | 46.1 µs | 53.0 µs | 59.8 µs | 45.8 µs (sd 0.4) | 21,718 qps |
| SHOW META (control) | B | 31.0 µs | 44.7 µs | 45.4 µs | 46.1 µs | 53.3 µs | 60.0 µs | 46.1 µs (sd 0.6) | 21,572 qps |

| shape (cores = 1 repeat, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | +0.00 µs [-0.15, +0.10] | -0.41 µs [-0.77, -0.10] | +0.55 % [+0.15, +1.68] | no cost resolved |
| INSERT again (replicate) | +0.00 µs [-0.10, +0.15] | -0.65 µs [-1.11, -0.22] | +1.30 % [-0.14, +2.39] | no cost resolved |
| SELECT by pk | +0.50 µs [-0.45, +0.90] | -0.05 µs [-1.15, +0.73] | -0.42 % [-1.30, +0.74] | no cost resolved |
| UPDATE by pk | +0.05 µs [-0.60, +0.30] | -0.38 µs [-1.08, +0.28] | +0.69 % [-0.88, +1.76] | no cost resolved |
| DELETE by pk | +0.00 µs [-0.10, +0.15] | -0.06 µs [-0.22, +0.10] | +0.06 % [-0.39, +0.52] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | -69.35 µs [-72.40, -68.80] | -77.15 µs [-80.83, -73.56] | +9.80 % [+8.83, +10.34] | B faster (-8.2 % at p50) |
| SHOW META (control) | +0.05 µs [-0.15, +0.20] | +0.31 µs [+0.00, +0.67] | -0.48 % [-1.37, +0.30] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -0.05 µs, sd 0.11 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -0.10 µs, sd 0.14 µs.
