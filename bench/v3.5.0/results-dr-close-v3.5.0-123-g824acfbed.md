# DR close - what the hashed home (F2) costs and buys a btree's wide primary key, and what it left to the statements that did not ask for it

> The close of DR is `bench/v3.5.0/results-dr-close-v3.5.0-124-ge0d86ec62.md` (at `e0d86ec62`); this file stands as the record of the hashed form (F2) against the indexed form (its section 2, F2 against P1) and of the run that found the loads outside the control.

Measured 2026-11-23 between 09:08 and 09:14 UTC on worktree
`dq-cabin-miss-build`, branch `worktree-dr-primary-key`, at `824acfbed`
(`v3.5.0-123-g824acfbed`), the commit DR closes at. Order:
`instructions/v3.2.0/workorder-dr-primary-key-over-keystone.md` §5a, §7
("DR-S3f, the hashed home" and "The landing of DR-S3f open, and its
review"); the form's contract is `docs/spec/heap-and-tuple.md` §3.1c. Driver
`tools/dr_close.py` (documented in its docstring), unchanged, raw output in
`bench/v3.5.0/archive/dr-close-v3.5.0-123-g824acfbed/`. The premise the
milestone was opened against is
`bench/v3.5.0/results-dr-s0-premise-v3.5.0-114-ge9df68d02.md`; the first
close, at `bff54d524`, whose wide key was the indexed form (P1), is
`bench/v3.5.0/results-dr-close-v3.5.0-120-gbff54d524.md` and is cited here
for the between-builds floor (its section 1a) and for its figures of the
cells whose path did not change.

| stamp | value |
|---|---|
| Executed (UTC) | run 1 `ab`: 2026-11-23 09:08:30 to 09:10:13 (102.9 s); run 2 `built` on B: 09:10:17 to 09:12:08 (110.9 s); run 3 `built` on the P1 binary: 09:12:13 to 09:14:04 (110.8 s). Runs 2 and 3 are sequential, not interleaved |
| Worktree / branch | `dq-cabin-miss-build` / `worktree-dr-primary-key` |
| Commit measured (B) | `824acfbed`, `v3.5.0-123-g824acfbed`, committed 2026-11-23 09:07:18 UTC. `git status` was clean at the start of runs 1 and 2 and at the start of run 3. At 09:13:52 UTC, during run 3, another session modified `src/catalog/catalog.cpp` in this worktree (a source edit; no build ran until 09:15:31, after run 3 ended at 09:14:04, and every server of the three runs started from a copy of a binary that existed before run 1), so the measured binaries are unaffected and the tree is `824acfbed` plus that uncommitted edit as this file is written |
| Binary B (F2) | copy `/home/louis/dr-bench/close/kds_server_B`, sha256 `2872c552bc049881...` (first 16, hashed by the driver in runs 1 and 2); source `build-release/kds_server` mtime 2026-11-23 09:07:47 UTC, after the commit's 09:07:18, so the binary is not older than HEAD |
| Binary P1 (the indexed form) | copy `/home/louis/dr-bench/close/kds_server_P1_bff54d524`, sha256 `5d7524c6a3f2846d...` (first 16, hashed by the driver in run 3), mtime 2026-11-23 08:21:29 UTC; built from `bff54d524`, the closing binary of the first close |
| Binary A and A2 (opening) | one file, copy `/home/louis/dr-bench/s0/kds_server_e9df68d0`, sha256 `e33a55473f81fade...`, mtime 2026-11-23 04:44:06 UTC, commit `e9df68d02` (`v3.5.0-114-ge9df68d02`); A2 is a second server started from a second copy of the same file (the control) |
| Build type | Release (`build-release`), nothing rebuilt for this run |
| Device | `/dev/nvme0n1p1`, ext4, mounted `/` (`df -T` run at 09:08 UTC); data files under `/home/louis/dr-bench/close/w`, not tmpfs |
| Server configuration | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, one fresh server and data file per cell; server pinned to host CPU 2, driver to CPUs 0 and 4; defaults otherwise |
| Shape | 100,000 rows; reads 20,000, writes 4,000, walked 40, ranges 1,000 statements a phase; 20 batches a phase; commands exactly as the standing brief gives them (`--rows`, `--reads`, `--writes`, `--walks`, `--ranges` at their defaults) |
| Rounds | run 1: 5 rounds, three arms a round (order rotated), 15 cells; runs 2 and 3: 3 rounds, 27 cells each |
| Machine | quiet-gate hold 0.0 s in all 69 cells; `cc1plus` count 0 at every cell's start and end; host load average (1 min) 1.12 to 1.38 across run 1, 1.00 to 1.41 across run 2 and 0.63 to 1.17 across run 3, the driver's own server and client included (the 5- and 15-minute averages were 4.10 and 3.98 at 09:08, left by earlier builds on the host); an idle `build-sandbox` `kds_server` and an unrelated database process of another user were resident throughout |

Server CPU a statement is the median of the phase's 20 batches; a table
cell reads "median over rounds [min-max over rounds]" unless a delta is
shown. Latency percentiles are the median over rounds of each percentile.
Rates marked "derived" are `1,000,000 / us` of the CPU figure; the driver
reports CPU per statement. Wait breakdown: **does not apply** (section 5).

## The answer first

**"No other path affected" (section 1).** On `dq-cabin-miss-build` at
`824acfbed`, run 1's B - A lies inside the control's range in **11 of the 14
phases and outside it in 3**: btree `load` (B - A +0.18 us/row, +5.1 %, 0.09
us/row above the control's upper end of +0.08), heap `load` (+0.13 us/row,
+1.8 %, 0.03 us/row above +0.11) and btree `walked` (-295.31 us, -4.2 %,
267.83 us below the control's lower end of -27.48, B faster, all five of B's
rounds below all five of A's). The `walked` phase is the one path DR added
an instruction to. From runs 2 and 3, the cells that are not a wide key
differ between the B and the P1 binary by -0.82 us to +0.44 us on a point read, -0.49 us to
+0.20 us on an update, -0.27 us to +0.29 us on a delete, -0.44 us to -0.11 us on a key
move, and -0.00 to +0.13 us/row on a load (section 1b): that is what two
sequential runs of one path differ by on this host, and the F2 deltas below
are read against it.

**What F2 costs and buys against P1 (section 2).** A `Wint` row costs
9.81 us/row to load against 8.35 us/row (+1.46 us/row, **+17.5 %**) and a
`Wstr` row 9.87 us/row against 8.31 us/row (+1.56 us/row, **+18.7 %**); the
operator's bar for the insert is +50 % (`raft-marks-2026-11-23.md` §16).
A point read by the whole key costs 25.85 us against 28.63 us (-2.78 us,
-9.7 %) on `Wint` and 24.51 us against 26.09 us (-1.58 us, -6.1 %) on
`Wstr`, one page where P1 read two. A range of 100 rows, an update and a
delete by the whole key are within the floor of section 1b except the
`Wint` update, 0.21 us beyond its lower end (range -0.5 % and +0.4 %,
update -2.3 % and +0.0 %, delete -0.8 % and -0.2 %). The tree divides 1,032 (`Wint`) and 1,993 (`Wstr`)
times where P1 counted 0, and the data file is 27.8 % larger (12,058,624 B
against 9,437,184 B; 24,117,248 B against 18,874,368 B).

## 1. "No other path affected"

### 1a. Run 1: the unchanged relation, A against A2 against B

Server CPU, `us` a statement (`us/row` for load), 100,000 rows, 5 rounds.
Control delta = A2 - A (medians). "Control range" is the lowest and highest
per-round A2 - A of the five rounds (the spread two runs of one engine
showed); "inside" means the median B - A lies between them. Percent is of
the A median. The last column is the previous close's B - A for the same
phase (`bench/v3.5.0/results-dr-close-v3.5.0-120-gbff54d524.md`, section
1), a different B binary against its own A.

btree:

| phase | A | A2 | B | A2 - A (control) | control range | B - A | verdict | previous close's B - A |
|---|---|---|---|---|---|---|---|---|
| load | 3.45 us/row [3.45-3.48] | 3.46 us/row [3.40-3.56] | 3.63 us/row [3.58-3.71] | +0.00 us/row (+0.1 %) | -0.07 to +0.08 us/row | +0.18 us/row (+5.1 %) | outside by +0.09 us/row | +0.14 us/row |
| read | 22.46 us [22.30-22.69] | 22.67 us [22.40-23.09] | 22.75 us [22.50-23.31] | +0.21 us (+0.9 %) | -0.25 to +0.48 us | +0.29 us (+1.3 %) | inside | +0.42 us |
| update | 24.88 us [24.82-25.22] | 24.98 us [24.80-25.23] | 24.90 us [24.74-25.29] | +0.10 us (+0.4 %) | -0.17 to +0.19 us | +0.02 us (+0.1 %) | inside | +0.16 us |
| walked | 7,004.70 us [6,970.01-7,040.80] | 7,042.70 us [7,012.16-7,272.15] | 6,709.39 us [6,661.61-6,731.86] | +38.00 us (+0.5 %) | -27.48 to +267.45 us | -295.31 us (-4.2 %) | outside by -267.83 us | +1.79 us |
| range | 45.91 us [45.23-46.61] | 46.40 us [45.70-47.92] | 46.17 us [45.35-47.26] | +0.49 us (+1.1 %) | -0.21 to +1.32 us | +0.26 us (+0.6 %) | inside | -0.19 us |
| insert | 22.48 us [22.32-22.98] | 22.57 us [22.32-22.85] | 22.67 us [22.49-22.94] | +0.09 us (+0.4 %) | -0.41 to +0.34 us | +0.19 us (+0.8 %) | inside | +0.03 us |
| delete | 22.36 us [21.90-22.49] | 22.07 us [22.04-22.98] | 22.24 us [21.94-22.51] | -0.29 us (-1.3 %) | -0.36 to +0.49 us | -0.12 us (-0.5 %) | inside | +0.36 us |

heap:

| phase | A | A2 | B | A2 - A (control) | control range | B - A | verdict | previous close's B - A |
|---|---|---|---|---|---|---|---|---|
| load | 7.48 us/row [7.45-7.63] | 7.46 us/row [7.43-7.59] | 7.62 us/row [7.55-7.70] | -0.02 us/row (-0.3 %) | -0.17 to +0.11 us/row | +0.13 us/row (+1.8 %) | outside by +0.03 us/row | +0.17 us/row |
| read | 23.97 us [23.61-24.66] | 23.98 us [23.84-24.46] | 24.01 us [23.77-24.08] | +0.01 us (+0.0 %) | -0.82 to +0.49 us | +0.04 us (+0.2 %) | inside | +0.29 us |
| update | 27.26 us [26.45-27.44] | 26.48 us [26.12-27.67] | 26.66 us [26.54-27.52] | -0.78 us (-2.9 %) | -1.18 to +1.04 us | -0.60 us (-2.2 %) | inside | +0.16 us |
| walked | 6,803.10 us [6,785.06-6,951.92] | 6,928.44 us [6,751.89-7,057.34] | 6,767.44 us [6,729.70-6,862.63] | +125.34 us (+1.8 %) | -162.49 to +272.28 us | -35.66 us (-0.5 %) | inside | -78.41 us |
| range | 71.81 us [69.20-74.06] | 72.91 us [69.46-74.14] | 71.86 us [70.76-72.51] | +1.10 us (+1.5 %) | -4.60 to +4.94 us | +0.05 us (+0.1 %) | inside | -0.31 us |
| insert | 27.05 us [26.75-27.53] | 27.41 us [27.00-27.84] | 27.61 us [26.79-27.75] | +0.36 us (+1.3 %) | -0.53 to +0.96 us | +0.56 us (+2.1 %) | inside | +0.39 us |
| delete | 23.16 us [22.82-24.02] | 23.51 us [23.03-23.79] | 23.40 us [22.94-23.71] | +0.35 us (+1.5 %) | -0.99 to +0.69 us | +0.24 us (+1.0 %) | inside | +0.03 us |

**The `walked` phase** (`UPDATE t SET w = w + 1 WHERE v = k`, no key: the
one statement path DR added an instruction to, the question whether a
unique index names the row), 100,000 rows walked a statement:

| relation | A | A2 | B | B - A | control range | verdict |
|---|---|---|---|---|---|---|
| btree | 7,004.70 us | 7,042.70 us | 6,709.39 us | -295.31 us (-4.2 %), -2.95 ns a row walked | -27.48 to +267.45 us | outside by -267.83 us (B below) |
| heap | 6,803.10 us | 6,928.44 us | 6,767.44 us | -35.66 us (-0.5 %), -0.36 ns a row walked | -162.49 to +272.28 us | inside |

Reading the table. The three phases outside are `load` on both relations
and the btree `walked`; the other eleven are inside. B - A is positive on
both loads: B costs more on the bulk insert, by +0.18 us/row on the
btree and +0.13 us/row on the heap, while the single-row `insert` phases
(+0.19 us and +0.56 us) are inside. The control is five rounds wide, and on
loads it is narrow here (-0.07 to +0.08 and -0.17 to +0.11 us/row), narrower
than in the previous close (-0.04 to +0.15 and -0.25 to +0.22 us/row), so
the same B - A level is outside now where it was inside then: the previous
close's B - A on those loads was +0.14 and +0.17 us/row. The btree `walked`
has a sign opposite to a cost: B is faster by 295.31 us. The previous
close's section 1a measured the same phase moving by 366.19 us between
builds that differ by no source on its path, so a move of this size in
either direction on `walked` is the size a link or layout change produced
there on this host; this run did not build a third arm from the opening
source and cannot say which it is. The control is five paired rounds and
bounds what two runs of one binary showed here; it is not a confidence
interval.

Latency (client), median over rounds of each percentile. The client
latency carries the socket round trip (about 14 us to 15 us a statement over
the server CPU), so the p0 of a point statement is the fixed cost.

| relation | phase | arm | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| btree | read | A | 20,000 | 36.0 us | 38.3 us | 38.8 us | 45.1 us | 53.8 us |
| btree | read | A2 | 20,000 | 36.3 us | 38.4 us | 39.1 us | 45.3 us | 54.5 us |
| btree | read | B | 20,000 | 36.2 us | 38.5 us | 39.1 us | 46.5 us | 54.0 us |
| btree | update | A | 4,000 | 36.4 us | 38.6 us | 39.3 us | 48.6 us | 63.6 us |
| btree | update | A2 | 4,000 | 36.6 us | 38.8 us | 39.5 us | 48.2 us | 63.8 us |
| btree | update | B | 4,000 | 36.5 us | 38.7 us | 39.5 us | 48.5 us | 65.8 us |
| btree | walked | A | 40 | 6,910.0 us | 7,018.1 us | 7,063.4 us | 7,349.6 us | 7,543.1 us |
| btree | walked | A2 | 40 | 6,980.9 us | 7,044.7 us | 7,113.0 us | 7,494.3 us | 7,555.4 us |
| btree | walked | B | 40 | 6,617.8 us | 6,734.1 us | 6,781.7 us | 7,342.9 us | 7,946.2 us |
| btree | range | A | 1,000 | 68.7 us | 72.6 us | 74.9 us | 95.4 us | 116.6 us |
| btree | range | A2 | 1,000 | 68.9 us | 72.6 us | 75.2 us | 93.9 us | 121.6 us |
| btree | range | B | 1,000 | 68.8 us | 72.4 us | 75.0 us | 92.7 us | 120.9 us |
| btree | insert | A | 4,000 | 34.4 us | 36.4 us | 37.0 us | 44.7 us | 61.5 us |
| btree | insert | A2 | 4,000 | 34.4 us | 36.3 us | 37.0 us | 45.5 us | 61.8 us |
| btree | insert | B | 4,000 | 34.7 us | 36.6 us | 37.2 us | 45.0 us | 61.6 us |
| btree | delete | A | 4,000 | 34.0 us | 36.3 us | 36.9 us | 45.0 us | 60.7 us |
| btree | delete | A2 | 4,000 | 34.0 us | 36.3 us | 36.9 us | 44.3 us | 59.5 us |
| btree | delete | B | 4,000 | 34.1 us | 36.3 us | 37.0 us | 44.0 us | 59.2 us |
| heap | read | A | 20,000 | 36.9 us | 39.5 us | 40.3 us | 46.8 us | 56.7 us |
| heap | read | A2 | 20,000 | 34.9 us | 39.5 us | 40.2 us | 46.6 us | 56.5 us |
| heap | read | B | 20,000 | 36.3 us | 39.6 us | 40.3 us | 46.8 us | 56.5 us |
| heap | update | A | 4,000 | 37.2 us | 39.8 us | 40.9 us | 52.0 us | 75.2 us |
| heap | update | A2 | 4,000 | 37.4 us | 39.8 us | 40.6 us | 52.0 us | 74.6 us |
| heap | update | B | 4,000 | 37.3 us | 40.0 us | 40.9 us | 51.5 us | 74.9 us |
| heap | walked | A | 40 | 6,704.7 us | 6,822.1 us | 6,875.7 us | 7,235.5 us | 7,647.8 us |
| heap | walked | A2 | 40 | 6,696.4 us | 6,898.3 us | 6,978.5 us | 7,345.8 us | 7,559.6 us |
| heap | walked | B | 40 | 6,678.9 us | 6,775.1 us | 6,819.8 us | 7,081.1 us | 7,135.0 us |
| heap | range | A | 1,000 | 93.4 us | 96.8 us | 99.1 us | 120.9 us | 178.6 us |
| heap | range | A2 | 1,000 | 93.5 us | 97.0 us | 99.3 us | 133.7 us | 162.9 us |
| heap | range | B | 1,000 | 94.0 us | 97.6 us | 99.2 us | 126.3 us | 193.8 us |
| heap | insert | A | 4,000 | 38.0 us | 40.5 us | 41.3 us | 51.5 us | 76.4 us |
| heap | insert | A2 | 4,000 | 38.0 us | 40.3 us | 41.3 us | 54.1 us | 78.4 us |
| heap | insert | B | 4,000 | 38.6 us | 40.8 us | 41.5 us | 53.0 us | 75.9 us |
| heap | delete | A | 4,000 | 34.7 us | 37.0 us | 37.8 us | 47.4 us | 64.9 us |
| heap | delete | A2 | 4,000 | 33.2 us | 37.1 us | 38.0 us | 47.1 us | 61.9 us |
| heap | delete | B | 4,000 | 34.5 us | 37.2 us | 38.0 us | 46.0 us | 64.1 us |

### 1b. Runs 2 and 3: the cells that are not a wide key, B against the P1 binary

Runs 2 (B) and 3 (the P1 binary) are **sequential, not interleaved**: run 2
ran 09:10 to 09:12 and run 3 09:12 to 09:14, so a drift of the host between
them is in every difference below. The cells whose code path is the same in
both binaries (every heap form, `ref_heap`, F0 and F1) serve as the
between-run control for the F2 against P1 comparison in section 2: run 3
minus run 2 for them is what two runs of one path differ by. The last
column is run 2 against the previous close's figure for the same cell
(`bench/v3.5.0/results-dr-close-v3.5.0-120-gbff54d524.md`, sections 2 and 3,
whose B binary was the P1 binary measured here). Server CPU, median
[min-max over 3 rounds]; `move` is the heap forms' UPDATE that moves the
key.

| form | phase | previous close (`bff54d524`) | run 2: B | run 3: P1 binary | run 3 - run 2 | run 2 - previous close |
|---|---|---|---|---|---|---|
| ref_heap | load | 7.71 us/row | 7.57 us/row [7.54-7.58] | 7.60 us/row [7.55-7.61] | +0.04 us/row (+0.5 %) | -0.15 us/row (-1.9 %) |
| ref_heap | read | 24.47 us | 23.92 us [23.68-24.03] | 23.90 us [23.87-24.14] | -0.02 us (-0.1 %) | -0.55 us (-2.2 %) |
| ref_heap | update | 26.58 us | 26.05 us [26.02-26.12] | 26.10 us [26.06-26.21] | +0.05 us (+0.2 %) | -0.53 us (-2.0 %) |
| ref_heap | delete | 23.81 us | 23.20 us [23.00-23.36] | 23.32 us [23.16-23.35] | +0.12 us (+0.5 %) | -0.61 us (-2.6 %) |
| int32 | load | 11.75 us/row | 11.68 us/row [11.67-11.94] | 11.76 us/row [11.67-11.79] | +0.08 us/row (+0.7 %) | -0.07 us/row (-0.6 %) |
| int32 | read | 25.82 us | 25.61 us [25.52-25.79] | 25.99 us [25.56-26.69] | +0.38 us (+1.5 %) | -0.21 us (-0.8 %) |
| int32 | update | 29.36 us | 29.49 us [28.74-29.74] | 29.02 us [28.74-30.08] | -0.47 us (-1.6 %) | +0.13 us (+0.4 %) |
| int32 | move | 34.01 us | 33.98 us [33.25-34.14] | 33.87 us [33.82-35.70] | -0.11 us (-0.3 %) | -0.03 us (-0.1 %) |
| int32 | delete | 26.86 us | 25.96 us [25.80-26.04] | 26.13 us [25.82-26.76] | +0.17 us (+0.7 %) | -0.90 us (-3.4 %) |
| int32x2 | load | 11.88 us/row | 11.78 us/row [11.77-12.24] | 11.88 us/row [11.82-11.95] | +0.09 us/row (+0.8 %) | -0.09 us/row (-0.8 %) |
| int32x2 | read | 27.38 us | 27.84 us [27.33-28.39] | 27.02 us [26.86-27.06] | -0.82 us (-2.9 %) | +0.46 us (+1.7 %) |
| int32x2 | update | 31.22 us | 31.06 us [30.80-32.80] | 30.65 us [30.52-31.27] | -0.41 us (-1.3 %) | -0.16 us (-0.5 %) |
| int32x2 | move | 36.12 us | 35.86 us [35.48-36.66] | 35.44 us [35.35-36.86] | -0.42 us (-1.2 %) | -0.26 us (-0.7 %) |
| int32x2 | delete | 27.81 us | 27.16 us [26.96-28.92] | 27.45 us [27.31-28.11] | +0.29 us (+1.1 %) | -0.65 us (-2.3 %) |
| varchar64 | load | 12.62 us/row | 12.20 us/row [12.15-12.23] | 12.34 us/row [12.32-12.73] | +0.13 us/row (+1.1 %) | -0.42 us/row (-3.3 %) |
| varchar64 | read | 26.22 us | 25.30 us [25.30-25.30] | 25.38 us [25.24-25.46] | +0.08 us (+0.3 %) | -0.92 us (-3.5 %) |
| varchar64 | update | 30.31 us | 29.62 us [29.34-29.70] | 29.39 us [29.31-29.57] | -0.23 us (-0.8 %) | -0.69 us (-2.3 %) |
| varchar64 | move | 35.29 us | 35.00 us [34.49-35.14] | 34.67 us [34.46-34.82] | -0.33 us (-0.9 %) | -0.29 us (-0.8 %) |
| varchar64 | delete | 27.03 us | 26.36 us [25.89-26.52] | 26.15 us [25.85-26.15] | -0.21 us (-0.8 %) | -0.67 us (-2.5 %) |
| mixed4 | load | 13.08 us/row | 13.01 us/row [12.94-13.06] | 13.13 us/row [12.99-13.41] | +0.12 us/row (+0.9 %) | -0.08 us/row (-0.6 %) |
| mixed4 | read | 30.07 us | 29.68 us [29.67-30.42] | 30.12 us [29.79-30.89] | +0.44 us (+1.5 %) | -0.39 us (-1.3 %) |
| mixed4 | update | 34.44 us | 34.23 us [33.62-34.41] | 33.96 us [33.48-34.34] | -0.27 us (-0.8 %) | -0.21 us (-0.6 %) |
| mixed4 | move | 38.84 us | 38.99 us [38.47-39.46] | 38.55 us [38.42-38.64] | -0.44 us (-1.1 %) | +0.15 us (+0.4 %) |
| mixed4 | delete | 30.18 us | 30.18 us [29.63-30.41] | 29.91 us [29.84-30.17] | -0.27 us (-0.9 %) | +0.00 us (+0.0 %) |
| F0 | load | 4.13 us/row | 4.19 us/row [4.13-4.24] | 4.19 us/row [4.14-4.20] | -0.00 us/row (-0.1 %) | +0.06 us/row (+1.5 %) |
| F0 | read | 23.20 us | 22.67 us [22.54-22.94] | 23.09 us [22.96-23.35] | +0.42 us (+1.9 %) | -0.53 us (-2.3 %) |
| F0 | range | 46.34 us | 45.64 us [45.57-46.49] | 46.76 us [45.76-49.09] | +1.12 us (+2.5 %) | -0.70 us (-1.5 %) |
| F0 | update | 25.26 us | 24.80 us [24.66-25.16] | 25.00 us [24.97-25.84] | +0.20 us (+0.8 %) | -0.46 us (-1.8 %) |
| F0 | delete | 22.36 us | 22.26 us [22.00-22.51] | 22.55 us [22.46-22.65] | +0.29 us (+1.3 %) | -0.10 us (-0.4 %) |
| F1 | load | 4.62 us/row | 4.58 us/row [4.55-4.71] | 4.58 us/row [4.58-4.79] | +0.01 us/row (+0.2 %) | -0.05 us/row (-1.0 %) |
| F1 | read | 25.11 us | 24.53 us [24.44-25.30] | 24.88 us [24.71-24.98] | +0.35 us (+1.4 %) | -0.58 us (-2.3 %) |
| F1 | range | 51.24 us | 51.49 us [48.76-51.89] | 49.76 us [49.61-50.02] | -1.73 us (-3.4 %) | +0.25 us (+0.5 %) |
| F1 | update | 27.11 us | 27.21 us [26.55-27.24] | 26.72 us [26.45-27.05] | -0.49 us (-1.8 %) | +0.10 us (+0.4 %) |
| F1 | delete | 24.49 us | 23.95 us [23.78-24.27] | 24.18 us [24.17-24.51] | +0.23 us (+1.0 %) | -0.54 us (-2.2 %) |

The span of run 3 minus run 2 over those cells, by phase (the between-run
floor the F2 deltas are read against):

| phase | cells | run 3 - run 2, lowest | highest |
|---|---|---|---|
| load | 7 | -0.00 us/row (-0.1 %) | +0.13 us/row (+1.1 %) |
| read | 7 | -0.82 us (-2.9 %) | +0.44 us (+1.9 %) |
| range | 2 | -1.73 us (-3.4 %) | +1.12 us (+2.5 %) |
| update | 7 | -0.49 us (-1.8 %) | +0.20 us (+0.8 %) |
| move | 4 | -0.44 us (-1.2 %) | -0.11 us (-0.3 %) |
| delete | 7 | -0.27 us (-0.9 %) | +0.29 us (+1.3 %) |

State of the cells: of the 34 cells, the run 3 - run 2 delta is below zero
in 14, above zero in 19 and -0.00 us/row in one (the F0 load); all four key moves are
lower in run 3 (-0.44 to -0.11 us). The largest delta is -1.73 us (F1
range, -3.4 %, whose three rounds in run 2 span 48.76 us to 51.89 us); the
largest on a point read is -0.82 us (`int32x2`, -2.9 %). Run 2 against the
previous close's figures: the 34 cells sit between -0.92 us (-3.5 %, heap
`varchar64` read) and +0.46 us (+1.7 %, heap `int32x2` read) on statements,
between -0.42 and +0.06 us/row on loads. The previous close's binary and
the binary of runs 2 and 3's P1 arm are the same file.

## 2. What F2 costs and buys against P1

The forms (rows arrive shuffled, 100,000 rows, 3 rounds each):
`Wint` is `(a int32, b int32, v, w, PRIMARY KEY (a, b))` and `Wstr` is
`(s varchar(64), v, w, PRIMARY KEY (s))`. On B each is F2, the hashed home:
the unique index `$pkey_<oid>` over a hidden Keystone, and each row placed
at the id its key hashes to where that id is free. On the P1 binary each is
P1, the same index with the hidden Keystone issued in arrival order. The
statements are the driver's: `SELECT v` by the whole key, a range of 100
rows by the key, `UPDATE ... SET w = w + 1` and `DELETE` by the whole key.
"Load" is server CPU a row of the 500-row `INSERT`s that fill the relation.
The two columns are from different runs (run 2 for F2, run 3 for P1).

| form | statement | P1 binary (P1), run 3 | B (F2), run 2 | F2 - P1 | F2 vs P1 | derived rate, F2 vs P1 | P1 in the previous close (`bff54d524`) |
|---|---|---|---|---|---|---|---|
| Wint | load | 8.35 us/row [8.32-8.66] | 9.81 us/row [9.77-9.86] | +1.46 us/row | +17.5 % | 101,958 vs 119,832 rows/s | 8.45 us/row |
| Wint | read | 28.63 us [28.22-29.12] | 25.85 us [25.65-26.11] | -2.78 us | -9.7 % | 38,685 vs 34,928 stmt/s | 28.67 us |
| Wint | range | 133.78 us [131.88-136.41] | 133.15 us [130.21-134.61] | -0.63 us | -0.5 % | 7,510 vs 7,475 stmt/s | 135.34 us |
| Wint | update | 30.74 us [30.12-30.83] | 30.04 us [29.99-30.44] | -0.70 us | -2.3 % | 33,289 vs 32,531 stmt/s | 31.18 us |
| Wint | delete | 27.44 us [27.40-27.48] | 27.22 us [26.88-27.42] | -0.22 us | -0.8 % | 36,738 vs 36,443 stmt/s | 28.04 us |
| Wstr | load | 8.31 us/row [8.25-8.34] | 9.87 us/row [9.84-9.90] | +1.56 us/row | +18.7 % | 101,317 vs 120,294 rows/s | 8.27 us/row |
| Wstr | read | 26.09 us [26.04-26.16] | 24.51 us [23.92-24.73] | -1.58 us | -6.1 % | 40,800 vs 38,329 stmt/s | 26.51 us |
| Wstr | range | 131.88 us [131.06-132.24] | 132.44 us [130.44-137.13] | +0.56 us | +0.4 % | 7,551 vs 7,583 stmt/s | 134.11 us |
| Wstr | update | 28.12 us [28.11-28.25] | 28.12 us [27.84-28.40] | +0.00 us | +0.0 % | 35,562 vs 35,562 stmt/s | 28.98 us |
| Wstr | delete | 25.04 us [24.95-25.09] | 25.00 us [24.77-25.70] | -0.04 us | -0.2 % | 40,000 vs 39,936 stmt/s | 25.60 us |

**The insert price the operator's bar is read against: +17.5 % (`Wint`)
and +18.7 % (`Wstr`) a row, against a bar of +50 %**
(`raft-marks-2026-11-23.md` §16). In rows a second, derived, 101,958 against
119,832 (`Wint`) and 101,317 against 120,294 (`Wstr`). The per-round load of
F2 is 9.772, 9.855 and 9.808 us/row (`Wint`) and 9.870, 9.902 and 9.842
us/row (`Wstr`); P1's is 8.656, 8.319 and 8.345 us/row and 8.339, 8.247 and
8.313 us/row. The between-run floor of section 1b on a load is -0.00 to
+0.13 us/row; the F2 - P1 differences, +1.46 and +1.56 us/row, are 11 and
12 times its upper end. The `Wint` P1 arm's round 0 (8.656 us/row) is above
its other two rounds (8.319, 8.345) by 0.34 us/row; the previous close's
P1int load (8.45 us/row) is 0.10 us/row above this run's median.

**The read gains what the order said it would, and no more.** The stored
`read_step` is the counters line of the driver's `ANALYZE` of the first
read statement of each cell (`step_line` keeps the `step 0 ... opens=` line;
the `home=` figure is on the plan line above it, which the driver does not
store). Stored lines at 100,000 rows:

| cell | stored `read_step` (counters line) |
|---|---|
| `Wint`, B (F2) | `step 0 IndexProbe t opens=1 examined=1 matched=1 sel=100% pages=1` |
| `Wstr`, B (F2) | `step 0 IndexProbe t opens=1 examined=1 matched=1 sel=100% pages=1` |
| `Wint`, P1 binary | `step 0 IndexProbe t opens=1 examined=1 matched=1 sel=100% pages=2 index_scanned=1 index_resolved=1` |
| `Wstr`, P1 binary | `step 0 IndexProbe t opens=1 examined=1 matched=1 sel=100% pages=2 index_scanned=1 index_resolved=1` |

F2's read reads one page and no index entry (`pages=1`, no
`index_scanned`); P1's reads two pages and one index entry resolved. The
`home=` figure itself is on the plan line, which the driver does not keep,
so it was read by a separate `ANALYZE` after run 3 ended, on a 1,000-row
`Wint` and `Wstr` relation of the same DDL on each binary, not a timed
cell, for key `(1, 500)` and for the string `acct-0000001500-...`:

| binary | plan line printed by `ANALYZE` |
|---|---|
| B (F2), `Wint` | `step 0 IndexProbe t home=89057426660058` |
| B (F2), `Wstr` | `step 0 IndexProbe t home=109776560052172` |
| P1 binary, `Wint` | `step 0 IndexProbe t` |
| P1 binary, `Wstr` | `step 0 IndexProbe t` |

Both homes are below 2^47 (140,737,488,355,328); the P1 binary prints no
`home=`. The read is 2.78 us (`Wint`) and 1.58 us (`Wstr`) cheaper on F2,
against a floor on a point read of -0.82 to +0.44 us: both lie outside it.
The index tree is skipped, but the clustered descent to the home and the
checks on the row remain, so F2's read does not reach F0's `Lookup`:

| form | statement | F2 | F0 | F2 - F0 | F1 | F2 - F1 |
|---|---|---|---|---|---|---|
| Wint | load | 9.81 us/row | 4.19 us/row | +5.61 us/row (+133.9 %) | 4.58 us/row | +5.23 us/row (+114.3 %) |
| Wint | read | 25.85 us | 22.67 us | +3.18 us (+14.0 %) | 24.53 us | +1.32 us (+5.4 %) |
| Wint | range | 133.15 us | 45.64 us | +87.51 us (+191.7 %) | 51.49 us | +81.66 us (+158.6 %) |
| Wint | update | 30.04 us | 24.80 us | +5.24 us (+21.1 %) | 27.21 us | +2.83 us (+10.4 %) |
| Wint | delete | 27.22 us | 22.26 us | +4.96 us (+22.3 %) | 23.95 us | +3.27 us (+13.7 %) |
| Wstr | load | 9.87 us/row | 4.19 us/row | +5.68 us/row (+135.3 %) | 4.58 us/row | +5.29 us/row (+115.7 %) |
| Wstr | read | 24.51 us | 22.67 us | +1.84 us (+8.1 %) | 24.53 us | -0.02 us (-0.1 %) |
| Wstr | range | 132.44 us | 45.64 us | +86.80 us (+190.2 %) | 51.49 us | +80.95 us (+157.2 %) |
| Wstr | update | 28.12 us | 24.80 us | +3.32 us (+13.4 %) | 27.21 us | +0.91 us (+3.3 %) |
| Wstr | delete | 25.00 us | 22.26 us | +2.74 us (+12.3 %) | 23.95 us | +1.05 us (+4.4 %) |

`Wstr` reads at F1's figure (24.51 us against 24.53 us); `Wint` reads 1.32
us above F1 and 3.18 us above F0. Update and delete by the whole key are
the index's alone (`heap-and-tuple.md` §3.1c: every write goes through the
index), so F2 does not change them: F2 - P1 is -0.70 us and 0.00 us on
update and -0.22 us and -0.04 us on delete, against a floor of -0.49 to
+0.20 us and -0.27 to +0.29 us (the `Wint` update, -0.70 us, is 0.21 us
outside the floor's lower end; the other three differences are inside).
The range of 100 rows by the key is `IndexRange` on both binaries
(`examined=100 pages=101 index_scanned=100 index_resolved=100`), about 2.9x
F0's and 2.6x F1's: the home gives a range no locality, because it is the
hash of a whole key and not an order.

**What F2 costs the load, and what the cells can and cannot say about
why.** F2's insert does what P1's does (checks the unique index, appends
the entry) plus the hash of the key, the home's row unit (asked without
waiting), one descent to see whether the home is bound, and a row placed at
a random leaf in place of the right edge. The driver times the statement as
a whole and does not separate these, and no hardware counter exists on
this host, so the 1.46 us to 1.56 us a row is the sum of them. The load is
about 2.3 times F0's and 2.1 times F1's (9.81 us/row against 4.19 and
4.58), the index being what F2 has and F0 and F1 do not.

## 3. Leaf divisions, and what they count

P1 read 0 leaf divisions because the counter does not count a right-edge
append, and P1's hidden Keystone ids are issued in arrival order, so every
row appended at the right edge (the first close's section 3 says the same of
P1int and P1str). F2's homes are hashes spread over `[1, 2^47)`, so each row
lands at a random leaf of the clustered tree and a full leaf divides in the
interior, which the counter counts:

| form | binary | leaf divisions, median (rounds) | data file, median (rounds) | data file in pages (8,192 B) |
|---|---|---|---|---|
| F0 | B | 879 (894, 879, 834) | 8,912,896 B (8,912,896; 8,912,896; 8,388,608) | 1,088 |
| F1 | B | 978 (978, 979, 959) | 9,437,184 B (9,437,184; 9,437,184; 9,437,184) | 1,152 |
| Wint | B (F2) | 1,032 (1,007, 1,032, 1,043) | 12,058,624 B (12,058,624; 12,058,624; 12,582,912) | 1,472 |
| Wint | P1 binary (P1) | 0 (0, 0, 0) | 9,437,184 B (9,437,184; 9,437,184; 9,437,184) | 1,152 |
| Wstr | B (F2) | 1,993 (1,977, 1,993, 2,005) | 24,117,248 B (24,117,248; 24,117,248; 24,117,248) | 2,944 |
| Wstr | P1 binary (P1) | 0 (0, 0, 0) | 18,874,368 B (18,874,368; 18,874,368; 18,874,368) | 2,304 |

F2's 1,032 (`Wint`) is of the order of F0's 879 and F1's 978, which also
take shuffled keys at interior positions; `Wstr` divides 1,993 times, about
twice as often, its row carrying a 64-byte inline cell. The counter is not
comparable with P1's 0, and the data file is: F2's is 27.8 % larger than
P1's on both forms (+2,621,440 B, 320 pages, on `Wint`; +5,242,880 B, 640
pages, on `Wstr`; `Wint`'s round 2 is 12,582,912 B, 33.3 % above P1's). The
driver reads the file's size and not the clustered tree's pages apart from
the index's, so the 27.8 % is the whole file; and every file size observed is a multiple
of 524,288 B (64 pages), so a difference smaller than a step
is not resolved. A random-position insert into a tree leaves its leaves
partly full where a right-edge append fills them, which is the direction of
the larger file; the cells do not measure leaf fill, so the size is the
measurement and the reason is the expected one, unverified.

Latency, median over rounds of each percentile (3 rounds), the cells of
sections 1b to 3:

| form | binary | phase | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| Wint | B (F2) | read | 20,000 | 39.4 us | 41.8 us | 42.4 us | 48.3 us | 58.6 us |
| Wint | B (F2) | range | 1,000 | 146.7 us | 156.3 us | 159.9 us | 195.6 us | 271.1 us |
| Wint | B (F2) | update | 4,000 | 40.4 us | 43.5 us | 44.8 us | 53.3 us | 73.4 us |
| Wint | B (F2) | delete | 4,000 | 37.7 us | 40.8 us | 42.0 us | 50.5 us | 66.4 us |
| Wint | P1 binary (P1) | read | 20,000 | 40.2 us | 43.6 us | 44.8 us | 51.3 us | 62.4 us |
| Wint | P1 binary (P1) | range | 1,000 | 145.4 us | 156.5 us | 160.4 us | 191.6 us | 243.8 us |
| Wint | P1 binary (P1) | update | 4,000 | 40.1 us | 43.5 us | 44.9 us | 56.5 us | 79.0 us |
| Wint | P1 binary (P1) | delete | 4,000 | 37.1 us | 40.4 us | 41.7 us | 52.2 us | 66.4 us |
| Wstr | B (F2) | read | 20,000 | 37.7 us | 40.3 us | 41.0 us | 47.0 us | 55.6 us |
| Wstr | B (F2) | range | 1,000 | 142.1 us | 157.3 us | 160.9 us | 194.6 us | 270.2 us |
| Wstr | B (F2) | update | 4,000 | 39.4 us | 41.8 us | 42.6 us | 52.3 us | 76.4 us |
| Wstr | B (F2) | delete | 4,000 | 36.5 us | 38.9 us | 39.7 us | 48.1 us | 65.9 us |
| Wstr | P1 binary (P1) | read | 20,000 | 37.8 us | 41.7 us | 42.4 us | 48.3 us | 58.2 us |
| Wstr | P1 binary (P1) | range | 1,000 | 141.9 us | 156.0 us | 159.9 us | 189.5 us | 245.6 us |
| Wstr | P1 binary (P1) | update | 4,000 | 38.9 us | 41.8 us | 42.6 us | 51.7 us | 72.0 us |
| Wstr | P1 binary (P1) | delete | 4,000 | 36.2 us | 38.7 us | 39.4 us | 47.9 us | 65.4 us |
| F0 | B | read | 20,000 | 36.3 us | 38.5 us | 39.1 us | 44.3 us | 53.2 us |
| F0 | B | range | 1,000 | 68.4 us | 72.3 us | 74.2 us | 91.5 us | 113.6 us |
| F0 | B | update | 4,000 | 36.6 us | 38.7 us | 39.4 us | 46.5 us | 61.1 us |
| F0 | B | delete | 4,000 | 33.8 us | 36.1 us | 36.8 us | 44.9 us | 61.0 us |
| F1 | B | read | 20,000 | 38.0 us | 40.8 us | 41.4 us | 45.9 us | 53.9 us |
| F1 | B | range | 1,000 | 72.3 us | 77.2 us | 79.6 us | 100.4 us | 134.1 us |
| F1 | B | update | 4,000 | 38.6 us | 40.9 us | 41.6 us | 51.6 us | 69.6 us |
| F1 | B | delete | 4,000 | 36.1 us | 38.0 us | 38.6 us | 47.2 us | 62.5 us |
| ref_heap | B | read | 20,000 | 37.4 us | 39.8 us | 40.5 us | 46.1 us | 54.9 us |
| ref_heap | B | update | 4,000 | 37.5 us | 39.9 us | 40.7 us | 49.5 us | 70.3 us |
| ref_heap | B | delete | 4,000 | 34.7 us | 37.1 us | 37.9 us | 45.7 us | 59.7 us |
| int32 | B | read | 20,000 | 38.0 us | 41.1 us | 42.0 us | 47.5 us | 56.9 us |
| int32 | B | update | 4,000 | 39.2 us | 42.5 us | 43.7 us | 54.5 us | 79.2 us |
| int32 | B | move | 4,000 | 41.9 us | 46.9 us | 48.3 us | 60.4 us | 82.1 us |
| int32 | B | delete | 4,000 | 36.2 us | 39.5 us | 40.5 us | 50.2 us | 70.4 us |
| int32x2 | B | read | 20,000 | 39.8 us | 42.6 us | 43.5 us | 53.2 us | 61.5 us |
| int32x2 | B | update | 4,000 | 41.1 us | 44.3 us | 45.4 us | 57.8 us | 77.1 us |
| int32x2 | B | move | 4,000 | 44.3 us | 48.7 us | 50.2 us | 67.6 us | 87.9 us |
| int32x2 | B | delete | 4,000 | 38.2 us | 41.1 us | 42.1 us | 53.8 us | 70.8 us |
| varchar64 | B | read | 20,000 | 38.9 us | 41.1 us | 41.8 us | 47.7 us | 57.1 us |
| varchar64 | B | update | 4,000 | 40.1 us | 42.8 us | 43.8 us | 55.0 us | 80.4 us |
| varchar64 | B | move | 4,000 | 43.7 us | 47.7 us | 49.0 us | 63.5 us | 92.1 us |
| varchar64 | B | delete | 4,000 | 36.9 us | 39.9 us | 40.9 us | 49.6 us | 68.0 us |
| mixed4 | B | read | 20,000 | 40.7 us | 45.4 us | 46.1 us | 54.0 us | 63.0 us |
| mixed4 | B | update | 4,000 | 44.1 us | 47.1 us | 48.2 us | 59.8 us | 81.2 us |
| mixed4 | B | move | 4,000 | 46.9 us | 51.5 us | 52.9 us | 70.3 us | 98.1 us |
| mixed4 | B | delete | 4,000 | 40.3 us | 43.4 us | 44.3 us | 54.3 us | 72.9 us |

## 4. What this run teaches about the engine

- **F2 buys the read what it was built to buy and nothing the range or the
  writes see.** A whole-key read drops from two pages and an index entry to
  one page, 1.58 us to 2.78 us a statement (6 % to 10 %), reaching F1's
  figure on `Wstr` and staying 1.32 us above it on `Wint`; a range, an
  update and a delete by the key are P1's.
- **The insert price is 1.46 us to 1.56 us a row on a base of 8.3 us.**
  Against F0 and F1, a wide key still pays the unique index, so its load is
  2.1x to 2.3x theirs; the hash moves a row's place and adds no second tree.
- **The expectation in `heap-and-tuple.md` §3.1c, that a free home takes the
  row and a read finds it in one descent, is borne out at one cardinality:**
  `pages=1`, no index entry scanned, on a relation loaded once and read after
  its load. The harder shapes (a re-insert after a delete, a moved key) are
  the ones the spec says fall back to the index, and none ran here.
- **The data file is 27.8 % larger than P1's and the tree divides about
  1,000 to 2,000 times per 100,000 rows**; whether that is leaf fill or
  something else is not separated by the driver.
- **Part 1's 3 outside cells are two loads, by 0.03 us/row to 0.09 us/row
  above a narrow control, and one `walked` move in the direction of a
  saving.** The first close's between-builds floor (section 1a of that
  file) shows a `walked` swing of 366 us across builds of one source, which
  is the size of the move here; the two loads are the phases the
  milestone's insert code is on.

## 5. Wait breakdown

Does not apply. One session on one connection, `cores = 1`, `relaxed`
durability (no commit fsync), pages resident in a pool of 131,072 frames
(no read waits), no concurrent writer (no lock wait): every figure above is
server CPU a statement, and the client latency is that CPU plus the socket
round trip (about 14 us). A decomposition into durability, read, lock and
conflict waits would assign shares to waits this run does not have.
Baseline: this engine's own previous number is the previous close's file
for the cells that did not change (section 1b) and run 3 (the P1 binary)
for the wide key; section 1's baseline is the opening commit's binary
measured inside run 1. The PostgreSQL floor was not measured for this
shape.

## 6. What the cells do not show

Stated as unverified, with the method that would check each.

| not measured | why it matters | what would check it |
|---|---|---|
| a read inside a read-write transaction | on F2 it reads the index by design (`heap-and-tuple.md` §3.1c: the home is tried only under a view that is a commit point), so it costs what P1's read costs; not measured | the driver's read phase inside `BEGIN ... READ WRITE` on `Wint` and `Wstr` |
| a relation larger than the pool | every page is resident here: F2's insert at a random leaf is a memory touch, and on a device it is a page read, so the +17.5 % and +18.7 % are a lower bound of a cold pool's price | `--frames` below the working set, with the page reader's waits named |
| `cores` above 1 | the home's row unit asked without waiting, the clustered descent and the index under two cores | the `built` part at `cores = 2` and above |
| two sessions racing one home | the home's unit is asked try-only and a refusal costs the row its place (an issued id); the rate of that, and the read of such a row, are unmeasured | a two-session driver inserting keys that hash alike or one key from two sessions |
| rows off their homes | the driver deletes after its reads and inserts nothing again: a key re-inserted after a delete with no `PURGE`, a moved key, a bound home, so the read's fallback to the index and the insert's issued id after a refused home are not exercised | a driver that deletes and re-inserts, or moves, a fraction of the keys and reads them, with the plan line's counters |
| 1,000,000 rows (and the 200- and 10,000-row sweep of the benchmark rules) | one cardinality, 100,000: a fixed cost and a per-row one are not separated, and F2's data file and leaf divisions at another size are not seen | `--rows 1000000`, `--rows 200` and `--rows 10000` |
| instructions per statement | no hardware counters (`perf` is absent on this host), so a time delta is not split into instructions and layout; the btree `walked` move of section 1a is therefore unexplained | a host with `perf stat` or a counter-reading harness over the same cells |
| the layout spread of the opening commit's own source | no third arm built from `e9df68d02` with a perturbed layout; the first close's section 1a floor is on B's source and a different B | rebuild `e9df68d02` with a changed link order and run it as A3 |
| runs 2 and 3 interleaved | they are sequential, so a host drift between 09:12 and 09:14 is in every F2 - P1 difference (the floor of section 1b bounds it for the shared-path cells only) | an `ab`-style driver that alternates the two binaries per round |
| the clustered tree's pages and the index's pages apart | the data file is one number; the driver does not read the pages by owner | `sys.relation_stats` space columns per structure, or a page walk |
