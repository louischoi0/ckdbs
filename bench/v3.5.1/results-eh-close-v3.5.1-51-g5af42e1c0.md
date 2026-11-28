# EH close: the native fold per row, OLD against NEW in one window

**One line.** At EH's close, the global folds with no predicate cost **10.5 to 27.9 ns of server CPU a row, 0.54x to 0.71x of the opening binary's 19.4 to 39.1 ns** (five shapes, all outside the spread), and now sit at 0.86x to 1.00x of PostgreSQL 18.7. The grouped folds with 8 or 7,560 groups are **inside the spread** against the opening binary. **One shape went the wrong way: a fold that founds a group for every row (`group-60480`) costs 280.1 ns a row against 237.2 ns on the opening binary (1.18x, dearer), and 284.5 ns against 170.9 ns on the EH-S2 stage binary (1.67x).** The regression arrived after S2, in the Sy range of commits, and is not fixed here. The control the change cannot reach (`pk-lookup`, `day-slice`) did not move (1.02x and 1.00x of server CPU, both inside the spread). Scenario 4's day-1 close phase reads 1.18x to 1.24x faster in QPS in all three arms.

Measured in the session that closes EH by `ck-tester`; the stage files this one reads against are `results-eh-s0-premise-v3.5.1-35-g01c6c741c.md`, `results-eh-s1-in-place-v3.5.1-41-g6142f32f1.md` and `results-eh-s2-group-table-v3.5.1-46-g70e0cb368.md`, and the work order is `instructions/v3.2.0/workorder-eh-native-fold-per-row.md`. Drivers are documented where they live (`tools/eh_s0_premise.py` wraps `tools/parity_bench.py`; `tools/scenario4_cabinopt_days.py`; `tools/pg_scenario4_cabinopt_days.py`); this file does not repeat how to run them.

## Stamp

| Field | Value |
|---|---|
| Version, commit | worktree `eh-native-fold-per-row`, branch `worktree-eh-native-fold-per-row`, `5af42e1c0` = `v3.5.1-51-g5af42e1c0`; the tree was clean when the run started |
| Date and time | 2026-11-28 05:10:24 UTC to 05:19:44 UTC, **9 min 20 s** of measurement (cell 1 and cell 3: 05:10:24 to 05:12:08; cell 2: 05:12:08 to 05:18:38; the localisation cell 4: 05:18:57 to 05:19:44), inside the 20-minute budget, nothing cut by time |
| OLD binary | Release from `git archive 9830e6bb0`, copied to `kds_server.old`, sha256 `3560324f74422c0b103344bd16dd89c208afbfa09182222f738df7f7e9146487`, source mtime 2026-11-27 22:34:36 -0500. `git diff 9830e6bb0 01c6c741c -- src include CMakeLists.txt` is empty (checked in this session), so its engine source is EH's opening commit `01c6c741c` (`v3.5.1-35-g01c6c741c`). The binary is 1 minute 34 seconds newer than that commit |
| NEW binary | Release from `git archive 5af42e1c0`, copied to `kds_server.new`, sha256 `553b0ed54734fa603b7cf46fb98b52d9ddeb7656ac4a0e06d52743cdc79268ef`, source mtime 2026-11-28 00:09:06 -0500, 40 seconds after the commit's timestamp (00:08:26 -0500), so it is not older than HEAD. Engine source differs from OLD in seven files: `aggregate.{hpp,cpp}`, `bytes_fold.hpp`, `step_vm.{hpp,cpp}`, `command_dispatcher.{hpp,cpp}` |
| Cell 4's extra binary | `kds_server.s2`, the EH-S2 stage's measured binary (sha256 `5ac068dd77bc80349ff41d5b31df499e14b60c64a7edaea2dfc22522c5328d4c`, the hash `archive/eh-s2-v3.5.1-46-g70e0cb368/binaries.sha256` carries), used only to place a regression |
| Every server | started from the copy in the run directory, never from a build tree; a fresh data file each, deleted after |
| KDS configuration | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`; cell 2 adds `decay_half_life = 5` and `cabin_optimizer_snapshot_interval_ms = 500` |
| PostgreSQL | 18.7 (Debian 18.7-1.pgdg13+5), `docker.io/library/postgres:18` under `podman` on the host network, port 15434, defaults but `--locale=C`/UTF8 at `initdb`; a fresh container for cells 1 to 3 and another for cell 4 |
| Placement | KDS servers and the PostgreSQL container `taskset -c 0-7` (P-cores); every driver `taskset -c 8-23`. The `powersave` governor, Intel Core Ultra 7 270K Plus, 24 CPUs, kernel 7.3.6-300.fc45 |
| Device | the run directory `/home/ukjin/bench-runs/eh-close-v3.5.1-51-g5af42e1c0/` on `/dev/mapper/fedora-root`, **xfs** (`df -T` at run time, archived). The data files and WAL are there; nothing is on tmpfs |
| Ports | `ss -ltn` first: 15791 (KDS), 15434 (PostgreSQL) and 15651 to 15653 (scenario 4) were free; 15433 is held by another project and unused |
| Build | Release (both); the suite was **not executed** in this run, because the run changes no code. The stage logs carry EH's suite results |
| Drivers | the worktree's `tools/` at `5af42e1c0`, copied to the run directory and compared byte for byte (`cmp`) with the worktree; hashes in the archive |
| Comparator | the OLD binary in the same window (rule 4); PostgreSQL stands beside it as the floor |
| Rule | spread = (max - min) / median over a binary's cells; a difference smaller than the larger spread is "inside the spread"; a spread over 20 % carries no claim |

### The host, per cell

The quiet gate (no `cc1plus|cmake --build|ctest|kds_tests` process and at most 8 % busy over 3 s; `kds_server` is not in the pattern, another project's server runs permanently) **passed at once before every cell, with a gate wait of 0 s** and a last-sample busy figure of 0 % or 1 %. The 1-minute load average is the host's recent history, not the gate's sample: the first cells carry the highest figures; what produced that load is not known.

| cell | UTC | `/proc/loadavg` (1, 5, 15 min) | gate busy | gate wait |
|---|---|---|---|---|
| cell 1, rounds 1 to 6 (old, new, new, old, old, new) | 05:10:31, :40, :49, :58, 05:11:07, :16 | 1.54, 1.61, 1.44, 1.37, 1.24, 1.22 (1 min); 3.25 down to 2.93 (5 min) | 0 to 1 % | 0 s each |
| cell 3, rounds 1 to 6 (same order) | 05:11:25, :32, :38, :46, :54, 05:12:01 | 1.18, 1.08, 1.00, 1.00, 1.14, 1.13 (1 min) | 0 to 1 % | 0 s each |
| cell 2, PostgreSQL | 05:12:08 and 05:18:36 | 1.03 and 0.60 (1 min) | 0 % | 0 s |
| cell 2, OLD run 1, NEW run 1, NEW run 2, OLD run 2 | 05:12:13, 05:13:49, 05:15:25, 05:17:01 | 1.03, 0.53, 0.57, 0.83 (1 min) | 1, 1, 0, 1 % | 0 s each |
| cell 4, rounds 1 to 6 (s2, new, new, s2, s2, new) | 05:19:04 to 05:19:40 | 0.32 to 0.52 (1 min) | 0 to 1 % | 0 s each |

The 5-minute load of 2.4 to 3.3 at cell 1 means the first nine minutes of the window were not as idle as the last. Cell 1 and cell 3 were run in that window for OLD and NEW alike, interleaved, so a drift lands on both; the per-binary spreads below are the price of it. Every cell's `precheck` file is in the archive, and no `competing` process was recorded in any of the 432 driver cells of cells 1, 3 and 4.

### Clients (bench README, "A ratio against another engine names both clients")

| cell | KDS side times | PostgreSQL side times |
|---|---|---|
| 1, 3, 4 (`parity_bench`) | `kwp.Connection.query`: rows counted, not decoded | `pg_wire.query`: rows counted, not decoded |
| 2 (scenario 4) | `ServerConnection.send_command` over counted rows (`meta.timed_client = "count"`) | `pg_wire.send_command`, which wraps `pg_wire.query`: rows counted, not decoded |

The ratio columns against PostgreSQL in cell 1 are in server CPU, which no client choice touches. Cell 2 sets no ratio against PostgreSQL: its twin runs one day without the pacing and the PostgreSQL phase of `d1-pk` has a 32.4 % spread.

## Cell 1: the eleven shapes

**Finding.** The fold's per-row cost fell by 29 % to 46 % on the five global shapes (S1's in-place cells) and did not move outside the spread on the two 8-group shapes or on the two 7,560-group shapes; the `group-60480` shape is 18 % dearer. The data set is `pb_bars`, 60,480 rows of 8 columns (`--symbols 8 --sessions 7560`, so `--bars` = 60,480); server CPU a row is the shape's server CPU net of idle, divided by 60,480 rows, a fitted per-row cost and so not a throughput (the QPS follow in the second table). Six rounds, old, new, new, old, old, new, two reps a round of 20 statements (10 for the 7,560-group shapes, 5 for `group-60480`), so six cells a binary and twelve for PostgreSQL. Spread in brackets.

| shape | OLD | NEW | NEW / OLD | read | PostgreSQL | NEW / PostgreSQL | read |
|---|---|---|---|---|---|---|---|
| `scan-reject` | 11.2 ns (11.4 %) | 11.0 ns (8.8 %) | **0.99x** | inside the spread | 18.1 ns (5.0 %) | **0.61x** | KDS cheaper |
| `count-star` | 19.4 ns (7.6 %) | 10.5 ns (4.0 %) | **0.54x** | NEW cheaper | 12.2 ns (9.4 %) | **0.86x** | KDS cheaper |
| `count-col` | 27.8 ns (6.2 %) | 17.9 ns (2.2 %) | **0.65x** | NEW cheaper | 18.9 ns (3.7 %) | **0.95x** | KDS cheaper |
| `sum` | 27.7 ns (5.9 %) | 18.1 ns (2.8 %) | **0.65x** | NEW cheaper | 20.3 ns (2.0 %) | **0.89x** | KDS cheaper |
| `min-max` | 32.4 ns (3.7 %) | 22.3 ns (3.6 %) | **0.69x** | NEW cheaper | 22.3 ns (2.9 %) | **1.00x** | inside the spread |
| `agg-global` | 39.1 ns (5.2 %) | 27.9 ns (8.4 %) | **0.71x** | NEW cheaper | 28.0 ns (2.3 %) | **1.00x** | inside the spread |
| `group-8` | 43.7 ns (3.1 %) | 42.4 ns (3.4 %) | **0.97x** | inside the spread | 39.5 ns (10.4 %) | **1.07x** | inside the spread |
| `group-8-sum` | 49.7 ns (2.5 %) | 49.5 ns (3.6 %) | **1.00x** | inside the spread | 50.2 ns (9.3 %) | **0.99x** | inside the spread |
| `group-7560` | 75.3 ns (6.8 %) | 68.4 ns (12.9 %) | **0.91x** | inside the spread | 59.6 ns (5.8 %) | **1.15x** | KDS dearer |
| `group-7560-sum` | 88.3 ns (10.3 %) | 81.5 ns (16.7 %) | **0.92x** | inside the spread | 77.5 ns (5.3 %) | **1.05x** | inside the spread |
| `group-60480` | 237.2 ns (2.0 %) | 280.1 ns (5.1 %) | **1.18x** | NEW dearer | 194.9 ns (6.3 %) | **1.44x** | KDS dearer |

Reading, against the opening binary: `count-star`, `count-col`, `sum`, `min-max` and `agg-global` are cheaper by 46 %, 35 %, 35 %, 31 % and 29 %, every one outside both spreads. They now read at or below PostgreSQL's cost a row (`min-max` and `agg-global` at 1.00x, inside the spread). `scan-reject` is a filter nothing touched (0.99x, inside the spread). `group-8` and `group-8-sum` are inside the spread. `group-7560` and `group-7560-sum` read 0.91x and 0.92x, but NEW's spreads are 12.9 % and 16.7 % and the difference is inside them, so no claim. `group-60480` is the one outside every spread in the wrong direction: six cells a binary, 5.1 % and 2.0 %, and every NEW cell is above every OLD cell (OLD 236.3 to 241.1 ns, NEW 274.3 to 288.4 ns a row). Against PostgreSQL it is 1.44x dearer in server CPU, where the S2 stage file read it 0.90x.

### Latency a statement, cell 1

Medians over the cells of a binary. A cell is 20 statements (10 and 5 on the last three shapes), so **p95 and p99 are the cell's maximum** and the two columns repeat; they are shown because the rule asks for them and they say the cell's worst statement, not a tail.

| shape | binary | cells | QPS (derived, 1e6 / mean) | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|
| `scan-reject` | OLD | 6 cells | 1,327 QPS | 758.8 µs | 678.2 µs | 685.1 µs | 732.7 µs | 1,017.6 µs | 1,017.6 µs |
| `scan-reject` | NEW | 6 cells | 1,210 QPS | 848.4 µs | 734.6 µs | 794.9 µs | 815.1 µs | 1,149.3 µs | 1,149.3 µs |
| `scan-reject` | PostgreSQL | 12 cells | 864 QPS | 1,157.7 µs | 1,052.4 µs | 1,073.7 µs | 1,099.5 µs | 1,461.8 µs | 1,461.8 µs |
| `count-star` | OLD | 6 cells | 817 QPS | 1,223.6 µs | 1,193.5 µs | 1,203.4 µs | 1,210.7 µs | 1,326.0 µs | 1,326.0 µs |
| `count-star` | NEW | 6 cells | 1,498 QPS | 667.4 µs | 644.9 µs | 656.3 µs | 661.4 µs | 751.9 µs | 751.9 µs |
| `count-star` | PostgreSQL | 12 cells | 1,252 QPS | 798.6 µs | 717.2 µs | 734.0 µs | 745.2 µs | 1,008.3 µs | 1,008.3 µs |
| `count-col` | OLD | 6 cells | 566 QPS | 1,765.4 µs | 1,714.6 µs | 1,725.3 µs | 1,738.1 µs | 1,943.5 µs | 1,943.5 µs |
| `count-col` | NEW | 6 cells | 873 QPS | 1,145.4 µs | 1,110.4 µs | 1,120.2 µs | 1,128.2 µs | 1,366.8 µs | 1,366.8 µs |
| `count-col` | PostgreSQL | 12 cells | 844 QPS | 1,184.2 µs | 1,143.7 µs | 1,160.5 µs | 1,170.4 µs | 1,374.9 µs | 1,374.9 µs |
| `sum` | OLD | 6 cells | 566 QPS | 1,766.7 µs | 1,702.0 µs | 1,715.7 µs | 1,733.2 µs | 2,008.2 µs | 2,008.2 µs |
| `sum` | NEW | 6 cells | 868 QPS | 1,152.2 µs | 1,114.0 µs | 1,124.7 µs | 1,134.2 µs | 1,312.1 µs | 1,312.1 µs |
| `sum` | PostgreSQL | 12 cells | 792 QPS | 1,263.3 µs | 1,225.5 µs | 1,238.8 µs | 1,252.7 µs | 1,446.9 µs | 1,446.9 µs |
| `min-max` | OLD | 6 cells | 489 QPS | 2,045.5 µs | 1,997.1 µs | 2,006.8 µs | 2,019.1 µs | 2,240.0 µs | 2,240.0 µs |
| `min-max` | NEW | 6 cells | 700 QPS | 1,429.0 µs | 1,379.1 µs | 1,393.5 µs | 1,401.8 µs | 1,618.1 µs | 1,618.1 µs |
| `min-max` | PostgreSQL | 12 cells | 724 QPS | 1,381.1 µs | 1,346.8 µs | 1,364.1 µs | 1,371.4 µs | 1,565.6 µs | 1,565.6 µs |
| `agg-global` | OLD | 6 cells | 406 QPS | 2,460.7 µs | 2,393.9 µs | 2,414.4 µs | 2,431.0 µs | 2,706.7 µs | 2,706.7 µs |
| `agg-global` | NEW | 6 cells | 558 QPS | 1,791.1 µs | 1,723.3 µs | 1,738.5 µs | 1,756.6 µs | 2,015.6 µs | 2,015.6 µs |
| `agg-global` | PostgreSQL | 12 cells | 578 QPS | 1,730.1 µs | 1,690.9 µs | 1,704.7 µs | 1,713.3 µs | 1,926.4 µs | 1,926.4 µs |
| `group-8` | OLD | 6 cells | 358 QPS | 2,794.5 µs | 2,656.0 µs | 2,702.2 µs | 2,767.8 µs | 3,036.7 µs | 3,036.7 µs |
| `group-8` | NEW | 6 cells | 373 QPS | 2,681.9 µs | 2,596.9 µs | 2,617.8 µs | 2,633.5 µs | 2,908.9 µs | 2,908.9 µs |
| `group-8` | PostgreSQL | 12 cells | 398 QPS | 2,512.0 µs | 2,348.6 µs | 2,409.9 µs | 2,501.2 µs | 2,698.5 µs | 2,698.5 µs |
| `group-8-sum` | OLD | 6 cells | 311 QPS | 3,220.1 µs | 3,051.1 µs | 3,091.9 µs | 3,171.2 µs | 3,450.4 µs | 3,450.4 µs |
| `group-8-sum` | NEW | 6 cells | 310 QPS | 3,230.4 µs | 3,054.5 µs | 3,102.8 µs | 3,203.9 µs | 3,495.7 µs | 3,495.7 µs |
| `group-8-sum` | PostgreSQL | 12 cells | 309 QPS | 3,239.7 µs | 2,984.5 µs | 3,111.3 µs | 3,217.5 µs | 3,525.7 µs | 3,525.7 µs |
| `group-7560` | OLD | 6 cells | 165 QPS | 6,076.3 µs | 5,852.1 µs | 5,911.7 µs | 6,049.0 µs | 6,291.1 µs | 6,291.1 µs |
| `group-7560` | NEW | 6 cells | 179 QPS | 5,603.0 µs | 5,451.4 µs | 5,533.4 µs | 5,613.4 µs | 5,792.5 µs | 5,792.5 µs |
| `group-7560` | PostgreSQL | 12 cells | 129 QPS | 7,722.1 µs | 7,609.9 µs | 7,665.5 µs | 7,711.8 µs | 7,856.1 µs | 7,856.1 µs |
| `group-7560-sum` | OLD | 6 cells | 134 QPS | 7,449.3 µs | 7,244.4 µs | 7,321.3 µs | 7,409.6 µs | 7,711.5 µs | 7,711.5 µs |
| `group-7560-sum` | NEW | 6 cells | 142 QPS | 7,025.9 µs | 6,912.3 µs | 6,975.5 µs | 7,046.9 µs | 7,194.3 µs | 7,194.3 µs |
| `group-7560-sum` | PostgreSQL | 12 cells | 110 QPS | 9,128.6 µs | 8,983.9 µs | 9,033.0 µs | 9,139.4 µs | 9,353.3 µs | 9,353.3 µs |
| `group-60480` | OLD | 6 cells | 41 QPS | 24,632.7 µs | 23,365.3 µs | 23,785.6 µs | 24,417.5 µs | 25,953.1 µs | 25,953.1 µs |
| `group-60480` | NEW | 6 cells | 36 QPS | 27,690.5 µs | 27,395.5 µs | 27,567.0 µs | 27,759.6 µs | 27,925.5 µs | 27,925.5 µs |
| `group-60480` | PostgreSQL | 12 cells | 24 QPS | 42,190.6 µs | 41,306.5 µs | 41,737.5 µs | 42,428.7 µs | 42,795.3 µs | 42,795.3 µs |

The wall clock agrees with the CPU: `count-star` goes from 1,223.6 µs to 667.4 µs a statement (817 to 1,498 QPS), `agg-global` from 2,460.7 µs to 1,791.1 µs, and `group-60480` from 24,632.7 µs to 27,690.5 µs (41 to 36 QPS).

### Waits

Where a latency here is composed: a statement is server CPU (the table above, times 60,480 rows), plus the client's counting and the socket round trip, plus the statements' wait for the device. **The device wait is nil by construction**: the table is read from a 65,536-frame buffer pool that holds it whole and the folds write nothing, so no fsync, no `io.page_read` and no lock wait is on the path. The remainder of the mean over the server CPU is the round trip plus the client: for `count-star` on NEW, 10.5 ns x 60,480 = 635 µs of server CPU in a mean of 667.4 µs, so the round trip and the counting client are about 32 µs, 5 % of it; that is a difference of two medians (6 cells and 6 cells) and is not a measured wait. A per-wait breakdown through `sys.wait_stats` was not taken.

## Cell 4: where the `group-60480` regression sits

Cell 1 reads the close binary against the opening one. The S2 stage's own file measured `group-60480` at 173.7 ns, so a second comparison was added after cell 1 showed the shape dear: the S2 stage's binary and NEW, interleaved the same way (s2, new, new, s2, s2, new), only the four grouped shapes, two reps a round.

| shape | S2-stage binary | NEW | NEW / S2-stage | read | PostgreSQL | mean statement, S2-stage | mean statement, NEW |
|---|---|---|---|---|---|---|---|
| `group-8` | 42.5 ns (3.6 %) | 43.4 ns (2.2 %) | **1.02x** | inside the spread | 38.0 ns (3.4 %) | 2,854 µs | 2,982 µs |
| `group-7560` | 63.8 ns (1.8 %) | 68.8 ns (12.0 %) | **1.08x** | inside the spread | 57.8 ns (4.7 %) | 5,374 µs | 5,651 µs |
| `group-7560-sum` | 75.8 ns (2.8 %) | 82.1 ns (14.8 %) | **1.08x** | inside the spread | 75.3 ns (3.0 %) | 6,701 µs | 7,066 µs |
| `group-60480` | 170.9 ns (4.6 %) | 284.5 ns (3.2 %) | **1.67x** | NEW dearer | 190.7 ns (7.9 %) | 20,608 µs | 27,842 µs |

The S2 stage binary reproduces its own figure (170.9 ns against its file's 173.7 ns), so the window is comparable and **the 1.67x is between the S2 stage and the close**. The commits between them in this worktree are `8eb475676` (the aggregated SELECT's walk handed to the sliced driver, on an aggregator of its own), `da1f786c3` (comments) and `5af42e1c0` (the aggregator pool bounded). **Which of them costs is not bisected here**; only the end points are measured. The 7,560-group shapes read 1.08x with 12 % and 15 % spreads on NEW, inside the spread, and in cell 1 the first rep of each of NEW's three rounds is dearer than the second by 7.2 to 7.4 ns a row at `group-7560` (OLD: 0.4 to 4.7 ns), which is not resolved at this sample size.

## Cell 3: the control the change cannot reach (EH-R5)

`pk-lookup` (tuple key equality, one row, 2,000 statements a rep) and `day-slice` (`session_no` equality, 8 rows, 200 statements a rep), `parity_bench.py --shapes pk-lookup,day-slice --reps 3`, **six rounds** (old, new, new, old, old, new) rather than the three the plan named, since a round cost 7 s: nine cells a binary, eighteen for PostgreSQL. Neither statement is an aggregate, so neither takes the changed code.

| shape | binary | cells | server CPU a statement | QPS (derived, 1e6 / mean) | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|---|
| `pk-lookup` | OLD | 9 cells | 6.6 µs (15.7 %) | 52,004 QPS (7.3 %) | 19.2 µs | 17.5 µs | 19.0 µs | 19.2 µs | 20.0 µs | 23.4 µs |
| `pk-lookup` | NEW | 9 cells | 6.8 µs (8.6 %) | 51,108 QPS (8.3 %) | 19.6 µs | 17.7 µs | 19.1 µs | 19.4 µs | 20.3 µs | 23.0 µs |
| `pk-lookup` | PostgreSQL | 18 cells | 10.7 µs (13.2 %) | 46,447 QPS (11.8 %) | 21.5 µs | 19.6 µs | 21.1 µs | 21.3 µs | 22.7 µs | 25.1 µs |
| `day-slice` | OLD | 9 cells | 590.2 µs (2.3 %) | 1,487 QPS (8.0 %) | 672.4 µs | 614.8 µs | 660.8 µs | 675.4 µs | 694.7 µs | 772.2 µs |
| `day-slice` | NEW | 9 cells | 590.9 µs (1.5 %) | 1,508 QPS (7.2 %) | 663.0 µs | 611.0 µs | 653.1 µs | 668.1 µs | 682.3 µs | 700.6 µs |
| `day-slice` | PostgreSQL | 18 cells | 884.6 µs (2.4 %) | 1,016 QPS (8.1 %) | 984.0 µs | 878.4 µs | 942.0 µs | 960.1 µs | 1,144.1 µs | 1,192.0 µs |

**EH-R5 holds on this sample.** `pk-lookup` is 1.02x of OLD's server CPU (6.8 µs against 6.6 µs a statement; spreads 8.6 % and 15.7 %), and 0.98x of its QPS (51,108 against 52,004; spreads 8.3 % and 7.3 %): inside the spread on both. `day-slice` is 1.00x in server CPU (590.9 µs against 590.2 µs; spreads 1.5 % and 2.3 %) and 1.01x in QPS (1,508 against 1,487; spreads 7.2 % and 8.0 %). A point `SELECT` and a walked `SELECT` were not made slower. A regression smaller than the spread (up to about 16 % of `pk-lookup`'s server CPU, up to about 2 % of `day-slice`'s) would not show here. Both KDS binaries are ahead of PostgreSQL's 10.7 µs and 884.6 µs of server CPU on these two shapes, which says only that the engine is in the league.

## Cell 2: scenario 4, day 1 `close`

`scenario4_cabinopt_days.py --days 1` with three servers (`off`, `on`, `declared`), one per arm, on `decay_half_life = 5` and `cabin_optimizer_snapshot_interval_ms = 500`; each run 92 s. Runs interleaved old, new, new, old, with the PostgreSQL twin (`pg_scenario4_cabinopt_days.py --seed 1 --days 1`) before and after: two runs a binary and two for PostgreSQL. **The day's `close` phase is 3 rounds of 4 full-scan aggregates (COUNT, SUM, GROUP BY) a run, so 12 statements per cell**; its spread is a two-run spread and the percentile columns over 12 statements repeat p95 and p99. `d1-pk` is the 240-statement control that touches no Cabin and no fold. All three arms' servers ran the same binary in a run; the `off`, `on` and `declared` phases of one run are interleaved by block by the driver. Cabin evidence: the `on` arm created 4 Cabins in every run, and the byte-identical verification across the three arms passed on every run (18 statements, no mismatch, OLD and NEW alike); client errors 0.

| phase | binary | runs | ops a run | QPS, each run | QPS median (spread) | NEW / OLD QPS | read | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `d1-close[off]` | OLD | 2 | 12 | 1,135 QPS, 1,205 QPS | 1,170 QPS (5.9 %) |  |  | 855.5 µs | 312.4 µs | 409.2 µs | 559.4 µs | 2,378.6 µs | 2,378.6 µs |
| `d1-close[off]` | NEW | 2 | 12 | 1,357 QPS, 1,516 QPS | 1,437 QPS (11.1 %) | **1.23x** | NEW faster | 698.1 µs | 188.4 µs | 264.8 µs | 453.3 µs | 2,381.9 µs | 2,381.9 µs |
| `d1-pk[off]` | OLD | 2 | 240 | 48,870 QPS, 48,096 QPS | 48,483 QPS (1.6 %) |  |  | 20.6 µs | 17.6 µs | 18.9 µs | 19.3 µs | 28.5 µs | 41.8 µs |
| `d1-pk[off]` | NEW | 2 | 240 | 50,575 QPS, 50,723 QPS | 50,649 QPS (0.3 %) | **1.04x** | NEW faster | 19.8 µs | 17.6 µs | 18.9 µs | 19.2 µs | 22.7 µs | 29.9 µs |
| `d1-close[on]` | OLD | 2 | 12 | 1,368 QPS, 1,577 QPS | 1,473 QPS (14.2 %) |  |  | 682.3 µs | 277.0 µs | 359.6 µs | 569.5 µs | 1,602.9 µs | 1,602.9 µs |
| `d1-close[on]` | NEW | 2 | 12 | 1,773 QPS, 1,893 QPS | 1,833 QPS (6.6 %) | **1.24x** | NEW faster | 546.2 µs | 198.1 µs | 253.8 µs | 307.8 µs | 1,444.3 µs | 1,444.3 µs |
| `d1-pk[on]` | OLD | 2 | 240 | 49,267 QPS, 51,086 QPS | 50,177 QPS (3.6 %) |  |  | 20.0 µs | 17.6 µs | 18.7 µs | 19.1 µs | 24.2 µs | 30.5 µs |
| `d1-pk[on]` | NEW | 2 | 240 | 49,676 QPS, 51,498 QPS | 50,587 QPS (3.6 %) | **1.01x** | inside the spread | 19.8 µs | 17.6 µs | 19.0 µs | 19.2 µs | 23.2 µs | 28.1 µs |
| `d1-close[declared]` | OLD | 2 | 12 | 1,687 QPS, 1,640 QPS | 1,664 QPS (2.8 %) |  |  | 601.2 µs | 302.2 µs | 319.5 µs | 389.4 µs | 1,528.1 µs | 1,528.1 µs |
| `d1-close[declared]` | NEW | 2 | 12 | 1,848 QPS, 2,086 QPS | 1,967 QPS (12.1 %) | **1.18x** | NEW faster | 510.3 µs | 213.2 µs | 256.1 µs | 359.5 µs | 1,258.5 µs | 1,258.5 µs |
| `d1-pk[declared]` | OLD | 2 | 240 | 51,750 QPS, 51,190 QPS | 51,470 QPS (1.1 %) |  |  | 19.4 µs | 17.6 µs | 18.9 µs | 19.1 µs | 21.6 µs | 26.0 µs |
| `d1-pk[declared]` | NEW | 2 | 240 | 51,930 QPS, 51,639 QPS | 51,785 QPS (0.6 %) | **1.01x** | inside the spread | 19.4 µs | 17.5 µs | 18.8 µs | 19.1 µs | 21.8 µs | 26.1 µs |
| `d1-close[pg]` | PostgreSQL | 2 | 12 | 2,049 QPS, 2,093 QPS | 2,071 QPS (2.1 %) | | | 483.0 µs | 161.2 µs | 166.1 µs | 210.2 µs | 1,542.3 µs | 1,542.3 µs |
| `d1-pk[pg]` | PostgreSQL | 2 | 240 | 34,925 QPS, 48,414 QPS | 41,670 QPS (32.4 %) | | | 24.6 µs | 18.8 µs | 20.2 µs | 20.6 µs | 28.9 µs | 153.0 µs |

**Reading.** All three `close` arms read faster on NEW, by 1.23x, 1.24x and 1.18x in QPS, and the three shift in the same direction because each arm runs the same close statements; they are the same measurement three times over, not three independent ones. The `off` arm's NEW spread is 11.1 % and the `declared` arm's 12.1 %; the differences (18 % to 24 %) clear both spreads, but a two-run spread is a floor on the noise, not an estimate of it, and each cell is 12 statements. **The control moved too**: `d1-pk[off]` reads 1.04x faster on NEW (spreads 1.6 % and 0.3 %, which is outside the spread by this rule) while `d1-pk[on]` and `d1-pk[declared]` read 1.01x, inside it. A statement that cannot reach the change moved 4 % in one arm, so **any scenario 4 difference under about 4 % is not a finding**, and the close phase's 18 % to 24 % is several times that. The PostgreSQL twin is shown as the floor and no ratio is taken: its `d1-pk` spread is 32.4 % (one run has a 278.7 µs p99 outlier), above the 20 % line, and its `close` phase is 12 statements of a day with no Cabin.

## What the run teaches

1. **S1 paid off where it was aimed, and S2 did not move what it was not aimed at.** The global fold dropped from 19.4 ns to 10.5 ns a row on `count-star` and from 39.1 ns to 27.9 ns on `agg-global`, now level with PostgreSQL's 12.2 ns and 28.0 ns: the shapes in S0 that add a decode and a fold to the counted row are the ones that fell most in ns a row, which fits the per-row decode being what the page-bytes fold removes; no profile was taken. The gap to PostgreSQL on `min-max` and `agg-global` is closed (1.00x), which is the work order's stated aim for the global shapes; the work order is the document carrying the expectation.
2. **The group table's win is real at the S2 stage and not present at the close for a group-per-row fold.** `group-60480` read 170.9 ns on the S2 binary and reads 284.5 ns on the close binary in the same window, while `group-8` (1.02x) is level. A cost that appears only where the table founds a group for every row points at the group table's founding or growth path in the sliced driver's pooled aggregator, not at the fold's per-row path; that is a hypothesis, not a finding, and it names where to bisect (`8eb475676`, `da1f786c3`, `5af42e1c0`). It should be settled before the milestone is read as closed: the S2 file's 0.71x of the opening binary at this shape (a figure that carried no claim there, at a 27 % spread) is now 1.18x dearer, outside the spread.
3. **A yielding fold costs the plain walk nothing.** `pk-lookup` and `day-slice` are level, and the three scenario 4 `pk` controls moved by 1 % to 4 %, so Sy's handing an aggregate to the sliced driver did not reach the paths that do not aggregate (EH-R5, at the resolution stated above).
4. **A scenario's close phase moves with the fold.** The close aggregates read 18 % to 24 % faster in QPS in every arm, on 12 statements a cell and two runs a binary. Whether a Cabin serves any close statement was not checked, and the arms differ on both binaries (OLD: 1,170 QPS `off`, 1,664 QPS `declared`), so the run says nothing about why.

## What was cut, absent or unverified

- **The suite was not executed** in this run, since no code changed; the stage logs carry EH's results.
- **Cell 3 ran six rounds, not three**, and cell 2 ran `--days 1` as the plan allowed, not the 3 days.
- **Cell 2's close phase is 12 statements a cell and two runs a binary**: its spreads are a floor; the PostgreSQL `d1-pk` row carries no claim (32.4 %).
- **The `group-60480` regression is placed between the S2 stage binary and NEW but not bisected** to a commit, and its cause is unverified; no profile was taken.
- **The group-7560 shapes carry no claim against OLD** (NEW spreads 12.9 % and 16.7 %); against the S2 stage they read 1.08x, inside the spread.
- **Waits** by `sys.wait_stats` were not taken; the device wait is nil by construction (a resident table, no write).
- **Only integer keys of one column** are in the grouped shapes; a `varchar` or multi-column key is pinned for correctness by the aggregate suite and not measured here.
- **`scan-reject` and `group-8`-class shapes** ran at one data set (60,480 rows). **Rule 9's three row counts (200, 1K, 10K) are not swept**: cells 1 and 4 measure one 60,480-row relation, and the per-row cost is a fit at that size only; scenario 4's tapes carry 200, 1,000 and 10,000 rows but the `close` phase is not split by them. This is a cut, not an exemption.
- **The first six rounds ran while the host's 5-minute load was still 2.5 to 3.3**; the gate passed at 0 to 1 % busy and nothing competing was recorded, but a quieter host would be needed to narrow the 12 % to 17 % spreads of `group-7560`.

## Archive

`bench/v3.5.1/archive/eh-close-v3.5.1-51-g5af42e1c0/`: every cell's JSON and stdout, return codes, per-cell prechecks (load, gate wait), the server configurations, `run.sh`, `c4.sh`, the watchdog and its log, the timeline, the device, the PostgreSQL version and affinity and server log, `binary.sha256`, `source-binary-mtime.txt` and `tools.sha256`. No data file or WAL segment is archived.
