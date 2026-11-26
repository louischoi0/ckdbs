| phase | ops | mean | p50 | p99 | share of the summed statement time |
|---|---|---|---|---|---|
| booking | 3,402 | 685.6 µs | 682.0 µs | 1,084.2 µs | - |
| cargo-lookup | 3,402 | 55.2 µs | 49.9 µs | 80.1 µs | 8.6 % |
| credit-lookup | 3,402 | 45.4 µs | 43.0 µs | 57.3 µs | 7.1 % |
| capacity-read | 3,402 | 43.2 µs | 41.5 µs | 51.1 µs | 6.8 % |
| recipe-read | 3,402 | 84.3 µs | 82.9 µs | 98.3 µs | 13.2 % |
| freight-insert | 3,000 | 38.1 µs | 35.4 µs | 48.4 µs | 5.3 % |
| charge-insert | 16,907 | 29.3 µs | 28.2 µs | 39.2 µs | 22.8 % |
| operation-update | 3,000 | 34.7 µs | 33.5 µs | 41.9 µs | 4.8 % |
| org-update | 3,000 | 31.7 µs | 31.3 µs | 35.8 µs | 4.4 % |
| commit | 3,000 | 186.9 µs | 146.1 µs | 428.1 µs | 25.8 % |
| manifest-scan | 60 | 130.4 µs | 113.2 µs | 550.9 µs | 0.4 % |
| voyage-rollup | 60 | 113.4 µs | 93.0 µs | 837.6 µs | 0.3 % |
| customer-statement | 30 | 526.5 µs | 589.3 µs | 1,071.9 µs | 0.7 % |
