
### copy control, A bytes in both slots: cores = 1, 10,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.70-1.09, after 0.77-1.34; busy of the other CPUs (6, 7 excluded) in kept runs 1.4%-1.9% (median 1.6%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-2%); gate wait median 3 s; ping p50 reference 50.2 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 9,714 us, B 9,444 us, max A 18,168 us, B 19,516 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 48,000 | 39.3 µs | 48.0 µs | 48.8 µs | 50.1 µs | 61.9 µs | 78.0 µs | 82.3 µs (sd 40.2) | 18,768 /s | 28.26 µs |
| INSERT, pk omitted | B | 48,000 | 36.3 µs | 48.1 µs | 48.9 µs | 50.2 µs | 61.6 µs | 73.7 µs | 100.4 µs (sd 48.5) | 9,441 /s | 28.66 µs |
| INSERT again (replicate) | A | 48,000 | 40.3 µs | 48.1 µs | 48.7 µs | 50.1 µs | 62.8 µs | 77.9 µs | 54.9 µs (sd 15.8) | 19,415 /s | 28.21 µs |
| INSERT again (replicate) | B | 48,000 | 40.8 µs | 48.0 µs | 48.8 µs | 50.1 µs | 62.8 µs | 77.5 µs | 89.7 µs (sd 32.5) | 9,839 /s | 28.53 µs |
| INSERT, pk named (keys below 2^40) | A | 48,000 | 40.7 µs | 48.3 µs | 49.0 µs | 50.8 µs | 63.8 µs | 78.6 µs | 52.0 µs (sd 0.7) | 18,964 /s | 28.79 µs |
| INSERT, pk named (keys below 2^40) | B | 48,000 | 39.5 µs | 48.2 µs | 48.9 µs | 50.5 µs | 63.4 µs | 77.4 µs | 52.1 µs (sd 1.9) | 19,031 /s | 28.59 µs |
| SELECT by pk | A | 48,000 | 56.6 µs | 64.3 µs | 65.2 µs | 67.2 µs | 79.0 µs | 88.7 µs | 67.4 µs (sd 0.7) | 14,716 /s | 32.46 µs |
| SELECT by pk | B | 48,000 | 57.7 µs | 64.2 µs | 64.9 µs | 67.0 µs | 78.7 µs | 88.8 µs | 67.2 µs (sd 0.6) | 14,800 /s | 32.15 µs |
| UPDATE by pk | A | 48,000 | 43.7 µs | 52.3 µs | 53.3 µs | 55.5 µs | 68.0 µs | 81.3 µs | 56.1 µs (sd 0.9) | 17,694 /s | 31.58 µs |
| UPDATE by pk | B | 48,000 | 42.5 µs | 52.2 µs | 53.1 µs | 55.1 µs | 67.7 µs | 80.4 µs | 55.8 µs (sd 0.9) | 17,789 /s | 31.36 µs |
| UPDATE of one hot row | A | 48,000 | 39.3 µs | 51.5 µs | 52.3 µs | 54.2 µs | 67.0 µs | 79.1 µs | 54.5 µs (sd 0.5) | 18,244 /s | 30.90 µs |
| UPDATE of one hot row | B | 48,000 | 43.6 µs | 51.5 µs | 52.4 µs | 54.4 µs | 67.0 µs | 79.4 µs | 55.1 µs (sd 0.9) | 18,004 /s | 30.98 µs |
| DELETE by pk | A | 48,000 | 40.1 µs | 48.3 µs | 48.9 µs | 50.2 µs | 62.7 µs | 72.6 µs | 51.2 µs (sd 0.8) | 19,444 /s | 28.15 µs |
| DELETE by pk | B | 48,000 | 40.4 µs | 48.2 µs | 48.9 µs | 50.1 µs | 62.2 µs | 72.8 µs | 51.3 µs (sd 1.3) | 19,532 /s | 28.00 µs |
| COUNT/MIN/MAX over 100 ids | A | 48,000 | 722.8 µs | 775.5 µs | 799.2 µs | 823.4 µs | 853.9 µs | 1,037.0 µs | 818.1 µs (sd 10.3) | 1,216 /s | 760.83 µs |
| COUNT/MIN/MAX over 100 ids | B | 48,000 | 723.0 µs | 779.4 µs | 805.4 µs | 831.3 µs | 893.1 µs | 997.9 µs | 825.4 µs (sd 23.7) | 1,219 /s | 759.83 µs |
| SHOW META (control) | A | 48,000 | 42.6 µs | 49.5 µs | 50.1 µs | 52.7 µs | 62.1 µs | 70.3 µs | 52.0 µs (sd 0.4) | 19,120 /s | 32.89 µs |
| SHOW META (control) | B | 48,000 | 43.1 µs | 49.5 µs | 50.2 µs | 52.9 µs | 62.1 µs | 69.4 µs | 52.0 µs (sd 0.3) | 19,125 /s | 32.86 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 48,000 | 40.8 µs | 48.3 µs | 49.0 µs | 50.5 µs | 63.5 µs | 75.8 µs | 51.6 µs (sd 0.8) | 19,102 /s | 28.49 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 48.8 µs | 48.8 µs | 20,492 → 20,471 /s (-0.2 % [-0.4, +0.2]) | +0.10 µs [-0.10, +0.20] | +18.08 µs [-19.10, +55.80] | +0.70 µs [-0.52, +0.92] | -32.2 % [-55.0, +101.4] | 0.67 µs | no change resolved |
| INSERT again (replicate) | 48.8 µs | 48.8 µs | 20,492 → 20,513 /s (-0.1 % [-0.4, +0.2]) | +0.05 µs [-0.10, +0.20] | +34.78 µs [+14.52, +53.06] | +0.07 µs [-0.06, +0.46] | -49.6 % [-55.9, -0.3] | 0.67 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.0 µs | 48.8 µs | 20,408 → 20,492 /s (+0.1 % [-0.1, +0.6]) | -0.05 µs [-0.30, +0.05] | +0.06 µs [-0.74, +1.24] | -0.10 µs [-0.40, +0.06] | +0.8 % [-0.3, +2.0] | 0.67 µs | no change resolved |
| SELECT by pk | 65.0 µs | 64.9 µs | 15,396 → 15,408 /s (+0.3 % [-0.2, +0.8]) | -0.20 µs [-0.50, +0.10] | -0.14 µs [-0.49, +0.21] | -0.12 µs [-0.68, +0.07] | -0.1 % [-0.3, +1.2] | 0.67 µs | no change resolved |
| UPDATE by pk | 53.1 µs | 53.0 µs | 18,832 → 18,850 /s (+0.0 % [-0.6, +0.6]) | +0.00 µs [-0.30, +0.30] | -0.29 µs [-0.79, +0.22] | -0.09 µs [-0.27, +0.07] | -0.1 % [-0.4, +2.1] | 0.67 µs | no change resolved |
| UPDATE of one hot row | 52.0 µs | 52.4 µs | 19,231 → 19,084 /s (-0.3 % [-0.9, +0.4]) | +0.15 µs [-0.20, +0.45] | +0.56 µs [+0.22, +0.86] | +0.07 µs [-0.02, +0.21] | -0.9 % [-1.9, -0.5] | 0.67 µs | no change resolved |
| DELETE by pk | 49.0 µs | 48.9 µs | 20,429 → 20,450 /s (-0.1 % [-0.2, +0.4]) | +0.05 µs [-0.20, +0.10] | +0.09 µs [-0.56, +0.81] | -0.06 µs [-0.37, +0.08] | +0.1 % [-0.7, +1.5] | 0.67 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 797.8 µs | 800.9 µs | 1,253 → 1,249 /s (+0.0 % [-1.0, +0.3]) | -0.05 µs [-2.70, +8.20] | +7.24 µs [-3.33, +20.31] | -1.05 µs [-4.90, +6.80] | -0.1 % [-2.0, +0.8] | 0.67 µs | no change resolved |
| SHOW META (control) | 50.1 µs | 50.2 µs | 19,960 → 19,920 /s (-0.4 % [-0.4, +0.2]) | +0.20 µs [-0.10, +0.20] | -0.00 µs [-0.15, +0.14] | -0.04 µs [-0.14, +0.11] | +0.0 % [-0.5, +0.5] | 0.67 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.15 µs, sd 0.16 µs; replicate arm B (INSERT again − INSERT p50 per run) median -0.05 µs, robust sd 0.22 µs, sd 0.21 µs; control SHOW META Δ p50 +0.20 µs [-0.10, +0.20]; run-to-run sd of the per-run Δ p50, INSERT 0.24 µs, UPDATE 0.80 µs, SELECT 0.46 µs, ping 0.20 µs.
