### 0. The host at each cell (bench/README.md rule 4), as the driver recorded it

`competing` counts the processes matching `cc1plus|cmake --build|ctest` at the cell's start and end; a bare `kds_tests` is not in that pattern, and the run script's gate tests for it by name. `driver wait` is the driver's own wait for a 1-minute load under 1.5, which gives up after 120 s and starts.

| cell | run | driver wait | load at start | load at end | competing at start | competing at end | `cz_mode` on its run lines |
|---|---|---|---|---|---|---|---|
| isolated | r1 | 20 s | 1.39 | 1.15 | 0 processes | 0 processes | [0] |
| isolated | r2 | 20 s | 1.49 | 1.16 | 0 processes | 0 processes | [0] |
| isolated | r3 | 120 s | 7.41 | 7.54 | 13 processes | 12 processes | [0] |
| isolated | r4 | 120 s | 3.38 | 2.78 | 3 processes | 0 processes | [0] |
| isolated | r5 | 0 s | 1.50 | 1.14 | 0 processes | 0 processes | [0] |
| levels | r1 | 0 s | 1.15 | 1.42 | 0 processes | 0 processes | [0] |
| levels | r2 | 10 s | 1.39 | 2.25 | 0 processes | 0 processes | [0] |
| levels | r3 | 0 s | 1.25 | 1.43 | 0 processes | 0 processes | [0] |
| levels | r4 | 20 s | 1.40 | 2.10 | 0 processes | 0 processes | [0] |
| levels | r5 | 0 s | 1.14 | 1.56 | 0 processes | 0 processes | [0] |
| c1 | r1 | 0 s | 1.42 | 1.75 | 0 processes | 0 processes | [1] |
| c1 | r2 | 0 s | 1.16 | 1.82 | 0 processes | 0 processes | [1] |
| c1 | r3 | 0 s | 1.43 | 1.98 | 0 processes | 0 processes | [1] |
| c1 | r4 | 15 s | 1.50 | 1.60 | 0 processes | 0 processes | [1] |
| c1 | r5 | 5 s | 1.43 | 1.66 | 0 processes | 0 processes | [1] |
| c2 | r1 | 15 s | 1.44 | 1.33 | 0 processes | 0 processes | [2] |
| c2 | r2 | 15 s | 1.41 | 1.74 | 0 processes | 0 processes | [2] |
| c2 | r3 | 25 s | 1.40 | 1.69 | 0 processes | 0 processes | [2] |
| c2 | r4 | 5 s | 1.48 | 1.50 | 0 processes | 0 processes | [2] |
| c2 | r5 | 85 s | 1.43 | 1.48 | 0 processes | 4 processes | [2] |
| c1c2 | r1 | 0 s | 1.33 | 3.03 | 0 processes | 12 processes | [3] |
| c1c2 | r2 | 15 s | 1.42 | 1.64 | 0 processes | 0 processes | [3] |
| c1c2 | r3 | 10 s | 1.43 | 1.25 | 0 processes | 0 processes | [3] |
| c1c2 | r4 | 20 s | 1.50 | 1.63 | 0 processes | 7 processes | [3] |
| c1c2 | r5 | 0 s | 1.44 | 2.10 | 3 processes | 0 processes | [3] |

### 1. Reader schedule latency

#### 1.1 per run
| cell | run | samples | p50 | p99 | p99.9 | max | slow samples (>= 1 ms) | stall samples (>= 10 ms) | stall episodes | errors |
|---|---|---|---|---|---|---|---|---|---|---|
| isolated | r1 | 4006 samples | 323 µs | 1.5 ms | 4.0 ms | 6.0 ms | 68 samples | 0 samples | 0 episodes | 0 |
| isolated | r2 | 4006 samples | 324 µs | 1.1 ms | 4.7 ms | 6.7 ms | 42 samples | 0 samples | 0 episodes | 0 |
| isolated | r3 | 4006 samples | 338 µs | 1.7 ms | 4.5 ms | 8.7 ms | 62 samples | 0 samples | 0 episodes | 0 |
| isolated | r4 | 4006 samples | 322 µs | 2.9 ms | 5.0 ms | 6.8 ms | 105 samples | 0 samples | 0 episodes | 0 |
| isolated | r5 | 4006 samples | 318 µs | 1.2 ms | 4.0 ms | 7.0 ms | 47 samples | 0 samples | 0 episodes | 0 |
| levels | r1 | 4006 samples | 324 µs | 2.3 ms | 39.0 ms | 55.1 ms | 85 samples | 12 samples | 1 episodes | 0 |
| levels | r2 | 4006 samples | 327 µs | 7.7 ms | 151.5 ms | 205.2 ms | 213 samples | 38 samples | 4 episodes | 0 |
| levels | r3 | 4006 samples | 323 µs | 5.1 ms | 68.8 ms | 81.8 ms | 187 samples | 35 samples | 4 episodes | 0 |
| levels | r4 | 4006 samples | 322 µs | 4.4 ms | 138.8 ms | 154.7 ms | 91 samples | 33 samples | 1 episodes | 0 |
| levels | r5 | 4006 samples | 318 µs | 102.2 ms | 376.4 ms | 429.5 ms | 120 samples | 56 samples | 3 episodes | 0 |
| c1 | r1 | 4006 samples | 317 µs | 1.4 ms | 4.4 ms | 6.1 ms | 59 samples | 0 samples | 0 episodes | 0 |
| c1 | r2 | 4006 samples | 321 µs | 1.3 ms | 4.2 ms | 6.6 ms | 56 samples | 0 samples | 0 episodes | 0 |
| c1 | r3 | 4006 samples | 327 µs | 3.5 ms | 5.4 ms | 7.5 ms | 153 samples | 0 samples | 0 episodes | 0 |
| c1 | r4 | 4006 samples | 326 µs | 2.4 ms | 4.9 ms | 7.2 ms | 78 samples | 0 samples | 0 episodes | 0 |
| c1 | r5 | 4006 samples | 320 µs | 1.3 ms | 3.8 ms | 6.8 ms | 51 samples | 0 samples | 0 episodes | 0 |
| c2 | r1 | 4006 samples | 331 µs | 2.1 ms | 5.3 ms | 9.7 ms | 82 samples | 0 samples | 0 episodes | 0 |
| c2 | r2 | 4006 samples | 321 µs | 3.2 ms | 6.8 ms | 18.8 ms | 107 samples | 1 samples | 1 episodes | 0 |
| c2 | r3 | 4006 samples | 331 µs | 3.8 ms | 5.9 ms | 6.6 ms | 191 samples | 0 samples | 0 episodes | 0 |
| c2 | r4 | 4006 samples | 320 µs | 2.2 ms | 5.6 ms | 10.0 ms | 80 samples | 0 samples | 0 episodes | 0 |
| c2 | r5 | 4006 samples | 320 µs | 19.7 ms | 205.3 ms | 252.3 ms | 143 samples | 45 samples | 3 episodes | 0 |
| c1c2 | r1 | 4006 samples | 349 µs | 3.8 ms | 6.7 ms | 9.1 ms | 224 samples | 0 samples | 0 episodes | 0 |
| c1c2 | r2 | 4006 samples | 318 µs | 2.2 ms | 6.2 ms | 21.5 ms | 91 samples | 1 samples | 1 episodes | 0 |
| c1c2 | r3 | 4006 samples | 327 µs | 2.8 ms | 6.2 ms | 9.5 ms | 140 samples | 0 samples | 0 episodes | 0 |
| c1c2 | r4 | 4006 samples | 328 µs | 3.6 ms | 6.0 ms | 7.4 ms | 187 samples | 0 samples | 0 episodes | 0 |
| c1c2 | r5 | 4006 samples | 320 µs | 3.4 ms | 63.5 ms | 117.8 ms | 142 samples | 8 samples | 1 episodes | 0 |

#### 1.2 median of runs, and the ratio to the isolated cell (CZ-Q1's bar; p99.9 and the stall count beside it, CZ-Q11)
| cell | runs | p50 (median) | p99 (median) | p99 range | p99 against `isolated`'s median | pairs within 2x | p99.9 (median) | p99.9 range | stall samples, all runs | stall samples a run |
|---|---|---|---|---|---|---|---|---|---|---|
| isolated | 5 runs | 323 µs | 1.5 ms | 1.1 ms - 2.9 ms | 1.00x | 5 of 5 | 4.5 ms | 4.0 ms - 5.0 ms | 0 samples | 0, 0, 0, 0, 0 |
| levels | 5 runs | 323 µs | 5.1 ms | 2.3 ms - 102.2 ms | 3.42x | 2 of 5 | 138.8 ms | 39.0 ms - 376.4 ms | 174 samples | 12, 38, 35, 33, 56 |
| c1 | 5 runs | 321 µs | 1.4 ms | 1.3 ms - 3.5 ms | 0.95x | 5 of 5 | 4.4 ms | 3.8 ms - 5.4 ms | 0 samples | 0, 0, 0, 0, 0 |
| c2 | 5 runs | 321 µs | 3.2 ms | 2.1 ms - 19.7 ms | 2.11x | 2 of 5 | 5.9 ms | 5.3 ms - 205.3 ms | 46 samples | 0, 1, 0, 0, 45 |
| c1c2 | 5 runs | 327 µs | 3.4 ms | 2.2 ms - 3.8 ms | 2.29x | 2 of 5 | 6.2 ms | 6.0 ms - 63.5 ms | 9 samples | 0, 1, 0, 0, 8 |

#### 1.3 each run against the isolated run interleaved with it
| run | `isolated` p99 | `levels` p99 | `levels` / `isolated` | `c1` p99 | `c1` / `isolated` | `c2` p99 | `c2` / `isolated` | `c1c2` p99 | `c1c2` / `isolated` |
|---|---|---|---|---|---|---|---|---|---|
| r1 | 1.5 ms | 2.3 ms | 1.55x | 1.4 ms | 0.95x | 2.1 ms | 1.37x | 3.8 ms | 2.55x |
| r2 | 1.1 ms | 7.7 ms | 7.21x | 1.3 ms | 1.26x | 3.2 ms | 2.97x | 2.2 ms | 2.06x |
| r3 | 1.7 ms | 5.1 ms | 2.94x | 3.5 ms | 1.99x | 3.8 ms | 2.16x | 2.8 ms | 1.60x |
| r4 | 2.9 ms | 4.4 ms | 1.54x | 2.4 ms | 0.85x | 2.2 ms | 0.78x | 3.6 ms | 1.25x |
| r5 | 1.2 ms | 102.2 ms | 86.34x | 1.3 ms | 1.07x | 19.7 ms | 16.64x | 3.4 ms | 2.89x |

#### 1.4 the same against the isolated cells no other session's build reached

An isolated cell is taken as clean when no `cc1plus|cmake --build|ctest` process was seen at either end and the load at its start was under 2: r1, r2, r5, whose p99 median is 1.2 ms. A loaded cell is marked `build` when such a process was seen at either of its ends.

| cell | p99 (median) | against the clean isolated median | runs within 2x of it | each run's p99 (ratio; `build` where one was seen) |
|---|---|---|---|---|
| levels | 5.1 ms | 4.33x | 1 of 5 | r1 2.3 ms (1.96x); r2 7.7 ms (6.49x); r3 5.1 ms (4.33x); r4 4.4 ms (3.71x); r5 102.2 ms (86.34x) |
| c1 | 1.4 ms | 1.21x | 3 of 5 | r1 1.4 ms (1.21x); r2 1.3 ms (1.14x); r3 3.5 ms (2.92x); r4 2.4 ms (2.04x); r5 1.3 ms (1.07x) |
| c2 | 3.2 ms | 2.67x | 2 of 5 | r1 2.1 ms (1.73x); r2 3.2 ms (2.67x); r3 3.8 ms (3.17x); r4 2.2 ms (1.89x); r5 19.7 ms (16.64x, build) |
| c1c2 | 3.4 ms | 2.89x | 1 of 5 | r1 3.8 ms (3.22x, build); r2 2.2 ms (1.85x); r3 2.8 ms (2.36x); r4 3.6 ms (3.02x, build); r5 3.4 ms (2.89x, build) |

### 2. What the stall samples overlap, and the cores they were answered on

`cores` is the core a SHOW META sent right after each stall sample was answered on (`--stall-cores`): the session's core at that head, which the placer may have moved since the sample.

| cell | run | stall samples | overlap a checkpoint run | overlap a roll hold | overlap neither | cores | sessions moved out of each read core in the window |
|---|---|---|---|---|---|---|---|
| levels | r1 | 12 samples | 12 samples | 12 samples | 0 samples | core 2: 4, core 3: 8 | core 1: 1, core 2: 0, core 3: 3 |
| levels | r2 | 38 samples | 38 samples | 22 samples | 0 samples | core 1: 5, core 2: 27, core 3: 6 | core 1: 2, core 2: 1, core 3: 2 |
| levels | r3 | 35 samples | 35 samples | 35 samples | 0 samples | core 1: 3, core 2: 19, core 3: 13 | core 1: 2, core 2: 3, core 3: 3 |
| levels | r4 | 33 samples | 33 samples | 32 samples | 0 samples | core 2: 11, core 3: 22 | core 1: 0, core 2: 0, core 3: 3 |
| levels | r5 | 56 samples | 56 samples | 53 samples | 0 samples | core 2: 22, core 3: 34 | core 1: 0, core 2: 0, core 3: 0 |
| c1 | r1 | 0 samples | 0 samples | 0 samples | 0 samples | - | core 1: 0, core 2: 0, core 3: 0 |
| c1 | r2 | 0 samples | 0 samples | 0 samples | 0 samples | - | core 1: 0, core 2: 1, core 3: 2 |
| c1 | r3 | 0 samples | 0 samples | 0 samples | 0 samples | - | core 1: 0, core 2: 0, core 3: 1 |
| c1 | r4 | 0 samples | 0 samples | 0 samples | 0 samples | - | core 1: 0, core 2: 0, core 3: 0 |
| c1 | r5 | 0 samples | 0 samples | 0 samples | 0 samples | - | core 1: 0, core 2: 0, core 3: 2 |
| c2 | r1 | 0 samples | 0 samples | 0 samples | 0 samples | - | core 1: 4, core 2: 4, core 3: 5 |
| c2 | r2 | 1 samples | 1 samples | 1 samples | 0 samples | core 2: 1 | core 1: 2, core 2: 3, core 3: 2 |
| c2 | r3 | 0 samples | 0 samples | 0 samples | 0 samples | - | core 1: 3, core 2: 3, core 3: 2 |
| c2 | r4 | 0 samples | 0 samples | 0 samples | 0 samples | - | core 1: 3, core 2: 4, core 3: 3 |
| c2 | r5 | 45 samples | 20 samples | 19 samples | 25 samples | core 1: 29, core 2: 16 | core 1: 2, core 2: 3, core 3: 2 |
| c1c2 | r1 | 0 samples | 0 samples | 0 samples | 0 samples | - | core 1: 4, core 2: 1, core 3: 0 |
| c1c2 | r2 | 1 samples | 1 samples | 0 samples | 0 samples | core 2: 1 | core 1: 2, core 2: 4, core 3: 4 |
| c1c2 | r3 | 0 samples | 0 samples | 0 samples | 0 samples | - | core 1: 4, core 2: 3, core 3: 3 |
| c1c2 | r4 | 0 samples | 0 samples | 0 samples | 0 samples | - | core 1: 3, core 2: 4, core 3: 4 |
| c1c2 | r5 | 8 samples | 8 samples | 8 samples | 0 samples | core 2: 8 | core 1: 2, core 2: 4, core 3: 4 |

#### 2.1 each stall episode against the checkpoint runs it overlaps

An episode is the stall samples whose intervals lie within 50 ms of one another. The reading is CZ-S0's rule, from timing, against the overlapped run on the episode's most frequent core when there is one, else the first overlapped run, tested in this order: **begin** (the run's `start` took 10 ms or more and the episode overlaps it, to 5 ms past its end); **page write** (the run's longest page write is 10 ms or more and no roll hold overlaps); **publish write** (the same with its longest publish-phase call); **run's end** (a roll hold overlaps and the episode ends within 5 ms of the run's end); **inside the run** (a roll hold overlaps, at neither edge); **none**. `latch at sites` is that run's stream-latch wait summed over the spike's sites, and `longest site call` the longest single call with its site.

| cell | run | episode at (from the window's start) | length | samples | samples' cores | runs overlapped (cores) | run read | run at | `start` max | longest page write | longest publish write | latch at sites | longest site call | rolls overlapped (hold) | reading |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| levels | r1 | 5373 ms - 5434 ms | 61.3 ms | 12 samples | core 2: 4, core 3: 8 | 3 | 3 | 5370 ms - 6992 ms | 57.0 ms | 3.3 ms | 5.2 ms | 63.4 ms | 55.7 ms (begin) | 244.8 ms | begin |
| levels | r2 | 368 ms - 464 ms | 96.2 ms | 6 samples | core 3: 6 | 3 | 3 | 366 ms - 1006 ms | 89.7 ms | 184 µs | 464 µs | 89.2 ms | 89.2 ms (begin) | 350.2 ms | begin |
| levels | r2 | 3213 ms - 3235 ms | 22.8 ms | 5 samples | core 1: 5 | 1 | 1 | 2866 ms - 3591 ms | 472 µs | 45 µs | 366 µs | 22.5 ms | 22.5 ms (flush) | 316.5 ms | inside the run |
| levels | r2 | 4123 ms - 4215 ms | 92.0 ms | 12 samples | core 2: 12 | 2 | 2 | 4116 ms - 4394 ms | 82.0 ms | 107 µs | 5.1 ms | 81.3 ms | 81.3 ms (begin) | 313.7 ms | begin |
| levels | r2 | 5428 ms - 5653 ms | 225.2 ms | 15 samples | core 2: 15 | 3 | 3 | 5366 ms - 6063 ms | 591 µs | 104 µs | 4.1 ms | 2.1 ms | 208.4 ms (flush) | none | none |
| levels | r3 | 380 ms - 473 ms | 92.6 ms | 18 samples | core 2: 6, core 3: 12 | 3 | 3 | 375 ms - 796 ms | 86.8 ms | 245 µs | 419 µs | 86.2 ms | 86.2 ms (begin) | 322.4 ms | begin |
| levels | r3 | 4125 ms - 4158 ms | 32.6 ms | 5 samples | core 1: 2, core 2: 3 | 2 | 2 | 4125 ms - 4346 ms | 30.4 ms | 118 µs | 5.4 ms | 29.7 ms | 29.7 ms (begin) | 103.8 ms | begin |
| levels | r3 | 5963 ms - 5983 ms | 19.9 ms | 2 samples | core 1: 1, core 3: 1 | 3 | 3 | 5375 ms - 6072 ms | 666 µs | 1.1 ms | 28.7 ms | 0 µs | 1.4 ms (flush) | 55.7 ms | inside the run |
| levels | r3 | 9455 ms - 9534 ms | 78.6 ms | 10 samples | core 2: 10 | 2 | 2 | 9125 ms - 9888 ms | 817 µs | 230 µs | 3.6 ms | 71.4 ms | 71.4 ms (flush) | 228.7 ms | inside the run |
| levels | r4 | 5378 ms - 5548 ms | 170.0 ms | 33 samples | core 2: 11, core 3: 22 | 3 | 3 | 5372 ms - 7050 ms | 159.2 ms | 5.9 ms | 4.3 ms | 160.1 ms | 158.1 ms (begin) | 250.3 ms | begin |
| levels | r5 | 125 ms - 460 ms | 334.6 ms | 22 samples | core 2: 22 | 2 | 2 | -885 ms - 460 ms | 237 µs | 198 µs | 451 µs | 383.5 ms | 317.1 ms (flush) | 379.2 ms | run's end |
| levels | r5 | 5373 ms - 5424 ms | 51.7 ms | 3 samples | core 3: 3 | 3 | 3 | 5364 ms - 7033 ms | 57.5 ms | 2.6 ms | 3.5 ms | 489.3 ms | 433.3 ms (flush) | 247.1 ms | begin |
| levels | r5 | 6393 ms - 6863 ms | 470.5 ms | 31 samples | core 3: 31 | 3 | 3 | 5364 ms - 7033 ms | 57.5 ms | 2.6 ms | 3.5 ms | 489.3 ms | 433.3 ms (flush) | 794.3 ms | inside the run |
| c2 | r2 | 2958 ms - 2976 ms | 18.8 ms | 1 samples | core 2: 1 | 1 | 1 | 2878 ms - 3068 ms | 693 µs | 183 µs | 5.3 ms | 19.3 ms | 19.3 ms (flush) | 21.5 ms | inside the run |
| c2 | r5 | 2975 ms - 2988 ms | 13.3 ms | 2 samples | core 1: 2 | 1 | 1 | 2872 ms - 4264 ms | 678 µs | 1.3 ms | 746 µs | 268.3 ms | 253.8 ms (flush) | 18.2 ms | inside the run |
| c2 | r5 | 3998 ms - 4281 ms | 283.4 ms | 19 samples | core 1: 19 | 1 | 1 | 2872 ms - 4264 ms | 678 µs | 1.3 ms | 746 µs | 268.3 ms | 253.8 ms (flush) | 618.9 ms | inside the run |
| c2 | r5 | 9745 ms - 9876 ms | 130.8 ms | 24 samples | core 1: 8, core 2: 16 | none | - | - | - | - | - | - | - | none | none |
| c1c2 | r2 | 9345 ms - 9367 ms | 21.5 ms | 1 samples | core 2: 1 | 2 | 2 | 9120 ms - 9404 ms | 1.4 ms | 201 µs | 22.3 ms | 0 µs | 16.1 ms (gate_publish) | none | publish write |
| c1c2 | r5 | 678 ms - 805 ms | 127.0 ms | 8 samples | core 2: 8 | 3 | 3 | 365 ms - 992 ms | 381 µs | 174 µs | 5.8 ms | 0 µs | 2.9 ms (gate_publish) | 119.1 ms | inside the run |

#### 2.2 the episodes by reading
| cell | reading | episodes | stall samples |
|---|---|---|---|
| levels | begin | 7 episodes | 89 samples |
| levels | inside the run | 4 episodes | 48 samples |
| levels | none | 1 episodes | 15 samples |
| levels | run's end | 1 episodes | 22 samples |
| c2 | inside the run | 3 episodes | 22 samples |
| c2 | none | 1 episodes | 24 samples |
| c1c2 | inside the run | 1 episodes | 8 samples |
| c1c2 | publish write | 1 episodes | 1 samples |

### 3. The read cores over the window: holds and the stream latch

`system polls > 10 ms` and the latch waits are window deltas; `longest system poll` is since the server's start.

| cell | run | read cores | system polls > 10 ms (sum) | longest system poll (max) | foreground polls > 10 ms (sum) | stream-latch wait, read cores (sum) | of it roll-blocked | roll-blocked waits | stream-latch wait, core 0 | checkpoint runs on read cores |
|---|---|---|---|---|---|---|---|---|---|---|
| isolated | r1 | 0,1,2,3 | 0 polls | 512 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 8 runs |
| isolated | r2 | 0,1,2,3 | 0 polls | 600 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 8 runs |
| isolated | r3 | 0,1,2,3 | 0 polls | 1.0 ms | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 8 runs |
| isolated | r4 | 0,1,2,3 | 0 polls | 408 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 8 runs |
| isolated | r5 | 0,1,2,3 | 0 polls | 353 µs | 0 polls | 0 µs | 0 µs | 0 waits | 0 µs | 8 runs |
| levels | r1 | 1,2,3 | 1 polls | 57.0 ms | 0 polls | 65.3 ms | 65.2 ms | 3 waits | 48 µs | 2 runs |
| levels | r2 | 1,2,3 | 4 polls | 208.4 ms | 0 polls | 195.0 ms | 195.0 ms | 4 waits | 44 µs | 4 runs |
| levels | r3 | 1,2,3 | 4 polls | 86.8 ms | 0 polls | 189.2 ms | 189.1 ms | 4 waits | 19 µs | 6 runs |
| levels | r4 | 1,2,3 | 2 polls | 317.3 ms | 0 polls | 476.9 ms | 476.8 ms | 3 waits | 35 µs | 2 runs |
| levels | r5 | 1,2,3 | 3 polls | 433.8 ms | 0 polls | 806.4 ms | 806.3 ms | 3 waits | 45 µs | 2 runs |
| c1 | r1 | 1,2,3 | 0 polls | 4.3 ms | 0 polls | 0 µs | 0 µs | 0 waits | 46 µs | 3 runs |
| c1 | r2 | 1,2,3 | 0 polls | 6.3 ms | 0 polls | 0 µs | 0 µs | 0 waits | 51 µs | 2 runs |
| c1 | r3 | 1,2,3 | 0 polls | 4.8 ms | 0 polls | 0 µs | 0 µs | 0 waits | 104 µs | 4 runs |
| c1 | r4 | 1,2,3 | 0 polls | 6.6 ms | 0 polls | 0 µs | 0 µs | 0 waits | 56 µs | 2 runs |
| c1 | r5 | 1,2,3 | 0 polls | 96.8 ms | 0 polls | 0 µs | 0 µs | 0 waits | 39 µs | 2 runs |
| c2 | r1 | 1,2,3 | 1 polls | 10.2 ms | 0 polls | 2.5 ms | 2.5 ms | 1 waits | 71 µs | 6 runs |
| c2 | r2 | 1,2,3 | 2 polls | 19.9 ms | 0 polls | 19.5 ms | 19.5 ms | 2 waits | 68 µs | 6 runs |
| c2 | r3 | 1,2,3 | 0 polls | 9.6 ms | 0 polls | 2.4 ms | 2.4 ms | 1 waits | 100 µs | 6 runs |
| c2 | r4 | 1,2,3 | 0 polls | 9.9 ms | 0 polls | 7.3 ms | 7.2 ms | 2 waits | 76 µs | 6 runs |
| c2 | r5 | 1,2,3 | 4 polls | 253.8 ms | 0 polls | 271.8 ms | 271.1 ms | 3 waits | 689 µs | 2 runs |
| c1c2 | r1 | 1,2,3 | 0 polls | 9.3 ms | 0 polls | 0 µs | 0 µs | 0 waits | 29 µs | 6 runs |
| c1c2 | r2 | 1,2,3 | 1 polls | 22.3 ms | 0 polls | 0 µs | 0 µs | 0 waits | 86 µs | 6 runs |
| c1c2 | r3 | 1,2,3 | 0 polls | 9.7 ms | 0 polls | 0 µs | 0 µs | 0 waits | 83 µs | 6 runs |
| c1c2 | r4 | 1,2,3 | 0 polls | 8.9 ms | 0 polls | 6 µs | 0 µs | 0 waits | 95 µs | 6 runs |
| c1c2 | r5 | 1,2,3 | 0 polls | 9.9 ms | 1 polls | 0 µs | 0 µs | 0 waits | 46 µs | 6 runs |

#### 3.1 the read cores' stream-latch wait, a cell
| cell | sum over the runs | a run, from the least |
|---|---|---|
| isolated | 0 µs | 0 µs, 0 µs, 0 µs, 0 µs, 0 µs |
| levels | 1732.8 ms | 65.3 ms, 189.2 ms, 195.0 ms, 476.9 ms, 806.4 ms |
| c1 | 0 µs | 0 µs, 0 µs, 0 µs, 0 µs, 0 µs |
| c2 | 303.5 ms | 2.4 ms, 2.5 ms, 7.3 ms, 19.5 ms, 271.8 ms |
| c1c2 | 6 µs | 0 µs, 0 µs, 0 µs, 0 µs, 6 µs |

#### 3.2 the read cores' holds, by the scheduler's own counters (independent of the latch counters)

A tried latch charges no wait, so under `c1` the latch counters of §3 and §5 read zero at a tried site by construction. These do not: `in-run holds` are a reactor's unpolled stretches inside a checkpoint run of its core, `foreground polls` its foreground tasks' polls. Counts are window deltas, per read core in core order; a `longest` is since the server's start. `checkpoint pages` is the engine's count of pages the checkpoint wrote in the window against the pages of the runs that logged a line and overlap it: a run in flight at the server's stop is abandoned and logs none.

| cell | run | in-run holds > 10 ms | in-run holds > 100 ms | longest in-run hold | system polls > 100 ms | foreground polls > 10 ms | longest foreground poll | page-latch spin turns | checkpoint pages: engine / logged runs |
|---|---|---|---|---|---|---|---|---|---|
| isolated | r1 | 0, 0, 0, 0 holds | 0, 0, 0, 0 holds | 1.5 ms | 0, 0, 0, 0 polls | 0, 0, 0, 0 polls | 1.2 ms | 0 turns | 17 / 8 pages |
| isolated | r2 | 0, 0, 0, 0 holds | 0, 0, 0, 0 holds | 1.5 ms | 0, 0, 0, 0 polls | 0, 0, 0, 0 polls | 1.7 ms | 0 turns | 18 / 8 pages |
| isolated | r3 | 0, 0, 0, 0 holds | 0, 0, 0, 0 holds | 1.9 ms | 0, 0, 0, 0 polls | 0, 0, 0, 0 polls | 5.3 ms | 0 turns | 17 / 8 pages |
| isolated | r4 | 0, 0, 0, 0 holds | 0, 0, 0, 0 holds | 1.8 ms | 0, 0, 0, 0 polls | 0, 0, 0, 0 polls | 932 µs | 0 turns | 19 / 8 pages |
| isolated | r5 | 0, 0, 0, 0 holds | 0, 0, 0, 0 holds | 1.5 ms | 0, 0, 0, 0 polls | 0, 0, 0, 0 polls | 1.9 ms | 0 turns | 16 / 8 pages |
| levels | r1 | 0, 0, 1 holds | 0, 0, 0 holds | 57.0 ms | 0, 0, 0 polls | 0, 0, 0 polls | 7.5 ms | 125 turns | 13,439 / 13,998 pages |
| levels | r2 | 1, 1, 2 holds | 0, 0, 1 holds | 208.4 ms | 0, 0, 1 polls | 0, 0, 0 polls | 3.8 ms | 49 turns | 13,944 / 10,653 pages |
| levels | r3 | 0, 2, 2 holds | 0, 0, 0 holds | 86.8 ms | 0, 0, 0 polls | 0, 0, 0 polls | 3.7 ms | 103,415 turns | 18,294 / 13,659 pages |
| levels | r4 | 0, 1, 1 holds | 0, 1, 1 holds | 317.3 ms | 0, 1, 1 polls | 0, 0, 0 polls | 7.5 ms | 65 turns | 13,419 / 12,467 pages |
| levels | r5 | 0, 1, 2 holds | 0, 1, 1 holds | 433.8 ms | 0, 1, 1 polls | 0, 0, 0 polls | 7.9 ms | 75 turns | 13,445 / 14,034 pages |
| c1 | r1 | 0, 0, 0 holds | 0, 0, 0 holds | 4.3 ms | 0, 0, 0 polls | 0, 0, 0 polls | 1.8 ms | 3,257 turns | 17,798 / 7,378 pages |
| c1 | r2 | 0, 0, 0 holds | 0, 0, 0 holds | 6.3 ms | 0, 0, 0 polls | 0, 0, 0 polls | 6.8 ms | 24 turns | 13,466 / 12,455 pages |
| c1 | r3 | 0, 0, 0 holds | 0, 0, 0 holds | 4.8 ms | 0, 0, 0 polls | 0, 0, 0 polls | 7.2 ms | 197 turns | 11,525 / 8,199 pages |
| c1 | r4 | 0, 0, 0 holds | 0, 0, 0 holds | 6.6 ms | 0, 0, 0 polls | 0, 0, 0 polls | 3.6 ms | 68 turns | 13,463 / 12,452 pages |
| c1 | r5 | 0, 0, 0 holds | 0, 0, 0 holds | 96.8 ms | 0, 0, 0 polls | 0, 0, 0 polls | 6.6 ms | 62 turns | 12,313 / 13,235 pages |
| c2 | r1 | 0, 0, 1 holds | 0, 0, 0 holds | 10.2 ms | 0, 0, 0 polls | 0, 0, 0 polls | 1.1 ms | 211 turns | 26,507 / 23,442 pages |
| c2 | r2 | 1, 0, 1 holds | 0, 0, 0 holds | 19.9 ms | 0, 0, 0 polls | 0, 0, 0 polls | 1.4 ms | 165 turns | 28,070 / 25,030 pages |
| c2 | r3 | 0, 0, 0 holds | 0, 0, 0 holds | 9.6 ms | 0, 0, 0 polls | 0, 0, 0 polls | 4.9 ms | 7,376 turns | 27,922 / 24,718 pages |
| c2 | r4 | 0, 0, 0 holds | 0, 0, 0 holds | 9.9 ms | 0, 0, 0 polls | 0, 0, 0 polls | 1.2 ms | 93 turns | 28,008 / 24,980 pages |
| c2 | r5 | 2, 1, 1 holds | 1, 1, 0 holds | 253.8 ms | 1, 1, 0 polls | 0, 0, 0 polls | 6.0 ms | 3,220 turns | 25,380 / 9,416 pages |
| c1c2 | r1 | 0, 0, 0 holds | 0, 0, 0 holds | 9.3 ms | 0, 0, 0 polls | 0, 0, 0 polls | 6.4 ms | 4,424 turns | 18,146 / 16,121 pages |
| c1c2 | r2 | 0, 1, 0 holds | 0, 0, 0 holds | 22.3 ms | 0, 0, 0 polls | 0, 0, 0 polls | 1.2 ms | 78 turns | 28,218 / 24,921 pages |
| c1c2 | r3 | 0, 0, 0 holds | 0, 0, 0 holds | 9.7 ms | 0, 0, 0 polls | 0, 0, 0 polls | 1.7 ms | 182 turns | 27,845 / 24,544 pages |
| c1c2 | r4 | 0, 0, 0 holds | 0, 0, 0 holds | 8.9 ms | 0, 0, 0 polls | 0, 0, 0 polls | 4.1 ms | 253 turns | 25,436 / 22,019 pages |
| c1c2 | r5 | 0, 0, 0 holds | 0, 0, 0 holds | 9.9 ms | 0, 0, 0 polls | 0, 1, 0 polls | 117.5 ms | 425,990 turns | 25,399 / 21,745 pages |

#### 3.3 the same, a cell
| cell | in-run holds > 10 ms, read cores, 5 runs | in-run holds > 100 ms | foreground polls > 10 ms |
|---|---|---|---|
| isolated | 0 holds | 0 holds | 0 polls |
| levels | 14 holds | 5 holds | 0 polls |
| c1 | 0 holds | 0 holds | 0 polls |
| c2 | 7 holds | 2 holds | 0 polls |
| c1c2 | 1 holds | 0 holds | 1 polls |

### 4. The log, the rolls and the inserter over the window (C2's subject; CZ-Q2's guards, reported)

`live segments` is derived from the log lines, as in CZ-S0. `body` is the roll's body outcome: `hit` the next segment's body was ready, `waited` the roll waited for the preparer, `inline` the roll built it.

| cell | run | log rate | inserter rate | rolls in window | hold median | hold p90 | hold max | hold total | share of window under a roll hold | body hit / waited / inline | checkpoint runs in window | share of window under a checkpoint run | longest run | live segments at the end |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| isolated | r1 | 0.0 MB/s | 0 rows/s | 0 rolls | - | - | - | 0 µs | 0 % | 0/0/0 | 8 runs | 0 % | 5.7 ms | 1 segments |
| isolated | r2 | 0.0 MB/s | 0 rows/s | 0 rolls | - | - | - | 0 µs | 0 % | 0/0/0 | 8 runs | 0 % | 5.7 ms | 1 segments |
| isolated | r3 | 0.0 MB/s | 0 rows/s | 0 rolls | - | - | - | 0 µs | 0 % | 0/0/0 | 8 runs | 3 % | 268.0 ms | 1 segments |
| isolated | r4 | 0.0 MB/s | 0 rows/s | 0 rolls | - | - | - | 0 µs | 0 % | 0/0/0 | 8 runs | 0 % | 5.4 ms | 1 segments |
| isolated | r5 | 0.0 MB/s | 0 rows/s | 0 rolls | - | - | - | 0 µs | 0 % | 0/0/0 | 8 runs | 0 % | 5.6 ms | 1 segments |
| levels | r1 | 74.2 MB/s | 158,795 rows/s | 11 rolls | 249.9 ms | 383.0 ms | 788.1 ms | 3445.8 ms | 34 % | 7/4/0 | 2 runs | 21 % | 1622.0 ms | 14 segments |
| levels | r2 | 73.5 MB/s | 157,510 rows/s | 11 rolls | 268.1 ms | 319.3 ms | 350.2 ms | 2539.2 ms | 25 % | 8/3/0 | 6 runs | 45 % | 1327.6 ms | 10 segments |
| levels | r3 | 73.0 MB/s | 156,245 rows/s | 11 rolls | 228.7 ms | 322.4 ms | 353.7 ms | 2160.4 ms | 22 % | 10/1/0 | 8 runs | 44 % | 1100.4 ms | 7 segments |
| levels | r4 | 74.0 MB/s | 158,412 rows/s | 11 rolls | 249.3 ms | 332.7 ms | 372.6 ms | 2739.0 ms | 27 % | 7/4/0 | 2 runs | 24 % | 1677.7 ms | 14 segments |
| levels | r5 | 72.7 MB/s | 158,919 rows/s | 12 rolls | 254.0 ms | 379.2 ms | 794.3 ms | 3742.9 ms | 35 % | 7/5/0 | 2 runs | 21 % | 1668.3 ms | 14 segments |
| c1 | r1 | 75.9 MB/s | 169,818 rows/s | 11 rolls | 235.6 ms | 342.9 ms | 346.6 ms | 2456.4 ms | 25 % | 9/2/0 | 4 runs | 29 % | 1257.5 ms | 14 segments |
| c1 | r2 | 74.1 MB/s | 158,730 rows/s | 11 rolls | 254.0 ms | 384.1 ms | 721.7 ms | 3515.9 ms | 35 % | 7/4/0 | 2 runs | 23 % | 1676.1 ms | 14 segments |
| c1 | r3 | 73.9 MB/s | 161,644 rows/s | 12 rolls | 268.1 ms | 351.8 ms | 369.0 ms | 3119.5 ms | 29 % | 9/3/0 | 5 runs | 36 % | 1478.6 ms | 11 segments |
| c1 | r4 | 73.8 MB/s | 157,973 rows/s | 11 rolls | 251.4 ms | 386.0 ms | 669.9 ms | 3415.2 ms | 34 % | 7/4/0 | 2 runs | 23 % | 1616.9 ms | 14 segments |
| c1 | r5 | 75.8 MB/s | 162,282 rows/s | 11 rolls | 254.7 ms | 320.6 ms | 743.8 ms | 3363.1 ms | 34 % | 7/4/0 | 2 runs | 21 % | 1610.4 ms | 14 segments |
| c2 | r1 | 113.5 MB/s | 245,799 rows/s | 17 rolls | 5.9 ms | 8.7 ms | 609.0 ms | 703.1 ms | 2 % | 17/0/0 | 8 runs | 18 % | 258.4 ms | 10 segments |
| c2 | r2 | 113.4 MB/s | 242,968 rows/s | 17 rolls | 5.7 ms | 6.2 ms | 21.5 ms | 111.8 ms | 1 % | 17/0/0 | 8 runs | 19 % | 294.0 ms | 10 segments |
| c2 | r3 | 112.2 MB/s | 240,650 rows/s | 17 rolls | 5.5 ms | 6.8 ms | 6.9 ms | 95.6 ms | 1 % | 17/0/0 | 8 runs | 18 % | 300.9 ms | 10 segments |
| c2 | r4 | 113.5 MB/s | 243,126 rows/s | 17 rolls | 5.9 ms | 7.3 ms | 13.4 ms | 105.5 ms | 1 % | 17/0/0 | 8 runs | 18 % | 298.3 ms | 10 segments |
| c2 | r5 | 97.6 MB/s | 214,511 rows/s | 15 rolls | 6.0 ms | 18.2 ms | 618.9 ms | 716.0 ms | 7 % | 15/0/0 | 3 runs | 19 % | 1392.0 ms | 19 segments |
| c1c2 | r1 | 72.8 MB/s | 156,246 rows/s | 11 rolls | 8.8 ms | 11.1 ms | 13.3 ms | 89.9 ms | 1 % | 11/0/0 | 8 runs | 17 % | 412.3 ms | 7 segments |
| c1c2 | r2 | 113.9 MB/s | 244,187 rows/s | 17 rolls | 5.2 ms | 7.1 ms | 7.2 ms | 93.6 ms | 1 % | 17/0/0 | 8 runs | 20 % | 299.0 ms | 10 segments |
| c1c2 | r3 | 112.8 MB/s | 241,647 rows/s | 17 rolls | 5.8 ms | 6.7 ms | 7.0 ms | 100.5 ms | 1 % | 17/0/0 | 8 runs | 19 % | 313.5 ms | 10 segments |
| c1c2 | r4 | 100.7 MB/s | 216,214 rows/s | 15 rolls | 5.7 ms | 14.1 ms | 83.6 ms | 173.2 ms | 2 % | 15/0/0 | 8 runs | 23 % | 732.3 ms | 8 segments |
| c1c2 | r5 | 102.5 MB/s | 219,518 rows/s | 16 rolls | 5.7 ms | 119.1 ms | 530.7 ms | 729.0 ms | 7 % | 16/0/0 | 8 runs | 28 % | 1062.9 ms | 10 segments |

#### 4.1 roll holds pooled over the runs of a cell, and the inserter
| cell | rolls | hold p50 | hold p90 | hold p99 | hold max | holds over 300 ms | sync-gate share of the hold | create share of the hold | inserter rate (median) | against `levels` |
|---|---|---|---|---|---|---|---|---|---|---|
| levels | 56 rolls | 250.9 ms | 353.7 ms | 794.3 ms | 794.3 ms | 14 rolls | 98.6 % | 1.1 % | 158,412 rows/s | +0.0 % |
| c1 | 56 rolls | 251.4 ms | 369.0 ms | 743.8 ms | 743.8 ms | 18 rolls | 98.6 % | 1.0 % | 161,644 rows/s | +2.0 % |
| c2 | 83 rolls | 5.7 ms | 7.3 ms | 618.9 ms | 618.9 ms | 2 rolls | 75.4 % | 10.6 % | 242,968 rows/s | +53.4 % |
| c1c2 | 76 rolls | 5.7 ms | 9.2 ms | 530.7 ms | 530.7 ms | 1 rolls | 63.0 % | 16.7 % | 219,518 rows/s | +38.6 % |

### 5. The cadence checkpoint's stream-latch sites, as the spike counted them

Summed over the checkpoint runs that overlap the window, all 5 runs of a cell; `longest call` and `longest latch wait` are the largest single ones.

#### 5.1 read cores (1-3)
| cell | checkpoint runs | site | calls | busy | parked on a roll | bound reached | reactor time in calls | longest call | stream-latch wait | longest latch wait | parked for the latch |
|---|---|---|---|---|---|---|---|---|---|---|---|
| levels | 16 runs | begin | 16 | 0 | 0 | 0 | 556.4 ms | 158.1 ms | 556.1 ms | 158.0 ms | 0 µs |
| levels | 16 runs | assert | 16 | 0 | 0 | 0 | 0 µs | 0 µs | 0 µs | 0 µs | 0 µs |
| levels | 16 runs | flush | 512 | 0 | 0 | 0 | 1493.8 ms | 433.3 ms | 1240.7 ms | 433.3 ms | 0 µs |
| levels | 16 runs | end | 16 | 0 | 0 | 0 | 7.8 ms | 7.8 ms | 7.8 ms | 7.8 ms | 0 µs |
| levels | 16 runs | recycle | 16 | 0 | 0 | 0 | 6.3 ms | 6.1 ms | 0 µs | 0 µs | 0 µs |
| levels | 16 runs | gate_step | 28 | 0 | 0 | 0 | 38.9 ms | 2.3 ms | 0 µs | 0 µs | 0 µs |
| levels | 16 runs | gate_publish | 22 | 0 | 0 | 0 | 37.5 ms | 2.8 ms | 0 µs | 0 µs | 0 µs |
| c1 | 13 runs | begin | 19 | 6 | 5 | 0 | 268 µs | 43 µs | 0 µs | 0 µs | 462.2 ms |
| c1 | 13 runs | assert | 13 | 0 | 0 | 0 | 0 µs | 0 µs | 0 µs | 0 µs | 0 µs |
| c1 | 13 runs | flush | 7849 | 7401 | 10 | 0 | 39.3 ms | 1.5 ms | 0 µs | 0 µs | 1011.2 ms |
| c1 | 13 runs | end | 16 | 3 | 3 | 0 | 39 µs | 6 µs | 0 µs | 0 µs | 23.1 ms |
| c1 | 13 runs | recycle | 13 | 1 | 0 | 0 | 37 µs | 5 µs | 0 µs | 0 µs | 0 µs |
| c1 | 13 runs | gate_step | 22 | 0 | 0 | 0 | 31.8 ms | 2.0 ms | 0 µs | 0 µs | 0 µs |
| c1 | 13 runs | gate_publish | 19 | 0 | 0 | 0 | 26.9 ms | 3.8 ms | 0 µs | 0 µs | 0 µs |
| c2 | 26 runs | begin | 26 | 0 | 0 | 0 | 1.5 ms | 435 µs | 206 µs | 206 µs | 0 µs |
| c2 | 26 runs | assert | 26 | 0 | 0 | 0 | 0 µs | 0 µs | 0 µs | 0 µs | 0 µs |
| c2 | 26 runs | flush | 1249 | 0 | 0 | 0 | 516.6 ms | 253.8 ms | 299.9 ms | 253.8 ms | 0 µs |
| c2 | 26 runs | end | 26 | 0 | 0 | 0 | 50 µs | 3 µs | 0 µs | 0 µs | 0 µs |
| c2 | 26 runs | recycle | 26 | 0 | 0 | 0 | 1.1 ms | 434 µs | 0 µs | 0 µs | 0 µs |
| c2 | 26 runs | gate_step | 92 | 0 | 0 | 0 | 265.5 ms | 4.3 ms | 0 µs | 0 µs | 0 µs |
| c2 | 26 runs | gate_publish | 63 | 0 | 0 | 0 | 181.2 ms | 3.7 ms | 0 µs | 0 µs | 0 µs |
| c1c2 | 30 runs | begin | 32 | 2 | 0 | 0 | 450 µs | 33 µs | 0 µs | 0 µs | 9 µs |
| c1c2 | 30 runs | assert | 30 | 0 | 0 | 0 | 0 µs | 0 µs | 0 µs | 0 µs | 0 µs |
| c1c2 | 30 runs | flush | 53698 | 52272 | 8 | 0 | 232.5 ms | 1.3 ms | 0 µs | 0 µs | 80.7 ms |
| c1c2 | 30 runs | end | 33 | 3 | 2 | 0 | 75 µs | 5 µs | 0 µs | 0 µs | 34.5 ms |
| c1c2 | 30 runs | recycle | 30 | 3 | 0 | 0 | 210 µs | 20 µs | 0 µs | 0 µs | 0 µs |
| c1c2 | 30 runs | gate_step | 93 | 0 | 0 | 0 | 275.5 ms | 4.8 ms | 0 µs | 0 µs | 0 µs |
| c1c2 | 30 runs | gate_publish | 78 | 0 | 0 | 0 | 233.4 ms | 16.1 ms | 6 µs | 6 µs | 0 µs |

#### 5.2 core 0
| cell | checkpoint runs | site | calls | busy | parked on a roll | bound reached | reactor time in calls | longest call | stream-latch wait | longest latch wait | parked for the latch |
|---|---|---|---|---|---|---|---|---|---|---|---|
| levels | 4 runs | begin | 4 | 0 | 0 | 0 | 29 µs | 9 µs | 0 µs | 0 µs | 0 µs |
| levels | 4 runs | assert | 4 | 0 | 0 | 0 | 12 µs | 7 µs | 0 µs | 0 µs | 0 µs |
| levels | 4 runs | flush | 214 | 0 | 0 | 0 | 47.6 ms | 17.7 ms | 0 µs | 0 µs | 0 µs |
| levels | 4 runs | end | 4 | 0 | 0 | 0 | 4 µs | 1 µs | 0 µs | 0 µs | 0 µs |
| levels | 4 runs | recycle | 4 | 0 | 0 | 0 | 24 µs | 9 µs | 0 µs | 0 µs | 0 µs |
| levels | 4 runs | gate_step | 7 | 0 | 0 | 0 | 19.8 ms | 7.2 ms | 0 µs | 0 µs | 0 µs |
| levels | 4 runs | gate_publish | 5 | 0 | 0 | 0 | 10.1 ms | 2.1 ms | 0 µs | 0 µs | 0 µs |
| c1 | 2 runs | begin | 2 | 0 | 0 | 0 | 12 µs | 6 µs | 0 µs | 0 µs | 0 µs |
| c1 | 2 runs | assert | 2 | 0 | 0 | 0 | 4 µs | 2 µs | 0 µs | 0 µs | 0 µs |
| c1 | 2 runs | flush | 109 | 0 | 0 | 0 | 27.8 ms | 14.0 ms | 0 µs | 0 µs | 0 µs |
| c1 | 2 runs | end | 2 | 0 | 0 | 0 | 2 µs | 1 µs | 0 µs | 0 µs | 0 µs |
| c1 | 2 runs | recycle | 2 | 0 | 0 | 0 | 8 µs | 4 µs | 0 µs | 0 µs | 0 µs |
| c1 | 2 runs | gate_step | 1 | 0 | 0 | 0 | 2.0 ms | 2.0 ms | 0 µs | 0 µs | 0 µs |
| c1 | 2 runs | gate_publish | 4 | 0 | 0 | 0 | 8.0 ms | 2.0 ms | 0 µs | 0 µs | 0 µs |
| c2 | 9 runs | begin | 9 | 0 | 0 | 0 | 130 µs | 18 µs | 0 µs | 0 µs | 0 µs |
| c2 | 9 runs | assert | 9 | 0 | 0 | 0 | 19 µs | 3 µs | 0 µs | 0 µs | 0 µs |
| c2 | 9 runs | flush | 386 | 0 | 0 | 0 | 77.2 ms | 647 µs | 0 µs | 0 µs | 0 µs |
| c2 | 9 runs | end | 9 | 0 | 0 | 0 | 10 µs | 2 µs | 0 µs | 0 µs | 0 µs |
| c2 | 9 runs | recycle | 9 | 0 | 0 | 0 | 55 µs | 13 µs | 0 µs | 0 µs | 0 µs |
| c2 | 9 runs | gate_step | 14 | 0 | 0 | 0 | 43.8 ms | 3.8 ms | 0 µs | 0 µs | 0 µs |
| c2 | 9 runs | gate_publish | 9 | 0 | 0 | 0 | 27.9 ms | 3.8 ms | 0 µs | 0 µs | 0 µs |
| c1c2 | 10 runs | begin | 10 | 0 | 0 | 0 | 102 µs | 14 µs | 0 µs | 0 µs | 0 µs |
| c1c2 | 10 runs | assert | 10 | 0 | 0 | 0 | 19 µs | 2 µs | 0 µs | 0 µs | 0 µs |
| c1c2 | 10 runs | flush | 436 | 0 | 0 | 0 | 95.7 ms | 4.3 ms | 0 µs | 0 µs | 0 µs |
| c1c2 | 10 runs | end | 10 | 0 | 0 | 0 | 13 µs | 2 µs | 0 µs | 0 µs | 0 µs |
| c1c2 | 10 runs | recycle | 10 | 0 | 0 | 0 | 67 µs | 13 µs | 0 µs | 0 µs | 0 µs |
| c1c2 | 10 runs | gate_step | 15 | 0 | 0 | 0 | 45.0 ms | 3.8 ms | 0 µs | 0 µs | 0 µs |
| c1c2 | 10 runs | gate_publish | 9 | 0 | 0 | 0 | 26.9 ms | 3.8 ms | 0 µs | 0 µs | 0 µs |

#### 5.3 the read cores' stream-latch wait against what the sites counted

The first column is the engine's own counter (a window delta); the second is the spike's, over the read cores' runs that overlap the window. They differ by the runs that straddle a window edge or a control read, and by any taker that is not a site.

| cell | stream-latch wait, read cores (engine's counter) | at the spike's sites | begin | assert | flush | end | recycle | gate_step | gate_publish |
|---|---|---|---|---|---|---|---|---|---|
| levels | 1732.8 ms | 1804.6 ms | 556.1 ms | 0 µs | 1240.7 ms | 7.8 ms | 0 µs | 0 µs | 0 µs |
| c1 | 0 µs | 0 µs | 0 µs | 0 µs | 0 µs | 0 µs | 0 µs | 0 µs | 0 µs |
| c2 | 303.5 ms | 300.1 ms | 206 µs | 0 µs | 299.9 ms | 0 µs | 0 µs | 0 µs | 0 µs |
| c1c2 | 6 µs | 6 µs | 0 µs | 0 µs | 0 µs | 0 µs | 0 µs | 0 µs | 6 µs |

#### 5.4 a read core's checkpoint run: what held its reactor longest (CZ-Q12: the page writes C1 leaves)

Over the read cores' runs that overlap the window: the longest single call of each kind in any run of the cell, and the runs in which it reached 10 ms.

| cell | runs | `start` (begin append) | page write | publish-phase call | step's whole hold | flush call | end append | recycle | inline gate (step / publish) |
|---|---|---|---|---|---|---|---|---|---|
| levels | 16 runs | 159.2 ms (7 runs) | 5.9 ms (0 runs) | 28.7 ms (1 runs) | 6.2 ms (0 runs) | 433.3 ms (6 runs) | 7.8 ms (0 runs) | 6.1 ms (0 runs) | 2.3 ms (0 runs) / 2.8 ms (0 runs) |
| c1 | 13 runs | 1.5 ms (0 runs) | 6.2 ms (0 runs) | 96.6 ms (1 runs) | 6.6 ms (0 runs) | 1.5 ms (0 runs) | 6 µs (0 runs) | 5 µs (0 runs) | 2.0 ms (0 runs) / 3.8 ms (0 runs) |
| c2 | 26 runs | 1.6 ms (0 runs) | 3.3 ms (0 runs) | 10.2 ms (2 runs) | 7.8 ms (0 runs) | 253.8 ms (3 runs) | 3 µs (0 runs) | 434 µs (0 runs) | 4.3 ms (0 runs) / 3.7 ms (0 runs) |
| c1c2 | 30 runs | 1.4 ms (0 runs) | 2.4 ms (0 runs) | 22.3 ms (1 runs) | 8.9 ms (0 runs) | 1.3 ms (0 runs) | 5 µs (0 runs) | 20 µs (0 runs) | 4.8 ms (0 runs) / 16.1 ms (1 runs) |

#### 5.5 checkpoint runs: wall time and parks (CZ-Q2's guard, reported)
| cell | runs in window (all cores) | wall median | wall max | log park, sum | data park, sum | parked for the latch, sum | gate parks | recycles skipped on a busy latch |
|---|---|---|---|---|---|---|---|---|
| levels | 20 runs | 743.6 ms | 1677.7 ms | 13337.3 ms | 1768.5 ms | 0 µs | 706 parks | 0 |
| c1 | 15 runs | 1257.5 ms | 1676.1 ms | 11629.9 ms | 2496.9 ms | 1496.5 ms | 552 parks | 1 |
| c2 | 35 runs | 225.4 ms | 1392.0 ms | 6449.6 ms | 418.5 ms | 0 µs | 1600 parks | 0 |
| c1c2 | 40 runs | 227.6 ms | 1062.9 ms | 7865.6 ms | 504.7 ms | 115.2 ms | 1830 parks | 3 |

### 6. The reader samples of 1 ms to 10 ms: inside and outside a read core's checkpoint run (what C1 leaves)

A sample is inside when its interval overlaps a checkpoint run of a read core (of any core in `isolated`). The rate is samples per second of the window so covered.

| cell | samples of 1-10 ms, 5 runs | inside a run | window under such runs | rate inside | rate outside |
|---|---|---|---|---|---|
| isolated | 324 samples | 3 samples | 0.5 s of 50 s | 6.4 /s | 6.5 /s |
| levels | 522 samples | 175 samples | 12.3 s of 50 s | 14.2 /s | 9.2 /s |
| c1 | 397 samples | 96 samples | 11.4 s of 50 s | 8.4 /s | 7.8 /s |
| c2 | 557 samples | 139 samples | 6.8 s of 50 s | 20.4 /s | 9.7 /s |
| c1c2 | 775 samples | 172 samples | 7.9 s of 50 s | 21.7 /s | 14.3 /s |
