# Ratification marks — 2026-10-21

The operator's words of 2026-10-21, recorded in English. Each section names
what the word settles and what it does not.

## 1. BS resumed on the standing go-ahead

| | |
|---|---|
| **Word** | *"BS /go-ahead-achieving-milestone"* - *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* - after CLA's listing of the open milestones, in which BS (`instructions/v3.0.0/workorder-bs-test-hardening.md`) stood open for S0 and S1 with S0 done, and BR's close (2026-10-20, `raft-marks-2026-10-20.md` §3) had lifted the gate on S2-S8 |
| **Mark** | **BS resumed** in the worktree `bs-test-hardening`, from `origin/main` at `e0dd4d04` (`v3.3.0-68-ge0dd4d04`). Every item the operator marked on 2026-10-13 stays as marked (BS-Q0..Q5, §6.1). Items that surface are adopted on the standing go-ahead of 2026-10-21 (go-ahead-achieving-milestone), not marked by the operator item by item; each is in the order's §7 (BS-A1 onward) |
| **Does not settle** | push, merge to `main`, tag, version |
| **Recorded at** | this file; the order's header and §7; `instructions/v3.0.0/index.md`'s BS row; `instructions/v3.2.0/index.md`'s row for this file |

## 2. CF written, as an exploratory milestone

| | |
|---|---|
| **Word** | W0: *"Then what about turning Waystone off by default and having the DBA create it explicitly?"* - after CLA's direction analysis of the day and its reading of when a trail is recorded. CLA answered with an order: measure replay first; if it does not pay, remove it or at the least turn it off by default; only if it pays on particular shapes, an opt-in that is not a second model of a pattern (`CREATE PATTERN` having been withdrawn on 2026-08-31). W1: *"For now, write a milestone in this order and push it, as an exploratory milestone."* |
| **Mark** | **CF written** (`workorder-cf-waystone-premise.md`) on `cf-waystone-premise` from `23ea3e61` (`v3.3.0-81-g23ea3e61`), **exploratory**: CF-S0 measures the premise and points at one of three branches - R (removed), D (off by default), O (opt-in per pattern) - and a branch not taken is recorded, not built. **Pushed on W1** with the commit that adds this file, documents only, with the pre-push hook skipped as its own header admits for a documentation-only push |
| **Does not settle** | CF's opening; CF-Q0..Q7, each with CLA's proposal beside it and none marked; the branch; CF-Q6, the project's stated purpose, which is the operator's alone; a tag, a version |
| **Recorded at** | this file; the order's header; this file's and the order's rows in `instructions/v3.2.0/index.md` |

## 3. CF opened, CF-Q0..Q5 marked (a), S0 started

| | |
|---|---|
| **Word** | W2: *"Open with CF-Q0..Q7 $!a and proceed with S0"* - after CLA's report that CF was written and pushed at `a1809988`, which named CLA's proposals as Q0 (a), Q1 (b), Q2 (a), Q3 (a), Q4 (a), Q5 (a) |
| **Mark** | **CF opened.** CF-Q0 (a) the operator marks the branch after S0's report; **CF-Q1 (a)** the bar without the 5 % floor, against CLA's (b); CF-Q2 (a) three arms in rotating order; CF-Q3 (a) `sys.patterns` deleted under R; CF-Q4 (a) the keys refused by name under R; CF-Q5 (a) invariants 8 and 9 kept, re-worded. CF-Q6 and CF-Q7 have no lettered option, so `$!a` marks nothing in them: CF-Q6 stays the operator's open question, CF-Q7 is asked only if S0 points at Branch O. **CF-S0 started** on this word |
| **Does not settle** | the branch (CF-Q0 (a) keeps it the operator's); CF-Q6; a push of anything beyond documents; a tag, a version |
| **Recorded at** | this file; the order's header, §4 and §7; the order's row in `instructions/v3.2.0/index.md` |
