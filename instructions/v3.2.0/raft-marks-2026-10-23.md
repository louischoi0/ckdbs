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

## 4. CI written: an online index build

| | |
|---|---|
| **Word** | W0 (CI's session): *"index build online, write milestone"*; W1: *"push it, documents only, no verify"*; W2, after the review was applied: the same words |
| **Mark** | **CI written**, not opened: `workorder-ci-online-index-build.md`, on `worktree-online-index-build` from `2f7ce828`. `CREATE INDEX` announces its row flagged building, drains writers on the older list, backfills with writers running and logging, then clears the flag; the relation is held `IS`, not `X`, across the walk. CI-Q0..Q10 carry CLA's proposals, none marked. **Pushed to `main` with `--no-verify`** on W1, documents only; no suite was run for it and no build input changed. The opening `critics-developer` review was still running at that push; its findings, checked by independent refuters, were applied and **pushed the same way on W2** (CI-R6 added, CI-Q3 proposed (b)) |
| **Does not settle** | CI's opening; every CI-Q |
| **Recorded at** | this file; the CI order; the CI row in `instructions/v3.2.0/index.md` |

## 5. CG-S1 started and built

| | |
|---|---|
| **Word** | W6 (CG's session): *"go cg-s1"* |
| **Mark** | **CG-S1 built** on `cg-on-conflict-do-update` (the CG order's §7, "CG-S1 built 2026-10-23"): `ON CONFLICT DO UPDATE` on the tuple key and a unique index's column, `DO NOTHING` on the latter too |
| **Does not settle** | CG-S3, the close and its measurement; a push; a tag, a version |
| **Recorded at** | this file; the CG order's §7; the CG row in `instructions/v3.2.0/index.md` |

## 6. Version 3.4.4 tagged

| | |
|---|---|
| **Word** | *"tag 3.4.4"* |
| **Mark** | **`v3.4.4`**, an annotated tag on `main` at `5840ee56`, then the head of `origin/main`. Its message states CG-S1 built, the WAL writer's kick fixed (`5840ee56`), CI written, and the gaps that bound it. The full Debug suite ran 4320/4320 on the tree of the tagged commit, so the tag was pushed with `--no-verify`. CLAUDE.md's version line moved to `v3.4.4` |
| **Does not settle** | a later version |
| **Recorded at** | this file; the tag message; CLAUDE.md's Version Management |

## 7. CG-S1 pushed; CG-S3 measured

| | |
|---|---|
| **Word** | W6b (CG's session): *"push no verify"*, then W7: *"go cg-s3"* |
| **Mark** | **CG-S1 pushed** to `main` at `6775aec1` with `--no-verify` (the full suite 4319/4319 at `54dbc716`, whose build inputs it shares). **CG-S3 measured** (the CG order's §7, "CG-S3 measured 2026-10-23") |
| **Does not settle** | **CG's close**, which is the operator's word; a push; a tag, a version |
| **Recorded at** | this file; the CG order's §7; the CG row in `instructions/v3.2.0/index.md` |
