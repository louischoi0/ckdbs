| cell | cores | durability | run 1 | run 2 | run 3 | median | spread | v3.5.0 run median (other host) | spread | now over v3.5.0 run (host + engine) |
|---|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 | group | 3,103.5 tps | 3,174.5 tps | 3,205.9 tps | **3,174.5 tps** | 3.2 % | 1,172.7 tps | 4.3 % | 2.71x |
| `s2-c8-g` | 8 | group | 7,354.7 tps | 8,111.5 tps | 8,193.6 tps | **8,111.5 tps** | 10.3 % | 2,612.6 tps | 4.8 % | 3.10x |
| `s2-c1-s` | 1 | strict | 3,089.2 tps | 3,177.1 tps | 1,922.1 tps | **3,089.2 tps** | 40.6 % | 1,108.6 tps | 3.7 % | 2.79x |
| `s2-c8-s` | 8 | strict | 8,175.0 tps | 4,031.4 tps | 7,521.4 tps | **7,521.4 tps** | 55.1 % | 2,384.5 tps | 5.1 % | 3.15x |
| `b1-s2-c1-g` | 1 | group | 1,820.8 tps | 1,097.6 tps | 1,270.7 tps | **1,270.7 tps** | 56.9 % | 615.9 tps | 3.8 % | 2.06x |
| `b1-s2-c1-s` | 1 | strict | 1,258.4 tps | 1,631.2 tps | 1,333.0 tps | **1,333.0 tps** | 28.0 % | 631.3 tps | 2.1 % | 2.11x |
