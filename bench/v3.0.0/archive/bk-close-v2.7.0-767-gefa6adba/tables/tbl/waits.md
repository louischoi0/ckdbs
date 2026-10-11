| shape | relaxed p50 (A) | server CPU per statement (A) | outside the server's CPU (client, loopback, wake-up) | strict p50 (A), rows as stated | commit wait: strict − relaxed p50 | commit wait as a share of the strict p50 |
|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.2 µs | 28.5 µs | 20.6 µs | 1004 µs (200 rows); 1007 µs (1,000 rows); 1008 µs (10,000 rows) | +955 µs; +958 µs; +958 µs | 95 %; 95 %; 95 % |
| INSERT, pk named | 49.2 µs | 28.8 µs | 20.4 µs | 1006 µs (200 rows); 1011 µs (1,000 rows); 1004 µs (10,000 rows) | +957 µs; +962 µs; +955 µs | 95 %; 95 %; 95 % |
| UPDATE by pk | 52.5 µs | 31.0 µs | 21.6 µs | 1011 µs (200 rows); 1013 µs (1,000 rows); 1008 µs (10,000 rows) | +959 µs; +961 µs; +956 µs | 95 %; 95 %; 95 % |
| UPDATE of one hot row | 52.0 µs | 30.7 µs | 21.3 µs | 1005 µs (200 rows); 1012 µs (1,000 rows); 1008 µs (10,000 rows) | +953 µs; +960 µs; +956 µs | 95 %; 95 %; 95 % |
| DELETE by pk | 49.1 µs | 28.1 µs | 21.0 µs | 994 µs (200 rows); 995 µs (1,000 rows); 999 µs (10,000 rows) | +945 µs; +946 µs; +950 µs | 95 %; 95 %; 95 % |
| SELECT by pk | 67.6 µs | 35.7 µs | 31.9 µs | 69 µs (200 rows); 66 µs (1,000 rows); 66 µs (10,000 rows) | +4 µs; -2 µs; +1 µs | 6 %; -3 %; 2 % |
| COUNT/MIN/MAX over 100 ids | 145.6 µs | 112.6 µs | 32.9 µs | 87 µs (200 rows); 147 µs (1,000 rows); 807 µs (10,000 rows) | +0 µs; +1 µs; +2 µs | 1 %; 1 %; 0 % |
| SHOW META (control) | 50.1 µs | 32.9 µs | 17.2 µs | 52 µs (200 rows); 52 µs (1,000 rows); 51 µs (10,000 rows) | +1 µs; +2 µs; +1 µs | 2 %; 4 %; 1 % |
