| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| load-organizations (1e6 / mean) | 5,675.4 qps | 14.8 % | 5,717.6 qps | 61.6 % | 1.01x | spread > 20 %, no claim |
| load-ships (1e6 / mean) | 6,480.9 qps | 15.6 % | 6,613.8 qps | 9.9 % | 1.02x | inside the spread |
| load-operations (1e6 / mean) | 6,414.4 qps | 9.4 % | 6,497.7 qps | 66.6 % | 1.01x | spread > 20 %, no claim |
| load-fees (1e6 / mean) | 6,835.3 qps | 13.9 % | 6,337.1 qps | 37.9 % | 0.93x | spread > 20 %, no claim |
| load-recipes (1e6 / mean) | 6,257.8 qps | 9.4 % | 6,510.4 qps | 71.8 % | 1.04x | spread > 20 %, no claim |
| load-cargos (1e6 / mean) | 6,153.8 qps | 11.0 % | 6,476.7 qps | 65.0 % | 1.05x | spread > 20 %, no claim |
| booking (1e6 / mean) | 1,611.6 qps | 2.8 % | 1,662.2 qps | 62.3 % | 1.03x | spread > 20 %, no claim |
| cargo-lookup (1e6 / mean) | 23,310.0 qps | 5.2 % | 23,923.4 qps | 34.1 % | 1.03x | spread > 20 %, no claim |
| credit-lookup (1e6 / mean) | 29,069.8 qps | 3.7 % | 29,498.5 qps | 37.9 % | 1.01x | spread > 20 %, no claim |
| capacity-read (1e6 / mean) | 29,498.5 qps | 4.6 % | 30,211.5 qps | 37.4 % | 1.02x | spread > 20 %, no claim |
| recipe-read (1e6 / mean) | 13,568.5 qps | 3.3 % | 13,755.2 qps | 21.2 % | 1.01x | spread > 20 %, no claim |
| freight-insert (1e6 / mean) | 33,333.3 qps | 5.5 % | 33,444.8 qps | 34.1 % | 1.00x | spread > 20 %, no claim |
| charge-insert (1e6 / mean) | 38,759.7 qps | 2.7 % | 40,816.3 qps | 17.4 % | 1.05x | inside the spread |
| operation-update (1e6 / mean) | 35,211.3 qps | 2.5 % | 36,900.4 qps | 6.7 % | 1.05x | inside the spread |
| org-update (1e6 / mean) | 36,630.0 qps | 3.3 % | 38,314.2 qps | 4.2 % | 1.05x | **finding** |
| commit (1e6 / mean) | 4,906.8 qps | 9.6 % | 4,931.0 qps | 85.4 % | 1.00x | spread > 20 %, no claim |
| manifest-scan (1e6 / mean) | 5,777.0 qps | 73.1 % | 8,928.6 qps | 63.8 % | 1.55x | spread > 20 %, no claim |
| voyage-rollup (1e6 / mean) | 6,872.9 qps | 64.4 % | 11,695.9 qps | 57.8 % | 1.70x | spread > 20 %, no claim |
| customer-statement (1e6 / mean) | 4,672.9 qps | 64.2 % | 8,503.4 qps | 109.5 % | 1.82x | spread > 20 %, no claim |
