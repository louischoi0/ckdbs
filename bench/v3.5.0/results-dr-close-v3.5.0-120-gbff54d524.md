# DR close - what a primary key over an unchanged Keystone cost the statements that did not ask for it, and what it bought the ones that did

> The close of DR is `bench/v3.5.0/results-dr-close-v3.5.0-124-ge0d86ec62.md` (at `e0d86ec62`); this file stands as the record of the first close, at `bff54d524`, and of its diagnostic builds.

Measured 2026-11-23 between 07:23 and 07:27 UTC on worktree
`dq-cabin-miss-build`, branch `worktree-dr-primary-key`, at `bff54d524`
(`v3.5.0-120-gbff54d524`), the commit DR closes at. Order:
`instructions/v3.2.0/workorder-dr-primary-key-over-keystone.md` §5a and §7.
Driver `tools/dr_close.py` (documented in its docstring), unchanged, raw
output in `bench/v3.5.0/archive/dr-close-v3.5.0-120-gbff54d524/`. The
premise this close is read against is
`bench/v3.5.0/results-dr-s0-premise-v3.5.0-114-ge9df68d02.md`.

| stamp | value |
|---|---|
| Executed (UTC) | `ab`: 2026-11-23 07:23:17 to 07:25:01 (103.9 s); `built`: 07:25:06 to 07:26:58 (112.0 s) |
| Worktree / branch | `dq-cabin-miss-build` / `worktree-dr-primary-key` |
| Commit measured (B) | `bff54d524`, `v3.5.0-120-gbff54d524` (committed 07:21:33 UTC). The tree was clean for both runs. At 07:28:12 UTC, after both runs had ended, the parent session tried a fourth remedy in five tracked files (`docs/spec/heap-and-tuple.md`, `include/kds/catalog/rows.hpp`, `src/catalog/catalog.cpp`, `src/catalog/rows.cpp`, `tests/catalog_row_test.cpp`) at 07:28:12 UTC, after both runs had ended: the key-form check moved out of `SysTableRow::Decode` to where the catalog loads a relation (`CheckKeyForm`). It was built, measured (B5, section 1a) and withdrawn; the five files are restored and the tree is `bff54d524` again |
| Opening commit (A, A2) | `e9df68d02`, `v3.5.0-114-ge9df68d02` |
| Binary B | copy `/home/louis/dr-bench/close/kds_server_B`, sha256 `5d7524c6a3f2846d...` (first 16), source `build-release/kds_server` mtime 2026-11-23 07:21:43 UTC, after the commit's 07:21:33 UTC, so the binary is not older than HEAD; every server of the run started from the copy |
| Binary A and A2 | one file, copy `/home/louis/dr-bench/s0/kds_server_e9df68d0`, sha256 `e33a55473f81fade...`; A2 is a second server started from the same file (the control) |
| Build type | Release (`build-release`), nothing rebuilt for this run |
| Device | `/dev/nvme0n1p1`, ext4, mounted `/`; data files under `/home/louis/dr-bench/close/w` (`df -T` checked, not tmpfs) |
| Server configuration | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, one fresh server and data file per cell; server pinned to host CPU 2, driver to CPUs 0 and 4 |
| Shape | 100,000 rows; reads 20,000, writes 4,000, walked 40, ranges 1,000 statements a phase; 20 batches a phase |
| Rounds | `ab`: 5 rounds, three arms a round (order rotated per round, 15 cells); `built`: 3 rounds, 27 cells |
| Machine | `cc1plus` count 0 at every cell; quiet-gate hold 0.0 s in all 42 cells; host load average (1 min) 0.91 to 1.25 across the `ab` cells and 0.93 to 1.25 across the `built` cells, the driver's own server and client included; one idle `kds_server` (a `build-sandbox` instance) was resident throughout |

Server CPU a statement is the median of the phase's 20 batches; a table
cell reads "median over rounds [min-max over rounds]". Latency percentiles
are the median over rounds of each percentile. Wait breakdown: **does not
apply** (section 4).

## The answer first

**On `dq-cabin-miss-build` at `bff54d524`, B - A lies inside the control's
range in all 14 of 14 phases (7 btree, 7 heap).** The control is the
lowest and highest per-round A2 - A of the five rounds. B - A is positive
in 11 of the 14 phases, from +0.03 us to +0.42 us a statement (+0.14 us a
row of load on the btree, +0.17 us on the heap) and +1.79 us on the btree
`walked`; it is negative on btree `range` (-0.19 us), heap `range`
(-0.31 us) and heap `walked` (-78.41 us). The btree `load`'s B - A,
+0.14 us/row, is 0.01 us/row below the control's upper end (+0.15 us/row).
The `walked` phase, the one path DR added an instruction to, reads
+1.79 us (btree, +0.0 %) and -78.41 us (heap, -1.2 %), both inside. This
file does not call DR-R9 met or not met beyond that; section 1a states what
the run cannot separate.

**What a change elsewhere moves is as large as the deltas DR-R9 is read
by.** Across four builds of this branch that differ by the three remedies
and the final link (section 1a), B - A of a phase no remedy touched swings
by 0.32 us to 0.76 us on reads, updates, deletes and the btree range, 2.60 us on heap `range`, and
168 us to 366 us on `walked`; the same source linked twice (B4, final B)
differs by up to 0.85 us a statement on a point phase and 112 us on the
btree `walked`.

**What DR built is large where it was built for.** A heap's UPDATE by the
whole non-Keystone key costs 29.36 us to 34.44 us a statement (DR-S0:
6,488 us to 9,554 us), 221x to 277x less than the premise; a key move costs
34.01 us to 38.84 us (6,808 us to 9,557 us), 200x to 246x less; a key move
costs 4.40 us to 4.98 us more than the plain UPDATE by the same key.

**A btree primary key costs a read 1.91 us (F1, embedded) to 5.47 us (P1int,
indexed) over the named-key form, and a range of 100 rows 4.90 us to
89.0 us.** F1 is cheaper than the same key as a unique index (P1int) in
every phase.

## 1. DR-R9: the unchanged relation, A against A2 against B

Server CPU, `us` a statement (`us/row` for load), 100,000 rows, 5 rounds.
Control delta = A2 - A (medians). "Control range" is the lowest and highest
per-round A2 - A of the five rounds (the spread two runs of one engine
showed); "inside" means the median B - A lies between them. Percent is of
the A median.

btree (`id int64 PRIMARY KEY`, the relation as before):

| phase | A | A2 | B | A2 - A | control range | B - A | verdict |
|---|---|---|---|---|---|---|---|
| load | 3.46 us/row [3.42-3.48] | 3.46 us/row [3.44-3.57] | 3.59 us/row [3.56-3.77] | +0.00 us/row (+0.1 %) | -0.04 to +0.15 us/row | +0.14 us/row (+3.9 %) | inside |
| read | 22.71 us [22.64-23.08] | 23.00 us [22.35-23.60] | 23.13 us [23.05-23.30] | +0.29 us (+1.3 %) | -0.29 to +0.96 us | +0.42 us (+1.8 %) | inside |
| update | 25.45 us [24.69-25.49] | 25.53 us [25.35-26.25] | 25.61 us [25.11-26.08] | +0.08 us (+0.3 %) | +0.07 to +1.20 us | +0.16 us (+0.6 %) | inside |
| walked | 7,058.68 us [7,013.16-7,093.73] | 7,074.89 us [7,008.65-7,316.60] | 7,060.47 us [6,956.30-7,095.56] | +16.21 us (+0.2 %) | -42.20 to +257.92 us | +1.79 us (+0.0 %) | inside |
| range | 46.92 us [46.21-47.29] | 47.16 us [45.92-48.96] | 46.73 us [45.84-48.87] | +0.24 us (+0.5 %) | -0.72 to +2.75 us | -0.19 us (-0.4 %) | inside |
| insert | 23.18 us [22.99-24.52] | 23.20 us [22.42-23.55] | 23.21 us [23.12-23.61] | +0.02 us (+0.1 %) | -1.32 to +0.26 us | +0.03 us (+0.1 %) | inside |
| delete | 22.57 us [22.24-22.83] | 22.43 us [22.28-23.23] | 22.93 us [22.61-23.01] | -0.14 us (-0.6 %) | -0.21 to +0.62 us | +0.36 us (+1.6 %) | inside |

heap (`id int64 PRIMARY KEY ... HEAP`, the Keystone declared):

| phase | A | A2 | B | A2 - A | control range | B - A | verdict |
|---|---|---|---|---|---|---|---|
| load | 7.53 us/row [7.46-7.67] | 7.68 us/row [7.42-7.68] | 7.70 us/row [7.61-7.79] | +0.15 us/row (+2.0 %) | -0.25 to +0.22 us/row | +0.17 us/row (+2.3 %) | inside |
| read | 24.18 us [23.65-24.95] | 24.14 us [23.98-24.56] | 24.47 us [24.25-24.94] | -0.04 us (-0.2 %) | -0.71 to +0.61 us | +0.29 us (+1.2 %) | inside |
| update | 27.49 us [26.61-28.26] | 27.22 us [26.77-28.24] | 27.65 us [27.27-28.58] | -0.27 us (-1.0 %) | -1.17 to +1.63 us | +0.16 us (+0.6 %) | inside |
| walked | 6,818.13 us [6,815.52-7,055.28] | 6,845.83 us [6,774.54-6,872.31] | 6,739.72 us [6,697.87-6,976.44] | +27.70 us (+0.4 %) | -277.96 to +54.18 us | -78.41 us (-1.2 %) | inside |
| range | 71.31 us [69.70-72.87] | 71.22 us [70.19-73.88] | 71.00 us [70.10-73.62] | -0.09 us (-0.1 %) | -1.66 to +1.81 us | -0.31 us (-0.4 %) | inside |
| insert | 27.36 us [27.01-28.43] | 27.46 us [27.07-28.03] | 27.75 us [27.39-28.48] | +0.10 us (+0.4 %) | -0.89 to +0.40 us | +0.39 us (+1.4 %) | inside |
| delete | 23.72 us [23.06-24.22] | 23.71 us [23.16-24.03] | 23.75 us [23.58-24.57] | -0.01 us (-0.0 %) | -0.56 to +0.66 us | +0.03 us (+0.1 %) | inside |

**The `walked` phase** (`UPDATE t SET w = w + 1 WHERE v = k`, no key: the
one statement path DR added an instruction to, the question whether a
unique index names the row), 100,000 rows walked a statement:

| relation | A | A2 | B | B - A | control range | verdict |
|---|---|---|---|---|---|---|
| btree | 7,058.68 us | 7,074.89 us | 7,060.47 us | +1.79 us (+0.0 %), +0.02 ns a row walked | -42.20 to +257.92 us | inside |
| heap | 6,818.13 us | 6,845.83 us | 6,739.72 us | -78.41 us (-1.2 %), -0.78 ns a row walked | -277.96 to +54.18 us | inside |

Reading the table. The control is five rounds wide and its width differs by
phase and relation: the btree `walked` control spans 300.1 us, the heap's
332.1 us, and the btree `update` control (+0.07 to +1.20 us) lies wholly
above zero (A2 above A in all five rounds), so a B - A of +0.16 us is
inside a control that itself leans positive. A control from five
paired rounds bounds what two runs of one binary showed; it is not a
confidence interval. The heap `load` control (A2 - A = +0.15 us/row) is as
large as the heap's B - A (+0.17 us/row).

Latency (client), median over rounds of each percentile, `us`. Operation
counts: read 20,000, update/insert/delete 4,000, walked 40, range 1,000.

| relation | phase | arm | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| btree | read | A | 36.1 us | 38.5 us | 39.2 us | 45.0 us | 53.9 us |
| btree | read | A2 | 36.3 us | 38.8 us | 39.5 us | 47.1 us | 55.9 us |
| btree | read | B | 36.5 us | 38.9 us | 39.6 us | 46.5 us | 55.3 us |
| btree | update | A | 36.8 us | 39.2 us | 39.9 us | 48.4 us | 67.3 us |
| btree | update | A2 | 36.9 us | 39.2 us | 39.9 us | 49.4 us | 66.4 us |
| btree | update | B | 37.0 us | 39.5 us | 40.2 us | 49.1 us | 66.0 us |
| btree | walked | A | 6,945.9 us | 7,081.2 us | 7,137.9 us | 7,369.8 us | 7,731.2 us |
| btree | walked | A2 | 6,961.7 us | 7,104.8 us | 7,152.1 us | 7,412.5 us | 7,524.8 us |
| btree | walked | B | 6,906.3 us | 7,070.3 us | 7,131.3 us | 7,339.1 us | 7,381.7 us |
| btree | range | A | 68.8 us | 73.6 us | 75.8 us | 95.2 us | 134.0 us |
| btree | range | A2 | 69.2 us | 73.6 us | 76.0 us | 96.2 us | 125.1 us |
| btree | range | B | 68.8 us | 73.3 us | 75.3 us | 93.0 us | 137.9 us |
| btree | insert | A | 34.5 us | 36.8 us | 37.5 us | 47.9 us | 66.3 us |
| btree | insert | A2 | 34.6 us | 36.6 us | 37.3 us | 48.6 us | 67.6 us |
| btree | insert | B | 34.7 us | 37.1 us | 37.8 us | 47.4 us | 64.9 us |
| btree | delete | A | 34.0 us | 36.6 us | 37.3 us | 45.3 us | 64.3 us |
| btree | delete | A2 | 34.2 us | 36.7 us | 37.3 us | 46.3 us | 62.6 us |
| btree | delete | B | 34.5 us | 37.0 us | 37.7 us | 46.1 us | 62.9 us |
| heap | read | A | 36.9 us | 39.9 us | 40.7 us | 49.5 us | 59.6 us |
| heap | read | A2 | 36.9 us | 39.8 us | 40.7 us | 48.5 us | 59.6 us |
| heap | read | B | 35.9 us | 40.3 us | 41.1 us | 47.2 us | 58.4 us |
| heap | update | A | 37.8 us | 40.4 us | 41.5 us | 54.0 us | 78.9 us |
| heap | update | A2 | 37.6 us | 40.3 us | 41.3 us | 52.5 us | 77.1 us |
| heap | update | B | 38.0 us | 40.8 us | 42.1 us | 53.1 us | 77.0 us |
| heap | walked | A | 6,737.4 us | 6,854.7 us | 6,913.3 us | 7,590.4 us | 8,536.8 us |
| heap | walked | A2 | 6,710.5 us | 6,872.0 us | 6,923.4 us | 7,341.8 us | 7,649.3 us |
| heap | walked | B | 6,663.7 us | 6,759.3 us | 6,815.5 us | 7,335.2 us | 7,403.0 us |
| heap | range | A | 93.6 us | 97.8 us | 99.6 us | 125.2 us | 164.3 us |
| heap | range | A2 | 94.2 us | 97.8 us | 100.2 us | 124.4 us | 173.8 us |
| heap | range | B | 94.5 us | 97.8 us | 99.6 us | 132.2 us | 171.9 us |
| heap | insert | A | 38.5 us | 40.8 us | 41.8 us | 55.5 us | 79.0 us |
| heap | insert | A2 | 38.2 us | 40.8 us | 41.7 us | 52.9 us | 82.0 us |
| heap | insert | B | 38.8 us | 41.0 us | 41.9 us | 53.0 us | 81.7 us |
| heap | delete | A | 34.9 us | 37.5 us | 38.4 us | 46.4 us | 63.0 us |
| heap | delete | A2 | 34.9 us | 37.5 us | 38.4 us | 48.4 us | 66.5 us |
| heap | delete | B | 34.8 us | 37.7 us | 38.6 us | 46.7 us | 65.3 us |

The client latency carries the socket round trip (about 14 us to 15 us a
statement over the server CPU), so the p0 of a point statement is the
fixed cost. B's p0 minus A's is -1.0 us (heap read) to +0.5 us on the
point phases (read, update, insert, delete), 0.0 us (btree) and +0.9 us
(heap) on the range, and -39.6 us (btree) and -73.7 us (heap) on `walked`;
A2's p0 minus A's is -0.3 us to +0.2 us on the point phases, +0.4 us and
+0.6 us on the range, and +15.8 us and -26.9 us on `walked`.

### 1a. What a change elsewhere moves: the between-builds floor

The same driver, 5 rounds, against the same binary A, on five builds of
this branch. Every build below carries remedy 1; they differ as follows.
B - A is the median B minus the median A of that run, `us` a statement
(`us/row` for load), percent of that run's A.

| build | source | sha256 (first 16) |
|---|---|---|
| B1 | `866860b85` | `a2e295474dc27504` |
| B2 | B1 plus `Schema::embedded_key` packed into the struct's padding (uncommitted tree) | `f295d05a1affed4b` |
| B3 | B2 plus `InsertOneRow`'s embedded-key derivation moved out of its body (uncommitted tree) | `49bcb6e3b0ed12b4` |
| B4 | B3 plus `SysTableRow::Decode`'s refusal built out of line: the tree `bff54d524` records, built before the commit | `7cf0302c22b1ea8d` |
| final B | `bff54d524` rebuilt after the commit: B4's source, a different link (the two binaries differ) | `5d7524c6a3f2846d` |

btree:

| phase | B1 - A | B2 - A | B3 - A | B4 - A | final B - A | final control range (A2 - A) | final B vs control | swing over B2, B3, B4, final | B5 - A (not in the swing) |
|---|---|---|---|---|---|---|---|---|---|
| load | +0.13 us/row (+3.7 %) | +0.23 us/row (+6.6 %) | +0.10 us/row (+2.9 %) | +0.12 us/row (+3.4 %) | +0.14 us/row (+3.9 %) | -0.04 to +0.15 us/row | inside | 0.13 us/row | +0.08 us/row (+2.2 %) |
| read | +0.06 us (+0.3 %) | +0.81 us (+3.6 %) | +0.32 us (+1.4 %) | +0.09 us (+0.4 %) | +0.42 us (+1.8 %) | -0.29 to +0.96 us | inside | 0.72 us (3.2 points) | +0.25 us (+1.1 %) |
| update | +0.01 us (+0.0 %) | +0.07 us (+0.3 %) | +0.35 us (+1.4 %) | -0.07 us (-0.3 %) | +0.16 us (+0.6 %) | +0.07 to +1.20 us | inside | 0.42 us (1.7 points) | -0.16 us (-0.6 %) |
| walked | +58.20 us (+0.8 %) | +213.04 us (+3.0 %) | -153.15 us (-2.2 %) | -110.66 us (-1.6 %) | +1.79 us (+0.0 %) | -42.20 to +257.92 us | inside | 366.19 us (5.2 points) | -250.79 us (-3.5 %) |
| range | +1.05 us (+2.3 %) | -0.95 us (-2.0 %) | -0.21 us (-0.5 %) | -0.79 us (-1.7 %) | -0.19 us (-0.4 %) | -0.72 to +2.75 us | inside | 0.76 us (1.6 points) | -2.00 us (-4.1 %) |
| insert | -0.09 us (-0.4 %) | +0.14 us (+0.6 %) | +0.18 us (+0.8 %) | +0.07 us (+0.3 %) | +0.03 us (+0.1 %) | -1.32 to +0.26 us | inside | 0.15 us | -0.19 us (-0.8 %) |
| delete | +0.02 us (+0.1 %) | -0.15 us (-0.7 %) | +0.23 us (+1.0 %) | -0.25 us (-1.1 %) | +0.36 us (+1.6 %) | -0.21 to +0.62 us | inside | 0.61 us (2.7 points) | -0.19 us (-0.8 %) |

heap:

| phase | B1 - A | B2 - A | B3 - A | B4 - A | final B - A | final control range (A2 - A) | final B vs control | swing over B2, B3, B4, final | B5 - A (not in the swing) |
|---|---|---|---|---|---|---|---|---|---|
| load | +0.34 us/row (+4.5 %) | +0.16 us/row (+2.1 %) | +0.11 us/row (+1.4 %) | +0.13 us/row (+1.7 %) | +0.17 us/row (+2.3 %) | -0.25 to +0.22 us/row | inside | 0.07 us/row | +0.11 us/row (+1.5 %) |
| read | +0.38 us (+1.6 %) | +0.16 us (+0.7 %) | +0.35 us (+1.4 %) | +0.48 us (+2.0 %) | +0.29 us (+1.2 %) | -0.71 to +0.61 us | inside | 0.32 us (1.3 points) | +0.57 us (+2.4 %) |
| update | +0.04 us (+0.1 %) | +0.45 us (+1.7 %) | +0.02 us (+0.1 %) | -0.17 us (-0.6 %) | +0.16 us (+0.6 %) | -1.17 to +1.63 us | inside | 0.62 us (2.3 points) | +0.39 us (+1.4 %) |
| walked | +307.67 us (+4.6 %) | +89.88 us (+1.3 %) | +85.90 us (+1.3 %) | -27.28 us (-0.4 %) | -78.41 us (-1.2 %) | -277.96 to +54.18 us | inside | 168.29 us (2.5 points) | +30.64 us (+0.4 %) |
| range | +4.76 us (+6.7 %) | -2.91 us (-4.0 %) | -1.35 us (-1.9 %) | -1.48 us (-2.0 %) | -0.31 us (-0.4 %) | -1.66 to +1.81 us | inside | 2.60 us (3.6 points) | +0.10 us (+0.1 %) |
| insert | +0.94 us (+3.5 %) | +0.09 us (+0.3 %) | +0.49 us (+1.8 %) | +0.85 us (+3.1 %) | +0.39 us (+1.4 %) | -0.89 to +0.40 us | inside | 0.76 us | +0.46 us (+1.7 %) |
| delete | +0.71 us (+3.1 %) | -0.03 us (-0.1 %) | +0.52 us (+2.2 %) | +0.66 us (+2.8 %) | +0.03 us (+0.1 %) | -0.56 to +0.66 us | inside | 0.69 us (2.9 points) | +0.65 us (+2.8 %) |

"Swing" is the highest minus the lowest B - A among B2, B3, B4 and final B
(B1 is excluded: remedy 1 is in every later build). The final run's control
range is shown for reference; B1 to B4 were each read in their own runs
against their own A, and their B - A is not compared to this control as a
verdict.

B5 is a fifth diagnostic run (`diag-ab-B5.json`, same driver, 5 rounds,
same binary A, 07:29:24 UTC, 103.6 s): `bff54d524` plus a fourth remedy, the
key-form check moved out of `SysTableRow::Decode` to where the catalog loads
a relation (`CheckKeyForm`), an uncommitted tree, binary sha256
`09668d34e0741855`. Its column is B5 - A against that run's own A, computed
as the other columns are. It is not part of the swing, which stays B2, B3,
B4 and final B.

B5's load, the one phase the remedy's path is on, beside the final B's:

| relation | final B - A, load | B5 - A, load | B5 run's control (A2 - A, 5 rounds) | B5 vs its control |
|---|---|---|---|---|
| btree | +0.14 us/row (+3.9 %) | +0.08 us/row (+2.2 %) | -0.17 to +0.04 us/row | outside by +0.04 us/row |
| heap | +0.17 us/row (+2.3 %) | +0.11 us/row (+1.5 %) | -0.40 to +0.10 us/row | outside by +0.01 us/row |

Per-round load in the B5 run, `us/row`, rounds 0 to 4:

| relation | arm | round 0 | round 1 | round 2 | round 3 | round 4 |
|---|---|---|---|---|---|---|
| btree | A | 3.490 us/row | 3.591 us/row | 3.599 us/row | 3.433 us/row | 3.494 us/row |
| btree | A2 | 3.460 us/row | 3.451 us/row | 3.427 us/row | 3.474 us/row | 3.443 us/row |
| btree | B5 | 3.573 us/row | 3.566 us/row | 3.545 us/row | 3.679 us/row | 3.571 us/row |
| heap | A | 7.859 us/row | 7.379 us/row | 7.549 us/row | 7.682 us/row | 7.407 us/row |
| heap | A2 | 7.455 us/row | 7.439 us/row | 7.491 us/row | 7.421 us/row | 7.507 us/row |
| heap | B5 | 7.663 us/row | 7.765 us/row | 7.642 us/row | 7.673 us/row | 7.570 us/row |

B5's load delta is 0.06 us/row (btree) and 0.06 us/row (heap) below the final
B's, a difference smaller than the 0.13 us/row and 0.07 us/row by which the
builds B2 to final B differ from one another on those phases, and it lies
just outside its own run's control on both relations (by 0.04 us/row and
0.01 us/row). The remedy was withdrawn because the load did not move by more
than the builds differ from one another; `Decode`'s key-form check stays
where it was.

What the table shows, and nothing more:

(a) **Phases whose B - A changed sign or size between B2, B3, B4 and final
B although no remedy between them touched their code path.** Remedy 2
touches only INSERT and remedy 3 only the decode of a catalog row on a
named key's INSERT, so after B2 the DELETE, the reads, the range, the
keyed update and the walked UPDATE are the same source in all four builds.
Their B - A, in the order B2, B3, B4, final B:

| relation | phase | B - A in B2, B3, B4, final B | sign changes | swing |
|---|---|---|---|---|
| btree | read | +0.81, +0.32, +0.09, +0.42 us | none (all positive) | 0.72 us (3.2 points) |
| btree | update | +0.07, +0.35, -0.07, +0.16 us | + to - to + | 0.42 us (1.7 points) |
| btree | walked | +213.04, -153.15, -110.66, +1.79 us | + to - to + | 366.19 us (5.2 points) |
| btree | range | -0.95, -0.21, -0.79, -0.19 us | none (all negative) | 0.76 us (1.6 points) |
| btree | delete | -0.15, +0.23, -0.25, +0.36 us | - to + to - to + | 0.61 us (2.7 points) |
| heap | read | +0.16, +0.35, +0.48, +0.29 us | none (all positive) | 0.32 us (1.3 points) |
| heap | update | +0.45, +0.02, -0.17, +0.16 us | + to - to + | 0.62 us (2.3 points) |
| heap | walked | +89.88, +85.90, -27.28, -78.41 us | + to - | 168.29 us (2.5 points) |
| heap | range | -2.91, -1.35, -1.48, -0.31 us | none (all negative) | 2.60 us (3.6 points) |
| heap | delete | -0.03, +0.52, +0.66, +0.03 us | - to + | 0.69 us (2.9 points) |

Five of these ten phases changed sign across the four builds, with no
source change on their path. The swing is the spread a binary's layout
alone produced on this host for a phase no remedy touched: 0.32 us to
0.76 us on the point phases and the btree range, 2.60 us on the heap
range (3.6 points), 168.29 us (heap) and 366.19 us (btree) on `walked`.
The keyed update is not named in the remedies' description of what they
touch; it is counted here as untouched on that basis, and its swing is
0.42 us (btree) and 0.62 us (heap).

(b) **B4 against the final B, the same source linked twice.** The two
binaries differ in bytes. Their B arm and their B - A:

| relation | phase | B, B4 | B, final | B, final minus B4 | B - A, B4 | B - A, final | B - A, final minus B4 |
|---|---|---|---|---|---|---|---|
| btree | load | 3.60 us/row | 3.59 us/row | -0.01 us/row (-0.3 %) | +0.12 us/row | +0.14 us/row | +0.02 us/row |
| btree | read | 23.01 us | 23.13 us | +0.12 us (+0.5 %) | +0.09 us | +0.42 us | +0.33 us |
| btree | update | 25.51 us | 25.61 us | +0.10 us (+0.4 %) | -0.07 us | +0.16 us | +0.23 us |
| btree | walked | 6,958.42 us | 7,060.47 us | +102.05 us (+1.5 %) | -110.66 us | +1.79 us | +112.45 us |
| btree | range | 46.19 us | 46.73 us | +0.54 us (+1.2 %) | -0.79 us | -0.19 us | +0.60 us |
| btree | insert | 23.17 us | 23.21 us | +0.04 us (+0.2 %) | +0.07 us | +0.03 us | -0.04 us |
| btree | delete | 22.88 us | 22.93 us | +0.05 us (+0.2 %) | -0.25 us | +0.36 us | +0.61 us |
| heap | load | 7.67 us/row | 7.70 us/row | +0.04 us/row (+0.5 %) | +0.13 us/row | +0.17 us/row | +0.04 us/row |
| heap | read | 24.80 us | 24.47 us | -0.33 us (-1.3 %) | +0.48 us | +0.29 us | -0.19 us |
| heap | update | 27.40 us | 27.65 us | +0.25 us (+0.9 %) | -0.17 us | +0.16 us | +0.33 us |
| heap | walked | 6,813.53 us | 6,739.72 us | -73.81 us (-1.1 %) | -27.28 us | -78.41 us | -51.13 us |
| heap | range | 71.38 us | 71.00 us | -0.38 us (-0.5 %) | -1.48 us | -0.31 us | +1.17 us |
| heap | insert | 28.60 us | 27.75 us | -0.85 us (-3.0 %) | +0.85 us | +0.39 us | -0.46 us |
| heap | delete | 24.24 us | 23.75 us | -0.49 us (-2.0 %) | +0.66 us | +0.03 us | -0.63 us |

One source, two links: B - A moved by up to 0.63 us on a point phase
(heap delete), by 1.17 us on heap range and by 51 us to 112 us on
`walked`. The B arm alone moved by up to 0.85 us (heap insert, -3.0 %) and
102.05 us (btree walked, +1.5 %). The A medians also differ between the two
runs (for example heap insert 27.75 us in the B4 run and 27.36 us in the
final run), so a part of each B - A difference is the A arm's own
run-to-run movement.

(c) **Phases outside the final run's control range in the final B: none.**
All 14 B - A values of the final B are inside, so there is no phase for which
to ask whether its final B - A exceeds the largest swing of an untouched
phase. For reference, each untouched phase's final B - A is smaller in
magnitude than that phase's own swing (btree: read +0.42 against 0.72 us,
update +0.16 against 0.42, walked +1.79 against 366.19, range -0.19 against
0.76, delete +0.36 against 0.61 us; heap: read +0.29 against 0.32, update
+0.16 against 0.62, walked -78.41 against 168.29, range -0.31 against 2.60,
delete +0.03 against 0.69 us). The load and insert phases are on the
touched paths (remedy 2, remedy 3), so no untouched-phase swing applies to
them; the btree load's B - A, +0.14 us/row, is above that phase's own
swing across the four builds (0.13 us/row) and 0.01 us/row below the control's
upper end, and the heap load's, +0.17 us/row, is above its own swing (0.07
us/row) and below the control's upper end (+0.22 us/row).

What the run cannot separate: no hardware counters exist on this host
(`perf` is absent), so instructions were not counted and a B - A in time
cannot be split into instructions and layout; and no third arm was built
from the opening commit with a perturbed layout, so the layout spread of
`A`'s own source is unmeasured and the between-builds swing above is the
only layout estimate, taken on B's source.

## 2. What was built, against DR-S0's cells

On `dq-cabin-miss-build` at `bff54d524`, 3 rounds, 100,000 rows, binary B
alone, a heap whose primary key is not its Keystone. "Premise" is
`results-dr-s0-premise-v3.5.0-114-ge9df68d02.md` (M1, M4: a read by the
key and an UPDATE by the key, run at `e9df68d02`); the premise has no
delete or move-by-whole-key cell that was not a walk, and no price for the
move. Server CPU a statement, median [min-max over 3 rounds].

| form | read by whole key | UPDATE by whole key | UPDATE that moves the key | DELETE by whole key |
|---|---|---|---|---|
| int32 | 25.82 us [25.77-26.32] | 29.36 us [29.22-30.07] | 34.01 us [33.87-35.96] | 26.86 us [26.46-27.06] |
| int32x2 | 27.38 us [27.16-27.50] | 31.22 us [30.81-31.76] | 36.12 us [35.63-36.38] | 27.81 us [27.76-28.22] |
| varchar64 | 26.22 us [25.76-26.30] | 30.31 us [30.20-30.32] | 35.29 us [34.82-36.33] | 27.03 us [26.30-27.09] |
| mixed4 | 30.07 us [30.00-31.29] | 34.44 us [34.19-35.32] | 38.84 us [38.45-40.36] | 30.18 us [29.80-30.90] |
| ref_heap (declared Keystone) | 24.47 us [24.17-25.02] | 26.58 us [26.11-27.82] | refused (K2) | 23.81 us [23.42-23.91] |

Against the premise (same cells at `e9df68d02`):

| form | read, premise | read, B | UPDATE, premise | UPDATE, B | ratio | move, premise | move, B | ratio |
|---|---|---|---|---|---|---|---|---|
| int32 | 25.9 us | 25.82 us | 6,488.2 us | 29.36 us | 221x | 6,807.8 us | 34.01 us | 200x |
| int32x2 | 27.3 us | 27.38 us | 7,234.1 us | 31.22 us | 232x | 7,248.3 us | 36.12 us | 201x |
| varchar64 | 26.5 us | 26.22 us | 7,690.2 us | 30.31 us | 254x | 7,832.3 us | 35.29 us | 222x |
| mixed4 | 30.5 us | 30.07 us | 9,554.4 us | 34.44 us | 277x | 9,556.5 us | 38.84 us | 246x |
| ref_heap | 25.3 us | 24.47 us | 31.5 us | 26.58 us | 1.2x | refused | refused | - |

The ref_heap row is a control inside this table: an UPDATE by the Keystone
was 31.5 us in the premise run and is 26.58 us here with the same arm's
three rounds inside 26.11 us to 27.82 us, so two runs of the same cell, a
commit apart, differ by 4.9 us (16 %); the ratios in the
table carry that much between-run spread and the 200x to 277x are not more
precise than it.

**The price of a key move over a plain UPDATE by the same key** (move -
update, server CPU a statement; DR-S0 could not measure it, both being
walks):

| form | UPDATE by whole key | move | move - update |
|---|---|---|---|
| int32 | 29.36 us | 34.01 us | 4.65 us |
| int32x2 | 31.22 us | 36.12 us | 4.90 us |
| varchar64 | 30.31 us | 35.29 us | 4.98 us |
| mixed4 | 34.44 us | 38.84 us | 4.40 us |

The move is one index entry taken out and one put in beside the row's
rewrite, 4.40 us to 4.98 us; the premise's per-row insert price of a primary
key's index was 4.5 us to 5.8 us (M2 in the premise file), the same
magnitude. Latency, median over 3 rounds, `us`:

| form | phase | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| int32 | read | 20,000 | 38.4 us | 41.3 us | 42.3 us | 48.2 us | 59.8 us |
| int32 | update | 4,000 | 39.1 us | 42.6 us | 43.9 us | 54.0 us | 78.3 us |
| int32 | move | 4,000 | 42.1 us | 47.1 us | 48.6 us | 60.1 us | 84.6 us |
| int32 | delete | 4,000 | 36.3 us | 39.9 us | 41.2 us | 51.8 us | 71.3 us |
| int32x2 | read | 20,000 | 39.9 us | 42.8 us | 43.7 us | 50.4 us | 59.6 us |
| int32x2 | update | 4,000 | 40.9 us | 44.5 us | 45.6 us | 56.2 us | 76.3 us |
| int32x2 | move | 4,000 | 44.0 us | 48.7 us | 50.2 us | 62.6 us | 88.2 us |
| int32x2 | delete | 4,000 | 38.0 us | 41.1 us | 42.2 us | 51.7 us | 70.7 us |
| varchar64 | read | 20,000 | 38.6 us | 41.4 us | 42.2 us | 50.3 us | 58.4 us |
| varchar64 | update | 4,000 | 40.2 us | 43.2 us | 44.4 us | 56.2 us | 83.3 us |
| varchar64 | move | 4,000 | 43.7 us | 48.1 us | 49.5 us | 65.5 us | 93.5 us |
| varchar64 | delete | 4,000 | 37.2 us | 40.2 us | 41.2 us | 51.1 us | 70.5 us |
| mixed4 | read | 20,000 | 42.9 us | 45.7 us | 46.4 us | 52.6 us | 64.2 us |
| mixed4 | update | 4,000 | 44.8 us | 47.8 us | 48.9 us | 59.2 us | 86.0 us |
| mixed4 | move | 4,000 | 48.0 us | 51.7 us | 53.0 us | 67.3 us | 98.8 us |
| mixed4 | delete | 4,000 | 40.7 us | 43.9 us | 44.8 us | 54.5 us | 76.4 us |
| ref_heap | read | 20,000 | 36.9 us | 40.3 us | 41.0 us | 48.1 us | 57.0 us |
| ref_heap | update | 4,000 | 38.0 us | 40.6 us | 41.5 us | 49.4 us | 71.6 us |
| ref_heap | delete | 4,000 | 35.2 us | 37.6 us | 38.5 us | 47.9 us | 63.6 us |

The write now ends where the read does: a heap's UPDATE by primary key
costs 3.54 us to 4.37 us more than the read by the same key (29.36 - 25.82
on int32, 34.44 - 30.07 on mixed4, 30.31 - 26.22 on varchar64, 31.22 -
27.38 on int32x2), against 2.11 us (26.58 - 24.47) on the Keystone-declared
heap. The premise's central finding, a write that walked and a read that did
not, no longer holds for the whole-key forms measured here.

## 3. The btree forms at 100,000 rows

Rows arrive shuffled. F0 is `(id int64, v, w)` with named keys, the
relation as before; F1 is `(a int32, b int16, v, w, PRIMARY KEY (a, b))`
(embedded: the key value is the Keystone id); P1int is
`(a int32, b int32, v, w, PRIMARY KEY (a, b))` and P1str is
`(s varchar(64), v, w, PRIMARY KEY (s))`, each a unique index over a
hidden Keystone. 3 rounds, binary B. Server CPU, median [min-max]; leaf
divisions and data file are the median of the three rounds with the rounds
beside them.

| form | load | read | range of 100 | UPDATE | DELETE | leaf divisions | data file |
|---|---|---|---|---|---|---|---|
| F0 | 4.13 us/row [4.09-4.18] | 23.20 us [22.91-23.54] | 46.34 us [46.32-46.53] | 25.26 us [24.95-25.27] | 22.36 us [22.19-23.05] | 879 (rounds 894, 879, 834) | 8,912,896 B (8,912,896; 8,912,896; 8,388,608) |
| F1 | 4.62 us/row [4.54-4.64] | 25.11 us [24.77-25.33] | 51.24 us [50.74-52.58] | 27.11 us [26.95-28.03] | 24.49 us [24.39-24.81] | 978 (978, 979, 959) | 9,437,184 B (all rounds) |
| P1int | 8.45 us/row [8.37-8.54] | 28.67 us [28.66-29.11] | 135.34 us [135.32-138.02] | 31.18 us [30.58-31.47] | 28.04 us [27.81-29.03] | 0 (all rounds) | 9,437,184 B (all rounds) |
| P1str | 8.27 us/row [8.26-8.41] | 26.51 us [26.41-26.62] | 134.11 us [133.12-134.17] | 28.98 us [28.20-30.19] | 25.60 us [25.38-25.67] | 0 (all rounds) | 18,874,368 B (all rounds) |

Plans (the driver's `ANALYZE` read): F0 and F1 read by `Lookup`
(`pages=1`); F0 range `Range examined=190 matched=100 pages=3`, F1 range
`Range examined=202 matched=100 pages=3`; P1int and P1str read by
`IndexProbe` (`pages=2`) and range by `IndexRange` (`examined=100
pages=101`).

**Leaf divisions of P1int and P1str are 0 because of what the counter
counts, not because the trees did not grow.** Their hidden-Keystone ids
are issued in arrival order, and a right-edge append is not counted as a
leaf division (the same reading as the premise file's P1 arm); F0 and F1
take their keys in shuffled order, so their splits fall in the interior and
are counted. The 0 is therefore not comparable with F0's and F1's counts;
the data file column (9,437,184 B for P1int, 18,874,368 B for P1str) is the
size comparison.

F1 against F0 (the embedded key's price) and against P1int (what the same
key as a unique index costs):

| statement | F1 - F0 | F1 vs F0 | F1 - P1int | F1 vs P1int |
|---|---|---|---|---|
| load | +0.49 us/row | +11.9 % | -3.83 us/row | -45.3 % |
| point read | +1.91 us | +8.2 % | -3.56 us | -12.4 % |
| range of 100 | +4.90 us | +10.6 % | -84.10 us | -62.1 % |
| UPDATE | +1.85 us | +7.3 % | -4.07 us | -13.1 % |
| DELETE | +2.13 us | +9.5 % | -3.55 us | -12.7 % |
| leaf divisions (median of 3 rounds) | +99 | +11.3 % | n/a | n/a |
| data file | +524,288 B | +5.9 % | 0 B | 0 % |

F1's row is wider than F0's (two key columns, 4 B + 2 B, beside one
`int64`) and its tree divides more: 978 leaf divisions against 879 and a
file 524,288 B (64 pages) larger. The F1 minus F0 differences are 0.49 us
to 4.90 us; F0's own three-round ranges are 0.09 us to 0.86 us wide (load
0.09 us/row, read 0.63 us, range 0.21 us, update 0.32 us, delete 0.86 us),
so every difference exceeds its range, with 3 rounds as the sample. F1
against P1int is the engine's answer to "embedded or indexed": the embedded
key reads by one page where the indexed one reads two, ranges by three pages
where the indexed one reads 101, and writes without a second tree.

Latency, median over 3 rounds, `us`:

| form | phase | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| F0 | read | 20,000 | 36.5 us | 38.7 us | 39.4 us | 44.9 us | 54.6 us |
| F0 | range | 1,000 | 68.5 us | 73.2 us | 75.3 us | 91.7 us | 108.4 us |
| F0 | update | 4,000 | 36.8 us | 39.1 us | 39.8 us | 47.3 us | 65.7 us |
| F0 | delete | 4,000 | 34.1 us | 36.4 us | 37.1 us | 44.0 us | 60.0 us |
| F1 | read | 20,000 | 37.4 us | 40.9 us | 41.6 us | 47.5 us | 56.4 us |
| F1 | range | 1,000 | 72.1 us | 77.5 us | 79.9 us | 100.6 us | 128.5 us |
| F1 | update | 4,000 | 38.5 us | 41.0 us | 41.9 us | 52.7 us | 72.0 us |
| F1 | delete | 4,000 | 36.3 us | 38.6 us | 39.4 us | 47.3 us | 66.0 us |
| P1int | read | 20,000 | 40.4 us | 43.8 us | 45.1 us | 51.4 us | 62.5 us |
| P1int | range | 1,000 | 149.7 us | 158.6 us | 162.3 us | 211.3 us | 301.6 us |
| P1int | update | 4,000 | 40.4 us | 44.0 us | 45.7 us | 57.5 us | 81.2 us |
| P1int | delete | 4,000 | 37.8 us | 41.3 us | 42.7 us | 52.5 us | 71.7 us |
| P1str | read | 20,000 | 39.2 us | 42.0 us | 42.8 us | 48.3 us | 57.1 us |
| P1str | range | 1,000 | 141.6 us | 157.6 us | 161.5 us | 199.9 us | 294.0 us |
| P1str | update | 4,000 | 39.6 us | 42.2 us | 43.1 us | 52.3 us | 81.6 us |
| P1str | delete | 4,000 | 36.4 us | 39.2 us | 40.2 us | 49.0 us | 71.2 us |

## 4. Wait breakdown

Does not apply. One session on one connection, `cores = 1`, `relaxed`
durability (no commit fsync), pages resident in a pool of 131,072 frames
(no read waits), no concurrent writer (no lock wait): every figure above
is server CPU a statement, and the client latency is that CPU plus the
socket round trip (about 14 us). A decomposition into durability, read,
lock and conflict waits would assign shares to waits that this run does
not have. Baseline: this engine's own previous number is the premise file
(`results-dr-s0-premise-v3.5.0-114-ge9df68d02.md`) for section 2; section
1's baseline is the opening commit's binary measured inside this run. The
PostgreSQL floor was not measured for this shape.

## 5. What is unverified

| not measured | what would check it |
|---|---|
| instructions per statement (no hardware counters on this host: `perf` is absent), so a B - A in time is not split into instruction count and code layout | a host with `perf stat` or a counter-reading harness over the same cells |
| the layout spread of the opening commit's own source (no third arm built from `e9df68d02` with a perturbed layout); the between-builds swing of section 1a is taken on B's source only | rebuild `e9df68d02` with a changed link order or padding and run it as A3 beside A and A2 |
| `cores > 1`: a statement's descent by a unique key on another core, the lock units of a keyed write under two cores | the `ab` and `built` parts at `cores = 2` and above |
| a relation larger than the pool (a read by key that misses; the second tree's pages competing for frames) | `--frames` below the working set, with the page reader's waits named |
| 1,000,000 rows (the per-row slope: this run is one cardinality, 100,000, so a fixed cost and a per-row one are not separated; the 200 and 10,000 row sweep of the benchmark rules was not run) | `--rows 1000000`, `--rows 200` and `--rows 10000` |
| concurrent writers on one key (a keyed write's wait, deadlock and the unique key's lock unit) | a multi-session driver; none exists for this milestone |
| the control's width: five rounds, one pairing per round | more rounds of the `ab` part, or a repeat of the control arm |
| F1's `leaf_divisions` count and the sort of a mixed-width key at a different shuffle | a different seed's rounds |
