# Work order SB — the Cabin under a split: scoped authority, ordered discard

Drafted 2026-09-01 by CLA against `main` at `9cb8674`
(`v2.7.0-41-g9cb8674`). Executes what SA-T2 became after its survey
(`docs/inflight/in-progress/workplan-auxiliaries-under-split.md` §2,
commit `76ece65`): the survey found that work order SA's discard is
necessary and not sufficient, surfaced a scoping proposal instead of
building on the order's letter, and the operator has now ruled. This
order records the ruling and builds T2a-T2d under it. Source of record:
the workplan §§2.3-2.5, `cabin.md` §7/§12, `crosscore.md` §6a/CC10,
`include/kds/stats/cabin_store.hpp`, `include/kds/exec/step_chain.hpp`,
`src/catalog/catalog.cpp:1094-1110`.

## Rulings — SB-R1..SB-R5

**SB-R1 — The authority scoping is ratified (operator, 2026-09-01).** A
Cabin's entry set is authoritative for **(observed value × the ranges
its core owns)**. A probe resolves the ranges it needs through the range
directory; ranges the serving core owns are answered from the set; any
range it does not own falls through to that range's own stage — the
fan-in the read surface already opens. A relation of one range is
unchanged byte for byte. This is an authority change, not a mechanism:
it lands in `cabin.md` §7's serve rules **before** any serve-path code
compiles against it.

**SB-R2 — The discard is ordered before the grant, for both classes.**
The Observational discard completes — broadcast sent, **every core's
acknowledgement received**, `CabinStore::Forget` run on each — before
CC10 grants the lease, because a peer can write from the grant onward
and a set banked when the relation was whole is a subset from that
moment. Under `peer_listeners = on` the acknowledgement set is every
core. This is the same window SA-T7 gives the Bound directory load
("between the durable directory row and the grant"), and the two
classes now share one rule stated once in CC10's sequence.

**SB-R3 — The bundle is atomic.** T2a (gate arms dropped from
`RangeEligible`), T2c (the ordered discard), and T2d
(`RefuseAuxiliaryOnSplitRelation` loses its Cabin arm) land together
with T2b (the scoping) **or none of them land**. Lifting either gate
without the scoping converts a refusal into a quiet wrong answer — the
one trade the engine's rules never make. Review enforces this as one
merge, not four.

**SB-R4 — Ratification conditions (operator's two, recorded as
acceptance terms).** (1) Two race cells are part of T2b's definition of
done, not follow-ups: the probe-versus-in-flight-split cell and the
authority-collapse-under-migration cell (SB4 below). (2) The serve path
is observable: a per-relation **fall-through counter** and the discard's
decline/discard counter land in `SHOW META` beside each other — a Cabin
on a split relation whose savings cannot be seen cannot be measured, and
an unmeasured saving is not claimed.

**SB-R5 — Scope of the ruling.** Observational class only. The Bound
Cabin's migration is SA-T7's (its split stays gated); UNIQUE stays
gated (SA-R2); nothing here touches banking's no-transaction rule
(`cabin.md` §6a) or the router (SA-T5a). The index arm of
`RangeEligible` (T2a's other half) rides along because the survey
proved it a comment — `IndexRef` carries no `unique` flag until IX11 —
and it names IX11 where it returns.

## Background — the finding, restated once

A Cabin set is a superset the read re-filters; a subset is a wrong
answer; an observed value's empty set is an authoritative "no rows"
(`step_chain.hpp:87-92`). `CabinStore::NoteWrite` appends only where the
value is observed and runs on the writing core
(`cabin_store.hpp:366`) — so after a split, a peer's inserts are
appended nowhere and the original observer's set silently stops
covering the relation. A cabin created *after* the split is born with
the same hole; the discard cannot help it, because there is nothing
stale to drop. Hence SB-R1 (scope the claim so it stays true) and
SB-R2 (kill the pre-split claims before anyone can falsify them). The
spreading amendment (`amendment-spreading-per-relation.md`) bounds the
blast radius of an error here to relations that opted into spreading —
which is why this lands now, not why it is safe.

## Hypotheses

- **H-SB1.** On a one-range relation the serve path is byte-identical
  and cycle-neutral: the scoping's range-directory resolution costs
  nothing when the answer is "all ranges are mine" — Guideline 2's
  shape, applied at the relation level, verified not assumed.
- **H-SB2.** No interleaving of split, discard, grant and probe can
  return a subset answer: a probe races the split and gets either the
  whole-relation set (before the discard completes, when the set's
  claim is still true — the grant has not happened) or the scoped
  world (after), never a set that claims a range a peer is writing.
- **H-SB3.** On a split relation, a cabin-served probe composed with
  SA-T1's re-derived fall-through stages answers correctly and
  recovers serve-rate as re-observation proceeds per owner (subsumes
  SA-M4's recovery cell).
- **H-SB4.** The split's added latency is one acknowledged broadcast,
  linear in core count and flat in relation size; at cores ∈ {2,4,8}
  it is measured, not waved at.

## Tasks

**SB0 — the ruling lands as text.** One commit, docs only, before any
code: `cabin.md` §7 gains the authority sentence (SB-R1) and the
serve-resolution rule (owned-from-set, unowned-fall-through, one-range
unchanged); §12's class table cross-references it; `crosscore.md` CC10
gains the pre-grant window naming **both** classes' obligations
(Observational discard-and-ack, Bound directory load — one window, two
sentences); §6a's Cabin bullet is rewritten to point at the scoped
rule and the remaining Bound gate; `known-gaps.md`'s entries amended.
The workplan's §2 is marked ruled. Gate: none.

**SB1 — the ordered discard (T2c).** The split path, before CC10's
grant: a `kCabinDiscard` broadcast for the relation's cabins on the
transport's existing message discipline, each core running
`Forget(cabin_id)` and acknowledging; the grant does not issue until
the acknowledgement set is complete; a core joining mid-window (mount)
inherits nothing to forget by construction and is stated as such. The
decline/discard counter lands in `SHOW META` (SB-R4). Crash cells: a
split that dies after the broadcast and before the grant leaves only
discarded sets and no lease movement — re-observation is the cost, a
mount replays nothing for it (unlogged class); a split that dies
before the broadcast never granted, so the old claims are still true.
Gate: SB0.

**SB2 — the scoped serve path (T2b).** The serve resolution per SB-R1:
the probe's range-need resolved through the range directory once per
serve, owned ranges answered from the set, unowned ranges opened as
their own stages (SA-T1's re-derive applies on arrival). `NoteWrite`
unchanged — it already runs on the owning core, which is now exactly
the scope of the claim. The one-range fast path is explicit and first,
asserted byte-identical against the pre-SB serve in the suite
(H-SB1). The fall-through counter lands here (SB-R4). Gate: SB0;
independent of SB1 until SB3 joins them.

**SB3 — the gates drop (T2a + T2d), atomically with SB1/SB2.**
`RangeEligible` loses its `kCabin` arm (never returns) and its
`kIndex` arm (returns at IX11, named in the comment);
`RefuseAuxiliaryOnSplitRelation` (`catalog.cpp:1094-1110`) loses the
`"a Cabin"` arm (`:2762`), manual and optimizer paths alike. A
post-split `CREATE CABIN` is admitted and born correctly scoped by
SB2's rule. This lands in the same merge as SB1+SB2 (SB-R3) and only
after SB4's cells are green. Gate: SB1, SB2, SB4.

**SB4 — the two ruled race cells, plus the interleavings.**
(1) *Probe versus in-flight split*: seeded scheduling points across
broadcast-sent / partially-acked / fully-acked / granted, a probe fired
at each, asserting H-SB2's dichotomy — the K1/epoch machinery either
closes it or the cell fails and this order stops. (2) *Authority
collapse under migration*: CC10 moves the whole relation; the origin
core's owned-range set goes to zero; a probe on the origin after the
grant serves nothing from the set and falls through entirely; the set
is discarded by SB1's rule as a migration is a grant too — asserted,
not assumed. Plus the crash cells named in SB1. Gate: SB1, SB2.

**SB5 — measurement.** Cells, `bench/v2.7.3/`, discipline unchanged
(release, tags, ≥3 runs, spread, interleaved arms):
- **SB-M1**: one-range serve, pre/post, byte-identity plus
  cycle-neutrality (H-SB1) — regression cell, reported even though its
  headline is "nothing moved".
- **SB-M2**: split latency at cores ∈ {2,4,8} with the acknowledged
  broadcast, against the pre-SB split, flat-in-relation-size checked
  at two sizes (H-SB4).
- **SB-M3**: split-relation probe latency and serve/fall-through mix,
  observed vs unobserved values, over re-observation time — subsumes
  SA-M4 and is named in SA's file as doing so (H-SB3).
Gate: SB3.

## What this order does not claim

It does not build the router (SA-T5a), touch Bound split, revisit
banking's transaction rule, or lift the index gate's *UNIQUE* future
arm. It does not decide any constant — the broadcast rides existing
message sizing, the counters are counters. It does not claim the
fall-through composition is fast, only correct and measured (SB-M3 is
the number). If SB4's first cell shows the epoch machinery does not
close the probe race, the order stops at SB2 and reports — the gates
stay up, because SB-R3 is a two-way rule: the bundle lands whole or
the refusal stays honest.

---

## Amendments taken while building it (2026-09-01)

The order above is the operator's input and is kept verbatim. Three of
its statements did not survive contact with the tree, and the record of
what was built instead lives here rather than being silently absorbed.

**A1 — SB-R2's mechanism, amended by the operator on the finding.**
There is exactly one `stats::CabinStore` in the engine,
`Expeditor::cabin_store_` on core 0; every peer dispatcher
(`src/server/core_runtime.cpp`) and every fan-in stage
(`src/server/remote_step_service.cpp`, three sites) is constructed with
`cabins = nullptr`, which `docs/inflight/known-gaps.md` already recorded
from the other direction. SB-R2's "every core's acknowledgement" is
therefore one core's, and it is the core performing the split. **Ruled:
a direct core-local `Forget` inside `OpenRangeOnSystemCore`, before the
rows that publish the boundary** — no window at all rather than a window
closed by acknowledgements, and no ring kinds reserved for state that
cannot exist. `cabin.md` §4b and `crosscore.md` CC10 state the
obligation in SB-R2's words and name the condition that turns it back
into a broadcast: the day a peer or a stage is given a store.

**A2 — SB0's "§7's serve rules" is a mis-citation, carried from the
survey.** `cabin.md` §7 is the *interlock* section; the serve rules are
§4, the read path. The scoping landed as **§4b** (authority under a
split) and **§4c** (what the serve path reports), with §7, §11 and §12.1
cross-referencing them.

**A3 — SB-R5 names IX11 as where the index arm returns. It is D1.**
`RangeEligible`'s `kIndex` arm was **dead code**: a secondary index is
btree-only (IX3, `Catalog::CheckIndexDef`), so every relation that could
trip it was already declined by the `kBtree` arm one line earlier. Its
removal is behaviour-preserving for that reason and not because §6a's
question was answered. The arm is owed again the day **D1** lifts;
SA-R2's narrowing to UNIQUE-indexed shapes the re-added arm and waits on
IX11's `unique` flag, which is a condition on the arm rather than the
trigger for its return.

**What SB4 and SB5 became under A1.** SB4's first ruled cell — probe
versus in-flight split across broadcast/ack/grant scheduling points —
has no seams to seed: the discard, the rows and the caller's reply are
one task on one reactor with no suspension between them, so H-SB2's
dichotomy holds structurally and the cells assert the ordering and its
two failure directions instead. The second cell (authority collapse) is
asserted as far as the tree carries it: every boundary this path opens
discards first, so a migration inherits the rule rather than needing a
second one; the mover (R5) does not exist. SB5's SB-M2 has no
acknowledged broadcast to price.

**One defect found by lifting the gate, and it was reachable.**
`CabinOptimizerExecutor::BuildSeededSets` walked `desc_page_id` alone.
On a split heap relation that is the `lo = 0` range's chain and nothing
else (RD6: one chain per range), and the result was then committed as an
**observed** set — a subset served as authoritative, the C1 break in its
most durable form, reachable the moment SB3 admitted `CREATE CABIN` on a
split relation. It now walks `access.WalkHeadsFor(catalog_.core_id())`
and asks §4b rule 3 first. Proven rather than argued: with the fix
reverted the cell returns `{1}` where `{1, 3}` is right.
