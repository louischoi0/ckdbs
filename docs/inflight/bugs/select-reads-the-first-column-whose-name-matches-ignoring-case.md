# A `SELECT` reads the first column whose name matches ignoring case

**Reproduced** by BW-S6's adversarial review on a scratch copy of
`worktree-bw-instant-add-drop-column` at `e1e17abc`, 2026-10-14; the code
path re-read on the same branch at the commit carrying this entry. Not caused
by BW-S6: the read side predates it.

## What is wrong

Column-name uniqueness is exact-case: `CREATE TABLE`, `ADD COLUMN` and
`RENAME COLUMN` compare names byte for byte (`catalog.cpp`'s collision scan,
`NameView(row.name) == new_name`), so `id`, `ID` and `Id` can all be columns
of one relation. Writes resolve names exactly as well - `UPDATE ... SET`,
Transitions, `RENAME COLUMN` and, since BW-S6, an `INSERT` column list
(`Schema::FindColumn`, BW-Q12.3). A read does not: `FindColumnPos`
(`src/exec/step_compiler.cpp`) compares with `IEquals` and returns the
first live column that matches. On a relation with two names that differ
only in case, a `SELECT` or `WHERE` naming the later one reads the earlier.

## Smallest reproduction

```sql
CREATE TABLE t (id int64, ID int64, Id int64 NULL);
INSERT INTO t (Id, ID, id) VALUES (1, 2, 3);
SELECT ID FROM t;      -- answers the tuple key column `id` (3), not `ID` (2)
```

The review's run answered `ID\n1\n3` over two rows - the values of `id`.

## What it costs

A **quiet wrong answer** (`[quiet-wrong]`): a read shows another column's
values under the name asked for, with no error. A write is never misplaced -
every write path matches exactly - but a value written to `ID` and read back
through `SELECT ID` comes back as `id`'s. BW-S6 makes such a relation easier
to fill correctly, which makes the wrong read easier to meet.

## The fix needs a ruling

Two directions, each closing it; neither is decided (BW-Q12.3 recorded the
read side as "a separate matter that BW-S6 does not change"):

- (a) refuse a column name that equals an existing live one ignoring case,
  at `CREATE TABLE`, `ADD COLUMN` and `RENAME COLUMN` - then case-folding
  reads are unambiguous, and the exact-match writes find the same column;
- (b) make `FindColumnPos` match exactly, as the writes do - this changes
  every statement that names a column in another case than it was declared
  in, which reads correctly today.

No work order carries it yet.
