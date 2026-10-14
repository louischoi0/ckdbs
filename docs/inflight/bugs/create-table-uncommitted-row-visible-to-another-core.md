# An uncommitted `CREATE TABLE` can be bound by another core inside one window

**Narrowed, not closed.** Reproduced at `7ad21023` on
`worktree-fix-create-table-cross-core-visibility`, 2026-10-14, by a probe on the
two-core rig. The same branch made the open-DDL count instance-wide
(`InstanceVisibility::AnyDdlOpen`, `ddl-transactional.md` §4). That closed
the always-on case. The window below was found by that change's
`critics-developer` review and has not been reproduced.

## What was wrong, and is fixed

`CommandDispatcher::ViewFor` resolved names unfiltered whenever *its own
core's* `ddl_txns_` was empty. With core 0 in `BEGIN; CREATE TABLE t`, core 1's
`SELECT * FROM t` answered `id,v`, and an autocommit `INSERT INTO t VALUES (5)`
answered `INSERTED`. Core 0's `ROLLBACK` then removed the relation together
with that acknowledged row. `CatalogNameRigTest.AnUncommittedCreateIsInvisibleToAnotherCoreUntilItCommits`
and `ARolledBackCreateWasNeverVisibleToAnotherCore` now pin the fix.

## What is still open

`ViewFor` reads the count once, and the caller resolves the name afterwards.
The interleaving that still binds:

1. Core 1 reads a count of zero.
2. Core 0's `DdlScopeFor` counts its DDL, and `CREATE TABLE` writes the
   `sys.objects` row.
3. Core 1's unfiltered `FindTableOidByName` scans that page and binds the
   uncommitted oid.

`CREATE TABLE` holds no `X` on the new oid, so core 1's `IX` is granted and the
`INSERT` succeeds; core 0's rollback then takes the row away. On one core the
window does not exist: a dispatcher runs one statement at a time and nothing
yields between `ViewFor` and the lookup.

The count alone cannot be re-checked after the lookup. A rollback followed by
its decrement returns it to zero, so a reader would bind a dead oid.

Two routes resolve a name without `ViewFor` at all, independent of the count:

- `src/exec/cabin_ddl.cpp`: `CREATE CABIN` resolves unfiltered with no
  transaction, so it can write a `sys.cabins` row naming another transaction's
  uncommitted relation. The row survives that relation's rollback.
- `src/server/kwp_load_server.cpp`: `C_LOAD_BEGIN` binds unfiltered and answers
  `S_LOAD_READY` for a relation the session cannot see. Each chunk re-resolves
  and refuses, so nothing is written, but the client is told READY first.

## Smallest reproduction (not yet run)

On the two-core rig with reactors running: a loop on core 0 of
`BEGIN; CREATE TABLE t_n; ROLLBACK`, against a loop on core 1 of
`INSERT INTO t_n VALUES (1)` for the same `n`. The prediction is an occasional
`INSERTED` for a relation that never committed.

## What it costs

A quiet wrong answer when it hits: an acknowledged autocommit write is lost
by another transaction's rollback. The window is the time between one
core's count read and its catalog scan, so it is narrow.

## The fix

Not decided. Two shapes:

- **An open generation** beside the count. A reader records it, resolves,
  and re-resolves filtered if it moved. This touches every `ViewFor` caller.
- **`CREATE TABLE` takes `X` on the new oid before it writes the row.** A bound
  reader's or writer's lock then waits for the decide, and a rolled-back
  relation is refused as dangling. This needs every bind to re-check after
  the grant.
