### Relaxed, every session count

| shape | cores | engine | 1 session | 2 sessions | 4 sessions | 8 sessions | 16 sessions |
|---|---|---|---|---|---|---|---|
| point | 1 | KDS `relaxed`, B | 14,092 stmt/s | 23,070 stmt/s | 43,458 stmt/s | 43,158 stmt/s | 41,560 stmt/s |
| point | 1 | KDS `relaxed`, BA-S4 | 14,521 stmt/s | 23,776 stmt/s | 44,842 stmt/s | 44,671 stmt/s | 43,409 stmt/s |
| point | CPUs 0 | PostgreSQL `off`, BA-S4 | 16,974 stmt/s | 30,116 stmt/s | 28,987 stmt/s | 27,438 stmt/s | 26,960 stmt/s |
| point | 2 | KDS `relaxed`, B | 13,840 stmt/s | 21,401 stmt/s | 41,917 stmt/s | 59,073 stmt/s | 64,908 stmt/s |
| point | 2 | KDS `relaxed`, BA-S4 | 14,262 stmt/s | 22,844 stmt/s | 43,536 stmt/s | 60,016 stmt/s | 65,347 stmt/s |
| point | CPUs 0,2 | PostgreSQL `off`, BA-S4 | 16,929 stmt/s | 29,184 stmt/s | 60,665 stmt/s | 57,128 stmt/s | 54,433 stmt/s |
| trade | 1 | KDS `relaxed`, B | 16,588 stmt/s | 27,161 stmt/s | 38,757 stmt/s | 38,552 stmt/s | 38,426 stmt/s |
| trade | 1 | KDS `relaxed`, BA-S4 | 16,436 stmt/s | 27,429 stmt/s | 37,600 stmt/s | 37,323 stmt/s | 37,118 stmt/s |
| trade | CPUs 0 | PostgreSQL `off`, BA-S4 | 13,812 stmt/s | 21,473 stmt/s | 19,897 stmt/s | 18,740 stmt/s | 18,540 stmt/s |
| trade | 2 | KDS `relaxed`, B | 16,140 stmt/s | 26,695 stmt/s | 47,445 stmt/s | 64,704 stmt/s | 63,915 stmt/s |
| trade | 2 | KDS `relaxed`, BA-S4 | 16,110 stmt/s | 26,642 stmt/s | 41,920 stmt/s | 53,805 stmt/s | 60,113 stmt/s |
| trade | CPUs 0,2 | PostgreSQL `off`, BA-S4 | 13,823 stmt/s | 24,204 stmt/s | 41,634 stmt/s | 39,222 stmt/s | 37,593 stmt/s |
| insert1 | 1 | KDS `relaxed`, B | 18,169 stmt/s | 31,053 stmt/s | 43,062 stmt/s | 42,516 stmt/s | 42,151 stmt/s |
| insert1 | 1 | KDS `relaxed`, BA-S4 | 17,799 stmt/s | 28,342 stmt/s | 38,884 stmt/s | 39,022 stmt/s | 38,166 stmt/s |
| insert1 | CPUs 0 | PostgreSQL `off`, BA-S4 | 17,332 stmt/s | 31,384 stmt/s | 29,454 stmt/s | 27,775 stmt/s | 27,186 stmt/s |
| insert1 | 2 | KDS `relaxed`, B | 18,062 stmt/s | 31,404 stmt/s | 51,469 stmt/s | 64,228 stmt/s | 69,538 stmt/s |
| insert1 | 2 | KDS `relaxed`, BA-S4 | 17,693 stmt/s | 28,389 stmt/s | 47,159 stmt/s | 54,525 stmt/s | 68,341 stmt/s |
| insert1 | CPUs 0,2 | PostgreSQL `off`, BA-S4 | 17,296 stmt/s | 29,595 stmt/s | 61,344 stmt/s | 58,062 stmt/s | 54,628 stmt/s |
| insertN | 1 | KDS `relaxed`, B | 18,323 stmt/s | 30,331 stmt/s | 42,512 stmt/s | 42,344 stmt/s | 42,220 stmt/s |
| insertN | 1 | KDS `relaxed`, BA-S4 | 17,955 stmt/s | 28,463 stmt/s | 38,410 stmt/s | 38,691 stmt/s | 37,721 stmt/s |
| insertN | CPUs 0 | PostgreSQL `off`, BA-S4 | 17,299 stmt/s | 31,136 stmt/s | 29,122 stmt/s | 27,595 stmt/s | 26,933 stmt/s |
| insertN | 2 | KDS `relaxed`, B | 17,931 stmt/s | 31,264 stmt/s | 51,685 stmt/s | 69,101 stmt/s | 72,063 stmt/s |
| insertN | 2 | KDS `relaxed`, BA-S4 | 17,582 stmt/s | 28,798 stmt/s | 46,430 stmt/s | 60,343 stmt/s | 64,647 stmt/s |
| insertN | CPUs 0,2 | PostgreSQL `off`, BA-S4 | 17,431 stmt/s | 29,954 stmt/s | 61,180 stmt/s | 57,862 stmt/s | 54,282 stmt/s |
| recent | 1 | KDS `relaxed`, B | 14,470 stmt/s | 23,036 stmt/s | 38,226 stmt/s | 38,549 stmt/s | 38,836 stmt/s |
| recent | 1 | KDS `relaxed`, BA-S4 | 14,344 stmt/s | 21,792 stmt/s | 33,341 stmt/s | 33,847 stmt/s | 34,206 stmt/s |
| recent | CPUs 0 | PostgreSQL `off`, BA-S4 | 14,173 stmt/s | 23,963 stmt/s | 23,176 stmt/s | 22,280 stmt/s | 21,652 stmt/s |
| recent | 2 | KDS `relaxed`, B | 14,293 stmt/s | 21,768 stmt/s | 40,887 stmt/s | 59,680 stmt/s | 66,407 stmt/s |
| recent | 2 | KDS `relaxed`, BA-S4 | 14,044 stmt/s | 21,064 stmt/s | 37,931 stmt/s | 47,495 stmt/s | 53,233 stmt/s |
| recent | CPUs 0,2 | PostgreSQL `off`, BA-S4 | 14,222 stmt/s | 25,031 stmt/s | 46,609 stmt/s | 45,710 stmt/s | 43,880 stmt/s |

### What the second core buys at 16 sessions (`cores = 2` over `cores = 1`, PostgreSQL CPUs 0,2 over CPU 0)

| shape | durability | B | BA-S4 | PostgreSQL (BA-S4) |
|---|---|---|---|---|
| point | relaxed | 1.56x | 1.51x | 2.02x (`off`) |
| point | group | 1.55x | 1.50x | 2.02x (`on`) |
| point | strict | 1.55x | 1.50x | 2.02x (`on`) |
| trade | relaxed | 1.66x | 1.62x | 2.03x (`off`) |
| trade | group | 1.19x | 0.33x | 1.05x (`on`) |
| trade | strict | 7.24x | 1.05x | 1.05x (`on`) |
| insert1 | relaxed | 1.65x | 1.79x | 2.01x (`off`) |
| insert1 | group | 1.05x | 0.59x | 1.04x (`on`) |
| insert1 | strict | 7.11x | 0.97x | 1.04x (`on`) |
| insertN | relaxed | 1.71x | 1.71x | 2.02x (`off`) |
| insertN | group | 1.05x | 0.72x | 1.04x (`on`) |
| insertN | strict | 7.42x | 1.03x | 1.04x (`on`) |
| recent | relaxed | 1.71x | 1.56x | 2.03x (`off`) |
| recent | group | 1.67x | 1.05x | 1.02x (`on`) |
| recent | strict | 6.66x | 1.10x | 1.02x (`on`) |

### Latency distribution of one statement (median over the cell's 3 runs of each run's percentile)

| cell | census | statements per run | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| insert1 `relaxed` `cores = 2`, 16 sessions | B | 556,967 statements | 41 µs | 116 µs | 151 µs | 286 µs | 378 µs |
| insert1 `relaxed` `cores = 2`, 16 sessions | BA-S4 | 548,467 statements | 43 µs | 138 µs | 152 µs | 243 µs | 309 µs |
| insert1 `group` `cores = 2`, 1 session | B | 6,736 statements | 891 µs | 997 µs | 1,034 µs | 1,747 µs | 4,679 µs |
| insert1 `group` `cores = 2`, 1 session | BA-S4 | 5,372 statements | 1,007 µs | 1,222 µs | 1,287 µs | 2,390 µs | 5,499 µs |
| insert1 `group` `cores = 2`, 16 sessions | B | 49,850 statements | 1,006 µs | 1,910 µs | 1,981 µs | 4,502 µs | 9,387 µs |
| insert1 `group` `cores = 2`, 16 sessions | BA-S4 | 23,391 statements | 1,410 µs | 2,243 µs | 2,391 µs | 5,125 µs | 12,238 µs |
| insert1 `strict` `cores = 2`, 1 session | B | 6,626 statements | 885 µs | 1,000 µs | 1,036 µs | 1,782 µs | 4,914 µs |
| insert1 `strict` `cores = 2`, 1 session | BA-S4 | 5,618 statements | 1,019 µs | 1,205 µs | 1,256 µs | 2,022 µs | 5,342 µs |
| insert1 `strict` `cores = 2`, 16 sessions | B | 49,753 statements | 1,013 µs | 1,990 µs | 2,076 µs | 4,782 µs | 9,607 µs |
| insert1 `strict` `cores = 2`, 16 sessions | BA-S4 | 5,645 statements | 11,891 µs | 17,185 µs | 18,951 µs | 40,516 µs | 53,426 µs |
| insert1 `group` `cores = 1`, 16 sessions | B | 47,419 statements | 1,331 µs | 2,261 µs | 2,332 µs | 4,923 µs | 9,234 µs |
| insert1 `group` `cores = 1`, 16 sessions | BA-S4 | 39,470 statements | 1,818 µs | 2,694 µs | 2,855 µs | 5,836 µs | 11,098 µs |
| point `relaxed` `cores = 1`, 1 session | B | 111,232 statements | 58 µs | 65 µs | 66 µs | 81 µs | 92 µs |
| point `relaxed` `cores = 1`, 1 session | BA-S4 | 115,484 statements | 58 µs | 64 µs | 65 µs | 76 µs | 88 µs |

### Same-day PostgreSQL control (BA-S4's `on` cells re-run after the KDS census)

| shape | CPUs | sessions | today (3 runs) | BA-S4 median | today over BA-S4 |
|---|---|---|---|---|---|
| insert1 | 0 | 1 | 705 stmt/s, 710 stmt/s, 708 stmt/s | 708 stmt/s | -0.0 % |
| insert1 | 0 | 16 | 5,335 stmt/s, 5,249 stmt/s, 5,167 stmt/s | 5,120 stmt/s | +2.5 % |
| insert1 | 0,2 | 1 | 711 stmt/s, 716 stmt/s, 736 stmt/s | 691 stmt/s | +3.6 % |
| insert1 | 0,2 | 16 | 5,350 stmt/s, 5,403 stmt/s, 5,393 stmt/s | 5,325 stmt/s | +1.3 % |
| insertN | 0,2 | 16 | 5,338 stmt/s, 5,464 stmt/s, 5,407 stmt/s | 5,035 stmt/s | +7.4 % |
| recent | 0,2 | 16 | 10,473 stmt/s, 10,492 stmt/s, 10,474 stmt/s | 9,942 stmt/s | +5.4 % |
| trade | 0,2 | 16 | 5,318 stmt/s, 5,242 stmt/s, 5,223 stmt/s | 5,150 stmt/s | +1.8 % |

### Cells BA did not change, B over BA-S4 at `cores = 1` (median of cell medians)

| durability | sessions | shapes | B over BA-S4, per shape | median |
|---|---|---|---|---|
| relaxed | 1 session | 5 shapes | point -3.0 %, trade +0.9 %, insert1 +2.1 %, insertN +2.0 %, recent +0.9 % | +0.9 % |
| relaxed | 2 sessions | 5 shapes | point -3.0 %, trade -1.0 %, insert1 +9.6 %, insertN +6.6 %, recent +5.7 % | +5.7 % |
| relaxed | 4 sessions | 5 shapes | point -3.1 %, trade +3.1 %, insert1 +10.7 %, insertN +10.7 %, recent +14.7 % | +10.7 % |
| relaxed | 8 sessions | 5 shapes | point -3.4 %, trade +3.3 %, insert1 +9.0 %, insertN +9.4 %, recent +13.9 % | +9.0 % |
| relaxed | 16 sessions | 5 shapes | point -4.3 %, trade +3.5 %, insert1 +10.4 %, insertN +11.9 %, recent +13.5 % | +10.4 % |
| group | 1 session | 5 shapes | point -1.9 %, trade +21.2 %, insert1 +27.4 %, insertN +21.1 %, recent +20.9 % | +21.1 % |
| group | 2 sessions | 5 shapes | point -1.8 %, trade +22.6 %, insert1 +22.3 %, insertN +20.0 %, recent +20.4 % | +20.4 % |
| group | 4 sessions | 5 shapes | point -1.5 %, trade +23.4 %, insert1 +25.3 %, insertN +24.2 %, recent +23.6 % | +23.6 % |
| group | 8 sessions | 5 shapes | point -5.4 %, trade +15.2 %, insert1 +21.0 %, insertN +22.2 %, recent +22.3 % | +21.0 % |
| group | 16 sessions | 5 shapes | point -3.4 %, trade +11.9 %, insert1 +20.2 %, insertN +18.0 %, recent +19.8 % | +18.0 % |
| strict | 1 session | 5 shapes | point -2.8 %, trade +18.8 %, insert1 +14.3 %, insertN +24.2 %, recent +18.1 % | +18.1 % |
| strict | 2 sessions | 5 shapes | point -1.2 %, trade +23.7 %, insert1 +17.1 %, insertN +21.0 %, recent +17.7 % | +17.7 % |
| strict | 4 sessions | 5 shapes | point -2.7 %, trade +20.4 %, insert1 +19.0 %, insertN +21.4 %, recent +23.0 % | +20.4 % |
| strict | 8 sessions | 5 shapes | point -3.2 %, trade +26.8 %, insert1 +25.1 %, insertN +18.4 %, recent +25.3 % | +25.1 % |
| strict | 16 sessions | 5 shapes | point -3.8 %, trade +26.1 %, insert1 +20.0 %, insertN +18.5 %, recent +27.8 % | +20.0 % |

### Between-run spread inside a cell (max/min - 1 of statements/s over the 3 runs)

- B: median 3.8 %, p90 19.3 %, max 75.6 % over 225 cells
- BA-S4: median 3.5 %, p90 39.8 %, max 191.3 % over 225 cells

### Carve and checkpoint longest, by durability and cores (max over each server's life; 75 runs per row)

| durability | cores | S4 carve median / max | B carve median / max | S4 carve runs over 10 ms | B carve runs over 10 ms | S4 checkpoint median / max | B checkpoint median / max | S4 checkpoint runs over 10 ms | B checkpoint runs over 10 ms |
|---|---|---|---|---|---|---|---|---|---|
| relaxed | 1 | 20.5 ms / 107.6 ms | 225.5 ms / 296.2 ms | 60 of 75 runs | 62 of 75 runs | 19.3 ms / 160.6 ms | 122.8 ms / 379.1 ms | 63 of 75 runs | 75 of 75 runs |
| relaxed | 2 | 30.5 ms / 452.4 ms | 257.9 ms / 333.7 ms | 62 of 75 runs | 59 of 75 runs | 55.9 ms / 203.1 ms | 187.9 ms / 510.5 ms | 75 of 75 runs | 75 of 75 runs |
| relaxed | 4 | 45.8 ms / 481.6 ms | 251.5 ms / 352.4 ms | 60 of 75 runs | 59 of 75 runs | 121.0 ms / 596.1 ms | 209.1 ms / 647.4 ms | 75 of 75 runs | 75 of 75 runs |
| group | 1 | 5.9 ms / 56.0 ms | 5.4 ms / 19.2 ms | 23 of 75 runs | 6 of 75 runs | 13.1 ms / 172.7 ms | 21.3 ms / 199.1 ms | 42 of 75 runs | 63 of 75 runs |
| group | 2 | 10.5 ms / 2,242.1 ms | 5.0 ms / 30.7 ms | 38 of 75 runs | 11 of 75 runs | 159.5 ms / 3,093.7 ms | 200.5 ms / 1,555.3 ms | 75 of 75 runs | 75 of 75 runs |
| group | 4 | 16.7 ms / 1,616.1 ms | 5.2 ms / 230.2 ms | 45 of 75 runs | 18 of 75 runs | 212.2 ms / 3,088.8 ms | 67.5 ms / 348.4 ms | 75 of 75 runs | 75 of 75 runs |
| strict | 1 | 3.0 ms / 22.5 ms | 3.6 ms / 17.4 ms | 11 of 75 runs | 6 of 75 runs | 12.6 ms / 161.0 ms | 15.8 ms / 210.2 ms | 41 of 75 runs | 60 of 75 runs |
| strict | 2 | 5.3 ms / 81.2 ms | 4.7 ms / 16.4 ms | 10 of 75 runs | 10 of 75 runs | 31.8 ms / 305.5 ms | 28.7 ms / 247.6 ms | 75 of 75 runs | 75 of 75 runs |
| strict | 4 | 8.7 ms / 73.4 ms | 5.9 ms / 48.1 ms | 31 of 75 runs | 12 of 75 runs | 40.4 ms / 204.8 ms | 34.6 ms / 441.5 ms | 75 of 75 runs | 75 of 75 runs |
