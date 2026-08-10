# DROP TABLE v1 — workplan

Tasks `DT01`-`DT05` for `docs/spec-drop-table.md` (DT1-DT6).

## DT01 — Parser: `DROP TABLE <name>`  **[DONE 2026-08-10]**

`DropTableStmt {table_name, byte_offset}` joins the variant; the DROP arm
of `Parse()` gains TABLE; the dispatcher's DROP router gains the route.
Corpus lines with `-` hashes. Files: `parser.cpp`, `ast.hpp`,
`tests/drop_table_test.cpp`, corpus.

## DT02 — Catalog: `DropTable(oid)` and the tombstone  **[DONE 2026-08-10]**

`kTypeDroppedTable` in `well_known.hpp`; `Catalog::DropTable(oid)`
retypes the `sys.objects` row (DT2), retires the `sys.tables` row, every
`sys.columns`/`sys.indexes`/`sys.cabins` row and the child-side
`sys.fkeys` rows (RetireSlot, the RetirePattern precedent), and bumps
once. Returns the dropped cabin ids for the store's Forget.

## DT03 — Dispatcher: `HandleDropTable` and the RESTRICT gate  **[DONE 2026-08-10]**

Resolve, refuse `sys.*`, scan `sys.fkeys` by `parent_rel_oid` and refuse
naming the child, consult `AssertionsOnRelation()` and refuse naming the
assertion, then DT02's call, `CabinStore::Forget` per cabin id, reply
`DROPPED TABLE <name> oid=<o>`.

## DT04 — Tests  **[DONE 2026-08-10]**

E2E: drop → `NotFound`; re-CREATE same name works and gets a **new** oid
(DT2 pinned through the `INSERTED oid=` reply); RESTRICT both blockers
(refuse naming them, then succeed after removing them); indexes and
cabins gone from `SHOW`; a pattern ghost is harmless; `sys.*` refused;
ROLLBACK keeps the drop (DT5); retired column slots reusable.

## DT05 — Docs  **[DONE 2026-08-10]**

Manual (`DROP` gains TABLE; the "no DROP TABLE" lines update),
`docs/known-gaps.md` (pages orphan; the FK reverse-check RESTRICT now
has its caller), CLAUDE.md milestone row.
