# A btree divide can promote into a parent another core divided

**Found by reading, not reproduced.** Verified at `007b5b5` on
`at-s12-prose-sweep`, by AT-S12's review (the second `critics-developer`
pass over the prose sweep), while checking what AT-S5c closed.

## What is wrong

The clustered tree - the storage default for every relation created since
SUS-1 - promotes a split's separator by walking the root-to-leaf path its
descent recorded (`PromoteSeparator`, `src/storage/btree/btree.cpp`),
fetching each parent by that recorded id and re-checking nothing. AT-S5c
closed the **leaf** window (`LeafStillCoversKey`, and a stale descent
restarts) and not this one.

It is safe on the **append** path: the split leaf's pins are held across
the whole promotion (`SplitLeafAndInsert`'s two, alive across the tail
call), and a monotonic id sequence splits only the tail leaf, so two
appends serialise on it. It is **not** safe on the **divide** path
(`DivideInternalNode`), which a caller-named pk below the relation's mark
requires (`heap-and-tuple.md` §4.1, "Promotion into a full internal node
divides it"): two such inserts on two cores, into two sibling leaves under
one full parent, each hold only their own leaf. Core B divides the parent
and moves its upper half to a new node; core A, promoting from the path it
recorded, inserts its separator into the lower half, where no descent for
A's new subtree will look.

## Smallest reproduction (not yet written)

A btree relation with rows whose parent node is full; two cores, each
inserting a named pk below the mark into a different leaf under that
parent, each leaf full; a barrier after A's leaf split and before A's
`PromoteSeparator` fetches the parent, released after B's divide. Then
read A's key by pk: the descent misses it. The two-core rig and a barrier
plus rounds is the shape.

## What it costs

A **quiet wrong answer**, and a durable one: the rows under A's new leaf
are unreachable by descent (a point read misses them, a walk that follows
sibling links may still reach them), and the next mount carries the tree
forward as it is. Reached only by concurrent below-mark named inserts; an
omitted pk, or a named one at or above the mark, is an append.

## The fix

None in the tree. The secondary index has the same window on its ordinary
path (`a-secondary-index-descent-is-not-revalidated-across-cores.md`,
window 2). Latch coupling on the walk up, or re-descending for the parent
when its coverage no longer holds the separator, are the two shapes; which
is a decision. No work order carries it.
