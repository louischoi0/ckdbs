# ALTER TABLE v1 — workplan

Tasks `ALT01`-`ALT05` for `docs/spec-alter.md` (AL1-AL9). Sequenced so the
refusal surface exists before anything can be half-accepted, and the
catalog write lands before the dispatcher can reach it.

## ALT01 — Parser: the two statements and the whole refusal surface  **[DONE 2026-08-10]**

`AlterStmt` in the AST: `{table_name, kind = kRenameTable | kRenameColumn,
new_name, old_column, byte offsets}`. `ALTER` joins the statement-head
dispatch; `alter`/`rename`/`to`/`column` are ordinary identifiers matched
by text (AL7). Refusals with bytes: `ADD`/`DROP`/`MODIFY`/`SET`/`ALTER`
after the table name answer `Unsupported` carrying AL1's reason;
malformed names are `InvalidArgument`. Corpus lines gain `-` hashes
(`ALTER` is not a patternable head). Files: `src/parser/parser.cpp`,
`include/kds/parser/ast.hpp`, `tests/parser_alter_test.cpp`, corpus.

## ALT02 — Catalog: RenameTable and RenameColumn  **[DONE 2026-08-10]**

`Catalog::RenameTable(oid, new_name)` and `RenameColumn(oid, old, new)`:
find the row (`ForFirstRow`'s shape), rewrite the fixed-width `Name`
field in place — same size, no relayout, `MutatePatternRow`'s precedent —
then `BumpVersion()` (AL5, no in-place-cache exception). Collision and
length checks per AL8; `sys.*` refused by namespace before anything else.
Files: `include/kds/catalog/catalog.hpp`, `src/catalog/catalog.cpp`,
`tests/catalog_test.cpp`.

## ALT03 — Dispatcher: HandleAlter and the AL4 gate  **[DONE 2026-08-10]**

`HandleAlter` beside the other DDL handlers: resolve the relation, consult
`exec::AssertionsOnRelation()` and refuse with the first assertion's name
(AL4 — the predicate's first call site), then call ALT02's mutator and
reply `RENAMED`. The "unknown SQL keyword" list gains `ALTER`. Admitted in
an explicit transaction with `CREATE TABLE`'s non-transactional caveat
(AL6). Files: `src/server/command_dispatcher.cpp`.

## ALT04 — The oid-identity proof  **[DONE 2026-08-10]**

The test the feature is *for* (AL2), one scenario per reference class,
each asserting behavior across the rename with no re-declaration:
- FK: child references parent by oid → rename parent → the check still
  enforces, and the error message names the new parent.
- Index: serve, rename, serve again — same bytes, `index_scanned` still
  moving.
- Cabin: observe, rename, probe again — still served.
- Waystone/pattern: record a trail, rename, old-name statement now fails
  resolution, new-name statement is a fresh pattern (AL3 — dying is the
  specified behavior).
- Cache: old name answers `NotFound` after the bump; a pre-rename
  `DESCRIBE` output and a post-rename one differ only in the name.
- Assertion RESTRICT: declare an assertion, both rename forms refuse
  naming it; drop it, the rename proceeds.
Files: `tests/alter_table_test.cpp`.

## ALT05 — Docs and manual  **[DONE 2026-08-10]**

`manual/sql/sql.md` gains the ALTER section (grammar, the AL1 refusal
table, AL3's pattern note, AL4's RESTRICT); `docs/known-gaps.md` notes
the unlogged-DDL exposure applies to renames; `CLAUDE.md`'s milestone
table gains the row. Move nothing out of `spec-alter.md` — it is the
canon.
