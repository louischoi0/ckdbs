# A negative foreign-key value skips the parent check

**Reproduced** at `d13c79ea` on `worktree-bj-expression-update` by BJ-S4's
`critics-developer` review, with a scratch probe that was not committed. One
core, nothing concurrent. Older than BJ: the two sites below predate it.

## What is wrong

A foreign key references the parent's primary key, a unique id in
`[1, 2^40 - 1]`. Both places that check a child's value against the parent
let a value through unchecked when it is not a non-negative integer:

- `CommandDispatcher::CheckForeignKeyOnWrite` (`src/server/command_dispatcher.cpp`)
  begins `if (value.type != kInt || value.int_val < 0) return OK`.
- The hoist that resolves the parents once before the rows
  (`ResolveForeignKeyParents`) takes the same bail.

The comment at the first says a wrong-typed value "is refused by the codec a
moment later". That holds for a string. It does not hold for a **negative
integer**: `-7` is a valid `int64`, the codec stores it, and no parent has the
id `-7`.

Observed, on `parent (id, name)` holding ids 1 and 2 and
`child (id, pid REFERENCES parent, v)` holding `(1, 1, 10)`:

| statement | expected | actual |
|---|---|---|
| `UPDATE child SET pid = -7 WHERE id = 1` | `FK_VIOLATION` | `UPDATED 1` |
| `INSERT INTO child VALUES (2, -9, 10)` | `FK_VIOLATION` | `INSERTED` |
| `UPDATE child SET pid = 0 - 5 WHERE id = 1` (BJ-S4's folded constant) | `FK_VIOLATION` | `UPDATED 1` |

By reading, a `uint64` foreign-key column holding a value above `INT64_MAX`
(decoded as a negative `int_val`, with the digits in `raw_int_text`) has the
same hole.

## What it costs

A **quiet wrong answer**: a child row that references a parent that does not
exist, with no error, which the reverse check (a parent `DELETE`) then never
sees either. Reachable from any client that sends a negative integer to a
foreign-key column.

## The fix it wants

At both sites, only `kNull` passes without a check (MATCH SIMPLE's vacuous
pass). Any other value that is not an integer in `[1, 2^40 - 1]` is an absent
parent, `FK_VIOLATION` at the value's byte, and a non-integer type stays the
codec's refusal. Cells: a negative and a zero value and a value above `2^40`
through `INSERT`, `UPDATE`, a constant expression, and a `uint64` column.

Outside BJ-S4's scope (`instructions/v3.0.0/workorder-bj-expression-update.md`
§6): the refusal is user-visible and changes what `INSERT` answers.
