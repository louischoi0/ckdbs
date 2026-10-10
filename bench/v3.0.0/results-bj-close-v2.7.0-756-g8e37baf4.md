# BJ's close: a SET value is an expression, and the literal path pays 1.6-1.9 µs for it

**Thesis.** BJ makes the value of `UPDATE ... SET` an expression. Measured in `build-release`
against the commit BJ opens from (plus BA, which BJ's branch carries), **a point
`UPDATE ... SET v = <literal> WHERE id = k` is 1.6-1.9 µs slower, +3.0 to +3.7 % at p50,
in all six cells (`cores` 1 and 2, 200 / 1,000 / 10,000 rows)**, each cell 16 runs with the
confidence interval excluding zero; the cost does not move with the row count, so it is a
fixed price per statement, and the in-run replicate (the same statement on a second relation)
and the two-assignment literal form (`SET v = <lit>, w = <lit>`, +1.9 to +2.15 µs) say the same.
`INSERT`, `DELETE` and `SELECT` by pk and the `SHOW META` control, which no BJ code touches,
do not move beyond noise. The every-row literal `UPDATE` is 0.8-2.0 % slower at p50 in all six
cells, a sign that holds six times in six but that no single cell resolves against its own
noise floor, so it is reported as unresolved (about +10 ns per row at `cores = 1` if real).
**This contradicts the expectation in `instructions/v3.0.0/workorder-bj-expression-update.md`
section 5 ("the expectation is flat")**; BJ-R7 says nothing gates, and nothing here does.

What an expression costs on top of that, on B only: `SET v = v + 1` is 1.55-1.80 µs more per
point statement than a literal (about +3 %) and 108-130 ns more per row in an every-row statement
(+10 to +13 %); the swap `SET v = w, w = v` is 1.15-1.5 µs and 56-75 ns per row; a three-way `CASE` is
7.4-7.9 µs and 256-330 ns per row; `SET v = (SELECT COUNT(*) FROM t)` costs one walk of the
relation per statement (+47 µs at 200 rows, +137 µs at 1,000, +1.28 ms at 10,000), not one per row.
**What BJ saves the client:** `SELECT v` then `UPDATE ... SET v = <v+1>` is 124-128 µs at p50;
one `UPDATE ... SET v = v + 1` is 55-56 µs, **69-72 µs (56 %) less, 2.3 times the operations per
second**, and `BEGIN; SELECT; UPDATE; COMMIT` is 187-194 µs.

- **A** = `e5a7e1b7`, `v2.7.0-736-ge5a7e1b7`: BA's close, and **the commit the task named as
  "`080cd555` merged with BA's code `e5a7e1b7`"**. `080cd555` is an ancestor of `e5a7e1b7`
  (13 BA commits sit between them), so that merge is a fast-forward and its tree is `e5a7e1b7`'s
  (`git merge-tree` wrote a clean tree and `git diff --stat` between it and `e5a7e1b7` is empty).
  **A is therefore a real commit, not a constructed one**, and is the commit
  `results-ba-close-overhead-v2.7.0-736-ge5a7e1b7.md` measured.
- **B** = `8e37baf4`, `v2.7.0-756-g8e37baf4`, BJ's close. Between A and B `git diff --stat -- src include`
  names 18 files, all of them BJ's (parser, lexer, fingerprint, `set_expr`, step compiler, row codec,
  command dispatcher, session): BA's code is the same in both.
- Order: `instructions/v3.0.0/workorder-bj-expression-update.md` section 5. Raw output:
  `archive/bj-close-v2.7.0-756-g8e37baf4/`. Drivers are scratch drivers in its `drivers/` (below).

| Item | Value |
|---|---|
| Executed | 2026-10-10, 16:24-18:31 UTC (the 96 matrix runs and their re-runs); stage bisection 18:00-18:22; one diagnostic 18:24-18:26. Builds 16:17-16:20 (A, B) and 17:52-18:00 (stage binaries); none of this session's builds overlapped a cell |
| Branch / worktree | `worktree-bj-expression-update`, worktree `bj-expression-update` (read-only for this measurement; only `bench/v3.0.0/` files were added). Builds in two temporary detached worktrees, `bj-measure-a` and `bj-measure-b`, removed afterwards |
| B | `8e37baf4`, `v2.7.0-756-g8e37baf4`. **Tree clean**: `git status --short` was empty in `bj-expression-update` at 16:17 UTC and again at 18:32 UTC. B was built from a fresh detached checkout of that commit (`bj-measure-b`), where `git status` could not be run from this session; nothing was edited there |
| A | `e5a7e1b7`, `v2.7.0-736-ge5a7e1b7`, fresh detached checkout (`bj-measure-a`), see above |
| Binaries | Release (`CMAKE_BUILD_TYPE` read back from both `CMakeCache.txt`), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`. A: mtime 2026-10-10 16:18:38 UTC, **newer than its commit** (02:06:37 UTC). B: mtime 16:19:30 UTC, **newer than its commit** (16:15:33 UTC). `build/` (Debug) of the BJ worktree was not touched |
| Measured copies (`archive/.../binaries.txt`) | A `d2a6977f220a32147b6a2df985aec10b60ee7411fe3fca3381b78e46e525223e`, B `69733b1bb373ca6ef034ed4d391685c8bd908cfc86f861909c78d983c6bfe99d`. Every server ran from a copy under `/home/cdkbs/bench-runs/bj-close/bin/`; stage binaries' hashes are in the same file |
| Host | 8 vCPU AMD EPYC 9V74 (2 threads per core, 32 MiB L3), Linux 7.0.0-1014-azure, 15 GiB. Server pinned to CPU 2 (`cores = 1`) or CPUs 2,3 (`cores = 2`, one physical core's two threads); the driver to CPU 4. `perf_event_paranoid` = 4, so no `perf` |
| Device | data files and WAL under `/home/cdkbs/bench-runs/bj-close/` on `/dev/root`, **ext4** (`df -T` at run time), not tmpfs |
| Server config | `buffer_pool_frames = 65536` (both binaries refuse to boot without it), `log_level = warn`, roles off, `cores` = 1 or 2, **`durability = relaxed`**; fresh server and data file per (cell, run) |
| Ports | 31911 (A) and 31912 (B); 31901/31902 for two probes. Never the default; 15432 (autotrade) was left running and untouched |
| Host load | `/proc/loadavg`, the `pgrep -a -f "cc1plus\|cmake --build\|ctest"` list and a per-CPU busy fraction (`/proc/stat`) over the whole run are in every run's JSON. **Other sessions were active throughout**: builds and `ctest -j8` runs at times, and a second session's A/B benchmark (servers pinned to CPU 6) was seen at 17:08 UTC and another benchmark at 17:51 UTC. Loadavg (1 min) 1.5-9.1 over the reported runs. Each run waited for the CPUs outside {server, driver} to be under 15 % busy (3 s window, at most 300 s). **Runs with a real `cmake --build` or `ctest` in their list were discarded and re-run (6 runs), and so were runs with other-CPU busy over 30 % (5 runs)**; 11 runs in all, their summaries kept in `archive/.../discarded/`. What stayed: other-CPU busy 2.2-28 % per run (median 2.8 % to 17.3 % per cell), and 4 runs whose list matches only another session's idle shell that contains the pattern (`until ... pgrep -f "cmake --build"`) |
| Suite | **Not executed in this session.** This measurement changes no code, so there was no before/after to run it over |

## What was run, and what was not

Run, at `cores = 1` and `cores = 2`, 200 / 1,000 / 10,000 rows, **16 runs per cell** (96 runs;
48,000 point statements per arm and shape pooled, 1,920 every-row statements):

- **Part 1, A and B interleaved** (section 1): the literal path (`SET v = <lit>` by pk, its replicate on a
  second relation, the two-assignment literal form, an every-row `SET v = <lit>` and its replicate) and the
  controls (`INSERT`, `SELECT` by pk, `DELETE` by pk, `SHOW META`).
- **Part 2, B only** (section 2): `SET v = v + 1`, `SET v = w, w = v`, a `CASE`, and `SET v = (SELECT
  COUNT(*) FROM t)` against `SET v = <lit>`, point and every-row.
- **Part 3, B only** (section 3): the client's former round trip, `SELECT v` then `UPDATE`, against one `UPDATE`.
- **A stage bisection** of the literal path (section 1.2): seven earlier commits' binaries against A, or
  against BJ's opening commit, at `cores = 1`, 1,000 rows, 12 runs, Part 1 only.
- **One diagnostic** of a 0.2-0.45 s stall that lands in the `INSERT` arm (section 6).

Not run: see "Not run" below. **Every reply is checked** (`UPDATED 1`, `UPDATED <rows>`, `INSERTED`,
`DELETED`; 0 mismatches and 0 errors in all 96 reported runs) **and every updating arm's end state is
verified** by a `SELECT` against the value the arm must have left (`SUM(v)`, `SUM(w)`, per-key models
for the point arms; `verify=ok` in every reported run).

## How it was run

**Rule 5 of `bench/README.md` is broken, as in the BH close.** `tools/` has no driver for `UPDATE` with
expressions or one that sets `buffer_pool_frames`, and `bench/docs/` stays closed. The scratch drivers are
in `archive/.../drivers/`: `bj_close_ab.py` (the matrix; shaped like
`archive/bh-close-v2.7.0-712-g4015736d/drivers/bh_overhead_ab.py`, imports `tools/bench_common.Phase`,
`tools/ckdbs_cli.ServerConnection` and `tools/bb_overhead_benchmark` unmodified), `bj_stage_ab.py` (the same with
`--p1-only`), `analyze.py`, `matrix.py`, `p2matrix.py`, `analyze_bisect.py`, and the shell scripts that ran
them (`run_all.sh`, `rerun_c2.sh`, `rerun2.sh`, `build_stages.sh`, `run_bisect.sh`). The KWP client is
`ServerConnection` over the default port, one serial session per server.

- **Rows.** `--rows N` is the row count every relation holds when an arm starts, loaded as
  `t (id int64, v int64, w int64) BTREE` with named ascending keys `1..N`, `v = id`, `w = id` (`w = id + 7` in
  the Part 2 and 3 relations), in 500-row `INSERT`s. A point statement targets key `1 + (q * 7919) mod N`. **An
  every-row statement is `WHERE id >= 1`, so it writes all N rows**: 200, 1,000 or 10,000 per statement. The
  three sizes are the first-class axis of every table.
- **Equal work.** Part 1: per (cell, run), A and B each run 3,000 statements per point arm and 120 per every-row
  arm, in 12 blocks (250 and 10 statements) that alternate which server goes first, the first server of a
  run alternating too, so a drift is shared. Part 2: one relation per arm, the arms of a group in blocks
  of the same size, rotated by one arm per block. Part 3: the three spellings rotated per block.
- **Statistics.** Per-run p50 and mean from the driver; pooled percentiles over the 16 runs from the raw
  latencies. The delta is the **median of the per-run p50 deltas B - A** with a bootstrap 95 % CI (4,000
  resamples of the 16 runs). Throughput in the headline matrix is **derived from the p50, 1,000,000 / p50
  µs** (rule 5a: a serial single-connection driver, so exact up to the choice of p50 over the mean); the driver's
  own `ops / elapsed` is in the appendix tables and **carries stalls** (section 6), which is why the matrix
  does not use it. The two bootstraps (headline matrix and appendix) use different random streams, so their
  intervals differ in the second decimal.
- **Noise floor from inside the run** (rule 8): the control `SHOW META` (a statement no BJ path
  touches), and the **replicate** arm (the same shape on a second relation, same binary). A delta is called
  resolved only if its CI excludes zero **and** it exceeds the floor: the larger of the control's CI bound and
  `|median| + 2 x robust sd` of the replicate's per-run relative difference (point arms 0.6-1.2 %, every-row
  arms 1.0-4.6 % by cell).
- **Quiet-machine gate and discards**: described in the host-load row of the stamp. The 11 discarded runs were
  replaced by new runs of the same (cell, run) index; **the 16 per cell are 16 kept runs**. The strict subset
  (only runs with other-CPU busy at or under 10 %: 16, 12 and 5 runs at `cores = 1`, 11, 10 and 9 at `cores = 2`, for 200 / 1,000 / 10,000 rows)
  gives the same point-statement deltas to within 0.4 µs (`archive/.../tables-per-cell-strict-subset.md`).

## 1. The literal path: +1.6-1.9 µs per point `UPDATE`, flat in the row count

**Finding.** A statement that BJ was not asked to change, a bare literal in `SET`, got 1.6-1.9 µs slower
per statement. The controls beside it are flat, so it is not the host or the build; the replicate arm is
the same size, so it is not the relation or the arm's position.

Throughput at p50 (derived, `1,000,000 / p50`), A → B, with the median per-run change and its 95 % CI.
Statements per second:

| shape | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| `UPDATE SET v = <lit> WHERE id = k` | 19,493 → 18,832 /s (-3.4 % [-3.7, -2.8]) | 19,474 → 18,832 /s (-3.0 % [-4.1, -2.6]) | 19,417 → 18,622 /s (-3.5 % [-4.5, -3.1]) | 18,921 → 18,365 /s (-3.0 % [-4.1, -2.6]) | 18,975 → 18,365 /s (-3.1 % [-4.4, -2.6]) | 18,832 → 18,298 /s (-2.9 % [-3.3, -2.4]) |
| same shape, second relation (replicate) | 19,512 → 18,850 /s (-3.4 % [-3.7, -2.8]) | 19,493 → 18,904 /s (-2.9 % [-4.0, -2.6]) | 19,342 → 18,622 /s (-3.3 % [-4.5, -2.8]) | 19,029 → 18,416 /s (-3.3 % [-4.1, -2.4]) | 19,084 → 18,382 /s (-3.5 % [-4.6, -2.5]) | 18,868 → 18,315 /s (-2.9 % [-4.1, -2.4]) |
| `UPDATE SET v = <lit>, w = <lit> WHERE id = k` | 19,249 → 18,501 /s (-4.0 % [-4.3, -3.0]) | 19,268 → 18,484 /s (-4.0 % [-4.8, -3.2]) | 19,066 → 18,315 /s (-3.7 % [-4.4, -3.4]) | 18,709 → 18,116 /s (-3.4 % [-4.2, -2.4]) | 18,779 → 18,100 /s (-3.8 % [-4.5, -2.7]) | 18,657 → 17,986 /s (-3.4 % [-4.1, -2.6]) |
| `UPDATE SET v = <lit> WHERE id >= 1` (every row) | 4,220 → 4,159 /s (-0.8 % [-1.7, +0.0]) | 1,046 → 1,037 /s (-0.9 % [-1.2, -0.0]) | 102 → 101 /s (-1.0 % [-1.5, -0.5]) | 3,238 → 3,180 /s (-1.5 % [-2.5, -1.1]) | 766 → 752 /s (-1.8 % [-2.2, -1.2]) | 75 → 74 /s (-1.9 % [-3.2, +0.4]) |
| same shape, second relation (replicate) | 4,163 → 4,161 /s (+0.1 % [-1.0, +0.7]) | 1,052 → 1,031 /s (-1.7 % [-2.5, -0.7]) | 102 → 101 /s (-1.4 % [-1.5, -0.5]) | 3,171 → 3,143 /s (-1.5 % [-1.9, +0.6]) | 760 → 743 /s (-1.7 % [-3.2, -0.7]) | 75 → 74 /s (-0.7 % [-2.4, +0.5]) |
| `INSERT`, pk named (not touched by BJ) | 20,346 → 20,325 /s (-0.3 % [-0.6, +0.2]) | 20,325 → 20,243 /s (-0.5 % [-0.8, +0.0]) | 20,284 → 20,284 /s (-0.1 % [-0.3, +0.2]) | 20,121 → 20,040 /s (-0.5 % [-0.8, +0.1]) | 20,121 → 19,940 /s (-0.8 % [-1.8, -0.2]) | 19,900 → 19,802 /s (-1.2 % [-2.2, +0.2]) |
| `SELECT * WHERE id = k` (control) | 15,302 → 15,279 /s (+0.2 % [-0.5, +0.8]) | 14,760 → 14,749 /s (+0.0 % [-0.7, +0.3]) | 15,444 → 15,385 /s (+0.0 % [-0.9, +0.2]) | 15,026 → 15,038 /s (-0.1 % [-0.2, +0.8]) | 14,556 → 14,524 /s (-0.5 % [-1.2, +0.3]) | 14,993 → 14,993 /s (+0.1 % [-0.8, +0.6]) |
| `DELETE WHERE id = k` (not touched by BJ) | 20,492 → 20,513 /s (+0.0 % [-0.3, +0.6]) | 20,513 → 20,471 /s (+0.0 % [-0.4, +0.2]) | 20,450 → 20,408 /s (-0.2 % [-0.4, +0.2]) | 20,263 → 20,243 /s (-0.2 % [-0.6, +0.2]) | 20,284 → 20,243 /s (-0.1 % [-0.8, +0.6]) | 20,263 → 20,202 /s (-0.2 % [-1.2, +0.6]) |
| `SHOW META` (control) | 19,881 → 19,920 /s (+0.2 % [-0.1, +0.6]) | 19,940 → 19,841 /s (-0.4 % [-0.8, -0.3]) | 19,920 → 19,940 /s (+0.1 % [-0.4, +0.4]) | 19,861 → 19,841 /s (-0.1 % [-0.6, +0.4]) | 19,802 → 19,841 /s (+0.0 % [-0.8, +0.8]) | 19,900 → 19,822 /s (-0.3 % [-0.8, +0.8]) |

The same data as p50 latency: A's p50, then B - A with the 95 % CI and the relative change:

| shape | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| `UPDATE SET v = <lit> WHERE id = k` | 51.3 µs: +1.80 µs [+1.50, +2.00] (+3.5 %) | 51.3 µs: +1.60 µs [+1.40, +2.20] (+3.1 %) | 51.5 µs: +1.90 µs [+1.65, +2.30] (+3.7 %) | 52.8 µs: +1.65 µs [+1.40, +2.20] (+3.1 %) | 52.7 µs: +1.70 µs [+1.40, +2.40] (+3.2 %) | 53.1 µs: +1.60 µs [+1.30, +1.80] (+3.0 %) |
| same shape, second relation (replicate) | 51.2 µs: +1.80 µs [+1.50, +2.00] (+3.5 %) | 51.3 µs: +1.55 µs [+1.40, +2.10] (+3.0 %) | 51.7 µs: +1.80 µs [+1.50, +2.40] (+3.5 %) | 52.5 µs: +1.80 µs [+1.30, +2.20] (+3.4 %) | 52.4 µs: +1.90 µs [+1.40, +2.50] (+3.6 %) | 53.0 µs: +1.55 µs [+1.30, +2.20] (+2.9 %) |
| `UPDATE SET v = <lit>, w = <lit> WHERE id = k` | 52.0 µs: +2.15 µs [+1.60, +2.30] (+4.1 %) | 51.9 µs: +2.15 µs [+1.70, +2.60] (+4.1 %) | 52.5 µs: +2.00 µs [+1.90, +2.40] (+3.8 %) | 53.5 µs: +1.90 µs [+1.30, +2.30] (+3.6 %) | 53.2 µs: +2.10 µs [+1.50, +2.50] (+3.9 %) | 53.6 µs: +1.90 µs [+1.40, +2.30] (+3.5 %) |
| `UPDATE SET v = <lit> WHERE id >= 1` (every row) | 237 µs: +1.9 µs [-0.1, +4.2] (+0.8 %) | 956 µs: +8.6 µs [+0.4, +11.6] (+0.9 %) | 9,759 µs: +96.6 µs [+54.4, +151.9] (+1.0 %) | 309 µs: +4.7 µs [+3.5, +7.3] (+1.5 %) | 1,306 µs: +24.6 µs [+15.5, +29.2] (+1.9 %) | 13,307 µs: +261.8 µs [-15.4, +448.4] (+2.0 %) |
| same shape, second relation (replicate) | 240 µs: -0.2 µs [-1.7, +2.4] (-0.1 %) | 951 µs: +16.8 µs [+6.0, +23.7] (+1.8 %) | 9,765 µs: +134.0 µs [+50.1, +165.8] (+1.4 %) | 315 µs: +4.7 µs [-1.8, +6.2] (+1.5 %) | 1,315 µs: +22.3 µs [+9.8, +44.8] (+1.7 %) | 13,417 µs: +92.6 µs [-99.8, +315.7] (+0.7 %) |
| `INSERT`, pk named (not touched by BJ) | 49.2 µs: +0.15 µs [-0.10, +0.30] (+0.3 %) | 49.2 µs: +0.25 µs [+0.00, +0.40] (+0.5 %) | 49.3 µs: +0.05 µs [-0.10, +0.20] (+0.1 %) | 49.7 µs: +0.25 µs [+0.00, +0.40] (+0.5 %) | 49.7 µs: +0.40 µs [+0.10, +0.90] (+0.8 %) | 50.2 µs: +0.60 µs [-0.10, +1.10] (+1.2 %) |
| `SELECT * WHERE id = k` (control) | 65.3 µs: -0.10 µs [-0.60, +0.30] (-0.2 %) | 67.8 µs: +0.00 µs [-0.20, +0.50] (+0.0 %) | 64.8 µs: +0.00 µs [-0.10, +0.60] (+0.0 %) | 66.5 µs: +0.05 µs [-0.50, +0.25] (+0.1 %) | 68.7 µs: +0.35 µs [-0.20, +0.80] (+0.5 %) | 66.7 µs: -0.05 µs [-0.40, +0.50] (-0.1 %) |
| `DELETE WHERE id = k` (not touched by BJ) | 48.8 µs: +0.00 µs [-0.30, +0.10] (+0.0 %) | 48.8 µs: +0.00 µs [-0.10, +0.20] (+0.0 %) | 48.9 µs: +0.10 µs [-0.10, +0.25] (+0.2 %) | 49.3 µs: +0.10 µs [-0.10, +0.30] (+0.2 %) | 49.3 µs: +0.05 µs [-0.35, +0.40] (+0.1 %) | 49.3 µs: +0.10 µs [-0.30, +0.60] (+0.2 %) |
| `SHOW META` (control) | 50.3 µs: -0.10 µs [-0.30, +0.05] (-0.2 %) | 50.2 µs: +0.20 µs [+0.20, +0.40] (+0.4 %) | 50.2 µs: -0.05 µs [-0.20, +0.20] (-0.1 %) | 50.3 µs: +0.05 µs [-0.20, +0.30] (+0.1 %) | 50.5 µs: +0.00 µs [-0.40, +0.40] (+0.0 %) | 50.2 µs: +0.15 µs [-0.40, +0.40] (+0.3 %) |

Reading. **Resolved**: the three point `UPDATE` shapes (single literal, its replicate, two literals), in all six
cells, +3.0 to +4.1 % at p50 against a floor of 0.6-1.2 %; **not resolved**: the every-row literal `UPDATE`
(+0.8 to +2.0 % in six of six cells and +0.7 to +1.8 % in five of six replicate cells, each inside its own
floor, so about +10 ns per row at `cores = 1` and +25 ns at `cores = 2` *if* the sign is real); **no change
resolved**: `INSERT` (the largest cell is +0.60 µs [-0.10, +1.10] at `cores = 2`, 10,000 rows), `SELECT`,
`DELETE`, `SHOW META`. The second assignment costs +0.1 to +0.55 µs on top of the first (`lit2` minus `lit`), so
the price is per statement, not per assignment. The price is the same at 200, 1,000 and 10,000 rows
(+1.80, +1.60, +1.90 µs at `cores = 1`; +1.65, +1.70, +1.60 µs at `cores = 2`), so **it is a fixed
cost of the statement (parse, compile, evaluate), not a per-row one**.

### 1.2 Where it arrives: a cumulative picture, not a resolved bisection

Binaries of earlier commits against A (or, for the three commits that do not carry BA, against BJ's opening
commit `080cd555`), `cores = 1`, 1,000 rows, 12 runs, Part 1 only. Median per-run p50 delta of the **right**
binary minus the **left**, 95 % CI:

| left vs right | runs | point `UPDATE SET v = <lit>` | its replicate | `SET v = <lit>, w = <lit>` | `SHOW META` (control) |
|---|---|---|---|---|---|
| `080cd555` (BJ opens) vs `586ffa5f` (+ BJ-Q12) | 12 | +0.50 µs [-0.15, +0.70] | +0.45 µs [-0.30, +1.20] | +0.55 µs [-0.10, +1.20] | -0.05 µs [-0.15, +0.10] |
| `080cd555` vs `6a4fb896` (+ BJ-S1, S2: lexer, AST, parser) | 12 | +0.75 µs [+0.30, +1.35] | +0.55 µs [+0.15, +0.90] | +0.75 µs [+0.30, +1.25] | -0.05 µs [-0.15, +0.10] |
| `080cd555` vs `88155d5b` (+ BJ-S3: typer) | 12 | +0.95 µs [+0.70, +1.35] | +1.00 µs [+0.50, +1.35] | +1.30 µs [+0.65, +1.95] | +0.05 µs [-0.15, +0.10] |
| `080cd555` vs `e5a7e1b7` (A: BA, not BJ) | 12 | +0.65 µs [+0.10, +1.25] | +0.60 µs [+0.30, +0.85] | +0.55 µs [+0.35, +0.85] | **+5.15 µs [+4.75, +5.30]** |
| A vs `836b9551` (BA merged with BJ-Q12, S1..S3) | 12 | +0.65 µs [+0.25, +0.90] | +0.70 µs [+0.25, +0.90] | +0.85 µs [+0.35, +1.40] | +0.10 µs [-0.20, +0.40] |
| A vs `1a251cea` (+ BJ-S4: evaluation and the write) | 12 | +1.35 µs [+0.40, +2.10] | +1.40 µs [+0.30, +2.15] | +1.60 µs [+0.75, +2.25] | +0.00 µs [-0.20, +0.30] |
| A vs `3292b20d` (+ BJ-S5, S6) | 12 | +1.35 µs [+0.40, +1.75] | +1.50 µs [+0.60, +1.85] | +1.70 µs [+0.95, +2.15] | -0.10 µs [-0.25, +0.10] |
| A vs B `8e37baf4` (+ BJ-S7; the matrix above, same cell, 16 runs) | 16 | +1.60 µs [+1.40, +2.20] | +1.55 µs [+1.35, +2.10] | +2.15 µs [+1.70, +2.60] | +0.20 µs [+0.20, +0.40] |

Read it as a trend, not as steps: the 12-run intervals are 0.6-1.7 µs wide and neighbouring rows overlap.
About **0.65-0.95 µs of the final 1.6 is in by the parser and typer stages** (BJ-S2/S3: the statement's SET
value is now parsed into an expression and typed at compile even when it is one literal), **about 0.7 more
arrives with BJ-S4** (the evaluator and the row loop that writes after evaluating), and BJ-S5 to S7 add
nothing resolvable on top. BJ-Q12 alone (a duplicate-target check, +0.5 µs) is not resolved from zero in any
of the three columns. **The `080cd555` to A row is BA's, not BJ's**: +0.65 µs on the same statement and, in the
control column, **+5.15 µs on `SHOW META`**, which is BA's change to what that statement reports; it is why
A, not `080cd555`, is the baseline of this file. No attribution inside a stage: `perf` is locked out
(`perf_event_paranoid` = 4) and the engine has no per-stage statement timers.

## 2. What an expression costs

**Finding.** On B, an expression adds a few microseconds per statement or 60-330 ns per row; none of it is
close to the 50 µs a statement already spends on the client round trip. `SET v = v + 1` costs about as much
again as the literal path's new 1.7 µs; a three-way `CASE` is the one expensive shape; the subquery runs once.

Statements per second at p50 (derived), B only. **Point** (`WHERE id = k`):

| SET value | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| `SET v = <lit>` | 18,762 /s | 18,797 /s | 18,536 /s | 18,416 /s | 18,332 /s | 18,215 /s |
| `SET v = v + 1` | 18,182 /s | 18,182 /s | 18,018 /s | 17,937 /s | 17,809 /s | 17,683 /s |
| `SET v = w, w = v` | 18,282 /s | 18,315 /s | 18,100 /s | 18,034 /s | 17,953 /s | 17,794 /s |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 16,393 /s | 16,420 /s | 16,207 /s | 16,260 /s | 16,064 /s | 16,026 /s |

**Every row** (`WHERE id >= 1`: each statement writes N rows), statements per second at p50, and rows per
second in brackets:

| SET value | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| `SET v = <lit>` | 4,163.2 /s (832,639 rows/s) | 1,046.7 /s (1,046,737 rows/s) | 109.5 /s (1,095,218 rows/s) | 3,419.4 /s (683,878 rows/s) | 835.6 /s (835,596 rows/s) | 87.2 /s (872,011 rows/s) |
| `SET v = v + 1` | 3,772.2 /s (754,432 rows/s) | 938.2 /s (938,174 rows/s) | 97.2 /s (972,342 rows/s) | 3,162.6 /s (632,511 rows/s) | 766.1 /s (766,078 rows/s) | 79.2 /s (791,869 rows/s) |
| `SET v = w, w = v` | 3,905.5 /s (781,097 rows/s) | 975.6 /s (975,562 rows/s) | 101.3 /s (1,012,730 rows/s) | 3,281.9 /s (656,383 rows/s) | 790.7 /s (790,733 rows/s) | 82.2 /s (821,794 rows/s) |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 3,281.4 /s (656,276 rows/s) | 805.2 /s (805,185 rows/s) | 85.2 /s (852,217 rows/s) | 2,849.4 /s (569,882 rows/s) | 686.6 /s (686,601 rows/s) | 71.2 /s (712,159 rows/s) |
| `SET v = (SELECT COUNT(*) FROM t)` | 3,510.6 /s (702,124 rows/s) | 913.5 /s (913,534 rows/s) | 96.0 /s (959,559 rows/s) | 2,946.4 /s (589,275 rows/s) | 750.6 /s (750,582 rows/s) | 78.9 /s (788,696 rows/s) |

The prices, per statement and per row, over B's own `SET v = <lit>` in the same run (median of the 16
per-run differences, 95 % CI). **Point statements**, extra µs per statement:

| | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| `SET v = v + 1` | +1.75 µs [+1.60, +1.90] | +1.80 µs [+1.65, +1.90] | +1.65 µs [+1.50, +1.80] | +1.55 µs [+1.40, +1.60] | +1.70 µs [+1.50, +1.70] | +1.65 µs [+1.55, +1.75] |
| `SET v = w, w = v` | +1.50 µs [+1.30, +1.60] | +1.40 µs [+1.20, +1.60] | +1.30 µs [+1.20, +1.40] | +1.15 µs [+1.00, +1.20] | +1.25 µs [+1.10, +1.40] | +1.30 µs [+1.20, +1.40] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | +7.85 µs [+7.70, +8.00] | +7.70 µs [+7.50, +8.00] | +7.80 µs [+7.70, +7.90] | +7.35 µs [+7.20, +7.50] | +7.75 µs [+7.60, +7.90] | +7.50 µs [+7.35, +7.70] |

**Every-row statements**, extra ns per row (the statement's extra cost over N rows):

| | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| `SET v = v + 1` | +130 ns [+123, +140] | +108 ns [+106, +113] | +117 ns [+115, +119] | +118 ns [+107, +130] | +109 ns [+106, +118] | +115 ns [+107, +119] |
| `SET v = w, w = v` | +75 ns [+66, +92] | +68 ns [+65, +72] | +74 ns [+67, +81] | +56 ns [+50, +72] | +65 ns [+60, +75] | +71 ns [+67, +72] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | +330 ns [+322, +338] | +284 ns [+278, +290] | +262 ns [+259, +266] | +296 ns [+284, +302] | +266 ns [+257, +274] | +256 ns [+252, +258] |
| `SET v = (SELECT COUNT(*) FROM t)` | +237 ns [+222, +241] | +137 ns [+133, +139] | +128 ns [+125, +132] | +229 ns [+219, +242] | +136 ns [+129, +144] | +123 ns [+121, +126] |

Extra µs per every-row statement:

| | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| `SET v = v + 1` | +25.95 µs [+24.70 µs, +28.10 µs] | +108.35 µs [+106.40 µs, +113.40 µs] | +1,167 µs [+1,150 µs, +1,191 µs] | +23.60 µs [+21.30 µs, +26.00 µs] | +109.15 µs [+106.40 µs, +118.50 µs] | +1,153 µs [+1,074 µs, +1,194 µs] |
| `SET v = w, w = v` | +15.00 µs [+13.30 µs, +18.40 µs] | +68.25 µs [+65.00 µs, +71.50 µs] | +737.65 µs [+673.50 µs, +808.70 µs] | +11.25 µs [+9.90 µs, +14.40 µs] | +65.30 µs [+60.00 µs, +75.20 µs] | +706.20 µs [+666.70 µs, +718.30 µs] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | +65.90 µs [+64.40 µs, +67.70 µs] | +283.90 µs [+277.60 µs, +289.80 µs] | +2,624 µs [+2,590 µs, +2,662 µs] | +59.15 µs [+56.80 µs, +60.50 µs] | +265.85 µs [+256.80 µs, +273.80 µs] | +2,557 µs [+2,525 µs, +2,580 µs] |
| `SET v = (SELECT COUNT(*) FROM t)` | +47.40 µs [+44.40 µs, +48.20 µs] | +136.55 µs [+133.20 µs, +138.95 µs] | +1,283 µs [+1,250 µs, +1,318 µs] | +45.80 µs [+43.90 µs, +48.30 µs] | +135.60 µs [+129.20 µs, +144.00 µs] | +1,229 µs [+1,208 µs, +1,264 µs] |

p50 per row, ns, for the every-row statements (the literal `UPDATE`'s own cost per written row is the
first line; the others are that plus the table above):

| | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| `SET v = lit` | 1209 ns | 959 ns | 922 ns | 1463 ns | 1202 ns | 1150 ns |
| `SET v = v + 1` | 1334 ns | 1068 ns | 1036 ns | 1580 ns | 1316 ns | 1263 ns |
| `SET v = w, w = v` | 1287 ns | 1031 ns | 1004 ns | 1522 ns | 1269 ns | 1218 ns |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1534 ns | 1243 ns | 1181 ns | 1758 ns | 1468 ns | 1403 ns |
| `SET v = (SELECT COUNT(*) FROM t)` | 1434 ns | 1095 ns | 1053 ns | 1692 ns | 1340 ns | 1273 ns |

Reading. The slopes between 1,000 and 10,000 rows (difference of the per-statement extras over 9,000 rows) are, in ns per
row, `v + 1` 117 / 116 (`cores` 1 / 2), the swap 74 / 71, the `CASE` 260 / 255, the subquery 127 / 122: a price on
the row loop, linear in the rows.
`v + 1` reads one column, adds with an overflow check and stores; the swap reads two and does no
arithmetic, so it is the cheaper per row; the `CASE` evaluates a comparison, a modulus and an add or a
subtract, and is 2.2 times `v + 1`. **The subquery's price is one walk of the relation per statement**
(137 µs at 1,000 rows, 1.28 ms at 10,000), 10 times per decade of rows and not 100: a per-row subquery
would grow with the square of the rows, so BJ-Q6's "evaluated once before the walk" holds in the cost as in the
design. At 200 rows the same subquery costs 229-237 ns per row because a fixed part of about 22 µs is spread
over fewer rows.

## 3. What BJ saves the client: one round trip, 56 %

**Finding.** The saving is the second client round trip, not the arithmetic.

Operations per second at p50 (derived), B, one session, `durability = relaxed`:

| spelling | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| one statement `UPDATE SET v = v + 1` | 18,215 /s | 18,215 /s | 18,051 /s | 17,921 /s | 17,825 /s | 17,794 /s |
| `SELECT v` then `UPDATE SET v = <v+1>` (2 round trips) | 8,065 /s | 8,016 /s | 8,042 /s | 7,905 /s | 7,837 /s | 7,908 /s |
| `BEGIN; SELECT; UPDATE; COMMIT` (4 round trips) | 5,373 /s | 5,263 /s | 5,330 /s | 5,262 /s | 5,176 /s | 5,297 /s |

Per-operation latency, cores = 1, 1,000 rows (pooled over 16 runs; the full tables per cell are in the
appendix): the single `UPDATE SET v = v + 1` p0 / p25 / p50 / p95 / p99 = 44.9 / 54.3 / 55.1 / 73.3 / 89.7 µs;
`SELECT v` then `UPDATE SET v = <v+1>` = 114.7 / 122.5 / 125.7 / 166.4 / 202.8 µs (of which the `SELECT`
65.6 µs and the `UPDATE` 58.2 µs at p50); `BEGIN; SELECT; UPDATE; COMMIT` = 169.6 / 183.2 / 191.3 / 247.0 /
297.4 µs. Over all six cells the pair costs **+68.8 to +71.8 µs** at p50 more than the single statement
(CIs within ±0.7 µs), and the transaction spelling +131.2 to +137.1 µs. The single statement is also
the only one of the three that is one atomic step with no window between the read and the write.

## 4. Waits: where a statement's microseconds go

Every cell is `durability = relaxed`, so **there is no commit (fsync) wait**; a single session has no peer, so
**no lock or conflict wait** (0 refusals and 0 `TxnConflict` in the 96 reported runs); `SHOW META`'s p50
(50.2-50.5 µs in every cell) is the best a client statement does here: client, socket, frame, dispatch and a
reply. At `cores = 1`, 1,000 rows, p50:

| piece | A | B | what it is |
|---|---|---|---|
| `SHOW META` (control) | 50.2 µs | 50.4 µs | client and socket round trip, frame, dispatch, a reply: **94-98 % of a point `UPDATE`** |
| `UPDATE SET v = <lit>` by pk | 51.4 µs | 53.2 µs | the floor plus 1.2 µs (A) or 3.0 µs (B) of statement body (the reply of `SHOW META` is the larger, so the body is more than this difference) |
| `UPDATE SET v = v + 1` by pk | - | 55.1 µs | B's literal plus 1.7 µs of evaluation |
| `SET v = CASE ...` by pk | - | 61.2 µs | B's literal plus 7.8 µs |
| `SELECT v` then `UPDATE` | - | 125.7 µs | two round trips: 65.6 µs and 58.2 µs, the second one 3 µs dearer than a single `UPDATE` (a literal after a read, same client) |
| the same four-statement transaction | - | 191.3 µs | four round trips |

Not measurable on this engine today: parse, compile, evaluate and write as separate shares of the 1.2-3.0 µs
statement body (no per-stage timers, no `perf`); synced commit (`strict`, `group`) was not run, where the
fsync is about 95 % of an update and a 1.7 µs change would be invisible.

## 5. What this says about the engine

- **The literal path paid for generality.** A bare literal now goes through an expression tree (parse,
  type, evaluate) like any other value, and it costs 1.6-1.9 µs a statement, 3 % of a point `UPDATE`
  and 0.8-2 % of an every-row one. That is the price of making BJ-R1's grammar one grammar. The
  work order's "expectation is flat" (section 5) was wrong by this much. **Which lever recovers it is not
  known**: 0.65-0.95 µs of it arrives before any evaluator exists (section 1.2: parse and type), so a fold
  of a constant expression to its value at compile (BJ-Q10 already folds one for a foreign-key column) could
  recover only the rest, and a parser fast path for a lone literal is the other candidate. Neither is built
  or measured here, and the choice is a design question this measurement does not decide.
- **An expression is cheap next to the client.** `v + 1` costs about 1.7 µs a statement and 110-130 ns a
  row; the second round trip it saves costs 70 µs. For a read-modify-write the engine is no longer the
  bottleneck; the client is, and the manual's "one lost-update-free step" is also the fast one (BJ-R2/R9).
- **`CASE` is the outlier.** 7.4-7.9 µs a point statement (+14 %) and 256-330 ns a row is 4.5-4.7 and 2.2
  times `v + 1`. The statement text is longer, the tree larger and there are three operators and a branch;
  which of them dominates is not known without a profile.
- **The subquery's run-once is real** (section 2): a per-row evaluation was the design's one `[quiet-wrong]`
  alternative (BJ-Q6), and the cost shape excludes it.
- **No other statement moved.** `INSERT`, `DELETE`, `SELECT` and `SHOW META` are inside their floors in
  all six cells: the lexer's six new tokens, the fingerprint tags and the parser's `-` rule did not cost
  a statement that uses none of them.
- **BA's `SHOW META` is 5.15 µs dearer than before it** (section 1.2, `080cd555` vs A, 12 runs). Not
  BJ's; recorded because the control statement of every A/B in this file sits on it.

## 6. What could not be resolved

- **The every-row literal `UPDATE`, +0.8 to +2.0 % at p50 in six of six cells** (replicate: five of six).
  The CI excludes zero in four of the six cells and none beats its cell's floor (1.0-4.6 %, set by the
  replicate's own 1-1.5 % per-run scatter, which includes a systematic +0.4 to +1.8 % for the second relation). Stated
  as measured, not explained; at 10,000 rows (9.8-13.3 ms a statement) a +10 ns per row is below what 16 runs
  can pin.
- **A 0.2-0.45 s stall lands in one arm per server per run** and moves that arm's mean and
  throughput, not its p50. `contention_carve_longest_us` reads 220-450 ms in **all 20 server-runs of the
  diagnostic, A and B alike** (`archive/.../diag/`), and `contention_checkpoint_runs` stays 0; where
  it lands differs: at `cores = 1`, 1,000 rows it fell in the `INSERT` arm in 13 of 16 B runs and 3 of 16 A runs
  (diagnostic: B 9 of 10, A 2 of 10, at statement 1,400-2,450 of 3,000), at `cores = 2`, 1,000 rows in 13 of 16 B runs and 8 of 16 A runs. **The
  driver's `ops / elapsed` throughput of the `INSERT` arm at 1,000 rows (-70 %, -15 %) is that stall, not an
  `INSERT` cost**: the p50 delta of the same arm is +0.25 and +0.40 µs. It is a carve (BA-S13's area, the
  `contention_carve_*` counters in `SHOW META`), and *why* B's lands there more often is not resolved: the arms before it
  (the every-row updates) do the same work on both, and the 10-run diagnostic was not designed to find a
  cause.
- **Every-row statements at 10,000 rows carry a tail**: p99 of 55-808 ms in the six arms of the six
  cells (p50 9.8-13.5 ms), A and B alike, and their means and the driver's `ops / elapsed` are those
  tails (the driver's throughput delta of the every-row arm reads +20.8 % and +215.8 % for the
  replicate at `cores = 1`, 10,000 rows, against a p50 delta of +1.0 % and +1.4 %): the matrix uses p50.
- **The stage bisection** has 12 runs per pair and intervals of 0.6-1.7 µs; its steps overlap (section 1.2).
- **Other sessions' load.** Other-CPU busy per cell: median 3.3 / 2.8 / 17.3 % at `cores = 1` (200 / 1,000 / 10,000
  rows; the 10,000-row cell ran next to another session's benchmark on CPUs 6-7) and 8.5 / 5.6 / 4.9 % at
  `cores = 2`. The `SHOW META` p50 is 50.2-50.5 µs in every cell, so the floor of the round trip did not move,
  and the strict subset gives the same deltas; but a neighbour on the shared L3 is not excluded.
- **Part 2's arms run on different relations** (one per arm, loaded the same way) and Part 1's arms in a
  fixed order after each other; the block rotation shares drift, not the position of a relation in the file.

## Not run

- **The suite.** Suite: not executed in this session.
- **Synced durability** (`strict`, `group`), where the commit is about 95 % of an update; `cores` above 2;
  more than one client (the `SET v = v + 1` two-core rig is a correctness cell of BJ-S4, not measured here);
  a lock or deadlock wait.
- **A PostgreSQL floor** for this shape: no twin exists (`tools/pg_*.py` has none for expression `UPDATE`), so
  the floor was not measured. PostgreSQL is only a floor (bench rule 4) and the baseline above is this
  engine's own previous commit.
- **Shapes beyond `int64` and one column**: `DECIMAL`, `DATE`, `varchar` and spilled values, `NOW()`, the
  scalar functions, `COALESCE`/`CAST`/`SET (a, b) = (x, y)`, a subquery in a point statement, an expression
  into an indexed, Cabin-managed, foreign-key or assertion-checked column (the relations here have none of
  them), a heap relation (suspended), and relations over 10,000 rows.
- **Attribution inside a stage** (no `perf`).
- **A re-measure after a fix**: a lone-literal fold (section 5) is not built.

## Baseline for the next run

The next measurement of this shape reads against **`v2.7.0-756-g8e37baf4` (this file)**:
point `UPDATE SET v = <lit> WHERE id = k` at `cores = 1`, 1,000 rows, B's p50 53.2 µs (pooled), A's 51.4 µs;
every-row at 10,000 rows 9.88 ms (B, `cores = 1`); `SET v = v + 1` 55.1 µs, 1,068 ns per row; round trip pair
125.7 µs against 55.1 µs. The drivers and the discard rule are in the archive.

## Appendix A. Every cell

Each cell: the runs used, host state, Part 1 (A and B percentiles, then deltas and the in-run noise floor),
Part 2 and Part 3 (B only). `ops` of an arm = runs x statements; `n` is that count. Latencies in µs or
ms, as the cell says. `throughput (median of runs)` is the driver's `ops / elapsed`
and **carries the stalls of section 6**; the p50-derived throughput is in the sections above. Deltas
are B - A.

### cores = 1, 200 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 2.06-6.72, after 1.98-6.00; other-CPU busy over the run 2.3%-9.0% (median 3.3%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 40.0 µs | 50.5 µs | 51.4 µs | 67.6 µs | 85.9 µs | 54.2 µs (sd 1.6) | 18,271 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 43.3 µs | 52.3 µs | 53.2 µs | 68.2 µs | 84.0 µs | 55.8 µs (sd 0.9) | 17,595 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 39.6 µs | 50.6 µs | 51.3 µs | 66.6 µs | 81.9 µs | 54.2 µs (sd 1.6) | 18,188 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 43.4 µs | 52.4 µs | 53.1 µs | 67.7 µs | 82.8 µs | 56.2 µs (sd 1.1) | 17,451 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 43.0 µs | 51.3 µs | 52.1 µs | 67.6 µs | 83.7 µs | 55.1 µs (sd 1.2) | 17,874 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 44.4 µs | 53.5 µs | 54.1 µs | 69.4 µs | 85.3 µs | 57.6 µs (sd 1.2) | 17,128 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 223.6 µs | 230.4 µs | 238.4 µs | 300.4 µs | 347.6 µs | 246.1 µs (sd 1.8) | 4,055 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 225.1 µs | 232.2 µs | 240.6 µs | 319.8 µs | 384.7 µs | 252.5 µs (sd 8.8) | 3,960 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 220.7 µs | 232.3 µs | 240.9 µs | 333.3 µs | 404.0 µs | 253.1 µs (sd 9.1) | 3,984 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 224.7 µs | 232.5 µs | 241.2 µs | 319.1 µs | 371.3 µs | 250.6 µs (sd 6.3) | 4,013 stmt/s |
| INSERT, pk named | A | 48,000 | 41.1 µs | 48.5 µs | 49.2 µs | 64.8 µs | 83.3 µs | 52.3 µs (sd 1.5) | 18,887 stmt/s |
| INSERT, pk named | B | 48,000 | 40.8 µs | 48.6 µs | 49.3 µs | 64.8 µs | 80.7 µs | 52.3 µs (sd 0.8) | 18,819 stmt/s |
| SELECT by pk (control) | A | 48,000 | 58.0 µs | 64.5 µs | 65.5 µs | 84.4 µs | 102.2 µs | 69.3 µs (sd 1.6) | 14,317 stmt/s |
| SELECT by pk (control) | B | 48,000 | 57.6 µs | 64.4 µs | 65.4 µs | 84.5 µs | 102.1 µs | 69.2 µs (sd 1.8) | 14,302 stmt/s |
| DELETE by pk | A | 48,000 | 38.5 µs | 48.0 µs | 48.8 µs | 63.7 µs | 75.0 µs | 51.4 µs (sd 1.6) | 19,413 stmt/s |
| DELETE by pk | B | 48,000 | 39.1 µs | 48.0 µs | 48.8 µs | 62.8 µs | 73.0 µs | 51.8 µs (sd 1.3) | 19,131 stmt/s |
| SHOW META (control) | A | 48,000 | 44.0 µs | 49.6 µs | 50.3 µs | 63.0 µs | 72.7 µs | 52.4 µs (sd 0.8) | 18,934 stmt/s |
| SHOW META (control) | B | 48,000 | 42.3 µs | 49.5 µs | 50.2 µs | 62.7 µs | 72.0 µs | 52.4 µs (sd 0.5) | 18,945 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.80 µs [+1.50 µs, +2.00 µs] | +3.52 % [+2.91, +3.88] | +1.64 µs [+0.87 µs, +2.25 µs] | -3.86 % [-4.33, -1.81] | **B slower** (+3.5 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.80 µs [+1.50 µs, +2.00 µs] | +3.51 % [+2.91, +3.88] | +1.93 µs [+0.68 µs, +2.96 µs] | -4.99 % [-5.62, -1.83] | **B slower** (+3.5 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, w = lit, by pk | +2.15 µs [+1.60 µs, +2.30 µs] | +4.13 % [+3.08, +4.48] | +2.51 µs [+1.83 µs, +3.18 µs] | -4.76 % [-5.81, -2.75] | **B slower** (+4.1 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, every row | +1.90 µs [-0.10 µs, +4.20 µs] | +0.79 % [-0.04, +1.72] | +6.46 µs [+2.94 µs, +11.37 µs] | -1.87 % [-3.04, -0.20] | no change resolved |
| UPDATE SET v = lit, every row (replicate) | -0.25 µs [-1.70 µs, +2.40 µs] | -0.10 % [-0.71, +0.99] | -2.52 µs [-5.52 µs, +0.43 µs] | +0.86 % [-0.53, +2.21] | no change resolved |
| INSERT, pk named | +0.15 µs [-0.10 µs, +0.30 µs] | +0.30 % [-0.20, +0.61] | +0.01 µs [-0.97 µs, +0.86 µs] | -1.18 % [-2.72, +1.62] | no change resolved |
| SELECT by pk (control) | -0.10 µs [-0.60 µs, +0.30 µs] | -0.15 % [-0.92, +0.46] | -0.09 µs [-0.62 µs, +0.43 µs] | +0.15 % [-1.19, +0.67] | no change resolved |
| DELETE by pk | +0.00 µs [-0.30 µs, +0.10 µs] | +0.00 % [-0.61, +0.21] | +0.36 µs [-0.59 µs, +1.13 µs] | -1.79 % [-2.39, +0.37] | no change resolved |
| SHOW META (control) | -0.10 µs [-0.30 µs, +0.05 µs] | -0.20 % [-0.60, +0.10] | -0.01 µs [-0.34 µs, +0.31 µs] | -0.15 % [-0.49, +0.73] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ -0.20 % [-0.60, +0.10]; replicate (second relation, same binary, per run, both arms pooled) point median -0.19 %, robust sd 0.28 % (plain sd 0.29 %); range median +0.44 %, robust sd 1.52 % (plain sd 1.56 %). Floor used: point 0.74 %, range 3.48 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 44.1 µs | 52.5 µs | 53.3 µs | 68.2 µs | 80.2 µs | 60.2 µs (sd 14.5) | 17,452 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 45.0 µs | 54.4 µs | 55.0 µs | 69.8 µs | 81.5 µs | 57.5 µs (sd 0.6) | 17,062 stmt/s | +1.75 µs [+1.60 µs, +1.90 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 45.0 µs | 54.1 µs | 54.7 µs | 69.8 µs | 81.1 µs | 58.8 µs (sd 1.7) | 16,790 stmt/s | +1.50 µs [+1.30 µs, +1.60 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 50.6 µs | 60.3 µs | 61.0 µs | 77.7 µs | 90.0 µs | 65.1 µs (sd 1.3) | 15,139 stmt/s | +7.85 µs [+7.70 µs, +8.00 µs] | - | - |

range (`WHERE id >= 1`, every one of the 200 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 223.6 µs | 231.7 µs | 241.8 µs | 364.8 µs | 905.4 µs | 270.2 µs (sd 12.9) | 3,727 stmt/s = 745,380 rows/s | - | 1209 ns | - |
| `SET v = v + 1` | 1,920 | 241.4 µs | 255.3 µs | 266.7 µs | 396.9 µs | 1,057 µs | 297.8 µs (sd 14.3) | 3,395 stmt/s = 679,010 rows/s | +25.95 µs [+24.70 µs, +28.10 µs] | 1334 ns | +130 ns [+123, +140] |
| `SET v = w, w = v` | 1,920 | 239.1 µs | 247.7 µs | 257.4 µs | 349.0 µs | 458.8 µs | 273.0 µs (sd 26.0) | 3,774 stmt/s = 754,730 rows/s | +15.00 µs [+13.30 µs, +18.40 µs] | 1287 ns | +75 ns [+66, +92] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 282.8 µs | 293.4 µs | 306.9 µs | 502.0 µs | 1,157 µs | 351.7 µs (sd 21.0) | 2,880 stmt/s = 575,960 rows/s | +65.90 µs [+64.40 µs, +67.70 µs] | 1534 ns | +330 ns [+322, +338] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 259.6 µs | 274.4 µs | 286.9 µs | 410.4 µs | 1,531 µs | 324.7 µs (sd 24.7) | 3,137 stmt/s = 627,330 rows/s | +47.40 µs [+44.40 µs, +48.20 µs] | 1434 ns | +237 ns [+222, +241] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 44.0 µs | 54.3 µs | 54.9 µs | 70.9 µs | 87.9 µs | 77.5 µs (sd 31.1) | 16,671 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 112.2 µs | 122.0 µs | 124.2 µs | 156.8 µs | 193.4 µs | 134.9 µs (sd 7.8) | 7,589 /s | +69.10 µs [+68.80 µs, +69.40 µs] |
|   of which the `SELECT v` | 48,000 | 55.9 µs | 63.9 µs | 64.8 µs | 85.4 µs | 106.8 µs | 69.0 µs (sd 2.6) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 48.7 µs | 57.1 µs | 57.9 µs | 75.3 µs | 93.7 µs | 65.1 µs (sd 6.9) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 172.2 µs | 181.8 µs | 186.9 µs | 226.8 µs | 271.7 µs | 243.4 µs (sd 27.3) | 4,045 /s | +131.25 µs [+130.70 µs, +132.00 µs] |


### cores = 1, 1,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 1.53-9.10, after 1.64-7.93; other-CPU busy over the run 2.2%-21.6% (median 2.8%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 40.0 µs | 50.5 µs | 51.4 µs | 66.9 µs | 83.7 µs | 53.9 µs (sd 1.0) | 18,275 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 43.3 µs | 52.3 µs | 53.2 µs | 68.4 µs | 85.2 µs | 56.0 µs (sd 1.9) | 17,656 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 41.1 µs | 50.6 µs | 51.4 µs | 66.2 µs | 81.3 µs | 54.4 µs (sd 1.4) | 18,121 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 43.8 µs | 52.3 µs | 53.1 µs | 68.2 µs | 82.7 µs | 56.5 µs (sd 0.9) | 17,357 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 42.9 µs | 51.3 µs | 52.0 µs | 67.1 µs | 81.5 µs | 54.9 µs (sd 1.0) | 17,879 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 45.5 µs | 53.6 µs | 54.2 µs | 68.8 µs | 84.4 µs | 57.3 µs (sd 1.0) | 17,070 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 903.1 µs | 938.0 µs | 964.0 µs | 1,120 µs | 1,460 µs | 993.6 µs (sd 27.2) | 1,016 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 914.0 µs | 947.1 µs | 964.8 µs | 1,106 µs | 1,399 µs | 998.1 µs (sd 14.1) | 1,006 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 900.5 µs | 937.2 µs | 959.1 µs | 1,099 µs | 1,500 µs | 993.2 µs (sd 22.3) | 1,009 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 910.0 µs | 946.8 µs | 972.0 µs | 2,330 µs | 3,374 µs | 1,164 µs (sd 19.4) | 861 stmt/s |
| INSERT, pk named | A | 48,000 | 37.9 µs | 48.5 µs | 49.2 µs | 64.2 µs | 78.1 µs | 79.9 µs (sd 55.2) | 18,738 stmt/s |
| INSERT, pk named | B | 48,000 | 36.6 µs | 48.7 µs | 49.4 µs | 64.0 µs | 77.3 µs | 164.2 µs (sd 58.2) | 5,289 stmt/s |
| SELECT by pk (control) | A | 48,000 | 57.6 µs | 64.9 µs | 67.8 µs | 88.1 µs | 108.6 µs | 75.6 µs (sd 16.1) | 13,935 stmt/s |
| SELECT by pk (control) | B | 48,000 | 59.5 µs | 64.9 µs | 67.9 µs | 88.6 µs | 107.1 µs | 71.3 µs (sd 1.7) | 13,959 stmt/s |
| DELETE by pk | A | 48,000 | 38.4 µs | 48.1 µs | 48.8 µs | 64.7 µs | 85.7 µs | 52.5 µs (sd 3.6) | 19,302 stmt/s |
| DELETE by pk | B | 48,000 | 39.8 µs | 48.2 µs | 48.9 µs | 64.3 µs | 80.7 µs | 51.6 µs (sd 2.8) | 19,433 stmt/s |
| SHOW META (control) | A | 48,000 | 44.5 µs | 49.5 µs | 50.2 µs | 62.9 µs | 74.8 µs | 52.4 µs (sd 0.9) | 19,069 stmt/s |
| SHOW META (control) | B | 48,000 | 44.2 µs | 49.7 µs | 50.4 µs | 63.2 µs | 75.0 µs | 52.6 µs (sd 1.0) | 18,955 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.60 µs [+1.40 µs, +2.20 µs] | +3.12 % [+2.71, +4.26] | +2.17 µs [+1.40 µs, +3.17 µs] | -3.79 % [-4.77, -2.26] | **B slower** (+3.1 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.55 µs [+1.40 µs, +2.10 µs] | +3.02 % [+2.62, +4.17] | +2.07 µs [+1.27 µs, +2.83 µs] | -4.15 % [-5.59, -1.47] | **B slower** (+3.0 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, w = lit, by pk | +2.15 µs [+1.70 µs, +2.60 µs] | +4.15 % [+3.28, +5.09] | +2.34 µs [+1.60 µs, +3.02 µs] | -4.71 % [-5.53, -2.28] | **B slower** (+4.1 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, every row | +8.60 µs [+0.45 µs, +11.60 µs] | +0.90 % [+0.05, +1.22] | +4.54 µs [-7.46 µs, +15.34 µs] | -0.56 % [-1.59, -0.16] | CI excludes 0, inside the floor (2.3 %): not resolved |
| UPDATE SET v = lit, every row (replicate) | +16.80 µs [+6.80 µs, +23.70 µs] | +1.77 % [+0.72, +2.52] | +171.33 µs [+160.93 µs, +181.76 µs] | -14.70 % [-16.52, -13.36] | CI excludes 0, inside the floor (2.3 %): not resolved |
| INSERT, pk named | +0.25 µs [+0.00 µs, +0.40 µs] | +0.51 % [+0.00, +0.81] | +84.31 µs [+26.06 µs, +132.44 µs] | -69.96 % [-72.54, -65.61] | no change resolved |
| SELECT by pk (control) | +0.00 µs [-0.20 µs, +0.50 µs] | +0.00 % [-0.30, +0.74] | -4.22 µs [-12.31 µs, +0.29 µs] | +0.01 % [-0.24, +0.17] | no change resolved |
| DELETE by pk | +0.00 µs [-0.10 µs, +0.20 µs] | +0.00 % [-0.20, +0.41] | -0.86 µs [-1.71 µs, -0.14 µs] | +0.80 % [-0.77, +2.93] | no change resolved |
| SHOW META (control) | +0.20 µs [+0.15 µs, +0.40 µs] | +0.40 % [+0.30, +0.80] | +0.23 µs [+0.03 µs, +0.43 µs] | -0.46 % [-0.89, -0.01] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ +0.40 % [+0.30, +0.80]; replicate (second relation, same binary, per run, both arms pooled) point median +0.00 %, robust sd 0.56 % (plain sd 0.49 %); range median +0.12 %, robust sd 1.11 % (plain sd 1.07 %). Floor used: point 1.12 %, range 2.33 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 43.9 µs | 52.6 µs | 53.3 µs | 68.0 µs | 80.9 µs | 58.8 µs (sd 9.7) | 17,448 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 45.7 µs | 54.4 µs | 55.0 µs | 70.5 µs | 84.2 µs | 59.5 µs (sd 7.1) | 16,960 stmt/s | +1.80 µs [+1.65 µs, +1.90 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 45.7 µs | 54.0 µs | 54.7 µs | 70.7 µs | 85.6 µs | 60.4 µs (sd 7.1) | 16,865 stmt/s | +1.40 µs [+1.20 µs, +1.60 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 50.7 µs | 60.3 µs | 61.1 µs | 77.3 µs | 90.2 µs | 65.4 µs (sd 2.3) | 15,177 stmt/s | +7.70 µs [+7.50 µs, +8.00 µs] | - | - |

range (`WHERE id >= 1`, every one of the 1,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 902.0 µs | 939.7 µs | 959.0 µs | 1,495 µs | 2,923 µs | 1,104 µs (sd 186.9) | 938 stmt/s = 937,600 rows/s | - | 959 ns | - |
| `SET v = v + 1` | 1,920 | 1,020 µs | 1,049 µs | 1,068 µs | 1,316 µs | 71.36 ms | 3,882 µs (sd 1221.4) | 244 stmt/s = 244,350 rows/s | +108.35 µs [+106.40 µs, +113.40 µs] | 1068 ns | +108 ns [+106, +113] |
| `SET v = w, w = v` | 1,920 | 983.3 µs | 1,012 µs | 1,031 µs | 1,229 µs | 6,404 µs | 1,315 µs (sd 622.3) | 883 stmt/s = 883,350 rows/s | +68.25 µs [+65.00 µs, +71.50 µs] | 1031 ns | +68 ns [+65, +72] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 1,177 µs | 1,219 µs | 1,243 µs | 1,754 µs | 4,114 µs | 1,621 µs (sd 858.6) | 738 stmt/s = 737,700 rows/s | +283.90 µs [+277.60 µs, +289.80 µs] | 1243 ns | +284 ns [+278, +290] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 1,044 µs | 1,075 µs | 1,095 µs | 1,394 µs | 3,761 µs | 1,490 µs (sd 972.8) | 843 stmt/s = 843,100 rows/s | +136.55 µs [+133.20 µs, +138.95 µs] | 1095 ns | +137 ns [+133, +139] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 44.9 µs | 54.3 µs | 55.0 µs | 72.2 µs | 88.7 µs | 77.7 µs (sd 34.8) | 17,183 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 114.7 µs | 122.4 µs | 125.3 µs | 163.6 µs | 200.1 µs | 132.5 µs (sd 10.0) | 7,656 /s | +69.80 µs [+69.50 µs, +70.00 µs] |
|   of which the `SELECT v` | 48,000 | 56.1 µs | 64.1 µs | 65.5 µs | 88.0 µs | 107.7 µs | 69.7 µs (sd 5.0) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 50.4 µs | 57.1 µs | 58.1 µs | 77.9 µs | 102.2 µs | 61.8 µs (sd 4.8) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 171.5 µs | 182.9 µs | 190.7 µs | 241.1 µs | 287.8 µs | 255.5 µs (sd 39.6) | 3,730 /s | +134.95 µs [+134.90 µs, +135.30 µs] |


### cores = 1, 10,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 1.94-6.32, after 2.44-5.09; other-CPU busy over the run 2.9%-20.2% (median 17.3%); 0 errors; gate wait median 21 s; processes matching cc1plus|cmake --build|ctest: 2 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 40.1 µs | 50.9 µs | 51.6 µs | 67.1 µs | 83.8 µs | 55.2 µs (sd 3.4) | 18,073 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 43.6 µs | 52.7 µs | 53.6 µs | 68.8 µs | 85.5 µs | 56.9 µs (sd 1.6) | 17,281 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 43.1 µs | 51.0 µs | 51.8 µs | 70.4 µs | 98.7 µs | 55.6 µs (sd 5.1) | 18,064 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 44.0 µs | 52.8 µs | 53.7 µs | 73.5 µs | 111.6 µs | 59.0 µs (sd 10.5) | 17,424 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 43.1 µs | 51.6 µs | 52.5 µs | 68.9 µs | 85.7 µs | 55.3 µs (sd 1.3) | 17,842 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 44.9 µs | 53.8 µs | 54.5 µs | 72.5 µs | 92.1 µs | 58.7 µs (sd 1.9) | 16,865 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 9,308 µs | 9,638 µs | 9,794 µs | 19.07 ms | 462.62 ms | 21.35 ms (sd 2359.8) | 48 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 9,496 µs | 9,754 µs | 9,877 µs | 16.83 ms | 394.77 ms | 17.55 ms (sd 673.1) | 58 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 9,275 µs | 9,618 µs | 9,779 µs | 19.43 ms | 808.29 ms | 35.67 ms (sd 3870.8) | 28 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 9,432 µs | 9,745 µs | 9,882 µs | 16.98 ms | 54.56 ms | 13.01 ms (sd 3107.0) | 87 stmt/s |
| INSERT, pk named | A | 48,000 | 40.1 µs | 48.6 µs | 49.4 µs | 70.6 µs | 94.1 µs | 54.6 µs (sd 5.2) | 18,662 stmt/s |
| INSERT, pk named | B | 48,000 | 41.8 µs | 48.6 µs | 49.5 µs | 71.5 µs | 92.6 µs | 54.9 µs (sd 8.1) | 18,824 stmt/s |
| SELECT by pk (control) | A | 48,000 | 59.0 µs | 64.0 µs | 64.9 µs | 81.9 µs | 99.6 µs | 68.7 µs (sd 2.5) | 14,552 stmt/s |
| SELECT by pk (control) | B | 48,000 | 57.5 µs | 64.2 µs | 65.0 µs | 84.1 µs | 102.7 µs | 69.1 µs (sd 3.1) | 14,568 stmt/s |
| DELETE by pk | A | 48,000 | 39.9 µs | 48.1 µs | 48.9 µs | 64.4 µs | 75.9 µs | 51.6 µs (sd 0.4) | 19,168 stmt/s |
| DELETE by pk | B | 48,000 | 38.0 µs | 48.2 µs | 49.0 µs | 64.5 µs | 76.3 µs | 51.8 µs (sd 1.1) | 19,167 stmt/s |
| SHOW META (control) | A | 48,000 | 42.1 µs | 49.5 µs | 50.2 µs | 63.9 µs | 73.4 µs | 52.7 µs (sd 1.0) | 18,882 stmt/s |
| SHOW META (control) | B | 48,000 | 42.0 µs | 49.5 µs | 50.2 µs | 63.0 µs | 71.6 µs | 52.9 µs (sd 3.1) | 18,981 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.90 µs [+1.65 µs, +2.40 µs] | +3.68 % [+3.18, +4.67] | +1.72 µs [-0.51 µs, +3.27 µs] | -4.07 % [-5.33, -3.17] | **B slower** (+3.7 % at p50, floor 0.6 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.80 µs [+1.50 µs, +2.40 µs] | +3.46 % [+2.92, +4.67] | +3.38 µs [+1.70 µs, +6.23 µs] | -3.99 % [-4.90, -2.90] | **B slower** (+3.5 % at p50, floor 0.6 %) |
| UPDATE SET v = lit, w = lit, by pk | +2.00 µs [+1.85 µs, +2.40 µs] | +3.83 % [+3.51, +4.59] | +3.41 µs [+2.36 µs, +4.54 µs] | -5.24 % [-7.53, -2.53] | **B slower** (+3.8 % at p50, floor 0.6 %) |
| UPDATE SET v = lit, every row | +96.55 µs [+45.80 µs, +153.40 µs] | +0.99 % [+0.73, +1.56] | -3,801 µs [-5,176 µs, -2,804 µs] | +20.76 % [+17.89, +23.17] | CI excludes 0, inside the floor (1.0 %): not resolved |
| UPDATE SET v = lit, every row (replicate) | +134.00 µs [+50.10 µs, +163.50 µs] | +1.37 % [+0.51, +1.66] | -22.66 ms [-25.46 ms, -19.65 ms] | +215.84 % [+130.46, +232.48] | **B slower** (+1.4 % at p50, floor 1.0 %) |
| INSERT, pk named | +0.05 µs [-0.10 µs, +0.20 µs] | +0.10 % [-0.20, +0.32] | +0.29 µs [-1.56 µs, +2.40 µs] | +0.75 % [-0.71, +2.01] | no change resolved |
| SELECT by pk (control) | +0.00 µs [-0.10 µs, +0.60 µs] | +0.00 % [-0.15, +0.93] | +0.46 µs [-0.73 µs, +1.53 µs] | -0.62 % [-1.75, +0.45] | no change resolved |
| DELETE by pk | +0.10 µs [-0.10 µs, +0.25 µs] | +0.20 % [-0.20, +0.51] | +0.21 µs [-0.41 µs, +0.87 µs] | -0.78 % [-1.54, +1.81] | no change resolved |
| SHOW META (control) | -0.05 µs [-0.20 µs, +0.20 µs] | -0.10 % [-0.40, +0.40] | +0.17 µs [-0.89 µs, +1.81 µs] | +0.34 % [+0.04, +1.27] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ -0.10 % [-0.40, +0.40]; replicate (second relation, same binary, per run, both arms pooled) point median +0.00 %, robust sd 0.28 % (plain sd 5.40 %); range median +0.04 %, robust sd 0.48 % (plain sd 0.54 %). Floor used: point 0.56 %, range 1.00 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 43.0 µs | 53.0 µs | 53.9 µs | 71.1 µs | 85.8 µs | 62.6 µs (sd 8.5) | 16,748 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 46.1 µs | 54.8 µs | 55.5 µs | 73.0 µs | 89.4 µs | 65.3 µs (sd 10.1) | 15,979 stmt/s | +1.65 µs [+1.50 µs, +1.80 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 46.0 µs | 54.5 µs | 55.2 µs | 72.6 µs | 86.9 µs | 65.7 µs (sd 13.7) | 16,207 stmt/s | +1.30 µs [+1.20 µs, +1.40 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 51.1 µs | 60.8 µs | 61.7 µs | 81.6 µs | 97.1 µs | 67.3 µs (sd 1.8) | 14,448 stmt/s | +7.80 µs [+7.70 µs, +7.90 µs] | - | - |

range (`WHERE id >= 1`, every one of the 10,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 8,721 µs | 8,956 µs | 9,221 µs | 16.20 ms | 406.08 ms | 19.46 ms (sd 3651.2) | 51 stmt/s = 513,500 rows/s | - | 922 ns | - |
| `SET v = v + 1` | 1,920 | 9,768 µs | 10.16 ms | 10.36 ms | 19.76 ms | 402.56 ms | 23.81 ms (sd 1411.0) | 42 stmt/s = 415,500 rows/s | +1,167 µs [+1,150 µs, +1,191 µs] | 1036 ns | +117 ns [+115, +119] |
| `SET v = w, w = v` | 1,920 | 9,407 µs | 9,689 µs | 10.04 ms | 16.61 ms | 69.35 ms | 14.19 ms (sd 1222.4) | 72 stmt/s = 723,500 rows/s | +737.65 µs [+673.50 µs, +808.70 µs] | 1004 ns | +74 ns [+67, +81] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 11.23 ms | 11.56 ms | 11.81 ms | 18.53 ms | 419.68 ms | 27.94 ms (sd 1453.1) | 36 stmt/s = 362,500 rows/s | +2,624 µs [+2,590 µs, +2,662 µs] | 1181 ns | +262 ns [+259, +266] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 9,848 µs | 10.25 ms | 10.53 ms | 18.48 ms | 405.35 ms | 21.06 ms (sd 1393.9) | 48 stmt/s = 483,500 rows/s | +1,283 µs [+1,250 µs, +1,318 µs] | 1053 ns | +128 ns [+125, +132] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 45.1 µs | 54.6 µs | 55.5 µs | 77.6 µs | 178.0 µs | 84.4 µs (sd 37.4) | 15,350 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 115.2 µs | 121.9 µs | 125.0 µs | 159.1 µs | 184.6 µs | 133.9 µs (sd 7.5) | 7,570 /s | +68.85 µs [+68.55 µs, +69.50 µs] |
|   of which the `SELECT v` | 48,000 | 55.1 µs | 63.7 µs | 65.0 µs | 86.1 µs | 101.3 µs | 69.7 µs (sd 3.1) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 51.7 µs | 57.0 µs | 58.1 µs | 77.2 µs | 90.3 µs | 63.3 µs (sd 5.3) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 172.0 µs | 182.1 µs | 188.5 µs | 236.3 µs | 273.3 µs | 207.7 µs (sd 24.4) | 5,069 /s | +132.15 µs [+131.10 µs, +133.00 µs] |


### cores = 2, 200 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 2.04-3.00, after 1.96-2.92; other-CPU busy over the run 2.9%-18.4% (median 8.5%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 43.3 µs | 51.8 µs | 52.8 µs | 71.2 µs | 88.0 µs | 57.5 µs (sd 7.6) | 17,634 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 48.1 µs | 53.8 µs | 54.5 µs | 72.8 µs | 85.8 µs | 57.7 µs (sd 1.2) | 17,124 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 46.0 µs | 51.7 µs | 52.6 µs | 70.0 µs | 81.0 µs | 56.3 µs (sd 1.8) | 17,560 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 47.2 µs | 53.7 µs | 54.4 µs | 71.7 µs | 83.4 µs | 58.3 µs (sd 1.4) | 16,949 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 44.7 µs | 52.7 µs | 53.5 µs | 70.4 µs | 79.5 µs | 57.0 µs (sd 1.2) | 17,199 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 48.7 µs | 54.6 µs | 55.2 µs | 72.6 µs | 82.6 µs | 58.7 µs (sd 1.0) | 16,639 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 276.5 µs | 296.9 µs | 308.6 µs | 359.7 µs | 409.9 µs | 313.9 µs (sd 2.9) | 3,170 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 280.5 µs | 301.6 µs | 314.4 µs | 371.5 µs | 413.1 µs | 319.6 µs (sd 4.6) | 3,124 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 282.1 µs | 303.4 µs | 317.2 µs | 365.9 µs | 418.5 µs | 321.6 µs (sd 6.8) | 3,116 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 287.5 µs | 305.5 µs | 318.1 µs | 376.1 µs | 471.2 µs | 325.8 µs (sd 8.8) | 3,076 stmt/s |
| INSERT, pk named | A | 48,000 | 45.0 µs | 49.0 µs | 49.7 µs | 68.2 µs | 83.0 µs | 53.8 µs (sd 1.6) | 18,381 stmt/s |
| INSERT, pk named | B | 48,000 | 43.7 µs | 49.1 µs | 49.9 µs | 68.9 µs | 83.7 µs | 53.2 µs (sd 1.2) | 18,538 stmt/s |
| SELECT by pk (control) | A | 48,000 | 60.2 µs | 65.6 µs | 66.7 µs | 86.6 µs | 99.3 µs | 70.3 µs (sd 0.8) | 14,068 stmt/s |
| SELECT by pk (control) | B | 48,000 | 60.4 µs | 65.6 µs | 66.6 µs | 86.1 µs | 100.0 µs | 70.7 µs (sd 2.9) | 14,148 stmt/s |
| DELETE by pk | A | 48,000 | 42.9 µs | 48.6 µs | 49.3 µs | 65.0 µs | 73.5 µs | 51.5 µs (sd 0.4) | 19,196 stmt/s |
| DELETE by pk | B | 48,000 | 43.9 µs | 48.6 µs | 49.4 µs | 65.4 µs | 74.0 µs | 52.0 µs (sd 0.7) | 19,046 stmt/s |
| SHOW META (control) | A | 48,000 | 45.0 µs | 49.7 µs | 50.4 µs | 64.2 µs | 72.8 µs | 52.7 µs (sd 1.0) | 18,875 stmt/s |
| SHOW META (control) | B | 48,000 | 42.4 µs | 49.8 µs | 50.5 µs | 65.2 µs | 73.7 µs | 53.0 µs (sd 0.9) | 18,797 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.65 µs [+1.35 µs, +2.20 µs] | +3.11 % [+2.55, +4.25] | +0.23 µs [-3.55 µs, +2.49 µs] | -2.80 % [-3.89, -1.99] | **B slower** (+3.1 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.80 µs [+1.30 µs, +2.20 µs] | +3.41 % [+2.46, +4.28] | +2.08 µs [+0.97 µs, +3.09 µs] | -3.14 % [-5.82, -2.14] | **B slower** (+3.4 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, w = lit, by pk | +1.90 µs [+1.30 µs, +2.30 µs] | +3.56 % [+2.41, +4.35] | +1.69 µs [+0.91 µs, +2.48 µs] | -3.16 % [-4.44, -1.02] | **B slower** (+3.6 % at p50, floor 0.7 %) |
| UPDATE SET v = lit, every row | +4.70 µs [+3.50 µs, +6.70 µs] | +1.53 % [+1.14, +2.16] | +5.66 µs [+3.02 µs, +8.34 µs] | -1.77 % [-2.59, -0.59] | CI excludes 0, inside the floor (4.6 %): not resolved |
| UPDATE SET v = lit, every row (replicate) | +4.70 µs [-1.40 µs, +6.20 µs] | +1.50 % [-0.45, +1.98] | +4.22 µs [-0.64 µs, +8.89 µs] | -1.74 % [-2.60, +0.12] | no change resolved |
| INSERT, pk named | +0.25 µs [-0.05 µs, +0.40 µs] | +0.50 % [+0.00, +0.81] | -0.58 µs [-1.22 µs, +0.13 µs] | +1.33 % [-0.25, +2.75] | no change resolved |
| SELECT by pk (control) | +0.05 µs [-0.50 µs, +0.10 µs] | +0.08 % [-0.75, +0.38] | +0.34 µs [-0.44 µs, +1.61 µs] | +0.13 % [-0.14, +1.06] | no change resolved |
| DELETE by pk | +0.10 µs [-0.10 µs, +0.30 µs] | +0.20 % [-0.20, +0.61] | +0.50 µs [+0.13 µs, +0.89 µs] | -0.59 % [-2.06, +0.11] | no change resolved |
| SHOW META (control) | +0.05 µs [-0.20 µs, +0.30 µs] | +0.10 % [-0.39, +0.59] | +0.31 µs [-0.33 µs, +0.94 µs] | -0.62 % [-1.19, +0.28] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ +0.10 % [-0.39, +0.59]; replicate (second relation, same binary, per run, both arms pooled) point median -0.19 %, robust sd 0.27 % (plain sd 0.89 %); range median +1.75 %, robust sd 1.43 % (plain sd 2.12 %). Floor used: point 0.73 %, range 4.61 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 47.1 µs | 53.7 µs | 54.3 µs | 70.9 µs | 80.6 µs | 57.4 µs (sd 1.3) | 17,172 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 49.0 µs | 55.2 µs | 55.8 µs | 73.1 µs | 82.5 µs | 59.0 µs (sd 0.9) | 16,722 stmt/s | +1.55 µs [+1.40 µs, +1.60 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 47.7 µs | 54.8 µs | 55.5 µs | 73.0 µs | 82.4 µs | 60.0 µs (sd 1.6) | 16,270 stmt/s | +1.15 µs [+1.00 µs, +1.20 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 54.8 µs | 60.9 µs | 61.7 µs | 81.1 µs | 90.3 µs | 66.1 µs (sd 2.2) | 15,123 stmt/s | +7.35 µs [+7.20 µs, +7.50 µs] | - | - |

range (`WHERE id >= 1`, every one of the 200 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 269.4 µs | 278.9 µs | 292.6 µs | 484.0 µs | 723.4 µs | 318.3 µs (sd 9.5) | 3,136 stmt/s = 627,220 rows/s | - | 1463 ns | - |
| `SET v = v + 1` | 1,920 | 290.8 µs | 302.0 µs | 316.0 µs | 479.6 µs | 645.4 µs | 339.1 µs (sd 7.9) | 2,959 stmt/s = 591,770 rows/s | +23.60 µs [+21.30 µs, +26.00 µs] | 1580 ns | +118 ns [+107, +130] |
| `SET v = w, w = v` | 1,920 | 282.8 µs | 291.7 µs | 304.5 µs | 374.6 µs | 448.6 µs | 310.8 µs (sd 4.2) | 3,215 stmt/s = 642,940 rows/s | +11.25 µs [+9.90 µs, +14.40 µs] | 1522 ns | +56 ns [+50, +72] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 324.0 µs | 335.7 µs | 351.5 µs | 467.2 µs | 1,003 µs | 373.0 µs (sd 14.9) | 2,726 stmt/s = 545,140 rows/s | +59.15 µs [+56.80 µs, +60.50 µs] | 1758 ns | +296 ns [+284, +302] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 310.6 µs | 322.6 µs | 338.3 µs | 470.5 µs | 555.4 µs | 356.4 µs (sd 5.6) | 2,808 stmt/s = 561,640 rows/s | +45.80 µs [+43.90 µs, +48.30 µs] | 1692 ns | +229 ns [+219, +242] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 48.3 µs | 55.1 µs | 55.8 µs | 73.6 µs | 82.7 µs | 93.5 µs (sd 30.0) | 9,218 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 118.4 µs | 124.1 µs | 126.6 µs | 154.3 µs | 173.0 µs | 137.7 µs (sd 12.7) | 7,523 /s | +70.55 µs [+70.50 µs, +71.10 µs] |
|   of which the `SELECT v` | 48,000 | 59.5 µs | 64.9 µs | 65.9 µs | 85.5 µs | 95.6 µs | 73.3 µs (sd 11.3) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 53.0 µs | 58.1 µs | 59.0 µs | 77.1 µs | 85.8 µs | 63.6 µs (sd 6.8) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 173.7 µs | 184.6 µs | 190.3 µs | 221.6 µs | 248.6 µs | 226.4 µs (sd 32.6) | 4,485 /s | +134.10 µs [+133.60 µs, +135.00 µs] |


### cores = 2, 1,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 1.81-3.72, after 2.08-3.62; other-CPU busy over the run 1.9%-28.3% (median 5.6%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 43.6 µs | 51.7 µs | 52.7 µs | 70.6 µs | 82.3 µs | 55.4 µs (sd 0.5) | 17,699 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 45.3 µs | 53.8 µs | 54.5 µs | 72.1 µs | 83.4 µs | 57.3 µs (sd 0.7) | 17,125 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 43.8 µs | 51.6 µs | 52.5 µs | 70.3 µs | 82.4 µs | 64.5 µs (sd 32.0) | 17,494 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 48.4 µs | 53.8 µs | 54.4 µs | 71.6 µs | 84.5 µs | 58.8 µs (sd 2.3) | 16,845 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 44.8 µs | 52.5 µs | 53.4 µs | 70.7 µs | 82.5 µs | 57.4 µs (sd 2.0) | 17,182 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 48.9 µs | 54.6 µs | 55.3 µs | 73.2 µs | 84.1 µs | 63.3 µs (sd 16.0) | 16,651 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 1,200 µs | 1,287 µs | 1,313 µs | 1,448 µs | 1,746 µs | 1,334 µs (sd 25.1) | 751 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 1,230 µs | 1,305 µs | 1,332 µs | 1,456 µs | 1,661 µs | 1,348 µs (sd 24.6) | 745 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 1,256 µs | 1,301 µs | 1,326 µs | 1,527 µs | 2,139 µs | 1,626 µs (sd 693.3) | 727 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 1,278 µs | 1,324 µs | 1,352 µs | 2,360 µs | 3,353 µs | 1,600 µs (sd 533.1) | 681 stmt/s |
| INSERT, pk named | A | 48,000 | 39.6 µs | 48.8 µs | 49.7 µs | 69.4 µs | 92.0 µs | 101.4 µs (sd 41.0) | 9,463 stmt/s |
| INSERT, pk named | B | 48,000 | 39.6 µs | 49.1 µs | 50.3 µs | 69.5 µs | 92.4 µs | 127.7 µs (sd 46.4) | 7,984 stmt/s |
| SELECT by pk (control) | A | 48,000 | 57.4 µs | 65.7 µs | 69.1 µs | 91.8 µs | 108.2 µs | 72.6 µs (sd 1.8) | 13,740 stmt/s |
| SELECT by pk (control) | B | 48,000 | 59.9 µs | 66.0 µs | 69.4 µs | 93.1 µs | 112.1 µs | 73.3 µs (sd 3.2) | 13,699 stmt/s |
| DELETE by pk | A | 48,000 | 41.2 µs | 48.6 µs | 49.3 µs | 66.7 µs | 76.7 µs | 51.9 µs (sd 1.2) | 19,121 stmt/s |
| DELETE by pk | B | 48,000 | 42.4 µs | 48.6 µs | 49.4 µs | 66.4 µs | 76.3 µs | 52.4 µs (sd 1.2) | 18,897 stmt/s |
| SHOW META (control) | A | 48,000 | 42.2 µs | 49.9 µs | 50.6 µs | 67.6 µs | 83.1 µs | 58.2 µs (sd 21.7) | 18,835 stmt/s |
| SHOW META (control) | B | 48,000 | 43.3 µs | 49.8 µs | 50.6 µs | 69.0 µs | 88.8 µs | 54.3 µs (sd 5.6) | 18,782 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.70 µs [+1.30 µs, +2.40 µs] | +3.22 % [+2.63, +4.59] | +1.92 µs [+1.59 µs, +2.24 µs] | -3.64 % [-4.08, -2.31] | **B slower** (+3.2 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.90 µs [+1.40 µs, +2.50 µs] | +3.62 % [+2.61, +4.83] | -5.69 µs [-21.92 µs, +3.24 µs] | -2.39 % [-6.34, -0.72] | **B slower** (+3.6 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, w = lit, by pk | +2.10 µs [+1.50 µs, +2.50 µs] | +3.91 % [+2.82, +4.70] | +5.85 µs [+1.15 µs, +13.90 µs] | -3.17 % [-6.46, -1.76] | **B slower** (+3.9 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, every row | +24.60 µs [+17.20 µs, +29.25 µs] | +1.86 % [+1.18, +2.26] | +13.89 µs [+2.74 µs, +24.23 µs] | -1.28 % [-2.06, -0.35] | CI excludes 0, inside the floor (2.9 %): not resolved |
| UPDATE SET v = lit, every row (replicate) | +22.35 µs [+9.80 µs, +44.80 µs] | +1.69 % [+0.74, +3.35] | -25.46 µs [-283.62 µs, +118.18 µs] | -4.66 % [-7.46, -3.34] | CI excludes 0, inside the floor (2.9 %): not resolved |
| INSERT, pk named | +0.40 µs [+0.10 µs, +0.90 µs] | +0.81 % [+0.20, +1.81] | +26.30 µs [-12.31 µs, +64.47 µs] | -15.33 % [-57.85, +29.35] | CI excludes 0, inside the floor (1.1 %): not resolved |
| SELECT by pk (control) | +0.35 µs [-0.20 µs, +0.80 µs] | +0.51 % [-0.29, +1.17] | +0.72 µs [-0.11 µs, +2.02 µs] | -0.20 % [-0.85, +0.27] | no change resolved |
| DELETE by pk | +0.05 µs [-0.35 µs, +0.40 µs] | +0.10 % [-0.61, +0.82] | +0.51 µs [+0.11 µs, +0.89 µs] | -1.22 % [-2.41, +0.05] | no change resolved |
| SHOW META (control) | +0.00 µs [-0.40 µs, +0.40 µs] | +0.00 % [-0.79, +0.79] | -3.87 µs [-12.11 µs, +0.43 µs] | -0.21 % [-0.71, +0.77] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ +0.00 % [-0.79, +0.79]; replicate (second relation, same binary, per run, both arms pooled) point median -0.28 %, robust sd 0.41 % (plain sd 0.58 %); range median +1.02 %, robust sd 0.96 % (plain sd 1.03 %). Floor used: point 1.11 %, range 2.95 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 48.9 µs | 53.9 µs | 54.6 µs | 83.8 µs | 107.0 µs | 74.1 µs (sd 30.9) | 16,896 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 48.7 µs | 55.5 µs | 56.2 µs | 85.1 µs | 107.9 µs | 61.7 µs (sd 9.0) | 16,491 stmt/s | +1.70 µs [+1.50 µs, +1.70 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 48.5 µs | 55.1 µs | 55.8 µs | 85.3 µs | 109.1 µs | 73.3 µs (sd 31.5) | 16,196 stmt/s | +1.25 µs [+1.10 µs, +1.40 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 53.9 µs | 61.4 µs | 62.3 µs | 95.4 µs | 120.5 µs | 78.2 µs (sd 27.9) | 14,856 stmt/s | +7.75 µs [+7.60 µs, +7.90 µs] | - | - |

range (`WHERE id >= 1`, every one of the 1,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 1,133 µs | 1,176 µs | 1,202 µs | 2,076 µs | 2,995 µs | 1,715 µs (sd 1214.1) | 773 stmt/s = 772,850 rows/s | - | 1202 ns | - |
| `SET v = v + 1` | 1,920 | 1,241 µs | 1,287 µs | 1,316 µs | 1,926 µs | 8,715 µs | 3,358 µs (sd 1492.2) | 267 stmt/s = 267,300 rows/s | +109.15 µs [+106.40 µs, +118.50 µs] | 1316 ns | +109 ns [+106, +118] |
| `SET v = w, w = v` | 1,920 | 1,192 µs | 1,243 µs | 1,269 µs | 1,941 µs | 7,289 µs | 2,097 µs (sd 1394.8) | 689 stmt/s = 688,750 rows/s | +65.30 µs [+60.00 µs, +75.20 µs] | 1269 ns | +65 ns [+60, +75] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 1,380 µs | 1,434 µs | 1,468 µs | 2,559 µs | 3,769 µs | 1,938 µs (sd 1241.5) | 641 stmt/s = 640,850 rows/s | +265.85 µs [+256.80 µs, +273.80 µs] | 1468 ns | +266 ns [+257, +274] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 1,270 µs | 1,315 µs | 1,340 µs | 1,562 µs | 2,410 µs | 1,562 µs (sd 495.3) | 717 stmt/s = 717,400 rows/s | +135.60 µs [+129.20 µs, +144.00 µs] | 1340 ns | +136 ns [+129, +144] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 46.8 µs | 55.3 µs | 56.1 µs | 85.1 µs | 109.3 µs | 86.9 µs (sd 43.9) | 16,539 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 116.1 µs | 124.5 µs | 128.4 µs | 188.7 µs | 237.7 µs | 138.8 µs (sd 17.7) | 7,462 /s | +71.50 µs [+71.20 µs, +72.10 µs] |
|   of which the `SELECT v` | 48,000 | 59.5 µs | 65.1 µs | 66.8 µs | 99.5 µs | 124.3 µs | 72.2 µs (sd 8.8) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 50.9 µs | 58.1 µs | 59.3 µs | 87.4 µs | 114.2 µs | 65.6 µs (sd 9.6) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 173.4 µs | 185.8 µs | 193.9 µs | 281.7 µs | 340.7 µs | 249.4 µs (sd 29.7) | 3,879 /s | +137.00 µs [+136.60 µs, +137.80 µs] |


### cores = 2, 10,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 2.30-3.31, after 2.39-3.57; other-CPU busy over the run 2.4%-23.3% (median 4.9%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 46.2 µs | 52.2 µs | 53.3 µs | 71.7 µs | 88.3 µs | 57.5 µs (sd 3.1) | 17,342 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 47.8 µs | 54.1 µs | 54.8 µs | 73.7 µs | 90.1 µs | 58.7 µs (sd 1.4) | 16,854 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 46.7 µs | 52.1 µs | 53.1 µs | 70.8 µs | 84.8 µs | 56.1 µs (sd 1.3) | 17,533 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 48.6 µs | 54.0 µs | 54.7 µs | 72.8 µs | 85.8 µs | 57.9 µs (sd 0.9) | 16,992 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 47.3 µs | 52.9 µs | 53.7 µs | 70.2 µs | 80.1 µs | 56.8 µs (sd 1.1) | 17,265 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 49.4 µs | 54.8 µs | 55.6 µs | 73.3 µs | 82.3 µs | 58.9 µs (sd 0.9) | 16,710 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 11.85 ms | 12.91 ms | 13.32 ms | 24.13 ms | 383.77 ms | 25.00 ms (sd 2342.9) | 40 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 12.15 ms | 13.04 ms | 13.47 ms | 23.21 ms | 334.98 ms | 25.80 ms (sd 4176.4) | 40 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 12.37 ms | 12.91 ms | 13.39 ms | 93.65 ms | 396.24 ms | 29.72 ms (sd 3791.3) | 34 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 12.63 ms | 13.18 ms | 13.42 ms | 43.10 ms | 272.69 ms | 23.64 ms (sd 2433.2) | 44 stmt/s |
| INSERT, pk named | A | 48,000 | 39.6 µs | 49.1 µs | 50.3 µs | 73.1 µs | 112.6 µs | 73.5 µs (sd 33.2) | 17,762 stmt/s |
| INSERT, pk named | B | 48,000 | 42.0 µs | 49.4 µs | 50.8 µs | 74.4 µs | 111.6 µs | 97.0 µs (sd 90.0) | 17,697 stmt/s |
| SELECT by pk (control) | A | 48,000 | 61.4 µs | 65.6 µs | 66.7 µs | 88.7 µs | 116.8 µs | 70.8 µs (sd 4.0) | 14,150 stmt/s |
| SELECT by pk (control) | B | 48,000 | 59.0 µs | 65.7 µs | 66.7 µs | 88.1 µs | 112.4 µs | 70.5 µs (sd 3.7) | 14,141 stmt/s |
| DELETE by pk | A | 48,000 | 41.0 µs | 48.5 µs | 49.5 µs | 69.3 µs | 87.4 µs | 53.3 µs (sd 4.4) | 18,862 stmt/s |
| DELETE by pk | B | 48,000 | 41.6 µs | 48.7 µs | 49.7 µs | 69.3 µs | 86.8 µs | 53.2 µs (sd 4.2) | 18,977 stmt/s |
| SHOW META (control) | A | 48,000 | 42.6 µs | 49.7 µs | 50.4 µs | 68.1 µs | 84.1 µs | 53.4 µs (sd 4.1) | 18,929 stmt/s |
| SHOW META (control) | B | 48,000 | 42.8 µs | 49.7 µs | 50.5 µs | 67.2 µs | 80.9 µs | 53.4 µs (sd 4.0) | 18,887 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +1.60 µs [+1.30 µs, +1.80 µs] | +3.02 % [+2.43, +3.59] | +1.27 µs [-0.00 µs, +2.21 µs] | -2.41 % [-5.11, -1.34] | **B slower** (+3.0 % at p50, floor 1.2 %) |
| UPDATE SET v = lit, by pk (replicate) | +1.55 µs [+1.30 µs, +2.20 µs] | +2.94 % [+2.43, +4.22] | +1.73 µs [+1.24 µs, +2.26 µs] | -2.54 % [-4.42, -1.93] | **B slower** (+2.9 % at p50, floor 1.2 %) |
| UPDATE SET v = lit, w = lit, by pk | +1.90 µs [+1.40 µs, +2.30 µs] | +3.55 % [+2.62, +4.25] | +2.11 µs [+1.36 µs, +2.85 µs] | -3.13 % [-5.01, -1.64] | **B slower** (+3.6 % at p50, floor 1.2 %) |
| UPDATE SET v = lit, every row | +261.75 µs [-15.35 µs, +427.90 µs] | +1.97 % [-0.11, +3.44] | +790.53 µs [-1,784 µs, +3,787 µs] | +1.96 % [-10.60, +13.78] | no change resolved |
| UPDATE SET v = lit, every row (replicate) | +92.60 µs [-67.40 µs, +315.70 µs] | +0.70 % [-0.50, +2.35] | -6,073 µs [-8,386 µs, -3,646 µs] | +32.07 % [+9.89, +39.79] | no change resolved |
| INSERT, pk named | +0.60 µs [-0.10 µs, +1.10 µs] | +1.19 % [-0.20, +2.23] | +23.52 µs [-18.60 µs, +79.21 µs] | -1.46 % [-27.34, +26.43] | no change resolved |
| SELECT by pk (control) | -0.05 µs [-0.40 µs, +0.50 µs] | -0.07 % [-0.60, +0.76] | -0.24 µs [-0.69 µs, +0.25 µs] | +0.49 % [-0.17, +1.27] | no change resolved |
| DELETE by pk | +0.10 µs [-0.30 µs, +0.60 µs] | +0.20 % [-0.61, +1.23] | -0.05 µs [-0.51 µs, +0.42 µs] | +0.16 % [-0.92, +1.69] | no change resolved |
| SHOW META (control) | +0.15 µs [-0.40 µs, +0.40 µs] | +0.30 % [-0.79, +0.79] | +0.02 µs [-0.24 µs, +0.26 µs] | -0.31 % [-0.65, +0.18] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ +0.30 % [-0.79, +0.79]; replicate (second relation, same binary, per run, both arms pooled) point median -0.36 %, robust sd 0.42 % (plain sd 0.58 %); range median +1.06 %, robust sd 1.15 % (plain sd 1.34 %). Floor used: point 1.21 %, range 3.36 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 48.4 µs | 54.2 µs | 55.1 µs | 76.1 µs | 102.5 µs | 59.5 µs (sd 4.4) | 16,762 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 52.0 µs | 55.8 µs | 56.7 µs | 77.2 µs | 101.2 µs | 62.4 µs (sd 5.9) | 16,264 stmt/s | +1.65 µs [+1.55 µs, +1.75 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 50.1 µs | 55.4 µs | 56.3 µs | 76.5 µs | 101.6 µs | 62.7 µs (sd 5.2) | 16,054 stmt/s | +1.30 µs [+1.20 µs, +1.40 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 55.7 µs | 61.6 µs | 62.6 µs | 84.5 µs | 107.9 µs | 70.9 µs (sd 5.0) | 14,102 stmt/s | +7.50 µs [+7.35 µs, +7.70 µs] | - | - |

range (`WHERE id >= 1`, every one of the 10,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 11.05 ms | 11.37 ms | 11.50 ms | 17.71 ms | 327.54 ms | 19.60 ms (sd 1586.7) | 51 stmt/s = 510,000 rows/s | - | 1150 ns | - |
| `SET v = v + 1` | 1,920 | 12.07 ms | 12.52 ms | 12.63 ms | 18.24 ms | 339.75 ms | 23.57 ms (sd 1351.5) | 43 stmt/s = 427,000 rows/s | +1,153 µs [+1,074 µs, +1,194 µs] | 1263 ns | +115 ns [+107, +119] |
| `SET v = w, w = v` | 1,920 | 11.66 ms | 12.06 ms | 12.18 ms | 15.64 ms | 61.52 ms | 16.22 ms (sd 1912.7) | 65 stmt/s = 650,000 rows/s | +706.20 µs [+666.70 µs, +718.30 µs] | 1218 ns | +71 ns [+67, +72] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 13.45 ms | 13.91 ms | 14.03 ms | 56.61 ms | 346.54 ms | 29.58 ms (sd 4099.1) | 35 stmt/s = 348,500 rows/s | +2,557 µs [+2,525 µs, +2,580 µs] | 1403 ns | +256 ns [+252, +258] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 12.19 ms | 12.62 ms | 12.73 ms | 18.72 ms | 319.95 ms | 20.43 ms (sd 1452.9) | 48 stmt/s = 483,000 rows/s | +1,229 µs [+1,208 µs, +1,264 µs] | 1273 ns | +123 ns [+121, +126] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 46.0 µs | 55.5 µs | 56.3 µs | 74.6 µs | 86.1 µs | 92.7 µs (sd 64.8) | 16,538 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 116.8 µs | 123.7 µs | 126.3 µs | 157.5 µs | 191.0 µs | 170.4 µs (sd 47.7) | 7,405 /s | +70.10 µs [+69.60 µs, +70.50 µs] |
|   of which the `SELECT v` | 48,000 | 57.1 µs | 64.8 µs | 65.9 µs | 86.1 µs | 99.2 µs | 80.5 µs (sd 29.1) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 51.0 µs | 57.8 µs | 58.7 µs | 78.1 µs | 92.7 µs | 89.1 µs (sd 34.8) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 174.9 µs | 184.0 µs | 188.8 µs | 225.4 µs | 273.4 µs | 217.5 µs (sd 30.0) | 4,999 /s | +132.30 µs [+131.80 µs, +133.40 µs] |

