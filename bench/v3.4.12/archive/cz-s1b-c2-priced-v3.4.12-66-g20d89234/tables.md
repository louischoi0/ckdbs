### 0. The host at each cell (bench/README.md rule 4)

`build` marks a cell with a `cc1plus|cmake --build|ctest` process at either end or a load of 2 or more at its start.

| cell | run | driver wait | load at start | load at end | competing at start | competing at end | marked | `cz_mode` | errors |
|---|---|---|---|---|---|---|---|---|---|
| iso | r1 | 20 s | 1.47 | 1.40 | 0 processes | 0 processes | - | [0] | 0 errors |
| iso | r2 | 10 s | 1.48 | 1.35 | 0 processes | 2 processes | build | [0] | 0 errors |
| iso | r3 | 5 s | 1.41 | 1.16 | 0 processes | 0 processes | - | [0] | 0 errors |
| iso | r4 | 0 s | 1.34 | 1.04 | 0 processes | 0 processes | - | [0] | 0 errors |
| iso | r5 | 0 s | 1.39 | 1.15 | 0 processes | 0 processes | - | [0] | 0 errors |
| c1h | r1 | 0 s | 1.40 | 1.36 | 0 processes | 0 processes | - | [1] | 0 errors |
| c1h | r2 | 0 s | 1.34 | 1.25 | 0 processes | 0 processes | - | [1] | 0 errors |
| c1h | r3 | 0 s | 1.16 | 1.13 | 0 processes | 10 processes | build | [1] | 0 errors |
| c1h | r4 | 0 s | 1.04 | 1.19 | 0 processes | 0 processes | - | [1] | 0 errors |
| c1h | r5 | 0 s | 1.15 | 1.20 | 0 processes | 0 processes | - | [1] | 0 errors |
| c1c2h | r1 | 0 s | 1.36 | 1.33 | 0 processes | 0 processes | - | [3] | 0 errors |
| c1c2h | r2 | 0 s | 1.25 | 1.27 | 0 processes | 0 processes | - | [3] | 0 errors |
| c1c2h | r3 | 0 s | 1.35 | 1.64 | 2 processes | 3 processes | build | [3] | 0 errors |
| c1c2h | r4 | 0 s | 1.19 | 1.36 | 0 processes | 0 processes | - | [3] | 0 errors |
| c1c2h | r5 | 0 s | 1.20 | 1.14 | 0 processes | 0 processes | - | [3] | 0 errors |
| c1x | r1 | 0 s | 1.01 | 1.25 | 0 processes | 0 processes | - | [1] | 0 errors |
| c1x | r2 | 0 s | 1.28 | 1.44 | 0 processes | 0 processes | - | [1] | 0 errors |
| c1x | r3 | 0 s | 1.44 | 1.54 | 0 processes | 0 processes | - | [1] | 0 errors |
| c1x | r4 | 0 s | 1.49 | 1.49 | 0 processes | 0 processes | - | [1] | 0 errors |
| c1x | r5 | 0 s | 1.49 | 1.59 | 0 processes | 0 processes | - | [1] | 0 errors |
| c1c2x | r1 | 0 s | 1.25 | 2.63 | 0 processes | 10 processes | build | [3] | 0 errors |
| c1c2x | r2 | 20 s | 1.44 | 1.28 | 0 processes | 0 processes | - | [3] | 0 errors |
| c1c2x | r3 | 5 s | 1.42 | 1.44 | 0 processes | 0 processes | - | [3] | 0 errors |
| c1c2x | r4 | 0 s | 1.44 | 1.49 | 0 processes | 0 processes | - | [3] | 0 errors |
| c1c2x | r5 | 5 s | 1.46 | 1.34 | 0 processes | 0 processes | - | [3] | 0 errors |
| g-base | r1 | 0 s | 1.33 | 1.10 | 0 processes | 0 processes | - | [0] | 0 errors |
| g-base | r2 | 0 s | 1.27 | 1.71 | 0 processes | 3 processes | build | [0] | 0 errors |
| g-base | r3 | 120 s | 7.40 | 7.83 | 2 processes | 2 processes | build | [0] | 0 errors |
| g-base | r4 | 15 s | 1.46 | 1.61 | 0 processes | 2 processes | build | [0] | 0 errors |
| g-base | r5 | 0 s | 1.14 | 1.20 | 0 processes | 0 processes | - | [0] | 0 errors |
| g-c2 | r1 | 0 s | 1.10 | 1.43 | 0 processes | 0 processes | - | [2] | 0 errors |
| g-c2 | r2 | 15 s | 1.45 | 1.46 | 0 processes | 0 processes | - | [2] | 0 errors |
| g-c2 | r3 | 20 s | 1.49 | 1.29 | 0 processes | 0 processes | - | [2] | 0 errors |
| g-c2 | r4 | 120 s | 8.00 | 8.36 | 2 processes | 2 processes | build | [2] | 0 errors |
| g-c2 | r5 | 0 s | 1.36 | 1.12 | 0 processes | 0 processes | - | [2] | 0 errors |
| s-base | r1 | 0 s | 1.43 | 1.34 | 0 processes | 0 processes | - | [0] | 0 errors |
| s-base | r2 | 0 s | 1.46 | 1.12 | 0 processes | 0 processes | - | [0] | 0 errors |
| s-base | r3 | 0 s | 1.29 | 2.42 | 0 processes | 4 processes | build | [0] | 0 errors |
| s-base | r4 | 20 s | 1.47 | 1.20 | 0 processes | 0 processes | - | [0] | 0 errors |
| s-base | r5 | 0 s | 1.12 | 1.43 | 0 processes | 0 processes | - | [0] | 0 errors |
| s-c2 | r1 | 0 s | 1.34 | 1.19 | 0 processes | 0 processes | - | [2] | 0 errors |
| s-c2 | r2 | 0 s | 1.12 | 1.36 | 0 processes | 2 processes | build | [2] | 0 errors |
| s-c2 | r3 | 15 s | 1.45 | 1.40 | 0 processes | 0 processes | - | [2] | 0 errors |
| s-c2 | r4 | 0 s | 1.20 | 1.26 | 0 processes | 0 processes | - | [2] | 0 errors |
| s-c2 | r5 | 0 s | 1.43 | 1.49 | 0 processes | 0 processes | - | [2] | 0 errors |
| gb-base | r1 | 0 s | 1.25 | 1.27 | 0 processes | 0 processes | - | [0] | 0 errors |
| gb-base | r2 | 0 s | 1.36 | 1.79 | 2 processes | 2 processes | build | [0] | 0 errors |
| gb-base | r3 | 0 s | 1.40 | 1.69 | 0 processes | 0 processes | - | [0] | 0 errors |
| gb-base | r4 | 0 s | 1.26 | 1.36 | 0 processes | 0 processes | - | [0] | 0 errors |
| gb-base | r5 | 0 s | 1.49 | 1.73 | 0 processes | 0 processes | - | [0] | 0 errors |
| gb-c2 | r1 | 0 s | 1.27 | 1.34 | 0 processes | 0 processes | - | [2] | 0 errors |
| gb-c2 | r2 | 20 s | 1.47 | 1.57 | 0 processes | 2 processes | build | [2] | 0 errors |
| gb-c2 | r3 | 10 s | 1.43 | 1.53 | 0 processes | 0 processes | - | [2] | 0 errors |
| gb-c2 | r4 | 0 s | 1.36 | 1.34 | 0 processes | 0 processes | - | [2] | 0 errors |
| gb-c2 | r5 | 15 s | 1.42 | 1.39 | 0 processes | 0 processes | - | [2] | 0 errors |

### 1. C1 against C1 with C2 at one load (the inserter held to a rate, `relaxed`): `h` 120 statements a second, `x` 150

The `x` cells ran in a second pass, 5 runs after the first pass ended; their pairs are against the first pass's `iso` of the same run number, which is not interleaved with them.

#### 1.1 per run
| cell | run | reader p50 | reader p99 | reader p99.9 | reader max | stall samples | inserter rate | log rate | walker statements | walker p99 | rolls in window | roll hold median | roll hold max | in-run holds > 10 ms, read cores | marked |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| iso | r1 | 328 µs | 1.5 ms | 4.7 ms | 6.8 ms | 0 samples | 0 rows/s | 0.0 MB/s | 0 statements | 0 µs | 0 rolls | - | - | 0 holds | - |
| iso | r2 | 334 µs | 3.9 ms | 32.7 ms | 87.9 ms | 9 samples | 0 rows/s | 0.0 MB/s | 0 statements | 0 µs | 0 rolls | - | - | 2 holds | build |
| iso | r3 | 334 µs | 583 µs | 2.9 ms | 4.6 ms | 0 samples | 0 rows/s | 0.0 MB/s | 0 statements | 0 µs | 0 rolls | - | - | 0 holds | - |
| iso | r4 | 333 µs | 974 µs | 4.2 ms | 6.7 ms | 0 samples | 0 rows/s | 0.0 MB/s | 0 statements | 0 µs | 0 rolls | - | - | 0 holds | - |
| iso | r5 | 341 µs | 857 µs | 2.9 ms | 5.4 ms | 0 samples | 0 rows/s | 0.0 MB/s | 0 statements | 0 µs | 0 rolls | - | - | 0 holds | - |
| c1h | r1 | 320 µs | 2.7 ms | 5.2 ms | 6.0 ms | 0 samples | 122,928 rows/s | 57.3 MB/s | 10694 statements | 6.6 ms | 9 rolls | 3.9 ms | 28.8 ms | 0 holds | - |
| c1h | r2 | 320 µs | 3.0 ms | 5.4 ms | 8.4 ms | 0 samples | 122,916 rows/s | 57.4 MB/s | 10269 statements | 7.0 ms | 9 rolls | 3.9 ms | 32.0 ms | 0 holds | - |
| c1h | r3 | 331 µs | 3.9 ms | 82.1 ms | 97.8 ms | 21 samples | 122,629 rows/s | 57.0 MB/s | 9617 statements | 7.7 ms | 9 rolls | 4.3 ms | 31.4 ms | 1 holds | build |
| c1h | r4 | 340 µs | 1.6 ms | 3.3 ms | 5.3 ms | 0 samples | 122,912 rows/s | 57.3 MB/s | 9836 statements | 7.5 ms | 9 rolls | 8.5 ms | 34.5 ms | 0 holds | - |
| c1h | r5 | 333 µs | 1.4 ms | 19.2 ms | 46.6 ms | 6 samples | 122,929 rows/s | 57.3 MB/s | 10461 statements | 7.3 ms | 9 rolls | 3.8 ms | 35.3 ms | 1 holds | - |
| c1c2h | r1 | 317 µs | 2.1 ms | 5.1 ms | 5.6 ms | 0 samples | 122,935 rows/s | 57.3 MB/s | 11806 statements | 4.5 ms | 9 rolls | 4.5 ms | 5.5 ms | 0 holds | - |
| c1c2h | r2 | 317 µs | 1.8 ms | 5.3 ms | 6.0 ms | 0 samples | 122,935 rows/s | 57.3 MB/s | 11735 statements | 4.5 ms | 9 rolls | 4.9 ms | 5.8 ms | 0 holds | - |
| c1c2h | r3 | 322 µs | 2.5 ms | 4.4 ms | 6.4 ms | 0 samples | 122,933 rows/s | 57.3 MB/s | 8826 statements | 6.9 ms | 9 rolls | 4.9 ms | 447.2 ms | 0 holds | build |
| c1c2h | r4 | 331 µs | 1.2 ms | 4.8 ms | 6.3 ms | 0 samples | 122,935 rows/s | 57.4 MB/s | 11764 statements | 4.5 ms | 9 rolls | 4.8 ms | 5.3 ms | 0 holds | - |
| c1c2h | r5 | 329 µs | 864 µs | 3.9 ms | 4.6 ms | 0 samples | 122,933 rows/s | 57.3 MB/s | 11688 statements | 4.5 ms | 9 rolls | 4.9 ms | 6.4 ms | 0 holds | - |
| c1x | r1 | 336 µs | 2.6 ms | 6.1 ms | 8.2 ms | 0 samples | 153,143 rows/s | 71.4 MB/s | 5674 statements | 12.1 ms | 11 rolls | 58.0 ms | 109.3 ms | 0 holds | - |
| c1x | r2 | 331 µs | 1.5 ms | 6.0 ms | 8.5 ms | 0 samples | 152,208 rows/s | 71.0 MB/s | 5452 statements | 14.2 ms | 11 rolls | 55.9 ms | 116.9 ms | 0 holds | - |
| c1x | r3 | 332 µs | 1.2 ms | 3.6 ms | 4.1 ms | 0 samples | 152,306 rows/s | 71.1 MB/s | 5945 statements | 12.6 ms | 11 rolls | 36.9 ms | 119.4 ms | 0 holds | - |
| c1x | r4 | 339 µs | 1.7 ms | 5.3 ms | 7.4 ms | 0 samples | 152,296 rows/s | 71.0 MB/s | 5421 statements | 14.3 ms | 11 rolls | 36.3 ms | 85.9 ms | 0 holds | - |
| c1x | r5 | 334 µs | 2.4 ms | 6.4 ms | 8.4 ms | 0 samples | 152,329 rows/s | 71.1 MB/s | 5532 statements | 13.7 ms | 11 rolls | 21.2 ms | 116.2 ms | 0 holds | - |
| c1c2x | r1 | 356 µs | 3.7 ms | 7.9 ms | 10.4 ms | 1 samples | 153,696 rows/s | 70.9 MB/s | 1036 statements | 22.7 ms | 11 rolls | 7.7 ms | 11.8 ms | 2 holds | build |
| c1c2x | r2 | 328 µs | 1.0 ms | 4.5 ms | 5.7 ms | 0 samples | 153,681 rows/s | 71.7 MB/s | 9324 statements | 4.6 ms | 11 rolls | 5.2 ms | 5.8 ms | 0 holds | - |
| c1c2x | r3 | 333 µs | 1.9 ms | 4.5 ms | 7.6 ms | 0 samples | 153,640 rows/s | 71.7 MB/s | 8906 statements | 4.8 ms | 11 rolls | 5.4 ms | 5.9 ms | 0 holds | - |
| c1c2x | r4 | 331 µs | 1.2 ms | 3.6 ms | 4.8 ms | 0 samples | 153,645 rows/s | 71.7 MB/s | 9029 statements | 5.0 ms | 11 rolls | 4.6 ms | 5.7 ms | 0 holds | - |
| c1c2x | r5 | 333 µs | 1.3 ms | 3.8 ms | 5.2 ms | 0 samples | 153,648 rows/s | 71.7 MB/s | 9043 statements | 4.9 ms | 11 rolls | 5.0 ms | 5.4 ms | 0 holds | - |

#### 1.2 medians of runs, and CZ-Q1 (a)'s two clauses
| cell | reader p99 (median) | p99 range | against `iso`'s median | pairs within 2x | pairs with no build on either side | p99.9 (median) | stall samples, all runs | inserter rate (median) | log rate (median) | walker statements (median) | walker p99 (median) | roll hold median, pooled | read cores' stream-latch wait, all runs |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| iso | 974 µs | 583 µs - 3.9 ms | 1.00x | 5 of 5 | 1.00x, 1.00x, 1.00x, 1.00x | 4.2 ms | 9 samples | 0 rows/s | 0.0 MB/s | 0 statements | 0 µs | - | 0 µs |
| c1h | 2.7 ms | 1.4 ms - 3.9 ms | 2.77x | 4 of 5 | 1.85x, 1.67x, 1.59x | 5.4 ms | 27 samples | 122,916 rows/s | 57.3 MB/s | 10269 statements | 7.3 ms | 4.0 ms | 0 µs |
| c1c2h | 1.8 ms | 864 µs - 2.5 ms | 1.87x | 4 of 5 | 1.47x, 1.22x, 1.01x | 4.8 ms | 0 samples | 122,935 rows/s | 57.3 MB/s | 11735 statements | 4.5 ms | 4.8 ms | 0 µs |
| c1x | 1.7 ms | 1.2 ms - 2.6 ms | 1.75x | 3 of 5 | 1.79x, 2.10x, 1.75x, 2.80x | 6.0 ms | 0 samples | 152,306 rows/s | 71.1 MB/s | 5532 statements | 13.7 ms | 39.6 ms | 9 µs |
| c1c2x | 1.3 ms | 1.0 ms - 3.7 ms | 1.29x | 3 of 5 | 3.29x, 1.22x, 1.46x | 4.5 ms | 1 samples | 153,648 rows/s | 71.7 MB/s | 9029 statements | 4.9 ms | 5.3 ms | 0 µs |

### 2. The commit path with and without the zero prewrite: 6 readers and 4 one-row writers at 800 a second

#### 2.1 per run
| cell | run | writer p50 | writer p99 | writer p99.9 | writer max | writer service p50 | writer service p99 | writes kept | reader p99 | writer syncs | group batches, all cores | commits a batch | rolls | marked |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| g-base | r1 | 789 µs | 1.9 ms | 5.3 ms | 6.4 ms | 699 µs | 1.5 ms | 8004 writes | 567 µs | 8002 syncs | 7983 batches | 1.0 | 0 rolls | - |
| g-base | r2 | 909 µs | 1566.8 ms | 1639.4 ms | 1646.1 ms | 709 µs | 8.2 ms | 8004 writes | 2.7 ms | 6480 syncs | 6386 batches | 1.3 | 0 rolls | build |
| g-base | r3 | 706 µs | 4.4 ms | 6.9 ms | 7.8 ms | 625 µs | 3.8 ms | 8004 writes | 2.9 ms | 7969 syncs | 7849 batches | 1.0 | 0 rolls | build |
| g-base | r4 | 799 µs | 4.0 ms | 5.3 ms | 8.8 ms | 708 µs | 2.6 ms | 8004 writes | 1.7 ms | 7984 syncs | 7974 batches | 1.0 | 0 rolls | build |
| g-base | r5 | 807 µs | 2.4 ms | 5.5 ms | 11.1 ms | 714 µs | 1.6 ms | 8003 writes | 1.4 ms | 8026 syncs | 7973 batches | 1.0 | 0 rolls | - |
| g-c2 | r1 | 809 µs | 3.4 ms | 6.1 ms | 7.0 ms | 718 µs | 2.6 ms | 8004 writes | 1.9 ms | 7985 syncs | 7970 batches | 1.0 | 0 rolls | - |
| g-c2 | r2 | 834 µs | 149.7 ms | 488.1 ms | 1234.6 ms | 740 µs | 2.7 ms | 7324 writes | 1.3 ms | 7206 syncs | 7322 batches | 1.0 | 0 rolls | - |
| g-c2 | r3 | 809 µs | 2.2 ms | 4.5 ms | 5.5 ms | 720 µs | 2.1 ms | 8004 writes | 614 µs | 7989 syncs | 7987 batches | 1.0 | 0 rolls | - |
| g-c2 | r4 | 717 µs | 109.9 ms | 179.4 ms | 184.6 ms | 637 µs | 4.0 ms | 8004 writes | 1.7 ms | 7805 syncs | 7860 batches | 1.0 | 0 rolls | build |
| g-c2 | r5 | 814 µs | 2.1 ms | 4.3 ms | 5.8 ms | 721 µs | 2.0 ms | 8004 writes | 494 µs | 8012 syncs | 7451 batches | 1.1 | 0 rolls | - |
| s-base | r1 | 783 µs | 2.7 ms | 5.5 ms | 8.3 ms | 693 µs | 2.1 ms | 8004 writes | 1.7 ms | 7998 syncs | 8002 batches | - | 0 rolls | - |
| s-base | r2 | 791 µs | 2.5 ms | 5.4 ms | 6.8 ms | 702 µs | 1.5 ms | 8004 writes | 897 µs | 7990 syncs | 7980 batches | - | 0 rolls | - |
| s-base | r3 | 832 µs | 4.1 ms | 6.0 ms | 8.1 ms | 739 µs | 3.3 ms | 8004 writes | 2.7 ms | 7972 syncs | 7937 batches | - | 0 rolls | build |
| s-base | r4 | 792 µs | 1.5 ms | 4.2 ms | 6.1 ms | 702 µs | 1.2 ms | 8004 writes | 477 µs | 8038 syncs | 7989 batches | - | 0 rolls | - |
| s-base | r5 | 804 µs | 1.5 ms | 4.5 ms | 7.4 ms | 713 µs | 1.2 ms | 8004 writes | 575 µs | 8006 syncs | 7963 batches | - | 0 rolls | - |
| s-c2 | r1 | 813 µs | 2.6 ms | 5.4 ms | 10.1 ms | 721 µs | 2.1 ms | 8004 writes | 1.3 ms | 7976 syncs | 7839 batches | - | 0 rolls | - |
| s-c2 | r2 | 836 µs | 4.0 ms | 5.6 ms | 9.2 ms | 743 µs | 3.3 ms | 8004 writes | 2.3 ms | 7934 syncs | 7627 batches | - | 0 rolls | build |
| s-c2 | r3 | 803 µs | 2.0 ms | 3.5 ms | 4.7 ms | 713 µs | 1.9 ms | 8004 writes | 493 µs | 8008 syncs | 7878 batches | - | 0 rolls | - |
| s-c2 | r4 | 800 µs | 2.1 ms | 5.2 ms | 7.6 ms | 708 µs | 1.9 ms | 8004 writes | 500 µs | 7994 syncs | 7726 batches | - | 0 rolls | - |
| s-c2 | r5 | 841 µs | 2.2 ms | 4.6 ms | 8.8 ms | 749 µs | 2.0 ms | 8003 writes | 529 µs | 7993 syncs | 7863 batches | - | 0 rolls | - |

#### 2.2 medians of runs
| cell | writer p50 | writer p99 | writer p99.9 | writer service p50 | writer service p99 | reader p99 | writer syncs a window | runs marked `build` |
|---|---|---|---|---|---|---|---|---|
| g-base | 799 µs | 4.0 ms | 5.5 ms | 708 µs | 2.6 ms | 1.7 ms | 7984 syncs | 3 of 5 |
| g-c2 | 809 µs | 3.4 ms | 6.1 ms | 720 µs | 2.6 ms | 1.3 ms | 7985 syncs | 1 of 5 |
| s-base | 792 µs | 2.5 ms | 5.4 ms | 702 µs | 1.5 ms | 897 µs | 7998 syncs | 1 of 5 |
| s-c2 | 813 µs | 2.2 ms | 5.2 ms | 721 µs | 2.0 ms | 529 µs | 7993 syncs | 1 of 5 |

#### 2.2a the writer's p95, which separates the arms where the median and the p99 do not
| cell | service p95 (median) | service p95, each run | from-schedule p95 (median) | writes 1.5 ms or more late, each run |
|---|---|---|---|---|
| g-base | 985 µs | 948 µs, 1.1 ms, 1.1 ms, 985 µs, 956 µs | 1.2 ms | 1.7 %, 41.2 %, 4.9 %, 3.6 %, 1.9 % |
| g-c2 | 1.6 ms | 1.6 ms, 1.7 ms, 1.6 ms, 1.8 ms, 1.6 ms | 1.8 ms | 9.1 %, 11.0 %, 7.4 %, 12.0 %, 6.9 % |
| s-base | 945 µs | 973 µs, 945 µs, 1.1 ms, 921 µs, 941 µs | 1.1 ms | 3.0 %, 1.8 %, 4.2 %, 1.0 %, 1.0 % |
| s-c2 | 1.6 ms | 1.6 ms, 1.7 ms, 1.5 ms, 1.6 ms, 1.6 ms | 1.7 ms | 8.4 %, 9.7 %, 6.7 %, 7.1 %, 7.5 % |

#### 2.3 the pairs: C2 against the base, medians
| durability | writer p50, base | writer p50, C2 | change | writer p99, base | writer p99, C2 | change | writer service p50, base | C2 | change |
|---|---|---|---|---|---|---|---|---|---|
| group | 799 µs | 809 µs | +1.3 % | 4.0 ms | 3.4 ms | -15.9 % | 708 µs | 720 µs | +1.8 % |
| strict | 792 µs | 813 µs | +2.6 % | 2.5 ms | 2.2 ms | -15.0 % | 702 µs | 721 µs | +2.7 % |

#### 2.4 the same over the runs no build reached
| cell | runs | writer p50 (median) | writer p50, each | writer service p50 (median) | writer p99 (median) | writer p99, each | writer p99.9 (median) | writer max, the largest |
|---|---|---|---|---|---|---|---|---|
| g-base | r1, r5 | 798 µs | 789 µs, 807 µs | 706 µs | 2.1 ms | 1.9 ms, 2.4 ms | 5.4 ms | 11.1 ms |
| g-c2 | r1, r2, r3, r5 | 812 µs | 809 µs, 834 µs, 809 µs, 814 µs | 721 µs | 2.8 ms | 3.4 ms, 149.7 ms, 2.2 ms, 2.1 ms | 5.3 ms | 1234.6 ms |
| s-base | r1, r2, r4, r5 | 792 µs | 783 µs, 791 µs, 792 µs, 804 µs | 702 µs | 2.0 ms | 2.7 ms, 2.5 ms, 1.5 ms, 1.5 ms | 4.9 ms | 8.3 ms |
| s-c2 | r1, r3, r4, r5 | 808 µs | 813 µs, 803 µs, 800 µs, 841 µs | 717 µs | 2.1 ms | 2.6 ms, 2.0 ms, 2.1 ms, 2.2 ms | 4.9 ms | 10.1 ms |

#### 2.5 the held-rate cells over the runs no build reached
| cell | runs | reader p99 (median) | reader p99, each | walker statements (median) | walker p99 (median) | stall samples | roll hold median, pooled | roll hold max |
|---|---|---|---|---|---|---|---|---|
| iso | r1, r3, r4, r5 | 915 µs | 1.5 ms, 583 µs, 974 µs, 857 µs | 0 statements | 0 µs | 0 samples | - | - |
| c1h | r1, r2, r4, r5 | 2.2 ms | 2.7 ms, 3.0 ms, 1.6 ms, 1.4 ms | 10365 statements | 7.2 ms | 6 samples | 3.9 ms | 35.3 ms |
| c1c2h | r1, r2, r4, r5 | 1.5 ms | 2.1 ms, 1.8 ms, 1.2 ms, 864 µs | 11750 statements | 4.5 ms | 0 samples | 4.8 ms | 6.4 ms |
| c1x | r1, r2, r3, r4, r5 | 1.7 ms | 2.6 ms, 1.5 ms, 1.2 ms, 1.7 ms, 2.4 ms | 5532 statements | 13.7 ms | 0 samples | 39.6 ms | 119.4 ms |
| c1c2x | r2, r3, r4, r5 | 1.2 ms | 1.0 ms, 1.9 ms, 1.2 ms, 1.3 ms | 9036 statements | 4.8 ms | 0 samples | 5.1 ms | 5.9 ms |

### 3. The bulk writer at `group`: the `levels` cell, the inserter closed-loop

| cell | run | inserter rate | inserter p50 | inserter p99 | inserter max | log rate | walker statements | walker p99 | reader p99 | stall samples | rolls in window | roll hold median | roll hold max | marked |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| gb-base | r1 | 121,469 rows/s | 5.6 ms | 66.8 ms | 360.1 ms | 56.6 MB/s | 11404 statements | 4.8 ms | 2.7 ms | 0 samples | 8 rolls | 3.0 ms | 3.4 ms | - |
| gb-base | r2 | 121,326 rows/s | 5.6 ms | 107.7 ms | 288.6 ms | 56.6 MB/s | 11150 statements | 5.4 ms | 3.8 ms | 0 samples | 9 rolls | 3.0 ms | 3.5 ms | build |
| gb-base | r3 | 120,936 rows/s | 5.6 ms | 80.1 ms | 289.9 ms | 56.1 MB/s | 11491 statements | 4.5 ms | 850 µs | 0 samples | 8 rolls | 3.3 ms | 3.9 ms | - |
| gb-base | r4 | 122,932 rows/s | 5.6 ms | 54.7 ms | 287.1 ms | 56.1 MB/s | 11668 statements | 4.5 ms | 1.1 ms | 0 samples | 8 rolls | 3.0 ms | 3.4 ms | - |
| gb-base | r5 | 121,845 rows/s | 5.6 ms | 54.5 ms | 286.5 ms | 56.0 MB/s | 11354 statements | 4.5 ms | 949 µs | 0 samples | 8 rolls | 3.3 ms | 3.4 ms | - |
| gb-c2 | r1 | 145,608 rows/s | 6.7 ms | 11.0 ms | 20.9 ms | 67.9 MB/s | 9459 statements | 4.6 ms | 1.5 ms | 0 samples | 10 rolls | 4.7 ms | 5.2 ms | - |
| gb-c2 | r2 | 142,142 rows/s | 6.7 ms | 11.3 ms | 213.7 ms | 66.3 MB/s | 9801 statements | 4.5 ms | 3.6 ms | 0 samples | 10 rolls | 4.6 ms | 8.7 ms | build |
| gb-c2 | r3 | 147,132 rows/s | 6.7 ms | 10.8 ms | 20.2 ms | 68.6 MB/s | 9444 statements | 4.5 ms | 887 µs | 0 samples | 10 rolls | 4.5 ms | 5.1 ms | - |
| gb-c2 | r4 | 146,748 rows/s | 6.7 ms | 11.1 ms | 24.6 ms | 68.5 MB/s | 9543 statements | 4.5 ms | 954 µs | 0 samples | 10 rolls | 4.7 ms | 5.7 ms | - |
| gb-c2 | r5 | 146,073 rows/s | 6.7 ms | 11.2 ms | 14.2 ms | 68.2 MB/s | 9603 statements | 4.5 ms | 1.0 ms | 0 samples | 10 rolls | 4.8 ms | 6.0 ms | - |

#### 3.1 medians of runs
| cell | inserter rate | against `gb-base` | inserter p50 | inserter p99 | inserter max | walker statements | walker p99 | reader p99 | stall samples, all runs | roll hold median, pooled |
|---|---|---|---|---|---|---|---|---|---|---|
| gb-base | 121,469 rows/s | +0.0 % | 5.6 ms | 66.8 ms | 288.6 ms | 11404 statements | 4.5 ms | 1.1 ms | 0 samples | 3.1 ms |
| gb-c2 | 146,073 rows/s | +20.3 % | 6.7 ms | 11.1 ms | 20.9 ms | 9543 statements | 4.5 ms | 1.0 ms | 0 samples | 4.7 ms |

### 4. CZ-S1's cells read for every session, not the reader alone (from CZ-S1's archive, `relaxed`, the inserter closed-loop)

| cell | reader p99 | reader p99.9 | stall samples, 5 runs | inserter rate | inserter p99 | inserter max | walker statements a window | walker p50 | walker p99 |
|---|---|---|---|---|---|---|---|---|---|
| isolated | 1.5 ms | 4.5 ms | 0 samples | 0 rows/s | 0 µs | 0 µs | 0 statements | 0 µs | 0 µs |
| levels | 5.1 ms | 138.8 ms | 174 samples | 158,412 rows/s | 13.5 ms | 639.2 ms | 1051 statements | 6.8 ms | 234.4 ms |
| c1 | 1.4 ms | 4.4 ms | 0 samples | 161,644 rows/s | 13.0 ms | 673.9 ms | 1070 statements | 6.9 ms | 237.5 ms |
| c2 | 3.2 ms | 5.9 ms | 46 samples | 242,968 rows/s | 8.3 ms | 17.2 ms | 1581 statements | 6.4 ms | 15.9 ms |
| c1c2 | 3.4 ms | 6.2 ms | 9 samples | 219,518 rows/s | 9.0 ms | 20.3 ms | 1521 statements | 7.1 ms | 17.2 ms |

### 5. Where the fill's disturbance falls in a segment (the cells with the prewrite and an inserter)

The inserter's statements are of one size, so the statement in the middle of those sent between two rolls marks the half-segment point, where the next body is asked for. A long statement is one of 30 ms or more. `disturbance ends` is the end of the last long statement that began after the half mark and before the roll. Medians over the segments wholly inside a cell's windows.

| cell | segments | half mark to roll | segments with a long statement after the half mark | first long statement after the half mark | disturbance ends after the half mark | roll hold | sync-gate share of the holds | body hit / waited / inline |
|---|---|---|---|---|---|---|---|---|
| gb-base | 41 segments | 773.8 ms | 38 of 41 | 45.3 ms | 416.9 ms | 3.1 ms | 0 % | 41/0/0 |
| c1h | 40 segments | 576.8 ms | 32 of 40 | 341.5 ms | 412.9 ms | 4.1 ms | 60 % | 40/0/0 |
| c1x | 55 segments | 511.6 ms | 55 of 55 | 378.2 ms | 526.9 ms | 39.6 ms | 93 % | 55/0/0 |
| CZ-S1's c1 | 56 segments | 303.7 ms | 55 of 56 | 299.9 ms | 553.1 ms | 251.4 ms | 99 % | 39/17/0 |
| CZ-S1's levels | 56 segments | 305.1 ms | 55 of 56 | 299.3 ms | 555.3 ms | 250.9 ms | 99 % | 39/17/0 |
| gb-c2 (no prewrite) | 50 segments | 481.8 ms | 0 of 50 | - | - | 4.7 ms | 0 % | 50/0/0 |
| c1c2x (no prewrite) | 55 segments | 461.6 ms | 0 of 55 | - | - | 5.3 ms | 1 % | 55/0/0 |

### 6. The held inserter: how far it fell behind, and how much of core 0 it took

A statement's due instant is rebuilt from the first send and the interval; `late` is a send 5 ms or more after it. `service time` is the inserter's summed statement time in the window.

| cell | run | statements kept | sent late | furthest behind | inserter service time a window | walker statements | marked |
|---|---|---|---|---|---|---|---|
| c1h | r1 | 1200 statements | 6 % | 65.0 ms | 5.08 s | 10694 statements | - |
| c1h | r2 | 1200 statements | 7 % | 83.3 ms | 5.20 s | 10269 statements | - |
| c1h | r3 | 1197 statements | 13 % | 168.4 ms | 5.50 s | 9617 statements | build |
| c1h | r4 | 1200 statements | 12 % | 246.7 ms | 5.40 s | 9836 statements | - |
| c1h | r5 | 1200 statements | 8 % | 113.1 ms | 5.16 s | 10461 statements | - |
| c1c2h | r1 | 1200 statements | 0 % | 5.1 ms | 4.60 s | 11806 statements | - |
| c1c2h | r2 | 1200 statements | 0 % | 3.7 ms | 4.65 s | 11735 statements | - |
| c1c2h | r3 | 1200 statements | 10 % | 451.7 ms | 5.73 s | 8826 statements | build |
| c1c2h | r4 | 1200 statements | 0 % | 2.2 ms | 4.61 s | 11764 statements | - |
| c1c2h | r5 | 1200 statements | 0 % | 2.7 ms | 4.62 s | 11688 statements | - |
| c1x | r1 | 1495 statements | 35 % | 137.9 ms | 7.42 s | 5674 statements | - |
| c1x | r2 | 1486 statements | 39 % | 128.8 ms | 7.53 s | 5452 statements | - |
| c1x | r3 | 1487 statements | 34 % | 126.9 ms | 7.37 s | 5945 statements | - |
| c1x | r4 | 1487 statements | 40 % | 226.4 ms | 7.57 s | 5421 statements | - |
| c1x | r5 | 1487 statements | 37 % | 135.1 ms | 7.48 s | 5532 statements | - |
| c1c2x | r1 | 1486 statements | 82 % | 83.3 ms | 9.33 s | 1036 statements | build |
| c1c2x | r2 | 1500 statements | 0 % | 4.5 ms | 5.80 s | 9324 statements | - |
| c1c2x | r3 | 1500 statements | 0 % | 3.8 ms | 5.86 s | 8906 statements | - |
| c1c2x | r4 | 1500 statements | 0 % | 4.3 ms | 5.86 s | 9029 statements | - |
| c1c2x | r5 | 1500 statements | 0 % | 2.9 ms | 5.83 s | 9043 statements | - |
