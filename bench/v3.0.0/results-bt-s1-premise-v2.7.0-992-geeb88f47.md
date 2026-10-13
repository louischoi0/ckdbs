# BT-S1: a rejected `bn` row pays 7.1-8.1 ns it does not need in `AcceptTupleAt`; the gate passes, and 24 ns of the row stays out of BT's reach

**Thesis.** On `bt-accept-tuple-row`, with the engine at `eeb88f47` (`v2.7.0-992-geeb88f47`; the worktree's HEAD `85a10ce6` differs from it in `instructions/` only), a `bn` row (`SELECT * FROM s WHERE w = ?`, 10,000 rows, `cores = 1`, `relaxed`, every page resident) costs **32.0 ns of server CPU on the literal path and 32.4 ns on the prepared path** (net row: (scan CPU - point `SELECT` CPU) / rows, median of 5 runs). Taking the work a kernel-rejected row does not need out of `AcceptTupleAt`, measured by ablation (D-all), removes **7.1 ns (22 %) literal and 8.1 ns (25 %) prepared**, in 5 of 5 runs on both paths, against a same-bytes gap (A') of 0.2 ns (median). The floor, D4, where the walk rejects the row itself, is 24.0 and 23.7 ns: **three quarters of the row is the walk, the visibility decision, the kernel and the slot loop, which BT does not touch.**

- **BT-Q1 (a) as revised passes** for the tuple read (D2: 3.1 / 3.3 ns literal / prepared), the row invariants (D1: 2.6 / 2.5), the rejected path's counters and charge (D3: 3.2 / 3.2) and the three together (D-all: 7.1 / 8.1 against the 3.0 ns bar). Each is above 1.0 ns and above the median A-against-A' gap (0.2 ns). The gate licenses BT-S2 and BT-S3.
- **D4 - D-all is 0.7-1.0 ns: at the 1.0 ns bar, not above it, and forbidden by BT-R1 in any case. Not built.**
- **What the gate does not price.** D2 is nearly the real BT-S2 change. D1 mixes three parts, one of them (#2, a cached kernel) wrong across steps. D3 counts nothing, so the real C3 (counts and charge kept, no `Status` on success) can recover only part of its 3.2 ns. The ablations overlap (D1 + D2 + D3 sum to 8.8 / 9.0 ns against D-all's 7.1 / 8.1), so 7.1-8.1 ns is a ceiling for S2 + S3, not a forecast.
- **Against history.** `bn` literal read 30.5 ns at `952c67e5` (BM-S6's remedy) and 31.0 at `6b771ec1` (BM's close); A reads 1.5 and 1.0 ns higher in another session, which is not a finding (section 9). PostgreSQL 18.6's 29.1 ns is the floor: A is 1.10 times it, D-all 0.85.

The run was held to the 20-minute stage budget (builds and census together 11 min 9 s; section 11 lists what that cut). Section 2 carries the net row, 3 the gate, 4 the ablations, 5 the identity check, 6 the latency distributions, 7 the waits, 8 the noise floor and host, 9 the comparison, 10 what the run teaches. The driver is documented in its own docstring (`bench/README.md`: `bench/docs/` stays closed); the exact invocation is `archive/bt-s1-premise-v2.7.0-992-geeb88f47/scripts/run.sh`.

| Item | Value |
|---|---|
| Executed | 2026-10-13, UTC. Builds 07:30:43-07:36:23 (A, D1, D2, D3, D-all by `buildall.sh`, 07:30:43-07:34:56; D4 built by hand afterwards, finished 07:36:23, so it is not in `build.out`). Census 07:36:35-07:41:52; run 0's cells began 07:38:45, after a 130 s quiet gate |
| Worktree / branch | `bt-accept-tuple-row`, branch `worktree-bt-accept-tuple-row`, HEAD `85a10ce6` (BT-S0, committed 07:37:04, 29 s after the census started; it adds `instructions/v3.0.0/` files only, `git diff --stat eeb88f47 85a10ce6`) |
| Commit measured | **A** = `eeb88f47ea158c816f31ae38fd17a459ed6b7faa`, `v2.7.0-992-geeb88f47`, committed 2026-10-13 07:24:18 UTC. The other six arms are A plus measurement-only patches (`archive/.../scripts/variant.py`, never committed) |
| Tree | Every binary was built from `/home/cdkbs/bench-runs/bt-s1/src`, a `git archive eeb88f47` export: `diff -rq` against a fresh export of `eeb88f47` is empty (checked after the run, with `src/` restored to A), and the two patched files' pristine copies equal the commit's blobs. **The worktree was dirty** (`include/kds/exec/chain_frame.hpp`, `step_vm.hpp`, `src/exec/chain_frame.cpp`, `step_vm.cpp`, `src/server/command_dispatcher.cpp`, edited 07:37-07:38, after the census began): **none of it is in a measured binary or number**. The drivers were read from the worktree's `tools/`, unmodified |
| Binary provenance | Copies in `/home/cdkbs/bench-runs/bt-s1/bin/`, each `cp`'d from the build directory right after its build, hashed, every server started from the copy. sha256 prefixes: **A and A'** `833501d0` (the same bytes; built 07:31:52, 7 min 34 s after the commit, so not older than it), D1 `4f3426ce` (07:32:38), D2 `80252cf8` (07:33:29), D3 `408aabd7` (07:34:20), D-all `97717fcb` (07:34:56), D4 `a3d81a5b` (copy 07:36:23). Full hashes and mtimes: `archive/.../binaries.txt`. The source binary's mtime survives only for D4 (`rel/kds_server`, 07:36:23.00): the next variant's build overwrote the others. A was not cross-checked against a second export's bytes |
| Build | `CMAKE_BUILD_TYPE=Release` (read from `CMakeCache.txt`), `-O3 -DNDEBUG`, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, `-j8`, gcc 13.3.0; one build directory, rebuilt incrementally per variant |
| Host / device | 8 vCPU AMD EPYC 9V74 (4 cores x 2 threads; siblings 2-3, 4-5), Linux 7.0.0-1014-azure, 16 GB. **Data files and WAL under `/home/cdkbs/bench-runs/bt-s1/bn10000/` on `/dev/root`, ext4, not tmpfs** (`df -T`); the binaries' copies are on the same device |
| Pinning | All seven servers (one process per arm, one busy at a time) on CPU 2, the driver on CPU 4; the SMT siblings 3 and 5 and CPUs 0, 1, 6, 7 were left to whatever else ran |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, Waystone at its defaults, `scan_kernel` not written (default, on); a fresh server and data file per arm per run (`bk-<arm>-r<run>.db`); port 32711 (not the default 15432). Every server's log is empty and its stderr is the four start-up and stop lines |
| Host load | 1-minute load 12.19 at the first host-watch sample (07:36:35, the tail of this run's own builds), 1.98 when run 0's gate released, 0.60-1.36 at the 140 cells' starts (section 8). `pgrep -a -f "cc1plus\|cmake --build\|ctest"` found nothing before any cell |
| Drivers | `tools/bm_scan.py` sha256 `1896ef20...` and `tools/bn_census.py` `d7b80765...`, both BM's close's, unchanged. Tables: `archive/.../scripts/table.py` (the run's), `bt_s1_tables.py` (gate, percentiles, waits) and `bt_s1_fragments.py` (this file's tables) |
| Suite | **Not run for these binaries** (measurement-only patches, `KDS_BUILD_TESTS=OFF`). The base suite at `eeb88f47` ran 3691/3691 (Debug, `-LE heap-suspended`) in the BT session before any BT change (work order §6), not in this run |

## 1. What was run

`tools/bm_scan.py census --cells bn`, seven arms, 5 runs (`run.sh` has the flags). Each run starts a fresh server per arm, loads the same relations into each, and measures the cell on all seven block by block: **12 blocks, the order of the servers rotating per block and per run, the two driver paths alternating inside a block**, so a server's position and host drift fall on every arm alike. **Row counts:** `--cells bn` is relation `s`, 10,000 rows `(id, v, w)`; `--scan-ops 192` is 192 scans per path per arm per run (16 per block), `--point-ops 3000` is 3,000 point `SELECT`s (250 per block). Equal work: 13,440 scans and 210,000 point statements over the run, the same count per arm. The measure is the server's on-CPU time (the sum of `/proc/<pid>/task/*/schedstat`), not wall time.

## 2. The net row: 32.0-32.4 ns, and every ablation is below it in the paired median

A' is level with A (+0.1 and -0.2 ns); each ablation is faster than A in the paired median. Net row, ns, median of 5 [min, max]; the delta is paired by run.

**Literal path** (an unnamed `C_PARSE` of the text per statement)

| arm | net row, median [min, max] | rows/s (derived, 1e9 / median ns) | arm - A, paired by run, median [min, max] | runs below A |
|---|---|---|---|---|
| A | 32.0 ns [31.3, 33.1] | 31,210,694 /s | (reference) | - |
| A' | 31.9 ns [31.5, 33.2] | 31,356,560 /s | +0.1 ns [-0.2, +0.3] | 1 of 5 |
| D1 | 29.7 ns [28.7, 31.6] | 33,703,699 /s | -2.6 ns [-3.2, +0.3] | 4 of 5 |
| D2 | 28.5 ns [28.2, 30.5] | 35,027,830 /s | -3.1 ns [-3.6, -2.6] | 5 of 5 |
| D3 | 28.9 ns [28.2, 29.9] | 34,547,453 /s | -3.2 ns [-3.3, -2.0] | 5 of 5 |
| D-all | 24.7 ns [24.5, 26.0] | 40,534,122 /s | -7.1 ns [-7.5, -6.2] | 5 of 5 |
| D4 | 24.0 ns [23.5, 25.2] | 41,700,919 /s | -7.9 ns [-8.6, -7.4] | 5 of 5 |

**Prepared path** (one named `C_PARSE`, then `BIND`/`EXECUTE`)

| arm | net row, median [min, max] | rows/s (derived, 1e9 / median ns) | arm - A, paired by run, median [min, max] | runs below A |
|---|---|---|---|---|
| A | 32.4 ns [31.5, 34.5] | 30,848,154 /s | (reference) | - |
| A' | 32.1 ns [31.3, 32.7] | 31,145,934 /s | -0.2 ns [-2.4, +0.1] | 4 of 5 |
| D1 | 30.5 ns [28.7, 32.1] | 32,801,639 /s | -2.5 ns [-3.5, -0.7] | 5 of 5 |
| D2 | 29.2 ns [28.2, 30.3] | 34,238,709 /s | -3.3 ns [-4.2, -2.8] | 5 of 5 |
| D3 | 29.1 ns [28.9, 30.1] | 34,345,950 /s | -3.2 ns [-4.7, -2.3] | 5 of 5 |
| D-all | 24.5 ns [24.3, 25.9] | 40,739,377 /s | -8.1 ns [-8.6, -6.7] | 5 of 5 |
| D4 | 23.7 ns [23.6, 24.8] | 42,214,090 /s | -8.8 ns [-9.7, -7.7] | 5 of 5 |

The 5 runs, net row per arm. Run 0 is about 1 ns above the others in every arm, a shift the paired reading removes.

**Literal path, per run**

| run | A | A' | D1 | D2 | D3 | D-all | D4 |
|---|---|---|---|---|---|---|---|
| 0 | 33.1 ns | 33.2 ns | 29.9 ns | 30.5 ns | 29.9 ns | 26.0 ns | 25.2 ns |
| 1 | 31.4 ns | 31.5 ns | 28.7 ns | 28.3 ns | 28.2 ns | 25.2 ns | 24.0 ns |
| 2 | 31.3 ns | 31.6 ns | 31.6 ns | 28.2 ns | 29.3 ns | 24.5 ns | 23.5 ns |
| 3 | 32.1 ns | 31.9 ns | 29.7 ns | 28.5 ns | 28.9 ns | 24.7 ns | 23.5 ns |
| 4 | 32.0 ns | 32.4 ns | 29.5 ns | 28.8 ns | 28.8 ns | 24.6 ns | 24.1 ns |

**Prepared path, per run**

| run | A | A' | D1 | D2 | D3 | D-all | D4 |
|---|---|---|---|---|---|---|---|
| 0 | 34.5 ns | 32.1 ns | 31.0 ns | 30.3 ns | 29.8 ns | 25.9 ns | 24.8 ns |
| 1 | 31.5 ns | 31.3 ns | 28.7 ns | 28.2 ns | 29.1 ns | 24.8 ns | 23.6 ns |
| 2 | 32.9 ns | 32.7 ns | 32.1 ns | 28.7 ns | 29.1 ns | 24.5 ns | 23.7 ns |
| 3 | 32.4 ns | 31.4 ns | 30.5 ns | 29.4 ns | 30.1 ns | 24.3 ns | 23.6 ns |
| 4 | 32.0 ns | 32.1 ns | 29.6 ns | 29.2 ns | 28.9 ns | 24.5 ns | 24.3 ns |

## 3. The gate (BT-Q1 (a), as revised) passes for D1, D2, D3 and D-all

The gate: build BT-S2/S3 only if D-all (or D4) saves at least 3.0 ns a row, and each stage only if its own ablation saves at least 1.0 ns **and** more than the median A-against-A' gap. That gap is 0.21 ns literal and 0.19 ns prepared (largest 0.34 and 2.37 ns, the latter in run 0, where A read 34.5 ns). Savings are medians of per-run differences against A.

| candidate | ablation | saved, literal | saved, prepared | runs saving 1.0 ns or more (literal, prepared) | reading |
|---|---|---|---|---|---|
| BT-S3 (C1, the row invariants) | D1 | 2.6 ns | 2.5 ns | 4 of 5, 4 of 5 | passes; mixes #1, #4 and an unsound #2 |
| BT-S2 (C2, the tuple read) | D2 | 3.1 ns | 3.3 ns | 5 of 5, 5 of 5 | passes |
| BT-S3 (C3, counters and charge) | D3 | 3.2 ns | 3.2 ns | 5 of 5, 5 of 5 | passes; the ablation counts nothing |
| the whole order | D-all | 7.1 ns | 8.1 ns | 5 of 5, 5 of 5 | **passes** the 3.0 ns bar (smallest run 6.2 and 6.7 ns) |
| the floor | D4 | 7.9 ns | 8.8 ns | 5 of 5, 5 of 5 | not a candidate: wrong by BT-R1 |
| what D4 adds to D-all | D4 - D-all | 0.8 ns (1.0 paired directly) | 0.7 ns (0.8 paired directly) | per run 0.5-1.2 ns, 0.3-1.3 ns | at the bar, not above it; forbidden; **not built** |

D1 misses once on the literal path (run 2: 31.6 ns against A's 31.3), which is why its range reaches +0.3 ns; no cause was found. **The three single ablations sum to 8.8 ns literal and 9.0 prepared against D-all's 7.1 and 8.1**: they overlap (the mechanism was not isolated), so stage savings do not add and S2 + S3 cannot be forecast from this table beyond the D-all ceiling.

## 4. What each arm changes, and what its number can be read as

| arm | patch over `eeb88f47` (`step_vm.cpp`, `heap_page.hpp`) | wrong as written? | survey items (work order §1.1) |
|---|---|---|---|
| A | none | no | - |
| A' | A's bytes started as a second server | no | the spread |
| D1 | `spills_.clear()` moved after the rejected return; `RelayoutEpoch()` read only on the kept paths; `ArmedKernel()` cached in a member once armed | yes: #2 is wrong across steps (a later step's `CheckStepShapeOnce` can move `statement_->kernels`; `bn` is one step) | #1, #4, #2 |
| D2 | `PageView::TryReadTupleIn`, an inline header read with `ReadTupleIn`'s checks in the same order and no `StatusOr` | only on a failure, which loses its worded reason | #5 |
| D3 | the kernel-rejected path skips its two counters and `ChargeRow()` | yes: counts nothing and charges nothing (`ANALYZE` `examined=` is wrong, section 5) | #7, #8 |
| D-all | D1 + D2 + D3 | as D1 and D3 | #1, #2, #4, #5, #7, #8 |
| D4 | D-all, and the walk's `visit_slot` reads, classifies and kernel-rejects a row itself before calling `AcceptTupleAt` (uses D1's cached kernel) | yes: a second `Classify` beside `AcceptTupleAt` (BT-R1) | D-all plus #9, with #3 and the call's prologue |

D4 - D-all is therefore an upper bound on #9 (the `Status` return alone): it also contains #3 (`PageSpanGuard`) and the call itself, and this design cannot separate them.

## 5. The same answer: rows and digest identical in all seven arms; D3's counts differ by design

`bm_scan` records each arm's `ANALYZE` of one statement and a digest of its rows in run 0 only. **`bad` (a statement that did not answer `SELECT` with the expected 1 row) is 0 in all 140 cells**; the digest is `1:6f3e98fc04fa4510` in all seven arms.

| arm | `examined=` | `matched=` | `sel=` | `pages=` | row digest |
|---|---|---|---|---|---|
| A, A', D1, D2 | 10,000 rows | 1 row | 0 % | 61 pages | `1:6f3e98fc04fa4510` |
| D3, D-all, D4 | 2 rows | 1 row | 50 % | 61 pages | `1:6f3e98fc04fa4510` |

`examined=2` is D3's skipped counter, as designed: only the rows that took the full path were counted (probably the step's first row, which arms the kernel under BM-R6, and the match; not checked). It is the one place this run's answer differs, and it is the reason D3, D-all and D4 are prices and not candidates. The contract suites (waystone, index, cabin, inner-build) were not run on any of them.

## 6. Latency distributions: the body of the scan statement falls with the row cost; the tail does not order by arm

Wall time per scan statement as the driver sees it, pooled over the 5 runs (192 per run), linear-interpolated percentiles; scans/s at p50 is derived (1e6 / p50).

| path | arm | n | p0 | p25 | p50 | p90 | p95 | p99 | scans/s at p50 (derived) |
|---|---|---|---|---|---|---|---|---|---|
| literal | A | 960 | 346.7 µs | 375.6 µs | 388.6 µs | 428.0 µs | 463.6 µs | 562.0 µs | 2,573 /s |
| literal | A' | 960 | 350.4 µs | 376.1 µs | 390.7 µs | 438.8 µs | 484.8 µs | 664.8 µs | 2,559 /s |
| literal | D1 | 960 | 327.2 µs | 348.7 µs | 362.3 µs | 416.0 µs | 486.6 µs | 985.8 µs | 2,760 /s |
| literal | D2 | 960 | 317.1 µs | 344.2 µs | 356.2 µs | 397.7 µs | 439.0 µs | 517.6 µs | 2,807 /s |
| literal | D3 | 960 | 314.2 µs | 339.3 µs | 353.7 µs | 414.7 µs | 494.8 µs | 584.1 µs | 2,827 /s |
| literal | D-all | 960 | 279.1 µs | 298.8 µs | 312.9 µs | 380.9 µs | 435.4 µs | 896.6 µs | 3,195 /s |
| literal | D4 | 960 | 270.2 µs | 290.7 µs | 303.8 µs | 366.9 µs | 415.9 µs | 536.4 µs | 3,291 /s |
| prepared | A | 960 | 345.8 µs | 372.5 µs | 387.6 µs | 449.1 µs | 527.1 µs | 620.6 µs | 2,580 /s |
| prepared | A' | 960 | 346.5 µs | 370.6 µs | 383.2 µs | 420.5 µs | 468.1 µs | 567.2 µs | 2,609 /s |
| prepared | D1 | 960 | 320.1 µs | 347.0 µs | 359.6 µs | 433.5 µs | 536.0 µs | 664.8 µs | 2,780 /s |
| prepared | D2 | 960 | 318.2 µs | 339.2 µs | 354.6 µs | 406.6 µs | 481.7 µs | 862.1 µs | 2,820 /s |
| prepared | D3 | 960 | 313.0 µs | 335.6 µs | 351.1 µs | 457.0 µs | 497.4 µs | 627.0 µs | 2,848 /s |
| prepared | D-all | 960 | 275.9 µs | 294.3 µs | 308.2 µs | 363.4 µs | 427.7 µs | 878.5 µs | 3,244 /s |
| prepared | D4 | 960 | 268.2 µs | 286.7 µs | 299.8 µs | 352.7 µs | 403.4 µs | 507.1 µs | 3,335 /s |

p0, p25 and p50 fall in the order of the ablations (A 346.7 / 375.6 / 388.6 µs to D4 270.2 / 290.7 / 303.8 µs, literal), a shift of 76-85 µs, which brackets the 79 µs that 7.9 ns x 10,000 rows predicts. **The p99 of 960 samples is about the tenth-worst: it is not ordered by arm** (D1 literal 985.8 µs, D-all 896.6 µs, D4 536.4 µs) and carries host hiccups, not code. The driver's own per-run percentiles (median over runs) are in `bmscan-bn.json.gz` and give the same picture.

## 7. Waits: the scan statement is 73-81 % per-row server work; the rest is the fixed statement cost and the client round trip

A latency is a sum. Medians over the 5 runs; the per-row term is 10,000 x the net row; the fixed term is the point `SELECT`'s server CPU on the same relation, arm, path and run; the last column is the wall mean minus the server CPU, so it carries the distribution's tail (it reads 43-62 µs, about 10 µs of scatter). The terms sum to the wall mean within the medians' rounding.

| path | arm | wall mean | per-row work (10,000 x net row) | fixed statement (point `SELECT` CPU) | client + socket (wall - server CPU) | per-row share of wall |
|---|---|---|---|---|---|---|
| literal | A | 399.0 µs | 320.4 µs | 29.2 µs | 49.5 µs | 80 % |
| literal | A' | 399.8 µs | 318.9 µs | 29.3 µs | 50.7 µs | 80 % |
| literal | D1 | 385.4 µs | 296.7 µs | 28.9 µs | 59.7 µs | 77 % |
| literal | D2 | 361.4 µs | 285.5 µs | 29.3 µs | 46.7 µs | 79 % |
| literal | D3 | 367.9 µs | 289.5 µs | 28.9 µs | 49.5 µs | 79 % |
| literal | D-all | 325.2 µs | 246.7 µs | 29.9 µs | 48.0 µs | 76 % |
| literal | D4 | 312.2 µs | 239.8 µs | 29.7 µs | 42.9 µs | 77 % |
| prepared | A | 398.9 µs | 324.2 µs | 27.8 µs | 46.8 µs | 81 % |
| prepared | A' | 397.0 µs | 321.1 µs | 28.2 µs | 47.6 µs | 81 % |
| prepared | D1 | 375.5 µs | 304.9 µs | 28.0 µs | 43.3 µs | 81 % |
| prepared | D2 | 379.4 µs | 292.1 µs | 28.1 µs | 59.3 µs | 77 % |
| prepared | D3 | 373.0 µs | 291.2 µs | 27.9 µs | 54.1 µs | 78 % |
| prepared | D-all | 335.8 µs | 245.5 µs | 28.2 µs | 61.9 µs | 73 % |
| prepared | D4 | 313.9 µs | 236.9 µs | 28.3 µs | 49.2 µs | 75 % |

| wait type | this engine at this configuration |
|---|---|
| durability / commit (fsync) | not applicable: a `SELECT` under `relaxed` commits nothing; `group` and `strict` were not measured |
| write statement | not applicable |
| read | the per-row term above. `pool_misses` = 0 in all 70 scan cells (every page resident); `lookups` = 0 and `lookups_instance` = 0 in all 140 cells: no row took a commit-window lookup, so every `Classify` in every arm ran on the floor path (BM-S1's counter is zero there) |
| client and socket round trip | 43-62 µs of the wall mean, 11-18 % of it (the driver's own CPU is 41-47 µs of it); the driver and server overlap, so it is not separable further here |
| lock or conflict wait | none: `window_wait_us` = 0 in all 140 cells (one connection, no writer) |

The fixed term is level across arms (28.9-29.9 µs literal, 27.8-28.3 prepared): the ablations do not move the per-statement cost, so subtracting the point `SELECT` removes it and leaves the per-row term clean (a 1 µs shift in it would move a net row by 0.1 ns). The point cells' `pool_misses` (first touch of the point path) are 1,125-1,140 literal and 650-665 prepared summed over 5 runs, the same in every arm.

## 8. The noise floor and the host

**A' against A, the control that cannot affect the result: 0.1 ns literal, -0.2 ns prepared (median paired), typical 0.2 ns, largest 0.34 ns literal (run 2) and 2.37 ns prepared (run 0).** Every saving in section 3 is at least 12 times the typical gap; the one place a single gap approaches a saving is run 0 on the prepared path (A' 2.4 ns under A), the same size as D1's median saving (2.5 ns). The A-against-A' gap on this shape in BM's close (3.2 ns, `results-bm-close-v2.7.0-946-g6b771ec1.md` section 8, as the work order cites it) did not recur here; this run cannot say why it was 3.2 ns there.

| run | gate wait (load at release) | cells | load at cells | sibling busy, max | foreign server / driver CPU, max | competing build or test |
|---|---|---|---|---|---|---|
| 0 | 130 s (1.98) | 28, all clean, 1 attempt | 1.25-1.36 | 9.8 % | 2.7 % / 2.2 % | none |
| 1 | 0 s (1.25) | 28, all clean, 1 attempt | 0.94 | 6.0 % | 2.7 % / 2.0 % | none |
| 2 | 0 s (0.95) | 28, all clean, 1 attempt | 1.25-1.27 | 6.5 % | 1.2 % / 4.0 % | none |
| 3 | 0 s (1.25) | 28, all clean, 1 attempt | 0.80-0.81 | 4.7 % | 2.9 % / 2.8 % | none |
| 4 | 0 s (0.81) | 28, all clean, 1 attempt | 0.60-0.66 | 5.6 % | 2.5 % / 1.8 % | none |

Run 0 waited 130 s for the load average to fall from this run's own builds. Host-watch (`hostwatch.log`, every 10 s, 32 samples) falls from 12.19 to 0.66, with small rises (0.94 to 1.30) mid-run. The per-cell record shows no other session on the server's or driver's CPU and no competing build or test: 140 of 140 cells clean on the first attempt, the longest per-cell quiet wait 12 s (run 2).

## 9. Against the engine's last number, and the floor

The baseline is this engine's own last run; the 1.0-1.9 ns by which A sits above it is **not a finding**: A and B/B2 were not in one run, `src/exec/step_vm.cpp`, `heap_page.{hpp,cpp}`, `visibility.hpp` and `budget.hpp` are unchanged between `952c67e5` and `eeb88f47` (`git diff --stat`; `step_vm.hpp` by 3 lines, `row_codec`, `step_compiler`, `set_expr` and `txn/manager` did change), and this cell moved 4 ns for the same binary between BM's close and BM-S6's session (A: 70.0 to 66.0 ns, `results-bm-s6-remedy-v2.7.0-948-g952c67e5.md` section 3). The scan statement's best case agrees (p0 349.5 µs here against 349.5 µs for B2, both medians over runs of each run's p0) while p25 and p50 are 21-22 µs higher here, which reads as host state in the body of the distribution, not a floor shift.

| `bn` net row | A, `eeb88f47` (this run) | B2, `952c67e5` (BM-S6 remedy, 5 runs) | B, `6b771ec1` (BM close, 8 runs) | PostgreSQL 18.6 vacuumed (BM close, 4 runs) | D-all (this run) | D4 (this run) |
|---|---|---|---|---|---|---|
| literal | 32.0 ns [31.3, 33.1] (31.2 M rows/s) | 30.5 ns [30.3, 30.9] (32.8 M rows/s) | 31.0 ns [30.8, 32.6] (32.3 M rows/s) | 29.1 ns [28.6, 29.5] (34.4 M rows/s) | 24.7 ns (40.5 M rows/s) | 24.0 ns (41.7 M rows/s) |
| prepared | 32.4 ns [31.5, 34.5] (30.8 M rows/s) | 30.5 ns [30.2, 30.8] (32.8 M rows/s) | 31.2 ns [30.8, 32.5] (32.1 M rows/s) | not measured (simple protocol only) | 24.5 ns (40.7 M rows/s) | 23.7 ns (42.2 M rows/s) |

A is +1.5 ns (+4.9 %) over B2 and +1.0 ns (+3.2 %) over B on the literal path; the prepared path reads +1.9 and +1.2 ns. PostgreSQL's figure is the BM close's and was not re-measured here: **the floor was not measured in this run.** As a floor, not a reference: A is 1.10 times PostgreSQL, D-all 0.85 and D4 0.82: the whole of BT's reach would take the engine from 1.10 times the floor to about 0.85 times it, and a floor is a league check, not a target. **This file is the baseline for the rejected row at `v2.7.0-992-geeb88f47`**: the next run reads against A's 32.0 / 32.4 ns.

## 10. What the run teaches

1. **The rejected row's fixed cost is real and small-grained: 22-25 % of the row, in three pieces of about 3 ns, none of them large.** No single survey item dominates; the tuple read (D2) and the counters-and-charge (D3) are the two that stand alone.
2. **The sampler's frame share predicted the tuple read.** The BM close's profile put `ReadTupleIn` at 3.4 ns (`results-bm-close-v2.7.0-946-g6b771ec1.md` section 6); D2 saves 3.1 and 3.3 ns, 91-97 % of it, so the checks D2 keeps inline cost little and what it removes is the out-of-line call and the `StatusOr` wrapper. The mechanism (a `Status` holding a `std::string`, returned through memory; `status.hpp:277`) is the survey's reading and was not examined here: no profile, no disassembly.
3. **`Status` on a success path is the common cause, and it is now priced three times.** #5 (3.1-3.3 ns), #8 inside D3 (not separable from #7), and #9, `AcceptTupleAt`'s own return, at no more than 0.7-1.0 ns. BM-S6's remedy removed the same pattern from UPDATE's conjunct check; BT-R4 generalises it, and this run gives the rule its first per-site numbers.
4. **BT-Q2 acquired its data point.** Option (b), a reject-only fast path in the walk, can buy at most 0.7-1.0 ns (2-3 % of the row) over the adopted (a) once S2 and S3 are built, an upper bound because D4 also skips the span guard and the call. The premise gives (a) no reason to be reopened.
5. **BT's ceiling is about a quarter of the row, and the rest is BM's.** After D4 24.0 ns remain: BM's profile gives the kernel's compare 7.4 ns and the slot loop 6.2, leaving about 10 ns for the tuple read's checks, `Classify` (four tests and `Floor()`'s acquire load), the walk's visit and the residual (inference from a profile of another build, not measured here). A real S2 + S3 should land between A and D-all (24.5-24.7 ns), layout effects aside, so the row after BT stays above 24 ns, about 0.85 times the floor.
6. **S3 is the stage the premise prices worst.** D1's number includes an unguarded #2; the real hoist must either re-read the kernel after any `CheckStepShapeOnce` or pre-size `statement_->kernels` at statement entry (BT-R2), so it keeps a per-row cost or adds a per-statement one. D3's number removes the counts BT-R3 requires. S3's ceiling is D1 + D3 less their overlap (at most 5.7 ns), and the real figure is lower by an amount this run cannot give. Build S2 first and measure it alone; split S3 into the plain moves (#1, #4), the guarded hoist (#2) and C3, so each is priced separately.
7. **The harness was quiet here.** With a 0.2 ns A-against-A' gap the 1.0 ns per-stage bar is meaningful, not a formality; BT-S5's A/B should keep A' in its arms to show whether the gap stays that small.

## 11. Not run

Cut to the 20-minute stage budget and the order's §5.1: **sizes other than 10,000 rows** (rule 9's 200 and 1,000 rows: `--cells rows-200,rows-1k,bn` would add them; the BM close's B net row at 200 / 1,000 / 10,000 rows was 36.3 / 32.3 / 31.0 ns, so the saving is read as per-row but its size at the small counts is unmeasured); `wide-h` and the other cells (BT-S5's); the profile (no sampler build; each ablation's number is a delta, not a share of the row); a split of D1 into #1, #2 and #4 and of D3 into #7 and #8; the every-row `UPDATE` and the point `SELECT` guard cells (BT-S5's); a PostgreSQL twin (the floor is the BM close's figure); 5 runs, not 8; `group`/`strict` durability (a read has no commit wait); any suite on the patched binaries. Nothing above is extrapolated across a cut.
