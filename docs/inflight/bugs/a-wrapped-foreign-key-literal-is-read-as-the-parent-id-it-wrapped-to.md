# A wrapped foreign-key literal is read as the parent id it wrapped to

**Found by reading, not reproduced.** Verified by the `critics-developer`
review of the fix for the same wrap in a WHERE
(`worktree-fix-int-literal-wrap-in-where`, on `origin/main`'s `6b0d8412`).

## What is wrong

The lexer wraps an integer literal past int64 (`18446744073709551621` lexes
as `5`; `parser::IntLiteralWrapped` tells). `ResolveForeignKeyParents` and
`CheckForeignKeyOnWrite` (`src/server/command_dispatcher.cpp`, about `:3608`,
`:3627` and `:3719`) run before the row is encoded and read `value.int_val` of
the foreign-key column as the parent id, with no `IntLiteralWrapped` test.

## What it costs

Not a wrong answer: the encode refuses the row `OutOfRange` afterwards. The
refusal is the wrong one, and one lock is taken first.

- Parent `5` absent: the `INSERT` is refused as a foreign-key violation
  naming id 5, not `OutOfRange` for the literal.
- Parent `5` present: the check takes `S` on parent 5 before the encode
  refuses the row, so an out-of-range literal briefly blocks a writer of that
  parent.

## The fix it wants

Add `|| parser::IntLiteralWrapped(value)` to the bail-out of both functions,
so the encode gives its `OutOfRange` refusal first.
