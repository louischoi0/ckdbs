| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| load-organizations (1e6 / mean) | 6,097.6 qps | 67.5 % | 6,035.0 qps | 5.7 % | 0.99x | spread > 20 %, no claim |
| load-ships (1e6 / mean) | 6,439.2 qps | 42.0 % | 6,027.7 qps | 25.6 % | 0.94x | spread > 20 %, no claim |
| load-operations (1e6 / mean) | 6,301.2 qps | 79.4 % | 6,518.9 qps | 4.8 % | 1.03x | spread > 20 %, no claim |
| load-fees (1e6 / mean) | 6,693.4 qps | 43.3 % | 6,756.8 qps | 12.6 % | 1.01x | spread > 20 %, no claim |
| load-recipes (1e6 / mean) | 6,169.0 qps | 70.8 % | 6,566.0 qps | 6.6 % | 1.06x | spread > 20 %, no claim |
| load-cargos (1e6 / mean) | 5,780.3 qps | 70.8 % | 6,451.6 qps | 3.2 % | 1.12x | spread > 20 %, no claim |
| booking (1e6 / mean) | 1,533.0 qps | 58.8 % | 1,614.2 qps | 10.3 % | 1.05x | spread > 20 %, no claim |
| cargo-lookup (1e6 / mean) | 22,573.4 qps | 21.1 % | 22,935.8 qps | 4.3 % | 1.02x | spread > 20 %, no claim |
| credit-lookup (1e6 / mean) | 25,773.2 qps | 22.4 % | 27,777.8 qps | 10.8 % | 1.08x | spread > 20 %, no claim |
| capacity-read (1e6 / mean) | 26,041.7 qps | 18.5 % | 29,069.8 qps | 13.2 % | 1.12x | inside the spread |
| recipe-read (1e6 / mean) | 12,970.2 qps | 10.4 % | 13,717.4 qps | 6.1 % | 1.06x | inside the spread |
| freight-insert (1e6 / mean) | 27,173.9 qps | 26.0 % | 29,940.1 qps | 30.8 % | 1.10x | spread > 20 %, no claim |
| charge-insert (1e6 / mean) | 38,910.5 qps | 5.3 % | 38,314.2 qps | 15.2 % | 0.98x | inside the spread |
| operation-update (1e6 / mean) | 33,670.0 qps | 10.8 % | 34,129.7 qps | 14.7 % | 1.01x | inside the spread |
| org-update (1e6 / mean) | 35,971.2 qps | 14.0 % | 35,461.0 qps | 16.6 % | 0.99x | inside the spread |
| commit (1e6 / mean) | 4,810.0 qps | 82.7 % | 4,868.5 qps | 10.7 % | 1.01x | spread > 20 %, no claim |
| manifest-scan (1e6 / mean) | 11,299.4 qps | 96.4 % | 7,604.6 qps | 215.7 % | 0.67x | spread > 20 %, no claim |
| voyage-rollup (1e6 / mean) | 18,148.8 qps | 137.9 % | 9,442.9 qps | 242.1 % | 0.52x | spread > 20 %, no claim |
| customer-statement (1e6 / mean) | 23,201.9 qps | 89.4 % | 9,803.9 qps | 296.2 % | 0.42x | spread > 20 %, no claim |
