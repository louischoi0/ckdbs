| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| load-organizations (1e6 / mean) | 6,518.9 qps | 57.8 % | 6,514.7 qps | 11.3 % | 1.00x | spread > 20 %, no claim |
| load-ships (1e6 / mean) | 6,812.0 qps | 26.0 % | 6,863.4 qps | 18.3 % | 1.01x | spread > 20 %, no claim |
| load-operations (1e6 / mean) | 7,163.3 qps | 10.7 % | 6,949.3 qps | 10.1 % | 0.97x | inside the spread |
| load-fees (1e6 / mean) | 7,102.3 qps | 30.3 % | 7,052.2 qps | 16.5 % | 0.99x | spread > 20 %, no claim |
| load-recipes (1e6 / mean) | 6,934.8 qps | 10.5 % | 7,027.4 qps | 11.4 % | 1.01x | inside the spread |
| load-cargos (1e6 / mean) | 6,949.3 qps | 15.3 % | 6,901.3 qps | 4.8 % | 0.99x | inside the spread |
| booking (1e6 / mean) | 629.7 qps | 89.3 % | 635.5 qps | 7.7 % | 1.01x | spread > 20 %, no claim |
| cargo-lookup (1e6 / mean) | 8,347.2 qps | 87.1 % | 8,460.2 qps | 8.0 % | 1.01x | spread > 20 %, no claim |
| credit-lookup (1e6 / mean) | 8,818.3 qps | 87.8 % | 8,896.8 qps | 6.4 % | 1.01x | spread > 20 %, no claim |
| capacity-read (1e6 / mean) | 9,058.0 qps | 92.9 % | 9,049.8 qps | 8.7 % | 1.00x | spread > 20 %, no claim |
| recipe-read (1e6 / mean) | 6,561.7 qps | 79.1 % | 6,510.4 qps | 6.8 % | 0.99x | spread > 20 %, no claim |
| freight-insert (1e6 / mean) | 8,143.3 qps | 92.0 % | 8,223.7 qps | 11.2 % | 1.01x | spread > 20 %, no claim |
| charge-insert (1e6 / mean) | 10,593.2 qps | 89.7 % | 10,649.6 qps | 7.8 % | 1.01x | spread > 20 %, no claim |
| operation-update (1e6 / mean) | 11,454.8 qps | 89.6 % | 11,627.9 qps | 4.4 % | 1.02x | spread > 20 %, no claim |
| org-update (1e6 / mean) | 11,709.6 qps | 85.0 % | 11,961.7 qps | 7.7 % | 1.02x | spread > 20 %, no claim |
| commit (1e6 / mean) | 4,330.9 qps | 92.6 % | 4,397.5 qps | 10.1 % | 1.02x | spread > 20 %, no claim |
| manifest-scan (1e6 / mean) | 4,315.9 qps | 397.6 % | 3,110.4 qps | 157.8 % | 0.72x | spread > 20 %, no claim |
| voyage-rollup (1e6 / mean) | 4,960.3 qps | 414.3 % | 4,029.0 qps | 226.1 % | 0.81x | spread > 20 %, no claim |
| customer-statement (1e6 / mean) | 2,971.8 qps | 474.4 % | 6,680.0 qps | 126.2 % | 2.25x | spread > 20 %, no claim |
