# Ratification marks — 2026-10-08

**The operator's words of 2026-10-08**, recorded by CLA on
`worktree-bg-scan-ring-order` (§1) and on `worktree-checkpoint-reactor-workorder`
(§2-§5) and on `worktree-bb-s5-close` (§6) (`v2.7.0-*`; the v3.0.0 tag is not
cut). All of it is **verbatim**: the words as typed in the session.

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
| **Word** | *"close entry 작성 진행"* ("go ahead and write the close entry"), given after CLA reported that BB's close entry, `index.md`'s BB and BA rows, and BA's resumption were what remained |
| **Mark** | BB-S5 closes BB on the measurement it has (BD-Q12 (b)). The runs the detached BB-S5 job finished after the push of `dbeb876c` (C4's last four, C5, pinned `cores = 2` C3 and C4) are added to BB-S5's results file as measured. They were not run again. BB closes; BA is no longer paused |
| **Does not settle** | the push; BA's next stage (BA-S2), which waits for its own word; the open bug BB carries |
| **Recorded at** | `workorder-bb-issue-under-the-leaf.md`'s header and §6 ("BB-S5 - the close"); `workorder-ba-parallelism.md`'s header and §6; `index.md`'s BA and BB rows; `workorder-bd-sorted-leaf-named-keys.md`'s header ("BB and BA"); `bench/v3.0.0/results-bb-s5-overhead-v2.7.0-640-gb76261bb.md` |
