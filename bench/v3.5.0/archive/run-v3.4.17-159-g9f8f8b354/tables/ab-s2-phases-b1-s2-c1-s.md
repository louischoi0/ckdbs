| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| load-organizations (1e6 / mean) | 5,977.3 qps | 67.9 % | 6,165.2 qps | 12.9 % | 1.03x | spread > 20 %, no claim |
| load-ships (1e6 / mean) | 5,977.3 qps | 31.0 % | 5,814.0 qps | 17.2 % | 0.97x | spread > 20 %, no claim |
| load-operations (1e6 / mean) | 6,353.2 qps | 65.4 % | 6,253.9 qps | 14.7 % | 0.98x | spread > 20 %, no claim |
| load-fees (1e6 / mean) | 6,561.7 qps | 24.7 % | 6,265.7 qps | 10.2 % | 0.95x | spread > 20 %, no claim |
| load-recipes (1e6 / mean) | 6,557.4 qps | 68.0 % | 5,841.1 qps | 10.9 % | 0.89x | spread > 20 %, no claim |
| load-cargos (1e6 / mean) | 4,866.2 qps | 71.3 % | 6,506.2 qps | 8.0 % | 1.34x | spread > 20 %, no claim |
| booking (1e6 / mean) | 1,345.4 qps | 68.8 % | 1,589.3 qps | 9.1 % | 1.18x | spread > 20 %, no claim |
| cargo-lookup (1e6 / mean) | 22,321.4 qps | 34.6 % | 20,242.9 qps | 12.4 % | 0.91x | spread > 20 %, no claim |
| credit-lookup (1e6 / mean) | 26,954.2 qps | 41.9 % | 23,419.2 qps | 8.4 % | 0.87x | spread > 20 %, no claim |
| capacity-read (1e6 / mean) | 28,169.0 qps | 46.3 % | 24,271.8 qps | 8.1 % | 0.86x | spread > 20 %, no claim |
| recipe-read (1e6 / mean) | 13,774.1 qps | 29.1 % | 12,210.0 qps | 5.5 % | 0.89x | spread > 20 %, no claim |
| freight-insert (1e6 / mean) | 32,258.1 qps | 40.5 % | 28,571.4 qps | 7.0 % | 0.89x | spread > 20 %, no claim |
| charge-insert (1e6 / mean) | 44,444.4 qps | 62.5 % | 34,965.0 qps | 6.7 % | 0.79x | spread > 20 %, no claim |
| operation-update (1e6 / mean) | 36,764.7 qps | 45.0 % | 29,940.1 qps | 9.2 % | 0.81x | spread > 20 %, no claim |
| org-update (1e6 / mean) | 40,983.6 qps | 54.9 % | 32,467.5 qps | 9.2 % | 0.79x | spread > 20 %, no claim |
| commit (1e6 / mean) | 2,819.3 qps | 138.3 % | 6,578.9 qps | 13.0 % | 2.33x | spread > 20 %, no claim |
| manifest-scan (1e6 / mean) | 9,066.2 qps | 93.9 % | 6,447.5 qps | 35.6 % | 0.71x | spread > 20 %, no claim |
| voyage-rollup (1e6 / mean) | 11,778.6 qps | 89.8 % | 7,704.2 qps | 32.5 % | 0.65x | spread > 20 %, no claim |
| customer-statement (1e6 / mean) | 2,453.4 qps | 33.3 % | 1,776.8 qps | 19.7 % | 0.72x | spread > 20 %, no claim |
