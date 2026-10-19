
#### u1-u4 pooled (18 runs), 1,000 rows: server CPU per statement, us (insert/select-pk: one statement; update-range: one every-row UPDATE of 1,000 rows)

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B above A | A' - A typical / largest abs gap | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| insert CPU us literal | 18 | 24.52 | 24.54 | 24.32 | -0.24 [-1.15, 1.12] | -1.0 % | 5 of 18 | 0.49 / 1.02 | inside |
| insert CPU us prepared | 18 | 23.93 | 23.98 | 23.75 | -0.21 [-0.97, 1.07] | -0.9 % | 6 of 18 | 0.48 / 1.06 | inside |
| insert-dropped CPU us literal | 18 | 26.90 | 26.89 | 26.63 | -0.17 [-1.45, 0.84] | -0.6 % | 6 of 18 | 0.44 / 1.49 | inside |
| insert-dropped CPU us prepared | 18 | 25.86 | 26.01 | 25.83 | -0.32 [-1.28, 1.01] | -1.3 % | 5 of 18 | 0.68 / 1.40 | inside |
| update-range CPU us literal | 18 | 820.86 | 789.97 | 815.63 | -11.62 [-88.87, 144.75] | -1.4 % | 6 of 18 | 27.55 / 124.61 | inside |
| update-range CPU us prepared | 18 | 811.89 | 829.95 | 801.37 | -15.01 [-39.77, 93.06] | -1.8 % | 6 of 18 | 28.31 / 273.74 | inside |

#### u1-u4 pooled (18 runs), 1,000 rows: statement wall latency, us; median over runs of each run's percentile

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | literal | A | 35964 | 40.8 | 45.2 | 45.9 | 55.1 | 76.5 | 20307 | 24.5 | 24.8 |
| insert | literal | A2 | 35964 | 39.9 | 45.4 | 45.9 | 54.9 | 76.7 | 20332 | 24.5 | 24.8 |
| insert | literal | B | 35964 | 40.0 | 45.2 | 45.8 | 54.6 | 77.3 | 20360 | 24.3 | 24.8 |
| insert | prepared | A | 35964 | 38.9 | 42.7 | 43.3 | 52.1 | 75.3 | 21552 | 23.9 | 22.7 |
| insert | prepared | A2 | 35964 | 37.9 | 42.8 | 43.4 | 52.4 | 76.1 | 21424 | 24.0 | 22.7 |
| insert | prepared | B | 35964 | 38.2 | 42.4 | 43.0 | 52.0 | 74.3 | 21590 | 23.8 | 22.7 |
| insert-dropped | literal | A | 35964 | 45.4 | 47.9 | 48.5 | 59.0 | 85.2 | 19173 | 26.9 | 25.5 |
| insert-dropped | literal | A2 | 35964 | 43.5 | 48.1 | 48.7 | 58.4 | 84.1 | 19170 | 26.9 | 25.5 |
| insert-dropped | literal | B | 35964 | 44.0 | 47.8 | 48.4 | 57.5 | 86.1 | 19276 | 26.6 | 25.5 |
| insert-dropped | prepared | A | 35964 | 39.5 | 44.5 | 45.1 | 55.0 | 78.6 | 20670 | 25.9 | 22.8 |
| insert-dropped | prepared | A2 | 35964 | 41.5 | 44.6 | 45.1 | 55.2 | 80.9 | 20544 | 26.0 | 22.9 |
| insert-dropped | prepared | B | 35964 | 39.7 | 44.4 | 45.0 | 54.3 | 80.4 | 20681 | 25.8 | 22.9 |
| update-range | literal | A | 2160 | 724.2 | 756.2 | 770.6 | 1026.2 | 1441.3 | 1157 | 820.9 | 39.0 |
| update-range | literal | A2 | 2160 | 724.1 | 756.8 | 768.7 | 954.4 | 1619.1 | 1196 | 790.0 | 38.9 |
| update-range | literal | B | 2160 | 709.0 | 740.6 | 759.6 | 1042.0 | 1440.9 | 1139 | 815.6 | 39.3 |
| update-range | prepared | A | 2160 | 720.1 | 752.9 | 766.5 | 995.7 | 1802.5 | 1162 | 811.9 | 35.3 |
| update-range | prepared | A2 | 2160 | 722.5 | 754.7 | 768.1 | 1046.2 | 2361.3 | 1148 | 830.0 | 34.8 |
| update-range | prepared | B | 2160 | 704.6 | 734.4 | 746.6 | 1009.2 | 2162.1 | 1184 | 801.4 | 34.6 |

bad replies: 0; end_state all ok: True; n runs 18; unclean cells (per arm-record): 0

#### update-range and insert-dropped by driver invocation: B - A and A' - A, server CPU us per statement, paired by run

| file | start order | shape | path | runs | A | A' | B | B - A median [min, max] | A' - A median [min, max] |
|---|---|---|---|---|---|---|---|---|---|
| u1 | B, A', A | update-range | literal | 3 | 746.5 | 751.5 | 757.0 | -8.3 [-9.9, +10.5] | +5.0 [-5.4, +10.1] |
| u1 | B, A', A | update-range | prepared | 3 | 753.0 | 821.6 | 800.3 | -21.1 [-25.6, +93.1] | +18.9 [-5.6, +83.1] |
| u1 | B, A', A | insert-dropped | literal | 3 | 26.5 | 26.1 | 26.5 | +0.1 [-1.5, +0.8] | +0.4 [-1.5, +0.8] |
| u1 | B, A', A | insert-dropped | prepared | 3 | 25.8 | 25.5 | 25.8 | +0.0 [-1.0, +1.0] | +0.6 [-1.4, +0.7] |
| u2 | A, A', B | update-range | literal | 3 | 742.2 | 771.2 | 819.1 | -11.6 [-11.7, +97.8] | +6.1 [-15.4, +29.0] |
| u2 | A, A', B | update-range | prepared | 3 | 816.0 | 838.3 | 802.4 | -11.1 [-13.6, +67.1] | +0.8 [-0.8, +22.3] |
| u2 | A, A', B | insert-dropped | literal | 3 | 26.9 | 26.9 | 26.8 | -0.3 [-0.3, +0.2] | -0.0 [-0.2, +0.4] |
| u2 | A, A', B | insert-dropped | prepared | 3 | 26.1 | 26.2 | 26.0 | -0.1 [-0.3, +0.3] | +0.1 [-0.8, +0.4] |
| u3 | A', B, A | update-range | literal | 6 | 838.8 | 823.7 | 826.8 | -19.3 [-88.9, +56.8] | +14.2 [-89.8, +124.6] |
| u3 | A', B, A | update-range | prepared | 6 | 823.6 | 820.0 | 822.8 | -16.0 [-38.2, +38.6] | -13.0 [-32.8, +32.4] |
| u3 | A', B, A | insert-dropped | literal | 6 | 26.9 | 26.8 | 26.6 | -0.2 [-0.8, +0.8] | +0.2 [-1.2, +0.8] |
| u3 | A', B, A | insert-dropped | prepared | 6 | 25.8 | 25.9 | 25.5 | -0.4 [-0.9, +1.0] | +0.1 [-0.9, +0.8] |
| u4 | B, A, A' | update-range | literal | 6 | 866.1 | 902.1 | 875.2 | -17.7 [-36.9, +144.8] | -3.1 [-73.9, +98.7] |
| u4 | B, A, A' | update-range | prepared | 6 | 818.6 | 869.7 | 825.6 | -17.1 [-39.8, +58.4] | -22.5 [-273.7, +227.9] |
| u4 | B, A, A' | insert-dropped | literal | 6 | 26.9 | 26.9 | 26.9 | -0.2 [-1.3, +0.8] | +0.0 [-0.5, +0.9] |
| u4 | B, A, A' | insert-dropped | prepared | 6 | 25.9 | 26.3 | 25.6 | -0.4 [-1.3, +0.6] | +0.3 [-0.8, +1.1] |

#### u1+u2 pooled, 1,000 rows: server CPU per statement, us (insert/select-pk: one statement; update-range: one every-row UPDATE of 1,000 rows)

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B above A | A' - A typical / largest abs gap | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| insert CPU us literal | 6 | 24.66 | 24.37 | 24.35 | -0.15 [-0.87, 0.35] | -0.6 % | 2 of 6 | 0.46 / 0.89 | inside |
| insert CPU us prepared | 6 | 23.93 | 23.68 | 23.75 | 0.01 [-0.97, 0.51] | +0.0 % | 3 of 6 | 0.47 / 1.06 | inside |
| insert-dropped CPU us literal | 6 | 26.79 | 26.89 | 26.58 | -0.08 [-1.45, 0.83] | -0.3 % | 3 of 6 | 0.40 / 1.49 | inside |
| insert-dropped CPU us prepared | 6 | 26.04 | 25.83 | 25.92 | -0.05 [-1.03, 1.00] | -0.2 % | 3 of 6 | 0.67 / 1.40 | inside |
| insert-plain CPU us literal | 6 | 26.66 | 26.60 | 26.44 | 0.01 [-1.10, 0.72] | +0.0 % | 3 of 6 | 0.74 / 1.11 | inside |
| insert-plain CPU us prepared | 6 | 25.76 | 25.49 | 25.78 | 0.07 [-1.31, 1.19] | +0.3 % | 3 of 6 | 0.71 / 1.68 | inside |
| insert-spare CPU us literal | 6 | 26.83 | 26.49 | 26.50 | -0.03 [-1.22, 1.02] | -0.1 % | 3 of 6 | 0.68 / 1.25 | inside |
| insert-spare CPU us prepared | 6 | 25.80 | 25.71 | 25.79 | -0.08 [-1.33, 0.76] | -0.3 % | 3 of 6 | 0.50 / 1.64 | inside |
| select-pk CPU us literal | 6 | 33.52 | 33.10 | 33.10 | -0.37 [-1.83, 0.28] | -1.1 % | 2 of 6 | 0.40 / 1.90 | inside |
| select-pk CPU us prepared | 6 | 29.99 | 29.41 | 28.93 | -0.38 [-1.78, 0.62] | -1.3 % | 1 of 6 | 0.68 / 1.68 | inside |
| update-range CPU us literal | 6 | 744.37 | 761.33 | 784.57 | -9.08 [-11.66, 97.79] | -1.2 % | 2 of 6 | 8.06 / 28.96 | inside |
| update-range CPU us prepared | 6 | 784.50 | 829.95 | 801.37 | -12.32 [-25.59, 93.06] | -1.6 % | 2 of 6 | 12.26 / 83.06 | inside |

#### u1+u2 pooled, 1,000 rows: statement wall latency, us; median over runs of each run's percentile

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | literal | A | 11988 | 38.6 | 45.3 | 45.9 | 54.6 | 76.1 | 20498 | 24.7 | 24.7 |
| insert | literal | A2 | 11988 | 39.7 | 45.2 | 45.7 | 54.2 | 76.4 | 20521 | 24.4 | 24.8 |
| insert | literal | B | 11988 | 39.4 | 45.3 | 45.8 | 54.2 | 76.5 | 20473 | 24.3 | 24.7 |
| insert | prepared | A | 11988 | 38.9 | 42.9 | 43.4 | 51.6 | 73.1 | 21678 | 23.9 | 22.6 |
| insert | prepared | A2 | 11988 | 37.9 | 42.7 | 43.2 | 51.2 | 75.8 | 21500 | 23.7 | 22.7 |
| insert | prepared | B | 11988 | 37.5 | 42.9 | 43.4 | 51.1 | 73.2 | 21644 | 23.8 | 22.6 |
| insert-dropped | literal | A | 11988 | 44.6 | 47.9 | 48.5 | 58.5 | 82.4 | 19231 | 26.8 | 25.4 |
| insert-dropped | literal | A2 | 11988 | 43.0 | 47.9 | 48.6 | 58.0 | 83.2 | 19211 | 26.9 | 25.5 |
| insert-dropped | literal | B | 11988 | 42.0 | 47.9 | 48.5 | 57.4 | 80.7 | 19312 | 26.6 | 25.5 |
| insert-dropped | prepared | A | 11988 | 39.5 | 44.5 | 45.2 | 55.5 | 78.3 | 20548 | 26.0 | 22.9 |
| insert-dropped | prepared | A2 | 11988 | 41.6 | 44.5 | 45.2 | 55.7 | 78.5 | 20417 | 25.8 | 23.2 |
| insert-dropped | prepared | B | 11988 | 40.6 | 44.6 | 45.3 | 55.5 | 76.3 | 20367 | 25.9 | 23.0 |
| insert-plain | literal | A | 11988 | 41.0 | 47.7 | 48.2 | 57.1 | 79.3 | 19451 | 26.7 | 25.4 |
| insert-plain | literal | A2 | 11988 | 42.1 | 47.5 | 48.2 | 58.8 | 78.6 | 19418 | 26.6 | 25.4 |
| insert-plain | literal | B | 11988 | 41.8 | 47.6 | 48.2 | 57.3 | 78.0 | 19451 | 26.4 | 25.4 |
| insert-plain | prepared | A | 11988 | 38.1 | 44.2 | 44.9 | 53.9 | 74.6 | 20674 | 25.8 | 22.7 |
| insert-plain | prepared | A2 | 11988 | 39.4 | 44.2 | 44.9 | 54.1 | 76.3 | 20655 | 25.5 | 22.7 |
| insert-plain | prepared | B | 11988 | 38.8 | 44.4 | 45.1 | 54.3 | 77.0 | 20759 | 25.8 | 22.8 |
| insert-spare | literal | A | 11988 | 42.3 | 47.7 | 48.3 | 57.6 | 78.4 | 19458 | 26.8 | 25.3 |
| insert-spare | literal | A2 | 11988 | 42.6 | 47.7 | 48.4 | 57.9 | 79.6 | 19365 | 26.5 | 25.5 |
| insert-spare | literal | B | 11988 | 41.7 | 47.7 | 48.3 | 57.5 | 80.4 | 19350 | 26.5 | 25.4 |
| insert-spare | prepared | A | 11988 | 40.5 | 44.3 | 45.0 | 54.5 | 80.2 | 20617 | 25.8 | 22.8 |
| insert-spare | prepared | A2 | 11988 | 40.5 | 44.3 | 44.8 | 55.0 | 77.7 | 20704 | 25.7 | 22.8 |
| insert-spare | prepared | B | 11988 | 39.9 | 44.5 | 45.1 | 54.0 | 79.1 | 20529 | 25.8 | 22.7 |
| select-pk | literal | A | 11988 | 56.4 | 58.6 | 64.4 | 73.0 | 91.8 | 15288 | 33.5 | 32.6 |
| select-pk | literal | A2 | 11988 | 55.9 | 58.4 | 63.9 | 73.1 | 93.6 | 15376 | 33.1 | 32.6 |
| select-pk | literal | B | 11988 | 55.8 | 58.4 | 64.0 | 72.7 | 93.7 | 15332 | 33.1 | 32.5 |
| select-pk | prepared | A | 11988 | 53.5 | 55.3 | 56.7 | 66.4 | 82.6 | 16766 | 30.0 | 29.9 |
| select-pk | prepared | A2 | 11988 | 52.9 | 54.8 | 56.3 | 65.2 | 83.0 | 16738 | 29.4 | 29.9 |
| select-pk | prepared | B | 11988 | 52.8 | 54.9 | 55.9 | 65.8 | 82.9 | 17019 | 28.9 | 30.0 |
| update-range | literal | A | 720 | 718.1 | 745.6 | 758.2 | 874.0 | 1001.9 | 1276 | 744.4 | 37.8 |
| update-range | literal | A2 | 720 | 721.0 | 749.0 | 761.9 | 900.6 | 1049.5 | 1248 | 761.3 | 38.0 |
| update-range | literal | B | 720 | 709.0 | 732.7 | 744.7 | 879.7 | 1169.6 | 1193 | 784.6 | 38.4 |
| update-range | prepared | A | 720 | 714.5 | 742.2 | 754.9 | 876.9 | 1595.3 | 1213 | 784.5 | 35.1 |
| update-range | prepared | A2 | 720 | 718.0 | 744.8 | 756.9 | 900.8 | 2361.3 | 1148 | 830.0 | 34.1 |
| update-range | prepared | B | 720 | 702.5 | 731.1 | 743.1 | 909.2 | 2162.1 | 1200 | 801.4 | 33.8 |

bad replies: 0; end_state all ok: True; n runs 6; unclean cells (per arm-record): 0

#### within-arm: insert-dropped minus insert-spare, server CPU us per statement (pooled u1+u2, paired by run)

| path | arm | n | insert-plain | insert-spare | insert-dropped | dropped - spare, median [min, max] | % |
|---|---|---|---|---|---|---|---|
| literal | A | 6 | 26.66 | 26.83 | 26.79 | +0.22 [-0.64, +0.38] | +0.8 % |
| literal | A2 | 6 | 26.60 | 26.49 | 26.89 | +0.26 [-0.31, +0.53] | +1.0 % |
| literal | B | 6 | 26.44 | 26.50 | 26.58 | +0.12 [-0.74, +0.33] | +0.5 % |
| prepared | A | 6 | 25.76 | 25.80 | 26.04 | +0.15 [-0.48, +0.30] | +0.6 % |
| prepared | A2 | 6 | 25.49 | 25.71 | 25.83 | +0.22 [-0.11, +0.58] | +0.9 % |
| prepared | B | 6 | 25.78 | 25.79 | 25.92 | +0.25 [-0.24, +0.50] | +1.0 % |

#### u1 alone: server CPU per statement, us (insert/select-pk: one statement; update-range: one every-row UPDATE of 1,000 rows)

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B above A | A' - A typical / largest abs gap | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| insert CPU us literal | 3 | 24.02 | 24.03 | 24.26 | 0.25 [-0.87, 0.35] | +1.0 % | 2 of 3 | 0.75 / 0.89 | inside |
| insert CPU us prepared | 3 | 23.45 | 23.26 | 23.53 | 0.08 [-0.97, 0.51] | +0.3 % | 2 of 3 | 0.87 / 1.06 | inside |
| insert-dropped CPU us literal | 3 | 26.52 | 26.14 | 26.54 | 0.09 [-1.45, 0.83] | +0.3 % | 2 of 3 | 0.79 / 1.49 | inside |
| insert-dropped CPU us prepared | 3 | 25.82 | 25.50 | 25.83 | 0.01 [-1.03, 1.00] | +0.0 % | 2 of 3 | 0.74 / 1.40 | inside |
| insert-plain CPU us literal | 3 | 26.12 | 25.91 | 26.25 | 0.44 [-1.10, 0.72] | +1.7 % | 2 of 3 | 0.78 / 1.11 | inside |
| insert-plain CPU us prepared | 3 | 25.20 | 25.09 | 25.35 | 0.15 [-1.31, 1.19] | +0.6 % | 2 of 3 | 0.90 / 1.68 | inside |
| insert-spare CPU us literal | 3 | 26.17 | 26.08 | 26.35 | 0.18 [-1.22, 1.02] | +0.7 % | 2 of 3 | 0.77 / 1.25 | inside |
| insert-spare CPU us prepared | 3 | 25.52 | 25.62 | 25.35 | -0.18 [-1.33, 0.76] | -0.7 % | 1 of 3 | 0.76 / 1.64 | inside |
| select-pk CPU us literal | 3 | 33.48 | 33.04 | 33.09 | -0.38 [-0.46, 0.28] | -1.1 % | 1 of 3 | 0.49 / 0.52 | inside |
| select-pk CPU us prepared | 3 | 29.24 | 29.52 | 28.81 | -0.38 [-0.43, 0.62] | -1.3 % | 1 of 3 | 0.60 / 0.76 | inside |
| update-range CPU us literal | 3 | 746.51 | 751.47 | 757.00 | -8.28 [-9.87, 10.49] | -1.1 % | 1 of 3 | 5.43 / 10.06 | inside |
| update-range CPU us prepared | 3 | 752.99 | 821.64 | 800.30 | -21.05 [-25.59, 93.06] | -2.8 % | 1 of 3 | 18.92 / 83.06 | inside |

#### u1 alone: statement wall latency, us; median over runs of each run's percentile

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | literal | A | 5994 | 38.9 | 45.0 | 45.5 | 54.7 | 74.7 | 20565 | 24.0 | 24.9 |
| insert | literal | A2 | 5994 | 39.4 | 44.8 | 45.4 | 55.5 | 77.9 | 20484 | 24.0 | 25.0 |
| insert | literal | B | 5994 | 39.7 | 45.2 | 45.8 | 54.6 | 76.7 | 20374 | 24.3 | 24.9 |
| insert | prepared | A | 5994 | 37.5 | 42.3 | 42.8 | 50.5 | 72.1 | 22011 | 23.5 | 22.6 |
| insert | prepared | A2 | 5994 | 37.8 | 42.2 | 42.8 | 50.8 | 75.8 | 21905 | 23.3 | 22.6 |
| insert | prepared | B | 5994 | 36.9 | 42.4 | 43.0 | 51.3 | 72.0 | 21725 | 23.5 | 22.6 |
| insert-dropped | literal | A | 5994 | 43.8 | 47.5 | 48.0 | 57.9 | 82.7 | 19429 | 26.5 | 25.3 |
| insert-dropped | literal | A2 | 5994 | 41.5 | 47.2 | 47.7 | 57.0 | 82.3 | 19308 | 26.1 | 25.4 |
| insert-dropped | literal | B | 5994 | 41.4 | 47.7 | 48.3 | 57.3 | 79.9 | 19313 | 26.5 | 25.2 |
| insert-dropped | prepared | A | 5994 | 39.2 | 44.2 | 44.9 | 55.2 | 76.5 | 20673 | 25.8 | 22.6 |
| insert-dropped | prepared | A2 | 5994 | 41.5 | 43.9 | 44.5 | 53.8 | 77.9 | 20986 | 25.5 | 22.7 |
| insert-dropped | prepared | B | 5994 | 40.7 | 44.5 | 45.1 | 54.6 | 76.4 | 20765 | 25.8 | 22.8 |
| insert-plain | literal | A | 5994 | 39.6 | 47.2 | 47.7 | 56.7 | 78.4 | 19535 | 26.1 | 25.3 |
| insert-plain | literal | A2 | 5994 | 41.7 | 47.0 | 47.5 | 56.1 | 77.8 | 19794 | 25.9 | 25.2 |
| insert-plain | literal | B | 5994 | 40.5 | 47.6 | 48.2 | 57.2 | 75.9 | 19552 | 26.3 | 25.4 |
| insert-plain | prepared | A | 5994 | 36.3 | 43.9 | 44.4 | 52.4 | 72.7 | 20774 | 25.2 | 22.7 |
| insert-plain | prepared | A2 | 5994 | 41.4 | 43.7 | 44.3 | 52.2 | 74.2 | 21206 | 25.1 | 22.6 |
| insert-plain | prepared | B | 5994 | 38.2 | 44.1 | 44.7 | 53.5 | 72.8 | 20994 | 25.3 | 22.7 |
| insert-spare | literal | A | 5994 | 43.2 | 47.3 | 47.8 | 56.8 | 77.1 | 19647 | 26.2 | 25.3 |
| insert-spare | literal | A2 | 5994 | 41.5 | 47.1 | 47.6 | 57.8 | 81.5 | 19530 | 26.1 | 25.4 |
| insert-spare | literal | B | 5994 | 40.9 | 47.6 | 48.1 | 57.4 | 82.5 | 19465 | 26.3 | 25.3 |
| insert-spare | prepared | A | 5994 | 37.3 | 44.1 | 44.6 | 54.4 | 75.6 | 20958 | 25.5 | 22.8 |
| insert-spare | prepared | A2 | 5994 | 40.0 | 43.9 | 44.5 | 54.9 | 77.7 | 20645 | 25.6 | 22.7 |
| insert-spare | prepared | B | 5994 | 39.2 | 44.1 | 44.7 | 52.8 | 80.9 | 20581 | 25.3 | 22.7 |
| select-pk | literal | A | 5994 | 56.0 | 58.4 | 64.5 | 73.1 | 93.0 | 15299 | 33.5 | 32.9 |
| select-pk | literal | A2 | 5994 | 56.0 | 58.3 | 63.9 | 72.8 | 92.9 | 15398 | 33.0 | 33.1 |
| select-pk | literal | B | 5994 | 56.0 | 58.5 | 64.1 | 72.8 | 94.8 | 15321 | 33.1 | 33.0 |
| select-pk | prepared | A | 5994 | 53.0 | 55.1 | 56.2 | 65.4 | 82.6 | 16900 | 29.2 | 29.9 |
| select-pk | prepared | A2 | 5994 | 52.4 | 54.4 | 56.0 | 65.6 | 86.0 | 16523 | 29.5 | 29.9 |
| select-pk | prepared | B | 5994 | 52.7 | 54.8 | 55.8 | 66.6 | 84.7 | 16953 | 28.8 | 30.1 |
| update-range | literal | A | 360 | 719.1 | 746.8 | 758.1 | 890.5 | 1081.0 | 1268 | 746.5 | 37.9 |
| update-range | literal | A2 | 360 | 718.7 | 750.6 | 762.6 | 886.1 | 1019.4 | 1261 | 751.5 | 39.0 |
| update-range | literal | B | 360 | 712.3 | 734.1 | 744.7 | 867.8 | 1067.0 | 1259 | 757.0 | 39.7 |
| update-range | prepared | A | 360 | 714.4 | 741.7 | 751.7 | 861.5 | 1027.1 | 1267 | 753.0 | 35.0 |
| update-range | prepared | A2 | 360 | 720.3 | 743.2 | 758.3 | 905.0 | 2372.4 | 1147 | 821.6 | 33.9 |
| update-range | prepared | B | 360 | 700.5 | 728.3 | 740.4 | 894.2 | 2091.4 | 1206 | 800.3 | 32.8 |

bad replies: 0; end_state all ok: True; n runs 3; unclean cells (per arm-record): 0

#### u2 alone: server CPU per statement, us (insert/select-pk: one statement; update-range: one every-row UPDATE of 1,000 rows)

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B above A | A' - A typical / largest abs gap | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| insert CPU us literal | 3 | 24.72 | 24.42 | 24.48 | -0.24 [-0.65, -0.06] | -1.0 % | 0 of 3 | 0.17 / 0.74 | inside |
| insert CPU us prepared | 3 | 23.94 | 23.88 | 23.94 | -0.07 [-0.47, 0.12] | -0.3 % | 1 of 3 | 0.07 / 0.92 | inside |
| insert-dropped CPU us literal | 3 | 26.89 | 26.93 | 26.84 | -0.26 [-0.33, 0.16] | -1.0 % | 1 of 3 | 0.16 / 0.37 | inside |
| insert-dropped CPU us prepared | 3 | 26.11 | 26.16 | 26.00 | -0.11 [-0.33, 0.27] | -0.4 % | 1 of 3 | 0.41 / 0.78 | inside |
| insert-plain CPU us literal | 3 | 27.01 | 27.09 | 26.70 | -0.26 [-0.70, 0.29] | -1.0 % | 1 of 3 | 0.71 / 0.99 | inside |
| insert-plain CPU us prepared | 3 | 26.21 | 25.61 | 25.97 | -0.00 [-0.25, 0.53] | -0.0 % | 1 of 3 | 0.30 / 0.86 | inside |
| insert-spare CPU us literal | 3 | 27.22 | 26.58 | 26.75 | -0.24 [-0.71, 0.26] | -0.9 % | 1 of 3 | 0.37 / 0.82 | inside |
| insert-spare CPU us prepared | 3 | 25.89 | 25.81 | 25.83 | 0.02 [-0.42, 0.17] | +0.1 % | 2 of 3 | 0.25 / 0.93 | inside |
| select-pk CPU us literal | 3 | 33.67 | 33.16 | 33.23 | -0.36 [-1.83, 0.03] | -1.1 % | 1 of 3 | 0.31 / 1.90 | inside |
| select-pk CPU us prepared | 3 | 30.55 | 29.30 | 29.04 | -0.39 [-1.78, -0.15] | -1.3 % | 0 of 3 | 0.77 / 1.68 | inside |
| update-range CPU us literal | 3 | 742.24 | 771.19 | 819.11 | -11.59 [-11.66, 97.79] | -1.6 % | 1 of 3 | 15.37 / 28.96 | inside |
| update-range CPU us prepared | 3 | 816.01 | 838.27 | 802.45 | -11.07 [-13.56, 67.08] | -1.4 % | 1 of 3 | 0.82 / 22.27 | inside |

#### u2 alone: statement wall latency, us; median over runs of each run's percentile

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | literal | A | 5994 | 38.2 | 45.4 | 45.9 | 54.6 | 76.4 | 20491 | 24.7 | 24.6 |
| insert | literal | A2 | 5994 | 39.9 | 45.4 | 45.9 | 54.1 | 76.1 | 20558 | 24.4 | 24.5 |
| insert | literal | B | 5994 | 39.1 | 45.4 | 45.9 | 53.5 | 76.3 | 20572 | 24.5 | 24.5 |
| insert | prepared | A | 5994 | 40.0 | 42.9 | 43.4 | 51.7 | 74.0 | 21673 | 23.9 | 22.7 |
| insert | prepared | A2 | 5994 | 38.5 | 43.0 | 43.5 | 51.2 | 75.9 | 21444 | 23.9 | 22.7 |
| insert | prepared | B | 5994 | 38.1 | 43.0 | 43.5 | 51.0 | 74.5 | 21595 | 23.9 | 22.6 |
| insert-dropped | literal | A | 5994 | 45.7 | 48.1 | 48.7 | 59.0 | 82.4 | 19038 | 26.9 | 25.5 |
| insert-dropped | literal | A2 | 5994 | 43.6 | 48.2 | 48.7 | 59.7 | 86.2 | 19107 | 26.9 | 25.6 |
| insert-dropped | literal | B | 5994 | 43.2 | 48.0 | 48.6 | 57.6 | 81.4 | 19311 | 26.8 | 25.5 |
| insert-dropped | prepared | A | 5994 | 40.8 | 44.5 | 45.4 | 55.9 | 78.3 | 20425 | 26.1 | 23.4 |
| insert-dropped | prepared | A2 | 5994 | 41.7 | 44.7 | 45.3 | 57.3 | 81.1 | 20194 | 26.2 | 23.4 |
| insert-dropped | prepared | B | 5994 | 40.5 | 44.7 | 45.3 | 56.2 | 74.7 | 20147 | 26.0 | 23.3 |
| insert-plain | literal | A | 5994 | 41.0 | 47.9 | 48.5 | 60.6 | 80.3 | 19188 | 27.0 | 25.4 |
| insert-plain | literal | A2 | 5994 | 42.2 | 47.9 | 48.5 | 60.4 | 80.1 | 19108 | 27.1 | 25.6 |
| insert-plain | literal | B | 5994 | 43.4 | 47.8 | 48.4 | 57.4 | 78.6 | 19432 | 26.7 | 25.5 |
| insert-plain | prepared | A | 5994 | 38.8 | 44.3 | 45.0 | 55.4 | 75.4 | 20529 | 26.2 | 22.8 |
| insert-plain | prepared | A2 | 5994 | 37.2 | 44.5 | 45.1 | 55.7 | 78.2 | 20495 | 25.6 | 22.9 |
| insert-plain | prepared | B | 5994 | 39.4 | 44.6 | 45.1 | 55.2 | 77.9 | 20521 | 26.0 | 22.9 |
| insert-spare | literal | A | 5994 | 41.4 | 47.9 | 48.6 | 59.4 | 84.0 | 18769 | 27.2 | 25.3 |
| insert-spare | literal | A2 | 5994 | 43.3 | 48.0 | 48.5 | 59.2 | 77.4 | 19343 | 26.6 | 25.6 |
| insert-spare | literal | B | 5994 | 41.7 | 47.7 | 48.4 | 59.7 | 79.7 | 19269 | 26.7 | 25.6 |
| insert-spare | prepared | A | 5994 | 42.0 | 44.4 | 45.0 | 54.6 | 81.6 | 20429 | 25.9 | 22.9 |
| insert-spare | prepared | A2 | 5994 | 40.8 | 44.5 | 45.0 | 55.4 | 79.9 | 20764 | 25.8 | 23.0 |
| insert-spare | prepared | B | 5994 | 40.6 | 44.5 | 45.2 | 54.7 | 77.4 | 20151 | 25.8 | 22.9 |
| select-pk | literal | A | 5994 | 56.7 | 58.8 | 64.4 | 72.9 | 90.7 | 15276 | 33.7 | 32.5 |
| select-pk | literal | A2 | 5994 | 55.8 | 58.8 | 63.9 | 73.3 | 94.3 | 15355 | 33.2 | 32.5 |
| select-pk | literal | B | 5994 | 55.7 | 58.3 | 64.0 | 72.5 | 92.5 | 15344 | 33.2 | 32.5 |
| select-pk | prepared | A | 5994 | 53.9 | 55.8 | 57.1 | 67.3 | 82.6 | 16634 | 30.6 | 29.9 |
| select-pk | prepared | A2 | 5994 | 53.6 | 55.8 | 56.6 | 64.8 | 79.1 | 16959 | 29.3 | 29.9 |
| select-pk | prepared | B | 5994 | 53.0 | 55.2 | 56.0 | 64.9 | 81.0 | 17086 | 29.0 | 30.0 |
| update-range | literal | A | 360 | 717.3 | 745.0 | 758.3 | 845.6 | 916.5 | 1285 | 742.2 | 37.3 |
| update-range | literal | A2 | 360 | 723.3 | 747.4 | 761.2 | 915.1 | 1079.5 | 1235 | 771.2 | 37.2 |
| update-range | literal | B | 360 | 705.8 | 730.8 | 744.8 | 891.7 | 1272.3 | 1133 | 819.1 | 37.2 |
| update-range | prepared | A | 360 | 714.6 | 742.7 | 756.7 | 943.7 | 2163.5 | 1164 | 816.0 | 35.2 |
| update-range | prepared | A2 | 360 | 715.6 | 746.4 | 755.6 | 896.5 | 2350.2 | 1149 | 838.3 | 35.3 |
| update-range | prepared | B | 360 | 704.5 | 733.9 | 745.7 | 924.2 | 2232.7 | 1195 | 802.4 | 36.1 |

bad replies: 0; end_state all ok: True; n runs 3; unclean cells (per arm-record): 0
