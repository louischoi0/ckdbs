| cores | shape | archive | n | statements/s | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| 1 | INSERT hot (10,000 rows) | off | 5 rounds | 23189 [23111 to 23331] /s | 41.4 [41.2 to 41.5] µs | 61.4 [60.9 to 64.0] µs | 0.1 [0.1 to 0.1] ms | 0 [0 to 0] ms |
| 1 | INSERT hot (10,000 rows) | on | 5 rounds | 23194 [22988 to 23497] /s | 41.3 [40.7 to 41.6] µs | 62.9 [61.8 to 65.3] µs | 0.1 [0.1 to 0.2] ms | 0 [0 to 0] ms |
| 1 | INSERT hot (10,000 rows) | on vs off, paired | 5 rounds | 0.0 [-1.5 to 1.5] % | | | | |
| 1 | SELECT by pk | off | 5 rounds | 17173 [16686 to 17544] /s | 55.9 [55.2 to 56.3] µs | 77.9 [73.2 to 84.1] µs | 0.1 [0.1 to 0.2] ms | 0 [0 to 1] ms |
| 1 | SELECT by pk | on | 5 rounds | 17334 [17188 to 17686] /s | 55.6 [55.0 to 55.9] µs | 78.0 [73.3 to 84.3] µs | 0.1 [0.1 to 0.1] ms | 0 [0 to 0] ms |
| 1 | SELECT by pk | on vs off, paired | 5 rounds | 0.5 [-1.5 to 6.0] % | | | | |
| 1 | UPDATE by pk | off | 5 rounds | 21408 [19812 to 21806] /s | 43.5 [43.0 to 44.0] µs | 62.8 [61.0 to 68.9] µs | 0.1 [0.1 to 1.0] ms | 4 [3 to 17] ms |
| 1 | UPDATE by pk | on | 5 rounds | 20982 [18912 to 22081] /s | 43.1 [43.0 to 44.3] µs | 63.7 [58.6 to 84.1] µs | 0.1 [0.1 to 0.4] ms | 4 [3 to 7] ms |
| 1 | UPDATE by pk | on vs off, paired | 5 rounds | -2.0 [-11.7 to 10.0] % | | | | |
| 1 | wide INSERT, 50 rows x 1.8 KB | off | 5 rounds | 206 [205 to 207] /s | 1230.9 [1214.3 to 1234.5] µs | 5801.9 [5649.8 to 5969.7] µs | 586.3 [578.0 to 638.1] ms | 1077 [976 to 1140] ms |
| 1 | wide INSERT, 50 rows x 1.8 KB | on | 5 rounds | 147 [146 to 148] /s | 1224.1 [1183.0 to 1230.6] µs | 5385.9 [5309.4 to 5955.9] µs | 998.6 [976.5 to 1315.8] ms | 1379 [1331 to 1404] ms |
| 1 | wide INSERT, 50 rows x 1.8 KB | on vs off, paired | 5 rounds | -28.7 [-28.9 to -28.0] % | | | | |
| 4 | INSERT hot (10,000 rows) | off | 5 rounds | 22633 [22547 to 22805] /s | 42.3 [42.0 to 42.4] µs | 64.3 [62.4 to 65.0] µs | 0.1 [0.1 to 0.1] ms | 0 [0 to 1] ms |
| 4 | INSERT hot (10,000 rows) | on | 5 rounds | 22524 [22362 to 22565] /s | 42.3 [42.0 to 42.6] µs | 65.6 [63.9 to 67.0] µs | 0.1 [0.1 to 0.1] ms | 0 [0 to 0] ms |
| 4 | INSERT hot (10,000 rows) | on vs off, paired | 5 rounds | -0.9 [-1.4 to -0.1] % | | | | |
| 4 | SELECT by pk | off | 5 rounds | 16897 [16673 to 16902] /s | 57.0 [56.7 to 57.0] µs | 80.4 [79.0 to 85.3] µs | 0.1 [0.1 to 0.2] ms | 0 [0 to 0] ms |
| 4 | SELECT by pk | on | 5 rounds | 16728 [16533 to 16860] /s | 57.2 [56.6 to 57.6] µs | 83.4 [80.3 to 84.4] µs | 0.1 [0.1 to 0.1] ms | 0 [0 to 0] ms |
| 4 | SELECT by pk | on vs off, paired | 5 rounds | -0.9 [-2.2 to 1.1] % | | | | |
| 4 | UPDATE by pk | off | 5 rounds | 20918 [20298 to 20947] /s | 44.7 [44.4 to 45.0] µs | 67.5 [66.5 to 73.2] µs | 0.1 [0.1 to 0.1] ms | 4 [3 to 8] ms |
| 4 | UPDATE by pk | on | 5 rounds | 20578 [20311 to 20804] /s | 45.0 [44.3 to 45.1] µs | 69.7 [68.7 to 70.2] µs | 0.1 [0.1 to 0.1] ms | 4 [4 to 6] ms |
| 4 | UPDATE by pk | on vs off, paired | 5 rounds | -0.5 [-1.7 to 0.1] % | | | | |
| 4 | wide INSERT, 50 rows x 1.8 KB | off | 5 rounds | 206 [205 to 207] /s | 1262.1 [1253.1 to 1270.9] µs | 6079.4 [5938.2 to 6123.2] µs | 572.5 [558.9 to 579.9] ms | 1059 [805 to 1083] ms |
| 4 | wide INSERT, 50 rows x 1.8 KB | on | 5 rounds | 151 [147 to 152] /s | 1260.0 [1252.1 to 1287.0] µs | 5838.6 [4991.5 to 6059.3] µs | 1189.9 [973.0 to 1320.2] ms | 1323 [1125 to 1335] ms |
| 4 | wide INSERT, 50 rows x 1.8 KB | on vs off, paired | 5 rounds | -26.7 [-28.5 to -26.3] % | | | | |

WAL and archive counters, wide INSERT phase:
| cores | archive | WAL written in the phase | WAL rate | segments archived by the end | wal_archive_lag_segments at the end of the phase | failures |
|---|---|---|---|---|---|---|
| 1 | off | 894 [892 to 896] MiB | 76.6 [76.4 to 77.1] MiB/s | 0 [0 to 0] | 0 [0 to 0] | 0 |
| 1 | on | 895 [892 to 901] MiB | 55.0 [54.6 to 55.1] MiB/s | 14 [14 to 14] | 2 [2 to 2] | 0 |
| 4 | off | 892 [892 to 898] MiB | 76.8 [76.3 to 77.1] MiB/s | 0 [0 to 0] | 0 [0 to 0] | 0 |
| 4 | on | 896 [891 to 901] MiB | 56.3 [54.8 to 57.2] MiB/s | 14 [14 to 14] | 3 [2 to 3] | 0 |

Diagnostic (cores = 1): archive directory on tmpfs (/dev/shm), on the data device, and off. 3 rounds, order rotated.
| archive | n | wide INSERT statements/s | p50 | p99 | WAL rate |
|---|---|---|---|---|---|
| off | 3 | 206.9 [206.5 to 207.4] /s | 1219.8 [1174.9 to 1224.5] µs | 5957.6 [5946.1 to 5979.2] µs | 77.0 [76.9 to 77.2] MiB/s |
| shm | 3 | 200.3 [199.8 to 205.7] /s | 1234.4 [1220.6 to 1240.1] µs | 5901.1 [5899.2 to 5939.2] µs | 75.0 [74.6 to 76.9] MiB/s |
| on | 3 | 147.8 [147.8 to 150.3] /s | 1217.3 [1180.5 to 1219.6] µs | 5599.1 [5575.1 to 5722.3] µs | 54.9 [54.9 to 56.1] MiB/s |
