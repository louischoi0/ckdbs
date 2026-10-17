# Ratification marks — 2026-10-17

The operator's words of 2026-10-17, recorded by CLA. Each is recorded
**in English**, translated from the Korean it was typed in.

## 1. CA written and opened

| | |
|---|---|
| **Word** | *"Things are now reasonably in order; this is the stage of optimizing performance from statistics. First, for frequently used query patterns, create index pages bound to the values queried most often; and where a query reads several tables, as a join does, add a concept that stores every lookup key ref."* (W0), then *"The aux structures the optimizer manages must be advisory, so their role is to help from the background while putting as little strain on transactions as possible. OLTP performance comes first; optimizing comes after it. So choose the structures and the design that help OLTP processing from the background."* (W1), then *"And now we open a real milestone, not a CN."* (W2) |
| **Mark** | **Every structure the optimizer manages is advisory** (invariants 8 and 9), **puts as little strain on transactions as possible** (CA-R2 reads it as one latch-free note and one latch-free probe), and **is built in the background**, with OLTP performance ranked ahead of optimization (W1). The design is CLA's choice, as W1 asks: `workorder-ca-background-advisory-optimizer.md` §0, §2. **CA is opened as a milestone, not a concept note** (W2). CA-S0, the premise measurement, needs no mark |
| **Does not settle** | CA-Q1..Q6 (`workorder-ca-background-advisory-optimizer.md` §4); no tag, no version |
| **Recorded at** | this file; `workorder-ca-background-advisory-optimizer.md` header; `index.md`'s row for it and this file's row |

## 2. CA made exploratory; CLA's proposals adopted on the standing go-ahead

| | |
|---|---|
| **Word** | *"For now treat this milestone as an exploratory one: apply several possibilities, find in the end a way that improves performance in all of scenarios 1, 2, 3 and 4, and report it."* (W3), with `/go-ahead-achieving-milestone` |
| **Mark** | **CA is exploratory** (`workorder-ca-background-advisory-optimizer.md` §5a): an option lands only if it improves `tools/scenario1..4` together. **Adopted on the operator's standing go-ahead of 2026-10-17 (go-ahead-achieving-milestone), not as the operator's own marks**: CA-Q1 **revised to (c)** before adoption - scenario 4's `on` arm is the controller (§4); CA-Q2 (a), CA-Q3 (b), CA-Q4 (b) (follows Q1 (c)), CA-Q5 (a), CA-Q6 (a), each as proposed and each subject to the §5a test |
| **Does not settle** | The cabin controller's deletion (W1's reading of it stands for a later order); push, merge, tag, version |
| **Recorded at** | this file; the work order's header, §4 and §5a; `index.md`'s CA row |
