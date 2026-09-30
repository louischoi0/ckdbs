# Scenario 0 (stock market), PostgreSQL 18.6 floor, at `v2.7.0-545-gf2f1ee7`

**One line.** With eight concurrent traders and four autocommit statements per
business transaction, PostgreSQL 18.6 at its defaults (`synchronous_commit =
on`, every commit waits for its WAL flush) runs **710.8 tps** (median of
three, spread 19.0 %). KDS under `durability = group` is **0.88x to 0.93x**
of that (626.0 and 661.0 tps). That is inside the three-run spread, but the
spread is one run: four of the five `on` executions (the two other runs, the
discarded first execution and the instrumented cell) lie within 698.6 to
711.2 tps, 1.8 %, and only the re-run at 831.1 tps is outside, so a 5 to 12 %
`group` deficit is the likelier reading, not a tie. KDS under `strict` is
**0.24x to 0.47x** (169.4 and 331.7 tps), outside every floor. The gap is the
commit path: PostgreSQL merged 3.9 commits into each WAL fsync in an
instrumented cell, and KDS `strict`'s rate at one core is what one fsync per
commit would cost on this device (inferred; KDS's sync count was not read). PostgreSQL is the floor here, not the reference: this file says
the engine is in the league under `group`, and that `strict` is not.

Driver: `tools/pg_scenario0_stockmarket.py`, unmodified, `--txn` off (four
autocommits, matching the KDS driver statement for statement), same
arguments as the KDS cells. KDS side: `bench/v3.0.0/results-scenario0-stockmarket-v2.7.0-531-g9a0525d.md`
(engine `9a0525d`, medians of three runs per cell, cited below as "the
9a0525d file"). This PostgreSQL run is the first of its shape, so it is
the baseline the next one is read against.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-09-30; cells 08:23:06 to 08:23:33 UTC, plus the re-run of `s0-pg-on-r1` at 08:36:17 to 08:36:26 (its first execution overlapped a foreign `ctest` and is in `contaminated/`); instrumented probe 08:42:24 |
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

Six cells, three runs each of two cluster settings, each a fresh `initdb`.
Arguments, identical to the KDS cells (`bench/v3.0.0/archive/scenario0-v2.7.0-531-g9a0525d/run_s02.sh`):
`--users 100 --accounts-per-user 3 --assets 30 --traders 8 --txn-per-user 50
--verify 200 --seed 1 --sync`, `--profit` on. The work target is 5,000
committed business transactions (625 per trader), each 2 `INSERT trades` + 2
`UPDATE accounts` on 100 users, 287 accounts and 30 assets, ending with
`trades` at 10,000 rows. Command:

    python3 tools/pg_scenario0_stockmarket.py --port 15700 --user $(id -un) --users 100 \
        --accounts-per-user 3 --assets 30 --traders 8 --txn-per-user 50 --verify 200 \
        --seed 1 --sync [--synchronous-commit off] --json <cell>.json

| cell | `synchronous_commit` | durable at the reply | in the like-for-like table |
|---|---|---|---|
| `s0-pg-on-r1..r3` | `on` (server default) | yes: the commit record is flushed before the reply | yes |
| `s0-pg-off-r1..r3` | `off` | **no**: the reply precedes the flush | **no**, reported apart, labelled non-durable |

**Which PostgreSQL setting is the comparator for which KDS mode.**
`docs/spec/wal.md` §1: `D1 strict` acknowledges after the commit record is
flushed and device-synced, and `D2 group` has "the same durability point;
flush batched (group commit)... zero loss window"; the classes "differ only
in batching, never in the durability point". Both are durable at the reply,
so PostgreSQL's default is the comparator for **both** and
`synchronous_commit = off` is the comparator for neither (KDS's non-durable
class is `D3 relaxed`, not measured here). The `off` cells are a control
that prices the durability wait inside PostgreSQL and nothing else.

**Client concurrency.** KDS's `cores` is a server-side knob with no
PostgreSQL equivalent (one backend process per connection over all eight
CPUs). The PG cell corresponds to the KDS cells by client concurrency:
all of them run eight trader processes and one reporter, so the PG figure is
compared against both `c1` and `c8`.

Rule 9's 200 / 1K / 10K sweep is not executed, for the reason the 9a0525d
file gives: the size is fixed by the arguments that mirror the KDS run and
the measured unit does not scale with `trades`' row count.

## 3. Throughput

**KDS `group` is inside PostgreSQL's three-run spread but probably 5 to 12 %
behind it; KDS `strict` is far behind.**

| cell | synchronous_commit | run 1 | run 2 | run 3 | median | spread (max/min-1) | durable at the reply |
|---|---|---|---|---|---|---|---|
| `s0-pg-on` | on (default) | 831.1 tps | 710.8 tps | 698.6 tps | **710.8 tps** | 19.0 % | yes |
| `s0-pg-off` | off | 15,848.9 tps | 16,208.1 tps | 13,931.7 tps | **15,848.9 tps** | 16.3 % | **no (non-durable, not like-for-like)** |

The `on` cells span 698.6 to 831.1 tps (19.0 %); this file reads no delta
below that as a result. The `off` control runs 15,848.9 tps median, 22.3x the
durable cell, with a 16.3 % spread.

Side by side (KDS medians from the 9a0525d file, section 3; ratio is KDS
over PostgreSQL `on`, above 1.00x KDS is faster):

| engine / cell | durability | tps median | spread | KDS / PG-on |
|---|---|---|---|---|
| PostgreSQL 18.6, 8 traders | `synchronous_commit = on` | **710.8 tps** | 19.0 % | 1.00x (reference for this table) |
| KDS `s0-c1-g` (cores 1, group) | group | 626.0 tps | 3.8 % | 0.88x |
| KDS `s0-c8-g` (cores 8, group) | group | 661.0 tps | 6.8 % | 0.93x |
| KDS `s0-c1-s` (cores 1, strict) | strict | 169.4 tps | 6.1 % | 0.24x |
| KDS `s0-c8-s` (cores 8, strict) | strict | 331.7 tps | 20.0 % | 0.47x |

Reading it: `group` is 0.88x and 0.93x, both inside the 19.0 % PostgreSQL
spread and above the KDS cells' own 3.8 % and 6.8 %. The 19.0 % is carried
by the one re-run at 831.1 tps: runs 2 and 3, the discarded first execution
of run 1 (711.2 tps) and the instrumented cell (698.8 tps) span 1.8 %, and
against that cluster `group` is 5 to 12 % slower. The reading is "in the
same league, probably somewhat slower", not "the same speed". `strict` is
4.2x and 2.1x slower than PostgreSQL at one and eight cores; the lowest
`on` run (698.6 tps) is still 2.1x `c8-s`'s median, so no floor explains it.

## 4. Where the time goes: the durability wait, and why `strict` loses

The measured unit is four autocommit statements, and the statements account
for all of it (section 5 below, last table: `trade-insert` and `account-update` are
49.8 to 50.0 % each, 0.1 % unattributed), so the decomposition is by what a
statement waits for:

| Wait | Estimate | How derived |
|---|---|---|
| Durability, commit fsync | **95.0 % of PostgreSQL's mean transaction** (run 1: 9,614 µs mean with the fsync, 478 µs with `synchronous_commit = off`) | `on` against `off`, same workload; the `off` cells' reporter never woke (a 0.3 s run is shorter than its 1 s interval, `profit-scan` 0 ops), so `off` is also 500 to 600 statements lighter (250 to 300 `profit-scan` plus as many `profit-insert` in the `on` cells) |
| Batching of that wait | **3.9 commits per WAL fsync** in the `on` cell: 21,258 commits, 5,437 client-backend and 5,441 total WAL fsyncs over the whole cell (load, run and `CHECKPOINT` included) | instrumented extra cell `s0-pg-on-fsyncprobe` (`pg_stat_database.xact_commit`, `pg_stat_io` `object = 'wal'`), 698.8 tps in that cell; `xact_commit` also counts the reporter's and the verify pass's read-only transactions (several hundred), which do not fsync, so the ratio is a slight over-count of write commits per fsync |
| Client and socket | not isolated; inside every number. p0 of a PostgreSQL `trade-insert` with `off` is 58.8 to 61.7 µs, so the fixed part of a statement is at most about 60 µs | `off` cells' p0 |
| Lock or conflict wait | none visible: 0 error replies in all six cells, `torn` 0 | error columns |
| Page work | not separable from the commit wait in the `on` cells; with `off`, `trade-insert` p50 is 105.8 µs and `account-update` 118.7 µs (median of the three runs' p50), so a statement is about 100 µs | `off` p50 |

**At one core, KDS `strict`'s rate is what one fsync per commit costs
(inferred); PostgreSQL batches.** At one core
`strict` sustains 169.4 tps x 4 = 678 durable statements per second, one every
1.5 ms; the device's own fsync is about that long (in the single-client cells of scenario 2 a
durable commit runs at 648/s for PostgreSQL and 650 to 672/s for KDS, derived
from the commit phase's mean). PostgreSQL's eight concurrent committers
share a flush: 3.9 commits per fsync turns the same device into 2,843
durable statements per second (710.8 x 4). KDS `group` reaches the same
place, 2,504 statements per second at `c1` and 2,644 at `c8`, by batching its
flushes. That the KDS `group` figure is in PostgreSQL's range is the
finding; that `strict` is a quarter to a half of it says what `strict`
costs on this shape. The KDS-side sync count was not read: `SHOW META`
exposes `wal_syncs`, `wal_group_commits` and `wal_mean_group_batch` at
`9a0525d`, and neither run sampled them, so "one fsync per commit" is the
inference from the arithmetic above and the wal.md description, not a count.
It holds at one core only: at `c8-s`, 331.7 tps x 4 = 1,327 durable
statements per second, one every 0.75 ms, is faster than one 1.3 to 1.5 ms
flush at a time, so at eight cores some syncs are shared or overlapped; the
mechanism was not isolated (peers' syncs go through core 0's writer,
`wal.md` §3).

The reporter's read differs by engine: `profit-scan` (a non-pk equality over
287 accounts, no index, concurrent with the traders) has p50 139.1 to 143.9
µs on PostgreSQL against 1,573.2 µs at KDS `c1-g` and 19,918.7 µs at `c1-s`,
where the scan queues behind eight traders on the one reactor, and 219.4
and 158.5 µs at `c8-g` / `c8-s`, where it does not (section 5 below).

## 5. Percentiles

PostgreSQL `synchronous_commit = on`, all three runs, microseconds. `ops` is
the phase's count in that cell.

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `s0-pg-on-r1` | txn | 5,000 ops | 7,399.7 µs | 8,012.0 µs | 8,320.8 µs | 16,538.0 µs | 22,678.3 µs | 30,041.7 µs | 0 errors |
| `s0-pg-on-r1` | trade-insert | 10,000 ops | 1,140.9 µs | 1,943.9 µs | 2,037.5 µs | 4,485.1 µs | 8,743.6 µs | 16,207.2 µs | 0 errors |
| `s0-pg-on-r1` | account-update | 10,000 ops | 1,136.1 µs | 1,951.2 µs | 2,043.6 µs | 4,268.7 µs | 8,507.6 µs | 16,212.3 µs | 0 errors |
| `s0-pg-on-r1` | profit-scan | 250 ops | 109.8 µs | 128.3 µs | 139.1 µs | 185.9 µs | 205.9 µs | 774.2 µs | 0 errors |
| `s0-pg-on-r1` | profit-insert | 250 ops | 1,292.3 µs | 1,797.2 µs | 1,886.1 µs | 3,470.8 µs | 5,763.5 µs | 7,517.8 µs | 0 errors |
| `s0-pg-on-r2` | txn | 5,000 ops | 6,511.0 µs | 9,993.5 µs | 10,367.2 µs | 16,748.9 µs | 22,014.2 µs | 36,095.4 µs | 0 errors |
| `s0-pg-on-r2` | trade-insert | 10,000 ops | 1,194.1 µs | 2,423.5 µs | 2,543.1 µs | 4,176.1 µs | 8,245.5 µs | 26,413.5 µs | 0 errors |
| `s0-pg-on-r2` | account-update | 10,000 ops | 1,340.7 µs | 2,431.4 µs | 2,543.3 µs | 4,659.4 µs | 8,443.0 µs | 26,214.0 µs | 0 errors |
| `s0-pg-on-r2` | profit-scan | 300 ops | 110.4 µs | 136.1 µs | 143.9 µs | 188.3 µs | 229.8 µs | 747.2 µs | 0 errors |
| `s0-pg-on-r2` | profit-insert | 300 ops | 1,439.4 µs | 2,297.3 µs | 2,402.0 µs | 4,522.7 µs | 8,383.8 µs | 11,035.4 µs | 0 errors |
| `s0-pg-on-r3` | txn | 5,000 ops | 8,775.2 µs | 9,979.6 µs | 10,406.8 µs | 18,325.7 µs | 23,015.8 µs | 28,721.7 µs | 0 errors |
| `s0-pg-on-r3` | trade-insert | 10,000 ops | 1,758.8 µs | 2,416.1 µs | 2,538.3 µs | 4,721.9 µs | 9,308.7 µs | 16,378.5 µs | 0 errors |
| `s0-pg-on-r3` | account-update | 10,000 ops | 1,381.4 µs | 2,427.3 µs | 2,544.5 µs | 4,966.1 µs | 9,152.8 µs | 16,346.9 µs | 0 errors |
| `s0-pg-on-r3` | profit-scan | 300 ops | 109.0 µs | 133.2 µs | 140.7 µs | 183.3 µs | 217.8 µs | 775.6 µs | 0 errors |
| `s0-pg-on-r3` | profit-insert | 300 ops | 1,861.6 µs | 2,350.2 µs | 2,454.8 µs | 4,314.6 µs | 7,965.0 µs | 14,583.6 µs | 0 errors |

PostgreSQL `synchronous_commit = off` (**non-durable**, not like-for-like).
`profit-scan` and `profit-insert` are 0 ops because the run ends before the
reporter's first wake:

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `s0-pg-off-r1` | txn | 5,000 ops | 292.0 µs | 436.3 µs | 465.6 µs | 578.6 µs | 752.6 µs | 2,367.0 µs | 0 errors |
| `s0-pg-off-r1` | trade-insert | 10,000 ops | 61.5 µs | 96.3 µs | 105.8 µs | 143.2 µs | 208.0 µs | 1,293.9 µs | 0 errors |
| `s0-pg-off-r1` | account-update | 10,000 ops | 75.9 µs | 108.7 µs | 118.7 µs | 156.5 µs | 221.6 µs | 1,099.7 µs | 0 errors |
| `s0-pg-off-r1` | profit-scan | 0 ops | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0 errors |
| `s0-pg-off-r1` | profit-insert | 0 ops | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0 errors |
| `s0-pg-off-r2` | txn | 5,000 ops | 300.9 µs | 426.7 µs | 447.4 µs | 577.5 µs | 764.9 µs | 3,467.9 µs | 0 errors |
| `s0-pg-off-r2` | trade-insert | 10,000 ops | 61.7 µs | 94.9 µs | 101.6 µs | 141.6 µs | 215.5 µs | 812.6 µs | 0 errors |
| `s0-pg-off-r2` | account-update | 10,000 ops | 70.3 µs | 105.3 µs | 112.7 µs | 150.0 µs | 217.5 µs | 3,111.6 µs | 0 errors |
| `s0-pg-off-r2` | profit-scan | 0 ops | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0 errors |
| `s0-pg-off-r2` | profit-insert | 0 ops | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0 errors |
| `s0-pg-off-r3` | txn | 5,000 ops | 285.1 µs | 449.2 µs | 495.5 µs | 754.1 µs | 893.4 µs | 3,004.8 µs | 0 errors |
| `s0-pg-off-r3` | trade-insert | 10,000 ops | 58.8 µs | 97.9 µs | 111.5 µs | 201.2 µs | 267.2 µs | 776.3 µs | 0 errors |
| `s0-pg-off-r3` | account-update | 10,000 ops | 74.9 µs | 112.0 µs | 124.9 µs | 218.5 µs | 286.7 µs | 2,490.5 µs | 0 errors |
| `s0-pg-off-r3` | profit-scan | 0 ops | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0 errors |
| `s0-pg-off-r3` | profit-insert | 0 ops | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0.0 µs | 0 errors |

KDS, run 1 of each cell, copied from the 9a0525d archive (`json/s0-*.json`)
for reading beside the two tables above:

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `s0-c1-g` | txn | 5,000 ops | 9,791.1 µs | 11,243.1 µs | 11,769.4 µs | 20,667.2 µs | 28,226.6 µs | 36,900.0 µs | 0 errors |
| `s0-c1-g` | trade-insert | 10,000 ops | 1,742.2 µs | 2,746.9 µs | 2,884.6 µs | 5,400.6 µs | 11,321.2 µs | 21,429.4 µs | 0 errors |
| `s0-c1-g` | account-update | 10,000 ops | 1,461.5 µs | 2,752.2 µs | 2,890.6 µs | 5,471.3 µs | 10,456.2 µs | 21,426.5 µs | 0 errors |
| `s0-c1-g` | profit-scan | 300 ops | 387.3 µs | 1,488.2 µs | 1,573.2 µs | 2,538.6 µs | 5,310.1 µs | 11,603.1 µs | 0 errors |
| `s0-c1-g` | profit-insert | 300 ops | 2,441.7 µs | 2,689.2 µs | 2,837.2 µs | 4,567.1 µs | 5,998.8 µs | 16,263.4 µs | 0 errors |
| `s0-c8-g` | txn | 5,000 ops | 5,005.7 µs | 9,732.7 µs | 10,338.3 µs | 18,829.3 µs | 27,599.2 µs | 49,323.8 µs | 0 errors |
| `s0-c8-g` | trade-insert | 10,000 ops | 1,186.6 µs | 2,351.5 µs | 2,505.8 µs | 5,114.6 µs | 9,242.7 µs | 41,696.1 µs | 0 errors |
| `s0-c8-g` | account-update | 10,000 ops | 1,245.9 µs | 2,355.0 µs | 2,511.2 µs | 5,448.2 µs | 10,246.0 µs | 25,453.6 µs | 0 errors |
| `s0-c8-g` | profit-scan | 300 ops | 153.5 µs | 197.8 µs | 219.4 µs | 362.7 µs | 512.2 µs | 730.3 µs | 0 errors |
| `s0-c8-g` | profit-insert | 300 ops | 1,782.0 µs | 2,125.2 µs | 2,255.8 µs | 3,826.9 µs | 7,063.6 µs | 9,731.8 µs | 0 errors |
| `s0-c1-s` | txn | 5,000 ops | 4,604.6 µs | 37,576.2 µs | 41,228.3 µs | 80,927.9 µs | 95,307.9 µs | 134,410.8 µs | 0 errors |
| `s0-c1-s` | trade-insert | 10,000 ops | 1,125.1 µs | 8,918.1 µs | 9,907.6 µs | 20,795.0 µs | 28,946.0 µs | 56,294.2 µs | 0 errors |
| `s0-c1-s` | account-update | 10,000 ops | 1,156.3 µs | 8,946.8 µs | 9,945.6 µs | 20,682.3 µs | 28,915.2 µs | 49,347.9 µs | 0 errors |
| `s0-c1-s` | profit-scan | 400 ops | 10,294.0 µs | 18,889.9 µs | 19,918.7 µs | 31,519.6 µs | 39,063.2 µs | 40,380.8 µs | 0 errors |
| `s0-c1-s` | profit-insert | 400 ops | 17,655.5 µs | 19,993.4 µs | 21,199.8 µs | 33,642.1 µs | 46,982.8 µs | 54,559.5 µs | 0 errors |
| `s0-c8-s` | txn | 5,001 ops | 8,425.2 µs | 16,952.5 µs | 20,250.3 µs | 45,447.2 µs | 56,883.8 µs | 81,571.0 µs | 1 errors |
| `s0-c8-s` | trade-insert | 10,002 ops | 1,831.4 µs | 3,869.4 µs | 4,938.5 µs | 11,600.2 µs | 18,126.6 µs | 42,392.6 µs | 1 errors |
| `s0-c8-s` | account-update | 10,002 ops | 1,343.3 µs | 3,849.9 µs | 4,937.0 µs | 11,538.1 µs | 18,294.3 µs | 41,832.5 µs | 0 errors |
| `s0-c8-s` | profit-scan | 100 ops | 134.2 µs | 147.9 µs | 158.5 µs | 190.9 µs | 237.7 µs | 387.1 µs | 0 errors |
| `s0-c8-s` | profit-insert | 100 ops | 1,850.5 µs | 2,211.0 µs | 2,323.4 µs | 4,409.0 µs | 6,079.8 µs | 25,000.1 µs | 0 errors |

Wait shares (each phase's mean times its ops over the transaction's mean
times its ops, run 1):

| cell | trade-insert | account-update | unattributed (client, socket, between statements) | txn mean |
|---|---|---|---|---|
| `s0-pg-on-r1` | 50.0 % | 49.8 % | 0.1 % | 9,614 µs |
| `s0-pg-off-r1` (non-durable) | 46.4 % | 51.2 % | 2.4 % | 478 µs |
| KDS `s0-c1-g` | 50.0 % | 50.0 % | 0.0 % | 13,017 µs |
| KDS `s0-c8-g` | 49.5 % | 50.5 % | 0.1 % | 11,556 µs |
| KDS `s0-c1-s` | 50.0 % | 50.0 % | 0.0 % | 44,064 µs |
| KDS `s0-c8-s` | 50.1 % | 49.9 % | 0.0 % | 22,520 µs |

## 6. Correctness

`--verify 200` read back 200 balances in every cell and **all matched**;
every phase of every cell replied with zero errors, `torn` is 0 and `exit` is
0, including the `on` cells. `underfunded 179` is the same count in all six
cells (transactions the client skips before sending a statement, a function
of the seed), so all six did the same work. There is nothing here
corresponding to the KDS `c8-s` refused insert (9a0525d file, section 6): the
PostgreSQL cells have no `TXN_CONFLICT` analogue.

| cell | committed | torn | underfunded (skipped, no statement sent) | balance verify | error replies (all phases) | driver exit |
|---|---|---|---|---|---|---|
| `s0-pg-on-r1` | 5,000 txns | 0 txns | 179 txns | 200 accounts read back - all match | 0 replies | exit 0 |
| `s0-pg-on-r2` | 5,000 txns | 0 txns | 179 txns | 200 accounts read back - all match | 0 replies | exit 0 |
| `s0-pg-on-r3` | 5,000 txns | 0 txns | 179 txns | 200 accounts read back - all match | 0 replies | exit 0 |
| `s0-pg-off-r1` | 5,000 txns | 0 txns | 179 txns | 200 accounts read back - all match | 0 replies | exit 0 |
| `s0-pg-off-r2` | 5,000 txns | 0 txns | 179 txns | 200 accounts read back - all match | 0 replies | exit 0 |
| `s0-pg-off-r3` | 5,000 txns | 0 txns | 179 txns | 200 accounts read back - all match | 0 replies | exit 0 |

## 7. Noise floor and host stamps

The floor is the three-run spread: 19.0 % for `on` (698.6, 710.8, 831.1
tps) and 16.3 % for `off`. PostgreSQL is noisier than KDS `group` here (3.8 %
and 6.8 % in the 9a0525d file). Run 1 of `on` at 831.1 tps is the re-run;
the first execution (711.2 tps) was discarded because its window, which
`validity.py` opens before the quiet-wait, held 100 s of waiting on a foreign
`ctest` that ended at 08:22:55; the measurement itself ran 08:22:56 to
08:23:06 with no foreign process in `logs/monitor.log`
(`contaminated/s0-pg-on-r1.*`). Without the re-run the spread is 1.8 %, which
is why section 3 reads the `group` gap as a probable 5 to 12 % deficit. The device thermometer
of the KDS files (`load-users` p50) is not applicable to PostgreSQL's load
phases, which run 100 to 287 rows. Every cell: monitored window with no
foreign process (`validity.py`), loadavg and `pgrep` at the start:

| cell | precheck UTC | loadavg 1/5/15 | cc1plus / cmake --build / ctest |
|---|---|---|---|
| `s0-pg-on-r1` | 2026-09-30T08:36:17Z | 1.15 / 3.08 / 2.64 | none |
| `s0-pg-off-r1` | 2026-09-30T08:23:06Z | 1.32 / 1.56 / 1.83 | none |
| `s0-pg-on-r2` | 2026-09-30T08:23:08Z | 1.29 / 1.55 / 1.82 | none |
| `s0-pg-off-r2` | 2026-09-30T08:23:18Z | 1.25 / 1.54 / 1.81 | none |
| `s0-pg-on-r3` | 2026-09-30T08:23:21Z | 1.25 / 1.54 / 1.81 | none |
| `s0-pg-off-r3` | 2026-09-30T08:23:31Z | 1.28 / 1.53 / 1.81 | none |

## 8. What the run says about the engine

1. **KDS `group` is in PostgreSQL's league on a durable, fsync-bound write
   workload.** 626.0 and 661.0 tps against 710.8 with a 19.0 % floor that
   one run carries; four of five PostgreSQL executions sit within 1.8 %, so
   KDS is probably 5 to 12 % slower here. It is in the right league, which
   is all a floor is for; it does not say KDS is as fast.
2. **`strict` is the price of not batching.** 0.24x and 0.47x. The
   PostgreSQL instrumented cell shows the mechanism it avoids: 3.9 commits
   per fsync. `docs/spec/wal.md` §1 states that `D1` and `D2` differ only in
   batching; the 9a0525d medians put a size on that difference for
   four-statement autocommit traffic from eight clients: `group` is 3.7x
   `strict` at one core and 2.0x at eight (this run adds that PostgreSQL is
   4.2x and 2.1x `strict`).
   An application that needs the per-commit flush semantics pays it; one
   that is content with group commit's zero loss window need not.
3. **Eight cores close half of `strict`'s gap** (0.24x to 0.47x): the
   durability waits overlap across cores, and PostgreSQL's process-per-
   connection model gets that overlap for free. Not separable here from the
   eight client processes sharing the same eight CPUs.
4. **Nothing in this scenario reached a limit that is the engine's, other
   than the flush.** Every statement is a 60 to 110 µs round trip once the
   commit wait is removed (PostgreSQL `off`), and at 16,000 tps the Python driver
   and the eight shared CPUs are presumably the bound, not the engine (not isolated); the KDS
   non-durable class was not measured, so no KDS-vs-PostgreSQL statement is
   made about the no-fsync path.

Archive: `bench/v3.0.0/archive/scenario0-pg18-v2.7.0-545-gf2f1ee7/`: per-cell
JSON, driver stdout, `pg_settings` dumps, prechecks, monitor log, contaminated
cells, `df -T`, run scripts.
