# CV close (CV-S4) - the overhead off the shape, and the gain on it

Measured 2026-11-20, 03:57 to 04:01 UTC, for `instructions/v3.2.0/workorder-cv-ordered-index-walk.md` §3 (CV-R5) and §5 (CV-S4). Worktree `cv-ordered-index-walk`.

**Findings.** (1) Off the CV shapes, B (`7d11ba44`) is not slower than A (`f4138860`) beyond the noise floor in any of the 21 off-shape cells: the server CPU per statement differs by -3.3 % to +4.1 %, and every difference is inside the floor. (2) On the CV shapes the elided statement is 2x (a user of 100 rows) to 1,367x cheaper in server CPU than it was; "latest 20 of a user with 10,000 rows" went from 7,565 µs to 58.5 µs of server CPU and `ORDER BY id DESC LIMIT 20` from 39,223 µs to 28.7 µs. The CV-R5 cost is nil as far as this host can resolve it, which is about 1 % to 5 % per cell (see "The floor").

## Stamp

| | |
|---|---|
| executed | 2026-11-20, overhead run 03:57:16 to 03:58:19 UTC, premise run 03:59:34 to 04:01:18 UTC |
| branch / worktree | `worktree-cv-ordered-index-walk`, `/home/louis/ckdbs/.claude/worktrees/cv-ordered-index-walk` |
| engines | **A** = `f4138860` (`v3.4.15-1-gf4138860`), the commit CV opened from. **B** = `7d11ba44` (`v3.4.15-5-g7d11ba44`), CV's closing code. The worktree's HEAD at write time is `9d92a436` (`v3.4.15-9-g9d92a436`, `CLAUDE.md` modified in the tree); the binary measured is B's, not HEAD's |
| binary provenance | copies in `/home/louis/ck-tmp/cv-close/run/`: `kds_A` and `kds_A2` sha256 `7217d0eea81a83f20435d0548cc701e7857888cc7b9823bdc6f64dab139e3a21` (A2 is a byte-identical copy of A); `kds_B` sha256 `78b91fd276287cf6e3d67575b6f320a3e202a8534114eb1d9c840ebbdf848f7c`. Source binaries' mtime 2026-11-20 02:58:45 UTC, after B's commit time 02:56:34 UTC, so B is not older than its commit. Every server started from the copy; no rebuild |
| build | `build-release` (Release) |
| device | data files under `/home/louis/ck-tmp/cv-close/w1` and `w2`, `/dev/nvme0n1p1`, ext4; never tmpfs |
| host | Intel Xeon Platinum 8581C, 8 CPUs, Linux 6.12.116; each driver pins its server and itself to separate CPUs |
| server config | `cores = 1`, `buffer_pool_frames = 65536`, durability `relaxed` (overhead run) and `group` (premise run), as each driver writes it. Default for every other key |
| ports | 31980 to 31982 and 15741 (chosen) |
| drivers | `tools/cu_close_overhead.py` and `tools/cv_s0_premise.py`, unmodified |
| quiet gate | host loadavg was about 20 with 15 `cc1plus` at 02:59 UTC. The first gate held 45.7 min (02:59:15 to 03:45:02; `gate.log`), and a first pass launched at its release was **discarded** because a `cc1plus` appeared during it (see "What was cut"). The second pass waited for three consecutive polls with `cc1plus` = 0 and loadavg < 3: holds of 578 s and 75 s. Total hold, outside the time budget, about 57 min. Host load at each overhead cell: 2.32 to 2.89 (`q1.json`, field `load`); at each premise cell: 2.85 to 3.46 for A's (CV-S0) run and 1.53 to 2.22 for B's, `cc1plus` 0 in every cell (Appendix B) |
| time spent | about 1 min for the overhead run and 2 min for the premise run, plus the discarded first pass |

## 1. CV-R5: the overhead off the shape

**Method.** The driver starts three servers (A, B, A2) with fresh data files per run, loads `t1000` and `t100000` (`id, v, w, u` int64, index on `w`), and rotates the arm order per block. I ran `--runs 5 --blocks 6 --scale 1.0` (the driver's defaults), so each arm has five runs of 1,200 to 3,000 statements per cell (120 to 1,500 for the write cells at 100,000 rows). A table value is the **median of the five runs' values**. The row count is the relation size, `t1000` or `t100000`, the driver's two sizes (200 and 10K were not run; the shapes here are a point, a 10-row range, a fixed-size probe or a fixed-size write, which do not scale with rows, and the two sizes show it). The driver has no p95 and reports p90; the tables carry p0, p25, p50, p75, p99 and the mean. The statement rate is derived (`1,000,000 / mean µs`). The server CPU is the on-CPU time of the server's threads (`/proc/<pid>/task/*/schedstat`) divided by statements. The CU list cells (`update-in4`, `update-in64`, `update-conj`, `ins-del-in4`) run in the same pass and are included: they are off CV's shape.

**The floor.** Read from inside the run twice: A2 against A (same binary, two servers) and the spread of A's five runs (max minus min of the per-run CPU, over the median). A2 against A is within +/-1.9 % in 17 of 21 cells and reaches +3.5 % (update-scan, 1,000), -5.0 % (update-in4, 100,000), +3.8 % (update-in64, 100,000) and +4.2 % (ins-del-in4, 100,000). A's five-run spread is 2.8 % to 32.8 % (the largest at the point select at 1,000 rows, one outlying run). A delta of B over A is called a slowdown only if it is positive and larger than the larger of the two.

### Summary: server CPU per statement, B against A

| shape | rows | A server CPU / stmt | B server CPU / stmt | B vs A (CPU) | A2 vs A (CPU) | A run spread | B vs A (p50 latency) | B slower than A beyond the floor? |
|---|---|---|---|---|---|---|---|---|
| select-pk | 1,000 | 21.03 µs | 20.99 µs | -0.2 % (-0.04 µs) | -0.2 % | 32.8 % | -0.1 % | no: within the floor |
| select-pk | 100,000 | 21.86 µs | 22.10 µs | +1.1 % (+0.24 µs) | +0.1 % | 19.2 % | +0.2 % | no: within the floor |
| select-range | 1,000 | 29.36 µs | 28.82 µs | -1.8 % (-0.54 µs) | -0.1 % | 19.1 % | +0.1 % | no: within the floor |
| select-range | 100,000 | 30.81 µs | 31.05 µs | +0.8 % (+0.24 µs) | -0.3 % | 20.3 % | +0.2 % | no: within the floor |
| select-idx | 1,000 | 23.84 µs | 23.84 µs | +0.0 % (+0.01 µs) | +0.4 % | 15.9 % | +0.7 % | no: within the floor |
| select-idx | 100,000 | 25.24 µs | 25.01 µs | -0.9 % (-0.23 µs) | -0.9 % | 11.8 % | -0.2 % | no: within the floor |
| select-scan | 1,000 | 42.09 µs | 41.49 µs | -1.4 % (-0.60 µs) | +0.6 % | 7.8 % | -0.4 % | no: within the floor |
| select-join | 1,000 | 44.70 µs | 43.58 µs | -2.5 % (-1.11 µs) | -1.7 % | 4.1 % | -0.6 % | no: within the floor |
| update-pk | 1,000 | 22.77 µs | 22.75 µs | -0.1 % (-0.02 µs) | +0.2 % | 8.6 % | -0.1 % | no: within the floor |
| update-pk | 100,000 | 25.00 µs | 24.88 µs | -0.5 % (-0.12 µs) | +1.8 % | 8.1 % | -0.3 % | no: within the floor |
| update-scan | 1,000 | 95.91 µs | 97.59 µs | +1.8 % (+1.68 µs) | +3.5 % | 7.0 % | +0.9 % | no: within the floor |
| update-conj | 1,000 | 25.33 µs | 25.40 µs | +0.3 % (+0.07 µs) | +1.7 % | 7.4 % | -0.1 % | no: within the floor |
| update-conj | 100,000 | 32.91 µs | 31.81 µs | -3.3 % (-1.10 µs) | +0.9 % | 17.9 % | -2.7 % | no: within the floor |
| update-in4 | 1,000 | 33.56 µs | 33.76 µs | +0.6 % (+0.20 µs) | -0.2 % | 4.3 % | -0.3 % | no: within the floor |
| update-in4 | 100,000 | 45.86 µs | 44.93 µs | -2.0 % (-0.92 µs) | -5.0 % | 12.1 % | -0.7 % | no: within the floor |
| update-in64 | 1,000 | 191.55 µs | 196.58 µs | +2.6 % (+5.04 µs) | +1.9 % | 5.0 % | +2.1 % | no: within the floor |
| update-in64 | 100,000 | 262.84 µs | 273.68 µs | +4.1 % (+10.84 µs) | +3.8 % | 17.7 % | -0.7 % | no: within the floor |
| ins-del-in4 | 1,000 | 68.04 µs | 67.36 µs | -1.0 % (-0.68 µs) | +1.6 % | 2.8 % | -0.1 % | no: within the floor |
| ins-del-in4 | 100,000 | 71.57 µs | 71.50 µs | -0.1 % (-0.07 µs) | +4.2 % | 9.2 % | -0.2 % | no: within the floor |
| ins-del | 1,000 | 43.10 µs | 42.93 µs | -0.4 % (-0.17 µs) | +0.1 % | 5.9 % | +0.2 % | no: within the floor |
| ins-del | 100,000 | 43.47 µs | 43.65 µs | +0.4 % (+0.17 µs) | +0.1 % | 4.3 % | -0.7 % | no: within the floor |

The verdict column uses the wider of the two floors, and the run spread of A (up to 33 %) is much wider than A2's distance from A, so it is a lenient test. Read against A2's distance alone (the in-run control, 0.1 % to 5 %), B is above A by more than A2 is in two cells: `update-in64` at 1,000 rows (+2.6 % against A2's +1.9 %) and at 100,000 rows (+4.1 % against +3.8 %). Both exceed A2 by under one percentage point, the p50 latency does not move with them (+2.1 % and -0.7 %), and `update-in64` is a CU list shape on which CV changed nothing in its path; I call neither a finding, and a re-run of those two cells is what would settle them. Every other cell is inside A2's own distance, and the sign of B against A is mixed (13 negative or zero, 8 positive). The CV-R5 claim, "no new instruction off the shape", cannot be tested below this resolution: the new work is one byte test per accepted row, one per index step and a coroutine call per 64 pages, all far under the per-statement noise.

**Wait breakdown.** Not decomposed: every cell is a pool-resident statement on one core with `relaxed` durability (no commit fsync) over a local socket; the server CPU per statement is about 40 % to 50 % of the client-side mean on the point shapes and the rest is the client and socket round trip. No lock or conflict wait occurs on a single connection.

The per-arm table (A, A2, B for every shape and size; p0, p25, p50, p75, p99, mean, derived statements/s, CPU) is Appendix A.

## 2. CV's gain: S0's arms on B

**Method.** `tools/cv_s0_premise.py --bin kds_B --reps 2 --seconds 2.0`, the same arguments as CV-S0. A's column is CV-S0's own run (`archive/cv-s0-premise-v3.4.15-1-gf4138860/s0.json`, loadavg 2.85 to 3.46, `cc1plus` 0); B's is this run (loadavg 1.53 to 2.22, `cc1plus` 0). They are two invocations on a host that differed by about one load point, so D, which CV did not touch, is the cross-run floor: D's p50 is 61.0, 54.7, 61.6, 55.8, 68.7, 70.1 µs on A and 60.8, 50.9, 50.8, 51.9, 51.2, 51.6 µs on B (0 % to 26 % apart), and its server CPU is 25.1 to 29.2 µs on A against 23.1 to 24.7 µs on B (5 % to 21 % lower on B). Differences under about 25 % in the sub-200 µs rows are not findings; the ratios below are 2x to 1,367x. T and T2 are the same statement, so |T - T2| is the in-run floor for T.

**T is elided in every elided cell**, and the rows check holds: T and P returned equal rows in all six cells (`checks` in `s4.json`). T's `ANALYZE` on B:

| cell | T's `ANALYZE` step line | rows examined |
|---|---|---|
| latest 20, user of 100 | `step 0 IndexProbe cv_t order=index_desc` | 20 (100 index entries scanned, 20 resolved) |
| latest 20, user of 10,000 | `step 0 IndexProbe cv_t order=index_desc` | 20 (106 scanned, 20 resolved) |
| latest 20, user of 100,000 | `step 0 IndexProbe cv_t order=index_desc` | 20 (203 scanned, 20 resolved) |
| `ORDER BY id DESC LIMIT 20` | `step 0 Scan cv_t order=tuple_key_desc` | 20, 1 page (110,100 on A) |
| `... DESC LIMIT 20 OFFSET 1000` | `step 0 Scan cv_t order=tuple_key_desc` | 1,020, 8 pages |
| `ORDER BY id LIMIT 20 OFFSET 1000` (elided before CV) | `step 0 Scan cv_t` | 1,020, 8 pages |

P's plan on B also carries `order=index_desc` (an `IndexRange` over `cv_uc`), so on B the P arm is the walk with a bound and no longer a proxy for it.

### T arms, A against B (mean of the 2 repetitions)

| cell | A p0 / p25 / p50 / p75 / p99 | A | A server CPU | B p0 / p25 / p50 / p75 / p99 | B | B server CPU | A / B, CPU |
|---|---|---|---|---|---|---|---|
| latest 20, user of 100 | 140.2 / 150.7 / 167.6 / 202.6 / 283.2 µs | 5,577 stmts/s | 115.7 µs | 83.9 / 89.1 / 122.0 / 125.7 / 167.9 µs | 9,005 stmts/s | 53.0 µs | 2.2x |
| latest 20, user of 10,000 | 6,978.3 / 7,186.9 / 7,337.8 / 8,072.5 / 9,173.4 µs | 130 stmts/s | 7,565.3 µs | 83.6 / 99.5 / 126.0 / 156.3 / 171.4 µs | 7,706 stmts/s | 58.5 µs | 129x |
| latest 20, user of 100,000 | 68,602.7 / 70,350.5 / 71,309.2 / 72,916.0 / 84,339.0 µs | 14 stmts/s | 72,500.0 µs | 86.3 / 90.5 / 122.7 / 126.7 / 170.2 µs | 8,948 stmts/s | 54.2 µs | 1,337x |
| `ORDER BY id DESC LIMIT 20` | 34,075.9 / 38,151.8 / 38,987.6 / 40,469.3 / 45,960.9 µs | 26 stmts/s | 39,223.3 µs | 67.9 / 71.1 / 72.4 / 77.3 / 124.1 µs | 12,698 stmts/s | 28.7 µs | 1,367x |
| `... DESC LIMIT 20 OFFSET 1000` | 42,945.5 / 44,991.0 / 46,647.3 / 48,420.7 / 54,841.0 µs | 21 stmts/s | 46,860.4 µs | 154.8 / 165.4 / 168.4 / 175.8 / 270.5 µs | 5,648 stmts/s | 125.0 µs | 375x |
| `ORDER BY id LIMIT 20 OFFSET 1000` (not CV's shape) | 150.3 / 212.3 / 216.4 / 223.2 / 303.7 µs | 4,560 stmts/s | 131.3 µs | 146.8 / 157.6 / 161.7 / 176.6 / 260.9 µs | 5,780 stmts/s | 116.5 µs | 1.13x (inside the cross-run floor) |

Statements per repetition: B 17,962 / 15,370 / 17,852 / 25,312 / 11,280 / 11,542; A 11,134 / 260 / 28 / 52 / 43 / 9,102.

The in-run floor on B, |T - T2| at p50: 19.0, 37.7, 31.3, 0.2, 2.9 and 0.2 µs (0.1 % to 30 %). It is large on the three user cells because their p50 sits on a step between two modes (about 90 µs and about 125 µs; p0 and p25 are tight, 84 to 100 µs). The server CPU is steadier: T against T2 is 53.0 / 49.8, 58.5 / 46.0, 54.2 / 50.4, 28.7 / 28.2, 125.0 / 127.1 and 116.5 / 118.6 µs (1 % to 27 %). None of it touches the 129x to 1,367x.

### What each cell says

- **The per-row cost left the statement.** A's T was linear in the user's rows (7.4 ms at 10,000, 71 ms at 100,000); B's is flat at 53 to 59 µs of server CPU whatever the user owns and resolves 20 rows (the index scanned 100, 106 and 203 entries: one to a few leaves from the high end). The 100-row user gains only 2.2x because A's T was already only 115.7 µs there.
- **B's walk costs what CV-S0's proxy cost on A.** T on B is 53.0, 58.5 and 54.2 µs of CPU; P on A was 58.9, 55.5 and 57.7 µs (within 11 %, inside the cross-run floor). A re-descent per leaf (CV-Q1 (a)) does not show at 20 rows; D bounds it at 23 to 26 µs.
- **`id DESC LIMIT 20`** costs 28.7 µs against D's 23.4 µs on B: one descent plus about 5 µs.
- **`OFFSET 1000`** keeps its 1,020-row walk (8 pages): 125.0 µs on B against 46,860 µs on A. Ascending, elided before CV, it costs 116.5 µs; the reverse walk is 7 % above the forward one, about the in-run floor (T2: 127.1 against 118.6 µs).
- **CV-Q0's bar** was met on the proxy at CV-S0; with the walk built, "latest 20 of a user with 10k rows" is 129x cheaper than today's top-N and `ORDER BY id DESC LIMIT 20` 1,367x, in server CPU.

The per-arm table with P, D, T2, p0 to p99, load per repetition and `cc1plus` is Appendix B.

## What the run says about the engine

1. **CV's per-row work off its shape is below this host's resolution.** The three added tests show no cell slower than the floor; the floor, 1 % to 5 % against A2, is far wider than a per-statement cost of about 0.5 µs (the DF-S1 class), which this driver cannot observe and which was not claimed.
2. **The ordered walk makes the statement a point-select-sized cost.** T on B (53 to 59 µs) is 2.1 to 2.5 times a point select's CPU (D, 23 to 25 µs) for 20 resolved rows and a 20-row reply; the 7.5 to 72 ms before was entirely reading and sorting rows the page does not return. This is the first data point for CV-Q1 (a): one descent per leaf does not show at one to a few leaves.
3. **What was not measured, and no cell approached a limit (rule 4b).** Every `LIMIT 20` is satisfied from one or two leaves, so these cells are the walk's best case. A walk across many leaves (a large `LIMIT` or a residual that rejects most entries, where the re-descent per leaf accumulates) was not measured; the widest walk here is the 1,020-row `OFFSET` cell. Say so before reading CV-Q1 (a) as settled.

## What was cut, and what was discarded

- **Discarded first pass.** At the first gate release (03:45 UTC) both drivers were launched (overhead at 3 runs and scale 0.5); a `cc1plus` was visible at the start of the first and 8 at the end of the second (another session's build restarted). Both were dropped and re-run behind a stricter gate (three consecutive quiet polls). The first pass's JSON is under `archive/.../discarded-first-pass/` and is not read here.
- All 21 off-shape and list cells of the overhead driver ran, 5 runs each. Cut or absent: the sizes 200 and 10K (the driver's sizes are 1,000 and 100,000; see Method), a wait decomposition, the many-leaf walk cell, a PostgreSQL floor (not measured for these shapes) and the test suite (not run: no code was changed by this measurement).
- Baseline: A is `f4138860`'s own binary measured in this run. No earlier results file for this driver's off-shape cells at that commit exists, so the comparison is within-run; `bench/v3.4.11/results-cu-close-v3.4.11-40-g70a307ea.md` is the last file for the driver, at a different engine state, and is not compared.

Raw output: `bench/v3.4.15/archive/cv-close-v3.4.15-5-g7d11ba44/` (`q1.json`, `q1.log`, `s4.json`, `s4.log`, `go.log`, `gate.log`, `wd.log`, `an.py`, `an2.py`, `go2.sh`, `discarded-first-pass/`). Commands (from `go2.sh`, binaries as the hashed copies):

```
python3 tools/cu_close_overhead.py --arm A=kds_A --arm B=kds_B --arm A2=kds_A2 --workdir w1 --port 31980 --runs 5 --blocks 6 --scale 1.0 --out q1.json --progress q1.progress
python3 tools/cv_s0_premise.py --bin kds_B --workdir w2 --out s4.json --port 15741 --reps 2 --seconds 2.0
```

## Appendix A: overhead run, every arm (median of 5 runs; p75 in the place of p95, which the driver does not emit; stmts/s derived from the mean; the SUM rows of the analysis script are omitted)

| shape | rows | arm | ops/run | p0 | p25 | p50 | p75 | p99 | mean | QPS (derived) | server CPU/stmt | A2 vs A (cpu) | B vs A (cpu) | B vs A (p50) | verdict |
| select-pk | 1000 | A | 3000 | 48.5 | 50.3 | 51.1 | 53.7 | 77.3 | 54.5 | 18,345 | 21.03 | | | | |
| select-pk | 1000 | A2 | 3000 | 48.8 | 50.5 | 51.3 | 53.6 | 77.0 | 54.2 | 18,448 | 20.98 | | | | |
| select-pk | 1000 | B | 3000 | 48.3 | 50.2 | 51.1 | 53.7 | 78.4 | 53.9 | 18,541 | 20.99 | | | | |
| select-pk | 100000 | A | 3000 | 49.0 | 50.9 | 52.0 | 54.9 | 81.2 | 55.7 | 17,948 | 21.86 | | | | |
| select-pk | 100000 | A2 | 3000 | 49.3 | 51.1 | 52.1 | 54.8 | 80.6 | 56.7 | 17,628 | 21.89 | | | | |
| select-pk | 100000 | B | 3000 | 49.0 | 51.0 | 52.1 | 55.1 | 74.5 | 54.7 | 18,274 | 22.10 | | | | |
| select-range | 1000 | A | 3000 | 63.6 | 66.0 | 67.1 | 71.0 | 95.6 | 70.2 | 14,246 | 29.36 | | | | |
| select-range | 1000 | A2 | 3000 | 63.9 | 66.1 | 67.3 | 70.9 | 95.3 | 70.2 | 14,237 | 29.34 | | | | |
| select-range | 1000 | B | 3000 | 63.6 | 65.9 | 67.2 | 71.0 | 98.9 | 72.0 | 13,883 | 28.82 | | | | |
| select-range | 100000 | A | 3000 | 64.6 | 67.2 | 68.8 | 72.4 | 99.0 | 72.2 | 13,860 | 30.81 | | | | |
| select-range | 100000 | A2 | 3000 | 65.0 | 67.4 | 69.2 | 73.4 | 99.0 | 72.7 | 13,758 | 30.72 | | | | |
| select-range | 100000 | B | 3000 | 64.6 | 67.2 | 68.9 | 73.0 | 102.3 | 72.3 | 13,828 | 31.05 | | | | |
| select-idx | 1000 | A | 3000 | 50.4 | 53.0 | 54.2 | 56.6 | 79.4 | 56.7 | 17,629 | 23.84 | | | | |
| select-idx | 1000 | A2 | 3000 | 50.7 | 53.3 | 54.4 | 56.9 | 80.0 | 57.3 | 17,442 | 23.94 | | | | |
| select-idx | 1000 | B | 3000 | 50.8 | 53.3 | 54.6 | 58.1 | 79.8 | 57.3 | 17,438 | 23.84 | | | | |
| select-idx | 100000 | A | 3000 | 51.3 | 54.4 | 55.9 | 59.0 | 87.8 | 59.8 | 16,734 | 25.24 | | | | |
| select-idx | 100000 | A2 | 3000 | 51.5 | 54.4 | 55.8 | 58.5 | 80.3 | 57.9 | 17,277 | 25.02 | | | | |
| select-idx | 100000 | B | 3000 | 51.2 | 54.3 | 55.8 | 59.2 | 90.8 | 60.1 | 16,627 | 25.01 | | | | |
| select-scan | 1000 | A | 1200 | 68.9 | 70.9 | 71.9 | 75.1 | 106.9 | 75.5 | 13,242 | 42.09 | | | | |
| select-scan | 1000 | A2 | 1200 | 69.1 | 71.1 | 72.2 | 75.8 | 133.5 | 76.1 | 13,134 | 42.36 | | | | |
| select-scan | 1000 | B | 1200 | 68.4 | 70.6 | 71.6 | 74.9 | 104.5 | 74.7 | 13,381 | 41.49 | | | | |
| select-join | 1000 | A | 1200 | 70.5 | 72.8 | 74.5 | 79.0 | 124.4 | 79.2 | 12,627 | 44.70 | | | | |
| select-join | 1000 | A2 | 1200 | 70.6 | 72.9 | 74.1 | 77.8 | 124.5 | 77.7 | 12,863 | 43.94 | | | | |
| select-join | 1000 | B | 1200 | 68.3 | 72.4 | 74.0 | 77.8 | 135.6 | 78.1 | 12,810 | 43.58 | | | | |
| update-pk | 1000 | A | 3000 | 40.1 | 42.0 | 42.6 | 45.3 | 72.0 | 46.0 | 21,735 | 22.77 | | | | |
| update-pk | 1000 | A2 | 3000 | 39.7 | 42.0 | 42.8 | 45.6 | 71.6 | 46.2 | 21,648 | 22.82 | | | | |
| update-pk | 1000 | B | 3000 | 37.9 | 41.8 | 42.6 | 45.4 | 72.4 | 46.0 | 21,727 | 22.75 | | | | |
| update-pk | 100000 | A | 3000 | 41.1 | 43.0 | 44.7 | 48.5 | 88.0 | 48.4 | 20,665 | 25.00 | | | | |
| update-pk | 100000 | A2 | 3000 | 41.1 | 43.2 | 44.8 | 47.7 | 83.2 | 48.2 | 20,742 | 25.45 | | | | |
| update-pk | 100000 | B | 3000 | 41.2 | 43.0 | 44.6 | 47.8 | 87.2 | 47.9 | 20,867 | 24.88 | | | | |
| update-scan | 1000 | A | 1200 | 106.9 | 111.7 | 114.9 | 123.3 | 208.5 | 124.6 | 8,028 | 95.91 | | | | |
| update-scan | 1000 | A2 | 1200 | 107.0 | 112.7 | 116.4 | 125.0 | 196.3 | 124.2 | 8,049 | 99.27 | | | | |
| update-scan | 1000 | B | 1200 | 107.8 | 112.9 | 116.0 | 125.2 | 200.5 | 123.8 | 8,077 | 97.59 | | | | |
| update-conj | 1000 | A | 1200 | 42.5 | 44.1 | 45.0 | 48.0 | 87.6 | 48.3 | 20,701 | 25.33 | | | | |
| update-conj | 1000 | A2 | 1200 | 42.5 | 44.1 | 45.0 | 48.2 | 100.1 | 49.6 | 20,147 | 25.77 | | | | |
| update-conj | 1000 | B | 1200 | 42.4 | 44.0 | 44.9 | 48.2 | 82.7 | 48.3 | 20,688 | 25.40 | | | | |
| update-conj | 100000 | A | 120 | 44.3 | 47.1 | 49.8 | 55.9 | 166.4 | 57.4 | 17,407 | 32.91 | | | | |
| update-conj | 100000 | A2 | 120 | 44.2 | 46.0 | 48.0 | 54.6 | 193.0 | 58.5 | 17,089 | 33.21 | | | | |
| update-conj | 100000 | B | 120 | 43.9 | 45.9 | 48.4 | 54.8 | 149.6 | 56.5 | 17,694 | 31.81 | | | | |
| update-in4 | 1000 | A | 1200 | 51.2 | 53.1 | 54.4 | 58.4 | 110.3 | 59.0 | 16,955 | 33.56 | | | | |
| update-in4 | 1000 | A2 | 1200 | 51.5 | 53.1 | 54.2 | 57.2 | 100.5 | 57.9 | 17,265 | 33.50 | | | | |
| update-in4 | 1000 | B | 1200 | 51.3 | 53.2 | 54.2 | 58.1 | 102.8 | 58.8 | 17,013 | 33.76 | | | | |
| update-in4 | 100000 | A | 120 | 55.3 | 59.5 | 62.3 | 70.4 | 223.3 | 73.6 | 13,579 | 45.86 | | | | |
| update-in4 | 100000 | A2 | 120 | 54.7 | 57.7 | 61.0 | 67.4 | 181.7 | 71.9 | 13,907 | 43.58 | | | | |
| update-in4 | 100000 | B | 120 | 54.9 | 58.6 | 61.8 | 71.8 | 204.5 | 72.0 | 13,892 | 44.93 | | | | |
| update-in64 | 1000 | A | 600 | 200.0 | 208.0 | 218.2 | 240.3 | 493.7 | 244.1 | 4,097 | 191.55 | | | | |
| update-in64 | 1000 | A2 | 600 | 199.9 | 207.9 | 217.3 | 243.3 | 502.7 | 249.0 | 4,016 | 195.24 | | | | |
| update-in64 | 1000 | B | 600 | 201.9 | 210.7 | 222.7 | 246.7 | 505.4 | 248.0 | 4,032 | 196.58 | | | | |
| update-in64 | 100000 | A | 120 | 241.6 | 271.2 | 305.1 | 366.0 | 791.4 | 364.3 | 2,745 | 262.84 | | | | |
| update-in64 | 100000 | A2 | 120 | 242.4 | 267.4 | 312.2 | 373.1 | 759.0 | 344.0 | 2,907 | 272.71 | | | | |
| update-in64 | 100000 | B | 120 | 242.5 | 271.8 | 302.9 | 365.4 | 823.3 | 352.0 | 2,841 | 273.68 | | | | |
| ins-del-in4 | 1000 | A | 240 | 99.4 | 102.7 | 106.9 | 117.9 | 253.5 | 117.5 | 8,511 | 68.04 | | | | |
| ins-del-in4 | 1000 | A2 | 240 | 99.8 | 103.6 | 107.4 | 119.1 | 272.8 | 117.5 | 8,513 | 69.11 | | | | |
| ins-del-in4 | 1000 | B | 240 | 99.4 | 102.7 | 106.8 | 117.7 | 252.8 | 119.0 | 8,407 | 67.36 | | | | |
| ins-del-in4 | 100000 | A | 120 | 101.1 | 105.0 | 110.3 | 120.7 | 261.7 | 123.0 | 8,132 | 71.57 | | | | |
| ins-del-in4 | 100000 | A2 | 120 | 100.8 | 104.5 | 111.1 | 126.4 | 341.8 | 137.7 | 7,260 | 74.58 | | | | |
| ins-del-in4 | 100000 | B | 120 | 100.4 | 104.3 | 110.1 | 123.1 | 300.6 | 124.0 | 8,063 | 71.50 | | | | |
| ins-del | 1000 | A | 1500 | 77.9 | 80.5 | 82.7 | 87.5 | 132.3 | 86.7 | 11,533 | 43.10 | | | | |
| ins-del | 1000 | A2 | 1500 | 78.2 | 80.8 | 83.4 | 88.1 | 130.9 | 87.6 | 11,410 | 43.13 | | | | |
| ins-del | 1000 | B | 1500 | 78.4 | 80.7 | 82.9 | 88.3 | 132.8 | 87.3 | 11,461 | 42.93 | | | | |
| ins-del | 100000 | A | 1500 | 78.5 | 81.1 | 83.1 | 87.8 | 136.9 | 88.1 | 11,357 | 43.47 | | | | |
| ins-del | 100000 | A2 | 1500 | 78.6 | 80.8 | 82.8 | 87.6 | 135.1 | 87.5 | 11,424 | 43.50 | | | | |
| ins-del | 100000 | B | 1500 | 78.4 | 80.8 | 82.5 | 88.2 | 139.9 | 89.0 | 11,235 | 43.65 | | | | |

## Appendix B: premise run, every arm (mean of 2 repetitions; A = CV-S0 at f4138860, B = this run)

| cell | arm | engine | n/rep | p0 | p25 | p50 | p75 | p99 | stmts/s | server CPU/stmt | load (per cell reps) |
| latest20_user100 | T | A (f4138860) | 11,134 | 140.2 µs | 150.7 µs | 167.6 µs | 202.6 µs | 283.2 µs | 5,577 stmts/s | 115.7 µs | 3.09, 3.00; cc1plus 0,0 |
| latest20_user100 | T | B (7d11ba44) | 17,962 | 83.9 µs | 89.1 µs | 122.0 µs | 125.7 µs | 167.9 µs | 9,005 stmts/s | 53.0 µs | 2.16, 2.22; cc1plus 0,0 |
| latest20_user100 | P | A (f4138860) | 16,380 | 87.5 µs | 94.2 µs | 115.7 µs | 140.3 µs | 191.1 µs | 8,211 stmts/s | 58.9 µs | 3.09, 2.92; cc1plus 0,0 |
| latest20_user100 | P | B (7d11ba44) | 15,435 | 85.1 µs | 122.8 µs | 126.0 µs | 159.9 µs | 178.4 µs | 7,738 stmts/s | 59.9 µs | 2.16, 2.12; cc1plus 0,0 |
| latest20_user100 | D | A (f4138860) | 31,872 | 47.8 µs | 51.4 µs | 61.0 µs | 69.6 µs | 105.2 µs | 16,010 stmts/s | 25.9 µs | 3.00, 3.00; cc1plus 0,0 |
| latest20_user100 | D | B (7d11ba44) | 32,788 | 48.3 µs | 53.0 µs | 60.8 µs | 63.6 µs | 90.7 µs | 16,468 stmts/s | 24.7 µs | 2.22, 2.13; cc1plus 0,0 |
| latest20_user100 | T2 | A (f4138860) | 10,940 | 139.2 µs | 147.3 µs | 183.2 µs | 213.4 µs | 272.4 µs | 5,480 stmts/s | 115.3 µs | 3.00, 3.00; cc1plus 0,0 |
| latest20_user100 | T2 | B (7d11ba44) | 18,076 | 84.7 µs | 90.0 µs | 103.0 µs | 126.5 µs | 172.1 µs | 9,063 stmts/s | 49.8 µs | 2.22, 2.13; cc1plus 0,0 |
| latest20_user10000 | T | A (f4138860) | 260 | 6,978.3 µs | 7,186.9 µs | 7,337.8 µs | 8,072.5 µs | 9,173.4 µs | 130 stmts/s | 7,565.3 µs | 2.92, 3.02; cc1plus 0,0 |
| latest20_user10000 | T | B (7d11ba44) | 15,370 | 83.6 µs | 99.5 µs | 126.0 µs | 156.3 µs | 171.4 µs | 7,706 stmts/s | 58.5 µs | 2.12, 1.94; cc1plus 0,0 |
| latest20_user10000 | P | A (f4138860) | 17,628 | 87.8 µs | 92.7 µs | 101.0 µs | 133.4 µs | 192.3 µs | 8,836 stmts/s | 55.5 µs | 2.85, 3.02; cc1plus 0,0 |
| latest20_user10000 | P | B (7d11ba44) | 16,324 | 84.6 µs | 89.3 µs | 123.9 µs | 158.2 µs | 179.1 µs | 8,183 stmts/s | 56.6 µs | 2.03, 1.87; cc1plus 0,0 |
| latest20_user10000 | D | A (f4138860) | 33,650 | 47.6 µs | 50.8 µs | 54.7 µs | 66.2 µs | 95.8 µs | 16,900 stmts/s | 25.1 µs | 2.85, 3.02; cc1plus 0,0 |
| latest20_user10000 | D | B (7d11ba44) | 36,603 | 47.3 µs | 50.0 µs | 50.9 µs | 54.9 µs | 80.7 µs | 18,381 stmts/s | 23.2 µs | 2.03, 1.87; cc1plus 0,0 |
| latest20_user10000 | T2 | A (f4138860) | 254 | 6,962.7 µs | 7,256.6 µs | 7,389.9 µs | 7,529.2 µs | 10,219.0 µs | 127 stmts/s | 7,363.7 µs | 3.02, 3.02; cc1plus 0,0 |
| latest20_user10000 | T2 | B (7d11ba44) | 21,014 | 83.2 µs | 86.6 µs | 88.3 µs | 92.5 µs | 160.6 µs | 10,534 stmts/s | 46.0 µs | 2.03, 1.94; cc1plus 0,0 |
| latest20_user100000 | T | A (f4138860) | 28 | 68,602.7 µs | 70,350.5 µs | 71,309.2 µs | 72,916.0 µs | 84,339.0 µs | 14 stmts/s | 72,500.0 µs | 3.10, 3.01; cc1plus 0,0 |
| latest20_user100000 | T | B (7d11ba44) | 17,852 | 86.3 µs | 90.5 µs | 122.7 µs | 126.7 µs | 170.2 µs | 8,948 stmts/s | 54.2 µs | 1.80, 1.73; cc1plus 0,0 |
| latest20_user100000 | P | A (f4138860) | 16,950 | 88.6 µs | 94.6 µs | 103.3 µs | 140.2 µs | 195.2 µs | 8,497 stmts/s | 57.7 µs | 3.10, 3.24; cc1plus 0,0 |
| latest20_user100000 | P | B (7d11ba44) | 17,897 | 84.8 µs | 88.8 µs | 121.1 µs | 127.2 µs | 173.2 µs | 8,972 stmts/s | 52.3 µs | 1.80, 1.59; cc1plus 0,0 |
| latest20_user100000 | D | A (f4138860) | 31,926 | 46.7 µs | 51.5 µs | 61.6 µs | 69.4 µs | 99.9 µs | 16,035 stmts/s | 25.6 µs | 3.01, 3.09; cc1plus 0,0 |
| latest20_user100000 | D | B (7d11ba44) | 36,610 | 47.5 µs | 50.0 µs | 50.8 µs | 55.1 µs | 78.0 µs | 18,383 stmts/s | 23.1 µs | 1.80, 1.59; cc1plus 0,0 |
| latest20_user100000 | T2 | A (f4138860) | 27 | 67,479.9 µs | 68,727.4 µs | 75,205.2 µs | 77,847.1 µs | 85,439.0 µs | 14 stmts/s | 74,175.9 µs | 3.01, 3.09; cc1plus 0,0 |
| latest20_user100000 | T2 | B (7d11ba44) | 19,427 | 86.1 µs | 89.4 µs | 91.4 µs | 124.2 µs | 166.9 µs | 9,738 stmts/s | 50.4 µs | 1.73, 1.59; cc1plus 0,0 |
| id_desc_limit20 | T | A (f4138860) | 52 | 34,075.9 µs | 38,151.8 µs | 38,987.6 µs | 40,469.3 µs | 45,960.9 µs | 26 stmts/s | 39,223.3 µs | 3.24, 3.21; cc1plus 0,0 |
| id_desc_limit20 | T | B (7d11ba44) | 25,312 | 67.9 µs | 71.1 µs | 72.4 µs | 77.3 µs | 124.1 µs | 12,698 stmts/s | 28.7 µs | 1.63, 1.53; cc1plus 0,0 |
| id_desc_limit20 | P | A (f4138860) | 20,020 | 74.7 µs | 80.0 µs | 88.3 µs | 119.8 µs | 174.5 µs | 10,039 stmts/s | 40.4 µs | 3.14, 3.19; cc1plus 0,0 |
| id_desc_limit20 | P | B (7d11ba44) | 25,080 | 70.8 µs | 74.0 µs | 75.2 µs | 78.2 µs | 140.3 µs | 12,580 stmts/s | 30.9 µs | 1.63, 1.65; cc1plus 0,0 |
| id_desc_limit20 | D | A (f4138860) | 33,160 | 47.8 µs | 51.1 µs | 55.8 µs | 68.0 µs | 106.2 µs | 16,653 stmts/s | 25.3 µs | 3.14, 3.19; cc1plus 0,0 |
| id_desc_limit20 | D | B (7d11ba44) | 35,965 | 48.1 µs | 50.8 µs | 51.9 µs | 58.6 µs | 78.4 µs | 18,060 stmts/s | 23.4 µs | 1.58, 1.53; cc1plus 0,0 |
| id_desc_limit20 | T2 | A (f4138860) | 52 | 34,586.7 µs | 35,954.8 µs | 39,652.2 µs | 40,400.9 µs | 44,555.2 µs | 26 stmts/s | 38,290.9 µs | 3.14, 3.21; cc1plus 0,0 |
| id_desc_limit20 | T2 | B (7d11ba44) | 25,509 | 67.8 µs | 71.2 µs | 72.6 µs | 76.9 µs | 120.8 µs | 12,796 stmts/s | 28.2 µs | 1.58, 1.53; cc1plus 0,0 |
| id_desc_limit20_offset1000 | T | A (f4138860) | 43 | 42,945.5 µs | 44,991.0 µs | 46,647.3 µs | 48,420.7 µs | 54,841.0 µs | 21 stmts/s | 46,860.4 µs | 3.10, 3.17; cc1plus 0,0 |
| id_desc_limit20_offset1000 | T | B (7d11ba44) | 11,280 | 154.8 µs | 165.4 µs | 168.4 µs | 175.8 µs | 270.5 µs | 5,648 stmts/s | 125.0 µs | 1.65, 1.70; cc1plus 0,0 |
| id_desc_limit20_offset1000 | P | A (f4138860) | 3,542 | 459.8 µs | 513.8 µs | 543.3 µs | 585.2 µs | 981.8 µs | 1,772 stmts/s | 466.3 µs | 3.10, 3.45; cc1plus 0,0 |
| id_desc_limit20_offset1000 | P | B (7d11ba44) | 9,524 | 181.2 µs | 189.9 µs | 193.3 µs | 206.8 µs | 367.4 µs | 4,768 stmts/s | 151.8 µs | 1.68, 1.65; cc1plus 0,0 |
| id_desc_limit20_offset1000 | D | A (f4138860) | 29,599 | 47.8 µs | 57.2 µs | 68.7 µs | 70.7 µs | 122.5 µs | 14,871 stmts/s | 27.2 µs | 3.17, 3.40; cc1plus 0,0 |
| id_desc_limit20_offset1000 | D | B (7d11ba44) | 36,910 | 47.8 µs | 50.3 µs | 51.2 µs | 53.8 µs | 77.2 µs | 18,533 stmts/s | 23.1 µs | 1.68, 1.65; cc1plus 0,0 |
| id_desc_limit20_offset1000 | T2 | A (f4138860) | 42 | 42,908.2 µs | 46,000.3 µs | 47,509.1 µs | 49,522.5 µs | 57,810.3 µs | 21 stmts/s | 47,657.8 µs | 3.17, 3.40; cc1plus 0,0 |
| id_desc_limit20_offset1000 | T2 | B (7d11ba44) | 10,544 | 155.3 µs | 166.3 µs | 171.3 µs | 209.5 µs | 285.5 µs | 5,280 stmts/s | 127.1 µs | 1.70, 1.70; cc1plus 0,0 |
| id_asc_limit20_offset1000 | T | A (f4138860) | 9,102 | 150.3 µs | 212.3 µs | 216.4 µs | 223.2 µs | 303.7 µs | 4,560 stmts/s | 131.3 µs | 3.45, 3.46; cc1plus 0,0 |
| id_asc_limit20_offset1000 | T | B (7d11ba44) | 11,542 | 146.8 µs | 157.6 µs | 161.7 µs | 176.6 µs | 260.9 µs | 5,780 stmts/s | 116.5 µs | 1.67, 1.62; cc1plus 0,0 |
| id_asc_limit20_offset1000 | P | A (f4138860) | 7,980 | 175.6 µs | 240.5 µs | 244.6 µs | 251.8 µs | 354.4 µs | 3,997 stmts/s | 160.4 µs | 3.41, 3.42; cc1plus 0,0 |
| id_asc_limit20_offset1000 | P | B (7d11ba44) | 10,008 | 174.4 µs | 181.5 µs | 185.4 µs | 204.8 µs | 304.7 µs | 5,011 stmts/s | 142.9 µs | 1.67, 1.60; cc1plus 0,0 |
| id_asc_limit20_offset1000 | D | A (f4138860) | 27,507 | 49.0 µs | 69.0 µs | 70.1 µs | 71.7 µs | 118.2 µs | 13,822 stmts/s | 29.2 µs | 3.41, 3.42; cc1plus 0,0 |
| id_asc_limit20_offset1000 | D | B (7d11ba44) | 35,490 | 47.6 µs | 50.4 µs | 51.6 µs | 61.6 µs | 80.6 µs | 17,821 stmts/s | 23.6 µs | 1.62, 1.65; cc1plus 0,0 |
| id_asc_limit20_offset1000 | T2 | A (f4138860) | 9,092 | 150.5 µs | 212.4 µs | 215.9 µs | 221.9 µs | 306.2 µs | 4,555 stmts/s | 132.0 µs | 3.41, 3.46; cc1plus 0,0 |
| id_asc_limit20_offset1000 | T2 | B (7d11ba44) | 11,222 | 145.6 µs | 157.8 µs | 161.9 µs | 201.3 µs | 267.8 µs | 5,620 stmts/s | 118.6 µs | 1.62, 1.65; cc1plus 0,0 |
