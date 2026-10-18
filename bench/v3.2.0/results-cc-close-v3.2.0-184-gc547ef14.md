# CC close: the join planner's statistics cost nothing off the planning path and buy 13-62 % of server CPU on the flips CC-S0 found

Work order: `instructions/v3.2.0/workorder-cc-plan-statistics.md` (§9 CC-close, §10), spec
`docs/spec/plan-stats.md`. Raw files, every block's per-statement latencies, the plans, the SHOW META
snapshots and the harness: `bench/v3.2.0/archive/cc-close-v3.2.0-184-gc547ef14/`.

**Finding.** On `v3.2.0-184-gc547ef14` (B, branch `worktree-cc-stats-planner`) against the opening commit
`v3.2.0-178-g65dc45c5` (A), measured in two full interleaved runs (R1, R2) and seven partial ones (ov2, ov3, the same-plan and memo-miss cells, gain2, gain3, gain4, restart2):

1. **The paths that do not plan did not move.** Point read, tuple-key three-relation join, single-row INSERT and
   single-row UPDATE are inside the noise floor of the run in mean CPU, p50 and p99: B against A pooled over
   36 paired rounds (20 for INSERT and UPDATE) is +0.81 % (point read), +0.43 % (join), -0.10 % (INSERT), +0.50 % (UPDATE), while the control
   server running A's binary sits +0.86 %, +0.82 %, +0.65 %, -0.09 % from A. Gate met.
2. **The S0 flips are realised.** With statistics published, B's server CPU per statement is 12.6-13.6 % lower on
   scenario 1 compare-all at 200 results and 32.9-39.6 % lower at 1,000 to 10,000; 38.2-39.5 % lower on
   scenario 2 customer-statement at 200 cargos and 60.4-61.7 % lower at 1,000. The 15 % gate from the flip is met on
   six of eight cells; 200 results is short (the planned path adds about 25 µs a statement and the statement is
   144 µs), and 200,000 cargos is the cell S0 called indifferent and the planner flips anyway (`switch=0.68`).
3. **One result needs its caveat.** At the CA size (10,000 cargos) the flip saves 29.1 / 31.6 % in the full
   matrix and 44.7-46.7 % on a server that never had other tables created in it; at 200,000 cargos the flipped plan
   costs +10.5 / +7.4 % in the full matrix and -4.6 % to +4.1 % on clean servers. The difference is not B's code:
   the same swapped plan under `join_order = written` on **A's** binary is 30.7 % and 12.7 % dearer on a server on
   which four small tables were created and loaded after mount (section "An engine effect the harness triggered").
   The cause is not diagnosed here.
4. **A memo hit pays for itself; a miss costs 3 µs.** The 3-relation cross-join with a repeated literal is 17.5 %
   (200 rows) and 15.1 % (1,000 rows) cheaper on B because a hit skips CB's order search; a statement whose literal
   no execution repeats costs +2.7 % to +3.0 % (+3 to +4 µs) at 200 and 1,000 rows and +1.1 % at 10,000.
5. **The steward is unmeasurable.** B's idle CPU over 30 s is 0.3 ms above A's in one run and 0.8 ms below it in the
   other, against a control spread of 1.8-10.5 ms; its longest step is 54-69 µs, an upper bound of 0.007 % of core 0.
6. **Steady state is one order.** In a 60 s mixed run the statistics generation moved 0 times (17 → 17 and
   16 → 16), and every statement's `ANALYZE` showed one executed order.

## Stamp

| Item | Value |
|---|---|
| Date and time | 2026-10-18. Build 14:24 UTC; full matrix R1 14:27:28-14:35:16; partial runs 14:35-14:44; full matrix R2 14:44:14-14:52:04; bisection runs to 14:59 UTC |
| Worktree / branch | `/home/cdkbs/ckdbs/.claude/worktrees/cc-stats-planner` / `worktree-cc-stats-planner` |
| B: `git describe --tags` | `v3.2.0-184-gc547ef14` (commit `c547ef14`, 2026-10-18 14:23:41 UTC) |
| A: `git describe --tags` | `v3.2.0-178-g65dc45c5`, the CC-S0 binary built from `git archive` (`bench/v3.2.0/results-cc-s0-premise-v3.2.0-178-g65dc45c5.md`) |
| Tree | The worktree carried uncommitted edits to `CLAUDE.md` and `instructions/v3.2.0/index.md` only (documents, not build inputs); `git status` shows nothing else. The binary was built in this tree's `build-release` |
| Binary B | `build-release/kds_server` built `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, source mtime 2026-10-18 14:24:45 (after the commit time 14:23:41, so it is the engine at HEAD); copied to `/home/cdkbs/bench-runs/cc-close/kds_server_B`, sha256 `3bc1180ec90c3957a1c5d7066c5bd696eea71dd190d9ed858c7cb6da7ad9d3ac`. Every B server started from the copy. The main checkout's `build-release` was not touched |
| Binary A | `/home/cdkbs/bench-runs/cc-close/kds_server_A`, copied from the CC-S0 copy, sha256 `8ce595c65ad1dcdc003443d96896d4c6f6b346fa262bf05b9a1de7c9d1ce495e` (matches the S0 record); source binary mtime 2026-10-18 11:55:47, newer than the commit (11:05:45). A2 is a second server started from the same A copy |
| Build type | Release, no frame pointers |
| Device | `/dev/root`, ext4 (`df -T` checked), data files under `/home/cdkbs/bench-runs/cc-close/run/`. Not tmpfs |
| Host | AMD EPYC 9V74 80-core model string, 8 CPUs visible. Servers pinned to CPU 2 with `taskset`, the client to CPU 3, analysis on CPU 4. Host state per block is in the archive: R1: 643 blocks, max loadavg1 at a block start 2.09, blocks with a build/ctest process visible: 0; R2: 280 blocks, max loadavg1 at a block start 1.95, blocks with a build/ctest process visible: 0 |
| Server config | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`, default `inline_cell_width`, `join_order = planned`; ports 25701-25735 |
| Dataset | The CC-S0 loader's pristine file (`/home/cdkbs/bench-runs/cc-s0/load/pristine.db`: scenario 1's five relations at 200 / 1,000 / 2,872 / 10,000 results and 200 / 1,000 / 10,000 / 60,480 stats rows, scenario 2 driver-loaded at 200 / 1,000 / 10,000 / 200,000 cargos), copied to every server. Every server then created `ov_ex`, `ov_ins`, `ov_upd` (10,000 rows) for the overhead cells |
| Metric | Server CPU per statement: the sum over every server thread of `/proc/<pid>/task/*/schedstat` run time, before and after each block, divided by the block's statements. Throughput is client-observed statements per second of the same block. Latency is client-observed per statement |
| Not executed | The full test suite (the order was measurement only; `-DKDS_BUILD_TESTS=OFF`); sessions on a core other than 0 (`cores = 1`); `durability = strict`; the PostgreSQL floor |

## What was measured

Three servers per run, started from one pristine file, one active at a time: **A** (65dc45c5), **A2** (65dc45c5 again,
the noise floor from inside the run) and **B** (c547ef14). Each cell is a block of a fixed number of statements
(calibrated once to about 1 s on A2, the same on every arm), 10 % of it run first as warm-up, and the arms rotate
(reversed on alternate cells and rounds) over 5 rounds. Before the timed rounds all three servers ran the same
warm-up statements and then 2.5 s idle, so B's steward had published (`generation` 16-17 at the first timed block).
Scenario statements are written as the drivers write them (spelling A of S0's harness, no swapped spelling).
The floor for a delta is the larger of the A2-minus-A difference, half A's round-to-round spread and 2 %.

| Cell | Statement | Size and how it was made |
|---|---|---|
| ov-point-read | `SELECT * FROM daily_bars_x10k WHERE id = N` | 10,000 rows, tuple key |
| ov-join3-tuplekey | `daily_bars_x10k b JOIN symbols_x10k s ON b.symbol_id = s.id JOIN ov_ex e ON s.exchange_id = e.id WHERE b.id = N` (bar → symbol → exchange) | anchored on a tuple key, so the planner's skip applies |
| ov-insert | `INSERT INTO ov_ins (a, b, c) VALUES (...)`, tuple key issued | one row, autocommit |
| ov-update | `UPDATE ov_upd SET a = a + 1 WHERE id = N` | 10,000 rows, tuple key |
| c1-compare-all-r200 / r1k / r2872 / r10k | `tools/scenario1_backtest.py` `compare_all_sql`, `model_results r JOIN models m` | 8 models and 200 / 1,000 / 2,872 (driver default) / 10,000 results |
| c2-customer-statement-n200 / n1k / ca10k / df200k | `tools/scenario2_freight.py` customer statement, `freights f JOIN cargos c ... WHERE c.org_id = X GROUP BY c.org_id` | `--cargos` 200, 1,000, **10,000 (the CA size)**, 200,000 (driver defaults); 180 / 879 / 8,234 / 10,795 freights |
| c3-x200 / x1k / x10k / x60k | `cross_section_sql`, `daily_stats t JOIN daily_bars b JOIN symbols s WHERE t.session_no = N` | 200 / 1,000 / 10,000 / 60,480 stats rows (driver default 30 years x 8 symbols) |

The two partial rows that carry the size sweep (rule 9): scenario 1 and 2 and the cross-join carry four sizes each,
200 to 60,480. The overhead cells run at 10,000 rows only; a point read and a tuple-key join descend a height-2
tree whatever the row count, so they were not swept (cut, stated).

## Result 1: the paths that do not plan did not move

The point read and the tuple-key join compile no searched block (the planner skips a chain of tuple-key descents),
INSERT and UPDATE compile none either, and the compile pins the statistics snapshot only for a block it searches
(`plan-stats.md` PS9), so by design B adds nothing on these statements. The measurement agrees to within the floor. Four
replicates of the same cells (R1 and R2 are the two full runs, ov2 and ov3 are partial runs of the same servers'
recipe with 10 and 16 rounds); the pooled table counts every round of every replicate.

| Cell | Replicate | Rounds | A | A2 | B | B vs A | A2 vs A | B vs A2 |
|---|---|---|---|---|---|---|---|---|
| ov-point-read | R1 (5 rounds) | 5 | 41.65 µs | 41.71 µs | 41.92 µs | +0.6 % | +0.1 % | +0.5 % |
| ov-point-read | R-ov2 (10 rounds) | 10 | 41.37 µs | 42.17 µs | 42.52 µs | +2.8 % | +1.9 % | +0.8 % |
| ov-point-read | R-ov3 (16 rounds) | 16 | 41.94 µs | 42.46 µs | 41.68 µs | -0.6 % | +1.3 % | -1.9 % |
| ov-point-read | R2 (5 rounds) | 5 | 42.10 µs | 41.31 µs | 42.76 µs | +1.5 % | -1.9 % | +3.5 % |
| ov-join3-tuplekey | R1 (5 rounds) | 5 | 58.76 µs | 58.11 µs | 58.67 µs | -0.2 % | -1.1 % | +1.0 % |
| ov-join3-tuplekey | R-ov2 (10 rounds) | 10 | 58.61 µs | 58.68 µs | 59.40 µs | +1.4 % | +0.1 % | +1.2 % |
| ov-join3-tuplekey | R-ov3 (16 rounds) | 16 | 58.45 µs | 59.92 µs | 58.28 µs | -0.3 % | +2.5 % | -2.7 % |
| ov-join3-tuplekey | R2 (5 rounds) | 5 | 59.20 µs | 58.40 µs | 60.09 µs | +1.5 % | -1.4 % | +2.9 % |
| ov-insert | R1 (5 rounds) | 5 | 86.19 µs | 85.75 µs | 84.41 µs | -2.1 % | -0.5 % | -1.6 % |
| ov-insert | R-ov2 (10 rounds) | 10 | 85.14 µs | 85.34 µs | 86.51 µs | +1.6 % | +0.2 % | +1.4 % |
| ov-insert | R2 (5 rounds) | 5 | 85.85 µs | 87.38 µs | 83.90 µs | -2.3 % | +1.8 % | -4.0 % |
| ov-update | R1 (5 rounds) | 5 | 90.40 µs | 90.18 µs | 89.95 µs | -0.5 % | -0.3 % | -0.2 % |
| ov-update | R-ov2 (10 rounds) | 10 | 89.77 µs | 89.82 µs | 90.42 µs | +0.7 % | +0.1 % | +0.7 % |
| ov-update | R2 (5 rounds) | 5 | 90.51 µs | 90.30 µs | 91.47 µs | +1.1 % | -0.2 % | +1.3 % |

Pooled per-round paired deltas (every round of every replicate):

| Cell | Rounds pooled | B vs A: mean / median / min..max | A2 vs A (floor): mean / median / min..max | B vs A2: mean / median |
|---|---|---|---|---|
| ov-point-read | 36 | +0.81 % / +0.72 % / -3.8..+4.4 % | +0.86 % / +1.09 % / -2.8..+5.2 % | -0.03 % / +0.43 % |
| ov-join3-tuplekey | 36 | +0.43 % / +0.06 % / -2.7..+4.9 % | +0.82 % / +0.39 % / -2.4..+5.4 % | -0.34 % / +0.29 % |
| ov-insert | 20 | -0.10 % / +0.18 % / -9.4..+11.4 % | +0.65 % / +0.21 % / -9.9..+18.5 % | -0.53 % / +0.07 % |
| ov-update | 20 | +0.50 % / +0.59 % / -1.6..+2.1 % | -0.09 % / -0.10 % / -1.5..+1.1 % | +0.60 % / +0.62 % |

The point read is the only cell where B sits above A in every replicate but one, by 0.6-2.8 %; the control A2
sits above A by 1.3-1.9 % in the same two replicates (ov2, ov3), and B against A2 pooled is -0.03 % mean.
That is the shape of a process-to-process offset of about 1 %, not a cost; the 16-round replicate has B below A.

R1 (5 rounds), with the latency distributions:

| Cell | Arm | Statements (n) | Server CPU / statement | Throughput | p0 | p25 | p50 | p95 | p99 | Round spread (CPU) |
|---|---|---|---|---|---|---|---|---|---|---|
| ov-point-read | A | 50,000 | 41.65 µs | 14,146 stmt/s | 62.3 µs | 65.6 µs | 66.9 µs | 87.1 µs | 96.3 µs | 1.6 % |
| ov-point-read | A2 | 50,000 | 41.71 µs | 14,250 stmt/s | 62.0 µs | 65.5 µs | 66.7 µs | 86.2 µs | 95.1 µs | 0.8 % |
| ov-point-read | B | 50,000 | 41.92 µs | 14,165 stmt/s | 62.4 µs | 65.8 µs | 67.1 µs | 86.7 µs | 96.0 µs | 2.1 % |
| ov-join3-tuplekey | A | 50,000 | 58.76 µs | 11,154 stmt/s | 79.8 µs | 83.6 µs | 85.3 µs | 106 µs | 117 µs | 1.9 % |
| ov-join3-tuplekey | A2 | 50,000 | 58.11 µs | 11,211 stmt/s | 79.2 µs | 83.1 µs | 84.8 µs | 106 µs | 116 µs | 1.3 % |
| ov-join3-tuplekey | B | 50,000 | 58.67 µs | 11,190 stmt/s | 79.3 µs | 83.5 µs | 85.1 µs | 106 µs | 117 µs | 3.9 % |
| ov-insert | A | 3,990 | 86.19 µs | 780 stmt/s | 964 µs | 1,116 µs | 1,175 µs | 1,476 µs | 3,810 µs | 11.1 % |
| ov-insert | A2 | 3,990 | 85.75 µs | 766 stmt/s | 996 µs | 1,136 µs | 1,186 µs | 1,483 µs | 3,806 µs | 9.5 % |
| ov-insert | B | 3,990 | 84.41 µs | 792 stmt/s | 976 µs | 1,117 µs | 1,174 µs | 1,455 µs | 3,405 µs | 1.1 % |
| ov-update | A | 3,945 | 90.40 µs | 788 stmt/s | 1,005 µs | 1,145 µs | 1,190 µs | 1,406 µs | 2,904 µs | 2.1 % |
| ov-update | A2 | 3,945 | 90.18 µs | 776 stmt/s | 1,001 µs | 1,165 µs | 1,207 µs | 1,417 µs | 3,319 µs | 2.0 % |
| ov-update | B | 3,945 | 89.95 µs | 805 stmt/s | 993 µs | 1,133 µs | 1,178 µs | 1,393 µs | 2,972 µs | 0.9 % |

| Cell | B vs A: CPU | B vs A: throughput | A2 vs A (noise floor): CPU | B vs A2: CPU | B vs A: p50 / p99 | Per-round B vs A CPU | Floor used | Verdict |
|---|---|---|---|---|---|---|---|---|
| ov-point-read | +0.6 % | +0.1 % | +0.1 % | +0.5 % | +0.3 % / -0.3 % | +0.9, +0.6, +0.8, +0.7, +0.3 % | ±2.0 % | within floor |
| ov-join3-tuplekey | -0.2 % | +0.3 % | -1.1 % | +1.0 % | -0.2 % / +0.1 % | +1.9, -1.1, -0.5, -0.9, -0.1 % | ±2.0 % | within floor |
| ov-insert | -2.1 % | +1.5 % | -0.5 % | -1.6 % | -0.1 % / -10.6 % | +0.8, -9.0, -0.4, -0.2, -0.8 % | ±5.5 % | within floor |
| ov-update | -0.5 % | +2.1 % | -0.3 % | -0.2 % | -1.0 % / +2.3 % | -1.6, -0.2, -0.3, -0.6, +0.3 % | ±2.0 % | within floor |

R2 (5 rounds, full matrix repeated from fresh files):

| Cell | B vs A: CPU | B vs A: throughput | A2 vs A (noise floor): CPU | B vs A2: CPU | B vs A: p50 / p99 | Per-round B vs A CPU | Floor used | Verdict |
|---|---|---|---|---|---|---|---|---|
| ov-point-read | +1.5 % | -1.2 % | -1.9 % | +3.5 % | +1.4 % / +1.0 % | +2.1, +1.6, +1.1, +1.6, +1.3 % | ±2.0 % | within floor |
| ov-join3-tuplekey | +1.5 % | -1.0 % | -1.4 % | +2.9 % | +0.2 % / +4.6 % | +4.9, +1.2, +0.5, +0.6, +0.3 % | ±2.0 % | within floor |
| ov-insert | -2.3 % | +2.3 % | +1.8 % | -4.0 % | -0.6 % / -6.7 % | +0.5, -9.4, -0.7, -0.2, -0.8 % | ±5.1 % | within floor |
| ov-update | +1.1 % | +4.9 % | -0.2 % | +1.3 % | -1.4 % / -28.3 % | +0.9, +0.7, +1.5, +0.2, +2.1 % | ±2.0 % | within floor |

ov2 (10 rounds) and ov3 (16 rounds, read cells only):

| Cell | B vs A: CPU | B vs A: throughput | A2 vs A (noise floor): CPU | B vs A2: CPU | B vs A: p50 / p99 | Per-round B vs A CPU | Floor used | Verdict |
|---|---|---|---|---|---|---|---|---|
| ov-point-read | +2.8 % | -2.1 % | +1.9 % | +0.8 % | +2.1 % / +1.5 % | +2.3, +2.6, +0.1, +3.9, +2.3, +2.5, +2.4, +4.4, +4.3, +3.1 % | ±2.0 % | slower |
| ov-join3-tuplekey | +1.4 % | -0.9 % | +0.1 % | +1.2 % | +0.9 % / +0.9 % | +1.6, -0.8, +3.7, -1.1, +2.3, +2.8, +1.2, +1.6, +1.5, +0.9 % | ±2.0 % | within floor |
| ov-insert | +1.6 % | +0.7 % | +0.2 % | +1.4 % | -0.7 % / +0.5 % | +0.7, +10.4, -8.9, +0.5, +11.4, +0.7, +0.5, +0.9, -8.1, +10.0 % | ±5.7 % | within floor |
| ov-update | +0.7 % | +1.3 % | +0.1 % | +0.7 % | -0.4 % / -1.9 % | +0.8, +2.0, +0.8, +1.2, +0.9, +1.0, +0.5, +0.1, +0.2, -0.4 % | ±2.0 % | within floor |

| Cell | B vs A: CPU | B vs A: throughput | A2 vs A (noise floor): CPU | B vs A2: CPU | B vs A: p50 / p99 | Per-round B vs A CPU | Floor used | Verdict |
|---|---|---|---|---|---|---|---|---|
| ov-point-read | -0.6 % | +0.8 % | +1.3 % | -1.9 % | -0.4 % / -0.9 % | -0.0, +1.9, -2.8, -0.4, +2.7, -0.3, -0.3, -3.8, -0.3, -0.4, -3.2, -1.0, +0.1, -0.9, -0.7, -0.4 % | ±2.0 % | within floor |
| ov-join3-tuplekey | -0.3 % | +0.1 % | +2.5 % | -2.7 % | -0.2 % / -0.5 % | -1.4, -0.3, +2.3, +0.1, +0.1, -0.3, -0.8, +2.1, -0.5, -0.7, -0.3, -2.4, +1.4, -0.7, -2.7, -0.6 % | ±2.5 % | within floor |

INSERT and UPDATE p99 deltas in the tables above are group-commit noise (INSERT's p99 is 3.2-3.8 ms against a 1.2 ms
median and moves by up to 10 % between identical servers); the means sit inside the floor in every replicate.

**Where the time goes (rule 3).** The server's CPU is a fraction of what a client sees. Remainders are derived as
p50 minus server CPU on A in R1; this run did not instrument them separately (the commit wait at `group` durability
is not split from the socket round trip here).

| Statement | p50 latency (A) | Server CPU / statement (A) | CPU share of p50 | Remainder of p50 (derived) |
|---|---|---|---|---|
| ov-point-read | 67 µs | 41.7 µs | 62 % | 25 µs |
| ov-join3-tuplekey | 85 µs | 58.8 µs | 69 % | 27 µs |
| ov-insert | 1,175 µs | 86.2 µs | 7 % | 1,089 µs |
| ov-update | 1,190 µs | 90.4 µs | 8 % | 1,099 µs |

For a read, 31-38 % of the p50 is the client and socket round trip, which no engine change can reach. For a write
at `durability = group` 93 % is the commit wait plus the round trip; that is why a sub-5 % CPU change in INSERT
or UPDATE cannot show in latency, and why those two cells are gated on CPU.

## Result 2: the gain, steady state against CB's data-free planner

On all eight scenario cells B's statistics flip the join to the order S0 found cheaper (`reordered=1`); seven of them
pay for it, and the 200,000-cargo cell does not. The
first table is R1's server CPU, throughput and latency distribution; the second is the delta against A and
against the control. R2 repeats it from fresh files.

R1:

| Cell | Arm | Statements (n) | Server CPU / statement | Throughput | p0 | p25 | p50 | p95 | p99 | Round spread (CPU) |
|---|---|---|---|---|---|---|---|---|---|---|
| c1-compare-all-r200 | A | 13,990 | 165.12 µs | 2,813 stmt/s | 334 µs | 346 µs | 354 µs | 374 µs | 385 µs | 0.6 % |
| c1-compare-all-r200 | A2 | 13,990 | 164.99 µs | 2,813 stmt/s | 334 µs | 345 µs | 354 µs | 374 µs | 385 µs | 0.4 % |
| c1-compare-all-r200 | B | 13,990 | 144.25 µs | 2,992 stmt/s | 314 µs | 324 µs | 332 µs | 353 µs | 364 µs | 1.7 % |
| c1-compare-all-r1k | A | 3,275 | 654.02 µs | 677 stmt/s | 1,426 µs | 1,458 µs | 1,471 µs | 1,507 µs | 1,578 µs | 0.6 % |
| c1-compare-all-r1k | A2 | 3,275 | 655.07 µs | 677 stmt/s | 1,438 µs | 1,466 µs | 1,475 µs | 1,505 µs | 1,527 µs | 0.3 % |
| c1-compare-all-r1k | B | 3,275 | 438.57 µs | 796 stmt/s | 1,220 µs | 1,245 µs | 1,255 µs | 1,285 µs | 1,306 µs | 0.8 % |
| c1-compare-all-r2872 | A | 1,210 | 1,759.36 µs | 247 stmt/s | 3,967 µs | 4,006 µs | 4,025 µs | 4,133 µs | 4,162 µs | 0.2 % |
| c1-compare-all-r2872 | A2 | 1,210 | 1,730.39 µs | 249 stmt/s | 3,938 µs | 3,982 µs | 3,997 µs | 4,105 µs | 4,133 µs | 0.6 % |
| c1-compare-all-r2872 | B | 1,210 | 1,133.16 µs | 293 stmt/s | 3,328 µs | 3,378 µs | 3,400 µs | 3,498 µs | 3,522 µs | 0.3 % |
| c1-compare-all-r10k | A | 365 | 5,780.83 µs | 73 stmt/s | 13,388 µs | 13,566 µs | 13,628 µs | 13,984 µs | 14,025 µs | 0.3 % |
| c1-compare-all-r10k | A2 | 365 | 5,798.93 µs | 73 stmt/s | 13,438 µs | 13,601 µs | 13,659 µs | 13,993 µs | 14,104 µs | 0.5 % |
| c1-compare-all-r10k | B | 365 | 3,667.16 µs | 87 stmt/s | 11,258 µs | 11,432 µs | 11,501 µs | 11,652 µs | 11,780 µs | 0.7 % |
| c2-customer-statement-n200 | A | 26,260 | 150.52 µs | 5,421 stmt/s | 161 µs | 175 µs | 179 µs | 206 µs | 216 µs | 0.7 % |
| c2-customer-statement-n200 | A2 | 26,260 | 150.26 µs | 5,441 stmt/s | 162 µs | 174 µs | 178 µs | 205 µs | 216 µs | 0.8 % |
| c2-customer-statement-n200 | B | 26,260 | 93.08 µs | 7,958 stmt/s | 113 µs | 119 µs | 121 µs | 144 µs | 156 µs | 0.3 % |
| c2-customer-statement-n1k | A | 9,075 | 507.33 µs | 1,831 stmt/s | 508 µs | 537 µs | 545 µs | 568 µs | 578 µs | 1.1 % |
| c2-customer-statement-n1k | A2 | 9,075 | 508.16 µs | 1,826 stmt/s | 510 µs | 538 µs | 546 µs | 569 µs | 577 µs | 0.4 % |
| c2-customer-statement-n1k | B | 9,075 | 200.85 µs | 4,240 stmt/s | 217 µs | 225 µs | 230 µs | 260 µs | 269 µs | 0.6 % |
| c2-customer-statement-ca10k | A | 1,165 | 4,306.39 µs | 228 stmt/s | 4,203 µs | 4,264 µs | 4,431 µs | 4,485 µs | 4,529 µs | 2.9 % |
| c2-customer-statement-ca10k | A2 | 1,165 | 4,365.15 µs | 225 stmt/s | 4,245 µs | 4,313 µs | 4,489 µs | 4,547 µs | 4,619 µs | 2.9 % |
| c2-customer-statement-ca10k | B | 1,165 | 3,054.21 µs | 320 stmt/s | 3,060 µs | 3,112 µs | 3,122 µs | 3,153 µs | 3,181 µs | 0.1 % |
| c2-customer-statement-df200k | A | 635 | 7,198.58 µs | 137 stmt/s | 7,188 µs | 7,254 µs | 7,276 µs | 7,491 µs | 7,672 µs | 1.1 % |
| c2-customer-statement-df200k | A2 | 635 | 7,178.10 µs | 137 stmt/s | 7,140 µs | 7,218 µs | 7,253 µs | 7,509 µs | 7,813 µs | 1.7 % |
| c2-customer-statement-df200k | B | 635 | 7,956.24 µs | 124 stmt/s | 7,944 µs | 8,006 µs | 8,038 µs | 8,298 µs | 8,598 µs | 0.9 % |

| Cell | B vs A: CPU | B vs A: throughput | A2 vs A (noise floor): CPU | B vs A2: CPU | B vs A: p50 / p99 | Per-round B vs A CPU | Floor used | Verdict |
|---|---|---|---|---|---|---|---|---|
| c1-compare-all-r200 | -12.6 % | +6.4 % | -0.1 % | -12.6 % | -6.2 % / -5.4 % | -13.4, -11.8, -12.7, -12.0, -13.2 % | ±2.0 % | faster |
| c1-compare-all-r1k | -32.9 % | +17.5 % | +0.2 % | -33.1 % | -14.7 % / -17.2 % | -33.2, -32.6, -33.2, -33.0, -32.7 % | ±2.0 % | faster |
| c1-compare-all-r2872 | -35.6 % | +18.4 % | -1.6 % | -34.5 % | -15.5 % / -15.4 % | -35.6, -35.5, -35.6, -35.6, -35.7 % | ±2.0 % | faster |
| c1-compare-all-r10k | -36.6 % | +19.1 % | +0.3 % | -36.8 % | -15.6 % / -16.0 % | -36.3, -36.9, -36.3, -36.7, -36.6 % | ±2.0 % | faster |
| c2-customer-statement-n200 | -38.2 % | +46.8 % | -0.2 % | -38.1 % | -32.3 % / -28.0 % | -38.0, -38.1, -38.2, -38.1, -38.4 % | ±2.0 % | faster |
| c2-customer-statement-n1k | -60.4 % | +131.5 % | +0.2 % | -60.5 % | -57.7 % / -53.4 % | -60.7, -60.2, -60.6, -60.2, -60.4 % | ±2.0 % | faster |
| c2-customer-statement-ca10k | -29.1 % | +40.4 % | +1.4 % | -30.0 % | -29.5 % / -29.8 % | -27.8, -28.5, -29.4, -29.6, -29.9 % | ±2.0 % | faster |
| c2-customer-statement-df200k | +10.5 % | -9.6 % | -0.3 % | +10.8 % | +10.5 % / +12.1 % | +10.5, +11.4, +10.2, +10.5, +10.1 % | ±2.0 % | slower |

R2:

| Cell | B vs A: CPU | B vs A: throughput | A2 vs A (noise floor): CPU | B vs A2: CPU | B vs A: p50 / p99 | Per-round B vs A CPU | Floor used | Verdict |
|---|---|---|---|---|---|---|---|---|
| c1-compare-all-r200 | -13.6 % | +6.4 % | -1.0 % | -12.7 % | -6.0 % / -5.7 % | -13.5, -13.3, -14.5, -13.0, -13.7 % | ±2.0 % | faster |
| c1-compare-all-r1k | -36.3 % | +19.7 % | -4.5 % | -33.3 % | -16.9 % / -5.7 % | -35.2, -36.9, -35.4, -37.3, -36.7 % | ±4.5 % | faster |
| c1-compare-all-r2872 | -39.2 % | +19.7 % | -5.8 % | -35.4 % | -17.4 % / -18.1 % | -38.9, -39.2, -38.9, -39.4, -39.4 % | ±5.8 % | faster |
| c1-compare-all-r10k | -39.6 % | +21.5 % | -5.0 % | -36.4 % | -18.3 % / -17.0 % | -38.9, -38.9, -39.0, -40.6, -40.5 % | ±5.0 % | faster |
| c2-customer-statement-n200 | -39.5 % | +49.5 % | -2.8 % | -37.8 % | -33.5 % / -29.4 % | -39.2, -39.5, -39.6, -39.7, -39.6 % | ±2.8 % | faster |
| c2-customer-statement-n1k | -61.7 % | +139.3 % | -3.2 % | -60.4 % | -59.1 % / -54.7 % | -61.9, -61.6, -61.5, -61.8, -61.7 % | ±3.2 % | faster |
| c2-customer-statement-ca10k | -31.6 % | +45.4 % | -3.0 % | -29.4 % | -31.9 % / -32.7 % | -30.5, -31.2, -31.9, -31.9, -32.4 % | ±3.0 % | faster |
| c2-customer-statement-df200k | +7.4 % | -6.9 % | -2.9 % | +10.6 % | +7.5 % / +6.6 % | +7.9, +7.5, +6.7, +7.3, +7.5 % | ±2.9 % | slower |

Read against CC-S0 (`v3.2.0-178-g65dc45c5`, the forced alternative on W:B versus P:A):

| Cell | S0 forced alternative (minimum over 3 pairs) | B realised, R1 / R2 (clean-server figure in brackets) | 15 % gate |
|---|---|---|---|
| compare-all 200 results | -21.6 % | -12.6 % / -13.6 % | not met (planned path costs about 25 µs per statement on a 144 µs statement) |
| compare-all 1,000 | -34.5 % | -32.9 % / -36.3 % | met |
| compare-all 2,872 (default) | -38.1 % | -35.6 % / -39.2 % | met |
| compare-all 10,000 | -37.3 % | -36.6 % / -39.6 % | met |
| customer-statement 200 cargos | -41.7 % | -38.2 % / -39.5 % | met |
| customer-statement 1,000 | -60.7 % | -60.4 % / -61.7 % | met |
| customer-statement 10,000 (CA size) | -32.2 % (typical -46 %) | -29.1 % / -31.6 % (-44.7 % to -46.7 %) | met |
| customer-statement 200,000 (defaults) | -0.7 % (S0: indifferent) | +10.5 % / +7.4 % (-4.6 % to +4.1 %) | not met, and not expected to be |

The plans B chooses at steady state, from `ANALYZE` (the full lines are in the archive's `plans.json`):

| Statement | Server A (CB data-free) | Server B (statistics, steady state) |
|---|---|---|
| c1-compare-all-r200 | Scan model_results → Probe models; examined 400, pages 202, opens 201 | Scan models → Scan model_results +build; examined 383, pages 17, opens 9; `planned order=planned gen=16 switch=0.194685` |
| c1-compare-all-r1k | Scan model_results → Probe models; examined 2000, pages 1010, opens 1001 | Scan models → Scan model_results +build; examined 1883, pages 75, opens 9; `planned order=planned gen=16 switch=0.19122` |
| c1-compare-all-r2872 | Scan model_results → Probe models; examined 5744, pages 2898, opens 2873 | Scan models → Scan model_results +build; examined 5393, pages 209, opens 9; `planned order=planned gen=16 switch=0.190739` |
| c1-compare-all-r10k | Scan model_results → Probe models; examined 20000, pages 10091, opens 10001 | Scan models → Scan model_results +build; examined 18758, pages 729, opens 9; `planned order=planned gen=16 switch=0.190542` |
| c2-customer-statement-n200 | Scan freights → Probe cargos; examined 360, pages 182, opens 181 | FilterScan cargos → Scan freights +build; examined 382, pages 6, opens 5; `planned order=planned gen=16 switch=0.183714` |
| c2-customer-statement-n1k | Scan freights → Probe cargos; examined 1758, pages 888, opens 880 | FilterScan cargos → Scan freights +build; examined 1893, pages 33, opens 16; `planned order=planned gen=16 switch=0.180121` |
| c2-customer-statement-ca10k | Scan freights → Probe cargos; examined 16468, pages 8313, opens 8235 | FilterScan cargos → Scan freights +build; examined 18264, pages 209, opens 46; `planned order=planned gen=16 switch=0.190848` |
| c2-customer-statement-df200k | Scan freights → Probe cargos; examined 21590, pages 10898, opens 10796 | FilterScan cargos → Scan freights +build; examined 210803, pages 2111, opens 103; `planned order=planned gen=16 switch=0.68201` |
| c3-same-x200 | FilterScan daily_stats → Probe daily_bars → Probe symbols; examined 216, pages 19, opens 17 | FilterScan daily_stats → Probe daily_bars → Probe symbols; examined 216, pages 19, opens 17; `planned order=written gen=16 switch=1` |
| c3-same-x1k | FilterScan daily_stats → Probe daily_bars → Probe symbols; examined 1016, pages 31, opens 17 | FilterScan daily_stats → Probe daily_bars → Probe symbols; examined 1016, pages 31, opens 17; `planned order=written gen=16 switch=1` |
| c3-same-x10k | FilterScan daily_stats → Probe daily_bars → Probe symbols; examined 10016, pages 161, opens 17 | FilterScan daily_stats → Probe daily_bars → Probe symbols; examined 10016, pages 161, opens 17; `planned order=written gen=16 switch=1` |
| c3-same-x60k | FilterScan daily_stats → Probe daily_bars → Probe symbols; examined 60496, pages 893, opens 17 | FilterScan daily_stats → Probe daily_bars → Probe symbols; examined 60496, pages 893, opens 17; `planned order=written gen=16 switch=1` |

**The planned path has a fixed cost.** The 200-result compare-all is 144 µs on B where S0's forced alternative was
119 µs (a cross-run comparison). On identical plans in one run (table below, `BP:drv` against `WB:swap`) B planned
costs +10.0 % (+19 µs) at n1k, +0.1 % at 10,000 results, +1.1 % at ca10k and +1.3 % at df200k over the same order
forced with `join_order = written`. A memo hit compiles the written order and then the remembered one, two
compiles where the forced order pays one (`plan-stats.md` §6a); that is the 19-25 µs, and it is why the gain is
short of the forced figure by 8-9 points at the smallest statement and by nothing from 10,000 results.

**First executions (cold) are data-free and cost what A costs.** Each cell's first 20 executions on a fresh server,
before the steward had seen the relation; A and B ran them in the same order. R1:

| Cell | Server | 1st execution: CPU | 1st execution: latency | 2nd-20th: mean CPU | 2nd-20th: median latency |
|---|---|---|---|---|---|
| c1-compare-all-r200 | A | 384 µs | 579 µs | 195.5 µs | 387 µs |
| c1-compare-all-r200 | B | 368 µs | 588 µs | 188.8 µs | 383 µs |
| c1-compare-all-r1k | A | 984 µs | 1,798 µs | 680.8 µs | 1,503 µs |
| c1-compare-all-r1k | B | 928 µs | 1,765 µs | 682.4 µs | 1,506 µs |
| c1-compare-all-r2872 | A | 2,396 µs | 4,694 µs | 1,925.9 µs | 4,198 µs |
| c1-compare-all-r2872 | B | 2,488 µs | 4,856 µs | 1,923.6 µs | 4,179 µs |
| c1-compare-all-r10k | A | 8,595 µs | 16,418 µs | 6,045.5 µs | 13,778 µs |
| c1-compare-all-r10k | B | 8,396 µs | 16,283 µs | 5,958.4 µs | 13,751 µs |
| c2-customer-statement-n200 | A | 349 µs | 399 µs | 169.1 µs | 210 µs |
| c2-customer-statement-n200 | B | 402 µs | 447 µs | 170.7 µs | 208 µs |
| c2-customer-statement-n1k | A | 746 µs | 793 µs | 506.9 µs | 546 µs |
| c2-customer-statement-n1k | B | 710 µs | 756 µs | 497.6 µs | 544 µs |
| c2-customer-statement-ca10k | A | 5,571 µs | 5,674 µs | 4,189.5 µs | 4,264 µs |
| c2-customer-statement-ca10k | B | 5,593 µs | 5,695 µs | 4,119.3 µs | 4,197 µs |
| c2-customer-statement-df200k | A | 22,953 µs | 23,191 µs | 7,181.6 µs | 7,269 µs |
| c2-customer-statement-df200k | B | 24,717 µs | 24,991 µs | 7,130.3 µs | 7,183 µs |

In R1 B's executions 2-20 are within -3.4 % to +1.0 % of A's mean CPU in every cell; the single first executions
differ by -5.6 % to +15 % either way (the first statement pays the page faults, 5.6-24.7 ms at 10,000-200,000
cargos). In R2 (below) the same holds within 7 % except compare-all at 2,872 results, where B's mean over
executions 2-20 is 25 % below A's: the steward's first publish landed during that cell's 20 executions and the flip
began there. The flip arrives with the first publish: one second after the
first execution (restart table, below).

R2's cold table:

| Cell | Server | 1st execution: CPU | 1st execution: latency | 2nd-20th: mean CPU | 2nd-20th: median latency |
|---|---|---|---|---|---|
| c1-compare-all-r200 | A | 357 µs | 568 µs | 190.2 µs | 381 µs |
| c1-compare-all-r200 | B | 390 µs | 590 µs | 190.6 µs | 378 µs |
| c1-compare-all-r1k | A | 1,067 µs | 1,904 µs | 706.7 µs | 1,552 µs |
| c1-compare-all-r1k | B | 964 µs | 1,781 µs | 660.1 µs | 1,501 µs |
| c1-compare-all-r2872 | A | 2,399 µs | 4,708 µs | 1,835.6 µs | 4,136 µs |
| c1-compare-all-r2872 | B | 2,484 µs | 4,868 µs | 1,379.6 µs | 3,440 µs |
| c1-compare-all-r10k | A | 8,716 µs | 16,604 µs | 6,192.1 µs | 14,070 µs |
| c1-compare-all-r10k | B | 8,305 µs | 16,155 µs | 5,951.5 µs | 13,789 µs |
| c2-customer-statement-n200 | A | 354 µs | 417 µs | 172.6 µs | 206 µs |
| c2-customer-statement-n200 | B | 391 µs | 436 µs | 172.1 µs | 209 µs |
| c2-customer-statement-n1k | A | 770 µs | 823 µs | 520.8 µs | 566 µs |
| c2-customer-statement-n1k | B | 702 µs | 757 µs | 497.9 µs | 542 µs |
| c2-customer-statement-ca10k | A | 5,720 µs | 5,815 µs | 4,343.2 µs | 4,404 µs |
| c2-customer-statement-ca10k | B | 5,639 µs | 5,751 µs | 4,141.0 µs | 4,200 µs |
| c2-customer-statement-df200k | A | 23,646 µs | 25,137 µs | 7,439.9 µs | 7,458 µs |
| c2-customer-statement-df200k | B | 24,581 µs | 24,870 µs | 7,172.8 µs | 7,196 µs |

### An engine effect the harness triggered

In both full runs the CA-size and default-size cells came out well short of the CC-S0 figure on B. A fresh B
re-run with the same warm-up history (`gain2`: servers A, A2, B as in the main run but without the overhead
tables, and `BL` warmed on the three cells only; 5 rounds) gave:

| Cell | A2 vs A | B vs A | BL vs A |
|---|---|---|---|
| c2-customer-statement-ca10k | -0.0 % | -46.7 % | -43.4 % |
| c2-customer-statement-df200k | -0.2 % | -4.6 % | -5.1 % |
| c2-customer-statement-n1k | +0.3 % | -60.0 % | -59.9 % |

so the figure depended on the server's history. The bisection below shows what it depended on. `B0` had the gain warm-up only; `B1` had `ov_ex`,
`ov_ins`, `ov_upd` created and `ov_upd` loaded (10,000 rows) before the warm-up; `B2` and `B3` add the overhead
statements and the cold executions. A has the same tables in this run.

| Cell | Arm | Statements (n) | Server CPU / statement | Throughput | p0 | p25 | p50 | p95 | p99 | Round spread (CPU) |
|---|---|---|---|---|---|---|---|---|---|---|
| c2-customer-statement-ca10k | A | 1,165 | 4,379.74 µs | 223 stmt/s | 4,270 µs | 4,434 µs | 4,518 µs | 4,607 µs | 4,749 µs | 2.5 % |
| c2-customer-statement-ca10k | B0 | 1,165 | 2,355.74 µs | 410 stmt/s | 2,338 µs | 2,397 µs | 2,424 µs | 2,512 µs | 2,563 µs | 3.4 % |
| c2-customer-statement-ca10k | B1 | 1,165 | 3,083.50 µs | 314 stmt/s | 3,111 µs | 3,161 µs | 3,176 µs | 3,225 µs | 3,288 µs | 1.4 % |
| c2-customer-statement-ca10k | B2 | 1,165 | 3,071.53 µs | 316 stmt/s | 3,063 µs | 3,152 µs | 3,163 µs | 3,211 µs | 3,258 µs | 0.8 % |
| c2-customer-statement-ca10k | B3 | 1,165 | 3,075.70 µs | 315 stmt/s | 3,066 µs | 3,153 µs | 3,167 µs | 3,221 µs | 3,351 µs | 0.8 % |
| c2-customer-statement-df200k | A | 635 | 7,242.83 µs | 135 stmt/s | 7,256 µs | 7,345 µs | 7,384 µs | 7,617 µs | 7,898 µs | 0.8 % |
| c2-customer-statement-df200k | B0 | 635 | 7,537.46 µs | 130 stmt/s | 7,310 µs | 7,483 µs | 7,776 µs | 8,062 µs | 8,322 µs | 1.9 % |
| c2-customer-statement-df200k | B1 | 635 | 8,119.03 µs | 120 stmt/s | 8,066 µs | 8,235 µs | 8,294 µs | 8,533 µs | 8,792 µs | 2.4 % |
| c2-customer-statement-df200k | B2 | 635 | 8,026.11 µs | 122 stmt/s | 8,062 µs | 8,146 µs | 8,193 µs | 8,396 µs | 8,668 µs | 0.9 % |
| c2-customer-statement-df200k | B3 | 635 | 7,993.43 µs | 122 stmt/s | 7,956 µs | 8,103 µs | 8,155 µs | 8,382 µs | 8,867 µs | 0.5 % |

| Cell | B0 vs A | B1 vs A | B2 vs A | B3 vs A |
|---|---|---|---|---|
| c2-customer-statement-ca10k | -46.2 % | -29.6 % | -29.9 % | -29.8 % |
| c2-customer-statement-df200k | +4.1 % | +12.1 % | +10.8 % | +10.4 % |

The overhead statements and the cold executions add nothing (`B1` = `B2` = `B3`); creating and loading the tables is
enough, and it is not B's code: under `join_order = written` the swapped order (the plan B picks) on **A's binary**
shows the same step, and so does B's binary:

| Cell | Arm | Statements (n) | Server CPU / statement | Throughput | p0 | p25 | p50 | p95 | p99 | Round spread (CPU) |
|---|---|---|---|---|---|---|---|---|---|---|
| c2-swapped-ca10k | WA0 | 1,165 | 2,320.80 µs | 418 stmt/s | 2,336 µs | 2,378 µs | 2,385 µs | 2,419 µs | 2,459 µs | 0.1 % |
| c2-swapped-ca10k | WA1 | 1,165 | 3,032.65 µs | 321 stmt/s | 3,052 µs | 3,103 µs | 3,114 µs | 3,146 µs | 3,178 µs | 0.6 % |
| c2-swapped-ca10k | WB0 | 1,165 | 2,345.79 µs | 414 stmt/s | 2,351 µs | 2,400 µs | 2,408 µs | 2,446 µs | 2,494 µs | 0.3 % |
| c2-swapped-ca10k | WB1 | 1,165 | 3,049.34 µs | 319 stmt/s | 3,064 µs | 3,119 µs | 3,131 µs | 3,167 µs | 3,222 µs | 0.5 % |
| c2-swapped-df200k | WA0 | 635 | 6,935.39 µs | 141 stmt/s | 6,966 µs | 7,020 µs | 7,041 µs | 7,226 µs | 7,449 µs | 0.5 % |
| c2-swapped-df200k | WA1 | 635 | 7,815.49 µs | 126 stmt/s | 7,833 µs | 7,900 µs | 7,932 µs | 8,150 µs | 8,403 µs | 0.7 % |
| c2-swapped-df200k | WB0 | 635 | 6,992.47 µs | 140 stmt/s | 7,010 µs | 7,078 µs | 7,100 µs | 7,273 µs | 7,505 µs | 0.7 % |
| c2-swapped-df200k | WB1 | 635 | 7,868.54 µs | 125 stmt/s | 7,889 µs | 7,950 µs | 7,991 µs | 8,203 µs | 8,329 µs | 0.9 % |

| Cell | WA1 vs WA0 | WB0 vs WA0 | WB1 vs WA0 |
|---|---|---|---|
| c2-swapped-ca10k | +30.7 % | +1.1 % | +31.4 % |
| c2-swapped-df200k | +12.7 % | +0.8 % | +13.5 % |

`WA1` and `WB1` hold the four small tables created and loaded after mount; `WA0` and `WB0` do not. The slow plan is
`FilterScan cargos`, then `Scan freights` with a statement-local build; A's own driver-order plan (Probes) is
not slowed (A's ca10k mean is 4,306-4,380 µs with and without the tables). I did not find the cause; it is
independent of the CC statistics (`65dc45c5` shows it) and of the overhead statements, and it needs a diagnosis
(page placement of the pages allocated after mount, and the pool, are the first suspects, both unproven). For the
gain figures the clean-server numbers are the ones comparable with CC-S0, whose servers never had other tables
created; the full-matrix R1 and R2 figures are what the harness measured and are kept in the table above.

Same-run checks on clean servers (the extra cells: both binaries under `join_order = written`, the driver's
order and the swapped order, beside B planned; `BP` had no extra tables):

| Cell | Arm | Statements (n) | Server CPU / statement | Throughput | p0 | p25 | p50 | p95 | p99 | Round spread (CPU) |
|---|---|---|---|---|---|---|---|---|---|---|
| c1-compare-all-r10k | WA:drv | 219 | 5,871.27 µs | 72 stmt/s | 13,649 µs | 13,811 µs | 13,933 µs | 14,298 µs | 14,391 µs | 1.7 % |
| c1-compare-all-r10k | WB:drv | 219 | 5,833.32 µs | 72 stmt/s | 13,643 µs | 13,782 µs | 13,856 µs | 14,062 µs | 14,288 µs | 1.7 % |
| c1-compare-all-r10k | WA:swap | 219 | 3,614.25 µs | 85 stmt/s | 11,423 µs | 11,586 µs | 11,729 µs | 12,139 µs | 15,390 µs | 0.7 % |
| c1-compare-all-r10k | WB:swap | 219 | 3,676.13 µs | 86 stmt/s | 11,483 µs | 11,612 µs | 11,664 µs | 11,847 µs | 11,966 µs | 0.4 % |
| c1-compare-all-r10k | BP:drv | 219 | 3,679.83 µs | 86 stmt/s | 11,441 µs | 11,616 µs | 11,674 µs | 11,866 µs | 11,985 µs | 0.5 % |
| c2-customer-statement-n1k | WA:drv | 5,445 | 503.29 µs | 1,830 stmt/s | 500 µs | 534 µs | 546 µs | 575 µs | 585 µs | 1.7 % |
| c2-customer-statement-n1k | WB:drv | 5,445 | 485.92 µs | 1,887 stmt/s | 482 µs | 512 µs | 529 µs | 559 µs | 567 µs | 0.1 % |
| c2-customer-statement-n1k | WA:swap | 5,445 | 188.73 µs | 4,436 stmt/s | 201 µs | 212 µs | 225 µs | 250 µs | 261 µs | 2.4 % |
| c2-customer-statement-n1k | WB:swap | 5,445 | 189.21 µs | 4,428 stmt/s | 202 µs | 213 µs | 226 µs | 249 µs | 260 µs | 0.9 % |
| c2-customer-statement-n1k | BP:drv | 5,445 | 208.14 µs | 4,066 stmt/s | 222 µs | 232 µs | 245 µs | 274 µs | 286 µs | 0.6 % |
| c2-customer-statement-ca10k | WA:drv | 699 | 4,305.73 µs | 226 stmt/s | 4,243 µs | 4,310 µs | 4,456 µs | 4,546 µs | 4,622 µs | 1.6 % |
| c2-customer-statement-ca10k | WB:drv | 699 | 4,210.52 µs | 232 stmt/s | 4,148 µs | 4,233 µs | 4,347 µs | 4,416 µs | 4,466 µs | 1.2 % |
| c2-customer-statement-ca10k | WA:swap | 699 | 2,261.51 µs | 428 stmt/s | 2,184 µs | 2,235 µs | 2,379 µs | 2,424 µs | 2,455 µs | 7.2 % |
| c2-customer-statement-ca10k | WB:swap | 699 | 2,354.26 µs | 411 stmt/s | 2,343 µs | 2,400 µs | 2,420 µs | 2,506 µs | 2,540 µs | 3.2 % |
| c2-customer-statement-ca10k | BP:drv | 699 | 2,380.37 µs | 406 stmt/s | 2,380 µs | 2,445 µs | 2,456 µs | 2,495 µs | 2,539 µs | 0.3 % |
| c2-customer-statement-df200k | WA:drv | 381 | 7,319.26 µs | 133 stmt/s | 7,283 µs | 7,395 µs | 7,437 µs | 7,795 µs | 8,360 µs | 0.9 % |
| c2-customer-statement-df200k | WB:drv | 381 | 7,204.81 µs | 136 stmt/s | 7,152 µs | 7,262 µs | 7,311 µs | 7,719 µs | 8,775 µs | 1.1 % |
| c2-customer-statement-df200k | WA:swap | 381 | 7,037.33 µs | 139 stmt/s | 6,941 µs | 7,135 µs | 7,204 µs | 7,405 µs | 7,604 µs | 2.6 % |
| c2-customer-statement-df200k | WB:swap | 381 | 7,056.22 µs | 138 stmt/s | 6,963 µs | 7,123 µs | 7,206 µs | 7,508 µs | 7,748 µs | 2.5 % |
| c2-customer-statement-df200k | BP:drv | 381 | 7,148.51 µs | 137 stmt/s | 7,157 µs | 7,253 µs | 7,286 µs | 7,518 µs | 7,932 µs | 0.4 % |

| Cell | WB vs WA, driver order | WB vs WA, swapped order | Swapped vs driver order, A binary | BP (B planned) vs WB:swap = planning cost | BP vs WA:drv = realised flip |
|---|---|---|---|---|---|
| c1-compare-all-r10k | -0.6 % | +1.7 % | -38.4 % | +0.1 % (+3.7 µs) | -37.3 % |
| c2-customer-statement-n1k | -3.5 % | +0.3 % | -62.5 % | +10.0 % (+18.9 µs) | -58.6 % |
| c2-customer-statement-ca10k | -2.2 % | +4.1 % | -47.5 % | +1.1 % (+26.1 µs) | -44.7 % |
| c2-customer-statement-df200k | -1.6 % | +0.3 % | -3.9 % | +1.3 % (+92.3 µs) | -2.3 % |

`WB:drv` against `WA:drv` and `WB:swap` against `WA:swap` are within -3.5 % to +4.1 % (the larger entries are the
7.2 % round spread of `WA:swap` at ca10k), so the two binaries run the same plan at the same cost; the realised
flip on a clean server is -37.3 % (10,000 results), -58.6 % (n1k), -44.7 % (ca10k) and -2.3 % (df200k).

The customer-statement at 200,000 cargos is the crossover S0 predicted: the planner's estimate (`est=1:91,0:6`,
switch 0.68) says the cargo-first order is 32 % cheaper, and the measured saving is between -4.6 % and +4.1 % on
clean servers. The prior for `cargos_df` is 131,072 against 200,000 (0.66x); the estimate of rows the filter
leaves (91 against the true 100 per organization) is fine, and the plan the model prices at 0.68 of the written order costs 0.95 to 1.04 of it. Where the model's error sits
(the price of a rejected row on the `FilterScan`, or the build) is not established here, but the inputs it was
given were close, so it is a cost-constant question and not a statistics one.

## Result 3: the order memo

The cross-join's plan does not change with statistics (`order=written`, `est=0:8,1:8,2:8` on B; the same
steps as A), so every B-over-A difference on this statement is the cost of choosing the order. With a repeated
literal B serves the order from the memo and compiles once; A searches every time.

R1:

| Cell | Arm | Statements (n) | Server CPU / statement | Throughput | p0 | p25 | p50 | p95 | p99 | Round spread (CPU) |
|---|---|---|---|---|---|---|---|---|---|---|
| c3-x200 | A-same | 34,790 | 101.83 µs | 6,898 stmt/s | 132 µs | 137 µs | 140 µs | 164 µs | 178 µs | 1.2 % |
| c3-x200 | A2-same | 34,790 | 100.40 µs | 6,976 stmt/s | 132 µs | 135 µs | 138 µs | 163 µs | 175 µs | 0.4 % |
| c3-x200 | B-same | 34,790 | 84.04 µs | 7,889 stmt/s | 116 µs | 120 µs | 122 µs | 146 µs | 158 µs | 1.2 % |
| c3-x200 | A-vary | 34,790 | 102.40 µs | 6,862 stmt/s | 133 µs | 138 µs | 141 µs | 164 µs | 176 µs | 2.1 % |
| c3-x200 | B-vary | 34,790 | 84.58 µs | 7,850 stmt/s | 117 µs | 120 µs | 123 µs | 145 µs | 157 µs | 1.0 % |
| c3-x1k | A-same | 29,870 | 124.41 µs | 5,936 stmt/s | 155 µs | 160 µs | 163 µs | 189 µs | 203 µs | 1.4 % |
| c3-x1k | A2-same | 29,870 | 123.37 µs | 5,991 stmt/s | 154 µs | 158 µs | 162 µs | 186 µs | 198 µs | 0.1 % |
| c3-x1k | B-same | 29,870 | 105.58 µs | 6,708 stmt/s | 138 µs | 142 µs | 145 µs | 167 µs | 178 µs | 0.9 % |
| c3-x1k | A-vary | 29,870 | 125.15 µs | 5,916 stmt/s | 155 µs | 160 µs | 164 µs | 189 µs | 202 µs | 1.9 % |
| c3-x1k | B-vary | 29,870 | 107.13 µs | 6,625 stmt/s | 139 µs | 144 µs | 147 µs | 170 µs | 183 µs | 1.3 % |
| c3-x10k | A-same | 12,255 | 362.59 µs | 2,423 stmt/s | 383 µs | 401 µs | 412 µs | 434 µs | 444 µs | 1.6 % |
| c3-x10k | A2-same | 12,255 | 356.24 µs | 2,465 stmt/s | 379 µs | 394 µs | 405 µs | 428 µs | 437 µs | 0.2 % |
| c3-x10k | B-same | 12,255 | 345.18 µs | 2,534 stmt/s | 370 µs | 380 µs | 395 µs | 418 µs | 426 µs | 0.3 % |
| c3-x10k | A-vary | 12,255 | 365.70 µs | 2,403 stmt/s | 386 µs | 403 µs | 415 µs | 440 µs | 451 µs | 0.5 % |
| c3-x10k | B-vary | 12,255 | 360.51 µs | 2,435 stmt/s | 371 µs | 398 µs | 408 µs | 442 µs | 454 µs | 1.4 % |
| c3-x60k | A-same | 2,875 | 1,634.72 µs | 587 stmt/s | 1,665 µs | 1,690 µs | 1,700 µs | 1,723 µs | 1,758 µs | 0.4 % |
| c3-x60k | A2-same | 2,875 | 1,630.38 µs | 588 stmt/s | 1,659 µs | 1,685 µs | 1,695 µs | 1,718 µs | 1,828 µs | 0.3 % |
| c3-x60k | B-same | 2,875 | 1,633.27 µs | 588 stmt/s | 1,649 µs | 1,686 µs | 1,697 µs | 1,728 µs | 1,771 µs | 0.8 % |
| c3-x60k | A-vary | 2,875 | 1,645.16 µs | 584 stmt/s | 1,666 µs | 1,698 µs | 1,709 µs | 1,743 µs | 1,780 µs | 0.8 % |
| c3-x60k | B-vary | 2,875 | 1,668.34 µs | 576 stmt/s | 1,682 µs | 1,723 µs | 1,734 µs | 1,765 µs | 1,800 µs | 0.5 % |

| Cell | B-same vs A-same: CPU | B-same vs A-same: throughput | A2-same vs A-same (noise floor): CPU | B-same vs A2-same: CPU | B-same vs A-same: p50 / p99 | Per-round B-same vs A-same CPU | Floor used | Verdict |
|---|---|---|---|---|---|---|---|---|
| c3-x200 | -17.5 % | +14.4 % | -1.4 % | -16.3 % | -13.0 % / -11.1 % | -16.9, -17.8, -17.6, -16.9, -18.1 % | ±2.0 % | faster |
| c3-x1k | -15.1 % | +13.0 % | -0.8 % | -14.4 % | -11.1 % / -12.4 % | -14.5, -15.6, -15.1, -14.3, -16.1 % | ±2.0 % | faster |
| c3-x10k | -4.8 % | +4.6 % | -1.8 % | -3.1 % | -4.1 % / -4.0 % | -4.0, -4.8, -4.5, -5.1, -5.6 % | ±2.0 % | faster |
| c3-x60k | -0.1 % | +0.1 % | -0.3 % | +0.2 % | -0.1 % / +0.7 % | -0.5, +0.1, -0.1, -0.4, +0.5 % | ±2.0 % | within floor |

R2:

| Cell | B-same vs A-same: CPU | B-same vs A-same: throughput | A2-same vs A-same (noise floor): CPU | B-same vs A2-same: CPU | B-same vs A-same: p50 / p99 | Per-round B-same vs A-same CPU | Floor used | Verdict |
|---|---|---|---|---|---|---|---|---|
| c3-x200 | -17.4 % | +14.4 % | -1.4 % | -16.2 % | -13.1 % / -10.1 % | -16.7, -18.2, -17.1, -17.0, -18.1 % | ±2.0 % | faster |
| c3-x1k | -14.8 % | +12.5 % | -1.0 % | -13.9 % | -10.7 % / -11.6 % | -13.9, -15.1, -14.8, -13.9, -16.2 % | ±2.0 % | faster |
| c3-x10k | -5.4 % | +5.1 % | -2.6 % | -2.9 % | -4.6 % / -5.2 % | -4.6, -4.6, -4.8, -6.3, -6.6 % | ±2.6 % | faster |
| c3-x60k | -1.8 % | +1.8 % | -2.0 % | +0.1 % | -1.8 % / -2.9 % | -0.6, -0.7, -2.3, -2.3, -3.1 % | ±2.0 % | within floor |

"Vary" draws `session_no` over the data (25, 125, 1,250, 7,560 distinct values), so at 200 and 1,000 rows the
literals repeat inside the 1,024-entry memo and mostly hit: the table below is not a miss measurement there.

| Cell | A-vary | B-vary | B-vary vs A-vary: CPU | B-vary vs A-vary: throughput |
|---|---|---|---|---|
| c3-x200 | 102.40 µs | 84.58 µs | -17.4 % | +14.4 % |
| c3-x1k | 125.15 µs | 107.13 µs | -14.4 % | +12.0 % |
| c3-x10k | 365.70 µs | 360.51 µs | -1.4 % | +1.3 % |
| c3-x60k | 1645.16 µs | 1668.34 µs | +1.4 % | -1.4 % |

**The miss cost** is the separate cell: an always-true conjunct (`AND t.ret_bp > -(1000 + k)`, `k` unique for
every execution) makes every execution miss the memo. Three planned servers, `PA`/`PA2` on A's binary and `BP` on
B's, 3 rounds each, all warmed with the same statements:

| Cell | Arm | Statements (n) | Server CPU / statement | Throughput | p0 | p25 | p50 | p95 | p99 | Round spread (CPU) |
|---|---|---|---|---|---|---|---|---|---|---|
| c3-uniq-x200 | PA:uniq | 20,874 | 111.68 µs | 6,367 stmt/s | 141 µs | 147 µs | 153 µs | 179 µs | 190 µs | 0.5 % |
| c3-uniq-x200 | PA2:uniq | 20,874 | 110.00 µs | 6,458 stmt/s | 140 µs | 145 µs | 150 µs | 177 µs | 190 µs | 0.5 % |
| c3-uniq-x200 | BP:uniq | 20,874 | 114.70 µs | 6,229 stmt/s | 145 µs | 150 µs | 157 µs | 182 µs | 193 µs | 0.3 % |
| c3-uniq-x1k | PA:uniq | 17,922 | 132.84 µs | 5,594 stmt/s | 162 µs | 169 µs | 174 µs | 202 µs | 216 µs | 0.8 % |
| c3-uniq-x1k | PA2:uniq | 17,922 | 131.80 µs | 5,624 stmt/s | 162 µs | 168 µs | 173 µs | 202 µs | 217 µs | 0.9 % |
| c3-uniq-x1k | BP:uniq | 17,922 | 136.86 µs | 5,453 stmt/s | 166 µs | 172 µs | 179 µs | 207 µs | 219 µs | 0.5 % |
| c3-uniq-x10k | PA:uniq | 7,353 | 370.61 µs | 2,352 stmt/s | 392 µs | 413 µs | 424 µs | 453 µs | 463 µs | 0.4 % |
| c3-uniq-x10k | PA2:uniq | 7,353 | 372.14 µs | 2,345 stmt/s | 388 µs | 415 µs | 426 µs | 453 µs | 464 µs | 2.0 % |
| c3-uniq-x10k | BP:uniq | 7,353 | 374.70 µs | 2,328 stmt/s | 394 µs | 418 µs | 429 µs | 456 µs | 468 µs | 0.1 % |

| Cell | BP vs PA: CPU | BP vs PA: throughput | PA2 vs PA (noise floor) | Per-round BP vs PA CPU |
|---|---|---|---|---|
| c3-uniq-x200 | +2.7 % (+3.0 µs) | -2.2 % | -1.5 % | +2.2, +3.0, +2.9 % |
| c3-uniq-x1k | +3.0 % (+4.0 µs) | -2.5 % | -0.8 % | +2.9, +3.0, +3.2 % |
| c3-uniq-x10k | +1.1 % (+4.1 µs) | -1.0 % | +0.4 % | +1.1, +1.3, +0.9 % |

A hit saves about 17 µs (CB's search was 21 µs in S0); a miss costs B 3.0-4.0 µs more than CB's data-free search
at 200 and 1,000 rows and 4.1 µs at 10,000 rows; the statistics pricing and the memo store are that difference.
The hit's saving is 4.8 % of a 363 µs statement at 10,000 rows and 0.1 % (inside the floor) of a 1,634 µs one at 60,480 rows.

## Result 4: the steward

SHOW META on B at each phase boundary of R1 (`plan_stats` and `plan_memo` blocks; `part_drops` stayed 0, which
means no core's part table filled):

| Snapshot (B) | UTC | generation | relations | posteriors | part_drops | seeds | seed_pages | publishes | steward_us_max | memo hits | memo misses | B server CPU so far |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| after-setup | 14:27:30 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.1 s |
| after-overhead | 14:28:47 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 25 | 0 | 0 | 7.3 s |
| after-cold | 14:28:48 | 1 | 8 | 4 | 0 | 8 | 16 | 1 | 51 | 87 | 73 | 7.7 s |
| after-warmup | 14:29:27 | 16 | 28 | 12 | 0 | 85 | 222 | 16 | 69 | 13964 | 2183 | 14.7 s |
| after-gain | 14:31:35 | 17 | 28 | 12 | 0 | 420 | 1188 | 17 | 69 | 74386 | 3348 | 36.2 s |
| after-memo | 14:33:34 | 17 | 28 | 12 | 0 | 729 | 2080 | 17 | 69 | 239325 | 13929 | 70.1 s |
| after-stability | 14:34:45 | 17 | 28 | 12 | 0 | 921 | 2624 | 17 | 69 | 254808 | 16580 | 96.8 s |

The steward publishes only when a band moved or a fact was added: 16 publishes in warm-up, 1 more in the 2 minutes
of gain rounds and none after (generation 17 from 14:31 to the end of the run). Its work after convergence is the
round-robin re-seed of 28 relations within the page budget, about 2.7 seeds and 7.5 pages a second. The standing
cost, with every server idle for 30 s:

| Run | A | A2 (control) | B | B minus A | A2 minus A (floor) | B steward over the 30 s |
|---|---|---|---|---|---|---|
| R1 | 136.5 ms | 147.0 ms | 136.8 ms | +0.3 ms | +10.5 ms | seeds 76, seed pages 224, publishes 0, steward_us_max 69 µs |
| R2 | 138.6 ms | 140.4 ms | 137.7 ms | -0.8 ms | +1.8 ms | seeds 79, seed pages 230, publishes 0, steward_us_max 54 µs |

B's idle CPU is within the control's spread of A's, so the steward's share of core 0 is at most the measurement's
resolution; the upper bound from `steward_us_max` (54-69 µs, one step a second) is 0.007 % of the core. The
gate (≤ 1 %) is met by two orders of magnitude.

Pool (SHOW META `pool` block). Steady-state pool misses did not move because of the steward: B's misses went from
8,819 (after warm-up) to 8,820 (after the gain rounds) while the steward read 966 pages, so every seed page was
resident. The hit counts differ because the plans do: B's flipped plans touch 5.4x fewer pool pages than A's
Probe plans in the same rounds.

| Snapshot | Server | pool_resident | pool_hits | pool_misses | pool_refused |
|---|---|---|---|---|---|
| after-setup | A | 224 | 60,566 | 224 | 0 |
| after-setup | B | 224 | 60,566 | 224 | 0 |
| after-overhead | A | 4319 | 1,650,487 | 4,319 | 0 |
| after-overhead | B | 4319 | 1,650,487 | 4,319 | 0 |
| after-cold | A | 6768 | 2,957,484 | 6,768 | 0 |
| after-cold | B | 6768 | 2,957,499 | 6,768 | 0 |
| after-warmup | A | 8927 | 18,441,282 | 8,927 | 0 |
| after-warmup | B | 8819 | 7,395,150 | 8,819 | 0 |
| after-gain | A | 9287 | 105,445,725 | 9,287 | 0 |
| after-gain | B | 8820 | 23,513,096 | 8,820 | 0 |
| after-memo | A | 10504 | 122,155,880 | 10,504 | 0 |
| after-memo | B | 10182 | 40,227,118 | 10,182 | 0 |
| after-stability | A | 10504 | 122,155,894 | 10,504 | 0 |
| after-stability | B | 11142 | 76,218,670 | 11,142 | 0 |

## Result 5: how good is the prior

`SELECT * FROM sys.relation_stats` on B after the memo phase (R2 shows the same values: the random-path estimate is
deterministic), against `COUNT(*)`. height 1: 8 relations, est/true 1.00-1.00; height 2: 17 relations, est/true 0.64-1.01; height 3: 3 relations, est/true 0.38-1.08. Relations a planned block never named carry no prior
(est_rows empty): 24 relations, all scenario 2 side tables (charges, fees, operations, organizations, recipes, ships).

| Relation | Height | Leaves | est_rows (published prior) | True COUNT(*) | est / true |
|---|---|---|---|---|---|
| models_r200 | 1 | 1 | 8 rows | 8 rows | 1.00 |
| model_results_r200 | 2 | 2 | 181 rows | 200 rows | 0.91 |
| models_r1k | 1 | 1 | 8 rows | 8 rows | 1.00 |
| model_results_r1k | 2 | 10 | 724 rows | 1,000 rows | 0.72 |
| models_r2872 | 1 | 1 | 8 rows | 8 rows | 1.00 |
| model_results_r2872 | 2 | 26 | 2,048 rows | 2,872 rows | 0.71 |
| models_r10k | 1 | 1 | 8 rows | 8 rows | 1.00 |
| model_results_r10k | 2 | 91 | 8,192 rows | 10,000 rows | 0.82 |
| symbols_x200 | 1 | 1 | 8 rows | 8 rows | 1.00 |
| daily_bars_x200 | 2 | 3 | 181 rows | 200 rows | 0.91 |
| daily_stats_x200 | 2 | 3 | 181 rows | 200 rows | 0.91 |
| symbols_x1k | 1 | 1 | 8 rows | 8 rows | 1.00 |
| daily_bars_x1k | 2 | 12 | 724 rows | 1,000 rows | 0.72 |
| daily_stats_x1k | 2 | 15 | 724 rows | 1,000 rows | 0.72 |
| symbols_x10k | 1 | 1 | 8 rows | 8 rows | 1.00 |
| daily_bars_x10k | 2 | 115 | 8,192 rows | 10,000 rows | 0.82 |
| daily_stats_x10k | 2 | 145 | 8,192 rows | 10,000 rows | 0.82 |
| symbols_x60k | 1 | 1 | 8 rows | 8 rows | 1.00 |
| daily_bars_x60k | 3 | 696 | 23,170 rows | 60,480 rows | 0.38 |
| daily_stats_x60k | 3 | 877 | 65,536 rows | 60,480 rows | 1.08 |
| cargos_n200 | 2 | 2 | 128 rows | 200 rows | 0.64 |
| freights_n200 | 2 | 2 | 181 rows | 180 rows | 1.01 |
| cargos_n1k | 2 | 10 | 724 rows | 1,000 rows | 0.72 |
| freights_n1k | 2 | 9 | 724 rows | 879 rows | 0.82 |
| cargos_ca | 2 | 100 | 8,192 rows | 10,000 rows | 0.82 |
| freights_ca | 2 | 79 | 5,793 rows | 8,234 rows | 0.70 |
| cargos_df | 3 | 2000 | 131,072 rows | 200,000 rows | 0.66 |
| freights_df | 2 | 103 | 8,192 rows | 10,795 rows | 0.76 |

Height 1 is exact (the root's live slots). Height 2 is 0.64-1.01 of the truth (0.91 at 200 rows, 0.71-0.72 at 1,000 and 2,872, 0.82 at 10,000), because the
estimate multiplies the root's children, exact, by a fixed leaf capacity and an assumed fill.
**Height 3 is the weak case:** `daily_bars_x60k` is 0.38x (23,170 against 60,480) and `cargos_df` 0.66x (131,072
against 200,000), while `daily_stats_x60k` is 1.08x. CC-S0 measured the formula at 0.75-0.98x on 30 relations of
height 2-3 and 4-6.8x off on small height-1 ones; the height-1 correction is in, the 0.38x of one height-3 relation
is the new data point. Only one gain cell involves a height-3 prior (`cargos_df`, 0.66x, the 200,000-cargo cell
whose flip was indifferent); the cross-join's 60,480-row relations (0.38x and 1.08x) leave `order=written`
in both.

## Result 6: stability and restart

A 60 s mixed run on B (six statements cycled: compare-all 2,872 and 10,000, customer-statement n1k and ca10k, the
cross-join with a varying literal at 10,000 rows and with a fixed one at 1,000), `ANALYZE` at 0, 15, 30, 45 and
60 s:

| Run | Statements | Window throughput (10 s windows) | generation before → after | publishes | seeds / seed pages in 60 s | posteriors | part_drops | steward_us_max | memo hits / misses in 60 s |
|---|---|---|---|---|---|---|---|---|---|
| R1 | 18,104 | 3051, 3059, 3055, 2975, 2971 | 17 → 17 | 17 → 17 | 160 / 448 | 12 | 0 | 69 µs | 15,478 / 2,650 |
| R2 | 18,230 | 3045, 3066, 3071, 3036, 3018 | 16 → 16 | 16 → 16 | 158 / 456 | 12 | 0 | 54 µs | 15,614 / 2,640 |

Every one of the six statements showed one executed order across its five `ANALYZE`s in both runs (the label moves
between `planned` at the first, `memo` afterwards and, for the varying-literal statement, `written` on a miss; the
steps do not). Throughput is flat across the windows (R1: 3,051 → 2,971 statements per 10 s, a 2.6 % drift over the minute with no
generation change in it). Generation did not move, so the expected "0-1 bumps and 1 order" is
met with 0.

Restart (B stopped cleanly and started again on its file, three attempts; the statement is compare-all at 2,872
results, run in a loop from the first connection):

| Attempt | Mount to listening | Statements run before the first publish | First statement to first publish (generation > 0) | First statement to warm plan (`reordered=1`, gen > 0) | recovery_checkpoint_us |
|---|---|---|---|---|---|
| 1 | 103 ms | 40 | 993 ms | 995 ms | 9.0 ms |
| 2 | 102 ms | 40 | 990 ms | 992 ms | 7.7 ms |
| 3 | 102 ms | 40 | 990 ms | 992 ms | 7.3 ms |

The first publish comes about 1 s (`kPlanStatsTickNs`) after the first statement, and the warm plan (generation
> 0, `reordered=1`) is visible 1-2 ms after it; the 40 statements before it run data-free. A first start from a
freshly copied file takes 0.86-1.0 s to listen (the recovery checkpoint of 0.9 s), against 0.103 s for a clean
restart.

## Insight

* **Statistics paid for themselves where S0 said they would** and the margin is in how much of the saved work is
  fixed against per-row: at 200 results the statement is 144 µs and a memo hit's second compile eats a fifth of
  the saving, at 10,000 it is nothing.
* **The cost model is the next constraint, not the statistics.** The only cell where the planner's choice lost
  (200,000 cargos) is one where its inputs were roughly right (the filtered-cargo estimate is 91 against about 100
  true; the large relation's prior is 0.66x) and its price of the cargo-first plan (0.68 of the written order)
  was not (measured 0.95-1.04).
* **The prior is dependable up to height 2 and weakest where leaves are many** (height 3 at 0.38x-1.08x); this
  matters only where two relations of that size compete.
* **The memo is an exact cache** (`plan-stats.md` PS6) and its two effects are measurable: a hit removes CB's
  search (17 µs here), a miss adds 3-4 µs. Repeated statements are the common OLTP shape, so the expected cost is a saving.
* **A separate engine finding came out of the matrix:** a server on which a few small tables were created and
  loaded after mount runs the "filter the small side, build the big side" plan 12.7-30.7 % slower, on both
  binaries. Nothing about it is CC's, and it would have been invisible without the A2/BL/bisection runs.

No design expectation in `workorder-cc-plan-statistics.md` §9-§10 was contradicted except the two "expected 15 %
from the flip" cells named above (200 results short by 2-3 points; 200,000 cargos not expected, S0 §Result 2).

## Cut and not executed

* Sessions on a core other than 0: `cores = 1` as ordered; the work order's "core 0 and another core" is not executed.
* `durability = strict`, saturation with several clients, and the PostgreSQL floor were not measured. **Rule 4b:** no
  cell approached a limit; these are single-connection, no-regression and flip measurements, not ceilings.
* The overhead cells are at 10,000 rows (and 200 / 1,000 were not run) because they do not scale with rows.
* The full test suite was not run in this measurement; it is the stage's gate, not a measurement cell.
* The off-core-0 steward fault is not executed. The 60 s run is static (no DDL, no writes); statistics under write
  load were not measured.
* The cause of the post-mount-tables slowdown is not diagnosed.
* The cold table reports the first 20 executions per cell, not a per-statement breakdown of the compile.

## What went wrong during the run

* The main harness's own restart cell tested for `order=planned|memo` and so counted a data-free memo hit as warm
  (`warm_plan_s` 5 ms); its output (`restart.json`) is not used and not archived. `restart2.py` replaces it
  with the generation > 0 and `reordered=1` test; the table above is that run.
* The first launch of the bisection run (`gain3`) was killed by the previous batch's watchdog, which read an
  idle status file as an orphan and relaunched; the relaunch's servers failed to start once
  (`extra5.log` keeps the traceback). The watchdog was stopped, the status file is now written at launch, and
  `gain3` was re-run from the start; no number in this file comes from the interrupted launch.
* The harness for the cross-join "vary" arm drew literals from the data's own range, so its 200- and 1,000-row
  cells mostly hit the memo; the unique-literal cell was added to measure the miss.
* No server was left running; the data files and WAL directories stay under
  `/home/cdkbs/bench-runs/cc-close/run/` and are not archived.
