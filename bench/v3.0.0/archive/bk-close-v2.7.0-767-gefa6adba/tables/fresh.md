
### fresh copies (A copy 3 against B copy 2), cores = 1, 1,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.55-0.86, after 0.65-0.95; busy of the other CPUs (6, 7 excluded) in kept runs 1.0%-2.0% (median 1.5%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-2%); gate wait median 3 s; ping p50 reference 50.1 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 10,882 us, B 10,016 us, max A 23,686 us, B 25,461 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 48,000 | 36.0 µs | 48.1 µs | 48.9 µs | 50.2 µs | 63.2 µs | 78.1 µs | 50.9 µs (sd 0.6) | 19,443 /s | 28.29 µs |
| INSERT, pk omitted | B | 48,000 | 40.9 µs | 48.1 µs | 48.9 µs | 50.1 µs | 62.7 µs | 76.4 µs | 50.7 µs (sd 0.5) | 19,489 /s | 28.31 µs |
| INSERT again (replicate) | A | 48,000 | 41.1 µs | 48.0 µs | 48.8 µs | 50.0 µs | 62.6 µs | 77.8 µs | 51.4 µs (sd 1.4) | 19,422 /s | 28.17 µs |
| INSERT again (replicate) | B | 48,000 | 40.7 µs | 48.1 µs | 48.8 µs | 50.0 µs | 62.4 µs | 75.9 µs | 51.6 µs (sd 1.5) | 19,095 /s | 28.26 µs |
| INSERT, pk named (keys below 2^40) | A | 48,000 | 39.7 µs | 48.2 µs | 48.9 µs | 50.2 µs | 63.3 µs | 77.9 µs | 51.7 µs (sd 0.8) | 19,031 /s | 28.47 µs |
| INSERT, pk named (keys below 2^40) | B | 48,000 | 40.8 µs | 48.2 µs | 49.0 µs | 50.2 µs | 63.2 µs | 75.8 µs | 51.5 µs (sd 0.8) | 19,086 /s | 28.54 µs |
| SELECT by pk | A | 48,000 | 59.6 µs | 64.8 µs | 67.2 µs | 73.0 µs | 85.8 µs | 101.1 µs | 70.4 µs (sd 0.6) | 14,079 /s | 35.53 µs |
| SELECT by pk | B | 48,000 | 56.7 µs | 64.3 µs | 66.9 µs | 72.5 µs | 85.2 µs | 100.0 µs | 70.2 µs (sd 0.9) | 14,147 /s | 35.16 µs |
| UPDATE by pk | A | 48,000 | 42.9 µs | 51.5 µs | 52.2 µs | 54.1 µs | 66.6 µs | 81.1 µs | 55.2 µs (sd 1.2) | 17,978 /s | 30.71 µs |
| UPDATE by pk | B | 48,000 | 42.8 µs | 51.2 µs | 52.1 µs | 54.0 µs | 66.5 µs | 80.0 µs | 55.5 µs (sd 1.9) | 18,030 /s | 30.45 µs |
| UPDATE of one hot row | A | 48,000 | 40.1 µs | 51.2 µs | 51.8 µs | 53.7 µs | 66.5 µs | 80.2 µs | 54.1 µs (sd 0.6) | 18,378 /s | 30.50 µs |
| UPDATE of one hot row | B | 48,000 | 43.6 µs | 51.0 µs | 51.8 µs | 53.6 µs | 66.3 µs | 78.9 µs | 54.9 µs (sd 1.3) | 18,046 /s | 30.38 µs |
| DELETE by pk | A | 48,000 | 40.7 µs | 48.1 µs | 48.9 µs | 49.9 µs | 61.0 µs | 71.0 µs | 51.1 µs (sd 1.4) | 19,697 /s | 27.71 µs |
| DELETE by pk | B | 48,000 | 39.9 µs | 48.2 µs | 49.0 µs | 50.0 µs | 61.5 µs | 71.4 µs | 50.8 µs (sd 0.8) | 19,637 /s | 27.88 µs |
| COUNT/MIN/MAX over 100 ids | A | 48,000 | 135.4 µs | 142.3 µs | 145.3 µs | 152.5 µs | 168.0 µs | 195.1 µs | 149.1 µs (sd 1.6) | 6,687 /s | 112.21 µs |
| COUNT/MIN/MAX over 100 ids | B | 48,000 | 136.8 µs | 144.2 µs | 147.3 µs | 153.6 µs | 167.7 µs | 188.4 µs | 150.6 µs (sd 1.0) | 6,622 /s | 113.99 µs |
| SHOW META (control) | A | 48,000 | 40.9 µs | 49.5 µs | 50.2 µs | 52.6 µs | 61.5 µs | 68.7 µs | 51.9 µs (sd 0.4) | 19,161 /s | 32.80 µs |
| SHOW META (control) | B | 48,000 | 44.1 µs | 49.4 µs | 50.1 µs | 52.5 µs | 61.1 µs | 68.4 µs | 51.8 µs (sd 0.6) | 19,251 /s | 32.67 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 48,000 | 40.6 µs | 48.3 µs | 49.0 µs | 50.4 µs | 63.4 µs | 74.3 µs | 51.2 µs (sd 0.7) | 19,193 /s | 28.44 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 48,000 | 40.2 µs | 48.4 µs | 49.1 µs | 50.5 µs | 63.6 µs | 73.3 µs | 52.0 µs (sd 0.9) | 18,925 /s | 28.49 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 48.9 µs | 49.0 µs | 20,450 → 20,429 /s (+0.1 % [+0.0, +0.3]) | -0.05 µs [-0.15, +0.00] | -0.18 µs [-0.39, -0.01] | -0.02 µs [-0.25, +0.11] | +0.3 % [-0.2, +0.6] | 0.60 µs | no change resolved |
| INSERT again (replicate) | 48.8 µs | 48.8 µs | 20,492 → 20,471 /s (-0.2 % [-0.2, +0.2]) | +0.10 µs [-0.10, +0.10] | +0.24 µs [-0.43, +0.89] | +0.08 µs [-0.27, +0.20] | -0.3 % [-2.3, +1.3] | 0.60 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 48.8 µs | 48.8 µs | 20,471 → 20,492 /s (+0.0 % [-0.3, +0.2]) | +0.00 µs [-0.10, +0.10] | -0.23 µs [-0.67, +0.26] | +0.01 µs [-0.23, +0.25] | +1.2 % [-0.7, +1.6] | 0.60 µs | no change resolved |
| SELECT by pk | 67.0 µs | 66.8 µs | 14,925 → 14,959 /s (+0.6 % [-0.1, +1.3]) | -0.40 µs [-0.85, +0.10] | -0.18 µs [-0.61, +0.26] | -0.53 µs [-0.96, +0.23] | +0.2 % [-0.8, +1.5] | 0.60 µs | no change resolved |
| UPDATE by pk | 52.2 µs | 51.8 µs | 19,175 → 19,305 /s (+0.8 % [+0.2, +0.9]) | -0.40 µs [-0.45, -0.10] | +0.27 µs [-0.62, +1.23] | -0.24 µs [-0.41, +0.17] | +0.9 % [-2.4, +2.5] | 0.60 µs | CI excludes 0, inside the floor: not resolved |
| UPDATE of one hot row | 51.8 µs | 51.5 µs | 19,305 → 19,417 /s (+0.5 % [+0.0, +1.0]) | -0.25 µs [-0.50, +0.00] | +0.82 µs [+0.21, +1.48] | -0.09 µs [-0.39, +0.07] | -1.2 % [-3.1, +0.5] | 0.60 µs | no change resolved |
| DELETE by pk | 48.9 µs | 48.9 µs | 20,450 → 20,450 /s (+0.0 % [-0.6, +0.4]) | +0.00 µs [-0.20, +0.30] | -0.27 µs [-0.95, +0.22] | +0.16 µs [-0.14, +0.44] | +0.2 % [-0.5, +0.7] | 0.60 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 145.2 µs | 147.1 µs | 6,887 → 6,798 /s (-1.2 % [-1.5, -1.0]) | +1.70 µs [+1.50, +2.20] | +1.54 µs [+0.83, +2.23] | +1.65 µs [+1.42, +2.18] | -1.0 % [-1.6, -0.6] | 0.60 µs | **B slower** (+1.2 % at p50) |
| SHOW META (control) | 50.1 µs | 50.0 µs | 19,960 → 19,980 /s (+0.2 % [+0.0, +0.5]) | -0.10 µs [-0.20, +0.00] | -0.10 µs [-0.28, +0.09] | -0.15 µs [-0.33, +0.00] | +0.4 % [-0.2, +0.7] | 0.60 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.10 µs [+0.00, +0.15], Δ mean +0.74 µs [+0.20, +1.24], Δ server CPU per statement +0.05 µs [+0.01, +0.17]; lo p50 48.9 µs, hi p50 49.0 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +0.25 µs [+0.00, +0.35].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.05 µs, robust sd 0.15 µs, sd 0.17 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.15 µs; control SHOW META Δ p50 -0.10 µs [-0.20, +0.00]; run-to-run sd of the per-run Δ p50, INSERT 0.13 µs, UPDATE 0.65 µs, SELECT 0.67 µs, ping 0.28 µs.

### fresh copies (A copy 3 against B copy 2), cores = 1, 10,000 rows

15 runs kept of 15 (runs 1-15); loadavg(1 min) before each run 0.64-0.95, after 0.76-1.08; busy of the other CPUs (6, 7 excluded) in kept runs 1.0%-2.0% (median 1.6%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-3%); gate wait median 3 s; ping p50 reference 50.0 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 8,439 us, B 9,906 us, max A 23,006 us, B 21,203 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 45,000 | 39.7 µs | 47.9 µs | 48.7 µs | 50.1 µs | 62.1 µs | 76.6 µs | 105.2 µs (sd 60.7) | 9,797 /s | 28.44 µs |
| INSERT, pk omitted | B | 45,000 | 38.4 µs | 48.0 µs | 48.7 µs | 50.0 µs | 61.9 µs | 74.9 µs | 97.7 µs (sd 57.6) | 18,718 /s | 28.68 µs |
| INSERT again (replicate) | A | 45,000 | 40.3 µs | 47.9 µs | 48.7 µs | 50.2 µs | 63.7 µs | 79.1 µs | 58.9 µs (sd 20.9) | 19,218 /s | 28.44 µs |
| INSERT again (replicate) | B | 45,000 | 41.1 µs | 48.0 µs | 48.7 µs | 50.2 µs | 63.5 µs | 79.4 µs | 73.8 µs (sd 30.1) | 18,538 /s | 28.69 µs |
| INSERT, pk named (keys below 2^40) | A | 45,000 | 40.7 µs | 48.3 µs | 49.0 µs | 50.4 µs | 63.7 µs | 78.6 µs | 51.8 µs (sd 0.7) | 19,043 /s | 28.56 µs |
| INSERT, pk named (keys below 2^40) | B | 45,000 | 40.6 µs | 48.3 µs | 48.9 µs | 50.4 µs | 63.8 µs | 78.4 µs | 51.7 µs (sd 0.8) | 19,120 /s | 28.70 µs |
| SELECT by pk | A | 45,000 | 58.3 µs | 63.8 µs | 64.7 µs | 66.6 µs | 78.5 µs | 89.9 µs | 67.2 µs (sd 1.1) | 14,811 /s | 31.97 µs |
| SELECT by pk | B | 45,000 | 58.5 µs | 63.7 µs | 64.5 µs | 66.2 µs | 77.9 µs | 87.0 µs | 66.4 µs (sd 0.7) | 14,914 /s | 31.81 µs |
| UPDATE by pk | A | 45,000 | 43.7 µs | 51.9 µs | 52.9 µs | 54.7 µs | 67.2 µs | 81.1 µs | 55.6 µs (sd 0.9) | 17,836 /s | 31.14 µs |
| UPDATE by pk | B | 45,000 | 43.2 µs | 51.9 µs | 52.8 µs | 54.6 µs | 67.0 µs | 79.5 µs | 55.4 µs (sd 1.0) | 17,773 /s | 31.26 µs |
| UPDATE of one hot row | A | 45,000 | 43.0 µs | 51.3 µs | 52.2 µs | 54.5 µs | 66.9 µs | 79.7 µs | 54.4 µs (sd 0.9) | 18,291 /s | 30.61 µs |
| UPDATE of one hot row | B | 45,000 | 40.4 µs | 51.2 µs | 52.1 µs | 54.2 µs | 66.6 µs | 78.2 µs | 55.0 µs (sd 1.1) | 18,102 /s | 30.65 µs |
| DELETE by pk | A | 45,000 | 39.9 µs | 48.1 µs | 48.9 µs | 50.1 µs | 61.9 µs | 72.5 µs | 51.2 µs (sd 0.9) | 19,394 /s | 27.91 µs |
| DELETE by pk | B | 45,000 | 40.6 µs | 48.2 µs | 48.9 µs | 50.1 µs | 62.4 µs | 72.2 µs | 50.8 µs (sd 1.0) | 19,569 /s | 28.02 µs |
| COUNT/MIN/MAX over 100 ids | A | 45,000 | 722.0 µs | 776.0 µs | 799.7 µs | 823.7 µs | 853.7 µs | 991.3 µs | 814.9 µs (sd 12.1) | 1,227 /s | 758.22 µs |
| COUNT/MIN/MAX over 100 ids | B | 45,000 | 733.8 µs | 790.2 µs | 813.2 µs | 837.5 µs | 874.6 µs | 1,059.8 µs | 832.1 µs (sd 11.8) | 1,197 /s | 773.68 µs |
| SHOW META (control) | A | 45,000 | 41.8 µs | 49.4 µs | 50.1 µs | 52.5 µs | 61.3 µs | 68.7 µs | 51.7 µs (sd 0.5) | 19,216 /s | 32.90 µs |
| SHOW META (control) | B | 45,000 | 42.5 µs | 49.3 µs | 50.0 µs | 52.6 µs | 61.2 µs | 69.1 µs | 51.8 µs (sd 0.4) | 19,213 /s | 32.87 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 45,000 | 39.2 µs | 48.3 µs | 49.0 µs | 50.3 µs | 62.9 µs | 73.2 µs | 51.4 µs (sd 0.7) | 19,183 /s | 28.44 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 45,000 | 39.6 µs | 48.5 µs | 49.1 µs | 50.5 µs | 63.2 µs | 74.1 µs | 52.0 µs (sd 1.2) | 18,866 /s | 28.63 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 48.8 µs | 48.8 µs | 20,492 → 20,492 /s (+0.0 % [-0.2, +0.6]) | +0.00 µs [-0.30, +0.20] | -7.47 µs [-62.95, +47.14] | +0.64 µs [-0.50, +1.12] | +86.4 % [-60.5, +128.0] | 0.89 µs | no change resolved |
| INSERT again (replicate) | 48.8 µs | 48.8 µs | 20,492 → 20,492 /s (+0.0 % [-0.4, +0.2]) | +0.00 µs [-0.10, +0.20] | +14.89 µs [-5.27, +34.74] | +0.07 µs [-0.07, +0.28] | -3.5 % [-52.6, -0.2] | 0.89 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 48.9 µs | 48.9 µs | 20,450 → 20,450 /s (+0.0 % [-0.4, +0.4]) | +0.00 µs [-0.20, +0.20] | -0.11 µs [-0.59, +0.41] | +0.06 µs [-0.18, +0.40] | +0.7 % [-1.5, +1.7] | 0.89 µs | no change resolved |
| SELECT by pk | 64.5 µs | 64.5 µs | 15,504 → 15,504 /s (+0.3 % [-0.3, +0.9]) | -0.20 µs [-0.60, +0.20] | -0.75 µs [-1.22, -0.34] | -0.21 µs [-0.59, +0.01] | +0.9 % [+0.2, +2.0] | 0.89 µs | no change resolved |
| UPDATE by pk | 52.6 µs | 52.8 µs | 19,011 → 18,939 /s (+0.4 % [-0.6, +1.1]) | -0.20 µs [-0.60, +0.30] | -0.18 µs [-0.81, +0.46] | +0.17 µs [-0.28, +0.30] | +0.1 % [-0.8, +2.0] | 0.89 µs | no change resolved |
| UPDATE of one hot row | 52.0 µs | 51.9 µs | 19,231 → 19,268 /s (+0.6 % [-0.2, +1.3]) | -0.30 µs [-0.70, +0.10] | +0.59 µs [+0.03, +1.26] | -0.24 µs [-0.39, +0.30] | -0.5 % [-2.0, -0.1] | 0.89 µs | no change resolved |
| DELETE by pk | 48.8 µs | 48.9 µs | 20,492 → 20,450 /s (-0.2 % [-0.4, +0.2]) | +0.10 µs [-0.10, +0.20] | -0.32 µs [-0.84, +0.30] | +0.11 µs [-0.22, +0.33] | +0.7 % [-0.0, +1.2] | 0.89 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 798.2 µs | 813.0 µs | 1,253 → 1,230 /s (-1.5 % [-2.0, -1.3]) | +12.10 µs [+10.60, +16.00] | +17.23 µs [+11.29, +23.28] | +15.85 µs [+12.43, +19.50] | -1.8 % [-3.6, -0.9] | 0.89 µs | **B slower** (+1.5 % at p50) |
| SHOW META (control) | 50.1 µs | 50.0 µs | 19,960 → 20,000 /s (+0.2 % [-0.4, +0.6]) | -0.10 µs [-0.20, +0.20] | +0.06 µs [-0.20, +0.36] | +0.05 µs [-0.28, +0.17] | +0.1 % [-0.5, +0.7] | 0.89 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.20 µs [+0.10, +0.20], Δ mean +0.59 µs [-0.20, +1.37], Δ server CPU per statement +0.12 µs [+0.05, +0.26]; lo p50 49.0 µs, hi p50 49.2 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +0.20 µs [+0.00, +0.50].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.15 µs, sd 0.25 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.30 µs, sd 0.25 µs; control SHOW META Δ p50 -0.10 µs [-0.20, +0.20]; run-to-run sd of the per-run Δ p50, INSERT 0.38 µs, UPDATE 0.66 µs, SELECT 0.49 µs, ping 0.35 µs.
