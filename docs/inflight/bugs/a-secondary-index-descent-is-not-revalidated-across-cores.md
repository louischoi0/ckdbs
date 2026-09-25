# A secondary index's descent is not re-validated across cores

**Found by reading, not reproduced.** Verified at `2b20369` on
`at-s12-prose-sweep`, during AT-S12's prose sweep of `page.md` §6.

## What is wrong

Since AT-S5 a write runs where its session is, so two writers on two cores
maintain **one secondary index** concurrently. `index_tree.cpp`'s insert
(`IndexInsert` via `DescendTo(..., leaf_for_write=true)`) has the two
windows AT-S5c closed for the clustered tree and not for this one:

1. **The leaf re-fetch.** `DescendTo` reads the leaf under a shared hold,
   **drops it**, and re-fetches it exclusive (`store.Get(current)`) - a
   page latch is never upgraded. Nothing checks that the leaf it gets back
   still covers the sort key. Its comment's justification - *"Nothing can
   evict between the release and the re-fetch on a single-threaded core"* -
   is the single-owner premise AT-S5 retired. Another core can divide the
   leaf in between, and the entry lands in the left half when its key now
   belongs to the new right sibling.
2. **The split's walk back up.** A divide inserts its separator by walking
   the root-to-leaf `path` the descent recorded, holding none of it. A
   parent another core divided meanwhile takes the separator on the wrong
   side.

The clustered tree has both answers since AT-S5c: `LeafStillCoversKey`
re-checks the leaf after the re-fetch, and a descent that finds itself
stale restarts (`src/storage/btree/btree.cpp`, "The window, and what
closes it"). AT-S5c's row records the index root's bump
(`Catalog::UpdateIndexRoot`) and says of it *"unlike the clustered root
there is no coverage check underneath"* - the same absence, at the root
rather than the leaf.

## Smallest reproduction (not yet written)

Two cores, one relation with a secondary index on `v`, each core inserting
rows whose `v` falls in one index leaf until it divides; a barrier between
core A's shared read of the leaf and its exclusive re-fetch, released after
core B's divide. Then probe the index for A's key: the descent reaches the
right sibling and does not find it. The two-core rig
(`tests/two_core_rig.hpp`) and a barrier plus rounds is the shape
(`mutation-testing` practice: one run proves nothing).

## What it costs

A **quiet wrong answer**: an index probe for the misplaced key misses a
live row, and `SELECT ... WHERE v = k` through the index returns fewer rows
than the relation holds. A mis-placed separator makes a subtree
unreachable for a range of keys. No refusal, no corruption error - the
leaf's own checks pass.

## The fix

Known in shape, unscheduled: AT-S5c's two answers applied to
`index_tree.cpp` - a coverage check after the re-fetch and a restart of a
stale descent, including the split's walk up. No work order carries it
yet.
