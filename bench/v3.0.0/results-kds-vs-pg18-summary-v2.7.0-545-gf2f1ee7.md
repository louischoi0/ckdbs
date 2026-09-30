# KDS against the PostgreSQL 18.6 floor, at `v2.7.0-545-gf2f1ee7`: summary

**Finding.** Against PostgreSQL 18.6 at its defaults, KDS is **in the same
league and not ahead**: at the same durability point (`group`, both durable at
the reply) it is 0.88x to 0.93x of PostgreSQL on four-statement autocommit
traffic (inside PostgreSQL's 19 % run-to-run spread) and 0.96x on the matched
single-client freight booking; with indexed equalities it is 0.95x to 0.97x
over twelve read shapes. It is **behind by a factor of two** wherever it walks
rows (70 ns per row against 34), **0.24x to 0.47x under `strict`** with
concurrent committers (one fsync per commit against PostgreSQL's 3.9 commits
per fsync), and **ahead only where a Cabin or a join path applies**: the
controller arm 1.20x to 1.30x and a declared Cabin 1.52x to 1.72x of
PostgreSQL's seqscan on a hot equality, and 1.24x to 2.06x on the correlated
`EXISTS` (and 2.50x on the join without a literal, indexed, at 10,000 rows). PostgreSQL is the floor: it says the
engine is in the right league and nothing finer. Every KDS number here is
the 9a0525d measurement (`v2.7.0-531-g9a0525d`, which `src/`, `include/`,
`CMakeLists.txt` and the drivers equal at HEAD) except the scenario 2
`--bookers 1` cells, measured in this run on the same binary
(sha256 `1d07d33386541ec3a13090d37918e229957e29c2c777ff15f2a63db6957f8746`).

Files, all under `bench/v3.0.0/`: `results-scenario0-stockmarket-pg18-v2.7.0-545-gf2f1ee7.md`,
`results-scenario1-backtest-pg18-...`, `results-scenario2-freight-pg18-...`,
`results-scenario3-library-pg18-...`, `results-scenario4-cabinopt-days-pg18-...`,
raw output under `archive/scenario<N>-pg18-v2.7.0-545-gf2f1ee7/`. KDS
comparators: the `-v2.7.0-531-g9a0525d` files of the same scenarios.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-09-30, PostgreSQL cells 08:23 to 08:41 UTC (re-runs to 08:39), KDS `--bookers 1` cells 08:23 to 08:27 |
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
| What "durable" means on each side | KDS `strict` and `group` both acknowledge after the commit record is flushed and device-synced (`docs/spec/wal.md` §1: "`D1/D2` differ only in batching, never in the durability point"); PostgreSQL's default `synchronous_commit = on` does the same. **PostgreSQL is therefore the comparator for both KDS modes**, and `synchronous_commit = off` (non-durable) is reported in the per-scenario files only as a labelled control and appears in no like-for-like table below |
| Concurrency mapping | KDS's `cores` is a server knob with no PostgreSQL equivalent (one backend per connection over all eight CPUs). A PG cell corresponds to a KDS cell **by client concurrency** and is compared against both `c1` and `c8`; scenario 2 is the exception, section 3 |
| Caveats | **Client CPU is shared with the servers on 8 CPUs** in every cell (KDS `c8` puts eight reactors, eight client processes and a reporter on them; PostgreSQL eight backends likewise), so neither engine's eight-way figure separates the engine from the clients' CPU. Each KDS figure is a median of three 9a0525d runs and each PostgreSQL figure a median of three runs here, with the spreads in the tables; PostgreSQL's spread is up to 19 % on scenario 0 and up to 39 % in single small scenario 3 cells (the host's slow mode), so ratios inside about 0.90x to 1.10x are not read as findings |

## 2. Scenario 0, stock market (8 traders, four autocommit statements per transaction)

Median of three, tps. KDS `c1`/`c8` are `cores` 1 and 8. Ratio is KDS over
PostgreSQL `on`. Not like-for-like and excluded: PostgreSQL
`synchronous_commit = off`, 15,848.9 tps (non-durable).

| engine / cell | durability | tps median | spread | KDS / PG-on |
|---|---|---|---|---|
| PostgreSQL 18.6, 8 traders | `synchronous_commit = on` | **710.8 tps** | 19.0 % | 1.00x (reference for this table) |
| KDS `s0-c1-g` (cores 1, group) | group | 626.0 tps | 3.8 % | 0.88x |
| KDS `s0-c8-g` (cores 8, group) | group | 661.0 tps | 6.8 % | 0.93x |
| KDS `s0-c1-s` (cores 1, strict) | strict | 169.4 tps | 6.1 % | 0.24x |
| KDS `s0-c8-s` (cores 8, strict) | strict | 331.7 tps | 20.0 % | 0.47x |

`group` is the same speed as PostgreSQL within its 19.0 % spread (0.88x and
0.93x); `strict` is 0.24x and 0.47x, and no floor explains it: PostgreSQL's
lowest run (698.6 tps) is 2.1x KDS's best `strict` median. An instrumented PostgreSQL
cell counted 3.9 commits per WAL fsync (21,258 commits, 5,441 fsyncs).
KDS `strict`'s throughput is what one fsync per commit costs on this
device; its fsync count was not measured.

## 3. Scenario 2, freight booking

The PostgreSQL twin drives **one** booking connection; the 9a0525d KDS cells
drive eight contended bookers. The like-for-like pair is therefore KDS
`--bookers 1`, measured in this run (the one place KDS was re-run).

| KDS cell | KDS median | PG-on median | KDS / PG-on | like-for-like? |
|---|---|---|---|---|
| `--bookers 1` cores 1 group | 378.2 tps | 392.2 tps | 0.96x | yes: 1 client, durable, same work |
| `--bookers 1` cores 1 strict | 377.7 tps | 392.2 tps | 0.96x | yes: 1 client, durable, same work |
| `s2-c1-g` 8 bookers | 537.6 tps | 392.2 tps | 1.37x | no: 8 clients vs 1 |
| `s2-c8-g` 8 bookers | 1,519.0 tps | 392.2 tps | 3.87x | no: 8 clients vs 1 |
| `s2-c1-s` 8 bookers | 482.5 tps | 392.2 tps | 1.23x | no: 8 clients vs 1 |
| `s2-c8-s` 8 bookers | 864.2 tps | 392.2 tps | 2.20x | no: 8 clients vs 1 |

Matched, one client, durable: **0.96x in both modes**, with the commit fsync
57 to 62 % of every booking on both engines. KDS's write statements are
0.95x to 1.19x of PostgreSQL's rate, its pk reads 0.84x to 0.87x, and its
two non-pk reads (which PostgreSQL serves from an index the twin creates)
0.47x to 0.48x and 0.28x to 0.29x. The 1.23x to 3.87x rows compare eight clients with one and
say how KDS scales (2.83x from one core to eight under `group`), not how it
compares with PostgreSQL. How PostgreSQL behaves under eight contended
bookers was not measured; the twin has no such mode.

## 4. Scenario 3, library reads (one serial connection)

Geometric mean over the twelve shapes of the KDS/PostgreSQL median-qps ratio,
per cell, with the number of shapes where KDS is ahead by more than 1.05x. The
full per-shape matrices are in the scenario 3 file. Durability does not enter a read shape (the load is the only write path, durable on both engines), so there is one row per `cores`.

| KDS cores | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| 1 | 0.87x (1 of 12 ahead) | 0.96x (3 of 12 ahead) | 0.77x (1 of 12 ahead) | 0.97x (4 of 12 ahead) | 0.62x (1 of 12 ahead) | 0.95x (4 of 12 ahead) |
| 8 | 0.82x (1 of 12 ahead) | 0.92x (2 of 12 ahead) | 0.72x (1 of 12 ahead) | 0.93x (3 of 12 ahead) | 0.60x (1 of 12 ahead) | 0.91x (4 of 12 ahead) |

Indexed, KDS is at 0.91x to 0.97x; unindexed it is at 0.60x to 0.87x and the
gap widens with rows, because an unindexed KDS equality costs 63 to 84 µs
fixed plus about 70 ns per row of the scanned relation against
PostgreSQL's 64 to 68 µs plus 34 ns (1.96x to 2.24x on five shapes). KDS
gains 8.1x to 10.8x from the index at 10,000 loans and PostgreSQL 3.9x to
5.1x, and they land in the same place (`loans-by-user` 10,881 and 10,917
qps). KDS is ahead on `exists-correlated` in every cell (1.24x to 2.06x at
`c1`) and on `join-no-literal` at 10,000 with indexes (2.50x).

## 5. Scenario 4, hot equality on a 10,000-row board (day 1)

PostgreSQL has no index and no Cabin; the KDS arms are the 9a0525d run A day
1. Derived qps, ratio KDS over PostgreSQL. The KDS arms ran `cores = 1`, `group` only (no `c8` or `strict` cell exists for this driver); the PostgreSQL cell is `on`:

| group | PG (no index) | KDS off (walks) | KDS on (controller) | KDS declared (Cabin) | KDS off / PG | KDS on / PG | KDS declared / PG |
|---|---|---|---|---|---|---|---|
| open | 823 qps | 648 qps | 691 qps | 616 qps | 0.79x | 0.84x | 0.75x |
| board | 1,994 qps | 960 qps | 2,403 qps | 3,039 qps | 0.48x | 1.20x | 1.52x |
| tape200 | 12,690 qps | 7,868 qps | 8,696 qps | 8,905 qps | 0.62x | 0.69x | 0.70x |
| tape1k | 8,985 qps | 5,128 qps | 7,391 qps | 8,197 qps | 0.57x | 0.82x | 0.91x |
| tape10k | 2,113 qps | 1,024 qps | 2,738 qps | 3,636 qps | 0.48x | 1.30x | 1.72x |
| pk | 15,649 qps | 12,315 qps | 12,870 qps | 13,316 qps | 0.79x | 0.82x | 0.85x |
| close | 1,183 qps | 596 qps | 577 qps | 598 qps | 0.50x | 0.49x | 0.51x |

KDS's `off` arm (the walk) is 0.48x to 0.62x on the equality shapes; the
controller arm is 1.20x on the board and 1.30x on `tape10k`, a declared
Cabin 1.52x and 1.72x, all above the 1.15x floor of the 9a0525d file. **Not
measured**: PostgreSQL with an index on `symbol`, so the Cabin arms are
compared against PostgreSQL's seqscan only.

## 6. Scenario 1, backtest

KDS refuses the driver (SUS-1: `daily_stats` and `model_results` are created
`HEAP`), so **there is no KDS counterpart**. PostgreSQL ran once at the
driver's defaults as a standalone floor (`results-scenario1-backtest-pg18-...`):
an unindexed non-pk equality at 414 qps that a btree index lifts 22x to 8,892
qps, a durable autocommit insert rate of 659 rows/s that 1,000-row batches
lift to 18,757, and 4.41x the join throughput at eight connections.

## 7. What it says about KDS

1. **`group` is at the floor, `strict` is not**: the `D1`/`D2` batching
   difference `docs/spec/wal.md` describes is worth 3.7x (one core) and 2.0x (eight) to KDS on
   concurrent autocommit traffic (the 9a0525d file), and zero on a single committer (scenario 2:
   378.2 and 377.7 tps).
2. **A KDS row walk costs twice a PostgreSQL row walk** (70 against 34 ns per
   row, five statements, `c1`), which is most of KDS's deficit
   unindexed and the reason its index and Cabin gains look larger than
   PostgreSQL's. It is the measured optimisation target of this series.
3. **Indexes and Cabins are what put KDS at or over the floor**: within 5 %
   with indexes, 1.2x to 1.7x over a seqscan with a Cabin.
4. **The fixed round trip is 4 to 10 µs more** than PostgreSQL's 62 to 69 µs
   mean (the `pk` controls in scenarios 2 to 4).
5. **Not measured, and the open comparisons**: PostgreSQL under eight
   contended bookers (scenario 2), PostgreSQL with an index on scenario 4's
   board, KDS's non-durable class against `synchronous_commit = off`, and
   scenario 1 on KDS at all.

## 8. Cells run and not run

| Scenario | PostgreSQL cells | Status |
|---|---|---|
| 0 | `on` x3, `off` x3 (non-durable control) | ran, all exit 0, verify clean |
| 1 | one run, driver defaults | ran (no KDS counterpart) |
| 2 | `on` x3, `off` x3, plus KDS `--bookers 1` group x3 and strict x3 | ran, 400 of 400 invariant checks in all 12 cells; PG driver needs a print-step shim (`pg_s2_shim.py`) because `--verify` crashes its printer |
| 3 | 3 sizes x {none, all} x3 = 18 | ran, all exit 0, 0 errors; no verify mode in the twin |
| 4 | day 1, x3 | ran, all exit 0 |

The KDS numbers of scenarios 0, 2 (8 bookers), 3 and 4 are not re-measured;
they are `v2.7.0-531-g9a0525d`'s.
