| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s1 bar-lookup cold | 12,865.4 qps | 68.5 % | 11,870.1 qps | 66.9 % | 0.92x | spread > 20 %, no claim |
| s1 bar-lookup warm | 26,242.5 qps | 10.4 % | 25,695.8 qps | 28.9 % | 0.98x | spread > 20 %, no claim |
| s1 bar-range cold | 1,735.9 qps | 6.9 % | 1,743.0 qps | 7.6 % | 1.00x | inside the spread |
| s1 bar-range warm | 1,739.0 qps | 6.4 % | 1,737.3 qps | 4.9 % | 1.00x | inside the spread |
| s1 day-slice cold | 1,387.1 qps | 7.5 % | 1,393.3 qps | 8.4 % | 1.00x | inside the spread |
| s1 day-slice warm | 1,376.2 qps | 3.5 % | 1,377.9 qps | 8.7 % | 1.00x | inside the spread |
| s1 day-slice cabin | 12,505.1 qps | 20.5 % | 11,435.2 qps | 52.7 % | 0.91x | spread > 20 %, no claim |
| s1 symbol-history cold | 42.4 qps | 1.2 % | 42.5 qps | 1.2 % | 1.00x | inside the spread |
| s1 symbol-history warm | 41.7 qps | 1.0 % | 41.9 qps | 1.2 % | 1.00x | inside the spread |
| s1 symbol-history cabin | 41.4 qps | 0.7 % | 41.7 qps | 0.7 % | 1.01x | inside the spread |
| s1 cross-join cold | 1,257.4 qps | 9.3 % | 1,285.4 qps | 6.0 % | 1.02x | inside the spread |
| s1 cross-join warm | 1,342.0 qps | 6.7 % | 1,373.5 qps | 4.2 % | 1.02x | inside the spread |
| s1 cross-join cabin | 11,764.8 qps | 10.5 % | 10,020.9 qps | 20.3 % | 0.85x | spread > 20 %, no claim |
| s1 point-join cold | 22,690.8 qps | 7.6 % | 22,107.6 qps | 36.1 % | 0.97x | spread > 20 %, no claim |
| s1 point-join warm | 29,364.6 qps | 5.4 % | 27,974.1 qps | 33.3 % | 0.95x | spread > 20 %, no claim |
| s1 model-join cold | 1,657.3 qps | 3.5 % | 1,632.1 qps | 11.3 % | 0.98x | inside the spread |
| s1 model-join warm | 1,515.3 qps | 1.8 % | 1,517.4 qps | 3.2 % | 1.00x | inside the spread |
| s1 model-join cabin | 1,537.6 qps | 2.2 % | 1,531.3 qps | 2.1 % | 1.00x | inside the spread |
