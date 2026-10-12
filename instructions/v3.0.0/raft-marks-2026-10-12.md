# Ratification marks — 2026-10-12

**The operator's words of 2026-10-12**, recorded by CLA in the main
checkout on `main` at `ade5bc20` (`v2.7.0-843-gade5bc20`; the v3.0.0 tag is
not cut); §4-§5 on `worktree-bl-drop-session-affinity` from `1aebdfe8`
(`v2.7.0-854-g1aebdfe8`). Every word is recorded **in English**, translated from the
Korean it was typed in.

## 1. A direction for scans asked for

| | |
|---|---|
| **Word** | *"The scan is slow right now. Propose ways to improve it, and a direction for scans."* |
| **Mark** | None: a question answered. CLA reported BN's close measurement (the read/update order, lettered BK until the collision was resolved on 2026-10-12) and the per-row repetitions of the walk, and proposed four directions in order: (A) the walk owning its slot loop and a compiled filter that rejects before it decodes; (B) a recent row's visibility, measured first; (C) cold-scan read-ahead, measured first and gated on the I/O-backend decision; (D) not now - page summaries and intra-statement parallelism |
| **Does not settle** | Anything |
| **Recorded at** | `workorder-bm-scan-row-kernel.md` §0.1 |

## 2. The scan kernel is not the scan ring - CLA's answer, recorded

| | |
|---|---|
| **Word** | *"Is the scan kernel the same concept as the scan ring we reviewed before?"* |
| **Mark** | None: a question answered. CLA answered no. The scan ring (`eviction.md` EV6) and BG's scan queue (`workorder-bg-scan-queue.md`) decide which frames a scan occupies and in what order they are reclaimed. That is the buffer pool's question, and it acts only when a scan and the working set compete for frames. The kernel is the executor's per-row CPU on pages already resident, which is what BN's 67 ns a row measured (the read/update order, lettered BK at the time). Neither moves the other's number. Cold-read latency is a third question |
| **Does not settle** | Anything |
| **Recorded at** | `workorder-bm-scan-row-kernel.md` header (W1) and §1.8 |

## 3. BM written and pushed

| | |
|---|---|
| **Word** | *"Write the workplan document for that improvement and push it."* |
| **Mark** | BM-S0: the order written (`workorder-bm-scan-row-kernel.md`) and pushed to `main` directly from the main checkout, with no worktree and no work branch, as a document-only change. The pre-push hook did not run, because it is not enabled in this clone; a documentation-only push is the case its own text names as legitimate for `--no-verify` (`scripts/githooks/pre-push:24-28`). No build and no suite ran. **Not opened**: BM-Q0..Q12 are unmarked. The letter BM is CLA's choice (BM-Q11) |
| **Does not settle** | BM-Q0..Q12, among them the kernel's authority (BM-Q2, `[quiet-wrong]` under (b)), the NULL operand (BM-Q3) and the sequencing (BM-Q9) |
| **Recorded at** | `workorder-bm-scan-row-kernel.md`; `index.md`'s BM row |

## 4. Session affinity struck from BL

| | |
|---|---|
| **Word** | *"Analyse the BL milestone. And I am thinking of discarding the affinity concept for now - give the grounds for it or the case against."* CLA analysed BL and recommended striking BL's session affinity axis, keeping the move's restraint as the engine's own constants, and treated the namespace's affinity (NS10, D18) as a separate question. The operator answered: *"Discard only session affinity, and amend the BL document."* |
| **Mark** | W4 of BL: BL-R3 is struck, and with it BL-Q5, BL-Q6 and BL-Q9; the balancer reads no session setting and applies one engine-wide hysteresis margin and dwell (BL-R4). It strikes the affinity half of W1 (`raft-marks-2026-10-11.md` §5). Written on `worktree-bl-drop-session-affinity` from `1aebdfe8` as a document-only change |
| **Does not settle** | The namespace's affinity (NS10, D18), which stays as written; BL-Q0..Q4, BL-Q7, BL-Q8 and BL-Q10..Q15, still unmarked; BL still not opened |
| **Recorded at** | `workorder-bl-session-balancer-priority-affinity.md` header (W4), BL-R3, §4, §6; `index.md`'s BL row |

## 5. BL's priority as four context levels

| | |
|---|---|
| **Word** | *"There is something to decide before the document is finalized: how the session priority levels are set. I want to design a set of levels that applies context - a level that puts priority on the single-row updates that carry business logic, a level that runs in the background, a level for query-analysis reads that gives way first, and so on."* (W5). CLA proposed four levels and the items below; asked for more on the latency class, CLA explained why the `transactional` level needs a latency class and why its lag limit replaces a per-statement demotion. The operator answered: *"Update the workplan document according to CLA's proposal."* (W6) |
| **Mark** | **Adopted whole on W6, not marked per item**: BL-Q16 (a), four levels `transactional`, `normal` (the default), `analytic`, `background`; BL-Q8 (a), declared by the session after the handshake, `transactional` alone gated by the `admin` role (BL-R8), engine-derived levels a later direction; the per-statement demotion folded into the lag limit (BL-R2); BL-Q17 (a), `analytic` not read-only; BL-Q18 (a), `background` the idle class with a floor; BL-Q4 (b), a share and a latency class bounded by the lag limit. Written on `worktree-bl-drop-session-affinity` from `1aebdfe8` as a document-only change |
| **Does not settle** | The numbers - shares, the 16-page slice, the lag limit L, the floor F - which stay `[provisional]` until BL-S1 and BL-S5; the spelling that declares a level (BL-S4); BL-Q0..Q3, BL-Q7 and BL-Q10..Q15, still unmarked; BL still not opened |
| **Recorded at** | `workorder-bl-session-balancer-priority-affinity.md` header (W5, W6), BL-R2, BL-R7, BL-R8, §4, §6; `index.md`'s BL row |

## 6. BL's priority served by placement first

| | |
|---|---|
| **Word** | *"Is scheduling on our own, without leaning on the OS, useful enough to be worth developing? Review it from the over-engineering side."* (W7). CLA answered that the reactor's scheduler is not optional, since the OS cannot see a session that is not a thread; that OS priorities for reactor threads would invert on the page latch's spin-then-yield wait; and that BL's weighted pick outweighed its evidence - a pick cannot cut a running poll, its first draft's review found four defects, and no workload asked for it - so the levels should be served by an interactive/batch core partition and a per-level slice first. The operator answered: *"Amend the BL document as CLA proposed, and push it."* (W8) |
| **Mark** | **Adopted whole on W8, not marked per item**: W6's four levels and their privileges stand as the user contract; v0 serves them by placement (BL-R2) and the per-level walk slice; BL-R7's four rungs, rungs 3 and 4 built only when the one below misses §5.2's bar (v) and on the operator's ruling; BL-Q4 (b) deferred to rung 4; OS thread priorities a non-goal. Written on `worktree-bl-drop-session-affinity` on top of `f9f0c6fa` as a document-only change, pushed to `main` on W8 in the same change as this record |
| **Does not settle** | BL-Q19 (the partition's size) and BL-Q20 (borrowing an idle batch core), written with CLA's proposals and left to §7; every number `[provisional]`; BL-Q0 and the other unmarked items; BL still not opened |
| **Recorded at** | `workorder-bl-session-balancer-priority-affinity.md` header (W7, W8), §0, BL-R2, BL-R4, BL-R5, BL-R7, BL-R8, BL-R9, BL-S4, §4, §5.1, §5.2, §6; `index.md`'s BL row |

## 7. BL-Q19 and BL-Q20 marked

| | |
|---|---|
| **Word** | *"Rule BL-Q19 and BL-Q20 as CLA proposed too."* (W9) |
| **Mark** | Marked per item, as CLA proposed: **BL-Q19 (a)** - the partition is an engine constant, `max(1, cores / 4)` batch cores, the highest-numbered, never core 0, sized by BL-S5, and not a setting; **BL-Q20 (a)** - an interactive session does not borrow an idle batch core in v0, taken up only if §5.2 shows the idle capacity material. Written on `worktree-bl-drop-session-affinity` on top of `f9f0c6fa` |
| **Does not settle** | The partition's size as a number, `[provisional]` until BL-S5; BL-Q0 and the other unmarked items; BL still not opened |
| **Recorded at** | `workorder-bl-session-balancer-priority-affinity.md` header (W9), §4, §6; `index.md`'s BL row |

## 8. What shared-nothing left, asked for

| | |
|---|---|
| **Word** | *"Sync the current repo and find what is left over from the shared-nothing era, and the overhead those leftovers cause."* (W0 of BP) |
| **Mark** | None: a question answered. CLA synced the main checkout to `fe248d94` and reported a survey in four groups: cost `cores = 1` pays, structure costing above one core, dead code, and comment and naming residue. Recorded in the main checkout on `main` at `fe248d94`, with no worktree |
| **Does not settle** | Anything |
| **Recorded at** | `workorder-bp-per-core-residue.md` header (W0) and §1 |

## 9. BP written and pushed

| | |
|---|---|
| **Word** | *"Write a work order for that issue: hypothesis, verification, code change, measurement, and re-exploration or rejection where needed."* (W1 of BP) |
| **Mark** | BP-S0: the order written (`workorder-bp-per-core-residue.md`), with W1's five words as its per-hypothesis loop (BP-R0). It was pushed to `main` directly from the main checkout at `fe248d94`, with no worktree and no work branch, as a document-only change, under the standing practice for documents written in a claude.ai session. The pre-push hook did not run, because it is not enabled in this clone; no build and no suite ran. **Not opened**: BP-Q0..Q13 are unmarked. The letter BP is CLA's choice (BP-Q12) |
| **Does not settle** | BP-Q0..Q13, among them the memo's option (BP-Q4, `[quiet-wrong]` under (a), (b) and (d)), the three ratified structures (BP-Q5..Q7) and the sequencing (BP-Q13) |
| **Recorded at** | `workorder-bp-per-core-residue.md`; `index.md`'s BP row |

## 10. BL opened, and how it runs

| | |
|---|---|
| **Word** | *"Work according to the BL milestone. Split it into work units; if a work unit has no utility, explore alternatives thoroughly, and if none is found, reject it, report and stop. The purpose of this milestone is to place cores per transaction rather than per session, to optimize scheduling, and to handle each finely by its character. Establish the four stages, but choose the detailed implementation by experiment and measurement. Not every concept in the order needs to be developed; what experiment and measurement show to be meaningless may be skipped."* (W10; given three times in one session, the last with "explore alternatives thoroughly" added, which is the form recorded) |
| **Mark** | **BL-Q0 opened**, BI-S5 having closed at `083ab01d`; **BL-Q1 (a)**, read from "per transaction"; the four stages are BL-S1..S4, each kept or dropped by measurement |
| **Does not settle** | BL-S4's three rulings, which BL-S4's stop puts to the operator (`workorder-bl-session-balancer-priority-affinity.md` §6): the read path's logged Waystone write, core 0's role in the partition, and the WAL roll's latch; every other unmarked BL-Q item |
| **Recorded at** | `workorder-bl-session-balancer-priority-affinity.md` §4 (BL-Q0, BL-Q1, BL-Q3, BL-Q14), §6; `index.md`'s BL row |

## 11. BL-S4's first ruling marked: `TouchPattern` deleted

| | |
|---|---|
| **Word** | *"What effect would deleting TouchPattern bring? The authoritative character must stay as it is, component by component."*, then, on CLA's answer: *"I will mark it (a), deletion."* (W11; recorded on `worktree-bl-touchpattern-delete` from `d1b88b70`) |
| **Mark** | **BL-S4's ruling 1 is (a)**: `Catalog::TouchPattern` and its one call on the read path are deleted, so a repeated point `SELECT` makes no logged write. `sys.patterns`' `use_count` and `last_seen` stay on disk, written at registration and advanced by nothing. **The constraint is the operator's**: each component keeps its authority class - `sys.patterns` stays wholly logged, with no unlogged field inside a logged page; Waystone stays advisory (invariants 8 and 9); `sys.access_stats` stays the one unlogged catalog relation `rules.md` §5 admits. That constraint is why (b), two unlogged heat fields inside a logged page, is not taken |
| **Does not settle** | BL-S4's rulings 2 (core 0's role under the partition) and 3 (whether a WAL segment roll keeps the stream latch across its device I/O); whether a tool that pins sessions needs a balancer off-switch (`known-gaps.md`'s BL section) |
| **Recorded at** | `workorder-bl-session-balancer-priority-affinity.md` §6; `docs/spec/waystone-concpets.md` §4; `docs/spec/parser-v2.md` (J5); `docs/inflight/known-gaps.md`'s BL section; `manual/sql/sql.md` (`SHOW PATTERNS`) |

## 12. BL paused

| | |
|---|---|
| **Word** | *"Stop this milestone here for now; I will resume it later. Reflect it in the documents."* (W12; recorded on `worktree-bl-touchpattern-delete` at `05cd011f`) |
| **Mark** | **BL is paused, not closed and not abandoned.** BL-S1..S3 and BL-S4's ruling 1 (a) stay built; BL-S4 stays stopped; the ruling 1 re-measurement was stopped with 2 of 18 cells taken and is reported nowhere |
| **Does not settle** | BL-S4's rulings 2 and 3; BL-S5; BL's close and its overhead A/B; when BL resumes |
| **Recorded at** | `workorder-bl-session-balancer-priority-affinity.md` §6 ("BL paused 2026-10-12"); `index.md`'s BL row; `docs/inflight/known-gaps.md`'s BL section; `CLAUDE.md`'s cross-core row |

## 13. BP opened

| | |
|---|---|
| **Word** | *"Open BP, adopt CLA's proposals, S2 first, and remove every citation of the shared-nothing concept (CLAUDE.md and other documents)."* (W2 of BP), with the go-ahead-achieving-milestone skill invoked on it |
| **Mark** | **BP-Q0 opened**, on `worktree-bp-per-core-residue` from `cf889b63`. BP-Q1..Q3, Q6, Q8..Q13 marked as CLA proposed: Q3 (a), Q10 (b), Q11 `kMaxCores`, Q13 (a), BP-S2 first. BP-Q4, Q5 and Q7 marked "decided by BP-S1": their option is adopted from the census on the operator's standing go-ahead. **BP-Q14**, the citation sweep's reach, written after W2 and adopted on the standing go-ahead as (a): live documents and source comments, not dated records. The order is renamed `workorder-bp-per-core-residue.md` |
| **Does not settle** | Any option the census decides, until BP-S1 has run; a push, a merge to `main`, a tag or a version |
| **Recorded at** | `workorder-bp-per-core-residue.md` header (W2), §5; `index.md`'s BP row |

## 14. BQ written: the WAL segment roll

| | |
|---|---|
| **Word** | *"Explain the WAL segment roll, and why you proposed writing it as a separate work order."* (W0 of BQ), *"Isn't it enough to just make the segment file larger?"* (W1), then *"For now, write the work order for the WAL segment roll: experiment, hypothesis verification, measurement, re-exploration, rejection."* (W2; recorded on `worktree-wal-segment-roll-order` from `59c4139f`) |
| **Mark** | **BQ-S0**: `workorder-bq-wal-segment-roll.md` written. W0 and W1 were questions, answered in the session and recorded in the order (§0.2, §1.3, H2). W2 sets the order's method. BQ takes BL-S4's ruling 3 out of BL as its subject |
| **Does not settle** | BL-S4's ruling 3 itself, which BQ answers by measurement; BQ's opening (BQ-Q0) and BQ-Q1..Q5; BL-S4's ruling 2; when BL resumes |
| **Recorded at** | `workorder-bq-wal-segment-roll.md`; `index.md`'s BQ row; `workorder-bl-session-balancer-priority-affinity.md` §6 ("BL paused 2026-10-12") |

## 15. BQ opened

| | |
|---|---|
| **Word** | *"Check whether milestone BP was closed or paused, then start the BQ workplan; go ahead and keep going until the milestone is achieved or a failure or ineffectiveness is discovered."* (W3 of BQ, given as a five-minute loop), then *"Don't wait for BP; start BQ right away."* (W4; both recorded on `worktree-wal-segment-roll-order` at `dbff867b`) |
| **Mark** | **BQ-Q0: opened.** W4 lifts W3's gate on BP: BQ starts while BP-S1 is measuring on the same host, so BQ's builds overlap BP's runs and BQ's own measurements are taken only when no BP run is live. **BQ-Q1..Q5 are marked as CLA proposed**, on W3's *"go ahead and keep going"*: Q1 (a), Q2 (a), Q3 (a), Q4 (b), Q5 (b) - so BQ stops at rung 3 and rung 4 (H6) needs its own word |
| **Does not settle** | Rung 4; BL-S4's rulings 2 and 3 (BQ answers 3 by measurement); BP |
| **Recorded at** | `workorder-bq-wal-segment-roll.md` §6, §7; `index.md`'s BQ row |

## 16. BQ-S1 measured; BQ-Q6 and BQ-Q7 adopted

| | |
|---|---|
| **Word** | *"Refer to the wal-segment worktree and merge the remaining work into this session; plan and orchestrate the work with its ordering and gates in mind; until two milestones close, go-ahead-achieving-milestone: set hypotheses and measure, verify and re-explore, establish, reject."* (the operator, 2026-10-12, translated) |
| **Mark** | BQ joins BP's branch (`6139c2de`). BQ-S1's verdicts are recorded in BQ's §7. **BQ-Q6 (a)** and **BQ-Q7 (a)**, new items written with CLA's proposal, are adopted on the operator's standing go-ahead of 2026-10-12 (go-ahead-achieving-milestone), not as the operator's own marks |
| **Does not settle** | Rung 4 (BQ-Q5 (b) stands); the recycling defect's fix, which is filed and not BQ's; a push, a merge to `main`, a tag or a version |
| **Recorded at** | `workorder-bq-wal-segment-roll.md` §6, §7; `docs/inflight/bugs/a-page-re-dirtied-after-its-writeback-copy-pins-the-redo-start.md` |
