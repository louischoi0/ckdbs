# DO-S3 - E2: the checkpoint's dirty table walked a slot chunk per hold (`v3.5.0-62-g1cfe6c0f` against E2)

Work order: `instructions/v3.2.0/workorder-do-frame-table-long-holds.md`
DO-S3 and §5a, built on the operator's mark (`raft-marks-2026-11-22.md`
§1). Measured 2026-11-22 on the worktree `dg-wal-segment-body` (branch
`worktree-do-frame-table-long-holds`). **A** is `1cfe6c0f`
(`v3.5.0-62-g1cfe6c0f`, whose store is DO-S2's `7116b668`); **B** is A
plus E2. Release builds of `bench/frame_table_hold_bench.cpp`, each
binary copied out of `build-release` and run from the copy. 8 vCPUs, data
file on `/dev/nvme0n1p1` (ext4), load 1.8-3.6, no compiler or `ctest` on
the host. Two rounds, A then B at each size, interleaved; every frame
dirty; three reader threads (cores 1-3) fetching random resident pages
for 3 s while the main thread runs H1 every 200 ms - **25 times the
production cadence** (5 s, the cores in turn), so the throughput rows
weigh the walk's cost far above what a server pays. Time spent: 11
minutes running and 4 building for the first run, 6 and 3 for the second.

## What E2 is

Armed, `DirtyPagesWithRecLsn` (H1) and `DirtyPageIds` (H2, and `Flush`
through it) walk the slot array 1,024 slots a structure-latch hold, a
slot's frame found through its owner's table entry, the latch released
between chunks; the table is each frame's as of its chunk's hold, a page
met twice merged to its lowest non-zero recLSN. **Between chunks the latch
is handed to one waiter**: `Latch` counts the waiters and the acquisitions
that waited, on its contended path only, and the walker yields until one
waiter is in or 20 us pass. Unarmed, one walk as before (DO-R2).

## Two runs, and why there are two

The first run (`ab-e2`, B = E2 with a handover that waited until **no**
waiter was left) is the table immediately below. Its review found that a
steady stream of fetches is never empty, so that handover could spin its
20 us at every chunk, stalling the walking core about 20 ms more a walk at
1 Mi frames; the handover was changed to wait for **one** waiter, and the
second run (`ab-e2v2`, 256 Ki and 1 Mi, two rounds, load 5.1-5.5 - another
session's workload) measures what lands, with the walker's own time added.

## The bar (DO-Q9 (a)): the longest single structure-latch hold

| resident frames | before E2 | E2, first run | E2 as landed | holds over 1 ms |
|---|---|---|---|---|
| 65,536 | 1,394-1,552 us | 27-30 us | (not run) | 0 |
| 262,144 | 4,858-5,905 us | 323-330 us | 312-323 us | 0 |
| 1,048,576 | 20,971-22,686 us | 59-134 us | 129-154 us | 0 |

**Met at every size measured.**

## E2 as landed, against no E2 (second run)

| resident frames | | before E2 | E2 |
|---|---|---|---|
| 262,144 | fetches a second | 898,550 / 924,069 | 1,059,396 / 1,111,981 (+18-20 %) |
| | fetch p99 | 17.8-18.5 us | 16.1-16.6 us |
| | fetch max | 4,084-4,220 us | 856-3,100 us |
| | the walker's H1 call (its reactor's stall) | 6.2-7.0 ms | 10.4-10.8 ms |
| 1,048,576 | fetches a second | 774,054 / 766,939 | 849,129 / 851,652 (+10-11 %) |
| | fetch p99 | 20.5-21.3 us | 23.0-23.4 us |
| | fetch max | 15,780-17,683 us | 4,400-4,452 us |
| | the walker's H1 call | 26.0-26.4 ms | 48.7-49.5 ms |

- **Every other core gains**: more fetches a second, the longest wait cut
  4-5 times at 256 Ki and 1 Mi, no hold over about 0.3 ms.
- **The walking core pays**: its H1 call roughly doubles - the sort (as
  before, off the latch), a table lookup a slot, and a handover a chunk -
  about 49 ms at 1 Mi frames, and the publish's walk again. That is a stall
  of that core's reactor, not of the latch (`known-gaps.md`).
- **A fetch still waits up to 4.4 ms** with no long hold: the one mutex's
  unfairness between fetches, which the first run's all-waiters handover
  hid better (0.4-1.6 ms at 1 Mi) at the walker's expense. CW's C1.

## The first run's other rows (all-waiters handover; walk every 200 ms)

| resident frames | fetch max A | fetch max B | fetch p99 A | fetch p99 B | fetches a second A | B |
|---|---|---|---|---|---|---|
| 65,536 | 896-1,640 us | 2,130-2,339 us | 12.8 us | 13.4-13.6 us | 1.39-1.40 M | 1.43-1.48 M |
| 262,144 | 4,066-4,177 us | 442-926 us | 15.9-17.1 us | 18.5-18.7 us | 0.95-1.00 M | 0.91-0.92 M |
| 1,048,576 | 15,834-15,937 us | 415-1,613 us | 20.2-20.5 us | 25.8-26.1 us | 0.81-0.82 M | 0.78 M |

At 64 Ki the maxima rose with every hold under 30 us: the readers'
unfairness among themselves. Before the waiter count existed at all, E2
alone left a fetch waiting 12.5-12.9 ms at 1 Mi frames with no hold over
224 us - the walker re-took the latch before the waiter it had woken ran.

## The crash cells (DO-R1)

`tests/dirty_table_chunked_walk_test.cpp`, on `SimInstance` with the store
armed: row 7 updated and committed from the walk's seam after its chunk
was walked - its record below the run's `CHECKPOINT_BEGIN`, its page
absent from the run's table. A crash before the run's anchor lands
(`AbandonCheckpointMidSteps`) recovers it from the previous anchor; a
crash after it (`RunCheckpoint`) finds it on the page, the publish having
written it. **The second cell was run against a mutant** in which the
publish's `DirtyPageIds` lists page 0 alone: recovery started at LSN
27,720, past the update, replayed nothing, and the row read its old value
- the cell fails, so it exercises the window and the publish's cover.
No mutant of the first cell's cover (the previous anchor's redo start)
was built.
