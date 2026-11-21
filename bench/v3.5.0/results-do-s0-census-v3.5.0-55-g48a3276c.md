# DO-S0 - the frame table's holds, counted and timed (`v3.5.0-55-g48a3276c` + DO-S0's instrument)

Work order: `instructions/v3.2.0/workorder-do-frame-table-long-holds.md`
DO-S0. Measured 2026-11-21 on the worktree `dg-wal-segment-body` (branch
`worktree-do-frame-table-long-holds`), on `48a3276c` with DO-S0's
instrument applied and nothing else changed: the instrument adds two clock
reads to a whole-table hold and nothing to a fetch, so the tree measured is
the tree as it is (DO-S0 item 4).

Host: 8 vCPUs (4 cores x 2 threads), Intel Xeon Platinum 8581C, 31 GiB.
Data files on `/dev/nvme0n1p1` (ext4), never tmpfs. Release builds:
`build-release` (`-DKDS_BUILD_BENCH=ON`) and `build-census` (the same with
`-DKDS_LATCH_HOLD_CENSUS=ON`, DO-Q1 (a)'s bench half). Load average 4.5
(the census build that had just ended, and the microbenchmark's own
threads); no other build or suite was running.

## 1. The holds proportional to the pool (H1, H2, H3)

`bench/frame_table_hold_bench.cpp`: one armed `DevicePageStore` over one
`FilePageDevice`, every resident frame dirty (a checkpoint's worst case).
The hold is the instrument's own reading (`frame_table_walk_us` as a
delta), exact; "call" is the whole function as a caller sees it. Ten to
twenty calls a cell, with nothing else running.

| resident frames | H1 hold p50 (`DirtyPagesWithRecLsn`) | H1 max | H2 hold p50 (`DirtyPageIds`) | H2 max | H3 rehash, longest | H3 rehashes in the fill |
|---|---|---|---|---|---|---|
| 65,536 | 1,660 us | 2,434 us | 1,286 us | 1,363 us | 833 us | 12 |
| 262,144 | 7,011 us | 10,038 us | 5,631 us | 5,685 us | 3,438 us | 14 |
| 1,048,576 | 37,576 us | 43,693 us | 30,069 us | 32,428 us | 12,098 us | 16 |

A call's time equals its hold to within 2 us at every size: **the sort runs
inside the hold** (§1.2's reading), and so does the `reserve`.

**The stall it puts into every core's fetch.** The same store, three reader
threads (cores 1-3) fetching random resident pages for 3 s while the main
thread runs H1 every 100-200 ms:

| resident frames | H1 hold under readers, p50 | fetch p50 | fetch p99 | fetch p99.9 | fetch max | fetches/s |
|---|---|---|---|---|---|---|
| 65,536 | 1,904 us | 1.45 us | 15.63 us | 25.91 us | 4,028 us | 1,037,102 |
| 262,144 | 8,764 us | 1.89 us | 19.36 us | 29.74 us | 9,199 us | 822,316 |
| 1,048,576 | 38,746 us | 2.11 us | 23.09 us | 37.11 us | 47,368 us | 590,015 |

A fetch's maximum is the walk's hold: every core waits it out, whatever
page it wanted. At 1 Mi frames that is 47 ms, every checkpoint, twice (H1
and H2).

## 2. The per-fetch holds (E4's arithmetic)

`build-census`, the same microbenchmark at 65,536 frames, read hits only:

| readers | fetches | structure-latch holds | holds a fetch | hold ns a fetch | frame-table waits |
|---|---|---|---|---|---|
| 1 | 3,135,795 | 9,407,405 | 3.000 | 335 ns | 37 |
| 3 | 1,179,839 | 3,539,537 | 3.000 | 605 ns | 1,029,215 |

**Three holds a read hit, exactly** (§1.2's FetchAndPin, UnpinFrame's
first hold, its second). The hold time includes the census's own clock
read inside each hold, and, in the build measured here, its record (three
relaxed adds and a compare-and-swap), which the review moved after the
release before DO-S0 was committed; the counts are unaffected. Threads that
declare no core (the WAL writer, the data-sync thread, the page readers)
record into slot 0 beside core 0, so a census build's core-0 numbers carry
their line traffic. With three readers 87 % of fetches waited for the
latch at least once and the throughput fell to 38 % of one reader's: the
per-fetch holds' cost is the line moving, which is CW's C1, not DO's.

**Per statement**, `tools/ba_census.py` against `build-census/kds_server`,
`cores = 2`, 2 sessions, `relaxed`, 5 s measured after 1 s of warm-up, one
run a shape (`--server-cpus 0,2 --client-cpus 4,5`):

| shape | statements | frame-table holds a statement | frame-table waits a statement | hold longest | walks in the span (longest) |
|---|---|---|---|---|---|
| `point` | 129,599 | 12.01 | 0.194 | 49 us | 2 (7 us) |
| `insert1` | 156,726 | 24.64 | 0.745 | 77 us | 4 (76 us) |
| `pkmix` (half point, half `UPDATE` by tuple key) | 124,966 | 18.50 | 0.434 | 26 us | 2 (16 us) |

So an `UPDATE` by tuple key takes about 25 holds (`2 x 18.50 - 12.01`). The
ba_census relation is 10,000 rows in a 65,536-frame pool, so its walks are
short; §1 is what a pool of production size walks.

## 3. The census of sites (DO-S0 items 1 and 2)

40 structure-latch hold sites in `device_page_store.cpp` at `48a3276c`,
as §1.2 said. Classified:

- **per fetch** (12): `FetchAndPin` (`PinResidentAndRelease` runs inside
  its hold), `AcquirePageLatch`'s exclusive re-mark, `UnpinFrame` x2,
  `UnpinUnlatchedFrame`, `LogOwedImage` x2 (the second only on a failed
  append), `StampPageLsn`, `MarkFrameDirty`, `CreatePinned` x2, `PinFrame`
  (unarmed callers' path, and `kUnlatched` pins);
- **per miss, create or free** (7): `InsertFrame`, `ReserveFrame` x2 (the
  second only on a refusal, H4), `ClaimFreshId`'s free-list pop,
  `CreateNamedIdUnpinned`, `FreePage`, `ReleaseScanSlot`;
- **per mutation** (2): `BeginNoRefuseWindow`, `EndNoRefuseWindow`;
- **periodic, bounded** (10): `WriteBack` x5, `MaintainFreeReserve` x2,
  `AwaitWritebackClaim`, `HighestDirtyPageLsn` (its input's length),
  `TakeDirtyEvictionQueue`;
- **proportional to the pool** (5 sites of their own, H1-H6):
  `DirtyPagesWithRecLsn` (H1),
  `DirtyPageIds` (H2), `InsertFrame`'s rehash (H3), the refusal census
  (H4), `resident_class_frames` and `pinned_frames` (H5), and **one §1.2
  did not list, H6**: `Flush`'s collection, a walk of every frame with no
  sort, reached by `Sync` - the synchronous checkpoint's publish
  (`SuperBlockCheckpointAnchor::Publish`), a shutdown's and a backup's;
  the cadence's publish uses H2 and not H6. Timed with the rest;
- **rare** (4): `AllowPins`, `RevokePins`, `EvictColdFrames` (test
  callers only), `pool_counters`. H3 and H4 share `InsertFrame`'s and
  `ReserveFrame`'s sites, so the count is 12 + 7 + 2 + 10 + 5 + 4 = 40.

**E4b's premise holds at `48a3276c`**: a free-map bit is cleared by
`FreePage`, which erases the frame in the same hold of both latches; by
`ReleaseClaim`, for an id whose frame was never inserted (a claim that
failed before `PublishFreshPage`, or a `ClaimNew` claim given back, its
slot returned unpublished); by mount's headerless-bit repair (the
headerless map, not the free map); and by `restore.cpp` offline, before a
store exists. No path clears a bit under a resident frame.

**E4d's premise holds at `48a3276c`**: `ReleaseFrameLocked` is the one
eraser and has three callers - `ReleaseScanSlot` (returns on a pin),
`FreePage` (`kDeferred` on a pin) and the sweep (skips a pin); nothing
clears, swaps, reserves or rehashes `frames_` but an insert, and a rehash
keeps every node's address.

## 4. What was cut (DO-S0 item 4, against the 20-minute budget)

- **The hold times at the three pool sizes are the microbenchmark's, not
  the engine's.** The same two functions on the same store type; an engine
  run at 1 Mi resident frames would spend the budget filling 8 GiB.
- **`cores = 1` and `cores = 4` not run** for the per-statement counts:
  the counts are a property of the code path (a read hit is three holds at
  any core count above one) and `cores = 1` takes none.
- **`sched_hold_*_longest` with and without an overlapping checkpoint not
  read.** §1's contend phase prices the same overlap directly.

## 5. For the bar (DO-Q6 (a))

The longest hold at a checkpoint is 1.7 ms at 64 Ki frames, 7 ms at 256 Ki
and 38 ms at 1 Mi, and a fetch on any core waits it out in full. CLA's
proposal for the bar, adopted on the operator's standing go-ahead (DO-Q9,
the order's §4): **no structure-latch hold at a checkpoint longer than
1 ms at 1 Mi resident frames.** That is eight times a `point` statement's
p99 at `cores = 2` (130 us) and the instrument's middle threshold, so
`contention_frame_table_walks_over_1ms` reads it directly in production.
