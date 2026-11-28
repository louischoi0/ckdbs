| cell | base CPU/stmt | close CPU/stmt | ctl CPU/stmt | close vs base (paired, 95% CI) | ctl vs base (paired, 95% CI) | base / close / ctl server-CPU-bound throughput |
|---|---|---|---|---|---|---|
| OH-bt-read | 6.51 +-0.03 us | 6.69 +-0.17 us | 6.53 +-0.04 us | +2.8 % +-2.8 | +0.3 % +-0.6 | 153,569/s / 149,414/s / 153,159/s |
| OH-bt-write | 7.41 +-0.09 us | 7.39 +-0.02 us | 7.36 +-0.04 us | -0.2 % +-1.1 | -0.6 % +-1.1 | 134,974/s / 135,314/s / 135,869/s |
| OH-hp-read | 7.22 +-0.04 us | 7.29 +-0.02 us | 7.20 +-0.04 us | +1.0 % +-0.5 | -0.3 % +-0.5 | 138,465/s / 137,096/s / 138,947/s |
| OH-hp-write | 8.14 +-0.12 us | 8.12 +-0.11 us | 8.17 +-0.18 us | -0.1 % +-2.0 | +0.4 % +-2.6 | 122,857/s / 123,079/s / 122,407/s |
| OH-e-read | 7.31 +-0.02 us | 7.54 +-0.04 us | 7.28 +-0.02 us | +3.1 % +-0.6 | -0.5 % +-0.3 | 136,757/s / 132,587/s / 137,382/s |
| OH-e-write | 8.31 +-0.14 us | 8.40 +-0.16 us | 8.23 +-0.04 us | +1.2 % +-2.4 | -0.9 % +-1.4 | 120,333/s / 119,059/s / 121,498/s |
| OH-w-read | 7.87 +-0.05 us | 8.09 +-0.05 us | 7.85 +-0.03 us | +2.8 % +-0.8 | -0.2 % +-0.6 | 127,050/s / 123,598/s / 127,365/s |
| OH-w-write | 9.47 +-0.16 us | 9.59 +-0.17 us | 9.51 +-0.16 us | +1.4 % +-1.9 | +0.5 % +-2.3 | 105,588/s / 104,240/s / 105,160/s |
| OH-bt-conflict | 5.89 +-0.04 us | 5.90 +-0.02 us | 5.91 +-0.08 us | +0.2 % +-0.7 | +0.3 % +-1.5 | 169,790/s / 169,466/s / 169,233/s |
| OH-bi-insert | 6.21 +-0.10 us | 6.21 +-0.03 us | 6.17 +-0.03 us | +0.2 % +-1.3 | -0.6 % +-1.4 | 161,160/s / 160,997/s / 162,197/s |

| cell | arm | n | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| OH-bt-read | base | 216,000 | 15.9 us | 16.7 us | 16.9 us | 17.8 us | 18.5 us | 1,361.9 us |
| OH-bt-read | close | 216,000 | 15.9 us | 16.7 us | 16.9 us | 17.9 us | 18.7 us | 2,050.5 us |
| OH-bt-read | ctl | 216,000 | 15.6 us | 16.6 us | 16.8 us | 17.8 us | 18.6 us | 1,743.3 us |
| OH-bt-write | base | 162,000 | 14.5 us | 15.6 us | 15.7 us | 17.0 us | 19.4 us | 4,275.4 us |
| OH-bt-write | close | 162,000 | 14.8 us | 15.5 us | 15.7 us | 16.8 us | 18.6 us | 1,063.5 us |
| OH-bt-write | ctl | 162,000 | 14.5 us | 15.5 us | 15.7 us | 16.8 us | 18.8 us | 2,331.6 us |
| OH-hp-read | base | 216,000 | 16.3 us | 17.3 us | 17.6 us | 18.5 us | 19.3 us | 1,919.0 us |
| OH-hp-read | close | 216,000 | 16.4 us | 17.4 us | 17.6 us | 18.6 us | 19.3 us | 575.8 us |
| OH-hp-read | ctl | 216,000 | 16.3 us | 17.3 us | 17.5 us | 18.5 us | 19.3 us | 2,022.8 us |
| OH-hp-write | base | 162,000 | 15.3 us | 16.2 us | 16.5 us | 17.7 us | 20.3 us | 7,048.8 us |
| OH-hp-write | close | 162,000 | 15.0 us | 16.2 us | 16.5 us | 17.6 us | 19.9 us | 8,020.8 us |
| OH-hp-write | ctl | 162,000 | 15.2 us | 16.2 us | 16.5 us | 17.7 us | 20.0 us | 2,129.9 us |
| OH-e-read | base | 216,000 | 16.8 us | 17.5 us | 17.6 us | 18.6 us | 19.3 us | 276.4 us |
| OH-e-read | close | 216,000 | 16.9 us | 17.7 us | 17.9 us | 18.9 us | 19.8 us | 2,785.7 us |
| OH-e-read | ctl | 216,000 | 16.6 us | 17.4 us | 17.6 us | 18.5 us | 19.2 us | 1,574.4 us |
| OH-e-write | base | 162,000 | 15.7 us | 16.5 us | 16.7 us | 17.9 us | 20.5 us | 12,132.3 us |
| OH-e-write | close | 162,000 | 15.8 us | 16.5 us | 16.7 us | 17.9 us | 20.7 us | 51,404.9 us |
| OH-e-write | ctl | 162,000 | 15.6 us | 16.5 us | 16.7 us | 17.8 us | 20.2 us | 12,673.0 us |
| OH-w-read | base | 216,000 | 17.2 us | 18.1 us | 18.3 us | 19.4 us | 20.2 us | 2,261.4 us |
| OH-w-read | close | 216,000 | 17.5 us | 18.3 us | 18.5 us | 19.5 us | 20.2 us | 1,628.2 us |
| OH-w-read | ctl | 216,000 | 17.0 us | 18.1 us | 18.3 us | 19.3 us | 20.0 us | 344.7 us |
| OH-w-write | base | 162,000 | 16.8 us | 17.7 us | 17.9 us | 19.7 us | 22.1 us | 2,993.0 us |
| OH-w-write | close | 162,000 | 16.8 us | 17.7 us | 18.0 us | 19.6 us | 21.3 us | 6,462.8 us |
| OH-w-write | ctl | 162,000 | 16.8 us | 17.7 us | 17.9 us | 19.6 us | 21.3 us | 567.1 us |
| OH-bt-conflict | base | 162,000 | 12.9 us | 14.2 us | 14.4 us | 15.3 us | 16.3 us | 2,024.1 us |
| OH-bt-conflict | close | 162,000 | 12.7 us | 14.2 us | 14.4 us | 15.3 us | 16.2 us | 1,495.1 us |
| OH-bt-conflict | ctl | 162,000 | 13.0 us | 14.1 us | 14.3 us | 15.3 us | 16.2 us | 2,068.0 us |
| OH-bi-insert | base | 108,000 | 13.7 us | 14.3 us | 14.5 us | 15.4 us | 18.1 us | 2,012.1 us |
| OH-bi-insert | close | 108,000 | 13.7 us | 14.3 us | 14.5 us | 15.4 us | 18.3 us | 742.9 us |
| OH-bi-insert | ctl | 108,000 | 13.7 us | 14.3 us | 14.5 us | 15.4 us | 18.5 us | 1,383.3 us |

load average (1 min) over 540 blocks: min 1.06, max 1.31, mean 1.21; build/ctest processes seen: 0
