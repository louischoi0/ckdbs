| measure | v3.4.14 engine | spread | v3.5.0 engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s1 bar-lookup warm | 13,648.6 qps | 7.4 % | 13,882.1 qps | 9.0 % | 1.02x | inside the spread |
| s1 bar-range warm | 1,149.7 qps | 2.3 % | 1,142.9 qps | 3.4 % | 0.99x | inside the spread |
| s1 day-slice warm | 668.5 qps | 2.4 % | 657.0 qps | 3.2 % | 0.98x | inside the spread |
| s1 symbol-history warm | 27.1 qps | 1.1 % | 27.3 qps | 2.6 % | 1.01x | inside the spread |
| s1 cross-join warm | 665.2 qps | 6.1 % | 651.5 qps | 3.9 % | 0.98x | inside the spread |
| s1 point-join warm | 13,567.9 qps | 36.0 % | 14,344.6 qps | 1.8 % | 1.06x | spread > 20 %, no claim |
| s1 model-join warm | 903.1 qps | 2.2 % | 906.4 qps | 3.1 % | 1.00x | inside the spread |
| s1 phase backtest-read | 610.1 qps | 2.2 % | 609.9 qps | 2.3 % | 1.00x | inside the spread |
| s1 phase result-insert | 1,840.8 qps | 1.3 % | 1,821.0 qps | 1.4 % | 0.99x | inside the spread |
| s1 phase compare-one | 894.3 qps | 2.2 % | 909.5 qps | 4.3 % | 1.02x | inside the spread |
| s1 phase load-bars | 21,876.0 qps | 1.3 % | 21,837.1 qps | 3.3 % | 1.00x | inside the spread |
| s1 phase agg-by-session | 41.2 qps | 0.7 % | 40.8 qps | 1.5 % | 0.99x | inside the spread |
