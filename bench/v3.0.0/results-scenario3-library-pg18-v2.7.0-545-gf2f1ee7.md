# Scenario 3 (library), PostgreSQL 18.6 floor, at `v2.7.0-545-gf2f1ee7`

**One line.** On a serial single-connection read workload, with the
equalities indexed, KDS at `cores = 1` runs **0.95x to 0.97x** of PostgreSQL
18.6 (geometric mean over twelve shapes, at all three row counts), and
without indexes it falls to **0.87x, 0.77x and 0.62x** at 200, 1,000 and
10,000 loans, because a KDS walk costs about **70 ns per row against
PostgreSQL's 34 ns**. The 8-core KDS cell is 0.02 to 0.05 lower still at every
size. The indexed equalities on `loans` land within 7 % of each other
(`loans-by-user` 10,881 qps KDS, 10,917 PostgreSQL at 10,000 loans) even though
KDS gains 8.5x from the index and PostgreSQL 4.4x; KDS wins outright on
`exists-correlated`, and with indexes on `join-loan-user`, `count-by-user` and
`join-no-literal` at 10,000. PostgreSQL is the
floor: indexed, KDS's `cores = 1` geometric mean is 3 to 5 % under it (not over it) and it
clears it only on those joins, `count-by-user` and `exists-correlated`;
unindexed it is at 0.46x to 0.96x on every shape but `exists-correlated`.

Driver: `tools/pg_scenario3_library.py`, unmodified, `--loans {200, 1000,
10000} --index-mode {none, all} --seed 1`, three runs per cell. KDS side:
`bench/v3.0.0/results-scenario3-library-v2.7.0-531-g9a0525d.md` (the 9a0525d
file; 36 cells, medians of three, `cores` 1 and 8). This is the first
PostgreSQL run of the shape and becomes its baseline.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-09-30; run 1 08:27:05 to 08:28:18 UTC, run 2 08:28:23 to 08:37:22 (cells `n10000-*-r2` re-run at 08:36:26 to 08:37:17), run 3 08:37:22 to 08:39:16 (six cells re-run there); per-cell start times in section 9 |
| Worktree / branch | `bench-pg-floor` / `worktree-bench-pg-floor` |
| HEAD | `f2f1ee7`, `git describe --tags` = `v2.7.0-545-gf2f1ee7`; tree clean at the start (`git status` empty), nothing under `src/`, `include/`, `tools/` edited or built by this run |
| Engine the KDS numbers describe | `9a0525d` (`v2.7.0-531-g9a0525d`). `git diff --stat 9a0525d HEAD -- src include CMakeLists.txt tools` lists one file, `tools/fk_overhead_benchmark.py` (7 insertions, 5 deletions), so no engine source, build file or scenario/pg driver differs at HEAD (`archive/.../stamp.txt`) |
| PostgreSQL | **18.6** (`SELECT version()`: `PostgreSQL 18.6 on x86_64-pc-linux-gnu, compiled by gcc 13.3.0`), built from the verified source tarball into `$HOME/.local/pgsql-18.6`; `./configure --prefix=... --without-icu --without-readline --without-zlib --with-openssl`, stock `-O2` and no other flag (`pg_config --configure`, `pg-build-evidence.txt`). The host lacks those dev packages and has no passwordless sudo |
| Do the omitted libraries touch this workload? | No, verified: `ldd postgres` links only `libssl`/`libcrypto`; the cluster is `initdb --locale=C --encoding=UTF8`, every database `datlocprovider = c` (libc, collation `C`), so ICU has no role; `default_toast_compression = pglz` (built in) and `wal_compression = off`, so zlib has none; readline is `psql`'s line editor and the drivers speak the wire protocol directly (`tools/pg_wire.py`). Evidence: `pg-build-evidence.txt` |
| Cluster | `tools/pg_setup.sh`, unmodified, with `PGROOT=/home/cdkbs/pg-bench-18`, `PGPORT=15700` (not 15432, held by an unrelated server; checked free with `ss -ltn`); trust auth on loopback; **a fresh `initdb` per cell** (`pg_setup.sh destroy --yes` then `init`), the counterpart of the KDS side's fresh data file per cell. Settings are PostgreSQL's defaults plus what `pg_setup.sh` writes (port, listen_addresses, unix_socket_directories, logging_collector, log_min_duration_statement = -1, log_line_prefix); the full `pg_settings WHERE source <> 'default'` dump is `logs/<cell>.pg_settings` in the archive. In force: `synchronous_commit = on`, `fsync = on`, `wal_sync_method = fdatasync`, `full_page_writes = on`, `shared_buffers = 128MB`, `max_wal_size = 1GB`, `max_connections = 100`, `autovacuum = on`, `io_method = worker` |
| Device | data directory `/home/cdkbs/pg-bench-18/data`, `/dev/root`, `ext4` (`df -T`, archived in `stamp.txt` and `pg-build-evidence.txt`); never tmpfs. Deleted after each cell |
| Host | 8 logical CPUs (AMD EPYC 9V74, 1 socket x 4 cores x 2 threads), Linux 7.0.0-1014-azure. **The client processes and the servers share those eight CPUs**; PostgreSQL runs one process per connection over all of them |
| Host quiet | before each cell the run script waited until no `cc1plus` / `cmake --build` / `ctest` process existed and the one-minute load was at most 1.6, and recorded `/proc/loadavg` and the `pgrep` result (per-cell tables below). **A 2-second monitor ran through the whole matrix** (`logs/monitor.log`, `validity.py`): a cell with a foreign process above 10 % CPU or a build/test process inside its window is discarded and re-run. Other sessions build and test on this host: a first pass of the matrix (08:04 to 08:20 UTC, no monitor yet) had three cells overlap a foreign `ctest` and was **discarded in full**; in the monitored pass 7 cells had a foreign build/test process in their window and 5 more started right after one (one-minute load 2.5 to 4.8) and were re-run, 12 cells in all (`contaminated/`). The window `validity.py` checks opens at the cell's `start` line, before the quiet-wait, so a cell that waited is flagged for what it waited out: `s0-pg-on-r1`'s first execution (711.2 tps) waited 100 s for a `ctest` that ended at 08:22:55 and then ran 08:22:56 to 08:23:06 with no foreign process in the monitor. **One reported cell has flagged samples**: the re-run of `s3-pg-n10000-all-r3` (08:38:24 to 08:39:16, 10 of 26 samples in `validity.txt`: a foreign `kds_tests` at 15 to 17 % CPU at 08:38:31 to 08:38:33, then a `claude`/`node` process at 10 to 126 % from 08:38:49 to 08:39:03). It was not re-run again; dropping it moves no scenario 3 ratio by more than 0.02 and no geometric mean by more than 0.003. Every other reported cell has no flagged sample. The monitor's limit: it sees CPU-busy processes at 2 s spacing, so a sub-2 s burst or a process that has only just become busy is not excluded |
| Drivers | unmodified `tools/pg_scenario*.py` (and, for the `--bookers 1` KDS cells, `tools/scenario2_freight.py`); the run scripts are in the archive |

## 2. What was run

Eighteen cells: `--loans` {200, 1000, 10000} x `--index-mode` {none, all} x
three runs, a fresh `initdb` each. The driver creates `users`, `books`,
`reservations` and `loans` (`--matches 5` rows per key, so at 10,000 loans:
2,000 users, 2,000 books, 5,000 reservations), loads them one row per
statement, then (`--index-when after`, the default) builds the indexes for
`all` and runs `ANALYZE` (default on), and times 200 operations of each of
twelve shapes on one connection. Command:

    python3 tools/pg_scenario3_library.py --port 15700 --user $(id -un) --loans <n> \
        --index-mode <none|all> --seed 1 --json <cell>.json

`--user` had to be passed: the twin's default is `ec2-user`, a role the
scratch cluster does not have. `--index-mode all` declares the same eight
indexes as KDS's `all`, including composite and covering ones
(`loans_user_cov_*` etc.); PostgreSQL's planner decides whether to use them.
The KDS cells use `--index-when after` too, so both load first and index
after.

**Row-set axis (rule 9), stated once.** `--loans N` is the bulk relation;
users and books are N/5 and reservations N/2, the same ratios on both
engines. All three sizes are columns in every matrix below.

**Not measured, and why.** The PostgreSQL twin has no `--verify`,
`--assert-index-reads` or `--server-indexes`; the plan of five shapes is
recorded by `EXPLAIN` (section 6) instead, all 18 cells exit 0 and reply
without error (0 error replies in every phase of every cell). `--cabin`
and the `single`/`composite`/`covering` modes were not run, as on the KDS
side.

## 3. Throughput

**Indexed, the two engines are within 5 % of each other at every size;
unindexed, KDS is slower and the gap widens with rows.**

PostgreSQL, derived qps (1,000,000 / mean µs, exact for a serial
connection), median of three runs:

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 15,748 qps | 16,026 qps | 14,815 qps | 14,577 qps | 15,552 qps | 15,552 qps |
| loans-by-user | 13,441 qps | 12,151 qps | 9,699 qps | 11,547 qps | 2,472 qps | 10,917 qps |
| loans-by-book | 13,850 qps | 12,739 qps | 9,804 qps | 12,195 qps | 2,376 qps | 12,005 qps |
| resv-by-user | 15,552 qps | 14,881 qps | 12,594 qps | 13,699 qps | 4,350 qps | 13,514 qps |
| books-by-author | 15,337 qps | 14,306 qps | 14,164 qps | 12,837 qps | 7,022 qps | 12,500 qps |
| books-by-genre | 16,000 qps | 14,620 qps | 11,976 qps | 11,001 qps | 3,130 qps | 3,893 qps |
| loans-by-daterange | 11,933 qps | 11,682 qps | 5,747 qps | 5,970 qps | 1,061 qps | 1,067 qps |
| overdue | 12,438 qps | 11,062 qps | 6,623 qps | 7,605 qps | 1,143 qps | 1,943 qps |
| join-loan-user | 10,091 qps | 9,434 qps | 7,358 qps | 8,446 qps | 2,340 qps | 9,009 qps |
| join-no-literal | 4,509 qps | 4,174 qps | 3,448 qps | 3,228 qps | 1,482 qps | 1,441 qps |
| exists-correlated | 5,731 qps | 5,397 qps | 3,338 qps | 5,000 qps | 759 qps | 5,147 qps |
| count-by-user | 13,532 qps | 12,210 qps | 9,823 qps | 11,962 qps | 2,461 qps | 11,628 qps |

KDS `cores = 1` over PostgreSQL, ratio of median qps (above 1.00x KDS is
faster); the KDS cells are the 9a0525d medians, shown in full further down this section
(the last table of this section) for reading the ratios against:

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 0.88x | 0.86x | 0.92x | 0.95x | 0.90x | 0.93x |
| loans-by-user | 0.76x | 0.92x | 0.66x | 0.96x | 0.52x | 1.00x |
| loans-by-book | 0.77x | 0.91x | 0.69x | 0.95x | 0.53x | 0.98x |
| resv-by-user | 0.82x | 0.89x | 0.73x | 0.95x | 0.54x | 0.96x |
| books-by-author | 0.83x | 0.83x | 0.58x | 0.95x | 0.61x | 0.94x |
| books-by-genre | 0.83x | 0.91x | 0.63x | 0.84x | 0.53x | 0.54x |
| loans-by-daterange | 0.79x | 0.80x | 0.66x | 0.65x | 0.46x | 0.48x |
| overdue | 0.83x | 1.02x | 0.67x | 0.85x | 0.53x | 0.55x |
| join-loan-user | 0.92x | 1.07x | 0.83x | 1.19x | 0.54x | 1.12x |
| join-no-literal | 0.84x | 0.88x | 0.83x | 1.07x | 0.53x | 2.50x |
| exists-correlated | 1.36x | 1.31x | 1.69x | 1.24x | 2.06x | 1.38x |
| count-by-user | 0.96x | 1.20x | 0.75x | 1.21x | 0.54x | 1.23x |

The same at KDS `cores = 8` (a single serial connection lands on one core,
so no gain is expected and the 9a0525d file measured about 5 % lost):

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 0.83x | 0.82x | 0.88x | 0.90x | 0.87x | 0.88x |
| loans-by-user | 0.71x | 0.89x | 0.57x | 0.92x | 0.49x | 0.94x |
| loans-by-book | 0.73x | 0.89x | 0.60x | 0.93x | 0.51x | 0.93x |
| resv-by-user | 0.77x | 0.87x | 0.66x | 0.91x | 0.54x | 0.92x |
| books-by-author | 0.79x | 0.84x | 0.69x | 0.92x | 0.57x | 0.91x |
| books-by-genre | 0.79x | 0.86x | 0.67x | 0.80x | 0.54x | 0.53x |
| loans-by-daterange | 0.74x | 0.75x | 0.62x | 0.60x | 0.46x | 0.46x |
| overdue | 0.77x | 1.01x | 0.61x | 0.77x | 0.51x | 0.52x |
| join-loan-user | 0.84x | 1.01x | 0.73x | 1.14x | 0.51x | 1.09x |
| join-no-literal | 0.78x | 0.82x | 0.75x | 1.02x | 0.50x | 2.32x |
| exists-correlated | 1.26x | 1.22x | 1.53x | 1.32x | 1.97x | 1.29x |
| count-by-user | 0.89x | 1.15x | 0.67x | 1.18x | 0.50x | 1.16x |

Summary of the twelve shapes per cell, geometric mean of the ratio:

| KDS cores | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| 1 core | 0.87x (1 of 12 ahead) | 0.96x (3 of 12 ahead) | 0.77x (1 of 12 ahead) | 0.97x (4 of 12 ahead) | 0.62x (1 of 12 ahead) | 0.95x (4 of 12 ahead) |
| 8 cores | 0.82x (1 of 12 ahead) | 0.92x (2 of 12 ahead) | 0.72x (1 of 12 ahead) | 0.93x (3 of 12 ahead) | 0.60x (1 of 12 ahead) | 0.91x (4 of 12 ahead) |

KDS `cores = 1`, derived qps, same cells (9a0525d, median of three):

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 13,928 qps | 13,774 qps | 13,661 qps | 13,831 qps | 14,025 qps | 14,388 qps |
| loans-by-user | 10,173 qps | 11,186 qps | 6,443 qps | 11,074 qps | 1,277 qps | 10,881 qps |
| loans-by-book | 10,730 qps | 11,614 qps | 6,739 qps | 11,601 qps | 1,270 qps | 11,737 qps |
| resv-by-user | 12,706 qps | 13,175 qps | 9,225 qps | 13,004 qps | 2,341 qps | 12,970 qps |
| books-by-author | 12,674 qps | 11,919 qps | 8,278 qps | 12,210 qps | 4,257 qps | 11,792 qps |
| books-by-genre | 13,298 qps | 13,316 qps | 7,570 qps | 9,285 qps | 1,646 qps | 2,101 qps |
| loans-by-daterange | 9,398 qps | 9,355 qps | 3,814 qps | 3,855 qps | 492 qps | 514 qps |
| overdue | 10,288 qps | 11,325 qps | 4,407 qps | 6,431 qps | 606 qps | 1,070 qps |
| join-loan-user | 9,242 qps | 10,091 qps | 6,135 qps | 10,070 qps | 1,257 qps | 10,132 qps |
| join-no-literal | 3,798 qps | 3,686 qps | 2,847 qps | 3,457 qps | 779 qps | 3,608 qps |
| exists-correlated | 7,788 qps | 7,057 qps | 5,631 qps | 6,215 qps | 1,564 qps | 7,128 qps |
| count-by-user | 13,038 qps | 14,599 qps | 7,358 qps | 14,430 qps | 1,323 qps | 14,306 qps |

**How to read the table.** `pk-user` is the control: a pk lookup does not
depend on the row count or on the index mode, and it reads 0.86x to 0.95x
here, so the fixed cost of a KDS statement (parse, one lookup, reply,
socket) is 4 to 10 µs above PostgreSQL's (mean 62 to 69 µs for PostgreSQL, 70 to 73 µs for KDS). Ratios inside about
0.90x to 1.10x are not findings at that fixed offset and at the PostgreSQL
spreads in section 8, which reach 39 % in the small-n cells (the host's
slow mode, which the 9a0525d file also saw).

## 4. What the index buys, per engine

The ratio all / none of median qps, inside each engine (above 1.00x the
index helps). The pk control reads 0.98x to 1.03x on both engines and is
the floor:

| shape | PG n=200 | PG n=1,000 | PG n=10,000 | KDS c1 n=200 | KDS c1 n=1,000 | KDS c1 n=10,000 |
|---|---|---|---|---|---|---|
| pk-user | 1.02x | 0.98x | 1.00x | 0.99x | 1.01x | 1.03x |
| loans-by-user | 0.90x | 1.19x | 4.42x | 1.10x | 1.72x | 8.52x |
| loans-by-book | 0.92x | 1.24x | 5.05x | 1.08x | 1.72x | 9.24x |
| resv-by-user | 0.96x | 1.09x | 3.11x | 1.04x | 1.41x | 5.54x |
| books-by-author | 0.93x | 0.91x | 1.78x | 0.94x | 1.47x | 2.77x |
| books-by-genre | 0.91x | 0.92x | 1.24x | 1.00x | 1.23x | 1.28x |
| loans-by-daterange | 0.98x | 1.04x | 1.01x | 1.00x | 1.01x | 1.05x |
| overdue | 0.89x | 1.15x | 1.70x | 1.10x | 1.46x | 1.77x |
| join-loan-user | 0.93x | 1.15x | 3.85x | 1.09x | 1.64x | 8.06x |
| join-no-literal | 0.93x | 0.94x | 0.97x | 0.97x | 1.21x | 4.63x |
| exists-correlated | 0.94x | 1.50x | 6.78x | 0.91x | 1.10x | 4.56x |
| count-by-user | 0.90x | 1.22x | 4.72x | 1.12x | 1.96x | 10.81x |

**KDS gains more from the index than PostgreSQL because its unindexed
walk is slower, and both end at the same place.** At 10,000 loans
`loans-by-user` is 1,277 qps unindexed on KDS against 2,472 on PostgreSQL;
indexed it is 10,881 against 10,917. Fitting `mean = fixed + rows x per-row`
to the n=200 and n=10,000 unindexed cells (loans are scanned, so rows = N):

| shape | PG fixed | PG per row | KDS c1 fixed | KDS c1 per row | KDS / PG per-row cost |
|---|---|---|---|---|---|
| loans-by-user | 67.7 µs | 33.7 ns/row | 84.3 µs | 69.9 ns/row | 2.07x |
| loans-by-book | 65.1 µs | 35.6 ns/row | 79.0 µs | 70.9 ns/row | 1.99x |
| count-by-user | 67.1 µs | 33.9 ns/row | 62.8 µs | 69.3 ns/row | 2.04x |
| loans-by-daterange | 66.3 µs | 87.6 ns/row | 67.1 µs | 196.6 ns/row | 2.24x |
| overdue | 64.2 µs | 81.1 ns/row | 65.5 µs | 158.5 ns/row | 1.96x |

An unindexed KDS equality costs **63 to 84 µs fixed plus about 70 ns per row of
`loans`** (158 to 197 ns on the two range shapes) where PostgreSQL costs 64 to 68 µs plus
34 ns (81 to 88 ns); the ratio is 1.96x to
2.24x on all five loans-scanning shapes, so it is a property of the walk,
not of one statement. The fixed part is nearly equal, which is why the
indexed cases converge: an indexed read on either engine is one round trip.
PostgreSQL's planner is also not fooled into using the index where it does
not pay: at 200 loans the `EXPLAIN` in the `all` cells is a `Seq Scan` for
`loans-by-user`, `books-by-genre` and `overdue` (section 6), so its indexed
and unindexed numbers agree (0.89x to 0.91x, inside the floor). KDS has no
plan choice and gets 1.00x to 1.10x on the same shapes.

## 5. Where KDS wins, and where it loses by more than the fixed offset

- **Wins.** `exists-correlated` 1.24x to 2.06x in every cell,
  `join-no-literal` 2.50x at 10,000 with indexes (3,608 qps against 1,441:
  PostgreSQL's plan is a hash join over a full `loans` scan, its own cost
  estimate's choice, while KDS runs a `Range` step with an `IndexProbe`), and
  `join-loan-user` and `count-by-user` at 1.07x to 1.23x once indexed. The
  `EXISTS` shape is a nested loop with an index-only scan on PostgreSQL only
  when indexed (5,147 qps); unindexed it plans a hash aggregate (759 qps at
  10,000) and KDS is 2.06x ahead.
- **Losses beyond the offset.** The unindexed walks (0.46x to 0.61x at
  10,000, every shape but `pk-user` and `exists-correlated`), `books-by-genre` at 10,000 with indexes (0.54x:
  a low-selectivity equality of about 130 rows on which PostgreSQL's bitmap
  scan reaches 3,893 qps and KDS's `IndexProbe` 2,101), `overdue` with
  indexes (0.55x) and `loans-by-daterange`, which no index covers and which
  is a plain `Scan` on both (0.46x to 0.80x: KDS 197 ns per row against
  PostgreSQL's 88). A KDS read that returns many rows pays per row what a
  walk pays; the index does not change the per-row cost of what it returns.

## 6. Plans PostgreSQL chose

`EXPLAIN` top nodes the driver records for five shapes (run 1 of each cell;
the section 7 tables mark the shapes it does not record):

| cell | loans-by-user | books-by-genre | overdue | join-no-literal | exists-correlated |
|---|---|---|---|---|---|
| n=200, all | Seq Scan | Seq Scan | Seq Scan | Hash Join | Hash Join |
| n=10,000, none | Seq Scan | Seq Scan | Seq Scan | Hash Join | Hash Join over a HashAggregate |
| n=10,000, all | Bitmap Heap Scan (index `loans_user_cov`) | Bitmap Heap Scan | Bitmap Heap Scan | Hash Join (seq scan of `loans`) | Nested Loop Semi Join (index-only scan on `users` pk) |

## 7. Percentiles

PostgreSQL, run 1 of every cell, 200 operations per shape, microseconds. The
runs 2 and 3 tables are in the archive JSON; their spreads are in section 8.
Run 1 of `n=200 all` met the host's slow mode (its `pk-user` p50 is 82.5 µs
against 60.3 µs in `none`), and section 3's medians vote it out; this table
does not.

| n | mode | shape | p0 | p25 | p50 | p95 | p99 | plan (EXPLAIN top node, recorded for 5 shapes) |
|---|---|---|---|---|---|---|---|---|
| 200 loans | none | pk-user | 58.5 µs | 59.8 µs | 60.3 µs | 74.5 µs | 87.0 µs | not recorded by the driver |
| 200 loans | none | loans-by-user | 61.5 µs | 68.2 µs | 71.6 µs | 84.9 µs | 89.4 µs | Seq Scan on loans_082706 |
| 200 loans | none | loans-by-book | 63.1 µs | 67.4 µs | 70.5 µs | 80.6 µs | 90.5 µs | not recorded by the driver |
| 200 loans | none | resv-by-user | 57.2 µs | 60.9 µs | 62.2 µs | 77.2 µs | 97.7 µs | not recorded by the driver |
| 200 loans | none | books-by-author | 57.4 µs | 60.0 µs | 64.2 µs | 78.7 µs | 84.2 µs | not recorded by the driver |
| 200 loans | none | books-by-genre | 57.2 µs | 59.2 µs | 61.1 µs | 73.1 µs | 79.3 µs | Seq Scan on books_082706 |
| 200 loans | none | loans-by-daterange | 71.5 µs | 77.8 µs | 81.0 µs | 97.1 µs | 104.3 µs | not recorded by the driver |
| 200 loans | none | overdue | 69.9 µs | 77.1 µs | 79.8 µs | 93.7 µs | 100.1 µs | Seq Scan on loans_082706 |
| 200 loans | none | join-loan-user | 84.3 µs | 93.5 µs | 97.0 µs | 113.3 µs | 140.3 µs | not recorded by the driver |
| 200 loans | none | join-no-literal | 210.3 µs | 213.5 µs | 215.6 µs | 231.8 µs | 238.8 µs | Hash Join |
| 200 loans | none | exists-correlated | 166.5 µs | 169.4 µs | 170.6 µs | 186.6 µs | 197.6 µs | Hash Join |
| 200 loans | none | count-by-user | 69.0 µs | 69.8 µs | 70.3 µs | 85.0 µs | 90.2 µs | not recorded by the driver |
| 200 loans | all | pk-user | 80.6 µs | 81.8 µs | 82.5 µs | 101.5 µs | 107.9 µs | not recorded by the driver |
| 200 loans | all | loans-by-user | 90.3 µs | 99.2 µs | 103.2 µs | 124.1 µs | 132.5 µs | Seq Scan on loans_082708 |
| 200 loans | all | loans-by-book | 92.7 µs | 98.6 µs | 101.6 µs | 118.0 µs | 125.0 µs | not recorded by the driver |
| 200 loans | all | resv-by-user | 61.6 µs | 87.4 µs | 89.0 µs | 108.7 µs | 132.6 µs | not recorded by the driver |
| 200 loans | all | books-by-author | 61.7 µs | 64.6 µs | 69.2 µs | 84.4 µs | 93.4 µs | not recorded by the driver |
| 200 loans | all | books-by-genre | 63.3 µs | 65.0 µs | 67.1 µs | 80.7 µs | 89.6 µs | Seq Scan on books_082708 |
| 200 loans | all | loans-by-daterange | 72.5 µs | 79.7 µs | 82.9 µs | 98.3 µs | 104.8 µs | not recorded by the driver |
| 200 loans | all | overdue | 79.0 µs | 86.3 µs | 89.0 µs | 101.7 µs | 111.5 µs | Seq Scan on loans_082708 |
| 200 loans | all | join-loan-user | 90.0 µs | 99.8 µs | 103.1 µs | 117.5 µs | 122.1 µs | not recorded by the driver |
| 200 loans | all | join-no-literal | 224.3 µs | 243.6 µs | 256.2 µs | 277.6 µs | 287.0 µs | Hash Join |
| 200 loans | all | exists-correlated | 204.8 µs | 207.6 µs | 209.6 µs | 230.9 µs | 236.3 µs | Hash Join |
| 200 loans | all | count-by-user | 100.7 µs | 102.2 µs | 102.9 µs | 122.2 µs | 128.8 µs | not recorded by the driver |
| 1,000 loans | none | pk-user | 62.3 µs | 63.4 µs | 64.0 µs | 78.6 µs | 92.6 µs | not recorded by the driver |
| 1,000 loans | none | loans-by-user | 90.2 µs | 96.3 µs | 100.2 µs | 122.2 µs | 128.8 µs | Seq Scan on loans_082710 |
| 1,000 loans | none | loans-by-book | 91.1 µs | 96.6 µs | 99.8 µs | 121.7 µs | 125.8 µs | not recorded by the driver |
| 1,000 loans | none | resv-by-user | 69.8 µs | 74.1 µs | 76.2 µs | 96.6 µs | 101.9 µs | not recorded by the driver |
| 1,000 loans | none | books-by-author | 60.3 µs | 65.5 µs | 68.2 µs | 89.3 µs | 97.3 µs | not recorded by the driver |
| 1,000 loans | none | books-by-genre | 70.9 µs | 78.7 µs | 81.2 µs | 99.2 µs | 116.4 µs | Seq Scan on books_082710 |
| 1,000 loans | none | loans-by-daterange | 102.2 µs | 158.0 µs | 165.0 µs | 185.7 µs | 205.8 µs | not recorded by the driver |
| 1,000 loans | none | overdue | 102.1 µs | 141.9 µs | 146.3 µs | 161.8 µs | 171.4 µs | Seq Scan on loans_082710 |
| 1,000 loans | none | join-loan-user | 119.6 µs | 126.2 µs | 130.1 µs | 156.7 µs | 265.2 µs | not recorded by the driver |
| 1,000 loans | none | join-no-literal | 273.4 µs | 277.8 µs | 281.6 µs | 306.1 µs | 366.9 µs | Hash Join |
| 1,000 loans | none | exists-correlated | 282.2 µs | 291.6 µs | 296.0 µs | 316.3 µs | 324.9 µs | Hash Join |
| 1,000 loans | none | count-by-user | 95.9 µs | 97.3 µs | 97.8 µs | 112.4 µs | 115.9 µs | not recorded by the driver |
| 1,000 loans | all | pk-user | 64.3 µs | 65.5 µs | 66.1 µs | 81.0 µs | 87.4 µs | not recorded by the driver |
| 1,000 loans | all | loans-by-user | 71.0 µs | 80.1 µs | 83.7 µs | 102.4 µs | 115.3 µs | Bitmap Heap Scan on loans_082714 |
| 1,000 loans | all | loans-by-book | 71.0 µs | 78.2 µs | 80.9 µs | 93.6 µs | 103.0 µs | not recorded by the driver |
| 1,000 loans | all | resv-by-user | 61.9 µs | 67.6 µs | 72.1 µs | 84.5 µs | 94.0 µs | not recorded by the driver |
| 1,000 loans | all | books-by-author | 67.7 µs | 73.0 µs | 75.7 µs | 90.0 µs | 99.3 µs | not recorded by the driver |
| 1,000 loans | all | books-by-genre | 80.1 µs | 87.2 µs | 89.6 µs | 103.7 µs | 109.9 µs | Seq Scan on books_082714 |
| 1,000 loans | all | loans-by-daterange | 111.5 µs | 160.6 µs | 166.2 µs | 186.1 µs | 189.8 µs | not recorded by the driver |
| 1,000 loans | all | overdue | 81.2 µs | 125.0 µs | 134.8 µs | 182.7 µs | 201.4 µs | Bitmap Heap Scan on loans_082714 |
| 1,000 loans | all | join-loan-user | 103.7 µs | 112.8 µs | 115.4 µs | 136.7 µs | 154.3 µs | not recorded by the driver |
| 1,000 loans | all | join-no-literal | 294.6 µs | 298.9 µs | 301.8 µs | 317.2 µs | 347.0 µs | Hash Join |
| 1,000 loans | all | exists-correlated | 191.9 µs | 193.5 µs | 195.0 µs | 211.5 µs | 222.7 µs | Nested Loop Semi Join |
| 1,000 loans | all | count-by-user | 78.2 µs | 80.8 µs | 81.5 µs | 97.2 µs | 101.2 µs | not recorded by the driver |
| 10,000 loans | none | pk-user | 59.2 µs | 60.3 µs | 61.0 µs | 81.5 µs | 90.7 µs | not recorded by the driver |
| 10,000 loans | none | loans-by-user | 385.5 µs | 396.3 µs | 407.0 µs | 428.8 µs | 469.5 µs | Seq Scan on loans_082718 |
| 10,000 loans | none | loans-by-book | 403.9 µs | 414.4 µs | 424.0 µs | 446.0 µs | 460.7 µs | not recorded by the driver |
| 10,000 loans | none | resv-by-user | 214.5 µs | 223.2 µs | 228.6 µs | 251.6 µs | 259.3 µs | not recorded by the driver |
| 10,000 loans | none | books-by-author | 131.1 µs | 137.7 µs | 142.2 µs | 163.5 µs | 171.2 µs | not recorded by the driver |
| 10,000 loans | none | books-by-genre | 293.6 µs | 317.2 µs | 325.4 µs | 357.1 µs | 427.1 µs | Seq Scan on books_082718 |
| 10,000 loans | none | loans-by-daterange | 496.2 µs | 927.6 µs | 944.6 µs | 988.1 µs | 1,266.3 µs | not recorded by the driver |
| 10,000 loans | none | overdue | 467.1 µs | 891.5 µs | 904.2 µs | 929.2 µs | 936.2 µs | Seq Scan on loans_082718 |
| 10,000 loans | none | join-loan-user | 416.3 µs | 426.2 µs | 441.2 µs | 458.9 µs | 465.5 µs | not recorded by the driver |
| 10,000 loans | none | join-no-literal | 649.6 µs | 669.3 µs | 676.6 µs | 693.0 µs | 714.0 µs | Hash Join |
| 10,000 loans | none | exists-correlated | 1,299.2 µs | 1,325.6 µs | 1,333.8 µs | 1,366.1 µs | 1,418.6 µs | Hash Join |
| 10,000 loans | none | count-by-user | 391.1 µs | 398.1 µs | 407.7 µs | 450.4 µs | 601.1 µs | not recorded by the driver |
| 10,000 loans | all | pk-user | 59.0 µs | 60.2 µs | 60.9 µs | 77.5 µs | 91.6 µs | not recorded by the driver |
| 10,000 loans | all | loans-by-user | 70.9 µs | 81.9 µs | 88.1 µs | 113.8 µs | 140.8 µs | Bitmap Heap Scan on loans_082748 |
| 10,000 loans | all | loans-by-book | 65.4 µs | 75.1 µs | 78.8 µs | 97.6 µs | 103.5 µs | not recorded by the driver |
| 10,000 loans | all | resv-by-user | 60.9 µs | 68.4 µs | 70.8 µs | 89.9 µs | 95.2 µs | not recorded by the driver |
| 10,000 loans | all | books-by-author | 64.7 µs | 73.4 µs | 75.9 µs | 90.9 µs | 111.5 µs | not recorded by the driver |
| 10,000 loans | all | books-by-genre | 228.0 µs | 247.6 µs | 255.6 µs | 271.0 µs | 289.0 µs | Bitmap Heap Scan on books_082748 |
| 10,000 loans | all | loans-by-daterange | 495.7 µs | 903.2 µs | 919.1 µs | 959.6 µs | 1,168.6 µs | not recorded by the driver |
| 10,000 loans | all | overdue | 84.3 µs | 499.9 µs | 524.6 µs | 573.4 µs | 582.1 µs | Bitmap Heap Scan on loans_082748 |
| 10,000 loans | all | join-loan-user | 96.7 µs | 104.5 µs | 108.5 µs | 126.6 µs | 144.2 µs | not recorded by the driver |
| 10,000 loans | all | join-no-literal | 663.5 µs | 677.2 µs | 684.2 µs | 759.2 µs | 813.5 µs | Hash Join |
| 10,000 loans | all | exists-correlated | 185.1 µs | 187.1 µs | 189.1 µs | 208.1 µs | 247.8 µs | Nested Loop Semi Join |
| 10,000 loans | all | count-by-user | 78.1 µs | 80.7 µs | 82.2 µs | 109.2 µs | 141.1 µs | not recorded by the driver |

Setup phases, PostgreSQL run 1 (one statement per row, an fsync each, which
the 1.4 ms load mean shows; KDS's load phase is not compared because its
phase is the same shape and adds nothing to a read benchmark):

| n | mode | ddl | load | create-index |
|---|---|---|---|---|
| 200 loans | none | 4 ops, mean 2,808.8 µs | 380 ops, mean 1,411.0 µs | - |
| 200 loans | all | 4 ops, mean 2,820.5 µs | 380 ops, mean 1,362.0 µs | 12 ops, mean 1,667.7 µs |
| 1,000 loans | none | 4 ops, mean 4,995.3 µs | 1,900 ops, mean 1,370.3 µs | - |
| 1,000 loans | all | 4 ops, mean 3,817.0 µs | 1,900 ops, mean 1,418.4 µs | 12 ops, mean 2,014.4 µs |
| 10,000 loans | none | 4 ops, mean 3,283.7 µs | 19,000 ops, mean 1,444.2 µs | - |
| 10,000 loans | all | 4 ops, mean 3,399.9 µs | 19,000 ops, mean 1,529.8 µs | 12 ops, mean 5,563.3 µs |

## 8. Where the time goes

A read shape has no durability, write or lock wait, so the decomposition is
the one the tables above already carry: a **fixed** cost per statement
(`pk-user`: PostgreSQL 62 to 69 µs mean, KDS 70 to 73 µs, client-side Python and
socket included in both) and a **per-row** cost of the walk (PostgreSQL 34 ns,
KDS 70 ns per `loans` row; section 4). The three-run spread of the
PostgreSQL cells is the floor:

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 1.3 % | 39.0 % | 1.9 % | 1.0 % | 2.2 % | 1.1 % |
| loans-by-user | 21.8 % | 29.5 % | 0.5 % | 2.5 % | 2.2 % | 2.5 % |
| loans-by-book | 13.5 % | 31.0 % | 0.7 % | 2.0 % | 1.5 % | 3.1 % |
| resv-by-user | 24.0 % | 36.0 % | 0.3 % | 3.5 % | 2.0 % | 1.6 % |
| books-by-author | 35.0 % | 0.3 % | 7.8 % | 2.1 % | 2.6 % | 2.3 % |
| books-by-genre | 37.3 % | 1.3 % | 32.9 % | 2.8 % | 3.9 % | 5.1 % |
| loans-by-daterange | 30.2 % | 2.6 % | 17.3 % | 2.0 % | 3.1 % | 1.4 % |
| overdue | 31.5 % | 2.4 % | 21.4 % | 11.1 % | 2.6 % | 2.5 % |
| join-loan-user | 28.9 % | 1.4 % | 21.3 % | 2.5 % | 3.0 % | 2.8 % |
| join-no-literal | 14.7 % | 9.9 % | 11.6 % | 2.8 % | 3.3 % | 2.4 % |
| exists-correlated | 17.4 % | 16.0 % | 12.2 % | 2.0 % | 2.6 % | 0.5 % |
| count-by-user | 34.5 % | 32.0 % | 5.2 % | 3.0 % | 4.3 % | 1.6 % |

## 9. Host stamps

Every cell has a monitored window with no foreign process (`validity.py`)
except `s3-pg-n10000-all-r3`, which has 10 flagged samples of 26 (section 1,
"Host quiet"):

| cell | precheck UTC | loadavg 1/5/15 | cc1plus / cmake --build / ctest |
|---|---|---|---|
| `s3-pg-n200-none-r1` | 2026-09-30T08:27:05Z | 1.24 / 1.36 / 1.68 | none |
| `s3-pg-n200-all-r1` | 2026-09-30T08:27:06Z | 1.22 / 1.36 / 1.68 | none |
| `s3-pg-n1000-none-r1` | 2026-09-30T08:27:09Z | 1.22 / 1.36 / 1.68 | none |
| `s3-pg-n1000-all-r1` | 2026-09-30T08:27:13Z | 1.20 / 1.35 / 1.67 | none |
| `s3-pg-n10000-none-r1` | 2026-09-30T08:27:17Z | 1.19 / 1.35 / 1.67 | none |
| `s3-pg-n10000-all-r1` | 2026-09-30T08:27:47Z | 1.11 / 1.31 / 1.65 | none |
| `s3-pg-n200-none-r2` | 2026-09-30T08:28:23Z | 1.14 / 1.29 / 1.63 | none |
| `s3-pg-n200-all-r2` | 2026-09-30T08:28:25Z | 1.14 / 1.29 / 1.63 | none |
| `s3-pg-n1000-none-r2` | 2026-09-30T08:28:28Z | 1.13 / 1.29 / 1.62 | none |
| `s3-pg-n1000-all-r2` | 2026-09-30T08:28:32Z | 1.12 / 1.28 / 1.62 | none |
| `s3-pg-n10000-none-r2` | 2026-09-30T08:36:26Z | 1.22 / 3.06 / 2.64 | none |
| `s3-pg-n10000-all-r2` | 2026-09-30T08:36:52Z | 1.13 / 2.87 / 2.59 | none |
| `s3-pg-n200-none-r3` | 2026-09-30T08:37:22Z | 1.26 / 2.73 / 2.55 | none |
| `s3-pg-n200-all-r3` | 2026-09-30T08:37:57Z | 0.85 / 2.47 / 2.47 | none |
| `s3-pg-n1000-none-r3` | 2026-09-30T08:37:24Z | 1.26 / 2.73 / 2.55 | none |
| `s3-pg-n1000-all-r3` | 2026-09-30T08:37:28Z | 1.32 / 2.72 / 2.55 | none |
| `s3-pg-n10000-none-r3` | 2026-09-30T08:37:59Z | 0.85 / 2.47 / 2.47 | none |
| `s3-pg-n10000-all-r3` | 2026-09-30T08:38:24Z | 1.12 / 2.40 / 2.45 | none |

## 10. What the run says about the engine

1. **The index is what makes a KDS read competitive.** Unindexed, KDS walks
   a relation at half PostgreSQL's speed per row (70 against 34 ns); indexed,
   the two are within 5 % on the equalities. The 9a0525d file's finding that
   a secondary index turns a per-row cost into a constant holds on both
   engines, and holds harder on KDS (8.1x to 10.8x at 10,000 rows on the four
   `loans` shapes against PostgreSQL's 3.9x to 5.1x) only because KDS starts
   lower.
2. **The per-row walk is the one KDS number that should not stay.** The
   ratio is 1.96x to 2.24x on five different statements; a query the index
   does not cover (`loans-by-daterange`) costs KDS 197 ns per row against
   88. That is an optimisation target with a measured floor: PostgreSQL
   reads a tuple in about half the time on the same host, same client.
3. **The fixed round trip is 4 to 10 µs more on KDS** (`pk-user` 0.86x to
   0.95x, `cores = 8` 0.82x to 0.90x). It does not grow with rows, and it
   is small against every walked shape.
4. **KDS's join and semi-join paths beat PostgreSQL's plans on the shapes
   where PostgreSQL picks a hash join over a full scan**: 2.50x at 10,000
   (`join-no-literal`) and 1.24x to 2.06x (`exists-correlated`). One
   client, one host, one data set: not evidence of a planner deficiency in
   PostgreSQL, only that KDS's fixed access path is the better choice here.
5. **Nothing reached a limit of the engine.** Every cell is one serial
   client at 60 to 2,000 µs per statement; about 15,000 qps on `pk-user`
   (PostgreSQL) and 14,000 (KDS) are round-trip bounds of the Python client on
   loopback, not database limits, and no cell stresses concurrency, durability
   or a row count beyond 10,000.

Archive: `bench/v3.0.0/archive/scenario3-pg18-v2.7.0-545-gf2f1ee7/`.
