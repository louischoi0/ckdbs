
**cores = 1, 200 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.28-1.60, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 41.0 µs | 48.4 µs | 49.4 µs | 50.3 µs | 63.3 µs | 77.4 µs | 51.1 µs (sd 0.8) | 19,279 qps |
| INSERT, pk omitted | B | 40.5 µs | 48.3 µs | 49.3 µs | 50.2 µs | 62.5 µs | 74.9 µs | 50.7 µs (sd 0.6) | 19,506 qps |
| INSERT again (replicate) | A | 40.4 µs | 48.4 µs | 49.3 µs | 50.3 µs | 63.1 µs | 76.3 µs | 52.7 µs (sd 0.7) | 18,682 qps |
| INSERT again (replicate) | B | 40.8 µs | 48.3 µs | 49.2 µs | 50.1 µs | 62.8 µs | 74.9 µs | 53.1 µs (sd 1.4) | 18,716 qps |
| SELECT by pk | A | 57.7 µs | 63.9 µs | 64.9 µs | 68.1 µs | 80.7 µs | 94.8 µs | 67.5 µs (sd 0.6) | 14,680 qps |
| SELECT by pk | B | 57.3 µs | 64.2 µs | 65.2 µs | 68.4 µs | 80.9 µs | 95.1 µs | 67.8 µs (sd 0.7) | 14,571 qps |
| UPDATE by pk | A | 40.0 µs | 50.4 µs | 51.5 µs | 53.0 µs | 66.3 µs | 79.2 µs | 56.0 µs (sd 1.4) | 17,767 qps |
| UPDATE by pk | B | 39.9 µs | 49.9 µs | 51.3 µs | 52.7 µs | 65.6 µs | 77.3 µs | 56.0 µs (sd 1.1) | 17,700 qps |
| DELETE by pk | A | 40.9 µs | 48.4 µs | 49.3 µs | 50.3 µs | 62.4 µs | 71.6 µs | 52.8 µs (sd 0.8) | 18,813 qps |
| DELETE by pk | B | 38.8 µs | 48.4 µs | 49.2 µs | 50.2 µs | 61.8 µs | 71.2 µs | 52.5 µs (sd 1.0) | 19,030 qps |
| bulk INSERT, 20 rows ascending | A | 88.0 µs | 93.6 µs | 95.5 µs | 105.8 µs | 121.5 µs | 166.9 µs | 104.1 µs (sd 1.4) | 9,088 qps |
| bulk INSERT, 20 rows ascending | B | 88.7 µs | 92.7 µs | 94.6 µs | 105.4 µs | 120.7 µs | 165.1 µs | 104.1 µs (sd 2.0) | 9,088 qps |
| COUNT/MIN/MAX over 100 ids | A | 80.8 µs | 85.9 µs | 86.9 µs | 89.5 µs | 103.0 µs | 113.1 µs | 89.5 µs (sd 1.1) | 11,094 qps |
| COUNT/MIN/MAX over 100 ids | B | 81.2 µs | 85.6 µs | 86.6 µs | 89.1 µs | 102.6 µs | 112.4 µs | 89.1 µs (sd 0.9) | 11,124 qps |
| SHOW META (control) | A | 34.6 µs | 44.7 µs | 45.4 µs | 46.1 µs | 54.6 µs | 62.0 µs | 46.1 µs (sd 0.5) | 21,527 qps |
| SHOW META (control) | B | 31.3 µs | 44.6 µs | 45.4 µs | 46.0 µs | 54.3 µs | 61.5 µs | 46.0 µs (sd 0.6) | 21,591 qps |

| shape (cores = 1, 200 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.20, +0.00] | -0.39 µs [-0.67, -0.16] | +0.39 % [+0.15, +1.21] | no cost resolved |
| INSERT again (replicate) | -0.10 µs [-0.20, +0.00] | +0.34 µs [-0.21, +1.04] | +0.27 % [-1.36, +0.70] | no cost resolved |
| SELECT by pk | +0.30 µs [-0.05, +0.45] | +0.26 µs [+0.07, +0.49] | -0.20 % [-0.63, -0.06] | no cost resolved |
| UPDATE by pk | -0.20 µs [-0.50, +0.10] | -0.05 µs [-0.83, +0.64] | -0.12 % [-1.14, +1.22] | no cost resolved |
| DELETE by pk | -0.20 µs [-0.30, +0.10] | -0.23 µs [-0.73, +0.26] | +0.35 % [-0.34, +1.12] | no cost resolved |
| bulk INSERT, 20 rows ascending | -0.95 µs [-1.20, -0.75] | +0.01 µs [-0.95, +1.02] | +0.36 % [-1.19, +0.96] | B faster (-1.0 % at p50) |
| COUNT/MIN/MAX over 100 ids | -0.40 µs [-0.70, -0.20] | -0.41 µs [-0.81, -0.02] | +0.55 % [+0.01, +0.93] | no cost resolved |
| SHOW META (control) | -0.10 µs [-0.20, +0.05] | -0.09 µs [-0.31, +0.14] | +0.19 % [-0.48, +0.90] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -0.10 µs, sd 0.13 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.13 µs.

**cores = 1, 1,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.42-1.60, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 41.2 µs | 48.5 µs | 49.4 µs | 50.3 µs | 62.0 µs | 76.9 µs | 50.9 µs (sd 0.4) | 19,338 qps |
| INSERT, pk omitted | B | 40.7 µs | 48.5 µs | 49.3 µs | 50.2 µs | 61.4 µs | 75.5 µs | 50.7 µs (sd 0.4) | 19,473 qps |
| INSERT again (replicate) | A | 40.8 µs | 48.6 µs | 49.4 µs | 50.2 µs | 61.9 µs | 75.7 µs | 53.5 µs (sd 1.0) | 18,565 qps |
| INSERT again (replicate) | B | 40.4 µs | 48.5 µs | 49.3 µs | 50.2 µs | 61.6 µs | 73.9 µs | 52.8 µs (sd 0.7) | 18,753 qps |
| SELECT by pk | A | 58.5 µs | 64.4 µs | 66.7 µs | 72.4 µs | 83.5 µs | 98.2 µs | 69.7 µs (sd 0.8) | 14,256 qps |
| SELECT by pk | B | 57.8 µs | 64.4 µs | 66.6 µs | 72.3 µs | 83.0 µs | 95.6 µs | 69.4 µs (sd 0.6) | 14,328 qps |
| UPDATE by pk | A | 40.2 µs | 50.7 µs | 51.6 µs | 53.0 µs | 64.9 µs | 77.6 µs | 58.4 µs (sd 1.7) | 17,083 qps |
| UPDATE by pk | B | 39.7 µs | 49.7 µs | 51.2 µs | 52.5 µs | 64.5 µs | 76.5 µs | 57.5 µs (sd 1.3) | 17,141 qps |
| DELETE by pk | A | 40.8 µs | 48.4 µs | 49.2 µs | 50.2 µs | 60.7 µs | 69.9 µs | 52.4 µs (sd 0.5) | 18,996 qps |
| DELETE by pk | B | 37.4 µs | 48.5 µs | 49.3 µs | 50.2 µs | 60.8 µs | 70.2 µs | 53.2 µs (sd 1.5) | 18,852 qps |
| bulk INSERT, 20 rows ascending | A | 87.5 µs | 93.1 µs | 94.8 µs | 104.7 µs | 119.4 µs | 161.7 µs | 104.0 µs (sd 1.9) | 9,128 qps |
| bulk INSERT, 20 rows ascending | B | 89.0 µs | 92.4 µs | 94.3 µs | 104.5 µs | 118.9 µs | 162.2 µs | 102.6 µs (sd 1.1) | 9,252 qps |
| COUNT/MIN/MAX over 100 ids | A | 134.7 µs | 141.5 µs | 144.4 µs | 150.2 µs | 162.0 µs | 174.6 µs | 147.0 µs (sd 0.9) | 6,786 qps |
| COUNT/MIN/MAX over 100 ids | B | 136.1 µs | 142.9 µs | 145.8 µs | 151.2 µs | 163.4 µs | 176.7 µs | 148.4 µs (sd 1.0) | 6,712 qps |
| SHOW META (control) | A | 32.1 µs | 44.8 µs | 45.5 µs | 46.1 µs | 53.1 µs | 60.2 µs | 46.0 µs (sd 0.3) | 21,634 qps |
| SHOW META (control) | B | 31.2 µs | 44.5 µs | 45.3 µs | 45.9 µs | 53.3 µs | 60.3 µs | 45.8 µs (sd 0.4) | 21,705 qps |

| shape (cores = 1, 1,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.20, +0.10] | -0.18 µs [-0.32, -0.04] | +0.25 % [-0.05, +0.79] | no cost resolved |
| INSERT again (replicate) | -0.05 µs [-0.20, +0.10] | -0.67 µs [-1.08, -0.27] | +1.01 % [+0.48, +2.61] | no cost resolved |
| SELECT by pk | -0.15 µs [-0.80, +0.40] | -0.31 µs [-0.71, +0.07] | +0.59 % [-0.52, +1.15] | no cost resolved |
| UPDATE by pk | -0.35 µs [-1.00, +0.00] | -0.89 µs [-1.63, -0.14] | +2.62 % [+0.10, +2.99] | no cost resolved |
| DELETE by pk | +0.10 µs [-0.10, +0.20] | +0.74 µs [+0.01, +1.58] | -0.72 % [-1.87, +0.73] | no cost resolved |
| bulk INSERT, 20 rows ascending | -0.85 µs [-1.15, -0.20] | -1.40 µs [-2.66, -0.28] | +0.78 % [+0.47, +1.06] | B faster (-0.9 % at p50) |
| COUNT/MIN/MAX over 100 ids | +1.30 µs [+1.00, +2.00] | +1.36 µs [+0.98, +1.72] | -0.85 % [-1.31, -0.73] | **B slower** (+0.9 % at p50) |
| SHOW META (control) | -0.10 µs [-0.25, +0.00] | -0.17 µs [-0.41, +0.06] | +0.17 % [-0.48, +1.02] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.10 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.14 µs.

**cores = 1, 10,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.36-1.58, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 41.2 µs | 48.4 µs | 49.2 µs | 50.2 µs | 61.6 µs | 77.2 µs | 50.8 µs (sd 0.3) | 19,467 qps |
| INSERT, pk omitted | B | 40.9 µs | 48.3 µs | 49.2 µs | 50.0 µs | 60.9 µs | 74.2 µs | 50.5 µs (sd 0.3) | 19,539 qps |
| INSERT again (replicate) | A | 40.2 µs | 48.4 µs | 49.2 µs | 50.1 µs | 62.0 µs | 76.5 µs | 54.7 µs (sd 1.1) | 18,160 qps |
| INSERT again (replicate) | B | 40.9 µs | 48.3 µs | 49.2 µs | 50.0 µs | 61.2 µs | 75.0 µs | 54.6 µs (sd 1.4) | 18,159 qps |
| SELECT by pk | A | 58.9 µs | 63.8 µs | 64.6 µs | 66.0 µs | 76.8 µs | 84.6 µs | 66.2 µs (sd 0.6) | 14,962 qps |
| SELECT by pk | B | 58.1 µs | 63.4 µs | 64.3 µs | 65.7 µs | 76.0 µs | 83.2 µs | 65.8 µs (sd 0.5) | 15,062 qps |
| UPDATE by pk | A | 43.7 µs | 51.4 µs | 52.1 µs | 53.7 µs | 66.5 µs | 81.9 µs | 56.5 µs (sd 1.2) | 17,648 qps |
| UPDATE by pk | B | 41.1 µs | 50.8 µs | 51.6 µs | 52.9 µs | 64.7 µs | 76.5 µs | 56.5 µs (sd 2.7) | 17,884 qps |
| DELETE by pk | A | 40.4 µs | 48.5 µs | 49.2 µs | 50.3 µs | 61.6 µs | 71.0 µs | 50.8 µs (sd 0.4) | 19,605 qps |
| DELETE by pk | B | 40.6 µs | 48.4 µs | 49.3 µs | 50.2 µs | 61.0 µs | 70.0 µs | 50.5 µs (sd 0.4) | 19,681 qps |
| bulk INSERT, 20 rows ascending | A | 89.3 µs | 94.1 µs | 96.2 µs | 106.0 µs | 120.8 µs | 161.2 µs | 105.6 µs (sd 2.0) | 8,988 qps |
| bulk INSERT, 20 rows ascending | B | 86.5 µs | 93.1 µs | 94.9 µs | 105.6 µs | 120.5 µs | 164.0 µs | 104.1 µs (sd 1.5) | 9,078 qps |
| COUNT/MIN/MAX over 100 ids | A | 712.0 µs | 768.0 µs | 792.0 µs | 815.8 µs | 847.6 µs | 899.4 µs | 797.2 µs (sd 9.8) | 1,255 qps |
| COUNT/MIN/MAX over 100 ids | B | 726.9 µs | 781.5 µs | 806.0 µs | 831.0 µs | 863.6 µs | 893.6 µs | 812.9 µs (sd 10.7) | 1,235 qps |
| SHOW META (control) | A | 36.6 µs | 44.8 µs | 45.4 µs | 46.0 µs | 52.9 µs | 59.8 µs | 45.9 µs (sd 0.4) | 21,689 qps |
| SHOW META (control) | B | 31.1 µs | 44.7 µs | 45.4 µs | 46.0 µs | 52.6 µs | 59.3 µs | 45.8 µs (sd 0.4) | 21,728 qps |

| shape (cores = 1, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | +0.00 µs [-0.10, +0.15] | -0.26 µs [-0.38, -0.14] | +0.56 % [+0.17, +0.85] | no cost resolved |
| INSERT again (replicate) | +0.05 µs [-0.10, +0.10] | -0.09 µs [-0.56, +0.42] | +1.12 % [-1.83, +1.36] | no cost resolved |
| SELECT by pk | -0.15 µs [-0.70, +0.10] | -0.44 µs [-0.78, -0.11] | +0.39 % [+0.06, +1.01] | no cost resolved |
| UPDATE by pk | -0.55 µs [-0.70, -0.20] | -0.05 µs [-1.22, +1.37] | +1.77 % [-0.71, +2.62] | no cost resolved |
| DELETE by pk | -0.05 µs [-0.10, +0.20] | -0.22 µs [-0.31, -0.13] | +0.50 % [+0.25, +0.60] | no cost resolved |
| bulk INSERT, 20 rows ascending | -0.70 µs [-1.30, -0.35] | -1.52 µs [-2.64, -0.46] | +0.99 % [-0.11, +3.07] | B faster (-0.7 % at p50) |
| COUNT/MIN/MAX over 100 ids | +14.05 µs [+10.45, +21.20] | +15.64 µs [+8.91, +21.88] | -1.98 % [-3.25, -1.16] | **B slower** (+1.8 % at p50) |
| SHOW META (control) | +0.00 µs [-0.15, +0.10] | -0.06 µs [-0.22, +0.10] | +0.08 % [-0.38, +0.63] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.10 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.17 µs.
