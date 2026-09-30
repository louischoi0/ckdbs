# Ratification marks — 2026-09-30

**The operator's words of 2026-09-30**, recorded by CLA on
`ay-s9-assertion-every-chain` from `b5465a9` (`v2.7.0-502-gb5465a9`; the
v3.0.0 tag is not cut - `raft-marks-2026-09-26.md` §4, Q3). All of it is
**verbatim**: the words as typed in the session.

---

## 1. AY-S9 started

Recorded on `worktree-ay-s9-assertion-every-chain` from `b5465a9`, with
AY-S7 on `main` at `b5465a9`.

| | |
|---|---|
| **Word** | *"/inv-where-to-start-and-go-ahead"* - the skill that takes the first startable stage in the version's order |
| **Mark** | **AY-S9 starts**: D7's assertion arm (AY-R8), red first |
| **Reading** | The chain's next stage, AY-S4, is not startable: its cells encode AY-Q2 and AY-Q3, both `[quiet-wrong]`, and AY-Q8, all unmarked. AY-S8 is not either: its exit reads a pre-AY stream and a torn run "as marked", and AY-Q6 and AY-Q7 are `[quiet-wrong]` and unmarked. S9 is independent of everything but S0 (`workorder-ay-following-letter.md` §5) and touches only the marked D7 (`raft-marks-2026-09-29.md` §3) |
| **Does not settle** | AY-S4, AY-S8, AY-S10 or any other stage; any §4 item; whether an auxiliary that fails its cell stays refused (the 2026-09-29 marks, §3) - the assertion passed its cells |
| **Recorded at** | `workorder-ay-following-letter.md` §6, `index.md` |

## 2. AY-Q2, AY-Q3 and AY-Q8 as proposed, and AY-S4 started

Recorded on `worktree-ay-s4-fk-cells-red` from `64b97e7`
(`v2.7.0-505-g64b97e7`), with AY-S9 on `main` at `64b97e7`.

| | |
|---|---|
| **Word** | *"push"* (AY-S9, `b5465a9..64b97e7`, the pre-push gate green); then *"follow CLA proposal for AY-Q2, AY-Q3, AY-Q8, start AY-S4"*; then, while CLA read the stage, *"[decision] [AY-Q2]: take S lock for parent relation first, then start to descent"*. Asked whether that meant `S` on the whole relation - which D9(a) as ratified does not say - the answer was **"IS rel + S tuple (Recommended)"** |
| **Mark** | **AY-Q2 as proposed**: `IS` on the parent relation, then `S` on the parent tuple, then the descent. **AY-Q3 as proposed**: the self-referencing arm takes the same `IS` + `S` per row. **AY-Q8 as proposed**: a parent `DELETE` meeting a child row whose writer holds no `S(P)` carries that writer's id out of the check and parks mid-walk, inside AY-S5, its cells in AY-S4. **AY-S4 starts** |
| **Answers** | Three of the order's `[quiet-wrong]`-tagged or user-visible items: AY-Q2 and AY-Q3 `[quiet-wrong]`, AY-Q8 user-visible |
| **Does not settle** | AY-S5 or any other stage; AY-Q1, AY-Q4-Q7, Q10-Q12 |
| **Recorded at** | `workorder-ay-following-letter.md` §4 and §6, `index.md` |

## 3. AY-Q3 restated

Recorded on `worktree-ay-s4-fk-cells-red` at `86b61b3`, during AY-S4's
review.

| | |
|---|---|
| **Word** | *"AY-Q3 as proposed: the self-referencing FK arm takes the hoist`s pair per row - IS on the relation, then S on the parent tuple, then the descent (AY-Q2`s order), held to decide. A busy parent is waited for, not refused; the wait is recorded as the insert`s own tuple borrow records its."* |
| **Mark** | §2's AY-Q3, made exact: per row, `IS` on the relation, `S` on the parent tuple, then the descent, both held to the decide; **a busy parent is waited for, not refused**, the wait recorded as the insert's own tuple borrow records its |
| **Answers** | What §2's "as proposed" left implicit: the order within the per-row arm, and what a busy self-referenced parent gets |
| **Does not settle** | AY-S5 or any other stage |
| **Recorded at** | `workorder-ay-following-letter.md` §4 and §6 |

## 4. AY-Q1 as proposed, and AY-S5 started

Recorded on `worktree-ay-s4-fk-cells-red` at `86b61b3`, with AY-S4 built on
that branch and not yet pushed.

| | |
|---|---|
| **Word** | *"follow CLA proposal for AY-Q1, start AY-S5"* |
| **Mark** | **AY-Q1 as proposed**: D9(a) is built as ratified, and `foreign-keys.md` states its three costs - the tuple `S` blocks every `UPDATE` of the parent row while a child writer is open, not only its `DELETE`; two transactions that each write a child of P and then update P deadlock and one is refused; a transaction referencing more than `max_locks_per_txn` distinct parents is refused (no escalation, AO-R10). An existence-only unit is not built; it is an AR2 unit change, put forward as its own item only if the cost is measured to matter. **AY-S5 starts**: D9(a) (AY-R4) |
| **Reading** | S5 follows S4 (§5 of the order: S4's cells before S5, so the fence lands against red). S4 is built on its branch and finishes first - its review, AY-Q3's busy-parent cell (§3 here), the suite - and S5 branches from S4's tip |
| **Does not settle** | AY-S6 or any later stage; AY-Q4-Q7, Q10-Q12 |
| **Recorded at** | `workorder-ay-following-letter.md` §4 and §6, `index.md` |

## 5. AY-Q4 as proposed

Recorded on `worktree-ay-s5-d9a-parent-fence` at `5aac081`, as AY-S5 opened.

| | |
|---|---|
| **Word** | *"AY-Q4 as proposed: DDL taking the parent relation`s X - CREATE/DROP INDEX, CREATE ASSERTION - waits for every open child writer`s held IS; a steady stream can refuse it TxnConflict at the `s net, as A0-0 item 25 accepts for DROP TABLE. Stated in foreign-keys.md and each DDL`s spec."* ("the `s net" read as the lock family's 1 s fault net, `lock_wait_fault_net_ms`; "A0-0 item 25" as AO-0 item 25) |
| **Mark** | **AY-Q4 as proposed**: a DDL taking the parent relation's `X` - `CREATE INDEX`, `DROP INDEX`, `CREATE ASSERTION` - waits for every open child writer's held `IS` on it, and a steady stream of child writers can refuse it `TxnConflict` at the 1 s net, as AO-0 item 25 accepts for `DROP TABLE`. Stated in `foreign-keys.md`, `index.md` and `assertion.md` |
| **Does not settle** | AY-S6 or any later stage; AY-Q5-Q7, Q10-Q12 |
| **Recorded at** | `workorder-ay-following-letter.md` §4 and §6 |

## 6. AY-Q8 restated

Recorded on `worktree-ay-s5-d9a-parent-fence` from `5aac081`, during AY-S5.

| | |
|---|---|
| **Word** | *"AY-Q8 as proposed: a parent DELETE whose reverse check meets an undecided child held by a writer with no S(P) - a child DELETE, or an UPDATE moving the fk column - parks mid-walk on that writer, the check carrying its trx id out, and re-checks at its decide. Built in AY-S5, its cell in AY-S4"* |
| **Mark** | §2's AY-Q8, made exact: the reverse check carries the undecided child writer's id out, the parent `DELETE` parks mid-walk on it, and the check runs again at that writer's decide |
| **Does not settle** | Any other stage or item |
| **Recorded at** | `workorder-ay-following-letter.md` §4 and §6 |
