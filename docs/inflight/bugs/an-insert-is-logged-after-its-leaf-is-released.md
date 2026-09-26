# An insert is logged after its leaf is released

**Found by reading, not reproduced.** Verified at `974a844` on
`btree-lookup-held-pageref`, by the `critics-developer` pass on AT-0 item
12's stage, as the write side of the same window: a `(page, slot)` carried
past the hold that made it true (`btree.hpp` `Location`).

## What is wrong

`CommandDispatcher::InsertIntoRelation` places the row and releases the
leaf; `LogInsert` then appends, **after the hold is gone**, the full-page
images of every page the insert changed, the spills, the index writes,
`HEAP_INSERT(placed.page_id, placed.slot)`, and stamps the leaf's
`page_lsn` (`src/server/command_dispatcher.cpp`, `LogInsert`).

Both places that justify that order give one reason:
`include/kds/server/command_dispatcher.hpp`'s ordering note and
`docs/spec/wal.md` §11a - *"the server is a single cooperative thread, no
flush can interleave between the mutation and the `page_lsn` stamp"*. It
has been false since AT-S5 put a write on every core, and since AT-S10b
every core's trx-id carve flushes the pool from its own thread.

## Two windows

1. **The record names a slot another core renumbered.** Core A places row
   `r` in leaf `L` at slot `s` and lets go of `L`. Core B divides `L` and
   logs `L`'s image. Core A then appends `HEAP_INSERT(L, s)` at a later LSN.
   Redo applies B's image, then A's record on top of it: `r` re-inserted
   into a leaf that may no longer cover its key, or a second copy of `r`
   beside the one the divide moved.
2. **A writeback between the mutation and the stamp.** Another core's
   flush can write `L` after A's row is in the frame and before A's
   `page_lsn` stamp, so the store's WAL gate compares against the old
   LSN and lets the page reach the device ahead of its record. A crash
   there leaves `r` on disk with no log record naming it.

Heap relations have the same shape (`ChainInsert`), reached by two cores
growing or filling one chain.

## Cost

A quiet wrong answer after a crash: a duplicated row, a row in a leaf
that does not cover it, or an unlogged row surviving recovery. Nothing
wrong is visible without a restart.

## Reproduction (to write)

Two cores, one btree relation, below-mark named-pk inserts into one full
leaf; a barrier after A's placement and before A's `LogInsert`, released
after B's divide is logged; crash, remount, count rows by pk and walk the
chain's invariants.

## Fix, and who owns it

`wal.md` §8-1 already states it: generate the record while holding the
page latch. `InsertPlacement` would carry the held leaf (as `Location`
now does) and `LogInsert` would append and stamp under it. Unscheduled;
it is AT's, being a window AT-S5 opened.
