
## §4 throughput

| cell | engine / configuration | writers | aggregate stmt/s | INSERT/s | point-SELECT/s | UPDATE/s | DELETE/s | multi ÷ single (per run) |
|---|---|---:|---:|---:|---:|---:|---:|---|
| A | KDS `cores = 1`, core 0 | 2 | 1,225 | 880 | 62,665 | 879 | 880 | — |
| A | KDS `cores = 2`, `creating`, core 0 | 2 | 1,156 | 815 | 48,038 | 842 | 840 | **0.944** (0.866 / 0.983 / 0.983) |
| B | KDS `cores = 1`, core 0 | 2 | 1,189 | 848 | 46,101 | 861 | 856 | — |
| B | **KDS `cores = 2`, `rotate` + listeners, core 1** | 2 | 1,177 | 852 | 55,462 | 831 | 860 | **0.990** (1.021 / 0.988 / 0.962) |
| C | KDS `cores = 1`, core 0 | 4 | 2,269 | 1,646 | 39,853 | 1,630 | 1,697 | — |
| C | **KDS `cores = 2`, `rotate` + listeners, core 1** | 4 | 2,337 | 1,697 | 41,562 | 1,685 | 1,700 | **1.030** (1.026 / 1.007 / 1.057) |
| — | PostgreSQL 16.14 (Ubuntu 16.14-0ubuntu0.24.04.1), defaults | 2 | 1,230 | 888 | 26,943 | 891 | 897 | — |
| — | PostgreSQL 16.14 (Ubuntu 16.14-0ubuntu0.24.04.1), defaults | 4 | 2,382 | 1,748 | 30,990 | 1,731 | 1,742 | — |

## §5 distributions


**insert**

| cell / configuration | n | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A `cores = 1` | 12,003 | 2,273 | 968 | 2,029 | 2,100 | 2,208 | 2,514 | 3,098 | 5,357 | 59,035 |
| A `cores = 2` | 12,003 | 2,461 | 992 | 2,053 | 2,128 | 2,248 | 2,664 | 3,490 | 6,666 | 485,842 |
| B `cores = 1` | 12,003 | 2,359 | 1,057 | 2,079 | 2,153 | 2,266 | 2,648 | 3,533 | 6,260 | 38,483 |
| **B core 1** | 12,003 | 2,347 | 981 | 2,044 | 2,121 | 2,247 | 2,671 | 3,458 | 6,125 | 117,204 |
| C `cores = 1` | 24,003 | 2,433 | 1,090 | 2,134 | 2,220 | 2,388 | 2,858 | 3,563 | 6,025 | 31,345 |
| **C core 1** | 24,003 | 2,356 | 1,005 | 2,100 | 2,177 | 2,307 | 2,698 | 3,378 | 5,168 | 25,758 |
| PostgreSQL, 2 backends | 12,000 | 2,253 | 1,013 | 2,036 | 2,105 | 2,218 | 2,464 | 3,232 | 5,051 | 23,134 |
| PostgreSQL, 4 backends | 24,000 | 2,286 | 1,045 | 2,059 | 2,136 | 2,262 | 2,534 | 3,123 | 4,986 | 20,055 |

**point-select**

| cell / configuration | n | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A `cores = 1` | 12,000 | 30 | 23 | 26 | 26 | 27 | 30 | 38 | 59 | 3,168 |
| A `cores = 2` | 12,000 | 48 | 19 | 26 | 26 | 27 | 35 | 44 | 1,040 | 77,459 |
| B `cores = 1` | 12,000 | 39 | 22 | 26 | 26 | 27 | 43 | 56 | 89 | 3,151 |
| **B core 1** | 12,000 | 33 | 21 | 24 | 25 | 26 | 35 | 41 | 64 | 3,347 |
| C `cores = 1` | 24,000 | 85 | 23 | 47 | 51 | 59 | 100 | 120 | 1,170 | 6,885 |
| **C core 1** | 24,000 | 76 | 18 | 47 | 48 | 51 | 70 | 91 | 1,122 | 10,263 |
| PostgreSQL, 2 backends | 12,000 | 74 | 44 | 50 | 55 | 83 | 121 | 146 | 180 | 1,907 |
| PostgreSQL, 4 backends | 24,000 | 126 | 40 | 86 | 116 | 144 | 186 | 221 | 337 | 2,080 |

**update**

| cell / configuration | n | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A `cores = 1` | 12,000 | 2,275 | 996 | 2,032 | 2,097 | 2,192 | 2,449 | 3,176 | 5,928 | 39,808 |
| A `cores = 2` | 12,000 | 2,367 | 996 | 2,060 | 2,132 | 2,236 | 2,519 | 3,473 | 7,226 | 48,156 |
| B `cores = 1` | 12,000 | 2,319 | 987 | 2,068 | 2,146 | 2,258 | 2,597 | 3,312 | 5,836 | 24,928 |
| **B core 1** | 12,000 | 2,402 | 983 | 2,060 | 2,139 | 2,279 | 2,802 | 3,673 | 6,604 | 99,969 |
| C `cores = 1` | 24,000 | 2,452 | 1,064 | 2,138 | 2,228 | 2,377 | 2,917 | 3,763 | 6,584 | 16,173 |
| **C core 1** | 24,000 | 2,367 | 1,038 | 2,087 | 2,160 | 2,268 | 2,711 | 3,562 | 6,298 | 24,171 |
| PostgreSQL, 2 backends | 12,000 | 2,243 | 1,041 | 2,026 | 2,094 | 2,203 | 2,456 | 3,131 | 5,173 | 31,103 |
| PostgreSQL, 4 backends | 24,000 | 2,308 | 1,053 | 2,081 | 2,167 | 2,297 | 2,548 | 3,170 | 5,028 | 26,663 |

**delete**

| cell / configuration | n | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A `cores = 1` | 6,000 | 2,269 | 989 | 2,000 | 2,065 | 2,162 | 2,466 | 3,297 | 5,883 | 37,841 |
| A `cores = 2` | 6,000 | 2,364 | 991 | 2,045 | 2,124 | 2,236 | 2,599 | 3,753 | 7,283 | 29,379 |
| B `cores = 1` | 6,000 | 2,326 | 995 | 2,041 | 2,115 | 2,241 | 2,666 | 3,416 | 6,044 | 33,898 |
| **B core 1** | 6,000 | 2,321 | 1,008 | 2,029 | 2,117 | 2,268 | 2,770 | 3,513 | 6,057 | 24,651 |
| C `cores = 1` | 12,000 | 2,354 | 1,020 | 2,065 | 2,138 | 2,248 | 2,640 | 3,510 | 6,606 | 21,363 |
| **C core 1** | 12,000 | 2,345 | 1,006 | 2,071 | 2,140 | 2,238 | 2,684 | 3,592 | 6,080 | 17,997 |
| PostgreSQL, 2 backends | 6,000 | 2,228 | 1,036 | 2,016 | 2,085 | 2,188 | 2,408 | 2,984 | 5,217 | 16,243 |
| PostgreSQL, 4 backends | 12,000 | 2,295 | 1,035 | 2,060 | 2,134 | 2,242 | 2,507 | 3,125 | 5,264 | 20,118 |

**scan**

| cell / configuration | n | p0 | p50 | p100 |
|---|---:|---:|---:|---:|
| A `cores = 1` / `cores = 2` | 6 / 6 | 252 / 267 | 280 / 502 | 1,355 / 1,460 |
| B `cores = 1` / **core 1** | 6 / 6 | 265 / 262 | 288 / 277 | 1,395 / 3,099 |
| C `cores = 1` / **core 1** | 12 / 12 | 261 / 259 | 1,246 / 1,251 | 1,422 / 1,379 |
| PostgreSQL, 2 / 4 backends | 6 / 12 | 1,123 / 1,104 | 1,138 / 1,142 | 1,381 / 3,027 |

## §8 vs PG

| shape | KDS core 0 (`cores = 1`) | KDS core 1 (`rotate` + listeners) | PostgreSQL | KDS core 0 ÷ PG | KDS core 1 ÷ PG |
|---|---:|---:|---:|---:|---:|
| 2 relations, aggregate stmt/s | 1,189 | 1,177 | 1,230 | 0.97× | 0.96× |
| 2 relations, INSERT/s | 848 | 852 | 888 | 0.96× | 0.96× |
| 2 relations, point-SELECT/s | 46,101 | 55,462 | 26,943 | 1.71× | 2.06× |
| 2 relations, INSERT p50 µs | 2,153 | 2,121 | 2,105 | 1.02× | 1.01× |
| 2 relations, point-SELECT p50 µs | 26 | 25 | 55 | 0.48× | 0.45× |
| 2 relations, point-SELECT p99 µs | 89 | 64 | 180 | 0.49× | 0.35× |
| 4 relations, aggregate stmt/s | 2,269 | 2,337 | 2,382 | 0.95× | 0.98× |
| 4 relations, INSERT/s | 1,646 | 1,697 | 1,748 | 0.94× | 0.97× |
| 4 relations, point-SELECT/s | 39,853 | 41,562 | 30,990 | 1.29× | 1.34× |
| 4 relations, INSERT p50 µs | 2,220 | 2,177 | 2,136 | 1.04× | 1.02× |
| 4 relations, point-SELECT p50 µs | 51 | 48 | 116 | 0.44× | 0.41× |
| 4 relations, point-SELECT p99 µs | 1,170 | 1,122 | 337 | 3.48× | 3.33× |

- PG t2 PG-t2-r1: PostgreSQL 16.14 (Ubuntu 16.14-0ubuntu0.24.04.1) synchronous_commit=on started 2026-08-25 12:42:22 load 0.48 wall 11.61 stmt/s 1,206 first 4,053 · 2,702 retries {} errors 0
- PG t2 PG-t2-r2: PostgreSQL 16.14 (Ubuntu 16.14-0ubuntu0.24.04.1) synchronous_commit=on started 2026-08-25 12:43:11 load 0.46 wall 11.23 stmt/s 1,247 first 3,868 · 2,495 retries {} errors 0
- PG t2 PG-t2-r3: PostgreSQL 16.14 (Ubuntu 16.14-0ubuntu0.24.04.1) synchronous_commit=on started 2026-08-25 12:44:25 load 0.48 wall 11.33 stmt/s 1,236 first 2,398 · 3,681 retries {} errors 0
- PG t4 PG-t4-r1: PostgreSQL 16.14 (Ubuntu 16.14-0ubuntu0.24.04.1) synchronous_commit=on started 2026-08-25 12:42:49 load 0.46 wall 11.70 stmt/s 2,395 first 4,306 · 4,194 · 2,698 · 4,278 retries {} errors 0
- PG t4 PG-t4-r2: PostgreSQL 16.14 (Ubuntu 16.14-0ubuntu0.24.04.1) synchronous_commit=on started 2026-08-25 12:43:33 load 0.49 wall 11.84 stmt/s 2,365 first 2,950 · 3,875 · 3,910 · 2,707 retries {} errors 0
- PG t4 PG-t4-r3: PostgreSQL 16.14 (Ubuntu 16.14-0ubuntu0.24.04.1) synchronous_commit=on started 2026-08-25 12:44:47 load 0.47 wall 11.74 stmt/s 2,385 first 4,494 · 4,550 · 2,733 · 5,271 retries {} errors 0

## §9 sweep

| cell | rows | single stmt/s | multi stmt/s | multi ÷ single | INSERT p50 µs, single → multi | point-SELECT p50 µs | first INSERT per relation on core 1, µs | retries / failed | rows lost |
|---|---:|---:|---:|---:|---|---|---|---|---|
| A | 200 | 1,224 | 1,222 | 0.998 | 2,086 → 2,109 | 26 → 26 | — | 0 / 0 | none |
| A | 2,000 | 1,225 | 1,156 | 0.944 | 2,100 → 2,128 | 26 → 26 | — | 0–0 / 0–0 | none |
| A | 10,000 | 1,308 | 1,372 | 1.048 | 2,094 → 1,838 | 26 → 26 | — | 0 / 0 | none |
| B | 200 | 1,254 | 1,218 | 0.971 | 2,085 → 2,074 | 27 → 25 | 2,420 · 4,870 | 5 / 0 | none |
| B | 2,000 | 1,189 | 1,177 | 0.990 | 2,153 → 2,121 | 26 → 25 | 3,667–3,791 · 4,093–7,575 | 7–7 / 0–0 | none |
| B | 10,000 | 1,358 | 1,339 | 0.986 | 1,824 → 1,871 | 26 → 25 | 2,217 · 3,819 | 5 / 0 | none |
| C | 200 | 2,281 | 2,219 | 0.973 | 2,216 → 2,229 | 51 → 50 | 2,449 · 7,555 · 10,690 · 14,303 | 18 / 0 | none |
| C | 2,000 | 2,269 | 2,337 | 1.030 | 2,220 → 2,177 | 51 → 48 | 2,792–3,040 · 4,318–5,643 · 7,243–7,726 · 10,528–11,071 | 18–20 / 0–0 | none |
| C | 10,000 | 2,508 | 2,317 | 0.924 | 1,999 → 2,139 | 51 → 49 | 3,147 · 2,664 · 7,612 · 11,563 | 12 / 0 | none |

### sweep refills and details

- A-creating-t2-rows200: started 2026-08-25 12:32:31 load 0.30; wall single 1.15s multi 1.15s; connects None/None; retries {}; verify ok
    insert: single p50 2,086 p99 4,161 p100 7,022 rate 904 | multi p0 1,052 p50 2,109 p99 4,568 p100 5,426 rate 887 err 0
    point-select: single p50 26 p99 1,271 p100 2,914 rate 10,030 | multi p0 23 p50 26 p99 101 p100 1,621 rate 42,552 err 0
    update: single p50 2,100 p99 4,321 p100 4,719 rate 910 | multi p0 1,050 p50 2,115 p99 5,023 p100 8,066 rate 888 err 0
    delete: single p50 2,204 p99 5,833 p100 6,491 rate 814 | multi p0 1,031 p50 2,102 p99 7,731 p100 7,948 rate 832 err 0
- A-creating-t2-rows10k: started 2026-08-25 12:34:17 load 0.46; wall single 53.50s multi 51.03s; connects None/None; retries {}; verify ok
    insert: single p50 2,094 p99 5,017 p100 28,679 rate 885 | multi p0 890 p50 1,838 p99 4,942 p100 23,764 rate 997 err 0
    point-select: single p50 26 p99 74 p100 2,260 rate 53,514 | multi p0 22 p50 26 p99 60 p100 1,732 rate 67,049 err 0
    update: single p50 1,903 p99 4,752 p100 27,970 rate 970 | multi p0 889 p50 1,845 p99 5,860 p100 19,455 rate 985 err 0
    delete: single p50 1,808 p99 5,336 p100 33,469 rate 1,005 | multi p0 955 p50 1,843 p99 6,711 p100 17,975 rate 967 err 0
- B-rotate-t2-rows200: started 2026-08-25 12:32:37 load 0.35; wall single 1.12s multi 1.15s; connects 1/3; retries {'insert': 5}; verify ok
    rowid 2/2 wait_max 1.1 ms (submit 0.0/0, to-grant 1.1/959, resume 0.0/1)
    trxid 1/1 wait_max 50.2 ms (submit 0.0/0, to-grant 50.2/119885, resume 0.0/1)
    extent 0/0 wait_max 0.0 ms (submit 0.0/0, to-grant 0.0/0, resume 0.0/0)
    insert: single p50 2,085 p99 5,014 p100 7,094 rate 879 | multi p0 999 p50 2,074 p99 3,618 p100 5,283 rate 924 err 0
    point-select: single p50 27 p99 60 p100 1,159 rate 55,880 | multi p0 22 p50 25 p99 61 p100 1,349 rate 46,328 err 0
    update: single p50 2,067 p99 4,279 p100 4,602 rate 913 | multi p0 1,057 p50 2,097 p99 8,860 p100 14,169 rate 850 err 0
    delete: single p50 2,065 p99 4,368 p100 5,769 rate 917 | multi p0 1,135 p50 2,072 p99 6,153 p100 12,265 rate 829 err 0
- B-rotate-t2-rows10k: started 2026-08-25 12:37:14 load 0.49; wall single 51.53s multi 52.27s; connects 1/10; retries {'insert': 5}; verify ok
    rowid 6/6 wait_max 2.9 ms (submit 0.0/0, to-grant 1.9/874, resume 1.2/1)
    trxid 13/13 wait_max 32.1 ms (submit 0.0/0, to-grant 32.1/73928, resume 1.0/1)
    extent 5/5 wait_max 8.5 ms (submit 0.0/0, to-grant 6.7/5, resume 3.0/1)
    insert: single p50 1,824 p99 5,266 p100 20,486 rate 995 | multi p0 904 p50 1,871 p99 5,102 p100 18,468 rate 968 err 0
    point-select: single p50 26 p99 72 p100 2,444 rate 63,106 | multi p0 20 p50 25 p99 929 p100 11,671 rate 36,014 err 0
    update: single p50 1,863 p99 6,051 p100 90,762 rate 955 | multi p0 863 p50 1,871 p99 6,004 p100 22,001 rate 959 err 0
    delete: single p50 1,836 p99 5,711 p100 21,554 rate 985 | multi p0 868 p50 1,857 p99 5,952 p100 17,961 rate 959 err 0
- C-rotate-t4-rows200: started 2026-08-25 12:32:43 load 0.45; wall single 1.23s multi 1.26s; connects 1/9; retries {'insert': 18}; verify ok
    rowid 4/4 wait_max 4.4 ms (submit 0.0/0, to-grant 2.8/712, resume 1.6/1)
    trxid 1/1 wait_max 26.5 ms (submit 0.0/0, to-grant 26.5/73620, resume 0.0/1)
    extent 0/0 wait_max 0.0 ms (submit 0.0/0, to-grant 0.0/0, resume 0.0/0)
    insert: single p50 2,216 p99 4,832 p100 6,123 rate 1,686 | multi p0 1,083 p50 2,229 p99 9,613 p100 14,303 rate 1,514 err 0
    point-select: single p50 51 p99 1,324 p100 3,189 rate 21,160 | multi p0 23 p50 50 p99 1,222 p100 1,681 rate 33,887 err 0
    update: single p50 2,203 p99 4,977 p100 8,083 rate 1,688 | multi p0 1,295 p50 2,277 p99 5,102 p100 6,509 rate 1,645 err 0
    delete: single p50 2,334 p99 4,571 p100 4,657 rate 1,607 | multi p0 1,401 p50 2,217 p99 3,752 p100 4,498 rate 1,706 err 0
- C-rotate-t4-rows10k: started 2026-08-25 12:40:31 load 0.48; wall single 55.82s multi 60.42s; connects 1/7; retries {'insert': 12}; verify ok
    rowid 12/12 wait_max 5.0 ms (submit 0.0/0, to-grant 2.1/2257, resume 3.8/1)
    trxid 25/25 wait_max 28.3 ms (submit 0.0/0, to-grant 28.3/76326, resume 2.6/1)
    extent 10/10 wait_max 9.5 ms (submit 0.0/0, to-grant 7.7/2, resume 2.1/1)
    insert: single p50 1,999 p99 6,482 p100 17,963 rate 1,775 | multi p0 989 p50 2,139 p99 6,042 p100 19,087 rate 1,708 err 0
    point-select: single p50 51 p99 1,063 p100 7,270 rate 28,035 | multi p0 23 p50 49 p99 749 p100 4,994 rate 48,588 err 0
    update: single p50 1,966 p99 6,006 p100 16,868 rate 1,842 | multi p0 985 p50 2,189 p99 6,137 p100 16,764 rate 1,655 err 0
    delete: single p50 1,904 p99 5,408 p100 15,530 rate 1,926 | multi p0 1,007 p50 2,160 p99 6,317 p100 25,996 rate 1,663 err 0
