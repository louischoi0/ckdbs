# Scenario 2 (freight), PostgreSQL 18.6 floor, at `v2.7.0-545-gf2f1ee7`

**One line.** On one booking connection, durable, the eight-statement
freight booking runs **392.2 tps** on PostgreSQL 18.6 (median of three,
spread 2.4 %) and **378.2 tps (group) / 377.7 tps (strict)** on KDS at
`cores = 1` with `--bookers 1`, measured in this run: KDS is **0.96x** of
PostgreSQL in both modes, and the commit fsync is 56 to 62 % of every one
of those bookings on both engines. The 9a0525d KDS figures (1,519.0 tps at
`c8-g`, 3.87x PostgreSQL) are **not** a like-for-like comparison: they drive
eight contended bookers and the PostgreSQL twin has one connection. What
the matched cells show is that KDS's write statements are as fast as
PostgreSQL's (0.95x to 1.19x), its non-pk reads that PostgreSQL serves from
an index it was given are 2.1x to 3.6x slower, and the durable commit is the
same fsync on both. PostgreSQL is the floor; KDS is 4 % under it, at the edge
of the spreads.

Driver: `tools/pg_scenario2_freight.py`, run through
`pg_s2_shim.py` (below), same workload arguments as the KDS cells. KDS
side: `bench/v3.0.0/results-scenario2-freight-v2.7.0-531-g9a0525d.md` (the
9a0525d file, 8 bookers) and, new in this run, `tools/scenario2_freight.py`
with `--bookers 1` on the same binary. This is the first PostgreSQL run of
this shape and becomes its baseline.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-09-30, 08:23:33 to 08:27:05 UTC (12 cells, PG and KDS cells interleaved); instrumented probe 08:42:34 |
| KDS binary for the `--bookers 1` cells | copy of `/home/cdkbs/bench-runs/rebaseline-9a0525d/kds_server`, itself a copy of `build-release/kds_server` (Release, source binary mtime 2026-09-30 06:30:43 UTC, after `9a0525d` at 06:19:05); **sha256 `1d07d33386541ec3a13090d37918e229957e29c2c777ff15f2a63db6957f8746`**, the same hash as the 9a0525d files; copied to `/home/cdkbs/bench-runs/pg18-f2f1ee7/kds_server` (mtime 08:00:23) and every KDS server of this run started from that copy. The binary is not older than the engine: `src/`, `include/`, `CMakeLists.txt` are byte-identical at HEAD. KDS config `cores = 1`, `durability` group or strict, `log_level = warn`, port 15710 / 15711 (never 15432), data under `/home/cdkbs/bench-runs/pg18-f2f1ee7/data/` on `/dev/root` ext4, fresh file per cell |
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
| Driver shim | `pg_s2_shim.py` (archive). **`pg_scenario2_freight.py --verify N` crashes after the measurement**: `verify()` returns a `(checks, failures, first)` tuple and the shared printer `print_bookings` (`tools/scenario2_freight.py`) reads `v.checks`, so `AttributeError: 'tuple' object has no attribute 'checks'` fires before `--json` is written (log: `s2-pg-driver-verify-crash.log`). The shim replaces only that print step, converting the tuple to the attribute object the printer reads; nothing measured, sent to the server or verified differs |

## 2. What was run and how the comparison is made valid

Twelve cells, three runs each: PostgreSQL `synchronous_commit = on`,
PostgreSQL `off` (**non-durable**, control), KDS `cores = 1` `group` and KDS
`cores = 1` `strict`, both with `--bookers 1`; run order interleaved PG
then KDS then PG then KDS, so a drifting host hits both. Arguments
(9a0525d's `run_s02.sh` minus `--bookers 8`): `--organizations 300 --ships
30 --operations 300 --cargos 4000 --bookings 3000 --verify 100 --seed 1`;
KDS additionally `--sync` and the driver's own three steps (`--schema-only`,
`--load-only`, run). A run is 3,000 committed bookings.

**Why `--bookers 1` had to be run.** The PostgreSQL twin has no `--bookers`
flag: it is one connection, and its manifest reporter is interleaved between
bookings instead of in a second process. The 9a0525d KDS cells drive eight
bookers on eight contended voyages, so KDS against PostgreSQL in the
9a0525d numbers would compare eight clients with one. The KDS driver's
default is `--bookers 1`; running it that way on the same binary gives the
matched pair. This is the one place this task re-ran KDS. It is not a
repeat of the 9a0525d cells: it is a different client concurrency.

**What is not identical.** Both do 3,000 committed bookings, but the RNG
streams differ between the two drivers' attempt loops: KDS attempted 3,402
(328 rejected for capacity, 74 for credit), PostgreSQL 3,340 (289, 51).
tps is committed bookings over elapsed time, so the rejected attempts
(10 to 12 % of the attempts: the four reads and a rollback, the `booking`
p0 of 336 to 475 µs) are in both denominators; KDS's 62 extra attempts at
under 0.5 ms each are under 31 ms of a 7.9 s run, about 0.4 %. The relation
set matches; on PostgreSQL `freights`/`charges` carry a pk index and
`freights(operation_id)`, `recipes(cargo_type)` carry the index a DBA would
add (the driver's stated choice), where KDS's `recipe-read` is a FilterScan
and its `manifest-scan` a scan.

Rule 9's 200 / 1K / 10K sweep is not executed, for the reason of the 9a0525d
file: the sizes mirror the KDS matrix, and a booking on a pk-addressed
voyage and customer does not scale with the ledgers' row count.

## 3. Throughput

**On one client, durable, the two engines run the same booking at the same
speed.**

| cell | bookers (client concurrency) | durable at the reply | run 1 | run 2 | run 3 | median | spread (max/min-1) |
|---|---|---|---|---|---|---|---|
| PostgreSQL 18.6 `synchronous_commit = on` | 1 connection | yes | 391.6 tps | 392.2 tps | 400.9 tps | **392.2 tps** | 2.4 % |
| PostgreSQL 18.6 `synchronous_commit = off` | 1 connection | **no (non-durable)** | 977.5 tps | 1,019.6 tps | 895.4 tps | **977.5 tps** | 13.9 % |
| KDS `cores = 1`, group, `--bookers 1` (this run) | 1 booker | yes | 378.2 tps | 378.2 tps | 367.1 tps | **378.2 tps** | 3.0 % |
| KDS `cores = 1`, strict, `--bookers 1` (this run) | 1 booker | yes | 377.7 tps | 369.2 tps | 382.1 tps | **377.7 tps** | 3.5 % |
| KDS `s2-c1-g` (cores 1, group), 9a0525d archive | 8 bookers, contended | yes | 542.7 tps | 537.6 tps | 537.0 tps | **537.6 tps** | 1.1 % |
| KDS `s2-c8-g` (cores 8, group), 9a0525d archive | 8 bookers, contended | yes | 1,477.0 tps | 1,519.0 tps | 1,542.8 tps | **1,519.0 tps** | 4.5 % |
| KDS `s2-c1-s` (cores 1, strict), 9a0525d archive | 8 bookers, contended | yes | 482.5 tps | 477.7 tps | 498.0 tps | **482.5 tps** | 4.3 % |
| KDS `s2-c8-s` (cores 8, strict), 9a0525d archive | 8 bookers, contended | yes | 864.2 tps | 741.1 tps | 1,052.6 tps | **864.2 tps** | 42.0 % |

PostgreSQL `on` 392.2 tps against KDS 378.2 (group) and 377.7 (strict): 0.96x
both. The KDS spreads (3.0 %, 3.5 %) and PostgreSQL's (2.4 %) are the floor;
0.96x is a 4 % gap and at the edge of it, so the reading is "equal to within
a few percent", not a loss. Group and strict agree to 0.1 %, as they
should with one committer and nothing to batch. The `off` control runs 977.5
tps (2.5x the durable cell); the 9a0525d file's 8-booker rows are listed for
the record and read in section 6, not here.

| KDS cell | KDS median | PG-on median | KDS / PG-on | like-for-like? |
|---|---|---|---|---|
| `--bookers 1` cores 1 group | 378.2 tps | 392.2 tps | 0.96x | yes: 1 client, durable, same work |
| `--bookers 1` cores 1 strict | 377.7 tps | 392.2 tps | 0.96x | yes: 1 client, durable, same work |
| `s2-c1-g` 8 bookers | 537.6 tps | 392.2 tps | 1.37x | no: 8 clients vs 1 |
| `s2-c8-g` 8 bookers | 1,519.0 tps | 392.2 tps | 3.87x | no: 8 clients vs 1 |
| `s2-c1-s` 8 bookers | 482.5 tps | 392.2 tps | 1.23x | no: 8 clients vs 1 |
| `s2-c8-s` 8 bookers | 864.2 tps | 392.2 tps | 2.20x | no: 8 clients vs 1 |

## 4. Percentiles

Booking (the eight-statement transaction, client-perceived) and its phases,
all three runs of each cell. `ops` exceeds 3,000 for `booking` and the reads
because a rejected attempt opens and closes a span. `s2-pg-off-*` cells are
**non-durable**. Microseconds:

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `s2-pg-on-r1` | booking | 3,340 ops | 335.7 µs | 2,144.3 µs | 2,260.9 µs | 3,269.9 µs | 7,234.5 µs | 31,664.7 µs | 0 errors |
| `s2-pg-on-r1` | commit | 3,000 ops | 1,099.0 µs | 1,303.6 µs | 1,383.6 µs | 2,529.9 µs | 6,448.6 µs | 30,730.1 µs | 0 errors |
| `s2-pg-on-r1` | freight-insert | 3,000 ops | 74.3 µs | 76.9 µs | 78.1 µs | 103.5 µs | 118.9 µs | 332.3 µs | 0 errors |
| `s2-pg-on-r1` | charge-insert | 16,889 ops | 47.0 µs | 49.6 µs | 51.0 µs | 73.4 µs | 91.2 µs | 212.3 µs | 0 errors |
| `s2-pg-on-r1` | operation-update | 3,000 ops | 67.4 µs | 69.7 µs | 70.6 µs | 94.5 µs | 102.9 µs | 147.8 µs | 0 errors |
| `s2-pg-on-r1` | org-update | 3,000 ops | 65.7 µs | 67.5 µs | 68.2 µs | 91.5 µs | 97.8 µs | 148.3 µs | 0 errors |
| `s2-pg-on-r1` | cargo-lookup | 3,340 ops | 59.9 µs | 75.4 µs | 76.9 µs | 97.8 µs | 107.1 µs | 265.9 µs | 0 errors |
| `s2-pg-on-r1` | credit-lookup | 3,340 ops | 56.4 µs | 64.5 µs | 65.7 µs | 89.2 µs | 97.9 µs | 150.0 µs | 0 errors |
| `s2-pg-on-r1` | capacity-read | 3,340 ops | 54.7 µs | 62.5 µs | 63.4 µs | 86.8 µs | 95.8 µs | 137.5 µs | 0 errors |
| `s2-pg-on-r1` | recipe-read | 3,340 ops | 79.7 µs | 86.6 µs | 88.5 µs | 113.5 µs | 126.1 µs | 186.9 µs | 0 errors |
| `s2-pg-on-r1` | manifest-scan | 140 ops | 67.9 µs | 85.1 µs | 92.5 µs | 129.6 µs | 147.0 µs | 154.3 µs | 0 errors |
| `s2-pg-on-r2` | booking | 3,340 ops | 348.4 µs | 2,140.8 µs | 2,255.3 µs | 3,118.1 µs | 6,426.3 µs | 46,743.0 µs | 0 errors |
| `s2-pg-on-r2` | commit | 3,000 ops | 1,076.4 µs | 1,274.6 µs | 1,341.0 µs | 2,336.0 µs | 5,594.8 µs | 45,924.5 µs | 0 errors |
| `s2-pg-on-r2` | freight-insert | 3,000 ops | 74.9 µs | 78.2 µs | 79.7 µs | 105.1 µs | 124.3 µs | 329.6 µs | 0 errors |
| `s2-pg-on-r2` | charge-insert | 16,889 ops | 49.3 µs | 51.5 µs | 53.1 µs | 76.0 µs | 95.2 µs | 1,590.7 µs | 0 errors |
| `s2-pg-on-r2` | operation-update | 3,000 ops | 68.8 µs | 71.3 µs | 72.2 µs | 96.0 µs | 109.6 µs | 168.7 µs | 0 errors |
| `s2-pg-on-r2` | org-update | 3,000 ops | 67.1 µs | 69.3 µs | 70.1 µs | 93.9 µs | 105.6 µs | 159.2 µs | 0 errors |
| `s2-pg-on-r2` | cargo-lookup | 3,340 ops | 61.2 µs | 76.2 µs | 77.8 µs | 98.5 µs | 115.3 µs | 282.7 µs | 0 errors |
| `s2-pg-on-r2` | credit-lookup | 3,340 ops | 58.8 µs | 66.5 µs | 67.8 µs | 91.0 µs | 101.7 µs | 127.5 µs | 0 errors |
| `s2-pg-on-r2` | capacity-read | 3,340 ops | 56.4 µs | 64.7 µs | 65.5 µs | 88.3 µs | 99.6 µs | 127.9 µs | 0 errors |
| `s2-pg-on-r2` | recipe-read | 3,340 ops | 82.0 µs | 88.8 µs | 90.7 µs | 115.4 µs | 133.9 µs | 163.8 µs | 0 errors |
| `s2-pg-on-r2` | manifest-scan | 140 ops | 70.3 µs | 85.6 µs | 93.3 µs | 115.2 µs | 125.7 µs | 152.8 µs | 0 errors |
| `s2-pg-on-r3` | booking | 3,340 ops | 340.2 µs | 2,110.6 µs | 2,221.6 µs | 3,236.0 µs | 6,423.8 µs | 20,785.8 µs | 0 errors |
| `s2-pg-on-r3` | commit | 3,000 ops | 1,062.3 µs | 1,263.7 µs | 1,345.4 µs | 2,500.3 µs | 5,748.3 µs | 19,872.8 µs | 0 errors |
| `s2-pg-on-r3` | freight-insert | 3,000 ops | 74.6 µs | 77.4 µs | 78.5 µs | 97.5 µs | 120.3 µs | 327.5 µs | 0 errors |
| `s2-pg-on-r3` | charge-insert | 16,889 ops | 47.5 µs | 49.7 µs | 50.9 µs | 68.4 µs | 87.8 µs | 267.1 µs | 0 errors |
| `s2-pg-on-r3` | operation-update | 3,000 ops | 67.0 µs | 70.1 µs | 70.9 µs | 86.6 µs | 97.1 µs | 147.9 µs | 0 errors |
| `s2-pg-on-r3` | org-update | 3,000 ops | 66.5 µs | 68.3 µs | 68.9 µs | 85.3 µs | 95.4 µs | 190.3 µs | 0 errors |
| `s2-pg-on-r3` | cargo-lookup | 3,340 ops | 60.6 µs | 75.3 µs | 76.6 µs | 92.1 µs | 101.4 µs | 265.6 µs | 0 errors |
| `s2-pg-on-r3` | credit-lookup | 3,340 ops | 57.1 µs | 65.3 µs | 66.6 µs | 80.9 µs | 92.6 µs | 154.7 µs | 0 errors |
| `s2-pg-on-r3` | capacity-read | 3,340 ops | 55.4 µs | 63.2 µs | 64.0 µs | 77.8 µs | 89.0 µs | 150.2 µs | 0 errors |
| `s2-pg-on-r3` | recipe-read | 3,340 ops | 79.6 µs | 87.1 µs | 88.9 µs | 105.8 µs | 117.0 µs | 373.4 µs | 0 errors |
| `s2-pg-on-r3` | manifest-scan | 140 ops | 71.0 µs | 85.2 µs | 93.7 µs | 120.0 µs | 135.2 µs | 139.5 µs | 0 errors |
| `s2-pg-off-r1` | booking | 3,340 ops | 333.0 µs | 850.5 µs | 906.1 µs | 1,266.6 µs | 1,345.0 µs | 1,737.5 µs | 0 errors |
| `s2-pg-off-r1` | commit | 3,000 ops | 38.3 µs | 39.2 µs | 39.8 µs | 50.0 µs | 60.9 µs | 112.2 µs | 0 errors |
| `s2-pg-off-r1` | freight-insert | 3,000 ops | 72.2 µs | 74.3 µs | 76.0 µs | 106.5 µs | 123.9 µs | 302.8 µs | 0 errors |
| `s2-pg-off-r1` | charge-insert | 16,889 ops | 47.3 µs | 49.1 µs | 51.2 µs | 77.2 µs | 95.0 µs | 198.5 µs | 0 errors |
| `s2-pg-off-r1` | operation-update | 3,000 ops | 65.9 µs | 68.5 µs | 69.6 µs | 95.7 µs | 113.4 µs | 132.7 µs | 0 errors |
| `s2-pg-off-r1` | org-update | 3,000 ops | 64.4 µs | 66.1 µs | 67.0 µs | 94.5 µs | 112.2 µs | 125.8 µs | 0 errors |
| `s2-pg-off-r1` | cargo-lookup | 3,340 ops | 59.6 µs | 65.4 µs | 66.5 µs | 91.7 µs | 109.4 µs | 265.9 µs | 0 errors |
| `s2-pg-off-r1` | credit-lookup | 3,340 ops | 56.1 µs | 62.4 µs | 63.4 µs | 89.7 µs | 107.2 µs | 125.9 µs | 0 errors |
| `s2-pg-off-r1` | capacity-read | 3,340 ops | 54.3 µs | 61.6 µs | 62.6 µs | 88.4 µs | 105.9 µs | 175.3 µs | 0 errors |
| `s2-pg-off-r1` | recipe-read | 3,340 ops | 78.6 µs | 86.0 µs | 88.1 µs | 116.8 µs | 133.1 µs | 171.6 µs | 0 errors |
| `s2-pg-off-r1` | manifest-scan | 60 ops | 69.9 µs | 90.4 µs | 113.5 µs | 134.5 µs | 146.6 µs | 146.6 µs | 0 errors |
| `s2-pg-off-r2` | booking | 3,340 ops | 332.3 µs | 845.0 µs | 900.0 µs | 1,237.6 µs | 1,318.2 µs | 1,783.1 µs | 0 errors |
| `s2-pg-off-r2` | commit | 3,000 ops | 38.5 µs | 39.4 µs | 39.8 µs | 49.0 µs | 55.6 µs | 75.5 µs | 0 errors |
| `s2-pg-off-r2` | freight-insert | 3,000 ops | 72.0 µs | 74.1 µs | 75.3 µs | 101.6 µs | 120.7 µs | 312.6 µs | 0 errors |
| `s2-pg-off-r2` | charge-insert | 16,889 ops | 46.7 µs | 48.9 µs | 50.4 µs | 73.1 µs | 89.0 µs | 209.8 µs | 0 errors |
| `s2-pg-off-r2` | operation-update | 3,000 ops | 66.5 µs | 68.3 µs | 69.2 µs | 93.2 µs | 108.0 µs | 157.2 µs | 0 errors |
| `s2-pg-off-r2` | org-update | 3,000 ops | 64.7 µs | 66.3 µs | 67.0 µs | 90.2 µs | 107.2 µs | 167.3 µs | 0 errors |
| `s2-pg-off-r2` | cargo-lookup | 3,340 ops | 58.9 µs | 64.7 µs | 65.8 µs | 89.9 µs | 103.2 µs | 267.2 µs | 0 errors |
| `s2-pg-off-r2` | credit-lookup | 3,340 ops | 56.9 µs | 63.2 µs | 64.2 µs | 88.2 µs | 100.2 µs | 139.3 µs | 0 errors |
| `s2-pg-off-r2` | capacity-read | 3,340 ops | 54.8 µs | 62.1 µs | 62.9 µs | 86.3 µs | 98.2 µs | 113.4 µs | 0 errors |
| `s2-pg-off-r2` | recipe-read | 3,340 ops | 79.0 µs | 85.5 µs | 87.4 µs | 112.9 µs | 128.5 µs | 178.9 µs | 0 errors |
| `s2-pg-off-r2` | manifest-scan | 40 ops | 69.5 µs | 85.1 µs | 92.2 µs | 113.2 µs | 115.8 µs | 115.8 µs | 0 errors |
| `s2-pg-off-r3` | booking | 3,340 ops | 336.4 µs | 879.7 µs | 953.6 µs | 1,323.6 µs | 1,363.0 µs | 1,907.8 µs | 0 errors |
| `s2-pg-off-r3` | commit | 3,000 ops | 38.5 µs | 39.9 µs | 41.7 µs | 52.9 µs | 67.2 µs | 89.8 µs | 0 errors |
| `s2-pg-off-r3` | freight-insert | 3,000 ops | 72.8 µs | 75.9 µs | 85.9 µs | 115.8 µs | 130.2 µs | 346.4 µs | 0 errors |
| `s2-pg-off-r3` | charge-insert | 16,889 ops | 47.5 µs | 50.4 µs | 59.5 µs | 81.7 µs | 100.0 µs | 210.8 µs | 0 errors |
| `s2-pg-off-r3` | operation-update | 3,000 ops | 67.9 µs | 70.3 µs | 78.6 µs | 103.5 µs | 115.9 µs | 131.7 µs | 0 errors |
| `s2-pg-off-r3` | org-update | 3,000 ops | 65.8 µs | 67.8 µs | 75.2 µs | 100.6 µs | 114.1 µs | 126.3 µs | 0 errors |
| `s2-pg-off-r3` | cargo-lookup | 3,340 ops | 60.9 µs | 66.8 µs | 75.9 µs | 99.7 µs | 112.3 µs | 374.9 µs | 0 errors |
| `s2-pg-off-r3` | credit-lookup | 3,340 ops | 57.7 µs | 64.8 µs | 72.7 µs | 98.2 µs | 112.4 µs | 162.5 µs | 0 errors |
| `s2-pg-off-r3` | capacity-read | 3,340 ops | 55.8 µs | 64.1 µs | 72.5 µs | 95.7 µs | 110.8 µs | 132.6 µs | 0 errors |
| `s2-pg-off-r3` | recipe-read | 3,340 ops | 81.8 µs | 88.5 µs | 99.0 µs | 126.2 µs | 137.6 µs | 184.2 µs | 0 errors |
| `s2-pg-off-r3` | manifest-scan | 60 ops | 85.9 µs | 99.6 µs | 112.7 µs | 146.2 µs | 159.7 µs | 159.7 µs | 0 errors |
| `s2-kds-b1-c1-g-r1` | booking | 3,402 ops | 471.9 µs | 2,195.9 µs | 2,307.3 µs | 3,573.1 µs | 6,697.7 µs | 40,061.5 µs | 0 errors |
| `s2-kds-b1-c1-g-r1` | commit | 3,000 ops | 1,079.5 µs | 1,255.2 µs | 1,325.5 µs | 2,543.6 µs | 5,347.8 µs | 39,112.2 µs | 0 errors |
| `s2-kds-b1-c1-g-r1` | freight-insert | 3,000 ops | 53.6 µs | 62.6 µs | 64.1 µs | 89.1 µs | 104.3 µs | 1,405.1 µs | 0 errors |
| `s2-kds-b1-c1-g-r1` | charge-insert | 16,907 ops | 42.5 µs | 50.4 µs | 51.5 µs | 74.1 µs | 93.5 µs | 1,562.3 µs | 0 errors |
| `s2-kds-b1-c1-g-r1` | operation-update | 3,000 ops | 56.1 µs | 58.2 µs | 58.9 µs | 81.6 µs | 107.6 µs | 1,359.6 µs | 0 errors |
| `s2-kds-b1-c1-g-r1` | org-update | 3,000 ops | 53.9 µs | 55.7 µs | 56.4 µs | 80.3 µs | 96.1 µs | 1,323.8 µs | 0 errors |
| `s2-kds-b1-c1-g-r1` | cargo-lookup | 3,402 ops | 73.8 µs | 87.3 µs | 88.9 µs | 116.4 µs | 127.3 µs | 368.8 µs | 0 errors |
| `s2-kds-b1-c1-g-r1` | credit-lookup | 3,402 ops | 66.8 µs | 72.1 µs | 73.4 µs | 106.2 µs | 130.0 µs | 1,493.3 µs | 0 errors |
| `s2-kds-b1-c1-g-r1` | capacity-read | 3,402 ops | 62.2 µs | 68.7 µs | 69.7 µs | 103.3 µs | 128.3 µs | 1,527.4 µs | 0 errors |
| `s2-kds-b1-c1-g-r1` | recipe-read | 3,402 ops | 164.4 µs | 181.7 µs | 186.8 µs | 221.7 µs | 255.7 µs | 1,636.0 µs | 0 errors |
| `s2-kds-b1-c1-g-r1` | manifest-scan | 160 ops | 68.0 µs | 154.9 µs | 278.3 µs | 1,509.8 µs | 1,698.9 µs | 1,870.2 µs | 0 errors |
| `s2-kds-b1-c1-g-r2` | booking | 3,402 ops | 475.5 µs | 2,175.0 µs | 2,280.0 µs | 3,659.8 µs | 7,673.6 µs | 42,927.5 µs | 0 errors |
| `s2-kds-b1-c1-g-r2` | commit | 3,000 ops | 1,081.9 µs | 1,233.8 µs | 1,305.5 µs | 2,635.8 µs | 6,605.4 µs | 41,940.7 µs | 0 errors |
| `s2-kds-b1-c1-g-r2` | freight-insert | 3,000 ops | 52.7 µs | 62.4 µs | 63.8 µs | 83.6 µs | 102.9 µs | 1,387.4 µs | 0 errors |
| `s2-kds-b1-c1-g-r2` | charge-insert | 16,907 ops | 43.2 µs | 50.4 µs | 51.4 µs | 70.3 µs | 90.7 µs | 1,592.2 µs | 0 errors |
| `s2-kds-b1-c1-g-r2` | operation-update | 3,000 ops | 56.3 µs | 58.1 µs | 58.7 µs | 76.4 µs | 88.8 µs | 941.7 µs | 0 errors |
| `s2-kds-b1-c1-g-r2` | org-update | 3,000 ops | 49.7 µs | 55.5 µs | 56.2 µs | 74.4 µs | 86.5 µs | 948.4 µs | 0 errors |
| `s2-kds-b1-c1-g-r2` | cargo-lookup | 3,402 ops | 73.5 µs | 86.8 µs | 88.5 µs | 106.6 µs | 122.4 µs | 1,910.4 µs | 0 errors |
| `s2-kds-b1-c1-g-r2` | credit-lookup | 3,402 ops | 59.4 µs | 71.9 µs | 73.1 µs | 95.4 µs | 129.1 µs | 1,590.0 µs | 0 errors |
| `s2-kds-b1-c1-g-r2` | capacity-read | 3,402 ops | 65.0 µs | 68.8 µs | 69.8 µs | 91.6 µs | 122.9 µs | 1,697.7 µs | 0 errors |
| `s2-kds-b1-c1-g-r2` | recipe-read | 3,402 ops | 164.8 µs | 179.9 µs | 185.8 µs | 211.4 µs | 243.3 µs | 1,659.4 µs | 0 errors |
| `s2-kds-b1-c1-g-r2` | manifest-scan | 160 ops | 58.5 µs | 163.3 µs | 238.3 µs | 1,403.7 µs | 1,743.2 µs | 2,053.0 µs | 0 errors |
| `s2-kds-b1-c1-g-r3` | booking | 3,402 ops | 472.8 µs | 2,213.4 µs | 2,325.2 µs | 3,917.5 µs | 7,742.1 µs | 47,977.6 µs | 0 errors |
| `s2-kds-b1-c1-g-r3` | commit | 3,000 ops | 1,080.5 µs | 1,262.5 µs | 1,342.3 µs | 2,903.5 µs | 6,282.2 µs | 23,419.0 µs | 0 errors |
| `s2-kds-b1-c1-g-r3` | freight-insert | 3,000 ops | 59.0 µs | 62.4 µs | 63.9 µs | 88.6 µs | 105.4 µs | 1,511.4 µs | 0 errors |
| `s2-kds-b1-c1-g-r3` | charge-insert | 16,907 ops | 43.0 µs | 50.6 µs | 51.9 µs | 74.2 µs | 93.1 µs | 45,032.4 µs | 0 errors |
| `s2-kds-b1-c1-g-r3` | operation-update | 3,000 ops | 56.1 µs | 58.0 µs | 58.7 µs | 81.2 µs | 96.1 µs | 1,629.2 µs | 0 errors |
| `s2-kds-b1-c1-g-r3` | org-update | 3,000 ops | 50.6 µs | 55.4 µs | 56.2 µs | 79.9 µs | 103.4 µs | 1,658.2 µs | 0 errors |
| `s2-kds-b1-c1-g-r3` | cargo-lookup | 3,402 ops | 73.6 µs | 87.5 µs | 89.1 µs | 116.9 µs | 134.1 µs | 1,357.4 µs | 0 errors |
| `s2-kds-b1-c1-g-r3` | credit-lookup | 3,402 ops | 65.8 µs | 72.3 µs | 73.5 µs | 105.9 µs | 127.3 µs | 1,442.3 µs | 0 errors |
| `s2-kds-b1-c1-g-r3` | capacity-read | 3,402 ops | 61.4 µs | 68.5 µs | 69.8 µs | 102.5 µs | 129.7 µs | 1,469.5 µs | 0 errors |
| `s2-kds-b1-c1-g-r3` | recipe-read | 3,402 ops | 163.6 µs | 182.6 µs | 188.2 µs | 221.7 µs | 281.0 µs | 1,908.9 µs | 0 errors |
| `s2-kds-b1-c1-g-r3` | manifest-scan | 180 ops | 59.0 µs | 176.3 µs | 247.2 µs | 419.3 µs | 1,631.8 µs | 1,769.6 µs | 0 errors |
| `s2-kds-b1-c1-s-r1` | booking | 3,402 ops | 467.1 µs | 2,164.0 µs | 2,266.9 µs | 3,631.6 µs | 7,319.3 µs | 40,161.8 µs | 0 errors |
| `s2-kds-b1-c1-s-r1` | commit | 3,000 ops | 1,072.9 µs | 1,215.1 µs | 1,283.8 µs | 2,511.5 µs | 5,720.1 µs | 11,565.9 µs | 0 errors |
| `s2-kds-b1-c1-s-r1` | freight-insert | 3,000 ops | 58.9 µs | 62.3 µs | 64.0 µs | 91.0 µs | 111.1 µs | 1,374.2 µs | 0 errors |
| `s2-kds-b1-c1-s-r1` | charge-insert | 16,907 ops | 42.6 µs | 50.5 µs | 51.7 µs | 76.5 µs | 97.1 µs | 1,547.4 µs | 0 errors |
| `s2-kds-b1-c1-s-r1` | operation-update | 3,000 ops | 52.4 µs | 57.7 µs | 58.4 µs | 82.7 µs | 101.3 µs | 20,211.0 µs | 0 errors |
| `s2-kds-b1-c1-s-r1` | org-update | 3,000 ops | 51.5 µs | 55.4 µs | 56.2 µs | 81.7 µs | 102.5 µs | 1,336.2 µs | 0 errors |
| `s2-kds-b1-c1-s-r1` | cargo-lookup | 3,402 ops | 72.8 µs | 87.2 µs | 88.8 µs | 118.0 µs | 136.6 µs | 356.4 µs | 0 errors |
| `s2-kds-b1-c1-s-r1` | credit-lookup | 3,402 ops | 65.3 µs | 71.9 µs | 73.2 µs | 108.5 µs | 131.0 µs | 919.8 µs | 0 errors |
| `s2-kds-b1-c1-s-r1` | capacity-read | 3,402 ops | 65.0 µs | 68.3 µs | 69.6 µs | 105.0 µs | 128.2 µs | 1,670.3 µs | 0 errors |
| `s2-kds-b1-c1-s-r1` | recipe-read | 3,402 ops | 164.9 µs | 181.4 µs | 186.9 µs | 227.2 µs | 259.4 µs | 1,625.9 µs | 0 errors |
| `s2-kds-b1-c1-s-r1` | manifest-scan | 160 ops | 57.1 µs | 135.0 µs | 262.9 µs | 1,466.3 µs | 1,603.8 µs | 6,812.7 µs | 0 errors |
| `s2-kds-b1-c1-s-r2` | booking | 3,402 ops | 467.2 µs | 2,173.9 µs | 2,286.9 µs | 3,761.9 µs | 8,908.2 µs | 40,189.9 µs | 0 errors |
| `s2-kds-b1-c1-s-r2` | commit | 3,000 ops | 1,085.8 µs | 1,231.9 µs | 1,301.9 µs | 2,748.6 µs | 7,680.9 µs | 24,349.5 µs | 0 errors |
| `s2-kds-b1-c1-s-r2` | freight-insert | 3,000 ops | 58.1 µs | 61.8 µs | 63.5 µs | 91.5 µs | 109.1 µs | 1,339.3 µs | 0 errors |
| `s2-kds-b1-c1-s-r2` | charge-insert | 16,907 ops | 42.6 µs | 50.6 µs | 51.8 µs | 76.6 µs | 99.9 µs | 28,126.5 µs | 0 errors |
| `s2-kds-b1-c1-s-r2` | operation-update | 3,000 ops | 56.2 µs | 57.9 µs | 58.6 µs | 82.5 µs | 106.4 µs | 1,321.8 µs | 0 errors |
| `s2-kds-b1-c1-s-r2` | org-update | 3,000 ops | 49.5 µs | 55.7 µs | 56.5 µs | 81.3 µs | 114.7 µs | 1,817.3 µs | 0 errors |
| `s2-kds-b1-c1-s-r2` | cargo-lookup | 3,402 ops | 72.4 µs | 86.6 µs | 88.3 µs | 118.5 µs | 134.1 µs | 273.6 µs | 0 errors |
| `s2-kds-b1-c1-s-r2` | credit-lookup | 3,402 ops | 65.5 µs | 71.9 µs | 73.3 µs | 112.5 µs | 148.5 µs | 1,643.7 µs | 0 errors |
| `s2-kds-b1-c1-s-r2` | capacity-read | 3,402 ops | 62.6 µs | 68.2 µs | 69.3 µs | 107.8 µs | 130.9 µs | 1,661.3 µs | 0 errors |
| `s2-kds-b1-c1-s-r2` | recipe-read | 3,402 ops | 164.3 µs | 181.7 µs | 186.8 µs | 229.2 µs | 259.3 µs | 1,756.4 µs | 0 errors |
| `s2-kds-b1-c1-s-r2` | manifest-scan | 180 ops | 75.2 µs | 177.6 µs | 240.5 µs | 1,387.7 µs | 4,468.4 µs | 11,351.9 µs | 0 errors |
| `s2-kds-b1-c1-s-r3` | booking | 3,402 ops | 466.8 µs | 2,161.3 µs | 2,261.3 µs | 3,529.6 µs | 7,481.3 µs | 47,341.1 µs | 0 errors |
| `s2-kds-b1-c1-s-r3` | commit | 3,000 ops | 1,060.0 µs | 1,213.7 µs | 1,282.3 µs | 2,504.6 µs | 6,289.2 µs | 12,304.2 µs | 0 errors |
| `s2-kds-b1-c1-s-r3` | freight-insert | 3,000 ops | 59.0 µs | 62.5 µs | 63.9 µs | 80.4 µs | 102.1 µs | 1,212.2 µs | 0 errors |
| `s2-kds-b1-c1-s-r3` | charge-insert | 16,907 ops | 42.8 µs | 50.6 µs | 51.7 µs | 67.4 µs | 88.8 µs | 1,640.5 µs | 0 errors |
| `s2-kds-b1-c1-s-r3` | operation-update | 3,000 ops | 53.6 µs | 58.0 µs | 58.6 µs | 73.9 µs | 95.1 µs | 25,637.8 µs | 0 errors |
| `s2-kds-b1-c1-s-r3` | org-update | 3,000 ops | 48.4 µs | 55.7 µs | 56.4 µs | 71.6 µs | 86.4 µs | 1,510.9 µs | 0 errors |
| `s2-kds-b1-c1-s-r3` | cargo-lookup | 3,402 ops | 74.5 µs | 87.2 µs | 88.9 µs | 106.8 µs | 123.8 µs | 1,581.6 µs | 0 errors |
| `s2-kds-b1-c1-s-r3` | credit-lookup | 3,402 ops | 67.3 µs | 72.6 µs | 74.0 µs | 95.2 µs | 127.6 µs | 1,552.2 µs | 0 errors |
| `s2-kds-b1-c1-s-r3` | capacity-read | 3,402 ops | 65.7 µs | 69.3 µs | 70.4 µs | 91.3 µs | 121.1 µs | 1,372.0 µs | 0 errors |
| `s2-kds-b1-c1-s-r3` | recipe-read | 3,402 ops | 163.9 µs | 180.8 µs | 186.6 µs | 211.2 µs | 251.8 µs | 1,488.3 µs | 0 errors |
| `s2-kds-b1-c1-s-r3` | manifest-scan | 160 ops | 68.5 µs | 144.1 µs | 224.8 µs | 814.6 µs | 1,515.9 µs | 1,522.8 µs | 0 errors |

For scale, the 9a0525d KDS 8-booker cells (run 1; the queue at one core and
the commit path at eight are that file's findings):

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | booking | 3,349 ops | 718.3 µs | 11,041.9 µs | 12,869.2 µs | 20,845.8 µs | 31,102.2 µs | 66,453.2 µs | 0 errors |
| `s2-c1-g` | commit | 3,000 ops | 1,326.9 µs | 1,616.5 µs | 1,762.6 µs | 4,103.2 µs | 7,977.5 µs | 33,632.8 µs | 0 errors |
| `s2-c1-g` | charge-insert | 16,918 ops | 44.5 µs | 176.3 µs | 261.1 µs | 1,854.4 µs | 3,572.7 µs | 22,943.5 µs | 0 errors |
| `s2-c1-g` | recipe-read | 3,349 ops | 189.7 µs | 372.9 µs | 525.4 µs | 2,172.1 µs | 4,276.9 µs | 20,415.9 µs | 0 errors |
| `s2-c8-g` | booking | 3,360 ops | 571.9 µs | 3,808.9 µs | 4,234.6 µs | 7,996.1 µs | 15,314.4 µs | 119,362.5 µs | 0 errors |
| `s2-c8-g` | commit | 3,000 ops | 1,149.1 µs | 1,913.6 µs | 2,312.3 µs | 5,760.4 µs | 13,375.0 µs | 61,345.4 µs | 0 errors |
| `s2-c8-g` | charge-insert | 16,926 ops | 39.0 µs | 96.5 µs | 123.2 µs | 244.1 µs | 389.8 µs | 115,683.2 µs | 0 errors |
| `s2-c8-g` | recipe-read | 3,360 ops | 177.2 µs | 271.3 µs | 298.8 µs | 476.1 µs | 593.8 µs | 6,461.1 µs | 0 errors |
| `s2-c1-s` | booking | 3,339 ops | 509.7 µs | 11,631.0 µs | 14,325.9 µs | 24,570.0 µs | 32,571.8 µs | 89,082.5 µs | 0 errors |
| `s2-c1-s` | commit | 3,000 ops | 1,163.8 µs | 1,470.9 µs | 1,634.7 µs | 4,597.8 µs | 8,906.4 µs | 21,151.5 µs | 0 errors |
| `s2-c1-s` | charge-insert | 16,919 ops | 43.1 µs | 201.4 µs | 318.1 µs | 3,061.1 µs | 5,525.1 µs | 69,612.2 µs | 0 errors |
| `s2-c1-s` | recipe-read | 3,339 ops | 171.9 µs | 333.5 µs | 466.7 µs | 3,189.9 µs | 5,909.5 µs | 21,520.9 µs | 0 errors |
| `s2-c8-s` | booking | 3,321 ops | 498.7 µs | 4,124.2 µs | 5,864.2 µs | 14,072.5 µs | 19,624.9 µs | 94,914.9 µs | 0 errors |
| `s2-c8-s` | commit | 3,000 ops | 1,149.3 µs | 2,145.5 µs | 2,994.1 µs | 8,141.6 µs | 13,750.6 µs | 20,424.2 µs | 0 errors |
| `s2-c8-s` | charge-insert | 17,597 ops | 34.2 µs | 63.4 µs | 78.5 µs | 193.6 µs | 2,782.4 µs | 18,061.6 µs | 0 errors |
| `s2-c8-s` | recipe-read | 3,446 ops | 169.2 µs | 224.1 µs | 268.1 µs | 424.1 µs | 3,458.8 µs | 9,272.6 µs | 0 errors |

## 5. Where the time goes

| cell | cargo-lookup | credit-lookup | capacity-read | recipe-read | freight-insert | charge-insert | operation-update | org-update | commit | unattributed (BEGIN, refused attempts, client, socket) | booking mean |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `s2-pg-on-r1` | 3.5 % | 3.0 % | 2.9 % | 4.0 % | 3.2 % | 12.2 % | 2.9 % | 2.8 % | 62.4 % | 3.0 % | 2,276 µs |
| `s2-pg-off-r1` (non-durable) | 8.0 % | 7.6 % | 7.6 % | 10.4 % | 8.2 % | 31.9 % | 7.5 % | 7.3 % | 4.2 % | 7.4 % | 910 µs |
| `s2-kds-b1-c1-g-r1` | 4.0 % | 3.4 % | 3.3 % | 8.2 % | 2.6 % | 12.0 % | 2.4 % | 2.3 % | 57.7 % | 4.0 % | 2,326 µs |
| `s2-kds-b1-c1-s-r1` | 4.0 % | 3.4 % | 3.3 % | 8.3 % | 2.7 % | 12.3 % | 2.7 % | 2.4 % | 56.3 % | 4.5 % | 2,329 µs |
| KDS `s2-c8-g` (8 bookers) | 3.3 % | 3.1 % | 3.0 % | 6.8 % | 3.2 % | 15.9 % | 2.6 % | 2.9 % | 55.4 % | 3.8 % | 4,645 µs |

| Wait | Reading |
|---|---|
| Durability / commit | **62.4 % of a PostgreSQL booking, 57.7 % (group) and 56.3 % (strict) of a KDS booking, and 4.2 % of a PostgreSQL booking with `synchronous_commit = off`**. `commit` p50 is 1,383.6 µs on PostgreSQL, 1,325.5 µs at KDS group and 1,283.8 µs at KDS strict (run 1), against 39.8 µs non-durable: the WAL flush is 1.3 to 1.4 ms on this device on both engines, and a single committer cannot share it (in the instrumented PG cell 9,592 commits, including the load's and the read-only ones, meet 7,751 WAL fsyncs: 1.2 commits per fsync, against 3.9 in scenario 0) |
| Write statements | `freight-insert`, `charge-insert`, `operation-update`, `org-update`: 2.3 to 12.3 % each on the two durable engines; the KDS ones are 0.95x to 1.19x of PostgreSQL's rate (the per-phase table above) |
| Reads | `cargo-lookup`, `credit-lookup`, `capacity-read` are pk-addressed: KDS 0.84x to 0.87x of PostgreSQL. `recipe-read` (a non-pk equality) and `manifest-scan` are the two the PostgreSQL twin gets an index for: KDS at 0.47x to 0.48x and 0.28x to 0.29x. `manifest-scan` is also not run the same way: the KDS driver's reporter is a second process, concurrent with the booker on the one reactor (its p95 is 1.7x to 6x its p50), where the twin interleaves it between bookings, so the 0.28x is not the walk alone; the "1 booker" cells are one booker plus that reporter |
| Lock or conflict wait | none: one connection, 0 conflicts, 0 error replies in all twelve cells |
| Client and socket | the unattributed column, 3.0 to 4.5 % on the durable cells and 7.4 % on `off`, where the Python client is a larger share of a 910 µs booking |

Per-phase derived qps (1,000,000 / mean µs; exact for one serial connection),
median of three runs:

| phase | PG on | PG off (non-durable) | KDS c1 group b1 | KDS c1 strict b1 | KDS group / PG on | KDS strict / PG on |
|---|---|---|---|---|---|---|
| booking | 440 qps | 1,099 qps | 430 qps | 429 qps | 0.98x | 0.98x |
| commit | 648 qps | 23,641 qps | 650 qps | 672 qps | 1.00x | 1.04x |
| freight-insert | 12,151 qps | 11,976 qps | 14,514 qps | 14,265 qps | 1.19x | 1.17x |
| charge-insert | 18,215 qps | 17,452 qps | 17,730 qps | 17,331 qps | 0.97x | 0.95x |
| operation-update | 13,550 qps | 13,123 qps | 15,674 qps | 14,104 qps | 1.16x | 1.04x |
| org-update | 13,986 qps | 13,550 qps | 16,260 qps | 16,026 qps | 1.16x | 1.15x |
| cargo-lookup | 12,706 qps | 13,793 qps | 10,858 qps | 10,753 qps | 0.85x | 0.85x |
| credit-lookup | 14,749 qps | 14,430 qps | 12,594 qps | 12,453 qps | 0.85x | 0.84x |
| capacity-read | 15,175 qps | 14,556 qps | 13,158 qps | 12,920 qps | 0.87x | 0.85x |
| recipe-read | 10,929 qps | 10,593 qps | 5,219 qps | 5,168 qps | 0.48x | 0.47x |
| manifest-scan | 10,482 qps | 9,285 qps | 3,073 qps | 2,902 qps | 0.29x | 0.28x |

## 6. The 9a0525d 8-booker cells are a different question

They are on the record and read against PostgreSQL only with the label
"8 clients versus 1": KDS `c1-g` 537.6 tps (1.37x), `c8-g` 1,519.0 tps
(3.87x), `c1-s` 482.5 tps (1.23x), `c8-s` 864.2 tps (2.20x), median of three
each. What they measure is how KDS scales with concurrent clients (2.83x
from one core to eight under `group`, the 9a0525d file's headline); how
PostgreSQL scales under eight contended bookers this run cannot say,
because the twin has one connection. Its only PG
concurrency figure is scenario 1's connection sweep (390.7 qps at one
connection to 1,725.1 at eight, 4.4x, in that file), on a different shape.
The 3.87x is therefore not "KDS beats PostgreSQL by 3.9x"; it is one figure
of a comparison whose other side was never measured at eight clients.

## 7. Correctness

| cell | committed | rejected-capacity | rejected-credit | conflicted | verify failures / checks | error replies (all phases) | driver exit |
|---|---|---|---|---|---|---|---|
| `s2-pg-on-r1` | 3,000 bookings | 289 bookings | 51 bookings | 0 conflicts | 0 / 400 checks | 0 replies | exit 0 |
| `s2-pg-on-r2` | 3,000 bookings | 289 bookings | 51 bookings | 0 conflicts | 0 / 400 checks | 0 replies | exit 0 |
| `s2-pg-on-r3` | 3,000 bookings | 289 bookings | 51 bookings | 0 conflicts | 0 / 400 checks | 0 replies | exit 0 |
| `s2-pg-off-r1` | 3,000 bookings | 289 bookings | 51 bookings | 0 conflicts | 0 / 400 checks | 0 replies | exit 0 |
| `s2-pg-off-r2` | 3,000 bookings | 289 bookings | 51 bookings | 0 conflicts | 0 / 400 checks | 0 replies | exit 0 |
| `s2-pg-off-r3` | 3,000 bookings | 289 bookings | 51 bookings | 0 conflicts | 0 / 400 checks | 0 replies | exit 0 |
| `s2-kds-b1-c1-g-r1` | 3,000 bookings | 328 bookings | 74 bookings | 0 conflicts | 0 / 400 checks | 0 replies | exit 0 |
| `s2-kds-b1-c1-g-r2` | 3,000 bookings | 328 bookings | 74 bookings | 0 conflicts | 0 / 400 checks | 0 replies | exit 0 |
| `s2-kds-b1-c1-g-r3` | 3,000 bookings | 328 bookings | 74 bookings | 0 conflicts | 0 / 400 checks | 0 replies | exit 0 |
| `s2-kds-b1-c1-s-r1` | 3,000 bookings | 328 bookings | 74 bookings | 0 conflicts | 0 / 400 checks | 0 replies | exit 0 |
| `s2-kds-b1-c1-s-r2` | 3,000 bookings | 328 bookings | 74 bookings | 0 conflicts | 0 / 400 checks | 0 replies | exit 0 |
| `s2-kds-b1-c1-s-r3` | 3,000 bookings | 328 bookings | 74 bookings | 0 conflicts | 0 / 400 checks | 0 replies | exit 0 |

The invariant check (I1 booked capacity against the sum of freight volumes, I2
within ship capacity, I3 outstanding against recomputed charges, I4 charge
rows against rules) passes **400 of 400 in all twelve cells**, and every
phase replies with zero errors. The lost update that fails 13 to 31 of 400
checks in every 8-booker KDS default-isolation cell (9a0525d file, section 6)
needs two concurrent writers of one voyage; with one booker it does not
occur, on either engine, which is the point of the matched cells and a
second reason the 8-booker numbers are not comparable.

## 8. Noise floor and host stamps

Three-run spreads: PostgreSQL `on` 2.4 %, `off` 13.9 %, KDS group 3.0 %,
KDS strict 3.5 %. Any difference below about 4 % between the durable cells
is not a result. Host stamps, every cell (monitored window with no foreign
process, `validity.py`):

| cell | precheck UTC | loadavg 1/5/15 | cc1plus / cmake --build / ctest |
|---|---|---|---|
| `s2-pg-on-r1` | 2026-09-30T08:23:33Z | 1.34 / 1.54 / 1.81 | none |
| `s2-kds-b1-c1-g-r1` | 2026-09-30T08:23:49Z | 1.26 / 1.52 / 1.80 | none |
| `s2-pg-off-r1` | 2026-09-30T08:24:14Z | 1.25 / 1.49 / 1.78 | none |
| `s2-kds-b1-c1-s-r1` | 2026-09-30T08:24:19Z | 1.23 / 1.48 / 1.78 | none |
| `s2-pg-on-r2` | 2026-09-30T08:24:43Z | 1.21 / 1.46 / 1.76 | none |
| `s2-kds-b1-c1-g-r2` | 2026-09-30T08:25:00Z | 1.39 / 1.48 / 1.77 | none |
| `s2-pg-off-r2` | 2026-09-30T08:25:25Z | 1.25 / 1.44 / 1.74 | none |
| `s2-kds-b1-c1-s-r2` | 2026-09-30T08:25:30Z | 1.23 / 1.44 / 1.74 | none |
| `s2-pg-on-r3` | 2026-09-30T08:25:54Z | 1.15 / 1.40 / 1.72 | none |
| `s2-kds-b1-c1-g-r3` | 2026-09-30T08:26:10Z | 1.12 / 1.38 / 1.71 | none |
| `s2-pg-off-r3` | 2026-09-30T08:26:35Z | 1.08 / 1.35 / 1.69 | none |
| `s2-kds-b1-c1-s-r3` | 2026-09-30T08:26:41Z | 1.07 / 1.34 / 1.68 | none |

## 9. What the run says about the engine

1. **At one client the durable booking is bound by the device's flush, and
   KDS pays it exactly as PostgreSQL does.** 56 to 62 % of a booking is
   the commit, 1.3 to 1.4 ms on both, 648/s (PostgreSQL) and 650 to 672/s
   (KDS) commits at the mean. Removing the wait (PostgreSQL `off`) takes the
   booking from 2,276 to 910 µs. Nothing in KDS's commit path adds a measurable
   cost over PostgreSQL's on one client.
2. **KDS's write statements are as fast as PostgreSQL's**, and slightly
   faster on inserts and updates (1.16x to 1.19x on `freight-insert` and the
   two updates under group), with no index-maintenance advantage claimed:
   both relations carry a pk index on both engines since AS-Q6.
3. **The read side is where KDS is behind the floor**: `recipe-read` at 0.47x to 0.48x
   and `manifest-scan` at 0.28x to 0.29x, the two statements PostgreSQL answers
   from an index the twin creates for it. The KDS driver declares no index
   and no Cabin on those columns, so both walk their relation; the pk reads
   are 0.84x to 0.87x, 10 to 14 µs of fixed cost per statement, above the
   4 to 10 µs scenario 3 shows on `pk-user`.
4. **`group` and `strict` are the same mode for one committer**: 378.2 and
   377.7 tps, commit p50 1,325.5 and 1,283.8 µs. The gap the scenario 0 file
   reports (`strict` at 0.24x to 0.47x) needs concurrent committers to
   appear; this file is the control showing it is batching and not a
   slower single flush.
5. **The eight-client question is open.** This run cannot say how KDS at
   `c8-g` compares with PostgreSQL under eight bookers, because the
   PostgreSQL twin is single-connection; only a multi-booker PostgreSQL
   twin would answer it.

Archive: `bench/v3.0.0/archive/scenario2-pg18-v2.7.0-545-gf2f1ee7/`.
