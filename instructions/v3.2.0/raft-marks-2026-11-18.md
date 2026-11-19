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

## 19. `bench/README.md` shortened

| | |
|---|---|
| **Word** | W18 (the BL/CZ matter's): *"For now, shrink the current README.md a little. For old history (a month or more), leave a link and abbreviate the content, and the historical records other than the rules need their information compressed further."* (given in Korean; recorded in English) |
| **Mark** | None. CLA read "README.md" as `bench/README.md`: the root `README.md` holds no history and no rules, and `bench/README.md` is the one README with both. The three deletion entries (2026-09-02, 2026-09-30, 2026-10-12) and the BTREE section's account of the `f6ed10c` re-measurement are folded into one table and its resolution list, with a link to the full text at `1ad75954`; 158 lines to 118. **The rules are byte-identical** from "Where a result goes" to the BTREE heading. Reviewed by `critics-developer`, which restored two rule passages CLA had shortened and corrected a resolution bullet that sent the `f6ed10c` scenario files to a commit that does not hold them |
| **Does not settle** | Whether the root `README.md` was meant; the host notes inside rules 2, 4 and 5, which name the earlier box and were left as written; a push |
| **Recorded at** | this file (written as §15 on the work branch and renumbered twice behind `origin/main`'s sections at its merges, §18 and then §19; the commit `dc884be6` cites it as §15); `bench/README.md`; this file's row in `instructions/v3.2.0/index.md` |

## 20. `bench/README.md` pushed with the hook skipped; the same treatment asked for every README and for `CLAUDE.md`

| | |
|---|---|
| **Word** | W19 (the BL/CZ matter's), on CLA's question of which file W18 meant: *"Not bench/readme - I mean all the README.md files."*, and bench/README.md's change to be kept. W20: *"Do the same for CLAUDE.md: leave only the rules, and replace the history with links, or compress it, or delete it."* W21: *"push no verify"*, given while the review of that work was still running (each given in Korean; recorded in English) |
| **Mark** | None. On W21, `dc884be6` (§19) and this merge go to `main` with `--no-verify`; the hook and the suite are skipped, and this session ran no suite - its commits change documents only, and the source the merges brought is `origin/main`'s own. **The README and `CLAUDE.md` compaction of W19-W20 is not in this push**: it was uncommitted and under `critics-developer`'s review at the word, and follows in its own commit under the same word |
| **Does not settle** | A version |
| **Recorded at** | this file; this file's row in `instructions/v3.2.0/index.md` |

## 21. `CLAUDE.md` and the READMEs compacted; pushed with the hook skipped

| | |
|---|---|
| **Word** | W19 and W20 (§20). W22, while the review was still running: *"When the review is done, push CLAUDE.md with no verify too."* (given in Korean; recorded in English) |
| **Mark** | None. **`CLAUDE.md`: 70,113 bytes to about 56,000.** Thirty milestone rows state the contract without the stage that built it, its superblock number or its date, and a row's work orders moved to its owning-docs cell; the header's compaction note, the "settled" paragraph and the reopening paragraph are shortened. **Everything from "Hard Invariants" to the end is byte-identical.** The prior text is at `abe870fc`. **READMEs**: `instructions/`, `docs/`, `docs/pending/` (its table's subjects shortened, entries linked), `docs/conceptnotes/` and `CIP/` ("Not cut, and why"), each with a `git show` link to its prior text; the root `README.md`, `docs/inflight/`'s two and `manual/`'s three hold no history worth cutting and are unchanged. **Reviewed by `critics-developer`**, which found CLA's first rewrite had cut past history - refusals by name, two `[quiet-wrong]` markers, hard limits, correctness premises and known-gap pointers in about two dozen rows, and four statements made wrong - and restored them; that is why the file is 56 KB and not CLA's 49 |
| **Does not settle** | The dated operator quotations inside the rule sections, left as written; the mechanism detail the review chose not to restore (its report names it; the specs carry it); a version |
| **Recorded at** | this file; `CLAUDE.md`'s header; this file's row in `instructions/v3.2.0/index.md` |

## 22. A survey of what the engine lacks as an OLTP engine; the torn-page order written as DB

| | |
|---|---|
| **Context** | A new conversation. W0: *"Looking at the kdbs code as it stands, are there layers or components to be filled in, or concepts to apply? From the standpoint of an engine specialized for OLTP."* CLA answered from the specs, `known-gaps.md`, `docs/pending/` and a search of the source at `origin/main` `0500200d`, measuring nothing, with eight items: (1) a torn page is refused and not healed; (2) no replication or standby; (3) a commit's outcome cannot be asked for after a lost connection (CN-5, a concept note only); (4) a pool miss is a blocking read on the reactor; (5) four cores deliver less than two (a pending entry); (6) a hot row's writers are refused and retry (CR, written and not opened); (7) index entries are never removed; (8) no `SELECT ... FOR UPDATE`. It recommended 1, then 4, then 6. **Four of CLA's statements in that answer and the turns after were wrong or incomplete on the tree, and the orders written under §22-§25 each say which**: item 2 holds of `origin/main` only - the unlanded branch `worktree-cm-physical-standby` carries an order CM and its stages S0-S2; item 1 was called "a design the spec already adopted", where BR had tried that rule and found no site for it (DB §1.3); item 7 said nothing measures index growth, where BY's close measured a rebuild and found a probe cost the same before and after (DD §1.1); item 8 said the cost should be small because `U` exists, where the claim machinery is on the write walk, which uses no index and does not yield (DC §0) |
| **Word** | W1: *"Write the work order for item 1, the FPI cadence, first."* (given in Korean; recorded in English) |
| **Mark** | **`workorder-db-torn-page-healing.md` is written and not opened.** The letter DB is CLA's proposal. Its one `[quiet-wrong]` decision, DB-Q1, is where the image is logged. CLA drafted the order around an image at the device write and told the operator so while the review ran; `critics-developer` found five obligations that placement had not stated - among them changes lost after a crash during recovery, and BF-R4's reclaim gate broken - and a single site for the spec's own placement. CLA rewrote the design and **moved its proposal to the image at the first exclusive fetch after a writeback copy** (F), keeping the device-write placement as (b). **F's soundness argument was written after the review and is not reviewed**; DB-S0 reads the tree against it before DB-Q1 is marked. No suite was run and nothing was measured: documents only |
| **Does not settle** | DB-Q1..Q6; the opening of DB; a push; a version |
| **Recorded at** | this file; the DB order; the DB row and this file's row in `instructions/v3.2.0/index.md` |

## 23. `SELECT ... FOR UPDATE`: the order written as DC

| | |
|---|---|
| **Word** | W2, given while DB was being read for: *"Write the work order for item 8, SELECT FOR UPDATE."* (given in Korean; recorded in English) |
| **Mark** | **`workorder-dc-select-for-update.md` is written and not opened.** The letter DC is CLA's proposal. Written by a fork of the session and reviewed by `critics-developer`, every finding applied. Two items are `[quiet-wrong]`: DC-Q1, the mode - under `U`, the claim an `UPDATE` takes here, the spelling `FOR UPDATE` gives what PostgreSQL calls `FOR NO KEY UPDATE`, because a foreign key child's share passes it - and DC-Q3, which version is returned. Under DC-Q0 (a) only an equality or a list on the tuple key descends; every other locking read walks the whole relation without yielding, and the order says so in its §0 |
| **Does not settle** | DC-Q0..Q10; the opening of DC; a push; a version |
| **Recorded at** | this file; the DC order; the DC row and this file's row in `instructions/v3.2.0/index.md` |

## 24. Index entries removed: the order written as DD, exploratory

| | |
|---|---|
| **Word** | W3: *"Write the work order for item 7, index entry removal."* (given in Korean; recorded in English) |
| **Mark** | **`workorder-dd-index-entry-purge.md` is written and not opened, exploratory by CLA's proposal.** The letter DD is CLA's proposal. Written by a fork of the session and reviewed by `critics-developer`, every finding applied. Its premise is weak as measured (§22), so DD-S0 measures before anything is built and may end the order with nothing. The driver CLA first weighed, a writer compacting its full index leaf, is not available as stated: the writer holds its clustered leaf while it maintains its indexes. The proposal is one statement, `PURGE INDEX`, under the relation `IX`, removing what a rebuild would leave out. DD-Q1..Q3 and DD-R1..R4 are `[quiet-wrong]` |
| **Does not settle** | DD-Q0..Q7; the opening of DD; the numbers in DD-Q0's bar, which are CLA's; a push; a version |
| **Recorded at** | this file; the DD order; the DD row and this file's row in `instructions/v3.2.0/index.md` |

## 25. A page read that does not stop its core: the order written as DE, exploratory

| | |
|---|---|
| **Word** | W4: *"Explain item 4 in more detail."* - answered in conversation from the tree at `a4d2fb3b`. W5: *"Write the work order for item 4, asynchronous I/O, as well."* (each given in Korean; recorded in English) |
| **Mark** | **`workorder-de-page-read-off-the-reactor.md` is written and not opened.** The letter DE is CLA's proposal, and so is its exploratory shape: the word does not ask for one, and DE-Q0 puts it to the operator. Reviewed by `critics-developer`; its findings applied but one, named in the order's §7. DE-S0 measures whether a miss's stall reaches sessions that did not miss once the page cache is kept from answering; DE-Q6 asks first whether a working set beyond the pool is a case the engine serves at all. **A side finding of that review is entered in `known-gaps.md`**: a backup starts threads after startup, against `sched.md` §9's invariant 1 |
| **Does not settle** | DE-Q0..Q6; the opening of DE; the backup threads; a push; a version |
| **Recorded at** | this file; the DE order; `docs/inflight/known-gaps.md` "Backup and restore (BR)"; the DE row and this file's row in `instructions/v3.2.0/index.md` |

## 26. DB, DC, DD and DE pushed with the hook skipped

| | |
|---|---|
| **Word** | W6, on CLA's report of the four orders at `ebcbe67f` on worktree `db-fpi-cadence-dc-for-update`: *"push no verify"* |
| **Mark** | None. **Pushed to `main` with `--no-verify`**: the four orders, §22-§25, their index rows and the `known-gaps.md` entry. The hook and the suite are skipped, and no suite was run: the commits change documents only. `origin/main` stood at `a4d2fb3b`, the branch's base, so nothing was merged |
| **Does not settle** | Any DB-Q, DC-Q, DD-Q or DE-Q; the opening of any of the four; a re-review of DB §2.1's argument, which CLA offered and the word does not ask for; a version |
| **Recorded at** | this file; this file's row in `instructions/v3.2.0/index.md` |

## 27. CX-S0 pushed with the hook skipped; CX-S1 to go on

| | |
|---|---|
| **Word** | W18 (the CX/CY matter's), on CLA's report of CX-S0 at `dbe3ddb5` on worktree `cx-plan-change-option`: *"push no verify, and go on with CX-S1"* |
| **Mark** | **Pushed to `main` with `--no-verify`**: **CX-S0** - `join_order = planned | fixed | written` with `fixed` a server's default, `SET JOIN_ORDER`, the capture in memory - with its review applied, its stage log (the work order's §8) and the specs it amends. The hook was skipped; the full Debug suite ran 4619/4619 at `dbe3ddb5` (`v3.4.12-45-gdbe3ddb5`), and the merge of `origin/main` after it (`2bb06d30`: the DB-DE orders, the compacted `CLAUDE.md` and READMEs) brought documents only, no source, test, CMake file or script. Overhead not measured; measured at the milestone's close. The work order's §6 measurement for S0 was not run. **CX-S1 goes on** under §16's go-ahead |
| **Does not settle** | a tag, a version; a ruling on what S0 built differently from an adopted item (CX-Q22 as (b), CX-Q19 tightened, CX-R7 amended - the stage log states each); the push of CX-S1, which stops before it |
| **Recorded at** | this file; this file's row in `instructions/v3.2.0/index.md` |

## 28. DA-Q8: the access-statistics store's memory set by a boot key, marked (a)

| | |
|---|---|
| **Context** | A listing of the open milestones from CX on, read at `origin/main` `2bb06d30`, in which DA stood written and not opened. The operator asked of DA's store; CLA read the order at `a5581343` and answered that it is bounded - DA-Q3 (a), 4,096 shapes a core, a shape past it dropped and counted - by a compile-time count, with no key, as BO's four stores are, and that the one per-core memory key in the tree is `ash_ring_bytes_per_core`. CLA's view as given: a key is worth adding, not for memory protection (a slot is tens of bytes by CLA's estimate, unmeasured, so a core's part is hundreds of KiB) but because a schema whose shapes pass the bound has nothing to turn. On the operator's second question CLA answered that the 4,096 counts distinct `(access kind, relation, column mask)` shapes and neither statements nor bound values (the order's §6: columns, never values). CLA put three options: (a) a boot key in bytes a core over a flat table of fixed-size slots; (b) a boot key in shapes a core; (c) the constant kept |
| **Word** | This session's W1, on CLA's listing of the open milestones: *"When DA turns access_stats into a memory-resident view, should a memory allocation policy not be set as well? By naming the maximum allocation in the configuration."* W2, on CLA's answer: *"Is 4096 the cap on queries times bound values that the access_stats memory view can manage?"* W3, on CLA's three options: *"Record it as DA-Q8, `$!a`."* (each given in Korean; recorded in English) |
| **Mark** | **DA-Q8 (a), the operator's mark**: the store's memory is set by a boot key in bytes for each core's part, and a part is a flat table of fixed-size slots, so its table allocates at most the bytes written. Recorded in `workorder-da-access-stats-memory-resident.md` §4, with §2's part, DA-R1, DA-R3, DA-Q3's row, DA-S1 and the tests owed amended to carry it. **Said by CLA beside the option and not marked**: the key separate from `access_statistics`, the name `access_stats_bytes_per_core`, the default what admits 4,096 shapes a core |
| **Does not settle** | DA-Q0..Q7, DA-Q3 among them - which the mark narrows to the key's default, its (b) (one count for the instance) then a shared cap on top of the key, against DA-R4, and open to the operator; the key's name, range and refusals, when the table is allocated, its probing and a capacity field in `sys.meta`, CLA's proposals left to DA-S1; a key for the four BO stores; the opening of DA; a push; a version |
| **Recorded at** | this file; the DA order's header, §2, §3, §4, §5 and §7; the DA row and this file's row in `instructions/v3.2.0/index.md` |

## 29. DA-Q8's record pushed with the hook skipped

| | |
|---|---|
| **Word** | This session's W4, on CLA's report of §28's record at `479fb910` on worktree `da-q8-memory-bound-key`: *"push no verify"* |
| **Mark** | **Pushed to `main` with `--no-verify`**: §28's record - DA-Q8 and its mark in the DA order, this file and the index - with its review applied. The hook was skipped and no suite was executed; the commits change no build input (documents only). `origin/main` stood at `a5581343`, the commit the record was written from, so nothing was merged |
| **Does not settle** | the opening of DA; DA-Q0..Q7; a tag, a version |
| **Recorded at** | this file; this file's row in `instructions/v3.2.0/index.md` |

## 30. CX-S1a pushed with the hook skipped; the session handed off

| | |
|---|---|
| **Word** | W19 (the CX/CY matter's), on CLA's report of CX-S1a at `98e3f214` on worktree `cx-plan-change-option`: *"push no verify, and hand off for now"* |
| **Mark** | **Pushed to `main` with `--no-verify`**: **CX-S1a** - `sys.plans`, the fourth declaration relation (root page 18, **superblock 37**, which refuses 36), the captured orders of `join_order = fixed` written off the statement by core 0's `system` task and at a clean shutdown and loaded at a mount, `DROP PLAN`, `SHOW PLANS` - with its review applied and its stage log (the work order's §8); and, outside CX, a writeback run's pins riding an allowance of their own in the Debug pin ceiling (`page.md` §3). The hook was skipped; the full Debug suite ran 4639/4639 at `98e3f214` (`v3.4.12-53-g98e3f214`), and the merge of `origin/main` after it (`56d20394`: DA-Q8) brought documents only, no source, test, CMake file or script. Overhead not measured; measured at the milestone's close. **The session stops here**: CX-S1b is not started |
| **Does not settle** | a tag, a version; whether a mount should refuse on a `sys.plans` it cannot scan (`known-gaps.md`, Captured plans), which CLA put to the operator and the word does not answer; what S1a built differently from an adopted item (CX-Q24 (a), the stage log states it); the opening of CX-S1b, which the standing go-ahead of §16 covers and this word pauses |
| **Recorded at** | this file; this file's row in `instructions/v3.2.0/index.md` |

## 31. CZ opened; CZ-S0 started

| | |
|---|---|
| **Word** | W23 (the BL/CZ matter's), in a new session: *"Proceed with the CZ milestone."* (given in Korean; recorded in English) |
| **Mark** | None on any CZ-Q item. **CZ opened; CZ-S0 (the premise and the census) started** on worktree `cz-checkpoint-roll-wait`, branch `worktree-cz-checkpoint-roll-wait`, from `a4d2fb3b` (`v3.4.12-42-ga4d2fb3b`). CLA reads the word as opening the order and its first stage, and not as a mark: S0 writes no engine code and rests on no unmarked item. CZ-Q9 (the letter) was settled by §18. The measurement runs inside the standing budget (`CLAUDE.md`, "A measurement has a time budget"), which is the rule in force and not CZ-Q8's mark. **V0's bar is CZ-Q1 and is unmarked**: S0 reports its numbers against CLA's proposed bar and the verdict is the operator's. The session's shell is rooted outside this repository, so the worktree was made with `git worktree add` and not `EnterWorktree` |
| **Does not settle** | CZ-Q0..Q8 and CZ-Q10; V0's verdict; the opening of CZ-S1 or of any later stage; H6; a merge, a push, a version |
| **Recorded at** | this file (written as §22 on the work branch and renumbered at its two merges behind `origin/main`'s sections, §30 and then §31; the commits `936f76cf` and `890523a6` cite it as §22, `2102edb7` as §30); the CZ order's header and §7; the CZ row and this file's row in `instructions/v3.2.0/index.md` |

## 32. CZ-S0 pushed with the hook skipped

| | |
|---|---|
| **Word** | W24 (the BL/CZ matter's): *"push no verify"* - after CLA's report of CZ-S0 at `2102edb7` on worktree `cz-checkpoint-roll-wait`, two questions on what the cadence-off cell turns off, and a handoff |
| **Mark** | None. **CZ's opening and CZ-S0 go to `main` with `--no-verify`**, in this section's commit: the results file `bench/v3.4.12/results-cz-s0-premise-v3.4.12-42-ga4d2fb3b.md` with its archive, the order's §7 entry, CZ-R3's new bullet and CZ-Q11..Q13. The hook and the suite are skipped; no suite was run for this work, which changes no build input, and the source the merges brought is `origin/main`'s own. Overhead not measured; measured at the milestone's close |
| **Does not settle** | V0's verdict (CZ-Q1); CZ-Q0..Q8 and CZ-Q10..Q13, each CLA's proposal only; the opening of CZ-S1; whether the quiet gate's 25.8-minute hold counts against the stage's measurement budget; a version |
| **Recorded at** | this file; the order's header; this file's row in `instructions/v3.2.0/index.md` |

## 33. DF written: the statistics the engine records, widened

| | |
|---|---|
| **Context** | A new session on worktree `da-q8-memory-bound-key` at `56d20394`. Asked by the operator, CLA read `docs/spec/sys-views.md` §5-§8, `plan-stats.md` §2, `known-gaps.md` and `sys.meta`'s field names, and listed nine kinds of statistic the engine does not record and a commercial database does, the comparison from CLA's own knowledge and looked up nowhere: (1) what a statement shape costs - rows, pages and pool hits and misses, log bytes, the split of its time, a latency distribution, compile time, refusals by cause, the executed order, a `DELETE` shape; (2) transactions and the MVCC read horizon; (3) index and relation usage; (4) space - delete-marked slots, an index's dead entries, var-heap bytes - noted as BO-Q13's exclusion and the operator's to reopen; (5) a history of counters over time; (6) a memory view; (7) I/O and log detail; (8) progress of long operations; (9) column statistics for the planner |
| **Word** | This session's W1: *"In terms of the kinds of statistics recorded now, set out the items it would be good to add compared with a commercial database."* W2, on CLA's list: *"Write items 1 to 4 up as a work order."* (each given in Korean; recorded in English) |
| **Mark** | **DF written, not opened**: `workorder-df-statistics-coverage.md`, four parts - the statement shape, transactions and the read horizon, usage, space - each a stage that lands alone, no format moved. The letter DF is CLA's proposal. **Corrected by the tree against CLA's list as given**: the executed order is proposed not built (DF-Q5 (a): `sys.plans` owns a shape's order and shows it since CX-S1a, read at the merge with `db1880da`); a pool hit is not countable per statement (`pages_fetched` counts one a keyed access), and a miss only through the session's wait statistics, DF-Q6's source; an index's dead entries are DD's. **The session was isolated in its worktree and could not open one named for the order**; the work is on branch `worktree-df-statistics-coverage` in that worktree |
| **Does not settle** | DF-Q0..Q10, each unmarked, DF-Q9 among them - whether BO-Q13's exclusion is lifted, which W2's "items 1 to 4" asks to be written up and does not mark; the opening of DF; items 5-9 of the list; a push; a version |
| **Recorded at** | this file; `workorder-df-statistics-coverage.md`; the DF row and this file's row in `instructions/v3.2.0/index.md` |

## 34. DF-Q0..Q10 marked (a); DF pushed with the hook skipped

| | |
|---|---|
| **Context** | CLA's report of §33's order at `84b29eea` on worktree `da-q8-memory-bound-key`, branch `worktree-df-statistics-coverage`: the four parts, what the review corrected, what the merge with `db1880da` changed, and that DF-Q0..Q10 stood unmarked with (a) CLA's proposal in each - DF-Q9 (whether BO-Q13's exclusion is lifted) and DF-Q10 (the cost bar, 0.5 us of server CPU a point statement) named as the two to settle first. CLA offered the push with `--no-verify`, the change being documents only |
| **Word** | This session's W3: *"Follow the proposals, and push no verify."* (given in Korean; recorded in English) |
| **Mark** | **DF-Q0..Q10 each marked (a), as CLA proposed, the operator's mark.** Among them: DF-Q0 (a), one order of four parts; DF-Q5 (a), the executed order not built; DF-Q9 (a), BO-Q13's exclusion lifted for delete-marked slots (the steward's sampled estimate) and leaf divisions (counted in memory at their sites, compactions beside them), and for nothing else of its list; DF-Q10 (a), DF-S0 prices each part and the bar is BO's recording, 0.5 us at `cores = 1`. DF-Q4 (a), DF-Q6 (a) and DF-Q10 (a) are each conditional on what DF-S0 reads or prices, as the order's §4 states. **Pushed to `main` with `--no-verify`**: the order, §33, this section and the index. The hook was skipped and no suite was executed; the commits change no build input (documents only) |
| **Does not settle** | the opening of DF - the word marks the items and asks for the push, and names no stage; items 5-9 of CLA's list; DA's opening, which part C's access-shape columns wait for; a tag, a version |
| **Recorded at** | this file; `workorder-df-statistics-coverage.md`'s header, §4 and §7; the DF row and this file's row in `instructions/v3.2.0/index.md` |

## 35. CX-S1b and CX-S2 pushed with the hook skipped; CX-S3 to go on

| | |
|---|---|
| **Word** | W20 (the CX/CY matter's), on CLA's report of CX-S1b and CX-S2 at `1fd03107` on worktree `cx-plan-change-option`: *"push no verify, and go on with CX-S3"* (given in Korean; recorded in English) |
| **Mark** | **Pushed to `main` with `--no-verify`**: **CX-S1b** - the administrator's plan: `CREATE PLAN <name> FOR <select> VARIANT ... WHEN <rule> ORDER (...) ... ELSE ORDER (...)` and `AS SEARCHED`, `ALTER PLAN ENABLE | DISABLE | REPLACE VARIANT`, a total `WHEN` rule over the statement's arguments, an enabled plan the first source of a join order under every `join_order`, `DROP TABLE` RESTRICTed by one, `sys.statement_stats`' `plan_source`, `plan_name` and `variant` - and **CX-S2**, the rewrite inventory in `parser-v2.md` I12, which builds no engine code; each with its review applied and its stage log (the work order's §8). No superblock moves. The hook was skipped; the full Debug suite ran 0 failed of 4672 at `1fd03107` (`v3.4.12-72-g1fd03107`), and this section's commit after it changes documents only. Overhead not measured; measured at the milestone's close. **CX-S3 (access path pins) goes on** under the standing go-ahead of §16 |
| **Does not settle** | a tag, a version; whether a mount should refuse on a `sys.plans` it cannot scan (`known-gaps.md`, Captured plans), still with the operator; what S1b built differently from an adopted item - no `IS NULL` in a rule (CX-Q5), a tuple key chain probed while a plan is enabled (CX-R6), `CREATE PLAN` by example with literals, `invalid` derived and a decline marking nothing (CX-Q4 (b), CX-Q16), `REPLACE VARIANT` not compiled, plan DDL not waiting for plan DDL - each stated in the stage log and reported, none marked by this word; the same for S2's two corrections of the order's classes |
| **Recorded at** | this file; the CX row and this file's row in `instructions/v3.2.0/index.md` |

## 36. DF opened; DF-S0 started

| | |
|---|---|
| **Context** | CLA's report of §34's push at `57d7c67f`, which said DF was not opened by that word and that an opening would start at DF-S0 - no engine code on `main`, the cited lines re-read, and each part's statement-path cost priced against DF-Q10's bar |
| **Word** | This session's W4: *"Open DF and proceed from S0."* (given in Korean; recorded in English) |
| **Mark** | **DF opened**, from `57d7c67f` (`v3.4.12-67-g57d7c67f`), on worktree `da-q8-memory-bound-key`, branch `worktree-df-statistics-coverage`. **DF-S0 started**: the census of the order's §5 and DF-Q10 (a)'s spike and cells |
| **Does not settle** | whether the stages after DF-S0 run without a further word - "from S0" names where to start and CLA stops at DF-S0's report, as the Session Workflow's step 4 has it; anything DF-S0 takes back to the operator; a push; a tag, a version |
| **Recorded at** | this file (written as §35 on the work branch and renumbered at the merge behind `origin/main`'s §35; the commit `6e7f7095` cites it as §35); `workorder-df-statistics-coverage.md`'s header; the DF row and this file's row in `instructions/v3.2.0/index.md` |

## 37. DF-Q11..Q14 marked (a); DF-S0 pushed with the hook skipped; the session handed off

| | |
|---|---|
| **Context** | CLA's report of DF-S0 at `011d1609` on worktree `da-q8-memory-bound-key`, branch `worktree-df-statistics-coverage`: the census; the spike's price at `cores = 1`, every median under DF-Q10 (a)'s 0.5 us and none resolved from the run's noise, `cores = 8` and part C's per-index fold not priced; and four items taken back with (a) CLA's proposal in each - DF-Q11 (`DELETE`'s shape, three behaviours moving besides the statistics key), DF-Q12 (a statement's log bytes booked to the polled task's session), DF-Q13 (part B's counts, six), DF-Q14 (the two cells with no data measured at the close after a driver change). CLA said DF-S1 would go on once the items were marked and that a push needed the word |
| **Word** | This session's W5: *"Follow the proposals and push no verify; after that, prepare the handoff."* (given in Korean; recorded in English) |
| **Mark** | **DF-Q11..Q14 each marked (a), as CLA proposed, the operator's mark**: `DELETE` takes a shape with its three movements accepted and stated; a statement's log bytes are booked at `AppendThrough` to the session of the task being polled; part B counts `commits`, `rollbacks`, `ceiling_rollbacks`, `close_rollbacks`, `aborts` and `wait_aborts`; `cores = 8` and part C's per-index fold are measured at the close, a `cores` flag and an index-served read added to the census driver in a commit of its own before it. DF-Q12 (a) replaces DF-Q7 (a)'s mechanism and its scope (a record under no transaction is then the statement's own), and DF-Q13 (a) replaces the four counts of the order's §2; DF-Q11 (a) discharges DF-Q4 (a)'s condition by accepting what failed it. **Pushed to `main` with `--no-verify`**: DF's opening (§36), DF-S0's census, its results file and archive, this section and the index. The hook was skipped and no suite was executed: the work branch changes no build input (bench and documents only), and the merge of `origin/main` before it (`5038f999`: CX-S1b, CX-S2) brought source that this session neither built nor tested. **The session stops here**: DF-S1 is not started, and the handoff is a memory entry outside the tree |
| **Does not settle** | when DF-S1 starts and whether the stages after it run on one word - W4's "from S0" was read as a start, and this word asks for a handoff, not a stage; the spike's branch `df-s0-spike`, kept local and unpushed; what DF-S0's review left unverified (the order's §7); a tag, a version |
| **Recorded at** | this file; `workorder-df-statistics-coverage.md`'s header and §7; the DF row and this file's row in `instructions/v3.2.0/index.md` |

## 38. CX-S3a pushed with the hook skipped; the session handed off

| | |
|---|---|
| **Word** | W21 (the CX/CY matter's), given while the review of CX-S3a (`4880575c`, worktree `cx-plan-change-option`) was still running: *"When the review is applied, push no verify and go on to the handoff."* (given in Korean; recorded in English) |
| **Mark** | **Pushed to `main` with `--no-verify`**: **CX-S3a** - a variant's access pins: `ACCESS <relation> TUPLE_KEY | INDEX [<name>] | CABIN | SCAN`, or `NOT` before one of the first three, in `CREATE PLAN` and `ALTER PLAN ... REPLACE VARIANT`; a pin restricts the ladder and is never forced (refused at `CREATE PLAN`, declined at a compile); `DROP INDEX` RESTRICTed by an enabled plan that pins the index by name; `ANALYZE`'s `arm=plan` - with its review applied and its stage log (the work order's §8). CX-S3 is split: **S3b (CX-Q6 (a), controller-made Cabins) is not started**. No superblock moves. The hook was skipped; the full Debug suite ran 0 failed of 4677 at `58d258a3` (`v3.4.12-76-g58d258a3`), and the merge of `origin/main` after it (`6f52c652`: DF-S0) brought bench and documents only - no source, test, CMake file or script. Overhead not measured; measured at the milestone's close. **The session stops here** |
| **Does not settle** | a tag, a version; the opening of CX-S3b, which the standing go-ahead of §16 covers and this word pauses; what S3a built differently from the order's text - `arm=plan` on the pinned step alone, a Cabin's drop not RESTRICTed, a one-relation statement still taking no plan - each stated in the stage log, none marked by this word; whether a mount should refuse on a `sys.plans` it cannot scan, still with the operator |
| **Recorded at** | this file; the CX row and this file's row in `instructions/v3.2.0/index.md` |

## 39. DF-S1 started

| | |
|---|---|
| **Context** | A new session, given the handoff §37 left: DF's order, its opening and DF-S0 on `origin/main` at `6f52c652`, every item DF-Q0..Q14 marked (a), DF-S1 not started, and the handoff's own note that §37's word started no stage and that the next session asks before starting DF-S1 unless the operator's message says to |
| **Word** | W6 (the DF matter's), this session's first: *"Continue from the DF-S0 handoff."* (given in Korean; recorded in English) |
| **Mark** | **Read as the start of DF-S1**, the one thing the handoff names as next. **DF-S1 started**, on worktree `da-q8-memory-bound-key`, branch `worktree-df-statistics-coverage`, from `6f52c652`: part A of the order - `DELETE`'s shape in a commit of its own, `sys.statement_stats` widened, `sys.statement_histogram`, `sys.refusal_stats` - built as DF-Q1..Q3, DF-Q6, DF-Q11 and DF-Q12 (a) mark it |
| **Does not settle** | whether DF-S2 and the stages after it run on this word - "continue" names no end, and CLA stops at DF-S1's report, as the Session Workflow's step 4 has it; a push; what DF-S1 built differently from the order's §2, each stated in its stage log and none marked by this word; a tag, a version |
| **Recorded at** | this file; `workorder-df-statistics-coverage.md`'s header and §7; the DF row and this file's row in `instructions/v3.2.0/index.md` |

## 40. DF-S1 pushed with the hook skipped; the session handed off

| | |
|---|---|
| **Context** | CLA's report of DF-S1 at `0e6605ce` (`v3.4.12-89-g0e6605ce`) on worktree `da-q8-memory-bound-key`, branch `worktree-df-statistics-coverage`: part A built, reviewed by `critics-developer` with what was applied and what was not taken, the full suite 0 failed of 4700 at `ee89842e`, `origin/main` (`2ca42fea`) merged clean, overhead not measured, the six things built differently from the order's §2 or decided where it was silent, the cells §5 owes that were not built, no `tsan` run. CLA said a push needed the word and that DF-S2 would not start without one |
| **Word** | W7 (the DF matter's): *"Push no verify, and hand off."* (given in Korean; recorded in English) |
| **Mark** | **Pushed to `main` with `--no-verify`**: **DF-S1** - `DELETE` has a shape (`9f9b207b`), `sys.statement_stats` carries what a shape cost, `sys.statement_histogram` and `sys.refusal_stats` are new (`44dddd27`), the review applied (`ee89842e`), with its stage log (the order's §7), §39 and this section. No format moves. The hook was skipped; the full Debug suite ran 0 failed of 4700 at `ee89842e` (`v3.4.12-88-gee89842e`), on the tree merged with `origin/main` at `2ca42fea`, and the two commits after it change documents only - no source, test, CMake file or script; `origin/main` had not moved at the fetch before the push. Overhead not measured; measured at the milestone's close. **The session stops here**: DF-S2 is not started, and the handoff is a memory entry outside the tree |
| **Does not settle** | when DF-S2 starts and whether the stages after it run on one word - this word asks for a push and a handoff, not a stage; the six things DF-S1 built differently from the order's §2, each stated in its stage log, the operator having been shown them and having marked none; the cells §5 owes for part A that were not built, and the two-core cell's `tsan` run; a tag, a version |
| **Recorded at** | this file; `workorder-df-statistics-coverage.md`'s header and §7; the DF row and this file's row in `instructions/v3.2.0/index.md` |

## 41. V0 passed; CZ-Q0..Q8 and CZ-Q10..Q13 marked (a); CZ-S1 opened; a quiet gate's hold is outside the budget

| | |
|---|---|
| **Context** | A new session, taken up from the handoff note §32's session left. CLA put three questions to the operator, its recommendation first in each: V0's verdict and the next step; the marks on the CZ-Q items; whether the time a quiet gate holds a measurement counts against its budget (the question §32 left open) |
| **Word** | W25 (the BL/CZ matter's): *"Continue from it."* (the handoff note), and then the operator's choice among the options of each question, CLA's recommendation in all three: *"V0 passed, start CZ-S1"*; *"all (a)"*; *"not counted"* (given in Korean; recorded in English) |
| **Mark** | **V0 passed**, on CZ-Q1 (a)'s bar. **CZ-Q0..Q8 and CZ-Q10..Q13 (a)**, each CLA's proposal: the order is exploratory (Q0); the bar is V0 and V1 as written (Q1); the bulk cell loses at most 3 %, a checkpoint's wall time and the live segment count reported with no bar (Q2); C1 alone is built, C2 priced in S1 and built only if C1 misses V1 and the operator marks it (Q3); 16 tries before the blocking path (Q4); one `levels` cell at `group` at the close, no bar (Q5); at the close CZ re-measures BL's bar (v) and amends BL's texts, BL not reopened (Q6); H6 out (Q7); the standing budget (Q8); the recycle joins C1 as C1c and the inline WAL gate stays blocking, counted in S1 (Q10); the bar stays p99, with p99.9 and the count of samples at or above 10 ms reported beside it (Q11); the checkpoint's page writes on a read core stay out of CZ, a run's longest page write reported beside the stalls (Q12); M4 dropped (Q13). CZ-Q9 was settled by §18. **CZ-S1 opened**: a measurement-only spike that is not landed. **The time a quiet gate holds a measurement is not counted against its budget**: the budget counts building and running the cells, and the results file states the hold beside them; `CLAUDE.md`'s rule carries the sentence |
| **Does not settle** | C0, C1 or C1 with C2 - S1's ceiling is the operator's to read; the opening of CZ-S2 or of any later stage; C2's build (Q3 (a) leaves it to a later mark); a merge, a push, a version |
| **Recorded at** | this file (written as §33 on the work branch and renumbered at its three merges behind `origin/main`'s sections, §35, §39 and then §41; the commit `bf8d7a56` cites it as §33, `fd96b5d3` as §35 and `beda4b7f` as §39); the order's header and §4; `CLAUDE.md`, "A measurement has a time budget"; the CZ row and this file's row in `instructions/v3.2.0/index.md` |

## 42. S1 read as C1 alone; CZ-S2 opened; C2 priced further before any order for it

| | |
|---|---|
| **Context** | CLA's report of CZ-S1 at `fd96b5d3` on worktree `cz-checkpoint-roll-wait`, then the operator's questions on what the cells and the inserter's rate mean, and a request to weigh the three readings by the database's performance as a whole rather than by the reader's p99. CLA's answer, from the CZ-S1 archive: C1 with C2 is the strongest measured (the bulk writer +39 %, the walker's statements +45 %, the writer's and the walker's stalls of hundreds of milliseconds gone), C1 alone mends the reader's tail only, and C2 cannot be adopted on this evidence because the cost it moves to a commit's `fsync` was not measured at `group` or `strict` and its arms carried more load. CLA recommended, in order: (1) CZ goes on with C1; (2) two measurements of C2 first - the commit path at `group` and `strict` with and without the prewrite, and C1 against C1 with C2 with the inserter held to one rate; (3) if they are good, a work order of its own for C2 |
| **Word** | W26 (the BL/CZ matter's): *"Proceed with 1, 2 and 3 in order."* (given in Korean; recorded in English) |
| **Mark** | **S1's reading is C1 alone** (CZ-Q3 (a) stands). **CZ-S2 opened**: the model on paper, no code, its items back to the operator before CZ-S3. **C2 is priced further under CZ-S1** (CZ-Q3 (a): "C2 is priced in S1"), as CZ-S1b: the two measurements of (2), on the spike's binary, inside the standing budget. The second needs an inserter held to a rate, a driver change CZ-Q13 (a) had dropped; CLA reads the word as admitting it for this measurement, with its own review. **A work order for C2 is written if CZ-S1b's numbers support it** - CLA's judgement of "good", stated in that order with the numbers - and is not opened by this word |
| **Does not settle** | the opening of CZ-S3 or of the C2 order; any item CZ-S2 brings back; C2's build; a merge, a push, a version |
| **Recorded at** | this file (written as §36 on the work branch and renumbered at its two merges behind `origin/main`'s sections, §40 and then §42; the commits `90ea12ca`, `860d36f9` and `0f477739` cite it as §36 and `beda4b7f` as §40); the CZ order's header and §7; `workorder-dg-wal-segment-body.md`; the CZ and DG rows and this file's row in `instructions/v3.2.0/index.md` |

## 43. CZ-Q14..Q22 marked (a); the session handed off

| | |
|---|---|
| **Context** | CLA's report of the three steps of §42 at `beda4b7f` on worktree `cz-checkpoint-roll-wait`: CZ-S2's model (the CZ order's §8) with its nine items CZ-Q14 to CZ-Q22, (a) CLA's proposal in each and two of them `[quiet-wrong]` (CZ-Q14, CZ-Q16; CZ-Q21's (b) as CZ-Q14 is); CZ-S1b's numbers, which do not support C2 as it stands; and DG, written wider than §42's word, DG-Q0 (c) being "no order". CLA listed what it owed the operator: the marks on CZ-Q14 to CZ-Q22 and the word for CZ-S3, DG-Q0, and the push |
| **Word** | W27 (the BL/CZ matter's): *"Mark CZ-Q14 to CZ-Q22 all (a) and go on to the handoff."* (given in Korean; recorded in English) |
| **Mark** | **CZ-Q14..Q22 each marked (a), as CLA proposed, the operator's mark**: a flush skipped on a busy latch asks the writer for `flushed_lsn()` and nothing above it, and the task takes the durability park with CZ-Q15's wait beside it and tries the flush again, to CZ-Q4's bound (Q14); the wait between two tries when no roll holds the latch is a park of 1 ms by a deadline in the park's predicate, every busy answer at a site counted against one bound of 16 (Q15); the assertion bases' appends stay blocking inside the directory latch's one hold, the visit's whole time counted (Q16); the park on a roll reads a flag the roll sets and a scope guard clears, with a ceiling of 1 s on one park and no kick (Q17); the park is booked under a new wait event, `sysio.wal_roll` (Q18); a tried append that takes the latch and must roll rolls on the task's reactor, as today, counted on the roll line (Q19); the owed simulator test is replaced by rig tests and the simulator is untouched (Q20); when the latch is free the park's flush is today's, this thread writing the staged ring (Q21); a recycle skipped 16 runs in a row blocks at the detach, counted, and a skip is counted on the run line (Q22). **The session stops here**: CZ-S3 is not opened and not started, nothing is pushed, and the handoff is a memory entry outside the tree |
| **Does not settle** | the opening of CZ-S3 - this word marks the items and asks for a handoff, not a stage; DG-Q0..Q10, DG's standing among them; the push of the work branch, which is ahead of `origin/main` by documents and `bench/` only with no suite executed; what CZ-S1b showed against CZ-Q21 (a) - the stalls C1 alone kept in those cells were the park's flush writing the staged ring on a read core, and (a) keeps that write, so V1 is where it is read; a merge, a version |
| **Recorded at** | this file (written as §41 on the work branch and renumbered §43 at its merge behind `origin/main`'s §39 and §40; the commit `25c2285d` cites it as §41); the CZ order's header, §4, §7 and §8; the CZ row and this file's row in `instructions/v3.2.0/index.md` |

## 44. CZ-S1, CZ-S1b, CZ-S2, the marks and DG's text pushed with the hook skipped

| | |
|---|---|
| **Word** | W28 (the BL/CZ matter's): *"push no verify"* - after CLA's report of §43's marks and the handoff, at `25c2285d` on worktree `cz-checkpoint-roll-wait`, which listed the push among what it owed the operator |
| **Mark** | None. **The work branch goes to `main` with `--no-verify`**, in this section's commit: §41 to §43 and this section; CZ-S1's results file `bench/v3.4.12/results-cz-s1-ceiling-v3.4.12-66-g20d89234.md` and CZ-S1b's `bench/v3.4.12/results-cz-s1b-c2-priced-v3.4.12-66-g20d89234.md`, each with its archive; the CZ order's §4, §7 and §8 (CZ-S2's model, its items marked); `workorder-dg-wal-segment-body.md`, written and not opened; the sentence on a quiet gate's hold in `CLAUDE.md`; the index. **The hook and the suite are skipped, and no suite was run by this session**: the work branch changes no build input (bench and documents only). The merge of `origin/main` before it (`3adf8810`: DF-S1) brought source and tests that this session neither built nor tested; §40 records that work's suite as 0 failed of 4700 at `ee89842e`, a run this session did not make or repeat. The spike (`worktree-cz-s1-spike`) is not pushed and not merged; its patch is in the two archives. Overhead not measured; measured at the milestone's close |
| **Does not settle** | the opening of CZ-S3; DG-Q0..Q10, DG's standing among them - DG's text reaching `main` is not its opening and not a mark on DG-Q0; C2's build; a tag, a version |
| **Recorded at** | this file; the CZ order's header; this file's row in `instructions/v3.2.0/index.md` |

## 45. CX-S3b pushed with the hook skipped

| | |
|---|---|
| **Word** | *"push no verify"* - after CLA's report of CX-S3b at `c5debeb9` and the merge of `origin/main` at `4e0a1649` on worktree `cx-plan-change-option`, while that merge's build and suite were running. The stage itself ran on the operator's earlier *"continue the CX handoff"* (given in Korean; recorded in English), under §16's standing go-ahead |
| **Mark** | None. **The work branch goes to `main` with `--no-verify`**: CX-S3b - CX-Q6 (a), a controller-made Cabin reaches the ladder only through a plan that names it, `ACCESS <relation> CABIN (<column>)`; a statement over one relation takes a plan for its arm; an enabled plan that names a Cabin RESTRICTs `DROP CABIN` and the cabin optimizer's own DROP - with its review applied and its stage log (the order's §8). No superblock moves. **The hook was skipped. The full Debug suite ran 0 failed of 4683 at `c5debeb9`**; the merge of `origin/main` after it (`5705027f`, which brought DF-S1's source and tests) was being built and suite-run when the word came, and that run is not part of this push and is not reported as a pass. **Corrected after the push**: that run finished 0 failed of 4706 at `4e0a1649`, and `2d297055` adds no build input past it (the push record and the landing merge only), so the pushed build inputs passed the full suite - after the push, not before it. Overhead not measured; measured at the milestone's close |
| **Does not settle** | whether a cabin controller that accelerates nothing until a plan names its Cabins is what the operator wants - CX-Q6 (a)'s consequence, recorded in the stage log and `known-gaps.md` as the operator's to confirm or reverse; CX-S5..S7; a tag, a version |
| **Recorded at** | this file; the CX order's §8; the CX row and this file's row in `instructions/v3.2.0/index.md` |

## 46. CX-S5 pushed with the hook skipped

| | |
|---|---|
| **Word** | *"push no verify"* - after CLA's report of CX-S5 at `6948c875` on worktree `cx-plan-change-option`. The stage ran on the operator's "go on with CX-S5" (given in Korean; recorded in English), under §16's standing go-ahead |
| **Mark** | None. **The work branch goes to `main` with `--no-verify`**: CX-S5 - a variant pins a relation's join method, `BUILD <relation> ON | OFF`: `ON` holds the step to the statement-local build, the keyed arms standing aside, met only where the build's own rule annotates the step; `OFF` takes the build's rank away; neither forced, the cap's run-time decline a resource limit (CX-Q8 (a)) - with its review applied and its stage log (the order's §8), and §45's suite sentence corrected. No superblock moves. **The hook was skipped; the full Debug suite ran 0 failed of 4708 at `6948c875`**, and this section's commit adds documents only. Overhead not measured; measured at the milestone's close |
| **Does not settle** | `ON`'s reading as a restriction to the build, which is CLA's and unmarked; CX-Q6 (a)'s consequence (§45); CX-S6 and S7; a tag, a version |
| **Recorded at** | this file; the CX order's §8; the CX row and this file's row in `instructions/v3.2.0/index.md` |

## 47. CX to its close on the standing go-ahead; CX-Q29..Q37 adopted as CLA proposed

| | |
|---|---|
| **Word** | The operator invoked `/go-ahead-achieving-milestone` after §46's push: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* The milestone is CX, the one this session and its worktree `cx-plan-change-option` are on |
| **Mark** | None of the operator's per item. **Adopted on the operator's standing go-ahead of 2026-11-18 (go-ahead-achieving-milestone), each as CLA proposed** in the order's items table: CX-Q29 (a), `ADVISE PLAN` is `admin`'s; CX-Q30 (a), `sys.plan_advice` is memory, one table, bounded; CX-Q31 (a), the view keeps the shape text and the reply the literals; CX-Q32 (a), an advice is priced against the plan the statement's next compile would run; CX-Q33 (a), a one-relation statement is refused, an unsearched join advised as written; CX-Q34 (a), a mount still refuses a `sys.plans` it cannot scan; CX-Q35 (a), `BUILD ON` as S5 built it; CX-Q36 (a), CX-Q6 (a)'s consequence kept; CX-Q37 (a), S6's measurement read against the removable compile, so no plan cache is built (its bounds in the order's items table). None is `[quiet-wrong]`. The stages left - S6 (gated on its measurement, CX-Q10; done, CX-Q37), S7 and the milestone's close measurement - run in this session; a push, a tag or a version still waits for the operator's word |
| **Does not settle** | a push, a merge to `main`, a tag, a version; CX-Q36 (b), left for the operator |
| **Recorded at** | this file; the CX order's items table and §8; the CX row and this file's row in `instructions/v3.2.0/index.md` |

## 48. CY to its close on the standing go-ahead; CY-Q24..Q30 adopted as CLA proposed

Numbered after §47, which is on CX's work branch `worktree-cx-plan-change-option` and not on `main` when this was written.

| | |
|---|---|
| **Word** | Given at 13:33 UTC and run six hours later, as asked: *"start the CY milestone 6 hours from now /go-ahead-achieving-milestone"* (given in Korean; recorded in English) - the skill's standing word, *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* The milestone is CY, named in the skill's argument. It runs on a worktree of its own, `cy-managed-statistics`, from `origin/main` at `9712f5f0`, because CX's worktree `cx-plan-change-option` is held by another live session |
| **Mark** | None of the operator's per item. **Adopted on the operator's standing go-ahead of 2026-11-18 (go-ahead-achieving-milestone), each as CLA proposed** in the CY order's items table: CY-Q24 (a), the relation object alone, the column surface refused by name; CY-Q25 (a), the managed store in memory, written by the shutdown tail; CY-Q26 (a), a gather counts live slots and leaves, PS1's quantity, with no view; CY-Q27 (a), the `analytic` slice whatever the session declared, on the session's core; CY-Q28 (a), a lock stops every act that changes the published object without `FORCE`; CY-Q29 (a), `EXPORT` a `SELECT` over `sys.statistics`, a one-line document; CY-Q30 (a), `ADVISE PLAN ... USING PENDING` not built in CY; and CY-Q31 (a), raised by the close's measurement, a gather paced to 16 leaves per 2 ms and a republish only where a band moved. None is `[quiet-wrong]`. The stages left - S1b, S4's relation-object surface, the close's measurement (CY-S5) - run in this session; a push, a tag or a version still waits for the operator's word |
| **Does not settle** | a push, a merge to `main`, a tag, a version; item 4 (`docs/pending/statistics-staleness-and-regather.md`); S2 and S3, which W13 keeps unbuilt |
| **Recorded at** | this file; the CY order's items table and §8; the CY row and this file's row in `instructions/v3.2.0/index.md` |

## 49. CX pushed with the hook skipped, the close's regression standing; a first merge that dropped CY, fixed forward

| | |
|---|---|
| **Word** | *"push no verify"* - after CLA's stop report of CX's close at `b1c0307a` on worktree `cx-plan-change-option` (§47), which stopped at the measured regression: point statements 1.2 % to 2.0 % slower than at `8b584cf6` at `cores = 1`, part DF-S1's and the rest unattributed (`bench/v3.4.12/results-cx-close-v3.4.12-115-ga32528a4.md`) |
| **Mark** | None. **CX-S6, CX-S7 with its review, and CX's close go to `main` with `--no-verify`, the regression standing and unattributed**; no bisect is run. **The hook was skipped. The full Debug suite ran 0 failed of 4712 at `a32528a4`; `b1c0307a` changed `bench/` and documents only. The merge with CY (`647ad6d4`) brought code, so that suite does not cover the merged tree**, and the reply says what ran on it. **A first push went wrong**: `origin/main` moved to CY's merge `647ad6d4` between CLA's fetch and its `commit-tree`, the shared ref was read at the new value, and the commit `828899f6` - parents `647ad6d4` and `b1c0307a` - carried the work branch's tree, which drops CY's change. It was fixed forward by a commit on top of it carrying the real merge of the two (conflicts in `command_dispatcher.hpp`, `sys_views.cpp`, this file and the index, each resolved by keeping both sides); no force push |
| **Does not settle** | the regression's cause (a bisect of `ee89842e..a32528a4` with a `sys.statement_stats` spike, waiting for a measurement budget); CX-Q36 (b) and CX-Q37 (b)/(c); a tag, a version |
| **Recorded at** | this file; the CX order's §8; the CX row and this file's row in `instructions/v3.2.0/index.md` |
