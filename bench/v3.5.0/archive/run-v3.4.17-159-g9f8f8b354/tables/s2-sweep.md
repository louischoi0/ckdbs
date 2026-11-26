| bookers | durability | tps now | runs | spread | v3.5.0 run (other host) | now over v3.5.0 run (host + engine) | conflicted (run 1) | verify failures / checks (run 1) |
|---|---|---|---|---|---|---|---|---|
| 1 | group | **1,351.5 tps** | 1 | n/a | 568.2 tps | 2.38x | 0 | 0 / 400 |
| 1 | strict | **1,615.2 tps** | 1 | n/a | 572.1 tps | 2.82x | 0 | 0 / 400 |
| 2 | group | **2,589.0 tps** | 1 | n/a | 1,058.2 tps | 2.45x | 0 | 1 / 400 |
| 2 | strict | **2,992.5 tps** | 1 | n/a | 914.5 tps | 3.27x | 1 | 9 / 400 |
| 4 | group | **4,719.5 tps** | 1 | n/a | 1,718.1 tps | 2.75x | 0 | 13 / 400 |
| 4 | strict | **5,395.0 tps** | 1 | n/a | 1,532.1 tps | 3.52x | 22 | 11 / 400 |
| 8 | group | **8,111.5 tps** | 3 | 10.3 % | 2,612.6 tps | 3.10x | 0 | 23 / 400 |
| 8 | strict | **7,521.4 tps** | 3 | 55.1 % | 2,384.5 tps | 3.15x | 43 | 15 / 400 |
| 16 | group | **8,402.1 tps** | 1 | n/a | 3,522.9 tps | 2.39x | 0 | 57 / 400 |
| 16 | strict | **7,331.6 tps** | 1 | n/a | 3,286.8 tps | 2.23x | 60 | 40 / 400 |
| 32 | group | **10,475.1 tps** | 1 | n/a | 3,735.5 tps | 2.80x | 1 | 75 / 400 |
| 32 | strict | **10,468.4 tps** | 1 | n/a | 3,570.9 tps | 2.93x | 115 | 66 / 400 |
