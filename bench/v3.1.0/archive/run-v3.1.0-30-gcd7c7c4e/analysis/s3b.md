### pk-user mean µs per run (the control: no row count, no index)
| cores | n | mode | run 1 | run 2 | run 3 | max/min-1 |
|---|---|---|---|---|---|---|
| 1 | 200 | none | 76.4 µs | 68.0 µs | 95.9 µs | 41.0 % |
| 1 | 200 | all | 66.5 µs | 65.3 µs | 71.4 µs | 9.3 % |
| 1 | 1,000 | none | 68.1 µs | 66.6 µs | 66.6 µs | 2.3 % |
| 1 | 1,000 | all | 66.1 µs | 67.6 µs | 66.8 µs | 2.3 % |
| 1 | 10,000 | none | 64.8 µs | 94.0 µs | 64.3 µs | 46.2 % |
| 1 | 10,000 | all | 67.8 µs | 94.1 µs | 65.9 µs | 42.8 % |
| 8 | 200 | none | 69.6 µs | 70.0 µs | 71.0 µs | 2.0 % |
| 8 | 200 | all | 71.1 µs | 70.6 µs | 69.9 µs | 1.7 % |
| 8 | 1,000 | none | 70.0 µs | 77.7 µs | 70.7 µs | 11.0 % |
| 8 | 1,000 | all | 74.5 µs | 71.4 µs | 72.3 µs | 4.3 % |
| 8 | 10,000 | none | 71.0 µs | 68.2 µs | 67.5 µs | 5.2 % |
| 8 | 10,000 | all | 68.3 µs | 69.6 µs | 68.8 µs | 1.9 % |

pk-user mean over 36 cells: min 64.3 median 69.6 max 95.9
cells above 85 µs: 3

### per-row fit, unindexed, cores 1: mean = a + b * rows, least squares over n = 200, 1000, 10000 (medians of 3), now / 9a0525d
| shape | rows walked per n | fixed a now | slope b now | fixed a 9a0525d | slope b 9a0525d | b ratio then/now |
|---|---|---|---|---|---|---|
| loans-by-user | 1 x n | 87.0 µs | 27 ns per row | 84.8 µs | 70 ns per row | 2.56x |
| loans-by-book | 1 x n | 75.1 µs | 29 ns per row | 78.3 µs | 71 ns per row | 2.43x |
| resv-by-user | 0.5 x n | 78.9 µs | 25 ns per row | 72.2 µs | 71 ns per row | 2.83x |
| books-by-author | 0.2 x n | 87.2 µs | 27 ns per row | 90.5 µs | 73 ns per row | 2.70x |
| books-by-genre | 0.2 x n | 76.4 µs | 198 ns per row | 71.1 µs | 268 ns per row | 1.36x |
| loans-by-daterange | 1 x n | 80.2 µs | 144 ns per row | 66.3 µs | 197 ns per row | 1.37x |
| overdue | 1 x n | 72.0 µs | 107 ns per row | 66.9 µs | 158 ns per row | 1.48x |
| join-loan-user | 1 x n | 107.8 µs | 28 ns per row | 93.5 µs | 70 ns per row | 2.47x |
| join-no-literal | 1 x n | 254.9 µs | 92 ns per row | 244.8 µs | 104 ns per row | 1.13x |
| exists-correlated | 1 x n | 121.5 µs | 51 ns per row | 121.8 µs | 52 ns per row | 1.02x |
| count-by-user | 1 x n | 58.2 µs | 29 ns per row | 64.8 µs | 69 ns per row | 2.42x |

### PG18 (reused) per-row slope from its qps medians (none columns: idx 0,2,4)
| loans-by-user | a 68.5 µs | b 34 ns per row |
| loans-by-book | a 65.8 µs | b 36 ns per row |
| resv-by-user | a 61.7 µs | b 34 ns per row |
| books-by-author | a 63.2 µs | b 40 ns per row |
| books-by-genre | a 57.3 µs | b 131 ns per row |
| loans-by-daterange | a 76.4 µs | b 87 ns per row |
| overdue | a 67.1 µs | b 81 ns per row |
| join-loan-user | a 97.5 µs | b 33 ns per row |
| join-no-literal | a 228.3 µs | b 45 ns per row |
| exists-correlated | a 167.2 µs | b 115 ns per row |
| count-by-user | a 67.5 µs | b 34 ns per row |

### load phase and create-index per run (cores 1): load mean µs, p50
200 none ['1198/1050', '1249/1082', '1313/1080'] ['-', '-', '-']
200 all ['1198/1028', '1268/1050', '1369/1048'] ['1232', '1219', '1221']
1000 none ['1198/1044', '1256/1080', '1247/1062'] ['-', '-', '-']
1000 all ['1222/1040', '1217/1047', '1227/1053'] ['1790', '2094', '3857']
10000 none ['1283/1077', '1251/1060', '1211/1036'] ['-', '-', '-']
10000 all ['1266/1067', '1244/1068', '1295/1059'] ['7607', '8012', '7237']

### cores 8 vs cores 1, same-shape medians: geometric mean of c8/c1 qps per column
200 none 1.126
200 all 0.935
1000 none 0.908
1000 all 0.965
10000 none 0.946
10000 all 0.965
