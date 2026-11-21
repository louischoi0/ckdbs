# DO-S1 - E1 and E3: the sorts out of the checkpoint's holds, the frame table sized once (`v3.5.0-56-g47f7016c` against S1)

Work order: `instructions/v3.2.0/workorder-do-frame-table-long-holds.md`
DO-S1. Measured 2026-11-21 on the worktree `dg-wal-segment-body` (branch
`worktree-do-frame-table-long-holds`). **A** is DO-S0's tree, `47f7016c`
(`v3.5.0-56-g47f7016c`); **B** is A plus E1 and E3, the tree the DO-S1
commit carries. Release builds of `bench/frame_table_hold_bench.cpp`,
each binary copied out of `build-release` and run from the copy (A
`sha256 174555c9...`). Same host and device as DO-S0
(`results-do-s0-census-v3.5.0-55-g48a3276c.md`): 8 vCPUs, data file on
`/dev/nvme0n1p1` (ext4). Two rounds, A then B at each size, interleaved;
10 calls a cell at 64 Ki and 256 Ki frames, 5 at 1 Mi; every frame dirty;
nothing else running in the store. **Load average 4.8-7.0 during the run**,
with no compiler or `ctest` process on the host (`pgrep -c -f
'cc1plus|ctest'` 0 at every cell): another session's load, not
identified. Time spent: 6 minutes running, 3 building.

## What changed

- **E1**: `DirtyPagesWithRecLsn` (H1) and `DirtyPageIds` (H2) reserve their
  vector before the hold, sized from `configured_capacity_` (set once at
  `Open`, an upper bound on the table), and sort after it. The same frames
  go into the same table at the same instant; the output is identical.
  `Flush`'s collection (H6) is now `DirtyPageIds()`, so it gets the same.
- **E3**: `frames_` reserved to `configured_capacity_` at `Open`; no
  insert rehashes it, and a debug assertion says so at the insert (DO-R6).

DO-Q4 (a) and §2's E1 name `capacity_`; the code uses
`configured_capacity_`, because `capacity_` moves after `Open` (lowered by
MG05's debug budget, raised by `ApplyMountFloor`) and `configured_capacity_`
bounds it from above.

## The holds, A against B (p50 of the calls, both rounds pooled)

| resident frames | H1 hold A | H1 hold B | change | H2 hold A | H2 hold B | change | H3 rehashes A (longest) | H3 B |
|---|---|---|---|---|---|---|---|---|
| 65,536 | 1,658 us | 497 us | -70 % | 1,269 us | 231 us | -82 % | 12 (839 us) | 0 |
| 262,144 | 7,415 us | 1,876 us | -75 % | 5,631 us | 1,173 us | -79 % | 14 (3,480 us) | 0 |
| 1,048,576 | 38,675 us | 15,372 us | -60 % | 29,580 us | 8,740 us | -70 % | 16 (12,475 us) | 0 |

The longest single call in B: H1 1,735 us, 5,957 us and 21,803 us; H2
301 us, 1,442 us and 10,019 us. **The sort was most of each hold**; what
is left is the walk itself - one pointer chase a resident frame through the
table's node list - plus the first-touch faults of the output vector,
which still land inside the hold. The whole call is longer than its hold by
the sort (B's `h1_call` p50 at 1 Mi frames 25,163 us), all of it now
outside the latch.

**Against DO-Q9 (a)'s bar** (no hold at a checkpoint over 1 ms at 1 Mi
frames): **not met**. B's holds are 15 ms and 9 ms at 1 Mi frames and over
1 ms from 256 Ki. That reaches DO-S3: the walk itself is over the bar
(DO-Q3 (a)).

## Tried and not taken: the output vector written through before the hold

The vector sized by `resize` before the hold and written by index inside
it, so no first-touch fault lands in the hold. Two rounds at 1 Mi frames,
interleaved with B: H1's hold 15.2 ms to 10.7 ms, **H2's 7.5 ms to 9.4 ms**,
and the calls level. It also writes the whole configured capacity's worth
of vector outside the hold at every checkpoint, which a large, mostly
empty pool never paid. Mixed, and not a fix for the walk: not landed.
