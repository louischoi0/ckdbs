
### m1b-c1: ballast 540,000 rows, loaded in 54.8 s; 5 backups
| window | n | duration (s) | statements/s, raw | statements/s excluding statements over 1 ms | share of the window inside statements over 1 ms | p50 | p99 | p99.9 | WAL bytes per statement |
|---|---|---|---|---|---|---|---|---|---|
| baseline (no backup) | 5 windows | 4.7 [4.2 to 7.0] s | 6946 [2323 to 8185] /s | 18271 [17447 to 18737] /s | 63 [55 to 87] % | 48.4 [48.3 to 48.5] µs | 83.4 [77.5 to 284.3] µs | 319.1 [178.9 to 352.4] µs | 1202 [1192 to 1212] B |
| BACKUP COPY running | 5 windows | 4.2 [4.2 to 4.8] s | 10925 [6117 to 14846] /s | 16801 [16318 to 16904] /s | 35 [12 to 63] % | 49.3 [49.1 to 49.4] µs | 334.1 [304.7 to 350.5] µs | 414.3 [406.1 to 530.5] µs | 1207 [1206 to 1212] B |
| BACKUP STOP finishing (data sync, log copy, manifest) | 5 windows | 10.1 [9.7 to 11.0] s | 5304 [5176 to 5962] /s | 18018 [17255 to 18250] /s | 70 [66 to 72] % | 48.6 [48.4 to 48.7] µs | 89.7 [84.3 to 192.5] µs | 379.0 [325.3 to 542.0] µs | 1842 [1277 to 2289] B |

5 backups, each copy window minus the baseline window just before it: p50 0.9 [0.8 to 1.0] µs; p99 240.2 [57.0 to 272.7] µs; service rate excluding statements over 1 ms -1470 [-2382 to -809] /s, -8.0 [-12.7 to -4.6] %.

| statement kind | baseline p50 | copy p50 | baseline p99 | copy p99 |
|---|---|---|---|---|
| ins-hot | 46.6 [46.5 to 46.8] µs | 47.2 [47.1 to 47.3] µs | 73.2 [69.7 to 268.7] µs | 330.3 [295.8 to 357.1] µs |
| upd-hot | 46.8 [46.6 to 46.9] µs | 47.4 [47.3 to 47.6] µs | 72.1 [66.8 to 285.1] µs | 292.1 [277.2 to 330.1] µs |
| upd-ballast | 48.5 [48.4 to 48.7] µs | 49.2 [49.1 to 49.4] µs | 74.4 [69.1 to 280.8] µs | 326.2 [290.7 to 341.0] µs |
| sel-hot | 60.9 [60.7 to 61.2] µs | 61.9 [61.7 to 62.9] µs | 87.9 [83.8 to 295.2] µs | 352.7 [318.8 to 369.1] µs |

The copy: 4.25 [4.21 to 4.82] s at 249.3 [222.8 to 253.6] MiB/s over 137152 [135552 to 137600] pages. STOP's finish: 10.25 [9.74 to 11.04] s. Writes that waited for the fence, in total at the end: 0 writes. Log copied per backup: 256, 320, 256, 320, 256 MiB. Backup size: 1067, 1070, 1072, 1074, 1080 MiB. Log span of the window (stop LSN - start LSN): 172, 255, 179, 306, 199 MiB.

### m1b-c1-offcpu: ballast 540,000 rows, loaded in 54.8 s; 5 backups
| window | n | duration (s) | statements/s, raw | statements/s excluding statements over 1 ms | share of the window inside statements over 1 ms | p50 | p99 | p99.9 | WAL bytes per statement |
|---|---|---|---|---|---|---|---|---|---|
| baseline (no backup) | 5 windows | 4.8 [4.2 to 6.0] s | 9768 [6230 to 16213] /s | 18231 [17916 to 18461] /s | 46 [11 to 65] % | 49.0 [48.8 to 49.3] µs | 83.0 [80.6 to 103.4] µs | 302.9 [113.6 to 324.3] µs | 1207 [1205 to 1209] B |
| BACKUP COPY running | 5 windows | 4.2 [4.2 to 5.0] s | 7021 [4195 to 11441] /s | 15973 [15646 to 16695] /s | 56 [31 to 73] % | 49.8 [49.7 to 50.2] µs | 332.4 [316.1 to 348.9] µs | 568.6 [391.2 to 652.9] µs | 1205 [1176 to 1214] B |
| BACKUP STOP finishing (data sync, log copy, manifest) | 5 windows | 10.4 [9.3 to 11.3] s | 4506 [4108 to 5305] /s | 17871 [17075 to 18007] /s | 74 [70 to 77] % | 49.0 [48.9 to 49.2] µs | 86.0 [82.7 to 190.6] µs | 329.9 [314.8 to 971.6] µs | 1532 [1381 to 2731] B |

5 backups, each copy window minus the baseline window just before it: p50 0.9 [0.8 to 0.9] µs; p99 251.0 [220.1 to 268.3] µs; service rate excluding statements over 1 ms -2420 [-2585 to -1222] /s, -13.2 [-14.2 to -6.8] %.

| statement kind | baseline p50 | copy p50 | baseline p99 | copy p99 |
|---|---|---|---|---|
| ins-hot | 47.4 [47.2 to 47.6] µs | 47.9 [47.9 to 48.2] µs | 72.8 [71.0 to 108.4] µs | 329.1 [310.7 to 360.8] µs |
| upd-hot | 47.6 [47.2 to 47.7] µs | 48.1 [47.8 to 48.2] µs | 72.7 [70.2 to 86.5] µs | 326.6 [306.4 to 349.4] µs |
| upd-ballast | 49.1 [48.9 to 49.4] µs | 49.7 [49.6 to 50.1] µs | 74.7 [71.6 to 91.3] µs | 320.2 [318.2 to 345.8] µs |
| sel-hot | 62.2 [61.4 to 62.4] µs | 62.8 [62.3 to 63.6] µs | 91.0 [88.6 to 130.1] µs | 343.1 [325.8 to 358.7] µs |

The copy: 4.26 [4.19 to 4.98] s at 252.8 [215.4 to 254.6] MiB/s over 137280 [135552 to 138112] pages. STOP's finish: 10.40 [9.44 to 11.35] s. Writes that waited for the fence, in total at the end: 0 writes. Log copied per backup: 256, 256, 192, 192, 384 MiB. Backup size: 1059, 1061, 1072, 1074, 1081 MiB. Log span of the window (stop LSN - start LSN): 161, 231, 113, 181, 279 MiB.

### m1b-c4: ballast 540,000 rows, loaded in 54.3 s; 5 backups
| window | n | duration (s) | statements/s, raw | statements/s excluding statements over 1 ms | share of the window inside statements over 1 ms | p50 | p99 | p99.9 | WAL bytes per statement |
|---|---|---|---|---|---|---|---|---|---|
| baseline (no backup) | 5 windows | 7.1 [5.3 to 8.9] s | 7943 [6327 to 13932] /s | 16405 [16053 to 17285] /s | 53 [16 to 61] % | 50.9 [50.0 to 51.1] µs | 146.9 [128.4 to 172.9] µs | 215.2 [182.9 to 284.2] µs | 1211 [1208 to 1212] B |
| BACKUP COPY running | 5 windows | 4.4 [4.2 to 5.3] s | 8758 [1972 to 10068] /s | 15624 [15431 to 16620] /s | 45 [35 to 87] % | 54.4 [51.4 to 54.6] µs | 145.7 [132.3 to 170.6] µs | 235.0 [191.6 to 418.2] µs | 1210 [1206 to 1229] B |
| BACKUP STOP finishing (data sync, log copy, manifest) | 5 windows | 10.8 [10.5 to 11.3] s | 3623 [2651 to 4537] /s | 8091 [6384 to 11870] /s | 61 [55 to 62] % | 52.7 [51.6 to 54.3] µs | 185.9 [173.0 to 192.6] µs | 415.4 [259.7 to 446.2] µs | 2333 [2036 to 3377] B |

5 backups, each copy window minus the baseline window just before it: p50 3.5 [1.4 to 3.8] µs; p99 -1.2 [-11.9 to 23.9] µs; service rate excluding statements over 1 ms -665 [-898 to -606] /s, -3.9 [-5.4 to -3.7] %.

| statement kind | baseline p50 | copy p50 | baseline p99 | copy p99 |
|---|---|---|---|---|
| ins-hot | 48.7 [48.6 to 48.8] µs | 49.7 [49.0 to 50.0] µs | 76.5 [73.0 to 90.5] µs | 82.8 [77.8 to 110.2] µs |
| upd-hot | 48.5 [48.3 to 48.6] µs | 49.5 [48.8 to 49.8] µs | 74.6 [71.3 to 89.5] µs | 78.9 [77.7 to 116.6] µs |
| upd-ballast | 51.2 [49.9 to 51.3] µs | 54.5 [50.9 to 55.0] µs | 172.4 [142.5 to 195.9] µs | 163.8 [148.6 to 198.3] µs |
| sel-hot | 64.3 [63.7 to 64.3] µs | 66.0 [64.8 to 67.0] µs | 95.0 [90.6 to 108.3] µs | 104.7 [102.1 to 130.5] µs |

The copy: 4.41 [4.24 to 5.35] s at 249.4 [204.0 to 253.8] MiB/s over 139968 [137856 to 140608] pages. STOP's finish: 10.94 [10.52 to 11.45] s. Writes that waited for the fence, in total at the end: 0 writes. Log copied per backup: 256, 448, 384, 384, 384 MiB. Backup size: 1082, 1092, 1093, 1097, 1099 MiB. Log span of the window (stop LSN - start LSN): 166, 382, 309, 321, 329 MiB.
