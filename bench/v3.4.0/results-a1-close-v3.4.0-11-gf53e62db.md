# A1 close: the yield costs nothing on the statement path, bounds a second session's point SELECT to one slice (9.6 ms behind CREATE INDEX, 5.4 ms behind CREATE ASSERTION, against the whole build, 1.05 s and 1.76 s), but at 1,000,000 rows CREATE ASSERTION still holds the core in two runs of 0.4-0.54 s

**Thesis.** On `worktree-ddl-build-yield` at `f53e62db` (`v3.4.0-11-gf53e62db`, **B**) against the commit A1 opened from, `4f175ba9` (`v3.3.0-74-g4f175ba9`, **A**), the four hot statements (tuple-key point `SELECT`, single-row `UPDATE`, single-row `INSERT`, unsorted scan) move by no more than this host resolves: best-rep p50 differs by -2.3 % to +0.9 % in all twelve cells, and the rep-to-rep p50 spread of A alone is 3 to 46 %. The benefit is large and clean for `CREATE INDEX`: on `cores = 1`, a second session's point `SELECT` waits **1,049-1,055 ms (the whole build) on A** and **p50 9.6 ms, p99 14.4-15.0 ms on B** (one slice of 64 leaves is ~9.5 ms of index work). The yield costs the build **+3.3 % wall (1.087 s mean against 1.052 s; B's three runs span 1.050-1.121 s, so not resolved at n = 3)**. For `CREATE ASSERTION` the p50 is bounded the same way (5.4 ms against 1,720-1,783 ms) and B's build is faster (1.536 s against 1.761 s, -13 %, cause not attributed), **but B's p99 and max are not bounded by a slice: every B run carries two stalls of 0.40-0.54 s at about 0.23-0.25 s and 0.93-1.07 s into the build.** At 500,000 rows no probe exceeds 20 ms. That is the finding to take away from this run.

Which commits carry what: B is A plus 43 commits (`git log 4f175ba9..HEAD`), of which 12 touch `src/`. A1's own are `bf831a94` (a relation-X build reports progress), `2e5ff6f8` (waker-table entry), `37a60b10` (stage 2a, index builds yield), `cca7415b` (stage 2b, assertion and transition builds yield) and `7f0fa91d` (a rename takes the relation X). The rest of the delta is **other sessions' work merged from main**: BS (sanitizer fixes `8ef3cd98`, `3099051b` frame-table race, fault sweeps `5d58f3ca`, `24a2e6d1`), `c8a1e45c`, `1fd28db0` (an `UPDATE`/`DELETE` WHERE declines a NotImplemented expression), the docs sync `005c4a04`, and CF Waystone branch O and superblock 33 (docs and spec). **The A/B delta is therefore "A1 plus the merged main", not A1 alone**: no cell below can separate the two, and the statement-path rows (all inside the noise) are the evidence that the sum is harmless.

## Run stamp

| Item | Value |
|---|---|
| Executed | 2026-10-21, UTC. Builds 14:03:44-14:05:44; final measurement: statement cells 14:14:39-14:16:02, build cells 14:16:02-14:22:05, diagnostic extras 14:23-14:25 (`archive/.../progress.txt` has every stamp). An earlier complete pass (5 reps x 1,000 ops, 3 build reps) agreed and was discarded when the driver gained the slow-probe timeline |
| Worktree / branch | `/home/cdkbs/ckdbs/.claude/worktrees/ddl-build-yield`, branch `worktree-ddl-build-yield`, HEAD `f53e62db` (committed 2026-10-21 13:48:44 UTC). Tree clean at the start; at the end the only change is untracked: `tools/a1_close_benchmark.py` (the driver, written for this run) and `bench/v3.4.0/`. No engine source touched |
| A | `4f175ba99fd49a80258b61d1492dec8d9a6049ee`, `git describe` = `v3.3.0-74-g4f175ba9`, committed 2026-10-21 04:31:22 UTC. Built in a detached worktree (`/tmp/claude-1000/.../scratchpad/a1-base`, removed after the run) |
| B | `f53e62dbacb3bffd06884337ba515256dadee4c1`, `v3.4.0-11-gf53e62db` |
| Binary provenance | Both built this session, in their own `build-release/`, after their commits (A binary 14:04:44, B binary 14:05:44 UTC; both newer than the commit). **Measured copies** `/home/cdkbs/a1-run/kds_A` sha256 `ecb0d89caecdf1b057cd0934a5ea38264e227805d3da4bf2493530ae18b2b3c1`, `kds_B` sha256 `5f659f70c3f7fc2eca9cf56f7361610730639639474cd5124e3b6283cf0aed8d` (also in `archive/a1-close-v3.4.0-11-gf53e62db/binaries-and-drivers.txt`); every server started from a copy |
| Build | `CMAKE_BUILD_TYPE=Release` (read from both `CMakeCache.txt`), target `kds_server`, `-j7`, gcc 13.3.0, finished before any measurement started (nothing was built during a cell) |
| Host / device | 8 vCPU AMD EPYC 9V74, Linux 7.0.0-1014-azure. Data files and WAL under `/home/cdkbs/a1-run/<cell>/` on `/dev/root`, **ext4, not tmpfs** (`df -T`; 94 % full, 17.7 GB free; each cell deletes its data file). Not pinned to CPUs |
| Host record | `archive/.../env.log` has `/proc/loadavg` and `pgrep -a -f "cc1plus\|cmake --build\|ctest"` before and after each of the 26 cells: 1-minute load 0.87-1.89, **no `cc1plus`, `cmake --build` or `ctest` in any record**. Two unrelated `kds_server` processes (autotrade on port 15432, a web sandbox) were present and idle. The 5-minute load was 3-4 at the start of the statement cells and fell to ~1.7 by the end, so the statement cells ran on a host draining earlier load |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, `log_level = warn`, default `checkpoint_interval_ms` (5000), one server and one fresh data file per cell. Everything below is **relaxed**: no fsync sits in a measured statement |
| Driver | `tools/a1_close_benchmark.py` (new, untracked; its docstring is the documentation, `bench/docs/` is closed per `bench/README.md`), over KWP/1 via `tools/ckdbs_cli.ServerConnection`. Hashes in `archive/.../binaries-and-drivers.txt` |
| Suite | Not run in this task; no code was changed |

## 1. What was run, what was cut

| Part | Cells | Work per cell |
|---|---|---|
| Statement path, A vs B | 10 per build, interleaved A B B A A B B A A B B A A B B A A B B A, fresh server each | at each of 200, 1,000, 10,000 preloaded rows (a fresh BTREE relation, 5 columns, one varchar of 16): 2,000 single-row `INSERT`s, 2,000 tuple-key `SELECT`s, 2,000 single-row `UPDATE`s (by tuple key, fixed-width column), 20 unsorted `SELECT * FROM t` |
| Build benefit, A vs B | 3 per build, interleaved A B B A A B, `cores = 1` | 1,000,000 rows of `big (grp, amount, note varchar(200))` (BTREE, ~215 B per row) + `small` (1,000 rows); a second connection loops tuple-key `SELECT * FROM small WHERE id = ?` for the whole cell; 1 s idle, then `CREATE INDEX ix_big_grp ON big (grp)`, 1 s idle, then `CREATE ASSERTION big_cap ON big GROUP BY (grp) CHECK COUNT(*) <= 1000000`, 1 s idle |
| Diagnostics | B with `checkpoint_interval_ms = 0` (1 run); A and B at 500,000 rows (1 run each) | as above |

Row-count mapping: the "rows" column is the preload; the INSERT phase adds 2,000, so the relation the scan reads holds 2,200 / 3,000 / 12,000 rows (the "few thousand" scan is the 1,000 row). Point reads draw ids uniformly in 1..preload.

**Cut:** nothing in the work order's cut list. Cut by choice: no PostgreSQL floor (no twin exists for relation-X builds, and the statement rows are identical code paths on both sides); no `strict`/`group` class (the brief is the statement path under relaxed); no multi-core cell (the milestone's claim is per core); the build cell was run at one size (1M rows; the 500K diagnostics are single runs); no p99.9. The "wait breakdown" is in section 4.

## 2. Statement path: nothing resolved, A vs B

Each cell is 10 servers per build; a table entry is the **median over the 10 reps of that percentile**. n per rep is 2,000 (20 for the scan). Microseconds. QPS is derived as 1e6 / median-of-rep-means (rule 5a), not reported by the driver.

| rows | statement | build | p0 | p25 | p50 | p90 | p99 | max | QPS (derived) | p50, best-worst rep |
|---|---|---|---|---|---|---|---|---|---|---|
| 200 | insert | A | 39.0 | 45.1 | 45.5 | 52.4 | 70.6 | 100.9 | 21,164 | 44.6-65.1 |
| 200 | insert | B | 43.3 | 44.8 | 45.3 | 53.6 | 71.1 | 115.2 | 21,030 | 44.8-56.9 |
| 200 | point-select | A | 62.5 | 64.3 | 65.6 | 77.9 | 96.4 | 153.8 | 14,493 | 64.9-78.8 |
| 200 | point-select | B | 62.0 | 63.7 | 64.8 | 76.3 | 94.5 | 161.4 | 14,695 | 63.4-92.9 |
| 200 | update | A | 39.3 | 47.4 | 47.9 | 53.8 | 72.7 | 2,097.9 | 19,724 | 46.6-67.5 |
| 200 | update | B | 43.1 | 47.0 | 47.6 | 55.9 | 72.7 | 2,047.7 | 19,900 | 46.7-67.7 |
| 200 | scan (2,200 rows) | A | 4,738.8 | 4,761.0 | 4,778.6 | 5,071.4 | 5,732.2 | 5,732.2 | 205 | 4,734.7-5,008.9 |
| 200 | scan (2,200 rows) | B | 4,734.6 | 4,770.1 | 4,792.0 | 5,050.9 | 5,677.8 | 5,677.8 | 205 | 4,741.6-4,918.4 |
| 1,000 | insert | A | 39.7 | 44.8 | 45.2 | 53.9 | 69.8 | 102.8 | 21,142 | 44.8-64.2 |
| 1,000 | insert | B | 41.5 | 44.8 | 45.4 | 56.2 | 72.5 | 115.4 | 21,008 | 44.3-65.0 |
| 1,000 | point-select | A | 62.5 | 65.2 | 67.5 | 80.8 | 102.3 | 146.2 | 14,065 | 66.6-97.0 |
| 1,000 | point-select | B | 62.6 | 65.2 | 68.1 | 92.6 | 121.7 | 165.4 | 13,633 | 65.7-96.9 |
| 1,000 | update | A | 39.2 | 47.5 | 48.1 | 55.8 | 72.5 | 2,012.0 | 19,646 | 46.8-67.2 |
| 1,000 | update | B | 40.1 | 47.0 | 47.5 | 59.8 | 77.0 | 2,024.9 | 19,342 | 46.6-67.5 |
| 1,000 | scan (3,000 rows) | A | 6,473.3 | 6,539.0 | 6,569.5 | 6,819.5 | 8,108.1 | 8,108.1 | 149 | 6,492.2-6,698.8 |
| 1,000 | scan (3,000 rows) | B | 6,458.7 | 6,516.8 | 6,555.1 | 6,700.8 | 8,118.9 | 8,118.9 | 150 | 6,435.3-6,767.2 |
| 10,000 | insert | A | 40.2 | 44.7 | 45.2 | 52.6 | 70.0 | 111.8 | 21,119 | 44.6-47.1 |
| 10,000 | insert | B | 43.5 | 45.2 | 63.4 | 68.6 | 88.4 | 117.3 | 17,227 | 44.9-65.2 |
| 10,000 | point-select | A | 62.3 | 65.8 | 79.0 | 100.0 | 120.1 | 168.2 | 12,461 | 64.2-94.6 |
| 10,000 | point-select | B | 62.3 | 64.3 | 66.2 | 94.9 | 115.3 | 167.2 | 13,605 | 64.8-95.0 |
| 10,000 | update | A | 40.5 | 47.6 | 48.6 | 62.3 | 80.8 | 2,196.4 | 18,850 | 46.8-67.0 |
| 10,000 | update | B | 43.3 | 47.2 | 48.0 | 68.3 | 81.7 | 2,172.2 | 19,084 | 47.2-66.7 |
| 10,000 | scan (12,000 rows) | A | 24,501.6 | 24,756.0 | 25,222.6 | 26,676.0 | 27,923.4 | 27,923.4 | 39 | 24,656.2-26,108.1 |
| 10,000 | scan (12,000 rows) | B | 24,229.2 | 24,446.7 | 24,958.5 | 26,632.9 | 27,680.2 | 27,680.2 | 40 | 24,386.1-26,097.1 |

**Reading it.** The noise floor comes from inside the run: the same binary's own 10 reps span, in p50, 44.6-65.1 (insert, 200 rows) and 64.2-94.6 (point-select, 10,000 rows) in A alone. That spread is bimodal: a server process either runs a statement stream at ~45 (insert) / ~65 (select) or at a mode ~20 us higher for the whole of its life, in both A and B, in different cells. It makes two cells look like a 16-40 % difference (10,000-row insert: B median p50 63.4 against A 45.2; 10,000-row point-select: A 79.0 against B 66.2), which is a count of how many of the ten servers drew the slow mode (6 of 10 B insert reps, 5 of 10 A select reps), not the build. The honest comparison is therefore the best rep of each side, and the median where the spread is small:

| rows | statement | A median p50 | B median p50 | A best-rep p50 | B best-rep p50 | best-rep delta |
|---|---|---|---|---|---|---|
| 200 | insert | 45.5 | 45.3 | 44.6 | 44.8 | +0.4 % |
| 200 | point-select | 65.6 | 64.8 | 64.9 | 63.4 | -2.3 % |
| 200 | update | 47.9 | 47.6 | 46.6 | 46.7 | +0.2 % |
| 200 | scan | 4,778.6 | 4,792.0 | 4,734.7 | 4,741.6 | +0.1 % |
| 1,000 | insert | 45.2 | 45.4 | 44.8 | 44.3 | -1.1 % |
| 1,000 | point-select | 67.5 | 68.1 | 66.6 | 65.7 | -1.4 % |
| 1,000 | update | 48.1 | 47.5 | 46.8 | 46.6 | -0.4 % |
| 1,000 | scan | 6,569.5 | 6,555.1 | 6,492.2 | 6,435.3 | -0.9 % |
| 10,000 | insert | 45.2 | 63.4 (slow mode) | 44.6 | 44.9 | +0.7 % |
| 10,000 | point-select | 79.0 (slow mode) | 66.2 | 64.2 | 64.8 | +0.9 % |
| 10,000 | update | 48.6 | 48.0 | 46.8 | 47.2 | +0.9 % |
| 10,000 | scan | 25,222.6 | 24,958.5 | 24,656.2 | 24,386.1 | -1.1 % |

Every best-rep delta is under 2.5 % in magnitude and has no consistent sign; nothing here is a finding. Two shape facts the tables also give: the point statements do not depend on the relation's size (p50 within 3 us across 200 / 1K / 10K, the expected B+ tree depth effect and nothing more), and the scan is linear at **2.1-2.2 us per row** in both builds (4.78 ms / 2,200, 6.57 ms / 3,000, 25.2 ms / 12,000). The scan yields every 64 pages in both A and B and shows no cost for the shared slicing code. The ~2 ms `UPDATE` max appears in the median rep of all six update rows, A and B alike (cause not investigated), so it is not an A1 effect.

## 3. Build benefit: a second session behind a relation-X build, `cores = 1`

**The finding.** A build holds the core on A for the whole statement; on B it hands the core back every 64 leaves and the probe session gets a turn each time.

Probe latency of `SELECT * FROM small WHERE id = ?` (the second connection), requests that overlapped the build window, microseconds. A's three runs each have only **two** such requests (one issued just before the build and finished when it ended, plus one after), so A is shown per run; B is the median over 3 runs of each percentile (n = 110-112 per run).

| Build | Statement | n probes | p0 | p25 | p50 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| A, run 1 | CREATE INDEX | 2 | 111 | 111 | 111 | 1,048,850 | 1,048,850 | 1,048,850 |
| A, run 2 | CREATE INDEX | 2 | 64 | 64 | 64 | 1,053,824 | 1,053,824 | 1,053,824 |
| A, run 3 | CREATE INDEX | 2 | 102 | 102 | 102 | 1,055,380 | 1,055,380 | 1,055,380 |
| **B**, med of 3 | CREATE INDEX | 110 | 88.9 | 9,251 | 9,576 | 10,338 | 14,691 | 37,829 |
| A, run 1 | CREATE ASSERTION | 2 | 57 | 57 | 57 | 1,779,943 | 1,779,943 | 1,779,943 |
| A, run 2 | CREATE ASSERTION | 2 | 85 | 85 | 85 | 1,783,061 | 1,783,061 | 1,783,061 |
| A, run 3 | CREATE ASSERTION | 2 | 76 | 76 | 76 | 1,719,996 | 1,719,996 | 1,719,996 |
| **B**, med of 3 | CREATE ASSERTION | 111 | 76.5 | 5,330 | 5,371 | 5,668 | 412,462 (runs: 412,462 / 397,173 / 536,630) | 489,011 (runs: 472,707 / 489,011 / 538,864) |

Idle reference (same probe loop before the first build, ~14,000-16,000 requests per run): p50 58.3-65.6, p99 84.3-111.1 us; max 0.68-1.3 ms.

The build's own wall time, seconds (client-measured, three runs each; the yield's price is the difference):

| Statement | A runs | A mean | B runs | B mean | B - A |
|---|---|---|---|---|---|
| CREATE INDEX (1M rows) | 1.047 / 1.054 / 1.055 | 1.052 | 1.091 / 1.121 / 1.050 | 1.087 | +0.035 s (+3.3 %), inside B's 0.071 s spread |
| CREATE ASSERTION (1M rows) | 1.780 / 1.783 / 1.720 | 1.761 | 1.488 / 1.466 / 1.653 | 1.536 | -0.225 s (-12.8 %) |

On B, the probe session got one turn per slice: ~110 probes over a 1.09 s index build, ~111 over a 1.54 s assertion build. Per-slice time is therefore ~9.5 ms (index) and ~5.3 ms (the assertion step costs less per leaf); the probe itself costs ~60 us of that cycle, so the competing statement takes <1 % of the build's core time and the build's own wall is not stretched by the traffic (B's three index builds took 1.050-1.121 s against A's 1.047-1.055 s). One consequence worth stating: **a yielded build is not interruptible faster than a slice**, and the p50 of a neighbour is the slice length, not zero. At 9.5 ms it is 160x the idle p50 (59 us), against 17,800x (1.05 s) on A.

**What is not bounded.**

| Statement | run | slow probes (> 20 ms): ms after build start, latency ms |
|---|---|---|
| CREATE INDEX | 1 / 2 / 3 | (1,053.9, 37.8) / (1,080.6, 40.2) / (1,015.9, 33.7) |
| CREATE ASSERTION | 1 | (252.5, 412.5), (969.5, 472.7) |
| CREATE ASSERTION | 2 | (234.8, 397.2), (930.0, 489.0) |
| CREATE ASSERTION | 3 | (231.1, 536.6), (1,070.4, 538.9) |

`CREATE INDEX` leaves one 34-40 ms stall at ~96-97 % of its window (the end of the build: a tail step, consistent with stage 2a's "tail" being run in a statement frame of its own). `CREATE ASSERTION` leaves two stalls per run of 0.40-0.54 s each, together **0.87-1.08 s of the 1.47-1.65 s build, 59-65 %**, both mid-build. They are not periodic work of the server: with `checkpoint_interval_ms = 0` a B run shows the same two (433 ms at 234 ms, 540 ms at 967 ms; wall 1.549 s). They are not present at 500,000 rows: B, one run, 0.312 s wall, probe n = 56, no probe over 20 ms (max 19.6 ms); A, one run, 0.381 s. So the assertion build is non-linear in this relation between 500K and 1M rows (B 0.31 -> 1.54 s, A 0.38 -> 1.76 s for 2x the rows), and on B the superlinear part is exactly the part that does not yield. Section 7 attributes the stalls: they are WAL segment rolls on the append path, not the slicing. Where the claim "bounded by a slice" is written in the A1 documents, it holds for `CREATE INDEX` at this size and for the assertion's p50 but **not for the assertion's tail at 1M rows**.

## 4. Where the time goes (rule 3)

Under `relaxed` no durability wait sits in any measured statement, and the probe touches a different relation than the build, so there is no lock wait between them (the relation X is on `big`). The probe's latency during a build on B is therefore the sum of two terms, both read from the tables above:

| Component | CREATE INDEX | CREATE ASSERTION |
|---|---|---|
| Client, socket, parse, one B+ tree descent (idle p50) | ~59 us | ~59 us |
| Wait for the core while the current slice finishes (p50 minus the above) | ~9.5 ms | ~5.3 ms |
| Wait inside an unsliced run (the tail, p99-max) | 34-40 ms, once, at the end | 0.40-0.54 s, twice, mid-build, at 1M rows |
| fsync / commit | not applicable (relaxed) | not applicable (relaxed) |

On A the second row is the build's whole remaining time. The statement-path cells (section 2) have one component each: the same ~45 us (insert, update) and ~65 us (select) on both builds, of which the client/socket floor of this Python driver is most.

## 5. What this teaches

1. **The slice is the unit of fairness, and it is long.** 64 leaves is 9.5 ms of index work here: a neighbour's `SELECT` that costs 59 us idle pays 160x for it. The yield gives a bounded wait, not a short one; the A1 number that matters to a latency-sensitive neighbour is the slice cost, which is a function of row width (215 B rows here) and what the step does per leaf, and `SlicePolicy::pages_per_slice` is the knob.
2. **Yielding is free for the build.** The index build pays +3.3 % (unresolved) and the assertion build is 13 % faster on B; whatever the cause of the latter (stage 2b rewrote the assertion build as `AssertionBuild` over a shared `LeafCursor`; main also merged in this window), the cooperative restructure did not slow it.
3. **The statement path pays nothing for any of it.** Twelve cells, best-rep deltas -2.3 % to +0.9 %, no consistent sign, scan linear at ~2.1 us per row.
4. **The expectation that stage 2b makes every relation-X build bounded by a slice is contradicted at 1,000,000 rows for `CREATE ASSERTION`** (stage 2b's commit message says every relation-X build gives its core back every `pages_per_slice` leaves): two unsliced runs of 0.4-0.54 s remain, and they are what makes the assertion build superlinear from 500K to 1M rows. The index build's single 34-40 ms tail is the same class at 1/12 the size. The spec text carrying that claim should be re-read against this table.

## 7. Diagnostic: the stalls are WAL segment rolls (one B run, 1M rows)

Hypothesis tested: the two 0.4-0.54 s CREATE ASSERTION stalls are WAL segment rolls (64 MiB segments), driven by the build's per-row log volume, not by the yield. One more B cell (`kds_B`, same config, one server, 14:30 UTC, load as in the main run), with `SHOW META` read before and after each build and the wal directory polled every ~50 ms (first-seen time of each file name; the poll loop's own period is ~60 ms, so alignments are good to about that). The driver gained the `SHOW META` snapshots for this; files in `archive/.../json/diag2*` and `scripts/diag2.*`.

| Build | wall | bytes logged (`wal_append_lsn` delta) | per row | segment boundaries crossed (`wal_rolls`) | roll latch hold, total (`wal_roll_hold_us` delta) | longest hold (`wal_roll_hold_max_us` after) | slow probes (start-end ms from build start) |
|---|---|---|---|---|---|---|---|
| CREATE INDEX | 1.066 s | 32,348,648 (30.9 MiB) | 32 B | 1 (18 -> 19) | 20.3 ms | 20.3 ms | 1,030.0-1,066.4 (36.4 ms) |
| CREATE ASSERTION | 1.647 s | 128,746,840 (122.8 MiB) | 129 B | 2 (19 -> 21) | **1,055.1 ms** | **532.1 ms** | 231.7-760.4 (528.7 ms); 1,059.9-1,599.5 (539.6 ms) |

Confirmed for the assertion. Two rolls, two stalls; the two stall lengths (528.7 and 539.6 ms, sum 1,068 ms) match the rolls' total hold (1,055 ms) and the longest hold (532 ms). The hold is dominated by `wal_roll_sync_gate_us` (+1,019,636 us of the +1,055,055 us); the preallocated-body step the known-gaps entry points at is not it here (`wal_roll_body_us` 0, `wal_roll_body_hit` +2: both bodies were ready). Each segment's `.tmp` file was first seen 92.7 ms and 949.0 ms into the build, 139 and 111 ms before its stall began, and the `.log` rename was first seen at 786.1 and 1,589.2 ms, 26 ms and 10 ms after the stall ended: each stall window contains the segment's publish (rename), and the stall begins during the segment's preparation. `wal_ring_full` rose by 106 during the build (the log ring filled 106 times). The cost is proportional to volume, not to leaves: 129 B of log per row, 64 MiB per roll, so a build crosses boundaries in proportion to its rows (2 at 1M rows). The 500K runs, which would log about 62 MiB, showed no stall, but their `SHOW META` was not read, so whether they crossed a boundary is not known.

Partly confirmed for the index. It logs 32 MB (about 5x less per row), crosses one boundary, and the one roll's hold is 20.3 ms, with the probe stall of 36.4 ms at the end of the build (1,030-1,066 ms, 97 % of the wall). The segment `.log` was first seen at ~1,098 ms, after the build's end, which is within the poll's lag, so the alignment is consistent but not tight. Whether the 32 MB are appended in the tail alone (the end-of-build stall suggests it) was not separated: the LSN delta spans the whole build and the boundary (1,275,068,416) sits 16.6 MB in.

Not tested: that A pays the same rolls (A was not run with `SHOW META`; its build is a single stall, so a roll inside it is invisible), and whether a smaller volume per row, a larger segment, or a roll off the append latch removes the stalls. The yield mechanism is not implicated: its slices keep running between the two stalls (the probes are served at the 5.4 ms cadence in the 0.8 s between them), and the stalls follow the log volume.

## 6. Reproducing

```
cp build-release/kds_server <run dir>/kds_B     # each build, measured from the copy
# server: cores = 1, durability = relaxed, buffer_pool_frames = 131072, log_level = warn, fresh data file
tools/a1_close_benchmark.py --port P --mode stmt  --sizes 200,1000,10000 --ops 2000 --scan-ops 20
tools/a1_close_benchmark.py --port P --mode build --rows 1000000 --note-len 200 --idle 1.0
```

The interleaving, server start and aggregation scripts are in `archive/a1-close-v3.4.0-11-gf53e62db/scripts/` (`run-all.sh`, `srv.sh`, `agg2.py`), the per-cell JSON in `json/`, the host record in `env.log`.
