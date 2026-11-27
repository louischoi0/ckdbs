# Raft marks - 2026-11-27

The operator's words of 2026-11-27, recorded in English.

## 1. DV opened on the standing go-ahead; DV-Q0..Q6 adopted

| | |
|---|---|
| **Word** | `/go-ahead-achieving-milestone DV`: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* |
| **Mark** | **DV is opened** on worktree `dv-unreachable-page-reclaim` (branch `worktree-dv-unreachable-page-reclaim`) from `630ecc359` (`v3.4.17-178-g630ecc359`). **DV-Q0..Q6 are adopted as CLA proposed, (a) in each, on the operator's standing go-ahead of 2026-11-27 (go-ahead-achieving-milestone)** - CLA's proposals, not per-item marks by the operator: Q0 (a) census, source fixes, then the sweep, S0's numbers deciding whether the sweep is reached; Q1 (a) S0 gates, a class under one page per 1,000 operations recorded and left; Q2 (a) the sweep runs on an administrator's statement only; Q3 (a) a mount's census only when a boot key asks; Q4 (a) Waystone's stranded directories freed at the renewal and at `OFF`; Q5 (a) leaked spills outside DV; Q6 (a) a restore's mount runs the sweep when asked. No item is `[quiet-wrong]`; DV-R1 is, and is a rule, not an item. |
| **Does not settle** | a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; `workorder-dv-unreachable-page-reclaim.md`'s header and §4; its rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 2. DV-Q4 revised, DV-Q7..Q9 added and adopted on the same go-ahead

| | |
|---|---|
| **Word** | the standing go-ahead of §1 - no new word from the operator |
| **Mark** | CLA's proposals, written into the order's §4 at DV-S0 and DV-S2 and **adopted on the operator's standing go-ahead of 2026-11-27 (go-ahead-achieving-milestone)**, not per-item marks by the operator: **DV-Q4' (a)** - DV-Q4 (a) revised against the tree: Waystone's stale directories are taken back at the mount's renewal only, trail pages freed, interior pages reused as fresh roots, `OFF` left (a headerless id is never freed, `device_page_store.cpp:1785`; an `OFF` would need a statement-epoch fence against a recorder holding the old root); **DV-Q7 (a)** - the sweep (DV-S3) is not reached: S0 measured no class without a source over DV-Q1's bar, so DV closes narrow; **DV-Q8 (a)** - a rolled-back create whose in-run job a crash loses is a stated known gap; **DV-Q9 (a)** - `sys.volume_census` reads the mount's census, never one taken at a read (DV-R2). None is `[quiet-wrong]`; DV-Q4' and DV-Q7 each stand beside DV-R1, which is. |
| **Does not settle** | a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; `workorder-dv-unreachable-page-reclaim.md` §4 and §7 |

## 3. DV-Q10 added and adopted on the same go-ahead

| | |
|---|---|
| **Word** | the standing go-ahead of §1 - no new word from the operator |
| **Mark** | **DV-Q10 (a)**, CLA's proposal written into the order's §4 at the review of DV-S2 and **adopted on the operator's standing go-ahead of 2026-11-27 (go-ahead-achieving-milestone)**, not the operator's own mark: DV-R6 is built narrower than written - the simulator fails a seed whose census mark is not whole and reports the unreached count, and the zero per closed source is each source's cell - because a page's class does not name the source that leaked it. Not `[quiet-wrong]`. |
| **Does not settle** | a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; `workorder-dv-unreachable-page-reclaim.md` §4 |
