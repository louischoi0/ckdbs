| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s1 phase load-exchanges | 3,429.5 qps | 34.8 % | 1,596.3 qps | 50.7 % | 0.47x | spread > 20 %, no claim |
| s1 phase load-symbols | 5,041.7 qps | 18.8 % | 5,332.1 qps | 43.7 % | 1.06x | spread > 20 %, no claim |
| s1 phase load-sessions | 47,507.1 qps | 3.1 % | 48,153.7 qps | 2.9 % | 1.01x | inside the spread |
| s1 phase load-bars | 45,136.1 qps | 1.3 % | 44,837.0 qps | 3.0 % | 0.99x | inside the spread |
| s1 phase load-stats | 46,749.7 qps | 1.4 % | 46,695.8 qps | 3.3 % | 1.00x | inside the spread |
| s1 phase load-models | 3,732.1 qps | 17.3 % | 4,474.1 qps | 28.1 % | 1.20x | spread > 20 %, no claim |
| s1 phase backtest-read | 1,309.2 qps | 4.3 % | 1,317.9 qps | 9.8 % | 1.01x | inside the spread |
| s1 phase result-insert | 4,805.1 qps | 8.2 % | 4,759.9 qps | 50.3 % | 0.99x | spread > 20 %, no claim |
| s1 phase backtest-replay | 1,324.4 qps | 6.1 % | 1,348.6 qps | 3.1 % | 1.02x | inside the spread |
| s1 phase compare-all | 139.2 qps | 13.4 % | 135.0 qps | 15.5 % | 0.97x | inside the spread |
| s1 phase compare-one | 1,504.3 qps | 7.1 % | 1,488.7 qps | 5.5 % | 0.99x | inside the spread |
| s1 phase read-bar-lookup | 24,336.0 qps | 4.9 % | 24,272.6 qps | 13.3 % | 1.00x | inside the spread |
| s1 phase read-bar-range | 1,732.0 qps | 3.9 % | 1,704.4 qps | 2.8 % | 0.98x | inside the spread |
| s1 phase read-symbol-history | 41.7 qps | 1.9 % | 41.7 qps | 1.7 % | 1.00x | inside the spread |
| s1 phase read-day-slice | 1,346.6 qps | 7.7 % | 1,342.4 qps | 1.9 % | 1.00x | inside the spread |
| s1 phase read-join-point | 24,736.9 qps | 8.8 % | 23,581.2 qps | 17.3 % | 0.95x | inside the spread |
| s1 phase read-join-exists | 479.9 qps | 3.3 % | 478.0 qps | 2.5 % | 1.00x | inside the spread |
| s1 phase agg-global | 388.8 qps | 7.3 % | 384.7 qps | 6.4 % | 0.99x | inside the spread |
| s1 phase agg-by-symbol | 255.1 qps | 7.9 % | 253.8 qps | 13.9 % | 0.99x | inside the spread |
| s1 phase agg-by-session | 79.7 qps | 1.5 % | 79.3 qps | 3.3 % | 0.99x | inside the spread |
| s1 phase agg-day-slice | 1,389.7 qps | 8.0 % | 1,378.7 qps | 4.2 % | 0.99x | inside the spread |
| s1 phase agg-distinct | 371.7 qps | 4.5 % | 359.8 qps | 3.1 % | 0.97x | inside the spread |
| s1 phase sync | 527.9 qps | 35.6 % | 440.6 qps | 31.0 % | 0.83x | spread > 20 %, no claim |
