
### s0 `four-traders`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| load-users | kds-group | 10,000 ops | 152.0 µs (5.6 %) | 123.9 µs | 137.6 µs | 140.3 µs | 184.3 µs | 339.4 µs | 6,573 ops/s | 10.4 µs | 0.00 µs | 14.3 µs | 0 errors |
| load-users | pg | 10,000 ops | 149.6 µs (0.6 %) | 124.7 µs | 136.6 µs | 139.0 µs | 174.1 µs | 327.1 µs | 6,679 ops/s | 12.2 µs | 0.00 µs | 11.7 µs | 0 errors |
| load-users | oracle | 10,000 ops | 279.3 µs (13.6 %) | 133.2 µs | 167.9 µs | 202.8 µs | 527.1 µs | 693.3 µs | 3,577 ops/s | 20.8 µs | 0.00 µs | 60.0 µs | 0 errors |
| load-assets | kds-group | 10,000 ops | 157.4 µs (393.1 %) | 124.1 µs | 137.1 µs | 139.9 µs | 183.4 µs | 365.2 µs | 6,349 ops/s | 9.9 µs | 0.00 µs | 13.9 µs | 0 errors |
| load-assets | pg | 10,000 ops | 158.9 µs (14.2 %) | 123.3 µs | 137.0 µs | 139.9 µs | 178.9 µs | 437.5 µs | 6,288 ops/s | 13.3 µs | 0.00 µs | 12.7 µs | 0 errors |
| load-assets | oracle | 10,000 ops | 326.4 µs (40.4 %) | 136.5 µs | 166.6 µs | 201.0 µs | 535.5 µs | 741.3 µs | 3,062 ops/s | 20.2 µs | 0.00 µs | 65.5 µs | 0 errors |
| load-accounts | kds-group | 20,041 ops | 153.0 µs (1.3 %) | 119.3 µs | 135.0 µs | 137.6 µs | 176.1 µs | 370.0 µs | 6,532 ops/s | 9.6 µs | 0.00 µs | 12.3 µs | 0 errors |
| load-accounts | pg | 20,041 ops | 155.5 µs (0.4 %) | 123.5 µs | 137.3 µs | 139.7 µs | 181.2 µs | 442.8 µs | 6,426 ops/s | 13.0 µs | 0.00 µs | 11.8 µs | 0 errors |
| load-accounts | oracle | 20,041 ops | 280.6 µs (7.7 %) | 132.6 µs | 166.6 µs | 194.4 µs | 517.7 µs | 714.2 µs | 3,562 ops/s | 20.4 µs | 0.00 µs | 57.4 µs | 0 errors |
| txn | kds-group | 141,883 ops | 1,690.6 µs (13.1 %) | 795.7 µs | 1,106.8 µs | 1,475.5 µs | 2,085.5 µs | 12,613.3 µs | 2,364 ops/s | 54.3 µs | 0.01 µs | 46.5 µs | profit-scan:other 8 |
| txn | pg | 205,519 ops | 1,167.0 µs (13.8 %) | 690.7 µs | 946.6 µs | 983.1 µs | 1,228.8 µs | 1,936.3 µs | 3,425 ops/s | 61.2 µs | 0.00 µs | 89.2 µs | 0 errors |
| txn | oracle | 130,533 ops | 1,837.5 µs (13.3 %) | 591.2 µs | 1,026.1 µs | 1,437.6 µs | 2,261.3 µs | 3,750.4 µs | 2,175 ops/s | 90.3 µs | 0.09 µs | 239.9 µs | 0 errors |
| trade-insert | kds-group | 283,766 ops | 420.4 µs (13.5 %) | 134.7 µs | 270.5 µs | 355.6 µs | 532.0 µs | 880.2 µs | 4,729 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| trade-insert | pg | 411,038 ops | 288.3 µs (13.2 %) | 131.6 µs | 228.0 µs | 243.0 µs | 317.1 µs | 503.0 µs | 6,850 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| trade-insert | oracle | 261,066 ops | 459.6 µs (11.0 %) | 134.0 µs | 239.6 µs | 333.0 µs | 637.9 µs | 887.8 µs | 4,351 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| account-update | kds-group | 283,766 ops | 424.4 µs (12.8 %) | 137.2 µs | 269.4 µs | 352.6 µs | 528.2 µs | 893.8 µs | 4,729 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| account-update | pg | 411,038 ops | 294.8 µs (14.4 %) | 134.1 µs | 228.1 µs | 243.1 µs | 317.0 µs | 504.3 µs | 6,850 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| account-update | oracle | 261,066 ops | 458.6 µs (15.7 %) | 135.7 µs | 239.9 µs | 331.2 µs | 636.7 µs | 882.1 µs | 4,351 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| profit-scan | kds-group | 1,000 ops | 1,326.2 µs (106.7 %) | 510.7 µs | 665.9 µs | 799.1 µs | 1,291.7 µs | 11,807.7 µs | 17 ops/s | 0.0 µs | 0.00 µs | - | other 8 |
| profit-scan | pg | 1,000 ops | 507.8 µs (12.5 %) | 382.8 µs | 409.3 µs | 427.1 µs | 803.1 µs | 1,800.5 µs | 17 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| profit-scan | oracle | 1,000 ops | 381.3 µs (27.6 %) | 130.1 µs | 193.4 µs | 241.5 µs | 911.8 µs | 1,510.2 µs | 16 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| profit-insert | kds-group | 998 ops | 545.6 µs (65.2 %) | 217.6 µs | 292.4 µs | 382.4 µs | 680.4 µs | 4,856.0 µs | 17 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| profit-insert | pg | 1,000 ops | 833.8 µs (174.1 %) | 132.0 µs | 178.7 µs | 228.5 µs | 472.4 µs | 4,239.5 µs | 17 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| profit-insert | oracle | 1,000 ops | 2,988.3 µs (176.9 %) | 145.9 µs | 238.4 µs | 380.5 µs | 889.2 µs | 4,430.5 µs | 16 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| load-users | kds-group | - | - | - | 5.76 µs | 0.00 µs | 122.06 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 122.1 µs); cpu.on_cpu (0.00, 5.7 µs); sched.run_queue (1.00, 0.1 µs) |
| load-users | pg | - | 0.00 µs | 0.00 µs | 3.00 µs | 0.00 µs | 256.76 µs | 0.00 µs | 8.76 µs | 0.00 µs | WalSync (0.24, 256.3 µs); ClientRead (0.01, 8.8 µs); CPU (0.01, 6.8 µs); WalWrite (0.00, 0.4 µs) |
| load-users | oracle | 0.02 µs | 0.05 µs | 0.07 µs | 14.43 µs | 0.00 µs | 173.55 µs | 0.00 µs | 0.77 µs | 0.00 µs | log file sync (1.00, 173.6 µs); SQL*Net message to client (1.00, 0.8 µs); log file switch completion (0.00, 0.3 µs) |
| load-assets | kds-group | - | - | - | 5.62 µs | 0.00 µs | 128.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 128.1 µs); cpu.on_cpu (0.00, 5.5 µs); sched.run_queue (1.00, 0.1 µs) |
| load-assets | pg | - | 0.00 µs | 0.00 µs | 2.10 µs | 0.00 µs | 167.66 µs | 0.00 µs | 7.29 µs | 0.00 µs | WalSync (0.16, 164.9 µs); ClientRead (0.01, 7.3 µs); CPU (0.01, 6.2 µs); WalInitSync (0.00, 2.0 µs) |
| load-assets | oracle | 0.01 µs | 0.07 µs | 0.08 µs | 26.40 µs | 0.40 µs | 198.67 µs | 0.00 µs | 0.79 µs | 0.00 µs | log file sync (1.00, 198.7 µs); log file switch completion (0.00, 10.0 µs); SQL*Net message to client (1.00, 0.8 µs); log file switch (checkpoint incomplete) (0.00, 0.5 µs) |
| load-accounts | kds-group | - | - | - | 3.52 µs | 0.00 µs | 126.64 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 126.6 µs); cpu.on_cpu (0.00, 3.4 µs); sched.run_queue (1.00, 0.1 µs) |
| load-accounts | pg | - | 0.00 µs | 0.00 µs | 1.79 µs | 0.00 µs | 123.86 µs | 0.00 µs | 7.98 µs | 0.00 µs | WalSync (0.12, 123.6 µs); ClientRead (0.01, 8.0 µs); CPU (0.00, 5.1 µs); WalWrite (0.00, 0.2 µs) |
| load-accounts | oracle | 0.01 µs | 0.04 µs | 0.04 µs | 24.01 µs | 0.00 µs | 174.06 µs | 0.00 µs | 0.73 µs | 0.00 µs | log file sync (1.00, 174.1 µs); log file switch completion (0.00, 10.0 µs); SQL*Net message to client (1.00, 0.7 µs); log file switch (checkpoint incomplete) (0.00, 0.5 µs) |
| txn | kds-group | - | - | 2.57 µs | 73.27 µs | 0.00 µs | 798.17 µs | 0.00 µs | 0.00 µs | 0.01 µs | commit.wal_flush (4.01, 798.2 µs); sched.run_queue (4.03, 53.5 µs); cpu.on_cpu (0.00, 22.4 µs) |
| txn | pg | - | 0.00 µs | 0.00 µs | 17.35 µs | 0.00 µs | 876.16 µs | 0.03 µs | 41.25 µs | 0.00 µs | WALWrite (0.59, 620.2 µs); WalSync (0.24, 252.5 µs); ClientRead (0.04, 41.2 µs); CPU (0.03, 30.0 µs) |
| txn | oracle | 0.00 µs | 0.00 µs | 0.01 µs | 192.42 µs | 0.82 µs | 1,257.16 µs | 4.63 µs | 4.15 µs | 0.09 µs | log file sync (4.01, 1,257.2 µs); log file switch (checkpoint incomplete) (0.00, 82.9 µs); log file switch completion (0.00, 41.5 µs); buffer busy waits (0.03, 4.6 µs) |
| trade-insert | kds-group | - | - | - | - | - | - | - | - | - | none |
| trade-insert | pg | - | - | - | - | - | - | - | - | - | none |
| trade-insert | oracle | - | - | - | - | - | - | - | - | - | none |
| account-update | kds-group | - | - | - | - | - | - | - | - | - | none |
| account-update | pg | - | - | - | - | - | - | - | - | - | none |
| account-update | oracle | - | - | - | - | - | - | - | - | - | none |
| profit-scan | kds-group | - | - | - | - | - | - | - | - | - | none |
| profit-scan | pg | - | - | - | - | - | - | - | - | - | none |
| profit-scan | oracle | - | - | - | - | - | - | - | - | - | none |
| profit-insert | kds-group | - | - | - | - | - | - | - | - | - | none |
| profit-insert | pg | - | - | - | - | - | - | - | - | - | none |
| profit-insert | oracle | - | - | - | - | - | - | - | - | - | none |

### s0 `one-trader`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, kds-strict 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| load-users | kds-group | 10,000 ops | 351.6 µs (57.0 %) | 126.6 µs | 153.0 µs | 165.3 µs | 336.8 µs | 10,851.0 µs | 2,843 ops/s | 15.5 µs | 0.00 µs | 27.1 µs | 0 errors |
| load-users | kds-strict | 10,000 ops | 394.8 µs (62.4 %) | 122.3 µs | 160.6 µs | 173.3 µs | 329.7 µs | 13,271.3 µs | 2,532 ops/s | 15.5 µs | 0.00 µs | 35.4 µs | 0 errors |
| load-users | pg | 10,000 ops | 361.4 µs (58.2 %) | 123.9 µs | 167.6 µs | 178.2 µs | 389.0 µs | 10,831.5 µs | 2,766 ops/s | 24.9 µs | 0.00 µs | 27.3 µs | 0 errors |
| load-users | oracle | 10,000 ops | 559.8 µs (47.1 %) | 137.6 µs | 183.5 µs | 280.5 µs | 626.7 µs | 11,047.2 µs | 1,786 ops/s | 24.3 µs | 0.00 µs | 101.4 µs | 0 errors |
| load-assets | kds-group | 10,000 ops | 270.6 µs (76.4 %) | 124.3 µs | 144.2 µs | 155.2 µs | 335.3 µs | 2,217.2 µs | 3,695 ops/s | 13.9 µs | 0.00 µs | 22.7 µs | 0 errors |
| load-assets | kds-strict | 10,000 ops | 368.3 µs (44.7 %) | 123.1 µs | 155.5 µs | 169.4 µs | 321.6 µs | 13,462.4 µs | 2,714 ops/s | 15.1 µs | 0.00 µs | 32.4 µs | 0 errors |
| load-assets | pg | 10,000 ops | 227.5 µs (103.6 %) | 123.1 µs | 145.0 µs | 153.5 µs | 400.5 µs | 2,064.4 µs | 4,394 ops/s | 17.3 µs | 0.00 µs | 17.4 µs | 0 errors |
| load-assets | oracle | 10,000 ops | 494.2 µs (44.5 %) | 135.4 µs | 183.4 µs | 270.0 µs | 623.1 µs | 11,117.7 µs | 2,023 ops/s | 24.2 µs | 0.00 µs | 82.1 µs | 0 errors |
| load-accounts | kds-group | 20,041 ops | 590.3 µs (73.7 %) | 121.6 µs | 144.2 µs | 177.4 µs | 576.6 µs | 13,650.3 µs | 1,694 ops/s | 15.4 µs | 0.00 µs | 41.6 µs | 0 errors |
| load-accounts | kds-strict | 20,041 ops | 204.9 µs (31.2 %) | 119.3 µs | 140.4 µs | 144.9 µs | 330.3 µs | 1,982.9 µs | 4,877 ops/s | 12.3 µs | 0.00 µs | 14.8 µs | 0 errors |
| load-accounts | pg | 20,041 ops | 175.6 µs (321.4 %) | 125.6 µs | 138.3 µs | 144.0 µs | 232.7 µs | 963.8 µs | 5,693 ops/s | 14.5 µs | 0.00 µs | 13.7 µs | 0 errors |
| load-accounts | oracle | 20,041 ops | 350.8 µs (13.3 %) | 136.1 µs | 173.0 µs | 217.3 µs | 676.4 µs | 2,067.8 µs | 2,850 ops/s | 23.4 µs | 0.00 µs | 68.2 µs | 0 errors |
| txn | kds-group | 70,306 ops | 852.3 µs (42.7 %) | 498.1 µs | 560.9 µs | 636.0 µs | 1,249.1 µs | 7,780.0 µs | 1,172 ops/s | 52.4 µs | 0.01 µs | 74.9 µs | profit-scan:other 1 |
| txn | kds-strict | 85,614 ops | 699.8 µs (3.8 %) | 497.1 µs | 553.4 µs | 580.1 µs | 992.0 µs | 1,435.0 µs | 1,427 ops/s | 46.6 µs | 0.01 µs | 62.5 µs | 0 errors |
| txn | pg | 93,186 ops | 643.0 µs (94.2 %) | 501.5 µs | 559.3 µs | 576.2 µs | 858.9 µs | 1,481.5 µs | 1,553 ops/s | 54.7 µs | 0.00 µs | 61.7 µs | 0 errors |
| txn | oracle | 51,506 ops | 1,163.7 µs (75.4 %) | 548.8 µs | 699.9 µs | 1,066.0 µs | 1,889.7 µs | 2,561.4 µs | 858 ops/s | 89.0 µs | 0.21 µs | 264.6 µs | 0 errors |
| trade-insert | kds-group | 140,612 ops | 218.3 µs (43.7 %) | 120.1 µs | 140.4 µs | 157.3 µs | 275.4 µs | 909.3 µs | 2,343 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| trade-insert | kds-strict | 171,228 ops | 176.0 µs (4.5 %) | 121.6 µs | 138.5 µs | 144.3 µs | 241.0 µs | 425.0 µs | 2,854 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| trade-insert | pg | 186,372 ops | 158.8 µs (98.7 %) | 120.6 µs | 137.7 µs | 140.7 µs | 184.6 µs | 450.7 µs | 3,106 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| trade-insert | oracle | 103,012 ops | 294.2 µs (79.9 %) | 131.5 µs | 168.4 µs | 199.5 µs | 531.1 µs | 753.5 µs | 1,717 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| account-update | kds-group | 140,612 ops | 207.4 µs (41.7 %) | 118.7 µs | 138.5 µs | 154.4 µs | 269.2 µs | 683.3 µs | 2,343 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| account-update | kds-strict | 171,228 ops | 173.6 µs (3.1 %) | 119.2 µs | 137.0 µs | 142.2 µs | 237.8 µs | 423.9 µs | 2,854 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| account-update | pg | 186,372 ops | 162.4 µs (89.7 %) | 120.2 µs | 140.7 µs | 143.9 µs | 189.7 µs | 467.0 µs | 3,106 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| account-update | oracle | 103,012 ops | 287.2 µs (70.8 %) | 133.8 µs | 168.5 µs | 199.9 µs | 532.4 µs | 754.1 µs | 1,717 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| profit-scan | kds-group | 1,000 ops | 670.2 µs (125.0 %) | 291.4 µs | 432.1 µs | 490.6 µs | 1,160.8 µs | 4,798.6 µs | 17 ops/s | 0.0 µs | 0.00 µs | - | other 1 |
| profit-scan | kds-strict | 1,000 ops | 580.9 µs (54.2 %) | 351.0 µs | 488.6 µs | 534.2 µs | 805.4 µs | 1,088.3 µs | 17 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| profit-scan | pg | 1,000 ops | 456.9 µs (37.5 %) | 374.6 µs | 396.8 µs | 406.5 µs | 702.9 µs | 1,333.1 µs | 17 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| profit-scan | oracle | 1,000 ops | 280.2 µs (28.9 %) | 109.5 µs | 166.0 µs | 205.1 µs | 604.4 µs | 802.2 µs | 17 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| profit-insert | kds-group | 1,000 ops | 450.5 µs (72.1 %) | 132.2 µs | 160.0 µs | 214.6 µs | 725.3 µs | 5,630.8 µs | 17 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| profit-insert | kds-strict | 1,000 ops | 336.6 µs (102.2 %) | 150.4 µs | 251.7 µs | 297.1 µs | 512.1 µs | 715.1 µs | 17 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| profit-insert | pg | 1,000 ops | 273.3 µs (466.0 %) | 131.2 µs | 155.1 µs | 222.2 µs | 522.9 µs | 1,703.6 µs | 17 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |
| profit-insert | oracle | 1,000 ops | 489.3 µs (546.7 %) | 143.3 µs | 217.1 µs | 315.2 µs | 746.4 µs | 3,563.9 µs | 17 ops/s | 0.0 µs | 0.00 µs | - | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| load-users | kds-group | - | - | - | 11.13 µs | 0.00 µs | 298.99 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 299.0 µs); cpu.on_cpu (0.00, 11.0 µs); sched.run_queue (1.00, 0.2 µs) |
| load-users | kds-strict | - | - | - | 15.40 µs | 0.00 µs | 337.90 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 337.9 µs); cpu.on_cpu (0.00, 14.9 µs); sched.run_queue (1.00, 0.4 µs) |
| load-users | pg | - | 0.00 µs | 0.00 µs | 1.65 µs | 0.00 µs | 110.78 µs | 0.00 µs | 9.63 µs | 0.00 µs | WalSync (0.10, 110.7 µs); ClientRead (0.01, 9.6 µs); CPU (0.00, 3.3 µs); WalWrite (0.00, 0.1 µs) |
| load-users | oracle | 0.14 µs | 1.10 µs | 1.23 µs | 44.03 µs | 40.57 µs | 363.70 µs | 0.00 µs | 1.06 µs | 0.00 µs | log file sync (1.00, 363.7 µs); local write wait (0.05, 40.6 µs); log file switch (checkpoint incomplete) (0.00, 11.6 µs); log file switch completion (0.00, 5.6 µs) |
| load-assets | kds-group | - | - | - | 9.20 µs | 0.00 µs | 228.23 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 228.2 µs); cpu.on_cpu (0.00, 9.1 µs); sched.run_queue (1.00, 0.1 µs) |
| load-assets | kds-strict | - | - | - | 13.78 µs | 0.00 µs | 315.67 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 315.7 µs); cpu.on_cpu (0.00, 13.6 µs); sched.run_queue (1.00, 0.2 µs) |
| load-assets | pg | - | 0.00 µs | 0.00 µs | 1.66 µs | 0.00 µs | 119.19 µs | 0.00 µs | 5.97 µs | 0.00 µs | WalSync (0.11, 118.5 µs); ClientRead (0.01, 6.0 µs); CPU (0.00, 4.9 µs); WalWrite (0.00, 0.3 µs) |
| load-assets | oracle | 0.04 µs | 0.22 µs | 0.26 µs | 25.28 µs | 5.86 µs | 354.44 µs | 0.00 µs | 1.05 µs | 0.00 µs | log file sync (1.00, 354.4 µs); log file switch completion (0.00, 10.0 µs); direct path sync (0.00, 5.7 µs); SQL*Net message to client (1.00, 1.1 µs) |
| load-accounts | kds-group | - | - | - | 13.22 µs | 0.00 µs | 529.83 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 529.8 µs); cpu.on_cpu (0.00, 12.9 µs); sched.run_queue (1.00, 0.3 µs) |
| load-accounts | kds-strict | - | - | - | 4.51 µs | 0.00 µs | 171.57 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 171.6 µs); cpu.on_cpu (0.00, 4.4 µs); sched.run_queue (1.00, 0.1 µs) |
| load-accounts | pg | - | 0.00 µs | 0.00 µs | 1.68 µs | 0.00 µs | 116.24 µs | 0.00 µs | 7.07 µs | 0.00 µs | WalSync (0.11, 116.0 µs); ClientRead (0.01, 7.1 µs); CPU (0.00, 4.1 µs); WalWrite (0.00, 0.2 µs) |
| load-accounts | oracle | 0.07 µs | 0.07 µs | 0.14 µs | 29.40 µs | 1.05 µs | 221.77 µs | 0.00 µs | 0.91 µs | 0.00 µs | log file sync (1.00, 221.8 µs); log file switch completion (0.00, 8.5 µs); events in waitclass Other (0.03, 6.2 µs); direct path sync (0.00, 1.0 µs) |
| txn | kds-group | - | - | 3.64 µs | 32.22 µs | 0.00 µs | 711.28 µs | 0.00 µs | 0.00 µs | 0.01 µs | commit.wal_flush (4.01, 711.3 µs); cpu.on_cpu (0.00, 31.2 µs); sched.run_queue (4.06, 4.6 µs) |
| txn | kds-strict | - | - | 2.98 µs | 26.68 µs | 0.00 µs | 575.06 µs | 0.00 µs | 0.00 µs | 0.01 µs | commit.wal_flush (4.01, 575.1 µs); cpu.on_cpu (0.00, 26.4 µs); sched.run_queue (4.05, 3.3 µs) |
| txn | pg | - | 0.00 µs | 0.00 µs | 17.32 µs | 0.00 µs | 583.56 µs | 0.02 µs | 38.92 µs | 0.00 µs | WalSync (0.51, 532.9 µs); WALWrite (0.05, 49.4 µs); ClientRead (0.04, 38.9 µs); CPU (0.02, 23.3 µs) |
| txn | oracle | 0.04 µs | 0.00 µs | 0.04 µs | 100.49 µs | 0.48 µs | 720.48 µs | 0.00 µs | 3.08 µs | 0.21 µs | log file sync (4.02, 720.5 µs); log file switch completion (0.00, 18.4 µs); log file switch (checkpoint incomplete) (0.00, 17.2 µs); events in waitclass Other (0.03, 4.2 µs) |
| trade-insert | kds-group | - | - | - | - | - | - | - | - | - | none |
| trade-insert | kds-strict | - | - | - | - | - | - | - | - | - | none |
| trade-insert | pg | - | - | - | - | - | - | - | - | - | none |
| trade-insert | oracle | - | - | - | - | - | - | - | - | - | none |
| account-update | kds-group | - | - | - | - | - | - | - | - | - | none |
| account-update | kds-strict | - | - | - | - | - | - | - | - | - | none |
| account-update | pg | - | - | - | - | - | - | - | - | - | none |
| account-update | oracle | - | - | - | - | - | - | - | - | - | none |
| profit-scan | kds-group | - | - | - | - | - | - | - | - | - | none |
| profit-scan | kds-strict | - | - | - | - | - | - | - | - | - | none |
| profit-scan | pg | - | - | - | - | - | - | - | - | - | none |
| profit-scan | oracle | - | - | - | - | - | - | - | - | - | none |
| profit-insert | kds-group | - | - | - | - | - | - | - | - | - | none |
| profit-insert | kds-strict | - | - | - | - | - | - | - | - | - | none |
| profit-insert | pg | - | - | - | - | - | - | - | - | - | none |
| profit-insert | oracle | - | - | - | - | - | - | - | - | - | none |

### s1 `bars1008`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| load-exchanges | kds-group | 2 ops | 255.6 µs (54.3 %) | 179.9 µs | 179.9 µs | 331.4 µs | 331.4 µs | 331.4 µs | 2,031 ops/s | 28.6 µs | 0.00 µs | 45.7 µs | 0 errors |
| load-exchanges | pg | 2 ops | 183.4 µs (42.0 %) | 135.2 µs | 135.2 µs | 232.0 µs | 232.0 µs | 232.0 µs | 4,396 ops/s | 19.3 µs | 0.00 µs | 17.0 µs | 0 errors |
| load-exchanges | oracle | 2 ops | 1,225.5 µs (24.4 %) | 673.1 µs | 673.1 µs | 1,777.9 µs | 1,777.9 µs | 1,777.9 µs | 595 ops/s | 532.3 µs | 0.00 µs | 162.0 µs | 0 errors |
| load-symbols | kds-group | 4 ops | 316.5 µs (49.7 %) | 289.3 µs | 299.9 µs | 305.2 µs | 371.6 µs | 371.6 µs | 2,698 ops/s | 34.8 µs | 0.00 µs | 27.8 µs | 0 errors |
| load-symbols | pg | 4 ops | 144.7 µs (48.0 %) | 131.3 µs | 132.3 µs | 132.6 µs | 182.5 µs | 182.5 µs | 5,978 ops/s | 16.1 µs | 0.00 µs | 11.5 µs | 0 errors |
| load-symbols | oracle | 4 ops | 548.5 µs (46.4 %) | 200.2 µs | 324.5 µs | 495.6 µs | 1,343.8 µs | 1,343.8 µs | 1,668 ops/s | 186.3 µs | 0.00 µs | 82.8 µs | 0 errors |
| load-sessions | kds-group | 252 ops | 21.4 µs (35.9 %) | 19.1 µs | 19.9 µs | 20.2 µs | 22.2 µs | 51.6 µs | 41,541 ops/s | 5.9 µs | 0.00 µs | 9.3 µs | 0 errors |
| load-sessions | pg | 252 ops | 14.6 µs (17.0 %) | 12.8 µs | 13.2 µs | 14.1 µs | 15.5 µs | 17.8 µs | 60,810 ops/s | 5.4 µs | 0.00 µs | 5.2 µs | 0 errors |
| load-sessions | oracle | 252 ops | 33.2 µs (62.5 %) | 20.6 µs | 23.4 µs | 26.3 µs | 45.8 µs | 305.9 µs | 27,343 ops/s | 16.3 µs | 0.00 µs | 22.4 µs | 0 errors |
| load-bars | kds-group | 1,008 ops | 15.9 µs (0.7 %) | 11.5 µs | 13.4 µs | 15.5 µs | 20.2 µs | 24.3 µs | 57,514 ops/s | 5.0 µs | 0.00 µs | 7.1 µs | 0 errors |
| load-bars | pg | 1,008 ops | 14.5 µs (6.5 %) | 12.4 µs | 13.6 µs | 14.1 µs | 16.3 µs | 23.1 µs | 63,953 ops/s | 4.9 µs | 0.00 µs | 5.5 µs | 0 errors |
| load-bars | oracle | 1,008 ops | 20.9 µs (14.1 %) | 14.4 µs | 16.3 µs | 18.8 µs | 25.0 µs | 44.1 µs | 43,698 ops/s | 7.2 µs | 0.00 µs | 9.6 µs | 0 errors |
| load-stats | kds-group | 1,008 ops | 16.4 µs (8.2 %) | 13.0 µs | 14.9 µs | 15.2 µs | 18.4 µs | 34.0 µs | 56,123 ops/s | 2.9 µs | 0.00 µs | 8.9 µs | 0 errors |
| load-stats | pg | 1,008 ops | 14.4 µs (2.7 %) | 12.7 µs | 13.9 µs | 14.0 µs | 15.2 µs | 23.0 µs | 64,122 ops/s | 4.6 µs | 0.00 µs | 5.9 µs | 0 errors |
| load-stats | oracle | 1,008 ops | 17.8 µs (64.9 %) | 14.5 µs | 16.1 µs | 16.6 µs | 19.0 µs | 35.6 µs | 50,393 ops/s | 5.4 µs | 0.00 µs | 21.4 µs | 0 errors |
| load-models | kds-group | 8 ops | 146.8 µs (45.1 %) | 127.5 µs | 129.9 µs | 150.2 µs | 197.3 µs | 197.3 µs | 6,363 ops/s | 5.9 µs | 0.00 µs | 10.6 µs | 0 errors |
| load-models | pg | 8 ops | 149.2 µs (21.1 %) | 130.1 µs | 131.2 µs | 133.0 µs | 246.8 µs | 246.8 µs | 6,252 ops/s | 9.9 µs | 0.00 µs | 11.6 µs | 0 errors |
| load-models | oracle | 8 ops | 339.1 µs (19.5 %) | 158.3 µs | 193.8 µs | 346.6 µs | 658.9 µs | 658.9 µs | 2,840 ops/s | 30.0 µs | 0.00 µs | 90.2 µs | 0 errors |
| backtest-read | kds-group | 12 ops | 102.8 µs (87.0 %) | 38.7 µs | 41.3 µs | 145.7 µs | 180.3 µs | 187.3 µs | 8,947 ops/s | 8.6 µs | 1.00 µs | 33.5 µs | 0 errors |
| backtest-read | pg | 12 ops | 61.3 µs (102.4 %) | 57.8 µs | 58.6 µs | 59.5 µs | 60.9 µs | 85.8 µs | 15,574 ops/s | 8.1 µs | 0.44 µs | 49.5 µs | 0 errors |
| backtest-read | oracle | 12 ops | 162.0 µs (51.8 %) | 40.2 µs | 72.6 µs | 183.1 µs | 340.9 µs | 491.1 µs | 5,361 ops/s | 47.1 µs | 10.34 µs | 61.9 µs | 0 errors |
| result-insert | kds-group | 88 ops | 142.1 µs (2.2 %) | 125.2 µs | 129.4 µs | 131.2 µs | 160.3 µs | 260.9 µs | 7,010 ops/s | 3.7 µs | 0.00 µs | 12.5 µs | 0 errors |
| result-insert | pg | 88 ops | 145.6 µs (5.5 %) | 127.9 µs | 129.8 µs | 132.0 µs | 202.3 µs | 288.4 µs | 6,851 ops/s | 5.6 µs | 0.00 µs | 14.4 µs | 0 errors |
| result-insert | oracle | 88 ops | 314.2 µs (33.7 %) | 144.5 µs | 177.8 µs | 307.7 µs | 551.5 µs | 714.9 µs | 3,139 ops/s | 15.1 µs | 0.00 µs | 69.0 µs | 0 errors |
| backtest-replay | kds-group | 12 ops | 150.2 µs (72.2 %) | 41.4 µs | 71.9 µs | 181.7 µs | 218.1 µs | 230.7 µs | 5,969 ops/s | 18.6 µs | 1.48 µs | 33.6 µs | 0 errors |
| backtest-replay | pg | 12 ops | 89.4 µs (51.3 %) | 61.1 µs | 61.4 µs | 72.6 µs | 163.9 µs | 199.7 µs | 8,832 ops/s | 11.2 µs | 0.52 µs | 49.8 µs | 0 errors |
| backtest-replay | oracle | 12 ops | 172.7 µs (46.4 %) | 82.1 µs | 83.8 µs | 86.5 µs | 123.1 µs | 1,036.8 µs | 4,723 ops/s | 113.7 µs | 22.52 µs | 39.0 µs | 0 errors |
| compare-all | kds-group | 1 ops | 204.9 µs (90.9 %) | 204.9 µs | 204.9 µs | 204.9 µs | 204.9 µs | 204.9 µs | 3,135 ops/s | 33.1 µs | 19.26 µs | 44.7 µs | 0 errors |
| compare-all | pg | 1 ops | 100.8 µs (26.6 %) | 100.8 µs | 100.8 µs | 100.8 µs | 100.8 µs | 100.8 µs | 6,347 ops/s | 76.7 µs | 16.38 µs | 30.0 µs | 0 errors |
| compare-all | oracle | 1 ops | 711.6 µs (35.8 %) | 711.6 µs | 711.6 µs | 711.6 µs | 711.6 µs | 711.6 µs | 964 ops/s | 350.3 µs | 77.87 µs | 223.0 µs | 0 errors |
| compare-one | kds-group | 32 ops | 47.4 µs (76.3 %) | 18.2 µs | 18.8 µs | 19.8 µs | 92.4 µs | 194.7 µs | 18,340 ops/s | 5.8 µs | 0.63 µs | 12.3 µs | 0 errors |
| compare-one | pg | 32 ops | 29.0 µs (24.0 %) | 23.7 µs | 24.3 µs | 24.9 µs | 44.0 µs | 57.9 µs | 32,974 ops/s | 14.7 µs | 0.84 µs | 9.6 µs | 0 errors |
| compare-one | oracle | 32 ops | 43.3 µs (104.7 %) | 22.1 µs | 22.8 µs | 23.2 µs | 35.9 µs | 374.3 µs | 20,219 ops/s | 14.9 µs | 4.27 µs | 17.5 µs | 0 errors |
| read-bar-lookup | kds-group | 200 ops | 16.0 µs (37.3 %) | 12.7 µs | 13.3 µs | 14.5 µs | 30.8 µs | 31.4 µs | 60,278 ops/s | 5.4 µs | 0.19 µs | 6.8 µs | 0 errors |
| read-bar-lookup | pg | 200 ops | 19.7 µs (47.8 %) | 16.0 µs | 16.6 µs | 17.6 µs | 26.5 µs | 42.4 µs | 47,819 ops/s | 9.0 µs | 0.14 µs | 5.7 µs | 0 errors |
| read-bar-lookup | oracle | 200 ops | 21.2 µs (26.8 %) | 15.0 µs | 15.9 µs | 16.0 µs | 17.6 µs | 51.6 µs | 44,478 ops/s | 6.8 µs | 1.32 µs | 6.4 µs | 0 errors |
| read-bar-range | kds-group | 200 ops | 75.8 µs (10.2 %) | 56.3 µs | 64.6 µs | 71.1 µs | 91.7 µs | 192.5 µs | 13,048 ops/s | 28.6 µs | 18.21 µs | 41.2 µs | 0 errors |
| read-bar-range | pg | 200 ops | 112.9 µs (9.3 %) | 93.3 µs | 95.7 µs | 96.3 µs | 263.1 µs | 308.9 µs | 8,835 ops/s | 77.2 µs | 8.23 µs | 49.3 µs | 0 errors |
| read-bar-range | oracle | 200 ops | 145.5 µs (15.3 %) | 137.8 µs | 140.3 µs | 141.5 µs | 147.1 µs | 282.6 µs | 6,818 ops/s | 117.5 µs | 91.13 µs | 32.6 µs | 0 errors |
| read-symbol-history | kds-group | 10 ops | 148.4 µs (48.2 %) | 100.3 µs | 102.7 µs | 111.5 µs | 247.9 µs | 247.9 µs | 6,551 ops/s | 35.4 µs | 22.74 µs | 68.0 µs | 0 errors |
| read-symbol-history | pg | 10 ops | 134.2 µs (57.9 %) | 122.1 µs | 126.2 µs | 131.2 µs | 167.7 µs | 167.7 µs | 7,209 ops/s | 102.3 µs | 14.74 µs | 74.4 µs | 0 errors |
| read-symbol-history | oracle | 10 ops | 426.6 µs (12.0 %) | 264.2 µs | 351.7 µs | 415.6 µs | 825.4 µs | 825.4 µs | 2,228 ops/s | 268.5 µs | 194.74 µs | 102.6 µs | 0 errors |
| read-day-slice | kds-group | 200 ops | 29.0 µs (15.3 %) | 23.0 µs | 23.9 µs | 24.4 µs | 43.5 µs | 172.7 µs | 33,646 ops/s | 4.0 µs | 0.45 µs | 17.4 µs | 0 errors |
| read-day-slice | pg | 200 ops | 37.0 µs (7.0 %) | 33.4 µs | 34.6 µs | 34.8 µs | 40.6 µs | 56.0 µs | 26,874 ops/s | 6.4 µs | 0.22 µs | 25.1 µs | 0 errors |
| read-day-slice | oracle | 200 ops | 31.4 µs (29.8 %) | 25.3 µs | 26.7 µs | 27.0 µs | 31.1 µs | 227.2 µs | 30,915 ops/s | 10.4 µs | 3.93 µs | 16.5 µs | 0 errors |
| read-join-point | kds-group | 200 ops | 15.6 µs (45.4 %) | 13.6 µs | 14.4 µs | 14.9 µs | 15.7 µs | 17.9 µs | 61,756 ops/s | 3.1 µs | 0.09 µs | 8.2 µs | 0 errors |
| read-join-point | pg | 200 ops | 21.4 µs (39.1 %) | 15.0 µs | 16.1 µs | 16.3 µs | 19.1 µs | 168.2 µs | 44,918 ops/s | 5.2 µs | 0.09 µs | 7.8 µs | 0 errors |
| read-join-point | oracle | 200 ops | 20.4 µs (47.2 %) | 15.2 µs | 16.5 µs | 16.7 µs | 18.7 µs | 148.5 µs | 46,796 ops/s | 6.5 µs | 0.97 µs | 8.9 µs | 0 errors |
| read-join-exists | kds-group | 10 ops | 101.5 µs (72.6 %) | 42.9 µs | 43.6 µs | 56.3 µs | 196.0 µs | 196.0 µs | 8,406 ops/s | 8.7 µs | 0.40 µs | 36.5 µs | 0 errors |
| read-join-exists | pg | 10 ops | 74.2 µs (51.0 %) | 44.4 µs | 45.3 µs | 45.9 µs | 199.9 µs | 199.9 µs | 12,809 ops/s | 8.7 µs | 0.16 µs | 37.1 µs | 0 errors |
| read-join-exists | oracle | 10 ops | 95.2 µs (167.8 %) | 40.5 µs | 41.4 µs | 53.4 µs | 399.9 µs | 399.9 µs | 9,634 ops/s | 22.0 µs | 1.39 µs | 45.8 µs | 0 errors |
| agg-global | kds-group | 10 ops | 47.6 µs (191.3 %) | 43.9 µs | 44.0 µs | 44.7 µs | 66.5 µs | 66.5 µs | 18,273 ops/s | 6.9 µs | 0.21 µs | 36.3 µs | 0 errors |
| agg-global | pg | 10 ops | 48.6 µs (211.4 %) | 45.8 µs | 46.4 µs | 46.4 µs | 62.0 µs | 62.0 µs | 19,198 ops/s | 6.3 µs | 0.13 µs | 38.9 µs | 0 errors |
| agg-global | oracle | 10 ops | 115.9 µs (98.6 %) | 39.5 µs | 41.2 µs | 57.1 µs | 557.4 µs | 557.4 µs | 7,585 ops/s | 23.8 µs | 1.49 µs | 63.8 µs | 0 errors |
| agg-by-symbol | kds-group | 10 ops | 78.2 µs (102.8 %) | 74.5 µs | 75.4 µs | 75.9 µs | 103.2 µs | 103.2 µs | 11,745 ops/s | 7.8 µs | 0.43 µs | 65.6 µs | 0 errors |
| agg-by-symbol | pg | 10 ops | 121.7 µs (94.2 %) | 70.7 µs | 75.7 µs | 82.7 µs | 273.9 µs | 273.9 µs | 7,478 ops/s | 8.5 µs | 0.17 µs | 70.7 µs | 0 errors |
| agg-by-symbol | oracle | 10 ops | 136.2 µs (104.5 %) | 47.5 µs | 47.9 µs | 58.1 µs | 531.5 µs | 531.5 µs | 6,964 ops/s | 27.9 µs | 2.72 µs | 75.8 µs | 0 errors |
| agg-by-session | kds-group | 10 ops | 125.6 µs (35.3 %) | 122.5 µs | 123.3 µs | 124.9 µs | 136.8 µs | 136.8 µs | 7,538 ops/s | 30.4 µs | 15.98 µs | 91.7 µs | 0 errors |
| agg-by-session | pg | 10 ops | 346.4 µs (62.9 %) | 185.9 µs | 327.0 µs | 378.1 µs | 441.5 µs | 441.5 µs | 2,836 ops/s | 93.9 µs | 8.49 µs | 140.5 µs | 0 errors |
| agg-by-session | oracle | 10 ops | 264.7 µs (20.7 %) | 160.1 µs | 163.9 µs | 245.2 µs | 439.1 µs | 439.1 µs | 3,447 ops/s | 107.5 µs | 64.96 µs | 108.8 µs | 0 errors |
| agg-day-slice | kds-group | 200 ops | 27.6 µs (10.7 %) | 25.9 µs | 26.3 µs | 27.0 µs | 32.2 µs | 41.6 µs | 35,150 ops/s | 5.5 µs | 0.23 µs | 17.6 µs | 0 errors |
| agg-day-slice | pg | 200 ops | 44.7 µs (18.5 %) | 35.3 µs | 36.4 µs | 37.0 µs | 68.7 µs | 193.1 µs | 21,977 ops/s | 6.6 µs | 0.17 µs | 28.7 µs | 0 errors |
| agg-day-slice | oracle | 200 ops | 30.1 µs (4.1 %) | 26.3 µs | 27.5 µs | 27.7 µs | 39.9 µs | 56.7 µs | 32,661 ops/s | 7.9 µs | 1.66 µs | 17.8 µs | 0 errors |
| agg-distinct | kds-group | 10 ops | 64.2 µs (98.4 %) | 59.0 µs | 60.9 µs | 61.5 µs | 81.2 µs | 81.2 µs | 14,332 ops/s | 8.4 µs | 0.24 µs | 50.8 µs | 0 errors |
| agg-distinct | pg | 10 ops | 51.4 µs (3.6 %) | 47.1 µs | 47.5 µs | 49.2 µs | 70.4 µs | 70.4 µs | 16,972 ops/s | 8.0 µs | 0.06 µs | 39.4 µs | 0 errors |
| agg-distinct | oracle | 10 ops | 92.6 µs (87.8 %) | 34.8 µs | 36.4 µs | 38.7 µs | 410.7 µs | 410.7 µs | 8,981 ops/s | 23.3 µs | 0.97 µs | 38.5 µs | 0 errors |
| qps-bar-lookup-cold | kds-group | 100 ops | 16.6 µs (16.8 %) | 14.9 µs | 15.2 µs | 15.3 µs | 23.9 µs | 36.1 µs | 53,419 ops/s | 5.4 µs | 0.21 µs | 6.4 µs | 0 errors |
| qps-bar-lookup-cold | pg | 100 ops | 18.0 µs (47.7 %) | 14.5 µs | 14.9 µs | 15.9 µs | 29.7 µs | 47.8 µs | 51,873 ops/s | 7.7 µs | 0.10 µs | 5.7 µs | 0 errors |
| qps-bar-lookup-cold | oracle | 100 ops | 20.7 µs (31.0 %) | 15.4 µs | 16.1 µs | 16.2 µs | 19.9 µs | 24.7 µs | 45,681 ops/s | 8.1 µs | 1.45 µs | 6.2 µs | 0 errors |
| qps-bar-lookup-warm | kds-group | 100 ops | 16.0 µs (8.7 %) | 14.9 µs | 15.2 µs | 15.3 µs | 17.2 µs | 27.4 µs | 57,498 ops/s | 5.4 µs | 0.21 µs | 6.3 µs | 0 errors |
| qps-bar-lookup-warm | pg | 100 ops | 20.5 µs (20.3 %) | 14.4 µs | 15.0 µs | 16.0 µs | 23.1 µs | 165.8 µs | 43,486 ops/s | 7.1 µs | 0.09 µs | 6.3 µs | 0 errors |
| qps-bar-lookup-warm | oracle | 100 ops | 28.9 µs (65.8 %) | 14.8 µs | 15.7 µs | 16.0 µs | 44.0 µs | 301.6 µs | 32,299 ops/s | 9.4 µs | 1.65 µs | 7.1 µs | 0 errors |
| qps-bar-range-cold | kds-group | 100 ops | 92.2 µs (15.4 %) | 75.2 µs | 85.2 µs | 86.6 µs | 112.6 µs | 202.4 µs | 10,739 ops/s | 42.6 µs | 27.49 µs | 42.1 µs | 0 errors |
| qps-bar-range-cold | pg | 100 ops | 126.8 µs (11.9 %) | 93.4 µs | 95.0 µs | 96.2 µs | 284.0 µs | 375.3 µs | 7,781 ops/s | 75.4 µs | 7.87 µs | 48.6 µs | 0 errors |
| qps-bar-range-cold | oracle | 100 ops | 159.2 µs (13.4 %) | 138.2 µs | 140.1 µs | 141.3 µs | 284.2 µs | 370.6 µs | 6,248 ops/s | 119.1 µs | 92.33 µs | 29.9 µs | 0 errors |
| qps-bar-range-warm | kds-group | 100 ops | 78.6 µs (3.1 %) | 62.2 µs | 67.9 µs | 70.5 µs | 122.4 µs | 217.0 µs | 12,254 ops/s | 27.5 µs | 17.95 µs | 42.1 µs | 0 errors |
| qps-bar-range-warm | pg | 100 ops | 101.3 µs (30.0 %) | 92.2 µs | 94.0 µs | 94.6 µs | 99.9 µs | 262.0 µs | 9,684 ops/s | 70.7 µs | 7.49 µs | 45.6 µs | 0 errors |
| qps-bar-range-warm | oracle | 100 ops | 159.0 µs (7.8 %) | 136.4 µs | 138.4 µs | 139.2 µs | 261.9 µs | 383.8 µs | 6,242 ops/s | 117.7 µs | 91.71 µs | 29.6 µs | 0 errors |
| qps-day-slice-cold | kds-group | 100 ops | 27.2 µs (31.1 %) | 23.7 µs | 24.4 µs | 25.1 µs | 27.5 µs | 123.4 µs | 36,092 ops/s | 3.9 µs | 0.43 µs | 17.5 µs | 0 errors |
| qps-day-slice-cold | pg | 100 ops | 38.0 µs (8.1 %) | 34.1 µs | 34.6 µs | 34.8 µs | 44.6 µs | 68.9 µs | 25,684 ops/s | 6.0 µs | 0.20 µs | 26.2 µs | 0 errors |
| qps-day-slice-cold | oracle | 100 ops | 36.8 µs (34.6 %) | 25.1 µs | 26.8 µs | 27.2 µs | 81.3 µs | 246.1 µs | 25,401 ops/s | 11.0 µs | 3.75 µs | 16.4 µs | 0 errors |
| qps-day-slice-warm | kds-group | 100 ops | 27.1 µs (30.3 %) | 23.8 µs | 24.4 µs | 24.5 µs | 27.5 µs | 141.5 µs | 34,680 ops/s | 3.7 µs | 0.36 µs | 17.4 µs | 0 errors |
| qps-day-slice-warm | pg | 100 ops | 38.6 µs (17.8 %) | 33.1 µs | 34.4 µs | 34.6 µs | 44.8 µs | 161.4 µs | 25,420 ops/s | 6.9 µs | 0.25 µs | 25.2 µs | 0 errors |
| qps-day-slice-warm | oracle | 100 ops | 31.7 µs (15.7 %) | 25.0 µs | 26.3 µs | 26.5 µs | 29.1 µs | 171.5 µs | 29,730 ops/s | 10.4 µs | 3.49 µs | 14.6 µs | 0 errors |
| qps-symbol-history-cold | kds-group | 3 ops | 144.5 µs (57.0 %) | 107.1 µs | 115.8 µs | 115.8 µs | 210.4 µs | 210.4 µs | 5,313 ops/s | 40.7 µs | 25.74 µs | 72.6 µs | 0 errors |
| qps-symbol-history-cold | pg | 3 ops | 197.9 µs (63.5 %) | 132.2 µs | 156.5 µs | 156.5 µs | 311.1 µs | 311.1 µs | 4,058 ops/s | 115.0 µs | 18.21 µs | 90.3 µs | 0 errors |
| qps-symbol-history-cold | oracle | 3 ops | 509.5 µs (29.5 %) | 423.1 µs | 508.4 µs | 508.4 µs | 527.1 µs | 527.1 µs | 1,674 ops/s | 297.8 µs | 199.51 µs | 107.0 µs | 0 errors |
| qps-symbol-history-warm | kds-group | 100 ops | 117.4 µs (11.1 %) | 94.1 µs | 105.6 µs | 118.1 µs | 131.7 µs | 231.6 µs | 8,468 ops/s | 44.2 µs | 31.59 µs | 67.4 µs | 0 errors |
| qps-symbol-history-warm | pg | 100 ops | 133.0 µs (4.8 %) | 112.2 µs | 117.6 µs | 122.1 µs | 242.8 µs | 317.4 µs | 7,485 ops/s | 95.4 µs | 12.12 µs | 76.1 µs | 0 errors |
| qps-symbol-history-warm | oracle | 100 ops | 358.4 µs (6.5 %) | 250.1 µs | 262.3 µs | 378.1 µs | 476.5 µs | 548.8 µs | 2,769 ops/s | 225.0 µs | 182.14 µs | 77.8 µs | 0 errors |
| qps-cross-join-cold | kds-group | 100 ops | 42.0 µs (22.7 %) | 37.8 µs | 38.9 µs | 39.4 µs | 58.4 µs | 165.5 µs | 23,042 ops/s | 4.4 µs | 0.43 µs | 32.4 µs | 0 errors |
| qps-cross-join-cold | pg | 100 ops | 63.5 µs (23.6 %) | 57.7 µs | 58.7 µs | 59.3 µs | 74.1 µs | 208.9 µs | 15,302 ops/s | 7.3 µs | 0.34 µs | 49.2 µs | 0 errors |
| qps-cross-join-cold | oracle | 100 ops | 42.9 µs (25.8 %) | 34.0 µs | 35.6 µs | 36.0 µs | 53.7 µs | 270.1 µs | 22,412 ops/s | 12.1 µs | 4.16 µs | 27.6 µs | 0 errors |
| qps-cross-join-warm | kds-group | 100 ops | 40.2 µs (4.2 %) | 37.5 µs | 38.9 µs | 39.1 µs | 43.1 µs | 60.1 µs | 24,549 ops/s | 3.5 µs | 0.36 µs | 32.0 µs | 0 errors |
| qps-cross-join-warm | pg | 100 ops | 67.5 µs (12.1 %) | 57.0 µs | 58.8 µs | 59.3 µs | 144.8 µs | 197.5 µs | 14,407 ops/s | 7.9 µs | 0.38 µs | 49.1 µs | 0 errors |
| qps-cross-join-warm | oracle | 100 ops | 46.0 µs (54.9 %) | 33.5 µs | 34.9 µs | 35.3 µs | 142.3 µs | 215.8 µs | 21,045 ops/s | 11.4 µs | 3.72 µs | 23.3 µs | 0 errors |
| qps-point-join-cold | kds-group | 100 ops | 18.8 µs (51.0 %) | 13.6 µs | 15.0 µs | 15.3 µs | 19.6 µs | 42.6 µs | 49,524 ops/s | 3.4 µs | 0.11 µs | 8.2 µs | 0 errors |
| qps-point-join-cold | pg | 100 ops | 33.8 µs (57.6 %) | 15.2 µs | 16.2 µs | 16.3 µs | 178.6 µs | 269.0 µs | 28,635 ops/s | 5.9 µs | 0.09 µs | 9.6 µs | 0 errors |
| qps-point-join-cold | oracle | 100 ops | 17.9 µs (7.5 %) | 15.2 µs | 15.6 µs | 16.4 µs | 18.2 µs | 29.7 µs | 53,996 ops/s | 7.4 µs | 0.98 µs | 7.2 µs | 0 errors |
| qps-point-join-warm | kds-group | 100 ops | 14.9 µs (8.3 %) | 13.6 µs | 14.4 µs | 15.0 µs | 15.7 µs | 16.6 µs | 65,378 ops/s | 3.0 µs | 0.09 µs | 8.2 µs | 0 errors |
| qps-point-join-warm | pg | 100 ops | 17.6 µs (33.4 %) | 14.9 µs | 16.1 µs | 16.2 µs | 17.1 µs | 23.2 µs | 52,811 ops/s | 4.9 µs | 0.09 µs | 7.4 µs | 0 errors |
| qps-point-join-warm | oracle | 100 ops | 18.4 µs (5.2 %) | 15.4 µs | 16.3 µs | 16.5 µs | 17.7 µs | 21.2 µs | 49,971 ops/s | 7.3 µs | 0.99 µs | 7.0 µs | 0 errors |
| qps-model-join-cold | kds-group | 7 ops | 26.4 µs (592.8 %) | 20.0 µs | 21.2 µs | 21.3 µs | 51.3 µs | 51.3 µs | 23,813 ops/s | 8.8 µs | 1.43 µs | 12.9 µs | 0 errors |
| qps-model-join-cold | pg | 7 ops | 23.8 µs (16.6 %) | 20.7 µs | 21.0 µs | 21.1 µs | 43.7 µs | 43.7 µs | 24,751 ops/s | 10.7 µs | 0.62 µs | 9.6 µs | 0 errors |
| qps-model-join-cold | oracle | 7 ops | 198.0 µs (85.3 %) | 129.7 µs | 171.6 µs | 199.8 µs | 467.8 µs | 467.8 µs | 4,262 ops/s | 48.6 µs | 7.93 µs | 40.1 µs | 0 errors |
| qps-model-join-warm | kds-group | 100 ops | 21.9 µs (9.6 %) | 19.4 µs | 19.8 µs | 20.4 µs | 22.9 µs | 29.8 µs | 44,359 ops/s | 5.6 µs | 0.84 µs | 11.9 µs | 0 errors |
| qps-model-join-warm | pg | 100 ops | 21.7 µs (48.6 %) | 19.4 µs | 20.5 µs | 20.7 µs | 22.3 µs | 47.7 µs | 43,804 ops/s | 8.6 µs | 0.44 µs | 9.3 µs | 0 errors |
| qps-model-join-warm | oracle | 100 ops | 47.5 µs (31.2 %) | 25.5 µs | 27.5 µs | 28.2 µs | 205.0 µs | 281.6 µs | 20,541 ops/s | 19.0 µs | 7.27 µs | 13.4 µs | 0 errors |
| write-batch-1 | kds-group | 2,000 ops | 152.0 µs (7.5 %) | 125.5 µs | 137.7 µs | 141.3 µs | 177.4 µs | 358.5 µs | 6,568 ops/s | 10.8 µs | 0.00 µs | 14.2 µs | 0 errors |
| write-batch-1 | pg | 2,000 ops | 149.2 µs (1.6 %) | 125.4 µs | 137.1 µs | 138.6 µs | 169.4 µs | 324.4 µs | 6,696 ops/s | 12.9 µs | 0.00 µs | 11.2 µs | 0 errors |
| write-batch-1 | oracle | 2,000 ops | 284.4 µs (24.8 %) | 137.8 µs | 165.5 µs | 191.6 µs | 515.2 µs | 688.5 µs | 3,511 ops/s | 20.3 µs | 0.00 µs | 62.4 µs | 0 errors |
| write-batch-10 | kds-group | 2,000 ops | 30.7 µs (9.3 %) | 12.6 µs | 14.1 µs | 15.1 µs | 141.4 µs | 169.9 µs | 32,424 ops/s | 4.6 µs | 0.00 µs | 9.0 µs | 0 errors |
| write-batch-10 | pg | 2,000 ops | 28.9 µs (3.6 %) | 11.9 µs | 13.2 µs | 13.3 µs | 139.5 µs | 165.2 µs | 34,436 ops/s | 6.4 µs | 0.00 µs | 6.1 µs | 0 errors |
| write-batch-10 | oracle | 2,000 ops | 34.1 µs (30.4 %) | 13.3 µs | 14.5 µs | 15.0 µs | 171.1 µs | 201.9 µs | 29,209 ops/s | 6.7 µs | 0.00 µs | 10.3 µs | 0 errors |
| write-batch-100 | kds-group | 2,000 ops | 16.5 µs (1.5 %) | 12.6 µs | 14.0 µs | 14.2 µs | 15.9 µs | 162.3 µs | 60,430 ops/s | 2.7 µs | 0.00 µs | 8.1 µs | 0 errors |
| write-batch-100 | pg | 2,000 ops | 14.8 µs (6.6 %) | 11.9 µs | 13.0 µs | 13.1 µs | 15.2 µs | 143.2 µs | 66,824 ops/s | 4.5 µs | 0.00 µs | 5.0 µs | 0 errors |
| write-batch-100 | oracle | 2,000 ops | 19.0 µs (6.9 %) | 13.0 µs | 14.4 µs | 15.1 µs | 20.3 µs | 222.8 µs | 52,085 ops/s | 4.5 µs | 0.00 µs | 8.4 µs | 0 errors |
| write-batch-1000 | kds-group | 2,000 ops | 15.5 µs (5.6 %) | 12.8 µs | 14.2 µs | 14.5 µs | 15.4 µs | 27.6 µs | 64,029 ops/s | 2.7 µs | 0.00 µs | 8.3 µs | 0 errors |
| write-batch-1000 | pg | 2,000 ops | 13.8 µs (2.5 %) | 11.8 µs | 12.8 µs | 13.1 µs | 14.0 µs | 24.4 µs | 71,842 ops/s | 4.5 µs | 0.00 µs | 5.1 µs | 0 errors |
| write-batch-1000 | oracle | 2,000 ops | 16.6 µs (6.6 %) | 13.6 µs | 15.1 µs | 15.2 µs | 17.6 µs | 36.4 µs | 59,508 ops/s | 4.4 µs | 0.00 µs | 7.6 µs | 0 errors |
| join-conn-1 | kds-group | 200 ops | 45.8 µs (9.8 %) | 37.7 µs | 38.4 µs | 39.0 µs | 55.5 µs | 180.2 µs | 21,466 ops/s | 4.0 µs | 0.38 µs | 32.2 µs | 0 errors |
| join-conn-1 | pg | 200 ops | 71.4 µs (16.7 %) | 56.7 µs | 58.1 µs | 58.9 µs | 79.8 µs | 330.2 µs | 13,894 ops/s | 7.6 µs | 0.35 µs | 50.8 µs | 0 errors |
| join-conn-1 | oracle | 200 ops | 42.2 µs (5.4 %) | 32.7 µs | 34.4 µs | 35.0 µs | 49.0 µs | 204.5 µs | 23,011 ops/s | 11.2 µs | 3.84 µs | 25.1 µs | 0 errors |
| join-conn-2 | kds-group | 200 ops | 65.7 µs (9.0 %) | 38.9 µs | 62.8 µs | 63.6 µs | 89.8 µs | 193.2 µs | 27,676 ops/s | 5.1 µs | 0.48 µs | 30.7 µs | 0 errors |
| join-conn-2 | pg | 200 ops | 80.9 µs (25.1 %) | 60.2 µs | 61.6 µs | 62.6 µs | 237.9 µs | 347.7 µs | 22,622 ops/s | 10.9 µs | 0.46 µs | 52.6 µs | 0 errors |
| join-conn-2 | oracle | 200 ops | 67.5 µs (81.2 %) | 36.7 µs | 38.9 µs | 39.9 µs | 261.3 µs | 436.6 µs | 24,552 ops/s | 18.4 µs | 5.42 µs | 30.1 µs | 0 errors |
| join-conn-4 | kds-group | 200 ops | 111.5 µs (4.9 %) | 38.6 µs | 65.2 µs | 95.3 µs | 200.0 µs | 251.8 µs | 29,872 ops/s | 5.9 µs | 0.54 µs | 30.7 µs | 0 errors |
| join-conn-4 | pg | 200 ops | 151.6 µs (54.0 %) | 75.0 µs | 80.6 µs | 83.4 µs | 423.4 µs | 475.1 µs | 21,550 ops/s | 33.8 µs | 1.36 µs | 63.5 µs | 0 errors |
| join-conn-4 | oracle | 200 ops | 72.3 µs (21.0 %) | 33.8 µs | 36.5 µs | 37.9 µs | 268.8 µs | 671.2 µs | 46,388 ops/s | 18.2 µs | 3.91 µs | 34.1 µs | 0 errors |
| join-conn-8 | kds-group | 200 ops | 218.4 µs (10.7 %) | 43.3 µs | 159.7 µs | 193.9 µs | 445.2 µs | 537.7 µs | 29,479 ops/s | 11.9 µs | 0.99 µs | 30.9 µs | 0 errors |
| join-conn-8 | pg | 200 ops | 131.9 µs (90.0 %) | 59.8 µs | 62.8 µs | 64.9 µs | 425.0 µs | 585.8 µs | 33,424 ops/s | 12.5 µs | 0.47 µs | 65.0 µs | 0 errors |
| join-conn-8 | oracle | 200 ops | 139.4 µs (15.3 %) | 34.6 µs | 38.1 µs | 42.5 µs | 471.4 µs | 1,295.1 µs | 32,142 ops/s | 25.1 µs | 4.85 µs | 44.3 µs | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| load-exchanges | kds-group | - | - | - | 13.00 µs | 0.00 µs | 149.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 149.0 µs); cpu.on_cpu (0.00, 13.0 µs); sched.run_queue (1.00, 0.0 µs) |
| load-exchanges | pg | - | 0.00 µs | 0.00 µs | 2.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| load-exchanges | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 30.00 µs | 0.00 µs | 367.00 µs | 0.00 µs | 1.50 µs | 0.00 µs | log file sync (1.00, 367.0 µs); SQL*Net message to client (1.00, 1.5 µs) |
| load-symbols | kds-group | - | - | - | 8.00 µs | 0.00 µs | 134.25 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 134.2 µs); cpu.on_cpu (0.00, 8.0 µs); sched.run_queue (1.00, 0.0 µs) |
| load-symbols | pg | - | 0.00 µs | 0.00 µs | 1.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| load-symbols | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 27.50 µs | 0.00 µs | 227.50 µs | 0.00 µs | 0.75 µs | 0.00 µs | log file sync (1.00, 227.5 µs); SQL*Net message to client (1.00, 0.8 µs) |
| load-sessions | kds-group | - | - | - | 4.39 µs | 0.00 µs | 1.23 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.3 µs); commit.wal_flush (0.01, 1.2 µs); sched.run_queue (1.02, 0.1 µs) |
| load-sessions | pg | - | 0.00 µs | 0.00 µs | 1.19 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.73 µs | 0.00 µs | ClientRead (0.00, 3.7 µs) |
| load-sessions | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.39 µs | 0.00 µs | 1.67 µs | 0.00 µs | 0.25 µs | 0.00 µs | log file sync (0.01, 1.7 µs); SQL*Net message to client (1.01, 0.3 µs); log file switch completion (0.00, 0.0 µs) |
| load-bars | kds-group | - | - | - | 2.64 µs | 0.00 µs | 0.94 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 2.6 µs); commit.wal_flush (0.01, 0.9 µs); sched.run_queue (1.01, 0.1 µs) |
| load-bars | pg | - | 0.00 µs | 0.00 µs | 1.23 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.21 µs | 0.00 µs | ClientRead (0.00, 2.2 µs) |
| load-bars | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.28 µs | 0.00 µs | 1.87 µs | 0.00 µs | 0.21 µs | 0.00 µs | log file sync (0.01, 1.9 µs); SQL*Net message to client (1.01, 0.2 µs) |
| load-stats | kds-group | - | - | - | 4.40 µs | 0.00 µs | 0.97 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.3 µs); commit.wal_flush (0.01, 1.0 µs); sched.run_queue (1.01, 0.1 µs) |
| load-stats | pg | - | 0.00 µs | 0.00 µs | 1.29 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.96 µs | 0.00 µs | ClientRead (0.00, 1.0 µs) |
| load-stats | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.62 µs | 0.00 µs | 1.63 µs | 0.00 µs | 0.21 µs | 0.00 µs | log file sync (0.01, 1.6 µs); SQL*Net message to client (1.01, 0.2 µs); Data file init write (0.00, 0.0 µs); Disk file operations I/O (0.00, 0.0 µs) |
| load-models | kds-group | - | - | - | 3.25 µs | 0.00 µs | 120.12 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 120.1 µs); cpu.on_cpu (0.00, 3.1 µs); sched.run_queue (1.00, 0.1 µs) |
| load-models | pg | - | 0.00 µs | 0.00 µs | 1.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| load-models | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 24.38 µs | 0.00 µs | 226.25 µs | 0.00 µs | 1.12 µs | 0.00 µs | log file sync (1.00, 226.2 µs); SQL*Net message to client (1.00, 1.1 µs) |
| backtest-read | kds-group | - | - | 12.83 µs | 16.17 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.00 µs | cpu.on_cpu (0.00, 29.0 µs); sched.run_queue (1.00, 0.1 µs) |
| backtest-read | pg | - | 0.00 µs | 0.00 µs | 42.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.48 µs | none |
| backtest-read | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 42.42 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.00 µs | 10.34 µs | SQL*Net message to client (1.00, 1.0 µs) |
| result-insert | kds-group | - | - | 0.00 µs | 4.50 µs | 0.00 µs | 123.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 123.8 µs); cpu.on_cpu (0.00, 4.4 µs); sched.run_queue (1.00, 0.1 µs) |
| result-insert | pg | - | 0.00 µs | 0.00 µs | 1.39 µs | 0.00 µs | 75.19 µs | 0.00 µs | 0.00 µs | 0.00 µs | WalSync (0.08, 75.2 µs) |
| result-insert | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 15.86 µs | 0.00 µs | 187.17 µs | 0.00 µs | 0.94 µs | 0.00 µs | log file sync (1.00, 187.2 µs); SQL*Net message to client (1.00, 0.9 µs) |
| backtest-replay | kds-group | - | - | 12.83 µs | 16.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.48 µs | cpu.on_cpu (0.00, 28.8 µs); sched.run_queue (1.00, 0.1 µs) |
| backtest-replay | pg | - | 0.00 µs | 0.00 µs | 42.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.04 µs | none |
| backtest-replay | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 26.58 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.67 µs | 22.52 µs | SQL*Net message to client (1.00, 0.7 µs) |
| compare-all | kds-group | - | - | 5.00 µs | 34.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 19.26 µs | cpu.on_cpu (0.00, 39.0 µs); sched.run_queue (1.00, 0.0 µs) |
| compare-all | pg | - | 0.00 µs | 0.00 µs | 23.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 8.98 µs | none |
| compare-all | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 79.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.00 µs | 77.87 µs | SQL*Net message to client (1.00, 4.0 µs) |
| compare-one | kds-group | - | - | 3.44 µs | 4.66 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.63 µs | cpu.on_cpu (0.00, 8.0 µs); sched.run_queue (1.00, 0.1 µs) |
| compare-one | pg | - | 0.00 µs | 0.00 µs | 4.09 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.48 µs | none |
| compare-one | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 10.31 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.41 µs | 4.27 µs | SQL*Net message to client (1.00, 0.4 µs) |
| read-bar-lookup | kds-group | - | - | 0.92 µs | 1.67 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.19 µs | cpu.on_cpu (0.00, 2.5 µs); sched.run_queue (1.00, 0.1 µs) |
| read-bar-lookup | pg | - | 0.00 µs | 0.00 µs | 1.23 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.07 µs | CPU (0.01, 1.2 µs) |
| read-bar-lookup | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.35 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.20 µs | 1.32 µs | SQL*Net message to client (1.00, 0.2 µs) |
| read-bar-range | kds-group | - | - | 1.01 µs | 34.41 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 18.21 µs | cpu.on_cpu (0.00, 35.4 µs); sched.run_queue (1.00, 0.1 µs) |
| read-bar-range | pg | - | 0.00 µs | 0.00 µs | 44.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.10 µs | 7.52 µs | CPU (0.03, 25.7 µs); ClientRead (0.01, 10.1 µs) |
| read-bar-range | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 23.41 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.38 µs | 91.13 µs | SQL*Net message to client (1.00, 3.4 µs) |
| read-symbol-history | kds-group | - | - | 1.20 µs | 59.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 22.74 µs | cpu.on_cpu (0.00, 61.0 µs); sched.run_queue (1.00, 0.1 µs) |
| read-symbol-history | pg | - | 0.00 µs | 0.00 µs | 97.30 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 16.78 µs | none |
| read-symbol-history | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 77.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 11.10 µs | 194.74 µs | SQL*Net message to client (1.00, 11.1 µs) |
| read-day-slice | kds-group | - | - | 1.03 µs | 12.54 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.45 µs | cpu.on_cpu (0.00, 13.5 µs); sched.run_queue (1.00, 0.1 µs) |
| read-day-slice | pg | - | 0.00 µs | 0.00 µs | 20.79 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.74 µs | 0.21 µs | CPU (0.01, 6.8 µs); ClientRead (0.01, 4.7 µs) |
| read-day-slice | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 10.65 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.26 µs | 3.93 µs | SQL*Net message to client (1.00, 0.3 µs) |
| read-join-point | kds-group | - | - | 1.95 µs | 2.57 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.09 µs | cpu.on_cpu (0.00, 4.5 µs); sched.run_queue (1.00, 0.0 µs) |
| read-join-point | pg | - | 0.00 µs | 0.00 µs | 1.74 µs | 0.00 µs | 0.00 µs | 0.00 µs | 7.33 µs | 0.10 µs | ClientRead (0.01, 7.3 µs) |
| read-join-point | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.69 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | 0.97 µs | SQL*Net message to client (1.00, 0.2 µs) |
| read-join-exists | kds-group | - | - | 1.60 µs | 31.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.40 µs | cpu.on_cpu (0.00, 32.9 µs); sched.run_queue (1.00, 0.0 µs) |
| read-join-exists | pg | - | 0.00 µs | 0.00 µs | 30.60 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.18 µs | none |
| read-join-exists | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 37.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.40 µs | 1.39 µs | SQL*Net message to client (1.00, 0.4 µs) |
| agg-global | kds-group | - | - | 1.20 µs | 31.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | cpu.on_cpu (0.00, 32.4 µs); sched.run_queue (1.00, 0.1 µs) |
| agg-global | pg | - | 0.00 µs | 0.00 µs | 32.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.13 µs | none |
| agg-global | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 38.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.70 µs | 1.49 µs | SQL*Net message to client (1.00, 0.7 µs) |
| agg-by-symbol | kds-group | - | - | 1.20 µs | 60.70 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.43 µs | cpu.on_cpu (0.00, 61.9 µs); sched.run_queue (1.00, 0.1 µs) |
| agg-by-symbol | pg | - | 0.00 µs | 0.00 µs | 55.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.27 µs | none |
| agg-by-symbol | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 50.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.80 µs | 2.72 µs | SQL*Net message to client (1.00, 0.8 µs) |
| agg-by-session | kds-group | - | - | 1.20 µs | 85.70 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 15.98 µs | cpu.on_cpu (0.00, 86.9 µs); sched.run_queue (1.00, 0.1 µs) |
| agg-by-session | pg | - | 0.00 µs | 0.00 µs | 97.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 12.92 µs | none |
| agg-by-session | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 95.30 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.30 µs | 64.96 µs | events in waitclass Other (6.00, 8.7 µs); SQL*Net message to client (1.00, 0.3 µs) |
| agg-day-slice | kds-group | - | - | 1.23 µs | 12.42 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | cpu.on_cpu (0.00, 13.6 µs); sched.run_queue (1.00, 0.1 µs) |
| agg-day-slice | pg | - | 0.00 µs | 0.00 µs | 21.39 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.14 µs | CPU (0.03, 22.9 µs) |
| agg-day-slice | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 13.76 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.20 µs | 1.66 µs | SQL*Net message to client (1.00, 0.2 µs) |
| agg-distinct | kds-group | - | - | 0.90 µs | 46.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | cpu.on_cpu (0.00, 47.0 µs); sched.run_queue (1.00, 0.0 µs) |
| agg-distinct | pg | - | 0.00 µs | 0.00 µs | 33.70 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.07 µs | none |
| agg-distinct | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 29.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.60 µs | 0.97 µs | SQL*Net message to client (1.00, 0.6 µs) |
| qps-bar-lookup-cold | kds-group | - | - | 0.94 µs | 1.67 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | cpu.on_cpu (0.00, 2.6 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-lookup-cold | pg | - | 0.00 µs | 0.00 µs | 1.13 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.09 µs | none |
| qps-bar-lookup-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.39 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | 1.45 µs | SQL*Net message to client (1.00, 0.2 µs) |
| qps-bar-lookup-warm | kds-group | - | - | 0.92 µs | 1.59 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | cpu.on_cpu (0.00, 2.5 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-lookup-warm | pg | - | 0.00 µs | 0.00 µs | 1.13 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.07 µs | none |
| qps-bar-lookup-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.68 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.28 µs | 1.65 µs | SQL*Net message to client (1.00, 0.3 µs) |
| qps-bar-range-cold | kds-group | - | - | 1.02 µs | 35.23 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 27.49 µs | cpu.on_cpu (0.00, 36.2 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-range-cold | pg | - | 0.00 µs | 0.00 µs | 42.69 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 8.32 µs | CPU (0.05, 53.4 µs) |
| qps-bar-range-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 25.91 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.33 µs | 92.33 µs | SQL*Net message to client (1.00, 4.3 µs) |
| qps-bar-range-warm | kds-group | - | - | 1.03 µs | 35.46 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 17.95 µs | cpu.on_cpu (0.00, 36.4 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-range-warm | pg | - | 0.00 µs | 0.00 µs | 42.25 µs | 0.00 µs | 0.00 µs | 0.00 µs | 8.74 µs | 7.55 µs | CPU (0.04, 34.0 µs); ClientRead (0.01, 8.7 µs) |
| qps-bar-range-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 25.57 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.24 µs | 91.71 µs | SQL*Net message to client (1.00, 4.2 µs) |
| qps-day-slice-cold | kds-group | - | - | 1.04 µs | 12.58 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.43 µs | cpu.on_cpu (0.00, 13.6 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-day-slice-cold | pg | - | 0.00 µs | 0.00 µs | 20.69 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | CPU (0.01, 9.2 µs) |
| qps-day-slice-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 11.36 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.30 µs | 3.75 µs | SQL*Net message to client (1.00, 0.3 µs) |
| qps-day-slice-warm | kds-group | - | - | 1.05 µs | 12.56 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.36 µs | cpu.on_cpu (0.00, 13.6 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-day-slice-warm | pg | - | 0.00 µs | 0.00 µs | 21.29 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.26 µs | CPU (0.03, 31.6 µs) |
| qps-day-slice-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 10.62 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 3.49 µs | SQL*Net message to client (1.00, 0.2 µs) |
| qps-symbol-history-cold | kds-group | - | - | 1.33 µs | 63.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 25.74 µs | cpu.on_cpu (0.00, 64.7 µs); sched.run_queue (1.00, 0.0 µs) |
| qps-symbol-history-cold | pg | - | 0.00 µs | 0.00 µs | 71.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 21.92 µs | none |
| qps-symbol-history-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 83.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 12.67 µs | 199.51 µs | SQL*Net message to client (1.00, 12.7 µs) |
| qps-symbol-history-warm | kds-group | - | - | 1.08 µs | 59.72 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 31.59 µs | cpu.on_cpu (0.00, 60.7 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-symbol-history-warm | pg | - | 0.00 µs | 0.00 µs | 75.49 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 14.72 µs | CPU (0.08, 73.4 µs) |
| qps-symbol-history-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 69.58 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.34 µs | 182.14 µs | SQL*Net message to client (1.00, 10.3 µs) |
| qps-cross-join-cold | kds-group | - | - | 12.49 µs | 15.70 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.43 µs | cpu.on_cpu (0.00, 28.2 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-cross-join-cold | pg | - | 0.00 µs | 0.00 µs | 43.01 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.15 µs | 0.35 µs | CPU (0.02, 18.6 µs); ClientRead (0.01, 5.2 µs) |
| qps-cross-join-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 20.35 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.26 µs | 4.16 µs | SQL*Net message to client (1.00, 0.3 µs) |
| qps-cross-join-warm | kds-group | - | - | 12.39 µs | 15.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.36 µs | cpu.on_cpu (0.00, 27.9 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-cross-join-warm | pg | - | 0.00 µs | 0.00 µs | 41.85 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.32 µs | CPU (0.04, 34.6 µs) |
| qps-cross-join-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 18.74 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.28 µs | 3.72 µs | SQL*Net message to client (1.00, 0.3 µs) |
| qps-point-join-cold | kds-group | - | - | 1.98 µs | 2.57 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.11 µs | cpu.on_cpu (0.00, 4.5 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-point-join-cold | pg | - | 0.00 µs | 0.00 µs | 1.75 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.12 µs | none |
| qps-point-join-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.40 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.18 µs | 0.98 µs | SQL*Net message to client (1.00, 0.2 µs) |
| qps-point-join-warm | kds-group | - | - | 1.96 µs | 2.52 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.09 µs | cpu.on_cpu (0.00, 4.4 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-point-join-warm | pg | - | 0.00 µs | 0.00 µs | 1.66 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.11 µs | none |
| qps-point-join-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.30 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.19 µs | 0.99 µs | SQL*Net message to client (1.00, 0.2 µs) |
| qps-model-join-cold | kds-group | - | - | 3.86 µs | 4.86 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.43 µs | cpu.on_cpu (0.00, 8.7 µs); sched.run_queue (1.00, 0.0 µs) |
| qps-model-join-cold | pg | - | 0.00 µs | 0.00 µs | 4.29 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.82 µs | none |
| qps-model-join-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 18.29 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.86 µs | 7.93 µs | SQL*Net message to client (1.00, 0.9 µs) |
| qps-model-join-warm | kds-group | - | - | 3.29 µs | 4.51 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.84 µs | cpu.on_cpu (0.00, 7.8 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-model-join-warm | pg | - | 0.00 µs | 0.00 µs | 4.18 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.71 µs | CPU (0.01, 9.0 µs) |
| qps-model-join-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 7.24 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.29 µs | 7.27 µs | SQL*Net message to client (1.00, 0.3 µs) |
| write-batch-1 | kds-group | - | - | 0.00 µs | 5.62 µs | 0.00 µs | 121.72 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 121.7 µs); cpu.on_cpu (0.00, 5.5 µs); sched.run_queue (1.00, 0.1 µs) |
| write-batch-1 | pg | - | 0.00 µs | 0.00 µs | 1.56 µs | 0.00 µs | 113.45 µs | 0.00 µs | 12.49 µs | 0.00 µs | WalSync (0.11, 112.9 µs); ClientRead (0.01, 12.5 µs); CPU (0.00, 3.2 µs); WalWrite (0.00, 0.6 µs) |
| write-batch-1 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 13.68 µs | 0.00 µs | 166.85 µs | 0.00 µs | 0.74 µs | 0.00 µs | log file sync (1.00, 166.9 µs); SQL*Net message to client (1.00, 0.7 µs); log file switch (checkpoint incomplete) (0.00, 0.0 µs); log file switch completion (0.00, 0.0 µs) |
| write-batch-10 | kds-group | - | - | 0.00 µs | 4.25 µs | 0.00 µs | 12.05 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (0.10, 12.1 µs); cpu.on_cpu (0.00, 4.2 µs); sched.run_queue (1.20, 0.1 µs) |
| write-batch-10 | pg | - | 0.00 µs | 0.00 µs | 1.16 µs | 0.00 µs | 10.08 µs | 0.00 µs | 3.05 µs | 0.00 µs | WalSync (0.01, 10.1 µs); ClientRead (0.00, 3.1 µs); CPU (0.00, 1.8 µs) |
| write-batch-10 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.40 µs | 0.00 µs | 13.68 µs | 0.00 µs | 0.23 µs | 0.00 µs | log file sync (0.10, 13.7 µs); SQL*Net message to client (1.10, 0.2 µs) |
| write-batch-100 | kds-group | - | - | 0.00 µs | 4.24 µs | 0.00 µs | 1.51 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.2 µs); commit.wal_flush (0.01, 1.5 µs); sched.run_queue (1.02, 0.1 µs) |
| write-batch-100 | pg | - | 0.00 µs | 0.00 µs | 1.09 µs | 0.00 µs | 1.92 µs | 0.00 µs | 2.06 µs | 0.00 µs | ClientRead (0.00, 2.1 µs); WalSync (0.00, 1.9 µs) |
| write-batch-100 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.75 µs | 0.00 µs | 2.47 µs | 0.00 µs | 0.20 µs | 0.00 µs | log file sync (0.01, 2.5 µs); SQL*Net message to client (1.01, 0.2 µs) |
| write-batch-1000 | kds-group | - | - | 0.00 µs | 4.50 µs | 0.00 µs | 0.19 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.4 µs); commit.wal_flush (0.00, 0.2 µs); sched.run_queue (1.00, 0.1 µs) |
| write-batch-1000 | pg | - | 0.00 µs | 0.00 µs | 1.09 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.52 µs | 0.00 µs | ClientRead (0.00, 2.5 µs); CPU (0.00, 0.5 µs) |
| write-batch-1000 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.51 µs | 0.00 µs | 0.67 µs | 0.00 µs | 0.18 µs | 0.00 µs | log file sync (0.00, 0.7 µs); SQL*Net message to client (1.00, 0.2 µs) |
| join-conn-1 | kds-group | - | - | 12.36 µs | 15.62 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.38 µs | cpu.on_cpu (0.00, 27.9 µs); sched.run_queue (1.00, 0.1 µs) |
| join-conn-1 | pg | - | 0.00 µs | 0.00 µs | 41.91 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.39 µs | 0.40 µs | CPU (0.04, 41.6 µs); ClientRead (0.01, 4.4 µs) |
| join-conn-1 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 19.48 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | 3.84 µs | SQL*Net message to client (1.00, 0.2 µs) |
| join-conn-2 | kds-group | - | - | 12.27 µs | 15.65 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.48 µs | cpu.on_cpu (0.00, 27.5 µs); sched.run_queue (1.00, 0.2 µs) |
| join-conn-2 | pg | - | 0.00 µs | 0.00 µs | 43.02 µs | 0.00 µs | 0.00 µs | 0.00 µs | 6.49 µs | 0.42 µs | CPU (0.04, 42.4 µs); ClientRead (0.01, 6.5 µs) |
| join-conn-2 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 23.71 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.39 µs | 5.42 µs | SQL*Net message to client (1.00, 0.4 µs) |
| join-conn-4 | kds-group | - | - | 12.33 µs | 37.36 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.54 µs | cpu.on_cpu (0.00, 27.7 µs); sched.run_queue (1.00, 22.1 µs) |
| join-conn-4 | pg | - | 0.00 µs | 0.00 µs | 48.60 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.50 µs | 1.58 µs | CPU (0.06, 58.7 µs); ClientRead (0.01, 2.5 µs) |
| join-conn-4 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 27.34 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.41 µs | 3.91 µs | SQL*Net message to client (1.00, 0.4 µs) |
| join-conn-8 | kds-group | - | - | 12.61 µs | 83.39 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.99 µs | sched.run_queue (1.00, 68.1 µs); cpu.on_cpu (0.00, 27.9 µs) |
| join-conn-8 | pg | - | 0.00 µs | 0.00 µs | 61.36 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.82 µs | CPU (0.08, 75.3 µs) |
| join-conn-8 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 32.75 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.78 µs | 4.85 µs | resmgr:cpu quantum (0.03, 2.7 µs); SQL*Net message to client (1.00, 0.8 µs) |

### s1 `bars10080`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| load-exchanges | kds-group | 2 ops | 254.2 µs (18.4 %) | 251.9 µs | 251.9 µs | 256.5 µs | 256.5 µs | 256.5 µs | 2,711 ops/s | 13.4 µs | 0.00 µs | 30.7 µs | 0 errors |
| load-exchanges | pg | 2 ops | 180.8 µs (11.5 %) | 134.1 µs | 134.1 µs | 206.5 µs | 206.5 µs | 206.5 µs | 4,442 ops/s | 18.3 µs | 0.00 µs | 17.0 µs | 0 errors |
| load-exchanges | oracle | 2 ops | 999.6 µs (10.3 %) | 597.4 µs | 597.4 µs | 1,321.8 µs | 1,321.8 µs | 1,321.8 µs | 713 ops/s | 308.3 µs | 0.00 µs | 166.0 µs | 0 errors |
| load-symbols | kds-group | 8 ops | 226.7 µs (637.7 %) | 139.1 µs | 202.8 µs | 237.3 µs | 293.1 µs | 293.1 µs | 4,097 ops/s | 14.8 µs | 0.00 µs | 20.0 µs | 0 errors |
| load-symbols | pg | 8 ops | 158.6 µs (44.2 %) | 129.6 µs | 130.2 µs | 131.8 µs | 258.9 µs | 258.9 µs | 6,087 ops/s | 8.5 µs | 0.00 µs | 10.4 µs | 0 errors |
| load-symbols | oracle | 8 ops | 477.7 µs (299.6 %) | 200.2 µs | 351.3 µs | 516.6 µs | 971.5 µs | 971.5 µs | 1,893 ops/s | 76.4 µs | 0.00 µs | 95.9 µs | 0 errors |
| load-sessions | kds-group | 1,260 ops | 16.0 µs (13.7 %) | 12.3 µs | 13.9 µs | 14.9 µs | 21.4 µs | 37.2 µs | 57,709 ops/s | 3.3 µs | 0.00 µs | 8.5 µs | 0 errors |
| load-sessions | pg | 1,260 ops | 13.7 µs (7.5 %) | 12.0 µs | 13.2 µs | 13.3 µs | 14.2 µs | 19.7 µs | 67,790 ops/s | 4.6 µs | 0.00 µs | 5.0 µs | 0 errors |
| load-sessions | oracle | 1,260 ops | 20.7 µs (11.1 %) | 14.2 µs | 15.4 µs | 15.9 µs | 29.4 µs | 36.3 µs | 43,724 ops/s | 8.0 µs | 0.00 µs | 8.4 µs | 0 errors |
| load-bars | kds-group | 10,080 ops | 13.2 µs (0.3 %) | 11.3 µs | 12.8 µs | 12.9 µs | 13.3 µs | 20.8 µs | 70,617 ops/s | 2.6 µs | 0.00 µs | 6.6 µs | 0 errors |
| load-bars | pg | 10,080 ops | 14.4 µs (2.9 %) | 12.5 µs | 13.8 µs | 13.9 µs | 14.8 µs | 24.3 µs | 64,946 ops/s | 4.6 µs | 0.00 µs | 5.7 µs | 0 errors |
| load-bars | oracle | 10,080 ops | 28.9 µs (41.5 %) | 14.0 µs | 15.8 µs | 16.0 µs | 21.5 µs | 60.5 µs | 32,893 ops/s | 5.4 µs | 0.00 µs | 9.4 µs | 0 errors |
| load-stats | kds-group | 10,080 ops | 15.8 µs (3.9 %) | 13.6 µs | 15.0 µs | 15.2 µs | 16.8 µs | 23.9 µs | 57,611 ops/s | 2.8 µs | 0.00 µs | 9.0 µs | 0 errors |
| load-stats | pg | 10,080 ops | 14.8 µs (2.5 %) | 12.7 µs | 14.0 µs | 14.2 µs | 15.2 µs | 29.5 µs | 60,745 ops/s | 4.6 µs | 0.00 µs | 6.2 µs | 0 errors |
| load-stats | oracle | 10,080 ops | 18.0 µs (42.4 %) | 14.3 µs | 16.2 µs | 16.5 µs | 21.4 µs | 44.7 µs | 45,777 ops/s | 5.2 µs | 0.00 µs | 9.0 µs | 0 errors |
| load-models | kds-group | 8 ops | 206.0 µs (713.1 %) | 132.3 µs | 199.0 µs | 211.3 µs | 290.1 µs | 290.1 µs | 4,341 ops/s | 11.2 µs | 0.00 µs | 10.9 µs | 0 errors |
| load-models | pg | 8 ops | 219.9 µs (33.2 %) | 130.5 µs | 227.5 µs | 229.8 µs | 275.9 µs | 275.9 µs | 4,180 ops/s | 16.6 µs | 0.00 µs | 10.9 µs | 0 errors |
| load-models | oracle | 8 ops | 395.9 µs (38.4 %) | 154.8 µs | 299.4 µs | 407.3 µs | 799.0 µs | 799.0 µs | 2,382 ops/s | 37.8 µs | 0.00 µs | 81.4 µs | 0 errors |
| backtest-read | kds-group | 60 ops | 161.6 µs (15.0 %) | 135.3 µs | 137.0 µs | 139.0 µs | 273.5 µs | 291.7 µs | 6,089 ops/s | 8.5 µs | 1.78 µs | 130.0 µs | 0 errors |
| backtest-read | pg | 60 ops | 248.7 µs (18.1 %) | 221.5 µs | 230.6 µs | 235.2 µs | 341.6 µs | 358.5 µs | 3,931 ops/s | 18.9 µs | 1.85 µs | 211.5 µs | 0 errors |
| backtest-read | oracle | 60 ops | 147.6 µs (12.3 %) | 100.4 µs | 103.0 µs | 104.7 µs | 364.6 µs | 500.5 µs | 6,570 ops/s | 23.8 µs | 10.01 µs | 96.2 µs | 0 errors |
| result-insert | kds-group | 472 ops | 157.8 µs (116.6 %) | 129.6 µs | 136.7 µs | 142.6 µs | 281.6 µs | 346.8 µs | 6,320 ops/s | 10.4 µs | 0.00 µs | 14.8 µs | 0 errors |
| result-insert | pg | 472 ops | 155.0 µs (0.5 %) | 132.0 µs | 138.3 µs | 142.4 µs | 180.2 µs | 405.1 µs | 6,433 ops/s | 14.9 µs | 0.00 µs | 11.8 µs | 0 errors |
| result-insert | oracle | 472 ops | 250.1 µs (117.4 %) | 144.9 µs | 162.8 µs | 187.6 µs | 482.0 µs | 668.5 µs | 3,979 ops/s | 22.1 µs | 0.00 µs | 52.3 µs | 0 errors |
| backtest-replay | kds-group | 60 ops | 154.1 µs (29.5 %) | 149.1 µs | 151.9 µs | 153.6 µs | 158.6 µs | 168.1 µs | 6,374 ops/s | 18.2 µs | 3.14 µs | 128.1 µs | 0 errors |
| backtest-replay | pg | 60 ops | 341.1 µs (20.6 %) | 238.6 µs | 248.3 µs | 256.9 µs | 582.1 µs | 613.1 µs | 2,890 ops/s | 50.7 µs | 2.79 µs | 224.5 µs | 0 errors |
| backtest-replay | oracle | 60 ops | 191.4 µs (39.6 %) | 122.0 µs | 137.7 µs | 147.0 µs | 401.3 µs | 473.3 µs | 5,028 ops/s | 64.8 µs | 26.72 µs | 96.1 µs | 0 errors |
| compare-all | kds-group | 1 ops | 566.7 µs (10.4 %) | 566.7 µs | 566.7 µs | 566.7 µs | 566.7 µs | 566.7 µs | 1,191 ops/s | 235.3 µs | 171.56 µs | 197.4 µs | 0 errors |
| compare-all | pg | 1 ops | 490.1 µs (34.5 %) | 490.1 µs | 490.1 µs | 490.1 µs | 490.1 µs | 490.1 µs | 1,561 ops/s | 303.2 µs | 76.19 µs | 130.0 µs | 0 errors |
| compare-all | oracle | 1 ops | 667.9 µs (2.3 %) | 667.9 µs | 667.9 µs | 667.9 µs | 667.9 µs | 667.9 µs | 1,115 ops/s | 430.7 µs | 229.01 µs | 179.0 µs | 0 errors |
| compare-one | kds-group | 32 ops | 68.6 µs (41.0 %) | 48.7 µs | 50.2 µs | 54.7 µs | 121.3 µs | 218.8 µs | 13,697 ops/s | 32.0 µs | 14.15 µs | 22.0 µs | 0 errors |
| compare-one | pg | 32 ops | 76.2 µs (145.2 %) | 50.8 µs | 52.1 µs | 60.0 µs | 181.6 µs | 291.0 µs | 12,435 ops/s | 36.5 µs | 3.52 µs | 24.2 µs | 0 errors |
| compare-one | oracle | 32 ops | 112.3 µs (23.4 %) | 43.8 µs | 45.8 µs | 47.3 µs | 314.4 µs | 441.7 µs | 8,444 ops/s | 34.9 µs | 19.68 µs | 25.8 µs | 0 errors |
| read-bar-lookup | kds-group | 200 ops | 20.1 µs (18.3 %) | 17.3 µs | 17.8 µs | 18.2 µs | 25.6 µs | 30.9 µs | 47,928 ops/s | 8.6 µs | 0.30 µs | 6.6 µs | 0 errors |
| read-bar-lookup | pg | 200 ops | 18.2 µs (38.9 %) | 14.3 µs | 16.0 µs | 17.4 µs | 20.6 µs | 59.5 µs | 53,536 ops/s | 8.7 µs | 0.10 µs | 5.8 µs | 0 errors |
| read-bar-lookup | oracle | 200 ops | 18.9 µs (34.6 %) | 14.8 µs | 15.6 µs | 15.7 µs | 18.0 µs | 20.7 µs | 49,956 ops/s | 7.6 µs | 1.54 µs | 6.3 µs | 0 errors |
| read-bar-range | kds-group | 200 ops | 74.7 µs (5.6 %) | 57.9 µs | 65.9 µs | 76.3 µs | 88.5 µs | 96.1 µs | 13,224 ops/s | 30.5 µs | 19.62 µs | 41.3 µs | 0 errors |
| read-bar-range | pg | 200 ops | 117.3 µs (16.3 %) | 93.3 µs | 96.5 µs | 97.9 µs | 283.5 µs | 392.9 µs | 8,437 ops/s | 74.9 µs | 8.66 µs | 48.3 µs | 0 errors |
| read-bar-range | oracle | 200 ops | 196.9 µs (8.6 %) | 157.0 µs | 162.6 µs | 164.9 µs | 327.0 µs | 413.3 µs | 5,047 ops/s | 133.4 µs | 106.28 µs | 44.2 µs | 0 errors |
| read-symbol-history | kds-group | 10 ops | 598.5 µs (14.7 %) | 497.9 µs | 542.6 µs | 584.5 µs | 695.9 µs | 695.9 µs | 1,610 ops/s | 175.5 µs | 117.73 µs | 358.2 µs | 0 errors |
| read-symbol-history | pg | 10 ops | 754.4 µs (12.5 %) | 585.3 µs | 715.2 µs | 776.5 µs | 1,044.1 µs | 1,044.1 µs | 1,289 ops/s | 493.4 µs | 70.50 µs | 491.4 µs | 0 errors |
| read-symbol-history | oracle | 10 ops | 1,501.8 µs (7.9 %) | 1,404.8 µs | 1,427.5 µs | 1,469.2 µs | 1,811.4 µs | 1,811.4 µs | 648 ops/s | 1,152.7 µs | 937.02 µs | 374.8 µs | 0 errors |
| read-day-slice | kds-group | 200 ops | 126.4 µs (6.3 %) | 114.4 µs | 119.3 µs | 123.5 µs | 134.5 µs | 232.7 µs | 7,854 ops/s | 8.8 µs | 1.54 µs | 109.0 µs | 0 errors |
| read-day-slice | pg | 200 ops | 227.2 µs (6.1 %) | 202.8 µs | 211.2 µs | 217.9 µs | 330.7 µs | 448.9 µs | 4,373 ops/s | 16.2 µs | 0.83 µs | 198.4 µs | 0 errors |
| read-day-slice | oracle | 200 ops | 99.2 µs (2.3 %) | 70.3 µs | 82.5 µs | 83.0 µs | 214.6 µs | 406.6 µs | 9,985 ops/s | 14.6 µs | 6.80 µs | 70.7 µs | 0 errors |
| read-join-point | kds-group | 200 ops | 23.0 µs (6.5 %) | 20.9 µs | 21.4 µs | 21.7 µs | 25.8 µs | 58.3 µs | 41,269 ops/s | 8.6 µs | 0.28 µs | 8.6 µs | 0 errors |
| read-join-point | pg | 200 ops | 31.1 µs (14.4 %) | 24.6 µs | 25.1 µs | 25.4 µs | 32.5 µs | 171.5 µs | 30,887 ops/s | 14.9 µs | 0.24 µs | 8.7 µs | 0 errors |
| read-join-point | oracle | 200 ops | 18.9 µs (64.4 %) | 15.0 µs | 16.3 µs | 16.5 µs | 21.1 µs | 63.9 µs | 47,651 ops/s | 6.3 µs | 0.91 µs | 8.1 µs | 0 errors |
| read-join-exists | kds-group | 10 ops | 406.5 µs (22.1 %) | 335.5 µs | 349.5 µs | 425.9 µs | 505.9 µs | 505.9 µs | 2,359 ops/s | 21.2 µs | 1.20 µs | 329.8 µs | 0 errors |
| read-join-exists | pg | 10 ops | 848.4 µs (22.6 %) | 721.5 µs | 735.4 µs | 900.2 µs | 1,015.7 µs | 1,015.7 µs | 1,143 ops/s | 70.5 µs | 2.25 µs | 687.0 µs | 0 errors |
| read-join-exists | oracle | 10 ops | 183.2 µs (111.7 %) | 131.6 µs | 134.6 µs | 143.7 µs | 412.5 µs | 412.5 µs | 5,037 ops/s | 24.8 µs | 2.08 µs | 128.6 µs | 0 errors |
| agg-global | kds-group | 10 ops | 373.3 µs (39.2 %) | 309.0 µs | 315.7 µs | 327.4 µs | 505.3 µs | 505.3 µs | 2,451 ops/s | 30.1 µs | 0.84 µs | 294.9 µs | 0 errors |
| agg-global | pg | 10 ops | 585.4 µs (33.8 %) | 352.8 µs | 493.0 µs | 626.8 µs | 705.7 µs | 705.7 µs | 1,622 ops/s | 58.3 µs | 0.87 µs | 349.3 µs | 0 errors |
| agg-global | oracle | 10 ops | 326.8 µs (20.3 %) | 230.2 µs | 239.4 µs | 332.1 µs | 501.2 µs | 501.2 µs | 2,793 ops/s | 38.6 µs | 4.56 µs | 219.0 µs | 0 errors |
| agg-by-symbol | kds-group | 10 ops | 722.1 µs (19.9 %) | 597.1 µs | 651.3 µs | 760.5 µs | 789.3 µs | 789.3 µs | 1,316 ops/s | 49.0 µs | 2.48 µs | 573.0 µs | 0 errors |
| agg-by-symbol | pg | 10 ops | 886.6 µs (22.7 %) | 742.5 µs | 773.5 µs | 915.2 µs | 1,005.8 µs | 1,005.8 µs | 1,087 ops/s | 115.0 µs | 1.88 µs | 578.0 µs | 0 errors |
| agg-by-symbol | oracle | 10 ops | 398.7 µs (37.2 %) | 294.7 µs | 299.7 µs | 354.6 µs | 632.5 µs | 632.5 µs | 2,452 ops/s | 32.2 µs | 5.32 µs | 285.3 µs | 0 errors |
| agg-by-session | kds-group | 10 ops | 966.2 µs (15.8 %) | 894.3 µs | 916.9 µs | 978.0 µs | 1,084.7 µs | 1,084.7 µs | 1,018 ops/s | 143.0 µs | 84.88 µs | 730.5 µs | 0 errors |
| agg-by-session | pg | 10 ops | 1,326.3 µs (3.2 %) | 1,152.1 µs | 1,292.3 µs | 1,320.7 µs | 1,414.1 µs | 1,414.1 µs | 738 ops/s | 417.3 µs | 37.50 µs | 848.5 µs | 0 errors |
| agg-by-session | oracle | 10 ops | 925.9 µs (4.9 %) | 710.3 µs | 882.2 µs | 929.8 µs | 1,139.2 µs | 1,139.2 µs | 1,051 ops/s | 321.3 µs | 220.96 µs | 508.8 µs | 0 errors |
| agg-day-slice | kds-group | 200 ops | 131.0 µs (17.9 %) | 122.0 µs | 125.0 µs | 128.2 µs | 137.3 µs | 252.3 µs | 7,561 ops/s | 13.0 µs | 0.86 µs | 108.7 µs | 0 errors |
| agg-day-slice | pg | 200 ops | 224.3 µs (2.0 %) | 205.2 µs | 218.9 µs | 225.4 µs | 236.9 µs | 242.1 µs | 4,440 ops/s | 19.1 µs | 0.59 µs | 199.9 µs | 0 errors |
| agg-day-slice | oracle | 200 ops | 100.0 µs (8.1 %) | 76.2 µs | 86.1 µs | 87.9 µs | 125.2 µs | 364.3 µs | 9,878 ops/s | 11.6 µs | 3.09 µs | 79.5 µs | 0 errors |
| agg-distinct | kds-group | 10 ops | 629.4 µs (7.6 %) | 489.3 µs | 614.5 µs | 635.4 µs | 664.6 µs | 664.6 µs | 1,486 ops/s | 62.0 µs | 1.57 µs | 452.9 µs | 0 errors |
| agg-distinct | pg | 10 ops | 651.4 µs (12.6 %) | 429.7 µs | 572.5 µs | 684.2 µs | 844.6 µs | 844.6 µs | 1,461 ops/s | 87.0 µs | 0.69 µs | 382.1 µs | 0 errors |
| agg-distinct | oracle | 10 ops | 396.4 µs (37.7 %) | 164.5 µs | 280.5 µs | 456.2 µs | 776.1 µs | 776.1 µs | 2,375 ops/s | 55.0 µs | 3.38 µs | 201.0 µs | 0 errors |
| qps-bar-lookup-cold | kds-group | 100 ops | 32.5 µs (10.4 %) | 29.4 µs | 30.5 µs | 30.8 µs | 41.3 µs | 45.2 µs | 27,024 ops/s | 15.9 µs | 0.61 µs | 7.5 µs | 0 errors |
| qps-bar-lookup-cold | pg | 100 ops | 58.7 µs (40.6 %) | 31.6 µs | 32.3 µs | 37.2 µs | 238.7 µs | 342.5 µs | 16,194 ops/s | 26.5 µs | 0.36 µs | 10.6 µs | 0 errors |
| qps-bar-lookup-cold | oracle | 100 ops | 29.9 µs (24.2 %) | 20.7 µs | 21.3 µs | 21.7 µs | 25.5 µs | 141.2 µs | 31,449 ops/s | 14.5 µs | 2.65 µs | 7.2 µs | 0 errors |
| qps-bar-lookup-warm | kds-group | 100 ops | 22.9 µs (20.8 %) | 22.1 µs | 22.4 µs | 22.6 µs | 24.5 µs | 35.5 µs | 39,196 ops/s | 10.4 µs | 0.41 µs | 6.8 µs | 0 errors |
| qps-bar-lookup-warm | pg | 100 ops | 24.5 µs (97.9 %) | 22.1 µs | 22.5 µs | 22.7 µs | 42.1 µs | 52.7 µs | 37,580 ops/s | 13.7 µs | 0.20 µs | 5.9 µs | 0 errors |
| qps-bar-lookup-warm | oracle | 100 ops | 29.6 µs (9.7 %) | 21.2 µs | 21.5 µs | 21.7 µs | 24.0 µs | 177.5 µs | 31,424 ops/s | 14.8 µs | 2.58 µs | 7.5 µs | 0 errors |
| qps-bar-range-cold | kds-group | 100 ops | 119.2 µs (40.2 %) | 80.2 µs | 86.1 µs | 99.9 µs | 186.9 µs | 290.3 µs | 8,107 ops/s | 50.3 µs | 31.17 µs | 42.6 µs | 0 errors |
| qps-bar-range-cold | pg | 100 ops | 125.5 µs (29.9 %) | 92.6 µs | 94.7 µs | 97.7 µs | 299.7 µs | 495.0 µs | 7,789 ops/s | 80.0 µs | 8.27 µs | 48.7 µs | 0 errors |
| qps-bar-range-cold | oracle | 100 ops | 203.0 µs (13.9 %) | 158.3 µs | 164.0 µs | 166.0 µs | 358.0 µs | 457.9 µs | 4,877 ops/s | 135.9 µs | 106.00 µs | 45.2 µs | 0 errors |
| qps-bar-range-warm | kds-group | 100 ops | 70.5 µs (23.4 %) | 60.5 µs | 64.5 µs | 67.7 µs | 78.4 µs | 187.4 µs | 13,492 ops/s | 23.9 µs | 15.31 µs | 39.3 µs | 0 errors |
| qps-bar-range-warm | pg | 100 ops | 106.5 µs (19.7 %) | 93.0 µs | 94.7 µs | 95.5 µs | 120.6 µs | 313.5 µs | 9,304 ops/s | 75.0 µs | 7.94 µs | 46.2 µs | 0 errors |
| qps-bar-range-warm | oracle | 100 ops | 203.9 µs (24.5 %) | 159.9 µs | 164.2 µs | 166.5 µs | 335.0 µs | 537.7 µs | 4,859 ops/s | 138.6 µs | 108.05 µs | 50.4 µs | 0 errors |
| qps-day-slice-cold | kds-group | 100 ops | 123.2 µs (7.1 %) | 115.3 µs | 117.8 µs | 119.0 µs | 132.5 µs | 216.7 µs | 8,059 ops/s | 6.2 µs | 1.09 µs | 110.1 µs | 0 errors |
| qps-day-slice-cold | pg | 100 ops | 213.2 µs (6.1 %) | 201.1 µs | 207.8 µs | 211.6 µs | 232.1 µs | 349.7 µs | 4,633 ops/s | 9.6 µs | 0.46 µs | 197.6 µs | 0 errors |
| qps-day-slice-cold | oracle | 100 ops | 103.7 µs (23.2 %) | 73.8 µs | 83.7 µs | 84.3 µs | 252.2 µs | 346.8 µs | 9,447 ops/s | 15.9 µs | 7.16 µs | 73.6 µs | 0 errors |
| qps-day-slice-warm | kds-group | 100 ops | 140.9 µs (5.4 %) | 122.2 µs | 128.6 µs | 131.7 µs | 200.9 µs | 242.7 µs | 7,039 ops/s | 13.6 µs | 2.35 µs | 115.0 µs | 0 errors |
| qps-day-slice-warm | pg | 100 ops | 246.4 µs (20.0 %) | 219.8 µs | 226.8 µs | 230.2 µs | 368.8 µs | 416.4 µs | 4,009 ops/s | 30.8 µs | 1.57 µs | 196.7 µs | 0 errors |
| qps-day-slice-warm | oracle | 100 ops | 94.2 µs (13.2 %) | 73.3 µs | 83.4 µs | 84.4 µs | 116.3 µs | 319.1 µs | 10,367 ops/s | 15.4 µs | 7.07 µs | 69.0 µs | 0 errors |
| qps-symbol-history-cold | kds-group | 7 ops | 616.7 µs (16.0 %) | 482.2 µs | 566.2 µs | 582.8 µs | 738.1 µs | 738.1 µs | 1,513 ops/s | 163.1 µs | 117.90 µs | 361.0 µs | 0 errors |
| qps-symbol-history-cold | pg | 7 ops | 806.8 µs (9.9 %) | 632.5 µs | 778.0 µs | 784.6 µs | 1,005.1 µs | 1,005.1 µs | 1,209 ops/s | 508.9 µs | 79.50 µs | 473.6 µs | 0 errors |
| qps-symbol-history-cold | oracle | 7 ops | 1,538.1 µs (8.4 %) | 1,409.9 µs | 1,477.5 µs | 1,499.2 µs | 1,878.0 µs | 1,878.0 µs | 640 ops/s | 1,211.5 µs | 962.36 µs | 385.4 µs | 0 errors |
| qps-symbol-history-warm | kds-group | 100 ops | 502.0 µs (4.6 %) | 393.0 µs | 448.2 µs | 464.8 µs | 623.6 µs | 717.4 µs | 1,986 ops/s | 146.9 µs | 110.60 µs | 381.4 µs | 0 errors |
| qps-symbol-history-warm | pg | 100 ops | 698.7 µs (14.8 %) | 566.2 µs | 583.7 µs | 669.7 µs | 891.0 µs | 947.2 µs | 1,426 ops/s | 464.7 µs | 63.82 µs | 463.4 µs | 0 errors |
| qps-symbol-history-warm | oracle | 100 ops | 1,469.8 µs (5.0 %) | 1,234.6 µs | 1,454.4 µs | 1,475.4 µs | 1,560.9 µs | 1,686.6 µs | 679 ops/s | 1,117.5 µs | 913.80 µs | 384.4 µs | 0 errors |
| qps-cross-join-cold | kds-group | 100 ops | 148.7 µs (0.7 %) | 134.1 µs | 135.7 µs | 136.9 µs | 232.2 µs | 277.5 µs | 6,657 ops/s | 5.3 µs | 0.73 µs | 129.8 µs | 0 errors |
| qps-cross-join-cold | pg | 100 ops | 242.2 µs (6.9 %) | 214.1 µs | 219.6 µs | 224.5 µs | 342.3 µs | 411.2 µs | 4,101 ops/s | 11.4 µs | 0.63 µs | 214.1 µs | 0 errors |
| qps-cross-join-cold | oracle | 100 ops | 136.3 µs (22.1 %) | 88.1 µs | 101.6 µs | 102.6 µs | 296.7 µs | 430.2 µs | 7,158 ops/s | 17.0 µs | 7.54 µs | 94.9 µs | 0 errors |
| qps-cross-join-warm | kds-group | 100 ops | 157.4 µs (7.1 %) | 135.9 µs | 140.2 µs | 143.1 µs | 225.6 µs | 269.5 µs | 6,263 ops/s | 10.6 µs | 1.51 µs | 129.1 µs | 0 errors |
| qps-cross-join-warm | pg | 100 ops | 256.3 µs (39.2 %) | 223.4 µs | 237.6 µs | 241.2 µs | 360.8 µs | 385.4 µs | 3,870 ops/s | 30.7 µs | 1.85 µs | 209.1 µs | 0 errors |
| qps-cross-join-warm | oracle | 100 ops | 156.1 µs (35.9 %) | 88.8 µs | 101.8 µs | 109.1 µs | 327.4 µs | 480.0 µs | 6,322 ops/s | 23.6 µs | 9.88 µs | 96.5 µs | 0 errors |
| qps-point-join-cold | kds-group | 100 ops | 27.6 µs (1.5 %) | 25.8 µs | 26.3 µs | 26.6 µs | 31.6 µs | 37.5 µs | 33,182 ops/s | 11.8 µs | 0.39 µs | 9.1 µs | 0 errors |
| qps-point-join-cold | pg | 100 ops | 35.2 µs (78.4 %) | 28.9 µs | 29.6 µs | 30.3 µs | 43.9 µs | 46.0 µs | 26,705 ops/s | 18.7 µs | 0.36 µs | 8.5 µs | 0 errors |
| qps-point-join-cold | oracle | 100 ops | 31.2 µs (21.2 %) | 19.0 µs | 19.5 µs | 20.4 µs | 61.4 µs | 248.3 µs | 30,750 ops/s | 12.4 µs | 1.75 µs | 8.8 µs | 0 errors |
| qps-point-join-warm | kds-group | 100 ops | 28.9 µs (18.8 %) | 27.2 µs | 27.8 µs | 28.0 µs | 32.3 µs | 53.1 µs | 31,250 ops/s | 12.6 µs | 0.42 µs | 9.1 µs | 0 errors |
| qps-point-join-warm | pg | 100 ops | 28.8 µs (52.2 %) | 23.5 µs | 23.9 µs | 24.1 µs | 35.9 µs | 49.6 µs | 33,203 ops/s | 15.3 µs | 0.25 µs | 8.2 µs | 0 errors |
| qps-point-join-warm | oracle | 100 ops | 25.4 µs (33.4 %) | 20.0 µs | 21.2 µs | 21.5 µs | 23.7 µs | 32.5 µs | 35,124 ops/s | 13.8 µs | 1.79 µs | 7.9 µs | 0 errors |
| qps-model-join-cold | kds-group | 7 ops | 151.2 µs (10.5 %) | 88.4 µs | 122.2 µs | 135.4 µs | 236.4 µs | 236.4 µs | 5,117 ops/s | 60.5 µs | 18.44 µs | 23.7 µs | 0 errors |
| qps-model-join-cold | pg | 7 ops | 99.3 µs (23.5 %) | 80.3 µs | 89.5 µs | 91.7 µs | 132.5 µs | 132.5 µs | 8,967 ops/s | 71.0 µs | 7.56 µs | 22.9 µs | 0 errors |
| qps-model-join-cold | oracle | 7 ops | 318.5 µs (55.7 %) | 89.3 µs | 243.1 µs | 252.0 µs | 720.6 µs | 720.6 µs | 2,681 ops/s | 118.7 µs | 46.78 µs | 58.7 µs | 0 errors |
| qps-model-join-warm | kds-group | 100 ops | 65.3 µs (6.1 %) | 53.1 µs | 54.3 µs | 56.8 µs | 99.3 µs | 218.1 µs | 14,660 ops/s | 30.6 µs | 13.27 µs | 22.1 µs | 0 errors |
| qps-model-join-warm | pg | 100 ops | 65.6 µs (81.8 %) | 52.0 µs | 58.7 µs | 61.2 µs | 74.8 µs | 107.2 µs | 14,681 ops/s | 38.2 µs | 3.85 µs | 21.1 µs | 0 errors |
| qps-model-join-warm | oracle | 100 ops | 78.2 µs (38.4 %) | 45.8 µs | 49.0 µs | 49.6 µs | 81.1 µs | 246.0 µs | 12,332 ops/s | 38.3 µs | 23.07 µs | 16.1 µs | 0 errors |
| write-batch-1 | kds-group | 2,000 ops | 148.7 µs (141.2 %) | 126.7 µs | 138.1 µs | 140.4 µs | 168.4 µs | 287.1 µs | 6,717 ops/s | 10.1 µs | 0.00 µs | 13.3 µs | 0 errors |
| write-batch-1 | pg | 2,000 ops | 150.2 µs (1.8 %) | 124.3 µs | 137.2 µs | 140.1 µs | 174.9 µs | 344.4 µs | 6,650 ops/s | 12.8 µs | 0.00 µs | 11.3 µs | 0 errors |
| write-batch-1 | oracle | 2,000 ops | 284.9 µs (133.9 %) | 138.2 µs | 169.9 µs | 205.8 µs | 523.1 µs | 714.7 µs | 3,504 ops/s | 20.9 µs | 0.00 µs | 65.8 µs | 0 errors |
| write-batch-10 | kds-group | 2,000 ops | 31.4 µs (64.4 %) | 12.8 µs | 14.3 µs | 14.7 µs | 140.1 µs | 166.7 µs | 31,728 ops/s | 4.9 µs | 0.00 µs | 9.3 µs | 0 errors |
| write-batch-10 | pg | 2,000 ops | 29.0 µs (6.9 %) | 12.0 µs | 13.2 µs | 13.4 µs | 139.8 µs | 162.8 µs | 34,297 ops/s | 6.5 µs | 0.00 µs | 6.4 µs | 0 errors |
| write-batch-10 | oracle | 2,000 ops | 35.7 µs (76.2 %) | 13.2 µs | 14.6 µs | 16.5 µs | 177.5 µs | 210.9 µs | 27,845 ops/s | 7.8 µs | 0.00 µs | 10.5 µs | 0 errors |
| write-batch-100 | kds-group | 2,000 ops | 16.6 µs (35.4 %) | 12.6 µs | 13.9 µs | 14.2 µs | 15.8 µs | 146.1 µs | 59,577 ops/s | 2.8 µs | 0.00 µs | 8.0 µs | 0 errors |
| write-batch-100 | pg | 2,000 ops | 15.0 µs (2.8 %) | 11.9 µs | 13.0 µs | 13.2 µs | 14.9 µs | 141.4 µs | 65,920 ops/s | 4.7 µs | 0.00 µs | 5.0 µs | 0 errors |
| write-batch-100 | oracle | 2,000 ops | 19.5 µs (39.1 %) | 13.7 µs | 15.2 µs | 15.5 µs | 20.0 µs | 225.5 µs | 51,105 ops/s | 4.4 µs | 0.00 µs | 8.3 µs | 0 errors |
| write-batch-1000 | kds-group | 2,000 ops | 15.3 µs (7.1 %) | 13.0 µs | 14.2 µs | 14.5 µs | 15.5 µs | 27.6 µs | 65,023 ops/s | 2.7 µs | 0.00 µs | 8.3 µs | 0 errors |
| write-batch-1000 | pg | 2,000 ops | 14.3 µs (6.7 %) | 11.9 µs | 13.2 µs | 13.3 µs | 13.7 µs | 34.6 µs | 69,687 ops/s | 4.6 µs | 0.00 µs | 5.0 µs | 0 errors |
| write-batch-1000 | oracle | 2,000 ops | 16.8 µs (4.6 %) | 13.6 µs | 15.1 µs | 15.4 µs | 16.2 µs | 22.9 µs | 58,860 ops/s | 4.3 µs | 0.00 µs | 7.8 µs | 0 errors |
| join-conn-1 | kds-group | 200 ops | 141.3 µs (4.8 %) | 133.8 µs | 137.2 µs | 138.8 µs | 149.7 µs | 224.3 µs | 7,037 ops/s | 5.5 µs | 0.84 µs | 129.3 µs | 0 errors |
| join-conn-1 | pg | 200 ops | 233.4 µs (5.0 %) | 212.7 µs | 220.8 µs | 226.2 µs | 246.2 µs | 342.3 µs | 4,252 ops/s | 13.3 µs | 0.87 µs | 209.3 µs | 0 errors |
| join-conn-1 | oracle | 200 ops | 116.2 µs (10.1 %) | 88.4 µs | 102.4 µs | 104.2 µs | 218.8 µs | 359.0 µs | 8,514 ops/s | 17.7 µs | 8.35 µs | 87.5 µs | 0 errors |
| join-conn-2 | kds-group | 200 ops | 269.8 µs (7.8 %) | 155.6 µs | 256.0 µs | 258.7 µs | 444.6 µs | 588.8 µs | 7,062 ops/s | 21.8 µs | 3.17 µs | 128.8 µs | 0 errors |
| join-conn-2 | pg | 200 ops | 295.6 µs (7.4 %) | 229.3 µs | 241.4 µs | 247.2 µs | 566.3 µs | 667.5 µs | 6,454 ops/s | 33.6 µs | 2.12 µs | 219.2 µs | 0 errors |
| join-conn-2 | oracle | 200 ops | 174.6 µs (26.3 %) | 103.9 µs | 117.2 µs | 120.8 µs | 362.0 µs | 568.0 µs | 10,484 ops/s | 31.7 µs | 13.35 µs | 103.0 µs | 0 errors |
| join-conn-4 | kds-group | 200 ops | 507.4 µs (1.2 %) | 193.4 µs | 506.5 µs | 515.5 µs | 627.9 µs | 650.2 µs | 7,640 ops/s | 22.9 µs | 2.95 µs | 127.6 µs | 0 errors |
| join-conn-4 | pg | 200 ops | 366.6 µs (23.7 %) | 233.5 µs | 254.0 µs | 265.4 µs | 677.7 µs | 731.4 µs | 9,590 ops/s | 46.5 µs | 2.62 µs | 238.6 µs | 0 errors |
| join-conn-4 | oracle | 200 ops | 191.9 µs (27.0 %) | 92.9 µs | 108.7 µs | 114.6 µs | 468.4 µs | 911.5 µs | 16,459 ops/s | 22.0 µs | 8.09 µs | 117.1 µs | 0 errors |
| join-conn-8 | kds-group | 200 ops | 1,013.4 µs (2.7 %) | 277.9 µs | 1,026.5 µs | 1,031.9 µs | 1,113.0 µs | 1,215.5 µs | 7,652 ops/s | 31.8 µs | 3.83 µs | 127.9 µs | 0 errors |
| join-conn-8 | pg | 200 ops | 474.9 µs (8.1 %) | 251.7 µs | 277.7 µs | 487.3 µs | 754.9 µs | 828.5 µs | 12,973 ops/s | 65.3 µs | 3.42 µs | 259.8 µs | 0 errors |
| join-conn-8 | oracle | 200 ops | 286.9 µs (14.1 %) | 95.1 µs | 113.8 µs | 182.5 µs | 688.7 µs | 1,406.2 µs | 19,723 ops/s | 26.6 µs | 8.01 µs | 132.0 µs | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| load-exchanges | kds-group | - | - | - | 9.00 µs | 0.00 µs | 147.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 147.5 µs); cpu.on_cpu (0.00, 9.0 µs); sched.run_queue (1.00, 0.0 µs) |
| load-exchanges | pg | - | 0.00 µs | 0.00 µs | 2.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| load-exchanges | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 43.50 µs | 0.00 µs | 316.00 µs | 0.00 µs | 2.00 µs | 0.00 µs | log file sync (1.00, 316.0 µs); SQL*Net message to client (1.00, 2.0 µs) |
| load-symbols | kds-group | - | - | - | 6.00 µs | 0.00 µs | 140.62 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 140.6 µs); cpu.on_cpu (0.00, 5.9 µs); sched.run_queue (1.00, 0.1 µs) |
| load-symbols | pg | - | 0.00 µs | 0.00 µs | 1.62 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| load-symbols | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 27.00 µs | 0.00 µs | 213.88 µs | 0.00 µs | 0.75 µs | 0.00 µs | log file sync (1.00, 213.9 µs); SQL*Net message to client (1.00, 0.8 µs) |
| load-sessions | kds-group | - | - | - | 4.47 µs | 0.00 µs | 0.83 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.4 µs); commit.wal_flush (0.01, 0.8 µs); sched.run_queue (1.01, 0.1 µs) |
| load-sessions | pg | - | 0.00 µs | 0.00 µs | 1.06 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.00 µs | 0.00 µs | ClientRead (0.00, 1.0 µs); CPU (0.00, 0.8 µs) |
| load-sessions | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.78 µs | 0.00 µs | 1.83 µs | 0.00 µs | 0.20 µs | 0.00 µs | log file sync (0.01, 1.8 µs); SQL*Net message to client (1.01, 0.2 µs) |
| load-bars | kds-group | - | - | - | 2.47 µs | 0.00 µs | 0.82 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 2.4 µs); commit.wal_flush (0.01, 0.8 µs); sched.run_queue (1.01, 0.1 µs) |
| load-bars | pg | - | 0.00 µs | 0.00 µs | 1.26 µs | 0.00 µs | 3.00 µs | 0.00 µs | 1.84 µs | 0.00 µs | WalSync (0.00, 3.0 µs); ClientRead (0.00, 1.8 µs); CPU (0.00, 1.0 µs); DataFileExtend (0.00, 0.1 µs) |
| load-bars | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 14.70 µs | 0.00 µs | 1.37 µs | 0.00 µs | 0.21 µs | 0.00 µs | log file switch (checkpoint incomplete) (0.00, 10.4 µs); log file switch completion (0.00, 9.9 µs); log file sync (0.01, 1.4 µs); SQL*Net message to client (1.01, 0.2 µs) |
| load-stats | kds-group | - | - | - | 4.51 µs | 0.00 µs | 0.94 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.5 µs); commit.wal_flush (0.01, 0.9 µs); sched.run_queue (1.01, 0.1 µs) |
| load-stats | pg | - | 0.00 µs | 0.00 µs | 1.38 µs | 0.00 µs | 15.20 µs | 0.00 µs | 2.36 µs | 0.00 µs | WalInitSync (0.01, 11.3 µs); CPU (0.01, 6.2 µs); WalSync (0.00, 3.8 µs); ClientRead (0.00, 2.4 µs) |
| load-stats | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.34 µs | 0.00 µs | 1.53 µs | 0.00 µs | 0.20 µs | 0.00 µs | log file sync (0.01, 1.5 µs); SQL*Net message to client (1.01, 0.2 µs); log file switch (checkpoint incomplete) (0.00, 0.0 µs); log file switch completion (0.00, 0.0 µs) |
| load-models | kds-group | - | - | - | 3.38 µs | 0.00 µs | 121.88 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 121.9 µs); cpu.on_cpu (0.00, 3.2 µs); sched.run_queue (1.00, 0.1 µs) |
| load-models | pg | - | 0.00 µs | 0.00 µs | 1.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| load-models | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 22.38 µs | 0.00 µs | 205.75 µs | 0.00 µs | 1.00 µs | 0.00 µs | log file sync (1.00, 205.8 µs); SQL*Net message to client (1.00, 1.0 µs) |
| backtest-read | kds-group | - | - | 12.82 µs | 112.78 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.78 µs | cpu.on_cpu (0.00, 125.1 µs); sched.run_queue (3.00, 0.6 µs) |
| backtest-read | pg | - | 0.00 µs | 0.00 µs | 204.15 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.81 µs | CPU (0.20, 201.0 µs) |
| backtest-read | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 88.30 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.50 µs | 10.01 µs | SQL*Net message to client (1.00, 0.5 µs) |
| result-insert | kds-group | - | - | 0.00 µs | 5.82 µs | 0.00 µs | 123.68 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 123.7 µs); cpu.on_cpu (0.00, 5.7 µs); sched.run_queue (1.00, 0.1 µs) |
| result-insert | pg | - | 0.00 µs | 0.00 µs | 3.40 µs | 0.00 µs | 286.17 µs | 0.00 µs | 9.95 µs | 0.00 µs | WalSync (0.27, 286.2 µs); CPU (0.01, 11.4 µs); ClientRead (0.01, 10.0 µs) |
| result-insert | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 12.75 µs | 0.00 µs | 161.11 µs | 0.00 µs | 0.63 µs | 0.00 µs | log file sync (1.00, 161.1 µs); SQL*Net message to client (1.00, 0.6 µs) |
| backtest-replay | kds-group | - | - | 12.57 µs | 110.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.14 µs | cpu.on_cpu (0.00, 122.4 µs); sched.run_queue (3.00, 0.6 µs) |
| backtest-replay | pg | - | 0.00 µs | 0.00 µs | 206.97 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.52 µs | CPU (0.18, 188.3 µs) |
| backtest-replay | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 87.43 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.58 µs | 26.72 µs | SQL*Net message to client (1.00, 0.6 µs) |
| compare-all | kds-group | - | - | 14.00 µs | 169.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 171.56 µs | cpu.on_cpu (0.00, 183.0 µs); sched.run_queue (1.00, 0.0 µs) |
| compare-all | pg | - | 0.00 µs | 0.00 µs | 98.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 131.05 µs | none |
| compare-all | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 153.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 17.00 µs | 229.01 µs | SQL*Net message to client (1.00, 17.0 µs) |
| compare-one | kds-group | - | - | 3.50 µs | 13.62 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 14.15 µs | cpu.on_cpu (0.00, 17.1 µs); sched.run_queue (1.00, 0.1 µs) |
| compare-one | pg | - | 0.00 µs | 0.00 µs | 17.09 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.99 µs | none |
| compare-one | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 16.09 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.59 µs | 19.68 µs | SQL*Net message to client (1.00, 0.6 µs) |
| read-bar-lookup | kds-group | - | - | 0.91 µs | 1.70 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.30 µs | cpu.on_cpu (0.00, 2.6 µs); sched.run_queue (1.00, 0.1 µs) |
| read-bar-lookup | pg | - | 0.00 µs | 0.00 µs | 1.97 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.49 µs | 0.10 µs | ClientRead (0.01, 4.5 µs) |
| read-bar-lookup | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.51 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 1.54 µs | SQL*Net message to client (1.00, 0.2 µs) |
| read-bar-range | kds-group | - | - | 1.04 µs | 34.78 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 19.62 µs | cpu.on_cpu (0.00, 35.7 µs); sched.run_queue (1.00, 0.1 µs) |
| read-bar-range | pg | - | 0.00 µs | 0.00 µs | 42.73 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.27 µs | 7.80 µs | CPU (0.01, 15.9 µs); ClientRead (0.01, 5.3 µs) |
| read-bar-range | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 37.18 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.64 µs | 106.28 µs | SQL*Net message to client (1.00, 5.6 µs) |
| read-symbol-history | kds-group | - | - | 1.50 µs | 349.30 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 117.73 µs | cpu.on_cpu (0.00, 350.2 µs); sched.run_queue (3.00, 0.6 µs) |
| read-symbol-history | pg | - | 0.00 µs | 0.00 µs | 501.70 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 73.70 µs | CPU (0.40, 363.0 µs) |
| read-symbol-history | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 347.90 µs | 0.00 µs | 0.00 µs | 0.00 µs | 34.50 µs | 937.02 µs | SQL*Net message to client (1.00, 17.4 µs); SQL*Net more data to client (6.00, 17.1 µs) |
| read-day-slice | kds-group | - | - | 1.11 µs | 103.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.54 µs | cpu.on_cpu (0.00, 104.4 µs); sched.run_queue (3.00, 0.5 µs) |
| read-day-slice | pg | - | 0.00 µs | 0.00 µs | 202.34 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.35 µs | 1.01 µs | CPU (0.20, 200.8 µs); ClientRead (0.01, 5.3 µs) |
| read-day-slice | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 64.39 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.29 µs | 6.80 µs | SQL*Net message to client (1.00, 0.3 µs) |
| read-join-point | kds-group | - | - | 1.94 µs | 2.54 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.28 µs | cpu.on_cpu (0.00, 4.4 µs); sched.run_queue (1.00, 0.1 µs) |
| read-join-point | pg | - | 0.00 µs | 0.00 µs | 1.98 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.69 µs | 0.31 µs | ClientRead (0.01, 5.7 µs) |
| read-join-point | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.94 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.27 µs | 0.91 µs | SQL*Net message to client (1.00, 0.3 µs) |
| read-join-exists | kds-group | - | - | 1.90 µs | 319.70 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.20 µs | cpu.on_cpu (0.00, 322.6 µs); sched.run_queue (1.00, 0.1 µs) |
| read-join-exists | pg | - | 0.00 µs | 0.00 µs | 664.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.11 µs | CPU (0.70, 649.9 µs) |
| read-join-exists | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 119.90 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.30 µs | 2.08 µs | events in waitclass Other (3.00, 4.8 µs); SQL*Net message to client (1.00, 0.3 µs) |
| agg-global | kds-group | - | - | 2.20 µs | 286.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.84 µs | cpu.on_cpu (0.00, 287.9 µs); sched.run_queue (2.00, 0.5 µs) |
| agg-global | pg | - | 0.00 µs | 0.00 µs | 311.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.20 µs | CPU (0.20, 139.2 µs) |
| agg-global | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 214.90 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.30 µs | 4.56 µs | SQL*Net message to client (1.00, 0.3 µs) |
| agg-by-symbol | kds-group | - | - | 1.30 µs | 566.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.48 µs | cpu.on_cpu (0.00, 567.2 µs); sched.run_queue (2.00, 0.3 µs) |
| agg-by-symbol | pg | - | 0.00 µs | 0.00 µs | 530.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.62 µs | CPU (0.30, 247.1 µs) |
| agg-by-symbol | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 273.30 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.90 µs | 5.32 µs | SQL*Net message to client (1.00, 0.9 µs) |
| agg-by-session | kds-group | - | - | 2.40 µs | 714.30 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 84.88 µs | cpu.on_cpu (0.00, 716.8 µs); sched.run_queue (2.00, 0.6 µs) |
| agg-by-session | pg | - | 0.00 µs | 0.00 µs | 806.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 31.40 µs | 38.83 µs | CPU (0.80, 828.2 µs); ClientRead (0.10, 31.4 µs) |
| agg-by-session | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 486.90 µs | 0.00 µs | 0.00 µs | 0.00 µs | 16.40 µs | 220.96 µs | events in waitclass Other (6.00, 15.7 µs); SQL*Net message to client (1.00, 13.3 µs); SQL*Net more data to client (1.00, 3.1 µs) |
| agg-day-slice | kds-group | - | - | 1.30 µs | 103.14 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.86 µs | cpu.on_cpu (0.00, 103.9 µs); sched.run_queue (3.00, 0.5 µs) |
| agg-day-slice | pg | - | 0.00 µs | 0.00 µs | 195.38 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.41 µs | CPU (0.18, 186.1 µs) |
| agg-day-slice | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 71.82 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.27 µs | 3.09 µs | SQL*Net message to client (1.00, 0.3 µs) |
| agg-distinct | kds-group | - | - | 1.20 µs | 446.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.57 µs | cpu.on_cpu (0.00, 446.7 µs); sched.run_queue (2.00, 0.4 µs) |
| agg-distinct | pg | - | 0.00 µs | 0.00 µs | 336.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.36 µs | CPU (0.30, 235.1 µs) |
| agg-distinct | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 179.40 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.50 µs | 3.38 µs | SQL*Net message to client (1.00, 1.5 µs) |
| qps-bar-lookup-cold | kds-group | - | - | 0.91 µs | 1.71 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.61 µs | cpu.on_cpu (0.00, 2.6 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-lookup-cold | pg | - | 0.00 µs | 0.00 µs | 1.79 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.42 µs | none |
| qps-bar-lookup-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.82 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.26 µs | 2.65 µs | SQL*Net message to client (1.00, 0.3 µs) |
| qps-bar-lookup-warm | kds-group | - | - | 0.91 µs | 1.60 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.41 µs | cpu.on_cpu (0.00, 2.5 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-lookup-warm | pg | - | 0.00 µs | 0.00 µs | 1.16 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.18 µs | none |
| qps-bar-lookup-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.54 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.29 µs | 2.58 µs | SQL*Net message to client (1.00, 0.3 µs) |
| qps-bar-range-cold | kds-group | - | - | 1.06 µs | 35.64 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 31.17 µs | cpu.on_cpu (0.00, 36.6 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-range-cold | pg | - | 0.00 µs | 0.00 µs | 45.12 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.53 µs | 8.91 µs | CPU (0.04, 32.3 µs); ClientRead (0.01, 10.5 µs) |
| qps-bar-range-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 39.09 µs | 0.00 µs | 0.00 µs | 0.00 µs | 6.58 µs | 106.00 µs | SQL*Net message to client (1.00, 6.6 µs) |
| qps-bar-range-warm | kds-group | - | - | 1.03 µs | 32.86 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 15.31 µs | cpu.on_cpu (0.00, 33.8 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-range-warm | pg | - | 0.00 µs | 0.00 µs | 48.67 µs | 0.00 µs | 0.00 µs | 0.00 µs | 32.64 µs | 8.12 µs | ClientRead (0.03, 32.6 µs); CPU (0.02, 21.1 µs) |
| qps-bar-range-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 39.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 6.55 µs | 108.05 µs | SQL*Net message to client (1.00, 6.5 µs) |
| qps-day-slice-cold | kds-group | - | - | 1.14 µs | 105.04 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.09 µs | cpu.on_cpu (0.00, 105.7 µs); sched.run_queue (3.00, 0.5 µs) |
| qps-day-slice-cold | pg | - | 0.00 µs | 0.00 µs | 202.38 µs | 0.00 µs | 0.00 µs | 0.00 µs | 11.04 µs | 0.52 µs | CPU (0.17, 171.1 µs); ClientRead (0.01, 11.0 µs) |
| qps-day-slice-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 66.47 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.30 µs | 7.16 µs | SQL*Net message to client (1.00, 0.3 µs) |
| qps-day-slice-warm | kds-group | - | - | 1.15 µs | 105.57 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.35 µs | cpu.on_cpu (0.00, 106.2 µs); sched.run_queue (3.00, 0.5 µs) |
| qps-day-slice-warm | pg | - | 0.00 µs | 0.00 µs | 202.74 µs | 0.00 µs | 0.00 µs | 0.00 µs | 11.93 µs | 1.53 µs | CPU (0.18, 181.0 µs); ClientRead (0.01, 11.9 µs) |
| qps-day-slice-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 64.06 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.25 µs | 7.07 µs | SQL*Net message to client (1.00, 0.2 µs) |
| qps-symbol-history-cold | kds-group | - | - | 5.57 µs | 348.29 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 117.90 µs | cpu.on_cpu (0.00, 351.3 µs); sched.run_queue (3.00, 1.4 µs) |
| qps-symbol-history-cold | pg | - | 0.00 µs | 0.00 µs | 480.86 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 75.00 µs | CPU (0.43, 400.7 µs) |
| qps-symbol-history-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 336.71 µs | 0.00 µs | 0.00 µs | 0.00 µs | 36.71 µs | 962.36 µs | SQL*Net more data to client (6.00, 18.9 µs); SQL*Net message to client (1.00, 17.7 µs) |
| qps-symbol-history-warm | kds-group | - | - | 1.79 µs | 371.25 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 110.60 µs | cpu.on_cpu (0.00, 353.2 µs); sched.run_queue (3.00, 22.4 µs) |
| qps-symbol-history-warm | pg | - | 0.00 µs | 0.00 µs | 479.99 µs | 0.00 µs | 0.00 µs | 0.00 µs | 11.02 µs | 58.32 µs | CPU (0.40, 424.0 µs); ClientRead (0.01, 11.0 µs) |
| qps-symbol-history-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 332.43 µs | 0.00 µs | 0.00 µs | 0.00 µs | 36.19 µs | 913.80 µs | SQL*Net more data to client (6.00, 18.1 µs); SQL*Net message to client (1.00, 17.6 µs) |
| qps-cross-join-cold | kds-group | - | - | 12.81 µs | 112.62 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.73 µs | cpu.on_cpu (0.00, 125.0 µs); sched.run_queue (3.00, 0.6 µs) |
| qps-cross-join-cold | pg | - | 0.00 µs | 0.00 µs | 204.82 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.55 µs | 0.84 µs | CPU (0.18, 184.9 µs); ClientRead (0.01, 10.6 µs) |
| qps-cross-join-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 88.11 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.40 µs | 7.54 µs | SQL*Net message to client (1.00, 0.4 µs) |
| qps-cross-join-warm | kds-group | - | - | 12.78 µs | 111.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.51 µs | cpu.on_cpu (0.00, 124.0 µs); sched.run_queue (3.00, 0.5 µs) |
| qps-cross-join-warm | pg | - | 0.00 µs | 0.00 µs | 204.34 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.07 µs | 1.88 µs | CPU (0.17, 176.4 µs); ClientRead (0.01, 10.1 µs) |
| qps-cross-join-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 87.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.57 µs | 9.88 µs | SQL*Net message to client (1.00, 0.6 µs) |
| qps-point-join-cold | kds-group | - | - | 1.97 µs | 2.61 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.39 µs | cpu.on_cpu (0.00, 4.5 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-point-join-cold | pg | - | 0.00 µs | 0.00 µs | 2.07 µs | 0.00 µs | 0.00 µs | 0.00 µs | 7.85 µs | 0.28 µs | ClientRead (0.01, 7.8 µs) |
| qps-point-join-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.25 µs | 1.75 µs | SQL*Net message to client (1.00, 0.2 µs) |
| qps-point-join-warm | kds-group | - | - | 1.96 µs | 2.53 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.42 µs | cpu.on_cpu (0.00, 4.4 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-point-join-warm | pg | - | 0.00 µs | 0.00 µs | 2.06 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | none |
| qps-point-join-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.44 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 1.79 µs | SQL*Net message to client (1.00, 0.2 µs) |
| qps-model-join-cold | kds-group | - | - | 4.00 µs | 14.43 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 18.44 µs | cpu.on_cpu (0.00, 18.4 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-model-join-cold | pg | - | 0.00 µs | 0.00 µs | 17.86 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.78 µs | none |
| qps-model-join-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 33.57 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.43 µs | 46.78 µs | SQL*Net message to client (1.00, 1.4 µs) |
| qps-model-join-warm | kds-group | - | - | 3.46 µs | 13.71 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 13.27 µs | cpu.on_cpu (0.00, 17.1 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-model-join-warm | pg | - | 0.00 µs | 0.00 µs | 18.05 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.00 µs | 5.41 µs | CPU (0.02, 21.4 µs); ClientRead (0.01, 3.0 µs) |
| qps-model-join-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 10.93 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.34 µs | 23.07 µs | SQL*Net message to client (1.00, 0.3 µs) |
| write-batch-1 | kds-group | - | - | 0.00 µs | 5.15 µs | 0.00 µs | 123.66 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 123.7 µs); cpu.on_cpu (0.00, 5.1 µs); sched.run_queue (1.00, 0.1 µs) |
| write-batch-1 | pg | - | 0.00 µs | 0.00 µs | 3.32 µs | 0.00 µs | 291.68 µs | 0.00 µs | 12.22 µs | 0.00 µs | WalSync (0.28, 291.7 µs); ClientRead (0.01, 12.2 µs); CPU (0.01, 6.8 µs) |
| write-batch-1 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 18.76 µs | 0.00 µs | 175.81 µs | 0.00 µs | 0.76 µs | 0.00 µs | log file sync (1.00, 175.8 µs); log file switch completion (0.00, 1.9 µs); SQL*Net message to client (1.00, 0.8 µs); Data file init write (0.00, 0.0 µs) |
| write-batch-10 | kds-group | - | - | 0.00 µs | 4.43 µs | 0.00 µs | 11.95 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (0.10, 11.9 µs); cpu.on_cpu (0.00, 4.4 µs); sched.run_queue (1.20, 0.1 µs) |
| write-batch-10 | pg | - | 0.00 µs | 0.00 µs | 1.24 µs | 0.00 µs | 29.38 µs | 0.00 µs | 2.60 µs | 0.00 µs | WalSync (0.03, 29.4 µs); CPU (0.00, 3.8 µs); ClientRead (0.00, 2.6 µs) |
| write-batch-10 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.37 µs | 0.00 µs | 13.71 µs | 0.00 µs | 0.24 µs | 0.00 µs | log file sync (0.10, 13.7 µs); SQL*Net message to client (1.10, 0.2 µs) |
| write-batch-100 | kds-group | - | - | 0.00 µs | 4.20 µs | 0.00 µs | 1.64 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.1 µs); commit.wal_flush (0.01, 1.6 µs); sched.run_queue (1.02, 0.1 µs) |
| write-batch-100 | pg | - | 0.00 µs | 0.00 µs | 1.29 µs | 0.00 µs | 7.95 µs | 0.00 µs | 5.40 µs | 0.00 µs | WalSync (0.01, 8.0 µs); ClientRead (0.01, 5.4 µs) |
| write-batch-100 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.72 µs | 0.00 µs | 3.22 µs | 0.00 µs | 0.21 µs | 0.00 µs | log file sync (0.01, 3.2 µs); SQL*Net message to client (1.01, 0.2 µs) |
| write-batch-1000 | kds-group | - | - | 0.00 µs | 4.42 µs | 0.00 µs | 0.19 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.4 µs); commit.wal_flush (0.00, 0.2 µs); sched.run_queue (1.00, 0.1 µs) |
| write-batch-1000 | pg | - | 0.00 µs | 0.00 µs | 1.02 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.82 µs | 0.00 µs | ClientRead (0.00, 2.8 µs) |
| write-batch-1000 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.50 µs | 0.00 µs | 0.76 µs | 0.00 µs | 0.19 µs | 0.00 µs | log file sync (0.00, 0.8 µs); SQL*Net message to client (1.00, 0.2 µs) |
| join-conn-1 | kds-group | - | - | 12.88 µs | 112.08 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.84 µs | cpu.on_cpu (0.00, 124.4 µs); sched.run_queue (3.00, 0.5 µs) |
| join-conn-1 | pg | - | 0.00 µs | 0.00 µs | 204.04 µs | 0.00 µs | 0.00 µs | 0.00 µs | 16.27 µs | 1.07 µs | CPU (0.18, 183.7 µs); ClientRead (0.01, 16.3 µs) |
| join-conn-1 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 81.86 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.34 µs | 8.35 µs | SQL*Net message to client (1.00, 0.3 µs) |
| join-conn-2 | kds-group | - | - | 13.65 µs | 181.40 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.17 µs | cpu.on_cpu (0.00, 126.6 µs); sched.run_queue (3.00, 68.5 µs) |
| join-conn-2 | pg | - | 0.00 µs | 0.00 µs | 212.56 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.37 µs | CPU (0.19, 202.0 µs) |
| join-conn-2 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 94.05 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.56 µs | 13.35 µs | SQL*Net message to client (1.00, 0.6 µs) |
| join-conn-4 | kds-group | - | - | 13.02 µs | 338.96 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.95 µs | sched.run_queue (3.00, 228.4 µs); cpu.on_cpu (0.00, 123.6 µs) |
| join-conn-4 | pg | - | 0.00 µs | 0.00 µs | 223.97 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.60 µs | 2.76 µs | CPU (0.16, 165.3 µs); ClientRead (0.01, 5.6 µs) |
| join-conn-4 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 105.75 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.69 µs | 8.09 µs | SQL*Net message to client (1.00, 0.7 µs) |
| join-conn-8 | kds-group | - | - | 13.12 µs | 699.03 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.83 µs | sched.run_queue (3.00, 587.8 µs); cpu.on_cpu (0.00, 124.3 µs) |
| join-conn-8 | pg | - | 0.00 µs | 0.00 µs | 232.79 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.44 µs | 3.64 µs | CPU (0.18, 175.1 µs); ClientRead (0.01, 5.4 µs) |
| join-conn-8 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 114.91 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.31 µs | 8.01 µs | resmgr:cpu quantum (0.07, 8.8 µs); SQL*Net message to client (1.00, 1.3 µs) |

### s1 `bars252`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| load-exchanges | kds-group | 1 ops | 357.8 µs (41.4 %) | 357.8 µs | 357.8 µs | 357.8 µs | 357.8 µs | 357.8 µs | 1,414 ops/s | 35.0 µs | 0.00 µs | 39.2 µs | 0 errors |
| load-exchanges | pg | 1 ops | 194.5 µs (44.9 %) | 194.5 µs | 194.5 µs | 194.5 µs | 194.5 µs | 194.5 µs | 3,325 ops/s | 33.2 µs | 0.00 µs | 25.0 µs | 0 errors |
| load-exchanges | oracle | 1 ops | 1,528.6 µs (11.4 %) | 1,528.6 µs | 1,528.6 µs | 1,528.6 µs | 1,528.6 µs | 1,528.6 µs | 461 ops/s | 701.5 µs | 0.00 µs | 256.0 µs | 0 errors |
| load-symbols | kds-group | 1 ops | 255.5 µs (47.8 %) | 255.5 µs | 255.5 µs | 255.5 µs | 255.5 µs | 255.5 µs | 2,573 ops/s | 35.6 µs | 0.00 µs | 23.3 µs | 0 errors |
| load-symbols | pg | 1 ops | 171.8 µs (58.0 %) | 171.8 µs | 171.8 µs | 171.8 µs | 171.8 µs | 171.8 µs | 4,473 ops/s | 28.8 µs | 0.00 µs | 18.0 µs | 0 errors |
| load-symbols | oracle | 1 ops | 974.9 µs (38.0 %) | 974.9 µs | 974.9 µs | 974.9 µs | 974.9 µs | 974.9 µs | 764 ops/s | 450.5 µs | 0.00 µs | 207.0 µs | 0 errors |
| load-sessions | kds-group | 252 ops | 21.7 µs (49.0 %) | 17.0 µs | 19.0 µs | 19.4 µs | 32.8 µs | 37.0 µs | 41,144 ops/s | 7.0 µs | 0.00 µs | 10.1 µs | 0 errors |
| load-sessions | pg | 252 ops | 13.4 µs (14.5 %) | 11.9 µs | 13.1 µs | 13.3 µs | 14.5 µs | 15.9 µs | 65,922 ops/s | 4.7 µs | 0.00 µs | 5.2 µs | 0 errors |
| load-sessions | oracle | 252 ops | 27.6 µs (41.6 %) | 22.1 µs | 22.6 µs | 22.9 µs | 26.2 µs | 66.8 µs | 31,435 ops/s | 12.9 µs | 0.00 µs | 9.6 µs | 0 errors |
| load-bars | kds-group | 252 ops | 18.7 µs (12.8 %) | 13.9 µs | 15.8 µs | 16.8 µs | 20.7 µs | 47.9 µs | 46,646 ops/s | 5.4 µs | 0.00 µs | 7.8 µs | 0 errors |
| load-bars | pg | 252 ops | 17.6 µs (20.2 %) | 12.9 µs | 14.5 µs | 15.6 µs | 19.3 µs | 29.5 µs | 51,239 ops/s | 7.4 µs | 0.00 µs | 6.0 µs | 0 errors |
| load-bars | oracle | 252 ops | 23.5 µs (56.0 %) | 15.8 µs | 17.1 µs | 17.6 µs | 28.2 µs | 165.9 µs | 37,623 ops/s | 7.9 µs | 0.00 µs | 9.4 µs | 0 errors |
| load-stats | kds-group | 252 ops | 18.1 µs (17.2 %) | 15.3 µs | 15.9 µs | 16.4 µs | 25.6 µs | 32.2 µs | 50,187 ops/s | 4.8 µs | 0.00 µs | 8.8 µs | 0 errors |
| load-stats | pg | 252 ops | 18.0 µs (24.6 %) | 14.8 µs | 15.8 µs | 16.6 µs | 19.8 µs | 40.7 µs | 50,754 ops/s | 6.7 µs | 0.00 µs | 6.1 µs | 0 errors |
| load-stats | oracle | 252 ops | 24.0 µs (37.6 %) | 15.6 µs | 17.0 µs | 17.9 µs | 24.7 µs | 182.1 µs | 35,035 ops/s | 7.2 µs | 0.00 µs | 10.1 µs | 0 errors |
| load-models | kds-group | 8 ops | 132.2 µs (1.5 %) | 128.5 µs | 129.4 µs | 130.1 µs | 145.0 µs | 145.0 µs | 6,663 ops/s | 5.8 µs | 0.00 µs | 10.6 µs | 0 errors |
| load-models | pg | 8 ops | 145.7 µs (11.4 %) | 130.3 µs | 131.3 µs | 137.2 µs | 205.9 µs | 205.9 µs | 6,674 ops/s | 9.2 µs | 0.00 µs | 12.1 µs | 0 errors |
| load-models | oracle | 8 ops | 447.5 µs (22.0 %) | 195.6 µs | 325.2 µs | 424.7 µs | 739.6 µs | 739.6 µs | 2,104 ops/s | 39.4 µs | 0.00 µs | 95.9 µs | 0 errors |
| backtest-read | kds-group | 12 ops | 33.9 µs (205.7 %) | 30.2 µs | 31.0 µs | 31.7 µs | 36.8 µs | 57.0 µs | 20,865 ops/s | 6.8 µs | 0.72 µs | 23.3 µs | 0 errors |
| backtest-read | pg | 12 ops | 31.1 µs (8.6 %) | 27.6 µs | 29.4 µs | 29.8 µs | 31.1 µs | 46.4 µs | 29,083 ops/s | 7.4 µs | 0.20 µs | 19.2 µs | 0 errors |
| backtest-read | oracle | 12 ops | 76.0 µs (21.9 %) | 24.8 µs | 25.5 µs | 26.9 µs | 204.3 µs | 429.0 µs | 10,616 ops/s | 27.9 µs | 2.24 µs | 26.5 µs | 0 errors |
| result-insert | kds-group | 88 ops | 145.4 µs (6.6 %) | 128.9 µs | 132.0 µs | 135.7 µs | 234.2 µs | 243.0 µs | 6,703 ops/s | 7.2 µs | 0.00 µs | 10.8 µs | 0 errors |
| result-insert | pg | 88 ops | 138.5 µs (2.7 %) | 127.4 µs | 131.1 µs | 132.4 µs | 159.5 µs | 170.2 µs | 7,190 ops/s | 7.7 µs | 0.00 µs | 9.3 µs | 0 errors |
| result-insert | oracle | 88 ops | 228.0 µs (87.1 %) | 139.4 µs | 151.5 µs | 172.5 µs | 473.7 µs | 549.3 µs | 4,314 ops/s | 12.8 µs | 0.00 µs | 50.0 µs | 0 errors |
| backtest-replay | kds-group | 12 ops | 122.6 µs (47.7 %) | 38.7 µs | 66.7 µs | 105.5 µs | 229.8 µs | 252.0 µs | 7,047 ops/s | 32.3 µs | 1.50 µs | 23.9 µs | 0 errors |
| backtest-replay | pg | 12 ops | 39.8 µs (37.6 %) | 33.8 µs | 34.4 µs | 35.4 µs | 40.1 µs | 75.8 µs | 19,078 ops/s | 15.5 µs | 0.30 µs | 19.3 µs | 0 errors |
| backtest-replay | oracle | 12 ops | 157.3 µs (12.0 %) | 49.4 µs | 50.6 µs | 57.1 µs | 291.1 µs | 991.7 µs | 5,125 ops/s | 92.5 µs | 9.63 µs | 31.4 µs | 0 errors |
| compare-all | kds-group | 1 ops | 278.0 µs (30.2 %) | 278.0 µs | 278.0 µs | 278.0 µs | 278.0 µs | 278.0 µs | 1,856 ops/s | 101.0 µs | 50.51 µs | 48.8 µs | 0 errors |
| compare-all | pg | 1 ops | 281.2 µs (68.6 %) | 281.2 µs | 281.2 µs | 281.2 µs | 281.2 µs | 281.2 µs | 2,268 ops/s | 118.2 µs | 24.40 µs | 26.0 µs | 0 errors |
| compare-all | oracle | 1 ops | 443.5 µs (43.5 %) | 443.5 µs | 443.5 µs | 443.5 µs | 443.5 µs | 443.5 µs | 1,508 ops/s | 236.8 µs | 57.29 µs | 78.0 µs | 0 errors |
| compare-one | kds-group | 32 ops | 30.4 µs (47.1 %) | 28.3 µs | 28.7 µs | 29.0 µs | 32.6 µs | 63.4 µs | 27,908 ops/s | 12.0 µs | 1.80 µs | 12.6 µs | 0 errors |
| compare-one | pg | 32 ops | 39.0 µs (58.4 %) | 30.9 µs | 31.3 µs | 32.0 µs | 46.7 µs | 63.3 µs | 24,128 ops/s | 24.3 µs | 1.34 µs | 9.8 µs | 0 errors |
| compare-one | oracle | 32 ops | 45.4 µs (88.3 %) | 22.0 µs | 22.9 µs | 24.0 µs | 87.6 µs | 294.1 µs | 20,341 ops/s | 15.7 µs | 4.40 µs | 14.5 µs | 0 errors |
| read-bar-lookup | kds-group | 200 ops | 22.3 µs (33.1 %) | 20.2 µs | 20.7 µs | 21.5 µs | 30.9 µs | 36.6 µs | 41,176 ops/s | 9.6 µs | 0.36 µs | 6.9 µs | 0 errors |
| read-bar-lookup | pg | 200 ops | 29.5 µs (44.3 %) | 18.6 µs | 29.6 µs | 29.8 µs | 34.1 µs | 40.3 µs | 33,231 ops/s | 14.3 µs | 0.15 µs | 10.0 µs | 0 errors |
| read-bar-lookup | oracle | 200 ops | 19.8 µs (54.7 %) | 14.3 µs | 15.5 µs | 15.6 µs | 18.1 µs | 142.1 µs | 48,324 ops/s | 7.4 µs | 1.33 µs | 6.3 µs | 0 errors |
| read-bar-range | kds-group | 200 ops | 66.7 µs (19.8 %) | 55.0 µs | 62.9 µs | 63.7 µs | 74.0 µs | 178.3 µs | 14,763 ops/s | 19.1 µs | 11.80 µs | 40.8 µs | 0 errors |
| read-bar-range | pg | 200 ops | 103.8 µs (16.6 %) | 90.4 µs | 93.3 µs | 94.6 µs | 116.0 µs | 270.7 µs | 9,606 ops/s | 77.1 µs | 8.42 µs | 43.0 µs | 0 errors |
| read-bar-range | oracle | 200 ops | 168.7 µs (6.7 %) | 136.2 µs | 138.2 µs | 139.6 µs | 307.9 µs | 427.8 µs | 5,891 ops/s | 118.6 µs | 91.17 µs | 31.0 µs | 0 errors |
| read-symbol-history | kds-group | 10 ops | 174.0 µs (9.0 %) | 91.6 µs | 99.4 µs | 205.9 µs | 251.4 µs | 251.4 µs | 5,291 ops/s | 38.3 µs | 22.54 µs | 58.4 µs | 0 errors |
| read-symbol-history | pg | 10 ops | 218.7 µs (48.8 %) | 116.4 µs | 119.6 µs | 214.4 µs | 348.8 µs | 348.8 µs | 4,422 ops/s | 103.3 µs | 14.84 µs | 81.3 µs | 0 errors |
| read-symbol-history | oracle | 10 ops | 373.7 µs (0.9 %) | 238.4 µs | 249.3 µs | 384.3 µs | 661.8 µs | 661.8 µs | 2,572 ops/s | 229.4 µs | 177.32 µs | 81.6 µs | 0 errors |
| read-day-slice | kds-group | 200 ops | 19.5 µs (22.1 %) | 14.4 µs | 15.2 µs | 15.7 µs | 17.0 µs | 124.8 µs | 48,511 ops/s | 3.0 µs | 0.14 µs | 9.0 µs | 0 errors |
| read-day-slice | pg | 200 ops | 20.9 µs (13.6 %) | 17.3 µs | 17.8 µs | 18.7 µs | 21.2 µs | 47.3 µs | 47,165 ops/s | 5.2 µs | 0.08 µs | 10.1 µs | 0 errors |
| read-day-slice | oracle | 200 ops | 22.3 µs (8.9 %) | 17.9 µs | 18.9 µs | 19.2 µs | 20.7 µs | 30.4 µs | 42,075 ops/s | 7.0 µs | 1.47 µs | 9.9 µs | 0 errors |
| read-join-point | kds-group | 200 ops | 16.8 µs (33.3 %) | 14.3 µs | 15.3 µs | 15.4 µs | 33.0 µs | 33.8 µs | 58,179 ops/s | 4.1 µs | 0.13 µs | 8.7 µs | 0 errors |
| read-join-point | pg | 200 ops | 27.7 µs (25.5 %) | 19.0 µs | 19.9 µs | 20.3 µs | 31.7 µs | 202.0 µs | 35,004 ops/s | 5.5 µs | 0.09 µs | 12.8 µs | 0 errors |
| read-join-point | oracle | 200 ops | 21.1 µs (8.3 %) | 14.8 µs | 16.1 µs | 16.2 µs | 45.8 µs | 172.7 µs | 44,273 ops/s | 7.7 µs | 1.11 µs | 7.2 µs | 0 errors |
| read-join-exists | kds-group | 10 ops | 39.4 µs (97.1 %) | 13.5 µs | 15.2 µs | 15.3 µs | 203.0 µs | 203.0 µs | 18,041 ops/s | 6.9 µs | 0.21 µs | 7.2 µs | 0 errors |
| read-join-exists | pg | 10 ops | 15.9 µs (4.3 %) | 13.5 µs | 14.1 µs | 14.6 µs | 28.7 µs | 28.7 µs | 50,101 ops/s | 6.1 µs | 0.07 µs | 6.2 µs | 0 errors |
| read-join-exists | oracle | 10 ops | 44.6 µs (11.5 %) | 24.0 µs | 24.2 µs | 24.9 µs | 224.0 µs | 224.0 µs | 17,349 ops/s | 24.3 µs | 0.97 µs | 17.7 µs | 0 errors |
| agg-global | kds-group | 10 ops | 22.4 µs (165.6 %) | 20.2 µs | 20.5 µs | 22.0 µs | 32.6 µs | 32.6 µs | 38,212 ops/s | 4.3 µs | 0.13 µs | 14.1 µs | 0 errors |
| agg-global | pg | 10 ops | 25.6 µs (619.6 %) | 21.7 µs | 22.9 µs | 23.3 µs | 46.5 µs | 46.5 µs | 20,642 ops/s | 7.0 µs | 0.10 µs | 14.5 µs | 0 errors |
| agg-global | oracle | 10 ops | 54.6 µs (191.3 %) | 21.1 µs | 22.4 µs | 36.4 µs | 270.7 µs | 270.7 µs | 16,044 ops/s | 27.6 µs | 1.43 µs | 18.1 µs | 0 errors |
| agg-by-symbol | kds-group | 10 ops | 57.2 µs (205.6 %) | 27.6 µs | 28.1 µs | 29.1 µs | 188.9 µs | 188.9 µs | 16,275 ops/s | 5.7 µs | 0.18 µs | 21.4 µs | 0 errors |
| agg-by-symbol | pg | 10 ops | 98.9 µs (133.7 %) | 29.6 µs | 31.0 µs | 43.3 µs | 314.9 µs | 314.9 µs | 8,962 ops/s | 8.7 µs | 0.07 µs | 36.9 µs | 0 errors |
| agg-by-symbol | oracle | 10 ops | 43.3 µs (205.0 %) | 25.8 µs | 26.1 µs | 27.1 µs | 185.4 µs | 185.4 µs | 16,626 ops/s | 20.4 µs | 1.18 µs | 17.9 µs | 0 errors |
| agg-by-session | kds-group | 10 ops | 84.4 µs (47.4 %) | 75.2 µs | 75.6 µs | 79.4 µs | 104.4 µs | 104.4 µs | 9,754 ops/s | 25.7 µs | 13.40 µs | 49.3 µs | 0 errors |
| agg-by-session | pg | 10 ops | 239.8 µs (16.5 %) | 136.4 µs | 159.3 µs | 283.8 µs | 391.3 µs | 391.3 µs | 3,812 ops/s | 88.3 µs | 7.46 µs | 81.5 µs | 0 errors |
| agg-by-session | oracle | 10 ops | 203.9 µs (5.9 %) | 137.9 µs | 139.3 µs | 149.9 µs | 557.1 µs | 557.1 µs | 4,501 ops/s | 82.5 µs | 45.09 µs | 80.5 µs | 0 errors |
| agg-day-slice | kds-group | 200 ops | 18.5 µs (7.7 %) | 17.0 µs | 17.4 µs | 18.5 µs | 19.5 µs | 21.6 µs | 52,295 ops/s | 4.6 µs | 0.12 µs | 9.2 µs | 0 errors |
| agg-day-slice | pg | 200 ops | 21.1 µs (34.9 %) | 19.3 µs | 20.2 µs | 20.6 µs | 21.9 µs | 23.1 µs | 46,801 ops/s | 5.6 µs | 0.06 µs | 11.3 µs | 0 errors |
| agg-day-slice | oracle | 200 ops | 25.6 µs (20.1 %) | 19.7 µs | 20.9 µs | 21.0 µs | 49.6 µs | 151.0 µs | 37,431 ops/s | 7.3 µs | 1.01 µs | 11.9 µs | 0 errors |
| agg-distinct | kds-group | 10 ops | 46.4 µs (172.3 %) | 25.4 µs | 25.7 µs | 27.3 µs | 139.1 µs | 139.1 µs | 18,491 ops/s | 10.4 µs | 0.21 µs | 17.1 µs | 0 errors |
| agg-distinct | pg | 10 ops | 33.0 µs (406.5 %) | 23.0 µs | 23.7 µs | 25.3 µs | 58.0 µs | 58.0 µs | 22,414 ops/s | 10.2 µs | 0.08 µs | 14.8 µs | 0 errors |
| agg-distinct | oracle | 10 ops | 64.1 µs (307.4 %) | 26.1 µs | 49.5 µs | 51.5 µs | 206.9 µs | 206.9 µs | 13,628 ops/s | 33.3 µs | 2.68 µs | 15.8 µs | 0 errors |
| qps-bar-lookup-cold | kds-group | 100 ops | 16.9 µs (6.2 %) | 14.4 µs | 14.9 µs | 15.1 µs | 30.4 µs | 34.2 µs | 56,313 ops/s | 5.6 µs | 0.21 µs | 6.5 µs | 0 errors |
| qps-bar-lookup-cold | pg | 100 ops | 18.6 µs (19.9 %) | 17.0 µs | 18.0 µs | 18.2 µs | 19.7 µs | 21.4 µs | 52,109 ops/s | 5.1 µs | 0.06 µs | 9.5 µs | 0 errors |
| qps-bar-lookup-cold | oracle | 100 ops | 23.2 µs (28.2 %) | 14.5 µs | 15.6 µs | 15.8 µs | 36.6 µs | 58.4 µs | 39,394 ops/s | 8.3 µs | 1.43 µs | 6.0 µs | 0 errors |
| qps-bar-lookup-warm | kds-group | 100 ops | 14.8 µs (11.5 %) | 13.3 µs | 13.8 µs | 14.9 µs | 16.3 µs | 18.0 µs | 63,664 ops/s | 4.4 µs | 0.18 µs | 6.3 µs | 0 errors |
| qps-bar-lookup-warm | pg | 100 ops | 20.4 µs (13.7 %) | 19.4 µs | 19.7 µs | 19.9 µs | 21.4 µs | 22.1 µs | 46,208 ops/s | 7.0 µs | 0.08 µs | 9.6 µs | 0 errors |
| qps-bar-lookup-warm | oracle | 100 ops | 21.1 µs (26.6 %) | 14.7 µs | 15.9 µs | 16.0 µs | 22.2 µs | 174.7 µs | 43,379 ops/s | 7.7 µs | 1.30 µs | 6.7 µs | 0 errors |
| qps-bar-range-cold | kds-group | 51 ops | 86.7 µs (29.0 %) | 68.9 µs | 74.3 µs | 75.9 µs | 93.0 µs | 233.4 µs | 11,241 ops/s | 35.0 µs | 21.33 µs | 41.7 µs | 0 errors |
| qps-bar-range-cold | pg | 51 ops | 120.2 µs (55.7 %) | 91.3 µs | 93.8 µs | 133.4 µs | 139.8 µs | 195.5 µs | 8,144 ops/s | 96.4 µs | 10.21 µs | 42.0 µs | 0 errors |
| qps-bar-range-cold | oracle | 51 ops | 170.4 µs (34.9 %) | 137.0 µs | 138.6 µs | 140.8 µs | 337.0 µs | 630.9 µs | 5,817 ops/s | 125.0 µs | 94.33 µs | 34.2 µs | 0 errors |
| qps-bar-range-warm | kds-group | 100 ops | 79.3 µs (13.9 %) | 65.7 µs | 67.6 µs | 68.8 µs | 92.0 µs | 94.6 µs | 12,253 ops/s | 28.4 µs | 17.97 µs | 41.0 µs | 0 errors |
| qps-bar-range-warm | pg | 100 ops | 104.6 µs (9.7 %) | 90.2 µs | 92.3 µs | 93.0 µs | 113.0 µs | 285.4 µs | 9,514 ops/s | 73.5 µs | 7.74 µs | 43.0 µs | 0 errors |
| qps-bar-range-warm | oracle | 100 ops | 192.7 µs (20.2 %) | 136.7 µs | 139.0 µs | 140.5 µs | 400.7 µs | 525.4 µs | 5,108 ops/s | 121.1 µs | 93.20 µs | 36.3 µs | 0 errors |
| qps-day-slice-cold | kds-group | 100 ops | 19.6 µs (27.3 %) | 15.5 µs | 15.7 µs | 16.8 µs | 17.4 µs | 139.6 µs | 48,430 ops/s | 3.7 µs | 0.18 µs | 9.1 µs | 0 errors |
| qps-day-slice-cold | pg | 100 ops | 19.2 µs (32.1 %) | 17.5 µs | 18.7 µs | 18.9 µs | 20.5 µs | 21.6 µs | 48,954 ops/s | 5.0 µs | 0.07 µs | 10.1 µs | 0 errors |
| qps-day-slice-cold | oracle | 100 ops | 25.2 µs (29.1 %) | 17.8 µs | 19.1 µs | 19.3 µs | 55.8 µs | 201.1 µs | 37,218 ops/s | 8.0 µs | 1.61 µs | 11.1 µs | 0 errors |
| qps-day-slice-warm | kds-group | 100 ops | 22.2 µs (35.1 %) | 14.6 µs | 15.5 µs | 15.8 µs | 24.3 µs | 153.0 µs | 40,651 ops/s | 4.1 µs | 0.19 µs | 9.0 µs | 0 errors |
| qps-day-slice-warm | pg | 100 ops | 19.0 µs (36.3 %) | 17.4 µs | 17.9 µs | 18.8 µs | 20.3 µs | 22.2 µs | 42,507 ops/s | 5.1 µs | 0.06 µs | 10.0 µs | 0 errors |
| qps-day-slice-warm | oracle | 100 ops | 20.3 µs (17.2 %) | 17.9 µs | 18.8 µs | 18.9 µs | 19.8 µs | 23.0 µs | 45,322 ops/s | 7.4 µs | 1.41 µs | 9.0 µs | 0 errors |
| qps-symbol-history-cold | kds-group | 0 ops | 0.0 µs (-) | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0 ops/s | 0.0 µs | 0.00 µs | 0.0 µs | 0 errors |
| qps-symbol-history-cold | pg | 0 ops | 0.0 µs (-) | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0 ops/s | 0.0 µs | 0.00 µs | 0.0 µs | 0 errors |
| qps-symbol-history-cold | oracle | 0 ops | 0.0 µs (-) | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0 ops/s | 0.0 µs | 0.00 µs | 0.0 µs | 0 errors |
| qps-symbol-history-warm | kds-group | 100 ops | 108.8 µs (14.3 %) | 86.4 µs | 97.8 µs | 104.9 µs | 125.6 µs | 226.9 µs | 9,148 ops/s | 41.7 µs | 29.59 µs | 59.6 µs | 0 errors |
| qps-symbol-history-warm | pg | 100 ops | 143.2 µs (21.9 %) | 109.6 µs | 112.5 µs | 114.3 µs | 333.0 µs | 382.8 µs | 6,896 ops/s | 94.5 µs | 11.74 µs | 62.0 µs | 0 errors |
| qps-symbol-history-warm | oracle | 100 ops | 330.3 µs (9.5 %) | 236.2 µs | 243.9 µs | 362.3 µs | 488.1 µs | 564.4 µs | 3,015 ops/s | 214.2 µs | 172.92 µs | 68.8 µs | 0 errors |
| qps-cross-join-cold | kds-group | 100 ops | 32.9 µs (14.1 %) | 27.5 µs | 28.1 µs | 28.9 µs | 46.9 µs | 48.0 µs | 29,984 ops/s | 5.9 µs | 0.27 µs | 22.4 µs | 0 errors |
| qps-cross-join-cold | pg | 100 ops | 28.0 µs (10.3 %) | 26.0 µs | 26.9 µs | 27.4 µs | 28.5 µs | 29.6 µs | 34,314 ops/s | 5.0 µs | 0.11 µs | 18.8 µs | 0 errors |
| qps-cross-join-cold | oracle | 100 ops | 31.8 µs (38.3 %) | 23.1 µs | 24.1 µs | 24.5 µs | 26.7 µs | 267.3 µs | 29,671 ops/s | 8.0 µs | 1.57 µs | 16.0 µs | 0 errors |
| qps-cross-join-warm | kds-group | 100 ops | 33.9 µs (27.4 %) | 27.9 µs | 28.7 µs | 28.8 µs | 30.2 µs | 161.9 µs | 27,278 ops/s | 3.1 µs | 0.15 µs | 22.0 µs | 0 errors |
| qps-cross-join-warm | pg | 100 ops | 28.8 µs (18.2 %) | 26.2 µs | 26.9 µs | 27.4 µs | 40.2 µs | 54.8 µs | 33,878 ops/s | 6.1 µs | 0.13 µs | 18.6 µs | 0 errors |
| qps-cross-join-warm | oracle | 100 ops | 27.6 µs (29.1 %) | 22.8 µs | 23.9 µs | 24.3 µs | 26.7 µs | 37.8 µs | 34,151 ops/s | 7.7 µs | 1.41 µs | 14.9 µs | 0 errors |
| qps-point-join-cold | kds-group | 100 ops | 16.6 µs (8.4 %) | 15.3 µs | 15.7 µs | 16.3 µs | 17.4 µs | 19.7 µs | 58,741 ops/s | 3.6 µs | 0.13 µs | 8.7 µs | 0 errors |
| qps-point-join-cold | pg | 100 ops | 26.7 µs (28.8 %) | 19.2 µs | 20.2 µs | 20.3 µs | 35.0 µs | 160.6 µs | 36,753 ops/s | 5.4 µs | 0.09 µs | 11.6 µs | 0 errors |
| qps-point-join-cold | oracle | 100 ops | 18.7 µs (11.1 %) | 15.0 µs | 16.0 µs | 16.2 µs | 19.0 µs | 57.0 µs | 50,513 ops/s | 7.3 µs | 0.90 µs | 7.1 µs | 0 errors |
| qps-point-join-warm | kds-group | 100 ops | 16.4 µs (8.5 %) | 15.2 µs | 15.4 µs | 15.8 µs | 17.3 µs | 23.2 µs | 59,029 ops/s | 3.8 µs | 0.13 µs | 8.6 µs | 0 errors |
| qps-point-join-warm | pg | 100 ops | 29.9 µs (56.6 %) | 19.0 µs | 20.1 µs | 20.3 µs | 72.3 µs | 222.1 µs | 31,413 ops/s | 5.3 µs | 0.09 µs | 12.7 µs | 0 errors |
| qps-point-join-warm | oracle | 100 ops | 29.2 µs (60.0 %) | 14.9 µs | 16.1 µs | 16.3 µs | 153.2 µs | 311.5 µs | 31,986 ops/s | 8.4 µs | 1.15 µs | 8.5 µs | 0 errors |
| qps-model-join-cold | kds-group | 7 ops | 124.5 µs (80.1 %) | 19.6 µs | 141.4 µs | 142.6 µs | 199.8 µs | 199.8 µs | 6,940 ops/s | 12.8 µs | 1.34 µs | 13.0 µs | 0 errors |
| qps-model-join-cold | pg | 7 ops | 28.9 µs (138.8 %) | 20.9 µs | 21.3 µs | 21.4 µs | 75.5 µs | 75.5 µs | 18,349 ops/s | 14.3 µs | 0.61 µs | 9.6 µs | 0 errors |
| qps-model-join-cold | oracle | 7 ops | 157.9 µs (138.7 %) | 41.6 µs | 51.5 µs | 213.8 µs | 372.5 µs | 372.5 µs | 5,335 ops/s | 39.5 µs | 5.07 µs | 38.0 µs | 0 errors |
| qps-model-join-warm | kds-group | 100 ops | 25.2 µs (51.3 %) | 18.6 µs | 19.0 µs | 20.1 µs | 39.8 µs | 149.3 µs | 37,406 ops/s | 5.9 µs | 0.74 µs | 11.9 µs | 0 errors |
| qps-model-join-warm | pg | 100 ops | 24.2 µs (60.7 %) | 19.7 µs | 20.4 µs | 20.6 µs | 55.1 µs | 61.4 µs | 40,689 ops/s | 10.6 µs | 0.57 µs | 9.1 µs | 0 errors |
| qps-model-join-warm | oracle | 100 ops | 44.9 µs (100.4 %) | 24.7 µs | 25.6 µs | 26.4 µs | 233.1 µs | 270.4 µs | 21,422 ops/s | 16.7 µs | 6.20 µs | 13.6 µs | 0 errors |
| write-batch-1 | kds-group | 2,000 ops | 148.9 µs (3.2 %) | 124.0 µs | 136.6 µs | 138.1 µs | 172.4 µs | 307.7 µs | 6,707 ops/s | 9.5 µs | 0.00 µs | 12.5 µs | 0 errors |
| write-batch-1 | pg | 2,000 ops | 154.3 µs (9.7 %) | 126.0 µs | 136.8 µs | 142.5 µs | 192.5 µs | 392.2 µs | 6,479 ops/s | 13.4 µs | 0.00 µs | 12.2 µs | 0 errors |
| write-batch-1 | oracle | 2,000 ops | 315.6 µs (13.2 %) | 141.9 µs | 185.1 µs | 294.0 µs | 555.7 µs | 738.0 µs | 3,164 ops/s | 22.7 µs | 0.00 µs | 65.8 µs | 0 errors |
| write-batch-10 | kds-group | 2,000 ops | 30.8 µs (3.2 %) | 12.7 µs | 14.0 µs | 14.7 µs | 140.5 µs | 166.6 µs | 32,336 ops/s | 4.9 µs | 0.00 µs | 9.2 µs | 0 errors |
| write-batch-10 | pg | 2,000 ops | 31.2 µs (3.6 %) | 11.9 µs | 13.1 µs | 13.4 µs | 163.7 µs | 173.8 µs | 31,968 ops/s | 6.7 µs | 0.00 µs | 6.5 µs | 0 errors |
| write-batch-10 | oracle | 2,000 ops | 34.8 µs (190.0 %) | 13.4 µs | 14.7 µs | 15.0 µs | 168.0 µs | 210.8 µs | 28,569 ops/s | 6.8 µs | 0.00 µs | 10.2 µs | 0 errors |
| write-batch-100 | kds-group | 2,000 ops | 16.7 µs (2.4 %) | 12.8 µs | 14.0 µs | 14.3 µs | 15.7 µs | 163.0 µs | 59,355 ops/s | 2.6 µs | 0.00 µs | 8.0 µs | 0 errors |
| write-batch-100 | pg | 2,000 ops | 15.4 µs (7.4 %) | 11.9 µs | 13.1 µs | 13.2 µs | 14.2 µs | 143.2 µs | 64,397 ops/s | 4.5 µs | 0.00 µs | 5.2 µs | 0 errors |
| write-batch-100 | oracle | 2,000 ops | 18.5 µs (22.4 %) | 13.3 µs | 14.8 µs | 15.4 µs | 18.8 µs | 242.4 µs | 53,489 ops/s | 4.5 µs | 0.00 µs | 8.1 µs | 0 errors |
| write-batch-1000 | kds-group | 2,000 ops | 15.0 µs (2.9 %) | 12.8 µs | 13.8 µs | 14.4 µs | 15.7 µs | 22.7 µs | 65,884 ops/s | 2.8 µs | 0.00 µs | 8.2 µs | 0 errors |
| write-batch-1000 | pg | 2,000 ops | 13.8 µs (6.8 %) | 11.9 µs | 13.1 µs | 13.3 µs | 14.0 µs | 30.4 µs | 72,099 ops/s | 4.5 µs | 0.00 µs | 5.0 µs | 0 errors |
| write-batch-1000 | oracle | 2,000 ops | 17.3 µs (7.4 %) | 13.8 µs | 15.2 µs | 15.4 µs | 16.5 µs | 25.6 µs | 57,199 ops/s | 4.4 µs | 0.00 µs | 8.0 µs | 0 errors |
| join-conn-1 | kds-group | 200 ops | 32.7 µs (20.1 %) | 27.3 µs | 27.7 µs | 28.3 µs | 47.0 µs | 175.9 µs | 29,479 ops/s | 3.8 µs | 0.16 µs | 22.1 µs | 0 errors |
| join-conn-1 | pg | 200 ops | 32.6 µs (31.9 %) | 26.1 µs | 27.1 µs | 27.5 µs | 36.9 µs | 180.4 µs | 30,105 ops/s | 5.3 µs | 0.11 µs | 19.9 µs | 0 errors |
| join-conn-1 | oracle | 200 ops | 27.4 µs (11.2 %) | 22.4 µs | 23.6 µs | 24.0 µs | 27.6 µs | 57.2 µs | 34,550 ops/s | 7.9 µs | 1.56 µs | 14.9 µs | 0 errors |
| join-conn-2 | kds-group | 200 ops | 51.5 µs (14.4 %) | 27.4 µs | 43.3 µs | 43.8 µs | 135.4 µs | 220.9 µs | 37,190 ops/s | 5.0 µs | 0.22 µs | 20.8 µs | 0 errors |
| join-conn-2 | pg | 200 ops | 35.7 µs (25.2 %) | 26.1 µs | 27.6 µs | 27.8 µs | 68.8 µs | 181.0 µs | 43,451 ops/s | 6.0 µs | 0.14 µs | 19.8 µs | 0 errors |
| join-conn-2 | oracle | 200 ops | 45.2 µs (59.3 %) | 26.7 µs | 28.7 µs | 29.5 µs | 41.4 µs | 431.9 µs | 39,873 ops/s | 15.1 µs | 2.57 µs | 18.0 µs | 0 errors |
| join-conn-4 | kds-group | 200 ops | 74.2 µs (2.7 %) | 28.4 µs | 43.6 µs | 45.3 µs | 182.7 µs | 257.7 µs | 42,146 ops/s | 4.5 µs | 0.20 µs | 20.8 µs | 0 errors |
| join-conn-4 | pg | 200 ops | 51.2 µs (26.8 %) | 28.7 µs | 29.8 µs | 30.4 µs | 213.0 µs | 310.5 µs | 57,194 ops/s | 9.4 µs | 0.23 µs | 23.0 µs | 0 errors |
| join-conn-4 | oracle | 200 ops | 57.9 µs (11.2 %) | 22.6 µs | 24.4 µs | 25.1 µs | 227.1 µs | 918.4 µs | 52,109 ops/s | 12.3 µs | 1.75 µs | 19.4 µs | 0 errors |
| join-conn-8 | kds-group | 200 ops | 150.8 µs (3.2 %) | 30.7 µs | 108.0 µs | 134.1 µs | 302.4 µs | 357.3 µs | 43,369 ops/s | 7.3 µs | 0.32 µs | 20.9 µs | 0 errors |
| join-conn-8 | pg | 200 ops | 155.9 µs (39.4 %) | 41.4 µs | 43.2 µs | 51.6 µs | 423.7 µs | 541.2 µs | 30,441 ops/s | 30.9 µs | 0.71 µs | 38.2 µs | 0 errors |
| join-conn-8 | oracle | 200 ops | 98.8 µs (20.1 %) | 23.4 µs | 25.1 µs | 26.6 µs | 402.3 µs | 1,353.1 µs | 50,048 ops/s | 16.9 µs | 2.05 µs | 25.8 µs | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| load-exchanges | kds-group | - | - | - | 9.00 µs | 0.00 µs | 160.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 160.0 µs); cpu.on_cpu (0.00, 9.0 µs); sched.run_queue (1.00, 0.0 µs) |
| load-exchanges | pg | - | 0.00 µs | 0.00 µs | 3.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| load-exchanges | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 54.00 µs | 0.00 µs | 512.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | log file sync (1.00, 512.0 µs); SQL*Net message to client (1.00, 0.0 µs) |
| load-symbols | kds-group | - | - | - | 6.00 µs | 0.00 µs | 149.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 149.0 µs); cpu.on_cpu (0.00, 6.0 µs); sched.run_queue (1.00, 0.0 µs) |
| load-symbols | pg | - | 0.00 µs | 0.00 µs | 2.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| load-symbols | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 40.00 µs | 0.00 µs | 275.00 µs | 0.00 µs | 1.00 µs | 0.00 µs | log file sync (1.00, 275.0 µs); SQL*Net message to client (1.00, 1.0 µs) |
| load-sessions | kds-group | - | - | - | 4.82 µs | 0.00 µs | 1.08 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.7 µs); commit.wal_flush (0.01, 1.1 µs); sched.run_queue (1.02, 0.1 µs) |
| load-sessions | pg | - | 0.00 µs | 0.00 µs | 1.13 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| load-sessions | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.08 µs | 0.00 µs | 2.84 µs | 0.00 µs | 0.20 µs | 0.00 µs | log file sync (0.01, 2.8 µs); SQL*Net message to client (1.01, 0.2 µs) |
| load-bars | kds-group | - | - | - | 3.14 µs | 0.00 µs | 1.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 3.1 µs); commit.wal_flush (0.01, 1.1 µs); sched.run_queue (1.02, 0.1 µs) |
| load-bars | pg | - | 0.00 µs | 0.00 µs | 1.53 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | CPU (0.00, 4.8 µs) |
| load-bars | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.32 µs | 0.00 µs | 2.66 µs | 0.00 µs | 0.23 µs | 0.00 µs | log file sync (0.01, 2.7 µs); SQL*Net message to client (1.01, 0.2 µs) |
| load-stats | kds-group | - | - | - | 4.01 µs | 0.00 µs | 1.26 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.0 µs); commit.wal_flush (0.01, 1.3 µs); sched.run_queue (1.02, 0.1 µs) |
| load-stats | pg | - | 0.00 µs | 0.00 µs | 1.37 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| load-stats | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.49 µs | 0.00 µs | 3.49 µs | 0.00 µs | 0.28 µs | 0.00 µs | log file sync (0.01, 3.5 µs); SQL*Net message to client (1.01, 0.3 µs) |
| load-models | kds-group | - | - | - | 3.12 µs | 0.00 µs | 115.12 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 115.1 µs); cpu.on_cpu (0.00, 3.0 µs); sched.run_queue (1.00, 0.1 µs) |
| load-models | pg | - | 0.00 µs | 0.00 µs | 1.62 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| load-models | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 32.38 µs | 0.00 µs | 184.12 µs | 0.00 µs | 0.62 µs | 0.00 µs | log file sync (1.00, 184.1 µs); SQL*Net message to client (1.00, 0.6 µs) |
| backtest-read | kds-group | - | - | 12.67 µs | 6.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.72 µs | cpu.on_cpu (0.00, 18.6 µs); sched.run_queue (1.00, 0.1 µs) |
| backtest-read | pg | - | 0.00 µs | 0.00 µs | 13.08 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.28 µs | none |
| backtest-read | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 16.67 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.58 µs | 2.24 µs | SQL*Net message to client (1.00, 0.6 µs) |
| result-insert | kds-group | - | - | 0.00 µs | 3.75 µs | 0.00 µs | 117.34 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 117.3 µs); cpu.on_cpu (0.00, 3.7 µs); sched.run_queue (1.00, 0.1 µs) |
| result-insert | pg | - | 0.00 µs | 0.00 µs | 1.41 µs | 0.00 µs | 110.18 µs | 0.00 µs | 0.00 µs | 0.00 µs | WalSync (0.11, 110.2 µs) |
| result-insert | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 10.78 µs | 0.00 µs | 151.64 µs | 0.00 µs | 0.64 µs | 0.00 µs | log file sync (1.00, 151.6 µs); SQL*Net message to client (1.00, 0.6 µs) |
| backtest-replay | kds-group | - | - | 12.75 µs | 5.92 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.50 µs | cpu.on_cpu (0.00, 18.6 µs); sched.run_queue (1.00, 0.1 µs) |
| backtest-replay | pg | - | 0.00 µs | 0.00 µs | 12.42 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.81 µs | none |
| backtest-replay | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 18.58 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.50 µs | 9.63 µs | SQL*Net message to client (1.00, 0.5 µs) |
| compare-all | kds-group | - | - | 5.00 µs | 37.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 50.51 µs | cpu.on_cpu (0.00, 41.0 µs); sched.run_queue (1.00, 0.0 µs) |
| compare-all | pg | - | 0.00 µs | 0.00 µs | 24.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 28.36 µs | none |
| compare-all | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 49.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.00 µs | 57.29 µs | SQL*Net message to client (1.00, 3.0 µs) |
| compare-one | kds-group | - | - | 3.41 µs | 4.56 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.80 µs | cpu.on_cpu (0.00, 7.9 µs); sched.run_queue (1.00, 0.1 µs) |
| compare-one | pg | - | 0.00 µs | 0.00 µs | 4.56 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.06 µs | none |
| compare-one | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 6.06 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.19 µs | 4.40 µs | SQL*Net message to client (1.00, 0.2 µs) |
| read-bar-lookup | kds-group | - | - | 0.92 µs | 1.61 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.36 µs | cpu.on_cpu (0.00, 2.5 µs); sched.run_queue (1.00, 0.0 µs) |
| read-bar-lookup | pg | - | 0.00 µs | 0.00 µs | 5.47 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.20 µs | none |
| read-bar-lookup | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.29 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.26 µs | 1.33 µs | SQL*Net message to client (1.00, 0.3 µs) |
| read-bar-range | kds-group | - | - | 1.01 µs | 33.53 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 11.80 µs | cpu.on_cpu (0.00, 34.5 µs); sched.run_queue (1.00, 0.1 µs) |
| read-bar-range | pg | - | 0.00 µs | 0.00 µs | 40.48 µs | 0.00 µs | 0.00 µs | 0.00 µs | 11.77 µs | 8.77 µs | CPU (0.03, 25.4 µs); ClientRead (0.01, 11.8 µs) |
| read-bar-range | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 26.61 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.14 µs | 91.17 µs | SQL*Net message to client (1.00, 5.1 µs) |
| read-symbol-history | kds-group | - | - | 1.10 µs | 50.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 22.54 µs | cpu.on_cpu (0.00, 51.2 µs); sched.run_queue (1.00, 0.0 µs) |
| read-symbol-history | pg | - | 0.00 µs | 0.00 µs | 73.70 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 14.60 µs | CPU (0.10, 24.6 µs) |
| read-symbol-history | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 71.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 13.10 µs | 177.32 µs | SQL*Net message to client (1.00, 13.1 µs) |
| read-day-slice | kds-group | - | - | 1.02 µs | 4.22 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.14 µs | cpu.on_cpu (0.00, 5.2 µs); sched.run_queue (1.00, 0.1 µs) |
| read-day-slice | pg | - | 0.00 µs | 0.00 µs | 6.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.23 µs | 0.09 µs | ClientRead (0.01, 1.2 µs) |
| read-day-slice | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 5.64 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | 1.47 µs | SQL*Net message to client (1.00, 0.2 µs) |
| read-join-point | kds-group | - | - | 1.96 µs | 2.89 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.13 µs | cpu.on_cpu (0.00, 4.8 µs); sched.run_queue (1.00, 0.1 µs) |
| read-join-point | pg | - | 0.00 µs | 0.00 µs | 5.96 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.04 µs | 0.08 µs | ClientRead (0.01, 5.0 µs) |
| read-join-point | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.20 µs | 1.11 µs | SQL*Net message to client (1.00, 0.2 µs) |
| read-join-exists | kds-group | - | - | 1.60 µs | 2.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | cpu.on_cpu (0.00, 3.7 µs); sched.run_queue (1.00, 0.1 µs) |
| read-join-exists | pg | - | 0.00 µs | 0.00 µs | 1.30 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.11 µs | none |
| read-join-exists | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 12.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.20 µs | 0.97 µs | SQL*Net message to client (1.00, 0.2 µs) |
| agg-global | kds-group | - | - | 1.20 µs | 9.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.13 µs | cpu.on_cpu (0.00, 10.6 µs); sched.run_queue (1.00, 0.1 µs) |
| agg-global | pg | - | 0.00 µs | 0.00 µs | 12.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.11 µs | none |
| agg-global | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 10.40 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.30 µs | 1.43 µs | SQL*Net message to client (1.00, 0.3 µs) |
| agg-by-symbol | kds-group | - | - | 1.20 µs | 16.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.18 µs | cpu.on_cpu (0.00, 17.6 µs); sched.run_queue (1.00, 0.1 µs) |
| agg-by-symbol | pg | - | 0.00 µs | 0.00 µs | 15.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.07 µs | none |
| agg-by-symbol | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 13.60 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.60 µs | 1.18 µs | SQL*Net message to client (1.00, 0.6 µs) |
| agg-by-session | kds-group | - | - | 1.20 µs | 43.60 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 13.40 µs | cpu.on_cpu (0.00, 44.7 µs); sched.run_queue (1.00, 0.1 µs) |
| agg-by-session | pg | - | 0.00 µs | 0.00 µs | 56.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 6.84 µs | none |
| agg-by-session | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 68.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.30 µs | 45.09 µs | events in waitclass Other (7.00, 9.6 µs); SQL*Net message to client (1.00, 0.3 µs) |
| agg-day-slice | kds-group | - | - | 1.20 µs | 4.18 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.12 µs | cpu.on_cpu (0.00, 5.3 µs); sched.run_queue (1.00, 0.1 µs) |
| agg-day-slice | pg | - | 0.00 µs | 0.00 µs | 6.01 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.74 µs | 0.06 µs | ClientRead (0.01, 4.7 µs) |
| agg-day-slice | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 7.93 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.17 µs | 1.01 µs | SQL*Net message to client (1.00, 0.2 µs) |
| agg-distinct | kds-group | - | - | 0.90 µs | 12.60 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | cpu.on_cpu (0.00, 13.6 µs); sched.run_queue (1.00, 0.1 µs) |
| agg-distinct | pg | - | 0.00 µs | 0.00 µs | 13.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.13 µs | none |
| agg-distinct | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 10.60 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.30 µs | 2.68 µs | SQL*Net message to client (1.00, 0.3 µs) |
| qps-bar-lookup-cold | kds-group | - | - | 0.91 µs | 1.65 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | cpu.on_cpu (0.00, 2.5 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-lookup-cold | pg | - | 0.00 µs | 0.00 µs | 6.13 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.07 µs | none |
| qps-bar-lookup-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.28 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 1.43 µs | SQL*Net message to client (1.00, 0.2 µs) |
| qps-bar-lookup-warm | kds-group | - | - | 0.91 µs | 1.59 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.18 µs | cpu.on_cpu (0.00, 2.5 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-lookup-warm | pg | - | 0.00 µs | 0.00 µs | 6.29 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.15 µs | none |
| qps-bar-lookup-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.38 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.25 µs | 1.30 µs | SQL*Net message to client (1.00, 0.2 µs) |
| qps-bar-range-cold | kds-group | - | - | 1.02 µs | 34.73 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 21.33 µs | cpu.on_cpu (0.00, 35.7 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-range-cold | pg | - | 0.00 µs | 0.00 µs | 44.98 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.99 µs | CPU (0.08, 81.2 µs) |
| qps-bar-range-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 28.06 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.41 µs | 94.33 µs | SQL*Net message to client (1.00, 5.4 µs) |
| qps-bar-range-warm | kds-group | - | - | 1.01 µs | 34.43 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 17.97 µs | cpu.on_cpu (0.00, 35.4 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-range-warm | pg | - | 0.00 µs | 0.00 µs | 39.61 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.59 µs | 7.60 µs | CPU (0.03, 32.0 µs); ClientRead (0.01, 10.6 µs) |
| qps-bar-range-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 28.95 µs | 0.00 µs | 0.00 µs | 0.00 µs | 6.11 µs | 93.20 µs | SQL*Net message to client (1.00, 6.1 µs) |
| qps-day-slice-cold | kds-group | - | - | 1.03 µs | 4.24 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.18 µs | cpu.on_cpu (0.00, 5.2 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-day-slice-cold | pg | - | 0.00 µs | 0.00 µs | 6.16 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.06 µs | none |
| qps-day-slice-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 6.30 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.27 µs | 1.61 µs | SQL*Net message to client (1.00, 0.3 µs) |
| qps-day-slice-warm | kds-group | - | - | 1.02 µs | 4.24 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.19 µs | cpu.on_cpu (0.00, 5.2 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-day-slice-warm | pg | - | 0.00 µs | 0.00 µs | 6.28 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.06 µs | CPU (0.01, 2.4 µs) |
| qps-day-slice-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 5.35 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.14 µs | 1.41 µs | SQL*Net message to client (1.00, 0.1 µs) |
| qps-symbol-history-cold | kds-group | - | - | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| qps-symbol-history-cold | pg | - | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| qps-symbol-history-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| qps-symbol-history-warm | kds-group | - | - | 1.07 µs | 51.62 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 29.59 µs | cpu.on_cpu (0.00, 52.6 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-symbol-history-warm | pg | - | 0.00 µs | 0.00 µs | 62.77 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.98 µs | 11.99 µs | CPU (0.05, 53.3 µs); ClientRead (0.01, 11.0 µs) |
| qps-symbol-history-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 58.49 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.20 µs | 172.92 µs | SQL*Net message to client (1.00, 10.2 µs) |
| qps-cross-join-cold | kds-group | - | - | 12.26 µs | 5.69 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.27 µs | cpu.on_cpu (0.00, 17.9 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-cross-join-cold | pg | - | 0.00 µs | 0.00 µs | 12.09 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.10 µs | CPU (0.01, 2.4 µs) |
| qps-cross-join-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 11.17 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.31 µs | 1.57 µs | SQL*Net message to client (1.00, 0.3 µs) |
| qps-cross-join-warm | kds-group | - | - | 12.23 µs | 5.63 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.15 µs | cpu.on_cpu (0.00, 17.8 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-cross-join-warm | pg | - | 0.00 µs | 0.00 µs | 12.12 µs | 0.00 µs | 0.00 µs | 0.00 µs | 6.07 µs | 0.14 µs | ClientRead (0.01, 6.1 µs) |
| qps-cross-join-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 10.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | 1.41 µs | SQL*Net message to client (1.00, 0.2 µs) |
| qps-point-join-cold | kds-group | - | - | 1.94 µs | 2.90 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.13 µs | cpu.on_cpu (0.00, 4.8 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-point-join-cold | pg | - | 0.00 µs | 0.00 µs | 5.98 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.07 µs | none |
| qps-point-join-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.16 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.19 µs | 0.90 µs | SQL*Net message to client (1.00, 0.2 µs) |
| qps-point-join-warm | kds-group | - | - | 1.95 µs | 2.88 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.13 µs | cpu.on_cpu (0.00, 4.8 µs); sched.run_queue (1.00, 0.0 µs) |
| qps-point-join-warm | pg | - | 0.00 µs | 0.00 µs | 5.94 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.46 µs | 0.07 µs | ClientRead (0.01, 2.5 µs) |
| qps-point-join-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.77 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.27 µs | 1.15 µs | SQL*Net message to client (1.00, 0.3 µs) |
| qps-model-join-cold | kds-group | - | - | 3.71 µs | 4.86 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.34 µs | cpu.on_cpu (0.00, 8.6 µs); sched.run_queue (1.00, 0.0 µs) |
| qps-model-join-cold | pg | - | 0.00 µs | 0.00 µs | 4.14 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.61 µs | none |
| qps-model-join-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 20.71 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.00 µs | 5.07 µs | SQL*Net message to client (1.00, 1.0 µs) |
| qps-model-join-warm | kds-group | - | - | 3.30 µs | 4.55 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.74 µs | cpu.on_cpu (0.00, 7.8 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-model-join-warm | pg | - | 0.00 µs | 0.00 µs | 4.09 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.56 µs | none |
| qps-model-join-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 7.32 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.31 µs | 6.20 µs | SQL*Net message to client (1.00, 0.3 µs) |
| write-batch-1 | kds-group | - | - | 0.00 µs | 4.98 µs | 0.00 µs | 120.31 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 120.3 µs); cpu.on_cpu (0.00, 4.9 µs); sched.run_queue (1.00, 0.1 µs) |
| write-batch-1 | pg | - | 0.00 µs | 0.00 µs | 1.60 µs | 0.00 µs | 105.84 µs | 0.00 µs | 8.89 µs | 0.00 µs | WalSync (0.10, 104.3 µs); ClientRead (0.01, 8.9 µs); CPU (0.01, 8.0 µs); WalWrite (0.00, 1.5 µs) |
| write-batch-1 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 15.64 µs | 0.00 µs | 187.49 µs | 0.00 µs | 0.87 µs | 0.00 µs | log file sync (1.00, 187.5 µs); SQL*Net message to client (1.00, 0.9 µs) |
| write-batch-10 | kds-group | - | - | 0.00 µs | 4.35 µs | 0.00 µs | 12.07 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (0.10, 12.1 µs); cpu.on_cpu (0.00, 4.3 µs); sched.run_queue (1.20, 0.1 µs) |
| write-batch-10 | pg | - | 0.00 µs | 0.00 µs | 1.10 µs | 0.00 µs | 10.63 µs | 0.00 µs | 1.01 µs | 0.00 µs | WalSync (0.01, 10.6 µs); ClientRead (0.00, 1.0 µs); CPU (0.00, 0.5 µs) |
| write-batch-10 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.25 µs | 0.00 µs | 13.72 µs | 0.00 µs | 0.23 µs | 0.00 µs | log file sync (0.10, 13.7 µs); SQL*Net message to client (1.10, 0.2 µs); log file switch (checkpoint incomplete) (0.00, 0.0 µs); log file switch completion (0.00, 0.0 µs) |
| write-batch-100 | kds-group | - | - | 0.00 µs | 4.20 µs | 0.00 µs | 1.59 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.1 µs); commit.wal_flush (0.01, 1.6 µs); sched.run_queue (1.02, 0.1 µs) |
| write-batch-100 | pg | - | 0.00 µs | 0.00 µs | 1.11 µs | 0.00 µs | 2.47 µs | 0.00 µs | 0.41 µs | 0.00 µs | WalSync (0.00, 2.5 µs); CPU (0.00, 1.0 µs); ClientRead (0.00, 0.4 µs) |
| write-batch-100 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.71 µs | 0.00 µs | 2.34 µs | 0.00 µs | 0.19 µs | 0.00 µs | log file sync (0.01, 2.3 µs); SQL*Net message to client (1.01, 0.2 µs) |
| write-batch-1000 | kds-group | - | - | 0.00 µs | 4.44 µs | 0.00 µs | 0.19 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.4 µs); commit.wal_flush (0.00, 0.2 µs); sched.run_queue (1.00, 0.1 µs) |
| write-batch-1000 | pg | - | 0.00 µs | 0.00 µs | 1.05 µs | 0.00 µs | 0.50 µs | 0.00 µs | 2.26 µs | 0.00 µs | ClientRead (0.00, 2.3 µs); WalSync (0.00, 0.5 µs) |
| write-batch-1000 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.58 µs | 0.00 µs | 0.70 µs | 0.00 µs | 0.20 µs | 0.00 µs | log file sync (0.00, 0.7 µs); SQL*Net message to client (1.00, 0.2 µs) |
| join-conn-1 | kds-group | - | - | 12.29 µs | 5.74 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.16 µs | cpu.on_cpu (0.00, 18.0 µs); sched.run_queue (1.00, 0.1 µs) |
| join-conn-1 | pg | - | 0.00 µs | 0.00 µs | 12.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 11.15 µs | 0.10 µs | ClientRead (0.01, 11.2 µs); CPU (0.01, 2.6 µs) |
| join-conn-1 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 10.63 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.26 µs | 1.56 µs | SQL*Net message to client (1.00, 0.3 µs) |
| join-conn-2 | kds-group | - | - | 12.18 µs | 6.04 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.22 µs | cpu.on_cpu (0.00, 17.7 µs); sched.run_queue (1.00, 0.5 µs) |
| join-conn-2 | pg | - | 0.00 µs | 0.00 µs | 14.11 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.49 µs | 0.13 µs | ClientRead (0.01, 5.5 µs) |
| join-conn-2 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 12.40 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.29 µs | 2.57 µs | SQL*Net message to client (1.00, 0.3 µs) |
| join-conn-4 | kds-group | - | - | 12.15 µs | 14.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.20 µs | cpu.on_cpu (0.00, 17.7 µs); sched.run_queue (1.00, 8.9 µs) |
| join-conn-4 | pg | - | 0.00 µs | 0.00 µs | 13.07 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.13 µs | CPU (0.01, 1.3 µs) |
| join-conn-4 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 13.63 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.33 µs | 1.75 µs | SQL*Net message to client (1.00, 0.3 µs) |
| join-conn-8 | kds-group | - | - | 12.46 µs | 48.19 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.32 µs | sched.run_queue (1.00, 42.6 µs); cpu.on_cpu (0.00, 18.1 µs) |
| join-conn-8 | pg | - | 0.00 µs | 0.00 µs | 16.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.50 µs | 0.61 µs | CPU (0.03, 19.1 µs); ClientRead (0.01, 2.5 µs) |
| join-conn-8 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 17.14 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.54 µs | 2.05 µs | SQL*Net message to client (1.00, 0.5 µs); resmgr:cpu quantum (0.01, 0.3 µs) |

### s1 `bars60480`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, kds-strict 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| load-exchanges | kds-group | 2 ops | 247.5 µs (17.5 %) | 210.7 µs | 210.7 µs | 289.1 µs | 289.1 µs | 289.1 µs | 2,888 ops/s | 13.9 µs | 0.00 µs | 43.2 µs | 0 errors |
| load-exchanges | kds-strict | 2 ops | 238.9 µs (54.7 %) | 174.7 µs | 174.7 µs | 303.2 µs | 303.2 µs | 303.2 µs | 2,948 ops/s | 13.0 µs | 0.00 µs | 45.9 µs | 0 errors |
| load-exchanges | pg | 2 ops | 188.0 µs (33.8 %) | 135.4 µs | 135.4 µs | 240.5 µs | 240.5 µs | 240.5 µs | 4,350 ops/s | 15.7 µs | 0.00 µs | 19.5 µs | 0 errors |
| load-exchanges | oracle | 2 ops | 838.5 µs (54.3 %) | 405.2 µs | 405.2 µs | 1,217.1 µs | 1,217.1 µs | 1,217.1 µs | 973 ops/s | 239.3 µs | 0.00 µs | 145.0 µs | 0 errors |
| load-symbols | kds-group | 8 ops | 231.7 µs (51.7 %) | 142.5 µs | 228.9 µs | 244.6 µs | 312.2 µs | 312.2 µs | 4,010 ops/s | 10.7 µs | 0.00 µs | 21.2 µs | 0 errors |
| load-symbols | kds-strict | 8 ops | 220.3 µs (50.9 %) | 137.4 µs | 222.4 µs | 245.4 µs | 262.1 µs | 262.1 µs | 4,255 ops/s | 10.0 µs | 0.00 µs | 19.0 µs | 0 errors |
| load-symbols | pg | 8 ops | 158.1 µs (32.5 %) | 127.9 µs | 130.2 µs | 133.4 µs | 261.5 µs | 261.5 µs | 6,136 ops/s | 8.9 µs | 0.00 µs | 10.0 µs | 0 errors |
| load-symbols | oracle | 8 ops | 490.7 µs (63.1 %) | 204.5 µs | 353.6 µs | 476.8 µs | 904.7 µs | 904.7 µs | 1,910 ops/s | 71.5 µs | 0.00 µs | 96.4 µs | 0 errors |
| load-sessions | kds-group | 7,560 ops | 15.2 µs (6.8 %) | 12.4 µs | 14.1 µs | 14.4 µs | 17.9 µs | 26.5 µs | 61,506 ops/s | 2.7 µs | 0.00 µs | 8.4 µs | 0 errors |
| load-sessions | kds-strict | 7,560 ops | 14.8 µs (2.4 %) | 12.4 µs | 13.7 µs | 14.1 µs | 15.9 µs | 23.3 µs | 63,212 ops/s | 2.8 µs | 0.00 µs | 8.3 µs | 0 errors |
| load-sessions | pg | 7,560 ops | 13.5 µs (1.2 %) | 11.9 µs | 13.0 µs | 13.4 µs | 14.0 µs | 19.0 µs | 69,226 ops/s | 4.5 µs | 0.00 µs | 5.1 µs | 0 errors |
| load-sessions | oracle | 7,560 ops | 16.6 µs (17.8 %) | 13.3 µs | 15.1 µs | 15.4 µs | 26.3 µs | 33.3 µs | 54,564 ops/s | 4.9 µs | 0.00 µs | 8.4 µs | 0 errors |
| load-bars | kds-group | 60,480 ops | 12.8 µs (3.8 %) | 11.1 µs | 12.5 µs | 12.7 µs | 13.3 µs | 20.4 µs | 72,516 ops/s | 2.7 µs | 0.00 µs | 6.5 µs | 0 errors |
| load-bars | kds-strict | 60,480 ops | 13.1 µs (1.0 %) | 11.1 µs | 12.5 µs | 12.8 µs | 13.5 µs | 21.1 µs | 70,950 ops/s | 2.7 µs | 0.00 µs | 6.5 µs | 0 errors |
| load-bars | pg | 60,480 ops | 14.2 µs (3.2 %) | 12.2 µs | 13.6 µs | 13.8 µs | 14.7 µs | 23.1 µs | 65,702 ops/s | 4.6 µs | 0.00 µs | 5.7 µs | 0 errors |
| load-bars | oracle | 60,480 ops | 22.8 µs (22.2 %) | 13.7 µs | 15.7 µs | 16.0 µs | 20.1 µs | 45.6 µs | 40,956 ops/s | 5.1 µs | 0.00 µs | 9.0 µs | 0 errors |
| load-stats | kds-group | 60,480 ops | 15.2 µs (2.4 %) | 13.0 µs | 14.3 µs | 14.9 µs | 16.1 µs | 22.7 µs | 60,672 ops/s | 2.7 µs | 0.00 µs | 9.0 µs | 0 errors |
| load-stats | kds-strict | 60,480 ops | 15.6 µs (2.0 %) | 13.2 µs | 14.8 µs | 15.1 µs | 16.3 µs | 23.5 µs | 59,329 ops/s | 2.7 µs | 0.00 µs | 9.1 µs | 0 errors |
| load-stats | pg | 60,480 ops | 14.2 µs (1.1 %) | 12.4 µs | 13.4 µs | 14.0 µs | 14.8 µs | 19.7 µs | 64,581 ops/s | 4.6 µs | 0.00 µs | 5.9 µs | 0 errors |
| load-stats | oracle | 60,480 ops | 23.4 µs (15.1 %) | 14.3 µs | 16.3 µs | 16.6 µs | 20.4 µs | 45.4 µs | 39,922 ops/s | 5.4 µs | 0.00 µs | 9.5 µs | 0 errors |
| load-models | kds-group | 8 ops | 297.2 µs (14.2 %) | 192.0 µs | 270.3 µs | 308.1 µs | 412.0 µs | 412.0 µs | 3,174 ops/s | 22.3 µs | 0.00 µs | 11.7 µs | 0 errors |
| load-models | kds-strict | 8 ops | 219.0 µs (34.2 %) | 180.9 µs | 189.1 µs | 212.4 µs | 305.6 µs | 305.6 µs | 4,007 ops/s | 6.0 µs | 0.00 µs | 11.5 µs | 0 errors |
| load-models | pg | 8 ops | 170.4 µs (49.7 %) | 129.2 µs | 135.9 µs | 150.1 µs | 276.0 µs | 276.0 µs | 5,229 ops/s | 10.5 µs | 0.00 µs | 12.5 µs | 0 errors |
| load-models | oracle | 8 ops | 412.4 µs (42.3 %) | 145.2 µs | 286.6 µs | 393.7 µs | 825.8 µs | 825.8 µs | 2,285 ops/s | 41.4 µs | 0.00 µs | 78.4 µs | 0 errors |
| backtest-read | kds-group | 360 ops | 699.2 µs (3.0 %) | 675.7 µs | 693.3 µs | 697.1 µs | 713.2 µs | 791.9 µs | 1,427 ops/s | 22.5 µs | 6.47 µs | 664.2 µs | 0 errors |
| backtest-read | kds-strict | 360 ops | 699.4 µs (2.0 %) | 666.9 µs | 687.5 µs | 694.4 µs | 708.5 µs | 847.6 µs | 1,428 ops/s | 22.8 µs | 6.40 µs | 662.9 µs | 0 errors |
| backtest-read | pg | 360 ops | 1,275.7 µs (23.7 %) | 1,223.1 µs | 1,252.5 µs | 1,264.0 µs | 1,303.6 µs | 1,636.9 µs | 783 ops/s | 32.3 µs | 3.28 µs | 1,228.5 µs | 0 errors |
| backtest-read | oracle | 360 ops | 527.8 µs (1.2 %) | 429.4 µs | 490.5 µs | 495.5 µs | 748.2 µs | 884.9 µs | 1,891 ops/s | 55.4 µs | 28.00 µs | 449.8 µs | 0 errors |
| result-insert | kds-group | 2,872 ops | 223.0 µs (3.5 %) | 176.7 µs | 197.9 µs | 202.6 µs | 298.1 µs | 486.0 µs | 4,480 ops/s | 14.3 µs | 0.00 µs | 16.7 µs | 0 errors |
| result-insert | kds-strict | 2,872 ops | 219.3 µs (3.0 %) | 178.4 µs | 196.9 µs | 201.0 µs | 307.8 µs | 474.3 µs | 4,555 ops/s | 13.8 µs | 0.00 µs | 16.8 µs | 0 errors |
| result-insert | pg | 2,872 ops | 149.2 µs (143.9 %) | 128.2 µs | 137.9 µs | 141.0 µs | 174.4 µs | 353.2 µs | 6,694 ops/s | 12.4 µs | 0.00 µs | 13.1 µs | 0 errors |
| result-insert | oracle | 2,872 ops | 277.8 µs (14.3 %) | 136.5 µs | 168.3 µs | 200.9 µs | 533.3 µs | 696.9 µs | 3,596 ops/s | 24.0 µs | 0.00 µs | 57.9 µs | 0 errors |
| backtest-replay | kds-group | 360 ops | 685.5 µs (2.8 %) | 660.3 µs | 677.5 µs | 682.6 µs | 694.6 µs | 735.4 µs | 1,457 ops/s | 18.4 µs | 2.97 µs | 654.9 µs | 0 errors |
| backtest-replay | kds-strict | 360 ops | 703.0 µs (2.4 %) | 672.8 µs | 692.7 µs | 697.3 µs | 776.8 µs | 859.2 µs | 1,420 ops/s | 20.1 µs | 3.09 µs | 667.8 µs | 0 errors |
| backtest-replay | pg | 360 ops | 1,269.3 µs (2.1 %) | 1,191.5 µs | 1,230.1 µs | 1,246.5 µs | 1,417.7 µs | 1,470.4 µs | 787 ops/s | 28.5 µs | 1.83 µs | 1,214.4 µs | 0 errors |
| backtest-replay | oracle | 360 ops | 520.2 µs (4.1 %) | 435.1 µs | 495.8 µs | 501.2 µs | 710.5 µs | 864.1 µs | 1,916 ops/s | 51.3 µs | 25.22 µs | 459.3 µs | 0 errors |
| compare-all | kds-group | 1 ops | 1,288.0 µs (5.4 %) | 1,288.0 µs | 1,288.0 µs | 1,288.0 µs | 1,288.0 µs | 1,288.0 µs | 697 ops/s | 248.3 µs | 216.67 µs | 973.3 µs | 0 errors |
| compare-all | kds-strict | 1 ops | 1,469.6 µs (13.8 %) | 1,469.6 µs | 1,469.6 µs | 1,469.6 µs | 1,469.6 µs | 1,469.6 µs | 579 ops/s | 251.1 µs | 214.72 µs | 1,043.8 µs | 0 errors |
| compare-all | pg | 1 ops | 1,475.3 µs (8.0 %) | 1,475.3 µs | 1,475.3 µs | 1,475.3 µs | 1,475.3 µs | 1,475.3 µs | 606 ops/s | 1,225.0 µs | 323.66 µs | 507.0 µs | 0 errors |
| compare-all | oracle | 1 ops | 2,140.8 µs (6.1 %) | 2,140.8 µs | 2,140.8 µs | 2,140.8 µs | 2,140.8 µs | 2,140.8 µs | 417 ops/s | 1,816.1 µs | 1,232.61 µs | 495.0 µs | 0 errors |
| compare-one | kds-group | 32 ops | 150.2 µs (48.0 %) | 110.3 µs | 111.8 µs | 115.3 µs | 244.1 µs | 250.6 µs | 6,491 ops/s | 38.6 µs | 23.05 µs | 78.4 µs | 0 errors |
| compare-one | kds-strict | 32 ops | 151.0 µs (29.2 %) | 112.6 µs | 113.8 µs | 115.5 µs | 240.7 µs | 248.6 µs | 6,319 ops/s | 40.4 µs | 23.03 µs | 77.4 µs | 0 errors |
| compare-one | pg | 32 ops | 181.7 µs (45.3 %) | 163.5 µs | 165.9 µs | 167.4 µs | 304.7 µs | 330.8 µs | 5,451 ops/s | 121.5 µs | 13.49 µs | 101.5 µs | 0 errors |
| compare-one | oracle | 32 ops | 321.5 µs (27.3 %) | 187.1 µs | 196.3 µs | 325.8 µs | 508.3 µs | 737.0 µs | 2,995 ops/s | 155.2 µs | 110.65 µs | 79.6 µs | 0 errors |
| read-bar-lookup | kds-group | 200 ops | 13.0 µs (14.9 %) | 11.5 µs | 12.7 µs | 13.1 µs | 13.8 µs | 14.9 µs | 74,260 ops/s | 3.0 µs | 0.10 µs | 6.4 µs | 0 errors |
| read-bar-lookup | kds-strict | 200 ops | 15.1 µs (30.5 %) | 11.9 µs | 13.0 µs | 13.3 µs | 14.2 µs | 54.2 µs | 62,568 ops/s | 3.3 µs | 0.11 µs | 6.5 µs | 0 errors |
| read-bar-lookup | pg | 200 ops | 16.4 µs (31.2 %) | 13.3 µs | 14.2 µs | 14.4 µs | 37.4 µs | 43.1 µs | 58,152 ops/s | 6.2 µs | 0.07 µs | 5.8 µs | 0 errors |
| read-bar-lookup | oracle | 200 ops | 21.7 µs (55.8 %) | 14.7 µs | 16.0 µs | 16.4 µs | 48.2 µs | 161.9 µs | 43,882 ops/s | 8.0 µs | 1.52 µs | 7.7 µs | 0 errors |
| read-bar-range | kds-group | 200 ops | 77.0 µs (7.3 %) | 56.9 µs | 65.2 µs | 69.9 µs | 85.1 µs | 227.5 µs | 12,849 ops/s | 26.5 µs | 16.63 µs | 40.7 µs | 0 errors |
| read-bar-range | kds-strict | 200 ops | 71.9 µs (8.3 %) | 56.8 µs | 62.4 µs | 64.5 µs | 85.5 µs | 184.8 µs | 13,669 ops/s | 20.1 µs | 12.41 µs | 41.6 µs | 0 errors |
| read-bar-range | pg | 200 ops | 113.8 µs (35.2 %) | 93.6 µs | 96.1 µs | 97.2 µs | 292.9 µs | 348.6 µs | 8,702 ops/s | 73.7 µs | 7.54 µs | 49.3 µs | 0 errors |
| read-bar-range | oracle | 200 ops | 228.0 µs (11.8 %) | 159.3 µs | 164.6 µs | 168.6 µs | 418.9 µs | 479.6 µs | 4,339 ops/s | 137.2 µs | 109.25 µs | 49.3 µs | 0 errors |
| read-symbol-history | kds-group | 10 ops | 2,534.2 µs (2.2 %) | 2,199.3 µs | 2,415.6 µs | 2,515.8 µs | 2,969.0 µs | 2,969.0 µs | 388 ops/s | 878.0 µs | 681.46 µs | 2,101.0 µs | 0 errors |
| read-symbol-history | kds-strict | 10 ops | 2,560.2 µs (3.5 %) | 2,210.6 µs | 2,368.5 µs | 2,535.0 µs | 3,219.6 µs | 3,219.6 µs | 385 ops/s | 850.4 µs | 657.14 µs | 2,084.8 µs | 0 errors |
| read-symbol-history | pg | 10 ops | 4,096.1 µs (0.5 %) | 3,678.4 µs | 3,794.9 µs | 4,013.8 µs | 5,400.4 µs | 5,400.4 µs | 242 ops/s | 3,019.8 µs | 459.30 µs | 2,781.4 µs | 0 errors |
| read-symbol-history | oracle | 10 ops | 8,588.4 µs (4.0 %) | 8,043.2 µs | 8,307.2 µs | 8,494.2 µs | 9,362.8 µs | 9,362.8 µs | 116 ops/s | 7,671.0 µs | 6,328.58 µs | 1,938.2 µs | 0 errors |
| read-day-slice | kds-group | 200 ops | 666.6 µs (2.3 %) | 633.8 µs | 656.5 µs | 666.5 µs | 725.7 µs | 781.0 µs | 1,498 ops/s | 17.5 µs | 3.11 µs | 638.5 µs | 0 errors |
| read-day-slice | kds-strict | 200 ops | 692.5 µs (5.0 %) | 641.3 µs | 665.8 µs | 670.2 µs | 809.0 µs | 846.7 µs | 1,442 ops/s | 20.3 µs | 3.28 µs | 642.7 µs | 0 errors |
| read-day-slice | pg | 200 ops | 1,332.0 µs (16.3 %) | 1,197.5 µs | 1,242.9 µs | 1,264.0 µs | 1,576.1 µs | 1,634.3 µs | 750 ops/s | 51.8 µs | 2.31 µs | 1,211.2 µs | 0 errors |
| read-day-slice | oracle | 200 ops | 450.1 µs (6.7 %) | 380.8 µs | 434.9 µs | 451.7 µs | 589.6 µs | 739.3 µs | 2,215 ops/s | 40.1 µs | 20.47 µs | 404.0 µs | 0 errors |
| read-join-point | kds-group | 200 ops | 34.3 µs (3.1 %) | 31.7 µs | 32.6 µs | 32.9 µs | 39.1 µs | 55.0 µs | 27,505 ops/s | 16.1 µs | 0.48 µs | 10.0 µs | 0 errors |
| read-join-point | kds-strict | 200 ops | 33.4 µs (4.2 %) | 29.5 µs | 32.7 µs | 33.2 µs | 36.2 µs | 48.6 µs | 28,264 ops/s | 15.2 µs | 0.46 µs | 10.0 µs | 0 errors |
| read-join-point | pg | 200 ops | 60.9 µs (30.5 %) | 25.1 µs | 32.0 µs | 34.5 µs | 270.2 µs | 410.8 µs | 15,672 ops/s | 23.7 µs | 0.32 µs | 12.6 µs | 0 errors |
| read-join-point | oracle | 200 ops | 31.5 µs (122.8 %) | 17.7 µs | 18.4 µs | 18.9 µs | 40.8 µs | 287.3 µs | 30,522 ops/s | 10.0 µs | 1.38 µs | 9.6 µs | 0 errors |
| read-join-exists | kds-group | 10 ops | 2,175.1 µs (9.7 %) | 2,075.6 µs | 2,154.5 µs | 2,196.4 µs | 2,214.7 µs | 2,214.7 µs | 450 ops/s | 65.8 µs | 3.73 µs | 1,935.4 µs | 0 errors |
| read-join-exists | kds-strict | 10 ops | 2,081.5 µs (14.8 %) | 2,004.7 µs | 2,040.3 µs | 2,091.9 µs | 2,163.3 µs | 2,163.3 µs | 469 ops/s | 75.2 µs | 4.21 µs | 1,938.1 µs | 0 errors |
| read-join-exists | pg | 10 ops | 4,306.9 µs (2.3 %) | 4,172.5 µs | 4,272.7 µs | 4,313.3 µs | 4,510.2 µs | 4,510.2 µs | 230 ops/s | 122.1 µs | 3.67 µs | 4,018.5 µs | 0 errors |
| read-join-exists | oracle | 10 ops | 687.8 µs (18.5 %) | 592.3 µs | 619.1 µs | 626.6 µs | 1,080.1 µs | 1,080.1 µs | 1,369 ops/s | 75.0 µs | 7.47 µs | 590.5 µs | 0 errors |
| agg-global | kds-group | 10 ops | 1,969.5 µs (12.2 %) | 1,823.3 µs | 1,910.1 µs | 1,929.4 µs | 2,144.0 µs | 2,144.0 µs | 496 ops/s | 74.8 µs | 2.11 µs | 1,733.7 µs | 0 errors |
| agg-global | kds-strict | 10 ops | 1,982.0 µs (0.8 %) | 1,836.1 µs | 1,936.3 µs | 1,974.4 µs | 2,103.1 µs | 2,103.1 µs | 494 ops/s | 72.9 µs | 2.08 µs | 1,745.4 µs | 0 errors |
| agg-global | pg | 10 ops | 2,256.4 µs (10.6 %) | 2,070.8 µs | 2,251.7 µs | 2,277.1 µs | 2,378.9 µs | 2,378.9 µs | 434 ops/s | 123.7 µs | 1.60 µs | 1,936.7 µs | 0 errors |
| agg-global | oracle | 10 ops | 1,483.8 µs (13.1 %) | 1,262.9 µs | 1,288.5 µs | 1,488.5 µs | 2,104.6 µs | 2,104.6 µs | 656 ops/s | 79.2 µs | 4.48 µs | 1,288.5 µs | 0 errors |
| agg-by-symbol | kds-group | 10 ops | 3,700.8 µs (7.3 %) | 3,619.5 µs | 3,647.3 µs | 3,681.1 µs | 3,880.5 µs | 3,880.5 µs | 267 ops/s | 65.8 µs | 3.22 µs | 3,485.2 µs | 0 errors |
| agg-by-symbol | kds-strict | 10 ops | 3,764.7 µs (2.0 %) | 3,593.8 µs | 3,645.8 µs | 3,718.9 µs | 3,883.9 µs | 3,883.9 µs | 263 ops/s | 76.5 µs | 3.63 µs | 3,489.8 µs | 0 errors |
| agg-by-symbol | pg | 10 ops | 3,523.7 µs (8.1 %) | 3,452.8 µs | 3,473.9 µs | 3,512.9 µs | 3,622.2 µs | 3,622.2 µs | 280 ops/s | 134.7 µs | 2.10 µs | 3,235.5 µs | 0 errors |
| agg-by-symbol | oracle | 10 ops | 1,909.7 µs (7.0 %) | 1,628.8 µs | 1,774.0 µs | 1,898.3 µs | 2,498.1 µs | 2,498.1 µs | 508 ops/s | 149.2 µs | 23.40 µs | 1,578.1 µs | 0 errors |
| agg-by-session | kds-group | 10 ops | 5,715.8 µs (8.4 %) | 5,268.0 µs | 5,400.6 µs | 5,638.6 µs | 6,689.3 µs | 6,689.3 µs | 173 ops/s | 788.3 µs | 497.91 µs | 4,820.3 µs | 0 errors |
| agg-by-session | kds-strict | 10 ops | 5,622.0 µs (3.2 %) | 5,138.1 µs | 5,485.0 µs | 5,542.5 µs | 6,568.4 µs | 6,568.4 µs | 176 ops/s | 764.7 µs | 482.88 µs | 4,780.5 µs | 0 errors |
| agg-by-session | pg | 10 ops | 6,558.5 µs (1.0 %) | 6,431.7 µs | 6,475.4 µs | 6,567.9 µs | 6,827.2 µs | 6,827.2 µs | 151 ops/s | 2,460.0 µs | 232.46 µs | 4,785.1 µs | 0 errors |
| agg-by-session | oracle | 10 ops | 4,117.4 µs (3.8 %) | 3,988.8 µs | 4,097.5 µs | 4,109.3 µs | 4,219.2 µs | 4,219.2 µs | 239 ops/s | 1,767.8 µs | 1,302.60 µs | 2,554.6 µs | 0 errors |
| agg-day-slice | kds-group | 200 ops | 681.3 µs (5.3 %) | 642.7 µs | 656.3 µs | 660.0 µs | 682.2 µs | 1,695.1 µs | 1,463 ops/s | 15.7 µs | 1.02 µs | 652.5 µs | 0 errors |
| agg-day-slice | kds-strict | 200 ops | 681.9 µs (3.8 %) | 639.5 µs | 651.7 µs | 660.5 µs | 682.2 µs | 1,587.8 µs | 1,462 ops/s | 18.5 µs | 1.15 µs | 649.5 µs | 0 errors |
| agg-day-slice | pg | 200 ops | 1,421.3 µs (6.4 %) | 1,218.9 µs | 1,252.9 µs | 1,294.9 µs | 1,701.3 µs | 1,751.8 µs | 702 ops/s | 58.1 µs | 1.30 µs | 1,242.6 µs | 0 errors |
| agg-day-slice | oracle | 200 ops | 486.5 µs (1.4 %) | 399.6 µs | 466.6 µs | 471.6 µs | 668.2 µs | 781.7 µs | 2,045 ops/s | 37.4 µs | 10.59 µs | 431.0 µs | 0 errors |
| agg-distinct | kds-group | 10 ops | 2,970.5 µs (7.3 %) | 2,788.6 µs | 2,883.1 µs | 2,953.1 µs | 3,791.5 µs | 3,791.5 µs | 330 ops/s | 64.5 µs | 1.59 µs | 2,829.7 µs | 0 errors |
| agg-distinct | kds-strict | 10 ops | 2,896.8 µs (10.0 %) | 2,720.9 µs | 2,779.3 µs | 2,791.1 µs | 3,835.1 µs | 3,835.1 µs | 342 ops/s | 42.5 µs | 1.13 µs | 2,753.3 µs | 0 errors |
| agg-distinct | pg | 10 ops | 2,038.0 µs (16.8 %) | 1,954.4 µs | 1,979.8 µs | 2,009.3 µs | 2,217.3 µs | 2,217.3 µs | 481 ops/s | 31.5 µs | 0.17 µs | 1,984.1 µs | 0 errors |
| agg-distinct | oracle | 10 ops | 1,209.9 µs (17.5 %) | 1,010.6 µs | 1,066.2 µs | 1,163.5 µs | 1,766.7 µs | 1,766.7 µs | 797 ops/s | 118.2 µs | 9.54 µs | 896.8 µs | 0 errors |
| qps-bar-lookup-cold | kds-group | 100 ops | 34.8 µs (6.1 %) | 29.8 µs | 30.5 µs | 30.8 µs | 42.4 µs | 87.8 µs | 25,341 ops/s | 16.5 µs | 0.64 µs | 7.7 µs | 0 errors |
| qps-bar-lookup-cold | kds-strict | 100 ops | 31.7 µs (42.0 %) | 27.6 µs | 30.1 µs | 30.4 µs | 37.0 µs | 45.1 µs | 27,872 ops/s | 15.9 µs | 0.61 µs | 7.5 µs | 0 errors |
| qps-bar-lookup-cold | pg | 100 ops | 38.9 µs (64.0 %) | 29.2 µs | 29.9 µs | 35.5 µs | 48.9 µs | 57.9 µs | 23,899 ops/s | 23.8 µs | 0.28 µs | 6.7 µs | 0 errors |
| qps-bar-lookup-cold | oracle | 100 ops | 60.3 µs (100.4 %) | 30.5 µs | 31.4 µs | 31.9 µs | 208.3 µs | 478.4 µs | 15,392 ops/s | 30.3 µs | 4.92 µs | 10.6 µs | 0 errors |
| qps-bar-lookup-warm | kds-group | 100 ops | 27.3 µs (28.4 %) | 22.4 µs | 22.9 µs | 23.4 µs | 28.2 µs | 47.3 µs | 31,882 ops/s | 12.3 µs | 0.48 µs | 7.3 µs | 0 errors |
| qps-bar-lookup-warm | kds-strict | 100 ops | 26.2 µs (44.4 %) | 19.3 µs | 19.7 µs | 28.6 µs | 29.8 µs | 36.3 µs | 34,870 ops/s | 12.3 µs | 0.48 µs | 7.2 µs | 0 errors |
| qps-bar-lookup-warm | pg | 100 ops | 27.7 µs (21.6 %) | 20.9 µs | 21.3 µs | 29.4 µs | 33.6 µs | 41.4 µs | 34,564 ops/s | 16.0 µs | 0.19 µs | 6.2 µs | 0 errors |
| qps-bar-lookup-warm | oracle | 100 ops | 39.7 µs (51.5 %) | 21.6 µs | 22.0 µs | 22.7 µs | 33.2 µs | 322.1 µs | 23,254 ops/s | 17.7 µs | 3.03 µs | 9.1 µs | 0 errors |
| qps-bar-range-cold | kds-group | 100 ops | 106.5 µs (34.0 %) | 67.5 µs | 75.9 µs | 85.9 µs | 221.6 µs | 258.4 µs | 9,144 ops/s | 47.7 µs | 29.92 µs | 41.0 µs | 0 errors |
| qps-bar-range-cold | kds-strict | 100 ops | 103.9 µs (11.5 %) | 65.4 µs | 79.6 µs | 87.8 µs | 220.5 µs | 257.3 µs | 9,352 ops/s | 44.1 µs | 28.73 µs | 42.4 µs | 0 errors |
| qps-bar-range-cold | pg | 100 ops | 122.7 µs (22.7 %) | 93.4 µs | 95.5 µs | 102.2 µs | 284.1 µs | 382.4 µs | 8,043 ops/s | 87.9 µs | 9.08 µs | 49.1 µs | 0 errors |
| qps-bar-range-cold | oracle | 100 ops | 195.3 µs (21.9 %) | 159.6 µs | 163.4 µs | 166.1 µs | 342.7 µs | 511.9 µs | 5,053 ops/s | 137.4 µs | 107.79 µs | 45.1 µs | 0 errors |
| qps-bar-range-warm | kds-group | 100 ops | 78.8 µs (19.7 %) | 57.5 µs | 61.0 µs | 64.8 µs | 178.7 µs | 230.2 µs | 12,287 ops/s | 22.9 µs | 14.75 µs | 40.1 µs | 0 errors |
| qps-bar-range-warm | kds-strict | 100 ops | 75.8 µs (53.8 %) | 57.4 µs | 60.5 µs | 64.9 µs | 175.2 µs | 214.7 µs | 12,940 ops/s | 24.1 µs | 14.86 µs | 40.2 µs | 0 errors |
| qps-bar-range-warm | pg | 100 ops | 101.9 µs (17.3 %) | 93.1 µs | 94.8 µs | 95.9 µs | 114.6 µs | 262.9 µs | 9,664 ops/s | 71.5 µs | 7.47 µs | 46.3 µs | 0 errors |
| qps-bar-range-warm | oracle | 100 ops | 187.9 µs (14.8 %) | 158.6 µs | 161.7 µs | 164.3 µs | 320.0 µs | 394.2 µs | 5,235 ops/s | 135.4 µs | 106.88 µs | 40.4 µs | 0 errors |
| qps-day-slice-cold | kds-group | 100 ops | 698.5 µs (3.2 %) | 644.9 µs | 660.4 µs | 665.7 µs | 797.2 µs | 1,692.7 µs | 1,426 ops/s | 21.7 µs | 3.27 µs | 656.1 µs | 0 errors |
| qps-day-slice-cold | kds-strict | 100 ops | 677.0 µs (2.7 %) | 642.9 µs | 654.6 µs | 663.9 µs | 678.0 µs | 795.6 µs | 1,470 ops/s | 17.7 µs | 3.16 µs | 650.6 µs | 0 errors |
| qps-day-slice-cold | pg | 100 ops | 1,314.9 µs (15.4 %) | 1,202.2 µs | 1,233.0 µs | 1,251.1 µs | 1,539.9 µs | 1,594.3 µs | 759 ops/s | 51.6 µs | 2.21 µs | 1,210.2 µs | 0 errors |
| qps-day-slice-cold | oracle | 100 ops | 472.1 µs (0.9 %) | 419.5 µs | 436.2 µs | 449.1 µs | 680.4 µs | 728.4 µs | 2,099 ops/s | 38.8 µs | 18.73 µs | 409.1 µs | 0 errors |
| qps-day-slice-warm | kds-group | 100 ops | 725.3 µs (11.4 %) | 659.7 µs | 670.5 µs | 674.1 µs | 911.1 µs | 1,695.8 µs | 1,371 ops/s | 32.4 µs | 4.27 µs | 657.4 µs | 0 errors |
| qps-day-slice-warm | kds-strict | 100 ops | 683.7 µs (10.9 %) | 655.5 µs | 665.7 µs | 669.6 µs | 816.1 µs | 848.9 µs | 1,456 ops/s | 25.1 µs | 3.69 µs | 651.1 µs | 0 errors |
| qps-day-slice-warm | pg | 100 ops | 1,474.9 µs (8.2 %) | 1,198.4 µs | 1,402.8 µs | 1,470.7 µs | 1,671.1 µs | 1,710.2 µs | 673 ops/s | 106.9 µs | 3.65 µs | 1,214.1 µs | 0 errors |
| qps-day-slice-warm | oracle | 100 ops | 527.8 µs (5.1 %) | 442.0 µs | 445.4 µs | 451.7 µs | 818.3 µs | 929.1 µs | 1,877 ops/s | 64.7 µs | 26.46 µs | 415.6 µs | 0 errors |
| qps-symbol-history-cold | kds-group | 7 ops | 2,478.7 µs (4.0 %) | 2,162.3 µs | 2,322.5 µs | 2,402.5 µs | 2,775.4 µs | 2,775.4 µs | 396 ops/s | 887.7 µs | 693.05 µs | 2,104.8 µs | 0 errors |
| qps-symbol-history-cold | kds-strict | 7 ops | 2,512.0 µs (8.4 %) | 2,322.3 µs | 2,407.3 µs | 2,478.4 µs | 2,710.5 µs | 2,710.5 µs | 391 ops/s | 852.5 µs | 657.73 µs | 2,177.7 µs | 0 errors |
| qps-symbol-history-cold | pg | 7 ops | 3,942.1 µs (4.7 %) | 3,529.3 µs | 3,643.6 µs | 3,659.2 µs | 4,809.5 µs | 4,809.5 µs | 248 ops/s | 3,007.1 µs | 472.70 µs | 2,672.6 µs | 0 errors |
| qps-symbol-history-cold | oracle | 7 ops | 8,627.4 µs (4.1 %) | 8,069.0 µs | 8,402.5 µs | 8,476.7 µs | 9,006.0 µs | 9,006.0 µs | 115 ops/s | 7,618.8 µs | 6,308.98 µs | 1,902.1 µs | 0 errors |
| qps-symbol-history-warm | kds-group | 100 ops | 2,572.5 µs (5.1 %) | 2,153.3 µs | 2,374.6 µs | 2,493.1 µs | 3,219.8 µs | 3,749.8 µs | 388 ops/s | 835.2 µs | 662.50 µs | 2,228.5 µs | 0 errors |
| qps-symbol-history-warm | kds-strict | 100 ops | 2,589.1 µs (2.6 %) | 2,124.4 µs | 2,380.1 µs | 2,511.3 µs | 3,326.1 µs | 3,565.8 µs | 386 ops/s | 839.1 µs | 665.69 µs | 2,238.0 µs | 0 errors |
| qps-symbol-history-warm | pg | 100 ops | 4,002.6 µs (0.8 %) | 3,572.5 µs | 3,834.9 µs | 4,002.0 µs | 4,254.8 µs | 4,297.8 µs | 249 ops/s | 2,813.9 µs | 399.46 µs | 2,680.2 µs | 0 errors |
| qps-symbol-history-warm | oracle | 100 ops | 8,210.0 µs (1.4 %) | 7,876.3 µs | 8,044.5 µs | 8,133.3 µs | 8,610.1 µs | 8,672.1 µs | 122 ops/s | 7,293.3 µs | 6,050.71 µs | 1,901.6 µs | 0 errors |
| qps-cross-join-cold | kds-group | 100 ops | 715.5 µs (4.2 %) | 669.7 µs | 682.8 µs | 690.4 µs | 827.0 µs | 1,071.1 µs | 1,392 ops/s | 19.1 µs | 2.65 µs | 680.0 µs | 0 errors |
| qps-cross-join-cold | kds-strict | 100 ops | 703.9 µs (7.7 %) | 672.4 µs | 688.6 µs | 692.7 µs | 771.8 µs | 1,052.5 µs | 1,416 ops/s | 18.3 µs | 2.79 µs | 678.7 µs | 0 errors |
| qps-cross-join-cold | pg | 100 ops | 1,433.5 µs (9.4 %) | 1,229.0 µs | 1,274.7 µs | 1,479.3 µs | 1,661.0 µs | 1,708.5 µs | 696 ops/s | 93.2 µs | 3.78 µs | 1,247.3 µs | 0 errors |
| qps-cross-join-cold | oracle | 100 ops | 534.2 µs (16.6 %) | 428.8 µs | 498.4 µs | 507.5 µs | 735.3 µs | 801.0 µs | 1,859 ops/s | 43.2 µs | 19.96 µs | 462.1 µs | 0 errors |
| qps-cross-join-warm | kds-group | 100 ops | 693.1 µs (1.1 %) | 675.5 µs | 686.8 µs | 691.9 µs | 701.6 µs | 713.7 µs | 1,436 ops/s | 20.3 µs | 3.05 µs | 661.0 µs | 0 errors |
| qps-cross-join-warm | kds-strict | 100 ops | 725.7 µs (3.3 %) | 676.6 µs | 689.5 µs | 693.2 µs | 873.8 µs | 899.8 µs | 1,370 ops/s | 30.4 µs | 3.80 µs | 662.1 µs | 0 errors |
| qps-cross-join-warm | pg | 100 ops | 1,288.5 µs (22.9 %) | 1,233.7 µs | 1,254.5 µs | 1,266.4 µs | 1,452.0 µs | 1,504.4 µs | 774 ops/s | 27.4 µs | 1.76 µs | 1,236.6 µs | 0 errors |
| qps-cross-join-warm | oracle | 100 ops | 601.1 µs (13.1 %) | 427.0 µs | 493.4 µs | 505.7 µs | 898.7 µs | 918.2 µs | 1,648 ops/s | 72.6 µs | 27.77 µs | 459.9 µs | 0 errors |
| qps-point-join-cold | kds-group | 100 ops | 34.3 µs (56.0 %) | 31.6 µs | 32.5 µs | 32.9 µs | 37.5 µs | 47.2 µs | 26,828 ops/s | 15.8 µs | 0.48 µs | 9.7 µs | 0 errors |
| qps-point-join-cold | kds-strict | 100 ops | 35.3 µs (17.9 %) | 32.0 µs | 33.0 µs | 33.5 µs | 42.5 µs | 51.9 µs | 25,639 ops/s | 16.4 µs | 0.55 µs | 9.9 µs | 0 errors |
| qps-point-join-cold | pg | 100 ops | 68.3 µs (69.3 %) | 42.4 µs | 43.5 µs | 44.0 µs | 236.0 µs | 395.0 µs | 13,915 ops/s | 29.3 µs | 0.42 µs | 13.8 µs | 0 errors |
| qps-point-join-cold | oracle | 100 ops | 146.1 µs (68.9 %) | 35.9 µs | 36.8 µs | 39.4 µs | 381.8 µs | 444.0 µs | 6,597 ops/s | 41.1 µs | 5.92 µs | 22.5 µs | 0 errors |
| qps-point-join-warm | kds-group | 100 ops | 30.6 µs (18.1 %) | 24.0 µs | 24.4 µs | 24.6 µs | 34.3 µs | 40.0 µs | 30,717 ops/s | 10.6 µs | 0.33 µs | 9.2 µs | 0 errors |
| qps-point-join-warm | kds-strict | 100 ops | 30.6 µs (31.5 %) | 23.2 µs | 23.6 µs | 23.9 µs | 45.2 µs | 90.7 µs | 28,286 ops/s | 12.0 µs | 0.39 µs | 8.9 µs | 0 errors |
| qps-point-join-warm | pg | 100 ops | 44.0 µs (113.2 %) | 21.8 µs | 22.1 µs | 22.2 µs | 55.1 µs | 61.1 µs | 21,098 ops/s | 17.2 µs | 0.27 µs | 8.8 µs | 0 errors |
| qps-point-join-warm | oracle | 100 ops | 44.0 µs (64.6 %) | 23.5 µs | 27.3 µs | 30.2 µs | 46.8 µs | 322.9 µs | 21,181 ops/s | 22.3 µs | 3.12 µs | 9.2 µs | 0 errors |
| qps-model-join-cold | kds-group | 7 ops | 218.1 µs (49.0 %) | 144.7 µs | 222.4 µs | 223.2 µs | 273.5 µs | 273.5 µs | 4,095 ops/s | 113.1 µs | 62.96 µs | 78.0 µs | 0 errors |
| qps-model-join-cold | kds-strict | 7 ops | 335.7 µs (37.8 %) | 191.1 µs | 313.4 µs | 340.2 µs | 423.0 µs | 423.0 µs | 2,595 ops/s | 119.7 µs | 62.73 µs | 101.1 µs | 0 errors |
| qps-model-join-cold | pg | 7 ops | 354.4 µs (19.8 %) | 266.2 µs | 340.1 µs | 362.4 µs | 411.6 µs | 411.6 µs | 2,568 ops/s | 154.4 µs | 16.69 µs | 125.7 µs | 0 errors |
| qps-model-join-cold | oracle | 7 ops | 418.9 µs (14.6 %) | 328.9 µs | 344.3 µs | 393.6 µs | 671.5 µs | 671.5 µs | 2,283 ops/s | 181.9 µs | 115.09 µs | 98.7 µs | 0 errors |
| qps-model-join-warm | kds-group | 100 ops | 129.4 µs (8.0 %) | 111.0 µs | 114.9 µs | 119.3 µs | 233.8 µs | 269.7 µs | 7,643 ops/s | 39.8 µs | 24.94 µs | 77.1 µs | 0 errors |
| qps-model-join-warm | kds-strict | 100 ops | 146.4 µs (5.9 %) | 114.8 µs | 128.0 µs | 132.6 µs | 254.0 µs | 284.6 µs | 6,744 ops/s | 48.3 µs | 28.84 µs | 77.6 µs | 0 errors |
| qps-model-join-warm | pg | 100 ops | 185.9 µs (12.6 %) | 162.1 µs | 164.8 µs | 166.2 µs | 334.5 µs | 357.5 µs | 5,317 ops/s | 120.3 µs | 13.39 µs | 100.8 µs | 0 errors |
| qps-model-join-warm | oracle | 100 ops | 265.2 µs (4.7 %) | 185.2 µs | 190.4 µs | 197.0 µs | 453.1 µs | 531.2 µs | 3,741 ops/s | 143.9 µs | 107.76 µs | 66.6 µs | 0 errors |
| write-batch-1 | kds-group | 2,000 ops | 215.2 µs (1.3 %) | 175.4 µs | 197.0 µs | 201.8 µs | 275.8 µs | 445.2 µs | 4,642 ops/s | 13.8 µs | 0.00 µs | 15.5 µs | 0 errors |
| write-batch-1 | kds-strict | 2,000 ops | 214.4 µs (3.3 %) | 174.7 µs | 196.4 µs | 201.8 µs | 279.5 µs | 462.4 µs | 4,659 ops/s | 12.8 µs | 0.00 µs | 17.3 µs | 0 errors |
| write-batch-1 | pg | 2,000 ops | 150.3 µs (5.5 %) | 125.4 µs | 135.5 µs | 139.0 µs | 180.8 µs | 340.7 µs | 6,644 ops/s | 10.9 µs | 0.00 µs | 10.6 µs | 0 errors |
| write-batch-1 | oracle | 2,000 ops | 281.9 µs (43.7 %) | 140.5 µs | 168.9 µs | 201.4 µs | 534.3 µs | 724.4 µs | 3,542 ops/s | 20.5 µs | 0.00 µs | 59.5 µs | 0 errors |
| write-batch-10 | kds-group | 2,000 ops | 37.4 µs (3.0 %) | 12.6 µs | 14.1 µs | 15.0 µs | 193.6 µs | 226.4 µs | 26,622 ops/s | 5.1 µs | 0.00 µs | 9.3 µs | 0 errors |
| write-batch-10 | kds-strict | 2,000 ops | 36.0 µs (7.2 %) | 12.5 µs | 14.0 µs | 14.4 µs | 194.2 µs | 224.4 µs | 27,581 ops/s | 4.8 µs | 0.00 µs | 9.1 µs | 0 errors |
| write-batch-10 | pg | 2,000 ops | 29.2 µs (3.9 %) | 12.0 µs | 12.9 µs | 13.4 µs | 140.3 µs | 164.7 µs | 34,040 ops/s | 6.5 µs | 0.00 µs | 6.1 µs | 0 errors |
| write-batch-10 | oracle | 2,000 ops | 37.0 µs (7.3 %) | 13.8 µs | 14.6 µs | 15.5 µs | 180.5 µs | 222.2 µs | 26,899 ops/s | 7.2 µs | 0.00 µs | 10.5 µs | 0 errors |
| write-batch-100 | kds-group | 2,000 ops | 16.9 µs (5.5 %) | 12.5 µs | 13.9 µs | 14.2 µs | 19.9 µs | 167.2 µs | 58,866 ops/s | 2.8 µs | 0.00 µs | 8.0 µs | 0 errors |
| write-batch-100 | kds-strict | 2,000 ops | 16.9 µs (5.4 %) | 12.6 µs | 14.0 µs | 14.2 µs | 17.2 µs | 161.0 µs | 58,841 ops/s | 2.8 µs | 0.00 µs | 8.0 µs | 0 errors |
| write-batch-100 | pg | 2,000 ops | 14.9 µs (5.4 %) | 11.9 µs | 12.7 µs | 13.3 µs | 14.2 µs | 137.7 µs | 66,431 ops/s | 4.6 µs | 0.00 µs | 5.0 µs | 0 errors |
| write-batch-100 | oracle | 2,000 ops | 19.3 µs (4.2 %) | 14.0 µs | 14.5 µs | 15.2 µs | 18.8 µs | 240.2 µs | 51,503 ops/s | 4.7 µs | 0.00 µs | 8.2 µs | 0 errors |
| write-batch-1000 | kds-group | 2,000 ops | 14.9 µs (4.8 %) | 12.6 µs | 13.9 µs | 14.2 µs | 15.2 µs | 22.0 µs | 66,542 ops/s | 2.7 µs | 0.00 µs | 8.2 µs | 0 errors |
| write-batch-1000 | kds-strict | 2,000 ops | 15.1 µs (1.7 %) | 12.8 µs | 14.2 µs | 14.4 µs | 15.2 µs | 30.4 µs | 65,616 ops/s | 2.6 µs | 0.00 µs | 8.1 µs | 0 errors |
| write-batch-1000 | pg | 2,000 ops | 14.8 µs (12.6 %) | 11.9 µs | 13.2 µs | 13.3 µs | 13.8 µs | 28.6 µs | 67,002 ops/s | 4.4 µs | 0.00 µs | 4.9 µs | 0 errors |
| write-batch-1000 | oracle | 2,000 ops | 18.4 µs (329.9 %) | 14.1 µs | 14.8 µs | 15.6 µs | 18.7 µs | 45.4 µs | 54,179 ops/s | 4.7 µs | 0.00 µs | 8.3 µs | 0 errors |
| join-conn-1 | kds-group | 200 ops | 699.0 µs (1.0 %) | 668.4 µs | 687.4 µs | 692.2 µs | 780.4 µs | 830.4 µs | 1,428 ops/s | 16.8 µs | 2.52 µs | 664.2 µs | 0 errors |
| join-conn-1 | kds-strict | 200 ops | 697.2 µs (2.0 %) | 665.6 µs | 684.2 µs | 691.9 µs | 755.8 µs | 813.9 µs | 1,430 ops/s | 16.9 µs | 2.54 µs | 663.0 µs | 0 errors |
| join-conn-1 | pg | 200 ops | 1,342.1 µs (24.8 %) | 1,231.6 µs | 1,261.7 µs | 1,277.6 µs | 1,614.8 µs | 1,653.5 µs | 744 ops/s | 42.4 µs | 2.41 µs | 1,242.9 µs | 0 errors |
| join-conn-1 | oracle | 200 ops | 527.7 µs (10.3 %) | 418.9 µs | 494.5 µs | 499.0 µs | 716.5 µs | 942.1 µs | 1,882 ops/s | 49.7 µs | 23.24 µs | 458.7 µs | 0 errors |
| join-conn-2 | kds-group | 200 ops | 1,312.8 µs (1.0 %) | 692.0 µs | 1,308.7 µs | 1,315.0 µs | 1,443.7 µs | 1,541.9 µs | 1,503 ops/s | 18.4 µs | 2.66 µs | 656.6 µs | 0 errors |
| join-conn-2 | kds-strict | 200 ops | 1,325.8 µs (0.4 %) | 707.7 µs | 1,312.6 µs | 1,326.8 µs | 1,556.2 µs | 1,742.8 µs | 1,471 ops/s | 28.5 µs | 3.59 µs | 661.9 µs | 0 errors |
| join-conn-2 | pg | 200 ops | 1,440.2 µs (13.5 %) | 1,230.7 µs | 1,283.7 µs | 1,435.6 µs | 1,690.0 µs | 1,720.7 µs | 1,364 ops/s | 80.6 µs | 3.56 µs | 1,241.5 µs | 0 errors |
| join-conn-2 | oracle | 200 ops | 554.5 µs (6.1 %) | 491.2 µs | 508.2 µs | 512.6 µs | 811.2 µs | 908.8 µs | 3,477 ops/s | 52.5 µs | 24.10 µs | 470.2 µs | 0 errors |
| join-conn-4 | kds-group | 200 ops | 2,593.0 µs (0.6 %) | 1,312.9 µs | 2,550.2 µs | 2,610.7 µs | 2,773.6 µs | 2,812.4 µs | 1,511 ops/s | 47.3 µs | 4.82 µs | 656.4 µs | 0 errors |
| join-conn-4 | kds-strict | 200 ops | 2,626.7 µs (3.5 %) | 1,805.1 µs | 2,559.6 µs | 2,634.3 µs | 2,759.0 µs | 2,787.8 µs | 1,502 ops/s | 68.9 µs | 6.60 µs | 660.7 µs | 0 errors |
| join-conn-4 | pg | 200 ops | 1,470.2 µs (13.3 %) | 1,230.8 µs | 1,348.8 µs | 1,474.2 µs | 1,700.7 µs | 1,797.6 µs | 2,577 ops/s | 64.5 µs | 3.35 µs | 1,251.7 µs | 0 errors |
| join-conn-4 | oracle | 200 ops | 638.0 µs (5.3 %) | 485.6 µs | 512.3 µs | 559.5 µs | 849.5 µs | 1,428.5 µs | 5,746 ops/s | 52.4 µs | 21.23 µs | 499.6 µs | 0 errors |
| join-conn-8 | kds-group | 200 ops | 5,234.9 µs (2.0 %) | 4,119.3 µs | 5,189.7 µs | 5,216.8 µs | 5,416.6 µs | 5,576.6 µs | 1,513 ops/s | 72.6 µs | 6.70 µs | 653.6 µs | 0 errors |
| join-conn-8 | kds-strict | 200 ops | 5,218.2 µs (3.0 %) | 3,920.9 µs | 5,176.0 µs | 5,214.7 µs | 5,351.0 µs | 5,410.1 µs | 1,522 ops/s | 65.6 µs | 6.36 µs | 651.4 µs | 0 errors |
| join-conn-8 | pg | 200 ops | 1,522.4 µs (10.7 %) | 1,244.3 µs | 1,340.2 µs | 1,520.2 µs | 1,999.1 µs | 2,167.4 µs | 4,832 ops/s | 76.8 µs | 3.77 µs | 1,303.3 µs | 0 errors |
| join-conn-8 | oracle | 200 ops | 803.2 µs (4.4 %) | 480.5 µs | 520.3 µs | 733.3 µs | 1,626.4 µs | 2,735.1 µs | 8,804 ops/s | 61.0 µs | 18.97 µs | 523.9 µs | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| load-exchanges | kds-group | - | - | - | 12.50 µs | 0.00 µs | 150.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 150.0 µs); cpu.on_cpu (0.00, 12.5 µs); sched.run_queue (1.00, 0.0 µs) |
| load-exchanges | kds-strict | - | - | - | 14.00 µs | 0.00 µs | 148.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 148.0 µs); cpu.on_cpu (0.00, 14.0 µs); sched.run_queue (1.00, 0.0 µs) |
| load-exchanges | pg | - | 0.00 µs | 0.00 µs | 2.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| load-exchanges | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 41.00 µs | 0.00 µs | 338.50 µs | 0.00 µs | 1.50 µs | 0.00 µs | log file sync (1.00, 338.5 µs); SQL*Net message to client (1.00, 1.5 µs) |
| load-symbols | kds-group | - | - | - | 6.50 µs | 0.00 µs | 123.75 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 123.8 µs); cpu.on_cpu (0.00, 6.4 µs); sched.run_queue (1.00, 0.1 µs) |
| load-symbols | kds-strict | - | - | - | 6.12 µs | 0.00 µs | 120.75 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 120.8 µs); cpu.on_cpu (0.00, 6.0 µs); sched.run_queue (1.00, 0.1 µs) |
| load-symbols | pg | - | 0.00 µs | 0.00 µs | 1.62 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| load-symbols | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 23.38 µs | 0.00 µs | 206.12 µs | 0.00 µs | 0.88 µs | 0.00 µs | log file sync (1.00, 206.1 µs); SQL*Net message to client (1.00, 0.9 µs) |
| load-sessions | kds-group | - | - | - | 4.38 µs | 0.00 µs | 0.86 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.2 µs); commit.wal_flush (0.01, 0.9 µs); sched.run_queue (1.01, 0.2 µs) |
| load-sessions | kds-strict | - | - | - | 4.33 µs | 0.00 µs | 0.79 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.2 µs); commit.wal_flush (0.01, 0.8 µs); sched.run_queue (1.01, 0.2 µs) |
| load-sessions | pg | - | 0.00 µs | 0.00 µs | 1.08 µs | 0.00 µs | 0.39 µs | 0.00 µs | 1.68 µs | 0.00 µs | ClientRead (0.00, 1.7 µs); WalSync (0.00, 0.4 µs); DataFileExtend (0.00, 0.1 µs) |
| load-sessions | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.75 µs | 0.00 µs | 1.61 µs | 0.00 µs | 0.19 µs | 0.00 µs | log file sync (0.01, 1.6 µs); SQL*Net message to client (1.01, 0.2 µs); log file switch (checkpoint incomplete) (0.00, 0.0 µs); log file switch completion (0.00, 0.0 µs) |
| load-bars | kds-group | - | - | - | 2.47 µs | 0.00 µs | 0.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 2.4 µs); commit.wal_flush (0.01, 0.8 µs); sched.run_queue (1.01, 0.1 µs) |
| load-bars | kds-strict | - | - | - | 2.46 µs | 0.00 µs | 0.81 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 2.4 µs); commit.wal_flush (0.01, 0.8 µs); sched.run_queue (1.01, 0.1 µs) |
| load-bars | pg | - | 0.00 µs | 0.00 µs | 1.22 µs | 0.00 µs | 0.82 µs | 0.00 µs | 1.89 µs | 0.00 µs | ClientRead (0.00, 1.9 µs); CPU (0.00, 1.2 µs); WalSync (0.00, 0.7 µs); WalInitSync (0.00, 0.0 µs) |
| load-bars | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 9.31 µs | 0.07 µs | 1.47 µs | 0.00 µs | 0.20 µs | 0.00 µs | log file switch (checkpoint incomplete) (0.00, 5.2 µs); log file sync (0.01, 1.5 µs); SQL*Net message to client (1.01, 0.2 µs); log file switch completion (0.00, 0.1 µs) |
| load-stats | kds-group | - | - | - | 4.48 µs | 0.00 µs | 1.11 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.4 µs); commit.wal_flush (0.01, 1.1 µs); sched.run_queue (1.01, 0.1 µs) |
| load-stats | kds-strict | - | - | - | 4.51 µs | 0.00 µs | 1.09 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.5 µs); commit.wal_flush (0.01, 1.1 µs); sched.run_queue (1.01, 0.0 µs) |
| load-stats | pg | - | 0.00 µs | 0.00 µs | 1.30 µs | 0.00 µs | 0.77 µs | 0.00 µs | 1.99 µs | 0.00 µs | ClientRead (0.00, 2.0 µs); CPU (0.00, 1.0 µs); WalSync (0.00, 0.7 µs); WalInitSync (0.00, 0.1 µs) |
| load-stats | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 9.74 µs | 0.09 µs | 1.43 µs | 0.00 µs | 0.21 µs | 0.00 µs | log file switch (checkpoint incomplete) (0.00, 5.4 µs); log file sync (0.01, 1.4 µs); SQL*Net message to client (1.01, 0.2 µs); log file switch completion (0.00, 0.2 µs) |
| load-models | kds-group | - | - | - | 3.38 µs | 0.00 µs | 182.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 182.0 µs); cpu.on_cpu (0.00, 3.4 µs); sched.run_queue (1.00, 0.1 µs) |
| load-models | kds-strict | - | - | - | 3.50 µs | 0.00 µs | 178.12 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 178.1 µs); cpu.on_cpu (0.00, 3.4 µs); sched.run_queue (1.00, 0.1 µs) |
| load-models | pg | - | 0.00 µs | 0.00 µs | 1.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| load-models | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 17.62 µs | 0.00 µs | 186.75 µs | 0.00 µs | 1.00 µs | 0.00 µs | log file sync (1.00, 186.8 µs); SQL*Net message to client (1.00, 1.0 µs) |
| backtest-read | kds-group | - | - | 14.35 µs | 642.41 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 6.47 µs | cpu.on_cpu (0.00, 653.7 µs); sched.run_queue (14.00, 3.0 µs) |
| backtest-read | kds-strict | - | - | 14.39 µs | 640.91 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 6.40 µs | cpu.on_cpu (0.00, 652.3 µs); sched.run_queue (14.00, 3.0 µs) |
| backtest-read | pg | - | 0.00 µs | 0.00 µs | 1,292.96 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.14 µs | 6.42 µs | CPU (1.24, 1,313.5 µs); ClientRead (0.00, 3.1 µs) |
| backtest-read | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 435.81 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.87 µs | 28.00 µs | SQL*Net message to client (1.00, 0.9 µs) |
| result-insert | kds-group | - | - | 0.00 µs | 6.25 µs | 0.00 µs | 183.12 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 183.1 µs); cpu.on_cpu (0.00, 6.2 µs); sched.run_queue (1.00, 0.1 µs) |
| result-insert | kds-strict | - | - | 0.00 µs | 6.52 µs | 0.00 µs | 184.88 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 184.9 µs); cpu.on_cpu (0.00, 6.4 µs); sched.run_queue (1.00, 0.1 µs) |
| result-insert | pg | - | 0.00 µs | 0.00 µs | 1.74 µs | 0.00 µs | 120.27 µs | 0.00 µs | 4.12 µs | 0.00 µs | WalSync (0.11, 120.3 µs); ClientRead (0.00, 4.1 µs); CPU (0.00, 3.1 µs) |
| result-insert | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 14.43 µs | 0.00 µs | 171.28 µs | 0.00 µs | 0.72 µs | 0.00 µs | log file sync (1.00, 171.3 µs); SQL*Net message to client (1.00, 0.7 µs) |
| backtest-replay | kds-group | - | - | 16.89 µs | 630.25 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.97 µs | cpu.on_cpu (0.00, 644.9 µs); sched.run_queue (14.00, 3.0 µs) |
| backtest-replay | kds-strict | - | - | 18.10 µs | 641.87 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.09 µs | cpu.on_cpu (0.00, 657.2 µs); sched.run_queue (14.00, 3.0 µs) |
| backtest-replay | pg | - | 0.00 µs | 0.00 µs | 1,290.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 11.48 µs | 3.82 µs | CPU (1.25, 1,319.0 µs); ClientRead (0.01, 11.5 µs) |
| backtest-replay | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 445.98 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.86 µs | 25.22 µs | SQL*Net message to client (1.00, 0.9 µs) |
| compare-all | kds-group | - | - | 15.00 µs | 933.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 216.67 µs | cpu.on_cpu (0.00, 948.0 µs); sched.run_queue (1.00, 0.0 µs) |
| compare-all | kds-strict | - | - | 18.00 µs | 985.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 214.72 µs | cpu.on_cpu (0.00, 1,003.0 µs); sched.run_queue (1.00, 0.0 µs) |
| compare-all | pg | - | 0.00 µs | 0.00 µs | 477.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 330.28 µs | none |
| compare-all | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 460.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 34.00 µs | 1,232.61 µs | SQL*Net more data to client (8.00, 21.0 µs); events in waitclass Other (3.00, 14.0 µs); SQL*Net message to client (1.00, 13.0 µs) |
| compare-one | kds-group | - | - | 4.97 µs | 67.91 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 23.05 µs | cpu.on_cpu (0.00, 72.8 µs); sched.run_queue (1.00, 0.1 µs) |
| compare-one | kds-strict | - | - | 4.44 µs | 67.88 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 23.03 µs | cpu.on_cpu (0.00, 72.2 µs); sched.run_queue (1.00, 0.1 µs) |
| compare-one | pg | - | 0.00 µs | 0.00 µs | 104.94 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 14.12 µs | CPU (0.06, 60.8 µs) |
| compare-one | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 58.97 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.31 µs | 110.65 µs | SQL*Net message to client (1.00, 1.3 µs) |
| read-bar-lookup | kds-group | - | - | 0.91 µs | 2.02 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.10 µs | cpu.on_cpu (0.00, 2.9 µs); sched.run_queue (1.00, 0.1 µs) |
| read-bar-lookup | kds-strict | - | - | 0.90 µs | 2.02 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.11 µs | cpu.on_cpu (0.00, 2.9 µs); sched.run_queue (1.00, 0.1 µs) |
| read-bar-lookup | pg | - | 0.00 µs | 0.00 µs | 1.81 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.26 µs | 0.06 µs | ClientRead (0.01, 5.3 µs) |
| read-bar-lookup | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.36 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 1.52 µs | SQL*Net message to client (1.00, 0.2 µs) |
| read-bar-range | kds-group | - | - | 1.01 µs | 34.02 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 16.63 µs | cpu.on_cpu (0.00, 35.0 µs); sched.run_queue (1.00, 0.1 µs) |
| read-bar-range | kds-strict | - | - | 1.03 µs | 35.26 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 12.41 µs | cpu.on_cpu (0.00, 36.2 µs); sched.run_queue (1.00, 0.1 µs) |
| read-bar-range | pg | - | 0.00 µs | 0.00 µs | 45.45 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.39 µs | 7.39 µs | CPU (0.03, 31.3 µs); ClientRead (0.01, 10.4 µs) |
| read-bar-range | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 42.72 µs | 0.00 µs | 0.00 µs | 0.00 µs | 7.58 µs | 109.25 µs | SQL*Net message to client (1.00, 7.6 µs) |
| read-symbol-history | kds-group | - | - | 7.20 µs | 2,095.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 681.46 µs | cpu.on_cpu (0.00, 2,097.4 µs); sched.run_queue (14.00, 5.0 µs) |
| read-symbol-history | kds-strict | - | - | 8.50 µs | 2,074.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 657.14 µs | cpu.on_cpu (0.00, 2,077.5 µs); sched.run_queue (14.00, 6.0 µs) |
| read-symbol-history | pg | - | 0.00 µs | 0.00 µs | 2,838.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 470.04 µs | CPU (2.70, 2,757.5 µs) |
| read-symbol-history | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 1,914.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 143.40 µs | 6,328.58 µs | SQL*Net more data to client (44.70, 121.5 µs); SQL*Net message to client (1.00, 21.1 µs) |
| read-day-slice | kds-group | - | - | 1.91 µs | 628.29 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.11 µs | cpu.on_cpu (0.00, 627.6 µs); sched.run_queue (14.00, 2.9 µs) |
| read-day-slice | kds-strict | - | - | 1.95 µs | 632.11 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.28 µs | cpu.on_cpu (0.00, 631.4 µs); sched.run_queue (14.00, 3.0 µs) |
| read-day-slice | pg | - | 0.00 µs | 0.00 µs | 1,204.96 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.62 µs | CPU (1.15, 1,222.4 µs) |
| read-day-slice | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 391.96 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.78 µs | 20.47 µs | SQL*Net message to client (1.00, 0.8 µs) |
| read-join-point | kds-group | - | - | 1.95 µs | 3.11 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.48 µs | cpu.on_cpu (0.00, 5.0 µs); sched.run_queue (1.00, 0.1 µs) |
| read-join-point | kds-strict | - | - | 2.00 µs | 3.13 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.46 µs | cpu.on_cpu (0.00, 5.1 µs); sched.run_queue (1.00, 0.1 µs) |
| read-join-point | pg | - | 0.00 µs | 0.00 µs | 2.49 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.08 µs | none |
| read-join-point | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.84 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.28 µs | 1.38 µs | SQL*Net message to client (1.00, 0.3 µs) |
| read-join-exists | kds-group | - | - | 8.50 µs | 1,908.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.73 µs | cpu.on_cpu (0.00, 1,916.6 µs); sched.run_queue (1.00, 0.5 µs) |
| read-join-exists | kds-strict | - | - | 3.00 µs | 1,927.90 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.21 µs | cpu.on_cpu (0.00, 1,930.8 µs); sched.run_queue (1.00, 0.1 µs) |
| read-join-exists | pg | - | 0.00 µs | 0.00 µs | 4,041.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.28 µs | CPU (3.90, 4,082.2 µs) |
| read-join-exists | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 570.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.10 µs | 7.47 µs | events in waitclass Other (3.00, 6.6 µs); SQL*Net message to client (1.00, 0.1 µs) |
| agg-global | kds-group | - | - | 4.70 µs | 1,717.70 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.11 µs | cpu.on_cpu (0.00, 1,719.3 µs); sched.run_queue (11.00, 3.2 µs) |
| agg-global | kds-strict | - | - | 5.00 µs | 1,727.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.08 µs | cpu.on_cpu (0.00, 1,729.7 µs); sched.run_queue (11.00, 3.1 µs) |
| agg-global | pg | - | 0.00 µs | 0.00 µs | 1,907.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.62 µs | CPU (1.70, 1,752.9 µs) |
| agg-global | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 1,261.40 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.40 µs | 4.48 µs | SQL*Net message to client (1.00, 2.4 µs) |
| agg-by-symbol | kds-group | - | - | 4.00 µs | 3,471.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.22 µs | cpu.on_cpu (0.00, 3,471.3 µs); sched.run_queue (11.00, 3.7 µs) |
| agg-by-symbol | kds-strict | - | - | 5.90 µs | 3,465.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.63 µs | cpu.on_cpu (0.00, 3,463.4 µs); sched.run_queue (11.00, 4.4 µs) |
| agg-by-symbol | pg | - | 0.00 µs | 0.00 µs | 3,155.90 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.03 µs | CPU (2.80, 2,931.2 µs) |
| agg-by-symbol | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 1,547.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.10 µs | 23.40 µs | SQL*Net message to client (1.00, 3.1 µs) |
| agg-by-session | kds-group | - | - | 9.90 µs | 4,769.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 497.91 µs | cpu.on_cpu (0.00, 4,666.2 µs); sched.run_queue (11.00, 112.9 µs) |
| agg-by-session | kds-strict | - | - | 8.30 µs | 4,659.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 482.88 µs | cpu.on_cpu (0.00, 4,624.9 µs); sched.run_queue (11.00, 108.7 µs) |
| agg-by-session | pg | - | 0.00 µs | 0.00 µs | 4,670.40 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 221.07 µs | CPU (4.30, 4,451.1 µs) |
| agg-by-session | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2,491.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 51.60 µs | 1,302.60 µs | SQL*Net more data to client (14.00, 36.2 µs); events in waitclass Other (9.00, 30.6 µs); SQL*Net message to client (1.00, 15.4 µs) |
| agg-day-slice | kds-group | - | - | 2.27 µs | 643.38 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.02 µs | cpu.on_cpu (0.00, 627.2 µs); sched.run_queue (14.00, 18.4 µs) |
| agg-day-slice | kds-strict | - | - | 2.12 µs | 639.21 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.15 µs | cpu.on_cpu (0.00, 624.8 µs); sched.run_queue (14.00, 12.9 µs) |
| agg-day-slice | pg | - | 0.00 µs | 0.00 µs | 1,180.05 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.36 µs | CPU (1.12, 1,190.4 µs) |
| agg-day-slice | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 419.59 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.78 µs | 10.59 µs | SQL*Net message to client (1.00, 0.8 µs) |
| agg-distinct | kds-group | - | - | 3.70 µs | 2,739.30 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.59 µs | cpu.on_cpu (0.00, 2,716.1 µs); sched.run_queue (11.00, 3.3 µs) |
| agg-distinct | kds-strict | - | - | 2.70 µs | 2,746.30 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.13 µs | cpu.on_cpu (0.00, 2,662.8 µs); sched.run_queue (11.00, 102.9 µs) |
| agg-distinct | pg | - | 0.00 µs | 0.00 µs | 1,977.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.82 µs | CPU (1.80, 1,920.0 µs) |
| agg-distinct | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 863.60 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.80 µs | 9.54 µs | SQL*Net message to client (1.00, 2.8 µs) |
| qps-bar-lookup-cold | kds-group | - | - | 0.94 µs | 1.86 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.64 µs | cpu.on_cpu (0.00, 2.7 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-lookup-cold | kds-strict | - | - | 0.93 µs | 1.84 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.61 µs | cpu.on_cpu (0.00, 2.7 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-lookup-cold | pg | - | 0.00 µs | 0.00 µs | 2.72 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.37 µs | none |
| qps-bar-lookup-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.14 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.31 µs | 4.92 µs | SQL*Net message to client (1.00, 0.3 µs) |
| qps-bar-lookup-warm | kds-group | - | - | 0.93 µs | 1.66 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.48 µs | cpu.on_cpu (0.00, 2.5 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-lookup-warm | kds-strict | - | - | 0.92 µs | 1.68 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.48 µs | cpu.on_cpu (0.00, 2.5 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-lookup-warm | pg | - | 0.00 µs | 0.00 µs | 1.06 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.22 µs | none |
| qps-bar-lookup-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.32 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.32 µs | 3.03 µs | SQL*Net message to client (1.00, 0.3 µs) |
| qps-bar-range-cold | kds-group | - | - | 1.03 µs | 33.91 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 29.92 µs | cpu.on_cpu (0.00, 34.7 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-range-cold | kds-strict | - | - | 1.03 µs | 35.48 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 28.73 µs | cpu.on_cpu (0.00, 34.6 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-range-cold | pg | - | 0.00 µs | 0.00 µs | 48.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 11.02 µs | CPU (0.02, 20.1 µs) |
| qps-bar-range-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 37.02 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.44 µs | 107.79 µs | SQL*Net message to client (1.00, 5.4 µs) |
| qps-bar-range-warm | kds-group | - | - | 1.03 µs | 33.62 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 14.75 µs | cpu.on_cpu (0.00, 34.6 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-range-warm | kds-strict | - | - | 1.04 µs | 33.51 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 14.86 µs | cpu.on_cpu (0.00, 34.5 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-bar-range-warm | pg | - | 0.00 µs | 0.00 µs | 42.84 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.74 µs | 7.55 µs | CPU (0.03, 23.8 µs); ClientRead (0.01, 10.7 µs) |
| qps-bar-range-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 35.55 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.17 µs | 106.88 µs | SQL*Net message to client (1.00, 5.2 µs) |
| qps-day-slice-cold | kds-group | - | - | 1.95 µs | 647.75 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.27 µs | cpu.on_cpu (0.00, 629.7 µs); sched.run_queue (14.00, 23.7 µs) |
| qps-day-slice-cold | kds-strict | - | - | 1.93 µs | 631.43 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.16 µs | cpu.on_cpu (0.00, 630.5 µs); sched.run_queue (14.00, 13.0 µs) |
| qps-day-slice-cold | pg | - | 0.00 µs | 0.00 µs | 1,185.62 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.26 µs | CPU (1.14, 1,206.0 µs) |
| qps-day-slice-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 396.02 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.92 µs | 18.73 µs | SQL*Net message to client (1.00, 0.9 µs) |
| qps-day-slice-warm | kds-group | - | - | 2.16 µs | 648.42 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.27 µs | cpu.on_cpu (0.00, 633.1 µs); sched.run_queue (14.00, 23.7 µs) |
| qps-day-slice-warm | kds-strict | - | - | 1.94 µs | 642.17 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.69 µs | cpu.on_cpu (0.00, 630.8 µs); sched.run_queue (14.00, 13.3 µs) |
| qps-day-slice-warm | pg | - | 0.00 µs | 0.00 µs | 1,177.29 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.48 µs | CPU (1.09, 1,147.2 µs) |
| qps-day-slice-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 400.90 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.15 µs | 26.46 µs | SQL*Net message to client (1.00, 1.1 µs) |
| qps-symbol-history-cold | kds-group | - | - | 8.14 µs | 2,096.57 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 693.05 µs | cpu.on_cpu (0.00, 2,099.6 µs); sched.run_queue (14.00, 5.1 µs) |
| qps-symbol-history-cold | kds-strict | - | - | 7.43 µs | 2,171.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 657.73 µs | cpu.on_cpu (0.00, 2,099.6 µs); sched.run_queue (14.00, 4.9 µs) |
| qps-symbol-history-cold | pg | - | 0.00 µs | 0.00 µs | 2,836.43 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 485.07 µs | CPU (2.29, 2,359.4 µs) |
| qps-symbol-history-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 1,833.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 146.43 µs | 6,308.98 µs | SQL*Net more data to client (46.00, 127.9 µs); SQL*Net message to client (1.00, 19.7 µs) |
| qps-symbol-history-warm | kds-group | - | - | 6.97 µs | 2,174.91 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 662.50 µs | cpu.on_cpu (0.00, 2,096.1 µs); sched.run_queue (14.00, 85.4 µs) |
| qps-symbol-history-warm | kds-strict | - | - | 7.01 µs | 2,190.12 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 665.69 µs | cpu.on_cpu (0.00, 2,109.1 µs); sched.run_queue (14.00, 88.0 µs) |
| qps-symbol-history-warm | pg | - | 0.00 µs | 0.00 µs | 2,826.71 µs | 0.00 µs | 0.00 µs | 0.00 µs | 21.22 µs | 388.01 µs | CPU (2.66, 2,774.0 µs); ClientRead (0.02, 21.2 µs) |
| qps-symbol-history-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 1,841.90 µs | 0.00 µs | 0.00 µs | 0.00 µs | 142.23 µs | 6,050.71 µs | SQL*Net more data to client (46.84, 122.4 µs); SQL*Net message to client (1.00, 19.9 µs) |
| qps-cross-join-cold | kds-group | - | - | 15.19 µs | 657.02 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.65 µs | cpu.on_cpu (0.00, 654.7 µs); sched.run_queue (14.00, 17.4 µs) |
| qps-cross-join-cold | kds-strict | - | - | 14.81 µs | 645.56 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.79 µs | cpu.on_cpu (0.00, 653.7 µs); sched.run_queue (14.00, 6.9 µs) |
| qps-cross-join-cold | pg | - | 0.00 µs | 0.00 µs | 1,293.79 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.80 µs | CPU (1.18, 1,245.6 µs) |
| qps-cross-join-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 449.85 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.97 µs | 19.96 µs | SQL*Net message to client (1.00, 1.0 µs) |
| qps-cross-join-warm | kds-group | - | - | 14.47 µs | 639.97 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.05 µs | cpu.on_cpu (0.00, 651.2 µs); sched.run_queue (14.00, 3.2 µs) |
| qps-cross-join-warm | kds-strict | - | - | 14.69 µs | 639.96 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.80 µs | cpu.on_cpu (0.00, 651.5 µs); sched.run_queue (14.00, 3.2 µs) |
| qps-cross-join-warm | pg | - | 0.00 µs | 0.00 µs | 1,300.71 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.89 µs | CPU (1.26, 1,331.9 µs) |
| qps-cross-join-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 441.72 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.15 µs | 27.77 µs | SQL*Net message to client (1.00, 1.1 µs) |
| qps-point-join-cold | kds-group | - | - | 2.00 µs | 2.93 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.48 µs | cpu.on_cpu (0.00, 4.9 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-point-join-cold | kds-strict | - | - | 1.97 µs | 2.95 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.55 µs | cpu.on_cpu (0.00, 4.9 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-point-join-cold | pg | - | 0.00 µs | 0.00 µs | 2.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.48 µs | none |
| qps-point-join-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 9.29 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.74 µs | 5.92 µs | SQL*Net message to client (1.00, 0.7 µs) |
| qps-point-join-warm | kds-group | - | - | 1.98 µs | 2.39 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.33 µs | cpu.on_cpu (0.00, 4.3 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-point-join-warm | kds-strict | - | - | 1.97 µs | 2.43 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.39 µs | cpu.on_cpu (0.00, 4.3 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-point-join-warm | pg | - | 0.00 µs | 0.00 µs | 2.46 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.60 µs | 0.31 µs | ClientRead (0.01, 3.6 µs) |
| qps-point-join-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.29 µs | 3.12 µs | SQL*Net message to client (1.00, 0.3 µs) |
| qps-model-join-cold | kds-group | - | - | 4.14 µs | 67.57 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 62.96 µs | cpu.on_cpu (0.00, 71.4 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-model-join-cold | kds-strict | - | - | 9.86 µs | 77.71 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 62.73 µs | cpu.on_cpu (0.00, 87.3 µs); sched.run_queue (1.00, 0.3 µs) |
| qps-model-join-cold | pg | - | 0.00 µs | 0.00 µs | 107.29 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 18.34 µs | CPU (0.14, 36.7 µs) |
| qps-model-join-cold | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 70.86 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.86 µs | 115.09 µs | SQL*Net message to client (1.00, 1.9 µs) |
| qps-model-join-warm | kds-group | - | - | 3.58 µs | 68.02 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 24.94 µs | cpu.on_cpu (0.00, 71.5 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-model-join-warm | kds-strict | - | - | 3.52 µs | 68.36 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 28.84 µs | cpu.on_cpu (0.00, 71.8 µs); sched.run_queue (1.00, 0.1 µs) |
| qps-model-join-warm | pg | - | 0.00 µs | 0.00 µs | 94.30 µs | 0.00 µs | 0.00 µs | 0.00 µs | 16.57 µs | 13.64 µs | CPU (0.08, 81.2 µs); ClientRead (0.02, 16.6 µs) |
| qps-model-join-warm | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 52.36 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.87 µs | 107.76 µs | SQL*Net message to client (1.00, 0.9 µs) |
| write-batch-1 | kds-group | - | - | 0.00 µs | 5.80 µs | 0.00 µs | 181.55 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 181.6 µs); cpu.on_cpu (0.00, 5.7 µs); sched.run_queue (1.00, 0.1 µs) |
| write-batch-1 | kds-strict | - | - | 0.00 µs | 6.69 µs | 0.00 µs | 180.64 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 180.6 µs); cpu.on_cpu (0.00, 6.6 µs); sched.run_queue (1.00, 0.1 µs) |
| write-batch-1 | pg | - | 0.00 µs | 0.00 µs | 1.61 µs | 0.00 µs | 111.93 µs | 0.00 µs | 11.20 µs | 0.00 µs | WalSync (0.11, 111.9 µs); ClientRead (0.01, 11.2 µs); CPU (0.00, 3.6 µs) |
| write-batch-1 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 13.83 µs | 0.00 µs | 177.36 µs | 0.00 µs | 0.76 µs | 0.00 µs | log file sync (1.00, 177.4 µs); SQL*Net message to client (1.00, 0.8 µs); Data file init write (0.00, 0.0 µs); Disk file operations I/O (0.00, 0.0 µs) |
| write-batch-10 | kds-group | - | - | 0.00 µs | 4.39 µs | 0.00 µs | 17.39 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (0.10, 17.4 µs); cpu.on_cpu (0.00, 4.3 µs); sched.run_queue (1.20, 0.1 µs) |
| write-batch-10 | kds-strict | - | - | 0.00 µs | 4.31 µs | 0.00 µs | 17.74 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (0.10, 17.7 µs); cpu.on_cpu (0.00, 4.2 µs); sched.run_queue (1.20, 0.1 µs) |
| write-batch-10 | pg | - | 0.00 µs | 0.00 µs | 1.10 µs | 0.00 µs | 9.43 µs | 0.00 µs | 1.50 µs | 0.00 µs | WalSync (0.01, 9.4 µs); CPU (0.00, 1.6 µs); ClientRead (0.00, 1.5 µs) |
| write-batch-10 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.86 µs | 0.00 µs | 14.12 µs | 0.00 µs | 0.23 µs | 0.00 µs | log file sync (0.10, 14.1 µs); SQL*Net message to client (1.10, 0.2 µs) |
| write-batch-100 | kds-group | - | - | 0.00 µs | 4.14 µs | 0.00 µs | 2.12 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.1 µs); commit.wal_flush (0.01, 2.1 µs); sched.run_queue (1.02, 0.1 µs) |
| write-batch-100 | kds-strict | - | - | 0.00 µs | 4.17 µs | 0.00 µs | 2.17 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.1 µs); commit.wal_flush (0.01, 2.2 µs); sched.run_queue (1.02, 0.1 µs) |
| write-batch-100 | pg | - | 0.00 µs | 0.00 µs | 1.08 µs | 0.00 µs | 0.56 µs | 0.00 µs | 2.63 µs | 0.00 µs | ClientRead (0.00, 2.6 µs); WalSync (0.00, 0.6 µs) |
| write-batch-100 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.69 µs | 0.00 µs | 2.42 µs | 0.00 µs | 0.19 µs | 0.00 µs | log file sync (0.01, 2.4 µs); SQL*Net message to client (1.01, 0.2 µs) |
| write-batch-1000 | kds-group | - | - | 0.00 µs | 4.44 µs | 0.00 µs | 0.24 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.4 µs); commit.wal_flush (0.00, 0.2 µs); sched.run_queue (1.00, 0.1 µs) |
| write-batch-1000 | kds-strict | - | - | 0.00 µs | 4.43 µs | 0.00 µs | 0.25 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4.4 µs); commit.wal_flush (0.00, 0.3 µs); sched.run_queue (1.00, 0.1 µs) |
| write-batch-1000 | pg | - | 0.00 µs | 0.00 µs | 1.08 µs | 0.00 µs | 1.05 µs | 0.00 µs | 1.02 µs | 0.00 µs | WalSync (0.00, 1.0 µs); ClientRead (0.00, 1.0 µs) |
| write-batch-1000 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.71 µs | 0.00 µs | 0.74 µs | 0.00 µs | 0.20 µs | 0.00 µs | log file sync (0.00, 0.7 µs); SQL*Net message to client (1.00, 0.2 µs); log file switch (checkpoint incomplete) (0.00, 0.0 µs); log file switch completion (0.00, 0.0 µs) |
| join-conn-1 | kds-group | - | - | 14.90 µs | 642.58 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.52 µs | cpu.on_cpu (0.00, 654.1 µs); sched.run_queue (14.00, 3.2 µs) |
| join-conn-1 | kds-strict | - | - | 14.46 µs | 641.36 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.54 µs | cpu.on_cpu (0.00, 652.7 µs); sched.run_queue (14.00, 3.1 µs) |
| join-conn-1 | pg | - | 0.00 µs | 0.00 µs | 1,216.52 µs | 0.00 µs | 0.00 µs | 0.00 µs | 15.41 µs | 2.04 µs | CPU (1.16, 1,219.9 µs); ClientRead (0.01, 15.4 µs) |
| join-conn-1 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 441.84 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.78 µs | 23.24 µs | SQL*Net message to client (1.00, 0.8 µs) |
| join-conn-2 | kds-group | - | - | 14.83 µs | 1,220.58 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.66 µs | cpu.on_cpu (0.00, 649.2 µs); sched.run_queue (14.00, 587.4 µs) |
| join-conn-2 | kds-strict | - | - | 15.82 µs | 1,221.03 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.59 µs | cpu.on_cpu (0.00, 656.4 µs); sched.run_queue (14.00, 578.0 µs) |
| join-conn-2 | pg | - | 0.00 µs | 0.00 µs | 1,228.79 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.72 µs | 1.02 µs | CPU (1.14, 1,195.5 µs); ClientRead (0.01, 5.7 µs) |
| join-conn-2 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 458.52 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.00 µs | 24.10 µs | SQL*Net message to client (1.00, 1.0 µs) |
| join-conn-4 | kds-group | - | - | 14.94 µs | 2,369.97 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.82 µs | sched.run_queue (14.00, 1,736.4 µs); cpu.on_cpu (0.00, 648.5 µs) |
| join-conn-4 | kds-strict | - | - | 14.77 µs | 2,357.76 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 6.60 µs | sched.run_queue (14.00, 1,719.0 µs); cpu.on_cpu (0.00, 653.7 µs) |
| join-conn-4 | pg | - | 0.00 µs | 0.00 µs | 1,230.61 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.40 µs | 2.68 µs | CPU (1.17, 1,225.4 µs); ClientRead (0.01, 5.4 µs) |
| join-conn-4 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 483.46 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.50 µs | 21.23 µs | SQL*Net message to client (1.00, 1.5 µs) |
| join-conn-8 | kds-group | - | - | 14.66 µs | 4,882.45 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 6.70 µs | sched.run_queue (14.00, 4,249.9 µs); cpu.on_cpu (0.00, 647.3 µs) |
| join-conn-8 | kds-strict | - | - | 14.70 µs | 4,880.63 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 6.36 µs | sched.run_queue (14.00, 4,250.3 µs); cpu.on_cpu (0.00, 645.0 µs) |
| join-conn-8 | pg | - | 0.00 µs | 0.00 µs | 1,263.13 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.07 µs | CPU (1.07, 1,145.5 µs) |
| join-conn-8 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 506.06 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.32 µs | 18.97 µs | resmgr:cpu quantum (0.09, 33.6 µs); SQL*Net message to client (1.00, 2.3 µs) |

### s2 `four-bookers`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| booking | kds-group | 3,181 ops | 907.9 µs (2.7 %) | 90.4 µs | 781.4 µs | 911.6 µs | 1,321.3 µs | 1,747.1 µs | 4,353 ops/s | 60.0 µs | 1.05 µs | 101.5 µs | 0 errors |
| booking | pg | 3,160 ops | 390.1 µs (3.5 %) | 84.2 µs | 343.2 µs | 362.6 µs | 499.4 µs | 881.5 µs | 10,057 ops/s | 70.1 µs | 0.52 µs | 103.7 µs | 0 errors |
| booking | oracle | 3,159 ops | 763.8 µs (5.3 %) | 221.2 µs | 380.4 µs | 415.5 µs | 811.9 µs | 2,128.9 µs | 5,168 ops/s | 90.4 µs | 8.88 µs | 166.2 µs | 0 errors |
| manifest-scan | kds-group | 100 ops | 19.7 µs (112.0 %) | 16.0 µs | 18.2 µs | 19.2 µs | 22.7 µs | 23.7 µs | 49,215 ops/s | 5.2 µs | 0.85 µs | 10.7 µs | 0 errors |
| manifest-scan | pg | 100 ops | 21.9 µs (4.5 %) | 17.2 µs | 20.2 µs | 21.6 µs | 26.0 µs | 27.4 µs | 42,498 ops/s | 7.9 µs | 0.41 µs | 9.9 µs | 0 errors |
| manifest-scan | oracle | 100 ops | 34.4 µs (76.8 %) | 18.6 µs | 21.5 µs | 23.1 µs | 35.3 µs | 312.7 µs | 27,157 ops/s | 12.7 µs | 4.70 µs | 11.9 µs | 0 errors |
| voyage-rollup | kds-group | 100 ops | 25.0 µs (81.1 %) | 15.2 µs | 16.5 µs | 17.1 µs | 34.4 µs | 40.2 µs | 37,499 ops/s | 4.1 µs | 0.10 µs | 10.0 µs | 0 errors |
| voyage-rollup | pg | 100 ops | 18.8 µs (2.6 %) | 17.1 µs | 18.1 µs | 18.6 µs | 19.9 µs | 20.6 µs | 51,602 ops/s | 4.9 µs | 0.03 µs | 9.8 µs | 0 errors |
| voyage-rollup | oracle | 100 ops | 31.5 µs (54.8 %) | 19.0 µs | 20.5 µs | 21.3 µs | 48.6 µs | 258.8 µs | 29,627 ops/s | 8.5 µs | 1.02 µs | 41.6 µs | 0 errors |
| customer-statement | kds-group | 50 ops | 860.6 µs (1.9 %) | 835.3 µs | 846.7 µs | 850.7 µs | 960.3 µs | 983.5 µs | 1,158 ops/s | 13.8 µs | 0.37 µs | 828.1 µs | 0 errors |
| customer-statement | pg | 50 ops | 147.5 µs (12.1 %) | 143.1 µs | 145.2 µs | 147.2 µs | 152.4 µs | 158.9 µs | 6,690 ops/s | 5.2 µs | 0.04 µs | 138.1 µs | 0 errors |
| customer-statement | oracle | 50 ops | 849.3 µs (2.3 %) | 816.6 µs | 834.8 µs | 843.1 µs | 863.0 µs | 1,197.8 µs | 1,166 ops/s | 28.8 µs | 3.65 µs | 821.0 µs | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| booking | kds-group | - | - | 7.81 µs | 156.82 µs | 0.00 µs | 131.25 µs | 5.33 µs | 0.00 µs | 1.05 µs | commit.wal_flush (0.94, 131.2 µs); sched.run_queue (14.13, 104.7 µs); cpu.on_cpu (0.00, 59.8 µs); app.lock_wait (0.02, 5.3 µs) |
| booking | pg | - | 0.00 µs | 0.00 µs | 34.01 µs | 0.00 µs | 140.15 µs | 4.53 µs | 50.85 µs | 0.58 µs | WalSync (0.08, 80.1 µs); WALWrite (0.06, 57.2 µs); ClientRead (0.05, 50.8 µs); CPU (0.02, 25.1 µs) |
| booking | oracle | 0.02 µs | 0.00 µs | 0.02 µs | 307.84 µs | 0.02 µs | 173.78 µs | 36.17 µs | 4.29 µs | 8.88 µs | log file sync (1.00, 173.8 µs); log file switch (checkpoint incomplete) (0.00, 105.1 µs); log file switch completion (0.00, 97.6 µs); buffer busy waits (0.33, 34.6 µs) |
| manifest-scan | kds-group | - | - | 1.10 µs | 5.67 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.85 µs | cpu.on_cpu (0.00, 6.7 µs); sched.run_queue (1.00, 0.1 µs) |
| manifest-scan | pg | - | 0.00 µs | 0.00 µs | 5.31 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.44 µs | none |
| manifest-scan | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 6.21 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.30 µs | 4.70 µs | SQL*Net message to client (1.00, 0.3 µs) |
| voyage-rollup | kds-group | - | - | 1.30 µs | 4.58 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.10 µs | cpu.on_cpu (0.00, 5.8 µs); sched.run_queue (1.00, 0.1 µs) |
| voyage-rollup | pg | - | 0.00 µs | 0.00 µs | 3.90 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.05 µs | none |
| voyage-rollup | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 7.87 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | 1.02 µs | SQL*Net message to client (1.00, 0.2 µs) |
| customer-statement | kds-group | - | - | 3.98 µs | 819.38 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.37 µs | cpu.on_cpu (0.00, 823.2 µs); sched.run_queue (1.00, 0.1 µs) |
| customer-statement | pg | - | 0.00 µs | 0.00 µs | 135.96 µs | 0.00 µs | 0.00 µs | 0.00 µs | 17.60 µs | 0.08 µs | CPU (0.10, 110.5 µs); ClientRead (0.02, 17.6 µs) |
| customer-statement | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 806.82 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.54 µs | 3.65 µs | SQL*Net message to client (1.00, 0.5 µs) |

### s2 `one-booker`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, kds-strict 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| booking | kds-group | 3,116 ops | 323.1 µs (3.3 %) | 71.7 µs | 313.8 µs | 327.3 µs | 360.4 µs | 454.6 µs | 3,094 ops/s | 40.7 µs | 0.70 µs | 117.7 µs | 0 errors |
| booking | kds-strict | 3,116 ops | 327.8 µs (5.4 %) | 74.3 µs | 317.0 µs | 329.6 µs | 367.0 µs | 588.6 µs | 3,049 ops/s | 40.0 µs | 0.71 µs | 117.0 µs | 0 errors |
| booking | pg | 3,116 ops | 337.7 µs (2.0 %) | 86.7 µs | 325.2 µs | 337.4 µs | 375.1 µs | 597.1 µs | 2,960 ops/s | 68.4 µs | 0.48 µs | 96.0 µs | 0 errors |
| booking | oracle | 3,116 ops | 438.7 µs (8.5 %) | 74.1 µs | 342.7 µs | 357.5 µs | 423.6 µs | 762.4 µs | 2,279 ops/s | 70.8 µs | 6.22 µs | 130.8 µs | 0 errors |
| manifest-scan | kds-group | 100 ops | 18.1 µs (34.0 %) | 14.6 µs | 16.7 µs | 17.5 µs | 21.2 µs | 24.0 µs | 49,410 ops/s | 3.6 µs | 0.59 µs | 10.4 µs | 0 errors |
| manifest-scan | kds-strict | 100 ops | 17.7 µs (34.5 %) | 14.4 µs | 16.5 µs | 17.2 µs | 20.7 µs | 23.2 µs | 49,579 ops/s | 3.3 µs | 0.54 µs | 10.3 µs | 0 errors |
| manifest-scan | pg | 100 ops | 22.5 µs (12.1 %) | 15.6 µs | 19.6 µs | 21.0 µs | 27.6 µs | 48.6 µs | 40,749 ops/s | 8.0 µs | 0.39 µs | 9.8 µs | 0 errors |
| manifest-scan | oracle | 100 ops | 30.8 µs (26.1 %) | 16.1 µs | 20.6 µs | 22.3 µs | 29.8 µs | 234.9 µs | 30,178 ops/s | 13.2 µs | 4.75 µs | 9.5 µs | 0 errors |
| voyage-rollup | kds-group | 100 ops | 23.7 µs (42.3 %) | 14.2 µs | 15.3 µs | 15.7 µs | 44.0 µs | 174.1 µs | 39,821 ops/s | 3.6 µs | 0.08 µs | 9.3 µs | 0 errors |
| voyage-rollup | kds-strict | 100 ops | 16.0 µs (75.7 %) | 14.0 µs | 15.1 µs | 15.5 µs | 18.4 µs | 25.7 µs | 60,905 ops/s | 2.7 µs | 0.07 µs | 9.2 µs | 0 errors |
| voyage-rollup | pg | 100 ops | 30.3 µs (50.9 %) | 16.4 µs | 17.9 µs | 18.4 µs | 143.7 µs | 201.6 µs | 30,393 ops/s | 6.1 µs | 0.04 µs | 12.9 µs | 0 errors |
| voyage-rollup | oracle | 100 ops | 22.2 µs (47.1 %) | 18.1 µs | 19.4 µs | 20.0 µs | 22.0 µs | 26.7 µs | 42,136 ops/s | 7.3 µs | 0.85 µs | 11.2 µs | 0 errors |
| customer-statement | kds-group | 50 ops | 881.1 µs (0.8 %) | 833.4 µs | 852.0 µs | 858.7 µs | 1,004.6 µs | 1,069.5 µs | 1,127 ops/s | 17.9 µs | 0.46 µs | 834.9 µs | 0 errors |
| customer-statement | kds-strict | 50 ops | 857.6 µs (3.2 %) | 818.5 µs | 830.3 µs | 836.5 µs | 955.1 µs | 1,012.5 µs | 1,160 ops/s | 15.0 µs | 0.38 µs | 811.7 µs | 0 errors |
| customer-statement | pg | 50 ops | 147.5 µs (12.7 %) | 143.0 µs | 144.9 µs | 147.1 µs | 156.5 µs | 169.0 µs | 6,605 ops/s | 5.9 µs | 0.04 µs | 137.7 µs | 0 errors |
| customer-statement | oracle | 50 ops | 882.8 µs (21.9 %) | 826.2 µs | 845.9 µs | 859.2 µs | 1,093.1 µs | 1,127.7 µs | 1,122 ops/s | 39.1 µs | 5.18 µs | 842.1 µs | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| booking | kds-group | - | - | 7.99 µs | 51.53 µs | 0.00 µs | 117.58 µs | 0.00 µs | 0.00 µs | 0.70 µs | commit.wal_flush (0.96, 117.6 µs); cpu.on_cpu (0.00, 58.6 µs); sched.run_queue (14.30, 0.9 µs) |
| booking | kds-strict | - | - | 7.99 µs | 52.16 µs | 0.00 µs | 117.59 µs | 0.00 µs | 0.00 µs | 0.71 µs | commit.wal_flush (0.96, 117.6 µs); cpu.on_cpu (0.00, 58.9 µs); sched.run_queue (14.30, 1.3 µs) |
| booking | pg | - | 0.00 µs | 0.00 µs | 33.04 µs | 0.00 µs | 107.18 µs | 0.00 µs | 36.65 µs | 0.57 µs | WalSync (0.10, 105.5 µs); ClientRead (0.04, 36.6 µs); CPU (0.02, 24.4 µs); WalInitSync (0.00, 1.0 µs) |
| booking | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 114.53 µs | 0.00 µs | 131.99 µs | 0.00 µs | 2.82 µs | 6.22 µs | log file sync (0.96, 132.0 µs); log file switch (checkpoint incomplete) (0.00, 35.1 µs); SQL*Net message to client (13.30, 2.8 µs); log file switch completion (0.00, 1.4 µs) |
| manifest-scan | kds-group | - | - | 1.09 µs | 5.46 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.59 µs | cpu.on_cpu (0.00, 6.5 µs); sched.run_queue (1.00, 0.1 µs) |
| manifest-scan | kds-strict | - | - | 1.10 µs | 5.45 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.54 µs | cpu.on_cpu (0.00, 6.5 µs); sched.run_queue (1.00, 0.1 µs) |
| manifest-scan | pg | - | 0.00 µs | 0.00 µs | 5.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.41 µs | none |
| manifest-scan | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.82 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.31 µs | 4.75 µs | SQL*Net message to client (1.00, 0.3 µs) |
| voyage-rollup | kds-group | - | - | 1.27 µs | 4.39 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.08 µs | cpu.on_cpu (0.00, 5.6 µs); sched.run_queue (1.00, 0.1 µs) |
| voyage-rollup | kds-strict | - | - | 1.29 µs | 4.34 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.07 µs | cpu.on_cpu (0.00, 5.5 µs); sched.run_queue (1.00, 0.1 µs) |
| voyage-rollup | pg | - | 0.00 µs | 0.00 µs | 3.74 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.39 µs | 0.05 µs | ClientRead (0.01, 3.4 µs) |
| voyage-rollup | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 7.24 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.18 µs | 0.85 µs | SQL*Net message to client (1.00, 0.2 µs) |
| customer-statement | kds-group | - | - | 3.90 µs | 826.18 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.46 µs | cpu.on_cpu (0.00, 830.2 µs); sched.run_queue (1.00, 0.1 µs) |
| customer-statement | kds-strict | - | - | 3.96 µs | 803.32 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.38 µs | cpu.on_cpu (0.00, 807.2 µs); sched.run_queue (1.00, 0.1 µs) |
| customer-statement | pg | - | 0.00 µs | 0.00 µs | 136.74 µs | 0.00 µs | 0.00 µs | 0.00 µs | 17.06 µs | 0.08 µs | CPU (0.08, 82.2 µs); ClientRead (0.02, 17.1 µs) |
| customer-statement | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 828.66 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.66 µs | 5.18 µs | SQL*Net message to client (1.00, 0.7 µs) |

### s3 `loans1000-indexed`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ddl | kds-group | 4 ops | 404.5 µs (52.7 %) | 347.8 µs | 373.1 µs | 435.7 µs | 461.3 µs | 461.3 µs | 1,818 ops/s | 73.0 µs | 0.00 µs | 96.4 µs | 0 errors |
| ddl | pg | 4 ops | 674.1 µs (5.0 %) | 449.2 µs | 463.3 µs | 497.5 µs | 1,227.1 µs | 1,227.1 µs | 1,419 ops/s | 17.0 µs | 0.00 µs | 468.2 µs | 0 errors |
| ddl | oracle | 4 ops | 3,904.2 µs (10.6 %) | 2,956.5 µs | 3,408.0 µs | 3,511.9 µs | 5,740.4 µs | 5,740.4 µs | 242 ops/s | 311.0 µs | 0.00 µs | 3,169.5 µs | 0 errors |
| load | kds-group | 1,900 ops | 150.1 µs (10.3 %) | 126.8 µs | 137.6 µs | 139.5 µs | 175.7 µs | 324.8 µs | 6,651 ops/s | 9.2 µs | 0.00 µs | 13.9 µs | 0 errors |
| load | pg | 1,900 ops | 153.7 µs (140.6 %) | 131.3 µs | 137.6 µs | 140.1 µs | 185.6 µs | 396.6 µs | 6,500 ops/s | 12.2 µs | 0.00 µs | 13.3 µs | 0 errors |
| load | oracle | 1,900 ops | 286.1 µs (9.7 %) | 140.6 µs | 171.9 µs | 216.0 µs | 531.9 µs | 732.9 µs | 3,489 ops/s | 22.7 µs | 0.00 µs | 59.3 µs | 0 errors |
| create-index | kds-group | 8 ops | 736.5 µs (10.8 %) | 397.0 µs | 448.0 µs | 932.3 µs | 1,115.0 µs | 1,115.0 µs | 1,279 ops/s | 44.3 µs | 0.00 µs | 435.8 µs | 0 errors |
| create-index | pg | 8 ops | 519.9 µs (63.8 %) | 376.8 µs | 477.3 µs | 553.1 µs | 679.2 µs | 679.2 µs | 1,838 ops/s | 17.0 µs | 0.00 µs | 232.5 µs | 0 errors |
| create-index | oracle | 8 ops | 2,280.1 µs (22.3 %) | 1,320.1 µs | 1,391.8 µs | 1,988.0 µs | 5,972.4 µs | 5,972.4 µs | 430 ops/s | 200.7 µs | 0.00 µs | 1,667.0 µs | 0 errors |
| pk-user | kds-group | 200 ops | 30.1 µs (13.5 %) | 23.5 µs | 29.8 µs | 30.1 µs | 32.0 µs | 45.5 µs | 31,193 ops/s | 14.3 µs | 0.45 µs | 7.4 µs | 0 errors |
| pk-user | pg | 200 ops | 29.9 µs (47.9 %) | 27.5 µs | 29.0 µs | 29.2 µs | 30.4 µs | 40.6 µs | 32,333 ops/s | 15.5 µs | 0.27 µs | 9.0 µs | 0 errors |
| pk-user | oracle | 200 ops | 27.8 µs (13.2 %) | 16.8 µs | 17.2 µs | 18.0 µs | 45.3 µs | 242.9 µs | 34,329 ops/s | 11.4 µs | 1.52 µs | 7.6 µs | 0 errors |
| loans-by-user | kds-group | 200 ops | 21.0 µs (22.8 %) | 17.0 µs | 18.6 µs | 19.1 µs | 21.2 µs | 36.6 µs | 44,621 ops/s | 6.7 µs | 0.46 µs | 8.7 µs | 0 errors |
| loans-by-user | pg | 200 ops | 24.4 µs (40.4 %) | 15.4 µs | 22.3 µs | 23.4 µs | 29.0 µs | 48.2 µs | 40,114 ops/s | 12.8 µs | 0.25 µs | 7.3 µs | 0 errors |
| loans-by-user | oracle | 200 ops | 26.7 µs (17.1 %) | 20.0 µs | 22.0 µs | 22.7 µs | 27.8 µs | 33.8 µs | 36,648 ops/s | 9.2 µs | 2.24 µs | 12.7 µs | 0 errors |
| loans-by-book | kds-group | 200 ops | 20.6 µs (19.9 %) | 15.6 µs | 18.5 µs | 19.2 µs | 33.8 µs | 40.9 µs | 46,294 ops/s | 6.7 µs | 0.37 µs | 8.6 µs | 0 errors |
| loans-by-book | pg | 200 ops | 21.7 µs (24.9 %) | 15.2 µs | 18.5 µs | 19.8 µs | 31.7 µs | 38.2 µs | 44,289 ops/s | 9.6 µs | 0.18 µs | 7.3 µs | 0 errors |
| loans-by-book | oracle | 200 ops | 28.6 µs (6.5 %) | 21.7 µs | 23.5 µs | 24.2 µs | 30.5 µs | 209.4 µs | 33,661 ops/s | 6.9 µs | 1.43 µs | 15.8 µs | 0 errors |
| resv-by-user | kds-group | 200 ops | 19.8 µs (26.0 %) | 14.8 µs | 16.0 µs | 16.7 µs | 18.9 µs | 128.0 µs | 48,897 ops/s | 5.2 µs | 0.19 µs | 7.7 µs | 0 errors |
| resv-by-user | pg | 200 ops | 18.5 µs (29.4 %) | 14.1 µs | 16.2 µs | 17.2 µs | 21.9 µs | 33.4 µs | 52,529 ops/s | 6.8 µs | 0.10 µs | 6.4 µs | 0 errors |
| resv-by-user | oracle | 200 ops | 19.7 µs (2.1 %) | 14.2 µs | 15.9 µs | 16.3 µs | 18.1 µs | 65.7 µs | 49,088 ops/s | 6.4 µs | 1.06 µs | 7.4 µs | 0 errors |
| books-by-author | kds-group | 200 ops | 20.6 µs (11.6 %) | 16.1 µs | 17.1 µs | 17.6 µs | 32.5 µs | 116.8 µs | 45,057 ops/s | 6.9 µs | 0.40 µs | 8.0 µs | 0 errors |
| books-by-author | pg | 200 ops | 22.7 µs (21.1 %) | 16.4 µs | 17.6 µs | 18.3 µs | 23.8 µs | 29.8 µs | 41,753 ops/s | 6.0 µs | 0.16 µs | 9.2 µs | 0 errors |
| books-by-author | oracle | 200 ops | 21.9 µs (20.6 %) | 14.1 µs | 15.9 µs | 16.5 µs | 47.2 µs | 162.6 µs | 43,957 ops/s | 8.0 µs | 1.63 µs | 7.1 µs | 0 errors |
| books-by-genre | kds-group | 200 ops | 21.0 µs (21.0 %) | 18.0 µs | 19.5 µs | 20.4 µs | 23.0 µs | 28.1 µs | 45,864 ops/s | 6.7 µs | 0.82 µs | 10.2 µs | 0 errors |
| books-by-genre | pg | 200 ops | 24.1 µs (49.1 %) | 19.6 µs | 21.5 µs | 22.7 µs | 26.5 µs | 86.2 µs | 40,581 ops/s | 9.5 µs | 0.53 µs | 11.6 µs | 0 errors |
| books-by-genre | oracle | 200 ops | 23.7 µs (30.8 %) | 16.5 µs | 18.3 µs | 19.2 µs | 21.8 µs | 198.0 µs | 40,592 ops/s | 9.2 µs | 2.80 µs | 8.7 µs | 0 errors |
| loans-by-daterange | kds-group | 200 ops | 39.1 µs (6.8 %) | 28.2 µs | 36.6 µs | 37.8 µs | 42.3 µs | 47.9 µs | 25,007 ops/s | 8.5 µs | 2.38 µs | 25.1 µs | 0 errors |
| loans-by-daterange | pg | 200 ops | 52.4 µs (2.8 %) | 34.8 µs | 49.1 µs | 51.3 µs | 57.2 µs | 60.1 µs | 18,632 ops/s | 17.4 µs | 0.92 µs | 30.7 µs | 0 errors |
| loans-by-daterange | oracle | 200 ops | 50.9 µs (28.3 %) | 26.7 µs | 40.1 µs | 41.8 µs | 47.2 µs | 335.7 µs | 19,319 ops/s | 17.2 µs | 8.84 µs | 23.4 µs | 0 errors |
| overdue | kds-group | 200 ops | 26.3 µs (23.7 %) | 17.0 µs | 24.2 µs | 25.8 µs | 34.0 µs | 36.5 µs | 36,788 ops/s | 6.7 µs | 1.49 µs | 14.9 µs | 0 errors |
| overdue | pg | 200 ops | 29.8 µs (34.5 %) | 14.8 µs | 26.2 µs | 27.5 µs | 30.6 µs | 65.3 µs | 32,759 ops/s | 12.8 µs | 0.60 µs | 11.1 µs | 0 errors |
| overdue | oracle | 200 ops | 43.6 µs (9.6 %) | 27.1 µs | 37.0 µs | 38.5 µs | 46.6 µs | 217.2 µs | 22,558 ops/s | 13.7 µs | 6.37 µs | 23.4 µs | 0 errors |
| join-loan-user | kds-group | 200 ops | 26.6 µs (19.5 %) | 17.6 µs | 19.4 µs | 20.3 µs | 23.7 µs | 169.1 µs | 36,133 ops/s | 5.3 µs | 0.33 µs | 11.8 µs | 0 errors |
| join-loan-user | pg | 200 ops | 23.3 µs (11.3 %) | 18.2 µs | 20.7 µs | 21.4 µs | 25.8 µs | 55.4 µs | 41,511 ops/s | 6.1 µs | 0.19 µs | 11.9 µs | 0 errors |
| join-loan-user | oracle | 200 ops | 19.9 µs (14.1 %) | 15.1 µs | 16.7 µs | 17.3 µs | 19.7 µs | 32.4 µs | 47,931 ops/s | 7.1 µs | 1.65 µs | 7.2 µs | 0 errors |
| join-no-literal | kds-group | 200 ops | 50.0 µs (14.3 %) | 43.9 µs | 46.3 µs | 47.6 µs | 61.2 µs | 81.0 µs | 19,682 ops/s | 13.0 µs | 5.11 µs | 31.1 µs | 0 errors |
| join-no-literal | pg | 200 ops | 91.5 µs (21.6 %) | 65.8 µs | 67.1 µs | 67.9 µs | 245.7 µs | 274.7 µs | 10,827 ops/s | 28.2 µs | 2.46 µs | 40.3 µs | 0 errors |
| join-no-literal | oracle | 200 ops | 70.5 µs (14.5 %) | 58.3 µs | 60.4 µs | 67.7 µs | 76.4 µs | 133.7 µs | 14,041 ops/s | 27.6 µs | 15.23 µs | 38.2 µs | 0 errors |
| exists-correlated | kds-group | 200 ops | 45.4 µs (10.7 %) | 35.3 µs | 36.8 µs | 37.8 µs | 55.4 µs | 209.8 µs | 21,550 ops/s | 5.6 µs | 0.43 µs | 29.6 µs | 0 errors |
| exists-correlated | pg | 200 ops | 36.2 µs (0.1 %) | 29.2 µs | 30.3 µs | 30.4 µs | 56.6 µs | 169.1 µs | 26,910 ops/s | 11.2 µs | 0.40 µs | 18.0 µs | 0 errors |
| exists-correlated | oracle | 200 ops | 25.3 µs (20.7 %) | 21.2 µs | 22.4 µs | 22.5 µs | 40.9 µs | 44.8 µs | 38,034 ops/s | 7.9 µs | 2.00 µs | 12.3 µs | 0 errors |
| count-by-user | kds-group | 200 ops | 19.0 µs (4.0 %) | 15.3 µs | 16.5 µs | 17.2 µs | 19.2 µs | 43.5 µs | 50,383 ops/s | 4.9 µs | 0.10 µs | 8.0 µs | 0 errors |
| count-by-user | pg | 200 ops | 16.8 µs (43.0 %) | 13.8 µs | 15.4 µs | 15.7 µs | 18.3 µs | 38.2 µs | 57,669 ops/s | 4.8 µs | 0.04 µs | 7.7 µs | 0 errors |
| count-by-user | oracle | 200 ops | 24.6 µs (37.8 %) | 14.7 µs | 15.9 µs | 16.0 µs | 19.1 µs | 233.9 µs | 38,804 ops/s | 6.9 µs | 0.79 µs | 7.5 µs | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ddl | kds-group | - | - | - | 74.75 µs | 0.00 µs | 133.25 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 133.2 µs); cpu.on_cpu (0.00, 74.5 µs); sched.run_queue (1.00, 0.2 µs) |
| ddl | pg | - | 0.00 µs | 0.00 µs | 420.00 µs | 35.75 µs | 248.75 µs | 0.00 µs | 0.00 µs | 0.00 µs | WalSync (0.25, 248.8 µs); CPU (0.25, 78.5 µs) |
| ddl | oracle | 520.50 µs | 494.25 µs | 1,014.75 µs | 2,776.75 µs | 0.00 µs | 301.00 µs | 0.00 µs | 0.50 µs | 0.00 µs | log file sync (1.00, 301.0 µs); events in waitclass Other (1.25, 8.8 µs); SQL*Net message to client (1.00, 0.5 µs) |
| load | kds-group | - | - | - | 5.37 µs | 0.00 µs | 122.72 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 122.7 µs); cpu.on_cpu (0.00, 5.3 µs); sched.run_queue (1.00, 0.1 µs) |
| load | pg | - | 0.00 µs | 0.00 µs | 1.89 µs | 0.00 µs | 111.61 µs | 0.00 µs | 7.77 µs | 0.00 µs | WalSync (0.11, 111.6 µs); ClientRead (0.01, 7.8 µs); CPU (0.00, 5.0 µs); DataFileExtend (0.00, 0.5 µs) |
| load | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 14.42 µs | 0.00 µs | 175.39 µs | 0.00 µs | 0.76 µs | 0.00 µs | log file sync (1.00, 175.4 µs); SQL*Net message to client (1.00, 0.8 µs) |
| create-index | kds-group | - | - | - | 418.75 µs | 0.00 µs | 159.75 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 418.5 µs); commit.wal_flush (1.00, 159.8 µs); sched.run_queue (1.00, 0.1 µs) |
| create-index | pg | - | 0.00 µs | 0.00 µs | 212.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | CPU (0.12, 63.4 µs) |
| create-index | oracle | 61.50 µs | 384.25 µs | 445.75 µs | 1,288.12 µs | 112.50 µs | 113.25 µs | 0.00 µs | 0.88 µs | 0.00 µs | events in waitclass Other (1.25, 454.6 µs); log file sync (1.00, 113.2 µs); direct path write (0.62, 112.5 µs); SQL*Net message to client (1.00, 0.9 µs) |
| pk-user | kds-group | - | - | 1.00 µs | 1.48 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.45 µs | cpu.on_cpu (0.00, 2.4 µs); sched.run_queue (1.00, 0.1 µs) |
| pk-user | pg | - | 0.00 µs | 0.00 µs | 4.31 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.90 µs | 0.30 µs | ClientRead (0.01, 2.9 µs) |
| pk-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.43 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.27 µs | 1.52 µs | SQL*Net message to client (1.00, 0.3 µs) |
| loans-by-user | kds-group | - | - | 1.21 µs | 3.46 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.46 µs | cpu.on_cpu (0.00, 4.6 µs); sched.run_queue (1.00, 0.1 µs) |
| loans-by-user | pg | - | 0.00 µs | 0.00 µs | 2.58 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.99 µs | 0.22 µs | ClientRead (0.01, 5.0 µs) |
| loans-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 8.63 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.17 µs | 2.24 µs | SQL*Net message to client (1.00, 0.2 µs) |
| loans-by-book | kds-group | - | - | 1.17 µs | 3.38 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.37 µs | cpu.on_cpu (0.00, 4.5 µs); sched.run_queue (1.00, 0.1 µs) |
| loans-by-book | pg | - | 0.00 µs | 0.00 µs | 2.28 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.15 µs | none |
| loans-by-book | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 11.38 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 1.43 µs | SQL*Net message to client (1.00, 0.2 µs) |
| resv-by-user | kds-group | - | - | 1.14 µs | 2.71 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.19 µs | cpu.on_cpu (0.00, 3.8 µs); sched.run_queue (1.00, 0.1 µs) |
| resv-by-user | pg | - | 0.00 µs | 0.00 µs | 1.56 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.10 µs | none |
| resv-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.17 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 1.06 µs | SQL*Net message to client (1.00, 0.2 µs) |
| books-by-author | kds-group | - | - | 1.18 µs | 2.97 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.40 µs | cpu.on_cpu (0.00, 4.1 µs); sched.run_queue (1.00, 0.1 µs) |
| books-by-author | pg | - | 0.00 µs | 0.00 µs | 5.18 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.16 µs | 0.17 µs | ClientRead (0.01, 1.2 µs) |
| books-by-author | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.96 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.20 µs | 1.63 µs | SQL*Net message to client (1.00, 0.2 µs) |
| books-by-genre | kds-group | - | - | 1.19 µs | 4.96 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.82 µs | cpu.on_cpu (0.00, 6.1 µs); sched.run_queue (1.00, 0.1 µs) |
| books-by-genre | pg | - | 0.00 µs | 0.00 µs | 6.17 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.66 µs | none |
| books-by-genre | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.71 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.26 µs | 2.80 µs | SQL*Net message to client (1.00, 0.3 µs) |
| loans-by-daterange | kds-group | - | - | 1.24 µs | 19.73 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.38 µs | cpu.on_cpu (0.00, 20.9 µs); sched.run_queue (1.00, 0.1 µs) |
| loans-by-daterange | pg | - | 0.00 µs | 0.00 µs | 26.07 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.06 µs | CPU (0.02, 17.8 µs) |
| loans-by-daterange | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 18.51 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 8.84 µs | SQL*Net message to client (1.00, 0.2 µs) |
| overdue | kds-group | - | - | 1.45 µs | 9.27 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.49 µs | cpu.on_cpu (0.00, 10.7 µs); sched.run_queue (1.00, 0.1 µs) |
| overdue | pg | - | 0.00 µs | 0.00 µs | 6.06 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.64 µs | none |
| overdue | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 18.98 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 6.37 µs | SQL*Net message to client (1.00, 0.2 µs) |
| join-loan-user | kds-group | - | - | 3.85 µs | 4.04 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.33 µs | cpu.on_cpu (0.00, 7.8 µs); sched.run_queue (1.00, 0.1 µs) |
| join-loan-user | pg | - | 0.00 µs | 0.00 µs | 6.47 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.12 µs | 0.20 µs | ClientRead (0.01, 1.1 µs) |
| join-loan-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.21 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.27 µs | 1.65 µs | SQL*Net message to client (1.00, 0.3 µs) |
| join-no-literal | kds-group | - | - | 3.80 µs | 23.12 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.11 µs | cpu.on_cpu (0.00, 26.9 µs); sched.run_queue (1.00, 0.1 µs) |
| join-no-literal | pg | - | 0.00 µs | 0.00 µs | 31.34 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.53 µs | CPU (0.04, 46.1 µs) |
| join-no-literal | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 30.82 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.15 µs | 15.23 µs | events in waitclass Other (3.00, 3.9 µs); SQL*Net message to client (1.00, 0.1 µs) |
| exists-correlated | kds-group | - | - | 1.96 µs | 23.71 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.43 µs | cpu.on_cpu (0.00, 25.6 µs); sched.run_queue (1.00, 0.1 µs) |
| exists-correlated | pg | - | 0.00 µs | 0.00 µs | 12.58 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.45 µs | CPU (0.02, 19.1 µs) |
| exists-correlated | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 8.27 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.19 µs | 2.00 µs | SQL*Net message to client (1.00, 0.2 µs) |
| count-by-user | kds-group | - | - | 1.02 µs | 3.13 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.10 µs | cpu.on_cpu (0.00, 4.1 µs); sched.run_queue (1.00, 0.1 µs) |
| count-by-user | pg | - | 0.00 µs | 0.00 µs | 2.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.05 µs | none |
| count-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.92 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | 0.79 µs | SQL*Net message to client (1.00, 0.2 µs) |

### s3 `loans1000-unindexed`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ddl | kds-group | 4 ops | 404.1 µs (25.2 %) | 270.0 µs | 350.7 µs | 465.5 µs | 536.9 µs | 536.9 µs | 2,215 ops/s | 21.4 µs | 0.00 µs | 136.4 µs | 0 errors |
| ddl | pg | 4 ops | 664.3 µs (4.8 %) | 454.0 µs | 463.0 µs | 484.7 µs | 1,210.8 µs | 1,210.8 µs | 1,448 ops/s | 21.1 µs | 0.00 µs | 460.2 µs | 0 errors |
| ddl | oracle | 4 ops | 3,897.7 µs (7.8 %) | 3,028.2 µs | 3,341.6 µs | 3,575.7 µs | 5,645.1 µs | 5,645.1 µs | 245 ops/s | 286.6 µs | 0.00 µs | 3,275.5 µs | 0 errors |
| load | kds-group | 1,900 ops | 154.8 µs (0.4 %) | 131.2 µs | 138.0 µs | 140.4 µs | 232.7 µs | 373.5 µs | 6,448 ops/s | 11.3 µs | 0.00 µs | 13.3 µs | 0 errors |
| load | pg | 1,900 ops | 147.0 µs (6.2 %) | 128.4 µs | 136.4 µs | 139.0 µs | 173.4 µs | 342.5 µs | 6,797 ops/s | 11.5 µs | 0.00 µs | 12.3 µs | 0 errors |
| load | oracle | 1,900 ops | 286.9 µs (6.9 %) | 151.3 µs | 173.5 µs | 208.5 µs | 523.5 µs | 682.2 µs | 3,480 ops/s | 23.8 µs | 0.00 µs | 59.1 µs | 0 errors |
| pk-user | kds-group | 200 ops | 22.5 µs (37.8 %) | 20.2 µs | 21.3 µs | 21.4 µs | 30.4 µs | 31.5 µs | 40,787 ops/s | 9.8 µs | 0.27 µs | 6.8 µs | 0 errors |
| pk-user | pg | 200 ops | 27.7 µs (44.9 %) | 21.4 µs | 21.7 µs | 24.5 µs | 37.5 µs | 49.6 µs | 34,495 ops/s | 13.1 µs | 0.16 µs | 9.0 µs | 0 errors |
| pk-user | oracle | 200 ops | 27.1 µs (30.6 %) | 15.9 µs | 16.3 µs | 17.4 µs | 37.6 µs | 228.0 µs | 34,648 ops/s | 10.2 µs | 1.42 µs | 7.3 µs | 0 errors |
| loans-by-user | kds-group | 200 ops | 37.2 µs (27.6 %) | 28.6 µs | 30.1 µs | 30.6 µs | 43.9 µs | 99.9 µs | 25,281 ops/s | 10.6 µs | 0.70 µs | 18.8 µs | 0 errors |
| loans-by-user | pg | 200 ops | 37.3 µs (20.7 %) | 31.3 µs | 34.8 µs | 36.2 µs | 52.3 µs | 74.7 µs | 26,041 ops/s | 9.7 µs | 0.20 µs | 23.7 µs | 0 errors |
| loans-by-user | oracle | 200 ops | 27.4 µs (11.3 %) | 20.1 µs | 22.4 µs | 23.1 µs | 27.9 µs | 36.4 µs | 35,415 ops/s | 8.6 µs | 2.06 µs | 13.1 µs | 0 errors |
| loans-by-book | kds-group | 200 ops | 33.5 µs (19.2 %) | 26.1 µs | 27.2 µs | 28.8 µs | 43.4 µs | 173.2 µs | 28,170 ops/s | 7.3 µs | 0.39 µs | 18.4 µs | 0 errors |
| loans-by-book | pg | 200 ops | 34.2 µs (4.4 %) | 31.0 µs | 33.1 µs | 33.8 µs | 37.1 µs | 38.8 µs | 28,662 ops/s | 6.6 µs | 0.12 µs | 23.1 µs | 0 errors |
| loans-by-book | oracle | 200 ops | 29.0 µs (19.8 %) | 22.1 µs | 23.9 µs | 24.6 µs | 27.1 µs | 207.9 µs | 33,014 ops/s | 7.3 µs | 1.52 µs | 15.3 µs | 0 errors |
| resv-by-user | kds-group | 200 ops | 22.4 µs (13.7 %) | 20.4 µs | 20.9 µs | 21.2 µs | 22.9 µs | 26.8 µs | 43,148 ops/s | 5.3 µs | 0.19 µs | 12.2 µs | 0 errors |
| resv-by-user | pg | 200 ops | 23.4 µs (12.0 %) | 21.1 µs | 22.5 µs | 22.9 µs | 24.7 µs | 26.2 µs | 41,957 ops/s | 5.2 µs | 0.08 µs | 13.7 µs | 0 errors |
| resv-by-user | oracle | 200 ops | 23.8 µs (25.4 %) | 17.7 µs | 19.0 µs | 19.3 µs | 21.2 µs | 89.3 µs | 39,758 ops/s | 7.2 µs | 1.08 µs | 10.5 µs | 0 errors |
| books-by-author | kds-group | 200 ops | 21.8 µs (10.4 %) | 16.5 µs | 18.3 µs | 19.3 µs | 33.4 µs | 83.5 µs | 43,945 ops/s | 6.7 µs | 0.38 µs | 9.1 µs | 0 errors |
| books-by-author | pg | 200 ops | 26.1 µs (31.0 %) | 16.4 µs | 17.7 µs | 18.6 µs | 22.1 µs | 184.8 µs | 37,472 ops/s | 6.1 µs | 0.17 µs | 9.8 µs | 0 errors |
| books-by-author | oracle | 200 ops | 22.8 µs (29.7 %) | 17.7 µs | 19.8 µs | 20.3 µs | 23.1 µs | 59.4 µs | 42,577 ops/s | 9.2 µs | 1.85 µs | 9.1 µs | 0 errors |
| books-by-genre | kds-group | 200 ops | 22.9 µs (5.2 %) | 18.3 µs | 19.7 µs | 20.4 µs | 23.9 µs | 53.2 µs | 41,053 ops/s | 7.6 µs | 0.93 µs | 10.4 µs | 0 errors |
| books-by-genre | pg | 200 ops | 24.0 µs (24.8 %) | 20.0 µs | 21.8 µs | 23.1 µs | 33.2 µs | 38.9 µs | 41,384 ops/s | 9.2 µs | 0.54 µs | 10.1 µs | 0 errors |
| books-by-genre | oracle | 200 ops | 26.5 µs (38.7 %) | 19.3 µs | 21.3 µs | 22.0 µs | 35.3 µs | 213.6 µs | 36,047 ops/s | 9.6 µs | 3.06 µs | 11.1 µs | 0 errors |
| loans-by-daterange | kds-group | 200 ops | 38.2 µs (15.5 %) | 28.0 µs | 36.8 µs | 37.9 µs | 43.9 µs | 125.5 µs | 25,893 ops/s | 9.0 µs | 2.52 µs | 25.1 µs | 0 errors |
| loans-by-daterange | pg | 200 ops | 52.4 µs (8.4 %) | 33.9 µs | 48.9 µs | 51.1 µs | 56.5 µs | 59.8 µs | 18,756 ops/s | 16.5 µs | 0.91 µs | 31.4 µs | 0 errors |
| loans-by-daterange | oracle | 200 ops | 50.5 µs (19.9 %) | 27.0 µs | 39.8 µs | 42.6 µs | 50.6 µs | 187.9 µs | 19,255 ops/s | 19.7 µs | 10.25 µs | 22.5 µs | 0 errors |
| overdue | kds-group | 200 ops | 37.0 µs (9.8 %) | 31.4 µs | 36.2 µs | 36.9 µs | 39.2 µs | 50.2 µs | 26,799 ops/s | 7.5 µs | 1.63 µs | 25.3 µs | 0 errors |
| overdue | pg | 200 ops | 56.3 µs (15.7 %) | 34.4 µs | 47.4 µs | 49.2 µs | 67.4 µs | 237.8 µs | 17,451 ops/s | 13.4 µs | 0.65 µs | 33.5 µs | 0 errors |
| overdue | oracle | 200 ops | 42.0 µs (18.4 %) | 26.6 µs | 37.4 µs | 38.9 µs | 44.5 µs | 111.2 µs | 23,252 ops/s | 13.5 µs | 6.35 µs | 23.4 µs | 0 errors |
| join-loan-user | kds-group | 200 ops | 32.0 µs (27.5 %) | 28.7 µs | 29.5 µs | 30.4 µs | 33.3 µs | 44.0 µs | 30,350 ops/s | 5.3 µs | 0.34 µs | 21.5 µs | 0 errors |
| join-loan-user | pg | 200 ops | 39.5 µs (24.8 %) | 34.9 µs | 36.8 µs | 37.5 µs | 55.3 µs | 88.0 µs | 24,862 ops/s | 7.1 µs | 0.23 µs | 28.5 µs | 0 errors |
| join-loan-user | oracle | 200 ops | 31.9 µs (58.9 %) | 20.7 µs | 22.7 µs | 23.3 µs | 51.1 µs | 327.7 µs | 29,911 ops/s | 8.8 µs | 2.04 µs | 15.6 µs | 0 errors |
| join-no-literal | kds-group | 200 ops | 62.5 µs (7.7 %) | 51.8 µs | 55.0 µs | 56.2 µs | 63.7 µs | 270.1 µs | 15,741 ops/s | 15.1 µs | 5.61 µs | 39.2 µs | 0 errors |
| join-no-literal | pg | 200 ops | 87.2 µs (25.0 %) | 65.4 µs | 68.0 µs | 70.8 µs | 163.2 µs | 327.1 µs | 11,338 ops/s | 31.4 µs | 2.76 µs | 37.2 µs | 0 errors |
| join-no-literal | oracle | 200 ops | 77.0 µs (3.6 %) | 61.7 µs | 70.6 µs | 72.5 µs | 81.5 µs | 120.1 µs | 12,858 ops/s | 26.6 µs | 14.21 µs | 43.2 µs | 0 errors |
| exists-correlated | kds-group | 200 ops | 68.1 µs (12.7 %) | 62.1 µs | 63.9 µs | 67.1 µs | 71.8 µs | 84.2 µs | 14,449 ops/s | 6.5 µs | 0.51 µs | 55.8 µs | 0 errors |
| exists-correlated | pg | 200 ops | 94.4 µs (12.7 %) | 87.1 µs | 89.2 µs | 90.3 µs | 105.1 µs | 149.4 µs | 10,519 ops/s | 11.2 µs | 0.39 µs | 78.6 µs | 0 errors |
| exists-correlated | oracle | 200 ops | 30.3 µs (14.5 %) | 26.5 µs | 27.7 µs | 28.0 µs | 33.8 µs | 58.0 µs | 32,375 ops/s | 7.4 µs | 1.87 µs | 18.0 µs | 0 errors |
| count-by-user | kds-group | 200 ops | 30.3 µs (21.6 %) | 26.8 µs | 27.3 µs | 27.5 µs | 41.3 µs | 43.2 µs | 31,649 ops/s | 7.4 µs | 0.14 µs | 17.8 µs | 0 errors |
| count-by-user | pg | 200 ops | 33.2 µs (15.4 %) | 30.0 µs | 31.6 µs | 32.1 µs | 35.2 µs | 39.6 µs | 29,406 ops/s | 6.1 µs | 0.05 µs | 23.1 µs | 0 errors |
| count-by-user | oracle | 200 ops | 24.7 µs (31.9 %) | 20.5 µs | 21.6 µs | 22.1 µs | 49.1 µs | 51.9 µs | 38,793 ops/s | 8.1 µs | 0.98 µs | 12.6 µs | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ddl | kds-group | - | - | - | 97.00 µs | 0.00 µs | 152.25 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 152.2 µs); cpu.on_cpu (0.00, 96.2 µs); sched.run_queue (1.00, 0.5 µs) |
| ddl | pg | - | 0.00 µs | 0.00 µs | 382.25 µs | 34.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | CPU (0.25, 74.8 µs) |
| ddl | oracle | 476.25 µs | 498.00 µs | 967.00 µs | 2,827.00 µs | 0.00 µs | 271.25 µs | 0.00 µs | 0.75 µs | 0.00 µs | log file sync (1.00, 271.2 µs); events in waitclass Other (1.00, 6.8 µs); SQL*Net message to client (1.00, 0.8 µs) |
| load | kds-group | - | - | - | 5.04 µs | 0.00 µs | 122.91 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 122.9 µs); cpu.on_cpu (0.00, 5.0 µs); sched.run_queue (1.00, 0.1 µs) |
| load | pg | - | 0.00 µs | 0.00 µs | 1.79 µs | 0.00 µs | 119.17 µs | 0.00 µs | 8.10 µs | 0.00 µs | WalSync (0.11, 118.6 µs); ClientRead (0.01, 8.1 µs); CPU (0.00, 2.1 µs); WalWrite (0.00, 0.6 µs) |
| load | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 14.74 µs | 0.00 µs | 176.25 µs | 0.00 µs | 0.76 µs | 0.00 µs | log file sync (1.00, 176.2 µs); log file switch completion (0.00, 1.5 µs); SQL*Net message to client (1.00, 0.8 µs) |
| pk-user | kds-group | - | - | 0.99 µs | 1.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.27 µs | cpu.on_cpu (0.00, 2.4 µs); sched.run_queue (1.00, 0.1 µs) |
| pk-user | pg | - | 0.00 µs | 0.00 µs | 4.32 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | CPU (0.01, 5.3 µs) |
| pk-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.52 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 1.42 µs | SQL*Net message to client (1.00, 0.2 µs) |
| loans-by-user | kds-group | - | - | 1.13 µs | 13.30 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.70 µs | cpu.on_cpu (0.00, 14.4 µs); sched.run_queue (1.00, 0.1 µs) |
| loans-by-user | pg | - | 0.00 µs | 0.00 µs | 18.89 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.47 µs | 0.35 µs | CPU (0.01, 9.2 µs); ClientRead (0.01, 5.5 µs) |
| loans-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 8.98 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | 2.06 µs | SQL*Net message to client (1.00, 0.2 µs) |
| loans-by-book | kds-group | - | - | 1.07 µs | 13.27 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.39 µs | cpu.on_cpu (0.00, 14.3 µs); sched.run_queue (1.00, 0.1 µs) |
| loans-by-book | pg | - | 0.00 µs | 0.00 µs | 19.38 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.33 µs | 0.18 µs | CPU (0.01, 10.4 µs); ClientRead (0.01, 5.3 µs) |
| loans-by-book | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 11.18 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.26 µs | 1.52 µs | SQL*Net message to client (1.00, 0.3 µs) |
| resv-by-user | kds-group | - | - | 1.07 µs | 7.13 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.19 µs | cpu.on_cpu (0.00, 8.2 µs); sched.run_queue (1.00, 0.1 µs) |
| resv-by-user | pg | - | 0.00 µs | 0.00 µs | 9.91 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.14 µs | none |
| resv-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 6.37 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 1.08 µs | SQL*Net message to client (1.00, 0.2 µs) |
| books-by-author | kds-group | - | - | 1.09 µs | 4.12 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.38 µs | cpu.on_cpu (0.00, 5.2 µs); sched.run_queue (1.00, 0.1 µs) |
| books-by-author | pg | - | 0.00 µs | 0.00 µs | 4.99 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.28 µs | CPU (0.01, 6.7 µs) |
| books-by-author | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 5.05 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | 1.85 µs | SQL*Net message to client (1.00, 0.2 µs) |
| books-by-genre | kds-group | - | - | 1.10 µs | 4.72 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.93 µs | cpu.on_cpu (0.00, 5.8 µs); sched.run_queue (1.00, 0.1 µs) |
| books-by-genre | pg | - | 0.00 µs | 0.00 µs | 6.05 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.17 µs | 0.67 µs | ClientRead (0.01, 1.2 µs) |
| books-by-genre | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 6.34 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.28 µs | 3.06 µs | SQL*Net message to client (1.00, 0.3 µs) |
| loans-by-daterange | kds-group | - | - | 1.16 µs | 19.66 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.52 µs | cpu.on_cpu (0.00, 20.8 µs); sched.run_queue (1.00, 0.1 µs) |
| loans-by-daterange | pg | - | 0.00 µs | 0.00 µs | 26.16 µs | 0.00 µs | 0.00 µs | 0.00 µs | 7.58 µs | 1.00 µs | CPU (0.02, 21.0 µs); ClientRead (0.01, 7.6 µs) |
| loans-by-daterange | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 17.98 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.26 µs | 10.25 µs | SQL*Net message to client (1.00, 0.3 µs) |
| overdue | kds-group | - | - | 1.28 µs | 19.91 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.63 µs | cpu.on_cpu (0.00, 21.1 µs); sched.run_queue (1.00, 0.1 µs) |
| overdue | pg | - | 0.00 µs | 0.00 µs | 27.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 15.71 µs | 0.67 µs | ClientRead (0.01, 15.7 µs); CPU (0.01, 15.6 µs) |
| overdue | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 19.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.20 µs | 6.35 µs | SQL*Net message to client (1.00, 0.2 µs) |
| join-loan-user | kds-group | - | - | 3.46 µs | 13.89 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.34 µs | cpu.on_cpu (0.00, 17.3 µs); sched.run_queue (1.00, 0.0 µs) |
| join-loan-user | pg | - | 0.00 µs | 0.00 µs | 22.88 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.23 µs | 0.16 µs | CPU (0.01, 10.4 µs); ClientRead (0.01, 1.2 µs) |
| join-loan-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 10.68 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.32 µs | 2.04 µs | SQL*Net message to client (1.00, 0.3 µs) |
| join-no-literal | kds-group | - | - | 3.49 µs | 31.59 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.61 µs | cpu.on_cpu (0.00, 35.0 µs); sched.run_queue (1.00, 0.1 µs) |
| join-no-literal | pg | - | 0.00 µs | 0.00 µs | 31.91 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.18 µs | 2.47 µs | CPU (0.04, 35.4 µs); ClientRead (0.01, 5.2 µs) |
| join-no-literal | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 35.19 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.15 µs | 14.21 µs | events in waitclass Other (4.00, 4.7 µs); SQL*Net message to client (1.00, 0.2 µs) |
| exists-correlated | kds-group | - | - | 1.84 µs | 49.99 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.51 µs | cpu.on_cpu (0.00, 51.8 µs); sched.run_queue (1.00, 0.1 µs) |
| exists-correlated | pg | - | 0.00 µs | 0.00 µs | 71.52 µs | 0.00 µs | 0.00 µs | 0.00 µs | 9.47 µs | 0.43 µs | CPU (0.06, 66.3 µs); ClientRead (0.01, 9.5 µs) |
| exists-correlated | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 13.94 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | 1.87 µs | SQL*Net message to client (1.00, 0.2 µs) |
| count-by-user | kds-group | - | - | 0.95 µs | 12.99 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.14 µs | cpu.on_cpu (0.00, 13.9 µs); sched.run_queue (1.00, 0.1 µs) |
| count-by-user | pg | - | 0.00 µs | 0.00 µs | 18.96 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.04 µs | CPU (0.01, 5.7 µs) |
| count-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 8.58 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.22 µs | 0.98 µs | SQL*Net message to client (1.00, 0.2 µs) |

### s3 `loans10000-indexed`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ddl | kds-group | 4 ops | 5,409.8 µs (45.4 %) | 501.4 µs | 512.1 µs | 839.0 µs | 19,688.1 µs | 19,688.1 µs | 180 ops/s | 67.8 µs | 0.00 µs | 193.4 µs | 0 errors |
| ddl | pg | 4 ops | 650.3 µs (22.8 %) | 444.6 µs | 464.8 µs | 541.7 µs | 1,149.3 µs | 1,149.3 µs | 1,465 ops/s | 18.2 µs | 0.00 µs | 478.0 µs | 0 errors |
| ddl | oracle | 4 ops | 3,528.0 µs (8.0 %) | 2,860.3 µs | 3,076.9 µs | 3,088.1 µs | 5,086.8 µs | 5,086.8 µs | 266 ops/s | 215.5 µs | 0.00 µs | 3,128.5 µs | 0 errors |
| load | kds-group | 19,000 ops | 357.7 µs (5.7 %) | 122.7 µs | 157.9 µs | 168.0 µs | 335.7 µs | 10,904.6 µs | 2,794 ops/s | 15.7 µs | 0.00 µs | 30.6 µs | 0 errors |
| load | pg | 19,000 ops | 299.0 µs (74.8 %) | 124.2 µs | 138.2 µs | 141.0 µs | 188.1 µs | 456.5 µs | 3,344 ops/s | 13.6 µs | 0.00 µs | 13.2 µs | 0 errors |
| load | oracle | 19,000 ops | 299.2 µs (3.1 %) | 135.9 µs | 170.0 µs | 208.4 µs | 539.6 µs | 720.4 µs | 3,341 ops/s | 22.6 µs | 0.00 µs | 61.7 µs | 0 errors |
| create-index | kds-group | 8 ops | 6,015.3 µs (26.2 %) | 1,742.2 µs | 2,792.4 µs | 8,043.0 µs | 10,077.5 µs | 10,077.5 µs | 165 ops/s | 73.3 µs | 0.00 µs | 4,411.5 µs | 0 errors |
| create-index | pg | 8 ops | 1,768.0 µs (105.1 %) | 686.1 µs | 814.7 µs | 2,264.8 µs | 2,899.9 µs | 2,899.9 µs | 557 ops/s | 30.2 µs | 0.00 µs | 1,424.5 µs | 0 errors |
| create-index | oracle | 8 ops | 3,954.8 µs (29.6 %) | 2,249.0 µs | 2,528.5 µs | 4,211.7 µs | 7,557.4 µs | 7,557.4 µs | 249 ops/s | 238.3 µs | 0.00 µs | 2,809.5 µs | 0 errors |
| pk-user | kds-group | 200 ops | 31.1 µs (3.6 %) | 29.2 µs | 29.9 µs | 30.1 µs | 35.9 µs | 44.1 µs | 29,762 ops/s | 15.1 µs | 0.47 µs | 7.5 µs | 0 errors |
| pk-user | pg | 200 ops | 32.5 µs (20.7 %) | 16.6 µs | 30.8 µs | 31.2 µs | 44.3 µs | 66.9 µs | 29,834 ops/s | 19.1 µs | 0.25 µs | 6.0 µs | 0 errors |
| pk-user | oracle | 200 ops | 29.4 µs (14.0 %) | 18.3 µs | 19.6 µs | 19.8 µs | 45.1 µs | 318.3 µs | 32,356 ops/s | 12.6 µs | 1.76 µs | 8.0 µs | 0 errors |
| loans-by-user | kds-group | 200 ops | 20.7 µs (36.1 %) | 15.2 µs | 16.8 µs | 18.0 µs | 35.1 µs | 45.2 µs | 45,704 ops/s | 6.9 µs | 0.49 µs | 8.7 µs | 0 errors |
| loans-by-user | pg | 200 ops | 22.7 µs (43.9 %) | 16.3 µs | 18.9 µs | 20.5 µs | 29.3 µs | 33.7 µs | 42,073 ops/s | 10.0 µs | 0.20 µs | 8.2 µs | 0 errors |
| loans-by-user | oracle | 200 ops | 27.7 µs (4.1 %) | 15.7 µs | 18.0 µs | 19.2 µs | 51.7 µs | 206.7 µs | 34,141 ops/s | 10.1 µs | 2.27 µs | 8.7 µs | 0 errors |
| loans-by-book | kds-group | 200 ops | 18.9 µs (22.2 %) | 15.1 µs | 16.6 µs | 17.3 µs | 28.2 µs | 34.8 µs | 51,913 ops/s | 6.3 µs | 0.37 µs | 8.3 µs | 0 errors |
| loans-by-book | pg | 200 ops | 21.3 µs (43.5 %) | 14.9 µs | 17.7 µs | 18.7 µs | 33.9 µs | 58.4 µs | 45,550 ops/s | 9.1 µs | 0.17 µs | 7.9 µs | 0 errors |
| loans-by-book | oracle | 200 ops | 20.9 µs (31.3 %) | 15.5 µs | 17.3 µs | 18.0 µs | 20.7 µs | 32.5 µs | 45,332 ops/s | 7.7 µs | 1.60 µs | 7.9 µs | 0 errors |
| resv-by-user | kds-group | 200 ops | 18.0 µs (32.6 %) | 15.6 µs | 16.6 µs | 17.0 µs | 19.2 µs | 44.0 µs | 52,546 ops/s | 6.2 µs | 0.22 µs | 7.6 µs | 0 errors |
| resv-by-user | pg | 200 ops | 22.4 µs (45.1 %) | 13.8 µs | 15.3 µs | 16.0 µs | 42.3 µs | 168.5 µs | 41,852 ops/s | 6.8 µs | 0.07 µs | 6.9 µs | 0 errors |
| resv-by-user | oracle | 200 ops | 24.5 µs (39.2 %) | 15.0 µs | 16.5 µs | 17.1 µs | 21.8 µs | 205.9 µs | 38,693 ops/s | 8.1 µs | 1.25 µs | 7.2 µs | 0 errors |
| books-by-author | kds-group | 200 ops | 18.3 µs (23.3 %) | 13.5 µs | 15.4 µs | 16.3 µs | 19.0 µs | 32.1 µs | 51,824 ops/s | 4.7 µs | 0.28 µs | 8.0 µs | 0 errors |
| books-by-author | pg | 200 ops | 21.5 µs (35.2 %) | 14.8 µs | 17.3 µs | 18.6 µs | 24.5 µs | 70.9 µs | 45,079 ops/s | 8.2 µs | 0.21 µs | 7.7 µs | 0 errors |
| books-by-author | oracle | 200 ops | 20.8 µs (12.9 %) | 14.6 µs | 16.5 µs | 17.1 µs | 22.4 µs | 65.3 µs | 45,656 ops/s | 7.6 µs | 1.39 µs | 7.6 µs | 0 errors |
| books-by-genre | kds-group | 200 ops | 61.4 µs (34.6 %) | 44.5 µs | 53.8 µs | 58.2 µs | 68.0 µs | 188.6 µs | 15,899 ops/s | 21.2 µs | 10.31 µs | 33.5 µs | 0 errors |
| books-by-genre | pg | 200 ops | 77.9 µs (15.4 %) | 60.2 µs | 66.5 µs | 70.9 µs | 87.8 µs | 232.8 µs | 12,792 ops/s | 46.2 µs | 4.59 µs | 21.9 µs | 0 errors |
| books-by-genre | oracle | 200 ops | 72.1 µs (2.4 %) | 61.0 µs | 64.6 µs | 66.4 µs | 74.5 µs | 228.7 µs | 13,783 ops/s | 35.2 µs | 21.92 µs | 30.7 µs | 0 errors |
| loans-by-daterange | kds-group | 200 ops | 233.9 µs (9.7 %) | 126.0 µs | 213.2 µs | 227.5 µs | 334.9 µs | 376.2 µs | 4,252 ops/s | 37.6 µs | 21.69 µs | 181.4 µs | 0 errors |
| loans-by-daterange | pg | 200 ops | 353.1 µs (4.5 %) | 217.9 µs | 329.3 µs | 340.2 µs | 520.8 µs | 582.3 µs | 2,819 ops/s | 126.8 µs | 8.12 µs | 264.0 µs | 0 errors |
| loans-by-daterange | oracle | 200 ops | 266.2 µs (8.0 %) | 111.7 µs | 235.5 µs | 250.4 µs | 411.4 µs | 439.8 µs | 3,742 ops/s | 99.3 µs | 73.27 µs | 151.3 µs | 0 errors |
| overdue | kds-group | 200 ops | 106.0 µs (5.2 %) | 23.3 µs | 96.8 µs | 101.8 µs | 132.0 µs | 198.0 µs | 9,333 ops/s | 23.0 µs | 12.83 µs | 73.2 µs | 0 errors |
| overdue | pg | 200 ops | 148.4 µs (6.4 %) | 17.8 µs | 127.6 µs | 130.7 µs | 325.4 µs | 421.3 µs | 6,673 ops/s | 84.3 µs | 5.24 µs | 54.8 µs | 0 errors |
| overdue | oracle | 200 ops | 283.1 µs (7.9 %) | 183.0 µs | 257.3 µs | 264.2 µs | 434.2 µs | 560.5 µs | 3,510 ops/s | 67.7 µs | 47.00 µs | 198.0 µs | 0 errors |
| join-loan-user | kds-group | 200 ops | 22.3 µs (26.3 %) | 16.5 µs | 18.4 µs | 18.9 µs | 37.0 µs | 42.3 µs | 42,763 ops/s | 4.6 µs | 0.29 µs | 12.1 µs | 0 errors |
| join-loan-user | pg | 200 ops | 21.2 µs (43.0 %) | 16.0 µs | 17.9 µs | 18.7 µs | 21.3 µs | 140.1 µs | 45,308 ops/s | 6.2 µs | 0.16 µs | 8.8 µs | 0 errors |
| join-loan-user | oracle | 200 ops | 22.8 µs (25.8 %) | 15.7 µs | 17.1 µs | 17.5 µs | 22.2 µs | 157.2 µs | 41,382 ops/s | 8.1 µs | 1.85 µs | 8.3 µs | 0 errors |
| join-no-literal | kds-group | 200 ops | 47.1 µs (9.6 %) | 42.1 µs | 43.6 µs | 44.6 µs | 56.5 µs | 64.6 µs | 20,944 ops/s | 8.8 µs | 3.50 µs | 32.0 µs | 0 errors |
| join-no-literal | pg | 200 ops | 242.3 µs (9.1 %) | 227.0 µs | 233.8 µs | 237.6 µs | 250.5 µs | 398.6 µs | 4,118 ops/s | 32.4 µs | 2.81 µs | 205.6 µs | 0 errors |
| join-no-literal | oracle | 200 ops | 74.5 µs (6.3 %) | 64.2 µs | 69.1 µs | 71.7 µs | 82.4 µs | 122.3 µs | 13,111 ops/s | 26.4 µs | 14.95 µs | 42.9 µs | 0 errors |
| exists-correlated | kds-group | 200 ops | 42.6 µs (10.7 %) | 34.9 µs | 36.3 µs | 37.0 µs | 56.4 µs | 174.8 µs | 23,132 ops/s | 4.9 µs | 0.42 µs | 29.7 µs | 0 errors |
| exists-correlated | pg | 200 ops | 32.0 µs (30.3 %) | 27.6 µs | 28.7 µs | 28.8 µs | 30.4 µs | 193.3 µs | 30,400 ops/s | 10.0 µs | 0.36 µs | 15.0 µs | 0 errors |
| exists-correlated | oracle | 200 ops | 27.7 µs (38.5 %) | 22.0 µs | 22.7 µs | 23.1 µs | 57.8 µs | 162.7 µs | 34,913 ops/s | 9.0 µs | 2.39 µs | 13.1 µs | 0 errors |
| count-by-user | kds-group | 200 ops | 16.7 µs (32.5 %) | 12.4 µs | 14.0 µs | 14.5 µs | 17.8 µs | 20.1 µs | 57,532 ops/s | 3.6 µs | 0.06 µs | 7.7 µs | 0 errors |
| count-by-user | pg | 200 ops | 19.0 µs (21.0 %) | 14.3 µs | 15.9 µs | 16.3 µs | 21.6 µs | 45.1 µs | 50,244 ops/s | 6.3 µs | 0.04 µs | 7.8 µs | 0 errors |
| count-by-user | oracle | 200 ops | 19.2 µs (4.7 %) | 14.2 µs | 15.2 µs | 15.3 µs | 17.4 µs | 161.1 µs | 49,569 ops/s | 6.9 µs | 0.81 µs | 6.8 µs | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ddl | kds-group | - | - | - | 119.00 µs | 0.00 µs | 4,944.75 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 4,944.8 µs); cpu.on_cpu (0.00, 118.2 µs); sched.run_queue (1.00, 0.8 µs) |
| ddl | pg | - | 0.00 µs | 0.00 µs | 384.00 µs | 33.25 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | CPU (0.25, 77.2 µs) |
| ddl | oracle | 455.75 µs | 498.25 µs | 951.75 µs | 2,735.25 µs | 0.00 µs | 235.00 µs | 0.00 µs | 0.25 µs | 0.00 µs | log file sync (1.00, 235.0 µs); events in waitclass Other (1.25, 8.5 µs); SQL*Net message to client (1.00, 0.2 µs) |
| load | kds-group | - | - | - | 12.09 µs | 0.00 µs | 305.58 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 305.6 µs); cpu.on_cpu (0.00, 11.9 µs); sched.run_queue (1.00, 0.2 µs) |
| load | pg | - | 0.00 µs | 0.00 µs | 1.77 µs | 0.00 µs | 117.16 µs | 0.00 µs | 8.38 µs | 0.00 µs | WalSync (0.11, 116.6 µs); ClientRead (0.01, 8.4 µs); CPU (0.00, 5.2 µs); WalWrite (0.00, 0.3 µs) |
| load | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 25.22 µs | 0.00 µs | 178.45 µs | 0.00 µs | 0.78 µs | 0.00 µs | log file sync (1.00, 178.4 µs); log file switch completion (0.00, 10.5 µs); SQL*Net message to client (1.00, 0.8 µs); log file switch (checkpoint incomplete) (0.00, 0.5 µs) |
| create-index | kds-group | - | - | - | 4,365.75 µs | 0.00 µs | 1,412.88 µs | 0.00 µs | 0.00 µs | 0.00 µs | cpu.on_cpu (0.00, 4,362.9 µs); commit.wal_flush (1.00, 1,412.9 µs); sched.run_queue (1.50, 2.9 µs) |
| create-index | pg | - | 0.00 µs | 0.00 µs | 1,338.50 µs | 0.00 µs | 312.62 µs | 0.00 µs | 0.00 µs | 0.00 µs | CPU (1.00, 947.5 µs); WalSync (0.25, 312.6 µs) |
| create-index | oracle | 65.25 µs | 400.25 µs | 462.00 µs | 2,423.88 µs | 718.75 µs | 112.88 µs | 0.00 µs | 0.50 µs | 0.00 µs | direct path write (4.62, 718.8 µs); events in waitclass Other (2.50, 517.9 µs); log file sync (1.00, 112.9 µs); SQL*Net message to client (1.00, 0.5 µs) |
| pk-user | kds-group | - | - | 1.01 µs | 1.58 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.47 µs | cpu.on_cpu (0.00, 2.5 µs); sched.run_queue (1.00, 0.1 µs) |
| pk-user | pg | - | 0.00 µs | 0.00 µs | 0.91 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.81 µs | 0.27 µs | ClientRead (0.01, 5.8 µs) |
| pk-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.87 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.27 µs | 1.76 µs | SQL*Net message to client (1.00, 0.3 µs) |
| loans-by-user | kds-group | - | - | 1.23 µs | 3.54 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.49 µs | cpu.on_cpu (0.00, 4.7 µs); sched.run_queue (1.00, 0.1 µs) |
| loans-by-user | pg | - | 0.00 µs | 0.00 µs | 2.83 µs | 0.84 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | none |
| loans-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.42 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.22 µs | 2.27 µs | SQL*Net message to client (1.00, 0.2 µs) |
| loans-by-book | kds-group | - | - | 1.18 µs | 3.42 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.37 µs | cpu.on_cpu (0.00, 4.6 µs); sched.run_queue (1.00, 0.1 µs) |
| loans-by-book | pg | - | 0.00 µs | 0.00 µs | 2.71 µs | 0.22 µs | 0.00 µs | 0.00 µs | 1.24 µs | 0.10 µs | ClientRead (0.01, 1.2 µs) |
| loans-by-book | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.03 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | 1.60 µs | SQL*Net message to client (1.00, 0.2 µs) |
| resv-by-user | kds-group | - | - | 1.14 µs | 2.73 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.22 µs | cpu.on_cpu (0.00, 3.8 µs); sched.run_queue (1.00, 0.1 µs) |
| resv-by-user | pg | - | 0.00 µs | 0.00 µs | 1.78 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.06 µs | none |
| resv-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.19 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.25 µs | 1.25 µs | SQL*Net message to client (1.00, 0.2 µs) |
| books-by-author | kds-group | - | - | 1.20 µs | 3.17 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.28 µs | cpu.on_cpu (0.00, 4.3 µs); sched.run_queue (1.00, 0.1 µs) |
| books-by-author | pg | - | 0.00 µs | 0.00 µs | 2.26 µs | 0.03 µs | 0.00 µs | 0.00 µs | 5.13 µs | 0.16 µs | ClientRead (0.01, 5.1 µs) |
| books-by-author | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.60 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.26 µs | 1.39 µs | SQL*Net message to client (1.00, 0.3 µs) |
| books-by-genre | kds-group | - | - | 1.25 µs | 28.09 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.31 µs | cpu.on_cpu (0.00, 29.3 µs); sched.run_queue (1.00, 0.1 µs) |
| books-by-genre | pg | - | 0.00 µs | 0.00 µs | 16.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 15.96 µs | 4.27 µs | CPU (0.02, 17.0 µs); ClientRead (0.01, 16.0 µs) |
| books-by-genre | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 25.83 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.27 µs | 21.92 µs | SQL*Net message to client (1.00, 0.3 µs) |
| loans-by-daterange | kds-group | - | - | 1.29 µs | 175.24 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 21.69 µs | cpu.on_cpu (0.00, 176.2 µs); sched.run_queue (2.00, 0.3 µs) |
| loans-by-daterange | pg | - | 0.00 µs | 0.00 µs | 257.93 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.25 µs | 8.10 µs | CPU (0.26, 264.1 µs); ClientRead (0.01, 5.2 µs) |
| loans-by-daterange | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 141.11 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.53 µs | 73.27 µs | SQL*Net message to client (1.00, 0.5 µs) |
| overdue | kds-group | - | - | 1.55 µs | 66.86 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 12.83 µs | cpu.on_cpu (0.00, 68.4 µs); sched.run_queue (1.00, 0.1 µs) |
| overdue | pg | - | 0.00 µs | 0.00 µs | 47.37 µs | 0.06 µs | 0.00 µs | 0.00 µs | 10.82 µs | 5.20 µs | CPU (0.04, 40.6 µs); ClientRead (0.01, 10.8 µs) |
| overdue | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 189.38 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.41 µs | 47.00 µs | SQL*Net message to client (1.00, 0.4 µs) |
| join-loan-user | kds-group | - | - | 3.90 µs | 4.26 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.29 µs | cpu.on_cpu (0.00, 8.2 µs); sched.run_queue (1.00, 0.1 µs) |
| join-loan-user | pg | - | 0.00 µs | 0.00 µs | 3.46 µs | 0.05 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.17 µs | none |
| join-loan-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.90 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.25 µs | 1.85 µs | SQL*Net message to client (1.00, 0.2 µs) |
| join-no-literal | kds-group | - | - | 3.77 µs | 24.20 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.50 µs | cpu.on_cpu (0.00, 27.9 µs); sched.run_queue (1.00, 0.1 µs) |
| join-no-literal | pg | - | 0.00 µs | 0.00 µs | 198.56 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.50 µs | 3.03 µs | CPU (0.19, 198.1 µs); ClientRead (0.01, 5.5 µs) |
| join-no-literal | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 34.98 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.13 µs | 14.95 µs | events in waitclass Other (5.00, 5.7 µs); SQL*Net message to client (1.00, 0.1 µs) |
| exists-correlated | kds-group | - | - | 1.93 µs | 23.95 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.42 µs | cpu.on_cpu (0.00, 25.8 µs); sched.run_queue (1.00, 0.1 µs) |
| exists-correlated | pg | - | 0.00 µs | 0.00 µs | 9.85 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.51 µs | 0.35 µs | CPU (0.01, 4.2 µs); ClientRead (0.01, 1.5 µs) |
| exists-correlated | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 9.08 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.16 µs | 2.39 µs | SQL*Net message to client (1.00, 0.2 µs) |
| count-by-user | kds-group | - | - | 1.04 µs | 3.18 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.06 µs | cpu.on_cpu (0.00, 4.2 µs); sched.run_queue (1.00, 0.1 µs) |
| count-by-user | pg | - | 0.00 µs | 0.00 µs | 2.47 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.45 µs | 0.04 µs | ClientRead (0.01, 4.5 µs) |
| count-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.97 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.18 µs | 0.81 µs | SQL*Net message to client (1.00, 0.2 µs) |

### s3 `loans10000-unindexed`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ddl | kds-group | 4 ops | 372.4 µs (47.9 %) | 244.9 µs | 287.2 µs | 439.6 µs | 490.6 µs | 490.6 µs | 1,863 ops/s | 42.5 µs | 0.00 µs | 132.9 µs | 0 errors |
| ddl | pg | 4 ops | 693.2 µs (33.5 %) | 450.6 µs | 465.6 µs | 489.9 µs | 1,401.8 µs | 1,401.8 µs | 1,362 ops/s | 28.7 µs | 0.00 µs | 470.8 µs | 0 errors |
| ddl | oracle | 4 ops | 4,212.2 µs (65.2 %) | 3,161.7 µs | 3,663.8 µs | 4,025.2 µs | 5,986.4 µs | 5,986.4 µs | 225 ops/s | 366.4 µs | 0.00 µs | 3,299.0 µs | 0 errors |
| load | kds-group | 19,000 ops | 153.1 µs (4.7 %) | 123.5 µs | 137.7 µs | 140.4 µs | 175.7 µs | 345.6 µs | 6,526 ops/s | 10.2 µs | 0.00 µs | 14.1 µs | 0 errors |
| load | pg | 19,000 ops | 176.9 µs (110.4 %) | 124.1 µs | 137.4 µs | 140.0 µs | 179.7 µs | 419.2 µs | 5,652 ops/s | 13.2 µs | 0.00 µs | 12.2 µs | 0 errors |
| load | oracle | 19,000 ops | 301.6 µs (41.4 %) | 137.1 µs | 172.2 µs | 211.1 µs | 538.4 µs | 731.2 µs | 3,313 ops/s | 23.0 µs | 0.00 µs | 62.1 µs | 0 errors |
| pk-user | kds-group | 200 ops | 24.4 µs (43.2 %) | 20.2 µs | 22.7 µs | 23.0 µs | 31.1 µs | 33.7 µs | 38,779 ops/s | 9.5 µs | 0.26 µs | 7.0 µs | 0 errors |
| pk-user | pg | 200 ops | 34.4 µs (24.3 %) | 26.8 µs | 29.2 µs | 31.0 µs | 42.3 µs | 72.7 µs | 27,752 ops/s | 21.1 µs | 0.25 µs | 6.1 µs | 0 errors |
| pk-user | oracle | 200 ops | 25.7 µs (41.9 %) | 15.4 µs | 16.7 µs | 17.1 µs | 30.9 µs | 207.4 µs | 37,281 ops/s | 10.1 µs | 1.34 µs | 6.8 µs | 0 errors |
| loans-by-user | kds-group | 200 ops | 131.9 µs (2.2 %) | 122.8 µs | 128.7 µs | 129.8 µs | 136.7 µs | 139.7 µs | 7,529 ops/s | 8.6 µs | 0.59 µs | 116.6 µs | 0 errors |
| loans-by-user | pg | 200 ops | 215.1 µs (2.9 %) | 190.9 µs | 199.1 µs | 203.0 µs | 279.7 µs | 471.0 µs | 4,624 ops/s | 20.6 µs | 0.39 µs | 180.1 µs | 0 errors |
| loans-by-user | oracle | 200 ops | 89.0 µs (39.0 %) | 61.1 µs | 74.4 µs | 77.3 µs | 148.8 µs | 333.9 µs | 11,096 ops/s | 12.5 µs | 2.90 µs | 66.0 µs | 0 errors |
| loans-by-book | kds-group | 200 ops | 136.6 µs (8.5 %) | 129.3 µs | 132.7 µs | 134.0 µs | 226.3 µs | 247.9 µs | 7,240 ops/s | 11.9 µs | 0.68 µs | 116.9 µs | 0 errors |
| loans-by-book | pg | 200 ops | 232.8 µs (7.7 %) | 199.1 µs | 210.2 µs | 217.8 µs | 392.7 µs | 497.2 µs | 4,266 ops/s | 25.6 µs | 0.44 µs | 185.8 µs | 0 errors |
| loans-by-book | oracle | 200 ops | 89.8 µs (16.7 %) | 71.4 µs | 85.3 µs | 86.8 µs | 90.7 µs | 145.9 µs | 11,015 ops/s | 13.0 µs | 2.79 µs | 71.5 µs | 0 errors |
| resv-by-user | kds-group | 200 ops | 83.5 µs (18.9 %) | 72.3 µs | 77.0 µs | 79.7 µs | 85.3 µs | 220.0 µs | 11,818 ops/s | 12.3 µs | 0.43 µs | 61.5 µs | 0 errors |
| resv-by-user | pg | 200 ops | 131.8 µs (32.8 %) | 103.0 µs | 111.2 µs | 118.0 µs | 275.6 µs | 392.1 µs | 7,520 ops/s | 22.3 µs | 0.24 µs | 93.3 µs | 0 errors |
| resv-by-user | oracle | 200 ops | 58.6 µs (32.5 %) | 42.7 µs | 47.0 µs | 48.0 µs | 75.4 µs | 307.4 µs | 16,746 ops/s | 11.9 µs | 1.96 µs | 35.6 µs | 0 errors |
| books-by-author | kds-group | 200 ops | 47.5 µs (9.9 %) | 40.9 µs | 42.4 µs | 43.0 µs | 54.8 µs | 189.4 µs | 20,388 ops/s | 9.5 µs | 0.56 µs | 29.8 µs | 0 errors |
| books-by-author | pg | 200 ops | 68.5 µs (31.8 %) | 57.1 µs | 61.6 µs | 66.0 µs | 77.9 µs | 170.7 µs | 14,414 ops/s | 18.3 µs | 0.44 µs | 44.1 µs | 0 errors |
| books-by-author | oracle | 200 ops | 53.8 µs (23.1 %) | 33.4 µs | 37.8 µs | 41.8 µs | 160.6 µs | 308.4 µs | 18,277 ops/s | 14.3 µs | 2.82 µs | 27.9 µs | 0 errors |
| books-by-genre | kds-group | 200 ops | 73.3 µs (8.4 %) | 51.4 µs | 56.9 µs | 70.7 µs | 86.1 µs | 94.2 µs | 13,395 ops/s | 26.9 µs | 13.01 µs | 37.7 µs | 0 errors |
| books-by-genre | pg | 200 ops | 117.9 µs (19.5 %) | 95.9 µs | 100.4 µs | 104.7 µs | 137.0 µs | 326.7 µs | 8,417 ops/s | 52.3 µs | 5.25 µs | 57.3 µs | 0 errors |
| books-by-genre | oracle | 200 ops | 84.3 µs (8.7 %) | 62.4 µs | 66.7 µs | 69.5 µs | 210.5 µs | 297.6 µs | 11,792 ops/s | 40.1 µs | 24.95 µs | 32.2 µs | 0 errors |
| loans-by-daterange | kds-group | 200 ops | 224.4 µs (13.7 %) | 126.5 µs | 206.4 µs | 223.9 µs | 283.4 µs | 337.1 µs | 4,438 ops/s | 32.7 µs | 18.91 µs | 182.3 µs | 0 errors |
| loans-by-daterange | pg | 200 ops | 344.2 µs (4.5 %) | 214.6 µs | 326.3 µs | 336.8 µs | 423.3 µs | 548.9 µs | 2,902 ops/s | 126.4 µs | 7.96 µs | 260.8 µs | 0 errors |
| loans-by-daterange | oracle | 200 ops | 265.8 µs (7.7 %) | 113.5 µs | 239.7 µs | 255.7 µs | 419.0 µs | 521.8 µs | 3,743 ops/s | 100.4 µs | 75.12 µs | 157.6 µs | 0 errors |
| overdue | kds-group | 200 ops | 219.9 µs (14.1 %) | 167.9 µs | 214.3 µs | 217.4 µs | 230.3 µs | 337.5 µs | 4,530 ops/s | 26.0 µs | 14.61 µs | 186.1 µs | 0 errors |
| overdue | pg | 200 ops | 323.1 µs (10.3 %) | 217.2 µs | 308.5 µs | 319.5 µs | 399.9 µs | 500.1 µs | 3,085 ops/s | 88.1 µs | 5.55 µs | 273.4 µs | 0 errors |
| overdue | oracle | 200 ops | 249.9 µs (9.0 %) | 112.4 µs | 211.4 µs | 232.1 µs | 419.5 µs | 545.1 µs | 3,976 ops/s | 69.3 µs | 49.49 µs | 158.6 µs | 0 errors |
| join-loan-user | kds-group | 200 ops | 137.2 µs (7.0 %) | 124.0 µs | 130.5 µs | 133.0 µs | 141.7 µs | 233.6 µs | 7,234 ops/s | 9.9 µs | 0.63 µs | 118.3 µs | 0 errors |
| join-loan-user | pg | 200 ops | 216.9 µs (17.1 %) | 185.8 µs | 195.9 µs | 202.3 µs | 338.3 µs | 503.6 µs | 4,587 ops/s | 15.9 µs | 0.40 µs | 185.2 µs | 0 errors |
| join-loan-user | oracle | 200 ops | 85.2 µs (15.1 %) | 62.5 µs | 75.1 µs | 76.4 µs | 107.4 µs | 264.0 µs | 11,591 ops/s | 10.4 µs | 2.52 µs | 65.3 µs | 0 errors |
| join-no-literal | kds-group | 200 ops | 209.8 µs (8.4 %) | 187.3 µs | 189.8 µs | 193.5 µs | 344.0 µs | 378.1 µs | 4,728 ops/s | 27.4 µs | 10.12 µs | 166.9 µs | 0 errors |
| join-no-literal | pg | 200 ops | 272.7 µs (6.1 %) | 227.5 µs | 234.4 µs | 242.5 µs | 471.7 µs | 515.7 µs | 3,654 ops/s | 47.7 µs | 4.23 µs | 202.8 µs | 0 errors |
| join-no-literal | oracle | 200 ops | 143.7 µs (7.6 %) | 112.4 µs | 119.2 µs | 128.4 µs | 270.3 µs | 415.5 µs | 6,880 ops/s | 35.4 µs | 18.61 µs | 95.2 µs | 0 errors |
| exists-correlated | kds-group | 200 ops | 269.0 µs (4.1 %) | 246.1 µs | 257.3 µs | 259.7 µs | 359.5 µs | 427.8 µs | 3,693 ops/s | 19.5 µs | 1.65 µs | 234.0 µs | 0 errors |
| exists-correlated | pg | 200 ops | 652.2 µs (2.5 %) | 612.5 µs | 630.3 µs | 634.7 µs | 872.8 µs | 991.9 µs | 1,530 ops/s | 59.9 µs | 1.94 µs | 576.2 µs | 0 errors |
| exists-correlated | oracle | 200 ops | 82.0 µs (4.7 %) | 70.6 µs | 74.5 µs | 77.2 µs | 86.6 µs | 108.5 µs | 11,953 ops/s | 12.8 µs | 3.46 µs | 63.2 µs | 0 errors |
| count-by-user | kds-group | 200 ops | 146.2 µs (14.8 %) | 130.1 µs | 134.0 µs | 137.4 µs | 146.3 µs | 305.9 µs | 6,746 ops/s | 15.4 µs | 0.30 µs | 116.8 µs | 0 errors |
| count-by-user | pg | 200 ops | 251.1 µs (8.0 %) | 189.7 µs | 196.8 µs | 200.8 µs | 481.8 µs | 555.4 µs | 3,951 ops/s | 24.9 µs | 0.15 µs | 185.5 µs | 0 errors |
| count-by-user | oracle | 200 ops | 112.7 µs (24.5 %) | 65.5 µs | 79.8 µs | 83.6 µs | 288.3 µs | 423.9 µs | 8,762 ops/s | 17.6 µs | 2.20 µs | 70.7 µs | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ddl | kds-group | - | - | - | 93.50 µs | 0.00 µs | 143.75 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 143.8 µs); cpu.on_cpu (0.00, 93.2 µs); sched.run_queue (1.00, 0.2 µs) |
| ddl | pg | - | 0.00 µs | 0.00 µs | 466.25 µs | 33.50 µs | 72.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | WalSync (0.25, 72.5 µs) |
| ddl | oracle | 538.00 µs | 500.75 µs | 1,047.00 µs | 2,877.25 µs | 0.00 µs | 436.50 µs | 0.00 µs | 0.75 µs | 0.00 µs | log file sync (1.00, 436.5 µs); events in waitclass Other (2.00, 11.2 µs); SQL*Net message to client (1.00, 0.8 µs) |
| load | kds-group | - | - | - | 5.52 µs | 0.00 µs | 124.13 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 124.1 µs); cpu.on_cpu (0.00, 5.4 µs); sched.run_queue (1.00, 0.1 µs) |
| load | pg | - | 0.00 µs | 0.00 µs | 1.73 µs | 0.00 µs | 114.54 µs | 0.00 µs | 8.30 µs | 0.00 µs | WalSync (0.11, 114.0 µs); ClientRead (0.01, 8.3 µs); CPU (0.00, 4.6 µs); WalInitSync (0.00, 0.3 µs) |
| load | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 20.20 µs | 0.00 µs | 180.15 µs | 0.00 µs | 0.79 µs | 0.00 µs | log file sync (1.00, 180.2 µs); log file switch completion (0.00, 5.4 µs); SQL*Net message to client (1.00, 0.8 µs); log file switch (checkpoint incomplete) (0.00, 0.5 µs) |
| pk-user | kds-group | - | - | 1.03 µs | 1.58 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.26 µs | cpu.on_cpu (0.00, 2.6 µs); sched.run_queue (1.00, 0.1 µs) |
| pk-user | pg | - | 0.00 µs | 0.00 µs | 1.16 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | none |
| pk-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.65 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.27 µs | 1.34 µs | SQL*Net message to client (1.00, 0.3 µs) |
| loans-by-user | kds-group | - | - | 1.15 µs | 111.28 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.59 µs | cpu.on_cpu (0.00, 112.1 µs); sched.run_queue (2.00, 0.3 µs) |
| loans-by-user | pg | - | 0.00 µs | 0.00 µs | 176.25 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.42 µs | CPU (0.15, 162.8 µs) |
| loans-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 60.34 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.28 µs | 2.90 µs | SQL*Net message to client (1.00, 0.3 µs) |
| loans-by-book | kds-group | - | - | 1.12 µs | 111.41 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.68 µs | cpu.on_cpu (0.00, 112.2 µs); sched.run_queue (2.00, 0.3 µs) |
| loans-by-book | pg | - | 0.00 µs | 0.00 µs | 180.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 11.38 µs | 0.42 µs | CPU (0.15, 155.3 µs); ClientRead (0.01, 11.4 µs) |
| loans-by-book | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 66.47 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.28 µs | 2.79 µs | SQL*Net message to client (1.00, 0.3 µs) |
| resv-by-user | kds-group | - | - | 1.10 µs | 55.98 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.43 µs | cpu.on_cpu (0.00, 57.0 µs); sched.run_queue (1.00, 0.1 µs) |
| resv-by-user | pg | - | 0.00 µs | 0.00 µs | 88.89 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.68 µs | 0.19 µs | CPU (0.06, 62.7 µs); ClientRead (0.01, 10.7 µs) |
| resv-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 30.81 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.26 µs | 1.96 µs | SQL*Net message to client (1.00, 0.3 µs) |
| books-by-author | kds-group | - | - | 1.11 µs | 24.46 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.56 µs | cpu.on_cpu (0.00, 25.5 µs); sched.run_queue (1.00, 0.1 µs) |
| books-by-author | pg | - | 0.00 µs | 0.00 µs | 39.92 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.16 µs | CPU (0.03, 24.6 µs) |
| books-by-author | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 21.78 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.28 µs | 2.82 µs | SQL*Net message to client (1.00, 0.3 µs) |
| books-by-genre | kds-group | - | - | 1.13 µs | 32.13 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 13.01 µs | cpu.on_cpu (0.00, 33.2 µs); sched.run_queue (1.00, 0.1 µs) |
| books-by-genre | pg | - | 0.00 µs | 0.00 µs | 54.13 µs | 0.00 µs | 0.00 µs | 0.00 µs | 6.17 µs | 5.48 µs | CPU (0.04, 48.5 µs); ClientRead (0.01, 6.2 µs) |
| books-by-genre | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 26.53 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.26 µs | 24.95 µs | SQL*Net message to client (1.00, 0.3 µs) |
| loans-by-daterange | kds-group | - | - | 1.23 µs | 176.41 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 18.91 µs | cpu.on_cpu (0.00, 177.3 µs); sched.run_queue (2.00, 0.3 µs) |
| loans-by-daterange | pg | - | 0.00 µs | 0.00 µs | 258.98 µs | 0.00 µs | 0.00 µs | 0.00 µs | 14.33 µs | 8.11 µs | CPU (0.20, 210.4 µs); ClientRead (0.01, 14.3 µs) |
| loans-by-daterange | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 144.24 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.48 µs | 75.12 µs | SQL*Net message to client (1.00, 0.5 µs) |
| overdue | kds-group | - | - | 1.38 µs | 179.72 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 14.61 µs | cpu.on_cpu (0.00, 180.8 µs); sched.run_queue (2.00, 0.3 µs) |
| overdue | pg | - | 0.00 µs | 0.00 µs | 267.58 µs | 0.00 µs | 0.00 µs | 0.00 µs | 11.73 µs | 5.34 µs | CPU (0.23, 236.7 µs); ClientRead (0.01, 11.7 µs) |
| overdue | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 150.78 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.47 µs | 49.49 µs | SQL*Net message to client (1.00, 0.5 µs) |
| join-loan-user | kds-group | - | - | 3.49 µs | 110.65 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.63 µs | cpu.on_cpu (0.00, 114.1 µs); sched.run_queue (1.00, 0.1 µs) |
| join-loan-user | pg | - | 0.00 µs | 0.00 µs | 174.36 µs | 0.00 µs | 0.00 µs | 0.00 µs | 21.64 µs | 0.41 µs | CPU (0.15, 157.3 µs); ClientRead (0.02, 21.6 µs) |
| join-loan-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 60.60 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.29 µs | 2.52 µs | SQL*Net message to client (1.00, 0.3 µs) |
| join-no-literal | kds-group | - | - | 3.54 µs | 158.82 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.12 µs | cpu.on_cpu (0.00, 162.3 µs); sched.run_queue (1.00, 0.1 µs) |
| join-no-literal | pg | - | 0.00 µs | 0.00 µs | 192.64 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.87 µs | 4.26 µs | CPU (0.18, 189.6 µs); ClientRead (0.01, 10.9 µs) |
| join-no-literal | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 85.83 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.12 µs | 18.61 µs | events in waitclass Other (5.00, 6.0 µs); SQL*Net message to client (1.00, 0.1 µs) |
| exists-correlated | kds-group | - | - | 1.92 µs | 227.32 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.65 µs | cpu.on_cpu (0.00, 229.2 µs); sched.run_queue (1.00, 0.1 µs) |
| exists-correlated | pg | - | 0.00 µs | 0.00 µs | 550.86 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.35 µs | 1.89 µs | CPU (0.51, 536.5 µs); ClientRead (0.01, 5.3 µs) |
| exists-correlated | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 57.85 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 3.46 µs | SQL*Net message to client (1.00, 0.2 µs) |
| count-by-user | kds-group | - | - | 0.99 µs | 111.44 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.30 µs | cpu.on_cpu (0.00, 112.1 µs); sched.run_queue (2.00, 0.3 µs) |
| count-by-user | pg | - | 0.00 µs | 0.00 µs | 174.81 µs | 0.00 µs | 0.00 µs | 0.00 µs | 14.98 µs | 0.12 µs | CPU (0.12, 132.1 µs); ClientRead (0.01, 15.0 µs) |
| count-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 63.11 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.36 µs | 2.20 µs | SQL*Net message to client (1.00, 0.4 µs) |

### s3 `loans200-indexed`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ddl | kds-group | 4 ops | 361.1 µs (18.4 %) | 250.2 µs | 314.5 µs | 341.6 µs | 460.5 µs | 460.5 µs | 2,123 ops/s | 30.6 µs | 0.00 µs | 115.0 µs | 0 errors |
| ddl | pg | 4 ops | 665.3 µs (23.2 %) | 433.6 µs | 462.3 µs | 468.4 µs | 1,180.1 µs | 1,180.1 µs | 1,424 ops/s | 20.0 µs | 0.00 µs | 464.5 µs | 0 errors |
| ddl | oracle | 4 ops | 3,920.5 µs (5.4 %) | 3,125.8 µs | 3,372.8 µs | 3,679.8 µs | 5,318.9 µs | 5,318.9 µs | 243 ops/s | 257.4 µs | 0.00 µs | 3,229.8 µs | 0 errors |
| load | kds-group | 380 ops | 153.6 µs (18.8 %) | 132.6 µs | 135.9 µs | 139.5 µs | 242.9 µs | 326.3 µs | 6,481 ops/s | 11.3 µs | 0.00 µs | 12.3 µs | 0 errors |
| load | pg | 380 ops | 145.6 µs (143.2 %) | 126.2 µs | 137.3 µs | 139.1 µs | 165.9 µs | 339.5 µs | 6,857 ops/s | 12.6 µs | 0.00 µs | 10.7 µs | 0 errors |
| load | oracle | 380 ops | 336.8 µs (64.2 %) | 159.0 µs | 189.6 µs | 323.0 µs | 629.9 µs | 906.4 µs | 2,957 ops/s | 29.1 µs | 0.00 µs | 72.2 µs | 0 errors |
| create-index | kds-group | 8 ops | 448.7 µs (12.9 %) | 306.0 µs | 323.2 µs | 411.3 µs | 810.4 µs | 810.4 µs | 2,099 ops/s | 43.3 µs | 0.00 µs | 120.3 µs | 0 errors |
| create-index | pg | 8 ops | 316.9 µs (130.8 %) | 244.6 µs | 249.0 µs | 278.2 µs | 521.6 µs | 521.6 µs | 3,027 ops/s | 26.9 µs | 0.00 µs | 136.8 µs | 0 errors |
| create-index | oracle | 8 ops | 2,185.4 µs (65.0 %) | 1,685.4 µs | 1,816.9 µs | 1,940.3 µs | 3,280.4 µs | 3,280.4 µs | 444 ops/s | 243.6 µs | 0.00 µs | 1,410.0 µs | 0 errors |
| pk-user | kds-group | 200 ops | 24.8 µs (9.0 %) | 21.9 µs | 22.8 µs | 23.0 µs | 28.4 µs | 32.3 µs | 37,396 ops/s | 11.4 µs | 0.33 µs | 6.9 µs | 0 errors |
| pk-user | pg | 200 ops | 25.1 µs (58.7 %) | 19.6 µs | 20.1 µs | 25.3 µs | 26.7 µs | 29.8 µs | 38,917 ops/s | 14.1 µs | 0.23 µs | 6.0 µs | 0 errors |
| pk-user | oracle | 200 ops | 30.8 µs (45.9 %) | 18.8 µs | 19.2 µs | 19.6 µs | 52.1 µs | 271.7 µs | 30,600 ops/s | 12.4 µs | 1.93 µs | 7.3 µs | 0 errors |
| loans-by-user | kds-group | 200 ops | 22.3 µs (26.7 %) | 17.3 µs | 18.2 µs | 20.4 µs | 33.7 µs | 41.2 µs | 42,019 ops/s | 8.3 µs | 0.55 µs | 8.7 µs | 0 errors |
| loans-by-user | pg | 200 ops | 29.7 µs (62.1 %) | 16.8 µs | 18.6 µs | 19.7 µs | 30.1 µs | 210.0 µs | 32,522 ops/s | 10.9 µs | 0.21 µs | 9.7 µs | 0 errors |
| loans-by-user | oracle | 200 ops | 20.4 µs (15.8 %) | 15.2 µs | 16.6 µs | 17.3 µs | 21.7 µs | 55.9 µs | 46,541 ops/s | 9.0 µs | 2.01 µs | 6.5 µs | 0 errors |
| loans-by-book | kds-group | 200 ops | 18.5 µs (15.9 %) | 15.0 µs | 16.7 µs | 17.7 µs | 20.5 µs | 33.6 µs | 50,705 ops/s | 5.8 µs | 0.32 µs | 8.2 µs | 0 errors |
| loans-by-book | pg | 200 ops | 19.8 µs (20.9 %) | 16.6 µs | 17.5 µs | 18.4 µs | 20.4 µs | 28.2 µs | 49,592 ops/s | 6.4 µs | 0.12 µs | 8.8 µs | 0 errors |
| loans-by-book | oracle | 200 ops | 24.5 µs (12.2 %) | 14.9 µs | 16.4 µs | 16.9 µs | 19.3 µs | 169.7 µs | 38,952 ops/s | 8.2 µs | 1.62 µs | 7.2 µs | 0 errors |
| resv-by-user | kds-group | 200 ops | 18.1 µs (31.2 %) | 14.1 µs | 15.6 µs | 16.3 µs | 31.2 µs | 54.0 µs | 53,366 ops/s | 5.6 µs | 0.21 µs | 7.3 µs | 0 errors |
| resv-by-user | pg | 200 ops | 19.7 µs (77.4 %) | 14.7 µs | 15.5 µs | 16.0 µs | 18.1 µs | 152.3 µs | 48,982 ops/s | 5.7 µs | 0.08 µs | 6.8 µs | 0 errors |
| resv-by-user | oracle | 200 ops | 22.7 µs (49.3 %) | 14.4 µs | 16.8 µs | 17.5 µs | 20.2 µs | 233.1 µs | 41,437 ops/s | 8.8 µs | 1.47 µs | 7.5 µs | 0 errors |
| books-by-author | kds-group | 200 ops | 18.1 µs (0.7 %) | 13.0 µs | 15.3 µs | 16.0 µs | 19.2 µs | 33.3 µs | 52,991 ops/s | 5.1 µs | 0.28 µs | 7.4 µs | 0 errors |
| books-by-author | pg | 200 ops | 16.5 µs (37.9 %) | 13.6 µs | 15.2 µs | 16.4 µs | 18.1 µs | 20.3 µs | 58,557 ops/s | 6.5 µs | 0.16 µs | 6.0 µs | 0 errors |
| books-by-author | oracle | 200 ops | 22.0 µs (11.8 %) | 14.5 µs | 16.6 µs | 17.8 µs | 19.9 µs | 211.1 µs | 43,637 ops/s | 8.1 µs | 1.57 µs | 7.3 µs | 0 errors |
| books-by-genre | kds-group | 200 ops | 17.1 µs (19.0 %) | 13.7 µs | 14.4 µs | 15.5 µs | 31.4 µs | 33.1 µs | 54,889 ops/s | 5.3 µs | 0.24 µs | 7.3 µs | 0 errors |
| books-by-genre | pg | 200 ops | 15.9 µs (17.6 %) | 14.2 µs | 15.1 µs | 15.8 µs | 17.2 µs | 18.4 µs | 60,047 ops/s | 5.8 µs | 0.13 µs | 5.8 µs | 0 errors |
| books-by-genre | oracle | 200 ops | 24.9 µs (32.5 %) | 15.1 µs | 16.3 µs | 17.1 µs | 20.4 µs | 288.6 µs | 37,986 ops/s | 8.0 µs | 1.46 µs | 7.1 µs | 0 errors |
| loans-by-daterange | kds-group | 200 ops | 20.6 µs (33.6 %) | 17.1 µs | 19.3 µs | 20.4 µs | 23.1 µs | 26.5 µs | 46,061 ops/s | 6.1 µs | 0.46 µs | 10.7 µs | 0 errors |
| loans-by-daterange | pg | 200 ops | 25.2 µs (11.3 %) | 17.6 µs | 20.5 µs | 21.1 µs | 26.2 µs | 72.4 µs | 38,817 ops/s | 7.1 µs | 0.22 µs | 10.7 µs | 0 errors |
| loans-by-daterange | oracle | 200 ops | 28.2 µs (48.0 %) | 18.9 µs | 21.3 µs | 21.9 µs | 35.3 µs | 234.4 µs | 33,467 ops/s | 9.7 µs | 2.50 µs | 12.0 µs | 0 errors |
| overdue | kds-group | 200 ops | 20.9 µs (22.9 %) | 15.5 µs | 17.7 µs | 18.4 µs | 22.5 µs | 40.8 µs | 44,619 ops/s | 5.4 µs | 0.34 µs | 9.3 µs | 0 errors |
| overdue | pg | 200 ops | 23.0 µs (39.3 %) | 17.2 µs | 19.5 µs | 20.2 µs | 26.5 µs | 39.1 µs | 42,302 ops/s | 7.3 µs | 0.16 µs | 10.7 µs | 0 errors |
| overdue | oracle | 200 ops | 28.6 µs (2.3 %) | 20.8 µs | 23.4 µs | 24.5 µs | 29.4 µs | 195.5 µs | 32,982 ops/s | 9.5 µs | 2.19 µs | 13.9 µs | 0 errors |
| join-loan-user | kds-group | 200 ops | 22.4 µs (10.7 %) | 19.6 µs | 20.4 µs | 20.8 µs | 36.6 µs | 50.9 µs | 43,005 ops/s | 5.7 µs | 0.37 µs | 11.8 µs | 0 errors |
| join-loan-user | pg | 200 ops | 23.6 µs (54.6 %) | 17.9 µs | 20.2 µs | 20.7 µs | 50.2 µs | 55.3 µs | 40,963 ops/s | 6.9 µs | 0.20 µs | 11.0 µs | 0 errors |
| join-loan-user | oracle | 200 ops | 22.7 µs (18.2 %) | 15.5 µs | 17.2 µs | 18.0 µs | 21.5 µs | 80.6 µs | 42,448 ops/s | 9.2 µs | 2.15 µs | 7.3 µs | 0 errors |
| join-no-literal | kds-group | 200 ops | 53.3 µs (8.8 %) | 44.1 µs | 47.3 µs | 50.9 µs | 60.6 µs | 160.9 µs | 18,481 ops/s | 13.5 µs | 5.40 µs | 31.8 µs | 0 errors |
| join-no-literal | pg | 200 ops | 59.9 µs (3.7 %) | 52.7 µs | 53.3 µs | 53.5 µs | 62.1 µs | 200.6 µs | 16,382 ops/s | 31.4 µs | 2.75 µs | 19.8 µs | 0 errors |
| join-no-literal | oracle | 200 ops | 78.0 µs (14.2 %) | 67.2 µs | 70.1 µs | 74.0 µs | 87.2 µs | 98.2 µs | 12,750 ops/s | 27.8 µs | 14.98 µs | 45.7 µs | 0 errors |
| exists-correlated | kds-group | 200 ops | 37.7 µs (8.0 %) | 34.2 µs | 35.8 µs | 37.1 µs | 39.4 µs | 53.0 µs | 26,108 ops/s | 4.5 µs | 0.45 µs | 28.7 µs | 0 errors |
| exists-correlated | pg | 200 ops | 39.5 µs (17.9 %) | 34.1 µs | 35.5 µs | 35.9 µs | 72.0 µs | 91.4 µs | 24,937 ops/s | 11.1 µs | 0.38 µs | 22.3 µs | 0 errors |
| exists-correlated | oracle | 200 ops | 23.2 µs (11.7 %) | 19.8 µs | 20.6 µs | 20.8 µs | 35.7 µs | 48.4 µs | 41,228 ops/s | 8.2 µs | 2.17 µs | 10.5 µs | 0 errors |
| count-by-user | kds-group | 200 ops | 18.3 µs (23.2 %) | 13.0 µs | 14.2 µs | 14.5 µs | 31.1 µs | 58.4 µs | 52,736 ops/s | 3.6 µs | 0.07 µs | 7.6 µs | 0 errors |
| count-by-user | pg | 200 ops | 21.1 µs (8.0 %) | 16.3 µs | 17.3 µs | 17.5 µs | 44.2 µs | 58.0 µs | 45,108 ops/s | 6.4 µs | 0.05 µs | 9.2 µs | 0 errors |
| count-by-user | oracle | 200 ops | 19.9 µs (15.4 %) | 14.1 µs | 15.2 µs | 15.4 µs | 17.5 µs | 163.8 µs | 46,676 ops/s | 6.2 µs | 0.73 µs | 6.8 µs | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ddl | kds-group | - | - | - | 84.50 µs | 0.00 µs | 137.25 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 137.2 µs); cpu.on_cpu (0.00, 84.5 µs); sched.run_queue (1.00, 0.2 µs) |
| ddl | pg | - | 0.00 µs | 0.00 µs | 413.50 µs | 36.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | CPU (0.25, 84.2 µs) |
| ddl | oracle | 498.00 µs | 497.25 µs | 991.75 µs | 2,794.00 µs | 0.00 µs | 344.00 µs | 0.00 µs | 0.75 µs | 0.00 µs | log file sync (1.00, 344.0 µs); events in waitclass Other (3.25, 20.5 µs); SQL*Net message to client (1.00, 0.8 µs) |
| load | kds-group | - | - | - | 4.26 µs | 0.00 µs | 119.08 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 119.1 µs); cpu.on_cpu (0.00, 4.2 µs); sched.run_queue (1.00, 0.1 µs) |
| load | pg | - | 0.00 µs | 0.00 µs | 1.78 µs | 0.00 µs | 110.74 µs | 0.00 µs | 13.95 µs | 0.00 µs | WalSync (0.11, 110.7 µs); ClientRead (0.01, 13.9 µs); CPU (0.00, 2.8 µs) |
| load | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 18.77 µs | 0.00 µs | 195.74 µs | 0.00 µs | 1.03 µs | 0.00 µs | log file sync (1.00, 195.7 µs); SQL*Net message to client (1.00, 1.0 µs) |
| create-index | kds-group | - | - | - | 110.00 µs | 0.00 µs | 162.88 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 162.9 µs); cpu.on_cpu (0.00, 109.9 µs); sched.run_queue (1.00, 0.1 µs) |
| create-index | pg | - | 0.00 µs | 0.00 µs | 130.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | CPU (0.12, 60.6 µs) |
| create-index | oracle | 135.12 µs | 431.75 µs | 566.88 µs | 737.12 µs | 0.00 µs | 222.50 µs | 0.00 µs | 0.62 µs | 0.00 µs | log file sync (1.00, 222.5 µs); SQL*Net message to client (1.00, 0.6 µs) |
| pk-user | kds-group | - | - | 1.01 µs | 1.44 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.33 µs | cpu.on_cpu (0.00, 2.4 µs); sched.run_queue (1.00, 0.1 µs) |
| pk-user | pg | - | 0.00 µs | 0.00 µs | 1.52 µs | 0.00 µs | 0.00 µs | 0.00 µs | 6.47 µs | 0.19 µs | ClientRead (0.01, 6.5 µs) |
| pk-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.58 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.29 µs | 1.93 µs | SQL*Net message to client (1.00, 0.3 µs) |
| loans-by-user | kds-group | - | - | 1.22 µs | 3.27 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.55 µs | cpu.on_cpu (0.00, 4.4 µs); sched.run_queue (1.00, 0.1 µs) |
| loans-by-user | pg | - | 0.00 µs | 0.00 µs | 4.66 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.52 µs | 0.19 µs | ClientRead (0.01, 5.5 µs) |
| loans-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.75 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.18 µs | 2.01 µs | SQL*Net message to client (1.00, 0.2 µs) |
| loans-by-book | kds-group | - | - | 1.16 µs | 3.21 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.32 µs | cpu.on_cpu (0.00, 4.2 µs); sched.run_queue (1.00, 0.1 µs) |
| loans-by-book | pg | - | 0.00 µs | 0.00 µs | 4.71 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.10 µs | none |
| loans-by-book | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.98 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.20 µs | 1.62 µs | SQL*Net message to client (1.00, 0.2 µs) |
| resv-by-user | kds-group | - | - | 1.16 µs | 2.35 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | cpu.on_cpu (0.00, 3.4 µs); sched.run_queue (1.00, 0.1 µs) |
| resv-by-user | pg | - | 0.00 µs | 0.00 µs | 2.65 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.07 µs | none |
| resv-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.96 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 1.47 µs | SQL*Net message to client (1.00, 0.2 µs) |
| books-by-author | kds-group | - | - | 1.17 µs | 2.44 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.28 µs | cpu.on_cpu (0.00, 3.5 µs); sched.run_queue (1.00, 0.1 µs) |
| books-by-author | pg | - | 0.00 µs | 0.00 µs | 1.87 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.29 µs | 0.14 µs | ClientRead (0.01, 5.3 µs) |
| books-by-author | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.92 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.22 µs | 1.57 µs | SQL*Net message to client (1.00, 0.2 µs) |
| books-by-genre | kds-group | - | - | 1.18 µs | 2.28 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | cpu.on_cpu (0.00, 3.4 µs); sched.run_queue (1.00, 0.1 µs) |
| books-by-genre | pg | - | 0.00 µs | 0.00 µs | 2.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.10 µs | none |
| books-by-genre | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.77 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | 1.46 µs | SQL*Net message to client (1.00, 0.2 µs) |
| loans-by-daterange | kds-group | - | - | 1.22 µs | 5.41 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.46 µs | cpu.on_cpu (0.00, 6.6 µs); sched.run_queue (1.00, 0.1 µs) |
| loans-by-daterange | pg | - | 0.00 µs | 0.00 µs | 5.86 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.22 µs | none |
| loans-by-daterange | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 7.46 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | 2.50 µs | SQL*Net message to client (1.00, 0.2 µs) |
| overdue | kds-group | - | - | 1.43 µs | 3.91 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.34 µs | cpu.on_cpu (0.00, 5.3 µs); sched.run_queue (1.00, 0.1 µs) |
| overdue | pg | - | 0.00 µs | 0.00 µs | 6.38 µs | 0.00 µs | 0.00 µs | 0.00 µs | 9.63 µs | 0.14 µs | ClientRead (0.01, 9.6 µs) |
| overdue | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 9.49 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.20 µs | 2.19 µs | SQL*Net message to client (1.00, 0.2 µs) |
| join-loan-user | kds-group | - | - | 3.83 µs | 3.83 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.37 µs | cpu.on_cpu (0.00, 7.6 µs); sched.run_queue (1.00, 0.1 µs) |
| join-loan-user | pg | - | 0.00 µs | 0.00 µs | 5.76 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.32 µs | 0.21 µs | ClientRead (0.01, 10.3 µs) |
| join-loan-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 3.12 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | 2.15 µs | SQL*Net message to client (1.00, 0.2 µs) |
| join-no-literal | kds-group | - | - | 3.77 µs | 23.86 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.40 µs | cpu.on_cpu (0.00, 27.6 µs); sched.run_queue (1.00, 0.1 µs) |
| join-no-literal | pg | - | 0.00 µs | 0.00 µs | 14.82 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.26 µs | CPU (0.01, 11.5 µs) |
| join-no-literal | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 37.56 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.10 µs | 14.98 µs | events in waitclass Other (6.00, 6.7 µs); SQL*Net message to client (1.00, 0.1 µs) |
| exists-correlated | kds-group | - | - | 1.93 µs | 22.96 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.45 µs | cpu.on_cpu (0.00, 24.7 µs); sched.run_queue (1.00, 0.1 µs) |
| exists-correlated | pg | - | 0.00 µs | 0.00 µs | 16.12 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.41 µs | CPU (0.01, 15.8 µs) |
| exists-correlated | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 6.65 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.19 µs | 2.17 µs | SQL*Net message to client (1.00, 0.2 µs) |
| count-by-user | kds-group | - | - | 1.05 µs | 2.95 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.07 µs | cpu.on_cpu (0.00, 4.0 µs); sched.run_queue (1.00, 0.1 µs) |
| count-by-user | pg | - | 0.00 µs | 0.00 µs | 4.47 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.93 µs | 0.03 µs | ClientRead (0.01, 4.9 µs) |
| count-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.73 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 0.73 µs | SQL*Net message to client (1.00, 0.2 µs) |

### s3 `loans200-unindexed`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ddl | kds-group | 4 ops | 507.5 µs (24.5 %) | 325.6 µs | 371.8 µs | 529.7 µs | 686.0 µs | 686.0 µs | 1,607 ops/s | 60.3 µs | 0.00 µs | 146.7 µs | 0 errors |
| ddl | pg | 4 ops | 657.6 µs (17.2 %) | 470.9 µs | 477.9 µs | 480.6 µs | 1,201.1 µs | 1,201.1 µs | 1,384 ops/s | 18.5 µs | 0.00 µs | 469.8 µs | 0 errors |
| ddl | oracle | 4 ops | 4,552.4 µs (5.5 %) | 3,119.2 µs | 3,247.8 µs | 5,860.3 µs | 5,921.4 µs | 5,921.4 µs | 216 ops/s | 104.4 µs | 0.00 µs | 3,881.0 µs | 0 errors |
| load | kds-group | 380 ops | 153.6 µs (13.1 %) | 131.3 µs | 137.9 µs | 141.1 µs | 191.0 µs | 330.3 µs | 6,457 ops/s | 12.0 µs | 0.00 µs | 12.9 µs | 0 errors |
| load | pg | 380 ops | 147.1 µs (146.3 %) | 128.8 µs | 136.7 µs | 139.2 µs | 164.1 µs | 280.0 µs | 6,774 ops/s | 13.2 µs | 0.00 µs | 10.3 µs | 0 errors |
| load | oracle | 380 ops | 291.8 µs (140.2 %) | 158.1 µs | 171.5 µs | 202.1 µs | 567.3 µs | 703.6 µs | 3,413 ops/s | 26.9 µs | 0.00 µs | 56.6 µs | 0 errors |
| pk-user | kds-group | 200 ops | 22.1 µs (21.1 %) | 20.1 µs | 20.9 µs | 21.2 µs | 31.0 µs | 31.5 µs | 43,122 ops/s | 9.6 µs | 0.29 µs | 7.2 µs | 0 errors |
| pk-user | pg | 200 ops | 24.0 µs (31.5 %) | 21.1 µs | 21.5 µs | 22.1 µs | 26.8 µs | 42.0 µs | 39,462 ops/s | 13.0 µs | 0.21 µs | 6.3 µs | 0 errors |
| pk-user | oracle | 200 ops | 23.8 µs (31.3 %) | 15.7 µs | 17.9 µs | 18.6 µs | 33.2 µs | 46.5 µs | 40,064 ops/s | 11.1 µs | 1.48 µs | 6.6 µs | 0 errors |
| loans-by-user | kds-group | 200 ops | 22.9 µs (19.0 %) | 18.0 µs | 18.8 µs | 19.0 µs | 33.2 µs | 69.9 µs | 40,859 ops/s | 6.9 µs | 0.44 µs | 9.6 µs | 0 errors |
| loans-by-user | pg | 200 ops | 29.2 µs (17.6 %) | 18.5 µs | 20.7 µs | 21.9 µs | 55.4 µs | 115.8 µs | 32,367 ops/s | 11.1 µs | 0.22 µs | 9.3 µs | 0 errors |
| loans-by-user | oracle | 200 ops | 27.1 µs (18.1 %) | 17.2 µs | 18.7 µs | 19.2 µs | 33.7 µs | 231.6 µs | 34,953 ops/s | 7.7 µs | 1.64 µs | 9.4 µs | 0 errors |
| loans-by-book | kds-group | 200 ops | 23.0 µs (26.8 %) | 18.0 µs | 18.6 µs | 18.8 µs | 34.3 µs | 145.8 µs | 41,769 ops/s | 7.2 µs | 0.38 µs | 9.6 µs | 0 errors |
| loans-by-book | pg | 200 ops | 21.2 µs (27.1 %) | 17.3 µs | 18.1 µs | 19.0 µs | 23.6 µs | 52.2 µs | 46,255 ops/s | 8.5 µs | 0.16 µs | 9.1 µs | 0 errors |
| loans-by-book | oracle | 200 ops | 25.3 µs (20.8 %) | 17.4 µs | 18.6 µs | 19.2 µs | 33.1 µs | 150.3 µs | 37,857 ops/s | 8.0 µs | 1.63 µs | 9.9 µs | 0 errors |
| resv-by-user | kds-group | 200 ops | 17.4 µs (18.4 %) | 15.7 µs | 16.4 µs | 17.3 µs | 24.0 µs | 33.1 µs | 54,829 ops/s | 5.6 µs | 0.21 µs | 7.7 µs | 0 errors |
| resv-by-user | pg | 200 ops | 18.4 µs (11.3 %) | 14.2 µs | 15.4 µs | 15.8 µs | 17.5 µs | 67.1 µs | 52,545 ops/s | 5.4 µs | 0.07 µs | 6.6 µs | 0 errors |
| resv-by-user | oracle | 200 ops | 23.9 µs (21.3 %) | 17.7 µs | 18.7 µs | 19.5 µs | 22.4 µs | 40.4 µs | 40,193 ops/s | 8.0 µs | 1.30 µs | 8.8 µs | 0 errors |
| books-by-author | kds-group | 200 ops | 17.6 µs (39.7 %) | 13.0 µs | 14.0 µs | 14.5 µs | 18.5 µs | 22.8 µs | 53,501 ops/s | 4.5 µs | 0.24 µs | 7.2 µs | 0 errors |
| books-by-author | pg | 200 ops | 18.9 µs (23.9 %) | 13.4 µs | 15.0 µs | 16.1 µs | 37.3 µs | 51.9 µs | 50,958 ops/s | 7.4 µs | 0.17 µs | 5.9 µs | 0 errors |
| books-by-author | oracle | 200 ops | 28.2 µs (24.9 %) | 16.0 µs | 17.7 µs | 18.7 µs | 23.8 µs | 241.3 µs | 33,670 ops/s | 9.3 µs | 1.71 µs | 9.4 µs | 0 errors |
| books-by-genre | kds-group | 200 ops | 16.7 µs (14.1 %) | 13.1 µs | 13.7 µs | 14.4 µs | 16.3 µs | 108.6 µs | 57,898 ops/s | 3.9 µs | 0.18 µs | 6.8 µs | 0 errors |
| books-by-genre | pg | 200 ops | 16.4 µs (18.1 %) | 13.8 µs | 14.6 µs | 15.4 µs | 17.4 µs | 25.9 µs | 60,115 ops/s | 6.2 µs | 0.14 µs | 5.8 µs | 0 errors |
| books-by-genre | oracle | 200 ops | 26.6 µs (29.1 %) | 17.7 µs | 18.4 µs | 19.3 µs | 23.1 µs | 227.8 µs | 35,532 ops/s | 9.2 µs | 1.61 µs | 9.3 µs | 0 errors |
| loans-by-daterange | kds-group | 200 ops | 24.9 µs (30.7 %) | 15.8 µs | 17.6 µs | 18.0 µs | 36.0 µs | 174.6 µs | 38,249 ops/s | 5.2 µs | 0.38 µs | 10.4 µs | 0 errors |
| loans-by-daterange | pg | 200 ops | 25.9 µs (15.5 %) | 17.8 µs | 21.7 µs | 22.6 µs | 32.2 µs | 137.5 µs | 37,662 ops/s | 9.4 µs | 0.28 µs | 10.8 µs | 0 errors |
| loans-by-daterange | oracle | 200 ops | 28.3 µs (10.8 %) | 20.3 µs | 23.5 µs | 24.2 µs | 27.9 µs | 215.9 µs | 34,598 ops/s | 10.4 µs | 2.79 µs | 11.8 µs | 0 errors |
| overdue | kds-group | 200 ops | 21.4 µs (49.6 %) | 17.6 µs | 18.6 µs | 19.5 µs | 21.4 µs | 48.6 µs | 43,929 ops/s | 5.0 µs | 0.29 µs | 10.4 µs | 0 errors |
| overdue | pg | 200 ops | 30.3 µs (33.0 %) | 17.6 µs | 20.4 µs | 21.6 µs | 69.4 µs | 232.6 µs | 32,287 ops/s | 7.6 µs | 0.18 µs | 13.5 µs | 0 errors |
| overdue | oracle | 200 ops | 42.3 µs (27.9 %) | 21.4 µs | 24.1 µs | 25.2 µs | 39.4 µs | 279.0 µs | 22,619 ops/s | 15.3 µs | 3.19 µs | 13.7 µs | 0 errors |
| join-loan-user | kds-group | 200 ops | 22.9 µs (35.9 %) | 19.7 µs | 20.6 µs | 21.1 µs | 26.2 µs | 33.9 µs | 42,289 ops/s | 5.1 µs | 0.34 µs | 12.4 µs | 0 errors |
| join-loan-user | pg | 200 ops | 23.0 µs (25.8 %) | 18.0 µs | 19.9 µs | 20.3 µs | 24.4 µs | 50.4 µs | 42,889 ops/s | 6.2 µs | 0.18 µs | 11.5 µs | 0 errors |
| join-loan-user | oracle | 200 ops | 23.2 µs (22.1 %) | 19.2 µs | 20.8 µs | 21.7 µs | 23.3 µs | 27.5 µs | 40,298 ops/s | 9.2 µs | 2.05 µs | 10.2 µs | 0 errors |
| join-no-literal | kds-group | 200 ops | 58.7 µs (31.9 %) | 47.7 µs | 48.2 µs | 48.5 µs | 86.5 µs | 214.8 µs | 16,889 ops/s | 17.6 µs | 6.88 µs | 30.8 µs | 0 errors |
| join-no-literal | pg | 200 ops | 57.3 µs (18.4 %) | 52.2 µs | 52.9 µs | 53.2 µs | 67.3 µs | 174.6 µs | 17,286 ops/s | 31.6 µs | 2.82 µs | 19.4 µs | 0 errors |
| join-no-literal | oracle | 200 ops | 81.3 µs (9.7 %) | 67.4 µs | 69.6 µs | 71.1 µs | 87.6 µs | 211.0 µs | 12,097 ops/s | 23.0 µs | 11.94 µs | 47.6 µs | 0 errors |
| exists-correlated | kds-group | 200 ops | 33.2 µs (22.3 %) | 27.4 µs | 28.6 µs | 28.8 µs | 47.3 µs | 49.0 µs | 29,755 ops/s | 4.6 µs | 0.38 µs | 21.6 µs | 0 errors |
| exists-correlated | pg | 200 ops | 36.9 µs (18.0 %) | 34.6 µs | 35.8 µs | 36.0 µs | 44.7 µs | 47.4 µs | 26,560 ops/s | 10.6 µs | 0.38 µs | 22.3 µs | 0 errors |
| exists-correlated | oracle | 200 ops | 28.4 µs (4.3 %) | 24.3 µs | 25.2 µs | 25.4 µs | 27.4 µs | 61.5 µs | 33,514 ops/s | 7.7 µs | 1.90 µs | 15.0 µs | 0 errors |
| count-by-user | kds-group | 200 ops | 17.9 µs (24.3 %) | 14.0 µs | 14.9 µs | 15.4 µs | 16.3 µs | 145.7 µs | 52,395 ops/s | 3.2 µs | 0.06 µs | 8.5 µs | 0 errors |
| count-by-user | pg | 200 ops | 19.4 µs (26.7 %) | 16.4 µs | 17.3 µs | 17.5 µs | 29.0 µs | 82.1 µs | 50,871 ops/s | 5.6 µs | 0.04 µs | 9.2 µs | 0 errors |
| count-by-user | oracle | 200 ops | 23.8 µs (5.3 %) | 17.1 µs | 18.3 µs | 18.4 µs | 27.1 µs | 164.2 µs | 40,065 ops/s | 8.0 µs | 0.87 µs | 9.9 µs | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ddl | kds-group | - | - | - | 103.75 µs | 0.00 µs | 151.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 151.0 µs); cpu.on_cpu (0.00, 103.0 µs); sched.run_queue (1.00, 0.8 µs) |
| ddl | pg | - | 0.00 µs | 0.00 µs | 461.75 µs | 33.25 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | none |
| ddl | oracle | 492.00 µs | 532.75 µs | 1,010.50 µs | 3,443.75 µs | 331.50 µs | 350.50 µs | 0.00 µs | 0.00 µs | 0.00 µs | log file sync (1.00, 350.5 µs); db file sequential read (3.50, 315.5 µs); events in waitclass Other (7.00, 20.0 µs); Disk file operations I/O (0.50, 16.5 µs) |
| load | kds-group | - | - | - | 4.58 µs | 0.00 µs | 121.62 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 121.6 µs); cpu.on_cpu (0.00, 4.5 µs); sched.run_queue (1.00, 0.1 µs) |
| load | pg | - | 0.00 µs | 0.00 µs | 1.76 µs | 0.00 µs | 108.60 µs | 0.00 µs | 14.72 µs | 0.00 µs | WalSync (0.11, 108.6 µs); ClientRead (0.01, 14.7 µs); CPU (0.01, 5.5 µs) |
| load | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 14.64 µs | 0.00 µs | 170.84 µs | 0.00 µs | 0.64 µs | 0.00 µs | log file sync (1.00, 170.8 µs); SQL*Net message to client (1.00, 0.6 µs) |
| pk-user | kds-group | - | - | 1.11 µs | 1.59 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.29 µs | cpu.on_cpu (0.00, 2.6 µs); sched.run_queue (1.00, 0.1 µs) |
| pk-user | pg | - | 0.00 µs | 0.00 µs | 1.76 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.34 µs | none |
| pk-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.29 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.22 µs | 1.48 µs | SQL*Net message to client (1.00, 0.2 µs) |
| loans-by-user | kds-group | - | - | 1.11 µs | 4.46 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.44 µs | cpu.on_cpu (0.00, 5.5 µs); sched.run_queue (1.00, 0.0 µs) |
| loans-by-user | pg | - | 0.00 µs | 0.00 µs | 4.98 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.15 µs | CPU (0.01, 5.2 µs) |
| loans-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 5.21 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.22 µs | 1.64 µs | SQL*Net message to client (1.00, 0.2 µs) |
| loans-by-book | kds-group | - | - | 1.12 µs | 4.60 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.38 µs | cpu.on_cpu (0.00, 5.5 µs); sched.run_queue (1.00, 0.2 µs) |
| loans-by-book | pg | - | 0.00 µs | 0.00 µs | 4.92 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | CPU (0.01, 5.5 µs) |
| loans-by-book | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 5.29 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 1.63 µs | SQL*Net message to client (1.00, 0.2 µs) |
| resv-by-user | kds-group | - | - | 1.09 µs | 2.78 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | cpu.on_cpu (0.00, 3.8 µs); sched.run_queue (1.00, 0.1 µs) |
| resv-by-user | pg | - | 0.00 µs | 0.00 µs | 2.66 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.11 µs | none |
| resv-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.60 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 1.30 µs | SQL*Net message to client (1.00, 0.2 µs) |
| books-by-author | kds-group | - | - | 1.08 µs | 2.27 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | cpu.on_cpu (0.00, 3.3 µs); sched.run_queue (1.00, 0.1 µs) |
| books-by-author | pg | - | 0.00 µs | 0.00 µs | 1.88 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.16 µs | none |
| books-by-author | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.30 µs | 1.71 µs | SQL*Net message to client (1.00, 0.3 µs) |
| books-by-genre | kds-group | - | - | 1.07 µs | 2.12 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.18 µs | cpu.on_cpu (0.00, 3.1 µs); sched.run_queue (1.00, 0.1 µs) |
| books-by-genre | pg | - | 0.00 µs | 0.00 µs | 1.82 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.08 µs | 0.15 µs | ClientRead (0.01, 5.1 µs) |
| books-by-genre | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 4.78 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.23 µs | 1.61 µs | SQL*Net message to client (1.00, 0.2 µs) |
| loans-by-daterange | kds-group | - | - | 1.15 µs | 5.42 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.38 µs | cpu.on_cpu (0.00, 6.5 µs); sched.run_queue (1.00, 0.1 µs) |
| loans-by-daterange | pg | - | 0.00 µs | 0.00 µs | 6.15 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.75 µs | 0.31 µs | ClientRead (0.01, 1.7 µs) |
| loans-by-daterange | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 7.29 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.22 µs | 2.79 µs | SQL*Net message to client (1.00, 0.2 µs) |
| overdue | kds-group | - | - | 1.28 µs | 5.29 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.29 µs | cpu.on_cpu (0.00, 6.5 µs); sched.run_queue (1.00, 0.0 µs) |
| overdue | pg | - | 0.00 µs | 0.00 µs | 6.47 µs | 0.00 µs | 0.00 µs | 0.00 µs | 4.07 µs | 0.16 µs | ClientRead (0.01, 4.1 µs) |
| overdue | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 8.33 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | 3.19 µs | SQL*Net message to client (1.00, 0.2 µs) |
| join-loan-user | kds-group | - | - | 3.39 µs | 5.03 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.34 µs | cpu.on_cpu (0.00, 8.4 µs); sched.run_queue (1.00, 0.1 µs) |
| join-loan-user | pg | - | 0.00 µs | 0.00 µs | 5.86 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.15 µs | 0.32 µs | ClientRead (0.01, 1.1 µs) |
| join-loan-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 5.88 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | 2.05 µs | SQL*Net message to client (1.00, 0.2 µs) |
| join-no-literal | kds-group | - | - | 3.94 µs | 22.16 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 6.88 µs | cpu.on_cpu (0.00, 26.0 µs); sched.run_queue (1.00, 0.1 µs) |
| join-no-literal | pg | - | 0.00 µs | 0.00 µs | 14.85 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.54 µs | 2.73 µs | CPU (0.01, 10.9 µs); ClientRead (0.01, 5.5 µs) |
| join-no-literal | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 38.10 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.14 µs | 11.94 µs | events in waitclass Other (6.00, 7.8 µs); SQL*Net message to client (1.00, 0.1 µs) |
| exists-correlated | kds-group | - | - | 1.79 µs | 16.25 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.38 µs | cpu.on_cpu (0.00, 18.0 µs); sched.run_queue (1.00, 0.1 µs) |
| exists-correlated | pg | - | 0.00 µs | 0.00 µs | 15.81 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.26 µs | 0.36 µs | ClientRead (0.01, 10.3 µs); CPU (0.01, 1.1 µs) |
| exists-correlated | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 11.01 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | 1.90 µs | SQL*Net message to client (1.00, 0.2 µs) |
| count-by-user | kds-group | - | - | 0.92 µs | 4.11 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.06 µs | cpu.on_cpu (0.00, 5.0 µs); sched.run_queue (1.00, 0.1 µs) |
| count-by-user | pg | - | 0.00 µs | 0.00 µs | 4.33 µs | 0.00 µs | 0.00 µs | 0.00 µs | 5.20 µs | 0.04 µs | ClientRead (0.01, 5.2 µs) |
| count-by-user | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 5.48 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | 0.87 µs | SQL*Net message to client (1.00, 0.2 µs) |

### s4 `day1`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| open | kds-group | 240 ops | 142.0 µs (5.0 %) | 121.2 µs | 127.4 µs | 130.1 µs | 169.0 µs | 310.5 µs | 6,988 ops/s | 4.9 µs | 0.00 µs | 9.4 µs | 0 errors |
| open | pg | 240 ops | 144.0 µs (2.1 %) | 126.3 µs | 129.9 µs | 133.3 µs | 208.5 µs | 291.9 µs | 6,886 ops/s | 9.5 µs | 0.00 µs | 9.3 µs | 0 errors |
| open | oracle | 240 ops | 313.9 µs (44.4 %) | 155.0 µs | 175.2 µs | 293.7 µs | 556.7 µs | 749.4 µs | 3,147 ops/s | 25.5 µs | 0.00 µs | 65.5 µs | 0 errors |
| board | kds-group | 2,400 ops | 133.8 µs (2.1 %) | 121.2 µs | 130.0 µs | 133.0 µs | 142.6 µs | 199.6 µs | 7,374 ops/s | 12.0 µs | 2.36 µs | 118.1 µs | 0 errors |
| board | pg | 2,400 ops | 250.6 µs (1.5 %) | 223.2 µs | 240.3 µs | 246.6 µs | 264.4 µs | 440.5 µs | 3,968 ops/s | 26.0 µs | 1.67 µs | 217.6 µs | 0 errors |
| board | oracle | 2,400 ops | 71.4 µs (2.3 %) | 61.0 µs | 66.3 µs | 68.8 µs | 78.7 µs | 198.3 µs | 13,857 ops/s | 14.6 µs | 7.27 µs | 51.7 µs | 0 errors |
| tape200 | kds-group | 396 ops | 21.9 µs (134.6 %) | 19.0 µs | 19.4 µs | 20.4 µs | 23.2 µs | 53.6 µs | 42,873 ops/s | 7.7 µs | 0.72 µs | 9.6 µs | 0 errors |
| tape200 | pg | 396 ops | 29.6 µs (10.5 %) | 20.2 µs | 22.7 µs | 28.4 µs | 31.6 µs | 64.9 µs | 32,544 ops/s | 14.5 µs | 0.60 µs | 10.1 µs | 0 errors |
| tape200 | oracle | 396 ops | 25.7 µs (9.3 %) | 20.2 µs | 21.0 µs | 21.7 µs | 24.8 µs | 196.4 µs | 38,063 ops/s | 9.5 µs | 3.33 µs | 10.1 µs | 0 errors |
| tape1k | kds-group | 396 ops | 27.1 µs (7.0 %) | 23.6 µs | 24.5 µs | 25.1 µs | 29.1 µs | 45.5 µs | 35,772 ops/s | 4.6 µs | 0.42 µs | 18.1 µs | 0 errors |
| tape1k | pg | 396 ops | 43.0 µs (16.6 %) | 36.0 µs | 37.0 µs | 37.7 µs | 58.3 µs | 160.6 µs | 22,784 ops/s | 9.2 µs | 0.38 µs | 26.5 µs | 0 errors |
| tape1k | oracle | 396 ops | 26.0 µs (0.1 %) | 23.1 µs | 24.5 µs | 24.7 µs | 26.2 µs | 27.5 µs | 37,722 ops/s | 9.3 µs | 3.31 µs | 12.5 µs | 0 errors |
| tape10k | kds-group | 396 ops | 132.1 µs (3.6 %) | 118.5 µs | 125.7 µs | 127.1 µs | 137.6 µs | 248.6 µs | 7,495 ops/s | 10.5 µs | 0.95 µs | 115.0 µs | 0 errors |
| tape10k | pg | 396 ops | 239.7 µs (4.4 %) | 218.8 µs | 236.2 µs | 239.2 µs | 245.6 µs | 310.1 µs | 4,147 ops/s | 21.9 µs | 0.90 µs | 210.9 µs | 0 errors |
| tape10k | oracle | 396 ops | 66.4 µs (7.0 %) | 59.5 µs | 60.9 µs | 61.4 µs | 71.2 µs | 184.0 µs | 14,900 ops/s | 10.5 µs | 3.60 µs | 50.0 µs | 0 errors |
| pk | kds-group | 240 ops | 20.2 µs (44.6 %) | 18.6 µs | 18.9 µs | 19.1 µs | 30.3 µs | 31.0 µs | 47,312 ops/s | 8.7 µs | 0.24 µs | 6.7 µs | 0 errors |
| pk | pg | 240 ops | 25.2 µs (77.8 %) | 21.1 µs | 21.9 µs | 24.0 µs | 41.0 µs | 60.2 µs | 38,694 ops/s | 14.2 µs | 0.17 µs | 5.6 µs | 0 errors |
| pk | oracle | 240 ops | 17.0 µs (11.3 %) | 14.0 µs | 15.2 µs | 15.3 µs | 16.9 µs | 29.6 µs | 57,444 ops/s | 6.3 µs | 0.97 µs | 6.3 µs | 0 errors |
| close | kds-group | 12 ops | 454.8 µs (11.9 %) | 131.4 µs | 259.3 µs | 347.8 µs | 882.8 µs | 946.5 µs | 2,072 ops/s | 89.0 µs | 32.95 µs | 266.2 µs | 0 errors |
| close | pg | 12 ops | 541.2 µs (32.3 %) | 261.9 µs | 290.0 µs | 425.5 µs | 1,080.8 µs | 1,151.6 µs | 1,789 ops/s | 120.7 µs | 13.95 µs | 330.2 µs | 0 errors |
| close | oracle | 12 ops | 274.8 µs (18.4 %) | 75.7 µs | 111.5 µs | 221.1 µs | 480.5 µs | 569.1 µs | 3,485 ops/s | 89.4 µs | 38.48 µs | 150.9 µs | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| open | kds-group | - | - | - | 2.79 µs | 0.00 µs | 119.43 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 119.4 µs); cpu.on_cpu (0.00, 2.7 µs); sched.run_queue (1.00, 0.1 µs) |
| open | pg | - | 0.00 µs | 0.00 µs | 2.71 µs | 0.00 µs | 312.52 µs | 0.00 µs | 4.17 µs | 0.00 µs | WalSync (0.30, 312.5 µs); CPU (0.01, 6.0 µs); ClientRead (0.00, 4.2 µs) |
| open | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 15.83 µs | 0.00 µs | 181.36 µs | 0.00 µs | 0.88 µs | 0.00 µs | log file sync (1.00, 181.4 µs); SQL*Net message to client (1.00, 0.9 µs) |
| board | kds-group | - | - | 1.15 µs | 112.76 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.36 µs | cpu.on_cpu (0.00, 113.6 µs); sched.run_queue (2.00, 0.3 µs) |
| board | pg | - | 0.00 µs | 0.00 µs | 208.86 µs | 0.00 µs | 0.00 µs | 0.00 µs | 7.38 µs | 1.82 µs | CPU (0.19, 203.3 µs); ClientRead (0.01, 7.4 µs) |
| board | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 47.30 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | 7.27 µs | SQL*Net message to client (1.00, 0.2 µs) |
| tape200 | kds-group | - | - | 1.07 µs | 4.65 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.72 µs | cpu.on_cpu (0.00, 5.7 µs); sched.run_queue (1.00, 0.1 µs) |
| tape200 | pg | - | 0.00 µs | 0.00 µs | 5.85 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.68 µs | CPU (0.01, 4.8 µs) |
| tape200 | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 5.83 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.21 µs | 3.33 µs | SQL*Net message to client (1.00, 0.2 µs) |
| tape1k | kds-group | - | - | 1.09 µs | 13.33 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.42 µs | cpu.on_cpu (0.00, 14.4 µs); sched.run_queue (1.00, 0.0 µs) |
| tape1k | pg | - | 0.00 µs | 0.00 µs | 21.99 µs | 0.00 µs | 0.00 µs | 0.00 µs | 3.62 µs | 0.34 µs | CPU (0.01, 10.7 µs); ClientRead (0.01, 3.6 µs) |
| tape1k | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 8.63 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.20 µs | 3.31 µs | SQL*Net message to client (1.00, 0.2 µs) |
| tape10k | kds-group | - | - | 1.12 µs | 109.87 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.95 µs | cpu.on_cpu (0.00, 109.3 µs); sched.run_queue (2.00, 3.2 µs) |
| tape10k | pg | - | 0.00 µs | 0.00 µs | 202.85 µs | 0.00 µs | 0.00 µs | 0.00 µs | 13.68 µs | 0.76 µs | CPU (0.16, 172.5 µs); ClientRead (0.01, 13.7 µs) |
| tape10k | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 45.31 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.26 µs | 3.60 µs | SQL*Net message to client (1.00, 0.3 µs) |
| pk | kds-group | - | - | 1.03 µs | 1.60 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.24 µs | cpu.on_cpu (0.00, 2.6 µs); sched.run_queue (1.00, 0.1 µs) |
| pk | pg | - | 0.00 µs | 0.00 µs | 1.34 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.19 µs | none |
| pk | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 2.37 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.16 µs | 0.97 µs | SQL*Net message to client (1.00, 0.2 µs) |
| close | kds-group | - | - | 1.33 µs | 256.17 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 32.95 µs | cpu.on_cpu (0.00, 257.1 µs); sched.run_queue (2.00, 0.4 µs) |
| close | pg | - | 0.00 µs | 0.00 µs | 295.67 µs | 0.00 µs | 0.00 µs | 0.00 µs | 87.08 µs | 13.26 µs | CPU (0.17, 119.6 µs); ClientRead (0.08, 87.1 µs) |
| close | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 137.42 µs | 0.00 µs | 0.00 µs | 0.00 µs | 2.42 µs | 38.48 µs | events in waitclass Other (1.25, 2.7 µs); SQL*Net message to client (1.00, 2.4 µs) |

### s4 `day1-declared`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| open | kds-group | 240 ops | 137.3 µs (5.2 %) | 121.3 µs | 127.2 µs | 129.1 µs | 166.2 µs | 260.2 µs | 7,211 ops/s | 4.6 µs | 0.00 µs | 9.1 µs | 0 errors |
| board | kds-group | 2,400 ops | 64.1 µs (4.3 %) | 14.7 µs | 19.8 µs | 21.1 µs | 274.7 µs | 285.5 µs | 15,341 ops/s | 6.5 µs | 1.26 µs | 54.0 µs | 0 errors |
| tape200 | kds-group | 396 ops | 18.4 µs (15.9 %) | 14.3 µs | 15.2 µs | 15.3 µs | 20.4 µs | 117.6 µs | 51,556 ops/s | 3.9 µs | 0.36 µs | 8.5 µs | 0 errors |
| tape1k | kds-group | 396 ops | 20.2 µs (3.3 %) | 14.3 µs | 15.2 µs | 15.4 µs | 40.1 µs | 58.5 µs | 47,491 ops/s | 3.8 µs | 0.36 µs | 11.6 µs | 0 errors |
| tape10k | kds-group | 396 ops | 64.0 µs (1.2 %) | 14.8 µs | 17.2 µs | 18.1 µs | 269.0 µs | 335.1 µs | 15,322 ops/s | 5.9 µs | 0.54 µs | 52.5 µs | 0 errors |
| pk | kds-group | 240 ops | 14.9 µs (10.1 %) | 13.1 µs | 13.6 µs | 14.7 µs | 15.4 µs | 27.2 µs | 65,769 ops/s | 4.3 µs | 0.12 µs | 6.1 µs | 0 errors |
| close | kds-group | 12 ops | 430.9 µs (24.5 %) | 140.3 µs | 246.0 µs | 317.1 µs | 762.7 µs | 1,040.6 µs | 2,206 ops/s | 51.5 µs | 20.36 µs | 269.6 µs | 0 errors |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| open | kds-group | - | - | - | 2.82 µs | 0.00 µs | 120.37 µs | 0.00 µs | 0.00 µs | 0.00 µs | commit.wal_flush (1.00, 120.4 µs); cpu.on_cpu (0.00, 2.8 µs); sched.run_queue (1.00, 0.1 µs) |
| board | kds-group | - | - | 1.11 µs | 49.08 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 1.26 µs | cpu.on_cpu (0.00, 50.1 µs); sched.run_queue (1.17, 0.1 µs) |
| tape200 | kds-group | - | - | 1.07 µs | 3.78 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.36 µs | cpu.on_cpu (0.00, 4.8 µs); sched.run_queue (1.00, 0.1 µs) |
| tape1k | kds-group | - | - | 1.07 µs | 6.82 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.36 µs | cpu.on_cpu (0.00, 7.9 µs); sched.run_queue (1.00, 0.1 µs) |
| tape10k | kds-group | - | - | 1.09 µs | 47.56 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.54 µs | cpu.on_cpu (0.00, 48.5 µs); sched.run_queue (1.18, 0.1 µs) |
| pk | kds-group | - | - | 0.99 µs | 1.55 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.12 µs | cpu.on_cpu (0.00, 2.5 µs); sched.run_queue (1.00, 0.1 µs) |
| close | kds-group | - | - | 2.33 µs | 259.17 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.00 µs | 20.36 µs | cpu.on_cpu (0.00, 260.9 µs); sched.run_queue (2.00, 0.6 µs) |

### s5 `four-terminals`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, kds-strict 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| new-order | kds-group | 300 ops | 1,131.2 µs (25.1 %) | 419.5 µs | 914.8 µs | 1,099.6 µs | 1,582.6 µs | 2,827.5 µs | 883 ops/s | 144.3 µs | 1.96 µs | 467.5 µs | 0 errors |
| new-order | kds-strict | 300 ops | 1,107.1 µs (16.4 %) | 418.6 µs | 890.1 µs | 1,078.1 µs | 1,454.8 µs | 2,747.6 µs | 903 ops/s | 137.7 µs | 2.02 µs | 458.7 µs | 0 errors |
| new-order | pg | 300 ops | 834.6 µs (2.4 %) | 420.4 µs | 675.3 µs | 845.6 µs | 1,115.1 µs | 1,339.5 µs | 1,198 ops/s | 230.2 µs | 2.14 µs | 300.7 µs | 0 errors |
| new-order | oracle | 300 ops | 977.7 µs (5.0 %) | 552.2 µs | 748.6 µs | 959.9 µs | 1,340.2 µs | 2,082.0 µs | 1,022 ops/s | 248.1 µs | 21.07 µs | 379.6 µs | 0 errors |
| payment | kds-group | 300 ops | 509.2 µs (4.6 %) | 447.7 µs | 458.6 µs | 465.4 µs | 546.4 µs | 1,283.0 µs | 1,960 ops/s | 34.5 µs | 0.29 µs | 71.3 µs | 0 errors |
| payment | kds-strict | 300 ops | 479.9 µs (17.3 %) | 440.6 µs | 457.8 µs | 469.1 µs | 545.1 µs | 788.0 µs | 2,079 ops/s | 39.9 µs | 0.33 µs | 72.9 µs | 0 errors |
| payment | pg | 300 ops | 246.5 µs (24.4 %) | 230.8 µs | 236.6 µs | 238.9 µs | 264.6 µs | 444.0 µs | 4,053 ops/s | 41.1 µs | 0.21 µs | 53.5 µs | 0 errors |
| payment | oracle | 300 ops | 328.2 µs (50.4 %) | 238.3 µs | 247.4 µs | 260.9 µs | 527.7 µs | 2,151.8 µs | 3,035 ops/s | 40.6 µs | 2.72 µs | 83.3 µs | 0 errors |
| order-status | kds-group | 300 ops | 461.1 µs (11.9 %) | 398.7 µs | 411.3 µs | 418.8 µs | 560.3 µs | 1,339.3 µs | 2,165 ops/s | 35.3 µs | 1.17 µs | 44.3 µs | 0 errors |
| order-status | kds-strict | 300 ops | 424.7 µs (1.3 %) | 387.3 µs | 406.3 µs | 411.1 µs | 484.3 µs | 612.0 µs | 2,353 ops/s | 30.5 µs | 1.12 µs | 42.7 µs | 0 errors |
| order-status | pg | 300 ops | 83.7 µs (10.7 %) | 71.5 µs | 77.2 µs | 79.5 µs | 89.4 µs | 112.7 µs | 11,804 ops/s | 26.3 µs | 0.43 µs | 34.8 µs | 0 errors |
| order-status | oracle | 300 ops | 70.0 µs (22.5 %) | 59.1 µs | 63.7 µs | 65.9 µs | 72.9 µs | 84.2 µs | 14,134 ops/s | 24.2 µs | 5.31 µs | 29.2 µs | 0 errors |
| delivery | kds-group | 300 ops | 1,518.1 µs (9.1 %) | 1,339.4 µs | 1,395.2 µs | 1,416.5 µs | 2,175.8 µs | 3,349.5 µs | 658 ops/s | 201.6 µs | 1.45 µs | 590.9 µs | 0 errors |
| delivery | kds-strict | 300 ops | 1,457.1 µs (2.1 %) | 1,387.1 µs | 1,409.0 µs | 1,418.9 µs | 1,528.9 µs | 1,921.2 µs | 686 ops/s | 199.6 µs | 1.57 µs | 592.7 µs | 0 errors |
| delivery | pg | 300 ops | 1,566.8 µs (8.1 %) | 1,405.8 µs | 1,469.7 µs | 1,500.1 µs | 1,818.6 µs | 2,859.8 µs | 638 ops/s | 335.3 µs | 1.59 µs | 773.9 µs | 0 errors |
| delivery | oracle | 300 ops | 1,907.8 µs (9.7 %) | 1,359.7 µs | 1,447.6 µs | 1,469.3 µs | 2,287.2 µs | 3,548.6 µs | 524 ops/s | 351.3 µs | 23.59 µs | 765.4 µs | 0 errors |
| stock-level | kds-group | 300 ops | 512.1 µs (19.7 %) | 430.3 µs | 465.5 µs | 472.4 µs | 560.2 µs | 1,620.0 µs | 1,948 ops/s | 32.3 µs | 0.37 µs | 97.3 µs | 0 errors |
| stock-level | kds-strict | 300 ops | 479.3 µs (3.4 %) | 430.4 µs | 461.7 µs | 467.9 µs | 540.7 µs | 873.6 µs | 2,082 ops/s | 30.1 µs | 0.32 µs | 96.9 µs | 0 errors |
| stock-level | pg | 300 ops | 191.4 µs (3.5 %) | 171.6 µs | 179.5 µs | 183.2 µs | 194.7 µs | 319.4 µs | 5,195 ops/s | 18.8 µs | 0.08 µs | 151.5 µs | 0 errors |
| stock-level | oracle | 300 ops | 124.3 µs (3.9 %) | 107.6 µs | 114.4 µs | 116.7 µs | 135.3 µs | 399.9 µs | 7,976 ops/s | 16.0 µs | 1.61 µs | 93.7 µs | 0 errors |
| mix | kds-group | 99,813 ops | 2,404.4 µs (2.7 %) | 299.6 µs | 822.9 µs | 1,676.6 µs | 5,082.1 µs | 14,303.0 µs | 1,663 ops/s | 145.5 µs | 2.04 µs | 229.4 µs | 0 errors |
| mix | kds-strict | 97,385 ops | 2,464.3 µs (4.9 %) | 251.3 µs | 712.6 µs | 1,539.9 µs | 5,904.9 µs | 14,894.2 µs | 1,623 ops/s | 141.3 µs | 2.05 µs | 228.5 µs | Other 1 |
| mix | pg | 252,686 ops | 949.7 µs (17.5 %) | 78.6 µs | 288.9 µs | 614.2 µs | 1,818.2 µs | 11,670.5 µs | 4,211 ops/s | 144.2 µs | 1.13 µs | 229.2 µs | 0 errors |
| mix | oracle | 175,479 ops | 1,369.8 µs (10.5 %) | 68.3 µs | 291.9 µs | 682.4 µs | 2,054.7 µs | 11,488.1 µs | 2,919 ops/s | 172.7 µs | 14.76 µs | 313.7 µs | 0 errors |

| phase | kind | arm | ops a round | mean | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| new-order | new-order | kds-group | 296 ops | 1,137.2 µs | 1,108.8 µs | 1,582.6 µs | 2,827.5 µs |
| new-order | new-order-rollback | kds-group | 4 ops | 685.3 µs | 900.9 µs | 928.0 µs | 928.0 µs |
| new-order | new-order | kds-strict | 296 ops | 1,112.1 µs | 1,085.4 µs | 1,441.0 µs | 2,206.0 µs |
| new-order | new-order-rollback | kds-strict | 4 ops | 737.1 µs | 881.7 µs | 1,085.6 µs | 1,085.6 µs |
| new-order | new-order | pg | 296 ops | 836.9 µs | 845.6 µs | 1,115.1 µs | 1,339.5 µs |
| new-order | new-order-rollback | pg | 4 ops | 667.5 µs | 872.8 µs | 893.1 µs | 893.1 µs |
| new-order | new-order | oracle | 296 ops | 977.8 µs | 959.9 µs | 1,333.2 µs | 2,082.0 µs |
| new-order | new-order-rollback | oracle | 4 ops | 966.2 µs | 1,229.5 µs | 1,340.2 µs | 1,340.2 µs |
| payment | payment | kds-group | 300 ops | 509.2 µs | 465.4 µs | 546.4 µs | 1,283.0 µs |
| payment | payment | kds-strict | 300 ops | 479.9 µs | 469.1 µs | 545.1 µs | 788.0 µs |
| payment | payment | pg | 300 ops | 246.5 µs | 238.9 µs | 264.6 µs | 444.0 µs |
| payment | payment | oracle | 300 ops | 328.2 µs | 260.9 µs | 527.7 µs | 2,151.8 µs |
| order-status | order-status | kds-group | 300 ops | 461.1 µs | 418.8 µs | 560.3 µs | 1,339.3 µs |
| order-status | order-status | kds-strict | 300 ops | 424.7 µs | 411.1 µs | 484.3 µs | 612.0 µs |
| order-status | order-status | pg | 300 ops | 83.7 µs | 79.5 µs | 89.4 µs | 112.7 µs |
| order-status | order-status | oracle | 300 ops | 70.0 µs | 65.9 µs | 72.9 µs | 84.2 µs |
| delivery | delivery | kds-group | 300 ops | 1,518.1 µs | 1,416.5 µs | 2,175.8 µs | 3,349.5 µs |
| delivery | delivery | kds-strict | 300 ops | 1,457.1 µs | 1,418.9 µs | 1,528.9 µs | 1,921.2 µs |
| delivery | delivery | pg | 300 ops | 1,566.8 µs | 1,500.1 µs | 1,818.6 µs | 2,859.8 µs |
| delivery | delivery | oracle | 300 ops | 1,907.8 µs | 1,469.3 µs | 2,287.2 µs | 3,548.6 µs |
| stock-level | stock-level | kds-group | 300 ops | 512.1 µs | 472.4 µs | 560.2 µs | 1,620.0 µs |
| stock-level | stock-level | kds-strict | 300 ops | 479.3 µs | 467.9 µs | 540.7 µs | 873.6 µs |
| stock-level | stock-level | pg | 300 ops | 191.4 µs | 183.2 µs | 194.7 µs | 319.4 µs |
| stock-level | stock-level | oracle | 300 ops | 124.3 µs | 116.7 µs | 135.3 µs | 399.9 µs |
| mix | delivery | kds-group | 3,995 ops | 5,285.5 µs | 4,457.6 µs | 8,194.5 µs | 17,707.7 µs |
| mix | new-order | kds-group | 44,523 ops | 3,629.1 µs | 2,959.0 µs | 5,745.7 µs | 15,527.2 µs |
| mix | new-order-rollback | kds-group | 474 ops | 2,905.1 µs | 2,480.9 µs | 4,811.7 µs | 14,384.7 µs |
| mix | order-status | kds-group | 3,927 ops | 807.3 µs | 580.2 µs | 1,039.6 µs | 4,557.2 µs |
| mix | payment | kds-group | 42,925 ops | 1,182.9 µs | 871.0 µs | 2,133.9 µs | 8,097.4 µs |
| mix | stock-level | kds-group | 3,969 ops | 716.3 µs | 551.9 µs | 973.1 µs | 1,842.7 µs |
| mix | delivery | kds-strict | 3,896 ops | 5,953.7 µs | 4,621.2 µs | 10,186.6 µs | 20,388.0 µs |
| mix | new-order | kds-strict | 43,441 ops | 3,739.4 µs | 2,874.6 µs | 6,648.1 µs | 16,291.1 µs |
| mix | new-order-rollback | kds-strict | 462 ops | 2,931.2 µs | 2,346.7 µs | 5,382.3 µs | 13,646.0 µs |
| mix | order-status | kds-strict | 3,838 ops | 632.1 µs | 469.5 µs | 971.6 µs | 2,388.4 µs |
| mix | payment | kds-strict | 41,874 ops | 1,159.9 µs | 773.4 µs | 2,325.6 µs | 10,346.9 µs |
| mix | stock-level | kds-strict | 3,874 ops | 669.0 µs | 489.5 µs | 939.7 µs | 2,385.2 µs |
| mix | delivery | pg | 10,159 ops | 2,355.5 µs | 1,660.1 µs | 5,325.9 µs | 13,593.6 µs |
| mix | new-order | pg | 112,296 ops | 1,300.2 µs | 930.1 µs | 2,107.7 µs | 11,939.9 µs |
| mix | new-order-rollback | pg | 1,215 ops | 758.1 µs | 700.8 µs | 1,288.8 µs | 1,958.6 µs |
| mix | order-status | pg | 10,079 ops | 131.7 µs | 107.4 µs | 196.0 µs | 755.2 µs |
| mix | payment | pg | 108,808 ops | 591.2 µs | 302.6 µs | 1,155.1 µs | 10,997.8 µs |
| mix | stock-level | pg | 10,129 ops | 352.8 µs | 321.4 µs | 525.3 µs | 1,123.4 µs |
| mix | delivery | oracle | 7,057 ops | 3,400.4 µs | 1,605.2 µs | 5,094.7 µs | 57,247.9 µs |
| mix | new-order | oracle | 78,202 ops | 2,050.4 µs | 1,077.5 µs | 2,726.5 µs | 12,547.2 µs |
| mix | new-order-rollback | oracle | 845 ops | 2,366.2 µs | 1,080.3 µs | 2,949.7 µs | 13,098.3 µs |
| mix | order-status | oracle | 6,976 ops | 117.2 µs | 88.0 µs | 200.6 µs | 830.0 µs |
| mix | payment | oracle | 75,412 ops | 696.4 µs | 306.1 µs | 1,116.5 µs | 4,407.6 µs |
| mix | stock-level | oracle | 6,987 ops | 230.9 µs | 179.5 µs | 389.5 µs | 904.7 µs |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| new-order | kds-group | - | - | 58.91 µs | 146.93 µs | 0.00 µs | 384.60 µs | 0.00 µs | 0.00 µs | 1.96 µs | commit.wal_flush (0.99, 384.6 µs); cpu.on_cpu (0.00, 203.1 µs); sched.run_queue (48.64, 2.7 µs) |
| new-order | kds-strict | - | - | 60.94 µs | 152.30 µs | 0.00 µs | 351.64 µs | 0.00 µs | 0.00 µs | 2.02 µs | commit.wal_flush (0.99, 351.6 µs); cpu.on_cpu (0.00, 210.5 µs); sched.run_queue (48.64, 2.7 µs) |
| new-order | pg | - | 0.00 µs | 0.00 µs | 83.28 µs | 0.10 µs | 255.05 µs | 0.00 µs | 117.12 µs | 1.48 µs | WalSync (0.24, 255.0 µs); ClientRead (0.11, 117.1 µs); CPU (0.08, 90.6 µs) |
| new-order | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 158.96 µs | 0.07 µs | 193.06 µs | 0.00 µs | 9.46 µs | 21.07 µs | log file sync (1.00, 193.1 µs); SQL*Net message to client (47.64, 9.5 µs); Disk file operations I/O (0.00, 0.1 µs) |
| payment | kds-group | - | - | 7.68 µs | 23.33 µs | 0.00 µs | 365.83 µs | 0.00 µs | 0.00 µs | 0.29 µs | commit.wal_flush (1.00, 365.8 µs); cpu.on_cpu (0.00, 30.5 µs); sched.run_queue (9.00, 0.5 µs) |
| payment | kds-strict | - | - | 7.91 µs | 23.93 µs | 0.00 µs | 335.66 µs | 0.00 µs | 0.00 µs | 0.33 µs | commit.wal_flush (1.00, 335.7 µs); cpu.on_cpu (0.00, 31.3 µs); sched.run_queue (9.00, 0.5 µs) |
| payment | pg | - | 0.00 µs | 0.00 µs | 15.06 µs | 0.00 µs | 257.75 µs | 0.00 µs | 52.79 µs | 0.19 µs | WalSync (0.25, 257.8 µs); ClientRead (0.05, 52.8 µs); CPU (0.03, 29.3 µs) |
| payment | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 25.27 µs | 0.00 µs | 174.65 µs | 0.00 µs | 1.81 µs | 2.72 µs | log file sync (1.00, 174.6 µs); SQL*Net message to client (8.00, 1.8 µs) |
| order-status | kds-group | - | - | 4.53 µs | 13.30 µs | 0.00 µs | 352.86 µs | 0.00 µs | 0.00 µs | 1.17 µs | commit.wal_flush (1.00, 352.9 µs); cpu.on_cpu (0.00, 17.5 µs); sched.run_queue (5.00, 0.3 µs) |
| order-status | kds-strict | - | - | 4.45 µs | 12.79 µs | 0.00 µs | 332.20 µs | 0.00 µs | 0.00 µs | 1.12 µs | commit.wal_flush (1.00, 332.2 µs); cpu.on_cpu (0.00, 17.0 µs); sched.run_queue (5.00, 0.3 µs) |
| order-status | pg | - | 5.36 µs | 5.36 µs | 8.45 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.15 µs | 0.42 µs | ClientRead (0.01, 10.1 µs); CPU (0.01, 7.0 µs) |
| order-status | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 14.52 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.76 µs | 5.31 µs | SQL*Net message to client (4.00, 0.8 µs) |
| delivery | kds-group | - | - | 74.49 µs | 242.33 µs | 0.00 µs | 447.87 µs | 0.00 µs | 0.00 µs | 1.45 µs | commit.wal_flush (1.00, 447.9 µs); cpu.on_cpu (0.00, 312.9 µs); sched.run_queue (72.00, 4.0 µs) |
| delivery | kds-strict | - | - | 74.87 µs | 242.12 µs | 0.00 µs | 375.82 µs | 0.00 µs | 0.00 µs | 1.57 µs | commit.wal_flush (1.00, 375.8 µs); cpu.on_cpu (0.00, 313.6 µs); sched.run_queue (72.00, 3.8 µs) |
| delivery | pg | - | 118.92 µs | 118.92 µs | 285.77 µs | 0.00 µs | 570.61 µs | 0.00 µs | 241.50 µs | 1.18 µs | CPU (0.35, 361.3 µs); WalInitSync (0.28, 292.4 µs); WalSync (0.26, 271.2 µs); ClientRead (0.23, 241.5 µs) |
| delivery | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 747.69 µs | 0.00 µs | 286.18 µs | 0.00 µs | 14.62 µs | 23.59 µs | log file switch (checkpoint incomplete) (0.00, 348.6 µs); log file sync (1.00, 286.2 µs); SQL*Net message to client (71.00, 14.6 µs); log file switch completion (0.00, 10.2 µs) |
| stock-level | kds-group | - | - | 4.94 µs | 69.99 µs | 0.00 µs | 364.09 µs | 0.00 µs | 0.00 µs | 0.37 µs | commit.wal_flush (1.00, 364.1 µs); cpu.on_cpu (0.00, 74.7 µs); sched.run_queue (4.00, 0.2 µs) |
| stock-level | kds-strict | - | - | 4.81 µs | 70.97 µs | 0.00 µs | 338.77 µs | 0.00 µs | 0.00 µs | 0.32 µs | commit.wal_flush (1.00, 338.8 µs); cpu.on_cpu (0.00, 75.2 µs); sched.run_queue (4.00, 0.2 µs) |
| stock-level | pg | - | 29.28 µs | 29.28 µs | 99.12 µs | 0.00 µs | 0.00 µs | 0.00 µs | 12.93 µs | 0.06 µs | CPU (0.14, 141.4 µs); ClientRead (0.01, 12.9 µs) |
| stock-level | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 81.41 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.62 µs | 1.61 µs | SQL*Net message to client (3.00, 0.6 µs) |
| mix | kds-group | - | - | 34.81 µs | 296.91 µs | 0.00 µs | 359.19 µs | 143.67 µs | 0.00 µs | 2.04 µs | commit.wal_flush (1.00, 359.2 µs); sched.run_queue (28.70, 199.2 µs); app.lock_wait (0.14, 143.7 µs); cpu.on_cpu (0.00, 133.1 µs) |
| mix | kds-strict | - | - | 34.87 µs | 539.70 µs | 0.00 µs | 355.11 µs | 162.77 µs | 0.00 µs | 2.05 µs | sched.run_queue (28.70, 440.5 µs); commit.wal_flush (1.00, 355.1 µs); app.lock_wait (0.14, 162.8 µs); cpu.on_cpu (0.00, 133.2 µs) |
| mix | pg | - | 8.93 µs | 8.93 µs | 73.14 µs | 0.19 µs | 514.03 µs | 96.50 µs | 103.78 µs | 0.94 µs | WALWrite (0.29, 307.2 µs); WalSync (0.18, 191.5 µs); ClientRead (0.10, 103.8 µs); transactionid (0.09, 96.5 µs) |
| mix | oracle | 0.00 µs | 0.00 µs | 0.01 µs | 524.42 µs | 1.64 µs | 314.87 µs | 75.23 µs | 8.58 µs | 14.76 µs | log file switch (checkpoint incomplete) (0.00, 328.9 µs); log file sync (0.92, 314.9 µs); log file switch completion (0.00, 57.6 µs); enq: TX - row lock contention (0.08, 52.8 µs) |

### s5 `one-terminal`

Checks: every check passed in every round. Rounds with figures (passed of run): kds-group 3 of 3, kds-strict 3 of 3, pg 3 of 3, oracle 3 of 3.

| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| new-order | kds-group | 300 ops | 1,125.6 µs (28.2 %) | 425.5 µs | 904.0 µs | 1,090.0 µs | 1,639.1 µs | 2,885.5 µs | 888 ops/s | 141.2 µs | 2.11 µs | 422.4 µs | 0 errors |
| new-order | kds-strict | 300 ops | 1,110.2 µs (5.9 %) | 423.8 µs | 906.0 µs | 1,085.2 µs | 1,485.8 µs | 2,068.7 µs | 900 ops/s | 140.5 µs | 2.03 µs | 399.7 µs | 0 errors |
| new-order | pg | 300 ops | 856.1 µs (30.6 %) | 418.1 µs | 672.8 µs | 839.8 µs | 1,114.5 µs | 1,819.8 µs | 1,167 ops/s | 223.7 µs | 1.59 µs | 296.3 µs | 0 errors |
| new-order | oracle | 300 ops | 1,330.0 µs (31.0 %) | 556.0 µs | 767.1 µs | 965.6 µs | 1,310.6 µs | 2,190.3 µs | 751 ops/s | 264.2 µs | 21.79 µs | 415.0 µs | 0 errors |
| payment | kds-group | 300 ops | 543.4 µs (72.2 %) | 438.1 µs | 450.3 µs | 463.8 µs | 936.1 µs | 1,350.4 µs | 1,837 ops/s | 30.8 µs | 0.31 µs | 93.8 µs | 0 errors |
| payment | kds-strict | 300 ops | 504.8 µs (12.3 %) | 439.5 µs | 447.1 µs | 460.6 µs | 579.0 µs | 1,378.4 µs | 1,978 ops/s | 31.3 µs | 0.28 µs | 74.2 µs | 0 errors |
| payment | pg | 300 ops | 274.1 µs (74.7 %) | 235.8 µs | 238.4 µs | 239.5 µs | 276.4 µs | 1,054.1 µs | 3,643 ops/s | 39.4 µs | 0.18 µs | 52.7 µs | 0 errors |
| payment | oracle | 300 ops | 285.1 µs (107.7 %) | 238.9 µs | 248.0 µs | 264.9 µs | 344.9 µs | 566.2 µs | 3,493 ops/s | 45.2 µs | 2.92 µs | 82.7 µs | 0 errors |
| order-status | kds-group | 300 ops | 433.4 µs (25.4 %) | 392.6 µs | 409.9 µs | 414.1 µs | 483.0 µs | 642.7 µs | 2,304 ops/s | 31.5 µs | 1.07 µs | 45.4 µs | 0 errors |
| order-status | kds-strict | 300 ops | 442.6 µs (5.6 %) | 399.0 µs | 414.5 µs | 423.6 µs | 496.4 µs | 824.1 µs | 2,254 ops/s | 38.0 µs | 1.30 µs | 45.1 µs | 0 errors |
| order-status | pg | 300 ops | 82.7 µs (7.5 %) | 72.3 µs | 75.8 µs | 78.4 µs | 86.1 µs | 125.7 µs | 11,952 ops/s | 24.9 µs | 0.41 µs | 34.5 µs | 0 errors |
| order-status | oracle | 300 ops | 73.8 µs (9.3 %) | 60.5 µs | 64.8 µs | 66.7 µs | 73.9 µs | 85.5 µs | 13,417 ops/s | 25.7 µs | 5.49 µs | 29.3 µs | 0 errors |
| delivery | kds-group | 300 ops | 1,440.9 µs (19.0 %) | 1,372.8 µs | 1,409.6 µs | 1,428.6 µs | 1,698.8 µs | 2,209.8 µs | 694 ops/s | 204.5 µs | 1.61 µs | 595.4 µs | 0 errors |
| delivery | kds-strict | 300 ops | 1,488.4 µs (2.3 %) | 1,367.5 µs | 1,397.4 µs | 1,412.4 µs | 1,834.0 µs | 2,504.3 µs | 671 ops/s | 206.3 µs | 1.56 µs | 599.1 µs | 0 errors |
| delivery | pg | 300 ops | 1,636.3 µs (30.9 %) | 1,393.3 µs | 1,447.5 µs | 1,475.9 µs | 2,221.7 µs | 3,399.5 µs | 611 ops/s | 318.3 µs | 1.09 µs | 767.4 µs | 0 errors |
| delivery | oracle | 300 ops | 2,295.0 µs (32.0 %) | 1,380.5 µs | 1,456.3 µs | 1,476.7 µs | 2,557.4 µs | 4,799.3 µs | 435 ops/s | 357.7 µs | 23.37 µs | 789.2 µs | 0 errors |
| stock-level | kds-group | 300 ops | 482.7 µs (58.3 %) | 429.6 µs | 464.2 µs | 472.2 µs | 529.3 µs | 851.4 µs | 2,067 ops/s | 30.4 µs | 0.34 µs | 99.6 µs | 0 errors |
| stock-level | kds-strict | 300 ops | 485.1 µs (9.8 %) | 431.2 µs | 456.7 µs | 468.7 µs | 583.2 µs | 834.2 µs | 2,057 ops/s | 27.9 µs | 0.31 µs | 97.7 µs | 0 errors |
| stock-level | pg | 300 ops | 187.8 µs (0.4 %) | 170.9 µs | 178.5 µs | 182.6 µs | 203.6 µs | 326.4 µs | 5,303 ops/s | 18.3 µs | 0.07 µs | 150.3 µs | 0 errors |
| stock-level | oracle | 300 ops | 124.6 µs (4.8 %) | 108.6 µs | 113.7 µs | 116.2 µs | 123.7 µs | 254.0 µs | 7,949 ops/s | 16.2 µs | 1.59 µs | 91.8 µs | 0 errors |
| mix | kds-group | 84,200 ops | 713.0 µs (13.2 %) | 227.6 µs | 341.9 µs | 529.0 µs | 1,359.1 µs | 1,710.0 µs | 1,402 ops/s | 80.7 µs | 1.15 µs | 254.4 µs | 0 errors |
| mix | kds-strict | 76,841 ops | 780.7 µs (18.9 %) | 190.5 µs | 338.0 µs | 570.0 µs | 1,428.0 µs | 2,471.9 µs | 1,281 ops/s | 83.6 µs | 1.12 µs | 254.0 µs | 0 errors |
| mix | pg | 99,811 ops | 601.0 µs (20.4 %) | 77.2 µs | 243.3 µs | 491.3 µs | 1,462.2 µs | 1,800.3 µs | 1,664 ops/s | 132.3 µs | 0.91 µs | 207.8 µs | 0 errors |
| mix | oracle | 73,344 ops | 818.0 µs (3.3 %) | 65.2 µs | 262.6 µs | 561.4 µs | 1,491.0 µs | 3,037.1 µs | 1,222 ops/s | 146.5 µs | 12.09 µs | 267.9 µs | 0 errors |

| phase | kind | arm | ops a round | mean | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| new-order | new-order | kds-group | 296 ops | 1,125.7 µs | 1,091.6 µs | 1,556.7 µs | 2,885.5 µs |
| new-order | new-order-rollback | kds-group | 4 ops | 718.4 µs | 916.7 µs | 1,026.8 µs | 1,026.8 µs |
| new-order | new-order | kds-strict | 296 ops | 1,115.6 µs | 1,087.9 µs | 1,485.8 µs | 2,068.7 µs |
| new-order | new-order-rollback | kds-strict | 4 ops | 681.2 µs | 888.1 µs | 921.7 µs | 921.7 µs |
| new-order | new-order | pg | 296 ops | 858.5 µs | 839.8 µs | 1,114.5 µs | 1,819.8 µs |
| new-order | new-order-rollback | pg | 4 ops | 674.3 µs | 883.7 µs | 899.1 µs | 899.1 µs |
| new-order | new-order | oracle | 296 ops | 1,334.8 µs | 965.6 µs | 1,310.6 µs | 2,190.3 µs |
| new-order | new-order-rollback | oracle | 4 ops | 950.2 µs | 1,196.9 µs | 1,252.3 µs | 1,252.3 µs |
| payment | payment | kds-group | 300 ops | 543.4 µs | 463.8 µs | 936.1 µs | 1,350.4 µs |
| payment | payment | kds-strict | 300 ops | 504.8 µs | 460.6 µs | 579.0 µs | 1,378.4 µs |
| payment | payment | pg | 300 ops | 274.1 µs | 239.5 µs | 276.4 µs | 1,054.1 µs |
| payment | payment | oracle | 300 ops | 285.1 µs | 264.9 µs | 344.9 µs | 566.2 µs |
| order-status | order-status | kds-group | 300 ops | 433.4 µs | 414.1 µs | 483.0 µs | 642.7 µs |
| order-status | order-status | kds-strict | 300 ops | 442.6 µs | 423.6 µs | 496.4 µs | 824.1 µs |
| order-status | order-status | pg | 300 ops | 82.7 µs | 78.4 µs | 86.1 µs | 125.7 µs |
| order-status | order-status | oracle | 300 ops | 73.8 µs | 66.7 µs | 73.9 µs | 85.5 µs |
| delivery | delivery | kds-group | 300 ops | 1,440.9 µs | 1,428.6 µs | 1,698.8 µs | 2,209.8 µs |
| delivery | delivery | kds-strict | 300 ops | 1,488.4 µs | 1,412.4 µs | 1,834.0 µs | 2,504.3 µs |
| delivery | delivery | pg | 300 ops | 1,636.3 µs | 1,475.9 µs | 2,221.7 µs | 3,399.5 µs |
| delivery | delivery | oracle | 300 ops | 2,295.0 µs | 1,476.7 µs | 2,557.4 µs | 4,799.3 µs |
| stock-level | stock-level | kds-group | 300 ops | 482.7 µs | 472.2 µs | 529.3 µs | 851.4 µs |
| stock-level | stock-level | kds-strict | 300 ops | 485.1 µs | 468.7 µs | 583.2 µs | 834.2 µs |
| stock-level | stock-level | pg | 300 ops | 187.8 µs | 182.6 µs | 203.6 µs | 326.4 µs |
| stock-level | stock-level | oracle | 300 ops | 124.6 µs | 116.2 µs | 123.7 µs | 254.0 µs |
| mix | delivery | kds-group | 3,394 ops | 1,569.7 µs | 1,487.0 µs | 1,798.4 µs | 2,916.3 µs |
| mix | new-order | kds-group | 37,538 ops | 1,011.6 µs | 941.4 µs | 1,278.4 µs | 1,847.9 µs |
| mix | new-order-rollback | kds-group | 393 ops | 676.1 µs | 653.7 µs | 976.1 µs | 1,395.5 µs |
| mix | order-status | kds-group | 3,380 ops | 337.9 µs | 298.9 µs | 407.8 µs | 731.7 µs |
| mix | payment | kds-group | 36,057 ops | 383.4 µs | 343.1 µs | 456.6 µs | 821.9 µs |
| mix | stock-level | kds-group | 3,438 ops | 399.0 µs | 355.6 µs | 486.8 µs | 764.1 µs |
| mix | delivery | kds-strict | 3,106 ops | 1,680.2 µs | 1,472.5 µs | 1,953.9 µs | 11,969.9 µs |
| mix | new-order | kds-strict | 34,208 ops | 1,072.1 µs | 946.7 µs | 1,339.3 µs | 2,929.8 µs |
| mix | new-order-rollback | kds-strict | 364 ops | 674.6 µs | 656.0 µs | 978.5 µs | 1,141.8 µs |
| mix | order-status | kds-strict | 3,084 ops | 397.4 µs | 296.2 µs | 435.0 µs | 1,185.9 µs |
| mix | payment | kds-strict | 32,912 ops | 462.7 µs | 340.6 µs | 485.9 µs | 1,376.0 µs |
| mix | stock-level | kds-strict | 3,167 ops | 441.4 µs | 352.3 µs | 522.2 µs | 1,152.4 µs |
| mix | delivery | pg | 4,025 ops | 1,630.6 µs | 1,536.9 µs | 1,837.5 µs | 2,959.5 µs |
| mix | new-order | pg | 44,437 ops | 878.3 µs | 827.9 µs | 1,132.3 µs | 1,938.7 µs |
| mix | new-order-rollback | pg | 469 ops | 658.6 µs | 644.9 µs | 950.3 µs | 1,122.6 µs |
| mix | order-status | pg | 4,055 ops | 106.8 µs | 100.4 µs | 117.9 µs | 267.8 µs |
| mix | payment | pg | 42,771 ops | 288.9 µs | 243.6 µs | 284.7 µs | 1,110.2 µs |
| mix | stock-level | pg | 4,054 ops | 320.9 µs | 308.5 µs | 434.6 µs | 583.7 µs |
| mix | delivery | oracle | 2,955 ops | 2,313.0 µs | 1,509.9 µs | 2,203.0 µs | 12,744.5 µs |
| mix | new-order | oracle | 32,652 ops | 1,219.3 µs | 943.0 µs | 1,397.3 µs | 3,974.0 µs |
| mix | new-order-rollback | oracle | 349 ops | 1,342.3 µs | 913.3 µs | 1,333.3 µs | 3,746.7 µs |
| mix | order-status | oracle | 2,953 ops | 93.3 µs | 79.8 µs | 120.0 µs | 458.7 µs |
| mix | payment | oracle | 31,423 ops | 392.7 µs | 269.8 µs | 381.5 µs | 2,079.9 µs |
| mix | stock-level | oracle | 3,012 ops | 166.4 µs | 151.9 µs | 229.1 µs | 546.4 µs |

Where the time went, µs an operation by each engine's own accounting (`-`: not reported by the engine; PostgreSQL's from the profile pass):

| phase | arm | parse | optimise | parse_optimise | execution | disk_read | commit | lock | network | formatting_client | top events (an op: count, µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| new-order | kds-group | - | - | 59.38 µs | 150.04 µs | 0.00 µs | 371.68 µs | 0.00 µs | 0.00 µs | 2.11 µs | commit.wal_flush (0.99, 371.7 µs); cpu.on_cpu (0.00, 206.6 µs); sched.run_queue (48.64, 2.8 µs) |
| new-order | kds-strict | - | - | 58.25 µs | 143.79 µs | 0.00 µs | 345.85 µs | 0.00 µs | 0.00 µs | 2.03 µs | commit.wal_flush (0.99, 345.9 µs); cpu.on_cpu (0.00, 199.4 µs); sched.run_queue (48.64, 2.6 µs) |
| new-order | pg | - | 0.00 µs | 0.00 µs | 83.28 µs | 0.18 µs | 118.79 µs | 0.00 µs | 108.13 µs | 1.50 µs | WalSync (0.11, 118.8 µs); ClientRead (0.11, 108.1 µs); CPU (0.06, 68.4 µs) |
| new-order | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 182.90 µs | 0.00 µs | 160.94 µs | 0.00 µs | 9.86 µs | 21.79 µs | log file sync (1.00, 160.9 µs); SQL*Net message to client (47.64, 9.9 µs); log file switch completion (0.00, 9.6 µs); log file switch (checkpoint incomplete) (0.00, 0.0 µs) |
| payment | kds-group | - | - | 9.70 µs | 29.84 µs | 0.00 µs | 363.62 µs | 0.00 µs | 0.00 µs | 0.31 µs | commit.wal_flush (1.00, 363.6 µs); cpu.on_cpu (0.00, 39.0 µs); sched.run_queue (9.00, 0.6 µs) |
| payment | kds-strict | - | - | 8.10 µs | 25.11 µs | 0.00 µs | 362.38 µs | 0.00 µs | 0.00 µs | 0.28 µs | commit.wal_flush (1.00, 362.4 µs); cpu.on_cpu (0.00, 32.7 µs); sched.run_queue (9.00, 0.5 µs) |
| payment | pg | - | 0.00 µs | 0.00 µs | 13.95 µs | 0.00 µs | 108.19 µs | 0.00 µs | 32.75 µs | 0.16 µs | WalSync (0.10, 108.2 µs); ClientRead (0.03, 32.7 µs) |
| payment | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 25.20 µs | 0.00 µs | 136.69 µs | 0.00 µs | 1.91 µs | 2.92 µs | log file sync (1.00, 136.7 µs); SQL*Net message to client (8.00, 1.9 µs) |
| order-status | kds-group | - | - | 4.70 µs | 13.60 µs | 0.00 µs | 339.79 µs | 0.00 µs | 0.00 µs | 1.07 µs | commit.wal_flush (1.00, 339.8 µs); cpu.on_cpu (0.00, 18.0 µs); sched.run_queue (5.00, 0.3 µs) |
| order-status | kds-strict | - | - | 4.66 µs | 13.54 µs | 0.00 µs | 337.35 µs | 0.00 µs | 0.00 µs | 1.30 µs | commit.wal_flush (1.00, 337.3 µs); cpu.on_cpu (0.00, 17.9 µs); sched.run_queue (5.00, 0.3 µs) |
| order-status | pg | - | 5.71 µs | 5.71 µs | 8.80 µs | 0.00 µs | 0.00 µs | 0.00 µs | 9.73 µs | 0.41 µs | ClientRead (0.01, 9.7 µs); CPU (0.00, 3.9 µs) |
| order-status | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 14.55 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.77 µs | 5.49 µs | SQL*Net message to client (4.00, 0.8 µs) |
| delivery | kds-group | - | - | 75.56 µs | 244.70 µs | 0.00 µs | 373.66 µs | 0.00 µs | 0.00 µs | 1.61 µs | commit.wal_flush (1.00, 373.7 µs); cpu.on_cpu (0.00, 316.4 µs); sched.run_queue (72.00, 3.8 µs) |
| delivery | kds-strict | - | - | 77.02 µs | 246.38 µs | 0.00 µs | 377.25 µs | 0.00 µs | 0.00 µs | 1.56 µs | commit.wal_flush (1.00, 377.2 µs); cpu.on_cpu (0.00, 319.6 µs); sched.run_queue (72.00, 3.8 µs) |
| delivery | pg | - | 119.13 µs | 119.13 µs | 283.12 µs | 0.00 µs | 147.35 µs | 0.00 µs | 162.93 µs | 1.09 µs | CPU (0.40, 425.6 µs); ClientRead (0.16, 162.9 µs); WalSync (0.12, 125.9 µs); WalInitSync (0.01, 14.5 µs) |
| delivery | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 1,089.93 µs | 0.00 µs | 229.49 µs | 0.00 µs | 15.33 µs | 23.37 µs | log file switch (checkpoint incomplete) (0.00, 699.1 µs); log file sync (1.00, 229.5 µs); SQL*Net message to client (71.00, 15.3 µs); log file switch completion (0.00, 10.4 µs) |
| stock-level | kds-group | - | - | 4.72 µs | 72.45 µs | 0.00 µs | 336.78 µs | 0.00 µs | 0.00 µs | 0.34 µs | commit.wal_flush (1.00, 336.8 µs); cpu.on_cpu (0.00, 76.9 µs); sched.run_queue (4.00, 0.2 µs) |
| stock-level | kds-strict | - | - | 4.64 µs | 71.44 µs | 0.00 µs | 336.89 µs | 0.00 µs | 0.00 µs | 0.31 µs | commit.wal_flush (1.00, 336.9 µs); cpu.on_cpu (0.00, 75.8 µs); sched.run_queue (4.00, 0.2 µs) |
| stock-level | pg | - | 29.47 µs | 29.47 µs | 100.21 µs | 0.00 µs | 0.00 µs | 0.00 µs | 10.06 µs | 0.11 µs | CPU (0.13, 137.8 µs); ClientRead (0.01, 10.1 µs) |
| stock-level | oracle | 0.00 µs | 0.00 µs | 0.00 µs | 79.74 µs | 0.00 µs | 0.00 µs | 0.00 µs | 0.61 µs | 1.59 µs | SQL*Net message to client (3.00, 0.6 µs) |
| mix | kds-group | - | - | 34.49 µs | 98.11 µs | 0.00 µs | 270.42 µs | 0.00 µs | 0.00 µs | 1.15 µs | commit.wal_flush (1.00, 270.4 µs); cpu.on_cpu (0.00, 130.9 µs); sched.run_queue (28.74, 1.7 µs) |
| mix | kds-strict | - | - | 34.56 µs | 98.65 µs | 0.00 µs | 331.27 µs | 0.00 µs | 0.00 µs | 1.12 µs | commit.wal_flush (1.00, 331.3 µs); cpu.on_cpu (0.00, 131.8 µs); sched.run_queue (28.71, 1.6 µs) |
| mix | pg | - | 8.20 µs | 8.20 µs | 62.48 µs | 0.08 µs | 111.58 µs | 0.00 µs | 71.94 µs | 0.84 µs | WalSync (0.10, 109.0 µs); ClientRead (0.07, 71.9 µs); CPU (0.06, 66.7 µs); WalInitSync (0.00, 1.8 µs) |
| mix | oracle | 0.01 µs | 0.00 µs | 0.01 µs | 205.18 µs | 5.86 µs | 215.56 µs | 0.00 µs | 5.84 µs | 12.09 µs | log file sync (0.92, 215.6 µs); log file switch (checkpoint incomplete) (0.00, 82.6 µs); log file switch completion (0.00, 8.9 µs); SQL*Net message to client (27.69, 5.8 µs) |

### s4 answers

Answers: every group's digest equal across all 12 runs read (each arm, engine and round).

