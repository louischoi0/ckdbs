
#### u1+u2 pooled, 1,000 rows: server CPU per statement, us (an insert/select-pk statement; update-range = one every-row UPDATE of 1,000 rows)

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| insert CPU us literal | 8 | 23.79 | 23.10 | 23.43 | -0.05 [-0.64, 0.68] | -0.2 % | 4 of 8 | 0.56 / 1.09 | inside |
| insert CPU us prepared | 8 | 21.88 | 22.05 | 22.19 | 0.38 [-0.11, 1.01] | +1.8 % | 3 of 8 | 0.27 / 0.84 | inside |
| select-pk CPU us literal | 8 | 30.71 | 30.21 | 30.55 | 0.27 [-1.23, 2.82] | +0.9 % | 3 of 8 | 1.18 / 4.18 | inside |
| select-pk CPU us prepared | 8 | 30.11 | 30.81 | 30.80 | 1.18 [-2.50, 4.77] | +3.9 % | 2 of 8 | 0.84 / 3.05 | inside |

#### u1+u2 pooled, 1,000 rows: statement wall latency, us; median over runs of each run's percentile

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | literal | A | 15984 | 41.1 | 43.2 | 43.9 | 54.7 | 80.2 | 20519 | 23.8 | 24.5 |
| insert | literal | A2 | 15984 | 41.0 | 43.2 | 43.8 | 55.7 | 88.6 | 19876 | 23.1 | 24.7 |
| insert | literal | B | 15984 | 37.5 | 43.3 | 43.9 | 55.2 | 81.2 | 16194 | 23.4 | 24.5 |
| insert | prepared | A | 15984 | 35.8 | 40.4 | 40.9 | 49.0 | 71.9 | 22686 | 21.9 | 22.3 |
| insert | prepared | A2 | 15984 | 34.7 | 40.5 | 41.0 | 49.1 | 71.4 | 22471 | 22.1 | 22.2 |
| insert | prepared | B | 15984 | 35.0 | 40.6 | 41.1 | 49.5 | 71.6 | 22578 | 22.2 | 22.2 |
| select-pk | literal | A | 15984 | 54.3 | 56.3 | 62.3 | 74.7 | 102.2 | 15464 | 30.7 | 32.9 |
| select-pk | literal | A2 | 15984 | 54.7 | 56.7 | 62.0 | 74.3 | 113.8 | 15508 | 30.2 | 32.5 |
| select-pk | literal | B | 15984 | 54.0 | 56.3 | 62.3 | 73.4 | 99.2 | 15300 | 30.5 | 32.8 |
| select-pk | prepared | A | 15984 | 51.0 | 53.2 | 57.4 | 69.1 | 140.7 | 16415 | 30.1 | 31.1 |
| select-pk | prepared | A2 | 15984 | 50.8 | 53.4 | 58.7 | 67.2 | 103.9 | 16680 | 30.8 | 30.0 |
| select-pk | prepared | B | 15984 | 51.3 | 53.4 | 58.7 | 66.9 | 89.7 | 16718 | 30.8 | 30.0 |

bad replies: 0; n runs 8
unclean windows: 0
host_before loadavg per run: ['10.88', '6.46', '5.45', '4.77', '5.36', '5.38', '3.16', '10.20'] competing count: [3, 2, 0, 0, 8, 2, 2, 3]

#### u1 alone: server CPU per statement, us (an insert/select-pk statement; update-range = one every-row UPDATE of 1,000 rows)

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| insert CPU us literal | 4 | 39.00 | 39.37 | 38.93 | -0.08 [-0.64, 0.45] | -0.2 % | 2 of 4 | 0.56 / 1.08 | inside |
| insert CPU us prepared | 4 | 21.88 | 22.13 | 22.19 | 0.21 [-0.11, 1.01] | +1.0 % | 2 of 4 | 0.51 / 0.84 | inside |
| select-pk CPU us literal | 4 | 30.71 | 30.21 | 30.37 | -0.02 [-1.23, 0.57] | -0.1 % | 2 of 4 | 1.06 / 1.24 | inside |
| select-pk CPU us prepared | 4 | 45.57 | 46.07 | 46.32 | 1.18 [-2.50, 4.06] | +2.6 % | 1 of 4 | 0.74 / 1.33 | inside |

#### u1 alone: statement wall latency, us; median over runs of each run's percentile

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | literal | A | 7992 | 50.6 | 81.1 | 99.3 | 163.4 | 586.9 | 8064 | 39.0 | 40.2 |
| insert | literal | A2 | 7992 | 53.1 | 80.4 | 95.2 | 189.9 | 585.6 | 7290 | 39.4 | 40.3 |
| insert | literal | B | 7992 | 50.2 | 81.2 | 95.8 | 169.6 | 568.6 | 7535 | 38.9 | 40.0 |
| insert | prepared | A | 7992 | 35.8 | 40.3 | 40.8 | 49.2 | 72.5 | 22410 | 21.9 | 22.3 |
| insert | prepared | A2 | 7992 | 34.7 | 40.6 | 41.1 | 49.1 | 70.6 | 22749 | 22.1 | 22.2 |
| insert | prepared | B | 7992 | 34.6 | 40.5 | 41.0 | 49.5 | 71.1 | 22721 | 22.2 | 22.2 |
| select-pk | literal | A | 7992 | 54.1 | 56.2 | 62.3 | 74.7 | 102.2 | 15464 | 30.7 | 32.9 |
| select-pk | literal | A2 | 7992 | 54.7 | 56.7 | 62.0 | 74.3 | 113.8 | 15508 | 30.2 | 32.5 |
| select-pk | literal | B | 7992 | 53.7 | 56.3 | 62.3 | 73.4 | 99.2 | 15300 | 30.4 | 32.8 |
| select-pk | prepared | A | 7992 | 65.8 | 103.1 | 131.4 | 340.5 | 643.5 | 5652 | 45.6 | 46.3 |
| select-pk | prepared | A2 | 7992 | 55.3 | 98.3 | 126.9 | 362.9 | 621.5 | 5057 | 46.1 | 47.6 |
| select-pk | prepared | B | 7992 | 58.8 | 92.6 | 113.5 | 288.1 | 594.8 | 6255 | 46.3 | 44.2 |

bad replies: 0; n runs 4
unclean windows: 0
host_before loadavg per run: ['10.88', '6.46', '5.45', '4.77'] competing count: [3, 2, 0, 0]

#### u2 alone: server CPU per statement, us (an insert/select-pk statement; update-range = one every-row UPDATE of 1,000 rows)

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| insert CPU us literal | 4 | 23.14 | 22.74 | 23.17 | -0.05 [-0.45, 0.68] | -0.2 % | 2 of 4 | 0.69 / 1.09 | inside |
| insert CPU us prepared | 4 | 21.85 | 21.99 | 22.16 | 0.38 [-0.09, 0.94] | +1.8 % | 1 of 4 | 0.27 / 0.50 | inside |
| select-pk CPU us literal | 4 | 32.38 | 33.90 | 33.61 | 0.27 [-0.35, 2.82] | +0.8 % | 1 of 4 | 1.62 / 4.18 | inside |
| select-pk CPU us prepared | 4 | 29.79 | 29.36 | 29.93 | 0.88 [-0.89, 4.77] | +3.0 % | 1 of 4 | 1.01 / 3.05 | inside |

#### u2 alone: statement wall latency, us; median over runs of each run's percentile

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | literal | A | 7992 | 38.5 | 43.1 | 43.7 | 53.9 | 77.5 | 20748 | 23.1 | 24.4 |
| insert | literal | A2 | 7992 | 40.9 | 43.0 | 43.8 | 55.0 | 79.9 | 20943 | 22.7 | 24.4 |
| insert | literal | B | 7992 | 37.5 | 43.3 | 43.9 | 54.1 | 79.5 | 16330 | 23.2 | 24.3 |
| insert | prepared | A | 7992 | 36.1 | 40.4 | 40.9 | 48.6 | 71.5 | 22820 | 21.8 | 22.3 |
| insert | prepared | A2 | 7992 | 34.7 | 40.5 | 41.0 | 50.9 | 74.4 | 22327 | 22.0 | 22.8 |
| insert | prepared | B | 7992 | 36.7 | 40.7 | 41.2 | 50.1 | 77.7 | 22446 | 22.2 | 22.5 |
| select-pk | literal | A | 7992 | 64.1 | 69.8 | 71.9 | 86.2 | 133.9 | 12617 | 32.4 | 45.0 |
| select-pk | literal | A2 | 7992 | 60.7 | 70.3 | 72.6 | 90.9 | 137.1 | 10762 | 33.9 | 44.8 |
| select-pk | literal | B | 7992 | 61.5 | 70.4 | 73.1 | 109.1 | 200.5 | 10234 | 33.6 | 45.3 |
| select-pk | prepared | A | 7992 | 51.0 | 53.2 | 56.4 | 68.7 | 114.4 | 16583 | 29.8 | 30.2 |
| select-pk | prepared | A2 | 7992 | 50.6 | 53.0 | 56.2 | 67.0 | 101.9 | 16830 | 29.4 | 29.8 |
| select-pk | prepared | B | 7992 | 51.0 | 53.4 | 56.8 | 66.9 | 88.3 | 16884 | 29.9 | 29.6 |

bad replies: 0; n runs 4
unclean windows: 0
host_before loadavg per run: ['5.36', '5.38', '3.16', '10.20'] competing count: [8, 2, 2, 3]

#### l1, B alone: positional vs column-listed INSERT, wall us, median over runs

| shape | path | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us |
|---|---|---|---|---|---|---|---|---|---|
| insert | literal | 7992 | 39.6 | 43.6 | 44.2 | 52.4 | 98.5 | 20404 | 23.0 |
| insert | prepared | 7992 | 39.5 | 41.1 | 41.5 | 46.7 | 69.5 | 22145 | 22.1 |
| insert-listed | literal | 7992 | 40.5 | 45.3 | 45.8 | 52.8 | 73.3 | 20368 | 23.8 |
| insert-listed | prepared | 7992 | 40.3 | 42.3 | 42.8 | 48.5 | 68.0 | 21738 | 23.3 |
listed - positional CPU us literal: median 1.26 [0.10, 1.72] n=4
listed - positional CPU us prepared: median 1.37 [-0.31, 1.90] n=4
