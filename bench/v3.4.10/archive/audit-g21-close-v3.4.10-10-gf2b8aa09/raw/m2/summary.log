# CP-S0 M2: the slice `IS` per leaf page (`base` - `close`)

Source `/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad/g21/out/20261115T074119Z/m2/census.json`: 5 run(s), blocks 12, scan_ops 768, point_ops 3000, cells rows-100k,wide-a, arms ['base', 'close'].
Load per run: [3.0, 2.7, 2.7, 2.7, 2.7] s; idle wait (s, us/s): {'base': [2.0, 7370.2], 'close': [3.0, 7682.8]}; quiet gate: [1.96, 6.48, 2.25, 1.75, 1.68].

## rows-100k (`s100k`, 100,000 rows, 603 leaf pages; 165.8 rows per page)

ANALYZE pages: base = 603, close = 603 (same); rows digest at run 0: same

| path | base scan CPU | close scan CPU | delta / stmt | delta / page | delta / row | share of base | point control (delta / stmt) | block spread base | block spread close | base wall p50 | close wall p50 | bad base/close |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| literal | 2099.3 [2081.1, 2219.1] us | 2106.2 [2081.8, 2332.5] us | -6.9 [-113.4, 16.6] us | -11 [-188, 28] ns | -0.07 [-1.13, 0.17] ns | -0.33 [-5.11, 0.79] % | -0.04 [-2.14, 0.29] us | 177.5 [109.4, 591.3] us | 168.7 [159.2, 975.5] us | 2216.2 us | 2219.0 us | 0/0 |
| prepared | 2117.2 [2078.0, 2236.1] us | 2113.7 [2095.6, 2592.1] us | -27.7 [-356.0, 3.7] us | -46 [-590, 6] ns | -0.28 [-3.56, 0.04] ns | -1.30 [-15.92, 0.17] % | 0.24 [-0.40, 0.98] us | 120.8 [111.9, 668.1] us | 253.9 [136.6, 1578.4] us | 2212.1 us | 2214.3 us | 0/0 |

Wall per statement, pooled over runs (us): base literal p50 2216.2 / p90 2654.4 / p99 4332.5; close literal p50 2219.0 / p90 2716.9 / p99 6099.2; base prepared p50 2212.1 / p90 2687.0 / p99 4313.8; close prepared p50 2214.3 / p90 2825.8 / p99 5539.4

## wide-a (`wd`, 10,000 rows, 304 leaf pages; 32.9 rows per page)

ANALYZE pages: base = 304, close = 304 (same); rows digest at run 0: same

| path | base scan CPU | close scan CPU | delta / stmt | delta / page | delta / row | share of base | point control (delta / stmt) | block spread base | block spread close | base wall p50 | close wall p50 | bad base/close |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| literal | 293.1 [284.8, 319.3] us | 292.2 [286.4, 353.2] us | -4.1 [-33.9, 8.3] us | -13 [-111, 27] ns | -0.41 [-3.39, 0.83] ns | -1.39 [-10.61, 2.82] % | 0.21 [-1.16, 0.49] us | 37.1 [30.8, 247.5] us | 38.4 [26.5, 221.8] us | 400.4 us | 401.7 us | 0/0 |
| prepared | 288.1 [281.4, 334.4] us | 287.2 [282.5, 340.4] us | -5.9 [-6.9, 7.4] us | -19 [-23, 24] ns | -0.59 [-0.69, 0.74] ns | -1.79 [-2.40, 2.53] % | 0.31 [-0.31, 0.47] us | 33.0 [25.6, 232.8] us | 34.8 [23.5, 229.5] us | 393.1 us | 393.0 us | 0/0 |

Wall per statement, pooled over runs (us): base literal p50 400.4 / p90 574.2 / p99 802.9; close literal p50 401.7 / p90 607.7 / p99 789.1; base prepared p50 393.1 / p90 574.3 / p99 832.3; close prepared p50 393.0 / p90 598.6 / p99 796.7

Reading: `delta / page` is the slice `IS`'s on-CPU price per leaf page at `cores = 1` (no partition latch exists at one core, `lock_table.hpp:511-512`); a `point control` of the same order as `delta / stmt` means the delta is inside the run's noise.
