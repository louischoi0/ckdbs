# Raft marks, 2026-11-17

The operator's words of 2026-11-17 that open a new matter, and what CLA
took under them.

## 1. The optimistic page read, and a work order written

| | |
|---|---|
| **Word** | W1, on 2026-11-17, after a conversation on Oracle's current and consistent-read buffers, on which unit an optimistic read belongs to, and on what the tree does today: *"Then write a milestone for the optimistic page read - the measurement, the validity verdict, the implementation and so on."* (given in Korean; recorded in English) |
| **Mark** | **`workorder-cw-optimistic-page-read.md` is written and not opened.** The letter CW is CLA's proposal. The order is exploratory: its first two stages measure, and either may close it with nothing built |
| **Does not settle** | the opening of CW or of any stage; CW-Q0 to CW-Q9, each CLA's proposal only - CW-Q1 is the bar the verdicts are read against; the pending question `docs/pending/multicore-scaling-cores4-below-cores2.md`, which stays the operator's; a version |
| **Recorded at** | this file; the order's header and §7; the CW row and this file's row in `instructions/v3.2.0/index.md` |

## 2. The order pushed

| | |
|---|---|
| **Word** | W2, on 2026-11-17, while §1's order was being written: *"Once the cw-optimistic-page-read milestone document is written, push it right away, no verify."* (given in Korean; recorded in English) |
| **Mark** | The work order, this file and the index rows are merged to `main` with `--no-ff` and pushed with `--no-verify`. **Skipped: the pre-push hook, the full suite (not executed) and the `critics-developer` review (not run).** The commit changes no build input - documents under `instructions/` only |
| **Does not settle** | the opening of CW; a tag, a version |
| **Recorded at** | this file; the order's §7 |
