# Two cores growing one var-heap chain at once can overwrite a link and leak a page

**Found** 2026-10-06 by BB-S1's latch-order census
(`instructions/v3.0.0/workorder-bb-issue-under-the-leaf.md` §6, the
var-heap row) and recorded by BB-S1's review; verified by reading on
`worktree-bb-issue-under-the-leaf` at `169072d7`, whose `src/storage/`,
`src/exec/` and `src/txn/` are `6dc792c9`'s. Line numbers are at
`6dc792c9`. **Not reproduced, not fixed.** Left out of BB: BB-R7 covers
heap relations' chains only, and none of BB's latch edges takes part in it.

**Cost: space, never a value.** One var-heap page - or more - stops being
reachable from its chain's root and is never appended into or freed again.
No answer changes.

## The wrong behaviour

`varheap::ChainAppend` (`src/storage/varheap.cpp:301-375`) finds the tail by
walking the chain with a shared hold, one page at a time (`:319-329`), and
drops that hold when the walk ends. It then takes the tail exclusive
(`:331`) and never re-reads the tail's link. When the tail is full
(`:334`), it creates a page (`:349`), puts the value there, re-fetches the
tail and links `tail.next = new` (`:367-369`), without asking whether the
tail already links somewhere. The heap chain has the same gap
(`two-cores-growing-one-heap-chain-can-orphan-a-page.md`).

Two cores that both walked to tail T while it was the last page:

1. Core A takes T exclusive, finds it full, creates P1, appends its value
   there, links `T.next = P1`, and releases T at its caller's scope.
2. Core B, which ended its walk at T before A's link and waited at `:331`,
   takes T exclusive. If B's value fits in what T has left, it lands in T
   and nothing is lost. Otherwise B creates P2 and links `T.next = P2`,
   overwriting A's link.

P1, and any page a third core grew past P1 before B's overwrite, is now
unreachable from the root. What that loses, precisely:

- **Not A's value.** A's tuple carries a `VarHeapPtr` naming P1 and the
  slot; `Fetch` reads that page directly (`varheap.cpp:382`) and a release
  finds the slot the same way (`txn::ReleaseVarHeapSlot` takes the page id),
  so neither walks the chain.
- **P1's free space, for good.** `ChainAppend` reaches a page only by the
  walk from the root, so no later append lands in P1; and nothing frees it -
  the store has no free-page path (`varheap.cpp:357-359`), and recovery
  repairs the free map by raising the allocation floor, not by reachability
  (`page.md` §5), so P1 is never handed out again and A's value is never
  overwritten.
- **A crash does not repair it.** The link travels as a full image of the
  tail page (`src/exec/wal_row_log.cpp:49-51`), so redo restores whichever
  link the last image carried.
- **Nothing that walks the chain is misled into a wrong answer.** The
  walkers are `ChainAppend`, `varheap::ChainLength` (`:401`; a test caller
  only) and the mount sweep (`src/exec/varheap_sweep.cpp:24-55`), which
  walks only `kVarHeapCatalogRelations` - `sys.assertions`, whose one
  appender, `InsertAssertion`, runs under its root page's exclusive hold
  (`src/exec/assertion_catalog.cpp:214`, `catalog.md` CT7), so its chain is
  never grown by two cores at once.

## Reach

User relations with a spillable column, at `cores > 1`, two writes
spilling into one relation's chain at once while its tail is full: two
`INSERT`s (at `6dc792c9` an insert encodes its spills before the descent,
`command_dispatcher.cpp:5122`, under no user page hold), or `UPDATE`s on
different pages - an `UPDATE` spills under its walk's exclusive hold of the
row's page, so two on one page are serialised there. From BB-S3 an
omitted-pk insert encodes under the leaf it lands on (BB-R2), which
serialises those against each other but not against writes to other pages. At `cores = 1` it is unreachable: `ChainAppend` has no suspension
point and the reactor runs one task at a time.

## Smallest reproduction (not run)

1. `cores = 2`; one relation with a `varchar(16)` column, and its var-heap
   tail filled to within one long value of full.
2. A seam between the walk's end and `:331`'s exclusive fetch, as BB-S2's
   rig seams do, parking each core there.
3. Core 0 and core 1 each insert a row whose value is longer than the
   tail's remaining space; release both seams.
4. `varheap::ChainLength` from the root counts one page fewer than were
   created; both values still read back.

## The fix, known and unscheduled

After taking the tail exclusive at `:331`, re-read its link: a tail that
already links somewhere is not the tail, so release it and walk on from the
link. The heap chain's fix (BB-R7's tail re-check) is the same shape. No
work order carries it.

Owner: the var-heap's chain growth, `docs/rules/rule-fixed-length-tuple.md`
§5 and §8b.
