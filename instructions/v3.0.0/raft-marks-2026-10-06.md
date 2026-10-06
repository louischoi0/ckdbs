# Ratification marks — 2026-10-06

**The operator's words of 2026-10-06**, recorded by CLA on
`worktree-aq-s0-order` from `3650e474` (`v2.7.0-616-g3650e474`; the v3.0.0
tag is not cut). All of it is **verbatim**: the words as typed in the
session.

---

## 1. AQ's order written

| | |
|---|---|
| **Word** | *"먼저 AQ에 대한 작업지시서를 작성해줘"* |
| **Mark** | `workorder-aq-expression-cabins.md` is written, as AQ-S0, on `worktree-aq-s0-order` from `740d954a` |
| **Does not settle** | the letter's opening and its §4 items (§2 below) |
| **Recorded at** | the order's §6 |

## 2. AQ opened, and its seven items marked as proposed

| | |
|---|---|
| **Word** | *"CLA 제안대로 진행하고 main에 push해줘"* |
| **Mark** | AQ-Q0..Q6 are marked as CLA proposed them (`workorder-aq-expression-cabins.md` §4). **AQ-Q0:** AQ opens. **AQ-Q1:** D7's mechanism is a v2 `SysCabinRow` recognised by its length - v1 rows read as column Cabins, with no rebuild and no superblock move - and `F`'s text lives in a new user-format `sys.cabin_exprs`. **AQ-Q2:** `expr_id` is computed over column positions, and `alter.md` states why that diverges from AL3. **AQ-Q3:** the predicate shape admits conjunctions of literal comparisons, plain or `kImmutable` function, matched conjunct for conjunct on the canonical lowered form, with no normalisation. **AQ-Q4:** cover serving is for `DATE` only, both-sided ranges, all or nothing, capped by `cabin_max_values`; monotone serving and D3's sortable directory go to a later letter. **AQ-Q5:** automatic creation of expression and predicate Cabins is not in AQ. **AQ-Q6:** D1's fold stays deferred |
| **Amends** | AP-R4 item 2's "never ... a Cabin probe" for the one case AQ-R5 states. AR1 D7 is supplied with its mechanism, not changed |
| **Does not settle** | any AQ stage's start - each waits for its own word; AR's letter; the v3.0.0 tag. The marks were given while AQ-S0's review was still running: a review finding that would change a marked ruling is put back to the operator, not applied |
| **Recorded at** | the order's header and §6, AR1's status line, the index row |

## 3. BA-S1c started

| | |
|---|---|
| **Word** | *"BA-S1c 진행해줘"*, recorded on `worktree-ba-s1c-strict-marker-snapshot` from `dfabae1` |
| **Mark** | **BA-S1c starts**: the fix for defect B's own-session case (BA-R1c), built per `workorder-ba-parallelism.md` §6 |
| **Reading** | BA-S1c is one of the stages BA-Q1 exempts from the census, as BA-S1 was (`raft-marks-2026-10-02.md` §6). It builds BA-R1c and marks nothing in §4 |
| **Does not settle** | BA-Q0..Q14 - BA-Q3 included, so the other-session remainder stays in the bug entry; any other BA stage's start; the push |
| **Recorded at** | `workorder-ba-parallelism.md` §6, the index row |

## 4. BA-Q14 marked (b), BA paused, and BB's order written

| | |
|---|---|
| **Word** | *"BA-S1b 진행해줘"*; then, to CLA's question on BA-Q14, the answer **"(a) key_order 뒤집기 (Recommended)"**; then *"일단 여기서 두 코어가 올바르지 않은 순서로 삽입을 하는것을 원천 차단 하는 방식으로 해야 하지 않을까? unordered된 btree라면 존재 이유가 없잖아"*, *"그러면 뒤집힌 unordered단위는 btree전체이니? 아니면 leaf tree에 국한되는것이니?"*, *"그러면 이러한 케이스가 발생하는 경로를 설명해줘"*, and *"(b)로 원천 차단하는 방식으로 진행하려고 해 일단 지금까지 마일스톤 작업은 잠시 중지하고 이 오류를 해결하기 위한 서브 마일스톤 작업  계획 & 지시서를 작성해줘"* |
| **Mark** | **BA-Q14 is (b)**: the id is issued under the hold of the leaf it lands on. The earlier answer (a) is superseded by the last word. **BA is paused**; a sub-milestone takes defect A, and its order is written: `workorder-bb-issue-under-the-leaf.md` (BB) |
| **Reading** | "원천 차단" is read as BB-R1's rule - no placement out of key order while `key_order` is `kAscending`. Whether it also withdraws `kUnordered` - a named key below the mark, the btree's licence to take keys in any order - is not read into the words; it is put to the operator as BB-Q8. The pause covers BA's stages; BA-S1c, already pushed at `bddd450c`, stands |
| **Does not settle** | BB-Q0..Q8 - BB's opening included; any BB stage's start; BA-Q0..Q13 |
| **Recorded at** | BA's header, its BA-S1b and BA-Q14 rows and §6; BB's header; the index |
