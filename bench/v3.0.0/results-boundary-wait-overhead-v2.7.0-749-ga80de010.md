# The statement-boundary ceiling wait: interleaved A/B overhead (`v2.7.0-749-ga80de010`)

**Thesis.** The change at `a80de010` makes every statement ask, at its boundary, whether
the instance has published a commit that no snapshot minted now would cover, and waits
if it has. Three findings, one per question the change raises.
(1) **On one connection that is not near a strict commit the cost is about 0.6-1.0 us on
a write and nothing resolvable on a read**: a single-row `INSERT` is +0.6 to +1.0 us
(0.9-1.4 % of a 72 us statement), a one-row `UPDATE` +0.6 us at 1,000 and 10,000 rows
(and nothing at 200), a pk `SELECT` -0.07 to +0.15 us against a floor of 0.2 us. The cost
sits on the statement that follows a commit, which is why writes pay it and reads do not.
(2) **Next to a relaxed writer, a reader on another core pays nothing resolvable**: read
QPS -0.2 % against a floor of the same size, p99 unchanged, the shared-line loop costing
at most 0.3 us at the median at one of three sizes. (3) **The new wait is real and large
for a session that does not commit, and adds nothing for one that does.** A read-only
connection beside one strict committer and one session issuing relaxed commits drops from
13,401 to 1,402 reads per second (-89.5 %, 12 of 12 triplets, p99 128 to 4,749 us), because
38 % of its statements now wait out a strict sync (mean 1.7 ms each). The session that
itself issues the relaxed commits does not change (1,592 vs 1,587 statements per second),
because the engine at `6b0d8412` already made a session wait for its own commit's bound;
what the change adds is that wait for **every other session on the instance**. A strict
committer with no other writer produces no wait (control: 1 ceiling wait per run against 0, reads unchanged).

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-10 21:25:55 - 22:44:24 UTC. Cell 1 21:25:56-21:36:07 (main pass 21:25:56-21:32:11, swap pass 21:32:13-21:36:07); cell 2 at 1,000 rows 21:36:08-21:42:08; cell 3 at 1,000 rows 21:42:09-21:54:20; cell 3c at 1,000 rows 21:54:21-22:07:31; cell 3b 22:07:32-22:13:25; the 200- and 10,000-row passes of cells 2, 3 and 3c 22:13:27-22:44:23 (`archive/.../windows.txt`) |
| Branch / worktree | `worktree-fix-strict-marker-other-session`, worktree `fix-strict-marker-other-session` |
| B (measured) | `a80de010`, `git describe --tags` = `v2.7.0-749-ga80de010` (HEAD commit 2026-10-10 21:19:42 UTC). Tree clean at the start (`git status --short` empty) and at the end; the files this run adds are untracked |
| A (baseline) | base commit `6b0d8412`, `v2.7.0-747-g6b0d8412`. B's diff against it (`git diff 6b0d8412 HEAD`): `src/server/command_dispatcher.cpp` (`UncoveredCommit`), `include/kds/server/command_dispatcher.hpp`, `include/kds/txn/instance_visibility.hpp`, `include/kds/txn/manager.hpp`, `tests/strict_marker_snapshot_rig_test.cpp`, `docs/spec/txn.md` and one `docs/inflight/bugs` entry (deleted) |
| Binary B | `build-release/kds_server` built by this session 2026-10-10 21:20:32-21:21:26 UTC (`nice`, CPUs 0-5, no measurement running), mtime 21:21:26, **after** HEAD's commit time (21:19:42); `CMAKE_BUILD_TYPE=Release`. Copy `sha256 d9ab2dda08e8d8c0b85d5c5f381f015eda22f213d1092ac7e8a856c415ea42c2`, unchanged at the end (re-hashed) |
| Binary A | `sha256 ee29d277d3b19939f1c93bdd57e789796c3a987dbd421deafc862afa2c7ef574`. **Provenance caveat:** reused, not rebuilt. It is the Release build made 2026-10-10 15:14 UTC from a base worktree at `1a5176ae` (`v2.7.0-746-g1a5176ae`), the parent of `6b0d8412`; `git diff --name-only 1a5176ae 6b0d8412 -- src include CMakeLists.txt tests` lists 0 files (re-run here), so the engine source is `6b0d8412`'s. The source file's own mtime was not recorded; copies of it in other run directories are dated 15:17 UTC, its copy used here is `/home/cdkbs/bench-runs/at/kds_server-A` |
| Copies | every server of the run was started from `/home/cdkbs/bench-runs/bw/kds_server-A` or `-B` (hashes above), never from a build tree; hashes in `archive/.../binaries.txt` |
| Device | data files and logs under `/home/cdkbs/bench-runs/bw/work1` and `work23` (deleted after each run), `/dev/root`, ext4, on `nvme0n1` (MSFT NVMe Accelerator v1.0); `df -T` confirmed, not tmpfs |
| Host | AMD EPYC 9V74, 8 logical CPUs (4 physical cores, siblings 0/1, 2/3, 4/5, 6/7), Linux 7.0.0-1014-azure, shared with other sessions |
| Server config | `durability = relaxed` (the server default; strict commits are per transaction, section 2), `buffer_pool_frames = 131072`, `log_level = warn`, BTREE relations, chosen ports (15760/15761 cell 1, 15780 cells 2-3; never 15432). Cell 1: `cores = 1`, both servers of a pair on CPU 6, client on CPU 7. Cells 2-3: `cores = 2`, `reactor_cpus = 6,4` (core 0 on CPU 6, core 1 on CPU 4), process affinity 4,6; one server at a time |
| Suite | not executed. No code changed in this measurement; the suite belongs to the change's stages (`tests/strict_marker_snapshot_rig_test.cpp` is the change's own) |
| PostgreSQL floor | not measured for this shape; the comparison is B against A (rule 4) |
| Drivers | scratch, in the archive's `drivers/`; `tools/` untouched. `bench/docs/` does not exist in the tree (`bench/README.md`), so rule 7's entry is the command list in section 2 and the drivers' docstrings |
| Archive | `bench/v3.0.0/archive/boundary-wait-overhead-v2.7.0-749-ga80de010/`: the drivers, every run's host record and driver output, the per-run summaries and aggregates of every cell, the raw per-statement samples of cell 1 and of cell 3c at 1,000 rows. The raw samples of the other cells were not archived (20 MB archive cap); their per-run percentile summaries are |

## 2. Method

**What changed, in the terms the cells need.** At a statement's start, `UncoveredCommit` takes the
higher of the session's last acknowledged commit and the instance's published commit ceiling (one
atomic load), skips everything if that is at or below the highest bound this dispatcher has already
seen covered, and otherwise runs `SnapshotCeiling()` (a loop over each core's commit marker). If a
marker caps the ceiling below the bound, the statement waits at its boundary until the marker lifts.
A marker is held by a commit between its start and its publish, and for a `strict` commit that spans
its `fdatasync`.

**Cells.** One knob per row against A; every cell runs binary A and binary B under identical
conditions.

| Cell | Question | Shape | Equal work |
|---|---|---|---|
| 1 | what a statement pays when nothing waits | `cores = 1`, `relaxed`, one connection, serial; pk `SELECT`, single-row `INSERT` (named ascending id), one-row `UPDATE` by pk, `SHOW NAMESPACES` as the client/socket floor, the `INSERT` arm repeated as the replicate | 16,000 statements per arm per server per run, in 16 blocks of 1,000 alternating which server goes first; 8 main pairs and 5 swap pairs at each size |
| 2 | the loop on a contended line | `cores = 2`, `relaxed`; writer connection on core 0 committing single-row `INSERT`s back to back, reader connection on core 1 doing pk `SELECT`s | reader: 20,000 `SELECT`s; the writer runs until the reader finishes |
| 3 | the wait, for the session that commits | `cores = 2`; connection on core 0 committing `strict` INSERTs back to back (frames `BEGIN` durability 1, `INSERT`, `COMMIT`); connection on core 1 alternating a pk `SELECT` and a `relaxed` single-row `INSERT` ("mix") | mix: 10,000 iterations (20,000 statements) |
| 3c | the wait, for a session that does not commit | cell 3 plus a third connection on core 1 that only does pk `SELECT`s, back to back, until the mix finishes ("bystander"; client on CPU 3) | mix: 10,000 iterations; the bystander's count is whatever it completes |
| 3b | control for 3: is the wait caused by a later commit | the strict connection and a reader doing pk `SELECT`s only (nothing else commits) | reader: 20,000 `SELECT`s |

**Rows.** The read relation `t` holds `--rows` rows, ids 1..rows (loaded in 250-row `INSERT`s), at 200,
1,000 and 10,000, as the first column of every table. Cell 1 runs all three sizes (8 main and 5 swap
pairs each); cells 2, 3 and 3c run 12 triplets at 1,000 rows and 6 at 200 and 10,000; cell 3b runs
12 triplets at 1,000 only (nothing in it scales with the rows). A write goes to its own relation (`w`
for the writer, `m` for the mix), so no cell writes the relation it reads. The pk `SELECT` does not
scale with the rows and the three sizes are shown as the evidence of it.

**Placement.** SO_REUSEPORT spreads accepts, so each client opens a connection, reads `core=` from `SHOW META`,
and reopens until it is on the core the cell wants (`connect_attempts` in the archive; 1-10 attempts, mean 2.0).
Every connection of every run reported the core it was placed on; the strict committer is on core 0
(CPU 6, client on CPU 7), the reader/mix/bystander on core 1 (CPU 4, clients on CPUs 5 and 3).

**Fresh server and data file per run** (`bench/README.md` rule 3 and the agent rules). Cell 1 runs two
servers side by side, one idle while the other is timed. Cells 2-3 run **one server at a time**: each
triplet is binary A, binary B and **binary A again (A2)** in a rotating order over the six
permutations, so A2 - A is the in-run noise floor of the delta B - A and the order is balanced.

**Statistics.** Percentiles are nearest rank over a run's samples; a table cell is the median over
runs (pairs, triplets) of the per-run percentile. A delta is B - A within a pair or triplet, shown as
median [Q1, Q3]; "B>A" counts the pairs or triplets where it is positive. Cell 1's **swap pass** gives
the position-free binary effect: with `d_main = slotB(binary B) - slotA(binary A)` and
`d_swap = slotB(binary A) - slotA(binary B)`, the binary effect is `x = (d_main - d_swap) / 2` and the slot
effect (a property of the server's place in the pair) is `s = (d_main + d_swap) / 2`. **QPS is derived
as 1,000,000 / p50 us** for the serial cell-1 arms (the main pass only, so it carries the slot effect); in
cells 2-3 it is operations over the connection's own measured span (measured, not derived). In cell 3
and 3c "statements per second" counts both the `SELECT` and the `INSERT` of the mix.

**Reproduce.**

```
# in the archive's drivers/, with kds_server-A and kds_server-B in /home/cdkbs/bench-runs/bw
python3 bw_run1.py 8 200,1000,10000 res/c1-main                 # cell 1, main pass
BW_SWAP=1 python3 bw_run1.py 5 200,1000,10000 res/c1-swap       # cell 1, swap pass
python3 bw_run23.py c2  12 1000 20000 res/c2-n1000              # args: cell triplets rows N resdir
python3 bw_run23.py c3  12 1000 10000 res/c3-n1000
python3 bw_run23.py c3c 12 1000 10000 res/c3c-n1000
python3 bw_run23.py c3b 12 1000 20000 res/c3b-n1000
python3 bw_an1.py res/c1-main res/c1-swap                       # cell 1 analysis
python3 bw_an23.py res/<cell> ; python3 bw_agg23.py res/<cell>/summary.json   # cells 2-3
```

(`bw_all.sh` is the exact sequence run; `post.sh` and `gen_tables.py` produce the tables.)

### Host state

291 runs (39 in cell 1, 252 in cells 2-3), every one started after a check that no `cc1plus`, `ld`,
`ctest`, `kds_tests`, `ninja`/`make` process or `cmake --build` was live (process names, not command
lines: another session's idle wait loop that names `ctest` is not a compile) and
the runner waited for that when it was not (it never had to: `waited_s = 0` in all 291). **Excluded
runs: 0 of 291.** Load average at the start of a run was 0.15-2.35 (1 minute). The per-CPU busy fraction was sampled each second during every run: the
pinned CPUs (6 and 7 for cell 1; 3-7 for cells 2-3) carried the run, and **no other CPU had a run-mean busy above 0.5** (maximum 0.403). Three
runs had one 1-second sample above 0.5 on **CPU 0** (1.0, 0.727, 1.0 in `c2-n1000-t1-B`,
`c3-n1000-t1-A`, `c3b-n1000-t1-A`), which coincide with the analysis step this session ran, pinned to CPU 0, as the preceding series ended; CPU 0 shares a physical core with CPU 1 only, and no
measured CPU shares one with it. They are kept, and removing them changes nothing (cell 2 read p50 B-A +0.3 [+0.1, +0.5] with 11
triplets, +0.3 [+0.1, +0.7] with 12; cell 3b the same pair of rows, +0.4 and +0.3). Nothing was built
during the measurement; the one build (B) finished at 21:21:26, four minutes before the first run.
The other CPUs' run-mean busy was at most 0.067 in every run except two (0.403 and 0.225, the runs holding the CPU-0 bursts). The `kds_server` under `/home/cdkbs/autotrade`
(port 15432) was running throughout on its own port and was not touched.

## 3. Cell 1: one connection, `cores = 1`, relaxed, nothing waiting

**A write pays about 0.6-1.0 us; a read pays nothing resolvable.** A single-row `INSERT` is 72.2, 71.8 and 71.8 us p50 on
A and 72.6, 72.6 and 72.9 us on B at 200, 1,000 and 10,000 rows. In the swap pass the effect follows the binary, not the
slot: with binary B in slot A, slot B minus slot A is negative in 5 of 5, 5 of 5 and 4 of 5 pairs (the
matrix's swap column shows the positive count, 0/5, 0/5, 1/5), so the position-free effect `x` is +0.62,
+0.90 and +0.98 us (+0.9 %, +1.3 %, +1.4 %). The replicate arm reproduces it (+0.55, +0.77, +0.90 us), and the
engine's own time per statement (`sched_foreground_polled_us` over polls, from `SHOW META`) agrees from
the server side: +0.37, +0.41, +0.71 us. A one-row `UPDATE` is +0.63 and +0.58 us at 1,000 and 10,000 rows (+0.8 %; 7 of 8 pairs,
swap pass reversed in 4 of 5 and 4 of 5), and at 200 rows nothing (-0.07 us, 4 of 8 pairs). The pk
`SELECT` is +0.08, +0.15 and -0.07 us, and `SHOW NAMESPACES`, which touches no storage, +0.18, +0.05 and -0.07 us.

**The floor.** The controls give it: `SHOW NAMESPACES` and the pk `SELECT`, both of which pass the new
boundary check with no commit to find, move by at most 0.18 us in the position-free effect and by at most 0.43 us in the
slot effect; the replicate arm of the same binary differs from its first arm by 0.0-0.2 us at p50. **A
delta under about 0.2 us is not a finding**, and the `INSERT` and the 1,000/10,000-row `UPDATE` deltas
are 3 to 5 times that and reproduce in sign across the pairs, the replicate, the swap pass and the
engine-side counter.

**Why writes and not reads (a reading of the code, not isolated by a probe).** A read-only statement
publishes no commit, so the next statement finds the published ceiling at or below
`covered_through_` and pays the one load. A write publishes a commit, so the statement after it finds
a bound above `covered_through_`, reads the markers with `SnapshotCeiling()` and re-checks with
`CeilingCovers`. At `cores = 1` that is one marker; its cost, 0.4-0.7 us of engine time, is a few
atomic loads and a function call each side of a statement, not a wait: `contention_ceiling_waits` was 0 at the end of every
cell-1 run on both engines.

Matrix (QPS derived from the main pass's p50; the delta columns are microseconds of p50, and the
effect column `x` is the position-free one that the verdict uses, so a QPS column and an `x` column can
differ by the slot effect):

| Statement | Rows | A QPS | B QPS | B-A QPS | B-A % | d p50 us, main med [IQR] | B>A pairs | d p50 us, swap med | binary effect x us | slot effect s us |
|---|---|---|---|---|---|---|---|---|---|---|
| pk point SELECT | 200 | 10,352 | 10,357 | +5 | +0.05 % | +0.15 [-0.17, +0.40] | 5/8 | +0.00 (2/5) | +0.08 | +0.08 |
| pk point SELECT | 1,000 | 10,315 | 10,256 | -58 | -0.56 % | +0.30 [+0.18, +0.43] | 7/8 | +0.00 (2/5) | +0.15 | +0.15 |
| pk point SELECT | 10,000 | 10,599 | 10,526 | -73 | -0.68 % | +0.35 [+0.10, +0.92] | 7/8 | +0.50 (3/5) | -0.07 | +0.43 |
| single-row INSERT | 200 | 13,841 | 13,774 | -67 | -0.48 % | +0.45 [+0.00, +0.90] | 5/8 | -0.80 (0/5) | +0.62 | -0.18 |
| single-row INSERT | 1,000 | 13,937 | 13,774 | -163 | -1.17 % | +1.10 [+0.48, +1.10] | 7/8 | -0.70 (0/5) | +0.90 | +0.20 |
| single-row INSERT | 10,000 | 13,918 | 13,717 | -200 | -1.44 % | +0.95 [+0.72, +1.18] | 8/8 | -1.00 (1/5) | +0.98 | -0.02 |
| single-row INSERT, repeated (replicate) | 200 | 13,831 | 13,755 | -76 | -0.55 % | +0.40 [+0.08, +0.90] | 6/8 | -0.70 (0/5) | +0.55 | -0.15 |
| single-row INSERT, repeated (replicate) | 1,000 | 13,928 | 13,765 | -163 | -1.17 % | +1.05 [+0.45, +1.10] | 7/8 | -0.50 (0/5) | +0.77 | +0.27 |
| single-row INSERT, repeated (replicate) | 10,000 | 13,879 | 13,717 | -162 | -1.17 % | +0.90 [+0.58, +1.15] | 7/8 | -0.90 (1/5) | +0.90 | +0.00 |
| one-row UPDATE by pk | 200 | 13,029 | 13,038 | +8 | +0.07 % | +0.05 [-0.53, +0.25] | 4/8 | +0.20 (4/5) | -0.07 | +0.13 |
| one-row UPDATE by pk | 1,000 | 13,149 | 13,046 | -103 | -0.78 % | +0.65 [+0.18, +1.30] | 7/8 | -0.60 (1/5) | +0.63 | +0.02 |
| one-row UPDATE by pk | 10,000 | 13,072 | 12,945 | -127 | -0.97 % | +0.85 [+0.55, +1.00] | 7/8 | -0.30 (1/5) | +0.58 | +0.27 |
| SHOW NAMESPACES (client/socket floor) | 200 | 19,417 | 19,305 | -112 | -0.58 % | +0.25 [+0.15, +0.92] | 6/8 | -0.10 (2/5) | +0.18 | +0.07 |
| SHOW NAMESPACES (client/socket floor) | 1,000 | 19,493 | 19,380 | -113 | -0.58 % | +0.20 [-0.05, +0.48] | 5/8 | +0.10 (3/5) | +0.05 | +0.15 |
| SHOW NAMESPACES (client/socket floor) | 10,000 | 19,436 | 19,417 | -19 | -0.10 % | -0.05 [-0.12, +0.10] | 2/8 | +0.10 (3/5) | -0.07 | +0.03 |

Distributions (main pass; per-run percentile, median over 8 runs, 16,000 statements per run per
engine; microseconds; the last column is the engine's own time per statement):

| Statement | Rows | Engine | p0 | p25 | p50 | p75 | p99 | engine us/stmt |
|---|---|---|---|---|---|---|---|---|
| pk point SELECT | 200 | A | 90.9 | 95.3 | 96.6 | 101.7 | 128.6 | 31.89 |
|  | 200 | B | 91.5 | 95.2 | 96.6 | 101.7 | 128.4 | 32.02 |
| pk point SELECT | 1,000 | A | 90.4 | 95.2 | 96.9 | 103.2 | 128.7 | 32.35 |
|  | 1,000 | B | 91.3 | 95.8 | 97.5 | 103.8 | 129.7 | 32.54 |
| pk point SELECT | 10,000 | A | 90.2 | 93.2 | 94.3 | 98.4 | 119.5 | 30.22 |
|  | 10,000 | B | 90.3 | 93.9 | 95.0 | 98.9 | 120.5 | 30.72 |
| single-row INSERT | 200 | A | 68.4 | 71.3 | 72.2 | 75.1 | 101.3 | 24.39 |
|  | 200 | B | 67.2 | 71.8 | 72.6 | 75.6 | 102.0 | 24.76 |
| single-row INSERT | 1,000 | A | 68.0 | 71.0 | 71.8 | 74.9 | 100.6 | 24.27 |
|  | 1,000 | B | 69.3 | 71.8 | 72.6 | 75.4 | 101.7 | 24.74 |
| single-row INSERT | 10,000 | A | 68.8 | 71.0 | 71.8 | 74.7 | 101.2 | 24.31 |
|  | 10,000 | B | 69.0 | 72.0 | 72.9 | 75.8 | 101.8 | 25.09 |
| single-row INSERT, repeated (replicate) | 200 | A | 68.9 | 71.5 | 72.3 | 75.2 | 101.3 | 24.48 |
|  | 200 | B | 69.6 | 71.9 | 72.7 | 75.6 | 102.0 | 24.79 |
| single-row INSERT, repeated (replicate) | 1,000 | A | 68.4 | 71.1 | 71.8 | 74.8 | 100.9 | 24.23 |
|  | 1,000 | B | 69.5 | 71.8 | 72.7 | 75.5 | 102.2 | 24.69 |
| single-row INSERT, repeated (replicate) | 10,000 | A | 68.8 | 71.3 | 72.0 | 74.9 | 101.0 | 24.34 |
|  | 10,000 | B | 69.2 | 72.2 | 72.9 | 75.9 | 102.4 | 25.05 |
| one-row UPDATE by pk | 200 | A | 73.1 | 75.9 | 76.8 | 79.9 | 106.4 | 27.75 |
|  | 200 | B | 73.3 | 75.8 | 76.7 | 79.9 | 106.8 | 27.63 |
| one-row UPDATE by pk | 1,000 | A | 72.6 | 75.2 | 76.0 | 79.2 | 106.2 | 27.51 |
|  | 1,000 | B | 73.5 | 75.8 | 76.7 | 79.8 | 106.5 | 27.70 |
| one-row UPDATE by pk | 10,000 | A | 73.0 | 75.7 | 76.5 | 79.6 | 106.0 | 27.44 |
|  | 10,000 | B | 73.8 | 76.4 | 77.2 | 80.4 | 106.9 | 27.85 |
| SHOW NAMESPACES (client/socket floor) | 200 | A | 49.5 | 51.0 | 51.5 | 52.5 | 74.5 | 10.41 |
|  | 200 | B | 49.7 | 51.3 | 51.8 | 53.0 | 75.3 | 10.66 |
| SHOW NAMESPACES (client/socket floor) | 1,000 | A | 49.3 | 50.8 | 51.3 | 52.5 | 74.5 | 10.20 |
|  | 1,000 | B | 49.3 | 51.0 | 51.6 | 52.7 | 75.1 | 10.38 |
| SHOW NAMESPACES (client/socket floor) | 10,000 | A | 49.5 | 51.0 | 51.5 | 52.5 | 74.1 | 10.38 |
|  | 10,000 | B | 49.5 | 51.0 | 51.5 | 52.6 | 74.7 | 10.39 |

The remaining deltas, main pass (median over 8 pairs; microseconds):

| Statement | Rows | d engine us/stmt, main med [IQR] | d p0 | d p25 | d p75 | d p99 | d mean |
|---|---|---|---|---|---|---|---|
| pk point SELECT | 200 | +0.10 [-0.07, +0.47] | -0.15 | +0.15 | +0.10 | -0.30 | +0.32 |
| pk point SELECT | 1,000 | +0.17 [+0.05, +0.30] | +0.75 | +0.30 | +0.35 | +1.00 | +0.41 |
| pk point SELECT | 10,000 | +0.50 [+0.27, +0.76] | +0.20 | +0.40 | +0.25 | +0.75 | +0.40 |
| single-row INSERT | 200 | +0.37 [+0.27, +0.52] | -1.05 | +0.50 | +0.65 | +0.50 | +2.06 |
| single-row INSERT | 1,000 | +0.41 [+0.34, +0.55] | +1.55 | +1.05 | +0.70 | +0.85 | +0.37 |
| single-row INSERT | 10,000 | +0.71 [+0.57, +0.81] | +0.00 | +0.95 | +1.05 | +0.70 | +1.50 |
| single-row INSERT, repeated (replicate) | 200 | +0.38 [+0.28, +0.44] | +0.65 | +0.35 | +0.40 | +0.75 | +0.50 |
| single-row INSERT, repeated (replicate) | 1,000 | +0.45 [+0.28, +0.55] | +1.00 | +0.95 | +0.85 | +0.55 | +1.09 |
| single-row INSERT, repeated (replicate) | 10,000 | +0.74 [+0.58, +0.82] | +0.10 | +1.00 | +1.00 | +1.65 | +0.74 |
| one-row UPDATE by pk | 200 | -0.08 [-0.22, +0.17] | -0.20 | +0.10 | +0.10 | +0.00 | +1.38 |
| one-row UPDATE by pk | 1,000 | +0.30 [+0.12, +0.57] | +1.10 | +0.70 | +0.55 | +0.40 | +2.17 |
| one-row UPDATE by pk | 10,000 | +0.51 [+0.20, +0.67] | +0.85 | +0.80 | +1.15 | +1.00 | +3.34 |
| SHOW NAMESPACES (client/socket floor) | 200 | +0.25 [+0.03, +1.08] | +0.15 | +0.25 | +0.50 | +1.05 | +0.41 |
| SHOW NAMESPACES (client/socket floor) | 1,000 | +0.18 [-0.14, +0.27] | +0.10 | +0.10 | +0.15 | +0.55 | +0.19 |
| SHOW NAMESPACES (client/socket floor) | 10,000 | -0.04 [-0.06, +0.01] | -0.05 | -0.10 | -0.05 | +0.45 | -0.05 |

**Where the time goes (rule 3).** A 72 us `INSERT` is about 24.3 us of engine foreground time
(`SHOW META` counters) and about 48 us of client, socket and reply round trip; a 97 us pk `SELECT` is
30-32 us of engine time and about 65 us outside it (the client renders the row); `SHOW NAMESPACES`, which
executes nothing, is 51.5 us of which 10.4 us is engine time. There is no durability wait (`relaxed` does not sync at commit), and with one
serial connection no lock, conflict or ceiling wait is possible (`contention_ceiling_waits` 0); those shares are
zero by construction, not unmeasured. The change adds 0.4-0.7 us (1.5-3 %) to the engine's share of a
write and nothing resolvable to a read's.

## 4. Cell 2: a reader beside a relaxed writer, `cores = 2`

**Nothing resolvable.** With a relaxed writer committing back to back (12,800 commits per second on
core 0), the reader on core 1 gets 9,382 (A) and 9,346 (B) reads per second at 1,000 rows, -18 QPS
[-69, +53] against a floor of -18 [-69, +87] (A2 - A, the same binary): the change is the floor.
p99 is 126.0 vs 125.9 us. The median read is +0.3 us in B at 1,000 rows (9 of 12 triplets positive, IQR
[+0.1, +0.7], A2 - A is 0.0 [-0.3, +0.3]), which is at the edge of the floor and absent at 200
rows (+0.5, 4 of 6, floor -0.8) and at 10,000 (-0.2, 2 of 6). The shared-line loop therefore costs a reader at most
0.3 us, 0.3 %. The relaxed writer's own rate does not move (12,846 vs 12,794 commits per second,
floor +3). The maximum is one stall of 35-98 ms (median per slot) in every slot including A2; its B - A is
-1.8 ms [-15.0, +7.0] against A2 - A +4.1 ms [-11.9, +22.0]: not a finding. At 1,000 rows B shows a median of 26 ceiling waits
per run totalling 11 us (A: 0): a reader sometimes meets a relaxed commit's marker window and waits less than a microsecond.

| Measure | A | B | A2 (A again) | B-A med [IQR] | B>A triplets | A2-A med [IQR] (floor) |
|---|---|---|---|---|---|---|
| read p0 us | 91.7 | 91.8 | 91.7 | +0.1 [-0.2, +1.1] | 7/12 | +0.3 [-0.8, +0.7] |
| read p25 us | 96.5 | 97.0 | 96.5 | +0.3 [+0.1, +0.6] | 10/12 | +0.1 [-0.3, +0.5] |
| read p50 us | 98.0 | 98.3 | 97.8 | +0.3 [+0.1, +0.7] | 9/12 | +0.0 [-0.3, +0.3] |
| read p75 us | 101.0 | 101.5 | 101.1 | +0.1 [+0.1, +0.6] | 10/12 | -0.0 [-0.2, +0.3] |
| read p99 us | 126.0 | 125.9 | 125.7 | -0.2 [-0.3, -0.0] | 3/12 | -0.1 [-1.1, +0.5] |
| read max us | 90,898.8 | 93,658.3 | 98,026.9 | -1,826.5 [-15,020.9, +6,977.5] | 6/12 | +4,149.9 [-11,854.0, +21,993.7] |
| read QPS | 9,382.4 | 9,345.5 | 9,369.5 | -18.4 [-69.0, +52.8] | 5/12 | -17.8 [-68.9, +86.5] |
| writer commits/s (relaxed INSERT) | 12,845.7 | 12,794.1 | 12,856.0 | +15.8 [-95.6, +57.2] | 7/12 | +3.3 [-74.3, +165.6] |
| writer unit p50 us | 73.2 | 73.7 | 73.2 | +0.2 [-0.1, +0.6] | 7/12 | -0.1 [-0.6, +0.3] |
| writer unit p99 us | 97.8 | 98.2 | 97.6 | -0.1 [-0.2, +0.7] | 5/12 | -0.1 [-1.0, +0.0] |

Row sweep (read and writer, all three sizes):

| Cell | Measure | Rows | A | B | A2 | B-A med [IQR] | B>A | A2-A (floor) | triplets |
|---|---|---|---|---|---|---|---|---|---|
| c2 | read p50 us | 200 | 97.0 | 97.4 | 96.3 | +0.5 [-0.5, +0.9] | 4/6 | -0.8 | 6 |
| c2 | read p50 us | 1,000 | 98.0 | 98.3 | 97.8 | +0.3 [+0.1, +0.7] | 9/12 | +0.0 | 12 |
| c2 | read p50 us | 10,000 | 95.1 | 94.8 | 95.0 | -0.2 [-0.9, +0.5] | 2/6 | -0.2 | 6 |
| c2 | read p99 us | 200 | 122.7 | 123.2 | 122.1 | +0.6 [-0.7, +1.3] | 4/6 | -0.6 | 6 |
| c2 | read p99 us | 1,000 | 126.0 | 125.9 | 125.7 | -0.2 [-0.3, -0.0] | 3/12 | -0.1 | 12 |
| c2 | read p99 us | 10,000 | 121.8 | 121.8 | 121.8 | +0.0 [-1.1, +0.8] | 3/6 | -0.2 | 6 |
| c2 | read QPS | 200 | 9,688.4 | 9,654.3 | 9,742.2 | -65.6 [-79.7, +65.7] | 2/6 | +44.4 | 6 |
| c2 | read QPS | 1,000 | 9,382.4 | 9,345.5 | 9,369.5 | -18.4 [-69.0, +52.8] | 5/12 | -17.8 | 12 |
| c2 | read QPS | 10,000 | 9,847.5 | 9,861.9 | 9,848.9 | +14.4 [-17.1, +132.7] | 3/6 | +10.7 | 6 |

## 5. Cell 3: the wait, seen from the session that commits

**The session that issues the relaxed commits does not change.** The mix connection runs at
1,592 (A), 1,587 (B) and 1,589 (A2) statements per second, strict commits at 405 / 402 / 402 per second,
and **A already waits as often as B**: 5,063 ceiling waits per run in A, 5,054 in B, 5,076 in A2, each about 2.0 ms (10.4 s
over the run). In A the cause is the session's own bound: it commits a relaxed `INSERT`, and its next
statement waits until the strict marker that caps the ceiling below that commit lifts. Half of the mix's
`SELECT`s wait (p25 108 us, p75 2,029 us in both), so the median of the mix `SELECT` sits on the cliff between the two modes and is not a usable
estimator in this cell (A 973, B 933, A2 966 us; B - A -25 us [-260, +40], A2 - A -2 us [-22, +13]); the
quartiles carry it. The change adds nothing here because the bound a statement waits for is already
its own commit, and the strict marker is the same marker. The same holds at 200 and 10,000 rows
(statements per second +12 [-12, +20] and +5 [-17, +24]).

| Measure | A | B | A2 (A again) | B-A med [IQR] | B>A triplets | A2-A med [IQR] (floor) |
|---|---|---|---|---|---|---|
| mix SELECT p0 us | 98.8 | 99.2 | 98.7 | +0.2 [-0.7, +1.1] | 7/12 | -0.2 [-1.2, +0.8] |
| mix SELECT p25 us | 107.8 | 108.1 | 107.8 | +0.5 [-0.5, +1.1] | 8/12 | +0.0 [-0.5, +0.7] |
| mix SELECT p50 us | 973.5 | 932.9 | 965.9 | -24.9 [-259.9, +39.6] | 5/12 | -2.1 [-21.5, +13.2] |
| mix SELECT p75 us | 2,029.0 | 2,029.8 | 2,029.0 | +0.4 [-12.4, +15.6] | 6/12 | +16.7 [-17.6, +26.2] |
| mix SELECT p99 us | 6,129.7 | 6,211.6 | 6,339.4 | +34.9 [-52.0, +249.7] | 8/12 | +147.4 [-6.1, +497.2] |
| mix SELECT max us | 41,257.3 | 55,978.5 | 47,521.6 | +14,944.8 [+2,238.6, +25,322.9] | 10/12 | +9,966.5 [+650.7, +16,464.2] |
| mix INSERT p0 us | 80.0 | 80.8 | 80.0 | +0.8 [+0.4, +2.0] | 10/12 | -0.1 [-0.5, +0.8] |
| mix INSERT p25 us | 84.8 | 85.4 | 84.9 | +0.6 [+0.2, +0.9] | 11/12 | -0.0 [-0.3, +0.3] |
| mix INSERT p50 us | 92.3 | 92.8 | 92.1 | +0.5 [+0.2, +0.8] | 9/12 | -0.2 [-0.7, +0.3] |
| mix INSERT p75 us | 95.4 | 96.0 | 95.3 | +0.6 [+0.3, +0.7] | 11/12 | -0.1 [-0.3, +0.3] |
| mix INSERT p99 us | 122.0 | 123.7 | 123.2 | +1.3 [+0.3, +2.4] | 10/12 | +1.3 [+0.1, +2.6] |
| mix INSERT max us | 26,379.0 | 2,757.7 | 3,974.1 | -12,876.8 [-34,284.1, -873.1] | 3/12 | -21,229.5 [-24,663.2, +0.4] |
| mix statements/s (SELECT + INSERT) | 1,591.6 | 1,587.0 | 1,589.0 | -6.3 [-31.4, +10.3] | 6/12 | -8.3 [-36.8, +7.4] |
| strict commits/s | 404.7 | 402.4 | 402.0 | +1.0 [-6.0, +2.4] | 7/12 | -4.6 [-8.1, +1.4] |
| strict unit p50 us | 2,315.6 | 2,304.8 | 2,308.0 | +0.8 [-15.4, +23.9] | 7/12 | +19.8 [-23.2, +28.5] |
| strict unit p99 us | 8,556.0 | 8,944.0 | 8,775.7 | +120.9 [-55.9, +669.8] | 8/12 | +256.7 [-21.3, +542.1] |
| ceiling waits per run (instance) | 5,063.0 | 5,054.5 | 5,076.5 | +18.5 [-85.0, +93.5] | 6/12 | +11.5 [-57.0, +43.2] |
| ceiling wait us per run (instance) | 10,350,719.5 | 10,403,551.5 | 10,392,843.5 | +91,146.0 [-78,606.2, +235,668.0] | 7/12 | +66,164.5 [-37,463.2, +309,512.5] |

## 6. Cell 3c: the wait, seen from a session that does not commit

**This is the new wait, and it is a 10x slowdown of the bystander.** Add a third connection that only
reads, on the mix's core. In A it runs at 13,401 reads per second with p50 67 us and p99 128 us
(it is never held: nothing it did is uncovered). In B it runs at **1,402 reads per second, -89.5 %**
(12 of 12 triplets, IQR of the delta [-12,030, -11,941]), with p50 99 us, **p75 1,252 us** and p99
4,749 us. Its median moves by only 32 us because 62 % of its statements still find the ceiling covered;
the other 38 % or so (7,656 extra ceiling waits per run [IQR 7,419, 7,885], on 20,070 reads; the mix's own
waits are unchanged) wait **1.7 ms each on average** (13.07 s of extra waiting per run [12.42, 13.55] over a 14.3 s run), and the p99 of 4.7 ms is about two strict commit units' worth (a
unit is 2.2 ms here, and the writer's `wal_syncs` delta is 1.8 device syncs per unit). The delta is the same at 200 and
10,000 rows (-12,072 and -12,134 QPS, 6 of 6 triplets each): it does not scale with the
rows, it scales with strict commits.

**Whose rate falls, whose rises.** The strict committer commits **more** in B (443 vs 372 per second, +19 %, 12 of 12;
floor -4): the bystander parked in a wait is no longer spinning on core 1 at 13,000 reads per second
beside the committer's sync. The mix connection falls by 6 % (1,407 vs 1,494 statements per second,
1 of 12 triplets positive, floor +15; -8 % and -11 % at 200 and 10,000 rows): resolvable, its cause
not isolated by this run. The explanation for the committer's gain is a reading, not a
measurement (the server process may run its other threads on CPUs 4 and 6, and core 1's reactor is on CPU 4); no probe
in this run tests it.

**What bounds it (rule 4b).** The bystander's rate in B is set by the strict commit rate: 1,402 reads per second
over 443 commits per second is 3.2 reads per strict commit. Anything that lengthens a strict commit
(a slower device) lengthens the bystander's wait one for one. The spec text on the change (`docs/spec/txn.md`,
`instance_visibility.hpp`) names the same bound ("two syncs when `strict` commits queue behind each
other") and names the removal: raising a marker to its own LSN once appended, which "needs a seam
the commit path does not have", or snapshots that carry their in-flight set. Neither was tried here
(no engine edits); the measurement is what each would be read against.

| Measure | A | B | A2 (A again) | B-A med [IQR] | B>A triplets | A2-A med [IQR] (floor) |
|---|---|---|---|---|---|---|
| bystander SELECT p0 us | 59.8 | 71.5 | 59.0 | +12.4 [+9.7, +13.3] | 12/12 | -0.1 [-0.6, +0.7] |
| bystander SELECT p25 us | 65.9 | 91.5 | 65.8 | +25.6 [+25.1, +26.0] | 12/12 | -0.1 [-0.4, +0.4] |
| bystander SELECT p50 us | 67.0 | 99.0 | 67.0 | +32.0 [+31.4, +32.4] | 12/12 | -0.0 [-0.4, +0.2] |
| bystander SELECT p75 us | 74.1 | 1,251.5 | 74.2 | +1,177.0 [+1,162.8, +1,188.5] | 12/12 | +0.2 [-0.6, +0.4] |
| bystander SELECT p99 us | 128.4 | 4,748.6 | 128.6 | +4,620.3 [+4,592.3, +4,767.0] | 12/12 | +0.2 [-0.6, +0.6] |
| bystander SELECT max us | 51,700.7 | 52,882.1 | 57,807.6 | +2,587.2 [-3,123.3, +11,196.9] | 8/12 | +4,354.8 [+1,398.4, +10,072.7] |
| bystander QPS | 13,401.1 | 1,402.2 | 13,371.2 | -12,005.4 [-12,029.9, -11,940.9] | 0/12 | -67.1 [-97.0, +122.7] |
| bystander SELECTs per run | 179,620.0 | 20,070.0 | 178,000.0 | -159,590.0 [-162,195.0, -157,480.0] | 0/12 | -460.0 [-2,765.0, +1,645.0] |
| mix SELECT p0 us | 102.8 | 103.7 | 103.9 | -0.8 [-2.3, +4.5] | 5/12 | +1.1 [-1.8, +3.6] |
| mix SELECT p25 us | 115.5 | 116.0 | 115.2 | +0.8 [-0.1, +1.6] | 7/12 | -0.5 [-0.7, +0.7] |
| mix SELECT p50 us | 873.5 | 1,237.9 | 141.6 | +378.8 [+145.8, +1,090.7] | 10/12 | -521.2 [-705.0, +3.3] |
| mix SELECT p75 us | 2,000.9 | 2,064.7 | 2,005.8 | +57.5 [+18.6, +87.0] | 9/12 | +10.3 [-23.9, +25.4] |
| mix SELECT p99 us | 6,452.6 | 6,589.8 | 6,458.4 | -33.4 [-301.0, +203.5] | 5/12 | -49.4 [-263.7, +131.6] |
| mix SELECT max us | 53,796.7 | 50,454.0 | 58,695.1 | +1,992.9 [-21,686.2, +15,034.7] | 7/12 | +3,796.3 [-2,360.9, +11,753.4] |
| mix INSERT p0 us | 81.7 | 81.5 | 81.6 | -0.8 [-1.6, +0.4] | 5/12 | -0.0 [-1.9, +1.2] |
| mix INSERT p25 us | 92.8 | 92.5 | 92.3 | -0.0 [-0.7, +0.4] | 6/12 | -0.5 [-0.7, +0.2] |
| mix INSERT p50 us | 98.2 | 94.9 | 97.6 | -3.1 [-3.5, -2.4] | 0/12 | -0.1 [-1.3, +0.5] |
| mix INSERT p75 us | 119.2 | 103.8 | 119.0 | -15.1 [-15.9, -14.7] | 0/12 | -0.3 [-0.9, +0.5] |
| mix INSERT p99 us | 153.7 | 135.9 | 152.8 | -17.8 [-18.8, -16.8] | 0/12 | -0.8 [-1.8, +1.2] |
| mix INSERT max us | 3,977.6 | 3,984.0 | 3,700.4 | +3.1 [-2,055.8, +8,304.5] | 6/12 | -421.1 [-2,981.7, +1,002.9] |
| mix statements/s (SELECT + INSERT) | 1,493.9 | 1,406.9 | 1,495.4 | -92.4 [-114.7, -52.9] | 1/12 | +14.9 [-10.1, +22.7] |
| strict commits/s | 372.0 | 442.8 | 373.4 | +72.2 [+69.2, +74.8] | 12/12 | -3.6 [-6.1, +5.4] |
| strict unit p50 us | 2,327.6 | 2,199.1 | 2,327.8 | -145.7 [-156.7, -111.4] | 0/12 | +20.0 [-31.5, +38.7] |
| strict unit p99 us | 9,349.2 | 8,224.1 | 9,319.9 | -814.6 [-1,467.7, -661.0] | 0/12 | +109.5 [-349.6, +286.4] |
| ceiling waits per run (instance) | 5,004.0 | 12,667.0 | 4,935.0 | +7,656.0 [+7,418.8, +7,885.2] | 12/12 | -38.0 [-132.5, +32.2] |
| ceiling wait us per run (instance) | 11,098,111.0 | 24,063,528.5 | 11,088,785.0 | +13,067,836.0 [+12,417,506.8, +13,554,467.2] | 12/12 | -93,896.0 [-164,960.5, +120,590.8] |

Row sweep (cells 3 and 3c, all three sizes; the strict-commit and mix rows are per second):

| Cell | Measure | Rows | A | B | A2 | B-A med [IQR] | B>A | A2-A (floor) | triplets |
|---|---|---|---|---|---|---|---|---|---|
| c3 | mix statements/s | 200 | 1,597.3 | 1,608.7 | 1,620.5 | +12.3 [-11.9, +20.4] | 4/6 | +1.6 | 6 |
| c3 | mix statements/s | 1,000 | 1,591.6 | 1,587.0 | 1,589.0 | -6.3 [-31.4, +10.3] | 6/12 | -8.3 | 12 |
| c3 | mix statements/s | 10,000 | 1,606.6 | 1,614.6 | 1,620.8 | +5.2 [-17.4, +24.4] | 4/6 | +13.4 | 6 |
| c3 | strict commits/s | 200 | 404.3 | 403.6 | 403.8 | -1.0 [-3.4, +0.8] | 2/6 | -1.3 | 6 |
| c3 | strict commits/s | 1,000 | 404.7 | 402.4 | 402.0 | +1.0 [-6.0, +2.4] | 7/12 | -4.6 | 12 |
| c3 | strict commits/s | 10,000 | 404.1 | 401.1 | 405.1 | -2.9 [-8.1, +3.0] | 3/6 | +1.0 | 6 |
| c3c | bystander SELECT p50 us | 200 | 66.7 | 97.5 | 66.1 | +31.0 [+30.4, +31.4] | 6/6 | -0.6 | 6 |
| c3c | bystander SELECT p50 us | 1,000 | 67.0 | 99.0 | 67.0 | +32.0 [+31.4, +32.4] | 12/12 | -0.0 | 12 |
| c3c | bystander SELECT p50 us | 10,000 | 65.4 | 98.2 | 65.5 | +32.8 [+32.1, +33.1] | 6/6 | +0.0 | 6 |
| c3c | bystander SELECT p99 us | 200 | 128.1 | 4,809.1 | 127.8 | +4,681.5 [+4,655.3, +4,857.0] | 6/6 | -0.0 | 6 |
| c3c | bystander SELECT p99 us | 1,000 | 128.4 | 4,748.6 | 128.6 | +4,620.3 [+4,592.3, +4,767.0] | 12/12 | +0.2 | 12 |
| c3c | bystander SELECT p99 us | 10,000 | 127.7 | 4,846.4 | 128.1 | +4,718.8 [+4,609.3, +4,813.9] | 6/6 | +0.5 | 6 |
| c3c | bystander QPS | 200 | 13,496.0 | 1,413.6 | 13,581.8 | -12,072.4 [-12,091.8, -12,044.6] | 0/6 | +128.2 | 6 |
| c3c | bystander QPS | 1,000 | 13,401.1 | 1,402.2 | 13,371.2 | -12,005.4 [-12,029.9, -11,940.9] | 0/12 | -67.1 | 12 |
| c3c | bystander QPS | 10,000 | 13,555.9 | 1,416.4 | 13,549.4 | -12,134.1 [-12,184.1, -12,089.4] | 0/6 | -6.5 | 6 |
| c3c | mix statements/s | 200 | 1,536.1 | 1,413.2 | 1,482.2 | -123.0 [-144.0, -89.7] | 0/6 | -43.7 | 6 |
| c3c | mix statements/s | 1,000 | 1,493.9 | 1,406.9 | 1,495.4 | -92.4 [-114.7, -52.9] | 1/12 | +14.9 | 12 |
| c3c | mix statements/s | 10,000 | 1,586.3 | 1,409.5 | 1,575.1 | -176.2 [-193.3, -153.6] | 0/6 | +6.1 | 6 |
| c3c | strict commits/s | 200 | 374.8 | 443.2 | 371.4 | +67.8 [+66.8, +70.0] | 6/6 | -1.8 | 6 |
| c3c | strict commits/s | 1,000 | 372.0 | 442.8 | 373.4 | +72.2 [+69.2, +74.8] | 12/12 | -3.6 | 12 |
| c3c | strict commits/s | 10,000 | 397.0 | 435.2 | 397.4 | +37.8 [+35.3, +43.7] | 6/6 | -0.9 | 6 |

## 7. Cell 3b: the control, a strict committer and nothing else that commits

**No wait.** With the same strict committer and a reader doing pk `SELECT`s only, B shows **1 ceiling wait per
run against 0** and the reader runs at 9,494 (B) vs 9,534 (A) reads per second (-42 [-99, +14], floor -31),
p50 +0.3 us (9 of 12, floor -0.1), p99 +1.0 us (floor -0.2). The same +0.3 us median as cell 2, consistent with a small cost of the loop on a line a committer is
writing, with or without a strict marker (not isolated). The wait therefore needs a **later
commit published inside a strict sync** (cell 3c's mix), as the change's own text says, and not a strict commit
alone. A strict committer serial with itself holds a marker equal to the ceiling its own previous
commit published, which every reader already covers.

| Measure | A | B | A2 (A again) | B-A med [IQR] | B>A triplets | A2-A med [IQR] (floor) |
|---|---|---|---|---|---|---|
| read p0 us | 92.3 | 92.5 | 92.0 | +0.3 [-0.5, +0.8] | 7/12 | -0.5 [-1.2, +0.3] |
| read p25 us | 96.8 | 97.3 | 96.9 | +0.3 [+0.2, +1.1] | 10/12 | -0.1 [-0.6, +1.0] |
| read p50 us | 98.2 | 98.5 | 98.1 | +0.3 [+0.1, +1.0] | 9/12 | -0.1 [-0.6, +0.8] |
| read p75 us | 101.4 | 101.5 | 101.4 | +0.2 [-0.2, +0.9] | 6/12 | +0.3 [-0.4, +0.8] |
| read p99 us | 126.5 | 127.2 | 126.2 | +1.0 [-0.2, +1.4] | 8/12 | -0.2 [-1.1, +1.3] |
| read max us | 61,826.8 | 63,984.3 | 63,320.5 | +1,951.3 [-2,607.0, +6,956.9] | 7/12 | -1,464.7 [-6,976.2, +4,809.7] |
| read QPS | 9,534.0 | 9,494.0 | 9,494.4 | -42.4 [-98.9, +13.7] | 4/12 | -31.0 [-46.9, +5.3] |
| writer commits/s (strict) | 379.2 | 382.8 | 384.0 | +2.4 [-7.3, +10.1] | 7/12 | +5.5 [-2.6, +10.5] |
| writer unit p50 us | 2,320.3 | 2,329.8 | 2,329.1 | +4.2 [-33.8, +29.0] | 8/12 | -4.1 [-26.8, +23.1] |
| writer unit p99 us | 9,102.0 | 8,549.3 | 8,979.1 | -586.0 [-1,414.7, -78.9] | 3/12 | -496.0 [-1,304.9, +398.5] |

## 8. What the run says

1. **The common path costs a write about 1 %.** 0.4-0.7 us of engine time and 0.6-1.0 us of statement
   latency on a write that follows a commit, nothing on a read: the `covered_through_` short-circuit does
   its job on reads and the walk over the markers is paid once per new commit.
2. **The wait is paid by readers of an instance that runs strict commits and any other
   committer.** The condition in cell 3c is two ordinary things at once: a `strict` transaction and any
   other session committing, even `relaxed`. A read-only session then loses 89.5 % of its throughput
   (at one strict committer; the loss is bound by the sync, so it moves with strict commit rate and the
   device). The change's own text states "a read-only `relaxed` session's included"; this is the first
   number for it. `group`, the default durability, holds no marker across its wait by the same text and
   **was not measured here**; neither were more strict committers, a bystander on the committer's core,
   or `cores` above 2.
3. **The same wait was already in the engine for a session's own commit.** Cell 3's A column waits on
   about half the mix's statements; the change widens whom the wait applies to, not what a wait costs.
4. **A design expectation, stated and now priced.** The text the change put in
   `include/kds/txn/instance_visibility.hpp` says the cost "is in the reads of an instance that runs `strict` commits"
   and that "a read-only `relaxed` session's [wait is] included"; cell 3c is the first number for it:
   a read-only session's throughput is bound by the strict commit rate once any other session commits.
   No open decision in `CLAUDE.md` acquired data.
5. **Not measured:** PostgreSQL's floor for these shapes; `group` and `strict` as the instance default; a strict
   committer on core 1; more than two cores; any Python-free client (the client's own round trip is 51.5 us
   of every statement here and dwarfs cell 1's deltas); `DELETE`, multi-row `INSERT` and transactions
   of more than one statement.
