# Ratification marks — 2026-10-14

**The operator's words of 2026-10-14**, recorded by CLA on
`worktree-cn4-transition-refresh` from `b0bc959c`. Each word is recorded
**in English**, translated from the Korean it was typed in.

## 1. CN-4 re-read

| | |
|---|---|
| **Word** | *"First fix the note to match the present."* Then, asked whether to rewrite the body or annotate it under `docs/conceptnotes/README.md`'s rule, the operator chose the annotation |
| **Mark** | CN-4's body stays as written at `d2e123d`. Its §9 records what the tree has moved since, at `b0bc959c` |
| **Does not settle** | Anything in CN-4 §8 |
| **Recorded at** | `docs/conceptnotes/cn4-transition-constraints.md` header and §9 |

## 2. CN-4 opened; BV written under v3.2.0

| | |
|---|---|
| **Word** | *"Now mark that concept note [open] and write a work order that carries the work plan against the current code. Mark it v3.2.0."* |
| **Mark** | CN-4 is `[open]` and cites BV. `instructions/v3.2.0/` opens with `workorder-bv-transition-constraints.md`. The operator names the version v3.2.0; no tag is taken |
| **Does not settle** | BV-Q0..Q5; CN-4 T1–T8 and T10; a tag |
| **Recorded at** | `workorder-bv-transition-constraints.md` header; `index.md`; CN-4's status line |

## 3. BV-Q1 marked (a)

| | |
|---|---|
| **Word** | *"Q1: $!a"* |
| **Mark** | **BV-Q1 (a)**: `sys.transitions` is a new row-codec catalog relation. Oid 146, column oids from 147, root page 16, overflow from 17, superblock 24 refusing 23 |
| **Does not settle** | Every other BV item |
| **Recorded at** | `workorder-bv-transition-constraints.md` header (W2) and §4 |

## 4. BV to its close on the standing go-ahead

| | |
|---|---|
| **Word** | The `go-ahead-achieving-milestone` skill, invoked on BV: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* |
| **Mark** | BV runs every stage to its close in this session. The following are adopted as CLA proposed and recorded as *"adopted on the operator's standing go-ahead of 2026-10-14 (go-ahead-achieving-milestone)"*: BV-Q0 (stage by stage, in §3's order); BV-Q2 (§0.1's grammar); BV-Q3 (`TRANSITION_VIOLATION`, wire category 18); BV-Q4 (a) (`PURGE` refused on a relation carrying a Transition); BV-Q5 (a CN-3 successor meets the initial set); CN-4 T1–T8 and T10 as CN-4 §8 proposes. None of these is the operator's own per-item mark |
| **Does not settle** | A push, a merge to `main`, a tag |
| **Recorded at** | `workorder-bv-transition-constraints.md` header (W3), §4 and §6; `index.md` |

_Sections 5-7 recorded by CLA on `worktree-bw-instant-add-drop-column` from `750496b1`._

## 5. BW written

| | |
|---|---|
| **Word** | *"I want to implement ALTER TABLE ADD/DROP now. The core is to guarantee the action takes effect as instantly as possible, while keeping the invariants"*, with the spare-slot idea (`workorder-bw-instant-add-drop-column.md` W0) |
| **Mark** | BW written; D-a..D-f marked (a) by the operator: *"D-a $!#1, D-b $!#1, D-c $!#1, D-d $!#1, the rest as proposed"* (W1) |
| **Recorded at** | `workorder-bw-instant-add-drop-column.md` header |

## 6. BW's measurement held

| | |
|---|---|
| **Word** | *"In this milestone skip every per-stage measurement, and hold the milestone measurement until directed."* (W2) |
| **Mark** | No BW stage measures; BW-S5 closes without the overhead measurement unless the operator directs it. This word is specific to BW and outranks `go-ahead-achieving-milestone` §4's close measurement |
| **Recorded at** | `workorder-bw-instant-add-drop-column.md` header and §5 |

## 7. BW opened on the standing go-ahead

| | |
|---|---|
| **Word** | `/go-ahead-achieving-milestone`: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* (W3) |
| **Mark** | **BW-Q1..Q7 adopted as CLA proposed, on the operator's standing go-ahead of 2026-10-14 (go-ahead-achieving-milestone)** - not marked per item by the operator. BW-Q3 `[quiet-wrong]`: its premise (one payload producer, `EncodeRow`) re-read at `750496b1` before adoption and holds |
| **Recorded at** | `workorder-bw-instant-add-drop-column.md` header and §4; `index.md`'s BW row |
