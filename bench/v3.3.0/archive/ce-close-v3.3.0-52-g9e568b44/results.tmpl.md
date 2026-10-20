# CE close: a streamed KWP reply reaches the client's first batch 54 % to 93 % sooner, hides 95 % to 99 % of the idle CE-S0 priced, and costs a client that does no work up to 4.4 % on small replies

**Thesis.** On `ce-streaming-reply` at engine commit `9e568b44` (`v3.3.0-52-g9e568b44`, **B**) against the engine at CE's opening commit `1f57f37b` (**A**, the binary CE-S0 measured), interleaved A/B/B/A/A/B in one window, three runs per binary:
a plain unsorted `SELECT` whose walk crosses 64-page slice boundaries now delivers its first `S_ROW_BATCH` at **1,847.5 µs (10,000 rows), 1,912.5 µs (60,480 rows) and 387.6 µs (the 7,560-row filtered walk)** where A delivers it at 4,040.8 µs, 26,673.7 µs and 4,346.6 µs (-54.3 %, -92.8 %, -91.1 %, p50, every one outside both binaries' run-to-run spreads).
The client's idle time per statement falls by the amount CE-S0 predicted: **idle removed over CE-S0's ceiling is 0.95 to 0.99 on all six boundary-crossing cells** (`send` and `split`, 10,000 rows, 60,480 rows, filtered walk), and the statement is that much shorter on the path the scenario drivers use: wall -6.0 %, -7.0 %, -12.0 % (throughput +6.4 %, +7.5 %, +13.7 %); on the faster `split` client -10.3 %, -14.1 %, -20.7 % (+11.5 %, +16.5 %, +26.1 %).
Replies of 1,000 rows or fewer (one slice, no boundary) are unchanged on a client that decodes (all 6 wall cells inside their spreads, largest |difference| 1.8 %), and CE-S0's prediction for `bar-range`, `model-join` and `books-by-genre` holds (+0.4 %, +0.7 %, +5.2 %, all inside their spreads).
**One cost the premise did not name:** a client that does no work (the `drain` path) sees the reply complete 4.2 % later at 1,000 rows (+20.2 µs), 2.6 % later at 10,000 rows and 4.4 % later on the filtered walk (+197.4 µs), against 5.9 % sooner at 60,480 rows; server CPU per statement moves the same way (section 5). A client that decodes never sees it.
The overhead control, point statements, shows no change outside its spreads (section 8).

Drivers and exact invocations: `archive/ce-close-v3.3.0-52-g9e568b44/run_close.sh` (master), `run_drain.sh` (supplement), `rowcost/ce_s0_rows.py`, `ttfb_probe.py`, the unmodified `tools/scenario1_backtest.py`, `tools/scenario3_library.py` and `tools/bn_census.py`; their flags are in the tools' own `--help` and docstrings (`bench/README.md`: `bench/docs/` stays closed). Measured by `ck-tester`. Work order: `instructions/v3.2.0/workorder-ce-streaming-reply.md` (CE-S5, section 6). Baseline for the premise: `bench/v3.3.0/results-ce-s0-premise-v3.3.0-50-g1f57f37b.md` (CE-S0), whose `send` walls A reproduces to within 1.6 % at the five sizes (section 9).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time executed | 2026-10-20, 10:57 to 11:37 UTC (task start 10:57:11). Build 10:57:50 to 10:59:19. Sweep cells 11:02:52 to 11:22:38; scenario cells 11:04:29 to 11:24:49 (12 cells, 37 s and 24 s each); census 11:24:53 to 11:27:21; `drain` supplement 11:29:22 to 11:36:28 UTC. Per-cell windows: `timeline`, `windows.txt`, `logs/*.t0`, `*.t1` in the archive |
| Worktree and branch | `ce-streaming-reply` (`/home/cdkbs/ckdbs/.claude/worktrees/ce-streaming-reply`), branch `ce-streaming-reply` |
| Commit measured | **B**: engine source at `9e568b44`, `git describe --tags` = `v3.3.0-52-g9e568b44`. Tree at the build: `src/`, `include/`, `tests/`, `CMakeLists.txt` and `tools/` clean; four uncommitted documents under `docs/` (`known-gaps.md`, `client-manual.md`, `protocol.md`, `sched.md`). HEAD moved to `c3ce8d55` (`v3.3.0-53-gc3ce8d55`) at 11:02:48 UTC, during the run, with those four documents and nothing else (`git diff --stat 9e568b44 c3ce8d55 -- src include CMakeLists.txt tools tests` is empty), so the engine measured is both commits' |
| Binary B | built in this run: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`, `cmake --build build-release --target kds_server -j4`, 1 min 29 s; **mtime 2026-10-20 10:59:19.31 UTC, newer than HEAD's commit (10:56:45 UTC)**. Copied with `cp -p` to `/home/cdkbs/bench-runs/ce-close-v3.3.0-52-g9e568b44/bin/kds_server-B`, **sha256 `85a168627b666a95c8bfddc7fd36949d58435b18003c258df8338dacc7dd536e`**; every B server started from the copy |
| Binary A | the existing copy `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/kds_server`, mtime 2026-10-20 03:37:20.03 UTC, `cp -p` to `bin/kds_server-A`, **sha256 `7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a`** (the value the task named). Built at `9682b725`, Release; its engine source equals `1f57f37b` (CE-S0's check, `git diff 9682b725 1f57f37b -- src include CMakeLists.txt` empty); the binary is older than HEAD and measures the engine CE started from |
| Build type | Release for both (`CMAKE_BUILD_TYPE:STRING=Release` in `build-release/CMakeCache.txt`; A per CE-S0) |
| Host | AMD EPYC 9V74, 8 vCPUs (4 cores x 2 threads), Linux 7.0.0-1014-azure; 15 GiB RAM |
| Data device | `/dev/root`, **ext4** (`df -T $HOME`, checked at run time), data files under `/home/cdkbs/bench-runs/ce-close-v3.3.0-52-g9e568b44/data/`; not tmpfs; deleted after every cell. Free space stayed above 9 GB (the 2 GB stop-rule was armed in every cell) |
| Server configuration | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`; no other key (`conf/*.conf`). The census (`bn_census.py`) runs its own servers at `cores = 1`, `durability = relaxed` |
| Ports and placement | 15761 (sweep, probe, supplement), 15610 (scenario 1), 15620 (scenario 3), 34021 (census); 15432 and 15434 untouched; no `pkill`, servers stopped by recorded PID. Sweep, probe, supplement: server pinned to CPU 2 and client to CPU 6 (`taskset`; not siblings of each other). Scenario cells unpinned. Census: server CPU 2, driver CPU 4 |
| Quiet host | Before every cell a gate (no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process, at most 8 % busy over 3 s, six checks in a row; capped at 240 s) and a 1-second monitor (`monitor.log.gz`) checked each cell against its window afterwards. Another session ran `kds_tests` from before 10:57 UTC to about 11:02 UTC; the first gate waited 165 s for it, every later gate 25 s. **Zero sweep, probe or scenario cells were set aside.** One `drain` cell was (section 9) |
| Client | `tools/` copied from this worktree at `9e568b44` into the run directory and put on `PYTHONPATH`; Python 3.12.3; the `a40882af` decoder |
| PostgreSQL | not measured for this shape (rule 4: the floor is optional) |
| Suite | **not executed in this task**; no source was changed by this measurement. The step's suite run is the parent's |

## 2. What was run, and what was cut

A and B alternated in one window: pair 1 A,B; pair 2 B,A; pair 3 A,B (so each binary starts first in half the pairs). Each (pair, binary) visit ran three fresh servers on fresh data files: the sweep with the probe, scenario 1, scenario 3. "Spread" everywhere is (max - min) / median over a binary's three runs (six for the census); **a B-over-A difference smaller than the larger of the two spreads is not a finding.**

| Cell | Server | What varies | Per run | What it records |
|---|---|---|---|---|
| Row-cost sweep, `SELECT * FROM daily_stats WHERE id BETWEEN 1 AND N`, N = 1, 100, 1,000, 10,000, 60,480, and `WHERE symbol_id = 3` (7,560 rows of 60,480 walked) | cores 1, group, CPU 2 | rows returned (1 to 60,480; the 200 / 1K / 10K of rule 9 are the 100, 1,000 and 10,000 rows, and the table is 60,480 rows, 877 pages, 69 rows per page); client path `send` (the scenario drivers' own: decode and render every cell to text) and `split` (cells become `bytes`) | 1 cell per (shape, path), about 2.5 s of statements each (7 to about 10,700 statements), 3 runs per binary | wall, client CPU, server CPU (`/proc/<pid>/stat`, 10 ms ticks) per statement, every statement's latency |
| Time to first byte / first batch / ready, `ttfb_probe.py` | same server and table | the same six statements | 100 timed after 3 warm-ups (60,480 rows: about 100 in the 5 s cap), 3 runs per binary | microseconds from before `sendall` to the `recv` that completed each frame; every reply's frame types, batches, bytes, error flags |
| Supplement, `drain` path | same | the same six statements, frame headers only (no cell touched) | 1 cell per shape, 3 runs per binary | wall and server CPU of a client that does nothing |
| Scenario 1, `c1-g` | cores 1, group | `--seed 1 --bars-clustered btree --verify --sync --analyze`, 60,480 bars; `bar-range` 200 rows, `symbol-history` 7,560, `model-join` 359, `day-slice` 8 | 3 per binary | the driver's QPS matrix and phases; `verify_problems` empty in 6 of 6 |
| Scenario 3, unindexed | cores 1, group | `--loans 10000 --index-mode none --server-indexes on --seed 1` | 3 per binary, 200 statements per shape | `books-by-genre`, `loans-by-daterange`, `overdue`, and `loans-by-user` (5 rows, no boundary possible: the control) |
| Overhead control, `bn_census.py` | cores 1, relaxed, two servers side by side, one busy at a time | `noop`, `ping`, `select-pk` (point), `update-lit` (point), literal and prepared | 6 runs x 6 interleaved blocks, 1,998 statements per arm per run | wall and server CPU (`schedstat` over every server thread) per statement |

**Cut** (about an hour, building included; the build and every measurement finished 39 minutes after the task started): the sweep's `drain` path in the main matrix (run as a smaller supplement instead: one cell per shape); `values` path; `strict` and `relaxed` durability for the many-row cells and `cores` above one (every cell is `cores = 1`, `group`); scenario 1 and 3 at smaller sizes (rule 9 is carried by the sweep's 1 to 60,480 rows); the census's insert, delete, scan and every-row update shapes; a sampler run for server CPU per slice (the sweep's server CPU has 10 ms resolution, 4 % of a 60,480-row statement); the client that stops reading, the horizon held while parked and the user timeout (CE-S4's own cells, which are tests and not measurements); PostgreSQL.

## 3. Time to first batch

**The first batch now arrives after one slice of server work, wherever the walk ends.** At 10,000 rows (146 pages, 3 slices) the first `S_ROW_BATCH` leaves at 1,847.5 µs against 4,040.8 µs; at 60,480 rows (877 pages) at 1,912.5 µs against 26,673.7 µs, 7.2 % of B's time to the end of the reply where A's is 94.4 %; on the filtered walk at 387.6 µs against 4,346.6 µs. B's three first-batch numbers are close to CE-S0's derived server CPU for one 64-page slice (1,863 µs, 2,076 µs and 331 µs; derived by CE-S0, not re-measured here). For 1,000 rows or fewer the whole reply is one slice and nothing changes (+0.3 %, -0.3 %, +4.0 %, each inside its spread).

Medians over the runs of each run's p50 (100 replies per run; 60,480 rows: the replies fitting the 5 s cap, 100 here). B over A is the change in the first-batch time; "ready" is `S_READY`, the reply over.

@@table analysis.md ^\| shape \| rows returned \| A first batch 0@@

Every percentile (each run's, then the median over the three runs):

@@table analysis.md ^\| shape \| rows returned \| batches \(A / B\) 0@@

Reply shape is identical between A and B on all six statements: the same frame order (`S_PARSE_OK`, `S_BIND_OK`, `S_ROW_DESC`, then batches), the same number of batches (1, 1, 3, 22, 130, 17), the same bytes (476 B, 14,336 B, 140,356 B, 1,400,546 B, 8,468,826 B, 1,058,896 B), and zero error flags in every reply of the six probe runs:

@@table analysis.md ^\| shape \| binary \| first_types 0@@

The first byte and the first batch are one event in B at every size, as in A (within 20 µs): CE writes whole sealed batches at the slice boundary, never a partial one.

## 4. The sweep: throughput, idle and the hidden fraction

**On the scenario drivers' client path a boundary-crossing reply is 6.0 % to 12.0 % shorter, on a faster client 10.3 % to 20.7 %; a reply inside one slice is unchanged.** Throughput (rule 5a: QPS, derived as 1,000,000 / wall per statement):

@@table headline.md ^\| shape \| rows returned \| A QPS 0@@

@@table headline.md ^\| shape \| rows returned \| A QPS 1@@

The first table is `send`, the second `split`. The per-statement walls, client CPU, idle and server CPU are in the appendix (section 12).

**The hidden fraction against CE-S0's ceiling** (CE-S0 section 6: the most a slice-boundary stream can remove is `idle x (1 - 1 / slices)`, with 2.28 slices at 10,000 rows and 13.70 at 60,480 rows and the filtered walk; here A's idle is taken from this window). *Wall hidden* is A's wall minus B's; *idle removed* is A's idle minus B's (idle = wall minus client CPU):

@@table analysis.md ^\| path \| shape \| rows returned \| slices 0@@

- **Idle removed is the mechanism, and it meets the ceiling: 0.95 to 0.99 of it on six of six boundary-crossing cells.** What remains in B is the first slice, which nothing can overlap: B's idle is 1,964.8 µs at 10,000 rows, 2,274.7 µs at 60,480 rows and 442.9 µs on the filtered walk (`send`), against the model's 1/2.28, 1/13.7 and 1/13.7 of A's idle (1,856 µs, 1,958 µs, 349 µs).
- **Wall hidden follows idle removed, with the client's own CPU as the difference.** Measured as a share of A's wall: 6.0 %, 7.0 %, 12.0 % (`send`) and 10.3 %, 14.1 %, 20.7 % (`split`), against ceilings of 4.9 %, 7.9 %, 12.2 % and 8.7 %, 13.5 %, 20.4 %. The ratio exceeds 1 only at 10,000 rows (1.23 and 1.18), where B's client CPU is also lower (-563.7 µs `send`, inside the 3.0 % spread; -315.3 µs `split`, outside its 1.0 % spread). Client CPU has no consistent sign across the cells (`send`: -1.3 %, +0.8 %, -0.1 %), so the excess is not credited to the mechanism.
- **Server CPU per statement did not change** at any size on either path (largest difference -6.7 % at `split` 60,480 rows, spreads 7.7 % and 0.4 %, and -5.7 % at `send` 1 row, spreads 4.3 % and 6.6 %; the 10 ms tick is 4 % of a 60,480-row statement, so a difference under about 4 % is not resolvable there). Streaming moves the server's work earlier relative to the client; it adds none that this instrument sees. The `drain` path (section 5) has the sharper instrument and does see a small one.
- CE-S0's three ceilings (5.0 %, 7.9 %, 12.1 % of the `send` wall) were derived; they are measured here as 6.0 %, 7.0 %, 12.0 %. The 10,000-row difference is the client-CPU excess above.

Distribution of one statement, all statements of the three runs pooled (rule 6). The 60,480-row cells hold 7 and 8 statements per binary on `send`; their percentiles are the order statistics of a handful:

@@table analysis.md ^\| path \| shape \| rows returned \| binary \| statements 0@@

## 5. A client that does no work: the cost

**The `drain` path (headers only, no decoding) sees the reply complete later in B at 100 to 10,000 rows and on the filtered walk, and sooner at 60,480 rows.** This is the engine's term seen without a client to hide it, and it is the one result of this run the premise did not predict. Wall and server CPU per statement, three runs per binary:

@@table analysis-drain.md ^\| shape \| rows returned \| statements per run 0@@

- **Small and size-dependent:** +1.0 % at 1 row (inside its spread), +1.3 % (+1.3 µs wall, +1.7 µs server CPU) at 100 rows, +4.2 % (+20.2 µs wall, +22.1 µs server CPU) at 1,000 rows, +2.6 % at 10,000 rows (+111.5 µs), +4.4 % on the filtered walk (+197.4 µs wall, +205.6 µs server CPU, 14 boundaries so about 14 µs each), and -5.9 % (-1,680.5 µs wall, -1,771.3 µs server CPU) at 60,480 rows. The spreads are 0.3 % to 2.2 %, so everything but the 1-row cell is outside them.
- **The 1,000-row cell is the informative one:** one slice, three batches, no boundary reached, and still +20.2 µs. The cost is therefore not the boundary write alone; it is per reply, per batch or per call into the new path, and this run did not attribute it (no sampler was run; no engine source was read for this document). It is also the cell the time-to-first-batch probe shows (first batch +4.0 %, ready +4.4 % at 1,000 rows, section 3), from a different client.
- **Why 60,480 rows goes the other way** is not measured: A writes the 8.5 MB reply from one buffer after the walk, B writes it in 14 pieces while walking, and A's server CPU is 1,771 µs higher; that A's one large write is slower than B's overlapped ones is an inference.
- **It does not reach a decoding client.** On `send` and `split` at 1,000 rows: wall +1.3 % and -0.7 %, idle +4.7 % and +4.2 %, all inside their spreads (6.2 % and 6.9 % for idle) because the client's decode is 9 times the server's.

Pooled distribution of one `drain` statement (rule 6):

@@table analysis-drain.md ^\| shape \| rows returned \| binary \| statements 0@@

## 6. Scenario cells, `c1-g`

**Scenario 1: `symbol-history` (7,560 rows, 14 slices) is 6.2 % faster; `bar-range` (200 rows), `model-join` (359 rows) and `day-slice` (8 rows) did not move.** CE-S0's prediction that a reply under one slice is unchanged holds for both many-row shapes it named: `bar-range` +0.4 % (spreads 2.5 % and 4.6 %), `model-join` +0.7 % (4.4 % and 3.4 %). Warm QPS is the driver's matrix cell (100 statements, 8 arguments cycled), three runs per binary:

@@table analysis.md ^\| shape \| rows returned \| A, runs 0@@

Cold QPS (each argument first seen):

@@table analysis.md ^\| shape \| A median \| B median \| B over A \| spread A / B \| a finding\? 0@@

The driver's phase distributions (each run's percentile, median over the runs; `read-symbol-history` has 10 statements per run, so its p95 and p99 are its maximum; `model-join` has no phase):

@@table analysis.md ^\| phase \| rows returned \| binary 0@@

`symbol-history`'s p50 falls 35,908.6 µs to 33,423.2 µs (-6.9 %), its p0 35,570.5 µs to 32,768.0 µs (-7.9 %). Its ceiling was not modelled (a different table from the sweep's), so no hidden fraction is stated for it.

**Scenario 3 (10,000 loans, unindexed): no shape moved by more than the control did.** QPS derived as 1,000,000 / mean µs, three runs per binary, 200 statements per run:

@@table analysis.md ^\| shape \| A, runs \| B, runs 0@@

- `loans-by-user` (about 5 rows, one slice, nothing CE can change) moved **+9.1 %** against an A spread of 8.8 %. It is this table's own control, and it says the unpinned scenario-3 cells cannot resolve a difference under about 9 %: `overdue` (+5.0 %) and `books-by-genre` (+5.2 %) are both below it, and `loans-by-daterange` is -0.6 %. None is credited to CE. CE-S0 measured the same floor as about 17 % across windows; interleaving brings it to about 9 % inside one.
- `books-by-genre`, which CE-S0 predicted unchanged, is not slower (+5.2 %, inside its 7.3 % spread). The row counts of `loans-by-daterange` and `overdue` were not recorded in either run, so whether their replies cross a slice boundary is not known from here.

Phase distributions (rule 6):

@@table analysis.md ^\| phase \| binary \| ops per run 0@@

## 7. Waits, named

A `send` statement at 60,480 rows is a sum; CE changes which parts are serial.

| component | A | B | where it shows |
|---|---|---|---|
| client decode and render | 289,330.6 µs | 291,624.2 µs | client CPU (+0.8 %, inside its spread), 91.5 % of A's statement and 99.2 % of B's |
| server work the client waits for | 26,831.6 µs | 2,274.7 µs | idle = wall minus client CPU; in B, one 64-page slice |
| wall | 316,102.5 µs | 293,937.1 µs | -22,165.4 µs |
| durability, lock, write-statement, conflict | not applicable | not applicable | reads on one connection: no commit, no fsync, no writer |
| socket round trip | not separable | not separable | below this run's resolution at 1-row (the 1-row statement is 83.8 µs, of which 32.8 µs is server CPU and 51.0 µs client CPU) |

The statement was serial in A (client send, server walks and encodes, the first byte leaves at the end, transfer, decode). In B the transfer and decode of slice k overlap the walk of slice k+1, and the term left is the first slice. A faster client raises the share removed (`split`: idle is 14.6 % of A's wall, 1.4 % of B's).

## 8. Overhead control: point statements

**No change outside the spreads.** CE adds a check per slice boundary and two virtual calls at the head of a statement; the census prices that on statements that never reach a boundary. `bn_census.py` runs A and B side by side, blocks alternating, six runs, server CPU being the sum of `schedstat` over every server thread (unclean cells: 0 of 84):

@@table analysis.md ^\| shape \| path \| A server CPU 0@@

Wall of one statement (rule 6; the driver reports p90, not p95):

@@table analysis.md ^\| shape \| path \| binary \| statements per run 0@@

- `noop` (a `C_PING` the session answers without the dispatcher), `ping` (`SHOW META`), `select-pk` and `update-lit` on both driver paths: server CPU -2.7 % to +1.9 %, wall -1.3 % to +3.0 %, every one inside its spread. The instrument resolves about 5 % on `noop` and `ping` (spreads 2.9 % to 5.0 %) and about 15 % to 20 % on `select-pk`'s server CPU (spreads 14.2 % to 20.0 %), so the verdict for the 30 µs statement is "no change of 15 % or more".
- The sharper instrument is section 5's `drain` cell at 1 row, a full `SELECT` through the new path: wall +1.0 % (+0.5 µs) and server CPU +0.5 µs, spreads 1.6 % and 1.5 %: not a finding, and an upper bound of about 1.5 % on the per-statement fixed cost. The p0 to p50 of `select-pk` wall sits 1.0 % to 1.9 % higher in B on both paths (54.13 vs 55.11 µs at p0, literal), consistent in sign and several times smaller than the cell's 7 % to 11 % wall spread: it cannot be called, and cannot be excluded.
- **Verdict: the expected "no measurable change" holds for point statements at the resolution above. It does not hold at 100 to 1,000 rows on a client that does nothing** (+1.3 % to +4.2 %, section 5), where it is measurable and small.

## 9. Noise floor, validity and what was set aside

- **In-run control.** The shapes CE cannot affect, `range-1` and `range-100` on both paths, 16 comparisons (wall, client CPU, idle, server CPU): none outside its spread; the largest |difference| is 2.6 % for wall, client CPU and idle (`split` `range-1` client CPU), and -5.7 % for server CPU (`send` `range-1`, spreads 4.3 % and 6.6 %, a 10 ms-tick effect). `loans-by-user` in scenario 3 is the loud one (+9.1 %, above).
- **Within-run floor.** The sweep's three runs spread 0.2 % to 15.0 % (wall 0.2 % to 4.3 %; client CPU 0.2 % to 4.7 %; the 15.0 % is `send` 60,480-row server CPU at A, a 10 ms-tick effect on 7 statements); scenario 1 spread 0.7 % to 4.7 %, scenario 3 0.6 % to 8.8 %, the census 0.8 % to 20.0 %.
- **Reproduction of CE-S0.** A's `send` wall against CE-S0's file (a window about 2.2 hours earlier): 83.8 vs 85.1 µs (-1.5 %), 541.5 vs 548.3 µs (-1.2 %), 4,684.1 vs 4,684.5 µs (0.0 %), 48,421.1 vs 47,662.6 µs (+1.6 %), 316,102.5 vs 318,902.8 µs (-0.9 %): the same client, host and binary measure the same.
- **Order.** The first A run (pair 1) came right after the other session's test run; its probe values are the slowest of A's three (ready at 60,480 rows 30,070 µs against 28,259 and 27,863 µs) and its `send` wall at 60,480 rows is 319,919.5 µs against the three-run median of 316,102.5 µs. The median over three resists one such run. B's three first-batch p50s at 60,480 rows are 1,912 to 1,920 µs (spread 0.4 %), so the A/B comparison of the first batch does not depend on it.
- **Validity.** 72 sweep cells and 12 scenario windows have no build/test sample and no foreign process at 50 % CPU or more (`windows.txt`); 6 sweep cells hold one or more samples of a foreign process between 10 % and 50 % CPU (other sessions' idle `claude` processes). No sweep, probe or scenario cell was set aside. In the supplement one cell was: `filter-sym3` `drain` at pair 3's B run held a `claude` process at 50 % or more, the pass ended, the gate re-ran and the cell was measured again (`rowcost/drain-p3-B.jsonl.contaminated.gz`). All sweep-run verifies passed (36 of 36 shape checks, row counts as expected), `verify_problems` was empty in all six scenario-1 runs, every driver exit code was 0. The census prints `range_v_ok: False` for both arms: its `--only` list omits the every-row `UPDATE` shape, so that end-state check has nothing to read; it is the same in A and B.

## 10. What the run teaches about the engine

1. **The premise was a good prediction.** CE-S0 derived, before any streaming build existed, that a slice-boundary stream could hide at most `idle x (1 - 1 / slices)`: 0.95 to 0.99 of that is what B hides, on two client paths, at three sizes (section 4). The engine's reply path was store-and-forward at 0.42 µs of idle per returned row and CE converts all but the first slice of it to overlap.
2. **What bounds the result is the client, then the first slice.** On the `send` path the client decodes for 91.5 % of A's statement and 99.2 % of B's; the statement cannot get shorter than its client CPU, so the best case is 7.0 % at 60,480 rows and 12.0 % on the filtered walk. The server-side term left is the first 64-page slice: about 1.85 to 1.91 ms for a key range and 0.39 ms for the filtered walk. `pages_per_slice` is therefore the lever on time to first batch, and nothing in this run varied it.
3. **A filtered walk benefits most per statement** (12.0 % on `send`, 20.7 % on `split`), as CE-S0 predicted: it walks 877 pages to return 7,560 rows, so most of its server time is a walk that produces few bytes, and the first batch now leaves 3,959.0 µs sooner than A's.
4. **The new path is not free for a reply it cannot overlap.** A 1,000-row reply, three batches and one slice, completes 20.2 µs later (+4.2 %) for a client that does nothing, and the 100-row reply 1.3 µs later (+1.3 %). Below the decoding client's noise, above the `drain` path's. It is a candidate for the next look, with a sampler: it is not explained by the boundary write, because that reply has no boundary.
5. **The overlap shows in the reply's tail as well as its head, at the largest size**: with a client that decodes nothing (`drain`), B's 60,480-row reply is complete 5.9 % sooner (28,364.2 µs to 26,683.7 µs, spreads 2.1 % and 2.2 %); the probe's client agrees in sign (-6.2 %, 28,259.3 µs to 26,520.7 µs) but its A spread is 7.8 %, so that one alone is not a finding. On the filtered walk the same two clients see the reply complete later: +5.3 % in the probe (inside A's 6.5 % spread) and +4.4 % in the `drain` sweep (outside its 1.0 % to 1.4 %).
6. **Nothing here approached a server limit.** One session on one core, the extreme taken the whole table (60,480 rows, 8.5 MB, 130 batches), and the bound is the client (rule 4b). The streaming path's own extremes, a client that stops reading and a wait held past the horizon, belong to CE-S4's tests and were not measured here.
7. **Which open decision acquired data.** CE-Q2 (a), writing at slice boundaries only, is priced: it leaves one slice of server time before the first batch and removes 0.95 to 0.99 of the ceiling that a boundary-only write allows. A write at every batch seal, which CE-S0 section 10 judged unable to help replies of 1,000 rows or fewer, was not built or measured.

## 11. Archive

`bench/v3.3.0/archive/ce-close-v3.3.0-52-g9e568b44/`: `run_close.sh` (master), `run_drain.sh`, `lib.sh`, `monitor.sh`, `watchdog.sh`, `watchdog2.sh`, `cellvalid.py`, `build.sh`, `archive.sh`, `rowcost/ce_s0_rows.py` (+ `rowcost.py`, `kdsrow.py`, unmodified), `ttfb_probe.py`, `analyze_close.py`, `analyze_census.py`, `analyze_drain.py`, `windows.py`, `verify.py`, `analysis.md` and `analysis-drain.md` (every table as rendered), `rowcost/cells-p*.jsonl.gz` and `drain-p*.jsonl.gz` (every statement's latency), `json/` (twelve scenario summaries, six probe files, the census), `logs/`, `conf/`, `monitor.log.gz`, `timeline`, `binary.sha256`, `source-binary-mtime-{A,B}.txt`. No data file, WAL segment or binary is archived. Re-run: `analyze_close.py <run dir>` rebuilds every table here from the run directory.

## 12. Appendix: the sweep, per statement

Median of three runs per binary; client CPU is `time.process_time`, idle is wall minus client CPU, server CPU from `/proc/<pid>/stat` (10 ms ticks). Spread is (max - min) / median.

`send` path:

@@table analysis.md ^\| shape \| rows returned \| quantity 0@@

`split` path:

@@table analysis.md ^\| shape \| rows returned \| quantity 1@@
