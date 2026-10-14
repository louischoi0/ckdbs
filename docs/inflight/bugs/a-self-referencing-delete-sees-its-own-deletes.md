# A `DELETE` on a self-referencing relation sees its own deletes in the reverse check

**Found by reading, not reproduced.** Read on `worktree-fix-write-subquery-own-writes`
at `541ce440`, 2026-10-14, by the `critics-developer` review of the fix that
makes a write collect its targets when its `WHERE` reads the relation it
writes (`docs/spec/parser-v2.md` R2).

## What is wrong

That fix covers the `WHERE`'s sub-chains only. `DeleteInner` also runs the
reverse foreign-key check per row (`CheckNoChildrenBeforeDelete`,
`src/server/command_dispatcher.cpp`), and on a self-referencing relation it
walks children in the relation being deleted from. A child that this
statement has already deleted may or may not count, depending on whether the
walk reached it before its parent. So whether a multi-row `DELETE` is
refused can depend on the order of the walk, not on the relation as it stood
when the statement began.

## Smallest reproduction (not yet run)

`CREATE TABLE t (id int64, parent int64 NULL REFERENCES t)`, rows
`(1, NULL)` and `(2, 1)`, then `DELETE FROM t WHERE id > 0`. Under statement
semantics the answer does not depend on the walk order. The reading predicts
that the walk meets id 1 first, finds child 2 still live and refuses, whereas
a walk meeting id 2 first would delete both rows. Whether a btree's key order
always takes the first path has not been checked.

## What it costs

A refusal or a success chosen by walk order, never a wrong row written: the
check guards a RESTRICT, so the worst outcome is a refused statement that
statement semantics would admit, or the reverse. Which outcome is correct for
a self-referencing RESTRICT is the operator's call (`docs/spec/foreign-keys.md`).
