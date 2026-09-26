# Ratification marks — 2026-09-26

**The operator's marks of 2026-09-26.** Recorded by CLA on `at-close-order`
against `main` at `6b0694d` (`v2.7.0-*`; the v3.0.0 tag is not cut, and
Q3 below says it is not cut at AT's close either).

Two kinds of record, kept apart because their provenance differs:

- **§1-§3 are verbatim**: the operator's own words in the session that
  recorded them, quoted as typed, typos included.
- **§4 is not verbatim.** Q1-Q3 were marked verbally before
  `workorder-at-close-ar0-5-left-open.md` was placed, and the only record
  CLA holds is that order's own sentence - *"the operator took CLA's
  proposal on all three on 2026-09-26 (verbal)"*. The order asks for the
  word verbatim; the verbatim word is not in hand, and this section says
  so rather than inventing one.

---

## 1. E7 — the execution default

| | |
|---|---|
| **Word** | *"[dicision] E7: local as default"* |
| **Mark** | E7 is **local**: a statement runs on the core its session landed on, for **every verb**, `INSERT` included |
| **Recorded at** | `2b41d31` - AR2's E7 row and R12, `workorder-at-m3-uniformity.md` AT-0 item 2, its D/E table and AT-S13's row, `index.md` |

**What it obliges.** Nothing to build: no route has existed since AT-S9,
so the mark makes the engine's shape its default. **What it does not
settle.** The word names no verb, and CLA read it as all of them; the
`INSERT` arm had been CLA's **routed** proposal (AR2 §7, on C2's evidence,
taken under insert spreading, which AT-S9 retired). If the operator meant
`UPDATE`/`DELETE` only, `2b41d31`'s reading is wrong and the `INSERT` arm
reopens. And AT-S13's cross-core refusal price stays AX's
(`known-gaps.md`, Locks), which E7 does not reach.

## 2. AT-0 item 12 — `BtreeLookup` returns the held `PageRef`

| | |
|---|---|
| **Word** | *"[decision] A6 - BtreeLookup returns PageRef"* |
| **Mark** | the word to build item 12's shape (a), marked 2026-09-10 |
| **Recorded at** | AT-6's AT-S14 row (`9be2d02`, renumbered at placement) |

**Provenance.** No item named A6 exists in the tree; CLA read the word as
item 12's (a), which it matches, and said so in the session. **Built** as
AT-S14 (`974a844`, `08f6162`), pushed at `6b0694d`.

## 3. The AT-close order — placed

| | |
|---|---|
| **Word** | *"Place the order"* |
| **Mark** | `workorder-at-close-ar0-5-left-open.md` placed at `instructions/v3.0.0/`, with its §0 recording what the tree settled between issue (`1f9592a`) and placement (`6b0694d`) |

The operator also said, the same day, *"rename docs/blueprint to
docs/conceptnotes"* (`c835464`), which touches no order; recorded here
only so the day's words are in one place.

## 4. Q1-Q3 of the AT-close order — marked verbally, not verbatim here

| # | item | mark (CLA's proposal, taken) |
|---|---|---|
| Q1 | the walk-up's shape, both trees (AT-S16) | **re-validate and re-descend**; latch coupling only if the re-descent's progress cannot be proved |
| Q2 | name atomicity (AT-S17) | **the held `sys.objects` root page** across check and write; no name lock unit |
| Q3 | the v3.0.0 annotated tag (AT-S20) | **not at AT's close**; cut when AR0 §8's chain is done |

The order's §3 is the ruling text; this table restates it and adds
nothing. **If the operator's verbal word differed from CLA's proposal on
any of the three, that row is wrong** and the stage it gates stops.
