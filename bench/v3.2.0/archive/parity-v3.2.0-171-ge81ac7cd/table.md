KDS idle server CPU: 6,455 µs per second

| shape | rows/op | KDS p50 | PG p50 | KDS/PG qps | KDS server CPU (net of idle) | PG backend CPU | KDS/PG server CPU | KDS client CPU | PG client CPU |
|---|---|---|---|---|---|---|---|---|---|
| pk-lookup | 1 rows | 48.0 µs | 62.8 µs | 1.30x | 29.6 µs (29.3 µs) | 45.9 µs | 0.64x | 19.9 µs | 18.8 µs |
| bar-range | 201 rows | 305.7 µs | 432.0 µs | 1.42x | 104.7 µs (102.7 µs) | 138.7 µs | 0.74x | 204.7 µs | 339.3 µs |
| symbol-history | 7,560 rows | 10,379.0 µs | 13,156.6 µs | 1.26x | 3,744.4 µs (3,677.6 µs) | 4,216.2 µs | 0.87x | 6,819.2 µs | 12,042.0 µs |
| day-slice | 8 rows | 1,523.5 µs | 1,862.4 µs | 1.22x | 1,503.3 µs (1,493.4 µs) | 1,833.4 µs | 0.81x | 33.6 µs | 38.0 µs |
| point-join | 1 rows | 60.6 µs | 134.2 µs | 2.19x | 42.3 µs (41.9 µs) | 119.5 µs | 0.35x | 20.1 µs | 19.0 µs |
| agg-global | 1 rows | 5,843.7 µs | 3,573.4 µs | 0.61x | 5,813.7 µs (5,775.9 µs) | 3,549.2 µs | 1.63x | 35.5 µs | 28.6 µs |
| agg-by-session | 7,560 rows | 13,699.8 µs | 14,647.6 µs | 1.07x | 11,108.1 µs (11,019.5 µs) | 9,119.5 µs | 1.21x | 2,598.2 µs | 7,302.6 µs |
