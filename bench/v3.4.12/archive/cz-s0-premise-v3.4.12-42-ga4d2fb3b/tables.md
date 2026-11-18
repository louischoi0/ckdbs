### 0. The host at each cell (bench/README.md rule 4), as the driver recorded it

`competing` counts the processes matching `cc1plus|cmake --build|ctest` at the cell's start and end; a bare `kds_tests` is not in that pattern, and the run script's gate tests for it by name. `driver wait` is the driver's own wait for a 1-minute load under 1.5, which gives up after 120 s and starts.

| cell | run | driver wait | load at start | load at end | competing at start | competing at end |
|---|---|---|---|---|---|---|
| isolated | r1 | 25 s | 1.45 | 1.11 | 0 processes | 0 processes |
| isolated | r2 | 5 s | 1.45 | 1.19 | 0 processes | 0 processes |
| isolated | r3 | 20 s | 1.46 | 1.04 | 0 processes | 0 processes |
| isolated | r4 | 15 s | 1.48 | 1.29 | 0 processes | 0 processes |
| isolated | r5 | 0 s | 1.40 | 1.36 | 0 processes | 0 processes |
| iso0 | r1 | 20 s | 1.39 | 1.08 | 0 processes | 0 processes |
| iso0 | r2 | 0 s | 1.28 | 1.21 | 0 processes | 3 processes |
| iso0 | r3 | 120 s | 7.41 | 7.69 | 11 processes | 11 processes |
| iso0 | r4 | 15 s | 1.44 | 1.10 | 0 processes | 0 processes |
| iso0 | r5 | 15 s | 1.46 | 1.11 | 0 processes | 0 processes |
| levels | r1 | 0 s | 1.11 | 1.39 | 0 processes | 0 processes |
| levels | r2 | 0 s | 1.19 | 1.60 | 0 processes | 0 processes |
| levels | r3 | 0 s | 1.04 | 1.55 | 0 processes | 0 processes |
| levels | r4 | 0 s | 1.29 | 1.57 | 0 processes | 0 processes |
| levels | r5 | 0 s | 1.36 | 2.47 | 0 processes | 11 processes |
| nockpt | r1 | 0 s | 1.39 | 1.76 | 0 processes | 0 processes |
| nockpt | r2 | 5 s | 1.47 | 1.28 | 0 processes | 0 processes |
| nockpt | r3 | 5 s | 1.43 | 1.57 | 0 processes | 0 processes |
| nockpt | r4 | 5 s | 1.45 | 1.67 | 0 processes | 0 processes |
| nockpt | r5 | 20 s | 1.39 | 1.79 | 0 processes | 0 processes |
| half | r1 | 0 s | 1.08 | 1.58 | 0 processes | 0 processes |
| half | r2 | 0 s | 1.21 | 2.62 | 3 processes | 3 processes |
| half | r3 | 20 s | 1.44 | 1.88 | 0 processes | 0 processes |
| half | r4 | 0 s | 1.10 | 1.40 | 0 processes | 0 processes |
| half | r5 | 0 s | 1.11 | 1.81 | 0 processes | 0 processes |

### 1. Reader schedule latency (M1, M4)

#### 1.1 per run
| cell | run | samples | p50 | p99 | p99.9 | max | slow samples (>= 1 ms) | stall samples (>= 10 ms) | stall episodes |
|---|---|---|---|---|---|---|---|---|---|
| isolated | r1 | 4006 samples | 329 µs | 2.4 ms | 4.5 ms | 5.9 ms | 79 samples | 0 samples | 0 episodes |
| isolated | r2 | 4006 samples | 334 µs | 1.4 ms | 4.4 ms | 5.8 ms | 57 samples | 0 samples | 0 episodes |
| isolated | r3 | 4006 samples | 325 µs | 1.4 ms | 4.9 ms | 5.0 ms | 66 samples | 0 samples | 0 episodes |
| isolated | r4 | 4006 samples | 322 µs | 1.6 ms | 5.3 ms | 5.6 ms | 76 samples | 0 samples | 0 episodes |
| isolated | r5 | 4006 samples | 328 µs | 1.4 ms | 4.4 ms | 5.0 ms | 53 samples | 0 samples | 0 episodes |
| iso0 | r1 | 4006 samples | 326 µs | 1.1 ms | 4.1 ms | 4.6 ms | 45 samples | 0 samples | 0 episodes |
| iso0 | r2 | 4006 samples | 332 µs | 1.5 ms | 4.4 ms | 5.4 ms | 74 samples | 0 samples | 0 episodes |
| iso0 | r3 | 4006 samples | 341 µs | 2.5 ms | 5.6 ms | 8.4 ms | 109 samples | 0 samples | 0 episodes |
| iso0 | r4 | 4006 samples | 323 µs | 1.7 ms | 4.6 ms | 10.1 ms | 72 samples | 1 samples | 1 episodes |
| iso0 | r5 | 4006 samples | 315 µs | 1.9 ms | 5.3 ms | 6.4 ms | 71 samples | 0 samples | 0 episodes |
| levels | r1 | 4006 samples | 333 µs | 3.2 ms | 71.1 ms | 100.9 ms | 106 samples | 13 samples | 1 episodes |
| levels | r2 | 4006 samples | 331 µs | 2.4 ms | 41.1 ms | 70.6 ms | 88 samples | 9 samples | 1 episodes |
| levels | r3 | 4006 samples | 329 µs | 7.5 ms | 79.5 ms | 94.3 ms | 178 samples | 38 samples | 3 episodes |
| levels | r4 | 4006 samples | 322 µs | 1.8 ms | 5.2 ms | 6.6 ms | 101 samples | 0 samples | 0 episodes |
| levels | r5 | 4006 samples | 343 µs | 4.2 ms | 85.8 ms | 115.4 ms | 239 samples | 15 samples | 1 episodes |
| nockpt | r1 | 4006 samples | 329 µs | 2.3 ms | 5.2 ms | 7.0 ms | 94 samples | 0 samples | 0 episodes |
| nockpt | r2 | 4006 samples | 312 µs | 1.9 ms | 4.7 ms | 5.7 ms | 66 samples | 0 samples | 0 episodes |
| nockpt | r3 | 4006 samples | 318 µs | 2.3 ms | 5.7 ms | 9.7 ms | 90 samples | 0 samples | 0 episodes |
| nockpt | r4 | 4006 samples | 321 µs | 2.6 ms | 5.7 ms | 12.1 ms | 101 samples | 1 samples | 1 episodes |
| nockpt | r5 | 4006 samples | 316 µs | 1.6 ms | 4.8 ms | 7.7 ms | 60 samples | 0 samples | 0 episodes |
| half | r1 | 4006 samples | 328 µs | 22.4 ms | 90.4 ms | 105.2 ms | 169 samples | 49 samples | 4 episodes |
| half | r2 | 4006 samples | 336 µs | 3.9 ms | 7.8 ms | 61.4 ms | 274 samples | 4 samples | 1 episodes |
| half | r3 | 4006 samples | 327 µs | 6.7 ms | 302.7 ms | 361.1 ms | 120 samples | 39 samples | 3 episodes |
| half | r4 | 4006 samples | 325 µs | 4.3 ms | 133.0 ms | 162.7 ms | 127 samples | 27 samples | 2 episodes |
| half | r5 | 4006 samples | 315 µs | 4.6 ms | 71.1 ms | 86.0 ms | 125 samples | 26 samples | 3 episodes |

#### 1.2 median of runs, and the ratio to the isolated cell
| cell | runs | p50 (median) | p99 (median) | p99 range | p99.9 (median) | p99 against `isolated`'s median | against `iso0`'s median |
|---|---|---|---|---|---|---|---|
| isolated | 5 runs | 328 µs | 1.4 ms | 1.4 ms - 2.4 ms | 4.5 ms | 1.00x | 0.82x |
| iso0 | 5 runs | 326 µs | 1.7 ms | 1.1 ms - 2.5 ms | 4.6 ms | 1.23x | 1.00x |
| levels | 5 runs | 331 µs | 3.2 ms | 1.8 ms - 7.5 ms | 71.1 ms | 2.27x | 1.85x |
| nockpt | 5 runs | 318 µs | 2.3 ms | 1.6 ms - 2.6 ms | 5.2 ms | 1.60x | 1.30x |
| half | 5 runs | 327 µs | 4.6 ms | 3.9 ms - 22.4 ms | 90.4 ms | 3.23x | 2.63x |

#### 1.3 each run against the isolated run interleaved with it (V0's pairing)
| run | `isolated` p99 | `iso0` p99 | `levels` p99 | `levels` / `isolated` | `nockpt` p99 | `nockpt` / `isolated` | `nockpt` / `iso0` | `half` p99 | `half` / `isolated` |
|---|---|---|---|---|---|---|---|---|---|
| r1 | 2.4 ms | 1.1 ms | 3.2 ms | 1.37x | 2.3 ms | 0.97x | 2.03x | 22.4 ms | 9.53x |
| r2 | 1.4 ms | 1.5 ms | 2.4 ms | 1.71x | 1.9 ms | 1.32x | 1.24x | 3.9 ms | 2.77x |
| r3 | 1.4 ms | 2.5 ms | 7.5 ms | 5.27x | 2.3 ms | 1.61x | 0.92x | 6.7 ms | 4.73x |
| r4 | 1.6 ms | 1.7 ms | 1.8 ms | 1.12x | 2.6 ms | 1.59x | 1.47x | 4.3 ms | 2.65x |
| r5 | 1.4 ms | 1.9 ms | 4.2 ms | 2.98x | 1.6 ms | 1.12x | 0.84x | 4.6 ms | 3.28x |

### 2. What the stall samples overlap (M1, M2)

The driver keeps a reader's first and last core only, and the balancer moves readers inside the window, so the core a stalled reader was on at the sample is not known. `moved` counts stall samples of readers whose two cores differ.

| cell | run | stall samples | overlap a checkpoint run | overlap a roll hold | overlap neither | moved | sessions moved out of each read core in the window |
|---|---|---|---|---|---|---|---|
| levels | r1 | 13 samples | 13 samples | 13 samples | 0 samples | 13 samples | core 1: 0, core 2: 1, core 3: 3 |
| levels | r2 | 9 samples | 9 samples | 9 samples | 0 samples | 5 samples | core 1: 0, core 2: 0, core 3: 2 |
| levels | r3 | 38 samples | 38 samples | 38 samples | 0 samples | 19 samples | core 1: 2, core 2: 2, core 3: 2 |
| levels | r4 | 0 samples | 0 samples | 0 samples | 0 samples | 0 samples | core 1: 1, core 2: 1, core 3: 1 |
| levels | r5 | 15 samples | 15 samples | 15 samples | 0 samples | 7 samples | core 1: 2, core 2: 1, core 3: 1 |
| half | r1 | 49 samples | 49 samples | 47 samples | 0 samples | 27 samples | core 1: 2, core 2: 3, core 3: 4 |
| half | r2 | 4 samples | 4 samples | 4 samples | 0 samples | 0 samples | core 1: 0, core 2: 0, core 3: 2 |
| half | r3 | 39 samples | 39 samples | 39 samples | 0 samples | 3 samples | core 1: 0, core 2: 0, core 3: 2 |
| half | r4 | 27 samples | 27 samples | 5 samples | 0 samples | 27 samples | core 1: 3, core 2: 1, core 3: 1 |
| half | r5 | 26 samples | 26 samples | 25 samples | 0 samples | 11 samples | core 1: 2, core 2: 3, core 3: 2 |
| nockpt | r1 | 0 samples | 0 samples | 0 samples | 0 samples | 0 samples | core 1: 0, core 2: 0, core 3: 0 |
| nockpt | r2 | 0 samples | 0 samples | 0 samples | 0 samples | 0 samples | core 1: 0, core 2: 0, core 3: 0 |
| nockpt | r3 | 0 samples | 0 samples | 0 samples | 0 samples | 0 samples | core 1: 0, core 2: 0, core 3: 0 |
| nockpt | r4 | 1 samples | 0 samples | 0 samples | 1 samples | 0 samples | core 1: 0, core 2: 0, core 3: 1 |
| nockpt | r5 | 0 samples | 0 samples | 0 samples | 0 samples | 0 samples | core 1: 0, core 2: 0, core 3: 0 |

#### 2.1 each stall episode against the checkpoint run it falls in (M2)

An episode is the stall samples whose intervals lie within 50 ms of one another; they end together, when the core runs again. The reading is from timing alone, against the one run the episode overlaps (no episode here overlaps two), tested in this order:
- **begin**: the run's `start` (its begin append) took 10 ms or more, and the episode overlaps the interval from the run's start to 5 ms past the end of that `start`;
- **page write**: the run's longest single page write (`page_write_max_us`) is 10 ms or more and no roll hold overlaps the episode. The rule tests the run, not the instant of the write;
- **publish write**: the same with the run's longest publish-phase call (`publish_writeback_max_us`): a batch of page writes, a map write, or an inline gate inside the publish, which that phase's timer hides;
- **run's end**: a roll hold overlaps and the episode ends within 5 ms of the run's end - the recycle at the anchor's commit is the run's last act;
- **inside the run**: a roll hold overlaps, at neither edge - the park's flush, the end append or the inline gate;
- **none**: nothing above.

| cell | run | episode at (from the window's start) | length | samples | run's core | run at | `start` max | longest page write | longest publish write | rolls overlapped (hold) | reading |
|---|---|---|---|---|---|---|---|---|---|---|---|
| levels | r1 | 5383 ms - 5484 ms | 101.6 ms | 13 samples | 3 | 5377 ms - 6998 ms | 106.5 ms | 3.1 ms | 4.3 ms | 244.3 ms | begin |
| levels | r2 | 5390 ms - 5461 ms | 71.3 ms | 9 samples | 3 | 5381 ms - 6986 ms | 78.9 ms | 4.3 ms | 3.0 ms | 246.6 ms | begin |
| levels | r3 | 360 ms - 455 ms | 95.3 ms | 23 samples | 3 | 358 ms - 786 ms | 96.2 ms | 239 µs | 2.4 ms | 336.3 ms | begin |
| levels | r3 | 4113 ms - 4138 ms | 25.9 ms | 2 samples | 2 | 4108 ms - 4314 ms | 29.6 ms | 187 µs | 4.9 ms | 351.8 ms | begin |
| levels | r3 | 9438 ms - 9516 ms | 78.6 ms | 13 samples | 2 | 9108 ms - 9888 ms | 839 µs | 1.4 ms | 3.3 ms | 213.5 ms | inside the run |
| levels | r5 | 375 ms - 491 ms | 116.3 ms | 15 samples | 3 | 374 ms - 817 ms | 116.4 ms | 275 µs | 469 µs | 352.7 ms | begin |
| half | r1 | 375 ms - 470 ms | 95.0 ms | 28 samples | 3 | 375 ms - 798 ms | 93.8 ms | 80 µs | 2.4 ms | 354.8 ms | begin |
| half | r1 | 4125 ms - 4159 ms | 33.5 ms | 5 samples | 2 | 4125 ms - 4287 ms | 33.1 ms | 141 µs | 4.5 ms | 70.2 ms | begin |
| half | r1 | 5680 ms - 5718 ms | 38.2 ms | 2 samples | 3 | 5375 ms - 6059 ms | 550 µs | 2.4 ms | 3.6 ms | none | none |
| half | r1 | 9438 ms - 9543 ms | 105.9 ms | 14 samples | 2 | 9125 ms - 9897 ms | 807 µs | 205 µs | 2.9 ms | 193.4 ms | inside the run |
| half | r2 | 1620 ms - 1682 ms | 61.7 ms | 4 samples | 3 | 279 ms - 1681 ms | 253 µs | 211 µs | 336 µs | 112.5 ms | run's end |
| half | r3 | 343 ms - 441 ms | 98.1 ms | 6 samples | 2 | -876 ms - 470 ms | 226 µs | 269 µs | 434 µs | 374.3 ms | inside the run |
| half | r3 | 5378 ms - 5429 ms | 51.7 ms | 9 samples | 3 | 5374 ms - 6975 ms | 54.6 ms | 1.2 ms | 3.4 ms | 215.3 ms | begin |
| half | r3 | 6465 ms - 6831 ms | 365.7 ms | 24 samples | 3 | 5374 ms - 6975 ms | 54.6 ms | 1.2 ms | 3.4 ms | 726.8 ms | inside the run |
| half | r4 | 388 ms - 464 ms | 76.3 ms | 5 samples | 3 | 378 ms - 783 ms | 84.9 ms | 230 µs | 505 µs | 336.6 ms | begin |
| half | r4 | 5398 ms - 5562 ms | 164.0 ms | 22 samples | 3 | 5379 ms - 6040 ms | 626 µs | 167.0 ms | 2.1 ms | none | page write |
| half | r5 | 383 ms - 469 ms | 86.6 ms | 11 samples | 3 | 376 ms - 798 ms | 92.1 ms | 145 µs | 558 µs | 314.2 ms | begin |
| half | r5 | 4128 ms - 4202 ms | 74.3 ms | 14 samples | 2 | 4127 ms - 4333 ms | 73.5 ms | 207 µs | 4.1 ms | 133.0 ms | begin |
| half | r5 | 5898 ms - 5910 ms | 12.3 ms | 1 samples | 3 | 5376 ms - 5960 ms | 635 µs | 50 µs | 17.2 ms | none | publish write |
| nockpt | r4 | 10013 ms - 10025 ms | 12.1 ms | 1 samples | - | - | - | - | - | none | none |

#### 2.2 the episodes by reading
| cell | reading | episodes | stall samples |
|---|---|---|---|
| levels | begin | 5 episodes | 62 samples |
| levels | inside the run | 1 episodes | 13 samples |
| half | begin | 6 episodes | 72 samples |
| half | inside the run | 3 episodes | 44 samples |
| half | none | 1 episodes | 2 samples |
| half | page write | 1 episodes | 22 samples |
| half | publish write | 1 episodes | 1 samples |
| half | run's end | 1 episodes | 4 samples |
| nockpt | none | 1 episodes | 1 samples |

### 3. The read cores over the window: holds and the stream latch (M1)

`system polls > 10 ms` and the latch waits are window deltas; `longest system poll` is since the server's start. `pool` is the pages resident at the closing read, of 65,536 frames, and the misses in the window.

| cell | run | read cores | system polls > 10 ms (sum) | longest system poll (max) | foreground polls > 10 ms (sum) | stream-latch wait, read cores (sum) | of it roll-blocked | roll-blocked waits | stream-latch wait, core 0 | checkpoint runs on read cores | pool: resident / misses |
|---|---|---|---|---|---|---|---|---|---|---|---|
| isolated | r1 | 0,1,2,3 | 0 polls | 331 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 8 runs | 497 / 0 pages |
| isolated | r2 | 0,1,2,3 | 0 polls | 354 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 8 runs | 497 / 0 pages |
| isolated | r3 | 0,1,2,3 | 0 polls | 370 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 8 runs | 497 / 0 pages |
| isolated | r4 | 0,1,2,3 | 0 polls | 545 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 8 runs | 497 / 0 pages |
| isolated | r5 | 0,1,2,3 | 0 polls | 376 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 8 runs | 497 / 0 pages |
| iso0 | r1 | 0,1,2,3 | 0 polls | 83 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 0 runs | 497 / 0 pages |
| iso0 | r2 | 0,1,2,3 | 0 polls | 24 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 0 runs | 497 / 0 pages |
| iso0 | r3 | 0,1,2,3 | 0 polls | 107 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 0 runs | 497 / 0 pages |
| iso0 | r4 | 0,1,2,3 | 0 polls | 27 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 0 runs | 497 / 0 pages |
| iso0 | r5 | 0,1,2,3 | 0 polls | 21 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 0 runs | 497 / 0 pages |
| levels | r1 | 1,2,3 | 2 polls | 111.7 ms | 0 polls | 219.0 ms | 218.9 ms | 3 waits | 56 µs | 2 runs | 23,080 / 18,488 pages |
| levels | r2 | 1,2,3 | 2 polls | 623.1 ms | 0 polls | 701.5 ms | 701.4 ms | 3 waits | 68 µs | 2 runs | 23,080 / 18,322 pages |
| levels | r3 | 1,2,3 | 4 polls | 96.2 ms | 0 polls | 218.6 ms | 218.4 ms | 5 waits | 54 µs | 6 runs | 22,564 / 17,908 pages |
| levels | r4 | 1,2,3 | 1 polls | 12.7 ms | 0 polls | 19.2 ms | 19.0 ms | 4 waits | 26 µs | 5 runs | 23,080 / 18,701 pages |
| levels | r5 | 1,2,3 | 1 polls | 116.4 ms | 0 polls | 121.3 ms | 121.2 ms | 3 waits | 3.6 ms | 6 runs | 20,227 / 15,637 pages |
| nockpt | r1 | 1,2,3 | 0 polls | 109 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 0 runs | 24,653 / 19,779 pages |
| nockpt | r2 | 1,2,3 | 0 polls | 72 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 0 runs | 24,653 / 19,762 pages |
| nockpt | r3 | 1,2,3 | 0 polls | 19 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 0 runs | 24,642 / 19,800 pages |
| nockpt | r4 | 1,2,3 | 0 polls | 20 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 0 runs | 24,501 / 19,737 pages |
| nockpt | r5 | 1,2,3 | 0 polls | 18 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 0 runs | 24,653 / 19,798 pages |
| half | r1 | 1,2,3 | 4 polls | 109.7 ms | 0 polls | 240.2 ms | 240.1 ms | 5 waits | 43 µs | 6 runs | 22,387 / 17,714 pages |
| half | r2 | 1,2,3 | 1 polls | 62.5 ms | 0 polls | 74.2 ms | 64.5 ms | 2 waits | 23 µs | 1 runs | 21,430 / 17,532 pages |
| half | r3 | 1,2,3 | 3 polls | 372.9 ms | 0 polls | 527.0 ms | 526.9 ms | 3 waits | 66 µs | 2 runs | 22,751 / 17,919 pages |
| half | r4 | 1,2,3 | 3 polls | 167.4 ms | 0 polls | 105.9 ms | 105.8 ms | 5 waits | 72 µs | 6 runs | 22,351 / 17,713 pages |
| half | r5 | 1,2,3 | 3 polls | 92.1 ms | 0 polls | 174.0 ms | 173.8 ms | 4 waits | 5.8 ms | 5 runs | 22,311 / 17,763 pages |

### 4. The log and the rolls over the window (M1's roll-hold distribution, M3, M4)

`live segments` is derived from the log lines, not read from the device: the segment being written at the window's end, less the segment of the lowest redo start among each core's latest `anchor published` line before it (the fold), plus one.

| cell | run | log rate | inserter rate | rolls in window | hold median | hold p90 | hold max | hold total | share of window under a roll hold | checkpoint runs in window | share of window under a checkpoint run | live segments at the end |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| isolated | r1 | 0.0 MB/s | 0 rows/s | 0 rolls | - | - | - | 0 µs | 0 % | 8 runs | 0 % | 1 segments |
| isolated | r2 | 0.0 MB/s | 0 rows/s | 0 rolls | - | - | - | 0 µs | 0 % | 8 runs | 0 % | 1 segments |
| isolated | r3 | 0.0 MB/s | 0 rows/s | 0 rolls | - | - | - | 0 µs | 0 % | 8 runs | 0 % | 1 segments |
| isolated | r4 | 0.0 MB/s | 0 rows/s | 0 rolls | - | - | - | 0 µs | 0 % | 8 runs | 0 % | 1 segments |
| isolated | r5 | 0.0 MB/s | 0 rows/s | 0 rolls | - | - | - | 0 µs | 0 % | 8 runs | 0 % | 1 segments |
| iso0 | r1 | 0.0 MB/s | 0 rows/s | 0 rolls | - | - | - | 0 µs | 0 % | 0 runs | 0 % | 1 segments |
| iso0 | r2 | 0.0 MB/s | 0 rows/s | 0 rolls | - | - | - | 0 µs | 0 % | 0 runs | 0 % | 1 segments |
| iso0 | r3 | 0.0 MB/s | 0 rows/s | 0 rolls | - | - | - | 0 µs | 0 % | 0 runs | 0 % | 1 segments |
| iso0 | r4 | 0.0 MB/s | 0 rows/s | 0 rolls | - | - | - | 0 µs | 0 % | 0 runs | 0 % | 1 segments |
| iso0 | r5 | 0.0 MB/s | 0 rows/s | 0 rolls | - | - | - | 0 µs | 0 % | 0 runs | 0 % | 1 segments |
| levels | r1 | 73.7 MB/s | 164,790 rows/s | 12 rolls | 244.1 ms | 310.6 ms | 746.5 ms | 3303.1 ms | 31 % | 2 runs | 22 % | 14 segments |
| levels | r2 | 73.1 MB/s | 159,955 rows/s | 12 rolls | 251.7 ms | 413.3 ms | 681.2 ms | 3637.8 ms | 34 % | 2 runs | 21 % | 14 segments |
| levels | r3 | 72.9 MB/s | 156,150 rows/s | 11 rolls | 254.0 ms | 345.5 ms | 351.8 ms | 2481.1 ms | 25 % | 8 runs | 41 % | 7 segments |
| levels | r4 | 77.6 MB/s | 169,798 rows/s | 12 rolls | 242.8 ms | 351.9 ms | 353.9 ms | 2909.3 ms | 27 % | 6 runs | 36 % | 12 segments |
| levels | r5 | 63.5 MB/s | 136,332 rows/s | 10 rolls | 59.9 ms | 352.7 ms | 354.2 ms | 1351.4 ms | 14 % | 8 runs | 35 % | 6 segments |
| nockpt | r1 | 80.5 MB/s | 172,528 rows/s | 12 rolls | 240.6 ms | 258.9 ms | 260.4 ms | 2877.3 ms | 29 % | 0 runs | 0 % | 15 segments |
| nockpt | r2 | 80.5 MB/s | 172,426 rows/s | 12 rolls | 239.0 ms | 251.4 ms | 257.2 ms | 2867.9 ms | 29 % | 0 runs | 0 % | 15 segments |
| nockpt | r3 | 80.6 MB/s | 172,712 rows/s | 12 rolls | 233.0 ms | 247.1 ms | 250.0 ms | 2758.9 ms | 28 % | 0 runs | 0 % | 15 segments |
| nockpt | r4 | 80.2 MB/s | 172,090 rows/s | 12 rolls | 239.0 ms | 255.5 ms | 256.4 ms | 2826.5 ms | 28 % | 0 runs | 0 % | 15 segments |
| nockpt | r5 | 80.6 MB/s | 172,618 rows/s | 12 rolls | 247.4 ms | 253.9 ms | 257.0 ms | 2942.9 ms | 29 % | 0 runs | 0 % | 15 segments |
| half | r1 | 72.1 MB/s | 154,567 rows/s | 11 rolls | 229.1 ms | 334.9 ms | 354.8 ms | 2200.0 ms | 22 % | 8 runs | 44 % | 7 segments |
| half | r2 | 63.7 MB/s | 153,563 rows/s | 11 rolls | 207.7 ms | 755.1 ms | 1269.4 ms | 3399.7 ms | 22 % | 2 runs | 34 % | 13 segments |
| half | r3 | 72.9 MB/s | 156,324 rows/s | 11 rolls | 214.6 ms | 374.3 ms | 726.8 ms | 2643.0 ms | 26 % | 2 runs | 21 % | 14 segments |
| half | r4 | 70.9 MB/s | 155,845 rows/s | 11 rolls | 256.8 ms | 331.6 ms | 336.6 ms | 2206.9 ms | 22 % | 8 runs | 46 % | 7 segments |
| half | r5 | 72.3 MB/s | 155,051 rows/s | 11 rolls | 240.7 ms | 316.3 ms | 375.3 ms | 2640.9 ms | 26 % | 7 runs | 32 % | 8 segments |

#### 4.1 roll holds pooled over the runs of a cell
| cell | rolls | hold p50 | hold p90 | hold p99 | hold max | holds over 300 ms | sync-gate share of the hold |
|---|---|---|---|---|---|---|---|
| levels | 57 rolls | 246.6 ms | 352.7 ms | 746.5 ms | 746.5 ms | 14 rolls | 95.4 % |
| half | 55 rolls | 215.3 ms | 354.8 ms | 1269.4 ms | 1269.4 ms | 13 rolls | 94.8 % |
| nockpt | 60 rolls | 240.2 ms | 253.9 ms | 260.4 ms | 260.4 ms | 0 rolls | 99.0 % |

### 5. Each checkpoint run of the default arm against the rolls it overlaps (M2)

`on-reactor, timed` is the run line's `start`, `log_gate_*`, `page_write`, `map_write` and `publish_writeback` sums: the phases the line times on the reactor thread. `parked` is `log_park_us + data_park_us`. `rest` is the wall time less both: yields, and whatever the line does not time - the park's flush, the end append and the recycle among it.

| run | core | wall | pages | steps | gate parks | start (begin append) max | inline gate n / sum | log park | data park | on-reactor, timed | rest | rolls overlapped | overlap with roll holds | longest roll overlapped |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| r1 | 2 | 1486.8 ms | 2625 pages | 52 steps | 33 parks | 305 µs | 0 / 0 µs | 1310.3 ms | 138.3 ms | 23.0 ms | 15.2 ms | 2 rolls | 296.5 ms | 273.9 ms |
| r1 | 3 | 1621.6 ms | 10299 pages | 179 steps | 39 parks | 106.5 ms | 2 / 3.0 ms | 1208.1 ms | 117.6 ms | 158.1 ms | 137.8 ms | 2 rolls | 852.9 ms | 746.5 ms |
| r2 | 2 | 1366.3 ms | 2640 pages | 53 steps | 14 parks | 241 µs | 0 / 0 µs | 621.7 ms | 652.0 ms | 18.3 ms | 74.3 ms | 2 rolls | 686.6 ms | 413.3 ms |
| r2 | 3 | 1604.7 ms | 10610 pages | 185 steps | 39 parks | 78.9 ms | 2 / 3.0 ms | 700.1 ms | 123.7 ms | 131.8 ms | 649.0 ms | 2 rolls | 760.0 ms | 681.2 ms |
| r3 | 3 | 428.6 ms | 1603 pages | 43 steps | 33 parks | 96.2 ms | 2 / 2.2 ms | 270.8 ms | 38.8 ms | 109.5 ms | 9.6 ms | 1 rolls | 96.2 ms | 336.3 ms |
| r3 | 0 | 900.8 ms | 1689 pages | 45 steps | 51 parks | 267 µs | 1 / 7.9 ms | 770.4 ms | 43.4 ms | 30.6 ms | 56.4 ms | 1 rolls | 109.2 ms | 109.2 ms |
| r3 | 1 | 666.5 ms | 1532 pages | 54 steps | 47 parks | 489 µs | 2 / 2.6 ms | 600.6 ms | 41.7 ms | 12.9 ms | 11.3 ms | 1 rolls | 310.9 ms | 310.9 ms |
| r3 | 2 | 205.9 ms | 1040 pages | 40 steps | 40 parks | 29.6 ms | 2 / 3.4 ms | 148.4 ms | 7.3 ms | 43.4 ms | 6.8 ms | 1 rolls | 29.6 ms | 351.8 ms |
| r3 | 3 | 700.8 ms | 2365 pages | 53 steps | 45 parks | 629 µs | 2 / 2.8 ms | 618.1 ms | 38.1 ms | 19.1 ms | 25.4 ms | 1 rolls | 24.3 ms | 24.3 ms |
| r3 | 0 | 350.6 ms | 1708 pages | 59 steps | 41 parks | 831 µs | 2 / 4.5 ms | 261.6 ms | 37.4 ms | 14.4 ms | 37.3 ms | 0 rolls | 0 µs | 0 µs |
| r3 | 1 | 102.5 ms | 1002 pages | 48 steps | 49 parks | 785 µs | 4 / 5.1 ms | 75.0 ms | 6.6 ms | 14.8 ms | 6.2 ms | 0 rolls | 0 µs | 0 µs |
| r3 | 2 | 780.6 ms | 2494 pages | 54 steps | 48 parks | 839 µs | 0 / 0 µs | 629.8 ms | 38.6 ms | 23.7 ms | 88.5 ms | 1 rolls | 213.5 ms | 213.5 ms |
| r4 | 2 | 1201.6 ms | 2632 pages | 56 steps | 20 parks | 234 µs | 0 / 0 µs | 677.6 ms | 494.6 ms | 18.4 ms | 11.0 ms | 1 rolls | 269.0 ms | 269.0 ms |
| r4 | 3 | 77.4 ms | 311 pages | 37 steps | 49 parks | 242 µs | 4 / 4.5 ms | 57.2 ms | 6.5 ms | 9.4 ms | 4.2 ms | 0 rolls | 0 µs | 0 µs |
| r4 | 0 | 872.7 ms | 2813 pages | 49 steps | 27 parks | 306 µs | 0 / 0 µs | 717.0 ms | 54.2 ms | 15.7 ms | 85.8 ms | 1 rolls | 130.2 ms | 130.2 ms |
| r4 | 1 | 655.0 ms | 1550 pages | 57 steps | 41 parks | 464 µs | 4 / 5.5 ms | 581.9 ms | 44.7 ms | 15.3 ms | 13.0 ms | 1 rolls | 318.3 ms | 318.3 ms |
| r4 | 2 | 193.3 ms | 993 pages | 44 steps | 45 parks | 12.7 ms | 2 / 2.7 ms | 155.0 ms | 7.3 ms | 24.6 ms | 6.4 ms | 1 rolls | 12.6 ms | 351.9 ms |
| r4 | 3 | 1516.2 ms | 2457 pages | 55 steps | 25 parks | 682 µs | 2 / 2.8 ms | 1408.1 ms | 79.1 ms | 15.8 ms | 13.2 ms | 2 rolls | 605.8 ms | 353.9 ms |
| r5 | 3 | 443.0 ms | 1603 pages | 43 steps | 26 parks | 116.4 ms | 2 / 2.2 ms | 276.1 ms | 31.1 ms | 127.6 ms | 8.2 ms | 1 rolls | 116.3 ms | 352.7 ms |
| r5 | 0 | 798.8 ms | 1753 pages | 44 steps | 33 parks | 250 µs | 1 / 2.0 ms | 623.3 ms | 11.4 ms | 15.9 ms | 148.2 ms | 1 rolls | 248.3 ms | 248.3 ms |
| r5 | 1 | 592.6 ms | 2234 pages | 57 steps | 29 parks | 481 µs | 2 / 2.9 ms | 519.5 ms | 42.2 ms | 15.0 ms | 15.8 ms | 1 rolls | 257.7 ms | 257.7 ms |
| r5 | 2 | 191.8 ms | 1179 pages | 27 steps | 17 parks | 583 µs | 0 / 0 µs | 161.5 ms | 9.8 ms | 11.1 ms | 9.4 ms | 1 rolls | 26.0 ms | 26.0 ms |
| r5 | 3 | 73.9 ms | 1738 pages | 38 steps | 27 parks | 878 µs | 0 / 0 µs | 47.0 ms | 6.2 ms | 13.4 ms | 7.4 ms | 0 rolls | 0 µs | 0 µs |
| r5 | 0 | 790.4 ms | 2046 pages | 62 steps | 52 parks | 811 µs | 1 / 2.2 ms | 680.3 ms | 18.9 ms | 21.4 ms | 69.8 ms | 0 rolls | 0 µs | 0 µs |
| r5 | 1 | 51.4 ms | 1113 pages | 22 steps | 16 parks | 957 µs | 0 / 0 µs | 30.7 ms | 5.0 ms | 6.3 ms | 9.4 ms | 0 rolls | 0 µs | 0 µs |
| r5 | 2 | 600.0 ms | 1810 pages | 39 steps | 15 parks | 1.1 ms | 0 / 0 µs | 531.6 ms | 44.3 ms | 12.3 ms | 11.8 ms | 0 rolls | 0 µs | 0 µs |

#### 5.1 a read core's stream-latch wait against the latch sites its runs time

The begin append is inside `start` and the inline gate is `log_gate_*`, so their sums bound the latch wait those two sites took; what is left of the core's stream-latch wait was taken at a site the run line does not time, or by another taker. An inline gate taken inside the publish is hidden in `publish_writeback`, so the last column takes that phase out too.

| cell | run | core | stream-latch wait in window | roll-blocked part | checkpoint runs overlapping the window | their `start` sum | their inline-gate sum | not at a timed latch site (at least) | the same, less their publish writeback |
|---|---|---|---|---|---|---|---|---|---|
| levels | r1 | 2 | 2.1 ms | 2.1 ms | 1 runs | 305 µs | 0 µs | 1.8 ms | 0 µs |
| levels | r1 | 3 | 216.9 ms | 216.8 ms | 1 runs | 106.5 ms | 3.0 ms | 107.4 ms | 94.5 ms |
| levels | r2 | 2 | 1.2 ms | 1.2 ms | 1 runs | 241 µs | 0 µs | 970 µs | 0 µs |
| levels | r2 | 3 | 700.3 ms | 700.2 ms | 1 runs | 78.9 ms | 3.0 ms | 618.4 ms | 606.8 ms |
| levels | r3 | 1 | 2.3 ms | 2.2 ms | 2 runs | 1.3 ms | 7.6 ms | 0 µs | 0 µs |
| levels | r3 | 2 | 106.6 ms | 106.5 ms | 2 runs | 30.4 ms | 3.4 ms | 72.7 ms | 56.5 ms |
| levels | r3 | 3 | 109.7 ms | 109.7 ms | 2 runs | 96.9 ms | 4.9 ms | 7.9 ms | 0 µs |
| levels | r4 | 1 | 2.2 ms | 2.2 ms | 1 runs | 464 µs | 5.5 ms | 0 µs | 0 µs |
| levels | r4 | 2 | 11.9 ms | 11.9 ms | 2 runs | 12.9 ms | 2.7 ms | 0 µs | 0 µs |
| levels | r4 | 3 | 5.0 ms | 4.9 ms | 2 runs | 924 µs | 7.3 ms | 0 µs | 0 µs |
| levels | r5 | 1 | 2.6 ms | 2.6 ms | 2 runs | 1.4 ms | 2.9 ms | 0 µs | 0 µs |
| levels | r5 | 2 | 2.9 ms | 2.8 ms | 2 runs | 1.6 ms | 0 µs | 1.2 ms | 0 µs |
| levels | r5 | 3 | 115.8 ms | 115.8 ms | 2 runs | 117.2 ms | 2.2 ms | 0 µs | 0 µs |
| half | r1 | 1 | 2.0 ms | 2.0 ms | 2 runs | 1.2 ms | 8.0 ms | 0 µs | 0 µs |
| half | r1 | 2 | 142.4 ms | 142.4 ms | 2 runs | 34.0 ms | 8.2 ms | 100.3 ms | 84.6 ms |
| half | r1 | 3 | 95.8 ms | 95.7 ms | 2 runs | 94.3 ms | 5.2 ms | 0 µs | 0 µs |
| half | r2 | 3 | 74.2 ms | 64.5 ms | 1 runs | 253 µs | 2.1 ms | 71.9 ms | 68.1 ms |
| half | r3 | 2 | 101.2 ms | 101.2 ms | 1 runs | 226 µs | 0 µs | 100.9 ms | 98.7 ms |
| half | r3 | 3 | 425.8 ms | 425.8 ms | 1 runs | 54.6 ms | 3.2 ms | 368.0 ms | 353.0 ms |
| half | r4 | 1 | 2.0 ms | 2.0 ms | 2 runs | 1.2 ms | 11.3 ms | 0 µs | 0 µs |
| half | r4 | 2 | 16.6 ms | 16.5 ms | 2 runs | 15.4 ms | 5.6 ms | 0 µs | 0 µs |
| half | r4 | 3 | 87.4 ms | 87.3 ms | 2 runs | 85.5 ms | 7.2 ms | 0 µs | 0 µs |
| half | r5 | 1 | 2.2 ms | 2.1 ms | 2 runs | 1.3 ms | 8.5 ms | 0 µs | 0 µs |
| half | r5 | 2 | 80.1 ms | 80.0 ms | 1 runs | 73.5 ms | 4.7 ms | 1.8 ms | 0 µs |
| half | r5 | 3 | 91.7 ms | 91.7 ms | 2 runs | 92.8 ms | 4.6 ms | 0 µs | 0 µs |

#### 5.2 the same, summed over the read cores and the runs of a cell (the last two columns core by core)
| cell | stream-latch wait, read cores | roll-blocked part | `start` sum of their checkpoint runs | their inline-gate sum | not at a timed latch site (at least) | the same, less their publish writeback |
|---|---|---|---|---|---|---|
| levels | 1279.5 ms | 1278.9 ms | 449.1 ms | 42.5 ms | 810.4 ms | 757.7 ms |
| half | 1121.3 ms | 1111.0 ms | 454.3 ms | 68.4 ms | 643.0 ms | 604.4 ms |
| nockpt | 0 µs | 0 µs | 0 µs | 0 µs | 0 µs | 0 µs |
