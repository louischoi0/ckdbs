# A write walk's subquery reads pages under the walk's exclusive leaf hold

**Found** 2026-10-06 by BB-S1's latch-order census
(`instructions/v3.0.0/workorder-bb-issue-under-the-leaf.md` §6), verified by
reading on `worktree-bb-issue-under-the-leaf` at `c566a4a4`, whose `src/` and
`include/` are `6dc792c9`'s. **Not reproduced, not fixed.** It is not BB's:
none of BB's latch edges takes part in it, and BB records it rather than
stopping on it, on the operator's word that a question BB raises is settled
by CLA's proposal (`raft-marks-2026-10-06.md` §7).

**Cost: two reactors hang for good.** A page-latch wait is an unbounded spin
with no fault net (`include/kds/storage/page_latch.hpp:160-170`), so a cycle
across two cores never resolves. No wrong answer is given; nothing is
answered.

## The wrong behaviour

`UPDATE` and `DELETE` walk their relation holding each leaf **exclusive**
across the per-row work (`VisitRelation(kWrite)`,
`src/server/command_dispatcher.cpp:7611` and `:9009`, through
`BtreeVisitLeafPage`'s `store.Get` at `src/storage/btree/btree.cpp:1192`, held
across the visitor). The per-row work evaluates the `WHERE` before the row's
borrow (`:7230` for `UPDATE`, `:8803` for `DELETE`), and every subquery there
is a sub-chain re-run per row (`step_compiler.cpp:1418-1436`,
`step_vm.cpp:2747-2766`) that walks its relation with shared holds. So a
write walk holds a leaf `X` and asks other leaves `S`, of its own relation or
another, to its left as readily as to its right.

Two such statements on two cores close a cycle:

- **Across two relations.** Core 0 runs
  `UPDATE a SET ... WHERE EXISTS (SELECT 1 FROM b WHERE ...)` and holds an
  `a` leaf `X` while it reads `b`; core 1 runs the mirror statement and holds
  a `b` leaf `X` while it reads `a`.
- **Within one relation.** Two `UPDATE a ... WHERE id IN (SELECT ... FROM a
  ...)` each walk `a` from its leftmost leaf under their own leaf's `X`, past
  the other's.
- **Through a foreign key.** A parent `DELETE` holds a parent leaf `X` and
  walks the child (`command_dispatcher.cpp:8888` → `fk_check.cpp:407`), while
  a child `UPDATE ... WHERE EXISTS (SELECT ... FROM parent ...)` holds a
  child leaf `X` and reads the parent.
- **Through any write descent.** `DescendTo` holds the leaf it reached `X`
  and reads its right neighbour `S` when it has one (`btree.cpp:191`, `:203`
  → `:103`, reached from `:891` and `:1095`), so a point `UPDATE`/`DELETE`
  and an insert whose leaf another core split before its exclusive grant are
  members of the same cycle against a sub-walk holding that neighbour.

Nothing else serialises the two statements: the relation intentions are
`IX` against `IS`, which are compatible; an `EXISTS`/`IN`-only `WHERE`
declares no range (`DeclaredWriteBorrow` returns nothing); and the rows each
writes are distinct. A heap relation has the same shape
(`ChainVisitOnePage`'s write hold, `heap_chain.cpp:302`).

**It contradicts a stated order.** `btree.cpp:95-96` and `:467-471` say
*"nothing anywhere holds a page and asks for the one to its left"*,
`device_page_store.hpp` (the page-latch section's page-against-page bullet)
gave "parent before child, old before new" as the order AM-S2 owes, and
`rules.md` §3's page latch row said nothing holds a tree node and asks for
one below it or to its left. The leftward rule is true of the tree's own
code; "parent before child" has been false of it since AT-S16
(`SecureParents` holds the leaf, then asks its parents). Both are false of
a write walk's `WHERE`; `rules.md` §3 and `device_page_store.hpp` now say
so.

**Not reachable today through a self-referencing foreign key**: one cannot be
declared (`REFERENCES` is resolved before the child exists,
`command_dispatcher.cpp:3987`, pinned by
`ASelfReferencingForeignKeyCannotBeDeclared`). If that changes, the
self-referencing `DELETE` and `UPDATE` arms become live members of the same
cycle.

## Smallest reproduction (not run)

1. `cores = 2`; two btree relations `a` and `b`, each large enough to span
   several leaves.
2. Core 0: `UPDATE a SET v = v + 1 WHERE EXISTS (SELECT 1 FROM b WHERE b.v = a.v)`.
3. Core 1: `UPDATE b SET v = v + 1 WHERE EXISTS (SELECT 1 FROM a WHERE a.v = b.v)`.
4. A seam in the per-row work, or enough rows, puts each walk's leaf `X` and
   the other's sub-walk on it at once; neither statement returns.

## What BB changes about it

Nothing structural. BB holds the rightmost leaf longer (the issue, the borrow
and the encode now run under it, BB-R2), which lengthens the time a sub-walk
waits on it. On the rightmost leaf the inserter reads no neighbour
(`btree.cpp:102`); it joins the cycle only through the descent's
right-neighbour read above, which every write descent already has - BB adds
no edge to it. **BD-S3 withdrew the longer hold on a btree** (BD-R6, verified
at `07e822a6`): the issue, the borrow and the encode run before the descent
again. A named key now lands mid-chain as often as on the rightmost leaf,
and pays the same right-neighbour read, which is no new edge either.

## The fix, known and unscheduled

Smallest first, from the census's verification:

- **(b) A try-shared acquire for a sub-chain's reads under a write hold**:
  a busy page refuses the statement retryable instead of spinning. A try that
  never blocks is not a wait edge. `DevicePageStore::TryAcquirePageLatchShared`
  is a private writeback helper today (its one caller is the writeback's
  copy); the fix needs a fetch that tries.
- **(c) Hoist uncorrelated sub-chains before the walk**, as `Execute` already
  does for a `SELECT` (`step_vm.cpp:2806-2825`). Covers only the
  uncorrelated, and only where the sub-chain reads a relation other than
  the one written - over its own relation, hoisting hides the statement's
  earlier rows from it.
- **(a) Evaluate sub-chains with no page latch held**: stop the walk through
  AO-S3b's resume-by-pk, evaluate, re-descend and re-check before writing.

Owner: `page.md` §6's owed page-against-page order (AM-S2's), and
`rules.md` §3's page latch row.
