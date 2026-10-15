# AQ close: a column Cabin's write and its served read show no change; the every-row UPDATE of a relation with no Cabin costs 3-4 % more server CPU at `a5e33998` than at `7c234766`

**Thesis.** On `worktree-aq-expression-cabins`, the engine at `a5e33998` (`v3.2.0-48-ga5e33998`, **B**, AQ code complete) against AQ's opening, `7c234766` (`v3.2.0-44-g7c234766`, **A**): the column-Cabin write path and the Cabin-served read, the two cells AQ-S5 says "must show none", read B - A inside the A-against-A' spread (single-row `INSERT` into a relation carrying a column Cabin: -0.46 / -0.26 us, B below A in 6 of 6 / 5 of 6 pairs; `WHERE sym = 'x'` served from the Cabin: +0.26 / +0.35 us, +0.8 % / +1.1 %), with one exception stated in section 2 (the same served read at 10,000 rows, prepared path, +1.5 us, +2.8 %, 3 of 3 runs above A and 0.1 us past the largest A' gap). The two cells that touch every relation but no Cabin are **not** at zero: the **every-row `UPDATE` of 1,000 rows costs +29.5 us literal (+4.0 %) and +23.0 us prepared (+2.9 %) more server CPU at B**, B above A in 6 of 6 and 5 of 6 clean pairs and above A' in the same runs, against a typical A' - A gap of 10 us; and the point `SELECT` by tuple key reads +0.97 us literal (+3.1 %) / +0.49 us prepared (+1.8 %), outside the largest A' gap on the literal path by 0.17 us. The single-row `INSERT` into a relation with no Cabin reads +0.33 / +0.32 us (+1.5 %), inside the spread, and the c-phase `INSERT` into the Cabin-free relation reads -0.31 / -0.30 us, so it is "not resolved", not a finding. **The expression Cabin's write cost, which AQ-S5 states as the feature's**: a single-row `INSERT` into a relation with a `DATE(ts)` Cabin costs +0.73 us literal / +0.92 us prepared (+3.1 % / +4.0 %) over the same `INSERT` into an identical relation with no Cabin, B only, 7 of 8 pairs above; a column Cabin costs +0.59 / +0.72 us for the same append, so the expression Cabin's write is a column Cabin's write plus about 0.5 us (section 5). A `DATE(ts) = d` `SELECT` served from the expression Cabin costs 39.9 us of server CPU against 115.9 us walked over the same 1,000 rows (2.9x less CPU; p50 wall 95.6 against 172.7 us, 10,022 against 5,616 statements/s from the mean).

The `UPDATE` shift is the second time a close finds +30 us on this statement (`results-bw-close-v3.2.0-22-g541ce440.md`), and it is again a per-row cost (section 2: 200 / 1,000 / 10,000 rows give +3.9 / +29.5 / +271.7 us literal). Its cause was not profiled; section 6 says what the diff does and does not show.

Server CPU is the sum of `/proc/<pid>/task/*/schedstat`; wall is the client's. Driver flags are in the drivers' docstrings (`bench/README.md`: `bench/docs/` stays closed); the exact invocations are `archive/aq-close-v3.2.0-48-ga5e33998/scripts/run.sh` and `run2.sh`, the new driver is `scripts/aq_cabin.py`.

| Item | Value |
|---|---|
| Executed | 2026-10-15, UTC. Builds 00:59:53-01:01:35; measurement 01:02:24-01:36:24 (phases u1, c1, u2, c2, x in `progress.txt`, then u3, r200, r10k, c200, c10k). Whole task 00:59 to the end of this file, inside the one-hour budget |
| Worktree / branch | `aq-expression-cabins`, branch `worktree-aq-expression-cabins`. HEAD was `a5e33998` when the first phase started and moved to `fecfb88a` (the parent's prose commit and a merge of origin/main) during the run; **nothing measured is HEAD's**: both arms are built from `git archive` of the two commits below |
| Commits measured | **A** = `7c234766` (`v3.2.0-44-g7c234766`, committed 2026-10-14 23:04:20 UTC, where AQ opened). **B** = `a5e33998` (`v3.2.0-48-ga5e33998`, committed 2026-10-15 00:59:02 UTC; AQ-S1 `f3e6d1fd`, S2 `1099ad88`, S3 `fce79b85`, S4 `a5e33998`). A' = A's bytes started as a second server (the in-run control); in the B-only phase **B2** = B's bytes started twice |
| Binary provenance | Built from `git archive <commit>` into `/home/cdkbs/bench-runs/aq-close/src-{A,B}` and `build-{A,B}`; copies in `bin/`, hashed after the copy (`archive/.../binaries.txt`). **A and A'** sha256 `b6d708801f322776b1b6b1754c4040e7a7bf9f0e0b168d5b2cd3de5fc8ab3278`, source mtime 2026-10-15 01:01:32.23 UTC. **B** `2216f1c3465209b954bc7eab3b7e460efcaf30725fe8f0c3ade95e4935dba0c3`, source mtime 01:01:33.30 UTC. Both newer than their commits and built from exactly them; every server started from the copies |
| Build | `CMAKE_BUILD_TYPE=Release` (read from each `CMakeCache.txt`), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, `-j4` each, built together (1 min 42 s) and finished before any measurement started; gcc 13.3.0 |
| Host / device | 8 vCPU AMD EPYC 9V74 (4 cores x 2 threads), Linux 7.0.0-1014-azure. Data files and WAL under `/home/cdkbs/bench-runs/aq-close/wd-*/` on `/dev/root`, **ext4, not tmpfs** (`df -T`). Load average at the start of the build 5.0 (other sessions' work, decaying), 3.2 at the start of measurement; per-phase gate loads in section 1 |
| Pinning | One server process per arm, one busy at a time, on CPU 2; the driver on CPU 4; as BW-close |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, `scan_kernel` default (on); Cabins on (the default); fresh server and data file per arm per run. `relaxed` means no fsync wait is in any measured statement |
| Drivers | `tools/bn_census.py` sha256 `35693dc2...`, `tools/kwp.py` `99341280...` (both as of `fecfb88a`; neither changed between BW-close and `a5e33998` except `9f5faf50` adding a shape), new `archive/.../scripts/aq_cabin.py` `7e26a4e1...` (it imports `bn_census` whole and swaps its relations and shapes). Gates: `--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1 --quiet-load 2` |
| Suite | Not executed in this task. No source, test, CMake or doc file was changed; only `bench/` was written. The tree had no uncommitted source changes at the end (`git status` showed only this archive directory) |
| PostgreSQL floor | Not measured for these shapes (rule 4: a missing twin is stated, not apologised for) |

## 1. What was run, and what was cut

Three arms (A, A', B), a fresh server and data file per arm per run, the arms interleaved block by block (6 blocks per cell, the order rotating one place per block and per run), the two driver paths (literal: an unnamed `C_PARSE` of the text per execution; prepared: one named `C_PARSE`, then bind/execute) alternating inside a run. `--rows N` is the relation's size and, for `update-range`, the number of rows the `UPDATE r SET v = ? WHERE id >= 1` writes; the point shapes do 2,000 statements per path per arm per run (1,200 at 10,000 rows for the Cabin shapes), `update-range` 120 statements (240 at 200 rows, 24 at 10,000: the same rows written per cell, within 2x).

| phase | driver, shapes | rows | server start order | runs | gate result |
|---|---|---|---|---|---|
| u1 | `bn_census` `select-pk`, `insert`, `update-range` | 1,000 | B, A', A | 3 | gate load 1.9-2.1; 0 unclean windows |
| u2 | same | 1,000 | A, A', B | 3 | gate load 0.6-1.0; 0 unclean |
| c1 | `aq_cabin` `plain-insert`, `cab-insert`, `plain-select`, `cab-select` | 1,000 | B, A', A | 3 | gate load 0.7-1.5; 0 unclean |
| c2 | same | 1,000 | A, A', B | 3 | gate load 0.4-0.6; 0 unclean |
| x | `aq_cabin`, B and B2 only: the four above plus `x-insert-plain`, `x-insert`, `x-walk`, `x-select` | 1,000 | B, B2 | 4 | gate load 0.55-1.0; 0 unclean |
| u3 | `bn_census` `select-pk`, `update-range` | 1,000 | A', B, A | 5 of 6 | **gate load 2.7-7.0 (another session on the host); 12 unclean windows**; the sixth run did not start ("server A2 did not listen on 32881", `u3.log`) and was not repeated |
| r200 | `bn_census` three shapes | 200 | A, A', B | 3 | gate load 2.5-5.6; **12 unclean windows** |
| r10k | same | 10,000 | B, A', A | 3 | gate load 1.7-2.0; 3 unclean windows (run 2, `select-pk` literal) |
| c200 | `aq_cabin` four shapes | 200 | A, A', B | 3 | 0 unclean |
| c10k | same | 10,000 | B, A', A | 3 | 0 unclean |

The clean 1,000-row phases (u1, u2, c1, c2, x) carry the verdicts. u3, r200 and r10k were run on a loaded host; they are used for direction and for the row sweep, flagged where they are, and never alone for a verdict. Bad replies: 0 in every phase. `range_v_ok`/`spill_txt_ok` end-state reads: all true in u1, u2, u3, r200, r10k (the `aq_cabin` phases do not run `update-range`, so their `range_v_ok` is false by construction and unused). `pages_allocated` delta of the 1,000 unspilled updates: 0 on every arm.

**Cut for the one-hour budget.** No scan-row census (`bm_scan`): AQ-S5 names point reads and writes, and AQ's scan-path change (the span of sets) is exercised by the `plain-select` shapes only (a walked 1,000-row `sym = 'x'`). The Cabin shapes have 3-6 pairs per cell, not the 12-15 BT-close had, so a difference under about 1 us per point statement or about 10 us per every-row `UPDATE` is unresolved. The expression-Cabin cells (6, 7) were not run on the predicate shape or the cover shape (a two-sided range): only `DATE(ts)` with `= d`. The expression Cabin was measured with 10 of 50 days observed for the write cell and all 50 observed for the served cell. No multi-row `INSERT`, no write contention, no cross-core run. Single-row `INSERT`s only.

## 2. Headline: B - A against the A' - A spread

Server CPU, per statement, 6 clean paired runs (u1 + u2; c1 + c2 for the Cabin cells). "Inside" means the median |B - A| does not exceed the largest |A' - A| of the same cell, the rule BW-close used. The sign counts are in `archive/.../out/signs-*.txt`.

| # | cell | path | A (us) | B (us) | B - A | % of A | B above A | typical / largest A' - A | verdict |
|---|---|---|---|---|---|---|---|---|---|
| 1 | point `SELECT` by tuple key, no Cabin | literal | 31.65 | 32.20 | +0.97 | +3.1 % | 4 of 6 | 0.45 / 0.80 | **outside by 0.17**; sign mixed (4 of 6) |
| 1 | point `SELECT` by tuple key | prepared | 27.45 | 27.83 | +0.49 | +1.8 % | 4 of 6 | 0.29 / 0.56 | inside |
| 2 | single-row `INSERT`, no Cabin (`bn_census` `i`) | literal | 22.90 | 23.07 | +0.33 | +1.5 % | 4 of 6 | 0.28 / 1.14 | inside |
| 2 | same | prepared | 22.11 | 22.49 | +0.32 | +1.5 % | 4 of 6 | 0.33 / 1.24 | inside |
| 2' | single-row `INSERT`, no Cabin (`aq_cabin` `pi`) | literal | 23.62 | 23.09 | -0.31 | -1.3 % | 0 of 6 | 0.49 / 0.84 | inside |
| 2' | same | prepared | 22.93 | 22.65 | -0.30 | -1.3 % | 1 of 6 | 0.54 / 0.83 | inside |
| 3 | every-row `UPDATE`, 1,000 rows, no Cabin | literal | 743.94 | 757.70 | **+29.53** | +4.0 % | 6 of 6 | 10.06 / 29.56 | equals the largest gap (29.53 vs 29.56); 3x the typical; B above A and above A' in 6 of 6 |
| 3 | same | prepared | 788.75 | 820.44 | **+23.04** | +2.9 % | 5 of 6 | 10.37 / 18.50 | **outside** |
| 4 | single-row `INSERT` into a relation with a column Cabin (`ci`, all inserted symbols observed) | literal | 24.28 | 23.78 | -0.46 | -1.9 % | 0 of 6 | 0.45 / 1.05 | inside; **none** |
| 4 | same | prepared | 23.62 | 23.38 | -0.26 | -1.1 % | 1 of 6 | 0.60 / 1.03 | inside; **none** |
| 5 | `WHERE sym = 'x'` served from the column Cabin (`cs`, 5 rows returned) | literal | 31.42 | 31.68 | +0.26 | +0.8 % | 4 of 6 | 0.53 / 1.37 | inside; **none** |
| 5 | same | prepared | 30.84 | 31.13 | +0.35 | +1.1 % | 4 of 6 | 0.62 / 1.44 | inside; **none** |
| 5' | the same `WHERE sym = 'x'` walked (`ps`, no Cabin, 1,000 rows) | literal | 62.55 | 64.29 | +1.57 | +2.5 % | 5 of 6 | 0.94 / 1.68 | inside, barely; direction positive |
| 5' | same | prepared | 61.47 | 62.83 | +1.68 | +2.7 % | 6 of 6 | 0.67 / 1.97 | inside, barely; direction positive |

Cell 3 pooled with the loaded u3 phase (11 pairs; u3 has 12 unclean windows and one pair where all three arms ran 25 % slow): literal +28.1 us (+3.7 %), B above A in 10 of 11, above A' in 11 of 11; prepared +17.6 us (+2.3 %), 9 of 11 and 10 of 11; the largest A' - A rises to 42.9 / 67.8 us on the loaded runs, which is what turns both to "inside" by the largest-gap rule. Cell 1 pooled the same way: literal +0.93 us (+2.9 %, B above A in 8 of 11, largest A' gap 0.80: outside), prepared +0.30 us (+1.1 %, 7 of 11, inside). A' sits below A on the `UPDATE` (A' above A in 2 of 11 literal): the typical A' - A is -10 us, so B - A' is the larger gap (+34.1 / +26.0 us pooled).

Reading it. The Cabin cells the work order names (4, 5) are at zero within the spread, on both paths, and 4's sign is negative. The cells it does not name but that every relation pays (1, 3) carry a small positive shift. The `UPDATE` is 95 % server CPU (wall p50 about 735-770 us against server CPU 744-820 us, section 4), so +30 us of CPU is +30 us of wall: 1,257 statements/s at B against 1,277 at A literal, 1,162 against 1,191 prepared (derived from the mean, section 4). The point `SELECT` and `INSERT` shifts are under 1.5 us of a 44-64 us statement whose client side is half of it.

**Row sweep (rule 9).** The `UPDATE` shift is per row: it scales with the rows written. Single-row shapes do not scale with rows and are shown at the three sizes as evidence of it. Server CPU, us; B - A median of 3 paired runs (6 at 1,000 rows).

| shape | path | 200 rows: A / B / B - A | 1,000 rows: A / B / B - A | 10,000 rows: A / B / B - A |
|---|---|---|---|---|
| every-row `UPDATE` | literal | 171.3 / 174.9 / +3.9 (+2.3 %, 20 ns per row) | 743.9 / 757.7 / +29.5 (+4.0 %, 30 ns per row) | 7,210 / 7,476 / +271.7 (+3.8 %, 27 ns per row) |
| every-row `UPDATE` | prepared | 173.8 / 174.9 / +5.4 (+3.1 %, 27 ns per row) | 788.8 / 820.4 / +23.0 (+2.9 %, 23 ns per row) | 7,258 / 7,371 / +113.2 (+1.6 %, 11 ns per row) |
| point `SELECT` by tuple key | literal | 28.89 / 29.18 / +0.29 | 31.65 / 32.20 / +0.97 | 29.89 / 30.98 / +0.89 |
| point `SELECT` by tuple key | prepared | 27.78 / 27.28 / +0.19 | 27.45 / 27.83 / +0.49 | 27.07 / 28.27 / +1.13 (outside; 0 of 3 below A, largest A' gap 0.38) |
| single-row `INSERT`, no Cabin (`bn_census`) | literal | 24.50 / 23.88 / -0.03 | 22.90 / 23.07 / +0.33 | 23.39 / 23.95 / +0.61 |
| single-row `INSERT`, no Cabin | prepared | 22.68 / 22.35 / -0.16 | 22.11 / 22.49 / +0.32 | 22.37 / 22.84 / +0.63 |

The 200-row column was taken at load 2.5-5.6 with 12 unclean windows; its `UPDATE` rows are the least reliable here (A' - A typical 1.8-5.2 us, largest 2.5-5.5 us). The 10,000-row point `SELECT` prepared cell is the one point cell outside its spread at that size. A point `SELECT` by tuple key does not scale with rows by design and its three sizes sit within 28.9-32.2 us (literal) and 27.1-28.3 us (prepared) at either binary.

The Cabin shapes at three sizes (`aq_cabin`, c200 / c1+c2 / c10k). `cab-select` returns 1, 5 and 50 rows at 200, 1,000 and 10,000 rows (200 symbols in every case).

| cell | path | 200 rows: A / B / B - A | 1,000 rows: A / B / B - A | 10,000 rows: A / B / B - A |
|---|---|---|---|---|
| `INSERT` with column Cabin (4) | literal | 23.70 / 24.31 / +0.21 (outside a 0.17 gap) | 24.28 / 23.78 / -0.46 | 23.85 / 24.51 / +0.45 |
| `INSERT` with column Cabin | prepared | 23.83 / 23.95 / +0.13 | 23.62 / 23.38 / -0.26 | 22.98 / 23.93 / +0.96 |
| `sym = 'x'` served (5) | literal | 28.50 / 28.71 / +0.21 | 31.42 / 31.68 / +0.26 | 54.18 / 54.98 / -0.31 |
| `sym = 'x'` served | prepared | 27.64 / 28.08 / +0.21 | 30.84 / 31.13 / +0.35 | 53.55 / 55.06 / **+1.51** (3 of 3 above A; largest A' gap 1.41) |
| `sym = 'x'` walked (5') | literal | 34.27 / 34.65 / +0.22 | 62.55 / 64.29 / +1.57 | 365.62 / 373.40 / **+8.28** (outside; largest A' gap 5.08) |
| `sym = 'x'` walked | prepared | 33.54 / 34.42 / +0.43 | 61.47 / 62.83 / +1.68 | 366.87 / 374.82 / +3.99 (inside) |

The `INSERT` with a column Cabin at 200 rows (literal) and the served read at 10,000 rows (prepared) are the two cells where a Cabin cell falls outside its spread, by 0.04 us and 0.10 us; three runs each. They are reported and not explained: the other path of each pair (prepared at 200; literal at 10,000) reads inside, and a gap of 0.04-0.10 us over 3 runs is not resolved.

## 3. Against this engine's own previous numbers (BW close, `v3.2.0-22-g541ce440`)

The previous record for cells 1-3 is `results-bw-close-v3.2.0-22-g541ce440.md` (A = `74b0a3dd`, B = `541ce440`, `bn_census`, 1,000 rows, 6 runs, same host class, a different day and different binaries). It is shown for league; this run's A already contains BW's hoisted dropped-column gate (`e8727478` is an ancestor of `7c234766`).

| shape | BW close A (`v3.2.0-13`) | BW close B (`v3.2.0-22`) | this run A (`v3.2.0-44`) | this run B (`v3.2.0-48`) |
|---|---|---|---|---|
| point `SELECT` by tuple key CPU, literal / prepared (us) | 31.3 / 28.8 | 31.4 / 27.7 | 31.65 / 27.45 | 32.20 / 27.83 |
| single-row `INSERT` CPU, literal / prepared (us) | 22.4 / 21.9 | 22.8 / 22.4 | 22.90 / 22.11 | 23.07 / 22.49 |
| every-row `UPDATE`, 1,000 rows, literal / prepared (us) | 752.1 / 786.0 | 770.2 / 814.7 | 743.9 / 788.8 | 757.7 / 820.4 |

This run's A reads where BW's A and B read (the literal `UPDATE` at 744 against 752 and 770: the hoist took back 14-20 us, as `results-bw-update-fix-v3.2.0-24-ge8727478.md` found); **B reads where BW's B read**, 757.7 / 820.4 against 770.2 / 814.7 us, so across five commits and the AQ code the literal `UPDATE` ends within 13 us (1.6 %) of the BW-close level and the prepared one within 6 us (0.7 %). The BW close's +30 us and this close's +30 us are therefore not additive: A here is the engine after the fix, and B is back at the number the fix was written to remove. The point `SELECT` and `INSERT` sit within 1 us (3 %) of the BW record on every cell. The Cabin cells (4, 5, 6, 7) have no previous number: this run is their baseline.

## 4. Latency and where the time goes

Percentiles are the median over runs of each run's percentile, per path and arm, over the statements of that arm (the driver reports p90, not p95). `A_` is A'. Mean-derived statements/s is `1,000,000 / mean wall us` (derived, not driver-reported). "wall mean - server CPU" is what the client, the socket and scheduling account for: the measured statement is one client round trip.

**Waits.** With `durability = relaxed` and one connection, **no fsync wait and no lock or conflict wait exists in any measured statement** (the lock family's table is never contended, no undecided writer is met; stated, not measured). A statement's wall time is its server CPU (the parse or bind, the compile, the write or read statement itself) plus the remainder column: the client's own CPU (driver CPU column, 22-38 us) overlapping the server's, and the kernel's socket round trip on loopback. The server CPU share of mean wall: point `SELECT` 50 % literal / 49 % prepared; `INSERT` 49 % / 51 %; every-row `UPDATE` 95 % (its remainder is 38-40 us); `INSERT` with a column Cabin 49 %; served `sym = 'x'` 46 %, walked 63 %. The write-statement / read split inside the server CPU is not separable without the sampler, which was not run (section 6).

Cells 1-3, u1 + u2 pooled (6 runs; ops = total statements over runs):

{{an-u12.txt:1}}

Cells 4, 5, 5' (`aq_cabin`, 1,000 rows, c1 + c2 pooled):

{{an-c.txt:1}}

## 5. The feature's cost (B only): the expression Cabin

Phase x, B and B2 (B's bytes started twice, the in-run control), 4 runs each, 1,000 rows; the cells are paired inside one server and one run. `x-insert` inserts into `xi` (`CREATE CABIN ON xi (DATE(ts))`, 10 of 50 days observed, every inserted row lands in an observed day, so every insert appends to a set); `x-insert-plain` into `xp`, an identical relation with no Cabin. `x-select` is `SELECT * FROM xs WHERE DATE(ts) = d` with all 50 days observed (20 rows returned; `SHOW CABINS` read at the end of the first run showed 50 observed days, 1,000 entries and 4,046 hits on `xs`'s Cabin); `x-walk` is the same statement over `xw`, identical data and no Cabin.

{{raw:an-feature.txt}}

Read: the expression Cabin's append costs +0.73 us (literal) and +0.92 us (prepared) of server CPU per inserted row, +3.1 % and +4.0 %, 7 of 8 pairs above the no-Cabin relation; the column Cabin's append on the same server measures +0.59 / +0.72 us (8 of 8 / 7 of 8), and +0.61 / +0.69 on the B arm of c1 + c2 (6 of 6), so the expression adds about 0.5 us on top of a column Cabin's append (x-insert - cab-insert +0.46 us literal, 8 of 8; +0.77 us prepared, 8 of 8), consistent with `DATE(ts)` being evaluated once per written row (not isolated). Against the noise floor: the same B binary started twice (B2 - B) differs by a median of -0.29 to -0.84 us on the insert cells, with a largest gap of 0.7-1.6 us (4 of 4 runs on the B2-below-B side for `plain-insert`), so each of these +0.5 to +0.9 us deltas is at the size of the floor and is reported as a direction held in 7 and 8 of 8 pairs, not as a resolved number.

B and B2, server CPU per statement (us), the floor:

{{an-x.txt:0}}

B and B2 pooled, latency:

{{an-x.txt:1}}

## 6. Insight

**What Cabin cost does depend on is the relation of rows returned to rows scanned, and the data say where it turns.** From this run's own A cells at 200 / 1,000 / 10,000 rows: the walked `sym = 'x'` costs 34.3 / 62.5 / 365.6 us of server CPU, about 34 ns per row scanned beyond the 200-row cell ((365.6 - 34.3) / 9,800); the Cabin-served one costs 28.5 / 31.4 / 54.2 us, about 0.5 us per row it returns beyond the first ((54.2 - 28.5) / 49). So a row served from a Cabin costs about 15x a row walked past, and the served read beats the walk only while the matches are fewer than about one in fifteen of the relation (fitted from three sizes, not measured at other selectivities): at 200 rows with 1 match the two are 28.5 against 34.3 us, at 10,000 rows with 50 matches (0.5 %) 54.2 against 365.6 us (6.7x), and for `DATE(ts) = d` at 20 matches of 1,000 (2 %) 39.9 against 115.9 us (2.9x). That is a measured shape for `cabin.md`'s serving rule and for the cabin optimizer's candidate pick (which reads `access_columns` in this build); this run does not test whether the optimizer declines a Cabin above the break-even.

**The every-row `UPDATE`'s extra cost is per row and lands on a relation that has no Cabin.** The delta is 20 / 30 / 27 ns per row at 200 / 1,000 / 10,000 rows (literal); the range `7c234766..a5e33998` touches `command_dispatcher.cpp` (+222), `step_vm.cpp` (238 lines), `step_compiler.cpp` and the catalog, and adds a field to `TableAccess`. This reading covered only the hunks that name `hooked`, `NoteCabinWrite` and `expr_cabins` in `command_dispatcher.cpp`: they add an `expr_cabins.empty()` test to the statement-level `hooked` boolean and to `NoteCabinWrite`'s early return, which does not look like 20-30 ns of work per row on a relation with no Cabin. The rest of the range was not read for a per-row cost, so the shift is either in a part not read, or code placement and inlining, which an A/B of two builds cannot separate. It is the same size as the BW close's +30 us, which the BW hoist took back once. This was not profiled: `tools/kds_sampler.c` over the every-row `UPDATE` at A and at B is the measurement that would say, and it did not fit.

Where the data support no insight: whether the +0.97 us literal point `SELECT` shift (cell 1) is real. Sign 4 of 6 on the clean pairs, 8 of 11 pooled, 3 of 3 at 200 and 10,000 rows, and the literal path pays the compile ladder (three new arms after the column Cabin arm) on every execution while the prepared path pays it once, which fits +0.97 literal against +0.49 prepared; but the 10,000-row prepared cell is outside its spread too, which the compile-per-execution reading does not explain. It stays "borderline, not resolved".

AQ-S5's requirement, stated against what was measured: the column-Cabin write path and the Cabin-served read show none at 200 and 1,000 rows; at 10,000 rows the prepared served read reads +1.5 us (+2.8 %), 3 of 3 above A, 0.1 us outside the spread (3 runs, not resolved); the point read by tuple key shows a 0.5-1.0 us shift at 1,000 rows that is borderline; and the every-row `UPDATE` of a relation with no Cabin shows a 3-4 % shift that is outside the typical spread and at the largest one.

## 7. What was not measured

The suite (parent's). The predicate and cover Cabin shapes. A multi-row `INSERT`, `UPDATE` of a Cabin-indexed column, `DELETE`, a drop of an expression Cabin, a restart with expression Cabins mounted. The scan census. The every-row `UPDATE` profile. More than 6 clean pairs on the 1,000-row cells. A Cabin-served read at selectivities between 0.5 % and 100 %.
