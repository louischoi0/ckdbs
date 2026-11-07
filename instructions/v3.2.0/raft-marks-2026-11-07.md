# The operator's words of 2026-11-07

## 1. CP written: the lock model re-examined, an exploratory milestone

| | |
|---|---|
| **Word** | *"A milestone opens here. First we re-explore the relations among the locks - `IS`, `S`, `IX`, `U` and the like. It is an exploratory, experimental milestone. The gist: not the conventional lock system, but one that is new, fits the KDS system better, is optimized, and keeps correctness - that is what we research."* (given in Korean; recorded in English) |
| **Mark** | **CP written, not opened** (`workorder-cp-lock-model-exploration.md`) on `worktree-cp-lock-model-exploration` from `6c4b9a6d` (`v3.4.7`). The order reads every ask site of the family (§1.2: eight live cells of twenty, the slice `IS` paid at every leaf page and read by nothing, `U` absent with one live conversion deadlock - the foreign key's, scenario 0's shape), states §2's rule for keeping a cell, and proposes five candidates (§3: C1 the slice `IS` deleted, C2 the `U` mode, C3 a per-relation lock header, C4 the row entry made lazy over a per-transaction decide slot, C5 fairness); a sixth, release before durability, was withdrawn at the opening review because the tree already releases before the `group` sync (`known-gaps.md:1417` is to be corrected). Reviewed the same day by the `critics-developer` pass, twelve findings applied (the order's §7). CP-Q0..CP-Q9 carry CLA's proposals, none marked. Exploratory: S0 measures inside the standing budget, S1 writes the model, S2 builds what S1 kept, S3 closes |
| **Does not settle** | CP's opening; CP-Q0..Q9; whether any candidate is built; E1's and E12's ratified status (CP-Q2, CP-Q3 would amend them on the operator's mark); whether a fenced bulk write's wait may become a refusal (CP-Q4); a push, a tag, a version |
| **Recorded at** | this file; the order's header; the CP row and this file's row in `instructions/v3.2.0/index.md` |
