# An index backfill walks the whole undo chain, not the versions a snapshot can reach

**Found by reading, not reproduced.** Verified at `34164296` on
`worktree-fix-index-backfill-append-failure`, by the `critics-developer` review
of the fix that makes the backfill refuse a failed append.

## What is wrong

`Backfill`'s phase 2 (`src/exec/index_ddl.cpp`) appends each row's current
version and then follows its undo chain to `kNoUndoPtr`, indexing every older
version. A reader stops at the first version its snapshot can see
(`src/txn/visibility.cpp`), and the undo purge relies on readers stopping
there: `include/kds/txn/undo_log.hpp` says a reader walks into undo only while
writers are invisible, so "a freed page's stale `undo_ptr`s are harmless". The
backfill breaks that rule.

## What it costs

- **A refusal that does not clear.** A row with a long chain - one whose
  covered column moved about 600 times - makes `CREATE INDEX` refuse for good,
  even after every old snapshot has ended, because the backfill indexes
  versions nobody can reach. Before `34164296` it published a partial index
  instead.
- **Possibly a quiet wrong answer, not reproduced.** Once purge has recycled an
  undo page, a tuple's stale `undo_ptr` points into another transaction's
  records. A record of another relation's image length is refused by
  `DecodeRow` (invariant 13), which now fails the `CREATE INDEX` as
  `Corruption`; a recycled record with the same image size decodes cleanly and
  would be appended under this row's pk, then its chain followed. Since AT-S22 a
  covering probe keeps a row if any of its entries passes the filter, so a
  foreign entry could let a row through that should not pass.

## The fix it wants

Stop the walk at the first version every live snapshot can see: pass the
instance's oldest live view into the walk and stop when the version is visible
to it, as a reader does. Smaller: refuse a version whose `pk` is not the row's
(`UndoVersion` carries it), which catches some foreign records and does not fix
the depth. Not applied, because it changes what a `CREATE INDEX` over an
updated relation indexes and needs the read horizon's definition at that call.
