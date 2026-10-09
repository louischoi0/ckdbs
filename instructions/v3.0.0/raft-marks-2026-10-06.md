# Ratification marks — 2026-10-06

**The operator's words of 2026-10-06**, recorded by CLA on
`worktree-aq-s0-order` from `3650e474` (`v2.7.0-616-g3650e474`; the v3.0.0
tag is not cut). Every word is recorded **in English**, translated where it
was typed in Korean.

---

## 1. AQ's order written

| | |
|---|---|
| **Word** | *"first, write the work order for AQ"* |
| **Mark** | `workorder-aq-expression-cabins.md` is written, as AQ-S0, on `worktree-aq-s0-order` from `740d954a` |
| **Does not settle** | the letter's opening and its §4 items (§2 below) |
| **Recorded at** | the order's §6 |

## 2. AQ opened, and its seven items marked as proposed

| | |
|---|---|
| **Word** | *"proceed as CLA proposed and push to main"* |
| **Mark** | AQ-Q0..Q6 are marked as CLA proposed them (`workorder-aq-expression-cabins.md` §4). **AQ-Q0:** AQ opens. **AQ-Q1:** D7's mechanism is a v2 `SysCabinRow` recognised by its length - v1 rows read as column Cabins, with no rebuild and no superblock move - and `F`'s text lives in a new user-format `sys.cabin_exprs`. **AQ-Q2:** `expr_id` is computed over column positions, and `alter.md` states why that diverges from AL3. **AQ-Q3:** the predicate shape admits conjunctions of literal comparisons, plain or `kImmutable` function, matched conjunct for conjunct on the canonical lowered form, with no normalisation. **AQ-Q4:** cover serving is for `DATE` only, both-sided ranges, all or nothing, capped by `cabin_max_values`; monotone serving and D3's sortable directory go to a later letter. **AQ-Q5:** automatic creation of expression and predicate Cabins is not in AQ. **AQ-Q6:** D1's fold stays deferred |
| **Amends** | AP-R4 item 2's "never ... a Cabin probe" for the one case AQ-R5 states. AR1 D7 is supplied with its mechanism, not changed |
| **Does not settle** | any AQ stage's start - each waits for its own word; AR's letter; the v3.0.0 tag. The marks were given while AQ-S0's review was still running: a review finding that would change a marked ruling is put back to the operator, not applied |
| **Recorded at** | the order's header and §6, AR1's status line, the index row |

## 3. BA-S1c started

| | |
|---|---|
| **Word** | *"go ahead with BA-S1c"*, recorded on `worktree-ba-s1c-strict-marker-snapshot` from `dfabae1` |
| **Mark** | **BA-S1c starts**: the fix for defect B's own-session case (BA-R1c), built per `workorder-ba-parallelism.md` §6 |
| **Reading** | BA-S1c is one of the stages BA-Q1 exempts from the census, as BA-S1 was (`raft-marks-2026-10-02.md` §6). It builds BA-R1c and marks nothing in §4 |
| **Does not settle** | BA-Q0..Q14 - BA-Q3 included, so the other-session remainder stays in the bug entry; any other BA stage's start; the push |
| **Recorded at** | `workorder-ba-parallelism.md` §6, the index row |

## 4. BA-Q14 marked (b), BA paused, and BB's order written

| | |
|---|---|
| **Word** | *"go ahead with BA-S1b"*; then, to CLA's question on BA-Q14, the answer **"(a) flip key_order (Recommended)"**; then *"shouldn't we, here, block at the source two cores inserting in the wrong order? an unordered btree has no reason to exist"*, *"then is the flipped unordered unit the whole btree? or is it confined to the leaf tree?"*, *"then explain the paths by which this case arises"*, and *"I intend to proceed with (b), blocking it at the source; pause the milestone work so far for now and write a sub-milestone work plan & work order to resolve this error"* |
| **Mark** | **BA-Q14 is (b)**: the id is issued under the hold of the leaf it lands on. The earlier answer (a) is superseded by the last word. **BA is paused**; a sub-milestone takes defect A, and its order is written: `workorder-bb-issue-under-the-leaf.md` (BB) |
| **Reading** | "blocking at the source" is read as BB-R1's rule - no placement out of key order while `key_order` is `kAscending`. Whether it also withdraws `kUnordered` - a named key below the mark, the btree's licence to take keys in any order - is not read into the words; it is put to the operator as BB-Q8. The pause covers BA's stages; BA-S1c, already pushed at `bddd450c`, stands |
| **Does not settle** | BB-Q0..Q8 - BB's opening included; any BB stage's start; BA-Q0..Q13 |
| **Recorded at** | BA's header, its BA-S1b and BA-Q14 rows and §6; BB's header; the index |

## 5. BB opened: `kUnordered` deleted, the rest as proposed, and the push

| | |
|---|---|
| **Word** | *"in BB-Q8, kUnordered itself must be deleted. Otherwise follow CLA's proposal, main push"* |
| **Mark** | **BB-Q8 is (b)**, against CLA's proposal (a): `kUnordered` is deleted, and a named key below the mark is refused on a btree as on a heap, so no relation is ever out of key order. **BB-Q0..Q7 are marked as CLA proposed them**: BB opens as BA's sub-milestone with BA paused until BB-S5 (Q0); named keys at or above the mark are in (Q1); the heap arm is (a), the rule under a re-checked tail's hold (Q2); an omitted pk encodes under the leaf's hold (Q3); the latch order and AR2-R2's amendment, conditional on BB-S1 (Q4); system relations out (Q5); the sim check (Q6); BB lands whatever it measures (Q7). **BB's order goes to `main`** |
| **Reading** | "otherwise" is read as BB-Q0..Q7, the items on the table when the word was given. The mark opens three questions it does not answer - what a mounted volume that already holds a `kUnordered` relation does, the code a refused named key gets, and `DESCRIBE`'s field - and they are written as BB-Q9..Q11 with CLA's proposals, not marked |
| **Does not settle** | BB-Q9..Q11; any BB stage's start; BA-Q0..Q13 |
| **Recorded at** | BB's header, its §2 (BB-R3 rewritten, BB-R10..R12 added), §3 (BB-S3b added), §4 and §6; the index row; the bug entry |

## 6. BB-Q9 marked (a): a volume holding a `kUnordered` relation is refused

| | |
|---|---|
| **Word** | *"BB-Q9: give up backward compatatibility, refuse it"* |
| **Mark** | **BB-Q9 is (a)**: a mounted volume that holds a `kUnordered` relation is refused at mount, naming each such relation (`Unsupported`). There is no legacy per-page emission, no re-sort at mount, and the flag is never read as ascending |
| **Reading** | "refuse it" is read as (a)'s refusal of a volume that holds such a relation, by a check at catalog load. "give up backward compatibility" is read as the reason (b) and (c) are declined, not as refusing **every** older volume through a superblock bump. That wider reading is a different mark, and it was put back to the operator rather than taken. **Confirmed by the operator:** *"yes, a is right"* - the scoped refusal, no superblock bump |
| **Does not settle** | BB-Q10, BB-Q11; any BB stage's start; the push of this record |
| **Recorded at** | BB's header, BB-R11, its §4 row, §5 and §6; the index row |

## 7. BB started whole: BB-Q10 and BB-Q11 as proposed, and a constraint on the close

| | |
|---|---|
| **Word** | *"read the BB milestone's work order and go ahead; for issues that arise, follow CLA's proposal; the btree must always be sorted and must not carry destructive overhead. Complete that milestone"*, recorded on `worktree-bb-issue-under-the-leaf` from `6dc792c9` (`v2.7.0-627-g6dc792c9`) |
| **Mark** | **Every BB stage starts**, BB-S1 through BB-S5, in §5's sequence and each through the Session Workflow up to the push gate: *"complete that milestone"* is the word each stage waited for. **BB-Q10 is (a)**: on a btree, a refused named key is `AlreadyExists` when present and `OutOfRange` when absent and below the mark; a heap keeps `OutOfRange` for both (BB-R12). **BB-Q11 is (a)**: `DESCRIBE`'s `key_order=` field is removed with the state it reported, and the `sys.tables` byte is reserved, written 0, and read only by BB-R11's mount check |
| **Reading** | *"for issues that arise, follow CLA's proposal"* is read as two things. The unmarked items on the table - BB-Q10, BB-Q11 - are marked as CLA proposed them. And a question BB raises while it runs is settled by CLA's proposal, recorded beside the stage that raised it with the alternatives it declined, instead of stopping BB for a word: that includes BB-S1's row, where an inversion was to stop BB and go to the operator. *"the btree must always be sorted"* is BB-R1 and BB-R3's invariant stated as the goal - every page's slot order is its key order, on every relation, at every core count - and is read as forbidding any arm that leaves a btree leaf out of key order, the deleted `kUnordered` included. *"must not carry destructive overhead"* is read as a constraint on BB's close, sharper than BB-Q7: BB still lands the wrong answer it closes, but a cost BB-R8 resolves is not landed as a number in a results file and a deferral to BA-S11. At `cores = 1`, where BB reorders work and adds none (§1.9), BB is to cost nothing the A/B resolves; a cost it does resolve is found and taken back inside BB. At `cores > 1`, the rightmost leaf's widened hold is measured against BB-R8's cells, and a loss there is taken back inside BB where BB's rules allow it, or stated with its number and the proposal that takes it back |
| **Does not settle** | BA-Q0..Q13 and BA's resumption, which waits for BB-S5; the push of any BB stage; the v3.0.0 tag |
| **Recorded at** | BB's header, §4 (BB-Q10, BB-Q11) and §6; the index row |
