# EM close: the front-end reuse against its base, server CPU per statement

**Finding.** On worktree `em-front-end-reuse` at `aa8eaebbc`
(`v3.5.1-74-gaa8eaebbc`) against `b46ac5140`, EM serves the two shapes it
was built for and costs the others a lean the A/A floor does not fully
exclude:

- **C4 (a qualified name's namespace from the catalog cache) saves 0.38 µs
  of 6.79 µs on `ns1.t2` (5.6 %) and 0.71 µs of 7.13 µs with a view in the
  instance (10.0 %)**, pooled over 18 rounds. Both runs agree and each
  saving is more than 9 times the floor on that cell (floor -0.2 % and
  +0.3 %).
- **C3 (a multi-variant plan's arguments from the statement's own parse)
  saves 0.80 and 0.94 µs of 11.7 µs on the join under a two-variant plan,
  text (6.8 % and 8.0 %)**; the cell's floor is wide (A2 vs A -1.5 % and
  -4.0 %, A's range 11.15 - 12.06 µs), so read the paired per-round
  difference, -0.80 and -0.48 µs, as the size: 0.5 to 0.9 µs, 4 to 8 %.
  The direction holds in both runs.
- **EM-R7 (the text path is never made slower) is not shown to hold to the
  floor.** The text point `SELECT` reads +0.04 and +0.07 µs (+0.7 % and
  +1.1 %) against floors of -0.4 % and 0.0 %; the prepared point `SELECT`,
  the text `DELETE`, `t2` under `USE ns1` and the text `UPDATE` lean the
  same way in the same sign, at +0.4 % to +1.5 % pooled. Each lean is
  inside A's own range in one run or the other, so none is a separated
  regression, but the sign repeats across two independent runs and EM-S1's
  C43 arm, so it is reported as a lean. The query-layer `SELECT`, the text
  `INSERT` and the three-relation join, text and prepared, are inside the
  floor (+0.2 %, -0.4 %, +0.1 %, +0.1 % pooled).

## 1. Stamp

| | |
|---|---|
| Executed | 2026-11-28; run 1 12:23:02 to 12:23:45 UTC, run 2 12:24:23 to 12:25:04 UTC (the same command twice, 9 rounds each) |
| Branch / commit | `worktree-em-front-end-reuse` at `aa8eaebbc`, `git describe --tags` = `v3.5.1-74-gaa8eaebbc`; the tree clean. The arms are built from detached worktrees of `b46ac5140` (open) and `aa8eaebbc` (close) |
| Arms | **A**: `b46ac5140`, `sha256` `3a15d379...`; **A2**: a byte copy of A (same hash), the floor; **B**: `aa8eaebbc`, `6390a5a6...`. No spike: both are the real `kds_server`. Full hashes in `archive/em-close-v3.5.1-74-gaa8eaebbc/binaries.sha256`; every server is started from a copy in the run directory |
| Binary provenance | each built in its own fresh worktree at 12:22:40 UTC, 40 s before the first cell, from the commit it is named for; the binaries are newer than both commits |
| Build | `build-release`, `CMAKE_BUILD_TYPE=Release`, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server` |
| Device | data under `/home/ukjin/em-close-run/work`: `/dev/mapper/fedora-root`, xfs; not tmpfs. The binaries and worktrees are on the same device |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536` (`tools/dk_close.py`'s `Server`), one fresh server and data file an arm a round |
| Pinning | server on CPU 2, client on CPU 5, both P-cores of the hybrid host |
| Host | load before run 1 5.56 / 5.72 / 4.61 (the two builds had just ended; no `cc1plus`, `cmake --build` or `ctest` process), after run 1 3.56 / 5.17 / 4.47; before run 2 1.95 / 4.56 / 4.29, after 1.88 / 4.19 / 4.18. The builds finished before the first cell; every cell's `hosts` carries its own reading |
| Driver | `tools/em_s1_ab.py --arm A=open --arm A2=open-copy --arm B=close --rounds 9 --k 2000` (its own docstring is its documentation; `bench/docs/` has no entry for it, a gap this run does not close), `n = 10000` rows; the arms' order rotates each round so each takes each position three times. A cell is server CPU per statement over `k = 2000` statements after a 300-statement warm-up |

## 2. Server CPU per statement (µs, medians of 9 rounds per run)

A2 vs A is the floor. B vs A is the close against the open. Run 1 then run 2,
then the two pooled (18 rounds an arm). Bold rows are the cells EM serves.
Server CPU per statement is a cost, not a rate; a rate row would invert it
without adding information, and a fixed per-statement cost has no
throughput form here.

### Run 1

| cell | A median | A range | A2 vs A (floor) | B vs A | B range |
|---|---|---|---|---|---|
| point `SELECT`, text | 6.250 | 6.182 - 6.297 | -0.027 (-0.4 %) | +0.041 (+0.7 %) | 6.228 - 6.337 |
| point `SELECT`, prepared | 5.385 | 5.333 - 5.644 | -0.026 (-0.5 %) | +0.060 (+1.1 %) | 5.383 - 5.510 |
| point `UPDATE`, text | 6.491 | 6.447 - 6.627 | -0.020 (-0.3 %) | +0.012 (+0.2 %) | 6.440 - 6.534 |
| point `UPDATE`, prepared | 5.731 | 5.693 - 5.803 | +0.034 (+0.6 %) | +0.002 (+0.0 %) | 5.705 - 5.781 |
| point `INSERT`, text | 5.919 | 5.875 - 5.951 | -0.027 (-0.5 %) | -0.036 (-0.6 %) | 5.850 - 5.955 |
| point `DELETE`, text | 6.027 | 5.959 - 6.086 | -0.036 (-0.6 %) | +0.046 (+0.8 %) | 6.006 - 6.198 |
| three-relation join, text | 10.126 | 10.020 - 10.157 | -0.033 (-0.3 %) | +0.021 (+0.2 %) | 10.039 - 10.493 |
| three-relation join, prepared | 7.503 | 7.421 - 7.607 | -0.021 (-0.3 %) | +0.019 (+0.3 %) | 7.421 - 7.852 |
| query-layer `SELECT v * 2`, text | 9.102 | 9.069 - 9.169 | +0.063 (+0.7 %) | +0.059 (+0.6 %) | 9.070 - 9.281 |
| **`ns1.t2`, text** | 6.786 | 6.707 - 7.351 | -0.010 (-0.2 %) | **-0.389 (-5.7 %)** | 6.331 - 6.493 |
| `t2` under `USE ns1`, text | 6.211 | 6.198 - 6.282 | +0.036 (+0.6 %) | +0.129 (+2.1 %) | 6.208 - 6.845 |
| **`ns1.t2`, one view, text** | 7.120 | 7.075 - 7.254 | +0.001 (+0.0 %) | **-0.690 (-9.7 %)** | 6.360 - 6.489 |
| **join under a two-variant plan, text** | 11.710 | 11.241 - 12.064 | -0.174 (-1.5 %) | **-0.802 (-6.8 %)** | 10.578 - 11.511 |
| join under a two-variant plan, prepared | 7.933 | 7.864 - 8.022 | +0.133 (+1.7 %) | -0.027 (-0.3 %) | 7.858 - 8.873 |

### Run 2

| cell | A median | A range | A2 vs A (floor) | B vs A | B range |
|---|---|---|---|---|---|
| point `SELECT`, text | 6.209 | 6.176 - 6.252 | +0.002 (+0.0 %) | +0.069 (+1.1 %) | 6.189 - 6.298 |
| point `SELECT`, prepared | 5.343 | 5.292 - 5.415 | +0.013 (+0.2 %) | +0.048 (+0.9 %) | 5.355 - 5.509 |
| point `UPDATE`, text | 6.439 | 6.386 - 6.604 | +0.052 (+0.8 %) | +0.053 (+0.8 %) | 6.435 - 6.510 |
| point `UPDATE`, prepared | 5.743 | 5.723 - 5.799 | +0.005 (+0.1 %) | -0.035 (-0.6 %) | 5.642 - 5.777 |
| point `INSERT`, text | 5.886 | 5.868 - 5.923 | +0.020 (+0.3 %) | -0.012 (-0.2 %) | 5.822 - 5.928 |
| point `DELETE`, text | 6.006 | 5.955 - 6.048 | +0.032 (+0.5 %) | +0.061 (+1.0 %) | 6.015 - 6.105 |
| three-relation join, text | 10.121 | 10.024 - 10.259 | -0.033 (-0.3 %) | -0.005 (-0.1 %) | 10.027 - 10.255 |
| three-relation join, prepared | 7.488 | 7.442 - 7.664 | -0.006 (-0.1 %) | -0.034 (-0.5 %) | 7.375 - 7.599 |
| query-layer `SELECT v * 2`, text | 9.146 | 9.071 - 9.198 | -0.003 (-0.0 %) | +0.002 (+0.0 %) | 9.077 - 9.560 |
| **`ns1.t2`, text** | 6.794 | 6.718 - 6.866 | -0.016 (-0.2 %) | **-0.383 (-5.6 %)** | 6.329 - 6.936 |
| `t2` under `USE ns1`, text | 6.239 | 6.168 - 6.820 | +0.023 (+0.4 %) | +0.068 (+1.1 %) | 6.259 - 6.337 |
| **`ns1.t2`, one view, text** | 7.127 | 7.083 - 7.280 | +0.040 (+0.6 %) | **-0.725 (-10.2 %)** | 6.340 - 7.004 |
| **join under a two-variant plan, text** | 11.784 | 11.153 - 11.962 | -0.467 (-4.0 %) | **-0.942 (-8.0 %)** | 10.596 - 11.553 |
| join under a two-variant plan, prepared | 7.978 | 7.781 - 8.136 | +0.104 (+1.3 %) | +0.142 (+1.8 %) | 7.874 - 8.645 |

### Pooled, 18 rounds an arm

| cell | A median | A range | A2 vs A (floor) | B vs A | B range |
|---|---|---|---|---|---|
| point `SELECT`, text | 6.216 | 6.176 - 6.297 | +0.003 (+0.0 %) | +0.065 (+1.0 %) | 6.189 - 6.337 |
| point `SELECT`, prepared | 5.358 | 5.292 - 5.644 | -0.001 (-0.0 %) | +0.071 (+1.3 %) | 5.355 - 5.510 |
| point `UPDATE`, text | 6.464 | 6.386 - 6.627 | +0.017 (+0.3 %) | +0.029 (+0.4 %) | 6.435 - 6.534 |
| point `UPDATE`, prepared | 5.742 | 5.693 - 5.803 | +0.009 (+0.2 %) | -0.009 (-0.2 %) | 5.642 - 5.781 |
| point `INSERT`, text | 5.905 | 5.868 - 5.951 | -0.008 (-0.1 %) | -0.027 (-0.4 %) | 5.822 - 5.955 |
| point `DELETE`, text | 6.018 | 5.955 - 6.086 | -0.013 (-0.2 %) | +0.052 (+0.9 %) | 6.006 - 6.198 |
| three-relation join, text | 10.124 | 10.020 - 10.259 | -0.031 (-0.3 %) | +0.008 (+0.1 %) | 10.027 - 10.493 |
| three-relation join, prepared | 7.495 | 7.421 - 7.664 | -0.013 (-0.2 %) | +0.011 (+0.1 %) | 7.375 - 7.852 |
| query-layer `SELECT v * 2`, text | 9.142 | 9.069 - 9.198 | +0.001 (+0.0 %) | +0.018 (+0.2 %) | 9.070 - 9.560 |
| **`ns1.t2`, text** | 6.793 | 6.707 - 7.351 | -0.016 (-0.2 %) | **-0.383 (-5.6 %)** | 6.329 - 6.936 |
| `t2` under `USE ns1`, text | 6.220 | 6.168 - 6.820 | +0.038 (+0.6 %) | +0.092 (+1.5 %) | 6.208 - 6.845 |
| **`ns1.t2`, one view, text** | 7.127 | 7.075 - 7.280 | +0.022 (+0.3 %) | **-0.713 (-10.0 %)** | 6.340 - 7.004 |
| **join under a two-variant plan, text** | 11.747 | 11.153 - 12.064 | -0.271 (-2.3 %) | **-0.887 (-7.6 %)** | 10.578 - 11.553 |
| join under a two-variant plan, prepared | 7.961 | 7.781 - 8.136 | +0.113 (+1.4 %) | +0.096 (+1.2 %) | 7.858 - 8.873 |

## 3. What the numbers say

**The served cells.** Qualified names (`ns1.t2`, and `ns1.t2` with a view in
the instance) and the plan-governed text join are the three cells whose
front-end work EM removed, and the three whose B median sits below A's
minimum in both runs (run 1: `ns1.t2` B median 6.397 against A's minimum
6.707; the view cell 6.43 against 7.075; the plan join 10.91 against
11.241). `ns1.t2` and the view cell clear
the floor by an order of magnitude. The plan join's savings are the least
certain figure in the file: its two floors disagree (-1.5 %, -4.0 %) and its
A range spans 0.9 µs, wider than any other cell's.

**The prepared plan join does not carry C3's saving and does not lose it.**
Prepared, the parse is kept and no second lex happens, so EM has nothing
to remove there. It reads -0.3 % and +1.8 % against floors of +1.7 % and
+1.3 %: inside the floor.

**EM-R7.** The cells EM does not serve, read against the floor:

| cell | B vs A, run 1 | B vs A, run 2 | floor (run 1, run 2) | verdict |
|---|---|---|---|---|
| point `SELECT`, text | +0.7 % | +1.1 % | -0.4 %, 0.0 % | lean, outside the floor in both runs; B's and A's ranges overlap in both |
| point `UPDATE`, text | +0.2 % | +0.8 % | -0.3 %, +0.8 % | inside the floor |
| point `INSERT`, text | -0.6 % | -0.2 % | -0.5 %, +0.3 % | inside the floor |
| point `DELETE`, text | +0.8 % | +1.0 % | -0.6 %, +0.5 % | lean, outside the floor in run 1, inside in run 2 |
| three-relation join, text | +0.2 % | -0.1 % | -0.3 %, -0.3 % | inside the floor |
| query-layer `SELECT v * 2`, text | +0.6 % | +0.0 % | +0.7 %, -0.0 % | inside the floor |
| `t2` under `USE ns1`, text | +2.1 % | +1.1 % | +0.6 %, +0.4 % | lean, outside the floor in both runs |

The query-layer `SELECT`, the shape EM-R7 names beside the point
shapes, shows no slowing: +0.6 % in a run whose floor is +0.7 %, and 0.0 %
in the other. The point `SELECT` and `UPDATE` pair is not made slower beyond
+1.1 % and +0.8 % in any single run, and the `UPDATE` is inside its floor.
The point `SELECT`, the `DELETE` and `t2` under `USE ns1` lean +0.07, +0.05
and +0.09 µs pooled (+1.0 %, +0.9 %, +1.5 %). Run 1's `t2` under `USE ns1`
B range reaches 6.845 µs against a median of 6.340 (one slow round of nine; the
paired median difference is +0.10 µs), and run 2's A reaches 6.820 on the same
cell, so a single outlier round touches each arm. The lean is one the floor
does not exclude on three cells, a 0.05 to 0.09 µs fixed cost on a 6 µs
statement. EM-S1's C43 arm, the same code in spike form, read +0.04 µs
(+0.6 %) on the point `SELECT` and +0.03 µs (+0.5 %) on `t2` under `USE`,
with the same sign; this run's size is larger on the point `SELECT` (+0.065
pooled) and on `t2` under `USE` (+0.092). The run supports the sign. It
does not isolate which of EM's two changes carries it, because the driver has
no arm with C3 or C4 alone.

## 4. Against this engine's earlier numbers

The baseline is EM-S1's C43 arm (`results-em-s1-poc-v3.5.1-70-g259a7fa3a.md`),
the spike of what this commit builds. The in-run deltas, not the medians,
are compared; EM-S1's A was EM-S0's counter-spike binary and these A medians
(6.22 against 6.27 µs on the point `SELECT`) are different binaries.

| cell | C43 vs A, EM-S1 | B vs A, this run (pooled) |
|---|---|---|
| `ns1.t2`, text | -0.397 µs (-5.7 %) | -0.383 µs (-5.6 %) |
| `ns1.t2`, one view, text | -0.767 µs (-10.6 %) | -0.713 µs (-10.0 %) |
| join under a two-variant plan, text | -0.669 µs (-5.7 %) | -0.887 µs (-7.6 %) |
| point `SELECT`, text | +0.039 µs (+0.6 %) | +0.065 µs (+1.0 %) |
| point `SELECT`, prepared | +0.040 µs (+0.7 %) | +0.071 µs (+1.3 %) |
| `t2` under `USE ns1`, text | +0.033 µs (+0.5 %) | +0.092 µs (+1.5 %) |
| query-layer `SELECT`, text | +0.034 µs (+0.4 %) | +0.018 µs (+0.2 %) |
| three-relation join, text | -0.048 µs (-0.5 %) | +0.008 µs (+0.1 %) |

The saving reproduces on the three served cells within 0.2 µs. The lean on
the point shapes is of the same sign and about 0.03 µs larger.

## 5. Waits, row-set size, limits

- **Waits (rule 3):** does not apply. The unit is server CPU per statement,
  from `/proc` over a block of `k` statements at `cores = 1`, `relaxed`;
  there is no durability wait or lock wait in it, and no client round trip,
  which is not counted. The cells are one front-end's fixed cost.
- **Row-set size (rule 9):** one size, `n = 10000`, was measured. The
  changes are per-statement fixed costs in the parse and bind, so a row-count
  sweep would measure the walk, not them. The point shapes read one row each
  and the join's rows are unchanged between arms. The 200 and 1K sizes were
  not run.
- **Extremes (rule 4b):** not approached. This is a per-statement CPU
  comparison of two binaries at one core, nothing was driven to saturation
  or under `strict`, and the result characterises the front end only.
- **Noise floor:** A2 against A is within ±0.6 % on 11 of 14 cells in each run;
  the exceptions are the plan join, text (-1.5 %, -4.0 %) and prepared
  (+1.7 %, +1.3 %), and the query-layer cell (+0.7 %, run 1) and the text
  `UPDATE` (+0.8 %, run 2). A delta under about
  ±0.8 % on a cell other than the plan join is not a finding; the plan
  join, text, is not resolved under 4 %.
- **Correctness:** the suite was not run in this measurement (the binaries
  are unmodified commits; EM-S2a and EM-S2b's own stages ran it). The
  driver ran with no `--verify` mode.
- **Budget:** builds about 10 minutes with two 6-job builds in parallel,
  two runs of 43 s each, no hold by another session.

Raw files: `archive/em-close-v3.5.1-74-gaa8eaebbc/` (`run1.json`,
`run2.json`, drivers' stdout, host readings, `binaries.sha256`, `tables.py`
that renders the tables above, reading `run1.json` and `run2.json` from the
run directory). Driver: `tools/em_s1_ab.py`, documented in its docstring only.
