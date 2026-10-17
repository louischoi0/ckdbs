# BZ close: the hidden tuple key costs a btree relation nothing measurable, and a heap relation is 6 % cheaper to write and 7 to 17 % cheaper to scan than its btree twin

**Thesis.** On `worktree-by-heap-redefine`, with the engine at `18e7b511` (`v3.2.0-153-g18e7b511`, **B**, BZ's closing code; the later `31094d71` is docs only) against `e1333a77` (`v3.2.0-150-ge1333a77`, **A**, the commit BZ opened from), three findings:

1. **A btree `INSERT` and a btree point `SELECT` by tuple key cost B the same as A to within the noise floor.** Server CPU per single-row autocommit `INSERT`: B - A = -0.54 us literal / -0.40 us prepared (-2.2 % / -1.7 %); per point `SELECT`: +0.23 us literal / +0.22 us prepared (+0.8 % / +0.8 %). The in-run control, A' (A's bytes as a second server), differs from A by -0.30 / -0.22 us on the `INSERT` and +0.00 / +0.03 us on the `SELECT`, and a single paired run scatters with a standard deviation of 0.5 to 1.0 us. Client-visible throughput: `INSERT` 21,396 -> 21,622 statements/s literal and 21,406 -> 21,621 prepared; point `SELECT` 16,603 -> 16,533 literal and 17,203 -> 17,224 prepared. **No cell regresses beyond the noise floor**; the `INSERT` improvement is not claimed either (it is the size of the control's own offset).
2. **A heap relation (`CREATE TABLE h (v int64, w int64) HEAP`) writes 1.4 to 1.7 us (-5.8 % to -6.8 %) less server CPU per single-row `INSERT` than its btree twin** (`CREATE TABLE b (id int64, v int64, w int64)`, key issued by the engine), in 179 of the 180 paired runs of the six `INSERT` cells, and **scans 7 % to 17 % cheaper per statement** (`SELECT *`: 4.4 us at 200 rows, 25 us at 1,000, 311 us at 10,000; 156 ns per row against 187 ns at 10,000 rows). The scan comparison carries a confound named in section 3.
3. **A heap relation's hidden key is not on the scan's wire**: `SELECT *` from the heap returns 2 fields per row, the btree twin 3 (the declared `id`), so a part of finding 2's scan gap is delivery, not walk.

Server CPU is the sum of `/proc/<pid>/task/*/schedstat` over the server's threads (`tools/bn_census.py`'s accounting, imported unmodified); wall latency is the client's. The A/B driver is a scratch script, `bz_ab.py`, that imports `tools/bn_census.py`, `tools/kwp.py` and `tools/bb_overhead_benchmark.py` unmodified; it is not in the tree (it lives with the raw output in `/home/cdkbs/bench-runs/bz-close/`). `bench/docs/` stays closed (`bench/README.md`).

| Item | Value |
|---|---|
| Executed | 2026-10-17, A/B matrix 09:32-09:35 UTC, heap matrix 09:35-09:39 UTC (host clock; builds finished 08:11 by the same clock) |
| Branch / worktree | `worktree-by-heap-redefine` (session worktree, HEAD `31094d71`, clean). Binaries built from two detached checkouts, `bz-ab-A` at `e1333a77` and `bz-ab-B` at `18e7b511`, removed afterwards |
| Commits measured | **A** = `e1333a77` (`v3.2.0-150-ge1333a77`, committed 00:40:26 UTC), **B** = `18e7b511` (`v3.2.0-153-g18e7b511`, committed 08:08:55 UTC by the host clock). A' = A's bytes started as a second server (in-run control) |
| Binary provenance | `cmake -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server -j8`, built sequentially, finished before any measurement. Source binary mtimes: A 08:10:49, B 08:11:43 UTC (both newer than their commits, so each is its commit's engine). **A and A'** sha256 `72e38e02dfa172d25d179b5f77dadd9accd0426fbed525a3017106b8a3920303`; **B** `fd8ae8333e3ec9d9de968d43e0ae800144054ce92878731f898c35ec7f56afa8`. Every server started from the copies in `/home/cdkbs/bench-runs/bz-close/bin/` |
| Device | Data files and WAL under `/home/cdkbs/bench-runs/bz-close/wd/` on `/dev/root`, ext4 (`df -T`), not tmpfs |
| Build type | Release |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, checkpoint cadence at default; fresh server and data file per arm per run. Server pinned to CPU 2, driver to CPU 4, one server busy at a time (A/B arms side by side, a block on one at a time) |
| Host load | 8 vCPU, Linux 7.0.0-1014-azure. `pgrep -f "cc1plus\|cmake --build\|ctest"` found nothing before any of the 180 runs (`competing` = 0 in every record); the load average at the end of the A/B matrix read 2.02 / 0.81 / 0.41 (the first value is the matrix's own servers plus the tail of the builds), 1.32 at the end of the heap matrix. The one-minute watchdog polled `ab.prog` / `heap.prog` (the driver rewrites them per run) and saw no stall or orphan |
| Drivers | `tools/bn_census.py` (`sched_ns`, `Server`, `load`, `pct`, `must`), `tools/kwp.py`, `tools/bb_overhead_benchmark.py` (`host_state`), unmodified at `31094d71` |
| Suite | Not executed; this measurement changed no engine code |
| Prior number | **None for this shape on a v3.2.0 btree INSERT/point SELECT at this driver**: the last neighbouring files are `results-by-close-v3.2.0-119-g9da66f0e.md` (an `INSERT` into a relation with a secondary index, 25.9 us) and `results-bx-close-v3.2.0-62-g9bf559e2.md`; neither measures an unindexed `INSERT`, so the A arm here is the baseline the next run is read against. **Heap against btree:** no prior number (heap relations were refused `Unsupported` at A). PostgreSQL floor: not measured for these shapes |

## 1. What was run and what was cut

### 1.1 Cells 1 and 2: btree, A against B

Three servers per run (A, A', B), a fresh server and data file for each, side by side. Each holds `t` (`id int64, v int64, w int64`, `--rows` rows, `BTREE`, declared first-column tuple key, no secondary index) and an empty `i` (same columns, `BTREE`). Each cell is cut into 6 blocks of 333 statements taken round-robin, the first server of a block rotating by one per block and per run, so each server holds each position equally often (the known second-server stall of `bn_census` lands on all three alike). Each shape is run as a literal (an unnamed `C_PARSE` per execution, what every driver in `tools/` does) and as a prepared statement.

| shape | statement | count per cell per arm per run |
|---|---|---|
| INSERT | `INSERT INTO i VALUES (?, ?, ?)`, distinct tuple key named | 1,998 |
| point SELECT | `SELECT * FROM t WHERE id = ?`, key `1 + (q * 7919) mod rows`, exactly one row expected | 1,998 |

`--rows` is the row count of `t`: **200, 1,000 and 10,000** (rule 9). The `INSERT` does not touch `t`, so its three sizes are three repeats of one shape and the point `SELECT` does not scale with rows (a tuple key descent); both are shown at three sizes as the evidence. **30 runs per size**, 90 paired runs per cell; 179,820 statements per arm per cell. Every reply was checked against its tag (and the `SELECT` against one returned row): **0 refused**; each arm's `i` ends with 3,996 rows at every run.

### 1.2 Cell 3: heap against its btree twin, at B only

One fresh server per run (B's copy) holding `h (v int64, w int64) HEAP` and `b (id int64, v int64, w int64) BTREE`, each loaded with `--rows` rows, plus empty `hi` / `bi` of the same two shapes for the `INSERT`. The `INSERT` into `b` omits the key (`INSERT INTO bi (v, w) VALUES (?, ?)`), so both relations take an engine-issued key, the heap's hidden. Blocks alternate heap / btree on the one server, the order flipping per block and per run. `SELECT * FROM h` / `SELECT * FROM b` with no `WHERE` (no key lookup exists on a heap), 600 / 300 / 60 statements per arm per run at 200 / 1,000 / 10,000 rows, every reply checked to return exactly the row count. 30 runs per size; 0 refused.

### 1.3 What was cut

Cut for the one-hour budget: **more than one core and `strict`/`group` durability** (this is a per-statement CPU question; under `group` the log wait is 82-85 % of a write and would hide a 1 us difference); **a cold buffer pool** (the pool holds every page); **a heap or btree with a secondary index**; **a heap point lookup through a column** (no key lookup exists on a heap; a column predicate is a scan and is not the cell measured); **heap `UPDATE`/`DELETE`**; **a scan cell for the btree A/B** (BZ changed the heap, the btree walk is what cells 1-2 cover by point); **a width-matched scan twin** (section 3); **the 100,000-row extreme** for the scan. The 10,000-row scan is the ceiling reached here.

## 2. Cells 1 and 2: the btree is unchanged to within the noise floor

Server CPU per statement in us, median over runs; deltas are medians of the per-run paired differences. Throughput is derived (`1,000,000 / mean wall us`, serial driver), as rule 5a asks.

| shape | path | A | A' | B | B - A | % of A | A' - A (control) | B - A' | B above A | A QPS (derived) | B QPS (derived) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT | literal | 24.65 us | 24.39 us | 24.18 us | -0.54 us | -2.2 % | -0.30 us | -0.22 us | 13 / 90 runs | 21,396 /s | 21,622 /s |
| INSERT | prepared | 24.08 us | 23.91 us | 23.63 us | -0.40 us | -1.7 % | -0.22 us | -0.10 us | 21 / 90 | 21,406 /s | 21,621 /s |
| point SELECT | literal | 29.97 us | 29.76 us | 30.09 us | +0.23 us | +0.8 % | +0.00 us | +0.21 us | 52 / 90 | 16,603 /s | 16,533 /s |
| point SELECT | prepared | 28.96 us | 29.03 us | 28.96 us | +0.22 us | +0.8 % | +0.03 us | +0.14 us | 51 / 90 | 17,203 /s | 17,224 /s |

The noise floor is the control's offset plus the scatter: A' is the same bytes as A and sits -0.30 us from it on the `INSERT`, a start-position effect that the rotation spreads but does not remove, and a single paired run's B - A has a standard deviation of 0.52 / 0.78 us (`INSERT`) and 0.75 / 0.96 us (`SELECT`), a standard error of 0.05 to 0.10 us over 90 runs. Against that, **the `SELECT`'s +0.2 us (+0.8 %) is of the control's order, "above A" in 52 of 90 runs is a coin toss, and its throughput moves -0.4 % and +0.1 %**; the `INSERT`'s -0.4 to -0.5 us is -1.7 % to -2.2 % but only -0.1 to -0.2 us (below 1 %) beyond the control. CLA reports both as "no regression beyond noise", not as exactly zero: the claim holds to about 1 %.

By size (B - A, median server CPU us; the size column is a repeat for the `INSERT`):

| shape | path | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|---|
| INSERT | literal | -0.36 us (-1.5 %) | -0.44 us (-1.8 %) | -0.70 us (-2.8 %) |
| INSERT | prepared | -0.46 us (-1.9 %) | -0.30 us (-1.2 %) | -0.50 us (-2.1 %) |
| point SELECT | literal | -0.03 us (-0.1 %) | +0.05 us (+0.2 %) | +0.48 us (+1.7 %) |
| point SELECT | prepared | +0.06 us (+0.2 %) | -0.05 us (-0.2 %) | +0.33 us (+1.0 %) |

The one pattern a reader might lift out is the 10,000-row `SELECT` (+0.33 and +0.48 us, B above A in 21 and 22 of 30 runs). The 10,000-row control reads +0.22 and +0.08 us in the same cells, and the per-size standard error is about 0.15 us; CLA does not report it as a finding. Nothing in the 200 and 1,000-row cells points the same way.

Latency distribution, all sizes pooled, us (179,820 statements per row; the wall time of the whole client round trip):

| shape | path | arm | ops | p0 | p25 | p50 | p95 | p99 | mean | QPS (derived) |
|---|---|---|---|---|---|---|---|---|---|---|
| INSERT | literal | A | 179,820 | 35.0 us | 43.5 us | 44.2 us | 59.5 us | 77.9 us | 46.7 us | 21,396 /s |
| INSERT | literal | A' | 179,820 | 35.2 us | 43.2 us | 43.9 us | 59.0 us | 77.0 us | 46.6 us | 21,478 /s |
| INSERT | literal | B | 179,820 | 34.4 us | 43.1 us | 43.7 us | 58.9 us | 76.5 us | 46.2 us | 21,622 /s |
| INSERT | prepared | A | 179,820 | 33.7 us | 42.9 us | 43.5 us | 58.9 us | 76.4 us | 46.7 us | 21,406 /s |
| INSERT | prepared | A' | 179,820 | 33.6 us | 42.6 us | 43.2 us | 58.8 us | 76.9 us | 46.5 us | 21,498 /s |
| INSERT | prepared | B | 179,820 | 34.6 us | 42.5 us | 43.1 us | 58.4 us | 76.9 us | 46.3 us | 21,621 /s |
| point SELECT | literal | A | 179,820 | 44.1 us | 55.7 us | 57.1 us | 74.9 us | 89.0 us | 60.2 us | 16,603 /s |
| point SELECT | literal | A' | 179,820 | 51.1 us | 55.7 us | 57.2 us | 75.2 us | 90.0 us | 60.0 us | 16,661 /s |
| point SELECT | literal | B | 179,820 | 50.8 us | 56.0 us | 57.4 us | 75.8 us | 91.2 us | 60.5 us | 16,533 /s |
| point SELECT | prepared | A | 179,820 | 46.6 us | 54.4 us | 56.4 us | 74.4 us | 88.2 us | 58.1 us | 17,203 /s |
| point SELECT | prepared | A' | 179,820 | 49.7 us | 54.4 us | 56.5 us | 74.1 us | 87.1 us | 58.0 us | 17,233 /s |
| point SELECT | prepared | B | 179,820 | 49.7 us | 54.6 us | 56.7 us | 73.7 us | 86.3 us | 58.1 us | 17,224 /s |

Every percentile of A, A' and B lies within 1 us of the others, except p0 of the literal `SELECT`, where A's 44.1 us is a single fast statement that no other arm reaches (A' and B bottom at 51 us) and is not a path difference. The per-size percentile tables are in the raw output (`analysis.txt` beside the data) and show the same.

**Where the time goes (rule 3).** The measured unit is one serial round trip under `durability = relaxed`, so there is **no commit-sync wait** to account for (the log is not synced per statement) and no lock or conflict wait (one session). Of B's mean wall, the server's CPU is 24.2 of 46.2 us (52 %) for a literal `INSERT`, 23.6 of 46.3 us (51 %) prepared, 30.1 of 60.5 us (50 %) for a literal `SELECT` and 29.0 of 58.1 us (50 %) prepared; the driver thread's own CPU is 22.5 / 22.7 / 30.4 / 29.6 us, and the remainder is the socket hop. The two CPU columns overlap (the client handles one reply while the server writes the next frames), so they are not a partition. A 0.2 us delta is therefore 0.5 % of an `INSERT` as the client sees it, and invisible in the wall percentiles.

## 3. Cell 3: a heap against its btree twin, at B

Server CPU per statement, median of 30 paired runs per cell; the delta is the median of the per-run differences. Throughput derived from mean wall.

| shape | path | rows | heap CPU | btree CPU | heap - btree | % of btree | heap below btree | heap QPS | btree QPS |
|---|---|---|---|---|---|---|---|---|---|
| INSERT | literal | 200 | 22.76 us | 24.38 us | -1.51 us | -6.2 % | 30 / 30 runs | 22,327 /s | 21,472 /s |
| INSERT | literal | 1,000 | 22.91 us | 24.41 us | -1.60 us | -6.6 % | 30 / 30 | 22,189 /s | 21,429 /s |
| INSERT | literal | 10,000 | 22.86 us | 24.55 us | -1.66 us | -6.8 % | 30 / 30 | 22,307 /s | 21,200 /s |
| INSERT | prepared | 200 | 22.28 us | 23.72 us | -1.38 us | -5.8 % | 30 / 30 | 22,624 /s | 21,869 /s |
| INSERT | prepared | 1,000 | 22.30 us | 23.71 us | -1.39 us | -5.8 % | 29 / 30 | 22,690 /s | 21,964 /s |
| INSERT | prepared | 10,000 | 22.24 us | 23.76 us | -1.49 us | -6.3 % | 30 / 30 | 22,766 /s | 21,942 /s |
| `SELECT *` scan | literal | 200 | 56.80 us | 61.19 us | -4.39 us | -7.2 % | 30 / 30 | 3,646 /s | 2,754 /s |
| `SELECT *` scan | literal | 1,000 | 179.07 us | 204.91 us | -24.93 us | -12.2 % | 30 / 30 | 840 /s | 609 /s |
| `SELECT *` scan | literal | 10,000 | 1,561.04 us | 1,866.72 us | -311.36 us | -16.7 % | 30 / 30 | 87 /s | 62 /s |

(The `INSERT` is insensitive to the relation's size, as it should be: its three sizes are repeats, -5.8 % to -6.8 %. The heap's 1.4 to 1.7 us is far outside the noise floor of section 2, whose largest control offset is 0.3 us.)

Latency, pooled over the 30 runs of each size, us:

| shape | rows | rel | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| INSERT literal | 200 | heap | 59,940 | 38.0 us | 42.5 us | 43.0 us | 54.3 us | 66.1 us |
| INSERT literal | 200 | btree | 59,940 | 34.9 us | 43.9 us | 44.4 us | 56.8 us | 68.4 us |
| INSERT prepared | 200 | heap | 59,940 | 36.9 us | 41.5 us | 41.9 us | 53.3 us | 64.4 us |
| INSERT prepared | 200 | btree | 59,940 | 34.5 us | 42.8 us | 43.3 us | 55.2 us | 66.9 us |
| scan | 200 | heap | 18,000 | 259.3 us | 267.5 us | 272.0 us | 290.3 us | 310.0 us |
| scan | 200 | btree | 18,000 | 344.1 us | 356.1 us | 360.8 us | 381.4 us | 453.1 us |
| scan | 1,000 | heap | 9,000 | 1,131.2 us | 1,163.1 us | 1,173.4 us | 1,219.1 us | 1,453.9 us |
| scan | 1,000 | btree | 9,000 | 1,558.0 us | 1,599.2 us | 1,614.4 us | 1,685.2 us | 2,262.2 us |
| scan | 10,000 | heap | 1,800 | 10,708.9 us | 11,203.4 us | 11,293.0 us | 13,365.7 us | 13,673.2 us |
| scan | 10,000 | btree | 1,800 | 15,104.8 us | 15,683.8 us | 15,799.2 us | 17,951.6 us | 18,549.9 us |

(The INSERT rows are the 200-row repeat; the 1,000 and 10,000-row repeats read the same to within 1 us at every percentile: heap p50 43.0 / 42.9 us against btree 44.5 / 44.4 us.) Note that the heap's p0 is not lower than the btree's (38.0 against 34.9 us on the literal `INSERT`) while its p25 and p50 are 1.4 us lower: the heap's advantage is in the body of the distribution, the best case is a floor set by the round trip.

**The scan comparison is confounded by width.** `SELECT *` from the heap returns 2 fields per row (`v`, `w`; the hidden key is not described), from the btree 3 (`id`, `v`, `w`). The scan gap per row is 156 ns (heap) against 187 ns (btree) at 10,000 rows, 179 against 205 ns at 1,000 and 284 against 306 ns at 200; the fixed cost per statement is of the order of 40-45 us on both (200 rows). A width-matched twin (a btree of `id, v` against the heap of `v, w`, or the btree projecting two columns) was not run inside the budget, so the share of the 17 % gap that is the cell's third field, as against the walk, is **not measured**. The `INSERT` is not confounded this way (both statements carry two values and the btree's key is issued).

## 4. What this teaches about the engine

1. **BZ's heap redefinition is free to the btree.** The code change between A and B retires the heap's own chain, tail, sorted fill and relayout (`0205444b`) and gives a `HEAP` relation a btree with a hidden key. On the btree side that change leaves a single-row autocommit `INSERT` and a point `SELECT` unchanged to about 1 %, at 200, 1,000 and 10,000 rows. This answers `workorder-bz-heap-hidden-key.md` §3 ("what does not move") with its first data point: it holds for the cost of a statement.
2. **A heap relation is now a btree relation that skips the key.** Its `INSERT` is 6 % cheaper (the cause is not isolated: both relations take an engine-issued key and carry the same row size, so the difference lies in the heap's insert path, which was not profiled), and its scan is cheaper per row because it describes and delivers one fewer field; both are consistent with the heap being the same structure as its twin, not a cheaper one. The width confound in section 3 means the engine has **not** been shown to walk a heap faster than a btree; it has been shown to answer `SELECT *` faster, which is the cost the client sees.
3. **Nothing here approached a limit (rule 4b).** One core, `relaxed`, one session, a pool holding every page: the cells characterise the per-statement CPU of the two paths, not saturation or durability. The scan reaches 10,000 rows (1.6 to 1.9 ms per statement) and the `INSERT` and `SELECT` are bounded by the client round trip (about half of wall is the server). The strict-durability and multi-core behaviour of a heap relation is not covered, and under `group` or `strict` the commit wait would dominate the 1.5 us seen here.
4. **The A arm's relations are created `BTREE`; A refuses `HEAP`**, so cell 3 has no A side and no delta; this file is the baseline the next heap measurement is read against.
