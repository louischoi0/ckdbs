POINT `WHERE id = k`: statements/s at p50 (derived), B
| SET value | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 1,000 rows |
|---|---|---|---|
| `SET v = <lit>` | 19,212 /s | 18,868 /s | 18,657 /s |
| `SET v = v + 1` | 18,132 /s | 17,905 /s | 17,841 /s |
| `SET v = w, w = v` | 18,315 /s | 18,067 /s | 17,969 /s |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 16,327 /s | 16,103 /s | 16,077 /s |

RANGE `WHERE id >= 1`: statements/s at p50 (derived) = rows/s in brackets, B
| SET value | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 1,000 rows |
|---|---|---|---|
| `SET v = <lit>` | 1,037.7 /s (1,037,721 rows/s) | 106.3 /s (1,062,524 rows/s) | 849.7 /s (849,690 rows/s) |
| `SET v = v + 1` | 920.3 /s (920,302 rows/s) | 93.9 /s (939,307 rows/s) | 769.5 /s (769,527 rows/s) |
| `SET v = w, w = v` | 952.4 /s (952,426 rows/s) | 96.6 /s (966,468 rows/s) | 793.8 /s (793,808 rows/s) |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 793.6 /s (793,619 rows/s) | 81.7 /s (816,556 rows/s) | 681.4 /s (681,385 rows/s) |
| `SET v = (SELECT COUNT(*) FROM t)` | 899.5 /s (899,483 rows/s) | 93.3 /s (933,480 rows/s) | 755.0 /s (755,002 rows/s) |

ROUND TRIP: operations/s at p50 (derived), B, one session
| spelling | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 1,000 rows |
|---|---|---|---|
| one statement `UPDATE SET v = v + 1` | 18,182 /s | 17,969 /s | 17,889 /s |
| `SELECT v` then `UPDATE SET v = <v+1>` (2 round trips) | 8,087 /s | 8,110 /s | 7,943 /s |
| `BEGIN; SELECT; UPDATE; COMMIT` (4 round trips) | 5,301 /s | 5,372 /s | 5,230 /s |
