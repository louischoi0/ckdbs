# Raft marks, 2026-11-19

The operator's words of 2026-11-19 that open a new matter, and what CLA
took under them.

## 1. v3.4.13 named and tagged

| | |
|---|---|
| **Word** | *"tag 3.4.13"* - after CLA's report that CX landed on `main` at `8dc37b80`, with CY's change restored and the full Debug suite 0 failed of 4732 there (`raft-marks-2026-11-18.md` §49) |
| **Mark** | **The version of record is `v3.4.13`**, the operator's own word: an annotated tag on `main` at `8dc37b80`, pushed. Its message states what is built since `v3.4.12` (CX, CY, DF-S1, the audit's G27-G30, G33-G38, G40-G42), the superblock's move from 35 to 37 (a `v3.4.12` volume does not mount), the suite, the overhead files, and the gaps that bound it - CX's close regression first, unattributed |
| **Does not settle** | the regression's bisect; CX-Q36 (b), CX-Q37 (b)/(c); anything after `8dc37b80` |
| **Recorded at** | this file; `CLAUDE.md`'s version of record; this file's row in `instructions/v3.2.0/index.md` |

## 2. CZ pushed with the hook skipped, C1 alone; the merged tree not suite-run

| | |
|---|---|
| **Context** | CZ closed on 2026-11-18 with C1 built, on worktree `cz-checkpoint-roll-wait` at `f8ffccea`, not merged and not pushed (`raft-marks-2026-11-18.md` §50). A session rooted outside the repository reported that state on 2026-11-19, with `origin/main` 12 commits ahead of the branch's base `9712f5f0` (CX, CY, `v3.4.13`), and then C1 against C2 from CZ-S1 and CZ-S1b |
| **Word** | *"Push CZ, C1 only, no verify."* and, while the merged tree's build was running, *"Just push; I am ending the session right away."* (given in Korean; recorded in English) |
| **Mark** | None. **CZ-S3 and CZ-S4 go to `main` with `--no-verify`, C1 alone**: `origin/main` at `11f72ce9` merged into the work branch at `44de9abc` - conflicts in `instructions/v3.2.0/index.md` and `raft-marks-2026-11-18.md` only, each resolved by keeping both sides, CZ's section renumbered from §47 to §50 there; `src/server/expeditor.cpp`, `tests/CMakeLists.txt`, `CLAUDE.md`, `known-gaps.md`, `sched.md` and `wait-events.md` merged by git with no conflict. C2 is not in it: `src/wal/file_log_device.cpp` is `origin/main`'s, and the spike branch `worktree-cz-s1-spike` is not merged. **The hook was skipped. The full suite was not executed on the merged tree**: it ran 0 failed of 4727 at `74861601`'s build inputs, before CX and CY reached `main`; the Debug build of `44de9abc` was at about half when the second word came, and no test of it had run. **No `critics-developer` review of the merge was run.** The overhead was measured at CZ's close on `74861601` against `9712f5f0`, not on the merged tree |
| **Does not settle** | whether the merged tree builds and passes - the next session owes the full suite at `44de9abc`'s build inputs before anything is built on it; whether BL's bar (v) met in the probe's cell closes BL-R7's remainder; the spike branch's deletion; DG-Q0..Q10 - DG stays unopened, and C2 with it; a tag, a version |
| **Recorded at** | this file; the CZ order's header and §7; the CZ row and this file's row in `instructions/v3.2.0/index.md` |

## 3. CX's close regression bisected

| | |
|---|---|
| **Word** | *"first, bisect and investigate the cause of the 1 % to 2 % slowdown"* - the item §1 left unsettled, the point-statement regression CX's close measured and landed with (`raft-marks-2026-11-18.md` §47) |
| **Mark** | None; an investigation. **The cost enters at DF-S1** (`58d258a3` to `ee89842e`) in the one server lifetime measured with an arm at every stage; CX-S0 to CX-S3a are at zero, CX-S3b to the close and CY sum to nothing; on `main` at `11f72ce9` the regression is +0.32, +0.39 and +0.53 µs of server CPU a point statement at `cores = 1`, pooled over four lifetimes. Inside DF-S1 the compile-time clock costs nothing and the session cost sample is the likeliest carrier, not separated from a binary's layout. The CX close's attribution of +0.29 to +0.50 µs to the later CX stages is withdrawn. No engine source changed; the suite was not executed (bench and documents only) |
| **Does not settle** | whether DF-S1 is over DF-Q10 (a)'s 0.5 µs bar (under by the pooled median, not resolved from it; over on both joins in the single lifetime); any remedy - CLA's proposal, a cheaper session sample, is unbuilt and the operator's to open; a write's share of DF-S1; `cores` above 1 |
| **Recorded at** | `bench/v3.4.13/results-cx-close-bisect-v3.4.13-1-g11f72ce9.md`; `workorder-df-statistics-coverage.md` §7; `workorder-cx-managed-plans.md` §8; this file's row in `instructions/v3.2.0/index.md` |

## 4. The bisect closed; DF-S1 stands with its measured cost

| | |
|---|---|
| **Word** | *"push no verify; close the bisect for now and confirm every feature built so far (the feature that causes the cost is not reverted)"* - after CLA's report of §3 and its answer that the statistics widening, DF-S1, is where the cost enters, at 0.7 to 0.8 % of a point statement's throughput pooled |
| **Mark** | **The bisect is closed. DF-S1 is not reverted**: `sys.statement_stats`' widening, `sys.statement_histogram` and `sys.refusal_stats` stand as built, with the cost §3 measured, and so does every other feature on `main`. §3's work goes to `main` with `--no-verify`: the hook was skipped and no suite was executed (bench and documents only, no build input changed) |
| **Does not settle** | a remedy for the cost - CLA's proposal of a cheaper session sample stays unbuilt and unopened; DF-Q10 (a)'s reading for the parts DF has still to build (B to E), each priced at its own stage; a write's share of DF-S1 and `cores` above 1, unmeasured; no version is named by this word and no tag is made |
| **Recorded at** | this file; `workorder-df-statistics-coverage.md` §7; this file's row in `instructions/v3.2.0/index.md` |

## 5. DC opened on the standing go-ahead, and closed on its work branch

| | |
|---|---|
| **Word** | *"The SELECT FOR UPDATE milestone"* with the skill `go-ahead-achieving-milestone` (given in Korean; recorded in English), whose standing word is: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* |
| **Mark** | **DC is opened**, on worktree `dc-select-for-update` from `8e7c820f` (`v3.4.13-10-g8e7c820f`). **No DC-Q item is the operator's own mark.** Each is **adopted on the operator's standing go-ahead of 2026-11-19 (go-ahead-achieving-milestone)**, as CLA proposed it: DC-Q0..Q10 each (a), and every item a stage raises, each recorded in the order's §4 with its options before it is adopted. The two `[quiet-wrong]` items, DC-Q1 (`U` under the spelling `FOR UPDATE`) and DC-Q3 (the write's verdict, unchanged), were adopted after their premises were re-read on the tree at `8e7c820f` (the order's §7, DC-S0) |
| **Closed** | DC-Q11 (which refusals fail the transaction) and DC-Q12 (the reply's bound) were raised by DC-S0 and adopted (a) the same way. **DC is closed on worktree `dc-select-for-update`, its code at `7e18598d` (`v3.4.13-16-g7e18598d`)**: suite 0 failed of 4794 there; the close measured (`bench/v3.4.13/results-dc-close-v3.4.13-14-gcd80d31d.md`, `...-16-g7e18598d.md`) - no increment lost through the clause, 2.70 % of locking reads refused in `READ COMMITTED`'s window, a point `SELECT` without the clause inside the floor with a 0.1 us lean not excluded; `origin/main` had not moved from `8e7c820f`. Not merged, not pushed, not tagged |
| **Does not settle** | a push, a merge to `main`, a tag, a version - each waits for the operator's word; DC-Q0 (b), DC-Q1 (b)/(c), DC-Q3 (b) and every other option not taken, which stay open to the operator's own mark; CR, which stays written and not opened |
| **Recorded at** | this file; the DC order's header, §4 and §7; the DC row and this file's row in `instructions/v3.2.0/index.md` |

## 6. DC pushed with the hook skipped

| | |
|---|---|
| **Word** | *"push no verify"* - after CLA's report that DC was closed on worktree `dc-select-for-update` at `f1e3a393`, its code at `7e18598d`, with the full suite 0 failed of 4794 at `7e18598d`'s build inputs and the close measured (§5) |
| **Mark** | None. **DC goes to `main` with `--no-verify`**: a `--no-ff` merge of `worktree-dc-select-for-update` onto `origin/main` at `8e7c820f`, which had not moved since DC opened from it, so the merged tree is the work branch's and no build input differs from the one the suite ran on. **The hook was skipped.** The suite it would have run was executed on these build inputs before the push: 0 failed of 4794 at `7e18598d` (the commits after it are bench and documents only). Not executed for DC: a `tsan` build and the `bs-nightly` run. The overhead was measured at the close (`bench/v3.4.13/results-dc-close-v3.4.13-14-gcd80d31d.md`, `bench/v3.4.13/results-dc-close-v3.4.13-16-g7e18598d.md`) |
| **Does not settle** | a tag, a version - no version is named by this word and no tag is made; the options the standing go-ahead did not take (DC-Q0 (b), DC-Q1 (b)/(c), DC-Q3 (b), DC-Q5 (b)/(c), DC-Q6 (b)/(c), DC-Q7 (b), DC-Q12 (b)), which stay open to the operator's own mark; the point `SELECT`'s 0.1 us lean, neither shown nor excluded; CR, written and not opened |
| **Recorded at** | this file; the DC order's header and §7; the DC row and this file's row in `instructions/v3.2.0/index.md` |

## 7. v3.4.14 named and tagged

| | |
|---|---|
| **Word** | *"tag 3.4.14"* - after CLA's report that DC landed on `main` at `35086008` with the hook skipped and the full suite 0 failed of 4794 on its build inputs (§6) |
| **Mark** | **The version of record is `v3.4.14`**, the operator's own word: an annotated tag on `main` at `35086008`, pushed. Its message states what is built since `v3.4.13` (DC, and CZ's C1), that the superblock did not move, the suite, DC's close measurement, and the gaps that bound it - first that `FOR UPDATE` takes `U`, PostgreSQL's `FOR NO KEY UPDATE` |
| **The record's push** | *"push no verify"* - after CLA's report that the tag was pushed and its record (`CLAUDE.md`'s line, this section, the index row) was committed on `worktree-dc-select-for-update` and not pushed. The record goes to `main` with `--no-verify`, a fast-forward of two documents-only commits; the hook was skipped and no suite was executed for them (no build input changed) |
| **Does not settle** | anything after `35086008` but this record; the options DC's standing go-ahead did not take |
| **Recorded at** | this file; `CLAUDE.md`'s version of record; this file's row in `instructions/v3.2.0/index.md` |

## 8. DB opened on the standing go-ahead

| | |
|---|---|
| **Word** | *"schedule the DB work for 4 hours from now /go-ahead-achieving-milestone"* (given in Korean, recorded in English) - at 08:38 UTC, after CLA listed the open milestones; the scheduled run started at 12:38 UTC |
| **Mark** | None of the operator's own. **DB opened** on worktree `db-torn-page-healing` from `origin/main` at `734484ca` (`v3.4.14-2-g734484ca`), under the skill's standing word: CLA's proposals are adopted. **DB-Q1..Q6 adopted (a)**, DB-Q1 and DB-Q2 as DB-S0 revised them against the tree (the order's §4 and §7), and **DB-Q7, added at DB-S0, adopted (a)** - each on the operator's standing go-ahead of 2026-11-19 (go-ahead-achieving-milestone) |
| **Does not settle** | a push, a merge to `main`, a tag or a version; the options the standing go-ahead did not take (DB-Q1 (b)/(c), DB-Q2 (b), DB-Q3 (b), DB-Q4 (b), DB-Q5 (b)/(c), DB-Q6 (b), DB-Q7 (b)), which stay open to the operator's own mark |
| **Recorded at** | this file; the DB order's header, §4 and §7; the DB row and this file's row in `instructions/v3.2.0/index.md` |

## 9. DB closed on the work branch, on the standing go-ahead

| | |
|---|---|
| **Word** | the same standing go-ahead as §8; no new word |
| **Mark** | None of the operator's own. DB-S1 (`c802fac9`), DB-S2 (`ec057c06`) and DB-S3 (documents and the close measurement) landed on `worktree-db-torn-page-healing`; DB-Q5's deltas recorded with no bar applied (`bench/v3.4.14/results-db-close-v3.4.14-5-gec057c06.md`) |
| **Does not settle** | a push, a merge to `main`, a tag or a version; the operator's ruling on the -11.7 % (100K-row updates, `cores = 4`) and -25.3 % (pool pressure) deltas |
| **Recorded at** | this file; the DB order's header and §7; the DB row in `instructions/v3.2.0/index.md` |
