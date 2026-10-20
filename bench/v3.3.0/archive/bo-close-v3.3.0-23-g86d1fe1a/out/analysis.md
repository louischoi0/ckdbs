#### headline (pooled over all invocations); server CPU per statement

| cell | runs | A | A' | B | B - A paired median [min, max] | B above A | A' - A typical / largest abs gap | verdict |
|---|---|---|---|---|---|---|---|---|
| select-pk literal | 42 | 31,615 /s | 32,035 /s | 31,220 /s | +0.42 us (+1.3 %, B slower) [-2.38, +3.49] us | 32 of 42 | 0.58 / 2.15 us | inside |
| select-pk prepared | 42 | 32,157 /s | 32,961 /s | 32,210 /s | +0.35 us (+1.1 %, B slower) [-1.59, +3.40] us | 29 of 42 | 0.36 / 2.69 us | inside |
| insert literal | 42 | 41,828 /s | 41,585 /s | 40,958 /s | +0.67 us (+2.8 %, B slower) [-1.24, +2.24] us | 33 of 42 | 0.31 / 1.70 us | inside |
| insert prepared | 42 | 43,393 /s | 43,024 /s | 42,241 /s | +0.55 us (+2.4 %, B slower) [-0.71, +1.78] us | 33 of 42 | 0.23 / 0.82 us | inside |
| delete-pk literal | 42 | 42,519 /s | 42,037 /s | 41,460 /s | +0.50 us (+2.1 %, B slower) [-0.33, +1.85] us | 37 of 42 | 0.47 / 1.62 us | inside |
| delete-pk prepared | 42 | 43,604 /s | 42,934 /s | 42,610 /s | +0.47 us (+2.1 %, B slower) [-0.34, +1.73] us | 34 of 42 | 0.34 / 1.57 us | inside |
| update-range literal | 24 | 1,328 /s | 1,350 /s | 1,298 /s | +9.55 us (+1.3 %, B slower) [-70.88, +102.44] us | 16 of 24 | 23.19 / 120.92 us | inside |
| update-range prepared | 24 | 1,342 /s | 1,339 /s | 1,307 /s | +11.78 us (+1.6 %, B slower) [-48.11, +122.93] us | 18 of 24 | 11.04 / 170.73 us | inside |

#### sign tests, per-run paired server CPU: B - A, B - A', A' - A, and B against the mean of A and A'

| cell | runs | B - A median | B above A | B - A' median | B above A' | A' - A median | A' above A | B - mean(A, A') median | B above mean | one-sided sign-test p, B above mean |
|---|---|---|---|---|---|---|---|---|---|---|
| select-pk literal | 42 | +0.42 us | 32 of 42 | +0.33 us | 27 of 42 | +0.27 us | 30 of 42 | +0.47 us (+1.5 %) | 28 of 42 | 0.0218 |
| select-pk prepared | 42 | +0.35 us | 29 of 42 | +0.23 us | 27 of 42 | +0.18 us | 30 of 42 | +0.39 us (+1.2 %) | 28 of 42 | 0.0218 |
| insert literal | 42 | +0.67 us | 33 of 42 | +0.35 us | 33 of 42 | +0.17 us | 27 of 42 | +0.52 us (+2.2 %) | 34 of 42 | 0.0000 |
| insert prepared | 42 | +0.55 us | 33 of 42 | +0.33 us | 30 of 42 | +0.13 us | 26 of 42 | +0.46 us (+2.0 %) | 31 of 42 | 0.0014 |
| delete-pk literal | 42 | +0.50 us | 37 of 42 | +0.22 us | 30 of 42 | +0.31 us | 32 of 42 | +0.40 us (+1.7 %) | 36 of 42 | 0.0000 |
| delete-pk prepared | 42 | +0.47 us | 34 of 42 | +0.18 us | 25 of 42 | +0.25 us | 30 of 42 | +0.35 us (+1.5 %) | 32 of 42 | 0.0005 |
| update-range literal | 24 | +9.55 us | 16 of 24 | +13.57 us | 20 of 24 | -2.66 us | 9 of 24 | +10.90 us (+1.4 %) | 17 of 24 | 0.0320 |
| update-range prepared | 24 | +11.78 us | 18 of 24 | +21.27 us | 21 of 24 | -7.47 us | 7 of 24 | +17.55 us (+2.4 %) | 19 of 24 | 0.0033 |

#### by invocation: B - A and A' - A, paired median [min, max], us of server CPU

| file | start order | shape | path | runs | A | A' | B | B - A | A' - A |
|---|---|---|---|---|---|---|---|---|---|
| w1 | B, A', A | select-pk | literal | 6 | 31.35 | 31.71 | 31.78 | +0.43 [-0.14, +2.13] | +0.67 [+0.17, +1.15] |
| w1 | B, A', A | select-pk | prepared | 6 | 30.13 | 30.61 | 30.70 | +0.47 [-1.59, +1.45] | +0.33 [+0.16, +0.80] |
| w1 | B, A', A | insert | literal | 6 | 23.77 | 24.34 | 24.78 | +0.79 [+0.58, +2.24] | +0.52 [-0.16, +1.70] |
| w1 | B, A', A | insert | prepared | 6 | 22.98 | 23.06 | 23.70 | +0.64 [+0.36, +1.78] | +0.28 [-0.06, +0.64] |
| w1 | B, A', A | delete-pk | literal | 6 | 23.75 | 24.05 | 24.89 | +0.91 [+0.38, +1.85] | +0.52 [-0.66, +1.23] |
| w1 | B, A', A | delete-pk | prepared | 6 | 22.80 | 23.00 | 23.68 | +0.87 [+0.16, +1.73] | +0.18 [-0.56, +1.12] |
| w1 | B, A', A | update-range | literal | 6 | 745.20 | 741.46 | 754.30 | +1.57 [-40.30, +26.04] | -8.01 [-120.92, +54.55] |
| w1 | B, A', A | update-range | prepared | 6 | 748.69 | 733.55 | 761.89 | +15.56 [-39.24, +103.35] | -14.36 [-73.56, +1.19] |
| w2 | A, A', B | select-pk | literal | 6 | 31.63 | 31.58 | 32.10 | +0.57 [-2.38, +3.49] | -0.25 [-1.93, +2.03] |
| w2 | A, A', B | select-pk | prepared | 6 | 31.66 | 30.55 | 31.43 | +0.09 [-0.77, +1.15] | -0.17 [-2.69, +1.22] |
| w2 | A, A', B | insert | literal | 6 | 24.10 | 23.97 | 24.47 | +0.66 [-0.67, +1.70] | +0.14 [-0.48, +1.13] |
| w2 | A, A', B | insert | prepared | 6 | 23.09 | 23.32 | 23.56 | +0.47 [-0.18, +1.03] | +0.28 [-0.16, +0.77] |
| w2 | A, A', B | delete-pk | literal | 6 | 23.29 | 23.72 | 23.99 | +0.60 [-0.17, +1.21] | +0.43 [-0.98, +1.62] |
| w2 | A, A', B | delete-pk | prepared | 6 | 22.95 | 23.20 | 23.37 | +0.76 [-0.05, +1.63] | +0.37 [-0.46, +1.18] |
| w2 | A, A', B | update-range | literal | 6 | 760.59 | 737.45 | 754.07 | +5.36 [-49.17, +24.79] | -6.97 [-68.85, +15.91] |
| w2 | A, A', B | update-range | prepared | 6 | 761.50 | 765.62 | 769.01 | +4.94 [-23.42, +35.56] | -3.15 [-110.57, +58.19] |
| w3 | A', B, A | select-pk | literal | 6 | 31.23 | 30.48 | 31.61 | +0.48 [-0.19, +1.35] | -0.21 [-1.48, +0.59] |
| w3 | A', B, A | select-pk | prepared | 6 | 30.68 | 30.38 | 30.67 | +0.05 [-1.28, +1.00] | -0.17 [-1.33, +0.60] |
| w3 | A', B, A | insert | literal | 6 | 24.67 | 24.51 | 24.31 | -0.06 [-1.24, +1.30] | -0.00 [-1.16, +0.22] |
| w3 | A', B, A | insert | prepared | 6 | 23.44 | 23.24 | 23.36 | +0.20 [-0.71, +0.92] | -0.19 [-0.63, +0.22] |
| w3 | A', B, A | delete-pk | literal | 6 | 23.79 | 23.78 | 24.28 | +0.45 [-0.09, +1.80] | +0.01 [-0.37, +0.99] |
| w3 | A', B, A | delete-pk | prepared | 6 | 23.37 | 23.37 | 23.54 | +0.29 [-0.10, +1.14] | +0.06 [-0.46, +0.42] |
| w3 | A', B, A | update-range | literal | 6 | 756.44 | 740.30 | 780.53 | +12.41 [-48.89, +102.44] | -17.61 [-107.79, +99.40] |
| w3 | A', B, A | update-range | prepared | 6 | 736.32 | 733.60 | 755.22 | +20.11 [-48.11, +122.93] | -7.47 [-170.73, +6.61] |
| w4 | B, A, A' | select-pk | literal | 6 | 30.83 | 31.05 | 31.45 | +0.42 [-0.69, +1.24] | +0.22 [+0.17, +0.77] |
| w4 | B, A, A' | select-pk | prepared | 6 | 30.24 | 30.67 | 31.86 | +1.07 [-0.34, +3.40] | +0.38 [+0.07, +1.33] |
| w4 | B, A, A' | insert | literal | 6 | 23.73 | 23.92 | 24.42 | +0.51 [+0.15, +1.01] | +0.09 [-0.23, +0.63] |
| w4 | B, A, A' | insert | prepared | 6 | 23.06 | 23.32 | 23.69 | +0.44 [+0.04, +1.02] | +0.10 [-0.32, +0.68] |
| w4 | B, A, A' | delete-pk | literal | 6 | 23.72 | 23.95 | 24.08 | +0.50 [-0.11, +0.88] | +0.29 [+0.00, +0.67] |
| w4 | B, A, A' | delete-pk | prepared | 6 | 23.11 | 23.43 | 23.42 | +0.48 [-0.12, +1.07] | +0.34 [-0.14, +0.72] |
| w4 | B, A, A' | update-range | literal | 6 | 739.75 | 743.03 | 758.97 | +14.53 [-70.88, +75.66] | +6.27 [-90.50, +89.93] |
| w4 | B, A, A' | update-range | prepared | 6 | 732.43 | 731.93 | 747.06 | +11.42 [-41.63, +103.68] | -4.22 [-56.80, +87.08] |
| w5 | A, A', B | select-pk | literal | 6 | 31.68 | 31.73 | 31.75 | +0.26 [-1.76, +1.19] | +0.39 [-2.15, +1.03] |
| w5 | A, A', B | select-pk | prepared | 6 | 30.04 | 30.42 | 30.19 | +0.31 [-0.72, +1.25] | +0.43 [-0.10, +0.91] |
| w5 | A, A', B | insert | literal | 6 | 23.98 | 23.74 | 24.29 | +0.77 [-0.49, +1.26] | +0.08 [-0.76, +0.64] |
| w5 | A, A', B | insert | prepared | 6 | 22.99 | 23.00 | 23.54 | +0.75 [-0.12, +0.96] | -0.03 [-0.50, +0.82] |
| w5 | A, A', B | delete-pk | literal | 6 | 23.32 | 23.61 | 23.86 | +0.54 [-0.28, +1.40] | +0.24 [-0.22, +0.68] |
| w5 | A, A', B | delete-pk | prepared | 6 | 22.76 | 23.06 | 23.30 | +0.45 [-0.34, +1.33] | +0.09 [-0.10, +1.10] |
| w6 | B, A, A' | select-pk | literal | 6 | 31.15 | 30.79 | 32.05 | +0.87 [+0.14, +3.06] | +0.58 [-0.70, +2.03] |
| w6 | B, A, A' | select-pk | prepared | 6 | 29.91 | 30.45 | 30.92 | +0.87 [-0.64, +1.61] | +0.11 [-0.63, +1.62] |
| w6 | B, A, A' | insert | literal | 6 | 23.87 | 24.15 | 24.23 | +0.39 [-0.14, +0.82] | +0.18 [-0.10, +0.73] |
| w6 | B, A, A' | insert | prepared | 6 | 23.04 | 23.49 | 23.68 | +0.51 [-0.22, +0.72] | +0.13 [+0.04, +0.67] |
| w6 | B, A, A' | delete-pk | literal | 6 | 23.39 | 24.09 | 24.07 | +0.37 [+0.12, +1.15] | +0.62 [+0.03, +1.09] |
| w6 | B, A, A' | delete-pk | prepared | 6 | 23.17 | 23.71 | 23.60 | +0.41 [-0.10, +1.05] | +0.73 [-0.01, +1.25] |
| w7 | A', B, A | select-pk | literal | 6 | 30.88 | 31.64 | 31.56 | +0.28 [-0.95, +1.13] | +0.39 [-0.55, +1.88] |
| w7 | A', B, A | select-pk | prepared | 6 | 30.67 | 30.34 | 30.49 | +0.42 [-0.52, +0.93] | +0.12 [-0.45, +0.66] |
| w7 | A', B, A | insert | literal | 6 | 23.86 | 23.98 | 24.45 | +0.48 [-0.05, +1.00] | +0.18 [-0.64, +0.77] |
| w7 | A', B, A | insert | prepared | 6 | 23.14 | 23.26 | 23.76 | +0.46 [-0.38, +1.08] | +0.06 [-0.57, +0.67] |
| w7 | A', B, A | delete-pk | literal | 6 | 23.18 | 23.78 | 23.88 | +0.51 [-0.33, +1.19] | +0.38 [-0.59, +1.19] |
| w7 | A', B, A | delete-pk | prepared | 6 | 22.92 | 23.37 | 23.45 | +0.41 [-0.03, +1.36] | +0.16 [-0.45, +1.57] |

#### statement wall latency (client side), us; median over runs of each run's percentile

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU | driver CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| select-pk | literal | A | 83916 | 55.2 us | 57.2 us | 60.3 us | 70.3 us | 90.6 us | 15,778 /s | 31.63 us | 32.19 us |
| select-pk | literal | A' | 83916 | 55.2 us | 57.4 us | 60.5 us | 70.9 us | 90.6 us | 15,720 /s | 31.22 us | 32.22 us |
| select-pk | literal | B | 83916 | 55.4 us | 57.7 us | 61.1 us | 71.7 us | 90.0 us | 15,625 /s | 32.03 us | 32.15 us |
| select-pk | prepared | A | 83916 | 51.9 us | 54.0 us | 57.3 us | 67.6 us | 87.2 us | 16,545 /s | 31.10 us | 29.52 us |
| select-pk | prepared | A' | 83916 | 52.0 us | 54.3 us | 57.5 us | 68.0 us | 87.0 us | 16,685 /s | 30.34 us | 29.55 us |
| select-pk | prepared | B | 83916 | 52.3 us | 54.5 us | 57.7 us | 68.7 us | 86.4 us | 16,447 /s | 31.05 us | 29.45 us |
| insert | literal | A | 83916 | 39.4 us | 44.4 us | 44.9 us | 54.1 us | 76.1 us | 20,773 /s | 23.91 us | 24.55 us |
| insert | literal | A' | 83916 | 39.5 us | 44.6 us | 45.1 us | 54.5 us | 76.0 us | 20,691 /s | 24.05 us | 24.55 us |
| insert | literal | B | 83916 | 39.3 us | 44.8 us | 45.4 us | 54.4 us | 76.3 us | 20,623 /s | 24.42 us | 24.58 us |
| insert | prepared | A | 83916 | 37.6 us | 41.8 us | 42.3 us | 50.3 us | 73.3 us | 22,029 /s | 23.05 us | 22.42 us |
| insert | prepared | A' | 83916 | 37.8 us | 41.9 us | 42.4 us | 50.4 us | 73.4 us | 22,039 /s | 23.24 us | 22.43 us |
| insert | prepared | B | 83916 | 37.6 us | 42.2 us | 42.8 us | 51.0 us | 73.9 us | 21,739 /s | 23.67 us | 22.42 us |
| delete-pk | literal | A | 83916 | 38.2 us | 43.7 us | 44.3 us | 52.2 us | 72.0 us | 21,234 /s | 23.52 us | 23.95 us |
| delete-pk | literal | A' | 83916 | 37.4 us | 44.0 us | 44.6 us | 53.0 us | 73.1 us | 20,973 /s | 23.79 us | 24.01 us |
| delete-pk | literal | B | 83916 | 38.2 us | 44.3 us | 44.9 us | 53.0 us | 73.0 us | 21,007 /s | 24.12 us | 23.98 us |
| delete-pk | prepared | A | 83916 | 34.5 us | 40.9 us | 41.4 us | 48.9 us | 68.7 us | 22,566 /s | 22.93 us | 21.48 us |
| delete-pk | prepared | A' | 83916 | 34.6 us | 41.1 us | 41.7 us | 49.8 us | 69.5 us | 22,468 /s | 23.29 us | 21.47 us |
| delete-pk | prepared | B | 83916 | 34.3 us | 41.4 us | 41.9 us | 50.1 us | 69.9 us | 22,329 /s | 23.47 us | 21.52 us |
| update-range | literal | A | 2880 | 682.2 us | 712.0 us | 728.9 us | 853.4 us | 1795.9 us | 1,226 /s | 752.83 us | 38.67 us |
| update-range | literal | A' | 2880 | 679.4 us | 708.2 us | 725.6 us | 884.7 us | 1597.8 us | 1,251 /s | 740.51 us | 38.88 us |
| update-range | literal | B | 2880 | 697.3 us | 725.2 us | 739.5 us | 864.5 us | 1913.5 us | 1,242 /s | 770.48 us | 38.44 us |
| update-range | prepared | A | 2880 | 678.7 us | 707.1 us | 724.0 us | 860.6 us | 1790.1 us | 1,268 /s | 745.04 us | 33.90 us |
| update-range | prepared | A' | 2880 | 675.9 us | 705.2 us | 720.3 us | 836.8 us | 1914.7 us | 1,267 /s | 747.07 us | 34.13 us |
| update-range | prepared | B | 2880 | 694.4 us | 725.4 us | 741.1 us | 893.9 us | 2005.3 us | 1,171 /s | 765.12 us | 35.08 us |

bad replies: 0; end_state all ok: False; runs: 42; unclean cells: 0 []
