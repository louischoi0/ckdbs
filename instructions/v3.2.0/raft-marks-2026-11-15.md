# Raft marks, 2026-11-15

The operator's words of 2026-11-15 that open a new matter, and what CLA
took under them.

## 1. F4 ruled, and taken up

| | |
|---|---|
| **Word** | W1, on 2026-11-15, after CLA listed F4's facts and its six decisions: *"`$!a`, land the audit branch first and proceed"* (given in Korean; recorded in English) |
| **Mark** | **F4 (a)**, the operator's own mark: an `UPDATE` or `DELETE` whose WHERE fails on the page's version of a row whose writer its view cannot see resolves the snapshot's version through the undo chain and tests it; a match goes on to `txn.md` §5's verdict table, which waits on or refuses that writer. **The audit branch first:** found already landed - `worktree-audit-lean-and-correct` merged to `main` as `27c6ebad` on 2026-11-14, wholly contained in `origin/main` `8d3adccc` - so F4 is built on worktree `audit-f4-where-on-snapshot` from `8d3adccc` |
| **Does not settle** | CR-Q1..Q8; the residues `txn.md` §5 names; a merge, a push, a version |
| **Recorded at** | this file; `docs/spec/txn.md` §5 ("Which version selects a row"); `manual/sql/sql.md`; the pending entry's F4; this file's row in `instructions/v3.2.0/index.md` |

## 2. What CLA took on the word "proceed"

Each is **CLA's proposal from the same list, taken on W1's "proceed"**,
not the operator's own per-item mark, and each is open to the operator
before the merge.

| Item | Options | Taken, and why |
|---|---|---|
| **F4-2** - a head committed after the view that fails the WHERE while the snapshot's version passes | (a) skip under `READ COMMITTED`, refuse under `REPEATABLE READ`; (b) refuse at both levels, as F4 (a) read literally does | **(a).** Under `READ COMMITTED` the page's version is the newest committed state, and skipping it is what PostgreSQL's re-check of the newest version answers; (b) would turn an answer that is right today into a `TxnConflict` until CR re-runs it. Under `REPEATABLE READ` the statement's state says the row matches, so a newer version is first-updater-wins' refusal |
| **F4-3** - F4 against CR | (a) F4 first, on its own, so that CR-S0's census reads a tree with F4 in it; (b) fold F4 into CR | **(a)** |
| **F4-4** - what form the work takes | (a) a work order of its own; (b) a fix on a worktree, the pending entry carrying the claim, as G1-G15 were taken | **(b)**: the pending entry carries the claim, the scenario and the fix, and one stage needs no order |
| **F4-6** - a measurement | (a) one contended cell at the close, interleaved A/B in `build-release`; (b) none | **(a)**: the added cost falls only on a row whose current writer is invisible and whose page version fails the WHERE - one undo read and one decode |
