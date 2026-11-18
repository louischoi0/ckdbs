#### headline: paired per-run server CPU us per statement, median [min, max] (runs with the first arm's cost exceeded by the second)

| cell | runs | A | B - A | C - A | B - C | A2 - A typical / largest abs | B - mean(A,A2) | C - mean(A,A2) | verdict B - A vs 0.5 us |
|---|---|---|---|---|---|---|---|---|---|
| select-pk literal | 16 | 48,865 /s (20.46 us) | +0.18 [-0.11, +1.00] (14 of 16) | +0.08 [-0.42, +0.61] (10 of 16) | +0.25 [-0.23, +0.91] (13 of 16) | 0.20 / 0.34 | +0.32 | +0.13 | not resolvable (median +0.18 +/- 0.34 straddles 0.5) |
| select-pk prepared | 14 | 50,573 /s (19.77 us) | +0.37 [-0.17, +1.29] (12 of 14) | +0.10 [-0.33, +1.39] (8 of 14) | +0.22 [-0.76, +1.06] (11 of 14) | 0.28 / 0.69 | +0.32 | +0.08 | not resolvable (median +0.37 +/- 0.69 straddles 0.5) |
| update-expr literal | 16 | 45,343 /s (22.05 us) | +0.23 [-0.13, +1.05] (15 of 16) | +0.14 [-0.39, +0.82] (12 of 16) | +0.07 [-0.61, +0.91] (10 of 16) | 0.19 / 0.41 | +0.31 | +0.11 | not resolvable (median +0.23 +/- 0.41 straddles 0.5) |
| update-expr prepared | 17 | 47,017 /s (21.27 us) | +0.11 [-0.79, +0.54] (12 of 17) | +0.14 [-0.84, +1.26] (10 of 17) | +0.05 [-1.09, +0.42] (11 of 17) | 0.29 / 1.00 | +0.11 | -0.01 | not resolvable (median +0.11 +/- 1.00 straddles 0.5) |

#### the control: A2 - A and sign tests (one-sided, unadjusted)

| cell | A2 - A median [min, max] | A2 above A | B - mean(A,A2) above 0 | p | C - mean above 0 | p |
|---|---|---|---|---|---|---|
| select-pk literal | -0.15 [-0.34, +0.25] (4 of 16) | 4 of 16 | 15 of 16 | 0.0003 | 12 of 16 | 0.0384 |
| select-pk prepared | +0.07 [-0.63, +0.69] (8 of 14) | 8 of 14 | 13 of 14 | 0.0009 | 8 of 14 | 0.3953 |
| update-expr literal | -0.08 [-0.41, +0.36] (8 of 16) | 8 of 16 | 14 of 16 | 0.0021 | 13 of 16 | 0.0106 |
| update-expr prepared | -0.00 [-0.93, +1.00] (8 of 17) | 8 of 17 | 11 of 17 | 0.1662 | 8 of 17 | 0.6855 |

#### client-side wall latency per statement, us (median over runs of each run's percentile)

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| select-pk | literal | A | 31968 | 47.5 | 49.3 | 50.1 | 57.3 | 73.8 | 18,796 /s | 20.46 | 31.93 |
| select-pk | literal | A2 | 31968 | 47.5 | 49.2 | 50.0 | 57.3 | 73.8 | 18,724 /s | 20.35 | 32.02 |
| select-pk | literal | B | 31968 | 47.6 | 49.5 | 50.3 | 57.7 | 75.8 | 18,662 /s | 20.78 | 31.86 |
| select-pk | literal | C | 31968 | 47.4 | 49.3 | 50.2 | 57.7 | 74.9 | 18,649 /s | 20.50 | 32.09 |
| select-pk | prepared | A | 27972 | 44.0 | 45.5 | 46.3 | 52.7 | 67.5 | 20,308 /s | 19.77 | 28.56 |
| select-pk | prepared | A2 | 27972 | 43.9 | 45.5 | 46.3 | 53.7 | 71.1 | 20,156 /s | 19.94 | 28.59 |
| select-pk | prepared | B | 27972 | 44.3 | 45.8 | 46.6 | 53.6 | 75.3 | 20,029 /s | 20.04 | 28.72 |
| select-pk | prepared | C | 27972 | 44.3 | 45.6 | 46.4 | 52.8 | 69.3 | 20,171 /s | 19.94 | 28.52 |
| update-expr | literal | A | 31968 | 37.0 | 42.3 | 43.0 | 50.6 | 68.0 | 21,628 /s | 22.05 | 23.33 |
| update-expr | literal | A2 | 31968 | 37.0 | 42.1 | 42.9 | 50.3 | 69.0 | 21,625 /s | 21.88 | 23.33 |
| update-expr | literal | B | 31968 | 37.8 | 42.4 | 43.1 | 50.6 | 69.1 | 21,350 /s | 22.28 | 23.30 |
| update-expr | literal | C | 31968 | 37.4 | 42.3 | 43.0 | 51.1 | 70.1 | 21,488 /s | 22.23 | 23.44 |
| update-expr | prepared | A | 33966 | 36.8 | 38.5 | 39.2 | 46.7 | 66.7 | 23,503 /s | 21.27 | 20.29 |
| update-expr | prepared | A2 | 33966 | 37.2 | 38.5 | 39.2 | 46.3 | 64.9 | 23,545 /s | 21.38 | 20.21 |
| update-expr | prepared | B | 33966 | 36.5 | 38.7 | 39.4 | 46.8 | 66.0 | 23,391 /s | 21.45 | 20.30 |
| update-expr | prepared | C | 33966 | 36.8 | 38.7 | 39.3 | 46.6 | 65.0 | 23,469 /s | 21.45 | 20.26 |

bad replies: 0; runs: 18; unclean cells: 28 [(1, 'select-pk', 'prepared', 'A'), (1, 'select-pk', 'prepared', 'A2'), (1, 'select-pk', 'prepared', 'B'), (1, 'select-pk', 'prepared', 'C'), (7, 'update-expr', 'prepared', 'B'), (7, 'update-expr', 'prepared', 'C'), (7, 'update-expr', 'prepared', 'A'), (7, 'update-expr', 'prepared', 'A2')]; cells dropped as not clean (strict=True): 9 [(1, ('select-pk', 'prepared')), (3, ('select-pk', 'prepared')), (4, ('select-pk', 'literal')), (7, ('update-expr', 'literal')), (7, ('update-expr', 'prepared')), (16, ('select-pk', 'literal')), (16, ('select-pk', 'prepared')), (16, ('update-expr', 'literal')), (17, ('select-pk', 'prepared'))]
