| placement | run | rolls | roll hold | hold max | sync-gate wait | roll-blocked wait c0 / c1 / c2 / c3 | session cores at end (6 readers, walker, inserter) | log MB/s | reader schedule p99 |
|---|---|---|---|---|---|---|---|---|---|
| segregated-no0 | r1 | 10 | 4.16 s | 171 ms | 4.09 s | 4.10 s / 0.00 s / 0.00 s / 0.00 s | 1, 2, 1, 2, 1, 2, 3, 3 | 77 | 0.9 ms |
| segregated-no0 | r2 | 12 | 4.74 s | 139 ms | 4.62 s | 4.69 s / 0.00 s / 0.01 s / 0.00 s | 1, 2, 1, 2, 1, 2, 3, 3 | 71 | 1.4 ms |
| segregated-no0 | r3 | 11 | 4.01 s | 0 ms | 3.93 s | 3.97 s / 0.00 s / 0.25 s / 0.00 s | 1, 2, 1, 2, 1, 2, 3, 3 | 75 | 48.2 ms |
| segregated | r1 | 12 | 4.75 s | 352 ms | 4.67 s | 4.67 s / 0.00 s / 0.30 s / 0.00 s | 0, 1, 2, 0, 1, 2, 3, 3 | 73 | 461.8 ms |
| segregated | r2 | 11 | 4.40 s | 352 ms | 4.37 s | 4.37 s / 0.00 s / 0.00 s / 0.00 s | 0, 1, 2, 0, 1, 2, 3, 3 | 76 | 478.0 ms |
| segregated | r3 | 11 | 4.38 s | 269 ms | 4.30 s | 4.34 s / 0.00 s / 0.19 s / 0.00 s | 0, 1, 2, 0, 1, 2, 3, 3 | 76 | 501.3 ms |
| batch0 | r1 | 11 | 4.54 s | 310 ms | 4.45 s | 0.00 s / 0.00 s / 0.05 s / 0.45 s | 1, 2, 3, 1, 2, 3, 0, 0 | 74 | 70.9 ms |
| batch0 | r2 | 11 | 4.48 s | 122 ms | 4.44 s | 0.00 s / 0.00 s / 0.00 s / 0.17 s | 1, 2, 3, 1, 2, 3, 0, 0 | 74 | 1.8 ms |
| batch0 | r3 | 11 | 4.60 s | 138 ms | 4.55 s | 0.00 s / 0.00 s / 0.24 s / 0.22 s | 1, 2, 3, 1, 2, 3, 0, 0 | 75 | 73.3 ms |
