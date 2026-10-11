
### null pair, cores = 1, 200 rows

15 runs kept of 15 (runs 0-15); loadavg(1 min) before each run 1.28-8.73, after 1.34-8.19; busy of the other CPUs (6, 7 excluded) in kept runs 1.4%-8.6% (median 2.5%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-9%); gate wait median 3 s; ping p50 reference 50.3 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 7,618 us, B 8,507 us, max A 14,215 us, B 44,146 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.4 µs | 49.4 µs | 20,243 → 20,243 /s (+0.2 % [+0.0, +0.6]) | -0.10 µs [-0.30, -0.10] | -0.33 µs [-0.76, +0.03] | -0.28 µs [-0.31, -0.08] | +0.5 % [+0.0, +1.3] | 0.60 µs | CI excludes 0, inside the floor: not resolved |
| INSERT again (replicate) | 49.4 µs | 49.3 µs | 20,243 → 20,284 /s (+0.4 % [+0.0, +0.6]) | -0.20 µs [-0.30, +0.00] | +1.12 µs [-0.45, +2.81] | -0.08 µs [-0.34, +0.06] | -1.7 % [-4.2, +2.6] | 0.60 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.4 µs | 49.3 µs | 20,243 → 20,284 /s (+0.2 % [+0.0, +0.4]) | -0.10 µs [-0.20, +0.00] | -0.47 µs [-1.24, +0.34] | -0.18 µs [-0.34, +0.23] | +0.6 % [-1.2, +2.7] | 0.60 µs | no change resolved |
| SELECT by pk | 65.3 µs | 65.6 µs | 15,314 → 15,244 /s (+0.0 % [-0.3, +0.9]) | +0.00 µs [-0.60, +0.20] | -0.04 µs [-0.62, +0.69] | +0.12 µs [-0.59, +0.27] | +0.5 % [-0.7, +1.0] | 0.60 µs | no change resolved |
| UPDATE by pk | 52.6 µs | 52.5 µs | 19,011 → 19,048 /s (+0.4 % [-0.2, +1.0]) | -0.20 µs [-0.50, +0.10] | -0.05 µs [-0.51, +0.50] | -0.20 µs [-0.52, +0.04] | +0.8 % [-0.9, +1.4] | 0.60 µs | no change resolved |
| UPDATE of one hot row | 52.2 µs | 52.3 µs | 19,157 → 19,120 /s (+0.6 % [+0.0, +0.8]) | -0.30 µs [-0.40, +0.10] | +3.00 µs [+0.06, +8.54] | -0.15 µs [-0.31, +0.06] | -0.9 % [-1.4, +0.2] | 0.60 µs | no change resolved |
| DELETE by pk | 49.3 µs | 49.2 µs | 20,284 → 20,325 /s (+0.4 % [-0.2, +0.6]) | -0.20 µs [-0.30, +0.10] | -0.03 µs [-0.55, +0.51] | -0.17 µs [-0.35, -0.09] | +0.1 % [-0.4, +1.4] | 0.60 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 86.5 µs | 86.2 µs | 11,561 → 11,601 /s (+0.2 % [-0.2, +0.5]) | -0.20 µs [-0.40, +0.20] | +0.31 µs [-0.66, +1.43] | -0.16 µs [-0.39, +0.03] | +0.0 % [-0.3, +0.8] | 0.60 µs | no change resolved |
| SHOW META (control) | 50.2 µs | 50.4 µs | 19,920 → 19,841 /s (-0.4 % [-0.6, +0.0]) | +0.20 µs [+0.00, +0.30] | +0.09 µs [-0.20, +0.39] | +0.05 µs [-0.23, +0.28] | +0.0 % [-0.7, +0.5] | 0.60 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.95 µs; replicate arm B (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.15 µs, sd 0.32 µs; control SHOW META Δ p50 +0.20 µs [+0.00, +0.30]; run-to-run sd of the per-run Δ p50, INSERT 0.74 µs, UPDATE 0.45 µs, SELECT 0.84 µs, ping 0.42 µs.

### null pair, cores = 1, 1,000 rows

15 runs kept of 15 (runs 0-15); loadavg(1 min) before each run 1.46-3.98, after 1.67-3.82; busy of the other CPUs (6, 7 excluded) in kept runs 1.4%-4.0% (median 2.5%); CPUs 6,7 (another session's benchmark) busy over 10 % in 6 of the kept runs (range 2%-63%); gate wait median 3 s; ping p50 reference 50.3 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 6,477 us, B 10,139 us, max A 343,345 us, B 32,404 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.4 µs | 49.2 µs | 20,243 → 20,325 /s (+0.0 % [-0.4, +0.6]) | +0.00 µs [-0.20, +0.20] | +0.91 µs [-18.42, +20.23] | -0.05 µs [-0.48, +0.07] | +0.6 % [-0.2, +1.7] | 0.89 µs | no change resolved |
| INSERT again (replicate) | 49.1 µs | 49.0 µs | 20,367 → 20,408 /s (+0.0 % [-0.2, +0.2]) | +0.00 µs [-0.10, +0.10] | +12.11 µs [-2.57, +32.15] | +0.02 µs [-0.48, +0.29] | -0.0 % [-3.9, +3.7] | 0.89 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.2 µs | 49.1 µs | 20,325 → 20,367 /s (+0.4 % [-0.2, +0.6]) | -0.20 µs [-0.30, +0.10] | -0.31 µs [-0.87, +0.23] | -0.11 µs [-0.36, +0.11] | +1.1 % [-1.9, +2.0] | 0.89 µs | no change resolved |
| SELECT by pk | 68.1 µs | 67.6 µs | 14,684 → 14,793 /s (+0.7 % [-0.1, +1.2]) | -0.50 µs [-0.90, -0.30] | -0.67 µs [-1.01, -0.36] | -0.39 µs [-0.61, -0.07] | +0.8 % [+0.3, +1.5] | 0.89 µs | CI excludes 0, inside the floor: not resolved |
| UPDATE by pk | 52.9 µs | 52.3 µs | 18,904 → 19,120 /s (+1.0 % [+0.0, +1.9]) | -0.50 µs [-1.00, +0.10] | -0.17 µs [-0.92, +0.51] | -0.38 µs [-0.65, +0.08] | +0.1 % [-0.9, +1.7] | 0.89 µs | no change resolved |
| UPDATE of one hot row | 52.4 µs | 51.8 µs | 19,084 → 19,305 /s (+0.8 % [+0.0, +1.7]) | -0.40 µs [-0.90, +0.00] | -1.29 µs [-4.33, +0.53] | -0.48 µs [-0.64, -0.04] | +0.5 % [-0.8, +1.4] | 0.89 µs | no change resolved |
| DELETE by pk | 49.2 µs | 49.1 µs | 20,325 → 20,367 /s (+0.2 % [+0.0, +0.4]) | -0.10 µs [-0.20, +0.00] | +0.09 µs [-0.48, +0.90] | -0.06 µs [-0.40, +0.02] | +0.5 % [-0.3, +1.0] | 0.89 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 146.1 µs | 145.9 µs | 6,845 → 6,854 /s (+0.0 % [-0.6, +0.5]) | +0.00 µs [-0.70, +0.80] | -0.75 µs [-1.72, +0.11] | -0.94 µs [-1.23, +0.43] | +0.5 % [-0.4, +0.9] | 0.89 µs | no change resolved |
| SHOW META (control) | 50.4 µs | 50.2 µs | 19,841 → 19,920 /s (+0.0 % [-0.4, +0.6]) | +0.00 µs [-0.30, +0.20] | +1.01 µs [-4.03, +5.77] | -0.30 µs [-0.51, +0.61] | +0.4 % [+0.0, +0.9] | 0.89 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.30 µs, robust sd 0.30 µs, sd 0.41 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.30 µs, sd 0.54 µs; control SHOW META Δ p50 +0.00 µs [-0.30, +0.20]; run-to-run sd of the per-run Δ p50, INSERT 0.25 µs, UPDATE 0.69 µs, SELECT 0.59 µs, ping 0.32 µs.

### null pair, cores = 1, 10,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 1.79-5.18, after 1.80-4.69; busy of the other CPUs (6, 7 excluded) in kept runs 1.4%-3.3% (median 2.1%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 2%-3%); gate wait median 3 s; ping p50 reference 50.4 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 8,596 us, B 9,280 us, max A 21,750 us, B 21,293 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.5 µs | 49.7 µs | 20,182 → 20,141 /s (+0.2 % [-0.4, +0.3]) | -0.10 µs [-0.15, +0.20] | +22.21 µs [-4.61, +48.73] | +0.70 µs [+0.43, +1.11] | -16.6 % [-55.7, +20.8] | 0.89 µs | no change resolved |
| INSERT again (replicate) | 49.6 µs | 49.5 µs | 20,161 → 20,182 /s (+0.0 % [-0.2, +0.1]) | +0.00 µs [+0.00, +0.10] | +4.03 µs [-28.71, +38.43] | -0.02 µs [-0.20, +0.37] | +1.4 % [-54.7, +99.7] | 0.89 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.5 µs | 49.5 µs | 20,202 → 20,202 /s (+0.0 % [-0.2, +0.4]) | +0.00 µs [-0.20, +0.15] | +0.30 µs [-0.28, +0.87] | +0.01 µs [-0.21, +0.13] | -0.5 % [-2.1, +0.4] | 0.89 µs | no change resolved |
| SELECT by pk | 65.0 µs | 64.9 µs | 15,373 → 15,408 /s (-0.1 % [-0.6, +0.5]) | +0.05 µs [-0.30, +0.40] | -0.12 µs [-0.63, +0.39] | -0.08 µs [-0.26, +0.07] | -0.0 % [-0.7, +1.0] | 0.89 µs | no change resolved |
| UPDATE by pk | 52.7 µs | 52.8 µs | 18,975 → 18,939 /s (-0.4 % [-1.0, +0.6]) | +0.20 µs [-0.20, +0.50] | +0.41 µs [-0.29, +1.08] | +0.26 µs [-0.15, +0.52] | -1.0 % [-2.2, +0.8] | 0.89 µs | no change resolved |
| UPDATE of one hot row | 52.0 µs | 52.0 µs | 19,231 → 19,212 /s (-0.3 % [-1.0, +0.4]) | +0.15 µs [-0.20, +0.50] | +1.29 µs [+0.71, +2.00] | +0.21 µs [-0.63, +0.51] | -1.8 % [-3.4, -0.7] | 0.89 µs | no change resolved |
| DELETE by pk | 49.5 µs | 49.5 µs | 20,222 → 20,202 /s (+0.0 % [-0.3, +0.2]) | +0.00 µs [-0.10, +0.20] | -0.33 µs [-1.24, +0.53] | +0.13 µs [-0.11, +0.25] | +0.6 % [-1.4, +2.4] | 0.89 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 799.7 µs | 804.0 µs | 1,251 → 1,244 /s (-0.7 % [-1.3, -0.4]) | +5.65 µs [+3.10, +9.10] | +14.19 µs [+5.62, +23.49] | +6.56 µs [+4.26, +8.71] | -1.1 % [-2.4, -0.9] | 0.89 µs | **B slower** (+0.7 % at p50) |
| SHOW META (control) | 50.3 µs | 50.5 µs | 19,881 → 19,802 /s (-0.2 % [-0.4, +0.2]) | +0.10 µs [-0.10, +0.20] | -0.13 µs [-0.51, +0.16] | +0.01 µs [-0.15, +0.08] | -0.0 % [-0.4, +0.3] | 0.89 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.15 µs, sd 0.24 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.30 µs, sd 0.23 µs; control SHOW META Δ p50 +0.10 µs [-0.10, +0.20]; run-to-run sd of the per-run Δ p50, INSERT 0.21 µs, UPDATE 0.74 µs, SELECT 0.71 µs, ping 0.25 µs.
