# The operator's words of 2026-11-01

## 1. CL written: the reservations of an undecided transaction

| | |
|---|---|
| **Prompt** | CK's close report (`ck-assertion-commit-settle`, `a59f9a2e`) named two paths that leave an undecided transaction's assertion reservations in their group totals after its rows are undone: a crash loser (reproduced on the base, `b66a0dbe`) and a failed `TransactionManager::Commit` (found by the CK-S1 review's reading, not reproduced). A departure on either fails open `[quiet-wrong]`. CLA recommended one order for both |
| **Word** | W0: *"open the crash-loser and failed-commit fix order"* |
| **Mark** | **CL written** (`workorder-cl-assertion-reservations-of-an-undecided-transaction.md`), **not opened for work**. The letter CL is CLA's proposal (CJ is another session's, CK is closed); the operator may reletter it. CL-Q0..Q5 carry CLA's proposals, and none is marked |
| **Does not settle** | any CL-Q; the order's S0; a push of anything but documents; a tag, a version |
| **Recorded at** | this file; the order's header; the CL row and this file's row in `index.md` |
