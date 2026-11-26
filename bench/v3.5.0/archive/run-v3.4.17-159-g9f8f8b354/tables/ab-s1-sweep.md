| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s1 bar-lookup cold | 12,955.0 qps | 74.2 % | 15,548.3 qps | 41.1 % | 1.20x | spread > 20 %, no claim |
| s1 bar-lookup warm | 26,430.7 qps | 28.5 % | 23,177.1 qps | 38.7 % | 0.88x | spread > 20 %, no claim |
| s1 bar-range cold | 1,932.9 qps | 27.2 % | 1,940.2 qps | 10.8 % | 1.00x | spread > 20 %, no claim |
| s1 bar-range warm | 1,948.9 qps | 28.7 % | 1,950.0 qps | 19.4 % | 1.00x | spread > 20 %, no claim |
| s1 day-slice cold | 1,105.7 qps | 44.4 % | 1,075.6 qps | 13.0 % | 0.97x | spread > 20 %, no claim |
| s1 day-slice warm | 1,106.6 qps | 46.9 % | 1,097.0 qps | 1.3 % | 0.99x | spread > 20 %, no claim |
| s1 day-slice cabin | 12,700.1 qps | 45.0 % | 12,989.3 qps | 9.2 % | 1.02x | spread > 20 %, no claim |
| s1 symbol-history cold | 43.2 qps | 20.8 % | 43.2 qps | 10.2 % | 1.00x | spread > 20 %, no claim |
| s1 symbol-history warm | 45.1 qps | 9.5 % | 45.1 qps | 12.2 % | 1.00x | inside the spread |
| s1 symbol-history cabin | 46.0 qps | 7.2 % | 45.1 qps | 14.0 % | 0.98x | inside the spread |
| s1 cross-join cold | 1,013.2 qps | 4.1 % | 1,066.2 qps | 4.1 % | 1.05x | **finding** |
| s1 cross-join warm | 1,073.4 qps | 2.9 % | 1,081.4 qps | 36.0 % | 1.01x | spread > 20 %, no claim |
| s1 cross-join cabin | 11,991.8 qps | 1.7 % | 11,738.9 qps | 30.8 % | 0.98x | spread > 20 %, no claim |
| s1 point-join cold | 21,467.4 qps | 13.6 % | 23,134.1 qps | 48.5 % | 1.08x | spread > 20 %, no claim |
| s1 point-join warm | 23,679.3 qps | 15.2 % | 23,801.1 qps | 49.7 % | 1.01x | spread > 20 %, no claim |
| s1 model-join cold | 1,626.1 qps | 1.8 % | 1,640.2 qps | 23.2 % | 1.01x | spread > 20 %, no claim |
| s1 model-join warm | 1,618.8 qps | 5.3 % | 1,683.4 qps | 26.6 % | 1.04x | spread > 20 %, no claim |
| s1 model-join cabin | 1,723.1 qps | 1.9 % | 1,705.2 qps | 22.6 % | 0.99x | spread > 20 %, no claim |
