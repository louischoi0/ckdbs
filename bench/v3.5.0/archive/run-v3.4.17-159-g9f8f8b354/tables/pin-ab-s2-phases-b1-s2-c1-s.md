| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| load-organizations (1e6 / mean) | 6,514.7 qps | 4.5 % | 6,561.7 qps | 9.3 % | 1.01x | inside the spread |
| load-ships (1e6 / mean) | 6,680.0 qps | 29.5 % | 7,052.2 qps | 4.3 % | 1.06x | spread > 20 %, no claim |
| load-operations (1e6 / mean) | 6,825.9 qps | 15.8 % | 7,225.4 qps | 4.7 % | 1.06x | inside the spread |
| load-fees (1e6 / mean) | 7,142.9 qps | 18.8 % | 7,352.9 qps | 6.9 % | 1.03x | inside the spread |
| load-recipes (1e6 / mean) | 6,734.0 qps | 13.7 % | 6,939.6 qps | 17.9 % | 1.03x | inside the spread |
| load-cargos (1e6 / mean) | 6,896.6 qps | 12.1 % | 6,973.5 qps | 13.1 % | 1.01x | inside the spread |
| booking (1e6 / mean) | 2,045.0 qps | 0.9 % | 2,045.8 qps | 1.1 % | 1.00x | inside the spread |
| cargo-lookup (1e6 / mean) | 28,169.0 qps | 1.4 % | 28,248.6 qps | 3.4 % | 1.00x | inside the spread |
| credit-lookup (1e6 / mean) | 33,003.3 qps | 2.7 % | 33,112.6 qps | 2.0 % | 1.00x | inside the spread |
| capacity-read (1e6 / mean) | 33,783.8 qps | 2.0 % | 33,898.3 qps | 1.7 % | 1.00x | inside the spread |
| recipe-read (1e6 / mean) | 14,814.8 qps | 1.2 % | 14,858.8 qps | 1.8 % | 1.00x | inside the spread |
| freight-insert (1e6 / mean) | 40,322.6 qps | 3.7 % | 40,983.6 qps | 1.6 % | 1.02x | inside the spread |
| charge-insert (1e6 / mean) | 47,393.4 qps | 1.4 % | 47,619.0 qps | 1.4 % | 1.00x | inside the spread |
| operation-update (1e6 / mean) | 42,553.2 qps | 3.5 % | 42,918.5 qps | 3.0 % | 1.01x | inside the spread |
| org-update (1e6 / mean) | 45,871.6 qps | 2.3 % | 45,871.6 qps | 1.4 % | 1.00x | inside the spread |
| commit (1e6 / mean) | 7,412.9 qps | 2.9 % | 7,380.1 qps | 1.0 % | 1.00x | inside the spread |
| manifest-scan (1e6 / mean) | 8,795.1 qps | 56.6 % | 7,745.9 qps | 25.8 % | 0.88x | spread > 20 %, no claim |
| voyage-rollup (1e6 / mean) | 10,235.4 qps | 64.2 % | 9,756.1 qps | 9.6 % | 0.95x | spread > 20 %, no claim |
| customer-statement (1e6 / mean) | 3,132.8 qps | 26.5 % | 3,112.4 qps | 16.6 % | 0.99x | spread > 20 %, no claim |
