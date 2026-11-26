| phase | ops | mean | p50 | p99 | share of the summed statement time |
|---|---|---|---|---|---|
| booking | 3,326 | 1,891.0 µs | 1,572.7 µs | 5,029.7 µs | - |
| cargo-lookup | 3,326 | 139.1 µs | 90.1 µs | 470.4 µs | 8.1 % |
| credit-lookup | 3,326 | 131.2 µs | 78.9 µs | 452.0 µs | 7.6 % |
| capacity-read | 3,326 | 132.1 µs | 79.4 µs | 470.3 µs | 7.7 % |
| recipe-read | 3,326 | 169.7 µs | 116.1 µs | 503.0 µs | 9.9 % |
| freight-insert | 3,000 | 140.7 µs | 122.8 µs | 455.2 µs | 7.4 % |
| charge-insert | 16,912 | 114.3 µs | 60.9 µs | 454.3 µs | 33.8 % |
| operation-update | 3,000 | 113.2 µs | 58.1 µs | 452.4 µs | 5.9 % |
| org-update | 3,000 | 108.2 µs | 56.1 µs | 444.4 µs | 5.7 % |
| commit | 3,000 | 263.6 µs | 214.0 µs | 750.9 µs | 13.8 % |
| manifest-scan | 20 | 69.5 µs | 40.9 µs | 263.0 µs | 0.0 % |
| voyage-rollup | 20 | 59.9 µs | 40.6 µs | 254.9 µs | 0.0 % |
| customer-statement | 10 | 43.0 µs | 39.9 µs | 61.1 µs | 0.0 % |
