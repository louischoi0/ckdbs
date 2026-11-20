# DD-S0 premise: stale index entries cost a probe only where they share its key run, and a unique insert 50 % more; a `REINDEX` stalls its relation's writers for its whole build

**Thesis.** On `worktree-dd-index-entry-purge`, with the engine at `054dbc5e` (`v3.4.17-1-g054dbc5e`, which has no index purge), seven cells priced a churned secondary index against a rebuilt one of the same history. **Two of the five verdict cells clear DD-Q0's 10 % bar by a wide margin and three do not.**

1. **A probe of a key whose run holds the surplus costs +65 % server CPU on the churned twin** (cell 1, a low-cardinality key, 25,000 rows returned: 48.1 ms churned against 29.2 ms rebuilt; the control pair differs by +0.2 ms). The churned tree is **not** larger: 424 pages against 420. The cost is the stale entries read, about 0.76 us each, not pages.
2. **A single-row `INSERT` into a unique index whose key values were deleted, purged and placed again costs +50 % server CPU on the churned twin** (cell 5: 84.6 us against 56.3 us; every one of 12 paired blocks is +19 to +39 us, the control pair's blocks span -16 to +9 us). The tree is 44 pages against 42. The mechanism was not isolated (section 5).
3. **BY's "no probe moves" holds for `UPDATE` churn at one level of surplus** (cell 2, a fresh-value point probe: churned minus rebuilt +0.38 us, inside the control's +4.6/-3.0 us), **and when the pool is smaller than the churned tree** (cell 6: pool of 1,622 frames, 93 % hit rate, still no difference). A single-row `UPDATE` of the indexed column (cell 4) and a covered column churned under a fixed key (cell 3, not a verdict cell) also sit inside the control.
4. **The only cost DD removes that is not about entries is real, and measured on a busy host:** a `REINDEX INDEX` at n = 100,000 held two single-row writers off the relation for **156-178 ms** (the whole build), no writer was refused (0 `TxnConflict` of 27,672 writer statements).
5. **Pages alone do not separate the twins at one level of surplus.** The churned tree is 2.0x the rebuilt one where the surplus arrives as appends (cells 2-4: new entries sort after the old, leaves divide), and **1.0x-1.05x where the surplus is inserted into leaves a `CREATE INDEX` left half full** (cells 1 and 5).

DD-Q0's rule as written (`instructions/v3.2.0/workorder-dd-index-entry-purge.md` section 4) gives **build**: cells 1 and 5 each cost at least 10 % more than the rebuilt twin beyond the control's spread. Section 6 states what that verdict rests on.

| Item | Value |
|---|---|
| Executed | 2026-11-20 UTC. Build 13:04-13:13 (a competing host, `-j6`); run 13:15:23-13:17:55 (152 s of cells); report 13:19 |
| Branch / worktree | `worktree-dd-index-entry-purge` (session worktree `dd-index-entry-purge`). The binary came from `git worktree add --detach` of `054dbc5e` in a scratch directory, removed afterwards; the session worktree's uncommitted engine edits are in no binary measured |
| Commit measured | `054dbc5e`, `git describe --tags` = `v3.4.17-1-g054dbc5e`, committed 2026-11-20 08:00:46 UTC; tree clean (detached checkout) |
| Binary provenance | `CMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`. Source binary mtime 13:13:26 UTC, after the commit, so it is the commit's engine. The run used a copy at `/home/louis/bench-runs/dd-s0/bin/kds_server`, sha256 `3df8801d61b6cbd8f9d9c6b16693c88b6de429069865bc8f527f2f5350587cbb` |
| Device | data files and WAL under `/home/louis/bench-runs/dd-s0/wd/` on `/dev/nvme0n1p1`, ext4 (`df -T`); the scratch `/tmp` is tmpfs on this host and held only the binary copy and the scripts |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536` except cell 6 (1,622), `log_level = warn`, cadence default; a fresh server and data file per cell; server pinned to CPU 2, driver to CPU 4 |
| Host | 8 vCPU, Linux 6.12.116. **Not quiet.** 1-minute load 17.2-20.1 throughout, and 15-23 processes matching `cc1plus\|cmake --build\|ctest` (other sessions' builds and suites) before every block of every cell (`competing` is recorded per block, minimum 15). The run could not wait for a quiet host: the whole of it fell inside other sessions' builds. Server run-queue wait was 40-470 us per statement. Every absolute number below is a busy-host number; the comparisons are interleaved and each carries its own control |
| Driver | scratch `dd_s0.py`, not in the tree (sha256 prefix `1e5ee73be430`); imports `tools/kwp.py`, `tools/bn_census.py`, `tools/sys_meta.py`, `tools/bb_overhead_benchmark.py` unmodified. Server CPU is the sum of `/proc/<pid>/task/*/schedstat` over the server's threads (`bn_census.sched_stat`) |
| Suite | Not executed: no engine code changed |
| Prior number | **Same shapes: none.** BY's close (`bench/v3.2.0/results-by-close-v3.2.0-119-g9da66f0e.md`) is this engine's last reading of the same question and is read against in section 4; it measured the surplus under keys no probe asked for and had no `UPDATE`-churn after-state. PostgreSQL floor: not measured for these shapes |

## 1. Method

Per cell, one fresh server holding three relations of one history, `x_a`, `x_b`, `x_c` (`id, v, w`, all `BTREE`, an index named `ix_a`, `ix_b`, `ix_c`). The churn is one statement over every row, so the surplus equals the live set (one level): each index holds 2n entries. Then `REINDEX INDEX ix_a`; `x_b` and `x_c` are never touched. **The control pair is `x_b` against `x_c`.** Statements are timed in 12 blocks per relation (3 repetitions x 4 blocks), the three relations taken in a rotating order inside each block; server CPU per statement is the schedstat delta over a block. A repetition's value is the mean of its 4 blocks; the control's spread is the range of `x_b - x_c` over the 3 repetitions. Entries and height are `SELECT * FROM sys.indexes` read before and after the `REINDEX`. **Pages** are not exposed per index: the rebuilt tree is the `pages_allocated` delta across the `REINDEX`, the churned one is the `pages_freed` delta after its reclaim (`SHOW META`, `allocation`), as BY did. Every statement's reply tag was checked: 0 refusals in every cell. `ANALYZE` confirmed `IndexProbe` on `x_a` in cells 1, 2, 3.

| cell | n (`--n-big` / `--n-small`) | history | measured statement | per block, x 12 blocks |
|---|---|---|---|---|
| 1 | 100,000 | `v = id mod 4`; `UPDATE SET v = 3 - v` | `SELECT * FROM x WHERE v = 0` (25,000 rows) | 6 |
| 2 | 100,000 | `v = id`; `UPDATE SET v = v + 10000000` | prepared `SELECT * WHERE v = ?`, 1 row | 1,000 |
| 3 | 10,000 | `v = id mod 100`, `COVERING (w)`; `UPDATE SET w = w + 10000000` | `SELECT * WHERE v = 5` (100 rows) | 40 |
| 4 | 10,000 | as cell 2 | prepared `UPDATE x SET v = ? WHERE id = ?` (fresh value) | 150 |
| 5 | 10,000 | `UNIQUE` on `v`; `DELETE` all, `PURGE` all, `INSERT` the same `v` under ids n+1..2n | `INSERT` of a row whose `v` was just deleted (the 150 `DELETE`s are untimed, before the block) | 150 |
| 6 | 100,000 | cell 2's data file, server restarted with 1,622 frames | cell 2's probe, 1,500 untimed warm-up probes then 1,500 timed per block | 1,500 |
| 7 | 100,000 | cell 2's data file, 65,536 frames | two writer connections `UPDATE x SET w = ? WHERE id = ?` (not the indexed column) in a loop on one relation; a third connection issues `REINDEX INDEX` after 0.5 s | one `REINDEX` per relation: `x_b`, `x_c` (churned), `x_a` (already rebuilt) |

Cut to fit the 20-minute stage budget (the build alone took 9.5 of them on this host): one surplus level, one run per cell, no `cores > 1`, no `group`/`strict`, no PostgreSQL twin, **no row-count sweep** (rule 9's 200 / 1K sweep is not run: the sizes are the order's, 100,000 and 10,000, and a cost's per-row or fixed nature is therefore read only from cell 1 against cell 2, which differ in shape), and no cold device read (see cell 6). Cell 7 has three samples, one per relation, not three repetitions of one state.

## 2. The verdict cells

Throughput is derived from the client's wall time per statement (`QPS = 1,000,000 / mean us`) and carries the busy host's noise; the cost the verdict reads is server CPU per statement, a cost and not a delay. "Churned" is the mean of `x_b` and `x_c`; "spread" is the range of `x_b - x_c` over the 3 repetitions.

| cell | x_a rebuilt CPU | x_b churned CPU | x_c churned CPU | churned / rebuilt | control spread (b - c) | b - a | churned >= 10 % beyond the spread |
|---|---|---|---|---|---|---|---|
| 1 low-cardinality probe | 29,236 us | 48,198 us | 47,989 us | 1.645 | -347 to +491 us | +18,963 us | **yes** |
| 2 fresh-value point probe | 64.11 us | 64.50 us | 63.52 us | 0.998 | -2.95 to +4.63 us | +0.38 us | no |
| 4 single-row `UPDATE` of the key | 51.19 us | 51.71 us | 50.20 us | 0.995 | +1.32 to +1.61 us | +0.52 us | no |
| 5 unique `INSERT` | 56.34 us | 84.81 us | 84.34 us | 1.501 | -1.97 to +3.40 us | +28.47 us | **yes** |
| 6 cell 2's probe, small pool | 58.91 us | 59.25 us | 60.90 us | 1.020 | -2.73 to -0.35 us | +0.34 us | no |
| 3 covered-column probe (not a verdict cell) | 236.08 us | 240.69 us | 245.21 us | 1.029 | -6.40 to -2.97 us | +4.61 us | no |

Derived wall throughput, `x_a` / `x_b` / `x_c`: cell 1 9 / 6 / 7 probes/s (the probes return 25,000 rows each); cell 2 3,174 / 2,846 / 2,810 statements/s; cell 4 6,417 / 6,324 / 6,416; cell 5 5,349 / 4,086 / 3,996; cell 6 3,227 / 3,251 / 3,140; cell 3 1,259 / 1,055 / 1,172. Only cells 1 and 5 have a difference larger than the b-to-c difference in the same row; cells 2, 3 and 6 have b and c differing from each other by as much as either does from a (the wall figures carry 120-470 us of run-queue wait a statement at load 19).

Client-observed latency per statement (distribution, not a matrix), us:

| cell / relation | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|
| 1 a rebuilt | 72 | 82,213 | 99,647 | 107,523 | 134,714 | 139,795 |
| 1 b churned | 72 | 114,070 | 140,905 | 155,421 | 186,216 | 214,335 |
| 1 c churned | 72 | 106,664 | 135,147 | 150,748 | 192,494 | 224,900 |
| 2 a | 12,000 | 65.3 | 84.8 | 94.0 | 1,296 | 5,297 |
| 2 b | 12,000 | 66.0 | 84.5 | 92.8 | 1,531 | 6,757 |
| 2 c | 12,000 | 67.3 | 84.5 | 92.9 | 1,787 | 6,699 |
| 4 a | 1,800 | 52.9 | 63.7 | 67.6 | 162 | 3,781 |
| 4 b | 1,800 | 55.2 | 64.4 | 68.1 | 188 | 3,172 |
| 4 c | 1,800 | 48.4 | 63.2 | 67.6 | 166 | 3,609 |
| 5 a | 1,800 | 55.8 | 66.6 | 72.8 | 576 | 2,745 |
| 5 b | 1,800 | 75.7 | 92.9 | 105.6 | 906 | 3,164 |
| 5 c | 1,800 | 74.8 | 91.6 | 102.8 | 945 | 3,379 |
| 6 a | 18,000 | 63.5 | 82.5 | 89.9 | 721 | 6,486 |
| 6 b | 18,000 | 64.0 | 82.4 | 90.0 | 516 | 5,995 |
| 6 c | 18,000 | 63.3 | 82.8 | 90.6 | 1,199 | 6,297 |
| 3 a | 480 | 243 | 307 | 331 | 4,138 | 8,736 |
| 3 b | 480 | 243 | 307 | 337 | 4,751 | 8,502 |
| 3 c | 480 | 249 | 318 | 344 | 4,280 | 9,554 |

The p0 and p25 columns carry the finding. In cells 1 and 5 the whole distribution moves, floor included (cell 5: p0 55.8 against 75.7 / 74.8 us, p25 66.6 against 92.9 / 91.6 us), so the added cost is fixed per statement, not a tail effect. In cells 2, 4 and 6 the three relations share p0 and p25 to within 1-2 us and differ only in the p95 and p99 tail, which is the host's (it differs between b and c as much as between either and a).

## 3. Structure and wait accounting

| cell | entries churned (a, b, c) | entries after `REINDEX` (x_a) | height churned / after | rebuilt pages | churned pages | churned / rebuilt | `REINDEX` time |
|---|---|---|---|---|---|---|---|
| 1 | 200,000 each | 100,000 | 3 / 3 | 420 | 424 | 1.01 | 280 ms |
| 2 | 200,000 each | 100,000 | 3 / 3 | 421 | 841 | 2.00 | 150 ms |
| 3 | 20,000 each | 10,000 | 2 / 2 | 40 | 80 | 2.00 | 24 ms |
| 4 | 20,000 each | 10,000 | 2 / 2 | 42 | 84 | 2.00 | 27 ms |
| 5 | 20,000 each | 10,000 | 2 / 2 | 42 | 44 | 1.05 | 108 ms |

Where the surplus fell in the sort order decides the page ratio. A `CREATE INDEX` leaves its leaves half full (420 pages for 100,000 entries of 16 bytes where a dense tree would need about 212). Cell 1's churn moves each row to a value whose run already exists, between entries already there, so the new entries fill the slack and the tree does not grow; cell 2's churn writes entries after every existing one (`v` rises above the old maximum), and each leaf divides. **The "the churned tree is at least twice the rebuilt one's pages" leg of DD-Q0's narrow rule holds for the append-shaped surplus and fails for the insert-shaped one**, and it is the insert-shaped one that costs a probe.

Wait accounting. The server's on-CPU share of the client-observed time per statement was 0.18-0.20 (cell 2, 6), 0.25-0.35 (3, 4, 5) and 0.27-0.32 (1) at this load; the rest is the client, the loopback round trip, and run-queue wait at load 19 (server run-queue 108-137 us a statement in cells 2 and 6, 40-80 us in 4 and 5, 330-470 us in 3, 42-70 ms in cell 1). There is no durability wait (`relaxed`) and no lock or conflict wait in cells 1-6 (single session). A write-statement/read split inside the engine is not measured by this run; the unit has no decomposition beyond the CPU share above.

## 4. Cell 6, cell 7, and the delta against BY

**Cell 6.** The boot floor is 384 frames (the server's refusal at 300 names it: 128 resident-class pages plus 256). The working set of one relation's probe is its heap plus its index: 609 + 421 = 1,030 pages rebuilt, 605 + 841 = 1,446 churned; the pool was set to 1,622 frames, which is the floor plus the midpoint of the two (384 + 1,238). It was pressured, not roomy: over the cell, 805,639 pool hits and 59,313 misses (a 93 % hit rate), 57,000 frames reclaimed. `pool_would_wait` stayed 0 and the data file was read from the host's page cache, so this is **a buffer-pool miss priced at a page-cache copy, not at a device read; a cold device read is not measured**. On that footing no twin differs (churned / rebuilt 1.020, b - a +0.34 us against a control of -2.73 to -0.35 us). The cell BY's section 8.2 named to run next shows no read-side saving from a smaller tree at a point probe with the pages in the host's cache.

**Cell 7.** A `REINDEX` at n = 100,000 ran 156 ms (`x_b`), 178 ms (`x_c`) and 158 ms (`x_a`, already rebuilt) with two writers on the same relation, and the longest writer statement in each run was the same figure within 0.2 ms: 155.9, 178.4 and 158.4 ms against a writers-only p50 of 0.065 ms and a p99 of 4.4-4.9 ms (busy host). **The longest stall is the build, entire**; 0 of 8,755 / 9,145 / 9,772 writer statements were refused. The build time is the same for a 200,000-entry churned tree and a 100,000-entry rebuilt one (156-178 ms against 158 ms), so under writers it is not entry-bound at this size.

**Delta against BY's close** (`v3.2.0-119-g9da66f0e`, a quiet host, every page resident): BY's churn was 3n surplus under keys no probe asked for and found rebuilt minus untouched at -0.48 to +0.14 us. This run's cell 2 (surplus under keys the probe asks for, `UPDATE` churn, 2n entries) is +0.38 us, inside its own control of -2.95 to +4.63 us: **the same result**. BY's REINDEX on 3n surplus gave 4.0x pages; this run's 2n gives 2.0x for appended surplus and 1.0x for inserted. BY's `REINDEX` build was 0.64 us a row (63-65 ms at 100,000 rows, a quiet host, no writers); this run's was 1.5 us a row (150 ms) on a host at load 20 with writers in cell 7: not a like-for-like delta, and no claim is made about the engine's build time from it. BY's finding 4, that a relation whose rows were updated cannot be rebuilt (`Corruption`), does not reproduce: cells 2, 3, 4 and 1 rebuilt after `UPDATE` churn, with no refusal.

## 5. What the run teaches

- **The stale entry's price is paid per entry read, and a probe reads only the run it asks for.** Cell 1's probe reads the 25,000 live and 25,000 stale entries of one value; each stale one costs about 0.76 us (19.0 ms over 25,000) in server CPU, (the order's section 1.2 B says each stale entry costs a descent; this run did not profile the split). Cell 2's probe reads one live and no stale entry, and costs nothing: a stale entry under another key is never visited. The expectation in the order's section 1.1, that BY's "no probe moves" was weak because its surplus sat under unasked keys, is **confirmed for a shared key and unconfirmed for a point probe**.
- **The tree's size is not the cost.** Cells 1 and 5 are the two that cost, and they are the two whose churned tree is the same size as the rebuilt one. A purge justified by space (the narrow rule's page leg) would have found no cell where bulk and cost coincide.
- **The unique check pays more than a probe does.** Cell 5 adds one stale entry to each key's run and costs +28 us (+50 %) a statement, about 37 times cell 1's per-entry cost. The data do not say why (this run did not profile it); a stale entry of a purged row meeting the check's per-row latest-state resolve is one candidate, a leaf-walk across the run another, and neither is established. The order's section 3 test (DD-R1) and the S1 dry run do not examine it. **This cell is the one to profile before DD-S1.**
- **Cell 1 is built to cost.** A probe that returns a quarter of a table is not an OLTP shape; the 65 % is real and the shape is what the order calls "a cost almost by construction". The count of rows a status-column probe returns decides how much of the statement the stale entries are: here they were 1 of every 2 entries read.
- **`REINDEX` is not a free substitute.** The 156-178 ms is what a writer waits behind the relation's `X`; it is on a loaded host, BY measured 63-65 ms on a quiet one without writers. A 100 ms stall threshold (DD-Q0's narrow leg) is therefore host-dependent at this size and this run's figure over it is a busy-host figure.

## 6. DD-Q0's verdict and what it rests on

| DD-Q0 leg | reading | met |
|---|---|---|
| **build**: a statement of cell 1, 2, 4, 5 or 6 costs >= 10 % more CPU churned beyond the control's spread | cell 1 +64.5 % (control range 2.9 % of the rebuilt cost wide), cell 5 +50.1 % (control range 9.6 % wide, still under a fifth of the effect); cells 2, 4, 6 not | **met by cells 1 and 5** |
| narrow: no cell reaches the bar, tree >= 2x pages, stall >= 100 ms or a refusal | a bar is reached, so not applicable (pages 1.0x-2.0x, stall 156-178 ms busy-host, no refusal) | n/a |
| do not build | not the reading | no |

By DD-Q0's own wording the answer is **build** (S1-S3; S4 offered). It rests on two cells with the conditions above: cell 1 is a construction (a probe returning 25,000 rows) and cell 5 has an unexplained size. **Items back to the operator:** (1) whether cell 1's shape, which clears the bar by construction, should carry the build verdict alone; cell 5 is the only other and its mechanism is open; (2) the 100 ms stall leg needs a quiet-host re-run to mean anything; (3) rule 9's row-count sweep was cut, so cell 5's +28 us per statement is not shown to be fixed or per-row-of-run; (4) DD-R1's and DD-R3's premise tests and the `FULL_PAGE_IMAGE` replay cell (the order's DD-S0 "Test" and "Read" items) were not part of this measurement and are not reported here.
