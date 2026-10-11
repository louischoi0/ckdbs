
### null pair, cores = 2, 200 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.46-1.05, after 0.57-1.13; busy of the other CPUs (6, 7 excluded) in kept runs 1.9%-4.1% (median 3.0%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 2%-3%); gate wait median 3 s; ping p50 reference 50.2 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 7,380 us, B 7,800 us, max A 24,341 us, B 11,962 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.2 µs | 49.2 µs | 20,346 → 20,346 /s (+0.1 % [-0.2, +0.4]) | -0.05 µs [-0.20, +0.10] | +0.01 µs [-0.29, +0.32] | -0.09 µs [-0.40, +0.23] | +0.0 % [-1.0, +1.0] | 0.67 µs | no change resolved |
| INSERT again (replicate) | 49.3 µs | 49.1 µs | 20,284 → 20,367 /s (+0.2 % [-0.4, +0.5]) | -0.10 µs [-0.20, +0.20] | -0.79 µs [-1.62, -0.07] | -0.03 µs [-0.36, +0.13] | +0.6 % [-0.3, +2.6] | 0.67 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.7 µs | 49.7 µs | 20,121 → 20,141 /s (+0.2 % [-0.2, +0.9]) | -0.10 µs [-0.40, +0.15] | +0.29 µs [-0.83, +1.39] | -0.13 µs [-0.32, +0.16] | -0.7 % [-2.0, +1.1] | 0.67 µs | no change resolved |
| SELECT by pk | 66.3 µs | 66.2 µs | 15,083 → 15,094 /s (+0.2 % [-0.7, +0.6]) | -0.10 µs [-0.40, +0.50] | -0.64 µs [-1.51, +0.04] | -0.04 µs [-0.64, +0.29] | +0.6 % [-0.8, +1.4] | 0.67 µs | no change resolved |
| UPDATE by pk | 53.8 µs | 53.7 µs | 18,605 → 18,622 /s (-0.2 % [-0.6, +0.6]) | +0.10 µs [-0.30, +0.30] | +0.63 µs [-0.31, +1.44] | +0.05 µs [-0.25, +0.24] | -1.9 % [-3.2, -0.6] | 0.67 µs | no change resolved |
| UPDATE of one hot row | 53.5 µs | 53.5 µs | 18,709 → 18,674 /s (+0.0 % [-0.6, +0.3]) | +0.00 µs [-0.15, +0.35] | +0.93 µs [+0.36, +1.45] | +0.02 µs [-0.18, +0.26] | -1.9 % [-2.7, -1.2] | 0.67 µs | no change resolved |
| DELETE by pk | 49.2 µs | 49.5 µs | 20,346 → 20,182 /s (-0.4 % [-0.7, +0.0]) | +0.20 µs [+0.00, +0.35] | -1.38 µs [-2.61, -0.38] | +0.19 µs [-0.15, +0.45] | +1.8 % [-0.3, +3.8] | 0.67 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 87.4 µs | 87.5 µs | 11,442 → 11,429 /s (+0.1 % [-0.8, +0.6]) | -0.05 µs [-0.50, +0.70] | +0.34 µs [-0.53, +1.24] | +0.19 µs [-0.52, +1.13] | +0.1 % [-1.0, +1.1] | 0.67 µs | no change resolved |
| SHOW META (control) | 50.3 µs | 50.2 µs | 19,881 → 19,940 /s (+0.4 % [+0.0, +0.8]) | -0.20 µs [-0.40, +0.00] | -0.53 µs [-0.95, -0.18] | -0.19 µs [-0.53, +0.13] | +0.5 % [+0.1, +1.3] | 0.67 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.05 µs, robust sd 0.22 µs, sd 0.16 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.15 µs; control SHOW META Δ p50 -0.20 µs [-0.40, +0.00]; run-to-run sd of the per-run Δ p50, INSERT 0.48 µs, UPDATE 0.67 µs, SELECT 0.68 µs, ping 0.37 µs.

### null pair, cores = 2, 1,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.36-1.14, after 0.33-1.21; busy of the other CPUs (6, 7 excluded) in kept runs 2.2%-4.3% (median 3.3%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 2%-4%); gate wait median 3 s; ping p50 reference 50.3 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 8,840 us, B 7,610 us, max A 18,680 us, B 15,695 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.2 µs | 49.2 µs | 20,346 → 20,346 /s (+0.0 % [-0.2, +0.3]) | +0.00 µs [-0.10, +0.10] | +0.01 µs [-0.27, +0.27] | -0.00 µs [-0.18, +0.19] | -0.2 % [-0.6, +0.4] | 0.60 µs | no change resolved |
| INSERT again (replicate) | 49.2 µs | 49.2 µs | 20,346 → 20,325 /s (-0.2 % [-0.4, +0.4]) | +0.10 µs [-0.20, +0.20] | -0.20 µs [-0.75, +0.39] | +0.08 µs [-0.21, +0.23] | +0.7 % [-1.0, +2.4] | 0.60 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.7 µs | 49.7 µs | 20,141 → 20,141 /s (+0.1 % [-0.8, +0.4]) | -0.05 µs [-0.25, +0.40] | -0.09 µs [-0.96, +0.86] | -0.05 µs [-0.30, +0.15] | +1.0 % [-1.8, +2.0] | 0.60 µs | no change resolved |
| SELECT by pk | 68.5 µs | 68.2 µs | 14,599 → 14,674 /s (+0.1 % [-0.3, +1.0]) | -0.10 µs [-0.70, +0.20] | -0.61 µs [-1.11, -0.09] | -0.06 µs [-0.70, +0.28] | +0.8 % [-0.2, +1.7] | 0.60 µs | no change resolved |
| UPDATE by pk | 53.8 µs | 53.9 µs | 18,587 → 18,553 /s (+0.1 % [-0.6, +0.4]) | -0.05 µs [-0.20, +0.30] | -0.08 µs [-0.63, +0.49] | -0.07 µs [-0.35, +0.16] | +0.5 % [-1.0, +0.9] | 0.60 µs | no change resolved |
| UPDATE of one hot row | 53.3 µs | 53.5 µs | 18,762 → 18,692 /s (+0.0 % [-0.7, +0.3]) | +0.00 µs [-0.10, +0.30] | +0.95 µs [+0.31, +1.54] | +0.17 µs [-0.09, +0.48] | -1.7 % [-3.2, +0.0] | 0.60 µs | no change resolved |
| DELETE by pk | 49.3 µs | 49.3 µs | 20,284 → 20,284 /s (-0.1 % [-0.4, +0.2]) | +0.05 µs [-0.10, +0.40] | -0.77 µs [-1.48, -0.11] | +0.09 µs [-0.20, +0.48] | +0.5 % [-0.1, +3.4] | 0.60 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 147.7 µs | 147.4 µs | 6,770 → 6,784 /s (-0.0 % [-0.3, +0.5]) | +0.05 µs [-0.80, +0.50] | +0.16 µs [-0.89, +1.21] | +0.15 µs [-0.46, +0.92] | -0.0 % [-0.7, +0.7] | 0.60 µs | no change resolved |
| SHOW META (control) | 50.3 µs | 50.2 µs | 19,861 → 19,920 /s (+0.6 % [-0.2, +1.0]) | -0.30 µs [-0.50, +0.10] | -0.28 µs [-1.36, +0.77] | -0.17 µs [-0.40, +0.18] | +0.4 % [-0.6, +1.3] | 0.60 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.15 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.15 µs; control SHOW META Δ p50 -0.30 µs [-0.50, +0.10]; run-to-run sd of the per-run Δ p50, INSERT 0.30 µs, UPDATE 0.50 µs, SELECT 0.56 µs, ping 0.48 µs.

### null pair, cores = 2, 10,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.48-1.07, after 0.56-1.20; busy of the other CPUs (6, 7 excluded) in kept runs 2.2%-4.5% (median 2.6%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 2%-4%); gate wait median 3 s; ping p50 reference 50.3 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 7,585 us, B 7,882 us, max A 15,018 us, B 24,706 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.0 µs | 49.3 µs | 20,408 → 20,284 /s (+0.0 % [-1.4, +0.4]) | +0.00 µs [-0.20, +0.70] | +19.60 µs [-0.31, +39.57] | +0.72 µs [+0.14, +1.36] | -29.7 % [-41.8, +4.0] | 0.67 µs | no change resolved |
| INSERT again (replicate) | 49.1 µs | 49.1 µs | 20,367 → 20,367 /s (-0.2 % [-0.6, +0.0]) | +0.10 µs [+0.00, +0.30] | +1.74 µs [-1.34, +6.48] | +0.01 µs [-0.33, +0.26] | +0.2 % [-0.8, +1.6] | 0.67 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.8 µs | 50.0 µs | 20,060 → 19,980 /s (-0.2 % [-0.8, +0.7]) | +0.10 µs [-0.20, +0.40] | -0.40 µs [-1.88, +0.56] | -0.12 µs [-0.25, +0.22] | -0.3 % [-1.3, +0.7] | 0.67 µs | no change resolved |
| SELECT by pk | 65.8 µs | 65.7 µs | 15,198 → 15,232 /s (+0.1 % [-0.3, +0.5]) | -0.05 µs [-0.30, +0.20] | -0.29 µs [-0.78, +0.22] | -0.40 µs [-0.56, +0.07] | +0.3 % [-0.1, +1.1] | 0.67 µs | no change resolved |
| UPDATE by pk | 54.0 µs | 54.2 µs | 18,501 → 18,450 /s (-0.6 % [-0.9, +0.0]) | +0.30 µs [+0.00, +0.50] | +0.18 µs [-0.47, +0.94] | +0.15 µs [-0.12, +0.20] | -0.1 % [-1.1, +1.3] | 0.67 µs | no change resolved |
| UPDATE of one hot row | 53.3 µs | 53.5 µs | 18,762 → 18,674 /s (-0.5 % [-1.3, +0.5]) | +0.25 µs [-0.25, +0.70] | +1.58 µs [+0.56, +3.01] | +0.30 µs [+0.05, +0.61] | -2.0 % [-2.9, -0.9] | 0.67 µs | no change resolved |
| DELETE by pk | 49.2 µs | 49.5 µs | 20,325 → 20,202 /s (-0.5 % [-1.6, -0.1]) | +0.25 µs [+0.05, +0.80] | +0.15 µs [-0.23, +0.53] | +0.27 µs [+0.00, +0.51] | -0.3 % [-1.3, +0.7] | 0.67 µs | CI excludes 0, inside the floor: not resolved |
| COUNT/MIN/MAX over 100 ids | 815.5 µs | 813.8 µs | 1,226 → 1,229 /s (+0.2 % [-0.2, +0.5]) | -1.55 µs [-3.70, +1.40] | -0.12 µs [-6.21, +6.17] | -0.81 µs [-3.95, +1.72] | +0.1 % [-0.5, +1.1] | 0.67 µs | no change resolved |
| SHOW META (control) | 50.2 µs | 50.4 µs | 19,920 → 19,841 /s (-0.2 % [-0.5, +0.0]) | +0.10 µs [+0.00, +0.20] | +1.05 µs [+0.09, +2.26] | +0.12 µs [-0.28, +0.37] | -0.5 % [-1.3, +0.2] | 0.67 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.15 µs, robust sd 0.22 µs, sd 0.22 µs; replicate arm B (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.22 µs, sd 0.40 µs; control SHOW META Δ p50 +0.10 µs [+0.00, +0.20]; run-to-run sd of the per-run Δ p50, INSERT 0.62 µs, UPDATE 0.45 µs, SELECT 0.53 µs, ping 0.37 µs.
