# An integer literal past int64 in a WHERE compares as its wrap

**Reproduced** at `04649133` plus BH-S5's uncommitted fixes, on
`worktree-bh-purge-key`, by a scratch cell that was not committed. Found by
BH-S5's `critics-developer` pass over the fix for the same wrap in `PURGE`.
It happens on one core, with nothing concurrent.

## What is wrong

The lexer wraps an integer literal past 64 bits (`lexer.cpp`; `token.hpp`'s
digits() note): `18446744073709551621` lexes as `5`. The digits survive in
`AstValue::raw_int_text`, and `parser::IntLiteralWrapped` tells a wrapped
literal from one that is not. Not every comparison asks:

- `CoerceLiteralToColumn`'s default arm (`src/exec/row_codec.cpp`) admits a
  wrapped literal against every integer column. Only its DATE, TIMESTAMP and
  decimal arms refuse one.
- `PkBound` and the `kLookup` gate (`src/exec/step_compiler.cpp`) read
  `int_val` and never ask.

Observed, on a btree `t (id int64, qty int64)` holding the row `(5, 1)`:

| statement | answer |
|---|---|
| `SELECT * FROM t WHERE id = 18446744073709551621` | `5,1` |
| `SELECT * FROM t WHERE qty = 18446744073709551617` | `5,1` |
| `DELETE FROM t WHERE id = 18446744073709551621` | `DELETED 1` |

A client that names a value nothing can hold reads, or deletes, a row it
never named. It gets no error.

## What it costs

A **quiet wrong answer**, reachable from any client that sends an
out-of-range integer, for example one passing an unsigned 64-bit value
through.

`INSERT` is not affected: BD-R5 judges a named pk from the literal's own
digits and refuses it `OutOfRange`. `PURGE` is not affected either:
BH-S5's fix makes the window's fold treat the literal as unread, and the
statement refuses it `InvalidArgument`.

## The fix it wants

- In `CoerceLiteralToColumn`'s default arm, refuse a wrapped literal against
  a signed integer column `OutOfRange` at its byte, as the DATE arm does.
  Leave uint64 out, since it reads the digits.
- Add `IntLiteralWrapped` to `PkBound` and the `kLookup` gate.
- Add a `SELECT`, `UPDATE` and `DELETE` row to the types contract suite.

Outside BH's scope (`instructions/v3.0.0/workorder-bh-purge-key.md` §7,
BH-S5's engine-fix review): the refusal is user-visible, and the
statements it changes are not `PURGE`.
