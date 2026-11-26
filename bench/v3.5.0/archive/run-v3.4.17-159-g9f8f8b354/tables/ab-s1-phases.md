| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s1 phase load-exchanges | 4,739.1 qps | 15.1 % | 1,633.6 qps | 101.9 % | 0.34x | spread > 20 %, no claim |
| s1 phase load-symbols | 6,183.6 qps | 28.9 % | 4,254.1 qps | 44.7 % | 0.69x | spread > 20 %, no claim |
| s1 phase load-sessions | 37,463.8 qps | 81.3 % | 50,040.1 qps | 47.8 % | 1.34x | spread > 20 %, no claim |
| s1 phase load-bars | 38,634.4 qps | 21.0 % | 43,287.3 qps | 28.1 % | 1.12x | spread > 20 %, no claim |
| s1 phase load-stats | 39,379.0 qps | 23.2 % | 43,717.2 qps | 29.3 % | 1.11x | spread > 20 %, no claim |
| s1 phase load-models | 2,136.7 qps | 18.0 % | 2,001.7 qps | 149.2 % | 0.94x | spread > 20 %, no claim |
| s1 phase backtest-read | 1,107.2 qps | 13.3 % | 1,190.1 qps | 1.6 % | 1.07x | inside the spread |
| s1 phase result-insert | 4,264.8 qps | 53.4 % | 4,185.3 qps | 47.4 % | 0.98x | spread > 20 %, no claim |
| s1 phase backtest-replay | 1,088.1 qps | 34.1 % | 1,404.4 qps | 38.4 % | 1.29x | spread > 20 %, no claim |
| s1 phase compare-all | 157.6 qps | 19.9 % | 178.4 qps | 18.2 % | 1.13x | inside the spread |
| s1 phase compare-one | 1,565.3 qps | 33.6 % | 1,173.1 qps | 33.3 % | 0.75x | spread > 20 %, no claim |
| s1 phase read-bar-lookup | 22,801.7 qps | 30.9 % | 26,033.5 qps | 5.3 % | 1.14x | spread > 20 %, no claim |
| s1 phase read-bar-range | 1,927.5 qps | 25.2 % | 2,339.8 qps | 7.2 % | 1.21x | spread > 20 %, no claim |
| s1 phase read-symbol-history | 42.2 qps | 25.8 % | 52.6 qps | 18.8 % | 1.25x | spread > 20 %, no claim |
| s1 phase read-day-slice | 1,084.8 qps | 18.4 % | 1,343.6 qps | 4.4 % | 1.24x | **finding** |
| s1 phase read-join-point | 22,398.6 qps | 17.7 % | 30,313.5 qps | 19.2 % | 1.35x | **finding** |
| s1 phase read-join-exists | 353.2 qps | 4.0 % | 488.3 qps | 1.2 % | 1.38x | **finding** |
| s1 phase agg-global | 269.7 qps | 6.4 % | 406.7 qps | 8.6 % | 1.51x | **finding** |
| s1 phase agg-by-symbol | 207.8 qps | 15.0 % | 292.9 qps | 11.2 % | 1.41x | **finding** |
| s1 phase agg-by-session | 76.7 qps | 10.8 % | 75.6 qps | 3.2 % | 0.99x | inside the spread |
| s1 phase agg-day-slice | 1,144.4 qps | 40.7 % | 1,101.5 qps | 16.0 % | 0.96x | spread > 20 %, no claim |
| s1 phase agg-distinct | 260.2 qps | 46.7 % | 253.1 qps | 4.6 % | 0.97x | spread > 20 %, no claim |
| s1 phase sync | 429.4 qps | 8.1 % | 439.5 qps | 15.6 % | 1.02x | inside the spread |
