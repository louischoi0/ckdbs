# A `DELETE` on a self-referencing relation decides its RESTRICT by key order

**Reproduced, through the catalog's internal door only**, on
`fix-catalog-page-ceiling` at `8ae05f6a`, 2026-10-19. **Read unchanged** at
`f3ec25ea` (`origin/main`). First found by reading on
`worktree-fix-write-subquery-own-writes` at `541ce440`, 2026-10-14, in the
`critics-developer` review of the fix that makes a write collect its targets
when its `WHERE` reads the relation it writes (`docs/spec/parser-v2.md` R2).
**No client can reach it today**: SQL cannot declare a self-referencing
foreign key.

## What is wrong

`DeleteInner`'s mark walk runs the reverse foreign-key check
(`CheckNoChildrenBeforeDelete`, `src/server/command_dispatcher.cpp`) per row,
before that row's delete-mark. The check reads through a view that includes
the writer's own id. A child that this statement has already marked reads
`kAbsent` and is skipped (`src/exec/fk_check.cpp`), while one the walk has
not reached yet is live and refuses the statement `FK_VIOLATION`. The R2
collection pass does not change this: the mark walk is one key-ascending
`VisitRelation` either way. So on a self-referencing relation, key order
decides the verdict:

- `(1, NULL), (2, 1)`: the parent sorts first, its child is still live, and
  the statement is refused.
- `(2, NULL), (1, 2)`: the child sorts first and is marked, and both rows are
  deleted.

The earlier question, whether a btree walk always takes the first path, has
an answer: it always walks in ascending key order.

## Why no client reaches it

- `CREATE TABLE t (... REFERENCES t)` is refused at the bind: `REFERENCES` is
  resolved against the catalog before the new relation exists
  (`src/server/command_dispatcher.cpp`, CREATE TABLE's reference
  resolution). `tests/foreign_key_test.cpp`
  `ASelfReferencingForeignKeyCannotBeDeclared` pins the refusal, and
  `docs/spec/foreign-keys.md` states the arm is unreachable.
- `ALTER TABLE ... ADD COLUMN` refuses `REFERENCES` ("a foreign key is
  declared at CREATE TABLE only").
- A `DELETE` writes one relation (`DELETE ... USING` included), so no
  parent/child pair of two relations meets in one walk.
- `Catalog::CreateForeignKey` (`src/catalog/catalog.cpp`) and
  `CheckForeignKeyDeclaration` (`src/catalog/foreign_key.cpp`) do not refuse
  a child relation equal to its parent, so the internal door admits the
  self-reference.

## Smallest reproduction

A gtest on the `CommandDispatcher` fixture (`tests/alter_table_test.cpp`'s
shape), with the self-reference declared through
`Catalog::CreateForeignKey(oid, 1, oid)` on two relations of one shape:

```text
CREATE TABLE lo_parent (id int64, parent int64 NULL) BTREE   -- + CreateForeignKey(lo, 1, lo)
INSERT INTO lo_parent VALUES (1, NULL)
INSERT INTO lo_parent VALUES (2, 1)
CREATE TABLE hi_parent (id int64, parent int64 NULL) BTREE   -- + CreateForeignKey(hi, 1, hi)
INSERT INTO hi_parent VALUES (2, NULL)
INSERT INTO hi_parent VALUES (1, 2)
DELETE FROM lo_parent WHERE id > 0   -- ERR FK_VIOLATION ... row id=1 is still referenced by 'lo_parent.parent'
DELETE FROM hi_parent WHERE id > 0   -- DELETED 2
```

It gave the same result in 5 of 5 runs.

## What it costs

Latent: nothing today. If self-reference ever becomes declarable, the cost is
a refusal or an admission chosen by key order, never a wrong row. A child the
check skips really is deleted by the same statement, so no orphan is left.
Which verdict is correct for a self-referencing RESTRICT is the operator's
call (`docs/spec/foreign-keys.md`). It has to be decided before any change
admits self-reference, along with whether `CreateForeignKey` should refuse
`child == parent` in the meantime.
