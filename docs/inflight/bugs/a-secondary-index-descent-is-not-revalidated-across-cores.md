# A secondary index's divide walks up an unchecked path across cores

**Found by reading, not reproduced.** First verified at `2b20369` on
`at-s12-prose-sweep`, during AT-S12's prose sweep of `page.md` §6.
**Window 1 closed at AT-S15** on `at-s15-index-leaf-coverage` (from
`4d1e970`); window 2, below, re-read there and still open.

## What is wrong

Since AT-S5 a write runs where its session is, so two writers on two cores
maintain **one secondary index** concurrently. `index_tree.cpp`'s insert
had two unchecked windows:

1. **The leaf's coverage** - closed at AT-S15. `DescendTo` now asks, once
   the leaf is held exclusive, whether the sort key still sorts below the
   right sibling's first entry, and a stale descent starts over
   (`LeafStillCoversKey`; `index.md` IX14). Its cells are
   `IndexTreeTest.ALeafDivided*`, `IndexTreeTest.AnInsertFromARootThatHasSinceGrownIsRefusedOutsideItsSubtree`
   and `IndexRaceTest.*`.
2. **The divide's walk back up - open.** A divide inserts its separator by
   walking the root-to-leaf `path` the descent recorded, holding none of
   it. A parent another core divided meanwhile takes the separator on the
   wrong side. A **stale root** reaches the same walk: an insert placed
   correctly in the stale root's subtree that divides past it grows a level
   from the stale root, and its `UpdateIndexRoot` republishes a root whose
   subtree lacks the other core's half.

The clustered tree has window 2 too, reached only by concurrent below-mark
named pks: `a-btree-divide-can-promote-into-a-parent-another-core-divided.md`.
On a secondary index, where keys are not a sequence, divides are the
ordinary case.

## Smallest reproduction (not yet written)

Two cores, one secondary index whose parent node is one entry from full;
two inserts into sibling leaves under that parent, each dividing its leaf;
a barrier before A's parent fetch, released after B's divide has divided
the parent. Then check the structural invariant - every separator routes to
the subtree holding its key - and probe A's key. `IndexRaceTest` excludes
this by asserting the root never divides during its rounds; AT-S16's cell
must let it.

## What it costs

**A refusal that does not clear, since AT-S15; before it, a quiet wrong
answer.** A separator placed in the wrong half of a parent is still the
lower bound of the child it points at, so a descent never lands to the
*right* of a key's leaf - it lands left of it. A probe walks forward and
still finds every row (at the cost of pages). An insert over the affected
key range lands on a leaf whose right sibling now bounds it below the key,
and AT-S15's coverage check refuses it `TxnConflict` on every attempt:
nothing repairs the parent, so the range stays unwritable. Before AT-S15
those inserts were admitted into the leaf the stale route reached, out of
chain order, and a probe then missed them.

## The fix

AT-S16, by the operator's Q1 ruling (`workorder-at-close-ar0-5-left-open.md`
§3): before inserting the separator, check that the recorded parent still
covers it; on a miss, re-descend from the root for the parent of the new
node - one shape for both trees.
