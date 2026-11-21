| phase | ops | now | spread | v3.4.14 | spread | delta | read |
|---|---|---|---|---|---|---|---|
| load-exchanges | 2 | 1,651.6 qps | 21.3 % | 1,858.9 qps | 9.0 % | -11.2 % | spread > 20 %, no claim |
| load-symbols | 8 | 2,010.0 qps | 22.2 % | 2,153.4 qps | 13.9 % | -6.7 % | spread > 20 %, no claim |
| load-sessions | 7,560 | 21,771.0 qps | 12.1 % | 23,826.3 qps | 5.9 % | -8.6 % | inside the spread |
| load-bars | 60,480 | 21,266.2 qps | 4.8 % | 22,397.4 qps | 6.0 % | -5.1 % | inside the spread |
| load-stats | 60,480 | 22,215.4 qps | 4.5 % | 21,824.7 qps | 1.0 % | +1.8 % | inside the spread |
| load-models | 8 | 1,475.5 qps | 17.0 % | 1,647.0 qps | 17.5 % | -10.4 % | inside the spread |
| backtest-read | 360 | 609.0 qps | 1.2 % | 615.0 qps | 1.1 % | -1.0 % | inside the spread |
| result-insert | 2,872 | 1,798.6 qps | 3.4 % | 1,789.2 qps | 3.0 % | +0.5 % | inside the spread |
| backtest-replay | 360 | 634.7 qps | 6.5 % | 658.4 qps | 15.8 % | -3.6 % | inside the spread |
| compare-all | 1 | 91.3 qps | 0.5 % | 92.4 qps | 4.1 % | -1.2 % | inside the spread |
| compare-one | 32 | 925.1 qps | 4.0 % | 915.3 qps | 2.6 % | +1.1 % | inside the spread |
| read-bar-lookup | 200 | 13,238.3 qps | 38.7 % | 13,081.4 qps | 30.0 % | +1.2 % | spread > 20 %, no claim |
| read-bar-range | 200 | 1,138.2 qps | 2.9 % | 1,071.5 qps | 9.2 % | +6.2 % | inside the spread |
| read-symbol-history | 10 | 27.3 qps | 5.1 % | 27.3 qps | 0.4 % | +0.0 % | inside the spread |
| read-day-slice | 200 | 685.7 qps | 3.5 % | 666.4 qps | 1.2 % | +2.9 % | inside the spread |
| read-join-point | 200 | 14,018.5 qps | 6.8 % | 13,645.8 qps | 4.2 % | +2.7 % | inside the spread |
| read-join-exists | 10 | 195.6 qps | 1.4 % | 202.8 qps | 3.7 % | -3.6 % | inside the spread |
| agg-global | 10 | 172.7 qps | 2.7 % | 172.8 qps | 3.4 % | -0.1 % | inside the spread |
| agg-by-symbol | 10 | 135.4 qps | 2.2 % | 130.4 qps | 4.7 % | +3.8 % | inside the spread |
| agg-by-session | 10 | 40.6 qps | 1.7 % | 40.6 qps | 1.5 % | +0.0 % | inside the spread |
| agg-day-slice | 200 | 700.5 qps | 1.4 % | 688.1 qps | 2.8 % | +1.8 % | inside the spread |
| agg-distinct | 10 | 167.4 qps | 0.7 % | 167.9 qps | 4.8 % | -0.3 % | inside the spread |
| sync | 1 | 295.0 qps | 13.9 % | 281.3 qps | 10.3 % | +4.9 % | inside the spread |
