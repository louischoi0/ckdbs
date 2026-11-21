# DN-S0 premise: replacing a secondary-index resolve's clustered descent with a page fetch saves 3-7 % of a point read and 20-40 % of a 20- to 1,000-row index range

**Thesis.** On `worktree-dn-heap-rowid` with the engine at `34958393` (`v3.4.18-9-g34958393`), a spike that lets every resolve of a tuple key after its first fetch the one leaf page directly - the most a stored row address (DN's rowid) could save - cut server CPU by **0.9-2.1 us of 28 us (3-7 %) on a one-row secondary point read**, by **10.8-12.0 us of 48-57 us (21-22 %) on a 20-row index range** and by **302-326 us of 826-983 us (31-40 %) on a 1,000-row range**, at 10,000 and 100,000 rows. The point-read saving is of the order of the control's spread; the two range savings are well beyond it. Nothing here measures what DN costs (a wider index entry, the slot check of DN-R3, the work DD owes); this is the upper bound of the benefit leg only.

| field | value |
|---|---|
| date executed | 2026-11-20, 15:10-15:32 UTC (four runs, section 1) |
| branch / commit | `worktree-dn-heap-rowid` at `34958393`, `git describe --tags` = `v3.4.18-9-g34958393`; tree clean (`git status` empty before this file was written) |
| work order | `instructions/v3.2.0/workorder-dn-heap-rowid.md` section 5, DN-S0's measurement leg |
| build | Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF` (`CMAKE_BUILD_TYPE=Release` read from both `CMakeCache.txt`) |
| **A**, **A2** | `kds_server_A`, built from `git archive 34958393` (no `.git` in that tree); sha256 `63dfe5835dc6e11ac3090bad3bf1fe2b521841b0f1c3a42cdd9562d84dbc2f56`; source binary mtime 2026-11-20 15:10:05 UTC, after the commit's 15:03:54 UTC. **A2 is the same file started as a second server**: the in-run control |
| **B** | `kds_server_B`, the spike, sha256 `f435a0c97c1e00c2d318cd22788af56e40934bf5427f8565ca0ee2a0bde7ac63`; source binary mtime 15:06:43 UTC. One uncommitted change to `src/exec/step_vm.cpp`'s `RunIndexStep` phase 2: a `static thread_local std::unordered_map` from `(relation oid << 48) ^ tuple key` to `page << 16 \| slot`, filled by a key's first resolve; a later resolve of the key does `GetForRead(page)` and `AcceptTupleAt` instead of `BtreeLookup`. It covers `RunIndexStep`'s resolve only (a probe, a range and an IN-list key set through a secondary index); CV's ordered walks are not spiked. Never committed |
| binaries run from | copies in `/home/louis/dn-s0-run/`; every server was started from a copy |
| device | data files and WAL on `/dev/nvme0n1p1` (ext4), under `/home/louis/dn-s0-run`; **the scratch directory the binaries were built in is tmpfs, so no data file was put there** |
| server | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, one KWP connection per server, servers pinned to cores 4, 5, 6 (A, A2, B), driver to cores 0-3; fresh data file per server per size |
| host | 8 cores shared with other sessions' builds and test suites (section 1); not quiet in every block |
| PostgreSQL floor | not measured for these shapes |
| prior number for this shape | none exists (section 5): this file is the baseline |

## 1. Method, and what was cut

**Relation.** `t (id int64, v int64, w int64)` `BTREE`, `v` a random permutation of 1..N, `CREATE INDEX ix ON t (v)`. N = 1,000, 10,000 and 100,000 (rule 9; N is the driver's `--n`). `ANALYZE` of the first statement of every cell was read on A: the point read is `IndexProbe`, both ranges are `IndexRange` (`examined=20 index_resolved=20 pages=21` and `examined=1000 index_resolved=1000 pages=1001`), and the tuple-key read is `Lookup`. `BETWEEN` on the indexed column is an index range, so the `IN`-list fallback was not needed.

| cell | statement | rows | statements per timed set | N |
|---|---|---|---|---|
| 1 `pt` | `SELECT * FROM t WHERE v = <k>` | 1 | 1,000 distinct k | 1K, 10K, 100K |
| 2 `r20` | `... WHERE v BETWEEN <lo> AND <lo+19>` | 20 | 100 distinct lo | 1K, 10K, 100K |
| 3 `r1000` | `... WHERE v BETWEEN <lo> AND <lo+999>` | 1,000 | 40 distinct lo | 10K, 100K (cut at 1K: it is the whole table) |
| 4 `tk` | `SELECT * FROM t WHERE id = <k>` (tuple key, for scale) | 1 | 1,000 distinct k | 1K, 10K, 100K |

**Protocol.** Three servers run at once. Every cell: one untimed pass of its full statement set on every server (warms the pool and puts every probed key's location into B's map; the row counts of the three servers were asserted equal), then **rounds** of one timed block per server, the same count of passes in each (equal work; a block is about 0.6 s: 5-9 passes of 1,000 `pt`/`tk` statements, 43-52 passes of 100 `r20`, 6 passes of 40 `r1000`), the arm order rotated and reversed per round. Server CPU is the sum of `/proc/<pid>/task/*/schedstat` run time over the server's threads, divided by the statements in the block. Every CPU figure below is the **median over rounds**; the bracket after a difference is its min..max over rounds. **The noise floor is A2 - A**: same binary, same data, a different server. Per-statement client latency is wall time around one statement over the loopback with a Python KWP client, so it carries the client.

| run | UTC | what | rounds | host while it ran | used for |
|---|---|---|---|---|---|
| 1 | 15:13-15:14 | 1K, 10K, 100K | 5 | no `cc1plus`; another session's test suite on the cores, load 7-9 | cross-check only |
| 2 | 15:16-15:20 | 1K, 10K, 100K | 12 | another session's build, 3-6 `cc1plus`, load 7-9 | **1K rows** (the only 12-round run covering them) |
| 3 | 15:22:47-15:26 | 10K, 100K | 16 | 10K blocks: no `cc1plus`, load 5.7 falling to 2.3. 100K blocks: 6-8 `cc1plus` appeared mid-run | **10K rows**; its 100K rows are not used |
| 4 | 15:30:03-15:31:58 | 100K | 16 | no `cc1plus` throughout; load average read 7.5 falling to 3.0 (a lagging figure from the minutes before) | **100K rows** |

The same cell's A moves by up to 50 % from one run to another (`r20` at 10K: 48.5 us in run 3, 72.5 us in run 2) because of what else the host runs; **only a difference taken inside one run is read here**, never an absolute across runs. Time budget: the builds and the measurement took about 14 minutes of the 20; about 11 further minutes of wall time (15:07 start to 15:32 end) were spent waiting for other sessions' builds to end before a run started, and are outside the budget. Cut: `cores > 1`, `group`/`strict` (these are read-only statements: no commit, no fsync), a PostgreSQL twin, an IN-list cell, a cold device read (every page resident), a 1K `r1000`.

Reproduce: `python3 -I dn_s0_premise.py --binA <A> --binB <B> --workdir <dir on a block device> --out <json> --rounds 16 --block-seconds 0.6 [--n 10000,100000]`. The driver was written for this stage and left in the session scratchpad (`/tmp/claude-1000/-home-louis-ckdbs/4d599df8-17cd-4ba6-a34a-164ae0fdf6fb/scratchpad/dn_s0_premise.py`), **not** in `tools/` (the caller asked that nothing else be touched), so `bench/docs/` does not yet document it.

## 2. The finding: the descent is a share that grows with the rows resolved

The resolve is one clustered descent per entry. A range pays it per row; a point read pays it once. Server CPU per statement, medians. **Throughput is derived** as `1,000,000 / CPU us` (the server-CPU bound on a serial connection; a client's wall rate is lower).

| N | cell | rounds | A CPU us | A2 CPU us | B CPU us | A2 - A (noise floor) | B - A | descent share (B-A)/A | A derived stmts/s | B derived stmts/s |
|---|---|---|---|---|---|---|---|---|---|---|
| 10,000 (run 3) | `pt` | 16 | 28.80 | 27.12 | 27.69 | -0.90 [-5.7, +3.3] | -0.89 [-4.9, +5.9] | -3.1 % | 34,722 | 36,114 |
| 10,000 | `r20` | 16 | 48.47 | 47.34 | 36.77 | -0.91 [-10.1, +14.0] | **-10.84** [-24.1, +0.3] | **-22.4 %** | 20,631 | 27,195 |
| 10,000 | `r1000` | 16 | 826.44 | 824.58 | 497.54 | -0.87 [-39.4, +26.5] | **-326.45** [-362.8, -212.4] | **-39.5 %** | 1,210 | 2,010 |
| 10,000 | `tk` (scale) | 16 | 23.66 | 22.63 | 23.34 | -0.61 [-4.0, +0.9] | +0.02 [-3.9, +4.0] | +0.1 % | 42,265 | 42,845 |
| 100,000 (run 4) | `pt` | 16 | 28.64 | 29.67 | 26.67 | +0.51 [-3.5, +4.5] | -1.98 [-4.9, +2.2] | -6.9 % | 34,916 | 37,495 |
| 100,000 | `r20` | 16 | 56.78 | 49.15 | 42.05 | -5.37 [-17.1, +8.9] | **-12.02** [-25.0, -0.2] | **-21.2 %** | 17,613 | 23,781 |
| 100,000 | `r1000` | 16 | 982.50 | 985.63 | 674.54 | +1.39 [-31.7, +62.4] | **-302.23** [-368.3, -261.9] | **-30.8 %** | 1,018 | 1,482 |
| 100,000 | `tk` (scale) | 16 | 24.69 | 25.54 | 23.55 | +0.52 [-6.0, +3.8] | -0.75 [-11.8, +3.8] | -3.1 % | 40,502 | 42,463 |
| 1,000 (run 2, busy host) | `pt` | 12 | 39.42 | 38.42 | 37.98 | -0.12 [-11.8, +3.6] | -1.68 [-15.8, +8.4] | -4.3 % | 25,368 | 26,330 |
| 1,000 | `r20` | 12 | 70.39 | 67.39 | 62.48 | -4.81 [-10.6, +14.2] | -8.36 [-15.7, +2.4] | -11.9 % | 14,206 | 16,005 |
| 1,000 | `tk` (scale) | 12 | 34.12 | 33.87 | 31.37 | -0.62 [-4.3, +5.8] | -2.48 [-5.9, +3.8] | -7.3 % | 29,308 | 31,877 |

How to read it. A cell's B - A is a finding only where it clears the control and keeps its sign across rounds:

- **`r1000` is a finding at 10K and 100K.** B - A is -326 and -302 us, its worst round still -212 and -262 us, against a control whose median is within 1.4 us of zero (its per-round range, to +-250 us, is the host; B - A never changes sign). About 0.30-0.33 us per resolved row is the descent: 31-40 % of the statement.
- **`r20` is a finding at 10K and 100K**: -10.8 and -12.0 us, 21-22 % of the statement. The 100K control's median is -5.4 us (range -17 to +9), so B - A there is a little over twice the floor, and at 10K about twelve times it. Per row this is 0.54-0.60 us, more than `r1000`'s 0.30-0.33, which the spike's own hash map may explain (section 4).
- **`pt` is not a finding at 10K and borderline at 100K.** -0.9 us is inside the 10K control's own range; -2.0 us is a little more than the 100K control's range, whose median is +0.5, but B - A's own range reaches +2.2. The most the descent can be of a one-row secondary read is about 1-2 us of 28; a figure this small is not read finer.
- **`tk` (cell 4) is the scale and a control on the spike**: the tuple-key read is the same engine path in A and B, and B - A is +0.0, -0.8 and -2.5 us (10K, 100K, 1K), none beyond its control. **A secondary point read costs about 4 us more than a tuple-key read** (28.8 against 23.7 us at 10K, 28.6 against 24.7 at 100K): that is the index descent plus the resolve plus the filter, and the resolve's descent is at most half of it.
- **1K is indicative.** Run 2 had 3-6 compilations running; its control medians are within 5 us and its ranges reach +-15 us, comparable with the effects of `pt` and `tk`. `r20` (-8.4 us, -11.9 %) points the same way as 10K and 100K. Run 1 (no compilation, a test suite on the host, 5 rounds) read `r20` -10.2 us (-15.2 %), `pt` +2.1 us (+7.5 %) and `tk` -3.2 us (-8.6 %): the sign of `pt` and `tk` flips between runs, which is what a figure inside the floor does.

The sweep. `r20`'s share is 12-15 % at 1K, 22 % at 10K and 21 % at 100K; `r1000`'s 39 % and 31 % at 10K and 100K. The saved time per row is about the same from 10K to 100K (0.54 and 0.60 us in `r20`, 0.33 and 0.30 in `r1000`); `r1000`'s share falls because A's other per-row cost rises at 100K (826 against 983 us), not because the saved part moved. The data cannot separate "a deeper clustered tree" from "colder cache lines"; the tree's height at each size was not read.

## 3. Latency distributions

Client wall time around one statement (loopback, Python client and the server's reply; **not** a server cost), us. Same runs as section 2.

| N | cell | arm | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| 10,000 | `pt` | A | 160,000 | 38.5 | 43.6 | 49.6 | 58.1 | 68.2 |
| 10,000 | `pt` | A2 | 160,000 | 38.4 | 43.0 | 46.6 | 57.4 | 67.2 |
| 10,000 | `pt` | B | 160,000 | 37.9 | 42.4 | 45.9 | 57.6 | 75.8 |
| 10,000 | `r20` | A | 84,800 | 61.7 | 66.4 | 69.1 | 110.1 | 120.0 |
| 10,000 | `r20` | A2 | 84,800 | 61.8 | 66.5 | 68.7 | 109.9 | 120.7 |
| 10,000 | `r20` | B | 84,800 | 54.3 | 58.8 | 60.4 | 82.8 | 122.0 |
| 10,000 | `r1000` | A | 7,680 | 1,157.9 | 1,222.2 | 1,249.9 | 1,456.0 | 1,770.4 |
| 10,000 | `r1000` | A2 | 7,680 | 1,162.6 | 1,227.4 | 1,250.6 | 1,414.1 | 1,609.3 |
| 10,000 | `r1000` | B | 7,680 | 844.8 | 903.9 | 922.4 | 1,117.7 | 1,336.7 |
| 10,000 | `tk` | A | 192,000 | 31.0 | 38.8 | 39.9 | 50.7 | 58.9 |
| 10,000 | `tk` | A2 | 192,000 | 36.0 | 38.7 | 39.4 | 49.1 | 57.6 |
| 10,000 | `tk` | B | 192,000 | 36.6 | 38.9 | 39.8 | 50.1 | 62.5 |
| 100,000 | `pt` | A | 176,000 | 38.6 | 43.1 | 45.4 | 57.7 | 66.7 |
| 100,000 | `pt` | A2 | 176,000 | 38.4 | 43.3 | 46.1 | 58.1 | 67.9 |
| 100,000 | `pt` | B | 176,000 | 35.6 | 41.8 | 43.4 | 56.0 | 96.1 |
| 100,000 | `r20` | A | 120,000 | 63.4 | 70.9 | 98.8 | 115.5 | 133.0 |
| 100,000 | `r20` | A2 | 120,000 | 63.0 | 68.8 | 71.6 | 112.2 | 126.6 |
| 100,000 | `r20` | B | 120,000 | 54.7 | 60.6 | 64.2 | 92.2 | 136.5 |
| 100,000 | `r1000` | A | 6,400 | 1,325.1 | 1,396.7 | 1,421.2 | 1,583.1 | 1,862.4 |
| 100,000 | `r1000` | A2 | 6,400 | 1,309.7 | 1,399.3 | 1,425.8 | 1,599.8 | 1,977.5 |
| 100,000 | `r1000` | B | 6,400 | 933.0 | 1,053.4 | 1,095.2 | 1,330.8 | 1,514.1 |
| 100,000 | `tk` | A | 224,000 | 31.9 | 39.3 | 42.2 | 56.9 | 67.0 |
| 100,000 | `tk` | A2 | 224,000 | 31.8 | 39.6 | 44.7 | 54.3 | 66.9 |
| 100,000 | `tk` | B | 224,000 | 32.6 | 39.2 | 41.4 | 54.6 | 68.5 |
| 1,000 (run 2, busy) | `pt` | A / A2 / B | 60,000 each | 38.4 / 38.3 / 37.5 | 51.1 / 50.3 / 42.8 | 56.9 / 55.4 / 53.4 | 84.2 / 79.8 / 86.5 | 225.9 / 229.2 / 321.9 |
| 1,000 (run 2, busy) | `r20` | A / A2 / B | 60,000 each | 61.6 / 70.6 / 55.4 | 104.6 / 100.6 / 88.2 | 117.0 / 109.3 / 100.2 | 176.0 / 175.7 / 172.8 | 324.9 / 679.8 / 282.0 |
| 1,000 (run 2, busy) | `tk` | A / A2 / B | 108,000 each | 35.5 / 37.1 / 36.8 | 53.2 / 52.9 / 49.7 | 56.0 / 55.8 / 54.0 | 74.6 / 74.5 / 76.0 | 167.4 / 164.7 / 190.5 |

What the shape says. In `r20` and `r1000` the **whole body moves**: p0 and p25 drop with p50 (`r1000` at 10K: p0 1,158 to 845 us, p25 1,222 to 904 us), so the descent is a fixed part of every resolve, not a tail effect. In `pt` the floor does not move at 10K (p0 38.5 / 38.4 / 37.9) and moves about 3 us at 100K (38.6 / 38.4 / 35.6, p25 1.3-1.4 us lower); B's p99 is the highest of the three in both (75.8 and 96.1 us against 66-68), which a map warming into cache could explain but this run did not isolate. The 100K `r20` A row's p50 of 98.8 us against A2's 71.6 is the competing compilations that appeared in that part of run 3, not the engine; that run's 100K CPU figures are not used. Run 2's 1K rows were taken on a busy host (A2's `r20` p99 680 us), and their body is wider.

## 4. Waits, and what the spike leaves out

Rule 3's decomposition: these are read-only statements on one connection at `relaxed`, so the commit (fsync), write-statement and lock/conflict waits are zero by construction, and every page is resident, so no `io.page_read` wait occurs. What a statement is here is server CPU plus client and loopback time. At 100K the server CPU of a one-row secondary read is 28.6 us against a client p50 of 45.4 us (63 %); the remaining 17 us is the Python client, the frame round trip and run-queue time on a shared host, not split further.

What the spike is and is not, because the numbers are bounded by it:

- **It is an upper bound on the resolve's benefit only to the extent its lookup is cheaper than a stored address.** The spike pays a hash lookup in a thread-local `unordered_map` of up to 100,000 entries per resolve; a stored address costs nothing to find. Its saving is therefore, if anything, an under-statement of the descent. `r20`'s larger per-row saving (0.54-0.60 us) than `r1000`'s (0.30-0.33 us) is consistent with the map's cache misses growing with the number of distinct keys resolved (2,000 in `r20`, 40,000 in `r1000`) rather than with the engine; this was not tested.
- **It keeps the page fetch and the tuple accept.** A range of 1,000 rows still reads 1,000 pages (`pages=1001` in A's plan) and B's statement still costs 498-1,206 us. The spike measures the descent removed; the floor under a secondary resolve is one `GetForRead` and one `AcceptTupleAt` a row.
- **It prices none of DN's costs**: eight more bytes an entry (DN-Q3) and the fan-out they take, the slot check, DD's entry removal, a heap's insert path. It does not cover an `UPDATE`'s or `DELETE`'s locate, `ON CONFLICT`, a foreign-key check, a Cabin entry or a Waystone hint, the other consumers of an address named in the work order.
- **Every remembered location was valid by construction**: the relation was static between the warm pass and the timed blocks, so no split moved a row and no slot was checked against the wrong key. Under writes a stored location must be verified (DN-R3); that check's cost is not in B - A.

## 5. Delta against this engine's own prior number

None exists for these shapes: no results file under `bench/` carries a secondary point read or an index range of 20 or 1,000 rows over this relation as server CPU with the resolve separated. The nearest figure is `bench/v3.4.17/dd-s0-premise-v3.4.17-1-g054dbc5e.md` cell 2, a prepared `SELECT * WHERE v = ?` of one row at 100,000 rows, x_a 64.11 us server CPU at `054dbc5e` on a host at load 19; it is a different harness (prepared statement, a churned-then-rebuilt twin) and a different host load, and this run's 28.6 us is **not** to be set beside it. **This file is the baseline** the next run of these shapes is read against.

## 6. What the run teaches

- **The resolve's descent is a per-row cost of about 0.3-0.6 us, so it matters where a statement resolves many rows and barely where it resolves one.** A point read would save at most about 1-2 us of 28 (3-7 %, at or just beyond the control's spread); a 20-row range about 11-12 us of 48-57 (21-22 %); a 1,000-row range about 300 us of 830-980 (31-40 %). A stored address is a throughput argument for ranges through an index, not for point lookups.
- **A secondary point read costs about 4 us more than a tuple-key read** (28.6-28.8 against 23.7-24.7 us), so at most about half of what an entry adds over a tuple key is the descent DN would replace; the rest is untouched by it.
- **The per-row cost under a stored address is still large.** B's `r1000` costs 498 us at 10K and 675 us at 100K, 0.50-0.67 us a row for a page fetch and an accept. A secondary range's floor is that fetch, so a rowid removes at most a third of a long range; a resolve that visits a leaf once for many entries is a different lever the spike does not touch.
- **Where this meets the work order.** DN-Q0 (exploratory) asked whether DN may end at DN-S0. On the speed leg the run gives a data point each way and no verdict: the benefit is real and large for ranges and not distinguishable from noise for a point read. Whether it pays for DN's costs (DN-Q3's eight bytes an entry, DD, DN-D7's invariants) rests on the census, which this file does not contain.
- **No contradiction with a stated design expectation was found**; the premise under test (that a descent per resolve is a cost worth removing) is supported for ranges and weak for point reads.
