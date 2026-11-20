### A against B

| cell | A | B | B vs A | floor (larger of A's and B's spread) | outside the floor? |
|---|---|---|---|---|---|
| m1: update 100K, 4 cores x 4 clients, group | 4,604 stmt/s (4,550; 4,659) | 4,888 stmt/s (4,939; 4,837) | +6.2 % | 2.4 % | yes, B faster |
| m2: update 100K, 8 cores x 8 clients, group | 7,595 stmt/s (7,516; 7,675) | 8,747 stmt/s (8,652; 8,841) | +15.2 % | 2.2 % | yes, B faster |
| m4: update 100K, 1 core x 4, group | 3,906 stmt/s (3,746; 4,065) | 3,945 stmt/s (3,914; 3,976) | +1.0 % | 8.2 % | no |
| m3: update 1M, pool 2,048 frames, 1 core x 4, group | 2,515 stmt/s (2,531; 2,500) | 2,887 stmt/s (2,843; 2,930) | +14.8 % | 3.0 % | yes, B faster |
| k3: update 10K, 1 core x 4 | 4,205 stmt/s (4,205) | 4,187 stmt/s (4,187) | -0.4 % | - | single run, no floor |

### Attribution (cells with all four arms)

| cell | A | B3 (L3 alone) | B (L3 + L2) | A0 (bound) | gap A0 vs A | gap recovered by B | by L3 (B3) | by L2 on top (B - B3) | floor |
|---|---|---|---|---|---|---|---|---|---|
| m1: update 100K, 4 cores x 4 clients, group | 4,604 stmt/s | 4,682 stmt/s | 4,888 stmt/s | 4,990 stmt/s | +8.4 % (+386 stmt/s) | 74 % of the gap | 20 % of the gap | 54 % of the gap | 2.4 % |
| m2: update 100K, 8 cores x 8 clients, group | 7,595 stmt/s | 8,297 stmt/s | 8,747 stmt/s | 8,789 stmt/s | +15.7 % (+1,194 stmt/s) | 96 % of the gap | 59 % of the gap | 38 % of the gap | 3.3 % |
| m3: update 1M, pool 2,048 frames, 1 core x 4, group | 2,515 stmt/s | 2,547 stmt/s | 2,887 stmt/s | 3,585 stmt/s | +42.5 % (+1,070 stmt/s) | 35 % of the gap | 3 % of the gap | 32 % of the gap | 3.0 % |

### Every micro run

| run | arm | throughput | p0 | p25 | p50 | p90 | p99 | server CPU | log written | images | image bytes | ckpt runs (counter) | ckpt runs by core | ckpt pages written | pages drained | wal_flush wait | run_queue wait | on_cpu | page_read wait | commits per sync | bad | hold / attempt |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| m1r1 | A | 4,550 stmt/s | 413 us | 723 us | 828 us | 1,088 us | 1,473 us | 108.9 us/stmt | 1,160 B/stmt | 0.111 img/stmt | 910 B/stmt (count x 8,224) | 11 runs | c0:3, c1:3, c2:3, c3:2 | 7,680 pages | 0 pages | 728.1 us/stmt | 0.9 us/stmt | 36.2 us/stmt | 0.0 us/stmt | 1.5 commits/sync | 0 stmts | 978 s / attempt 3 |
| m1r1 | B | 4,939 stmt/s | 399 us | 701 us | 790 us | 993 us | 1,285 us | 93.4 us/stmt | 348 B/stmt | 0.029 img/stmt | 99 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,238 pages | 0 pages | 671.1 us/stmt | 1.0 us/stmt | 29.7 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m1r1 | B3 | 4,628 stmt/s | 411 us | 745 us | 845 us | 1,068 us | 1,372 us | 109.5 us/stmt | 513 B/stmt | 0.032 img/stmt | 264 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,275 pages | 0 pages | 714.4 us/stmt | 0.7 us/stmt | 35.7 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m1r2 | A | 4,659 stmt/s | 363 us | 706 us | 809 us | 1,054 us | 1,404 us | 109.9 us/stmt | 1,166 B/stmt | 0.111 img/stmt | 916 B/stmt (count x 8,224) | 11 runs | c0:3, c1:3, c2:3, c3:2 | 7,930 pages | 0 pages | 708.8 us/stmt | 0.9 us/stmt | 40.1 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 678 s / attempt 2 |
| m1r2 | B | 4,837 stmt/s | 368 us | 703 us | 801 us | 1,032 us | 1,370 us | 101.1 us/stmt | 351 B/stmt | 0.030 img/stmt | 102 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,225 pages | 0 pages | 684.3 us/stmt | 0.9 us/stmt | 32.1 us/stmt | 0.0 us/stmt | 2.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m1r2 | B3 | 4,736 stmt/s | 335 us | 725 us | 817 us | 1,034 us | 1,389 us | 94.2 us/stmt | 523 B/stmt | 0.033 img/stmt | 274 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,408 pages | 0 pages | 705.8 us/stmt | 1.0 us/stmt | 31.4 us/stmt | 0.0 us/stmt | 3.8 commits/sync | 0 stmts | 287 s / attempt 3 |
| m1r2 | A0 | 4,956 stmt/s | 381 us | 687 us | 774 us | 987 us | 1,451 us | 91.7 us/stmt | 249 B/stmt | 0.000 img/stmt | 0 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,235 pages | 0 pages | 670.2 us/stmt | 1.1 us/stmt | 30.9 us/stmt | 0.0 us/stmt | - | 0 stmts | 0 s / attempt 1 |
| m2r1 | A | 7,516 stmt/s | 384 us | 835 us | 967 us | 1,324 us | 2,010 us | 106.5 us/stmt | 1,402 B/stmt | 0.140 img/stmt | 1,152 B/stmt (count x 8,224) | 21 runs | c0:3, c1:3, c2:3, c3:3, c4:3, c5:2, c6:2, c7:2 | 16,049 pages | 0 pages | 871.6 us/stmt | 1.1 us/stmt | 41.9 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m2r1 | B | 8,652 stmt/s | 414 us | 780 us | 885 us | 1,141 us | 1,716 us | 94.0 us/stmt | 324 B/stmt | 0.021 img/stmt | 75 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,828 pages | 0 pages | 745.1 us/stmt | 1.5 us/stmt | 34.8 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m2r1 | B3 | 8,296 stmt/s | 407 us | 800 us | 907 us | 1,172 us | 1,773 us | 99.0 us/stmt | 422 B/stmt | 0.021 img/stmt | 173 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,775 pages | 0 pages | 777.9 us/stmt | 1.3 us/stmt | 38.7 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m2r1 | A0 | 8,644 stmt/s | 150 us | 780 us | 881 us | 1,125 us | 1,844 us | 101.6 us/stmt | 249 B/stmt | 0.000 img/stmt | 0 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,736 pages | 0 pages | 737.5 us/stmt | 0.9 us/stmt | 39.4 us/stmt | 0.0 us/stmt | 1.6 commits/sync | 2 stmts | 0 s / attempt 1 |
| m2r2 | A | 7,675 stmt/s | 416 us | 819 us | 946 us | 1,298 us | 1,977 us | 106.6 us/stmt | 1,385 B/stmt | 0.138 img/stmt | 1,135 B/stmt (count x 8,224) | 21 runs | c0:3, c1:3, c2:3, c3:3, c4:3, c5:2, c6:2, c7:2 | 16,167 pages | 0 pages | 849.1 us/stmt | 0.9 us/stmt | 42.5 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m2r2 | B | 8,841 stmt/s | 386 us | 768 us | 871 us | 1,115 us | 1,561 us | 87.4 us/stmt | 319 B/stmt | 0.019 img/stmt | 70 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,677 pages | 0 pages | 731.1 us/stmt | 1.6 us/stmt | 31.4 us/stmt | 0.0 us/stmt | - | 0 stmts | 0 s / attempt 1 |
| m2r2 | B3 | 8,297 stmt/s | 420 us | 795 us | 899 us | 1,149 us | 2,154 us | 100.7 us/stmt | 417 B/stmt | 0.020 img/stmt | 169 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,634 pages | 0 pages | 777.9 us/stmt | 1.6 us/stmt | 37.1 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m2r2 | A0 | 8,934 stmt/s | 358 us | 763 us | 862 us | 1,093 us | 1,569 us | 97.4 us/stmt | 249 B/stmt | 0.000 img/stmt | 0 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,831 pages | 0 pages | 711.6 us/stmt | 1.0 us/stmt | 37.9 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 348 s / attempt 2 |
| m1r1 | A0 | 5,024 stmt/s | 393 us | 685 us | 771 us | 977 us | 1,290 us | 109.3 us/stmt | 249 B/stmt | 0.000 img/stmt | 0 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,258 pages | 0 pages | 647.4 us/stmt | 0.7 us/stmt | 36.3 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 1,953 s / attempt 2 |
| m4r1 | A | 3,746 stmt/s | 472 us | 883 us | 995 us | 1,253 us | 1,601 us | 51.4 us/stmt | 522 B/stmt | 0.033 img/stmt | 273 B/stmt (count x 8,224) | 3 runs | c0:3 | 1,869 pages | 0 pages | 495.4 us/stmt | 39.3 us/stmt | 19.2 us/stmt | 0.0 us/stmt | 2.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m4r1 | B | 3,914 stmt/s | 453 us | 851 us | 953 us | 1,190 us | 1,497 us | 50.1 us/stmt | 354 B/stmt | 0.032 img/stmt | 105 B/stmt | 3 runs | c0:3 | 1,856 pages | 0 pages | 470.5 us/stmt | 38.9 us/stmt | 19.2 us/stmt | 0.0 us/stmt | 2.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m4r2 | A | 4,065 stmt/s | 498 us | 854 us | 959 us | 1,205 us | 1,528 us | 48.0 us/stmt | 499 B/stmt | 0.030 img/stmt | 250 B/stmt (count x 8,224) | 3 runs | c0:3 | 1,860 pages | 0 pages | 451.8 us/stmt | 36.8 us/stmt | 18.1 us/stmt | 0.0 us/stmt | 2.0 commits/sync | 0 stmts | 508 s / attempt 2 |
| m4r2 | B | 3,976 stmt/s | 509 us | 866 us | 978 us | 1,237 us | 1,623 us | 53.2 us/stmt | 353 B/stmt | 0.031 img/stmt | 104 B/stmt | 3 runs | c0:3 | 1,862 pages | 0 pages | 458.9 us/stmt | 41.0 us/stmt | 20.4 us/stmt | 0.0 us/stmt | 2.0 commits/sync | 0 stmts | 399 s / attempt 2 |
| m3r1 | A | 2,531 stmt/s | 775 us | 1,259 us | 1,390 us | 1,708 us | 2,415 us | 76.1 us/stmt | 6,902 B/stmt | 0.809 img/stmt | 6,652 B/stmt (count x 8,224) | 3 runs | c0:3 | 3,558 pages | 27,156 pages | 737.8 us/stmt | 68.8 us/stmt | 24.6 us/stmt | 4.1 us/stmt | 2.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m3r1 | B | 2,843 stmt/s | 679 us | 1,167 us | 1,289 us | 1,592 us | 2,211 us | 76.2 us/stmt | 3,080 B/stmt | 0.808 img/stmt | 2,830 B/stmt | 3 runs | c0:3 | 3,550 pages | 30,925 pages | 636.1 us/stmt | 77.4 us/stmt | 29.6 us/stmt | 4.0 us/stmt | 2.0 commits/sync | 0 stmts | 636 s / attempt 2 |
| m3r1 | B3 | 2,547 stmt/s | 772 us | 1,263 us | 1,385 us | 1,696 us | 2,365 us | 77.2 us/stmt | 6,909 B/stmt | 0.810 img/stmt | 6,659 B/stmt | 3 runs | c0:3 | 3,561 pages | 27,342 pages | 762.2 us/stmt | 65.7 us/stmt | 24.8 us/stmt | 4.1 us/stmt | 2.0 commits/sync | 0 stmts | 678 s / attempt 2 |
| m3r1 | A0 | 3,585 stmt/s | 495 us | 927 us | 1,037 us | 1,304 us | 2,027 us | 62.3 us/stmt | 249 B/stmt | 0.000 img/stmt | 0 B/stmt | 3 runs | c0:3 | 3,617 pages | 39,772 pages | 508.7 us/stmt | 69.8 us/stmt | 21.8 us/stmt | 3.9 us/stmt | 2.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m3r2 | A | 2,500 stmt/s | 803 us | 1,272 us | 1,396 us | 1,709 us | 2,555 us | 74.9 us/stmt | 6,917 B/stmt | 0.811 img/stmt | 6,667 B/stmt (count x 8,224) | 3 runs | c0:3 | 3,596 pages | 26,807 pages | 738.5 us/stmt | 63.9 us/stmt | 23.8 us/stmt | 4.0 us/stmt | 2.0 commits/sync | 0 stmts | 384 s / attempt 2 |
| m3r2 | B | 2,930 stmt/s | 605 us | 1,134 us | 1,249 us | 1,540 us | 2,178 us | 74.6 us/stmt | 3,076 B/stmt | 0.807 img/stmt | 2,827 B/stmt | 3 runs | c0:3 | 3,531 pages | 31,938 pages | 640.5 us/stmt | 66.9 us/stmt | 28.7 us/stmt | 4.0 us/stmt | 2.0 commits/sync | 0 stmts | 614 s / attempt 2 |
| k3r1 | A | 4,205 stmt/s | 329 us | 835 us | 931 us | 1,151 us | 1,419 us | 47.6 us/stmt | 275 B/stmt | 0.003 img/stmt | 27 B/stmt (count x 8,224) | 3 runs | c0:3 | 207 pages | 0 pages | 434.6 us/stmt | 36.6 us/stmt | 17.8 us/stmt | 0.0 us/stmt | 2.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| k3r1 | B | 4,187 stmt/s | 460 us | 837 us | 935 us | 1,159 us | 1,421 us | 47.9 us/stmt | 261 B/stmt | 0.003 img/stmt | 12 B/stmt | 3 runs | c0:3 | 207 pages | 0 pages | 438.6 us/stmt | 37.2 us/stmt | 17.8 us/stmt | 0.0 us/stmt | 2.0 commits/sync | 0 stmts | 0 s / attempt 1 |

### Scenario 2 runs

| run | arm | cores x bookers | throughput | booking p0 | p25 | p50 | p95 | p99 | commit p0 | p25 | p50 | p95 | p99 | log written | images | image bytes | ckpt runs (counter) | verify failures | committed | hold / attempt |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|

### Recovery and log kept (8 cores x 8 clients, update 100K, group, 50 s then kill -9)

| run | arm | throughput | log written in the window | images | image bytes | ckpt runs (counter) | ckpt rows by core | WAL files at the kill | WAL directory at the kill | first segment | mount (exec to first connection) | rows after | recovery records | redo applied | pages healed | recovery analysis / redo / undo | hold / attempt |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| rec1 | A | 7,266 stmt/s | 544.8 MB | 55,196 images | 453.9 MB | 72 runs | c0:9, c1:9, c2:9, c3:9, c4:9, c5:9, c6:9, c7:9 | 3 files | 201.3 MB | 7 | 0.19 s | 100000 rows | 161207 records | 0 pages | 0 pages | 52372 / 56739 / 0 us | 0 s / attempt 1 |
| rec1 | B | 8,067 stmt/s | 136.6 MB | 9,177 images | 36.3 MB | 9 runs | c0:1, c1:1, c2:1, c3:1, c4:1, c5:2, c6:1, c7:1 | 4 files | 268.4 MB | 0 | 0.72 s | 100000 rows | 1456727 records | 28691 pages | 0 pages | 291197 / 281853 / 0 us | 0 s / attempt 1 |
