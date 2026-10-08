# Ratification marks — 2026-10-08

**The operator's words of 2026-10-08**, recorded by CLA on
`worktree-bg-scan-ring-order` (§1) and `worktree-bh-purge-key` (§2-§4)
(`v2.7.0-*`; the v3.0.0 tag is not cut). All of it is **verbatim**: the
words as typed in the session.

## 1. BG-Q9 marked as proposed

| | |
|---|---|
| **Word** | *"mark BG-Q9 as proposed"* |
| **Mark** | **BG-Q9 (a):** no floor under the scan queue (BG-R5). §5's per-day probes, run over a pool whose rest is warm, are the gate. A floor as a function of the capacity, (b), is written only if they re-fault |
| **Does not settle** | BG-S1's premise gate, where BG stops for a ruling; whether (b) is needed, which §5 decides |
| **Recorded at** | `workorder-bg-scan-queue.md`'s header (W2) and §4; `index.md`'s BG row |

## 2. A deleted key admitted again - answered with the conflict, not marked

| | |
|---|---|
| **Word** | *"아냐 Invariant에 pk 값으로 오류가 나는 경우는 중복되거나 2^40-1이 모두 소진되었을때 뿐이니까 이미 삭제된 pk키값으로 삽입하는건 허용되어야해"* - no: under the invariant a pk is refused only as a duplicate or once 2^40 - 1 is exhausted, so an insert naming an already-deleted pk must be admitted |
| **Mark** | None. CLA answered with the conflict: K1 (`keystoneid-invariant.md:18-29`), invariant 11 and BD-R4 make a deleted key a duplicate for the life of its relation, and lifting that for every `DELETE` re-opens the hazard K1's first reason names. No file was written on this word |
| **Does not settle** | Anything: §3 replaced its direction |
| **Recorded at** | `workorder-bh-purge-key.md`'s header (W1) |

## 3. K1 kept; a statement distinct from `DELETE` frees a key

| | |
|---|---|
| **Word** | *"그러면 규칙을 폐기하지 말고 pk 정보 자체를 purge하는 다른 api를 작성하는것은 어떄? (delete와 구분 되는 구문이고 이 경우 재삽입이 가능함)"* - then, instead of discarding the rule, how about another API that purges the pk information itself (a statement distinct from DELETE, after which re-insertion is possible)? |
| **Mark** | The direction: K1 and `DELETE` stay as they are, and a separate statement frees a deleted key. CLA's reply proposed seven points: committed delete-marked rows only, a live row named refused `InvalidArgument`; older snapshots waited on, then `TxnConflict` at the 1 s fault net; autocommit only, one record per key with no rollback, and a superblock bump; `PURGE FROM t WHERE` pk equality or range; index entries left; heap relations `Unsupported`; an engine-internal purge still owes a keyed tombstone |
| **Does not settle** | Any item of the order; whether the order opens |
| **Recorded at** | `workorder-bh-purge-key.md`'s header (W2), §0 and BH-R1 |

## 4. BH's order written

| | |
|---|---|
| **Word** | *"제안 대로 작업 명세와 지시서를 작성해줘"* - write the specification and the work order as proposed |
| **Mark** | BH-S0: the order written on §3's seven proposals, with the specification as its §2 (PU1-PU12), and reviewed. Four proposals are revised by the survey and the review, each stated at its item: BH-Q2 (a range passes over a live key), BH-Q3 (the 1 s bound is BH's own constant, not the fault net), BH-Q5 (a refusal, not only a crash, can leave a window partly purged) and BH-Q6 (no record kind, so no superblock bump). BH-Q10..Q16 are new, and no word has seen them. **Not opened** |
| **Does not settle** | BH-Q0..Q16; BH-S1's start |
| **Recorded at** | `workorder-bh-purge-key.md`; `index.md`'s BH row |

## 5. BH opened, every item adopted as CLA proposed

| | |
|---|---|
| **Word** | *"go ahead until achiving milestone, I will follow CLA proposal if decision needed, keep going in this session"* (given as the `go-ahead-achieving-milestone` skill), then *"GO AHEAD"* |
| **Mark** | **Adopted on the standing go-ahead, not marked per item:** BH-Q0 yes (BH opens); BH-Q1 (a) `[quiet-wrong]`; BH-Q2 (b); BH-Q3 (a); BH-Q4 (a); BH-Q5 (a); BH-Q6 (a); BH-Q7 (a); BH-Q8 (a); BH-Q9 (a); BH-Q10 (a); BH-Q11 (a); BH-Q12 (a); BH-Q13 yes; BH-Q14 as written; BH-Q15 moot under BH-Q6 (a); BH-Q16 (a) `[quiet-wrong]`. Before adopting them, CLA re-read the premises of the two `[quiet-wrong]` items at `37a7a9cd`, and neither had moved (the order's header) |
| **Does not settle** | Any decision that surfaces mid-stage. Under the same word, each such decision is written into the order with CLA's proposal and adopted, never recorded as the operator's own mark. A push, a tag or a version still waits for the operator's word |
| **Recorded at** | `workorder-bh-purge-key.md`'s header, §5's mark column and §7; `index.md`'s BH row |
