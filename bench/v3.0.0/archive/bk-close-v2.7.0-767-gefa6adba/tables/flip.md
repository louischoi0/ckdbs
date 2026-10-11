
### flipped pair, cores = 1, 1,000 rows

13 runs kept of 13 (runs 0-14); loadavg(1 min) before each run 0.73-2.79, after 0.76-3.05; busy of the other CPUs (6, 7 excluded) in kept runs 1.6%-4.9% (median 2.3%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-4%); gate wait median 3 s; ping p50 reference 50.3 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 7,913 us, B 7,898 us, max A 458,462 us, B 301,699 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.3 µs | 49.3 µs | 20,284 → 20,284 /s (+0.2 % [-1.0, +0.6]) | -0.10 µs [-0.30, +0.50] | +5.95 µs [-0.45, +17.40] | +0.12 µs [-0.19, +0.56] | -0.3 % [-2.4, +1.0] | 0.89 µs | no change resolved |
| INSERT again (replicate) | 49.3 µs | 48.9 µs | 20,284 → 20,450 /s (+0.2 % [-0.2, +0.6]) | -0.10 µs [-0.30, +0.10] | -0.71 µs [-1.93, +0.38] | +0.14 µs [-0.13, +0.43] | +0.6 % [-1.0, +3.1] | 0.89 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.4 µs | 49.3 µs | 20,243 → 20,284 /s (+0.2 % [-0.4, +0.6]) | -0.10 µs [-0.30, +0.20] | +0.28 µs [-0.72, +1.48] | +0.19 µs [-0.12, +0.60] | +0.4 % [-1.7, +1.2] | 0.89 µs | no change resolved |
| SELECT by pk | 73.3 µs | 72.3 µs | 13,643 → 13,831 /s (+0.5 % [+0.0, +1.7]) | -0.40 µs [-1.30, +0.00] | +2.12 µs [-0.22, +5.86] | +0.07 µs [-0.26, +0.45] | -0.0 % [-1.9, +0.9] | 0.89 µs | no change resolved |
| UPDATE by pk | 53.3 µs | 52.5 µs | 18,762 → 19,048 /s (+0.2 % [-1.0, +0.7]) | -0.10 µs [-0.50, +0.50] | -8.42 µs [-46.12, +21.36] | -0.31 µs [-0.49, +0.12] | +1.5 % [-2.3, +4.9] | 0.89 µs | no change resolved |
| UPDATE of one hot row | 51.9 µs | 52.4 µs | 19,268 → 19,084 /s (+0.0 % [-1.0, +0.8]) | +0.00 µs [-0.40, +0.50] | -7.22 µs [-19.32, -0.53] | -0.06 µs [-0.38, +0.32] | +1.6 % [+0.6, +4.2] | 0.89 µs | no change resolved |
| DELETE by pk | 49.1 µs | 49.0 µs | 20,367 → 20,408 /s (+0.2 % [+0.0, +0.4]) | -0.10 µs [-0.20, +0.00] | -4.45 µs [-11.67, +0.72] | -0.10 µs [-0.41, +0.10] | -0.4 % [-1.6, +1.1] | 0.89 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 147.0 µs | 150.9 µs | 6,803 → 6,627 /s (-2.6 % [-3.2, -1.9]) | +4.00 µs [+2.90, +5.00] | -38.37 µs [-108.24, +10.63] | +4.05 µs [+3.19, +5.56] | -2.7 % [-3.6, -2.4] | 0.89 µs | **B slower** (+2.7 % at p50) |
| SHOW META (control) | 50.3 µs | 50.4 µs | 19,881 → 19,841 /s (-0.4 % [-0.6, +0.0]) | +0.20 µs [+0.00, +0.30] | +1.92 µs [-0.81, +6.02] | +0.36 µs [+0.06, +0.50] | -0.5 % [-0.9, +0.9] | 0.89 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.30 µs, sd 4.18 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.30 µs, sd 2.89 µs; control SHOW META Δ p50 +0.20 µs [+0.00, +0.30]; run-to-run sd of the per-run Δ p50, INSERT 3.23 µs, UPDATE 1.61 µs, SELECT 7.02 µs, ping 0.37 µs.

### flipped pair, cores = 1, 10,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.71-1.22, after 0.86-1.39; busy of the other CPUs (6, 7 excluded) in kept runs 1.6%-4.0% (median 2.1%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-3%); gate wait median 3 s; ping p50 reference 50.1 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 8,494 us, B 8,520 us, max A 14,614 us, B 12,180 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 48.8 µs | 48.8 µs | 20,513 → 20,471 /s (+0.0 % [-0.4, +0.4]) | +0.00 µs [-0.20, +0.20] | +15.96 µs [-16.07, +46.89] | -0.28 µs [-0.74, +0.22] | -1.2 % [-56.7, +1.6] | 0.67 µs | no change resolved |
| INSERT again (replicate) | 48.8 µs | 48.7 µs | 20,513 → 20,534 /s (+0.0 % [-0.2, +0.4]) | +0.00 µs [-0.20, +0.10] | -20.14 µs [-48.51, +11.43] | -0.10 µs [-0.33, +0.07] | +41.2 % [-0.5, +114.6] | 0.67 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.0 µs | 48.9 µs | 20,429 → 20,450 /s (-0.2 % [-0.3, +0.2]) | +0.10 µs [-0.10, +0.15] | +0.26 µs [-0.32, +0.83] | +0.13 µs [-0.10, +0.25] | -1.1 % [-2.2, +1.5] | 0.67 µs | no change resolved |
| SELECT by pk | 64.8 µs | 64.8 µs | 15,420 → 15,432 /s (+0.2 % [-0.2, +0.6]) | -0.10 µs [-0.40, +0.15] | +0.01 µs [-0.36, +0.40] | -0.05 µs [-0.33, +0.10] | +0.1 % [-0.4, +0.6] | 0.67 µs | no change resolved |
| UPDATE by pk | 53.0 µs | 52.8 µs | 18,868 → 18,939 /s (+0.0 % [-0.4, +0.9]) | +0.00 µs [-0.45, +0.35] | +0.04 µs [-0.56, +0.65] | +0.02 µs [-0.44, +0.28] | +0.6 % [-1.1, +1.5] | 0.67 µs | no change resolved |
| UPDATE of one hot row | 52.2 µs | 52.0 µs | 19,175 → 19,249 /s (+0.1 % [-0.6, +1.2]) | -0.05 µs [-0.60, +0.30] | -0.29 µs [-1.51, +1.27] | -0.10 µs [-0.46, +0.22] | +1.2 % [-0.0, +2.5] | 0.67 µs | no change resolved |
| DELETE by pk | 48.9 µs | 48.9 µs | 20,450 → 20,450 /s (+0.0 % [-0.2, +0.2]) | +0.00 µs [-0.10, +0.10] | +0.38 µs [-0.16, +0.96] | +0.21 µs [-0.14, +0.36] | -0.4 % [-1.8, +0.3] | 0.67 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 804.5 µs | 839.3 µs | 1,243 → 1,191 /s (-4.4 % [-4.9, -3.5]) | +36.90 µs [+29.30, +41.10] | +41.74 µs [+33.38, +49.96] | +37.69 µs [+30.71, +45.25] | -4.9 % [-6.2, -3.1] | 0.67 µs | **B slower** (+4.6 % at p50) |
| SHOW META (control) | 50.1 µs | 50.1 µs | 19,960 → 19,960 /s (+0.2 % [-0.1, +0.4]) | -0.10 µs [-0.20, +0.00] | -0.11 µs [-0.27, +0.08] | -0.16 µs [-0.24, +0.09] | +0.3 % [-0.1, +0.6] | 0.67 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.05 µs, robust sd 0.22 µs, sd 0.22 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.15 µs; control SHOW META Δ p50 -0.10 µs [-0.20, +0.00]; run-to-run sd of the per-run Δ p50, INSERT 0.28 µs, UPDATE 0.65 µs, SELECT 0.46 µs, ping 0.23 µs.
