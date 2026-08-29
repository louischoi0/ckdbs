# Work order R4-M — the k sweep, the read ceiling, and what two cores could not say

Drafted 2026-08-29 against `main` at `a135a59` (`v2.2.1-127-ga135a59`).

Completes `docs/inflight/in-progress/workplan-insert-spreading.md` (IS1–IS8
built) on hardware that can run the sweep IS7 could not. The mechanism is
built and correct; what is missing is the evidence it was built to produce.

## 0. What the two-core run established, and what it cannot

IS7 measured, honestly, at the only k the host supported:

```
concentrated / group    749 ips     spread / group    848 ips   1.132x
concentrated / relaxed  20832 ips   spread / relaxed  23622 ips 1.134x
```

Two facts come out of it and neither is the headline.

**The two ratios agree to 0.2% while the absolute rates differ 28×.** That
corrects the plan's own prediction: §8 expected the group arm flat, on
v2.1.0's finding that spreading divides the commit batch. It is not flat at
two cores. **Why is unknown**, and the sweep that would say why was not run
because the host reports two CPUs.

**And 1.132× at k = 2 does not tell you the shape.** The question the whole
range line exists to answer is how throughput moves with k, not whether it
moves at k = 2. A ratio at one point is consistent with linear, with
sub-linear, and with a curve that peaks at three cores and declines —
`bench/v2.1.0` §3a measured this device overlapping four streams at 3.37×
before declining, so a decline inside the range this order can now reach is
a live possibility rather than a remote one.

**There is a third thing, and it is larger than either.** §3 priced a
ceiling before any code was written, and nothing since has measured it:

> A spread relation is readable up to **64 × `range_size_ids`** rows and
> refuses every `SELECT *` after that. At 4,096 that is **262,144 rows**.

The arithmetic is not subtle: a range is a lease grant, spreading makes
consecutive ranges differently-owned so `stages == ranges`, and
`kMaxFanInUpstreams` is 64 with a refusal above it rather than a
degradation. **R4 as built ships a mechanism whose benefit expires inside
one benchmark**, and the scenario relations sit close enough to that number
that the aggregate cell below may meet it mid-run rather than in theory.

## 1. Scope

**In.** IS7's measurement re-run as a k sweep on capable hardware. The read
ceiling measured rather than derived. The `range_size_ids` sweep with its
two-sided constraint. The aggregate cell. The two conclusions IS7 left
open. `build-release` throughout.

**Out.** Every mechanism change. This order measures what IS1–IS8 built and
does not extend it — **if a cell wants a code change to be runnable, that is
a finding and the cell is reported as not run.** The three ways out of §3's
ceiling, each named there with its owner: raising `range_size_ids` is the
operator's on this order's numbers; raising `kMaxFanInUpstreams` is
`crosscore.md` §9's; the per-core stripe reverses D6 and is the operator's
alone. The mover, R6's multi-range work, merge, per-range trees, M5.

**Explicitly not touched**, so a diff outside is a finding: the pump, the
routing, `CheckRangePlacement`, the fan-in, `RangeEligible`.

## 2. The debts this order clears first

Three things are owed before a number from this tree means anything.

**The suite has not been re-run since `1d9ccbd`.** IS7's commit says so
plainly rather than implying it: 3008/3008 green on the commit before the
`peer_listeners` gate change, and not since. A measurement order that
begins on an unverified tree measures an unverified tree.

**IS7's numbers came off a Debug build.** IS1–IS3 record *"3003/3003 green
in build/ (Debug)"* and IS7 does not state otherwise. RD9(a) found what
this costs — a sequential full sweep read this build 14–18% slower and a
rep-interleaved re-check of the identical shape **reversed the sign** — and
`ck-tester` requires `build-release` for a measurement. **The 1.132× is
therefore a provisional number, not a baseline**, and this order does not
subtract from it.

**RB5's two review items are still open.** Whether every shippable shape has
an equivalence test, and whether
`TheEquivalenceRestsOnInsertionOrderMatchingRangeOrder` asserts the
interesting half of its claim. The second is this line's business directly:
**interleaved blocks are what make insertion order and range order
diverge**, so a test assuming they match is a test R4 invalidated.

## 3. The conclusions this milestone must produce

**CK1 — the shape of spreading against k, and where it stops.** Not a
ratio: the curve, with the binding constraint named at the point it binds.
Candidates to distinguish, from the engine's own history: the device's
sync-overlap limit (`bench/v2.1.0` §3a's 3.37× at four streams, declining
after), the group committer (v2.1.0's central result), and the fan-in stage
count. **The conclusion names which one binds, not that one does.**

**CK2 — why the group arm was not flat.** §8 predicted flat on v2.1.0's
finding that spreading divides the commit batch. At k = 2 it was 1.132×,
matching the relaxed arm to 0.2%. Two readings fit: the batch at k = 2 is
still large enough that division costs nothing, in which case the arms
diverge as k rises; or v2.1.0's result does not survive spreading, in which
case they stay locked. **The sweep distinguishes them and the conclusion
says which**, because v2.1.0's finding is the most load-bearing
measurement this engine has and R4 is the first work to put it at risk.

**CK3 — the ceiling as a measured number.** §3's 64 × `range_size_ids` is
arithmetic from two ratified choices. Measure it: the row count at which a
spread relation's `SELECT *` refuses, the range count actually produced by
a run, and how both move with `range_size_ids`. Then state the ceiling's
trade against burn — a core that stops inserting burns its block's
remainder, and a restart burns every live block — because that trade is
what the operator decides on.

**CK4 — whether the scenario benches fit under the ceiling.** The aggregate
cell drives realistic relations. If any of them exceeds 64 ranges at the
armed size, the cell does not produce a slow number, it produces a
**refusal**. Establish this before running the aggregate, not from its
results.

**CK5 — what the aggregate says.** The first measurement in this line that
asks whether the engine goes faster with more cores. Whatever it reads, it
is the conclusion the range work has been building toward and has never
stated.

## 4. Hypotheses — each with its falsifier

**HK1 — throughput rises with k until the device binds.** Spreading gives
each core its own tail on its own chain with no shared structure, so the
first limit reached is the one `bench/v2.1.0` §3a already measured on this
class of device.
*Falsifier*: it binds earlier, and CK1 names what. **The stage count is the
candidate this order introduces** — `stages == ranges` under spreading, and
a run producing more than 64 refuses rather than degrades.

**HK2 — the two durability arms diverge as k rises.** The group arm tracks
the relaxed arm at k = 2 because the batch is still large; as k rises the
batch divides and the group arm falls away.
*Falsifier*: they stay locked across the sweep, which would mean v2.1.0's
commit-batching result does not survive spreading — **the more interesting
outcome, and the one to report loudest if it happens.**

**HK3 — the ceiling is exactly 64 × `range_size_ids`.** The arithmetic in
§3 is complete and nothing else intervenes.
*Falsifier*: the measured refusal point differs, in either direction. Lower
means something else caps stages first; higher means IS5's contiguous-refill
suppression is collapsing more ranges than the arithmetic assumes — which
would be good news and should be measured rather than hoped for.

**HK4 — IS5's suppression does not bound a contended relation.** IS5's own
comment says so: continuing the asking core's top range opens no boundary,
but that does not bound the ceiling when several cores contend.
*Falsifier*: the measured range count on a contended relation comes in
materially below one per block, meaning suppression fires more often than
its comment claims.

**HK5 — `cores = 1` is untouched.** One core, one block, one range, one
tail, today's path.
*Falsifier*: any measurable change at `cores = 1`. Guideline 2, eleventh
consecutive order.

## 5. Task series — IM rows

| # | Task | Gate |
|---|---|---|
| **IM0** | **Clear §2's debts.** Full suite green on the current tree, `build-release` built and used from here on, `scripts/sim.sh` green. RB5's two review items closed — the second one first, since R4 invalidated its premise | none |
| **IM1** | **CK4 before the aggregate.** For each scenario bench relation: rows produced, ranges that implies at each swept `range_size_ids`, and whether it crosses 64. **A cell that will refuse is not run and is reported as such**, with the size at which it would fit | IM0 |
| **IM2** | **The k sweep**, and **CK1**, **CK2**, **HK1**, **HK2**. Insert throughput, one relation, k = 1 … N writer cores, spread against concentrated, crossed with both durability arms. `bench/spread_insert_probe.py`, rep-interleaved. **Both arms at every k** — the divergence is the finding, so an arm dropped for time destroys it | IM0 |
| **IM3** | **The ceiling measured**, and **CK3**, **HK3**, **HK4**. The row count at which a spread relation's `SELECT *` refuses; the range count a run actually produces against the arithmetic; both across the `range_size_ids` sweep. Plus the burn side: 40-bit space consumed per (relation, core, mount) at each size | IM2 |
| **IM4** | **RD9(b) re-run with the large end reachable.** The size sweep now varies single-core concentration, which two cores could not. D6's value gets measurement behind it, **bounded on one side by IM3's ceiling** — a constraint §10b's table did not carry | IM2, IM3 |
| **IM5** | **RD9(c): k-owner fan-in from the wire.** Reachable for the first time, and now with IS5's suppression in the picture — the stage count as a function of block size and core count, measured rather than derived | IM2 |
| **IM6** | **The aggregate**, and **CK5**. Scenario benches whole, `cores = 1` against `cores = N`, spreading on, only for the relations IM1 cleared | IM1, IM2 |
| **IM7** | **CK1–CK5 written into the workplan**, and §3's ceiling updated from arithmetic to measurement with the three ways out re-priced against the number | IM3, IM4, IM5, IM6 |

## 6. Correctness — ahead of any number

- **Full suite and `scripts/sim.sh` green before the first cell**, and
  re-run after, both `build-release`, both in one sitting, which sitting
  named. §2's first debt is why this is a row rather than a habit.
- **rows in = rows out in every cell.** RB6's driver manufactured duplicate
  rows by retrying an `ERR` that followed a commit; spreading multiplies
  those paths, and a cell that silently gains rows reports a throughput
  that did not happen.
- **`cores = 1` unchanged** (HK5).
- **No engine diff.** This order measures; a cell needing a code change is
  a finding, and the changed-file set is reviewed rather than trusted.
- **A refusal is a result, not a failed run.** If a cell meets the fan-in
  ceiling, that is CK3's evidence and it is recorded as such.

## 7. Measurement

`build-release` — non-negotiable here, and §2 says why. **Rep-interleaved
arms**, standing rule for this line since RD9(a) reversed a sign. Fresh
server and data file per invocation, per-arm processes, per-rep spreads
before any median, rows in = rows out. Anything not run is reported as
**not run**, with the reason and the host fact that made it so.

| cell | measures | reads against |
|---|---|---|
| **K-a** | Insert throughput, k = 1…N, spread vs concentrated, group arm | **HK1**, **HK2**, CK1 |
| **K-b** | Same, relaxed arm — the two must be run at every k, not one at each | **HK2**, CK2 |
| **K-c** | The refusal point: rows at which a spread relation's `SELECT *` refuses, across `range_size_ids` | **HK3**, CK3 |
| **K-d** | Range count produced by a contended run, against one-per-block | **HK4** |
| **K-e** | Burn: 40-bit space consumed per (relation, core, mount) at each swept size | CK3's other side |
| **K-f** | RD9(b): concentration against `range_size_ids`, large end now varying | D6's value |
| **K-g** | RD9(c): stage count and read cost at k owners | CK1's third candidate |
| **A** | The aggregate: scenario benches, `cores = 1` vs `cores = N` | **CK5** |

**On the host.** The host changed between RB6 and IS7 and changes again
here. Every result file states its own host and **no number is compared
across two of them** — IS7's 1.132× is not this order's baseline and is not
subtracted from. Where a value was derived from host characteristics, the
derivation is restated on the host that measured it.

**On the A-cell.** It is gated on IM1 rather than run and interpreted,
because the failure mode is a refusal rather than a slow number. It is also
the cell this line has been deferring: every measurement so far priced a
cost inside the range machinery and none asked whether the machinery wins.
**R5 is a policy layer on top of ranges, and a policy built on an unverified
premise inverts the order of evidence** — so this number should exist before
the mover is designed.

Results to `bench/v2.6.0/`, named by `git describe --tags`, in the format
IS7's file used: host with physical-versus-logical cores and SMT state,
filesystem, build flags, binary provenance, per-rep tables, findings tagged
**measured** or **source-read** with sites, a §"what this run does not
measure", the subject named beside the numbers rather than once in a
preface, and the PostgreSQL section citing RP8 §10's reasoning where no
honest twin exists rather than re-deriving it.

## 8. Improvement — what this leaves better

- **The line's premise tested.** Ranges were built to spread work across
  cores. Every number so far measured what that cost. This order measures
  whether it bought anything, at more than one k.
- **v2.1.0's result put at risk.** Commit batching governing ingest
  throughput has been the engine's most load-bearing measurement for a
  version and a half, and nothing has revisited it under a mechanism
  designed to divide the batch. Either it survives or it does not, and both
  are worth knowing.
- **A ceiling moved from arithmetic to measurement.** §3 priced it before
  any code, which was right; leaving it priced rather than measured would
  make the operator decide the size sweep against a derivation.
- **A measurement order that cleared its debts first.** An unverified tree,
  a Debug number, and two open review items were all inherited; clearing
  them is what makes the numbers that follow mean what they say.

## 9. Deliverable

- §2's three debts cleared, each recorded as closed rather than assumed.
- K-a … K-g and **A** under `bench/v2.6.0/`, with everything not run
  reported as not run and why.
- CK1–CK5 written into `workplan-insert-spreading.md`.
- §3's ceiling updated from arithmetic to measurement, with its three ways
  out re-priced against the measured number and each still attributed to
  its owner.
- D6's value handed to the operator with a sweep behind it and IM3's
  ceiling bounding one side.
- Each row landing with its worktree name and short commit **inside the
  sentence that makes the claim**, and its `critics-developer` pass with
  findings applied or rejected by name.

**What this does not close.** The ceiling itself — this order measures it
and the three ways out stay with their owners. The btree subject problem,
which is per-range trees'. D1 for auxiliary placement. M5 and R1, and with
R1 the per-core statistics R5 needs. The read-only-participant COMMIT cost.
The missing per-leg instrument, three times a blocked attribution. RB6's
wire-classification gap, which spreading makes more frequent. The ~11 ms
periodic stall; RW-C1; the 992-byte reply cap.
