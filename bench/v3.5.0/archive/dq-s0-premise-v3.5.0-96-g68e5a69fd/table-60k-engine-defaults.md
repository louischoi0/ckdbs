| inner rows | outer keys | arm | statement | server CPU, median | min | max | misses | hits | recorded | built | refused |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 60,000 rows | unique | A | #1 | 4503.40 ms | 4503.40 ms | 4503.40 ms | - | - | - | - | 1 of 1 |
| 60,000 rows | unique | A | #2 | 4886.86 ms | 4886.86 ms | 4886.86 ms | - | - | - | - | 1 of 1 |
| 60,000 rows | unique | Agate | #1 | 4486.61 ms | 4486.61 ms | 4486.61 ms | - | - | - | - | 1 of 1 |
| 60,000 rows | unique | Agate | #2 | 4583.60 ms | 4583.60 ms | 4583.60 ms | - | - | - | - | 1 of 1 |
| 60,000 rows | unique | B | #1 | 14.58 ms | 14.58 ms | 14.58 ms | 0 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |
| 60,000 rows | unique | B | #2 | 10.71 ms | 10.71 ms | 10.71 ms | 0 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |

| inner rows | outer keys | arm | first statement at or below B's CPU | A/B CPU at statement 1 |
|---|---|---|---|---|
| 60,000 rows | unique | A | none of 2 statements | 309x |
| 60,000 rows | unique | Agate | none of 2 statements | 308x |
