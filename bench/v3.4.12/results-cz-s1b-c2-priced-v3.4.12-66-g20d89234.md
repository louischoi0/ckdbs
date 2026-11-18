# CZ-S1b, C2 priced further: without the zero prewrite a one-row commit's median barely moves and its p95 rises by two thirds; the bulk writer gains 20 % at `group`; at one load C1 with C2 is not worse than C1 for the reader

**Thesis.** Measured on worktree `cz-checkpoint-roll-wait` with CZ-S1's
spike binary, `20d89234` (`v3.4.12-66-g20d89234`), and the spike branch's
driver at `e9ee523f`: two measurements the operator asked for before any
order for C2 (`raft-marks-2026-11-18.md` §36), 55 cells. C2 is "no zero
prewrite where `posix_fallocate` reserved the extents". **The binary is a
spike: not landed, no test, no proof.**

- **A commit pays for the missing prewrite at its p95, not at its
  median** (§2). 4 writers committing one-row updates at 800 a second, a
  sync a commit: the p50 from the schedule is 809 µs against 799 µs at
  `group` (+1.3 %) and 813 µs against 792 µs at `strict` (+2.6 %). The
  service p95 is **1.6 ms against 985 µs** (+65 %) and **1.6 ms against
  945 µs** (+71 %), with no run of one arm inside the other's range.
  - Read as a sync that meets a never-written block costing about 1 ms
    more, which about one commit in sixteen of this size is (inferred,
    §2).
  - This is C2's cost alone: no commit cell filled half a segment, so the
    prewrite never ran in either arm and its own cost is not in B.
- **The bulk writer gains 20 % at `group`**, by trading its tail for its
  median (§3): 146,073 against 121,469 rows/s; a statement's p50 6.7 ms
  against 5.6 ms, its p99 11.1 ms against 66.8 ms. The walker beside it
  ran 16 % fewer statements.
- **At one load C1 with C2 is not worse than C1 for the reader** (§1):
  reader p99 1.8 ms against 2.7 ms with the inserter held to 120
  statements a second, 1.3 ms against 1.7 ms at 150. The runs overlap. The
  walker ran 14 % and 63 % more statements, because the inserter takes
  less of the core they share.
- **What the prewrite does to the others is a disturbance of about half a
  second a segment, and it starts at the half-segment mark** (§5), where
  the next body is asked for. A bulk writer's statements stall inside it;
  a roll that arrives before it ends waits the rest out at the sync gate.
  That is the "long roll": 251 ms where the roll comes 304 ms after the
  mark, 4 ms where it comes 577 ms after. The roll never waited on the
  body at 120 or 150 statements a second.
- **Neither C1 nor C1 with C2 meets CZ-Q1 (a)'s bar as written in these
  cells** but C1 with C2 at 120 a second (§1). They are not CZ-S1's cell;
  V1 is the close's.

**CLA's judgement, which §36 left to it.** The numbers support an order
on the prewrite, and not C2 as it stands. The fill costs a bulk writer
and whatever shares its core half a second a segment; dropping it costs
one commit in sixteen about a millisecond. Extents that are written and
were never filled - a recycled segment - would owe neither, by these
numbers (inferred). The order is exploratory.

- **Order**: `instructions/v3.2.0/workorder-cz-checkpoint-roll-wait.md`
  §7, "CZ-S1b"; the operator's word, `raft-marks-2026-11-18.md` §36.
- **The spike**: CZ-S1's, unchanged
  (`archive/cz-s1-ceiling-v3.4.12-66-g20d89234/cz-s1-spike.patch`).
- **Driver**: `tools/bl_mixed.py` with the spike's `--stall-cores` and a
  second change, `--insert-rate`
  (`archive/cz-s1b-c2-priced-v3.4.12-66-g20d89234/cz-s1b-driver.patch`):
  the inserter's statements are due at a fixed interval and a late one is
  sent at once. CZ-Q13 (a) had dropped a rate-held inserter; §36 admits it
  for this measurement. `bench/README.md` counts both as driver changes.
- **Raw output**: `archive/cz-s1b-c2-priced-v3.4.12-66-g20d89234/`:
  `out/r<run>/<cell>/`, the run scripts `cz-s1b.sh` and `cz-s1b-x.sh` with
  their progress files, the watchdog's logs, and `cz_s1b.py`, which writes
  `tables.md` ("`tables.md` §n" below).
- **Reviewed by `critics-developer`** against the raw data before the
  commit; what it corrected is in the order's §7.

| Item | Value |
|---|---|
| Executed | 2026-11-18 UTC. First pass 08:16:46-09:06:25, 45 cells; second pass 09:06:34-09:15:45, 10 cells. Every cell rc=0 and 0 errors. No build: CZ-S1's binary |
| Budget | The standing one, 20 minutes a stage. The 55 cells ran 17.7 minutes and two smoke cells under a minute. The quiet gate held the passes 33.7 minutes and the driver's wait for load another 7.3 minutes, outside the budget (`raft-marks-2026-11-18.md` §35). The second pass was added, not cut: the first showed that at 120 statements a second the rolls are short with the prewrite too |
| Binary | `kds_server.20d89234`, sha256 `ebaf66561336c35e8a69c9e1cbe096e7ee7b373eb8f1df4496b59eb15762d7c7`, CZ-S1's copy. `KDS_CZ_SPIKE` chooses the arm; each run line carries it (`tables.md` §0). The arms with the spike off run the instrumented binary, not `bf8d7a56`'s |
| Host / device | 8 vCPU Intel Xeon Platinum 8581C, Linux 6.12.116+deb13-cloud-amd64; data and WAL on `/dev/nvme0n1p1`, **ext4**, not tmpfs. CZ-S1's host; other sessions built and ran suites on it throughout (§0) |
| Server configuration | `cores = 4`, `buffer_pool_frames = 65536`, `log_level = info`, `reactor_cpus = 0,2,4,6`, clients on CPUs 1,3,5,7; a fresh server and data file per cell; port 15597; `durability` as the cell says |
| Cells of A | The `levels` cell at `relaxed` (6 readers at 400/s, the walker, the 1,024-row inserter) with `--insert-rate`: `c1h` and `c1c2h` at 120 statements a second, `c1x` and `c1c2x` at 150; `iso`, readers only, the spike off |
| Cells of B | 6 readers at 400/s and 4 open-loop one-row `UPDATE` writers at 800/s in all, no walker, no inserter: `g-base` and `g-c2` at `group`, `s-base` and `s-c2` at `strict`. And the `levels` cell at `group`, the inserter closed-loop: `gb-base`, `gb-c2` |
| Order | 5 runs. The first pass's nine cells rotate their order by one a run. `c1x` and `c1c2x` ran in the second pass, after it, alternating; their `iso` is the first pass's |
| Baseline | `results-cz-s1-ceiling-v3.4.12-66-g20d89234.md`. The PostgreSQL floor is not measured |
| Suite | Not executed: no build input changed, and the spike is not landed |

## 0. The host

Another session's build or suite was on the host for most of the first
pass (a load of 17 at its worst; the quiet gate waited 338 times 5 s).
`tables.md` §0 lists every cell. **12 of the 55** had a `cc1plus`,
`cmake --build` or `ctest` process at one end or began at a load of 2 or
more, and are marked `build`:

| cell | marked runs |
|---|---|
| iso | r2 |
| c1h | r3 |
| c1c2h | r3 |
| c1c2x | r1 |
| g-base | r2, r3, r4 |
| g-c2 | r4 |
| s-base | r3 |
| s-c2 | r2 |
| gb-base | r2 |
| gb-c2 | r2 |

- A build that begins and ends inside a cell is not seen. `g-c2` r2 is
  not marked, yet the run script saw a build's shell at its start
  (`cz-s1b.progress`) and it ran 30 s after `g-base` r2, which is.
- Every table below gives the medians of all five runs; §1 and §2 also
  give them over the unmarked runs.

## 1. A: C1 against C1 with C2 at one load

`tables.md` §1.1 has every run. Medians of 5:

| cell | inserter held to | inserter rate | log rate | reader p99 | against `iso`'s median | pairs within 2x | reader p99.9 | stall samples, 5 runs | walker statements a window | walker p99 | roll hold median, pooled |
|---|---|---|---|---|---|---|---|---|---|---|---|
| iso | - | 0 rows/s | 0.0 MB/s | 974 µs | 1.00x | 5 of 5 | 4.2 ms | 9 samples | 0 statements | - | no roll |
| c1h | 120 statements/s | 122,916 rows/s | 57.3 MB/s | 2.7 ms | 2.77x | 4 of 5 | 5.4 ms | 27 samples | 10,269 statements | 7.3 ms | 4.0 ms |
| c1c2h | 120 statements/s | 122,935 rows/s | 57.3 MB/s | 1.8 ms | 1.87x | 4 of 5 | 4.8 ms | 0 samples | 11,735 statements | 4.5 ms | 4.8 ms |
| c1x | 150 statements/s | 152,306 rows/s | 71.1 MB/s | 1.7 ms | 1.75x | 3 of 5 | 6.0 ms | 0 samples | 5,532 statements | 13.7 ms | 39.6 ms |
| c1c2x | 150 statements/s | 153,648 rows/s | 71.7 MB/s | 1.3 ms | 1.29x | 3 of 5 | 4.5 ms | 1 sample | 9,029 statements | 4.9 ms | 5.3 ms |

Over the runs no build reached (`tables.md` §2.5):

| cell | runs | reader p99 (median) | each run | walker statements (median) | walker p99 (median) | stall samples | roll hold median | roll hold max |
|---|---|---|---|---|---|---|---|---|
| iso | r1, r3, r4, r5 | 915 µs | 1.5 ms, 583 µs, 974 µs, 857 µs | - | - | 0 samples | no roll | no roll |
| c1h | r1, r2, r4, r5 | 2.2 ms | 2.7 ms, 3.0 ms, 1.6 ms, 1.4 ms | 10,365 statements | 7.2 ms | 6 samples | 3.9 ms | 35.3 ms |
| c1c2h | r1, r2, r4, r5 | 1.5 ms | 2.1 ms, 1.8 ms, 1.2 ms, 864 µs | 11,750 statements | 4.5 ms | 0 samples | 4.8 ms | 6.4 ms |
| c1x | r1 to r5 | 1.7 ms | 2.6 ms, 1.5 ms, 1.2 ms, 1.7 ms, 2.4 ms | 5,532 statements | 13.7 ms | 0 samples | 39.6 ms | 119.4 ms |
| c1c2x | r2, r3, r4, r5 | 1.2 ms | 1.0 ms, 1.9 ms, 1.2 ms, 1.3 ms | 9,036 statements | 4.8 ms | 0 samples | 5.1 ms | 5.9 ms |

- **The load is one as a mean**: the inserter's rate differs by under
  1 % between the arms of a pair, and the log's by under 1 %. It is not
  one in shape (`tables.md` §6): with the prewrite the held inserter falls
  behind at every segment and catches up back to back - 6-12 % of its
  statements sent late at 120 a second, 34-40 % at 150, up to 247 ms
  behind - and without it 0 %.
- **Neither arm meets CZ-Q1 (a)'s bar as written but `c1c2h`.** `c1h`
  misses the median clause (2.77x), `c1x` and `c1c2x` the pairs clause (3
  of 5).
- **The reader.** C1 with C2 is lower at p99 in both pairs, by 0.4 to
  0.9 ms. The runs overlap (C1 alone 1.2-3.0 ms, with C2 0.9-2.1 ms over
  the unmarked runs), so the size is not established; the direction is
  the same in both pairs and both readings.
- **The walker** shares core 0 with the inserter. Its statements follow
  what the inserter leaves of that core: the inserter's summed statement
  time is 7.5 s of a 10 s window against 5.9 s at 150 a second, and 5.2 s
  against 4.6 s at 120 (`tables.md` §6). Its p99 follows the roll's hold,
  which reaches 35 ms at 120 a second and 119 ms at 150 with the prewrite
  and 6 ms without.
- **The x cells' pairs** against `iso` are not interleaved with it, and
  `iso` r2 is a build cell.
- **`c1h`'s stall samples are the checkpoint's own flush call, with no
  latch wait.** In r5 (6 samples, no build) a read core's run line has a
  flush call of 51.2 ms against a longest in-run hold of 51.2 ms; in r3
  (21 samples, a build cell) 103.8 ms against 103.9 ms. That is the task
  writing the staged ring on its own reactor, which C1 leaves (CZ-Q21),
  inside the fill's disturbance. `c1c2h` has none in 5 runs.
- **`c1c2h` r3**, a build cell, has one roll hold of 447 ms and no stall
  sample.

## 2. B: a one-row commit with and without the prewrite

`tables.md` §2.1 has every run. No cell rolled: the writers log too little
to fill a segment. Every commit was its own sync (about 8,000 writer
syncs for 8,004 writes a window).

| cell | writer p50 | writer p99 | writer p99.9 | writer service p50 | writer service p99 | reader p99 | runs marked `build` |
|---|---|---|---|---|---|---|---|
| g-base | 799 µs | 4.0 ms | 5.5 ms | 708 µs | 2.6 ms | 1.7 ms | 3 of 5 |
| g-c2 | 809 µs | 3.4 ms | 6.1 ms | 720 µs | 2.6 ms | 1.3 ms | 1 of 5 |
| s-base | 792 µs | 2.5 ms | 5.4 ms | 702 µs | 1.5 ms | 897 µs | 1 of 5 |
| s-c2 | 813 µs | 2.2 ms | 5.2 ms | 721 µs | 2.0 ms | 529 µs | 1 of 5 |

| durability | writer p50, base | writer p50, C2 | change | writer service p50, base | C2 | change | writer p99, base | C2 |
|---|---|---|---|---|---|---|---|---|
| group | 799 µs | 809 µs | +1.3 % | 708 µs | 720 µs | +1.8 % | 4.0 ms | 3.4 ms |
| strict | 792 µs | 813 µs | +2.6 % | 702 µs | 721 µs | +2.7 % | 2.5 ms | 2.2 ms |

Over the runs no build reached (`tables.md` §2.4):

| cell | runs | writer p50 (median) | each run | writer service p50 (median) | writer p99 (median) | each run |
|---|---|---|---|---|---|---|
| g-base | r1, r5 | 798 µs | 789 µs, 807 µs | 706 µs | 2.1 ms | 1.9 ms, 2.4 ms |
| g-c2 | r1, r2, r3, r5 | 812 µs | 809 µs, 834 µs, 809 µs, 814 µs | 721 µs | 2.8 ms | 3.4 ms, 149.7 ms, 2.2 ms, 2.1 ms |
| s-base | r1, r2, r4, r5 | 792 µs | 783 µs, 791 µs, 792 µs, 804 µs | 702 µs | 2.0 ms | 2.7 ms, 2.5 ms, 1.5 ms, 1.5 ms |
| s-c2 | r1, r3, r4, r5 | 808 µs | 813 µs, 803 µs, 800 µs, 841 µs | 717 µs | 2.1 ms | 2.6 ms, 2.0 ms, 2.1 ms, 2.2 ms |

**The p95 separates the arms in every run** (`tables.md` §2.2a):

| cell | writer service p95 (median) | each run | writer p95 from its schedule (median) | writes 1.5 ms or more late, each run |
|---|---|---|---|---|
| g-base | 985 µs | 948 µs, 1.1 ms, 1.1 ms, 985 µs, 956 µs | 1.2 ms | 1.7 %, 41.2 %, 4.9 %, 3.6 %, 1.9 % |
| g-c2 | 1.6 ms | 1.6 ms, 1.7 ms, 1.6 ms, 1.8 ms, 1.6 ms | 1.8 ms | 9.1 %, 11.0 %, 7.4 %, 12.0 %, 6.9 % |
| s-base | 945 µs | 973 µs, 945 µs, 1.1 ms, 921 µs, 941 µs | 1.1 ms | 3.0 %, 1.8 %, 4.2 %, 1.0 %, 1.0 % |
| s-c2 | 1.6 ms | 1.6 ms, 1.7 ms, 1.5 ms, 1.6 ms, 1.6 ms | 1.7 ms | 8.4 %, 9.7 %, 6.7 %, 7.1 %, 7.5 % |

- **The median moves by 10-20 µs**, C2 the higher, at the edge of the
  runs' spread: at `strict` the unmarked runs barely overlap (783-804 µs
  against 800-841 µs); at `group` the base has two unmarked runs only.
- **The p95 moves by 0.6-0.7 ms**, +65 % and +71 %, with no overlap
  between the arms in any of the 20 cells, build cells included.
- **The p99 from the schedule does not separate the arms**; the service
  p99 over the unmarked runs is 2.3 ms against 1.5 ms at `group` and
  2.0 ms against 1.4 ms at `strict` (the review's computation).
- **The price is a sync's, about 1 ms, where it meets a never-written
  block** (inferred). A commit here logs about 250 bytes, some sixteen to
  a 4 KiB block, and 5-8 % more writes are 1.5 ms late under C2. The roll
  lines show the same step: a roll's header write takes 1.25 ms against
  0.45 ms. A commit that logs a block or more would meet it every time;
  that was not measured.
- **`group` and `strict` were one regime here**: a commit a sync in both
  (about 8,000 syncs for 8,004 commits), so `group` did not group.
- **B prices C2 alone.** No commit cell reached half a segment (the log
  is at 19 MiB at a window's end), so the preparer never ran in either
  arm.
- **The writers were placed by the kernel**, unevenly: all four on one
  core in `g-c2` r5.
- **Three long stalls, too few to read.** `g-base` r2, a build cell, has
  two episodes of 1.6 s and 1.5 s, so they occur with the prewrite.
  `g-c2` r2, 30 s later and unmarked, has 175 writer samples of 50 ms or
  more, the longest 1,235 ms and cut short by the window's end (7,324 of
  8,004 writes were sent). `g-c2` r4, at a load of 8, has one of 185 ms
  where its base twin at a load of 7.4 has none of 20 ms. In `g-c2` r2 no
  reactor went unpolled longer than 1.9 ms, so the wait was in the
  commit's sync. Whether an unwritten extent makes such a sync longer is
  not known.

## 3. B: the bulk writer at `group`

`tables.md` §3. The `levels` cell with the inserter closed-loop; each of
its statements is a commit of 1,024 rows.

| cell | inserter rate | against `gb-base` | statement p50 | statement p99 | statement, longest (median of runs) | log rate | walker statements a window | walker p99 | reader p99 | stall samples, 5 runs | roll hold median |
|---|---|---|---|---|---|---|---|---|---|---|---|
| gb-base | 121,469 rows/s | - | 5.6 ms | 66.8 ms | 288.6 ms | 56.1 MB/s | 11,404 statements | 4.5 ms | 1.1 ms | 0 samples | 3.1 ms |
| gb-c2 | 146,073 rows/s | +20.3 % | 6.7 ms | 11.1 ms | 20.9 ms | 68.2 MB/s | 9,543 statements | 4.5 ms | 1.0 ms | 0 samples | 4.7 ms |

- **A statement costs 1.1 ms more at the median without the prewrite**
  (+19 %): its sync covers about 480 KB of log in extents that were never
  written (§2's price, met by every statement).
- **And loses its tail.** With the prewrite 38 of 41 segments have a
  stall that begins 45 ms after the half-segment mark at the median and
  ends 417 ms after it (§5): one to four statements in a row, each run's
  longest 287-360 ms, none overlapping a roll. Without it, 0 of 50
  segments. The stall's place is measured; that it is the preparer's fill
  is inferred from the place, the fill itself not being timed.
- **The walker ran 16 % fewer statements**: the inserter takes more of
  core 0.
- **The reader is unmoved** (1.1 ms and 1.0 ms, no stall sample in either
  arm): the roll comes 774 ms after the half mark, long after the
  disturbance.
- One run of each arm is a build cell (r2); the medians do not rest on
  them.

## 4. CZ-S1's cells, every session

From CZ-S1's archive (`tables.md` §4): `relaxed`, the inserter
closed-loop, medians of 5. The reader's columns are that file's.

| cell | reader p99 | reader p99.9 | stall samples, 5 runs | inserter rate | inserter statement p99 | inserter statement, longest | walker statements a window | walker p50 | walker p99 |
|---|---|---|---|---|---|---|---|---|---|
| isolated | 1.5 ms | 4.5 ms | 0 samples | - | - | - | - | - | - |
| levels | 5.1 ms | 138.8 ms | 174 samples | 158,412 rows/s | 13.5 ms | 639.2 ms | 1,051 statements | 6.8 ms | 234.4 ms |
| c1 | 1.4 ms | 4.4 ms | 0 samples | 161,644 rows/s | 13.0 ms | 673.9 ms | 1,070 statements | 6.9 ms | 237.5 ms |
| c2 | 3.2 ms | 5.9 ms | 46 samples | 242,968 rows/s | 8.3 ms | 17.2 ms | 1,581 statements | 6.4 ms | 15.9 ms |
| c1c2 | 3.4 ms | 6.2 ms | 9 samples | 219,518 rows/s | 9.0 ms | 20.3 ms | 1,521 statements | 7.1 ms | 17.2 ms |

C1 moves the reader's tail and nothing else: the sessions on core 0 stop
at every roll as before. C2 removes the slow sync a roll waits out; the
gate stays, and 5 holds of 80 ms or more remain without the prewrite in
the two files (447, 531, 609, 619 and 84 ms), all at the sync gate, four
of them in cells a build reached.

## 5. Where the prewrite's disturbance falls in a segment

`tables.md` §5. The inserter's statements are of one size, so the middle
statement between two rolls marks the half-segment point, where the next
body is asked for (`src/wal/stream.cpp:454-463`). A long statement is one
of 30 ms or more. Medians over the segments inside a cell's windows.

| cell | inserter | segments | half mark to the roll | segments with a long statement after the mark | the disturbance ends after the mark | roll hold | sync gate's share of the holds | body ready / waited for / built by the roll |
|---|---|---|---|---|---|---|---|---|
| gb-base | closed-loop, `group` | 41 segments | 773.8 ms | 38 of 41 | 416.9 ms | 3.1 ms | 0 % | 41 / 0 / 0 |
| c1h | held to 120/s, `relaxed` | 40 segments | 576.8 ms | 32 of 40 | 412.9 ms | 4.1 ms | 60 % | 40 / 0 / 0 |
| c1x | held to 150/s, `relaxed` | 55 segments | 511.6 ms | 55 of 55 | 526.9 ms | 39.6 ms | 93 % | 55 / 0 / 0 |
| CZ-S1's `c1` | closed-loop, `relaxed` | 56 segments | 303.7 ms | 55 of 56 | 553.1 ms | 251.4 ms | 99 % | 39 / 17 / 0 |
| CZ-S1's `levels` | closed-loop, `relaxed` | 56 segments | 305.1 ms | 55 of 56 | 555.3 ms | 250.9 ms | 99 % | 39 / 17 / 0 |
| gb-c2 | closed-loop, `group`, no prewrite | 50 segments | 481.8 ms | 0 of 50 | - | 4.7 ms | 0 % | 50 / 0 / 0 |
| c1c2x | held to 150/s, `relaxed`, no prewrite | 55 segments | 461.6 ms | 0 of 55 | - | 5.3 ms | 1 % | 55 / 0 / 0 |

- **With the prewrite something slows the log's syncs for 0.41-0.56 s
  from the half mark, in nearly every segment; without it, in none.** It
  is read as the preparer's 64 MiB fill and its `fsync` (inferred: the
  preparer is not timed).
- **A roll's hold is what is left of it when the roll arrives.** In
  CZ-S1's `c1`, 553 ms less 304 ms is 249 ms, against a hold of 251 ms.
  Where the roll comes after it has ended the hold is 3-4 ms.
- **The roll waits at the sync gate, not for the body.** The body was
  ready at every roll at 120 and 150 statements a second; in CZ-S1's
  cells 17 of 56 rolls waited for it, at most 44 ms of the hold.
- **So it is a ramp in the time from the half mark to the roll**, not a
  threshold in the log's mean rate: a closed-loop inserter logs faster
  between its stalls than its mean says.

## 6. What this file does not establish

- **C2's cost for a commit that logs a block or more**, at every commit;
  the mean cost of the one measured (the driver keeps percentiles); and
  the cost on another filesystem. `Prewrite`'s comment prices the
  conversion on xfs; this is ext4.
- **The commit path under a log that rolls, or with the preparer
  running.** B's writers never reached half a segment.
- **Group commit.** `group` synced a commit at a time here.
- **What the disturbance is.** Its place in the segment is measured; the
  preparer's write and `fsync` are not timed.
- **Whether the prewrite moved would do.** Asked for earlier, the fill
  would end before the roll; the bulk writer's stall inside it would
  stay, as it does in `gb-base` and `c1h`, whose rolls are short.
- **Whether written extents with no fill would owe neither cost.** A
  recycled segment has them. By this file's p50s the bulk writer at
  `group` would keep 5.6 ms and lose its tail; nothing measured it.
- **Recovery over a segment that was never zero-filled**, and a power cut
  inside it: not read here.
- **V1.** These cells are not CZ's.
- One host, 10 s windows, 5 runs, 12 of 55 cells with a build on the
  host, and the base of each pair run just before its C2 arm in 4 of 5
  runs.
- The smoke cells and the passes' first seconds are not in the archive.
