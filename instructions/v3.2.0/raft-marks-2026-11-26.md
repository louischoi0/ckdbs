# Raft marks - 2026-11-26

The operator's words of 2026-11-26, recorded in English.

**§1 and §2 of this day are not in this tree.** They were recorded on
`worktree-ds-known-gaps-contained` (a test fixed for OpenSSL 4 and pushed;
DS written and opened), which had not landed on `main` when this section
was written on `worktree-known-gaps-milestone-orders` from `8274c951`.
Whichever branch lands second joins the two files; the section numbers
are kept.

## 3. Twelve orders written from the known gaps, DT through EE; none opened

| | |
|---|---|
| **Word** | *"From the known gaps, pick out the large pieces of work worth registering as milestones"*, then, to CLA's list of twelve candidates, *"write a milestone work order for each item, one by one"* |
| **Mark** | **Twelve orders are written and none is opened**, on worktree `known-gaps-milestone-orders` (branch `worktree-known-gaps-milestone-orders`) from `8274c951` (`v3.4.17-148-g8274c951e`), documents only: **DT** (a write that gives its core back), **DU** (a keyed write), **DV** (unreachable pages reclaimed), **DW** (a btree's purge driver), **DX** (the pool under pressure), **DY** (the segment roll off the latch), **DZ** (the checkpoint's reactor residue), **EA** (a prepared statement parsed once), **EB** (the key forms in the executor), **EC** (a multi-core crash harness), **ED** (backup, a second pass), **EE** (the decide slot). **The letters are CLA's**, the next free after DS, which the other session of this day holds. **The selection and each order's scope are CLA's reading of `known-gaps.md`**, not the operator's per-item choice. **No item of any order is marked**: every `-Q` row carries CLA's proposal and waits. Candidate 7 of the list was restated when written - from the structure latch's per-fetch holds, which CW priced and abandoned, to what a checkpoint still does on its reactor (DZ §0) |
| **Does not settle** | whether any of the twelve is opened, or in what order; any `-Q` item; DY-Q0, which BQ-Q5 (b) reserves to the operator's own word; the two questions EA would reopen (BN-Q13, CX-Q37 (a)); the pending entry DU leaves pending (`docs/pending/keyed-write-beside-a-subquery.md`); K1's second exception (DW-Q4 (c)); a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; each order's header; their rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 4. DT opened and worked on a standing go-ahead; its push authorized ahead of its close

| | |
|---|---|
| **Word** | Four, in order, each to CLA in the session that had the twelve orders open. *"Reflect the worktree the milestones were written in into main."* Then *"push main."* Then *"In two hours, start the DT work with /go-ahead-achieving-milestone"*, and with it *"and when the work is done, commit and push at once."* The skill the third names carries the operator's standing word: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* |
| **Mark** | **The twelve orders are on `main`**: `worktree-known-gaps-milestone-orders` (`e2b6a313f`) merged at `60dc593af` and pushed, the hook skipped for a push of documents only, no suite run. **DT is opened** on worktree `dt-write-path-yield` from `60dc593af` and worked stage after stage in one session. **Every DT-Q item is CLA's proposal adopted on that standing go-ahead**, recorded as such in the order's §4 "Marks" and never as the operator's mark on the item; DT-Q1 was adopted as CLA revised it at DT-S0 (the bar read per mechanism). **The push of `main` at DT's close is authorized ahead of it**, by the fourth sentence: the Session Workflow's step 4 runs without a second word, the hook skipped only under the standing rule |
| **Does not settle** | a tag or a version; a force push; a push with the suite skipped; a push if DT does not close - a failing suite, a failed review or a measured regression stops it; any milestone but DT; the two questions DT-S0's measurement raised and handed to `known-gaps.md` |
| **Recorded at** | this file; `workorder-dt-write-path-yield.md` (header, §4 "Marks", §7); DT's rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |
