# DS close: single-row and multi-row INSERT, base against closing commit

## Answer

**DS's code change did not move the server CPU of a single-row `INSERT` or of
a multi-row `INSERT`; no delta reaches the control.** Server CPU a statement,
close (B) minus base (A), 100,000-row relations, 5 rounds, two independent
runs (section 2):

| phase | relation | run 1: B - A | run 2: B - A | control A2 - A, both runs | verdict |
|---|---|---|---|---|---|
| single-row `INSERT` | btree | +0.09 µs/statement (+1.4 %) | +0.01 µs/statement (+0.2 %) | +0.18 to +0.13 µs/statement (+2.8 %, +2.0 %) | inside the control |
| single-row `INSERT` | heap | +0.11 µs/statement (+1.4 %) | +0.01 µs/statement (+0.1 %) | +0.28 to +0.26 µs/statement (+3.4 %, +3.2 %) | inside the control |
| multi-row `INSERT` (500 rows a statement) | btree | +0.005 µs/row (+0.4 %) | +0.003 µs/row (+0.2 %) | +0.002 to +0.001 µs/row (+0.2 %, +0.1 %) | inside (both deltas are at the control's size, 0.005 µs/row, and are not called a finding) |
| multi-row `INSERT` (500 rows a statement) | heap | +0.019 µs/row (+0.6 %) | +0.010 µs/row (+0.3 %) | +0.020 µs/row (+0.7 %), +0.016 µs/row (+0.5 %) | inside the control |

The source change agrees: `git diff 8274c951e cc46c0d85 -- src include` touches
the `INSERT` path only on a refusal (`AtValue` on `EncodeRow`'s failure return,
`row_codec.cpp`), in the `INSERT ... SELECT` row loop of
`CommandDispatcher::InsertSelectRows`, in a rename arm and in the archive
command's timeout (`expeditor.cpp`, `wal_archive.cpp`). A successful row takes
none of those lines, which is what the measurement shows. The one cell outside
the control is not an `INSERT` (section 4).

## Stamp

| item | value |
|---|---|
| executed | 2026-11-26 23:06:42 to 23:12:04 UTC (run 1 23:06:42-23:07:44, run 2 23:09:37-23:10:41, 10,000-row run 23:11:28-23:12:04); builds 23:03 to about 23:06 UTC |
| branch / commit | `worktree-ds-known-gaps-contained` at `cc46c0d85` (close); base `8274c951e` |
| `git describe --tags` | `v3.4.17-151-gcc46c0d85` (this clone has no `v3.5.0` tag, so the describe reads `v3.4.17-...`) |
| tree | the ordering session edits documents (`docs/`, `instructions/`, `manual/`, `CLAUDE.md`) in the worktree during the run and changes no source or build file; `git status --short` at the moment B was built (23:06:42 UTC): ` M CLAUDE.md`, ` M docs/inflight/known-gaps.md`, nothing else, and the same two lines after the runs |
| work order | `instructions/v3.2.0/workorder-ds-contained-known-gaps.md` section 4, DS-Q7 |
| arm A (base) | `build-release/ds-close-bin/kds_server.base`, built from `git archive 8274c951e` in its own Release tree (`build-release/ds-close-build-a`), sha256 `9f90e618a7847c1f1f514ff7bcaf78f2044457bbba5c126973990d25642a1714`, source mtime 2026-11-26 18:03:49 -0500 (HEAD's commit date is 09:22 -0500 that day, so the binary is newer than HEAD) |
| arm A2 (control) | the driver's `.control` copy of A, another inode, the same engine (driver hash `9f90e618a7847c1f` for A and A2) |
| arm B (close) | `build-release/ds-close-bin/kds_server.close`, built from the worktree at `cc46c0d85` (Release, `build-release`), sha256 `9c428743437fe565e8ebb621761e6ec64261a3efed62257f45b707a1e280d6f0`, source mtime 2026-11-26 18:03:48 -0500 |
| build | Release for both (`CMAKE_BUILD_TYPE:STRING=Release` in both `CMakeCache.txt`), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`; Debug not measured |
| device | `/dev/mapper/fedora-root`, xfs (`df -T $HOME`); data files and WAL under `build-release/ds-close-run/`, not tmpfs |
| server config | the driver's: `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, server pinned to CPU 2, driver to CPUs 0,4, one fresh server and data file a cell, ports 31980 and 31981 (checked free with `ss -ltn`) |
| driver | `tools/dr_close.py --part ab` unmodified; run 1 and run 2 `--rows 100000 --rounds 5` (defaults), the 10,000-row run `--rows 10000 --reads 5000 --writes 2000 --walks 40 --ranges 1000 --rounds 5` |
| host state | 24 CPUs. Quiet gate: run 1 held 5.0 s at its start (a `cc1plus` was present, 1-minute load 4.45; no process of this run's builds was running), runs 2 and 10,000-row 0.0 s; after that `cc1plus` count 0 at every cell's start. 1-minute load per cell 3.46 to 4.09 in run 1's first round falling to 2.2, 0.48 to 1.44 in run 2 (the driver's own server and client included) |
| PostgreSQL floor | not measured for this shape |
| raw output | `bench/v3.5.0/archive/ds-close-v3.4.17-151-gcc46c0d85/` (`out.json`, `out2.json`, `out-rows10000.json`, driver logs, heartbeats) |

## 1. What was measured

`tools/dr_close.py --part ab` runs three arms a round, in an order rotated per
round: A the base binary, A2 a second copy of A (the control), B the close. In
each cell, on a btree relation (`CREATE TABLE tb (id int64, v int64, w int64)
BTREE`) and a heap one (`th`, `id int64 PRIMARY KEY ... HEAP`) of `--rows` rows:

| phase | statement | rows a statement | reported as |
|---|---|---|---|
| `load` (the multi-row `INSERT`) | `INSERT INTO t (id, v, w) VALUES (..), (..) ...`, 500 rows a statement, keys in order | 500 | server CPU a row, whole phase (`µs/row`) |
| `insert` (the single-row `INSERT`) | `INSERT INTO t (id, v, w) VALUES (n+1+j, j, 0)`, 4,000 statements | 1 | median of 20 batches' server CPU a statement (`µs/statement`) |

The other phases (point read, key `UPDATE`, walked `UPDATE`, 100-row range, key
`DELETE`) are in section 4. Server CPU is the server process's CPU (user plus
kernel) from `/proc`, settled before each read.

## 2. Results, run 1

`µs/statement` (`µs/row` for `load`), median of 5 rounds, [min-max]. B - A and
the control's A2 - A are medians' differences. Cells are server CPU, derived by
the driver from process CPU; they are time costs, not a matrix of throughput,
because a CPU a statement is the quantity DS-Q7 asks for (the rate form is
`1,000,000 / CPU` statements per CPU-second: 158,000 for the btree `insert`
at A, 123,000 for the heap `insert`).

| relation | phase | A | A2 (control) | B | B - A | A2 - A | within control |
|---|---|---|---|---|---|---|---|
| btree | `load` | 1.215 µs/row [1.212-1.231] | 1.217 µs/row [1.213-1.248] | 1.220 µs/row [1.217-1.224] | +0.005 µs/row (+0.4 %) | +0.002 µs/row (+0.2 %) | yes (below 0.01 µs/row) |
| btree | `insert` | 6.32 µs [6.31-6.44] | 6.50 µs [6.39-6.59] | 6.41 µs [6.31-6.48] | +0.09 µs (+1.4 %) | +0.18 µs (+2.8 %) | yes |
| heap | `load` | 2.965 µs/row [2.950-3.001] | 2.985 µs/row [2.977-3.048] | 2.984 µs/row [2.944-2.996] | +0.019 µs/row (+0.6 %) | +0.020 µs/row (+0.7 %) | yes |
| heap | `insert` | 8.13 µs [8.08-8.29] | 8.41 µs [8.25-8.46] | 8.24 µs [8.07-8.29] | +0.11 µs (+1.4 %) | +0.28 µs (+3.4 %) | yes |

Run 2 (5 rounds, separate servers and data files, same binaries):

| relation | phase | A | A2 (control) | B | B - A | A2 - A |
|---|---|---|---|---|---|---|
| btree | `load` | 1.217 µs/row [1.211-1.230] | 1.218 µs/row [1.217-1.229] | 1.220 µs/row [1.210-1.244] | +0.003 µs/row (+0.2 %) | +0.001 µs/row (+0.1 %) |
| btree | `insert` | 6.42 µs [6.31-6.47] | 6.55 µs [6.40-6.59] | 6.43 µs [6.29-6.52] | +0.01 µs (+0.2 %) | +0.13 µs (+2.0 %) |
| heap | `load` | 2.968 µs/row [2.965-3.052] | 2.984 µs/row [2.963-3.018] | 2.978 µs/row [2.947-3.074] | +0.010 µs/row (+0.3 %) | +0.016 µs/row (+0.5 %) |
| heap | `insert` | 8.22 µs [8.09-8.24] | 8.48 µs [8.33-8.50] | 8.23 µs [8.06-8.32] | +0.01 µs (+0.1 %) | +0.26 µs (+3.2 %) |

The control is not zero and is one-sided: A2 reads above A on every `insert`
cell in both runs (+2.0 % to +3.4 %), the signature of a second copy of the
binary (another inode and layout) and not of the engine. B sits between A and
A2 on all four `insert` readings and below A2 on two of four `load`
readings (the heap's; on the btree it is 0.002-0.003 µs/row above A2), so B - A is not distinguishable from the control's own spread. The
noise floor for a single-row `INSERT` is taken as 3.4 % (0.28 µs on the heap,
0.18 µs on the btree) and for `load` as 0.7 % (0.02 µs/row on the heap).

### 10,000-row relations

A smaller relation, run once (5 rounds, `--writes 2000`, so 2,000 statements
and 20 `load` statements of 500 rows). The control moves more here (the `load`
is 10,000 rows in 20 statements, a third the sample of the 100,000-row run).

| relation | phase | A | A2 (control) | B | B - A | A2 - A |
|---|---|---|---|---|---|---|
| btree | `load` | 1.424 µs/row [1.272-1.546] | 1.347 µs/row [1.302-1.495] | 1.367 µs/row [1.238-1.432] | -0.057 µs/row (-4.0 %) | -0.077 µs/row (-5.4 %) |
| btree | `insert` | 6.52 µs [6.40-6.71] | 6.79 µs [6.60-6.90] | 6.66 µs [6.46-7.43] | +0.14 µs (+2.1 %) | +0.27 µs (+4.1 %) |
| heap | `load` | 2.887 µs/row [2.830-2.908] | 2.894 µs/row [2.876-2.909] | 2.877 µs/row [2.868-2.932] | -0.010 µs/row (-0.3 %) | +0.007 µs/row (+0.2 %) |
| heap | `insert` | 8.49 µs [8.28-8.71] | 8.81 µs [8.55-8.90] | 8.39 µs [8.34-8.67] | -0.10 µs (-1.2 %) | +0.32 µs (+3.8 %) |

All four are inside the control (the btree `load` spread, 0.27 µs/row from
min to max across rounds, is wider than any B - A).

## 3. Client latency of the single-row `INSERT`

The client-observed latency of `insert`, median across the 5 rounds of each
round's percentile, run 1 (4,000 statements a round and arm), `µs`:

| relation | arm | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| btree | A | 4,000 a round | 12.1 µs | 12.6 µs | 12.7 µs | 13.6 µs | 16.5 µs |
| btree | A2 | 4,000 a round | 12.3 µs | 12.8 µs | 13.0 µs | 13.9 µs | 16.7 µs |
| btree | B | 4,000 a round | 12.5 µs | 13.0 µs | 13.1 µs | 13.9 µs | 16.4 µs |
| heap | A | 4,000 a round | 13.7 µs | 14.3 µs | 14.5 µs | 15.5 µs | 21.4 µs |
| heap | A2 | 4,000 a round | 14.0 µs | 14.8 µs | 15.0 µs | 16.0 µs | 22.2 µs |
| heap | B | 4,000 a round | 13.9 µs | 14.7 µs | 14.9 µs | 15.9 µs | 21.5 µs |

Run 2: btree A 12.5/13.0/13.2/14.1/21.3, A2 12.3/12.9/13.0/13.9/16.5, B
12.2/12.7/12.9/13.7/17.2; heap A 13.6/14.5/14.7/15.8/22.2, A2
14.2/14.9/15.1/16.1/23.5, B 13.7/14.4/14.6/15.7/21.7 µs (p0/p25/p50/p95/p99).
B is within 0.5 µs of the nearer of A and A2 at every percentile: above both
at the btree run-1 p0 to p50 (+0.4 µs over A), below both at the btree run-2
p0 to p95 and the heap run-2 p25 to p99 (0.1 to 0.5 µs), at or between them
elsewhere but the btree run-1 p99 (0.1 µs below A); the tight body (p25
to p50 within 0.2 µs) and the p95 within 1 µs of p50 say the path is a fixed
cost, and the 0.3 to 0.4 µs spreads between arms are at the control's size. The
`load` phase reports a whole-phase CPU a row, not a latency distribution, so it
carries no percentile table.

### Where the time goes (a wait breakdown)

Derived, single-row `INSERT`, run 1, btree A: p50 client latency 12.7 µs, of
which 6.3 µs is server CPU (50 %) and the remaining 6.4 µs (50 %) is client
and socket round trip plus scheduling, which the driver does not split. The
heap is 14.5 µs, of which 8.1 µs (56 %) is server CPU. Durability is
`relaxed`, so a commit waits for no fsync; no lock wait exists (one
connection, one core, a key of its own); page reads do not occur (every page
resident). Waits other than these were not decomposed by this driver. `strict`
was not measured, and an `INSERT`'s refusal path, the only one DS changed, is
not exercised by a driver that never errors.

## 4. The other phases, outside the control or not

Server CPU, µs/statement, run 1 and run 2 together (median of 5 rounds each,
B - A vs A2 - A):

| relation | phase | A (run 1) | B - A, run 1 | A2 - A, run 1 | B - A, run 2 | A2 - A, run 2 | outside the control |
|---|---|---|---|---|---|---|---|
| btree | `read` (point) | 6.17 µs | -0.01 µs (-0.2 %) | +0.12 µs (+1.9 %) | 0.00 µs (0.0 %) | +0.13 µs (+2.1 %) | no |
| btree | `update` (by key) | 6.86 µs | +0.09 µs (+1.3 %) | +0.09 µs (+1.3 %) | +0.18 µs (+2.6 %) | +0.21 µs (+3.1 %) | no |
| btree | `walked` (UPDATE, no key) | 3,295.6 µs | -24.7 µs (-0.7 %) | +4.4 µs (+0.1 %) | -23.8 µs (-0.7 %) | -1.6 µs (0.0 %) | **yes, both runs** |
| btree | `range` (100 rows) | 15.63 µs | +0.06 µs (+0.4 %) | +0.04 µs (+0.3 %) | +0.02 µs (+0.1 %) | +0.16 µs (+1.0 %) | no |
| btree | `delete` (by key) | 6.03 µs | +0.03 µs (+0.5 %) | +0.10 µs (+1.7 %) | -0.02 µs (-0.3 %) | +0.14 µs (+2.3 %) | no |
| heap | `read` (point) | 6.78 µs | +0.02 µs (+0.3 %) | +0.06 µs (+0.9 %) | 0.00 µs (0.0 %) | +0.02 µs (+0.3 %) | no |
| heap | `update` (by key) | 7.30 µs | +0.10 µs (+1.4 %) | +0.14 µs (+1.9 %) | +0.07 µs (+1.0 %) | +0.19 µs (+2.6 %) | no |
| heap | `walked` (UPDATE, no key) | 3,274.2 µs | -6.1 µs (-0.2 %) | +5.6 µs (+0.2 %) | -24.7 µs (-0.8 %) | +0.4 µs (0.0 %) | run 2 only, run 1 inside |
| heap | `range` (100 rows) | 25.10 µs | +0.37 µs (+1.5 %) | +0.11 µs (+0.4 %) | +0.33 µs (+1.3 %) | +0.34 µs (+1.4 %) | run 1 only, run 2 inside |
| heap | `delete` (by key) | 6.50 µs | -0.01 µs (-0.2 %) | +0.10 µs (+1.5 %) | -0.02 µs (-0.3 %) | +0.17 µs (+2.6 %) | no |

One reading falls outside the control and repeats: the **walked `UPDATE`** (a
1,000-value-per-row walk, 100 rows written a statement, no key) is 0.7 % to
0.8 % cheaper on B in three of four cells (24 µs of about 3,300 µs, the control
at 0.0 % to 0.2 %). It is a speed-up, 0.7 %, in a statement DS did not touch on
any line the diff names (the walk's `UPDATE` write goes through neither
`EncodeRow`'s refusal nor `InsertSelectRows`), and it is not explained: the
binaries differ in layout (194 source lines added and 14 removed in `src` and
`include`), which could shift code placement; no cause was tested. It is reported as unresolved, not as a gain.
The heap's 100-row `range` is the other reading outside a control, by the
same per-run rule: +1.5 % on B in run 1 against a +0.4 % control, +1.3 % in
run 2 inside its +1.4 %; a statement DS does not touch either, unexplained.

## 5. Against this engine's previous number

The same driver's last run is `bench/v3.5.0/results-dr-close-v3.5.0-124-ge0d86ec62.md`,
measured on a different host (`/home/louis/...`) at a different commit. Its
absolute figures are not comparable to these: its btree `insert` read 22.66 µs
and its btree `load` 3.46 µs/row, against 6.32 µs and 1.215 µs/row here (3.6x
and 2.8x slower, a host difference and not an engine one), and a delta across
hosts would be the hosts' difference. **There is no same-host previous number
for these shapes, so this run's A column is the baseline the next run on this
host is read against** (btree `insert` 6.32 µs, heap `insert` 8.13 µs, btree
`load` 1.215 µs/row, heap `load` 2.965 µs/row, at `8274c951e`; B at
`cc46c0d85`: 6.41, 8.24, 1.220, 2.984).

## 6. What this teaches about the engine

- A single-row `INSERT` costs 6.4 µs (btree) or 8.2 µs (heap) of server CPU on
  one core, `relaxed`. Inside a 500-row statement a row costs 1.2 µs (btree) and 3.0
  µs (heap): the per-statement fixed cost is about 5.2 µs on the btree and about
  5.2 µs on the heap (6.4 minus 1.2; 8.2 minus 3.0), the same fixed cost, so the
  per-statement fixed part does not separate the two storages and the heap's
  1.8 µs more a single-row statement is its row's. Derived from the two
  phases above, not measured separately.
- DS's refusal-path changes cost nothing on the accepting path: `AtValue`'s
  tests run only after `EncodeRow` has already failed.

## 7. What was cut or not verified

- **Row-set sweep (rule 9):** run at 100,000 and 10,000 rows. 200 and 1,000
  rows were attempted and the driver refused them (`dr_close.py` samples 2,000
  distinct keys for its warm-up and expects `UPDATED n // 1000` from its walked
  update, so it cannot run below about 2,000 rows); it was not patched, per the
  order. A single-row `INSERT` does not scale with the relation's rows beyond
  tree depth, and 100,000 against 10,000 rows reads 6.32 against 6.52 µs
  (btree, A) and 8.13 against 8.49 µs (heap, A), but this is two points, not a
  fit.
- **Rounds:** 5 a run, 2 runs at 100,000 rows and 1 at 10,000, as the driver's
  default; no more than that were run, so the control has 5 samples a run.
- **Tests:** the correctness suite was not run for this measurement (no code
  was changed here, and the builds exclude tests).
- **`strict` durability, the refusal path's cost and `INSERT ... SELECT`** (the
  loop DS changed in `InsertSelectRows`) were not measured: the driver has no
  such phase, and DS-Q7 names a one-row and a multi-row `INSERT`.
- The walked `UPDATE`'s 0.7 % advantage (section 4) is unexplained.

## 8. Time spent

| item | time |
|---|---|
| quiet-gate hold | 5.0 s (run 1 start); 0 s elsewhere |
| builds (A and B in parallel, `-j12` each, Release, `kds_server` only) | about 3 minutes |
| runs | 61.7 s (run 1), 64.5 s (run 2), 35.9 s (10,000 rows), plus two failed attempts at 200 and 1,000 rows (a few seconds each) |
