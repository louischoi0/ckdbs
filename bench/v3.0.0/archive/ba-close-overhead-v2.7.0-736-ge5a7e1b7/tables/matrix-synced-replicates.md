| shape | group cores 2 (first pass), 200 rows | group cores 2 (first pass), 1,000 rows | group cores 2 (first pass), 10,000 rows | group cores 1 (first pass), 200 rows | group cores 1 (first pass), 1,000 rows | group cores 1 (first pass), 10,000 rows | strict cores 1 (first pass), 200 rows | strict cores 1 (first pass), 1,000 rows | strict cores 1 (first pass), 10,000 rows |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | +31.65 µs [+18.00, +36.70]; -3.1 % tps [-4.4, -1.3] | +29.65 µs [+22.35, +35.80]; -2.4 % tps [-4.0, -0.6] | +20.60 µs [+5.50, +24.10]; -1.6 % tps [-6.2, +1.1] | +1.60 µs [-10.50, +13.40]; +0.0 % tps [-1.4, +1.2] | -1.15 µs [-4.50, +5.70]; -1.0 % tps [-5.2, +3.8] | +0.90 µs [-5.40, +6.90]; +1.8 % tps [+0.3, +4.9] | +1.10 µs [-2.60, +3.10]; +0.6 % tps [-2.1, +4.3] | +3.95 µs [-1.90, +8.40]; +0.1 % tps [-3.4, +1.6] | +1.55 µs [-2.40, +5.10]; -2.2 % tps [-8.8, +0.8] |
| INSERT again (replicate) | +31.80 µs [+18.70, +34.70]; -3.6 % tps [-4.5, -2.0] | +29.70 µs [+25.20, +39.00]; -2.8 % tps [-4.1, -0.9] | +27.05 µs [+9.10, +30.70]; -1.9 % tps [-3.8, +2.1] | +2.80 µs [-4.00, +21.30]; +0.2 % tps [-1.7, +2.1] | +1.55 µs [-5.10, +4.40]; -0.9 % tps [-2.1, +1.5] | +0.85 µs [-3.10, +5.50]; -1.2 % tps [-8.1, +0.2] | +1.05 µs [-1.40, +2.80]; +0.3 % tps [-1.6, +1.9] | -1.15 µs [-2.30, +2.90]; +1.3 % tps [-1.3, +10.0] | -3.00 µs [-9.50, +3.60]; +0.3 % tps [-10.5, +3.4] |
| SHOW META | +5.00 µs [+4.75, +5.35]; -12.1 % tps [-13.6, -10.7] | +4.80 µs [+4.70, +5.30]; -11.3 % tps [-12.3, -10.1] | +5.30 µs [+4.90, +7.10]; -12.3 % tps [-12.8, -10.9] | +5.20 µs [+5.10, +12.40]; -11.4 % tps [-12.7, -10.5] | +5.05 µs [+4.95, +5.40]; -10.0 % tps [-12.4, -6.1] | +5.60 µs [+5.20, +8.30]; -11.9 % tps [-13.2, -11.6] | +5.10 µs [+4.90, +5.20]; -11.6 % tps [-13.0, -9.8] | +4.95 µs [+4.60, +6.65]; -11.2 % tps [-12.6, -8.3] | +6.35 µs [+4.90, +8.10]; -12.0 % tps [-14.1, -6.3] |

Throughput, A then B (median of runs' ops/elapsed):

| shape | group cores 2 (first pass), 200 rows | group cores 2 (first pass), 1,000 rows | group cores 2 (first pass), 10,000 rows | group cores 1 (first pass), 200 rows | group cores 1 (first pass), 1,000 rows | group cores 1 (first pass), 10,000 rows | strict cores 1 (first pass), 200 rows | strict cores 1 (first pass), 1,000 rows | strict cores 1 (first pass), 10,000 rows |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 842 -> 816 tps | 827 -> 795 tps | 823 -> 811 tps | 677 -> 675 tps | 808 -> 798 tps | 752 -> 742 tps | 817 -> 819 tps | 824 -> 821 tps | 640 -> 623 tps |
| INSERT again (replicate) | 836 -> 814 tps | 835 -> 814 tps | 824 -> 810 tps | 678 -> 679 tps | 773 -> 760 tps | 807 -> 800 tps | 818 -> 826 tps | 811 -> 796 tps | 654 -> 714 tps |
| SHOW META | 21,278 -> 18,677 tps | 21,464 -> 18,958 tps | 21,650 -> 19,021 tps | 21,453 -> 18,950 tps | 20,983 -> 18,644 tps | 20,718 -> 17,799 tps | 21,642 -> 19,051 tps | 20,327 -> 17,596 tps | 21,362 -> 17,832 tps |

A's p50:

| shape | group cores 2 (first pass), 200 rows | group cores 2 (first pass), 1,000 rows | group cores 2 (first pass), 10,000 rows | group cores 1 (first pass), 200 rows | group cores 1 (first pass), 1,000 rows | group cores 1 (first pass), 10,000 rows | strict cores 1 (first pass), 200 rows | strict cores 1 (first pass), 1,000 rows | strict cores 1 (first pass), 10,000 rows |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 1014.8 µs | 1023.0 µs | 1019.1 µs | 1263.0 µs | 1033.2 µs | 1038.8 µs | 1023.8 µs | 1015.3 µs | 1053.8 µs |
| INSERT again (replicate) | 1011.0 µs | 1011.8 µs | 1018.4 µs | 1261.7 µs | 1029.4 µs | 1045.4 µs | 1029.2 µs | 1029.0 µs | 1056.8 µs |
| SHOW META | 45.4 µs | 45.4 µs | 45.1 µs | 45.7 µs | 45.7 µs | 45.7 µs | 45.3 µs | 45.6 µs | 45.6 µs |
