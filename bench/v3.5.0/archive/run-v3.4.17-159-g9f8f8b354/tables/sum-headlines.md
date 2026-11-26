| measure | now (`9f8f8b354`, this host) | spread | `v3.5.0` run (`07f4552a`, other host) | spread | now over `v3.5.0` run (host + engine) |
|---|---|---|---|---|---|
| 1 backtest, `day-slice` warm (60,480 bars) | 1,079.9 qps | 35.9 % | 677.6 qps | 2.6 % | 1.59x |
| 1 backtest, `cross-join` warm | 1,053.7 qps | 22.7 % | 640.7 qps | 5.4 % | 1.64x |
| 1 backtest, `bar-range` warm | 1,939.4 qps | 28.8 % | 1,160.4 qps | 4.9 % | 1.67x |
| 1 backtest, `model-join` warm | 1,622.1 qps | 12.5 % | 898.4 qps | 5.5 % | 1.81x |
| 1 backtest, `result-insert` phase | 4,621.7 qps | 6.1 % | 1,798.6 qps | 3.4 % | 2.57x |
| 2 freight, 8 bookers, `cores = 1`, `group` | 3,174.5 tps | 3.2 % | 1,172.7 tps | 4.3 % | 2.71x |
| 2 freight, 8 bookers, `cores = 8`, `group` | 8,111.5 tps | 10.3 % | 2,612.6 tps | 4.8 % | 3.10x |
| 2 freight, 8 bookers, `cores = 8`, `strict` | 7,521.4 tps | 55.1 % | 2,384.5 tps | 5.1 % | 3.15x |
| 2 freight, 1 booker, `strict` | 1,333.0 tps | 28.0 % | 631.3 tps | 2.1 % | 2.11x |
| 2 freight, 32 bookers, `cores = 8`, `group` (one run) | 10,475.1 tps | n/a | 3,735.5 tps | n/a | 2.80x |
| 3 library, `loans-by-user` (10,000 loans, unindexed) | 5,388 qps | 28.1 % | 3,180 qps | 4.8 % | 1.69x |
| 3 library, `join-no-literal` (10,000 loans, unindexed) | 3,396 qps | 24.8 % | 1,954 qps | 4.0 % | 1.74x |
| 3 library, `exists-correlated` (10,000 loans, unindexed) | 2,945 qps | 40.0 % | 1,520 qps | 2.9 % | 1.94x |
| 3 library, `loans-by-user` (10,000 loans, indexed) | 18,083 qps | 37.3 % | 12,180 qps | 8.0 % | 1.48x |
