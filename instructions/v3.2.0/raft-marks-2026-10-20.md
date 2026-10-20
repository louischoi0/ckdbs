# Ratification marks — 2026-10-20

The operator's words of 2026-10-20, recorded in English. Each section names
what the word settles and what it does not.

## 1. BO resumed on the standing go-ahead

| | |
|---|---|
| **Word** | *"BO /go-ahead-achieving-milestone"* - *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* - after CLA's listing of the open milestones, in which BO (`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md`) stood in progress since 2026-10-14 with S6, S7 and S8 open |
| **Mark** | **BO resumed** in the worktree `bo-relation-stats-checkpoints`, from `origin/main` at `7f204bbf` (`v3.3.0-22-g7f204bbf`); its earlier stages are on `main` from the merge of `bo-show-sugar`. Every item already marked stays as marked. Items that surface are adopted on the standing go-ahead of 2026-10-20 (go-ahead-achieving-milestone), not marked by the operator item by item: **BO-Q14 (b)** - the close measurement's base is the closing commit with BO's statement-path recording compiled out, not `cd7c7c4e`; **BO-Q15 (b)** - `sys.relation_stats`' bound counts dropped relations, stated rather than pruned; **BO-Q16 (a)** `[quiet-wrong]` - the views reporting a declaration's enforcement (`sys.assertions`, `sys.cabins`, `sys.transitions`) stay unfiltered diagnostics, stated in `ddl-transactional.md` §5a. All three are in the order's §5 |
| **Does not settle** | push, merge to `main`, tag, version |
| **Recorded at** | this file; the order's header, §5 and §7; `instructions/v3.0.0/index.md`'s BO row |

## 2. BO closed; pushed with the hook skipped

| | |
|---|---|
| **Word** | *"push no verify and close the milestone"* - after CLA's report that every stage of BO was built and its close measured |
| **Mark** | **BO closed** (`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md`): S0..S3 and S5 on `main` since 2026-10-14, S4 not run (BO-Q5 (b)), S6 and S7 `86d1fe1a`, S8 `7ba9180b`, `c462ba08`, `6b6add74`, `c6c74879`; the close measured no cell outside the spread, 0.35-0.52 us of server CPU per point statement (`bench/v3.3.0/results-bo-close-v3.3.0-23-g86d1fe1a.md`); `origin/main` had nothing new to merge, and the full Debug suite ran 4180/4180 at `6b6add74`, the last commit that changed a build input. **Pushed to `main` with `--no-verify`** on this word. The decisions adopted on the standing go-ahead are the order's BO-Q14..Q16 |
| **Does not settle** | a tag, a version; the gaps `known-gaps.md`'s BO section names |
| **Recorded at** | this file; the order's header; `instructions/v3.0.0/index.md`'s BO row and `instructions/v3.2.0/index.md`'s row for this file |

## 3. BR, BZ, BY, BV and CB closed

| | |
|---|---|
| **Word** | *"close BR, BZ, BY, BV, CB"* - after CLA's listing of the open milestones, in which the five stood built with their close run and no close mark recorded |
| **Mark** | **Closed on the operator's word**, each as built and measured, its code on `main` (checked at `4dfed311`): **BR** (`instructions/v3.0.0/workorder-br-backup-restore.md`), built to its close 2026-10-12, measured in `bench/v3.0.0/results-br-close-backup-restore-v2.7.0-913-ga8110b05.md` and `results-br-close-overhead-v2.7.0-913-ga8110b05.md`; **BV** (`workorder-bv-transition-constraints.md`), BV-S4's close 2026-10-14; **BY** (`workorder-by-reindex-drop-index-reclaim.md`), BY-S4's close 2026-10-16, `bench/v3.2.0/results-by-close-v3.2.0-119-g9da66f0e.md`; **BZ** (`workorder-bz-heap-hidden-key.md`), S1-S3 and the close measured, `bench/v3.2.0/results-bz-close-v3.2.0-153-g18e7b511.md` (about 92 minutes, over the one-hour budget); **CB** (`workorder-cb-join-planner.md`), S1 `bf0637da` and S2 `0640941f`, the close measured at `62adcf76`. BR's close lifts the gate that held BS-S2..S8 (`instructions/v3.0.0/workorder-bs-test-hardening.md`) |
| **Does not settle** | a tag, a version; each order's open gaps as `known-gaps.md` states them; BS's next stage |
| **Recorded at** | this file; each order's header; the five index rows (`instructions/v3.0.0/index.md` for BR, `instructions/v3.2.0/index.md` for the other four and for this file) |

## 4. CE written

| | |
|---|---|
| **Word** | *"RN7 client transport while executing plan new milestone"* - after the scenario re-measurement with the fixed client decoder, which put 56 % to 84 % of the remaining many-row gap to PostgreSQL in the reply leaving only at the statement's completion (BN-R7) |
| **Mark** | **CE written** (`workorder-ce-streaming-reply.md`), read as BN-R7, the rule it amends: rows of a plain `SELECT` leave at the walk's slice boundaries while it runs. **Not opened**: CE-Q0..Q6 are CLA's proposals, unmarked |
| **Does not settle** | any of CE's questions; the opening |
| **Recorded at** | this file; the order's header; `instructions/v3.2.0/index.md`'s rows for the order and this file |

## 5. CE opened

| | |
|---|---|
| **Word** | *"CE-Q0..Q6 $!a, go ahead"* - after CLA's listing of CE's questions |
| **Mark** | **CE opened** (`workorder-ce-streaming-reply.md`): CE-Q0..Q6 each marked (a) by the operator. The opening review's findings (`2c43d900`) adopted on this go-ahead as CE-A1..A8 (the order's §6), among them **CE-A1**, a live `[quiet-wrong]` defect: a BA-S14 re-run of a `SELECT` answers the first attempt's rows again over KWP; fixed in CE-S1 |
| **Does not settle** | push, merge to `main`, tag, version |
| **Recorded at** | this file; the order's header and §6; `instructions/v3.2.0/index.md`'s rows for the order and this file |

## 6. CE closed; pushed with the hook skipped

| | |
|---|---|
| **Word** | *"push no verify and close the milestone"* - after CLA's report that every stage of CE was built, reviewed and its close measured |
| **Mark** | **CE closed** (`workorder-ce-streaming-reply.md`): S0 `042cb59a`, S1..S4 `9e568b44`, S5 `c3ce8d55`, `89320fff`, `380f6a1d`, `cf655187`, `5f4d64fe`; the close measured (`bench/v3.3.0/results-ce-close-v3.3.0-52-g9e568b44.md`: first batch -54 % to -93 % past one slice, idle removed 0.95-0.99 of CE-S0's ceiling, point statements flat) and its one cost not reproduced (`results-ce-drain-attribution-v3.3.0-52-g9e568b44.md`); `origin/main` merged at `e09f3b9d`, where the full Debug suite ran 4189/4189. **Pushed to `main` with `--no-verify`** on this word |
| **Does not settle** | a tag, a version; the gaps `known-gaps.md`'s "Streamed replies (CE)" names |
| **Recorded at** | this file; the order's header; `instructions/v3.2.0/index.md`'s rows for the order and this file |

## 7. AL, AN, AU and AV closed on paper

| | |
|---|---|
| **Word** | *"close AL, AN, AU, AV on paper"* - after CLA's listing of the open milestones, in which the four AR0-chain orders stood with no close mark recorded while what they built is in force |
| **Mark** | **Closed on paper, on the operator's word**: the record catches up with the tree, and no close stage or close measurement is run for any of them (checked at `244b031c`). **AL** (`instructions/v3.0.0/workorder-al-m0-single-wal.md`), AR0 M0: AL-S0..S9 built 2026-09-02/03, AL-S8 measured at `f6ed10c`, its review row left "in flight" with no follow-up row; the instance has one log stream and a pre-M0 volume does not mount (`wal.md`). **AN** (`workorder-an-read-view.md`): AN-S0..S4 built (AN-S2 the cutover, `18fed497`), AN-S5 half measured and half blocked as its stage log states; every reader is a commit-LSN snapshot (`txn.md` §4). **AU** (`workorder-au-ring-retirement.md`): AU-S1, S1b, S1c, S2 and S3 built; S4 rode AM-S2 and AO-S5 and S5 ran inside AT, neither as an AU stage, and S6 was built as AT-S10d (`6c643d40`); AT closed at AT-S20 (`8627f0d0`). **AV** (`workorder-av-two-core-rig.md`): the rig built at `62ab2e05` (AV-S0..S2); AV-S3's hosting is recorded cell by cell - cells 4, 6 and 7 since AU-S2 (`a8fa35e3`), cell 5 since AO-S4b (`54e7854d`), cell 8 restated as absent. Each order's unrun remainder stays unrun, and none is reported as a pass |
| **Does not settle** | a tag, a version; AN-S5's blocked half; the gaps `known-gaps.md` states for the log, the read view and the wake path |
| **Recorded at** | this file; each order's header; the four rows of `instructions/v3.0.0/index.md` and this file's row in `instructions/v3.2.0/index.md` |
