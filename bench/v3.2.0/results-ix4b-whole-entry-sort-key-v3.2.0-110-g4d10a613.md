# IX4b close overhead: a whole-entry index sort key adds a level to a wide-covering tree and costs at most +0.3 us (+0.7 %) on an INSERT, nothing resolvable on a probe or an UPDATE

**Thesis.** On `worktree-fix-index-equal-sort-key-run`, with the engine at `4d10a613` (`v3.2.0-110-g4d10a613`, **B**) against `0479cb29` (`v3.2.0-108-g0479cb29`, **A**, B minus this change only), making a secondary index's sort key the whole entry (key, tuple key, covered bytes) leaves the **narrow-covering** index (41-byte entries) at height 2 on both sides and its statements no slower (B is 0.1 to 1.0 us *below* the A baseline, -0.9 % to -3.1 %, which CLA reads as binary layout, not a gain, section 3). On the **wide-covering** index (529-byte entries, four `varchar(128)` cells) the tree is **one level deeper on B at every size** (height 3 against 2 at 200 and 1,000 rows, 4 against 3 at 10,000), and the only cell that resolves a cost is the `INSERT`: **+0.26 us of server CPU per statement pooled (+0.7 %), +0.5 us (+1.2 % to +1.4 %) at 10,000 rows**, B above A in 51 of 72 paired runs against 31 of 72 for the A2 control. A probe through the deeper tree costs -0.3 us (-0.9 %, inside the floor); an `UPDATE` of a covered column on distinct rows +0.08 us (+0.2 %, inside the floor). The change's reason for being shows on A, not B: **A refuses the covered-column `UPDATE`s of one hot row after 169 (narrow) or 20 (wide) of them; B accepted all 3,000 attempted on both layouts** (section 5).

Server CPU is the sum of `/proc/<pid>/task/*/schedstat` over the server's threads (`tools/bn_census.py`'s accounting, imported unmodified); wall is the client's. The driver is new and scratch (`ix_ab.py`, modelled on `tools/bx_ab.py`); it is not in the tree. `bench/docs/` stays closed (`bench/README.md`).

| Item | Value |
|---|---|
| Executed | 2026-10-15 23:57:47 to 2026-10-16 00:05:05 UTC (matrix, 36 runs); hot-row depth probe 00:05-00:06 UTC; pilots 23:56-23:58. Builds 23:53-23:56 UTC |
| Branch / worktree | `worktree-fix-index-equal-sort-key-run` (session worktree `fix-index-equal-sort-key-run`, HEAD `4d10a613`, tree clean at the start). The binaries were built from two detached checkouts (`git worktree add --detach`, `build-rel/`) outside that tree; the session's own `build/` and sources were not touched. The checkouts were removed afterwards |
| Commits measured | **A** = `0479cb29` (`v3.2.0-108-g0479cb29`, committed 23:43:34 UTC; origin/main as merged), **B** = `4d10a613` (`v3.2.0-110-g4d10a613`, committed 23:47:59 UTC). **A2** = A's bytes started as a second server (in-run control) |
| Binary provenance | `CMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server -j4`, A then B, both finished (23:55:11.75 and 23:56:22.42 UTC, both newer than their commits, so both are the commits' engines) before any measurement. **A and A2** sha256 `2cb423d1c80ad5a1f6ab0ab403e18f1879fef53884de739b17f2056a7f1eae5e`; **B** `3171dc08ac3dd0c5f2adb821b7e4e078ad5b0590e09c4fb25d2c3458116358f6`. Every server started from copies in `/home/cdkbs/bench-runs/ix4b/bin/` |
| Device | Data files and WAL under `/home/cdkbs/bench-runs/ix4b/wd/` on `/dev/root`, ext4 (`df -T`), not tmpfs |
| Build type | Release (above) |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, `scan_kernel` default; fresh server and fresh data file per arm per run (superblock 28 on B, 27 on A, so the two cannot share a file); server pinned to CPU 2, driver to CPU 4, one server busy at a time |
| Why `relaxed` | The BX close used it (`bench/v3.2.0/results-bx-close-v3.2.0-62-g9bf559e2.md`), and `group` durability is 82-85 % of an update's wall (project memory, "KDS benchmark host"), which would hide a sub-microsecond CPU effect. The percentages here are therefore the worst case as a share of a statement; under `group`/`strict` the same microseconds are a smaller share |
| Host load | 8 vCPU, Linux 7.0.0-1014-azure. `uptime` load average 5.95 at 23:53 (other sessions' tails and the builds), 1.63 at 00:05. Load average read before each of 720 cell-sets (2,160 arm records): maximum 2.59; `host_state()` found **0** competing build or test processes in every record; no `cc1plus` was running during any measurement; the builds finished before the first pilot. Watchdog: the driver wrote a progress line per run into `p.txt`; polled every 60 s, last line never older than 8 s, no stall, no orphan |
| Drivers | `ix_ab.py` (scratch, `/home/cdkbs/bench-runs/ix4b/`), `hot_depth.py` (scratch); `tools/bn_census.py` `35693dc24f17...`, `tools/kwp.py` and `tools/bb_overhead_benchmark.py` imported unmodified |
| Suite | Not executed in this task; no engine code was changed by this measurement |
| Prior number | None for this shape (wide and narrow covering-index cells have no earlier results file). **A is this change's baseline**; this file is the one the next run of the shape is read against. PostgreSQL floor: not measured for this shape |

## 1. What was run and what was cut

Three servers per run (A, A2, B), a fresh server and data file for each, side by side; each cell is cut into 6 blocks taken round-robin, the first server of a block rotating by one per block and per run, so each arm takes each position equally often. Each server holds two `BTREE` tables, each with one index on `owner` created **before** the load, so the tree is built by organic divides (owners are `id * 7919 mod rows/4`, so the load is out of key order and about 4 rows share an owner):

| table | columns | index | entry | rows per owner |
|---|---|---|---|---|
| `n` (narrow) | `id, owner, c1, c2, c3` all `int64` | `ix_n ON n(owner) COVERING (c1, c2, c3)` | 41 bytes (the `SHOW INDEXES` width column) | about 4 |
| `w` (wide) | `id, owner int64`, `c1..c4 varchar(128)` (100-character values, no spill) | `ix_w ON w(owner) COVERING (c1, c2, c3, c4)` | 529 bytes | about 4 |

`--rows` (the driver flag) is the row count loaded into each table: **200, 1,000 and 10,000** (rule 9). Shapes, each run as a literal (unnamed `C_PARSE` per execution) and as a prepared statement, in this order per server (probes first, so they see the table as loaded):

| shape | statement | statements per arm per run, per path |
|---|---|---|
| probe | `SELECT * FROM x WHERE owner = ?` (about 4 rows) | 3,000 |
| probe-filter | `SELECT id FROM x WHERE owner = ? AND c1 = ?` (c1 is a covered column) | 3,000 |
| insert | `INSERT INTO x VALUES (...)` with a fresh tuple key and an existing owner, so every insert lands mid-tree | 1,200 |
| update-cold | `UPDATE x SET c1 = ? WHERE id = ?`, distinct rows | `n`: min(3,000, 4 x rows); `w`: min(3,000, rows) (798 and 198 at 200 rows, after rounding to 6 blocks) |
| update-hot | the same statement on one row (id 1 literal, id 2 prepared) | `n`: 150; `w`: 6 |

Runs: **12 per size**, 36 in all, 720 timed cell-sets (3 sizes x 12 runs x 10 shapes x 2 paths), 2,160 arm records; every reply was checked against its expected tag and **0 statements were refused in the matrix** (the counts were set below A's refusal threshold on purpose; section 5 measures the threshold itself).

**Cut for the one-hour budget, and why:** a `strict` or `group` durability rung (the question is CPU per statement; section above); a second core; a table large enough to push the **narrow** index past height 2 (a 41-byte entry gives about 200 per page and roughly 40,000 rows to a third level; CLA did not load that, so the claim that a narrow index is unaffected is a claim about heights up to 2 only); a width between 41 and 529 bytes; deletes; multi-row `INSERT`; the hot-row cells on `w` are 6 statements per path (A refuses `w` after about 20, section 5), so they carry almost no resolving power and are reported as such; the update-cold cells on `w` are one update per row per path, because a pilot at 200 rows showed A refusing at 8 updates per row. No PostgreSQL twin: the floor was not measured for this shape. No sampler run, so the split of an insert's cost between the descent, the divide and the log is unmeasured.

**Pilots, not in the matrix.** Two single-run pilots preceded the matrix on the first driver revision (300 hot statements per path, inserts after the hot updates). On A they refused 56 of 300 narrow and 286 of 300 wide hot updates and then 5 of 1,200 narrow and 2 of 1,200 wide *inserts* into the same table, with `index descent ... gave up after 5 attempts` (the tree already held a separator its parent could not take); B refused nothing. That ordering was changed (inserts first, hot updates last, counts below the threshold) so that every matrix cell compares the same operation on both sides.

## 2. Deltas, per size

Server CPU per statement in us, median over 12 runs of each arm; the delta column is B minus the mean of A and A2 for the same run (paired), median [min, max]. `% ` is that median over A's median. `A2-A` is the control: the same bytes as A, a second server. `B>A` and `A2>A` count runs in which B (or A2) is above A. `A refused`/`B refused` count statements the server refused over all 12 runs (0 everywhere in this matrix). Shape names: `probe` = `SELECT * ... WHERE owner = ?` (4 rows); `probe-filter` = `SELECT id ... WHERE owner = ? AND c1 = ?` (1 or more rows, the filter column is covered); `insert`, `update-cold` (distinct rows, `SET c1 = ?`), `update-hot` (one row, `SET c1 = ?`, 150 statements per path on `n`, 6 on `w`). Table `n` = narrow covering, `w` = wide covering.

### 2.1 200 rows

| table | shape | path | ops/arm/run | A cpu | A2 cpu | B cpu | B - mean(A,A2) med [min,max] | % | A2-A med | sd(A2-A) | B>A | A2>A | A refused stmts | B refused |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| n | probe | literal | 3000 | 34.23 | 34.09 | 33.84 | -0.58 [-1.24,+0.68] | -1.7 | -0.12 | 0.67 | 3/12 | 5/12 | 0 | 0 |
| n | probe | prepared | 3000 | 33.42 | 33.43 | 33.03 | -0.39 [-1.39,+0.44] | -1.2 | -0.05 | 0.85 | 3/12 | 6/12 | 0 | 0 |
| n | probe-filter | literal | 3000 | 34.41 | 34.01 | 33.65 | -0.64 [-1.54,+0.52] | -1.9 | +0.05 | 0.79 | 4/12 | 6/12 | 0 | 0 |
| n | probe-filter | prepared | 3000 | 33.73 | 33.19 | 33.05 | -0.45 [-1.45,+0.37] | -1.3 | -0.20 | 0.88 | 1/12 | 5/12 | 0 | 0 |
| n | insert | literal | 1200 | 28.02 | 27.68 | 27.73 | -0.09 [-1.25,+0.54] | -0.3 | -0.11 | 0.77 | 5/12 | 5/12 | 0 | 0 |
| n | insert | prepared | 1200 | 26.94 | 26.96 | 26.75 | -0.21 [-1.69,+0.75] | -0.8 | +0.04 | 0.67 | 4/12 | 7/12 | 0 | 0 |
| n | update-cold | literal | 798 | 31.48 | 30.95 | 30.76 | -0.38 [-1.53,+0.91] | -1.2 | -0.41 | 0.94 | 4/12 | 4/12 | 0 | 0 |
| n | update-cold | prepared | 798 | 29.98 | 29.51 | 29.30 | -0.32 [-1.27,+1.31] | -1.1 | -0.40 | 0.97 | 3/12 | 4/12 | 0 | 0 |
| n | update-hot | literal | 150 | 32.93 | 32.73 | 31.62 | -1.17 [-1.61,+0.82] | -3.6 | -0.01 | 1.18 | 1/12 | 6/12 | 0 | 0 |
| n | update-hot | prepared | 150 | 31.73 | 31.64 | 31.36 | -0.95 [-2.21,+0.69] | -3.0 | -0.13 | 1.36 | 3/12 | 5/12 | 0 | 0 |
| w | probe | literal | 3000 | 36.17 | 36.22 | 35.84 | -0.24 [-1.35,+0.49] | -0.7 | +0.00 | 0.91 | 4/12 | 6/12 | 0 | 0 |
| w | probe | prepared | 3000 | 35.57 | 35.46 | 35.01 | -0.62 [-1.64,+0.64] | -1.7 | -0.07 | 1.05 | 3/12 | 6/12 | 0 | 0 |
| w | probe-filter | literal | 3000 | 36.15 | 35.96 | 35.50 | -0.37 [-1.04,+0.77] | -1.0 | -0.01 | 0.79 | 5/12 | 6/12 | 0 | 0 |
| w | probe-filter | prepared | 3000 | 35.01 | 35.10 | 34.66 | -0.31 [-1.52,+1.11] | -0.9 | -0.36 | 0.80 | 6/12 | 5/12 | 0 | 0 |
| w | insert | literal | 1200 | 38.48 | 38.12 | 38.41 | +0.29 [-0.79,+0.96] | +0.8 | -0.08 | 0.65 | 9/12 | 5/12 | 0 | 0 |
| w | insert | prepared | 1200 | 36.97 | 36.92 | 37.09 | +0.23 [-0.87,+0.73] | +0.6 | -0.17 | 0.70 | 9/12 | 5/12 | 0 | 0 |
| w | update-cold | literal | 198 | 36.95 | 37.18 | 36.50 | -0.14 [-2.26,+2.72] | -0.4 | +0.21 | 1.48 | 6/12 | 6/12 | 0 | 0 |
| w | update-cold | prepared | 198 | 35.84 | 35.89 | 35.95 | +0.24 [-0.81,+1.54] | +0.7 | -0.00 | 1.30 | 7/12 | 6/12 | 0 | 0 |
| w | update-hot | literal | 6 | 71.38 | 68.92 | 69.41 | +0.94 [-10.35,+4.93] | +1.3 | -2.65 | 5.29 | 8/12 | 4/12 | 0 | 0 |
| w | update-hot | prepared | 6 | 60.72 | 62.22 | 61.33 | -0.89 [-4.40,+6.81] | -1.5 | +2.04 | 4.72 | 4/12 | 7/12 | 0 | 0 |

### 2.2 1000 rows

| table | shape | path | ops/arm/run | A cpu | A2 cpu | B cpu | B - mean(A,A2) med [min,max] | % | A2-A med | sd(A2-A) | B>A | A2>A | A refused stmts | B refused |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| n | probe | literal | 3000 | 34.71 | 34.63 | 34.44 | -0.21 [-0.81,+0.90] | -0.6 | -0.29 | 0.56 | 3/12 | 4/12 | 0 | 0 |
| n | probe | prepared | 3000 | 33.84 | 33.64 | 33.52 | -0.15 [-1.33,+0.87] | -0.4 | -0.24 | 0.87 | 6/12 | 5/12 | 0 | 0 |
| n | probe-filter | literal | 3000 | 34.30 | 34.15 | 34.13 | -0.21 [-1.30,+0.61] | -0.6 | +0.01 | 0.55 | 5/12 | 6/12 | 0 | 0 |
| n | probe-filter | prepared | 3000 | 33.79 | 33.51 | 33.26 | -0.10 [-1.13,+1.40] | -0.3 | -0.21 | 0.52 | 4/12 | 3/12 | 0 | 0 |
| n | insert | literal | 1200 | 27.84 | 28.17 | 27.48 | -0.45 [-1.35,+0.41] | -1.6 | +0.11 | 0.43 | 2/12 | 7/12 | 0 | 0 |
| n | insert | prepared | 1200 | 27.37 | 27.40 | 27.11 | -0.35 [-1.36,+0.41] | -1.3 | +0.30 | 0.73 | 2/12 | 8/12 | 0 | 0 |
| n | update-cold | literal | 3000 | 31.01 | 30.57 | 30.33 | -0.27 [-1.05,+1.45] | -0.9 | -0.49 | 0.92 | 4/12 | 5/12 | 0 | 0 |
| n | update-cold | prepared | 3000 | 30.22 | 29.70 | 29.64 | -0.17 [-1.45,+1.52] | -0.6 | -0.60 | 0.89 | 4/12 | 4/12 | 0 | 0 |
| n | update-hot | literal | 150 | 33.34 | 33.32 | 32.29 | -1.12 [-1.80,+1.19] | -3.3 | -0.18 | 1.61 | 1/12 | 4/12 | 0 | 0 |
| n | update-hot | prepared | 150 | 32.32 | 32.09 | 31.52 | -1.23 [-2.46,+1.93] | -3.8 | -0.34 | 1.15 | 3/12 | 3/12 | 0 | 0 |
| w | probe | literal | 3000 | 36.83 | 36.75 | 36.52 | -0.19 [-1.44,+0.84] | -0.5 | -0.27 | 0.64 | 5/12 | 3/12 | 0 | 0 |
| w | probe | prepared | 3000 | 36.23 | 35.93 | 35.91 | -0.25 [-1.04,+0.82] | -0.7 | -0.42 | 0.83 | 4/12 | 3/12 | 0 | 0 |
| w | probe-filter | literal | 3000 | 36.55 | 36.27 | 36.30 | +0.03 [-0.78,+0.73] | +0.1 | -0.22 | 0.58 | 7/12 | 4/12 | 0 | 0 |
| w | probe-filter | prepared | 3000 | 35.42 | 35.09 | 35.41 | +0.04 [-0.99,+0.98] | +0.1 | -0.24 | 0.92 | 6/12 | 5/12 | 0 | 0 |
| w | insert | literal | 1200 | 38.37 | 38.43 | 38.56 | +0.19 [-0.32,+0.62] | +0.5 | +0.06 | 0.61 | 10/12 | 6/12 | 0 | 0 |
| w | insert | prepared | 1200 | 35.18 | 35.16 | 35.16 | -0.08 [-1.01,+0.75] | -0.2 | +0.03 | 0.60 | 5/12 | 6/12 | 0 | 0 |
| w | update-cold | literal | 996 | 39.33 | 38.73 | 38.71 | -0.18 [-3.01,+2.32] | -0.5 | -0.41 | 1.97 | 4/12 | 5/12 | 0 | 0 |
| w | update-cold | prepared | 996 | 35.85 | 35.35 | 35.50 | +0.11 [-1.11,+3.67] | +0.3 | -0.49 | 2.06 | 6/12 | 4/12 | 0 | 0 |
| w | update-hot | literal | 6 | 69.39 | 68.67 | 69.10 | +1.10 [-4.48,+6.24] | +1.6 | +0.41 | 5.21 | 8/12 | 6/12 | 0 | 0 |
| w | update-hot | prepared | 6 | 60.20 | 60.36 | 61.22 | +0.64 [-3.57,+5.14] | +1.1 | +0.40 | 3.31 | 7/12 | 9/12 | 0 | 0 |

### 2.3 10000 rows

| table | shape | path | ops/arm/run | A cpu | A2 cpu | B cpu | B - mean(A,A2) med [min,max] | % | A2-A med | sd(A2-A) | B>A | A2>A | A refused stmts | B refused |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| n | probe | literal | 3000 | 36.18 | 35.63 | 35.10 | -0.70 [-2.21,+0.35] | -1.9 | -0.39 | 1.05 | 2/12 | 3/12 | 0 | 0 |
| n | probe | prepared | 3000 | 35.17 | 34.77 | 34.19 | -0.77 [-1.35,+0.51] | -2.2 | -0.60 | 0.84 | 2/12 | 2/12 | 0 | 0 |
| n | probe-filter | literal | 3000 | 35.17 | 35.14 | 34.29 | -0.88 [-2.49,+0.71] | -2.5 | +0.08 | 1.46 | 2/12 | 6/12 | 0 | 0 |
| n | probe-filter | prepared | 3000 | 33.98 | 34.38 | 33.45 | -0.47 [-2.07,+1.03] | -1.4 | +0.23 | 1.44 | 5/12 | 8/12 | 0 | 0 |
| n | insert | literal | 1200 | 27.99 | 28.14 | 27.92 | -0.16 [-1.60,+0.60] | -0.6 | -0.20 | 0.83 | 5/12 | 5/12 | 0 | 0 |
| n | insert | prepared | 1200 | 27.49 | 27.49 | 27.46 | -0.12 [-1.36,+1.44] | -0.4 | -0.31 | 0.67 | 3/12 | 5/12 | 0 | 0 |
| n | update-cold | literal | 3000 | 31.81 | 31.30 | 31.62 | -0.04 [-1.37,+1.71] | -0.1 | -0.55 | 0.76 | 6/12 | 4/12 | 0 | 0 |
| n | update-cold | prepared | 3000 | 30.55 | 29.93 | 30.11 | -0.40 [-1.34,+0.88] | -1.3 | -0.54 | 0.67 | 4/12 | 4/12 | 0 | 0 |
| n | update-hot | literal | 150 | 33.51 | 32.78 | 32.72 | -0.31 [-3.09,+0.81] | -0.9 | -0.55 | 0.93 | 5/12 | 3/12 | 0 | 0 |
| n | update-hot | prepared | 150 | 32.98 | 32.34 | 31.59 | -1.23 [-1.90,+0.61] | -3.7 | -0.54 | 1.49 | 1/12 | 4/12 | 0 | 0 |
| w | probe | literal | 3000 | 40.24 | 40.07 | 39.73 | -0.26 [-1.10,+0.54] | -0.6 | -0.48 | 0.72 | 5/12 | 4/12 | 0 | 0 |
| w | probe | prepared | 3000 | 39.42 | 38.84 | 38.73 | -0.39 [-1.12,+0.54] | -1.0 | -0.72 | 0.68 | 4/12 | 4/12 | 0 | 0 |
| w | probe-filter | literal | 3000 | 38.33 | 37.97 | 37.87 | -0.11 [-0.74,+1.42] | -0.3 | -0.39 | 0.89 | 5/12 | 3/12 | 0 | 0 |
| w | probe-filter | prepared | 3000 | 37.28 | 37.14 | 37.34 | -0.10 [-0.74,+1.43] | -0.3 | -0.19 | 0.73 | 5/12 | 4/12 | 0 | 0 |
| w | insert | literal | 1200 | 38.89 | 38.79 | 39.44 | +0.47 [-0.44,+1.34] | +1.2 | -0.18 | 0.70 | 9/12 | 5/12 | 0 | 0 |
| w | insert | prepared | 1200 | 37.77 | 37.50 | 38.08 | +0.52 [-1.52,+2.14] | +1.4 | -0.51 | 0.84 | 9/12 | 4/12 | 0 | 0 |
| w | update-cold | literal | 3000 | 38.96 | 38.32 | 38.56 | +0.06 [-1.49,+1.17] | +0.1 | -0.54 | 1.07 | 7/12 | 4/12 | 0 | 0 |
| w | update-cold | prepared | 3000 | 40.91 | 39.90 | 40.85 | +0.47 [-0.87,+5.20] | +1.2 | -1.09 | 1.34 | 9/12 | 2/12 | 0 | 0 |
| w | update-hot | literal | 6 | 66.76 | 69.09 | 67.38 | -0.78 [-10.52,+10.92] | -1.2 | -1.05 | 14.25 | 4/12 | 6/12 | 0 | 0 |
| w | update-hot | prepared | 6 | 61.63 | 62.66 | 62.88 | +0.15 [-3.35,+3.65] | +0.2 | -0.49 | 4.52 | 6/12 | 5/12 | 0 | 0 |

### 2.4 Pooled over the three sizes and both paths

Pairs where a statement was refused on any of the three arms would be dropped; none were. B - mean(A,A2) is the pooled median.

| table | shape | pairs | B - mean(A,A2) med | % of A | B>A | A2-A med | A2>A | sd(A2-A) |
|---|---|---|---|---|---|---|---|---|
| n | probe | 72 | -0.48 | -1.4 | 19/72 | -0.37 | 25/72 | 0.81 |
| n | probe-filter | 72 | -0.43 | -1.3 | 21/72 | -0.01 | 34/72 | 1.03 |
| n | insert | 72 | -0.25 | -0.9 | 21/72 | +0.04 | 37/72 | 0.68 |
| n | update-cold | 72 | -0.27 | -0.9 | 25/72 | -0.48 | 25/72 | 0.84 |
| n | update-hot | 72 | -1.02 | -3.1 | 14/72 | -0.26 | 25/72 | 1.27 |
| w | probe | 72 | -0.33 | -0.9 | 25/72 | -0.24 | 26/72 | 0.81 |
| w | probe-filter | 72 | -0.08 | -0.2 | 34/72 | -0.25 | 27/72 | 0.78 |
| w | insert | 72 | +0.26 | +0.7 | 51/72 | -0.13 | 31/72 | 0.68 |
| w | update-cold | 72 | +0.08 | +0.2 | 39/72 | -0.54 | 27/72 | 1.62 |
| w | update-hot | 72 | +0.18 | +0.3 | 37/72 | +0.05 | 37/72 | 7.15 |

## 3. Index height, and what a level costs

`SHOW INDEXES` prints `height` and `entries` walked from the tree. The driver decoded them by position (the tenth and eleventh columns of the row; the eleventh equals the rows loaded, 200 / 1,000 / 10,000, which is the check that the position is right). Heights, identical in all 12 runs and for A and A2 alike; "loaded" is read after the load and before any timed statement, "after" at the end of the run, after the inserts (the table has then grown: narrow 4,496 / 9,700 / 18,700 entries, wide 3,008 / 5,404 / 18,412 at 200 / 1,000 / 10,000 starting rows).

| table | rows loaded | A height loaded | B height loaded | A height after | B height after |
|---|---|---|---|---|---|
| `n` narrow (41 B) | 200 | 2 | 2 | 2 | 2 |
| `n` narrow | 1,000 | 2 | 2 | 2 | 2 |
| `n` narrow | 10,000 | 2 | 2 | 2 | 2 |
| `w` wide (529 B) | 200 | 2 | **3** | 2 | **4** |
| `w` wide | 1,000 | 2 | **3** | 3 | **4** |
| `w` wide | 10,000 | 3 | **4** | 3 | **4** |

The brief asked for an index with at least three levels on B: the wide index has it at every size (the 200-row table reaches 4 after its 2,400 inserts), the narrow one never does (the cut list names that). The mechanism is the one `docs/spec/index.md` §4 states: a separator is now as wide as a leaf entry, so an internal page of the wide index holds about as many children as a leaf holds entries (about 14), where on A it held several hundred; B is one level taller for it from the first divide. A narrow entry is 41 bytes, so the separator grows from 16 to 41 bytes and the fanout falls by about 2.5x, which no table of up to 18,700 entries notices.

**A level costs less than this driver can resolve on a probe.** The wide probe at 10,000 rows descends 4 levels on B and 3 on A and B - mean(A,A2) is -0.26 us (literal) and -0.39 us (prepared), -0.6 % to -1.0 %, with the A2 control at -0.5 to -0.7 below A and sd(A2-A) 0.7 us. The extra descent step is a pinned page already in the pool and a binary search over about 14 separators; it does not show against a 38 us statement. On the **insert** path the same extra level does show, small and consistent: wide insert B - mean(A,A2) is +0.29 / +0.23 us at 200 rows (heights 3 vs 2 loaded), +0.47 / +0.52 us at 10,000 rows (4 vs 3), and 51 of 72 pooled pairs are above A where 31 of 72 are for the control. An insert pays the descent and then the divides, and a wide leaf of about 14 entries divides every 7 to 14 inserts, so the wider separator copied up and the taller parent path a split must hold (AT-S16 holds every parent it writes before writing) are plausible places for the +0.3 to +0.5 us; CLA did not decompose it, so that is a hypothesis, not a finding.

| cell | B - mean(A,A2), pooled | median / A | pairs above A: B / A2 | sd(A2 - A) | verdict |
|---|---|---|---|---|---|
| `w` insert | +0.26 us | +0.7 % | 51 / 31 of 72 | 0.68 us | resolved, smaller than 1.5 % |
| `w` probe | -0.33 us | -0.9 % | 25 / 26 of 72 | 0.81 us | inside the floor |
| `w` probe-filter | -0.08 us | -0.2 % | 34 / 27 of 72 | 0.78 us | inside the floor |
| `w` update-cold | +0.08 us | +0.2 % | 39 / 27 of 72 | 1.62 us | inside the floor |
| `w` update-hot (6 statements per cell) | +0.18 us | +0.3 % | 37 / 37 of 72 | 7.15 us | no resolving power |
| `n` probe / probe-filter / insert / update-cold | -0.48 / -0.43 / -0.25 / -0.27 us | -1.4 / -1.3 / -0.9 / -0.9 % | 19-25 / 25-37 of 72 | 0.68-1.03 us | B below A, see below |
| `n` update-hot (150 statements per path) | -1.02 us | -3.1 % | 14 / 25 of 72 | 1.27 us | B below A, see below |

**The narrow cells are below the baseline, and CLA does not claim a gain.** B is under A by 0.25 to 1.0 us in every narrow cell and above it in only 14 to 25 of 72 pairs. A and B have the same tree at the same height, and the same descent (a 41-byte separator against 16 changes the fanout, not the number of levels), so there is no mechanism in the change for a gain; the A2 control sits below A too (median -0.01 to -0.48 us), which says A, the first server of every run, carries an offset of its own. CLA reads B - A as the sum of that offset and a binary layout difference between two builds, takes B - mean(A,A2) as the better estimate, and says what it supports: **no narrow cell is slower on B**. The one cell that moves the other way (hot narrow, -1.0 us, -3.1 %) is also the one with the largest control scatter (sd 1.27) and 150 statements per cell.

### Where the time goes

A statement is the client's wall, of which the server's CPU is one part. For B at 10,000 rows, prepared path, medians over 12 runs of the per-run means (`bn_census.summarize` fields); "rest" is the wall minus the larger of the two CPUs, which is the socket round trip and the hand-off between the two pinned CPUs. The `w update-cold` row is omitted: its mean wall carries the stalls named in section 4.

| statement | wall mean | server CPU | driver CPU | rest (socket and hand-off) |
|---|---|---|---|---|
| `n` probe | 73.4 us | 34.2 us | 38.6 us | 34.8 us |
| `n` probe-filter | 59.5 us | 33.4 us | 26.1 us | 26.1 us |
| `n` insert | 50.4 us | 27.5 us | 23.6 us | 22.9 us |
| `n` update-cold | 51.6 us | 30.1 us | 21.7 us | 21.5 us |
| `w` probe | 81.8 us | 38.7 us | 43.1 us | 38.7 us |
| `w` probe-filter | 63.6 us | 37.3 us | 26.7 us | 26.3 us |
| `w` insert | 63.4 us | 38.1 us | 26.0 us | 25.3 us |

The change lives inside the server-CPU column and is at most +0.5 us of a 38 us figure. Durability/commit wait, lock or conflict wait and the second-session wait **do not apply** to this measurement: one session on `relaxed` durability waits on none of them, and the cut list names the rungs where they would. No finer split of the server's 27-41 us (parse, lock unit, descent, undo, log append) was measured; no sampler was run.

## 4. Latency distributions (wall per statement at the client)

Median over 12 runs of each run's percentile; `ops` is the statement count summed over runs; `stmts/s` is derived (1e6 over the median-over-runs mean wall). A2 rows are omitted here (they are A's bytes; A2 is in the delta tables). The `w update-cold prepared` rows at 10,000 rows carry a stall in several runs on every arm (mean wall 140-200 us in 8 of 12 runs of B against a p50 of 53 us), which depresses their derived stmts/s for A and B alike; read those two rows by p50.

### 4.1 200 rows

| table | shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | stmts/s | server cpu |
|---|---|---|---|---|---|---|---|---|---|---|---|
| n | probe | literal | A | 36000 | 69.1 | 71.6 | 72.6 | 83.4 | 97.7 | 13157 | 34.23 |
| n | probe | literal | B | 36000 | 68.5 | 71.2 | 72.2 | 82.2 | 97.2 | 13288 | 33.84 |
| n | probe | prepared | A | 36000 | 65.9 | 68.1 | 68.9 | 78.5 | 92.5 | 13929 | 33.42 |
| n | probe | prepared | B | 36000 | 65.2 | 67.6 | 68.5 | 77.8 | 92.7 | 14018 | 33.03 |
| n | probe-filter | literal | A | 36000 | 57.2 | 59.4 | 60.2 | 71.2 | 85.2 | 15736 | 34.41 |
| n | probe-filter | literal | B | 36000 | 56.3 | 58.7 | 59.5 | 70.6 | 85.5 | 15887 | 33.65 |
| n | probe-filter | prepared | A | 36000 | 51.5 | 55.6 | 56.3 | 67.2 | 82.2 | 16805 | 33.73 |
| n | probe-filter | prepared | B | 36000 | 52.6 | 55.2 | 55.9 | 66.0 | 80.8 | 16871 | 33.05 |
| n | insert | literal | A | 14400 | 42.2 | 47.8 | 48.7 | 61.1 | 89.8 | 18911 | 28.02 |
| n | insert | literal | B | 14400 | 44.0 | 47.5 | 48.1 | 60.5 | 87.5 | 18957 | 27.73 |
| n | insert | prepared | A | 14400 | 42.5 | 45.5 | 46.2 | 58.0 | 82.2 | 19829 | 26.94 |
| n | insert | prepared | B | 14400 | 42.2 | 45.2 | 45.9 | 58.2 | 82.3 | 20021 | 26.75 |
| n | update-cold | literal | A | 9576 | 45.1 | 50.6 | 51.6 | 66.1 | 97.8 | 17719 | 31.48 |
| n | update-cold | literal | B | 9576 | 46.4 | 49.9 | 50.8 | 65.6 | 97.1 | 18032 | 30.76 |
| n | update-cold | prepared | A | 9576 | 41.4 | 47.5 | 48.2 | 60.1 | 91.5 | 19226 | 29.98 |
| n | update-cold | prepared | B | 9576 | 41.1 | 46.9 | 47.5 | 59.3 | 91.6 | 19541 | 29.30 |
| n | update-hot | literal | A | 1800 | 49.4 | 50.8 | 52.1 | 67.8 | 107.0 | 17394 | 32.93 |
| n | update-hot | literal | B | 1800 | 48.6 | 49.8 | 50.7 | 67.8 | 105.8 | 17704 | 31.62 |
| n | update-hot | prepared | A | 1800 | 45.9 | 47.8 | 48.7 | 65.3 | 98.9 | 18461 | 31.73 |
| n | update-hot | prepared | B | 1800 | 45.7 | 46.9 | 48.1 | 66.0 | 96.2 | 18715 | 31.36 |
| w | probe | literal | A | 36000 | 74.9 | 77.6 | 79.0 | 91.6 | 106.4 | 12110 | 36.17 |
| w | probe | literal | B | 36000 | 74.6 | 77.5 | 79.0 | 90.3 | 106.3 | 12153 | 35.84 |
| w | probe | prepared | A | 36000 | 71.8 | 74.1 | 75.6 | 87.1 | 102.5 | 12649 | 35.57 |
| w | probe | prepared | B | 36000 | 71.6 | 74.0 | 75.6 | 87.9 | 101.7 | 12653 | 35.01 |
| w | probe-filter | literal | A | 36000 | 59.1 | 60.9 | 61.7 | 73.4 | 87.6 | 15173 | 36.15 |
| w | probe-filter | literal | B | 36000 | 58.8 | 60.6 | 61.4 | 73.2 | 87.1 | 15240 | 35.50 |
| w | probe-filter | prepared | A | 36000 | 54.9 | 57.0 | 57.9 | 69.7 | 82.5 | 16193 | 35.01 |
| w | probe-filter | prepared | B | 36000 | 54.8 | 56.9 | 57.7 | 69.8 | 84.3 | 16186 | 34.66 |
| w | insert | literal | A | 14400 | 53.1 | 55.2 | 57.7 | 75.4 | 118.2 | 15284 | 38.48 |
| w | insert | literal | B | 14400 | 53.2 | 55.0 | 57.8 | 77.0 | 114.3 | 15224 | 38.41 |
| w | insert | prepared | A | 14400 | 49.7 | 51.8 | 54.4 | 72.7 | 116.4 | 15985 | 36.97 |
| w | insert | prepared | B | 14400 | 49.2 | 51.7 | 54.7 | 74.8 | 117.9 | 15955 | 37.09 |
| w | update-cold | literal | A | 2376 | 52.4 | 54.3 | 55.7 | 73.6 | 120.3 | 16050 | 36.95 |
| w | update-cold | literal | B | 2376 | 52.2 | 54.1 | 55.7 | 74.5 | 116.7 | 16181 | 36.50 |
| w | update-cold | prepared | A | 2376 | 49.2 | 51.0 | 52.5 | 70.6 | 112.7 | 17102 | 35.84 |
| w | update-cold | prepared | B | 2376 | 48.7 | 50.9 | 52.4 | 71.0 | 114.5 | 17036 | 35.95 |
| w | update-hot | literal | A | 72 | 83.6 | 89.6 | 93.1 | 126.9 | 126.9 | 9591 | 71.38 |
| w | update-hot | literal | B | 72 | 83.9 | 87.1 | 89.5 | 119.2 | 119.2 | 9897 | 69.41 |
| w | update-hot | prepared | A | 72 | 80.9 | 82.9 | 85.1 | 107.5 | 107.5 | 10561 | 60.72 |
| w | update-hot | prepared | B | 72 | 79.4 | 82.5 | 87.8 | 114.7 | 114.7 | 10580 | 61.33 |

### 4.2 1000 rows

| table | shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | stmts/s | server cpu |
|---|---|---|---|---|---|---|---|---|---|---|---|
| n | probe | literal | A | 36000 | 69.6 | 72.0 | 73.0 | 82.7 | 98.7 | 13180 | 34.71 |
| n | probe | literal | B | 36000 | 69.0 | 71.5 | 72.5 | 82.1 | 97.8 | 13235 | 34.44 |
| n | probe | prepared | A | 36000 | 65.6 | 68.2 | 69.2 | 79.1 | 93.1 | 13867 | 33.84 |
| n | probe | prepared | B | 36000 | 65.4 | 67.9 | 68.7 | 78.3 | 92.9 | 13959 | 33.52 |
| n | probe-filter | literal | A | 36000 | 57.0 | 59.2 | 60.0 | 69.2 | 83.0 | 15845 | 34.30 |
| n | probe-filter | literal | B | 36000 | 56.9 | 59.0 | 59.8 | 69.6 | 83.7 | 15850 | 34.13 |
| n | probe-filter | prepared | A | 36000 | 53.3 | 55.5 | 56.3 | 65.7 | 80.4 | 16786 | 33.79 |
| n | probe-filter | prepared | B | 36000 | 53.1 | 55.4 | 56.1 | 65.5 | 79.7 | 16827 | 33.26 |
| n | insert | literal | A | 14400 | 44.6 | 47.9 | 48.5 | 59.3 | 85.4 | 19077 | 27.84 |
| n | insert | literal | B | 14400 | 43.1 | 47.5 | 48.2 | 59.3 | 82.9 | 19189 | 27.48 |
| n | insert | prepared | A | 14400 | 43.3 | 45.5 | 46.2 | 58.0 | 85.4 | 19788 | 27.37 |
| n | insert | prepared | B | 14400 | 41.3 | 45.2 | 45.9 | 57.7 | 84.8 | 19969 | 27.11 |
| n | update-cold | literal | A | 36000 | 43.0 | 50.5 | 51.2 | 62.7 | 81.6 | 18155 | 31.01 |
| n | update-cold | literal | B | 36000 | 43.2 | 49.9 | 50.7 | 62.1 | 81.6 | 18369 | 30.33 |
| n | update-cold | prepared | A | 36000 | 41.4 | 47.4 | 48.3 | 59.2 | 78.2 | 19173 | 30.22 |
| n | update-cold | prepared | B | 36000 | 40.8 | 46.9 | 47.6 | 58.9 | 77.9 | 19463 | 29.64 |
| n | update-hot | literal | A | 1800 | 49.4 | 51.0 | 52.1 | 68.8 | 111.6 | 17458 | 33.34 |
| n | update-hot | literal | B | 1800 | 48.6 | 49.8 | 50.9 | 67.8 | 110.3 | 17494 | 32.29 |
| n | update-hot | prepared | A | 1800 | 46.3 | 48.0 | 49.2 | 64.0 | 100.3 | 18430 | 32.32 |
| n | update-hot | prepared | B | 1800 | 45.4 | 46.7 | 47.7 | 63.3 | 106.4 | 18806 | 31.52 |
| w | probe | literal | A | 36000 | 75.6 | 78.0 | 79.5 | 90.5 | 106.8 | 12124 | 36.83 |
| w | probe | literal | B | 36000 | 75.6 | 77.8 | 79.3 | 89.7 | 104.9 | 12141 | 36.52 |
| w | probe | prepared | A | 36000 | 72.3 | 74.7 | 76.0 | 86.8 | 101.8 | 12661 | 36.23 |
| w | probe | prepared | B | 36000 | 71.7 | 74.4 | 75.9 | 87.0 | 102.3 | 12668 | 35.91 |
| w | probe-filter | literal | A | 36000 | 59.2 | 61.3 | 62.1 | 73.0 | 85.7 | 15175 | 36.55 |
| w | probe-filter | literal | B | 36000 | 59.0 | 61.0 | 61.9 | 73.2 | 86.6 | 15209 | 36.30 |
| w | probe-filter | prepared | A | 36000 | 55.2 | 57.4 | 58.3 | 69.4 | 82.2 | 16105 | 35.42 |
| w | probe-filter | prepared | B | 36000 | 54.6 | 57.3 | 58.2 | 68.8 | 82.4 | 16147 | 35.41 |
| w | insert | literal | A | 14400 | 53.3 | 55.4 | 57.6 | 75.7 | 113.8 | 15325 | 38.37 |
| w | insert | literal | B | 14400 | 52.5 | 55.4 | 57.8 | 76.4 | 117.5 | 15274 | 38.56 |
| w | insert | prepared | A | 14400 | 49.5 | 51.5 | 53.2 | 69.9 | 108.8 | 16485 | 35.18 |
| w | insert | prepared | B | 14400 | 49.5 | 51.6 | 53.3 | 71.0 | 105.4 | 16513 | 35.16 |
| w | update-cold | literal | A | 11952 | 51.2 | 54.9 | 57.2 | 74.8 | 130.0 | 15547 | 39.33 |
| w | update-cold | literal | B | 11952 | 52.4 | 54.6 | 57.0 | 75.7 | 127.2 | 15219 | 38.71 |
| w | update-cold | prepared | A | 11952 | 47.7 | 51.5 | 52.6 | 68.0 | 103.3 | 17336 | 35.85 |
| w | update-cold | prepared | B | 11952 | 47.0 | 51.2 | 52.4 | 68.7 | 109.1 | 16989 | 35.50 |
| w | update-hot | literal | A | 72 | 82.8 | 85.0 | 91.3 | 141.7 | 141.7 | 9815 | 69.39 |
| w | update-hot | literal | B | 72 | 82.8 | 86.5 | 90.8 | 141.2 | 141.2 | 9945 | 69.10 |
| w | update-hot | prepared | A | 72 | 79.1 | 81.7 | 87.3 | 108.7 | 108.7 | 10690 | 60.20 |
| w | update-hot | prepared | B | 72 | 78.9 | 81.7 | 85.4 | 111.2 | 111.2 | 10533 | 61.22 |

### 4.3 10000 rows

| table | shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | stmts/s | server cpu |
|---|---|---|---|---|---|---|---|---|---|---|---|
| n | probe | literal | A | 36000 | 69.9 | 72.6 | 74.0 | 85.9 | 99.6 | 12949 | 36.18 |
| n | probe | literal | B | 36000 | 69.4 | 72.0 | 73.2 | 84.7 | 98.8 | 13082 | 35.10 |
| n | probe | prepared | A | 36000 | 66.4 | 69.1 | 70.4 | 82.0 | 96.5 | 13575 | 35.17 |
| n | probe | prepared | B | 36000 | 65.6 | 68.3 | 69.4 | 80.1 | 94.7 | 13629 | 34.19 |
| n | probe-filter | literal | A | 36000 | 57.1 | 59.7 | 60.7 | 70.8 | 84.3 | 15677 | 35.17 |
| n | probe-filter | literal | B | 36000 | 57.2 | 59.1 | 60.1 | 69.5 | 84.6 | 15809 | 34.29 |
| n | probe-filter | prepared | A | 36000 | 53.5 | 55.9 | 56.8 | 65.2 | 80.6 | 16739 | 33.98 |
| n | probe-filter | prepared | B | 36000 | 53.2 | 55.5 | 56.4 | 66.1 | 79.8 | 16793 | 33.45 |
| n | insert | literal | A | 14400 | 44.2 | 47.9 | 48.6 | 59.8 | 87.1 | 19066 | 27.99 |
| n | insert | literal | B | 14400 | 42.0 | 47.9 | 48.7 | 59.1 | 84.9 | 19090 | 27.92 |
| n | insert | prepared | A | 14400 | 43.4 | 45.8 | 46.4 | 58.5 | 82.5 | 19860 | 27.49 |
| n | insert | prepared | B | 14400 | 42.1 | 45.7 | 46.4 | 57.7 | 83.9 | 19850 | 27.46 |
| n | update-cold | literal | A | 36000 | 43.7 | 51.0 | 51.9 | 64.7 | 86.7 | 17367 | 31.81 |
| n | update-cold | literal | B | 36000 | 43.6 | 50.4 | 51.2 | 63.8 | 86.0 | 17360 | 31.62 |
| n | update-cold | prepared | A | 36000 | 40.6 | 47.9 | 48.6 | 58.6 | 80.6 | 18436 | 30.55 |
| n | update-cold | prepared | B | 36000 | 40.8 | 47.2 | 48.0 | 58.8 | 80.9 | 19374 | 30.11 |
| n | update-hot | literal | A | 1800 | 49.3 | 51.2 | 52.2 | 67.3 | 105.3 | 17439 | 33.51 |
| n | update-hot | literal | B | 1800 | 48.8 | 49.9 | 50.9 | 68.9 | 105.8 | 17553 | 32.72 |
| n | update-hot | prepared | A | 1800 | 46.7 | 48.1 | 49.2 | 65.7 | 98.0 | 18248 | 32.98 |
| n | update-hot | prepared | B | 1800 | 45.2 | 46.6 | 47.6 | 64.3 | 98.6 | 18642 | 31.59 |
| w | probe | literal | A | 36000 | 77.8 | 81.1 | 82.9 | 94.6 | 108.8 | 11619 | 40.24 |
| w | probe | literal | B | 36000 | 77.2 | 80.7 | 82.5 | 94.0 | 107.6 | 11668 | 39.73 |
| w | probe | prepared | A | 36000 | 74.4 | 77.4 | 79.1 | 90.7 | 104.2 | 12159 | 39.42 |
| w | probe | prepared | B | 36000 | 73.7 | 77.0 | 78.8 | 88.2 | 103.3 | 12225 | 38.73 |
| w | probe-filter | literal | A | 36000 | 60.3 | 62.9 | 63.8 | 73.9 | 89.4 | 14799 | 38.33 |
| w | probe-filter | literal | B | 36000 | 59.9 | 62.5 | 63.5 | 75.4 | 90.3 | 14811 | 37.87 |
| w | probe-filter | prepared | A | 36000 | 56.2 | 59.0 | 59.9 | 70.3 | 83.7 | 15734 | 37.28 |
| w | probe-filter | prepared | B | 36000 | 56.5 | 58.7 | 59.7 | 70.8 | 84.6 | 15713 | 37.34 |
| w | insert | literal | A | 14400 | 53.4 | 55.7 | 57.9 | 76.0 | 121.8 | 14983 | 38.89 |
| w | insert | literal | B | 14400 | 53.2 | 56.2 | 58.2 | 78.2 | 125.3 | 14672 | 39.44 |
| w | insert | prepared | A | 14400 | 49.3 | 52.3 | 54.7 | 73.7 | 121.1 | 15929 | 37.77 |
| w | insert | prepared | B | 14400 | 49.6 | 52.3 | 54.8 | 76.6 | 123.1 | 15785 | 38.08 |
| w | update-cold | literal | A | 36000 | 50.2 | 55.4 | 57.2 | 73.1 | 113.2 | 15791 | 38.96 |
| w | update-cold | literal | B | 36000 | 50.8 | 55.2 | 56.7 | 73.9 | 111.7 | 15668 | 38.56 |
| w | update-cold | prepared | A | 36000 | 47.5 | 52.7 | 54.2 | 71.2 | 157.1 | 7371 | 40.91 |
| w | update-cold | prepared | B | 36000 | 47.9 | 52.1 | 53.6 | 70.5 | 149.9 | 6324 | 40.85 |
| w | update-hot | literal | A | 72 | 84.5 | 87.7 | 90.7 | 173.7 | 173.7 | 9356 | 66.76 |
| w | update-hot | literal | B | 72 | 84.7 | 86.4 | 89.3 | 172.9 | 172.9 | 9464 | 67.38 |
| w | update-hot | prepared | A | 72 | 81.5 | 84.6 | 87.1 | 110.8 | 110.8 | 10548 | 61.63 |
| w | update-hot | prepared | B | 72 | 82.4 | 84.0 | 88.5 | 109.9 | 109.9 | 10412 | 62.88 |

## 5. The hot row: where A stops

Separate probe, one run per arm, fresh server, 1,000 rows loaded, one row (id 1) updated by `UPDATE x SET c1 = <distinct>` up to 3,000 times until the first refusal; server pinned as above; no timing taken.

| table | A accepted | A first refusal | B accepted |
|---|---|---|---|
| `n` narrow (3 covered `int64`) | **169** | `maintaining index 1: this separator is already present in the index node; two subtrees cannot share a low key` | 3,000 of 3,000, no refusal |
| `w` wide (4 covered `varchar(128)`) | **20** | same message | 3,000 of 3,000, no refusal |

A refuses for good after those counts, and a later `INSERT` into the same tree can be refused too (the pilots, section 1). B's sort key is distinct per entry, so a run of equal keys cannot form and the cap on the probe (3,000) was reached on both layouts; B's own limit, if there is one, was not found. The matrix therefore compares A and B only below A's threshold, which is why it holds 0 refusals on both sides.

## 6. What it says about the engine

1. **The cost of the whole-entry separator is paid in tree height, and only a wide covering clause pays it.** A 529-byte entry makes an internal page as small as a leaf, so a 10,000-row wide-covered index is 4 levels on B and 3 on A, and a 200-row one is 3 against 2. A 41-byte entry stays at 2 levels up to 18,700 entries on both. The wide covering clause is where `docs/spec/index.md` §4 says the cost lands ("a wide `COVERING` clause gives internal nodes as few children as its leaves hold entries"); this run is its first data point and it supports the statement.
2. **A level is cheap here: below 1 % of a statement.** The probe cell through the deeper tree is not slower than the control scatter (0.7 to 0.8 us). The only resolved cost is on the `INSERT`: +0.26 us pooled, up to +0.5 us at 10,000 rows (+1.2 % to +1.4 %), the divides' path. Relaxed durability on one core is where that share is largest; the client's socket round trip (about 25-39 us) and the server's fixed per-statement work hide the rest.
3. **The measured price buys a bounded failure out of the engine.** On A one hot row's covered-column updates are refused after 169 (narrow) and 20 (wide) of them at 1,000 rows, so the threshold falls as the entry widens (about 8x fewer for a 13x wider entry; two points, no fit). On B the same row took 3,000 updates on both layouts. Against that, +0.3 us on a wide insert is cheap.
4. **Not measured, so not claimed:** the page cost (more internal pages for the same entries; the run did not read a page count), a narrow tree past 40,000 rows, `group`/`strict` durability, and whether B's own hot-row limit exists beyond 3,000 updates.
5. **No stated expectation was contradicted.** The change's own expectation (index.md §4) is that fanout drops with covering width and trees can be deeper; both are observed. The manual's "about 600" for the refusal is the one number this run does not reproduce (169 and 20); the manual's figure may describe a different layout, and reconciling it is for the owner of `manual/sql/sql.md`.

## Reproduce

    # A and B built from detached checkouts: cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release \
    #   -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF ; cmake --build build-rel --target kds_server -j4
    cp srcA/build-rel/kds_server $RUN/bin/kds_A ; cp srcA/build-rel/kds_server $RUN/bin/kds_A2 ; cp srcB/build-rel/kds_server $RUN/bin/kds_B
    python3 ix_ab.py --tools tools --arm A=$RUN/bin/kds_A --arm A2=$RUN/bin/kds_A2 --arm B=$RUN/bin/kds_B \
        --workdir $RUN/wd --runs 12 --rows {200|1000|10000} --port 31970 --out g<rows>.json --progress p.txt
    python3 hot_depth.py --tools tools --arm A=$RUN/bin/kds_A --arm B=$RUN/bin/kds_B --workdir $RUN/wd --out hot.json

The raw per-run JSON (`g200.json`, `g1000.json`, `g10000.json`, `hot.json`), the progress file, both scratch drivers and `analyze.py` are under `/home/cdkbs/bench-runs/ix4b/`; a narrower measurement archives nothing in the tree (rule 1b).
