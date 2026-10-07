# Ratification marks — 2026-10-07

**The operator's words of 2026-10-07**, recorded by CLA on
`worktree-wal-recycling` (`v2.7.0-*`; the v3.0.0 tag is not cut). All of it
is **verbatim**: the words as typed in the session.

---

## 1. BC's order written

| | |
|---|---|
| **Word** | *"main을 동기화하고 먼저 wal 회수 기능에 대한 작업 지시서를 작성해줘"* |
| **Mark** | `workorder-bc-wal-recycling.md` is written, as BC-S0, on `worktree-wal-recycling` from `6dc792c9` |
| **Does not settle** | the letter's opening and its §4 items (§2 below) |
| **Recorded at** | the order's §6 |

## 2. BC opened, its seven items marked as proposed, and BC-S1 started

| | |
|---|---|
| **Word** | *"BC-Q0~Q6 제안대로 마킹하고 BC-S1 진행해줘"* |
| **Mark** | BC-Q0..Q6 are marked as CLA proposed them (`workorder-bc-wal-recycling.md` §4). **BC-Q0:** BC opens, with BC-S0..S5 and BC-R1..R6 as written. **BC-Q1 (a):** recycling does not wait for archiving; archiving, when built, adds its floor at BC-R1's one computation (CN-9 §9 O4 answered). **BC-Q2:** redo is floored at the anchor, **conditional on BC-S1's comparison being green**. **BC-Q3 (a):** segment reuse is out of BC. **BC-Q4:** no retention knob. **BC-Q5 (a):** the unlink runs on core 0's WAL writer thread, measured at the close at `cores > 1`. **BC-Q6:** `SHOW META`'s three fields. **BC-S1 starts** |
| **Does not settle** | any stage after BC-S1 - each waits for its own word; BC-Q2's condition, which BC-S1's result decides; the push; the v3.0.0 tag |
| **Recorded at** | the order's header, §4 and §6, CN-9 §9 O4, the index row |

## 3. BC's close-out measurement waived

| | |
|---|---|
| **Word** | *"해당 마일스톤에서는 측정은 예외적으로 생략할거야"* |
| **Mark** | **BC closes without §5's measurement**, as an exception to `CLAUDE.md`'s Session Workflow step 3 for this milestone. The interleaved A/B is not run, and BC-Q5 (a) stands without the `cores > 1` peer-latency check it was marked against |
| **Reading** | An exception for BC, not a change to the rule: the next milestone measures as before. Nothing BC lands may be reported as measured; BC-S5's report states the measurement as **not executed, waived by the operator** |
| **Does not settle** | BC-Q5's fallback to (b), which now has no measurement to trigger it; a later order may measure BC's change |
| **Recorded at** | the order's §5 and BC-S5 row, the index row |

## 4. BC-S2 started

| | |
|---|---|
| **Word** | *"리뷰 끝나면 커밋하고 BC-S2 진행해줘"* |
| **Mark** | BC-S1 is committed once its review is applied, and **BC-S2 starts**: the device (BC-R3, BC-R4) |
| **Does not settle** | BC-S3..S5's start; the push |
| **Recorded at** | the order's header and §6 |
