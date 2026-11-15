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

## 8. CS opened on the standing go-ahead

| | |
|---|---|
| **Word** | W7, on 2026-11-15: the operator invoked `go-ahead-achieving-milestone` with the argument `cs-read-only-transaction` - the skill's standing word, *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* |
| **Mark** | **CS is opened** on worktree `cs-read-only-transaction` from `32354dbe` (`v3.4.11-1-g32354dbe`). **CS-Q0..CS-Q9 are each adopted as CLA proposed - option (a) in every row - on the operator's standing go-ahead of 2026-11-15 (go-ahead-achieving-milestone)**, not the operator's own per-item marks. No item of the table carries `[quiet-wrong]`; the two `[quiet-wrong]` rules, CS-R1 and CS-R2, are rules and not choices, and each is pinned by a test in the stage that lands it. CS-Q1 (a) keeps its own stop: if CS-S0's bar is not met, CS returns to the operator |
| **Does not settle** | a merge, a push, a tag or a version |
| **Recorded at** | this file; the order's header, items table and §7; the CS row and this file's row in `instructions/v3.2.0/index.md` |
