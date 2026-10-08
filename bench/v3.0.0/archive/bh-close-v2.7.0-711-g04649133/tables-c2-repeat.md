
**cores = 2 repeat, 10,000 rows** - 10 runs, 30,000 statements per arm per shape pooled (loadavg before each run 1.33-1.53, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.2 µs | 48.5 µs | 49.3 µs | 50.7 µs | 66.1 µs | 77.9 µs | 51.6 µs (sd 0.8) | 19,142 qps |
| INSERT, pk omitted | B | 41.9 µs | 48.4 µs | 49.2 µs | 50.4 µs | 65.4 µs | 75.9 µs | 51.2 µs (sd 0.2) | 19,313 qps |
| INSERT again (replicate) | A | 42.2 µs | 48.5 µs | 49.2 µs | 50.5 µs | 65.2 µs | 76.1 µs | 56.4 µs (sd 1.5) | 17,659 qps |
| INSERT again (replicate) | B | 42.1 µs | 48.4 µs | 49.1 µs | 50.3 µs | 64.9 µs | 75.1 µs | 55.9 µs (sd 1.1) | 17,790 qps |
| SELECT by pk | A | 60.9 µs | 64.6 µs | 65.3 µs | 67.1 µs | 81.4 µs | 91.2 µs | 67.6 µs (sd 0.7) | 14,709 qps |
| SELECT by pk | B | 60.2 µs | 64.8 µs | 65.5 µs | 67.4 µs | 81.1 µs | 90.5 µs | 67.7 µs (sd 0.7) | 14,659 qps |
| UPDATE by pk | A | 46.9 µs | 51.9 µs | 52.8 µs | 54.8 µs | 68.8 µs | 79.1 µs | 57.7 µs (sd 1.1) | 17,190 qps |
| UPDATE by pk | B | 47.0 µs | 52.4 µs | 53.2 µs | 55.0 µs | 68.5 µs | 77.1 µs | 57.8 µs (sd 0.9) | 17,211 qps |
| DELETE by pk | A | 43.3 µs | 48.7 µs | 49.3 µs | 50.7 µs | 63.4 µs | 71.2 µs | 51.1 µs (sd 0.3) | 19,432 qps |
| DELETE by pk | B | 43.3 µs | 48.8 µs | 49.4 µs | 50.8 µs | 64.9 µs | 72.9 µs | 51.4 µs (sd 0.4) | 19,294 qps |
| COUNT/MIN/MAX over 100 ids | A | 751.4 µs | 825.8 µs | 862.2 µs | 898.9 µs | 935.7 µs | 980.3 µs | 865.2 µs (sd 4.2) | 1,154 qps |
| COUNT/MIN/MAX over 100 ids | B | 716.7 µs | 773.6 µs | 799.0 µs | 821.5 µs | 852.3 µs | 912.9 µs | 802.4 µs (sd 11.7) | 1,249 qps |
| SHOW META (control) | A | 35.4 µs | 42.7 µs | 45.2 µs | 46.1 µs | 55.5 µs | 63.7 µs | 46.1 µs (sd 1.6) | 21,773 qps |
| SHOW META (control) | B | 31.3 µs | 43.4 µs | 45.3 µs | 46.2 µs | 55.8 µs | 63.9 µs | 46.1 µs (sd 0.8) | 21,582 qps |

| shape (cores = 2 repeat, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.15 µs [-0.30, +0.20] | -0.44 µs [-1.01, -0.01] | +0.48 % [-0.14, +1.20] | no cost resolved |
| INSERT again (replicate) | -0.10 µs [-0.20, +0.00] | -0.56 µs [-1.01, -0.21] | +0.91 % [+0.22, +1.31] | no cost resolved |
| SELECT by pk | +0.15 µs [-0.15, +0.50] | +0.16 µs [-0.16, +0.48] | -0.32 % [-0.91, +0.42] | no cost resolved |
| UPDATE by pk | +0.20 µs [-0.05, +0.80] | +0.12 µs [-0.40, +0.71] | +0.11 % [-1.15, +0.89] | no cost resolved |
| DELETE by pk | +0.10 µs [+0.00, +0.15] | +0.27 µs [+0.09, +0.46] | -0.49 % [-1.05, +0.11] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | -67.35 µs [-69.95, -54.05] | -62.72 µs [-69.26, -55.46] | +8.12 % [+6.79, +8.85] | B faster (-7.8 % at p50) |
| SHOW META (control) | +0.10 µs [-0.40, +1.25] | +0.06 µs [-0.81, +0.90] | -0.58 % [-2.39, +1.98] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.27 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.16 µs.
