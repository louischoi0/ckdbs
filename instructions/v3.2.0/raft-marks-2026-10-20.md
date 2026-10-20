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
