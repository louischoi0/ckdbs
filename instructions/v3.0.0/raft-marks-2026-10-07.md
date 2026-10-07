# Ratification marks — 2026-10-07

**The operator's words of 2026-10-07**, recorded by CLA on
`worktree-wal-recycling` (§1-§6) and `worktree-bd-sorted-leaf-named-keys` (§7-§13) (`v2.7.0-*`; the v3.0.0 tag is not cut). All of it
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

## 5. BC-S4's defect: investigated and fixed by fail-stop

| | |
|---|---|
| **Word** | *"(a)로 진행해줘"*, then the answer *"Fail-stop (Recommended)"* to CLA's question of the fix |
| **Mark** | The refused reboot BC-S4's simulator found is investigated and fixed before BC-S4 closes. **The fix is fail-stop**: after a refused record the log refuses every write until a restart (`docs/spec/wal.md` §6-5) |
| **Does not settle** | whether a failed device *sync* stops the log too (`known-gaps.md`, WAL); the status a commit refused after the stop reports |
| **Recorded at** | the order's §6 (BC-S4) |

## 6. BC-S5 started, and the push

| | |
|---|---|
| **Word** | *"리뷰 끝나면 커밋하고 BC-S5 진행해줘"*, then *"BC-S5 끝나면 커밋하고 main에 push해줘"* |
| **Mark** | BC-S4 is committed after its review; BC-S5 is built, reviewed and committed, and the branch is pushed to `main` |
| **Does not settle** | the v3.0.0 tag |
| **Recorded at** | the order's header and §6 |


## 7. BD's order written, reviewed and pushed

| | |
|---|---|
| **Word** | *"kUnordered를 삭제하면서도 high water mark 오류가 발생하지 않도록 하는 방법이 있지 않니?"*; *"순서를 어기는 삽입이 왜 필요하지? 낮은 key를 가진 leaf를 삽입할때 항상 끝쪽이 아닌 알맞은 순서에 넣으면 되잖아"*; *"kUnordered를 삭제하면서, 별도 플래그를 두지 않으면서, btree를 항상 정렬되게 유지하면서도 high water mark 오류는 없어야해."*; *"해당 Invariant 변경에 대한 작업 지시서를 작성해줘, 추가로 named_pk 삽입이 pk때문에 실패하는 이유는 2가지 뿐이어야해. 중복되거나, 소진되었거나."*; *"high water mark (pk=100을 먼저 쓰면 pk=99 삽입이 안되는 현상이 있으면 안돼"*; *"작업 지시서만 작성해서 push해"*; *"리뷰 끝나면 반영해서 push해"* |
| **Mark** | `workorder-bd-sorted-leaf-named-keys.md` is written as BD-S0 on `worktree-bd-sorted-leaf-named-keys` from `bf8ea937`. Its `critics-developer` review is applied (the order's §6), and it is pushed. The order alone moves; no engine file does. A btree places a named key where it sorts, and a named key's pk is refused for exactly two reasons (BD-R5) |
| **Does not settle** | BD's opening (BD-Q0); BD-Q10, BD-Q11 and BD-Q12, which the review raised |
| **Recorded at** | the order's header (W1-W3, W5, W8, W11, W14) and §6 |

## 8. BD-Q1 marked (a)

| | |
|---|---|
| **Word** | *"(a)를 선택할거야."* - to CLA's (a), shift the slot directory and change what addresses a slot, against (b), fixed slots and a key-order array |
| **Mark** | BD-Q1 (a): BD-R2's sorted placement, and BD-R3's shift-safe holders |
| **Does not settle** | how recovery undo re-finds a row (BD-Q8, §13) |
| **Recorded at** | the order's header (W4) and §4 |

## 9. BD-Q2 marked as proposed, then revised: rolled-back keys are reused

| | |
|---|---|
| **Word** | *"tombstone을 사용하는 것은 CLA제안에 따라서 구성"*, then *"롤백된 아이디는 당연히 재사용되어야해"* |
| **Mark** | BD-Q2 as CLA proposed it, with the second word overriding its rolled-back half. An id is consumed when its tuple commits. Today the delete-marked row is the tombstone, since nothing purges a user relation. A rolled-back key is free and may be named again. A purge, when one is built, owes a keyed tombstone (BD-R4). K1 now reads "bound to at most one committed tuple", restated at BD-S5 |
| **Does not settle** | a purge; the second path into the equal-index-entries bug, which is recorded in the order's §1.7 and not fixed |
| **Recorded at** | the order's header (W6, W12), §1.7, BD-R4 and §4 |

## 10. BD-Q3 marked (a)

| | |
|---|---|
| **Word** | *"(a)로 진행해"* - to CLA's reading of the two reasons. (a): duplicate is a slot keyed `k` (`AlreadyExists`), and exhausted is outside `[1, 2^40 - 1]` (`OutOfRange`, with today's `InvalidArgument` folded in). (b): present-now against used-before, with out-of-space values left `InvalidArgument` |
| **Mark** | BD-Q3 (a), as BD-R5 states it |
| **Does not settle** | an undecided insert of the same key, which §9's later word makes a wait rather than a duplicate (BD-Q9, §13) |
| **Recorded at** | the order's header (W7) and §4 |

## 11. Superblock backward compatibility is never kept - a standing order

| | |
|---|---|
| **Word** | *"superblock 하위호환성은 필요없어. 내가 따로 말하기전까진 이러한 사항을 항상 반영해"* |
| **Mark** | **Standing until the operator says otherwise:** a change to an on-disk meaning bumps `kSuperBlockVersion` and refuses every older volume, with no migration, no legacy reader, and no item asking whether to keep one. For BD it is BD-Q5: superblock 20, with version 19 refused (BD-R8) |
| **Does not settle** | nothing further; it is the default from here |
| **Recorded at** | the order's header (W9), BD-R8 and §4 |

## 12. BB-R1 deleted, and BD's measurement waived

| | |
|---|---|
| **Word** | *"BB-R1 은 삭제해. 성능 측정은 skip"* |
| **Mark** | BD-Q6: BB-R1 is withdrawn on a btree, and the issue and the admission leave the leaf's hold (BD-R6). BD-Q7: BD's close-out measurement is waived, so nothing BD lands is measured (BD-R10) |
| **Does not settle** | the median divide BB-R1's deletion brings back at `cores > 1` (BD-Q11); BA's rows, rebased per BD-Q12 |
| **Recorded at** | the order's header (W10), BD-R6, BD-R10 and §4 |

## 13. BD-Q4, BD-Q8 and BD-Q9 marked as proposed

| | |
|---|---|
| **Word** | *"BD-Q4, Q8, Q9 제안대로 마킹해줘"* |
| **Mark** | **BD-Q4 (a):** the heap is untouched. BB-R7, its two entry points, the mark's refusal and its ordinal mark stay; the cost is a second insert protocol kept alive for a relation type no mountable volume holds. **BD-Q8 (a):** recovery undo re-finds a row by scanning the recorded `kBtreeLeaf` and rightward while `min_key <= pk`. **BD-Q9 (a):** a named key meeting an undecided insert of the same key waits for its decide, and the re-run answers by the outcome. The word came before BD-S0's review, which sharpened all three without changing which option they are |
| **Does not settle** | BD-Q0, BD-Q10, BD-Q11 and BD-Q12 |
| **Recorded at** | the order's header (W13) and §4 |
