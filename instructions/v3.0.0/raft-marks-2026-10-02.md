# Ratification marks — 2026-10-02

**The operator's words of 2026-10-02**, recorded by CLA on
`worktree-az-q3-release-any-failed-check` from `2e8d213`
(`v2.7.0-571-g2e8d213`; the v3.0.0 tag is not cut - `raft-marks-2026-09-26.md`
§4, Q3). All of it is **verbatim**: the words as typed in the session.

---

## 1. AZ pushed to `main`

| | |
|---|---|
| **Word** | *"push it to main"* |
| **Mark** | AZ - AZ-S2..S5 and the close AZ-S7, merged on `worktree-az-s7-close` - lands on `main` at `2e8d213`, through the pre-push hook |
| **Does not settle** | anything AZ-S7's row carries forward |
| **Recorded at** | here |

## 2. A failed foreign-key check gives back the parent's `S`, whoever took it

| | |
|---|---|
| **Word** | *"give back the S on any failed check, as the review proposed"* |
| **Mark** | **AZ-R5 is amended** (`workorder-az-ay-carry-forward.md`): on `FK_VIOLATION` the parent row's `S` is given back whoever took it - this statement's ask, an earlier statement's, or this statement's own run before it parked. The relation's `IS` still stays. AZ-S5's review proposed it (its row, §6) |
| **Reading** | An `S` on a parent the check reads as absent protects no row. While any `S` is held no other transaction can take the row's `X`, so the parent was absent at every grant, or this transaction deleted it under its own `X`, which the release does not touch. It closes the parked-statement gap AZ-S5's review found, and deletes the took-it test (`TookShare`, `LockHoldings::LastIs`) and the hoist's asked set |
| **Does not settle** | anything else in D9(a) - the `S` a passing check takes is held to the decide as before |
| **Recorded at** | `workorder-az-ay-carry-forward.md` §6, `foreign-keys.md` §2c, `known-gaps.md` (the entry deleted) |
