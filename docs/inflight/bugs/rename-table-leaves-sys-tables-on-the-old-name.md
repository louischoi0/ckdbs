# `RENAME TO` leaves `sys.tables` on the old name

**Reproduced** on `build-sandbox` at `228fc689` (`v3.2.0-114`), 2026-10-18,
while the web documentation was being checked against v3.2.0. **Read
unchanged** at `abbe8150` (`origin/main`, 2026-10-17): neither
`Catalog::RenameTable` nor `CommandDispatcher::TablesView` changed in a way
that touches it. Not reproduced at `abbe8150`, because no build of it was run.

## What is wrong

`ALTER TABLE t RENAME TO t2` rewrites the relation's `sys.objects` row and
nothing else (`src/catalog/catalog.cpp`, `Catalog::RenameTable`). A
`sys.tables` row also holds the relation's name (`SysTableRow::name`,
`include/kds/catalog/rows.hpp`), written once by `CREATE TABLE` and never
rewritten. The `sys.tables` view and `SHOW TABLES` read that copy
(`src/server/sys_views.cpp`, `TablesView`), so they go on showing the old name
for as long as the relation lives. Name resolution reads `sys.objects`, and
so do `sys.columns`, `sys.relation_stats` and `SHOW BUDGET`. Those show the
new name.

The divergence survives a restart: both rows are logged and replayed as
written. It does not depend on a transaction. An autocommit rename, a
committed one and a rolled-back one leave the same state. A rolled-back one
looks most wrong, because the rename is not transactional (`ddl-transactional.md`
§5) and stays, while `sys.tables` still shows the name from before the
transaction. That is how it was first noticed.

The freed old name can be taken again, which makes `sys.tables` show two
rows under one name in one namespace.

`RENAME COLUMN` is not affected. `sys.columns` holds the only copy of a
column name.

## Smallest reproduction

Against a fresh server:

```text
CREATE TABLE u (id int64)
ALTER TABLE u RENAME TO u2
SELECT oid, name FROM sys.tables WHERE oid >= 4000      -- 4003, u
SELECT oid, name FROM sys.objects WHERE oid >= 4000     -- 4003, u2
SHOW BUDGET                                             -- 4003, u2
SELECT * FROM u                                         -- ERR no table with this name
CREATE TABLE u (id int64)                               -- CREATED oid=4005
SHOW TABLES                                             -- 4003, u and 4005, u
```

## What it costs

A **quiet wrong answer** from the catalog views. A client that lists tables
through `sys.tables` or `SHOW TABLES` gets a name that does not resolve, and,
once the name is reused, two rows it cannot tell apart. The web sandbox's
schema panel builds its table list from `sys.tables`, so a visitor's renamed
table is shown, and offered for `SELECT`, under its old name. No data is
wrong, and nothing the engine resolves by name is affected.

## The fix

Not scheduled, and no work order carries it. Two shapes, not yet weighed:

- `TablesView` takes the name from the `sys.objects` row it already holds
  (`object.name`). This also corrects every volume written before the fix,
  because the stale copies are never read again. Any other reader of
  `SysTableRow::name` has to be checked for the same reliance.
- `RenameTable` also rewrites the `sys.tables` row, under the same
  transactionless, logged overwrite it uses for `sys.objects`. A volume
  renamed before the fix keeps its stale copy unless a mount repairs it.
