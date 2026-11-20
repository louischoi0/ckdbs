| cell | cores | durability | run 1 | run 2 | run 3 | median | spread | v3.4.14 median | spread | delta | read |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 | group | 1,172.7 tps | 1,182.7 tps | 1,131.8 tps | **1,172.7 tps** | 4.3 % | 1,183.3 tps | 1.0 % | -0.9 % | inside the spread |
| `s2-c8-g` | 8 | group | 2,612.6 tps | 2,545.0 tps | 2,669.7 tps | **2,612.6 tps** | 4.8 % | 2,548.8 tps | 1.4 % | +2.5 % | inside the spread |
| `s2-c1-s` | 1 | strict | 1,108.6 tps | 1,089.3 tps | 1,130.1 tps | **1,108.6 tps** | 3.7 % | 1,123.5 tps | 5.1 % | -1.3 % | inside the spread |
| `s2-c8-s` | 8 | strict | 2,299.9 tps | 2,384.5 tps | 2,421.2 tps | **2,384.5 tps** | 5.1 % | 2,321.0 tps | 2.7 % | +2.7 % | inside the spread |
| `b1-s2-c1-g` | 1 | group | 636.9 tps | 615.9 tps | 613.6 tps | **615.9 tps** | 3.8 % | 628.7 tps | 3.7 % | -2.0 % | inside the spread |
| `b1-s2-c1-s` | 1 | strict | 631.3 tps | 637.4 tps | 624.0 tps | **631.3 tps** | 2.1 % | 625.8 tps | 6.7 % | +0.9 % | inside the spread |
