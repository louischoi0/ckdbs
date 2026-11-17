# CU close: a value list served by descents costs a sum where the walk cost a product, and a statement with no list pays nothing this run can see

At `70a307ea` a value list is a key set, and the three questions of the close read as follows.

- **CU-R5, the cost off the set's shape.** Eight statement shapes with no list conjunct, 13 cells, three servers interleaved (A, B, and A2, a byte-identical copy of A): B - A is between -1.59 and +0.80 us of server CPU per statement on 24.7 to 108.6 us, within 2 standard errors of zero in all 13 cells, while the control A2 - A spans -1.10 to +4.46 us. Pooled per shape it is -1.6 % to +1.6 %, and the largest cost the run does not exclude (the pooled delta plus 2 standard errors) is 0.2 to 4.8 % by shape, 4.8 % on a tuple key lookup. A supplement on a quiet host ran the four statements that reach CU-R5's per-row tests (a walked list, an `OR`, a walked write by list, a lookup that records and replays a trail): B - A is -1.81 to +0.02 us, and the largest cost not excluded is 3.0 %. The finding is "no cost found", not "no cost".
- **CU-S0's arms again.** The bar's two cells, a tuple key list over 100,000 rows: 4 values cost 9,974 us as the walk on A and 34.4 us as a set on B (290 times), 64 values cost 78.1 ms and 148 us (526 times). On the indexed column: 12.7 ms against 56.4 us (226 times), and 91.6 ms against 343 us (267 times). CU-S0's estimate of a set, made from statements that existed before anything was built, was 30.9, 113, 37.4 and 242 us for those four cells. Over the 28 cells both runs hold, the measured set is 0.81 to 1.51 times that estimate, and 0.81 to 1.26 times the same estimate made from this run's own arms.
- **The writes CU moved.** `UPDATE ... WHERE id = k AND u >= 0` over 100,000 rows: 8,755 us on A, which visits every leaf under a write latch, 42.8 us on B, which descends (205 times). A 4-value list: 13,855 us against 59.7 us (232 times). A 64-value list: 86.5 ms against 312 us (278 times). At 1,000 rows the same shapes are 4.1 to 4.9 times cheaper.

**What the numbers say about the engine.** A list as a residual tested each row against each value, so the walk's cost was rows times list length: about 10 ns a comparison, 4.17 s for 4,096 values over 100,000 rows. As a set the cost is the list itself plus one descent per value: 1.3 to 2.3 us per value on the tuple key and 3.5 to 5.4 us on an index from 64 values up. A hundred times the rows cost a set 0.97 to 1.32 times, where they cost the walk 73 to 186 times. What remains is the list's own text: a 4,096-value list costs 4.5 to 5.3 ms to parse, type and compile before a row is read (the walk over a 1-row relation on A; CU-S0 read 4.0 ms), and that is more than half of the 8.3 ms the tuple key set costs at 100,000 rows. **CU-Q3's loss is real and is as large as CU-S0 said**: a set is taken at every relation size, and at 1 and 10 rows it is slower than the walk from 64 values up on the tuple key and from 4 values up on an index (one cell of each inside its spread), and at 100 rows from 64 values up on an index. The largest loss is about 3.6 ms per statement (index, 100 rows, 4,096 values: 6,542 us as the walk, 10.1 ms as the set), against CU-S0's estimate of 3.7 ms. **The loss CU-S0 priced for a whole-list re-check is absent**: the four 1,000-row cells of 1,024 and 4,096 values, in which every row of the relation matches the list, were estimated to lose under CU-Q9 (a) and are 1.6 to 4.8 times faster as built, at 0.20 to 0.55 times that estimate. No arm runs (a), so this is consistent with CU-Q9 (c) and is not a measurement of it.

| field | value |
|---|---|
| date/time executed | 2026-11-17 03:58 to 04:28 UTC, and a supplement at 04:47. Both Release builds 03:58:21 to 04:04:12; a Debug build of the worktree (`./build`, for the test suite) ran beside them and linked at 04:05:30. Question 1 and the writes 04:06:02 to 04:13:36, the lists 04:13:45 to 04:21:52, the small relations 04:22:00 to 04:27:49. The test suite ran 04:27:58 to 04:43:22, after the last cell. The supplement 04:47:03 to 04:47:54. About 30 minutes of building and measuring; the budget is about an hour for a milestone's close |
| branch | `worktree-cu-key-set-access` (worktree `cu-key-set-access`) |
| arm A | `7aa75760`, `git describe --tags` = `v3.4.11-28-g7aa75760`: `origin/main` at the time of the run. **Not the commit CU opened from** (`1edf5c54`): `main` moved twice while CU was open, both moves are merged into the work branch, and `7aa75760` is the merge base of the closing commit and `main`, so the difference between the arms is CU's change and nothing else. Against `1edf5c54` the difference would also hold the unique index probe and the ceiling wait, which are not CU's |
| arm B | `70a307ea`, `git describe --tags` = `v3.4.11-40-g70a307ea`: the work branch with `origin/main` merged (CU-S0 to CU-S3b; the closing code) |
| tree | both arms built from `git archive` exports under `/home/louis/bench-runs/cu-close/src-A`, `src-B`, outside the worktree, so no uncommitted edit can be in either binary |
| binaries | Release (`-DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, GCC 14.2, target `kds_server`). Measured copies in `run/`: `kds_A` and `kds_A2` sha256 `b0679f91b74379c7ecb013b3b37c78eb267385373b691636358c0df9ea888dfc` (byte-identical), `kds_B` sha256 `3a11e8ef7b1ffc52fb91220bbb3a65504b4ed04cf25842d5fb8a97ec338e079c` (`binaries.sha256`) |
| device | data files and WAL under `/home/louis/bench-runs/cu-close/data`, ext4 on `/dev/nvme0n1p1` (`df -T`), not tmpfs; a fresh server and data file per run (question 1, supplement) or per relation size (lists) |
| host | Intel Xeon Platinum 8581C, 8 CPUs. **Not quiet until the supplement**: a PostgreSQL workload of another user of the host ran through the main run (one backend near a whole CPU at its start), and the 1-minute load average was 3.1 to 6.5 during question 1 (the high end is the builds' tail: 6.5 at its highest, 4.5 at the end of its fifth run), 3.2 to 5.2 during the lists and 1.0 to 4.9 during the small relations. The lists driver records the `cc1plus` count per arm run (0 throughout); question 1's driver records the load average alone, and what clears it of a build is the clock: the Debug build linked 32 s before its first cell and the test suite started 9 s after the small relations' last. The supplement ran at a load average of 0.2 to 0.7 with no competing process recorded in any cell. Every delta is read against an in-run control: A2 for question 1 and the supplement, L2 and the same-run walk LA for the lists |
| server config | question 1, the writes and the supplement: `cores` 1, `durability` relaxed, server on CPU 2, driver on CPU 4. Lists: `cores` 1, `durability` group, `join_order` written (read by arm J alone), both servers on CPU 4 and only one ever busy, driver on CPUs 0-3: CU-S0's configuration. `buffer_pool_frames` = 65536, defaults otherwise. **Ports**: question 1 ran on 31980 to 31982 and the lists on 15631 and 15632, the drivers' defaults, not passed on the command line, where `bench/README.md` asks for a chosen port; both ranges were free. The supplement passes 31990 |
| drivers | `tools/cu_close_overhead.py` (question 1 and the writes), `tools/cu_close_rows.py` (the supplement) and `tools/cu_close_lists.py` (the lists, derived from `tools/cu_s0_premise.py`); archived with the table scripts under `archive/cu-close-v3.4.11-40-g70a307ea/scripts/`. `bench/docs/` does not exist at this commit; each driver's docstring carries its flags, and the commands are listed under "Commands" |
| archive | `bench/v3.4.11/archive/cu-close-v3.4.11-40-g70a307ea/` (`q1.json`, `rows.json`, `lists.json`, `small.json`, the generated tables, the logs, the build logs, `binaries.sha256`, the scripts) |
| checks | every unit of question 1 and of the supplement is checked for its row count or its write tag (`bad` = 0 in every cell). Per list cell, the set's rows on B are compared row for row with the n single statements on B, with the walk's rows on A, and with arm J's rows (all 50 cells agree); B's plan is required to carry `key_set=` for every list of two values or more and A's is required not to |
| tests | the full suite passed on the merged tree at `70a307ea`, 4565 of 4565 (Debug, `ctest -LE bs-nightly`, the range the push hook runs), between the main run and the supplement. The measurement changes no engine code |

## Question 1 (CU-R5): no cost found off the set's shape

Three servers on one host, one connection each, six blocks per cell rotated in arm order, 10 runs with a fresh server per run. Every server holds `t1000` and `t100000` (`id int64, v int64, w int64, u int64`, row `i` = `(i, i, i, i)`, an index on `w`). The shapes, none of which carries a value list:

- `select-pk`: `SELECT * FROM t WHERE id = k`. The ladder is asked whether the step holds a list.
- `select-range`: `SELECT * FROM t WHERE id >= k AND id < k + 10`.
- `select-idx`: `SELECT * FROM t WHERE w = k`, an index probe.
- `select-scan`: `SELECT * FROM t WHERE u = k`, a walk of 1,000 rows.
- `select-join`: ten rows of `t1000` joined to `t100000` by its tuple key, ten inner probes.
- `update-pk`: `UPDATE t SET v = v + 1 WHERE id = k`. Since CU-S3b its key is read off the compiled `WHERE` (`exec::WriteKeyOf`), no longer off the parsed conditions.
- `update-scan`: `UPDATE t SET v = v + 1 WHERE u = k`, a walked write, which now asks both recognisers and is answered by neither.
- `ins-del`: an `INSERT` of one row under a fresh key and a `DELETE ... WHERE id = <it>`.

Server CPU is the sum of `/proc/<pid>/task/*/schedstat` on-CPU ns over a block, divided by its units. A delta is paired by run. **Reading**: a cell is "inside the floor" when B - A is within 2 standard errors of zero, or no larger than |A2 - A| plus 2 of its standard errors; A2 - A is what two servers on one binary differ by. The second half of that rule is lenient (on `update-scan` it would pass a delta of 8.5 us), and no cell below needs it: every one passes the first half.

@Q1_DELTAS@

Pooled over the two sizes:

@Q1_POOLED@

- All 13 cells have B - A within 2 standard errors of zero. It is positive in 6 cells and negative in 7.
- The largest positive cell is `select-pk` at 100,000 rows, +0.80 us (SE 0.77), with -0.11 us at 1,000 rows; a tuple key lookup does not depend on the relation's size, and pooled it is +0.35 us (SE 0.43).
- The control is as large as the effect: A2 - A reaches +1.54 us (`select-range`, 1,000 rows) and +4.46 us (`update-scan`), on servers that run one binary. Those two cells are 2.8 and 2.2 standard errors from zero, so the run-to-run error understates what two servers differ by, and "2 standard errors" below is not a 95 % bound (with 10 runs the factor would be 2.26 even if it were).
- The two shapes CU's write path touches without moving, `update-pk` and `update-scan`, read +0.22 us (SE 0.30) and -1.59 us (SE 1.47).
- **Resolution.** Two standard errors of the pooled delta are 0.56 to 2.9 us, 1.6 to 3.4 % of the statement. The pooled delta plus two of them, the largest cost this run does not exclude, is 0.2 to 4.8 % by shape: `select-pk` +4.8 %, `select-scan` +4.0 %, `update-pk` +3.2 %, `select-idx` +2.8 %, `select-range` +2.4 %, `select-join` and `update-scan` +1.2 %, `ins-del` +0.2 %. A cost of 4 % on a tuple key lookup is consistent with this run.
- **What these shapes reach of CU-R5.** CU-R5 as CU-S1 restated it (the work order's §7) names four tests: `key_set.has_value()` once per run of a lookup or probe step; one per accepted row of a step that has a boolean conjunct, with a null test per such conjunct; one per accepted row on a statement that records a trail; one per step where a trail is built. None of the eight shapes has a boolean conjunct or records a trail, so they reach the first alone. The supplement below runs the rest.

@Q1_HOST@

### The per-row tests (supplement)

`tools/cu_close_rows.py`, the same three arms, one relation `t1000`, 10 runs, on the host once it was quiet. Each statement reaches a test question 1's shapes do not:

- `scan-in2`: `SELECT * FROM t1000 WHERE u IN (k, k2)`. A list on a column nothing serves: a walk on every arm (`step 0 Scan`, `filter bool`), the boolean conjunct asked of each of 1,000 rows.
- `scan-or`: `SELECT * FROM t1000 WHERE u = k OR w = k2`. An `OR` over two columns, which is no list: the same walk.
- `update-in2`: `UPDATE t1000 SET v = v + 1 WHERE u IN (k, k2)`. The write path's boolean conjunct, a walked write on every arm.
- `pk-trail`: `SELECT * FROM t1000 WHERE id = k` after `WAYSTONE ON` for the shape (answered `waystone_recording=on waystone_replay=on` on each arm). Each of the 1,000 keys is asked three times a run, so a trail is recorded once and replayed twice; `ANALYZE` shows `replays=1` on each arm.

@ROWS_POOLED@

- No shape reads a cost: B - A is -1.81 to +0.02 us. `scan-in2` reads B 1.8 % cheaper at 2.1 standard errors, with the control at +0.48 us; one cell in four at that distance is not a finding, and it is not reported as a gain.
- The largest cost not excluded is +3.0 % (`update-in2`, whose run-to-run error is the widest), +1.7 % on the trail lookup and +0.2 % or less on the two walks, where the per-row test is multiplied by 1,000 rows a statement.

@ROWS_HOST@

### Against the engine's previous number

The last close to measure these shapes was CS's (`results-cs-close-v3.4.11-6-gadbea55b.md`), whose arm B at `adbea55b` (`v3.4.11-6-gadbea55b`) is 22 commits before this run's arm A by `git describe`'s counts.

| shape | rows | CS close, B at `adbea55b` | this run, A at `7aa75760` | delta |
|---|---|---|---|---|
| `select-pk` | 1,000 | 25.09 us | 24.72 us | -0.37 us, -1.5 % |
| `update-pk` | 1,000 | 27.02 us | 25.14 us | -1.88 us, -7.0 % |

The relations differ (three `int64` columns there, four and an index here), the hosts' loads differ, and the runs are two days apart, so the delta is a consistency check on the method and no finding about the 22 commits.

## The writes CU moved

The same three servers and blocks as question 1, in the same run. On A each of these visits every slot of every leaf of the relation under a write latch; on B each descends once per key (CU-S3, CU-S3b).

- `update-conj`: `UPDATE t SET v = v + 1 WHERE id = k AND u >= 0`.
- `update-in4`, `update-in64`: `UPDATE t SET v = v + 1 WHERE id IN (...)`, 4 and 64 distinct keys, every one present.
- `ins-del-in4`: an `INSERT` of four rows under fresh keys and a `DELETE ... WHERE id IN (<them>)`. The `INSERT` is in the unit on every arm, so the ratio understates the `DELETE`'s.

@Q1_MOVED@

- At 100,000 rows the walked write costs 8.8 to 86.5 ms a statement and the descents 43 to 312 us: 161 to 278 times.
- At 1,000 rows, where the relation is a handful of leaves, 3.3 to 4.9 times.
- A's cost grows with the list as the read walk's does (4 values 13.9 ms, 64 values 86.5 ms over 100,000 rows); B's is about 4 us per further key (59.7 us for 4, 312 us for 64).
- **B's 100,000-row means are the least firm numbers here.** Those cells hold 120 units per run and arm, and the block order is one of three rotations, so B's block follows A's in two blocks of three and runs on a CPU A's walks have just left. `update-conj` reads 42.8 us there and 28.0 us at 1,000 rows, where it runs 1,200 units per run. Both effects raise B, so the ratios are floors.
- `ins-del-in4` leaves its slots delete-marked, so `t1000` grows by up to 960 slots while its cell runs; A walks them, B does not.
- What these cells do not show is the lock picture: a walked write held every leaf in turn under a write latch, and concurrent sessions were not run.

Every cell of question 1 and the writes, with the client's latency. p25 to p99 and the client mean are means of the per-run values; p0 is the smallest unit of any run and max the largest.

@Q1_CELLS@

Every cell of the supplement:

@ROWS_CELLS@

## CU-S0's arms again: the list as a set, beside today's walk

`tools/cu_close_lists.py`, CU-S0's driver with one arm added. One relation `cu_t (id int64, k int64, a int64, b int64)` with `k = 3 * id + 1` and an index on `k`, loaded into a server of each binary. Per cell (rows, column, n), two repeats of 2 s per arm (1 s on the small relations), arm order rotated:

- **LA**: `SELECT * FROM cu_t WHERE <col> IN (v1, ..., vn)` sent to A. The walk with the list as a residual: what CU-S0 called L.
- **L**, **L2**: the same statement sent to B, twice. A key set; L2 is the noise control.
- **D**: the n values as n single-value statements on B. CU-S0's upper bound.
- **J**: the n values stored as rows and joined to `cu_t` on B, n descents in one statement. CU-S0's closest stand-in for a set.

A group's values are drawn afresh, distinct, from `[1, max(rows, n)]`, so a list longer than the relation carries misses. A one-value list is collapsed to an equality by the parser and is a single descent on both binaries. In the tables the ratios against the set use the mean of L and L2.

Server CPU per list, 1,000 and 100,000 rows:

@LISTS_CPU@

- **Every list of two values or more is faster as a set at 1,000 rows and above**: 4.2 to 6.9 times on the tuple key and 1.6 to 3.3 times on the index at 1,000 rows, 290 to 685 times and 226 to 310 times at 100,000 rows.
- **The set costs what n descents in one statement cost** (L / J 0.94 to 1.47 up to 1,024 values), and more than J at 4,096 values on the tuple key (1.87 and 2.49 times), where the list's text is what the statement spends: J's statement carries no list.
- **n statements are 2.2 to 18 times dearer than one set** (D / L), as CU-S0 said they would be: each pays a parse, a reply and a round trip.
- **The walk's long-list cells at 100,000 rows rest on few statements**: a 1,024-value walk takes over a second, so LA holds 4 groups there and 2 at 4,096 values. The ratios at those lengths (273 to 685 times) are of that firmness; the set's side of each holds hundreds of groups.

Per value:

@LISTS_PER@

@LISTS_HOST@

### Against CU-S0's own numbers

CU-S0 measured at `1edf5c54` (`v3.4.11-20-g1edf5c54`,
`results-cu-s0-premise-v3.4.11-20-g1edf5c54.md`), 8 commits before arm A and 20 before arm B by `git describe`'s counts, on the same host on 2026-11-16. Its estimate of a set is recomputed from its own archive as its `estimate.py` did (the list alone plus the descents); the same-run estimate is that formula over this run's arms (the list alone is the walk on A over the 1-row relation). "Walk at CU-S0" and "set measured" are the means of the two L arms of each run.

@S0CMP@

- **The walk reads 0.89 to 1.36 times its CU-S0 number**, on a binary whose walk CU did not touch (arm A). The tuple key cells of the table are within 10 %. The index cells read 1.19 and 1.27 times at 1,000 rows from 1,024 values and 1.26 to 1.36 times at 100,000 rows, and D and J at 100,000 rows read 1.13 to 1.40 times theirs.
- **That drift is not explained.** The host was busier than at CU-S0, but the tuple key walk and the index-column walk are the same scan of the same pages and only the second drifted, under a load average that was no higher in its cells. The comparison that holds is the same-run one, LA against L.
- **The measured set is 0.81 to 1.51 times CU-S0's estimate, and 0.81 to 1.26 times the same-run estimate.** Against CU-S0's the three cells above 1.3 are at 100,000 rows: two index cells (1.51, 1.41), where this run's walk reads 1.36 and 1.26 times its CU-S0 number, and the 64-value tuple key cell (1.32), where it reads 1.07. Against the same-run estimate those three read 1.26, 1.04 and 1.08. The estimate left out the re-check and the set's own build; neither shows as a term the estimate missed.
- CU-S0 did not measure the 1,024-value cells at 10 and 100 rows; they are in the small table below.

### The small relations: CU-Q3's loss, measured

@SMALL_CPU@

- **Tuple key.** At 1 and 10 rows a set is slower than the walk from 64 values up: by 16 to 20 us at 64 values (inside the spread at 10 rows), 0.2 to 0.3 ms at 1,024, and 0.9 to 1.1 ms at 4,096. At 100 rows it is faster at every length (1.12 to 1.38 times).
- **Index.** At 1 and 10 rows a set is slower at every length from 4 values (the 64-value cell at 1 row is inside its spread): 3 to 8 us at 4 values, 0.4 ms at 1,024, 1.5 to 1.7 ms at 4,096. At 100 rows it is slower from 64 values up: 34 us, 0.9 ms and about 3.6 ms.
- **The largest loss is about 3.6 ms per statement** (index, 100 rows, 4,096 values), where CU-S0 estimated 3.7 ms. The crossover is between 10 and 100 rows on the tuple key and between 100 and 1,000 on an index, as CU-S0 placed it.
- A 1-row relation shows what the set adds to the list alone: 4,096 values cost 5,257 us on A and 6,371 us on B, about 0.27 us a value for building the set and descending for values that are not there.

@SMALL_HOST@

### CU-Q8's bound on the built path

A statement driven by a set runs inline and does not yield. The largest set the grammar admits, 4,096 values, holds its core for the whole statement: 8.3 ms of server CPU on the tuple key over 100,000 rows (6.4 ms over 1,000), and 20.0 ms on the index (20.6 ms over 1,000 rows). Between 4.5 and 5.3 ms of each is the list's parse, type and compile, which the walk also spent before its first slice. CU-S0 derived 1.6 to 4.2 ms of descents on the tuple key and 2.0 to 12.7 ms on an index on top of about 4 ms for the list. On the tuple key the measured statement is at that sum (8.3 ms against 8.2 ms at 100,000 rows); on the index it is above it (20.0 ms against 15.7 ms at 100,000 rows, 20.6 ms against 16.7 ms at 1,000), in cells where this run's other arms also read above CU-S0's. The join bound (a set on a step whose later steps walk) was not measured.

## Client latency per list

p25 to p99 are means of the two repeats' values; p0 is the smallest group of either repeat and max the largest.

1,000 and 100,000 rows:

@LISTS_LAT@

1, 10 and 100 rows:

@SMALL_LAT@

## Plans

`ANALYZE`'s reply at 100,000 rows, 4 values.

@LISTS_PLANS@

## Wait breakdown

Not sampled. Every cell is one connection against one core with no other session, so a statement takes no lock, latch or I/O wait, and `ANALYZE`'s `DB time` line for each list statement reads CPU alone (above). The servers ran `relaxed` (question 1, supplement) and `group` (lists, read-only), so no cell waits on a sync. What was not recorded is the run-queue wait behind the host's other load during the main run; server CPU, the figure every table reads, does not include it, and client latency does.

## What was cut, and what this run does not show

- **No Cabin set.** No relation carries a Cabin, as at CU-S0; the Cabin set (CU-S2) is covered by its tests and has no number.
- **One core, one connection.** No cell runs `cores` above 1 or a second session, so nothing here measures what a walked write's leaf holds cost its neighbours, or a set under contention.
- **No set on a join's inner step**, and so no measurement of CU-Q8's join bound.
- **`DELETE` alone** is not a cell: it is measured inside `ins-del` and `ins-del-in4`, whose units carry an `INSERT`.
- **The write cells at 100,000 rows are 120 units per run**, to keep arm A's walks inside the budget.
- **No PostgreSQL floor**, as at CU-S0: the comparison is this engine before and after.
- **Two repeats per list arm**, CU-S0's count, with L2 and LA as the controls.

What the drivers do that a reader would not assume. Each is as it ran; none is corrected after the fact.

- **Question 1's single-key shapes cycle over 125 keys of `t1000` and 3,125 of `t100000`** (the key step shares a factor of 8 with both sizes), so `update-pk` at 1,000 rows rewrites each of 125 rows 24 times a run. Every arm does the same.
- **Block order is one of three rotations**, so B's block follows A's in two blocks of three (above, under the writes).
- **In the lists, L2 and LA draw the same values and so do L and D**; L and LA are compared on different draws of the same distribution. L is the first arm of both repeats, as at CU-S0.
- `cu_close_overhead.py` records no driver CPU (`driver_cpu_us` is 0 in every row of `q1.json`), and `cu_close_lists.py` carries CU-S0's docstring below its own, which still calls L "today a walk".

## Commands

```
# both arms, from exports outside the worktree
git archive --format=tar -o src-A.tar 7aa75760   # and 70a307ea for B; extracted to src-A, src-B
cmake -S src-A -B build-A -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF
cmake --build build-A --target kds_server -j4    # the same for B

# question 1 and the writes (ports 31980..31982, the driver's default)
python3 scripts/cu_close_overhead.py --arm A=./kds_A --arm B=./kds_B --arm A2=./kds_A2 \
    --workdir /home/louis/bench-runs/cu-close/data --out out/q1.json --progress out/q1.progress --runs 10

# the supplement
python3 scripts/cu_close_rows.py --arm A=./kds_A --arm B=./kds_B --arm A2=./kds_A2 \
    --workdir /home/louis/bench-runs/cu-close/data --port 31990 --runs 10 \
    --out out/rows.json --progress out/rows.progress

# CU-S0's arms again (ports 15631, 15632, the driver's default)
python3 scripts/cu_close_lists.py --bin ./kds_B --bin-a ./kds_A --workdir /home/louis/bench-runs/cu-close/data \
    --out out/lists.json --heartbeat out/lists.progress --status out/lists.status \
    --rows 1000,100000 --n 1,4,64,1024,4096 --reps 2 --seconds 2.0
python3 scripts/cu_close_lists.py --bin ./kds_B --bin-a ./kds_A --workdir /home/louis/bench-runs/cu-close/data \
    --out out/small.json --heartbeat out/small.progress --status out/small.status \
    --rows 1,10,100 --n 1,4,64,1024,4096 --reps 2 --seconds 1.0

# tables, then this file
python3 q1_tables.py out/q1.json > out/q1.md; python3 q1_tables.py out/rows.json > out/rows.md
python3 lists_tables.py out/lists.json > out/lists.md; python3 lists_tables.py out/small.json > out/small.md
python3 s0cmp.py <CU-S0 archive>/result.json result-small.json result-one.json out/lists.json out/small.json > out/s0cmp.md
python3 assemble.py head.md out/ results-cu-close-v3.4.11-40-g70a307ea.md
```

Each run was watched every 60 s through the file its driver writes (`q1.progress`, `rows.progress`, `lists.progress`, `small.progress`); none stalled.
