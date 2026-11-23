| inner rows | outer keys | arm | statement | server CPU, median | min | max | misses | hits | recorded | built | refused |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 60,000 rows | unique | A | #1 | 4659.50 ms | 4659.50 ms | 4659.50 ms | - | - | - | - | 1 of 1 |
| 60,000 rows | unique | A | #2 | 4869.48 ms | 4869.48 ms | 4869.48 ms | - | - | - | - | 1 of 1 |
| 60,000 rows | unique | Agate | #1 | 4509.01 ms | 4509.01 ms | 4509.01 ms | - | - | - | - | 1 of 1 |
| 60,000 rows | unique | Agate | #2 | 4492.85 ms | 4492.85 ms | 4492.85 ms | - | - | - | - | 1 of 1 |
| 60,000 rows | unique | B | #1 | 14.68 ms | 14.68 ms | 14.68 ms | 0 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |
| 60,000 rows | unique | B | #2 | 11.21 ms | 11.21 ms | 11.21 ms | 0 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |

| inner rows | outer keys | arm | first statement at or below B's CPU | A/B CPU at statement 1 |
|---|---|---|---|---|
| 60,000 rows | unique | A | none of 2 statements | 317x |
| 60,000 rows | unique | Agate | none of 2 statements | 307x |
