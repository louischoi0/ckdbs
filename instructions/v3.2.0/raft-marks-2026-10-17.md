# Ratification marks — 2026-10-17

The operator's words of 2026-10-17, recorded by CLA. Each is recorded
**in English**, translated from the Korean it was typed in.

## 1. BZ written and opened

| | |
|---|---|
| **Word** | *"Things are now reasonably in order; this is the stage of optimizing performance from statistics. First, for frequently used query patterns, create index pages bound to the values queried most often; and where a query reads several tables, as a join does, add a concept that stores every lookup key ref."* (W0), then *"The aux structures the optimizer manages must be advisory, so their role is to help from the background while putting as little strain on transactions as possible. OLTP performance comes first; optimizing comes after it. So choose the structures and the design that help OLTP processing from the background."* (W1), then *"And now we open a real milestone, not a CN."* (W2) |
| **Mark** | **Every structure the optimizer manages is advisory** (invariants 8 and 9), **puts as little strain on transactions as possible** (BZ-R2 reads it as one latch-free note and one latch-free probe), and **is built in the background**, with OLTP performance ranked ahead of optimization (W1). The design is CLA's choice, as W1 asks: `workorder-bz-background-advisory-optimizer.md` §0, §2. **BZ is opened as a milestone, not a concept note** (W2). BZ-S0, the premise measurement, needs no mark |
| **Does not settle** | BZ-Q1..Q6 (`workorder-bz-background-advisory-optimizer.md` §4); no tag, no version |
| **Recorded at** | this file; `workorder-bz-background-advisory-optimizer.md` header; `index.md`'s row for it and this file's row |
