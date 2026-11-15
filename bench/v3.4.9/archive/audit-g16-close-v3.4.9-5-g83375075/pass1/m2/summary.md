# CP-S0 M2: the slice `IS` per leaf page (`base` - `close`)

Source `/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad/run4/20261114T070437Z/m2/census.json`: 5 run(s), blocks 12, scan_ops 768, point_ops 3000, cells rows-100k,wide-a, arms ['base', 'close'].
Load per run: [2.7, 2.7, 2.7, 2.7, 2.7] s; idle wait (s, us/s): {'base': [2.0, 9464.2], 'close': [4.0, 9688.6]}; quiet gate: [1.91, 1.71, 1.56, 1.41, 1.15].

## rows-100k (`s100k`, 100,000 rows, 603 leaf pages; 165.8 rows per page)

ANALYZE pages: base = 603, close = 603 (same); rows digest at run 0: same

| path | base scan CPU | close scan CPU | delta / stmt | delta / page | delta / row | share of base | point control (delta / stmt) | block spread base | block spread close | base wall p50 | close wall p50 | bad base/close |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| literal | 2099.4 [2075.2, 2285.9] us | 2075.9 [2064.8, 2191.1] us | 15.2 [-87.5, 210.5] us | 25 [-145, 349] ns | 0.15 [-0.87, 2.10] ns | 0.73 [-4.16, 9.21] % | 0.12 [-0.08, 0.40] us | 219.7 [149.7, 701.5] us | 118.0 [100.5, 1485.4] us | 2180.8 us | 2171.2 us | 0/0 |
| prepared | 2090.9 [2078.1, 2284.9] us | 2068.9 [2059.1, 2078.9] us | 14.8 [9.1, 216.5] us | 24 [15, 359] ns | 0.15 [0.09, 2.17] ns | 0.71 [0.44, 9.48] % | -0.32 [-2.50, 0.14] us | 223.6 [110.9, 670.2] us | 112.5 [75.8, 203.9] us | 2179.0 us | 2165.9 us | 0/0 |

Wall per statement, pooled over runs (us): base literal p50 2180.8 / p90 2620.1 / p99 3059.3; close literal p50 2171.2 / p90 2314.5 / p99 3048.1; base prepared p50 2179.0 / p90 2626.5 / p99 3044.6; close prepared p50 2165.9 / p90 2289.2 / p99 2749.5

## wide-a (`wd`, 10,000 rows, 304 leaf pages; 32.9 rows per page)

ANALYZE pages: base = 304, close = 304 (same); rows digest at run 0: same

| path | base scan CPU | close scan CPU | delta / stmt | delta / page | delta / row | share of base | point control (delta / stmt) | block spread base | block spread close | base wall p50 | close wall p50 | bad base/close |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| literal | 284.0 [281.0, 295.9] us | 280.6 [277.6, 283.4] us | 4.6 [-0.6, 14.2] us | 15 [-2, 47] ns | 0.46 [-0.06, 1.42] ns | 1.61 [-0.20, 4.79] % | 0.05 [-0.28, 0.60] us | 44.4 [29.4, 64.0] us | 25.0 [22.1, 35.5] us | 370.0 us | 363.3 us | 0/0 |
| prepared | 278.4 [276.2, 291.9] us | 274.7 [270.6, 285.7] us | 6.1 [1.5, 9.8] us | 20 [5, 32] ns | 0.61 [0.15, 0.98] ns | 2.11 [0.56, 3.42] % | -0.15 [-0.31, 0.03] us | 31.8 [23.8, 55.4] us | 26.6 [21.8, 39.6] us | 360.3 us | 353.4 us | 0/0 |

Wall per statement, pooled over runs (us): base literal p50 370.0 / p90 458.8 / p99 642.2; close literal p50 363.3 / p90 448.1 / p99 628.6; base prepared p50 360.3 / p90 440.3 / p99 634.0; close prepared p50 353.4 / p90 430.0 / p99 643.6

Reading: `delta / page` is the slice `IS`'s on-CPU price per leaf page at `cores = 1` (no partition latch exists at one core, `lock_table.hpp:511-512`); a `point control` of the same order as `delta / stmt` means the delta is inside the run's noise.
