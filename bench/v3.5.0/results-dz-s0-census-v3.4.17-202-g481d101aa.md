# DZ-S0 - what one cadence checkpoint run holds its own reactor for, piece by piece (`v3.4.17-202-g481d101aa`)

Work order: `instructions/v3.2.0/workorder-dz-checkpoint-reactor-residue.md`
DZ-S0, opened on the operator's standing go-ahead (`raft-marks-2026-11-27.md`
§11). Measured 2026-11-27 15:38-15:43 UTC on the worktree
`dz-checkpoint-reactor-residue` at `481d101aa` (`v3.4.17-202-g481d101aa`),
the tree DZ opened from, with only the bench added.

## The answer

**The walk and the writes are over DZ-R5's bar; the map halves' own holds
are not.** At 1 Mi dirty frames the run's start - the dirty-table walk -
holds the reactor 28-73 ms, the publish's walk about 10-11 ms, and a step's
`pwrite`s up to 35-1,256 ms (the kernel's writeback throttle, with 8 GiB
dirty); the map halves hold it 0.04-0.05 ms. At 256 Ki the start is over
the bar in three of four cells (6.8-14.3 ms) and every write is under 1 ms;
at 64 Ki every piece is under it, the longest one start (8.5 ms, under three
readers).

## Setup

- **Driver**: `bench/checkpoint_reactor_hold_bench.cpp` (new here), the
  run task of one `Checkpointer` on one `Scheduler` on the system clock,
  over one `DevicePageStore` on one `FilePageDevice`, `--frames` pages
  created and left dirty; the data-sync thread started, and the log a
  `FileLogDevice` beside the data file with its writer thread started. A
  `foreground` task on the same reactor yields every poll and records its
  longest gap. `--readers 0` is the unarmed store (`cores = 1`), `--readers
  3` the armed store with three threads fetching random resident pages for
  the run's length (`cores = 4`). Each target call is timed by a wrapper
  (`TimedTarget`), so a long poll is named by its piece.
- **Binary**: `build-release` at `481d101aa` plus the bench, copied to
  `/var/tmp/dz-bench/hold-a` (sha256 `1e8b00fd8f97d9d4...`) and run from the
  copy, pinned to CPUs 0-7 (the P cores).
- **Device**: the data file and the log on `/dev/mapper/fedora-root`, xfs
  (`df -T /var/tmp`), never tmpfs.
- **Grid**: 64 Ki, 256 Ki, 1 Mi frames x `cores` 1 and 4 x two rounds, one
  process a cell, in that order. Load at each cell's start is in
  `status` in the archive (0.9-7.7 at a cell's start, 9.3 at the last
  cell's end; other sessions on the host). **Rule 4 is
  met by the load half only**: no `pgrep` for compilers was recorded.
- **What is cut**, each a piece the bench does not price: the pages carry no
  LSN, so no write is gated on the log and no step defers; the anchor is the
  in-memory one, so the anchor page's write and sync are absent; nothing
  holds the map write barrier beside the run, so the map halves' wait on
  another caller's `fsync` (`known-gaps.md`, "Carve and checkpoint stalls")
  is not exercised; the carve is not re-measured here (moved to DZ-S3,
  which needs the server binary). Time spent: 5 minutes running
  (15:38:16-15:43:16 in `status`), 4 building.

Archive: `archive/dz-s0-census-v3.4.17-202-g481d101aa/` (each cell's
output, the status log, the driver).

## The census, the longest occurrence of each piece in one run

| frames | cores | round | longest `system` poll | polls over 10 ms | start (walk) | step's writes | publish walk | maps (either half) | run |
|---|---|---|---|---|---|---|---|---|---|
| 65,536 | 1 | 1 | 1.97 ms | 0 | 1.77 ms | 0.21 ms | 0.38 ms | 0.01 ms | 0.16 s |
| 65,536 | 1 | 2 | 1.99 ms | 0 | 1.79 ms | 0.16 ms | 0.37 ms | 0.18 ms | 2.80 s |
| 65,536 | 4 | 1 | 2.25 ms | 0 | 2.03 ms | 0.31 ms | 0.52 ms | 0.01 ms | 0.23 s |
| 65,536 | 4 | 2 | 9.11 ms | 0 | 8.47 ms | 0.59 ms | 0.34 ms | 0.18 ms | 3.96 s |
| 262,144 | 1 | 1 | 7.78 ms | 0 | 6.80 ms | 0.45 ms | 1.94 ms | 0.02 ms | 5.08 s |
| 262,144 | 1 | 2 | 13.93 ms | 1 | 12.97 ms | 0.44 ms | 1.51 ms | 0.03 ms | 18.56 s |
| 262,144 | 4 | 1 | 14.97 ms | 1 | 13.88 ms | 0.55 ms | 2.18 ms | 0.02 ms | 2.63 s |
| 262,144 | 4 | 2 | 15.32 ms | 1 | 14.29 ms | 0.67 ms | 1.98 ms | 0.02 ms | 11.56 s |
| 1,048,576 | 1 | 1 | 1,255.79 ms | 1,288 | 72.87 ms | 1,255.78 ms | 9.85 ms | 0.05 ms | 54.58 s |
| 1,048,576 | 1 | 2 | 35.08 ms | 966 | 28.02 ms | 35.08 ms | 10.37 ms | 0.04 ms | 30.38 s |
| 1,048,576 | 4 | 1 | 46.08 ms | 1,738 | 41.74 ms | 34.59 ms | 10.92 ms | 0.04 ms | 76.31 s |
| 1,048,576 | 4 | 2 | 268.71 ms | 1,635 | 41.62 ms | 268.70 ms | 10.61 ms | 0.05 ms | 64.13 s |

The foreground task's longest gap equals the longest `system` poll to
within 0.02 ms in every cell: a session on the checkpointing core waits
exactly as long as the run's longest poll.

- **The start's walk is `DirtyTable()`, not `DirtyPagesWithRecLsn()` alone**:
  timed directly after the run with every page dirtied again, the latter is
  17-25 ms at 1 Mi (`h1_call_max_ms` in each cell), against 28-73 ms for the
  former in the run - the projection into a second vector, grown with no
  reserve, and the first touch of both, on top of the walk and the sort.
- **The writes are the throttle's, not the device's**: at 256 Ki (2 GiB
  dirty) no step's writes passed 0.7 ms; at 1 Mi (8 GiB, past the host's
  dirty limits) every cell had 966-1,738 `system` polls over 10 ms and one
  step's writes took 1.26 s.
- **Rounds disagree on run length** (0.16 s against 2.80 s at 64 Ki,
  `cores = 1`), the data park the largest part of it (0.01 s against
  1.51 s; under three readers 0.23 s against 3.96 s, the park 0.0003 s
  against 3.44 s): the data file's `fsync` behind another session's
  writeback. No piece's hold moved with it.

## DO's §5a re-read with a yield between slices (DZ-R1) `[quiet-wrong]`

§5a proves the fuzzy table a correct input to `RedoStartFrom` on two
premises, the second of which DZ removes: "the checkpoint's task does not
suspend in the walk". Re-read case by case with the walk and the publish's
collection each taken in slices with a yield between:

1. **Cases 1-3 and 5 do not use the premise**; they hold as written.
2. **Case 4's cover by the publish holds**: a record stamped before
   `CHECKPOINT_BEGIN` is on a page dirty before the publish's collection
   begins, whatever slices that collection is taken in; a page that leaves
   a slot not yet visited for one already visited was clean when it left
   (only a clean frame is evicted), so its record is on the device. The
   publish writes every page it meets before the anchor's page.
3. **Case 4's cover by the fold is not needed for a run's own core**:
   §5a says the checkpointing core appends nothing in its own walk, so its
   contribution is never the one replaced in the window. With a yield, its
   own sessions run and append below `begin_lsn`, on pages already visited;
   the publish of the same run writes those pages before its anchor lands,
   so no record is exposed by the run that replaces that core's
   contribution.
4. **The active-transaction table is the premise's other dependant, and
   §5a does not name it.** `Start` snapshots the active transactions
   *before* the dirty table. With no suspension between the two and the
   append, no transaction of this core can begin in between. With a yield
   in the walk one can: it begins, writes a page already visited, and is
   still undecided at `CHECKPOINT_BEGIN`, which does not list it. Its
   records are below the new redo start, its undo head is in no
   `CHECKPOINT_BEGIN` inside the replay range, and the publish writes its
   uncommitted change to the device: a crash then leaves a loser that
   recovery cannot see, **its change on the device as if committed**.
   **So the transaction table is snapshotted after the walk, with no
   suspension between it and the append** - DZ-S1's rule, and the crash
   cell DZ-R1 asks for is that loser.

## What DZ-Q1 (a) leaves and builds

| piece | over the bar (10 ms) | DZ-Q1 (a) |
|---|---|---|
| the start's walk | 256 Ki and 1 Mi | built (DZ-S1) |
| the publish's walk | 1 Mi, at the bar (9.9-10.9 ms) | built (DZ-S1), the same mechanism |
| a step's and a publish batch's writes | 1 Mi | built (DZ-S2) |
| the map halves' own holds | never (0.01-0.18 ms) | **not measured where it waits**: a wait on another caller's `fsync` needs a concurrent blocking flush and a changed headerless map, which this bench does not create; DZ-Q6 |
| the carve | not measured here | DZ-S3 measures it first (DZ-Q4 (a)) |
