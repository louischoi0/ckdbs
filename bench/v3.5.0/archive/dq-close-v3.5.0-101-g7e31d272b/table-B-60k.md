| inner rows | outer keys | arm | statement | server CPU, median | min | max | misses | hits | recorded | built | refused |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 60,000 rows | unique | A | #1 | 14.19 ms | 14.19 ms | 14.19 ms | 2000 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |
| 60,000 rows | unique | A | #2 | 4871.17 ms | 4871.17 ms | 4871.17 ms | - | - | - | - | 1 of 1 |
| 60,000 rows | unique | Agate | #1 | 14.26 ms | 14.26 ms | 14.26 ms | 2000 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |
| 60,000 rows | unique | Agate | #2 | 11.19 ms | 11.19 ms | 11.19 ms | 2000 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |
| 60,000 rows | unique | B | #1 | 13.83 ms | 13.83 ms | 13.83 ms | 0 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |
| 60,000 rows | unique | B | #2 | 10.54 ms | 10.54 ms | 10.54 ms | 0 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |

| inner rows | outer keys | arm | first statement at or below B's CPU | A/B CPU at statement 1 |
|---|---|---|---|---|
| 60,000 rows | unique | A | none of 2 statements | 1x |
| 60,000 rows | unique | Agate | none of 2 statements | 1x |
