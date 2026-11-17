# Raft marks, 2026-11-17

The operator's words of 2026-11-17 that open a new matter, and what CLA
took under them.

Sections are in the order they reached this file on `main`, not the order
the words were given: §3 and §4 were given in another session, on worktree
`cu-key-set-access`, §3 before §1.

## 1. The optimistic page read, and a work order written

| | |
|---|---|
| **Word** | W1, on 2026-11-17, after a conversation on Oracle's current and consistent-read buffers, on which unit an optimistic read belongs to, and on what the tree does today: *"Then write a milestone for the optimistic page read - the measurement, the validity verdict, the implementation and so on."* (given in Korean; recorded in English) |
| **Mark** | **`workorder-cw-optimistic-page-read.md` is written and not opened.** The letter CW is CLA's proposal. The order is exploratory: its first two stages measure, and either may close it with nothing built |
| **Does not settle** | the opening of CW or of any stage; CW-Q0 to CW-Q9, each CLA's proposal only - CW-Q1 is the bar the verdicts are read against; the pending question `docs/pending/multicore-scaling-cores4-below-cores2.md`, which stays the operator's; a version |
| **Recorded at** | this file; the order's header and §7; the CW row and this file's row in `instructions/v3.2.0/index.md` |

## 2. The order pushed

| | |
|---|---|
| **Word** | W2, on 2026-11-17, while §1's order was being written: *"Once the cw-optimistic-page-read milestone document is written, push it right away, no verify."* (given in Korean; recorded in English) |
| **Mark** | The work order, this file and the index rows are merged to `main` with `--no-ff` and pushed with `--no-verify`. **Skipped: the pre-push hook, the full suite (not executed) and the `critics-developer` review (not run).** The commit changes no build input - documents under `instructions/` only |
| **Does not settle** | the opening of CW; a tag, a version |
| **Recorded at** | this file; the order's §7 |

## 3. A keyed write beside other conjuncts taken into CU as CU-S3b; the subquery case left pending

| | |
|---|---|
| **Word** | W3 (the first word of the CU session; given before §1's), on 2026-11-17, after CLA reported CU-S3 (`84bf6828`, the branch at `94f3c4a3`) and, asked, explained that a write descends only where the tuple key's conjunct is its whole `WHERE`, that the row function asks the whole clause of the row a descent finds so the gate can be widened, and that a `WHERE` holding a subquery is the one case that cannot simply follow: *"Take it into CU, leave the subquery case as pending work, and carry on with the work"* (given in Korean; recorded in English) |
| **Mark** | **CU-Q14 (a)**, the operator's own mark: an `UPDATE` or `DELETE` descends for a single tuple key equality or a tuple key list beside any other conjunct, in CU. **The subquery case is deferred**: a `WHERE` holding a subquery keeps its two walks, recorded in `docs/pending/keyed-write-beside-a-subquery.md`. **CU-S3b is opened**, on worktree `cu-key-set-access` from `94f3c4a3`; it landed on the work branch at `04d9afe9` |
| **Does not settle** | the opening of CU-S4, the close: CLA reads "the work" as the stage this word takes into CU, as every earlier word of CU named one stage, and stops after it; any option of the pending entry; a write over a bound on the tuple key or through an index or a Cabin, which CLA named as larger and separate; a merge, a push, a version |
| **Recorded at** | this file; the order's header, items table (CU-Q14), stages table and §7; `docs/pending/keyed-write-beside-a-subquery.md` and its row in `docs/pending/README.md`; the CU row and this file's row in `instructions/v3.2.0/index.md` |

## 4. CU-S4, the close, opened

| | |
|---|---|
| **Word** | W4 (the second word of the CU session), on 2026-11-17, after CLA reported CU-S3b (`04d9afe9`, the branch at `3cc59968`) and said that CU-S4 was not opened: *"Proceed with CU-S4"* (given in Korean; recorded in English) |
| **Mark** | **CU-S4 is opened**, on worktree `cu-key-set-access`: the overhead A/B over the whole change (CU-R5), CU-S0's arms measured again, the known gaps, the report. No item is marked; none was open |
| **Does not settle** | a merge to `main`, a push, a version: the order's stages table ends CU-S4 in "the operator's word on landing", and CLA stops at the report; any option of `docs/pending/keyed-write-beside-a-subquery.md` |
| **Recorded at** | this file; the order's header and §7; the CU row and this file's row in `instructions/v3.2.0/index.md` |

## 5. CU landed

| | |
|---|---|
| **Word** | W5 (the third word of the CU session), on 2026-11-17, after CLA reported CU-S4 (`bc3cb6ba`, the branch at `e5a85e24` with `origin/main` `f345652f` merged in): *"Merge it into main and push, no verify"* (given in Korean; recorded in English) |
| **Mark** | Branch `worktree-cu-key-set-access` is merged to `main` with `--no-ff` and pushed with `--no-verify`. **Skipped: the pre-push hook.** The full suite is not skipped: it ran green on the merged tree at `70a307ea`, 4565 of 4565 (`ctest -LE bs-nightly`, Debug), and every commit after it holds drivers under `tools/`, `bench/` and documents only. The stage's `critics-developer` review ran (the order's §7) |
| **Does not settle** | a tag, a version; any option of `docs/pending/keyed-write-beside-a-subquery.md`; what the close did not measure (`docs/inflight/known-gaps.md`, "Key sets (CU)") |
| **Recorded at** | this file; the order's header and §7; the CU row and this file's row in `instructions/v3.2.0/index.md` |
