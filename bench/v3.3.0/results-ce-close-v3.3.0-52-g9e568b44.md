# CE close: a streamed KWP reply reaches the client's first batch 54 % to 93 % sooner, hides 95 % to 99 % of the idle CE-S0 priced; the cost §5 reported on a client that does no work did not reproduce in a controlled re-run

**Thesis.** On `ce-streaming-reply` at engine commit `9e568b44` (`v3.3.0-52-g9e568b44`, **B**) against the engine at CE's opening commit `1f57f37b` (**A**, the binary CE-S0 measured), interleaved A/B/B/A/A/B in one window, three runs per binary:
a plain unsorted `SELECT` whose walk crosses 64-page slice boundaries now delivers its first `S_ROW_BATCH` at **1,847.5 µs (10,000 rows), 1,912.5 µs (60,480 rows) and 387.6 µs (the 7,560-row filtered walk)** where A delivers it at 4,040.8 µs, 26,673.7 µs and 4,346.6 µs (-54.3 %, -92.8 %, -91.1 %, p50, every one outside both binaries' run-to-run spreads).
The client's idle time per statement falls by the amount CE-S0 predicted: **idle removed over CE-S0's ceiling is 0.95 to 0.99 on all six boundary-crossing cells** (`send` and `split`, 10,000 rows, 60,480 rows, filtered walk), and the statement is that much shorter on the path the scenario drivers use: wall -6.0 %, -7.0 %, -12.0 % (throughput +6.4 %, +7.5 %, +13.7 %); on the faster `split` client -10.3 %, -14.1 %, -20.7 % (+11.5 %, +16.5 %, +26.1 %).
Replies of 1,000 rows or fewer (one slice, no boundary) are unchanged on a client that decodes (all 6 wall cells inside their spreads, largest |difference| 1.8 %), and CE-S0's prediction for `bar-range`, `model-join` and `books-by-genre` holds (+0.4 %, +0.7 %, +5.2 %, all inside their spreads).
**One cost the premise did not name:** a client that does no work (the `drain` path) sees the reply complete 1.3 % later at 100 rows (+1.2 µs, 0.1 point outside its spread), 4.2 % later at 1,000 rows (+20.2 µs), 2.6 % later at 10,000 rows and 4.4 % later on the filtered walk (+197.4 µs), against 5.9 % sooner at 60,480 rows; server CPU per statement moves the same way and is outside its spreads at 1,000 rows and above (section 5). The cost is not attributed, and it rests on a supplement of one cell per shape per run (section 5). On a client that decodes it is inside the spreads.
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
| Quiet host | Before every cell a gate (no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process, at most 8 % busy over 3 s, six checks in a row; capped at 240 s) and a 1-second monitor (`monitor.log.gz`) checked each cell against its window afterwards. Another session ran `kds_tests` from before 10:57 UTC to about 11:02 UTC; the first gate waited 165 s for it, every later gate 25 s (55 s before the re-measured `drain` cell). **Zero sweep, probe or scenario cells were set aside.** One `drain` cell was (section 9) |
| Client | `tools/` copied from this worktree at `9e568b44` into the run directory and put on `PYTHONPATH`; Python 3.12.3; the `a40882af` decoder |
| PostgreSQL | not measured for this shape (rule 4: the floor is optional) |
| Suite | **not executed in this task**; no source was changed by this measurement. The step's suite run is the parent's |

## 2. What was run, and what was cut

A and B alternated in one window: pair 1 A,B; pair 2 B,A; pair 3 A,B (A starts first in pairs 1 and 3, B in pair 2). Each (pair, binary) visit ran three fresh servers on fresh data files: the sweep with the probe, scenario 1, scenario 3. "Spread" everywhere is (max - min) / median over a binary's three runs (six for the census); **a B-over-A difference smaller than the larger of the two spreads is not a finding.** A bare "rule N" in this file is writing rule N of `.claude/agents/ck-tester.md`; `bench/README.md`'s five rules are cited by that file's name.

| Cell | Server | What varies | Per run | What it records |
|---|---|---|---|---|
| Row-cost sweep, `SELECT * FROM daily_stats WHERE id BETWEEN 1 AND N`, N = 1, 100, 1,000, 10,000, 60,480, and `WHERE symbol_id = 3` (7,560 rows of 60,480 walked) | cores 1, group, CPU 2 | rows returned (1 to 60,480; the 200 / 1K / 10K of rule 9 are the 100, 1,000 and 10,000 rows, and the table is 60,480 rows, 877 pages, 69 rows per page); client path `send` (the scenario drivers' own: decode and render every cell to text) and `split` (cells become `bytes`) | 1 cell per (shape, path), its statement count calibrated from the warm-up for 2.5 s; the cells ran 0.6 s (1 row) to 2.6 s, 7 to 10,764 statements; 3 runs per binary | wall, client CPU, server CPU (`/proc/<pid>/stat`, 10 ms ticks) per statement, every statement's latency |
| Time to first byte / first batch / ready, `ttfb_probe.py` | same server and table | the same six statements | 100 timed after 3 warm-ups (60,480 rows: about 100 in the 5 s cap), 3 runs per binary | microseconds from before `sendall` to the `recv` that completed each frame; every reply's frame types, batches, bytes, error flags |
| Supplement, `drain` path | same | the same six statements, frame headers only (no cell touched) | 1 cell per shape, 0.7 s to 2.4 s each, 3 runs per binary, in its own window after the census | wall and server CPU of a client that does nothing |
| Scenario 1, `c1-g` | cores 1, group | `--seed 1 --bars-clustered btree --verify --sync --analyze`, 60,480 bars; `bar-range` 200 rows, `symbol-history` 7,560, `model-join` 359, `day-slice` 8 | 3 per binary | the driver's QPS matrix and phases; `verify_problems` empty in 6 of 6 |
| Scenario 3, unindexed | cores 1, group | `--loans 10000 --index-mode none --server-indexes on --seed 1` | 3 per binary, 200 statements per shape | `books-by-genre`, `loans-by-daterange`, `overdue`, and `loans-by-user` (5 rows, no boundary possible: the control) |
| Overhead control, `bn_census.py` | cores 1, relaxed, two servers side by side, one busy at a time | `noop`, `ping`, `select-pk` (point), `update-lit` (point), literal and prepared | 6 runs x 6 interleaved blocks, 1,998 statements per arm per run | wall and server CPU (`schedstat` over every server thread) per statement |

**Cut** (about an hour, building included; the build and every measurement finished 39 minutes after the task started): the sweep's `drain` path in the main matrix (run as a smaller supplement instead: one cell per shape); `values` path; `strict` and `relaxed` durability for the many-row cells and `cores` above one (every cell is `cores = 1`, `group`); scenario 1 and 3 at smaller sizes (rule 9 is carried by the sweep's 1 to 60,480 rows); the census's insert, delete, scan and every-row update shapes; a sampler run for server CPU per slice (the sweep's server CPU counts 10 ms ticks, 20 to 73 per cell, so one tick is 1.4 % to 5 % of a cell's server CPU); the client that stops reading, the horizon held while parked and the user timeout (CE-S4's own cells, which are tests and not measurements); PostgreSQL.

## 3. Time to first batch

**The first batch now arrives after one slice of server work, wherever the walk ends.** At 10,000 rows (146 pages, 3 slices) the first `S_ROW_BATCH` leaves at 1,847.5 µs against 4,040.8 µs; at 60,480 rows (877 pages) at 1,912.5 µs against 26,673.7 µs, 7.2 % of B's time to the end of the reply where A's is 94.4 %; on the filtered walk at 387.6 µs against 4,346.6 µs. B's three first-batch numbers are near CE-S0's derived server CPU for one 64-page slice (1,863 µs, 2,076 µs and 331 µs, so -0.8 %, -7.9 % and +17 %; derived by CE-S0, not re-measured here). For 1,000 rows or fewer the whole reply is one slice and nothing changes (+0.3 %, -0.3 %, +4.0 %, each inside its spread).

Medians over the runs of each run's p50 (100 replies per run; 60,480 rows: the replies fitting the 5 s cap, 100 here). B over A is the change in the first-batch time; "ready" is `S_READY`, the reply over.

| shape | rows returned | A first batch | B first batch | B over A | spread A / B | a finding? | A first batch / A ready | B first batch / B ready | A ready | B ready | ready B over A |
|---|---|---|---|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | 44.3 µs | 44.5 µs | +0.3 % | 3.6 % / 1.9 % | no (inside the spread) | 100.0 % | 100.0 % | 44.3 µs | 44.5 µs | +0.3 % |
| range-100 | 100 rows | 90.1 µs | 89.8 µs | -0.3 % | 2.7 % / 0.9 % | no (inside the spread) | 100.0 % | 100.0 % | 90.1 µs | 89.8 µs | -0.3 % |
| range-1000 | 1,000 rows | 470.9 µs | 489.6 µs | +4.0 % | 5.4 % / 3.0 % | no (inside the spread) | 99.7 % | 99.3 % | 472.3 µs | 493.3 µs | +4.4 % |
| range-10000 | 10,000 rows | 4,040.8 µs | 1,847.5 µs | -54.3 % | 5.2 % / 2.0 % | **yes** | 95.3 % | 43.0 % | 4,238.2 µs | 4,299.8 µs | +1.5 % |
| range-60480 | 60,480 rows | 26,673.7 µs | 1,912.5 µs | -92.8 % | 8.3 % / 0.4 % | **yes** | 94.4 % | 7.2 % | 28,259.3 µs | 26,520.7 µs | -6.2 % |
| filter-symbol-3 | 7,560 rows | 4,346.6 µs | 387.6 µs | -91.1 % | 6.3 % / 3.1 % | **yes** | 97.4 % | 8.2 % | 4,462.5 µs | 4,698.5 µs | +5.3 % |

Every percentile (each run's, then the median over the three runs):

| shape | rows returned | batches (A / B) | event | binary | p0 | p25 | p50 | p95 | p99 | spread of p50 over runs |
|---|---|---|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | 1 / 1 batches | first_byte | A | 43.1 µs | 43.9 µs | 44.3 µs | 55.5 µs | 66.5 µs | 3.6 % |
| | | | first_byte | B | 43.1 µs | 43.9 µs | 44.5 µs | 56.1 µs | 66.6 µs | 1.9 % |
| | | | first_batch | A | 43.1 µs | 43.9 µs | 44.3 µs | 55.5 µs | 66.5 µs | 3.6 % |
| | | | first_batch | B | 43.1 µs | 43.9 µs | 44.5 µs | 56.1 µs | 66.6 µs | 1.9 % |
| | | | ready | A | 43.1 µs | 43.9 µs | 44.3 µs | 55.5 µs | 66.5 µs | 3.6 % |
| | | | ready | B | 43.1 µs | 43.9 µs | 44.5 µs | 56.1 µs | 66.6 µs | 1.9 % |
| range-100 | 100 rows | 1 / 1 batches | first_byte | A | 85.1 µs | 87.1 µs | 90.1 µs | 106.7 µs | 112.6 µs | 2.7 % |
| | | | first_byte | B | 85.1 µs | 86.9 µs | 89.8 µs | 109.6 µs | 113.0 µs | 0.9 % |
| | | | first_batch | A | 85.1 µs | 87.1 µs | 90.1 µs | 106.7 µs | 112.6 µs | 2.7 % |
| | | | first_batch | B | 85.1 µs | 86.9 µs | 89.8 µs | 109.6 µs | 113.0 µs | 0.9 % |
| | | | ready | A | 85.1 µs | 87.1 µs | 90.1 µs | 106.7 µs | 112.6 µs | 2.7 % |
| | | | ready | B | 85.1 µs | 86.9 µs | 89.8 µs | 109.6 µs | 113.0 µs | 0.9 % |
| range-1000 | 1,000 rows | 3 / 3 batches | first_byte | A | 434.1 µs | 439.8 µs | 454.2 µs | 469.6 µs | 485.5 µs | 5.6 % |
| | | | first_byte | B | 453.5 µs | 461.7 µs | 473.5 µs | 490.7 µs | 499.0 µs | 3.3 % |
| | | | first_batch | A | 449.3 µs | 454.6 µs | 470.9 µs | 485.7 µs | 503.6 µs | 5.4 % |
| | | | first_batch | B | 468.8 µs | 478.0 µs | 489.6 µs | 507.3 µs | 516.0 µs | 3.0 % |
| | | | ready | A | 453.1 µs | 460.6 µs | 472.3 µs | 491.3 µs | 503.6 µs | 5.7 % |
| | | | ready | B | 471.1 µs | 483.6 µs | 493.3 µs | 512.0 µs | 519.5 µs | 2.7 % |
| range-10000 | 10,000 rows | 22 / 22 batches | first_byte | A | 3,998.6 µs | 4,014.5 µs | 4,023.5 µs | 4,124.8 µs | 4,181.2 µs | 5.1 % |
| | | | first_byte | B | 1,788.7 µs | 1,806.3 µs | 1,834.0 µs | 1,867.4 µs | 1,904.6 µs | 1.9 % |
| | | | first_batch | A | 4,015.1 µs | 4,030.3 µs | 4,040.8 µs | 4,128.3 µs | 4,200.4 µs | 5.2 % |
| | | | first_batch | B | 1,803.4 µs | 1,820.5 µs | 1,847.5 µs | 1,882.4 µs | 1,917.6 µs | 2.0 % |
| | | | ready | A | 4,206.6 µs | 4,226.3 µs | 4,238.2 µs | 4,343.2 µs | 4,398.8 µs | 4.9 % |
| | | | ready | B | 4,200.5 µs | 4,242.3 µs | 4,299.8 µs | 4,368.5 µs | 4,426.2 µs | 1.7 % |
| range-60480 | 60,480 rows | 130 / 130 batches | first_byte | A | 25,929.9 µs | 26,535.0 µs | 26,673.7 µs | 27,033.2 µs | 29,561.4 µs | 8.3 % |
| | | | first_byte | B | 1,854.2 µs | 1,877.8 µs | 1,897.2 µs | 1,937.8 µs | 1,973.7 µs | 0.4 % |
| | | | first_batch | A | 25,929.9 µs | 26,535.0 µs | 26,673.7 µs | 27,033.2 µs | 29,561.4 µs | 8.3 % |
| | | | first_batch | B | 1,869.1 µs | 1,893.0 µs | 1,912.5 µs | 1,953.5 µs | 1,988.6 µs | 0.4 % |
| | | | ready | A | 27,408.7 µs | 28,091.4 µs | 28,259.3 µs | 28,717.4 µs | 30,838.1 µs | 7.8 % |
| | | | ready | B | 26,000.4 µs | 26,240.7 µs | 26,520.7 µs | 26,994.0 µs | 27,115.9 µs | 0.3 % |
| filter-symbol-3 | 7,560 rows | 17 / 17 batches | first_byte | A | 4,269.6 µs | 4,316.8 µs | 4,341.6 µs | 4,427.6 µs | 4,482.2 µs | 6.4 % |
| | | | first_byte | B | 371.5 µs | 377.9 µs | 387.5 µs | 412.7 µs | 422.8 µs | 3.1 % |
| | | | first_batch | A | 4,287.3 µs | 4,332.3 µs | 4,346.6 µs | 4,437.4 µs | 4,482.2 µs | 6.3 % |
| | | | first_batch | B | 371.5 µs | 378.1 µs | 387.6 µs | 412.7 µs | 422.8 µs | 3.1 % |
| | | | ready | A | 4,397.3 µs | 4,441.0 µs | 4,462.5 µs | 4,561.1 µs | 4,576.1 µs | 6.5 % |
| | | | ready | B | 4,610.2 µs | 4,662.2 µs | 4,698.5 µs | 4,753.3 µs | 4,777.5 µs | 1.2 % |

Reply shape is identical between A and B on all six statements: the same frame order (`S_PARSE_OK`, `S_BIND_OK`, `S_ROW_DESC`, then batches), the same number of batches (1, 1, 3, 22, 130, 17), the same bytes (476 B, 14,336 B, 140,356 B, 1,400,546 B, 8,468,826 B, 1,058,896 B), and zero error flags in every reply of the six probe runs:

| shape | binary | first_types of rep 0 | batches | bytes | rows | error flags in any rep |
|---|---|---|---|---|---|---|
| range-1 | A | [3, 4, 5, 6, 8, 2] | 1 batch | 476 B | 1 rows | 0 flags |
| range-1 | B | [3, 4, 5, 6, 8, 2] | 1 batch | 476 B | 1 rows | 0 flags |
| range-100 | A | [3, 4, 5, 6, 8, 2] | 1 batch | 14,336 B | 100 rows | 0 flags |
| range-100 | B | [3, 4, 5, 6, 8, 2] | 1 batch | 14,336 B | 100 rows | 0 flags |
| range-1000 | A | [3, 4, 5, 6, 6, 6] | 3 batches | 140,356 B | 1,000 rows | 0 flags |
| range-1000 | B | [3, 4, 5, 6, 6, 6] | 3 batches | 140,356 B | 1,000 rows | 0 flags |
| range-10000 | A | [3, 4, 5, 6, 6, 6] | 22 batches | 1,400,546 B | 10,000 rows | 0 flags |
| range-10000 | B | [3, 4, 5, 6, 6, 6] | 22 batches | 1,400,546 B | 10,000 rows | 0 flags |
| range-60480 | A | [3, 4, 5, 6, 6, 6] | 130 batches | 8,468,826 B | 60,480 rows | 0 flags |
| range-60480 | B | [3, 4, 5, 6, 6, 6] | 130 batches | 8,468,826 B | 60,480 rows | 0 flags |
| filter-symbol-3 | A | [3, 4, 5, 6, 6, 6] | 17 batches | 1,058,896 B | 7,560 rows | 0 flags |
| filter-symbol-3 | B | [3, 4, 5, 6, 6, 6] | 17 batches | 1,058,896 B | 7,560 rows | 0 flags |

The first byte and the first batch are one event in B at every size, as in A (within 20 µs): CE writes whole sealed batches at the slice boundary, never a partial one.

## 4. The sweep: throughput, idle and the hidden fraction

**On the scenario drivers' client path a boundary-crossing reply is 6.0 % to 12.0 % shorter, on a faster client 10.3 % to 20.7 %; a reply inside one slice is unchanged.** Throughput (rule 5a: QPS, derived as 1,000,000 / wall per statement):

| shape | rows returned | A QPS (3 runs) | B QPS (3 runs) | A median | B median | B over A (QPS) | spread A / B | outside the larger spread? |
|---|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | 12,018 / 11,673 / 11,933 qps | 11,752 / 11,864 / 12,030 qps | 11,933 qps | 11,864 qps | -0.6 % | 2.9 % / 2.3 % | no |
| range-100 | 100 rows | 1,822 / 1,848 / 1,847 qps | 1,846 / 1,861 / 1,875 qps | 1,847 qps | 1,861 qps | +0.7 % | 1.4 % / 1.5 % | no |
| range-1000 | 1,000 rows | 208 / 213 / 214 qps | 213 / 211 / 208 qps | 213 qps | 211 qps | -1.3 % | 2.5 % / 2.1 % | no |
| range-10000 | 10,000 rows | 20.6 / 20.7 / 21.2 qps | 22.0 / 22.0 / 22.0 qps | 20.7 qps | 22.0 qps | +6.4 % | 3.1 % / 0.2 % | **yes** |
| range-60480 | 60,480 rows | 3.13 / 3.16 / 3.17 qps | 3.37 / 3.40 / 3.41 qps | 3.16 qps | 3.40 qps | +7.5 % | 1.3 % / 1.2 % | **yes** |
| filter-sym3 | 7,560 rows | 27.2 / 27.6 / 27.8 qps | 31.5 / 30.9 / 31.4 qps | 27.6 qps | 31.4 qps | +13.7 % | 2.1 % / 2.2 % | **yes** |

| shape | rows returned | A QPS (3 runs) | B QPS (3 runs) | A median | B median | B over A (QPS) | spread A / B | outside the larger spread? |
|---|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | 13,462 / 13,291 / 13,115 qps | 12,887 / 13,054 / 13,384 qps | 13,291 qps | 13,054 qps | -1.8 % | 2.6 % / 3.8 % | no |
| range-100 | 100 rows | 2,973 / 3,103 / 3,077 qps | 3,078 / 3,065 / 3,073 qps | 3,077 qps | 3,073 qps | -0.1 % | 4.2 % / 0.4 % | no |
| range-1000 | 1,000 rows | 376 / 376 / 383 qps | 379 / 382 / 377 qps | 376 qps | 379 qps | +0.7 % | 1.9 % / 1.3 % | no |
| range-10000 | 10,000 rows | 37.4 / 37.6 / 37.9 qps | 42.2 / 41.8 / 41.9 qps | 37.6 qps | 41.9 qps | +11.5 % | 1.2 % / 0.9 % | **yes** |
| range-60480 | 60,480 rows | 5.33 / 5.34 / 5.40 qps | 6.30 / 6.22 / 6.20 qps | 5.34 qps | 6.22 qps | +16.5 % | 1.4 % / 1.7 % | **yes** |
| filter-sym3 | 7,560 rows | 47.7 / 47.9 / 48.5 qps | 60.3 / 60.4 / 60.5 qps | 47.9 qps | 60.4 qps | +26.1 % | 1.7 % / 0.4 % | **yes** |

The first table is `send`, the second `split`. The per-statement walls, client CPU, idle and server CPU are in the appendix (section 12).

**The hidden fraction against CE-S0's ceiling** (CE-S0 section 6: the most a slice-boundary stream can remove is `idle x (1 - 1 / slices)`, with 2.28 slices at 10,000 rows and 13.70 at 60,480 rows and the filtered walk; here A's idle is taken from this window). *Wall hidden* is A's wall minus B's; *idle removed* is A's idle minus B's (idle = wall minus client CPU):

| path | shape | rows returned | slices (CE-S0 fractional) | A wall | B wall | measured hidden | measured, share of A wall | ceiling, share of A wall | wall hidden / ceiling | A idle | B idle | idle removed | idle removed / ceiling | spread of wall A / B |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| send | range-1 | 1 rows | 1 slice | 83.8 µs | 84.3 µs | -0.5 µs | -0.6 % | 0.0 % | n/a (no boundary) | 32.6 µs | 32.6 µs | -0.1 µs | n/a | 2.9 % / 2.3 % |
| send | range-100 | 100 rows | 1 slice | 541.5 µs | 537.5 µs | 4.0 µs | +0.7 % | 0.0 % | n/a (no boundary) | 82.3 µs | 81.0 µs | 1.3 µs | n/a | 1.4 % / 1.5 % |
| send | range-1000 | 1,000 rows | 1 slice | 4,684.1 µs | 4,743.5 µs | -59.4 µs | -1.3 % | 0.0 % | n/a (no boundary) | 451.2 µs | 472.4 µs | -21.2 µs | n/a | 2.6 % / 2.1 % |
| send | range-10000 | 10,000 rows | 2.28 slices | 48,421.1 µs | 45,506.3 µs | 2,914.8 µs | +6.0 % | 4.9 % | 1.23x | 4,232.6 µs | 1,964.8 µs | 2,267.8 µs | 0.95x | 3.0 % / 0.2 % |
| send | range-60480 | 60,480 rows | 13.7 slices | 316,102.5 µs | 293,937.1 µs | 22,165.4 µs | +7.0 % | 7.9 % | 0.89x | 26,831.6 µs | 2,274.7 µs | 24,556.9 µs | 0.99x | 1.3 % / 1.2 % |
| send | filter-sym3 | 7,560 rows | 13.7 slices | 36,239.9 µs | 31,877.6 µs | 4,362.3 µs | +12.0 % | 12.2 % | 0.98x | 4,783.6 µs | 442.9 µs | 4,340.7 µs | 0.98x | 2.1 % / 2.2 % |
| split | range-1 | 1 rows | 1 slice | 75.2 µs | 76.6 µs | -1.4 µs | -1.8 % | 0.0 % | n/a (no boundary) | 32.5 µs | 32.8 µs | -0.3 µs | n/a | 2.6 % / 3.8 % |
| split | range-100 | 100 rows | 1 slice | 325.0 µs | 325.4 µs | -0.4 µs | -0.1 % | 0.0 % | n/a (no boundary) | 81.2 µs | 81.4 µs | -0.2 µs | n/a | 4.3 % / 0.4 % |
| split | range-1000 | 1,000 rows | 1 slice | 2,659.2 µs | 2,641.5 µs | 17.7 µs | +0.7 % | 0.0 % | n/a (no boundary) | 440.1 µs | 458.5 µs | -18.4 µs | n/a | 1.9 % / 1.3 % |
| split | range-10000 | 10,000 rows | 2.28 slices | 26,605.8 µs | 23,871.6 µs | 2,734.2 µs | +10.3 % | 8.7 % | 1.18x | 4,132.6 µs | 1,878.3 µs | 2,254.3 µs | 0.97x | 1.2 % / 0.8 % |
| split | range-60480 | 60,480 rows | 13.7 slices | 187,348.7 µs | 160,841.3 µs | 26,507.4 µs | +14.1 % | 13.5 % | 1.04x | 27,374.3 µs | 2,196.6 µs | 25,177.7 µs | 0.99x | 1.3 % / 1.7 % |
| split | filter-sym3 | 7,560 rows | 13.7 slices | 20,884.4 µs | 16,565.5 µs | 4,319.0 µs | +20.7 % | 20.4 % | 1.02x | 4,586.3 µs | 401.5 µs | 4,184.8 µs | 0.98x | 1.7 % / 0.4 % |

- **Idle removed is the mechanism, and it meets the ceiling: 0.95 to 0.99 of it on six of six boundary-crossing cells.** What remains in B is the first slice, which nothing can overlap: B's idle is 1,964.8 µs at 10,000 rows, 2,274.7 µs at 60,480 rows and 442.9 µs on the filtered walk (`send`), against the model's 1/2.28, 1/13.7 and 1/13.7 of A's idle (1,856 µs, 1,958 µs, 349 µs).
- **Wall hidden follows idle removed, with the client's own CPU as the difference.** Measured as a share of A's wall: 6.0 %, 7.0 %, 12.0 % (`send`) and 10.3 %, 14.1 %, 20.7 % (`split`), against ceilings of 4.9 %, 7.9 %, 12.2 % and 8.7 %, 13.5 %, 20.4 %. The ratio exceeds 1 at 10,000 rows (1.23 `send`, 1.18 `split`) and, by less, at `split` 60,480 rows and the filtered walk (1.04, 1.02). Idle removed stays under the ceiling in every cell, so any excess is client CPU. At `send` 10,000 rows the excess over the ceiling is 538.6 µs, 1.1 % of A's wall, inside A's 3.0 % wall spread. B's client CPU there is -563.7 µs, inside its 3.0 % spread, and changes sign across the pairs (-563.7, -725.3, +682.0 µs), so it is noise. At `split` 10,000 rows the excess is 414.1 µs, 1.6 % of A's wall and outside the 1.2 % spread. It matches a client-CPU difference that is a finding: -315.3 µs (-1.4 % against a 1.0 % spread), lower in all three pairs, with the same sign inside the spreads at `split` 60,480 rows and the filtered walk (-0.3 %, -0.3 %). That part of the excess is measured, and this run does not attribute it. The two smaller excesses (1,131.3 µs and 67.5 µs, 0.6 % and 0.3 % of A's wall) are inside their wall spreads. None of the excess is credited to the mechanism.
- **Server CPU per statement shows no change outside its spreads** at any size on either path (largest difference -6.7 % at `split` 60,480 rows, spreads 7.7 % and 0.4 %, and -5.7 % at `send` 1 row, spreads 4.3 % and 6.6 %; a `send` 60,480-row cell holds 20 to 23 ticks of 10 ms, so one tick is 4 % to 5 % of it and a smaller difference is not resolvable there). Streaming moves the server's work earlier relative to the client; it adds none that this instrument sees. The `drain` path (section 5) has the sharper instrument and does see a small one.
- CE-S0's three ceilings (5.0 %, 7.9 %, 12.1 % of the `send` wall) were derived; they are measured here as 6.0 %, 7.0 %, 12.0 %. The 10,000-row difference is the client-CPU excess above.

Distribution of one statement, all statements of the three runs pooled (rule 6). The 60,480-row cells hold 7 and 8 statements per binary on `send`; their percentiles are the order statistics of a handful:

| path | shape | rows returned | binary | statements | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|
| send | range-1 | 1 rows | A | 31,430 statements | 77.9 µs | 80.5 µs | 81.7 µs | 99.0 µs | 107.3 µs |
| send | range-1 | 1 rows | B | 30,051 statements | 77.5 µs | 80.1 µs | 81.0 µs | 97.9 µs | 109.8 µs |
| send | range-100 | 100 rows | A | 8,326 statements | 517.0 µs | 531.2 µs | 536.1 µs | 565.2 µs | 709.7 µs |
| send | range-100 | 100 rows | B | 8,471 statements | 513.3 µs | 528.6 µs | 533.1 µs | 554.2 µs | 601.8 µs |
| send | range-1000 | 1,000 rows | A | 1,480 statements | 4,566.2 µs | 4,639.5 µs | 4,675.9 µs | 4,858.1 µs | 6,271.2 µs |
| send | range-1000 | 1,000 rows | B | 1,455 statements | 4,589.6 µs | 4,658.4 µs | 4,691.8 µs | 4,956.5 µs | 6,491.1 µs |
| send | range-10000 | 10,000 rows | A | 142 statements | 45,990.1 µs | 46,991.3 µs | 47,845.6 µs | 50,446.7 µs | 51,432.5 µs |
| send | range-10000 | 10,000 rows | B | 147 statements | 43,781.5 µs | 44,888.4 µs | 45,267.3 µs | 47,238.2 µs | 47,958.6 µs |
| send | range-60480 | 60,480 rows | A | 21 statements | 308,948.4 µs | 312,946.2 µs | 315,451.6 µs | 326,046.8 µs | 329,798.6 µs |
| send | range-60480 | 60,480 rows | B | 24 statements | 285,066.8 µs | 290,790.9 µs | 292,606.2 µs | 304,195.2 µs | 305,776.3 µs |
| send | filter-sym3 | 7,560 rows | A | 199 statements | 35,275.0 µs | 35,801.9 µs | 36,104.3 µs | 37,920.0 µs | 38,467.5 µs |
| send | filter-sym3 | 7,560 rows | B | 231 statements | 31,016.3 µs | 31,491.8 µs | 31,882.4 µs | 33,419.8 µs | 34,364.5 µs |
| split | range-1 | 1 rows | A | 26,683 statements | 68.0 µs | 72.2 µs | 73.0 µs | 87.6 µs | 98.0 µs |
| split | range-1 | 1 rows | B | 27,189 statements | 69.6 µs | 72.0 µs | 72.9 µs | 90.7 µs | 103.0 µs |
| split | range-100 | 100 rows | A | 14,078 statements | 306.6 µs | 315.8 µs | 321.0 µs | 342.6 µs | 431.5 µs |
| split | range-100 | 100 rows | B | 13,979 statements | 306.7 µs | 316.3 µs | 321.7 µs | 341.0 µs | 374.6 µs |
| split | range-1000 | 1,000 rows | A | 2,546 statements | 2,518.2 µs | 2,599.2 µs | 2,628.3 µs | 2,696.8 µs | 3,053.7 µs |
| split | range-1000 | 1,000 rows | B | 2,535 statements | 2,541.7 µs | 2,592.8 µs | 2,612.1 µs | 2,683.4 µs | 3,334.5 µs |
| split | range-10000 | 10,000 rows | A | 278 statements | 25,263.6 µs | 26,077.3 µs | 26,387.9 µs | 28,196.2 µs | 29,387.0 µs |
| split | range-10000 | 10,000 rows | B | 306 statements | 22,706.3 µs | 23,426.5 µs | 23,651.0 µs | 25,469.6 µs | 25,945.6 µs |
| split | range-60480 | 60,480 rows | A | 39 statements | 179,120.2 µs | 182,755.0 µs | 184,609.4 µs | 197,341.2 µs | 197,714.5 µs |
| split | range-60480 | 60,480 rows | B | 46 statements | 153,796.0 µs | 156,207.2 µs | 158,618.3 µs | 169,519.2 µs | 171,234.0 µs |
| split | filter-sym3 | 7,560 rows | A | 354 statements | 19,932.3 µs | 20,438.5 µs | 20,692.5 µs | 22,359.4 µs | 23,157.9 µs |
| split | filter-sym3 | 7,560 rows | B | 452 statements | 15,928.9 µs | 16,272.8 µs | 16,429.0 µs | 18,111.6 µs | 18,611.9 µs |

## 5. A client that does no work: the cost

**Not reproduced** (`results-ce-drain-attribution-v3.3.0-52-g9e568b44.md`, five rotated A/B/C rounds in one window, 2026-10-20): B equals A at 1 to 10,000 rows, slower in 0 of 5 pairs at 1,000 and at 10,000 rows. B's medians reproduce this section's; A's do not - the same binary ran 4.9 % slower there than here (501.0 against 477.5 us at 1,000 rows), more than the gap this section reports, so the gap below is A's window, not CE. This run had no in-window A-against-A control; that run supplies one. The section stands as measured.

**The `drain` path (headers only, no decoding) sees the reply complete later in B at 100 to 10,000 rows and on the filtered walk, and sooner at 60,480 rows.** This is the engine's term seen without a client to hide it, and it is the one result of this run the premise did not predict. Wall and server CPU per statement, three runs per binary:

| shape | rows returned | statements per run | A wall, runs | B wall, runs | A median | B median | B over A | spread A / B | a finding? | A server CPU | B server CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | 15,132 statements | 48 / 48 / 48 µs | 48 / 49 / 48 µs | 47.9 µs | 48.4 µs | +1.0 % | 1.6 % / 1.5 % | no (inside the spread) | 31.2 µs | 31.7 µs |
| range-100 | 100 rows | 8,965 statements | 93 / 94 / 94 µs | 94 / 95 / 95 µs | 93.5 µs | 94.8 µs | +1.3 % | 0.9 % / 1.2 % | **yes** | 75.0 µs | 76.7 µs |
| range-1000 | 1,000 rows | 3,550 statements | 477 / 480 / 473 µs | 503 / 495 / 498 µs | 477.5 µs | 497.7 µs | +4.2 % | 1.5 % / 1.6 % | **yes** | 452.8 µs | 474.9 µs |
| range-10000 | 10,000 rows | 533 statements | 4,264 / 4,252 / 4,264 µs | 4,378 / 4,299 / 4,375 µs | 4,263.8 µs | 4,375.3 µs | +2.6 % | 0.3 % / 1.8 % | **yes** | 4,232.2 µs | 4,337.8 µs |
| range-60480 | 60,480 rows | 60 statements | 28,886 / 28,277 / 28,364 µs | 27,158 / 26,582 / 26,684 µs | 28,364.2 µs | 26,683.7 µs | -5.9 % | 2.1 % / 2.2 % | **yes** | 28,474.6 µs | 26,703.3 µs |
| filter-sym3 | 7,560 rows | 477 statements | 4,538 / 4,589 / 4,526 µs | 4,735 / 4,707 / 4,756 µs | 4,537.7 µs | 4,735.1 µs | +4.4 % | 1.4 % / 1.0 % | **yes** | 4,505.3 µs | 4,710.9 µs |

- **Small and size-dependent:** +1.0 % at 1 row (inside its spread), +1.3 % (+1.2 µs wall; server CPU +1.7 µs, inside its 2.7 % spread) at 100 rows, +4.2 % (+20.2 µs wall, +22.2 µs server CPU) at 1,000 rows, +2.6 % at 10,000 rows (+111.5 µs wall, +105.6 µs server CPU), +4.4 % on the filtered walk (+197.4 µs wall, +205.7 µs server CPU), and -5.9 % (-1,680.5 µs wall, -1,771.3 µs server CPU) at 60,480 rows. The wall spreads are 0.3 % to 2.2 %, so every wall difference but the 1-row cell's is outside them, and server CPU is outside its spreads (0.4 % to 2.3 %) from 1,000 rows up.
- **How far it is supported.** The supplement is one cell per shape per run, 0.7 s to 2.4 s each, run in its own 7-minute window after the census. B is slower than A in every one of the three pairs at 100, 1,000 and 10,000 rows and on the filtered walk, and every B run is above every A run. With three runs per binary that is the strongest result the design can give, and still a small sample. The 100-row difference clears its spread by 0.1 point (1.3 % against 1.2 %; B's lowest run is 0.03 µs above A's highest), so it is at the edge of this instrument. The 1,000-row, 10,000-row and filtered-walk differences are 1.4 to 3.1 times their larger spread, and their server CPU, which these cells resolve to 0.4 % to 0.6 % per tick, moves with them.
- **The 1,000-row cell is the informative one:** one slice, three batches, no boundary reached, and still +20.2 µs. The cost is therefore not the boundary write alone; it is per reply, per batch or per call into the new path, and this run did not attribute it (no sampler was run; no engine source was read for this document). The time-to-first-batch probe, a different client, agrees in median (first batch +4.0 %, ready +4.4 % at 1,000 rows, section 3). It is inside its spreads (5.4 %, 5.7 %) and reversed in pair 1, so it is not a second finding.
- **Why 60,480 rows goes the other way** is not measured: A writes the 8.5 MB reply from one buffer after the walk, B writes it in 14 pieces while walking, and A's server CPU is 1,771 µs higher; that A's one large write is slower than B's overlapped ones is an inference.
- **A decoding client does not resolve it.** On `send` and `split` at 1,000 rows: wall +1.3 % and -0.7 %, idle +4.7 % and +4.2 %, all inside their spreads (6.2 % and 6.9 % for idle); the client's CPU is 8.7 times the server's on `send` and 4.7 times on `split`.

Pooled distribution of one `drain` statement (rule 6):

| shape | rows returned | binary | statements | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | A | 45,425 statements | 43.5 µs | 46.1 µs | 46.5 µs | 58.2 µs | 68.1 µs |
| range-1 | 1 rows | B | 44,118 statements | 44.8 µs | 46.8 µs | 47.4 µs | 57.7 µs | 66.6 µs |
| range-100 | 100 rows | A | 26,835 statements | 82.1 µs | 88.4 µs | 91.3 µs | 107.4 µs | 137.1 µs |
| range-100 | 100 rows | B | 27,791 statements | 81.4 µs | 89.4 µs | 92.2 µs | 109.8 µs | 121.9 µs |
| range-1000 | 1,000 rows | A | 10,662 statements | 449.0 µs | 463.0 µs | 469.7 µs | 497.5 µs | 631.5 µs |
| range-1000 | 1,000 rows | B | 10,180 statements | 468.5 µs | 484.1 µs | 490.6 µs | 521.3 µs | 720.4 µs |
| range-10000 | 10,000 rows | A | 1,595 statements | 4,160.2 µs | 4,214.5 µs | 4,228.8 µs | 4,341.7 µs | 4,761.9 µs |
| range-10000 | 10,000 rows | B | 1,573 statements | 4,168.3 µs | 4,300.0 µs | 4,339.8 µs | 4,431.7 µs | 4,652.8 µs |
| range-60480 | 60,480 rows | A | 179 statements | 27,700.3 µs | 28,121.0 µs | 28,533.3 µs | 29,178.2 µs | 29,764.0 µs |
| range-60480 | 60,480 rows | B | 266 statements | 25,911.0 µs | 26,506.3 µs | 26,781.9 µs | 27,386.0 µs | 27,755.2 µs |
| filter-sym3 | 7,560 rows | A | 1,430 statements | 4,424.9 µs | 4,487.4 µs | 4,525.8 µs | 4,644.6 µs | 4,864.3 µs |
| filter-sym3 | 7,560 rows | B | 1,397 statements | 4,592.4 µs | 4,671.2 µs | 4,706.3 µs | 4,871.5 µs | 5,259.1 µs |

## 6. Scenario cells, `c1-g`

**Scenario 1: `symbol-history` (7,560 rows, 14 slices) is 6.2 % faster; `bar-range` (200 rows), `model-join` (359 rows) and `day-slice` (8 rows) did not move.** CE-S0's prediction that a reply under one slice is unchanged holds for both many-row shapes it named: `bar-range` +0.4 % (spreads 2.5 % and 4.6 %), `model-join` +0.7 % (4.4 % and 3.4 %). Warm QPS is the driver's matrix cell (100 statements, 8 arguments cycled), three runs per binary:

| shape | rows returned | A, runs | B, runs | A median | B median | B over A | spread A / B | a finding? |
|---|---|---|---|---|---|---|---|---|
| bar-range | 200 rows | 1,257.1 / 1,265.6 / 1,234.2 qps | 1,269.8 / 1,262.6 / 1,211.7 qps | 1,257.1 qps | 1,262.6 qps | +0.4 % | 2.5 % / 4.6 % | no (inside the spread) |
| symbol-history | 7,560 rows | 27.2 / 27.2 / 27.4 qps | 28.9 / 28.8 / 29.1 qps | 27.2 qps | 28.9 qps | +6.2 % | 0.7 % / 1.0 % | **yes** |
| model-join | 359 rows | 1,073.4 / 1,033.2 / 1,028.1 qps | 1,065.6 / 1,040.8 / 1,029.9 qps | 1,033.2 qps | 1,040.8 qps | +0.7 % | 4.4 % / 3.4 % | no (inside the spread) |
| day-slice | 8 rows | 614.5 / 586.6 / 599.9 qps | 591.4 / 604.1 / 588.8 qps | 599.9 qps | 591.4 qps | -1.4 % | 4.7 % / 2.6 % | no (inside the spread) |

Cold QPS (each argument first seen):

| shape | A median | B median | B over A | spread A / B | a finding? |
|---|---|---|---|---|---|
| bar-range | 1,246.9 qps | 1,273.2 qps | +2.1 % | 5.8 % / 1.2 % | no (inside the spread) |
| symbol-history | 27.7 qps | 28.3 qps | +2.2 % | 0.7 % / 6.4 % | no (inside the spread) |
| model-join | 1,016.7 qps | 1,033.9 qps | +1.7 % | 6.1 % / 2.6 % | no (inside the spread) |
| day-slice | 601.0 qps | 595.4 qps | -0.9 % | 0.6 % / 3.6 % | no (inside the spread) |

The driver's phase distributions (each run's percentile, median over the runs; `read-symbol-history` has 10 statements per run, so its p95 and p99 are its maximum; `model-join` has no phase):

| phase | rows returned | binary | ops per run | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| read-bar-range | 200 rows | A | 200 ops | 767.1 µs | 791.6 µs | 800.0 µs | 816.3 µs | 836.6 µs |
| read-bar-range | 200 rows | B | 200 ops | 767.3 µs | 786.5 µs | 793.3 µs | 819.4 µs | 866.0 µs |
| read-symbol-history | 7,560 rows | A | 10 ops | 35,570.5 µs | 35,762.1 µs | 35,908.6 µs | 39,780.1 µs | 39,780.1 µs |
| read-symbol-history | 7,560 rows | B | 10 ops | 32,768.0 µs | 33,103.2 µs | 33,423.2 µs | 38,200.8 µs | 38,200.8 µs |
| read-day-slice | 8 rows | A | 200 ops | 1,625.3 µs | 1,652.8 µs | 1,660.2 µs | 1,714.8 µs | 1,829.9 µs |
| read-day-slice | 8 rows | B | 200 ops | 1,627.4 µs | 1,646.3 µs | 1,656.2 µs | 1,727.6 µs | 1,803.6 µs |

`symbol-history`'s p50 falls 35,908.6 µs to 33,423.2 µs (-6.9 %), its p0 35,570.5 µs to 32,768.0 µs (-7.9 %). Its ceiling was not modelled (a different table from the sweep's), so no hidden fraction is stated for it.

**Scenario 3 (10,000 loans, unindexed): no shape moved by more than the control did.** QPS derived as 1,000,000 / mean µs, three runs per binary, 200 statements per run:

| shape | A, runs | B, runs | A median | B median | B over A | spread A / B | a finding? |
|---|---|---|---|---|---|---|---|
| books-by-genre | 2,918 / 3,135 / 2,966 qps | 3,102 / 3,197 / 3,120 qps | 2,966 qps | 3,120 qps | +5.2 % | 7.3 % / 3.1 % | no (inside the spread) |
| loans-by-daterange | 997 / 1,003 / 999 qps | 994 / 1,007 / 989 qps | 999 qps | 994 qps | -0.6 % | 0.6 % / 1.8 % | no (inside the spread) |
| overdue | 1,192 / 1,239 / 1,192 qps | 1,251 / 1,261 / 1,248 qps | 1,192 qps | 1,251 qps | +5.0 % | 3.9 % / 1.0 % | **yes** |
| loans-by-user | 2,660 / 2,886 / 2,651 qps | 2,930 / 2,904 / 2,894 qps | 2,660 qps | 2,904 qps | +9.1 % | 8.8 % / 1.3 % | **yes** |

- `loans-by-user` (about 5 rows, one slice, nothing CE can change) moved **+9.1 %** against an A spread of 8.8 %. It is this table's own control, and it says the unpinned scenario-3 cells cannot resolve a difference under about 9 %: `overdue` (+5.0 %) and `books-by-genre` (+5.2 %) are both below it, and `loans-by-daterange` is -0.6 %. None is credited to CE. CE-S0 measured the same floor as about 17 % across windows; interleaving brings it to about 9 % inside one.
- `books-by-genre`, which CE-S0 predicted unchanged, is not slower (+5.2 %, inside its 7.3 % spread). The row counts of `loans-by-daterange` and `overdue` were not recorded in either run, so whether their replies cross a slice boundary is not known from here.

Phase distributions (rule 6):

| phase | binary | ops per run | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| books-by-genre | A | 200 ops | 290.8 µs | 320.9 µs | 337.2 µs | 372.8 µs | 390.5 µs |
| books-by-genre | B | 200 ops | 288.1 µs | 310.9 µs | 319.2 µs | 344.4 µs | 357.4 µs |
| loans-by-daterange | A | 200 ops | 392.2 µs | 982.9 µs | 1,012.7 µs | 1,068.3 µs | 1,117.0 µs |
| loans-by-daterange | B | 200 ops | 415.2 µs | 985.9 µs | 1,017.8 µs | 1,082.6 µs | 1,117.0 µs |
| overdue | A | 200 ops | 406.2 µs | 834.0 µs | 855.7 µs | 914.4 µs | 946.0 µs |
| overdue | B | 200 ops | 371.8 µs | 794.5 µs | 816.7 µs | 871.7 µs | 887.2 µs |
| loans-by-user | A | 200 ops | 331.8 µs | 367.0 µs | 373.9 µs | 397.9 µs | 407.2 µs |
| loans-by-user | B | 200 ops | 321.9 µs | 334.0 µs | 341.2 µs | 372.6 µs | 380.9 µs |

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

| shape | path | A server CPU | B server CPU | B over A | spread A / B | a finding? | A wall mean | B wall mean | wall B over A | wall spread A / B |
|---|---|---|---|---|---|---|---|---|---|---|
| noop | literal | 12.13 µs | 12.18 µs | +0.4 % | 2.9 % / 2.9 % | no (inside the spread) | 27.25 µs | 27.23 µs | -0.1 % | 2.3 % / 1.3 % |
| ping | literal | 108.88 µs | 107.68 µs | -1.1 % | 2.9 % / 5.0 % | no (inside the spread) | 292.52 µs | 290.03 µs | -0.9 % | 2.1 % / 4.3 % |
| ping | prepared | 108.55 µs | 106.83 µs | -1.6 % | 4.8 % / 4.2 % | no (inside the spread) | 289.11 µs | 286.99 µs | -0.7 % | 2.1 % / 0.8 % |
| select-pk | literal | 31.20 µs | 31.21 µs | +0.0 % | 20.0 % / 14.8 % | no (inside the spread) | 61.97 µs | 62.30 µs | +0.5 % | 10.8 % / 6.9 % |
| select-pk | prepared | 29.89 µs | 30.47 µs | +1.9 % | 14.2 % / 13.7 % | no (inside the spread) | 58.59 µs | 60.32 µs | +3.0 % | 8.1 % / 8.3 % |
| update-lit | literal | 26.64 µs | 26.59 µs | -0.2 % | 9.3 % / 5.1 % | no (inside the spread) | 51.33 µs | 51.58 µs | +0.5 % | 5.8 % / 9.8 % |
| update-lit | prepared | 26.48 µs | 25.78 µs | -2.7 % | 7.4 % / 6.9 % | no (inside the spread) | 48.67 µs | 48.04 µs | -1.3 % | 6.5 % / 4.3 % |

Wall of one statement (rule 6; the driver reports p90, not p95):

| shape | path | binary | statements per run | p0 | p25 | p50 | p90 | p99 |
|---|---|---|---|---|---|---|---|---|
| noop | literal | A | 1,998 statements | 22.89 µs | 26.16 µs | 26.42 µs | 27.60 µs | 39.95 µs |
| noop | literal | B | 1,998 statements | 22.64 µs | 26.15 µs | 26.43 µs | 27.36 µs | 38.90 µs |
| ping | literal | A | 1,998 statements | 273.06 µs | 281.25 µs | 287.17 µs | 304.17 µs | 367.36 µs |
| ping | literal | B | 1,998 statements | 272.48 µs | 280.84 µs | 285.97 µs | 301.51 µs | 348.61 µs |
| ping | prepared | A | 1,998 statements | 269.39 µs | 278.54 µs | 284.31 µs | 300.19 µs | 363.47 µs |
| ping | prepared | B | 1,998 statements | 269.20 µs | 277.94 µs | 283.92 µs | 299.00 µs | 329.33 µs |
| select-pk | literal | A | 1,998 statements | 54.13 µs | 56.56 µs | 59.40 µs | 69.73 µs | 87.29 µs |
| select-pk | literal | B | 1,998 statements | 55.11 µs | 57.11 µs | 60.25 µs | 69.45 µs | 88.54 µs |
| select-pk | prepared | A | 1,998 statements | 51.20 µs | 53.28 µs | 56.57 µs | 64.88 µs | 81.02 µs |
| select-pk | prepared | B | 1,998 statements | 52.15 µs | 54.08 µs | 57.11 µs | 65.92 µs | 85.92 µs |
| update-lit | literal | A | 1,998 statements | 40.42 µs | 47.12 µs | 47.74 µs | 57.00 µs | 76.08 µs |
| update-lit | literal | B | 1,998 statements | 42.85 µs | 47.29 µs | 48.01 µs | 56.45 µs | 74.99 µs |
| update-lit | prepared | A | 1,998 statements | 39.41 µs | 44.43 µs | 45.03 µs | 53.41 µs | 72.38 µs |
| update-lit | prepared | B | 1,998 statements | 40.48 µs | 44.18 µs | 44.82 µs | 53.18 µs | 74.08 µs |

- `noop` (a `C_PING` the session answers without the dispatcher), `ping` (`SHOW META`), `select-pk` and `update-lit` on both driver paths: server CPU -2.7 % to +1.9 %, wall -1.3 % to +3.0 %, every one inside its spread. The instrument resolves about 5 % on `noop` and `ping` (spreads 2.9 % to 5.0 %) about 7 % to 9 % on `update-lit` (larger spreads 7.4 % and 9.3 %), and 14 % to 20 % on `select-pk`'s server CPU (larger spreads 14.2 % prepared, 20.0 % literal), so the verdict for the 30 µs statement is "no change of 20 % or more (literal) or 14 % or more (prepared) in server CPU"; its wall resolves 8 % to 11 %.
- The sharper instrument is section 5's `drain` cell at 1 row, a full `SELECT` through the new path: wall +1.0 % (+0.5 µs), spreads 1.6 % and 1.5 %: not a finding. It resolves no fixed cost of 1.6 % or more and does not exclude one up to about 2.6 % (the +1.0 % measured plus that spread). Its server CPU, +0.5 µs, is under one 10 ms tick of the cell (0.66 µs per statement) and says nothing. The p0 to p50 of `select-pk` wall sits 1.0 % to 1.9 % higher in B on both paths (54.13 vs 55.11 µs at p0, literal), consistent in sign and several times smaller than the cell's 7 % to 11 % wall spread: it cannot be called, and cannot be excluded.
- **Verdict: the expected "no measurable change" holds for point statements at the resolution above. It does not hold at 100 to 1,000 rows on a client that does nothing** (+1.3 % to +4.2 %, section 5), where it is small, outside the spreads (the 100-row cell by 0.1 point) and unattributed.

## 9. Noise floor, validity and what was set aside

- **In-run control.** The shapes CE cannot affect, `range-1` and `range-100` on both paths, 16 comparisons (wall, client CPU, idle, server CPU): none outside its spread; the largest |difference| is 2.6 % for wall, client CPU and idle (`split` `range-1` client CPU), and -5.7 % for server CPU (`send` `range-1`, spreads 4.3 % and 6.6 %, a 10 ms-tick effect). `loans-by-user` in scenario 3 is the loud one (+9.1 %, above).
- **Within-run floor.** The sweep's three runs spread 0.2 % to 15.0 % (wall 0.2 % to 4.3 %; client CPU 0.2 % to 4.7 %; the 15.0 % is `send` 60,480-row server CPU at A, a 10 ms-tick effect on 7 statements); scenario 1 spread 0.7 % to 4.7 % warm and 0.6 % to 6.4 % cold, scenario 3 0.6 % to 8.8 %, the census 0.8 % to 20.0 %.
- **Reproduction of CE-S0.** A's `send` wall against CE-S0's file (a window about 2.2 hours earlier): 83.8 vs 85.1 µs (-1.5 %), 541.5 vs 548.3 µs (-1.2 %), 4,684.1 vs 4,684.5 µs (0.0 %), 48,421.1 vs 47,662.6 µs (+1.6 %), 316,102.5 vs 318,902.8 µs (-0.9 %): the same client, host and binary measure the same.
- **Order.** The first A run (pair 1) came right after the other session's test run; its probe values are the slowest of A's three (ready at 60,480 rows 30,070 µs against 28,259 and 27,863 µs) and its `send` wall at 60,480 rows is 319,919.5 µs against the three-run median of 316,102.5 µs. The median over three resists one such run. B's three first-batch p50s at 60,480 rows are 1,912 to 1,920 µs (spread 0.4 %), so the A/B comparison of the first batch does not depend on it.
- **Validity.** 72 sweep cells and 12 scenario windows have no build/test sample and no foreign process at 50 % CPU or more (`windows.txt`); 6 sweep cells hold one or more samples of a foreign process between 10 % and 50 % CPU (other sessions' idle `claude` processes). No sweep, probe or scenario cell was set aside. In the supplement one cell was: `filter-sym3` `drain` at pair 3's B run held a `claude` process at 50 % or more, the pass ended, the gate re-ran and the cell was measured again (`rowcost/drain-p3-B.jsonl.contaminated.gz`). All sweep-run verifies passed (36 of 36 shape checks, row counts as expected), `verify_problems` was empty in all six scenario-1 runs, every driver exit code was 0. The census prints `range_v_ok: False` for both arms: its `--only` list omits the every-row `UPDATE` shape, so that end-state check has nothing to read; it is the same in A and B.
- **Host load per cell** (`bench/README.md` rule 4; read from `logs/*.precheck` before each cell, 1-minute `/proc/loadavg`). `pgrep` for `cc1plus|cmake --build|ctest|kds_tests` found none before any of the 25 cells:

| unit | sweep and probe | scenario 1 | scenario 3 | `drain` |
|---|---|---|---|---|
| p1-A | 1.82 load | 0.77 load | 0.71 load | 0.26 load |
| p1-B | 0.76 load | 0.56 load | 0.64 load | 0.09 load |
| p2-B | 0.59 load | 0.56 load | 0.54 load | 0.38 load |
| p2-A | 0.42 load | 0.45 load | 0.66 load | 0.39 load |
| p3-A | 0.54 load | 0.29 load | 0.42 load | 0.41 load |
| p3-B | 0.44 load | 0.43 load | 0.46 load | 0.36 load (0.41 load before the re-measured cell) |

  The census records its own per arm and block: 0.33 to 1.17 load over 84 cells, all clean by its gate.

## 10. What the run teaches about the engine

1. **The premise was a good prediction.** CE-S0 derived, before any streaming build existed, that a slice-boundary stream could hide at most `idle x (1 - 1 / slices)`: 0.95 to 0.99 of that is what B hides, on two client paths, at three sizes (section 4). The engine's reply path was store-and-forward at 0.42 µs of idle per returned row and CE converts all but the first slice of it to overlap.
2. **What bounds the result is the client, then the first slice.** On the `send` path the client decodes for 91.5 % of A's statement and 99.2 % of B's; the statement cannot get shorter than its client CPU, so the best case is 7.0 % at 60,480 rows and 12.0 % on the filtered walk. The server-side term left is the first 64-page slice: about 1.85 to 1.91 ms for a key range and 0.39 ms for the filtered walk. `pages_per_slice` is therefore the lever on time to first batch, and nothing in this run varied it.
3. **A filtered walk benefits most per statement** (12.0 % on `send`, 20.7 % on `split`), as CE-S0 predicted: it walks 877 pages to return 7,560 rows, so most of its server time is a walk that produces few bytes, and the first batch now leaves 3,959.0 µs sooner than A's.
4. **The new path is not free for a reply it cannot overlap.** A 1,000-row reply, three batches and one slice, completes 20.2 µs later (+4.2 %) for a client that does nothing, and the 100-row reply 1.2 µs later (+1.3 %, at the edge of its spread). Below the decoding client's noise, above the `drain` path's. It is a candidate for the next look, with a sampler: it is not explained by the boundary write, because that reply has no boundary.
5. **The overlap shows in the reply's tail as well as its head, at the largest size**: with a client that decodes nothing (`drain`), B's 60,480-row reply is complete 5.9 % sooner (28,364.2 µs to 26,683.7 µs, spreads 2.1 % and 2.2 %); the probe's client agrees in sign (-6.2 %, 28,259.3 µs to 26,520.7 µs) but its A spread is 7.8 %, so that one alone is not a finding. On the filtered walk the same two clients see the reply complete later: +5.3 % in the probe (inside A's 6.5 % spread) and +4.4 % in the `drain` sweep (outside its 1.0 % to 1.4 %).
6. **Nothing here approached a server limit.** One session on one core, the extreme taken the whole table (60,480 rows, 8.5 MB, 130 batches), and the bound is the client (rule 4b). The streaming path's own extremes, a client that stops reading and a wait held past the horizon, belong to CE-S4's tests and were not measured here.
7. **Which open decision acquired data.** CE-Q2 (a), writing at slice boundaries only, is priced: it leaves one slice of server time before the first batch and removes 0.95 to 0.99 of the ceiling that a boundary-only write allows. A write at every batch seal, which CE-S0 section 10 judged unable to help replies of 1,000 rows or fewer, was not built or measured.

## 11. Archive

`bench/v3.3.0/archive/ce-close-v3.3.0-52-g9e568b44/`: `run_close.sh` (master), `run_drain.sh`, `lib.sh`, `monitor.sh`, `watchdog.sh`, `watchdog2.sh`, `cellvalid.py`, `build.sh`, `archive.sh`, `rowcost/ce_s0_rows.py` (+ `rowcost.py`, `kdsrow.py`, unmodified), `ttfb_probe.py`, `analyze_close.py`, `analyze_census.py`, `analyze_drain.py`, `windows.py`, `verify.py`, `analysis.md` and `analysis-drain.md` (every table as rendered), `rowcost/cells-p*.jsonl.gz` and `drain-p*.jsonl.gz` (every statement's latency), `json/` (twelve scenario summaries, six probe files, the census), `logs/`, `conf/`, `monitor.log.gz`, `timeline`, `binary.sha256`, `source-binary-mtime-{A,B}.txt`. Also `headline.py` and `headline.md` (section 4's QPS tables), `waiter.sh`, `windows.txt`, the watchdog logs and `build.*`. No data file, WAL segment or binary is archived. Re-run: `gunzip` the `rowcost/*.jsonl.gz`. Then, on the archive directory, `analyze_close.py` rebuilds sections 3, 4 and 6 and the appendix, `headline.py` rebuilds the QPS tables, `analyze_drain.py` rebuilds section 5 and `analyze_census.py` rebuilds section 8 (this review re-ran all four on 2026-10-20: every table reproduces).

## 12. Appendix: the sweep, per statement

Median of three runs per binary; client CPU is `time.process_time`, idle is wall minus client CPU, server CPU from `/proc/<pid>/stat` (10 ms ticks). Spread is (max - min) / median.

`send` path:

| shape | rows returned | quantity | A | B | B over A | spread A / B | a finding? |
|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | wall | 83.8 µs | 84.3 µs | +0.6 % | 2.9 % / 2.3 % | no (inside the spread) |
| range-1 | 1 rows | client CPU | 51.0 µs | 51.7 µs | +1.3 % | 4.7 % / 1.2 % | no (inside the spread) |
| range-1 | 1 rows | idle (wall - client CPU) | 32.6 µs | 32.6 µs | +0.2 % | 0.9 % / 4.2 % | no (inside the spread) |
| range-1 | 1 rows | server CPU | 32.8 µs | 31.0 µs | -5.7 % | 4.3 % / 6.6 % | no (inside the spread) |
| range-100 | 100 rows | wall | 541.5 µs | 537.5 µs | -0.7 % | 1.4 % / 1.5 % | no (inside the spread) |
| range-100 | 100 rows | client CPU | 459.7 µs | 456.5 µs | -0.7 % | 0.3 % / 0.9 % | no (inside the spread) |
| range-100 | 100 rows | idle (wall - client CPU) | 82.3 µs | 81.0 µs | -1.6 % | 9.6 % / 5.2 % | no (inside the spread) |
| range-100 | 100 rows | server CPU | 85.1 µs | 81.6 µs | -4.2 % | 8.8 % / 6.2 % | no (inside the spread) |
| range-1000 | 1,000 rows | wall | 4,684.1 µs | 4,743.5 µs | +1.3 % | 2.6 % / 2.1 % | no (inside the spread) |
| range-1000 | 1,000 rows | client CPU | 4,239.9 µs | 4,267.1 µs | +0.6 % | 2.4 % / 2.2 % | no (inside the spread) |
| range-1000 | 1,000 rows | idle (wall - client CPU) | 451.2 µs | 472.4 µs | +4.7 % | 6.2 % / 1.7 % | no (inside the spread) |
| range-1000 | 1,000 rows | server CPU | 488.8 µs | 485.8 µs | -0.6 % | 8.9 % / 10.4 % | no (inside the spread) |
| range-10000 | 10,000 rows | wall | 48,421.1 µs | 45,506.3 µs | -6.0 % | 3.0 % / 0.2 % | **yes** |
| range-10000 | 10,000 rows | client CPU | 44,030.9 µs | 43,467.2 µs | -1.3 % | 3.0 % / 0.2 % | no (inside the spread) |
| range-10000 | 10,000 rows | idle (wall - client CPU) | 4,232.6 µs | 1,964.8 µs | -53.6 % | 7.8 % / 5.0 % | **yes** |
| range-10000 | 10,000 rows | server CPU | 4,680.9 µs | 4,893.6 µs | +4.5 % | 6.6 % / 8.2 % | no (inside the spread) |
| range-60480 | 60,480 rows | wall | 316,102.5 µs | 293,937.1 µs | -7.0 % | 1.3 % / 1.2 % | **yes** |
| range-60480 | 60,480 rows | client CPU | 289,330.6 µs | 291,624.2 µs | +0.8 % | 0.9 % / 1.3 % | no (inside the spread) |
| range-60480 | 60,480 rows | idle (wall - client CPU) | 26,831.6 µs | 2,274.7 µs | -91.5 % | 6.6 % / 6.5 % | **yes** |
| range-60480 | 60,480 rows | server CPU | 28,571.4 µs | 28,750.0 µs | +0.6 % | 15.0 % / 4.3 % | no (inside the spread) |
| filter-sym3 | 7,560 rows | wall | 36,239.9 µs | 31,877.6 µs | -12.0 % | 2.1 % / 2.2 % | **yes** |
| filter-sym3 | 7,560 rows | client CPU | 31,472.0 µs | 31,434.7 µs | -0.1 % | 1.9 % / 2.2 % | no (inside the spread) |
| filter-sym3 | 7,560 rows | idle (wall - client CPU) | 4,783.6 µs | 442.9 µs | -90.7 % | 3.8 % / 3.4 % | **yes** |
| filter-sym3 | 7,560 rows | server CPU | 5,074.6 µs | 5,063.3 µs | -0.2 % | 6.0 % / 2.6 % | no (inside the spread) |

`split` path:

| shape | rows returned | quantity | A | B | B over A | spread A / B | a finding? |
|---|---|---|---|---|---|---|---|
| range-1 | 1 rows | wall | 75.2 µs | 76.6 µs | +1.8 % | 2.6 % / 3.8 % | no (inside the spread) |
| range-1 | 1 rows | client CPU | 42.7 µs | 43.8 µs | +2.6 % | 3.1 % / 2.4 % | no (inside the spread) |
| range-1 | 1 rows | idle (wall - client CPU) | 32.5 µs | 32.8 µs | +0.9 % | 3.7 % / 5.6 % | no (inside the spread) |
| range-1 | 1 rows | server CPU | 32.0 µs | 32.0 µs | -0.3 % | 5.8 % / 11.0 % | no (inside the spread) |
| range-100 | 100 rows | wall | 325.0 µs | 325.4 µs | +0.1 % | 4.3 % / 0.4 % | no (inside the spread) |
| range-100 | 100 rows | client CPU | 244.3 µs | 244.0 µs | -0.1 % | 1.5 % / 1.7 % | no (inside the spread) |
| range-100 | 100 rows | idle (wall - client CPU) | 81.2 µs | 81.4 µs | +0.2 % | 13.5 % / 3.6 % | no (inside the spread) |
| range-100 | 100 rows | server CPU | 80.6 µs | 80.3 µs | -0.4 % | 8.9 % / 6.9 % | no (inside the spread) |
| range-1000 | 1,000 rows | wall | 2,659.2 µs | 2,641.5 µs | -0.7 % | 1.9 % / 1.3 % | no (inside the spread) |
| range-1000 | 1,000 rows | client CPU | 2,196.1 µs | 2,186.7 µs | -0.4 % | 2.5 % / 1.8 % | no (inside the spread) |
| range-1000 | 1,000 rows | idle (wall - client CPU) | 440.1 µs | 458.5 µs | +4.2 % | 6.9 % / 1.7 % | no (inside the spread) |
| range-1000 | 1,000 rows | server CPU | 464.6 µs | 480.5 µs | +3.4 % | 7.4 % / 1.1 % | no (inside the spread) |
| range-10000 | 10,000 rows | wall | 26,605.8 µs | 23,871.6 µs | -10.3 % | 1.2 % / 0.8 % | **yes** |
| range-10000 | 10,000 rows | client CPU | 22,309.0 µs | 21,993.7 µs | -1.4 % | 1.0 % / 0.9 % | **yes** |
| range-10000 | 10,000 rows | idle (wall - client CPU) | 4,132.6 µs | 1,878.3 µs | -54.5 % | 6.6 % / 0.7 % | **yes** |
| range-10000 | 10,000 rows | server CPU | 4,456.5 µs | 4,455.4 µs | -0.0 % | 7.2 % / 1.2 % | no (inside the spread) |
| range-60480 | 60,480 rows | wall | 187,348.7 µs | 160,841.3 µs | -14.1 % | 1.3 % / 1.7 % | **yes** |
| range-60480 | 60,480 rows | client CPU | 159,042.4 µs | 158,644.7 µs | -0.3 % | 1.8 % / 1.7 % | no (inside the spread) |
| range-60480 | 60,480 rows | idle (wall - client CPU) | 27,374.3 µs | 2,196.6 µs | -92.0 % | 6.4 % / 1.9 % | **yes** |
| range-60480 | 60,480 rows | server CPU | 30,000.0 µs | 28,000.0 µs | -6.7 % | 7.7 % / 0.4 % | no (inside the spread) |
| filter-sym3 | 7,560 rows | wall | 20,884.4 µs | 16,565.5 µs | -20.7 % | 1.7 % / 0.4 % | **yes** |
| filter-sym3 | 7,560 rows | client CPU | 16,203.1 µs | 16,147.1 µs | -0.3 % | 1.6 % / 0.5 % | no (inside the spread) |
| filter-sym3 | 7,560 rows | idle (wall - client CPU) | 4,586.3 µs | 401.5 µs | -91.2 % | 4.3 % / 5.5 % | **yes** |
| filter-sym3 | 7,560 rows | server CPU | 4,827.6 µs | 4,802.6 µs | -0.5 % | 5.2 % / 1.3 % | no (inside the spread) |
