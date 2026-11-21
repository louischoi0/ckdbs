# Raft marks - 2026-11-21

The operator's words of 2026-11-21, recorded in English.

## 1. DL landed: push no verify

| | |
|---|---|
| **Word** | *"go ahead"*, given at DL's push gate, after the report that ended "the work is held only by the push gate" |
| **Mark** | **DL lands on `main`**: the work branch `worktree-dl-statistics-queue-no-statement`, synced with `origin/main` at `01a7609b` (documents and bench only) and again at `2c2639df` (DD's `PURGE INDEX` and DN-S0/DN-S1, with build inputs), merged `--no-ff` and pushed. The full suite ran 0 failed of 5,052 on the merged tree, whose build inputs are the pushed tree's, so the push goes out with `--no-verify` (the pre-push hook skipped, the suite not run again by it). `tsan` ran 235 cells with no report at `43cecfa0`; `bs-nightly` not run. The close measurement: `bench/v3.4.17/results-dl-close-v3.4.17-11-g30e169df.md` |
| **Skipped on that word** | the pre-push hook |
| **Does not settle** | a tag or a version; the reader storm's tail and DL-Q4's two words, left in `known-gaps.md` |
| **Recorded at** | this file; the DL order's header and §7; its row in `instructions/v3.2.0/index.md` |

## 2. DK opened on the standing go-ahead

| | |
|---|---|
| **Word** | *"The open milestones by priority, /go-ahead-achieving-milestone"* (given in Korean; recorded in English) |
| **Which milestone** | The one opened order not closed is DN, and it waits at DN-S1's gate for the operator's mark on the amended hard invariants (`raft-marks-2026-11-20.md` §28); its worktree is held by another live session. So CLA took the highest-priority order written and not opened: **DK**, the btree descent. Why first: it is on every keyed statement's path; it carries a refusal with nothing racing (the stale root), a defect a client can meet; and its S0 run is the one DM-S0's M1 shares (`workorder-dm-leaf-upper-bound.md` DM-Q1). CR's items need a contract change in `txn.md` §5, CW's protocol is wider, and DG's own proposal is "no order" (DG-Q0 (c)) |
| **Mark** | **DK is opened** on worktree `dk-descent-low-key` from `2c2639df` (`v3.5.0-12-g2c2639df`). DK-Q1..Q9 are adopted as CLA proposed, **on the operator's standing go-ahead of 2026-11-21 (go-ahead-achieving-milestone)**, each at the stage that reached it; none is the operator's own per-item mark. DK-S0 done: D1 go (DK-Q2 (a)), D2 go (DK-Q3 (a), by its refusal clause), D3 handed to CW (DK-Q1 (a)); `bench/v3.5.0/dk-s0-premise-v3.5.0-13-g187ff9b1.md` |
| **Closed** | **DK closed narrow on the work branch** at the same go-ahead: DK-S1 (D1) `7622d619`, DK-S2 (D2) `9a551ddd`, DK-S3 nothing built, DK-S4 measured (`bench/v3.5.0/results-dk-close-v3.5.0-15-g9a551ddd.md`: level with `2c2639df` in every cell; refusals not lowered). Adopted on the standing go-ahead, none the operator's own mark: DK-Q1, Q2, Q3, Q4, Q5, Q7, Q8, Q9 (a), and two items CLA added at DK-S2, **DK-Q10 `[quiet-wrong]` (a)** (no write hop from the named root) and DK-Q11 (a) (the `descent_hops` tally); DK-Q6 not reached |
| **Does not settle** | a push, a merge to `main`, a tag or a version; DN-S1's gate |
| **Recorded at** | this file; the DK order's header, §4 and §7; the DK row in `instructions/v3.2.0/index.md` |

## 3. DG opened on the standing go-ahead

| | |
|---|---|
| **Word** | *"/go-ahead-achieving-milestone DG"*: the operator named DG |
| **Mark** | **DG is opened** on worktree `dg-wal-segment-body` from `46ee8a6e` (`v3.5.0-34-g46ee8a6e`). DG-Q0..Q10 are adopted as CLA proposed, (a) in each, **on the operator's standing go-ahead of 2026-11-21 (go-ahead-achieving-milestone)**. None is the operator's own per-item mark. DG-Q5 (a) is revised against the tree: CZ's C1 is on `main` (CZ-S3, `0f4edbd5`), so both arms of DG-S0 carry C1 as it was built, with no switch |
| **Correction** | §2 read DG's own proposal as "no order" (DG-Q0 (c)). That was wrong. (c) is the alternative the order sets beside its proposal, and CLA's proposal is (a): an order on the fill, exploratory |
| **What the adoption itself stops** | DG-Q1 (a) makes V0 the operator's reading of DG-S0's table. DG-Q2 (a) gives the choice of candidate to the operator at V0, and gives P2 and P3 (`[quiet-wrong]`) a mark each. So under these adoptions DG-S1..S3 cannot proceed on the standing word, and the milestone stops at V0 |
| **DG-S0** | Done on the same word (the order's §7, "DG-S0"). The driver's `--insert-rate` is at `e9952d64`; the measurement is `bench/v3.5.0/results-dg-s0-premise-v3.5.0-36-ge9952d64.md`, and its §1 is V0's table. It ran 23.9 minutes against the standing 20 and states the cut. **The milestone stops at V0** |
| **Does not settle** | V0, V1, the candidate, a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; the DG order's header, §4 and §7; the DG row in `instructions/v3.2.0/index.md` |

## 4. DG-S0 pushed with `--no-verify`

| | |
|---|---|
| **Word** | *"push no verify"*, given at DG's V0 stop |
| **Mark** | **DG-S0 lands on `main`**: the work branch `worktree-dg-wal-segment-body` (`2c2ed8dd`, `e9952d64`, `ea8fd59d` and this record), merged `--no-ff` onto `origin/main` `46ee8a6e` and pushed. The branch changes no build input (bench, `tools/`, documents only), so no suite was run for it and none was run by the hook |
| **Skipped on that word** | the pre-push hook |
| **Does not settle** | V0, V1, the candidate (DG-Q2), a tag or a version |
| **Recorded at** | this file |

## 6. DD-S4 ruled (b): the driver is DN-S2's; DN taken up on the standing go-ahead

Numbered §6 because §5 is held by DG's close record on the work branch
`worktree-dg-wal-segment-body` (`e98163d2`), not landed.

| | |
|---|---|
| **Word** | *"$!b /go-ahead-achieving-milestone"*, given after CLA's report on DD put three options for DD-S4 and proposed (b) (given in Korean; recorded in English) |
| **Mark** | **DD-S4 is option (b), the operator's own mark**: the driver that removes index entries unasked is not built as DD-S4. It is designed and built inside DN-S2, after the operator's mark at DN-S1's gate, together with DN's record that no entry of **any** index of a relation names a slot (DN-R2; DD's §0a item 5). The ground: DD's `PURGE INDEX` purges one index, while slot reuse on a DN heap needs a fact about every index of the relation, so a driver built per index now would be rebuilt at DN-S2. **DD stays closed**, its DD-S4 moved to DN-S2; for a btree relation, entry removal stays an administrator's `PURGE INDEX` (DD-Q4 (a)) |
| **Which milestone** | The word names no milestone; the mark puts the driver's work in DN, so the milestone is **DN** (the skill's §1, item 2), on worktree `dn-dd-s4-into-dn-s2` from `dcab41e5` (`v3.5.0-39-gdcab41e5`) |
| **The stop** | **DN stops at DN-S1's gate at once.** That gate is the operator's mark on the amended hard invariants (the DN order's §8: invariant 4 under DN-D7, 9, 11's restatement, with DN-Q13 (c) and DN-Q14 (a), both `[quiet-wrong]`). A hard invariant is amended only under a recorded mark of the operator's (DN-S1's log), which the standing go-ahead is not (the skill's §5). **Still owed before DN-S2, apart from the mark**: DN-Q11's descending named load, measured |
| **Does not settle** | DN-S1's gate; DN-Q13, DN-Q14; the driver's design (DN-S2's); a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; the DD order's header, §5 and §7; the DN order's header, §5 and §7; their rows and this file's row in `instructions/v3.2.0/index.md`; `docs/inflight/known-gaps.md` (Index entry purge) |

## 7. §6's record pushed with `--no-verify`

| | |
|---|---|
| **Word** | *"approve"* (given in Korean; recorded in English), in reply to CLA's report that ended by asking for a push word for `2d930204` |
| **Mark** | **§6's record lands on `main`**: the work branch `worktree-dn-dd-s4-into-dn-s2` (`2d930204` and this record), merged `--no-ff` onto `origin/main` `dcab41e5` and pushed. Documents only, so no suite was run for it and none was run by the hook |
| **Skipped on that word** | the pre-push hook |
| **Does not settle** | DN-S1's gate: CLA read the word as the push it was asked for, not as the mark on DN's §8, which is asked for separately; a tag or a version |
| **Recorded at** | this file; this file's row in `instructions/v3.2.0/index.md` |

## 8. DO written: the frame table's long holds

| | |
|---|---|
| **Word** | *"Yes."*, given after CLA's account of CW's C1 - what it buys without the rest of CW - and its proposal to cut "the structure latch's long holds removed" out as a small milestone of its own, ending with the question whether to write it (given in Korean; recorded in English) |
| **Mark** | **DO is written, not opened**: `instructions/v3.2.0/workorder-do-frame-table-long-holds.md`, on `main` at `3a07ad49`, documents only. That it is an order of its own, cut out of CW's C1, is the operator's yes to CLA's proposal; the letter, the candidates (E1-E4), the rules (DO-R1..R6) and the stages are CLA's proposals, DO-Q0..Q8 unmarked |
| **Corrected** | Two claims CLA made in the conversation are false on the tree, and the order says so in its §0: that `EvictColdFrames` holds the structure latch across full laps in production (it has test callers only; production sweeps are bounded batches, at most 512 slot visits a hold), and that the hit path's usage bump should be made to read before it writes (it already does, `device_page_store.cpp:1089`) |
| **Does not settle** | DO-Q0..Q8; whether DO opens; CW's C1, which DO-Q8 (a) would amend to read "after DO" at DO's close; a push |
| **Recorded at** | this file; the DO order's header, §0 and §7; the DO row and this file's row in `instructions/v3.2.0/index.md` |

## 9. DO pushed with `--no-verify`

| | |
|---|---|
| **Word** | *"Push it."* (given in Korean; recorded in English), in reply to CLA's report that ended by asking whether to push `d4fc645e` |
| **Mark** | **§8's order lands on `main`**: `d4fc645e` (the DO order, its index row and §8) and this record, on `origin/main` `3a07ad49`, pushed. Documents only, so no suite was run for it and none was run by the hook |
| **Skipped on that word** | the pre-push hook |
| **Does not settle** | DO-Q0..Q8; whether DO opens; a tag or a version |
| **Recorded at** | this file; this file's row in `instructions/v3.2.0/index.md` |
