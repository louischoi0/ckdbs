| phase | ops | now | spread | v3.5.0 run (other host) | spread | now over v3.5.0 run (host + engine) |
|---|---|---|---|---|---|---|
| load-exchanges | 2 | 2,728.8 qps | 41.8 % | 1,651.6 qps | 21.3 % | 1.65x |
| load-symbols | 8 | 6,510.0 qps | 66.7 % | 2,010.0 qps | 22.2 % | 3.24x |
| load-sessions | 7,560 | 68,740.8 qps | 49.8 % | 21,771.0 qps | 12.1 % | 3.16x |
| load-bars | 60,480 | 44,817.5 qps | 12.6 % | 21,266.2 qps | 4.8 % | 2.11x |
| load-stats | 60,480 | 45,144.6 qps | 15.6 % | 22,215.4 qps | 4.5 % | 2.03x |
| load-models | 8 | 2,240.7 qps | 35.4 % | 1,475.5 qps | 17.0 % | 1.52x |
| backtest-read | 360 | 1,096.2 qps | 5.2 % | 609.0 qps | 1.2 % | 1.80x |
| result-insert | 2,872 | 4,621.7 qps | 6.1 % | 1,798.6 qps | 3.4 % | 2.57x |
| backtest-replay | 360 | 1,062.1 qps | 30.1 % | 634.7 qps | 6.5 % | 1.67x |
| compare-all | 1 | 157.7 qps | 11.7 % | 91.3 qps | 0.5 % | 1.73x |
| compare-one | 32 | 1,559.5 qps | 18.0 % | 925.1 qps | 4.0 % | 1.69x |
| read-bar-lookup | 200 | 23,031.6 qps | 40.1 % | 13,238.3 qps | 38.7 % | 1.74x |
| read-bar-range | 200 | 1,924.5 qps | 5.6 % | 1,138.2 qps | 2.9 % | 1.69x |
| read-symbol-history | 10 | 45.5 qps | 23.5 % | 27.3 qps | 5.1 % | 1.67x |
| read-day-slice | 200 | 912.8 qps | 17.4 % | 685.7 qps | 3.5 % | 1.33x |
| read-join-point | 200 | 23,106.5 qps | 27.5 % | 14,018.5 qps | 6.8 % | 1.65x |
| read-join-exists | 10 | 337.6 qps | 2.6 % | 195.6 qps | 1.4 % | 1.73x |
| agg-global | 10 | 277.4 qps | 14.7 % | 172.7 qps | 2.7 % | 1.61x |
| agg-by-symbol | 10 | 211.6 qps | 26.6 % | 135.4 qps | 2.2 % | 1.56x |
| agg-by-session | 10 | 71.8 qps | 5.8 % | 40.6 qps | 1.7 % | 1.77x |
| agg-day-slice | 200 | 1,111.6 qps | 17.0 % | 700.5 qps | 1.4 % | 1.59x |
| agg-distinct | 10 | 252.8 qps | 10.5 % | 167.4 qps | 0.7 % | 1.51x |
| sync | 1 | 457.3 qps | 24.6 % | 295.0 qps | 13.9 % | 1.55x |
