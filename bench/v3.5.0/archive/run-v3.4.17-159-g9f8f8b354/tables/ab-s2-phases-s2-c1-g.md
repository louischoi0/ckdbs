| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| load-organizations (1e6 / mean) | 6,002.4 qps | 2.4 % | 6,596.3 qps | 16.8 % | 1.10x | inside the spread |
| load-ships (1e6 / mean) | 6,180.5 qps | 29.3 % | 5,249.3 qps | 44.2 % | 0.85x | spread > 20 %, no claim |
| load-operations (1e6 / mean) | 6,321.1 qps | 15.2 % | 6,472.5 qps | 6.2 % | 1.02x | inside the spread |
| load-fees (1e6 / mean) | 6,640.1 qps | 17.2 % | 6,281.4 qps | 18.7 % | 0.95x | inside the spread |
| load-recipes (1e6 / mean) | 6,609.4 qps | 13.3 % | 6,261.7 qps | 13.8 % | 0.95x | inside the spread |
| load-cargos (1e6 / mean) | 6,273.5 qps | 1.3 % | 6,389.8 qps | 1.4 % | 1.02x | **finding** |
| booking (1e6 / mean) | 547.9 qps | 14.1 % | 575.1 qps | 13.2 % | 1.05x | inside the spread |
| cargo-lookup (1e6 / mean) | 7,092.2 qps | 17.0 % | 7,564.3 qps | 14.9 % | 1.07x | inside the spread |
| credit-lookup (1e6 / mean) | 7,598.8 qps | 18.5 % | 7,936.5 qps | 17.0 % | 1.04x | inside the spread |
| capacity-read (1e6 / mean) | 7,704.2 qps | 17.1 % | 8,110.3 qps | 16.3 % | 1.05x | inside the spread |
| recipe-read (1e6 / mean) | 5,872.0 qps | 13.9 % | 6,176.7 qps | 10.9 % | 1.05x | inside the spread |
| freight-insert (1e6 / mean) | 7,299.3 qps | 11.5 % | 7,843.1 qps | 13.7 % | 1.07x | inside the spread |
| charge-insert (1e6 / mean) | 9,082.7 qps | 14.0 % | 9,606.1 qps | 13.7 % | 1.06x | inside the spread |
| operation-update (1e6 / mean) | 9,718.2 qps | 8.9 % | 9,920.6 qps | 17.6 % | 1.02x | inside the spread |
| org-update (1e6 / mean) | 9,746.6 qps | 8.8 % | 10,416.7 qps | 15.7 % | 1.07x | inside the spread |
| commit (1e6 / mean) | 3,818.3 qps | 12.1 % | 3,984.1 qps | 14.9 % | 1.04x | inside the spread |
| manifest-scan (1e6 / mean) | 14,641.3 qps | 61.0 % | 14,534.9 qps | 86.0 % | 0.99x | spread > 20 %, no claim |
| voyage-rollup (1e6 / mean) | 13,587.0 qps | 97.5 % | 22,727.3 qps | 77.7 % | 1.67x | spread > 20 %, no claim |
| customer-statement (1e6 / mean) | 12,738.9 qps | 89.0 % | 16,750.4 qps | 66.6 % | 1.31x | spread > 20 %, no claim |
