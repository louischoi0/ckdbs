# Ratification marks — 2026-09-28

**The operator's words of 2026-09-28**, recorded by CLA on
`at-s16-walk-up-revalidate` from `1f230e4` (`v2.7.0-424-g1f230e4`; the v3.0.0
tag is not cut - `raft-marks-2026-09-26.md` §4, Q3). All of it is
**verbatim**: the words as typed in the session, or, where the operator
answered a question CLA put, the question and the answer chosen, quoted as
they stood.

---

## 1. AT-S16 started

| | |
|---|---|
| **Word** | *"start AT-S16"* |
| **Mark** | AT-S16 opens, by the AT-close order's §4 and Q1's ruling (re-validate and re-descend) |
| **Recorded at** | AT-6's AT-S16 row, `workorder-at-m3-uniformity.md` |

## 2. AT-S16 at the root — the grown-over mark

The survey found that Q1 does not decide one point: how a walker learns that
the page it is about to grow a new root over is no longer the root. CLA put
it as a question with three options.

| | |
|---|---|
| **Question** | *"AT-S16 at the root: how should a walker learn that the page it is about to grow a new root over is no longer the root (another core grew over it, published or not)?"* |
| **Answer chosen** | *"Grown-over bit (Recommended)"* - described as: a bit in each tree page's body flags, set under the old root's exclusive hold when a root is grown over it and logged in its image; existing pages read 0, which claims nothing; a walker finding it re-descends for the parent, and with a stale memo is refused `TxnConflict` naming a stale root; no anchor read, no hold outside the tree |
| **Declined** | *"Anchor read + hold"* and *"Non-root only"* |
| **Mark** | `kHeaderFlagGrownOver`, `kInternalFlagGrownOver` and `kIndexFlagGrownOver`, bit `0x2` of each tree page's body `flags` |
| **Recorded at** | `heap-and-tuple.md` §5, `index.md`, AT-6's AT-S16 row |

**What CLA's reading added**, stated so it can be checked. The bit alone is
sound only if a stale root is refused **before anything is written**:
otherwise a stale internal root, discovered after the levels below it have
divided, strands their new halves. So AT-S16 finds and holds every parent a
split writes before the first write. That is an implementation of the
ruling, not a new decision: it adds no persisted field beyond the bit.

## 3. Amendment 1 to the AT-close order — the order of work

Pasted mid-stage with the amendment's text (*"Amendment 1 to the AT-close
order — the quiet-wrong windows found after issue"*):

| | |
|---|---|
| **Word** | *"ignore AT-S16 things, but follow what remains:"* |

CLA asked what that meant for the stage in progress and for the placed text:

| | |
|---|---|
| **Question** | *""Ignore AT-S16 things": what happens to the S16 work in progress? ..."* |
| **Answer chosen** | *"Finish S16 too"* |
| **Question** | *"When the amendment is placed as the order's §7, what happens to its S16 parts ...?"* |
| **Answer given** | *"finish AT-S16 first"* |
| **Mark** | AT-S16 is finished first. The amendment is placed after it, with the rest of what it orders following |
