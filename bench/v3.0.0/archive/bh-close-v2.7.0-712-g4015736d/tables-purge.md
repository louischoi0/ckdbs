**durability = relaxed**
| cores | statement | relation rows | runs x statements | PURGE keys freed per second, median [min, max] | PURGE per key | PURGE statement p0 / p25 / p50 / p95 / p99 | DELETE keys marked per second, median | DELETE statement p50 |
|---|---|---|---|---|---|---|---|---|
| 1 | `PURGE ... WHERE id = k` | 200 | 5 x 200 | 18,023 keys/s [17,799, 18,111] | 55.48 µs | 41.2 µs / 50.0 µs / 54.9 µs / 65.2 µs / 72.3 µs | 19,875 keys/s | 48.5 µs |
| 1 | `PURGE ... WHERE id = k` | 1,000 | 5 x 1,000 | 17,660 keys/s [17,395, 17,824] | 56.62 µs | 40.8 µs / 52.3 µs / 56.0 µs / 64.5 µs / 72.6 µs | 20,284 keys/s | 48.0 µs |
| 1 | `PURGE ... WHERE id = k` | 10,000 | 5 x 10,000 | 17,219 keys/s [17,089, 17,414] | 58.08 µs | 40.8 µs / 52.2 µs / 56.4 µs / 65.9 µs / 75.1 µs | 19,654 keys/s | 48.2 µs |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+199` | 30,000 | 5 x 150 | 166,683 keys/s [165,220, 167,568] | 6.00 µs | 1,048.2 µs / 1,126.6 µs / 1,201.1 µs / 1,326.1 µs / 1,347.8 µs | 110,724 keys/s | 1,805.3 µs |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+999` | 30,000 | 5 x 30 | 171,708 keys/s [152,162, 172,976] | 5.82 µs | 5.73 ms / 5.79 ms / 5.82 ms / 5.88 ms / 5.91 ms | 466,268 keys/s | 2.08 ms |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+9999` | 30,000 | 5 x 3 | 175,112 keys/s [174,912, 175,216] | 5.71 µs | 57.00 ms / 57.00 ms / 57.10 ms / 57.22 ms / 57.22 ms | 1,481,561 keys/s | 6.71 ms |
| 2 | `PURGE ... WHERE id = k` | 200 | 5 x 200 | 17,719 keys/s [16,963, 17,892] | 56.44 µs | 45.6 µs / 50.2 µs / 55.7 µs / 66.1 µs / 74.5 µs | 19,835 keys/s | 48.8 µs |
| 2 | `PURGE ... WHERE id = k` | 1,000 | 5 x 1,000 | 17,456 keys/s [17,168, 17,560] | 57.29 µs | 41.7 µs / 52.5 µs / 56.8 µs / 66.4 µs / 77.2 µs | 20,117 keys/s | 48.3 µs |
| 2 | `PURGE ... WHERE id = k` | 10,000 | 5 x 10,000 | 17,114 keys/s [16,779, 17,147] | 58.43 µs | 40.8 µs / 52.4 µs / 56.7 µs / 67.2 µs / 78.1 µs | 19,585 keys/s | 48.1 µs |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+199` | 30,000 | 5 x 150 | 144,744 keys/s [144,038, 160,340] | 6.91 µs | 1,207.1 µs / 1,302.2 µs / 1,377.0 µs / 1,524.5 µs / 1,554.0 µs | 95,618 keys/s | 2.08 ms |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+999` | 30,000 | 5 x 30 | 149,083 keys/s [148,974, 169,268] | 6.71 µs | 6.61 ms / 6.69 ms / 6.71 ms / 6.75 ms / 6.79 ms | 390,191 keys/s | 2.54 ms |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+9999` | 30,000 | 5 x 3 | 170,341 keys/s [151,187, 172,300] | 5.87 µs | 58.62 ms / 58.62 ms / 58.67 ms / 58.81 ms / 58.81 ms | 1,165,851 keys/s | 8.54 ms |
**durability = strict**
| cores | statement | relation rows | runs x statements | PURGE keys freed per second, median [min, max] | PURGE per key | PURGE statement p0 / p25 / p50 / p95 / p99 | DELETE keys marked per second, median | DELETE statement p50 |
|---|---|---|---|---|---|---|---|---|
| 1 | `PURGE ... WHERE id = k` | 1,000 | 3 x 1,000 | 711 keys/s [690, 733] | 1,406.07 µs | 1,032.2 µs / 1,169.5 µs / 1,232.6 µs / 1,906.3 µs / 6.01 ms | 714 keys/s | 1,225.5 µs |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+999` | 10,000 | 3 x 10 | 132,877 keys/s [130,374, 134,361] | 7.53 µs | 7.31 ms / 7.41 ms / 7.52 ms / 7.73 ms / 7.73 ms | 308,981 keys/s | 3.26 ms |
**DELETE commit to PURGE success**
