# Raft marks - 2026-11-22

The operator's words of 2026-11-22, recorded in English.

## 1. DO-S3: the operator's mark on E2's proof

| | |
|---|---|
| **Word** | *"go ahead"*, given after CLA's account of E2 - what it walks, why it is `[quiet-wrong]`, the proof's cases in the DO order's §5a, what it buys and costs, and that it waited for the operator's own mark - which ended "if you mark it, I build E2 with its crash cells, measure it like E1, and close DO" |
| **Mark** | **The operator's own mark on the DO order's §5a** (DO-Q3 (a)'s gate): E2, the checkpoint's dirty table walked a slot chunk per structure-latch hold, is built, with §5a's crash cells. Recorded as said: "go ahead", read as the mark the account asked for |
| **CLA's additions under it** | a handover between chunks (the latch's waiters counted on its contended path; the walker yields up to 20 us while one waits), because a measured fetch still waited out the whole walk without it; and §5a's merge corrected in the build to keep a page's lowest **non-zero** recLSN, so a 0 never hides a real one. Both recorded in the order's §7 as CLA's, not the operator's |
| **Does not settle** | a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; the DO order's header, §5a and §7; its row and this file's row in `instructions/v3.2.0/index.md` |

## 2. DO's E2 landed: push no verify

| | |
|---|---|
| **Word** | *"push no verify"*, given after CLA's account of where DO stood: closed on the work branch, E2 at `4b7b4855` waiting for a push |
| **Mark** | **E2 and DO's close land on `main`**: the work branch `worktree-do-frame-table-long-holds` (worktree `dg-wal-segment-body`; `4b7b4855`, its sync with `origin/main` `8f463edf`, and this record), merged `--no-ff` and pushed with `--no-verify`. The full suite ran 0 failed of 5107 on E2's build inputs, and nothing after it changed a build input (the sync and this record are documents only), so the hook was not run again. E2's measurement: `bench/v3.5.0/results-do-s3-e2-v3.5.0-62-g1cfe6c0f.md` |
| **Skipped on that word** | the pre-push hook |
| **Does not settle** | a tag or a version; the walking core's stall and the latch's unfairness between fetches, left in `known-gaps.md` |
| **Recorded at** | this file; this file's row in `instructions/v3.2.0/index.md` |
