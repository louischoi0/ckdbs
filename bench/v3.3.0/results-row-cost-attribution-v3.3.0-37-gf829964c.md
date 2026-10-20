# Where the per-returned-row cost goes: KDS against PostgreSQL 18.6, at `v3.3.0-37-gf829964c`

**Finding.** The 2x gap in cost per returned row between KDS and PostgreSQL is in the Python wire decoder in `tools/kwp.py`, not in the engine.
On a 12-column `daily_stats` read by tuple-key range, KDS's server spends **0.43 µs of CPU per returned row against PostgreSQL's 0.57 µs**
(KDS at 0.75x), while the client spends **5.8 µs per row decoding KDS's reply against 3.6 µs decoding PostgreSQL's**
(1.62x). A control that rewrites only `kwp._row_batch`'s inner loop into the single-function shape `pg_wire._decode_data_row`
already has takes KDS's client CPU from 5.18 to 2.14 µs per row, and KDS's wall slope falls to 2.56 µs per row against
PostgreSQL's 2.87 (0.89x). No sampler break-down of the server was run, because the server is not where the gap is (section 5).

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
| Host | AMD EPYC 9V74, 8 logical CPUs, Linux 7.0.0-1014-azure; quiet at start (load average 0.07, no `cc1plus`, `ctest` or `kds_tests`); no build ran during the measurement |
| Device | data file `/home/cdkbs/bench-runs/row-cost/data/kds.db` on `/dev/root`, `ext4` (`df -T`), never tmpfs; deleted afterwards. Reads only, so durability does not enter the numbers |
| Data | `daily_stats`, scenario 1's schema (`id, bar_id, symbol_id, session_no int32`, eight `int64` feature columns), 60,480 rows, tuple keys 1..60,480, loaded with 1,000-row multi-row `INSERT`s by the same generator into both engines. KDS `BTREE`; PostgreSQL `PRIMARY KEY (id)` plus `ANALYZE` |
| Query | `SELECT * FROM daily_stats WHERE id BETWEEN 1 AND N`, N = the returned row count: 1, 10, 100, 1,000, 10,000, 60,480. Mapping to the scenario: 60,480 is its default `--years 30 --symbols 8` bar count. Neither engine scans: both walk the key range |
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
over 1 to 10,000 rows (the fit over 100 to 60,480 is in the archive, within 12 % of it for the slopes of table 1, and that its intercepts go negative,
which is a cache effect at 60,480 rows and why the 1..10,000 fit is the one quoted). `/proc` ticks are 10 ms, so the first sweep (windows of 0.1 to 1 s) gave
KDS server-CPU spreads of 6 to 18 % at 100 to 1,000 rows; **the server-CPU column is from a second sweep with 3 s windows** (`drain` mode,
`--target-s 3`), whose spreads are 0 to 3 %. KDS server CPU at idle is 0.7 ms per second (its 50 ms tick).

## 3. The component table

Microseconds per returned row, slope over 1 to 10,000 rows, median of 3 repeats per size. `values` mode is the like-for-like read (client holds Python ints).

| component | KDS | PostgreSQL 18.6 | KDS / PG |
|---|---|---|---|
| **Wall slope, `values`** | 6.19 µs/row | 3.57 µs/row | **1.74x** |
| Server CPU (fixed-window sweep) | 0.43 µs/row | 0.57 µs/row | **0.75x** |
| Client CPU, total | 5.76 µs/row | 3.55 µs/row | **1.62x** |
| - receive (`drain` CPU: syscalls plus header parse) | 0.02 | 0.27 | 0.07x |
| - split cells into `bytes` objects (`split` minus `drain`) | 5.17 | 2.59 | **1.99x** |
| - convert cells to ints (`values` minus `split`) | 0.57 | 0.69 | 0.82x |
| Remainder = wall - server - client | +0.00 | -0.56 | overlap |
| Wall slope, `split` | 5.61 µs/row | 2.87 µs/row | 1.95x |
| Wall slope, `drain` (Python decode removed) | 0.43 µs/row | 0.57 µs/row | 0.75x |
| Wire bytes per returned row | 140.0 B | 97.4 B (text) | 1.44x |
| Fixed cost per statement, `drain` wall intercept | 57 µs | 86 µs | 0.66x |

Reading it. With the Python decode removed (`drain`) KDS is the faster engine per row, and its wall slope equals its server CPU slope: the server alone bounds it. A negative
remainder means server and client work overlap in time: PostgreSQL's backend and the client overlap 0.56 of the backend's 0.57 µs per row; KDS's do not overlap at all
(wall 6.19 = 0.43 + 5.76 within 0.01). That is measured; why KDS does not overlap was not separated. Bytes on the wire go the other way from cost: KDS sends 44 % more bytes
per row (12 length-prefixed binary cells) and still costs the client less to receive, because its frames carry about 460 rows each and PostgreSQL's carry one.

Where the 2.62 µs/row wall gap goes (6.19 - 3.57):

| part of the gap | µs/row | share of gap |
|---|---|---|
| server CPU | -0.14 | -5 % |
| client: receive | -0.25 | -10 % |
| client: split into `bytes` | +2.58 | +98 % |
| client: convert to int | -0.12 | -5 % |
| remainder (overlap PostgreSQL has and KDS does not) | +0.56 | +21 % |
| **sum** | **+2.62** | 100 % |

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
| values | 1 | KDS | 9,000 | 74 | 76 | 77 | 88 | 96 |
| values | 1 | PG | 9,000 | 84 | 85 | 86 | 102 | 105 |
| values | 100 | KDS | 6,000 | 660 | 674 | 680 | 701 | 725 |
| values | 100 | PG | 6,000 | 448 | 462 | 466 | 482 | 498 |
| values | 1,000 | KDS | 600 | 5,990 | 6,084 | 6,133 | 6,262 | 7,308 |
| values | 1,000 | PG | 600 | 3,500 | 3,528 | 3,541 | 3,612 | 3,673 |
| values | 10,000 | KDS | 60 | 60,098 | 60,982 | 61,511 | 63,700 | 67,187 |
| values | 10,000 | PG | 60 | 34,583 | 35,162 | 35,418 | 37,560 | 37,932 |
| values | 60,480 | KDS | 60 | 389,972 | 395,923 | 399,155 | 409,896 | 411,642 |
| values | 60,480 | PG | 60 | 232,737 | 235,544 | 237,662 | 247,048 | 247,742 |
| drain | 1 | KDS | 9,000 | 45 | 46 | 47 | 56 | 64 |
| drain | 1 | PG | 9,000 | 71 | 81 | 82 | 117 | 132 |
| drain | 1,000 | KDS | 600 | 489 | 501 | 505 | 532 | 631 |
| drain | 1,000 | PG | 600 | 633 | 646 | 654 | 680 | 701 |
| drain | 10,000 | KDS | 60 | 4,228 | 4,276 | 4,288 | 4,392 | 4,536 |
| drain | 10,000 | PG | 60 | 5,733 | 5,759 | 5,786 | 5,902 | 5,982 |
| drain | 60,480 | KDS | 60 | 28,034 | 28,230 | 28,418 | 28,833 | 28,952 |
| drain | 60,480 | PG | 60 | 32,496 | 32,621 | 32,973 | 33,580 | 33,711 |

The distributions are tight at 10,000 rows and above (p99 within 10 % of p50); at 1,000 rows KDS `values` p99 is 19 % above p50, and at 1 row the `drain` p99 is 36 % (KDS) and 61 % (PG) above p50. With the decode removed KDS is faster at every size,
at p0 as at p99; the order flips only with the decode in.

## 5. What the decode costs, by function

`cProfile` of 30 statements of 1,000 rows each (`profile-size1000.txt`). Profiled seconds are inflated by the profiler and are used for shares and call counts only.

| | KDS `split` | PostgreSQL `split` |
|---|---|---|
| Python function calls per returned row (12 cells) | 85.4 | 34.1 |
| Profiled time, 30 statements | 0.540 s | 0.256 s |
| Top function | `kwp.Reader._take`, 722,370 calls, 33 % own time (2 per cell) | `pg_wire._decode_data_row`, 30,000 calls, 48 % own time |
| Next | `kwp.Connection._row_batch` own, 25 %; `_Reader.i32`, 360,000 calls, 17 % own (237 ms cumulative with its `_take`) | `struct.Struct.unpack`, 420,120 calls, 16 % |
| Calls per cell | `i32` + 2 `_take` + 2 `len` + `append` + `struct.unpack` | one `unpack`, one `append` |

`kwp._row_batch` reads each cell through two `_Reader` method calls, and each of those calls another (`_take`, then `struct.unpack` for the length). `pg_wire._decode_data_row` does the
same job in a single loop with one `struct.unpack` and a slice per cell. The 2.5x difference in function calls per cell matches the 2x difference in client CPU.

**Control.** `Kds.inline` in `rowcost.py` reads the same frames and returns the same list of lists of `bytes`, with `_row_batch`'s inner loop written like `_decode_data_row`'s (the
frame loop is unchanged `kwp._recv_frame`). Slopes, µs per row over 1 to 10,000 rows, 3 repeats (`analysis-inline.txt`):

| | wall | client CPU | server CPU |
|---|---|---|---|
| KDS `split` (`kwp.execute`) | 5.54 | 5.11 | 0.50 |
| KDS `split`, inlined loop | **2.56** | **2.14** | 0.45 |
| PostgreSQL `split` | 2.88 | 2.86 | 0.54 |

With one function's inner loop changed, KDS's client cost per row falls 58 % and it is no longer above PostgreSQL's (wall 0.89x, client CPU 0.75x). That makes the claim
that the decoder is the cause a measured one and not an inference from the profile. (The `split` row differs by 0.07 µs from table 3's because this is a separate run of 3 repeats.)

## 6. Verdict

- **Cause of the 2x per-row gap: the client decoder, about 84 % of it; the engine contributes none of it.** Net client CPU is +2.20 µs of the 2.62 µs wall gap (84 %), the engine's server
  CPU is -0.14 µs (-5 %: KDS is cheaper), and 0.56 µs (21 %) is overlap of server and client work that PostgreSQL gets and KDS does not (not separated further).
- The decoder cost is `tools/kwp.py`'s `_Reader` call chain in `_row_batch` (2 `_take` calls and an `i32` call per cell): about 5.2 µs per row against 2.6 for `pg_wire`'s equivalent.
  Both decoders are plain Python; neither is what an application driver would use, so none of the 5.8 and 3.6 µs figures is a statement about either engine's driver ecosystem.
- **Engine side.** Server CPU per returned row is 0.43 µs (KDS, binary, 140 B per row) against 0.57 µs (PostgreSQL, text, 97 B per row); 7 % of KDS's wall per row and 16 % of PostgreSQL's.
  KDS's fixed server cost per statement is also lower (39 µs against 69 µs of CPU, from the fixed-window sweep's intercepts).
  Nothing here is evidence of an engine-side per-row problem, so the `kds_sampler` break-down was not run (it also needs a frame-pointer build of this commit, which does not exist).
  The design expectation checked is the one section 4 of the scenario 1 file left open ("not separated whether the cost lives in the engine or in the two Python wire decoders"); this answers it.
- **What the scenario 1 table should not be read as.** The 2.1x to 2.3x there came through the scenario drivers' own clients and is not re-run here; with the two bare decoders the ratio is
  1.74x (`values`) and 1.95x (`split`). Both exceed 1 for the same reason, but the number is the decoder pair's, not the engine's. KDS rows returned to a client with a decoder that is
  not slower than PostgreSQL's would cost less wall time per row than PostgreSQL's (0.43 against 0.57 µs with no decode, 2.56 against 2.88 with the inlined control).

## 7. Limits

- Single synchronous connection, `cores = 1`; the engine's multi-connection or multi-core read throughput is not measured here.
- PostgreSQL ran in text format through the simple query protocol because `pg_wire.py` supports nothing else; a binary-format PostgreSQL would move bytes per row and PG's decode, not KDS's.
- Server CPU is process-level `/proc` ticks (10 ms), measured over windows of at least 3 s; sub-0.05 µs differences are not resolved. KDS's figure includes every thread of the server process.
- The control rewrites the decode loop for the measurement only; no tool was changed. Whether `kwp._row_batch` should be changed is the operator's call.
- Not done: `kds_sampler` (not needed, see section 6; no frame-pointer build exists for this commit and building one was not worth a minute of shared host); a fit with a wider variety of column counts, so whether the
  cost per cell or per row is the unit of the decoder is inferred from the call counts, not swept; `LIMIT` shapes (only key ranges were run).
- Time: from the first command at 06:16 to the last measurement at 06:26 UTC, about 10 minutes of wall-clock including set-up. Nothing was cut after the operator lifted the budget; the first sweep's
  server-CPU windows were too short and were re-measured (section 2), and the 60,480-row point was added.

Archive: `bench/v3.3.0/archive/row-cost-attribution-v3.3.0-37-gf829964c/` - `rowcost.py` (driver; `load`, `run`, `prof`, `idle`), `run.sh`, `start.sh`, `watchdog.sh`,
`analyze.py`, raw cells `cells-main.jsonl` (3 modes, short windows), `cells-cpu.jsonl` (`drain`, 3 s windows), `cells-inline.jsonl` (decoder control), the analyses, `profile-size1000.txt`, `kds.conf`, `watchdog.log`.
Re-run: `start.sh`, create database `rowcost` on the twin, `run.sh load --kds-port 15690 --pg-port 15434`, then `run.sh run ...` as `watchdog.sh` shows.
