# KDS against PostgreSQL 18 on equal clients: only the whole-relation fold is behind

The v3.1.0 scenario files put KDS behind PostgreSQL on the tuple key round trip (0.83x-1.03x),
on shapes that return many rows (`bar-range` 0.49x, `symbol-history` 0.43x) and on the folds
(`agg-global` 1.48x slower, `agg-by-session` 1.54x slower). Most of that was the benchmark client,
not the engine. The scenario drivers time `ckdbs_cli.send_command`, which decodes every field and
renders it to text in Python. The PostgreSQL twins time `pg_wire` paths that decode less, or nothing.
On this host that difference is **29.0 µs against 5.0 µs of client CPU per point statement, and
6.3 µs against 2.5 µs per returned 9-column row**. A stub socket reproduces the scenario files'
"7.5 against 3.6 µs per returned row" from client CPU alone.

With both engines read through clients that count rows and build no per-field object, run
interleaved on one data set:

- **every shape listed as behind is ahead except the folds.** The tuple key lookup is 1.30x,
  `bar-range` 1.42x and `symbol-history` 1.26x of PostgreSQL's rate, and KDS's server CPU is lower
  on each (0.64x, 0.74x and 0.87x of PostgreSQL's backend).
- **`agg-global` is behind, and the cause is the engine.** It runs at 0.61x of PostgreSQL's rate and
  uses 1.63x its server CPU: 5,776 µs against 3,549 µs to fold 60,480 rows, about 95 ns per row
  against 59 ns.
- **`agg-by-session` is ahead on the wall clock only.** It runs at 1.07x, but KDS's server CPU is
  1.21x PostgreSQL's (11,020 µs against 9,120 µs). The wall-clock lead comes from the client: KWP's
  binary rows decode for 2,598 µs per statement against 7,303 µs for PostgreSQL's text rows.

## Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-18, 07:54:37 UTC precheck, run about 1 minute (98 cells) |
| Worktree / branch | `bench-client-parity` / `worktree-bench-client-parity` |
| `git describe --tags` | `v3.2.0-171-ge81ac7cd` (`describe.txt`); engine sources identical to `dd3af538` on `main` (the two commits since touch `tools/` and `bench/` only) |
| Binary | `build-release/kds_server`, Release, built in this worktree at 07:48 UTC from the engine at `dd3af538`; copy `/home/cdkbs/bench-runs/parity/kds_server`, sha256 `a56618c86e17031a3afaee79718c2766d96f4fc931314275bbb19aaac7d82025`; every server started from the copy |
| Device | `/dev/root`, ext4 (`device.txt`); data file under `/home/cdkbs/bench-runs/parity/data`, fresh |
| Host | AMD EPYC 9V74, 8 CPUs; loadavg 0.53-0.80 over all 98 cells; no `cc1plus`/`cmake --build`/`ctest` in any cell (`host` per cell in `cells.json`) |
| KDS | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`, port 15791; everything else default, **Waystone recording and replay on** |
| PostgreSQL | 18.6, `/home/cdkbs/pg-bench-ba/data`, port 15434, its defaults (`shared_buffers = 128MB`), with `max_parallel_workers_per_gather = 0` set per session; tables `VACUUM (FREEZE, ANALYZE)`d after the load |
| Driver | `tools/parity_bench.py --reps 7` via `bench/v3.2.0/archive/parity-harness/run.sh` |
| Tests | no engine change; the correctness suite was not executed for this run. Both engines returned the same row count for every statement of every cell (the driver stops otherwise) |

## Method

- Data, the same rows in both engines from seed 1:
  - `pb_bars`: 8 symbols x 7,560 sessions = 60,480 rows, 8 integer columns.
  - `pb_symbols`: 8 rows. `pb_exchanges`: 2 rows.
  - The tuple key / primary key is `id`, and there is no other index on either side.
- Clients:
  - KDS: `kwp.Connection.query` sends PARSE, BIND, EXECUTE, CLOSE and SYNC in one write, counts rows and walks field lengths.
  - PostgreSQL: `pg_wire.PgConnection.query` sends one simple `Q`, counts rows and walks DataRow lengths.
  - Neither client decodes a value.
  - Measured on stub sockets, the two clients cost **5.0 µs against 3.4 µs per statement and 1.0 µs against 1.6 µs per returned row**. The remainder is the shape of each protocol: five frames against one message, and one batch against one message per row. It is not client work one side skipped.
- Server CPU per statement:
  - KDS: the change in the sum of every server thread's `schedstat` on-CPU time over the cell.
  - PostgreSQL: the change in the backend's `schedstat`.
  - KDS's idle tick was measured once, at 6,455 µs per second of wall time. **Net** subtracts it over each cell's wall time; it moves no shape by more than 2 %.
  - PostgreSQL's background processes are not counted. A read writes no WAL.
- Order and warm-up:
  - Each rep runs every shape on both engines, KDS first on odd reps and PostgreSQL first on even reps.
  - Every cell is preceded by a warm-up of a tenth of its ops, drawn from a separate generator.
- Statistics:
  - Ratios use the median of 7 reps.
  - **Spread** is (max - min) / median over the 7 reps.
  - Every rep of every shape has the same sign as the median (per-rep column below).
- Cut:
  - No row-count sweep.
  - No Waystone-off arm (see "What this does not show").
  - One connection, a read-only workload, and no write shape. Rule 3's wait breakdown has nothing to break down here: no lock or durability wait can occur.

## Result

| shape | rows/op | KDS p50 | PG p50 | KDS/PG qps | KDS server CPU (net of idle) | PG backend CPU | KDS/PG server CPU | KDS client CPU | PG client CPU |
|---|---|---|---|---|---|---|---|---|---|
| pk-lookup | 1 rows | 48.0 µs | 62.8 µs | 1.30x | 29.6 µs (29.3 µs) | 45.9 µs | 0.64x | 19.9 µs | 18.8 µs |
| bar-range | 201 rows | 305.7 µs | 432.0 µs | 1.42x | 104.7 µs (102.7 µs) | 138.7 µs | 0.74x | 204.7 µs | 339.3 µs |
| symbol-history | 7,560 rows | 10,379.0 µs | 13,156.6 µs | 1.26x | 3,744.4 µs (3,677.6 µs) | 4,216.2 µs | 0.87x | 6,819.2 µs | 12,042.0 µs |
| day-slice | 8 rows | 1,523.5 µs | 1,862.4 µs | 1.22x | 1,503.3 µs (1,493.4 µs) | 1,833.4 µs | 0.81x | 33.6 µs | 38.0 µs |
| point-join | 1 rows | 60.6 µs | 134.2 µs | 2.19x | 42.3 µs (41.9 µs) | 119.5 µs | 0.35x | 20.1 µs | 19.0 µs |
| agg-global | 1 rows | 5,843.7 µs | 3,573.4 µs | 0.61x | 5,813.7 µs (5,775.9 µs) | 3,549.2 µs | 1.63x | 35.5 µs | 28.6 µs |
| agg-by-session | 7,560 rows | 13,699.8 µs | 14,647.6 µs | 1.07x | 11,108.1 µs (11,019.5 µs) | 9,119.5 µs | 1.21x | 2,598.2 µs | 7,302.6 µs |

Percentiles (median over reps of each cell's percentile) and spreads:

| shape | engine | p0 | p25 | p50 | p95 | p99 | max | qps spread | server CPU spread | KDS/PG qps per rep |
|---|---|---|---|---|---|---|---|---|---|---|
| pk-lookup | KDS | 45.6 µs | 47.4 µs | 48.0 µs | 62.0 µs | 73.2 µs | 118.0 µs | 11.7 % | 13.0 % | 1.38 1.31 1.32 1.28 1.29 1.28 1.33 |
| pk-lookup | PG | 60.4 µs | 62.2 µs | 62.8 µs | 78.4 µs | 91.4 µs | 120.8 µs | 9.6 % | 8.9 % | |
| bar-range | KDS | 291.0 µs | 300.4 µs | 305.7 µs | 324.8 µs | 345.0 µs | 359.6 µs | 3.6 % | 9.4 % | 1.41 1.39 1.37 1.60 1.40 1.40 1.42 |
| bar-range | PG | 412.8 µs | 425.5 µs | 432.0 µs | 457.5 µs | 476.6 µs | 502.4 µs | 13.1 % | 31.7 % | |
| symbol-history | KDS | 10,311.4 µs | 10,325.2 µs | 10,379.0 µs | 10,630.2 µs | 10,630.2 µs | 10,630.2 µs | 2.5 % | 3.7 % | 1.25 1.21 1.27 1.40 1.30 1.25 1.26 |
| symbol-history | PG | 12,148.1 µs | 12,739.1 µs | 13,156.6 µs | 13,697.4 µs | 13,697.4 µs | 13,697.4 µs | 15.7 % | 56.7 % | |
| day-slice | KDS | 1,493.8 µs | 1,508.4 µs | 1,523.5 µs | 1,577.1 µs | 1,730.1 µs | 1,904.6 µs | 2.2 % | 2.3 % | 1.21 1.22 1.23 1.23 1.21 1.22 1.21 |
| day-slice | PG | 1,843.2 µs | 1,856.1 µs | 1,862.4 µs | 1,904.6 µs | 1,991.2 µs | 2,125.0 µs | 1.4 % | 1.1 % | |
| point-join | KDS | 57.7 µs | 59.8 µs | 60.6 µs | 76.3 µs | 89.9 µs | 142.6 µs | 8.5 % | 9.2 % | 2.22 2.24 2.33 2.36 2.19 2.17 2.18 |
| point-join | PG | 130.3 µs | 132.8 µs | 134.2 µs | 159.0 µs | 168.5 µs | 233.7 µs | 13.9 % | 9.7 % | |
| agg-global | KDS | 5,736.4 µs | 5,784.6 µs | 5,843.7 µs | 6,018.1 µs | 6,018.1 µs | 6,018.1 µs | 1.7 % | 1.6 % | 0.62 0.62 0.62 0.61 0.61 0.61 0.61 |
| agg-global | PG | 3,532.8 µs | 3,555.9 µs | 3,573.4 µs | 3,627.9 µs | 3,627.9 µs | 3,627.9 µs | 1.7 % | 1.8 % | |
| agg-by-session | KDS | 13,524.8 µs | 13,597.4 µs | 13,699.8 µs | 13,950.5 µs | 13,950.5 µs | 13,950.5 µs | 2.0 % | 2.4 % | 1.08 1.09 1.07 1.06 1.07 1.07 1.06 |
| agg-by-session | PG | 14,472.6 µs | 14,552.0 µs | 14,647.6 µs | 14,814.6 µs | 14,814.6 µs | 14,814.6 µs | 1.6 % | 1.8 % | |

Against this engine's own previous number for the same shapes
(`bench/v3.1.0/results-scenario1-backtest-v3.1.0-30-gcd7c7c4e.md`, `read-bar-lookup` p50 75.1 µs,
`read-bar-range` 1,498.2 µs, `agg-global` 6,113.9 µs): **no delta is claimed.** That file timed
`send_command` and was taken on a different relation. The pk p50 here is 48.0 µs against 75.1 µs;
roughly 25 µs of that gap is the client cost this file removes, not an engine change. `agg-global`
(5,844 µs against 6,114 µs) returns one row and so carries no client artifact. It is within about
5 % of the earlier figure, consistent with an unchanged fold.

## What the run teaches about the engine

- **On a statement that returns rows, the client decides the scenario files' PG ratio more than the
  engine does.** A Python client that renders every KWP field to text pays 3.8 µs more per 9-column
  row than one that reads PostgreSQL's text. On an equal client, the binary row protocol is the
  *cheaper* one to receive: 1.0 µs against 1.6 µs per row on stub sockets, and 204.7 µs against
  339.3 µs of client CPU for `bar-range`.
- **The tuple key round trip is ahead, not behind.** The p50 is 48.0 µs against 62.8 µs, with
  29.3 µs of server CPU against PostgreSQL's 45.9 µs. The 0.83x-1.03x in the scenario 3 file was the
  driver's `send_command`: 29.0 µs of client CPU against `pg_wire`'s 5.0 µs.
- **The fold is the one real read-side gap, and it is per row.**
  - `agg-global` reads the same 60,480 rows as `day-slice`'s scan. Its server CPU over that relation
    is 5,776 µs against `day-slice`'s 1,493 µs, so the fold adds about 71 ns per row on top of the walk.
  - PostgreSQL's whole aggregate is about 59 ns per row.
  - `agg-by-session` shows the same direction: 1.21x PostgreSQL's server CPU.
  - BM and BT cut the *rejected* row's cost to 27 ns. An *accepted* row passed to an aggregate does
    not take that path. That is where the next read-side milestone would land
    (`docs/spec/aggregate.md` owns the fold).
- **The shapes KDS already led keep their lead on equal clients.** `point-join` runs at 2.19x on
  0.35x the server CPU, and `day-slice`, a scan with no index on either side, at 1.22x. So the
  scenario files' "ahead" column was not inflated by the same artifact; that artifact ran against KDS.

## What this does not show

- **Waystone was on.** KDS may replay a recorded trail for a repeated statement, and `symbol-history`
  has only 8 distinct keys. This compares the engines as shipped, not a scan against a scan. A
  Waystone-off arm was not run. CA-S0 (`results-ca-s0-premise-v3.2.0-152-ga9f0f5cd.md`) put replay's
  effect on scenario 1's walks at a few percent.
- **The scenario drivers are unchanged**, so their PG ratios still carry the artifact. Until a
  driver times through `kwp.Connection.query`, or its twin decodes as much as it does, a ratio taken
  from a scenario file is a statement about two Python clients.
- One connection, read-only, `cores = 1`, a 60,480-row relation, no row-count sweep. No write shape
  was measured. PostgreSQL's background processes are not in its CPU column.
- PostgreSQL was serial by setting, to match `cores = 1`. Its default could choose a parallel plan
  for a larger relation, which this run does not price.

## Reproduction

    bench/v3.2.0/archive/parity-harness/run.sh build-release/kds_server <OUTDIR> --reps 7

Raw output: `bench/v3.2.0/archive/parity-v3.2.0-171-ge81ac7cd/` (`cells.json` holds every cell, with
its per-statement row counts and host state). The stub-socket client costs come from the scratch
scripts described in Method; they were not kept in the tree.
