
### copy control, B bytes in both slots: cores = 1, 10,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.78-8.54, after 0.92-7.53; busy of the other CPUs (6, 7 excluded) in kept runs 1.1%-2.4% (median 1.5%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-4%); gate wait median 3 s; ping p50 reference 50.2 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 8,292 us, B 8,348 us, max A 21,039 us, B 14,413 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 48,000 | 38.6 µs | 48.0 µs | 48.9 µs | 50.2 µs | 61.5 µs | 75.8 µs | 88.1 µs (sd 49.2) | 17,177 /s | 28.27 µs |
| INSERT, pk omitted | B | 48,000 | 38.1 µs | 48.0 µs | 48.9 µs | 50.0 µs | 61.1 µs | 73.4 µs | 87.3 µs (sd 40.9) | 13,956 /s | 28.56 µs |
| INSERT again (replicate) | A | 48,000 | 37.8 µs | 48.0 µs | 48.8 µs | 50.2 µs | 62.4 µs | 78.1 µs | 67.5 µs (sd 34.0) | 19,262 /s | 28.30 µs |
| INSERT again (replicate) | B | 48,000 | 40.3 µs | 47.8 µs | 48.7 µs | 49.9 µs | 61.9 µs | 77.9 µs | 86.6 µs (sd 46.2) | 19,430 /s | 28.27 µs |
| INSERT, pk named (keys below 2^40) | A | 48,000 | 37.8 µs | 48.3 µs | 49.0 µs | 50.5 µs | 63.1 µs | 78.1 µs | 51.9 µs (sd 1.0) | 19,042 /s | 28.54 µs |
| INSERT, pk named (keys below 2^40) | B | 48,000 | 38.9 µs | 48.1 µs | 48.7 µs | 50.0 µs | 62.4 µs | 76.4 µs | 51.5 µs (sd 1.6) | 19,345 /s | 28.25 µs |
| SELECT by pk | A | 48,000 | 59.8 µs | 64.0 µs | 64.7 µs | 66.5 µs | 78.6 µs | 89.4 µs | 67.2 µs (sd 1.4) | 14,840 /s | 32.18 µs |
| SELECT by pk | B | 48,000 | 57.4 µs | 63.1 µs | 63.9 µs | 65.7 µs | 77.3 µs | 87.2 µs | 66.3 µs (sd 1.7) | 15,014 /s | 31.31 µs |
| UPDATE by pk | A | 48,000 | 43.2 µs | 52.1 µs | 53.0 µs | 54.8 µs | 67.6 µs | 81.7 µs | 55.9 µs (sd 1.0) | 17,617 /s | 31.38 µs |
| UPDATE by pk | B | 48,000 | 43.5 µs | 51.7 µs | 52.6 µs | 54.5 µs | 67.3 µs | 80.6 µs | 55.5 µs (sd 1.2) | 17,900 /s | 31.16 µs |
| UPDATE of one hot row | A | 48,000 | 41.7 µs | 51.4 µs | 52.2 µs | 54.3 µs | 67.1 µs | 80.4 µs | 54.5 µs (sd 1.0) | 18,277 /s | 30.89 µs |
| UPDATE of one hot row | B | 48,000 | 40.4 µs | 51.1 µs | 51.8 µs | 53.8 µs | 66.8 µs | 80.3 µs | 54.8 µs (sd 0.9) | 18,226 /s | 30.50 µs |
| DELETE by pk | A | 48,000 | 40.4 µs | 48.2 µs | 48.9 µs | 50.2 µs | 62.6 µs | 72.8 µs | 51.6 µs (sd 1.2) | 19,237 /s | 28.01 µs |
| DELETE by pk | B | 48,000 | 40.2 µs | 48.1 µs | 48.9 µs | 50.1 µs | 62.1 µs | 72.5 µs | 50.8 µs (sd 0.9) | 19,637 /s | 28.04 µs |
| COUNT/MIN/MAX over 100 ids | A | 48,000 | 760.0 µs | 816.3 µs | 836.2 µs | 857.0 µs | 889.6 µs | 1,018.2 µs | 853.6 µs (sd 13.9) | 1,167 /s | 793.88 µs |
| COUNT/MIN/MAX over 100 ids | B | 48,000 | 735.4 µs | 794.9 µs | 819.0 µs | 842.6 µs | 879.4 µs | 1,004.0 µs | 833.3 µs (sd 12.5) | 1,200 /s | 778.54 µs |
| SHOW META (control) | A | 48,000 | 43.9 µs | 49.7 µs | 50.4 µs | 53.1 µs | 62.1 µs | 70.5 µs | 52.3 µs (sd 0.6) | 19,043 /s | 32.92 µs |
| SHOW META (control) | B | 48,000 | 43.3 µs | 49.4 µs | 50.1 µs | 52.8 µs | 61.6 µs | 69.7 µs | 52.0 µs (sd 0.6) | 19,181 /s | 32.79 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 48,000 | 41.0 µs | 48.1 µs | 48.8 µs | 50.1 µs | 62.7 µs | 74.0 µs | 51.6 µs (sd 0.9) | 19,004 /s | 28.44 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 48.9 µs | 48.8 µs | 20,450 → 20,471 /s (+0.0 % [-0.3, +0.2]) | +0.00 µs [-0.10, +0.15] | -0.81 µs [-37.09, +33.54] | +0.23 µs [-0.30, +1.14] | +1.6 % [-55.8, +67.1] | 0.67 µs | no change resolved |
| INSERT again (replicate) | 48.7 µs | 48.8 µs | 20,555 → 20,513 /s (-0.1 % [-0.3, +0.2]) | +0.05 µs [-0.10, +0.15] | +19.16 µs [-14.33, +50.95] | -0.07 µs [-0.15, +0.18] | -0.9 % [-55.9, +11.9] | 0.67 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 48.9 µs | 48.8 µs | 20,450 → 20,492 /s (+0.2 % [+0.0, +0.5]) | -0.10 µs [-0.20, +0.00] | -0.41 µs [-1.33, +0.64] | -0.28 µs [-0.52, -0.05] | +1.6 % [-0.9, +2.6] | 0.67 µs | no change resolved |
| SELECT by pk | 64.7 µs | 63.9 µs | 15,456 → 15,649 /s (+1.3 % [+0.9, +1.9]) | -0.80 µs [-1.20, -0.60] | -0.87 µs [-1.22, -0.51] | -0.90 µs [-1.20, -0.42] | +1.2 % [+0.8, +2.0] | 0.67 µs | **B faster** (-1.2 % at p50) |
| UPDATE by pk | 52.9 µs | 52.6 µs | 18,904 → 18,993 /s (+0.9 % [+0.2, +1.5]) | -0.45 µs [-0.80, -0.30] | -0.37 µs [-1.01, +0.26] | -0.31 µs [-0.60, -0.12] | +0.6 % [-0.9, +2.7] | 0.67 µs | CI excludes 0, inside the floor: not resolved |
| UPDATE of one hot row | 52.0 µs | 51.8 µs | 19,212 → 19,324 /s (+0.8 % [+0.3, +1.3]) | -0.40 µs [-0.70, -0.20] | +0.30 µs [-0.16, +0.72] | -0.35 µs [-0.66, +0.11] | -0.9 % [-1.4, +0.4] | 0.67 µs | CI excludes 0, inside the floor: not resolved |
| DELETE by pk | 48.9 µs | 49.0 µs | 20,450 → 20,408 /s (+0.0 % [-0.6, +0.2]) | +0.00 µs [-0.10, +0.30] | -0.82 µs [-1.47, -0.16] | -0.07 µs [-0.26, +0.13] | +1.1 % [+0.5, +2.9] | 0.67 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 834.8 µs | 818.0 µs | 1,198 → 1,222 /s (+2.1 % [+2.0, +2.4]) | -17.20 µs [-19.50, -16.40] | -20.30 µs [-26.01, -14.71] | -18.18 µs [-21.13, -16.91] | +2.3 % [+1.8, +3.2] | 0.67 µs | **B faster** (-2.1 % at p50) |
| SHOW META (control) | 50.3 µs | 50.1 µs | 19,861 → 19,960 /s (+0.6 % [+0.0, +1.0]) | -0.30 µs [-0.50, +0.00] | -0.31 µs [-0.63, -0.01] | -0.27 µs [-0.48, -0.04] | +0.5 % [+0.0, +1.1] | 0.67 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.22 µs, sd 0.25 µs; replicate arm B (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.15 µs, sd 0.21 µs; control SHOW META Δ p50 -0.30 µs [-0.50, +0.00]; run-to-run sd of the per-run Δ p50, INSERT 0.74 µs, UPDATE 0.70 µs, SELECT 0.50 µs, ping 0.39 µs.
