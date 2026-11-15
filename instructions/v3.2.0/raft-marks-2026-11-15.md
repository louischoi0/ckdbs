# Raft marks, 2026-11-15

The operator's words of 2026-11-15 that open a new matter, and what CLA
took under them.

## 1. F4 ruled, and taken up

| | |
|---|---|
| **Word** | W1, on 2026-11-15, after CLA listed F4's facts and its six decisions: *"`$!a`, land the audit branch first and proceed"* (given in Korean; recorded in English) |
| **Mark** | **F4 (a)**, the operator's own mark: an `UPDATE` or `DELETE` whose WHERE fails on the page's version of a row whose writer its view cannot see resolves the snapshot's version through the undo chain and tests it; a match goes on to `txn.md` §5's verdict table, which waits on or refuses that writer. **The audit branch first:** found already landed - `worktree-audit-lean-and-correct` merged to `main` as `27c6ebad` on 2026-11-14, wholly contained in `origin/main` `8d3adccc` - so F4 is built on worktree `audit-f4-where-on-snapshot` from `8d3adccc` |
| **Does not settle** | CR-Q1..Q8; the residues `txn.md` §5 names; a merge, a push, a version |
| **Recorded at** | this file; `docs/spec/txn.md` §5 ("Which version selects a row"); `manual/sql/sql.md`; the pending entry's F4; this file's row in `instructions/v3.2.0/index.md` |

## 2. What CLA took on the word "proceed"

Each is **CLA's proposal from the same list, taken on W1's "proceed"**,
not the operator's own per-item mark, and each is open to the operator
before the merge.

| Item | Options | Taken, and why |
|---|---|---|
| **F4-2** - a head committed after the view that fails the WHERE while the snapshot's version passes | (a) skip under `READ COMMITTED`, refuse under `REPEATABLE READ`; (b) refuse at both levels, as F4 (a) read literally does | **(a).** Under `READ COMMITTED` the page's version is the newest committed state, and skipping it is what PostgreSQL's re-check of the newest version answers; (b) would turn an answer that is right today into a `TxnConflict` until CR re-runs it. Under `REPEATABLE READ` the statement's state says the row matches, so a newer version is first-updater-wins' refusal |
| **F4-3** - F4 against CR | (a) F4 first, on its own, so that CR-S0's census reads a tree with F4 in it; (b) fold F4 into CR | **(a)** |
| **F4-4** - what form the work takes | (a) a work order of its own; (b) a fix on a worktree, the pending entry carrying the claim, as G1-G15 were taken | **(b)**: the pending entry carries the claim, the scenario and the fix, and one stage needs no order |
| **F4-6** - a measurement | (a) one contended cell at the close, interleaved A/B in `build-release`; (b) none | **(a)**: the added cost falls only on a row whose current writer is invisible and whose page version fails the WHERE - one undo read and one decode |

## 3. The cost accepted, and landed

| | |
|---|---|
| **Word** | W2, on 2026-11-15, after CLA reported the close measurement: *"`$!a`, push no verify"* |
| **Mark** | **F4-M (a)**, the operator's own mark: the measured cost is accepted as the fix's - nil on the common path, about 123 ns of server CPU per row on a row whose writer the view cannot see and whose page version fails the WHERE (`bench/v3.4.10/results-audit-f4-overhead-v3.4.10-1-g4f5b2e17.md`); no profiling or memo of the in-flight answer is owed. **Landed:** `worktree-audit-f4-where-on-snapshot` merged to `main` with `--no-ff` and pushed with `--no-verify`. The hook was skipped; the full suite had run 4462/4462 at `4f5b2e17`, and every commit after it is documents only |
| **Does not settle** | a version; CR-Q1..Q8 |
| **Recorded at** | this file; this file's row in `instructions/v3.2.0/index.md` |

## 4. Oracle's read-only transaction, and a work order written

| | |
|---|---|
| **Word** | W1, on 2026-11-15, after CLA's survey of Oracle features this engine could adopt (item 1: a `READ ONLY` transaction over an SCN-style snapshot) and CLA's reading of the tree, which found the one-number snapshot already built (the commit LSN ceiling) and the writer's machinery spent at every `BEGIN`: *"Write the work order for item 1."* (given in Korean; recorded in English) |
| **Mark** | **`workorder-cs-read-only-transaction.md` is written and not opened.** The letter CS is CLA's proposal |
| **Does not settle** | the opening of CS or of any stage; CS-Q0 to CS-Q9, each CLA's proposal only - CS-Q1 among them, whether CS-S0's measurement can stop the build; a merge, a push, a version |
| **Recorded at** | this file; the order's header and §7; the CS row and this file's row in `instructions/v3.2.0/index.md` |

## 5. Tagged

| | |
|---|---|
| **Words** | W3, on 2026-11-15: *"tag 3.4.11"*; W4, the same day: *"push no verify"* |
| **Mark** | **W3:** `v3.4.11` is an annotated tag at `a61ab905`, the head of `origin/main`, pushed; its message carries what is built - F4 - and the gaps that bound it. One error in it: its "15 + 27 slices are unread" double-counts, and 15 slices are unread in all (the second run read 12 of the first run's 27). The tag was not re-pointed, which would need a force push. **W4:** `CLAUDE.md`'s version line moved in a documents-only commit, pushed to `main` with `--no-verify` |
| **Does not settle** | CR-Q1..Q8; CS-Q0..Q9; G21-G64 |
| **Recorded at** | this file; the tag message; `CLAUDE.md`'s Version Management; this file's row in `instructions/v3.2.0/index.md` |

## 6. The optimizer survey's item 1, and a work order written

| | |
|---|---|
| **Word** | W5, on 2026-11-15, after CLA's survey of the optimizer layer against other engines (six candidates, item 1 a value list served as a key): *"Write the work order for the item 1 milestone."* (given in Korean; recorded in English) |
| **Mark** | **`workorder-cu-key-set-access.md` is written and not opened.** The letter CU is CLA's proposal; CT is passed over because `catalog.md` numbers its rules CT1..CT7 |
| **Does not settle** | the opening of CU or of any stage; CU-Q0 to CU-Q7, each CLA's proposal only; a merge, a push, a version |
| **Recorded at** | this file; the order's header and §7; the CU row and this file's row in `instructions/v3.2.0/index.md` |

## 7. The optimizer survey's item 2, and a work order written

| | |
|---|---|
| **Word** | W6, on 2026-11-15, while §6's order was being written: *"And write the item 2 milestone as a work order."* (given in Korean; recorded in English) |
| **Mark** | **`workorder-cv-ordered-index-walk.md` is written and not opened.** The letter CV is CLA's proposal |
| **Does not settle** | the opening of CV or of any stage; CV-Q0 to CV-Q6, each CLA's proposal only - CV-Q4, the tie order under a reverse walk, amends `parser-v2.md` I12 if marked (a); a merge, a push, a version |
| **Recorded at** | this file; the order's header and §7; the CV row and this file's row in `instructions/v3.2.0/index.md` |

## 8. G21, G23-G26 verified, then taken up

| | |
|---|---|
| **Words** | W7: *"resume the G21-G64 verification"*; W8: *"fix G21 and G23-G26; G21 as proposed"*, with the skill `go-ahead-achieving-milestone` invoked (*"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session"*) (each given in Korean; recorded in English) |
| **Mark** | **W7:** CLA kept the scale the operator chose on 2026-11-14 for G16-G20 (the finders' highest severity first, two verifiers a finding, about ten a pass) and verified G21 and G23-G26 at `030b5bd9`; all five survived both lenses (the pending entry's §10, `f63c6023`). **W8:** the five taken up as one milestone, on worktree `audit-g1-g15-fixes` (its directory; a worktree-isolated session cannot create a second one) on branch `worktree-audit-g21-g26-fixes` from `f63c6023`, whose build inputs are `61917b0f`'s - `origin/main` with F4 landed by another session (§3) after the verification read `030b5bd9`. **G21-D is the operator's own mark**: the version-chain walks take `kds::CycleGuard` in place of the 2^16-step bound `txn.md` §3.7 stated, `kMaxUndoChainLength` kept only for `UndecidedInsert`, as the verifiers proposed |
| **Does not settle** | G27-G64 (less G59), the 15 unread slices, the review's two findings put to the operator (the pending entry's §10), a merge, a push, a version |
| **Closed** | on branch `worktree-audit-g21-g26-fixes`: the fixes at `0304a822`, two reviews (`6c26ee45`, `f2b8aa09`), `origin/main` `32354dbe` merged at `e944b639` (documents only), the suite 4469/4469 and the sim corpus 280/0 there, the close's A/B at `f153f3e5` (`bench/v3.4.10/results-audit-g21-close-v3.4.10-10-gf2b8aa09.md`; a shared host, nothing attributable to the fixes). Not merged to `main`, not pushed |
| **Recorded at** | this file; the pending entry's §10; this file's row in `instructions/v3.2.0/index.md` |

## 9. Decisions adopted on the standing go-ahead

Each is **adopted on the operator's standing go-ahead of 2026-11-15
(go-ahead-achieving-milestone)**: CLA's proposal, not the operator's own
per-item mark. None is `[quiet-wrong]`.

| Item | Options | Adopted, and why |
|---|---|---|
| **G23-D** - where the cut run's length is applied | (a) `scratch.resize(run * kPageSize)` where the run is cut; (b) a span over the prefix at the write | **(a)**, the verifiers' alternative: the buffer is the run's in every later use, not only at the write |
| **G24-D** - how a released spill under the copy is answered | (a) the decode split from the append, and a `NotFound` decode of a version the head writer wrote read again by CI-R7's loop; (b) decode only the key columns | **(a)**, both verifiers': (b) does not hold for a covering index or a unique build's check, which use the whole row. Widened at review to the head writer's older versions in the undo walk, and to a re-read that finds the row exactly as copied failing with the decode's own status. No cell: the copy/append seam CI-R7 lacks does not exist (`known-gaps.md`) |
| **G25-D** - the guard on the Bound Cabin walks | (a) the leaf walks' guard, renamed `storage::PageChainGuard` now that it guards two chain kinds; (b) `kds::CycleGuard` written into each walk | **(a)**: one call shape and one message at every page-chain site; the old name survives only in history records |
| **G26-D** - how a refused apply leaves no record | (a) check under the registry latch, then log, then apply; a refused check takes the placed entry back off the held tail (`BoundCabinPage::TakeBackLast`); (b) check before the page work | **(a)**, both verifiers': the check must run under the hold the apply runs under, which the page work is outside of, and an entry left unlogged on the tail would shift the next record's index at replay. Widened at review: a departure whose first hold found no group (group id 0) is refused before its record, as replay would refuse it |
