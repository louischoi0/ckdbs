# Raft marks, 2026-11-18

The operator's words of 2026-11-18 that open a new matter, and what CLA
took under them.

## 1. The plan contract kept, plans managed by bindings, and a work order written

| | |
|---|---|
| **Context** | A conversation on commercial optimizers (Oracle, SQL Server, PostgreSQL, MySQL, DB2), CLA's six introduction priorities for this engine - (1) rule-based rewrites, (2) access path selection, (3) data distribution statistics, (4) join method selection, (5) the plan cache, (6) the join order search - and whether to keep "the query is the plan". In that conversation CLA stated, without reading the tree, that the contract held today. It does not: CB-S2 and CC-S3/S4 made the planned order the default (`workorder-cx-managed-plans.md` §1.1) |
| **Word** | W0: *"Keep the 'the query is the plan' contract for now. But go in the direction of letting the user set and manage the plan directly according to bind variables. In any case, the database itself implicitly changing a query's plan is not allowed. Down to the low level, the plan becomes something a DBA or an AI agent manages, like the statistics model."* W1: *"Write the milestone."* (given in Korean; recorded in English) |
| **Mark** | **`workorder-cx-managed-plans.md` is written and not opened.** The letter CX is CLA's proposal. CX-Q0 puts the keep-versus-restore difference to the operator first |
| **Does not settle** | the opening of CX or of any stage; CX-Q0 to CX-Q11, each CLA's proposal only; the setting `join_order`, which stays `planned` on `main` until S0 lands; a version |
| **Recorded at** | this file; the order's header; the CX row and this file's row in `instructions/v3.2.0/index.md` |

## 2. The six priorities in the order

| | |
|---|---|
| **Word** | W2, while §1's order was being written: *"Put introduction priorities 1-6 into the milestone."* (given in Korean; recorded in English) |
| **Mark** | Priorities 1-6 are CX-S2 to CX-S7, behind the two foundation stages S0 (the contract restored) and S1 (the plan as a catalog object with argument-value variants). Priority 6, the join order search, is an advisor under W0 (CX-S7, CX-R5) |
| **Does not settle** | the order in which stages open |
| **Recorded at** | this file; the order's §3 |

## 3. Implicit plan change made an option; the first plan fixed when it is off; an administrator's plan always admitted

| | |
|---|---|
| **Context** | After CLA's account of where "the query is the plan" broke (CB-S2 at `0640941f`; CC-S3/S4), why (scenario 3's `join-no-literal` built its 10,000-row inner side; scenario 1's `model-join` probed one key 359 times), how the planner works and what it reaches |
| **Word** | W3: *"For now, whether to allow implicit plan regressions or changes - plan changes made at the database level, driven by statistics according to the cost model and its safeguards - must be a database option the user chooses."* W4, given during the same turn: *"And if this flag is off, the first plan is fixed, and after that a DBA or an agent must be able to choose the plan per query. An administrator's plan choice is always possible, whatever this option is."* W5, the operator's answers to CLA's three questions, each CLA's recommendation: the first plan is **the plan the cost model chose at the shape's first compile**, captured and kept; the option is **a boot key and an instance-wide administrator statement at run time**, re-scoping `join_order` rather than adding a second name; the default is **off - the first plan fixed** (all given in Korean; recorded in English) |
| **Mark** | **CX amended** (`workorder-cx-managed-plans.md`, header, §0, §1.1, §2, §3 S0-S1 and S5-S7, §4-§8; S0 is renamed "the option and the capture", superseding §2's "the contract restored"). W3 replaces W0's third clause as an absolute rule with a choice: `join_order = planned` admits statistics-driven plan change (CB/CC as built), `fixed` (the default) captures each shape's first cost-model plan and never moves it, `written` keeps the written order. W4 makes an administrator's plan the first source under every value. CX-Q0 and CX-Q1 are settled by W3-W5 and recorded as such; CX-Q2 and CX-Q3 are re-scoped; CX-Q12..Q18 are new and unmarked |
| **Does not settle** | the opening of CX or of any stage; CX-Q2..Q18; `join_order`'s value names, which are CLA's proposal; a push; a version |
| **Recorded at** | this file; the order's header, §2 and §4; the CX row and this file's row in `instructions/v3.2.0/index.md` |

## 4. CX-Q12..Q18 marked (a); pushed with the hook skipped

| | |
|---|---|
| **Word** | W6: *"CX-Q12..Q18 $!a push no verify"* - after CLA's report of §3's amendment at `27a286e1` on worktree `cx-plan-change-option` |
| **Mark** | **CX-Q12..Q18 (a)**, each CLA's proposal: a capture holds the order and the build side, the access arms staying the ladder's (Q12); one capture per shape, serving every binding (Q13); a restart before a capture is persisted re-captures, the window bounded and counted (Q14); a run-time `SET JOIN_ORDER` is memory-resident (Q15); a captured plan that no longer compiles runs the written order, marked `INVALID`, never re-captured by the engine (Q16); only `DROP PLAN` re-arms a capture (Q17); switching values deletes no captured row (Q18). **Pushed to `main` with `--no-verify`** on this word, documents only: no suite ran, and the `critics-developer` review of the amendment had not reported when the push went out |
| **Does not settle** | the opening of CX or of any stage; CX-Q2..Q11; `join_order`'s value names; a version |
| **Recorded at** | this file; the order's header, §4 and §8; this file's row in `instructions/v3.2.0/index.md` |

## 5. Statistics taken into a new order, CY; the re-gather question deferred; the review of §3's amendment applied

| | |
|---|---|
| **Context** | CLA's list of five gaps in the engine's statistics against commercial systems: (1) no column statistics - NDV, NULL fraction, most-common values, histogram - so selectivity is three constants; (2) no persistence, so a mount or a restore plans data-free until the first publish, which the `fixed` default turns into a fixed plan; (3) no multi-column correlation or functional dependency; (4) no rule for when to gather again; (5) no management surface - explicit collection, lock, export and import, history and restore, pending statistics |
| **Word** | W7: *"From the list below, leaving item 4 out for now, bring the statistics features into the milestone or create a new one. Item 4 needs more thought."* (given in Korean; recorded in English) |
| **Mark** | **`workorder-cy-managed-statistics.md` is written and not opened**, a new order by CLA's choice between the two W7 offered: S0 the premise, S1 persistence (item 2), S2 column statistics (item 1), S3 declared column groups (item 3), S4 the management surface (item 5). CX-S4 points to it. **Item 4 is recorded as pending** (`docs/pending/statistics-staleness-and-regather.md`), and CY gathers nothing again on its own until it is ruled on (CY-R3). **The `critics-developer` review of §3's amendment**, which reported after §4's push, is applied to the CX order in the same change: seventeen wording corrections, and its design findings as CX-Q19..Q27, unmarked - the largest being that a `fixed` first compile before any statistic is published fixes a data-free plan (CX-Q19), which CY-S1 is proposed to gate (CY-Q0) |
| **Does not settle** | the opening of CY or of any stage; CY-Q0..Q13 and CX-Q19..Q27, each CLA's proposal only; item 4; a push; a version |
| **Recorded at** | this file; CY's header; the CX order's §3 S4, §4 and §8; `docs/pending/README.md`; the CX, CY and this file's rows in `instructions/v3.2.0/index.md` |

## 6. CY-Q0 and CY-Q1 marked (a); pushed with the hook skipped

| | |
|---|---|
| **Word** | W8: *"CY-Q0, CY-Q1 $!a push no verify"* - after CLA's report of §5 at `fb65d7bd` on worktree `cx-plan-change-option` |
| **Mark** | **CY-Q0 (a)**: CY-S1 (persistence) is CX-S0's progress gate - `fixed` does not become the default until relation statistics survive a restart. **CY-Q1 (a)**: CY-S2 and CY-S3 are built only if CY-S0 finds at least one scenario statement whose order changes under true selectivities and runs 15 % or more faster at p50; S1 and S4's lock, export and import do not wait for it. **Pushed to `main` with `--no-verify`** on this word, documents only: no suite ran, and the `critics-developer` review of CY had not reported when the push went out |
| **Does not settle** | the opening of CY, of CX or of any stage; CY-Q2..Q13; CX-Q2..Q11 and CX-Q19..Q27; item 4 (`docs/pending/statistics-staleness-and-regather.md`); a version |
| **Recorded at** | this file; CY's header, §4 and §8; the CX order's §3 S4; the CY row and this file's row in `instructions/v3.2.0/index.md` |

## 7. CY-Q2..Q13 marked (a); CY opened, CY-S0 started

| | |
|---|---|
| **Word** | W9: *"CY-Q2..Q13 $!a, proceed with CY-S0"* - after CLA's report of §6's push at `be205b83` |
| **Mark** | **CY-Q2..Q13 (a)**, each CLA's proposal: the steward saves a relation prior when its band moves, beside `GATHER` (Q2); the newer of a live prior and a persisted object is published, a locked one always winning (Q3); a whole walk by default, `SAMPLE n PERCENT` by leaf (Q4); a most-common list and an equi-depth histogram, at most 100 each (Q5); no histogram for `char`/`varchar` (Q6); a view per slice (Q7); a `GATHER` publishes at once unless `PENDING` (Q8); a line-oriented export document (Q9); `card(T)` not persisted (Q10); one immutable snapshot with a byte ceiling (Q11); the last 8 versions kept (Q12); no statistics on an expression (Q13). **CY opened; CY-S0 started** on worktree `cx-plan-change-option` (a worktree-isolated session cannot create a second one). S0 writes no engine code |
| **Does not settle** | the opening of CY-S1..S5 or of CX; CX-Q2..Q11 and CX-Q19..Q28; item 4; a push; a version |
| **Recorded at** | this file; CY's header, §4 and §8; the CY row and this file's row in `instructions/v3.2.0/index.md` |

## 8. An uncaptured shape can always be given a fixed plan

| | |
|---|---|
| **Word** | W10, given while §7 was being recorded: *"And when no capture was made - the capture deferred for lack of statistics, the plan made again at each execution of the SQL - the user must still always be able to create a plan and fix it."* (given in Korean; recorded in English) |
| **Mark** | **CX-R11** added to `workorder-cx-managed-plans.md`: whatever keeps a shape uncaptured, a DBA or an agent can give it an administrator's plan that stays, written by hand or fixed from the search's answer of that moment, and a shape running uncaptured is visible with its reason. The second form is **CX-Q28**, CLA's proposal, unmarked |
| **Does not settle** | CX-Q28's spelling; CX-Q19, which decides when a capture is deferred at all |
| **Recorded at** | this file; the CX order's header, §2, §3 S1 and §4; the CX row and this file's row in `instructions/v3.2.0/index.md` |

## 9. Statistics work is behind OLTP

| | |
|---|---|
| **Word** | W11, given while CY-S0 was being set up: *"And applying statistics is lower priority: it must not affect the processing of OLTP business logic, or must keep that effect to a minimum."* (given in Korean; recorded in English) |
| **Mark** | **CY-R10** added to `workorder-cy-managed-statistics.md`: every act that makes or applies a statistic - a gather's walk and result write, the steward's save, the mount's seed, a publish, an import - runs at the `background` level, sliced and yielding, takes no lock a statement can wait on, adds nothing to a write or a point read, is budgeted in page reads and log writes, and is what waits or stops when resources are short. **CY-Q14**, the numeric bar, is CLA's proposal and unmarked |
| **Does not settle** | CY-Q14's numbers; whether a re-gather is ever started by the engine (item 4, pending) |
| **Recorded at** | this file; CY's header, §2, §4 and §6; the CY row and this file's row in `instructions/v3.2.0/index.md` |

## 10. CY-Q14..Q23 and CX-Q28 marked (a); pushed with the hook skipped

| | |
|---|---|
| **Word** | W12: *"CY-Q14..Q23, CX-Q28 $!a push no verify"* - after CLA's report at `e2f2e6d3` on worktree `cx-plan-change-option`, which put three marked items back to the operator |
| **Mark** | **CY-Q14..Q23 (a)**, each CLA's proposal: the bar for statistics work beside OLTP - inside the noise floor with a batch core, within 5 % at `cores = 1`, provisional (Q14); **the gate is CC's, 15 % of server CPU per statement in each of 3 interleaved pairs, with a bar each for S2 and S3, amending CY-Q1** (Q15); **the steward's `observed` object never replaces a gathered or imported one, amending CY-Q2 and CY-Q3** (Q16); a literal outside a histogram's range is priced by the constant (Q17); a compile that will be captured prices an equality from the column's average, never the literal's frequency (Q18); the stored object is unbanded and the band applies to each derived estimate, a publish being fresh on import, restore and mount (Q19); a gather uses bounded sketches under a byte cap (Q20); a gather holds its `IS` per slice and stops if the relation changed (Q21); **the model's ceiling is enforced at the write and nothing published is unloaded, amending CY-Q11** (Q22); the mount's seed reads relation objects only (Q23). **CX-Q28 (a)**: `CREATE PLAN <name> FOR <statement> AS SEARCHED` beside a plan written by hand. **Pushed to `main` with `--no-verify`** on this word, documents only: no suite ran; CY-S0's measurement was still running and none of its files is in the push |
| **Does not settle** | CX-Q2..Q11 and CX-Q19..Q27; the opening of CY-S1..S5 or of CX; item 4; a version |
| **Recorded at** | this file; CY's header, §2, §3, §4 and §8; the CX order's header, §2 and §4; the CX and CY rows and this file's row in `instructions/v3.2.0/index.md` |

## 11. BL's rung-3 probe measured

| | |
|---|---|
| **Word** | W13: *"measure rung 3 first"* - after CLA's answer that BL-R7's rungs 3 and 4 were neither discarded on cost nor merely unstarted: rung 3 waits on a condition, and the BL close cut the probe that tests it (given in Korean; recorded in English) |
| **Mark** | None by the operator. CLA ran §5.2's cut probe on worktree `bl-rung3-probe` at `67973ebc` (`v3.4.12-24-g67973ebc`). `tools/bl_mixed.py` gained `--normal-walkers` and `--meta-cores`, reviewed by `critics-developer`, two driver bugs fixed. Results: `bench/v3.4.12/results-bl-rung3-probe-v3.4.12-24-g67973ebc.md`. **Rung 3 is not opened under §5.2's condition**: bar (v)'s excess is still the checkpoint on a roll (all 227 reader samples at or above 10 ms overlap a checkpoint run), and readers beside the `normal` walker took no more slow samples than the rest. Forced sharing is not a clean bound (median reader p99 535 µs against 506 µs, 2 of 5 pairs above 2x) |
| **Does not settle** | Whether rung 3 is ruled out; rung 4; the checkpoint's roll wait (WAL work); a `normal` statement that does not yield; a merge, a push, a version |
| **Recorded at** | this file; the BL order's §6 ("Rung 3's probe measured 2026-11-18"); `docs/inflight/known-gaps.md`'s BL-R7 remainder; this file's row in `instructions/v3.2.0/index.md` |

## 12. The rung-3 probe pushed with the hook skipped

| | |
|---|---|
| **Word** | W14: *"push no verify"* - after CLA's report at `3b3f4851` on worktree `bl-rung3-probe` |
| **Mark** | None. `3b3f4851` pushed to `main` with `--no-verify` (`48857a5a..3b3f4851`, a fast-forward): the driver commit `67973ebc` and the results commit. The hook and the suite were skipped; the suite was never run for this work, which changes a tool and documents only. Recorded here after the push, in §13's commit |
| **Does not settle** | Whether rung 3 is ruled out; a version |
| **Recorded at** | this file; this file's row in `instructions/v3.2.0/index.md` |

## 13. The checkpoint's wait on a roll, and a work order written

| | |
|---|---|
| **Word** | W15, a question: *"Explain the part where the checkpoint waits on the segment roll, and explain the improvement options too."* W16, after CLA's answer: *"For now, leave it as a work order."* (each given in Korean; recorded in English) |
| **CLA's answer to W15** | From the code at `3b3f4851` and BQ's order. The checkpoint task takes the stream latch with a blocking lock at its begin and end records and at its log park's flush, so a roll's hold stops the reader core's reactor. Three options were named: the checkpoint parks for the latch; no zero prewrite where `posix_fallocate` reserved (BQ's H4 option 3); the latch released for the roll's I/O (BQ's rung 4, H6). CLA proposed the first, after a premise run with the cadence checkpoint off. **One statement in that answer is corrected here**: CLA said BQ's rung-2 bound predates the partition. BQ-S1 did measure driver-forced segregated placements, so the bound is re-read by CZ-S0, not set aside |
| **Mark** | None by the operator. **CZ written, not opened**: `instructions/v3.2.0/workorder-cz-checkpoint-roll-wait.md`, on worktree `bl-rung3-probe` from `3b3f4851`. Exploratory: CZ-S0 is the premise and the census with no engine code, CZ-S1 a spike that stays on the branch. CZ-Q0..Q10 carry CLA's proposals. The letter is CLA's proposal. **The order's review corrected the answer above**: the task reaches the latch at five kinds of site, not three (the inline WAL gate and the recycle at the anchor's commit besides), and the order's §1.2 is the account that stands |
| **Does not settle** | Every CZ-Q item; the opening of any CZ stage; H6; whether rung 3 is ruled out; a merge, a push, a version |
| **Recorded at** | this file; the order; the CZ row and this file's row in `instructions/v3.2.0/index.md` |

## 14. CZ pushed with the hook skipped

| | |
|---|---|
| **Word** | W17: *"push no verify"* - after CLA's report at `ddc4fb31` on worktree `bl-rung3-probe` |
| **Mark** | None. CZ's order, `ceiling.py` with its output, and §12-§13 go to `main` with `--no-verify`, in this section's commit. The hook and the suite are skipped; no suite was run for this work, which is documents and one analysis script |
| **Does not settle** | Every CZ-Q item; the opening of any CZ stage; a version |
| **Recorded at** | this file; this file's row in `instructions/v3.2.0/index.md` |

## 15. CY-S0's gate taken as not passed: CY-S2 and CY-S3 are not built

| | |
|---|---|
| **Context** | CLA's report of CY-S0 (`bench/v3.4.12/results-cy-s0-premise-v3.4.12-18-ge0cd5a93.md`, corrected after its review at `648d11ba`): over 15 cells no forced join order that differs from the informed planner's saves 15 % of server CPU, or of p50, in any round; the only different order is a tie; the scenario columns are uniform, and the compile with relation priors and no posterior was not measured. CLA recommended measuring two more cells before a final verdict |
| **Word** | W13 (the CX/CY matter's; §11's W13 is BL's): *"For now, take this gate as not passed; what is the next work?"* (given in Korean; recorded in English) |
| **Mark** | **CY-Q15 (a)'s gate is not passed. CY-S2 (column statistics) and CY-S3 (column-group statistics) are not built.** CY keeps S1 (persistence) and S4's lock, pending and publish, history and restore, export and import, over relation objects. The marks on the items that only S2 or S3 would use stay as marked and apply to nothing until an order reopens them. "For now" is the operator's: the verdict is not a ruling that column statistics are never built |
| **Does not settle** | the opening of CY-S1; CX-Q2..Q11 and CX-Q19..Q27; item 4; a push of `fc705f97` and `648d11ba`; a version |
| **Recorded at** | this file; CY's header, §3 and §8; the CY row and this file's row in `instructions/v3.2.0/index.md` |

## 16. Statistics unlogged; CY-S1 and CX opened; the standing go-ahead

| | |
|---|---|
| **Context** | CLA's answer on which engine state is memory-only and which is unlogged but on disk, then its list of the next work (CY-S1, CY-S4's remainder, CX-S0, CX-S1, CX-S2 onward) |
| **Word** | W14: *"For now, write every statistic as unlogged, and go on with: the statistics persisted to disk and the capture, the features around `CREATE PLAN`, the interface, and CX-S2 onward."* W15, the operator's answers to three questions CLA put before recording W14, each CLA's recommendation: (1) unlogged statistics are **kept across a clean shutdown only** - a mount after a crash or a restore discards them, as it discards every unlogged structure today (`DiscardAdvisoryState`, `src/server/expeditor.cpp:1043`); (2) **plans are logged**, the captured and the administrator's alike; (3) the unmarked CX items are **adopted as CLA proposed and the stages run one after another** (given in Korean; recorded in English) |
| **Mark** | **CY-S1 opened**, its `sys.statistics` unlogged under CR6 (`crosscore.md` CC13) and kept across a clean shutdown only - this replaces CY-S1's "logged" and the restore sentence, and CX-Q7's withdrawal text. **CX opened.** CX-Q2..Q11 (CX-Q7 withdrawn) and CX-Q19..Q27 are **adopted on the operator's standing go-ahead of 2026-11-18**, each as CLA proposed, not marked by the operator item by item. The order of work: CY-S1, then CX-S0, CX-S1, CX-S2 onward, with CY-S4's relation-object surface where it fits; every stage with its `critics-developer` review and the full suite; **it stops before any push, tag or version** |
| **Does not settle** | a push, a tag, a version; item 4; whether column statistics are ever built (§11's "for now"); `join_order`'s value names |
| **Recorded at** | this file; CY's header, §3 S1, §7 and §8; the CX order's header, §4 and §8; the CX and CY rows and this file's row in `instructions/v3.2.0/index.md` |

## 17. CY-S1a pushed with the hook skipped

| | |
|---|---|
| **Word** | W16 (the CX/CY matter's): *"For now, push the current work to main, no verify"* - after CLA's report of CY-S1a at `e13efd80` on worktree `cx-plan-change-option` |
| **Mark** | **Pushed to `main` with `--no-verify`**: CY-S0's results and their correction, §15's verdict, §16's opening, and **CY-S1a** - a relation's prior across a clean shutdown, unlogged `sys.statistics`, superblock 36. The hook was skipped; the full Debug suite ran 4595/4595 at `e13efd80` (`v3.4.12-26-ge13efd80`), and the two merges of `origin/main` after it (BL's rung-3 probe, the CZ order) brought documents, bench files and `tools/bl_mixed.py`, no source, test, CMake file or script. Overhead not measured; measured at the milestone's close |
| **Does not settle** | a tag, a version; CX-S0 and the stages after it, which go on under §16's go-ahead and stop before the next push |
| **Recorded at** | this file; this file's row in `instructions/v3.2.0/index.md` |

## 18. Access statistics kept, their storage to move to memory: the order split from CZ as DA

| | |
|---|---|
| **Context** | Written in another session of the day, on worktree `cz-access-stats-memory-resident`, and recorded there as that branch's §12; brought here unchanged but for the letter. A conversation that began at a two-priority WAL buffer and narrowed, on the operator's own turns, to writes that take no LSN, touch no visibility and may be lost. CLA read the tree for them and reported: the engine's statistics views are already memory-resident (`sys-views.md` §6); `sys.access_stats` is the one statistic still on pages - a catalog relation at page 11, unlogged by CR6, written under an exclusive hold by every statement, and emptied at every mount; it is on pages by history (the relayout planner's input, 2026-08-03; the planner deleted in BZ); its measured cost is 0.7 % of a point `SELECT` (BN-S1) and inside noise at `cores = 8` (CA-S0), so moving it is a cleanup and not a speed-up; and the feature itself - a count per relation, column set and access kind - is given by no other view. CLA recommended keeping the feature, moving the storage, and not raising the format for it alone |
| **Word** | That session's W14: *"On 'keep the feature and move only the storage to a memory-resident statistics view, which is cleanest; what is seen stays the same (it is emptied at a restart today as well), and the unlogged exception rule, the mount's discard code and the exclusive latch at every statement go; the one thing in the way is a superblock bump, so bind it to the next milestone that raises the format' - write the milestone."* (given in Korean, the quoted part being CLA's own recommendation returned; recorded in English). Its W15: *"push no verify"*. **In this session**, after CLA reported that the letter CZ named two orders: *"And separate CZ."* (given in Korean; recorded in English) |
| **Mark** | **The access-statistics order is `workorder-da-access-stats-memory-resident.md`, written and not opened; `workorder-cz-checkpoint-roll-wait.md` keeps CZ**, having reached `main` first (§13, §14). Every `CZ-` token of the access-statistics order is `DA-`; its text is otherwise as committed at `68d109d0`, with two notes in its header for what moved under it: the superblock is 36 since CY-S1a without the removal, and CY-S1a shares the discard walk the order deletes. **That session's push did not reach `main`**: `origin/main` at `59dcf2af` holds no file of it, and its landing merge `cd8e60cb` stands unpushed on its worktree. As that session recorded: the order's review corrected the ground its W14 stands on - a superblock bump is the cost of removing the relation's bootstrap rows and root only, the move of the storage changes no format - so DA-Q0's proposal is that only the removal (DA-S3) waits for a bump. DA-Q0..Q7 are unmarked |
| **Does not settle** | DA-Q0..Q7; the opening of DA; whether `SHOW ACCESS` is used at all; the branch `worktree-cz-access-stats-memory-resident` and its worktree, which this session did not touch and which still carry the order under its old name - not to be pushed as they stand; a push; a version |
| **Recorded at** | this file; the DA order's header; the DA row and this file's row in `instructions/v3.2.0/index.md` |

