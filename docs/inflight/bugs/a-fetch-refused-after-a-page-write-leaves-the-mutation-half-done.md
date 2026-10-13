# A fetch or creation refused after a page write leaves the mutation half done

**Found by reading, not reproduced.** First verified at `e4b107af` on
`worktree-pool-budget-required`, by BE-S1's Census B
(`instructions/v3.0.0/workorder-be-bounded-pool.md` §6, BE-S1). **Re-verified
at `afb21fbb` on `worktree-fix-fetch-refused-half-done`**, after four of its
groups were fixed there; what follows is the tree at that commit.

## What is wrong

A mutation path that writes a page and then fetches or creates another one
fails the second step on a device error and, under BE-R4, whenever the pool
is at its cap outside a no-refuse window (BE-Q11). Its error returns with the
first write in place and nothing that undoes it. Four groups remain:

- **A rollback compensation refused** (`txn/manager.cpp:592`). `Abort`
  records the first failure, keeps unwinding, still appends `TXN_ABORT`
  (`:605`) and retires the transaction, so that one write stays un-undone.
  A restart does not repair it: analysis reads an aborted transaction as
  compensated (`wal/analysis.cpp:141`) and undo skips it. The proposed fix
  - on the first failure, no `TXN_ABORT`, stop the instance, let mount undo
  finish - is blocked by
  `a-loser-that-inserted-then-changed-a-row-refuses-a-second-undo.md`: the
  mount would re-run compensations the live rollback already applied.
- **The assertion commit and abort loops** (`exec/assertion_check.cpp:776`,
  `:846`). Both erase the pending list (`:803`, `:858`) before the loop, so a
  failure partway through drifts the group totals. The erase is deliberate
  (a reported drift over a double settle); the drift is still wrong state
  that nothing repairs. `assertion-reservations-stranded-by-a-failed-settle.md`
  and `a-losers-assertion-reservations-survive-the-mount.md` are the same
  subsystem's neighbours.
- **The carved bulk fill and its page images** (`server/command_dispatcher.cpp:5710`
  `ChainAppendCarved`, then the undo loop and the images). The fill places
  every row before any undo record and re-fetches each page for its image
  after. BE-Q11's window, sized to the fill, covers the pool; a device error
  in either step still leaves rows with no undo, or rows and undo with no
  image, which a crash before writeback turns into a refused mount. The path
  is heap-gated, so SUS-1 leaves it reachable only for relations created
  before the suspension. Proposed: delete the path rather than fix it.
- **E5's take-back re-fetches its page** (`server/command_dispatcher.cpp:6212`).
  On an append split the placed row is on the new leaf, which the split
  released before it returned, so the take-back's `Get` can fault - and a
  refused fault is logged and swallowed, leaving the unlogged row the
  take-back exists to remove.

## Fixed on `worktree-fix-fetch-refused-half-done`

- **An `INSERT` between placing the row and writing its undo** (`02fa4438`).
  E5's take-back already covered a btree leaf; it now covers a heap tail, and
  a chain the insert grew keeps its new, empty page logged at `kNoTxnId`.
- **A var-heap growth followed by a refused undo** (`02fa4438`). The spill is
  logged even when its undo record failed, at the cost of one leaked slot.
- **A split torn between its pages** (`4b09cba4`). Both trees claim every page
  the split will create (`PageStore::ClaimNew`, `page.md` §14) before they
  write, so a creation the device refuses leaves the tree as it was.
- **A root re-publish after the new root is built** (`afb21fbb`). A split that
  grows a level pins the anchor with no latch before it writes
  (`PageStore::PinUnlatched`, `page.md` §3), and the no-WAL index path
  publishes the root while that pin is held.

## Found while fixing it

- UPDATE's `NoteSpills` call may be a no-op as INSERT's was (deleted at
  `02fa4438`); deleting it needs a check that no configuration runs UPDATE
  with a WAL but no transaction manager.

## What it costs

A quiet wrong answer is no longer among the open groups' costs except
through the take-back (an aborted row that stays). The rest is durable wrong
state - a write rollback left in place, a drifted assertion total - or a
mount refused after a crash. It takes a device error, or BE-R4's cap outside
a window.

## The fix

BE-S4 owns it. The rollback group waits on the second-undo entry; the
assertion loops and the carved fill wait on the operator's ruling.
