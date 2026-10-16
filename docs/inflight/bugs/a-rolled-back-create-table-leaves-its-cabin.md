# A rolled-back `CREATE TABLE ... CABIN` leaves its `sys.cabins` row

**Reproduced** on `worktree-fix-names-resolved-with-no-view` off
`01d04fa8`, 2026-10-16, on the two-core rig (`TwoCoreRig`), by a probe the
`critics-developer` review of that branch's fix asked for. The probe was
not kept.

## What is wrong

A column declared `CABIN` gets its Cabin as part of the `CREATE TABLE`
(`src/server/command_dispatcher.cpp`, the `CABIN` block of the create
handler, `catalog_.CreateCabin`). That write is made with no transaction,
like `CREATE CABIN`'s, so a rollback of the transaction that created the
relation does not take it back. The row then names an oid no relation
holds.

`CREATE CABIN` itself resolves under a committed-now view since the fix of
`names-resolved-with-no-view.md` (`docs/spec/ddl-transactional.md` §4), so
it no longer reaches this. The `CREATE TABLE` route does.

## Smallest reproduction

```
BEGIN;
CREATE TABLE t (id int64, v int64 CABIN);   -- CREATED oid=4000
ROLLBACK;
SHOW CABINS;
```

`SHOW CABINS` answers
`1,4000,NULL,1,user,active,0,0,0,0,column,1`: an active Cabin on oid 4000,
whose relation name is NULL because no relation has that oid.

## What it costs

A catalog row naming a relation that never committed. No query answer is
wrong: no relation has the oid and oids are never reissued, so nothing
probes the Cabin. Its observed sets cost memory, and the row stays until
something removes it by id.

## The fix

The operator's choice, not scheduled. Either make the Cabin's row a
transactional write of the `CREATE TABLE` that declared it, so a rollback
retires it with the relation, or refuse a `CABIN` column clause inside an
explicit transaction, as `CREATE CABIN` on an uncommitted relation now is.
