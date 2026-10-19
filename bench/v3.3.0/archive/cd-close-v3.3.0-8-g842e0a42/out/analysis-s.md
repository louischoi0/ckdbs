
#### scan 10,000 rows (bn), s1+s2 pooled: net scan row, ns of server CPU per row, (scan CPU - point SELECT CPU on the same relation) / rows; point SELECT CPU in us

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B above A | A' - A typical / largest abs gap | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| bn (10000) net row ns literal | 7 | 25.86 | 25.71 | 25.59 | 0.10 [-1.58, 0.23] | +0.4 % | 4 of 7 | 0.12 / 1.32 | inside |
| bn (10000) net row ns prepared | 7 | 26.01 | 25.77 | 25.67 | -0.22 [-1.74, 0.15] | -0.8 % | 3 of 7 | 0.23 / 1.80 | inside |
| point:s CPU us literal | 7 | 30.99 | 31.06 | 30.99 | -0.09 [-1.13, 0.92] | -0.3 % | 3 of 7 | 1.03 / 1.42 | inside |
| point:s CPU us prepared | 7 | 28.88 | 29.16 | 28.94 | -0.32 [-1.26, 0.68] | -1.1 % | 3 of 7 | 0.73 / 1.60 | inside |

#### scan 10,000 rows (bn), s1+s2 pooled: latency of one scan (wall, client side), us; median over runs of each run's percentile

| cell | path | arm | ops | p0 | p25 | p50 | p95 | p99 | scans/s at p50 (derived) | server CPU us/scan |
|---|---|---|---|---|---|---|---|---|---|---|
| bn | literal | A | 672 | 303.6 | 311.4 | 321.5 | 362.4 | 401.7 | 3111 | 290.4 |
| bn | literal | A2 | 672 | 303.9 | 310.8 | 320.2 | 357.6 | 398.9 | 3124 | 288.0 |
| bn | literal | B | 672 | 302.3 | 311.2 | 321.4 | 354.2 | 400.7 | 3111 | 286.8 |
| bn | prepared | A | 672 | 300.5 | 308.9 | 319.3 | 366.2 | 401.2 | 3132 | 290.0 |
| bn | prepared | A2 | 672 | 299.6 | 307.3 | 319.7 | 349.1 | 403.9 | 3128 | 286.9 |
| bn | prepared | B | 672 | 299.9 | 308.3 | 318.0 | 351.2 | 392.1 | 3144 | 285.6 |

bad replies: 0; window_wait_us total: 0; scan pool_misses total: 0; digest arms-distinct per cell: [('bn', 1)]; n runs 7; unclean windows: 0 []

#### scan 10,000 rows (bn), s1 alone (start order A, A', B): net scan row, ns of server CPU per row, (scan CPU - point SELECT CPU on the same relation) / rows; point SELECT CPU in us

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B above A | A' - A typical / largest abs gap | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| bn (10000) net row ns literal | 3 | 25.47 | 25.56 | 25.59 | 0.12 [-0.62, 0.23] | +0.5 % | 2 of 3 | 0.09 / 0.12 | **OUTSIDE** |
| bn (10000) net row ns prepared | 3 | 25.70 | 25.68 | 25.66 | 0.01 [-0.57, 0.15] | +0.0 % | 2 of 3 | 0.23 / 0.24 | inside |
| point:s CPU us literal | 3 | 30.99 | 30.60 | 30.90 | -0.09 [-1.13, 0.73] | -0.3 % | 1 of 3 | 1.19 / 1.20 | inside |
| point:s CPU us prepared | 3 | 28.74 | 29.16 | 28.60 | -0.32 [-1.26, 0.64] | -1.1 % | 1 of 3 | 0.73 / 0.75 | inside |

#### scan 10,000 rows (bn), s1 alone (start order A, A', B): latency of one scan (wall, client side), us; median over runs of each run's percentile

| cell | path | arm | ops | p0 | p25 | p50 | p95 | p99 | scans/s at p50 (derived) | server CPU us/scan |
|---|---|---|---|---|---|---|---|---|---|---|
| bn | literal | A | 288 | 303.8 | 308.8 | 320.0 | 352.1 | 401.7 | 3125 | 285.5 |
| bn | literal | A2 | 288 | 303.9 | 310.8 | 320.2 | 357.6 | 398.9 | 3124 | 287.0 |
| bn | literal | B | 288 | 302.3 | 310.2 | 321.4 | 352.0 | 400.7 | 3111 | 286.8 |
| bn | prepared | A | 288 | 299.3 | 305.2 | 315.6 | 352.2 | 381.4 | 3169 | 285.7 |
| bn | prepared | A2 | 288 | 299.6 | 307.3 | 318.7 | 341.7 | 394.6 | 3138 | 286.3 |
| bn | prepared | B | 288 | 299.7 | 307.0 | 316.4 | 346.1 | 402.3 | 3160 | 285.4 |

bad replies: 0; window_wait_us total: 0; scan pool_misses total: 0; digest arms-distinct per cell: [('bn', 1)]; n runs 3; unclean windows: 0 []

#### scan 10,000 rows (bn), s2 alone (start order B, A', A): net scan row, ns of server CPU per row, (scan CPU - point SELECT CPU on the same relation) / rows; point SELECT CPU in us

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B above A | A' - A typical / largest abs gap | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| bn (10000) net row ns literal | 4 | 26.47 | 25.93 | 25.80 | -0.35 [-1.58, 0.14] | -1.3 % | 2 of 4 | 0.48 / 1.32 | inside |
| bn (10000) net row ns prepared | 4 | 26.60 | 26.21 | 25.90 | -0.77 [-1.74, 0.12] | -2.9 % | 1 of 4 | 0.81 / 1.80 | inside |
| point:s CPU us literal | 4 | 31.16 | 31.08 | 31.01 | -0.07 [-0.97, 0.92] | -0.2 % | 2 of 4 | 0.83 / 1.42 | inside |
| point:s CPU us prepared | 4 | 29.34 | 29.19 | 28.94 | -0.10 [-1.17, 0.68] | -0.4 % | 2 of 4 | 0.72 / 1.60 | inside |

#### scan 10,000 rows (bn), s2 alone (start order B, A', A): latency of one scan (wall, client side), us; median over runs of each run's percentile

| cell | path | arm | ops | p0 | p25 | p50 | p95 | p99 | scans/s at p50 (derived) | server CPU us/scan |
|---|---|---|---|---|---|---|---|---|---|---|
| bn | literal | A | 384 | 302.9 | 312.8 | 324.0 | 377.6 | 501.4 | 3087 | 295.7 |
| bn | literal | A2 | 384 | 304.5 | 315.5 | 324.0 | 361.9 | 393.4 | 3086 | 290.6 |
| bn | literal | B | 384 | 303.0 | 315.4 | 323.1 | 354.8 | 397.9 | 3095 | 288.9 |
| bn | prepared | A | 384 | 300.8 | 311.7 | 321.9 | 413.3 | 480.8 | 3106 | 294.5 |
| bn | prepared | A2 | 384 | 300.1 | 308.4 | 321.4 | 356.3 | 449.5 | 3112 | 291.5 |
| bn | prepared | B | 384 | 300.0 | 311.8 | 320.5 | 353.4 | 386.5 | 3120 | 287.9 |

bad replies: 0; window_wait_us total: 0; scan pool_misses total: 0; digest arms-distinct per cell: [('bn', 1)]; n runs 4; unclean windows: 0 []

#### scan 200 / 1,000 rows: net scan row, ns of server CPU per row, (scan CPU - point SELECT CPU on the same relation) / rows; point SELECT CPU in us

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B above A | A' - A typical / largest abs gap | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| rows-200 (200) net row ns literal | 2 | 29.64 | 30.35 | 28.90 | -0.74 [-2.53, 1.04] | -2.5 % | 1 of 2 | 0.70 / 0.73 | **OUTSIDE** |
| rows-200 (200) net row ns prepared | 2 | 29.11 | 29.05 | 29.75 | 0.64 [-2.64, 3.91] | +2.2 % | 1 of 2 | 3.21 / 3.27 | inside |
| rows-1k (1000) net row ns literal | 2 | 26.35 | 24.48 | 25.26 | -1.09 [-2.72, 0.55] | -4.1 % | 1 of 2 | 1.88 / 3.68 | inside |
| rows-1k (1000) net row ns prepared | 2 | 24.26 | 24.17 | 23.97 | -0.29 [-0.90, 0.32] | -1.2 % | 1 of 2 | 1.04 / 1.14 | inside |
| point:s200 CPU us literal | 2 | 29.50 | 28.50 | 28.87 | -0.63 [-0.80, -0.46] | -2.1 % | 0 of 2 | 1.00 / 1.22 | inside |
| point:s200 CPU us prepared | 2 | 28.95 | 27.65 | 28.19 | -0.76 [-0.89, -0.64] | -2.6 % | 0 of 2 | 1.31 / 1.54 | inside |
| point:s1k CPU us literal | 2 | 31.60 | 30.91 | 30.90 | -0.70 [-0.92, -0.49] | -2.2 % | 0 of 2 | 0.69 / 0.80 | inside |
| point:s1k CPU us prepared | 2 | 32.41 | 31.46 | 31.74 | -0.67 [-0.82, -0.52] | -2.1 % | 0 of 2 | 0.95 / 1.14 | inside |

#### scan 200 / 1,000 rows: latency of one scan (wall, client side), us; median over runs of each run's percentile

| cell | path | arm | ops | p0 | p25 | p50 | p95 | p99 | scans/s at p50 (derived) | server CPU us/scan |
|---|---|---|---|---|---|---|---|---|---|---|
| rows-200 | literal | A | 1536 | 61.6 | 63.4 | 64.5 | 82.2 | 97.3 | 15513 | 35.4 |
| rows-200 | literal | A2 | 1536 | 60.8 | 62.6 | 63.7 | 81.9 | 99.3 | 15702 | 34.6 |
| rows-200 | literal | B | 1536 | 61.2 | 63.0 | 64.0 | 81.7 | 95.1 | 15626 | 34.7 |
| rows-200 | prepared | A | 1536 | 58.8 | 60.1 | 61.2 | 78.1 | 94.5 | 16341 | 34.8 |
| rows-200 | prepared | A2 | 1536 | 57.4 | 59.1 | 60.0 | 77.8 | 93.9 | 16667 | 33.5 |
| rows-200 | prepared | B | 1536 | 57.8 | 59.5 | 60.5 | 78.6 | 95.6 | 16516 | 34.1 |
| rows-1k | literal | A | 768 | 82.5 | 84.3 | 85.8 | 112.5 | 140.6 | 11650 | 58.0 |
| rows-1k | literal | A2 | 768 | 80.9 | 83.2 | 84.5 | 102.0 | 129.2 | 11838 | 55.4 |
| rows-1k | literal | B | 768 | 81.2 | 83.6 | 85.1 | 104.5 | 125.2 | 11752 | 56.2 |
| rows-1k | prepared | A | 768 | 79.7 | 81.2 | 82.4 | 101.4 | 128.1 | 12134 | 56.7 |
| rows-1k | prepared | A2 | 768 | 77.8 | 80.0 | 81.8 | 105.6 | 150.3 | 12232 | 55.6 |
| rows-1k | prepared | B | 768 | 78.2 | 80.4 | 81.6 | 100.9 | 115.8 | 12255 | 55.7 |

bad replies: 0; window_wait_us total: 0; scan pool_misses total: 0; digest arms-distinct per cell: [('rows-1k', 1), ('rows-200', 1)]; n runs 2; unclean windows: 0 []
