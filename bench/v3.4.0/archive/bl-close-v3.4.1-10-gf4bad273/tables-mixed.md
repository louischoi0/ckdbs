
#### reader schedule latency, per run
| arm | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| isolated | r1 | 4006 | 138 µs | 178 µs | 184 µs | 205 µs | 632 µs | 2.4 ms | 4.8 ms |
| isolated | r2 | 4006 | 160 µs | 181 µs | 192 µs | 216 µs | 467 µs | 2.3 ms | 3.8 ms |
| isolated | r3 | 4006 | 132 µs | 174 µs | 180 µs | 205 µs | 323 µs | 2.4 ms | 3.8 ms |
| levels | r1 | 4006 | 134 µs | 183 µs | 194 µs | 278 µs | 6.5 ms | 239.2 ms | 269.0 ms |
| levels | r2 | 4006 | 135 µs | 187 µs | 194 µs | 268 µs | 1.9 ms | 138.4 ms | 197.7 ms |
| levels | r3 | 4006 | 114 µs | 180 µs | 193 µs | 279 µs | 9.6 ms | 143.5 ms | 158.4 ms |
| none | r1 | 4006 | 132 µs | 182 µs | 202 µs | 151.1 ms | 292.2 ms | 696.3 ms | 755.9 ms |
| none | r2 | 4005 | 139 µs | 194 µs | 216 µs | 188.2 ms | 287.2 ms | 437.3 ms | 466.8 ms |
| none | r3 | 4006 | 132 µs | 180 µs | 200 µs | 286 µs | 1.3 ms | 21.6 ms | 31.5 ms |

#### reader schedule latency, median of runs
| arm | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|
| isolated | 138 µs | 178 µs | 184 µs | 205 µs | 467 µs | 2.4 ms | 3.8 ms |
| levels | 134 µs | 183 µs | 194 µs | 278 µs | 6.5 ms | 143.5 ms | 197.7 ms |
| none | 132 µs | 182 µs | 202 µs | 151.1 ms | 287.2 ms | 437.3 ms | 466.8 ms |

#### reader service latency, per run
| arm | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| isolated | r1 | 4006 | 94 µs | 109 µs | 114 µs | 134 µs | 282 µs | 1.4 ms | 4.7 ms |
| isolated | r2 | 4006 | 94 µs | 112 µs | 119 µs | 144 µs | 187 µs | 2.1 ms | 2.7 ms |
| isolated | r3 | 4006 | 83 µs | 106 µs | 111 µs | 133 µs | 170 µs | 1.6 ms | 3.3 ms |
| levels | r1 | 4006 | 58 µs | 113 µs | 124 µs | 177 µs | 573 µs | 5.7 ms | 41.8 ms |
| levels | r2 | 4006 | 93 µs | 118 µs | 125 µs | 170 µs | 375 µs | 4.8 ms | 197.6 ms |
| levels | r3 | 4006 | 82 µs | 112 µs | 123 µs | 170 µs | 837 µs | 4.0 ms | 158.3 ms |
| none | r1 | 4006 | 63 µs | 112 µs | 124 µs | 212 µs | 2.7 ms | 304.5 ms | 755.8 ms |
| none | r2 | 4005 | 69 µs | 118 µs | 134 µs | 742 µs | 3.6 ms | 289.6 ms | 466.8 ms |
| none | r3 | 4006 | 74 µs | 112 µs | 128 µs | 183 µs | 746 µs | 21.6 ms | 31.4 ms |

#### reader service latency, median of runs
| arm | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|
| isolated | 94 µs | 109 µs | 114 µs | 134 µs | 187 µs | 1.6 ms | 3.3 ms |
| levels | 82 µs | 113 µs | 124 µs | 170 µs | 573 µs | 4.8 ms | 158.3 ms |
| none | 69 µs | 112 µs | 128 µs | 212 µs | 2.7 ms | 289.6 ms | 466.8 ms |

#### sessions' cores at the end (6 readers, walker, inserter), partition counters, batch work
| arm | run | cores at end | readers on core 0 | batch sessions on core 0 | moves (run) | partition moves | batch_sessions at end | inserter rows/s | walker stmts |
|---|---|---|---|---|---|---|---|---|---|
| isolated | r1 | 2, 1, 3, 1, 3, 0 | 1 | 0 | 0 | 0 | 0.0 | 0 | 0 |
| isolated | r2 | 0, 3, 0, 2, 1, 2 | 2 | 0 | 0 | 0 | 0.0 | 0 | 0 |
| isolated | r3 | 0, 3, 3, 0, 3, 2 | 2 | 0 | 0 | 0 | 0.0 | 0 | 0 |
| levels | r1 | 2, 1, 1, 2, 2, 1, 0, 0 | 0 | 2 | 11 | 0 | 0.0 | 176,955 | 1,002 |
| levels | r2 | 1, 2, 2, 3, 1, 1, 0, 0 | 0 | 2 | 8 | 0 | 0.0 | 175,254 | 1,018 |
| levels | r3 | 1, 1, 2, 2, 3, 3, 0, 0 | 0 | 2 | 9 | 0 | 0.0 | 174,151 | 1,025 |
| none | r1 | 2, 2, 2, 0, 2, 2, 3, 1 | 1 | 0 | 27 | 0 | 0.0 | 173,159 | 20,351 |
| none | r2 | 3, 0, 3, 3, 0, 3, 1, 2 | 2 | 0 | 39 | 0 | 0.0 | 172,552 | 21,205 |
| none | r3 | 2, 2, 1, 2, 2, 1, 3, 0 | 0 | 1 | 11 | 0 | 0.0 | 174,493 | 20,204 |
