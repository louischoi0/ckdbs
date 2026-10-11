| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | 0.60 µs | 0.89 µs | 1.11 µs |
| INSERT again (replicate) | 0.60 µs | 0.89 µs | 1.11 µs |
| INSERT, pk named (keys below 2^40) | 0.60 µs | 0.89 µs | 1.11 µs |
| SELECT by pk | 0.60 µs | 0.89 µs | 1.11 µs |
| UPDATE by pk | 0.60 µs | 0.89 µs | 1.11 µs |
| UPDATE of one hot row | 0.60 µs | 0.89 µs | 1.11 µs |
| DELETE by pk | 0.60 µs | 0.89 µs | 1.11 µs |
| COUNT/MIN/MAX over 100 ids (walks the relation) | 0.60 µs | 0.89 µs | 2.55 µs |
| SHOW META (control) | 0.60 µs | 0.89 µs | 1.11 µs |
