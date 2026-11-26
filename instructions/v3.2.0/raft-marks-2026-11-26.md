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
