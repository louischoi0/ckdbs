# Ratification marks — 2026-10-16

The operator's words of 2026-10-16, recorded by CLA. Each is recorded
**in English**, translated from the Korean it was typed in.

## 1. BX closed

| | |
|---|---|
| **Word** | *"Push."* (2026-10-15) - then *"Close the milestone."* |
| **Mark** | **BX closed.** BX-S1, S2, S3, S5 and S6 landed (S4 cut by BX-Q1 (a)) and were pushed to `main` at `0479cb29` with the hook skipped, the full suite having run green on the build inputs pushed (4055/4055 at `7ea52998`; the later merge brought `instructions/` only). `CREATE UNIQUE INDEX` is built and enforcing (`docs/spec/index.md` §10.2); a failed build frees its unlogged tree (BX-Q6 (b)). The close measurement ran at `9bf559e2` (`bench/v3.2.0/results-bx-close-v3.2.0-62-g9bf559e2.md`); BX-S6 is unmeasured, running only on a failed build |
| **Does not settle** | `ON CONFLICT DO NOTHING` and `ON CONFLICT (<col>)` against a unique index (`known-gaps.md`, Unique indexes); INSERT resume (BX-Q3 (b)); NULLs in a unique key; `UNIQUE` in `CREATE TABLE`; a foreign key referencing a unique column; no tag, no version |
| **Recorded at** | this file; `workorder-bx-unique-index.md` header and §7; `index.md`'s BX row and this file's row; `CLAUDE.md`'s secondary-index row |

## 2. BY written, BY-Q1..Q6 marked (a), BY opened

| | |
|---|---|
| **Word** | *"Then write the milestone document for implementing the REINDEX feature and the DROP INDEX page reclaim feature."* (W0), then *"BY-Q1..Q6 $!a, go ahead"* (W1) |
| **Mark** | **BY-Q1 (a)**: `REINDEX INDEX <name>` only. **BY-Q2 (a)**: a rebuilt index takes a new `index_oid` and keeps its name. **BY-Q3 (a)**: a settled anchor slot is **removed**, by a new record kind - **the operator's choice against CLA's proposal (b)** (reuse in place). **BY-Q4 (a)**: `REINDEX` in autocommit only, refused `NotImplemented` inside `BEGIN` - **the operator's choice against CLA's proposal (b)**. **BY-Q5 (a)**: the name check admits a name whose holder this transaction itself delete-marked. **BY-Q6 (a)**: the close measures entry count, pages and probe CPU before and after `REINDEX`, the pages `DROP INDEX` returns, and the unchanged A/B. *"go ahead"* opens BY at BY-S1 |
| **Does not settle** | `REINDEX TABLE`; an online (non-`X`) rebuild; a purge of a live tree; no tag, no version |
| **Recorded at** | this file; `workorder-by-reindex-drop-index-reclaim.md` (header W1, §4, §5, §6); `index.md`'s row for it |
