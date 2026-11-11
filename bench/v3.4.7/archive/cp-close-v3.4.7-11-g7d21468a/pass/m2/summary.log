# CP-S0 M2: the slice `IS` per leaf page (`base` - `close`)

Source `/tmp/claude-1000/-home-louis-ckdbs/5bdaae98-e052-4814-8a2c-eb67c5cf878d/scratchpad/cp-close/run/20261111T094919Z/m2/census.json`: 5 run(s), blocks 12, scan_ops 768, point_ops 3000, cells rows-100k,wide-a, arms ['base', 'close'].
Load per run: [2.7, 2.7, 2.7, 2.7, 2.7] s; idle wait (s, us/s): {'base': [2.0, 8394.2], 'close': [4.0, 8126.3]}; quiet gate: [1.29, 1.29, 1.28, 1.15, 1.03].

## rows-100k (`s100k`, 100,000 rows, 603 leaf pages; 165.8 rows per page)

ANALYZE pages: base = 603, close = 603 (same); rows digest at run 0: same

| path | base scan CPU | close scan CPU | delta / stmt | delta / page | delta / row | share of base | point control (delta / stmt) | block spread base | block spread close | base wall p50 | close wall p50 | bad base/close |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| literal | 2155.7 [2150.3, 2189.8] us | 2096.2 [2071.4, 2227.3] us | 59.8 [-37.5, 82.9] us | 99 [-62, 138] ns | 0.60 [-0.37, 0.83] ns | 2.78 [-1.71, 3.85] % | -0.08 [-3.66, 0.52] us | 230.0 [147.4, 571.3] us | 125.7 [85.3, 1399.6] us | 2226.4 us | 2181.3 us | 0/0 |
| prepared | 2151.2 [2132.5, 2190.2] us | 2103.5 [2077.2, 2114.5] us | 54.0 [41.6, 75.6] us | 90 [69, 125] ns | 0.54 [0.42, 0.76] ns | 2.50 [1.94, 3.45] % | -0.29 [-3.64, 0.46] us | 134.1 [125.6, 348.2] us | 153.6 [138.1, 212.0] us | 2221.6 us | 2169.6 us | 0/0 |

Wall per statement, pooled over runs (us): base literal p50 2226.4 / p90 2461.0 / p99 3599.5; close literal p50 2181.3 / p90 2334.6 / p99 3702.5; base prepared p50 2221.6 / p90 2402.4 / p99 3111.2; close prepared p50 2169.6 / p90 2358.8 / p99 3109.8

## wide-a (`wd`, 10,000 rows, 304 leaf pages; 32.9 rows per page)

ANALYZE pages: base = 304, close = 304 (same); rows digest at run 0: same

| path | base scan CPU | close scan CPU | delta / stmt | delta / page | delta / row | share of base | point control (delta / stmt) | block spread base | block spread close | base wall p50 | close wall p50 | bad base/close |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| literal | 310.7 [306.4, 314.5] us | 284.7 [281.2, 288.4] us | 27.6 [21.7, 29.3] us | 91 [71, 96] ns | 2.76 [2.17, 2.93] ns | 8.77 [7.08, 9.42] % | -0.30 [-0.68, 0.33] us | 34.6 [21.2, 39.8] us | 34.2 [26.5, 37.9] us | 382.3 us | 355.1 us | 0/0 |
| prepared | 305.5 [302.1, 311.9] us | 280.7 [279.6, 284.1] us | 24.9 [21.4, 29.8] us | 82 [70, 98] ns | 2.49 [2.14, 2.98] ns | 8.16 [7.04, 9.63] % | -0.12 [-0.37, 0.16] us | 25.4 [21.6, 33.0] us | 28.8 [25.3, 47.7] us | 373.1 us | 348.4 us | 0/0 |

Wall per statement, pooled over runs (us): base literal p50 382.3 / p90 470.8 / p99 664.9; close literal p50 355.1 / p90 441.5 / p99 652.3; base prepared p50 373.1 / p90 448.4 / p99 619.9; close prepared p50 348.4 / p90 430.1 / p99 642.9

Reading: `delta / page` is the slice `IS`'s on-CPU price per leaf page at `cores = 1` (no partition latch exists at one core, `lock_table.hpp:511-512`); a `point control` of the same order as `delta / stmt` means the delta is inside the run's noise.
