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

## 2. The column-name case work closed; BW stands closed with it

| | |
|---|---|
| **Word** | *"close milestone"* (after the column-name case work landed on `main` at `12170d1d`) |
| **Mark** | **Closed.** The work the operator's marks of 2026-10-14 §13 (`$!b`, then option (a)) and 2026-10-15 §1 opened - column names unique ignoring case, every resolution ignoring case with the ambiguity refusal - is landed: `2cc15aca`, `00800684` (option (a) and its review), `c4281ba4` (§1, one resolver), `46d723ba` (its review: `DROP COLUMN` writes the position its RESTRICT was asked of), merged at `12170d1d` (`v3.2.0-49-g12170d1d`). Its bug entry is deleted and its known-gaps line removed. **BW stays closed** (`raft-marks-2026-10-14.md` §12) with BW-Q12.3 overridden by §1. The suite last ran green at `46d723ba` (3886/3886); the merge `12170d1d` and this record landed with it not run. No overhead measured: resolution runs at compile and DDL, never per row |
| **Does not settle** | Relation names (untouched); a relation created before uniqueness-ignoring-case holding a case pair, whose two columns no statement can name (stated in the manual and `catalog.md` CT7); BW-Q11's positional-`INSERT` gap; cleaning (D-e); no tag, no version |
| **Recorded at** | this file; `index.md`'s row for it; `workorder-bw-instant-add-drop-column.md` §6 |

