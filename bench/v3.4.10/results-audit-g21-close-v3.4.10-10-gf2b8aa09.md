# Audit G21, G23-G26 close measurement: no regression in any cell that resolved, and most cells did not resolve because another session shared the host

**Thesis.** The milestone (`61917b0f`, `v3.4.10-6-g61917b0f`, against `f2b8aa09`, `v3.4.10-10-gf2b8aa09`) replaced the step counter in `txn::ResolveThroughUndo`
and `UndoLog::Walk` with a Brent cycle guard (G21), added a const `CheckApply`/`CheckDeparture` before every checked write's reservation in
`AssertionEnforcer::ReserveOne` (G26), guarded the mount's Bound Cabin chain adoption (G25), and changed two cold paths (G23 writeback drain, G24 online
`CREATE INDEX` backfill decode). Measured as an interleaved A/B in Release, **nothing regressed that the measurement could resolve, and the hottest changed path was
measured on a quiet host**: a write whose WHERE fails over rows an open transaction holds, which makes every row resolve through undo, costs
**+14 to +26 µs of server CPU per 10,000-row statement (+0.7 to +1.3 %)** and +3 to +5 µs per 1,000-row statement (+1.5 to +2.4 %) on B, against a control
(a second server running A's own binary) that reads +0.0 to +0.5 % (§3). That is a small cost 1.4-2.6 ns per resolved row at 10,000 rows,
resolved at 1,000 and 10,000 rows (B is dearer in 7 to 10 of 10 runs there) and not at 200 rows. The same run shows the **non-key scan `UPDATE` with no undo walk at all reading +4.2 to +4.5 % on B at 1,000 and 10,000 rows** (0 of 10 runs
B cheaper), a path in which this milestone changed nothing but a type name; the previous measurement of the same cell read B 3.3 % *faster*, so that sign is
layout between two builds, not code, and bounds how far the +0.7 to +2.4 % above can be read (§3, §9).
**Every other cell ran while another agent's compile and test run shared the host** (§1, §2): scenario 2 reads A 1,899 / B 1,925 TPS (+1.3 %) inside a 13.5-15.5 % within-build spread;
eight sessions on a hot row read A 18,393 / B 19,344 QPS (+5.2 %) beside a 1.7-5.5 % noise floor; the assertion cells, pinned to one core each, read assertion-minus-control p50 of +1.0 and +1.6 µs on A and B at 1,000 rows
and +18.0 / +17.4 µs at 10,000 rows (equal across builds); mounts with an assertion differ by -7.3 to +3.7 ms inside a 12-30 ms range. The scan census, run through the other agent's
whole compile, is not interpretable (B 1.3 % dearer on one path with the per-run range crossing zero).

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-11-15 (UTC). Builds 07:38-07:41; M1 pass 07:41:19-07:50:08; the cells below 08:07:56-08:27:14 (master run, status in `archive/.../status*.txt`); the quiet F4-shape cell 08:27:22-08:28:34. First command 07:37, results written at about 08:38: inside the one-hour budget only by cutting cells (§2); the first master run (07:52-07:55) and its follow-on attempts to wait for a quiet host were discarded |
| Worktree / branch | `/home/louis/ckdbs/.claude/worktrees/audit-g1-g15-fixes` on `worktree-audit-g21-g26-fixes`; measured from two detached `git worktree`s of the exact commits, built clean (`cmake -DCMAKE_BUILD_TYPE=Release --target kds_server`), removed afterwards. The branch head `e944b639` is B plus a merge of documents-only commits; `tools/` is byte-identical between A, B and the head |
| **A (baseline, the milestone's open)** | `61917b0f` = `v3.4.10-6-g61917b0f`, committed 2026-11-15 06:07:16 UTC. Release; binary mtime 07:41:13 UTC (after the commit). Copy `kds_server-A`, 7,578,800 bytes, sha256 `a810124f81c968049b163df2882289143721115cd1ac6072ad40a95056eb2780` |
| **B (closing, the milestone's last code commit)** | `f2b8aa09` = `v3.4.10-10-gf2b8aa09`, committed 2026-11-15 07:25:53 UTC. Release; binary mtime 07:41:12 UTC (after the commit). Copy `kds_server-B`, 7,592,672 bytes, sha256 `1d84cab14b452936b161bd25407aa73ce1fb851d8548bb8493feaab6ab6017a5` |
| What differs A to B in `src/` and `include/` | 24 files, 264 insertions, 111 deletions: `ResolveThroughUndo` and `UndoLog::Walk` (Brent guard, G21), `assertion_check.cpp` (`CheckApply`/`CheckDeparture`, G26), `bound_cabin.cpp` and `cabin_bound_page.cpp` (G25), `device_page_store.cpp` (G23), `index_ddl.cpp` (G24), and renames of `LeafChainGuard` to `PageChainGuard` at seven call sites in `btree.cpp`, `index_tree.cpp`, `command_dispatcher.cpp`, `step_vm.cpp` |
| Binary discipline | every server of every cell started from the two copies (and, for the control server of §3, a third copy `kds_server-A2` with A's sha256) in a scratch directory on `/tmp`, tmpfs: binary only. The mount driver writes a config without the required `buffer_pool_frames`; it ran unmodified through a one-line `exec` wrapper (`archive/.../scripts/wrap-A.sh`, `wrap-B.sh`) |
| Device | data files and WALs under `/home/louis/bench-runs/audit-g21-close/` on `/dev/nvme0n1p1`, ext4 (`df -T`) |
| **Host state: not quiet** | **Another session on this host built and ran its test suite during the run** (`cs-read-only-transaction`'s `cmake --build` with 8 `cc1plus`, then `ctest -j8` over `kds_tests`). My `pgrep cc1plus` before launch was empty (the compilers had not started); the sampler showed `ctest -j8` running during M1 rep 1.1 (07:41:19-07:43:01) and the compile and test phases from 07:50 on. I killed the first run at 07:55, re-ran under a quiet gate, and then, because the other session's activity did not end, ran the cells with the gate set to wait at most 45 s. `archive/.../status6.txt` lists the count of 2 s samples in which a compiler, `ctest` or `kds_tests` process existed during each step: **31-42 samples in each 64-95 s step (every sample), 127 in the 8.7 minute scan census, and 0 in the first-pass M1 reps 1.2 to 2.3 apart from blips of 4 s, 6 s and up to 4 s at the end of rep 2.3**. The §3 cell ran at 08:27-08:28 with one-minute load 1.5-2.8 in every run record and no competing process visible after the other session's suite ended (the busy sampler had stopped by then; load is the evidence) |
| Server config | M1, M4: `cores = 4` on CPUs 2-5, drivers on 6-7, `relaxed`; scenarios: `cores = 4`, `group`, `ash_interval_ms = 100`; scan census and §3 cell: `cores = 1`, `relaxed`, server on CPU 2, driver on 3 (census) or 4 (§3); assertion cells: `cores = 4` on 2-5 (driver 6-7) and `cores = 1`, A and B on CPUs 2 and 3 (swapped once), driver on CPUs 0-1; `buffer_pool_frames = 65536`, `ash_enabled = on` throughout. Fresh server and volume per cell |
| Drivers | unmodified `tools/lock_contention_benchmark.py` (M1, M4), `tools/scenario2_freight.py` and `tools/scenario0_stockmarket.py`, `tools/assertion_overhead_benchmark.py`, `tools/mount_cost_benchmark.py`, `tools/bm_scan.py census`, and the F4 overhead close's scratch driver `bench/v3.4.10/archive/f4-overhead-v3.4.10-1-g4f5b2e17/scripts/f4_ab.py` (a driver, not new code) for §3. The wrappers are the previous close's (`bench/v3.4.9/archive/audit-g16-close-v3.4.9-5-g83375075/scripts/`) with paths set |
| Prior numbers | this engine's own: `bench/v3.4.9/results-audit-g16-close-v3.4.9-5-g83375075.md` (every cell but §3) and `bench/v3.4.10/results-audit-f4-overhead-v3.4.10-1-g4f5b2e17.md` (§3). Cross-day deltas are context; the A/B inside this run is the comparison. PostgreSQL floor: not measured for these shapes (rule 4) |
| Raw outputs | `bench/v3.4.10/archive/audit-g21-close-v3.4.10-10-gf2b8aa09/` (`raw/` driver output, `f4/`, `scripts/`, status and watchdog logs; no data file, WAL segment or server log) |
| Tests | no engine code was changed by this measurement and **no test suite was run by it**; the coordinator reports the suite |

## 2. What was measured, and what was cut

A is `61917b0f`, B is `f2b8aa09`; a delta is B minus A. **A run flagged "shared host" below ran with another session's compiler or `ctest` live on the same eight CPUs**; its numbers are reported with that flag and read only against their own spread.

| Cell | Question | Driver and arms | Size | Host |
|---|---|---|---|---|
| F4-shape | what the Brent guard costs on the path that resolves versions through undo (G21), `ResolveThroughUndo` per row | `f4_ab.py`: 3 servers A, B, A2 (a second copy of A), 10 runs; cell 2: a second session holds an open transaction that updated all N rows, the measured autocommit `UPDATE ... WHERE v = -1` / `DELETE` fails the WHERE on the page version and the snapshot version, so every row resolves through undo; cell 1 (control): no open transaction, non-key scan and point `UPDATE` | N = 200, 1,000, 10,000 rows | **quiet-ish** (load 1.5-2.8) |
| M4 | eight sessions on one hot row (writers meet an undecided holder) | `lock_contention_benchmark.py --sessions 8 --ops 4000` | 256 rows | shared |
| M3 | scenarios (readers and writers on shared rows under transactions) | `scenario2_freight.py --bookers 8`, A,B,B,A; `scenario0_stockmarket.py --fk --txn`, A,B only | scenario 2: 300 organizations, 30 ships, 300 operations, 24,000 cargos; scenario 0: 2,000 users, 2,000 assets, 8 traders | shared |
| M6 | G26: the const check before a reservation | `assertion_overhead_benchmark.py --ops 8000 --blocks 16`, **inserts only** (the driver has no delete arm, so `CheckDeparture` is not measured), at `--cores 4` (all three sizes) and pinned `--cores 1` (three sizes, plus 1,000 rows with the two CPUs swapped) | preload 200 / 1,000 / 10,000 rows and groups | shared |
| M5 | G25: the mount's Bound Cabin chain adoption | `mount_cost_benchmark.py --mounts 7`, 3 runs per build and cell, order A B / B A / A B: empty log; `--assertion` 4,000 and 10,000 rows (clean stop); `--assertion --crash` 4,000 rows | 64 MiB segment | shared |
| M1 | single-session point statements (control) | `lock_contention_benchmark.py --sessions 1 --ops 40000`, 3 reps with A first, 3 swapped | 256 rows | first rep **contaminated**, others quiet |
| M2 | the scan (control: only a type name changed on this path) | `bm_scan.py census`, 5 runs x 12 blocks, literal and prepared | `rows-100k` (603 leaf pages), `wide-a` (304 pages) | **shared throughout** |

**What was cut or not reached.** (1) **No driver reaches G23 or G24**: G23 is the buffer pool's writeback drain after a cut run (`device_page_store.cpp`) and G24 the online `CREATE INDEX` backfill's decode split;
no driver in `tools/` times either, and I built none. The scenarios' background writeback and no index build were the only incidental exposure, and neither is evidence. (2) **`CheckDeparture` (a DELETE or an UPDATE that leaves a group) is not measured**: the assertion driver inserts only. (3) **A SELECT that reads rows an open writer holds is
not measured**: no driver does it; the nearest is the §3 cell, whose statements are writes that read the snapshot's version through undo, which is the same `ResolveThroughUndo` walk (the F4 commit made the write path take it). (4) The unmeasured chain depth: §3's chains are one version deep, so the guard's per-step cost is
measured once per row; a deep chain is not. (5) **M1 extra reps (`m1x`) and scenario 0's second pair were cut for the budget**; scenario 0 has one run per build. (6) M1 rep 1.1 is discarded (concurrent `ctest -j8`); of the five remaining reps **one is placement-matched** (§6). (7) No PostgreSQL twin. (8) No test suite.
**Discarded runs:** the first master run (07:41-07:55) lost its M4, assertion and scenario-2 cells to the other session's compile (their data files and outputs were removed, not reported), and a second attempt waited for a quiet host that did not come.

## 3. F4-shape cell: the Brent guard on the undo walk costs 0.7 to 2.4 % where resolved, with a control that reads 0.0 to 0.5 %

Quiet-ish host, the one cell here whose host held still. Each (cell, size) is 6 blocks per arm per run, 10 runs, the three servers taking the same statements, order rotating. Server on-CPU time from `/proc/<pid>/task/*/schedstat`; `bad` (wrong reply tag) was 0 in every cell (the driver aborts otherwise).
Medians over runs, [min, max]. **A2 is a second server running A's own binary: A2 minus A is the placement and drift floor**; it cannot price code layout, which differs between any two builds.

**Cell 2, every row resolves through undo (`UPDATE ... WHERE v = -1` / `DELETE` against an open transaction's rows):**

| Shape | Rows | Stmts per run per arm | A server CPU | A2 (control) | B | B minus A | A2 minus A (noise) | Runs with B < A |
|---|---|---|---|---|---|---|---|---|
| update | 200 | 600 | 65.84 µs [60.92, 73.22] | 60.85 µs | 61.67 µs [60.78, 63.98] | -4.17 µs (-6.3 %) | -4.99 µs (-7.6 %) | 9 / 10 |
| delete | 200 | 600 | 59.67 µs [58.77, 61.70] | 59.61 µs | 59.73 µs [58.40, 62.59] | +0.06 µs (+0.1 %) | -0.06 µs (-0.1 %) | 6 / 10 |
| update | 1,000 | 300 | 225.28 µs [218.91, 239.82] | 225.76 µs | 230.57 µs [221.55, 251.62] | **+5.30 µs (+2.4 %, +5.3 ns/row)** | +0.48 µs (+0.2 %) | 3 / 10 |
| delete | 1,000 | 300 | 217.31 µs [211.84, 230.04] | 217.83 µs | 220.66 µs [218.28, 236.14] | **+3.35 µs (+1.5 %, +3.4 ns/row)** | +0.52 µs (+0.2 %) | 1 / 10 |
| update | 10,000 | 60 | 1,993.68 µs [1,958.83, 2,089.66] | 1,993.86 µs | 2,007.72 µs [1,979.45, 2,104.89] | **+14.04 µs (+0.7 %, +1.4 ns/row)** | +0.18 µs (0.0 %) | 0 / 10 |
| delete | 10,000 | 60 | 1,945.09 µs [1,925.46, 2,147.31] | 1,949.81 µs | 1,971.01 µs [1,953.27, 2,099.57] | **+25.92 µs (+1.3 %, +2.6 ns/row)** | +4.72 µs (+0.2 %) | 2 / 10 |

**Reading.** At 1,000 and 10,000 rows B costs a little more than A on this path and the control does not: the B-minus-A deltas (+0.7 to +2.4 %) exceed the A2-minus-A floor (0.0 to 0.5 %) by three to ten times, and B is the cheaper arm in at most 3 of 10 runs.
At 200 rows the update's A median (65.84 µs, with a maximum of 73.2 µs) is itself 5 µs above its own control, so that row's -4.17 µs is A's noise and is no finding; the 200-row delete is +0.06 µs.
The per-row figure (1.4-5.3 ns) is the Brent check against a counter's increment on one chain step, and is 1-4 % of the path's own per-row cost (about 120-125 ns, the previous file's measure of the snapshot read). **The caution:** the same run's cell 1 below, where this milestone changed nothing but a type name, reads B +4.2 to +4.5 % at 1,000 and 10,000 rows, so
**layout between two builds can produce a larger delta than the one reported here**, and I do not attribute the +0.7 to +2.4 % to the guard alone.

| Shape | Arm | p0 | p25 | p50 | p75 | p99 | Throughput (derived, 1,000,000 / mean µs) |
|---|---|---|---|---|---|---|---|
| update, 200 rows | A | 76.8 µs | 79.1 µs | 81.0 µs | 86.7 µs | 207.4 µs | 11,060 QPS |
| update, 200 rows | B | 77.1 µs | 79.6 µs | 81.1 µs | 86.0 µs | 145.5 µs | 11,327 QPS |
| delete, 200 rows | A | 74.9 µs | 77.6 µs | 79.0 µs | 83.0 µs | 136.4 µs | 11,901 QPS |
| delete, 200 rows | B | 75.1 µs | 77.5 µs | 78.8 µs | 83.3 µs | 144.2 µs | 11,966 QPS |
| update, 1,000 rows | A | 235.0 µs | 254.9 µs | 277.1 µs | 307.6 µs | 442.8 µs | 3,460 QPS |
| update, 1,000 rows | B | 238.0 µs | 257.3 µs | 281.1 µs | 311.6 µs | 446.2 µs | 3,419 QPS |
| delete, 1,000 rows | A | 229.7 µs | 246.9 µs | 269.8 µs | 301.7 µs | 430.0 µs | 3,516 QPS |
| delete, 1,000 rows | B | 232.2 µs | 250.1 µs | 273.5 µs | 303.5 µs | 471.4 µs | 3,452 QPS |
| update, 10,000 rows | A | 1,950.1 µs | 2,020.0 µs | 2,068.2 µs | 2,172.9 µs | 2,528.8 µs | 474 QPS |
| update, 10,000 rows | B | 1,973.6 µs | 2,041.6 µs | 2,087.6 µs | 2,179.7 µs | 2,517.0 µs | 470 QPS |
| delete, 10,000 rows | A | 1,898.2 µs | 1,971.0 µs | 2,017.2 µs | 2,109.6 µs | 2,557.4 µs | 485 QPS |
| delete, 10,000 rows | B | 1,934.3 µs | 2,003.3 µs | 2,043.5 µs | 2,123.5 µs | 2,549.6 µs | 481 QPS |

(Operation count per cell and arm is stmts per run x 10 runs: 6,000 / 3,000 / 600.) The client-visible QPS delta is -0.9 to -1.2 % at 1,000 and 10,000 rows and +2.4 % / +0.5 % at 200 rows, where the 200-row cells are the noise.

**Cell 1, control: no open transaction, no undo walk** (`UPDATE t SET v = v + 1 WHERE w = ?` scans the relation; the point `UPDATE` descends by tuple key):

| Shape | Rows | A server CPU | A2 (control) | B | B minus A | A2 minus A (noise) | Runs with B < A |
|---|---|---|---|---|---|---|---|
| scan update | 200 | 38.76 µs [37.63, 40.04] | 37.98 µs | 38.65 µs | -0.11 µs (-0.3 %) | -0.78 µs (-2.0 %) | 4 / 10 |
| point update | 200 | 22.02 µs | 21.93 µs | 22.13 µs | +0.12 µs (+0.5 %) | -0.08 µs (-0.4 %) | 3 / 10 |
| scan update | 1,000 | 93.39 µs [92.07, 97.40] | 94.12 µs | 97.58 µs | **+4.18 µs (+4.5 %)** | +0.73 µs (+0.8 %) | 0 / 10 |
| point update | 1,000 | 22.29 µs | 22.05 µs | 22.13 µs | -0.16 µs (-0.7 %) | -0.24 µs (-1.1 %) | 6 / 10 |
| scan update | 10,000 | 684.06 µs [677.39, 701.95] | 685.33 µs | 712.92 µs | **+28.86 µs (+4.2 %)** | +1.26 µs (+0.2 %) | 0 / 10 |
| point update | 10,000 | 22.55 µs | 22.66 µs | 22.37 µs | -0.18 µs (-0.8 %) | +0.11 µs (+0.5 %) | 5 / 10 |

Cell 1's point updates (size-independent, as expected) are inside the noise at all three sizes. **The non-key scan reads B 4.2-4.5 % dearer at 1,000 and 10,000 rows, 0 of 10 runs B cheaper, with a 0.2-0.8 % control.** The only change on that path is `LeafChainGuard` renamed to `PageChainGuard` at the walk's call site; the guard's body (the Brent check G19 introduced) is unchanged, and
the previous file measured the same cell as B 3.3 % *faster* (A 712.45 µs, B 689.18 µs at 10,000 rows) across a different pair of builds. I cannot say what moved it (code alignment, inlining of the walk, the 13.5 KiB larger binary); it is a delta a **rename** produced, and I report it as a regression candidate that needs a second look, not as a finding about the guard: a build-to-build effect of this size on a path no logic change touched means every sub-5 % build-to-build delta in this file is within what layout alone can do.

**Delta against this engine's previous number for the same shape** (`bench/v3.4.10/results-audit-f4-overhead-v3.4.10-1-g4f5b2e17.md`, A = `v3.4.10`, B = `v3.4.10-1-g4f5b2e17`, cell 2 B arm: 61.49 / 59.80 µs at 200 rows, 222.13 / 216.65 µs at 1,000, 2,007.16 / 1,969.31 µs at 10,000): **this run's B reads 61.67 / 59.73, 230.57 / 220.66 and 2,007.72 / 1,971.01 µs, the same level to 0.0-0.3 % at 200 and 10,000 rows and +3.8 % / +1.8 % at 1,000 rows**. This run's A (the F4 commit's descendant, `v3.4.10-6`) reads 1,993.68 and 1,945.09 µs at 10,000 rows, the F4 close's B within 1.2 %; so the milestone's own effect is the +0.7-1.3 % above. Wait breakdown: **does not apply** (one session, `relaxed`, no waits; the measured quantity is server CPU).

## 4. M4 - eight sessions on one hot row: the same refusals, equal throughput inside the spread (shared host)

`lock_contention_benchmark.py --sessions 8 --ops 4000` (32,000 per arm), both servers accepted on cores 0-3. **Shared host (33 of 33 samples had a compiler or test process live).**

| Arm | Build | ops | errors | p0 | p25 | p50 | p95 | p99 | max | QPS |
|---|---|---|---|---|---|---|---|---|---|---|
| update-hot | A | 32,000 | 3,266 | 39.8 µs | 206.3 µs | 339.0 µs | 915.4 µs | 1,501.9 µs | 6,369.4 µs | 18,393 QPS |
| update-hot | B | 32,000 | 3,069 | 40.5 µs | 213.0 µs | 339.6 µs | 873.1 µs | 1,244.7 µs | 6,534.8 µs | 19,344 QPS |
| update-disjoint | A | 32,000 | 0 | 40.2 µs | 208.6 µs | 332.1 µs | 854.0 µs | 1,245.9 µs | 10,715.3 µs | 19,485 QPS |
| update-disjoint | B | 32,000 | 0 | 39.7 µs | 196.4 µs | 322.5 µs | 900.4 µs | 1,470.1 µs | 8,126.8 µs | 18,896 QPS |
| update-disjoint-again | A | 32,000 | 0 | 36.1 µs | 199.2 µs | 324.2 µs | 863.9 µs | 1,290.5 µs | 8,418.3 µs | 19,812 QPS |
| update-disjoint-again | B | 32,000 | 0 | 39.9 µs | 211.5 µs | 332.3 µs | 849.4 µs | 1,195.0 µs | 6,330.6 µs | 19,944 QPS |
| insert-omitted | A | 32,000 | 0 | 39.9 µs | 207.7 µs | 337.3 µs | 904.1 µs | 1,455.8 µs | 20,709.1 µs | 18,760 QPS |
| insert-omitted | B | 32,000 | 0 | 36.8 µs | 131.8 µs | 236.2 µs | 773.2 µs | 1,162.9 µs | 234,068.9 µs | 18,173 QPS |
| select-hot | A | 32,000 | 0 | 52.2 µs | 305.5 µs | 489.4 µs | 1,244.2 µs | 1,792.8 µs | 6,438.4 µs | 13,494 QPS |
| select-hot | B | 32,000 | 0 | 48.4 µs | 296.1 µs | 470.5 µs | 1,203.8 µs | 1,689.4 µs | 6,568.7 µs | 14,129 QPS |

Both builds refuse the hot-row arm with the same first-updater-wins message (3,266 and 3,069 of 32,000 `TXN_CONFLICT retryable=1 row id=1`). **update-hot B is +5.2 % over A, inside the noise floor of the run**: the repeat arm (disjoint vs disjoint-again) differs by 1.7 % (A) and 5.5 % (B), so no delta here is a finding. `sys.lock_waits`: all granted, tuple `U`;
A 152 rows, 72.2 ms total, max 4.8 ms; B 67 rows, 4.9 ms total, max 1.0 ms; 0 deadlock, 0 `fault_net`. `app.lock_wait` events 2,270 (A, 144.7 ms) and 1,668 (B, 10.1 ms); `cpu.on_cpu` 24.6 s and 23.8 s. The lock-wait totals differ more than the arms' throughput does and the busy host is the likelier cause (the other session's CPU use lengthens a holder's decide); the count of waits, not the throughput, is the comparable figure
and it is of the same order (A 2,270, B 1,668; the previous close read 1,838 / 1,716). The `insert-omitted` 234 ms maximum on B (the second label) is the one-stall-per-arm artefact the previous two closes recorded on the second server. This cell does **not** isolate `ResolveThroughUndo`: a writer meeting an undecided holder waits on it or is refused; it is not a reader walking a chain. §3 is the cell that does.
**Delta against the previous close** (`v3.4.9-5-g83375075`): update-hot 21,755 / 21,571 QPS, now 18,393 / 19,344, **-15 % / -10 %** across a day on a host the other session was loading; context only. Wait breakdown: lock waits are 0.59 % (A) and 0.04 % (B) of `cpu.on_cpu`; the arm's time is eight sessions queueing on four reactors.

## 5. M3 - the scenarios (shared host): inside a 14-15 % spread

### 5.1 Scenario 2 (`--bookers 8`, `group`)

The 24,000-cargo pool ran out before the 60 s allowance in every run, as at earlier closes. Order A, B, B, A. **Every run had a compiler or test process live for the whole run.**

| Run | Build | committed | TPS | booking p0 / p25 / p50 / p95 / p99 | commit p0 / p25 / p50 / p95 / p99 | `--verify` failures of 400 |
|---|---|---|---|---|---|---|
| a | A | 19,914 | 1,770.7 TPS | 411.3 / 1,781.1 / 2,840.9 / 4,732.2 / 8,009.2 µs | 378.4 / 762.6 / 914.6 / 1,426.6 / 2,457.8 µs | 106 |
| b | A | 19,904 | 2,028.0 TPS | 448.2 / 1,700.2 / 2,864.0 / 4,124.7 / 5,790.6 µs | 385.2 / 774.3 / 924.4 / 1,424.2 / 2,087.3 µs | 101 |
| a | B | 19,907 | 2,073.8 TPS | 404.7 / 1,573.2 / 2,730.0 / 3,963.9 / 6,786.1 µs | 376.9 / 738.0 / 881.5 / 1,329.6 / 2,218.4 µs | 116 |
| b | B | 19,877 | 1,775.0 TPS | 401.8 / 2,017.4 / 2,814.7 / 6,737.5 / 9,353.0 µs | 369.4 / 736.8 / 892.5 / 1,636.9 / 5,567.1 µs | 115 |

Mean TPS **A 1,899.4, B 1,924.4 (+1.3 %)**; within-build ranges 1,771-2,028 (A) and 1,775-2,074 (B), i.e. **13.5 % and 15.5 %**. The two slow runs are the first A and the last B in the A,B,B,A order. **Not a finding in either direction.** Statement-phase p50s of the four runs sit within 5-15 % of each other (`freight-insert` 134-141 µs, `charge-insert` 93-102 µs, `operation-update` 113-127 µs, `org-update` 101-116 µs).

**Wait breakdown (DB time).** The groups are the server's CPU, the group commit's `fsync`, the run queue, WAL writes and lock waits:

| Run | `cpu.on_cpu` | `commit.wal_flush` | `sched.run_queue` | `sysio.wal_write` | `sysio.checkpoint` | `app.lock_wait` | `sys.lock_waits` (tuple `U`, all granted; deadlock / `fault_net` / cancelled 0 / 0 / 0) |
|---|---|---|---|---|---|---|---|
| A a | 48.4 % | 47.3 % | 3.4 % | 0.67 % | 0.23 % | 0.023 % (20.0 ms) | 46 rows, max 12.5 ms |
| A b | 56.5 % | 40.3 % | 2.5 % | 0.39 % | 0.33 % | 0.006 % (5.1 ms) | 50 rows, max 0.4 ms |
| B a | 51.9 % | 43.7 % | 3.4 % | 0.73 % | 0.25 % | 0.018 % (13.5 ms) | 59 rows, max 6.7 ms |
| B b | 51.3 % | 44.3 % | 3.8 % | 0.32 % | 0.25 % | 0.023 % (17.7 ms) | 54 rows, max 6.2 ms |

Lock waits are two to six hundredths of a percent of DB time; the non-CPU half is the group commit's `fsync`, as at every earlier close. The run-queue share (2.5-3.8 %) is above the previous close's 2.1-2.5 %, the other session's CPU use. **Delta against the previous close** (`v3.4.9-5`): A 2,301.8 and B 2,343.7 TPS then, 1,899.4 and 1,924.4 now (-17.5 % and -17.9 %); the host, not the build, moved it (both builds fell together).

### 5.2 Scenario 0 (`--fk --txn`, 8 traders, `group`): equal, one run per build

| Run | Build | committed | TPS | txn p0 / p25 / p50 / p95 / p99 | trade-insert p0 / p25 / p50 / p95 / p99 | account-update p0 / p25 / p50 / p95 / p99 |
|---|---|---|---|---|---|---|
| a | A | 304,722 | 5,078.7 TPS | 622.9 / 1,277.0 / 1,464.6 / 1,976.9 / 2,608.8 µs | 37.8 / 85.5 / 107.5 / 204.7 / 361.6 µs | 37.1 / 75.4 / 91.2 / 175.2 / 349.2 µs |
| a | B | 307,549 | 5,125.8 TPS | 608.6 / 1,255.9 / 1,434.0 / 1,945.1 / 2,727.6 µs | 38.1 / 85.1 / 107.4 / 202.7 / 341.4 µs | 37.3 / 75.1 / 90.6 / 172.4 / 328.4 µs |

B reads +0.9 % on one run each: **no spread exists to read it against, so it is not a finding.** (Scenario 0's previous close spread was 1.0-2.0 % within a build.) Wait breakdown: `commit.wal_flush` 85.1 % (A) and 85.1 % (B) of DB time, `cpu.on_cpu` 12.7 % and 12.9 %, run queue 1.7 % and 1.6 %, WAL writes 0.5 % and 0.4 %; `sys.lock_waits` empty. 200 of 200 balances matched, 0 rolled back, 0 torn. **Delta against the previous close:** A 5,200.1 and B 5,182.8 TPS then, 5,078.7 and 5,125.8 now (-2.3 %, -1.1 %).

## 6. M1 - single-session point statements: equal on the one matched rep

One session, autocommit on its own row, `relaxed`; as at the earlier closes **a pair is comparable only if both sessions sit on the same kind of core**, because core 0 carries the ticks and core 1 reads 5-20 µs slow. Rep 1.1 (A on core 1, B on core 0) ran during the other session's `ctest -j8` and is
**discarded** (its p99 reads 0.7-3.8 ms on both builds and a 2-3x spread in QPS between identical arms); it is not in the table below. Of reps 1.2 to 2.3 the quiet sampler shows a 4 s `kds_tests` blip in 1.3 (07:45:15) and a 6 s `cc1plus` in 2.1 (07:47:12). **Only rep 1.2 (both on core 3) is placement-matched**; the other four put at least one build on core 0 or 1.

| Rep | Core A / B | Placement | Arm | A p50 | B p50 | Δ p50 (B - A) | A QPS | B QPS | QPS, B vs A |
|---|---|---|---|---|---|---|---|---|---|
| **1.2** | 3 / 3 | **matched** | update-disjoint | 40.8 µs | 40.8 µs | +0.0 µs | 22,580 | 22,760 | +0.8 % |
| **1.2** | 3 / 3 | **matched** | update-hot | 41.1 µs | 41.1 µs | +0.0 µs | 22,343 | 22,360 | +0.1 % |
| **1.2** | 3 / 3 | **matched** | insert-omitted | 38.9 µs | 38.9 µs | +0.0 µs | 21,242 | 21,165 | -0.4 % |
| **1.2** | 3 / 3 | **matched** | select-hot | 52.2 µs | 52.3 µs | +0.1 µs | 17,846 | 17,741 | -0.6 % |
| 1.3 | 1 / 3 | unmatched | update-disjoint / update-hot / insert-omitted / select-hot | 45.6 / 52.5 / 45.2 / 56.7 µs | 40.9 / 41.0 / 39.4 / 52.2 µs | -4.7 / -11.5 / -5.8 / -4.5 µs | 20,313 / 19,054 / 19,311 / 15,761 | 22,693 / 22,624 / 20,735 / 18,223 | +11.7 / +18.7 / +7.4 / +15.6 % |
| 2.1 | 0 / 3 | unmatched | same four arms | 42.5 / 43.9 / 40.7 / 58.7 µs | 40.8 / 40.9 / 39.2 / 52.4 µs | -1.7 / -3.0 / -1.5 / -6.3 µs | 20,894 / 20,121 / 20,289 / 15,259 | 22,749 / 22,299 / 20,555 / 17,891 | +8.9 / +10.8 / +1.3 / +17.2 % |
| 2.2 | 2 / 0 | unmatched | same four arms | 40.9 / 40.9 / 39.1 / 52.3 µs | 47.8 / 46.3 / 40.2 / 55.1 µs | +6.9 / +5.4 / +1.1 / +2.8 µs | 22,706 / 22,563 / 21,170 / 18,162 | 20,238 / 20,510 / 19,871 / 15,894 | -10.9 / -9.1 / -6.1 / -12.5 % |
| 2.3 | 0 / 2 | unmatched | same four arms | 49.8 / 47.0 / 48.9 / 72.6 µs | 40.9 / 40.8 / 38.9 / 52.2 µs | -8.9 / -6.2 / -10.0 / -20.4 µs | 20,150 / 20,295 / 18,691 / 14,153 | 22,494 / 22,481 / 21,408 / 18,142 | +11.6 / +10.8 / +14.5 / +28.2 % |

(Reps 1.x have A as the driver's primary label, 2.x have B.) In every unmatched rep the slower arm is the one on core 0 or core 1, whichever build it was (1.3 A on core 1; 2.1 A on core 0; 2.2 B on core 0; 2.3 A on core 0), so those four reps measure placement and **say nothing about the builds**. Matched rep 1.2's full distributions:

| Rep | Arm | Build | ops | p0 | p25 | p50 | p95 | p99 | mean | QPS |
|---|---|---|---|---|---|---|---|---|---|---|
| 1.2 | update-disjoint | A | 40,000 | 38.4 µs | 40.1 µs | 40.8 µs | 51.1 µs | 62.2 µs | 43.5 µs | 22,580 QPS |
| 1.2 | update-disjoint | B | 40,000 | 37.8 µs | 40.0 µs | 40.8 µs | 50.5 µs | 60.5 µs | 43.2 µs | 22,760 QPS |
| 1.2 | update-disjoint-again | A | 40,000 | 38.3 µs | 40.0 µs | 40.8 µs | 50.4 µs | 60.8 µs | 43.2 µs | 22,760 QPS |
| 1.2 | update-disjoint-again | B | 40,000 | 34.8 µs | 40.1 µs | 40.8 µs | 51.1 µs | 63.1 µs | 43.5 µs | 22,598 QPS |
| 1.2 | update-hot | A | 40,000 | 37.8 µs | 40.3 µs | 41.1 µs | 51.6 µs | 67.2 µs | 44.0 µs | 22,343 QPS |
| 1.2 | update-hot | B | 40,000 | 38.3 µs | 40.1 µs | 41.1 µs | 52.3 µs | 65.1 µs | 44.0 µs | 22,360 QPS |
| 1.2 | insert-omitted | A | 40,000 | 33.4 µs | 38.2 µs | 38.9 µs | 49.0 µs | 61.9 µs | 46.5 µs | 21,242 QPS |
| 1.2 | insert-omitted | B | 40,000 | 32.1 µs | 38.2 µs | 38.9 µs | 48.9 µs | 62.7 µs | 46.6 µs | 21,165 QPS |
| 1.2 | select-hot | A | 40,000 | 48.7 µs | 51.2 µs | 52.2 µs | 65.5 µs | 111.6 µs | 55.3 µs | 17,846 QPS |
| 1.2 | select-hot | B | 40,000 | 48.3 µs | 51.2 µs | 52.3 µs | 65.7 µs | 122.7 µs | 55.6 µs | 17,741 QPS |

**Reading: equal on one matched rep.** The p50 and p25 deltas are 0.0 to +0.1 µs and -0.1 to +0.1 µs; the run's own noise floor (`update-disjoint` against its repeat) is 0.0 µs at p50. One matched rep is thinner than the previous close's two; the second is the `m1x` batch that was cut. **Whole-server `cpu.on_cpu` per statement (A minus B over 240,000 statements)**: -0.41 (rep 1.1, discarded), -0.20, -2.23, -1.00, +1.05, +0.75 µs; the matched rep reads -0.20 µs, i.e. B 0.2 µs dearer of about 72 µs, and the sign changes across reps, so no CPU delta is claimed.
Wait breakdown: `relaxed`, one session, no commit wait and no one to wait for; `sys.lock_waits` is empty; a statement is the client round trip plus the server's CPU. **Delta against the previous close** (matched rep, `v3.4.9-5-g83375075`): `update-disjoint` p50 40.4 / 40.7 µs and 22,958 / 22,736 QPS then; 40.8 / 40.8 µs and 22,580 / 22,760 QPS now: the same level to 0.4 µs.

## 7. M6 - G26's const check before an assertion reservation: not resolved on `--cores 4`, small and equal on pinned cores

The driver prices an assertion against a control relation on the same server (`insert-assert-existing` against `insert-none-existing`), so the question is whether the A/B difference in that *difference* moves. **Inserts only; no delete arm exists, so `CheckDeparture` is not measured.**

**Pinned, `--cores 1` (server A and B on CPUs 2 and 3, the pair swapped once at 1,000 rows; driver on CPUs 0-1; shared host):**

| Preload | Servers (A @ cpu, B @ cpu) | Arm | A p0 / p25 / p50 / p95 / p99 | B p0 / p25 / p50 / p95 / p99 | A QPS | B QPS | A: assert - control p50 | B: assert - control p50 | noise floor, A / B (`none` vs `none-again`) p50 |
|---|---|---|---|---|---|---|---|---|---|
| 200 | 2 / 3 | insert-assert-existing | 37.4 / 43.2 / 49.7 / 110.9 / 172.8 µs | 36.6 / 43.6 / 50.0 / 113.7 / 170.5 µs | 16,240 | 16,025 | -4.5 µs | -5.0 µs | -1.0 / +2.1 µs |
| 1,000 | 2 / 3 | insert-assert-existing | 38.3 / 52.1 / 57.9 / 129.3 / 214.1 µs | 35.8 / 49.3 / 55.9 / 115.2 / 165.0 µs | 13,746 | 14,938 | +3.0 µs | -0.8 µs | +1.9 / +6.0 µs |
| 1,000 | 3 / 2 | insert-assert-existing | 37.3 / 39.0 / 39.8 / 54.3 / 92.1 µs | 35.3 / 39.3 / 40.3 / 53.2 / 83.1 µs | 22,338 | 22,528 | +1.0 µs | +1.6 µs | +0.0 / -1.5 µs |
| 10,000 | 2 / 3 | insert-assert-existing | 35.7 / 49.3 / 56.9 / 107.2 / 171.0 µs | 37.3 / 47.5 / 56.9 / 114.2 / 167.0 µs | 14,938 | 14,938 | **+18.0 µs** | **+17.4 µs** | -5.8 / -3.5 µs |
| 10,000 | 2 / 3 | insert-assert-newgroup | 36.4 / 45.0 / 51.9 / 113.1 / 166.6 µs | 34.6 / 46.8 / 54.5 / 114.5 / 164.8 µs | 15,522 | 15,297 | +12.7 µs | +15.0 µs | |

(8,000 operations per arm and server; QPS as the driver reports it.) **The 1,000-row swapped pair (A on CPU 3, B on CPU 2) is the cleanest: both builds read about 40 µs, and the assertion costs +1.0 µs (A) and +1.6 µs (B) over the control, with floors 0.0 and 1.5 µs: equal.** The CPU 2 / 3 pairing is not: the first 1,000-row pair
reads 55-58 µs for the same statement on both builds (probably the other session's compile on the sibling CPU 6; not verified) and its floors are 1.9 and 6.0 µs, which swamps a 1-2 µs effect; the 200-row pair reads the same way (-4.5 / -5.0 µs, floors -1.0 / +2.1). **At 10,000 rows the assertion costs +18 µs per insert on both builds and the two agree to 0.6 µs**; that +18 µs (a 45 % rise) is the assertion's cost on a relation with 10,000 groups, with this run's floors of -5.8 / -3.5 µs, and is **equal across A and B**, which is the milestone's question.
The conclusion for G26's added check: **no difference between A and B resolved at any size that is larger than the floor**, the 10,000-row pair (A +18.0, B +17.4 µs) being the one where a check on a large registry would show.

**`--cores 4` (the earlier closes' shape; shared host): not resolved.** The noise floors (`insert-none-existing` against `insert-none-again`) are 4.1-28.2 µs per server at 200, 1,000 and 10,000 rows, the 200-row cell reads 5,329 to 10,109 QPS across its arms, and the per-server assertion-minus-control p50 swings by -63.5 to +36.8 µs. As at the previous close, placement across four cores confounds this driver, and this time the host did too. The raw JSON is in the archive; I do not table it.
**Delta against the previous close:** that file's assertion cell was also "not resolved", with placement as the cause; no number from it is comparable. This run's pinned cells are the first that resolve an A/B difference of 1-2 µs. Wait breakdown: does not apply (serial driver, `relaxed`).

## 8. M5 - mount start-to-ready, with an assertion present: equal (shared host)

`mount_cost_benchmark.py --mounts 7 --durability relaxed`, 3 runs per build and cell, order A B / B A / A B; wall times in ms from `Popen` to the `listening on` line (a one-shot duration; QPS does not apply). `--assertion` declares a group ceiling over the loaded relation, so each loaded row leaves one Bound Cabin entry and every
measured mount runs the revival and chain adoption G25 guards (the driver's stated claim; the crash cell's recovery record count rises from the previous close's 29,348 to 33,665 with the assertion, and a clean-stop mount reads `records` 3 against 2 without one; **I read no Bound Cabin counter**, so the chain's length is the driver's, not measured here). p99 equals the maximum at n = 21; at n = 3 p25 equals p0.

| Cell | Build | Mounts | p0 | p25 | p50 | p95 | p99 | mean | `recovery_analysis_us` mean | `recovery_checkpoint_us` mean |
|---|---|---|---|---|---|---|---|---|---|---|
| empty log (no assertion) | A | 21 | 64.2 | 66.8 | 69.4 | 75.6 | 76.3 | 69.2 | 23.8 | 3.3 |
| empty log (no assertion) | B | 21 | 64.0 | 69.1 | 70.8 | 76.1 | 77.6 | 70.2 | 24.2 | 3.4 |
| 4,000 rows, assertion, clean stop | A | 21 | 77.1 | 81.9 | 82.3 | 90.6 | 92.9 | 83.9 | 17.7 | 4.0 |
| 4,000 rows, assertion, clean stop | B | 21 | 76.7 | 83.2 | 86.1 | 92.6 | 96.2 | 85.9 | 18.0 | 4.0 |
| 10,000 rows, assertion, clean stop | A | 21 | 64.5 | 69.4 | 78.5 | 86.8 | 93.0 | 76.9 | 9.4 | 4.6 |
| 10,000 rows, assertion, clean stop | B | 21 | 65.0 | 69.0 | 71.3 | 84.1 | 86.3 | 73.5 | 8.9 | 4.4 |
| 4,000 rows, assertion, SIGKILL, first mount (about 33.7k records) | A | 3 | 154.6 | 154.6 | 160.1 | 168.7 | 168.7 | 161.1 | 31.5 | 14.1 |
| 4,000 rows, assertion, SIGKILL, first mount | B | 3 | 144.0 | 144.0 | 156.5 | 161.1 | 161.1 | 153.9 | 29.5 | 14.1 |
| 4,000 rows, assertion, SIGKILL, mounts 2-7 | A | 18 | 77.3 | 81.3 | 82.0 | 96.8 | 96.8 | 84.1 | 17.8 | 4.1 |
| 4,000 rows, assertion, SIGKILL, mounts 2-7 | B | 18 | 77.4 | 79.8 | 82.1 | 96.9 | 96.9 | 84.1 | 17.4 | 4.0 |

B minus A: **+1.0 ms** (empty log), **+2.1 ms** (4,000 rows, clean), **-3.5 ms** (10,000 rows, clean), **-7.3 ms** (SIGKILL first mount, n = 3), **0.0 ms** (SIGKILL mounts 2-7); p50 +1.4, +3.7, -7.1, -3.7, +0.1 ms. Every difference is inside the baseline's own p0-to-max range (12.1, 15.8, 28.5, 14.1 and 19.5 ms). **Equal**, and the cells where the sign is negative are the cells with the wider A range, not a guard saving. Wait breakdown: does not apply (one-shot start).
**Delta against the previous close** (`v3.4.9-5-g83375075`, no assertion in its cells): its empty-log mount was 66.4 / 67.6 ms (A / B), now 69.2 / 70.2 ms; a mount with an assertion and 4,000 rows reads 21-22 ms more than the previous 4,000-row clean mount (62.5 / 63.9 ms) on both builds, which is the assertion's revival, and (no assertion cell existed there) is also the host's share, which this run cannot separate.
The mounts here ran on a loaded host (the empty-log mount is 2.8 ms slower than the previous close's on the same code path), so the previous file is context only.

## 9. M2 - the scan census (control; shared host throughout, not interpretable)

`bm_scan.py census`, `cores = 1`, A (label `base`) against B (label `close`), 5 runs of 12 blocks; server on-CPU time. The census's own quiet gate read loads 1.68-6.48 and `competing: none` per run, but the sampler counted a compiler or test process in all 127 samples of its 8.7 minutes. **Delta is A minus B** (positive = B cheaper). Medians over runs, [min, max]; same page counts on both (603, 304), same row digest, 0 bad scans.

| Path | Scan server CPU, A | Scan server CPU, B | Δ per statement | Δ per leaf page | Share of A | Per-run Δ (µs) |
|---|---|---|---|---|---|---|
| rows-100k, literal | 2,099.3 [2,081.1, 2,219.1] µs | 2,106.2 [2,081.8, 2,332.5] µs | -6.9 [-113.4, 16.6] µs | -11 [-188, 28] ns | -0.33 % | -113.4, -6.9, +12.6, -24.1, +16.6 |
| rows-100k, prepared | 2,117.2 [2,078.0, 2,236.1] µs | 2,113.7 [2,095.6, 2,592.1] µs | -27.7 [-356.0, 3.7] µs | -46 [-590, 6] ns | -1.30 % | -356.0, -30.3, +3.7, -27.7, -17.6 |
| wide-a, literal | 293.1 [284.8, 319.3] µs | 292.2 [286.4, 353.2] µs | -4.1 [-33.9, 8.3] µs | -13 [-111, 27] ns | -1.39 % | -33.9, -4.1, -5.9, +8.3, -1.8 |
| wide-a, prepared | 288.1 [281.4, 334.4] µs | 287.2 [282.5, 340.4] µs | -5.9 [-6.9, 7.4] µs | -19 [-23, 24] ns | -1.79 % | -6.0, -6.9, -0.1, +7.4, -5.9 |

Wall p50 (pooled): rows-100k literal 2,216 / 2,219 µs, prepared 2,212 / 2,214 µs (A / B; p90 and p99 in `raw/m2/summary.md`); wide-a literal 400.4 / 401.7 µs. The census does not report p0/p25. **Reading: not interpretable.** B reads dearer on 15 of 20 per-run comparisons, by -0.1 to -33.9 µs apart from run 1 (-113 to -356 µs), whose B arms read 2,332 and 2,592 µs while A's read 2,219 and 2,236: the other session's CPU burst landed in B's block. The point controls are not tabled here (see `raw/m2/summary.md`: -0.04 to +0.31 µs per statement, inside 0.4 µs).
Excluding run 1, the rows-100k prepared path still reads A minus B of -30.3, +3.7, -27.7, -17.6 µs (B about 1 % dearer, 3 of 4 runs), the same layout-sized effect §3's scan control showed, on a loop whose only change in this milestone is a type name. **Delta against the previous close** (`v3.4.9-5-g83375075`, a quiet host): rows-100k literal 2,099.4 / 2,075.9 µs (A / B), prepared 2,090.9 / 2,068.9 µs; wide-a 284.0 / 280.6 and 278.4 / 274.7 µs; this run's A and B read 0.0-1.3 % above that run's A on the rows-100k paths and 2.9-3.5 % above on wide-a. Wait breakdown: does not apply (one session scanning, no waits).

## 10. What this says about the engine

1. **The undo walk's cycle guard is about free, and I could only bound it, not price it.** The quiet-host cell puts the Brent check at +0.7 to +2.4 % of a statement that does nothing but resolve versions through undo, 1.4-5.3 ns per resolved row against about 120 ns per row of work, and the control (a copy of A's own binary) does not read it. The honest limit is that the same cell's scan control reads +4.2 % from a rename; the guard's true cost is somewhere between
   0 and the delta reported, probably nearer 0. The hot path the guard guards is cheap enough that a layout effect two to three times larger than the guard's own share can hide it, which is the best case a guard like this can offer. **No design expectation was contradicted**; the guard was meant to replace a counter without a visible cost, and it did not show one.
2. **A measurement's host is part of the measurement, and this suite has no way to make it so.** A second session on the same eight CPUs built and ran tests during every cell here but the first-pass M1 and §3, and the drivers' own host checks (`pgrep cc1plus`, load) did not catch it at the start: the load average is a minute's memory and the compilers had not yet started. The scenarios fell 17-18 % across both builds and the assertion cells' noise floors reached 28 µs. A close that depends on a shared development host should hold the host (a lock file the sessions honour) or pin to CPUs the other work cannot take; I did neither, and the numbers say what that costs: only the cells that were self-controlling (A2 in §3, pinned cores in §7, a matched rep in §6) resolved anything.
3. **The assertion's own cost is visible only with a large registry**: +18 µs per insert at 10,000 groups on both builds against +1-2 µs at 1,000 and nothing at 200. G26's added const check does not change that: A and B agree to 0.6 µs at the size where a registry-size-dependent check would show. It says nothing about `CheckDeparture`, which the driver cannot reach.
4. **A rename is a build-to-build perturbation.** Seven `LeafChainGuard` to `PageChainGuard` renames sit on the scan walk and the scan reads +4.2 % in one pair of builds and -3.3 % in the previous. Layout (alignment, inlining, binary size) is the only difference. Any close of this suite that reads an A/B delta under about 5 % on a CPU-bound statement should say it is below what two builds of the same logic produce.

## 11. Limits

| Not measured | Why |
|---|---|
| G23 (writeback drain on a cut run) and G24 (online `CREATE INDEX` backfill decode split) | no driver in `tools/` reaches either, and none was built (§2) |
| `CheckDeparture` (DELETE, or an UPDATE leaving a group) | the assertion driver inserts only |
| A SELECT resolving versions through undo under a concurrent writer | no driver; §3's write statements run the same walk, one version deep |
| Deep undo chains (the guard's per-step cost beyond one step) | §3's chains are one version deep |
| The Bound Cabin chain's length at mount | no counter was read (§8) |
| A quiet-host scenario 2, M4, M1 (beyond one matched rep), scan census, assertion at `--cores 4` and mount | another session's compile and tests shared the host (§1); these cells carry the flag and are read only against their own spread |
| M1 extra reps (`m1x`), scenario 0's second pair | cut for the budget |
| PostgreSQL floor | no twin for these shapes (rule 4: not a hole) |
| Test suite | not run by this measurement |

## 12. Reproduce

The driver commands, as run (`archive/.../scripts/`: `master.sh` through `master6.sh` are the master scripts of the attempts in order, `master6.sh` the one whose outputs are reported with `master.sh`'s first-pass M1 reps; `run_m1m4.sh`, `run_m3.sh`, `m2_run_m2.sh`, `f4run.sh`, `f4_ab.py`, `wrap-A.sh`, `wrap-B.sh`, and the analysis scripts `gen_m1b.py`, `an_m14.py`, `an_m3.py`, `assert_an.py`, `mount_an.py`):

```
# §3: f4_ab.py --arm A=<A> --arm B=<B> --arm A2=<copy of A> --workdir <ext4 dir> --port 31970 --runs 10 --out full.json --progress progress   (KDS_TOOLS=tools/)
# M1 (3 reps; the swapped batch exchanges the binaries): lock_contention_benchmark.py --port P --label base --ab-port Q --ab-label close --sessions 1 --rows 256 --ops 40000 --blocks 4
# M4: the same with --sessions 8 --ops 4000
# M3: scenario2_freight.py --suffix m3s2 --organizations 300 --ships 30 --operations 300 --cargos 24000 --verify 100 --seed 1 --sync --bookers 8 --seconds 60 --bookings 0
#     scenario0_stockmarket.py --suffix m3s0 --users 2000 --assets 2000 --traders 8 --seconds 60 --fk --txn --verify 200 --seed 1 --sync
# M5: mount_cost_benchmark.py --binary <wrapper> --label X --rows {0,4000,10000} --mounts 7 --durability relaxed [--assertion] [--crash] --port 15481 --scratch <ext4 dir> --json X.json --force
# M6: assertion_overhead_benchmark.py --port P --label B --ab-port Q --ab-label A --preload {200,1000,10000} --ops 8000 --blocks 16   (servers started with --cores 4 --taskset 2-5, or --cores 1 --taskset {2,3})
# M2: bm_scan.py census --arm base=A --arm close=B --runs 5 --blocks 12 --scan-ops 768 --point-ops 3000 --cells rows-100k,wide-a --pin-server 2 --pin-driver 3
```

Row-count mapping: §3's N is the relation loaded (`t<N>`, `u<N>`, `d<N>`); M2 `rows-100k` is 100,000 rows in 603 leaf pages and `wide-a` 10,000 wide rows in 304; M1/M4 `--rows` is the table size (256); M5 `--rows` is the spilled-row count loaded before the mounts; M6 `--preload` is the row and group count present when the measured inserts start.
