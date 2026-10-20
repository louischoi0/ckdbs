# BO close: the always-on statement recording costs 0.35-0.52 us of server CPU per point statement (1.2-2.2 %), consistent in sign, inside the largest same-binary gap

**Thesis.** On `bo-relation-stats-checkpoints` (branch `worktree-bo-relation-stats-checkpoints`), BO's closing commit `86d1fe1a` (`v3.3.0-23-g86d1fe1a`, **B**) against the same commit with BO's statement-path recording compiled out (**A**, BO-Q14 (b); `RecordStatement` and `RelationStats::Record` return at once), with A's bytes started as a second server as the in-run control (**A'**), reads on `cores = 1`, `durability = relaxed`: **B is slower than A by 0.35 to 0.67 us of server CPU per single-row statement** (+1.1 % to +2.8 %), and B is above A in 29 to 37 of 42 paired runs in every point cell. **No cell leaves the A' - A spread by the benchmark's own rule** (median B - A against the largest |A' - A| of the cell: largest gaps 0.82 to 2.69 us on point statements), so by that rule BO's recording is not resolved from noise. Two further readings narrow it. The control itself reads A' above A by a median of +0.13 to +0.31 us (second-server position or placement; the arms rotate their start order and the sign survives, so it is a bias of this harness and not of the engine), and B against the mean of A and A' - the estimate that cancels it - is **+0.35 to +0.52 us (+1.2 % to +2.2 %)** with one-sided sign-test p between 0.0000 and 0.022 per cell (eight cells, not corrected, the cells are correlated). The every-row `UPDATE` of 1,000 rows reads +10.9 / +17.6 us (+1.4 % / +2.4 %) over 24 runs against largest A' - A gaps of 121 / 171 us: not resolvable. **BO's price is therefore about half a microsecond per statement, 1-2 % of a point statement's server CPU and 1 % of its client-seen latency, the size §3.1's "two clock reads and one latched table update" predicts, and it is paid by reads and writes alike** (the `select-pk` cell shows it too, so `RelationStats::Record`, which runs on writes only, is not the larger share). Nothing in this run approached a limit: it is a single-connection serial run, so what it prices is the recording's CPU, not its latch under contention (section 5).

Server CPU is the sum of `/proc/<pid>/task/*/schedstat` over every server thread; wall is the client's. Drivers and exact invocations: `archive/bo-close-v3.3.0-23-g86d1fe1a/scripts/run.sh`, `run2.sh`, `run3.sh` (the flags are in `tools/bn_census.py`'s docstring; `bench/README.md`: `bench/docs/` stays closed).

| Item | Value |
|---|---|
| Executed | 2026-10-20, UTC. Task start 01:52:25; builds 01:52:26-01:54:15 (both together); measurement 01:54:41-02:23:05 in invocations `w1`-`w7` (`archive/.../progress.txt` carries every phase stamp, including the discarded ones) |
| Worktree / branch | `bo-relation-stats-checkpoints`, branch `worktree-bo-relation-stats-checkpoints`; HEAD `86d1fe1a` (committed 2026-10-20 01:51:38 UTC). `git status` at the start: `instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md` modified, `instructions/v3.2.0/raft-marks-2026-10-20.md` untracked; **no source, no `tools/` file** differed from HEAD. No engine source was touched; nothing was built in `build/` or `build-release/` of the worktree |
| Commits measured | **B** = `86d1fe1a`, `v3.3.0-23-g86d1fe1a`, built from a clean `git archive`. **A** = the same export with `archive/.../scripts/bo_off.py` applied (sha256 `61f691b6ebe9a1f1d4df58ecb256f248b15a136c49f153b94fbdcf938aad92ee`); the diff it makes is `archive/.../out/patch.diff` and is two lines: in `src/server/command_dispatcher.cpp`, `CommandDispatcher::RecordStatement` gains `if (out.fetch_id != ~std::uint64_t{0}) return;` as its first statement; in `src/stats/relation_stats.cpp`, `RelationStats::Record`'s `if (rows == 0) return;` becomes `if (rows != ~std::uint64_t{0}) return;`. Left in A: `Dispatch`'s and `DispatchAsync`'s one `NowNs()` at the head (unread in A). **A'** = A's bytes started as a second server |
| Binary provenance | Built 01:54:15 UTC (A 01:54:15.21, B 01:54:15.49), after HEAD's commit time, from the exported tree. Measured copies in `/home/cdkbs/bench-runs/bo-close/bin/`, hashed after the copy (`archive/.../out/binaries.txt`): **A and A'** sha256 `cb67d758f9b130a49839cbb2705c2149522d6ad4ff6d0e1d45eedcef9460f0a6` (one file, copied twice); **B** `7b02297161fee82c8adb9bf26bac9df2bb9abbc3a3396fa1a13e864193f75b41`. Both build trees and both exports were deleted before the first cell; every server started from the copies |
| Build | `CMAKE_BUILD_TYPE=Release` (each `CMakeCache.txt`), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, `-j4` each, gcc 13.3.0; finished before any measurement |
| Host / device | 8 vCPU AMD EPYC 9V74, Linux 7.0.0-1014-azure. Data files and WAL under `/home/cdkbs/bench-runs/bo-close/wd-*/` on `/dev/root`, **ext4, not tmpfs** (`df -T`); volume 98 % full, 6.2 GB free at the start, lowest free space seen 3.9 GB (each invocation's data directory was 1.3-2.5 GB and was removed when the invocation ended). Per-cell host records (`loadavg`, `pgrep`, busy CPUs) are in each JSON's `arms[].host`; every cell of the seven kept invocations is `competing = []`, `clean = true`. Ports 34xxx |
| Pinning | One server process per arm, one busy at a time, on CPU 2; the driver on CPU 4 |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, other keys default; fresh server and data file per arm per run |
| Drivers | `tools/bn_census.py` sha256 `35693dc24f17...` and `tools/kwp.py` `4fc3597164f9...`, unmodified. Gates: `--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1 --quiet-load 2 --quiet-wait 30`. Zero unclean cells in the kept data |
| Suite | Not executed in this task; no code was changed by this measurement |

## 1. What was run, what was cut, what was discarded

Three arms (A, A', B) in every run, a fresh server and data file per arm per run, the arms interleaved block by block (6 blocks per cell, the first server of a block rotating by one per block and per run), the two driver paths (literal, prepared) both measured in every run.

| invocation | start order | runs | shapes | status |
|---|---|---|---|---|
| `w1` | B, A', A | 6 | `select-pk`, `insert`, `delete-pk`, `update-range` | kept |
| `w2` | A, A', B | 6 | same | kept |
| `w3` | A', B, A | 6 | same | kept |
| `w4` | B, A, A' | 6 | same | kept |
| `w5` | A, A', B | 6 | `select-pk`, `insert`, `delete-pk` | kept (re-run, see below) |
| `w6` | B, A, A' | 6 | same | kept |
| `w7` | A', B, A | 6 | same | kept (re-run, see below) |

`bn_census` row mapping: `--rows 1000` is the size of the tables `t` (point `SELECT`), `d` (the `DELETE` target, its 2,000+ rows inserted untimed before the arm), `r` (the every-row `UPDATE` writes all 1,000 rows) and `i` (the `INSERT` target, starting empty and growing by about 2,000 rows per arm per path over a run). Work per run per arm per path: 2,000 point statements (1,998 timed), or 120 every-row `UPDATE`s. Totals: 42 runs of every point cell (83,916 timed statements per arm per path), 24 runs of `update-range` (2,880 per arm per path).

**Discarded and re-run (two, both named in `archive/.../progress.txt`).**
- **The first `w5`** (started 02:08:08) overlapped a Debug `cmake --build build -j8` that the parent session ran in the worktree from about 02:08:00 to 02:10:30 (the host record of its run 0 lists the `cc1plus` processes, `clean = false`, a 60 s quiet wait and a run-queue wait of 129 %). The coordinator's instruction was to treat cells before 02:10:45 as invalid; the whole invocation was dropped, not only run 0, because its later runs' start times were not recorded per run. Its JSON and log are in `archive/.../discarded/`. It was re-run as `w5` at 02:18:03.
- **The first `w7`** (started 02:16:17) died in run 3 of 6 when `server B did not listen on 34082` (cause not diagnosed; the port was free afterwards). Its three completed runs were clean but the invocation was dropped as a unit and re-run as `w7` on other ports at 02:20:34; its files are in `discarded/`.
- A planned `w8` (a fourth point-only invocation) was not run: the discards used its time.

**What was cut or not run.**
- **The sweep over 200 / 1K / 10K rows (rule 9).** Every cell is 1,000 rows. The point statements do not scale with rows (a single-row key lookup, insert and delete-by-key), and the claim being priced - a fixed per-statement cost - is independent of it, but that is the argument and not a measurement here; the `update-range` cell (a per-row cost) was run at 1,000 rows only, so whether the recording scales with rows written is not shown. `RelationStats::Record` takes the statement's row count, not a per-row call, so none is expected.
- **`update-range` in `w5`-`w7`** (24 runs, not 42): it is the noisiest cell and the budget went to the point cells.
- **Multi-row `INSERT`, `PURGE`, DDL and refused statements**, which also pass through `RecordStatement` (a refusal is a call too).
- **Concurrency.** One connection, one core: the part's latch is taken uncontended, which is the case the design relies on (work order §3), and the measurement says nothing about two connections sharing one part.
- **`durability = strict` and `group`**, `cores > 1`, and the cost of reading the views (`sys.statement_stats`, `sys.relation_stats`), which are not on a statement's path.
- **The PostgreSQL floor**: no twin exists for a recording cost.
- **`end_state` in `w5`-`w7`**: the driver's end-state check includes `range_v_ok` for the every-row `UPDATE`, which those invocations did not run, so it reads false there by construction (their `bad replies` are 0; `w1`-`w4` read true).

## 2. Headline: B - A against the A' - A spread

Throughput is derived from the median server CPU per statement (`1e6 / CPU us`); the B - A cost is the paired per-run median in us of server CPU, with the range over runs in brackets. "Inside" means the median |B - A| does not exceed the largest |A' - A| of the same cell.

| cell | runs | A | A' | B | B - A paired median [min, max] | B above A | A' - A typical / largest abs gap | verdict |
|---|---|---|---|---|---|---|---|---|
| select-pk literal | 42 | 31,615 /s | 32,035 /s | 31,220 /s | +0.42 us (+1.3 %, B slower) [-2.38, +3.49] us | 32 of 42 | 0.58 / 2.15 us | inside |
| select-pk prepared | 42 | 32,157 /s | 32,961 /s | 32,210 /s | +0.35 us (+1.1 %, B slower) [-1.59, +3.40] us | 29 of 42 | 0.36 / 2.69 us | inside |
| insert literal | 42 | 41,828 /s | 41,585 /s | 40,958 /s | +0.67 us (+2.8 %, B slower) [-1.24, +2.24] us | 33 of 42 | 0.31 / 1.70 us | inside |
| insert prepared | 42 | 43,393 /s | 43,024 /s | 42,241 /s | +0.55 us (+2.4 %, B slower) [-0.71, +1.78] us | 33 of 42 | 0.23 / 0.82 us | inside |
| delete-pk literal | 42 | 42,519 /s | 42,037 /s | 41,460 /s | +0.50 us (+2.1 %, B slower) [-0.33, +1.85] us | 37 of 42 | 0.47 / 1.62 us | inside |
| delete-pk prepared | 42 | 43,604 /s | 42,934 /s | 42,610 /s | +0.47 us (+2.1 %, B slower) [-0.34, +1.73] us | 34 of 42 | 0.34 / 1.57 us | inside |
| update-range literal | 24 | 1,328 /s | 1,350 /s | 1,298 /s | +9.55 us (+1.3 %, B slower) [-70.88, +102.44] us | 16 of 24 | 23.19 / 120.92 us | inside |
| update-range prepared | 24 | 1,342 /s | 1,339 /s | 1,307 /s | +11.78 us (+1.6 %, B slower) [-48.11, +122.93] us | 18 of 24 | 11.04 / 170.73 us | inside |

**Reading.** No cell is outside by the stated rule, and no cell is a clean null either: every one of the six point cells carries the same sign, 29-37 of 42 runs with B above A, at a size (0.35-0.67 us) that is the typical A' - A gap or a small multiple of it (0.23-0.58 us). The honest statement is: **BO costs at most about 0.7 us and most likely about 0.4-0.5 us per point statement, and this host cannot place it more tightly than that**. The cells where it is relatively most visible are the cheapest write statements, `insert` and `delete-pk` (+2.1 % to +2.8 % of 23-24 us), as §6 predicted of the cheapest statement; `select-pk` is +1.1 % to +1.5 % of 31 us in absolute terms of the same order (0.35-0.47 us), which fits a cost dominated by the statement-level recording (two monotonic reads, one wall read, one latched table update in `StatementStats::Record`) rather than by the write-only `RelationStats::Record`.

The control matters here, so its sign is stated: A' (A's own bytes) is above A in 26-32 of 42 runs in every point cell, a median of +0.13 to +0.31 us. Taking B against A' alone (the pair that has the same server position bias as B) gives +0.18 to +0.35 us; taking B against the mean of A and A' gives +0.35 to +0.52 us; B against A gives +0.35 to +0.67 us. **The range of the estimates, 0.2 to 0.7 us, is the honest resolution of this run.**

Sign tests per cell (server CPU per statement, paired by run; the p-values are one-sided, unadjusted, eight correlated cells):

| cell | runs | B - A median | B above A | B - A' median | B above A' | A' - A median | A' above A | B - mean(A, A') median | B above mean | one-sided sign-test p, B above mean |
|---|---|---|---|---|---|---|---|---|---|---|
| select-pk literal | 42 | +0.42 us | 32 of 42 | +0.33 us | 27 of 42 | +0.27 us | 30 of 42 | +0.47 us (+1.5 %) | 28 of 42 | 0.0218 |
| select-pk prepared | 42 | +0.35 us | 29 of 42 | +0.23 us | 27 of 42 | +0.18 us | 30 of 42 | +0.39 us (+1.2 %) | 28 of 42 | 0.0218 |
| insert literal | 42 | +0.67 us | 33 of 42 | +0.35 us | 33 of 42 | +0.17 us | 27 of 42 | +0.52 us (+2.2 %) | 34 of 42 | 0.0000 |
| insert prepared | 42 | +0.55 us | 33 of 42 | +0.33 us | 30 of 42 | +0.13 us | 26 of 42 | +0.46 us (+2.0 %) | 31 of 42 | 0.0014 |
| delete-pk literal | 42 | +0.50 us | 37 of 42 | +0.22 us | 30 of 42 | +0.31 us | 32 of 42 | +0.40 us (+1.7 %) | 36 of 42 | 0.0000 |
| delete-pk prepared | 42 | +0.47 us | 34 of 42 | +0.18 us | 25 of 42 | +0.25 us | 30 of 42 | +0.35 us (+1.5 %) | 32 of 42 | 0.0005 |
| update-range literal | 24 | +9.55 us | 16 of 24 | +13.57 us | 20 of 24 | -2.66 us | 9 of 24 | +10.90 us (+1.4 %) | 17 of 24 | 0.0320 |
| update-range prepared | 24 | +11.78 us | 18 of 24 | +21.27 us | 21 of 24 | -7.47 us | 7 of 24 | +17.55 us (+2.4 %) | 19 of 24 | 0.0033 |

The `update-range` cell is the least resolvable: per-run server CPU of one arm moves by up to +-12 % between runs, the largest A' - A gaps are 121 us (literal) and 171 us (prepared), and B - A is +9.6 / +11.8 us. The recording is one statement's worth (one call, 1,000 rows counted in one update), so the expected cost is the same ~0.5 us, 0.07 % of this statement; the measurement cannot see it, and the +1.3 % / +1.6 % it reads is noise-sized.

By invocation (each start order; B - A and A' - A paired medians), so a reader can see the sign does not come from one order:

| file | start order | shape | path | runs | A | A' | B | B - A | A' - A |
|---|---|---|---|---|---|---|---|---|---|
| w1 | B, A', A | select-pk | literal | 6 | 31.35 | 31.71 | 31.78 | +0.43 [-0.14, +2.13] | +0.67 [+0.17, +1.15] |
| w1 | B, A', A | select-pk | prepared | 6 | 30.13 | 30.61 | 30.70 | +0.47 [-1.59, +1.45] | +0.33 [+0.16, +0.80] |
| w1 | B, A', A | insert | literal | 6 | 23.77 | 24.34 | 24.78 | +0.79 [+0.58, +2.24] | +0.52 [-0.16, +1.70] |
| w1 | B, A', A | insert | prepared | 6 | 22.98 | 23.06 | 23.70 | +0.64 [+0.36, +1.78] | +0.28 [-0.06, +0.64] |
| w1 | B, A', A | delete-pk | literal | 6 | 23.75 | 24.05 | 24.89 | +0.91 [+0.38, +1.85] | +0.52 [-0.66, +1.23] |
| w1 | B, A', A | delete-pk | prepared | 6 | 22.80 | 23.00 | 23.68 | +0.87 [+0.16, +1.73] | +0.18 [-0.56, +1.12] |
| w1 | B, A', A | update-range | literal | 6 | 745.20 | 741.46 | 754.30 | +1.57 [-40.30, +26.04] | -8.01 [-120.92, +54.55] |
| w1 | B, A', A | update-range | prepared | 6 | 748.69 | 733.55 | 761.89 | +15.56 [-39.24, +103.35] | -14.36 [-73.56, +1.19] |
| w2 | A, A', B | select-pk | literal | 6 | 31.63 | 31.58 | 32.10 | +0.57 [-2.38, +3.49] | -0.25 [-1.93, +2.03] |
| w2 | A, A', B | select-pk | prepared | 6 | 31.66 | 30.55 | 31.43 | +0.09 [-0.77, +1.15] | -0.17 [-2.69, +1.22] |
| w2 | A, A', B | insert | literal | 6 | 24.10 | 23.97 | 24.47 | +0.66 [-0.67, +1.70] | +0.14 [-0.48, +1.13] |
| w2 | A, A', B | insert | prepared | 6 | 23.09 | 23.32 | 23.56 | +0.47 [-0.18, +1.03] | +0.28 [-0.16, +0.77] |
| w2 | A, A', B | delete-pk | literal | 6 | 23.29 | 23.72 | 23.99 | +0.60 [-0.17, +1.21] | +0.43 [-0.98, +1.62] |
| w2 | A, A', B | delete-pk | prepared | 6 | 22.95 | 23.20 | 23.37 | +0.76 [-0.05, +1.63] | +0.37 [-0.46, +1.18] |
| w2 | A, A', B | update-range | literal | 6 | 760.59 | 737.45 | 754.07 | +5.36 [-49.17, +24.79] | -6.97 [-68.85, +15.91] |
| w2 | A, A', B | update-range | prepared | 6 | 761.50 | 765.62 | 769.01 | +4.94 [-23.42, +35.56] | -3.15 [-110.57, +58.19] |
| w3 | A', B, A | select-pk | literal | 6 | 31.23 | 30.48 | 31.61 | +0.48 [-0.19, +1.35] | -0.21 [-1.48, +0.59] |
| w3 | A', B, A | select-pk | prepared | 6 | 30.68 | 30.38 | 30.67 | +0.05 [-1.28, +1.00] | -0.17 [-1.33, +0.60] |
| w3 | A', B, A | insert | literal | 6 | 24.67 | 24.51 | 24.31 | -0.06 [-1.24, +1.30] | -0.00 [-1.16, +0.22] |
| w3 | A', B, A | insert | prepared | 6 | 23.44 | 23.24 | 23.36 | +0.20 [-0.71, +0.92] | -0.19 [-0.63, +0.22] |
| w3 | A', B, A | delete-pk | literal | 6 | 23.79 | 23.78 | 24.28 | +0.45 [-0.09, +1.80] | +0.01 [-0.37, +0.99] |
| w3 | A', B, A | delete-pk | prepared | 6 | 23.37 | 23.37 | 23.54 | +0.29 [-0.10, +1.14] | +0.06 [-0.46, +0.42] |
| w3 | A', B, A | update-range | literal | 6 | 756.44 | 740.30 | 780.53 | +12.41 [-48.89, +102.44] | -17.61 [-107.79, +99.40] |
| w3 | A', B, A | update-range | prepared | 6 | 736.32 | 733.60 | 755.22 | +20.11 [-48.11, +122.93] | -7.47 [-170.73, +6.61] |
| w4 | B, A, A' | select-pk | literal | 6 | 30.83 | 31.05 | 31.45 | +0.42 [-0.69, +1.24] | +0.22 [+0.17, +0.77] |
| w4 | B, A, A' | select-pk | prepared | 6 | 30.24 | 30.67 | 31.86 | +1.07 [-0.34, +3.40] | +0.38 [+0.07, +1.33] |
| w4 | B, A, A' | insert | literal | 6 | 23.73 | 23.92 | 24.42 | +0.51 [+0.15, +1.01] | +0.09 [-0.23, +0.63] |
| w4 | B, A, A' | insert | prepared | 6 | 23.06 | 23.32 | 23.69 | +0.44 [+0.04, +1.02] | +0.10 [-0.32, +0.68] |
| w4 | B, A, A' | delete-pk | literal | 6 | 23.72 | 23.95 | 24.08 | +0.50 [-0.11, +0.88] | +0.29 [+0.00, +0.67] |
| w4 | B, A, A' | delete-pk | prepared | 6 | 23.11 | 23.43 | 23.42 | +0.48 [-0.12, +1.07] | +0.34 [-0.14, +0.72] |
| w4 | B, A, A' | update-range | literal | 6 | 739.75 | 743.03 | 758.97 | +14.53 [-70.88, +75.66] | +6.27 [-90.50, +89.93] |
| w4 | B, A, A' | update-range | prepared | 6 | 732.43 | 731.93 | 747.06 | +11.42 [-41.63, +103.68] | -4.22 [-56.80, +87.08] |
| w5 | A, A', B | select-pk | literal | 6 | 31.68 | 31.73 | 31.75 | +0.26 [-1.76, +1.19] | +0.39 [-2.15, +1.03] |
| w5 | A, A', B | select-pk | prepared | 6 | 30.04 | 30.42 | 30.19 | +0.31 [-0.72, +1.25] | +0.43 [-0.10, +0.91] |
| w5 | A, A', B | insert | literal | 6 | 23.98 | 23.74 | 24.29 | +0.77 [-0.49, +1.26] | +0.08 [-0.76, +0.64] |
| w5 | A, A', B | insert | prepared | 6 | 22.99 | 23.00 | 23.54 | +0.75 [-0.12, +0.96] | -0.03 [-0.50, +0.82] |
| w5 | A, A', B | delete-pk | literal | 6 | 23.32 | 23.61 | 23.86 | +0.54 [-0.28, +1.40] | +0.24 [-0.22, +0.68] |
| w5 | A, A', B | delete-pk | prepared | 6 | 22.76 | 23.06 | 23.30 | +0.45 [-0.34, +1.33] | +0.09 [-0.10, +1.10] |
| w6 | B, A, A' | select-pk | literal | 6 | 31.15 | 30.79 | 32.05 | +0.87 [+0.14, +3.06] | +0.58 [-0.70, +2.03] |
| w6 | B, A, A' | select-pk | prepared | 6 | 29.91 | 30.45 | 30.92 | +0.87 [-0.64, +1.61] | +0.11 [-0.63, +1.62] |
| w6 | B, A, A' | insert | literal | 6 | 23.87 | 24.15 | 24.23 | +0.39 [-0.14, +0.82] | +0.18 [-0.10, +0.73] |
| w6 | B, A, A' | insert | prepared | 6 | 23.04 | 23.49 | 23.68 | +0.51 [-0.22, +0.72] | +0.13 [+0.04, +0.67] |
| w6 | B, A, A' | delete-pk | literal | 6 | 23.39 | 24.09 | 24.07 | +0.37 [+0.12, +1.15] | +0.62 [+0.03, +1.09] |
| w6 | B, A, A' | delete-pk | prepared | 6 | 23.17 | 23.71 | 23.60 | +0.41 [-0.10, +1.05] | +0.73 [-0.01, +1.25] |
| w7 | A', B, A | select-pk | literal | 6 | 30.88 | 31.64 | 31.56 | +0.28 [-0.95, +1.13] | +0.39 [-0.55, +1.88] |
| w7 | A', B, A | select-pk | prepared | 6 | 30.67 | 30.34 | 30.49 | +0.42 [-0.52, +0.93] | +0.12 [-0.45, +0.66] |
| w7 | A', B, A | insert | literal | 6 | 23.86 | 23.98 | 24.45 | +0.48 [-0.05, +1.00] | +0.18 [-0.64, +0.77] |
| w7 | A', B, A | insert | prepared | 6 | 23.14 | 23.26 | 23.76 | +0.46 [-0.38, +1.08] | +0.06 [-0.57, +0.67] |
| w7 | A', B, A | delete-pk | literal | 6 | 23.18 | 23.78 | 23.88 | +0.51 [-0.33, +1.19] | +0.38 [-0.59, +1.19] |
| w7 | A', B, A | delete-pk | prepared | 6 | 22.92 | 23.37 | 23.45 | +0.41 [-0.03, +1.36] | +0.16 [-0.45, +1.57] |

## 3. Waits: what a statement is made of here

The measured unit is a statement on a serial connection. Its client-seen latency is the sum below; the server and driver CPU overlap (the client handles one reply while the server writes the next frame), so the two do not add to the wall.

| wait type | in this run |
|---|---|
| durability / commit (fsync) | Absent from a statement by construction: `durability = relaxed`; the only fsync is the WAL segment roll's, which a 2,000-row-per-path insert stream does not reach |
| write statement (server CPU) | 22.9-24.4 us for `insert` / `delete-pk`; 30.3-32.0 us for `select-pk`; ~750 us for the 1,000-row `UPDATE` (table below) |
| client and socket round trip | the remainder of the wall: p50 41-45 us (write) and 57-61 us (read) against 23-32 us of server CPU, with the driver's own CPU 21-32 us |
| lock or conflict wait | none: one connection, one core, no concurrent writer |
| BO's recording | the delta of this document: 0.35-0.52 us of server CPU, 0.3 to 0.8 us of the p50 (below the percentile resolution) |

Statement wall latency, client side, per arm (median over runs of each run's percentile; 83,916 timed statements per point cell and arm, 2,880 per `update-range`):

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU | driver CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| select-pk | literal | A | 83916 | 55.2 us | 57.2 us | 60.3 us | 70.3 us | 90.6 us | 15,778 /s | 31.63 us | 32.19 us |
| select-pk | literal | A' | 83916 | 55.2 us | 57.4 us | 60.5 us | 70.9 us | 90.6 us | 15,720 /s | 31.22 us | 32.22 us |
| select-pk | literal | B | 83916 | 55.4 us | 57.7 us | 61.1 us | 71.7 us | 90.0 us | 15,625 /s | 32.03 us | 32.15 us |
| select-pk | prepared | A | 83916 | 51.9 us | 54.0 us | 57.3 us | 67.6 us | 87.2 us | 16,545 /s | 31.10 us | 29.52 us |
| select-pk | prepared | A' | 83916 | 52.0 us | 54.3 us | 57.5 us | 68.0 us | 87.0 us | 16,685 /s | 30.34 us | 29.55 us |
| select-pk | prepared | B | 83916 | 52.3 us | 54.5 us | 57.7 us | 68.7 us | 86.4 us | 16,447 /s | 31.05 us | 29.45 us |
| insert | literal | A | 83916 | 39.4 us | 44.4 us | 44.9 us | 54.1 us | 76.1 us | 20,773 /s | 23.91 us | 24.55 us |
| insert | literal | A' | 83916 | 39.5 us | 44.6 us | 45.1 us | 54.5 us | 76.0 us | 20,691 /s | 24.05 us | 24.55 us |
| insert | literal | B | 83916 | 39.3 us | 44.8 us | 45.4 us | 54.4 us | 76.3 us | 20,623 /s | 24.42 us | 24.58 us |
| insert | prepared | A | 83916 | 37.6 us | 41.8 us | 42.3 us | 50.3 us | 73.3 us | 22,029 /s | 23.05 us | 22.42 us |
| insert | prepared | A' | 83916 | 37.8 us | 41.9 us | 42.4 us | 50.4 us | 73.4 us | 22,039 /s | 23.24 us | 22.43 us |
| insert | prepared | B | 83916 | 37.6 us | 42.2 us | 42.8 us | 51.0 us | 73.9 us | 21,739 /s | 23.67 us | 22.42 us |
| delete-pk | literal | A | 83916 | 38.2 us | 43.7 us | 44.3 us | 52.2 us | 72.0 us | 21,234 /s | 23.52 us | 23.95 us |
| delete-pk | literal | A' | 83916 | 37.4 us | 44.0 us | 44.6 us | 53.0 us | 73.1 us | 20,973 /s | 23.79 us | 24.01 us |
| delete-pk | literal | B | 83916 | 38.2 us | 44.3 us | 44.9 us | 53.0 us | 73.0 us | 21,007 /s | 24.12 us | 23.98 us |
| delete-pk | prepared | A | 83916 | 34.5 us | 40.9 us | 41.4 us | 48.9 us | 68.7 us | 22,566 /s | 22.93 us | 21.48 us |
| delete-pk | prepared | A' | 83916 | 34.6 us | 41.1 us | 41.7 us | 49.8 us | 69.5 us | 22,468 /s | 23.29 us | 21.47 us |
| delete-pk | prepared | B | 83916 | 34.3 us | 41.4 us | 41.9 us | 50.1 us | 69.9 us | 22,329 /s | 23.47 us | 21.52 us |
| update-range | literal | A | 2880 | 682.2 us | 712.0 us | 728.9 us | 853.4 us | 1795.9 us | 1,226 /s | 752.83 us | 38.67 us |
| update-range | literal | A' | 2880 | 679.4 us | 708.2 us | 725.6 us | 884.7 us | 1597.8 us | 1,251 /s | 740.51 us | 38.88 us |
| update-range | literal | B | 2880 | 697.3 us | 725.2 us | 739.5 us | 864.5 us | 1913.5 us | 1,242 /s | 770.48 us | 38.44 us |
| update-range | prepared | A | 2880 | 678.7 us | 707.1 us | 724.0 us | 860.6 us | 1790.1 us | 1,268 /s | 745.04 us | 33.90 us |
| update-range | prepared | A' | 2880 | 675.9 us | 705.2 us | 720.3 us | 836.8 us | 1914.7 us | 1,267 /s | 747.07 us | 34.13 us |
| update-range | prepared | B | 2880 | 694.4 us | 725.4 us | 741.1 us | 893.9 us | 2005.3 us | 1,171 /s | 765.12 us | 35.08 us |

The percentiles are the driver's (p90 in place of p95); each is the median over runs of that run's percentile. **B's p0 to p99 sit on top of A's and A''s to within the 1-2 % the three arms differ by** - the recording is invisible in the distribution and visible only in the paired server-CPU count, which is why §2's table is the evidence and this one is context. Statements per second at the mean are the driver's derived rate on the client's wall.

## 4. Insight

- **BO's recording is a fixed per-statement cost of the order of half a microsecond, paid by reads as well as writes.** That is two clock reads, a wall read and one latched map update, as work order §3.1 and §6 describe, and BN's 0.7 % for access statistics on a point statement (cited from `CLAUDE.md`/§6, not re-measured here) is the same order. No switch exists (BO-Q8): the cost is 1-3 % of the cheapest statements in the engine and under 0.1 % of anything that touches a few hundred rows.
- **The cost is not per row.** The every-row `UPDATE` of 1,000 rows shows no resolved delta against a 120-170 us gap; a per-row cost of even 10 ns would have been 10 us there and is below what the cell resolves, so this run does not prove per-row independence either - it shows that the cost is not much larger than per-statement.
- **What the run cannot say:** the cost under contention (the part's latch with several connections on one core's part or many cores), the cost of a refused or long-running statement, and the cost at `strict`/`group`, where a statement is 82-85 % commit wait and half a microsecond is out of sight.
- **No stated design expectation is contradicted.** §6 expected the fixed cost to be most visible on the cheapest statement; the run is consistent with it (+2-3 % of an insert or delete, +1-1.5 % of a point read).

## 5. Limits of this document

- The decision rule (median against the largest A' - A gap) is the CD close's; it is conservative where the gap has outliers (one run of A' off by 1.7 us governs `insert literal`). The sign tests and the three estimates in section 2 are added so that the reader is not left with only "inside"; they are not a replacement for a quieter host.
- Prior numbers for the same shapes: no earlier BO-bearing run exists, so this is the baseline for BO. The nearest kdbs numbers for the same drivers are in `bench/v3.3.0/results-cd-close-v3.3.0-8-g842e0a42.md` (a different day, different invocations and a different engine, 15 commits earlier, with INSERT into a differently shaped relation): `select-pk` 29.8-30.2 k/s literal there against 31.2-31.6 k/s here, `update-range` 1,218-1,226 /s there against 1,298-1,328 /s here. They are context for "same league", not a controlled delta;
- Nothing approached a limit (rule 4b): this is a CPU-cost measurement on an idle core; the limits of `statement_stats` (4096 shapes per core, drops counted) were not exercised, and the driver's statements are single-row or one fixed range.
- The tests were not run; the A binary is a measurement-only patch and was never run against the suite.
