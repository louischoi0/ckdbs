# Three routes resolve a name with no view, and one cache window is left

**Found by reading, not reproduced.** Read on
`worktree-fix-create-table-cross-core-visibility` at `7ad21023`, 2026-10-14,
by the two `critics-developer` reviews of the change that made the open-DDL
gate the instance's and put every dispatcher relation resolution behind
`ResolveNames` (`ddl-transactional.md` §4). `CatalogNameRigTest` pins the
cross-core binding that change fixed. Nothing here is reached by the count or
by the generation.

## What is wrong

- **`DROP INDEX`'s index name** (`src/server/command_dispatcher.cpp`, the
  `CREATE`/`DROP INDEX` handler; `src/exec/index_ddl.cpp`).
  `Catalog::FindIndexByName` takes no view, so `DROP INDEX i` can find an
  index that another transaction's open `CREATE INDEX i` wrote. It then
  borrows that index's relation and delete-marks a row nobody has decided.
  Whether `Catalog::DropIndex` refuses an undecided row has not been checked.
- **`CREATE CABIN`** (`src/exec/cabin_ddl.cpp`) resolves its relation
  unfiltered, with no transaction. It can write a `sys.cabins` row that names
  another transaction's uncommitted relation, on any core including its own.
  The row survives that relation's rollback and names a dead oid.
- **`C_LOAD_BEGIN`** (`src/server/kwp_load_server.cpp`) binds unfiltered and
  answers `S_LOAD_READY` for a relation the session cannot see. Each chunk
  re-resolves and refuses on a shape or oid mismatch, so nothing is written,
  but the client is told READY first.
- **A cache filled while a DDL was open.** An unfiltered lookup still fills
  the core's `CatalogCache`. Fillers include the duplicate-name check, the
  name renderer, and a resolution that `ResolveNames` then discards. Its
  entry is dropped at the next statement head once the schema word has moved.
  `EndDdlScopeById` moves the word before it uncounts. Even so, a statement
  head that `Revalidate`s before the move and reads the count after it can
  take the fast path and hit a cached oid the rollback removed.

## Smallest reproduction (not yet run)

`DROP INDEX`: `BEGIN; CREATE INDEX i ON t (v)` in one session. Another session
runs `DROP INDEX i`.

`CREATE CABIN`: `BEGIN; CREATE TABLE t (id int64, v int64)` in one session.
Another session runs `CREATE CABIN ON t (v)`, then the first runs `ROLLBACK`.
The reading predicts a `sys.cabins` row for the rolled-back oid.

## What it costs

- `DROP INDEX`: possibly a delete-mark over an undecided index row, not
  checked.
- `CREATE CABIN`: a catalog row naming a relation that never committed.
- `C_LOAD_BEGIN`: a misleading READY, then a refusal.
- The cache window: a statement bound to a rolled-back relation in a window
  of one statement head.

## Also left

A resolution that `ResolveNames` discards keeps whatever `IS` its compile
declared on the relation it bound, until the statement ends. The relation
was never bound for use, so this is not a wrong answer. A DDL wanting that
relation's `X` waits for the statement.

## The fix

Known in shape, unscheduled. Give `FindIndexByName` a view and resolve the
three routes through `ResolveNames`. `CREATE CABIN` has no transaction, so
it takes a committed-now view. The cache window closes if a statement head
reads the count before it revalidates.
