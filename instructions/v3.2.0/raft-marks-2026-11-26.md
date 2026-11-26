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

## 4. DU opened on the standing go-ahead, and its push authorized in advance

| | |
|---|---|
| **Word** | *"In 4 hours, work the DU milestone with /go-ahead-achieving-milestone, and when it is done commit and push right away"* (given in Korean at about 14:58 UTC; recorded in English). The session scheduled the work and ran it at 18:58 UTC |
| **Mark** | **DU is opened** on worktree `du-keyed-write` (branch `worktree-du-keyed-write`) from `60dc593af` (`v3.4.17-150-g60dc593af`). **DU-Q0..Q6 are adopted as CLA proposed, (a) in each, on the operator's standing go-ahead of 2026-11-26 (go-ahead-achieving-milestone)**, each recorded in the order's §4; none is the operator's own per-item mark. Two are adopted with a correction CLA made against the tree at DU-S0: **DU-Q2 (a)** - a collection past `max_locks_per_txn` is not written from and the statement is answered by its walk, because a refusal on the size of a superset would refuse a statement the walk answers (DU-R4); **DU-Q3 (a)** - its premise does not arise: a write that bounds no tuple key declares nothing, so the index-driven write takes the walk's row units and no unit is designed. **DU-Q7 (a)**, an item CLA added at DU-S2 from its review, is adopted on the same go-ahead: on a heap a keyed write meets its rows in key order where its walk met them in page order, so a statement whose rows are judged against each other may change between an answer and a refusal; accepted and stated, which narrows DU-R4 on a heap. **The push at the close is authorized by the same word**: when DU closes, the branch is committed, merged to `main` and pushed without the Session Workflow's step 4 stop. **CLA's reading of "push"**: `git merge origin/main` on the work branch, `git merge --no-ff` on `main`, `git push origin main`; the hook and the suite are not waived |
| **Does not settle** | a tag, a tag's push or a version; DU-Q5's pending entry (`docs/pending/keyed-write-beside-a-subquery.md`), which stays pending; any other order of §3; how DU and DT are joined - `worktree-dt-write-path-yield` (another session's, not on `main` at `60dc593af`) edits the same walk, and whichever lands second merges the two |
| **Recorded at** | this file; `workorder-du-keyed-write.md`'s header, §4 and §7; its rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |
