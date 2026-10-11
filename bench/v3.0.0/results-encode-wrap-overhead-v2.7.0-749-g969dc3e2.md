# Row-encode overhead of the wrapped-literal pre-check in `EncodeOneValue`

**Verdict: no regression within noise.** On `fix-date-wrapped-literal-stored` at
`969dc3e2` (`v2.7.0-749-g969dc3e2`), `EncodeOneValue` carries a pre-switch
`IntLiteralWrapped` check that runs once per column per row on every INSERT and every
UPDATE re-encode. Against the base `6b0d8412` (`v2.7.0-747-g6b0d8412`), no statement shape
measured moved by more than the run's own noise floor. The floor is about 1.0 us at p50 and
3.5 to 4 us at p99 per single-row statement (1.3 % of a 76 us INSERT at p50); every B - A
delta on a statement that encodes is at or inside it. The check itself is below what this harness can
resolve: it is one type compare and one size compare per column, expected at a few ns, and
a 9-column row has nine of them. The amplified arms (250 rows x 9 columns encoded in one
INSERT) bound it from above at about 4 ns per column check, a bound set by the floor, not by
an effect.

Two binaries are compared, with a third as a layout control:

| Label | What it is |
|---|---|
| A (base) | `6b0d8412`, `v2.7.0-747-g6b0d8412`, the engine before the change |
| B (change) | `969dc3e2`, `v2.7.0-749-g969dc3e2`, the pre-switch check added, the signed-int arm's own copy removed |
| C (control) | A plus one dead, non-inlined, never-called 700-statement padding function placed before `EncodeOneValue`; no behaviour change. It moves code addresses the way B's change does and bounds what layout alone does |

## 1. Run stamp

| Item | Value |
|---|---|
| Date and time executed | 2026-10-10, 19:33 to 19:52 UTC. Single-row passes 19:33-19:38 (main 19:33-19:35, swap to 19:35, control to 19:38) plus one pair re-run 19:39-19:41; amplified passes 19:42-19:47 with the gated re-runs 19:48-19:52 |
| Branch / worktree | `worktree-fix-date-wrapped-literal-stored`, worktree `/home/cdkbs/ckdbs/.claude/worktrees/fix-date-wrapped-literal-stored` |
| B commit | `969dc3e2`, `git describe --tags` = `v2.7.0-749-g969dc3e2`, tree clean (committed 2026-10-10 19:26:23 UTC) |
| A commit | `6b0d8412`, `v2.7.0-747-g6b0d8412` (committed 2026-10-10 15:06:09 UTC) |
| Binaries | A `ee29d277d3b19939f1c93bdd57e789796c3a987dbd421deafc862afa2c7ef574`, a release build of `6b0d8412` made earlier in this session (18:20 to 18:22 UTC; its build tree no longer exists, so the source binary's own mtime is not available - the hash is the one an earlier archive, `wrapped-literal-overhead-v2.7.0-749-g94ca25f8/binaries.sha256`, recorded for it); B `7f37ad2d6db5b1b1cb1c8111c48fb11abd312afa867f26c8d90979ed441e6c2e`, built 19:30:06 UTC from `969dc3e2` (binary newer than HEAD); C `996254086edd3fb3fc1146169ec3ae676d76f3f623a7b81b7bea875679946b2f`, built 19:32:36 UTC from `6b0d8412` plus the padding (`drivers/mkpad.py`). Every server was started from a copy in the run directory, never from a build tree |
| Functional check | `func.log` in the archive: a wrapped literal stored into a `date` column is accepted by A and C and refused `integer literal ... does not fit column 'd'` by B, so B is the changed engine and C is not |
| Build type | Release (`CMAKE_BUILD_TYPE=Release`, B and C build trees) |
| Device | data files under `/home/cdkbs/bench-runs/ew/work/`, on `/dev/root`, ext4 (`df -T`); not tmpfs |
| Server configuration | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, `log_level = warn`, every relation `BTREE`; two servers per pair, both pinned to CPU 6 (one idle while the other is timed), client on CPU 7 |
| Pairing | a fresh server and fresh data file per (rows, pair, slot); in each pair every arm runs in 10 or 20 blocks that alternate which server goes first, with equal statement counts on both |
| Passes | main: 8 pairs x 3 sizes (A vs B); swap: 5 pairs x 2 sizes with the binaries exchanged between the two slots (A vs B); control: 8 pairs x 2 sizes (A vs C); control swap: 5 pairs x 2 sizes. The same four passes for the amplified arms |
| Host | 8 CPUs; no compile or test running at any kept run's start (pgrep of `cc1plus`, `cmake --build`, `ctest`); another session's pinned benchmark (servers on CPU 2) was running during part of the time and is accounted for in section 8 |
| Tests | no engine code was changed by this run; the correctness suite was not executed by this run. The change's own contract tests are in `tests/types_contract_test.cpp` and were not run here |

Row-count mapping: `--rows N` creates the wide table `w` with N rows (200, 1000, 10000)
before the first arm; the INSERT arms then add rows (1000 per server for the single-row
arms, 50,000 for the amplified one), so "rows" names the size the table had when the
measurement began. The driver, its flags and the runner are archived in `drivers/`
(`ew_ab.py`, `ew_bulk.py`, `ew_run.py`); no `bench/docs` entry exists for them because they
are one-off scratch drivers and `tools/` is untouched.

## 2. Single-row statements: throughput

The change costs nothing a serial client can see. Throughput of B against A stays within
1.1 % in every cell except INSERT wide at 10,000 rows, a tail artefact explained below the
table; the behaviour-neutral control C against A spans -1.12 % to +2.44 % over the same
cells.

Derived QPS = 1,000,000 / mean us per statement (a serial single connection, so the
conversion is exact), median over the pairs. "C vs A" is C against the A of its own pair.
C was not run at 200 rows. The two control rows (a pk point SELECT and `SHOW META`) encode
nothing and are the floor of the matrix.

**QPS matrix (derived: 1,000,000 / mean us per statement, serial single connection; median over pairs)**

| rows | statement | A (base) QPS | B (change) QPS | B vs A | C (A + dead padding) QPS | C vs A |
|---|---|---|---|---|---|---|
| 200 | INSERT wide (9 cols) | 12,339 | 12,269 | -0.57 % | not run | - |
| 200 | UPDATE 1 col, wide row | 12,342 | 12,261 | -0.66 % | not run | - |
| 200 | INSERT narrow (2 cols) | 13,235 | 13,284 | +0.37 % | not run | - |
| 200 | SELECT pk point (control) | 8,386 | 8,303 | -0.99 % | not run | - |
| 200 | SHOW META (control, no storage) | 13,408 | 13,371 | -0.27 % | not run | - |
| 1000 | INSERT wide (9 cols) | 12,291 | 12,417 | +1.03 % | 12,196 | -1.12 % |
| 1000 | UPDATE 1 col, wide row | 12,333 | 12,328 | -0.04 % | 12,293 | -0.53 % |
| 1000 | INSERT narrow (2 cols) | 13,322 | 13,270 | -0.39 % | 13,305 | +0.36 % |
| 1000 | SELECT pk point (control) | 8,295 | 8,266 | -0.35 % | 8,313 | +0.42 % |
| 1000 | SHOW META (control, no storage) | 13,496 | 13,418 | -0.58 % | 13,519 | +0.35 % |
| 10000 | INSERT wide (9 cols) | 12,081 | 12,556 | +3.93 % | 12,565 | +2.44 % |
| 10000 | UPDATE 1 col, wide row | 12,316 | 12,288 | -0.23 % | 12,297 | -0.31 % |
| 10000 | INSERT narrow (2 cols) | 13,214 | 13,239 | +0.19 % | 13,320 | +0.03 % |
| 10000 | SELECT pk point (control) | 8,634 | 8,682 | +0.55 % | 8,700 | +0.30 % |
| 10000 | SHOW META (control, no storage) | 13,457 | 13,329 | -0.95 % | 13,492 | -0.03 % |

The no-encode controls span -0.99 % to +0.55 % between A and B (all sizes); the statements
that encode span -0.66 % to +1.03 %, with INSERT wide at 10,000 rows (+3.93 %) the exception.
That cell is the mean being pulled by a few slow statements per run (the p50 moves by -0.20 us
there, and the p99 by -3.15 us). The A binary shows the slower mean in all four passes it
appears in (against B in the main and swap passes, against C in the control and control-swap
passes: 2.1 to 3.6 us), while B and C do not; C differs from A by dead padding alone
(+2.44 % here), so the difference is code placement, not the check, and is not a gain
credited to B.

## 3. Single-row statements: latency distribution

Medians over the pairs of each run's nearest-rank percentile, microseconds. The shape is
tight (p0 to p25 spans 1.9 to 5.1 us on every row). On the three statements that encode, the
A and B medians differ by at most 0.80 us at p0 (the fixed cost of the path) and 0.45 us at
p25 and p50; the two controls differ by up to 2.35 us at p0 and 0.75 us at p50.

**Latency distribution, rows = 200 (us; median over 8 pairs of each run's nearest-rank percentile; ops per run per server)**

| statement | ops | bin | p0 | p25 | p50 | p95 | p99 | mean |
|---|---|---|---|---|---|---|---|---|
| INSERT wide (9 cols) | 1000 | A | 72.8 | 74.9 | 76.0 | 96.5 | 113.2 | 81.0 |
|  |  | B | 73.2 | 75.2 | 76.3 | 96.8 | 113.8 | 81.5 |
| UPDATE 1 col, wide row | 2000 | A | 74.2 | 76.8 | 77.8 | 96.8 | 111.8 | 81.0 |
|  |  | B | 74.7 | 77.3 | 78.2 | 97.5 | 112.0 | 81.6 |
| INSERT narrow (2 cols) | 1000 | A | 68.8 | 71.2 | 72.1 | 91.5 | 105.4 | 75.6 |
|  |  | B | 69.2 | 71.2 | 72.1 | 91.3 | 106.4 | 75.3 |
| SELECT pk point (control) | 4000 | A | 108.4 | 112.8 | 114.8 | 136.3 | 151.2 | 119.3 |
|  |  | B | 109.2 | 113.5 | 115.6 | 137.8 | 153.6 | 120.4 |
| SHOW META (control, no storage) | 4000 | A | 67.6 | 71.4 | 72.3 | 87.6 | 93.9 | 74.6 |
|  |  | B | 69.2 | 71.5 | 72.5 | 88.1 | 94.5 | 74.8 |

**Latency distribution, rows = 1000 (us; median over 8 pairs of each run's nearest-rank percentile; ops per run per server)**

| statement | ops | bin | p0 | p25 | p50 | p95 | p99 | mean |
|---|---|---|---|---|---|---|---|---|
| INSERT wide (9 cols) | 1000 | A | 72.5 | 75.0 | 76.0 | 96.8 | 112.3 | 81.4 |
|  |  | B | 72.5 | 75.2 | 76.2 | 96.5 | 112.2 | 80.5 |
| UPDATE 1 col, wide row | 2000 | A | 74.2 | 76.6 | 77.6 | 96.7 | 112.1 | 81.1 |
|  |  | B | 74.6 | 77.0 | 78.0 | 96.8 | 111.8 | 81.1 |
| INSERT narrow (2 cols) | 1000 | A | 68.8 | 70.8 | 71.8 | 91.1 | 105.8 | 75.1 |
|  |  | B | 69.0 | 71.3 | 72.2 | 91.0 | 105.7 | 75.4 |
| SELECT pk point (control) | 4000 | A | 107.9 | 113.0 | 116.7 | 139.7 | 154.5 | 120.6 |
|  |  | B | 108.4 | 113.1 | 116.7 | 140.5 | 155.7 | 121.0 |
| SHOW META (control, no storage) | 4000 | A | 68.7 | 71.1 | 71.9 | 87.2 | 93.2 | 74.1 |
|  |  | B | 66.3 | 71.5 | 72.5 | 87.5 | 93.8 | 74.5 |

**Latency distribution, rows = 10000 (us; median over 8 pairs of each run's nearest-rank percentile; ops per run per server)**

| statement | ops | bin | p0 | p25 | p50 | p95 | p99 | mean |
|---|---|---|---|---|---|---|---|---|
| INSERT wide (9 cols) | 1000 | A | 73.1 | 75.2 | 76.2 | 96.2 | 113.3 | 82.8 |
|  |  | B | 72.3 | 74.8 | 75.8 | 95.8 | 110.0 | 79.6 |
| UPDATE 1 col, wide row | 2000 | A | 74.7 | 77.1 | 78.0 | 97.0 | 112.6 | 81.2 |
|  |  | B | 74.3 | 77.0 | 78.0 | 97.4 | 112.4 | 81.4 |
| INSERT narrow (2 cols) | 1000 | A | 69.0 | 71.3 | 72.2 | 91.7 | 107.7 | 75.7 |
|  |  | B | 68.9 | 71.2 | 72.2 | 91.5 | 105.1 | 75.5 |
| SELECT pk point (control) | 4000 | A | 107.8 | 111.0 | 112.5 | 130.8 | 138.4 | 115.8 |
|  |  | B | 106.9 | 110.5 | 111.9 | 130.2 | 138.7 | 115.2 |
| SHOW META (control, no storage) | 4000 | A | 69.3 | 71.2 | 72.1 | 87.2 | 93.2 | 74.3 |
|  |  | B | 69.6 | 71.8 | 72.8 | 88.0 | 94.2 | 75.0 |

## 4. Per-pair deltas and the noise floor

The deltas are B - A within one pair (the same instant, the same data), median and
quartiles over the 8 pairs, and the count of pairs with B slower than A at p50.

**B - A per-pair deltas (us), main pass: median [Q1, Q3] over 8 pairs, count of pairs with B > A**

| rows | statement | d p0 | d p25 | d p50 | d p75 | d p99 | d mean | d engine us/op | pairs B>A at p50 |
|---|---|---|---|---|---|---|---|---|---|
| 200 | ins-wide | +0.10 [-0.63, +0.65] | +0.15 [-0.20, +0.55] | +0.20 [+0.02, +0.42] | +0.15 [-0.20, +0.48] | +3.20 [-0.80, +7.38] | +0.17 [-2.03, +6.22] | +0.09 [-0.16, +0.39] | 6/8 |
| 200 | upd-wide | +0.65 [+0.22, +0.92] | +0.50 [+0.47, +0.70] | +0.55 [+0.47, +0.80] | +0.75 [+0.65, +1.05] | +0.70 [-0.00, +1.30] | +0.50 [+0.42, +0.82] | +0.51 [+0.29, +0.81] | 7/8 |
| 200 | ins-narrow | +0.70 [+0.30, +1.48] | +0.10 [-0.23, +0.45] | +0.20 [-0.05, +0.50] | +0.40 [+0.05, +0.75] | +0.90 [-0.78, +2.62] | +0.35 [-0.33, +0.73] | +0.16 [+0.04, +0.21] | 5/8 |
| 200 | sel-pk | -0.05 [-1.03, +0.85] | +0.25 [+0.12, +0.58] | +0.25 [-0.02, +0.50] | +0.40 [+0.18, +0.67] | +1.40 [-0.65, +6.45] | +0.25 [+0.12, +1.42] | +0.23 [+0.09, +0.53] | 5/8 |
| 200 | ctl-ping | +0.10 [-1.60, +5.80] | -0.00 [-0.30, +0.22] | -0.00 [-0.30, +0.32] | +0.00 [-0.12, +0.22] | +0.00 [-0.30, +1.32] | -0.01 [-0.13, +0.49] | +0.14 [+0.02, +0.27] | 4/8 |
| 1000 | ins-wide | -0.05 [-0.45, +0.22] | +0.30 [+0.15, +0.45] | +0.30 [-0.03, +0.55] | +0.20 [-0.50, +0.62] | -2.80 [-5.60, +1.88] | -0.23 [-3.32, +3.05] | +0.17 [-0.01, +0.27] | 5/8 |
| 1000 | upd-wide | +0.45 [-0.22, +0.73] | +0.40 [+0.05, +0.60] | +0.40 [+0.00, +0.63] | +0.10 [-0.13, +0.62] | -1.20 [-12.93, -0.15] | +0.07 [-1.08, +0.52] | +0.04 [-0.06, +0.34] | 5/8 |
| 1000 | ins-narrow | -0.05 [-0.12, +0.32] | +0.40 [+0.22, +0.55] | +0.40 [+0.20, +0.55] | +0.45 [+0.23, +0.80] | -0.55 [-2.42, +1.65] | +0.26 [+0.11, +0.69] | +0.13 [-0.06, +0.24] | 6/8 |
| 1000 | sel-pk | +0.90 [-0.45, +1.52] | -0.15 [-0.73, +0.72] | -0.15 [-0.70, +0.75] | +0.00 [-0.73, +0.55] | +2.10 [+1.32, +3.05] | +0.56 [-0.33, +0.93] | +0.01 [-0.32, +0.45] | 4/8 |
| 1000 | ctl-ping | -1.00 [-3.25, +0.28] | +0.40 [+0.20, +0.60] | +0.45 [+0.27, +0.63] | +0.35 [+0.10, +0.57] | +0.35 [+0.08, +1.03] | +0.43 [+0.21, +0.61] | +0.06 [-0.05, +0.15] | 7/8 |
| 10000 | ins-wide | -0.85 [-1.52, +0.15] | -0.25 [-0.63, +0.03] | -0.20 [-0.57, +0.10] | -0.15 [-0.82, +0.68] | -3.15 [-5.50, -0.05] | -2.60 [-3.89, +0.14] | -0.14 [-0.38, +0.11] | 2/8 |
| 10000 | upd-wide | -0.05 [-0.42, +0.32] | +0.10 [-0.73, +0.40] | +0.15 [-0.55, +0.50] | +0.40 [-0.15, +0.70] | +0.20 [-0.78, +1.55] | +0.27 [-0.49, +0.95] | +0.32 [-0.18, +0.45] | 5/8 |
| 10000 | ins-narrow | -0.15 [-0.92, +0.37] | -0.15 [-0.38, +0.20] | -0.05 [-0.35, +0.20] | -0.20 [-0.28, +0.30] | -3.40 [-5.60, -1.07] | -0.21 [-0.48, +0.30] | -0.08 [-0.24, +0.31] | 3/8 |
| 10000 | sel-pk | -0.40 [-1.57, -0.12] | -0.45 [-0.75, -0.22] | -0.45 [-0.73, -0.05] | -0.30 [-0.73, -0.07] | +0.40 [-0.25, +1.30] | -0.51 [-0.72, -0.03] | -0.32 [-0.53, -0.15] | 2/8 |
| 10000 | ctl-ping | +0.25 [-0.12, +0.57] | +0.50 [+0.18, +0.80] | +0.50 [+0.20, +0.85] | +0.45 [+0.12, +0.93] | +0.55 [+0.18, +1.15] | +0.69 [+0.15, +0.91] | +0.39 [+0.13, +0.45] | 6/8 |

**Noise floor.** The two control rows never encode, so any delta on them is the harness
and the binary layout. Across the four passes (A vs B, A vs B swapped, A vs C, A vs C
swapped), the largest median shift on a no-encode row is **1.00 us at p50** (SELECT pk,
C swapped, 10,000 rows) and **3.50 us at p99** (SELECT pk, C swapped, 1000 rows). Within
A vs B alone it is 0.50 us at p50. A delta smaller than that is not a finding. In the main
pass every B - A p50 delta on a statement that encodes lies between -0.20 and +0.55 us
(0.7 % of the statement) and every p99 delta between -3.40 and +3.20 us, in both signs; in
the swap pass the encode rows' p50 deltas are within 0.50 us and their p99 deltas reach 3.90
us in magnitude (UPDATE wide, 10,000 rows), a little over the no-encode p99 floor and with
the sign following the slot (there A is the slower binary, B - A = -3.90 us, against +0.20
us in the main pass). So the p50 floor is 1.0 us and the p99 floor 3.5 to 4 us, and no
encode row exceeds either in both passes.

The only row that leans one way is UPDATE wide, which is B slower than A at p50 by +0.55
(7 of 8 pairs), +0.40 (5 of 8) and +0.15 us (5 of 8) at 200, 1000 and 10,000 rows, with the
engine's own per-statement time up by +0.51, +0.04 and +0.32 us. Three things keep it from
being attributed to the check. The control C, which has no behaviour change, shows +0.35 and
+0.35 us at p50 on the same row. The no-encode `SHOW META` row on B is +0.45 and +0.50 us
at 1000 and 10,000 rows. And swapping the slots decomposes it (below): the binary effect on
UPDATE wide is +0.40 and +0.32 us, against +0.38 and +0.45 us for `SHOW META`, which runs
none of the changed code.

The control pass, A against C:

**C - A per-pair deltas (us), layout control, main pass (8 pairs)**

| rows | statement | d p50 | d p99 | d mean | d engine us/op |
|---|---|---|---|---|---|
| 1000 | ins-wide | +0.00 [-0.47, +0.13] | -2.15 [-5.10, +2.08] | +1.00 [-3.77, +3.92] | -0.03 [-0.16, +0.07] |
| 1000 | upd-wide | +0.35 [+0.03, +0.90] | +1.50 [+0.53, +3.20] | +0.27 [+0.02, +0.90] | +0.25 [-0.05, +0.60] |
| 1000 | ins-narrow | -0.15 [-0.40, +0.30] | -2.40 [-4.18, +0.67] | -0.22 [-0.50, +0.06] | +0.02 [-0.27, +0.36] |
| 1000 | sel-pk | -0.90 [-1.40, -0.02] | -0.45 [-1.67, +2.02] | -1.07 [-1.32, -0.03] | -0.42 [-0.58, +0.01] |
| 1000 | ctl-ping | -0.20 [-0.30, -0.05] | -0.60 [-0.70, +0.47] | -0.18 [-0.38, +0.09] | -0.05 [-0.25, +0.03] |
| 10000 | ins-wide | -0.25 [-0.55, +0.50] | -2.75 [-3.30, +0.40] | -2.05 [-4.82, +1.20] | +0.02 [-0.09, +0.28] |
| 10000 | upd-wide | +0.35 [-0.05, +0.80] | +0.60 [-1.20, +1.40] | +0.40 [+0.16, +0.74] | +0.47 [+0.24, +0.56] |
| 10000 | ins-narrow | +0.00 [-0.12, +0.32] | -1.65 [-2.55, +0.25] | -0.01 [-0.30, +0.34] | +0.10 [-0.06, +0.32] |
| 10000 | sel-pk | -0.20 [-0.58, +0.52] | +0.35 [-0.70, +1.50] | -0.18 [-0.58, +0.50] | -0.11 [-0.32, +0.03] |
| 10000 | ctl-ping | +0.15 [-0.17, +0.25] | -0.30 [-0.95, +0.55] | +0.07 [-0.31, +0.35] | +0.15 [-0.15, +0.26] |

Slot-swap decomposition. A pair's two servers sit in two slots; the swap pass runs the
binaries in the opposite slots so that the binary effect x = (d_main - d_swap) / 2 and the
slot effect s = (d_main + d_swap) / 2 can be separated. Left block: B against A; right
block: C against A.

**Slot-swap decomposition (us at p50): x = (d_main - d_swap)/2 is the binary effect, s = (d_main + d_swap)/2 the slot effect**

| rows | statement | B vs A: d_main | d_swap | x (binary) | s (slot) | C vs A: d_main | d_swap | x (binary) | s (slot) |
|---|---|---|---|---|---|---|---|---|---|
| 1000 | ins-wide | +0.30 | +0.50 | -0.10 | +0.40 | +0.00 | +0.10 | -0.05 | +0.05 |
| 1000 | upd-wide | +0.40 | -0.40 | +0.40 | +0.00 | +0.35 | -0.30 | +0.33 | +0.02 |
| 1000 | ins-narrow | +0.40 | +0.50 | -0.05 | +0.45 | -0.15 | +0.10 | -0.13 | -0.02 |
| 1000 | sel-pk | -0.15 | +0.10 | -0.12 | -0.03 | -0.90 | +0.80 | -0.85 | -0.05 |
| 1000 | ctl-ping | +0.45 | -0.30 | +0.38 | +0.08 | -0.20 | +0.30 | -0.25 | +0.05 |
| 10000 | ins-wide | -0.20 | -0.10 | -0.05 | -0.15 | -0.25 | +0.30 | -0.28 | +0.03 |
| 10000 | upd-wide | +0.15 | -0.50 | +0.32 | -0.18 | +0.35 | +0.10 | +0.12 | +0.23 |
| 10000 | ins-narrow | -0.05 | +0.20 | -0.13 | +0.07 | +0.00 | +0.10 | -0.05 | +0.05 |
| 10000 | sel-pk | -0.45 | +0.00 | -0.23 | -0.23 | -0.20 | +1.00 | -0.60 | +0.40 |
| 10000 | ctl-ping | +0.50 | -0.40 | +0.45 | +0.05 | +0.15 | +0.10 | +0.02 | +0.12 |

Read across a row: the binary effect of B on UPDATE wide (+0.40, +0.32) sits with the binary
effect of B on the no-encode `SHOW META` row (+0.38, +0.45), and C's own UPDATE wide effect
(+0.33, +0.12) is the same size. The remaining effects are mixed in sign and smaller. The
slot effect is up to 0.45 us and is the reason the swap pass exists.

## 5. Amplified statements: the per-column bound

A single-row statement runs the check 9 times in about 76 us, so a few-ns check is under
0.1 % of it and unresolvable beside a 1 us floor. The amplified arms multiply the per-column
work inside one statement to raise the check's share:

| Arm | Statement | Columns through `EncodeOneValue` per statement |
|---|---|---|
| ins-bulk | `INSERT INTO w VALUES` with 250 wide rows | 250 x 9 = 2250 |
| upd-range | `UPDATE w SET c3 = k WHERE id >= a AND id <= a+199` (200 rows) | 200 x 9 = 1800 re-encodes |
| sel-range | `SELECT * FROM w WHERE id >= a AND id <= a+199` (200 rows), control, no encode | 0 |

200 statements per server per pair per arm (20 blocks of 10), 8 pairs, same four passes.
The QPS here is derived from p50 (1,000,000 / p50 us), not from the mean: at 10,000 rows
the INSERT arm's mean is dominated by engine-side stalls that sit in slot A whichever binary
runs there (section 7), so a mean-derived QPS compares slots, not binaries.

**QPS matrix (derived: 1,000,000 / p50 us per statement, serial single connection; median over pairs)**

| rows | statement | A (base) QPS | B (change) QPS | B vs A | C (A + dead padding) QPS | C vs A |
|---|---|---|---|---|---|---|
| 200 | INSERT 250 wide rows | 1,033 | 1,029 | -0.36 % | not run | - |
| 200 | UPDATE 200 rows, 1 col | 3,070 | 3,048 | -0.70 % | not run | - |
| 200 | SELECT 200-row range (control) | 597 | 598 | +0.02 % | not run | - |
| 1000 | INSERT 250 wide rows | 1,030 | 1,030 | -0.01 % | 1,025 | -0.62 % |
| 1000 | UPDATE 200 rows, 1 col | 2,548 | 2,538 | -0.41 % | 2,501 | -1.48 % |
| 1000 | SELECT 200-row range (control) | 597 | 595 | -0.35 % | 595 | +0.01 % |
| 10000 | INSERT 250 wide rows | 1,029 | 1,026 | -0.23 % | 1,024 | -0.62 % |
| 10000 | UPDATE 200 rows, 1 col | 880 | 884 | +0.46 % | 880 | -0.10 % |
| 10000 | SELECT 200-row range (control) | 595 | 595 | +0.01 % | 594 | -0.04 % |

**Latency distribution, rows = 200 (us; median over 8 pairs of each run's nearest-rank percentile; ops per run per server)**

| statement | ops | bin | p0 | p25 | p50 | p95 | p99 | mean |
|---|---|---|---|---|---|---|---|---|
| INSERT 250 wide rows | 200 | A | 934.8 | 957.3 | 968.3 | 1154.9 | 2203.7 | 1035.2 |
|  |  | B | 930.0 | 958.9 | 971.8 | 1117.0 | 2220.9 | 1024.2 |
| UPDATE 200 rows, 1 col | 200 | A | 304.8 | 312.6 | 325.8 | 416.2 | 452.4 | 335.9 |
|  |  | B | 304.6 | 313.4 | 328.0 | 422.1 | 467.4 | 339.9 |
| SELECT 200-row range (control) | 200 | A | 1645.2 | 1665.0 | 1673.7 | 1710.2 | 1756.7 | 1677.7 |
|  |  | B | 1642.7 | 1664.3 | 1673.3 | 1732.1 | 1774.3 | 1681.0 |

**Latency distribution, rows = 1000 (us; median over 8 pairs of each run's nearest-rank percentile; ops per run per server)**

| statement | ops | bin | p0 | p25 | p50 | p95 | p99 | mean |
|---|---|---|---|---|---|---|---|---|
| INSERT 250 wide rows | 200 | A | 927.1 | 958.2 | 970.8 | 1072.2 | 1179.2 | 1002.4 |
|  |  | B | 925.5 | 960.2 | 970.8 | 1820.1 | 2604.1 | 1079.5 |
| UPDATE 200 rows, 1 col | 200 | A | 366.8 | 379.2 | 392.5 | 484.8 | 536.0 | 405.0 |
|  |  | B | 367.1 | 380.8 | 394.0 | 486.2 | 543.7 | 407.7 |
| SELECT 200-row range (control) | 200 | A | 1648.2 | 1666.5 | 1674.8 | 1724.8 | 1781.0 | 1685.6 |
|  |  | B | 1647.1 | 1672.2 | 1680.6 | 1737.7 | 1805.3 | 1686.2 |

**Latency distribution, rows = 10000 (us; median over 8 pairs of each run's nearest-rank percentile; ops per run per server)**

| statement | ops | bin | p0 | p25 | p50 | p95 | p99 | mean |
|---|---|---|---|---|---|---|---|---|
| INSERT 250 wide rows | 200 | A | 930.5 | 958.9 | 972.1 | 1288.8 | 2733.1 | 5874.1 |
|  |  | B | 933.5 | 963.3 | 974.4 | 1131.0 | 2870.1 | 1822.2 |
| UPDATE 200 rows, 1 col | 200 | A | 1073.6 | 1109.6 | 1136.7 | 1223.2 | 1323.5 | 1147.8 |
|  |  | B | 1072.6 | 1103.2 | 1131.4 | 1235.1 | 1300.9 | 1143.1 |
| SELECT 200-row range (control) | 200 | A | 1644.7 | 1673.2 | 1681.6 | 1729.8 | 1871.2 | 1691.2 |
|  |  | B | 1646.2 | 1671.7 | 1681.4 | 1724.2 | 1825.2 | 1686.7 |

B - A per pair:

**B - A per-pair deltas (us), main pass: median [Q1, Q3] over 8 pairs, count of pairs with B > A**

| rows | statement | d p0 | d p25 | d p50 | d p75 | d p99 | d mean | d engine us/op | pairs B>A at p50 |
|---|---|---|---|---|---|---|---|---|---|
| 200 | ins-bulk | -6.80 [-10.80, +0.50] | +0.60 [-0.40, +2.00] | +1.35 [-0.45, +3.60] | -2.40 [-8.60, +4.05] | +44.65 [-96.23, +181.72] | -13.26 [-22.70, -8.19] | -2.16 [-13.08, +3.58] | 6/8 |
| 200 | upd-range | -0.05 [-1.80, +2.62] | +1.40 [-0.45, +2.67] | +2.05 [+1.07, +2.88] | +3.85 [+1.52, +5.55] | +15.20 [-2.35, +76.52] | +2.78 [+1.39, +10.43] | +1.19 [+0.91, +2.13] | 7/8 |
| 200 | sel-range | -1.10 [-3.95, +1.13] | -0.40 [-1.20, -0.03] | -0.65 [-1.15, +0.40] | -0.10 [-2.27, +1.78] | +36.70 [+9.57, +113.50] | +2.80 [-3.47, +6.79] | -1.22 [-2.02, -0.22] | 3/8 |
| 1000 | ins-bulk | -0.20 [-4.38, +3.85] | +0.60 [-2.47, +6.15] | -0.50 [-1.45, +4.30] | +21.05 [+18.75, +28.20] | +1438.05 [+499.12, +1768.75] | +94.38 [+0.17, +1161.52] | +33.78 [-49.64, +1119.64] | 3/8 |
| 1000 | upd-range | +1.00 [-0.17, +4.80] | +1.20 [-1.05, +4.93] | +1.85 [+1.05, +3.23] | +3.95 [+1.15, +6.90] | +10.60 [-9.65, +36.65] | +4.83 [+1.20, +30.26] | -1.52 [-11.72, +1.57] | 7/8 |
| 1000 | sel-range | +1.35 [-2.95, +8.18] | +1.00 [-0.77, +4.50] | +1.05 [+0.77, +3.58] | +2.35 [-0.75, +5.80] | +20.05 [-23.25, +74.48] | +0.99 [-2.15, +4.47] | -1.08 [-1.34, +1.40] | 7/8 |
| 10000 | ins-bulk | +2.35 [-20.87, +10.07] | +2.05 [-6.65, +8.38] | +2.45 [-7.53, +5.12] | -9.15 [-21.93, +3.00] | +422.55 [-1435.00, +994.27] | -4091.14 [-6848.25, -2766.09] | -4733.51 [-6831.63, -2765.17] | 5/8 |
| 10000 | upd-range | -4.20 [-13.95, +3.82] | -7.00 [-13.12, +2.08] | -9.50 [-12.32, +5.93] | -6.00 [-14.67, +6.43] | -5.25 [-32.82, +11.05] | -6.12 [-10.36, +12.51] | -6.65 [-11.74, +2.27] | 3/8 |
| 10000 | sel-range | -4.25 [-6.28, +2.02] | -1.65 [-5.45, +0.12] | -1.50 [-3.02, -0.38] | -3.45 [-6.37, +1.03] | -1.60 [-95.52, +51.97] | -1.75 [-9.84, +1.46] | -0.80 [-1.42, -0.34] | 1/8 |

A against C (layout control) and the slot-swap decomposition:

**C - A per-pair deltas (us), layout control, main pass (8 pairs)**

| rows | statement | d p50 | d p99 | d mean | d engine us/op |
|---|---|---|---|---|---|
| 1000 | ins-bulk | +8.55 [+0.43, +8.93] | +1456.90 [+1129.90, +1514.23] | +84.94 [+75.86, +89.29] | +18.56 [+12.26, +35.50] |
| 1000 | upd-range | +5.95 [+3.30, +7.80] | +6.25 [-4.25, +14.30] | +5.70 [+3.91, +6.32] | +5.24 [+3.41, +5.79] |
| 1000 | sel-range | +0.05 [-0.50, +1.65] | +1.85 [-13.53, +29.20] | +1.65 [+0.24, +2.99] | +0.05 [-0.03, +0.25] |
| 10000 | ins-bulk | +6.95 [+3.62, +17.48] | +270.80 [-925.02, +791.50] | -3472.37 [-3653.68, -2785.11] | -3466.81 [-3649.01, -3089.93] |
| 10000 | upd-range | +1.20 [-5.88, +7.82] | +0.15 [-5.40, +18.70] | +0.83 [-3.52, +7.75] | +1.23 [-5.97, +6.48] |
| 10000 | sel-range | -0.30 [-1.40, +1.27] | +1.20 [-114.70, +27.15] | -1.05 [-8.48, +1.97] | +1.07 [+0.80, +1.80] |

**Slot-swap decomposition (us at p50): x = (d_main - d_swap)/2 is the binary effect, s = (d_main + d_swap)/2 the slot effect**

| rows | statement | B vs A: d_main | d_swap | x (binary) | s (slot) | C vs A: d_main | d_swap | x (binary) | s (slot) |
|---|---|---|---|---|---|---|---|---|---|
| 1000 | ins-bulk | -0.50 | +3.30 | -1.90 | +1.40 | +8.55 | +2.20 | +3.18 | +5.37 |
| 1000 | upd-range | +1.85 | +2.10 | -0.13 | +1.97 | +5.95 | -4.30 | +5.13 | +0.83 |
| 1000 | sel-range | +1.05 | +2.60 | -0.77 | +1.82 | +0.05 | +1.40 | -0.68 | +0.73 |
| 10000 | ins-bulk | +2.45 | -0.90 | +1.67 | +0.78 | +6.95 | -16.50 | +11.72 | -4.78 |
| 10000 | upd-range | -9.50 | +6.70 | -8.10 | -1.40 | +1.20 | +1.00 | +0.10 | +1.10 |
| 10000 | sel-range | -1.50 | +2.30 | -1.90 | +0.40 | -0.30 | +3.40 | -1.85 | +1.55 |

The floor for these arms is larger in absolute terms and about the same in relative terms:
C against A moves INSERT bulk by +8.55 and +6.95 us at p50 (and by 16.5 us in magnitude in
the swap pass at 10,000 rows), about 1 % of 970 us. B - A at p50 is +1.35, -0.50 and +2.45 us for INSERT bulk
(0.25 % at most) and +2.05, +1.85 and -9.50 us for UPDATE range. The UPDATE range delta is
B slower in 7 of 8 pairs at 200 and 1000 rows, but the no-encode `sel-range` control reads
+1.05 us (7 of 8) at 1000 rows and C moves UPDATE range by +5.95 us there, so that +2 us is
inside the layout floor and not attributable. The 10,000-row UPDATE range reads -9.50 us (3
of 8 pairs B slower), the other sign. The INSERT bulk tail at 1000 rows (B's p95 1820 us and
p99 2604 us against A's 1072 and 1179 us) is a slot tail, not the binary: C against A shows
the same +1457 us at p99 and the swap pass shows +1348 us with the binaries exchanged.

Upper bound on the check. With a layout floor of roughly 10 us on a statement that runs
2250 column encodes, any per-column cost larger than 10 / 2250, about 4 ns, would have
risen above the floor; none did. For UPDATE range the same arithmetic over 1800 re-encodes
with a floor of about 6 us gives about 3 ns. These bounds are derived from the measured
floor and are floor-limited: the run does not resolve the check, it excludes anything larger
than the expected few ns per column by a small factor.

## 6. Where the time goes

A latency is a sum. In these runs a single-row statement's server-side time is about a
third of what the client sees, and a wide row's encode is a small slice of the server's part.

**Engine vs client (A binary, main pass, medians)**

| rows | statement | client mean us | engine us/op (sched_foreground_polled_us / ops) | client minus engine us |
|---|---|---|---|---|
| 200 | ins-wide | 81.0 | 27.3 | 53.7 |
| 200 | upd-wide | 81.0 | 28.5 | 52.5 |
| 200 | ins-narrow | 75.6 | 24.8 | 50.8 |
| 200 | sel-pk | 119.3 | 33.5 | 85.7 |
| 200 | ctl-ping | 74.6 | 31.0 | 43.6 |
| 1000 | ins-wide | 81.4 | 27.5 | 53.9 |
| 1000 | upd-wide | 81.1 | 28.7 | 52.3 |
| 1000 | ins-narrow | 75.1 | 24.8 | 50.3 |
| 1000 | sel-pk | 120.6 | 35.1 | 85.4 |
| 1000 | ctl-ping | 74.1 | 30.8 | 43.3 |
| 10000 | ins-wide | 82.8 | 27.4 | 55.4 |
| 10000 | upd-wide | 81.2 | 28.8 | 52.4 |
| 10000 | ins-narrow | 75.7 | 24.8 | 50.9 |
| 10000 | sel-pk | 115.8 | 31.7 | 84.1 |
| 10000 | ctl-ping | 74.3 | 30.9 | 43.4 |

How to read it. "Engine" is the foreground scheduler's polled time per statement
(`sched_foreground_polled_us` over the statements, from `SHOW META`, A binary); "client minus
engine" is the socket round trip, client formatting and parsing, and the wake-up gap. A write
is 24.8 to 28.8 us of engine time inside 75 to 83 us of mean client time; a point SELECT is
31.7 to 35.1 us of engine time inside 116 to 121 us (the reply is formatted client-side).

- **Durability wait: not a separate wait here.** The configuration is `durability = relaxed`,
  whose statements do not wait for a device sync; no fsync wait was instrumented separately,
  and none is in the table.
- **Lock or conflict wait: none.** One connection per server, nothing concurrent; the
  statements wrote distinct rows.
- **Write statement, read, client and socket:** the engine and client-minus-engine columns
  above.
- **Encode, the part this change touches.** Within the A binary the 7 extra columns of the
  wide row cost 3.9 to 4.2 us at p50 over the narrow one (76.0 against 72.1 us at 200 rows;
  76.0 against 71.8 us at 1000; 76.3 against 72.3 us at 10,000), about 0.55 to 0.6 us per
  extra column for the whole pipeline (parse, encode, log, client). A check of a few ns per
  column is about 1 % of that per-column cost, which is consistent with nothing being
  visible.
- The amplified table below puts the same measure at scale: at 200 and 1000 rows INSERT bulk
  is 769 to 780 us of engine time inside about 1000 us of client time (250 rows, 3.1 us of
  engine per row, 0.35 us per column for the whole write path). The 10,000-row figure is
  the stall-dominated mean of section 7.

**Engine vs client (A binary, main pass, medians)**

| rows | statement | client mean us | engine us/op (sched_foreground_polled_us / ops) | client minus engine us |
|---|---|---|---|---|
| 200 | ins-bulk | 1035.2 | 779.5 | 255.8 |
| 200 | upd-range | 335.9 | 274.4 | 61.5 |
| 200 | sel-range | 1677.7 | 122.1 | 1555.6 |
| 1000 | ins-bulk | 1002.4 | 768.8 | 233.6 |
| 1000 | upd-range | 405.0 | 342.3 | 62.7 |
| 1000 | sel-range | 1685.6 | 124.6 | 1561.1 |
| 10000 | ins-bulk | 5874.1 | 5628.8 | 245.3 |
| 10000 | upd-range | 1147.8 | 1075.8 | 72.0 |
| 10000 | sel-range | 1691.2 | 125.2 | 1566.0 |

## 7. What the run teaches about the engine

- **The harness resolves about 1 us on a ~75 us statement, and the change is about two orders
  of magnitude below that.** An overhead of this kind is only ever bounded here, never
  measured: the finding is "no more than the floor", with the amplified arms tightening the
  bound to a few ns per column.
- **Code placement alone moves a mean by about 3 %.** INSERT wide at 10,000 rows is slower
  in the A binary by 2 to 3.6 us in the mean in four independent passes and the padding-only
  control C reproduces it. No change was made to any code on that path; the cause is layout
  of the binary, which any later single-binary comparison at this cell has to expect.
- **The amplified INSERT at 10,000 existing rows has single-statement stalls of 0.03 to 1.8
  seconds, larger in one slot.** The p50 is 972 us in both slots, but each server has one to
  three statements above 10 ms per run (in the first five pairs of the main pass: 1.4, 0.43,
  1.2, 1.6 and 0.42 s in slot A, 0.04, 0.30, 0.25, 0.41 and 0.29 s in slot B, as each run's
  largest). Slot A's mean engine time is 4.4 to 5.6 ms per statement in all four passes
  whichever binary runs there (A, B or C), against 0.8 to 0.9 ms in slot B. It follows the
  slot, not the binary, and is not examined further here: the cause is not established, and
  it is quoted so the mean-based numbers of that cell are not read as a comparison.
- **No expectation in `CLAUDE.md` or a spec was contradicted.** The run bears on no open
  decision; it is an overhead guard on the per-column encoder.

## 8. Baselines, limits and exclusions

- **This engine's own last run:** no results file under `bench/v3.0.0/` measures this
  statement shape (wide-row INSERT, one-column UPDATE of a wide row, bulk INSERT, range
  UPDATE), so this file is the baseline the next one is read against. The comparison made
  here is the base commit `6b0d8412` against `969dc3e2`, both measured in this run.
- **PostgreSQL floor:** not measured for this shape.
- **Extremes (4b):** nothing in this run approached an engine limit. The bound on the
  single-row cells is the client round trip: one serial connection, `relaxed` durability,
  `cores = 1`, about 12,000 statements per second, of which only 25 to 29 us per statement is
  the engine. The amplified arms are this change's extreme (2250 columns per statement);
  `strict` and `group` durability and a saturating multi-client load were not run; the
  change is CPU-side and ahead of the log, which is why the arms chosen amplify the CPU path
  instead.
- **Quiet-host rule.** A run was kept only if no compile or test was running and every CPU
  other than 6 and 7 was at most 0.5 busy in the 1 s before launch and during the run. Kept
  runs: 60 single-row and 60 amplified. **Excluded: 3 single-row runs and 35 amplified runs.**
  Single-row: the first main-pass pair at 10,000 rows (CPU 4 at 0.667 busy before launch,
  another session's client) and two re-runs of it (CPU 2 at 0.716 and 0.707 busy before
  launch, another session's server); the accepted re-run had every other CPU at or below 0.418
  before and during, but CPU 6 and 7 busy 0.33 and 5 s duration against 3 to 4 s for the other
  pairs. Dropping it (7 pairs at 10,000 rows) moves no B - A p50 delta there by more than 0.2
  us (`res-sens/` in the archive). Amplified: 34 of the first 60 runs had CPU 2 above 0.5
  busy before launch (another session's pinned server; 6 of them also CPU 4, its client),
  and 1 more was flagged in the second round; all 35 were re-run behind a gate that waits
  for a quiet sample.
  The excluded runs' files are in `res-excluded/` and `res-bulk*-excluded/`. A run
  kept after the gate was launched at a moment the other session's server was below 0.5 busy
  (max 0.417 during any kept run), so kept amplified runs are biased toward that session's
  quiet moments; the interleaved pairs cancel what is common to both servers.
- **Host load at each run's start:** `loadavg` 1 minute 1.21 to 2.76 for the single-row
  passes and 2.21 to 4.95 for the amplified ones (`hoststats.out`, `hoststats-bulk.out`;
  per-run values in each run's `.host.txt`).
- **Counts:** every cell is 8 pairs (5 for the swap passes); the p99 of a 1000-statement INSERT
  arm is the 11th slowest statement of its run, so p99 deltas of a few us are dominated by
  single events and are read against the floor, not on their own.

## 9. Reproduction

Archive: `bench/v3.0.0/archive/encode-wrap-overhead-v2.7.0-749-g969dc3e2/` carries
the drivers (`drivers/`), the runners' logs, per-run JSON (gzip), per-run host files, the
four passes for each arm family (`res`, `res-swap`, `res-ctl`, `res-ctl-swap`, `res-bulk`,
`res-bulk-swap`, `res-bulk-ctl`, `res-bulk-ctl-swap`), the excluded runs, the binary hashes
(`binaries.sha256`), the build logs and the rendered tables (`render.out`, `render2.out`).
The single-row passes ran as `ew_all.sh`, the amplified ones as `ew_all2.sh` followed by the
gated re-runs in `ew_all3.sh`; `ew_run.py` carries the run protocol (one fresh server pair
per row count and pair, the quiet gate with `EW_QWAIT`, `EW_SWAP=1` for the slot swap,
`EW_BINB=C` for the control) and `analyze.py`, `slot.py`, `render.py` and `render2.py`
produce the tables. The driver JSON is gzip: gunzip it before `analyze.py`.
