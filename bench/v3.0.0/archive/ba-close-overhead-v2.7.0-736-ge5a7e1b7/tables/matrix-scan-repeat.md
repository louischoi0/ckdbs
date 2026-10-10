| shape | cores 1 repeat, 200 rows | cores 1 repeat, 1,000 rows | cores 1 repeat, 10,000 rows |
|---|---|---|---|
| SELECT by pk | +0.85 µs [+0.25, +1.10]; -1.1 % tps [-1.8, -0.6] | +1.00 µs [+0.75, +1.35]; -1.6 % tps [-1.8, -1.0] | +0.70 µs [+0.35, +1.00]; -1.3 % tps [-1.8, -0.9] |
| whole-relation scan (COUNT/MIN/MAX, 100-id window) | +1.65 µs [+1.10, +1.90]; -2.6 % tps [-3.4, -0.9] | +5.20 µs [+4.30, +6.10]; -3.7 % tps [-5.0, -2.9] | +42.95 µs [+38.30, +51.30]; -5.1 % tps [-5.9, -4.0] |
| SHOW NAMESPACES (control) | +0.10 µs [+0.05, +0.20]; +0.2 % tps [-0.2, +0.6] | +0.00 µs [+0.00, +0.10]; +0.2 % tps [-0.5, +0.4] | +0.05 µs [-0.10, +0.15]; +0.1 % tps [-0.6, +0.8] |
| SET ISOLATION LEVEL (control) | +0.00 µs [-0.10, +0.10]; -0.1 % tps [-0.3, +0.2] | +0.00 µs [-0.10, +0.10]; -0.2 % tps [-0.5, +0.1] | +0.00 µs [-0.10, +0.10]; +0.1 % tps [-0.2, +0.4] |

Throughput, A then B (median of runs' ops/elapsed):

| shape | cores 1 repeat, 200 rows | cores 1 repeat, 1,000 rows | cores 1 repeat, 10,000 rows |
|---|---|---|---|
| SELECT by pk | 14,347 -> 14,140 tps | 13,951 -> 13,734 tps | 14,649 -> 14,386 tps |
| whole-relation scan (COUNT/MIN/MAX, 100-id window) | 11,008 -> 10,716 tps | 6,645 -> 6,420 tps | 1,245 -> 1,179 tps |
| SHOW NAMESPACES (control) | 25,166 -> 25,070 tps | 25,105 -> 25,150 tps | 25,194 -> 25,297 tps |
| SET ISOLATION LEVEL (control) | 25,304 -> 25,323 tps | 25,338 -> 25,353 tps | 25,404 -> 25,480 tps |

A's p50:

| shape | cores 1 repeat, 200 rows | cores 1 repeat, 1,000 rows | cores 1 repeat, 10,000 rows |
|---|---|---|---|
| SELECT by pk | 66.0 µs | 68.0 µs | 65.7 µs |
| whole-relation scan (COUNT/MIN/MAX, 100-id window) | 87.0 µs | 145.2 µs | 790.5 µs |
| SHOW NAMESPACES (control) | 38.0 µs | 38.0 µs | 38.0 µs |
| SET ISOLATION LEVEL (control) | 38.0 µs | 38.0 µs | 38.0 µs |
