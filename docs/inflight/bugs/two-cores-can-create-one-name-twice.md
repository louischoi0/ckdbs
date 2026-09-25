# Two cores can create one name twice

**Found by reading, not reproduced.** Verified at `2b20369` on
`at-s12-prose-sweep`, by AT-S12's code-comment sweep of `src/catalog/`
(the work order's AT-7 item 16: the catalog was never swept for rules
AT-S5's routing retired).

## What is wrong

Every name-creating DDL checks that the name is free and then writes it,
as two separate latch holds:

- `CREATE TABLE`, both forms: `CommandDispatcher::HandleCreateTable`
  (and the SQL form) calls `Catalog::FindTableOidByName`, then
  `Catalog::CreateTable`, whose `InsertObjectRow` inserts the
  `sys.objects` row under that page's latch taken afresh.
- `ALTER TABLE ... RENAME TO`: `Catalog::RenameTable` checks
  `FindTableOidByName(new_name)`, then rewrites the row. `HandleAlter`
  holds no relation `X` over it, and the new name has no lock unit.
- `CREATE NAMESPACE`: `Catalog::CreateNamespace` checks, then inserts.

Nothing holds across the pair. That was sound while every DDL ran on core
0 and the reactor ran it to completion; **AT-S5 made a DDL run where its
session is**, so two cores can both find a name free and both write it.
`ddl-transactional.md`'s own reason for the unfiltered duplicate check -
*"both creates would succeed, and two rows would claim one name"* - is
the outcome, reached by concurrency rather than by a filtered view.

The pattern that closes it is in the tree: `Catalog::RegisterPattern`
holds its relation's root page across the check and the insert.

## Smallest reproduction (not yet written)

Two cores, a barrier between each session's `FindTableOidByName` and its
`CreateTable`, both creating `t`. Both answer `OK`; `sys.objects` holds two
`t` rows. The two-core rig and a barrier plus rounds is the shape.

## What it costs

Two relations under one name: `FindTableOidByName` answers whichever row
it meets first, so statements naming `t` reach one relation and the other's
rows are unreachable by name - a **quiet wrong answer**, and catalog state
a later mount carries forward. For `RENAME` and `CREATE NAMESPACE` the same
shape, one level up.

## The fix

Known in shape, unscheduled: hold the `sys.objects` root page exclusive
across the check and the insert (`RegisterPattern`'s shape), or give a
name a lock unit in the lock family (`txn.md` §5). Which is a decision; no
work order carries it yet.
