# A column assigned twice checks its first foreign-key value and writes its last

**Found by reading, not reproduced.** Verified at `080cd555` on
`bj-expression-update`, by BJ-S0's review (`workorder-bj-expression-update.md`
§1.1, BJ-Q12).

## What is wrong

`UpdateInner` pairs each foreign-key column with the **first** assignment
that names it and stops (`src/server/command_dispatcher.cpp:7550-7554`,
the `break` after `fk_assignments.emplace_back`). The per-row check applies
that pair's verdict (`:7780-7784`). The write loop then assigns in list
order, so the **last** assignment to the column is the one written
(`:7803-7810`).

Nothing refuses a column named twice: `CompileAssignments`
(`src/exec/step_compiler.cpp:1355-1382`) checks each name for existence and
for the pk, never for repetition, and `ParseUpdate` has no such check.

## Smallest reproduction

A child relation `c(id, p)` with `p` a foreign key to parent `r(id)`, where
`r` holds row 1 and no row 2:

```sql
UPDATE c SET p = 1, p = 2 WHERE id = 7;
```

The check resolves and passes parent 1; the write stores `p = 2`.

## What it costs

A **quiet wrong answer**: a child row pointing at an absent parent, with no
error, past a constraint the engine otherwise enforces on every core.

## The fix

One compile-time check in `CompileAssignments`: a column named twice is
refused `InvalidArgument` at the second name's byte. It changes no statement
that means anything; today's last-wins is not a behaviour a client can have
relied on to mean something. BJ-Q12 proposes it as a fix of its own ahead of
BJ; BJ-R2 carries the same refusal into the expression grammar if it is not
fixed first.
