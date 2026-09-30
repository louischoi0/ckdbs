# Scenario 1 (backtest), PostgreSQL 18.6 standalone floor, at `v2.7.0-545-gf2f1ee7`

**One line.** KDS refuses this scenario (SUS-1: `daily_stats` and
`model_results` are created `HEAP`), so there is **no KDS counterpart**, and
this file is a standalone PostgreSQL 18.6 floor for the workload at the
driver's defaults, one run: 128,536 rows loaded at 14,000 rows/s, a
359-period, 8-model backtest in 4.9 s, an unindexed non-pk equality
(`day-slice`) at 414 qps that a btree index lifts 22x to 8,892 qps, and a
durable insert rate of 659 rows/s in autocommit that batching lifts 28x to
18,757 rows/s at 1,000 rows per transaction. The next KDS run of this
scenario, once its schema is BTREE-only, is read against this file.

Driver: `tools/pg_scenario1_backtest.py`, unmodified, **defaults** (the task
was "once per the driver's defaults"): `--years 30 --symbols 8 --exchanges 2
--rebalance 21 --top-k 3 --batch 200 --ops 200 --qps-ops 100 --warm-keys 8
--write-batches 1,10,100,1000 --write-ops 2000 --connections 1,2,4,8
--conn-ops 200 --seed 1`, sweep, aggregates and verify on. KDS side: none;
`bench/v3.0.0/results-scenario1-backtest-v2.7.0-531-g9a0525d.md` records the
three cells that stopped at `CREATE TABLE ... HEAP` (byte 219,
`HEAP storage is suspended (SUS-1)`), so nothing is compared.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-09-30, 08:40:39 to 08:41:20 UTC, one run (a first execution at 08:35:08 started right after a foreign burst, one-minute load 2.50, and was discarded: `contaminated/s1-pg-r1.*`) |
| Worktree / branch | `bench-pg-floor` / `worktree-bench-pg-floor` |
| HEAD | `f2f1ee7`, `git describe --tags` = `v2.7.0-545-gf2f1ee7`; tree clean at the start (`git status` empty), nothing under `src/`, `include/`, `tools/` edited or built by this run |
| Engine the KDS numbers describe | `9a0525d` (`v2.7.0-531-g9a0525d`). `git diff --stat 9a0525d HEAD -- src include CMakeLists.txt tools` lists one file, `tools/fk_overhead_benchmark.py` (7 insertions, 5 deletions), so no engine source, build file or scenario/pg driver differs at HEAD (`archive/.../stamp.txt`) |
| PostgreSQL | **18.6** (`SELECT version()`: `PostgreSQL 18.6 on x86_64-pc-linux-gnu, compiled by gcc 13.3.0`), built from the verified source tarball into `$HOME/.local/pgsql-18.6`; `./configure --prefix=... --without-icu --without-readline --without-zlib --with-openssl`, stock `-O2` and no other flag (`pg_config --configure`, `pg-build-evidence.txt`). The host lacks those dev packages and has no passwordless sudo |
| Do the omitted libraries touch this workload? | No, verified: `ldd postgres` links only `libssl`/`libcrypto`; the cluster is `initdb --locale=C --encoding=UTF8`, every database `datlocprovider = c` (libc, collation `C`), so ICU has no role; `default_toast_compression = pglz` (built in) and `wal_compression = off`, so zlib has none; readline is `psql`'s line editor and the drivers speak the wire protocol directly (`tools/pg_wire.py`). Evidence: `pg-build-evidence.txt` |
| Cluster | `tools/pg_setup.sh`, unmodified, with `PGROOT=/home/cdkbs/pg-bench-18`, `PGPORT=15700` (not 15432, held by an unrelated server; checked free with `ss -ltn`); trust auth on loopback; **a fresh `initdb` per cell** (`pg_setup.sh destroy --yes` then `init`), the counterpart of the KDS side's fresh data file per cell. Settings are PostgreSQL's defaults plus what `pg_setup.sh` writes (port, listen_addresses, unix_socket_directories, logging_collector, log_min_duration_statement = -1, log_line_prefix); the full `pg_settings WHERE source <> 'default'` dump is `logs/<cell>.pg_settings` in the archive. In force: `synchronous_commit = on`, `fsync = on`, `wal_sync_method = fdatasync`, `full_page_writes = on`, `shared_buffers = 128MB`, `max_wal_size = 1GB`, `max_connections = 100`, `autovacuum = on`, `io_method = worker` |
| Device | data directory `/home/cdkbs/pg-bench-18/data`, `/dev/root`, `ext4` (`df -T`, archived in `stamp.txt` and `pg-build-evidence.txt`); never tmpfs. Deleted after each cell |
| Host | 8 logical CPUs (AMD EPYC 9V74, 1 socket x 4 cores x 2 threads), Linux 7.0.0-1014-azure. **The client processes and the servers share those eight CPUs**; PostgreSQL runs one process per connection over all of them |
| Host quiet | before each cell the run script waited until no `cc1plus` / `cmake --build` / `ctest` process existed and the one-minute load was at most 1.6, and recorded `/proc/loadavg` and the `pgrep` result (per-cell tables below). **A 2-second monitor ran through the whole matrix** (`logs/monitor.log`, `validity.py`): a cell with a foreign process above 10 % CPU or a build/test process inside its window is discarded and re-run. Other sessions build and test on this host: a first pass of the matrix (08:04 to 08:20 UTC, no monitor yet) had three cells overlap a foreign `ctest` and was **discarded in full**; in the monitored pass 7 cells had a foreign build/test process in their window and 5 more started right after one (one-minute load 2.5 to 4.5) and were re-run, 12 cells in all (`contaminated/`). The cells reported here have no flagged sample. The monitor's limit: it sees CPU-busy processes at 2 s spacing, so a sub-2 s burst or a process that has only just become busy is not excluded |
| Drivers | unmodified `tools/pg_scenario*.py` (and, for the `--bookers 1` KDS cells, `tools/scenario2_freight.py`); the run scripts are in the archive |

## 2. What was run

One cell, one fresh `initdb`, `python3 tools/pg_scenario1_backtest.py --port
15700 --user $(id -un) --json s1-pg-r1.json`. The data set: 30 years x 252
sessions = 7,560 sessions, 8 symbols, 60,480 bars, 60,480 feature rows,
8 models, 359 rebalance periods; `daily_stats` and `model_results` have no
index, the two filter columns get a btree index at runtime in the sweep's
third case and lose it in the fourth. The exit status is 0 and the model
P&L read back through the comparison join matches what was accumulated
(`verify: OK`, `verify_problems []`); 0 error replies in every phase.

**Not swept and one run only.** Rule 9's 200 / 1K / 10K sweep is not
executed: the task was one run at the driver's defaults, which is a single
cardinality (60,480 bars), so a fixed cost cannot be separated from a per-row
cost from the phases alone; the read shapes do return 1, 200 and 7,560 rows
(`bar-lookup`, `bar-range`, `symbol-history`), which is the only within-run
evidence. There is also **no noise floor from three runs**. The discarded first
execution of the same cell differs from this one by 1 % to 26 % row by row on
the sweeps (autocommit 745.8 against 659 rows/s, batch 1,000 15,396 against
18,757), so single figures below are good to about 25 %.

## 3. Results

| shape | detail | cold | warm | index built | index dropped |
|---|---|---|---|---|---|
| bar-lookup | pk equality, a btree descent | 14,834.0 qps (p50 64.9 µs) | 15,286.8 qps (p50 64.0 µs) | not applicable | not applicable |
| bar-range | pk BETWEEN, 200 wide - a Range step | 1,382.1 qps (p50 720.5 µs) | 1,374.7 qps (p50 727.0 µs) | not applicable | not applicable |
| day-slice | one session's features - a FilterScan | 414.4 qps (p50 2,386.7 µs) | 401.3 qps (p50 2,421.0 µs) | 8,892.4 qps (p50 104.0 µs) | 409.3 qps (p50 2,427.5 µs) |
| symbol-history | one symbol's whole 30 years - a FilterScan | 33.3 qps (p50 29,957.2 µs) | 32.3 qps (p50 30,919.1 µs) | 33.9 qps (p50 29,240.2 µs) | 32.3 qps (p50 30,856.8 µs) |
| cross-join | the backtest read - FilterScan + Probe + Probe | 375.6 qps (p50 2,611.1 µs) | 385.5 qps (p50 2,591.5 µs) | 3,982.7 qps (p50 241.3 µs) | 380.1 qps (p50 2,591.4 µs) |
| point-join | bar -> symbol -> exchange, anchored on a pk | 5,769.2 qps (p50 168.4 µs) | 5,790.4 qps (p50 167.3 µs) | not applicable | not applicable |
| model-join | one model's results joined to its model | 983.2 qps (p50 1,000.5 µs) | 1,000.5 qps (p50 996.0 µs) | 1,099.4 qps (p50 872.0 µs) | 1,122.4 qps (p50 885.2 µs) |

`day-slice` and `cross-join` are the shapes whose filter is a non-pk
equality on an unindexed relation: 414 and 376 qps cold, 8,892 and 3,983 qps
with the index built (22.2x and 10.3x), back to 409 and 380 after it is
dropped. `symbol-history` returns 7,560 rows, so an index changes nothing
(33 to 34 qps): it is a 30 ms result set, bound by rows returned.
`model-join` returns 8 rows from an 8-row model table and gains 1.10x.
Cold and warm agree within about 3 %: the data is in `shared_buffers`.

**Writes: the durability wait is the cost of a row.**

| rows per BEGIN/COMMIT | qps | errors |
|---|---|---|
| 1 | 658.7 qps | 0 errors |
| 10 | 4,883.3 qps | 0 errors |
| 100 | 14,048.4 qps | 0 errors |
| 1000 | 18,756.6 qps | 0 errors |

Autocommit is 1,517 µs per row (one commit fsync each, the same 1.3 ms
flush scenario 2 isolates); a batch of 1,000 spreads it to 53 µs per row,
28x, which is the driver's own comment: "every row is one round trip in
every case; what changes is how many durability points they cost".

**Concurrency: PostgreSQL scales with connections, because it is a process
per connection.**

| connections | qps | errors |
|---|---|---|
| 1 | 390.7 qps | 0 errors |
| 2 | 760.8 qps | 0 errors |
| 4 | 1,432.3 qps | 0 errors |
| 8 | 1,725.1 qps | 0 errors |

The backtest's 3-relation join at 1, 2, 4 and 8 connections: 391 qps, then
1.95x, 3.67x and 4.41x (the last is 216 qps per connection, the eight
backends and the client sharing eight CPUs). This is the
concurrency figure scenario 2's file cites and the one place in this
series where a PostgreSQL number exists at eight clients; on a read-only
join, not on the freight booking.

## 4. Percentiles

Every phase of the run, microseconds (the driver's own order):

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `s1-pg-r1` | load-exchanges | 2 ops | 1,258.6 µs | 1,258.6 µs | 1,258.6 µs | 1,381.3 µs | 1,381.3 µs | 1,381.3 µs | 0 errors |
| `s1-pg-r1` | load-symbols | 8 ops | 986.0 µs | 1,009.5 µs | 1,017.0 µs | 1,260.9 µs | 1,260.9 µs | 1,260.9 µs | 0 errors |
| `s1-pg-r1` | load-sessions | 7,560 ops | 54.1 µs | 56.0 µs | 56.7 µs | 72.3 µs | 88.8 µs | 212.0 µs | 0 errors |
| `s1-pg-r1` | load-bars | 60,480 ops | 57.5 µs | 63.3 µs | 64.2 µs | 96.4 µs | 117.7 µs | 1,686.9 µs | 0 errors |
| `s1-pg-r1` | load-stats | 60,480 ops | 61.2 µs | 63.3 µs | 64.4 µs | 94.1 µs | 113.1 µs | 476.6 µs | 0 errors |
| `s1-pg-r1` | load-models | 8 ops | 1,288.5 µs | 1,316.8 µs | 1,421.7 µs | 8,551.1 µs | 8,551.1 µs | 8,551.1 µs | 0 errors |
| `s1-pg-r1` | backtest-read | 360 ops | 2,708.4 µs | 2,801.8 µs | 2,864.2 µs | 3,607.7 µs | 3,895.4 µs | 5,104.1 µs | 0 errors |
| `s1-pg-r1` | result-insert | 2,872 ops | 1,062.5 µs | 1,253.9 µs | 1,320.5 µs | 2,262.0 µs | 5,249.4 µs | 18,971.5 µs | 0 errors |
| `s1-pg-r1` | backtest-replay | 360 ops | 2,513.6 µs | 2,546.9 µs | 2,586.3 µs | 2,808.5 µs | 3,579.4 µs | 3,747.8 µs | 0 errors |
| `s1-pg-r1` | compare-all | 1 ops | 7,396.9 µs | 7,396.9 µs | 7,396.9 µs | 7,396.9 µs | 7,396.9 µs | 7,396.9 µs | 0 errors |
| `s1-pg-r1` | compare-one | 32 ops | 870.0 µs | 884.7 µs | 892.2 µs | 930.1 µs | 1,006.4 µs | 1,006.4 µs | 0 errors |
| `s1-pg-r1` | read-bar-lookup | 200 ops | 63.2 µs | 64.7 µs | 65.4 µs | 81.2 µs | 89.1 µs | 143.5 µs | 0 errors |
| `s1-pg-r1` | read-bar-range | 200 ops | 699.3 µs | 710.9 µs | 718.1 µs | 736.7 µs | 768.2 µs | 930.5 µs | 0 errors |
| `s1-pg-r1` | read-symbol-history | 10 ops | 28,835.6 µs | 30,054.5 µs | 30,497.3 µs | 34,731.0 µs | 34,731.0 µs | 34,731.0 µs | 0 errors |
| `s1-pg-r1` | read-day-slice | 200 ops | 2,359.2 µs | 2,373.1 µs | 2,381.4 µs | 2,504.7 µs | 3,571.7 µs | 3,626.3 µs | 0 errors |
| `s1-pg-r1` | read-join-point | 200 ops | 133.2 µs | 136.3 µs | 138.5 µs | 167.4 µs | 177.2 µs | 365.4 µs | 0 errors |
| `s1-pg-r1` | read-join-exists | 10 ops | 7,955.1 µs | 8,075.1 µs | 8,190.9 µs | 8,866.5 µs | 8,866.5 µs | 8,866.5 µs | 0 errors |
| `s1-pg-r1` | agg-global | 10 ops | 4,093.2 µs | 4,127.8 µs | 4,132.4 µs | 4,416.1 µs | 4,416.1 µs | 4,416.1 µs | 0 errors |
| `s1-pg-r1` | agg-by-symbol | 10 ops | 7,197.4 µs | 7,226.9 µs | 7,234.8 µs | 7,288.5 µs | 7,288.5 µs | 7,288.5 µs | 0 errors |
| `s1-pg-r1` | agg-by-session | 10 ops | 19,655.7 µs | 19,747.7 µs | 19,883.1 µs | 23,036.2 µs | 23,036.2 µs | 23,036.2 µs | 0 errors |
| `s1-pg-r1` | agg-day-slice | 200 ops | 2,364.7 µs | 2,378.1 µs | 2,388.5 µs | 2,507.1 µs | 3,360.1 µs | 3,684.0 µs | 0 errors |
| `s1-pg-r1` | agg-distinct | 10 ops | 3,975.3 µs | 3,983.0 µs | 3,995.5 µs | 4,713.2 µs | 4,713.2 µs | 4,713.2 µs | 0 errors |

## 5. Where the time goes

A backtest is reads, so its decomposition is fixed cost and rows: the pk
lookup (`read-bar-lookup`) is 65.4 µs p50, the pure round trip, and the
200-row range (`read-bar-range`) 718 µs, so about 3.3 µs per returned row
of a range; the unindexed `day-slice` seqscan (`read-day-slice`, 2,381 µs p50 over
60,480 feature rows) is 39 ns per row scanned (2,381 µs / 60,480), comparable to
the 34 ns per row of scenario 3's `loans` scans, while `read-symbol-history`
(30.5 ms) is bound by the 7,560 rows it returns and formats client-side, about 4 µs a row.
The write phases wait on durability
(`result-insert` p50 1,320.5 µs, one fsync per insert; `load-*` amortise it
over 200-row transactions to 64 µs per row). No lock or conflict wait exists
on one connection, and the concurrency sweep above is the only multi-client
measurement.

## 6. Host stamp

| cell | precheck UTC | loadavg 1/5/15 | cc1plus / cmake --build / ctest |
|---|---|---|---|
| `s1-pg-r1` | 2026-09-30T08:40:39Z | 0.67 / 1.85 / 2.24 | none |

## 7. What the run says about the engine

1. **It gives the next KDS scenario 1 run a floor.** When the driver's
   `HEAP` relations are BTREE, its `day-slice` and `cross-join` are the shapes
   to compare: 414 qps and 376 qps unindexed, 8,892 and 3,983 with a
   btree index on PostgreSQL. A KDS Cabin or index on `daily_stats.session_no`
   is the counterpart.
2. **The write ceiling is the flush.** 659 rows/s autocommit against 18,757
   in 1,000-row batches on the same device; the scenario 0 and 2 files put
   KDS at the same flush.
3. **Nothing here stresses a limit of the engine beyond the load**: one run
   at default sizes, one client except the connection sweep.

Archive: `bench/v3.0.0/archive/scenario1-pg18-v2.7.0-545-gf2f1ee7/`.
