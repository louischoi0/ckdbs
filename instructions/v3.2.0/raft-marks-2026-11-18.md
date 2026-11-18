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
