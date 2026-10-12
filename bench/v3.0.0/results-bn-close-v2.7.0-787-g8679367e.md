# BN's close: the read and update paths, measured against the commit BN opens from

**Thesis.** Against `v2.7.0-769-gb70f79d8` (A, BN's opening commit), `v2.7.0-787-g8679367e` (B, BN's closing
commit) is faster in every statement shape measured, at one session, `cores = 1`, `relaxed`, 1,000-row relations
(10,000 for the scan). **The largest win is the smallest change: a statement's reply leaves in one `send()` instead
of two (BN-S4).** The point `SELECT`, `UPDATE` (literal and `v + 1`), `INSERT` and `DELETE` are 7.2-9.4 µs lower at
p50 (13.9-17.5 %, +16 to +21 % statements per second), the server's CPU per statement is 6.3-7.5 µs lower and the
client's 8.8-9.8 µs, in 8 of 8 runs; the `C_PING` control, which has no early flush to lose, did not move (+0.01 µs
at p50, +0.02 µs of server CPU). **An `UPDATE` of a row with a spilled `varchar` is 16.6-17.0 µs (26-29 %) lower and one
assigning the spilled value 14.1-15.2 µs (20-24 %) lower, and a thousand updates of a spilled row that does not assign
the cell allocate 0 var-heap pages on B where they allocated 38 on A in all 14 runs.** The every-row `UPDATE` is 228-260 µs per 1,000 rows lower (22-24 %,
764 ns a row at p50 against 987), 96 % of it inside the per-row visitor. **The scan moved least: 37 µs per 10,000 rows
(4.9 %), 3 ns of a 67 ns row, so a KDS scan row is still 1.96 times PostgreSQL's 34.3 ns** (A: 2.03). What is left of
a point statement is the request path: in B's profile a point `UPDATE` spends 59 % of its 24.2 µs of server CPU in
the socket's `send` and `read`, and **its own storage work - undo, WAL, locks, descent, decode - is 2.3 µs, 10 % of
the CPU and 5 % of the wall time**. Where one stage's share of a delta can be read from the profile it is said;
where two stages touch one function it is not, because no binary between A and B was measured.

- **A** = `b70f79d8`, `v2.7.0-769-gb70f79d8`: BN's opening commit, engine code identical to `ac04b84e`
  (`bench/v3.0.0/results-bn-s1-census-v2.7.0-769-gb70f79d8.md`).
- **B** = `8679367e`, `v2.7.0-787-g8679367e`: BN-S4 (`fd851d32`, one write per reply), BN-S5 (`ed4a5655`, carried
  spills), BN-S9 (`e6b320b4`, the var-heap tail), BN-S3 (`22f07942` partial decode and one undo hold, `3a5081e5`
  `ExecStats::For` inline) and origin/main merged at the close. **B also carries four `main` engine commits A does not**
  (`git log b70f79d8..8679367e -- src include`): `696a4a38` (one Keystone-id test for a named pk and a foreign-key
  value; `command_dispatcher.cpp`, on the path of the census's `INSERT`, which names its pk), `31dac324` (an integer
  literal over a decimal; five lines of `set_expr.cpp`), `53e7d505` and `6bbd0711` (aggregate scale, no measured shape).
  None is on the send, undo or var-heap path, but no merge-free B was measured, so their share of any delta is not
  separated, and the `C_PING` control cannot see them; the `INSERT`'s server-CPU fall is the smallest of the point
  statements (6.3 µs), inside the repeat floor of section 7.
- Drivers, as the branch's `tools/` at the commit that adds this file: `tools/bn_census.py` (its docstring is the flag
  reference; `bench/docs/` stays closed, `bench/README.md`), `tools/bn_census_table.py`, `tools/kds_sampler.c` with
  `tools/kds_sampler_report.py` and `tools/bn_parts.json`, `tools/bn_pg_scan.py`.
- Raw output: `archive/bn-close-v2.7.0-787-g8679367e/` - `ab.json` and `ab-delete.json` (per run, per arm), the two
  table dumps, `profA.json`/`profB.json` with their top-25 reports and parts tables, `pg.json`, `binaries.txt`. The
  drivers' console logs (`ab.log`, `ab-delete.log`, `profA.log`, `profB.log`) were archived after this file was written;
  the sampler's raw dumps were left in `/home/cdkbs/bench-runs/bk/close/` and are not archived.

| Item | Value |
|---|---|
| Executed | 2026-10-11, from the output files' modification times: binaries copied 08:57:10 UTC; A/B census 08:57-08:59 (`ab.json` last written 08:59:16); B's profile ends 09:00:05, A's 09:00:18; PostgreSQL twin 09:01:07; DELETE supplement ends 09:02:01 |
| Worktree / branch | `bk-read-update-paths`, `worktree-bk-read-update-paths`; this file is the only file its author wrote |
| A | `b70f79d8`, `v2.7.0-769-gb70f79d8`, committed 2026-10-11 07:01:07 UTC. Its build tree no longer exists (`build-release` now holds B), so **the source binary's mtime is not available**: the copy's mtime, 07:11:35 UTC, is the copy time, 10 minutes after the commit; the copy's sha256 is the one the S1 census file names for its A, which that file states was built from `b70f79d8` before any `src/` change of BN |
| B | `8679367e`, `v2.7.0-787-g8679367e`, committed 08:50:45 UTC. **Source binary** `build-release/kds_server` of this worktree: mtime 08:56:49 UTC, 6 minutes **newer than its commit**, sha256 equal to the copy's (checked again at 09:27 UTC); copy made 08:57:10. **`git diff HEAD -- src include` was empty when checked at 09:06 UTC**, after the build and the measurement, so the binary was built from HEAD's sources; the working tree has changed since (next row) |
| Tree | **Dirty.** At 09:27 UTC `git status` lists 25 modified files: documents, `tools/bn_census.py`, `tools/bn_pg_scan.py` and **14 under `src/`, `include/` and `tests/` that were not modified at 09:06 UTC**, plus untracked archive files. The measured binaries predate every one of them (built 08:56:49 UTC, measured by 09:02:01), so none of those edits is in a number here. **The drivers that produced `ab-delete.json` and `pg.json` are the branch's `tools/` at the commit that adds this file, not `8679367e`'s**: `tools/bn_census.py`'s `--only` flag and `delete-pk` shape and `tools/bn_pg_scan.py`'s `(rows, error)` return were uncommitted edits when the runs were taken and are committed with this file. `ab.json`'s arguments carry no `only` key and it has no delete arm, so it ran before those edits |
| Measured copies | `/home/cdkbs/bench-runs/bk/bin/`: `kds_server-A-b70f79d8` `b48117d162cb499523ab7d371adecf4529f49df784e3c19b9291ee76201b9bbc`; `kds_server-B-8679367e` `d57464e631a831e9634a0bb6e7af63386d73ffc1b83d7c9e6869e316c57a2096`; profiled `kds_server-Aprof-b70f79d8` `329e419df73119d0f5c6f18d330cb15731d8fb63b4a5a7f21fbcd8ee19212eaa`, `kds_server-Bprof-8679367e` `e403a390ae7fa01dfbddcf898bbe84e3c8b423d79366c4262effe66538fe8414` (source `build-prof/kds_server`, mtime 08:57:09 UTC), `kds_sampler.so` `df1e67bae6a60f47ef05cd916c94938215287195bdc802b04730c414b787379f`. Every server started from a copy |
| Build | Release (`CMAKE_BUILD_TYPE=Release` read from B's and the profiled copy's `CMakeCache.txt`; A's per the S1 file), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`; the profiled copies add `-DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer` |
| Host / device | 8 vCPU AMD EPYC 9V74, Linux 7.0.0-1014-azure; data files and WAL under `/home/cdkbs/bench-runs/bk/close/` on `/dev/root`, **ext4**, not tmpfs (`df -T`, checked when this file was written); server on CPU 2, driver on CPU 4 |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`; a fresh server and data file per binary per run, both servers of a run alive together on CPU 2 with one busy at a time; ports 31961/31962 (A/B census), 31991/31992 (supplement), 31971 (B's profile), 31981 (A's profile), PostgreSQL 15434 - none the default 15432 |
| Host load | **Not quiet, and not busy with builds.** 1-minute loadavg at the starts of the 8 census runs: 3.73 (after a 20 s wait for the gate at 4), 3.46, 3.09, 3.00, 3.02, 2.95, 3.05, 2.74; its 5-minute average fell 7.06 to 5.86, so the host had been busier just before; the supplement's six starts 1.10-1.32, the PostgreSQL runs 1.37-1.42; the profile passes carry no host record (the driver's profile path has none) and ran between the census and those. **Every recorded competing-process list held one entry, pid 1278524**, another session's idle `bash -c 'until [ -f build/tests/kds_tests ] && ! pgrep -f "cmake --build build" ...; do sleep 5; done; ...'` loop, which matches the `pgrep` pattern by its own text; the recorded command line was checked in every cell of `ab.json` and `ab-delete.json` and every run of `pg.json` (before and after), and no `cc1plus`, `cmake --build` or `ctest` process appears in any. The other-CPU busy fraction was not recorded by this driver. **59 arm-cells of the census read more than 12 % above their own median p50** (section 7); they hit A and B alike |
| Suite | No code changed for this measurement and **the suite was not run in this session**; the main session reported the full Debug suite green at `8679367e` (3521 of 3521), which this file did not re-check |

## What was run, and what was not

- **Interleaved A/B**, `python3 tools/bn_census.py --bin <A> --bin-b <B> --workdir ... --port 31961 --runs 8 --quiet-load 4 --quiet-wait 300`.
  Per run: a fresh server per binary, `t` and `r` (1,000 rows), `s` (10,000 rows), `sp` and `sa` (1,000 rows with a
  300-byte `varchar` cell, spilled, which is longer than the instance's inline cell), an empty `i`; then each shape over two
  driver paths - **literal** (an unnamed `C_PARSE` of the statement's text per execution, what every driver in `tools/`
  does) and **prepared** (one named `C_PARSE`, then BIND/EXECUTE). Every arm is cut into 12 blocks taken alternately by A
  and B, the first server of a block alternating, so host drift lands on both; the path order alternates per run (literal
  first in even runs). Arms: 3,000 point statements (250 a block), 60 every-row `UPDATE`s (5 a block), 192 scans (16 a
  block), per run and binary.
- **Rows.** `--rows 1000` sizes `t`, `r`, `sp`, `sa`; the every-row `UPDATE` (`WHERE id >= 1`) writes all 1,000 rows of `r`.
  `--scan-rows 10000` sizes `s`. **Row-set sizes of 200 and 10,000 for the point and every-row shapes and of 200 and
  1,000 for the scan were not run**, so a statement's fixed cost and its per-row cost are not separated by a sweep; the
  per-row figures below subtract a measured point statement's CPU and say so.
- **DELETE supplement**, `--only noop,delete-pk,select-pk`, 6 runs: the first census predated the `delete-pk` shape. Its
  `select-pk` and `noop` arms repeat the census's, which is the replicate of section 7. **Its `end_state` reads
  `range_v_ok: false` on both A and B (`ab-delete.json`, `tables-ab-delete.md`); that is not an error**: `--only` left
  the every-row arm unrun, so the read-back of `r` has no statement to match. Its `spill_txt_ok` is true on both.
- **Per arm the driver records** the client's wall time per statement (`time.perf_counter()` around the call), the
  server's on-CPU time (the sum over every server thread of `/proc/<pid>/task/*/schedstat`, kernel time of its syscalls
  included) per statement, the driver thread's own on-CPU time, the server CPU per block of 250, the host state, and
  every reply's tag; after the arms, `pages_allocated` from `SHOW META` around 1,000 updates of an unspilled and of a
  spilled row, and the end state of `r` and `sp`. **0 bad replies in 14 runs; `range_v_ok` and `spill_txt_ok` true on A
  and on B in all 8 census runs.**
- **Percentiles** are per run, and every table below carries the median over the runs of each. **The driver emits p90,
  not p95** (`tools/bn_census.py`'s `summarize`), so the tables carry p0, p25, p50, p90 and p99 and p95 cannot be derived
  from the output.
- **Deltas are paired per run (B - A)** and reported as the median with [min, max] over the runs; with 8 runs a 95 %
  interval would be no tighter than the range, so none is claimed. **Rates are derived**: `1,000,000 / p50` for a serial
  single-connection driver; the driver does not report one, and the `change in rate` column is the per-run
  `A p50 / B p50 - 1`.
- **Profile**, `LD_PRELOAD` `tools/kds_sampler.c` (SIGPROF at 1 kHz of process CPU time; `perf` is refused on this host,
  `perf_event_paranoid = 4`): B over every shape, 30,000 statements per point arm, 1,500 every-row `UPDATE`s, 2,000
  scans; A over the every-row `UPDATE` and the scan only. **A has no profile of any point statement in this run.** Each
  sample goes to the first part of `tools/bn_parts.json` that any frame of its chain matches, so **for the every-row
  `UPDATE` and the scan `descent and page access` is the whole row walk** (`btree::BtreeVisitLeafPage` is on every
  chain and matches before decode does). One sample is about 0.5 µs of a scan statement and 0.7 µs of an every-row one,
  so a 100-sample entry carries about ±5 µs; the profiled builds' wall times are not quoted.
- **PostgreSQL**, `python3 tools/bn_pg_scan.py --port 15434 --database bench --runs 5`: PG 18.6, the cluster
  `/home/cdkbs/pg-bench-ba` (`postgresql.conf` read: defaults except `port`, `listen_addresses`, the socket directory
  and logging off), the backend pinned to CPU 2, the simple query protocol, one literal statement per execution.

## 1. Point statements: one write per reply is the whole gain

Every point statement that earns a PARSE/BIND acknowledgement is 7.2-9.4 µs faster at p50 on both driver paths, in 8
of 8 runs, and the saving is the same size for a `SELECT`, an `INSERT`, a `DELETE` and an `UPDATE` (in the table
below the A and B columns are medians of the 8 per-run p50s; the paired column is the figure to quote). `SHOW META`
is the exception, 4.4-4.7 µs, and the `C_PING` frame is the control.

| statement | path | A: `b70f79d8` | B: `8679367e` | change in rate, paired per run: median [min, max] | runs B faster at p50 |
|---|---|---|---|---|---|
| `C_PING` frame (control: no statement, no dispatcher) | literal | 38,169 /s | 38,147 /s | 0.0 % [-1.9 %, +6.1 %] (control) | 4 of 8 |
| `SHOW META` | literal | 19,819 /s | 21,622 /s | +9.7 % [+8.2 %, +20.9 %] | 8 of 8 |
| `SHOW META` | prepared | 20,647 /s | 22,802 /s | +10.8 % [+8.4 %, +19.7 %] | 8 of 8 |
| `SELECT * FROM t WHERE id = ?` | literal | 15,276 /s | 17,696 /s | +16.4 % [+14.3 %, +21.4 %] | 8 of 8 |
| `SELECT * FROM t WHERE id = ?` | prepared | 15,783 /s | 18,601 /s | +17.5 % [+16.4 %, +18.9 %] | 8 of 8 |
| `UPDATE t SET v = ? WHERE id = ?` | literal | 18,585 /s | 21,880 /s | +17.3 % [+0.3 %, +20.5 %] | 8 of 8 |
| `UPDATE t SET v = ? WHERE id = ?` | prepared | 19,475 /s | 23,608 /s | +20.4 % [+14.3 %, +23.7 %] | 8 of 8 |
| `UPDATE t SET v = v + 1 WHERE id = ?` | literal | 17,655 /s | 20,891 /s | +17.3 % [+12.4 %, +21.0 %] | 8 of 8 |
| `UPDATE t SET v = v + 1 WHERE id = ?` | prepared | 18,637 /s | 22,618 /s | +21.2 % [+14.4 %, +22.8 %] | 8 of 8 |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | literal | 16,040 /s | 21,894 /s | +35.6 % [+31.4 %, +40.8 %] | 8 of 8 |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | prepared | 16,669 /s | 23,491 /s | +39.8 % [+31.1 %, +43.7 %] | 8 of 8 |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | literal | 15,396 /s | 19,669 /s | +24.8 % [+23.4 %, +31.1 %] | 8 of 8 |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | prepared | 16,035 /s | 21,636 /s | +32.4 % [+19.2 %, +36.2 %] | 8 of 8 |
| `INSERT INTO i VALUES (?, ?, ?)` | literal | 19,101 /s | 22,466 /s | +16.2 % [+5.9 %, +18.9 %] | 8 of 8 |
| `INSERT INTO i VALUES (?, ?, ?)` | prepared | 20,609 /s | 24,333 /s | +17.5 % [+11.1 %, +19.5 %] | 8 of 8 |
| `DELETE FROM d WHERE id = ?` (6-run supplement) | literal | 19,952 /s | 23,641 /s | +18.0 % [+8.3 %, +19.0 %] | 6 of 6 |
| `DELETE FROM d WHERE id = ?` (6-run supplement) | prepared | 21,358 /s | 25,275 /s | +18.2 % [+7.9 %, +19.2 %] | 6 of 6 |
| `UPDATE r SET v = ? WHERE id >= 1` (every one of 1,000 rows) | literal | 1,014 /s (1,013,557 rows/s) | 1,310 /s (1,309,538 rows/s) | +27.5 % [+21.8 %, +32.7 %] | 8 of 8 |
| `UPDATE r SET v = ? WHERE id >= 1` (every one of 1,000 rows) | prepared | 931 /s (931,115 rows/s) | 1,222 /s (1,222,307 rows/s) | +32.2 % [+29.2 %, +37.9 %] | 8 of 8 |
| `SELECT * FROM s WHERE w = ?` (10,000 rows, unindexed) | literal | 1,277 /s (12,771,841 rows/s) | 1,372 /s (13,720,151 rows/s) | +5.2 % [+3.7 %, +13.3 %] | 8 of 8 |
| `SELECT * FROM s WHERE w = ?` (10,000 rows, unindexed) | prepared | 1,305 /s (13,050,545 rows/s) | 1,380 /s (13,802,643 rows/s) | +5.1 % [+4.4 %, +10.4 %] | 8 of 8 |

The same data as paired p50 differences, with the CPU on both sides (server CPU is every server thread's on-CPU time per
statement; driver CPU is the Python client's own thread):

| statement | path | A p50 | B - A at p50, median [min, max] over runs | relative, median [min, max] | A server CPU | B - A server CPU, median [min, max] | A driver CPU | B - A driver CPU, median |
|---|---|---|---|---|---|---|---|---|
| `C_PING` frame (control: no statement, no dispatcher) | literal | 26.2 µs | +0.01 µs [-2.81 µs, +0.67 µs] | +0.0 % [-5.8, +2.0] | 12.2 µs | +0.02 µs [-0.44 µs, +0.22 µs] | 15.2 µs | +0.00 µs |
| `SHOW META` | literal | 50.5 µs | -4.44 µs [-11.32 µs, -3.82 µs] | -8.8 % [-17.3, -7.6] | 33.5 µs | -8.76 µs [-9.52 µs, -7.68 µs] | 29.8 µs | -5.27 µs |
| `SHOW META` | prepared | 48.4 µs | -4.72 µs [-10.25 µs, -3.85 µs] | -9.8 % [-16.4, -7.7] | 32.6 µs | -9.13 µs [-9.35 µs, -8.12 µs] | 28.0 µs | -5.41 µs |
| `SELECT * FROM t WHERE id = ?` | literal | 65.5 µs | -9.23 µs [-14.71 µs, -8.08 µs] | -14.1 % [-17.7, -12.5] | 35.7 µs | -7.43 µs [-7.97 µs, -5.81 µs] | 42.3 µs | -9.76 µs |
| `SELECT * FROM t WHERE id = ?` | prepared | 63.4 µs | -9.44 µs [-12.87 µs, -9.01 µs] | -14.9 % [-15.9, -14.1] | 35.0 µs | -6.50 µs [-6.90 µs, -5.51 µs] | 39.4 µs | -9.68 µs |
| `UPDATE t SET v = ? WHERE id = ?` | literal | 53.8 µs | -7.88 µs [-9.30 µs, -0.34 µs] | -14.7 % [-17.0, -0.3] | 31.9 µs | -7.45 µs [-7.86 µs, -5.88 µs] | 34.3 µs | -9.63 µs |
| `UPDATE t SET v = ? WHERE id = ?` | prepared | 51.3 µs | -8.67 µs [-10.03 µs, -7.90 µs] | -16.9 % [-19.2, -12.5] | 30.0 µs | -6.61 µs [-8.57 µs, -6.11 µs] | 31.9 µs | -9.58 µs |
| `UPDATE t SET v = v + 1 WHERE id = ?` | literal | 56.6 µs | -8.53 µs [-9.89 µs, -7.47 µs] | -14.8 % [-17.4, -11.0] | 33.6 µs | -6.38 µs [-7.66 µs, -5.63 µs] | 34.3 µs | -9.60 µs |
| `UPDATE t SET v = v + 1 WHERE id = ?` | prepared | 53.7 µs | -9.39 µs [-10.01 µs, -8.36 µs] | -17.5 % [-18.6, -12.6] | 32.7 µs | -7.09 µs [-8.09 µs, -4.36 µs] | 31.5 µs | -9.70 µs |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | literal | 62.3 µs | -16.56 µs [-24.81 µs, -14.25 µs] | -26.2 % [-29.0, -23.9] | 41.6 µs | -15.95 µs [-19.21 µs, -12.61 µs] | 35.1 µs | -9.93 µs |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | prepared | 60.0 µs | -17.03 µs [-18.38 µs, -14.89 µs] | -28.5 % [-30.4, -23.7] | 39.4 µs | -15.52 µs [-16.57 µs, -12.21 µs] | 32.2 µs | -9.57 µs |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | literal | 65.0 µs | -14.10 µs [-17.07 µs, -11.77 µs] | -19.9 % [-23.7, -19.0] | 41.9 µs | -12.19 µs [-16.01 µs, -10.08 µs] | 35.4 µs | -9.33 µs |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | prepared | 62.4 µs | -15.17 µs [-16.60 µs, -11.73 µs] | -24.4 % [-26.6, -16.1] | 41.5 µs | -13.31 µs [-14.37 µs, -11.50 µs] | 32.8 µs | -9.74 µs |
| `INSERT INTO i VALUES (?, ?, ?)` | literal | 52.4 µs | -7.24 µs [-8.52 µs, -3.80 µs] | -13.9 % [-15.9, -5.5] | 29.7 µs | -6.35 µs [-6.77 µs, -5.64 µs] | 33.5 µs | -8.75 µs |
| `INSERT INTO i VALUES (?, ?, ?)` | prepared | 48.5 µs | -7.19 µs [-7.90 µs, -5.43 µs] | -14.9 % [-16.3, -10.0] | 28.3 µs | -6.28 µs [-7.18 µs, -5.90 µs] | 31.8 µs | -9.35 µs |
| `DELETE FROM d WHERE id = ?` | literal | 50.1 µs | -7.61 µs [-8.03 µs, -4.35 µs] | -15.2 % [-16.0, -7.7] | 28.5 µs | -6.74 µs [-7.54 µs, -5.71 µs] | 32.9 µs | -9.16 µs |
| `DELETE FROM d WHERE id = ?` | prepared | 46.8 µs | -7.21 µs [-7.53 µs, -4.43 µs] | -15.4 % [-16.1, -7.3] | 27.1 µs | -6.28 µs [-8.12 µs, -5.29 µs] | 30.6 µs | -9.19 µs |
| `UPDATE r SET v = ? WHERE id >= 1` (every one of 1,000 rows) | literal | 986.6 µs | -227.75 µs [-244.19 µs, -191.39 µs] | -21.6 % [-24.6, -17.9] | 1,003.5 µs | -220.91 µs [-319.55 µs, -210.83 µs] | 68.8 µs | -10.27 µs |
| `UPDATE r SET v = ? WHERE id >= 1` (every one of 1,000 rows) | prepared | 1,074.0 µs | -259.96 µs [-401.37 µs, -230.80 µs] | -24.4 % [-27.5, -22.6] | 989.1 µs | -236.38 µs [-312.82 µs, -169.80 µs] | 66.6 µs | -13.87 µs |
| `SELECT * FROM s WHERE w = ?` (10,000 rows, unindexed) | literal | 783.0 µs | -37.74 µs [-126.87 µs, -30.52 µs] | -4.9 % [-11.7, -3.6] | 731.1 µs | -36.68 µs [-48.42 µs, -21.65 µs] | 58.4 µs | -10.26 µs |
| `SELECT * FROM s WHERE w = ?` (10,000 rows, unindexed) | prepared | 766.3 µs | -37.01 µs [-75.53 µs, -32.44 µs] | -4.9 % [-9.4, -4.2] | 732.3 µs | -35.01 µs [-52.99 µs, -16.14 µs] | 53.8 µs | -9.94 µs |

**Why this is BN-S4 and not the other stages.**

- **The size is the same for statements BN-S3, S5 and S9 do not touch.** Server CPU falls 6.3-7.4 µs for the `SELECT`,
  `INSERT` and `DELETE` (none runs an UPDATE's code) and 6.4-7.5 µs for the unspilled `UPDATE`s, 18.7-23.8 % of the
  statement's server CPU. The UPDATE's extra over the others is about 1 µs at most, inside the repeat floor of section 7. **BN-S1's
  census predicted 19-23 % of every point statement for the second flush; the measured fall is 18.7-23.8 %.** Its
  predicted 12 µs was taken on a busy host that doubled every CPU figure; on this one the same share is 6.3-7.5 µs.
- **It is the price of one send.** B's profile puts the single `send` of a point statement at 8.1-10.3 µs and the
  `C_PING` reply's at 7.8 µs (64 % of that frame's 12.2 µs); the fall in server CPU is within 0.3-1.5 µs of the
  smaller figure.
- **The client saved a wake-up too.** Driver CPU falls 8.8-9.8 µs, which is why the wall time falls by more than the
  server's CPU does (section 5).
- **The control did not move.** `C_PING` never had an early flush, and its p50 and server CPU are unchanged.
- **What this cannot separate**: BN-S3's partial decode and BN-S5's one encode inside a point `UPDATE` of an unspilled
  three-column row. No binary between A and B was measured; the UPDATE shapes lose at most 1 µs more than the shapes
  that run none of that code. Nothing resolvable is claimed for them.

**`SHOW META` gains less wall time than CPU.** Its server CPU falls 8.8-9.1 µs, more than any other statement's, but its
p50 only 4.4-4.7 µs and the driver's CPU 5.3-5.4 µs, half the others' 8.8-9.8. Two runs of the eight (0 and 2) carry
-10.9 and -11.3 µs in the literal arm, which are in the first two arms of runs whose first arms were disturbed; the other
six lie in -3.8 to -4.6 µs. **Not explained**: why the client saves half a wake-up on this statement was not
investigated (31 % of its server CPU is statement scaffolding, 7.4 µs against 1.9-4.3 µs in the other point
statements, section 5).

## 2. The spilled-row `UPDATE`: the copy stops, and the walk with it

Two defects BN-S1 found are gone, and both show as numbers. **A thousand updates of a spilled row allocated 38 var-heap
pages on A and 0 on B, in every one of 14 runs** (the S1 census read 38 as well: a 300-byte copy per update, about 26
to an 8 KiB page); an unspilled row allocated 0 on both.

| relation | arm | `UPDATE ... SET v = k` statements | `SHOW META` `pages_allocated` growth |
|---|---|---|---|
| unspilled row (`t`) | A | 1,000 | 0 pages in all 14 runs |
| unspilled row (`t`) | B | 1,000 | 0 pages in all 14 runs |
| spilled row (`sp`, 300-byte cell not assigned) | A | 1,000 | 38 pages in all 14 runs |
| spilled row (`sp`, 300-byte cell not assigned) | B | 1,000 | 0 pages in all 14 runs |

The cost follows. The statement that does not assign the spilled cell (`sp`) is 16.6 µs (literal) and 17.0 µs
(prepared) lower at p50, the one that assigns it (`sa`) 14.1 and 15.2 µs lower:

| statement | path | B - A at p50, median [min, max] | relative | B - A server CPU, median [min, max] | change in rate |
|---|---|---|---|---|---|
| `UPDATE sp SET v = ? WHERE id = ?` (spilled cell not assigned) | literal | -16.56 µs [-24.81 µs, -14.25 µs] | -26.2 % | -15.95 µs [-19.21 µs, -12.61 µs] | +35.6 % |
| same | prepared | -17.03 µs [-18.38 µs, -14.89 µs] | -28.5 % | -15.52 µs [-16.57 µs, -12.21 µs] | +39.8 % |
| `UPDATE sa SET txt = ? WHERE id = ?` (spilled cell assigned) | literal | -14.10 µs [-17.07 µs, -11.77 µs] | -19.9 % | -12.19 µs [-16.01 µs, -10.08 µs] | +24.8 % |
| same | prepared | -15.17 µs [-16.60 µs, -11.73 µs] | -24.4 % | -13.31 µs [-14.37 µs, -11.50 µs] | +32.4 % |

**A's cost depends on how long the relation's var-heap chain already is, and B's does not.** The server CPU of A's
spilled arms rises over an arm's 3,000 updates (last three blocks minus first three, median over runs: +2.5, +1.9, +2.9
and +1.8 µs for the four arms) and B's does not (+0.3, +0.3, +0.1 and -0.2 µs):

| statement | path | arm | server CPU, first block (250 statements) | server CPU, last block | last three blocks minus first three, per run: median [min, max] |
|---|---|---|---|---|---|
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | literal | A | 37.5 µs | 39.9 µs | +2.5 µs [-0.1 µs, +7.4 µs] |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | literal | B | 24.8 µs | 23.8 µs | +0.3 µs [-1.6 µs, +4.1 µs] |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | prepared | A | 37.9 µs | 40.7 µs | +1.9 µs [-0.2 µs, +3.0 µs] |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | prepared | B | 23.6 µs | 23.2 µs | +0.3 µs [-0.5 µs, +2.4 µs] |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | literal | A | 41.7 µs | 42.3 µs | +2.9 µs [+1.2 µs, +3.7 µs] |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | literal | B | 30.2 µs | 28.9 µs | +0.1 µs [-0.4 µs, +2.3 µs] |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | prepared | A | 40.4 µs | 46.2 µs | +1.8 µs [+0.6 µs, +8.9 µs] |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | prepared | B | 28.2 µs | 27.8 µs | -0.2 µs [-2.5 µs, +5.2 µs] |

The arm that ran second on a relation starts after 3,000 more appended values (about 116 more pages) than the one
that ran first, and A's delta grows with it. In the runs where neither binary's arm was disturbed, the second arm's
delta is 1-4 µs larger than the first's; B's cost does not depend on the position. The per-run p50 deltas, with the
arm's position on its relation:

| run | arm order on the spilled relations | `sp` literal | `sp` prepared | `sa` literal | `sa` prepared |
|---|---|---|---|---|---|
| 0 | literal, then prepared | -15.1 µs (1st) | -18.0 µs (2nd) | -12.4 µs (1st) | -16.2 µs (2nd) |
| 1 | prepared, then literal | -18.5 µs (2nd) | -16.2 µs (1st) | -15.9 µs (2nd) | -14.4 µs (1st) |
| 2 | literal, then prepared | -14.7 µs (1st) | -17.9 µs (2nd) | -12.4 µs (1st) | -15.8 µs (2nd) |
| 3 | prepared, then literal | -18.0 µs (2nd) | -15.1 µs (1st) | -15.8 µs (2nd) | -15.1 µs (1st) |
| 4 | literal, then prepared | -14.3 µs (1st) | -17.4 µs (2nd) | -11.8 µs (1st) | -15.3 µs (2nd) |
| 5 | prepared, then literal | -20.8 µs (2nd) | -14.9 µs (1st) | -16.3 µs (2nd) | -11.7 µs (1st) |
| 6 | literal, then prepared | -14.7 µs (1st) | -18.4 µs (2nd) | -12.4 µs (1st) | -16.6 µs (2nd) |
| 7 | prepared, then literal | -24.8 µs (2nd) | -16.7 µs (1st) | -17.1 µs (2nd) | -14.4 µs (1st) |

(Runs 5 and 7 carry arms 1.2-1.4 times their median on both binaries, which scales the delta with them: -20.8 and -24.8
µs in the `sp` literal column are those, not the chain.)

**Attribution, as far as the data goes.** Taking the arms that ran first on their relation (the shortest chain) and
setting each against the same path's unspilled `UPDATE`, whose fall is the send plus whatever BN-S3 gives a point
update:

| statement | path | B - A server CPU, arm ran first on its relation: median [min, max] over its 4 runs | the same path's unspilled `UPDATE t` B - A server CPU (8 runs) | difference |
|---|---|---|---|---|
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | literal | -13.51 µs [-15.02 µs, -12.61 µs] | -7.45 µs | -6.06 µs |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | prepared | -13.76 µs [-16.16 µs, -12.21 µs] | -6.61 µs | -7.15 µs |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | literal | -10.36 µs [-10.66 µs, -10.08 µs] | -7.45 µs | -2.91 µs |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | prepared | -12.19 µs [-13.56 µs, -11.50 µs] | -6.61 µs | -5.58 µs |

The statement that does not assign the spilled cell falls **6.1-7.2 µs of server CPU beyond the unspilled one**: the
copy BN-S5 stops - the fetch of the old value, the append, its log record and its undo note - and no walk, since B
appends nothing. B's var-heap part in the profile is 0.0 µs (not listed) for that statement and 0.4 µs (1 %) for the
assigning one; BN-S1 had it at 37-54 % and 32-60 % of the statement on A, on a busy host. The assigning statement still
appends one value; its **2.9 µs (literal) and 5.6 µs (prepared)** beyond the unspilled one are shared by BN-S9 (the walk
starts at the tail, not the root) and BN-S3 (the cell it overwrites is no longer fetched and resolved), and **the two
are not separable here**. The two paths differ by 2.7 µs; the prepared-first runs are 1, 3, 5 and 7, and three of
them (3, 5, 7) carry disturbed `sa` arms, so the difference is not read as an effect of the path.

## 3. The every-row `UPDATE`: the row visit lost 174 µs, the undo and WAL records 32

The `UPDATE r SET v = ? WHERE id >= 1` over 1,000 rows is 227.8 µs (literal) and 260.0 µs (prepared) faster at p50, 21.6
and 24.4 %, in 8 of 8 runs; the server's CPU falls 220.9 and 236.4 µs. In rate: 1,013,557 to 1,309,538 rows/s
literal, 931,115 to 1,222,307 prepared. **Per row, A reads 987 ns at p50 and 1,003 ns of server CPU, B 764 ns and 744
ns** (literal; a point `UPDATE`'s server CPU is about 3 % of the statement's, so the one-size figure is close to a
per-row cost, which a size sweep would settle).

**The profile says where.** A: 967.6 µs of server CPU per statement; B: 753.1 µs; 214.5 µs fewer, of which 207.1 µs (96.5
%) is inside the per-row visitor lambda `UpdateInner::{lambda#1}` and 7.4 µs outside it.

| part (first matching part of `tools/bn_parts.json`) | A: 967.6 µs per statement, 1,415 samples | B: 753.1 µs, 1,130 samples | B - A (-214.5 µs) |
|---|---|---|---|
| socket send | 23.3 µs (2 %) | 7.3 µs (1 %) | -16.0 µs |
| socket read and wait | 2.7 µs (0 %) | 6.0 µs (1 %) | +3.3 µs |
| KWP frames | 3.4 µs (0 %) | 3.3 µs (0 %) | -0.1 µs |
| parse | 1.4 µs (0 %) | 4.0 µs (1 %) | +2.6 µs |
| var-heap append | 3.4 µs (0 %) | 4.0 µs (1 %) | +0.6 µs |
| undo and WAL records | 316.6 µs (33 %) | 284.6 µs (38 %) | -32.0 µs |
| transaction begin, commit, locks | 17.1 µs (2 %) | 18.0 µs (2 %) | +0.9 µs |
| descent and page access | 564.2 µs (58 %) | 389.9 µs (52 %) | -174.3 µs |
| statement scaffolding | 4.1 µs (0 %) | 6.0 µs (1 %) | +1.9 µs |
| scheduler and checkpoint | 3.4 µs (0 %) | 4.0 µs (1 %) | +0.6 µs |
| other | 28.0 µs (3 %) | 25.3 µs (3 %) | -2.7 µs |

(`descent and page access` is the first matching part for every sample on the row walk, because `btree::` is on every
chain: here it is the row visit - decode, SET evaluation, encode, page latching - less what `undo and WAL records` and the
parts above it claim first.)

The functions, self time unless marked (one profile per binary, 1,500 statements; members below the report's 25th entry
are "unlisted" and each lies under that entry's time):

| function family (every-row `UPDATE`, literal path) | A: 967.6 µs per statement, 1,415 samples | B: 753.1 µs, 1,130 samples | B - A |
|---|---|---|---|
| the per-row visitor lambda `UpdateInner::{lambda#1}`, inclusive | 808.9 µs | 601.8 µs | -207.1 µs |
| undo log append, inclusive (`TransactionManager::AppendUndo`) | 225.0 µs | 187.3 µs | -37.7 µs |
| the row lambda's own code (self) | 106.0 µs | 104.0 µs | -1.9 µs |
| page-table lookup (`std::_Hashtable::find`, the `frames_` map under `DevicePageStore::Resolve`) | 60.9 µs | 34.6 µs | -26.2 µs |
| allocator (`malloc`, `free`, `_int_malloc`, `_int_free`, `tcache_get_n`) | 106.6 µs | 64.6 µs + 1 unlisted (each < 8.0 µs) | falls by at least 34.0 µs |
| vector and string frames (`vector` ctor and dtor, `basic_string::_M_dispose`) | 45.8 µs | not listed (< 8.0 µs) | falls by at least 21.8 µs |
| decode (`DecodeRow`, `DecodeOneValueInto`, `DecodeColumnsInto`) | 35.6 µs + 1 unlisted (each < 12.3 µs) | 17.3 µs + 2 unlisted (each < 8.0 µs) | falls by at least 2.3 µs |
| encode (`EncodeRow`, `EncodeAssigned`, `EncodeColumn`, `EncodeOneValue`) | 30.8 µs + 2 unlisted (each < 12.3 µs) | 32.6 µs + 1 unlisted (each < 8.0 µs) | no fall resolved |
| WAL and data-file writes (`pwrite64`, `WalStream::Append`, `HardwareExtend`) | 78.6 µs | 78.0 µs | -0.5 µs |
| `__libc_send` | 22.5 µs | not listed (< 8.0 µs) | falls by at least 14.6 µs |

- **The undo log**: `AppendUndo` falls 37.7 µs (225.0 to 187.3). BN-S3 appends through the hold that checked the tail's
  room (one page fetch a row fewer) and BN-S5 takes the before-image from the walk's copy of the row's bytes
  (`payload_copy`) instead of re-reading the page (another); the page table's hash lookup (`std::_Hashtable::find`, the `frames_` map under `DevicePageStore::Resolve`)
  falls 26.2 µs (89 to 52 samples), which is what two fetches fewer a row would look like. **The stage split is not
  separable**: each removes one, and no binary has one without the other.
- **Allocation and the row's vectors**: the allocator's own frames fall by at least 34.0 µs and the vector and string
  frames by at least 21.8 µs (they overlap: the vectors allocate). That is what the whole-row decode into per-row vectors
  and the extra before-image copy going away would look like (BN-S3's statement-scoped partial row, BN-S5's copy); the
  decode functions' own self time falls by at least 2.3 µs only.
- **Encode did not shrink at this resolution** (30.8 against 32.6 µs listed): BN-S5's in-place re-encode of one cell
  saves nothing resolvable on a three-column integer row; its value is the carried spill (section 2).
- **The WAL did not change**: `pwrite`, `WalStream::Append` and the CRC-32C extension total 78.6 and 78.0 µs. **BN-R5
  (byte-range records), discarded at BN-S3's re-profile, is confirmed by this run**: the undo and WAL records are the
  largest named part of B (284.6 µs, 38 % of the statement, 33 % on A) and did not shrink while the row visit shrank by
  174 µs; they are per record, not per byte.

**What B spends per row** (753 ns; inclusive figures can overlap, since an undo write is itself logged): the undo
log's append 181 ns (24 %, `UndoLog::Append` inclusive), the WAL manager's append 65 ns (8.6 %, inclusive), the row
visitor's own code 104 ns (13.8 %, self) and `pwrite` 36 ns (4.8 %, self).

## 4. The scan row and the PostgreSQL floor

The 10,000-row unindexed equality is 37.7 µs (literal) and 37.0 µs (prepared) lower at p50, 4.9 % both, in 8 of 8 runs;
server CPU 36.7 and 35.0 µs lower. About 7 µs of that is the send every statement saves (section 1), so the walk itself
gained about 29 µs, **2.9 ns of a row**. Taking a point `SELECT`'s server CPU off the scan's, per run, as the PostgreSQL
twin does:

| engine | path | runs | point `SELECT` by pk, server CPU | 10,000-row scan, server CPU | cost per scanned row net of a point statement: (scan - point) / 10,000, median [min, max] over runs |
|---|---|---|---|---|---|
| KDS A `b70f79d8` | literal | 8 | 35.7 µs | 731.1 µs | 69.5 ns [66.4, 72.0] |
| KDS A `b70f79d8` | prepared | 8 | 35.0 µs | 732.3 µs | 69.7 ns [66.6, 72.1] |
| KDS B `8679367e` | literal | 8 | 28.1 µs | 699.8 µs | 67.2 ns [64.3, 68.7] |
| KDS B `8679367e` | prepared | 8 | 28.4 µs | 697.8 µs | 67.0 ns [64.3, 68.9] |
| PostgreSQL 18.6 backend (defaults; simple query protocol, a literal per statement) | literal | 5 | 41.0 µs | 383.9 µs | 34.3 ns [34.1, 34.8] |

The paired fall of A's net row is -2.98 ns [-4.15, -1.38] literal and -2.84 ns [-4.61, -0.92] prepared (8 of 8
negative). **KDS's scan row is 1.96 times PostgreSQL's on B and 2.03 on A**: the order's §0 reading of 70 against 34 ns
holds on one driver and one host family, and BN moved it by 3 ns. PostgreSQL is the floor, not the reference: its
point statement costs 41.0 µs of backend CPU against KDS's 28.1 µs (B) and 35.7 µs (A), so the engine clears the floor
there, and the scan row, twice as dear, is the part of the engine that does not. The twin exists for the scan only;
the every-row `UPDATE` and the spilled-row shapes have none.

The scan's sampled parts (every sample of the row walk lands in `descent and page access`, 93 % in both, which is why
the functions below are the useful split):

| part (first matching part of `tools/bn_parts.json`) | A: 754.4 µs per statement, 1,523 samples | B: 708.6 µs, 1,394 samples | B - A (-45.8 µs) |
|---|---|---|---|
| socket send | 21.3 µs (3 %) | 9.1 µs (1 %) | -12.2 µs |
| socket read and wait | 4.5 µs (1 %) | 7.6 µs (1 %) | +3.1 µs |
| KWP frames | 1.5 µs (0 %) | 3.0 µs (0 %) | +1.5 µs |
| parse | 3.0 µs (0 %) | 1.5 µs (0 %) | -1.5 µs |
| compile | 3.0 µs (0 %) | 2.5 µs (0 %) | -0.5 µs |
| Waystone and access statistics | 0.0 µs (0 %) | 1.0 µs (0 %) | +1.0 µs |
| transaction begin, commit, locks | 7.4 µs (1 %) | 4.1 µs (1 %) | -3.3 µs |
| descent and page access | 698.0 µs (93 %) | 657.3 µs (93 %) | -40.7 µs |
| statement scaffolding | 12.4 µs (2 %) | 16.3 µs (2 %) | +3.9 µs |
| scheduler and checkpoint | 3.5 µs (0 %) | 3.6 µs (1 %) | +0.1 µs |
| other | 0.0 µs (0 %) | 2.0 µs (0 %) | +2.0 µs |

**What B's scan spends** (one profile each of A and B, 2,000 scans, self time; B per row is the B column over 10,000):

| function (scan, literal path, self time) | A: 754.4 µs, 1,523 samples | B: 708.6 µs, 1,394 samples | B - A | B per row |
|---|---|---|---|---|
| `ChainRunner::AcceptTupleAt` (the one place a snapshot filters a row, `step_vm.hpp:275`) | 119.3 µs (15.8 %, 241 samples) | 117.4 µs (16.6 %, 231 samples) | -1.9 µs | 11.7 ns |
| `DecodeColumnsInto` | 89.6 µs (11.9 %, 181 samples) | 80.3 µs (11.3 %, 158 samples) | -9.3 µs | 8.0 ns |
| `BtreeVisitLeafPage` | 67.4 µs (8.9 %, 136 samples) | 65.0 µs (9.2 %, 128 samples) | -2.3 µs | 6.5 ns |
| `std::_Function_handler::_M_invoke` (the walk's callback) | 61.9 µs (8.2 %, 125 samples) | 65.5 µs (9.2 %, 129 samples) | +3.6 µs | 6.6 ns |
| `PageView::ReadTuple` | 57.9 µs (7.7 %, 117 samples) | 40.7 µs (5.7 %, 80 samples) | -17.3 µs | 4.1 ns |
| `DecodeOneValueInto` | 49.6 µs (6.6 %, 100 samples) | 56.4 µs (8.0 %, 111 samples) | +6.8 µs | 5.6 ns |
| `EvaluatePredicate` | 41.6 µs (5.5 %, 84 samples) | 42.7 µs (6.0 %, 84 samples) | +1.1 µs | 4.3 ns |
| `EvaluateAll` | 37.1 µs (4.9 %, 75 samples) | 26.4 µs (3.7 %, 52 samples) | -10.7 µs | 2.6 ns |
| `ExecStats::For` | 34.7 µs (4.6 %, 70 samples) | 19.3 µs (2.7 %, 38 samples) | -15.4 µs | 1.9 ns |
| `DecodeAndResolve` | 27.8 µs (3.7 %, 56 samples) | 32.5 µs (4.6 %, 64 samples) | +4.8 µs | 3.3 ns |
| `CheckDecodeInputs` | 27.2 µs (3.6 %, 55 samples) | 28.5 µs (4.0 %, 56 samples) | +1.3 µs | 2.8 ns |
| `EvaluateConjunct` | 27.2 µs (3.6 %, 55 samples) | 23.9 µs (3.4 %, 47 samples) | -3.4 µs | 2.4 ns |
| `CompareValues` | 24.7 µs (3.3 %, 50 samples) | 25.4 µs (3.6 %, 50 samples) | +0.7 µs | 2.5 ns |
| `__libc_send` | 21.3 µs (2.8 %, 43 samples) | 9.1 µs (1.3 %, 18 samples) | -12.1 µs | 0.9 ns |
| `memcpy` | 13.9 µs (1.8 %, 28 samples) | 17.8 µs (2.5 %, 35 samples) | +3.9 µs | 1.8 ns |

- **`ExecStats::For` fell from 34.7 to 19.3 µs (70 to 38 samples, about three sampling errors): 1.5 ns of the 2.9 ns
  the interleaved pair puts on the row**, BN-S3's inline and its single counter update per examined row. The symbol is still sampled at 2.7 %, so some
  call sites remain out of line in this build, or the work moved into its callers; the profile cannot tell which.
- The send falls 12.1 µs in the profile (43 to 18 samples); the interleaved pair puts the fixed saving at 7 µs.
- **The rest of the movement is inside the sampling error**: `PageView::ReadTuple` -17.3 µs (117 to 80 samples, no BN
  stage touches it), `EvaluateAll` -10.7 µs and `DecodeColumnsInto` -9.3 µs against rises of +6.8 µs
  (`DecodeOneValueInto`), +4.8 µs (`DecodeAndResolve`) and +3.6 µs (`_M_invoke`). `PageView::ReadTuple`'s source is
  unchanged between A and B. Not attributed.
- **What a B row is made of**, the largest roughly disjoint pieces: decoding (`DecodeAndResolve` and what it calls,
  28.1 % inclusive, 199.3 µs, **19.9 ns**), evaluating `w = ?` (`EvaluateAll`, 16.3 % inclusive, **11.5 ns**),
  `AcceptTupleAt`'s own body (16.6 % self, **11.7 ns**), the leaf walk and its callback (`BtreeVisitLeafPage` and
  `_M_invoke`, **13.1 ns**) and `ReadTuple` (**4.1 ns**).

## 5. Where a statement's time goes

A point statement's wall time is two processes' CPU in series with a little of it overlapped; at one session on
`relaxed` nothing waits on a device or a lock. Named by wait type, for the unit this run measures:

| wait type | in a point statement here |
|---|---|
| durability / commit (fsync) | none on the statement's path: `relaxed` returns without waiting for a sync. An `fdatasync` shows as 1.15 % (8.7 µs) of an every-row statement's server CPU in B (the stack was not split by thread, so whose sync it is is not stated), CPU not wait. `strict` and `group`, where a commit does wait, were **not measured** |
| write statement, read statement | the server CPU parts of the table below and of section 3 and 4 |
| client and socket round trip | wall time not in the server's CPU: 28.4 µs of a point `SELECT`'s 56.5 µs on B, 21.2 µs of a point `UPDATE`'s 45.7 µs; the driver's own CPU is 23.8-32.7 µs across the point shapes (a Python client), and **this driver cannot split the loopback wake from the client's CPU**: both sit inside the two CPU columns |
| lock or conflict wait | none can occur with one session; the lock table's CPU is the `txn + locks` part (0.6 µs of a point `UPDATE`). Not measurable here, and not meaningful at one session |

**The double flush cost more CPU than wall time.** The sum of the two processes' CPU exceeds the wall time by the CPU
that ran concurrently; it fell from 10.9-14.3 µs on A to 2.9-5.1 µs on B in every point shape:

| statement (literal path) | arm | wall p50 | server CPU | driver CPU | server + driver - wall (CPU that overlapped) | server CPU as a share of wall |
|---|---|---|---|---|---|---|
| `C_PING` frame (control: no statement, no dispatcher) | A | 26.2 µs | 12.2 µs | 15.2 µs | 1.3 µs | 47 % |
| `C_PING` frame (control: no statement, no dispatcher) | B | 26.2 µs | 12.3 µs | 15.3 µs | 1.4 µs | 47 % |
| `SHOW META` | A | 50.5 µs | 33.5 µs | 29.8 µs | 12.8 µs | 66 % |
| `SHOW META` | B | 46.2 µs | 24.6 µs | 24.9 µs | 3.2 µs | 53 % |
| `SELECT * FROM t WHERE id = ?` | A | 65.5 µs | 35.7 µs | 42.3 µs | 12.5 µs | 54 % |
| `SELECT * FROM t WHERE id = ?` | B | 56.5 µs | 28.1 µs | 32.7 µs | 4.4 µs | 50 % |
| `UPDATE t SET v = ? WHERE id = ?` | A | 53.8 µs | 31.9 µs | 34.3 µs | 12.4 µs | 59 % |
| `UPDATE t SET v = ? WHERE id = ?` | B | 45.7 µs | 24.5 µs | 24.9 µs | 3.7 µs | 54 % |
| `UPDATE t SET v = v + 1 WHERE id = ?` | A | 56.6 µs | 33.6 µs | 34.3 µs | 11.3 µs | 59 % |
| `UPDATE t SET v = v + 1 WHERE id = ?` | B | 47.9 µs | 27.1 µs | 24.8 µs | 4.0 µs | 57 % |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | A | 62.3 µs | 41.6 µs | 35.1 µs | 14.3 µs | 67 % |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | B | 45.7 µs | 24.7 µs | 25.2 µs | 4.2 µs | 54 % |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | A | 65.0 µs | 41.9 µs | 35.4 µs | 12.4 µs | 65 % |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | B | 50.8 µs | 29.7 µs | 26.2 µs | 5.1 µs | 58 % |
| `INSERT INTO i VALUES (?, ?, ?)` | A | 52.4 µs | 29.7 µs | 33.5 µs | 10.9 µs | 57 % |
| `INSERT INTO i VALUES (?, ?, ?)` | B | 44.5 µs | 23.3 µs | 24.8 µs | 3.5 µs | 52 % |
| `DELETE FROM d WHERE id = ?` (supplement) | A | 50.1 µs | 28.5 µs | 32.9 µs | 11.3 µs | 57 % |
| `DELETE FROM d WHERE id = ?` (supplement) | B | 42.3 µs | 21.4 µs | 23.8 µs | 2.9 µs | 51 % |

For the point `SELECT` the two CPUs summed 78.0 µs on A and 60.8 µs on B (17.2 µs less) while the wall fell 9.0: **8.1
µs of what A removed had been running concurrently, so it bought nothing but a second wake-up**. A `C_PING` frame, which
reaches no dispatcher, costs 26.2 µs of wall in this driver (12.3 µs of server CPU, 15.3 µs of driver CPU); a B point
`UPDATE` adds 19.5 µs to that, a point `SELECT` 30.3 µs.

B's server CPU per point statement, by part (one profile of 30,000 statements per arm; one sample is 0.03 µs of a
statement, so a part of 1 µs carries about ±0.2 µs; `-` is under 0.05 µs; the profiled build's CPU per statement
agrees with the unprofiled B's within 1.3 µs on seven of the nine shapes and within 5 µs on all):

| statement (literal path) | server CPU | samples | send | read + wait | KWP frames | parse | compile | Waystone + access | var-heap append | undo + WAL | txn + locks | descent + pages | decode + encode | scaffolding | scheduler | engine work (Waystone + access, var-heap, undo + WAL, txn + locks, descent + pages, decode + encode, visibility; sum) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `C_PING` frame (control: no statement, no dispatcher) | 12.2 µs | 322 | 7.8 µs (64 %) | 4.0 µs (33 %) | 0.1 µs (1 %) | - | - | - | - | - | - | - | - | - | 0.3 µs (3 %) | 0.0 µs (0 %) |
| `SHOW META` | 23.6 µs | 630 | 9.2 µs (39 %) | 4.8 µs (20 %) | 1.2 µs (5 %) | - | - | - | - | - | - | - | - | 7.4 µs (31 %) | 1.0 µs (4 %) | 0.0 µs (0 %) |
| `SELECT * FROM t WHERE id = ?` | 26.9 µs | 748 | 9.6 µs (36 %) | 4.9 µs (18 %) | 1.8 µs (7 %) | 1.7 µs (6 %) | 1.5 µs (5 %) | 1.6 µs (6 %) | - | 0.3 µs (1 %) | - | 0.3 µs (1 %) | 0.3 µs (1 %) | 3.9 µs (14 %) | 1.2 µs (4 %) | 2.5 µs (9 %) |
| `UPDATE t SET v = ? WHERE id = ?` | 24.2 µs | 723 | 9.2 µs (38 %) | 5.2 µs (21 %) | 1.5 µs (6 %) | 1.1 µs (5 %) | 0.8 µs (3 %) | - | - | 0.7 µs (3 %) | 0.6 µs (2 %) | 0.8 µs (3 %) | 0.2 µs (1 %) | 2.4 µs (10 %) | 1.5 µs (6 %) | 2.3 µs (10 %) |
| `UPDATE t SET v = v + 1 WHERE id = ?` | 31.9 µs | 933 | 9.0 µs (28 %) | 5.0 µs (16 %) | 2.1 µs (7 %) | 2.7 µs (9 %) | 1.6 µs (5 %) | - | - | 1.2 µs (4 %) | 1.8 µs (6 %) | 1.2 µs (4 %) | 0.5 µs (2 %) | 4.3 µs (13 %) | 1.6 µs (5 %) | 4.9 µs (15 %) |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | 24.4 µs | 669 | 8.9 µs (37 %) | 4.8 µs (20 %) | 1.9 µs (8 %) | 2.2 µs (9 %) | 0.6 µs (2 %) | - | - | 0.8 µs (3 %) | 0.9 µs (4 %) | 0.8 µs (3 %) | 0.3 µs (1 %) | 1.9 µs (8 %) | 1.2 µs (5 %) | 2.9 µs (12 %) |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | 30.1 µs | 837 | 10.3 µs (34 %) | 4.7 µs (16 %) | 3.3 µs (11 %) | 1.9 µs (6 %) | 0.5 µs (2 %) | - | 0.4 µs (1 %) | 1.4 µs (5 %) | 1.1 µs (4 %) | 1.4 µs (5 %) | 1.0 µs (3 %) | 2.7 µs (9 %) | 1.2 µs (4 %) | 5.4 µs (18 %) |
| `INSERT INTO i VALUES (?, ?, ?)` | 22.0 µs | 627 | 8.9 µs (41 %) | 4.8 µs (22 %) | 1.5 µs (7 %) | 1.1 µs (5 %) | - | - | - | 0.7 µs (3 %) | 0.9 µs (4 %) | 0.7 µs (3 %) | 0.1 µs (1 %) | 2.0 µs (9 %) | 1.1 µs (5 %) | 2.5 µs (11 %) |
| `DELETE FROM d WHERE id = ?` | 23.6 µs | 646 | 8.1 µs (34 %) | 4.7 µs (20 %) | 2.0 µs (8 %) | 1.7 µs (7 %) | 0.4 µs (2 %) | - | - | 1.1 µs (4 %) | 1.4 µs (6 %) | 0.7 µs (3 %) | 0.2 µs (1 %) | 2.0 µs (9 %) | 1.0 µs (4 %) | 3.6 µs (15 %) |

**The request path is what is left.** In the point `UPDATE`, `send` and `read` are 59 % of the 24.2 µs and the
engine's own work (`undo + WAL` 0.7 µs, `txn + locks` 0.6, `descent + pages` 0.8, `decode + encode` 0.2) is **2.3 µs,
10 % of the server CPU and 5.0 % of the 45.7 µs wall time**; a bare `C_PING` already costs 12.2 µs of that 24.2. The
statement scaffolding (`CommandDispatcher` and the chain's runner, 2.4 µs) is as large as the whole storage side, and
the scheduler (1.5 µs) and the frame decoding (1.5 µs) two-thirds of it.

## 6. Against this engine's last number

The previous results file for these shapes is `bench/v3.0.0/results-bj-close-v2.7.0-757-ge458e272.md`, taken **13-14
hours earlier** (matrix 2026-10-10 18:55-19:48 UTC) at `v2.7.0-757-ge458e272`: **12 commits before A and 30 before B by
`git describe`**, on a different driver (`bj_close_ab.py`, which renders each reply as text through
`ServerConnection.send_command`; pooled percentiles over 16 runs, loadavg 1.3-10.3 at its run starts), the same server
configuration and pinning. Its p50s for the B' arm, as rates:

| statement (literal path) | BJ close `v2.7.0-757-ge458e272`, p50 | this file, A `v2.7.0-769-gb70f79d8` | A against BJ | this file, B `v2.7.0-787-g8679367e` | B against BJ |
|---|---|---|---|---|---|
| `SHOW META` | 19,920 /s (50.2 µs) | 19,819 /s (50.5 µs) | -0.5 % rate (+0.3 µs) | 21,622 /s (46.2 µs) | +8.5 % rate (-4.0 µs) |
| `SELECT * FROM t WHERE id = ?` | 14,859 /s (67.3 µs) | 15,276 /s (65.5 µs) | +2.8 % rate (-1.8 µs) | 17,696 /s (56.5 µs) | +19.1 % rate (-10.8 µs) |
| `UPDATE t SET v = ? WHERE id = ?` | 19,231 /s (52.0 µs) | 18,585 /s (53.8 µs) | -3.4 % rate (+1.8 µs) | 21,880 /s (45.7 µs) | +13.8 % rate (-6.3 µs) |
| `UPDATE t SET v = v + 1 WHERE id = ?` (BJ: its Part 2, B' only) | 18,083 /s (55.3 µs) | 17,655 /s (56.6 µs) | -2.4 % rate (+1.3 µs) | 20,891 /s (47.9 µs) | +15.5 % rate (-7.4 µs) |
| `INSERT INTO i VALUES (?, ?, ?)` | 20,243 /s (49.4 µs) | 19,101 /s (52.4 µs) | -5.6 % rate (+3.0 µs) | 22,466 /s (44.5 µs) | +11.0 % rate (-4.9 µs) |
| `DELETE FROM d WHERE id = ?` | 20,450 /s (48.9 µs) | 19,952 /s (50.1 µs) | -2.4 % rate (+1.2 µs) | 23,641 /s (42.3 µs) | +15.6 % rate (-6.6 µs) |
| `UPDATE r SET v = ? WHERE id >= 1` (every one of 1,000 rows) | 1,017 /s (983 µs) | 1,014 /s (987 µs) | -0.3 % rate (+3 µs) | 1,310 /s (764 µs) | +28.8 % rate (-220 µs) |

**A reads between -1.8 and +3.4 µs from BJ's close in every shape, and +0.3 µs on `SHOW META`**, the control BJ chose
because no BJ path can touch it (and which has no BN code on it in A), so BJ's figures are a usable previous number to
about 3 µs. The engine commits between BJ's close and A are three BJ review fixes and two foreign-key fixes (`git log
e458e272..b70f79d8 -- src include`), the drivers differ, and the 13-14 hours between are another host state. **B is 4.0-10.8
µs lower than BJ's close in every point shape and 220 µs lower on the every-row `UPDATE`**: `SHOW META` (-4.0 µs) by
the size the interleaved pair measured (-4.4 µs), the others by more than the 3 µs band. The interleaved A/B, not this
comparison, is the result; this table says B is faster than the number the last milestone left, by about what the
pair measured.

## 7. The noise floor, and what could not be resolved

**The control and the repeat.** `C_PING` is the control that cannot be affected; the `SELECT` by pk was measured in both
the census and the supplement, which is the repeat:

| control / replicate | runs | B - A at p50, median [min, max] | B - A server CPU, median [min, max] | block pairs (250 statements each) with B below A |
|---|---|---|---|---|
| `C_PING` control, main census | 8 | +0.01 µs [-2.81 µs, +0.67 µs] | +0.02 µs [-0.44 µs, +0.22 µs] | 44 of 96 |
| `C_PING` control, supplement | 6 | -0.12 µs [-0.71 µs, -0.03 µs] | -0.02 µs [-0.08 µs, +0.08 µs] | 35 of 72 |
| `SELECT` by pk, literal, main census | 8 | -9.23 µs [-14.71 µs, -8.08 µs] | -7.43 µs [-7.97 µs, -5.81 µs] | 96 of 96 |
| `SELECT` by pk, literal, supplement (a repeat of the arm, 6 further runs) | 6 | -8.05 µs [-9.97 µs, -6.91 µs] | -6.52 µs [-8.11 µs, -5.49 µs] | 71 of 72 |
| `SELECT` by pk, prepared, main census | 8 | -9.44 µs [-12.87 µs, -9.01 µs] | -6.50 µs [-6.90 µs, -5.51 µs] | 94 of 96 |
| `SELECT` by pk, prepared, supplement (a repeat of the arm, 6 further runs) | 6 | -9.53 µs [-9.91 µs, -7.91 µs] | -7.22 µs [-8.50 µs, -6.71 µs] | 72 of 72 |

The control's paired p50 is +0.01 µs [-2.81, +0.67] in the census and -0.12 µs [-0.71, -0.03] in the supplement; the
two extremes (+0.67 and -2.81) are runs 0 and 2, whose first arms were disturbed on both binaries, and in the six clean
runs the control's range is [-0.05, +0.10] µs of p50 and [-0.09, +0.14] µs of server CPU. The repeat differs from the
census by 1.18 µs (literal) and 0.09 µs (prepared) at p50 and 0.91 and 0.72 µs of server CPU. **The floor on a paired
point delta is therefore about 1 µs; every point statement's server-CPU delta in section 1 is 6.3 µs or more and every
p50 delta 7.2 µs or more, except `SHOW META`'s 4.4-4.7 µs, itself four times the control's clean range.** The scan and
every-row deltas have no control of their own length: they are read by sign (8 of 8 runs, 91-94 % of the block pairs)
and by range.

**Disturbed cells.** 59 of the census's 304 arm-cells (19 arms x 8 runs x 2 binaries) read more than 12 %
above their arm's median p50 on one binary or the other, by run: run 0: 14, run 1: 0, run 2: 4, run 3: 11, run 4: 0, run 5: 11, run 6: 0, run 7: 19 (of 38 arm-cells per run, both binaries counted; 59 of 304 in all). Runs 0, 3, 5 and 7 carry most (run 7
slowed `update-spill`, `update-spill-assign`, `update-expr`, the literal `scan` and the prepared every-row arm by
1.2-1.4 times on both binaries). The medians over runs barely move when those arm-runs are dropped - the unspilled
point shapes and `SHOW META` within 0.25 µs, the scan and the literal every-row arm within 1.1 µs - except the spilled arms
(1.7 µs at most) and the prepared every-row arm (12.2 µs), where the disturbed runs are the larger deltas and the kept runs
shift which position each arm held:

| statement | path | runs kept | B - A at p50, kept runs | B - A at p50, all runs |
|---|---|---|---|---|
| `C_PING` frame (control: no statement, no dispatcher) | literal | 6 of 8 | +0.01 µs | +0.01 µs |
| `SHOW META` | literal | 6 of 8 | -4.29 µs | -4.44 µs |
| `SHOW META` | prepared | 7 of 8 | -4.61 µs | -4.72 µs |
| `SELECT * FROM t WHERE id = ?` | literal | 7 of 8 | -9.11 µs | -9.23 µs |
| `SELECT * FROM t WHERE id = ?` | prepared | 7 of 8 | -9.28 µs | -9.44 µs |
| `UPDATE t SET v = ? WHERE id = ?` | literal | 6 of 8 | -7.94 µs | -7.88 µs |
| `UPDATE t SET v = ? WHERE id = ?` | prepared | 7 of 8 | -8.70 µs | -8.67 µs |
| `UPDATE t SET v = v + 1 WHERE id = ?` | literal | 7 of 8 | -8.70 µs | -8.53 µs |
| `UPDATE t SET v = v + 1 WHERE id = ?` | prepared | 7 of 8 | -9.40 µs | -9.39 µs |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | literal | 6 of 8 | -14.93 µs | -16.56 µs |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | prepared | 7 of 8 | -17.37 µs | -17.03 µs |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | literal | 5 of 8 | -12.37 µs | -14.10 µs |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | prepared | 5 of 8 | -15.81 µs | -15.17 µs |
| `INSERT INTO i VALUES (?, ?, ?)` | literal | 6 of 8 | -7.24 µs | -7.24 µs |
| `INSERT INTO i VALUES (?, ?, ?)` | prepared | 5 of 8 | -7.40 µs | -7.19 µs |
| `DELETE FROM d WHERE id = ?` | literal | 5 of 6 | -7.78 µs | -7.61 µs |
| `DELETE FROM d WHERE id = ?` | prepared | 4 of 6 | -7.33 µs | -7.21 µs |
| `UPDATE r SET v = ? WHERE id >= 1` (every one of 1,000 rows) | literal | 7 of 8 | -228.82 µs | -227.75 µs |
| `UPDATE r SET v = ? WHERE id >= 1` (every one of 1,000 rows) | prepared | 6 of 8 | -247.80 µs | -259.96 µs |
| `SELECT * FROM s WHERE w = ?` (10,000 rows, unindexed) | literal | 6 of 8 | -37.74 µs | -37.74 µs |
| `SELECT * FROM s WHERE w = ?` (10,000 rows, unindexed) | prepared | 7 of 8 | -35.97 µs | -37.01 µs |

**Per-run paired deltas**, literal path, so the extremes are visible:

| statement | run 0 | run 1 | run 2 | run 3 | run 4 | run 5 | run 6 | run 7 |
|---|---|---|---|---|---|---|---|---|
| `C_PING` frame (control: no statement, no dispatcher) | +0.7 µs | 0.0 µs | -2.8 µs | +0.1 µs | -0.1 µs | +0.0 µs | +0.1 µs | 0.0 µs |
| `SHOW META` | -10.8 µs | -3.9 µs | -11.3 µs | -3.8 µs | -4.6 µs | -4.3 µs | -4.3 µs | -4.6 µs |
| `SELECT * FROM t WHERE id = ?` | -14.7 µs | -8.9 µs | -9.1 µs | -8.1 µs | -9.4 µs | -9.1 µs | -9.8 µs | -9.3 µs |
| `UPDATE t SET v = ? WHERE id = ?` | -0.3 µs | -9.3 µs | -7.9 µs | -7.8 µs | -7.5 µs | -8.2 µs | -8.0 µs | -6.7 µs |
| `UPDATE t SET v = v + 1 WHERE id = ?` | -8.7 µs | -9.9 µs | -8.1 µs | -7.9 µs | -9.3 µs | -8.4 µs | -9.0 µs | -7.5 µs |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | -15.1 µs | -18.5 µs | -14.7 µs | -18.0 µs | -14.3 µs | -20.8 µs | -14.7 µs | -24.8 µs |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | -12.4 µs | -15.9 µs | -12.4 µs | -15.8 µs | -11.8 µs | -16.3 µs | -12.4 µs | -17.1 µs |
| `INSERT INTO i VALUES (?, ?, ?)` | -7.5 µs | -8.5 µs | -7.3 µs | -7.5 µs | -7.2 µs | -3.8 µs | -7.0 µs | -6.6 µs |
| `UPDATE r SET v = ? WHERE id >= 1` (every one of 1,000 rows) | -244.2 µs | -234.0 µs | -211.3 µs | -191.4 µs | -200.8 µs | -226.7 µs | -239.6 µs | -228.8 µs |
| `SELECT * FROM s WHERE w = ?` (10,000 rows, unindexed) | -30.5 µs | -37.8 µs | -37.6 µs | -31.8 µs | -70.6 µs | -68.7 µs | -32.2 µs | -126.9 µs |

**Not resolved or not attributed**

- **BN-S3's and BN-S5's share of a point `UPDATE`** and **the split of the every-row `UPDATE`'s 214.5 µs between BN-S3's
  decode and undo hold and BN-S5's encode and before-image**: no stage binary exists.
- **The scan's remainder**: about 14 µs of the walk's 29 µs gain is not in `ExecStats::For`, and the profile's movements
  there are inside its sampling error.
- **A mid-arm bump** in the literal `SELECT` arm: blocks 4-7 (counting from 0) read 4-6.5 µs higher in server CPU on
  both binaries (A 38.1, 37.2, 40.0 and 39.4 µs against 33.2-34.5 in the other blocks, B 31.0, 30.6, 33.1 and 30.5
  against 26.1-27.7, as medians over runs), in the same blocks of most runs. Symmetric, so inside the paired difference;
  not investigated.
- **`SHOW META`'s smaller wall gain** (section 1).
- **A profile of A's point statements**, and PostgreSQL twins for anything but the scan.
- **The server's idle burn** is A 5,065 µs/s [4,448, 5,610], B 4,859 µs/s [4,416, 5,601] over 14 runs, about 0.5 % of one CPU on both binaries; reported, not subtracted.

## 8. What the run teaches

1. **A point statement is now mostly request path; the engine's own storage work is a tenth of the server CPU.** Send
   and read are 59 % of a point `UPDATE`'s CPU in B, parse and compile 8 %, scaffolding 10 %, and the storage side 2.3
   µs. Any further point-statement gain is in the request path BN-Q8 put out of BN, in the dispatcher's scaffolding or in
   the client's decode of the reply - not in undo, locks or the tree.
2. **The order's §0 reading of the point `SELECT` does not hold.** `instructions/v3.0.0/workorder-bn-opt-read-update-path.md`
   §0 (from BJ's close) calls the `SELECT`'s 17.5 µs over `SHOW META` "the largest engine-side share in the table". On A
   the two differ by 15.0 µs of wall, **2.2 µs of server CPU** and **12.5 µs of the client's CPU**; on B by 10.3, 3.5
   and 7.8 µs. The extra is mostly the client decoding a result set, and `SHOW META` is not "a statement that touches
   nothing": 31 % of its 23.6 µs of server CPU is statement scaffolding, 7.4 µs (BN-S1 read that part as the
   statement's own text), against 1.9-4.3 µs in the other point statements.
3. **A second flush costs the client as much as the server, and overlapped CPU is not free.** Removing it took 17.2 µs
   of CPU out of a point `SELECT` for a 9.0 µs fall in wall time; the 8.1 µs difference had been running concurrently.
4. **A thousand updates of a spilled row no longer leak 38 pages, and the update no longer touches the var-heap at all
   unless it assigns the spilled cell** (var-heap append: 0.0 µs listed, 0.4 µs when assigned). The per-statement cost
   no longer depends on how many values the relation holds; A's did, by about 1 µs per 1,000 updates.
5. **The undo and WAL records are the every-row `UPDATE`'s floor.** At 38 % of B's CPU and unchanged by BN, with the row
   visit around them down by 174 µs, they are per record, not per byte; BN-R5's discard stands and CN-7's O5 has its data
   point: no record size is material on a three-column row.
6. **The scan row is decode-bound and BN barely touched it.** `ExecStats::For` is the only read-path cut (1.5 ns of 67);
   decode (19.9 ns), the residual (11.5 ns) and `AcceptTupleAt`'s own body (11.7 ns) are the next candidates. **§1.4's
   question, the uncontended `window_latch_`, is not answered**: the snapshot is applied at exactly one place,
   `AcceptTupleAt` (`include/kds/exec/step_vm.hpp:275`), and no `Classify` or `ReadView` frame reaches the profile's self
   list (cut-off 0.22 %, 0.16 ns a row), so the classification and its latch are inlined into one of the walk's bodies
   (`AcceptTupleAt` 11.7 ns, `BtreeVisitLeafPage` 6.5 ns, the callback 6.6 ns) or cost under 0.16 ns, and the profile
   cannot say which.
7. **A second reading for BN-Q13's BN-S2 (not built).** Parse plus compile on the prepared path, in B, is 1.2-4.5 µs,
   6-15 % of server CPU and 2.9-10.2 % of the wall time (table below). BN-Q1's bar reads 5 % of the statement's server
   time, which every row meets; three of the seven reach 5 % of the wall. BN-Q13 chose not to build BN-S2 on 2.2-4.2 µs
   (7-12 %); these are the close's figures, on one core, `relaxed`, by a driver that prepares.

| statement (prepared path, B) | parse | compile | parse + compile | share of server CPU | share of wall p50 |
|---|---|---|---|---|---|
| `SELECT * FROM t WHERE id = ?` | 1.8 µs | 0.8 µs | 2.6 µs | 10 % of 25.7 µs | 4.8 % of 53.8 µs |
| `UPDATE t SET v = ? WHERE id = ?` | 1.3 µs | 0.4 µs | 1.7 µs | 7 % of 23.4 µs | 4.0 % of 42.4 µs |
| `UPDATE t SET v = v + 1 WHERE id = ?` | 3.3 µs | 1.2 µs | 4.5 µs | 15 % of 30.6 µs | 10.2 % of 44.2 µs |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | 1.5 µs | 0.4 µs | 1.9 µs | 8 % of 23.4 µs | 4.5 % of 42.6 µs |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | 2.7 µs | 0.5 µs | 3.2 µs | 11 % of 28.2 µs | 6.9 % of 46.2 µs |
| `INSERT INTO i VALUES (?, ?, ?)` | 1.2 µs | 0.0 µs | 1.2 µs | 6 % of 21.3 µs | 2.9 % of 41.1 µs |
| `DELETE FROM d WHERE id = ?` | 1.9 µs | 0.4 µs | 2.3 µs | 9 % of 26.0 µs | 5.8 % of 39.6 µs |

**What bound each cell, and what nothing did.** A point statement is bound by the request path (59 % of its CPU) and by
the client; the every-row `UPDATE` by the per-record undo and WAL work (38 %) and the row visit; the scan by decode and
evaluation. **No cell approached a throughput limit**: one synchronous session measures a statement's service time, not
the engine's capacity, and nothing here ran at saturation, with `strict` or `group`, or on more than one core (the
operator's direction of 2026-08-31 to measure at the extreme); the optimise-and-re-measure loop this closes is the
request path's and the every-row visit's, and the next candidates are named in items 1 and 6.

## Not run

- **`strict` and `group` durability.** Every number is `relaxed`: no statement waits on a sync, so this run says nothing
  about what a commit costs when it must.
- **`cores` above 1**, more than one session, and any saturation or contention cell. BN-S9's tail hint is a table of
  relaxed atomics shared by the cores; its cell tests two growers, this run does not price them.
- **Row-set sizes**: 200 and 10,000 rows for the point and every-row shapes, 200 and 1,000 for the scan (the flags are
  `--rows` and `--scan-rows`); a point lookup should not scale, and the scan and every-row figures per row are one-size.
- **Wider rows and several spilled cells.** Every row here has three `int64`s or two and one 300-byte `varchar`; the
  partial decode's saving grows with the columns not read, and a row with two spilled cells was not built.
- **A relation with a Cabin, an index or an assertion**, which decodes its row whole (BN-S3's rule) and so does not
  take the decode half of BN-R2.
- **Larger var-heap chains.** A census relation's chain reaches 7,000 values (1,000 loaded, two arms of 3,000); B's
  profile of the assigning statement reaches 61,000 (two arms of 30,000), and A was profiled on no spilled statement.
- **A profile of A's point statements**, and a stage-by-stage build.
- **`HEAP` relations** (suspended, SUS-1), **the suite**, and PostgreSQL twins other than the scan.

## Baseline for the next run

The next measurement of these shapes reads against **`v2.7.0-787-g8679367e` (this file)**, `cores = 1`, `relaxed`, one
session, 1,000-row relations, medians of per-run p50 over 8 runs (literal path): point `SELECT` 56.5 µs, point `UPDATE`
45.7 µs, `v + 1` 47.9 µs, spilled-row `UPDATE` 45.7 µs (assigned 50.8 µs), `INSERT` 44.5 µs, `DELETE` 42.3 µs (6
runs), `SHOW META` 46.2 µs, `C_PING` 26.2 µs; every-row `UPDATE` 763.6 µs (1,000 rows); the 10,000-row scan 728.9 µs,
699.8 µs of server CPU, 67.2 ns net a row; PostgreSQL 18.6's scan row 34.3 ns. The tables below are the full per-arm
distributions.

## Appendix A. Every arm, A and B

`n` is statements (scans, every-row statements) per arm over the 8 runs; each column is the median over the runs of the
per-run value; **p90 stands where the driver does not emit p95**; `mean` includes the stalls of the tail. Units in the
cells; the every-row and scan arms are in whole µs.

| statement | path | arm | n | p0 | p25 | p50 | p90 | p99 | mean | server CPU | driver CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `C_PING` frame (control: no statement, no dispatcher) | literal | A | 24,000 | 18.5 µs | 25.8 µs | 26.2 µs | 29.0 µs | 40.9 µs | 27.3 µs | 12.2 µs | 15.2 µs |
| `C_PING` frame (control: no statement, no dispatcher) | literal | B | 24,000 | 18.5 µs | 25.8 µs | 26.2 µs | 29.3 µs | 41.0 µs | 27.3 µs | 12.3 µs | 15.3 µs |
| `SHOW META` | literal | A | 24,000 | 44.4 µs | 49.8 µs | 50.5 µs | 59.0 µs | 72.3 µs | 52.8 µs | 33.5 µs | 29.8 µs |
| `SHOW META` | literal | B | 24,000 | 42.5 µs | 45.7 µs | 46.2 µs | 56.3 µs | 69.7 µs | 48.9 µs | 24.6 µs | 24.9 µs |
| `SHOW META` | prepared | A | 24,000 | 43.3 µs | 47.8 µs | 48.4 µs | 56.0 µs | 68.7 µs | 50.5 µs | 32.6 µs | 28.0 µs |
| `SHOW META` | prepared | B | 24,000 | 40.9 µs | 43.3 µs | 43.9 µs | 50.1 µs | 65.3 µs | 46.0 µs | 23.5 µs | 22.8 µs |
| `SELECT * FROM t WHERE id = ?` | literal | A | 24,000 | 59.5 µs | 63.7 µs | 65.5 µs | 78.2 µs | 97.9 µs | 70.2 µs | 35.7 µs | 42.3 µs |
| `SELECT * FROM t WHERE id = ?` | literal | B | 24,000 | 52.6 µs | 54.9 µs | 56.5 µs | 68.0 µs | 86.1 µs | 60.5 µs | 28.1 µs | 32.7 µs |
| `SELECT * FROM t WHERE id = ?` | prepared | A | 24,000 | 58.0 µs | 60.9 µs | 63.4 µs | 75.4 µs | 93.7 µs | 66.9 µs | 35.0 µs | 39.4 µs |
| `SELECT * FROM t WHERE id = ?` | prepared | B | 24,000 | 49.8 µs | 51.9 µs | 53.8 µs | 65.5 µs | 83.4 µs | 57.7 µs | 28.4 µs | 29.9 µs |
| `UPDATE t SET v = ? WHERE id = ?` | literal | A | 24,000 | 47.2 µs | 53.0 µs | 53.8 µs | 64.4 µs | 82.8 µs | 81.4 µs | 31.9 µs | 34.3 µs |
| `UPDATE t SET v = ? WHERE id = ?` | literal | B | 24,000 | 37.5 µs | 45.2 µs | 45.7 µs | 53.9 µs | 71.9 µs | 50.0 µs | 24.5 µs | 24.9 µs |
| `UPDATE t SET v = ? WHERE id = ?` | prepared | A | 24,000 | 43.8 µs | 50.4 µs | 51.3 µs | 60.4 µs | 77.0 µs | 54.6 µs | 30.0 µs | 31.9 µs |
| `UPDATE t SET v = ? WHERE id = ?` | prepared | B | 24,000 | 36.2 µs | 41.9 µs | 42.4 µs | 51.3 µs | 69.8 µs | 45.6 µs | 23.5 µs | 22.6 µs |
| `UPDATE t SET v = v + 1 WHERE id = ?` | literal | A | 24,000 | 48.9 µs | 55.9 µs | 56.6 µs | 67.4 µs | 88.7 µs | 60.2 µs | 33.6 µs | 34.3 µs |
| `UPDATE t SET v = v + 1 WHERE id = ?` | literal | B | 24,000 | 40.0 µs | 47.4 µs | 47.9 µs | 57.2 µs | 76.5 µs | 66.5 µs | 27.1 µs | 24.8 µs |
| `UPDATE t SET v = v + 1 WHERE id = ?` | prepared | A | 24,000 | 44.7 µs | 52.8 µs | 53.7 µs | 64.2 µs | 85.7 µs | 57.6 µs | 32.7 µs | 31.5 µs |
| `UPDATE t SET v = v + 1 WHERE id = ?` | prepared | B | 24,000 | 37.1 µs | 43.7 µs | 44.2 µs | 53.6 µs | 75.0 µs | 48.5 µs | 25.8 µs | 21.9 µs |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | literal | A | 24,000 | 54.8 µs | 60.8 µs | 62.3 µs | 80.6 µs | 114.3 µs | 70.0 µs | 41.6 µs | 35.1 µs |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | literal | B | 24,000 | 37.6 µs | 45.0 µs | 45.7 µs | 57.4 µs | 84.5 µs | 50.8 µs | 24.7 µs | 25.2 µs |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | prepared | A | 24,000 | 53.9 µs | 58.8 µs | 60.0 µs | 73.1 µs | 98.5 µs | 65.5 µs | 39.4 µs | 32.2 µs |
| `UPDATE sp SET v = ? WHERE id = ?` (row has a spilled cell, not assigned) | prepared | B | 24,000 | 35.7 µs | 41.9 µs | 42.6 µs | 52.3 µs | 73.6 µs | 46.3 µs | 23.5 µs | 22.5 µs |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | literal | A | 24,000 | 58.0 µs | 63.3 µs | 65.0 µs | 80.8 µs | 144.5 µs | 77.7 µs | 41.9 µs | 35.4 µs |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | literal | B | 24,000 | 44.6 µs | 49.7 µs | 50.8 µs | 66.0 µs | 115.7 µs | 63.8 µs | 29.7 µs | 26.2 µs |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | prepared | A | 24,000 | 54.4 µs | 60.9 µs | 62.4 µs | 76.0 µs | 99.8 µs | 68.0 µs | 41.5 µs | 32.8 µs |
| `UPDATE sa SET txt = ? WHERE id = ?` (the spilled cell assigned) | prepared | B | 24,000 | 41.4 µs | 45.5 µs | 46.2 µs | 58.1 µs | 78.1 µs | 52.3 µs | 27.8 µs | 23.5 µs |
| `INSERT INTO i VALUES (?, ?, ?)` | literal | A | 24,000 | 44.1 µs | 50.5 µs | 52.4 µs | 78.2 µs | 118.2 µs | 60.5 µs | 29.7 µs | 33.5 µs |
| `INSERT INTO i VALUES (?, ?, ?)` | literal | B | 24,000 | 37.2 µs | 43.3 µs | 44.5 µs | 63.1 µs | 102.7 µs | 51.5 µs | 23.3 µs | 24.8 µs |
| `INSERT INTO i VALUES (?, ?, ?)` | prepared | A | 24,000 | 42.1 µs | 47.8 µs | 48.5 µs | 59.1 µs | 107.4 µs | 57.4 µs | 28.3 µs | 31.8 µs |
| `INSERT INTO i VALUES (?, ?, ?)` | prepared | B | 24,000 | 35.3 µs | 40.6 µs | 41.1 µs | 50.0 µs | 91.7 µs | 51.1 µs | 22.1 µs | 22.8 µs |
| `UPDATE r SET v = ? WHERE id >= 1` (every one of 1,000 rows) | literal | A | 480 | 934 µs | 958 µs | 987 µs | 1,195 µs | 2,719 µs | 1,122 µs | 1,003 µs | 69 µs |
| `UPDATE r SET v = ? WHERE id >= 1` (every one of 1,000 rows) | literal | B | 480 | 699 µs | 722 µs | 764 µs | 1,006 µs | 1,989 µs | 887 µs | 744 µs | 59 µs |
| `UPDATE r SET v = ? WHERE id >= 1` (every one of 1,000 rows) | prepared | A | 480 | 941 µs | 1,017 µs | 1,074 µs | 1,652 µs | 3,229 µs | 1,218 µs | 989 µs | 67 µs |
| `UPDATE r SET v = ? WHERE id >= 1` (every one of 1,000 rows) | prepared | B | 480 | 691 µs | 746 µs | 818 µs | 1,324 µs | 2,219 µs | 948 µs | 768 µs | 53 µs |
| `SELECT * FROM s WHERE w = ?` (10,000 rows, unindexed) | literal | A | 1,536 | 722 µs | 758 µs | 783 µs | 851 µs | 1,139 µs | 794 µs | 731 µs | 58 µs |
| `SELECT * FROM s WHERE w = ?` (10,000 rows, unindexed) | literal | B | 1,536 | 691 µs | 719 µs | 729 µs | 786 µs | 1,219 µs | 749 µs | 700 µs | 48 µs |
| `SELECT * FROM s WHERE w = ?` (10,000 rows, unindexed) | prepared | A | 1,536 | 720 µs | 754 µs | 766 µs | 812 µs | 1,013 µs | 791 µs | 732 µs | 54 µs |
| `SELECT * FROM s WHERE w = ?` (10,000 rows, unindexed) | prepared | B | 1,536 | 688 µs | 713 µs | 724 µs | 763 µs | 1,372 µs | 744 µs | 698 µs | 45 µs |

### The DELETE supplement (6 runs)

| statement | path | arm | n | p0 | p25 | p50 | p90 | p99 | mean | server CPU | driver CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `DELETE FROM d WHERE id = ?` | literal | A | 18,000 | 42.4 µs | 49.2 µs | 50.1 µs | 57.3 µs | 77.2 µs | 59.2 µs | 28.5 µs | 32.9 µs |
| `DELETE FROM d WHERE id = ?` | literal | B | 18,000 | 34.9 µs | 41.8 µs | 42.3 µs | 50.0 µs | 66.1 µs | 44.7 µs | 21.4 µs | 23.8 µs |
| `DELETE FROM d WHERE id = ?` | prepared | A | 18,000 | 40.3 µs | 46.2 µs | 46.8 µs | 54.8 µs | 75.1 µs | 49.6 µs | 27.1 µs | 30.6 µs |
| `DELETE FROM d WHERE id = ?` | prepared | B | 18,000 | 32.4 µs | 39.2 µs | 39.6 µs | 45.5 µs | 66.0 µs | 43.2 µs | 20.8 µs | 21.6 µs |
| `C_PING` frame (control: no statement, no dispatcher) | literal | A | 18,000 | 17.8 µs | 25.9 µs | 26.2 µs | 28.8 µs | 48.4 µs | 31.6 µs | 12.1 µs | 15.0 µs |
| `C_PING` frame (control: no statement, no dispatcher) | literal | B | 18,000 | 18.0 µs | 25.8 µs | 26.1 µs | 29.1 µs | 49.8 µs | 30.0 µs | 12.0 µs | 15.0 µs |
| `SELECT * FROM t WHERE id = ?` | literal | A | 18,000 | 59.2 µs | 63.2 µs | 65.2 µs | 76.2 µs | 100.9 µs | 70.2 µs | 35.3 µs | 41.4 µs |
| `SELECT * FROM t WHERE id = ?` | literal | B | 18,000 | 52.2 µs | 54.7 µs | 56.8 µs | 70.0 µs | 119.5 µs | 64.7 µs | 28.6 µs | 32.2 µs |
| `SELECT * FROM t WHERE id = ?` | prepared | A | 18,000 | 54.4 µs | 60.5 µs | 62.2 µs | 75.6 µs | 148.0 µs | 78.5 µs | 34.9 µs | 38.9 µs |
| `SELECT * FROM t WHERE id = ?` | prepared | B | 18,000 | 46.4 µs | 51.5 µs | 52.7 µs | 61.5 µs | 81.1 µs | 57.6 µs | 26.8 µs | 29.4 µs |

## Appendix B. Reproducing

All from `/home/cdkbs/ckdbs/.claude/worktrees/bk-read-update-paths`, with the copies named in the stamp table; the
drivers are the branch's `tools/` at the commit that adds this file:

    python3 tools/bn_census.py --bin /home/cdkbs/bench-runs/bk/bin/kds_server-A-b70f79d8 \
        --bin-b /home/cdkbs/bench-runs/bk/bin/kds_server-B-8679367e \
        --workdir /home/cdkbs/bench-runs/bk/close/wd --port 31961 --runs 8 \
        --quiet-load 4 --quiet-wait 300 --out ab.json
    python3 tools/bn_census.py --bin ... --bin-b ... --workdir .../wdd --port 31991 --runs 6 \
        --quiet-load 4 --quiet-wait 300 --only noop,delete-pk,select-pk --out ab-delete.json
    python3 tools/bn_census.py --bin .../kds_server-Bprof-8679367e --workdir .../wdp --port 31971 \
        --sampler .../kds_sampler.so --sampler-dir .../profB --runs 0 --profile-ops 30000 \
        --profile-range-ops 1500 --profile-scan-ops 2000 --out profB.json
    (A: --bin .../kds_server-Aprof-b70f79d8 --port 31981 --profile-only update-range,scan)
    python3 tools/bn_pg_scan.py --port 15434 --database bench --runs 5 --out pg.json
    python3 tools/bn_census_table.py ab.json
