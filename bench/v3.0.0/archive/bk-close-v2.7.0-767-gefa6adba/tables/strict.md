
### strict, cores = 1, 200 rows

6 runs kept of 6 (runs 0-5); loadavg(1 min) before each run 1.26-8.83, after 1.47-7.70; busy of the other CPUs (6, 7 excluded) in kept runs 2.0%-5.2% (median 2.9%); CPUs 6,7 (another session's benchmark) busy over 10 % in 2 of the kept runs (range 2%-18%); gate wait median 3 s; ping p50 reference 51.5 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 11,621 us, B 7,582 us, max A 56,559 us, B 11,825 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 3,600 | 874.5 µs | 969.1 µs | 1,002.3 µs | 1,055.7 µs | 1,748.0 µs | 5,070.4 µs | 1169.1 µs (sd 44.2) | 863 /s | 77.48 µs |
| INSERT, pk omitted | B | 3,600 | 868.5 µs | 968.5 µs | 1,003.4 µs | 1,055.1 µs | 2,050.0 µs | 5,052.9 µs | 1185.3 µs (sd 60.4) | 847 /s | 78.79 µs |
| INSERT again (replicate) | A | 3,600 | 861.2 µs | 972.0 µs | 1,009.6 µs | 1,065.1 µs | 2,062.9 µs | 4,871.5 µs | 1190.1 µs (sd 36.0) | 835 /s | 78.49 µs |
| INSERT again (replicate) | B | 3,600 | 857.7 µs | 973.6 µs | 1,012.3 µs | 1,075.0 µs | 2,269.3 µs | 6,862.4 µs | 1745.2 µs (sd 834.5) | 799 /s | 79.55 µs |
| INSERT, pk named (keys below 2^40) | A | 3,600 | 878.8 µs | 971.7 µs | 1,008.5 µs | 1,062.1 µs | 1,875.8 µs | 5,041.7 µs | 1441.6 µs (sd 478.5) | 837 /s | 79.42 µs |
| INSERT, pk named (keys below 2^40) | B | 3,600 | 869.7 µs | 970.9 µs | 1,006.1 µs | 1,056.4 µs | 1,751.0 µs | 4,535.5 µs | 1408.2 µs (sd 469.8) | 874 /s | 79.59 µs |
| SELECT by pk | A | 3,600 | 55.7 µs | 65.2 µs | 69.7 µs | 74.2 µs | 91.1 µs | 128.0 µs | 72.7 µs (sd 0.5) | 13,656 /s | 36.69 µs |
| SELECT by pk | B | 3,600 | 58.9 µs | 65.0 µs | 68.4 µs | 73.8 µs | 89.8 µs | 115.4 µs | 71.9 µs (sd 1.0) | 13,831 /s | 35.85 µs |
| UPDATE by pk | A | 3,600 | 879.7 µs | 980.1 µs | 1,015.4 µs | 1,070.5 µs | 2,013.8 µs | 5,585.3 µs | 1676.0 µs (sd 797.1) | 818 /s | 84.07 µs |
| UPDATE by pk | B | 3,600 | 882.5 µs | 978.3 µs | 1,013.5 µs | 1,068.3 µs | 1,967.8 µs | 5,353.9 µs | 1284.0 µs (sd 212.8) | 825 /s | 84.53 µs |
| UPDATE of one hot row | A | 3,600 | 870.0 µs | 972.9 µs | 1,009.4 µs | 1,062.7 µs | 1,819.9 µs | 6,105.8 µs | 1457.6 µs (sd 612.4) | 813 /s | 83.45 µs |
| UPDATE of one hot row | B | 3,600 | 881.3 µs | 973.2 µs | 1,008.2 µs | 1,055.9 µs | 1,757.0 µs | 4,611.7 µs | 1399.5 µs (sd 631.8) | 872 /s | 82.91 µs |
| DELETE by pk | A | 3,600 | 859.4 µs | 966.2 µs | 1,000.6 µs | 1,050.8 µs | 1,903.0 µs | 4,998.5 µs | 1256.7 µs (sd 202.5) | 839 /s | 78.57 µs |
| DELETE by pk | B | 3,600 | 867.3 µs | 968.3 µs | 1,002.3 µs | 1,055.0 µs | 2,106.6 µs | 5,707.0 µs | 1302.4 µs (sd 263.8) | 839 /s | 79.40 µs |
| COUNT/MIN/MAX over 100 ids | A | 3,600 | 82.9 µs | 85.5 µs | 87.0 µs | 92.8 µs | 111.4 µs | 182.0 µs | 96.7 µs (sd 14.6) | 10,922 /s | 54.47 µs |
| COUNT/MIN/MAX over 100 ids | B | 3,600 | 83.4 µs | 86.5 µs | 87.7 µs | 93.2 µs | 110.4 µs | 136.1 µs | 92.4 µs (sd 1.1) | 10,737 /s | 55.42 µs |
| SHOW META (control) | A | 3,600 | 44.0 µs | 49.6 µs | 51.4 µs | 53.5 µs | 64.1 µs | 73.1 µs | 53.1 µs (sd 0.3) | 18,745 /s | 32.50 µs |
| SHOW META (control) | B | 3,600 | 45.1 µs | 49.7 µs | 51.6 µs | 53.4 µs | 64.1 µs | 73.8 µs | 53.2 µs (sd 0.6) | 18,732 /s | 32.45 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 3,600 | 867.9 µs | 967.5 µs | 1,003.3 µs | 1,057.2 µs | 1,994.4 µs | 4,888.8 µs | 1181.7 µs (sd 53.4) | 856 /s | 78.94 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 3,600 | 881.4 µs | 967.2 µs | 1,001.5 µs | 1,054.8 µs | 1,790.0 µs | 4,589.7 µs | 1186.8 µs (sd 82.9) | 867 /s | 79.29 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 1004.2 µs | 1001.9 µs | 996 → 998 /s (+0.1 % [-0.9, +0.7]) | -0.55 µs [-6.75, +8.50] | +16.12 µs [-50.17, +77.80] | +1.38 µs [-0.38, +1.93] | -1.5 % [-8.6, +6.9] | 47.15 µs | no change resolved |
| INSERT again (replicate) | 1015.9 µs | 1016.0 µs | 984 → 984 /s (-0.0 % [-1.4, +0.3]) | +0.10 µs [-3.15, +14.45] | +555.12 µs [+14.17, +1105.65] | +1.38 µs [-0.37, +1.96] | -4.4 % [-57.3, +0.5] | 47.15 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 1006.5 µs | 1004.7 µs | 994 → 995 /s (+0.1 % [-0.2, +0.5]) | -0.80 µs [-5.10, +1.95] | -33.45 µs [-335.38, +271.45] | +0.18 µs [-1.44, +2.86] | +0.1 % [-13.3, +25.8] | 47.15 µs | no change resolved |
| SELECT by pk | 69.3 µs | 68.3 µs | 14,430 → 14,641 /s (+1.5 % [-1.7, +4.2]) | -1.00 µs [-2.85, +1.20] | -0.77 µs [-1.47, +0.17] | -0.62 µs [-1.17, +0.48] | +2.0 % [-0.7, +2.1] | 47.15 µs | no change resolved |
| UPDATE by pk | 1011.3 µs | 1010.8 µs | 989 → 989 /s (+0.3 % [-0.3, +0.6]) | -3.10 µs [-6.55, +3.00] | -392.02 µs [-980.55, +38.27] | +1.02 µs [-1.01, +1.28] | +2.5 % [-6.1, +92.3] | 47.15 µs | no change resolved |
| UPDATE of one hot row | 1005.3 µs | 1005.6 µs | 995 → 994 /s (-0.0 % [-0.1, +0.7]) | +0.15 µs [-7.05, +0.65] | -58.10 µs [-803.53, +691.10] | -0.34 µs [-0.71, +2.31] | +7.3 % [-28.1, +72.5] | 47.15 µs | no change resolved |
| DELETE by pk | 994.1 µs | 1000.9 µs | 1,006 → 999 /s (-0.2 % [-0.8, +0.1]) | +1.90 µs [-1.40, +8.15] | +45.62 µs [-221.70, +328.80] | +1.77 µs [-0.58, +2.92] | -3.1 % [-20.2, +21.4] | 47.15 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 87.0 µs | 87.8 µs | 11,501 → 11,396 /s (-0.7 % [-1.6, -0.2]) | +0.60 µs [+0.15, +1.40] | -4.27 µs [-15.70, +1.75] | +1.00 µs [-0.63, +1.42] | -1.0 % [-2.2, +16.9] | 47.15 µs | CI excludes 0, inside the floor: not resolved |
| SHOW META (control) | 51.5 µs | 51.4 µs | 19,417 → 19,455 /s (+0.6 % [-3.2, +1.6]) | -0.30 µs [-0.80, +1.65] | +0.15 µs [-0.27, +0.65] | +0.05 µs [-0.29, +0.49] | -0.1 % [-1.5, +0.8] | 47.15 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 -2.60 µs [-10.65, +4.80], Δ mean +5.15 µs [-70.05, +81.92], Δ server CPU per statement +0.38 µs [-0.51, +1.73]; lo p50 999.5 µs, hi p50 1001.3 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 -12.65 µs [-23.05, +11.45].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +6.65 µs, robust sd 15.72 µs, sd 14.84 µs; replicate arm B (INSERT again − INSERT p50 per run) median +5.75 µs, robust sd 12.53 µs, sd 16.10 µs; control SHOW META Δ p50 -0.30 µs [-0.80, +1.65]; run-to-run sd of the per-run Δ p50, INSERT 7.85 µs, UPDATE 5.30 µs, SELECT 2.10 µs, ping 1.19 µs.

### strict, cores = 1, 1,000 rows

6 runs kept of 6 (runs 0-5); loadavg(1 min) before each run 1.95-5.99, after 1.82-5.23; busy of the other CPUs (6, 7 excluded) in kept runs 1.5%-2.6% (median 2.0%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 2%-3%); gate wait median 3 s; ping p50 reference 51.8 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 9,714 us, B 8,136 us, max A 15,374 us, B 9,166 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 3,600 | 884.0 µs | 972.8 µs | 1,007.1 µs | 1,058.1 µs | 1,735.6 µs | 4,877.9 µs | 1166.0 µs (sd 60.4) | 860 /s | 77.97 µs |
| INSERT, pk omitted | B | 3,600 | 886.6 µs | 976.0 µs | 1,012.3 µs | 1,063.5 µs | 1,980.9 µs | 5,001.4 µs | 1181.0 µs (sd 19.4) | 846 /s | 77.33 µs |
| INSERT again (replicate) | A | 3,600 | 836.4 µs | 979.8 µs | 1,015.6 µs | 1,068.9 µs | 1,794.0 µs | 4,784.1 µs | 1169.5 µs (sd 21.8) | 859 /s | 78.18 µs |
| INSERT again (replicate) | B | 3,600 | 884.0 µs | 978.5 µs | 1,014.4 µs | 1,072.5 µs | 1,924.3 µs | 5,008.4 µs | 1191.7 µs (sd 58.5) | 841 /s | 77.44 µs |
| INSERT, pk named (keys below 2^40) | A | 3,600 | 882.1 µs | 974.3 µs | 1,008.7 µs | 1,058.0 µs | 1,437.4 µs | 4,296.9 µs | 1134.0 µs (sd 28.6) | 885 /s | 78.72 µs |
| INSERT, pk named (keys below 2^40) | B | 3,600 | 879.6 µs | 976.0 µs | 1,010.5 µs | 1,064.0 µs | 1,683.1 µs | 5,357.5 µs | 1199.2 µs (sd 52.7) | 844 /s | 78.02 µs |
| SELECT by pk | A | 3,600 | 59.8 µs | 64.7 µs | 65.7 µs | 68.2 µs | 81.1 µs | 101.1 µs | 68.6 µs (sd 0.8) | 14,459 /s | 32.59 µs |
| SELECT by pk | B | 3,600 | 56.6 µs | 64.3 µs | 65.4 µs | 67.6 µs | 80.3 µs | 101.0 µs | 67.9 µs (sd 0.8) | 14,575 /s | 32.12 µs |
| UPDATE by pk | A | 3,600 | 879.6 µs | 981.0 µs | 1,016.2 µs | 1,063.3 µs | 1,429.7 µs | 4,322.1 µs | 1137.4 µs (sd 23.7) | 880 /s | 83.47 µs |
| UPDATE by pk | B | 3,600 | 878.9 µs | 981.6 µs | 1,018.8 µs | 1,070.9 µs | 1,872.5 µs | 6,530.9 µs | 1211.6 µs (sd 72.3) | 831 /s | 83.55 µs |
| UPDATE of one hot row | A | 3,600 | 870.1 µs | 976.2 µs | 1,013.7 µs | 1,067.5 µs | 1,890.8 µs | 4,639.0 µs | 1177.9 µs (sd 45.4) | 852 /s | 83.28 µs |
| UPDATE of one hot row | B | 3,600 | 875.9 µs | 975.5 µs | 1,012.5 µs | 1,068.2 µs | 1,888.2 µs | 4,964.5 µs | 1173.7 µs (sd 51.4) | 845 /s | 83.28 µs |
| DELETE by pk | A | 3,600 | 873.5 µs | 966.7 µs | 1,002.8 µs | 1,049.7 µs | 1,716.7 µs | 5,046.5 µs | 1180.6 µs (sd 64.8) | 862 /s | 78.95 µs |
| DELETE by pk | B | 3,600 | 858.5 µs | 967.9 µs | 1,002.2 µs | 1,047.5 µs | 1,471.6 µs | 4,218.5 µs | 1127.3 µs (sd 60.3) | 907 /s | 77.19 µs |
| COUNT/MIN/MAX over 100 ids | A | 3,600 | 137.3 µs | 143.6 µs | 147.3 µs | 156.8 µs | 172.8 µs | 196.5 µs | 151.7 µs (sd 1.7) | 6,560 /s | 114.82 µs |
| COUNT/MIN/MAX over 100 ids | B | 3,600 | 140.5 µs | 148.0 µs | 152.1 µs | 162.0 µs | 179.6 µs | 205.0 µs | 157.0 µs (sd 2.0) | 6,336 /s | 119.28 µs |
| SHOW META (control) | A | 3,600 | 44.5 µs | 50.0 µs | 52.1 µs | 54.2 µs | 65.9 µs | 74.8 µs | 53.6 µs (sd 0.6) | 18,554 /s | 32.50 µs |
| SHOW META (control) | B | 3,600 | 45.8 µs | 50.1 µs | 51.5 µs | 54.0 µs | 65.5 µs | 74.3 µs | 53.3 µs (sd 0.6) | 18,658 /s | 32.84 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 3,600 | 866.4 µs | 973.9 µs | 1,007.6 µs | 1,056.7 µs | 1,539.6 µs | 4,492.0 µs | 1147.1 µs (sd 25.7) | 869 /s | 77.98 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 3,600 | 883.6 µs | 974.3 µs | 1,008.8 µs | 1,057.2 µs | 1,647.6 µs | 4,854.6 µs | 1175.0 µs (sd 52.5) | 856 /s | 78.84 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 1007.4 µs | 1009.4 µs | 993 → 991 /s (-0.7 % [-1.5, +0.5]) | +6.80 µs [-4.55, +15.15] | +14.92 µs [-28.32, +56.77] | -0.59 µs [-1.25, -0.17] | -1.0 % [-6.9, +4.1] | 20.24 µs | no change resolved |
| INSERT again (replicate) | 1015.0 µs | 1014.9 µs | 985 → 985 /s (-0.1 % [-0.3, +0.6]) | +0.70 µs [-6.45, +3.35] | +22.15 µs [-28.30, +71.25] | -0.73 µs [-1.25, +0.73] | -1.4 % [-7.5, +4.1] | 20.24 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 1011.2 µs | 1009.9 µs | 989 → 990 /s (-0.6 % [-1.0, +0.9]) | +5.60 µs [-9.30, +10.00] | +65.17 µs [+23.13, +116.45] | -0.57 µs [-1.51, +0.21] | -4.7 % [-10.3, -0.8] | 20.24 µs | no change resolved |
| SELECT by pk | 65.8 µs | 65.5 µs | 15,186 → 15,279 /s (+0.3 % [-0.2, +1.2]) | -0.20 µs [-0.80, +0.10] | -0.60 µs [-1.20, -0.02] | -0.42 µs [-0.74, +0.22] | +0.7 % [-0.2, +2.2] | 20.24 µs | no change resolved |
| UPDATE by pk | 1013.2 µs | 1017.1 µs | 987 → 983 /s (-0.4 % [-0.7, +0.8]) | +3.85 µs [-8.00, +7.40] | +74.23 µs [+26.10, +132.93] | -0.21 µs [-1.66, +1.27] | -3.3 % [-12.6, -1.7] | 20.24 µs | no change resolved |
| UPDATE of one hot row | 1011.5 µs | 1010.0 µs | 989 → 990 /s (+0.3 % [-0.4, +0.4]) | -3.05 µs [-4.15, +4.30] | -4.13 µs [-17.53, +9.63] | -0.98 µs [-2.30, +1.34] | +0.6 % [-1.5, +2.0] | 20.24 µs | no change resolved |
| DELETE by pk | 995.5 µs | 1000.5 µs | 1,005 → 1,000 /s (-0.2 % [-0.5, +0.5]) | +1.75 µs [-5.10, +5.50] | -53.30 µs [-107.47, -4.47] | -1.06 µs [-2.40, +0.53] | +3.8 % [-0.8, +11.6] | 20.24 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 146.9 µs | 151.6 µs | 6,805 → 6,596 /s (-3.1 % [-3.4, -2.8]) | +4.70 µs [+4.30, +5.25] | +5.33 µs [+4.80, +6.00] | +4.69 µs [+3.92, +5.55] | -3.3 % [-3.8, -2.9] | 20.24 µs | CI excludes 0, inside the floor: not resolved |
| SHOW META (control) | 52.0 µs | 51.6 µs | 19,249 → 19,380 /s (+1.6 % [+0.4, +2.2]) | -0.80 µs [-1.15, -0.20] | -0.27 µs [-0.47, -0.03] | +0.78 µs [-0.07, +1.28] | +0.7 % [-0.1, +1.1] | 20.24 µs | CI excludes 0, inside the floor: not resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +1.45 µs [-4.35, +6.80], Δ mean +27.87 µs [-8.07, +66.33], Δ server CPU per statement +1.17 µs [+0.09, +1.78]; lo p50 1008.5 µs, hi p50 1010.0 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +1.20 µs [-11.00, +9.40].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +9.60 µs, robust sd 3.41 µs, sd 6.74 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.95 µs, robust sd 6.75 µs, sd 14.75 µs; control SHOW META Δ p50 -0.80 µs [-1.15, -0.20]; run-to-run sd of the per-run Δ p50, INSERT 9.62 µs, UPDATE 7.75 µs, SELECT 0.42 µs, ping 0.44 µs.

### strict, cores = 1, 10,000 rows

6 runs kept of 6 (runs 0-5); loadavg(1 min) before each run 1.01-6.88, after 1.01-5.64; busy of the other CPUs (6, 7 excluded) in kept runs 1.8%-8.7% (median 2.3%); CPUs 6,7 (another session's benchmark) busy over 10 % in 1 of the kept runs (range 2%-35%); gate wait median 3 s; ping p50 reference 50.9 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 8,056 us, B 8,396 us, max A 13,486 us, B 16,893 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 3,600 | 877.7 µs | 973.4 µs | 1,007.1 µs | 1,058.7 µs | 1,555.1 µs | 4,413.2 µs | 1150.0 µs (sd 18.8) | 866 /s | 79.01 µs |
| INSERT, pk omitted | B | 3,600 | 872.4 µs | 973.3 µs | 1,009.2 µs | 1,061.4 µs | 2,091.4 µs | 5,032.2 µs | 1189.0 µs (sd 43.8) | 849 /s | 77.54 µs |
| INSERT again (replicate) | A | 3,600 | 839.0 µs | 969.8 µs | 1,004.3 µs | 1,059.2 µs | 2,000.9 µs | 4,875.2 µs | 1283.6 µs (sd 208.5) | 822 /s | 80.58 µs |
| INSERT again (replicate) | B | 3,600 | 857.3 µs | 969.1 µs | 1,003.7 µs | 1,055.0 µs | 1,669.0 µs | 4,690.5 µs | 1173.0 µs (sd 45.8) | 859 /s | 79.37 µs |
| INSERT, pk named (keys below 2^40) | A | 3,600 | 881.8 µs | 971.1 µs | 1,006.5 µs | 1,057.8 µs | 1,856.8 µs | 4,583.4 µs | 1240.2 µs (sd 171.3) | 850 /s | 78.62 µs |
| INSERT, pk named (keys below 2^40) | B | 3,600 | 865.1 µs | 972.0 µs | 1,006.6 µs | 1,062.7 µs | 2,063.5 µs | 4,559.4 µs | 1209.9 µs (sd 79.0) | 852 /s | 79.43 µs |
| SELECT by pk | A | 3,600 | 61.6 µs | 64.9 µs | 66.1 µs | 71.7 µs | 81.7 µs | 102.9 µs | 69.4 µs (sd 2.2) | 14,384 /s | 33.28 µs |
| SELECT by pk | B | 3,600 | 57.9 µs | 64.8 µs | 66.1 µs | 71.7 µs | 83.7 µs | 103.9 µs | 69.5 µs (sd 2.5) | 14,408 /s | 33.24 µs |
| UPDATE by pk | A | 3,600 | 888.1 µs | 975.9 µs | 1,009.8 µs | 1,058.9 µs | 1,799.7 µs | 5,011.9 µs | 1173.9 µs (sd 37.0) | 856 /s | 84.18 µs |
| UPDATE by pk | B | 3,600 | 881.7 µs | 975.3 µs | 1,009.1 µs | 1,058.0 µs | 1,657.7 µs | 4,686.8 µs | 1158.3 µs (sd 37.9) | 851 /s | 82.48 µs |
| UPDATE of one hot row | A | 3,600 | 875.5 µs | 970.9 µs | 1,004.5 µs | 1,052.2 µs | 1,670.4 µs | 4,690.0 µs | 1157.0 µs (sd 33.6) | 870 /s | 83.46 µs |
| UPDATE of one hot row | B | 3,600 | 879.3 µs | 970.2 µs | 1,007.2 µs | 1,065.8 µs | 2,129.3 µs | 5,599.9 µs | 1275.2 µs (sd 202.1) | 827 /s | 82.09 µs |
| DELETE by pk | A | 3,600 | 879.7 µs | 970.0 µs | 1,003.4 µs | 1,053.1 µs | 1,579.2 µs | 4,370.6 µs | 1137.0 µs (sd 44.9) | 875 /s | 78.01 µs |
| DELETE by pk | B | 3,600 | 867.5 µs | 970.0 µs | 1,003.4 µs | 1,053.0 µs | 1,683.8 µs | 4,662.3 µs | 1223.3 µs (sd 187.4) | 884 /s | 76.30 µs |
| COUNT/MIN/MAX over 100 ids | A | 3,600 | 736.5 µs | 784.8 µs | 809.4 µs | 834.7 µs | 874.2 µs | 1,050.2 µs | 823.3 µs (sd 14.7) | 1,214 /s | 766.61 µs |
| COUNT/MIN/MAX over 100 ids | B | 3,600 | 774.3 µs | 829.7 µs | 853.3 µs | 877.1 µs | 923.9 µs | 1,143.8 µs | 864.0 µs (sd 11.7) | 1,155 /s | 814.89 µs |
| SHOW META (control) | A | 3,600 | 45.0 µs | 49.7 µs | 51.0 µs | 53.7 µs | 64.0 µs | 73.5 µs | 55.6 µs (sd 6.8) | 18,777 /s | 32.90 µs |
| SHOW META (control) | B | 3,600 | 45.6 µs | 49.8 µs | 51.0 µs | 53.5 µs | 63.3 µs | 72.4 µs | 52.8 µs (sd 0.6) | 18,931 /s | 32.75 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 3,600 | 873.0 µs | 971.7 µs | 1,005.8 µs | 1,055.1 µs | 1,810.4 µs | 4,913.3 µs | 1175.9 µs (sd 20.5) | 854 /s | 76.68 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 3,600 | 863.4 µs | 969.9 µs | 1,005.1 µs | 1,054.3 µs | 1,631.3 µs | 5,237.9 µs | 1248.2 µs (sd 233.2) | 880 /s | 77.03 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 1007.6 µs | 1012.0 µs | 992 → 988 /s (-0.2 % [-0.6, +0.5]) | +2.15 µs [-4.70, +6.35] | +39.00 µs [+10.48, +67.52] | -1.08 µs [-1.81, -0.10] | -3.1 % [-6.7, +0.2] | 35.58 µs | no change resolved |
| INSERT again (replicate) | 1009.7 µs | 1003.1 µs | 990 → 997 /s (+0.1 % [-1.0, +0.9]) | -1.05 µs [-8.90, +10.40] | -110.57 µs [-289.40, +8.47] | -0.44 µs [-3.79, +1.44] | +4.5 % [-2.7, +27.0] | 35.58 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 1004.1 µs | 1006.3 µs | 996 → 994 /s (-0.1 % [-1.0, +1.2]) | +1.10 µs [-11.70, +10.10] | -30.25 µs [-205.32, +108.88] | +1.45 µs [-2.08, +4.16] | +0.6 % [-12.7, +21.9] | 35.58 µs | no change resolved |
| SELECT by pk | 65.9 µs | 65.9 µs | 15,175 → 15,175 /s (-0.0 % [-1.0, +1.6]) | +0.00 µs [-1.05, +0.65] | +0.08 µs [-1.13, +1.32] | -0.21 µs [-1.34, +0.98] | +0.1 % [-2.6, +2.2] | 35.58 µs | no change resolved |
| UPDATE by pk | 1008.2 µs | 1009.2 µs | 992 → 991 /s (+0.2 % [-0.7, +0.5]) | -2.25 µs [-4.95, +7.25] | -15.62 µs [-49.53, +23.97] | -1.38 µs [-1.83, -1.08] | +2.9 % [-3.7, +5.1] | 35.58 µs | no change resolved |
| UPDATE of one hot row | 1008.2 µs | 1012.4 µs | 992 → 988 /s (-0.1 % [-0.7, +0.4]) | +0.60 µs [-3.75, +7.35] | +118.20 µs [+9.73, +295.33] | -1.25 µs [-2.40, -0.09] | -4.5 % [-19.5, +1.2] | 35.58 µs | no change resolved |
| DELETE by pk | 999.3 µs | 1000.1 µs | 1,001 → 1,000 /s (+0.4 % [-1.0, +0.6]) | -4.10 µs [-6.30, +9.85] | +86.37 µs [-30.80, +251.87] | -1.71 µs [-2.11, -0.80] | -0.2 % [-20.5, +4.2] | 35.58 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 807.4 µs | 854.2 µs | 1,239 → 1,171 /s (-5.0 % [-6.0, -4.4]) | +42.50 µs [+37.15, +51.00] | +40.77 µs [+28.38, +54.28] | +44.96 µs [+36.25, +56.01] | -4.9 % [-6.7, -2.5] | 35.58 µs | **B slower** (+5.3 % at p50) |
| SHOW META (control) | 50.8 µs | 50.9 µs | 19,666 → 19,646 /s (-0.2 % [-1.0, +0.6]) | +0.10 µs [-0.30, +0.50] | -2.85 µs [-8.53, +0.10] | -0.09 µs [-0.70, +0.27] | +0.0 % [-0.2, +16.4] | 35.58 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 -0.80 µs [-5.10, +2.65], Δ mean +72.25 µs [-57.85, +256.92], Δ server CPU per statement +0.40 µs [-0.38, +1.21]; lo p50 1011.5 µs, hi p50 1007.9 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 -4.75 µs [-11.05, +9.45].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -3.25 µs, robust sd 6.45 µs, sd 7.35 µs; replicate arm B (INSERT again − INSERT p50 per run) median -6.40 µs, robust sd 11.86 µs, sd 10.44 µs; control SHOW META Δ p50 +0.10 µs [-0.30, +0.50]; run-to-run sd of the per-run Δ p50, INSERT 5.43 µs, UPDATE 5.77 µs, SELECT 0.95 µs, ping 0.41 µs.
