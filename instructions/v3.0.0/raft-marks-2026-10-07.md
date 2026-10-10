# Ratification marks — 2026-10-07

**The operator's words of 2026-10-07**, recorded by CLA on
`worktree-wal-recycling` (§1-§6), `worktree-bd-sorted-leaf-named-keys` (§7-§13, §17), `worktree-pool-budget-required` (§14-§16), `worktree-keep-btree-leaf-slots` (§18-§20), `worktree-drop-table-page-reclaim` (§21-§22, §25) and `worktree-bg-scan-ring-order` (§23-§24) (`v2.7.0-*`; the v3.0.0 tag is not cut). Every word
is recorded **in English**, translated where it was typed in Korean.

---

## 1. BC's order written

| | |
|---|---|
| **Word** | *"sync main, and first write the work order for WAL recycling"* |
| **Mark** | `workorder-bc-wal-recycling.md` is written, as BC-S0, on `worktree-wal-recycling` from `6dc792c9` |
| **Does not settle** | the letter's opening and its §4 items (§2 below) |
| **Recorded at** | the order's §6 |

## 2. BC opened, its seven items marked as proposed, and BC-S1 started

| | |
|---|---|
| **Word** | *"mark BC-Q0 to Q6 as proposed and go ahead with BC-S1"* |
| **Mark** | BC-Q0..Q6 are marked as CLA proposed them (`workorder-bc-wal-recycling.md` §4). **BC-Q0:** BC opens, with BC-S0..S5 and BC-R1..R6 as written. **BC-Q1 (a):** recycling does not wait for archiving; archiving, when built, adds its floor at BC-R1's one computation (CN-9 §9 O4 answered). **BC-Q2:** redo is floored at the anchor, **conditional on BC-S1's comparison being green**. **BC-Q3 (a):** segment reuse is out of BC. **BC-Q4:** no retention knob. **BC-Q5 (a):** the unlink runs on core 0's WAL writer thread, measured at the close at `cores > 1`. **BC-Q6:** `SHOW META`'s three fields. **BC-S1 starts** |
| **Does not settle** | any stage after BC-S1 - each waits for its own word; BC-Q2's condition, which BC-S1's result decides; the push; the v3.0.0 tag |
| **Recorded at** | the order's header, §4 and §6, CN-9 §9 O4, the index row |

## 3. BC's close-out measurement waived

| | |
|---|---|
| **Word** | *"in this milestone, measurement will be skipped as an exception"* |
| **Mark** | **BC closes without §5's measurement**, as an exception to `CLAUDE.md`'s Session Workflow step 3 for this milestone. The interleaved A/B is not run, and BC-Q5 (a) stands without the `cores > 1` peer-latency check it was marked against |
| **Reading** | An exception for BC, not a change to the rule: the next milestone measures as before. Nothing BC lands may be reported as measured; BC-S5's report states the measurement as **not executed, waived by the operator** |
| **Does not settle** | BC-Q5's fallback to (b), which now has no measurement to trigger it; a later order may measure BC's change |
| **Recorded at** | the order's §5 and BC-S5 row, the index row |

## 4. BC-S2 started

| | |
|---|---|
| **Word** | *"when the review is done, commit and go ahead with BC-S2"* |
| **Mark** | BC-S1 is committed once its review is applied, and **BC-S2 starts**: the device (BC-R3, BC-R4) |
| **Does not settle** | BC-S3..S5's start; the push |
| **Recorded at** | the order's header and §6 |

## 5. BC-S4's defect: investigated and fixed by fail-stop

| | |
|---|---|
| **Word** | *"go ahead with (a)"*, then the answer *"Fail-stop (Recommended)"* to CLA's question of the fix |
| **Mark** | The refused reboot BC-S4's simulator found is investigated and fixed before BC-S4 closes. **The fix is fail-stop**: after a refused record the log refuses every write until a restart (`docs/spec/wal.md` §6-5) |
| **Does not settle** | whether a failed device *sync* stops the log too (`known-gaps.md`, WAL); the status a commit refused after the stop reports |
| **Recorded at** | the order's §6 (BC-S4) |

## 6. BC-S5 started, and the push

| | |
|---|---|
| **Word** | *"when the review is done, commit and go ahead with BC-S5"*, then *"when BC-S5 is done, commit and push to main"* |
| **Mark** | BC-S4 is committed after its review; BC-S5 is built, reviewed and committed, and the branch is pushed to `main` |
| **Does not settle** | the v3.0.0 tag |
| **Recorded at** | the order's header and §6 |


## 7. BD's order written, reviewed and pushed

| | |
|---|---|
| **Word** | *"isn't there a way to delete kUnordered and still have no high water mark error?"*; *"why is an insert that breaks the order needed at all? when inserting a lower key into a leaf, just put it in its proper place rather than always at the end"*; *"while deleting kUnordered, without a separate flag, and keeping the btree always sorted, there must be no high water mark error."*; *"write the work order for this invariant change; also, a named_pk insert must fail because of its pk for only 2 reasons: a duplicate, or exhaustion."*; *"high water mark (there must be no case where writing pk=100 first makes inserting pk=99 fail"*; *"write only the work order and push"*; *"when the review is done, apply it and push"* |
| **Mark** | `workorder-bd-sorted-leaf-named-keys.md` is written as BD-S0 on `worktree-bd-sorted-leaf-named-keys` from `bf8ea937`. Its `critics-developer` review is applied (the order's §6), and it is pushed. The order alone moves; no engine file does. A btree places a named key where it sorts, and a named key's pk is refused for exactly two reasons (BD-R5) |
| **Does not settle** | BD's opening (BD-Q0); BD-Q10, BD-Q11 and BD-Q12, which the review raised |
| **Recorded at** | the order's header (W1-W3, W5, W8, W11, W14) and §6 |

## 8. BD-Q1 marked (a)

| | |
|---|---|
| **Word** | *"I will choose (a)."* - to CLA's (a), shift the slot directory and change what addresses a slot, against (b), fixed slots and a key-order array |
| **Mark** | BD-Q1 (a): BD-R2's sorted placement, and BD-R3's shift-safe holders |
| **Does not settle** | how recovery undo re-finds a row (BD-Q8, §13) |
| **Recorded at** | the order's header (W4) and §4 |

## 9. BD-Q2 marked as proposed, then revised: rolled-back keys are reused

| | |
|---|---|
| **Word** | *"build the use of tombstones as CLA proposes"*, then *"a rolled-back id must of course be reusable"* |
| **Mark** | BD-Q2 as CLA proposed it, with the second word overriding its rolled-back half. An id is consumed when its tuple commits. Today the delete-marked row is the tombstone, since nothing purges a user relation. A rolled-back key is free and may be named again. A purge, when one is built, owes a keyed tombstone (BD-R4). K1 now reads "bound to at most one committed tuple", restated at BD-S5 |
| **Does not settle** | a purge; the second path into the equal-index-entries bug, which is recorded in the order's §1.7 and not fixed |
| **Recorded at** | the order's header (W6, W12), §1.7, BD-R4 and §4 |

## 10. BD-Q3 marked (a)

| | |
|---|---|
| **Word** | *"go with (a)"* - to CLA's reading of the two reasons. (a): duplicate is a slot keyed `k` (`AlreadyExists`), and exhausted is outside `[1, 2^40 - 1]` (`OutOfRange`, with today's `InvalidArgument` folded in). (b): present-now against used-before, with out-of-space values left `InvalidArgument` |
| **Mark** | BD-Q3 (a), as BD-R5 states it |
| **Does not settle** | an undecided insert of the same key, which §9's later word makes a wait rather than a duplicate (BD-Q9, §13) |
| **Recorded at** | the order's header (W7) and §4 |

## 11. Superblock backward compatibility is never kept - a standing order

| | |
|---|---|
| **Word** | *"superblock backward compatibility is not needed. Until I say otherwise, always apply this"* |
| **Mark** | **Standing until the operator says otherwise:** a change to an on-disk meaning bumps `kSuperBlockVersion` and refuses every older volume, with no migration, no legacy reader, and no item asking whether to keep one. For BD it is BD-Q5: superblock 20, with version 19 refused (BD-R8) |
| **Does not settle** | nothing further; it is the default from here |
| **Recorded at** | the order's header (W9), BD-R8 and §4 |

## 12. BB-R1 deleted, and BD's measurement waived

| | |
|---|---|
| **Word** | *"delete BB-R1. skip the performance measurement"* |
| **Mark** | BD-Q6: BB-R1 is withdrawn on a btree, and the issue and the admission leave the leaf's hold (BD-R6). BD-Q7: BD's close-out measurement is waived, so nothing BD lands is measured (BD-R10) |
| **Does not settle** | the median divide BB-R1's deletion brings back at `cores > 1` (BD-Q11); BA's rows, rebased per BD-Q12 |
| **Recorded at** | the order's header (W10), BD-R6, BD-R10 and §4 |

## 13. BD-Q4, BD-Q8 and BD-Q9 marked as proposed

| | |
|---|---|
| **Word** | *"mark BD-Q4, Q8, Q9 as proposed"* |
| **Mark** | **BD-Q4 (a):** the heap is untouched. BB-R7, its two entry points, the mark's refusal and its ordinal mark stay; the cost is a second insert protocol kept alive for a relation type no mountable volume holds. **BD-Q8 (a):** recovery undo re-finds a row by scanning the recorded `kBtreeLeaf` and rightward while `min_key <= pk`. **BD-Q9 (a):** a named key meeting an undecided insert of the same key waits for its decide, and the re-run answers by the outcome. The word came before BD-S0's review, which sharpened all three without changing which option they are |
| **Does not settle** | BD-Q0, BD-Q10, BD-Q11 and BD-Q12 |
| **Recorded at** | the order's header (W13) and §4 |

## 14. `buffer_pool_frames` is required, `0` is an error, and the value is a ceiling

| | |
|---|---|
| **Word** | *"buffer_pool_frames=0 is an error. This value becomes the maximum"*; asked what a missing key does, *"refuse boot (required key)"*; asked whether to build it, *"record the decision only"* |
| **Mark** | The key is required: a config without it is refused at boot, and `0` is an error. The value is the pool's maximum, not a soft target. Recorded as not built in `eviction.md` §6 and `known-gaps.md` (Eviction) at `e352eac0`; nothing in the engine moved |
| **Does not settle** | the refusal's status code; whether the store's unbounded mode dies with the key's; what a fault at the ceiling does - each is a BE item (BE-Q2..Q4) |
| **Recorded at** | `eviction.md` §6, `known-gaps.md` (Eviction), and `workorder-be-bounded-pool.md`'s header |

## 15. BE's order written

| | |
|---|---|
| **Word** | *"write the work order and prepare the handoff, and also tell me the key to open the next session"* |
| **Mark** | `workorder-be-bounded-pool.md` is written as BE-S0 on `worktree-pool-budget-required` from `e352eac0`, reviewed, and not opened. No engine file moves |
| **Does not settle** | BE-Q0..Q10 |
| **Recorded at** | the order's header and §6 |

## 16. BE opened, BE-Q0..Q10 marked as proposed

| | |
|---|---|
| **Word** | *"push it, and mark BE-Q0..Q10 as proposed"* |
| **Mark** | **BE-Q0:** BE opens with BE-S0..S6 and BE-R1..R5 as written. **BE-Q1 (a):** a chunked slot array with a free list and a hand over slots. **BE-Q2 (b):** the store's unbounded mode is deleted everywhere, its `Open` taking a required capacity at all 51 sites. **BE-Q3:** a missing key, `0` and a cap below the floor are refused `InvalidArgument`. **BE-Q4 (a):** at the cap, the bounded sweep retries and then `ResourceExhausted`; nothing writes back on the fault path, so one statement dirtying more than the cap is refused. **BE-Q5:** no new keys. **BE-Q6:** a mount floor of resident-class pages plus 256 frames. **BE-Q7 (a):** the outermost read walk faults cold; the scan ring is declined. **BE-Q8:** measured once at BE's close. **BE-Q9:** BE before BA-S8; BD runs in parallel. **BE-Q10:** `txn.md` governs a refused statement's scope |
| **Does not settle** | BE-S1's premise gate: if the in-engine sweep costs under a tenth of the standalone figure, BE stops at BE-S1 and the operator rules again |
| **Recorded at** | the order's header (W1), §2's heading and §4; `index.md`'s BE row |

## 17. BD-Q10, BD-Q11 and BD-Q12 marked as proposed

| | |
|---|---|
| **Word** | *"mark Q10, Q11, Q12 as proposed"* |
| **Mark** | **BD-Q10 (a):** a split is logged as one `BTREE_SPLIT` record carrying every image it writes, which redo applies whole (BD-R12). **BD-Q11 (a):** a full rightmost leaf splits at the insertion point rather than at the median (BD-R2). **BD-Q12 (b):** BB closes on the measurement it has, without rebasing BA, and BD-S6 rebases BA once, against BD (§5) |
| **Does not settle** | BD-Q0, BD's opening; writing BB's close entry, which waits for its own word; the push |
| **Recorded at** | the order's header (W15), BD-R2, BD-R12, §4, §5 and §6 |

## 18. BD opened

| | |
|---|---|
| **Word** | *"open BD-Q0, with the worktree named keep-btree-leaf-slots"* |
| **Mark** | **BD-Q0: yes.** BD is open with BD-S0..S6 and BD-R1..R12 as written at `50d35916`. BD-S1, red first, starts on `worktree-keep-btree-leaf-slots`, branched from `50d35916` |
| **Does not settle** | writing BB's close entry, which waits for its own word; the push |
| **Recorded at** | the order's header (W16), §4 and §6, and the index row |

## 19. BD run to its close

| | |
|---|---|
| **Word** | *"keep going until closing this milestone, follow CLA proposal"* (given twice), then *"keep going until the milestone closes"* |
| **Mark** | BD-S1 to BD-S6 run in order on `worktree-keep-btree-leaf-slots`, each through its review and the suite; every choice a stage raised taken as CLA proposed it |
| **Does not settle** | the push; BB's close entry, which waits for its own word; BA's resumption |
| **Recorded at** | the order's status line, and the index row |

## 20. BD pushed

| | |
|---|---|
| **Word** | *"push it"* |
| **Mark** | `worktree-keep-btree-leaf-slots` at `bc144dbf` - BD-S4 to BD-S6 and the merge of `origin/main` - pushed to `main` (`e4b107af..bc144dbf`), the pre-push hook run and green (3223 of 3223) |
| **Does not settle** | BB's close entry, which waits for its own word; BA's resumption |
| **Recorded at** | here |

## 21. BF's order written

| | |
|---|---|
| **Word** | *"next, the work order for DROP TABLE page reclamation"* |
| **Mark** | `workorder-bf-drop-table-page-reclaim.md` is written as BF-S0 on `worktree-drop-table-page-reclaim` from `bc144dbf`, reviewed, and not opened. No engine file moves |
| **Does not settle** | BF-Q0..Q18 |
| **Recorded at** | the order's header and §6, and `index.md`'s BF row |

## 22. BF opened, BF-Q0..Q18 marked as proposed

| | |
|---|---|
| **Word** | *"mark BF-Q0..Q as proposed"* |
| **Reading** | "BF-Q0..Q" read as every item §4 holds once BF-S0's review was applied: BF-Q0..Q18 |
| **Mark** | **BF-Q0: yes** - BF opens with BF-S0..S6 and BF-R1..R13 as written. **BF-Q1 (b):** the mount first, then within the run (BF-S5). **BF-Q2 (a):** a free is crash-safe once `D` is past the drop; no WAL record added. **BF-Q3 (a):** the roots wait in the tombstone's word. **BF-Q4:** superblock 21, version 20 refused (§11). **BF-Q5 (c):** the walk reclaims; the owner census is the sim's oracle. **BF-Q6 (i):** an in-memory free list ahead of the cursor. **BF-Q7 (a):** a freed page's frame is discarded, `EvictClean` deleted. **BF-Q8 (a):** what the walk reaches. **BF-Q9 (b):** the statement epoch, published before the schema word is read. **BF-Q10 (a):** all three defence checks. **BF-Q11 (a):** the RESTRICT window fixed in BF-S5. **BF-Q12 (a):** the code governs; the autocommit specs are restated. **BF-Q13:** the counters, no key, `kReclaimBatchPages` a constant. **BF-Q14 (a):** BF-S2 after BE-S2, and against the map-keyed table if BE stops at BE-S1. **BF-Q15:** measured once at BF's close. **BF-Q16 (a):** core 0's system tick. **BF-Q17 (a):** BF answers DT1's gate 3 and horizon preconditions for a dropped relation. **BF-Q18 (c):** a chain freed tail-first. Five of these are `[quiet-wrong]`: BF-Q1, Q2, Q9, Q11 and Q17 |
| **Does not settle** | BF-S1's start; BF-S1's premise gate on BF-R4's `cores > 1` arm, where BF stops and the operator rules; the push |
| **Recorded at** | the order's header (W1), §2's heading, §4 and §6; `index.md`'s BF row |

## 23. BG's order written

| | |
|---|---|
| **Word** | *"write the scan ring work order"* |
| **Mark** | `workorder-bg-scan-queue.md` is written as BG-S0 on `worktree-bg-scan-ring-order` from `cd8ca91e`, reviewed, and not opened. It answers BE's close finding of partial scan resistance, the case BE-Q7 named. No engine file moves |
| **Reading** | "The scan ring" read as the order BE-Q7 said would follow, not as a fixed mechanism. BG-Q1 puts the operator's words - (b), the slot ring for executor scans - beside CLA's proposal (a), a scan queue inside the store |
| **Does not settle** | BG-Q0..Q8 |
| **Recorded at** | the order's header and §6, and `index.md`'s BG row |

## 24. BG opened, BG-Q0..Q8 marked as proposed

| | |
|---|---|
| **Word** | *"mark BG-Q0..Q8 as proposed"* |
| **Mark** | **BG-Q0: yes** - BG opens with BG-S0..S4 and BG-R1..R5 as written. **BG-Q1 (a):** a scan queue the reclaim reads before the hand. **BG-Q2 (a):** the slot ring retired, its consumers on the queue. **BG-Q3:** promotion at the head, the counter kept. **BG-Q4:** `kRing` stays distinct from `kScan`. **BG-Q5:** no keys; `kds.scan_ring_frames` deleted from `eviction.md` §6. **BG-Q6:** BG-S4's bar as §4 states it after BG-S0's review. **BG-Q7:** either order against BF. **BG-Q8:** measured once at BG's close |
| **Does not settle** | BG-Q9, the queue's floor, which BG-S0's review added after the word was given; BG-S2 waits for it. BG-S1's premise gate, where BG stops for a ruling |
| **Recorded at** | the order's header (W1), §2's heading and §4; `index.md`'s BG row |

## 25. BF run to its close

| | |
|---|---|
| **Word** | *"go ahead dont stop until milestone, follow CLA proposal if decision needed"* |
| **Mark** | BF-S1 to BF-S6 run in order on `worktree-drop-table-page-reclaim`, each through its review and the suite; every choice a stage raises taken as CLA proposes it. **BF-Q14 read under it:** BE has not started (BE-S1 is its next stage), so BF-S2 proceeds against the map-keyed frame table - BF-Q14's own fallback, (b) - and BE-R1's eraser list counts BF-R5's discard when BE resumes |
| **Does not settle** | the push; BE's start; BB's close entry; BA's resumption |
| **Recorded at** | the order's status line and §6, and the index row |
