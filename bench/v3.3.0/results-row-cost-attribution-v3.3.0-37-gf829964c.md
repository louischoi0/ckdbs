# Where the per-returned-row cost goes: KDS against PostgreSQL 18.6, at `v3.3.0-37-gf829964c`

**Finding.** Most of the 2x gap in cost per returned row between KDS and PostgreSQL, about 84 % of it, is in the Python wire decoder in `tools/kwp.py`.
The other 16 % (0.42 µs per row) is engine-side, though not CPU: KDS's server work is not overlapped with the client's, because a KWP reply leaves only at
statement completion. On a 12-column `daily_stats` read by tuple-key range, KDS's server spends **0.43 µs of CPU per returned row against PostgreSQL's 0.57 µs**
(KDS at 0.75x over 1 to 10,000 rows, 0.88x over 100 to 60,480), while the client spends **5.8 µs per row decoding KDS's reply against 3.6 µs decoding PostgreSQL's**
(1.62x). A control that rewrites only `kwp._row_batch`'s inner loop into one loop takes KDS's client CPU from 5.11 to 2.14 µs per row, and KDS's wall
slope falls from 5.54 to 2.56 µs per row against PostgreSQL's 2.88. The control's loop does less per cell than `pg_wire._decode_data_row` does, so
the 0.89x is not a like-for-like decoder comparison (section 5). No sampler break-down of the server was run: the server's CPU is not where the gap is (section 6).

Driver: `bench/v3.3.0/archive/row-cost-attribution-v3.3.0-37-gf829964c/rowcost.py` (this measurement's own driver; scenario drivers under `tools/`
are untouched). Measured by `ck-tester`. The question comes from section 4 of `bench/v3.3.0/results-scenario1-backtest-v3.3.0-33-g9682b725.md`.
**Budget lifted by the operator for this measurement.**

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; main sweep 06:19 to 06:21 UTC, fixed-window server-CPU sweep 06:22 to 06:24, decoder control 06:24 to 06:26, `cProfile` 06:25 UTC |
| Branch, commit | worktree `row-cost-attribution`, `f829964c`, `git describe --tags` = `v3.3.0-37-gf829964c`, tree clean when the run started (the archive directory is new and untracked) |
| Engine measured | binary copy `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/kds_server`, **sha256 `7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a`** (checked equal before the server started), mtime 2026-10-20 03:37:20 UTC, Release, built at `9682b725` (`v3.3.0-33-g9682b725`). `git diff 9682b725 f829964c -- src include CMakeLists.txt tools` is empty, so engine source and both client modules (`tools/kwp.py`, `tools/pg_wire.py`) are those of `f829964c` |
| KDS server config | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, port 15690, `log_level = warn` (`kds.conf` in the archive); server started under `taskset -c 2` |
| PostgreSQL | 18.6, the scratch twin on port 15434 (`/home/cdkbs/pg-bench-ba`), defaults (`shared_buffers` 128MB, `jit on`), connected as `cdkbs` into a throwaway database `rowcost` (dropped afterwards); backend pinned to CPU 4 by `sched_setaffinity` on the pid `pg_backend_pid()` returned |
| Client | one Python process (`python3`, `tools/kwp.py` and `tools/pg_wire.py` unmodified) under `taskset -c 6`; one connection per engine, engines alternated within every cell group |
| Pinning | CPUs 2, 4 and 6 are three different physical cores (SMT siblings 3, 5, 7 idle, from `/sys/devices/system/cpu/cpu*/topology`). **The scenario runs were not pinned** (their run scripts carry no `taskset`); this measurement pins to remove scheduler placement from the attribution |
| Host | AMD EPYC 9V74, 8 logical CPUs, Linux 7.0.0-1014-azure; quiet at start (load average 0.07, no `cc1plus`, `ctest` or `kds_tests`); no build ran during the measurement. **Rule 4 of `bench/README.md` is not met:** the driver records no `/proc/loadavg` or `pgrep` per cell, so the host's load is known at the start only |
| Device | data file `/home/cdkbs/bench-runs/row-cost/data/kds.db` on `/dev/root`, `ext4` (`df -T`), never tmpfs; deleted afterwards. Reads only, so durability does not enter the numbers |
| Data | `daily_stats`, scenario 1's schema (`id, bar_id, symbol_id, session_no int32`, eight `int64` feature columns), 60,480 rows, tuple keys 1..60,480, loaded with 1,000-row multi-row `INSERT`s by the same generator into both engines. KDS `BTREE`; PostgreSQL `PRIMARY KEY (id)` plus `ANALYZE` |
| Query | `SELECT * FROM daily_stats WHERE id BETWEEN 1 AND N`, N = the returned row count: 1, 10, 100, 1,000, 10,000, 60,480. Mapping to the scenario: 60,480 is its default `--years 30 --symbols 8` bar count. Neither engine scans: both walk the key range. **Not the scenario's shapes:** scenario 1's `read-bar-range` (7.0 µs per row there) reads `daily_bars`, 9 columns, by key range, and `read-symbol-history` (9.3 µs per row) reads `daily_stats`, 12 columns, by a `symbol_id` filter. The call counts of section 5 put the decode cost per cell, so a 9-column row would cost the decoder about 3/4 of a 12-column row; that, and whether the KDS / PG ratio moves with the column count, was not measured |
| Repeats | 3 per point; spread = (max - min) / median |
| PG result format | **text** (simple query protocol; `tools/pg_wire.py` has no way to request binary). KDS sends fixed-width binary values |
| Baseline | no earlier kdbs row-cost attribution exists; this file is the baseline. PostgreSQL is the floor, as the rules set it |

## 2. Method

Three client modes read the same reply, so the Python decode can be priced by subtraction:

| mode | KDS | PostgreSQL | what it costs the client |
|---|---|---|---|
| `drain` | frame headers only (`u32` length, type byte) to `S_READY` | message headers only (type byte, `u32` length) to `ReadyForQuery`; one `D` message per row | receiving, plus one Python header parse per frame (KDS: one per 64 KiB batch; PG: one per row) |
| `split` | `kwp.Connection.execute` | `pg_wire.PgConnection.fetch` | cells become `bytes` objects in a list of lists |
| `values` | `split` + `int.from_bytes` on every cell | `split` + `int()` on every cell | cells become Python ints |

Server CPU is `utime + stime` of the KDS server process (all threads) or the PostgreSQL backend from `/proc/<pid>/stat`, over `n` statements of a cell;
client CPU is `time.process_time()` of the client process over the same window. The slope per returned row is a least-squares fit of the per-size medians
over 1 to 10,000 rows (the fit over 100 to 60,480 is in the archive, within 12 % of it for each slope of section 3's component table, and its intercepts
go negative, which is why the 1..10,000 fit is the one quoted; the cause of the curvature at 60,480 rows was not separated). The KDS / PG ratio of server CPU
does move with the fit range: 0.75x over 1 to 10,000 rows, 0.88x over 100 to 60,480 (0.474 against 0.540 µs per row), and 0.87x per statement at 60,480 rows.
`/proc` ticks are 10 ms, so the first sweep (windows of 0.09 to 8 s, `drain` cells 0.09 to 0.7 s) gave
KDS server-CPU spreads of 6 to 18 % at 100 to 1,000 rows; **the server-CPU column is from a second sweep with 2.6 to 5.5 s windows** (`drain` mode,
`--target-s 3`, at least 206 ticks per window), whose spreads are 0 to 3 %. That sweep is `drain` only: in `values` mode, where the client reads slowly,
server CPU was taken in the short windows alone (0.50 µs per row KDS, 0.54 PostgreSQL, 0.91x, within that sweep's noise). The remainder in section 3
mixes the two modes, but the engine-side share it feeds does not depend on the server figure (section 3). KDS server CPU at idle was read with `rowcost.py idle`
as 0.7 ms per second (its 50 ms tick); **that output was not archived**.
What each figure counts: KDS's is every thread of the server process (reactor, WAL writer, data-sync thread, the `system` tick), all pinned to CPU 2;
PostgreSQL's is the backend alone, not the postmaster, WAL writer, checkpointer, background writer or autovacuum. The run is read-only, so the excluded
PostgreSQL processes do little, and counting more threads for KDS only biases against KDS. On loopback the kernel runs delivery into the receiver's socket
in the sender's context, so each server figure carries part of its client's receive cost. Client CPU is `CLOCK_PROCESS_CPUTIME_ID` (ns resolution) of the
single-threaded client.

## 3. The component table

Microseconds per returned row, slope over 1 to 10,000 rows, median of 3 repeats per size. `values` mode is the like-for-like read (client holds Python ints).

| component | KDS | PostgreSQL 18.6 | KDS / PG |
|---|---|---|---|
| **Wall slope, `values`** | 6.19 µs/row | 3.57 µs/row | **1.74x** |
| Server CPU (fixed-window sweep) | 0.43 µs/row | 0.57 µs/row | **0.75x** |
| Client CPU, total | 5.76 µs/row | 3.55 µs/row | **1.62x** |
| - receive (`drain` CPU: syscalls plus header parse) | 0.02 µs/row | 0.27 µs/row | 0.07x |
| - split cells into `bytes` objects (`split` minus `drain`) | 5.17 µs/row | 2.59 µs/row | **1.99x** |
| - convert cells to ints (`values` minus `split`) | 0.57 µs/row | 0.69 µs/row | 0.82x |
| Client idle = wall - client CPU | 0.43 µs/row | 0.01 µs/row | server exposed |
| Remainder = wall - server - client | +0.00 µs/row | -0.56 µs/row | overlap |
| Wall slope, `split` | 5.61 µs/row | 2.87 µs/row | 1.95x |
| Wall slope, `drain` (Python decode removed) | 0.43 µs/row | 0.57 µs/row | 0.75x |
| Wire bytes per returned row | 140.0 B | 97.4 B (text) | 1.44x |
| Fixed cost per statement, `drain` wall intercept | 57 µs | 86 µs | 0.66x |

Reading it. With the Python decode removed (`drain`) KDS is the faster engine per row, and its wall slope equals its server CPU slope: the server alone bounds it. A negative
remainder means server and client work overlap in time: PostgreSQL's backend and the client overlap 0.56 of the backend's 0.57 µs per row; KDS's do not overlap at all
(wall 6.19 = 0.43 + 5.76 within 0.01). The KDS client idles 0.41 to 0.43 µs per row in every mode (`drain`, `split`, `values`, and the control of section 5),
which is KDS's server CPU per row; PostgreSQL's client idles 0.01 µs per row in `split` and `values`. The engine source accounts for it: the server encodes every
row into sealed batches while the statement runs (`WireResultSink::Emit`, `src/server/kwp_session.cpp:263`), and the row description, the batches and the
completion are put on the wire only at statement completion (`KwpSession::Deliver`, `src/server/kwp_session.cpp:864`, called from `OnStatementComplete`;
the one write is BN-R7, `src/server/tcp_server.cpp:565`). The client therefore receives nothing until the last row is built, while PostgreSQL's backend sends
its rows as it produces them. That makes the non-overlap a property of the engine's reply path, not of the decoder. Bytes on the wire go the other way from cost: KDS sends 44 % more bytes
per row (12 length-prefixed binary cells) and still costs the client less to receive, because its frames carry about 460 rows each and PostgreSQL's carry one.

Where the 2.62 µs/row wall gap goes (6.19 - 3.57):

| part of the gap | µs/row | share of gap |
|---|---|---|
| server CPU | -0.14 µs/row | -5 % |
| client: receive | -0.25 µs/row | -10 % |
| client: split into `bytes` | +2.58 µs/row | +98 % |
| client: convert to int | -0.12 µs/row | -5 % |
| remainder (overlap PostgreSQL has and KDS does not: KDS's reply leaves at completion) | +0.56 µs/row | +21 % |
| **sum** | **+2.62 µs/row** | 100 % |
| of which client CPU (receive + split + convert) | +2.20 µs/row | +84 % |
| of which engine-side (server CPU + remainder = difference in client idle) | +0.42 µs/row | +16 % |

The engine-side subtotal is the difference in client idle (wall minus client CPU), so it does not depend on which sweep's server CPU is used.

Sizes behind the fit (median of 3, spread in parentheses; full table with every mode in `analysis-main.txt`; server CPU in `analysis-cpu.txt`):

| rows returned | KDS wall `values` | PG wall `values` | KDS client CPU `values` | PG client CPU `values` | KDS server CPU | PG server CPU | KDS wire | PG wire |
|---|---|---|---|---|---|---|---|---|
| 1 | 78.5 µs (1 %) | 87.8 µs (0 %) | 46.1 µs | 20.1 µs | 31.4 µs | 67.8 µs | 476 B | 434 B |
| 10 | 134.0 µs (2 %) | 122.8 µs (1 %) | 97.2 µs | 50.4 µs | 35.7 µs | 72.2 µs | 1,736 B | 1,230 B |
| 100 | 681.5 µs (1 %) | 466.7 µs (1 %) | 601.5 µs | 356.0 µs | 74.7 µs | 127.0 µs | 14,336 B | 9,428 B |
| 1,000 | 6,174 µs (1 %) | 3,560 µs (0 %) | 5,661 µs | 3,433 µs | 490.6 µs | 641.4 µs | 140,356 B | 93,874 B |
| 10,000 | 61,928 µs (1 %) | 35,732 µs (1 %) | 57,570 µs | 35,496 µs | 4,310 µs | 5,774 µs | 1,400,546 B | 955,884 B |
| 60,480 | 400,107 µs (0 %) | 238,833 µs (0 %) | 372,984 µs | 238,504 µs | 28,586 µs | 32,828 µs | 8,468,826 B | 5,891,937 B |

These are per statement, one connection. At 1 row KDS is faster (78.5 against 87.8 µs, a fixed-cost advantage of 36 µs of server CPU); from 100 rows the decoder
decides the order. The ratio of wall time (KDS / PG, `values`) is 0.89x, 1.09x, 1.46x, 1.73x, 1.73x, 1.68x at 1, 10, 100, 1,000, 10,000, 60,480 rows: it is a per-row cost, flat
from 1,000 rows up. Spreads: every `values` wall and client-CPU cell is at or below 5 % (PG `drain` wall at 1 row is 10 %, and PG `drain` client CPU at 100 rows 11 %; neither carries weight in the slopes).

## 4. Latency distributions

Per-statement wall time in µs, pooled over the 3 repeats (n = statements). `values` mode is the like-for-like read; `drain` is the same reply with no cell decoded.

| mode | rows | engine | n | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| values | 1 row | KDS | 9,000 stmts | 74 µs | 76 µs | 77 µs | 88 µs | 96 µs |
| values | 1 row | PG | 9,000 stmts | 84 µs | 85 µs | 86 µs | 102 µs | 105 µs |
| values | 100 rows | KDS | 6,000 stmts | 660 µs | 674 µs | 680 µs | 701 µs | 725 µs |
| values | 100 rows | PG | 6,000 stmts | 448 µs | 462 µs | 466 µs | 482 µs | 498 µs |
| values | 1,000 rows | KDS | 600 stmts | 5,990 µs | 6,084 µs | 6,133 µs | 6,262 µs | 7,308 µs |
| values | 1,000 rows | PG | 600 stmts | 3,500 µs | 3,528 µs | 3,541 µs | 3,612 µs | 3,673 µs |
| values | 10,000 rows | KDS | 60 stmts | 60,098 µs | 60,982 µs | 61,511 µs | 63,700 µs | 67,187 µs |
| values | 10,000 rows | PG | 60 stmts | 34,583 µs | 35,162 µs | 35,418 µs | 37,560 µs | 37,932 µs |
| values | 60,480 rows | KDS | 60 stmts | 389,972 µs | 395,923 µs | 399,155 µs | 409,896 µs | 411,642 µs |
| values | 60,480 rows | PG | 60 stmts | 232,737 µs | 235,544 µs | 237,662 µs | 247,048 µs | 247,742 µs |
| drain | 1 row | KDS | 9,000 stmts | 45 µs | 46 µs | 47 µs | 56 µs | 64 µs |
| drain | 1 row | PG | 9,000 stmts | 71 µs | 81 µs | 82 µs | 117 µs | 132 µs |
| drain | 1,000 rows | KDS | 600 stmts | 489 µs | 501 µs | 505 µs | 532 µs | 631 µs |
| drain | 1,000 rows | PG | 600 stmts | 633 µs | 646 µs | 654 µs | 680 µs | 701 µs |
| drain | 10,000 rows | KDS | 60 stmts | 4,228 µs | 4,276 µs | 4,288 µs | 4,392 µs | 4,536 µs |
| drain | 10,000 rows | PG | 60 stmts | 5,733 µs | 5,759 µs | 5,786 µs | 5,902 µs | 5,982 µs |
| drain | 60,480 rows | KDS | 60 stmts | 28,034 µs | 28,230 µs | 28,418 µs | 28,833 µs | 28,952 µs |
| drain | 60,480 rows | PG | 60 stmts | 32,496 µs | 32,621 µs | 32,973 µs | 33,580 µs | 33,711 µs |

The distributions are tight at 10,000 rows and above (p99 within 10 % of p50); at 1,000 rows KDS `values` p99 is 19 % above p50, and at 1 row the `drain` p99 is 36 % (KDS) and 61 % (PG) above p50. With the decode removed KDS is faster at every size,
at p0 as at p99; the order flips only with the decode in.

## 5. What the decode costs, by function

`cProfile` of 30 statements of 1,000 rows each (`profile-size1000.txt`). Profiled seconds are inflated by the profiler and are used for shares and call counts only.

| | KDS `split` | PostgreSQL `split` |
|---|---|---|
| Python function calls per returned row (12 cells) | 85.4 calls | 34.1 calls |
| Profiled time, 30 statements | 0.540 s | 0.256 s |
| Top function | `kwp._Reader._take`, 722,370 calls, 33 % own time (2 per cell) | `pg_wire._decode_data_row`, 30,000 calls, 48 % own time |
| Next | `kwp.Connection._row_batch` own, 25 %; `_Reader.i32`, 360,000 calls, 17 % own (237 ms cumulative with its `_take`) | `struct.Struct.unpack`, 420,120 calls, 16 % |
| Calls per cell | `i32` + 2 `_take` + 2 `len` + `append` + `struct.unpack` | one `unpack`, one `append` |

`kwp._row_batch` reads each cell through two `_Reader` method calls, and each of those calls another (`_take`, then `struct.unpack` for the length). `pg_wire._decode_data_row` does the
same job in a single loop: per cell a 4-byte slice, one `struct.unpack` and a value slice, with no bounds check. The 2.5x difference in function calls per cell matches the 2x difference in client CPU.

**Control.** `Kds.inline` in `rowcost.py` reads the same frames into a list of lists of `bytes` with `_row_batch`'s inner loop written as one loop (the
frame loop is unchanged `kwp._recv_frame`). It is not `_decode_data_row`'s shape cell for cell: it reads the length with `unpack_from` at an offset, so
it skips the 4-byte slice `_decode_data_row` allocates per cell, and it drops `_take`'s truncation check, which `pg_wire` does not have either. A negative length
is NULL there (`n < 0`) and in `_row_batch` (`n == -1`), the same on any valid reply; the data holds no NULL, so neither path ran. The driver checks the control's
row count (equal to N in every cell) but not that its cells equal `_row_batch`'s. Slopes over 1 to 10,000 rows, 3 repeats (`analysis-inline.txt`):

| | wall | client CPU | server CPU (short windows) | client idle (wall - client CPU) |
|---|---|---|---|---|
| KDS `split` (`kwp.execute`) | 5.54 µs/row | 5.11 µs/row | 0.50 µs/row | 0.43 µs/row |
| KDS `split`, inlined loop | **2.56 µs/row** | **2.14 µs/row** | 0.45 µs/row | 0.42 µs/row |
| PostgreSQL `split` | 2.88 µs/row | 2.86 µs/row | 0.54 µs/row | 0.01 µs/row |

The server-CPU column comes from this sweep's 0.15 to 7 s windows (spreads up to 22 %), so the 0.05 µs difference between the two KDS rows is noise. The
`pg inline` rows in `analysis-inline.txt` are `pg_wire.fetch` again (`Pg.run` has no `inline` branch) and repeat `pg split`.

Per-statement wall time of the control, pooled over the 3 repeats (computed from `cells-inline.jsonl` at review; `analyze.py` prints no percentiles for these modes):

| mode | rows | engine | n | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| split | 1 row | KDS | 9,000 stmts | 74 µs | 75 µs | 76 µs | 88 µs | 98 µs |
| inline | 1 row | KDS | 9,000 stmts | 48 µs | 50 µs | 50 µs | 59 µs | 66 µs |
| split | 1 row | PG | 9,000 stmts | 83 µs | 85 µs | 85 µs | 102 µs | 113 µs |
| split | 1,000 rows | KDS | 600 stmts | 5,365 µs | 5,447 µs | 5,475 µs | 5,621 µs | 6,208 µs |
| inline | 1,000 rows | KDS | 600 stmts | 2,442 µs | 2,487 µs | 2,508 µs | 2,580 µs | 2,827 µs |
| split | 1,000 rows | PG | 600 stmts | 2,732 µs | 2,765 µs | 2,787 µs | 2,823 µs | 2,903 µs |
| split | 10,000 rows | KDS | 60 stmts | 54,159 µs | 54,913 µs | 55,392 µs | 57,512 µs | 57,683 µs |
| inline | 10,000 rows | KDS | 60 stmts | 24,657 µs | 25,252 µs | 25,763 µs | 28,397 µs | 29,538 µs |
| split | 10,000 rows | PG | 60 stmts | 27,528 µs | 28,086 µs | 28,550 µs | 30,764 µs | 31,320 µs |
| split | 60,480 rows | KDS | 60 stmts | 351,950 µs | 357,483 µs | 359,891 µs | 369,017 µs | 370,898 µs |
| inline | 60,480 rows | KDS | 60 stmts | 174,809 µs | 177,848 µs | 181,544 µs | 190,094 µs | 191,018 µs |
| split | 60,480 rows | PG | 60 stmts | 188,847 µs | 192,170 µs | 193,890 µs | 202,870 µs | 204,475 µs |

With one function's inner loop changed, KDS's client CPU per row falls 58 %. That makes the claim that the decoder is the cause of most of the gap a measured one
and not an inference from the profile. It does not show that KDS is cheaper than PostgreSQL with matched decoders: the control does one slice per cell less than
`_decode_data_row`, and the KDS wall still carries the 0.42 µs per row of unoverlapped server time. (The `split` row differs by 0.07 µs from section 3's because this
is a separate run of 3 repeats.)

## 6. Verdict

- **Cause of the 2x per-row gap: the client decoder, about 84 % of it; the engine's reply path, about 16 %.** Net client CPU is +2.20 µs of the 2.62 µs wall gap (84 %).
  The engine-side +0.42 µs (16 %) is the engine's server CPU, -0.14 µs (-5 %: KDS is cheaper), plus 0.56 µs (21 %) of overlap that PostgreSQL gets and KDS does not,
  because a KWP reply is put on the wire only at statement completion (section 3). The engine's per-row CPU is not part of the gap; the timing of its reply is.
- The decoder cost is `tools/kwp.py`'s `_Reader` call chain in `_row_batch` (2 `_take` calls and an `i32` call per cell): about 5.2 µs per row against 2.6 for `pg_wire`'s equivalent.
  Both decoders are plain Python; neither is what an application driver would use, so none of the 5.8 and 3.6 µs figures is a statement about either engine's driver ecosystem.
- **Engine side.** Server CPU per returned row is 0.43 µs (KDS, binary, 140 B per row) against 0.57 µs (PostgreSQL, text, 97 B per row), 0.75x over 1 to 10,000 rows
  and 0.88x over 100 to 60,480; 7 % of KDS's wall per row and 16 % of PostgreSQL's.
  KDS's fixed server cost per statement is also lower (39 µs against 69 µs of CPU, from the fixed-window sweep's intercepts).
  Nothing here is evidence of an engine-side per-row CPU problem, so the `kds_sampler` break-down was not run (it also needs a frame-pointer build of this commit, which does not exist).
  What is engine-side is that the client waits for the whole reply: KDS's 0.43 µs per row of server CPU is all on the wall, PostgreSQL's 0.57 is almost all hidden.
  The design expectation checked is the one section 4 of the scenario 1 file left open ("not separated whether the cost lives in the engine or in the two Python wire decoders"); this answers it.
- **What the scenario 1 table should not be read as.** The 2.1x to 2.3x there came through the scenario drivers' own clients and is not re-run here; with the two bare decoders the ratio is
  1.74x (`values`) and 1.95x (`split`). Both exceed 1 for the same reason, and most of the number is the decoder pair's, not the engine's. With no decode KDS costs less wall time per
  row (0.43 against 0.57 µs). With a client whose CPU per row matched PostgreSQL's and exceeded the server's, KDS would cost about 0.42 µs per row more, the
  unoverlapped server time; the control's 2.56 against 2.88 µs is below PostgreSQL only because its client CPU is 0.72 µs per row lower (2.14 against 2.86).

## 7. Limits

- Single synchronous connection, `cores = 1`; the engine's multi-connection or multi-core read throughput is not measured here.
- PostgreSQL ran in text format through the simple query protocol because `pg_wire.py` supports nothing else; a binary-format PostgreSQL would move bytes per row and PG's decode, not KDS's.
- Server CPU is process-level `/proc` ticks (10 ms), measured over windows of 2.6 to 5.5 s in `drain` mode only; sub-0.05 µs differences are not resolved. KDS's figure includes
  every thread of the server process, PostgreSQL's the backend alone (section 2).
- The cause of the unoverlapped server time is read from the source (section 3), not separated by a measurement: no run streamed a KWP reply.
- Host load was recorded at the start only, not per cell (`bench/README.md` rule 4).
- The control rewrites the decode loop for the measurement only; no tool was changed. Whether `kwp._row_batch` should be changed is the operator's call.
- Not done: `kds_sampler` (not needed, see section 6; no frame-pointer build exists for this commit and building one was not worth a minute of shared host); a fit with a wider variety of column counts, so whether the
  cost per cell or per row is the unit of the decoder is inferred from the call counts, not swept; `LIMIT` shapes (only key ranges were run).
- Time: from the first command at 06:16 to the last measurement at 06:26 UTC, about 10 minutes of wall-clock including set-up. Nothing was cut after the operator lifted the budget; the first sweep's
  server-CPU windows were too short and were re-measured (section 2), and the 60,480-row point was added.

Archive: `bench/v3.3.0/archive/row-cost-attribution-v3.3.0-37-gf829964c/` - `rowcost.py` (driver; `load`, `run`, `prof`, `idle`), `run.sh`, `start.sh`, `watchdog.sh`,
`analyze.py`, raw cells `cells-main.jsonl` (3 modes, short windows), `cells-cpu.jsonl` (`drain`, 2.6 to 5.5 s windows), `cells-inline.jsonl` (decoder control), the analyses, `profile-size1000.txt`, `kds.conf`, `watchdog.log`.
Re-run: `start.sh`, create database `rowcost` on the twin, `run.sh load --kds-port 15690 --pg-port 15434`, then `run.sh run ...` as `watchdog.sh` shows for the main sweep.
`watchdog.sh` and `watchdog.log` cover the main sweep only; the other two sweeps' command lines were not archived, and the cells show their arguments:
`--modes drain --target-s 3` for `cells-cpu.jsonl` and `--modes split,inline` for `cells-inline.jsonl`, each with the main sweep's ports, pid and `--reps 3`.
