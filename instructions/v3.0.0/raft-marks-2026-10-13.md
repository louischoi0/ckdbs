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

## 5. BM-S1's second round stopped

| | |
|---|---|
| **Word** | *"Finish it with the data you have now."* (after CLA said the measurement's end could not be fixed and offered to close it on the first round) |
| **Mark** | BM-S1's results file is written from the first round alone. The second round (a 200-row cell, `pool_misses` per arm, a second writers cell and profile) is stopped and stated as not run |
| **Does not settle** | Anything the second round was to measure |
| **Recorded at** | `workorder-bm-scan-row-kernel.md` §6, BM-S1; `bench/v3.0.0/results-bm-s1-scan-row-v2.7.0-942-gf5a7efb2.md` |

## 6. Two standing words: `%%%`, and a measurement's time budget

| | |
|---|---|
| **Word** | *"`%%%` means \"tell me the current progress\"."* and *"Unless the operator directs otherwise, a measurement runs within 20 minutes per stage and around 1 hour per milestone; keep this rule until directed otherwise."* |
| **Mark** | `%%%` is added to CLAUDE.md's operator abbreviations: a request for the progress of the work running in the session, read off its files and logs at the moment of asking. A stage's measurement is planned and run inside 20 minutes and a milestone's close inside about an hour, building included; a design that cannot fit is cut to what fits and the results file says what was cut. BM-S6's close measurement and its remedy re-measure were the first held to it |
| **Does not settle** | A budget for any measurement the operator sizes separately; what `&*S` means (asked, not answered) |
| **Recorded at** | CLAUDE.md, *Operator abbreviations* and *Working Rules*; `bench/v3.0.0/results-bm-close-v2.7.0-946-g6b771ec1.md` §1 |

## 7. BT opened

| | |
|---|---|
| **Word** | *"`$!a`"* - the operator's only words - choosing CLA's option (a) after BM's close, which CLA had written as: a new milestone on `AcceptTupleAt`'s per-row cost, a read-only survey first, then a work order with its premise measured first |
| **Mark** | **BT is opened** (`workorder-bt-accept-tuple-row.md`), lettered by CLA. BT-Q1..Q3 were put with CLA's proposals beside them and were not marked item by item |
| **Does not settle** | BT-Q1..Q3 (§8 adopts them); a push, a merge to `main`, a tag or a version |
| **Recorded at** | `workorder-bt-accept-tuple-row.md` header and §4; `index.md`'s BT row |

## 8. BT to its close on the standing go-ahead

| | |
|---|---|
| **Word** | The `go-ahead-achieving-milestone` skill, invoked on BT: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* |
| **Mark** | BT runs every stage to its close in this session. BT-Q1 (a), BT-Q2 (a) `[quiet-wrong]` and BT-Q3 (a) are adopted as CLA proposed, recorded as *"adopted on the operator's standing go-ahead of 2026-10-13 (go-ahead-achieving-milestone)"*, never as the operator's own marks; so is any later decision a stage reaches |
| **Does not settle** | A push, a merge to `main`, a tag or a version |
| **Recorded at** | `workorder-bt-accept-tuple-row.md` header, §4 and §6; `index.md`'s BT row |

## 9. BN and BT closed

| | |
|---|---|
| **Word** | *"Close the BN and BT milestones."* |
| **Mark** | **BN closed** and **BT closed** on the operator's word. Both stood closed by CLA under a standing go-ahead - BN at its close on 2026-10-11 (`bench/v3.0.0/results-bn-close-v2.7.0-787-g8679367e.md`), BT at BT-S5 on 2026-10-13 (`bench/v3.0.0/results-bt-close-v2.7.0-997-g4274ed81.md`, on `main` since `5ff40594`) - with no mark of the operator's own; this is that mark. It settles nothing either order left open: BN-S2, S6 and S7 stay not built, each with its number, and BT's remainders stay in `docs/inflight/known-gaps.md` ("The scan row (BM)") and `docs/inflight/bugs/payloadat-reads-a-slot-that-leaves-the-page.md` |
| **Does not settle** | A tag or a version; the `PayloadAt` fix, which waits for its own word |
| **Recorded at** | `workorder-bn-opt-read-update-path.md` and `workorder-bt-accept-tuple-row.md` headers; `index.md`'s BN and BT rows |
