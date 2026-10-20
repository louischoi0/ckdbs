# CE-S0: the premise of streaming the KWP reply, measured at `v3.3.0-50-g1f57f37b`

**One line.** A `SELECT`'s reply leaves the server after its last row is encoded, and the client waits through all of it: the time a client idles
per statement equals the server's CPU per statement to within 12 % in every one of 18 sweep cells (idle / server CPU 0.88 to 1.01), which is **0.421 µs per returned row on a key range
and 0.633 µs per returned row on a filtered walk** (`WHERE symbol_id = 3`, 7,560 rows of 60,480). The first byte of a 60,480-row answer arrives 26,357 µs (p50) after the send, 94.6 % of the way to the end of the reply;
a 10,000-row answer's first batch arrives at 3,972 µs of 4,171 µs. **That wait is 8.5 % to 13.0 % of a statement on the client path the scenario drivers use**
(the client spends the other 87 % to 91.5 % on decoding and rendering text), and a slice-boundary stream (CE-R2) can hide at most
5.0 % (10,000 rows), 7.9 % (60,480 rows) and 12.1 % (the filtered walk) of it, nothing at 1,000 rows or fewer, which are 16 pages or fewer and so inside one 64-page slice.

Driver: `ce_s0_rows.py` (row-cost sweep, built on the row-cost attribution's `rowcost.py` and the `send` mode of `kdsrow.py`, both archived unmodified) and the unmodified `tools/scenario1_backtest.py`
and `tools/scenario3_library.py`; the new reusable probe `tools/ttfb_probe.py` (time to first byte and first `S_ROW_BATCH`, `--help` documents it; `bench/docs/` stays closed per `bench/README.md`).
Measured by `ck-tester`. The work order is `instructions/v3.2.0/workorder-ce-streaming-reply.md` (CE-S0, stage budget).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time executed | 2026-10-20, 09:47 to 10:11 UTC. Row-cost sweep cells 09:50:47 to 10:03:57 UTC (54 valid cells; 49 set aside, section 9); time-to-first-batch probe 10:04:04 to 10:04:08 UTC; scenario cells 10:04:41 to 10:10:31 UTC (six cells, 24 to 37 s each); plan pages (not a timing) afterwards. Cell windows: `timeline` and `logs/*.t0`, `*.t1` in the archive |
| Worktree and branch | `ce-streaming-reply` (`/home/cdkbs/ckdbs/.claude/worktrees/ce-streaming-reply`), branch `ce-streaming-reply` |
| Commit | `1f57f37b`, `git describe --tags` = `v3.3.0-50-g1f57f37b`. Tree clean at the start (`git status` empty at 09:42 UTC). **Engine source equals `9682b725`**: `git diff 9682b725 1f57f37b --stat -- src include CMakeLists.txt` is empty; since `9682b725` only `bench/`, `instructions/` and `tools/kwp.py` / `tools/ckdbs_cli.py` (the client decoder of `a40882af`) changed |
| Engine binary | **not built in this run.** `/home/cdkbs/bench-runs/ce-s0-v3.3.0-50-g1f57f37b/kds_server`, a `cp -p` of `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/kds_server`; **sha256 `7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a`**, equal to the source copy's before the first cell and after the last. Built at `9682b725` as Release (`CMAKE_BUILD_TYPE=Release`, per `summary-scenarios-v3.3.0-33-g9682b725.md`), original `build-release` mtime 2026-10-20 03:36:57.38 UTC, 3 min 17 s after its commit; the copy's mtime is 2026-10-20 03:37:20.03 UTC. The binary is older than HEAD (commit time 09:41:57 UTC) by 6 h, and measures the engine at HEAD because nothing under `src/`, `include/` or `CMakeLists.txt` differs. Every server started from the copy |
| Client | `tools/` extracted from `1f57f37b` into the run directory (`tools/kwp.py`, `tools/ckdbs_cli.py` at the `a40882af` decoder) and put on `PYTHONPATH`; Python 3.12.3; the unmodified scenario drivers; `tools/ttfb_probe.py` is new (committed with this file) |
| Build type | Release |
| Host | AMD EPYC 9V74, 8 vCPUs (4 cores x 2 threads), Linux 7.0.0-1014-azure; 15 GiB RAM |
| Data device | `/dev/root` = `/dev/nvme0n1p1`, **ext4** (`df -T` at run time), under `/home/cdkbs/bench-runs/ce-s0-v3.3.0-50-g1f57f37b/data/`; not tmpfs. Data files deleted after every cell (`/` was 97 % full) |
| Server configuration | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`; no other key (`conf/*.conf`). Ports 15761 (sweep and probe), 15610 (scenario 1), 15620 (scenario 3), 15763 (plan pages); 15432 and the PostgreSQL twin on 15434 untouched |
| Placement | Row-cost sweep and probe: server pinned to CPU 2 and client to CPU 6 (`taskset`), the attribution's pinning; CPUs 2 and 6 are not siblings of each other (siblings are 2-3 and 6-7). Scenario cells: unpinned, as the earlier matrices |
| Quiet host | Before every cell a gate required no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process and at most 8 % busy over 3 s, six checks in a row (about 30 s). A 1-second monitor (`monitor.log.gz`) ran through the run, and **every cell was checked against it** (a build/test process, or a foreign process at 50 % CPU or more, inside the cell's window discards the cell). Another session was building and running `kds_tests` in `fix-cabin-column-position-63` throughout the first 14 minutes; section 9 says what that cost |
| PostgreSQL | not measured for this shape (rule 4: the floor is optional); no PostgreSQL number is quoted from this run |
| Baseline | this engine's last run of the same shapes with the same binary and client: `bench/v3.3.0/results-scenario1-backtest-v3.3.0-38-ga40882af.md` (section 10's row-cost tables), `results-scenario3-library-v3.3.0-38-ga40882af.md` |

## 2. What was run

| Cell | Server | Knob varied | Repeats | What it records |
|---|---|---|---|---|
| Row-cost sweep, `SELECT * FROM daily_stats WHERE id BETWEEN 1 AND N`, N = 1, 100, 1,000, 10,000, 60,480 | cores 1, group, CPU 2 | returned rows N (the table is the attribution's 12-column, 60,480-row `daily_stats` BTREE, loaded by 60 inserts of 1,000 rows) | 3 per (N, path); each cell about 3 s (the statement count is calibrated from the warm-up: 9 to 27,769 statements) | wall, client CPU (`time.process_time`), server CPU (`/proc/<pid>/stat`, 10 ms ticks), per statement, with every statement's latency |
| Filtered walk, `SELECT * FROM daily_stats WHERE symbol_id = 3` | same | the access kind (`FilterScan`, 60,480 rows walked, 7,560 returned) | 3 per path | same |
| Client paths | same | `send` = the scenario drivers' own path (`ServerConnection.send_command`: decode, render every cell to text, join); `split` = `kwp.Connection.execute` (cells become `bytes`); `drain` = frame headers only, no cell touched (the wire plus the server) | each path in every cell group | `drain` is the server's own cost seen from a client that does nothing |
| Time to first byte / first batch (`tools/ttfb_probe.py`) | same server, same table | the same six statements | 100 timed per statement after 3 warm-ups (60,480 rows: 100 in 2.9 s) | microseconds from the instant before `sendall` to the `recv` that completed each frame |
| Plan pages (`ANALYZE`) | fresh server, same table | the same six statements | 1 | pages walked and rows examined, to turn rows into 64-page slices (not a timing) |
| Scenario 1, `c1-g` | cores 1, group | `--seed 1 --bars-clustered btree --verify --sync --analyze`, 60,480 bars | 3 (`s1-c1-g`, `r2-`, `r3-`) | the driver's QPS matrix and phase percentiles for `bar-range`, `symbol-history`, `model-join`, `day-slice`; `verify_problems` empty in 3 of 3 |
| Scenario 3, unindexed | cores 1, group | `--loans 10000 --index-mode none --server-indexes on --seed 1` (users 2,000, books 2,000, reservations 5,000, loans 10,000) | 3 | `books-by-genre`, `loans-by-daterange`, `overdue`, `loans-by-user`, 200 statements each per run |

**Row-count axis (rule 9).** The sweep returns 1, 100, 1,000, 10,000 and 60,480 rows by the `BETWEEN` upper bound; the filtered walk returns 7,560. Scenario 1's shapes return a fixed count
(`bar-range` 200 rows, `symbol-history` 7,560, `model-join` 359, `day-slice` 8) on 60,480 bars; scenario 3's at 10,000 loans (`books-by-genre` about 125 rows by the earlier file's count, `loans-by-user` about 5; the row counts of
`loans-by-daterange` and `overdue` were not recorded in this run).
**Cut** (stage budget, 20 minutes, no building): the sweep's 10-row size (not asked for); scenario 1 and 3 at their smaller sizes (`--years/--symbols/--exchanges` and `--loans 200, 1000`), so the 200 / 1K / 10K sweep of rule 9 is carried by the row-cost sweep alone;
the `values` client path; `c8-g` and `strict`; PostgreSQL; server CPU per slice from a sampler (derived from the plan pages instead, section 6); a per-slice CPU measurement inside the walk, which needs the build this stage does not do.

## 3. The reply is built whole, and the client idles for the server's CPU

On the `send` path a returned row costs 4.76 µs of wall time (least-squares slope over 1 to 10,000 rows): 4.34 µs of client CPU and **0.42 µs of idle**. The server's CPU for the same row, measured from a client that does nothing (`drain`), is 0.42 µs.
Idle and server CPU agree to three digits at the slope, and per statement they agree to 12 % at the worst cell. Nothing the server does overlaps with anything the client does; the only overlap visible is the 4 % to 12 % by which server CPU exceeds idle at 1,000 rows and more, which is probably the reply's tail draining through the socket buffer while the client already decodes (inferred, not measured).

Per statement, median of three runs, `send` path (the scenario drivers' path). Idle = wall minus client CPU. Baseline: the same binary and client, `v3.3.0-38-ga40882af`.

| rows returned | wall | spread of 3 | client CPU | idle | idle / wall | server CPU | idle / server CPU | wall, baseline | delta |
|---|---|---|---|---|---|---|---|---|---|
| 1 row | 85.1 µs | 3.6 % | 51.7 µs | 32.7 µs | 38.4 % | 32.3 µs | 1.01 | 83.8 µs | +1.6 % |
| 100 rows | 548.3 µs | 3.0 % | 464.1 µs | 81.3 µs | 14.8 % | 83.5 µs | 0.97 | 540.9 µs | +1.4 % |
| 1,000 rows | 4,684.5 µs | 0.7 % | 4,204.8 µs | 476.1 µs | 10.2 % | 495.0 µs | 0.96 | 4,683.6 µs | +0.0 % |
| 10,000 rows | 47,662.6 µs | 3.4 % | 43,415.4 µs | 4,247.2 µs | 8.9 % | 4,833.3 µs | 0.88 | 47,083.8 µs | +1.2 % |
| 60,480 rows | 318,902.8 µs | 0.6 % | 291,873.8 µs | 27,060.7 µs | 8.5 % | 30,000.0 µs | 0.90 | 318,365.9 µs | +0.2 % |

The same cells on the two other client paths. `drain` has no client work, so what it waits for is the server: its idle is 96 % to 100 % of the server's CPU at every size (31.8 of 32.0 µs, 75.5 of 75.6 µs, 455.1 of 470.4 µs, 4,065.3 of 4,250.4 µs, 27,236.7 of 28,446.6 µs) and 65.7 % to 95.7 % of the statement.

| rows returned | `split` wall | `split` client CPU | `split` idle | `split` idle / wall | `drain` wall | `drain` client CPU | `drain` idle | `drain` server CPU |
|---|---|---|---|---|---|---|---|---|
| 1 row | 76.7 µs | 43.8 µs | 32.3 µs | 42.1 % | 48.5 µs | 16.9 µs | 31.8 µs | 32.0 µs |
| 100 rows | 324.5 µs | 245.2 µs | 79.3 µs | 24.4 % | 93.8 µs | 17.6 µs | 75.5 µs | 75.6 µs |
| 1,000 rows | 2,612.1 µs | 2,150.7 µs | 456.1 µs | 17.5 % | 494.1 µs | 39.0 µs | 455.1 µs | 470.4 µs |
| 10,000 rows | 26,230.7 µs | 22,118.1 µs | 4,167.6 µs | 15.9 % | 4,275.1 µs | 209.8 µs | 4,065.3 µs | 4,250.4 µs |
| 60,480 rows | 186,440.3 µs | 159,189.2 µs | 27,218.7 µs | 14.6 % | 28,449.7 µs | 1,216.9 µs | 27,236.7 µs | 28,446.6 µs |

**Per returned row** (a fitted slope is the per-row cost; the marginal is `(cost(N) - cost(1 row)) / (N - 1)`; the fixed part is the 1-row statement: 32 µs of server CPU and 85 µs of wall on `send`):

| client path | quantity | marginal at 100 | marginal at 1,000 | marginal at 10,000 | marginal at 60,480 | slope, 1 to 10,000 rows | slope, 1 to 60,480 rows |
|---|---|---|---|---|---|---|---|
| `send` | wall | 4.679 µs/row | 4.604 µs/row | 4.758 µs/row | 5.272 µs/row | 4.762 µs/row | 5.283 µs/row |
| `send` | client CPU | 4.166 µs/row | 4.157 µs/row | 4.337 µs/row | 4.825 µs/row | 4.342 µs/row | 4.836 µs/row |
| `send` | **idle** | 0.491 µs/row | 0.444 µs/row | 0.421 µs/row | 0.447 µs/row | **0.421 µs/row** | 0.447 µs/row |
| `send` | server CPU | 0.517 µs/row | 0.463 µs/row | 0.480 µs/row | 0.496 µs/row | 0.480 µs/row | 0.496 µs/row |
| `split` | wall | 2.503 µs/row | 2.538 µs/row | 2.616 µs/row | 3.081 µs/row | 2.618 µs/row | 3.092 µs/row |
| `split` | client CPU | 2.034 µs/row | 2.109 µs/row | 2.208 µs/row | 2.631 µs/row | 2.211 µs/row | 2.641 µs/row |
| `split` | **idle** | 0.475 µs/row | 0.424 µs/row | 0.414 µs/row | 0.450 µs/row | **0.413 µs/row** | 0.450 µs/row |
| `split` | server CPU | 0.470 µs/row | 0.446 µs/row | 0.448 µs/row | 0.485 µs/row | 0.448 µs/row | 0.486 µs/row |
| `drain` | wall | 0.458 µs/row | 0.446 µs/row | 0.423 µs/row | 0.470 µs/row | 0.422 µs/row | 0.470 µs/row |
| `drain` | client CPU | 0.007 µs/row | 0.022 µs/row | 0.019 µs/row | 0.020 µs/row | 0.019 µs/row | 0.020 µs/row |
| `drain` | **idle** | 0.441 µs/row | 0.424 µs/row | 0.403 µs/row | 0.450 µs/row | **0.403 µs/row** | 0.451 µs/row |
| `drain` | server CPU | 0.441 µs/row | 0.439 µs/row | 0.422 µs/row | 0.470 µs/row | 0.421 µs/row | 0.471 µs/row |

The engine's term is the same 0.40 to 0.45 µs per row on all three paths and does not depend on how fast the client is. What changes between the paths is the share: 8.9 % of a `send` statement at 10,000 rows, 15.9 % of a `split` statement, 95.1 % of a `drain` one.
The client's own per-row cost grows 11 % from 10,000 to 60,480 rows (4.34 to 4.83 µs of client CPU per row) while the idle term moves 0.421 to 0.447 µs: the client is the part that is not flat.

### Distribution of one statement (`send` and `drain`)

All statements of the three runs pooled; the count is the statements measured. The `drain` rows are the wire plus the server with no client work.

| shape | path | statements | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| 1 row | `send` | 28,030 | 78.3 µs | 81.2 µs | 82.5 µs | 99.3 µs | 111.1 µs |
| 1 row | `drain` | 54,895 | 44.6 µs | 46.2 µs | 46.9 µs | 58.6 µs | 67.8 µs |
| 100 rows | `send` | 10,937 | 514.6 µs | 530.2 µs | 536.2 µs | 572.6 µs | 656.7 µs |
| 100 rows | `drain` | 45,013 | 85.3 µs | 89.0 µs | 91.8 µs | 108.7 µs | 122.3 µs |
| 1,000 rows | `send` | 1,805 | 4,533.2 µs | 4,623.8 µs | 4,660.9 µs | 4,795.5 µs | 5,678.5 µs |
| 1,000 rows | `drain` | 14,072 | 468.1 µs | 482.1 µs | 486.1 µs | 518.0 µs | 764.1 µs |
| 10,000 rows | `send` | 172 | 45,561.8 µs | 46,729.6 µs | 47,523.6 µs | 49,639.8 µs | 51,152.8 µs |
| 10,000 rows | `drain` | 1,879 | 4,115.2 µs | 4,200.9 µs | 4,218.4 µs | 4,486.5 µs | 5,147.7 µs |
| 60,480 rows | `send` | 27 | 310,093.9 µs | 314,728.7 µs | 316,197.3 µs | 329,953.4 µs | 335,152.6 µs |
| 60,480 rows | `drain` | 309 | 27,886.0 µs | 28,152.6 µs | 28,294.4 µs | 29,095.8 µs | 32,337.9 µs |
| filtered walk, 7,560 rows | `send` | 234 | 35,340.1 µs | 36,008.2 µs | 36,361.7 µs | 38,414.5 µs | 40,731.3 µs |
| filtered walk, 7,560 rows | `drain` | 1,679 | 4,414.2 µs | 4,472.1 µs | 4,534.1 µs | 4,634.9 µs | 4,835.3 µs |

On the `send` path p99 is 6.0 % to 12 % above p50 from 10,000 rows (7.6 % at 10,000, 6.0 % at 60,480, 12 % on the filtered walk), and 22 % at 100 and 1,000 rows and 35 % at one row; the `drain` tails are longer (6.6 % to 57 % above p50, 57 % at 1,000 rows; section 9).

## 4. The filtered walk: the non-overlap is 0.633 µs per returned row, measured

`WHERE symbol_id = 3` is a `FilterScan` that walks all 877 pages (60,480 rows) to return 7,560. The client waits through the whole walk, the unreturned rows' share included:

| client path | wall | client CPU | idle | idle / wall | server CPU | idle / server CPU | idle per returned row | server CPU per returned row | server CPU per row walked |
|---|---|---|---|---|---|---|---|---|---|
| `send` | 36,691.1 µs | 31,913.4 µs | 4,782.6 µs | 13.0 % | 5,068.5 µs | 0.94 | **0.633 µs/row** | 0.670 µs/row | 0.084 µs/row |
| `split` | 20,904.3 µs | 16,287.6 µs | 4,616.7 µs | 22.1 % | 4,822.7 µs | 0.96 | 0.611 µs/row | 0.638 µs/row | 0.080 µs/row |
| `drain` | 4,546.5 µs | 164.1 µs | 4,382.4 µs | 96.4 % | 4,531.0 µs | 0.97 | 0.580 µs/row | 0.599 µs/row | 0.075 µs/row |

The work order's bound ("up to about 0.64 µs", `instructions/v3.2.0/workorder-ce-streaming-reply.md` section 1, derived from a key range's 0.43 µs plus the walk) holds: measured, 0.633 µs on `send`.
Against the 10,000-row key range's 0.421 µs idle per row, the filtered walk's extra 0.212 µs per returned row is 1,600 µs of walk over the 52,920 rows not returned, 0.030 µs per row not returned (derived by subtraction across two shapes in one window).
The filtered walk is 13.0 % non-overlap on `send` and 22.1 % on `split`, against 8.9 % and 15.9 % for the key range: the unreturned rows make it the shape CE helps most, per statement.

## 5. Time to first byte and to the first `S_ROW_BATCH`

Microseconds from the instant before `sendall` to the `recv` that completed the frame; 100 statements per row, client on CPU 6 and server on CPU 2, `tools/ttfb_probe.py` (reusable: the close of CE re-runs it unchanged). "Ready" = `S_READY`, the reply over. No frame reaches the client before the statement ends: the first byte and the first batch arrive within 20 µs of each other at every size (medians), and every one of the 600 replies began `S_PARSE_OK`, `S_BIND_OK`, `S_ROW_DESC`, `S_ROW_BATCH` (a one-batch reply continues `S_COMPLETE`, `S_READY`).

| rows returned | batches | bytes | event | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| 1 row | 1 | 476 B | first byte | 44.5 µs | 45.4 µs | 45.7 µs | 51.8 µs | 61.4 µs |
| | | | first batch | 44.5 µs | 45.4 µs | 45.7 µs | 51.8 µs | 61.4 µs |
| | | | ready | 44.5 µs | 45.4 µs | 45.7 µs | 51.8 µs | 61.4 µs |
| 100 rows | 1 | 14,336 B | first byte | 84.8 µs | 86.6 µs | 89.5 µs | 102.9 µs | 108.6 µs |
| | | | first batch | 84.8 µs | 86.6 µs | 89.5 µs | 102.9 µs | 108.6 µs |
| | | | ready | 84.8 µs | 86.6 µs | 89.5 µs | 102.9 µs | 108.6 µs |
| 1,000 rows | 3 | 140,356 B | first byte | 452.6 µs | 457.0 µs | 465.0 µs | 480.0 µs | 493.1 µs |
| | | | first batch | 468.4 µs | 473.2 µs | 482.1 µs | 496.7 µs | 508.0 µs |
| | | | ready | 471.8 µs | 483.7 µs | 489.8 µs | 506.5 µs | 513.4 µs |
| 10,000 rows | 22 | 1,400,546 B | first byte | 3,926.4 µs | 3,944.0 µs | 3,953.9 µs | 4,012.5 µs | 4,066.5 µs |
| | | | first batch | 3,944.3 µs | 3,962.4 µs | 3,972.1 µs | 4,025.6 µs | 4,087.5 µs |
| | | | ready | 4,133.0 µs | 4,159.6 µs | 4,171.3 µs | 4,269.3 µs | 4,547.6 µs |
| 60,480 rows | 130 | 8,468,826 B | first byte | 25,945.7 µs | 26,242.5 µs | 26,356.5 µs | 27,222.9 µs | 28,670.5 µs |
| | | | first batch | 25,945.7 µs | 26,242.5 µs | 26,356.5 µs | 27,226.1 µs | 28,670.5 µs |
| | | | ready | 27,482.3 µs | 27,742.9 µs | 27,849.2 µs | 28,998.6 µs | 30,294.0 µs |
| filtered walk, 7,560 rows | 17 | 1,058,896 B | first byte | 4,258.0 µs | 4,276.5 µs | 4,295.2 µs | 4,369.3 µs | 4,618.8 µs |
| | | | first batch | 4,277.6 µs | 4,297.1 µs | 4,315.1 µs | 4,384.0 µs | 4,639.2 µs |
| | | | ready | 4,406.6 µs | 4,430.3 µs | 4,448.9 µs | 4,510.6 µs | 4,793.1 µs |

Medians: the first batch is at 98.4 % (1,000 rows), 95.2 % (10,000), 94.6 % (60,480) and 97.0 % (filtered walk) of the time to the end of the reply. What remains after the first batch is the transfer of the rest: 7.7 µs, 199.2 µs, 1,492.7 µs and 133.8 µs, over 2, 9, 58 and 7 `recv` calls. A client on this server sees nothing of a 60,480-row answer for 26.4 ms.

## 6. Slices: how much a slice-boundary stream could overlap

`ANALYZE` of the same statements (`logs/analyze-plans.txt`; `Range` is the clustered head seek plus the leaf-chain walk of `RunWalkStep`, which yields every `pages_per_slice` = 64 pages, `src/exec/step_vm.cpp:1994` and `:2126` at `1f57f37b` (read with `git show`, the worktree file was being edited), engine source equal to `9682b725`; read, not exercised):

| rows returned | pages walked | rows examined | 64-page slices | server CPU (`drain`) | server CPU per 64 pages (derived) |
|---|---|---|---|---|---|
| 1 row | 2 pages | 69 rows | 1 | 32.0 µs | - |
| 100 rows | 3 pages | 138 rows | 1 | 75.6 µs | - |
| 1,000 rows | 16 pages | 1,035 rows | 1 | 470.4 µs | - |
| 10,000 rows | 146 pages | 10,005 rows | 3 (2.28 full) | 4,250.4 µs | 1,863 µs |
| 60,480 rows | 877 pages | 60,480 rows | 14 (13.70 full) | 28,446.6 µs | 2,076 µs |
| filtered walk, 7,560 rows | 877 pages | 60,480 rows | 14 (13.70 full) | 4,531.0 µs | 331 µs |

**Derived ceiling** (`[design]`, not measured; no build exists that streams): with a write at each slice boundary the server's first slice cannot overlap the client (nothing to decode yet) and the remainder can, if the client is the slower side, which it is on `send` and `split` by 9x and 5x.
The most a stream can then remove from a statement is `idle x (1 - 1 / slices)`:

| rows returned | `send`: most removable | share of `send` wall | `split`: most removable | share of `split` wall |
|---|---|---|---|---|
| 1,000 rows (one slice) | 0 µs | 0.0 % | 0 µs | 0.0 % |
| 10,000 rows | 2,385 µs | 5.0 % | 2,341 µs | 8.9 % |
| 60,480 rows | 25,086 µs | 7.9 % | 25,232 µs | 13.5 % |
| filtered walk, 7,560 rows | 4,434 µs | 12.1 % | 4,280 µs | 20.5 % |

A reply of 1,000 rows or fewer is inside one slice and reaches no boundary before the end, so CE-R2 (a) changes nothing for it. That includes `bar-range` (200 rows), `model-join` (359 rows) and scenario 3's `books-by-genre` (about 125 rows).

## 7. Scenario cells, `c1-g`, three runs each

Scenario 1 (60,480 bars). Warm QPS is the driver's matrix cell (100 statements, 8 arguments cycled); the three runs are `s1-c1-g`, `r2-s1-c1-g`, `r3-s1-c1-g`. Baseline: `v3.3.0-38-ga40882af`, same binary and client, its median of three.

| shape | rows returned | warm run 1 | run 2 | run 3 | median | spread of 3 | baseline | delta | cold, median |
|---|---|---|---|---|---|---|---|---|---|
| bar-range | 200 rows | 1,257.2 qps | 1,263.3 qps | 1,239.6 qps | 1,257.2 qps | 1.9 % | 1,260.3 qps | -0.2 % | 1,262.4 qps |
| symbol-history | 7,560 rows | 27.3 qps | 27.3 qps | 27.5 qps | 27.3 qps | 0.7 % | 27.3 qps | +0.0 % | 27.7 qps |
| model-join | 359 rows | 1,051.0 qps | 1,042.8 qps | 1,059.3 qps | 1,051.0 qps | 1.6 % | 1,030.2 qps | +2.0 % | 1,035.8 qps |
| day-slice | 8 rows | 589.6 qps | 590.1 qps | 602.6 qps | 590.1 qps | 2.2 % | 508.7 qps | +16.0 % | 599.3 qps |

Distributions, the driver's own phases (median over the three runs of each run's percentile; `read-symbol-history` has 10 statements per run, so its p95 and p99 are its maximum):

| phase | rows returned | statements per run | p0 | p25 | p50 | p95 | p99 | QPS from the mean |
|---|---|---|---|---|---|---|---|---|
| read-bar-range | 200 rows | 200 | 763.9 µs | 778.1 µs | 784.1 µs | 817.7 µs | 829.8 µs | 1,267.1 qps |
| read-symbol-history | 7,560 rows | 10 | 35,570.0 µs | 35,909.6 µs | 36,108.9 µs | 39,414.5 µs | 39,414.5 µs | 27.1 qps |
| read-day-slice | 8 rows | 200 | 1,618.1 µs | 1,657.3 µs | 1,666.3 µs | 1,734.8 µs | 1,862.2 µs | 598.3 qps |

`model-join` is reported by the matrix only (mean 951.1 µs and p50 941.1 µs warm, medians of the runs); the driver has no phase for it.

Scenario 3 (10,000 loans, no index). QPS is derived (1,000,000 / the phase's mean µs) per run; 200 statements per run. Baseline: `v3.3.0-38-ga40882af`'s `none: now` column.

| shape | run 1 | run 2 | run 3 | median | spread of 3 | baseline | delta | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| books-by-genre | 3,096 qps | 3,135 qps | 3,125 qps | 3,125 qps | 1.2 % | 2,925 qps | +6.8 % | 285.3 µs | 306.0 µs | 316.4 µs | 348.0 µs | 441.7 µs |
| loans-by-daterange | 946 qps | 987 qps | 1,000 qps | 987 qps | 5.5 % | 913 qps | +8.1 % | 394.2 µs | 993.2 µs | 1,021.4 µs | 1,095.4 µs | 1,157.7 µs |
| overdue | 1,176 qps | 1,232 qps | 1,241 qps | 1,232 qps | 5.4 % | 1,126 qps | +9.4 % | 380.2 µs | 805.6 µs | 829.1 µs | 888.4 µs | 937.2 µs |
| loans-by-user | 2,875 qps | 2,846 qps | 2,950 qps | 2,875 qps | 3.6 % | 2,449 qps | +17.4 % | 325.1 µs | 334.4 µs | 342.7 µs | 374.9 µs | 394.0 µs |

`loans-by-daterange` and `overdue` have a p0 far below their p25 (394 and 380 µs against 993 and 806 µs); the baseline's run shows the same split (495 and 428 µs against 1,079 and 884 µs). It is not explained here.

**Reading the deltas against the baseline.** The pinned sweep reproduces the baseline's `send` wall, from a window about an hour earlier on the same host (its `send` cells ran around 08:50 UTC), to within 1.6 % at every size (table in section 3). The unpinned scenario cells do not: `loans-by-user` is a 5-row shape that no change to the reply path could move
and sits 17.4 % above the baseline, and `day-slice` (8 rows) 16.0 % above it; the many-row shapes sit between -0.2 % and +9.4 %. The window-to-window floor for an unpinned scenario cell is therefore about 17 %, an order of magnitude over its within-run spread (0.7 % to 5.5 %),
and **the close of CE must read its scenario cells from an interleaved old-binary / new-binary pair in one window, not against this file**; this file's scenario numbers are the base for the shape of the distributions and the within-run spread, not for a cross-window delta.

## 8. Waits, named

Every cell is a read on one connection: no durability wait (no commit, no fsync), no lock or conflict wait (one session, no writer), no write-statement wait. A `send`-path statement is the sum:

| component | where it shows | share of a 10,000-row `send` statement |
|---|---|---|
| client decode (cells to Python `bytes`) | `split` client CPU minus `drain` client CPU: 21,908 µs | 46.0 % |
| client render and join to text | `send` client CPU minus `split` client CPU: 21,297 µs | 44.7 % |
| client receive | `drain` client CPU: 209.8 µs | 0.4 % |
| server work the client waits for (walk, encode, write): idle | 4,247.2 µs | 8.9 % |
| socket and client round trip | the 1-row statement's 85.1 µs wall less its 32.3 µs server CPU is 52.8 µs, of which 51.7 µs is client CPU; the loopback transit is not separable and is below this run's resolution | under 0.1 % |
| durability, lock, write-statement | do not apply | - |

The statement's wait structure is serial: client send, server walk and encode (the first byte leaves only now), the transfer, the client's decode. Idle is the first two.

## 9. Noise floor, validity and what contaminated the first minutes

- **Within-run floor.** The three repeats of the sweep's cells spread 0.2 % to 3.6 % of the median in wall time (`drain` 0.2 % to 3.6 %, `split` 0.6 % to 2.2 %, `send` 0.6 % to 3.6 %); the three scenario runs spread 0.7 % to 5.5 % (section 7). A difference below those is not a finding.
- **Across-window floor.** The pinned sweep reproduced `a40882af`'s `send` wall, from a window about an hour earlier, to within 1.6 % at five sizes; the unpinned scenario cells moved up to 17.4 % on shapes the reply path cannot affect (section 7).
- **Contamination.** Another session built (`cc1plus`, `ld`, `ar`) and ran `kds_tests` and `ctest` in `fix-cabin-column-position-63` from before 09:42 UTC until about 09:58 UTC. The gate (then one quiet check) passed during a lull and the first sweep pass ran into the build:
  **49 sweep cells were set aside** (`rowcost/cells.contaminated.jsonl.gz`, each with a build/test sample or a foreign process at 50 % CPU or more in its window) and re-run on a quiet host after the gate was changed to six consecutive checks (about 30 s) and the driver to check each cell against the monitor as it ends. No scenario cell and no probe pass was set aside;
  all six scenario windows (23 to 34 monitor samples each) and the probe window (7 samples) hold no build/test sample and no foreign process at 10 % CPU or more.
- **Residual samples.** 17 of the 54 kept sweep cells hold one to three samples of a foreign process between 10 % and 50 % CPU: the other sessions' idle `claude` processes (10 % to 21 %), one `git add` at 11 %, and two cells of the first pass that coincide with single `ar` / `ranlib` samples (26 % and 29 % / 49 %) on 09:50 to 09:51. The two build-tool cells are `range-1000 split` run 1 and `range-1 drain` run 2;
  without them the medians move by 0.5 % and 0.0 % (`sens.py` in the archive).
  The 54 kept cells come from two server instances that read the same freshly loaded table (the sweep only reads): the first master's (5 cells at 09:50 to 09:51 and 15 at 09:59, before the gate and the driver were strengthened and the master restarted at about 10:00) and the second's (34 cells, 10:00:53 to 10:03:57).
- **The 1,000-row tail.** The 1,000-row `send` and `drain` distributions have a p99 of 5,678.5 µs and 764.1 µs against p50s of 4,660.9 µs and 486.1 µs; with 1,805 and 14,072 statements this is the slowest 1 % of statements, present in the baseline's table as well (`send` p99 5,485 µs); it is not explained here.
- **Server CPU resolution.** `/proc/<pid>/stat` ticks are 10 ms; every cell lasted about 3 s or more (at least 5 statements at 60,480 rows, 9 per run in the median), so a tick is 0.3 % of a cell. The `drain` cells' server CPU equals their wall to within 0.5 % to 4 %, which bounds the error of the idle / server CPU ratio.

## 10. What the run teaches about the engine

1. **The reply path is store-and-forward, and the price is exactly the server's CPU.** `WireResultSink` buffers every batch and `OnStatementComplete` sends them (`instructions/v3.2.0/workorder-ce-streaming-reply.md` section 2); this run shows the consequence is a clean one: idle equals server CPU per row at 0.42 µs, 0.41 µs and 0.40 µs on three client paths,
   the first byte arrives at the statement's end (94.6 % of the reply's time at 60,480 rows), and the engine's term does not change with client speed. `docs/spec/protocol.md` section 7's "the statement runs whole into the portal's buffered batches" and BN-R7's single write are priced by this file at 0.42 µs per returned row.
2. **The gain is capped by the client, not the engine.** On the path the scenario drivers use, the client spends 91.1 % to 91.5 % of a many-row statement; hiding the engine's whole term would take at most 8.5 % to 8.9 % of it, and a slice-boundary stream hides 5.0 % to 7.9 % of it at best. A faster client (the `split` path) raises the ceiling to 8.9 % to 13.5 %, a client that decodes nothing (`drain`) to nearly the whole statement.
   The work order's section 1 states the non-overlap as "56 % to 84 % of the remaining gap" to PostgreSQL (not measured here); this run measures the term itself, 0.421 µs per row on a key range and 0.633 µs on the filtered walk, and a slice-boundary stream can hide at most the fraction `1 - 1/slices` of it.
3. **Streaming at 64-page slices has nothing to do for a reply of 1,000 rows or fewer.** 1,000 rows are 16 pages, one slice; a boundary is reached only at 64 pages (about 4,400 rows at 69 rows per page). The shapes the premise cites (`bar-range` 200 rows, `model-join` 359 rows) end inside the first slice, so CE-R2 (a) has no boundary to write at for them; `symbol-history` (7,560 rows, 877 pages, 14 slices) and the filtered walk are the shapes CE moves.
   A write at each batch seal would not reach the small ones either: a reply of 100 rows is one batch and of 1,000 rows three (batches are about 64 KiB), so a seal-triggered write sends the last batch only at the end.
4. **The first byte is the slowest thing a client of a large answer sees**: 26.4 ms for 60,480 rows, 4.3 ms for 7,560 filtered rows. The first byte is stable (p99 / p50 1.03 to 1.09 above 1,000 rows). The server CPU of one 64-page slice is about 2.1 ms at 60,480 rows (derived, section 6), 7.9 % of today's 26.4 ms to the first batch.
5. **No limit was found and none was sought**: serial single-connection reads on one core are bounded by the client's per-row work (87 % to 91.5 %), not by a server resource; durability does not apply to reads. The extreme taken is the whole table (60,480 rows, 8.5 MB, 130 batches).

## 11. What the close of CE re-runs

`ce_s0_rows.py run` (sweep, same pinning, same three paths), `tools/ttfb_probe.py --daily-stats-sizes 1,100,1000,10000,60480 --daily-stats-filter 3` (same table), and the scenario cells, with the old and the new binary interleaved in one window for the scenario cells (section 7).
Commands are in `bench/v3.3.0/archive/ce-s0-v3.3.0-50-g1f57f37b/run_ce_s0.sh`; `analyze.py <run dir>` rebuilds every table above from the run directory (`rowcost/cells.jsonl`, `json/`).

## 12. Archive

`bench/v3.3.0/archive/ce-s0-v3.3.0-50-g1f57f37b/`: `run_ce_s0.sh` (master), `lib.sh`, `monitor.sh`, `watchdog.sh`, `cellvalid.py`, `rowcost/ce_s0_rows.py` (+ the attribution's `rowcost.py`, `kdsrow.py`, unmodified), `ttfb_probe.py`, `analyze.py`, `analysis.md` (the tables as rendered),
`rowcost/cells.jsonl.gz` (54 valid cells with every statement's latency), `rowcost/cells.contaminated.jsonl.gz`, `json/` (six scenario summaries and `ttfb.json`), `logs/` (drivers' stdout, prechecks, validity, `analyze-plans.txt`, server logs), `conf/`, `monitor.log.gz`, `timeline`, `watchdog.log`, `binary.sha256`.
No data file, no WAL segment and no binary is archived.
