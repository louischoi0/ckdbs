# Ratification marks — 2026-10-18

The operator's words of 2026-10-18, recorded by CLA. Each is recorded
**in English**, translated from the Korean it was typed in.

## 1. CB written: a join planner over I12

| | |
|---|---|
| **Prompt** | CLA's report, read at `dd3af538` against `bench/v3.1.0/` and a re-run on `build-release` at the same commit: scenario 3's `join-no-literal` (10,000 loans, no index on either engine, 0.57x of PostgreSQL's hash join) runs with `inner_built=1 build_rows=10000`, building the 10,000-row inner side where the 16-row outer side would do; written the other way round it runs 4.4x slower, because I12 fixes the written order and `u.id BETWEEN` is not carried to `l.user_id`; scenario 1's `model-join` probes one constant tuple key 359 times |
| **Word** | *"For 1, choose the smaller side; for 2, optimize the join order and carry the range condition to the key."* Then four answers to CLA's questions, each CLA's recommendation: emission order is no longer a contract without `ORDER BY` (CB-Q1 (a)); the join order is chosen by a data-free heuristic (CB-Q2 (a)); a trail's `step_id` names the relation's written position (CB-Q3 (a)); the rewrite covers inner joins only, decorrelation stays forbidden (CB-Q4 (a)) |
| **Mark** | **CB written and opened** (`workorder-cb-join-planner.md`). I12's "written order is the plan" and its hard rule "never the outer side", I11's defined prefix for `LIMIT` without `ORDER BY`, and §5's "propagating range bounds through a join key" being out of scope by decision are **overruled by the operator** for inner joins |
| **Does not settle** | push, merge to `main`, tag, version; the correlated-subquery prohibition stands (CB-Q4 (a)) |
| **Recorded at** | this file; the work order's header and §2; `index.md`'s CB row and this file's row |

## 2. CC written and opened on the standing go-ahead

| | |
|---|---|
| **Word** | *"Explore and develop a layer that heuristically finds the best path when planning, using statistics and identifiers"* with `/go-ahead-achieving-milestone` - *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session"* - then the same sentence again ending *"the purpose is query performance optimization"*. Before it: *"push no verify"*, which landed CB at `65dc45c5` |
| **Mark** | **CC written and opened** (`workorder-cc-plan-statistics.md`). CC-Q1..Q19 **adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone)**, each as CLA proposed, not marked by the operator item by item; CC-Q1 (a) is what W0 orders, and supersedes CB-Q2 (a) for the order of a planned block. CC-Q17 is `[quiet-wrong]` |
| **Does not settle** | push, merge to `main`, tag, version |
| **Recorded at** | this file; the order's header and §8; `index.md`'s CC row |
