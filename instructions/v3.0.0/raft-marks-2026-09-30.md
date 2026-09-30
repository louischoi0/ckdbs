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
