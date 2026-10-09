# Ratification marks — 2026-10-08

**The operator's words of 2026-10-08**, recorded by CLA on
`worktree-bg-scan-ring-order` (§1) and on `worktree-checkpoint-reactor-workorder`
(§2-§5) and on `worktree-bb-s5-close` (§6) and on `worktree-bh-purge-key`
(§7-§10) (`v2.7.0-*`; the v3.0.0 tag is not cut). All of it is **verbatim**: the words as typed in the session.

## 1. BG-Q9 marked as proposed

| | |
|---|---|
| **Word** | *"mark BG-Q9 as proposed"* |
| **Mark** | **BG-Q9 (a):** no floor under the scan queue (BG-R5). §5's per-day probes, run over a pool whose rest is warm, are the gate. A floor as a function of the capacity, (b), is written only if they re-fault |
| **Does not settle** | BG-S1's premise gate, where BG stops for a ruling; whether (b) is needed, which §5 decides |
| **Recorded at** | `workorder-bg-scan-queue.md`'s header (W2) and §4; `index.md`'s BG row |

## 2. BI's order written

| | |
|---|---|
| **Word** | *"ckdbs 최신 main을 clone하고 CLAUDE.md를 읽은 뒤, 작업 지시서 하나를 새로 작성해 push하라."* and *"주제: checkpoint가 코어의 reactor를 동기적으로 막는 문제를 없앤다."* |
| **Mark** | `workorder-bi-checkpoint-off-the-reactor.md` is written as BI-S0 on `worktree-checkpoint-reactor-workorder` from `65a28553`, reviewed, and not opened. No engine file moves. "Clone" was taken as a fresh worktree from `origin/main`, the Session Workflow's step 1 |
| **Does not settle** | BI-Q0..Q11 |
| **Recorded at** | the order's header and §6, `index.md`'s BI row, and the bug entry's *The fix* |

## 3. The checkpoint runs one Step per reactor task

| | |
|---|---|
| **Word** | *"(1) Step 하나를 reactor task 하나로 실행해 step 사이에 yield한다."* |
| **Mark** | BI-R1: the cadence callback only submits a `system`-group run task, which takes one `Step` and yields before the next. It is the operator's decision, not an item. The same proposal stood unmarked as BA-R10's third paragraph |
| **Does not settle** | BI-Q8, the reading of "one task" as one coroutine per run; BI-Q5, the anchor publish's whole-store writeback, which is not a `Step`; BI-Q1, whether BA-R10 gives this half up; BI-Q7, the gate's fairness once runs outlast `period / cores` |
| **Recorded at** | the order's header (W1) and BI-R1 |

## 4. The checkpoint's syncs leave the reactor, and the caller parks

| | |
|---|---|
| **Word** | *"(2) data file sync와 EnsureDurable은 writer/I/O thread로 넘기고, 호출한 task는 park한다(RequestSyncNow와 같은 모양)."* |
| **Mark** | BI-R2: the log gate's wait becomes `RequestSyncNow` plus a park on `IsDurable`, core 0's checkpoint included. The data-file sync becomes a request to a thread plus a park on its completion. It is the operator's decision, not an item |
| **Does not settle** | BI-Q2, which thread; BI-Q3, what a failed data-file sync does; BI-Q4, the per-run gate inside `WriteBack`; BI-Q6, whether the thread kicks the parked core |
| **Recorded at** | the order's header (W2) and BI-R2 |

## 5. BI's composition

| | |
|---|---|
| **Word** | *"S1은 측정만 한다. checkpoint 구간의 reactor 정지 시간과 같은 코어 다른 세션의 p99를 build-release로 잰다. 코드 변경은 계측뿐이다."*; *"세션 배치(BA-R13, CN-10 §3.4)와 core 0 commit drain의 inline fdatasync는 범위에서 뺀다."*; *"기존 작업 지시서와 같은 구조를 따르고, 모든 주장에 [source-read]/[design]/[measured] 태그를 붙인다."*; *"Q 항목은 전부 미마킹 상태로 둔다."* |
| **Mark** | BI-S1 is instrumentation and a `build-release` measurement, nothing else (§5.1). "BI does not do" lists session placement and `DrainOnce`'s inline `Sync()` (`manager.cpp:478`). Every claim in the order carries a tag; the operator's words and the rules that restate them carry none, being decisions. BI-Q0..Q11 have an empty mark column |
| **Does not settle** | BI-Q9, S1's premise gate |
| **Recorded at** | the order's header (W3-W5), §0 and §4 |

## 6. BB's close entry written

| | |
|---|---|
| **Word** | *"Go ahead and write the close entry"* (translated), given after CLA reported that BB's close entry, `index.md`'s BB and BA rows, and BA's resumption were what remained |
| **Mark** | BB-S5 closes BB on the measurement it has (BD-Q12 (b)). The runs the detached BB-S5 job finished after the push of `dbeb876c` (C4's last four, C5, pinned `cores = 2` C3 and C4) are added to BB-S5's results file as measured. They were not run again. BB closes; BA is no longer paused |
| **Does not settle** | the push; BA's next stage (BA-S2), which waits for its own word; the open bug BB carries |
| **Recorded at** | `workorder-bb-issue-under-the-leaf.md`'s header and §6 ("BB-S5 - the close"); `workorder-ba-parallelism.md`'s header and §6; `index.md`'s BA and BB rows; `workorder-bd-sorted-leaf-named-keys.md`'s header ("BB and BA"); `bench/v3.0.0/results-bb-s5-overhead-v2.7.0-640-gb76261bb.md` |

## 7. A deleted key admitted again - answered with the conflict, not marked

| | |
|---|---|
| **Word** | *"아냐 Invariant에 pk 값으로 오류가 나는 경우는 중복되거나 2^40-1이 모두 소진되었을때 뿐이니까 이미 삭제된 pk키값으로 삽입하는건 허용되어야해"* - no: under the invariant a pk is refused only as a duplicate or once 2^40 - 1 is exhausted, so an insert naming an already-deleted pk must be admitted |
| **Mark** | None. CLA answered with the conflict: K1 (`keystoneid-invariant.md:18-29`), invariant 11 and BD-R4 make a deleted key a duplicate for the life of its relation, and lifting that for every `DELETE` re-opens the hazard K1's first reason names. No file was written on this word |
| **Does not settle** | Anything: §8 replaced its direction |
| **Recorded at** | `workorder-bh-purge-key.md`'s header (W1) |

## 8. K1 kept; a statement distinct from `DELETE` frees a key

| | |
|---|---|
| **Word** | *"그러면 규칙을 폐기하지 말고 pk 정보 자체를 purge하는 다른 api를 작성하는것은 어떄? (delete와 구분 되는 구문이고 이 경우 재삽입이 가능함)"* - then, instead of discarding the rule, how about another API that purges the pk information itself (a statement distinct from DELETE, after which re-insertion is possible)? |
| **Mark** | The direction: K1 and `DELETE` stay as they are, and a separate statement frees a deleted key. CLA's reply proposed seven points: committed delete-marked rows only, a live row named refused `InvalidArgument`; older snapshots waited on, then `TxnConflict` at the 1 s fault net; autocommit only, one record per key with no rollback, and a superblock bump; `PURGE FROM t WHERE` pk equality or range; index entries left; heap relations `Unsupported`; an engine-internal purge still owes a keyed tombstone |
| **Does not settle** | Any item of the order; whether the order opens |
| **Recorded at** | `workorder-bh-purge-key.md`'s header (W2), §0 and BH-R1 |

## 9. BH's order written

| | |
|---|---|
| **Word** | *"제안 대로 작업 명세와 지시서를 작성해줘"* - write the specification and the work order as proposed |
| **Mark** | BH-S0: the order written on §8's seven proposals, with the specification as its §2 (PU1-PU12), and reviewed. Four proposals are revised by the survey and the review, each stated at its item: BH-Q2 (a range passes over a live key), BH-Q3 (the 1 s bound is BH's own constant, not the fault net), BH-Q5 (a refusal, not only a crash, can leave a window partly purged) and BH-Q6 (no record kind, so no superblock bump). BH-Q10..Q16 are new, and no word has seen them. **Not opened** |
| **Does not settle** | BH-Q0..Q16; BH-S1's start |
| **Recorded at** | `workorder-bh-purge-key.md`; `index.md`'s BH row |

## 10. BH opened, every item adopted as CLA proposed

| | |
|---|---|
| **Word** | *"go ahead until achiving milestone, I will follow CLA proposal if decision needed, keep going in this session"* (given as the `go-ahead-achieving-milestone` skill), then *"GO AHEAD"* |
| **Mark** | **Adopted on the standing go-ahead, not marked per item:** BH-Q0 yes (BH opens); BH-Q1 (a) `[quiet-wrong]`; BH-Q2 (b); BH-Q3 (a); BH-Q4 (a); BH-Q5 (a); BH-Q6 (a); BH-Q7 (a); BH-Q8 (a); BH-Q9 (a); BH-Q10 (a); BH-Q11 (a); BH-Q12 (a); BH-Q13 yes; BH-Q14 as written; BH-Q15 moot under BH-Q6 (a); BH-Q16 (a) `[quiet-wrong]`. **BH-Q17 (a)**, written into the order at BH-S1 when Census D found `DELETE` refuses a system relation only by accident, and adopted under the same word. Before adopting them, CLA re-read the premises of the two `[quiet-wrong]` items at `37a7a9cd`, and neither had moved (the order's header) |
| **Does not settle** | Any decision that surfaces mid-stage. Under the same word, each such decision is written into the order with CLA's proposal and adopted, never recorded as the operator's own mark. A push, a tag or a version still waits for the operator's word |
| **Recorded at** | `workorder-bh-purge-key.md`'s header, §5's mark column and §7; `index.md`'s BH row |
