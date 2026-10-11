**1,000 rows**, `COUNT/MIN/MAX` over 100 ids, p50 as the median of the per-run p50s (`cores = 1`):

| data set | binary in slot A (port 32011) | binary in slot B (port 32012) | slot A p50 | slot B p50 | runs |
|---|---|---|---|---|---|
| main (A in slot A, B in slot B) | A | B | 145.6 µs | 147.1 µs | 32 |
| null (A in both) | A | A | 146.1 µs | 145.9 µs | 15 |
| flipped (B in slot A, A in slot B) | B | A | 150.9 µs | 147.0 µs | 13 |

**10,000 rows**, `COUNT/MIN/MAX` over 100 ids, p50 as the median of the per-run p50s (`cores = 1`):

| data set | binary in slot A (port 32011) | binary in slot B (port 32012) | slot A p50 | slot B p50 | runs |
|---|---|---|---|---|---|
| main (A in slot A, B in slot B) | A | B | 805.2 µs | 819.3 µs | 32 |
| null (A in both) | A | A | 799.7 µs | 804.0 µs | 16 |
| flipped (B in slot A, A in slot B) | B | A | 839.3 µs | 804.5 µs | 16 |
