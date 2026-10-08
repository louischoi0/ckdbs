
**cores = 2, 200 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 0.94-1.00, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.2 µs | 48.8 µs | 49.8 µs | 53.9 µs | 69.7 µs | 82.9 µs | 53.2 µs (sd 1.7) | 18,657 qps |
| INSERT, pk omitted | B | 42.2 µs | 48.7 µs | 49.8 µs | 54.5 µs | 71.4 µs | 93.2 µs | 55.2 µs (sd 5.6) | 18,725 qps |
| INSERT again (replicate) | A | 42.3 µs | 48.7 µs | 49.7 µs | 52.8 µs | 68.4 µs | 80.9 µs | 55.4 µs (sd 1.4) | 17,883 qps |
| INSERT again (replicate) | B | 41.7 µs | 48.6 µs | 49.6 µs | 53.2 µs | 68.5 µs | 82.2 µs | 56.2 µs (sd 2.4) | 17,822 qps |
| SELECT by pk | A | 58.6 µs | 65.5 µs | 66.9 µs | 73.4 µs | 87.9 µs | 101.2 µs | 71.0 µs (sd 1.5) | 14,018 qps |
| SELECT by pk | B | 59.3 µs | 65.2 µs | 66.6 µs | 73.2 µs | 87.6 µs | 102.3 µs | 71.4 µs (sd 3.6) | 14,024 qps |
| UPDATE by pk | A | 43.8 µs | 51.8 µs | 52.7 µs | 57.8 µs | 75.4 µs | 241.7 µs | 63.2 µs (sd 14.5) | 16,775 qps |
| UPDATE by pk | B | 40.9 µs | 51.8 µs | 52.7 µs | 57.0 µs | 72.4 µs | 84.4 µs | 59.3 µs (sd 1.6) | 16,734 qps |
| DELETE by pk | A | 38.2 µs | 48.9 µs | 49.7 µs | 53.1 µs | 69.9 µs | 234.9 µs | 58.1 µs (sd 13.2) | 18,138 qps |
| DELETE by pk | B | 42.1 µs | 48.8 µs | 49.7 µs | 52.8 µs | 67.6 µs | 76.3 µs | 55.2 µs (sd 1.9) | 18,156 qps |
| COUNT/MIN/MAX over 100 ids | A | 78.7 µs | 86.6 µs | 88.7 µs | 96.2 µs | 114.3 µs | 178.9 µs | 97.5 µs (sd 10.5) | 10,595 qps |
| COUNT/MIN/MAX over 100 ids | B | 83.5 µs | 87.2 µs | 89.1 µs | 97.2 µs | 113.7 µs | 135.3 µs | 94.7 µs (sd 3.1) | 10,619 qps |
| SHOW META (control) | A | 31.6 µs | 45.1 µs | 45.8 µs | 47.1 µs | 60.1 µs | 70.1 µs | 48.4 µs (sd 3.9) | 21,028 qps |
| SHOW META (control) | B | 33.8 µs | 45.0 µs | 45.7 µs | 47.0 µs | 59.2 µs | 67.1 µs | 47.2 µs (sd 0.4) | 21,031 qps |

| shape (cores = 2, 200 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.05 µs [-0.20, +0.20] | +1.95 µs [-0.04, +4.43] | +0.07 % [-1.61, +0.83] | no cost resolved |
| INSERT again (replicate) | -0.10 µs [-0.25, +0.10] | +0.73 µs [-0.39, +1.94] | -0.58 % [-2.85, +1.06] | no cost resolved |
| SELECT by pk | -0.50 µs [-0.70, +0.50] | +0.48 µs [-0.97, +2.58] | +0.69 % [-1.21, +1.00] | no cost resolved |
| UPDATE by pk | +0.05 µs [-0.20, +0.20] | -3.93 µs [-11.04, +0.26] | +0.75 % [-1.15, +3.45] | no cost resolved |
| DELETE by pk | +0.05 µs [-0.40, +0.25] | -2.90 µs [-9.24, +0.64] | +0.09 % [-0.87, +1.03] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | +0.75 µs [+0.20, +1.20] | -2.85 µs [-8.17, +1.36] | -0.17 % [-1.34, +2.53] | **B slower** (+0.9 % at p50) |
| SHOW META (control) | -0.10 µs [-0.20, +0.05] | -1.23 µs [-3.29, -0.06] | +0.35 % [-0.20, +1.49] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 1.38 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -0.10 µs, sd 1.51 µs.

**cores = 2, 1,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.16-1.60, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.2 µs | 48.9 µs | 49.8 µs | 52.4 µs | 68.7 µs | 85.6 µs | 52.9 µs (sd 1.8) | 18,970 qps |
| INSERT, pk omitted | B | 40.6 µs | 48.8 µs | 49.7 µs | 51.6 µs | 68.1 µs | 85.1 µs | 54.5 µs (sd 9.2) | 19,052 qps |
| INSERT again (replicate) | A | 42.7 µs | 48.8 µs | 49.8 µs | 53.0 µs | 70.5 µs | 90.9 µs | 56.1 µs (sd 2.6) | 17,942 qps |
| INSERT again (replicate) | B | 41.9 µs | 48.7 µs | 49.8 µs | 53.4 µs | 71.9 µs | 107.2 µs | 60.9 µs (sd 14.2) | 18,022 qps |
| SELECT by pk | A | 59.5 µs | 66.1 µs | 70.1 µs | 75.9 µs | 95.0 µs | 131.8 µs | 76.2 µs (sd 8.9) | 13,588 qps |
| SELECT by pk | B | 59.0 µs | 65.9 µs | 69.5 µs | 75.6 µs | 94.0 µs | 151.0 µs | 76.8 µs (sd 11.7) | 13,788 qps |
| UPDATE by pk | A | 45.5 µs | 52.0 µs | 53.0 µs | 56.5 µs | 72.4 µs | 87.6 µs | 62.2 µs (sd 6.3) | 16,520 qps |
| UPDATE by pk | B | 45.2 µs | 52.0 µs | 53.0 µs | 56.3 µs | 72.9 µs | 89.0 µs | 64.3 µs (sd 10.5) | 16,276 qps |
| DELETE by pk | A | 40.2 µs | 48.9 µs | 49.8 µs | 52.0 µs | 67.5 µs | 77.6 µs | 55.4 µs (sd 2.0) | 18,145 qps |
| DELETE by pk | B | 43.1 µs | 48.9 µs | 49.8 µs | 52.2 µs | 68.5 µs | 87.8 µs | 60.2 µs (sd 19.2) | 18,170 qps |
| COUNT/MIN/MAX over 100 ids | A | 137.0 µs | 148.1 µs | 153.5 µs | 164.1 µs | 188.2 µs | 257.0 µs | 162.1 µs (sd 9.7) | 6,321 qps |
| COUNT/MIN/MAX over 100 ids | B | 136.2 µs | 144.0 µs | 147.9 µs | 159.1 µs | 183.7 µs | 252.4 µs | 159.6 µs (sd 14.3) | 6,551 qps |
| SHOW META (control) | A | 31.4 µs | 43.0 µs | 45.5 µs | 46.5 µs | 58.3 µs | 66.8 µs | 46.4 µs (sd 1.3) | 21,412 qps |
| SHOW META (control) | B | 32.4 µs | 43.0 µs | 45.5 µs | 46.5 µs | 58.6 µs | 67.2 µs | 46.2 µs (sd 1.4) | 21,547 qps |

| shape (cores = 2, 1,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | +0.05 µs [-0.30, +0.15] | +1.66 µs [-0.69, +5.94] | +0.43 % [+0.04, +0.82] | no cost resolved |
| INSERT again (replicate) | -0.10 µs [-0.25, +0.20] | +4.83 µs [-0.22, +11.76] | -1.36 % [-3.01, +1.00] | no cost resolved |
| SELECT by pk | -0.25 µs [-0.90, +0.50] | +0.59 µs [-5.31, +7.03] | +0.44 % [-0.34, +3.13] | no cost resolved |
| UPDATE by pk | +0.05 µs [-0.10, +0.30] | +2.12 µs [-0.72, +6.88] | -0.67 % [-1.81, +0.52] | no cost resolved |
| DELETE by pk | -0.05 µs [-0.25, +0.15] | +4.88 µs [-0.54, +14.72] | -0.35 % [-1.78, +0.88] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | -6.05 µs [-7.20, -3.95] | -2.46 µs [-5.53, +2.14] | +2.86 % [+1.57, +4.39] | B faster (-3.9 % at p50) |
| SHOW META (control) | +0.10 µs [-0.40, +0.40] | -0.24 µs [-1.11, +0.54] | -0.63 % [-1.84, +2.01] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.62 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.15 µs, sd 1.03 µs.

**cores = 2, 10,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.41-1.56, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.6 µs | 48.9 µs | 49.7 µs | 51.7 µs | 69.0 µs | 85.2 µs | 52.7 µs (sd 1.3) | 18,842 qps |
| INSERT, pk omitted | B | 42.2 µs | 48.7 µs | 49.6 µs | 51.3 µs | 67.9 µs | 82.8 µs | 52.8 µs (sd 1.7) | 18,872 qps |
| INSERT again (replicate) | A | 43.1 µs | 48.9 µs | 49.6 µs | 51.3 µs | 67.4 µs | 79.3 µs | 57.1 µs (sd 1.1) | 17,369 qps |
| INSERT again (replicate) | B | 42.6 µs | 48.7 µs | 49.5 µs | 51.1 µs | 66.8 µs | 78.9 µs | 57.1 µs (sd 1.2) | 17,326 qps |
| SELECT by pk | A | 59.7 µs | 64.8 µs | 65.7 µs | 67.9 µs | 83.0 µs | 93.8 µs | 68.2 µs (sd 0.9) | 14,565 qps |
| SELECT by pk | B | 58.8 µs | 64.9 µs | 65.7 µs | 68.1 µs | 83.4 µs | 94.5 µs | 68.4 µs (sd 1.0) | 14,522 qps |
| UPDATE by pk | A | 46.2 µs | 52.2 µs | 53.1 µs | 55.5 µs | 70.8 µs | 81.9 µs | 58.3 µs (sd 1.1) | 17,094 qps |
| UPDATE by pk | B | 47.4 µs | 52.5 µs | 53.5 µs | 55.8 µs | 71.3 µs | 82.5 µs | 58.4 µs (sd 1.3) | 17,019 qps |
| DELETE by pk | A | 42.2 µs | 48.9 µs | 49.6 µs | 51.0 µs | 65.6 µs | 73.5 µs | 51.6 µs (sd 0.4) | 19,220 qps |
| DELETE by pk | B | 42.9 µs | 48.9 µs | 49.6 µs | 51.2 µs | 66.2 µs | 74.9 µs | 51.8 µs (sd 0.5) | 19,203 qps |
| COUNT/MIN/MAX over 100 ids | A | 750.8 µs | 831.5 µs | 870.3 µs | 907.6 µs | 954.8 µs | 1,121.3 µs | 878.4 µs (sd 16.2) | 1,140 qps |
| COUNT/MIN/MAX over 100 ids | B | 715.3 µs | 772.7 µs | 798.2 µs | 823.0 µs | 859.7 µs | 1,017.5 µs | 805.1 µs (sd 4.0) | 1,242 qps |
| SHOW META (control) | A | 33.5 µs | 43.0 µs | 45.4 µs | 46.3 µs | 57.4 µs | 66.1 µs | 46.4 µs (sd 1.2) | 21,432 qps |
| SHOW META (control) | B | 33.4 µs | 43.0 µs | 45.5 µs | 46.4 µs | 57.6 µs | 66.0 µs | 46.5 µs (sd 1.4) | 21,484 qps |

| shape (cores = 2, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.30, +0.10] | +0.04 µs [-0.68, +0.88] | +0.50 % [-0.92, +1.40] | no cost resolved |
| INSERT again (replicate) | -0.10 µs [-0.30, +0.10] | -0.08 µs [-0.66, +0.50] | +0.92 % [-1.75, +1.38] | no cost resolved |
| SELECT by pk | +0.20 µs [-0.20, +0.40] | +0.18 µs [-0.26, +0.66] | -0.13 % [-0.86, +0.79] | no cost resolved |
| UPDATE by pk | +0.30 µs [-0.20, +0.90] | +0.16 µs [-0.72, +1.01] | -0.47 % [-1.65, +1.25] | no cost resolved |
| DELETE by pk | +0.00 µs [-0.10, +0.30] | +0.16 µs [-0.19, +0.56] | +0.00 % [-0.60, +0.57] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | -69.35 µs [-73.40, -65.10] | -73.35 µs [-81.63, -66.93] | +8.94 % [+8.01, +9.61] | B faster (-8.0 % at p50) |
| SHOW META (control) | -0.10 µs [-0.50, +0.80] | +0.05 µs [-0.84, +0.91] | -0.15 % [-2.46, +2.61] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -0.10 µs, sd 0.36 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.17 µs.
