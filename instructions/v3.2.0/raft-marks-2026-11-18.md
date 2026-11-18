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
