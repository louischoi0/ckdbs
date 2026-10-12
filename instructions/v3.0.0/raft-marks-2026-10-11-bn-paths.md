# Ratification marks — 2026-10-11, the read and update paths order

**The operator's words of 2026-10-11 on `workorder-bn-opt-read-update-path.md`**,
recorded by CLA on `worktree-bk-read-update-paths` (`v2.7.0-*`; the v3.0.0
tag is not cut). Every word is recorded **in English**, translated where it
was typed in Korean.

**Why a file of its own.** `raft-marks-2026-10-11.md` belongs to the other
orders of that day that carry the letter BN - `workorder-bk-keystone-48.md`,
written and opened on `worktree-bk-keystone-48` and not yet on `main`, and
`workorder-bo-sys-statistics-show-sugar.md`, written on `main` at `ab2c46d0`
and reaching this branch through the merge at `8679367e`. Sessions writing one marks file collide when they land;
this file holds this order's marks, as `raft-marks-2026-09-08-as-q6.md` held
one order's beside `raft-marks-2026-09-08.md`. **Cite the file, never the
bare number**: `BN-Q3` names three different items in the three orders.

## 1. A direction for the read and update paths; the order written

| | |
|---|---|
| **Word** | *"Propose a direction for optimizing the read and update paths to be faster."* (W0); *"Name the workplan BY."*, then *"No, make it BN."* (W1) |
| **Mark** | BN-S0: the order written (`workorder-bn-opt-read-update-path.md`) and reviewed. **Not opened**: BN-Q0 was unmarked. W1 names the letter, so the collision with `workorder-bk-keystone-48.md` is the operator's naming and is kept (§2's BN-Q10) |
| **Does not settle** | BN-Q0..Q9; any push |
| **Recorded at** | `workorder-bn-opt-read-update-path.md`'s header and §6 |

## 2. BN opened; every item adopted as CLA proposed

| | |
|---|---|
| **Word** | *"Take a look at workorder-bn-opt-read-update-path.md in instructions, go ahead for this milestone, keep working until this milestone is achieved, and discard a proposal if it turns out to be useless. Go ahead for this milestone, don't stop; I will follow CLA's proposal if any decision needs to be made."* (W2), with the `go-ahead-achieving-milestone` skill, whose standing word is *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session"* |
| **Mark** | **Adopted on the operator's standing go-ahead of 2026-10-11 (go-ahead-achieving-milestone), not marked per item:** BN-Q0 yes (BN opens); BN-Q1 CLA's bar; BN-Q2 (a); BN-Q3 both accepted; BN-Q4 (a) `[quiet-wrong]`; BN-Q5 whatever BN-S1 ranks first; BN-Q6 as proposed; BN-Q7 as proposed; BN-Q8 out of BN; BN-Q9 as written; **BN-Q10** (new, the letter collision) keep the letter, cite by file, marks in this file. Before adopting BN-Q4, CLA re-read its premise at `ac04b84e` - the releasers of a var-heap value (rollback of its own appends, `PURGE`'s tombstone release, recovery's undo) - and found it unchanged. **W2's "discard a proposal if it turns out to be useless"** is read as BN-R0 already reads: a stage whose part BN-S1 finds immaterial is not built, and the order records it as discarded with the number that discarded it, rather than leaving it open |
| **Does not settle** | Any decision that surfaces mid-stage: under the same word, each is written into the order with CLA's proposal and adopted, never recorded as the operator's own mark. A push, a merge to `main`, a tag or a version still waits for the operator's word |
| **Recorded at** | `workorder-bn-opt-read-update-path.md`'s header, §4's mark column and §6; `index.md`'s BN row for this order |

## 3. Decisions that surfaced mid-stage, each CLA's proposal, adopted

| | |
|---|---|
| **Word** | The same standing go-ahead as §2 (W2) |
| **Mark** | **Adopted, not marked per item:** BN-R7 (a statement's reply in one write) and BN-R8 (a var-heap append from the chain's last tail), added at BN-S1 as rulings the census ranked; BN-Q11 (the trace spans and the schema word's rate not built - the sampler replaced the first, and the second prices only a cached plan); BN-Q12 (BN-R8 in BN, as BN-S9); BN-Q13 (BN-S2 not built - parse and compile are 7-12 % of a point statement's server CPU but reachable only by a client that prepares, and 2-4 % of the statement a client waits for; BN-S3 widened to the every-row re-profile's per-row parts) `[quiet-wrong]` for the option not taken; BN-R2 revised in one point (a relation with any hook decodes whole, rather than the union of the hooks' columns). **Discarded with the number that discarded each**, as W2 asks: BN-R6 / BN-S6 (access statistics, 0.7 % of a point `SELECT`), BN-R5 / BN-S7 (byte-range records: the cost is per record, not per byte), BN-R1 / BN-S2 (BN-Q13) |
| **Does not settle** | A push, a merge to `main`, a tag or a version |
| **Recorded at** | `workorder-bn-opt-read-update-path.md` §0, §2, §3, §4 and §6 (code at `8679367e`); `index.md`'s row for this order |
