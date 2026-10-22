| placement | run | rolls | roll hold | hold max | sync-gate wait | roll-blocked wait c0 / c1 / c2 / c3 | session cores at end (6 readers, walker, inserter) | log MB/s | reader schedule p99 |
|---|---|---|---|---|---|---|---|---|---|
| segregated-no0 | r1 | 11 | 4.61 s | 370 ms | 4.56 s | 3.12 s / 0.00 s / 0.21 s / 0.05 s | 3, 3, 3, 3, 0, 3, 2, 1 | 71 | 485.9 ms |
| segregated-no0 | r2 | 11 | 4.40 s | 368 ms | 4.30 s | 4.36 s / 0.00 s / 0.21 s / 0.16 s | 3, 3, 3, 3, 0, 3, 2, 1 | 74 | 581.0 ms |
| segregated-no0 | r3 | 11 | 3.96 s | 324 ms | 3.82 s | 3.90 s / 0.00 s / 0.00 s / 0.06 s | 3, 0, 3, 3, 3, 3, 2, 1 | 75 | 290.7 ms |
| spread | r1 | 11 | 4.01 s | 466 ms | 3.88 s | 3.94 s / 0.00 s / 0.00 s / 0.21 s | 2, 0, 2, 2, 2, 2, 3, 1 | 73 | 358.4 ms |
| spread | r2 | 11 | 4.07 s | 349 ms | 3.94 s | 2.06 s / 0.00 s / 0.51 s / 0.29 s | 1, 1, 1, 1, 1, 1, 3, 2 | 73 | 1.0 ms |
| spread | r3 | 11 | 4.60 s | 395 ms | 4.56 s | 2.70 s / 0.00 s / 0.01 s / 0.00 s | 2, 2, 2, 2, 0, 2, 1, 3 | 73 | 426.8 ms |
| batch0 | r1 | 11 | 3.76 s | 363 ms | 3.71 s | 3.69 s / 0.00 s / 0.00 s / 0.17 s | 3, 3, 3, 0, 3, 3, 1, 2 | 72 | 433.0 ms |
| batch0 | r2 | 12 | 4.22 s | 397 ms | 4.17 s | 3.73 s / 0.00 s / 0.01 s / 0.06 s | 2, 2, 2, 0, 2, 2, 3, 1 | 73 | 327.8 ms |
| batch0 | r3 | 12 | 4.16 s | 356 ms | 4.06 s | 2.61 s / 0.00 s / 0.01 s / 0.15 s | 0, 2, 2, 2, 2, 2, 3, 1 | 74 | 433.1 ms |
