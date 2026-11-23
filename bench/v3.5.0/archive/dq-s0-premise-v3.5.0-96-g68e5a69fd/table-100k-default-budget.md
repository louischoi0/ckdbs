| inner rows | outer keys | arm | statement | server CPU, median | min | max | misses | hits | recorded | built | refused |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 100,000 rows | repeat | A | #1 | 946.94 ms | 946.94 ms | 946.94 ms | 200 misses | 800 hits | 100 sets | 0 maps | 0 of 1 |
| 100,000 rows | repeat | A | #2 | 0.78 ms | 0.78 ms | 0.78 ms | 0 misses | 1000 hits | 0 sets | 0 maps | 0 of 1 |
| 100,000 rows | repeat | A | #3 | 0.98 ms | 0.98 ms | 0.98 ms | 0 misses | 1000 hits | 0 sets | 0 maps | 0 of 1 |
| 100,000 rows | repeat | Agate | #1 | 4497.68 ms | 4497.68 ms | 4497.68 ms | - | - | - | - | 1 of 1 |
| 100,000 rows | repeat | Agate | #2 | 4503.17 ms | 4503.17 ms | 4503.17 ms | - | - | - | - | 1 of 1 |
| 100,000 rows | repeat | Agate | #3 | 4497.03 ms | 4497.03 ms | 4497.03 ms | - | - | - | - | 1 of 1 |
| 100,000 rows | repeat | B | #1 | 24.75 ms | 24.75 ms | 24.75 ms | 0 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |
| 100,000 rows | repeat | B | #2 | 17.34 ms | 17.34 ms | 17.34 ms | 0 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |
| 100,000 rows | repeat | B | #3 | 18.86 ms | 18.86 ms | 18.86 ms | 0 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |
| 100,000 rows | skewed | A | #1 | 2483.98 ms | 2483.98 ms | 2483.98 ms | 540 misses | 460 hits | 20 sets | 0 maps | 0 of 1 |
| 100,000 rows | skewed | A | #2 | 2520.36 ms | 2520.36 ms | 2520.36 ms | 500 misses | 500 hits | 500 sets | 0 maps | 0 of 1 |
| 100,000 rows | skewed | A | #3 | 4.79 ms | 4.79 ms | 4.79 ms | 0 misses | 1000 hits | 0 sets | 0 maps | 0 of 1 |
| 100,000 rows | skewed | Agate | #1 | 4597.87 ms | 4597.87 ms | 4597.87 ms | - | - | - | - | 1 of 1 |
| 100,000 rows | skewed | Agate | #2 | 4580.28 ms | 4580.28 ms | 4580.28 ms | - | - | - | - | 1 of 1 |
| 100,000 rows | skewed | Agate | #3 | 4675.18 ms | 4675.18 ms | 4675.18 ms | - | - | - | - | 1 of 1 |
| 100,000 rows | skewed | B | #1 | 24.14 ms | 24.14 ms | 24.14 ms | 0 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |
| 100,000 rows | skewed | B | #2 | 17.77 ms | 17.77 ms | 17.77 ms | 0 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |
| 100,000 rows | skewed | B | #3 | 20.16 ms | 20.16 ms | 20.16 ms | 0 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |
| 100,000 rows | unique | A | #1 | 4611.31 ms | 4611.31 ms | 4611.31 ms | - | - | - | - | 1 of 1 |
| 100,000 rows | unique | A | #2 | 4909.20 ms | 4909.20 ms | 4909.20 ms | - | - | - | - | 1 of 1 |
| 100,000 rows | unique | A | #3 | 3.25 ms | 3.25 ms | 3.25 ms | 1 misses | 999 hits | 1 sets | 0 maps | 0 of 1 |
| 100,000 rows | unique | Agate | #1 | 4662.46 ms | 4662.46 ms | 4662.46 ms | - | - | - | - | 1 of 1 |
| 100,000 rows | unique | Agate | #2 | 4664.90 ms | 4664.90 ms | 4664.90 ms | - | - | - | - | 1 of 1 |
| 100,000 rows | unique | Agate | #3 | 4683.77 ms | 4683.77 ms | 4683.77 ms | - | - | - | - | 1 of 1 |
| 100,000 rows | unique | B | #1 | 24.55 ms | 24.55 ms | 24.55 ms | 0 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |
| 100,000 rows | unique | B | #2 | 18.51 ms | 18.51 ms | 18.51 ms | 0 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |
| 100,000 rows | unique | B | #3 | 18.75 ms | 18.75 ms | 18.75 ms | 0 misses | 0 hits | 0 sets | 1 maps | 0 of 1 |

| inner rows | outer keys | arm | first statement at or below B's CPU | A/B CPU at statement 1 |
|---|---|---|---|---|
| 100,000 rows | repeat | A | #2 | 38x |
| 100,000 rows | repeat | Agate | none of 3 statements | 182x |
| 100,000 rows | skewed | A | #3 | 103x |
| 100,000 rows | skewed | Agate | none of 3 statements | 190x |
| 100,000 rows | unique | A | #3 | 188x |
| 100,000 rows | unique | Agate | none of 3 statements | 190x |
