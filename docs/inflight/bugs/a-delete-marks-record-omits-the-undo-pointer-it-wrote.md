# A delete-mark's record omits the undo pointer the delete wrote

**Found by reading, not reproduced.** Verified at `8f9a887` on
`at-s21-log-under-hold`, by AT-S21's survey.

## What is wrong

`DeleteInner`'s mark (`src/server/command_dispatcher.cpp`) makes two writes
under one hold: `OverwriteTuple(slot, same, new_trx_id, new_undo_ptr)` - the
header's link back to the version the delete supersedes - then
`DeleteMark(slot, new_trx_id)`. Its record, `HEAP_DELETE_MARK`, carries the
slot and `new_trx_id` only, and redo's `ApplyDeleteMark`
(`src/wal/redo.cpp`) calls `DeleteMark` alone. **The `undo_ptr` write is
not in the log**: a page rebuilt by redo carries the delete mark with the
header's previous `undo_ptr`.

## What it costs

Bounded by what reads a mark's `undo_ptr` after a restart, and that is
narrow: no snapshot survives one, a committed delete hides the row from
every new reader, and a loser's rollback restores the header from its own
undo record. What remains is any reader of the chain between redo and that
rollback, and purge's reading of it. Stated rather than guessed further; a
record that does not describe a write it covers is a defect whatever it
costs today.

## The fix

Carry `undo_ptr` in `HEAP_DELETE_MARK` - a record format change, so the
decoder must keep reading the old width - or log the header write as its own
`HEAP_OVERWRITE` ahead of the mark. Unscheduled.
