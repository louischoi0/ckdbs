# EF-S0: a short read through equal clients on `main`, and what each scenario client adds

**One line.** On `main` at `72580aa04`, through clients that count rows on both sides, a tuple key lookup is 20.1 µs on KDS against 22.1 µs on PostgreSQL 18.7 (1.10x, on 7.0 µs against 10.9 µs of server CPU), the four other reads that fold nothing are 1.47x to 2.01x and the grouped fold 1.20x; **the scenario drivers' KDS client adds 16.6 µs a statement and 1.4 µs to 1.7 µs a returned 8-column row, where PostgreSQL's decoding client adds 0.8 µs and 0.7 µs to 0.8 µs**, which turns the same lookup into 0.62x to 0.63x. The premise EF was asked for - a short read's fixed cost is PostgreSQL's lead - is the client's on this host, and EF-Q1's gate reads "the engine stage does not open" on this evidence, which is `pk`'s alone: the 200-row tape the gate also names was not run. One shape is behind on equal clients and it is EH's: a global fold at 0.71x on 1.40x the server CPU.

Measured by CLA in the session that opened EF (no `ck-tester` agent was spawned). Driver: `tools/parity_bench.py` as this stage commits it - `--kds-client` and `--pg-client` added, and a containerised backend's host pid resolved for its `schedstat`; passes A and B ran before `--pg-client` was added, which changes neither's timed path.

## Stamp

| Field | Value |
|---|---|
| Version, commit | `v3.5.1-6-g72580aa04` (`git describe --tags` at `72580aa04`), built from `git archive 72580aa04` outside every worktree, Release, target `kds_server`; the measured copy's sha256 is in the archive (`binary.sha256`) |
| Date and time | 2026-11-28; passes A and B 01:47:11 to 01:47:30 UTC, pass C 01:48:19 to 01:48:30 UTC. **About 30 s of measurement**, inside the 20-minute stage budget; the build took 32 s |
| KDS | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, port 15791 (chosen), a fresh data file, `taskset -c 0-7` |
| PostgreSQL | 18.7 (Debian 18.7-1.pgdg13+5), a fresh `docker.io/library/postgres:18` container at its defaults (port 15434 and the listen address only), host network, every server process on CPUs 0 to 7; `max_parallel_workers_per_gather = 0` for the session, as the driver sets it |
| Driver | `taskset -c 8-23`; each rep runs every shape on both engines, the engine order alternating by rep; a warm-up of a tenth of the ops before each cell; both engines must return the same row counts or the run stops |
| Data | `pb_bars`, 60,480 rows of 8 columns (seven `int64`, one `int32`), `pb_symbols` 8 rows, `pb_exchanges` 2 rows; no secondary index on either engine |
| Host and device | Intel Core Ultra 7 270K Plus, 24 CPUs (P-cores 0 to 7), `powersave`; data on `/dev/mapper/fedora-root`, xfs (`df -T` at run time, `device.txt`) |
| Load | `/proc/loadavg` 1.76 before pass A and 2.39 before pass C, no build or test process at either (`precheck.txt`, `precheck2.txt`); **no quiet gate was applied**, and another project's containers and `kds_server` run on this host throughout |
| Rule | spread = (max - min) / median over a cell's reps; a difference smaller than the larger spread is inside the spread; a spread over 20 % carries no claim. Server CPU is `schedstat` on-CPU time: every KDS server thread less its idle tick, and PostgreSQL's one backend |
| Comparator | this engine's own last number for the shapes: `bench/v3.2.0/results-pg-parity-v3.2.0-171-ge81ac7cd.md`, **another host**, so no delta is taken; its ratios are set beside this run's in "Against `e81ac7cd`" |
| Suite | not executed by this measurement |

## The three passes

### Pass A: equal clients (`kwp.Connection.query` against `pg_wire` `query`), 5 reps

| shape | rows a statement | KDS rate (spread) | PostgreSQL rate (spread) | KDS / PG | read | KDS server CPU | PG backend CPU | KDS client CPU | PG client CPU |
|---|---|---|---|---|---|---|---|---|---|
| `pk-lookup` | 1 rows | 49,435.5 qps (7.7 %) | 44,998.4 qps (6.7 %) | **1.10x** | KDS ahead | 7.0 µs | 10.9 µs | 8.8 µs | 7.8 µs |
| `bar-range` | 201 rows | 4,986.5 qps (9.7 %) | 2,869.8 qps (11.2 %) | **1.74x** | KDS ahead | 43.1 µs | 80.1 µs | 141.5 µs | 229.0 µs |
| `symbol-history` | 7,560 rows | 168.2 qps (2.8 %) | 112.2 qps (4.3 %) | **1.50x** | KDS ahead | 1,719.9 µs | 2,038.4 µs | 4,883.6 µs | 8,187.1 µs |
| `day-slice` | 8 rows | 1,500.2 qps (10.7 %) | 1,021.4 qps (4.3 %) | **1.47x** | KDS ahead | 593.9 µs | 910.9 µs | 53.2 µs | 63.2 µs |
| `point-join` | 1 rows | 42,513.5 qps (3.4 %) | 21,136.5 qps (5.1 %) | **2.01x** | KDS ahead | 11.2 µs | 35.9 µs | 8.5 µs | 7.7 µs |
| `agg-global` | 1 rows | 405.3 qps (5.0 %) | 567.3 qps (8.7 %) | **0.71x** | PostgreSQL ahead | 2,386.2 µs | 1,699.2 µs | 30.2 µs | 21.5 µs |
| `agg-by-session` | 7,560 rows | 132.8 qps (3.2 %) | 110.6 qps (1.5 %) | **1.20x** | KDS ahead | 5,471.1 µs | 4,496.7 µs | 1,834.5 µs | 5,164.6 µs |

| shape | engine | ops a rep | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| `pk-lookup` | KDS | 2,000 ops | 20.1 µs | 17.5 µs | 18.6 µs | 19.2 µs | 21.3 µs | 34.7 µs |
| `pk-lookup` | PostgreSQL | 2,000 ops | 22.1 µs | 19.7 µs | 21.1 µs | 21.3 µs | 24.6 µs | 27.4 µs |
| `bar-range` | KDS | 300 ops | 200.4 µs | 169.0 µs | 181.4 µs | 187.1 µs | 317.4 µs | 364.8 µs |
| `bar-range` | PostgreSQL | 300 ops | 348.3 µs | 259.7 µs | 267.9 µs | 286.2 µs | 457.8 µs | 525.9 µs |
| `symbol-history` | KDS | 10 ops | 5,946.5 µs | 5,474.2 µs | 5,698.2 µs | 5,980.6 µs | 6,401.7 µs | 6,401.7 µs |
| `symbol-history` | PostgreSQL | 10 ops | 8,913.4 µs | 8,536.2 µs | 8,673.5 µs | 9,004.4 µs | 9,380.1 µs | 9,380.1 µs |
| `day-slice` | KDS | 200 ops | 666.2 µs | 610.4 µs | 629.2 µs | 653.8 µs | 689.7 µs | 740.7 µs |
| `day-slice` | PostgreSQL | 200 ops | 978.6 µs | 905.8 µs | 955.2 µs | 978.6 µs | 1,033.4 µs | 1,181.0 µs |
| `point-join` | KDS | 2,000 ops | 23.4 µs | 21.5 µs | 22.9 µs | 23.1 µs | 24.5 µs | 28.4 µs |
| `point-join` | PostgreSQL | 2,000 ops | 47.2 µs | 43.9 µs | 45.5 µs | 46.1 µs | 47.7 µs | 82.4 µs |
| `agg-global` | KDS | 20 ops | 2,467.2 µs | 2,399.7 µs | 2,424.4 µs | 2,450.5 µs | 2,661.6 µs | 2,661.6 µs |
| `agg-global` | PostgreSQL | 20 ops | 1,762.3 µs | 1,679.1 µs | 1,705.3 µs | 1,713.9 µs | 1,986.7 µs | 1,986.7 µs |
| `agg-by-session` | KDS | 10 ops | 7,530.6 µs | 7,427.5 µs | 7,473.6 µs | 7,526.5 µs | 7,790.6 µs | 7,790.6 µs |
| `agg-by-session` | PostgreSQL | 10 ops | 9,041.5 µs | 8,849.3 µs | 8,934.4 µs | 9,050.1 µs | 9,258.9 µs | 9,258.9 µs |

KDS idle tick in this pass: 12,855 µs of server CPU a second, subtracted from KDS's server CPU.

### Pass B: KDS through `send_command`, PostgreSQL through `query`, 3 reps (scenario 4's pair)

| shape | rows a statement | KDS rate (spread) | PostgreSQL rate (spread) | KDS / PG | read | KDS server CPU | PG backend CPU | KDS client CPU | PG client CPU |
|---|---|---|---|---|---|---|---|---|---|
| `pk-lookup` | 1 rows | 27,269.7 qps (1.2 %) | 43,904.5 qps (8.0 %) | **0.62x** | PostgreSQL ahead | 6.4 µs | 11.0 µs | 26.4 µs | 7.9 µs |
| `bar-range` | 201 rows | 1,796.4 qps (4.0 %) | 3,022.5 qps (13.3 %) | **0.59x** | PostgreSQL ahead | 61.7 µs | 75.1 µs | 413.2 µs | 226.7 µs |
| `symbol-history` | 7,560 rows | 60.8 qps (1.1 %) | 113.6 qps (2.9 %) | **0.53x** | PostgreSQL ahead | 2,331.8 µs | 2,048.7 µs | 14,926.9 µs | 8,096.8 µs |
| `agg-by-session` | 7,560 rows | 80.3 qps (1.6 %) | 111.2 qps (1.7 %) | **0.72x** | PostgreSQL ahead | 5,435.3 µs | 4,646.8 µs | 6,622.2 µs | 5,012.8 µs |

| shape | engine | ops a rep | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| `pk-lookup` | KDS | 2,000 ops | 36.5 µs | 34.7 µs | 36.0 µs | 36.5 µs | 37.8 µs | 39.0 µs |
| `pk-lookup` | PostgreSQL | 2,000 ops | 22.6 µs | 19.4 µs | 20.8 µs | 21.0 µs | 25.2 µs | 29.9 µs |
| `bar-range` | KDS | 300 ops | 556.5 µs | 450.7 µs | 465.4 µs | 574.6 µs | 665.0 µs | 776.3 µs |
| `bar-range` | PostgreSQL | 300 ops | 330.7 µs | 257.8 µs | 265.8 µs | 272.6 µs | 453.4 µs | 512.2 µs |
| `symbol-history` | KDS | 10 ops | 16,458.3 µs | 15,821.9 µs | 16,041.0 µs | 16,416.3 µs | 17,713.4 µs | 17,713.4 µs |
| `symbol-history` | PostgreSQL | 10 ops | 8,804.7 µs | 8,273.5 µs | 8,754.7 µs | 8,865.8 µs | 9,255.7 µs | 9,255.7 µs |
| `agg-by-session` | KDS | 10 ops | 12,451.7 µs | 12,224.4 µs | 12,276.2 µs | 12,414.5 µs | 12,638.8 µs | 12,638.8 µs |
| `agg-by-session` | PostgreSQL | 10 ops | 8,989.6 µs | 8,799.8 µs | 8,919.8 µs | 9,007.8 µs | 9,076.8 µs | 9,076.8 µs |

KDS idle tick in this pass: 14,387 µs of server CPU a second, subtracted from KDS's server CPU.

### Pass C: KDS through `send_command`, PostgreSQL through `fetch` and decode, 3 reps (scenario 2's and 3's pair)

| shape | rows a statement | KDS rate (spread) | PostgreSQL rate (spread) | KDS / PG | read | KDS server CPU | PG backend CPU | KDS client CPU | PG client CPU |
|---|---|---|---|---|---|---|---|---|---|
| `pk-lookup` | 1 rows | 26,939.5 qps (1.1 %) | 42,931.8 qps (1.7 %) | **0.63x** | PostgreSQL ahead | 6.6 µs | 11.2 µs | 26.1 µs | 8.6 µs |
| `bar-range` | 201 rows | 1,864.3 qps (2.3 %) | 2,011.7 qps (7.4 %) | **0.93x** | inside the spread | 60.8 µs | 92.7 µs | 406.6 µs | 342.5 µs |
| `symbol-history` | 7,560 rows | 61.6 qps (1.9 %) | 71.6 qps (1.4 %) | **0.86x** | PostgreSQL ahead | 2,192.2 µs | 2,298.6 µs | 14,825.8 µs | 13,130.9 µs |
| `day-slice` | 8 rows | 1,475.2 qps (1.7 %) | 980.7 qps (5.9 %) | **1.50x** | KDS ahead | 590.4 µs | 897.0 µs | 70.7 µs | 92.4 µs |
| `agg-by-session` | 7,560 rows | 79.5 qps (1.3 %) | 93.1 qps (2.1 %) | **0.85x** | PostgreSQL ahead | 5,498.9 µs | 4,577.8 µs | 6,624.2 µs | 6,902.1 µs |

| shape | engine | ops a rep | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| `pk-lookup` | KDS | 2,000 ops | 37.0 µs | 34.6 µs | 36.2 µs | 36.4 µs | 39.8 µs | 40.8 µs |
| `pk-lookup` | PostgreSQL | 2,000 ops | 23.2 µs | 20.5 µs | 21.9 µs | 22.1 µs | 34.1 µs | 35.8 µs |
| `bar-range` | KDS | 300 ops | 536.2 µs | 447.8 µs | 461.1 µs | 562.9 µs | 622.0 µs | 674.3 µs |
| `bar-range` | PostgreSQL | 300 ops | 497.0 µs | 369.8 µs | 397.8 µs | 511.1 µs | 632.5 µs | 707.4 µs |
| `symbol-history` | KDS | 10 ops | 16,234.8 µs | 15,792.9 µs | 15,853.8 µs | 16,110.7 µs | 17,301.0 µs | 17,301.0 µs |
| `symbol-history` | PostgreSQL | 10 ops | 13,962.8 µs | 13,035.1 µs | 13,544.1 µs | 13,881.2 µs | 15,921.8 µs | 15,921.8 µs |
| `day-slice` | KDS | 200 ops | 677.6 µs | 640.2 µs | 675.1 µs | 679.7 µs | 691.1 µs | 788.5 µs |
| `day-slice` | PostgreSQL | 200 ops | 1,019.1 µs | 906.1 µs | 964.1 µs | 981.6 µs | 1,272.0 µs | 1,370.7 µs |
| `agg-by-session` | KDS | 10 ops | 12,577.4 µs | 12,258.3 µs | 12,364.1 µs | 12,410.8 µs | 13,165.1 µs | 13,165.1 µs |
| `agg-by-session` | PostgreSQL | 10 ops | 10,743.7 µs | 10,436.9 µs | 10,557.3 µs | 10,575.3 µs | 12,530.9 µs | 12,530.9 µs |

KDS idle tick in this pass: 15,720 µs of server CPU a second, subtracted from KDS's server CPU.


## The break between the clients

"Added" is the mean through the decoding client less the mean through the counting one, per statement for the one-row shape and per row for the others (KDS: `send_command` of passes B and C against `query` of pass A; PostgreSQL: `fetch` of pass C against `query` of passes A and B).

| shape | rows | KDS `query` mean | KDS `send_command` mean | KDS added a statement | KDS added a row | PG `query` mean | PG `fetch` mean | PG added a statement | PG added a row |
|---|---|---|---|---|---|---|---|---|---|
| `pk-lookup` | 1 rows | 20.1 µs | 36.8 µs | 16.6 µs | - | 22.4 µs | 23.2 µs | 0.8 µs | - |
| `bar-range` | 201 rows | 200.4 µs | 546.4 µs | 345.9 µs | 1.72 µs | 339.5 µs | 497.0 µs | 157.4 µs | 0.78 µs |
| `symbol-history` | 7,560 rows | 5,946.5 µs | 16,346.5 µs | 10,400.1 µs | 1.38 µs | 8,859.1 µs | 13,962.8 µs | 5,103.8 µs | 0.68 µs |
| `agg-by-session` | 7,560 rows | 7,530.6 µs | 12,514.6 µs | 4,983.9 µs | 0.66 µs | 9,015.5 µs | 10,743.7 µs | 1,728.2 µs | 0.23 µs |

- **Which scenario times which pair** (read at `72580aa04`): scenario 4's twin times `pg_wire`'s `send_command`, which counts (`tools/pg_scenario4_cabinopt_days.py:56-70`, `tools/pg_wire.py:313-322`), so its cells are pass B's pair; scenario 2's twin times its reads through `rows_timed` and scenario 3's through `Client.timed`, both `fetch` and a decode of every value (`tools/pg_scenario2_freight.py:164-168`, `tools/pg_scenario3_library.py:109-121`), so their reads are pass C's pair; scenario 1's twin decodes and renders (`tools/pg_scenario1_backtest.py:266-292`). Every KDS driver times `ckdbs_cli.send_command`.
- **The statement term is the KDS client's on every pair**: 16.6 µs against 0.8 µs. It is the whole of scenario 4's `pk` gap (31.9 µs against 23.2 µs in the comparison file, 36.8 µs against 22.4 µs to 23.2 µs here) and of a booking's point reads.
- **The row term is about twice PostgreSQL's**: 1.4 µs to 1.7 µs a row against 0.7 µs to 0.8 µs for 8 columns, so a decoded pair still favours PostgreSQL on rows (`bar-range` 0.93x, `symbol-history` 0.86x in pass C) where the counting pair has KDS at 1.74x and 1.50x.
- **KDS's server CPU is higher behind `send_command` on the shapes that return many 8-column rows** (`bar-range` 43.1 µs in pass A, 61.7 µs and 60.8 µs in B and C; `symbol-history` 1,719.9 µs against 2,331.8 µs and 2,192.2 µs) and not on the lookup (7.0 µs against 6.4 µs and 6.6 µs), nor on `agg-by-session`'s 7,560 three-column rows (5,471.1 µs against 5,435.3 µs and 5,498.9 µs). Not attributed; a slower reader is the only difference between the passes.

## Against `e81ac7cd` (another host; ratios only)

| shape | KDS / PG at `e81ac7cd` | KDS / PG here | KDS / PG server CPU at `e81ac7cd` | here |
|---|---|---|---|---|
| `pk-lookup` | 1.30x | 1.10x | 0.64x | 0.65x |
| `bar-range` | 1.42x | 1.74x | 0.74x | 0.54x |
| `symbol-history` | 1.26x | 1.50x | 0.87x | 0.84x |
| `day-slice` | 1.22x | 1.47x | 0.81x | 0.65x |
| `point-join` | 2.19x | 2.01x | 0.35x | 0.31x |
| `agg-global` | 0.61x | 0.71x | 1.63x | 1.40x |
| `agg-by-session` | 1.07x | 1.20x | 1.21x | 1.22x |

The picture is the one `e81ac7cd` gave: ahead on every shape but the global fold, whose server CPU is 39.5 ns a row here against PostgreSQL's 28.1 ns (2,386.2 µs and 1,699.2 µs over 60,480 rows); `agg-by-session` is 90.5 ns against 74.4 ns a row, 1.22x, and leads on the wall clock only because KDS's rows are cheaper to receive.

## What the run teaches

1. **A scenario file's "PostgreSQL ahead" on a statement that returns rows is a statement about two Python clients until shown otherwise.** On equal clients no such shape is behind but the global fold, which returns one row and is item 3's.
2. **The lookup's server CPU is 7.0 µs against PostgreSQL's 10.9 µs.** There is no engine fixed cost to recover against PostgreSQL on this path; what §1.3 of EF's order lists is what a statement costs, not a deficit.
3. **The fold is the engine's deficit**, and it is smaller here than the 0.47x the scenario drivers read: 0.71x of the rate, 1.40x the server CPU.

## What is cut, absent or unverified

- **A booking's reads were not run** (EF-S0's plan named them): they are point reads through the same two clients as `pk-lookup`, and the scenario 2 schema was left for S2.
- **The 200-row tape was not run either** (EF-S0's plan named it, and EF-Q1's gate names it beside `pk`): scenario 4's `tape200` is a `symbol` equality over a 200-row relation, and no shape here is it - `bar-range` returns 201 rows by a tuple key range. EF-Q1 is read above on `pk` alone.
- **No quiet gate**, three to five reps, 30 s: a ratio inside 10 % of 1 is not a finding here (the lookup's 1.10x is just over its 7.7 % spread).
- **The row term for other widths** is not measured: 8 columns here, 9 in `kwp.py`'s note.
- **Why a slower reader raises KDS's server CPU** on the row-heavy shapes is not attributed.
- **PostgreSQL's server CPU is one backend's**; its background processes are not counted, and KDS's every thread is.

## Archive

`bench/v3.5.1/archive/ef-s0-v3.5.1-6-g72580aa04/`: the three passes' JSON (`equal.json`, `render.json`, `drivers.json`, every rep's cell with its percentiles, counts and host state), `run.sh`, `run2.sh`, `watchdog.sh` and its log, the server configuration, both engines' affinity, the device, the load before and after each run, the binary's hash.
