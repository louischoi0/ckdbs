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

## 4. CF's branch marked O; pushed with the hook skipped

| | |
|---|---|
| **Word** | W3: *"Proceed with $!O and push no verify"* - after CLA's report of CF-S0 (`bench/v3.3.0/results-cf-s0-premise-v3.3.0-97-g3d628df4.md`): CF-Q1 (a) not met, replay paying only on a scan-driven chain with many tuple-key probes, the default costing point reads and short chains 2.7-3.9 % of server CPU; CLA proposed O, with D as its code-free half |
| **Mark** | **Branch O** (CF-Q0 (a)): a per-pattern opt-in that is not a second model of a pattern (the order's §2.3). Per §5, CF-S1 for O is written as an amendment to the order first (CF-Q7), then built. **Pushed to `main` with `--no-verify`** on this word: the work branch at `6c62aee6` differed from `origin/main` in `bench/` and `instructions/` only, and no suite ran on it |
| **Does not settle** | CF-Q7's answers (the amendment's questions); CF-Q6; whether the default turns off as part of O; a tag, a version |
| **Recorded at** | this file; the order's header and §7; the order's row in `instructions/v3.2.0/index.md` |

## 5. CG written, as an exploratory concept check; pushed, documents only

| | |
|---|---|
| **Word** | W0: *"From the point of view of production-level DB software, is it fatal that a pk-update (delete-purge-insert) cannot be expressed as one transaction?"* - CLA answered that K1's never-rebound tuple key is within common practice, that a delete and re-insert of one key inside one transaction is the pattern that bites, and that `ON CONFLICT DO UPDATE` would express most of it without touching K1. W1: *"Record ON CONFLICT DO UPDATE in docs/pending."* W2: *"Not as a pending issue - add it as an exploratory concept validation milestone and push it, documents only."* |
| **Mark** | **CG written** (`workorder-cg-on-conflict-do-update.md`) on `worktree-pending-on-conflict-do-update` from `722006ec` (`v3.3.0-104-g722006ec`), **exploratory**: CG-S0 checks whether an upsert fits as one more arm of the existing write paths (CG-R1..R4) and ends in build, narrow or drop. The `docs/pending/` entry W1 asked for was written on the work branch and is withdrawn by W2 before it reached `main`; the order carries its content. **Pushed on W2**, documents only, with the pre-push hook skipped |
| **Does not settle** | CG's opening; CG-Q0..Q7, each with CLA's proposal beside it and none marked; a tag, a version |
| **Recorded at** | this file; the order's header; this file's and the order's rows in `instructions/v3.2.0/index.md` |

## 6. CH written, as an exploratory concept check; pushed, documents only

| | |
|---|---|
| **Word** | W0: *"So in the end, is the milestone about whether a pk update can be written as one transaction?"* - of CG. CLA answered no: CG keeps the key (CG-R2), and a key change inside one transaction makes a new exception to K1 and K2, so CLA offered (a) widening CG or (b) a separate exploratory milestone, and proposed (b). W1: *"$!b - write it as a separate milestone and push it, documents only."* |
| **Mark** | **(b). CH written** (`workorder-ch-tuple-key-update.md`) on `worktree-ch-tuple-key-update` from `9fdf3351` (`v3.4.0`), **exploratory**: CH-S0 checks three classes of target key - C1 never bound (K2 amended only), C2 delete-marked (a new K1 exception), C3 a permutation of live keys - and ends each in build, narrow or drop (CH-R1..R4). CG is unchanged. **Pushed on W1**, documents only, with the pre-push hook skipped |
| **Does not settle** | CH's opening; CH-Q0..Q7, each with CLA's proposal beside it and none marked; any wording of K1, K2 or invariant 11; a tag, a version |
| **Recorded at** | this file; the order's header; this file's and the order's rows in `instructions/v3.2.0/index.md` |

## 7. Version 3.4.0 tagged

| | |
|---|---|
| **Word** | W4 (CF's session): *"and tag 3.4.0"* - directly after CF's W3 (§4) |
| **Mark** | **`v3.4.0`**, an annotated tag on `main` at `9fdf3351`, then the head of `origin/main`. Its message states what has been built since `v3.3.0` (CD, BO, CE, A1 stage 2a, BS S1-S8, the defect fixes), what was measured, and the gaps that bound it. The tagged build inputs are identical to `5f1d1307`, where the full Debug suite ran 4238/4238, so the tag was pushed with `--no-verify`. CLAUDE.md's version line moved to `v3.4.0` (`5404a95b`, pushed with `--no-verify`) |
| **Does not settle** | a later version; CF-O-Q1..Q14 |
| **Recorded at** | this file; the tag message; CLAUDE.md's Version Management |

## 8. CF-O-Q1..Q14 adopted on the go-ahead; pushed with the hook skipped

| | |
|---|---|
| **Word** | W5 (CF's session): *"push no verify and go ahead"* - after CLA's report of CF-S1a, Branch O's amendment (the order's §8), which put CF-O-Q1..Q14 with CLA's proposal beside each, every one (a) |
| **Mark** | **CF-O-Q1..Q14 adopted as CLA proposed, each (a), on this go-ahead**: not marked by the operator item by item, and recorded as adopted. CF-O-Q3's word (`WAYSTONE`) is taken as CLA proposed it. **The amendment was pushed to `main` with `--no-verify`** (documents only, no suite run on it). **CF-S1b started** on the same word |
| **Does not settle** | CF-Q6; a push of code; a tag, a version |
| **Recorded at** | this file; the order's §7 and §8.4; the order's row in `instructions/v3.2.0/index.md` |

## 9. CF closed; pushed with the hook skipped

| | |
|---|---|
| **Word** | W6 (CF's session): *"push no verify and close the milestone"* - after CLA's report that CF-S1b, S1c (with the instance-words fold), S1d and S2 were built and measured |
| **Mark** | **CF closed** (`workorder-cf-waystone-premise.md`): Branch O built - S1b `4b46acd5`, S1c `b0e6db12`, the fold `46c632aa`, S1d `46ca0729`, S2 `8eb4a898` (`bench/v3.4.0/results-cf-s2-close-v3.4.0-9-g46c632aa.md`); CF-O-Q15 settled by `catalog.md` CT6's threshold. The full Debug suite ran 4263/4263 at `27f80aa6`, the last commit that changed a build input; the commits after it are documents only. **Pushed to `main` with `--no-verify`** on this word |
| **Does not settle** | CF-Q6 (CLAUDE.md's "What This Project Is" item 2), which stays the operator's; the refill and mount-cost measurements the close cut; a tag, a version |
| **Recorded at** | this file; the order's header and §7; the order's row in `instructions/v3.2.0/index.md` |

## 10. CF-Q6 settled (b): the project's stated purpose; pushed with the hook skipped

| | |
|---|---|
| **Word** | W7 (CF's session): *"$!b push no verify"* - after CLA laid out CF-Q6. The options were (a) correct the two mechanisms in place, (b) restate the purpose as declared, engine-enforced correctness with advisory acceleration second, and (c) drop the two-mechanism paragraph. CLA proposed (b) |
| **Mark** | **CF-Q6 (b).** CLAUDE.md's "What This Project Is" now reads: fast and correct OLTP, where correctness is declared and the engine enforces it - (1) declared invariants enforced on every write (assertions, transitions, foreign keys, unique indexes, Keystone identity); (2) acceleration that can be dropped without changing a result (Cabin value sets, authoritative for the values they observed; Waystone trails for opted shapes, advisory). The wording of (2) was corrected at landing from "advisory acceleration": `cabin.md` C1 makes a Cabin authoritative for its observed values. It replaces engine-driven physical page optimization (no tuple mover since BZ) and Waystone as a default (opt-in since CF). `README.md` and `overview.md` are brought to it and to the built engine. **Pushed to `main` with `--no-verify`**, documents only |
| **Does not settle** | `physical-optimizer.md`'s title and scope; a tag, a version |
| **Recorded at** | this file; CLAUDE.md; the CF order's header and §7; `instructions/v3.2.0/index.md` |

## 11. BL resumed; ruling 2 re-measured before it is marked

| | |
|---|---|
| **Word** | W13 (BL's session): *"go bl"*. Asked how BL should resume with ruling 2 unmarked, the operator chose "Re-measure first": re-run the mixed cell on current `main` within the stage budget, then put ruling 2 back with current numbers |
| **Mark** | **BL resumed** (`instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md` §6, "BL resumed 2026-10-21"). C1 re-measured at `v3.4.0-22-g418670a3` (`bench/v3.4.0/results-bl-ruling2-remeasure-v3.4.0-22-g418670a3.md`); ruling 2 is recast in the order's §6 and goes back unmarked |
| **Does not settle** | BL-S4's ruling 2, which goes back to the operator; BL-S4; BL's close and its overhead A/B; a push |
| **Recorded at** | this file; the BL order's §6; `docs/inflight/known-gaps.md`'s BL section; the BL row in `instructions/v3.0.0/index.md`; CLAUDE.md's Cross-core execution row |

## 12. BL-S4's ruling 2 marked (a): core 0 is the first batch core; pushed with the hook skipped

| | |
|---|---|
| **Word** | W14 (BL's session): *"$!a push no verify"*, after CLA put ruling 2 back recast (§11): (a) "core 0 first", (b) core 0 kept free of interactive sessions, or more runs |
| **Mark** | **Ruling 2 (a).** While the partition is active, core 0 is the first batch core. BL-R2's partition, BL-S4's cell and BL-Q19 in `instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md` are amended to it; BL-S4 is unblocked. **Pushed to `main` with `--no-verify`**, documents and bench only, with no suite run |
| **Does not settle** | BL-S4's start; BL's close and its overhead A/B; a tag, a version |
| **Recorded at** | this file; the BL order's BL-R2, BL-S4, BL-Q19 and §6; the BL row in `instructions/v3.0.0/index.md`; `docs/inflight/known-gaps.md`'s BL section; CLAUDE.md's Cross-core execution row |

## 13. BL-S4 started and built

| | |
|---|---|
| **Word** | W15 (BL's session): *"go bl-s4"*, after ruling 2 (a) unblocked it (§12) |
| **Mark** | **BL-S4 built** on `worktree-bl-s4-partition` (`66d8e533`, review `402317a2`): `SET PRIORITY`, the partition with core 0 first, the per-level slice, the fallback accept passing over batch cores; BL-Q10 (a) adopted under W10's latitude; the eight-connection cell cut (the BL order's §6, "BL-S4 built 2026-10-22") |
| **Does not settle** | BL-S5 (the close, its overhead A/B and texts); a push; a tag, a version |
| **Recorded at** | this file; the BL order's §6; `docs/inflight/known-gaps.md`'s BL section; the BL row in `instructions/v3.0.0/index.md`; CLAUDE.md's Cross-core execution row; `docs/rules/rules.md` §3 |

## 14. BL-S4 pushed with the hook skipped

| | |
|---|---|
| **Word** | W16 (BL's session): *"push no verify"* |
| **Mark** | **BL-S4 pushed to `main`** at `f4bad273` with `--no-verify`; the full suite had run 4295/4295 on that tree |
| **Does not settle** | BL-S5; BL's close; a tag, a version |
| **Recorded at** | this file |

## 15. BL-S5 started and measured; BL's close is the operator's

| | |
|---|---|
| **Word** | W17 (BL's session): *"go bl-s5"* |
| **Mark** | **BL-S5 measured** at `v3.4.1-10-gf4bad273` (`bench/v3.4.0/results-bl-close-v3.4.1-10-gf4bad273.md`) and its texts written (the BL order's §6, "BL-S5 measured 2026-10-22") |
| **Does not settle** | **BL's close**, which is the operator's word; BL-Q2, BL-Q7 and BL-Q10..Q15, unmarked - BL-Q10 (a) and BL-Q11 (a) are what CLA built and wrote under W10's latitude, and their marks are owed; a push; a tag, a version |
| **Recorded at** | this file; the BL order's §4 (BL-Q10, BL-Q11) and §6; the BL row in `instructions/v3.0.0/index.md`; CLAUDE.md's Cross-core execution row |

## 16. BL-S5 pushed with the hook skipped

| | |
|---|---|
| **Word** | W17b (BL's session): *"push no verify"* |
| **Mark** | **BL-S5 pushed to `main`** at `04aa27e7` with `--no-verify`; no build input had changed since the suite's 4295/4295 at `f4bad273` |
| **Does not settle** | BL's close; a tag, a version |
| **Recorded at** | this file |

## 17. BL closed

| | |
|---|---|
| **Word** | W18 (BL's session): *"close bl"* |
| **Mark** | **BL closed** (`instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`, header and §6 "BL closed 2026-10-22"): every stage built and measured, §5.2's bar (v) not met and recorded as BL-R7's remainder |
| **Does not settle** | BL-Q2, BL-Q7, BL-Q10..Q15, unmarked (BL-Q10 (a) and BL-Q11 (a) as CLA built them); the checkpoint's hold on a reader core during a roll, which is WAL work; a push; a tag, a version |
| **Recorded at** | this file; the BL order's header and §6; the BL row in `instructions/v3.0.0/index.md`; `docs/inflight/known-gaps.md`'s BL section; CLAUDE.md's Cross-core execution row |
