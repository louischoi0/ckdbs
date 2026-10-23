# The operator's words of 2026-10-23

## 1. CG opened, CG-Q0..Q7 marked as proposed, S0 run

| | |
|---|---|
| **Word** | W3 (CG's session): *"go cg"*. Asked which marks to open CG with, since CG-S0 needs CG-Q1..Q4 marked (`workorder-cg-on-conflict-do-update.md` §3), the operator chose W4: *"$!a: adopt all"* |
| **Mark** | **CG opened.** CG-Q0 (a), Q1 (c), Q2 (a), Q3 (a), Q4 (a), Q5 (a), Q6 (a), Q7 (a), each as CLA proposed. **CG-S0 ran** on `cg-on-conflict-do-update` from `4fb01b46`, documents only; its verdict, **narrow**, is in the order's §7, with CLA's proposal to re-mark CG-Q1 to (b) and to drop CG-Q3 (b) from CG |
| **Does not settle** | Build, narrow or drop after S0 (CG-Q0 (a): the operator's); CG-Q1's re-mark; CG-S1; a push |
| **Recorded at** | this file; the CG order's header, §4 and §7; the CG row in `instructions/v3.2.0/index.md` |

## 2. CG narrowed: CG-Q1 re-marked (b), CG-Q3 (b) dropped; pushed with the hook skipped

| | |
|---|---|
| **Word** | W5 (CG's session): *"$!narrow push no verify"*, after CLA reported CG-S0's verdict and proposed narrow: build the update arm, re-mark CG-Q1 to (b), drop CG-Q3 (b) |
| **Mark** | **Narrow, as proposed.** CG-Q1 re-marked from (c) to (b): CG-S1 builds the tuple key and one named unique index as arbiters, S2 folded in. CG-Q3 (b) leaves CG. **Pushed to `main` with `--no-verify`**, documents only; no suite was run for it and no build input changed |
| **Does not settle** | CG-S1's start; a tag, a version |
| **Recorded at** | this file; the CG order's header, CG-Q1 and §7 "Narrowed 2026-10-23"; the CG row in `instructions/v3.2.0/index.md` |

## 3. Version 3.4.3 tagged

| | |
|---|---|
| **Word** | *"tag 3.4.3"* |
| **Mark** | **`v3.4.3`**, an annotated tag on `main` at `f44846ef`, then the head of `origin/main`. Its message states the map write order fixed (`4fb01b46`), BL closed, CG opened and narrowed, and the gaps that bound it. The full Debug suite ran 4311/4311 on `4fb01b46`, whose build inputs the tagged commit shares (everything after it is under `instructions/`), so the tag was pushed with `--no-verify`. CLAUDE.md's version line moved to `v3.4.3` |
| **Does not settle** | a later version |
| **Recorded at** | this file; the tag message; CLAUDE.md's Version Management |
