#### headline: paired per-run server CPU us per statement, median [min, max] (runs with the first arm's cost exceeded by the second)

| cell | runs | A | B - A | C - A | B - C | A2 - A typical / largest abs | B - mean(A,A2) | C - mean(A,A2) | verdict B - A vs 0.5 us |
|---|---|---|---|---|---|---|---|---|---|
| select-pk literal | 12 | 48,917 /s (20.44 us) | +0.24 [-0.05, +1.00] (11 of 12) | +0.08 [-0.42, +0.61] (8 of 12) | +0.29 [-0.23, +0.91] (10 of 12) | 0.20 / 0.70 | +0.38 | +0.10 | not resolvable (median +0.24 +/- 0.70 straddles 0.5) |
| select-pk prepared | 11 | 50,519 /s (19.79 us) | +0.35 [-0.17, +1.29] (10 of 11) | -0.02 [-0.33, +1.39] (5 of 11) | +0.29 [-0.76, +1.06] (9 of 11) | 0.29 / 0.69 | +0.29 | -0.01 | not resolvable (median +0.35 +/- 0.69 straddles 0.5) |
| update-expr literal | 11 | 45,364 /s (22.04 us) | +0.21 [-0.13, +1.05] (10 of 11) | +0.14 [-0.20, +0.82] (8 of 11) | +0.01 [-0.61, +0.91] (6 of 11) | 0.18 / 0.37 | +0.28 | +0.04 | not resolvable (median +0.21 +/- 0.37 straddles 0.5) |
| update-expr prepared | 11 | 46,600 /s (21.46 us) | +0.06 [-0.79, +0.54] (6 of 11) | -0.03 [-0.84, +0.63] (5 of 11) | +0.10 [-0.57, +0.42] (9 of 11) | 0.29 / 0.93 | +0.11 | -0.03 | not resolvable (median +0.06 +/- 0.93 straddles 0.5) |

#### the control: A2 - A and sign tests (one-sided, unadjusted)

| cell | A2 - A median [min, max] | A2 above A | B - mean(A,A2) above 0 | p | C - mean above 0 | p |
|---|---|---|---|---|---|---|
| select-pk literal | -0.15 [-0.31, +0.70] (3 of 12) | 3 of 12 | 12 of 12 | 0.0002 | 7 of 12 | 0.3872 |
| select-pk prepared | +0.09 [-0.35, +0.69] (7 of 11) | 7 of 11 | 10 of 11 | 0.0059 | 5 of 11 | 0.7256 |
| update-expr literal | +0.02 [-0.37, +0.36] (6 of 11) | 6 of 11 | 9 of 11 | 0.0327 | 9 of 11 | 0.0327 |
| update-expr prepared | -0.10 [-0.93, +0.49] (4 of 11) | 4 of 11 | 6 of 11 | 0.5000 | 3 of 11 | 0.9673 |

#### client-side wall latency per statement, us (median over runs of each run's percentile)

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| select-pk | literal | A | 23976 | 47.5 | 49.3 | 50.3 | 57.4 | 73.6 | 18,770 /s | 20.44 | 32.03 |
| select-pk | literal | A2 | 23976 | 47.6 | 49.2 | 50.2 | 57.3 | 74.2 | 18,623 /s | 20.35 | 32.08 |
| select-pk | literal | B | 23976 | 47.7 | 49.6 | 50.4 | 57.8 | 74.8 | 18,560 /s | 20.78 | 31.86 |
| select-pk | literal | C | 23976 | 47.6 | 49.4 | 50.2 | 57.3 | 76.7 | 18,703 /s | 20.44 | 32.09 |
| select-pk | prepared | A | 21978 | 44.1 | 45.7 | 46.6 | 53.0 | 68.2 | 20,199 /s | 19.79 | 28.88 |
| select-pk | prepared | A2 | 21978 | 44.1 | 45.6 | 46.4 | 54.0 | 72.3 | 20,051 /s | 19.98 | 28.75 |
| select-pk | prepared | B | 21978 | 44.3 | 45.9 | 46.7 | 54.5 | 77.1 | 19,949 /s | 20.04 | 28.89 |
| select-pk | prepared | C | 21978 | 44.3 | 45.7 | 46.6 | 52.8 | 71.1 | 20,001 /s | 19.81 | 28.76 |
| update-expr | literal | A | 21978 | 37.3 | 42.3 | 43.0 | 50.9 | 68.0 | 21,625 /s | 22.04 | 23.38 |
| update-expr | literal | A2 | 21978 | 36.8 | 42.3 | 42.9 | 50.4 | 69.3 | 21,589 /s | 21.88 | 23.47 |
| update-expr | literal | B | 21978 | 37.7 | 42.4 | 43.1 | 50.7 | 69.4 | 21,375 /s | 22.25 | 23.30 |
| update-expr | literal | C | 21978 | 37.5 | 42.3 | 43.1 | 51.0 | 70.4 | 21,432 /s | 22.22 | 23.42 |
| update-expr | prepared | A | 21978 | 36.8 | 38.7 | 39.4 | 47.0 | 67.3 | 23,401 /s | 21.46 | 20.35 |
| update-expr | prepared | A2 | 21978 | 37.3 | 38.6 | 39.3 | 46.4 | 64.8 | 23,494 /s | 21.46 | 20.30 |
| update-expr | prepared | B | 21978 | 37.0 | 38.8 | 39.5 | 47.0 | 66.0 | 23,320 /s | 21.52 | 20.35 |
| update-expr | prepared | C | 21978 | 37.2 | 38.8 | 39.4 | 46.6 | 65.2 | 23,389 /s | 21.46 | 20.28 |

bad replies: 0; runs: 12; unclean cells: 12 [(1, 'select-pk', 'prepared', 'A'), (1, 'select-pk', 'prepared', 'A2'), (1, 'select-pk', 'prepared', 'B'), (1, 'select-pk', 'prepared', 'C'), (7, 'update-expr', 'prepared', 'B'), (7, 'update-expr', 'prepared', 'C'), (7, 'update-expr', 'prepared', 'A'), (7, 'update-expr', 'prepared', 'A2')]; cells dropped as not clean (strict=False): 3 [(1, ('select-pk', 'prepared')), (7, ('update-expr', 'literal')), (7, ('update-expr', 'prepared'))]
