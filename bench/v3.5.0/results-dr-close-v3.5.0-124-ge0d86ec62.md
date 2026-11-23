# DR close at the closing commit - what the per-row catalog scan's remedy moved, and what it left where it was

Measured 2026-11-23 between 09:22 and 09:27 UTC on worktree
`dq-cabin-miss-build`, branch `worktree-dr-primary-key`, at `e0d86ec62`
(`v3.5.0-124-ge0d86ec62`), the commit DR closes at. The commit changes one
thing: `Catalog::AllocateRowId` and `AdmitExplicitRowId` scan `sys.tables`
from its head to the relation's row once per inserted row, and used to decode
every row on the way; they now pass a row that is not the relation's over on
its four oid bytes (`ForRowOfOid`, `src/catalog/catalog.cpp`). It touches
every insert on every relation and nothing else. The earlier runs of this
milestone are kept as they were:
`bench/v3.5.0/results-dr-close-v3.5.0-123-g824acfbed.md` (the hashed form F2
against the indexed form P1, and the run that found two loads outside the
control) and `bench/v3.5.0/results-dr-close-v3.5.0-120-gbff54d524.md` (the
first close). Driver `tools/dr_close.py` (documented in its docstring),
unchanged, commands as in the `-123` file's runs 1 and 2; raw output in
`bench/v3.5.0/archive/dr-close-v3.5.0-124-ge0d86ec62/`. The premise the
milestone was opened against is
`bench/v3.5.0/results-dr-s0-premise-v3.5.0-114-ge9df68d02.md`.

| stamp | value |
|---|---|
| Executed (UTC) | run 1 `ab` (A against B, 5 rounds, 15 cells): 2026-11-23 09:22:57 to 09:24:40 (102.8 s); run 2 `built` on B (3 rounds, 27 cells): 09:25:11 to 09:27:02 (111.2 s). Sequential, not interleaved. No P1 run (section 3) |
| Worktree / branch | `dq-cabin-miss-build` / `worktree-dr-primary-key` |
| Commit measured (B) | `e0d86ec62`, `v3.5.0-124-ge0d86ec62`, committed 2026-11-23 09:22:16 UTC; `git status` clean at 09:22:50 and at 09:25:11 (the starts of runs 1 and 2) |
| Binary B | copy `/home/louis/dr-bench/close/kds_server_B`, sha256 `2f98b36199c69219...` (first 16, hashed by the driver in both runs and equal to `build-release/kds_server`'s at 09:27); source `build-release/kds_server` mtime 2026-11-23 09:22:27 UTC, 11 s after the commit, so the binary is not older than HEAD |
| Binary A and A2 (opening) | one file, copy `/home/louis/dr-bench/s0/kds_server_e9df68d0`, sha256 `e33a55473f81fade...`, commit `e9df68d02` (`v3.5.0-114-ge9df68d02`); A2 is a second server started from a second copy of the same file (the control) |
| Build type | Release (`build-release`), nothing rebuilt for these runs |
| Device | `/dev/nvme0n1p1`, ext4, mounted `/` (`df -T` at 09:22 UTC); data files under `/home/louis/dr-bench/close/w`, not tmpfs |
| Server configuration | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, one fresh server and data file per cell; server pinned to host CPU 2, driver to CPUs 0 and 4; defaults otherwise |
| Shape | 100,000 rows; reads 20,000, writes 4,000, walked 40, ranges 1,000 statements a phase; 20 batches a phase; driver defaults |
| Machine | quiet-gate hold 0.0 s in all 42 cells (15 + 27) and nothing waited for; `cc1plus` count 0 at every cell's start and end; host load average (1 min) 1.43 to 2.20 across run 1 and 0.96 to 1.38 across run 2, the driver's own server and client included (the 1-, 5- and 15-minute averages were 2.51, 3.42 and 3.23 at 09:22:50, left by the parent session's earlier builds; no build ran during either run); an idle `build-sandbox` `kds_server` and an unrelated database process of another user were resident throughout |

Server CPU a statement is the median of the phase's 20 batches; a table
cell reads "median over rounds [min-max over rounds]" unless a delta is
shown. Latency percentiles are the median over rounds of each percentile.
Wait breakdown: **does not apply** (section 5).

## The answer first

**Both loads are inside the control (section 1).** On `dq-cabin-miss-build`
at `e0d86ec62`, run 1's B - A is -0.01 us/row (-0.3 %) on the btree load and
-0.08 us/row (-1.0 %) on the heap load, each inside the control's range
(-0.12 to +0.05 and -0.08 to +0.05 us/row). Of the 14 phases, 11 are inside
and 3 are outside: btree `read` (+0.32 us, +1.4 %, 0.05 us above the control's
upper end), btree `range` (+0.22 us, +0.5 %, 0.26 us above a control range
that is -2.14 to -0.04 us) and btree `walked` (-277.46 us, -4.0 %, 148.39 us
below the control's lower end, B faster, all five of B's rounds below all
five of A's). All seven heap phases are inside; the heap `walked`, the
phase DR added an instruction to, is -91.45 us (-1.3 %), inside. Across the
nine builds of this milestone the load's B - A is positive in the seven
that precede this commit's change (+0.08 to +0.23 us/row on the btree, +0.11
to +0.34 us/row on the heap) and -0.06 and -0.01 us/row (btree), -0.05 and
-0.08 us/row (heap) in B6, the uncommitted tree of the change, and this run
(section 1b).

**Repeated over 15 rounds (section 1a2).** A 15-round repeat of part 1 on
the same two binaries, run by the parent session, has 13 of 14 phases inside
and the btree `walked` outside (B below A in 15 of 15 rounds, -414.59 us,
-5.9 %). Of the 5-round run's three outside phases it repeats `walked` and
does not repeat btree `read` (+0.00 us) or `range` (-0.53 us); the loads are
-0.08 us/row (btree) and -0.09 us/row (heap), B above A in 1 of 15 rounds
each; heap `range` (+1.51 us, +2.1 %), `delete` and `insert` lean to B above
A, inside the control. A wider control is what more rounds give.

**What run 2 moved (section 2).** Of run 2's 44 cells, 26 are inside and 18
outside the between-run floor the `-123` file measured. Seven of the nine
loads moved: F0 -0.31 us/row (-7.4 %), F1 -0.23 (-5.0 %), `mixed4` -0.21,
`ref_heap` -0.15, `int32` -0.11, `int32x2` -0.08 us/row, and `varchar64`
+0.17 us/row (+1.4 %); `Wint` (+0.07 us/row, +0.7 %) and `Wstr` (+0.03
us/row, +0.3 %) are inside. Eleven of the 35 other cells are outside, by
0.06 us to 3.08 us, the largest being the two wide keys' ranges (+3.73 us
and +4.20 us).

## 1. "No other path affected"

### 1a. Run 1: the unchanged relation, A against A2 against B

Server CPU, `us` a statement (`us/row` for load), 100,000 rows, 5 rounds.
Control delta = A2 - A (medians). "Control range" is the lowest and highest
per-round A2 - A of the five rounds; "inside" means the median B - A lies
between them. Percent is of the A median.


btree:

| phase | A | A2 | B | A2 - A (control) | control range | B - A | verdict |
|---|---|---|---|---|---|---|---|
| load | 3.46 us/row [3.44-3.59] | 3.47 us/row [3.41-3.50] | 3.45 us/row [3.32-3.48] | +0.01 us/row (+0.2 %) | -0.12 us/row to +0.05 us/row | -0.01 us/row (-0.3 %) | inside |
| read | 22.56 us [22.49-22.63] | 22.38 us [22.25-22.83] | 22.88 us [22.58-23.34] | -0.18 us (-0.8 %) | -0.26 us to +0.27 us | +0.32 us (+1.4 %) | outside by +0.05 us |
| update | 25.03 us [24.86-25.13] | 24.64 us [24.42-26.02] | 25.20 us [24.76-26.18] | -0.39 us (-1.6 %) | -0.67 us to +0.99 us | +0.17 us (+0.7 %) | inside |
| walked | 7,013.92 us [6,999.76-7,122.30] | 6,993.23 us [6,959.66-7,025.40] | 6,736.46 us [6,660.29-6,774.20] | -20.69 us (-0.3 %) | -129.07 us to +10.60 us | -277.46 us (-4.0 %) | outside by -148.39 us |
| range | 46.31 us [46.02-47.28] | 45.72 us [44.92-45.98] | 46.53 us [45.57-47.60] | -0.59 us (-1.3 %) | -2.14 us to -0.04 us | +0.22 us (+0.5 %) | outside by +0.26 us |
| insert | 22.66 us [22.20-22.84] | 22.42 us [22.21-22.65] | 22.80 us [22.42-23.03] | -0.24 us (-1.1 %) | -0.40 us to +0.45 us | +0.14 us (+0.6 %) | inside |
| delete | 22.09 us [21.87-22.55] | 22.16 us [21.90-22.38] | 22.30 us [22.25-22.75] | +0.07 us (+0.3 %) | -0.65 us to +0.51 us | +0.21 us (+1.0 %) | inside |

heap:

| phase | A | A2 | B | A2 - A (control) | control range | B - A | verdict |
|---|---|---|---|---|---|---|---|
| load | 7.45 us/row [7.42-7.52] | 7.43 us/row [7.39-7.50] | 7.37 us/row [7.34-7.59] | -0.02 us/row (-0.3 %) | -0.08 us/row to +0.05 us/row | -0.08 us/row (-1.0 %) | inside |
| read | 23.91 us [23.51-23.94] | 23.73 us [23.53-24.59] | 24.15 us [23.70-24.72] | -0.18 us (-0.8 %) | -0.35 us to +0.65 us | +0.24 us (+1.0 %) | inside |
| update | 26.77 us [26.59-26.90] | 27.18 us [26.05-27.32] | 26.90 us [26.50-27.42] | +0.41 us (+1.5 %) | -0.81 us to +0.73 us | +0.13 us (+0.5 %) | inside |
| walked | 6,813.36 us [6,778.23-6,840.00] | 6,872.36 us [6,684.35-7,021.28] | 6,721.91 us [6,707.56-6,983.58] | +59.00 us (+0.9 %) | -129.01 us to +210.83 us | -91.45 us (-1.3 %) | inside |
| range | 70.76 us [70.44-71.37] | 71.93 us [70.18-73.55] | 71.58 us [71.44-72.64] | +1.17 us (+1.7 %) | -1.13 us to +2.79 us | +0.82 us (+1.2 %) | inside |
| insert | 27.44 us [27.27-27.59] | 27.58 us [26.90-28.02] | 27.47 us [27.25-28.13] | +0.14 us (+0.5 %) | -0.69 us to +0.63 us | +0.03 us (+0.1 %) | inside |
| delete | 23.44 us [22.84-23.65] | 23.68 us [22.84-23.78] | 23.59 us [23.29-23.83] | +0.24 us (+1.0 %) | -0.61 us to +0.84 us | +0.15 us (+0.6 %) | inside |


**The `walked` phase** (`UPDATE t SET w = w + 1 WHERE v = k`, no key; 100,000
rows walked a statement), on its own line:

| relation | B - A | control range | verdict |
|---|---|---|---|
| btree | -277.46 us (-4.0 %), -2.77 ns a row walked | -129.07 to +10.60 us | outside by -148.39 us (B below) |
| heap | -91.45 us (-1.3 %), -0.91 ns a row walked | -129.01 to +210.83 us | inside |

What is in and what is out: the two loads and eleven other phases are
inside; the three outside are all on the btree and all within 0.26 us of the
control's end on `read` and `range`, +1.4 % and +0.5 % of the A median; the
`walked` move is 148.39 us beyond a control that is itself -129.07 to +10.60
us and has the sign of a saving, as in the `-123` file's run 1 (-295.31 us)
and as in the first close (a 366.19 us swing between builds of one source on
that phase, section 1a of `-120`); no hardware counter exists on this host,
so the move stays unexplained. The control is five paired rounds and bounds
what two runs of one binary showed here; it is not a confidence interval.

Latency (client), median over rounds of each percentile; the client latency
carries the socket round trip (about 14 us over the server CPU).

| relation | phase | arm | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| btree | read | A | 20,000 | 36.1 us | 38.3 us | 38.9 us | 44.9 us | 54.6 us |
| btree | read | A2 | 20,000 | 36.1 us | 38.2 us | 38.8 us | 45.6 us | 53.6 us |
| btree | read | B | 20,000 | 36.1 us | 38.5 us | 39.1 us | 45.8 us | 52.1 us |
| btree | update | A | 4,000 | 36.6 us | 38.8 us | 39.5 us | 48.4 us | 65.4 us |
| btree | update | A2 | 4,000 | 36.4 us | 38.5 us | 39.1 us | 47.1 us | 62.9 us |
| btree | update | B | 4,000 | 36.5 us | 38.8 us | 39.5 us | 49.3 us | 69.9 us |
| btree | walked | A | 40 | 6,914.4 us | 7,045.3 us | 7,091.5 us | 7,447.6 us | 7,734.0 us |
| btree | walked | A2 | 40 | 6,914.6 us | 6,993.8 us | 7,053.9 us | 7,387.4 us | 7,570.7 us |
| btree | walked | B | 40 | 6,580.4 us | 6,736.6 us | 6,807.4 us | 7,106.6 us | 7,698.4 us |
| btree | range | A | 1,000 | 69.3 us | 73.3 us | 75.5 us | 98.8 us | 146.5 us |
| btree | range | A2 | 1,000 | 68.6 us | 72.4 us | 74.8 us | 89.7 us | 123.7 us |
| btree | range | B | 1,000 | 68.8 us | 72.7 us | 75.0 us | 95.5 us | 130.7 us |
| btree | insert | A | 4,000 | 34.4 us | 36.4 us | 37.0 us | 46.4 us | 67.8 us |
| btree | insert | A2 | 4,000 | 34.4 us | 36.3 us | 36.8 us | 45.9 us | 63.1 us |
| btree | insert | B | 4,000 | 34.4 us | 36.5 us | 37.1 us | 46.2 us | 66.1 us |
| btree | delete | A | 4,000 | 34.0 us | 36.3 us | 37.0 us | 44.9 us | 59.1 us |
| btree | delete | A2 | 4,000 | 34.0 us | 36.3 us | 37.0 us | 44.3 us | 61.0 us |
| btree | delete | B | 4,000 | 34.2 us | 36.5 us | 37.1 us | 44.9 us | 60.8 us |
| heap | read | A | 20,000 | 36.9 us | 39.4 us | 40.1 us | 46.8 us | 57.4 us |
| heap | read | A2 | 20,000 | 36.8 us | 39.4 us | 40.1 us | 46.1 us | 56.0 us |
| heap | read | B | 20,000 | 37.1 us | 39.8 us | 40.6 us | 47.8 us | 57.6 us |
| heap | update | A | 4,000 | 37.2 us | 39.9 us | 40.8 us | 51.7 us | 74.7 us |
| heap | update | A2 | 4,000 | 37.4 us | 40.1 us | 41.2 us | 52.4 us | 73.6 us |
| heap | update | B | 4,000 | 37.5 us | 40.1 us | 41.1 us | 51.8 us | 76.0 us |
| heap | walked | A | 40 | 6,690.3 us | 6,815.3 us | 6,868.5 us | 7,075.5 us | 7,225.0 us |
| heap | walked | A2 | 40 | 6,688.8 us | 6,898.8 us | 6,968.6 us | 7,354.5 us | 7,873.3 us |
| heap | walked | B | 40 | 6,615.3 us | 6,744.4 us | 6,793.5 us | 7,250.9 us | 7,462.1 us |
| heap | range | A | 1,000 | 94.1 us | 97.1 us | 98.6 us | 121.5 us | 154.7 us |
| heap | range | A2 | 1,000 | 93.5 us | 97.0 us | 98.8 us | 133.9 us | 168.4 us |
| heap | range | B | 1,000 | 94.6 us | 97.9 us | 99.4 us | 125.0 us | 188.5 us |
| heap | insert | A | 4,000 | 38.3 us | 40.7 us | 41.5 us | 53.5 us | 76.3 us |
| heap | insert | A2 | 4,000 | 38.0 us | 40.4 us | 41.3 us | 52.6 us | 75.9 us |
| heap | insert | B | 4,000 | 38.3 us | 40.6 us | 41.5 us | 53.6 us | 81.3 us |
| heap | delete | A | 4,000 | 34.6 us | 37.1 us | 37.9 us | 46.2 us | 62.1 us |
| heap | delete | A2 | 4,000 | 34.4 us | 37.0 us | 37.8 us | 47.3 us | 62.1 us |
| heap | delete | B | 4,000 | 34.7 us | 37.1 us | 38.0 us | 47.1 us | 67.1 us |


### 1a2. Part 1 repeated over 15 server lifetimes

A repeat of run 1 on the same two binaries (B `2f98b36199c69219`, the
closing commit `e0d86ec62`; A `e33a55473f81fade`), **run by the parent
session, not by this agent**, after run 2: `tools/dr_close.py --part ab
--rounds 15`, workdir `/home/louis/dr-bench/close/w15`, 2026-11-23 09:30:17
to 09:35:25 UTC (307.8 s); raw output `ab-15.json` and `ab-15.beat` in
the archive. From the files: 45 cells, 15 of each of A, A2 and B, the
arm order rotating through six orders (`A2B`, `AB2`, `BA2`, `B2A`, `2BA`,
`2AB`) over the rounds; the three arms' binary hashes as above; quiet-gate
hold 0.0 s in every cell, `cc1plus` 0 at every cell, 1-min load 0.42 to 1.47
at the cells, the heartbeat's first cell 09:30:24 and its `done` line
09:35:25 (307.8 s). Same driver, same shape. Computed as in section 1a, from
15 rounds: the control is the lowest and highest of 15 per-round A2 - A
(wider than five rounds give); the last column counts the rounds, of 15, in
which B's value is above A's.


btree:

| phase | A | A2 | B | A2 - A (control) | control range | B - A | verdict | rounds with B above A |
|---|---|---|---|---|---|---|---|---|
| load | 3.44 us/row [3.39-3.65] | 3.46 us/row [3.39-3.59] | 3.35 us/row [3.31-3.46] | +0.02 us/row (+0.7 %) | -0.23 us/row to +0.16 us/row | -0.08 us/row (-2.5 %) | inside | 1 of 15 |
| read | 22.61 us [22.01-23.47] | 22.49 us [22.18-23.51] | 22.61 us [22.27-23.48] | -0.12 us (-0.5 %) | -1.15 us to +1.00 us | +0.00 us (+0.0 %) | inside | 8 of 15 |
| update | 25.02 us [24.29-25.88] | 24.97 us [24.46-27.23] | 24.90 us [24.53-25.37] | -0.05 us (-0.2 %) | -1.33 us to +1.49 us | -0.12 us (-0.5 %) | inside | 5 of 15 |
| walked | 7,041.84 us [6,949.24-7,289.87] | 7,053.54 us [6,881.33-7,300.92] | 6,627.25 us [6,588.31-6,743.15] | +11.70 us (+0.2 %) | -408.54 us to +351.68 us | -414.59 us (-5.9 %) | outside by -6.05 us | 0 of 15 |
| range | 46.23 us [45.28-48.70] | 46.34 us [45.13-47.57] | 45.70 us [45.25-47.83] | +0.11 us (+0.2 %) | -2.27 us to +1.80 us | -0.53 us (-1.1 %) | inside | 5 of 15 |
| insert | 22.75 us [22.45-23.21] | 22.63 us [22.30-23.20] | 22.51 us [22.14-23.19] | -0.12 us (-0.5 %) | -0.53 us to +0.70 us | -0.24 us (-1.1 %) | inside | 6 of 15 |
| delete | 22.26 us [21.83-23.12] | 22.16 us [21.96-22.87] | 22.29 us [22.00-23.04] | -0.10 us (-0.4 %) | -0.96 us to +0.93 us | +0.03 us (+0.1 %) | inside | 8 of 15 |

heap:

| phase | A | A2 | B | A2 - A (control) | control range | B - A | verdict | rounds with B above A |
|---|---|---|---|---|---|---|---|---|
| load | 7.45 us/row [7.38-7.65] | 7.46 us/row [7.38-7.69] | 7.36 us/row [7.29-7.76] | +0.01 us/row (+0.1 %) | -0.20 us/row to +0.26 us/row | -0.09 us/row (-1.3 %) | inside | 1 of 15 |
| read | 23.91 us [23.44-24.71] | 23.90 us [23.61-24.39] | 23.88 us [23.57-24.67] | -0.01 us (-0.0 %) | -0.79 us to +0.76 us | -0.03 us (-0.1 %) | inside | 8 of 15 |
| update | 26.56 us [26.27-28.29] | 26.71 us [26.31-27.62] | 26.67 us [26.30-27.60] | +0.15 us (+0.6 %) | -1.58 us to +1.06 us | +0.11 us (+0.4 %) | inside | 7 of 15 |
| walked | 6,764.35 us [6,708.17-7,041.64] | 6,759.08 us [6,713.16-6,953.74] | 6,671.10 us [6,640.32-6,994.48] | -5.27 us (-0.1 %) | -233.69 us to +245.57 us | -93.25 us (-1.4 %) | inside | 3 of 15 |
| range | 70.29 us [69.34-73.39] | 70.44 us [69.36-72.54] | 71.80 us [70.35-76.82] | +0.15 us (+0.2 %) | -3.14 us to +2.17 us | +1.51 us (+2.1 %) | inside | 12 of 15 |
| insert | 27.01 us [26.70-27.92] | 27.17 us [26.86-27.84] | 27.17 us [26.80-28.79] | +0.16 us (+0.6 %) | -0.69 us to +0.79 us | +0.16 us (+0.6 %) | inside | 10 of 15 |
| delete | 23.10 us [22.58-23.94] | 23.24 us [22.69-23.77] | 23.32 us [23.06-24.29] | +0.14 us (+0.6 %) | -0.98 us to +0.94 us | +0.22 us (+1.0 %) | inside | 11 of 15 |

What the two runs show together. **13 of 14 phases are inside the 15-round
control; the one outside is the btree `walked`**, B below A in 15 of 15
rounds (-414.59 us, -5.9 %), 6.05 us past a control that reaches -408.54 us:
the 5-round run's `walked` finding (-277.46 us, outside) is repeated, in the
direction of a saving, and still unexplained. Of the 5-round run's other
two outside phases the 15-round run does **not** repeat them: btree `read`
is +0.00 us (+0.32 us before) and btree `range` is -0.53 us (+0.22 us
before), both inside, with B above A in 8 and 5 of 15 rounds, so those two
were not repeated as costs. The loads are -0.08 us/row (btree) and -0.09
us/row (heap), B above A in 1 of 15 rounds each, which is the sign of the
5-round run's loads (-0.01 and -0.08 us/row). A wider control is what more
rounds give: the control range is wider in every phase than the 5-round one
(for example btree `read` -1.15 to +1.00 us against -0.26 to +0.27 us), so
"inside" over 15 rounds is a weaker statement about any one round and a
stronger one about the medians, which are estimated from three times the
rounds. Phases that lean one way inside the control: heap `range` +1.51 us
(+2.1 %), B above A in 12 of 15 rounds, heap `delete` +0.22 us (+1.0 %) in
11 and heap `insert` +0.16 us (+0.6 %) in 10; the 5-round run had heap
`range` +0.82 us, `delete` +0.15 us and `insert` +0.03 us, the same sign. Btree
`walked` leans to B below A, heap `walked` too (B above A in 3 of 15
rounds, -93.25 us, inside). Whether the heap leans are costs or the
control's own skew is not separable here.


### 1b. The load in every build of this milestone

B - A of the `load` phase, each against the A (and control A2) of its own
run, 5 rounds each, medians; percent is of that run's A. Rows B1 to B5 and
the `-120` run's final B are from
`bench/v3.5.0/archive/dr-close-v3.5.0-120-gbff54d524/` (`diag-ab-B1.json` to
`diag-ab-B5.json`, `ab.json`), the `-123` run's B from
`bench/v3.5.0/archive/dr-close-v3.5.0-123-g824acfbed/ab.json`, B6 from that
archive's `diag-ab-B6.json` (the parent's own diagnostic A/B of the change,
run 09:15:37 to 09:17:20 UTC on an uncommitted tree, binary sha
`8c21d8e30d702f62`), and the last row from this run. Different B binaries,
each against its own A; the control column is that run's A2 - A.

| build | B sha256 (first 16) | btree load B - A | heap load B - A | btree A2 - A | heap A2 - A |
|---|---|---|---|---|---|
| B1 | a2e295474dc27504 | +0.13 us/row (+3.7 %) | +0.34 us/row (+4.5 %) | -0.05 us/row | +0.02 us/row |
| B2 | f295d05a1affed4b | +0.23 us/row (+6.6 %) | +0.16 us/row (+2.1 %) | -0.05 us/row | +0.06 us/row |
| B3 | 49bcb6e3b0ed12b4 | +0.10 us/row (+2.9 %) | +0.11 us/row (+1.4 %) | -0.06 us/row | -0.10 us/row |
| B4 | 7cf0302c22b1ea8d | +0.12 us/row (+3.4 %) | +0.13 us/row (+1.7 %) | +0.05 us/row | -0.03 us/row |
| B5 | 09668d34e0741855 | +0.08 us/row (+2.2 %) | +0.11 us/row (+1.5 %) | -0.04 us/row | -0.09 us/row |
| `-120` run, final B | 5d7524c6a3f2846d | +0.14 us/row (+3.9 %) | +0.17 us/row (+2.3 %) | +0.00 us/row | +0.15 us/row |
| `-123` run, B | 2872c552bc049881 | +0.18 us/row (+5.1 %) | +0.13 us/row (+1.8 %) | +0.00 us/row | -0.02 us/row |
| B6 (diag) | 8c21d8e30d702f62 | -0.06 us/row (-1.7 %) | -0.05 us/row (-0.7 %) | +0.00 us/row | -0.05 us/row |
| this run, B | 2f98b36199c69219 | -0.01 us/row (-0.3 %) | -0.08 us/row (-1.0 %) | +0.01 us/row | -0.02 us/row |


The sign changes between the `-123` run's B and B6 and holds in this run's
B, whose binary differs from B6's (B6 was built from the uncommitted tree).
The seven builds before the change differ by 0.08 to 0.23 us/row (btree) and
0.11 to 0.34 us/row (heap) above A; the two after sit at -0.06 to -0.01 and
-0.08 to -0.05 us/row. The control column's medians are -0.10 to +0.15
us/row; the per-round control ranges, which the verdicts use, are in
section 1a for this run and in the `-123` file's section 1a for that one
(outside on both loads there, inside on both here).

## 2. Run 2: every form's cells against the `-123` run's

Run 2 is the `built` part on B at `e0d86ec62`; the comparison column is the
`-123` file's run 2, B at `824acfbed`, from
`bench/v3.5.0/archive/dr-close-v3.5.0-123-g824acfbed/built.json` (3 rounds
each). Server CPU, median [min-max over 3 rounds]; `move` is the heap
forms' UPDATE that moves the key. The floor column reads each delta against
the span of run 3 minus run 2 that the `-123` file's section 1b measured for
the same phase over the cells whose path was the same in both of its
binaries (load -0.00 to +0.13 us/row, read -0.82 to +0.44 us, range -1.73 to
+1.12 us, update -0.49 to +0.20 us, move -0.44 to -0.11 us, delete -0.27 to
+0.29 us). That floor is two adjacent runs of one path; here the two runs
are 15 minutes apart and a code path changed, and the load and move floors
are one-sided (every one of the four moves and 6 of 7 loads in that table
had the same sign), so "outside" on those two phases reads against a narrow
band.

| form | phase | `-123` run 2 (`824acfbed`) | this run (`e0d86ec62`) | delta | delta % | vs between-run floor |
|---|---|---|---|---|---|---|
| ref_heap | load | 7.57 us/row [7.54-7.58] | 7.42 us/row [7.37-7.54] | -0.15 us/row | -2.0 % | outside by -0.15 us/row |
| ref_heap | read | 23.92 us [23.68-24.03] | 23.85 us [23.72-24.39] | -0.07 us | -0.3 % | inside |
| ref_heap | update | 26.05 us [26.02-26.12] | 26.08 us [25.89-26.52] | +0.03 us | +0.1 % | inside |
| ref_heap | delete | 23.20 us [23.00-23.36] | 23.29 us [23.21-23.31] | +0.09 us | +0.4 % | inside |
| int32 | load | 11.68 us/row [11.67-11.94] | 11.56 us/row [11.52-11.69] | -0.11 us/row | -1.0 % | outside by -0.11 us/row |
| int32 | read | 25.61 us [25.52-25.79] | 25.63 us [25.58-25.64] | +0.02 us | +0.1 % | inside |
| int32 | update | 29.49 us [28.74-29.74] | 28.88 us [28.44-28.98] | -0.61 us | -2.1 % | outside by -0.12 us |
| int32 | move | 33.98 us [33.25-34.14] | 33.55 us [33.37-36.43] | -0.43 us | -1.3 % | inside |
| int32 | delete | 25.96 us [25.80-26.04] | 26.31 us [25.62-26.85] | +0.35 us | +1.3 % | outside by +0.06 us |
| int32x2 | load | 11.78 us/row [11.77-12.24] | 11.71 us/row [11.65-12.01] | -0.08 us/row | -0.6 % | outside by -0.08 us/row |
| int32x2 | read | 27.84 us [27.33-28.39] | 27.24 us [26.93-27.51] | -0.60 us | -2.2 % | inside |
| int32x2 | update | 31.06 us [30.80-32.80] | 30.94 us [30.51-31.07] | -0.12 us | -0.4 % | inside |
| int32x2 | move | 35.86 us [35.48-36.66] | 35.82 us [34.73-35.89] | -0.04 us | -0.1 % | outside by +0.07 us |
| int32x2 | delete | 27.16 us [26.96-28.92] | 27.38 us [27.16-27.59] | +0.22 us | +0.8 % | inside |
| varchar64 | load | 12.20 us/row [12.15-12.23] | 12.37 us/row [12.14-12.60] | +0.17 us/row | +1.4 % | outside by +0.04 us/row |
| varchar64 | read | 25.30 us [25.30-25.30] | 25.36 us [25.15-25.87] | +0.06 us | +0.2 % | inside |
| varchar64 | update | 29.62 us [29.34-29.70] | 29.25 us [29.09-31.01] | -0.37 us | -1.2 % | inside |
| varchar64 | move | 35.00 us [34.49-35.14] | 35.07 us [34.20-36.37] | +0.07 us | +0.2 % | outside by +0.18 us |
| varchar64 | delete | 26.36 us [25.89-26.52] | 25.98 us [25.40-26.47] | -0.38 us | -1.4 % | outside by -0.11 us |
| mixed4 | load | 13.01 us/row [12.94-13.06] | 12.80 us/row [12.76-13.00] | -0.21 us/row | -1.6 % | outside by -0.21 us/row |
| mixed4 | read | 29.68 us [29.67-30.42] | 29.76 us [29.70-30.72] | +0.08 us | +0.3 % | inside |
| mixed4 | update | 34.23 us [33.62-34.41] | 33.36 us [33.27-36.02] | -0.87 us | -2.5 % | outside by -0.38 us |
| mixed4 | move | 38.99 us [38.47-39.46] | 38.41 us [38.01-39.80] | -0.58 us | -1.5 % | outside by -0.14 us |
| mixed4 | delete | 30.18 us [29.63-30.41] | 30.14 us [29.52-30.39] | -0.04 us | -0.1 % | inside |
| F0 | load | 4.19 us/row [4.13-4.24] | 3.88 us/row [3.83-3.95] | -0.31 us/row | -7.4 % | outside by -0.31 us/row |
| F0 | read | 22.67 us [22.54-22.94] | 22.84 us [22.75-22.95] | +0.17 us | +0.7 % | inside |
| F0 | range | 45.64 us [45.57-46.49] | 45.65 us [45.00-48.21] | +0.01 us | +0.0 % | inside |
| F0 | update | 24.80 us [24.66-25.16] | 24.86 us [24.63-24.91] | +0.06 us | +0.2 % | inside |
| F0 | delete | 22.26 us [22.00-22.51] | 22.29 us [22.07-22.35] | +0.03 us | +0.1 % | inside |
| F1 | load | 4.58 us/row [4.55-4.71] | 4.35 us/row [4.33-4.47] | -0.23 us/row | -5.0 % | outside by -0.23 us/row |
| F1 | read | 24.53 us [24.44-25.30] | 24.70 us [24.63-25.21] | +0.17 us | +0.7 % | inside |
| F1 | range | 51.49 us [48.76-51.89] | 50.38 us [49.91-52.02] | -1.11 us | -2.2 % | inside |
| F1 | update | 27.21 us [26.55-27.24] | 26.86 us [26.40-27.21] | -0.35 us | -1.3 % | inside |
| F1 | delete | 23.95 us [23.78-24.27] | 23.97 us [23.80-24.81] | +0.02 us | +0.1 % | inside |
| Wint | load | 9.81 us/row [9.77-9.86] | 9.87 us/row [9.72-9.88] | +0.07 us/row | +0.7 % | inside |
| Wint | read | 25.85 us [25.65-26.11] | 26.03 us [25.94-26.74] | +0.18 us | +0.7 % | inside |
| Wint | range | 133.15 us [130.21-134.61] | 136.88 us [131.91-139.48] | +3.73 us | +2.8 % | outside by +2.61 us |
| Wint | update | 30.04 us [29.99-30.44] | 30.80 us [29.75-30.85] | +0.76 us | +2.5 % | outside by +0.56 us |
| Wint | delete | 27.22 us [26.88-27.42] | 27.57 us [26.90-27.59] | +0.35 us | +1.3 % | outside by +0.06 us |
| Wstr | load | 9.87 us/row [9.84-9.90] | 9.90 us/row [9.86-10.22] | +0.03 us/row | +0.3 % | inside |
| Wstr | read | 24.51 us [23.92-24.73] | 24.58 us [24.39-24.60] | +0.07 us | +0.3 % | inside |
| Wstr | range | 132.44 us [130.44-137.13] | 136.64 us [135.10-137.09] | +4.20 us | +3.2 % | outside by +3.08 us |
| Wstr | update | 28.12 us [27.84-28.40] | 28.27 us [28.02-28.41] | +0.15 us | +0.5 % | inside |
| Wstr | delete | 25.00 us [24.77-25.70] | 25.19 us [24.95-25.39] | +0.19 us | +0.8 % | inside |


Reading the table. The nine loads: `ref_heap`, `int32`, `int32x2`, `mixed4`,
F0 and F1 are 0.08 to 0.31 us/row lower than at `824acfbed`, `varchar64` is
0.17 us/row higher, and `Wint` and `Wstr` (F2) did not move (+0.07 and +0.03
us/row) while F0 and F1 did. The 35 cells that are not loads: 24 are inside
the floor; the 11 outside are `int32` update (-0.61 us) and delete (+0.35
us), `int32x2` move (-0.04 us, 0.07 us above the move band's upper end),
`varchar64` move (+0.07 us) and delete (-0.38 us), `mixed4` update (-0.87
us) and move (-0.58 us), `Wint` update (+0.76 us) and delete (+0.35 us), and
the two wide keys' ranges (`Wint` +3.73 us, +2.8 %; `Wstr` +4.20 us,
+3.2 %). The ranges of 100 rows by a wide key are `IndexRange` over 101
pages, and their floor comes from two cells (F0 and F1, whose own three
rounds span 45.00 to 48.21 us and 49.91 to 52.02 us here), so the range
floor is the least certain of the six. Deltas on the reads (the
`824acfbed` run 2's cells against these) are -0.60 to +0.18 us, all inside.
The statements that none of this commit's insert path is on (read, update,
delete, move by a key) move in both directions with no pattern by form
(`int32` update -0.61 us and delete +0.35 us), which is what the floor's
own spread looks like; this run cannot say whether any of those 11 is more
than that spread.

Leaf divisions, data file and the stored counters are identical to the
`-123` run 2's, round by round: F0 894, 879, 834 and F1 978, 979, 959
divisions; `Wint` 1,007, 1,032, 1,043 and `Wstr` 1,977, 1,993, 2,005
divisions; data files 8,912,896 B (F0, round 2 8,388,608 B), 9,437,184 B
(F1), 12,058,624 B (`Wint`, round 2 12,582,912 B) and 24,117,248 B
(`Wstr`); the F2 reads' counters line is `IndexProbe ... pages=1` for both
wide keys and the btree forms' is `Lookup ... pages=1`. The row layout and
the placement are as before, by those counters.

Latency (client), median over rounds of each percentile, run 2:

| form | phase | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| ref_heap | read | 20,000 | 37.3 us | 39.6 us | 40.3 us | 47.9 us | 56.2 us |
| ref_heap | update | 4,000 | 37.4 us | 39.9 us | 40.6 us | 52.4 us | 72.9 us |
| ref_heap | delete | 4,000 | 34.5 us | 37.0 us | 37.9 us | 46.0 us | 59.1 us |
| int32 | read | 20,000 | 38.1 us | 41.0 us | 42.0 us | 48.3 us | 58.0 us |
| int32 | update | 4,000 | 38.9 us | 42.0 us | 43.1 us | 52.6 us | 72.0 us |
| int32 | move | 4,000 | 41.7 us | 46.4 us | 47.9 us | 62.3 us | 88.8 us |
| int32 | delete | 4,000 | 36.1 us | 39.6 us | 41.0 us | 51.8 us | 70.5 us |
| int32x2 | read | 20,000 | 37.4 us | 42.6 us | 43.4 us | 50.5 us | 59.9 us |
| int32x2 | update | 4,000 | 40.8 us | 43.9 us | 45.1 us | 58.3 us | 81.7 us |
| int32x2 | move | 4,000 | 44.0 us | 48.2 us | 49.6 us | 62.7 us | 85.1 us |
| int32x2 | delete | 4,000 | 37.4 us | 40.9 us | 41.9 us | 52.3 us | 71.4 us |
| varchar64 | read | 20,000 | 38.4 us | 41.1 us | 41.8 us | 47.2 us | 56.5 us |
| varchar64 | update | 4,000 | 40.1 us | 42.8 us | 43.9 us | 55.6 us | 75.2 us |
| varchar64 | move | 4,000 | 43.7 us | 47.4 us | 48.8 us | 63.1 us | 91.2 us |
| varchar64 | delete | 4,000 | 36.8 us | 39.7 us | 40.6 us | 50.8 us | 68.0 us |
| mixed4 | read | 20,000 | 39.4 us | 45.3 us | 46.0 us | 53.8 us | 66.6 us |
| mixed4 | update | 4,000 | 43.9 us | 46.5 us | 47.4 us | 58.5 us | 83.0 us |
| mixed4 | move | 4,000 | 46.9 us | 50.8 us | 52.1 us | 67.2 us | 95.9 us |
| mixed4 | delete | 4,000 | 40.7 us | 43.4 us | 44.3 us | 55.5 us | 75.8 us |
| F0 | read | 20,000 | 36.4 us | 38.6 us | 39.1 us | 46.2 us | 54.9 us |
| F0 | range | 1,000 | 67.9 us | 72.4 us | 74.2 us | 88.8 us | 106.8 us |
| F0 | update | 4,000 | 36.5 us | 38.7 us | 39.3 us | 47.3 us | 63.0 us |
| F0 | delete | 4,000 | 34.3 us | 36.3 us | 36.9 us | 44.4 us | 59.2 us |
| F1 | read | 20,000 | 35.4 us | 40.3 us | 40.9 us | 47.3 us | 56.7 us |
| F1 | range | 1,000 | 71.0 us | 76.1 us | 78.3 us | 97.6 us | 109.4 us |
| F1 | update | 4,000 | 37.9 us | 40.2 us | 40.9 us | 51.7 us | 70.4 us |
| F1 | delete | 4,000 | 35.6 us | 37.8 us | 38.5 us | 47.5 us | 62.4 us |
| Wint | read | 20,000 | 39.0 us | 41.8 us | 42.4 us | 47.7 us | 56.7 us |
| Wint | range | 1,000 | 147.6 us | 158.7 us | 162.6 us | 198.4 us | 257.7 us |
| Wint | update | 4,000 | 40.2 us | 43.5 us | 45.1 us | 58.3 us | 80.2 us |
| Wint | delete | 4,000 | 36.9 us | 40.6 us | 41.9 us | 52.0 us | 68.7 us |
| Wstr | read | 20,000 | 38.1 us | 40.3 us | 40.9 us | 47.7 us | 56.6 us |
| Wstr | range | 1,000 | 142.3 us | 159.5 us | 163.7 us | 204.6 us | 274.1 us |
| Wstr | update | 4,000 | 38.7 us | 41.6 us | 42.5 us | 52.4 us | 74.6 us |
| Wstr | delete | 4,000 | 36.5 us | 39.0 us | 39.8 us | 49.1 us | 68.3 us |

## 3. F2 against P1, as measured at `824acfbed`

Not re-measured at this commit. The P1 binary
(`/home/louis/dr-bench/close/kds_server_P1_bff54d524`, built from
`bff54d524`) does not carry the change of `e0d86ec62`, so a wide key's load
on the two binaries would differ by that change as well as by the form, and
the pair would not isolate the form. The table below is the `-123` file's
section 2 (runs 2 and 3 there, both binaries without the change), repeated
for the two load percentages and the two read deltas only; the rest of that
section stands in that file.

| form | statement | P1 binary | F2 | F2 - P1 | F2 vs P1 | measured at |
|---|---|---|---|---|---|---|
| Wint | load | 8.35 us/row | 9.81 us/row | +1.46 us/row | +17.5 % | `824acfbed` |
| Wstr | load | 8.31 us/row | 9.87 us/row | +1.56 us/row | +18.7 % | `824acfbed` |
| Wint | read | 28.63 us | 25.85 us | -2.78 us | -9.7 % | `824acfbed` |
| Wstr | read | 26.09 us | 24.51 us | -1.58 us | -6.1 % | `824acfbed` |

At `e0d86ec62` the F2 loads are 9.87 us/row (`Wint`, [9.72-9.88]) and 9.90
us/row (`Wstr`, [9.86-10.22]) in run 2 above; no P1 figure exists at this
commit to set them beside.

## 4. What this run teaches about the engine

- **The loads outside the control in the previous run lie inside it at this
  commit, on both relations.** B - A is -0.01 and -0.08 us/row here against
  +0.18 and +0.13 us/row at `824acfbed`; the same sign change shows in the
  parent's diagnostic B6. The remedy named the per-row catalog scan, and
  the cells that carry no other change between the two commits (F0, F1,
  `ref_heap`, `int32`, `int32x2`, `mixed4`) read 0.08 to 0.31 us/row lower
  on the load. This run measures the effect of the commit, not
  the scan alone.
- **The F2 load did not move with the others.** `Wint` and `Wstr` load at
  9.87 and 9.90 us/row, +0.07 and +0.03 us/row against `824acfbed`, while
  F0 and F1 dropped 0.31 and 0.23 us/row. This run does not separate why;
  the driver times the statement as a whole.
- **The btree `walked` phase is still 4 % below A** (-277.46 us), with a
  control that does not reach it; it is unexplained for lack of a counter.

## 5. Wait breakdown

Does not apply. One session on one connection, `cores = 1`, `relaxed`
durability (no commit fsync), pages resident in a pool of 131,072 frames (no
read waits), no concurrent writer (no lock wait): every figure above is
server CPU a statement, and the client latency is that CPU plus the socket
round trip. Baseline: this engine's own previous number is the `-123`
file's run 2 for every cell of section 2, the opening commit's binary
measured inside run 1 for section 1, and the `-123` file's runs 2 and 3 for
the F2 against P1 pair. The PostgreSQL floor was not measured for this
shape.

## 6. What is unverified

| not measured | why it matters | what would check it |
|---|---|---|
| who drove the 15-round run | section 1a2's run was driven by the parent session, not by this agent; this file's checks of it are from its JSON and heartbeat (rounds, arm order, binary hashes, quiet-gate holds, times), and the unchecked parts are the process the parent ran, whether anything else ran on the host beyond the loads and `cc1plus` counts recorded in the cells, and the command line beyond the `args` stored in the JSON | re-run it from this session |
| F2 against P1 at this commit | not re-measured: the P1 binary lacks the change, so the pair would differ by it as well as by the form (section 3) | rebuild the P1 source with this commit's `ForRowOfOid` change and run `built` on both |
| the between-run floor at this commit | the floor of section 2 is the `-123` file's run 3 minus run 2, adjacent runs of one path; run 2 here and the `-123` run 2 are 15 minutes apart on different binaries, so the floor may understate or overstate their spread | repeat `built` on B twice in sequence and take its own run 3 minus run 2 |
| the range floor | it rests on two cells (F0, F1) | more same-path cells with a range phase, or repeated runs |
| `cores` above 1 | catalog scans and the home's row unit under two cores | the `built` part at `cores = 2` and above |
| a relation larger than the pool | every page is resident; the insert's catalog scan is memory-only here | `--frames` below the working set |
| 1,000,000 rows, and the 200- and 10,000-row sweep of the benchmark rules | one cardinality, 100,000: a fixed cost and a per-row one are not separated, and the per-row scan's cost may depend on the catalog's size | `--rows 1000000`, `--rows 200`, `--rows 10000` |
| concurrent writers on one key, two sessions racing one home | one session only | a two-session driver |
| rows off their homes, a read inside a read-write transaction | not exercised by the driver (`-123` file, section 6) | a driver that re-inserts, moves or reads inside `BEGIN ... READ WRITE` |
| instructions per statement | no hardware counters (`perf` is absent), so the btree `walked` move and the 11 non-load cells outside the floor are not split into instructions and layout | a host with `perf stat` |
| the layout spread of the opening commit's own source | no third arm built from `e9df68d02` with a perturbed layout | rebuild `e9df68d02` with a changed link order and run it as A3 |
| the catalog's size at the load | the scan walks `sys.tables` to the relation's row; the catalog holds few relations in every cell here, so the cost on a database with many relations is not seen | a load into a relation created after hundreds of others |
