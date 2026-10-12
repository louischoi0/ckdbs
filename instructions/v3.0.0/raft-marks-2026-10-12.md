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
