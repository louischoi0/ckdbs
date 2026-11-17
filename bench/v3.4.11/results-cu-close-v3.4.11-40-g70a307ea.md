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

| shape | rows | units per run | A | B - A (SE) | A2 - A (SE) | B - A % | reading |
|---|---|---|---|---|---|---|---|
| select-pk | 1,000 | 3,000 | 24.72 us | -0.11 us (0.36) | -0.19 us (0.49) | -0.4 % | inside the floor |
| select-pk | 100,000 | 3,000 | 25.32 us | +0.80 us (0.77) | -0.31 us (0.46) | +3.2 % | inside the floor |
| select-range | 1,000 | 3,000 | 34.54 us | +0.59 us (0.35) | +1.54 us (0.56) | +1.7 % | inside the floor |
| select-range | 100,000 | 3,000 | 35.63 us | -0.04 us (0.42) | +0.17 us (0.44) | -0.1 % | inside the floor |
| select-idx | 1,000 | 3,000 | 27.38 us | +0.44 us (0.42) | +0.04 us (0.54) | +1.6 % | inside the floor |
| select-idx | 100,000 | 3,000 | 28.60 us | -0.19 us (0.53) | -0.54 us (0.45) | -0.7 % | inside the floor |
| select-scan | 1,000 | 1,200 | 48.27 us | +0.77 us (0.58) | +0.76 us (0.66) | +1.6 % | inside the floor |
| select-join | 1,000 | 1,200 | 50.33 us | -0.36 us (0.48) | -0.42 us (1.14) | -0.7 % | inside the floor |
| update-pk | 1,000 | 3,000 | 25.14 us | +0.06 us (0.23) | +0.06 us (0.27) | +0.2 % | inside the floor |
| update-pk | 100,000 | 3,000 | 25.74 us | +0.38 us (0.57) | +0.24 us (0.49) | +1.5 % | inside the floor |
| update-scan | 1,000 | 1,200 | 108.61 us | -1.59 us (1.47) | +4.46 us (2.04) | -1.5 % | inside the floor |
| ins-del | 1,000 | 1,500 | 49.87 us | -0.36 us (0.39) | +0.11 us (0.60) | -0.7 % | inside the floor |
| ins-del | 100,000 | 1,500 | 51.21 us | -1.23 us (0.82) | -1.10 us (0.62) | -2.4 % | inside the floor |

Pooled over the two sizes:

| shape | paired run-cells | A | B - A (SE) | A2 - A (SE) | B - A % | 2 SE % | largest cost not excluded, B - A + 2 SE |
|---|---|---|---|---|---|---|---|
| select-pk | 20 | 25.02 us | +0.35 us (0.43) | -0.25 us (0.33) | +1.4 % | 3.4 % | +1.20 us, +4.8 % |
| select-range | 20 | 35.08 us | +0.27 us (0.28) | +0.86 us (0.38) | +0.8 % | 1.6 % | +0.83 us, +2.4 % |
| select-idx | 20 | 27.99 us | +0.12 us (0.34) | -0.25 us (0.35) | +0.4 % | 2.4 % | +0.80 us, +2.8 % |
| select-scan | 10 | 48.27 us | +0.77 us (0.58) | +0.76 us (0.66) | +1.6 % | 2.4 % | +1.93 us, +4.0 % |
| select-join | 10 | 50.33 us | -0.36 us (0.48) | -0.42 us (1.14) | -0.7 % | 1.9 % | +0.61 us, +1.2 % |
| update-pk | 20 | 25.44 us | +0.22 us (0.30) | +0.15 us (0.27) | +0.9 % | 2.4 % | +0.82 us, +3.2 % |
| update-scan | 10 | 108.61 us | -1.59 us (1.47) | +4.46 us (2.04) | -1.5 % | 2.7 % | +1.35 us, +1.2 % |
| ins-del | 20 | 50.54 us | -0.79 us (0.45) | -0.49 us (0.44) | -1.6 % | 1.8 % | +0.11 us, +0.2 % |

- All 13 cells have B - A within 2 standard errors of zero. It is positive in 6 cells and negative in 7.
- The largest positive cell is `select-pk` at 100,000 rows, +0.80 us (SE 0.77), with -0.11 us at 1,000 rows; a tuple key lookup does not depend on the relation's size, and pooled it is +0.35 us (SE 0.43).
- The control is as large as the effect: A2 - A reaches +1.54 us (`select-range`, 1,000 rows) and +4.46 us (`update-scan`), on servers that run one binary. Those two cells are 2.8 and 2.2 standard errors from zero, so the run-to-run error understates what two servers differ by, and "2 standard errors" below is not a 95 % bound (with 10 runs the factor would be 2.26 even if it were).
- The two shapes CU's write path touches without moving, `update-pk` and `update-scan`, read +0.22 us (SE 0.30) and -1.59 us (SE 1.47).
- **Resolution.** Two standard errors of the pooled delta are 0.56 to 2.9 us, 1.6 to 3.4 % of the statement. The pooled delta plus two of them, the largest cost this run does not exclude, is 0.2 to 4.8 % by shape: `select-pk` +4.8 %, `select-scan` +4.0 %, `update-pk` +3.2 %, `select-idx` +2.8 %, `select-range` +2.4 %, `select-join` and `update-scan` +1.2 %, `ins-del` +0.2 %. A cost of 4 % on a tuple key lookup is consistent with this run.
- **What these shapes reach of CU-R5.** CU-R5 as CU-S1 restated it (the work order's §7) names four tests: `key_set.has_value()` once per run of a lookup or probe step; one per accepted row of a step that has a boolean conjunct, with a null test per such conjunct; one per accepted row on a statement that records a trail; one per step where a trail is built. None of the eight shapes has a boolean conjunct or records a trail, so they reach the first alone. The supplement below runs the rest.

runs: 10; loads per cell: min 3.11 max 6.54; bad units: 0

### The per-row tests (supplement)

`tools/cu_close_rows.py`, the same three arms, one relation `t1000`, 10 runs, on the host once it was quiet. Each statement reaches a test question 1's shapes do not:

- `scan-in2`: `SELECT * FROM t1000 WHERE u IN (k, k2)`. A list on a column nothing serves: a walk on every arm (`step 0 Scan`, `filter bool`), the boolean conjunct asked of each of 1,000 rows.
- `scan-or`: `SELECT * FROM t1000 WHERE u = k OR w = k2`. An `OR` over two columns, which is no list: the same walk.
- `update-in2`: `UPDATE t1000 SET v = v + 1 WHERE u IN (k, k2)`. The write path's boolean conjunct, a walked write on every arm.
- `pk-trail`: `SELECT * FROM t1000 WHERE id = k` after `WAYSTONE ON` for the shape (answered `waystone_recording=on waystone_replay=on` on each arm). Each of the 1,000 keys is asked three times a run, so a trail is recorded once and replayed twice; `ANALYZE` shows `replays=1` on each arm.

| shape | paired run-cells | A | B - A (SE) | A2 - A (SE) | B - A % | 2 SE % | largest cost not excluded, B - A + 2 SE |
|---|---|---|---|---|---|---|---|
| scan-in2 | 10 | 102.14 us | -1.81 us (0.88) | +0.48 us (0.40) | -1.8 % | 1.7 % | -0.05 us, -0.0 % |
| scan-or | 10 | 107.68 us | -0.87 us (0.54) | +0.26 us (0.50) | -0.8 % | 1.0 % | +0.22 us, +0.2 % |
| pk-trail | 10 | 22.65 us | +0.02 us (0.19) | +0.25 us (0.15) | +0.1 % | 1.6 % | +0.40 us, +1.7 % |
| update-in2 | 10 | 134.74 us | -0.02 us (2.05) | -0.81 us (1.00) | -0.0 % | 3.0 % | +4.09 us, +3.0 % |

- No shape reads a cost: B - A is -1.81 to +0.02 us. `scan-in2` reads B 1.8 % cheaper at 2.1 standard errors, with the control at +0.48 us; one cell in four at that distance is not a finding, and it is not reported as a gain.
- The largest cost not excluded is +3.0 % (`update-in2`, whose run-to-run error is the widest), +1.7 % on the trail lookup and +0.2 % or less on the two walks, where the per-row test is multiplied by 1,000 rows a statement.

runs: 10; loads per cell: min 0.23 max 0.7; bad units: 0

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

| shape | rows | units per run | A | A2 | B | A / B | B - A (SE) |
|---|---|---|---|---|---|---|---|
| update-conj | 1,000 | 1,200 | 115.8 us | 117.8 us | 28.0 us | 4.1 times | -87.8 us (3.0) |
| update-conj | 100,000 | 120 | 8,755.4 us | 8,781.5 us | 42.8 us | 204.7 times | -8,712.6 us (481.2) |
| update-in4 | 1,000 | 1,200 | 190.3 us | 185.2 us | 39.2 us | 4.9 times | -151.1 us (8.3) |
| update-in4 | 100,000 | 120 | 13,854.9 us | 13,825.5 us | 59.7 us | 232.0 times | -13,795.2 us (673.6) |
| update-in64 | 1,000 | 600 | 1,089.8 us | 1,083.2 us | 230.0 us | 4.7 times | -859.8 us (59.5) |
| update-in64 | 100,000 | 120 | 86,457.8 us | 87,273.5 us | 311.5 us | 277.6 times | -86,146.3 us (2,493.7) |
| ins-del-in4 | 1,000 | 240 | 232.6 us | 234.4 us | 70.9 us | 3.3 times | -161.8 us (10.5) |
| ins-del-in4 | 100,000 | 120 | 14,339.5 us | 14,416.1 us | 88.9 us | 161.3 times | -14,250.6 us (903.9) |

- At 100,000 rows the walked write costs 8.8 to 86.5 ms a statement and the descents 43 to 312 us: 161 to 278 times.
- At 1,000 rows, where the relation is a handful of leaves, 3.3 to 4.9 times.
- A's cost grows with the list as the read walk's does (4 values 13.9 ms, 64 values 86.5 ms over 100,000 rows); B's is about 4 us per further key (59.7 us for 4, 312 us for 64).
- **B's 100,000-row means are the least firm numbers here.** Those cells hold 120 units per run and arm, and the block order is one of three rotations, so B's block follows A's in two blocks of three and runs on a CPU A's walks have just left. `update-conj` reads 42.8 us there and 28.0 us at 1,000 rows, where it runs 1,200 units per run. Both effects raise B, so the ratios are floors.
- `ins-del-in4` leaves its slots delete-marked, so `t1000` grows by up to 960 slots while its cell runs; A walks them, B does not.
- What these cells do not show is the lock picture: a walked write held every leaf in turn under a write latch, and concurrent sessions were not run.

Every cell of question 1 and the writes, with the client's latency. p25 to p99 and the client mean are means of the per-run values; p0 is the smallest unit of any run and max the largest.

| shape | rows | arm | units/run | runs | server CPU per unit (mean of run means) | min..max over runs | client mean | p0 | p25 | p50 | p75 | p90 | p99 | max | bad |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| select-pk | 1,000 | A | 3,000 | 10 | 24.72 us | 21.45..30.01 us | 64.7 us | 46.9 us | 53.9 us | 58.1 us | 66.8 us | 75.4 us | 139.5 us | 5,595.3 us | 0 |
| select-pk | 1,000 | B | 3,000 | 10 | 24.61 us | 22.37..29.02 us | 64.7 us | 46.6 us | 53.7 us | 58.4 us | 67.2 us | 76.3 us | 138.5 us | 4,378.0 us | 0 |
| select-pk | 1,000 | A2 | 3,000 | 10 | 24.53 us | 21.75..31.52 us | 64.4 us | 47.6 us | 53.2 us | 58.2 us | 66.9 us | 74.9 us | 131.1 us | 6,475.8 us | 0 |
| select-pk | 100,000 | A | 3,000 | 10 | 25.32 us | 22.39..30.73 us | 64.2 us | 48.0 us | 54.0 us | 58.2 us | 64.9 us | 73.9 us | 132.1 us | 5,708.9 us | 0 |
| select-pk | 100,000 | B | 3,000 | 10 | 26.12 us | 22.21..34.36 us | 64.0 us | 46.7 us | 53.3 us | 56.6 us | 64.3 us | 73.3 us | 147.9 us | 6,601.8 us | 0 |
| select-pk | 100,000 | A2 | 3,000 | 10 | 25.00 us | 21.85..30.71 us | 62.7 us | 47.9 us | 53.4 us | 57.0 us | 63.0 us | 71.6 us | 132.8 us | 6,135.3 us | 0 |
| select-range | 1,000 | A | 3,000 | 10 | 34.54 us | 30.17..44.08 us | 82.4 us | 62.2 us | 69.5 us | 73.9 us | 86.0 us | 101.5 us | 167.1 us | 4,012.0 us | 0 |
| select-range | 1,000 | B | 3,000 | 10 | 35.13 us | 31.03..43.69 us | 83.4 us | 62.2 us | 69.8 us | 75.1 us | 86.3 us | 100.7 us | 175.0 us | 6,005.2 us | 0 |
| select-range | 1,000 | A2 | 3,000 | 10 | 36.08 us | 30.84..44.48 us | 85.3 us | 62.6 us | 69.7 us | 74.7 us | 87.1 us | 102.3 us | 209.8 us | 4,646.8 us | 0 |
| select-range | 100,000 | A | 3,000 | 10 | 35.63 us | 31.52..46.31 us | 83.4 us | 63.1 us | 71.7 us | 75.4 us | 85.1 us | 100.2 us | 173.9 us | 3,669.1 us | 0 |
| select-range | 100,000 | B | 3,000 | 10 | 35.58 us | 31.43..46.52 us | 83.9 us | 62.6 us | 71.8 us | 76.0 us | 85.5 us | 100.4 us | 174.9 us | 5,732.8 us | 0 |
| select-range | 100,000 | A2 | 3,000 | 10 | 35.80 us | 31.38..46.70 us | 84.9 us | 62.7 us | 72.0 us | 76.7 us | 88.2 us | 102.2 us | 174.5 us | 5,744.1 us | 0 |
| select-idx | 1,000 | A | 3,000 | 10 | 27.38 us | 24.47..34.19 us | 63.8 us | 49.2 us | 55.2 us | 58.7 us | 66.1 us | 74.5 us | 116.2 us | 3,060.2 us | 0 |
| select-idx | 1,000 | B | 3,000 | 10 | 27.82 us | 24.13..34.21 us | 64.8 us | 49.3 us | 55.6 us | 60.5 us | 67.5 us | 75.8 us | 120.3 us | 2,670.3 us | 0 |
| select-idx | 1,000 | A2 | 3,000 | 10 | 27.42 us | 23.75..34.23 us | 64.4 us | 49.4 us | 55.4 us | 59.5 us | 66.6 us | 75.1 us | 117.2 us | 6,663.3 us | 0 |
| select-idx | 100,000 | A | 3,000 | 10 | 28.60 us | 24.97..35.94 us | 65.7 us | 50.3 us | 56.3 us | 60.4 us | 68.9 us | 78.0 us | 134.2 us | 2,157.3 us | 0 |
| select-idx | 100,000 | B | 3,000 | 10 | 28.40 us | 25.28..36.51 us | 64.9 us | 50.3 us | 56.1 us | 59.9 us | 67.8 us | 76.9 us | 120.8 us | 4,205.8 us | 0 |
| select-idx | 100,000 | A2 | 3,000 | 10 | 28.06 us | 25.38..35.74 us | 64.4 us | 49.3 us | 55.9 us | 59.6 us | 66.9 us | 76.2 us | 117.4 us | 1,197.0 us | 0 |
| select-scan | 1,000 | A | 1,200 | 10 | 48.27 us | 42.88..64.21 us | 86.6 us | 67.3 us | 73.7 us | 78.9 us | 90.1 us | 106.3 us | 178.8 us | 1,199.4 us | 0 |
| select-scan | 1,000 | B | 1,200 | 10 | 49.04 us | 43.04..68.65 us | 88.4 us | 67.7 us | 74.7 us | 79.6 us | 90.5 us | 106.5 us | 181.4 us | 3,987.0 us | 0 |
| select-scan | 1,000 | A2 | 1,200 | 10 | 49.04 us | 43.14..66.99 us | 87.2 us | 67.7 us | 74.4 us | 80.0 us | 91.1 us | 106.5 us | 173.5 us | 3,271.9 us | 0 |
| select-join | 1,000 | A | 1,200 | 10 | 50.33 us | 45.17..63.84 us | 92.8 us | 67.0 us | 75.2 us | 81.5 us | 96.5 us | 114.0 us | 245.7 us | 4,301.3 us | 0 |
| select-join | 1,000 | B | 1,200 | 10 | 49.97 us | 45.07..62.35 us | 90.1 us | 67.1 us | 75.1 us | 80.7 us | 93.7 us | 110.3 us | 198.8 us | 4,110.6 us | 0 |
| select-join | 1,000 | A2 | 1,200 | 10 | 49.91 us | 45.40..63.20 us | 89.5 us | 67.8 us | 75.2 us | 81.8 us | 91.6 us | 106.2 us | 201.2 us | 5,054.9 us | 0 |
| update-pk | 1,000 | A | 3,000 | 10 | 25.14 us | 22.62..30.71 us | 50.9 us | 36.3 us | 43.4 us | 45.7 us | 50.9 us | 57.1 us | 93.4 us | 4,063.7 us | 0 |
| update-pk | 1,000 | B | 3,000 | 10 | 25.20 us | 22.99..31.51 us | 51.1 us | 37.0 us | 43.3 us | 45.5 us | 51.0 us | 57.7 us | 108.2 us | 3,856.5 us | 0 |
| update-pk | 1,000 | A2 | 3,000 | 10 | 25.20 us | 23.21..30.28 us | 51.0 us | 37.7 us | 43.2 us | 45.6 us | 51.1 us | 57.9 us | 97.2 us | 3,521.3 us | 0 |
| update-pk | 100,000 | A | 3,000 | 10 | 25.74 us | 23.62..31.46 us | 51.1 us | 38.2 us | 44.1 us | 46.9 us | 53.1 us | 59.6 us | 94.3 us | 4,511.8 us | 0 |
| update-pk | 100,000 | B | 3,000 | 10 | 26.12 us | 23.70..32.05 us | 52.3 us | 37.5 us | 43.5 us | 47.6 us | 53.9 us | 61.1 us | 104.2 us | 5,193.8 us | 0 |
| update-pk | 100,000 | A2 | 3,000 | 10 | 25.98 us | 23.71..32.59 us | 52.3 us | 38.5 us | 44.1 us | 47.7 us | 53.8 us | 61.8 us | 104.1 us | 2,956.1 us | 0 |
| update-scan | 1,000 | A | 1,200 | 10 | 108.61 us | 102.84..130.77 us | 142.2 us | 107.0 us | 115.2 us | 125.3 us | 148.6 us | 186.2 us | 279.9 us | 6,501.4 us | 0 |
| update-scan | 1,000 | B | 1,200 | 10 | 107.02 us | 99.12..134.17 us | 139.9 us | 104.7 us | 113.7 us | 124.5 us | 147.3 us | 180.4 us | 294.2 us | 5,238.6 us | 0 |
| update-scan | 1,000 | A2 | 1,200 | 10 | 113.07 us | 105.14..151.37 us | 151.7 us | 106.6 us | 118.0 us | 131.6 us | 159.7 us | 201.5 us | 333.1 us | 3,943.2 us | 0 |
| ins-del | 1,000 | A | 1,500 | 10 | 49.87 us | 43.60..59.33 us | 103.7 us | 75.4 us | 87.8 us | 95.5 us | 106.7 us | 120.2 us | 239.7 us | 4,785.1 us | 0 |
| ins-del | 1,000 | B | 1,500 | 10 | 49.51 us | 43.38..59.96 us | 103.0 us | 76.0 us | 87.8 us | 95.8 us | 106.3 us | 119.4 us | 220.1 us | 5,516.4 us | 0 |
| ins-del | 1,000 | A2 | 1,500 | 10 | 49.98 us | 43.06..59.44 us | 103.9 us | 76.3 us | 85.8 us | 93.7 us | 105.2 us | 119.1 us | 275.7 us | 5,928.6 us | 0 |
| ins-del | 100,000 | A | 1,500 | 10 | 51.21 us | 43.85..61.60 us | 101.9 us | 76.7 us | 87.6 us | 94.7 us | 105.6 us | 119.5 us | 210.8 us | 2,249.3 us | 0 |
| ins-del | 100,000 | B | 1,500 | 10 | 49.98 us | 44.12..64.35 us | 101.4 us | 76.5 us | 86.3 us | 92.3 us | 104.3 us | 117.8 us | 215.2 us | 3,187.8 us | 0 |
| ins-del | 100,000 | A2 | 1,500 | 10 | 50.11 us | 42.92..61.83 us | 100.8 us | 76.3 us | 85.2 us | 92.7 us | 104.4 us | 117.9 us | 201.2 us | 5,808.1 us | 0 |
| update-conj | 1,000 | A | 1,200 | 10 | 115.83 us | 107.17..142.52 us | 150.5 us | 113.7 us | 123.8 us | 134.1 us | 159.1 us | 194.5 us | 293.9 us | 3,864.6 us | 0 |
| update-conj | 1,000 | B | 1,200 | 10 | 28.03 us | 25.93..32.19 us | 56.1 us | 38.6 us | 45.6 us | 49.9 us | 56.3 us | 65.2 us | 154.0 us | 3,805.6 us | 0 |
| update-conj | 1,000 | A2 | 1,200 | 10 | 117.76 us | 109.38..130.34 us | 154.7 us | 113.3 us | 124.5 us | 134.6 us | 164.5 us | 205.2 us | 322.6 us | 6,763.2 us | 0 |
| update-conj | 100,000 | A | 120 | 10 | 8,755.42 us | 7,784.64..12,696.56 us | 9,005.5 us | 7,308.8 us | 8,331.0 us | 8,665.0 us | 9,486.9 us | 10,485.4 us | 13,086.6 us | 18,382.0 us | 0 |
| update-conj | 100,000 | B | 120 | 10 | 42.78 us | 39.59..52.25 us | 78.0 us | 43.5 us | 51.1 us | 58.0 us | 70.2 us | 117.8 us | 302.0 us | 875.3 us | 0 |
| update-conj | 100,000 | A2 | 120 | 10 | 8,781.50 us | 7,785.55..13,330.10 us | 9,023.5 us | 7,327.0 us | 8,339.0 us | 8,647.7 us | 9,244.8 us | 10,592.1 us | 12,885.2 us | 17,050.4 us | 0 |
| update-in4 | 1,000 | A | 1,200 | 10 | 190.30 us | 162.60..260.55 us | 245.5 us | 159.9 us | 192.5 us | 219.2 us | 272.7 us | 334.0 us | 470.7 us | 5,464.9 us | 0 |
| update-in4 | 1,000 | B | 1,200 | 10 | 39.17 us | 34.35..47.83 us | 70.0 us | 49.9 us | 58.0 us | 63.8 us | 71.6 us | 83.8 us | 179.7 us | 1,164.6 us | 0 |
| update-in4 | 1,000 | A2 | 1,200 | 10 | 185.22 us | 159.16..242.40 us | 239.9 us | 158.3 us | 185.1 us | 215.7 us | 263.2 us | 320.9 us | 499.3 us | 4,577.4 us | 0 |
| update-in4 | 100,000 | A | 120 | 10 | 13,854.94 us | 12,233.64..17,959.17 us | 14,293.1 us | 11,534.0 us | 12,243.2 us | 14,264.9 us | 15,478.5 us | 16,818.2 us | 21,865.7 us | 34,765.3 us | 0 |
| update-in4 | 100,000 | B | 120 | 10 | 59.73 us | 54.71..71.72 us | 110.4 us | 54.2 us | 67.0 us | 75.3 us | 98.7 us | 176.3 us | 454.9 us | 3,059.3 us | 0 |
| update-in4 | 100,000 | A2 | 120 | 10 | 13,825.49 us | 12,082.09..18,668.78 us | 14,256.4 us | 11,489.2 us | 12,648.3 us | 13,770.9 us | 15,365.8 us | 16,309.7 us | 21,631.0 us | 35,343.6 us | 0 |
| update-in64 | 1,000 | A | 600 | 10 | 1,089.80 us | 942.73..1,513.32 us | 1,218.6 us | 888.4 us | 1,101.7 us | 1,141.4 us | 1,224.2 us | 1,459.2 us | 1,968.3 us | 6,223.8 us | 0 |
| update-in64 | 1,000 | B | 600 | 10 | 230.03 us | 194.39..293.01 us | 306.3 us | 194.5 us | 243.4 us | 282.0 us | 337.2 us | 404.1 us | 595.8 us | 4,215.6 us | 0 |
| update-in64 | 1,000 | A2 | 600 | 10 | 1,083.22 us | 953.09..1,515.19 us | 1,211.3 us | 882.1 us | 1,104.2 us | 1,142.8 us | 1,223.9 us | 1,412.3 us | 1,984.1 us | 6,918.6 us | 0 |
| update-in64 | 100,000 | A | 120 | 10 | 86,457.76 us | 80,177.35..103,890.87 us | 87,715.8 us | 73,837.1 us | 80,416.3 us | 84,120.5 us | 90,690.1 us | 102,311.8 us | 129,473.2 us | 144,996.6 us | 0 |
| update-in64 | 100,000 | B | 120 | 10 | 311.46 us | 265.13..341.98 us | 470.7 us | 237.9 us | 317.6 us | 383.6 us | 503.4 us | 684.2 us | 1,620.1 us | 4,111.2 us | 0 |
| update-in64 | 100,000 | A2 | 120 | 10 | 87,273.50 us | 79,866.65..99,872.64 us | 88,545.6 us | 73,985.1 us | 79,973.4 us | 83,909.8 us | 92,774.9 us | 105,497.9 us | 129,016.7 us | 144,097.3 us | 0 |
| ins-del-in4 | 1,000 | A | 240 | 10 | 232.65 us | 194.74..317.19 us | 314.5 us | 210.4 us | 255.2 us | 289.3 us | 341.5 us | 406.7 us | 643.9 us | 3,728.5 us | 0 |
| ins-del-in4 | 1,000 | B | 240 | 10 | 70.88 us | 66.85..78.70 us | 130.1 us | 96.6 us | 106.0 us | 117.8 us | 134.4 us | 156.5 us | 325.6 us | 1,973.7 us | 0 |
| ins-del-in4 | 1,000 | A2 | 240 | 10 | 234.36 us | 208.44..304.46 us | 323.7 us | 212.9 us | 255.7 us | 296.3 us | 349.5 us | 433.4 us | 701.6 us | 2,668.6 us | 0 |
| ins-del-in4 | 100,000 | A | 120 | 10 | 14,339.49 us | 12,353.21..21,594.92 us | 14,764.7 us | 11,602.4 us | 13,424.7 us | 14,187.4 us | 15,591.1 us | 16,959.2 us | 21,601.3 us | 25,757.2 us | 0 |
| ins-del-in4 | 100,000 | B | 120 | 10 | 88.92 us | 77.72..111.29 us | 163.2 us | 99.1 us | 117.0 us | 130.3 us | 152.4 us | 218.7 us | 610.7 us | 1,193.6 us | 0 |
| ins-del-in4 | 100,000 | A2 | 120 | 10 | 14,416.06 us | 12,231.67..21,853.98 us | 14,836.9 us | 11,640.4 us | 13,462.0 us | 14,209.0 us | 15,590.8 us | 17,024.9 us | 21,092.4 us | 27,026.8 us | 0 |

Every cell of the supplement:

| shape | rows | arm | units/run | runs | server CPU per unit (mean of run means) | min..max over runs | client mean | p0 | p25 | p50 | p75 | p90 | p99 | max | bad |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| scan-in2 | 1,000 | A | 1,200 | 10 | 102.14 us | 98.23..120.34 us | 140.3 us | 121.6 us | 127.6 us | 132.1 us | 140.8 us | 155.0 us | 237.0 us | 5,773.2 us | 0 |
| scan-in2 | 1,000 | B | 1,200 | 10 | 100.34 us | 97.20..112.11 us | 137.3 us | 119.4 us | 126.1 us | 130.2 us | 138.8 us | 152.6 us | 228.6 us | 1,534.1 us | 0 |
| scan-in2 | 1,000 | A2 | 1,200 | 10 | 102.62 us | 98.22..119.82 us | 140.1 us | 121.2 us | 127.7 us | 132.0 us | 141.1 us | 155.9 us | 233.7 us | 6,064.8 us | 0 |
| scan-or | 1,000 | A | 1,200 | 10 | 107.68 us | 105.94..110.87 us | 144.1 us | 128.0 us | 134.5 us | 138.1 us | 144.6 us | 157.5 us | 229.2 us | 1,665.5 us | 0 |
| scan-or | 1,000 | B | 1,200 | 10 | 106.82 us | 103.10..110.17 us | 144.3 us | 124.8 us | 132.7 us | 136.3 us | 143.3 us | 157.8 us | 245.8 us | 3,569.2 us | 0 |
| scan-or | 1,000 | A2 | 1,200 | 10 | 107.95 us | 105.94..111.56 us | 144.4 us | 128.2 us | 134.7 us | 138.1 us | 144.7 us | 158.1 us | 230.9 us | 939.6 us | 0 |
| pk-trail | 1,000 | A | 3,000 | 10 | 22.65 us | 22.05..23.71 us | 55.8 us | 48.0 us | 50.9 us | 52.4 us | 56.2 us | 62.0 us | 83.6 us | 1,033.4 us | 0 |
| pk-trail | 1,000 | B | 3,000 | 10 | 22.67 us | 21.77..23.45 us | 55.8 us | 47.3 us | 50.8 us | 52.3 us | 55.8 us | 61.6 us | 86.2 us | 2,015.5 us | 0 |
| pk-trail | 1,000 | A2 | 3,000 | 10 | 22.90 us | 21.73..24.26 us | 56.0 us | 47.9 us | 50.9 us | 52.4 us | 55.7 us | 61.2 us | 84.7 us | 2,440.6 us | 0 |
| update-in2 | 1,000 | A | 1,200 | 10 | 134.74 us | 124.20..199.50 us | 163.4 us | 134.2 us | 142.8 us | 157.0 us | 168.9 us | 191.7 us | 282.0 us | 1,066.5 us | 0 |
| update-in2 | 1,000 | B | 1,200 | 10 | 134.72 us | 122.74..184.23 us | 165.6 us | 131.3 us | 140.7 us | 155.6 us | 169.3 us | 200.2 us | 310.6 us | 5,146.9 us | 0 |
| update-in2 | 1,000 | A2 | 1,200 | 10 | 133.93 us | 123.14..190.50 us | 164.3 us | 133.8 us | 141.0 us | 156.6 us | 169.0 us | 192.7 us | 341.8 us | 6,422.9 us | 0 |

## CU-S0's arms again: the list as a set, beside today's walk

`tools/cu_close_lists.py`, CU-S0's driver with one arm added. One relation `cu_t (id int64, k int64, a int64, b int64)` with `k = 3 * id + 1` and an index on `k`, loaded into a server of each binary. Per cell (rows, column, n), two repeats of 2 s per arm (1 s on the small relations), arm order rotated:

- **LA**: `SELECT * FROM cu_t WHERE <col> IN (v1, ..., vn)` sent to A. The walk with the list as a residual: what CU-S0 called L.
- **L**, **L2**: the same statement sent to B, twice. A key set; L2 is the noise control.
- **D**: the n values as n single-value statements on B. CU-S0's upper bound.
- **J**: the n values stored as rows and joined to `cu_t` on B, n descents in one statement. CU-S0's closest stand-in for a set.

A group's values are drawn afresh, distinct, from `[1, max(rows, n)]`, so a list longer than the relation carries misses. A one-value list is collapsed to an equality by the parser and is a single descent on both binaries. In the tables the ratios against the set use the mean of L and L2.

Server CPU per list, 1,000 and 100,000 rows:

| column | rows | n | LA walk (base) | L set | L2 set | set spread | D n statements | J n descents | LA / L | L / J | D / L | reading |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `id` | 1,000 rows | 1 value | 24.0 µs | 25.2 µs | 24.4 µs | 8 % | 23.9 µs | 30.0 µs | 0.97 times | 0.83 times | 0.96 times | one value is keyed on both |
| `id` | 1,000 rows | 4 values | 137 µs | 30.1 µs | 30.9 µs | 5 % | 93.7 µs | 32.5 µs | 4.49 times | 0.94 times | 3.07 times | set faster |
| `id` | 1,000 rows | 64 values | 880 µs | 126 µs | 129 µs | 9 % | 1,467 µs | 97.1 µs | 6.92 times | 1.31 times | 11.53 times | set faster |
| `id` | 1,000 rows | 1,024 values | 6,370 µs | 1,347 µs | 1,328 µs | 5 % | 24.2 ms | 913 µs | 4.76 times | 1.47 times | 18.09 times | set faster |
| `id` | 1,000 rows | 4,096 values | 27.0 ms | 6,395 µs | 6,356 µs | 10 % | 95.6 ms | 2,561 µs | 4.23 times | 2.49 times | 14.99 times | set faster |
| `id` | 100,000 rows | 1 value | 28.5 µs | 29.0 µs | 29.4 µs | 7 % | 28.9 µs | 37.2 µs | 0.97 times | 0.78 times | 0.99 times | one value is keyed on both |
| `id` | 100,000 rows | 4 values | 9,974 µs | 35.0 µs | 33.8 µs | 8 % | 98.2 µs | 35.4 µs | 290.17 times | 0.97 times | 2.86 times | set faster |
| `id` | 100,000 rows | 64 values | 78.1 ms | 147 µs | 150 µs | 4 % | 1,748 µs | 103 µs | 525.86 times | 1.44 times | 11.77 times | set faster |
| `id` | 100,000 rows | 1,024 values | 1.14 s | 1,679 µs | 1,656 µs | 6 % | 25.2 ms | 1,225 µs | 685.23 times | 1.36 times | 15.14 times | set faster |
| `id` | 100,000 rows | 4,096 values | 4.17 s | 8,391 µs | 8,179 µs | 6 % | 96.2 ms | 4,442 µs | 503.93 times | 1.87 times | 11.62 times | set faster |
| `k` | 1,000 rows | 1 value | 28.1 µs | 28.9 µs | 28.0 µs | 7 % | 28.0 µs | 36.7 µs | 0.99 times | 0.77 times | 0.99 times | one value is keyed on both |
| `k` | 1,000 rows | 4 values | 139 µs | 45.6 µs | 44.0 µs | 8 % | 114 µs | 44.7 µs | 3.11 times | 1.00 times | 2.54 times | set faster |
| `k` | 1,000 rows | 64 values | 848 µs | 260 µs | 258 µs | 2 % | 1,736 µs | 243 µs | 3.27 times | 1.07 times | 6.70 times | set faster |
| `k` | 1,000 rows | 1,024 values | 7,099 µs | 3,669 µs | 3,581 µs | 17 % | 30.7 ms | 3,248 µs | 1.96 times | 1.12 times | 8.47 times | set faster |
| `k` | 1,000 rows | 4,096 values | 32.3 ms | 20.7 ms | 20.5 ms | 6 % | 122.9 ms | 16.8 ms | 1.57 times | 1.23 times | 5.95 times | set faster |
| `k` | 100,000 rows | 1 value | 31.5 µs | 29.9 µs | 31.6 µs | 13 % | 31.8 µs | 37.4 µs | 1.03 times | 0.82 times | 1.03 times | one value is keyed on both |
| `k` | 100,000 rows | 4 values | 12.7 ms | 59.1 µs | 53.6 µs | 26 % | 127 µs | 52.1 µs | 226.20 times | 1.08 times | 2.25 times | set faster |
| `k` | 100,000 rows | 64 values | 91.6 ms | 336 µs | 350 µs | 7 % | 2,042 µs | 286 µs | 267.07 times | 1.20 times | 5.95 times | set faster |
| `k` | 100,000 rows | 1,024 values | 1.32 s | 4,287 µs | 4,255 µs | 5 % | 32.8 ms | 3,980 µs | 309.66 times | 1.07 times | 7.68 times | set faster |
| `k` | 100,000 rows | 4,096 values | 5.46 s | 19.8 ms | 20.2 ms | 5 % | 125.7 ms | 16.1 ms | 272.94 times | 1.24 times | 6.28 times | set faster |

- **Every list of two values or more is faster as a set at 1,000 rows and above**: 4.2 to 6.9 times on the tuple key and 1.6 to 3.3 times on the index at 1,000 rows, 290 to 685 times and 226 to 310 times at 100,000 rows.
- **The set costs what n descents in one statement cost** (L / J 0.94 to 1.47 up to 1,024 values), and more than J at 4,096 values on the tuple key (1.87 and 2.49 times), where the list's text is what the statement spends: J's statement carries no list.
- **n statements are 2.2 to 18 times dearer than one set** (D / L), as CU-S0 said they would be: each pays a parse, a reply and a round trip.
- **The walk's long-list cells at 100,000 rows rest on few statements**: a 1,024-value walk takes over a second, so LA holds 4 groups there and 2 at 4,096 values. The ratios at those lengths (273 to 685 times) are of that firmness; the set's side of each holds hundreds of groups.

Per value:

| column | rows | n | L set CPU per value | J CPU per value | D CPU per value | LA CPU per row walked |
|---|---|---|---|---|---|---|
| `id` | 1,000 rows | 1 value | 24.80 µs | 30.05 µs | 23.85 µs | no walk (one value is keyed) |
| `id` | 1,000 rows | 4 values | 7.62 µs | 8.12 µs | 23.41 µs | 137 ns |
| `id` | 1,000 rows | 64 values | 1.99 µs | 1.52 µs | 22.92 µs | 880 ns |
| `id` | 1,000 rows | 1,024 values | 1.31 µs | 0.89 µs | 23.63 µs | 6,370 ns |
| `id` | 1,000 rows | 4,096 values | 1.56 µs | 0.63 µs | 23.33 µs | 26,985 ns |
| `id` | 100,000 rows | 1 value | 29.20 µs | 37.25 µs | 28.90 µs | no walk (one value is keyed) |
| `id` | 100,000 rows | 4 values | 8.59 µs | 8.84 µs | 24.55 µs | 100 ns |
| `id` | 100,000 rows | 64 values | 2.32 µs | 1.61 µs | 27.31 µs | 781 ns |
| `id` | 100,000 rows | 1,024 values | 1.63 µs | 1.20 µs | 24.66 µs | 11,425 ns |
| `id` | 100,000 rows | 4,096 values | 2.02 µs | 1.08 µs | 23.50 µs | 41,750 ns |
| `k` | 1,000 rows | 1 value | 28.43 µs | 36.70 µs | 28.00 µs | no walk (one value is keyed) |
| `k` | 1,000 rows | 4 values | 11.20 µs | 11.18 µs | 28.43 µs | 139 ns |
| `k` | 1,000 rows | 64 values | 4.05 µs | 3.79 µs | 27.12 µs | 848 ns |
| `k` | 1,000 rows | 1,024 values | 3.54 µs | 3.17 µs | 29.97 µs | 7,099 ns |
| `k` | 1,000 rows | 4,096 values | 5.04 µs | 4.10 µs | 29.99 µs | 32,335 ns |
| `k` | 100,000 rows | 1 value | 30.73 µs | 37.40 µs | 31.80 µs | no walk (one value is keyed) |
| `k` | 100,000 rows | 4 values | 14.09 µs | 13.02 µs | 31.72 µs | 127 ns |
| `k` | 100,000 rows | 64 values | 5.36 µs | 4.47 µs | 31.91 µs | 916 ns |
| `k` | 100,000 rows | 1,024 values | 4.17 µs | 3.89 µs | 32.03 µs | 13,225 ns |
| `k` | 100,000 rows | 4,096 values | 4.88 µs | 3.93 µs | 30.69 µs | 54,600 ns |

load average (1 min) min 3.19 max 5.18; cc1plus processes min 0 max 0; 200 arm runs; checks all same_rows: True (20 cells)

### Against CU-S0's own numbers

CU-S0 measured at `1edf5c54` (`v3.4.11-20-g1edf5c54`,
`results-cu-s0-premise-v3.4.11-20-g1edf5c54.md`), 8 commits before arm A and 20 before arm B by `git describe`'s counts, on the same host on 2026-11-16. Its estimate of a set is recomputed from its own archive as its `estimate.py` did (the list alone plus the descents); the same-run estimate is that formula over this run's arms (the list alone is the walk on A over the 1-row relation). "Walk at CU-S0" and "set measured" are the means of the two L arms of each run.

| column | rows | n | walk at CU-S0 | walk now (LA) | now / then | set estimated at CU-S0 | set measured (L, L2) | measured / estimate | same-run estimate | measured / same-run | walk now / set | D at CU-S0 | D now | J at CU-S0 | J now |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `id` | 10 rows | 4 values | 28.2 µs | 29.9 µs | 1.06 | 25.6 µs | 25.8 µs | 1.00 | 27.4 µs | 0.94 | 1.16 times | 93.7 µs | 81.8 µs | 28.5 µs | 27.6 µs |
| `id` | 10 rows | 64 values | 66.2 µs | 67.6 µs | 1.02 | 82.4 µs | 84.0 µs | 1.02 | 104 µs | 0.81 | 0.80 times | 1,340 µs | 1,553 µs | 57.7 µs | 69.5 µs |
| `id` | 10 rows | 4,096 values | 4,630 µs | 4,658 µs | 1.01 | 5,604 µs | 5,562 µs | 0.99 | 6,760 µs | 0.82 | 0.84 times | 86.7 ms | 95.8 ms | 1,617 µs | 1,534 µs |
| `id` | 100 rows | 4 values | 37.1 µs | 35.1 µs | 0.94 | 25.6 µs | 25.4 µs | 0.99 | 27.4 µs | 0.93 | 1.38 times | 86.1 µs | 83.7 µs | 30.0 µs | 27.3 µs |
| `id` | 100 rows | 64 values | 120 µs | 125 µs | 1.05 | 92.8 µs | 101 µs | 1.09 | 112 µs | 0.90 | 1.24 times | 1,370 µs | 1,292 µs | 68.1 µs | 77.8 µs |
| `id` | 100 rows | 4,096 values | 5,992 µs | 6,475 µs | 1.08 | 5,811 µs | 5,776 µs | 0.99 | 6,981 µs | 0.83 | 1.12 times | 88.0 ms | 79.2 ms | 1,824 µs | 1,755 µs |
| `id` | 1,000 rows | 4 values | 124 µs | 137 µs | 1.10 | 26.3 µs | 30.5 µs | 1.16 | 28.6 µs | 1.07 | 4.49 times | 105 µs | 93.7 µs | 33.2 µs | 32.5 µs |
| `id` | 1,000 rows | 64 values | 826 µs | 880 µs | 1.07 | 111 µs | 127 µs | 1.14 | 131 µs | 0.97 | 6.92 times | 1,434 µs | 1,467 µs | 86.5 µs | 97.1 µs |
| `id` | 1,000 rows | 1,024 values | 6,054 µs | 6,370 µs | 1.05 | 1,479 µs | 1,337 µs | 0.90 | 1,567 µs | 0.85 | 4.76 times | 24.5 ms | 24.2 ms | 887 µs | 913 µs |
| `id` | 1,000 rows | 4,096 values | 25.2 ms | 27.0 ms | 1.07 | 6,190 µs | 6,375 µs | 1.03 | 7,787 µs | 0.82 | 4.23 times | 87.0 ms | 95.6 ms | 2,202 µs | 2,561 µs |
| `id` | 100,000 rows | 4 values | 9,110 µs | 9,974 µs | 1.09 | 30.9 µs | 34.4 µs | 1.11 | 31.5 µs | 1.09 | 290.17 times | 98.4 µs | 98.2 µs | 37.9 µs | 35.4 µs |
| `id` | 100,000 rows | 64 values | 73.3 ms | 78.1 ms | 1.07 | 113 µs | 148 µs | 1.32 | 137 µs | 1.08 | 525.86 times | 1,479 µs | 1,748 µs | 88.0 µs | 103 µs |
| `id` | 100,000 rows | 1,024 values | 1.04 s | 1.14 s | 1.09 | 1,726 µs | 1,667 µs | 0.97 | 1,880 µs | 0.89 | 685.23 times | 23.9 ms | 25.2 ms | 1,135 µs | 1,225 µs |
| `id` | 100,000 rows | 4,096 values | 3.98 s | 4.17 s | 1.05 | 8,196 µs | 8,285 µs | 1.01 | 9,667 µs | 0.86 | 503.93 times | 102.1 ms | 96.2 ms | 4,208 µs | 4,442 µs |
| `k` | 10 rows | 4 values | 28.3 µs | 25.3 µs | 0.89 | 35.1 µs | 28.4 µs | 0.81 | 26.7 µs | 1.06 | 0.89 times | 95.8 µs | 85.8 µs | 36.2 µs | 30.9 µs |
| `k` | 10 rows | 64 values | 67.9 µs | 67.6 µs | 1.00 | 111 µs | 96.0 µs | 0.86 | 115 µs | 0.83 | 0.70 times | 1,544 µs | 1,531 µs | 73.2 µs | 70.7 µs |
| `k` | 10 rows | 4,096 values | 4,627 µs | 4,698 µs | 1.02 | 5,952 µs | 6,426 µs | 1.08 | 6,577 µs | 0.98 | 0.73 times | 91.0 ms | 82.8 ms | 2,041 µs | 2,088 µs |
| `k` | 100 rows | 4 values | 40.5 µs | 40.1 µs | 0.99 | 36.3 µs | 36.8 µs | 1.01 | 33.7 µs | 1.09 | 1.09 times | 110 µs | 108 µs | 37.5 µs | 41.0 µs |
| `k` | 100 rows | 64 values | 126 µs | 128 µs | 1.01 | 168 µs | 163 µs | 0.97 | 185 µs | 0.88 | 0.79 times | 1,724 µs | 1,511 µs | 130 µs | 141 µs |
| `k` | 100 rows | 4,096 values | 5,996 µs | 6,542 µs | 1.09 | 9,655 µs | 10.1 ms | 1.04 | 10.5 ms | 0.96 | 0.65 times | 113.8 ms | 90.0 ms | 5,744 µs | 5,993 µs |
| `k` | 1,000 rows | 4 values | 128 µs | 139 µs | 1.09 | 43.5 µs | 44.8 µs | 1.03 | 37.4 µs | 1.20 | 3.11 times | 114 µs | 114 µs | 44.6 µs | 44.7 µs |
| `k` | 1,000 rows | 64 values | 789 µs | 848 µs | 1.07 | 241 µs | 259 µs | 1.07 | 287 µs | 0.90 | 3.27 times | 1,728 µs | 1,736 µs | 203 µs | 243 µs |
| `k` | 1,000 rows | 1,024 values | 5,983 µs | 7,099 µs | 1.19 | 3,299 µs | 3,625 µs | 1.10 | 3,852 µs | 0.94 | 1.96 times | 25.8 ms | 30.7 ms | 2,675 µs | 3,248 µs |
| `k` | 1,000 rows | 4,096 values | 25.4 ms | 32.3 ms | 1.27 | 16.7 ms | 20.6 ms | 1.24 | 21.3 ms | 0.97 | 1.57 times | 112.6 ms | 122.9 ms | 12.8 ms | 16.8 ms |
| `k` | 100,000 rows | 4 values | 9,359 µs | 12.7 ms | 1.36 | 37.4 µs | 56.4 µs | 1.51 | 44.8 µs | 1.26 | 226.20 times | 103 µs | 127 µs | 38.5 µs | 52.1 µs |
| `k` | 100,000 rows | 64 values | 72.7 ms | 91.6 ms | 1.26 | 242 µs | 343 µs | 1.41 | 331 µs | 1.04 | 267.07 times | 1,745 µs | 2,042 µs | 204 µs | 286 µs |
| `k` | 100,000 rows | 1,024 values | 1.04 s | 1.32 s | 1.27 | 3,710 µs | 4,271 µs | 1.15 | 4,584 µs | 0.93 | 309.66 times | 27.7 ms | 32.8 ms | 3,085 µs | 3,980 µs |
| `k` | 100,000 rows | 4,096 values | 4.02 s | 5.46 s | 1.36 | 15.7 ms | 20.0 ms | 1.27 | 20.6 ms | 0.97 | 272.94 times | 111.1 ms | 125.7 ms | 11.8 ms | 16.1 ms |

- **The walk reads 0.89 to 1.36 times its CU-S0 number**, on a binary whose walk CU did not touch (arm A). The tuple key cells of the table are within 10 %. The index cells read 1.19 and 1.27 times at 1,000 rows from 1,024 values and 1.26 to 1.36 times at 100,000 rows, and D and J at 100,000 rows read 1.13 to 1.40 times theirs.
- **That drift is not explained.** The host was busier than at CU-S0, but the tuple key walk and the index-column walk are the same scan of the same pages and only the second drifted, under a load average that was no higher in its cells. The comparison that holds is the same-run one, LA against L.
- **The measured set is 0.81 to 1.51 times CU-S0's estimate, and 0.81 to 1.26 times the same-run estimate.** Against CU-S0's the three cells above 1.3 are at 100,000 rows: two index cells (1.51, 1.41), where this run's walk reads 1.36 and 1.26 times its CU-S0 number, and the 64-value tuple key cell (1.32), where it reads 1.07. Against the same-run estimate those three read 1.26, 1.04 and 1.08. The estimate left out the re-check and the set's own build; neither shows as a term the estimate missed.
- CU-S0 did not measure the 1,024-value cells at 10 and 100 rows; they are in the small table below.

### The small relations: CU-Q3's loss, measured

| column | rows | n | LA walk (base) | L set | L2 set | set spread | D n statements | J n descents | LA / L | L / J | D / L | reading |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `id` | 1 rows | 1 value | 24.3 µs | 26.1 µs | 24.6 µs | 14 % | 24.8 µs | 32.8 µs | 0.96 times | 0.77 times | 0.97 times | one value is keyed on both |
| `id` | 1 rows | 4 values | 27.4 µs | 28.8 µs | 29.1 µs | 7 % | 94.1 µs | 32.2 µs | 0.95 times | 0.90 times | 3.25 times | inside noise |
| `id` | 1 rows | 64 values | 65.5 µs | 82.5 µs | 88.1 µs | 12 % | 1,457 µs | 62.0 µs | 0.77 times | 1.38 times | 17.08 times | set slower |
| `id` | 1 rows | 1,024 values | 686 µs | 968 µs | 941 µs | 13 % | 23.4 ms | 444 µs | 0.72 times | 2.15 times | 24.49 times | set slower |
| `id` | 1 rows | 4,096 values | 5,257 µs | 6,461 µs | 6,281 µs | 12 % | 98.8 ms | 1,921 µs | 0.83 times | 3.32 times | 15.50 times | set slower |
| `id` | 10 rows | 1 value | 20.2 µs | 20.6 µs | 21.7 µs | 12 % | 20.7 µs | 28.2 µs | 0.95 times | 0.75 times | 0.98 times | one value is keyed on both |
| `id` | 10 rows | 4 values | 29.9 µs | 26.0 µs | 25.5 µs | 14 % | 81.8 µs | 27.6 µs | 1.16 times | 0.93 times | 3.18 times | inside noise |
| `id` | 10 rows | 64 values | 67.6 µs | 80.8 µs | 87.2 µs | 25 % | 1,553 µs | 69.5 µs | 0.80 times | 1.21 times | 18.49 times | inside noise |
| `id` | 10 rows | 1,024 values | 670 µs | 891 µs | 904 µs | 3 % | 23.4 ms | 418 µs | 0.75 times | 2.15 times | 26.03 times | set slower |
| `id` | 10 rows | 4,096 values | 4,658 µs | 5,591 µs | 5,532 µs | 2 % | 95.8 ms | 1,534 µs | 0.84 times | 3.62 times | 17.22 times | set slower |
| `id` | 100 rows | 1 value | 20.2 µs | 21.4 µs | 20.7 µs | 5 % | 20.0 µs | 28.1 µs | 0.96 times | 0.75 times | 0.95 times | one value is keyed on both |
| `id` | 100 rows | 4 values | 35.1 µs | 25.3 µs | 25.4 µs | 4 % | 83.7 µs | 27.3 µs | 1.38 times | 0.93 times | 3.30 times | set faster |
| `id` | 100 rows | 64 values | 125 µs | 99.8 µs | 102 µs | 9 % | 1,292 µs | 77.8 µs | 1.24 times | 1.30 times | 12.78 times | set faster |
| `id` | 100 rows | 1,024 values | 1,191 µs | 977 µs | 977 µs | 3 % | 20.0 ms | 507 µs | 1.22 times | 1.93 times | 20.47 times | set faster |
| `id` | 100 rows | 4,096 values | 6,475 µs | 5,784 µs | 5,768 µs | 1 % | 79.2 ms | 1,755 µs | 1.12 times | 3.29 times | 13.71 times | set faster |
| `k` | 1 rows | 1 value | 29.0 µs | 29.8 µs | 29.1 µs | 4 % | 28.4 µs | 37.1 µs | 0.99 times | 0.79 times | 0.96 times | one value is keyed on both |
| `k` | 1 rows | 4 values | 26.7 µs | 34.4 µs | 35.3 µs | 13 % | 87.0 µs | 33.5 µs | 0.77 times | 1.04 times | 2.49 times | set slower |
| `k` | 1 rows | 64 values | 78.5 µs | 94.6 µs | 103 µs | 29 % | 1,306 µs | 69.0 µs | 0.79 times | 1.43 times | 13.21 times | inside noise |
| `k` | 1 rows | 1,024 values | 638 µs | 988 µs | 1,016 µs | 4 % | 25.1 ms | 473 µs | 0.64 times | 2.12 times | 25.00 times | set slower |
| `k` | 1 rows | 4,096 values | 4,523 µs | 5,977 µs | 5,977 µs | 1 % | 84.0 ms | 1,810 µs | 0.76 times | 3.30 times | 14.05 times | set slower |
| `k` | 10 rows | 1 value | 29.9 µs | 27.6 µs | 27.5 µs | 20 % | 25.5 µs | 30.3 µs | 1.09 times | 0.91 times | 0.93 times | one value is keyed on both |
| `k` | 10 rows | 4 values | 25.3 µs | 28.1 µs | 28.8 µs | 5 % | 85.8 µs | 30.9 µs | 0.89 times | 0.92 times | 3.02 times | set slower |
| `k` | 10 rows | 64 values | 67.6 µs | 91.1 µs | 101 µs | 21 % | 1,531 µs | 70.7 µs | 0.70 times | 1.36 times | 15.94 times | set slower |
| `k` | 10 rows | 1,024 values | 681 µs | 1,089 µs | 1,096 µs | 3 % | 24.6 ms | 560 µs | 0.62 times | 1.95 times | 22.48 times | set slower |
| `k` | 10 rows | 4,096 values | 4,698 µs | 6,426 µs | 6,426 µs | 1 % | 82.8 ms | 2,088 µs | 0.73 times | 3.08 times | 12.89 times | set slower |
| `k` | 100 rows | 1 value | 23.1 µs | 26.4 µs | 22.0 µs | 24 % | 23.9 µs | 28.6 µs | 0.95 times | 0.85 times | 0.99 times | one value is keyed on both |
| `k` | 100 rows | 4 values | 40.1 µs | 32.8 µs | 40.7 µs | 42 % | 108 µs | 41.0 µs | 1.09 times | 0.90 times | 2.93 times | inside noise |
| `k` | 100 rows | 64 values | 128 µs | 156 µs | 169 µs | 21 % | 1,511 µs | 141 µs | 0.79 times | 1.16 times | 9.29 times | set slower |
| `k` | 100 rows | 1,024 values | 1,181 µs | 2,041 µs | 2,051 µs | 4 % | 25.6 ms | 1,548 µs | 0.58 times | 1.32 times | 12.50 times | set slower |
| `k` | 100 rows | 4,096 values | 6,542 µs | 10.1 ms | 10.1 ms | 1 % | 90.0 ms | 5,993 µs | 0.65 times | 1.68 times | 8.95 times | set slower |

- **Tuple key.** At 1 and 10 rows a set is slower than the walk from 64 values up: by 16 to 20 us at 64 values (inside the spread at 10 rows), 0.2 to 0.3 ms at 1,024, and 0.9 to 1.1 ms at 4,096. At 100 rows it is faster at every length (1.12 to 1.38 times).
- **Index.** At 1 and 10 rows a set is slower at every length from 4 values (the 64-value cell at 1 row is inside its spread): 3 to 8 us at 4 values, 0.4 ms at 1,024, 1.5 to 1.7 ms at 4,096. At 100 rows it is slower from 64 values up: 34 us, 0.9 ms and about 3.6 ms.
- **The largest loss is about 3.6 ms per statement** (index, 100 rows, 4,096 values), where CU-S0 estimated 3.7 ms. The crossover is between 10 and 100 rows on the tuple key and between 100 and 1,000 on an index, as CU-S0 placed it.
- A 1-row relation shows what the set adds to the list alone: 4,096 values cost 5,257 us on A and 6,371 us on B, about 0.27 us a value for building the set and descending for values that are not there.

load average (1 min) min 1.03 max 4.89; cc1plus processes min 0 max 0; 300 arm runs; checks all same_rows: True (30 cells)

### CU-Q8's bound on the built path

A statement driven by a set runs inline and does not yield. The largest set the grammar admits, 4,096 values, holds its core for the whole statement: 8.3 ms of server CPU on the tuple key over 100,000 rows (6.4 ms over 1,000), and 20.0 ms on the index (20.6 ms over 1,000 rows). Between 4.5 and 5.3 ms of each is the list's parse, type and compile, which the walk also spent before its first slice. CU-S0 derived 1.6 to 4.2 ms of descents on the tuple key and 2.0 to 12.7 ms on an index on top of about 4 ms for the list. On the tuple key the measured statement is at that sum (8.3 ms against 8.2 ms at 100,000 rows); on the index it is above it (20.0 ms against 15.7 ms at 100,000 rows, 20.6 ms against 16.7 ms at 1,000), in cells where this run's other arms also read above CU-S0's. The join bound (a set on a step whose later steps walk) was not measured.

## Client latency per list

p25 to p99 are means of the two repeats' values; p0 is the smallest group of either repeat and max the largest.

1,000 and 100,000 rows:

| column | rows | n | arm | groups | lists/s | p0 | p25 | p50 | p75 | p99 | max | rows/list |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `id` | 1,000 rows | 1 value | LA | 57,022 groups | 15,362.4 lists/s | 48.7 µs | 52.5 µs | 62.8 µs | 72.3 µs | 132 µs | 4,314 µs | 1 rows |
| `id` | 1,000 rows | 1 value | L | 53,120 groups | 14,292.8 lists/s | 49.1 µs | 53.5 µs | 67.2 µs | 74.0 µs | 162 µs | 4,109 µs | 1 rows |
| `id` | 1,000 rows | 1 value | L2 | 55,457 groups | 14,937.0 lists/s | 49.1 µs | 53.2 µs | 64.6 µs | 72.5 µs | 140 µs | 4,679 µs | 1 rows |
| `id` | 1,000 rows | 1 value | D | 56,699 groups | 15,297.4 lists/s | 45.9 µs | 52.7 µs | 63.8 µs | 70.9 µs | 136 µs | 1,904 µs | 1 rows |
| `id` | 1,000 rows | 1 value | J | 45,630 groups | 13,594.2 lists/s | 52.8 µs | 57.1 µs | 66.3 µs | 84.2 µs | 150 µs | 3,906 µs | 1 rows |
| `id` | 1,000 rows | 4 values | LA | 18,994 groups | 4,944.6 lists/s | 146 µs | 165 µs | 197 µs | 220 µs | 356 µs | 4,277 µs | 4 rows |
| `id` | 1,000 rows | 4 values | L | 45,793 groups | 12,427.6 lists/s | 58.1 µs | 62.1 µs | 73.9 µs | 91.1 µs | 160 µs | 4,150 µs | 4 rows |
| `id` | 1,000 rows | 4 values | L2 | 46,373 groups | 12,559.5 lists/s | 58.2 µs | 62.1 µs | 70.9 µs | 84.7 µs | 164 µs | 4,636 µs | 4 rows |
| `id` | 1,000 rows | 4 values | D | 14,960 groups | 3,865.1 lists/s | 195 µs | 209 µs | 258 µs | 283 µs | 474 µs | 3,407 µs | 4 rows |
| `id` | 1,000 rows | 4 values | J | 41,549 groups | 12,410.0 lists/s | 58.7 µs | 62.8 µs | 72.0 µs | 88.0 µs | 167 µs | 3,420 µs | 4 rows |
| `id` | 1,000 rows | 64 values | LA | 3,607 groups | 935.3 lists/s | 844 µs | 953 µs | 1,016 µs | 1,116 µs | 1,724 µs | 4,170 µs | 64 rows |
| `id` | 1,000 rows | 64 values | L | 13,553 groups | 3,772.7 lists/s | 184 µs | 203 µs | 260 µs | 299 µs | 460 µs | 1,978 µs | 64 rows |
| `id` | 1,000 rows | 64 values | L2 | 13,308 groups | 3,698.5 lists/s | 184 µs | 210 µs | 273 µs | 306 µs | 462 µs | 2,613 µs | 64 rows |
| `id` | 1,000 rows | 64 values | D | 982 groups | 247.7 lists/s | 3,191 µs | 3,513 µs | 3,989 µs | 4,495 µs | 5,558 µs | 8,133 µs | 64 rows |
| `id` | 1,000 rows | 64 values | J | 15,560 groups | 4,519.4 lists/s | 146 µs | 167 µs | 225 µs | 248 µs | 400 µs | 4,246 µs | 64 rows |
| `id` | 1,000 rows | 1,024 values | LA | 512 groups | 133.2 lists/s | 6,861 µs | 7,076 µs | 7,215 µs | 7,575 µs | 11.4 ms | 13.7 ms | 1,000 rows |
| `id` | 1,000 rows | 1,024 values | L | 1,411 groups | 398.4 lists/s | 2,178 µs | 2,310 µs | 2,382 µs | 2,574 µs | 3,648 µs | 4,302 µs | 1,000 rows |
| `id` | 1,000 rows | 1,024 values | L2 | 1,409 groups | 397.4 lists/s | 2,196 µs | 2,299 µs | 2,362 µs | 2,701 µs | 3,660 µs | 6,593 µs | 1,000 rows |
| `id` | 1,000 rows | 1,024 values | D | 62 groups | 15.3 lists/s | 54.3 ms | 62.6 ms | 66.1 ms | 69.7 ms | 76.7 ms | 76.8 ms | 1,000 rows |
| `id` | 1,000 rows | 1,024 values | J | 1,808 groups | 530.5 lists/s | 1,689 µs | 1,779 µs | 1,808 µs | 1,884 µs | 2,752 µs | 5,354 µs | 1,000 rows |
| `id` | 1,000 rows | 4,096 values | LA | 136 groups | 35.0 lists/s | 25.5 ms | 26.8 ms | 27.4 ms | 28.7 ms | 46.8 ms | 47.7 ms | 1,000 rows |
| `id` | 1,000 rows | 4,096 values | L | 441 groups | 125.9 lists/s | 7,220 µs | 7,511 µs | 7,711 µs | 8,101 µs | 10.4 ms | 11.4 ms | 1,000 rows |
| `id` | 1,000 rows | 4,096 values | L2 | 435 groups | 124.8 lists/s | 7,132 µs | 7,378 µs | 7,612 µs | 8,364 µs | 11.0 ms | 13.1 ms | 1,000 rows |
| `id` | 1,000 rows | 4,096 values | D | 17 groups | 4.0 lists/s | 214.8 ms | 231.0 ms | 240.1 ms | 254.5 ms | 299.5 ms | 329.2 ms | 1,000 rows |
| `id` | 1,000 rows | 4,096 values | J | 773 groups | 263.4 lists/s | 2,967 µs | 3,140 µs | 3,654 µs | 4,370 µs | 6,421 µs | 9,370 µs | 1,000 rows |
| `id` | 100,000 rows | 1 value | LA | 48,497 groups | 13,025.2 lists/s | 50.5 µs | 65.4 µs | 72.9 µs | 78.3 µs | 170 µs | 2,358 µs | 1 rows |
| `id` | 100,000 rows | 1 value | L | 47,280 groups | 12,703.5 lists/s | 50.2 µs | 66.3 µs | 73.7 µs | 79.7 µs | 179 µs | 6,012 µs | 1 rows |
| `id` | 100,000 rows | 1 value | L2 | 46,335 groups | 12,441.0 lists/s | 50.9 µs | 66.5 µs | 74.0 µs | 80.7 µs | 193 µs | 4,402 µs | 1 rows |
| `id` | 100,000 rows | 1 value | D | 47,838 groups | 12,859.1 lists/s | 49.4 µs | 64.8 µs | 71.8 µs | 77.9 µs | 191 µs | 4,115 µs | 1 rows |
| `id` | 100,000 rows | 1 value | J | 36,804 groups | 10,750.7 lists/s | 53.9 µs | 68.4 µs | 82.2 µs | 104 µs | 247 µs | 6,096 µs | 1 rows |
| `id` | 100,000 rows | 4 values | LA | 392 groups | 98.1 lists/s | 9,410 µs | 9,655 µs | 9,940 µs | 10.3 ms | 15.6 ms | 17.3 ms | 4 rows |
| `id` | 100,000 rows | 4 values | L | 42,039 groups | 11,333.5 lists/s | 59.4 µs | 64.8 µs | 77.0 µs | 102 µs | 189 µs | 4,818 µs | 4 rows |
| `id` | 100,000 rows | 4 values | L2 | 43,569 groups | 11,773.3 lists/s | 59.5 µs | 64.1 µs | 75.9 µs | 98.8 µs | 165 µs | 1,391 µs | 4 rows |
| `id` | 100,000 rows | 4 values | D | 14,669 groups | 3,784.4 lists/s | 197 µs | 213 µs | 262 µs | 289 µs | 497 µs | 5,096 µs | 4 rows |
| `id` | 100,000 rows | 4 values | J | 39,607 groups | 11,751.6 lists/s | 58.9 µs | 63.8 µs | 76.9 µs | 100 µs | 164 µs | 5,890 µs | 4 rows |
| `id` | 100,000 rows | 64 values | LA | 52 groups | 12.6 lists/s | 73.5 ms | 76.0 ms | 77.0 ms | 79.0 ms | 104.0 ms | 112.5 ms | 64 rows |
| `id` | 100,000 rows | 64 values | L | 12,756 groups | 3,518.1 lists/s | 196 µs | 219 µs | 285 µs | 319 µs | 488 µs | 5,424 µs | 64 rows |
| `id` | 100,000 rows | 64 values | L2 | 12,573 groups | 3,464.7 lists/s | 199 µs | 225 µs | 286 µs | 321 µs | 484 µs | 6,550 µs | 64 rows |
| `id` | 100,000 rows | 64 values | D | 867 groups | 218.2 lists/s | 3,216 µs | 3,967 µs | 4,554 µs | 5,220 µs | 7,618 µs | 18.3 ms | 64 rows |
| `id` | 100,000 rows | 64 values | J | 15,248 groups | 4,420.1 lists/s | 151 µs | 168 µs | 231 µs | 255 µs | 425 µs | 2,288 µs | 64 rows |
| `id` | 100,000 rows | 1,024 values | LA | 4 groups | 0.9 lists/s | 1.11 s | 1.13 s | 1.13 s | 1.17 s | 1.17 s | 1.18 s | 1,024 rows |
| `id` | 100,000 rows | 1,024 values | L | 1,193 groups | 335.1 lists/s | 2,504 µs | 2,709 µs | 2,850 µs | 3,178 µs | 4,049 µs | 7,654 µs | 1,024 rows |
| `id` | 100,000 rows | 1,024 values | L2 | 1,221 groups | 342.2 lists/s | 2,499 µs | 2,688 µs | 2,817 µs | 3,122 µs | 4,002 µs | 9,427 µs | 1,024 rows |
| `id` | 100,000 rows | 1,024 values | D | 63 groups | 15.3 lists/s | 54.1 ms | 59.4 ms | 65.0 ms | 69.5 ms | 77.1 ms | 77.6 ms | 1,024 rows |
| `id` | 100,000 rows | 1,024 values | J | 1,535 groups | 441.4 lists/s | 1,973 µs | 2,109 µs | 2,160 µs | 2,308 µs | 3,323 µs | 6,778 µs | 1,024 rows |
| `id` | 100,000 rows | 4,096 values | LA | 2 groups | 0.2 lists/s | 4.20 s | 4.21 s | 4.21 s | 4.21 s | 4.21 s | 4.22 s | 4,096 rows |
| `id` | 100,000 rows | 4,096 values | L | 269 groups | 74.2 lists/s | 11.6 ms | 12.1 ms | 13.5 ms | 14.0 ms | 18.9 ms | 20.5 ms | 4,096 rows |
| `id` | 100,000 rows | 4,096 values | L2 | 269 groups | 74.8 lists/s | 11.5 ms | 12.1 ms | 12.8 ms | 14.7 ms | 17.2 ms | 18.6 ms | 4,096 rows |
| `id` | 100,000 rows | 4,096 values | D | 16 groups | 3.8 lists/s | 235.1 ms | 250.3 ms | 261.8 ms | 273.8 ms | 281.2 ms | 283.9 ms | 4,096 rows |
| `id` | 100,000 rows | 4,096 values | J | 403 groups | 116.3 lists/s | 7,675 µs | 7,959 µs | 8,158 µs | 8,653 µs | 11.8 ms | 15.9 ms | 4,096 rows |
| `k` | 1,000 rows | 1 value | LA | 50,826 groups | 13,653.4 lists/s | 51.6 µs | 57.3 µs | 69.9 µs | 78.9 µs | 161 µs | 1,233 µs | 1 rows |
| `k` | 1,000 rows | 1 value | L | 50,807 groups | 13,646.4 lists/s | 51.7 µs | 60.2 µs | 67.9 µs | 79.4 µs | 158 µs | 4,998 µs | 1 rows |
| `k` | 1,000 rows | 1 value | L2 | 51,838 groups | 13,932.9 lists/s | 51.6 µs | 57.2 µs | 67.7 µs | 78.4 µs | 154 µs | 1,306 µs | 1 rows |
| `k` | 1,000 rows | 1 value | D | 53,147 groups | 14,283.3 lists/s | 50.7 µs | 55.9 µs | 68.2 µs | 76.5 µs | 145 µs | 1,184 µs | 1 rows |
| `k` | 1,000 rows | 1 value | J | 42,004 groups | 12,341.0 lists/s | 56.5 µs | 60.8 µs | 70.7 µs | 98.0 µs | 160 µs | 3,939 µs | 1 rows |
| `k` | 1,000 rows | 4 values | LA | 18,816 groups | 4,896.2 lists/s | 149 µs | 165 µs | 204 µs | 224 µs | 347 µs | 4,550 µs | 4 rows |
| `k` | 1,000 rows | 4 values | L | 36,803 groups | 9,868.9 lists/s | 64.6 µs | 72.5 µs | 88.2 µs | 132 µs | 178 µs | 2,477 µs | 4 rows |
| `k` | 1,000 rows | 4 values | L2 | 36,938 groups | 9,927.5 lists/s | 65.3 µs | 74.8 µs | 90.3 µs | 124 µs | 185 µs | 3,322 µs | 4 rows |
| `k` | 1,000 rows | 4 values | D | 13,456 groups | 3,470.8 lists/s | 207 µs | 226 µs | 286 µs | 310 µs | 566 µs | 7,152 µs | 4 rows |
| `k` | 1,000 rows | 4 values | J | 34,232 groups | 9,995.2 lists/s | 64.9 µs | 71.5 µs | 88.5 µs | 127 µs | 190 µs | 4,013 µs | 4 rows |
| `k` | 1,000 rows | 64 values | LA | 3,703 groups | 960.4 lists/s | 853 µs | 948 µs | 999 µs | 1,077 µs | 1,634 µs | 5,927 µs | 64 rows |
| `k` | 1,000 rows | 64 values | L | 8,561 groups | 2,315.4 lists/s | 299 µs | 377 µs | 412 µs | 466 µs | 701 µs | 5,609 µs | 64 rows |
| `k` | 1,000 rows | 64 values | L2 | 8,609 groups | 2,326.9 lists/s | 302 µs | 382 µs | 411 µs | 459 µs | 671 µs | 1,209 µs | 64 rows |
| `k` | 1,000 rows | 64 values | D | 893 groups | 225.2 lists/s | 3,378 µs | 3,808 µs | 4,556 µs | 4,852 µs | 6,368 µs | 11.9 ms | 64 rows |
| `k` | 1,000 rows | 64 values | J | 8,936 groups | 2,477.1 lists/s | 270 µs | 351 µs | 382 µs | 434 µs | 630 µs | 4,358 µs | 64 rows |
| `k` | 1,000 rows | 1,024 values | LA | 457 groups | 118.5 lists/s | 6,948 µs | 7,318 µs | 7,798 µs | 8,790 µs | 13.8 ms | 14.7 ms | 1,000 rows |
| `k` | 1,000 rows | 1,024 values | L | 729 groups | 195.5 lists/s | 4,041 µs | 4,412 µs | 4,947 µs | 5,691 µs | 7,122 µs | 7,423 µs | 1,000 rows |
| `k` | 1,000 rows | 1,024 values | L2 | 749 groups | 201.0 lists/s | 4,020 µs | 4,369 µs | 4,781 µs | 5,521 µs | 7,197 µs | 11.2 ms | 1,000 rows |
| `k` | 1,000 rows | 1,024 values | D | 53 groups | 13.0 lists/s | 60.1 ms | 72.0 ms | 76.4 ms | 79.7 ms | 98.5 ms | 114.3 ms | 1,000 rows |
| `k` | 1,000 rows | 1,024 values | J | 829 groups | 224.5 lists/s | 3,576 µs | 3,852 µs | 4,224 µs | 4,819 µs | 6,603 µs | 10.5 ms | 1,000 rows |
| `k` | 1,000 rows | 4,096 values | LA | 112 groups | 28.9 lists/s | 27.2 ms | 28.9 ms | 32.2 ms | 39.9 ms | 51.6 ms | 53.0 ms | 1,000 rows |
| `k` | 1,000 rows | 4,096 values | L | 167 groups | 43.8 lists/s | 18.3 ms | 19.5 ms | 21.4 ms | 25.8 ms | 32.5 ms | 33.2 ms | 1,000 rows |
| `k` | 1,000 rows | 4,096 values | L2 | 168 groups | 44.3 lists/s | 18.0 ms | 19.4 ms | 20.9 ms | 24.8 ms | 32.1 ms | 32.5 ms | 1,000 rows |
| `k` | 1,000 rows | 4,096 values | D | 14 groups | 3.3 lists/s | 279.4 ms | 282.2 ms | 299.0 ms | 309.2 ms | 323.3 ms | 327.2 ms | 1,000 rows |
| `k` | 1,000 rows | 4,096 values | J | 206 groups | 55.1 lists/s | 14.0 ms | 15.3 ms | 16.9 ms | 20.8 ms | 25.5 ms | 27.4 ms | 1,000 rows |
| `k` | 100,000 rows | 1 value | LA | 47,660 groups | 12,771.6 lists/s | 52.3 µs | 63.6 µs | 73.4 µs | 82.2 µs | 172 µs | 5,753 µs | 1 rows |
| `k` | 100,000 rows | 1 value | L | 48,555 groups | 13,028.0 lists/s | 52.7 µs | 61.5 µs | 73.4 µs | 82.0 µs | 165 µs | 4,610 µs | 1 rows |
| `k` | 100,000 rows | 1 value | L2 | 46,939 groups | 12,571.5 lists/s | 52.7 µs | 63.3 µs | 74.5 µs | 83.0 µs | 182 µs | 4,388 µs | 1 rows |
| `k` | 100,000 rows | 1 value | D | 47,357 groups | 12,706.7 lists/s | 51.4 µs | 64.0 µs | 76.6 µs | 82.2 µs | 177 µs | 1,324 µs | 1 rows |
| `k` | 100,000 rows | 1 value | J | 37,851 groups | 11,092.4 lists/s | 55.7 µs | 66.3 µs | 79.2 µs | 102 µs | 218 µs | 5,378 µs | 1 rows |
| `k` | 100,000 rows | 4 values | LA | 308 groups | 77.0 lists/s | 9,672 µs | 10.7 ms | 12.3 ms | 15.3 ms | 17.8 ms | 21.1 ms | 4 rows |
| `k` | 100,000 rows | 4 values | L | 28,281 groups | 7,548.2 lists/s | 66.6 µs | 94.6 µs | 119 µs | 154 µs | 402 µs | 5,105 µs | 4 rows |
| `k` | 100,000 rows | 4 values | L2 | 31,910 groups | 8,530.3 lists/s | 67.4 µs | 89.7 µs | 108 µs | 138 µs | 233 µs | 2,822 µs | 4 rows |
| `k` | 100,000 rows | 4 values | D | 12,214 groups | 3,147.4 lists/s | 208 µs | 271 µs | 301 µs | 330 µs | 647 µs | 8,909 µs | 4 rows |
| `k` | 100,000 rows | 4 values | J | 31,096 groups | 8,997.8 lists/s | 66.6 µs | 85.5 µs | 96.9 µs | 132 µs | 220 µs | 5,768 µs | 4 rows |
| `k` | 100,000 rows | 64 values | LA | 44 groups | 10.8 lists/s | 74.3 ms | 84.2 ms | 89.2 ms | 99.4 ms | 122.3 ms | 130.9 ms | 64 rows |
| `k` | 100,000 rows | 64 values | L | 7,171 groups | 1,918.2 lists/s | 324 µs | 436 µs | 484 µs | 604 µs | 791 µs | 7,383 µs | 64 rows |
| `k` | 100,000 rows | 64 values | L2 | 7,005 groups | 1,869.8 lists/s | 325 µs | 442 µs | 512 µs | 617 µs | 793 µs | 5,420 µs | 64 rows |
| `k` | 100,000 rows | 64 values | D | 808 groups | 203.7 lists/s | 3,476 µs | 4,497 µs | 4,914 µs | 5,263 µs | 6,640 µs | 7,829 µs | 64 rows |
| `k` | 100,000 rows | 64 values | J | 8,068 groups | 2,218.0 lists/s | 274 µs | 367 µs | 414 µs | 535 µs | 719 µs | 2,773 µs | 64 rows |
| `k` | 100,000 rows | 1,024 values | LA | 4 groups | 0.8 lists/s | 1.29 s | 1.29 s | 1.29 s | 1.38 s | 1.38 s | 1.43 s | 1,024 rows |
| `k` | 100,000 rows | 1,024 values | L | 658 groups | 175.7 lists/s | 4,436 µs | 4,859 µs | 5,408 µs | 6,424 µs | 7,851 µs | 10.2 ms | 1,024 rows |
| `k` | 100,000 rows | 1,024 values | L2 | 663 groups | 177.2 lists/s | 4,396 µs | 4,753 µs | 5,244 µs | 6,595 µs | 7,873 µs | 10.4 ms | 1,024 rows |
| `k` | 100,000 rows | 1,024 values | D | 50 groups | 12.4 lists/s | 70.6 ms | 77.1 ms | 80.3 ms | 82.7 ms | 96.8 ms | 102.1 ms | 1,024 rows |
| `k` | 100,000 rows | 1,024 values | J | 711 groups | 191.3 lists/s | 3,980 µs | 4,298 µs | 4,871 µs | 6,100 µs | 8,020 µs | 10.4 ms | 1,024 rows |
| `k` | 100,000 rows | 4,096 values | LA | 2 groups | 0.2 lists/s | 5.49 s | 5.53 s | 5.53 s | 5.53 s | 5.53 s | 5.56 s | 4,096 rows |
| `k` | 100,000 rows | 4,096 values | L | 147 groups | 39.0 lists/s | 19.5 ms | 22.9 ms | 25.2 ms | 28.5 ms | 34.0 ms | 34.7 ms | 4,096 rows |
| `k` | 100,000 rows | 4,096 values | L2 | 143 groups | 37.8 lists/s | 19.9 ms | 23.0 ms | 26.4 ms | 29.6 ms | 35.4 ms | 37.5 ms | 4,096 rows |
| `k` | 100,000 rows | 4,096 values | D | 14 groups | 3.2 lists/s | 277.2 ms | 297.3 ms | 304.6 ms | 315.5 ms | 333.0 ms | 340.6 ms | 4,096 rows |
| `k` | 100,000 rows | 4,096 values | J | 175 groups | 47.0 lists/s | 15.5 ms | 18.3 ms | 20.9 ms | 24.0 ms | 32.7 ms | 36.4 ms | 4,096 rows |

1, 10 and 100 rows:

| column | rows | n | arm | groups | lists/s | p0 | p25 | p50 | p75 | p99 | max | rows/list |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `id` | 1 rows | 1 value | LA | 27,566 groups | 14,756.7 lists/s | 48.8 µs | 53.5 µs | 65.3 µs | 72.8 µs | 149 µs | 8,837 µs | 1 rows |
| `id` | 1 rows | 1 value | L | 27,857 groups | 14,868.3 lists/s | 48.9 µs | 56.0 µs | 63.7 µs | 72.2 µs | 146 µs | 5,769 µs | 1 rows |
| `id` | 1 rows | 1 value | L2 | 27,610 groups | 14,780.8 lists/s | 49.0 µs | 52.7 µs | 65.5 µs | 73.0 µs | 138 µs | 10.5 ms | 1 rows |
| `id` | 1 rows | 1 value | D | 28,283 groups | 15,185.5 lists/s | 47.6 µs | 52.6 µs | 64.9 µs | 71.4 µs | 136 µs | 1,515 µs | 1 rows |
| `id` | 1 rows | 1 value | J | 21,338 groups | 12,561.9 lists/s | 53.1 µs | 60.6 µs | 68.9 µs | 96.4 µs | 165 µs | 1,573 µs | 1 rows |
| `id` | 1 rows | 4 values | LA | 25,952 groups | 14,033.4 lists/s | 53.0 µs | 56.4 µs | 64.2 µs | 78.2 µs | 149 µs | 3,674 µs | 1 rows |
| `id` | 1 rows | 4 values | L | 25,758 groups | 13,900.0 lists/s | 52.9 µs | 56.6 µs | 63.4 µs | 80.8 µs | 150 µs | 4,136 µs | 1 rows |
| `id` | 1 rows | 4 values | L2 | 24,740 groups | 13,362.2 lists/s | 53.5 µs | 57.6 µs | 68.2 µs | 82.3 µs | 157 µs | 4,114 µs | 1 rows |
| `id` | 1 rows | 4 values | D | 7,764 groups | 3,999.2 lists/s | 186 µs | 201 µs | 250 µs | 273 µs | 478 µs | 3,185 µs | 1 rows |
| `id` | 1 rows | 4 values | J | 22,407 groups | 13,399.0 lists/s | 55.0 µs | 58.1 µs | 64.5 µs | 87.0 µs | 152 µs | 2,018 µs | 1 rows |
| `id` | 1 rows | 64 values | LA | 13,275 groups | 7,794.2 lists/s | 90.6 µs | 96.9 µs | 118 µs | 154 µs | 236 µs | 2,075 µs | 1 rows |
| `id` | 1 rows | 64 values | L | 11,767 groups | 6,783.6 lists/s | 106 µs | 126 µs | 138 µs | 171 µs | 261 µs | 3,216 µs | 1 rows |
| `id` | 1 rows | 64 values | L2 | 11,014 groups | 6,336.6 lists/s | 105 µs | 122 µs | 159 µs | 175 µs | 280 µs | 4,782 µs | 1 rows |
| `id` | 1 rows | 64 values | D | 535 groups | 269.4 lists/s | 2,993 µs | 3,170 µs | 3,571 µs | 4,052 µs | 6,531 µs | 7,894 µs | 1 rows |
| `id` | 1 rows | 64 values | J | 14,065 groups | 8,917.9 lists/s | 75.9 µs | 81.7 µs | 108 µs | 139 µs | 207 µs | 3,321 µs | 1 rows |
| `id` | 1 rows | 1,024 values | LA | 1,634 groups | 1,076.9 lists/s | 745 µs | 841 µs | 884 µs | 965 µs | 1,369 µs | 7,191 µs | 1 rows |
| `id` | 1 rows | 1,024 values | L | 1,297 groups | 815.4 lists/s | 982 µs | 1,117 µs | 1,176 µs | 1,266 µs | 1,786 µs | 6,378 µs | 1 rows |
| `id` | 1 rows | 1,024 values | L2 | 1,331 groups | 840.1 lists/s | 1,000 µs | 1,102 µs | 1,155 µs | 1,236 µs | 1,711 µs | 3,138 µs | 1 rows |
| `id` | 1 rows | 1,024 values | D | 33 groups | 15.8 lists/s | 52.8 ms | 59.6 ms | 63.2 ms | 67.0 ms | 71.6 ms | 78.5 ms | 1 rows |
| `id` | 1 rows | 1,024 values | J | 2,304 groups | 1,784.5 lists/s | 413 µs | 470 µs | 510 µs | 578 µs | 1,340 µs | 6,084 µs | 1 rows |
| `id` | 1 rows | 4,096 values | LA | 276 groups | 166.1 lists/s | 4,908 µs | 5,203 µs | 5,606 µs | 6,649 µs | 8,318 µs | 9,480 µs | 1 rows |
| `id` | 1 rows | 4,096 values | L | 239 groups | 140.3 lists/s | 5,907 µs | 6,276 µs | 6,811 µs | 7,953 µs | 9,612 µs | 9,887 µs | 1 rows |
| `id` | 1 rows | 4,096 values | L2 | 242 groups | 141.9 lists/s | 5,885 µs | 6,250 µs | 6,616 µs | 7,666 µs | 9,531 µs | 12.2 ms | 1 rows |
| `id` | 1 rows | 4,096 values | D | 8 groups | 3.8 lists/s | 261.0 ms | 261.4 ms | 265.0 ms | 268.1 ms | 269.9 ms | 272.3 ms | 1 rows |
| `id` | 1 rows | 4,096 values | J | 604 groups | 485.8 lists/s | 1,493 µs | 1,646 µs | 1,854 µs | 2,516 µs | 2,922 µs | 6,966 µs | 1 rows |
| `id` | 10 rows | 1 value | LA | 34,713 groups | 18,526.6 lists/s | 48.7 µs | 50.9 µs | 51.7 µs | 54.0 µs | 81.6 µs | 781 µs | 1 rows |
| `id` | 10 rows | 1 value | L | 34,376 groups | 18,333.3 lists/s | 48.7 µs | 51.1 µs | 52.2 µs | 54.9 µs | 78.5 µs | 515 µs | 1 rows |
| `id` | 10 rows | 1 value | L2 | 32,393 groups | 17,330.7 lists/s | 48.8 µs | 51.2 µs | 53.5 µs | 62.9 µs | 88.2 µs | 1,872 µs | 1 rows |
| `id` | 10 rows | 1 value | D | 34,315 groups | 18,381.8 lists/s | 47.9 µs | 50.1 µs | 51.2 µs | 56.3 µs | 76.9 µs | 363 µs | 1 rows |
| `id` | 10 rows | 1 value | J | 25,166 groups | 15,055.7 lists/s | 52.6 µs | 56.1 µs | 59.5 µs | 78.1 µs | 102 µs | 414 µs | 1 rows |
| `id` | 10 rows | 4 values | LA | 25,029 groups | 13,442.2 lists/s | 57.5 µs | 60.4 µs | 67.7 µs | 85.2 µs | 131 µs | 6,438 µs | 4 rows |
| `id` | 10 rows | 4 values | L | 27,891 groups | 15,002.0 lists/s | 56.6 µs | 59.8 µs | 61.0 µs | 65.3 µs | 107 µs | 4,091 µs | 4 rows |
| `id` | 10 rows | 4 values | L2 | 28,598 groups | 15,399.3 lists/s | 56.9 µs | 59.7 µs | 60.8 µs | 64.7 µs | 105 µs | 879 µs | 4 rows |
| `id` | 10 rows | 4 values | D | 9,287 groups | 4,770.9 lists/s | 193 µs | 201 µs | 204 µs | 210 µs | 280 µs | 721 µs | 4 rows |
| `id` | 10 rows | 4 values | J | 25,711 groups | 15,493.4 lists/s | 58.2 µs | 60.7 µs | 62.0 µs | 64.7 µs | 99.0 µs | 952 µs | 4 rows |
| `id` | 10 rows | 64 values | LA | 13,426 groups | 7,728.7 lists/s | 104 µs | 109 µs | 127 µs | 146 µs | 193 µs | 2,430 µs | 10 rows |
| `id` | 10 rows | 64 values | L | 12,507 groups | 7,094.7 lists/s | 116 µs | 122 µs | 126 µs | 159 µs | 217 µs | 886 µs | 10 rows |
| `id` | 10 rows | 64 values | L2 | 11,667 groups | 6,605.8 lists/s | 117 µs | 152 µs | 156 µs | 161 µs | 236 µs | 948 µs | 10 rows |
| `id` | 10 rows | 64 values | D | 496 groups | 249.4 lists/s | 3,034 µs | 3,642 µs | 4,185 µs | 4,217 µs | 4,511 µs | 4,873 µs | 10 rows |
| `id` | 10 rows | 64 values | J | 12,132 groups | 7,427.2 lists/s | 87.9 µs | 124 µs | 128 µs | 160 µs | 183 µs | 567 µs | 10 rows |
| `id` | 10 rows | 1,024 values | LA | 1,702 groups | 1,109.3 lists/s | 776 µs | 877 µs | 895 µs | 919 µs | 1,077 µs | 1,533 µs | 10 rows |
| `id` | 10 rows | 1,024 values | L | 1,425 groups | 888.0 lists/s | 993 µs | 1,101 µs | 1,121 µs | 1,144 µs | 1,244 µs | 1,534 µs | 10 rows |
| `id` | 10 rows | 1,024 values | L2 | 1,427 groups | 887.1 lists/s | 1,011 µs | 1,102 µs | 1,119 µs | 1,138 µs | 1,354 µs | 1,681 µs | 10 rows |
| `id` | 10 rows | 1,024 values | D | 33 groups | 16.2 lists/s | 49.1 ms | 56.6 ms | 65.1 ms | 66.0 ms | 69.1 ms | 69.5 ms | 10 rows |
| `id` | 10 rows | 1,024 values | J | 2,583 groups | 1,989.1 lists/s | 454 µs | 484 µs | 493 µs | 510 µs | 606 µs | 756 µs | 10 rows |
| `id` | 10 rows | 4,096 values | LA | 322 groups | 192.4 lists/s | 5,033 µs | 5,148 µs | 5,183 µs | 5,239 µs | 5,475 µs | 5,751 µs | 10 rows |
| `id` | 10 rows | 4,096 values | L | 279 groups | 162.5 lists/s | 5,914 µs | 6,094 µs | 6,151 µs | 6,202 µs | 6,447 µs | 6,758 µs | 10 rows |
| `id` | 10 rows | 4,096 values | L2 | 282 groups | 164.5 lists/s | 5,887 µs | 6,024 µs | 6,067 µs | 6,111 µs | 6,331 µs | 7,573 µs | 10 rows |
| `id` | 10 rows | 4,096 values | D | 9 groups | 4.0 lists/s | 231.8 ms | 243.1 ms | 250.2 ms | 259.8 ms | 261.4 ms | 261.9 ms | 10 rows |
| `id` | 10 rows | 4,096 values | J | 756 groups | 612.3 lists/s | 1,574 µs | 1,608 µs | 1,624 µs | 1,646 µs | 1,776 µs | 2,489 µs | 10 rows |
| `id` | 100 rows | 1 value | LA | 35,096 groups | 18,768.0 lists/s | 48.9 µs | 51.1 µs | 51.9 µs | 53.8 µs | 68.3 µs | 1,756 µs | 1 rows |
| `id` | 100 rows | 1 value | L | 33,607 groups | 17,960.8 lists/s | 49.2 µs | 51.3 µs | 52.3 µs | 55.2 µs | 84.9 µs | 4,136 µs | 1 rows |
| `id` | 100 rows | 1 value | L2 | 34,779 groups | 18,577.3 lists/s | 48.6 µs | 51.3 µs | 52.3 µs | 54.5 µs | 70.3 µs | 1,959 µs | 1 rows |
| `id` | 100 rows | 1 value | D | 35,462 groups | 18,948.3 lists/s | 47.8 µs | 50.3 µs | 51.1 µs | 53.0 µs | 70.7 µs | 1,948 µs | 1 rows |
| `id` | 100 rows | 1 value | J | 25,792 groups | 15,443.6 lists/s | 53.2 µs | 55.8 µs | 57.8 µs | 77.5 µs | 101 µs | 325 µs | 1 rows |
| `id` | 100 rows | 4 values | LA | 25,652 groups | 13,715.4 lists/s | 65.5 µs | 68.6 µs | 69.9 µs | 73.0 µs | 114 µs | 2,156 µs | 4 rows |
| `id` | 100 rows | 4 values | L | 29,609 groups | 15,980.9 lists/s | 57.2 µs | 59.7 µs | 60.6 µs | 62.9 µs | 93.5 µs | 396 µs | 4 rows |
| `id` | 100 rows | 4 values | L2 | 29,534 groups | 15,945.9 lists/s | 57.5 µs | 60.0 µs | 61.0 µs | 63.4 µs | 80.8 µs | 446 µs | 4 rows |
| `id` | 100 rows | 4 values | D | 8,968 groups | 4,616.6 lists/s | 194 µs | 205 µs | 209 µs | 215 µs | 318 µs | 1,101 µs | 4 rows |
| `id` | 100 rows | 4 values | J | 26,018 groups | 15,711.6 lists/s | 58.1 µs | 60.8 µs | 61.9 µs | 64.3 µs | 81.8 µs | 1,145 µs | 4 rows |
| `id` | 100 rows | 64 values | LA | 7,991 groups | 4,327.2 lists/s | 199 µs | 207 µs | 215 µs | 252 µs | 356 µs | 828 µs | 64 rows |
| `id` | 100 rows | 64 values | L | 9,131 groups | 4,998.3 lists/s | 172 µs | 182 µs | 187 µs | 223 µs | 291 µs | 695 µs | 64 rows |
| `id` | 100 rows | 64 values | L2 | 8,794 groups | 4,808.4 lists/s | 172 µs | 183 µs | 188 µs | 227 µs | 373 µs | 1,435 µs | 64 rows |
| `id` | 100 rows | 64 values | D | 596 groups | 299.9 lists/s | 3,165 µs | 3,236 µs | 3,268 µs | 3,307 µs | 4,171 µs | 4,930 µs | 64 rows |
| `id` | 100 rows | 64 values | J | 9,745 groups | 5,652.6 lists/s | 144 µs | 155 µs | 188 µs | 194 µs | 242 µs | 1,284 µs | 64 rows |
| `id` | 100 rows | 1,024 values | LA | 1,125 groups | 665.0 lists/s | 1,316 µs | 1,439 µs | 1,495 µs | 1,557 µs | 1,807 µs | 2,214 µs | 100 rows |
| `id` | 100 rows | 1,024 values | L | 1,279 groups | 775.6 lists/s | 1,138 µs | 1,225 µs | 1,276 µs | 1,344 µs | 1,527 µs | 2,109 µs | 100 rows |
| `id` | 100 rows | 1,024 values | L2 | 1,259 groups | 762.1 lists/s | 1,148 µs | 1,254 µs | 1,287 µs | 1,329 µs | 1,695 µs | 6,807 µs | 100 rows |
| `id` | 100 rows | 1,024 values | D | 40 groups | 19.9 lists/s | 49.2 ms | 49.5 ms | 49.8 ms | 50.0 ms | 54.8 ms | 58.2 ms | 100 rows |
| `id` | 100 rows | 1,024 values | J | 2,092 groups | 1,474.0 lists/s | 589 µs | 642 µs | 670 µs | 704 µs | 901 µs | 1,157 µs | 100 rows |
| `id` | 100 rows | 4,096 values | LA | 244 groups | 139.0 lists/s | 6,744 µs | 7,064 µs | 7,183 µs | 7,313 µs | 7,620 µs | 7,695 µs | 100 rows |
| `id` | 100 rows | 4,096 values | L | 268 groups | 155.1 lists/s | 6,156 µs | 6,324 µs | 6,390 µs | 6,471 µs | 8,410 µs | 9,374 µs | 100 rows |
| `id` | 100 rows | 4,096 values | L2 | 267 groups | 154.8 lists/s | 6,185 µs | 6,352 µs | 6,405 µs | 6,473 µs | 7,286 µs | 15.5 ms | 100 rows |
| `id` | 100 rows | 4,096 values | D | 12 groups | 5.0 lists/s | 195.3 ms | 197.2 ms | 197.7 ms | 199.1 ms | 199.9 ms | 201.1 ms | 100 rows |
| `id` | 100 rows | 4,096 values | J | 661 groups | 508.0 lists/s | 1,804 µs | 1,913 µs | 1,964 µs | 2,010 µs | 2,221 µs | 2,796 µs | 100 rows |
| `k` | 1 rows | 1 value | LA | 24,110 groups | 12,897.7 lists/s | 50.6 µs | 64.7 µs | 73.8 µs | 79.4 µs | 168 µs | 4,451 µs | 1 rows |
| `k` | 1 rows | 1 value | L | 24,200 groups | 12,942.5 lists/s | 50.8 µs | 63.2 µs | 73.6 µs | 80.0 µs | 174 µs | 958 µs | 1 rows |
| `k` | 1 rows | 1 value | L2 | 23,744 groups | 12,701.2 lists/s | 50.6 µs | 66.7 µs | 74.5 µs | 80.1 µs | 172 µs | 2,538 µs | 1 rows |
| `k` | 1 rows | 1 value | D | 25,381 groups | 13,585.0 lists/s | 49.9 µs | 62.7 µs | 70.4 µs | 76.2 µs | 160 µs | 1,179 µs | 1 rows |
| `k` | 1 rows | 1 value | J | 19,126 groups | 11,190.4 lists/s | 54.7 µs | 70.3 µs | 79.0 µs | 99.9 µs | 192 µs | 2,791 µs | 1 rows |
| `k` | 1 rows | 4 values | LA | 27,074 groups | 14,641.0 lists/s | 52.2 µs | 56.2 µs | 66.8 µs | 75.8 µs | 123 µs | 1,137 µs | 1 rows |
| `k` | 1 rows | 4 values | L | 23,150 groups | 12,412.4 lists/s | 55.4 µs | 64.1 µs | 74.3 µs | 88.5 µs | 180 µs | 3,726 µs | 1 rows |
| `k` | 1 rows | 4 values | L2 | 21,833 groups | 11,764.1 lists/s | 55.5 µs | 61.4 µs | 85.4 µs | 96.2 µs | 144 µs | 2,455 µs | 1 rows |
| `k` | 1 rows | 4 values | D | 8,764 groups | 4,509.4 lists/s | 190 µs | 198 µs | 206 µs | 241 µs | 374 µs | 1,608 µs | 1 rows |
| `k` | 1 rows | 4 values | J | 23,138 groups | 13,842.2 lists/s | 55.9 µs | 59.9 µs | 69.2 µs | 83.1 µs | 114 µs | 2,977 µs | 1 rows |
| `k` | 1 rows | 64 values | LA | 11,220 groups | 6,544.6 lists/s | 92.9 µs | 153 µs | 156 µs | 160 µs | 187 µs | 842 µs | 1 rows |
| `k` | 1 rows | 64 values | L | 11,787 groups | 6,718.9 lists/s | 114 µs | 149 µs | 153 µs | 158 µs | 208 µs | 909 µs | 1 rows |
| `k` | 1 rows | 64 values | L2 | 10,018 groups | 5,719.0 lists/s | 114 µs | 152 µs | 183 µs | 189 µs | 218 µs | 569 µs | 1 rows |
| `k` | 1 rows | 64 values | D | 628 groups | 315.9 lists/s | 3,011 µs | 3,078 µs | 3,104 µs | 3,140 µs | 3,979 µs | 5,374 µs | 1 rows |
| `k` | 1 rows | 64 values | J | 13,220 groups | 8,199.5 lists/s | 80.3 µs | 88.7 µs | 118 µs | 148 µs | 182 µs | 2,887 µs | 1 rows |
| `k` | 1 rows | 1,024 values | LA | 1,725 groups | 1,129.1 lists/s | 762 µs | 858 µs | 877 µs | 903 µs | 1,056 µs | 1,606 µs | 1 rows |
| `k` | 1 rows | 1,024 values | L | 1,305 groups | 796.7 lists/s | 1,119 µs | 1,228 µs | 1,245 µs | 1,268 µs | 1,451 µs | 1,887 µs | 1 rows |
| `k` | 1 rows | 1,024 values | L2 | 1,289 groups | 785.8 lists/s | 1,145 µs | 1,236 µs | 1,255 µs | 1,279 µs | 1,691 µs | 2,640 µs | 1 rows |
| `k` | 1 rows | 1,024 values | D | 33 groups | 16.4 lists/s | 49.5 ms | 58.6 ms | 58.9 ms | 67.5 ms | 68.2 ms | 68.4 ms | 1 rows |
| `k` | 1 rows | 1,024 values | J | 2,432 groups | 1,810.2 lists/s | 498 µs | 532 µs | 541 µs | 558 µs | 706 µs | 965 µs | 1 rows |
| `k` | 1 rows | 4,096 values | LA | 325 groups | 193.9 lists/s | 4,948 µs | 5,075 µs | 5,136 µs | 5,213 µs | 5,766 µs | 5,871 µs | 1 rows |
| `k` | 1 rows | 4,096 values | L | 261 groups | 150.2 lists/s | 6,405 µs | 6,558 µs | 6,618 µs | 6,694 µs | 7,202 µs | 7,244 µs | 1 rows |
| `k` | 1 rows | 4,096 values | L2 | 261 groups | 149.8 lists/s | 6,419 µs | 6,570 µs | 6,630 µs | 6,701 µs | 7,594 µs | 9,062 µs | 1 rows |
| `k` | 1 rows | 4,096 values | D | 10 groups | 5.0 lists/s | 198.7 ms | 199.7 ms | 199.8 ms | 200.3 ms | 202.4 ms | 203.7 ms | 1 rows |
| `k` | 1 rows | 4,096 values | J | 674 groups | 514.0 lists/s | 1,757 µs | 1,872 µs | 1,913 µs | 1,967 µs | 2,615 µs | 6,774 µs | 1 rows |
| `k` | 10 rows | 1 value | LA | 24,073 groups | 12,964.4 lists/s | 50.9 µs | 73.9 µs | 74.8 µs | 77.7 µs | 107 µs | 442 µs | 1 rows |
| `k` | 10 rows | 1 value | L | 26,411 groups | 14,185.7 lists/s | 51.1 µs | 64.0 µs | 65.8 µs | 76.8 µs | 109 µs | 360 µs | 1 rows |
| `k` | 10 rows | 1 value | L2 | 26,814 groups | 14,400.3 lists/s | 50.8 µs | 63.9 µs | 65.7 µs | 76.7 µs | 107 µs | 508 µs | 1 rows |
| `k` | 10 rows | 1 value | D | 29,725 groups | 15,949.5 lists/s | 49.4 µs | 62.1 µs | 63.0 µs | 64.8 µs | 84.7 µs | 549 µs | 1 rows |
| `k` | 10 rows | 1 value | J | 24,115 groups | 14,333.6 lists/s | 54.8 µs | 57.2 µs | 59.1 µs | 70.5 µs | 127 µs | 1,205 µs | 1 rows |
| `k` | 10 rows | 4 values | LA | 29,622 groups | 15,927.0 lists/s | 57.5 µs | 60.1 µs | 61.2 µs | 63.6 µs | 80.2 µs | 984 µs | 4 rows |
| `k` | 10 rows | 4 values | L | 28,167 groups | 15,106.2 lists/s | 60.6 µs | 63.1 µs | 64.2 µs | 66.9 µs | 97.0 µs | 330 µs | 4 rows |
| `k` | 10 rows | 4 values | L2 | 27,823 groups | 14,918.3 lists/s | 60.3 µs | 63.2 µs | 64.4 µs | 67.0 µs | 107 µs | 465 µs | 4 rows |
| `k` | 10 rows | 4 values | D | 9,084 groups | 4,663.0 lists/s | 198 µs | 208 µs | 211 µs | 217 µs | 252 µs | 1,337 µs | 4 rows |
| `k` | 10 rows | 4 values | J | 24,868 groups | 14,835.8 lists/s | 60.4 µs | 63.2 µs | 64.4 µs | 67.2 µs | 107 µs | 617 µs | 4 rows |
| `k` | 10 rows | 64 values | LA | 13,848 groups | 7,961.1 lists/s | 106 µs | 111 µs | 116 µs | 144 µs | 194 µs | 1,850 µs | 10 rows |
| `k` | 10 rows | 64 values | L | 12,402 groups | 6,971.0 lists/s | 132 µs | 136 µs | 139 µs | 144 µs | 219 µs | 1,420 µs | 10 rows |
| `k` | 10 rows | 64 values | L2 | 10,941 groups | 6,127.0 lists/s | 132 µs | 142 µs | 174 µs | 180 µs | 230 µs | 793 µs | 10 rows |
| `k` | 10 rows | 64 values | D | 503 groups | 252.8 lists/s | 3,075 µs | 3,229 µs | 4,294 µs | 4,329 µs | 4,737 µs | 4,904 µs | 10 rows |
| `k` | 10 rows | 64 values | J | 13,507 groups | 8,327.0 lists/s | 99.6 µs | 104 µs | 108 µs | 140 µs | 190 µs | 506 µs | 10 rows |
| `k` | 10 rows | 1,024 values | LA | 1,644 groups | 1,060.7 lists/s | 811 µs | 920 µs | 935 µs | 956 µs | 1,095 µs | 1,336 µs | 10 rows |
| `k` | 10 rows | 1,024 values | L | 1,230 groups | 737.9 lists/s | 1,242 µs | 1,334 µs | 1,349 µs | 1,368 µs | 1,488 µs | 1,990 µs | 10 rows |
| `k` | 10 rows | 1,024 values | L2 | 1,223 groups | 732.4 lists/s | 1,229 µs | 1,325 µs | 1,358 µs | 1,396 µs | 1,507 µs | 1,788 µs | 10 rows |
| `k` | 10 rows | 1,024 values | D | 33 groups | 16.2 lists/s | 49.9 ms | 55.7 ms | 64.2 ms | 67.6 ms | 69.2 ms | 69.8 ms | 10 rows |
| `k` | 10 rows | 1,024 values | J | 2,162 groups | 1,542.3 lists/s | 580 µs | 620 µs | 641 µs | 673 µs | 754 µs | 948 µs | 10 rows |
| `k` | 10 rows | 4,096 values | LA | 315 groups | 187.6 lists/s | 5,055 µs | 5,255 µs | 5,319 µs | 5,384 µs | 5,816 µs | 6,219 µs | 10 rows |
| `k` | 10 rows | 4,096 values | L | 249 groups | 142.5 lists/s | 6,770 µs | 6,943 µs | 6,990 µs | 7,067 µs | 7,420 µs | 7,573 µs | 10 rows |
| `k` | 10 rows | 4,096 values | L2 | 249 groups | 142.6 lists/s | 6,789 µs | 6,931 µs | 6,988 µs | 7,048 µs | 7,387 µs | 9,185 µs | 10 rows |
| `k` | 10 rows | 4,096 values | D | 11 groups | 5.0 lists/s | 195.7 ms | 198.5 ms | 199.3 ms | 199.6 ms | 202.4 ms | 203.6 ms | 10 rows |
| `k` | 10 rows | 4,096 values | J | 613 groups | 448.9 lists/s | 2,062 µs | 2,167 µs | 2,217 µs | 2,268 µs | 2,580 µs | 3,129 µs | 10 rows |
| `k` | 100 rows | 1 value | LA | 32,028 groups | 17,126.7 lists/s | 51.0 µs | 53.7 µs | 54.8 µs | 59.7 µs | 87.7 µs | 332 µs | 1 rows |
| `k` | 100 rows | 1 value | L | 28,362 groups | 15,181.5 lists/s | 51.0 µs | 53.9 µs | 65.2 µs | 76.6 µs | 107 µs | 562 µs | 1 rows |
| `k` | 100 rows | 1 value | L2 | 33,668 groups | 17,965.8 lists/s | 50.5 µs | 53.2 µs | 54.1 µs | 56.0 µs | 74.0 µs | 439 µs | 1 rows |
| `k` | 100 rows | 1 value | D | 30,728 groups | 16,465.2 lists/s | 49.9 µs | 52.7 µs | 63.0 µs | 65.2 µs | 93.5 µs | 1,130 µs | 1 rows |
| `k` | 100 rows | 1 value | J | 27,341 groups | 16,406.5 lists/s | 55.3 µs | 57.8 µs | 58.9 µs | 61.4 µs | 90.2 µs | 863 µs | 1 rows |
| `k` | 100 rows | 4 values | LA | 22,089 groups | 11,800.2 lists/s | 66.6 µs | 69.8 µs | 72.1 µs | 102 µs | 144 µs | 1,620 µs | 4 rows |
| `k` | 100 rows | 4 values | L | 25,939 groups | 13,904.2 lists/s | 62.0 µs | 65.6 µs | 67.0 µs | 70.3 µs | 113 µs | 928 µs | 4 rows |
| `k` | 100 rows | 4 values | L2 | 20,487 groups | 10,970.8 lists/s | 62.5 µs | 85.6 µs | 96.6 µs | 100 µs | 146 µs | 609 µs | 4 rows |
| `k` | 100 rows | 4 values | D | 7,559 groups | 3,884.2 lists/s | 204 µs | 254 µs | 258 µs | 266 µs | 467 µs | 2,828 µs | 4 rows |
| `k` | 100 rows | 4 values | J | 19,490 groups | 11,469.9 lists/s | 63.9 µs | 93.7 µs | 96.3 µs | 99.6 µs | 121 µs | 922 µs | 4 rows |
| `k` | 100 rows | 64 values | LA | 7,914 groups | 4,282.9 lists/s | 202 µs | 211 µs | 219 µs | 253 µs | 349 µs | 840 µs | 64 rows |
| `k` | 100 rows | 64 values | L | 7,057 groups | 3,786.3 lists/s | 232 µs | 245 µs | 251 µs | 268 µs | 394 µs | 602 µs | 64 rows |
| `k` | 100 rows | 64 values | L2 | 6,568 groups | 3,514.9 lists/s | 234 µs | 282 µs | 288 µs | 298 µs | 372 µs | 600 µs | 64 rows |
| `k` | 100 rows | 64 values | D | 544 groups | 273.2 lists/s | 3,308 µs | 3,388 µs | 3,438 µs | 4,089 µs | 4,906 µs | 5,213 µs | 64 rows |
| `k` | 100 rows | 64 values | J | 7,185 groups | 4,011.8 lists/s | 203 µs | 214 µs | 250 µs | 284 µs | 350 µs | 1,264 µs | 64 rows |
| `k` | 100 rows | 1,024 values | LA | 1,118 groups | 661.1 lists/s | 1,374 µs | 1,467 µs | 1,499 µs | 1,539 µs | 1,798 µs | 2,420 µs | 100 rows |
| `k` | 100 rows | 1,024 values | L | 740 groups | 412.9 lists/s | 2,249 µs | 2,366 µs | 2,394 µs | 2,446 µs | 2,780 µs | 3,907 µs | 100 rows |
| `k` | 100 rows | 1,024 values | L2 | 746 groups | 415.9 lists/s | 2,224 µs | 2,360 µs | 2,391 µs | 2,432 µs | 2,628 µs | 3,074 µs | 100 rows |
| `k` | 100 rows | 1,024 values | D | 34 groups | 16.4 lists/s | 51.0 ms | 60.5 ms | 60.9 ms | 62.5 ms | 71.3 ms | 71.6 ms | 100 rows |
| `k` | 100 rows | 1,024 values | J | 1,001 groups | 579.2 lists/s | 1,634 µs | 1,699 µs | 1,719 µs | 1,742 µs | 1,858 µs | 2,675 µs | 100 rows |
| `k` | 100 rows | 4,096 values | LA | 240 groups | 136.9 lists/s | 6,941 µs | 7,189 µs | 7,287 µs | 7,393 µs | 7,778 µs | 8,016 µs | 100 rows |
| `k` | 100 rows | 4,096 values | L | 171 groups | 93.2 lists/s | 10.4 ms | 10.6 ms | 10.7 ms | 10.8 ms | 11.7 ms | 12.0 ms | 100 rows |
| `k` | 100 rows | 4,096 values | L2 | 170 groups | 92.7 lists/s | 10.4 ms | 10.6 ms | 10.7 ms | 10.8 ms | 14.4 ms | 16.2 ms | 100 rows |
| `k` | 100 rows | 4,096 values | D | 10 groups | 4.6 lists/s | 203.1 ms | 206.1 ms | 221.0 ms | 224.3 ms | 239.8 ms | 267.6 ms | 100 rows |
| `k` | 100 rows | 4,096 values | J | 277 groups | 162.1 lists/s | 6,001 µs | 6,099 µs | 6,170 µs | 6,211 µs | 6,672 µs | 7,225 µs | 100 rows |

## Plans

`ANALYZE`'s reply at 100,000 rows, 4 values.

- `id`, 100,000 rows, n = 4, arm L: `SELECT * FROM cu_t WHERE id IN (9228, 89109, 49376, 11113)`
    - `analyze rows=4 class=JoinSelect steps=1 examined=4 pages=4 opens=1 pattern_id=0x37dac98f166831de fetch_id=0x69d4d7cd6d180d15`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 Lookup cu_t key_set=4`
    - `  filter bool (0:0.0 = 9228 OR 0:0.0 = 89109 OR 0:0.0 = 49376 OR 0:0.0 = 11113)`
    - `step 0 Lookup cu_t opens=1 examined=4 matched=4 sel=100% pages=4`
    - `DB time 0.13 ms = CPU 0.13`
- `id`, 100,000 rows, n = 4, arm LA: `SELECT * FROM cu_t WHERE id IN (9228, 89109, 49376, 11113)`
    - `analyze rows=4 class=JoinSelect steps=1 examined=100000 pages=705 opens=1 pattern_id=0x37dac98f166831de fetch_id=0x69d4d7cd6d180d15`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 Scan cu_t`
    - `  filter bool (0:0.0 = 9228 OR 0:0.0 = 89109 OR 0:0.0 = 49376 OR 0:0.0 = 11113)`
    - `step 0 Scan cu_t opens=1 examined=100000 matched=4 sel=0% pages=705`
    - `DB time 16.09 ms = CPU 16.09`
- `id`, 100,000 rows, n = 4, arm J: `SELECT t.id, t.k, t.a, t.b FROM cu_v_4 AS v JOIN cu_t AS t ON t.id = v.rid`
    - `analyze rows=4 class=JoinSelect steps=2 examined=8 pages=5 opens=5 pattern_id=0xc054b33f51ca54bc fetch_id=0x133730dc035e9b4`
    - `class=JoinSelect steps=2 hoisted=0`
    - `step 0 Scan cu_v_4 AS v`
    - `step 1 Probe cu_t AS t key=0:0.1`
    - `  filter 0:1.0 = 0:0.1`
    - `step 0 Scan cu_v_4 AS v opens=1 examined=4 matched=4 sel=100% pages=1`
    - `step 1 Probe cu_t AS t opens=4 examined=4 matched=4 sel=100% pages=4`
    - `DB time 0.14 ms = CPU 0.14`
- `id`, 100,000 rows, n = 4, arm D: `SELECT * FROM cu_t WHERE id = 77`
    - `analyze rows=1 class=PointSelect steps=1 examined=1 pages=1 opens=1 pattern_id=0x207dc3e32ec5053b fetch_id=0xd8b45cb18d55b324`
    - `class=PointSelect steps=1 hoisted=0`
    - `step 0 Lookup cu_t key=77`
    - `  filter 0:0.0 = 77`
    - `step 0 Lookup cu_t opens=1 examined=1 matched=1 sel=100% pages=1`
    - `DB time 0.04 ms = CPU 0.04`
- `k`, 100,000 rows, n = 4, arm L: `SELECT * FROM cu_t WHERE k IN (27685, 267328, 148129, 33340)`
    - `analyze rows=4 class=JoinSelect steps=1 examined=4 pages=8 opens=1 pattern_id=0x1310e3352f39eb6b fetch_id=0x6b650e973563bea`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 IndexProbe cu_t key_set=4`
    - `  filter bool (0:0.1 = 27685 OR 0:0.1 = 267328 OR 0:0.1 = 148129 OR 0:0.1 = 33340)`
    - `step 0 IndexProbe cu_t opens=1 examined=4 matched=4 sel=100% pages=8 index_scanned=4 index_resolved=4`
    - `DB time 0.10 ms = CPU 0.10`
- `k`, 100,000 rows, n = 4, arm LA: `SELECT * FROM cu_t WHERE k IN (27685, 267328, 148129, 33340)`
    - `analyze rows=4 class=JoinSelect steps=1 examined=100000 pages=705 opens=1 pattern_id=0x1310e3352f39eb6b fetch_id=0x6b650e973563bea`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 Scan cu_t`
    - `  filter bool (0:0.1 = 27685 OR 0:0.1 = 267328 OR 0:0.1 = 148129 OR 0:0.1 = 33340)`
    - `step 0 Scan cu_t opens=1 examined=100000 matched=4 sel=0% pages=705`
    - `DB time 11.26 ms = CPU 11.26`
- `k`, 100,000 rows, n = 4, arm J: `SELECT t.id, t.k, t.a, t.b FROM cu_v_4 AS v JOIN cu_t AS t ON t.k = v.rk`
    - `analyze rows=4 class=JoinSelect steps=2 examined=8 pages=9 opens=5 pattern_id=0xb569831e9ad879b0 fetch_id=0x6890c43fabf52fc8`
    - `class=JoinSelect steps=2 hoisted=0`
    - `step 0 Scan cu_v_4 AS v`
    - `step 1 IndexProbe cu_t AS t key=0:0.2`
    - `  filter 0:1.1 = 0:0.2`
    - `step 0 Scan cu_v_4 AS v opens=1 examined=4 matched=4 sel=100% pages=1`
    - `step 1 IndexProbe cu_t AS t opens=4 examined=4 matched=4 sel=100% pages=8 index_scanned=4 index_resolved=4`
    - `DB time 0.10 ms = CPU 0.10`
- `k`, 100,000 rows, n = 4, arm D: `SELECT * FROM cu_t WHERE k = 232`
    - `analyze rows=1 class=JoinSelect steps=1 examined=1 pages=2 opens=1 pattern_id=0xe94e9ed3346cd4a2 fetch_id=0xa647081d39bbe5b7`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 IndexProbe cu_t`
    - `  filter 0:0.1 = 232`
    - `step 0 IndexProbe cu_t opens=1 examined=1 matched=1 sel=100% pages=2 index_scanned=1 index_resolved=1`
    - `DB time 0.06 ms = CPU 0.06`

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
