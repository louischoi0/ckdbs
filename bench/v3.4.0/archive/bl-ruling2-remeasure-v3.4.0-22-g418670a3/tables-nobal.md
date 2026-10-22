
#### reader schedule latency, per run
| arm | placement | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|---|
| mixed | segregated-no0 | r1 | 4006 | 125 µs | 177 µs | 185 µs | 266 µs | 925 µs | 3.8 ms | 8.8 ms |
| mixed | segregated-no0 | r2 | 4006 | 123 µs | 177 µs | 185 µs | 267 µs | 1.4 ms | 3.4 ms | 7.3 ms |
| mixed | segregated-no0 | r3 | 4006 | 130 µs | 178 µs | 186 µs | 267 µs | 48.2 ms | 226.9 ms | 246.7 ms |
| mixed | segregated | r1 | 3990 | 123 µs | 197 µs | 210 µs | 267.0 ms | 461.8 ms | 714.5 ms | 736.6 ms |
| mixed | segregated | r2 | 3963 | 155 µs | 195 µs | 207 µs | 268.2 ms | 478.0 ms | 746.0 ms | 768.4 ms |
| mixed | segregated | r3 | 3987 | 137 µs | 191 µs | 200 µs | 274.2 ms | 501.3 ms | 769.2 ms | 791.5 ms |
| mixed | batch0 | r1 | 4006 | 116 µs | 178 µs | 188 µs | 300 µs | 70.9 ms | 220.2 ms | 249.9 ms |
| mixed | batch0 | r2 | 4006 | 150 µs | 186 µs | 194 µs | 280 µs | 1.8 ms | 129.8 ms | 159.5 ms |
| mixed | batch0 | r3 | 4006 | 126 µs | 182 µs | 192 µs | 286 µs | 73.3 ms | 207.6 ms | 237.4 ms |

#### reader schedule latency, median of runs
| arm | placement | runs | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| mixed | segregated-no0 | 3 | 125 µs | 177 µs | 185 µs | 267 µs | 1.4 ms | 3.8 ms | 8.8 ms |
| mixed | segregated | 3 | 137 µs | 195 µs | 207 µs | 268.2 ms | 478.0 ms | 746.0 ms | 768.4 ms |
| mixed | batch0 | 3 | 126 µs | 182 µs | 192 µs | 286 µs | 70.9 ms | 207.6 ms | 237.4 ms |

#### reader service latency, per run
| arm | placement | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|---|
| mixed | segregated-no0 | r1 | 4006 | 93 µs | 109 µs | 115 µs | 170 µs | 593 µs | 3.0 ms | 8.7 ms |
| mixed | segregated-no0 | r2 | 4006 | 89 µs | 109 µs | 116 µs | 171 µs | 1.0 ms | 2.5 ms | 7.2 ms |
| mixed | segregated-no0 | r3 | 4006 | 59 µs | 109 µs | 116 µs | 167 µs | 533 µs | 3.1 ms | 246.6 ms |
| mixed | segregated | r1 | 3990 | 76 µs | 121 µs | 132 µs | 181 µs | 1.2 ms | 467.8 ms | 736.5 ms |
| mixed | segregated | r2 | 3963 | 73 µs | 118 µs | 130 µs | 187 µs | 1.4 ms | 423.9 ms | 768.3 ms |
| mixed | segregated | r3 | 3987 | 72 µs | 116 µs | 126 µs | 184 µs | 2.3 ms | 448.9 ms | 791.5 ms |
| mixed | batch0 | r1 | 4006 | 52 µs | 110 µs | 118 µs | 170 µs | 992 µs | 49.8 ms | 249.9 ms |
| mixed | batch0 | r2 | 4006 | 57 µs | 115 µs | 124 µs | 184 µs | 714 µs | 4.1 ms | 159.4 ms |
| mixed | batch0 | r3 | 4006 | 55 µs | 113 µs | 121 µs | 185 µs | 426 µs | 7.7 ms | 237.3 ms |

#### reader service latency, median of runs
| arm | placement | runs | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| mixed | segregated-no0 | 3 | 89 µs | 109 µs | 116 µs | 170 µs | 593 µs | 3.0 ms | 8.7 ms |
| mixed | segregated | 3 | 73 µs | 118 µs | 130 µs | 184 µs | 1.4 ms | 448.9 ms | 768.3 ms |
| mixed | batch0 | 3 | 55 µs | 113 µs | 121 µs | 184 µs | 714 µs | 7.7 ms | 237.3 ms |

#### batch sessions, moves, stream-latch wait per core (window)
| arm | placement | run | inserter rows/s | walker stmts | moves (run / window) | errors / refusals | c0 | c1 | c2 | c3 |
|---|---|---|---|---|---|---|---|---|---|---|
| mixed | segregated-no0 | r1 | 164,258 | 1,539 | 0 / 0 | 0 / 0 | 4.10 s | 0.00 s | 0.00 s | 0.00 s |
| mixed | segregated-no0 | r2 | 158,402 | 1,553 | 0 / 0 | 0 / 0 | 4.69 s | 0.00 s | 0.01 s | 0.00 s |
| mixed | segregated-no0 | r3 | 166,756 | 1,537 | 0 / 0 | 0 / 0 | 3.98 s | 0.00 s | 0.25 s | 0.00 s |
| mixed | segregated | r1 | 161,008 | 1,550 | 0 / 0 | 0 / 0 | 4.67 s | 0.00 s | 0.30 s | 0.00 s |
| mixed | segregated | r2 | 169,587 | 1,539 | 0 / 0 | 0 / 0 | 4.37 s | 0.00 s | 0.00 s | 0.00 s |
| mixed | segregated | r3 | 168,055 | 1,535 | 0 / 0 | 0 / 0 | 4.34 s | 0.00 s | 0.19 s | 0.00 s |
| mixed | batch0 | r1 | 163,617 | 1,575 | 0 / 0 | 0 / 0 | 0.00 s | 0.00 s | 0.05 s | 0.45 s |
| mixed | batch0 | r2 | 165,646 | 1,537 | 0 / 0 | 0 / 0 | 0.00 s | 0.00 s | 0.00 s | 0.17 s |
| mixed | batch0 | r3 | 166,358 | 1,569 | 0 / 0 | 0 / 0 | 0.00 s | 0.00 s | 0.24 s | 0.22 s |
