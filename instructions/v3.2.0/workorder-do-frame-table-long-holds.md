# Work order DO - the frame table's long holds: what the instance's one structure latch is held for, shortened where nothing moves, measured before and after

Status: **OPENED 2026-11-21 on the operator's standing go-ahead
(go-ahead-achieving-milestone, `raft-marks-2026-11-21.md` §12)**, on the
worktree `dg-wal-segment-body` (branch `worktree-do-frame-table-long-holds`)
from `48a3276c` (`v3.5.0-55-g48a3276c`). **DO-Q0..Q8 adopted (a) and DO-Q9
(a) added and adopted, as CLA proposed, on that go-ahead; none is the
operator's own per-item mark** (§4). **CLOSED 2026-11-22 on the work branch, and landed on `main` at `c22a1a95` the same day on "push no verify"** (`raft-marks-2026-11-22.md` §2), untagged: DO-S0 to DO-S2 built (`47f7016c` to `7116b668`) and landed on `main` at `bf289a82` (`raft-marks-2026-11-21.md` §13); **E2 built on the operator's own mark on §5a** (`raft-marks-2026-11-22.md` §1, DO-S3, §7); the close measured (DO-S4, and E2's own A/B). E2 is not landed: a push waits for the operator's word. Written on `main` at
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

## 5a. DO-S3: E2's proof under DO-R1 (CLA, for the operator's mark)

`[design]` when written, **marked by the operator on 2026-11-22**
(`raft-marks-2026-11-22.md` §1) and built (§7, DO-S3). Written at DO-S3 on
the worktree `dg-wal-segment-body` at `b8759946`, read against `src/wal/checkpointer.cpp` (`Start`, the paced
publish), `wal/analysis.hpp` (`RedoStartFrom`) and
`src/server/superblock_checkpoint_anchor.cpp` (the fold). Two corrections were
made in the build and are marked here as CLA's, not the operator's: the
merge keeps a page's lowest **non-zero** recLSN (a 0 is "nothing to
replay", so it must not hide a real one), and case 3 below covers a page
never visited at all.

**What E2 is, exactly.** H1 (`DirtyPagesWithRecLsn`) and H2
(`DirtyPageIds`) walk the slot array a chunk (`kFrameChunk`, 1,024 slots,
DO-Q2 (a)) per hold of the structure latch, taking each resident slot's
frame - its page id, `dirty`, `rec_lsn` - under that chunk's hold, and
releasing the latch between chunks with nothing held across the gap. The
checkpoint's task does not suspend in the walk (no `co_await` between the
first chunk and `CHECKPOINT_BEGIN`'s append), so CZ-R3 stands. The result
is sorted by page id and **deduplicated, keeping each page's lowest
non-zero recLSN** (a page that leaves a slot not yet walked for one
already walked was clean when it left and is not met at all - case 3; one
that leaves a walked slot for a later one can be met twice; the minimum is
the conservative entry).

**What redo needs of the table.** The anchor's redo start `R` is the
minimum, over every core's last contribution to the fold, of
`RedoStartFrom(begin_lsn, table)` - the table's lowest non-zero recLSN,
floored at the run's `begin_lsn`. Recovery is correct iff **every record
with LSN below `R` is in the image of its page that the device holds when
the anchor lands**. Call a record *exposed* if it is below this run's own
`RedoStartFrom` and its page's device image lacks it when the anchor lands.

**Claim.** E2 exposes no record the whole-table snapshot of today does not
already expose to the same two covers; and the covers hold for it.

**Proof.** Take a record `r` on page `P` with `lsn(r) < RedoStartFrom(begin_lsn, table)`.
Since the result is floored at `begin_lsn`, `lsn(r) < begin_lsn`: `r` was
appended before `CHECKPOINT_BEGIN`. Let `v` be the instant P's slot was
visited (its chunk's hold), if P was resident then.

1. *P resident and dirty at `v`, with `rec_lsn(P) != 0` and
   `rec_lsn(P) <= lsn(r)`.* Then the table holds an entry `<= lsn(r)`, so
   `RedoStartFrom <= lsn(r)`: contradiction. Not exposed.
2. *P resident and dirty at `v` with `rec_lsn(P) > lsn(r)`, or `0`.* A
   frame's `rec_lsn` is the minimum stamp since its last clean
   (`StampPageLsn`, `WriteBack`), so `r` is either already on the device
   (written by a copy before the clean that started this dirty span) or
   not yet stamped at `v` - appended, or stamped, after `v`. The first is
   not exposed. The second is case 4.
3. *P not resident, or resident and clean, at `v` - or never visited*,
   having moved from a slot not yet walked to one already walked, which
   only a clean frame can (only a clean frame is evicted). Every record
   stamped on P before its last clean is on the device. A record `r` not
   on the device was therefore stamped after `v`, or after P's move.
   Case 4.
4. *`r` stamped on P after `v` and appended before `CHECKPOINT_BEGIN`.*
   This is exactly the window the whole-table snapshot already has
   between its one hold and the append (`checkpointer.cpp`'s comment at
   the redo start, CZ-S0's census), widened from "after the snapshot's
   hold" to "after P's chunk's hold". Two covers, both unchanged by E2:
   - **The publish.** The run's anchor is encoded only after H2's walk,
     which starts after `CHECKPOINT_END` and so after `CHECKPOINT_BEGIN`.
     `r` was stamped before `BEGIN` (the stamp precedes the append's
     return on the appending core's reactor, which does not yield between
     an append and its stamp - the premise the code already names), so P
     was dirty with `r` before H2's walk began. At P's visit in H2 - under
     any chunking - P is either still dirty, and the publish writes it
     before the anchor's page, or was cleaned by a writeback copy that
     carried `r`. Either way `r` is on the device when the anchor lands.
     Chunking H2 changes nothing here: every chunk of it is after `BEGIN`.
   - **The fold**, for the anchor of a *later* run that does not re-walk
     P: the appending core `C`'s own contribution stays at its last run's
     redo start, which is `<=` that run's `begin_lsn`, which precedes `r`
     unless `C`'s last run began after `r` - in which case `C`'s run walked
     P with `r` stamped (case 1) or found it on the device. The
     checkpointing core itself appends nothing inside its own walk, since
     its task does not suspend there, so `C` is never the core whose
     contribution this run replaces in the window.
5. *Duplicates.* Deduplication by minimum recLSN keeps case 1's
   inequality for every entry, and `RedoStartFrom` takes a minimum, so a
   duplicate can only lower `R`.

So the only records E2 adds to the window are of case 4's kind, which the
publish covers within the run and the fold across runs, exactly as for
today's window. **Where the premise would fail**: a core that yields
between an append and its stamp (already named in the code as the
premise of both covers), and a checkpoint task that suspends inside the
walk (E2 must not add one: `Start` calls the two snapshots and the
append as plain functions today, and E2 keeps the chunk loop inside
`DirtyPagesWithRecLsn`, a non-coroutine, so no suspension point can enter
it without changing its signature).

**The crash cells DO-R1 asks for** (built with E2, not before):
`DirtyTableChunkedWalkTest` with a test seam after a chunk's hold
(`SetAfterDirtyTableChunkForTest`): (i) a page in an already-walked
chunk dirtied, logged and stamped from a second core below `begin_lsn`,
then a crash before the anchor lands - recovery from the previous anchor
replays it; (ii) the same, crash after the anchor lands - the publish
wrote it, and the page reads back whole; (iii) a page evicted from an
walked chunk and re-faulted into a later one, dirtied - one entry, the
lower non-zero recLSN; (iv) the same cells at `cores = 1`, where the store is
unarmed and E2 is the one-hold walk it is today (DO-R2).

**What it buys** (DO-S1's numbers): the hold falls from the whole walk,
15 ms (H1) and 9 ms (H2) at 1 Mi frames, to one chunk's, about
`15 ms / 1,024` - some 15 us plus a chunk's frame lookups - well under
DO-Q9 (a)'s 1 ms. Its cost: one `frames_.find` per resident slot, the
slot array naming a page and not a frame; DO-S3's build would measure it.

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

### DO-S2 - E4a (H39), done 2026-11-21

The write fetch's image debt taken in the hold the exclusive grant already
takes for its dirty mark (`AcquirePageLatch`'s `owed`), armed only; the
unarmed fetch still asks `LogOwedImage` (DO-R2). `CreatePinned`'s separate
clear folded into the same hold. **A write hit is four structure-latch
holds in place of five, exactly** (`build-census`'s count phase of
`bench/frame_table_hold_bench.cpp`, 100,000 write hits on one thread with
images on: 5.000 at `b8759946`, 4.000 here); a read hit stays three.

- **Reviewed by `critics-developer`**: no bug; the bit read at the same
  point of the protocol (after the exclusive grant, under the structure
  latch), the debt never taken on a pin that is then lost, `drop_always`
  the old clear exactly, the unarmed path unchanged. Applied: the test gap
  it found - `FetchImageTest` never ran armed, so the new path had no
  direct cell; it is now parameterised over unarmed and armed (14 cells,
  all pass) - an assertion that a write fetch is exclusive, and a comment
  in the past tense. Not taken: an enum for `owed`/`drop_always` (it would
  save nothing, the pointer still has to travel), a helper for the "owed
  and images on" test (two sites), and deleting `LogOwedImage` by taking
  the debt from the unarmed `PinFrame`'s frame (H39's own suggestion, but
  it changes the unarmed path, which DO-R2 keeps untouched).
- **Suite**: 0 failed of 5094 on E4a before the review's additions;
  `FetchImageTest`'s 14 cells and the eviction and frame-table suites pass
  on the commit. The full suite runs again at E4b. Overhead not measured;
  measured at the milestone's close.

### DO-S2 - E4b, done 2026-11-21

The armed hit asks no `IsAllocated`, so no nested free-map hold: one
`frames_.find` serves the resident test and `HitLocked` (the hit's touch,
factored out of `Resolve`, which the unarmed path still runs whole,
DO-R2); `assert(IsAllocated(...))` in its place (DO-R6). The audit's G56
(`docs/pending/whole-engine-audit-second-run-twelve-slices.md`), noted
there as taken. **Measured against `a97bd952`**, on 64 Ki frames, one
thread: structure-latch hold time a read hit **212 to 121 ns** (`build-census`,
1,000,000 hits; a write hit 241 to 152 ns), holds unchanged at 3 and 4; and
one reader's fetch rate **3.44 to 3.71 million a second (+7.9 %)**, p50
0.24 to 0.22 us, three interleaved rounds in `build-release` at load
5.4-7.1 (another session's). **The number carries one of E4c's lookups**:
bypassing `Resolve` drops its `find`, so a hit is two lookups here, and
E4c removes the third.

- **Reviewed by `critics-developer`**: no bug; every path that clears a
  free-map bit re-read at `a97bd952` (four in the store, one offline) and
  the premise holds for every caller that fetches only ids it holds; the
  debug assertion's order is the declared one. Applied: the comment at the
  assertion now says the one exposure - a stale fetch of a claimed id
  given back unpublished, a defect class `InsertFrame`'s create refusal
  already guards, would now be served in a release build where it used to
  answer `NotFound`; four comments and docs that gave the hit's
  `IsAllocated` as the second latch's reason reworded (`FetchAndPin`,
  `FreePage`, the header, `page.md` §5, `rules.md` §3), the order
  unchanged (DO-R3); `HitLocked`'s comment. Not changed: the unlatched
  `*Unpinned` accessors' `Resolve` (tests only, a race of its own already).
- **Suite**: 0 failed of 5101 on the code before the review's comment
  changes. Overhead not measured; measured at the milestone's close.

### DO-S2 - E4c, done 2026-11-21

One hash lookup a hit: the iterator found for the resident test also
serves the touch and the pin (`PinFrameAndRelease`, the pin tail on a
frame the caller holds under the hold), and the fault arm pins the frame
its own lookup finds. **Measured against `5da99054`** on 64 Ki frames, one
thread: hold time a read hit 122.5 to 121.4 ns (`build-census`) and one
reader 3.73 to 3.83 million fetches a second (+2.5 %, three interleaved
rounds, `build-release`) - most of the lookup saving was already E4b's
(its `Resolve` lookup); no regression, so it lands as the no-behaviour
change DO-R5 measures anyway.

- **Reviewed by `critics-developer`**: no bug; the latch held from the
  `find` to the pin on every hit, the dropped `nullopt` arm unreachable,
  the three branches (hit, wait, fault) the old ones exactly. Applied:
  `PinResidentAndRelease`, left with one caller, inlined into the fault arm
  and deleted with its `std::optional` (its AM-S2-P F-1 note moved beside
  the fault's pin); the `loading_.empty()` guard dropped - libstdc++ 14's
  `find` already returns at once on an empty set, so it saved nothing; a
  doubled comment. Its header comment, which said the tail had two
  callers, rewritten.
- **Suite**: 0 failed of 5101 before the review's cuts; after them the
  store's suites (151 cells, `KDS_TEST_PAGE_LATCH=1`) pass, and the full
  suite runs again at E4d. Overhead not measured; measured at the
  milestone's close.

### DO-S2 - E4d, done 2026-11-21

`PageRef` carries the frame its pin landed on (an opaque `void*`, DO-Q5
(a)): `FetchPinned` and `CreatePinned` hand it back through an out
parameter the caller nulls, and the handle's release calls
`UnpinFrameAt`, which releases the page latch and uncounts the pin in one
structure-latch hold with no lookup. `UnpinFrame`'s armed tail is
`UnpinFrameAt`, so the release order has one definition. An unlatched pin,
the scan ring and the writeback still release by id. `ReleaseFrameLocked`
asserts that no eraser takes a pinned frame (DO-R6). **A read hit is two
structure-latch holds in place of three, and a write hit three in place
of four, exactly**; against `9e8c5a81` on 64 Ki frames, one thread: hold
time a read hit 121.7 to 80.7 ns (`build-census`) and one reader 3.84 to
4.14 million fetches a second (**+8.0 %**, three interleaved rounds,
`build-release`, load 5.4-7.1, another session's).

- **Reviewed by `critics-developer`**: no bug; every token is the frame
  the pin was counted on (both fetch arms, the create, its refetch
  fallback, the claims path), the three erasers all refuse a pin, moved
  and released handles clear the token, no other store overrides the
  changed virtuals. Applied: the release order's two copies collapsed into
  `UnpinFrameAt`; the out parameter written on success only, which drops
  the one new test on the unarmed fetch path (DO-R2); two comments.
  Recorded, not changed: `PageRef` grows from 24 to 32 bytes for every
  store, the cost DO-Q5 (a) carries; the unarmed release gains one null
  test in `PageRef::Release`; the scan ring's and the writeback's unpins
  and `PageRef::MarkDirty` still look the frame up by id (outside E4d).
- **Suite**: 0 failed of 5101 on E4d before the review's cuts; after them
  the store's 231 cells pass under `KDS_TEST_PAGE_LATCH=1`, and the full
  suite runs again on the merged tree at the close. Overhead not measured
  here; measured at the milestone's close.

### DO-S4 - the close measured, 2026-11-21; DO not closed: DO-S3 waits for the operator

`bench/v3.5.0/results-do-close-v3.5.0-61-g7116b668.md`, run by `ck-tester`:
interleaved A/B, A `48a3276c` (`v3.5.0-55-g48a3276c`, the commit DO opened
from) against B `7116b668` (`v3.5.0-61-g7116b668`), `tools/ba_census.py`,
`point`, `insert1` and `pkmix` at `cores = 1, 2, 4`, `relaxed`, three
runs an arm a cell, 54 runs; release copies, `/dev/nvme0n1p1`; another
session's workload on the host throughout (its load recorded per run).

- **DO-Q6 (a)'s E4 clause is met in all nine cells**: no throughput or p99
  regression beyond the host's spread; `pkmix` at `cores = 2` +5.4 % and
  `insert1` at `cores = 4` +2.2 % with the ranges disjoint; `insert1` at
  `cores = 2` -5.3 %, inside A's own three-run spread and unresolved;
  `cores = 1` level (+0.2 to +0.9 %, DO-R2).
- **The frame table's wait a statement fell 26-46 % in every row above
  one core**, the arms' ranges disjoint. Three readers on one store (the
  microbenchmark, added by CLA): +27 % fetches a second, p99 -13 %.
- **H1-H3**: DO-Q9 (a)'s bar is not met by what is built (H1 15 ms, H2
  9 ms at 1 Mi frames); E2 is what would meet it.
- **DO-Q8 (a) done**: the results entered in CW §1.1 and C1 amended to
  read "after DO" (`workorder-cw-optimistic-page-read.md`).
- **Documents moved**: `page.md` §3 (the handle's frame), §8 (the dirty
  table's hold), §12 (two holds a read hit, three a write hit); `CLAUDE.md`'s
  buffer-pool row; `known-gaps.md` "The frame table's structure latch
  (DO)"; `client-manual.md` (the `contention_frame_table_walk*` fields).
  No SQL surface changed, so the manual is untouched.
- **Synced**: `origin/main` has not moved since `48a3276c`; the merge is
  a no-op.
- **What holds DO**: E2 (§5a). If the operator marks the proof, E2 is
  built with its crash cells and measured like E1, and DO closes; if not,
  DO closes as it stands, with H1 and H2 over the bar in `known-gaps.md`.

### DO-S3 - E2, built on the operator's mark, 2026-11-22; DO closed

The operator's *"go ahead"* after CLA's account of E2 (`raft-marks-2026-11-22.md`
§1), read as the mark §5a asked for. `bench/v3.5.0/results-do-s3-e2-v3.5.0-62-g1cfe6c0f.md`.

- **Built**: `ForEachDirtyFrame` walks the slot array a `kFrameChunk`
  (1,024 slots) a structure-latch hold, armed only, and returns after the
  last chunk; `DirtyPageIds` de-duplicates and `DirtyPagesWithRecLsn` merges
  to the lowest non-zero recLSN (`MergeDirtyTable`); a slot's frame is
  asserted to be its owner's (DO-R6). **CLA's addition**: a handover
  between chunks - `Latch` counts its waiters and the acquisitions that
  waited, on the contended path only - because E2 alone left a fetch
  waiting 12.5-12.9 ms at 1 Mi frames with no hold over 224 us (the walker
  re-took the latch before the waiter it had woken ran); the walker yields
  until one waiter is in, or 20 us.
- **The bar met** (DO-Q9 (a)): no hold over about 0.3 ms at 256 Ki and
  1 Mi frames, 30 us at 64 Ki, none over 1 ms. Against no E2 at 256 Ki and
  1 Mi: other cores' fetches a second +10-20 %, the longest fetch wait cut
  4-5 times (15.8-17.7 ms to 4.4 ms at 1 Mi). **The cost**: the walking
  core's H1 call about doubles (26 to 49 ms at 1 Mi under three readers),
  a stall of its own reactor, recorded in `known-gaps.md`.
- **The crash cells** (`tests/dirty_table_chunked_walk_test.cpp`): §5a's
  (i) and (ii) on `SimInstance`, the window's write run from the seam on
  the walking thread; (ii) shown to fail under a mutant in which the
  publish writes no page but page 0 (recovery started at LSN 27,720, past
  the update, and the row read its old value). (iii) is a unit test of the
  merge, and (iv) the unarmed walk's gap count: forcing a page to move
  between particular slots needs eviction control the store does not
  expose. The fold is exercised by no cell, since the window's writer is
  the walking thread.
- **Reviewed by `critics-developer`**: no engine bug; no dirty frame missed
  beyond case 4 (a slot's owner and its frame's slot written and cleared
  in one hold; a frame never changes slot; slots only appended), every
  caller suspension-free, the merge right, one core unchanged. Fixed by
  the reviewer: the unarmed cell skips under `KDS_TEST_PAGE_LATCH=1`, which
  arms every store. Applied: the handover waits for one waiter, not none
  (its finding: a steady stream is never empty, so the walk could spin
  20 us a chunk); the walk returns after its last chunk; the impossible
  slot/frame mismatch asserted; the merge one loop; the crash fixture
  asserts one chunk and its header says what the cells do and do not
  exercise; §5a's case 3 and cell (iii) wording corrected; `page.md` §8,
  `client-manual.md` and `known-gaps.md`, which the code now contradicted.
- **Suite**: 0 failed of 5107 on E2 before the review's changes, and 0
  failed of 5107 again on the commit's build inputs, after them.
- **The close**: DO-S4's engine A/B (`48a3276c` against `7116b668`) was not
  run again for E2. E2 changes only the checkpoint's dirty-table walks,
  and the census shapes' pools hold under one chunk resident, where the
  armed walk is one chunk; E2 is measured by its own A/B above.

**DO is closed** on the work branch: every stage built, E2 on the
operator's mark, the measurements recorded. It is not landed: the E2
commits wait for the operator's push.
