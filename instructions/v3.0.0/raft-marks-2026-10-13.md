# Ratification marks — 2026-10-13

**The operator's words of 2026-10-13**, recorded by CLA on
`worktree-bm-scan-kernel-open` from `fa80da9d`. Every word is recorded **in
English**, translated from the Korean it was typed in.

## 1. BP and BQ closed

| | |
|---|---|
| **Word** | *"Open the scan kernel milestone. First, leave close marks for BP and BQ."* |
| **Mark** | **BP closed** and **BQ closed**. Both had every stage landed and pushed (`441f8f75`, on `main` since `fe47814c`), and stood "not closed until the operator's word". The close's one open question - a `cores = 1` every-row `UPDATE` 3-6 % slower in every build from BQ's merge into BP's branch on, cause not found - is not answered by the mark; it is carried to `known-gaps.md`'s "Per-core structures kept (BP)" |
| **Does not settle** | That question's cause; BQ's rung 4 (BQ-Q5 (b) stands); BP-Q15 (b); BL-S4's ruling 2 and when BL resumes; a tag or a version |
| **Recorded at** | `workorder-bp-per-core-residue.md` header and §7; `workorder-bq-wal-segment-roll.md` header and §7; `index.md`'s BP and BQ rows; `known-gaps.md` |

## 2. BM opened

| | |
|---|---|
| **Word** | The same word: *"Open the scan kernel milestone."* The scan kernel is BM (`workorder-bm-scan-row-kernel.md`, `raft-marks-2026-10-12.md` §1-§3) |
| **Mark** | **BM-Q0: opened**, with BM-S0..S6 and BM-R1..R8 as written; BM opens now, in its own worktree, and BM-S1's gate decides whether S2..S5 follow. BM-S0's owed `critics-developer` review ran at the opening (`workorder-bm-scan-row-kernel.md` §6) |
| **Does not settle** | BM-Q1..Q8 and BM-Q10..Q12 (marked at §3); any stage past BM-S1 before BM-Q1's gate reads |
| **Recorded at** | `workorder-bm-scan-row-kernel.md` header, §4 and §6; `index.md`'s BM row |

## 3. BM-Q items adopted as proposed

| | |
|---|---|
| **Word** | *"Adopt the BM-Q items as CLA proposed."* (given while the opening review was running) |
| **Mark** | **BM-Q1..Q12 marked as CLA proposed**: Q1 as written (the one-third gate, `[provisional]`), Q2 (a) the kernel a pre-filter, Q3 (a) a NULL operand passed to the residual, Q4 as written, Q5 (a) `scan_kernel = on\|off` on by default, Q6 as written, Q7 yes, Q8 as written (BM-S5 gated at 10 %), Q9 (a), Q10 out, Q11 BM, Q12 as written. A finding of the opening review that changes a proposal is applied to the order and adopted under §4's go-ahead, not under this word |
| **Does not settle** | BM-Q1's and BM-Q8's readings, which BM-S1 measures; a push, a merge to `main`, a tag or a version |
| **Recorded at** | `workorder-bm-scan-row-kernel.md` header and §4; `index.md`'s BM row |

## 4. BM to its close on the standing go-ahead

| | |
|---|---|
| **Word** | The `go-ahead-achieving-milestone` skill, invoked on BM: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* |
| **Mark** | BM runs every stage to its close in this session. A decision a stage reaches is adopted on CLA's proposal and recorded as *"adopted on the operator's standing go-ahead of 2026-10-13 (go-ahead-achieving-milestone)"*, never as the operator's own mark. The first such adoptions are the opening review's revisions: BM-S2 as the VM-owned btree loop, BM-R6's check per step per statement, BM-Q12's spread taken across A and a rebuilt A' (`workorder-bm-scan-row-kernel.md` §6, "BM opened") |
| **Does not settle** | A push, a merge to `main`, a tag or a version |
| **Recorded at** | `workorder-bm-scan-row-kernel.md` header, §4 and §6; `index.md`'s BM row |
