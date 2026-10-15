# Ratification marks — 2026-10-15

**The operator's words of 2026-10-15**, recorded by CLA on
`worktree-fix-select-column-case` from `00800684`. Each word is recorded
**in English**, translated from the Korean it was typed in.

## 1. Column names match ignoring case, reads and writes

| | |
|---|---|
| **Word** | *"Don't distinguish case for either reads or writes."* |
| **Mark** | **Every column-name resolution matches ignoring ASCII case**, through one resolver (`Schema::FindColumn`, `ColumnNamesCollide`'s rule): a read, a `SET` value's column read, a `SET` target, an `INSERT` column list, `DROP COLUMN` and `RENAME COLUMN`'s old name, and the column `CREATE INDEX`, `CABIN`, `ASSERTION` or `TRANSITION` names, a stored assertion canon re-parsed at mount included. A name matching two or more live columns - only a relation created before names were unique ignoring case (§13 of `raft-marks-2026-10-14.md`) can hold them - is refused `InvalidArgument` as ambiguous at the name's byte, on every path; no statement can then name either column of such a pair. A dropped column never matches. K2 is asked of the resolved column, so `SET ID` on a tuple key `id` is refused `Unsupported`. A rename onto the stored name byte for byte stays `AlreadyExists`; one differing only in case is a rename. **Overrides BW-Q12.3** (an `INSERT` column list matched exactly). Uniqueness ignoring case stands as built |
| **Does not settle** | Relation (table) names, which are untouched |
| **Recorded at** | `include/kds/catalog/schema.hpp`, `src/catalog/schema.cpp` (`Schema::FindColumn` the one resolver); `src/catalog/catalog.cpp` (`RenameColumn`, `DropColumn`); `src/exec/step_compiler.cpp` (`FindColumnPos`, K2 in `CompileAssignments`); `src/exec/set_expr.cpp`; `src/exec/index_ddl.cpp`, `src/exec/cabin_ddl.cpp`, `src/exec/assertion_catalog.cpp` (comments); `src/server/command_dispatcher.cpp` (`HandleAlter`'s rename, `NormalizeListedInsertRows`, `PurgeConjunctRefusal`); `tests/column_name_case_test.cpp`, `tests/insert_column_list_test.cpp`; `manual/sql/sql.md`; `docs/spec/parser-v2.md`'s resolution rules; `docs/spec/catalog.md` CT7; `docs/spec/alter.md`; `docs/spec/types.md` BJ-Q4; `docs/rules/keystoneid-invariant.md` K2; `workorder-bw-instant-add-drop-column.md` BW-R13, BW-Q12.3 |
