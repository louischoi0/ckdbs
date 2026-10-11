# The nested-hoist change's interleaved A/B overhead (`v2.7.0-749-g23e53bed`)

**Thesis.** On `worktree-fix-nested-uncorrelated-subquery` at `23e53bed` (`v2.7.0-749-g23e53bed`),
the one cost of the change that this measurement resolves is **on the correlated sub-chain
evaluation: about +35 ns per outer row** (+7.2 µs at 200 outer rows, +38.3 µs at 1,000,
+282 µs at 10,000 on a statement whose median is 552 µs, 3.08 ms and 30.6 ms: **+0.9 to +1.3 %**),
with B slower in 7 of 7 runs at every size, and the same sign and size when the two binaries
swap slots (section 6). That is larger than the "at most a few ns" expected of an early-out on an
empty vector. **Everything else is inside a floor of about 1 µs (about 1 %) per statement**: a
point statement carrying a correlated, uncorrelated, `IN` or scalar subquery costs +0.3 to +1.6 µs,
and the pk `SELECT` control with no subquery, which the change cannot reach, costs +0.8 to +1.0 µs.
The uncorrelated `IN` that is re-evaluated per outer row (it is not hoisted) shows no resolvable
delta: its per-row noise (±0.5 %) is larger than 35 ns on a 14 µs row. **The new behaviour is
priced separately and is not a win or a loss to count**: a nested uncorrelated `EXISTS` now runs,
at +1.06 µs per outer row for a 50-row inner relation, and an empty one now empties the statement,
where A dropped the inner chain and answered as if it were absent.

- **A** = `6b0d8412` (`v2.7.0-747-g6b0d8412`), the commit the branch was cut from.
- **B** = `23e53bed` (`v2.7.0-749-g23e53bed`), the tip of `worktree-fix-nested-uncorrelated-subquery`,
  two commits on (`4deb5a8e`, `23e53bed`). `git diff --stat 6b0d8412 23e53bed` is nine files, 234 insertions
  and 71 deletions: the `SubChain::hoisted` vector, `ChainRunner::HoistedAllTrue` and its two call sites
  in `src/exec/step_vm.cpp`, the compiler's nested hoisting (`src/exec/step_compiler.cpp`), the plan
  printer, the trail replay and access-stats fingerprint paths, two tests and a bug entry's deletion.
  **The change is wider than the evaluation helper** and the A/B prices all of it; the compiler, printer
  and fingerprint edits run once per statement, not per row.
- The suite was **not executed** by this measurement; no code changed in it. The commits' own stages carry it.
- **The PostgreSQL floor was not measured for this shape**, and no prior kdbs file measures it (rule 4), so
  there is no history series: this A/B is the comparison, and a later run of these arms is read against it.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-10. Main matrix 16:54 to 17:15 UTC (8 runs x 3 sizes, 24 server pairs); slot-swap confirmation 17:16 to 17:29 UTC (5 runs x 3 sizes, 15 pairs); plan dump (`ANALYZE`) afterwards |
| Branch / worktree | `worktree-fix-nested-uncorrelated-subquery`, worktree `fix-nested-uncorrelated-subquery`; `git status --short` empty before the build and at the start of the measurement |
| B | `23e53bed`, `v2.7.0-749-g23e53bed`, commit time 2026-10-10 16:35:51 UTC. Built in the worktree's `build-release/` (Release, `-O3 -DNDEBUG`, `kds_server` target only) at 16:38:05 UTC, **after** the commit; source binary mtime 2026-10-10 16:38:05 UTC |
| A | `6b0d8412`, `v2.7.0-747-g6b0d8412`, commit time 2026-10-10 15:06:09 UTC. A detached `git worktree` of that commit, built in its own `build-release/` (same script, same flags); source binary mtime 2026-10-10 16:39:23 UTC. The worktree was removed afterwards |
| Measured copies | A `ee29d277d3b19939f1c93bdd57e789796c3a987dbd421deafc862afa2c7ef574`, B `13a9ba31e8aa6de235defcd257390fa2b90b4a78dcc6c96ed1c84ffc1a580173` (`archive/.../binaries.sha256`; re-verified with `sha256sum -c` after the runs). Every server of every run was started from these copies under `~/bench-runs/nh/`; the build trees were not read after the copy |
| Device | data files under `~/bench-runs/nh/work/`, `/dev/root`, ext4 (`df -T`, 2026-10-10), not tmpfs; a fresh data file and server per (run, rows), deleted after the run |
| Host | 8 logical CPUs, AMD EPYC 9V74 (4 cores x 2 threads), Linux 7.0.0-1014-azure; other sessions' work shared it throughout (section 2) |
| Pinning | both servers of a pair on CPU 6 (one is idle while the other is timed), the client on CPU 7; ports 15620 (B) and 15621 (A), chosen, never the default. another session's `bj-close` measurement (servers on CPU 2, driver on CPU 4) was seen running at 16:36 and 16:46 UTC, before the main matrix started, and no process of it was looked for during the matrix; every CPU's busy fraction per run is in `archive/.../main/n*-run*.host.txt` |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, `log_level = warn`, everything else default; relations `BTREE` |
| Drivers | scratch, in `archive/.../drivers/`; `tools/` untouched. The driver reuses `tools/bench_common.Phase`, `ckdbs_cli.ServerConnection` and `fk_overhead_benchmark`'s `Server`, `timed`, `meta`, `host_state`. `bench/docs/` is closed (`bench/README.md`), so the commands are `drivers/nh_run.py 8 200,1000,10000 <out>` and the same under `NH_SWAP=1` for the swap pass |

## 2. Method, and the host each run ran on

One client connection, serial. Per (run, rows) a fresh A server and a fresh B server, each loaded
with identical data, then every arm runs in 16 blocks that alternate which server goes first, the
same statements on both: **equal work, not equal time**. Relations (all `BTREE`), with `--rows N`
being the outer relation's row count:

| Relation | Rows | Shape |
|---|---:|---|
| `a (id int64, v int64)` | N = 200, 1,000, 10,000 | `id` 1..N, `v = id % 7`; the outer relation of every subquery arm |
| `b (id int64, a_id int64, v int64)` | N / 2 | `a_id = 2 * id`, so every even `a.id` has one `b` row; `CREATE INDEX ix_b_aid ON b(a_id)` |
| `c (id int64, tag int64)` | 50 | `tag = id`; `tag = 999999` never exists |
| `ins (id int64, v int64)` | grows | the `INSERT` control's target |

The size is a row of every table. **Point arms** run 400 statements per block (6,400 per run per
server, 44,800 pooled over 7 runs); **scan arms** (a whole-`a` statement) run `max(2, 40000 / N)`
statements per block, so each block visits about 40,000 outer rows: 200 (3,200 per run, 22,400 pooled),
1,000 (640, 4,480) and 10,000 (64, 448). A point arm names `a.id = k` (k cycling through 1..N) and
`AND`s the subquery; a scan arm names no key.

| Arm | Statement (outer `a`) | Path it exercises |
|---|---|---|
| corr-exists-point / -scan | `EXISTS (SELECT b.id FROM b WHERE b.a_id = a.id)` | correlated sub-chain, an index probe per outer row (the path the change edits) |
| corr-notexists-point | `NOT EXISTS (...)` of the same | same, the other kind |
| unc-exists-point / -scan | `EXISTS (SELECT c.id FROM c WHERE c.tag = 5)` (scan adds `a.v = 3`) | top-level hoisted, once per statement; now through `HoistedAllTrue` |
| unc-in-point / -scan | `a.id IN (SELECT b.a_id FROM b WHERE b.id BETWEEN 1 AND 20)` | **not hoisted**: `ANALYZE` shows `sub_runs=1000` for 1,000 outer rows, so one `EvaluateSubChain` per outer row |
| unc-scalar-point | `a.v = (SELECT c.tag FROM c WHERE c.id = 3)` | scalar subquery |
| nested-none-point / -scan | correlated `EXISTS` over `b` whose inner also holds `AND EXISTS (SELECT c.id FROM c WHERE c.tag = 999999)` | **new behaviour**: A drops the inner chain (returns the outer's matches), B runs it and returns no row |
| nested-some-point / -scan | the same with `c.tag = 5` (non-empty) | **new behaviour**: both answer the same rows; B does the inner work A skipped |
| ctl-select-pk, -again | `SELECT * FROM a WHERE id = k` | no subquery; the change cannot reach it |
| ctl-insert | `INSERT INTO ins` with a named ascending pk | no subquery |
| ctl-ping | `SHOW META` | no executor at all |
| ctl-scan-filter, -again | `SELECT a.id FROM a WHERE a.v = 3` | a whole-relation walk with **no subquery**: the scan-shaped control |

**Verification.** Every reply is counted; the error count is 0 in all 39 pairs. The rows each arm
returned are recorded per server: **A and B return the same total row count in every arm except
`nested-none-point` and `nested-none-scan`**, where A returns the outer's matches (320,000 rows per run
in each scan cell) and B returns none, which is the behaviour change itself.

**Statistics.** A per-run p50 (and p99, mean, engine time) per side; the delta of a cell is the median
over runs of the per-run B − A, with the interquartile range of those per-run differences (linear
interpolation, 7 values). Pooled distributions (p0, p25, p50, p75, p99, nearest rank) are over the 7 runs'
statements. **The p95 column the rule names is replaced by p75, as this stage's order specified.**
"Engine" is the delta of `SHOW META`'s `sched_foreground_polled_us` across each block divided by the
statements in it, the server's own time on the statement; the rest of a statement's latency is the client,
the socket round trip and the reply.

**The run excluded, and why.** Run 5's three pairs started with another session's `cmake --build` of a
`carve-diag` tree live (loadavg 4.3 to 5.2, `pgrep` named the build) and the other runs' started with none.
Its three pairs are **excluded from every table below** (rule 4's record, applied) and kept in the archive:
the 8-run analysis reaches the same conclusions, and the figures that differ are the ones this file calls
outlier-driven. **7 runs per size remain**, 21 pairs in the main matrix, and the swap pass's 5 runs per
size, none with a competing build at its start. Loads at each run's start ran 1.95 to 4.73 outside run
5 (the 4.73 is run 6's first pair, which began as run 5's neighbours' builds drained and which
`pgrep` found clear), 2.16 to 3.49 in the swap pass: other sessions' servers and drivers shared the
host throughout. The per-run record is `archive/.../host.md`.

**The noise floor, from inside the run.** The controls cannot reach the change: a pk `SELECT` (twice), an
`INSERT`, `SHOW META` and two whole-relation filter walks. Their per-run B − A medians are in section 5; they
bound what a point statement and a scan's per-row cost can be said to differ by.

## 3. The correlated sub-chain: about +35 ns per outer row

On a statement that evaluates the correlated sub-chain once per outer row, B's median is slower by
7.2, 38.3 and 282 µs at 200, 1,000 and 10,000 outer rows: **36, 38 and 28 ns per outer row, 1.3, 1.2
and 0.9 % of the statement's median**, and 1.5 % of A's engine time per row at 1,000 (2.41 µs). B is
slower in 7 of 7 runs at every size. The engine's own time moves with the client's (+7.6, +39.1 and
+198.6 µs per statement), so the delta is the server's, not the client's or the socket's. The two scan
controls move by 3 ns per row or less at 200 rows and 0.5 ns or less at 1,000 and 10,000 (section 5).

| Arm | Rows | A p50 | A engine per statement | B p50 | median per-run Δp50 | IQR of per-run Δp50 | median per-run Δp99 | IQR of per-run Δp99 | median Δ engine per statement | IQR Δ engine | Δp50 per outer row |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| corr-exists-scan | 200 | 552.2 µs | 355.5 µs | 554.8 µs | +7.2 µs | [+4.5, +9.9] µs | +2.8 µs | [-9.2, +19.3] µs | +7.6 µs | [+1.6, +9.9] µs | +36.0 ns |
| corr-exists-scan | 1,000 | 3,079.9 µs | 2,413.8 µs | 3,113.2 µs | +38.3 µs | [+29.3, +39.4] µs | +70.0 µs | [-68.2, +224.5] µs | +39.1 µs | [+29.2, +51.6] µs | +38.3 ns |
| corr-exists-scan | 10,000 | 30.64 ms | 24.76 ms | 30.99 ms | +282.1 µs | [+237.8, +925.8] µs | -1,303.5 µs | [-20180.2, +763.8] µs | +198.6 µs | [+165.3, +273.0] µs | +28.2 ns |

Paired by block (112 blocks per cell: 7 runs x 16 blocks, B − A on the block's own median), the sign
holds without the run structure: B is slower in 101, 112 and 94 of 112 blocks. The pk `SELECT` control
is slower by the same proportion (+0.8 to +1.2 %), which is what the point arms below carry; that control
does not explain the scan arms' *growth* with the row count, which a per-statement offset of 0.9 µs cannot.

| Arm | Rows | Paired blocks | Median block Δp50 (B − A) | IQR of block Δ | Median relative Δ | Blocks with B slower |
|---|---:|---:|---:|---:|---:|---:|
| corr-exists-point | 200 | 112 | +0.30 µs | [-0.26, +0.81] µs | +0.28 % | 70/112 |
| corr-exists-point | 1,000 | 112 | +1.15 µs | [+0.70, +1.56] µs | +1.03 % | 102/112 |
| corr-exists-point | 10,000 | 112 | +0.88 µs | [+0.10, +1.40] µs | +0.82 % | 87/112 |
| corr-exists-scan | 200 | 112 | +7.72 µs | [+1.78, +10.32] µs | +1.39 % | 101/112 |
| corr-exists-scan | 1,000 | 112 | +36.50 µs | [+28.24, +44.49] µs | +1.19 % | 112/112 |
| corr-exists-scan | 10,000 | 112 | +314.07 µs | [+96.90, +518.09] µs | +0.98 % | 94/112 |
| ctl-select-pk | 200 | 112 | +0.72 µs | [+0.10, +1.46] µs | +0.77 % | 85/112 |
| ctl-select-pk | 1,000 | 112 | +1.17 µs | [+0.45, +2.00] µs | +1.21 % | 93/112 |
| ctl-select-pk | 10,000 | 112 | +0.90 µs | [+0.10, +1.40] µs | +0.97 % | 89/112 |
| ctl-scan-filter | 200 | 112 | +0.88 µs | [+0.30, +1.45] µs | +0.60 % | 94/112 |
| ctl-scan-filter | 1,000 | 112 | -0.63 µs | [-3.72, +2.27] µs | -0.18 % | 50/112 |
| ctl-scan-filter | 10,000 | 112 | +6.70 µs | [-3.74, +21.34] µs | +0.26 % | 73/112 |
| ctl-scan-filter-again | 200 | 112 | +0.62 µs | [+0.00, +1.26] µs | +0.42 % | 83/112 |
| ctl-scan-filter-again | 1,000 | 112 | +0.03 µs | [-2.15, +3.70] µs | +0.01 % | 56/112 |
| ctl-scan-filter-again | 10,000 | 112 | +3.42 µs | [-11.40, +20.74] µs | +0.13 % | 62/112 |

**What the number is not.** It is not attributed to a line. The change adds, per sub-chain evaluation,
one call of `HoistedAllTrue(sub.hoisted)` (an empty-vector early-out returning a `StatusOr<bool>`), a
`.ok()` and `.value()` test, and moves the runner's construction inside an `if`. 35 ns is on the order of
a hundred cycles, far more than that sequence costs on its face; the measurement cannot separate the added
instructions from a different inlining or layout of `EvaluateSubChain`, which is a large function. The
swap pass shows it follows the binary and not the slot, the port, the start order or the CPU.

## 4. Point statements and the uncorrelated shapes: inside the floor

A point statement evaluates its sub-chain once. B's median is slower by 0.3 to 1.6 µs on every point arm
that carries a subquery, and by 0.2 to 1.1 µs on the pk `SELECT` controls that carry none: the
statements the change cannot reach move as much as the ones it can. The one cell that sits above those
controls is `unc-in-point` at 1,000 rows (+1.6 µs, IQR +1.5 to +2.1, 7 of 7), which is 0.5 to 0.8 µs above the
controls' +0.8 and +1.1 and **inside a 1 µs floor**; it is stated here and **not reported as a finding**.
The top-level hoisted `EXISTS` (`unc-exists-point`, `-scan`) moves by +0.6 to +1.4 µs and +0.8 to
+5.0 µs; the 10,000-row scan's +5.0 µs [-0.3, +6.0] is what its scan control also shows (+5.0 [-1.9, +7.9]
and +5.3 [+0.2, +9.1]), 0.2 %.

| Arm | Rows | A p50 | A engine per statement | B p50 | median per-run Δp50 | IQR of per-run Δp50 | median per-run Δp99 | IQR of per-run Δp99 | median Δ engine per statement | IQR Δ engine | Δp50 per outer row |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| corr-exists-point | 200 | 109.3 µs | 42.9 µs | 109.2 µs | +0.5 µs | [-0.1, +0.6] µs | +2.1 µs | [+0.2, +4.7] µs | +0.0 µs | [-0.0, +0.4] µs | n/a |
| corr-exists-point | 1,000 | 109.3 µs | 44.0 µs | 110.4 µs | +1.2 µs | [+0.7, +1.4] µs | +2.0 µs | [+0.5, +2.9] µs | +0.7 µs | [+0.5, +0.8] µs | n/a |
| corr-exists-point | 10,000 | 107.0 µs | 42.5 µs | 108.1 µs | +1.1 µs | [+0.5, +1.1] µs | +1.6 µs | [+0.1, +2.2] µs | +0.4 µs | [+0.3, +0.5] µs | n/a |
| corr-notexists-point | 200 | 107.5 µs | 42.8 µs | 108.2 µs | +0.8 µs | [+0.5, +1.2] µs | +1.0 µs | [+0.4, +2.0] µs | +0.4 µs | [+0.3, +0.6] µs | n/a |
| corr-notexists-point | 1,000 | 108.2 µs | 44.1 µs | 109.5 µs | +1.0 µs | [+0.7, +1.3] µs | +2.1 µs | [+0.6, +3.2] µs | +0.6 µs | [+0.4, +0.8] µs | n/a |
| corr-notexists-point | 10,000 | 106.8 µs | 42.7 µs | 107.1 µs | +1.1 µs | [+0.5, +1.1] µs | +0.3 µs | [-3.6, +1.0] µs | +0.4 µs | [+0.1, +0.9] µs | n/a |
| unc-exists-point | 200 | 105.3 µs | 41.0 µs | 106.7 µs | +0.6 µs | [+0.4, +1.3] µs | +2.6 µs | [+1.3, +6.5] µs | +0.3 µs | [+0.2, +0.6] µs | n/a |
| unc-exists-point | 1,000 | 106.3 µs | 42.3 µs | 107.9 µs | +1.4 µs | [+0.9, +1.9] µs | +0.5 µs | [-2.1, +1.8] µs | +0.6 µs | [+0.4, +0.8] µs | n/a |
| unc-exists-point | 10,000 | 103.7 µs | 39.5 µs | 104.7 µs | +1.3 µs | [+0.7, +1.9] µs | +2.1 µs | [-0.1, +2.6] µs | +0.5 µs | [+0.4, +1.4] µs | n/a |
| unc-exists-scan | 200 | 156.5 µs | 57.6 µs | 158.1 µs | +0.8 µs | [+0.5, +1.8] µs | +2.2 µs | [+0.8, +2.8] µs | +0.7 µs | [+0.4, +1.1] µs | +4.0 ns |
| unc-exists-scan | 1,000 | 365.8 µs | 124.1 µs | 366.3 µs | +0.9 µs | [+0.5, +1.8] µs | +0.9 µs | [-3.2, +5.5] µs | +0.9 µs | [-0.1, +1.1] µs | +0.9 ns |
| unc-exists-scan | 10,000 | 2,618.5 µs | 858.7 µs | 2,623.2 µs | +5.0 µs | [-0.3, +6.0] µs | +2,093.4 µs | [+1917.0, +2201.9] µs | -2.7 µs | [-4.9, +3.4] µs | +0.5 ns |
| unc-in-point | 200 | 108.6 µs | 47.5 µs | 109.4 µs | +0.7 µs | [+0.5, +1.2] µs | +0.5 µs | [-0.4, +3.0] µs | +0.5 µs | [+0.2, +0.9] µs | n/a |
| unc-in-point | 1,000 | 113.6 µs | 53.1 µs | 115.6 µs | +1.6 µs | [+1.5, +2.1] µs | +1.7 µs | [+0.9, +3.8] µs | +1.2 µs | [+1.0, +1.5] µs | n/a |
| unc-in-point | 10,000 | 111.7 µs | 51.5 µs | 113.8 µs | +1.4 µs | [+1.2, +1.8] µs | +0.2 µs | [-0.2, +1.5] µs | +0.8 µs | [+0.7, +1.4] µs | n/a |
| unc-scalar-point | 200 | 101.5 µs | 39.8 µs | 102.4 µs | +0.6 µs | [+0.4, +1.1] µs | +2.0 µs | [-0.2, +3.0] µs | +0.3 µs | [+0.1, +0.8] µs | n/a |
| unc-scalar-point | 1,000 | 101.8 µs | 40.7 µs | 102.7 µs | +0.5 µs | [-0.1, +1.0] µs | +0.6 µs | [-1.1, +4.8] µs | +0.3 µs | [+0.0, +0.6] µs | n/a |
| unc-scalar-point | 10,000 | 98.3 µs | 37.6 µs | 99.2 µs | +0.3 µs | [+0.2, +0.8] µs | +0.4 µs | [-1.0, +2.1] µs | +0.2 µs | [+0.2, +0.6] µs | n/a |

**The uncorrelated `IN` evaluated per outer row** goes through the same `EvaluateSubChain` once per
outer row but costs 10 to 14 µs a row (a 20-row range walk of `b` plus the `IN` build). A 35 ns
per-row cost would be 0.25 to 0.35 % of it; the cell's own spread is larger: Δp50 is -1.1 µs [-5.9, +1.3]
at 200 rows, +228 µs [-11, +399] at 1,000 and -246 µs [-472, +492] at 10,000, with B slower in 3, 5 and 3
of 7 runs. **The measurement cannot see a per-row cost of that size here, in either direction.**

| Arm | Rows | A p50 | A engine per statement | B p50 | median per-run Δp50 | IQR of per-run Δp50 | median per-run Δp99 | IQR of per-run Δp99 | median Δ engine per statement | IQR Δ engine | Δp50 per outer row |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| unc-in-scan | 200 | 1,998.9 µs | 1,903.3 µs | 1,995.2 µs | -1.1 µs | [-5.9, +1.3] µs | -8.4 µs | [-110.2, +14.6] µs | -3.4 µs | [-8.2, -3.0] µs | -5.5 ns |
| unc-in-scan | 1,000 | 15.93 ms | 15.88 ms | 16.05 ms | +228.0 µs | [-11.2, +399.2] µs | -364.1 µs | [-2348.8, +137.8] µs | +253.9 µs | [+45.8, +398.8] µs | +228.0 ns |
| unc-in-scan | 10,000 | 138.53 ms | 138.78 ms | 138.45 ms | -245.5 µs | [-471.9, +492.0] µs | -6,552.2 µs | [-45549.3, -900.8] µs | +358.4 µs | [-421.4, +1329.3] µs | -24.6 ns |

## 5. The noise floor, from the controls

The controls give the floor each cell is read against. A point statement moves by up to +1.1 µs
(0.1 to 1.2 %) on the `SELECT`, 0.4 µs on the `INSERT`, 0.5 µs on `SHOW META`; **the floor on a point
statement is therefore taken as 1 µs**, and a delta below it is not a finding. The scan controls (a
walk with no subquery) move by -0.4 to +0.6 µs at 200 and 1,000 rows and by +5.0 and +5.3 µs at
10,000 (0.2 %, 0.5 ns per row); **the floor on a scan's per-row cost is taken as 3 ns per row at 200
rows and 1 ns at 1,000 and 10,000**, against which 28 to 38 ns is 10 to 30 times as large. **A p99 at 10,000 rows is not usable**: a block holds
4 statements and a run 64, so p99 is the maximum, and the control's own per-run Δp99 is -2.9 and -3.3 ms
with a tight IQR (a systematic outlier of the host, not of the engine); the p99 columns of the scan
rows at 10,000 are listed and not read.

| Arm | Rows | A p50 | A engine per statement | B p50 | median per-run Δp50 | IQR of per-run Δp50 | median per-run Δp99 | IQR of per-run Δp99 | median Δ engine per statement | IQR Δ engine | Δp50 per outer row |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ctl-select-pk | 200 | 95.0 µs | 32.4 µs | 96.1 µs | +0.8 µs | [+0.2, +1.4] µs | +0.3 µs | [-1.2, +2.5] µs | +0.1 µs | [-0.1, +0.6] µs | n/a |
| ctl-select-pk | 1,000 | 95.4 µs | 33.2 µs | 96.7 µs | +0.8 µs | [+0.6, +1.4] µs | +1.3 µs | [-0.0, +2.4] µs | +0.3 µs | [+0.1, +0.6] µs | n/a |
| ctl-select-pk | 10,000 | 92.8 µs | 30.5 µs | 93.7 µs | +1.0 µs | [+0.3, +1.3] µs | +0.3 µs | [+0.1, +2.1] µs | +0.6 µs | [+0.2, +0.9] µs | n/a |
| ctl-select-pk-again | 200 | 95.0 µs | 32.1 µs | 96.1 µs | +0.8 µs | [+0.2, +1.1] µs | +0.7 µs | [-0.2, +1.4] µs | +0.1 µs | [-0.1, +0.4] µs | n/a |
| ctl-select-pk-again | 1,000 | 95.3 µs | 32.5 µs | 96.6 µs | +1.1 µs | [+0.5, +1.6] µs | +2.5 µs | [+1.6, +3.7] µs | +0.5 µs | [+0.1, +0.9] µs | n/a |
| ctl-select-pk-again | 10,000 | 92.9 µs | 31.1 µs | 93.1 µs | +0.2 µs | [-0.3, +0.6] µs | -4.8 µs | [-6.2, -4.2] µs | -0.3 µs | [-0.7, -0.3] µs | n/a |
| ctl-insert | 200 | 72.5 µs | 24.6 µs | 72.6 µs | +0.4 µs | [-0.1, +0.9] µs | +0.9 µs | [+0.0, +2.0] µs | +0.4 µs | [+0.2, +0.6] µs | n/a |
| ctl-insert | 1,000 | 72.4 µs | 24.7 µs | 72.9 µs | +0.4 µs | [-0.0, +0.6] µs | +0.5 µs | [+0.4, +1.9] µs | +0.4 µs | [+0.0, +0.4] µs | n/a |
| ctl-insert | 10,000 | 72.4 µs | 24.8 µs | 72.5 µs | +0.0 µs | [-0.3, +0.1] µs | +2.5 µs | [+0.9, +7.7] µs | +0.1 µs | [-0.0, +0.2] µs | n/a |
| ctl-ping | 200 | 72.5 µs | 30.6 µs | 72.4 µs | -0.1 µs | [-0.6, +0.6] µs | -0.1 µs | [-1.0, +0.6] µs | +0.3 µs | [+0.2, +0.4] µs | n/a |
| ctl-ping | 1,000 | 74.7 µs | 26.9 µs | 74.9 µs | +0.1 µs | [-0.1, +0.3] µs | +0.5 µs | [+0.1, +0.9] µs | +0.2 µs | [+0.1, +0.3] µs | n/a |
| ctl-ping | 10,000 | 74.8 µs | 27.0 µs | 75.3 µs | +0.5 µs | [-0.4, +0.6] µs | +0.4 µs | [-0.3, +0.8] µs | +0.2 µs | [+0.1, +0.3] µs | n/a |
| ctl-scan-filter | 200 | 146.4 µs | 51.3 µs | 147.9 µs | +0.6 µs | [+0.4, +1.2] µs | +0.9 µs | [+0.2, +7.0] µs | +0.4 µs | [+0.3, +0.6] µs | +3.0 ns |
| ctl-scan-filter | 1,000 | 356.1 µs | 118.0 µs | 355.7 µs | -0.4 µs | [-1.3, -0.2] µs | +0.8 µs | [-3.2, +5.1] µs | -0.4 µs | [-0.5, -0.2] µs | -0.4 ns |
| ctl-scan-filter | 10,000 | 2,612.9 µs | 849.0 µs | 2,612.3 µs | +5.0 µs | [-1.9, +7.9] µs | -2,888.5 µs | [-2950.5, -2777.6] µs | +2.3 µs | [-2.3, +7.1] µs | +0.5 ns |
| ctl-scan-filter-again | 200 | 146.9 µs | 51.5 µs | 147.7 µs | +0.3 µs | [+0.0, +1.0] µs | -0.7 µs | [-1.3, +1.1] µs | +0.3 µs | [+0.2, +0.5] µs | +1.5 ns |
| ctl-scan-filter-again | 1,000 | 356.3 µs | 117.4 µs | 357.0 µs | +0.5 µs | [+0.1, +0.7] µs | -2.0 µs | [-5.3, -1.0] µs | -0.2 µs | [-0.5, +0.2] µs | +0.5 ns |
| ctl-scan-filter-again | 10,000 | 2,603.4 µs | 848.8 µs | 2,604.1 µs | +5.3 µs | [+0.2, +9.1] µs | -3,335.9 µs | [-3507.3, -3214.9] µs | +5.5 µs | [-0.4, +8.6] µs | +0.5 ns |

## 6. The slot-swap confirmation

The controls show B a little slower on a pk `SELECT`, so the next question is whether that offset and
the correlated scan's follow the **binary** or the **slot** (the second server of a pair, its port,
its start order). The swap pass runs the same driver with the A binary in slot B and the B binary in
slot A (5 runs per size). In the table, the first value column is B − A with the B binary in slot B (the main matrix); the second is
the swap pass's slot-B − slot-A **with its sign flipped**, so it is B − A again; the last two columns split the
average of the two into what follows the binary and what follows the slot.

| Arm | Rows | Δp50, B binary in slot B (7 runs) | Δp50, B binary in slot A, sign flipped to B − A (5 runs) | Effect that follows the binary | Effect that follows the slot |
|---|---:|---:|---:|---:|---:|
| corr-exists-point | 200 | +0.5 µs | +0.7 µs | +0.6 µs | -0.1 µs |
| corr-exists-point | 1,000 | +1.2 µs | +0.2 µs | +0.7 µs | +0.5 µs |
| corr-exists-point | 10,000 | +1.1 µs | +0.7 µs | +0.9 µs | +0.2 µs |
| corr-exists-scan | 200 | +7.2 µs | +7.6 µs | +7.4 µs | -0.2 µs |
| corr-exists-scan | 1,000 | +38.3 µs | +32.2 µs | +35.2 µs | +3.1 µs |
| corr-exists-scan | 10,000 | +282.1 µs | +370.2 µs | +326.1 µs | -44.1 µs |
| unc-in-scan | 200 | -1.1 µs | -4.5 µs | -2.8 µs | +1.7 µs |
| unc-in-scan | 1,000 | +228.0 µs | -136.9 µs | +45.6 µs | +182.4 µs |
| unc-in-scan | 10,000 | -245.5 µs | -139.3 µs | -192.4 µs | -53.1 µs |
| ctl-select-pk | 200 | +0.8 µs | +0.2 µs | +0.5 µs | +0.3 µs |
| ctl-select-pk | 1,000 | +0.8 µs | +0.2 µs | +0.5 µs | +0.3 µs |
| ctl-select-pk | 10,000 | +1.0 µs | +0.8 µs | +0.9 µs | +0.1 µs |
| ctl-select-pk-again | 200 | +0.8 µs | +0.2 µs | +0.5 µs | +0.3 µs |
| ctl-select-pk-again | 1,000 | +1.1 µs | +0.6 µs | +0.9 µs | +0.3 µs |
| ctl-select-pk-again | 10,000 | +0.2 µs | +0.5 µs | +0.4 µs | -0.1 µs |
| ctl-insert | 200 | +0.4 µs | -0.0 µs | +0.2 µs | +0.2 µs |
| ctl-insert | 1,000 | +0.4 µs | -0.4 µs | +0.0 µs | +0.4 µs |
| ctl-insert | 10,000 | +0.0 µs | +0.7 µs | +0.4 µs | -0.4 µs |
| ctl-ping | 200 | -0.1 µs | -0.3 µs | -0.2 µs | +0.1 µs |
| ctl-ping | 1,000 | +0.1 µs | +0.6 µs | +0.3 µs | -0.2 µs |
| ctl-ping | 10,000 | +0.5 µs | +0.6 µs | +0.6 µs | -0.1 µs |
| ctl-scan-filter | 200 | +0.6 µs | +0.1 µs | +0.3 µs | +0.2 µs |
| ctl-scan-filter | 1,000 | -0.4 µs | +0.3 µs | -0.1 µs | -0.4 µs |
| ctl-scan-filter | 10,000 | +5.0 µs | -10.3 µs | -2.7 µs | +7.7 µs |
| ctl-scan-filter-again | 200 | +0.3 µs | +0.8 µs | +0.5 µs | -0.2 µs |
| ctl-scan-filter-again | 1,000 | +0.5 µs | +0.9 µs | +0.7 µs | -0.2 µs |
| ctl-scan-filter-again | 10,000 | +5.3 µs | -5.8 µs | -0.2 µs | +5.5 µs |

The correlated scan's +7.4, +35.2 and +326 µs follow the binary (the slot effect is -0.2, +3.1 and
-44 µs). The pk `SELECT` controls' +0.4 to +0.9 µs also follow the binary, which says B's `SELECT`
path is about 0.9 µs (about 1 %) slower per statement for a reason **the control does not isolate**; whether it is the
same cause as the per-row +35 ns is not established by this run. The scan controls' 10,000-row +5 µs is a
slot effect (+7.7 and +5.5) and not the binary's (-2.7 and -0.2), which is why it is not read as a cost.

## 7. The new behaviour: a nested uncorrelated `EXISTS` now runs

`ANALYZE` of the nested statement at 1,000 outer rows (`archive/.../analyze-plans.txt`) shows A's plan with
no inner chain (`examined=1500`, `opens=1001`) and B's plan with a `hoisted EXISTS [uncorrelated]` under the
correlated `EXISTS` that **runs once per outer row** (`step 2 FilterScan c opens=1000 examined=5000`;
`examined=6500`, `opens=2001`). The two nested-`some` arms return the same rows on both servers, and the
price of the work A skipped is +1.06 µs per outer row (50-row `c`, five rows examined before the match),
+2.9 to +4.2 µs on a point statement. **The `nested-none` arms are not comparable work**: B returns no row
where A returned the outer's matches (100, 500 and 5,000 rows per statement at 200, 1,000 and 10,000 outer
rows; B: 0), and a B statement that empties the outer is slower than A's anyway, +1.4 to +2.3 µs per outer
row, because the inner chain runs for every outer row before the sub-chain's steps are skipped; the point
arm is faster in B by 4.8 to 6.5 µs because it returns nothing. **None of these four arms is counted as a
win or a loss.**

| Arm | Rows | A p50 | A engine per statement | B p50 | median per-run Δp50 | IQR of per-run Δp50 | median per-run Δp99 | IQR of per-run Δp99 | median Δ engine per statement | IQR Δ engine | Δp50 per outer row |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| nested-none-point | 200 | 113.9 µs | 46.2 µs | 108.0 µs | -5.9 µs | [-6.2, -5.2] µs | -11.1 µs | [-11.9, -9.1] µs | -0.1 µs | [-0.4, +0.5] µs | n/a |
| nested-none-point | 1,000 | 114.5 µs | 47.7 µs | 108.0 µs | -6.5 µs | [-7.1, -6.4] µs | -13.2 µs | [-14.9, -12.4] µs | -1.8 µs | [-1.8, -1.5] µs | n/a |
| nested-none-point | 10,000 | 112.6 µs | 45.8 µs | 107.9 µs | -4.8 µs | [-5.1, -4.2] µs | -2.4 µs | [-3.1, -1.2] µs | +0.0 µs | [+0.0, +0.6] µs | n/a |
| nested-none-scan | 200 | 552.1 µs | 354.5 µs | 1,015.4 µs | +463.4 µs | [+461.0, +465.7] µs | +483.6 µs | [+481.0, +485.0] µs | +585.8 µs | [+585.4, +588.8] µs | +2317.0 ns |
| nested-none-scan | 1,000 | 3,094.9 µs | 2,423.6 µs | 4,598.0 µs | +1,482.8 µs | [+1471.8, +1523.6] µs | +1,647.4 µs | [+1609.6, +1751.1] µs | +2,080.7 µs | [+2069.8, +2132.9] µs | +1482.8 ns |
| nested-none-scan | 10,000 | 30.53 ms | 24.50 ms | 44.54 ms | +13,992.6 µs | [+13820.8, +14563.8] µs | +12,549.4 µs | [+11277.2, +13513.2] µs | +20,069.4 µs | [+19856.6, +20693.8] µs | +1399.3 ns |
| nested-some-point | 200 | 114.1 µs | 46.3 µs | 117.5 µs | +2.9 µs | [+2.7, +3.5] µs | +3.6 µs | [+3.4, +7.4] µs | +2.7 µs | [+2.6, +3.1] µs | n/a |
| nested-some-point | 1,000 | 114.6 µs | 47.7 µs | 119.0 µs | +4.2 µs | [+4.1, +4.4] µs | +6.0 µs | [+4.5, +6.7] µs | +3.4 µs | [+3.3, +3.5] µs | n/a |
| nested-some-point | 10,000 | 112.3 µs | 45.6 µs | 116.5 µs | +4.2 µs | [+3.2, +4.6] µs | +5.8 µs | [+5.1, +7.3] µs | +3.3 µs | [+3.2, +3.8] µs | n/a |
| nested-some-scan | 200 | 552.5 µs | 354.6 µs | 764.9 µs | +213.8 µs | [+208.3, +215.7] µs | +223.8 µs | [+218.1, +244.5] µs | +212.0 µs | [+206.5, +213.0] µs | +1069.0 ns |
| nested-some-scan | 1,000 | 3,091.3 µs | 2,422.5 µs | 4,155.6 µs | +1,056.7 µs | [+1048.2, +1062.8] µs | +1,076.8 µs | [+1033.0, +1122.8] µs | +1,049.5 µs | [+1045.4, +1059.0] µs | +1056.7 ns |
| nested-some-scan | 10,000 | 30.51 ms | 24.46 ms | 41.01 ms | +10,566.0 µs | [+10266.2, +10578.6] µs | +11,598.7 µs | [+9481.2, +12550.1] µs | +10,545.5 µs | [+10251.7, +10619.0] µs | +1056.6 ns |

## 8. Throughput, as a matrix

Derived: tps = 1,000,000 / the median over runs of the per-run mean (µs); the driver is serial, so the
conversion is exact. **The mean is outlier-driven on scan arms and disagrees with the p50 there**:
`corr-exists-scan` reads +3.1 % and +7.8 % tps for B at 1,000 and 10,000 rows here while its p50 is slower
in 7 of 7 runs. The swap pass shows why: the mean's sign follows the **slot** (slot B's mean is lower by
about 100 to 160 µs at 1,000 rows in both passes, whichever binary it holds), while the p50 follows the
binary. The sections above are read from the p50 and the block pairs; this matrix is the throughput view
the rules ask for, and a tps delta on a scan row of it is not a finding.

| Arm | Rows | A tps | B tps | B vs A | Runs B slower (p50) |
|---|---:|---:|---:|---:|---:|
| **Correlated subquery (the per-outer-row `EvaluateSubChain` path)** | | | | | |
| corr-exists-point | 200 | 9,027 tps | 8,992 tps | -0.39 % | 4/7 |
| corr-exists-point | 1,000 | 8,961 tps | 8,855 tps | -1.19 % | 7/7 |
| corr-exists-point | 10,000 | 9,158 tps | 9,073 tps | -0.93 % | 5/7 |
| corr-notexists-point | 200 | 9,084 tps | 8,954 tps | -1.44 % | 7/7 |
| corr-notexists-point | 1,000 | 8,913 tps | 8,836 tps | -0.87 % | 7/7 |
| corr-notexists-point | 10,000 | 9,128 tps | 9,089 tps | -0.43 % | 7/7 |
| corr-exists-scan | 200 | 1,792 tps | 1,775 tps | -0.94 % | 7/7 |
| corr-exists-scan | 1,000 | 309 tps | 319 tps | +3.12 % | 7/7 |
| corr-exists-scan | 10,000 | 30 tps | 32 tps | +7.77 % | 7/7 |
| **Uncorrelated subquery** | | | | | |
| unc-exists-point | 200 | 9,109 tps | 8,996 tps | -1.24 % | 7/7 |
| unc-exists-point | 1,000 | 8,988 tps | 8,889 tps | -1.10 % | 7/7 |
| unc-exists-point | 10,000 | 9,353 tps | 9,245 tps | -1.15 % | 6/7 |
| unc-exists-scan | 200 | 6,241 tps | 6,174 tps | -1.06 % | 6/7 |
| unc-exists-scan | 1,000 | 2,724 tps | 2,720 tps | -0.15 % | 7/7 |
| unc-exists-scan | 10,000 | 381 tps | 375 tps | -1.42 % | 5/7 |
| unc-in-point | 200 | 8,650 tps | 8,823 tps | +2.00 % | 7/7 |
| unc-in-point | 1,000 | 8,495 tps | 7,547 tps | -11.17 % | 7/7 |
| unc-in-point | 10,000 | 8,625 tps | 8,518 tps | -1.24 % | 7/7 |
| unc-in-scan | 200 | 481 tps | 498 tps | +3.58 % | 3/7 |
| unc-in-scan | 1,000 | 62 tps | 62 tps | -0.97 % | 5/7 |
| unc-in-scan | 10,000 | 7 tps | 7 tps | -0.26 % | 3/7 |
| unc-scalar-point | 200 | 9,532 tps | 9,395 tps | -1.44 % | 6/7 |
| unc-scalar-point | 1,000 | 9,495 tps | 9,415 tps | -0.84 % | 4/7 |
| unc-scalar-point | 10,000 | 9,744 tps | 9,781 tps | +0.38 % | 6/7 |
| **Nested uncorrelated EXISTS (the new behaviour; A drops the inner chain)** | | | | | |
| nested-none-point | 200 | 8,618 tps | 8,960 tps | +3.97 % | 0/7 |
| nested-none-point | 1,000 | 8,536 tps | 8,943 tps | +4.76 % | 0/7 |
| nested-none-point | 10,000 | 8,727 tps | 8,869 tps | +1.62 % | 0/7 |
| nested-none-scan | 200 | 1,799 tps | 978 tps | -45.63 % | 7/7 |
| nested-none-scan | 1,000 | 322 tps | 212 tps | -34.29 % | 7/7 |
| nested-none-scan | 10,000 | 32 tps | 22 tps | -30.98 % | 7/7 |
| nested-some-point | 200 | 8,630 tps | 8,298 tps | -3.84 % | 7/7 |
| nested-some-point | 1,000 | 8,520 tps | 7,703 tps | -9.59 % | 7/7 |
| nested-some-point | 10,000 | 8,777 tps | 8,425 tps | -4.01 % | 7/7 |
| nested-some-scan | 200 | 1,803 tps | 1,302 tps | -27.78 % | 7/7 |
| nested-some-scan | 1,000 | 323 tps | 240 tps | -25.75 % | 7/7 |
| nested-some-scan | 10,000 | 33 tps | 24 tps | -25.48 % | 7/7 |
| **Controls (no subquery)** | | | | | |
| ctl-select-pk | 200 | 10,100 tps | 10,006 tps | -0.93 % | 6/7 |
| ctl-select-pk | 1,000 | 10,021 tps | 9,918 tps | -1.03 % | 6/7 |
| ctl-select-pk | 10,000 | 10,422 tps | 10,347 tps | -0.72 % | 5/7 |
| ctl-select-pk-again | 200 | 10,092 tps | 10,021 tps | -0.70 % | 5/7 |
| ctl-select-pk-again | 1,000 | 10,116 tps | 9,984 tps | -1.30 % | 6/7 |
| ctl-select-pk-again | 10,000 | 10,320 tps | 10,401 tps | +0.78 % | 5/7 |
| ctl-insert | 200 | 13,242 tps | 13,053 tps | -1.43 % | 4/7 |
| ctl-insert | 1,000 | 13,259 tps | 11,533 tps | -13.01 % | 5/7 |
| ctl-insert | 10,000 | 13,237 tps | 13,133 tps | -0.78 % | 3/7 |
| ctl-ping | 200 | 13,416 tps | 13,401 tps | -0.11 % | 3/7 |
| ctl-ping | 1,000 | 12,924 tps | 12,948 tps | +0.19 % | 4/7 |
| ctl-ping | 10,000 | 12,931 tps | 12,927 tps | -0.03 % | 4/7 |
| ctl-scan-filter | 200 | 6,670 tps | 6,603 tps | -1.00 % | 6/7 |
| ctl-scan-filter | 1,000 | 2,800 tps | 2,791 tps | -0.34 % | 2/7 |
| ctl-scan-filter | 10,000 | 375 tps | 381 tps | +1.69 % | 5/7 |
| ctl-scan-filter-again | 200 | 6,606 tps | 6,619 tps | +0.20 % | 5/7 |
| ctl-scan-filter-again | 1,000 | 2,798 tps | 2,802 tps | +0.16 % | 5/7 |
| ctl-scan-filter-again | 10,000 | 375 tps | 382 tps | +1.99 % | 5/7 |

## 9. Where the time goes (rule 3)

A read statement on one connection at `cores = 1`, `relaxed`, has no commit wait and no lock wait in the
measurement (one session, nothing to contend with), so the decomposition is the server's engine time against
the rest:

| Statement | Median latency (A) | Engine time (A) | Share | The remainder |
|---|---:|---:|---:|---|
| pk `SELECT` at 1,000 rows | 95.4 µs | 33.2 µs | 35 % | client, socket round trip, reply: 62.2 µs |
| point statement with a correlated `EXISTS` | 109.3 µs | 44.0 µs | 40 % | 65.3 µs |
| correlated `EXISTS` scan, 1,000 outer rows | 3,079.9 µs | 2,413.8 µs | 78 % | 666 µs (500 reply rows) |
| correlated `EXISTS` scan, 10,000 outer rows | 30.64 ms | 24.76 ms | 81 % | 5.9 ms (5,000 reply rows) |
| `INSERT` (named pk, unsynced) | 72.4 µs | 24.7 µs | 34 % | 47.7 µs |

Durability waits (fsync, group commit) are **not applicable**: `relaxed` and no read statement commits;
the `INSERT` control's sync is the relaxed class's. The per-row engine cost of a correlated evaluation is
2.4 µs, so +35 ns is about 1.5 % of it.

## 10. What the run teaches

- **The early-out is not free where it is evaluated per row.** A helper that returns immediately on an empty
  vector still costs about 35 ns per call in this engine's per-row sub-chain path (1.0 to 1.4 %); the same
  helper at the top level is paid once and is invisible. A hoisted check that runs per outer row for a
  statement with no hoisting is what a later change would remove, for example by testing the vector
  before the call. That is a suggestion and is not measured.
- **A nested uncorrelated `EXISTS` is hoisted to the *sub-chain*, not to the statement**: `ANALYZE` shows
  it opened once per outer row of the enclosing correlated sub-chain (`opens=1000` for 1,000 outer rows),
  as `step_chain.hpp`'s comment for `SubChain::hoisted` says ("runs once per evaluation of this sub-chain").
  An uncorrelated inner is a statement invariant, so the same plan could run it once; the cost of not doing
  that is the +1.06 µs per outer row of section 7, and grows with the inner relation (a 50-row `c` here).
- **An uncorrelated `IN` is not hoisted at all**: one `EvaluateSubChain` per outer row (`sub_runs=1000`), which
  puts `a.id IN (SELECT b.a_id FROM b WHERE b.id BETWEEN 1 AND 20)` at 14 µs per outer row (138 ms at 10,000
  rows) for a 20-row list. This is the same on A and on B and is not caused by the change; it is why an
  uncorrelated `IN` is the arm on which a per-row delta of this size cannot be seen. The inner uses
  `BETWEEN 1 AND 20` because a `b.id <= 20` range does not prune the walk (a smoke run took 3.4 s a
  statement at 10,000 rows).
- **The host floor is about 1 µs and 1 %** on this box with other sessions running; a delta under it is not
  a finding, and the pk `SELECT` carries +0.8 to +1.0 µs that follows the binary without a subquery in it.

## 11. Not measured, and what bounds this

- **One connection, `cores = 1`, `relaxed`.** Nothing here saturates: the cells measure the statement's own
  cost at the extreme of the row count (10,000 outer rows, 30 ms, the walk's engine time) and not a
  concurrent limit; **no cell approached a saturation or a durability limit**, because the change adds no
  waiting path. A multi-connection or `strict` run of these shapes was not made.
- **Tree attribution.** The +35 ns is a property of the binary at `23e53bed` against `6b0d8412`; whether it is
  the helper call or the layout is not established.
- **The 8-run analysis** (run 5 included) is `archive/.../main/analysis-8runs.json`; its conclusions are the
  same (`corr-exists-scan` slower in 8 of 8 runs at every size, +8.4, +35.7 and +437.8 µs).
- **The suite** was not executed by this measurement.

## 12. Full tables

### Latency distributions (pooled over the 7 clean runs)

| Arm | Rows | Side | Statements | p0 | p25 | p50 | p75 | p99 |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| corr-exists-point | 200 | A | 44,800 | 86.2 µs | 103.1 µs | 109.4 µs | 117.9 µs | 189.7 µs |
| corr-exists-point | 200 | B | 44,800 | 87.8 µs | 103.7 µs | 109.7 µs | 118.6 µs | 193.8 µs |
| corr-exists-point | 1,000 | A | 44,800 | 97.7 µs | 103.5 µs | 109.4 µs | 116.6 µs | 146.8 µs |
| corr-exists-point | 1,000 | B | 44,800 | 98.4 µs | 104.5 µs | 110.6 µs | 118.1 µs | 148.7 µs |
| corr-exists-point | 10,000 | A | 44,800 | 87.5 µs | 104.1 µs | 108.2 µs | 119.5 µs | 190.0 µs |
| corr-exists-point | 10,000 | B | 44,800 | 87.1 µs | 104.8 µs | 109.1 µs | 120.3 µs | 190.8 µs |
| corr-notexists-point | 200 | A | 44,800 | 89.2 µs | 104.6 µs | 107.9 µs | 118.2 µs | 190.5 µs |
| corr-notexists-point | 200 | B | 44,800 | 90.6 µs | 105.4 µs | 108.9 µs | 119.7 µs | 194.0 µs |
| corr-notexists-point | 1,000 | A | 44,800 | 99.1 µs | 105.2 µs | 108.4 µs | 116.0 µs | 145.7 µs |
| corr-notexists-point | 1,000 | B | 44,800 | 100.1 µs | 106.1 µs | 109.4 µs | 117.2 µs | 147.8 µs |
| corr-notexists-point | 10,000 | A | 44,800 | 91.3 µs | 104.8 µs | 107.6 µs | 118.3 µs | 190.4 µs |
| corr-notexists-point | 10,000 | B | 44,800 | 92.0 µs | 105.5 µs | 108.4 µs | 119.3 µs | 187.3 µs |
| corr-exists-scan | 200 | A | 22,400 | 518.2 µs | 540.3 µs | 550.5 µs | 564.0 µs | 1,254.6 µs |
| corr-exists-scan | 200 | B | 22,400 | 527.3 µs | 548.0 µs | 557.1 µs | 570.6 µs | 1,267.6 µs |
| corr-exists-scan | 1,000 | A | 4,480 | 3,011.5 µs | 3,061.7 µs | 3,079.5 µs | 3,099.7 µs | 3,304.4 µs |
| corr-exists-scan | 1,000 | B | 4,480 | 3,052.9 µs | 3,099.2 µs | 3,118.5 µs | 3,145.1 µs | 3,390.3 µs |
| corr-exists-scan | 10,000 | A | 448 | 30.22 ms | 30.51 ms | 30.70 ms | 31.49 ms | 67.74 ms |
| corr-exists-scan | 10,000 | B | 448 | 30.50 ms | 30.82 ms | 31.01 ms | 32.84 ms | 67.59 ms |
| unc-exists-point | 200 | A | 44,800 | 99.7 µs | 103.9 µs | 105.6 µs | 111.5 µs | 142.2 µs |
| unc-exists-point | 200 | B | 44,800 | 100.6 µs | 104.9 µs | 106.5 µs | 112.8 µs | 145.6 µs |
| unc-exists-point | 1,000 | A | 44,800 | 98.9 µs | 104.2 µs | 106.6 µs | 115.0 µs | 147.2 µs |
| unc-exists-point | 1,000 | B | 44,800 | 100.7 µs | 105.8 µs | 108.0 µs | 116.6 µs | 147.1 µs |
| unc-exists-point | 10,000 | A | 44,800 | 94.6 µs | 102.1 µs | 103.6 µs | 107.9 µs | 132.3 µs |
| unc-exists-point | 10,000 | B | 44,800 | 98.4 µs | 103.3 µs | 104.8 µs | 109.2 µs | 134.8 µs |
| unc-exists-scan | 200 | A | 22,400 | 149.0 µs | 154.2 µs | 156.9 µs | 164.9 µs | 187.8 µs |
| unc-exists-scan | 200 | B | 22,400 | 149.8 µs | 155.4 µs | 158.0 µs | 166.1 µs | 190.1 µs |
| unc-exists-scan | 1,000 | A | 4,480 | 340.6 µs | 356.0 µs | 365.0 µs | 373.2 µs | 403.8 µs |
| unc-exists-scan | 1,000 | B | 4,480 | 345.6 µs | 356.6 µs | 366.5 µs | 374.2 µs | 404.4 µs |
| unc-exists-scan | 10,000 | A | 448 | 2,563.4 µs | 2,604.1 µs | 2,624.1 µs | 2,649.9 µs | 2,799.0 µs |
| unc-exists-scan | 10,000 | B | 448 | 2,571.2 µs | 2,606.7 µs | 2,626.1 µs | 2,660.5 µs | 4,716.0 µs |
| unc-in-point | 200 | A | 44,800 | 102.4 µs | 106.7 µs | 108.5 µs | 113.8 µs | 144.5 µs |
| unc-in-point | 200 | B | 44,800 | 103.3 µs | 107.7 µs | 110.1 µs | 115.5 µs | 145.6 µs |
| unc-in-point | 1,000 | A | 44,800 | 104.2 µs | 112.0 µs | 113.8 µs | 118.5 µs | 145.3 µs |
| unc-in-point | 1,000 | B | 44,800 | 104.5 µs | 113.9 µs | 115.6 µs | 120.3 µs | 147.3 µs |
| unc-in-point | 10,000 | A | 44,800 | 102.5 µs | 110.6 µs | 112.3 µs | 117.1 µs | 143.1 µs |
| unc-in-point | 10,000 | B | 44,800 | 105.9 µs | 112.1 µs | 113.7 µs | 118.5 µs | 143.7 µs |
| unc-in-scan | 200 | A | 22,400 | 1,953.4 µs | 1,993.6 µs | 2,001.6 µs | 2,044.1 µs | 4,078.4 µs |
| unc-in-scan | 200 | B | 22,400 | 1,952.0 µs | 1,989.6 µs | 1,996.6 µs | 2,006.9 µs | 4,132.0 µs |
| unc-in-scan | 1,000 | A | 4,480 | 15.65 ms | 15.81 ms | 15.90 ms | 16.06 ms | 27.88 ms |
| unc-in-scan | 1,000 | B | 4,480 | 15.60 ms | 15.84 ms | 16.08 ms | 16.39 ms | 32.01 ms |
| unc-in-scan | 10,000 | A | 448 | 136.70 ms | 138.22 ms | 138.73 ms | 139.88 ms | 244.00 ms |
| unc-in-scan | 10,000 | B | 448 | 136.51 ms | 137.67 ms | 138.87 ms | 140.57 ms | 239.99 ms |
| unc-scalar-point | 200 | A | 44,800 | 94.7 µs | 99.3 µs | 101.5 µs | 106.9 µs | 137.9 µs |
| unc-scalar-point | 200 | B | 44,800 | 95.9 µs | 100.0 µs | 102.3 µs | 107.8 µs | 139.4 µs |
| unc-scalar-point | 1,000 | A | 44,800 | 87.3 µs | 99.0 µs | 102.1 µs | 109.5 µs | 150.7 µs |
| unc-scalar-point | 1,000 | B | 44,800 | 88.1 µs | 99.7 µs | 102.8 µs | 110.0 µs | 143.6 µs |
| unc-scalar-point | 10,000 | A | 44,800 | 85.3 µs | 96.5 µs | 98.9 µs | 106.2 µs | 175.4 µs |
| unc-scalar-point | 10,000 | B | 44,800 | 86.6 µs | 97.2 µs | 99.7 µs | 106.6 µs | 173.0 µs |
| nested-none-point | 200 | A | 44,800 | 102.6 µs | 107.5 µs | 113.8 µs | 118.9 µs | 149.0 µs |
| nested-none-point | 200 | B | 44,800 | 102.7 µs | 106.8 µs | 108.3 µs | 112.7 µs | 138.6 µs |
| nested-none-point | 1,000 | A | 44,800 | 102.4 µs | 108.8 µs | 114.7 µs | 122.0 µs | 152.9 µs |
| nested-none-point | 1,000 | B | 44,800 | 103.1 µs | 106.7 µs | 108.1 µs | 112.8 µs | 139.0 µs |
| nested-none-point | 10,000 | A | 44,800 | 96.6 µs | 109.0 µs | 113.2 µs | 120.2 µs | 187.2 µs |
| nested-none-point | 10,000 | B | 44,800 | 100.0 µs | 106.7 µs | 108.5 µs | 118.0 µs | 186.8 µs |
| nested-none-scan | 200 | A | 22,400 | 525.3 µs | 542.8 µs | 552.6 µs | 561.3 µs | 586.1 µs |
| nested-none-scan | 200 | B | 22,400 | 969.9 µs | 1,010.6 µs | 1,015.4 µs | 1,020.1 µs | 1,068.1 µs |
| nested-none-scan | 1,000 | A | 4,480 | 3,020.9 µs | 3,076.8 µs | 3,096.1 µs | 3,123.9 µs | 3,278.4 µs |
| nested-none-scan | 1,000 | B | 4,480 | 4,486.0 µs | 4,565.5 µs | 4,602.7 µs | 4,640.2 µs | 4,957.5 µs |
| nested-none-scan | 10,000 | A | 448 | 30.21 ms | 30.45 ms | 30.58 ms | 31.60 ms | 67.19 ms |
| nested-none-scan | 10,000 | B | 448 | 43.97 ms | 44.32 ms | 44.60 ms | 45.46 ms | 91.30 ms |
| nested-some-point | 200 | A | 44,800 | 102.3 µs | 108.5 µs | 114.6 µs | 122.8 µs | 184.5 µs |
| nested-some-point | 200 | B | 44,800 | 105.1 µs | 112.1 µs | 118.0 µs | 126.8 µs | 194.6 µs |
| nested-some-point | 1,000 | A | 44,800 | 102.4 µs | 108.9 µs | 114.8 µs | 121.7 µs | 153.8 µs |
| nested-some-point | 1,000 | B | 44,800 | 105.3 µs | 113.1 µs | 119.1 µs | 126.8 µs | 159.5 µs |
| nested-some-point | 10,000 | A | 44,800 | 102.4 µs | 108.3 µs | 112.4 µs | 116.5 µs | 139.4 µs |
| nested-some-point | 10,000 | B | 44,800 | 106.0 µs | 112.4 µs | 116.4 µs | 121.0 µs | 145.7 µs |
| nested-some-scan | 200 | A | 22,400 | 524.9 µs | 544.5 µs | 553.8 µs | 566.1 µs | 1,218.7 µs |
| nested-some-scan | 200 | B | 22,400 | 723.1 µs | 755.8 µs | 765.7 µs | 775.9 µs | 1,709.0 µs |
| nested-some-scan | 1,000 | A | 4,480 | 3,022.2 µs | 3,076.2 µs | 3,092.9 µs | 3,110.4 µs | 3,223.7 µs |
| nested-some-scan | 1,000 | B | 4,480 | 4,066.0 µs | 4,129.7 µs | 4,149.9 µs | 4,170.8 µs | 4,297.7 µs |
| nested-some-scan | 10,000 | A | 448 | 30.16 ms | 30.45 ms | 30.56 ms | 30.74 ms | 35.90 ms |
| nested-some-scan | 10,000 | B | 448 | 40.42 ms | 40.79 ms | 41.07 ms | 41.34 ms | 45.91 ms |
| ctl-select-pk | 200 | A | 44,800 | 81.6 µs | 93.7 µs | 95.4 µs | 103.7 µs | 164.3 µs |
| ctl-select-pk | 200 | B | 44,800 | 81.3 µs | 94.4 µs | 96.2 µs | 105.0 µs | 168.8 µs |
| ctl-select-pk | 1,000 | A | 44,800 | 89.6 µs | 93.7 µs | 95.8 µs | 103.9 µs | 133.9 µs |
| ctl-select-pk | 1,000 | B | 44,800 | 90.3 µs | 94.7 µs | 96.7 µs | 104.8 µs | 135.3 µs |
| ctl-select-pk | 10,000 | A | 44,800 | 84.2 µs | 91.8 µs | 93.1 µs | 97.0 µs | 119.1 µs |
| ctl-select-pk | 10,000 | B | 44,800 | 85.3 µs | 92.6 µs | 93.9 µs | 97.8 µs | 120.9 µs |
| ctl-select-pk-again | 200 | A | 44,800 | 80.5 µs | 93.8 µs | 95.5 µs | 103.5 µs | 167.9 µs |
| ctl-select-pk-again | 200 | B | 44,800 | 81.0 µs | 94.4 µs | 96.2 µs | 104.4 µs | 168.7 µs |
| ctl-select-pk-again | 1,000 | A | 44,800 | 89.5 µs | 94.0 µs | 96.0 µs | 103.5 µs | 169.7 µs |
| ctl-select-pk-again | 1,000 | B | 44,800 | 89.7 µs | 95.3 µs | 97.2 µs | 104.8 µs | 171.6 µs |
| ctl-select-pk-again | 10,000 | A | 44,800 | 86.9 µs | 91.6 µs | 93.3 µs | 98.9 µs | 127.2 µs |
| ctl-select-pk-again | 10,000 | B | 44,800 | 87.7 µs | 92.0 µs | 93.3 µs | 97.7 µs | 122.8 µs |
| ctl-insert | 200 | A | 44,800 | 64.1 µs | 71.4 µs | 72.4 µs | 75.3 µs | 103.6 µs |
| ctl-insert | 200 | B | 44,800 | 64.7 µs | 71.9 µs | 72.8 µs | 75.8 µs | 105.0 µs |
| ctl-insert | 1,000 | A | 44,800 | 56.4 µs | 71.5 µs | 72.8 µs | 79.0 µs | 150.6 µs |
| ctl-insert | 1,000 | B | 44,800 | 59.9 µs | 71.9 µs | 73.2 µs | 78.9 µs | 131.7 µs |
| ctl-insert | 10,000 | A | 44,800 | 62.7 µs | 71.7 µs | 72.9 µs | 78.5 µs | 152.1 µs |
| ctl-insert | 10,000 | B | 44,800 | 64.9 µs | 71.7 µs | 72.9 µs | 78.3 µs | 144.8 µs |
| ctl-ping | 200 | A | 44,800 | 68.6 µs | 71.4 µs | 72.5 µs | 74.8 µs | 94.3 µs |
| ctl-ping | 200 | B | 44,800 | 67.2 µs | 71.4 µs | 72.6 µs | 74.8 µs | 94.1 µs |
| ctl-ping | 1,000 | A | 44,800 | 62.3 µs | 73.8 µs | 75.3 µs | 80.8 µs | 128.1 µs |
| ctl-ping | 1,000 | B | 44,800 | 61.8 µs | 74.0 µs | 75.4 µs | 80.8 µs | 125.8 µs |
| ctl-ping | 10,000 | A | 44,800 | 67.4 µs | 73.9 µs | 75.2 µs | 79.1 µs | 109.4 µs |
| ctl-ping | 10,000 | B | 44,800 | 67.9 µs | 74.1 µs | 75.4 µs | 79.2 µs | 110.0 µs |
| ctl-scan-filter | 200 | A | 22,400 | 139.6 µs | 144.5 µs | 147.4 µs | 158.4 µs | 296.4 µs |
| ctl-scan-filter | 200 | B | 22,400 | 140.5 µs | 145.4 µs | 148.4 µs | 159.4 µs | 292.6 µs |
| ctl-scan-filter | 1,000 | A | 4,480 | 334.9 µs | 348.0 µs | 359.1 µs | 370.9 µs | 445.4 µs |
| ctl-scan-filter | 1,000 | B | 4,480 | 336.1 µs | 348.0 µs | 359.0 µs | 370.4 µs | 434.5 µs |
| ctl-scan-filter | 10,000 | A | 448 | 2,547.9 µs | 2,591.0 µs | 2,611.7 µs | 2,641.1 µs | 5,937.1 µs |
| ctl-scan-filter | 10,000 | B | 448 | 2,553.7 µs | 2,597.2 µs | 2,615.5 µs | 2,644.9 µs | 3,271.6 µs |
| ctl-scan-filter-again | 200 | A | 22,400 | 140.4 µs | 144.7 µs | 147.3 µs | 155.8 µs | 283.4 µs |
| ctl-scan-filter-again | 200 | B | 22,400 | 140.1 µs | 145.4 µs | 148.0 µs | 156.1 µs | 277.5 µs |
| ctl-scan-filter-again | 1,000 | A | 4,480 | 334.1 µs | 348.4 µs | 358.9 µs | 370.2 µs | 432.4 µs |
| ctl-scan-filter-again | 1,000 | B | 4,480 | 336.7 µs | 348.4 µs | 359.8 µs | 370.0 µs | 431.8 µs |
| ctl-scan-filter-again | 10,000 | A | 448 | 2,545.8 µs | 2,591.5 µs | 2,612.7 µs | 2,645.0 µs | 5,930.4 µs |
| ctl-scan-filter-again | 10,000 | B | 448 | 2,547.7 µs | 2,597.6 µs | 2,615.4 µs | 2,650.6 µs | 3,006.6 µs |

### The per-cell deltas of every arm

| Arm | Rows | A p50 | A engine per statement | B p50 | median per-run Δp50 | IQR of per-run Δp50 | median per-run Δp99 | IQR of per-run Δp99 | median Δ engine per statement | IQR Δ engine | Δp50 per outer row |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| corr-exists-point | 200 | 109.3 µs | 42.9 µs | 109.2 µs | +0.5 µs | [-0.1, +0.6] µs | +2.1 µs | [+0.2, +4.7] µs | +0.0 µs | [-0.0, +0.4] µs | n/a |
| corr-exists-point | 1,000 | 109.3 µs | 44.0 µs | 110.4 µs | +1.2 µs | [+0.7, +1.4] µs | +2.0 µs | [+0.5, +2.9] µs | +0.7 µs | [+0.5, +0.8] µs | n/a |
| corr-exists-point | 10,000 | 107.0 µs | 42.5 µs | 108.1 µs | +1.1 µs | [+0.5, +1.1] µs | +1.6 µs | [+0.1, +2.2] µs | +0.4 µs | [+0.3, +0.5] µs | n/a |
| corr-notexists-point | 200 | 107.5 µs | 42.8 µs | 108.2 µs | +0.8 µs | [+0.5, +1.2] µs | +1.0 µs | [+0.4, +2.0] µs | +0.4 µs | [+0.3, +0.6] µs | n/a |
| corr-notexists-point | 1,000 | 108.2 µs | 44.1 µs | 109.5 µs | +1.0 µs | [+0.7, +1.3] µs | +2.1 µs | [+0.6, +3.2] µs | +0.6 µs | [+0.4, +0.8] µs | n/a |
| corr-notexists-point | 10,000 | 106.8 µs | 42.7 µs | 107.1 µs | +1.1 µs | [+0.5, +1.1] µs | +0.3 µs | [-3.6, +1.0] µs | +0.4 µs | [+0.1, +0.9] µs | n/a |
| corr-exists-scan | 200 | 552.2 µs | 355.5 µs | 554.8 µs | +7.2 µs | [+4.5, +9.9] µs | +2.8 µs | [-9.2, +19.3] µs | +7.6 µs | [+1.6, +9.9] µs | +36.0 ns |
| corr-exists-scan | 1,000 | 3,079.9 µs | 2,413.8 µs | 3,113.2 µs | +38.3 µs | [+29.3, +39.4] µs | +70.0 µs | [-68.2, +224.5] µs | +39.1 µs | [+29.2, +51.6] µs | +38.3 ns |
| corr-exists-scan | 10,000 | 30.64 ms | 24.76 ms | 30.99 ms | +282.1 µs | [+237.8, +925.8] µs | -1,303.5 µs | [-20180.2, +763.8] µs | +198.6 µs | [+165.3, +273.0] µs | +28.2 ns |
| unc-exists-point | 200 | 105.3 µs | 41.0 µs | 106.7 µs | +0.6 µs | [+0.4, +1.3] µs | +2.6 µs | [+1.3, +6.5] µs | +0.3 µs | [+0.2, +0.6] µs | n/a |
| unc-exists-point | 1,000 | 106.3 µs | 42.3 µs | 107.9 µs | +1.4 µs | [+0.9, +1.9] µs | +0.5 µs | [-2.1, +1.8] µs | +0.6 µs | [+0.4, +0.8] µs | n/a |
| unc-exists-point | 10,000 | 103.7 µs | 39.5 µs | 104.7 µs | +1.3 µs | [+0.7, +1.9] µs | +2.1 µs | [-0.1, +2.6] µs | +0.5 µs | [+0.4, +1.4] µs | n/a |
| unc-exists-scan | 200 | 156.5 µs | 57.6 µs | 158.1 µs | +0.8 µs | [+0.5, +1.8] µs | +2.2 µs | [+0.8, +2.8] µs | +0.7 µs | [+0.4, +1.1] µs | +4.0 ns |
| unc-exists-scan | 1,000 | 365.8 µs | 124.1 µs | 366.3 µs | +0.9 µs | [+0.5, +1.8] µs | +0.9 µs | [-3.2, +5.5] µs | +0.9 µs | [-0.1, +1.1] µs | +0.9 ns |
| unc-exists-scan | 10,000 | 2,618.5 µs | 858.7 µs | 2,623.2 µs | +5.0 µs | [-0.3, +6.0] µs | +2,093.4 µs | [+1917.0, +2201.9] µs | -2.7 µs | [-4.9, +3.4] µs | +0.5 ns |
| unc-in-point | 200 | 108.6 µs | 47.5 µs | 109.4 µs | +0.7 µs | [+0.5, +1.2] µs | +0.5 µs | [-0.4, +3.0] µs | +0.5 µs | [+0.2, +0.9] µs | n/a |
| unc-in-point | 1,000 | 113.6 µs | 53.1 µs | 115.6 µs | +1.6 µs | [+1.5, +2.1] µs | +1.7 µs | [+0.9, +3.8] µs | +1.2 µs | [+1.0, +1.5] µs | n/a |
| unc-in-point | 10,000 | 111.7 µs | 51.5 µs | 113.8 µs | +1.4 µs | [+1.2, +1.8] µs | +0.2 µs | [-0.2, +1.5] µs | +0.8 µs | [+0.7, +1.4] µs | n/a |
| unc-in-scan | 200 | 1,998.9 µs | 1,903.3 µs | 1,995.2 µs | -1.1 µs | [-5.9, +1.3] µs | -8.4 µs | [-110.2, +14.6] µs | -3.4 µs | [-8.2, -3.0] µs | -5.5 ns |
| unc-in-scan | 1,000 | 15.93 ms | 15.88 ms | 16.05 ms | +228.0 µs | [-11.2, +399.2] µs | -364.1 µs | [-2348.8, +137.8] µs | +253.9 µs | [+45.8, +398.8] µs | +228.0 ns |
| unc-in-scan | 10,000 | 138.53 ms | 138.78 ms | 138.45 ms | -245.5 µs | [-471.9, +492.0] µs | -6,552.2 µs | [-45549.3, -900.8] µs | +358.4 µs | [-421.4, +1329.3] µs | -24.6 ns |
| unc-scalar-point | 200 | 101.5 µs | 39.8 µs | 102.4 µs | +0.6 µs | [+0.4, +1.1] µs | +2.0 µs | [-0.2, +3.0] µs | +0.3 µs | [+0.1, +0.8] µs | n/a |
| unc-scalar-point | 1,000 | 101.8 µs | 40.7 µs | 102.7 µs | +0.5 µs | [-0.1, +1.0] µs | +0.6 µs | [-1.1, +4.8] µs | +0.3 µs | [+0.0, +0.6] µs | n/a |
| unc-scalar-point | 10,000 | 98.3 µs | 37.6 µs | 99.2 µs | +0.3 µs | [+0.2, +0.8] µs | +0.4 µs | [-1.0, +2.1] µs | +0.2 µs | [+0.2, +0.6] µs | n/a |
| nested-none-point | 200 | 113.9 µs | 46.2 µs | 108.0 µs | -5.9 µs | [-6.2, -5.2] µs | -11.1 µs | [-11.9, -9.1] µs | -0.1 µs | [-0.4, +0.5] µs | n/a |
| nested-none-point | 1,000 | 114.5 µs | 47.7 µs | 108.0 µs | -6.5 µs | [-7.1, -6.4] µs | -13.2 µs | [-14.9, -12.4] µs | -1.8 µs | [-1.8, -1.5] µs | n/a |
| nested-none-point | 10,000 | 112.6 µs | 45.8 µs | 107.9 µs | -4.8 µs | [-5.1, -4.2] µs | -2.4 µs | [-3.1, -1.2] µs | +0.0 µs | [+0.0, +0.6] µs | n/a |
| nested-none-scan | 200 | 552.1 µs | 354.5 µs | 1,015.4 µs | +463.4 µs | [+461.0, +465.7] µs | +483.6 µs | [+481.0, +485.0] µs | +585.8 µs | [+585.4, +588.8] µs | +2317.0 ns |
| nested-none-scan | 1,000 | 3,094.9 µs | 2,423.6 µs | 4,598.0 µs | +1,482.8 µs | [+1471.8, +1523.6] µs | +1,647.4 µs | [+1609.6, +1751.1] µs | +2,080.7 µs | [+2069.8, +2132.9] µs | +1482.8 ns |
| nested-none-scan | 10,000 | 30.53 ms | 24.50 ms | 44.54 ms | +13,992.6 µs | [+13820.8, +14563.8] µs | +12,549.4 µs | [+11277.2, +13513.2] µs | +20,069.4 µs | [+19856.6, +20693.8] µs | +1399.3 ns |
| nested-some-point | 200 | 114.1 µs | 46.3 µs | 117.5 µs | +2.9 µs | [+2.7, +3.5] µs | +3.6 µs | [+3.4, +7.4] µs | +2.7 µs | [+2.6, +3.1] µs | n/a |
| nested-some-point | 1,000 | 114.6 µs | 47.7 µs | 119.0 µs | +4.2 µs | [+4.1, +4.4] µs | +6.0 µs | [+4.5, +6.7] µs | +3.4 µs | [+3.3, +3.5] µs | n/a |
| nested-some-point | 10,000 | 112.3 µs | 45.6 µs | 116.5 µs | +4.2 µs | [+3.2, +4.6] µs | +5.8 µs | [+5.1, +7.3] µs | +3.3 µs | [+3.2, +3.8] µs | n/a |
| nested-some-scan | 200 | 552.5 µs | 354.6 µs | 764.9 µs | +213.8 µs | [+208.3, +215.7] µs | +223.8 µs | [+218.1, +244.5] µs | +212.0 µs | [+206.5, +213.0] µs | +1069.0 ns |
| nested-some-scan | 1,000 | 3,091.3 µs | 2,422.5 µs | 4,155.6 µs | +1,056.7 µs | [+1048.2, +1062.8] µs | +1,076.8 µs | [+1033.0, +1122.8] µs | +1,049.5 µs | [+1045.4, +1059.0] µs | +1056.7 ns |
| nested-some-scan | 10,000 | 30.51 ms | 24.46 ms | 41.01 ms | +10,566.0 µs | [+10266.2, +10578.6] µs | +11,598.7 µs | [+9481.2, +12550.1] µs | +10,545.5 µs | [+10251.7, +10619.0] µs | +1056.6 ns |
| ctl-select-pk | 200 | 95.0 µs | 32.4 µs | 96.1 µs | +0.8 µs | [+0.2, +1.4] µs | +0.3 µs | [-1.2, +2.5] µs | +0.1 µs | [-0.1, +0.6] µs | n/a |
| ctl-select-pk | 1,000 | 95.4 µs | 33.2 µs | 96.7 µs | +0.8 µs | [+0.6, +1.4] µs | +1.3 µs | [-0.0, +2.4] µs | +0.3 µs | [+0.1, +0.6] µs | n/a |
| ctl-select-pk | 10,000 | 92.8 µs | 30.5 µs | 93.7 µs | +1.0 µs | [+0.3, +1.3] µs | +0.3 µs | [+0.1, +2.1] µs | +0.6 µs | [+0.2, +0.9] µs | n/a |
| ctl-select-pk-again | 200 | 95.0 µs | 32.1 µs | 96.1 µs | +0.8 µs | [+0.2, +1.1] µs | +0.7 µs | [-0.2, +1.4] µs | +0.1 µs | [-0.1, +0.4] µs | n/a |
| ctl-select-pk-again | 1,000 | 95.3 µs | 32.5 µs | 96.6 µs | +1.1 µs | [+0.5, +1.6] µs | +2.5 µs | [+1.6, +3.7] µs | +0.5 µs | [+0.1, +0.9] µs | n/a |
| ctl-select-pk-again | 10,000 | 92.9 µs | 31.1 µs | 93.1 µs | +0.2 µs | [-0.3, +0.6] µs | -4.8 µs | [-6.2, -4.2] µs | -0.3 µs | [-0.7, -0.3] µs | n/a |
| ctl-insert | 200 | 72.5 µs | 24.6 µs | 72.6 µs | +0.4 µs | [-0.1, +0.9] µs | +0.9 µs | [+0.0, +2.0] µs | +0.4 µs | [+0.2, +0.6] µs | n/a |
| ctl-insert | 1,000 | 72.4 µs | 24.7 µs | 72.9 µs | +0.4 µs | [-0.0, +0.6] µs | +0.5 µs | [+0.4, +1.9] µs | +0.4 µs | [+0.0, +0.4] µs | n/a |
| ctl-insert | 10,000 | 72.4 µs | 24.8 µs | 72.5 µs | +0.0 µs | [-0.3, +0.1] µs | +2.5 µs | [+0.9, +7.7] µs | +0.1 µs | [-0.0, +0.2] µs | n/a |
| ctl-ping | 200 | 72.5 µs | 30.6 µs | 72.4 µs | -0.1 µs | [-0.6, +0.6] µs | -0.1 µs | [-1.0, +0.6] µs | +0.3 µs | [+0.2, +0.4] µs | n/a |
| ctl-ping | 1,000 | 74.7 µs | 26.9 µs | 74.9 µs | +0.1 µs | [-0.1, +0.3] µs | +0.5 µs | [+0.1, +0.9] µs | +0.2 µs | [+0.1, +0.3] µs | n/a |
| ctl-ping | 10,000 | 74.8 µs | 27.0 µs | 75.3 µs | +0.5 µs | [-0.4, +0.6] µs | +0.4 µs | [-0.3, +0.8] µs | +0.2 µs | [+0.1, +0.3] µs | n/a |
| ctl-scan-filter | 200 | 146.4 µs | 51.3 µs | 147.9 µs | +0.6 µs | [+0.4, +1.2] µs | +0.9 µs | [+0.2, +7.0] µs | +0.4 µs | [+0.3, +0.6] µs | +3.0 ns |
| ctl-scan-filter | 1,000 | 356.1 µs | 118.0 µs | 355.7 µs | -0.4 µs | [-1.3, -0.2] µs | +0.8 µs | [-3.2, +5.1] µs | -0.4 µs | [-0.5, -0.2] µs | -0.4 ns |
| ctl-scan-filter | 10,000 | 2,612.9 µs | 849.0 µs | 2,612.3 µs | +5.0 µs | [-1.9, +7.9] µs | -2,888.5 µs | [-2950.5, -2777.6] µs | +2.3 µs | [-2.3, +7.1] µs | +0.5 ns |
| ctl-scan-filter-again | 200 | 146.9 µs | 51.5 µs | 147.7 µs | +0.3 µs | [+0.0, +1.0] µs | -0.7 µs | [-1.3, +1.1] µs | +0.3 µs | [+0.2, +0.5] µs | +1.5 ns |
| ctl-scan-filter-again | 1,000 | 356.3 µs | 117.4 µs | 357.0 µs | +0.5 µs | [+0.1, +0.7] µs | -2.0 µs | [-5.3, -1.0] µs | -0.2 µs | [-0.5, +0.2] µs | +0.5 ns |
| ctl-scan-filter-again | 10,000 | 2,603.4 µs | 848.8 µs | 2,604.1 µs | +5.3 µs | [+0.2, +9.1] µs | -3,335.9 µs | [-3507.3, -3214.9] µs | +5.5 µs | [-0.4, +8.6] µs | +0.5 ns |

Everything above is reproducible from `archive/nested-hoist-overhead-v2.7.0-749-g23e53bed/`:
`drivers/nh_run.py` (`python3 nh_run.py 8 200,1000,10000 <out>` and `NH_SWAP=1` for the swap pass),
`drivers/analyze.py <out> 5` and `drivers/render.py <out>` for the tables, the gzipped per-run
JSON (every statement's latency by block, the engine delta, the reply row counts, the host's load),
`drivers/extra.py` for the swap, paired-block and host tables, and `analyze-plans.txt`.
