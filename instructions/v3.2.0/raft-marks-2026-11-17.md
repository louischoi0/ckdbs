# Raft marks, 2026-11-17

The operator's words of 2026-11-17 that open a new matter, and what CLA
took under them.

## 1. A keyed write beside other conjuncts taken into CU as CU-S3b; the subquery case left pending

| | |
|---|---|
| **Word** | W1, on 2026-11-17, after CLA reported CU-S3 (`84bf6828`, the branch at `94f3c4a3`) and, asked, explained that a write descends only where the tuple key's conjunct is its whole `WHERE`, that the row function asks the whole clause of the row a descent finds so the gate can be widened, and that a `WHERE` holding a subquery is the one case that cannot simply follow: *"Take it into CU, leave the subquery case as pending work, and carry on with the work"* (given in Korean; recorded in English) |
| **Mark** | **CU-Q14 (a)**, the operator's own mark: an `UPDATE` or `DELETE` descends for a single tuple key equality or a tuple key list beside any other conjunct, in CU. **The subquery case is deferred**: a `WHERE` holding a subquery keeps its two walks, recorded in `docs/pending/keyed-write-beside-a-subquery.md`. **CU-S3b is opened**, on worktree `cu-key-set-access` from `94f3c4a3` |
| **Does not settle** | the opening of CU-S4, the close: CLA reads "the work" as the stage this word takes into CU, as every earlier word of CU named one stage, and stops after it; any option of the pending entry; a write over a bound on the tuple key or through an index or a Cabin, which CLA named as larger and separate; a merge, a push, a version |
| **Recorded at** | this file; the order's header, items table (CU-Q14), stages table and §7; `docs/pending/keyed-write-beside-a-subquery.md` and its row in `docs/pending/README.md`; the CU row and this file's row in `instructions/v3.2.0/index.md` |
