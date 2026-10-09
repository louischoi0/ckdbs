# Ratification marks — 2026-10-02

**The operator's words of 2026-10-02**, recorded by CLA on
`worktree-az-q3-release-any-failed-check` from `2e8d213`
(`v2.7.0-571-g2e8d213`; the v3.0.0 tag is not cut - `raft-marks-2026-09-26.md`
§4, Q3). Every word is recorded **in English**, translated where it was typed in Korean.

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

## 3. AP's order written

| | |
|---|---|
| **Word** | *"b"*, answering CLA's three options with *"Write AP's work order (AP-S0) from AR1 §14"* |
| **Mark** | `workorder-ap-function-catalog-fetch-id.md` is written: AP-S0 on `worktree-ap-s0-order` from `4012617`, reviewed at `8a1c017` |
| **Does not settle** | the letter's opening and its §4 items (§4 below) |
| **Recorded at** | the order's §6 |

## 4. AP opened, and its six items marked as proposed

| | |
|---|---|
| **Word** | *"proceed as CLA proposed and push to main"* |
| **Mark** | AP-Q0..Q5 are marked as CLA proposed them (`workorder-ap-function-catalog-fetch-id.md` §4). **AP-Q0:** AP opens, and the trail's re-key to `fetch_id` is AP's, not AR's "Waystone re-key". **AP-Q1:** the first functions are `DATE(timestamp) → date` (`kImmutable`, with a cover) and `NOW()` (`kStable`), on the column side of a WHERE conjunct only; no `kVolatileRow` function ships. **AP-Q2 (b):** the existing row is keyed by `fetch_id` and `kFingerprintVersion` moves 1 → 2. **AP-Q3:** `InvalidArgument` for a name not in the catalog. **AP-Q4:** the wire's `pattern_id` stays the statement's. **AP-Q5:** D1's fold is deferred to AQ |
| **Amends** | AR1 D6 ("`SysPatternRow` gains `fetch_id`") and AR1-3's "`kFingerprintVersion` does not move", both by AP-Q2 (b); AR1-2's D1 fold is deferred to AQ by AP-Q5. The `pattern_id` values do not move |
| **Does not settle** | any AP stage's start - each waits for its own word; AQ's and AR's letters; the v3.0.0 tag, which waits on M4 |
| **Recorded at** | the order's §4 and §6, AR1's status line, the index row |

## 5. Every absent parent the failed statement resolved gives back its `S` too

| | |
|---|---|
| **Word** | *"push it to main"*, then *"give back the S on every absent parent too"* |
| **Mark** | §2 lands on `main` at `ca473a4`. **AZ-R5 is amended again**: a failed check gives back the `S` on every parent row the statement resolved as absent, not only the failing check's - the hoist resolves every row's parents before any row is written, so the others' rows are never reached. A present parent's `S` and the relation's `IS` stay. The review of §2 recorded it (`known-gaps.md`, Foreign keys, at `7ce9718`) |
| **Reading** | §2's argument, applied to each: an `S` on a parent the check reads as absent protects no row |
| **Does not settle** | anything else in D9(a) |
| **Recorded at** | `workorder-az-ay-carry-forward.md` §6, `foreign-keys.md` §2c, `known-gaps.md` (the entry deleted) |

## 5. A function on the value side too

| | |
|---|---|
| **Word** | answering *"How should AP-S4 treat NOW()?"* with *"Value side too (Recommended)"* |
| **Mark** | **AP-Q1 is amended** (`workorder-ap-function-catalog-fetch-id.md`). A function call may stand on either side of a WHERE comparison: `F(col, …) op value`, `col op F(…)` and `F(…) op F(…)`. So `ts < NOW()` is written as it reads. Every such conjunct lowers to the function-conjunct residual kind of AP-R4 item 2. It is never a step key, a bound, an index key, a join key, a `BuildKey` or a Cabin probe, including when `col` is the pk. |
| **Reading** | Under the first mark `NOW()`, which takes no column, could be written only as `NOW() op literal`. That compares the clock with a constant and filters no row. The value side is what makes `NOW()` the D1 example AR1 §3 names. |
| **Does not settle** | anything else in AP-Q1 - the first functions are still `DATE(timestamp)` and `NOW()`, and still WHERE only |
| **Recorded at** | the order's AP-R4 and its AP-S4 row |

## 6. BA's order on `main`, and BA-S1 started

| | |
|---|---|
| **Word** | *"push it"*, then *"go ahead with BA-S1"*, then *"go ahead"* |
| **Mark** | BA's order lands on `main` at `e7617b2`: `workorder-ba-parallelism.md` with BA-S0 and its review, and the three bug entries. The first push passed the gate (3,091 tests) but was refused because `main` had moved during the run. The merge with AP-S4 passed the gate again (3,108 tests) and went in. **BA-S1 starts**: the fix for defect C, the peer-thread sync |
| **Reading** | BA-S1 is one of the stages BA-Q1 exempts from the census. A defect's fix needs no other §4 item, so starting it marks none of them |
| **Does not settle** | BA-Q0..Q14, the letter included; any other BA stage's start - each waits for its own word |
| **Recorded at** | `workorder-ba-parallelism.md` §6 |
