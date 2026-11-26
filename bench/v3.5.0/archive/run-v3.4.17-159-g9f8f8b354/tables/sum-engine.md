| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| 1 backtest, `day-slice` warm | 1,376.2 qps | 3.5 % | 1,377.9 qps | 8.7 % | 1.00x | inside the spread |
| 1 backtest, `cross-join` warm | 1,342.0 qps | 6.7 % | 1,373.5 qps | 4.2 % | 1.02x | inside the spread |
| 1 backtest, `bar-range` warm | 1,739.0 qps | 6.4 % | 1,737.3 qps | 4.9 % | 1.00x | inside the spread |
| 1 backtest, `model-join` warm | 1,515.3 qps | 1.8 % | 1,517.4 qps | 3.2 % | 1.00x | inside the spread |
| 1 backtest, `symbol-history` warm | 41.7 qps | 1.0 % | 41.9 qps | 1.2 % | 1.00x | inside the spread |
| 1 backtest, phase `backtest-read` | 1,309.2 qps | 4.3 % | 1,317.9 qps | 9.8 % | 1.01x | inside the spread |
| 1 backtest, phase `result-insert` | 4,805.1 qps | 8.2 % | 4,759.9 qps | 50.3 % | 0.99x | spread > 20 %, no claim |
| 1 backtest, phase `load-bars` | 45,136.1 qps | 1.3 % | 44,837.0 qps | 3.0 % | 0.99x | inside the spread |
| 1 backtest, phase `agg-by-session` | 79.7 qps | 1.5 % | 79.3 qps | 3.3 % | 0.99x | inside the spread |
| 2 freight, 8 bookers, `cores = 1`, `group` | 4,192.8 tps | 88.3 % | 4,228.0 tps | 6.3 % | 1.01x | spread > 20 %, no claim |
| 2 freight, 1 booker, `cores = 1`, `strict` | 1,749.5 tps | 0.9 % | 1,749.7 tps | 1.1 % | 1.00x | inside the spread |
| 2 freight, 8 bookers, `cores = 8`, `group` | 9,543.2 tps | 3.9 % | 9,801.8 tps | 58.4 % | 1.03x | spread > 20 %, no claim |
| 3 library, `pk-user` (10,000 loans, unindexed) | 30,303.0 qps | 15.4 % | 30,864.2 qps | 4.2 % | 1.02x | inside the spread |
| 3 library, `loans-by-user` (10,000 loans, unindexed) | 6,887.1 qps | 5.5 % | 6,839.9 qps | 1.0 % | 0.99x | inside the spread |
| 3 library, `join-no-literal` (10,000 loans, unindexed) | 4,147.7 qps | 2.9 % | 4,116.9 qps | 5.4 % | 0.99x | inside the spread |
| 3 library, `exists-correlated` (10,000 loans, unindexed) | 4,095.0 qps | 2.1 % | 4,122.0 qps | 4.6 % | 1.01x | inside the spread |
| 3 library, `count-by-user` (10,000 loans, unindexed) | 7,501.9 qps | 0.6 % | 7,418.4 qps | 1.0 % | 0.99x | **finding** |
| 3 library, `pk-user` (10,000 loans, indexed) | 18,975.3 qps | 10.1 % | 18,181.8 qps | 13.8 % | 0.96x | inside the spread |
| 3 library, `loans-by-user` (10,000 loans, indexed) | 25,062.7 qps | 6.4 % | 24,813.9 qps | 2.2 % | 0.99x | inside the spread |
| 3 library, `join-no-literal` (10,000 loans, indexed) | 8,888.9 qps | 3.1 % | 8,635.6 qps | 7.5 % | 0.97x | inside the spread |
| 3 library, `exists-correlated` (10,000 loans, indexed) | 17,064.8 qps | 5.4 % | 16,420.4 qps | 2.9 % | 0.96x | inside the spread |
| 3 library, `count-by-user` (10,000 loans, indexed) | 34,722.2 qps | 2.1 % | 34,482.8 qps | 3.1 % | 0.99x | inside the spread |
