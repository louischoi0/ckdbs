# Raft marks - 2026-11-27

The operator's words of 2026-11-27 (UTC), recorded in English.

## 1. DW opened on a standing go-ahead scheduled three hours ahead; DW-Q0..Q6 adopted

| | |
|---|---|
| **Word** | *"In three hours, /go-ahead-achieving-milestone DW"*, given at 2026-11-27 04:04 UTC and run by the session's one-shot job at 07:04 UTC. The skill it names carries the operator's standing word: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* |
| **Mark** | **DW is opened** on worktree `dw-btree-purge-driver` (branch `worktree-dw-btree-purge-driver`) from `630ecc359` (`v3.4.17-178-g630ecc359`) and worked stage after stage in one session. **DW-Q0..Q6 are CLA's proposals adopted on that standing go-ahead**, recorded as such in the order's §4 "Marks" and never as the operator's own mark on an item: DW-Q0 (a), DW-Q1 (a), DW-Q2 (a), DW-Q3 (a), DW-Q4 (a), DW-Q5 (a), DW-Q6 (a). No DW-Q item is tagged `[quiet-wrong]`; the rule DW-R1, which is, changes nothing the judge decides. **DW-Q4 stays at (a)**: the keyed tombstone, (c), needs the operator's own word by the order's text, and the standing word is not that |
| **Does not settle** | a push, a merge to `main`, a tag or a version; DW-Q4 beyond (a); any milestone but DW |
| **At the close** | **DW closed at `f85aa3042` and nothing was pushed.** Three things in it are CLA's and not the operator's, each recorded as such in the order: **the reading of DW-Q1's bar** at DW-S0 (a point read pays where key values recur and nothing where they do not, read as "the bar is not met"); **DW-Q7**, an item CLA raised at DW-S2 and revised from its own (a) to (b) at the close, which adds a count and so **amends DW-Q2 (a)**, "existing counters only", as adopted; and **the reading of the close** - two measured costs with the key on (a row flipping between two values, 7.2 % of server CPU a statement for nothing; a slice's stall on core 0) taken as the mechanism's price and written into `known-gaps.md`, the other reading left to the operator in the order's §7. **The key is off by default and nothing here turns it on** |
| **Recorded at** | this file; `workorder-dw-btree-purge-driver.md` (header, §4 "Marks", §7); DW's rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 2. DV opened on the standing go-ahead; DV-Q0..Q6 adopted

| | |
|---|---|
| **Word** | `/go-ahead-achieving-milestone DV`: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* |
| **Mark** | **DV is opened** on worktree `dv-unreachable-page-reclaim` (branch `worktree-dv-unreachable-page-reclaim`) from `630ecc359` (`v3.4.17-178-g630ecc359`). **DV-Q0..Q6 are adopted as CLA proposed, (a) in each, on the operator's standing go-ahead of 2026-11-27 (go-ahead-achieving-milestone)** - CLA's proposals, not per-item marks by the operator: Q0 (a) census, source fixes, then the sweep, S0's numbers deciding whether the sweep is reached; Q1 (a) S0 gates, a class under one page per 1,000 operations recorded and left; Q2 (a) the sweep runs on an administrator's statement only; Q3 (a) a mount's census only when a boot key asks; Q4 (a) Waystone's stranded directories freed at the renewal and at `OFF`; Q5 (a) leaked spills outside DV; Q6 (a) a restore's mount runs the sweep when asked. No item is `[quiet-wrong]`; DV-R1 is, and is a rule, not an item. |
| **Does not settle** | a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; `workorder-dv-unreachable-page-reclaim.md`'s header and §4; its rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 3. DV-Q4 revised, DV-Q7..Q9 added and adopted on the same go-ahead

| | |
|---|---|
| **Word** | the standing go-ahead of §2 - no new word from the operator |
| **Mark** | CLA's proposals, written into the order's §4 at DV-S0 and DV-S2 and **adopted on the operator's standing go-ahead of 2026-11-27 (go-ahead-achieving-milestone)**, not per-item marks by the operator: **DV-Q4' (a)** - DV-Q4 (a) revised against the tree: Waystone's stale directories are taken back at the mount's renewal only, trail pages freed, interior pages reused as fresh roots, `OFF` left (a headerless id is never freed, `device_page_store.cpp:1785`; an `OFF` would need a statement-epoch fence against a recorder holding the old root); **DV-Q7 (a)** - the sweep (DV-S3) is not reached: S0 measured no class without a source over DV-Q1's bar, so DV closes narrow; **DV-Q8 (a)** - a rolled-back create whose in-run job a crash loses is a stated known gap; **DV-Q9 (a)** - `sys.volume_census` reads the mount's census, never one taken at a read (DV-R2). None is `[quiet-wrong]`; DV-Q4' and DV-Q7 each stand beside DV-R1, which is. |
| **Does not settle** | a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; `workorder-dv-unreachable-page-reclaim.md` §4 and §7 |

## 4. DV-Q10 added and adopted on the same go-ahead

| | |
|---|---|
| **Word** | the standing go-ahead of §2 - no new word from the operator |
| **Mark** | **DV-Q10 (a)**, CLA's proposal written into the order's §4 at the review of DV-S2 and **adopted on the operator's standing go-ahead of 2026-11-27 (go-ahead-achieving-milestone)**, not the operator's own mark: DV-R6 is built narrower than written - the simulator fails a seed whose census mark is not whole and reports the unreached count, and the zero per closed source is each source's cell - because a page's class does not name the source that leaked it. Not `[quiet-wrong]`. |
| **Does not settle** | a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; `workorder-dv-unreachable-page-reclaim.md` §4 |

## 5. DV closed narrow on the work branch

| | |
|---|---|
| **Word** | the standing go-ahead of §2 - no new word from the operator |
| **Mark** | **DV is closed narrow on `worktree-dv-unreachable-page-reclaim`**, on CLA's reading of DV-S0 and of the close measurement, adopted on the operator's standing go-ahead of 2026-11-27 (go-ahead-achieving-milestone) and not the operator's own word: the census and two source fixes built, the sweep not (DV-Q7 (a)). The order's §7 carries the close. |
| **Does not settle** | **the landing**: no merge to `main`, no push, no tag and no version - each waits for the operator's word |
| **Recorded at** | this file; the order's header and §7; its rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 6. DV-Q11 added and adopted on the same go-ahead

| | |
|---|---|
| **Word** | the standing go-ahead of §2 - no new word from the operator |
| **Mark** | **DV-Q11 (a)**, CLA's proposal written into the order's §4 at the review of the close and **adopted on the operator's standing go-ahead of 2026-11-27 (go-ahead-achieving-milestone)**, not the operator's own mark: DV-Q1's bar is read by what an operation leaks with no fault armed, so an uncommitted `CREATE TABLE` (3 pages a rollback) is closed and a crash loser's own index root (at most one page a crash landing between two adjacent records) is recorded and left, though both read 2.57 pages a 1,000 ops in the `io` fault cells and 0.05 over the matrix. Not `[quiet-wrong]`. **The review named this the operator's to rule on**; it is listed in the close's report for that reason. |
| **Does not settle** | a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; `workorder-dv-unreachable-page-reclaim.md` §4 |
