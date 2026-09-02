# AH-T6: the crossing's matrix, its two legs, and two hypotheses that did not survive contact

| | |
|---|---|
| Measured at | `v2.7.0-97-g199dabf` (`git describe --tags`) |
| Pre-hoist arm | `v2.7.0-81-ge5f1bf3` (AH-T0, text only — the last commit before AH-T1 moved the descent), built from a clean `git archive` into its own tree |
| Build | `build-release` (Release) both arms, `-DOPENSSL_ROOT_DIR=…` |
| Driver | `bench/fk_crossing_cost_probe.py`, modes `hoist-ab`, `crossing`, `owners` |
| Host | 8 cores, AMD EPYC 9V74. Four runs, sequential in one script; no build, suite or second measurement overlapped any of them |
| Raw | `bench/v2.8.0/archive/ah-t6-hoist-ab.json`, `…-rows-axis.json`, `…-legs-group.json`, `…-owners-axis.json` |
| Tree state | The measured binary is `199dabf`'s tree. Only `bench/*.py` changed after it (a tolerant read of a `SHOW META` block that is absent by design); nothing under `src/`, `include/` or `tests/` moved between the build and the last run |

Every arm is alternated block by block, three blocks each, 300 statements a
block. **This file is AH-T6's measurement half**; its closure half — the
verdicts, the race cell and the leg instrument — is `199dabf` itself.

## 1. H-AH1 — held, and "for free" understates it

> *On a colocated parent … B's latency increment on the colocated path is
> zero within noise — measured, not asserted, because the extraction pass
> runs before knowing the parents are local.*

One core, `durability = relaxed`, a colocated foreign key, three statement
widths. µs per statement, p50 pooled over 900 samples.

| rows per statement | `e5f1bf3` pre-hoist | `199dabf` post-hoist | delta |
|---|---|---|---|
| 1 | 44.1 | 44.5 | **+0.4 µs (+1.0%)** |
| 10 | 60.7 | 60.1 | −0.6 µs (−0.9%) |
| 100 | 148.3 | 109.5 | **−38.7 µs (−26.1%)** |

Per-block p50 spread (min–max, sd): pre-hoist 42.8–47.4 (2.1), 60.5–61.4
(0.4), **127.4–157.0 (12.6)**; post-hoist 44.2–47.2 (1.3), 59.8–60.4 (0.2),
**108.8–110.2 (0.6)**.

**Held at one row and a real win at a hundred**, which is what the hoist was
always going to be: one descent per statement instead of one per row. The
number the hypothesis asked for is the first line — +1.0%, inside a spread
of 2.1 µs, so free. The line worth carrying is the third: **the hoist also
removed the variance**, 12.6 µs of block-to-block spread down to 0.6, because
a hundred descents have a hundred chances to fault a page and one has one.

## 2. The two legs, and the first thing they say is not to add them up

`199dabf` times the crossing's two rounds apart (`fk_probe_round_*`,
`fk_release_decide_*`), which is what `results-ai-t3-fk-crossing-cost` named
as owed. Mean µs per walk, read off `SHOW META` at each block boundary on the
core that owns the child.

| run | probe round | release decide | end-to-end delta |
|---|---|---|---|
| `relaxed`, 1 row | 36.1 / 40.7 / 39.1 | 28.7 / 42.5 / 40.8 | +50.4 µs |
| `relaxed`, 10 rows | 43.8 / 36.7 / 36.3 | 44.5 / 52.6 / 39.2 | +55.7 µs |
| `relaxed`, 100 rows | 55.4 / 47.0 / 31.3 | 107.6 / 106.2 / 105.8 | +95.3 µs |
| `group`, 1 row | 30.4 / 30.5 / 30.4 | **1247.7 / 1335.4 / 1246.1** | **+71.0 µs** |

**The split AI-T3 could not make**: at one row and `relaxed` the two rounds
are 36 µs and 37 µs — the release decide costs as much as the probe. Half of
the crossing's price is the round that exists only to say *the statement is
over*.

**And the fourth row is why a leg is not a summand.** Under the shipped
durability class the release decide's leg measures **1.25 ms** while the
statement's end-to-end increment is **71 µs**. It is not that the leg is
wrong: the coordinator really does wait that long between sending the decide
and seeing it acknowledged. It is that the wait **overlaps the commit's own
group-commit drain**, which the statement was going to pay anyway. A reader
who added 1.25 ms to the write's cost would be wrong by a factor of eighteen.
XE's line, on a different leg: *a round counted is not a round waited for.*

## 3. H-AH2 — split, and both halves of the split are findings

> *A statement writing N ∈ {1, 10, 100} child rows against one foreign
> parent owner shows a flat cross-owner increment across N; two distinct
> foreign owners show approximately twice the increment at every N.*

### 3a. The round count counts owners and not rows — held, exactly

Counted, never inferred. Every crossing block: **300 probe rounds for 300
statements**, at 1, 10 and 100 rows per statement; every colocated block:
**0**. The two-owner shape: **600 rounds for 300 statements**. AH-R2's rule
holds at 300 statements and at a hundred rows apiece, and the dedup that
makes it true (by parent relation and pk, before the grouping by owner) is
what the sweep is really testing.

### 3b. The *latency* increment is not flat in rows — refuted, and §2 says why

| rows per statement | colocated p50 | crossing p50 | delta |
|---|---|---|---|
| 1 | 93.9 | 144.3 | +50.4 µs (+53.7%) |
| 10 | 114.0 | 169.7 | +55.7 µs (+48.9%) |
| 100 | 268.8 | 364.1 | +95.3 µs (+35.5%) |

The increment grows 50 → 96 µs while the round count stays at one. §2's table
attributes it: the **probe** leg is flat (36, 39, 45 µs on average) and the
**release decide** leg is what grows — 37 → 45 → 107 µs. Nothing about the
decide's *round* depends on the statement's size; what depends on it is the
reactor's queue behind a heavier commit, which the leg measures because it
measures wall time from send to acknowledgement. **So the hypothesis is right
about the mechanism and wrong about the observable**, and the honest form of
it is: *the crossing's own cost counts owners; what grows with rows is the
wait for a reactor that has more to do.*

### 3c. Two owners do not cost twice — refuted, and the design is why

Four cores, `relaxed`, one row per statement, both children on the core that
owns neither parent. Both statements are two values wide; `one-owner`'s
second column references nothing, so the shapes differ in **owner count and
nothing else**.

| shape | p50 | p90 | mean | per-block p50 | rounds |
|---|---|---|---|---|---|
| one foreign owner | 179.4 | 195.6 | 210.2 | 158.9, 179.7, 187.9 | 300 |
| two foreign owners | 162.6 | 177.7 | 191.3 | 156.1, 163.2, 164.1 | 600 |

**Twice the rounds, and no more time — 9.4% *less*, inside the one-owner
arm's own 29 µs of block spread.** The prediction of "approximately twice the
increment" assumed the rounds are serial. They are not: the fork sends every
owner's request and parks **once, over all of them** (`pending_remote`'s rule,
RD7 — *"k sequential parks would serialise on whichever owner the loop named
first, turning a fan-out into a fan-out-then-queue"*). So a statement's
cross-owner wait is the **slowest owner's reply**, not the sum of them, and
the second owner is free unless it is slower than the first.

That is a stronger property than the hypothesis asked for, and it is the one
worth quoting: **the crossing's cost is a function of the slowest owner a
statement touches, not of how many it touches.**

## 4. The tail, which AI-T3 named and this file can only halve

`results-ai-t3-fk-crossing-cost` §4 recorded a crossing p99 of 13× the
colocated arm's and left it open. Here, at one row and `relaxed`, the same
shape: crossing p99 **1684 µs** against colocated **127 µs**. But at 10 and
100 rows the colocated arm's p99 is **1809** and **3278 µs** — so the tail is
*not* the crossing's alone; it is a property of a write path that
occasionally waits milliseconds, and the crossing merely reaches it at one
row where the colocated statement needs ten. Retries are zero throughout, so
it is not a lease refill.

What would settle it is the legs' **maxima** rather than their means
(`fk_probe_round_max_us`, `fk_release_decide_max_us` are printed and not yet
read per block). Recorded as the next instrument step rather than a
conclusion.

## 5. Verdict on AH's four hypotheses

| | verdict |
|---|---|
| **H-AH1** the hoist changes no colocated answer, for free | **Held**, and understated: free at one row (+1.0%, inside noise), 26.1% *cheaper* at a hundred, with 20× less block-to-block variance |
| **H-AH2** cost counts owners, not rows | **Split.** The *round count* counts owners and not rows, exactly (300 rounds for 300 statements at every width; 600 for two owners). The *latency* does neither: it is flat in owners (a parallel fan-out, §3c) and grows with rows through a leg that is the reactor's queue and not the round's (§3b) |
| **H-AH3** the restart window cannot commit | **Held**, by AH-T5's probe: 3/3 across three passes, killed after the grant and before any prepare, no child row and no in-doubt residue (`results-ah-t5-fk-intent-crash-v2.7.0-89-g956f00d.md`). The half a single process cannot stage — a participant dying under a *surviving* coordinator — stays owed |
| **H-AH4** the race answers retryable, never wrong | **Held**, and at the dispatcher rather than in miniature: `AParentDeleteMeetingALiveForeignIntentAnswersBusyBeforeAnythingElse` parks a real statement mid-probe and shows the parent `DELETE` answering busy — *retryable*, and before §3a's terminal refusal, which takes over once the decide releases |

## 6. What AH does not close

- **A participant dying under a surviving coordinator** (H-AH3's other half)
  needs two processes and is not staged anywhere.
- **The leg maxima**, §4's tail.
- **`strict`**, and concurrency: every number here is one client, one
  statement at a time. XE's finding that a leg's cost can *reverse* under
  load is the reason to say so rather than let a serial number stand for the
  engine.
- **The reverse direction's fan-out**: a cross-owner parent still cannot be
  deleted (§3a), which `known-gaps.md` keeps.
