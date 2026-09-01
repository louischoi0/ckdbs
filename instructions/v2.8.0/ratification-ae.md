# Ratification AE — v2.8.0 gives up parallelism and finishes the five auxiliaries

Recorded 2026-09-01 by CLA on `worktree-v2.8.0-ratification-ae` at
`ea49be1` (`v2.7.0-73-gea49be1`), from the operator's direction of the
same day. **This is operator input, not a CLA proposal.** It opens
`instructions/v2.8.0/` and governs every work order filed under it; where
it conflicts with a v2.7.x work order, this document wins and the older
order becomes the record of a road not taken.

## AE-1 — The direction, verbatim

> 이번 v2.8.0 버전에서는 병렬성을 포기하고 기능의 완결성과 극한의 성능
> 최적화를 통해 assertion, cabin, fk, secondary index, unique 인덱스
> 기능을 완수하는데 포커싱한다.
>
> 즉 이전 split에 따른 aux 기능 거부(refusal)을 무리하게 해결하기 위한
> 논리와 개념, 기능 코드 등은 많은 부분 제거될 필요가 있음.

and, clarifying the scope in the same session:

> range split relation is not priority, mover (R5) is too. for this
> session, just focus one range per relation and complete functionality
> with consistency and optimized form.

Rendered, for a reader who needs it: **v2.8.0 gives up parallelism.** It
focuses on **feature completeness and extreme performance optimization**,
finishing **assertions, the Cabin, foreign keys, secondary indexes, and
the UNIQUE index**. The logic, the concepts and the feature code built to
force a resolution of the auxiliary-refusal-under-split are to be removed
in large part. **The working shape is one range per relation**; the split
relation and the mover (R5) are not priorities.

## AE-2 — What the direction decides

1. **The five auxiliaries are the version's subject.** Assertion, Cabin,
   foreign keys, secondary indexes, UNIQUE index — completeness first,
   then extreme optimization on the same five. Everything that competes
   with them for this version's attention loses.
2. **One range per relation is the design assumption.** Not merely the
   default configuration — the *shape every v2.8.0 design is drawn
   against*. A relation of two or more ranges is out of this version's
   scope, and **"it must work under a split" stops being an admissible
   objection** to an auxiliary's design. The mover (R5) is likewise not
   a priority and no v2.8.0 work is sequenced behind it.
3. **The forcing work is withdrawn.** Work order SA
   (`instructions/v2.7.1/workorder-sa.md`) and its successor SB
   (`workorder-sb.md`) exist to make the auxiliaries and range ownership
   coexist. That coexistence is not this version's goal, so the concepts
   they introduced to buy it — a foreign-key reference intent, a
   range-scoped Cabin authority claim, per-owner index builds, ship-time
   structure re-derivation — are candidates for removal rather than for
   completion.
4. **A refusal is an acceptable answer again.** SA's premise was that a
   refusal under a split had to be traded for a mechanism. Under AE the
   trade is off: the refusal stands, the auxiliary is finished on the
   one-range shape that is actually shipped, and nothing is built to
   lift a gate no shipped relation reaches.
5. **Consistency and optimized form are the acceptance test**, in the
   direction's own words. A feature is finished when it is correct on
   the one-range shape, consistent with the specs that own it, and
   measured — not when it merely compiles and refuses less.

## AE-3 — The removal inventory, as the tree stands at `ea49be1`

Named so a later work order has a target list rather than a search. Every
row is a *candidate under AE-2.3*; none is removed by this document.

| # | What | Where | Note |
|---|---|---|---|
| AE-3a | The FK reference intent and its wire kinds — the whole of SA-T4's landed slice (`ff500ee`) | `include/kds/server/fk_intent.hpp`, `tests/fk_intent_test.cpp`, `kFkProbeRequest`/`kFkProbeReply` at `include/kds/sched/ring_message.hpp:218-242`, their `src/sched/spsc_ring.cpp` arms, the `tests/CMakeLists.txt` entry | **Referenced by nothing but its own test** — verified by `grep -rln 'fk_intent\|FkIntent'`. The cleanest cut in the list and the one that costs no behaviour |
| AE-3b | Ship-time structure re-derivation (SA-T1) | `exec::RestructureForExecutingCore` — `src/exec/step_compiler.cpp`, `include/kds/exec/step_compiler.hpp`, `src/server/remote_step_service.cpp`, `tests/index_compile_test.cpp` | Serves a **peer-owned** relation's stage. Under one range per relation it has no consumer this version cares about; it is *not* split-specific, so it is cut only if the shipped/remote-stage path itself is being trimmed — otherwise left alone as dormant |
| AE-3c | The range-scoped Cabin authority claim (SB-R1) and the pre-grant discard (SB-R2) | `docs/spec/cabin.md` §4b, `include/kds/stats/cabin_store.hpp`, `src/stats/cabin_store.cpp`, `DiscardObservationalCabins` at `src/server/range_alloc.cpp:44-61,173-195`, `include/kds/server/refusal_counters.hpp` | The *authority narrowing* is exactly the concept AE-2.3 names. **Do not remove it while the `kCabin` gate stays down** — AE-4.2 |
| AE-3d | The gate rewrites that dropped `kIndex` and `kCabin` from `RangeEligible` (SB3) | `src/exec/range_eligible.cpp:26-52`, `include/kds/exec/range_eligible.hpp`, `tests/range_eligible_test.cpp` | Restoring the arms is the conservative direction and costs nothing under AE-2.2, since no v2.8.0 relation splits |
| AE-3e | The `RefuseAuxiliaryOnSplitRelation` narrowing programme (SA-T2/T3/T6) | `src/catalog/catalog.cpp:1094`, FK arms at `:2876,:2879`, index arm at `:3046` | **The refusals themselves stay.** What is withdrawn is the plan to take them down |
| AE-3f | The unbuilt remainder of SA | SA-T3, T5, T7, T8, T9 in `docs/inflight/in-progress/workplan-auxiliaries-under-split.md` | Never started; withdrawn on paper only |
| AE-3g | The prose that carries the concepts | `docs/spec/crosscore.md` §6a, `docs/spec/cabin.md` §4b/§11/§12, `docs/spec/foreign-keys.md` F5/F3, `docs/spec/index.md` §13, `docs/inflight/known-gaps.md`'s SA/SB entries, and the workplan above | Prose is removed **last**, after the code it describes, and a withdrawn design is *marked withdrawn* rather than deleted — the `create-pattern` precedent (`instructions/v2.7.0/pd-remove-declared-patterns.md`) |

## AE-4 — The line: what removal may not cost

Three limits. They are not hedges on the direction; they are what keeps
"remove the forcing logic" from becoming "remove a correctness guard".

**AE-4.1 — A refusal is never removed by this programme.** Every
`RefuseAuxiliaryOnSplitRelation` arm, `RangeEligible` gate, and
`CheckForeignKeyColocation` refusal is *strengthened* by AE, not
loosened. Removing the plan to lift a gate is the work; removing the
gate is its opposite.

**AE-4.2 — A guard that keeps a live gate honest stays until the thing
it guards is gone.** Two of SB's outputs are guards, not forcing logic,
and the workplan proves each by having broken it
(`workplan-auxiliaries-under-split.md` §3.2): `CheckNoChildReferences`
walking `WalkHeadsFor(core_id)` rather than one chain, and the
`ServableBy` scope refusal in front of the Cabin serve
(`src/exec/fk_check.cpp`). With either reverted, a parent DELETE over a
split child answered `DELETED 1` — a dangling foreign key with the
constraint reporting success. **These stay** while a relation can still
be split by any reachable path. The same sentence governs AE-3c: the
`kCabin` gate came down *because* the authority claim was narrowed, so
the claim may not be un-narrowed before the gate goes back up. The order
is: gate first, concept second.

**AE-4.3 — Nothing is removed that a shipped configuration executes.**
The removal's test is "no relation an operator can reach runs this",
demonstrated by `cores = 1` byte-identity plus the full suite green, not
by argument. A candidate that fails that test is reported, not cut.

## AE-5 — What "one range per relation" leaves standing

AE-2.2 sets the design shape; it does not by itself delete a subsystem.
Until the operator says otherwise, range ownership (R0-R4, the range
directory, insert spreading, cross-owner transactions, statement
shipping, the answer edge) **stays in the tree, shipping off, untouched
and unextended** — and the AE-4.2 guards stay with it, because a
mechanism that is reachable is a mechanism that must be correct. Deleting
the subsystem outright is a larger step (it would retire
`blueprint-range-ownership.md`, three workplans, `Expeditor`'s spreading,
and turn much of the `bench/v2.6.0`/`v2.7.0` record into history) and is
**the operator's call, not CLA's**. Working under the smaller step wastes
nothing if the larger one is taken later.

## AE-6 — What the version owes, so "완수" is checkable

The five subjects, with what "finished" would have to mean, drawn from
the milestone rows they already have — **each read at one range per
relation**, which is what makes several of them tractable at all. This is
the shape of the work orders to be filed under `instructions/v2.8.0/`,
not the orders themselves:

- **Assertions** — the admission straddle a parked statement opens, the
  unacknowledged `done` legs, the pre-PW1c-6c file's core-0-built cabin
  (`docs/spec/assertion.md` §6.1,
  `docs/inflight/in-progress/workplan-peer-writer.md` §7d).
- **Cabin** — CB12-CB14 (the correlated probe, EXISTS convergence,
  per-key observation), and `docs/spec/cabin.md` §11's open budgets,
  `CABIN AUTO` threshold, pruning cadence, entry-set persistence, and
  multi-column keys.
- **Foreign keys** — the forward-check expression, the busy status code,
  heap parents, `kFkNullable` (`docs/spec/foreign-keys.md`). CASCADE and
  SET NULL are v1's stated non-goals and stay out unless the operator
  says otherwise.
- **Secondary indexes** — `docs/spec/index.md` §13's residue:
  `kIndexStringKeyBytes`, split point, column caps, entry reclamation,
  the index-only scan (gated on a visibility witness — **no partial
  one**), and IX3's heap-relation admission, which
  `instructions/v2.7.2/index.md` (work order IB) already specifies end to
  end. **AE does not withdraw IB**: it is a single-core feature and it is
  this version's subject. §13's per-range-vs-global question simply does
  not arise at one range.
- **UNIQUE index** — IX11, refused today at
  `src/catalog/catalog.cpp:3034-3036`. Under AE it is a **single-range**
  uniqueness problem, which is the version's whole point: SA-R2 kept it
  gated because cross-range uniqueness was undesigned, and AE removes the
  question rather than answering it.

And the second half of the direction, which is not a feature list:
**극한의 성능 최적화**. The CIP programme (`CIP/`, OPT-001..OPT-006) is
the existing vehicle and its discipline is this version's — measured in
`build-release`, interleaved A/B, per-statement server CPU, every result
filed under `bench/v2.8.0/` named by `git describe --tags`.

## AE-7 — Where this lands

Nothing in this document changes code. What it changes on acceptance:
`CLAUDE.md`'s milestone rows for cross-core execution, range-granular
ownership and the three auxiliaries gain AE's withdrawal sentence;
`docs/inflight/known-gaps.md` re-points its SA/SB entries here;
`workplan-auxiliaries-under-split.md` is marked withdrawn under AE-3g's
rule rather than deleted; and the work orders of AE-6 are filed under
`instructions/v2.8.0/`, one subject at a time.
