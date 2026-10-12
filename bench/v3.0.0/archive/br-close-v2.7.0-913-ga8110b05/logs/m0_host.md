| cell | runs | UTC window of the run files | 1-min load before a run | 1-min load after a run | runs with a live compiler/build/test program | runs with any competing process | longest hold-back | errors |
|---|---|---|---|---|---|---|---|---|
| `relaxed-c1` | 72 runs | 15:55:27-15:58:58 UTC | 1.43-2.28 | 1.43-2.28 | 0 of 72 runs | 0 of 72 runs | 0 s | 0 errors |
| `relaxed-c4` | 72 runs | 15:59:01-16:02:43 UTC | 1.53-2.12 | 1.53-2.12 | 0 of 72 runs | 0 of 72 runs | 0 s | 0 errors |
| `aa-c1` | 36 runs | 16:02:46-16:04:29 UTC | 1.55-2.07 | 1.55-2.07 | 0 of 36 runs | 0 of 36 runs | 0 s | 0 errors |
| `aa-c4` | 36 runs | 16:04:32-16:06:19 UTC | 1.63-1.99 | 1.63-1.99 | 0 of 36 runs | 0 of 36 runs | 0 s | 0 errors |
| `group-c1` | 30 runs | 16:06:28-16:10:48 UTC | 1.52-2.54 | 1.52-2.54 | 0 of 30 runs | 0 of 30 runs | 0 s | 0 errors |
| `group-c4` | 30 runs | 16:10:57-16:15:14 UTC | 1.54-2.36 | 1.54-2.36 | 0 of 30 runs | 0 of 30 runs | 0 s | 0 errors |

Sampler (1 s, per-CPU busy from /proc/stat; foreign process = not under /home/cdkbs/bench-runs, >= 20 % of a CPU):

| cell | UTC window | samples | 1-min load, min-max | mean busy, server CPUs | mean busy, driver CPU 7 | mean busy, the other CPUs | samples with a foreign process >= 20 % | those processes |
|---|---|---|---|---|---|---|---|---|
| `relaxed-c1` | 15:55:25-15:58:58 | 210 | 1.43-2.28 | 39 % | 38 % | 4 % | 1 | [kds_server-A] <defunct> |
| `relaxed-c4` | 15:58:58-16:02:44 | 224 | 1.53-2.12 | 13 % | 39 % | 3 % | 4 | [kds_server-A]; [kds_server-B] <defunct> |
| `aa-c1` | 16:02:44-16:04:29 | 105 | 1.55-2.07 | 38 % | 38 % | 3 % | 3 | [kds_server-A]; python3 drivers/analyze_m0.py out/m0 relaxed-c1 relaxed-c4 |
| `aa-c4` | 16:04:29-16:06:19 | 110 | 1.63-1.99 | 11 % | 38 % | 1 % | 1 | [kds_server-A] <defunct> |
| `group-c1` | 16:06:19-16:10:48 | 267 | 1.52-2.59 | 9 % | 4 % | 3 % | 2 | python3 drivers/analyze_m0.py out/m0 relaxed-c1 relaxed-c4 a |
| `group-c4` | 16:10:48-16:15:14 | 263 | 1.52-2.36 | 6 % | 6 % | 4 % | 0 | - |
