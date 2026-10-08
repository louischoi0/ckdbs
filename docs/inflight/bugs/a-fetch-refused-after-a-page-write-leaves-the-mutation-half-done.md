# A fetch or creation refused after a page write leaves the mutation half done

**Found by reading, not reproduced.** Verified at `e4b107af` on
`worktree-pool-budget-required`, by BE-S1's Census B
(`instructions/v3.0.0/workorder-be-bounded-pool.md` §6, BE-S1).

## What is wrong

Several mutation paths write a page and then fetch or create another one.
Today the second step fails only on a device error. Under BE-R4 it fails
whenever the pool is at its cap. Either way, its error returns with the
first write left in place and nothing that undoes it:

- **An `INSERT` between placing the row and writing its undo.** The row is
  placed (`btree.cpp:1017`, or `heap_chain.cpp:104`/`:126`), and then the
  index entries (`command_dispatcher.cpp:5300`), the assertion reservation
  (`:5320`) and the undo append (`:5347`) each fetch or create pages. A
  failure in any of them returns before the undo record and the trail entry
  exist. `Abort` undoes only the trail (`txn/manager.cpp:563-571`), so the
  row stays. Once the read floor passes its transaction id it is visible
  (`read_view.hpp:127`), missing the index entries not yet written. The same gap exists in the bulk
  carved fill (`heap_chain.cpp:273`, the undo loop at
  `command_dispatcher.cpp:4941`).
- **An index split after the leaf is divided.** Creating the next internal
  node (`index_tree.cpp:494`) or a new root (`:523`) after the leaf is split
  and linked (`:432`, `:455`) leaves a sibling with no parent separator,
  unlogged. Inserts into that range are then refused `TxnConflict`.
  The clustered tree's leaf divide has the same tear: `btree.cpp:766-869`
  divides and links before `PromoteSeparator` creates (`:395`, `:623`,
  `:644`), the order its own H9 comment (`:1105-1125`) calls unsurvivable.
  SQL does not reach that divide at `e4b107af` (BB-R3 appends at the
  rightmost leaf); `workorder-bd-sorted-leaf-named-keys.md` would.
- **A root re-publish after the new root is built.** If the anchor `Get`
  (`catalog.cpp:1101`) fails after `btree.cpp:661`/`index_tree.cpp:538`
  marks the old root grown-over, the relation or index is durably unable
  to grow.
- **A rollback compensation refused** (`txn/manager.cpp:438`, `:463`).
  `Abort` records the failure, still appends `TXN_ABORT` and retires the
  transaction, so that write stays un-undone. A restart does not repair
  it: analysis reads an aborted transaction as already compensated
  (`analysis.cpp:170-173`) and undo skips it (`recovery.cpp:154-155`).
- **A var-heap growth followed by a refused undo** (`varheap.cpp:365`, then
  `txn/manager.cpp:265`). It leaves an unlogged link, which can refuse the
  mount after a crash (`redo.cpp:368`).
- **Bulk fill's page images** (`log_page_image.hpp:31`, called from
  `command_dispatcher.cpp:4957`), refused after the rows and undo are
  written. A crash before writeback then refuses the mount.
- **The assertion commit and abort loops** (`assertion_check.cpp:727`,
  `:831`). Both erase the pending list before the loop, so a failure partway
  through drifts the group totals. The erase is deliberate (`:707-717`
  prefers a reported drift to a double settle); the drift is still wrong
  state that nothing repairs.

## What it costs

A quiet wrong answer: an aborted row that comes back. Also durable structural
damage, or a mount refused after a crash. Today it takes a device error.
**BE-R4 makes it a load property**, because a full pool refuses the fault.

## The fix

BE-S4 owns it. BE-S4 does not ship a refusal path that can reach these sites
(BE-Q4's `[quiet-wrong]` flag). The order's §6 BE-S1 row lists the options.
