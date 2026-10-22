# BL close: the levels cost nothing measurable on the statement path, and a declared bulk writer no longer takes a transactional reader's p99 to 287 ms, but to 2-10 ms - not the 2x-isolated bar

**Thesis.** Measured on `worktree-bl-s4-partition` at `f4bad273`
(`v3.4.1-10-gf4bad273`, **B**), BL's close:

- **Statement path (bar (i)): within run-to-run noise.** B against BL-S4's
  parent `0af06bbc` (`v3.4.1-4-g0af06bbc`, **A**) and against B with the
  balancer switched off (**N**, an uncommitted measurement build), at
  `cores = 1` and `cores = 4`, ten interleaved servers per arm. Every p50
  delta's rep ranges overlap. At `cores = 1` the deltas are -1.1 % to +0.5 %.
  At `cores = 4` B is +0.1 % to +1.8 % over A and +0.2 % to +1.7 % over N on
  the single-row statements - a consistent sign, inside the spread, which
  prices the per-head decision at well under a microsecond. Scans are -2.1 %
  to +0.5 %.
- **Mixed cell with levels (bars (iii) and (v)), `cores = 4`, the balancer
  placing.** Six `transactional` point readers at 400/s in total, an
  `analytic` 10,000-row walker and a `background` 1,024-row inserter:
  - **Partition fidelity (iii) holds** at every run's end: both batch
    sessions on core 0 and no reader there, in 3 of 3 runs.
  - **Reader schedule p99: 1.9 / 6.5 / 9.6 ms** (median 6.5 ms), against
    0.47 ms isolated and **287 ms** with the same load declaring nothing.
    **Bar (v), at most 2x the isolated p99, is not met** (14x at the median).
    The p99.9 is 138-239 ms, against 437 ms undeclared.
  - **The excess is not a `normal` session sharing a reader's core** - there
    is none in this cell - so under BL-R7 it opens no rung 3 and is reported
    as BL-R7's remainder. It lines up with roll waits on the reader-only
    cores 2 and 3: 0.16-0.28 s per 10 s, the waiter the BL ruling-2 file
    left unidentified.
  - **The partition's cost is the batch sessions'.** The walker completes
    1,002-1,025 statements, against 20,204-21,205 undeclared, because it
    shares core 0 with the non-yielding inserter. The inserter is
    unchanged (174-177k rows/s, against 173-174k).
- **The 5-rep statement rotation's one outlier did not reproduce.** In the
  first rotation (5 reps), B's tuple-key SELECT at `cores = 4` over 10,000
  rows had 3 of 5 reps at 80-94 us. Ten reps, run again from the start,
  put B at 64.5 us against 64.2 for both A and N, every rep at 63.4-65.7.
  Both rotations are in the archive.

- **Order**: `instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`
  §5.2, §6 ("BL-S5 measured 2026-10-22"); `instructions/v3.2.0/raft-marks-2026-10-21.md` §15.
- **Drivers**: `tools/a1_close_benchmark.py --mode stmt` (the A1 close's
  statement driver, unchanged) and `tools/bl_mixed.py` at `ee6d74a0`
  (`--levels` added for this close: each role sends `SET PRIORITY` before
  the window). Run by `s5-stmt.sh`, `s5-stmt10.sh`, `s5-mixed.sh` and
  `s5-run.sh` in the archive.
- **Raw output**: `archive/bl-close-v3.4.1-10-gf4bad273/`. It holds
  `stmt/` (5 reps), `stmt10/` (10 reps, the reported one) and `mixed/`
  (driver JSON and server logs), `progress.txt` (every cell's start and end,
  load and compiler count), `nobal.patch`, the build script and the table
  scripts with their rendered output (`tables-stmt10.md`,
  `tables-stmt5.md`, `tables-mixed.md`).

| Item | Value |
|---|---|
| Executed | 2026-10-22 UTC. Builds 09:33:30-09:37:54. Statement rotation (5 reps) 09:45:35-09:46:32, after the quiet gate waited 275 s for another session's test suite; mixed cells 09:46:32-09:49:48; statement rotation (10 reps) 09:50:01-09:51:52 |
| Versions | **A** `0af06bbc` (`v3.4.1-4-g0af06bbc`, BL-S4's parent), exported with `git archive` and built in a scratch directory. **B** `f4bad273` (`v3.4.1-10-gf4bad273`, BL-S4 merged with `origin/main`; B carries one other session's change, `8591af46`, an assertion-admission fix no cell here reaches). **N** = B plus `nobal.patch` (`LoadPlacer::Place` returns the session's core when `KDS_BL_NO_BALANCE` is set), never committed, source restored after the build |
| Binaries | Release, built this session after their commits, each run from a copy: `kds_server.0af06bbc` sha256 `60723ba6d4333cff1eeae0b9019febf77e3c1b4854255af222fae05947b0831d`; `kds_server.f4bad273` `3323c0a73f47560055820e69eda2196c6da446c954fd60226811070f376eb3d3`; `kds_server.f4bad273-nobal` `947d63aea93c14b8c0d18129a16eabaeb24b5454abcfbc0ec664e9352d875be3` |
| Host / device | 8 vCPU AMD EPYC 9V74, Linux 7.0.0-1014-azure; data and WAL on `/dev/root`, ext4, not tmpfs, 91 % full. Statement cells unpinned; mixed cells with servers on CPUs 0,2,4,6 and clients on 1,3,5,7. **The quiet gate** before every cell: no `kds_tests` and no `cc1plus` process, and a 1-minute load under 2 (another session's suite was running when the run began; `progress.txt` records each cell's load) |
| Server configuration | Statement cells: `cores` 1 or 4, `durability = relaxed`, `buffer_pool_frames = 131072`, `log_level = warn`, one fresh server and data file per cell. Mixed cells: `cores = 4`, `relaxed`, `buffer_pool_frames = 65536`, `log_level = info`, `reactor_cpus = 0,2,4,6` |
| Statement cell | One connection, autocommit: at 200, 1,000 and 10,000 preloaded rows of a fresh 5-column BTREE relation, 1,000 single-row INSERTs, 1,000 tuple-key SELECTs, 1,000 single-row UPDATEs and 20 unsorted whole scans (the A1 close's cell). Arm order rotated per rep: `cores = 1` A B / B A; `cores = 4` A B N / B N A / N A B |
| Mixed cell | C1 as in `results-bl-ruling2-remeasure-v3.4.0-22-g418670a3.md` §0, placement `kernel` (the balancer places), `--ctl-core -1` (a control session pinned to core 0 would be moved off it by the partition): arms `isolated` (readers only, `transactional`), `levels` (readers `transactional`, walker `analytic`, inserter `background`) and `none` (nothing declared), 3 runs each, interleaved |
| Baseline | This engine's previous number for the mixed cell: `results-bl-ruling2-remeasure-v3.4.0-22-g418670a3.md` (balancer live, nothing declared: reader schedule p99 291-581 ms; today's `none` arm 1.3-292 ms). The PostgreSQL floor was not measured: it has no core placement to compare |
| Suite | The full suite (`ctest -LE bs-nightly`, Debug) ran 4295/4295 at `f4bad273` before this measurement; no code changed for it |

## 1. What was run, what was cut

- **Run**: §5.2's overhead A/B (bar (i)), its partition fidelity (bar
  (iii), at each run's end, from the driver's per-session cores), and its
  mixed cell (bar (v)) with the readers `transactional`.
- **What the A/B can and cannot price.** BL's milestone opened from
  `a3208be7` on 2026-10-12; BL-S1..S3 (the load words, the movable
  connection, the balancer) landed then, and 60-odd unrelated commits and
  several superblock bumps lie between that commit and B. An A/B over the
  whole milestone would price them all. So the A/B is split:
  - **A -> B** prices BL-S4 (the level, its seat, the side test, the slice
    read, the fallback accept) and one unrelated assertion fix;
  - **N -> B** prices the per-head placement decision itself (BL-S3's
    `Place` plus BL-S4's side), at `cores = 4`, where it runs;
  - BL-S1/S2's `unique_ptr<Connection>` and the load-word writes are in
    all three arms and are **not priced** here.
- **Cut, for the hour's budget**:
  - **Convergence (bars (ii) and (iv))**: measured at BL-S3 (`bench/v3.0.0/results-bl-s1-*`
    and the order's §6) - a skewed start brought to the balanced spread at
    under six moves a second. The undeclared path is BL-S3's: with no
    batch-level session `Decide` reads `Side::kAny`, the code BL-S3 ran.
  - **The second `normal` walker** sharing the interactive cores (§5.2's
    rung-3 probe) and the readers-at-`normal` arm (identical placement to
    `transactional` in v0).
  - **Inversions** (BL-Q12's counter, which BL-S4 did not build).
  - `durability = group`; `cores` 2 and 8; the idle batch capacity (BL-Q20).

## 2. Statement path (ten reps)

A table entry is the median over the ten reps of that percentile, in
microseconds. Every rep of every arm is in `tables-stmt10.md`.

| cores | rows | statement | arm | p0 | p25 | p50 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 200 | insert | A | 42.1 | 44.8 | 45.2 | 53.5 | 68.8 | 101.4 |
| 1 | 200 | insert | B | 42.0 | 44.8 | 45.3 | 52.4 | 68.5 | 83.9 |
| 1 | 200 | point-select | A | 60.5 | 62.0 | 62.7 | 73.2 | 91.5 | 143.2 |
| 1 | 200 | point-select | B | 60.7 | 62.1 | 62.8 | 73.0 | 89.6 | 134.7 |
| 1 | 10000 | update | A | 40.7 | 47.5 | 48.0 | 53.5 | 71.5 | 104.8 |
| 1 | 10000 | update | B | 43.3 | 46.9 | 47.5 | 54.6 | 72.3 | 105.7 |
| 1 | 10000 | full-scan | A | 22,141.8 | 22,371.3 | 22,760.6 | 24,169.8 | 25,786.2 | 25,786.2 |
| 1 | 10000 | full-scan | B | 22,147.5 | 22,449.7 | 22,765.2 | 24,135.2 | 25,300.9 | 25,300.9 |
| 4 | 200 | insert | A | 44.4 | 45.7 | 46.2 | 55.0 | 72.0 | 89.0 |
| 4 | 200 | insert | N | 44.5 | 45.6 | 46.4 | 56.5 | 73.7 | 104.4 |
| 4 | 200 | insert | B | 44.7 | 46.2 | 47.0 | 58.5 | 76.3 | 105.0 |
| 4 | 200 | point-select | A | 61.5 | 62.9 | 63.9 | 74.6 | 92.2 | 141.2 |
| 4 | 200 | point-select | N | 60.7 | 62.2 | 63.5 | 75.2 | 90.6 | 148.8 |
| 4 | 200 | point-select | B | 61.5 | 63.4 | 64.6 | 77.4 | 97.2 | 165.6 |
| 4 | 10000 | point-select | A | 61.5 | 63.3 | 64.2 | 76.0 | 93.3 | 144.6 |
| 4 | 10000 | point-select | N | 61.0 | 63.0 | 64.2 | 76.3 | 95.2 | 149.4 |
| 4 | 10000 | point-select | B | 62.2 | 63.7 | 64.5 | 75.2 | 91.5 | 129.2 |
| 4 | 10000 | update | A | 46.9 | 48.2 | 48.8 | 57.3 | 75.4 | 101.8 |
| 4 | 10000 | update | N | 46.8 | 48.0 | 48.6 | 57.3 | 74.8 | 98.2 |
| 4 | 10000 | update | B | 46.6 | 48.0 | 48.7 | 56.3 | 72.8 | 98.3 |
| 4 | 10000 | full-scan | A | 22,175.0 | 22,475.5 | 22,686.0 | 24,398.8 | 25,552.0 | 25,552.0 |
| 4 | 10000 | full-scan | N | 22,301.2 | 22,546.8 | 22,782.1 | 24,300.8 | 25,630.2 | 25,630.2 |
| 4 | 10000 | full-scan | B | 22,026.5 | 22,243.5 | 22,509.9 | 23,938.3 | 25,362.9 | 25,362.9 |

The full table (every size and statement) and the delta table, with each
pair's rep-range overlap, are `tables-stmt10.md`; **all 36 p50 deltas
overlap**. The widest at `cores = 4` are B over A +1.8 % (200-row insert:
A's reps 45.5-46.6 with one at 65.8, B's 46.4-48.1 with one at 65.4) and B
over N +1.7 % (200-row point-select). B's 200-row insert sits about 0.6 us
above N's in nine of ten reps: this is the size of the per-head decision -
four load slots read from other cores' cache lines and the side computed -
and it is inside the run-to-run spread. **The QPS a client sees** (1e6 /
the median of rep means) moves by the same fractions.

## 3. Mixed cell: the partition with levels

### 3.1 Reader schedule latency (from the instant the open-loop schedule said to send)

| arm | run | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| isolated | r1 / r2 / r3 | 138 / 160 / 132 µs | 178 / 181 / 174 µs | 184 / 192 / 180 µs | 205 / 216 / 205 µs | 632 / 467 / 323 µs | 2.4 / 2.3 / 2.4 ms | 4.8 / 3.8 / 3.8 ms |
| levels | r1 / r2 / r3 | 134 / 135 / 114 µs | 183 / 187 / 180 µs | 194 / 194 / 193 µs | 278 / 268 / 279 µs | **6.5 / 1.9 / 9.6 ms** | 239.2 / 138.4 / 143.5 ms | 269.0 / 197.7 / 158.4 ms |
| none | r1 / r2 / r3 | 132 / 139 / 132 µs | 182 / 194 / 180 µs | 202 / 216 / 200 µs | 151.1 / 188.2 ms / 286 µs | **292.2 / 287.2 / 1.3 ms** | 696.3 / 437.3 / 21.6 ms | 755.9 / 466.8 / 31.5 ms |

Reader **service** latency (from the send) is in `tables-mixed.md`: p99
0.38-0.84 ms with levels, 0.75-3.6 ms undeclared, 0.17-0.28 ms isolated.

### 3.2 Where every session ended, and the batch sessions' work

| arm | run | cores at end (6 readers, walker, inserter) | readers on core 0 | batch sessions on core 0 | moves | inserter rows/s | walker statements |
|---|---|---|---|---|---|---|---|
| levels | r1 | 2, 1, 1, 2, 2, 1, 0, 0 | 0 | 2 | 11 | 176,955 | 1,002 |
| levels | r2 | 1, 2, 2, 3, 1, 1, 0, 0 | 0 | 2 | 8 | 175,254 | 1,018 |
| levels | r3 | 1, 1, 2, 2, 3, 3, 0, 0 | 0 | 2 | 9 | 174,151 | 1,025 |
| none | r1 | 2, 2, 2, 0, 2, 2, 3, 1 | 1 | 0 | 27 | 173,159 | 20,351 |
| none | r2 | 3, 0, 3, 3, 0, 3, 1, 2 | 2 | 0 | 39 | 172,552 | 21,205 |
| none | r3 | 2, 2, 1, 2, 2, 1, 3, 0 | 0 | 1 | 11 | 174,493 | 20,204 |

The one undeclared run with no reader on core 0 (`none` r3) is also the
one at 1.3 ms - the same signature the ruling-2 file found. The partition
moves happen in the warm-up (each core's `load_c<k>_partition_moves_out` at
the window's end: 3-4 on core 0, 0-1 on each other core), and
`batch_sessions` reads 0 after the clients disconnect, which is the seat
given back on close.

### 3.3 What is left (BL-R7's remainder)

With levels, no reader shares a core with a batch session or with core 0's
drain, and still the p99 is 4-21x the isolated one. In every `levels` run
the WAL's roll-blocked wait lands on reader-only cores: 0.27 s on core 3
(r1), 0.28 s on core 3 (r2), 0.17 s on core 2 and 0.16 s on core 3 (r3),
per 10 s, while core 0 - where the inserter rolls - waits 0. A reader core
takes the stream latch while a roll holds it; which of its tasks does (the
checkpoint task rotating over cores is one candidate) is not identified
here. No `normal` session shares those cores, so §5.2 bar (v)'s rule sends
the excess to BL-R7's remainder, not to rung 3.

## 4. The bars

| bar | result |
|---|---|
| (i) overhead A/B within run-to-run noise | **Met.** 36 of 36 p50 deltas overlap at ten reps; at `cores = 4` a consistent +0.1 % to +1.8 % on the single-row statements |
| (ii) arm D's per-core p99 spread within arm B's | **Not measured at the close** (cut; BL-S3 measured convergence) |
| (iii) partition fidelity | **Met** at every run's end (3/3): batch sessions on core 0, no reader there. Not sampled per statement |
| (iv) moves bounded by the dwell | **Not measured at the close** (cut; BL-S3's cells and convergence runs) |
| (v) `transactional` p99 at most 2x isolated | **Not met**: 6.5 ms median against 0.47 ms (14x); 287 ms undeclared. The excess is roll waits on reader cores, not a `normal` session: BL-R7's remainder, no rung 3 |
