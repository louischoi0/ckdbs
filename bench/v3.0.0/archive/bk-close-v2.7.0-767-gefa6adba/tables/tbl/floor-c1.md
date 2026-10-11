| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | 0.89 µs | 0.67 µs | 0.60 µs |
| INSERT again (replicate) | 0.89 µs | 0.67 µs | 0.60 µs |
| INSERT, pk named (keys below 2^40) | 0.89 µs | 0.67 µs | 0.60 µs |
| SELECT by pk | 0.89 µs | 0.67 µs | 0.60 µs |
| UPDATE by pk | 0.89 µs | 0.67 µs | 0.60 µs |
| UPDATE of one hot row | 0.89 µs | 0.67 µs | 0.60 µs |
| DELETE by pk | 0.89 µs | 0.67 µs | 0.60 µs |
| COUNT/MIN/MAX over 100 ids (walks the relation) | 0.89 µs | 0.75 µs | 5.65 µs |
| SHOW META (control) | 0.89 µs | 0.67 µs | 0.60 µs |
