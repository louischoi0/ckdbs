## Supplement: the `drain` client path (frame headers only; the wire and the server, no client work)

Runs per binary: {'A': 3, 'B': 3}. Wall = the client's per statement; server CPU from /proc/<pid>/stat (10 ms ticks).

| shape | rows returned | statements per run | A wall, runs | B wall, runs | A median | B median | B over A | spread A / B | a finding? | A server CPU | B server CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | 15132 | 48 / 48 / 48 µs | 48 / 49 / 48 µs | 47.9 µs | 48.4 µs | +1.0 % | 1.6 % / 1.5 % | no (inside the spread) | 31.2 µs | 31.7 µs |
| range-100 | 100 rows | 8965 | 93 / 94 / 94 µs | 94 / 95 / 95 µs | 93.5 µs | 94.8 µs | +1.3 % | 0.9 % / 1.2 % | **yes** | 75.0 µs | 76.7 µs |
| range-1000 | 1,000 rows | 3550 | 477 / 480 / 473 µs | 503 / 495 / 498 µs | 477.5 µs | 497.7 µs | +4.2 % | 1.5 % / 1.6 % | **yes** | 452.8 µs | 474.9 µs |
| range-10000 | 10,000 rows | 533 | 4,264 / 4,252 / 4,264 µs | 4,378 / 4,299 / 4,375 µs | 4,263.8 µs | 4,375.3 µs | +2.6 % | 0.3 % / 1.8 % | **yes** | 4,232.2 µs | 4,337.8 µs |
| range-60480 | 60,480 rows | 60 | 28,886 / 28,277 / 28,364 µs | 27,158 / 26,582 / 26,684 µs | 28,364.2 µs | 26,683.7 µs | -5.9 % | 2.1 % / 2.2 % | **yes** | 28,474.6 µs | 26,703.3 µs |
| filter-sym3 | 7,560 rows | 477 | 4,538 / 4,589 / 4,526 µs | 4,735 / 4,707 / 4,756 µs | 4,537.7 µs | 4,735.1 µs | +4.4 % | 1.4 % / 1.0 % | **yes** | 4,505.3 µs | 4,710.9 µs |

Distribution of one statement (all statements of the runs pooled):

| shape | rows returned | binary | statements | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | A | 45,425 | 43.5 µs | 46.1 µs | 46.5 µs | 58.2 µs | 68.1 µs |
| range-1 | 1 rows | B | 44,118 | 44.8 µs | 46.8 µs | 47.4 µs | 57.7 µs | 66.6 µs |
| range-100 | 100 rows | A | 26,835 | 82.1 µs | 88.4 µs | 91.3 µs | 107.4 µs | 137.1 µs |
| range-100 | 100 rows | B | 27,791 | 81.4 µs | 89.4 µs | 92.2 µs | 109.8 µs | 121.9 µs |
| range-1000 | 1,000 rows | A | 10,662 | 449.0 µs | 463.0 µs | 469.7 µs | 497.5 µs | 631.5 µs |
| range-1000 | 1,000 rows | B | 10,180 | 468.5 µs | 484.1 µs | 490.6 µs | 521.3 µs | 720.4 µs |
| range-10000 | 10,000 rows | A | 1,595 | 4,160.2 µs | 4,214.5 µs | 4,228.8 µs | 4,341.7 µs | 4,761.9 µs |
| range-10000 | 10,000 rows | B | 1,573 | 4,168.3 µs | 4,300.0 µs | 4,339.8 µs | 4,431.7 µs | 4,652.8 µs |
| range-60480 | 60,480 rows | A | 179 | 27,700.3 µs | 28,121.0 µs | 28,533.3 µs | 29,178.2 µs | 29,764.0 µs |
| range-60480 | 60,480 rows | B | 266 | 25,911.0 µs | 26,506.3 µs | 26,781.9 µs | 27,386.0 µs | 27,755.2 µs |
| filter-sym3 | 7,560 rows | A | 1,430 | 4,424.9 µs | 4,487.4 µs | 4,525.8 µs | 4,644.6 µs | 4,864.3 µs |
| filter-sym3 | 7,560 rows | B | 1,397 | 4,592.4 µs | 4,671.2 µs | 4,706.3 µs | 4,871.5 µs | 5,259.1 µs |
