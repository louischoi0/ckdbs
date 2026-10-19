
#### cell 4: PURGE DROPPED COLUMNS FROM p (binary B), by relation size; wall is the client's, one statement

| rows | rep | load s | dirty pass wall | rows/s (dirty) | ns/row | server CPU s | WAL bytes | WAL bytes/row | rows cleaned | spills released | clean re-run wall | rows/s (all clean) | ns/row | WAL bytes (re-run) | reusing ADD us | host clean |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 200 | 0 | 0.0 | 0.28 ms | 722,551 | 1384 | 0.000 | 30,800 | 154.0 | 200 | 200 | 0.141 ms | 1,422,860 | 703 | 64 | 116 | False |
| 200 | 1 | 0.0 | 0.27 ms | 741,215 | 1349 | 0.000 | 30,800 | 154.0 | 200 | 200 | 0.133 ms | 1,502,733 | 665 | 64 | 109 | True |
| 1,000 | 0 | 0.0 | 0.73 ms | 1,374,555 | 728 | 0.000 | 152,400 | 152.4 | 1000 | 1000 | 0.200 ms | 5,003,976 | 200 | 64 | 120 | False |
| 1,000 | 1 | 0.0 | 0.72 ms | 1,392,715 | 718 | 0.000 | 152,400 | 152.4 | 1000 | 1000 | 0.174 ms | 5,736,114 | 174 | 64 | 108 | True |
| 10,000 | 0 | 0.0 | 6.19 ms | 1,615,102 | 619 | 0.006 | 1,520,400 | 152.0 | 10000 | 10000 | 0.408 ms | 24,491,434 | 41 | 64 | 109 | False |
| 10,000 | 1 | 0.0 | 6.01 ms | 1,664,977 | 601 | 0.006 | 1,520,400 | 152.0 | 10000 | 10000 | 0.412 ms | 24,242,903 | 41 | 64 | 105 | False |
| 100,000 | 0 | 0.8 | 64.04 ms | 1,561,489 | 640 | 0.063 | 15,200,400 | 152.0 | 100000 | 100000 | 2.881 ms | 34,706,784 | 29 | 64 | 112 | True |
| 100,000 | 1 | 0.8 | 60.84 ms | 1,643,689 | 608 | 0.061 | 15,200,400 | 152.0 | 100000 | 100000 | 2.940 ms | 34,010,031 | 29 | 64 | 122 | True |
| 1,000,000 | 0 | 9.2 | 1301.67 ms | 768,241 | 1302 | 0.693 | 152,169,776 | 152.2 | 1000000 | 1000000 | 27.297 ms | 36,633,446 | 27 | 64 | 125 | False |
| 1,000,000 | 1 | 9.1 | 1300.63 ms | 768,858 | 1301 | 0.666 | 152,008,672 | 152.0 | 1000000 | 1000000 | 27.513 ms | 36,346,040 | 28 | 64 | 123 | True |

reply tags (rep 0):
- 200: dirty `PURGED DROPPED COLUMNS p: 1 column(s) clean, 200 row(s) cleaned, 200 spill(s) released`; re-run `PURGED DROPPED COLUMNS p: 1 column(s) clean, 0 row(s) cleaned, 0 spill(s) released`; ADD `ADDED COLUMN p.again`; rows reading NULL in the reused column: 200
- 1,000: dirty `PURGED DROPPED COLUMNS p: 1 column(s) clean, 1000 row(s) cleaned, 1000 spill(s) released`; re-run `PURGED DROPPED COLUMNS p: 1 column(s) clean, 0 row(s) cleaned, 0 spill(s) released`; ADD `ADDED COLUMN p.again`; rows reading NULL in the reused column: 1000
- 10,000: dirty `PURGED DROPPED COLUMNS p: 1 column(s) clean, 10000 row(s) cleaned, 10000 spill(s) released`; re-run `PURGED DROPPED COLUMNS p: 1 column(s) clean, 0 row(s) cleaned, 0 spill(s) released`; ADD `ADDED COLUMN p.again`; rows reading NULL in the reused column: 10000
- 100,000: dirty `PURGED DROPPED COLUMNS p: 1 column(s) clean, 100000 row(s) cleaned, 100000 spill(s) released`; re-run `PURGED DROPPED COLUMNS p: 1 column(s) clean, 0 row(s) cleaned, 0 spill(s) released`; ADD `ADDED COLUMN p.again`; rows reading NULL in the reused column: 100000
- 1,000,000: dirty `PURGED DROPPED COLUMNS p: 1 column(s) clean, 1000000 row(s) cleaned, 1000000 spill(s) released`; re-run `PURGED DROPPED COLUMNS p: 1 column(s) clean, 0 row(s) cleaned, 0 spill(s) released`; ADD `ADDED COLUMN p.again`; rows reading NULL in the reused column: 1000000

#### cell 5: serial autocommit point-UPDATE stream (1 connection, literal) around PURGE DROPPED COLUMNS on a 1,000,000-row relation; latency us

| rep | window | ops | stmts/s | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|---|
| 0 | pre (3 s) | 59,256 | 19,752 | 37.7 | 46.5 | 47.0 | 59.8 | 72.0 | 5151 |
| 0 | during pass | 3 |  | 1057.2 | 1057.2 | 43387.8 | 1773101.4 | 1773101.4 | 1773101 |
| 0 | post (3 s) | 42,335 | 14,112 | 37.2 | 46.8 | 47.4 | 61.8 | 82.9 | 597087 |
| 0 | whole run | 101,594 |  | 37.2 | 46.6 | 47.2 | 60.7 | 74.5 | 1773101 |
| 1 | pre (3 s) | 58,185 | 19,395 | 37.9 | 47.7 | 48.2 | 60.9 | 73.4 | 5402 |
| 1 | during pass | 3 |  | 56.8 | 56.8 | 971.1 | 1848976.0 | 1848976.0 | 1848976 |
| 1 | post (3 s) | 45,511 | 15,170 | 38.3 | 47.8 | 48.4 | 62.7 | 85.0 | 344726 |
| 1 | whole run | 103,699 |  | 37.9 | 47.8 | 48.3 | 61.8 | 76.1 | 1848976 |
| 2 | pre (3 s) | 59,081 | 19,694 | 38.0 | 47.1 | 47.6 | 60.4 | 72.0 | 6666 |
| 2 | during pass | 4 |  | 52.1 | 52.1 | 54.8 | 1817746.9 | 1817746.9 | 1817747 |
| 2 | post (3 s) | 46,246 | 15,415 | 37.6 | 47.1 | 47.7 | 61.6 | 80.3 | 554365 |
| 2 | whole run | 105,331 |  | 37.6 | 47.1 | 47.6 | 60.9 | 74.3 | 1817747 |

| rep | pass wall | pass server CPU | pass reply | re-run reply | top-5 stream latencies, ms | bad replies |
|---|---|---|---|---|---|---|
| 0 | 1800.9 ms | 662.7 ms | `PURGED DROPPED COLUMNS p: 1 column(s) clean, 1000000 row(s) cleaned, 1000000 spill(s) released` | `PURGED DROPPED COLUMNS p: 1 column(s) clean, 0 row(s) cleaned, 0 spill(s) released` | [43.388, 80.077, 83.184, 597.087, 1773.101] | 0 |
| 1 | 1848.7 ms | 670.0 ms | `PURGED DROPPED COLUMNS p: 1 column(s) clean, 1000000 row(s) cleaned, 1000000 spill(s) released` | `PURGED DROPPED COLUMNS p: 1 column(s) clean, 0 row(s) cleaned, 0 spill(s) released` | [45.79, 89.52, 94.365, 344.726, 1848.976] | 0 |
| 2 | 1781.7 ms | 662.8 ms | `PURGED DROPPED COLUMNS p: 1 column(s) clean, 1000000 row(s) cleaned, 1000000 spill(s) released` | `PURGED DROPPED COLUMNS p: 1 column(s) clean, 0 row(s) cleaned, 0 spill(s) released` | [6.666, 11.295, 11.48, 554.365, 1817.747] | 0 |

#### cell 5 control: the same stream and windows, PURGE not sent (a 2.0 s idle gap in its place); latency us

| rep | window | ops | stmts/s | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|---|
| 0 | pre (3 s) | 57,327 | 19,109 | 39.8 | 47.6 | 48.1 | 61.3 | 74.4 | 48336 |
| 0 | gap (2 s) | 21,996 |  | 40.9 | 47.4 | 48.1 | 62.7 | 184.6 | 701030 |
| 0 | post (3 s) | 57,549 | 19,183 | 38.2 | 47.5 | 48.0 | 59.8 | 71.4 | 53943 |
| 0 | whole run | 136,872 |  | 38.2 | 47.5 | 48.1 | 60.9 | 75.2 | 701030 |
| 1 | pre (3 s) | 58,904 | 19,635 | 37.6 | 46.8 | 47.3 | 60.7 | 73.8 | 4310 |
| 1 | gap (2 s) | 22,334 |  | 36.9 | 46.9 | 47.6 | 61.1 | 124.2 | 600193 |
| 1 | post (3 s) | 59,546 | 19,849 | 39.1 | 46.7 | 47.2 | 59.0 | 70.6 | 4832 |
| 1 | whole run | 140,784 |  | 36.9 | 46.8 | 47.3 | 60.1 | 74.0 | 600193 |
| 2 | pre (3 s) | 54,585 | 18,195 | 41.4 | 47.8 | 48.4 | 64.3 | 75.9 | 8837 |
| 2 | gap (2 s) | 22,732 |  | 41.8 | 48.0 | 48.6 | 63.1 | 188.6 | 397042 |
| 2 | post (3 s) | 58,419 | 19,473 | 38.1 | 47.7 | 48.2 | 59.5 | 70.7 | 4317 |
| 2 | whole run | 135,736 |  | 38.1 | 47.8 | 48.3 | 61.6 | 75.9 | 397042 |

#### cell 5 (pass): stream statements over 5 ms, [offset from the pass start (or the gap start) s, latency ms]

- rep 0: [[-1.915, 5.2], [-0.015, 43.4], [0.029, 1773.1], [1.804, 10.0], [2.035, 80.1], [2.335, 83.2], [2.635, 597.1], [4.485, 6.7]]
- rep 1: [[-1.021, 5.4], [0.001, 1849.0], [1.879, 89.5], [2.229, 45.8], [2.479, 94.4], [2.829, 344.7], [4.829, 6.6]]
- rep 2: [[-1.041, 6.7], [0.001, 1817.7], [2.009, 5.6], [2.259, 11.3], [2.459, 11.5], [2.709, 554.4]]

#### cell 5 (control): stream statements over 5 ms, [offset from the pass start (or the gap start) s, latency ms]

- rep 0: [[-0.194, 48.3], [0.106, 42.7], [0.305, 7.2], [0.356, 7.8], [0.606, 56.1], [0.856, 701.0], [3.172, 5.9], [4.956, 53.9]]
- rep 1: [[0.018, 52.8], [0.268, 55.4], [0.568, 103.4], [0.868, 600.2]]
- rep 2: [[-1.189, 8.8], [-0.989, 5.3], [-0.138, 152.5], [0.261, 60.1], [0.761, 397.0], [1.361, 300.2]]

#### cell 4: SHOW META deltas around the dirty pass (the waits)

| rows | rep | wall ms | server CPU ms | WAL segment rolls | roll hold ms (fsync of the segment left, under the latch) | ring-full waits | longest non-yielding run ms | pool hits |
|---|---|---|---|---|---|---|---|---|
| 200 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1,020 |
| 200 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 1,020 |
| 1,000 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 5,023 |
| 1,000 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 5,023 |
| 10,000 | 0 | 6 | 6 | 0 | 0 | 1 | 3 | 50,177 |
| 10,000 | 1 | 6 | 6 | 0 | 0 | 1 | 2 | 50,177 |
| 100,000 | 0 | 64 | 63 | 0 | 0 | 14 | 0 | 601,801 |
| 100,000 | 1 | 61 | 61 | 0 | 0 | 14 | 0 | 601,801 |
| 1,000,000 | 0 | 1302 | 693 | 2 | 626 | 144 | 911 | 6,017,933 |
| 1,000,000 | 1 | 1301 | 666 | 2 | 642 | 144 | 916 | 6,017,933 |

#### cell 5 (pass): SHOW META deltas over the pass

| rep | wall ms | WAL rolls | roll hold ms | ring-full | longest non-yielding run ms |
|---|---|---|---|---|---|
| 0 | 1801 | 3 | 1119 | 144 | 1395 |
| 1 | 1849 | 3 | 1189 | 144 | 1282 |
| 2 | 1782 | 3 | 1129 | 144 | 1423 |

#### cell 6: ADD COLUMN, wall us, 100 trials each (3-row relations, interleaved)

| ADD kind | n | p0 | p25 | p50 | p95 | p99 | max | statements/s at mean (derived) | server CPU us (median) |
|---|---|---|---|---|---|---|---|---|---|
| vacant slot | 100 | 70.1 | 101.2 | 131.3 | 190.3 | 202.2 | 215.8 | 7,441 | 97.2 |
| reusing a clean dropped slot | 100 | 71.3 | 106.2 | 142.7 | 213.8 | 222.4 | 223.8 | 6,938 | 109.4 |

Paired by trial (reusing - vacant, adjacent in time, order alternating): median +9.3 us, p25 +1.0, p75 +18.4, reusing slower in 78 of 100. Vacant trials split by position: first-in-pair median 136.0, second-in-pair 134.4; reusing first-in-pair 147.1, second-in-pair 141.2 (pairs alternate order).
