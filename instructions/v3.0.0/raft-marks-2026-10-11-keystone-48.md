# Ratification marks — 2026-10-11

**The operator's words of 2026-10-11 in the Keystone-width session**,
recorded by CLA on `worktree-bk-keystone-48` (`v2.7.0-*`; the v3.0.0 tag is
not cut). Every word is recorded **in English**, translated where it was
typed in Korean. `raft-marks-2026-10-11.md` beside this file is another
session's record of the same day, kept under the day's name; the two
sessions ran concurrently and each opened an order lettered BK
(`workorder-bk-keystone-48.md` here, `workorder-bk-sys-statistics-show-sugar.md`
and `workorder-bk-opt-read-update-path.md` there), so a citation to "BK"
or to this day's marks names the file, never the bare letter.

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
