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


## 3. BX written, BX-Q1..Q5 marked (a), BX opened

| | |
|---|---|
| **Word** | *"Compose the milestone for the UNIQUE constraint and index."* (W0), then *"BX-Q1..Q5 $!a, go ahead"* (W1) |
| **Mark** | **BX-Q1 (a)**: `CREATE UNIQUE INDEX` is the one surface; `UNIQUE` in `CREATE TABLE` and `ALTER TABLE ... ADD CONSTRAINT` stay refused, so BX-S4 is cut - **the operator's choice, against CLA's proposal (b)**. **BX-Q2 (a)**: the key unit is released once the row's entry is appended. **BX-Q3 (a)**: a busy verdict uses the waits that exist; row 2 onward of a multi-row `INSERT`, and a KWP load row past its chunk's first, is refused `TxnConflict`. **BX-Q4 (a)**: checked per row, immediately. **BX-Q5 (a)**: a nullable key column stays refused. Q2..Q5 are as CLA proposed. *"go ahead"* opens BX at BX-S1 |
| **Does not settle** | INSERT resume (BX-Q3 (b)); NULLs in a unique key; a foreign key referencing a unique column; no tag, no version |
| **Recorded at** | this file; `workorder-bx-unique-index.md` (header W1, §4, §5, §6); `index.md`'s row for it |

## 4. BX-Q6 marked (b), BX-S6 opened

| | |
|---|---|
| **Word** | *"Q6 $!b, go ahead"* (after CLA's account of the pages a refused `CREATE UNIQUE INDEX` leaves) |
| **Mark** | **BX-Q6 (b)**: a failed `CREATE INDEX` build frees its partial tree. Freeing pages never logged and named by nothing is ruled within BF's precondition (`page.md` §5: no record that names the page can be replayed again, no reader can reach it), so `FreePage` gains a second caller beside a dropped relation's reclaim. Built as BX-S6 |
| **Does not settle** | Pages a failed divide left unlinked; a build that fails after its tree is logged; a crash before the free map is written |
| **Recorded at** | this file; `workorder-bx-unique-index.md` (W2, BX-Q6, BX-S6); `index.md` §12; `known-gaps.md` |
