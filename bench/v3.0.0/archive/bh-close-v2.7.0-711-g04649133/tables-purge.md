
**durability = relaxed**

| cores | statement | relation rows | runs x statements | PURGE keys freed per second, median [min, max] | PURGE per key | PURGE statement p0 / p25 / p50 / p95 / p99 | DELETE keys marked per second, median | DELETE statement p50 |
|---|---|---|---|---|---|---|---|---|
| 1 | `PURGE ... WHERE id = k` | 200 | 5 x 200 | 18,114 keys/s [18,039, 18,194] | 55.21 µs | 41.2 µs / 49.7 µs / 54.9 µs / 64.3 µs / 72.0 µs | 20,201 keys/s | 48.2 µs |
| 1 | `PURGE ... WHERE id = k` | 1,000 | 5 x 1,000 | 17,746 keys/s [17,678, 17,939] | 56.35 µs | 40.8 µs / 51.8 µs / 55.7 µs / 64.2 µs / 73.5 µs | 20,255 keys/s | 48.2 µs |
| 1 | `PURGE ... WHERE id = k` | 10,000 | 5 x 10,000 | 17,430 keys/s [17,165, 17,515] | 57.37 µs | 40.5 µs / 51.7 µs / 55.9 µs / 65.4 µs / 73.6 µs | 19,722 keys/s | 48.3 µs |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+199` | 30,000 | 5 x 150 | 164,824 keys/s [151,236, 166,488] | 6.07 µs | 1,050.5 µs / 1,134.6 µs / 1,209.9 µs / 1,340.1 µs / 1,378.0 µs | 116,330 keys/s | 1,705.8 µs |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+999` | 30,000 | 5 x 30 | 172,196 keys/s [171,606, 172,312] | 5.81 µs | 5.72 ms / 5.78 ms / 5.80 ms / 5.86 ms / 5.87 ms | 489,982 keys/s | 2.03 ms |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+9999` | 30,000 | 5 x 3 | 175,326 keys/s [172,920, 177,206] | 5.70 µs | 56.96 ms / 56.96 ms / 57.02 ms / 57.08 ms / 57.08 ms | 1,509,577 keys/s | 6.63 ms |
| 2 | `PURGE ... WHERE id = k` | 200 | 5 x 200 | 17,665 keys/s [17,571, 17,940] | 56.61 µs | 41.8 µs / 50.7 µs / 55.8 µs / 65.8 µs / 74.3 µs | 19,780 keys/s | 48.9 µs |
| 2 | `PURGE ... WHERE id = k` | 1,000 | 5 x 1,000 | 17,610 keys/s [17,341, 17,670] | 56.79 µs | 41.7 µs / 52.3 µs / 56.1 µs / 65.9 µs / 73.4 µs | 19,893 keys/s | 48.7 µs |
| 2 | `PURGE ... WHERE id = k` | 10,000 | 5 x 10,000 | 17,216 keys/s [16,073, 17,391] | 58.09 µs | 40.6 µs / 52.3 µs / 56.3 µs / 66.8 µs / 78.0 µs | 19,596 keys/s | 48.3 µs |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+199` | 30,000 | 5 x 150 | 161,956 keys/s [143,354, 163,078] | 6.17 µs | 1,077.3 µs / 1,161.0 µs / 1,236.0 µs / 1,365.8 µs / 1,397.9 µs | 99,539 keys/s | 2.02 ms |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+999` | 30,000 | 5 x 30 | 166,689 keys/s [164,568, 169,080] | 6.00 µs | 5.88 ms / 5.97 ms / 6.00 ms / 6.09 ms / 6.10 ms | 402,289 keys/s | 2.47 ms |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+9999` | 30,000 | 5 x 3 | 170,958 keys/s [149,787, 172,047] | 5.85 µs | 58.44 ms / 58.44 ms / 58.46 ms / 58.56 ms / 58.56 ms | 1,180,141 keys/s | 8.51 ms |

**durability = group**

| cores | statement | relation rows | runs x statements | PURGE keys freed per second, median [min, max] | PURGE per key | PURGE statement p0 / p25 / p50 / p95 / p99 | DELETE keys marked per second, median | DELETE statement p50 |
|---|---|---|---|---|---|---|---|---|
| 1 | `PURGE ... WHERE id = k` | 1,000 | 3 x 1,000 | 706 keys/s [691, 733] | 1,416.83 µs | 1,042.2 µs / 1,187.9 µs / 1,246.2 µs / 1,918.6 µs / 4.87 ms | 723 keys/s | 1,229.4 µs |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+999` | 10,000 | 3 x 10 | 126,942 keys/s [116,130, 129,136] | 7.88 µs | 7.45 ms / 7.55 ms / 7.64 ms / 10.54 ms / 10.54 ms | 264,679 keys/s | 3.21 ms |
| 2 | `PURGE ... WHERE id = k` | 1,000 | 3 x 1,000 | 719 keys/s [718, 744] | 1,390.43 µs | 1,031.7 µs / 1,181.4 µs / 1,224.6 µs / 1,725.8 µs / 4.74 ms | 711 keys/s | 1,216.5 µs |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+999` | 10,000 | 3 x 10 | 128,668 keys/s [117,994, 129,430] | 7.77 µs | 7.60 ms / 7.64 ms / 7.65 ms / 8.39 ms / 8.39 ms | 269,830 keys/s | 3.43 ms |

**durability = strict**

| cores | statement | relation rows | runs x statements | PURGE keys freed per second, median [min, max] | PURGE per key | PURGE statement p0 / p25 / p50 / p95 / p99 | DELETE keys marked per second, median | DELETE statement p50 |
|---|---|---|---|---|---|---|---|---|
| 1 | `PURGE ... WHERE id = k` | 1,000 | 3 x 1,000 | 726 keys/s [719, 738] | 1,377.79 µs | 1,040.2 µs / 1,169.2 µs / 1,229.2 µs / 1,895.4 µs / 5.08 ms | 708 keys/s | 1,241.3 µs |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+999` | 10,000 | 3 x 10 | 122,029 keys/s [116,480, 133,790] | 8.19 µs | 7.41 ms / 7.57 ms / 7.64 ms / 10.23 ms / 10.23 ms | 277,409 keys/s | 3.22 ms |
| 2 | `PURGE ... WHERE id = k` | 1,000 | 3 x 1,000 | 704 keys/s [694, 735] | 1,420.66 µs | 1,057.4 µs / 1,202.3 µs / 1,249.3 µs / 1,906.3 µs / 4.83 ms | 728 keys/s | 1,232.1 µs |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+999` | 10,000 | 3 x 10 | 123,342 keys/s [107,225, 128,291] | 8.11 µs | 7.47 ms / 7.57 ms / 7.63 ms / 11.74 ms / 11.74 ms | 268,869 keys/s | 3.44 ms |


**DELETE commit to PURGE success**

| cores | background load | reps x samples | p0 | p25 | p50 | p95 | p99 | worst single sample (any rep) | PURGE first attempt p50 / p99 | DELETE p50 / p99 | TXN_CONFLICT refusals | each reader: throughput, p50 | writer throughput |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | none (idle server) | 3 x 20,000 | 41.9 µs | 53.9 µs | 57.8 µs | 68.4 µs | 77.8 µs | 22.6 ms | 57.6 / 77.6 µs | 56.7 / 77.0 µs | 0 of 60,000 | - | - |
| 1 | 2 readers + 1 writer | 3 x 20,000 | 48.4 µs | 93.2 µs | 99.7 µs | 125.9 µs | 165.5 µs | 33.7 ms | 99.3 / 165.1 µs | 97.9 / 164.8 µs | 0 of 60,000 | 9,557 qps, 96 µs | 9,984 qps |
| 1 | 3 readers | 3 x 20,000 | 44.5 µs | 87.2 µs | 98.3 µs | 125.5 µs | 156.0 µs | 32.2 ms | 97.9 / 155.6 µs | 95.7 / 151.8 µs | 0 of 60,000 | 9,673 qps, 96 µs | - |
| 2 | none (idle server) | 3 x 20,000 | 44.1 µs | 54.6 µs | 58.5 µs | 69.6 µs | 78.9 µs | 17.5 ms | 58.3 / 78.6 µs | 57.4 / 78.5 µs | 0 of 60,000 | - | - |
| 2 | 2 readers + 1 writer | 3 x 20,000 | 41.5 µs | 75.1 µs | 82.8 µs | 112.2 µs | 131.1 µs | 105.3 ms | 82.4 / 130.6 µs | 80.0 / 127.0 µs | 0 of 60,000 | 10,326 qps, 83 µs | 12,591 qps |
| 2 | 3 readers | 3 x 20,000 | 41.5 µs | 76.0 µs | 83.9 µs | 112.3 µs | 126.7 µs | 41.3 ms | 83.5 / 126.3 µs | 81.5 / 126.1 µs | 0 of 60,000 | 11,075 qps, 83 µs | - |


**Older snapshot held H ms after the DELETE**

| cores | H | keys | PURGE total (DELETE reply to PURGED), min / median / max | first attempt, median | TXN_CONFLICT refusals | total minus H, median |
|---|---|---|---|---|---|---|
| 1 | 0 ms | 5 | 0.05 / 0.07 / 0.07 ms | 0.06 ms | 0 of 5 | 0.07 ms |
| 1 | 50 ms | 5 | 50.25 / 50.28 / 50.30 ms | 50.28 ms | 0 of 5 | 0.28 ms |
| 1 | 200 ms | 5 | 200.37 / 200.41 / 200.46 ms | 200.41 ms | 0 of 5 | 0.41 ms |
| 1 | 500 ms | 5 | 500.39 / 500.42 / 500.45 ms | 500.42 ms | 0 of 5 | 0.42 ms |
| 1 | 900 ms | 5 | 900.42 / 900.45 / 900.46 ms | 900.45 ms | 0 of 5 | 0.45 ms |
| 1 | 1100 ms | 5 | 1100.36 / 1100.37 / 1100.39 ms | 1000.89 ms | 5 of 5 | 0.37 ms |
| 2 | 0 ms | 5 | 0.05 / 0.07 / 0.11 ms | 0.07 ms | 0 of 5 | 0.07 ms |
| 2 | 50 ms | 5 | 50.32 / 50.33 / 50.34 ms | 50.33 ms | 0 of 5 | 0.33 ms |
| 2 | 200 ms | 5 | 200.41 / 200.44 / 200.46 ms | 200.44 ms | 0 of 5 | 0.44 ms |
| 2 | 500 ms | 5 | 500.42 / 500.43 / 500.45 ms | 500.43 ms | 0 of 5 | 0.43 ms |
| 2 | 900 ms | 5 | 900.43 / 900.47 / 900.48 ms | 900.47 ms | 0 of 5 | 0.47 ms |
| 2 | 1100 ms | 5 | 1100.37 / 1100.40 / 1100.42 ms | 1000.92 ms | 5 of 5 | 0.40 ms |
