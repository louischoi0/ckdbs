# CP-S0 M2: the slice `IS` per leaf page (`base` - `noslice`)

Source `/tmp/claude-1000/-home-louis-ckdbs/1c794fd2-a838-4425-a96a-e3b26a73757b/scratchpad/cp-s0/run/20261108T072351Z/m2/census.json`: 5 run(s), blocks 12, scan_ops 768, point_ops 3000, cells rows-100k,wide-a, arms ['base', 'noslice'].
Load per run: [2.7, 2.7, 2.7, 2.7, 2.7] s; idle wait (s, us/s): {'base': [5.0, 9475.3], 'noslice': [2.0, 9475.6]}; quiet gate: [1.26, 1.06, 0.91, 1.12, 1.02].

## rows-100k (`s100k`, 100,000 rows, 603 leaf pages; 165.8 rows per page)

ANALYZE pages: base = 603, noslice = 603 (same); rows digest at run 0: same

| path | base scan CPU | noslice scan CPU | delta / stmt | delta / page | delta / row | share of base | point control (delta / stmt) | block spread base | block spread noslice | base wall p50 | noslice wall p50 | bad base/noslice |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| literal | 2155.2 [2139.0, 2195.3] us | 2105.0 [2082.4, 2157.2] us | 52.8 [38.1, 68.4] us | 88 [63, 113] ns | 0.53 [0.38, 0.68] ns | 2.47 [1.73, 3.12] % | 0.35 [-0.07, 0.98] us | 133.9 [94.2, 180.4] us | 176.5 [92.9, 185.8] us | 2257.4 us | 2198.1 us | 0/0 |
| prepared | 2168.3 [2148.2, 2207.8] us | 2115.2 [2080.6, 2195.5] us | 59.6 [-27.3, 73.4] us | 99 [-45, 122] ns | 0.60 [-0.27, 0.73] ns | 2.76 [-1.26, 3.36] % | 0.08 [-3.07, 0.16] us | 215.1 [197.3, 243.7] us | 183.2 [104.1, 1231.5] us | 2255.4 us | 2193.5 us | 0/0 |

Wall per statement, pooled over runs (us): base literal p50 2257.4 / p90 2445.0 / p99 2928.8; noslice literal p50 2198.1 / p90 2401.5 / p99 2880.1; base prepared p50 2255.4 / p90 2440.9 / p99 2968.1; noslice prepared p50 2193.5 / p90 2379.5 / p99 3471.9

## wide-a (`wd`, 10,000 rows, 304 leaf pages; 32.9 rows per page)

ANALYZE pages: base = 304, noslice = 304 (same); rows digest at run 0: same

| path | base scan CPU | noslice scan CPU | delta / stmt | delta / page | delta / row | share of base | point control (delta / stmt) | block spread base | block spread noslice | base wall p50 | noslice wall p50 | bad base/noslice |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| literal | 326.7 [309.5, 342.3] us | 285.8 [280.8, 299.5] us | 38.4 [27.2, 53.3] us | 126 [90, 175] ns | 3.84 [2.72, 5.33] ns | 12.03 [8.33, 15.58] % | -0.09 [-0.59, 0.44] us | 57.2 [36.5, 234.2] us | 27.7 [17.0, 44.2] us | 408.5 us | 376.0 us | 0/0 |
| prepared | 314.9 [298.5, 321.0] us | 279.7 [271.9, 294.4] us | 28.5 [22.8, 40.2] us | 94 [75, 132] ns | 2.85 [2.28, 4.02] ns | 9.04 [7.64, 12.87] % | 0.13 [-0.68, 0.26] us | 35.0 [17.7, 39.5] us | 38.9 [26.0, 54.9] us | 393.1 us | 360.2 us | 0/0 |

Wall per statement, pooled over runs (us): base literal p50 408.5 / p90 533.5 / p99 829.0; noslice literal p50 376.0 / p90 468.0 / p99 676.3; base prepared p50 393.1 / p90 477.6 / p99 685.6; noslice prepared p50 360.2 / p90 445.7 / p99 620.9

Reading: `delta / page` is the slice `IS`'s on-CPU price per leaf page at `cores = 1` (no partition latch exists at one core, `lock_table.hpp:511-512`); a `point control` of the same order as `delta / stmt` means the delta is inside the run's noise.
