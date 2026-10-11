# Ratification marks — 2026-10-11

**The operator's words of 2026-10-11**, recorded by CLA in the main
checkout on `main` at `ab2c46d0` (`v2.7.0-771-gab2c46d0`; the v3.0.0 tag is
not cut). All of it is the operator's words, translated into English, or
the option the operator chose where CLA asked with options.

## 1. This session's documents go to `main` directly

| | |
|---|---|
| **Word** | *"In this session it will be almost only documents, so push directly."*, answering CLA's question of whether the session's documents land by a direct push to `main` or by a branch and a pull request |
| **Mark** | A direction for this session: a document CLA writes in it is committed in the main checkout and pushed to `main`, with no worktree and no work branch. A push of documents only skips the pre-push hook under the hook's own exemption, and the reply says so |
| **Does not settle** | Code: a change that touches code still runs the Session Workflow and its gates. Any other session |
| **Recorded at** | This file; `workorder-bk-sys-statistics-show-sugar.md`'s header |

## 2. Statistics tables wanted; five proposed, one excluded

| | |
|---|---|
| **Word** | *"Right now ckdbs has only the tables the sys catalog strictly needs, so the meta-information tables for analysis, operations and profiling are limited. Pick about five tables worth adding."* CLA proposed `sys.statement_stats`, `sys.relation_stats`, `sys.lock_waits`, `sys.checkpoints` and `sys.sessions`; asked which to leave out, the operator chose *"Exclude `sys.sessions`."* (the option CLA had marked as its recommendation) |
| **Mark** | BK-R1: four relations, `sys.sessions` not among them |
| **Does not settle** | BK-Q0..Q13 |
| **Recorded at** | `workorder-bk-sys-statistics-show-sugar.md` §2 |

## 3. `SHOW` is sugar; BK written and pushed

| | |
|---|---|
| **Word** | *"Write a workplan that adds four tables, and it must contain this: keywords such as `SHOW META` and `BUDGET` are only syntactic sugar, and the same result must be referenceable through `SELECT` as well. And push it."* |
| **Mark** | BK-R2 (SV1-SV4): every `SHOW` target is one `SELECT` over one `sys` view, the same result through either spelling, nothing readable only through `SHOW`. BK-S0: the order written and pushed to `main`. **Not opened**: BK-Q0 is unmarked |
| **Does not settle** | BK-Q0..Q13, among them the rendering of a `SHOW` reply (BK-Q1), each view's filter class (BK-Q2, `[quiet-wrong]`) and whether three of the relations are persisted at the cost of a superblock bump (BK-Q5) |
| **Recorded at** | `workorder-bk-sys-statistics-show-sugar.md` §2, §7; `index.md`'s BK row |

## 4. No session is pinned; a balancer between connection and core

| | |
|---|---|
| **Word** | *"I am thinking of a scheme with a balancer between the connection and the core, and scheduling according to the session's priority. That is, a session is never pinned to a core at any time."* |
| **Mark** | BL-R1: a session is placed at its accept and may move at any transaction boundary. The operator's ruling on `docs/pending/session-load-balancing.md`, which BL-S0 deletes |
| **Does not settle** | The grain of "never pinned" - session or statement (BL-Q1, `[quiet-wrong]` under the statement); the carrier (BL-Q2) |
| **Recorded at** | `workorder-bl-session-balancer-priority-affinity.md` header, §2 |

## 5. Two session axes, priority and affinity

| | |
|---|---|
| **Word** | *"So the session has to be split along two axes, priority and affinity. Priority is how many resources are allocated, and how quickly, within one core; the affinity setting is how much the session moves between cores."* |
| **Mark** | BL-R2 (priority: a share of a contended core) and BL-R3 (affinity: the cost the balancer charges a move) |
| **Does not settle** | BL-Q4..Q9: priority's components, affinity's form, whether the axes interact, whether the settings are user-visible, and the name, since `affinity` already names a namespace's quantity |
| **Recorded at** | `workorder-bl-session-balancer-priority-affinity.md` §0.1, §2 |

## 6. BL written, on the premise that BI has closed

| | |
|---|---|
| **Word** | *"Now write a work order for this, including the measurements, the work and the intent. If one already exists, amend that work order. Write it on the premise that BI has been completed."* |
| **Mark** | BL-S0: the order written and pushed to `main` (§1's direction for documents). No order on `main` held the subject, so none was amended. **Not opened**: BL-Q0 is unmarked, and proposes opening only after BI-S5 closes |
| **Does not settle** | BL-Q0..Q15; BI-Q0..Q11, which are BI's |
| **Recorded at** | `workorder-bl-session-balancer-priority-affinity.md`; `index.md`'s BL row |

## 7. A balancer is recommended in any case - CLA's answer, recorded

| | |
|---|---|
| **Word** | *"In any case, putting a balancer between the session and the core is recommended, right?"* CLA answered yes, on two conditions: the balancer is a decision each core takes at a transaction boundary, not a central dispatcher on every transaction's path; and its premise is measured first, since BA's census ran no balanced arm |
| **Mark** | None: a question answered. The two conditions are written into BL as BL-R4 and BL-S1 |
| **Does not settle** | Anything |
| **Recorded at** | `workorder-bl-session-balancer-priority-affinity.md` header (W3) |

## 7. BI opened, every item adopted as CLA proposed

| | |
|---|---|
| **Word** | *"Read the work order for the BI milestone and start the work. Do not stop working until the milestone closes; every matter that needs a decision follows CLA's proposal. Start the work."* (translated), given with the `go-ahead-achieving-milestone` skill |
| **Mark** | **Adopted on the operator's standing go-ahead of 2026-10-11 (go-ahead-achieving-milestone), not marked per item:** BI-Q0 yes (BI opens, BI-R3..R5 as written); BI-Q1 (a); BI-Q2 (a); **BI-Q3 (a) `[quiet-wrong]`**; BI-Q4 (b); BI-Q5 (a); BI-Q6 (b); BI-Q7 (a); BI-Q8 yes; BI-Q9 as written; BI-Q10 as written; BI-Q11 as written; **BI-Q9-R (a)** (BI-Q9's gate tripped on (ii) by its letter; `pages_per_step` stays 64), written into the order at BI-S1, **BI-Q12 (a)**, written into the order at BI-S2, **BI-Q6-R (a')** (BI-Q6's premise was stale: the log writer already kicks a `DurableWait`), **BI-Q10-R (a)** (BI-Q10 (i) read as the checkpoint task's own polls; `kGateParks` 16), written into the order at BI-S5, **BI-Q13 (a)** (one log park per snapshot, no data-file sync per step), written into the order at BI-S5, and BI-Q4 (a) written because BI-Q4's adopted mark conditioned it on S1 counting the per-run arm non-zero, which S1 did (BI-Q4-T) when the simulation corpus found a step's map write ahead of its pages, and adopted under the same word. Before adopting BI-Q3, CLA re-read its premise on `worktree-bi-checkpoint-off-the-reactor` at `0e6e7d8d`: `FilePageDevice::Sync` still returns an `fsync` failure and records nothing (`src/storage/file_page_device.cpp:241-249`), and a writeback still cleans its frames before any sync, so the premise holds. `worktree-fix-data-file-fsync-sticky-failure` (another session's branch, `454879b1`, not on `main`) makes the failure sticky and stops the log; BI does not build on it and does not touch it. BI-Q9's gate, if it trips, is ruled the same way: CLA writes the ruling into the order with its proposal and adopts it |
| **Does not settle** | Any decision that surfaces mid-stage; each is written into the order with CLA's proposal and adopted under the same word, never recorded as the operator's own mark. A push, a tag or a version still waits for the operator's word |
| **Recorded at** | This file; `workorder-bi-checkpoint-off-the-reactor.md`'s header and §4's mark column; `index.md`'s BI row |
