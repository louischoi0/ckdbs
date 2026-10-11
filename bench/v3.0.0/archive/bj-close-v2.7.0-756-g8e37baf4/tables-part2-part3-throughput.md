POINT `WHERE id = k`: statements/s at p50 (derived), B
| SET value | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| `SET v = <lit>` | 18,762 /s | 18,797 /s | 18,536 /s | 18,416 /s | 18,332 /s | 18,215 /s |
| `SET v = v + 1` | 18,182 /s | 18,182 /s | 18,018 /s | 17,937 /s | 17,809 /s | 17,683 /s |
| `SET v = w, w = v` | 18,282 /s | 18,315 /s | 18,100 /s | 18,034 /s | 17,953 /s | 17,794 /s |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 16,393 /s | 16,420 /s | 16,207 /s | 16,260 /s | 16,064 /s | 16,026 /s |

RANGE `WHERE id >= 1`: statements/s at p50 (derived) = rows/s in brackets, B
| SET value | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| `SET v = <lit>` | 4,163.2 /s (832,639 rows/s) | 1,046.7 /s (1,046,737 rows/s) | 109.5 /s (1,095,218 rows/s) | 3,419.4 /s (683,878 rows/s) | 835.6 /s (835,596 rows/s) | 87.2 /s (872,011 rows/s) |
| `SET v = v + 1` | 3,772.2 /s (754,432 rows/s) | 938.2 /s (938,174 rows/s) | 97.2 /s (972,342 rows/s) | 3,162.6 /s (632,511 rows/s) | 766.1 /s (766,078 rows/s) | 79.2 /s (791,869 rows/s) |
| `SET v = w, w = v` | 3,905.5 /s (781,097 rows/s) | 975.6 /s (975,562 rows/s) | 101.3 /s (1,012,730 rows/s) | 3,281.9 /s (656,383 rows/s) | 790.7 /s (790,733 rows/s) | 82.2 /s (821,794 rows/s) |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 3,281.4 /s (656,276 rows/s) | 805.2 /s (805,185 rows/s) | 85.2 /s (852,217 rows/s) | 2,849.4 /s (569,882 rows/s) | 686.6 /s (686,601 rows/s) | 71.2 /s (712,159 rows/s) |
| `SET v = (SELECT COUNT(*) FROM t)` | 3,510.6 /s (702,124 rows/s) | 913.5 /s (913,534 rows/s) | 96.0 /s (959,559 rows/s) | 2,946.4 /s (589,275 rows/s) | 750.6 /s (750,582 rows/s) | 78.9 /s (788,696 rows/s) |

ROUND TRIP: operations/s at p50 (derived), B, one session
| spelling | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| one statement `UPDATE SET v = v + 1` | 18,215 /s | 18,215 /s | 18,051 /s | 17,921 /s | 17,825 /s | 17,794 /s |
| `SELECT v` then `UPDATE SET v = <v+1>` (2 round trips) | 8,065 /s | 8,016 /s | 8,042 /s | 7,905 /s | 7,837 /s | 7,908 /s |
| `BEGIN; SELECT; UPDATE; COMMIT` (4 round trips) | 5,373 /s | 5,263 /s | 5,330 /s | 5,262 /s | 5,176 /s | 5,297 /s |
