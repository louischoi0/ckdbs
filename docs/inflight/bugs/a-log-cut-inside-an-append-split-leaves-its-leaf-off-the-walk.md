# A log cut inside an append split leaves its leaf off the walk

**Reproduced** on `keep-btree-leaf-slots` by BD-S1's cell
`SortedLeafCrashTest.ALogCutInsideAnAppendSplitLeavesEveryLaterRowOnTheWalk`
(`tests/sorted_leaf_crash_test.cpp`), on an engine whose code is
`ec3acda5`'s - BD-S1 adds cells and a test seam only. One core, one
statement, nothing concurrent. Found by
`instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md` §1.8, which
predicted it from reading, and decided at BD-S1 as that order asks.

## What is wrong

An append split's records are separate, and the cell reads the statement's
eight off the log in this order:

1. `TXN_BEGIN`
2. `HEAP_OVERWRITE` (the catalog's `sys.tables` row)
3. `UNDO_WRITE`
4. `PAGE_INIT` - the new leaf
5. `FULL_PAGE_IMAGE` - the parent, carrying the new separator
6. `FULL_PAGE_IMAGE` - the old leaf, carrying the link to the new leaf
7. `HEAP_INSERT` - the row, into the new leaf
8. `TXN_COMMIT`

`btree.cpp`'s `PlaceUnderHold` says the order among the images *"is not
load-bearing"*. That holds only if the durable log never ends between two
of them, and nothing makes a split's records one unit. When it ends after
record 5, redo rebuilds a parent that routes `[split key, ...)` to the new
leaf beside an old leaf whose `next_page_id` does not reach it. The
statement is a loser and its row is rolled back; the new leaf stays routed
and unlinked.

## Smallest reproduction

Two full leaves of `t (id int64, v int64)` at ids `10, 20, ..., 3960`, then
`INSERT INTO t VALUES (3970, 0)`, which splits the second leaf right. Mount
the pre-statement data file beside the log cut after record 5. The mount
succeeds and every committed row is present. Then insert ids
`100000..100004`: each lands in the new leaf, which a descent reaches, and
`SELECT id FROM t` - a walk of the leaf chain - does not return them.

## What it costs

A **quiet wrong answer**: every row placed in that leaf after the restart
is found by a point lookup and missed by every scan, `ORDER BY`, aggregate
and join that walks the chain. Nothing refuses, and the mount reports
nothing.

## What does not reproduce

The same cut through the secondary index's root split
(`SortedLeafCrashTest.ALogCutInsideAnIndexSplitLeavesEveryRowOnItsIndex`):
every sampled row is found through the index at every cut. That cell stays
as a guard; it covers the root leaf's split only.

## The fix

BD-R12, at BD-S2: one `BTREE_SPLIT` record carrying every image a split
writes, applied whole by redo and atomic by its CRC (BD-Q10 (a)).
