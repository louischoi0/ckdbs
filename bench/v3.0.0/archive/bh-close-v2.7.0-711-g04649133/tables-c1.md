
**cores = 1, 200 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 0.94-0.97, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 41.4 µs | 48.7 µs | 49.7 µs | 52.6 µs | 69.8 µs | 93.1 µs | 54.7 µs (sd 5.1) | 18,803 qps |
| INSERT, pk omitted | B | 35.7 µs | 48.7 µs | 49.6 µs | 51.7 µs | 68.4 µs | 85.6 µs | 52.5 µs (sd 2.5) | 19,018 qps |
| INSERT again (replicate) | A | 39.4 µs | 48.7 µs | 49.7 µs | 52.1 µs | 68.2 µs | 84.4 µs | 55.2 µs (sd 3.4) | 18,202 qps |
| INSERT again (replicate) | B | 40.9 µs | 48.7 µs | 49.6 µs | 51.6 µs | 67.5 µs | 81.6 µs | 54.6 µs (sd 1.5) | 18,314 qps |
| SELECT by pk | A | 57.7 µs | 64.6 µs | 66.1 µs | 72.3 µs | 87.0 µs | 110.3 µs | 71.7 µs (sd 5.1) | 14,195 qps |
| SELECT by pk | B | 58.1 µs | 64.0 µs | 65.6 µs | 71.6 µs | 86.0 µs | 103.5 µs | 70.3 µs (sd 4.1) | 14,429 qps |
| UPDATE by pk | A | 39.7 µs | 50.7 µs | 51.9 µs | 55.8 µs | 70.9 µs | 85.5 µs | 57.6 µs (sd 1.1) | 17,313 qps |
| UPDATE by pk | B | 39.0 µs | 50.2 µs | 51.7 µs | 55.0 µs | 70.2 µs | 83.5 µs | 57.3 µs (sd 1.2) | 17,443 qps |
| DELETE by pk | A | 40.3 µs | 48.7 µs | 49.6 µs | 52.0 µs | 66.9 µs | 77.9 µs | 54.7 µs (sd 1.3) | 18,317 qps |
| DELETE by pk | B | 40.4 µs | 48.6 µs | 49.6 µs | 51.9 µs | 67.5 µs | 82.2 µs | 55.9 µs (sd 5.6) | 18,462 qps |
| COUNT/MIN/MAX over 100 ids | A | 79.6 µs | 85.8 µs | 87.7 µs | 94.2 µs | 110.3 µs | 128.3 µs | 93.3 µs (sd 4.4) | 10,796 qps |
| COUNT/MIN/MAX over 100 ids | B | 81.0 µs | 86.6 µs | 88.5 µs | 95.4 µs | 111.0 µs | 124.4 µs | 93.0 µs (sd 1.7) | 10,736 qps |
| SHOW META (control) | A | 33.1 µs | 44.9 µs | 45.6 µs | 47.4 µs | 60.6 µs | 72.1 µs | 48.0 µs (sd 2.9) | 21,090 qps |
| SHOW META (control) | B | 31.7 µs | 44.8 µs | 45.5 µs | 47.4 µs | 59.7 µs | 69.2 µs | 47.7 µs (sd 1.9) | 21,142 qps |

| shape (cores = 1, 200 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.15 µs [-0.30, +0.00] | -2.16 µs [-4.72, -0.47] | +1.35 % [+0.46, +1.80] | no cost resolved |
| INSERT again (replicate) | -0.10 µs [-0.25, +0.05] | -0.62 µs [-2.04, +0.38] | +0.34 % [-1.08, +1.61] | no cost resolved |
| SELECT by pk | -0.55 µs [-0.75, -0.20] | -1.46 µs [-4.54, +1.47] | +0.94 % [+0.59, +1.96] | no cost resolved |
| UPDATE by pk | -0.10 µs [-0.70, +0.10] | -0.36 µs [-0.89, +0.26] | +0.47 % [+0.00, +1.89] | no cost resolved |
| DELETE by pk | +0.00 µs [-0.20, +0.10] | +1.23 µs [-1.02, +4.56] | +0.63 % [-0.67, +1.90] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | +1.00 µs [+0.70, +1.20] | -0.26 µs [-2.27, +0.99] | -0.90 % [-1.08, +0.08] | **B slower** (+1.1 % at p50) |
| SHOW META (control) | -0.10 µs [-0.20, +0.00] | -0.25 µs [-0.96, +0.40] | +0.41 % [-0.24, +1.01] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -0.10 µs, sd 1.12 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -0.05 µs, sd 0.32 µs.

**cores = 1, 1,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 0.93-0.96, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 40.5 µs | 48.8 µs | 49.7 µs | 52.4 µs | 68.1 µs | 83.1 µs | 52.6 µs (sd 0.7) | 18,814 qps |
| INSERT, pk omitted | B | 35.9 µs | 48.7 µs | 49.6 µs | 52.1 µs | 67.6 µs | 82.7 µs | 53.1 µs (sd 4.1) | 18,909 qps |
| INSERT again (replicate) | A | 41.3 µs | 48.7 µs | 49.6 µs | 52.0 µs | 67.8 µs | 84.0 µs | 55.6 µs (sd 4.1) | 18,193 qps |
| INSERT again (replicate) | B | 40.7 µs | 48.7 µs | 49.5 µs | 51.6 µs | 67.6 µs | 81.4 µs | 54.9 µs (sd 1.4) | 18,190 qps |
| SELECT by pk | A | 58.6 µs | 65.3 µs | 69.4 µs | 75.1 µs | 92.8 µs | 116.8 µs | 73.9 µs (sd 4.3) | 13,692 qps |
| SELECT by pk | B | 54.3 µs | 64.8 µs | 68.8 µs | 74.7 µs | 91.7 µs | 113.3 µs | 73.2 µs (sd 3.5) | 13,849 qps |
| UPDATE by pk | A | 41.9 µs | 50.7 µs | 52.0 µs | 56.1 µs | 71.8 µs | 89.2 µs | 61.3 µs (sd 4.5) | 16,516 qps |
| UPDATE by pk | B | 39.4 µs | 50.9 µs | 52.0 µs | 55.8 µs | 71.1 µs | 85.7 µs | 60.6 µs (sd 2.1) | 16,432 qps |
| DELETE by pk | A | 40.7 µs | 48.8 µs | 49.7 µs | 53.1 µs | 68.2 µs | 82.2 µs | 56.7 µs (sd 4.7) | 18,036 qps |
| DELETE by pk | B | 41.1 µs | 48.6 µs | 49.5 µs | 52.0 µs | 67.1 µs | 78.8 µs | 54.3 µs (sd 0.9) | 18,319 qps |
| COUNT/MIN/MAX over 100 ids | A | 134.5 µs | 143.4 µs | 148.3 µs | 160.9 µs | 183.1 µs | 224.4 µs | 156.4 µs (sd 7.2) | 6,480 qps |
| COUNT/MIN/MAX over 100 ids | B | 141.8 µs | 152.7 µs | 158.7 µs | 170.8 µs | 189.9 µs | 229.4 µs | 165.8 µs (sd 5.2) | 6,072 qps |
| SHOW META (control) | A | 31.1 µs | 45.0 µs | 45.6 µs | 47.1 µs | 60.0 µs | 69.1 µs | 47.6 µs (sd 1.0) | 20,983 qps |
| SHOW META (control) | B | 31.8 µs | 44.8 µs | 45.4 µs | 46.8 µs | 59.6 µs | 69.8 µs | 48.1 µs (sd 4.3) | 21,176 qps |

| shape (cores = 1, 1,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.20, +0.00] | +0.49 µs [-0.76, +2.68] | +0.76 % [+0.09, +1.15] | no cost resolved |
| INSERT again (replicate) | -0.05 µs [-0.20, +0.00] | -0.78 µs [-2.82, +0.61] | +0.33 % [-0.95, +1.02] | no cost resolved |
| SELECT by pk | -0.60 µs [-0.95, +0.05] | -0.73 µs [-3.06, +1.10] | +0.48 % [-0.25, +1.52] | no cost resolved |
| UPDATE by pk | +0.00 µs [-0.60, +0.40] | -0.68 µs [-2.48, +0.66] | -0.09 % [-1.24, +1.19] | no cost resolved |
| DELETE by pk | -0.25 µs [-0.40, +0.00] | -2.34 µs [-4.86, -0.48] | +1.11 % [-0.15, +4.89] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | +10.45 µs [+9.80, +11.30] | +9.44 µs [+4.22, +14.26] | -5.96 % [-7.07, -5.30] | **B slower** (+7.1 % at p50) |
| SHOW META (control) | -0.25 µs [-0.30, -0.10] | +0.47 µs [-0.86, +2.73] | +0.51 % [-0.36, +2.26] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.23 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -0.05 µs, sd 0.25 µs.

**cores = 1, 10,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 0.85-1.00, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 41.2 µs | 48.8 µs | 49.7 µs | 52.7 µs | 68.8 µs | 86.9 µs | 53.7 µs (sd 3.7) | 18,772 qps |
| INSERT, pk omitted | B | 41.0 µs | 48.7 µs | 49.6 µs | 51.9 µs | 67.1 µs | 81.9 µs | 52.2 µs (sd 0.6) | 18,985 qps |
| INSERT again (replicate) | A | 41.0 µs | 48.6 µs | 49.7 µs | 52.7 µs | 68.8 µs | 87.0 µs | 57.5 µs (sd 3.3) | 17,689 qps |
| INSERT again (replicate) | B | 40.6 µs | 48.7 µs | 49.6 µs | 52.0 µs | 67.8 µs | 84.5 µs | 57.0 µs (sd 2.6) | 17,662 qps |
| SELECT by pk | A | 57.9 µs | 64.0 µs | 65.3 µs | 70.5 µs | 86.1 µs | 102.7 µs | 69.6 µs (sd 3.0) | 14,483 qps |
| SELECT by pk | B | 57.8 µs | 63.9 µs | 65.2 µs | 69.7 µs | 84.7 µs | 102.4 µs | 69.3 µs (sd 3.2) | 14,549 qps |
| UPDATE by pk | A | 40.1 µs | 51.3 µs | 52.3 µs | 56.3 µs | 72.9 µs | 90.5 µs | 58.3 µs (sd 1.8) | 17,198 qps |
| UPDATE by pk | B | 38.9 µs | 51.3 µs | 52.2 µs | 56.0 µs | 71.9 µs | 89.3 µs | 58.9 µs (sd 4.0) | 17,168 qps |
| DELETE by pk | A | 40.8 µs | 48.6 µs | 49.7 µs | 52.7 µs | 68.9 µs | 81.1 µs | 52.8 µs (sd 2.1) | 19,007 qps |
| DELETE by pk | B | 40.9 µs | 48.6 µs | 49.6 µs | 52.0 µs | 67.4 µs | 80.4 µs | 53.2 µs (sd 2.6) | 19,150 qps |
| COUNT/MIN/MAX over 100 ids | A | 712.9 µs | 786.9 µs | 814.0 µs | 843.2 µs | 1,026.1 µs | 1,132.9 µs | 839.4 µs (sd 49.7) | 1,205 qps |
| COUNT/MIN/MAX over 100 ids | B | 798.1 µs | 886.2 µs | 918.2 µs | 951.1 µs | 1,005.4 µs | 1,135.4 µs | 929.4 µs (sd 8.6) | 1,075 qps |
| SHOW META (control) | A | 31.8 µs | 45.0 µs | 45.7 µs | 47.0 µs | 59.9 µs | 69.5 µs | 48.5 µs (sd 4.1) | 21,001 qps |
| SHOW META (control) | B | 31.3 µs | 44.8 µs | 45.5 µs | 46.6 µs | 58.8 µs | 67.3 µs | 46.9 µs (sd 0.6) | 21,184 qps |

| shape (cores = 1, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.20 µs [-0.20, +0.00] | -1.57 µs [-3.54, -0.47] | +0.99 % [+0.75, +1.28] | no cost resolved |
| INSERT again (replicate) | -0.05 µs [-0.20, +0.10] | -0.48 µs [-1.89, +0.86] | -0.18 % [-0.49, +1.49] | no cost resolved |
| SELECT by pk | -0.25 µs [-0.60, +0.00] | -0.34 µs [-1.34, +0.64] | +0.23 % [-0.55, +1.62] | no cost resolved |
| UPDATE by pk | -0.15 µs [-0.50, +0.10] | +0.59 µs [-0.92, +2.96] | +1.37 % [-0.10, +1.91] | no cost resolved |
| DELETE by pk | -0.10 µs [-0.20, +0.00] | +0.32 µs [-0.82, +1.73] | +0.53 % [-0.02, +0.71] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | +107.85 µs [+101.55, +111.90] | +90.06 µs [+62.82, +106.89] | -10.43 % [-11.54, -10.14] | **B slower** (+13.3 % at p50) |
| SHOW META (control) | -0.20 µs [-0.30, -0.10] | -1.56 µs [-3.67, -0.32] | +0.95 % [+0.54, +1.64] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -0.10 µs, sd 0.44 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.28 µs.
