# Raft marks - 2026-11-21

The operator's words of 2026-11-21, recorded in English.

## 1. DL landed: push no verify

| | |
|---|---|
| **Word** | *"go ahead"*, given at DL's push gate, after the report that ended "the work is held only by the push gate" |
| **Mark** | **DL lands on `main`**: the work branch `worktree-dl-statistics-queue-no-statement`, synced with `origin/main` at `01a7609b` (documents and bench only) and again at `2c2639df` (DD's `PURGE INDEX` and DN-S0/DN-S1, with build inputs), merged `--no-ff` and pushed. The full suite ran 0 failed of 5,052 on the merged tree, whose build inputs are the pushed tree's, so the push goes out with `--no-verify` (the pre-push hook skipped, the suite not run again by it). `tsan` ran 235 cells with no report at `43cecfa0`; `bs-nightly` not run. The close measurement: `bench/v3.4.17/results-dl-close-v3.4.17-11-g30e169df.md` |
| **Skipped on that word** | the pre-push hook |
| **Does not settle** | a tag or a version; the reader storm's tail and DL-Q4's two words, left in `known-gaps.md` |
| **Recorded at** | this file; the DL order's header and §7; its row in `instructions/v3.2.0/index.md` |

## 2. DK opened on the standing go-ahead

| | |
|---|---|
| **Word** | *"The open milestones by priority, /go-ahead-achieving-milestone"* (given in Korean; recorded in English) |
| **Which milestone** | The one opened order not closed is DN, and it waits at DN-S1's gate for the operator's mark on the amended hard invariants (`raft-marks-2026-11-20.md` §28); its worktree is held by another live session. So CLA took the highest-priority order written and not opened: **DK**, the btree descent. Why first: it is on every keyed statement's path; it carries a refusal with nothing racing (the stale root), a defect a client can meet; and its S0 run is the one DM-S0's M1 shares (`workorder-dm-leaf-upper-bound.md` DM-Q1). CR's items need a contract change in `txn.md` §5, CW's protocol is wider, and DG's own proposal is "no order" (DG-Q0 (c)) |
| **Mark** | **DK is opened** on worktree `dk-descent-low-key` from `2c2639df` (`v3.5.0-12-g2c2639df`). DK-Q1..Q9 are adopted as CLA proposed, **on the operator's standing go-ahead of 2026-11-21 (go-ahead-achieving-milestone)**, each at the stage that reached it; none is the operator's own per-item mark. DK-S0 done: D1 go (DK-Q2 (a)), D2 go (DK-Q3 (a), by its refusal clause), D3 handed to CW (DK-Q1 (a)); `bench/v3.5.0/dk-s0-premise-v3.5.0-13-g187ff9b1.md` |
| **Closed** | **DK closed narrow on the work branch** at the same go-ahead: DK-S1 (D1) `7622d619`, DK-S2 (D2) `9a551ddd`, DK-S3 nothing built, DK-S4 measured (`bench/v3.5.0/results-dk-close-v3.5.0-15-g9a551ddd.md`: level with `2c2639df` in every cell; refusals not lowered). Adopted on the standing go-ahead, none the operator's own mark: DK-Q1, Q2, Q3, Q4, Q5, Q7, Q8, Q9 (a), and two items CLA added at DK-S2, **DK-Q10 `[quiet-wrong]` (a)** (no write hop from the named root) and DK-Q11 (a) (the `descent_hops` tally); DK-Q6 not reached |
| **Does not settle** | a push, a merge to `main`, a tag or a version; DN-S1's gate |
| **Recorded at** | this file; the DK order's header, §4 and §7; the DK row in `instructions/v3.2.0/index.md` |

## 3. DG opened on the standing go-ahead

| | |
|---|---|
| **Word** | *"/go-ahead-achieving-milestone DG"*: the operator named DG |
| **Mark** | **DG is opened** on worktree `dg-wal-segment-body` from `46ee8a6e` (`v3.5.0-34-g46ee8a6e`). DG-Q0..Q10 are adopted as CLA proposed, (a) in each, **on the operator's standing go-ahead of 2026-11-21 (go-ahead-achieving-milestone)**. None is the operator's own per-item mark. DG-Q5 (a) is revised against the tree: CZ's C1 is on `main` (CZ-S3, `0f4edbd5`), so both arms of DG-S0 carry C1 as it was built, with no switch |
| **Correction** | §2 read DG's own proposal as "no order" (DG-Q0 (c)). That was wrong. (c) is the alternative the order sets beside its proposal, and CLA's proposal is (a): an order on the fill, exploratory |
| **What the adoption itself stops** | DG-Q1 (a) makes V0 the operator's reading of DG-S0's table. DG-Q2 (a) gives the choice of candidate to the operator at V0, and gives P2 and P3 (`[quiet-wrong]`) a mark each. So under these adoptions DG-S1..S3 cannot proceed on the standing word, and the milestone stops at V0 |
| **Does not settle** | V0, V1, the candidate, a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; the DG order's header, §4 and §7; the DG row in `instructions/v3.2.0/index.md` |
