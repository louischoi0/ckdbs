# Raft marks - 2026-11-26

The operator's words of 2026-11-26, recorded in English.

## 1. A test that did not compile against OpenSSL 4, fixed and pushed through the hook

| | |
|---|---|
| **Word** | *"build ckdbs"*, then *"yes, proceed"* to CLA's proposal of a fix in a worktree, then *"proceed with option 1"* - merge to `main` and push, the known flaky cell read as such |
| **Mark** | `tests/tls_channel_test.cpp` builds its self-signed certificate's name fresh (`0239d156`), merged `--no-ff` at `8274c951` and **pushed to `main` through the pre-push hook, not skipped**: 5,224 of 5,224 passed, two cells disabled and two skipped. One earlier run of the suite on the work branch, at `-j8`, had failed `ContentionCountersTest.TwoCoresInsertingIntoOneRelationContendWhatAnInsertTakes` (`known-gaps.md`, "Testing"), which then failed 2 of 6 runs alone |
| **Does not settle** | the flaky cell; a tag or a version |
| **Recorded at** | this file |

## 2. DS written and opened on the standing go-ahead; DS-Q0..Q8 adopted

| | |
|---|---|
| **Word** | *"delete the worktree, and for the known-gaps work that is low in impact and needs no operator decision,"* `/go-ahead-achieving-milestone`: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session"* |
| **Mark** | **DS is written and opened** at `8274c951` (`v3.4.17-148-g8274c951e`), on worktree `ds-known-gaps-contained`, branch `worktree-ds-known-gaps-contained`: four entries of `known-gaps.md`, one stage each. **The scope is CLA's reading of the operator's phrase**, set out in the order's §0 and §6. **DS-Q0..Q8 are adopted (a) on that standing go-ahead.** They are CLA's proposals, not the operator's per-item mark. No item is `[quiet-wrong]` |
| **Does not settle** | the six gaps the order's §6 leaves out; a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; the DS order's header and §4; its row in `instructions/v3.2.0/index.md` and in `instructions/milestones.md` |

## 3. Twelve orders written from the known gaps, DT through EE; none opened

| | |
|---|---|
| **Word** | *"From the known gaps, pick out the large pieces of work worth registering as milestones"*, then, to CLA's list of twelve candidates, *"write a milestone work order for each item, one by one"* |
| **Mark** | **Twelve orders are written and none is opened**, on worktree `known-gaps-milestone-orders` (branch `worktree-known-gaps-milestone-orders`) from `8274c951` (`v3.4.17-148-g8274c951e`), documents only: **DT** (a write that gives its core back), **DU** (a keyed write), **DV** (unreachable pages reclaimed), **DW** (a btree's purge driver), **DX** (the pool under pressure), **DY** (the segment roll off the latch), **DZ** (the checkpoint's reactor residue), **EA** (a prepared statement parsed once), **EB** (the key forms in the executor), **EC** (a multi-core crash harness), **ED** (backup, a second pass), **EE** (the decide slot). **The letters are CLA's**, the next free after DS, which the other session of this day holds. **The selection and each order's scope are CLA's reading of `known-gaps.md`**, not the operator's per-item choice. **No item of any order is marked**: every `-Q` row carries CLA's proposal and waits. Candidate 7 of the list was restated when written - from the structure latch's per-fetch holds, which CW priced and abandoned, to what a checkpoint still does on its reactor (DZ §0) |
| **Does not settle** | whether any of the twelve is opened, or in what order; any `-Q` item; DY-Q0, which BQ-Q5 (b) reserves to the operator's own word; the two questions EA would reopen (BN-Q13, CX-Q37 (a)); the pending entry DU leaves pending (`docs/pending/keyed-write-beside-a-subquery.md`); K1's second exception (DW-Q4 (c)); a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; each order's header; their rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 4. DU opened on the standing go-ahead, and its push authorized in advance

| | |
|---|---|
| **Word** | *"In 4 hours, work the DU milestone with /go-ahead-achieving-milestone, and when it is done commit and push right away"* (given in Korean at about 14:58 UTC; recorded in English). The session scheduled the work and ran it at 18:58 UTC |
| **Mark** | **DU is opened** on worktree `du-keyed-write` (branch `worktree-du-keyed-write`) from `60dc593af` (`v3.4.17-150-g60dc593af`). **DU-Q0..Q6 are adopted as CLA proposed, (a) in each, on the operator's standing go-ahead of 2026-11-26 (go-ahead-achieving-milestone)**, each recorded in the order's §4; none is the operator's own per-item mark. Two are adopted with a correction CLA made against the tree at DU-S0: **DU-Q2 (a)** - a collection past `max_locks_per_txn` is not written from and the statement is answered by its walk, because a refusal on the size of a superset would refuse a statement the walk answers (DU-R4); **DU-Q3 (a)** - its premise does not arise: a write that bounds no tuple key declares nothing, so the index-driven write takes the walk's row units and no unit is designed. **DU-Q7 (a)**, an item CLA added at DU-S2 from its review, is adopted on the same go-ahead: on a heap a keyed write meets its rows in key order where its walk met them in page order, so a statement whose rows are judged against each other may change between an answer and a refusal; accepted and stated, which narrows DU-R4 on a heap. **The push at the close is authorized by the same word**: when DU closes, the branch is committed, merged to `main` and pushed without the Session Workflow's step 4 stop. **CLA's reading of "push"**: `git merge origin/main` on the work branch, `git merge --no-ff` on `main`, `git push origin main`; the hook and the suite are not waived |
| **Closed** | **DU is closed on the work branch the same day**, every stage landed there: DU-S0 `c2b138317` (the census, the premise: 12.9x and 308x, the bar passed), DU-S1 `b248c1cb1` (the bounded walk), DU-S2 `3abb5ea39` (the index-driven write), DU-S3 `0babe36e6` (a range, `ANALYZE` of a write, a write's plan), DU-S4 the close (`bench/v3.5.0/results-du-close-v3.4.17-155-g0babe36e6.md`: fixed costs level, the index arm 11.2 % slower than the walk at 1 row in 2 of a 100,000-row relation and a `SCAN` pin level with the walk). **DU-Q8 (a)**, an item CLA added at the close, is adopted on the same go-ahead: that loss is stated, bounded by DU-Q2's bound and answered by the administrator's pin; an arm declined by a count of rows is not built. None of DU-Q0..Q8 is the operator's own per-item mark |
| **Landed** | `5f9b6546` on `main`, 2026-11-26: the merge of `worktree-du-keyed-write` at `85577d87` onto `origin/main` at `60dc593af`, pushed with `--no-verify` - the hook skipped because the full suite had run green, 5,262 of 5,262, on the closing code's build inputs. No tag, no version |
| **Does not settle** | a tag, a tag's push or a version; DU-Q5's pending entry (`docs/pending/keyed-write-beside-a-subquery.md`), which stays pending; any other order of §3; how DU and DT are joined - `worktree-dt-write-path-yield` (another session's, not on `main` at `60dc593af`) edits the same walk, and whichever lands second merges the two |
| **Recorded at** | this file; `workorder-du-keyed-write.md`'s header, §4 and §7; its rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 5. DT opened and worked on a standing go-ahead; its push authorized ahead of its close

| | |
|---|---|
| **Word** | Four, in order, each to CLA in the session that had the twelve orders open. *"Reflect the worktree the milestones were written in into main."* Then *"push main."* Then *"In two hours, start the DT work with /go-ahead-achieving-milestone"*, and with it *"and when the work is done, commit and push at once."* The skill the third names carries the operator's standing word: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* |
| **Mark** | **The twelve orders are on `main`**: `worktree-known-gaps-milestone-orders` (`e2b6a313f`) merged at `60dc593af` and pushed, the hook skipped for a push of documents only, no suite run. **DT is opened** on worktree `dt-write-path-yield` from `60dc593af` and worked stage after stage in one session. **Every DT-Q item is CLA's proposal adopted on that standing go-ahead**, recorded as such in the order's §4 "Marks" and never as the operator's mark on the item; DT-Q1 was adopted as CLA revised it at DT-S0 (the bar read per mechanism). **The push of `main` at DT's close is authorized ahead of it**, by the fourth sentence: the Session Workflow's step 4 runs without a second word, the hook skipped only under the standing rule |
| **Does not settle** | a tag or a version; a force push; a push with the suite skipped; a push if DT does not close - a failing suite, a failed review or a measured regression stops it; any milestone but DT; the two questions DT-S0's measurement raised and handed to `known-gaps.md` |
| **At the close** | **DT closed narrow at `6e43ec8c0` and `main` was pushed under the fourth sentence.** The first close measured a regression (stalls of seconds under the checkpoint cadence), so nothing was pushed on it; a remedy was built, reviewed and measured again, and the stalls were gone. **Whether what remains is a regression was CLA's reading, not the operator's**: a paused btree walk's +6.4 % of server CPU and a repeated writing walk's 9 % to 21 % more log were read as the mechanism's price and recorded in `known-gaps.md`; the order's §7 ("DT-S4: the close") states the other reading and leaves it to the operator. The status `closed-narrow` is CLA's too: the write's page miss, half of the order's title, is not built. **DT landed second, so the join of DT and DU that §4 left unsettled was made in DT's merge**: `origin/main` had moved to `9f8f8b354` (DU, `5f9b6546`) when DT closed, both orders edit one write walk, and the conflicts were resolved on DT's branch, reviewed and run under the full suite before the push. The join is one tree neither close measured (the order's §7, "The merge with DU") |
| **Recorded at** | this file; `workorder-dt-write-path-yield.md` (header, §4 "Marks", §7); DT's rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 6. DS's handoff taken up; DS closed on option (a), its suite not green

| | |
|---|---|
| **Word** | *"open the recently handed-off work"*; then, to CLA's report that DS-S5 was not started because the earlier go-ahead was that session's, *"if the suite is green, proceed with DS-S5"*; then, to CLA's report that three runs were not green and its three options, *"a"* (each given in Korean; recorded in English) |
| **Mark** | **DS is closed** on worktree `ds-known-gaps-contained` (branch `worktree-ds-known-gaps-contained`), its code at `cc46c0d85` (`v3.4.17-151-gcc46c0d85`). **The operator's condition was not met and the operator then chose CLA's option (a)**: the suite at `cc46c0d85` ran three times and failed `ContentionCountersTest.TwoCoresInsertingIntoOneRelationContendWhatAnInsertTakes` in all three and `SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement` in the third (5,238, 5,238 and 5,237 of 5,239); both cells fail on the test binary of `8274c951`, the commit DS opened from (8 of 32 against 8 of 32; 163 of 400 against 196 of 400, not resolved); option (a) reads them as the flaky cells `known-gaps.md` names and closes, **the suite reported by its figures and never as green**. DS-Q7's measurement: no `INSERT` delta reaches the control (`bench/v3.5.0/results-ds-close-v3.4.17-151-gcc46c0d85.md`) |
| **Does not settle** | the two cells, which have no owner; a push, a merge to `main`, a tag or a version; the six gaps the DS order's §6 leaves out |
| **Recorded at** | this file; the DS order's header and §7 (DS-S5); `docs/inflight/known-gaps.md` ("Testing"); its rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |
