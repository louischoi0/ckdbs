# DH close: the page image's price, A (`f4138860`) against B (`22818e44`)

**Status: PARTIAL, stopped on the operator's word at 06:48 UTC on 2026-11-20.** Cells with kept
data (30 runs; 20 discarded for a foreign process): k3, m1, m2, m3, m4, rec1. The cells in section 7
did not run: the measurement was stopped on the operator's word with a run in flight (discarded),
after most of it had been spent holding for other sessions' builds, test suites and measurements
(section 1). Nothing is said of a cell that did not run.

**Finding.** B, the closing build, is **not slower than A, the opening build, in any cell that
ran**, and is faster outside the floor where pages are written often: +6.2 % at four cores
(floor 2.4 %), +15.2 % at eight cores (2.2 %), and **+14.8 % in the one-core pressure cell, the
cell the milestone is named for** (2,515 to 2,887 stmt/s; floor 3.0 %). The one-core 100K cell,
where DH-R4 says no lever may make it worse, reads +1.0 % with a floor of 8.2 %: B is not
slower and the difference is inside the floor (its A runs differ by 8 % between rounds). The
one control that ran (update 10K, one core) reads -0.4 % in a single round, floor unknown.
**What L3 costs is measured once (eight cores, 50 s, one round each, floor unknown): the mount
after `kill -9` took 0.72 s under B against 0.19 s under A, the replay was 1,456,727 records
against 161,207, and the log directory was 268 MB against 201 MB although B wrote a quarter as
much log in the window (137 MB against 545 MB).** Rows after the mount were 100,000 in both. The
attribution (one round of B3 and A0 in the pressure cell, rounds in the others): L3 alone buys
20 % and 59 % of the gap between A and A0 at four and eight cores and nothing at one core
(3 %, a single round); the packed image buys the rest (54 %, 38 % and 32 %).

## 1. Stamp

| | |
|---|---|
| Executed | 2026-11-20, 03:11 to 06:48 UTC (wall), stopped on the operator's word; run time 520 s of measured windows (about 10 minutes with warm-up, mount and stop) against **7,462 s (124 minutes) of recorded hold** on the kept runs |
| Branch / commit | `worktree-dh-page-image-volume`; A = `f4138860` (`v3.4.15-1-gf4138860`, the commit DH opened from), B = `22818e44` (`v3.4.15-5-g22818e44`, the closing code commit); the worktree's tools (`tools/dh_close.py`, `tools/dh_close_report.py`, `tools/dh_s0.py` as reviewed) are uncommitted; no engine file in the worktree was touched |
| Binaries | three fresh `git archive` extractions, Release (`-DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`), built 03:11 to 03:17 UTC while another session compiled in the same host (build time 2.6, 2.0 and 1.7 minutes). **A** unpatched; **B** unpatched; **X** = `22818e44` plus `dh-close-switches.patch` (archived beside this file; `<cstdlib>` added to `src/server/expeditor.cpp` and `src/wal/manager.cpp` by `build.sh`): `KDS_DH_NO_IMAGES=1` (image debt never paid, arm **A0**) and `KDS_DH_NO_PACK=1` (images logged verbatim, arm **B3**, the cadence alone). X with neither set is never used as B. Copies run: `bin/kds_server_A` sha256 `b743a60ce0d685061ee63d73ed71c2ae0bb7667782ff453a03938471f1ac42cd`, `bin/kds_server_B` `3712ab478d3e1e219fd2cfce3eac2a27f5f3d0ebdac5c3da8730ae02179e5948`, `bin/kds_server_X` `7bf5cc266d798909c0c9e9ca0c3806bd52bf854a2590858c400630642a1d340d`, all copied 03:17:35 UTC (newer than both commits); every server started from a copy |
| Device | `/home/louis/bench-runs/dh-close/` on `/dev/nvme0n1p1`, ext4; data files, WAL, snapshots and the binaries' copies there; never tmpfs |
| Snapshots | DH-S0's `update-lit` 100K, 1M (pool 2,048) and `insert` snapshots, loaded by an A-format binary, copied (`cp -r`) for every run; `select-pk:10000` and `update-lit:10000` loaded by A at 03:17 |
| Server | `buffer_pool_frames = 65536` (2,048 in pressure cells), `durability = group` (`strict` in cell 8), default config in every arm (`checkpoint_interval_ms` 5000; no L arm), port 17433, 3 s warm-up, **15 s** window |
| Driver | `tools/dh_close.py`, on `tools/dh_s0.py` (imported); per run a process scan **before** (a foreign compiler, linker, `ctest`, test binary or another `kds_server` holds the run until 30 s of quiet), a poll every 3 s **during**, and a scan **after**; a run with a foreign process at any of the three is discarded to `runs.jsonl.contaminated` and re-run (in the first 16 runs a run was kept after 3 attempts with `gave_up`; those rows were removed and re-run, and from `plan3` on a contaminated run is never kept: it re-runs until clean or the deadline). `hold_s` is the hold a result carries |
| Quiet gate | **The host was shared with other sessions' compiles (the `di-shape-identity`, `audit-dead-code`, `dl-*` worktrees), full `ctest` runs and at least two other measurements (`di-s0`, `cv-close`, a `kds_spike` run) throughout.** Holds before runs of this file reached 16 minutes at a time; the holds recorded on the 30 kept runs sum to 7,462 s (124 minutes; the longest single hold 1,953 s), and the hold before the run in flight at the stop is in no result. The runs that are in the tables passed all three scans; the discarded ones are in the archive. A reading of the load average alone is not the test (the 1-minute average lags a finished build by minutes: `load1` before a kept run reads up to 6.2 and no foreign process was present) |
| Tests | no engine code was changed; the suite was not run for this measurement and is not claimed |
| Raw | `bench/v3.4.15/archive/dh-close-v3.4.15-5-g22818e44/`: `runs.jsonl`, `runs.jsonl.contaminated`, `logs/` (each run's server stderr and its own log), `plan.txt`, `plan2.txt` and `plan3.txt` (the order was changed twice, below), patch, hashes (`sha256`), `progress.log`, `tables.md` |

**Budget.** The brief's one hour (building included) was not met and the measurement was
stopped on the operator's word at 06:48 UTC. Builds 6 minutes; kept runs about 10 minutes of
wall (520 s of windows); recorded hold 7,462 s. Plan order: `plan.txt` as briefed; `plan2.txt`
moved cell 4 and recovery ahead of cell 3; `plan3.txt` (the operator's order: cell 4, recovery,
cell 3, the controls, recovery round 2, select-pk at eight cores, scenario 2, the four-core
pressure and `strict` cells, then the rest) with a 2 hour wall bound that the stop preceded.

## 2. Results

**Arms.** A: `f4138860`. B: `22818e44`. B3: B's code with images logged verbatim (L3 alone).
A0: B's code with the image debt never paid (the bound). Every cell interleaves A, B, B3, A0
in each of two rounds. A and A0 have no `pool_fetch_image_bytes` field in the A binary: image
bytes for A are count x 8,224 (a 32-byte record header and the 8,192-byte page), marked so in
the table; A0 pays none.

### A against B

| cell | A | B | B vs A | floor (larger of A's and B's spread) | outside the floor? |
|---|---|---|---|---|---|
| m1: update 100K, 4 cores x 4 clients, group | 4,604 stmt/s (4,550; 4,659) | 4,888 stmt/s (4,939; 4,837) | +6.2 % | 2.4 % | yes, B faster |
| m2: update 100K, 8 cores x 8 clients, group | 7,595 stmt/s (7,516; 7,675) | 8,747 stmt/s (8,652; 8,841) | +15.2 % | 2.2 % | yes, B faster |
| m4: update 100K, 1 core x 4, group | 3,906 stmt/s (3,746; 4,065) | 3,945 stmt/s (3,914; 3,976) | +1.0 % | 8.2 % | no |
| m3: update 1M, pool 2,048 frames, 1 core x 4, group | 2,515 stmt/s (2,531; 2,500) | 2,887 stmt/s (2,843; 2,930) | +14.8 % | 3.0 % | yes, B faster |
| k3: update 10K, 1 core x 4 | 4,205 stmt/s (4,205) | 4,187 stmt/s (4,187) | -0.4 % | - | single run, no floor |

### Attribution (cells with all four arms)

| cell | A | B3 (L3 alone) | B (L3 + L2) | A0 (bound) | gap A0 vs A | gap recovered by B | by L3 (B3) | by L2 on top (B - B3) | floor |
|---|---|---|---|---|---|---|---|---|---|
| m1: update 100K, 4 cores x 4 clients, group | 4,604 stmt/s | 4,682 stmt/s | 4,888 stmt/s | 4,990 stmt/s | +8.4 % (+386 stmt/s) | 74 % of the gap | 20 % of the gap | 54 % of the gap | 2.4 % |
| m2: update 100K, 8 cores x 8 clients, group | 7,595 stmt/s | 8,297 stmt/s | 8,747 stmt/s | 8,789 stmt/s | +15.7 % (+1,194 stmt/s) | 96 % of the gap | 59 % of the gap | 38 % of the gap | 3.3 % |
| m3: update 1M, pool 2,048 frames, 1 core x 4, group | 2,515 stmt/s | 2,547 stmt/s | 2,887 stmt/s | 3,585 stmt/s | +42.5 % (+1,070 stmt/s) | 35 % of the gap | 3 % of the gap | 32 % of the gap | 3.0 % |

### Every micro run

| run | arm | throughput | p0 | p25 | p50 | p90 | p99 | server CPU | log written | images | image bytes | ckpt runs (counter) | ckpt runs by core | ckpt pages written | pages drained | wal_flush wait | run_queue wait | on_cpu | page_read wait | commits per sync | bad | hold / attempt |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| m1r1 | A | 4,550 stmt/s | 413 us | 723 us | 828 us | 1,088 us | 1,473 us | 108.9 us/stmt | 1,160 B/stmt | 0.111 img/stmt | 910 B/stmt (count x 8,224) | 11 runs | c0:3, c1:3, c2:3, c3:2 | 7,680 pages | 0 pages | 728.1 us/stmt | 0.9 us/stmt | 36.2 us/stmt | 0.0 us/stmt | 1.5 commits/sync | 0 stmts | 978 s / attempt 3 |
| m1r1 | B | 4,939 stmt/s | 399 us | 701 us | 790 us | 993 us | 1,285 us | 93.4 us/stmt | 348 B/stmt | 0.029 img/stmt | 99 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,238 pages | 0 pages | 671.1 us/stmt | 1.0 us/stmt | 29.7 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m1r1 | B3 | 4,628 stmt/s | 411 us | 745 us | 845 us | 1,068 us | 1,372 us | 109.5 us/stmt | 513 B/stmt | 0.032 img/stmt | 264 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,275 pages | 0 pages | 714.4 us/stmt | 0.7 us/stmt | 35.7 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m1r2 | A | 4,659 stmt/s | 363 us | 706 us | 809 us | 1,054 us | 1,404 us | 109.9 us/stmt | 1,166 B/stmt | 0.111 img/stmt | 916 B/stmt (count x 8,224) | 11 runs | c0:3, c1:3, c2:3, c3:2 | 7,930 pages | 0 pages | 708.8 us/stmt | 0.9 us/stmt | 40.1 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 678 s / attempt 2 |
| m1r2 | B | 4,837 stmt/s | 368 us | 703 us | 801 us | 1,032 us | 1,370 us | 101.1 us/stmt | 351 B/stmt | 0.030 img/stmt | 102 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,225 pages | 0 pages | 684.3 us/stmt | 0.9 us/stmt | 32.1 us/stmt | 0.0 us/stmt | 2.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m1r2 | B3 | 4,736 stmt/s | 335 us | 725 us | 817 us | 1,034 us | 1,389 us | 94.2 us/stmt | 523 B/stmt | 0.033 img/stmt | 274 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,408 pages | 0 pages | 705.8 us/stmt | 1.0 us/stmt | 31.4 us/stmt | 0.0 us/stmt | 3.8 commits/sync | 0 stmts | 287 s / attempt 3 |
| m1r2 | A0 | 4,956 stmt/s | 381 us | 687 us | 774 us | 987 us | 1,451 us | 91.7 us/stmt | 249 B/stmt | 0.000 img/stmt | 0 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,235 pages | 0 pages | 670.2 us/stmt | 1.1 us/stmt | 30.9 us/stmt | 0.0 us/stmt | - | 0 stmts | 0 s / attempt 1 |
| m2r1 | A | 7,516 stmt/s | 384 us | 835 us | 967 us | 1,324 us | 2,010 us | 106.5 us/stmt | 1,402 B/stmt | 0.140 img/stmt | 1,152 B/stmt (count x 8,224) | 21 runs | c0:3, c1:3, c2:3, c3:3, c4:3, c5:2, c6:2, c7:2 | 16,049 pages | 0 pages | 871.6 us/stmt | 1.1 us/stmt | 41.9 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m2r1 | B | 8,652 stmt/s | 414 us | 780 us | 885 us | 1,141 us | 1,716 us | 94.0 us/stmt | 324 B/stmt | 0.021 img/stmt | 75 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,828 pages | 0 pages | 745.1 us/stmt | 1.5 us/stmt | 34.8 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m2r1 | B3 | 8,296 stmt/s | 407 us | 800 us | 907 us | 1,172 us | 1,773 us | 99.0 us/stmt | 422 B/stmt | 0.021 img/stmt | 173 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,775 pages | 0 pages | 777.9 us/stmt | 1.3 us/stmt | 38.7 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m2r1 | A0 | 8,644 stmt/s | 150 us | 780 us | 881 us | 1,125 us | 1,844 us | 101.6 us/stmt | 249 B/stmt | 0.000 img/stmt | 0 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,736 pages | 0 pages | 737.5 us/stmt | 0.9 us/stmt | 39.4 us/stmt | 0.0 us/stmt | 1.6 commits/sync | 2 stmts | 0 s / attempt 1 |
| m2r2 | A | 7,675 stmt/s | 416 us | 819 us | 946 us | 1,298 us | 1,977 us | 106.6 us/stmt | 1,385 B/stmt | 0.138 img/stmt | 1,135 B/stmt (count x 8,224) | 21 runs | c0:3, c1:3, c2:3, c3:3, c4:3, c5:2, c6:2, c7:2 | 16,167 pages | 0 pages | 849.1 us/stmt | 0.9 us/stmt | 42.5 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m2r2 | B | 8,841 stmt/s | 386 us | 768 us | 871 us | 1,115 us | 1,561 us | 87.4 us/stmt | 319 B/stmt | 0.019 img/stmt | 70 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,677 pages | 0 pages | 731.1 us/stmt | 1.6 us/stmt | 31.4 us/stmt | 0.0 us/stmt | - | 0 stmts | 0 s / attempt 1 |
| m2r2 | B3 | 8,297 stmt/s | 420 us | 795 us | 899 us | 1,149 us | 2,154 us | 100.7 us/stmt | 417 B/stmt | 0.020 img/stmt | 169 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,634 pages | 0 pages | 777.9 us/stmt | 1.6 us/stmt | 37.1 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m2r2 | A0 | 8,934 stmt/s | 358 us | 763 us | 862 us | 1,093 us | 1,569 us | 97.4 us/stmt | 249 B/stmt | 0.000 img/stmt | 0 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,831 pages | 0 pages | 711.6 us/stmt | 1.0 us/stmt | 37.9 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 348 s / attempt 2 |
| m1r1 | A0 | 5,024 stmt/s | 393 us | 685 us | 771 us | 977 us | 1,290 us | 109.3 us/stmt | 249 B/stmt | 0.000 img/stmt | 0 B/stmt | 3 runs | c0:1, c1:1, c2:1 | 2,258 pages | 0 pages | 647.4 us/stmt | 0.7 us/stmt | 36.3 us/stmt | 0.0 us/stmt | 1.0 commits/sync | 0 stmts | 1,953 s / attempt 2 |
| m4r1 | A | 3,746 stmt/s | 472 us | 883 us | 995 us | 1,253 us | 1,601 us | 51.4 us/stmt | 522 B/stmt | 0.033 img/stmt | 273 B/stmt (count x 8,224) | 3 runs | c0:3 | 1,869 pages | 0 pages | 495.4 us/stmt | 39.3 us/stmt | 19.2 us/stmt | 0.0 us/stmt | 2.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m4r1 | B | 3,914 stmt/s | 453 us | 851 us | 953 us | 1,190 us | 1,497 us | 50.1 us/stmt | 354 B/stmt | 0.032 img/stmt | 105 B/stmt | 3 runs | c0:3 | 1,856 pages | 0 pages | 470.5 us/stmt | 38.9 us/stmt | 19.2 us/stmt | 0.0 us/stmt | 2.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m4r2 | A | 4,065 stmt/s | 498 us | 854 us | 959 us | 1,205 us | 1,528 us | 48.0 us/stmt | 499 B/stmt | 0.030 img/stmt | 250 B/stmt (count x 8,224) | 3 runs | c0:3 | 1,860 pages | 0 pages | 451.8 us/stmt | 36.8 us/stmt | 18.1 us/stmt | 0.0 us/stmt | 2.0 commits/sync | 0 stmts | 508 s / attempt 2 |
| m4r2 | B | 3,976 stmt/s | 509 us | 866 us | 978 us | 1,237 us | 1,623 us | 53.2 us/stmt | 353 B/stmt | 0.031 img/stmt | 104 B/stmt | 3 runs | c0:3 | 1,862 pages | 0 pages | 458.9 us/stmt | 41.0 us/stmt | 20.4 us/stmt | 0.0 us/stmt | 2.0 commits/sync | 0 stmts | 399 s / attempt 2 |
| m3r1 | A | 2,531 stmt/s | 775 us | 1,259 us | 1,390 us | 1,708 us | 2,415 us | 76.1 us/stmt | 6,902 B/stmt | 0.809 img/stmt | 6,652 B/stmt (count x 8,224) | 3 runs | c0:3 | 3,558 pages | 27,156 pages | 737.8 us/stmt | 68.8 us/stmt | 24.6 us/stmt | 4.1 us/stmt | 2.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m3r1 | B | 2,843 stmt/s | 679 us | 1,167 us | 1,289 us | 1,592 us | 2,211 us | 76.2 us/stmt | 3,080 B/stmt | 0.808 img/stmt | 2,830 B/stmt | 3 runs | c0:3 | 3,550 pages | 30,925 pages | 636.1 us/stmt | 77.4 us/stmt | 29.6 us/stmt | 4.0 us/stmt | 2.0 commits/sync | 0 stmts | 636 s / attempt 2 |
| m3r1 | B3 | 2,547 stmt/s | 772 us | 1,263 us | 1,385 us | 1,696 us | 2,365 us | 77.2 us/stmt | 6,909 B/stmt | 0.810 img/stmt | 6,659 B/stmt | 3 runs | c0:3 | 3,561 pages | 27,342 pages | 762.2 us/stmt | 65.7 us/stmt | 24.8 us/stmt | 4.1 us/stmt | 2.0 commits/sync | 0 stmts | 678 s / attempt 2 |
| m3r1 | A0 | 3,585 stmt/s | 495 us | 927 us | 1,037 us | 1,304 us | 2,027 us | 62.3 us/stmt | 249 B/stmt | 0.000 img/stmt | 0 B/stmt | 3 runs | c0:3 | 3,617 pages | 39,772 pages | 508.7 us/stmt | 69.8 us/stmt | 21.8 us/stmt | 3.9 us/stmt | 2.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| m3r2 | A | 2,500 stmt/s | 803 us | 1,272 us | 1,396 us | 1,709 us | 2,555 us | 74.9 us/stmt | 6,917 B/stmt | 0.811 img/stmt | 6,667 B/stmt (count x 8,224) | 3 runs | c0:3 | 3,596 pages | 26,807 pages | 738.5 us/stmt | 63.9 us/stmt | 23.8 us/stmt | 4.0 us/stmt | 2.0 commits/sync | 0 stmts | 384 s / attempt 2 |
| m3r2 | B | 2,930 stmt/s | 605 us | 1,134 us | 1,249 us | 1,540 us | 2,178 us | 74.6 us/stmt | 3,076 B/stmt | 0.807 img/stmt | 2,827 B/stmt | 3 runs | c0:3 | 3,531 pages | 31,938 pages | 640.5 us/stmt | 66.9 us/stmt | 28.7 us/stmt | 4.0 us/stmt | 2.0 commits/sync | 0 stmts | 614 s / attempt 2 |
| k3r1 | A | 4,205 stmt/s | 329 us | 835 us | 931 us | 1,151 us | 1,419 us | 47.6 us/stmt | 275 B/stmt | 0.003 img/stmt | 27 B/stmt (count x 8,224) | 3 runs | c0:3 | 207 pages | 0 pages | 434.6 us/stmt | 36.6 us/stmt | 17.8 us/stmt | 0.0 us/stmt | 2.0 commits/sync | 0 stmts | 0 s / attempt 1 |
| k3r1 | B | 4,187 stmt/s | 460 us | 837 us | 935 us | 1,159 us | 1,421 us | 47.9 us/stmt | 261 B/stmt | 0.003 img/stmt | 12 B/stmt | 3 runs | c0:3 | 207 pages | 0 pages | 438.6 us/stmt | 37.2 us/stmt | 17.8 us/stmt | 0.0 us/stmt | 2.0 commits/sync | 0 stmts | 0 s / attempt 1 |

### Scenario 2 runs

| run | arm | cores x bookers | throughput | booking p0 | p25 | p50 | p95 | p99 | commit p0 | p25 | p50 | p95 | p99 | log written | images | image bytes | ckpt runs (counter) | verify failures | committed | hold / attempt |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|

### Recovery and log kept (8 cores x 8 clients, update 100K, group, 50 s then kill -9)

| run | arm | throughput | log written in the window | images | image bytes | ckpt runs (counter) | ckpt rows by core | WAL files at the kill | WAL directory at the kill | first segment | mount (exec to first connection) | rows after | recovery records | redo applied | pages healed | recovery analysis / redo / undo | hold / attempt |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| rec1 | A | 7,266 stmt/s | 544.8 MB | 55,196 images | 453.9 MB | 72 runs | c0:9, c1:9, c2:9, c3:9, c4:9, c5:9, c6:9, c7:9 | 3 files | 201.3 MB | 7 | 0.19 s | 100000 rows | 161207 records | 0 pages | 0 pages | 52372 / 56739 / 0 us | 0 s / attempt 1 |
| rec1 | B | 8,067 stmt/s | 136.6 MB | 9,177 images | 36.3 MB | 9 runs | c0:1, c1:1, c2:1, c3:1, c4:1, c5:2, c6:1, c7:1 | 4 files | 268.4 MB | 0 | 0.72 s | 100000 rows | 1456727 records | 28691 pages | 0 pages | 291197 / 281853 / 0 us | 0 s / attempt 1 |


## 3. Reading the cells

Verdicts, A against B, the floor being the larger of the two arms' spread over the rounds:

| cell | A | B | B vs A | floor | verdict |
|---|---|---|---|---|---|
| update 100K, 4 cores x 4 clients | 4,604 stmt/s | 4,888 stmt/s | +6.2 % | 2.4 % | outside the floor, B faster |
| update 100K, 8 cores x 8 clients | 7,595 stmt/s | 8,747 stmt/s | +15.2 % | 2.2 % | outside the floor, B faster |
| update 1M, pool 2,048 frames, 1 core x 4 | 2,515 stmt/s | 2,887 stmt/s | +14.8 % | 3.0 % | outside the floor, B faster |
| update 100K, 1 core x 4 (DH-R4) | 3,906 stmt/s | 3,945 stmt/s | +1.0 % | 8.2 % | inside the floor; B not slower |
| update 10K, 1 core x 4 (control) | 4,205 stmt/s | 4,187 stmt/s | -0.4 % | not known (one round) | no verdict |

- **Nothing that ran reads slower outside the floor.** The one-core 100K cell's floor is wide
  (A's two rounds, 3,746 and 4,065 stmt/s, differ by 8.2 %), so "no slower" holds there only to
  that width; B's own rounds (3,914 and 3,976 stmt/s) differ by 1.6 %. The control reads -0.4 %
  from one round, which says nothing about the floor.
- **Four and eight cores.** A ran 11 and 21 checkpoint runs a 15 s window; B and B3 ran 3 at
  both, one a 5 s period, cores 0, 1 and 2 in turn. Images a statement fell from 0.111 to 0.029
  and from 0.139 to 0.020; image bytes a statement from about 910 to 100 B and from about
  1,140 to 70 B (A's are count x 8,224). Server CPU a statement is lower under B (93.4 and 101.1
  against 108.9 and 109.9 us at four cores; 94.0 and 87.4 against 106.5 and 106.6 at eight);
  `commit.wal_flush` falls from 728 and 709 us a statement to 671 and 684 us at four cores and
  from 872 and 849 us to 745 and 731 us at eight.
- **One core.** The cadence is one run a period at one core in A as well (3 runs a window
  in every arm), so L3 changes nothing there and the image count is unchanged (0.033 and 0.032
  images a statement at 100K; 0.809 and 0.808 in the pressure cell). **What moves is the image's
  size**: image bytes a statement fall from 273 and 250 B to 105 and 104 B (100K) and from 6,652 to
  2,830 B (pressure; -57 %), the log a statement from about 510 to 354 B and from 6,902 to
  3,080 B. At 100K that is worth +1.0 %, inside the floor. In the pressure cell it is worth
  **+14.8 %, outside it**: the cell DH-S0 priced at -28 % against A0.
- **Attribution** (one round of B3 and A0 in the pressure cell; two in the 100K cells):
  A0 is 42.5 % above A in the pressure cell (3,585 against 2,515 stmt/s). B recovers 35 % of that
  gap; **B3, the cadence alone, 3 %** (2,547 stmt/s, equal to A within noise, as it must at one
  core), so the packed image is all of it (32 %). B stays below A0 there: the pack cut the
  image's bytes by 57 % and the throughput gap by 35 %, and the rest is the fixed cost of an
  image and the page drain the pool still does (A0 drained 39,772 pages in the window, B 30,925,
  A 27,156). Four cores: B recovers 74 %, L3 20 %, L2 54 %; eight cores: 96 %, 59 %, 38 %
  (floors 2.4 % and 3.3 %).

## 4. Previous numbers

| cell | DH-S0 / DH-S1 (the `f4138860` engine, other sessions) | this file, A -> B |
|---|---|---|
| update 100K, 4 cores | S0 B 4,649 and A0 5,065 stmt/s; S1 B 4,492 stmt/s | 4,604 -> 4,888 stmt/s (A0 4,990) |
| update 100K, 8 cores | S0 B 7,836 and A0 8,980; S1 B 7,713 and A0 8,961 stmt/s | 7,595 -> 8,747 stmt/s (A0 8,789) |
| update 1M pool 2,048, 1 core | S0 B 2,576 and A0 3,582; S1 B 2,550 and A0 3,554 stmt/s | 2,515 -> 2,887 stmt/s (A0 3,585) |
| update 100K, 1 core | S0 B 4,064 and A0 4,298; S1 B 3,896 and A0 4,140 stmt/s | 3,906 -> 3,945 stmt/s |
| update 10K, 1 core | no number from DH-S0 or DH-S1 (DB's close measured 10K at one core; not re-read here) | 4,205 -> 4,187 stmt/s |

A here is DH-S0's B (the same engine, `f4138860`, image debt paid): its numbers (4,604, 7,595,
2,515 and 3,906 stmt/s) sit within 3 % of DH-S0's and DH-S1's B at every cell, which is the
host's drift between sessions and not a result. The A0 here (the closing code with the debt
unpaid) equals DH-S0's A0 within 2 %. PostgreSQL has no twin for this shape and its floor was
not measured.

## 5. What L3 costs: recovery and log kept

Eight cores x eight clients, update 100K, `group`, 50 s of load, then `kill -9` and a timed mount
(exec to first accepted connection) on the same files, **one round each: the floor is not known**.

| | A (`f4138860`) | B (`22818e44`) |
|---|---|---|
| throughput during the 50 s | 7,266 stmt/s | 8,067 stmt/s |
| log written in the window | 544.8 MB | 136.6 MB |
| images / image bytes | 55,196 images / 453.9 MB | 9,177 images / 36.3 MB |
| checkpoint runs in the window (counter) | 72 runs | 9 runs |
| checkpoint rows by core | cores 0 to 7: 9, 9, 9, 9, 9, 9, 9, 9 rows | cores 0 to 7: 1, 1, 1, 1, 1, 2, 1, 1 rows |
| WAL directory at the kill | 201.3 MB in 3 files, first segment 7 | **268.4 MB in 4 files, first segment 0** |
| **mount, exec to first connection** | **0.19 s** | **0.72 s** |
| records the mount replayed | 161,207 records | **1,456,727 records** |
| pages redo applied / healed | 0 / 0 pages | 28,691 / 0 pages |
| mount: analysis / redo / undo | 52.4 / 56.7 / 0 ms | 291.2 / 281.9 / 0 ms |
| rows after the mount | 100,000 rows | 100,000 rows |

- **Every core had had its turn under B** (50 s is ten periods; cores 0 to 7 each took at least
  one run, core 5 two), and the price is the replay range: **B's mount took 3.8 times as long and
  replayed 9 times as many records, because the oldest core's last checkpoint was up to eight
  periods old** (the work order's "about `cores + 1` periods"). B's log directory is **1.3 times
  larger (268 against 201 MB) although B wrote a quarter as much**: the recycler keeps what the
  oldest core's run still needs, and no segment had been recycled by the kill (first segment 0;
  A's first segment was 7, so A had recycled seven).
- **At this load the absolute cost is small**: a 0.72 s mount and 268 MB of log at 8,067 stmt/s.
  It grows with the write rate and with `cores` (the factor is `cores + 1` periods); a different
  `checkpoint_interval_ms` scales it directly. It is the cost the work order named and it is
  stated here as plainly as the +15 %.
- The distance from the anchor's redo start to the append LSN at the kill is **not exposed**
  by `sys.meta` or `sys.checkpoints` while running; the replay range here is the mount's record
  count. One round each: a repeat is needed for a floor, and none ran.

## 6. The controls

Ran: update 10K at one core, A and B, **one round**, 4,205 and 4,187 stmt/s (-0.4 %, floor
not known). **Not measured:** select-pk at one and eight cores, insert at four cores, scenario 2
(`s4a`, `s4b`), the four-core pressure cell and the `strict` cell. "No cell slower" is
therefore said only of the five cells in section 3, and for the controls it cannot be said.

## 7. Cut and not measured, by cell

Stopped on the operator's word at 06:48 UTC, after hours of holding for other sessions' builds,
test suites and measurements (`di-shape-identity`, `audit-dead-code`, `dl-*` compiles, `di-s0`,
`cv-close` and `kds_spike` runs), which left a quiet window for minutes at a time.

| cell | status |
|---|---|
| update 100K at 4 and 8 cores, update 1M at one core, update 100K at one core | measured, two rounds (the pressure cell's B3 and A0: one round) |
| recovery and log size, 8 cores | measured, **one round** (round 2 did not run) |
| update 10K, one core | measured, one round (A and B) |
| update 10K, one core, round 2 | not measured: stopped on the operator's word, in a hold |
| select-pk 10K at one core and at eight cores (A, B) | not measured: stopped on the operator's word |
| insert at four cores (A, B) | not measured: stopped on the operator's word |
| scenario 2 `s4a` and `s4b` (A, B) | not measured: stopped on the operator's word |
| four-core pressure cell (A, B, B3, A0) | not measured: stopped on the operator's word |
| 100K at four cores `strict` (A, B) | not measured: stopped on the operator's word |
| one-core 100K B3 and A0 arms; pressure cell round 2 B3 and A0 | not measured: stopped on the operator's word |

## 8. What the run teaches

- **The milestone's gain is real at all three shapes that ran and its size follows the lever
  that applies.** Where more than one core multiplies the cadence (four and eight cores) L3
  is a large part (20 % and 59 % of the gap to A0) and the pack the rest; at one core L3 has
  nothing to remove and the pack alone buys +14.8 % in the pressure cell, where the image is 0.81
  a statement and the log is the cost.
- **DH-R4 holds on what ran:** the one-core 100K cell is +1.0 % inside an 8.2 % floor. It is not
  shown stronger than that; B's log a statement there is lower (354 against 522 B), so no
  mechanism for a loss exists, but the floor is wide and a third round would narrow it.
- **L3's cost is the replay range, and it is paid at eight cores**: 0.72 s against 0.19 s, 9
  times the records, 1.3 times the log directory, from one round each. It is not free: the
  design traded it for the checkpoint rate. Whether it matters is the operator's call; the
  numbers are one round and grow with `cores` and the write rate.
- **A packed image is 37 to 42 % of the verbatim record on a table of small integers and 43 %
  in the pressure cell (2,830 against 6,652 B a statement)**; the ratio is the table's, not the
  engine's.
- **Not known:** the controls (select, insert, scenario 2), the four-core pressure cell and
  `strict`, and a floor for recovery.
