# A loser that inserted and then changed a row refuses a second undo

**Found by reading, not reproduced**, while fixing
`a-fetch-refused-after-a-page-write-leaves-the-mutation-half-done.md`.
Verified at `afb21fbb` on `worktree-fix-fetch-refused-half-done`.

## What is wrong

Recovery undo walks a loser's chain from its head and decides per record
whether it was already compensated. For an insert it answers "nothing there"
as done. For an overwrite or a delete-mark it requires the row to be there:

- a heap or catalog page: the identity check refuses `Corruption` when no row
  is at the slot (`txn/recovery_undo.cpp:145-157`);
- a btree leaf: a key in no leaf from the recorded page rightward is
  `Corruption` (`:196-206`).

A transaction that inserted a row and then updated or deleted it leaves an
overwrite or delete-mark record above the insert record. Once the insert has
been compensated - its slot retired - the later record's row is gone, and the
re-run refuses. Two ways reach it:

- **A crash during mount undo**, after the insert's `SLOT_RETIRE` and before
  the loser is finished. The next mount redoes the retire and runs the chain
  again from its head.
- **A crash after a live rollback, before its `TXN_ABORT` is durable.** `Abort`
  does not wait for the record (`txn/manager.cpp:605`), so the transaction is a
  loser at the next mount, and undo re-runs compensations the rollback
  already logged.

## What it costs

A refused mount, with nothing wrong on disk. `INSERT` then `UPDATE` or
`DELETE` of the same row in one transaction is ordinary SQL.

## The fix

Not scheduled. The rule `CompensateOnLeaf` states - only a version the loser
wrote is compensated - needs its other half: a missing row is done when an
insert record of the same loser for the same key lies below it in the chain.
It blocks the rollback-compensation group of the fetch-refused entry, whose
proposed fix hands an unfinished rollback to mount undo.
