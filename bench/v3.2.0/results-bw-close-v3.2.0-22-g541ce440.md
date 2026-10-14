# BW close: the every-row UPDATE costs 3-5 % more server CPU at `541ce440` than at `74b0a3dd`; the scan row, the point SELECT and the INSERT do not move beyond the spread

**Thesis.** On `worktree-bw-instant-add-drop-column`, with the engine at `541ce440` (`v3.2.0-22-g541ce440`, **B**, BW closed) against BW's opening plus the tuple-key rename merge, `74b0a3dd` (`v3.2.0-13-g74b0a3dd`, **A**), the only cell that moves outside the A-against-A' spread is the **every-row `UPDATE` of 1,000 rows: +30.2 µs of server CPU per statement on both paths (pooled median of 6 paired runs; +4.0 % literal, +3.8 % prepared; about 30 ns per updated row), B above A in 6 of 6 literal and 5 of 6 prepared runs, and in both server start orders** (B started first in u1: +23.7 / +34.2 µs; B started last in u2: +37.4 / +26.2 µs), against a typical A' - A gap of 8-10 µs and a largest of 27 / 17 µs. The scan row (`bn`, `wide-h`, 10,000 rows), the point `SELECT` and the single-row `INSERT` read B - A inside the spread: scan net row +0.04 / +0.17 ns on `bn` (literal / prepared, of 27.4 / 27.5 ns), +0.13 / +0.40 ns on `wide-h`; point `SELECT` +0.11 / +0.17 µs; `INSERT` +0.48 / +0.29 µs (+2.1 % / +1.3 %, the largest of the three and still inside the largest gap, section 3). This run is short (budget below), so the scan and `INSERT` deltas are "not resolved", not "zero". The cause of the `UPDATE` delta was not profiled: BW put a `has_dropped()` gate in every decode loop and routes `CellOf`/`MutableCellOf` through `cell_ends`, and an `UPDATE` (which since BN-R3 carries every unassigned cell) decodes and re-encodes the whole row, so it is the one statement that pays both per row; that is a reading of the diff, not a measurement.

Server CPU is the sum of `/proc/<pid>/task/*/schedstat`; wall is the client's. The driver flags are documented in the drivers' docstrings (`bench/README.md`: `bench/docs/` stays closed); the exact invocations are `archive/bw-close-v3.2.0-22-g541ce440/scripts/run.sh` and `run2.sh`.

| Item | Value |
|---|---|
| Executed | 2026-10-14, UTC. Builds 08:13:33-08:15:14; measurement 08:15:45-08:22:28 (phases s1, u1, s2, z, u2 in `progress.txt`). Whole task 08:12:53 to the commit of this file, inside the 20-minute budget |
| Worktree / branch | `bw-instant-add-drop-column`, branch `worktree-bw-instant-add-drop-column`; HEAD `541ce440`, tree clean at the start and at the end (`git status --short` empty) |
| Commits measured | **A** = `74b0a3dd1c7d26b1033b7b9a2c982d01e2aa7664`, `v3.2.0-13-g74b0a3dd`, committed 2026-10-14 06:09:42 UTC (BW's opening `750496b1` plus the "pk -> tuple key" merge, so A..B is BW's code). **B** = `541ce440cc48d139ad72f133e6e2da20f56dc17b`, `v3.2.0-22-g541ce440`, committed 08:00:14 UTC. A' = A's bytes started as a second server (the in-run control) |
| Binary provenance | Each built from `git archive <commit>` into `/home/cdkbs/bench-runs/bw-close/src-{A,B}` and `build-{A,B}`; copies in `bin/` hashed after the copy (`archive/.../binaries.txt`). **A and A'** sha256 `911b4b85f0fff3109611142f8c9de0bc775dcc82b91ffdd86bff3b047ba4f1ae`, source mtime 08:15:14.05 UTC. **B** `07628c94ed32f035afbbe3063b0db24fad173310640150e35933b7ae28f8bd26`, source mtime 08:15:13.73 UTC. Both newer than their commits; every server started from the copies |
| Build | `CMAKE_BUILD_TYPE=Release` (read from each `CMakeCache.txt`), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, `-j4` each, built together (about 1 min 40 s) and finished before any measurement started; gcc 13.3.0 |
| Host / device | 8 vCPU AMD EPYC 9V74, Linux 7.0.0-1014-azure. Data files and WAL under `/home/cdkbs/bench-runs/bw-close/wd-*/` on `/dev/root`, **ext4, not tmpfs** (`df -T`). Load average 0.3-2.2 at the start (one more reading, 1.0, after the last phase) |
| Pinning | One server process per arm, one busy at a time, on CPU 2; the driver on CPU 4; as BT-close |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, `scan_kernel` default (on); fresh server and data file per arm per run. `relaxed` means no fsync wait is in any measured statement |
| Drivers | `tools/bm_scan.py census` sha256 `1a1ca4ee...`, `tools/bn_census.py` `7d28608e...`, `tools/kwp.py` `99341280...`, read from this worktree. Gates: `--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1 --quiet-load 2`; zero windows came out unclean |
| Suite | Not executed in this task; the close's suite run is the parent's. No code was changed by this measurement |

## 1. What was run and what was cut

Three arms (A, A', B), a fresh server and data file per arm per run, the arms interleaved block by block (6 blocks per cell, order rotating), the two driver paths (literal, prepared) alternating inside a block. `bm_scan.py census --cells bn,wide-h` is relation `s` / `wd`, 10,000 rows; `rows-200` and `rows-1k` are `s200` / `s1k`. `bn_census.py --rows 1000 --only select-pk,insert,update-range`: `--rows` is the table's size and the number of rows the every-row `UPDATE r SET v = ? WHERE id >= 1` writes; `--ops 2,000` point statements, `--range-ops 120` every-row `UPDATE`s per path per arm per run. The only `INSERT` shape the drivers have is the single-row `INSERT INTO i VALUES (?, ?, ?)` (`bn_census`'s `insert`); **no multi-row `INSERT` driver exists and none was written** (it did not fit the budget). BW's `NormalizeInsertRows` therefore is measured on one row per statement only.

| group | driver / cells | rows | start order | runs | work per run per arm (equal across arms) |
|---|---|---|---|---|---|
| s1 | `bm_scan` `bn`, `wide-h` + point cells | 10,000 | A, A', B | 3 | 96 scans per path, 2,000 point `SELECT`s per path |
| s2 | same | 10,000 | B, A', A | 4 | same |
| z | `bm_scan` `rows-200`, `rows-1k` | 200; 1,000 | A, A', B | 2 | same |
| u1 | `bn_census` `select-pk`, `insert`, `update-range` | 1,000 | B, A', A | 3 | 2,000 point `SELECT`s, 2,000 `INSERT`s, 120 every-row `UPDATE`s per path |
| u2 | same | 1,000 | A, A', B | 3 | same |

**Cut for the 20-minute budget** (builds included): BT-close ran 13 scan runs and 15 `UPDATE` runs, with 12 blocks; this run has 7 scan runs and 6 `UPDATE` runs at 6 blocks and about a third of the statements per cell, so the typical gap is less well known (3-7 pairs) and any delta under about 1 ns per scan row or 1 µs per statement is unresolved. `rows-200`/`rows-1k` have 2 runs each; the `UPDATE` at 200 and 10,000 rows, `wide-h` at 1,000 rows and a profile of the `UPDATE` were not run. No PostgreSQL twin was run (floor not measured for this shape set; BT-close's 29.1 ns per scan row is the BM close's figure, not re-measured).

## 2. Headline: B - A against the A' - A spread

Server CPU, pooled over both start orders; B - A paired by run. "Outside" means the median B - A exceeds the largest |A' - A| of the same cell. Full tables, with the groups apart, are in section 6.

| cell | path | A | B | B - A | % of A | B above A | typical / largest A' - A | verdict |
|---|---|---|---|---|---|---|---|---|
| every-row `UPDATE`, 1,000 rows (us per statement) | literal | 752.1 | 770.2 | +30.2 | +4.0 % | 6 of 6 | 8.1 / 27.1 | **outside the spread** |
| every-row `UPDATE`, 1,000 rows | prepared | 786.0 | 814.7 | +30.2 | +3.8 % | 5 of 6 | 10.1 / 17.2 | **outside the spread** |
| `INSERT`, one row (us) | literal | 22.4 | 22.8 | +0.48 | +2.1 % | 5 of 6 | 0.40 / 1.07 | inside (0.45 of the largest gap) |
| `INSERT`, one row | prepared | 21.9 | 22.4 | +0.29 | +1.3 % | 5 of 6 | 0.48 / 0.76 | inside |
| point `SELECT` by key (us) | literal | 31.3 | 31.4 | +0.11 | +0.3 % | 4 of 6 | 0.40 / 0.85 | inside |
| point `SELECT` by key | prepared | 28.8 | 27.7 | +0.17 | +0.6 % | 4 of 6 | 0.46 / 2.53 | inside |
| `bn` net scan row (ns), 10,000 rows | literal | 27.40 | 27.60 | +0.04 | +0.2 % | 5 of 7 | 0.13 / 1.37 | inside |
| `bn` net scan row | prepared | 27.49 | 27.80 | +0.17 | +0.6 % | 6 of 7 | 0.12 / 0.45 | inside |
| `wide-h` net scan row (ns) | literal | 30.16 | 30.33 | +0.13 | +0.4 % | 5 of 7 | 0.34 / 0.88 | inside |
| `wide-h` net scan row | prepared | 30.31 | 30.62 | +0.40 | +1.3 % | 7 of 7 | 0.21 / 1.39 | inside |

Reading it. The `UPDATE` is the one cell where the sign agrees across both start orders (u1 B first, u2 B last), both paths, and 11 of 12 path-runs; a start-position penalty would have shown in one order only. The `UPDATE` is 98-100 % server CPU (wall p50 735-770 us, server CPU 752-815 us, section 5), so +30 us is +30 us of wall as well: 1,230 / 1,172 statements/s at B against 1,264 / 1,226 at A (derived from mean). The scan row's tendency is positive on every row of the table (B above A in 5-7 of 7 runs on three of four scan rows) but its median never exceeds 0.4 ns (1.3 %) against gaps up to 1.4 ns; with 7 runs that is a direction, not a finding, and the `INSERT`'s +1.3 - 2.1 % is the same kind of reading (5 of 6, inside).

## 3. Against this engine's own previous numbers (BT close, `v2.7.0-997-g4274ed81`)

Different binaries, different host day and fewer runs here; shown for league only. The BT-close file is the previous record for these shapes; nothing between it and A is in this table's delta.

| shape | BT close, B (`v2.7.0-997-g4274ed81`) | this run, A (`v3.2.0-13-g74b0a3dd`) | this run, B (`v3.2.0-22-g541ce440`) |
|---|---|---|---|
| `bn` net scan row, literal / prepared (ns) | 27.4 / 27.5 | 27.40 / 27.49 | 27.60 / 27.80 |
| `wide-h` net scan row, literal / prepared (ns) | 30.9 / 30.9 | 30.16 / 30.31 | 30.33 / 30.62 |
| point `SELECT` CPU, literal / prepared (us) | 29.8 / 27.9 | 31.3 / 28.8 (bn_census, 1,000-row table) | 31.4 / 27.7 |
| every-row `UPDATE`, 1,000 rows, literal / prepared (us) | 752.4 / 755.1 | 752.1 / 786.0 | 770.2 / 814.7 |
| `INSERT` | not in BT's shapes | 22.4 / 21.9 | 22.8 / 22.4 |

The scan row sits where BT left it: A is within 0.8 ns of BT's B on all four rows. The literal `UPDATE` at A equals BT's B (752); BW's B is 18 us (2.4 %) above it, and the prepared `UPDATE` is 31-60 us above BT's B across A and B (A already reads 786 against 755; this run has 6 runs against BT's 15, and the A-against-A' gap on the prepared path reached 17 us, so A's own 31 us excess over BT is not read as a change).

## 4. Insight

The cost BW added is per decoded column, and the every-row `UPDATE` is the statement here that decodes every column of every row it touches (it carries every cell it does not assign, so the whole row is decoded and re-encoded). Only the `UPDATE` rose beyond the spread, which fits a cost that depends on rows x columns written rather than on rows scanned: the scan kernel rejects on the cell's bytes before any decode (BM), so a rejected row never reaches a `has_dropped()` gate, and `bn`'s 10,000-row scan decodes 1 matching row. That reading predicts the `UPDATE` delta scales with rows and columns written (30 ns per row here); this was not measured, and a profile of the `UPDATE` (`tools/kds_sampler.c`) is the next step. If the gate must stay per column, hoisting `live_columns != offsets.size()` out of the loop into one branch per row, or specialising the loop for the no-dropped layout, are the two shapes that would remove it; that is a suggestion, not tested.

## 5. Waits

No decomposition beyond this applies: the measured unit is a statement on one connection, `relaxed`, one core.

| wait type | scan (10,000 rows) | every-row UPDATE | INSERT / point SELECT |
|---|---|---|---|
| durability / fsync | none (`relaxed`, read) | none: `relaxed` | none: `relaxed` |
| write statement | n/a | server CPU 752-815 us of wall p50 735-770 us (the mean wall is 776-780 us) | `INSERT` server CPU 22-23 us of wall p50 41-44 us |
| read | server CPU 302-306 us of wall p50 333-339 us on `bn` | in the walk | point `SELECT` 27-31 us of wall p50 54-62 us |
| client and socket round trip | the remainder, wall p50 less server CPU, about 30-35 us per scan (driver CPU 53-59 us) | driver CPU 33-38 us | the remainder, about 20-30 us per statement |
| lock or conflict wait | `window_wait_us` = 0, `lookups` = 0 in every window | none: one connection | none |
| buffer-pool miss | `pool_misses` = 0 in every scan window | `pages_allocated_delta` = 0 in every arm | n/a |

Correctness: `bad` replies = 0 in every cell; the scan digest, `examined=` and result rows agree across A, A' and B in run 0 of every file (`digest` arms-distinct = 1 per cell); every `UPDATE` run's end state (`range_v_ok`, `spill_txt_ok`) passed in all arms.

## 6. Tables

`B - A` columns are in the unit named in each row; throughput forms are derived (`1e6 / p50` or `1e6 / mean`). The `bn_census` driver emits p0, p25, p50, p90, p99 (no p95); `bm_scan` emits p0, p25, p50, p90, p95, p99 and the tables show p95 for it. Row sweep: `rows-200` and `rows-1k` below; the point `SELECT` is size-independent as the point rows show.


#### scan 10,000 rows, s1+s2 pooled: net scan row, ns of server CPU per row, (scan CPU - point SELECT CPU on the same relation) / rows; point SELECT CPU in us

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| bn (10000) net row ns literal | 7 | 27.40 | 27.24 | 27.60 | 0.04 [-0.29, 1.08] | +0.2 % | 2 of 7 | 0.13 / 1.37 | inside |
| bn (10000) net row ns prepared | 7 | 27.49 | 27.42 | 27.80 | 0.17 [-0.57, 1.30] | +0.6 % | 1 of 7 | 0.12 / 0.45 | inside |
| wide-h (10000) net row ns literal | 7 | 30.16 | 30.10 | 30.33 | 0.13 [-0.54, 1.96] | +0.4 % | 2 of 7 | 0.34 / 0.88 | inside |
| wide-h (10000) net row ns prepared | 7 | 30.31 | 30.88 | 30.62 | 0.40 [0.23, 0.53] | +1.3 % | 0 of 7 | 0.21 / 1.39 | inside |
| point:s CPU us literal | 7 | 29.32 | 28.89 | 29.49 | 0.16 [-0.20, 1.25] | +0.6 % | 3 of 7 | 0.52 / 1.05 | inside |
| point:s CPU us prepared | 7 | 27.43 | 27.17 | 27.49 | -0.01 [-0.53, 1.15] | -0.0 % | 4 of 7 | 0.24 / 0.94 | inside |
| point:wd CPU us literal | 7 | 30.84 | 30.69 | 30.98 | 0.37 [-0.61, 0.70] | +1.2 % | 1 of 7 | 0.29 / 0.84 | inside |
| point:wd CPU us prepared | 7 | 29.98 | 29.96 | 29.97 | 0.18 [-0.59, 1.02] | +0.6 % | 2 of 7 | 0.33 / 0.85 | inside |

#### scan 10,000 rows, s1+s2 pooled: latency of one scan (wall, client side), us; median over runs of each run's percentile; ops = total scans over runs

| cell | path | arm | ops | p0 | p25 | p50 | p95 | p99 | scans/s at p50 (derived) | server CPU us/scan | driver CPU us/scan |
|---|---|---|---|---|---|---|---|---|---|---|---|
| bn | literal | A | 672 | 320.3 | 327.6 | 337.0 | 366.6 | 412.4 | 2968 | 303.6 | 59.8 |
| bn | literal | A2 | 672 | 319.3 | 325.8 | 336.6 | 361.7 | 409.1 | 2971 | 301.6 | 57.7 |
| bn | literal | B | 672 | 322.3 | 331.1 | 338.9 | 370.3 | 410.3 | 2951 | 305.1 | 56.2 |
| bn | prepared | A | 672 | 317.3 | 324.9 | 333.9 | 368.5 | 411.5 | 2995 | 302.3 | 54.5 |
| bn | prepared | A2 | 672 | 316.5 | 325.5 | 334.1 | 359.5 | 420.5 | 2993 | 301.8 | 53.4 |
| bn | prepared | B | 672 | 320.6 | 327.5 | 336.9 | 364.0 | 415.0 | 2969 | 305.3 | 53.9 |
| wide-h | literal | A | 672 | 361.3 | 375.4 | 383.2 | 429.5 | 758.7 | 2609 | 331.8 | 89.4 |
| wide-h | literal | A2 | 672 | 362.5 | 373.1 | 380.6 | 427.9 | 756.8 | 2627 | 332.3 | 89.0 |
| wide-h | literal | B | 672 | 365.9 | 376.7 | 386.2 | 425.9 | 766.2 | 2589 | 334.3 | 90.3 |
| wide-h | prepared | A | 672 | 360.0 | 371.5 | 380.4 | 418.5 | 757.1 | 2629 | 333.1 | 86.0 |
| wide-h | prepared | A2 | 672 | 358.1 | 372.6 | 381.4 | 457.8 | 786.1 | 2622 | 340.1 | 88.3 |
| wide-h | prepared | B | 672 | 362.9 | 374.9 | 383.6 | 449.2 | 800.7 | 2607 | 336.2 | 88.6 |

bad replies: 0; window_wait_us total: 0; scan-cell pool_misses total: 0; digest arms-distinct per cell: [('bn', 1), ('wide-h', 1)]; n runs 7
unclean windows: 0 []

#### scan 10,000 rows, s1 alone: net scan row, ns of server CPU per row, (scan CPU - point SELECT CPU on the same relation) / rows; point SELECT CPU in us

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| bn (10000) net row ns literal | 3 | 27.44 | 27.32 | 27.50 | 0.04 [-0.15, 0.10] | +0.2 % | 1 of 3 | 0.13 / 0.20 | inside |
| bn (10000) net row ns prepared | 3 | 27.68 | 27.36 | 27.81 | 0.11 [0.02, 0.32] | +0.4 % | 0 of 3 | 0.12 / 0.38 | inside |
| wide-h (10000) net row ns literal | 3 | 30.54 | 29.83 | 30.17 | 0.13 [-0.54, 0.52] | +0.4 % | 1 of 3 | 0.34 / 0.88 | inside |
| wide-h (10000) net row ns prepared | 3 | 30.02 | 30.88 | 30.34 | 0.38 [0.32, 0.53] | +1.3 % | 0 of 3 | 0.19 / 1.39 | inside |
| point:s CPU us literal | 3 | 29.24 | 28.92 | 29.49 | 0.17 [-0.04, 1.25] | +0.6 % | 1 of 3 | 0.52 / 1.05 | inside |
| point:s CPU us prepared | 3 | 27.43 | 27.32 | 27.81 | 0.34 [-0.22, 1.15] | +1.2 % | 1 of 3 | 0.26 / 0.76 | inside |
| point:wd CPU us literal | 3 | 30.84 | 30.69 | 31.25 | 0.41 [0.37, 0.55] | +1.3 % | 0 of 3 | 0.64 / 0.71 | inside |
| point:wd CPU us prepared | 3 | 30.12 | 29.84 | 30.29 | 0.34 [-0.16, 1.02] | +1.1 % | 1 of 3 | 0.33 / 0.85 | inside |

#### scan 10,000 rows, s2 alone: net scan row, ns of server CPU per row, (scan CPU - point SELECT CPU on the same relation) / rows; point SELECT CPU in us

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| bn (10000) net row ns literal | 4 | 27.30 | 27.20 | 27.67 | 0.27 [-0.29, 1.08] | +1.0 % | 1 of 4 | 0.40 / 1.37 | inside |
| bn (10000) net row ns prepared | 4 | 27.40 | 27.58 | 27.77 | 0.29 [-0.57, 1.30] | +1.1 % | 1 of 4 | 0.22 / 0.45 | inside |
| wide-h (10000) net row ns literal | 4 | 30.05 | 30.22 | 30.41 | 0.25 [-0.09, 1.96] | +0.8 % | 1 of 4 | 0.32 / 0.54 | inside |
| wide-h (10000) net row ns prepared | 4 | 30.35 | 30.85 | 30.72 | 0.44 [0.23, 0.51] | +1.4 % | 0 of 4 | 0.50 / 0.83 | inside |
| point:s CPU us literal | 4 | 29.52 | 28.79 | 29.34 | -0.01 [-0.20, 0.86] | -0.0 % | 2 of 4 | 0.66 / 0.98 | inside |
| point:s CPU us prepared | 4 | 27.42 | 27.03 | 27.40 | -0.09 [-0.53, 1.07] | -0.3 % | 3 of 4 | 0.20 / 0.94 | inside |
| point:wd CPU us literal | 4 | 30.66 | 30.72 | 30.95 | 0.15 [-0.61, 0.70] | +0.5 % | 1 of 4 | 0.24 / 0.84 | inside |
| point:wd CPU us prepared | 4 | 29.89 | 30.02 | 29.83 | 0.16 [-0.59, 0.59] | +0.5 % | 1 of 4 | 0.20 / 0.52 | inside |

#### scan 200 / 1,000 rows: net scan row, ns of server CPU per row, (scan CPU - point SELECT CPU on the same relation) / rows; point SELECT CPU in us

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| rows-200 (200) net row ns literal | 2 | 26.86 | 29.41 | 28.14 | 1.27 [0.77, 1.78] | +4.7 % | 0 of 2 | 2.54 / 3.83 | inside |
| rows-200 (200) net row ns prepared | 2 | 28.99 | 30.82 | 28.23 | -0.76 [-1.58, 0.06] | -2.6 % | 1 of 2 | 1.83 / 3.37 | inside |
| rows-1k (1000) net row ns literal | 2 | 27.00 | 27.05 | 27.29 | 0.29 [-0.26, 0.84] | +1.1 % | 1 of 2 | 0.63 / 0.67 | inside |
| rows-1k (1000) net row ns prepared | 2 | 25.69 | 25.88 | 26.44 | 0.75 [0.48, 1.02] | +2.9 % | 0 of 2 | 0.52 / 0.71 | outside |
| point:s200 CPU us literal | 2 | 27.53 | 27.24 | 27.58 | 0.05 [-0.42, 0.52] | +0.2 % | 1 of 2 | 0.60 / 0.89 | inside |
| point:s200 CPU us prepared | 2 | 26.60 | 26.35 | 26.85 | 0.25 [-0.04, 0.54] | +0.9 % | 1 of 2 | 0.70 / 0.95 | inside |
| point:s1k CPU us literal | 2 | 28.93 | 28.96 | 29.01 | 0.08 [-0.40, 0.57] | +0.3 % | 1 of 2 | 0.85 / 0.89 | inside |
| point:s1k CPU us prepared | 2 | 30.07 | 29.82 | 29.29 | -0.78 [-0.84, -0.73] | -2.6 % | 2 of 2 | 0.25 / 0.44 | outside |

#### scan 200 / 1,000 rows: latency of one scan (wall, client side), us; median over runs of each run's percentile; ops = total scans over runs

| cell | path | arm | ops | p0 | p25 | p50 | p95 | p99 | scans/s at p50 (derived) | server CPU us/scan | driver CPU us/scan |
|---|---|---|---|---|---|---|---|---|---|---|---|
| rows-200 | literal | A | 1536 | 59.2 | 61.0 | 61.9 | 76.9 | 93.5 | 16163 | 32.9 | 34.6 |
| rows-200 | literal | A2 | 1536 | 59.7 | 61.1 | 62.0 | 76.3 | 91.6 | 16140 | 33.1 | 34.5 |
| rows-200 | literal | B | 1536 | 59.8 | 61.2 | 62.0 | 76.3 | 86.8 | 16123 | 33.2 | 35.1 |
| rows-200 | prepared | A | 1536 | 56.2 | 57.7 | 58.5 | 73.9 | 87.2 | 17083 | 32.4 | 31.9 |
| rows-200 | prepared | A2 | 1536 | 55.8 | 57.9 | 58.8 | 72.9 | 89.3 | 17013 | 32.5 | 32.1 |
| rows-200 | prepared | B | 1536 | 56.6 | 57.8 | 58.6 | 73.1 | 89.3 | 17075 | 32.5 | 31.9 |
| rows-1k | literal | A | 768 | 81.4 | 83.4 | 84.8 | 102.5 | 130.4 | 11793 | 55.9 | 38.3 |
| rows-1k | literal | A2 | 768 | 81.3 | 83.4 | 84.8 | 102.2 | 123.4 | 11792 | 56.0 | 39.7 |
| rows-1k | literal | B | 768 | 82.3 | 83.9 | 85.2 | 101.4 | 128.7 | 11741 | 56.3 | 37.5 |
| rows-1k | prepared | A | 768 | 78.4 | 80.2 | 81.5 | 98.9 | 128.9 | 12266 | 55.8 | 34.9 |
| rows-1k | prepared | A2 | 768 | 78.2 | 80.3 | 81.9 | 97.8 | 118.7 | 12204 | 55.7 | 35.6 |
| rows-1k | prepared | B | 768 | 78.7 | 80.5 | 81.7 | 98.1 | 124.6 | 12246 | 55.7 | 34.2 |

bad replies: 0; window_wait_us total: 0; scan-cell pool_misses total: 0; digest arms-distinct per cell: [('rows-1k', 1), ('rows-200', 1)]; n runs 2
unclean windows: 0 []

#### u1+u2 pooled, 1,000 rows: server CPU per statement, us (an insert/select-pk statement; update-range = one every-row UPDATE of 1,000 rows)

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| insert CPU us literal | 6 | 22.36 | 22.75 | 22.77 | 0.48 [-0.58, 0.84] | +2.1 % | 1 of 6 | 0.40 / 1.07 | inside |
| insert CPU us prepared | 6 | 21.86 | 22.16 | 22.40 | 0.29 [-0.04, 0.81] | +1.3 % | 1 of 6 | 0.48 / 0.76 | inside |
| select-pk CPU us literal | 6 | 31.31 | 31.42 | 31.42 | 0.11 [-0.51, 2.21] | +0.3 % | 2 of 6 | 0.40 / 0.85 | inside |
| select-pk CPU us prepared | 6 | 28.84 | 27.39 | 27.72 | 0.17 [-2.48, 0.31] | +0.6 % | 2 of 6 | 0.46 / 2.53 | inside |
| update-range CPU us literal | 6 | 752.14 | 737.81 | 770.18 | 30.24 [10.32, 44.08] | +4.0 % | 0 of 6 | 8.06 / 27.11 | outside |
| update-range CPU us prepared | 6 | 785.98 | 789.08 | 814.68 | 30.19 [-6.11, 53.12] | +3.8 % | 1 of 6 | 10.11 / 17.18 | outside |

#### u1+u2 pooled, 1,000 rows: statement wall latency, us; median over runs of each run's percentile

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | literal | A | 11988 | 37.5 | 43.0 | 43.5 | 51.4 | 73.0 | 21463 | 22.4 | 24.3 |
| insert | literal | A2 | 11988 | 39.8 | 43.3 | 43.9 | 51.7 | 70.2 | 21215 | 22.8 | 24.3 |
| insert | literal | B | 11988 | 38.8 | 43.3 | 43.9 | 51.7 | 73.5 | 20791 | 22.8 | 24.2 |
| insert | prepared | A | 11988 | 34.5 | 40.5 | 41.0 | 48.7 | 70.9 | 22383 | 21.9 | 22.3 |
| insert | prepared | A2 | 11988 | 35.2 | 40.9 | 41.4 | 49.3 | 73.5 | 22507 | 22.2 | 22.3 |
| insert | prepared | B | 11988 | 34.8 | 40.9 | 41.4 | 50.0 | 71.2 | 22436 | 22.4 | 22.2 |
| select-pk | literal | A | 11988 | 54.0 | 56.4 | 61.6 | 70.2 | 91.2 | 15910 | 31.3 | 31.6 |
| select-pk | literal | A2 | 11988 | 54.0 | 56.4 | 61.5 | 70.0 | 87.6 | 15897 | 31.4 | 31.9 |
| select-pk | literal | B | 11988 | 54.0 | 56.4 | 61.7 | 70.1 | 86.8 | 15903 | 31.4 | 31.8 |
| select-pk | prepared | A | 11988 | 50.7 | 53.0 | 54.3 | 66.8 | 87.3 | 17197 | 28.8 | 29.3 |
| select-pk | prepared | A2 | 11988 | 50.8 | 53.0 | 53.9 | 61.5 | 76.3 | 17780 | 27.4 | 29.2 |
| select-pk | prepared | B | 11988 | 50.6 | 52.9 | 54.0 | 62.8 | 78.3 | 17613 | 27.7 | 29.2 |
| update-range | literal | A | 720 | 696.0 | 724.3 | 737.8 | 862.9 | 1284.9 | 1264 | 752.1 | 36.9 |
| update-range | literal | A2 | 720 | 694.2 | 720.8 | 733.5 | 863.1 | 984.9 | 1291 | 737.8 | 38.2 |
| update-range | literal | B | 720 | 728.1 | 758.7 | 769.7 | 894.0 | 1003.5 | 1229 | 770.2 | 38.1 |
| update-range | prepared | A | 720 | 692.5 | 723.6 | 735.2 | 851.7 | 2094.7 | 1226 | 786.0 | 33.4 |
| update-range | prepared | A2 | 720 | 690.7 | 717.8 | 731.4 | 869.0 | 2129.5 | 1204 | 789.1 | 33.5 |
| update-range | prepared | B | 720 | 719.8 | 752.6 | 767.7 | 890.8 | 2058.9 | 1172 | 814.7 | 33.5 |

bad replies: 0; end_state all ok: True; n runs 6
pages_allocated_delta (unspilled) per arm per run: [{'B': 0, 'A2': 0, 'A': 0}, {'B': 0, 'A2': 0, 'A': 0}, {'B': 0, 'A2': 0, 'A': 0}]

#### u1 alone: server CPU per statement, us (an insert/select-pk statement; update-range = one every-row UPDATE of 1,000 rows)

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| insert CPU us literal | 3 | 22.96 | 23.28 | 23.80 | 0.32 [-0.58, 0.84] | +1.4 % | 1 of 3 | 0.41 / 1.07 | inside |
| insert CPU us prepared | 3 | 22.44 | 22.31 | 22.40 | 0.19 [-0.04, 0.21] | +0.8 % | 1 of 3 | 0.36 / 0.72 | inside |
| select-pk CPU us literal | 3 | 32.06 | 31.49 | 31.57 | 0.60 [-0.50, 2.21] | +1.9 % | 1 of 3 | 0.57 / 0.85 | inside |
| select-pk CPU us prepared | 3 | 30.05 | 27.52 | 27.88 | 0.25 [-2.48, 0.31] | +0.8 % | 1 of 3 | 0.99 / 2.53 | inside |
| update-range CPU us literal | 3 | 762.15 | 740.58 | 772.47 | 23.68 [10.32, 25.77] | +3.1 % | 0 of 3 | 14.69 / 27.11 | inside |
| update-range CPU us prepared | 3 | 797.36 | 808.98 | 818.56 | 34.15 [-6.11, 53.12] | +4.3 % | 1 of 3 | 5.95 / 11.63 | outside |

#### u2 alone: server CPU per statement, us (an insert/select-pk statement; update-range = one every-row UPDATE of 1,000 rows)

| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |
|---|---|---|---|---|---|---|---|---|---|
| insert CPU us literal | 3 | 22.24 | 22.50 | 22.70 | 0.54 [0.42, 0.54] | +2.4 % | 0 of 3 | 0.38 / 0.41 | outside |
| insert CPU us prepared | 3 | 21.69 | 22.00 | 22.40 | 0.64 [0.37, 0.81] | +2.9 % | 0 of 3 | 0.60 / 0.76 | inside |
| select-pk CPU us literal | 3 | 31.25 | 31.35 | 31.34 | 0.09 [-0.51, 0.13] | +0.3 % | 1 of 3 | 0.23 / 0.83 | inside |
| select-pk CPU us prepared | 3 | 27.24 | 27.25 | 27.02 | 0.11 [-0.29, 0.22] | +0.4 % | 1 of 3 | 0.36 / 0.56 | inside |
| update-range CPU us literal | 3 | 721.34 | 719.86 | 756.05 | 37.42 [34.71, 44.08] | +5.2 % | 0 of 3 | 1.48 / 14.58 | outside |
| update-range CPU us prepared | 3 | 782.23 | 778.74 | 810.81 | 26.22 [21.08, 48.91] | +3.4 % | 0 of 3 | 10.99 / 17.18 | outside |

