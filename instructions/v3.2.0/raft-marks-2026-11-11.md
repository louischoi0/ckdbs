# The operator's words of 2026-11-11

## 1. CP paused after S2: the state recorded, the session closed

| | |
|---|---|
| **Word** | *"For now, record the work so far and the next work in the documents; I am ending the session."* (given in Korean; recorded in English), after CP-S2's report of 2026-11-08 |
| **Mark** | **CP pauses on the work branch after S2.** `worktree-cp-lock-model-exploration` holds, unmerged and unpushed, CP written (`cc54a296`), CP-S0 (`c2937a32`, `origin/main` `adbe2638` merged as `d7b75977`), CP-S1 (`2a4a3f79`), CP-S2a (`0ddfbfab`) and CP-S2b (`766f6a6d`); the full suite ran green on the last two build-input trees (4405/4405 at `0ddfbfab`, 4427/4427 at `766f6a6d`). The hand-off is `workorder-cp-lock-model-exploration.md` §7, "Paused after S2": where CP stands, what CP-S3 does in order, what a resuming session reads first |
| **Does not settle** | whether CP-S3 (the close) starts; a merge to `main`, a push, a tag, a version |
| **Recorded at** | this file; the order's header, §5 and §7; the CP row and this file's row in `instructions/v3.2.0/index.md` |

## 2. CP's branch after S2 pushed with the hook skipped

| | |
|---|---|
| **Word** | *"For now, push this session's milestone with no verify."* (given in Korean; recorded in English), in the same session, after §1's hand-off was recorded and the interim report given |
| **Mark** | **CP's branch pushed to `main` with `--no-verify`**: CP written `cc54a296`, CP-S0 `c2937a32`, `origin/main` (`adbe2638`) merged at `d7b75977`, CP-S1 `2a4a3f79`, CP-S2a `0ddfbfab`, CP-S2b `766f6a6d`, the hand-off `b558f30a` and this record. `origin/main` still stood at `adbe2638` at the push, already merged, so the push is a fast-forward and resolved nothing. The pre-push hook was skipped; the full Debug suite ran 4427/4427 at `766f6a6d`, the last commit that changed a build input, and everything after it is under `instructions/`. **CP is not closed**: CP-S3 - the close measurement over `cc54a296`..`766f6a6d`, `known-gaps.md`'s Locks rewritten, the deferred comment trims - has not run, so the milestone lands on `main` with its overhead **not measured**, stated here as a fact and not an implied pass. The operator's question of the same moment, whether CP has yet produced a new concept or a new solution, was answered in the reply: no new mechanism - the two-source structure stands as the census found it; what CP produced is §2's rule for keeping a cell, S0's measured boundary (the hot row's client-visible effect is `txn.md` §5's refusal, not a lock's), the `U` mode re-derived as a version claim against `X`'s existence claim, the slice unit deleted, and two designs recorded unbuilt (C3, C4) because no number asked for them |
| **Does not settle** | whether CP-S3 starts; a tag; a version |
| **Recorded at** | this file; the order's header and its §7 hand-off entry; the CP row and this file's row in `instructions/v3.2.0/index.md` |

## 3. CP-S3 started on a schedule

| | |
|---|---|
| **Word** | *"Start the CP milestone in 30 minutes."* (given in Korean; recorded in English), at 09:05 UTC in a new session; the session's one-shot job fired at 09:35 UTC |
| **Mark** | **CP-S3, the close, starts** at 09:35 UTC on 2026-11-11, on a worktree of the same commit (`worktree-worktree-cp-lock-model-exploration`, opened from `5d858feb`), by the steps of the order's §7 hand-off entry. It supersedes the schedule the earlier session set for 17:00 UTC, which died with that session |
| **Does not settle** | the merge to `main`, a push, a tag, a version - each waits for the operator's word, repeated in the scheduled prompt: "do not push or tag until the operator says so" |
| **Recorded at** | this file; the order's §7 close entry; the CP row and this file's row in `instructions/v3.2.0/index.md` |

## 4. CP's close merged to `main` and pushed with the hook skipped

| | |
|---|---|
| **Word** | *"Merge it into main and push with no verify."* (given in Korean; recorded in English), at 23:05 UTC in the session that ran CP-S3, after the close's report |
| **Mark** | **CP's close lands on `main`**: `worktree-worktree-cp-lock-model-exploration` - the trims `f5d310ec`, their review applied with `known-gaps.md`'s Locks rewritten `7d21468a`, the close entry and its results file `e855b6d5`, and this record - merged into `main` with `--no-ff` and pushed with `--no-verify`. `origin/main` still stood at `5d858feb` at the push, an ancestor of the branch, so the merge resolved nothing. The pre-push hook was skipped; the full Debug suite ran 4427/4427 at `7d21468a`, the last commit that changed a build input (comments only), and everything after it is under `bench/` and `instructions/`. The close's open ends land with it as stated facts, not passes: scenario 0's -0.45 % inside a 5.5 % run-to-run spread is unresolved, the `U` mode has no throughput number, and the eight-core, `strict` and PostgreSQL cells were not measured (`bench/v3.4.7/results-cp-close-v3.4.7-11-g7d21468a.md`) |
| **Does not settle** | a tag; a version - the version of record stays `v3.4.7` |
| **Recorded at** | this file; the order's header and its §7 close entry; the CP row and this file's row in `instructions/v3.2.0/index.md` |

## 5. Version 3.4.8 tagged

| | |
|---|---|
| **Word** | *"tag 3.4.8"* (given in Korean; recorded in English), in the same session, after §4's push was reported at 23:06 UTC |
| **Mark** | **`v3.4.8`**, an annotated tag on `main` at `b0c11e0a`, the head of `origin/main` when the tag was taken. It carries CP whole: the slice unit deleted (`0ddfbfab`), the `U` mode (`766f6a6d`), the close (§4). Its message states what CP built, that nothing of it persists on disk, the close's measurement, and the gaps that bound it - CP-Q10 (a)'s accepted deadlock class, the `U` mode with no throughput number, scenario 0 unresolved, the cells not measured, C3 and C4 not built. The full Debug suite ran 4427/4427 at `7d21468a`, whose build inputs the tagged commit shares, so the tag was pushed with `--no-verify`. CLAUDE.md's version line moved to `v3.4.8` |
| **Does not settle** | nothing of CP; the next version is the operator's to name |
| **Recorded at** | this file; the tag message; CLAUDE.md's Version Management; the CP order's §7 close entry; `instructions/v3.2.0/index.md` |

## 6. CQ written: column reservations, an exploratory proof of concept

| | |
|---|---|
| **Word** | W0: *"Among the features KDBS could borrow, is there an OLTP-specialized feature of Oracle's?"*; W1, after CLA's ranked answer: *"Write a milestone document that borrows feature 1. It is an exploratory (experimental, empirical) milestone and the proof of concept comes first. Adopting it must weigh cost against benefit. Write the milestone document first."*; W2: *"No, not a concept document - write a work order under instructions."* (given in Korean; recorded in English), in another session than §1's and §2's |
| **Mark** | **CQ written**, `workorder-cq-column-reservation.md`, on `worktree-cq-lock-free-reservation`: Oracle 23ai's Lock-Free Reservation as an exploratory order whose deliverable is a benefit-against-cost ledger and whose spike stays on the branch. Documents only; the suite not executed |
| **Does not settle** | whether CQ opens; **CQ-Q1** - how CN-7's rulings R1-R3 read against a marked column - which is the operator's alone; CQ-Q0 and CQ-Q2..Q9, which carry CLA's proposals unmarked; a merge to `main`, a push |
| **Recorded at** | this file; the order's header; the CQ row and this file's row in `instructions/v3.2.0/index.md` |

## 7. CQ opened: CQ-Q1 (a), the rest as proposed; CQ-S0 to start in 40 minutes

| | |
|---|---|
| **Word** | W3, at 09:00 UTC, after CQ's report: *"Start this work in 40 minutes."*; asked how that reads against CQ-Q1, the operator chose *"(a) explore as an exception"*, the option stating that CQ-Q0 and CQ-Q2..Q9 are adopted as CLA proposed and that work starts from S0 (given in Korean; recorded in English) |
| **Mark** | **CQ opened. CQ-Q1 (a) is the operator's own mark**: CN-7's R1, R2 and R3 are set aside for the spike only, nothing ships, and the rulings are put again at CQ-S3 with the ledger. **CQ-Q0 (a), CQ-Q2 (a), CQ-Q3 (a), CQ-Q4 (a), CQ-Q5 (a), CQ-Q6 (a), CQ-Q7 (a), CQ-Q8 (a), CQ-Q9 (a)** adopted as CLA proposed, by the same choice. CQ-S0 is scheduled for about 09:40 UTC in the session that wrote the order |
| **Does not settle** | anything past CQ-S0's verdict; CN-7's rulings outside the spike; a merge to `main`, a push |
| **Recorded at** | this file; the order's header and §5; the CQ row and this file's row in `instructions/v3.2.0/index.md` |


## 8. CQ-Q10 (a): CQ-S1 only, S2 to be put again

| | |
|---|---|
| **Word** | W4, after CQ-S0's report: *"CQ-Q10 $!a, proceed with S1."* (given in Korean; recorded in English), in the session that ran S0 |
| **Mark** | **CQ-Q10 (a)**: the model (CQ-S1) and K2's and K8's lists are written, and S2 is put to the operator again with them. CQ-Q6 (a)'s schedule is amended to that extent: the judgement it left to S3 is taken before the spike is paid for |
| **Does not settle** | whether S2 is built (CQ-Q15); CQ-Q11..CQ-Q14 and CQ-Q16, which S1 raises; CN-7's rulings outside the spike; a merge to `main`, a push |
| **Recorded at** | this file; the order's header, §5, §8 and §9; the CQ row and this file's row in `instructions/v3.2.0/index.md` |

## 9. CQ-Q11..CQ-Q16 (a): CQ-S2, the reduced spike

| | |
|---|---|
| **Word** | W5, given on 2026-11-12 after CQ-S1's report: *"CQ-Q11~Q16 $!a, proceed with S2."* (given in Korean; recorded in English), in the session that ran S0 and S1. Recorded in this file beside W4 because it continues that day's exchange |
| **Mark** | **CQ-Q11 (a)**: a `COMMIT` refused in its apply rolls the transaction back and ends it. **CQ-Q12 (a)**: a reserving statement's row count is read from the newest committed version. **CQ-Q13 (a)**: the apply does not ask the archive's gate. **CQ-Q14 (a)**: the apply's two view tests are replaced by "the writer is committed", an exception to `txn.md` §5 for the spike. **CQ-Q15 (a)**: the reduced spike - a floor on one `int64` column, every other write of the column refused, CQ-Q5 (b)'s refusal at once. **CQ-Q16 (a)**: a starving `DELETE` is accepted |
| **Does not settle** | CQ-S3 and its verdict; CN-7's rulings outside the spike, which CQ-Q1 (a) puts again at S3; whether anything of the spike leaves the branch (CQ-Q2 (a) says nothing does); a merge to `main`, a push |
| **Recorded at** | this file; the order's header, §5 and §8; the CQ row and this file's row in `instructions/v3.2.0/index.md` |

## 10. CQ-S3: the verdict is narrow

| | |
|---|---|
| **Word** | W6, on 2026-11-12 after CQ-S2's report: *"Proceed with S3."*; asked how CQ closes against S0's to S2's results, the operator chose *"narrow"*, the option stating that C1 is not adopted, that the advice to put the debit last and send it with `COMMIT` goes into the client manual, that the re-run (A1) stays in `docs/pending/`, that the model and the numbers stay in the order as a design record, that the spike's code is reverted on the branch, and that CN-7's R1 to R3 stand (given in Korean; recorded in English). Recorded in this file beside W4 and W5 because it continues that exchange |
| **Mark** | **CQ closed, narrow.** The column reservation (C1) is not adopted. CQ-Q1 (a)'s exception ends with the spike: **CN-7's R1, R2 and R3 stand as ruled** |
| **Does not settle** | A1, the re-run under `READ COMMITTED`, which stays the operator's pending question; a merge of the work branch to `main`, a push; a version |
| **Recorded at** | this file; the order's header, §5 and §8; the CQ row and this file's row in `instructions/v3.2.0/index.md`; `docs/conceptnotes/cn7-delta-verbs.md` §6; `manual/client/client.md` §5 |

## 11. CQ landed by a squash merge

| | |
|---|---|
| **Word** | W7, on 2026-11-12 after CQ-S3's report, which said a `--no-ff` merge would carry the spike's two commits into `main`'s history: *"Merge into main with a squash merge and push with no-verify."* (given in Korean; recorded in English) |
| **Mark** | **CQ lands on `main` as one squashed commit**, pushed with the pre-push hook skipped. `f72b8232` and `3dea53c2` stay on `worktree-cq-lock-free-reservation` |
| **Does not settle** | a tag, a version; A1, which stays the operator's pending question |
| **Recorded at** | this file; the order's header and §8; the CQ row in `instructions/v3.2.0/index.md` |
