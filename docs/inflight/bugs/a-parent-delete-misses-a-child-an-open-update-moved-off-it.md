# A parent DELETE misses a child an open UPDATE moved off it

**Reproduced.** Verified at `64b97e7` on `ay-s4-fk-cells-red`, by AY-S4's
cell
`FkCrossCoreRigTest.DISABLED_AParentDeleteWaitsOutAChildMovedOffItAndRefusesAtItsRollback`
(`tests/fk_cross_core_rig_test.cpp`), red 5/5. **`[quiet-wrong]`**: the
parent is deleted and the child's rollback leaves an orphan, with nothing
refused.

## What is wrong

A committed child row `c(1, pid = 7)` references parent 7. Transaction A
runs `UPDATE c SET pid = 8 WHERE id = 1` and stays open. Transaction B runs
`DELETE FROM p WHERE id = 7`, and it answers `DELETED 1`. A then rolls
back, which puts the child back at `pid = 7`, and 7 is gone.

The reverse check (`CheckNoChildReferences`, `src/exec/fk_check.cpp`)
reads each child row's **in-page** payload and skips it when the fk value is
not the parent's, *before* it asks the row's visibility. The in-page
version is A's, `pid = 8`, so the row is skipped. The version B's view would
see, `pid = 7`, is never read, and A's in-flight write never makes the check
busy.

**Inferred, not run: it is not a cross-core defect.** Nothing in the
visitor asks which core wrote the row, so two sessions on one core should
reach it the same way. The two-core rig is only where the cell lives.

AY's order put this shape under AY-Q8 as "refused, not waited". That is
right for a child `DELETE`, which the check sees as busy and refuses
`TxnConflict` (the sibling cell
`DISABLED_AParentDeleteWaitsOutAnOpenChildDeleteAndPassesAtItsCommit`). It
is wrong for an `UPDATE` that moves the fk column: the check answers early,
not refuses.

## Where it is fixed

AY-S5, under AY-Q8 as marked (`raft-marks-2026-09-30.md` §2). A parent
`DELETE` that meets a child row whose writer holds no `S` on the parent
parks mid-walk on that writer. For this shape, meeting the row has to mean a
row whose writer is undecided and whose **visible** version references the
parent, not only the in-page one. The cell is AY-S5's exit.
