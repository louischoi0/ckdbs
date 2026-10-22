
#### reader schedule latency, per run
| arm | placement | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|---|
| isolated | segregated-no0 | r1 | 4006 | 138 µs | 174 µs | 184 µs | 213 µs | 722 µs | 2.7 ms | 3.7 ms |
| isolated | segregated-no0 | r2 | 4006 | 123 µs | 172 µs | 177 µs | 199 µs | 478 µs | 1.8 ms | 3.5 ms |
| isolated | segregated-no0 | r3 | 4006 | 124 µs | 170 µs | 177 µs | 198 µs | 558 µs | 2.3 ms | 4.6 ms |
| mixed | segregated-no0 | r1 | 4006 | 128 µs | 190 µs | 211 µs | 253.2 ms | 485.9 ms | 686.8 ms | 716.5 ms |
| mixed | segregated-no0 | r2 | 4006 | 115 µs | 189 µs | 209 µs | 281.1 ms | 581.0 ms | 758.0 ms | 780.2 ms |
| mixed | segregated-no0 | r3 | 3993 | 140 µs | 198 µs | 225 µs | 149.8 ms | 290.7 ms | 391.2 ms | 406.0 ms |
| mixed | spread | r1 | 4006 | 134 µs | 187 µs | 209 µs | 131.3 ms | 358.4 ms | 803.4 ms | 862.8 ms |
| mixed | spread | r2 | 4006 | 131 µs | 180 µs | 193 µs | 259 µs | 972 µs | 3.0 ms | 5.1 ms |
| mixed | spread | r3 | 4006 | 143 µs | 192 µs | 216 µs | 186.9 ms | 426.8 ms | 691.8 ms | 751.1 ms |
| mixed | batch0 | r1 | 4006 | 96 µs | 187 µs | 203 µs | 192.9 ms | 433.0 ms | 608.9 ms | 628.8 ms |
| mixed | batch0 | r2 | 4000 | 119 µs | 181 µs | 197 µs | 153.6 ms | 327.8 ms | 632.7 ms | 692.1 ms |
| mixed | batch0 | r3 | 3996 | 139 µs | 185 µs | 210 µs | 85.4 ms | 433.1 ms | 701.1 ms | 725.9 ms |

#### reader schedule latency, median of runs
| arm | placement | runs | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| isolated | segregated-no0 | 3 | 124 µs | 172 µs | 177 µs | 199 µs | 558 µs | 2.3 ms | 3.7 ms |
| mixed | segregated-no0 | 3 | 128 µs | 190 µs | 211 µs | 253.2 ms | 485.9 ms | 686.8 ms | 716.5 ms |
| mixed | spread | 3 | 134 µs | 187 µs | 209 µs | 131.3 ms | 358.4 ms | 691.8 ms | 751.1 ms |
| mixed | batch0 | 3 | 119 µs | 185 µs | 203 µs | 153.6 ms | 433.0 ms | 632.7 ms | 692.1 ms |

#### reader service latency, per run
| arm | placement | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|---|
| isolated | segregated-no0 | r1 | 4006 | 92 µs | 106 µs | 116 µs | 140 µs | 594 µs | 1.4 ms | 3.6 ms |
| isolated | segregated-no0 | r2 | 4006 | 90 µs | 104 µs | 109 µs | 126 µs | 273 µs | 1.3 ms | 2.2 ms |
| isolated | segregated-no0 | r3 | 4006 | 93 µs | 104 µs | 109 µs | 126 µs | 367 µs | 1.5 ms | 2.7 ms |
| mixed | segregated-no0 | r1 | 4006 | 72 µs | 116 µs | 129 µs | 251 µs | 3.4 ms | 502.2 ms | 716.5 ms |
| mixed | segregated-no0 | r2 | 4006 | 54 µs | 113 µs | 127 µs | 215 µs | 1.5 ms | 468.1 ms | 780.2 ms |
| mixed | segregated-no0 | r3 | 3993 | 73 µs | 122 µs | 142 µs | 306 µs | 1.2 ms | 368.4 ms | 405.9 ms |
| mixed | spread | r1 | 4006 | 60 µs | 117 µs | 129 µs | 180 µs | 900 µs | 325.8 ms | 862.8 ms |
| mixed | spread | r2 | 4006 | 86 µs | 112 µs | 121 µs | 166 µs | 532 µs | 2.9 ms | 5.0 ms |
| mixed | spread | r3 | 4006 | 70 µs | 116 µs | 135 µs | 193 µs | 1.8 ms | 468.8 ms | 751.0 ms |
| mixed | batch0 | r1 | 4006 | 65 µs | 116 µs | 127 µs | 183 µs | 2.6 ms | 349.8 ms | 628.7 ms |
| mixed | batch0 | r2 | 4000 | 75 µs | 114 µs | 125 µs | 203 µs | 2.3 ms | 357.4 ms | 692.0 ms |
| mixed | batch0 | r3 | 3996 | 62 µs | 114 µs | 130 µs | 189 µs | 1.3 ms | 411.5 ms | 725.9 ms |

#### reader service latency, median of runs
| arm | placement | runs | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| isolated | segregated-no0 | 3 | 92 µs | 104 µs | 109 µs | 126 µs | 367 µs | 1.4 ms | 2.7 ms |
| mixed | segregated-no0 | 3 | 72 µs | 116 µs | 129 µs | 251 µs | 1.5 ms | 468.1 ms | 716.5 ms |
| mixed | spread | 3 | 70 µs | 116 µs | 129 µs | 180 µs | 900 µs | 325.8 ms | 751.0 ms |
| mixed | batch0 | 3 | 65 µs | 114 µs | 127 µs | 189 µs | 2.3 ms | 357.4 ms | 692.0 ms |

#### batch sessions, moves, stream-latch wait per core (window)
| arm | placement | run | inserter rows/s | walker stmts | moves (run / window) | errors / refusals | c0 | c1 | c2 | c3 |
|---|---|---|---|---|---|---|---|---|---|---|
| isolated | segregated-no0 | r1 | 0 | 0 | 1 / 0 | 0 / 0 | 0.00 s | 0.00 s | 0.00 s | 0.00 s |
| isolated | segregated-no0 | r2 | 0 | 0 | 0 / 0 | 0 / 0 | 0.00 s | 0.00 s | 0.00 s | 0.00 s |
| isolated | segregated-no0 | r3 | 0 | 0 | 0 / 0 | 0 / 0 | 0.00 s | 0.00 s | 0.00 s | 0.00 s |
| mixed | segregated-no0 | r1 | 152,046 | 20,044 | 24 / 18 | 0 / 0 | 3.12 s | 0.00 s | 0.21 s | 0.05 s |
| mixed | segregated-no0 | r2 | 158,787 | 19,759 | 17 / 10 | 0 / 0 | 4.36 s | 0.00 s | 0.22 s | 0.16 s |
| mixed | segregated-no0 | r3 | 167,159 | 17,874 | 83 / 76 | 0 / 0 | 3.90 s | 0.00 s | 0.00 s | 0.06 s |
| mixed | spread | r1 | 157,018 | 20,439 | 9 / 3 | 0 / 0 | 3.94 s | 0.00 s | 0.00 s | 0.21 s |
| mixed | spread | r2 | 155,397 | 20,182 | 13 / 1 | 0 / 0 | 2.06 s | 0.00 s | 0.51 s | 0.29 s |
| mixed | spread | r3 | 155,462 | 21,453 | 28 / 11 | 0 / 0 | 2.70 s | 0.00 s | 0.01 s | 0.00 s |
| mixed | batch0 | r1 | 154,414 | 20,759 | 13 / 6 | 0 / 0 | 3.69 s | 0.00 s | 0.00 s | 0.17 s |
| mixed | batch0 | r2 | 159,468 | 20,799 | 19 / 8 | 0 / 0 | 3.73 s | 0.00 s | 0.01 s | 0.06 s |
| mixed | batch0 | r3 | 164,044 | 20,636 | 20 / 10 | 0 / 0 | 2.61 s | 0.00 s | 0.01 s | 0.15 s |
