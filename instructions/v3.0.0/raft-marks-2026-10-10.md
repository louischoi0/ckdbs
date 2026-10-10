# Ratification marks — 2026-10-10

**The operator's words of 2026-10-10**, recorded by CLA on
`worktree-bj-expression-update` (`v2.7.0-*`; the v3.0.0 tag is not cut).
Every word is recorded **in English**, translated where it was typed in
Korean.

## 1. Expression `UPDATE` wanted, and its eight rulings

| | |
|---|---|
| **Word** | *"kdbs has to support expression update now."* (W0), then, answering CLA's proposal item by item: *"The scope of expressions is the same as a general commercial DB."* (E1); *"Follow the SQL standard."* (E2, every SET expression reads the row as it was before the statement changed it); *"No implicit conversion."* (E3); *"Follow the standard."* (E4, NULL); *"No pk update."* (E5); *"The expression's structure (operators, referenced columns) is part of the statement's shape, and a literal is treated as a value."* (E6); *"No measurement gate; the overhead is measured."* (E7); *"Write CN-7 to match the changed spec (not deleted, not closed)."* (E8) |
| **Mark** | Decisions, not items: BJ-R1 (E1), BJ-R2 (E2), BJ-R3 (E3), BJ-R4 (E4), BJ-R5 (E5), BJ-R6 (E6), BJ-R7 (E7), BJ-R8 (E8). A SET value becomes a value expression; no implicit conversion; NULL and before-image per the standard; the pk is read, never written; structure is shape and a literal is a value; the overhead is measured once and gates nothing; CN-7 is restated and stays a concept note |
| **Does not settle** | BJ-Q0..Q12, among them the reference dialect (BJ-Q1), what "one type" is under E3 (BJ-Q2) and the minus (BJ-Q9) |
| **Recorded at** | `workorder-bj-expression-update.md`'s header (W0-W8) and §2; `docs/conceptnotes/cn7-delta-verbs.md` |

## 2. BJ's order written

| | |
|---|---|
| **Word** | *"Proceed with option 1."*, given after CLA reported that none of BJ-S0's deliverables existed on `main` at `0446b3b0` and offered three actions: sync BJ-S0 into the tree, fix BJ-Q12 first, or something else |
| **Mark** | BJ-S0: the order written on §1's eight rulings, `raft-marks-2026-10-10.md` (this file), `index.md`'s BJ row, CN-7 revised per BJ-R8, and the bug entry BJ-Q12 refers to, filed at `docs/inflight/bugs/a-column-assigned-twice-checks-its-first-foreign-key-value-and-writes-its-last.md`. **Not opened**: BJ-Q0 is unmarked |
| **Does not settle** | BJ-Q0..Q12, among them BJ-Q12 (the duplicate-target fix ahead of BJ), which the operator did not choose over option 1; BJ-S1's start; any push |
| **Recorded at** | `workorder-bj-expression-update.md` §6; `index.md`'s BJ row |

## 3. BJ opened; every item adopted as CLA proposed (one revised)

| | |
|---|---|
| **Word** | *"apply it to workplan after review and /go-ahead-achieving-milestone"* (the `go-ahead-achieving-milestone` skill, whose standing word is *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session"*) |
| **Mark** | **Adopted on the standing go-ahead, not marked per item:** BJ-Q0 yes (BJ opens); BJ-Q1 PostgreSQL 18; BJ-Q2 **(b), revised from CLA's (a)** (W3's letter); BJ-Q3 (a) and (c) `[quiet-wrong]`; BJ-Q4 (a); BJ-Q5 as listed; BJ-Q6 as revised by the review `[quiet-wrong]`; BJ-Q7; BJ-Q8; BJ-Q9 (a); BJ-Q10 as revised by the review `[quiet-wrong]`; BJ-Q11; BJ-Q12 the fix first `[quiet-wrong]`. Before adopting them, CLA re-read the premises of the four `[quiet-wrong]` items at `080cd555`; BJ-Q6 and BJ-Q10 had moved under the review and were revised |
| **Does not settle** | Any decision that surfaces mid-stage. Under the same word, each such decision is written into the order with CLA's proposal and adopted, never recorded as the operator's own mark. A push, a tag or a version still waits for the operator's word. BJ-S4 still waits for BA's code on `main` (the order's §3), which no adoption changes |
| **Recorded at** | `workorder-bj-expression-update.md`'s header, §4's mark column and §6; `index.md`'s BJ row |
