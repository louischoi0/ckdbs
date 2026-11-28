
#### C1 `idle` (5+5 rounds, 500 inserts each; rounds: OLD 5, NEW 5)

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| inserts/s (derived, 1e6/mean_us) | 6389 | 6843 | +7.1 % | 69.5 % / 60.6 % | no claim (spread >20%) |
| mean_us | 156.5 | 146.1 | -6.6 % | 142.4 % / 152.5 % | no claim (spread >20%) |
| p0_us | 130.9 | 130.5 | -0.3 % | 6.3 % / 8.3 % | inside the spread |
| p25_us | 140.5 | 134.7 | -4.1 % | 16.5 % / 23.5 % | no claim (spread >20%) |
| p50_us | 146.1 | 137.7 | -5.7 % | 27.0 % / 31.5 % | no claim (spread >20%) |
| p95_us | 202.9 | 171.2 | -15.6 % | 78.5 % / 119.0 % | no claim (spread >20%) |
| p99_us | 342.1 | 270.1 | -21.0 % | 3135.8 % / 3905.1 % | no claim (spread >20%) |
| server CPU us/insert | 14.66 | 14.54 | -0.9 % | 97.4 % / 137.5 % | no claim (spread >20%) |
| d_commit_wal_flush_count per round | [0, 0, 0, 0, 0] | [500, 500, 500, 500, 500] | | | |
| d_commit_wal_flush_us per round | [0, 0, 0, 0, 0] | [60837, 151049, 60583, 59814, 60059] | | | |
| booked us per wait (NEW) | | [121.7, 302.1, 121.2, 119.6, 120.1] | | | |
| d_wal_syncs per round | [500, 500, 500, 500, 500] | [500, 500, 500, 500, 500] | | | |
| per-round mean_us | [138.8, 361.8, 157.5, 151.1, 156.5] | [146.6, 368.7, 145.8, 146.1, 145.8] | | | |
| per-round server CPU us/insert | [12.1, 26.38, 18.09, 13.84, 14.66] | [11.99, 31.98, 12.85, 14.54, 14.88] | | | |

#### C1 `after-load` (5+5 rounds, 500 inserts each; rounds: OLD 5, NEW 5)

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| inserts/s (derived, 1e6/mean_us) | 4665 | 4820 | +3.3 % | 53.4 % / 52.8 % | no claim (spread >20%) |
| mean_us | 214.4 | 207.4 | -3.2 % | 96.3 % / 103.0 % | no claim (spread >20%) |
| p0_us | 183.6 | 183.2 | -0.3 % | 3.3 % / 4.4 % | inside the spread |
| p25_us | 198.5 | 193.7 | -2.4 % | 18.9 % / 23.6 % | no claim (spread >20%) |
| p50_us | 205.8 | 197.9 | -3.9 % | 25.1 % / 27.5 % | no claim (spread >20%) |
| p95_us | 242.4 | 240.5 | -0.8 % | 62.6 % / 79.8 % | no claim (spread >20%) |
| p99_us | 454.4 | 367.5 | -19.1 % | 2311.4 % / 2846.5 % | no claim (spread >20%) |
| server CPU us/insert | 16.02 | 12.27 | -23.4 % | 107.2 % / 168.1 % | no claim (spread >20%) |
| d_commit_wal_flush_count per round | [0, 0, 0, 0, 0] | [500, 500, 500, 500, 500] | | | |
| d_commit_wal_flush_us per round | [0, 0, 0, 0, 0] | [87975, 171535, 89648, 89549, 88945] | | | |
| booked us per wait (NEW) | | [175.9, 343.1, 179.3, 179.1, 177.9] | | | |
| d_wal_syncs per round | [500, 500, 500, 500, 500] | [500, 500, 500, 500, 500] | | | |
| per-round mean_us | [202.5, 409.0, 222.9, 214.4, 208.2] | [201.9, 415.5, 208.5, 202.5, 207.4] | | | |
| per-round server CPU us/insert | [14.55, 29.52, 16.02, 17.79, 12.34] | [11.05, 31.56, 13.64, 10.93, 12.27] | | | |

#### C1 `recycled` (5+5 rounds, 500 inserts each; rounds: OLD 5, NEW 5)

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| inserts/s (derived, 1e6/mean_us) | 6525 | 6348 | -2.7 % | 7.1 % / 67.3 % | no claim (spread >20%) |
| mean_us | 153.3 | 157.5 | +2.8 % | 7.3 % / 162.7 % | no claim (spread >20%) |
| p0_us | 134.2 | 132.2 | -1.5 % | 8.0 % / 11.0 % | inside the spread |
| p25_us | 138.1 | 136.9 | -0.8 % | 8.1 % / 27.7 % | no claim (spread >20%) |
| p50_us | 142.3 | 143.4 | +0.8 % | 7.1 % / 41.8 % | no claim (spread >20%) |
| p95_us | 182.5 | 189.7 | +3.9 % | 36.3 % / 120.3 % | no claim (spread >20%) |
| p99_us | 380.1 | 393.7 | +3.6 % | 25.2 % / 3383.9 % | no claim (spread >20%) |
| server CPU us/insert | 14.08 | 12.89 | -8.4 % | 17.9 % / 149.4 % | no claim (spread >20%) |
| d_commit_wal_flush_count per round | [0, 0, 0, 0, 0] | [500, 500, 500, 500, 500] | | | |
| d_commit_wal_flush_us per round | [0, 0, 0, 0, 0] | [61993, 163789, 60615, 60756, 59892] | | | |
| booked us per wait (NEW) | | [124.0, 327.6, 121.2, 121.5, 119.8] | | | |
| d_wal_syncs per round | [500, 500, 500, 500, 500] | [500, 500, 500, 500, 500] | | | |
| per-round mean_us | [150.4, 154.5, 161.1, 149.9, 153.3] | [157.5, 404.7, 159.6, 151.5, 148.4] | | | |
| per-round server CPU us/insert | [12.69, 14.25, 13.56, 14.08, 15.21] | [15.72, 31.81, 12.89, 12.73, 12.55] | | | |

### c2


| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| booking rate meta.tps (bookings/s) | 2098 | 2108 | +0.5 % | 2.2 % / 0.8 % | inside the spread |
| per-round tps | [2079, 2073, 2120, 2100, 2098] | [2103, 2115, 2101, 2108, 2117] | | | |

**phase `commit`** (ops/round 3000; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 7429 | 7418 | -0.1 % | 1.5 % / 2.5 % | inside the spread |
| mean_us | 134.6 | 134.8 | +0.1 % | 1.6 % / 2.4 % | inside the spread |
| p0_us | 121.1 | 121.5 | +0.3 % | 5.8 % / 5.1 % | inside the spread |
| p25_us | 128.6 | 128.9 | +0.2 % | 0.9 % / 0.5 % | inside the spread |
| p50_us | 129.4 | 129.7 | +0.2 % | 0.7 % / 0.5 % | inside the spread |
| p95_us | 154.4 | 153.6 | -0.5 % | 1.2 % / 1.1 % | inside the spread |
| p99_us | 170.1 | 167.3 | -1.6 % | 8.4 % / 4.4 % | inside the spread |

**phase `freight-insert`** (ops/round 3000; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 47393 | 48077 | +1.4 % | 3.7 % / 2.9 % | inside the spread |
| mean_us | 21.1 | 20.8 | -1.4 % | 3.8 % / 2.9 % | inside the spread |
| p0_us | 18.4 | 18.4 | +0.0 % | 2.2 % / 1.1 % | inside the spread |
| p25_us | 20.0 | 19.9 | -0.5 % | 2.0 % / 3.0 % | inside the spread |
| p50_us | 20.3 | 20.2 | -0.5 % | 2.5 % / 1.0 % | inside the spread |
| p95_us | 21.9 | 21.5 | -1.8 % | 10.0 % / 5.1 % | inside the spread |
| p99_us | 31.0 | 30.7 | -1.0 % | 15.8 % / 11.1 % | inside the spread |

**phase `cargo-lookup`** (ops/round 3402; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 39216 | 39841 | +1.6 % | 2.8 % / 2.8 % | inside the spread |
| mean_us | 25.5 | 25.1 | -1.6 % | 2.7 % / 2.8 % | inside the spread |
| p0_us | 20.8 | 20.9 | +0.5 % | 1.0 % / 1.4 % | inside the spread |
| p25_us | 24.3 | 24.2 | -0.4 % | 1.6 % / 3.3 % | inside the spread |
| p50_us | 24.6 | 24.6 | +0.0 % | 2.0 % / 1.2 % | inside the spread |
| p95_us | 26.6 | 26.3 | -1.1 % | 8.6 % / 4.9 % | inside the spread |
| p99_us | 38.6 | 35.0 | -9.3 % | 24.9 % / 43.4 % | no claim (spread >20%) |

### c3


| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| booking rate meta.tps (bookings/s) | 9507 | 9636 | +1.4 % | 54.5 % / 3.0 % | no claim (spread >20%) |
| per-round tps | [4725, 9660, 9786, 9507, 4610] | [9636, 9860, 9638, 9572, 9607] | | | |

**phase `commit`** (ops/round 3000; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 4810 | 4900 | +1.9 % | 81.0 % / 3.8 % | no claim (spread >20%) |
| mean_us | 207.9 | 204.1 | -1.8 % | 338.7 % / 3.8 % | no claim (spread >20%) |
| p0_us | 126.9 | 127.0 | +0.1 % | 2.5 % / 2.0 % | inside the spread |
| p25_us | 169.3 | 165.0 | -2.5 % | 5.4 % / 1.9 % | inside the spread |
| p50_us | 206.2 | 202.0 | -2.0 % | 7.2 % / 2.6 % | inside the spread |
| p95_us | 288.0 | 279.3 | -3.0 % | 3649.1 % / 9.3 % | no claim (spread >20%) |
| p99_us | 341.7 | 337.1 | -1.3 % | 3204.9 % / 13.2 % | no claim (spread >20%) |

**phase `freight-insert`** (ops/round 3044; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 33784 | 36630 | +8.4 % | 23.9 % / 4.7 % | no claim (spread >20%) |
| mean_us | 29.6 | 27.3 | -7.8 % | 24.3 % / 4.8 % | no claim (spread >20%) |
| p0_us | 17.0 | 16.8 | -1.2 % | 14.1 % / 4.2 % | inside the spread |
| p25_us | 21.2 | 21.1 | -0.5 % | 4.2 % / 1.4 % | inside the spread |
| p50_us | 22.7 | 22.5 | -0.9 % | 6.2 % / 2.7 % | inside the spread |
| p95_us | 41.5 | 36.6 | -11.8 % | 24.1 % / 25.4 % | no claim (spread >20%) |
| p99_us | 143.6 | 149.8 | +4.3 % | 138.0 % / 63.8 % | no claim (spread >20%) |

**phase `cargo-lookup`** (ops/round 3389; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 30303 | 30675 | +1.2 % | 34.0 % / 4.4 % | no claim (spread >20%) |
| mean_us | 33.0 | 32.6 | -1.2 % | 45.2 % / 4.3 % | no claim (spread >20%) |
| p0_us | 21.4 | 21.4 | +0.0 % | 5.6 % / 7.0 % | inside the spread |
| p25_us | 25.8 | 25.7 | -0.4 % | 5.4 % / 1.2 % | inside the spread |
| p50_us | 27.2 | 27.2 | +0.0 % | 8.5 % / 0.7 % | inside the spread |
| p95_us | 43.2 | 40.8 | -5.6 % | 372.5 % / 19.9 % | no claim (spread >20%) |
| p99_us | 215.5 | 199.9 | -7.2 % | 132.4 % / 65.1 % | no claim (spread >20%) |

verify failures / error replies per round (OLD | NEW): []

### c4


**phase `pk-user`** (ops/round 200; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 36900 | 35714 | -3.2 % | 20.5 % / 27.7 % | no claim (spread >20%) |
| mean_us | 27.1 | 28.0 | +3.3 % | 22.9 % / 28.2 % | no claim (spread >20%) |
| p0_us | 22.8 | 25.1 | +10.1 % | 21.5 % / 38.6 % | no claim (spread >20%) |
| p25_us | 25.3 | 25.8 | +2.0 % | 19.8 % / 36.4 % | no claim (spread >20%) |
| p50_us | 26.1 | 26.2 | +0.4 % | 26.8 % / 34.4 % | no claim (spread >20%) |
| p95_us | 28.0 | 28.4 | +1.4 % | 30.7 % / 28.9 % | no claim (spread >20%) |
| p99_us | 33.9 | 33.2 | -2.1 % | 33.3 % / 26.5 % | no claim (spread >20%) |

**phase `loans-by-user`** (ops/round 200; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 7337 | 7321 | -0.2 % | 2.6 % / 5.1 % | inside the spread |
| mean_us | 136.3 | 136.6 | +0.2 % | 2.6 % / 5.2 % | inside the spread |
| p0_us | 128.6 | 128.0 | -0.5 % | 1.3 % / 1.6 % | inside the spread |
| p25_us | 132.8 | 132.3 | -0.4 % | 0.8 % / 1.0 % | inside the spread |
| p50_us | 135.2 | 134.4 | -0.6 % | 0.5 % / 0.8 % | inside the spread |
| p95_us | 143.1 | 143.4 | +0.2 % | 25.1 % / 44.1 % | no claim (spread >20%) |
| p99_us | 161.5 | 166.9 | +3.3 % | 27.6 % / 38.3 % | no claim (spread >20%) |

**phase `count-by-user`** (ops/round 200; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 7628 | 7686 | +0.8 % | 2.4 % / 3.4 % | inside the spread |
| mean_us | 131.1 | 130.1 | -0.8 % | 2.4 % / 3.4 % | inside the spread |
| p0_us | 126.3 | 125.6 | -0.6 % | 2.2 % / 3.6 % | inside the spread |
| p25_us | 128.9 | 128.3 | -0.5 % | 1.7 % / 3.6 % | inside the spread |
| p50_us | 129.9 | 129.6 | -0.2 % | 2.0 % / 3.8 % | inside the spread |
| p95_us | 135.8 | 134.7 | -0.8 % | 4.8 % / 3.7 % | inside the spread |
| p99_us | 139.3 | 138.7 | -0.4 % | 71.4 % / 1.8 % | no claim (spread >20%) |
