| shape | 1,000 rows | 10,000 rows |
|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.30, +0.50] | +0.00 µs [-0.20, +0.20] |
| INSERT again (replicate) | -0.10 µs [-0.30, +0.10] | +0.00 µs [-0.20, +0.10] |
| INSERT, pk named (keys below 2^40) | -0.10 µs [-0.30, +0.20] | +0.10 µs [-0.10, +0.15] |
| SELECT by pk | -0.40 µs [-1.30, +0.00] | -0.10 µs [-0.40, +0.15] |
| UPDATE by pk | -0.10 µs [-0.50, +0.50] | +0.00 µs [-0.45, +0.35] |
| UPDATE of one hot row | +0.00 µs [-0.40, +0.50] | -0.05 µs [-0.60, +0.30] |
| DELETE by pk | -0.10 µs [-0.20, +0.00] | +0.00 µs [-0.10, +0.10] |
| COUNT/MIN/MAX over 100 ids (walks the relation) | +4.00 µs [+2.90, +5.00] | +36.90 µs [+29.30, +41.10] |
| SHOW META (control) | +0.20 µs [+0.00, +0.30] | -0.10 µs [-0.20, +0.00] |
