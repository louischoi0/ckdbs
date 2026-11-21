# Work order DO - the frame table's long holds: what the instance's one structure latch is held for, shortened where nothing moves, measured before and after

Status: **OPENED 2026-11-21 on the operator's standing go-ahead
(go-ahead-achieving-milestone, `raft-marks-2026-11-21.md` §12)**, on the
worktree `dg-wal-segment-body` (branch `worktree-do-frame-table-long-holds`)
from `48a3276c` (`v3.5.0-55-g48a3276c`). **DO-Q0..Q8 adopted (a) and DO-Q9
(a) added and adopted, as CLA proposed, on that go-ahead; none is the
operator's own per-item mark** (§4). DO-S0 done (§7). Written on `main` at
`3a07ad49` on the operator's word of 2026-11-21 (`raft-marks-2026-11-21.md`
§8); that DO is an order of its own, cut out of CW's C1 rather than written
into CW, is CLA's proposal and the operator's yes to it. Every
`[source-read]` claim in §1 was read at `3a07ad49` and re-read at
`48a3276c` by DO-S0.

**Not exploratory, but nothing lands unpaid** (DO-Q0, DO-R5). Most
candidates here change no behaviour, so each one that pays at DO-S0 lands
on its own A/B. One candidate (E2) changes what a checkpoint's dirty table
is a snapshot of. It is `[quiet-wrong]` and gated on a written proof and
the operator's mark (DO-R1, DO-S3).

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` where a wrong choice turns a
correct answer into a wrong one with no error.

Related: `instructions/v3.2.0/workorder-cw-optimistic-page-read.md` (C1,
the structure latch partitioned; DO is what can be bought before it, and
DO's numbers are CW-S0's input); `instructions/v3.0.0/workorder-am-m1-shared-pool.md`
(AM-S2, the shared frame table and its latch);
`instructions/v3.0.0/workorder-be-bounded-pool.md` (BE-R1 slots, BE-R2
bounded batches); `instructions/v3.2.0/workorder-db-torn-page-healing.md`
and `workorder-dh-page-image-volume.md` (the owed image);
`instructions/v3.2.0/workorder-cz-checkpoint-roll-wait.md` (CZ-R3, no
suspension between the snapshot and the append);
`docs/pending/multicore-scaling-cores4-below-cores2.md` (the unattributed
tail); `docs/pending/v3-4-and-architecture-audit.md` H39;
`docs/pending/whole-engine-audit-second-run-twelve-slices.md` (the hit
path's redundant `IsAllocated`); `docs/spec/page.md` §3, §5, §6;
`docs/spec/wal.md` §11; `docs/rules/rules.md` §3, §3a.

## 0. What DO is

`[operator]` In a conversation about the instance's memory layout, after
CLA's account of CW's C1 and its question whether C1 pays without the
rest of CW, CLA proposed cutting "the structure latch's long holds removed"
out as a small milestone of its own and asked whether to write it. The
operator answered *"Yes."* (given in Korean; recorded in English).

`[design]` `DevicePageStore`'s structure latch, `frames_latch_`, is one
`std::mutex` for the instance. Every page fetch and every unpin on every
core takes it, and so does every path that walks the whole frame table.
**A hold is a stall for every core's fetch**, whatever page that fetch is
for. So the latch's cost has two parts that need different remedies:

1. **the longest holds** - the walks over every resident frame - which put
   a stall of a size proportional to the pool into every core's tail;
2. **the per-fetch holds** - three for a read, more for a write - whose
   cost is the latch's cache line moving between cores, unmeasured.

CW's C1 answers both by partitioning the latch. DO answers what can be
answered **without** partitioning: it shortens the long holds and removes
the redundant work inside the short ones. Nothing in DO adds a latch,
changes an acquisition order, or touches `cores = 1`.

**What CLA told the operator, and what the tree corrects.** CLA said that
`EvictColdFrames` holds the latch across full laps of the pool in
production. **That is false.** `EvictColdFrames` has test callers only
(`tests/eviction_test.cpp`, `tests/frame_table_race_test.cpp`). The
production sweeps run in bounded batches, one batch per hold (§1.2). CLA
also proposed making the hit path's usage bump read before it writes. **The
tree already does that**: the bump is taken only below the cap
(`src/storage/device_page_store.cpp:1089`). Neither claim is a candidate
here.

## 1. The tree as read

### 1.1 What is measured `[measured]`

| measurement | reading |
|---|---|
| BP/BQ close, `v2.7.0-924-g3e77e7d1`, `cores = 4`, `relaxed`, 8 sessions (CW §1.1) | frame-table wait `0.73-0.75 us` per `point` statement, `0.45-0.51 us` per `insert1`; every named wait together about `0.55 %` of a `136-139 us` mean |
| multicore re-measurement, `v3.2.0-64-g9b7d53d7` (the pending entry) | `cores = 4` is `21-28 %` under `cores = 2` on `point` and `insert1`; p95 and p99 are 2-4 times `cores = 2`'s; unattributed |
| AM-S6 (`results-am-s6-m1-baseline-v2.7.0-286-g1e7148f.md`) | the noise floor on the host then used: about 7 % at one core, about 50 % at eight |

**Not measured anywhere:** how long any hold of the structure latch is.
The instrument counts a wait, and only on the waiter's side
(`include/kds/base/latch.hpp:70-80`, `include/kds/base/contention.hpp:5-17`).
No hold is timed, and no results file says how long a checkpoint's
dirty-table walk holds the latch, at any pool size.

### 1.2 What the tree does `[source-read]` at `3a07ad49`

**What the latch guards.** `frames_` (`std::unordered_map<PageId, Frame>`,
`device_page_store.hpp:1783`), the slot array and its free list,
`clock_hand_`, `live_pins_`, `pin_high_water_`, `dirty_eviction_queue_`,
`window_promised_`, `dirty_gens_`, and each frame's `pins`, `dirty`,
`rec_lsn`, `writing` and `dirty_gen` (`device_page_store.hpp:1611-1639`).
It is armed only at `cores > 1`; unarmed it is null and costs a null test
(`:1639`).

**The holds proportional to the pool (whole-table walks).**

| id | site | what it does under the hold | who calls it, how often |
|---|---|---|---|
| H1 | `DirtyPagesWithRecLsn` (`device_page_store.cpp:2402-2412`) | `reserve(frames_.size())`, every resident frame visited, **then the result sorted** | a checkpoint's start, through `PageStoreCheckpointTarget::DirtyTable` (`page_store_checkpoint_target.hpp:35-41`, `src/wal/checkpointer.cpp:205`); one cadence run every `checkpoint_interval_ns` (5 s by default, `include/kds/server/expeditor.hpp:202`), the cores in turn |
| H2 | `DirtyPageIds` (`:2389-2400`) | the same walk and the same sort; its comment says "there is nothing to hoist out", which is true of the walk and not of the sort | a checkpoint's publish (`checkpointer.cpp:627`), every run |
| H3 | `frames_` growth | the map is never reserved (no `reserve`/`rehash` call in the file), so a rehash of every node runs under the hold each time the map passes its load factor | warm-up only: about `log2(capacity)` times until the pool is full, then never |
| H4 | the refusal census (`:902-908`) | every frame visited to print the `ResourceExhausted` message | only on a refusal |
| H5 | `resident_class_frames`, `pinned_frames` (`:1044-1054`, `:3162-3172`) | every frame visited | boot and tests |

**The holds that are bounded, and why DO leaves them.** A reservation's
sweep and `MaintainFreeReserve`'s run `SweepLocked` for at most
`ReclaimBatch()` reclaims - `clamp(capacity / 16, 1, 64)` - in at most
eight steps a frame, one batch a hold (`device_page_store.hpp:423-426`,
`device_page_store.cpp:952-953`, `:2324-2325`). That is at most 512 slot
visits a hold. `EvictColdFrames`'s full laps (`:3173-3183`) have test
callers only.

**The per-fetch holds.** A read of a resident page at `cores > 1`:

1. **Hold 1** (`FetchAndPin`, `:2638-2657`): `frames_.find` for the
   resident test, `loading_.find`, then `Resolve` (`:1074-1085`), which
   asks `IsAllocated` - a nested `map_latch_` hold - and does
   `frames_.find` again, then `PinResidentAndRelease`, which does
   `frames_.find` a third time and counts the pin. **Three hash lookups
   and a nested latch in one hold.**
2. The page latch, by compare-and-swap.
3. **Hold 2** (`UnpinFrame`, `:2973-2992`): `frames_.find` to get the
   frame back.
4. The page latch released.
5. **Hold 3** (`:3012-3017`): the pin uncounted.

A write fetch adds a hold for the dirty mark after the exclusive grant
(`AcquirePageLatch`, `:2966-2969`). It adds another, with a fourth lookup,
for `LogOwedImage`'s test of `owes_image` (`:2504-2514`), which is H39 of
the third-run audit. And it adds `StampPageLsn`'s hold after the append
(`:1816-1840`).

**Two premises the candidates rest on.**

- **No eraser removes a pinned frame.** `FreePage` returns `kDeferred` on
  a pin, a writeback claim or a held latch (`:1741-1744`).
  `ReleaseScanSlot` returns on a pin (`:1203-1212`). The sweep skips a pin
  (`:3213`). A writeback already holds a raw `Frame*` from its copy to its
  clean on exactly this ground (`:1738-1740`). `std::unordered_map` keeps
  an element's address across a rehash.
- **A resident frame implies an allocated page.** `FreePage` erases the
  frame and clears the bit in one hold of both latches (BF-R5); the audit's
  reading of `ReleaseClaim` and of a fault's `loading_` entry is that no
  other path clears a bit under a resident frame.

**The checkpoint's snapshot.** `Checkpointer::Start` takes the
active-transaction table and the dirty table, then appends
`CHECKPOINT_BEGIN`, with no suspension between them (CZ-R3,
`checkpointer.cpp:198-206`). A page that another core dirties and logs
between the snapshot and the append is absent from the table with a
recLSN below `begin_lsn_`. Two things cover it: each core's fold, which
keeps that core's last redo start until its next run snapshots the page,
and the publish, which writes every page dirty before the anchor's page.
The code says both rest on no core yielding between an append and its
stamp (`checkpointer.cpp:240-256`). DO's E2 widens that window. It does
not add a new kind of page to it. That is CLA's reading, and §3's DO-R1
does not let the reading stand in for a proof.

## 2. The candidates `[design]`

Each one is priced at DO-S0. None is chosen here.

- **E0 - nothing.** The verdict if no hold is long enough to matter at the
  pool sizes DO-S0 measures.
- **E1 - the sorts out of H1 and H2.** The walk copies under the hold and
  the sort runs after it. The same frames go into the same table, so the
  output is identical and the snapshot is the same instant. The
  `reserve` moves out too, sized from `capacity_`, which needs no latch.
  No behaviour change.
- **E2 - H1 and H2 walked in chunks** `[quiet-wrong]`. The slot array
  (`kFrameChunk` = 1,024 slots) is walked a chunk per hold, with the latch
  released between chunks. Nothing else is held across the gap, and the
  checkpoint's task does not suspend in it, so CZ-R3's "no suspension"
  still holds. **What changes**: the dirty table stops being one instant's
  and becomes a fuzzy one, the ARIES shape. E2 is built only if E1's
  measurement leaves the walk itself over DO-Q6's bar, and only past
  DO-S3's gate.
- **E3 - `frames_` reserved at `Open`** to the configured capacity,
  removing H3. The cost is the bucket array, eight bytes a frame: 8 MiB at
  a million frames, against 8 GiB of pages. No behaviour change.
- **E4 - the per-fetch holds' redundant work**:
  - **E4a - H39.** The owed image decided inside the hold that the
    exclusive grant's dirty mark already takes (`:2966-2969`), so
    `LogOwedImage` loses its own hold and its lookup, and `CreatePinned`'s
    separate clear (`:2798-2799`) folds into its neighbour. One hold fewer
    on every write fetch.
  - **E4b - the hit path's `IsAllocated` dropped** on the second premise
    above, with a debug assertion in its place. The nested `map_latch_`
    hold leaves every hit. `[quiet-wrong]` only if the premise is false,
    so DO-S0 re-reads every path that clears a bit.
  - **E4c - one lookup per hit.** `FetchAndPin`'s resident test, `Resolve`
    and `PinResidentAndRelease` share one iterator, so the hold does one
    hash lookup in place of three. No behaviour change.
  - **E4d - the frame carried by the handle.** `PageRef` keeps the
    `Frame*` it was pinned on, valid while its pin stands (the first
    premise). `UnpinFrame` then needs no lookup and no first hold: it
    releases the page latch and uncounts the pin in one hold. **A read
    goes from three holds to two.** The pin's ordering - the page latch
    released before the pin drops - is kept. A debug check asserts that
    every eraser saw `pins == 0`.

**Not in DO.** These are CW's C1, C2 and C3: the latch partitioned, an
atomic pin, a pin dropped with no hold, any read without the page latch.
So are a lock-free frame table, a change to the CLOCK policy, and the
other instance latches, which are separate questions (the window, the
lock table's alignment, the WAL stream).

## 3. Rules

Binding from DO-S1 on.

- **DO-R1 `[quiet-wrong]` - the checkpoint's dirty table stays a correct
  input to `RedoStartFrom`.** No candidate changes what the table is a
  snapshot of without a written proof, the operator's mark on it, and a
  crash cell. The cell uses a seam to force a page dirtied and logged in a
  chunk already passed, below `begin_lsn_`, then crashes before and after
  the anchor. E1, E3 and E4 change no table and need no proof. E2 does.
- **DO-R2 - `cores = 1` is untouched** (AM's G2). An unarmed store takes
  no new branch on the hit path that it did not take before. The
  one-core cells are measured, and they must stay inside the floor.
- **DO-R3 - no new latch, and no change to `rules.md` §3's order rows.**
  E2 releases the structure latch and takes it again, holding nothing
  across the gap; E4a moves a test into a hold that already exists. If a
  candidate needs a new row, it is not DO's.
- **DO-R4 - the hold instrument obeys `rules.md` §3a.** It is written only
  by the holder's own core, into a per-core, cache-line-aligned slot, the
  `Contention` shape. It adds no line that two cores write. It times only
  the sites DO-Q1 names in a release build.
- **DO-R5 - each candidate lands alone, on its own A/B.** A candidate that
  does not meet DO-Q6's bar is recorded with its numbers and not landed.
  E1, E3 and E4c change no behaviour and are measured anyway.
- **DO-R6 - a premise a candidate rests on is asserted.** E4b has an
  assertion that a resident frame's bit is set, and E4d one that no
  eraser meets a pin. Both run in debug builds and under
  `KDS_TEST_PAGE_LATCH=1`, so the whole suite runs them armed.

## 4. Items for the operator (DO-Q), CLA's proposals

- **DO-Q0 - the order's kind.** (a) **not exploratory**: E1, E3 and
  E4a-d land on their own measurement (DO-R5), and only E2 is gated as an
  exploration; (b) exploratory throughout: DO-S0 may close the order with
  nothing built. CLA proposes (a). **Adopted (a) on the operator's standing go-ahead of 2026-11-21 (go-ahead-achieving-milestone)**, at DO's opening; not the operator's own mark.
- **DO-Q1 - the hold instrument.** (a) the whole-table sites (H1-H5)
  timed in every build - longest hold, and counts over 100 us, 1 ms and
  10 ms - with every structure-latch site timed only in a bench build;
  (b) every site in every build. The per-fetch sites are three to six
  holds a statement, and two clock reads each is a cost on the path being
  measured. CLA proposes (a). **Adopted (a) on the operator's standing go-ahead of 2026-11-21 (go-ahead-achieving-milestone)**, at DO's opening; not the operator's own mark.
- **DO-Q2 - E2's chunk.** (a) `kFrameChunk` (1,024 slots), so a chunk is
  the unit the array already grows by; (b) a setting. CLA proposes (a). **Adopted (a) on the operator's standing go-ahead of 2026-11-21 (go-ahead-achieving-milestone)**, at DO's opening; not the operator's own mark.
- **DO-Q3 - E2's gate.** (a) E2 is built only past DO-S3: E1 measured
  first, the walk alone still over DO-Q6's bar, a written proof under
  DO-R1, and the operator's own mark on the proof; (b) built with E1.
  CLA proposes (a). **Adopted (a) on the operator's standing go-ahead of 2026-11-21 (go-ahead-achieving-milestone)**, at DO's opening; not the operator's own mark.
- **DO-Q4 - E3's size.** (a) `frames_` reserved to `capacity_` at `Open`;
  (b) to the resident-class floor plus 256, growing past it as today.
  CLA proposes (a). **Adopted (a) on the operator's standing go-ahead of 2026-11-21 (go-ahead-achieving-milestone)**, at DO's opening; not the operator's own mark.
- **DO-Q5 - E4d's reach.** (a) `PageRef` carries the `Frame*` for every
  pinned handle, with the eraser assertion (DO-R6); (b) E4d left to CW's
  C1, where the frame descriptor array makes it natural. CLA proposes (a):
  the premise holds today, and E4d needs no descriptor array. **Adopted (a) on the operator's standing go-ahead of 2026-11-21 (go-ahead-achieving-milestone)**, at DO's opening; not the operator's own mark.
- **DO-Q6 - the bars.** (a) **for H1-H3**: the longest structure-latch
  hold during a checkpoint, at 64 Ki, 256 Ki and 1 Mi resident frames,
  falls to the bar the operator sets at DO-S0's report. **For E4**: the
  structure-latch holds a statement takes - a count, which is exact on any
  host - fall by the candidate's arithmetic, with no regression in
  throughput or p99 beyond the host's floor at `cores = 1, 2, 4`. A
  throughput gain is reported, not required, because the eight-core floor
  is about 50 %. (b) a throughput gain required. CLA proposes (a). **Adopted (a) on the operator's standing go-ahead of 2026-11-21 (go-ahead-achieving-milestone)**, at DO's opening; not the operator's own mark.
- **DO-Q7 - the host.** (a) the counts and hold times on the operator's
  host as it is, and throughput as a regression check only; (b) a host
  with eight or more cores and a measured floor below 10 % before DO-S0.
  CLA proposes (a), and records that DO cannot say what E4 is worth in
  throughput on such a host. **Adopted (a) on the operator's standing go-ahead of 2026-11-21 (go-ahead-achieving-milestone)**, at DO's opening; not the operator's own mark.
- **DO-Q8 - CW.** (a) DO's results file is entered in CW §1.1 at DO's
  close, and C1 in CW §2 is amended to read "after DO", so that C1 is
  priced against the tree DO leaves; (b) CW is untouched. CLA proposes (a). **Adopted (a) on the operator's standing go-ahead of 2026-11-21 (go-ahead-achieving-milestone)**, at DO's opening; not the operator's own mark.
- **DO-Q9 - the bar's value for H1-H3** (raised by DO-S0, which DO-Q6 (a)
  leaves to the operator at DO-S0's report). (a) **no structure-latch hold
  at a checkpoint longer than 1 ms at 1 Mi resident frames** - eight times
  a `point` statement's p99 at `cores = 2` (130 us) and the instrument's
  middle threshold, so `contention_frame_table_walks_over_1ms` reads it in
  production; (b) 10 ms, which E1 alone may meet; (c) 100 us. CLA proposes
  (a): a hold is a stall of every core's fetch, and DO-S0 measured 38 ms at
  1 Mi frames (`bench/v3.5.0/results-do-s0-census-v3.5.0-55-g48a3276c.md`
  §1); (b) would leave a fetch on every core waiting tens of statements'
  time, and (c) is under what a 1 Mi-frame walk can do in one hold on this
  host even chunked. **Adopted (a) on the operator's standing go-ahead of
  2026-11-21 (go-ahead-achieving-milestone)**, at DO-S0; not the
  operator's own mark. It is DO-Q6 (a)'s number, so DO-Q3 (a)'s gate - E2
  past a proof and the operator's own mark - stands unchanged.
- **DO-Q10 - does a candidate that improves every cell but alone misses
  the bar land?** (raised by DO-S1). DO-R5 says a candidate that does not
  meet DO-Q6's bar is recorded and not landed, and in the same breath that
  E1, E3 and E4c change no behaviour and are measured anyway. (a) **E1 and
  E3 land**: they change no output, cut every hold they touch by 60-82 %,
  and DO-Q3 (a) itself presupposes E1 measured and in the tree before E2 is
  priced against what is left - the bar for H1-H3 is E1 and E2's together;
  (b) E1 and E3 held back until E2 meets the bar with them. CLA proposes
  (a). **Adopted (a) on the operator's standing go-ahead of 2026-11-21
  (go-ahead-achieving-milestone)**, at DO-S1; not the operator's own mark.

## 5. Stages

- **DO-S0 - census and price.**
  1. List every structure-latch hold site: the `LatchGuard`s and
     `unique_lock`s in `device_page_store.cpp`, 40 at
     `3a07ad49`. Classify each as per-fetch, periodic or rare, and
     confirm §1.2's table.
  2. Re-read every path that clears a free-map bit or erases a frame,
     for E4b's and E4d's premises, and record the result.
  3. Build DO-Q1's instrument.
  4. Measure, on the tree as it is:
     - hold times for H1-H3 at the three pool sizes, with a dirty-heavy
       update workload so that the checkpoint has a table to walk;
     - `frame_table` waits and their longest;
     - `sched_hold_*_longest` with and without a checkpoint run
       overlapping;
     - structure-latch holds per statement for `point`, `insert1` and an
       `UPDATE` by tuple key;
     - `cores = 1, 2, 4`.
  5. Report a table for the operator's bar (DO-Q6).
- **DO-S1 - E1 and E3.** Built, A/B against DO-S0's tree, and landed or
  recorded under DO-R5.
- **DO-S2 - E4a, E4b, E4c, E4d.** Each a commit of its own, with its
  assertion (DO-R6) and its count. Measured as DO-Q6 (a) says.
- **DO-S3 - E2's gate.** Reached only if DO-S1's numbers leave the walk
  itself over the bar. CLA writes the proof under DO-R1 and its crash
  cells; the operator marks it or does not; E2 is built only on the mark,
  and measured like E1.
- **DO-S4 - the close.** One results file. CW §1.1 and §2 amended
  (DO-Q8). `known-gaps.md` updated with what was not built.

## 6. What DO does not do

It partitions no latch and makes no pin atomic. It reads no page without
its latch, and changes no format, no record and no setting. It does not
re-measure or attribute the `cores = 4` tail; it reports whether a long
hold coincides with it, and leaves the attribution to that entry. It does
not touch the other instance latches.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-21

On `main` at `3a07ad49`, documents only. Nothing is built or measured. No
review run and no suite executed: no build input changed. Two claims CLA
made in conversation are corrected in §0: `EvictColdFrames` is not a
production path, and the usage bump already reads before it writes.

### Opened 2026-11-21, on the standing go-ahead

On the operator's `/go-ahead-achieving-milestone DO`
(`raft-marks-2026-11-21.md` §12), from `48a3276c`. The session's own
worktree `dg-wal-segment-body` (DG's, closed) carries the new branch
`worktree-do-frame-table-long-holds` from `origin/main`: the session is
pinned to that worktree, so no worktree named for DO could be created, and
DG's branch and its unpushed record `e98163d2` are untouched. DO-Q0..Q8
adopted (a) as CLA proposed; none is the operator's own mark.

### DO-S0 - census and price, done 2026-11-21

`bench/v3.5.0/results-do-s0-census-v3.5.0-55-g48a3276c.md`. The
instrument (DO-Q1 (a)): `frame_table_walks`, `frame_table_walk_us`,
`frame_table_walks_over_{100us,1ms,10ms}` and
`frame_table_walk_longest_us` in `sys.meta`'s contention section, timed at
every whole-table hold of an armed store (H1-H6) and at an insert that
rehashes (H3); a bench build, `-DKDS_LATCH_HOLD_CENSUS=ON`, counting and
timing every hold of every latch kind; and `bench/frame_table_hold_bench.cpp`.

- **H1 and H2 are long and the sort is inside them**: at 64 Ki, 256 Ki and
  1 Mi resident dirty frames H1 holds 1.7, 7.0 and 37.6 ms (p50) and H2
  1.3, 5.6 and 30.1 ms; H3's longest rehash is 0.8, 3.4 and 12.1 ms. A
  fetch on another core waits the hold out whole (47 ms at 1 Mi).
- **A read hit is three holds, exactly**; a `point` statement 12.0, an
  `insert1` 24.6 and an `UPDATE` by tuple key about 25, at `cores = 2`.
- **H6, a sixth whole-table walk §1.2 did not list**: `Flush`'s
  collection, under `Sync` (the synchronous checkpoint's publish, a
  shutdown's, a backup's). Timed with the rest; no candidate changes it.
- Both premises (E4b's, E4d's) re-read at `48a3276c` and hold (results
  §3).
- **DO-Q9 added** (the bar's value, which DO-Q6 (a) left to DO-S0's
  report) and adopted (a), 1 ms at 1 Mi frames, on the standing go-ahead.
- Cut against the 20-minute budget, and said so in the results file: the
  hold times are the microbenchmark's, not an engine run's; `cores = 1, 4`
  not run for the counts; `sched_hold_*_longest` not read.
- **Reviewed by `critics-developer`**: no correctness bug. Applied: the
  guard and the timer merged into one `WalkHold` (the declaration-order
  hazard gone), the rehash predicate in `double` as libstdc++'s
  `_M_need_rehash` (`WouldRehash`, exact above 2^24 frames), the census
  build's record moved after the release with one clock read on the
  contended path, and the benchmark's fill reading two snapshots an insert
  in place of four. Noted in the results file: threads with no core write
  slot 0. Not taken: merging `WalkTimer` with `StallTimer` (the records
  differ, as the review itself says).

### DO-S1 - E1 and E3, done 2026-11-21

`bench/v3.5.0/results-do-s1-e1-e3-v3.5.0-56-g47f7016c.md`, interleaved
A/B against DO-S0's `47f7016c`. E1: H1's and H2's vector reserved before
the hold (from `configured_capacity_`, which never moves after `Open`; §2
and DO-Q4 (a) said `capacity_`, which does) and sorted after it; `Flush`'s
collection (H6) is `DirtyPageIds()`. E3: `frames_` reserved at `Open`, an
insert asserting it does not rehash (DO-R6).

- **Every hold cut**: H1 1,658 to 497 us, 7,415 to 1,876 us and 38,675 to
  15,372 us at 64 Ki, 256 Ki and 1 Mi frames; H2 1,269 to 231 us, 5,631 to
  1,173 us and 29,580 to 8,740 us; H3's rehashes, up to 12.5 ms, gone.
- **DO-Q9 (a)'s bar not met** from 256 Ki frames: the walk itself is left,
  15 ms and 9 ms at 1 Mi. DO-S3 is reached (DO-Q3 (a)).
- **DO-Q10 added and adopted (a)**: E1 and E3 land though alone they miss
  the bar.
- Tried and not taken: the output vector written through before the hold
  (H1 -30 %, H2 +25 %, a whole-capacity write a checkpoint).
- **Reviewed by `critics-developer`**: no bug; E1's output and instant
  confirmed identical (DO-R1), `configured_capacity_` confirmed written
  once and an upper bound. Applied: `Flush` through `DirtyPageIds()`; the
  H3 timer replaced by the assertion; four comments that had become false
  (`WouldRehash`'s, the manual's list of walks, `capacity_`'s "set once",
  and the work order's `capacity_`, recorded here). Recorded, not changed:
  the per-call reserve is address space, an `mmap` above 32 MiB, outside
  the hold.
- **Suite**: 0 failed of 5094 on E1 and E3 before the review's cuts; the
  cuts are covered by DO-S2's first run. Overhead not measured; measured
  at the milestone's close.
