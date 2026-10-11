| cell | shape | BJ close B' (757), median of runs | this A (763), same binary | this B (767) |
|---|---|---|---|---|
| cores = 1, 1,000 rows | UPDATE by pk | 51.75 µs | 52.55 µs (+0.80) | 52.25 µs (+0.50) |
| cores = 1, 1,000 rows | INSERT, pk named | 49.40 µs | 49.20 µs (-0.20) | 49.10 µs (-0.30) |
| cores = 1, 1,000 rows | SELECT by pk | 66.95 µs | 67.60 µs (+0.65) | 67.20 µs (+0.25) |
| cores = 1, 1,000 rows | DELETE by pk | 48.80 µs | 49.10 µs (+0.30) | 49.15 µs (+0.35) |
| cores = 1, 1,000 rows | SHOW META (control) | 50.10 µs | 50.10 µs (+0.00) | 50.30 µs (+0.20) |
| cores = 1, 10,000 rows | UPDATE by pk | 52.55 µs | 52.50 µs (-0.05) | 52.55 µs (+0.00) |
| cores = 1, 10,000 rows | INSERT, pk named | 49.60 µs | 49.00 µs (-0.60) | 49.10 µs (-0.50) |
| cores = 1, 10,000 rows | SELECT by pk | 64.80 µs | 64.90 µs (+0.10) | 64.60 µs (-0.20) |
| cores = 1, 10,000 rows | DELETE by pk | 49.30 µs | 49.20 µs (-0.10) | 49.10 µs (-0.20) |
| cores = 1, 10,000 rows | SHOW META (control) | 50.25 µs | 50.30 µs (+0.05) | 50.30 µs (+0.05) |
| cores = 2, 1,000 rows | UPDATE by pk | 53.60 µs | 54.20 µs (+0.60) | 53.60 µs (+0.00) |
| cores = 2, 1,000 rows | INSERT, pk named | 50.05 µs | 49.85 µs (-0.20) | 49.85 µs (-0.20) |
| cores = 2, 1,000 rows | SELECT by pk | 68.05 µs | 69.05 µs (+1.00) | 68.60 µs (+0.55) |
| cores = 2, 1,000 rows | DELETE by pk | 49.30 µs | 49.60 µs (+0.30) | 49.50 µs (+0.20) |
| cores = 2, 1,000 rows | SHOW META (control) | 50.50 µs | 50.70 µs (+0.20) | 50.90 µs (+0.40) |
