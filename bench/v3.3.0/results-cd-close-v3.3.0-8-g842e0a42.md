# CD close: no cost outside the spread on INSERT, UPDATE, SELECT or scan; the clean pass costs 0.6 us and 152 bytes of log per row, and at 1,000,000 rows holds its core for 0.9 s, about half of it a WAL segment-roll fsync

**Thesis.** On `cd-column-slot-cleaning` (branch `worktree-cd-column-slot-cleaning`), CD's close commit `842e0a42` (`v3.3.0-8-g842e0a42`, **B**) against the commit CD opened at, `f3ec25ea` (`v3.3.0`, **A**), moves **no cell outside the A' - A spread** (A' = A's bytes started as a second server, the in-run control). CD-S1's null-bit OR in `EncodeRow` is below what this host resolves: a single-row `INSERT` into a relation with two dropped, uncleaned slot columns reads B - A = -0.17 us literal / -0.32 us prepared (-0.6 % / -1.3 %, B above A in 6 and 5 of 18 runs) against largest A' - A gaps of 1.49 / 1.40 us. The every-row `UPDATE` of 1,000 rows, the cell where BW's decode gate showed (+30 us), reads B - A = -11.6 us literal / -15.0 us prepared (-1.4 % / -1.8 %) over 18 runs, but that cell is noisy on this host day (largest A' - A gaps 125 / 274 us), so the honest reading is "no regression resolved", not "B is faster". Point `SELECT` and the `bn` scan row (7 runs) are inside by 1 % or less. **The price of CD is the pass itself, and it is paid in one place**: `PURGE DROPPED COLUMNS` cleans a dirty row in 0.6 us of server CPU and 152 bytes of log (this schema); that is 62 ms for 100,000 rows and **1.30 s for 1,000,000**, where the 152 MB of log crosses two WAL segment rolls (about 0.63 s of roll hold, the rest CPU) and the scheduler reports a **0.91 s non-yielding run**. A point-`UPDATE` stream sharing the core sees its percentiles untouched (p50 47-48 us, p99 74-76 us, equal to a no-pass control) and exactly **one statement stalled for the whole pass: 1.78-1.85 s** at 1,000,000 rows under traffic. A re-run on a clean relation is a read walk at 27 ns per row and writes 64 bytes of log in all.

Server CPU is the sum of `/proc/<pid>/task/*/schedstat` over every server thread; wall is the client's. Drivers and exact invocations: `archive/cd-close-v3.3.0-8-g842e0a42/scripts/run.sh` (part 1) and `run2.sh` to `run6.sh`; the flags of `bn_census.py` and `bm_scan.py` are in their own docstrings (`bench/README.md`: `bench/docs/` stays closed), and `scripts/cd_census.py` / `scripts/cd_pass.py` carry theirs.

| Item | Value |
|---|---|
| Executed | 2026-10-19, UTC. Task start 09:56:24. Builds 09:57:38-09:59:25 (both together, 1 min 47 s). A smoke run of both new scripts at 10:00-10:01 (discarded, not a measurement). Measurement, in six parts (`progress.txt` in the archive carries every phase stamp): part 1 (u1, u2, the first pass cells, s1, z) 10:02:07-10:10:41; part 2 (cells 4 and 5 re-run with the final driver, and the control) 10:10:42-10:14:51; part 3 (s2) 10:14:53-10:15:58; part 4 (u3, u4) 10:17:35-10:22:45; part 5 (cell 6) 10:22:46-10:22:52; part 6 (cell 4 under strict and group, an extra) 10:26:53-10:33:51, its first attempt lost to `ENOSPC` and re-run from 10:30:30. The A/B measurement ended at 10:22:45 |
| Worktree / branch | `cd-column-slot-cleaning`, branch `worktree-cd-column-slot-cleaning`; HEAD `842e0a42`; `git status` clean at the start; at the end the only change is the new untracked `bench/v3.3.0/`. No engine source was touched |
| Commits measured | **A** = `f3ec25ea7ac231d5efd8cb4f6dca025d71dc2a68`, `v3.3.0`, committed 2026-10-19 00:46:53 UTC. **B** = `842e0a421c51dd22504ea9bda72c6c1103278d17`, `v3.3.0-8-g842e0a42`, committed 2026-10-19 09:55:57 UTC; 8 commits above A (CD-S0 to CD-S5; 20 files under `src/` and `include/`, +948/-142 lines). A' = A's bytes started as a second server |
| Binary provenance | Each built from `git archive <commit>` into `/home/cdkbs/bench-runs/cd-close/src-{A,B}` and `build-{A,B}`; the binary is newer than its commit (A 09:59:24.40, B 09:59:25.17 UTC), built at the measured commit from a clean export. Measured copies in `bin/`, hashed after the copy (`archive/.../binaries.txt`): **A and A'** sha256 `8ab97db61ca6226dcf44a3601bd53327bca246643caf186f8c762aa1b8cc39c1`; **B** `8b755584fd26d9d6a5bf3c4e3f9c1938700f802b3e84b3264f0a3025ef0a5f67`. Every server started from the copies |
| Build | `CMAKE_BUILD_TYPE=Release` (read from each `CMakeCache.txt`), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, `-j4` each; gcc 13.3.0; finished before any measurement started |
| Host / device | 8 vCPU AMD EPYC 9V74 (4 cores x 2 threads), Linux 7.0.0-1014-azure. Data files and WAL under `/home/cdkbs/bench-runs/cd-close/wd-*/` on `/dev/root`, **ext4, not tmpfs** (`df -T`; the volume was 95 % full, 13.8 GB free at the start and **filled during part 6**: see section 1). 1-minute load average 0.3-1.7 across the cell records (0.75 at launch); no `cc1plus`, `cmake --build` or `ctest` in any cell's host record (`pgrep` per cell, in the JSON). The unrelated `kds_server` instances on the host (port 15432 and a sandbox) sat idle; all ports here are 33xxx |
| Pinning | One server process per arm, one busy at a time, on CPU 2; the driver (and `cd_pass.py`'s stream thread) on CPU 4 |
| Server config | `cores = 1`, `durability = relaxed` (the cells below; one extra table runs `strict` and `group`), `buffer_pool_frames = 65536`, `log_level = warn`, `scan_kernel` default (on); fresh server and data file per arm per run. Under `relaxed` no fsync wait is in a measured statement except the WAL segment roll's (section 5) |
| Drivers | `tools/bn_census.py` sha256 `35693dc2...`, `tools/bm_scan.py` `1a1ca4ee...`, `tools/kwp.py` `4fc35971...`, unmodified; `scripts/cd_census.py` `0a16c713...` (imports `bn_census`, adds three `INSERT` shapes and their relations), `scripts/cd_pass.py` `be381cb5...` (the final revision; it gained `--durability` after the relaxed phases ran, and earlier in the day the meta deltas, the stall timeline, the control and the raw vectors: the first runs of cells 4-6 are archived as `*-first`, superseded). Gates: `--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1 --quiet-load 2`. Zero unclean cells in the A/B phases; in cell 4, 6 of the 20 timed passes carry the gate's `clean = False` (their walls agree with the clean ones in the twin rep to 0.1 % at 1,000,000 rows) |
| Suite | Not executed in this task; the close's suite run is the parent's. No code was changed by this measurement |

## 1. What was run and what was cut

Three arms (A, A', B) in the A/B phases, a fresh server and data file per arm per run, the arms interleaved block by block (6 blocks per cell, the first server of a block rotating by one per block and per run), the two driver paths (literal, prepared) alternating by run parity. The pass cells run on **B only**: A has no `PURGE DROPPED COLUMNS`.

| group | driver | rows | start order | runs | work per run per arm |
|---|---|---|---|---|---|
| u1 | `cd_census.py` (`bn_census` shapes `select-pk`, `insert`, `update-range` + `insert-plain`, `insert-spare`, `insert-dropped`) | 1,000 | B, A', A | 3 | 2,000 point statements and 120 every-row `UPDATE`s per path |
| u2 | same | 1,000 | A, A', B | 3 | same |
| u3 | same, `--only insert,insert-dropped,update-range` | 1,000 | A', B, A | 6 | same |
| u4 | same | 1,000 | B, A, A' | 6 | same |
| s1 | `bm_scan.py census --cells bn` | 10,000 | A, A', B | 3 | 96 scans per path, 2,000 point `SELECT`s |
| s2 | same | 10,000 | B, A', A | 4 | same |
| z | `bm_scan.py census --cells rows-200,rows-1k` | 200; 1,000 | B, A', A | 2 | same |
| p4b | `cd_pass.py --cells 4` | 200, 1K, 10K, 100K, 1M | B only | 2 reps per size | 1 dirty pass, 1 clean re-run, 1 reusing `ADD` |
| p5b / p5c | `cd_pass.py --cells 5` (and `--control`) | 1,000,000 | B only | 3 reps each | 3 s stream, pass (or a 2 s gap), 3 s stream |
| p6b | `cd_pass.py --cells 6` | 3-row relations | B only | 100 trials per kind | interleaved vacant and reusing `ADD` |
| p4s / p4g | `cd_pass.py --cells 4 --durability strict` / `group` | 10K, 100K, 1M | B only | 2 reps per size | as p4b |

`bn_census` row mapping: `--rows 1000` is the size of tables `t`, `r` (the every-row `UPDATE r SET v = ? WHERE id >= 1` writes all 1,000 rows) and of the others; the `INSERT` relations start empty and grow by 12,000 rows per arm over a run. `bm_scan --cells bn` is relation `s`, 10,000 rows; `rows-200` and `rows-1k` are `s200` / `s1k`. In `cd_census.py`: `ib` (no `SPARE`), `ip` (`SPARE 2`, never altered) and `idr` (`SPARE 2`, `ADD x1 int64`, `ADD x2 varchar(16)`, `DROP x1`, `DROP x2`, never cleaned) each take `INSERT INTO <rel> (id, v, w) VALUES (?, ?, ?)`, a column list so the dropped slots are not in the way; the positional `insert` into `i` is the shape BW's close measured.

**What was cut or not run.** Nothing in the work order's cut list was dropped: cell 3 (scan and point `SELECT` controls), cell 6, cell 5's p99 and cell 4's 1,000,000 rows all ran. Not run, and so not characterised:
- **The every-row `UPDATE` at 200 and 10,000 rows** (cell 2 is at 1,000 rows, as BW's was); rule 9's sweep is therefore met for the scan (200 / 1K / 10K) and for the pass (200 to 1M) but not for the `UPDATE`. A multi-row `INSERT` was not measured (no driver shape); the `INSERT` relations grow from 0 to 12,000+ rows over a run, so size independence is only weakly shown.
- **`wide-h`** (BW measured it; CD's change does not decode a scan's rejected rows) and the scan at 200 / 1,000 rows has 2 runs.
- **The pass under traffic on any relation but the one the stream updates**: cell 5's stream dirties the same ~6,000 pages the pass does, so a checkpoint cost the pass adds to a relation the stream does not touch is not isolated (section 3).
- **The PostgreSQL floor** (none of these shapes has a twin in `tools/pg_*.py` run for this close).
- **A pass larger than 1,000,000 rows**, and cell 5 at any other size.
- **A pass on a relation whose dropped column is a long varchar spilled more than once**, or on a wider row: the 152 bytes of log per row belong to this schema (3 `int64`, two 16-byte slots, one 20-byte spilled value).
- During part 6 the volume filled (`ENOSPC`, 13.8 GB free at the start; the earlier phases' data directories, 15 GB under `bench-runs/cd-close`, were the cause) and the first attempt of the strict and group cells died at 1,000,000 rows; the data directories were removed (they are not evidence) and the cells re-run. Parts 1-5 had completed with `rc=0` before that and carry no I/O error.

## 2. Headline: B - A against the A' - A spread

Throughput is derived from median server CPU (statements per second `= 1e6 / CPU us`; scan rows per second `= 1e9 / net ns per row`). "Outside" means the median paired B - A exceeds the largest |A' - A| of the same cell; in either direction. The cost columns are in the unit named. All runs pooled over the start orders of section 1.

| cell | runs | A | A' | B | B - A cost, paired median | B slower than A | A' - A typical / largest abs gap | verdict |
|---|---|---|---|---|---|---|---|---|
| INSERT, no dropped column (positional, relation `i`), literal | 18 | 40,790 /s | 40,742 /s | 41,124 /s | -0.24 us (-1.0 %, B faster) | 5 of 18 | 0.49 / 1.02 us | inside |
| INSERT, no dropped column, prepared | 18 | 41,793 /s | 41,707 /s | 42,097 /s | -0.21 us (-0.9 %, B faster) | 6 of 18 | 0.48 / 1.06 us | inside |
| INSERT, **two dropped slot columns, not cleaned**, literal | 18 | 37,180 /s | 37,195 /s | 37,546 /s | -0.17 us (-0.6 %, B faster) | 6 of 18 | 0.44 / 1.49 us | inside |
| INSERT, two dropped slot columns, prepared | 18 | 38,663 /s | 38,444 /s | 38,710 /s | -0.32 us (-1.3 %, B faster) | 5 of 18 | 0.68 / 1.40 us | inside |
| INSERT, `SPARE 2`, no ALTER (column list), literal | 6 | 37,272 /s | 37,751 /s | 37,735 /s | -0.03 us (-0.1 %) | 3 of 6 | 0.68 / 1.25 us | inside |
| INSERT, `SPARE 2`, no ALTER, prepared | 6 | 38,753 /s | 38,894 /s | 38,778 /s | -0.08 us (-0.3 %) | 3 of 6 | 0.50 / 1.64 us | inside |
| INSERT, no `SPARE` (column list), literal | 6 | 37,508 /s | 37,593 /s | 37,824 /s | +0.01 us (+0.0 %) | 3 of 6 | 0.74 / 1.11 us | inside |
| INSERT, no `SPARE` (column list), prepared | 6 | 38,819 /s | 39,238 /s | 38,786 /s | +0.07 us (+0.3 %) | 3 of 6 | 0.71 / 1.68 us | inside |
| every-row `UPDATE` of 1,000 rows, literal | 18 | 1,218 /s | 1,266 /s | 1,226 /s | -11.6 us (-1.4 %, B faster) | 6 of 18 | 27.6 / 124.6 us | inside |
| every-row `UPDATE` of 1,000 rows, prepared | 18 | 1,232 /s | 1,205 /s | 1,248 /s | -15.0 us (-1.8 %, B faster) | 6 of 18 | 28.3 / 273.7 us | inside |
| point `SELECT` by key, literal | 6 | 29,836 /s | 30,212 /s | 30,216 /s | -0.37 us (-1.1 %, B faster) | 2 of 6 | 0.40 / 1.90 us | inside |
| point `SELECT` by key, prepared | 6 | 33,346 /s | 34,002 /s | 34,571 /s | -0.38 us (-1.3 %, B faster) | 1 of 6 | 0.68 / 1.68 us | inside |
| scan net row, `bn`, 10,000 rows, literal | 7 | 38.68 M rows/s | 38.89 M rows/s | 39.08 M rows/s | +0.10 ns (+0.4 %, B slower) | 4 of 7 | 0.12 / 1.32 ns | inside |
| scan net row, `bn`, 10,000 rows, prepared | 7 | 38.44 M rows/s | 38.80 M rows/s | 38.96 M rows/s | -0.22 ns (-0.8 %, B faster) | 3 of 7 | 0.23 / 1.80 ns | inside |
| scan net row, 200 rows, literal | 2 | 33.7 M rows/s | 33.0 M rows/s | 34.6 M rows/s | -0.74 ns (-2.5 %, B faster) | 1 of 2 | 0.70 / 0.73 ns | **outside, B faster** (2 runs) |
| scan net row, 200 rows, prepared | 2 | 34.4 M rows/s | 34.4 M rows/s | 33.6 M rows/s | +0.64 ns (+2.2 %, B slower) | 1 of 2 | 3.21 / 3.27 ns | inside |
| scan net row, 1,000 rows, literal | 2 | 37.9 M rows/s | 40.9 M rows/s | 39.6 M rows/s | -1.09 ns (-4.1 %, B faster) | 1 of 2 | 1.88 / 3.68 ns | inside |
| scan net row, 1,000 rows, prepared | 2 | 41.2 M rows/s | 41.4 M rows/s | 41.7 M rows/s | -0.29 ns (-1.2 %, B faster) | 1 of 2 | 1.04 / 1.14 ns | inside |

**No B - A is outside the spread in the slower direction, so no regression is named.** Two readings need their caveats stated:
- The one flagged cell (200-row scan, literal) is B *faster* by 2.5 % over two runs whose A' - A gap is 0.7 ns; with two runs the spread is not an estimate, and the same size prepared reads +2.2 % inside. Read it as noise. Separately, the 3-run `s1` group alone read the `bn` literal row B +0.12 ns (+0.5 %) against a 0.12 ns gap, marginally outside; the 4-run `s2` group reads -0.35 ns and the 7 pooled runs +0.10 ns against 1.32 ns (all in `archive/.../out/analysis-s.md`). It is the same size as the INSERT's noise and is not a finding.
- The every-row `UPDATE` is the least resolvable cell. Per-run server CPU of the same arm moves by up to +-12 % between runs of one invocation, and between invocations the A arm's literal median rose 746 us (u1) to 839 us (u3) to 866 us (u4) on the same host day, with the cell's run-queue wait share (`wait_pct`, the server's own threads against its one pinned CPU) rising from 40 % to 316 % through u3 and u4. B - A is negative in the median of every invocation and path (-8 to -21 us, `analysis-u.md`'s per-invocation table), B above A in 6 of 18 runs, but the largest A' - A gaps are 125 and 274 us. This is "B does not cost more than the 11-28 us typical gap", not "B is 1.5 % faster". A cost of 30 us (4 %) per statement would sit at the typical gap of this cell and could not be told from noise by it; the finding is that no cost larger than that gap appears, and CD's diff does not touch the `UPDATE` path.

**The null-bit OR (CD-S1) is priced at under 1.5 us per `INSERT` and no resolved amount.** The in-arm view removes the cross-server noise: with the layout held fixed (`SPARE 2`), `insert-dropped` minus `insert-spare` is +0.12 us literal / +0.25 us prepared in B, and +0.22 / +0.15 us in A, which writes no bit (u1+u2, 6 runs, `analysis-u.md` within-arm table). The 0.1-0.25 us the dropped layout costs is already in A, i.e. BW's `has_dropped` branch, and B does not add to it.

## 3. The clean pass: cost, log volume and the stall

`PURGE DROPPED COLUMNS FROM p` on a relation `p (id, v, w) BTREE SPARE 2` with `note varchar(16)` added, filled with N rows each carrying a 20-byte `note` (a spill), then `DROP COLUMN note`. Binary B, `relaxed`, two reps per size. Rows/s is derived (`N / wall`); the walls are one statement each.

| rows | rep | dirty pass: rows/s | dirty wall | server CPU | log bytes per row (total) | clean re-run: rows/s | clean re-run wall | log bytes of the re-run | reusing `ADD` afterwards |
|---|---|---|---|---|---|---|---|---|---|
| 200 | 0 / 1 | 722,551 / 741,215 | 0.28 / 0.27 ms | below 1 ms | 154.0 (30,800) | 1.42 M / 1.50 M | 0.141 / 0.133 ms | 64 | 116 / 109 us |
| 1,000 | 0 / 1 | 1.37 M / 1.39 M | 0.73 / 0.72 ms | below 1 ms | 152.4 (152,400) | 5.00 M / 5.74 M | 0.200 / 0.174 ms | 64 | 120 / 108 us |
| 10,000 | 0 / 1 | 1.62 M / 1.66 M | 6.19 / 6.01 ms | 6 ms | 152.0 (1,520,400) | 24.5 M / 24.2 M | 0.408 / 0.412 ms | 64 | 109 / 105 us |
| 100,000 | 0 / 1 | 1.56 M / 1.64 M | 64.0 / 60.8 ms | 63 / 61 ms | 152.0 (15,200,400) | 34.7 M / 34.0 M | 2.88 / 2.94 ms | 64 | 112 / 122 us |
| **1,000,000** | 0 / 1 | **768,241 / 768,858** | **1,302 / 1,301 ms** | 693 / 666 ms | 152.2 / 152.0 (152,169,776 / 152,008,672) | 36.6 M / 36.3 M | 27.3 / 27.5 ms | 64 | 125 / 123 us |

Every pass reply reported `<N> row(s) cleaned, <N> spill(s) released`, every re-run `0 row(s) cleaned, 0 spill(s) released`, and after the reusing `ADD` the new column read NULL in all N rows (`SELECT count(*) ... WHERE again IS NULL` = N).

**The cost is linear at 0.6 us and 152 bytes per row up to 100,000 rows, then doubles per row at 1,000,000.** From 10,000 to 100,000 rows the wall is the CPU (61-64 ms of 63 / 61 ms CPU) and 0.60-0.64 us per row; at 1,000,000 it is 1.30 us per row, with CPU still 0.67-0.69 us per row. The missing half is the log: the pass writes 152 MB, which is two WAL segment rolls (64 MiB segments, `wal.md` §6), and `SHOW META` read around the pass says where the 1.30 s went.

| rows | rep | wall | server CPU (all threads) | segment rolls | roll hold (the device sync of the segment left, under the stream latch) | ring-full waits | longest non-yielding run |
|---|---|---|---|---|---|---|---|
| 10,000 | 0 / 1 | 6 ms | 6 ms | 0 | 0 | 1 | below the counter's threshold (`hold_out_run_over_10ms` = 1) |
| 100,000 | 0 / 1 | 64 / 61 ms | 63 / 61 ms | 0 | 0 | 14 | below the counter's threshold (`hold_out_run_over_10ms` = 1) |
| 1,000,000 | 0 | 1,302 ms | 693 ms | 2 | 626 ms | 144 | **911 ms** |
| 1,000,000 | 1 | 1,301 ms | 666 ms | 2 | 642 ms | 144 | **916 ms** |

CPU plus roll hold is 1,319 / 1,308 ms against a 1,302 / 1,301 ms wall: **at 1,000,000 rows about 48 % of the pass is a fsync waited on inside the log append**, not work. The ring-full count is 1 per MB of log (14 at 15 MB, 144 at 152 MB) and costs nothing visible at 100,000 rows, where wall equals CPU. The dirty-to-clean ratio at 1,000,000 rows is 48x (1.30 s against 27 ms): a re-run is a read walk at 27-36 ns per row and writes 64 bytes in all, whatever N (the statement's own transaction records).

**A stream sharing the core sees one stalled statement and nothing in its percentiles** (cell 5). A serial autocommit `UPDATE p SET v = ? WHERE id = ?` over random keys, one connection, literal, runs 3 s alone, through the pass, then 3 s alone, on the 1,000,000-row relation; the control is the same stream with a 2 s idle gap where the pass would be (no `PURGE` sent). Latency in us.

| run | window | ops | stmts/s | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|---|
| pass, rep 0 | pre (3 s) | 59,256 | 19,752 | 37.7 | 46.5 | 47.0 | 59.8 | 72.0 | 5,151 |
| pass, rep 0 | post (3 s) | 42,335 | 14,112 | 37.2 | 46.8 | 47.4 | 61.8 | 82.9 | 597,087 |
| pass, rep 0 | whole run | 101,594 | | 37.2 | 46.6 | 47.2 | 60.7 | 74.5 | 1,773,101 |
| pass, rep 1 | pre (3 s) | 58,185 | 19,395 | 37.9 | 47.7 | 48.2 | 60.9 | 73.4 | 5,402 |
| pass, rep 1 | post (3 s) | 45,511 | 15,170 | 38.3 | 47.8 | 48.4 | 62.7 | 85.0 | 344,726 |
| pass, rep 1 | whole run | 103,699 | | 37.9 | 47.8 | 48.3 | 61.8 | 76.1 | 1,848,976 |
| pass, rep 2 | pre (3 s) | 59,081 | 19,694 | 38.0 | 47.1 | 47.6 | 60.4 | 72.0 | 6,666 |
| pass, rep 2 | post (3 s) | 46,246 | 15,415 | 37.6 | 47.1 | 47.7 | 61.6 | 80.3 | 554,365 |
| pass, rep 2 | whole run | 105,331 | | 37.6 | 47.1 | 47.6 | 60.9 | 74.3 | 1,817,747 |
| control, rep 0 | pre (3 s) | 57,327 | 19,109 | 39.8 | 47.6 | 48.1 | 61.3 | 74.4 | 48,336 |
| control, rep 0 | post (3 s) | 57,549 | 19,183 | 38.2 | 47.5 | 48.0 | 59.8 | 71.4 | 53,943 |
| control, rep 0 | whole run | 136,872 | | 38.2 | 47.5 | 48.1 | 60.9 | 75.2 | 701,030 |
| control, rep 1 | pre (3 s) | 58,904 | 19,635 | 37.6 | 46.8 | 47.3 | 60.7 | 73.8 | 4,310 |
| control, rep 1 | post (3 s) | 59,546 | 19,849 | 39.1 | 46.7 | 47.2 | 59.0 | 70.6 | 4,832 |
| control, rep 1 | whole run | 140,784 | | 36.9 | 46.8 | 47.3 | 60.1 | 74.0 | 600,193 |
| control, rep 2 | pre (3 s) | 54,585 | 18,195 | 41.4 | 47.8 | 48.4 | 64.3 | 75.9 | 8,837 |
| control, rep 2 | post (3 s) | 58,419 | 19,473 | 38.1 | 47.7 | 48.2 | 59.5 | 70.7 | 4,317 |
| control, rep 2 | whole run | 135,736 | | 38.1 | 47.8 | 48.3 | 61.6 | 75.9 | 397,042 |

| rep | pass wall | pass server CPU | roll hold / rolls | longest non-yielding run | statements in flight during the pass | stalled statement |
|---|---|---|---|---|---|---|
| 0 | 1,801 ms | 663 ms | 1,119 ms / 3 | 1,395 ms | 3 | 1,773 ms (starts 29 ms after the pass) |
| 1 | 1,849 ms | 670 ms | 1,189 ms / 3 | 1,282 ms | 3 | 1,849 ms |
| 2 | 1,782 ms | 663 ms | 1,129 ms / 3 | 1,423 ms | 4 | 1,818 ms |

The stream's p0 to p99 are the control's to within the 1-3 us the control varies by, in every window, and the whole-run p99 (74.3-76.1 us) is the control's (74.0-75.9 us). **The pass is invisible below p99.999 and visible as exactly one statement**, whose latency equals the pass wall (1.78-1.85 s): the engine runs the pass without yielding and one core serves one connection's statement at a time, so a second connection's would stall alike. Under traffic the pass is longer than alone (1.78-1.85 s against 1.30 s) because the stream's own log, written in the 3 s before, puts a third roll in the pass (3 rolls, 1.12-1.19 s of roll hold against 0.63 s): the stall is set by where the pass starts in the 64 MiB segment, not only by its own bytes.

**The 0.3-0.7 s stall after the pass is not the pass's.** The pass runs' post window shows 14.1-15.4 k stmts/s against 19.2-19.8 k in the control's, and a statement of 345-597 ms in each; the control has a statement of 397-701 ms in each run too, 0.76-0.87 s after its gap began, where the pass runs show theirs 0.83-0.98 s after the pass ended. The stream dirties some 57,000 random rows over a relation of about 6,000 leaf pages (the checkpoint after the pass wrote 5,919-5,968 pages), so the same pages the pass dirties; the control cannot separate the two, and `SHOW META` over the stream shows one checkpoint run (1.6-1.75 s wall in the control, 3.3-3.4 s in the pass runs) and a transaction-id carve whose longest call was 245-653 ms (`contention_carve_longest_us`) in the control and 339-554 ms in the pass runs. The pass shifts when that stall lands (it holds the core past the instant the control's stall comes) and was not shown to add to it. That stall is a finding about a plain `UPDATE` stream on a million-row relation, outside CD; section 7 (item 6) says what to do with it.

**Under `strict` and `group` durability** the same cell 4 (10,000, 100,000 and 1,000,000 rows, 2 reps; extra to the work order, per rule 4b):

| durability | rows | rep | dirty pass: rows/s | dirty wall | server CPU | roll hold / rolls | longest non-yielding run | clean re-run wall | log bytes of the re-run |
|---|---|---|---|---|---|---|---|---|---|
| relaxed | 10,000 | 0 / 1 | 1.62 M / 1.66 M | 6.2 / 6.0 ms | 6 / 6 ms | 0 / 0 | 3 / 2 ms | 0.41 / 0.41 ms | 64 |
| relaxed | 100,000 | 0 / 1 | 1.56 M / 1.64 M | 64.0 / 60.8 ms | 63 / 61 ms | 0 / 0 | below threshold | 2.88 / 2.94 ms | 64 |
| relaxed | 1,000,000 | 0 / 1 | 768,241 / 768,858 | 1,302 / 1,301 ms | 693 / 666 ms | 626 / 642 ms, 2 rolls | 911 / 916 ms | 27.3 / 27.5 ms | 64 |
| strict | 10,000 | 0 / 1 | 888,047 / 718,956 | 11.3 / 13.9 ms | 6 / 9 ms | 0 / 0 | 3 / 8 ms | 2.73 / 2.53 ms | 64 |
| strict | 100,000 | 0 / 1 | 1.33 M / 1.33 M | 75.2 / 75.1 ms | 63 / 62 ms | 0 / 0 | below threshold | 6.00 / 6.70 ms | 64 |
| strict | 1,000,000 | 0 / 1 | 492,178 / 491,679 | 2,032 / 2,034 ms | 658 / 661 ms | 762 / 763 ms, 2 rolls | 1,584 / 1,625 ms | 31.0 / 128.9 ms | 64 |
| group | 10,000 | 0 / 1 | 915,681 / 867,943 | 10.9 / 11.5 ms | 6 / 6 ms | 0 / 0 | 2 / 0 ms | 2.23 / 59.0 ms | 64 |
| group | 100,000 | 0 / 1 | 1.33 M / 1.36 M | 75.1 / 73.7 ms | 62 / 62 ms | 0 / 0 | below threshold | 4.46 / 4.81 ms | 64 |
| group | 1,000,000 | 0 / 1 | 448,390 / 493,345 | 2,230 / 2,027 ms | 656 / 654 ms | 922 / 760 ms, 2 rolls | 1,861 / 1,639 ms | 29.1 / 29.5 ms | 64 |

**`strict` and `group` add the commit's sync to the pass and nothing else changes shape.** The CPU is the same (62-63 ms at 100,000 rows, 654-661 ms at 1,000,000), the 152 bytes of log per row are the same, and the wall grows by a sync: +10-14 ms at 100,000 rows (61-64 to 74-75 ms) and **+0.73 to +0.93 s at 1,000,000 (1.30 s to 2.03-2.23 s)**, with the longest non-yielding run 1.58-1.86 s against 0.91 s. The clean re-run's wall rises from 0.41 ms to 2.2-2.7 ms at 10,000 rows, which is one commit sync of a 64-byte log; two re-runs read 59 ms (`group`, 10,000 rows) and 129 ms (`strict`, 1,000,000) against 2.2-2.7 and 31 ms in their twin reps: a sync that met a busy device. Of the extra 0.73-0.93 s at 1,000,000 rows only about 0.12-0.30 s shows in the roll hold (760-922 ms against 626-642 ms); the rest was not decomposed and is most likely the commit's sync of the tail of the log the pass wrote, which `relaxed` does not wait for. Two reps per cell and one device: the strict-against-group difference (2,032 against 2,027-2,230 ms) is inside their own rep-to-rep spread.

## 4. ADD COLUMN: vacant slot against a reusing ADD

100 trials of each kind on 3-row relations, interleaved, the order of a pair alternating; wall latency in us, server CPU the median per statement. Each reusing trial is the full cycle `SPARE 1`, `ADD note`, `UPDATE` (a spill), `DROP`, `PURGE DROPPED COLUMNS`, then the timed `ADD`.

| ADD kind | n | p0 | p25 | p50 | p95 | p99 | max | statements/s at mean (derived) | server CPU (median) |
|---|---|---|---|---|---|---|---|---|---|
| vacant slot | 100 | 70.1 | 101.2 | 131.3 | 190.3 | 202.2 | 215.8 | 7,441 | 97.2 us |
| reusing a clean dropped slot | 100 | 71.3 | 106.2 | 142.7 | 213.8 | 222.4 | 223.8 | 6,938 | 109.4 us |

**A reusing `ADD` costs 9 us more at the median, paired by trial (reusing slower in 78 of 100, p25 +1.0 us, p75 +18.4 us; the medians of the two distributions are 131.3 and 142.7 us, +8.7 %), 7-9 % of the statement.** The p0 is 1.2 us apart: the reuse's extra work (the `PositionDependant` ask under the `X`, the Cabin sweep, `ForgetAccessStats`, an overwrite in place of an insert) sits in the body of the distribution, not in its floor. A 60-trial first run of this cell (superseded, archived as `p6-first`) read 107.0 against 109.0 us at the median, +1.9 %; so the cost is 2-9 % and the order of the two kinds in a pair moved neither (vacant first 136.0, second 134.4 us; reusing first 147.1, second 141.2 us). There is no A' control for this cell (B only); the pair-order split is the nearest thing to one. Row count is no part of the cost: the reusing `ADD` after the pass costs 105-125 us at every size from 200 to 1,000,000 rows (section 3's last column), the same as on a 3-row relation, because the statement is catalog-only. The cost at issue is 0.1 ms of DDL that runs a few times in a relation's life.

## 5. Against this engine's own previous numbers

The previous results for these shapes are `bench/v3.2.0/results-bw-close-v3.2.0-22-g541ce440.md`. Its B is `v3.2.0-22-g541ce440`; this run's A is `v3.3.0` (`f3ec25ea`), with BX (`v3.2.0-62`) through CC (`v3.2.0-184`) landed between them, and the host day, the number of runs and the gating differ. **Every difference between its numbers and this run's A is therefore not CD's**, and the rows are for league only; CD's delta is section 2's. Throughput is derived from server CPU as in section 2; BW's file reports the CPU in us, shown in brackets.

| shape | BW close, B (`v3.2.0-22-g541ce440`) | this run, A (`v3.3.0`) | this run, B (`v3.3.0-8-g842e0a42`) |
|---|---|---|---|
| `INSERT` literal / prepared (single row, positional) | 43,917 / 44,643 /s (22.77 / 22.40 us CPU) | 40,790 / 41,793 /s | 41,124 / 42,097 /s |
| point `SELECT` literal / prepared | 31,827 / 36,075 /s (31.42 / 27.72 us CPU) | 29,836 / 33,346 /s | 30,216 / 34,571 /s |
| every-row `UPDATE` of 1,000 rows literal / prepared | 1,298 / 1,227 /s (770.2 / 814.7 us CPU) | 1,218 / 1,232 /s | 1,226 / 1,248 /s |
| `bn` net scan row literal / prepared | 36.2 M / 36.0 M rows/s (27.60 / 27.80 ns) | 38.7 M / 38.4 M rows/s | 39.1 M / 39.0 M rows/s |

This run's A against BW's B: the scan row is 7 % faster, the point `SELECT` 6-8 % and the `INSERT` 6-7 % slower, the literal `UPDATE` 6 % slower and the prepared one level (+0.4 %). Those are differences in the state before CD touched anything, so none is CD's; B in this run sits within 1-2 % of A on each (section 2). The opposing signs on a scan and a point read, both server-CPU measures on one box, look more like a host day than an engine change, but the two cannot be separated from these files.

## 6. Waits

The measured unit of the A/B cells is a statement on one connection, `relaxed`, one core: no decomposition beyond the table applies. The pass has a decomposition, and it is the finding.

| wait type | INSERT / point SELECT / every-row UPDATE | clean pass at 1,000,000 rows (cell 4) | pass under traffic (cell 5) |
|---|---|---|---|
| durability / fsync | none: `relaxed` | **WAL segment-roll sync, inside the append under the stream latch: 626 / 642 ms of 1,302 / 1,301 ms (48-49 %)**, 2 rolls | **1,119-1,189 ms of 1,782-1,849 ms (62-64 %)**, 3 rolls |
| write statement (CPU) | `INSERT` 23.8-26.9 us CPU of wall p50 43.0-48.7 us; `UPDATE` 790-830 us CPU of wall p50 747-771 us (the CPU is every server thread and a mean over the arm, the wall the median of one statement) | 666-693 ms, all server threads | 663-670 ms |
| read | point `SELECT` 33.1 us CPU of wall p50 64.0 us (literal); scan row 25.6 ns | included in the walk (`pool_hits` 6.0 M, no `pool_misses` delta) | same |
| client and socket round trip | the remainder of wall over server CPU: about 20-30 us per point statement | none visible: one statement | the stalled statement's latency is the pass wall |
| lock or conflict wait | none: one connection | none: no `PurgeWait` poll showed (a 1 s poll would be a step in the wall); every pass cleaned all rows in one call | none |
| log ring full | n/a | 144 waits, 1 per MB of log, not separately timed | 144 |
| non-yielding run | n/a | **911-916 ms** longest run (scheduler counter) | 1,282-1,423 ms |

The `INSERT` and `SELECT` wall p50 is about twice the server CPU because each statement is two socket round trips on this box; the driver CPU (24-25 us) is the client side of the same trip, and the two overlap (`bn_census`'s docstring).

## 7. Insight

1. **CD adds nothing the steady-state paths can see.** The `INSERT` that pays CD-S1's bit reads inside the spread, and the within-arm comparison shows why the question has a floor: the dropped layout is 0.1-0.25 us dearer than an unaltered `SPARE 2` layout in B, and equally dearer in A, which writes no bit. The bit is one OR behind a branch that is already there. The diff's one change on a per-row hot path is `EncodeRow`'s dropped-column skip (`row_codec.cpp`, 9 lines); the scan kernel and the decoders are not in the 20 changed files, and the `INSERT` log call was folded into a shared `LogHeapWrite` (`wal_row_log.cpp`) with no cost the `INSERT` cell can see.
2. **OI-11 (the pass does not yield) has its first data point, and the data say the stall is half the log's.** The work order's section 5 predicted "a large relation holds its core for the walk, and the close measures that stall": 0.91 s at 1,000,000 rows, 64 ms at 100,000. The part of it that is not CPU is a `fsync` of the WAL segment the log leaves, taken inside the append under the stream latch (`wal.md` §6: BQ kept the roll's sync under the latch). So a yield between the pass's 512-key batches would let a point statement through between batches, but not through a roll that lands inside a batch. How much a yield would shorten the stall depends on how that roll time is spread, and the counters do not settle it: cell 5 reports 3 rolls and 1.12-1.19 s of total roll hold but a largest single hold (`wal_roll_hold_max_us`) of 14-185 ms, which this run could not reconcile. A yielding pass is therefore a hypothesis, not a measured option; `strict` and `group` make the stall longer (1.58-1.86 s non-yielding at 1,000,000 rows) because the commit's sync follows the log.
3. **The pass is a log writer first.** 152 bytes of log per cleaned row for a row of three `int64` and two 16-byte slots is a `HEAP_OVERWRITE` image plus the spill's release record; it means a pass over N rows writes 152 x N bytes, crosses a 64 MiB roll every 440,000 rows, and costs about 0.6 us per row plus a roll's sync per crossing. This is the first sizing number for the pass. It does not scale with the dropped column's width (the whole row is imaged), which is untested.
4. **A clean relation is cheap to confirm.** The re-run is a read walk at 27-36 ns per row (36 M rows/s at 1,000,000) that writes 64 bytes of log in all; "is it clean" can be asked of a large relation in tens of milliseconds, and the idempotence the work order claims shows as `0 row(s) cleaned` in all 25 re-runs (10 relaxed, 3 under traffic, 12 strict and group).
5. **Where a result contradicts an expectation.** None does: the work order expected the UPDATE flat, the INSERT bit negligible and the stall real, and they are. The one number the work order did not anticipate is the pass's dependence on its start position in the segment (0.63 s of roll hold alone, 1.12-1.19 s under traffic): the stall is not a function of N alone.
6. **A finding outside CD.** A plain autocommit `UPDATE` stream on a million-row relation, with no pass and no DDL, stalls for 397-701 ms once within about four seconds of starting, in each of three control runs, when the stream has dirtied most of the relation's pages and a checkpoint run (1.6-1.75 s) and a long transaction-id carve (245-653 ms) coincide with it. The cause was not isolated (a device-level `fsync` queued behind the checkpoint's page writes is one candidate); it deserves its own measurement, since a stall of that size is invisible to p99 and shows only in the max. No `docs/inflight/bugs/` entry was written here.

## 8. Tables

The per-invocation, per-start-order tables, the full `bn_census` and `bm_scan` percentile tables, the 18 stall timelines and the `SHOW META` deltas are in `archive/cd-close-v3.3.0-8-g842e0a42/out/analysis-{u,s,p}.md` (generated by `scripts/analyze.py` and `scripts/headline.py` from the JSON in the same directory). The percentile tables the headline rests on follow. `bn_census` emits p0, p25, p50, p90 and p99 (no p95: the driver is unmodified); `bm_scan` and `cd_pass.py` emit p95 as well. Medians over runs of each run's percentile; ops are totals over runs.

#### INSERT and every-row UPDATE, u1-u4 pooled (18 runs): statement wall latency in us; server CPU and driver CPU per statement in us

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU | driver CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | literal | A | 35,964 | 40.8 | 45.2 | 45.9 | 55.1 | 76.5 | 20,307 | 24.5 | 24.8 |
| insert | literal | A' | 35,964 | 39.9 | 45.4 | 45.9 | 54.9 | 76.7 | 20,332 | 24.5 | 24.8 |
| insert | literal | B | 35,964 | 40.0 | 45.2 | 45.8 | 54.6 | 77.3 | 20,360 | 24.3 | 24.8 |
| insert | prepared | A | 35,964 | 38.9 | 42.7 | 43.3 | 52.1 | 75.3 | 21,552 | 23.9 | 22.7 |
| insert | prepared | A' | 35,964 | 37.9 | 42.8 | 43.4 | 52.4 | 76.1 | 21,424 | 24.0 | 22.7 |
| insert | prepared | B | 35,964 | 38.2 | 42.4 | 43.0 | 52.0 | 74.3 | 21,590 | 23.8 | 22.7 |
| insert-dropped | literal | A | 35,964 | 45.4 | 47.9 | 48.5 | 59.0 | 85.2 | 19,173 | 26.9 | 25.5 |
| insert-dropped | literal | A' | 35,964 | 43.5 | 48.1 | 48.7 | 58.4 | 84.1 | 19,170 | 26.9 | 25.5 |
| insert-dropped | literal | B | 35,964 | 44.0 | 47.8 | 48.4 | 57.5 | 86.1 | 19,276 | 26.6 | 25.5 |
| insert-dropped | prepared | A | 35,964 | 39.5 | 44.5 | 45.1 | 55.0 | 78.6 | 20,670 | 25.9 | 22.8 |
| insert-dropped | prepared | A' | 35,964 | 41.5 | 44.6 | 45.1 | 55.2 | 80.9 | 20,544 | 26.0 | 22.9 |
| insert-dropped | prepared | B | 35,964 | 39.7 | 44.4 | 45.0 | 54.3 | 80.4 | 20,681 | 25.8 | 22.9 |
| update-range | literal | A | 2,160 | 724.2 | 756.2 | 770.6 | 1,026.2 | 1,441.3 | 1,157 | 820.9 | 39.0 |
| update-range | literal | A' | 2,160 | 724.1 | 756.8 | 768.7 | 954.4 | 1,619.1 | 1,196 | 790.0 | 38.9 |
| update-range | literal | B | 2,160 | 709.0 | 740.6 | 759.6 | 1,042.0 | 1,440.9 | 1,139 | 815.6 | 39.3 |
| update-range | prepared | A | 2,160 | 720.1 | 752.9 | 766.5 | 995.7 | 1,802.5 | 1,162 | 811.9 | 35.3 |
| update-range | prepared | A' | 2,160 | 722.5 | 754.7 | 768.1 | 1,046.2 | 2,361.3 | 1,148 | 830.0 | 34.8 |
| update-range | prepared | B | 2,160 | 704.6 | 734.4 | 746.6 | 1,009.2 | 2,162.1 | 1,184 | 801.4 | 34.6 |

#### Point SELECT by key, u1+u2 pooled (6 runs): statement wall latency in us

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU | driver CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| select-pk | literal | A | 11,988 | 56.4 | 58.6 | 64.4 | 73.0 | 91.8 | 15,288 | 33.5 | 32.6 |
| select-pk | literal | A' | 11,988 | 55.9 | 58.4 | 63.9 | 73.1 | 93.6 | 15,376 | 33.1 | 32.6 |
| select-pk | literal | B | 11,988 | 55.8 | 58.4 | 64.0 | 72.7 | 93.7 | 15,332 | 33.1 | 32.5 |
| select-pk | prepared | A | 11,988 | 53.5 | 55.3 | 56.7 | 66.4 | 82.6 | 16,766 | 30.0 | 29.9 |
| select-pk | prepared | A' | 11,988 | 52.9 | 54.8 | 56.3 | 65.2 | 83.0 | 16,738 | 29.4 | 29.9 |
| select-pk | prepared | B | 11,988 | 52.8 | 54.9 | 55.9 | 65.8 | 82.9 | 17,019 | 28.9 | 30.0 |

#### Scan, `bn`, 10,000 rows, s1+s2 pooled (7 runs): latency of one scan (wall) in us

| path | arm | ops | p0 | p25 | p50 | p95 | p99 | scans/s at p50 (derived) | server CPU per scan |
|---|---|---|---|---|---|---|---|---|---|
| literal | A | 672 | 303.6 | 311.4 | 321.5 | 362.4 | 401.7 | 3,111 | 290.4 us |
| literal | A' | 672 | 303.9 | 310.8 | 320.2 | 357.6 | 398.9 | 3,124 | 288.0 us |
| literal | B | 672 | 302.3 | 311.2 | 321.4 | 354.2 | 400.7 | 3,111 | 286.8 us |
| prepared | A | 672 | 300.5 | 308.9 | 319.3 | 366.2 | 401.2 | 3,132 | 290.0 us |
| prepared | A' | 672 | 299.6 | 307.3 | 319.7 | 349.1 | 403.9 | 3,128 | 286.9 us |
| prepared | B | 672 | 299.9 | 308.3 | 318.0 | 351.2 | 392.1 | 3,144 | 285.6 us |

Correctness: `bad` replies = 0 in every cell; the scan digest agrees across the three arms in every run (`digest arms-distinct` = 1 per cell); every `UPDATE` run's end state (`range_v_ok`, `spill_txt_ok`) passed in all arms; every pass cleaned exactly N rows and released N spills at every size, and the reusing `ADD` read NULL in N rows. The suite was not run in this task.
