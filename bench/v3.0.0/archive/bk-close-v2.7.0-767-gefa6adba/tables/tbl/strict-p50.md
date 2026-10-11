| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | 1004 → 1002 µs | 1007 → 1009 µs | 1008 → 1012 µs |
| INSERT again (replicate) | 1016 → 1016 µs | 1015 → 1015 µs | 1010 → 1003 µs |
| INSERT, pk named (keys below 2^40) | 1006 → 1005 µs | 1011 → 1010 µs | 1004 → 1006 µs |
| SELECT by pk | 69 → 68 µs | 66 → 65 µs | 66 → 66 µs |
| UPDATE by pk | 1011 → 1011 µs | 1013 → 1017 µs | 1008 → 1009 µs |
| UPDATE of one hot row | 1005 → 1006 µs | 1012 → 1010 µs | 1008 → 1012 µs |
| DELETE by pk | 994 → 1001 µs | 995 → 1000 µs | 999 → 1000 µs |
| COUNT/MIN/MAX over 100 ids (walks the relation) | 87 → 88 µs | 147 → 152 µs | 807 → 854 µs |
| SHOW META (control) | 52 → 51 µs | 52 → 52 µs | 51 → 51 µs |
