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
