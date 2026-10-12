# Ratification marks — 2026-10-12

**The operator's words of 2026-10-12**, recorded by CLA in the main
checkout on `main` at `ade5bc20` (`v2.7.0-843-gade5bc20`; the v3.0.0 tag is
not cut). Every word is recorded **in English**, translated from the
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
