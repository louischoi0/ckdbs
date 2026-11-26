| phase | ops | mean | p50 | p99 | share of the summed statement time |
|---|---|---|---|---|---|
| booking | 3,355 | 673.6 µs | 594.5 µs | 1,795.3 µs | - |
| cargo-lookup | 3,355 | 41.9 µs | 37.1 µs | 100.5 µs | 6.7 % |
| credit-lookup | 3,355 | 38.8 µs | 30.5 µs | 137.2 µs | 6.2 % |
| capacity-read | 3,355 | 39.5 µs | 29.5 µs | 143.7 µs | 6.3 % |
| recipe-read | 3,355 | 80.5 µs | 67.0 µs | 268.5 µs | 12.9 % |
| freight-insert | 3,000 | 48.2 µs | 26.9 µs | 354.8 µs | 6.9 % |
| charge-insert | 16,916 | 26.4 µs | 20.6 µs | 105.0 µs | 21.3 % |
| operation-update | 3,000 | 31.2 µs | 24.1 µs | 119.5 µs | 4.5 % |
| org-update | 3,000 | 29.3 µs | 22.0 µs | 105.0 µs | 4.2 % |
| commit | 3,000 | 216.4 µs | 196.4 µs | 497.2 µs | 30.9 % |
| manifest-scan | 20 | 72.9 µs | 42.2 µs | 614.9 µs | 0.1 % |
| voyage-rollup | 20 | 55.5 µs | 38.3 µs | 356.1 µs | 0.1 % |
| customer-statement | 10 | 52.5 µs | 43.8 µs | 117.4 µs | 0.0 % |
