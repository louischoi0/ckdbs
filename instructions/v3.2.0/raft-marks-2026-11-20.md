# Raft marks, 2026-11-20

The operator's words of 2026-11-20 that open a new matter, and what CLA
took under them.

## 1. DH written: the page image's price

| | |
|---|---|
| **Word** | *"Since the torn page heal came in, the overhead has grown a lot."* and *"Write a work order for that as an additional milestone."* (given in Korean; recorded in English) - after CLA's reading of `bench/v3.4.14/results-db-close-v3.4.14-5-gec057c06.md` in the same conversation: -11.7 % at 100,000-row updates on four cores, -25.3 % under pool pressure, the standard cells inside the floor |
| **Mark** | **DH is written, not opened**: `instructions/v3.2.0/workorder-dh-page-image-volume.md`, on `main` at `673f025d`, documents only. The letter and the order's exploratory standing (DH-Q0 (a)) are CLA's proposals, not the operator's word. Six levers are laid out (L0 nothing, L1 the hole left out, L2 compression, L3 the writeback rate independent of `cores`, L4 fewer copies under pool pressure, L5 a switch), none built or measured; DH-S0 prices them |
| **Corrected** | CLA told the operator that leaving the free space out of the image was the lowest-risk lever. The tree does not support it as said (the common header names no free region; the payload is the page verbatim; DB-R5 and DB §6 name it a payload change); the order says so in §0 |
| **Does not settle** | DH-Q0..Q6; whether DH opens; the bar (DH-Q2); whether a log format event is admissible (DH-Q3); the price on scenarios 1 to 3, which were measured at a tag that predates DB |

## 2. v3.4.15 named and tagged

| | |
|---|---|
| **Word** | *"tag 3.4.15"* - after CLA's report that DA landed on `main` at `1eeab8ce` with the hook skipped, the full suite 0 failed of 4865 at `89c26252`, whose code is the merge's (`raft-marks-2026-11-19.md` §17) |
| **Mark** | **The version of record is `v3.4.15`**, the operator's own word: an annotated tag on `main` at `1eeab8ce`, pushed by the operator from their own shell (`git push origin v3.4.15`) after the permission classifier refused CLA's push. Its message states what is built since `v3.4.14` (DF, DE narrow, DB, DA), that the superblock did not move, the suite and what was not executed, and the gaps that bound it - first DB's unruled close deltas (-11.7 %, -25.3 %), then DF's cost, DE's narrowness and DA-S3 owed |
| **Does not settle** | anything after `1eeab8ce` but this record; the ruling on DB's close deltas; DA-S3's host |
| **Recorded at** | this file; `CLAUDE.md`'s version of record; this file's row in `instructions/v3.2.0/index.md` |

## 3. DH opened on the standing go-ahead

| | |
|---|---|
| **Word** | The operator invoked `go-ahead-achieving-milestone` with the argument `DH`: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* (the skill's standing word) |
| **Mark** | **DH is open**, on worktree `dh-page-image-volume` from `f4138860` (`v3.4.15-1-gf4138860`). **Every DH-Q item is CLA's proposal adopted under that word; none is the operator's own per-item mark**: DH-Q0 (a) exploratory; DH-Q1 (a), cut to the budget; DH-Q2 (a), which sets **no bar** and leaves it to the operator with the close's numbers; DH-Q3 (a) `[quiet-wrong]`, its premise re-read and then measured by a spike before the format event was admitted; DH-Q4 (a) `[quiet-wrong]`, its premise re-read against the checkpointer and the anchor's fold; DH-Q5 (a); DH-Q6 (a). Items CLA added and adopted the same way: DH-Q7 (the byte counter comes with the packed image), DH-Q8 (DH-S0's A arm is main with the image debt never paid, not the pre-DB commit - a revision of the stage as written), DH-Q9 (L3 then L2 are built; L1 and L4 dropped; L5 unbuilt), DH-Q10 `[quiet-wrong]` (the cadence is the instance's: one run a period, the cores in turn), DH-Q11 `[quiet-wrong]` (a new record kind 35 for the packed image, kind 10 untouched, no version bump), DH-Q12 (zero suppression by word, no dependency), DH-Q13 (only the write fetch's image is packed) |
| **Does not settle** | the bar (DH-Q2); any push, merge to `main`, tag or version; the options not taken, each open to the operator's own mark |
| **Recorded at** | this file; the order's header, §4.1 and §7; the order's row in `instructions/v3.2.0/index.md` |

