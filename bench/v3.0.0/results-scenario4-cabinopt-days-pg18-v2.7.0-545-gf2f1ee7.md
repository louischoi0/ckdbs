# Scenario 4 (cabin optimizer, business day 1), PostgreSQL 18.6 floor, at `v2.7.0-545-gf2f1ee7`

**One line.** PostgreSQL 18.6 at its defaults has nothing that observes a hot
predicate, so its day-1 board probe is a sequential scan every time
(1,994 qps on a 10,000-row board, spread 2.9 %), and it is **2.1x** the KDS
`off` arm (960 qps, the same walk), **0.83x** the KDS controller arm
(2,403 qps: KDS **1.20x**) and **0.66x** the hand-declared Cabin arm (3,039
qps: KDS **1.52x**). On the 10,000-row tape the same comparison is 1.30x and
1.72x for KDS's `on` and `declared` arms. So the Cabin, automatic or declared,
is the only thing that puts KDS ahead of PostgreSQL's seqscan on this
shape, and KDS's own uncached walk is at half PostgreSQL's speed, the same
per-row cost the scenario 3 file measures. PostgreSQL is the floor and is
clearly cleared by the Cabin arms; it is not cleared by the unaided walk.

Driver: `tools/pg_scenario4_cabinopt_days.py`, unmodified, driver defaults,
three runs. It runs **day 1 only, unpaced, one server, no index**, by its
own design: at defaults a PostgreSQL probe seq-scans on day 1 as on day 300,
so days 2 and 3 (the KDS hot-set rotation) have nothing to act on. KDS side:
`bench/v3.0.0/results-scenario4-cabinopt-days-v2.7.0-531-g9a0525d.md` (the
9a0525d file), **run A day 1 of arms `off`, `on`, `declared`**, shown per
statement group. The comparison is per-statement-shape latency and derived
qps, not the KDS sessions' wall-paced TPS. This is the first PostgreSQL run of
the shape and becomes its baseline.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-09-30; run 1 08:28:18 to 08:28:23 UTC, run 2 08:37:17 to 08:37:22 (re-run; its first execution overlapped a foreign build), run 3 08:39:16 to 08:39:21 (re-run, it followed a foreign burst); each run 3.6 to 3.7 s |
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

Three cells, each a fresh `initdb`, `python3 tools/pg_scenario4_cabinopt_days.py
--port 15700 --json <cell>.json`. The driver imports the KDS driver's row
generators and plan (`--seed 20260810`, same rows, same statement shapes: an
open burst of 240 inserts, 2,400 skewed hot-symbol probes on the 10,000-row
`board_a`, 396 probes on each of `tape_200`, `tape_1k`, `tape_10k`, 240 pk
lookups, 3 rounds of the close aggregates), with `bigserial` for the pk and
no secondary index ever. **Row-set axis (rule 9)**: the tape relations are
200, 1,000 and 10,000 rows and the board 10,000; the size is a row label in
every table. The pk control is one relation and does not scale with rows;
scenario 3's `pk-user` shows it at all three sizes (14,577 to 16,026 qps on
PostgreSQL).

## 3. Throughput

**The Cabin arms beat PostgreSQL's seqscan; KDS's unaided walk does not.**
Derived qps (1,000,000 / mean µs, a serial connection); PostgreSQL is the
median of three runs, KDS is run A day 1 (the 9a0525d file, section 3, one
draw per arm, so the KDS side carries that file's floor: the pk control
0.97x to 1.15x, "a ratio below about 1.15x is not a finding"). Ratios are
KDS over PostgreSQL:

| group | PG (no index) | KDS off (walks) | KDS on (controller) | KDS declared (Cabin) | KDS off / PG | KDS on / PG | KDS declared / PG |
|---|---|---|---|---|---|---|---|
| open | 823 qps | 648 qps | 691 qps | 616 qps | 0.79x | 0.84x | 0.75x |
| board | 1,994 qps | 960 qps | 2,403 qps | 3,039 qps | 0.48x | 1.20x | 1.52x |
| tape200 | 12,690 qps | 7,868 qps | 8,696 qps | 8,905 qps | 0.62x | 0.69x | 0.70x |
| tape1k | 8,985 qps | 5,128 qps | 7,391 qps | 8,197 qps | 0.57x | 0.82x | 0.91x |
| tape10k | 2,113 qps | 1,024 qps | 2,738 qps | 3,636 qps | 0.48x | 1.30x | 1.72x |
| pk | 15,649 qps | 12,315 qps | 12,870 qps | 13,316 qps | 0.79x | 0.82x | 0.85x |
| close | 1,183 qps | 596 qps | 577 qps | 598 qps | 0.50x | 0.49x | 0.51x |

PostgreSQL's own three-run spread (the floor for its column):

| group | run 1 | run 2 | run 3 | spread (max/min-1) |
|---|---|---|---|---|
| open | 748 qps | 833 qps | 823 qps | 11.4 % |
| board | 1,994 qps | 2,009 qps | 1,953 qps | 2.9 % |
| tape200 | 13,158 qps | 12,690 qps | 12,392 qps | 6.2 % |
| tape1k | 9,149 qps | 8,985 qps | 8,673 qps | 5.5 % |
| tape10k | 2,113 qps | 2,130 qps | 2,060 qps | 3.4 % |
| pk | 15,649 qps | 15,699 qps | 14,728 qps | 6.6 % |
| close | 1,183 qps | 1,189 qps | 1,168 qps | 1.7 % |

Reading it. The hot-equality shapes (`board`, `tape10k`) are where the
Cabin arms show: KDS `on` is 1.20x and 1.30x PostgreSQL's seqscan, `declared`
1.52x and 1.72x, and both are above the 1.15x floor. On `tape1k` the `on` arm is
0.82x and `declared` 0.91x, and on `tape200` 0.69x and 0.70x: at small
relations PostgreSQL's seqscan over 200 rows is already a round trip (79
µs mean), and KDS's fixed extra is 11 to 17 µs here (`pk` 0.79x to 0.85x:
KDS 75.1 to 81.2 µs mean against PostgreSQL's 63.9); `tape200`'s 34 to 48 µs
gap is more than that offset. `open` (240 durable inserts, one fsync each) is 0.75x to 0.84x
and `close` (`COUNT`/`SUM`/`GROUP BY` full scans of the board, three
rounds) is 0.49x to 0.51x in every arm, the same 2x per-row cost.

**Not measured**: PostgreSQL with a btree index on `symbol`, the fair
counterpart of a *declared* Cabin. This twin never builds one (its
docstring: the point is that nothing at defaults does). Scenario 3 shows an
indexed PostgreSQL equality at 10,000 rows at about 11,000 to 12,000 qps on
a different table with 5 matches per key; this board's hot symbol matches
tens of rows (`EXPLAIN`: `rows=28`), so the number does not transfer, and
the claim above is only about PostgreSQL at defaults.

## 4. Where the time goes

The board probe has no durability, write or lock wait; it decomposes into a
fixed round trip and the walk. PostgreSQL's own `EXPLAIN (ANALYZE,
BUFFERS)`, captured by the driver in each run (archive JSON `explain`), for
the hot board probe: `Seq Scan on board_a_a ... rows=28 loops=1`,
`Rows Removed by Filter: 10212`, `Buffers: shared hit=76`, **Execution Time
0.409 ms**, Planning Time 0.013 ms; for `tape_10k` the same plan at 0.409
ms. The probe's mean over the wire is 502 µs, so about **82 % is the
server's scan and about 90 µs (18 %) is client, socket and parse**;
EXPLAIN ANALYZE's own timing overhead is inside the 0.409, so the split is
an estimate. The pk lookup, with no scan, costs 64 µs mean, which bounds the
fixed part. All buffers are `shared hit`: the 10,000-row board is in
`shared_buffers`, so there is no read wait either.

For `open`, the write shape, a PostgreSQL insert with `synchronous_commit =
on` is p50 1,169.4 µs (run 1) against 1,328.2 µs at KDS `off`; the commit's
flush is the bulk of both, the same 1.0 to 1.4 ms flush scenario 2 isolates.

## 5. Percentiles

PostgreSQL, all three runs (`ops` is the phase's count; `[pg]` is the twin's
phase suffix):

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `s4-pg-r1` | load[pg] | 31,200 ops | 45.9 µs | 47.8 µs | 48.3 µs | 66.1 µs | 79.7 µs | 340.9 µs | 0 errors |
| `s4-pg-r1` | d1-open[pg] | 240 ops | 1,027.6 µs | 1,125.8 µs | 1,169.4 µs | 1,471.3 µs | 5,338.4 µs | 11,830.2 µs | 0 errors |
| `s4-pg-r1` | d1-board[pg] | 2,400 ops | 467.5 µs | 491.6 µs | 499.5 µs | 527.2 µs | 573.0 µs | 907.8 µs | 0 errors |
| `s4-pg-r1` | d1-tape200[pg] | 396 ops | 71.8 µs | 73.1 µs | 73.8 µs | 88.0 µs | 94.2 µs | 102.6 µs | 0 errors |
| `s4-pg-r1` | d1-tape1k[pg] | 396 ops | 103.9 µs | 105.4 µs | 106.2 µs | 123.1 µs | 133.9 µs | 135.5 µs | 0 errors |
| `s4-pg-r1` | d1-tape10k[pg] | 396 ops | 456.3 µs | 463.7 µs | 471.1 µs | 500.8 µs | 530.3 µs | 615.9 µs | 0 errors |
| `s4-pg-r1` | d1-pk[pg] | 240 ops | 58.3 µs | 59.7 µs | 60.6 µs | 82.2 µs | 89.6 µs | 163.2 µs | 0 errors |
| `s4-pg-r1` | d1-close[pg] | 12 ops | 357.5 µs | 367.1 µs | 451.3 µs | 2,309.0 µs | 2,309.0 µs | 2,309.0 µs | 0 errors |
| `s4-pg-r2` | load[pg] | 31,200 ops | 46.4 µs | 48.3 µs | 48.8 µs | 63.1 µs | 77.6 µs | 1,971.5 µs | 0 errors |
| `s4-pg-r2` | d1-open[pg] | 240 ops | 946.9 µs | 1,023.6 µs | 1,062.7 µs | 1,805.0 µs | 3,591.3 µs | 6,755.5 µs | 0 errors |
| `s4-pg-r2` | d1-board[pg] | 2,400 ops | 463.6 µs | 487.9 µs | 496.6 µs | 523.9 µs | 549.5 µs | 861.3 µs | 0 errors |
| `s4-pg-r2` | d1-tape200[pg] | 396 ops | 72.0 µs | 73.6 µs | 74.3 µs | 99.8 µs | 117.2 µs | 122.9 µs | 0 errors |
| `s4-pg-r2` | d1-tape1k[pg] | 396 ops | 104.5 µs | 105.7 µs | 106.5 µs | 132.8 µs | 152.8 µs | 160.4 µs | 0 errors |
| `s4-pg-r2` | d1-tape10k[pg] | 396 ops | 453.5 µs | 460.5 µs | 466.7 µs | 489.8 µs | 539.4 µs | 613.4 µs | 0 errors |
| `s4-pg-r2` | d1-pk[pg] | 240 ops | 58.0 µs | 59.9 µs | 60.7 µs | 82.9 µs | 94.0 µs | 159.2 µs | 0 errors |
| `s4-pg-r2` | d1-close[pg] | 12 ops | 360.0 µs | 368.4 µs | 457.4 µs | 2,283.0 µs | 2,283.0 µs | 2,283.0 µs | 0 errors |
| `s4-pg-r3` | load[pg] | 31,200 ops | 46.2 µs | 47.6 µs | 48.1 µs | 67.4 µs | 80.6 µs | 321.2 µs | 0 errors |
| `s4-pg-r3` | d1-open[pg] | 240 ops | 931.9 µs | 1,004.1 µs | 1,044.7 µs | 1,854.9 µs | 5,169.8 µs | 6,334.6 µs | 0 errors |
| `s4-pg-r3` | d1-board[pg] | 2,400 ops | 466.0 µs | 497.9 µs | 512.5 µs | 539.0 µs | 558.8 µs | 932.9 µs | 0 errors |
| `s4-pg-r3` | d1-tape200[pg] | 396 ops | 72.9 µs | 74.4 µs | 75.6 µs | 100.7 µs | 115.4 µs | 125.9 µs | 0 errors |
| `s4-pg-r3` | d1-tape1k[pg] | 396 ops | 104.8 µs | 106.8 µs | 109.4 µs | 137.5 µs | 152.2 µs | 168.0 µs | 0 errors |
| `s4-pg-r3` | d1-tape10k[pg] | 396 ops | 460.6 µs | 471.0 µs | 483.9 µs | 515.3 µs | 557.8 µs | 644.3 µs | 0 errors |
| `s4-pg-r3` | d1-pk[pg] | 240 ops | 58.4 µs | 60.3 µs | 61.7 µs | 90.0 µs | 107.0 µs | 174.8 µs | 0 errors |
| `s4-pg-r3` | d1-close[pg] | 12 ops | 364.2 µs | 375.0 µs | 463.4 µs | 2,305.0 µs | 2,305.0 µs | 2,305.0 µs | 0 errors |

KDS run A day 1, from the 9a0525d archive (`json/s4.json`), for reading
beside the table above:

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| KDS `off` | d1-open[off] | 240 ops | 1,094.9 µs | 1,247.2 µs | 1,328.2 µs | 2,529.9 µs | 6,035.6 µs | 11,831.9 µs | 0 errors |
| KDS `on` | d1-open[on] | 240 ops | 1,063.7 µs | 1,285.9 µs | 1,353.1 µs | 1,653.6 µs | 4,560.2 µs | 8,426.4 µs | 0 errors |
| KDS `declared` | d1-open[declared] | 240 ops | 1,128.0 µs | 1,261.8 µs | 1,335.2 µs | 2,737.7 µs | 9,220.7 µs | 10,052.6 µs | 0 errors |
| KDS `off` | d1-board[off] | 2,400 ops | 949.0 µs | 1,019.7 µs | 1,040.4 µs | 1,104.5 µs | 1,138.7 µs | 1,432.0 µs | 0 errors |
| KDS `on` | d1-board[on] | 2,400 ops | 103.6 µs | 169.7 µs | 187.4 µs | 1,130.3 µs | 1,175.0 µs | 1,363.1 µs | 0 errors |
| KDS `declared` | d1-board[declared] | 2,400 ops | 102.6 µs | 168.6 µs | 182.6 µs | 1,060.5 µs | 1,102.2 µs | 1,270.4 µs | 0 errors |
| KDS `off` | d1-tape200[off] | 396 ops | 113.0 µs | 115.7 µs | 119.0 µs | 158.2 µs | 181.6 µs | 205.8 µs | 0 errors |
| KDS `on` | d1-tape200[on] | 396 ops | 99.2 µs | 101.5 µs | 106.5 µs | 155.2 µs | 178.1 µs | 198.4 µs | 0 errors |
| KDS `declared` | d1-tape200[declared] | 396 ops | 99.5 µs | 102.5 µs | 106.1 µs | 139.2 µs | 160.8 µs | 187.9 µs | 0 errors |
| KDS `off` | d1-tape1k[off] | 396 ops | 178.8 µs | 183.6 µs | 187.0 µs | 224.4 µs | 242.2 µs | 270.2 µs | 0 errors |
| KDS `on` | d1-tape1k[on] | 396 ops | 98.7 µs | 101.8 µs | 111.0 µs | 226.0 µs | 258.1 µs | 275.0 µs | 0 errors |
| KDS `declared` | d1-tape1k[declared] | 396 ops | 99.6 µs | 102.6 µs | 106.7 µs | 202.1 µs | 232.3 µs | 267.8 µs | 0 errors |
| KDS `off` | d1-tape10k[off] | 396 ops | 927.0 µs | 955.0 µs | 974.1 µs | 1,037.6 µs | 1,089.1 µs | 1,117.1 µs | 0 errors |
| KDS `on` | d1-tape10k[on] | 396 ops | 99.0 µs | 103.0 µs | 112.9 µs | 1,107.9 µs | 1,166.2 µs | 1,183.0 µs | 0 errors |
| KDS `declared` | d1-tape10k[declared] | 396 ops | 100.1 µs | 103.3 µs | 111.6 µs | 1,084.9 µs | 1,193.6 µs | 1,538.3 µs | 0 errors |
| KDS `off` | d1-pk[off] | 240 ops | 66.1 µs | 68.3 µs | 71.8 µs | 107.3 µs | 131.9 µs | 147.8 µs | 0 errors |
| KDS `on` | d1-pk[on] | 240 ops | 65.0 µs | 68.0 µs | 69.7 µs | 104.1 µs | 122.9 µs | 131.6 µs | 0 errors |
| KDS `declared` | d1-pk[declared] | 240 ops | 66.3 µs | 68.5 µs | 69.7 µs | 98.4 µs | 123.0 µs | 124.0 µs | 0 errors |
| KDS `off` | d1-close[off] | 12 ops | 841.5 µs | 860.7 µs | 897.6 µs | 4,189.2 µs | 4,189.2 µs | 4,189.2 µs | 0 errors |
| KDS `on` | d1-close[on] | 12 ops | 921.6 µs | 929.9 µs | 983.3 µs | 4,184.9 µs | 4,184.9 µs | 4,184.9 µs | 0 errors |
| KDS `declared` | d1-close[declared] | 12 ops | 837.6 µs | 880.6 µs | 943.8 µs | 4,097.5 µs | 4,097.5 µs | 4,097.5 µs | 0 errors |

## 6. Correctness, errors, host

| cell | client errors | driver exit | elapsed |
|---|---|---|---|
| `s4-pg-r1` | 0 errors | exit 0 | 3.7 s |
| `s4-pg-r2` | 0 errors | exit 0 | 3.6 s |
| `s4-pg-r3` | 0 errors | exit 0 | 3.6 s |

| cell | precheck UTC | loadavg 1/5/15 | cc1plus / cmake --build / ctest |
|---|---|---|---|
| `s4-pg-r1` | 2026-09-30T08:28:18Z | 1.07 / 1.28 / 1.63 | none |
| `s4-pg-r2` | 2026-09-30T08:37:17Z | 1.28 / 2.76 / 2.56 | none |
| `s4-pg-r3` | 2026-09-30T08:39:16Z | 1.51 / 2.32 / 2.42 | none |

Zero client errors in all three runs, exit 0. This twin has no result-equality
check across arms (the KDS driver compares answers across its three
arms); it runs the identical statements on identical rows, and the row
counts the `EXPLAIN` shows (28 for the board's hot symbol) are the driver's
only evidence of what a probe returns.

## 7. What the run says about the engine

1. **A Cabin is what lifts KDS over PostgreSQL's seqscan on a repeated
   hot equality**: 1.2x to 1.3x from the controller with no declaration,
   1.5x to 1.7x when declared, on the two relations big enough to matter (10,000
   rows). The controller's share of the declared arm is the 9a0525d
   file's finding and it is intact against this floor: the `on` arm
   reaches 79 % (board) and 75 % (tape10k) of the declared arm's rate.
2. **Without a Cabin, the KDS walk is half PostgreSQL's speed** (`off`
   0.48x on the board and `tape10k`, 0.50x on `close`), 2x in every
   measurement of this series and the same ratio as scenario 3's 70 against
   34 ns per row (here about 94 against 43 ns per board row: the board
   mean less the `pk` mean, over the 10,240 rows the `EXPLAIN` filters). It is what a Cabin or an index avoids, and the reason the
   optimizer's value shows most on scans this engine does slowly.
3. **The `on` arm's tail is a real cost of being automatic**: `d1-tape10k[on]`
   p95 is 1,107.9 µs (a walk) against a p50 of 112.9 µs, i.e. about 5 % or more of
   probes still walk before the Cabin covers their value, where PostgreSQL's
   tail is flat (p95 527 µs on the board against p50 500 µs) because every
   probe is the same scan. A latency-SLO reading of this scenario favours
   PostgreSQL's boring plan until the controller is warm.
4. **Nothing reached a limit.** This is one serial client for 3.6 s per run.
   The per-day KDS behaviour that is the scenario's subject (rotation,
   decay, drop and re-nomination) has no PostgreSQL counterpart and none
   is claimed.

Archive: `bench/v3.0.0/archive/scenario4-pg18-v2.7.0-545-gf2f1ee7/`.
