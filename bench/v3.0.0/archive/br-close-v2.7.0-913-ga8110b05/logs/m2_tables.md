marks: {'load_start': 0.0, 'fail_start': 45.01, 'recover_start': 65.01, 'load_end': 105.2}
writers: 6 stmts 11132 errors 0 {'ops': 11132, 'p0_us': 1150.8, 'p25_us': 1823.4, 'p50_us': 2604.0, 'p95_us': 255603.8, 'p99_us': 1443522.7, 'p999_us': 2426743.8, 'max_us': 4252781.6, 'mean_us': 56636.8, 'slow_gt_1ms_n': 11132, 'slow_gt_1ms_s': 630.4805}

| phase | span | duration | WAL generation (durable LSN) | archive rate (segments archived x 64 MiB) | max wal_archive_lag_segments | archive failures in the phase |
|---|---|---|---|---|---|---|
| steady (archive writable) | 0-45 s | 44.8 s | 54.3 MiB/s | 48.6 MiB/s | 5 segments | 0 |
| archive directory unwritable | 45-65 s | 20.2 s | 76.0 MiB/s | 0.0 MiB/s | 27 segments | 5 |
| archive writable again, load running | 65-105 s | 40.2 s | 49.3 MiB/s | 46.2 MiB/s | 39 segments | 0 |
| load stopped (tail) | 105-130 s | 24.8 s | 0.1 MiB/s | 77.6 MiB/s | 29 segments | 0 |
| whole load | 0-105 s | 105.2 s | 56.6 MiB/s | 38.3 MiB/s | 39 segments | 5 |

peak WAL generation over any 5 s window: 106.7 MiB/s (starting at 30.8 s); median of 5 s windows 64.7 MiB/s; min 19.1
peak over any 1 s window 768.2 MiB/s

archive progress events (t, segments added, lag after):
[(1.803, 1.0, 1.0), (4.832, 1.0, 3.0), (5.983, 2.0, 2.0), (7.956, 1.0, 3.0), (9.099, 1.0, 3.0), (10.622, 2.0, 2.0), (12.639, 1.0, 3.0), (13.804, 1.0, 3.0), (14.946, 1.0, 3.0), (17.637, 1.0, 3.0), (20.572, 2.0, 4.0), (21.73, 1.0, 4.0)] ... [(117.197, 1.0, 11.0), (117.697, 1.0, 10.0), (118.197, 1.0, 9.0), (118.447, 1.0, 8.0), (118.947, 1.0, 7.0), (119.197, 1.0, 6.0), (119.697, 1.0, 5.0), (119.947, 1.0, 4.0), (120.448, 1.0, 3.0), (120.698, 1.0, 2.0), (121.198, 1.0, 1.0), (121.698, 1.0, 0.0)]

lag trace through the failure (t: lag, failures):
[(43, 4.0, 0.0), (47, 6.0, 2.0), (51, 11.0, 3.0), (56, 15.0, 4.0), (59, 19.0, 4.0), (62, 23.0, 5.0), (65, 27.0, 5.0), (69, 31.0, 5.0), (74, 35.0, 5.0), (77, 39.0, 5.0), (83, 37.0, 5.0), (88, 34.0, 5.0), (96, 31.0, 5.0), (101, 29.0, 5.0), (106, 29.0, 5.0), (108, 27.0, 5.0), (110, 26.0, 5.0)]
first archived segment after the directory was writable again at t = 12.13 s after
max lag 39.0 at t = 76.349
drain from lag 39 to <= 1: 44.8 s, 58 segments archived = 82.8 MiB/s, while the log grew 27.2 MiB/s (net drain 55.6 MiB/s)
