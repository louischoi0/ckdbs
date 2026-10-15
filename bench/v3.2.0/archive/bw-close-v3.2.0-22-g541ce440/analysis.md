
#### scan 10,000 rows, s1+s2 pooled: net scan row, ns of server CPU per row, (scan CPU - point SELECT CPU on the same relation) / rows; point SELECT CPU in us

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| bn (10000) net row ns literal | 7 | 27.40 | 27.24 | 27.60 | 0.04 [-0.29, 1.08] | +0.2 % | 2 of 7 | 0.13 / 1.37 | inside |
| bn (10000) net row ns prepared | 7 | 27.49 | 27.42 | 27.80 | 0.17 [-0.57, 1.30] | +0.6 % | 1 of 7 | 0.12 / 0.45 | inside |
| wide-h (10000) net row ns literal | 7 | 30.16 | 30.10 | 30.33 | 0.13 [-0.54, 1.96] | +0.4 % | 2 of 7 | 0.34 / 0.88 | inside |
| wide-h (10000) net row ns prepared | 7 | 30.31 | 30.88 | 30.62 | 0.40 [0.23, 0.53] | +1.3 % | 0 of 7 | 0.21 / 1.39 | inside |
| point:s CPU us literal | 7 | 29.32 | 28.89 | 29.49 | 0.16 [-0.20, 1.25] | +0.6 % | 3 of 7 | 0.52 / 1.05 | inside |
| point:s CPU us prepared | 7 | 27.43 | 27.17 | 27.49 | -0.01 [-0.53, 1.15] | -0.0 % | 4 of 7 | 0.24 / 0.94 | inside |
| point:wd CPU us literal | 7 | 30.84 | 30.69 | 30.98 | 0.37 [-0.61, 0.70] | +1.2 % | 1 of 7 | 0.29 / 0.84 | inside |
| point:wd CPU us prepared | 7 | 29.98 | 29.96 | 29.97 | 0.18 [-0.59, 1.02] | +0.6 % | 2 of 7 | 0.33 / 0.85 | inside |

#### scan 10,000 rows, s1+s2 pooled: latency of one scan (wall, client side), us; median over runs of each run's percentile; ops = total scans over runs

| cell | path | arm | ops | p0 | p25 | p50 | p95 | p99 | scans/s at p50 (derived) | server CPU us/scan | driver CPU us/scan |
|---|---|---|---|---|---|---|---|---|---|---|---|
| bn | literal | A | 672 | 320.3 | 327.6 | 337.0 | 366.6 | 412.4 | 2968 | 303.6 | 59.8 |
| bn | literal | A2 | 672 | 319.3 | 325.8 | 336.6 | 361.7 | 409.1 | 2971 | 301.6 | 57.7 |
| bn | literal | B | 672 | 322.3 | 331.1 | 338.9 | 370.3 | 410.3 | 2951 | 305.1 | 56.2 |
| bn | prepared | A | 672 | 317.3 | 324.9 | 333.9 | 368.5 | 411.5 | 2995 | 302.3 | 54.5 |
| bn | prepared | A2 | 672 | 316.5 | 325.5 | 334.1 | 359.5 | 420.5 | 2993 | 301.8 | 53.4 |
| bn | prepared | B | 672 | 320.6 | 327.5 | 336.9 | 364.0 | 415.0 | 2969 | 305.3 | 53.9 |
| wide-h | literal | A | 672 | 361.3 | 375.4 | 383.2 | 429.5 | 758.7 | 2609 | 331.8 | 89.4 |
| wide-h | literal | A2 | 672 | 362.5 | 373.1 | 380.6 | 427.9 | 756.8 | 2627 | 332.3 | 89.0 |
| wide-h | literal | B | 672 | 365.9 | 376.7 | 386.2 | 425.9 | 766.2 | 2589 | 334.3 | 90.3 |
| wide-h | prepared | A | 672 | 360.0 | 371.5 | 380.4 | 418.5 | 757.1 | 2629 | 333.1 | 86.0 |
| wide-h | prepared | A2 | 672 | 358.1 | 372.6 | 381.4 | 457.8 | 786.1 | 2622 | 340.1 | 88.3 |
| wide-h | prepared | B | 672 | 362.9 | 374.9 | 383.6 | 449.2 | 800.7 | 2607 | 336.2 | 88.6 |

bad replies: 0; window_wait_us total: 0; scan-cell pool_misses total: 0; digest arms-distinct per cell: [('bn', 1), ('wide-h', 1)]; n runs 7
unclean windows: 0 []

#### scan 10,000 rows, s1 alone: net scan row, ns of server CPU per row, (scan CPU - point SELECT CPU on the same relation) / rows; point SELECT CPU in us

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| bn (10000) net row ns literal | 3 | 27.44 | 27.32 | 27.50 | 0.04 [-0.15, 0.10] | +0.2 % | 1 of 3 | 0.13 / 0.20 | inside |
| bn (10000) net row ns prepared | 3 | 27.68 | 27.36 | 27.81 | 0.11 [0.02, 0.32] | +0.4 % | 0 of 3 | 0.12 / 0.38 | inside |
| wide-h (10000) net row ns literal | 3 | 30.54 | 29.83 | 30.17 | 0.13 [-0.54, 0.52] | +0.4 % | 1 of 3 | 0.34 / 0.88 | inside |
| wide-h (10000) net row ns prepared | 3 | 30.02 | 30.88 | 30.34 | 0.38 [0.32, 0.53] | +1.3 % | 0 of 3 | 0.19 / 1.39 | inside |
| point:s CPU us literal | 3 | 29.24 | 28.92 | 29.49 | 0.17 [-0.04, 1.25] | +0.6 % | 1 of 3 | 0.52 / 1.05 | inside |
| point:s CPU us prepared | 3 | 27.43 | 27.32 | 27.81 | 0.34 [-0.22, 1.15] | +1.2 % | 1 of 3 | 0.26 / 0.76 | inside |
| point:wd CPU us literal | 3 | 30.84 | 30.69 | 31.25 | 0.41 [0.37, 0.55] | +1.3 % | 0 of 3 | 0.64 / 0.71 | inside |
| point:wd CPU us prepared | 3 | 30.12 | 29.84 | 30.29 | 0.34 [-0.16, 1.02] | +1.1 % | 1 of 3 | 0.33 / 0.85 | inside |

#### scan 10,000 rows, s1 alone: latency of one scan (wall, client side), us; median over runs of each run's percentile; ops = total scans over runs

| cell | path | arm | ops | p0 | p25 | p50 | p95 | p99 | scans/s at p50 (derived) | server CPU us/scan | driver CPU us/scan |
|---|---|---|---|---|---|---|---|---|---|---|---|
| bn | literal | A | 288 | 320.3 | 330.3 | 337.7 | 363.8 | 412.4 | 2961 | 303.7 | 57.4 |
| bn | literal | A2 | 288 | 319.6 | 326.7 | 337.5 | 360.1 | 433.3 | 2963 | 301.9 | 59.5 |
| bn | literal | B | 288 | 322.1 | 330.0 | 338.9 | 370.3 | 395.0 | 2951 | 304.3 | 56.6 |
| bn | prepared | A | 288 | 317.8 | 325.4 | 335.5 | 370.3 | 415.5 | 2980 | 304.2 | 55.3 |
| bn | prepared | A2 | 288 | 316.8 | 323.1 | 333.3 | 361.2 | 420.5 | 3001 | 301.0 | 53.3 |
| bn | prepared | B | 288 | 320.8 | 328.7 | 336.6 | 368.6 | 420.5 | 2971 | 305.9 | 54.3 |
| wide-h | literal | A | 288 | 361.8 | 375.5 | 385.0 | 505.1 | 758.7 | 2597 | 336.7 | 90.9 |
| wide-h | literal | A2 | 288 | 361.1 | 373.1 | 380.6 | 427.9 | 760.7 | 2627 | 328.5 | 90.2 |
| wide-h | literal | B | 288 | 367.8 | 378.7 | 386.7 | 425.9 | 732.0 | 2586 | 333.0 | 88.8 |
| wide-h | prepared | A | 288 | 360.0 | 370.0 | 381.7 | 418.5 | 729.9 | 2620 | 330.1 | 86.0 |
| wide-h | prepared | A2 | 288 | 357.2 | 372.6 | 379.7 | 428.8 | 786.1 | 2634 | 340.1 | 91.3 |
| wide-h | prepared | B | 288 | 362.9 | 371.8 | 382.9 | 426.8 | 748.8 | 2612 | 333.7 | 89.8 |

bad replies: 0; window_wait_us total: 0; scan-cell pool_misses total: 0; digest arms-distinct per cell: [('bn', 1), ('wide-h', 1)]; n runs 3
unclean windows: 0 []

#### scan 10,000 rows, s2 alone: net scan row, ns of server CPU per row, (scan CPU - point SELECT CPU on the same relation) / rows; point SELECT CPU in us

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| bn (10000) net row ns literal | 4 | 27.30 | 27.20 | 27.67 | 0.27 [-0.29, 1.08] | +1.0 % | 1 of 4 | 0.40 / 1.37 | inside |
| bn (10000) net row ns prepared | 4 | 27.40 | 27.58 | 27.77 | 0.29 [-0.57, 1.30] | +1.1 % | 1 of 4 | 0.22 / 0.45 | inside |
| wide-h (10000) net row ns literal | 4 | 30.05 | 30.22 | 30.41 | 0.25 [-0.09, 1.96] | +0.8 % | 1 of 4 | 0.32 / 0.54 | inside |
| wide-h (10000) net row ns prepared | 4 | 30.35 | 30.85 | 30.72 | 0.44 [0.23, 0.51] | +1.4 % | 0 of 4 | 0.50 / 0.83 | inside |
| point:s CPU us literal | 4 | 29.52 | 28.79 | 29.34 | -0.01 [-0.20, 0.86] | -0.0 % | 2 of 4 | 0.66 / 0.98 | inside |
| point:s CPU us prepared | 4 | 27.42 | 27.03 | 27.40 | -0.09 [-0.53, 1.07] | -0.3 % | 3 of 4 | 0.20 / 0.94 | inside |
| point:wd CPU us literal | 4 | 30.66 | 30.72 | 30.95 | 0.15 [-0.61, 0.70] | +0.5 % | 1 of 4 | 0.24 / 0.84 | inside |
| point:wd CPU us prepared | 4 | 29.89 | 30.02 | 29.83 | 0.16 [-0.59, 0.59] | +0.5 % | 1 of 4 | 0.20 / 0.52 | inside |

#### scan 10,000 rows, s2 alone: latency of one scan (wall, client side), us; median over runs of each run's percentile; ops = total scans over runs

| cell | path | arm | ops | p0 | p25 | p50 | p95 | p99 | scans/s at p50 (derived) | server CPU us/scan | driver CPU us/scan |
|---|---|---|---|---|---|---|---|---|---|---|---|
| bn | literal | A | 384 | 320.0 | 326.9 | 335.9 | 367.2 | 418.4 | 2977 | 302.8 | 60.4 |
| bn | literal | A2 | 384 | 319.3 | 325.7 | 335.2 | 362.2 | 408.1 | 2983 | 301.2 | 57.2 |
| bn | literal | B | 384 | 323.5 | 332.6 | 340.2 | 371.2 | 421.2 | 2939 | 306.1 | 56.2 |
| bn | prepared | A | 384 | 316.5 | 323.8 | 333.7 | 360.4 | 410.1 | 2997 | 301.7 | 54.3 |
| bn | prepared | A2 | 384 | 316.5 | 326.8 | 334.2 | 358.1 | 439.7 | 2992 | 303.0 | 53.9 |
| bn | prepared | B | 384 | 320.5 | 327.0 | 337.5 | 362.8 | 410.4 | 2963 | 304.9 | 53.2 |
| wide-h | literal | A | 384 | 361.3 | 374.9 | 382.6 | 424.7 | 755.8 | 2614 | 331.1 | 88.2 |
| wide-h | literal | A2 | 384 | 363.4 | 373.7 | 382.3 | 464.5 | 750.8 | 2615 | 332.9 | 86.3 |
| wide-h | literal | B | 384 | 364.5 | 375.9 | 385.7 | 443.9 | 844.2 | 2593 | 334.5 | 91.3 |
| wide-h | prepared | A | 384 | 360.5 | 372.6 | 379.9 | 447.3 | 770.3 | 2632 | 333.2 | 87.4 |
| wide-h | prepared | A2 | 384 | 359.3 | 371.0 | 381.9 | 480.0 | 791.3 | 2618 | 338.0 | 87.6 |
| wide-h | prepared | B | 384 | 363.3 | 375.0 | 383.6 | 469.5 | 1307.7 | 2607 | 336.9 | 87.1 |

bad replies: 0; window_wait_us total: 0; scan-cell pool_misses total: 0; digest arms-distinct per cell: [('bn', 1), ('wide-h', 1)]; n runs 4
unclean windows: 0 []

#### scan 200 / 1,000 rows: net scan row, ns of server CPU per row, (scan CPU - point SELECT CPU on the same relation) / rows; point SELECT CPU in us

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| rows-200 (200) net row ns literal | 2 | 26.86 | 29.41 | 28.14 | 1.27 [0.77, 1.78] | +4.7 % | 0 of 2 | 2.54 / 3.83 | inside |
| rows-200 (200) net row ns prepared | 2 | 28.99 | 30.82 | 28.23 | -0.76 [-1.58, 0.06] | -2.6 % | 1 of 2 | 1.83 / 3.37 | inside |
| rows-1k (1000) net row ns literal | 2 | 27.00 | 27.05 | 27.29 | 0.29 [-0.26, 0.84] | +1.1 % | 1 of 2 | 0.63 / 0.67 | inside |
| rows-1k (1000) net row ns prepared | 2 | 25.69 | 25.88 | 26.44 | 0.75 [0.48, 1.02] | +2.9 % | 0 of 2 | 0.52 / 0.71 | outside |
| point:s200 CPU us literal | 2 | 27.53 | 27.24 | 27.58 | 0.05 [-0.42, 0.52] | +0.2 % | 1 of 2 | 0.60 / 0.89 | inside |
| point:s200 CPU us prepared | 2 | 26.60 | 26.35 | 26.85 | 0.25 [-0.04, 0.54] | +0.9 % | 1 of 2 | 0.70 / 0.95 | inside |
| point:s1k CPU us literal | 2 | 28.93 | 28.96 | 29.01 | 0.08 [-0.40, 0.57] | +0.3 % | 1 of 2 | 0.85 / 0.89 | inside |
| point:s1k CPU us prepared | 2 | 30.07 | 29.82 | 29.29 | -0.78 [-0.84, -0.73] | -2.6 % | 2 of 2 | 0.25 / 0.44 | outside |

#### scan 200 / 1,000 rows: latency of one scan (wall, client side), us; median over runs of each run's percentile; ops = total scans over runs

| cell | path | arm | ops | p0 | p25 | p50 | p95 | p99 | scans/s at p50 (derived) | server CPU us/scan | driver CPU us/scan |
|---|---|---|---|---|---|---|---|---|---|---|---|
| rows-200 | literal | A | 1536 | 59.2 | 61.0 | 61.9 | 76.9 | 93.5 | 16163 | 32.9 | 34.6 |
| rows-200 | literal | A2 | 1536 | 59.7 | 61.1 | 62.0 | 76.3 | 91.6 | 16140 | 33.1 | 34.5 |
| rows-200 | literal | B | 1536 | 59.8 | 61.2 | 62.0 | 76.3 | 86.8 | 16123 | 33.2 | 35.1 |
| rows-200 | prepared | A | 1536 | 56.2 | 57.7 | 58.5 | 73.9 | 87.2 | 17083 | 32.4 | 31.9 |
| rows-200 | prepared | A2 | 1536 | 55.8 | 57.9 | 58.8 | 72.9 | 89.3 | 17013 | 32.5 | 32.1 |
| rows-200 | prepared | B | 1536 | 56.6 | 57.8 | 58.6 | 73.1 | 89.3 | 17075 | 32.5 | 31.9 |
| rows-1k | literal | A | 768 | 81.4 | 83.4 | 84.8 | 102.5 | 130.4 | 11793 | 55.9 | 38.3 |
| rows-1k | literal | A2 | 768 | 81.3 | 83.4 | 84.8 | 102.2 | 123.4 | 11792 | 56.0 | 39.7 |
| rows-1k | literal | B | 768 | 82.3 | 83.9 | 85.2 | 101.4 | 128.7 | 11741 | 56.3 | 37.5 |
| rows-1k | prepared | A | 768 | 78.4 | 80.2 | 81.5 | 98.9 | 128.9 | 12266 | 55.8 | 34.9 |
| rows-1k | prepared | A2 | 768 | 78.2 | 80.3 | 81.9 | 97.8 | 118.7 | 12204 | 55.7 | 35.6 |
| rows-1k | prepared | B | 768 | 78.7 | 80.5 | 81.7 | 98.1 | 124.6 | 12246 | 55.7 | 34.2 |

bad replies: 0; window_wait_us total: 0; scan-cell pool_misses total: 0; digest arms-distinct per cell: [('rows-1k', 1), ('rows-200', 1)]; n runs 2
unclean windows: 0 []

#### u1+u2 pooled, 1,000 rows: server CPU per statement, us (an insert/select-pk statement; update-range = one every-row UPDATE of 1,000 rows)

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| insert CPU us literal | 6 | 22.36 | 22.75 | 22.77 | 0.48 [-0.58, 0.84] | +2.1 % | 1 of 6 | 0.40 / 1.07 | inside |
| insert CPU us prepared | 6 | 21.86 | 22.16 | 22.40 | 0.29 [-0.04, 0.81] | +1.3 % | 1 of 6 | 0.48 / 0.76 | inside |
| select-pk CPU us literal | 6 | 31.31 | 31.42 | 31.42 | 0.11 [-0.51, 2.21] | +0.3 % | 2 of 6 | 0.40 / 0.85 | inside |
| select-pk CPU us prepared | 6 | 28.84 | 27.39 | 27.72 | 0.17 [-2.48, 0.31] | +0.6 % | 2 of 6 | 0.46 / 2.53 | inside |
| update-range CPU us literal | 6 | 752.14 | 737.81 | 770.18 | 30.24 [10.32, 44.08] | +4.0 % | 0 of 6 | 8.06 / 27.11 | outside |
| update-range CPU us prepared | 6 | 785.98 | 789.08 | 814.68 | 30.19 [-6.11, 53.12] | +3.8 % | 1 of 6 | 10.11 / 17.18 | outside |

#### u1+u2 pooled, 1,000 rows: statement wall latency, us; median over runs of each run's percentile

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | literal | A | 11988 | 37.5 | 43.0 | 43.5 | 51.4 | 73.0 | 21463 | 22.4 | 24.3 |
| insert | literal | A2 | 11988 | 39.8 | 43.3 | 43.9 | 51.7 | 70.2 | 21215 | 22.8 | 24.3 |
| insert | literal | B | 11988 | 38.8 | 43.3 | 43.9 | 51.7 | 73.5 | 20791 | 22.8 | 24.2 |
| insert | prepared | A | 11988 | 34.5 | 40.5 | 41.0 | 48.7 | 70.9 | 22383 | 21.9 | 22.3 |
| insert | prepared | A2 | 11988 | 35.2 | 40.9 | 41.4 | 49.3 | 73.5 | 22507 | 22.2 | 22.3 |
| insert | prepared | B | 11988 | 34.8 | 40.9 | 41.4 | 50.0 | 71.2 | 22436 | 22.4 | 22.2 |
| select-pk | literal | A | 11988 | 54.0 | 56.4 | 61.6 | 70.2 | 91.2 | 15910 | 31.3 | 31.6 |
| select-pk | literal | A2 | 11988 | 54.0 | 56.4 | 61.5 | 70.0 | 87.6 | 15897 | 31.4 | 31.9 |
| select-pk | literal | B | 11988 | 54.0 | 56.4 | 61.7 | 70.1 | 86.8 | 15903 | 31.4 | 31.8 |
| select-pk | prepared | A | 11988 | 50.7 | 53.0 | 54.3 | 66.8 | 87.3 | 17197 | 28.8 | 29.3 |
| select-pk | prepared | A2 | 11988 | 50.8 | 53.0 | 53.9 | 61.5 | 76.3 | 17780 | 27.4 | 29.2 |
| select-pk | prepared | B | 11988 | 50.6 | 52.9 | 54.0 | 62.8 | 78.3 | 17613 | 27.7 | 29.2 |
| update-range | literal | A | 720 | 696.0 | 724.3 | 737.8 | 862.9 | 1284.9 | 1264 | 752.1 | 36.9 |
| update-range | literal | A2 | 720 | 694.2 | 720.8 | 733.5 | 863.1 | 984.9 | 1291 | 737.8 | 38.2 |
| update-range | literal | B | 720 | 728.1 | 758.7 | 769.7 | 894.0 | 1003.5 | 1229 | 770.2 | 38.1 |
| update-range | prepared | A | 720 | 692.5 | 723.6 | 735.2 | 851.7 | 2094.7 | 1226 | 786.0 | 33.4 |
| update-range | prepared | A2 | 720 | 690.7 | 717.8 | 731.4 | 869.0 | 2129.5 | 1204 | 789.1 | 33.5 |
| update-range | prepared | B | 720 | 719.8 | 752.6 | 767.7 | 890.8 | 2058.9 | 1172 | 814.7 | 33.5 |

bad replies: 0; end_state all ok: True; n runs 6
pages_allocated_delta (unspilled) per arm per run: [{'B': 0, 'A2': 0, 'A': 0}, {'B': 0, 'A2': 0, 'A': 0}, {'B': 0, 'A2': 0, 'A': 0}]

#### u1 alone: server CPU per statement, us (an insert/select-pk statement; update-range = one every-row UPDATE of 1,000 rows)

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| insert CPU us literal | 3 | 22.96 | 23.28 | 23.80 | 0.32 [-0.58, 0.84] | +1.4 % | 1 of 3 | 0.41 / 1.07 | inside |
| insert CPU us prepared | 3 | 22.44 | 22.31 | 22.40 | 0.19 [-0.04, 0.21] | +0.8 % | 1 of 3 | 0.36 / 0.72 | inside |
| select-pk CPU us literal | 3 | 32.06 | 31.49 | 31.57 | 0.60 [-0.50, 2.21] | +1.9 % | 1 of 3 | 0.57 / 0.85 | inside |
| select-pk CPU us prepared | 3 | 30.05 | 27.52 | 27.88 | 0.25 [-2.48, 0.31] | +0.8 % | 1 of 3 | 0.99 / 2.53 | inside |
| update-range CPU us literal | 3 | 762.15 | 740.58 | 772.47 | 23.68 [10.32, 25.77] | +3.1 % | 0 of 3 | 14.69 / 27.11 | inside |
| update-range CPU us prepared | 3 | 797.36 | 808.98 | 818.56 | 34.15 [-6.11, 53.12] | +4.3 % | 1 of 3 | 5.95 / 11.63 | outside |

#### u1 alone: statement wall latency, us; median over runs of each run's percentile

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | literal | A | 5994 | 38.6 | 43.4 | 44.0 | 54.2 | 81.0 | 21056 | 23.0 | 24.8 |
| insert | literal | A2 | 5994 | 39.6 | 43.4 | 44.6 | 58.0 | 81.0 | 20306 | 23.3 | 25.1 |
| insert | literal | B | 5994 | 37.2 | 43.6 | 44.4 | 57.8 | 87.3 | 19970 | 23.8 | 24.5 |
| insert | prepared | A | 5994 | 34.5 | 40.8 | 41.3 | 51.6 | 78.2 | 22340 | 22.4 | 22.3 |
| insert | prepared | A2 | 5994 | 35.4 | 41.0 | 41.5 | 50.5 | 74.5 | 22399 | 22.3 | 22.3 |
| insert | prepared | B | 5994 | 34.1 | 40.9 | 41.4 | 50.0 | 74.0 | 22260 | 22.4 | 22.3 |
| select-pk | literal | A | 5994 | 54.3 | 56.5 | 62.3 | 71.4 | 103.9 | 15639 | 32.1 | 31.6 |
| select-pk | literal | A2 | 5994 | 54.0 | 56.5 | 61.6 | 70.4 | 85.8 | 15873 | 31.5 | 31.9 |
| select-pk | literal | B | 5994 | 54.3 | 56.8 | 61.8 | 69.8 | 86.5 | 15876 | 31.6 | 31.8 |
| select-pk | prepared | A | 5994 | 50.9 | 53.6 | 54.6 | 67.4 | 90.2 | 16870 | 30.0 | 29.3 |
| select-pk | prepared | A2 | 5994 | 50.6 | 53.1 | 54.0 | 61.6 | 77.3 | 17711 | 27.5 | 29.4 |
| select-pk | prepared | B | 5994 | 50.7 | 53.4 | 54.3 | 63.6 | 78.7 | 17509 | 27.9 | 29.1 |
| update-range | literal | A | 360 | 702.2 | 731.4 | 750.8 | 927.1 | 1602.4 | 1244 | 762.1 | 36.3 |
| update-range | literal | A2 | 360 | 694.8 | 723.9 | 738.7 | 878.9 | 1002.4 | 1288 | 740.6 | 38.4 |
| update-range | literal | B | 360 | 726.7 | 763.0 | 778.8 | 914.7 | 1026.8 | 1212 | 772.5 | 38.3 |
| update-range | prepared | A | 360 | 691.5 | 724.0 | 735.7 | 1030.6 | 2127.2 | 1206 | 797.4 | 35.8 |
| update-range | prepared | A2 | 360 | 690.9 | 724.8 | 739.3 | 905.4 | 2274.6 | 1178 | 809.0 | 32.7 |
| update-range | prepared | B | 360 | 722.9 | 754.3 | 773.1 | 908.1 | 2078.8 | 1170 | 818.6 | 33.2 |

bad replies: 0; end_state all ok: True; n runs 3
pages_allocated_delta (unspilled) per arm per run: [{'B': 0, 'A2': 0, 'A': 0}, {'B': 0, 'A2': 0, 'A': 0}, {'B': 0, 'A2': 0, 'A': 0}]

#### u2 alone: server CPU per statement, us (an insert/select-pk statement; update-range = one every-row UPDATE of 1,000 rows)

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| insert CPU us literal | 3 | 22.24 | 22.50 | 22.70 | 0.54 [0.42, 0.54] | +2.4 % | 0 of 3 | 0.38 / 0.41 | outside |
| insert CPU us prepared | 3 | 21.69 | 22.00 | 22.40 | 0.64 [0.37, 0.81] | +2.9 % | 0 of 3 | 0.60 / 0.76 | inside |
| select-pk CPU us literal | 3 | 31.25 | 31.35 | 31.34 | 0.09 [-0.51, 0.13] | +0.3 % | 1 of 3 | 0.23 / 0.83 | inside |
| select-pk CPU us prepared | 3 | 27.24 | 27.25 | 27.02 | 0.11 [-0.29, 0.22] | +0.4 % | 1 of 3 | 0.36 / 0.56 | inside |
| update-range CPU us literal | 3 | 721.34 | 719.86 | 756.05 | 37.42 [34.71, 44.08] | +5.2 % | 0 of 3 | 1.48 / 14.58 | outside |
| update-range CPU us prepared | 3 | 782.23 | 778.74 | 810.81 | 26.22 [21.08, 48.91] | +3.4 % | 0 of 3 | 10.99 / 17.18 | outside |

#### u2 alone: statement wall latency, us; median over runs of each run's percentile

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | literal | A | 5994 | 36.7 | 42.8 | 43.3 | 50.6 | 72.2 | 21657 | 22.2 | 24.3 |
| insert | literal | A2 | 5994 | 40.0 | 43.1 | 43.5 | 50.5 | 69.4 | 21667 | 22.5 | 24.0 |
| insert | literal | B | 5994 | 40.1 | 43.3 | 43.8 | 51.2 | 70.8 | 21502 | 22.7 | 24.1 |
| insert | prepared | A | 5994 | 35.1 | 40.4 | 41.0 | 47.8 | 67.5 | 22427 | 21.7 | 22.3 |
| insert | prepared | A2 | 5994 | 35.1 | 40.8 | 41.3 | 48.0 | 73.0 | 22616 | 22.0 | 22.3 |
| insert | prepared | B | 5994 | 36.5 | 40.9 | 41.4 | 50.0 | 70.7 | 22614 | 22.4 | 22.1 |
| select-pk | literal | A | 5994 | 54.0 | 56.3 | 61.6 | 70.1 | 86.5 | 15926 | 31.3 | 31.6 |
| select-pk | literal | A2 | 5994 | 54.1 | 56.3 | 61.4 | 69.8 | 89.4 | 15922 | 31.4 | 31.9 |
| select-pk | literal | B | 5994 | 53.7 | 56.2 | 61.7 | 70.4 | 87.1 | 15918 | 31.3 | 31.9 |
| select-pk | prepared | A | 5994 | 50.5 | 52.9 | 54.0 | 61.1 | 73.7 | 17784 | 27.2 | 29.2 |
| select-pk | prepared | A2 | 5994 | 51.0 | 53.0 | 53.8 | 60.6 | 72.5 | 17850 | 27.3 | 29.2 |
| select-pk | prepared | B | 5994 | 50.5 | 52.9 | 53.6 | 61.0 | 74.2 | 17789 | 27.0 | 29.3 |
| update-range | literal | A | 360 | 693.2 | 722.5 | 735.0 | 823.7 | 967.4 | 1319 | 721.3 | 37.5 |
| update-range | literal | A2 | 360 | 693.6 | 719.9 | 732.7 | 831.9 | 967.4 | 1311 | 719.9 | 38.0 |
| update-range | literal | B | 360 | 729.5 | 756.0 | 767.8 | 867.5 | 947.5 | 1256 | 756.0 | 37.8 |
| update-range | prepared | A | 360 | 693.6 | 723.2 | 733.5 | 839.8 | 2062.3 | 1233 | 782.2 | 31.5 |
| update-range | prepared | A2 | 360 | 690.4 | 716.8 | 729.2 | 858.6 | 1984.4 | 1205 | 778.7 | 34.2 |
| update-range | prepared | B | 360 | 716.6 | 751.0 | 763.5 | 887.8 | 2039.0 | 1174 | 810.8 | 33.9 |

bad replies: 0; end_state all ok: True; n runs 3
pages_allocated_delta (unspilled) per arm per run: [{'A': 0, 'A2': 0, 'B': 0}, {'A': 0, 'A2': 0, 'B': 0}, {'A': 0, 'A2': 0, 'B': 0}]
