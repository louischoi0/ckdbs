### 0. The host at each cell

| part | cell | run | driver wait | load at start | load at end | competing at start | competing at end |
|---|---|---|---|---|---|---|---|
| v1 | isolated-B | r1 | 0 s | 1.28 | 0.92 | 0 | 0 |
| v1 | levels-A | r1 | 0 s | 0.92 | 1.24 | 0 | 0 |
| v1 | levels-B | r1 | 0 s | 1.14 | 1.50 | 0 | 0 |
| v1 | isolated-B | r2 | 0 s | 1.47 | 1.19 | 0 | 0 |
| v1 | levels-A | r2 | 0 s | 1.37 | 1.58 | 0 | 0 |
| v1 | levels-B | r2 | 0 s | 1.19 | 1.37 | 0 | 0 |
| v1 | isolated-B | r3 | 0 s | 1.44 | 1.09 | 0 | 0 |
| v1 | levels-A | r3 | 0 s | 1.09 | 1.57 | 0 | 0 |
| v1 | levels-B | r3 | 5 s | 1.45 | 1.75 | 0 | 0 |
| v1 | isolated-B | r4 | 15 s | 1.40 | 1.13 | 0 | 0 |
| v1 | levels-A | r4 | 0 s | 1.31 | 1.68 | 0 | 0 |
| v1 | levels-B | r4 | 0 s | 1.13 | 1.31 | 0 | 0 |
| v1 | isolated-B | r5 | 20 s | 1.45 | 1.20 | 0 | 0 |
| v1 | levels-A | r5 | 0 s | 1.20 | 1.40 | 0 | 0 |
| v1 | levels-B | r5 | 0 s | 1.40 | 1.38 | 0 | 0 |
| c3 | bulk-A | r1 | 20 s | 1.40 | 1.47 | 0 | 0 |
| c3 | bulk-B | r1 | 0 s | 1.50 | 2.03 | 0 | 0 |
| c3 | bulk-A | r2 | 5 s | 1.49 | 1.73 | 0 | 0 |
| c3 | bulk-B | r2 | 10 s | 1.46 | 1.44 | 0 | 0 |
| c3 | bulk-A | r3 | 5 s | 1.41 | 1.80 | 0 | 0 |
| c3 | bulk-B | r3 | 10 s | 1.48 | 1.53 | 0 | 0 |
| c3 | bulk-A | r4 | 10 s | 1.42 | 1.40 | 0 | 0 |
| c3 | bulk-B | r4 | 0 s | 1.40 | 2.37 | 0 | 0 |
| c3 | bulk-A | r5 | 10 s | 1.42 | 1.66 | 0 | 0 |
| c3 | bulk-B | r5 | 0 s | 1.38 | 1.68 | 0 | 0 |
| g | isolated-B | r1 | 10 s | 1.40 | 1.17 | 0 | 0 |
| g | levels-A | r1 | 0 s | 1.50 | 1.29 | 0 | 0 |
| g | levels-B | r1 | 0 s | 1.17 | 1.50 | 0 | 0 |
| g | isolated-B | r2 | 0 s | 1.29 | 0.92 | 0 | 0 |
| g | levels-A | r2 | 0 s | 0.92 | 1.35 | 0 | 0 |
| g | levels-B | r2 | 0 s | 1.35 | 1.34 | 0 | 0 |
| g | isolated-B | r3 | 0 s | 1.34 | 1.02 | 0 | 0 |
| g | levels-A | r3 | 0 s | 1.38 | 1.63 | 0 | 0 |
| g | levels-B | r3 | 0 s | 1.02 | 1.38 | 0 | 0 |

### 1. V1, relaxed: reader schedule latency

| run | isolated-B p99 | levels-A p99 | levels-B p99 | A / isolated | B / isolated | isolated p99.9 | A p99.9 | B p99.9 | stall samples A | stall samples B | errors (iso, A, B) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| r1 | 745 µs | 1.4 ms | 811 µs | 1.85x | 1.09x | 3.0 ms | 31.3 ms | 4.3 ms | 8 | 0 | 0, 0, 0 |
| r2 | 435 µs | 25.5 ms | 673 µs | 58.65x | 1.55x | 2.7 ms | 141.3 ms | 4.3 ms | 50 | 0 | 0, 0, 0 |
| r3 | 406 µs | 4.1 ms | 633 µs | 10.05x | 1.56x | 1.6 ms | 382.8 ms | 3.5 ms | 30 | 0 | 0, 0, 0 |
| r4 | 429 µs | 1.2 ms | 849 µs | 2.71x | 1.98x | 1.4 ms | 63.2 ms | 7.4 ms | 15 | 3 | 0, 0, 0 |
| r5 | 704 µs | 881 µs | 569 µs | 1.25x | 0.81x | 3.6 ms | 59.4 ms | 3.8 ms | 11 | 0 | 0, 0, 0 |

| reading | isolated-B | levels-A | levels-B |
|---|---|---|---|
| p99, median | 435 µs | 1.4 ms | 673 µs |
| against the isolated median | 1.00x | 3.17x | 1.55x |
| levels-A: pairs within 2x | | 2 of 5 | |
| levels-B: pairs within 2x | | | 5 of 5 |
| p99.9, median | 2.7 ms | 63.2 ms | 4.3 ms |
| stall samples (>= 10 ms), all runs | 0 | 114 | 3 |
| inserter rows/s, median | | 159,910 | 159,268 |

#### read cores' holds

| arm | run | read cores | system polls > 10 ms | longest system poll (since start) | in-run holds > 10 ms | stream-latch wait, read cores | roll holds in window | roll hold median |
|---|---|---|---|---|---|---|---|---|
| levels-A | r1 | 1,2,3 | 2 | 110.4 ms | 2 | 161.0 ms | 12 | 251.0 ms |
| levels-A | r2 | 1,2,3 | 5 | 163.1 ms | 5 | 154.0 ms | 11 | 296.2 ms |
| levels-A | r3 | 1,2,3 | 2 | 450.3 ms | 2 | 478.3 ms | 11 | 231.4 ms |
| levels-A | r4 | 1,2,3 | 2 | 675.3 ms | 2 | 757.0 ms | 12 | 250.9 ms |
| levels-A | r5 | 1,2,3 | 3 | 164.8 ms | 3 | 274.4 ms | 12 | 249.7 ms |
| levels-B | r1 | 1,2,3 | 0 | 3.7 ms | 0 | 0 µs | 12 | 250.4 ms |
| levels-B | r2 | 1,2,3 | 0 | 4.9 ms | 0 | 0 µs | 12 | 253.9 ms |
| levels-B | r3 | 1,2,3 | 0 | 3.6 ms | 0 | 0 µs | 12 | 253.6 ms |
| levels-B | r4 | 1,2,3 | 1 | 56.2 ms | 1 | 0 µs | 11 | 248.7 ms |
| levels-B | r5 | 1,2,3 | 0 | 3.5 ms | 0 | 0 µs | 12 | 252.9 ms |

### 2. V1: B's checkpoint runs inside the window, every core

| run | runs | wall, median | wall, max | busy begin/end/flush | roll parks begin/end/flush | bound begin/end/flush | parked begin/end/flush | blocking | assertion visit | recycle skips | recycle blocked | live segments at window end (A, B) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| r1 | 2 | 1476.0 ms | 1610.5 ms | 1/1/4 | 1/1/3 | 0/0/0 | 50.5 ms/111.9 ms/303.1 ms | 0 µs/0 µs/0 µs | 0 µs | 0 | 0 | 14, 14 |
| r2 | 2 | 1589.8 ms | 1641.4 ms | 1/2/6 | 1/2/2 | 0/0/0 | 126.4 ms/118.1 ms/231.1 ms | 0 µs/0 µs/0 µs | 0 µs | 0 | 0 | 7, 14 |
| r3 | 2 | 1577.7 ms | 1636.6 ms | 1/2/4 | 1/2/2 | 0/0/0 | 128.8 ms/212.5 ms/343.6 ms | 0 µs/0 µs/0 µs | 0 µs | 0 | 0 | 11, 14 |
| r4 | 4 | 1019.6 ms | 1279.4 ms | 0/0/7 | 0/0/3 | 0/0/0 | 0 µs/0 µs/6.4 ms | 0 µs/0 µs/0 µs | 2 µs | 1 | 0 | 14, 14 |
| r5 | 2 | 1564.7 ms | 1606.5 ms | 2/2/3 | 1/2/2 | 0/0/0 | 130.7 ms/47.3 ms/346.8 ms | 0 µs/0 µs/0 µs | 0 µs | 0 | 0 | 14, 14 |

#### A's checkpoint runs inside the window, for the wall time

| run | runs | wall, median | wall, max |
|---|---|---|---|
| r1 | 2 | 1480.9 ms | 1608.5 ms |
| r2 | 8 | 678.9 ms | 1477.3 ms |
| r3 | 6 | 859.8 ms | 1828.9 ms |
| r4 | 2 | 1468.4 ms | 1615.3 ms |
| r5 | 2 | 1501.6 ms | 1632.4 ms |

### 3. C3, the bulk writer alone (CZ-Q2 (a): B loses at most 3 %)

| run | A rows/s | B rows/s | B / A | A log MB/s | B log MB/s |
|---|---|---|---|---|---|
| r1 | 159,481 | 159,367 | 0.999 | 74.6 | 74.6 |
| r2 | 159,510 | 154,984 | 0.972 | 74.7 | 72.5 |
| r3 | 159,029 | 159,301 | 1.002 | 74.4 | 74.6 |
| r4 | 159,516 | 165,088 | 1.035 | 74.7 | 73.6 |
| r5 | 159,404 | 155,560 | 0.976 | 72.8 | 72.8 |
| median | 159,481 | 159,301 | 0.999 | | |

### 4. The `group` cell (CZ-Q5 (a), no bar): reader schedule latency

| run | isolated-B p99 | levels-A p99 | levels-B p99 | A / isolated | B / isolated | isolated p99.9 | A p99.9 | B p99.9 | stall samples A | stall samples B | errors (iso, A, B) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| r1 | 430 µs | 575 µs | 674 µs | 1.34x | 1.57x | 1.8 ms | 3.2 ms | 3.5 ms | 0 | 0 | 0, 0, 0 |
| r2 | 408 µs | 540 µs | 517 µs | 1.33x | 1.27x | 736 µs | 2.7 ms | 2.8 ms | 0 | 0 | 0, 0, 0 |
| r3 | 422 µs | 608 µs | 555 µs | 1.44x | 1.31x | 1.1 ms | 3.5 ms | 2.9 ms | 0 | 0 | 0, 0, 0 |

| reading | isolated-B | levels-A | levels-B |
|---|---|---|---|
| p99, median | 422 µs | 575 µs | 555 µs |
| against the isolated median | 1.00x | 1.36x | 1.31x |
| levels-A: pairs within 2x | | 3 of 3 | |
| levels-B: pairs within 2x | | | 3 of 3 |
| p99.9, median | 1.1 ms | 3.2 ms | 2.9 ms |
| stall samples (>= 10 ms), all runs | 0 | 0 | 0 |
| inserter rows/s, median | | 121,711 | 122,143 |

#### read cores' holds

| arm | run | read cores | system polls > 10 ms | longest system poll (since start) | in-run holds > 10 ms | stream-latch wait, read cores | roll holds in window | roll hold median |
|---|---|---|---|---|---|---|---|---|
| levels-A | r1 | 1,2,3 | 0 | 1.8 ms | 0 | 8 µs | 8 | 3.0 ms |
| levels-A | r2 | 1,2,3 | 0 | 906 µs | 0 | 0 µs | 8 | 3.0 ms |
| levels-A | r3 | 1,2,3 | 0 | 1.4 ms | 0 | 1 µs | 8 | 3.0 ms |
| levels-B | r1 | 1,2,3 | 0 | 1.2 ms | 0 | 0 µs | 8 | 3.1 ms |
| levels-B | r2 | 1,2,3 | 0 | 891 µs | 0 | 0 µs | 8 | 3.1 ms |
| levels-B | r3 | 1,2,3 | 0 | 2.0 ms | 0 | 0 µs | 8 | 2.9 ms |

### 4.1 The `group` cell: B's checkpoint runs inside the window, every core

| run | runs | wall, median | wall, max | busy begin/end/flush | roll parks begin/end/flush | bound begin/end/flush | parked begin/end/flush | blocking | assertion visit | recycle skips | recycle blocked | live segments at window end (A, B) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| r1 | 8 | 95.0 ms | 369.5 ms | 0/0/4 | 0/0/0 | 0/0/0 | 0 µs/0 µs/4.3 ms | 0 µs/0 µs/0 µs | 4 µs | 0 | 0 | 6, 6 |
| r2 | 8 | 119.9 ms | 386.3 ms | 0/0/4 | 0/0/0 | 0/0/0 | 0 µs/0 µs/4.3 ms | 0 µs/0 µs/0 µs | 2 µs | 0 | 0 | 6, 6 |
| r3 | 8 | 123.8 ms | 393.3 ms | 1/0/3 | 0/0/0 | 0/0/0 | 1.5 ms/0 µs/3.3 ms | 0 µs/0 µs/0 µs | 3 µs | 0 | 0 | 6, 6 |

#### A's checkpoint runs inside the window, for the wall time

| run | runs | wall, median | wall, max |
|---|---|---|---|
| r1 | 8 | 161.0 ms | 397.3 ms |
| r2 | 8 | 125.6 ms | 393.9 ms |
| r3 | 8 | 123.5 ms | 388.9 ms |
