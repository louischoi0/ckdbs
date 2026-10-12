# Ratification marks — 2026-10-11

**The operator's words of 2026-10-11 in the Keystone-width session**,
recorded by CLA on `worktree-bk-keystone-48` (`v2.7.0-*`; the v3.0.0 tag is
not cut). Every word is recorded **in English**, translated where it was
typed in Korean. `raft-marks-2026-10-11.md` beside this file is another
session's record of the same day, kept under the day's name; the two
sessions ran concurrently and each opened an order lettered BK, and §4
records the word that resolved it (BN and BO).

## 1. The pk width asked about, 42 then 48; BK's order written

| | |
|---|---|
| **Word** | *"Is it possible to change kdbs's pk width to 42 now? What would the scope of work be? Do not mind backward compatibility of the data."* (W0); *"How about changing it to about 48?"* (W1); *"Do it as 48 bits. Write a new milestone and ask me about the parts that need a decision."* (W2) |
| **Mark** | **W2 decides the width: 48 bits.** BK-S0: the order written on it (`workorder-bk-keystone-48.md`), this file, `index.md`'s BK row. The rejection of widening at `docs/rules/keystoneid-invariant.md:230-233` is overturned by W2 and will be restated by BK-S3. W0's "do not mind backward compatibility" is answered by the standing order of 2026-10-07 (bump the superblock version, refuse older volumes, never migrate), applied as BK-R4 - a ruling, not an item. **Not opened**: BK-Q0 is unmarked, and W2 asks that the items be put to the operator, so no standing go-ahead applies |
| **Does not settle** | BK-Q0..Q6: the layout of the upper 16 bits (BK-Q1), the rules document's stance on K5 (BK-Q2), how a refusal names the ceiling (BK-Q3), the close's measurement (BK-Q4), a tag (BK-Q5), the narrow-pk wire truncation the review found (BK-Q6); BK-S1's start; any push |
| **Recorded at** | `workorder-bk-keystone-48.md`'s header (W0-W2), §0 and §4; `index.md`'s BK row |

## 2. BK opened; every item marked as CLA proposed

| | |
|---|---|
| **Word** | The operator's answers to the four questions CLA put after the order's review, each the first option offered (W3): BK-Q1 *"(a) flags:8 \| reserved:8"*; BK-Q6 *"(a) file the bug only, the fix in its own letter"*; BK-Q2 *"(a) overturn the rejection sentence with its date and keep K5"*; and for BK-Q0, Q3, Q4 and Q5 *"all as proposed"* |
| **Mark** | **Marked per item, by the operator:** BK-Q0 yes - BK opens; BK-Q1 (a), `id:48 \| flags:8 \| reserved:8`, the Cabin bound entry's word identical to the Keystone word; BK-Q2 (a), the rejection at `keystoneid-invariant.md:230-233` restated as overturned on 2026-10-11, §3's table recomputed, K5 kept; BK-Q3 (a), refusals name the ceiling's value; BK-Q4 as written, the A/B at the close reported and gating nothing; BK-Q5 (a), no tag at the close; BK-Q6 (a), the narrow-pk wire truncation filed as a `docs/inflight/bugs/` entry in BK-S1 with (b) recommended as its fix, in its own letter. BK-R1..R9 are rulings; BK-S1 is next |
| **Does not settle** | BK-S1's start; any push; the letter that takes BK-Q6's fix; a decision that surfaces mid-stage, which W2's "ask me" sends back to the operator rather than to a standing go-ahead |
| **Recorded at** | `workorder-bk-keystone-48.md`'s header (W3), §2's heading, §4's mark column and §6; `index.md`'s BK row |

## 3. BK-S1 started, and the standing go-ahead for BK

| | |
|---|---|
| **Word** | *"Start BK-S1 and do not stop until that milestone is complete in full; after that, for the matters that need a decision, follow CLA's proposal."* (W4) |
| **Mark** | The standing go-ahead of `go-ahead-achieving-milestone`, for BK: every stage to the close in this session; a decision that surfaces mid-stage is written into the order with CLA's proposal and adopted under this word, recorded as *adopted on the operator's standing go-ahead of 2026-10-11* and never as the operator's own mark. BK-S1 started |
| **Does not settle** | Any push, merge to `main`, tag or version; a decision CLA cannot propose from the tree; anything outside BK |
| **Recorded at** | `workorder-bk-keystone-48.md`'s header (W4) and §6's BK-S1 row; `index.md`'s BK row |

## 4. The BK letter collision resolved

| | |
|---|---|
| **Word** | *"Clean up the BK letter collision."* (W5, after BK had been pushed to `main` and three orders were found to carry the letter) |
| **Mark** | The first written keeps the letter: `workorder-bk-keystone-48.md` (BK-S0 at 02:11 UTC) stays BK. `workorder-bn-opt-read-update-path.md` (opened 07:01 UTC; its own W1 had named BK after rejecting BY) is **BN**, its order and marks renamed (`workorder-bn-opt-read-update-path.md`, `raft-marks-2026-10-11-bn-paths.md`), its items BN-R*/Q*/S*, its tools `tools/bn_census.py`, `bn_census_table.py`, `bn_pg_scan.py`, its results files and archives `results-bn-close-v2.7.0-787-g8679367e.md`, `results-bn-s1-census-v2.7.0-769-gb70f79d8.md` and their archive directories. `workorder-bo-sys-statistics-show-sugar.md` (07:19 UTC) is **BO**, renamed `workorder-bo-sys-statistics-show-sugar.md`, its items BO-*, its marks staying in `raft-marks-2026-10-11.md` §1-§3 under the new letter. Every reference followed - `CLAUDE.md`'s rows, the specs, `known-gaps.md`'s section, the code comments, `index.md`'s rows, BL's citations. Commit messages and worktree names keep the letters they had; a BK in git history before this mark is read by the file it names. **Second pass, 2026-10-12**: the first pass had lettered the read/update order BM; before it was pushed, `workorder-bm-scan-row-kernel.md` landed on `main` lettered BM (00:04 UTC), so the read/update order is **BN** and the statistics order **BO**, and the names above are the landed ones. What they replaced: `workorder-bk-opt-read-update-path.md`, `raft-marks-2026-10-11-bk-paths.md`, `tools/bk_census.py`, `bk_census_table.py`, `bk_pg_scan.py`, `bk_parts.json`, `results-bk-close-v2.7.0-787-g8679367e.md`, `results-bk-s1-census-v2.7.0-769-gb70f79d8.md` and their archives; `workorder-bk-sys-statistics-show-sugar.md`. The scan-row order's citations of the read/update order follow (BN) |
| **Does not settle** | Which order should have kept BK: the rule applied is CLA's (first written), and the read/update order's letter was the operator's own choice (its W1), so the assignment is the operator's to reverse - every step is mechanical and recorded here |
| **Recorded at** | This file; the three orders' headers (`workorder-bk-keystone-48.md`, `workorder-bn-opt-read-update-path.md` §0 and BN-Q10, `workorder-bo-sys-statistics-show-sugar.md`); `index.md`'s three rows |
