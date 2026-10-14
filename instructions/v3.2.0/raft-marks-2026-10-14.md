# Ratification marks — 2026-10-14

**The operator's words of 2026-10-14**, recorded by CLA on
`worktree-cn4-transition-refresh` from `b0bc959c`. Each word is recorded
**in English**, translated from the Korean it was typed in.

## 1. CN-4 re-read

| | |
|---|---|
| **Word** | *"First fix the note to match the present."* Then, asked whether to rewrite the body or annotate it under `docs/conceptnotes/README.md`'s rule, the operator chose the annotation |
| **Mark** | CN-4's body stays as written at `d2e123d`. Its §9 records what the tree has moved since, at `b0bc959c` |
| **Does not settle** | Anything in CN-4 §8 |
| **Recorded at** | `docs/conceptnotes/cn4-transition-constraints.md` header and §9 |

## 2. CN-4 opened; BV written under v3.2.0

| | |
|---|---|
| **Word** | *"Now mark that concept note [open] and write a work order that carries the work plan against the current code. Mark it v3.2.0."* |
| **Mark** | CN-4 is `[open]` and cites BV. `instructions/v3.2.0/` opens with `workorder-bv-transition-constraints.md`. The operator names the version v3.2.0; no tag is taken |
| **Does not settle** | BV-Q0..Q5; CN-4 T1–T8 and T10; a tag |
| **Recorded at** | `workorder-bv-transition-constraints.md` header; `index.md`; CN-4's status line |

## 3. BV-Q1 marked (a)

| | |
|---|---|
| **Word** | *"Q1: $!a"* |
| **Mark** | **BV-Q1 (a)**: `sys.transitions` is a new row-codec catalog relation. Oid 146, column oids from 147, root page 16, overflow from 17, superblock 24 refusing 23 |
| **Does not settle** | Every other BV item |
| **Recorded at** | `workorder-bv-transition-constraints.md` header (W2) and §4 |

## 4. BV to its close on the standing go-ahead

| | |
|---|---|
| **Word** | The `go-ahead-achieving-milestone` skill, invoked on BV: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* |
| **Mark** | BV runs every stage to its close in this session. The following are adopted as CLA proposed and recorded as *"adopted on the operator's standing go-ahead of 2026-10-14 (go-ahead-achieving-milestone)"*: BV-Q0 (stage by stage, in §3's order); BV-Q2 (§0.1's grammar); BV-Q3 (`TRANSITION_VIOLATION`, wire category 18); BV-Q4 (a) (`PURGE` refused on a relation carrying a Transition); BV-Q5 (a CN-3 successor meets the initial set); CN-4 T1–T8 and T10 as CN-4 §8 proposes. None of these is the operator's own per-item mark |
| **Does not settle** | A push, a merge to `main`, a tag |
| **Recorded at** | `workorder-bv-transition-constraints.md` header (W3), §4 and §6; `index.md` |

_Sections 5-7 recorded by CLA on `worktree-bw-instant-add-drop-column` from `750496b1`._

## 5. BW written

| | |
|---|---|
| **Word** | *"I want to implement ALTER TABLE ADD/DROP now. The core is to guarantee the action takes effect as instantly as possible, while keeping the invariants"*, with the spare-slot idea (`workorder-bw-instant-add-drop-column.md` W0) |
| **Mark** | BW written; D-a..D-f marked (a) by the operator: *"D-a $!#1, D-b $!#1, D-c $!#1, D-d $!#1, the rest as proposed"* (W1) |
| **Recorded at** | `workorder-bw-instant-add-drop-column.md` header |

## 6. BW's measurement held

| | |
|---|---|
| **Word** | *"In this milestone skip every per-stage measurement, and hold the milestone measurement until directed."* (W2) |
| **Mark** | No BW stage measures; BW-S5 closes without the overhead measurement unless the operator directs it. This word is specific to BW and outranks `go-ahead-achieving-milestone` §4's close measurement |
| **Recorded at** | `workorder-bw-instant-add-drop-column.md` header and §5 |

## 7. BW opened on the standing go-ahead

| | |
|---|---|
| **Word** | `/go-ahead-achieving-milestone`: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* (W3) |
| **Mark** | **BW-Q1..Q7 adopted as CLA proposed, on the operator's standing go-ahead of 2026-10-14 (go-ahead-achieving-milestone)** - not marked per item by the operator. BW-Q3 `[quiet-wrong]`: its premise (one payload producer, `EncodeRow`) re-read at `750496b1` before adoption and holds |
| **Recorded at** | `workorder-bw-instant-add-drop-column.md` header and §4; `index.md`'s BW row |

_Sections 8-10 recorded by CLA on `worktree-bw-instant-add-drop-column` at `d6cb3826`._

## 8. BW pushed, and its close measured

| | |
|---|---|
| **Word** | *"push it"* (W4, after BW-S5), then *"do the A/B overhead measurement within 20 minutes now"* (W5, lifting W2's hold for that measurement) |
| **Mark** | BW landed on `main` at `541ce440`; the close measured A `74b0a3dd` / A' / B `541ce440` in Release (`bench/v3.2.0/results-bw-close-v3.2.0-22-g541ce440.md`): the every-row `UPDATE` +30.2 us / 1,000 rows outside the spread, every other cell inside |
| **Recorded at** | `workorder-bw-instant-add-drop-column.md` §6, §7 |

## 9. The `UPDATE` regression profiled and fixed

| | |
|---|---|
| **Word** | *"profile the UPDATE regression and fix it"*, then *"push it"* |
| **Mark** | Profiled (no BW-changed line carries the delta; layout/inlining suspected, not shown), the dropped-column gate hoisted (`e8727478`), re-measured (`results-bw-update-fix-v3.2.0-24-ge8727478.md`: below `541ce440` 7/7 on both paths, equality with the pre-BW tree unresolved), pushed at `d6cb3826` |
| **Recorded at** | `workorder-bw-instant-add-drop-column.md` §6 |

## 10. BW closed

| | |
|---|---|
| **Word** | *"close milestone"* (W6) |
| **Mark** | **BW closed** at `d6cb3826`. **Does not settle**: BW-Q11 (adopted (a) on the go-ahead; the operator's own mark on it is still the one it most needs), cleaning (D-e), and a quiet-host re-measure of `e8727478` against the pre-BW tree; no tag, no version |
| **Recorded at** | `workorder-bw-instant-add-drop-column.md` §6; `index.md`'s BW row; CLAUDE.md's ALTER TABLE row |

## 11. BW-Q11 marked (a); its fix taken into BW

| | |
|---|---|
| **Word** | *"BW-Q11 $!a include this fix in BW M"* (W7) |
| **Mark** | **BW-Q11 (a)**, now the operator's own mark: a positional `INSERT` written before a `DROP` is not caught by its count, and an `INSERT` column list is what closes it. **The column list is built in BW**, as BW-S6, not left to BU-S8: BW reopens for that stage (its close at §10 stands for S1-S5). BU-S8 keeps the rest of its row (`INSERT ... SELECT`, `VALUES` expressions, `RETURNING`, `UPDATE ... FROM`, `DELETE ... USING`, `ON CONFLICT DO NOTHING`). BW-S6's own design items are adopted as CLA proposes on the standing go-ahead of 2026-10-14 (W3), which covers BW |
| **Recorded at** | `workorder-bw-instant-add-drop-column.md` header, §4 BW-Q11, §5 BW-S6, §6; `workorder-bu-basic-query-surface.md` BU-S8's row; both index rows |

## 12. BW closed again, with BW-S6

| | |
|---|---|
| **Word** | *"push it"*, then, interrupting the merged tree's suite run, *"merge, close it, and /commit-push-no-verify"* (W8) |
| **Mark** | **BW closed** with BW-S6 at the merge `3b5daa80` of `origin/main` (`72cb79d9`). The merged tree's suite was **not run** on the operator's word; the last green suite is `33519b28`'s 3865/3865. **Does not settle**: the `SELECT` case-folding `[quiet-wrong]` the BW-S6 review found (`docs/inflight/bugs/select-reads-the-first-column-whose-name-matches-ignoring-case.md`, the operator's choice between refusing case-only-different names and exact matching), a positional `INSERT` written before a `DROP` (BW-Q11's remaining gap, `known-gaps.md`), cleaning (D-e); no tag, no version |
| **Recorded at** | `workorder-bw-instant-add-drop-column.md` §6; `index.md`'s BW row |

## 13. Column names unique ignoring case

| | |
|---|---|
| **Word** | *"Fix the SELECT case bug with $!b"*, then, after CLA's explanation of both options, *"a"* |
| **Mark** | **Option (a)**: a column name equal to an existing live one ignoring case is refused at `CREATE TABLE`, `ALTER TABLE ADD COLUMN` and `ALTER TABLE RENAME COLUMN` (and `CREATE TABLE` now refuses an exact repeat, which it admitted before). **CLA's addition, adopted on its own judgement and stated to the operator**: a read whose name matches two or more columns ignoring case - which a relation created before the fix can still hold - is refused `InvalidArgument` as ambiguous, naming the columns, at the name's byte, instead of reading the first. The `[quiet-wrong]` bug entry is deleted |
| **Recorded at** | `src/catalog/schema.cpp` (`ColumnNamesCollide`, `CheckColumnNamesUnique`, `Schema::MatchColumns`); `src/catalog/catalog.cpp` (`CreateTable`, `AddColumn`, `RenameColumn`); `src/exec/step_compiler.cpp` (`FindColumnPos`); `src/server/command_dispatcher.cpp` (`HandleCreateTableSql`, `NamesTupleKey`, the rename's byte); `src/parser/parser.cpp` and `include/kds/parser/ast.hpp` (`ColumnDef::name_byte_offset`); `tests/column_name_case_test.cpp`; `docs/spec/catalog.md` CT7; `docs/spec/alter.md` AL8, AL11; `docs/spec/parser-v2.md`'s resolution rules; `manual/sql/sql.md`; `docs/inflight/known-gaps.md` |

