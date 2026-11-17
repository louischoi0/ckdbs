runs: 10; loads per cell: min 0.23 max 0.7; bad units: 0

### Off the set's shape: paired deltas, server CPU per unit

| shape | rows | units per run | A | B - A (SE) | A2 - A (SE) | B - A % | reading |
|---|---|---|---|---|---|---|---|
| scan-in2 | 1,000 | 1,200 | 102.14 us | -1.81 us (0.88) | +0.48 us (0.40) | -1.8 % | B faster |
| scan-or | 1,000 | 1,200 | 107.68 us | -0.87 us (0.54) | +0.26 us (0.50) | -0.8 % | inside the floor |
| pk-trail | 1,000 | 3,000 | 22.65 us | +0.02 us (0.19) | +0.25 us (0.15) | +0.1 % | inside the floor |
| update-in2 | 1,000 | 1,200 | 134.74 us | -0.02 us (2.05) | -0.81 us (1.00) | -0.0 % | inside the floor |

### Off the set's shape: pooled over the sizes

| shape | paired run-cells | A | B - A (SE) | A2 - A (SE) | B - A % | 2 SE % | largest cost not excluded, B - A + 2 SE |
|---|---|---|---|---|---|---|---|
| scan-in2 | 10 | 102.14 us | -1.81 us (0.88) | +0.48 us (0.40) | -1.8 % | 1.7 % | -0.05 us, -0.0 % |
| scan-or | 10 | 107.68 us | -0.87 us (0.54) | +0.26 us (0.50) | -0.8 % | 1.0 % | +0.22 us, +0.2 % |
| pk-trail | 10 | 22.65 us | +0.02 us (0.19) | +0.25 us (0.15) | +0.1 % | 1.6 % | +0.40 us, +1.7 % |
| update-in2 | 10 | 134.74 us | -0.02 us (2.05) | -0.81 us (1.00) | -0.0 % | 3.0 % | +4.09 us, +3.0 % |

### The shapes CU moved: server CPU per unit

| shape | rows | units per run | A | A2 | B | A / B | B - A (SE) |
|---|---|---|---|---|---|---|---|

### Every cell: server CPU and client latency (microseconds)

| shape | rows | arm | units/run | runs | server CPU per unit (mean of run means) | min..max over runs | client mean | p0 | p25 | p50 | p75 | p90 | p99 | max | bad |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| scan-in2 | 1,000 | A | 1,200 | 10 | 102.14 us | 98.23..120.34 us | 140.3 us | 121.6 us | 127.6 us | 132.1 us | 140.8 us | 155.0 us | 237.0 us | 5,773.2 us | 0 |
| scan-in2 | 1,000 | B | 1,200 | 10 | 100.34 us | 97.20..112.11 us | 137.3 us | 119.4 us | 126.1 us | 130.2 us | 138.8 us | 152.6 us | 228.6 us | 1,534.1 us | 0 |
| scan-in2 | 1,000 | A2 | 1,200 | 10 | 102.62 us | 98.22..119.82 us | 140.1 us | 121.2 us | 127.7 us | 132.0 us | 141.1 us | 155.9 us | 233.7 us | 6,064.8 us | 0 |
| scan-or | 1,000 | A | 1,200 | 10 | 107.68 us | 105.94..110.87 us | 144.1 us | 128.0 us | 134.5 us | 138.1 us | 144.6 us | 157.5 us | 229.2 us | 1,665.5 us | 0 |
| scan-or | 1,000 | B | 1,200 | 10 | 106.82 us | 103.10..110.17 us | 144.3 us | 124.8 us | 132.7 us | 136.3 us | 143.3 us | 157.8 us | 245.8 us | 3,569.2 us | 0 |
| scan-or | 1,000 | A2 | 1,200 | 10 | 107.95 us | 105.94..111.56 us | 144.4 us | 128.2 us | 134.7 us | 138.1 us | 144.7 us | 158.1 us | 230.9 us | 939.6 us | 0 |
| pk-trail | 1,000 | A | 3,000 | 10 | 22.65 us | 22.05..23.71 us | 55.8 us | 48.0 us | 50.9 us | 52.4 us | 56.2 us | 62.0 us | 83.6 us | 1,033.4 us | 0 |
| pk-trail | 1,000 | B | 3,000 | 10 | 22.67 us | 21.77..23.45 us | 55.8 us | 47.3 us | 50.8 us | 52.3 us | 55.8 us | 61.6 us | 86.2 us | 2,015.5 us | 0 |
| pk-trail | 1,000 | A2 | 3,000 | 10 | 22.90 us | 21.73..24.26 us | 56.0 us | 47.9 us | 50.9 us | 52.4 us | 55.7 us | 61.2 us | 84.7 us | 2,440.6 us | 0 |
| update-in2 | 1,000 | A | 1,200 | 10 | 134.74 us | 124.20..199.50 us | 163.4 us | 134.2 us | 142.8 us | 157.0 us | 168.9 us | 191.7 us | 282.0 us | 1,066.5 us | 0 |
| update-in2 | 1,000 | B | 1,200 | 10 | 134.72 us | 122.74..184.23 us | 165.6 us | 131.3 us | 140.7 us | 155.6 us | 169.3 us | 200.2 us | 310.6 us | 5,146.9 us | 0 |
| update-in2 | 1,000 | A2 | 1,200 | 10 | 133.93 us | 123.14..190.50 us | 164.3 us | 133.8 us | 141.0 us | 156.6 us | 169.0 us | 192.7 us | 341.8 us | 6,422.9 us | 0 |
