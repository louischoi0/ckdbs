`send` path. QPS is derived as 1,000,000 / wall per statement of each run; medians of 3 runs per binary (A, B alternate start order); spread = (max - min) / median.

| shape | rows returned | A QPS (3 runs) | B QPS (3 runs) | A median | B median | B over A (QPS) | spread A / B | outside the larger spread? |
|---|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | 12,018 / 11,673 / 11,933 qps | 11,752 / 11,864 / 12,030 qps | 11,933 qps | 11,864 qps | -0.6 % | 2.9 % / 2.3 % | no |
| range-100 | 100 rows | 1,822 / 1,848 / 1,847 qps | 1,846 / 1,861 / 1,875 qps | 1,847 qps | 1,861 qps | +0.7 % | 1.4 % / 1.5 % | no |
| range-1000 | 1,000 rows | 208 / 213 / 214 qps | 213 / 211 / 208 qps | 213 qps | 211 qps | -1.3 % | 2.5 % / 2.1 % | no |
| range-10000 | 10,000 rows | 20.6 / 20.7 / 21.2 qps | 22.0 / 22.0 / 22.0 qps | 20.7 qps | 22.0 qps | +6.4 % | 3.1 % / 0.2 % | **yes** |
| range-60480 | 60,480 rows | 3.13 / 3.16 / 3.17 qps | 3.37 / 3.40 / 3.41 qps | 3.16 qps | 3.40 qps | +7.5 % | 1.3 % / 1.2 % | **yes** |
| filter-sym3 | 7,560 rows | 27.2 / 27.6 / 27.8 qps | 31.5 / 30.9 / 31.4 qps | 27.6 qps | 31.4 qps | +13.7 % | 2.1 % / 2.2 % | **yes** |

`split` path. QPS is derived as 1,000,000 / wall per statement of each run; medians of 3 runs per binary (A, B alternate start order); spread = (max - min) / median.

| shape | rows returned | A QPS (3 runs) | B QPS (3 runs) | A median | B median | B over A (QPS) | spread A / B | outside the larger spread? |
|---|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | 13,462 / 13,291 / 13,115 qps | 12,887 / 13,054 / 13,384 qps | 13,291 qps | 13,054 qps | -1.8 % | 2.6 % / 3.8 % | no |
| range-100 | 100 rows | 2,973 / 3,103 / 3,077 qps | 3,078 / 3,065 / 3,073 qps | 3,077 qps | 3,073 qps | -0.1 % | 4.2 % / 0.4 % | no |
| range-1000 | 1,000 rows | 376 / 376 / 383 qps | 379 / 382 / 377 qps | 376 qps | 379 qps | +0.7 % | 1.9 % / 1.3 % | no |
| range-10000 | 10,000 rows | 37.4 / 37.6 / 37.9 qps | 42.2 / 41.8 / 41.9 qps | 37.6 qps | 41.9 qps | +11.5 % | 1.2 % / 0.9 % | **yes** |
| range-60480 | 60,480 rows | 5.33 / 5.34 / 5.40 qps | 6.30 / 6.22 / 6.20 qps | 5.34 qps | 6.22 qps | +16.5 % | 1.4 % / 1.7 % | **yes** |
| filter-sym3 | 7,560 rows | 47.7 / 47.9 / 48.5 qps | 60.3 / 60.4 / 60.5 qps | 47.9 qps | 60.4 qps | +26.1 % | 1.7 % / 0.4 % | **yes** |

