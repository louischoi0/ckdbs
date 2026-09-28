# A DDL rollback compensates logged catalog pages without a record

**Found by reading, not reproduced, and its consequence not verified.**
Verified at `8f9a887` on `at-s21-log-under-hold`, by AT-S21's survey.

## What is wrong

`TransactionManager::Compensate` (`src/txn/manager.cpp`) skips the log for a
page below `catalog::kCatalogOverflowLimit` (`unlogged_page`), on the
premise its comment states: *"the forward writes that put those rows on the
page are unlogged - catalog writes have no WAL records and the catalog is
not recovered"*. That premise is stale: catalog writes **are** logged
(`catalog.cpp`'s `InsertRow`, `OverwriteLogged`, `DeleteMarkLogged`,
`RetireLogged`; `CLAUDE.md`'s WAL row). A transactional DDL's catalog rows
reach this trail through `CommandDispatcher::NoteCatalogRowChanges`, so a
**live** rollback of such a DDL changes a logged catalog page and writes no
record of the change.

## What it may cost - not verified

If recovery treats the rolled-back DDL as decided (a durable `TXN_ABORT` is
not a loser, `wal_recovery_test.cpp`'s
`ADurableAbortIsNotALoserAndNeedsNoUndoPhase`), redo replays the forward
catalog writes and nothing replays their compensation: a rolled-back
`CREATE TABLE`'s rows would return after a crash. Whether a catalog-specific
path (the DDL undo hook, mount finalization) covers it was not established by
this survey, which is why the entry says "may".

## The fix

Verify first. If the consequence holds: drop the `unlogged_page` arm so a
catalog compensation logs like any other, and rewrite the comment.
Unscheduled.
