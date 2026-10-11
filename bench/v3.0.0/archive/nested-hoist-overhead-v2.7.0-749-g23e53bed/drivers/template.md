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

{{DELTA:corr-exists-scan}}

Paired by block (112 blocks per cell: 7 runs x 16 blocks, B − A on the block's own median), the sign
holds without the run structure: B is slower in 101, 112 and 94 of 112 blocks. The pk `SELECT` control
is slower by the same proportion (+0.8 to +1.2 %), which is what the point arms below carry; that control
does not explain the scan arms' *growth* with the row count, which a per-statement offset of 0.9 µs cannot.

{{PAIRED:corr-exists-scan,corr-exists-point,ctl-select-pk,ctl-scan-filter,ctl-scan-filter-again}}

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

{{DELTA:corr-exists-point,corr-notexists-point,unc-exists-point,unc-exists-scan,unc-in-point,unc-scalar-point}}

**The uncorrelated `IN` evaluated per outer row** goes through the same `EvaluateSubChain` once per
outer row but costs 10 to 14 µs a row (a 20-row range walk of `b` plus the `IN` build). A 35 ns
per-row cost would be 0.25 to 0.35 % of it; the cell's own spread is larger: Δp50 is -1.1 µs [-5.9, +1.3]
at 200 rows, +228 µs [-11, +399] at 1,000 and -246 µs [-472, +492] at 10,000, with B slower in 3, 5 and 3
of 7 runs. **The measurement cannot see a per-row cost of that size here, in either direction.**

{{DELTA:unc-in-scan}}

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

{{DELTA:ctl-select-pk,ctl-select-pk-again,ctl-insert,ctl-ping,ctl-scan-filter,ctl-scan-filter-again}}

## 6. The slot-swap confirmation

The controls show B a little slower on a pk `SELECT`, so the next question is whether that offset and
the correlated scan's follow the **binary** or the **slot** (the second server of a pair, its port,
its start order). The swap pass runs the same driver with the A binary in slot B and the B binary in
slot A (5 runs per size). In the table, the first value column is B − A with the B binary in slot B (the main matrix); the second is
the swap pass's slot-B − slot-A **with its sign flipped**, so it is B − A again; the last two columns split the
average of the two into what follows the binary and what follows the slot.

{{SWAP:corr-exists-scan,corr-exists-point,unc-in-scan,ctl-select-pk,ctl-select-pk-again,ctl-insert,ctl-ping,ctl-scan-filter,ctl-scan-filter-again}}

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

{{DELTA:nested-none-point,nested-none-scan,nested-some-point,nested-some-scan}}

## 8. Throughput, as a matrix

Derived: tps = 1,000,000 / the median over runs of the per-run mean (µs); the driver is serial, so the
conversion is exact. **The mean is outlier-driven on scan arms and disagrees with the p50 there**:
`corr-exists-scan` reads +3.1 % and +7.8 % tps for B at 1,000 and 10,000 rows here while its p50 is slower
in 7 of 7 runs. The swap pass shows why: the mean's sign follows the **slot** (slot B's mean is lower by
about 100 to 160 µs at 1,000 rows in both passes, whichever binary it holds), while the p50 follows the
binary. The sections above are read from the p50 and the block pairs; this matrix is the throughput view
the rules ask for, and a tps delta on a scan row of it is not a finding.

{{MATRIX}}

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

{{DIST}}

### The per-cell deltas of every arm

{{DELTA:ALL}}

Everything above is reproducible from `archive/nested-hoist-overhead-v2.7.0-749-g23e53bed/`:
`drivers/nh_run.py` (`python3 nh_run.py 8 200,1000,10000 <out>` and `NH_SWAP=1` for the swap pass),
`drivers/analyze.py <out> 5` and `drivers/render.py <out>` for the tables, the gzipped per-run
JSON (every statement's latency by block, the engine delta, the reply row counts, the host's load),
`drivers/extra.py` for the swap, paired-block and host tables, and `analyze-plans.txt`.
