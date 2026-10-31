# The operator's words of 2026-10-31

## 1. CA closed

| | |
|---|---|
| **Word** | *"close CA"* |
| **Mark** | **CA closed** (`workorder-ca-background-advisory-optimizer.md`, header and §7). Its premise was refuted at CA-S0 (the advisory work is 0.2-0.6 % of sampled server CPU on scenarios 1-4), so CA-S1..S3 were never built; the kept candidates are C1 `txn::VisibilityMemo`, C3 `ExecStats::For` inline and C4 `storage::InlineCellBytes`, all on `main`. Its close was measured at `v3.2.0-159-g954abc20` (`bench/v3.2.0/results-ca-close-v3.2.0-159-g954abc20.md`): scenarios 1 and 4 improved, scenarios 2 and 3 unchanged within noise. What it leaves is §8 of the order and `docs/inflight/known-gaps.md`'s "The advisory optimizer, in the background (CA)". No suite was run for this record, which adds only `instructions/` |
| **Does not settle** | Scenario 2's commit-protocol change; the advisory structures CA-R1..R6 describe; a push; a tag, a version |
| **Recorded at** | this file; the CA order's header; the CA row and this file's row in `instructions/v3.2.0/index.md` |

## 2. CJ written

| | |
|---|---|
| **Word** | The operator pasted the spec "Wait Events & ASH" (decisions WD1-WD13 stated as settled on 2026-10-31) and its workplan (WE01-WE14), then, asked what to do with them, chose *"work order"* |
| **Mark** | **CJ written** (`workorder-cj-wait-events-ash.md`), translated into English, **not opened**. WD1-WD13 are recorded as the operator's decisions as the paste states them; the paste's `[PROPOSED]` values stay proposed. CLA's additions are tagged `[CLA]` and CJ-Q0..Q7 are unmarked. Nothing is built, measured or read against the tree yet |
| **Does not settle** | CJ's opening; CJ-Q0..Q7; any stage; a push; a tag, a version |
| **Recorded at** | this file; the CJ order; the CJ row in `instructions/v3.2.0/index.md` |

## 3. CJ opened on the standing go-ahead

| | |
|---|---|
| **Word** | `/go-ahead-achieving-milestone`, with CJ the milestone this session is on: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session"* |
| **Mark** | **CJ opened** on `worktree-cj-wait-events-ash` from `ed32c10d`. CJ-Q0..Q7 are adopted as CLA proposed and CJ-Q8 (a page read is not a suspension) was added and adopted at WE03. None is the operator's own per-item mark; each is recorded in the order's §8 as adopted under this word |
| **Does not settle** | a push, a merge to `main`, a tag, a version; the milestone's close (the stages are not all built) |
| **Recorded at** | this file; the CJ order's header, §4 and §8; the CJ row in `instructions/v3.2.0/index.md` |
