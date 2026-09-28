# An index backfill drops an append failure in a row's undo walk

**Found by reading, not reproduced.** Verified at `3d97d87` on
`at-s22-covered-row-survives`, by AT-S22's `critics-developer` pass: AT-S22's
correctness argument ("every version that wrote a key or covered value has
an entry") rests on the backfill, and the backfill does not always keep it.

## What is wrong

`Backfill`'s phase 2 (`src/exec/index_ddl.cpp`, the loop over
`staged`) appends each row's current version, then walks its undo chain and
appends every older version. Inside the walk, an `AppendVersion` failure is
turned into `return false`:

```cpp
if (Status s = AppendVersion(store, access, ix, row.pk, version.image);
    !s.ok()) {
    return false;
}
```

and `UndoLog::Walk` reads a `false` from its callback as "stop here",
returning `Status::OK()` (`src/txn/undo_log.cpp`). So the failure is lost:
`CREATE INDEX` succeeds, and the index is published without that version's
entry or any older one of that row.

## What it costs

A **quiet wrong answer**, for an old snapshot. A `REPEATABLE READ` reader
whose visible version is one of the dropped ones reaches the row through no
entry, and an index probe answers without it - what IX10a forbids. Current
readers are unaffected: the current version's append is outside the walk and
its failure does fail the statement.

Reached by any append failure during the walk: an I/O error, or the known
refusal of a run of equal sort keys
(`a-run-of-equal-index-sort-keys-promotes-one-separator-twice.md`), which a
row whose covered column was updated ~600 times reaches in the backfill.

## The fix

Carry the status out of the callback: a `Status append_failed` beside the
walk, set before `return false`, returned after `if (!walked.ok())`. **Not
applied at AT-S22**, because it changes what `CREATE INDEX` does on a
relation holding such a run - a refusal where it now publishes a partial
index - and that relation's only way out is the IX4a decision the
equal-sort-key entry waits on. The fix is right either way; when it lands is
the operator's.
