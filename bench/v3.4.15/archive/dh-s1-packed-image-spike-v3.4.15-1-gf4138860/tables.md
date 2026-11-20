| cell | arm | runs | mean | spread of runs | vs A0 | gap recovered, (arm - B) / (A0 - B) | outside the floor? |
|---|---|---|---|---|---|---|---|
| d5 | A0 | 3,611 stmt/s, 3,498 stmt/s | 3,554 stmt/s | 3.2 % | +0.0 % | - | - |
| d5 | B | 2,577 stmt/s, 2,522 stmt/s | 2,550 stmt/s | 2.2 % | -28.3 % | - | - |
| d5 | P1 | 3,039 stmt/s, 2,950 stmt/s | 2,995 stmt/s | 3.0 % | -15.8 % | 44 % of the gap | yes (delta vs B +17.4 %, floor 3.2 %) |
| d5 | P2 | 3,141 stmt/s, 3,058 stmt/s | 3,099 stmt/s | 2.7 % | -12.8 % | 55 % of the gap | yes (delta vs B +21.6 %, floor 3.2 %) |
| d1 | A0 | 5,006 stmt/s, 4,142 stmt/s, 5,068 stmt/s | 4,739 stmt/s | 19.5 % | +0.0 % | - | - |
| d1 | B | 4,334 stmt/s, 4,505 stmt/s, 4,635 stmt/s | 4,492 stmt/s | 6.7 % | -5.2 % | - | - |
| d1 | P1 | 4,656 stmt/s, 4,634 stmt/s, 4,789 stmt/s | 4,693 stmt/s | 3.3 % | -1.0 % | 82 % of the gap | no (delta vs B +4.5 %, floor 19.5 %) |
| d1 | P2 | 4,741 stmt/s, 4,608 stmt/s, 4,566 stmt/s | 4,638 stmt/s | 3.8 % | -2.1 % | 59 % of the gap | no (delta vs B +3.3 %, floor 19.5 %) |
| d3 | A0 | 8,961 stmt/s | 8,961 stmt/s | single run | +0.0 % | - | - |
| d3 | B | 7,713 stmt/s | 7,713 stmt/s | single run | -13.9 % | - | - |
| d3 | P1 | 8,246 stmt/s | 8,246 stmt/s | single run | -8.0 % | 43 % of the gap | single run (delta vs B +6.9 %, floor 0.0 %) |
| d3 | P2 | 8,129 stmt/s | 8,129 stmt/s | single run | -9.3 % | 33 % of the gap | single run (delta vs B +5.4 %, floor 0.0 %) |
| d7 | A0 | 4,129 stmt/s, 4,150 stmt/s | 4,140 stmt/s | 0.5 % | +0.0 % | - | - |
| d7 | B | 3,899 stmt/s, 3,892 stmt/s | 3,896 stmt/s | 0.2 % | -5.9 % | - | - |
| d7 | P1 | 4,010 stmt/s, 3,994 stmt/s | 4,002 stmt/s | 0.4 % | -3.3 % | 44 % of the gap | yes (delta vs B +2.7 %, floor 0.5 %) |

| run | arm | throughput | p0 | p25 | p50 | p90 | p99 | server CPU | log written | images | wal_flush wait | run_queue wait | on_cpu | page_read wait | ckpt runs (counter) | ckpt pages written | pages drained | bad |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| d5r1 | A0 | 3,611 stmt/s | 541 us | 899 us | 996 us | 1,244 us | 1,963 us | 59.8 us/stmt | 249 B/stmt | 0.000 img/stmt | 505.1 us/stmt | 76.4 us/stmt | 21.2 us/stmt | 3.9 us/stmt | 3 runs | 3,514 pages | 40,054 pages | 0 stmts |
| d5r1 | B | 2,577 stmt/s | 676 us | 1,248 us | 1,368 us | 1,669 us | 2,310 us | 74.5 us/stmt | 6,905 B/stmt | 0.809 img/stmt | 738.8 us/stmt | 69.6 us/stmt | 24.2 us/stmt | 4.0 us/stmt | 3 runs | 3,550 pages | 27,713 pages | 0 stmts |
| d5r1 | P1 | 3,039 stmt/s | 669 us | 1,095 us | 1,205 us | 1,485 us | 2,137 us | 73.3 us/stmt | 2,367 B/stmt | 0.807 img/stmt | 599.4 us/stmt | 62.7 us/stmt | 30.3 us/stmt | 3.9 us/stmt | 3 runs | 3,553 pages | 33,215 pages | 0 stmts |
| d5r1 | P2 | 3,141 stmt/s | 603 us | 1,073 us | 1,188 us | 1,496 us | 2,135 us | 83.8 us/stmt | 1,122 B/stmt | 0.808 img/stmt | 564.5 us/stmt | 71.9 us/stmt | 42.7 us/stmt | 3.9 us/stmt | 3 runs | 3,595 pages | 34,435 pages | 0 stmts |
| d5r2 | A0 | 3,498 stmt/s | 517 us | 903 us | 1,000 us | 1,246 us | 1,974 us | 61.5 us/stmt | 249 B/stmt | 0.000 img/stmt | 517.8 us/stmt | 78.3 us/stmt | 21.6 us/stmt | 3.8 us/stmt | 3 runs | 3,531 pages | 38,733 pages | 0 stmts |
| d5r2 | B | 2,522 stmt/s | 796 us | 1,271 us | 1,398 us | 1,709 us | 2,381 us | 75.9 us/stmt | 6,915 B/stmt | 0.810 img/stmt | 764.4 us/stmt | 69.2 us/stmt | 24.5 us/stmt | 4.1 us/stmt | 3 runs | 3,588 pages | 27,070 pages | 0 stmts |
| d5r2 | P1 | 2,950 stmt/s | 660 us | 1,112 us | 1,224 us | 1,521 us | 2,295 us | 74.1 us/stmt | 2,371 B/stmt | 0.808 img/stmt | 618.8 us/stmt | 66.8 us/stmt | 30.4 us/stmt | 3.9 us/stmt | 3 runs | 3,584 pages | 32,165 pages | 0 stmts |
| d5r2 | P2 | 3,058 stmt/s | 553 us | 1,111 us | 1,222 us | 1,515 us | 2,182 us | 84.6 us/stmt | 1,122 B/stmt | 0.808 img/stmt | 576.7 us/stmt | 70.3 us/stmt | 43.4 us/stmt | 3.9 us/stmt | 3 runs | 3,579 pages | 33,478 pages | 0 stmts |
| d1r1 | A0 | 5,006 stmt/s | 172 us | 675 us | 762 us | 971 us | 1,360 us | 87.0 us/stmt | 250 B/stmt | 0.000 img/stmt | 667.7 us/stmt | 1.0 us/stmt | 30.1 us/stmt | 0.0 us/stmt | 11 runs | 7,848 pages | 0 pages | 1 stmts |
| d1r1 | B | 4,334 stmt/s | 200 us | 754 us | 866 us | 1,144 us | 1,745 us | 106.3 us/stmt | 1,202 B/stmt | 0.116 img/stmt | 775.4 us/stmt | 1.0 us/stmt | 35.5 us/stmt | 0.0 us/stmt | 11 runs | 7,648 pages | 0 pages | 1 stmts |
| d1r1 | P1 | 4,656 stmt/s | 406 us | 712 us | 813 us | 1,072 us | 1,581 us | 103.4 us/stmt | 625 B/stmt | 0.120 img/stmt | 705.4 us/stmt | 1.0 us/stmt | 45.0 us/stmt | 0.0 us/stmt | 11 runs | 8,492 pages | 0 pages | 0 stmts |
| d1r1 | P2 | 4,741 stmt/s | 353 us | 713 us | 811 us | 1,049 us | 1,427 us | 104.9 us/stmt | 443 B/stmt | 0.109 img/stmt | 695.6 us/stmt | 1.1 us/stmt | 41.4 us/stmt | 0.0 us/stmt | 11 runs | 7,901 pages | 0 pages | 0 stmts |
| d1r2 | A0 | 4,142 stmt/s | 355 us | 675 us | 757 us | 963 us | 1,281 us | 99.6 us/stmt | 249 B/stmt | 0.000 img/stmt | 828.2 us/stmt | 0.8 us/stmt | 31.5 us/stmt | 0.0 us/stmt | 7 runs | 4,947 pages | 0 pages | 0 stmts |
| d1r2 | B | 4,505 stmt/s | 375 us | 730 us | 833 us | 1,089 us | 1,506 us | 106.8 us/stmt | 1,196 B/stmt | 0.115 img/stmt | 738.8 us/stmt | 1.0 us/stmt | 40.2 us/stmt | 0.0 us/stmt | 11 runs | 7,935 pages | 0 pages | 0 stmts |
| d1r2 | P1 | 4,634 stmt/s | 373 us | 706 us | 811 us | 1,071 us | 1,572 us | 106.9 us/stmt | 615 B/stmt | 0.117 img/stmt | 716.5 us/stmt | 0.8 us/stmt | 46.1 us/stmt | 0.0 us/stmt | 11 runs | 8,225 pages | 0 pages | 0 stmts |
| d1r2 | P2 | 4,608 stmt/s | 414 us | 736 us | 834 us | 1,080 us | 1,570 us | 104.5 us/stmt | 452 B/stmt | 0.114 img/stmt | 721.6 us/stmt | 1.0 us/stmt | 38.2 us/stmt | 0.0 us/stmt | 11 runs | 8,012 pages | 0 pages | 0 stmts |
| d3 | A0 | 8,961 stmt/s | 388 us | 755 us | 858 us | 1,111 us | 1,624 us | 96.3 us/stmt | 250 B/stmt | 0.000 img/stmt | 714.4 us/stmt | 1.3 us/stmt | 36.4 us/stmt | 0.0 us/stmt | 21 runs | 16,156 pages | 0 pages | 0 stmts |
| d3 | B | 7,713 stmt/s | 142 us | 816 us | 945 us | 1,309 us | 1,860 us | 106.6 us/stmt | 1,381 B/stmt | 0.138 img/stmt | 842.8 us/stmt | 1.1 us/stmt | 42.4 us/stmt | 0.0 us/stmt | 21 runs | 16,198 pages | 0 pages | 1 stmts |
| d3 | P1 | 8,246 stmt/s | 153 us | 761 us | 883 us | 1,186 us | 1,819 us | 107.1 us/stmt | 657 B/stmt | 0.130 img/stmt | 779.4 us/stmt | 1.2 us/stmt | 42.7 us/stmt | 0.0 us/stmt | 21 runs | 16,288 pages | 0 pages | 1 stmts |
| d3 | P2 | 8,129 stmt/s | 404 us | 804 us | 930 us | 1,234 us | 1,743 us | 112.2 us/stmt | 483 B/stmt | 0.132 img/stmt | 782.9 us/stmt | 1.0 us/stmt | 51.0 us/stmt | 0.0 us/stmt | 21 runs | 16,342 pages | 0 pages | 0 stmts |
| d7r1 | A0 | 4,129 stmt/s | 467 us | 842 us | 945 us | 1,181 us | 1,523 us | 49.6 us/stmt | 249 B/stmt | 0.000 img/stmt | 440.6 us/stmt | 38.3 us/stmt | 19.0 us/stmt | 0.0 us/stmt | 3 runs | 1,860 pages | 0 pages | 0 stmts |
| d7r1 | B | 3,899 stmt/s | 516 us | 897 us | 1,001 us | 1,249 us | 1,615 us | 49.9 us/stmt | 510 B/stmt | 0.032 img/stmt | 469.3 us/stmt | 37.7 us/stmt | 18.7 us/stmt | 0.0 us/stmt | 3 runs | 1,862 pages | 0 pages | 0 stmts |
| d7r1 | P1 | 4,010 stmt/s | 493 us | 867 us | 970 us | 1,224 us | 1,572 us | 49.9 us/stmt | 345 B/stmt | 0.031 img/stmt | 455.7 us/stmt | 38.3 us/stmt | 19.1 us/stmt | 0.0 us/stmt | 3 runs | 1,856 pages | 0 pages | 0 stmts |
| d7r2 | A0 | 4,150 stmt/s | 506 us | 842 us | 942 us | 1,170 us | 1,463 us | 48.8 us/stmt | 249 B/stmt | 0.000 img/stmt | 442.0 us/stmt | 38.4 us/stmt | 18.6 us/stmt | 0.0 us/stmt | 3 runs | 1,866 pages | 0 pages | 0 stmts |
| d7r2 | B | 3,892 stmt/s | 480 us | 894 us | 999 us | 1,256 us | 1,646 us | 49.8 us/stmt | 511 B/stmt | 0.032 img/stmt | 475.2 us/stmt | 38.4 us/stmt | 18.5 us/stmt | 0.0 us/stmt | 3 runs | 1,865 pages | 0 pages | 0 stmts |
| d7r2 | P1 | 3,994 stmt/s | 468 us | 871 us | 978 us | 1,228 us | 1,575 us | 50.2 us/stmt | 346 B/stmt | 0.031 img/stmt | 461.4 us/stmt | 39.4 us/stmt | 19.1 us/stmt | 0.0 us/stmt | 3 runs | 1,855 pages | 0 pages | 0 stmts |
| d1r3 | A0 | 5,068 stmt/s | 387 us | 666 us | 757 us | 974 us | 1,334 us | 107.5 us/stmt | 250 B/stmt | 0.000 img/stmt | 635.2 us/stmt | 0.9 us/stmt | 41.5 us/stmt | 0.0 us/stmt | 11 runs | 8,121 pages | 0 pages | 0 stmts |
| d1r3 | B | 4,635 stmt/s | 369 us | 708 us | 816 us | 1,072 us | 1,401 us | 112.9 us/stmt | 1,191 B/stmt | 0.114 img/stmt | 707.6 us/stmt | 0.9 us/stmt | 41.0 us/stmt | 0.0 us/stmt | 11 runs | 8,111 pages | 0 pages | 0 stmts |
| d1r3 | P1 | 4,789 stmt/s | 374 us | 709 us | 808 us | 1,033 us | 1,338 us | 116.0 us/stmt | 596 B/stmt | 0.111 img/stmt | 677.9 us/stmt | 0.8 us/stmt | 42.4 us/stmt | 0.0 us/stmt | 11 runs | 8,058 pages | 0 pages | 0 stmts |
| d1r3 | P2 | 4,566 stmt/s | 369 us | 730 us | 828 us | 1,071 us | 1,472 us | 116.9 us/stmt | 454 B/stmt | 0.115 img/stmt | 713.3 us/stmt | 3.9 us/stmt | 45.6 us/stmt | 0.0 us/stmt | 11 runs | 8,049 pages | 0 pages | 0 stmts |
