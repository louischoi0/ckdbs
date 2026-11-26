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

## 6. DS's handoff taken up; DS closed on option (a), its suite not green

| | |
|---|---|
| **Word** | *"open the recently handed-off work"*; then, to CLA's report that DS-S5 was not started because the earlier go-ahead was that session's, *"if the suite is green, proceed with DS-S5"*; then, to CLA's report that three runs were not green and its three options, *"a"* (each given in Korean; recorded in English) |
| **Mark** | **DS is closed** on worktree `ds-known-gaps-contained` (branch `worktree-ds-known-gaps-contained`), its code at `cc46c0d85` (`v3.4.17-151-gcc46c0d85`). **The operator's condition was not met and the operator then chose CLA's option (a)**: the suite at `cc46c0d85` ran three times and failed `ContentionCountersTest.TwoCoresInsertingIntoOneRelationContendWhatAnInsertTakes` in all three and `SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement` in the third (5,238, 5,238 and 5,237 of 5,239); both cells fail on the test binary of `8274c951`, the commit DS opened from (8 of 32 against 8 of 32; 163 of 400 against 196 of 400, not resolved); option (a) reads them as the flaky cells `known-gaps.md` names and closes, **the suite reported by its figures and never as green**. DS-Q7's measurement: no `INSERT` delta reaches the control (`bench/v3.5.0/results-ds-close-v3.4.17-151-gcc46c0d85.md`) |
| **Does not settle** | the two cells, which have no owner; a push, a merge to `main`, a tag or a version; the six gaps the DS order's §6 leaves out |
| **Recorded at** | this file; the DS order's header and §7 (DS-S5); `docs/inflight/known-gaps.md` ("Testing"); its rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |
