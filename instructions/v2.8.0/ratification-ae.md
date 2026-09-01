# Ratification AE — v2.8.0 drops the range split, keeps the parallel transaction, and finishes the five auxiliaries

Recorded 2026-09-01 by CLA on `worktree-v2.8.0-ratification-ae` at
`ea49be1` (`v2.7.0-73-gea49be1`); **amended the same day** after the
first draft misread the direction — AE-9 records what was wrong, because
a governing document that was once wrong about its own subject has to say
so. **This is operator input, not a CLA proposal.** It opens
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

> range split relation is not priority, mover (R5) is too. for this
> session, just focus one range per relation and complete functionality
> with consistency and optimized form.

and the correction that fixes what "병렬성을 포기한다" does and does not
mean:

> 병렬성을 포기한다는 건 relation당 split을 통해 range를 잘게 나누지
> 않는다는 뜻이지, 2PC를 통한 병렬 트랜잭션 기능은 모두 보존되거나
> 최적화 되어야함. range split을 통해 못얻는 병렬성을 다른 관점에서
> 해결하고자 함. kdbs 전체 프로젝트 관점에서는 소프트웨어가 발휘할수
> 있는 극한의 성능과 논리적 견고성을 구현하는것이 목표

> 즉 단일코어 뿐 아니라 병렬성을 최대한 높이되, range split을 이용하지
> 않는 다는 것 뿐임.

## AE-2 — The seam: two different axes, and only one is dropped

This is the distinction the whole document turns on, and the one the
first draft collapsed.

**Owner-granular parallelism — KEPT, and pushed as far as it goes.** A
relation has an owner core; different relations have different owners; a
transaction touching two of them crosses, and 2PC is how it commits.
Statement shipping, cross-owner reads and writes, the answer edge, the
decomposed commit and its ack-at-append — **all of it is preserved**, and
"보존되거나 최적화" is not a courtesy in the direction's sentence. The
operator's third message settles the strength of the verb: **parallelism
is to be maximized, not merely kept** — 단일코어 뿐 아니라 병렬성을 최대한
높이되. What is forbidden is one mechanism for reaching it, the range
split, and nothing else. A v2.8.0 order that increases concurrency by any
other means is on-subject, not a distraction from the five auxiliaries.

**Range-granular parallelism — DROPPED.** Subdividing *one* relation into
many ranges spread across cores, so that one relation has many writers.
That is the axis v2.8.0 gives up: **one range per relation**, the mover
(R5) not a priority, no design drawn against a split relation.

The auxiliary refusals that motivated work orders SA and SB are
**range**-shaped, not owner-shaped: `RefuseAuxiliaryOnSplitRelation`
fires on a relation of two or more ranges, and `RangeEligible` decides
whether one relation may be cut. Those are what AE withdraws. A refusal
that fires because two *relations* sit on two cores —
`CheckForeignKeyColocation` at `src/catalog/foreign_key.cpp:31`, which
refuses `parent.owner_core != child.owner_core` — is **owner**-shaped and
is therefore this version's *work*, not its casualty.

**The parallelism the split would have bought is to be found elsewhere.**
The direction says so explicitly and does not name where. AE records it
as the version's standing question (AE-8) rather than answering it.

## AE-3 — What the direction decides

1. **The five auxiliaries are the version's subject.** Assertion, Cabin,
   foreign keys, secondary indexes, UNIQUE index — completeness first,
   then extreme optimization on the same five.
2. **One range per relation is the design assumption.** Not merely the
   shipped default — the shape every v2.8.0 design is drawn against.
   "It must work under a split" stops being an admissible objection to an
   auxiliary's design. The mover (R5) is not a priority and no v2.8.0
   work is sequenced behind it.
3. **Parallelism is maximized; only the range split is off the table.**
   Every mechanism that lets a transaction span two *relations* on two
   cores stays, and increasing concurrency is an explicit goal of this
   version rather than a tolerated side effect. Nothing in AE's removal
   inventory may cost a cross-owner transaction a capability, and no
   design here may assume single-core execution as its only case.
4. **The split-forcing work is withdrawn.** SA
   (`instructions/v2.7.1/workorder-sa.md`) and SB (`workorder-sb.md`)
   exist to make auxiliaries coexist with a *split* relation. The
   concepts they introduced for that — a range-scoped Cabin authority
   claim, per-owner index builds, fan-out to child-range owners, Bound
   Cabin migration, var-heap page grants across a boundary — are
   candidates for removal rather than completion. **Their owner-granular
   parts are not** (AE-2).
5. **A range refusal is an acceptable answer again.** SA's premise was
   that a refusal under a split had to be traded for a mechanism. That
   trade is off. An *owner* refusal is a different matter and AE-6 asks
   for several of them to be answered rather than kept.
6. **Consistency and optimized form are the acceptance test**, in the
   direction's own words: correct on the one-range shape, consistent with
   the spec that owns it, and measured. Project-wide, the goal it serves
   is stated once and applies to every order filed here — **극한의 성능과
   논리적 견고성**, the extreme performance the software can reach and the
   logical robustness that makes the performance mean something.

## AE-4 — The removal inventory, as the tree stands at `ea49be1`

Range-shaped only. Each row is a *candidate under AE-3.4*; none is
removed by this document, and every one is subject to AE-5.

| # | What | Where | Note |
|---|---|---|---|
| AE-4a | The range-scoped Cabin authority claim (SB-R1) and the pre-grant discard (SB-R2) | `docs/spec/cabin.md` §4b, `include/kds/stats/cabin_store.hpp`, `src/stats/cabin_store.cpp`, `DiscardObservationalCabins` at `src/server/range_alloc.cpp:44-61,173-195`, `include/kds/server/refusal_counters.hpp` | Squarely the concept AE-3.4 names — a set's authority narrowed to *(observed value × the ranges its core owns)* exists only because one relation had many owners. **Do not remove it while the `kCabin` gate stays down** — AE-5.2 |
| AE-4b | The gate rewrites that dropped `kIndex` and `kCabin` from `RangeEligible` (SB3) | `src/exec/range_eligible.cpp:26-52`, `include/kds/exec/range_eligible.hpp`, `tests/range_eligible_test.cpp` | Restoring the arms is the conservative direction and costs nothing at one range per relation |
| AE-4c | The `RefuseAuxiliaryOnSplitRelation` narrowing programme (SA-T2/T3/T6's split halves) | `src/catalog/catalog.cpp:1094`, FK arms at `:2876,:2879`, index arm at `:3046` | **The refusals stay** (AE-5.1). What is withdrawn is the plan to take them down |
| AE-4d | The unbuilt split-shaped remainder of SA | SA-T3 (per-owner index builds), SA-T5b (fan-out to child-*range* owners), SA-T7 (Bound Cabin migration), SA-T8 (var-heap page grants across a boundary), SA-T9 — `docs/inflight/in-progress/workplan-auxiliaries-under-split.md` | Never started; withdrawn on paper only |
| AE-4e | The prose carrying the range-shaped concepts | `docs/spec/crosscore.md` §6a, `cabin.md` §4b/§11/§12, `index.md` §13's per-range-vs-global question, `known-gaps.md`'s SA/SB entries, and the workplan above | Prose goes **last**, after the code it describes, and a withdrawn design is *marked withdrawn* rather than deleted — the `create-pattern` precedent (`instructions/v2.7.0/pd-remove-declared-patterns.md`) |

**Explicitly not on this list, and kept:**

- **SA-T0**, the read-only participant optimisation (`1beda80`) — a
  commit-path optimization, exactly what AE-3.3 preserves.
- **SA-T1**, `exec::RestructureForExecutingCore`
  (`src/exec/step_compiler.cpp:1053`, `include/kds/exec/step_compiler.hpp:126`,
  `src/server/remote_step_service.cpp`) — it serves a stage on a
  **peer-owned relation**, which is owner-granular and survives at one
  range per relation.
- **SA-T4's landed slice** (`ff500ee`) — `include/kds/server/fk_intent.hpp`,
  `tests/fk_intent_test.cpp`, and `kFkProbeRequest`/`kFkProbeReply` at
  `include/kds/sched/ring_message.hpp:218-242`. The forward check it
  serves crosses because parent and child sit on **different cores**, not
  because either is split. It is unreferenced today
  (`grep -rln 'fk_intent\|FkIntent'` finds only its own test) — that
  makes it *unfinished*, which AE-6 asks for, not *dead*.
- **SA-R6/SA-R7** — F5 relaxed from "same core" to "2PC-reachable owner",
  and `kConstraintBusy` de-proposed. Both are owner-granular and both are
  AE-6's FK work.
- Every part of cross-owner transactions, statement shipping, and the
  answer edge (XD, XE, XF, XG).

## AE-5 — The line: what removal may not cost

**AE-5.1 — A refusal is never removed by this programme.** Every
`RefuseAuxiliaryOnSplitRelation` arm and `RangeEligible` gate is
*strengthened* by AE, not loosened. Removing the plan to lift a gate is
the work; removing the gate is its opposite. (`CheckForeignKeyColocation`
is not in this class — it is owner-shaped, and AE-6 asks for it to be
answered.)

**AE-5.2 — A guard that keeps a live gate honest stays until the thing it
guards is gone.** Two of SB's outputs are guards, not forcing logic, and
the workplan proves each by having broken it
(`workplan-auxiliaries-under-split.md` §3.2): `CheckNoChildReferences`
walking `WalkHeadsFor(core_id)` rather than one chain, and the
`ServableBy` scope refusal in front of the Cabin serve
(`src/exec/fk_check.cpp`). With either reverted, a parent DELETE over a
split child answered `DELETED 1` — a dangling foreign key with the
constraint reporting success. **These stay** while a relation can still
be split by any reachable path. The same order governs AE-4a: the
`kCabin` gate came down *because* the authority claim was narrowed, so
the claim may not be un-narrowed before the gate goes back up. Gate
first, concept second.

**AE-5.3 — Nothing is removed that a shipped configuration executes**,
and nothing is removed that a **cross-owner transaction** needs. The
test is demonstrated — `cores = 1` byte-identity, the cross-owner suites,
and the full suite green — not argued. A candidate that fails it is
reported, not cut.

## AE-6 — What the version owes, so "완수" is checkable

Each subject read at **one range per relation**, and each free to cross
**owners**. This is the shape of the work orders to be filed here, not
the orders themselves.

- **Assertions** — the admission straddle a parked statement opens, the
  unacknowledged `done` legs, the pre-PW1c-6c file's core-0-built cabin
  (`docs/spec/assertion.md` §6.1, `workplan-peer-writer.md` §7d).
- **Cabin** — CB12-CB14 (correlated probe, EXISTS convergence, per-key
  observation), and `cabin.md` §11's budgets, `CABIN AUTO` threshold,
  pruning cadence, entry-set persistence, multi-column keys.
- **Foreign keys** — the forward-check expression, the busy status code,
  heap parents, `kFkNullable`; and now, as first-class rather than
  deferred, **the cross-owner FK**: `CheckForeignKeyColocation`
  (`src/catalog/foreign_key.cpp:31`) refuses a parent and child on
  different cores, which under AE-2 is a parallelism the version keeps.
  SA-R6/R7 and the `fk_intent` slice are its starting material; the
  survey's blocker — the forward check has no park point
  (`workplan-auxiliaries-under-split.md` §4.1) — is the thing to answer,
  and answering it is owner-granular work with no split in it. CASCADE
  and SET NULL stay out as v1 non-goals unless the operator says
  otherwise.
- **Secondary indexes** — `docs/spec/index.md` §13's residue:
  `kIndexStringKeyBytes`, split point, column caps, entry reclamation,
  the index-only scan (gated on a visibility witness — **no partial
  one**), and IX3's heap-relation admission, which
  `instructions/v2.7.2/index.md` (work order IB) already specifies end to
  end. **AE does not withdraw IB.** §13's per-range-vs-global question
  simply does not arise at one range.
- **UNIQUE index** — IX11, refused today at
  `src/catalog/catalog.cpp:3034-3036`. Under AE it is a **single-range**
  uniqueness problem: SA-R2 kept it gated because cross-*range*
  uniqueness was undesigned, and AE removes that question rather than
  answering it. Cross-*owner* uniqueness does not arise either — an index
  belongs to its relation, and a relation has one owner.
- **The cross-owner commit path itself**, because AE-3.3 says "preserved
  **or optimized**". The record to build on: XD's decomposition (three
  device syncs, two of the three legs unconditional on durability class),
  XE's ack-at-append (waited syncs 3 → 2; **25.9% of commit p50 at eight
  concurrent coordinators**, and nothing serially), SA-T0's read-only
  participant. `docs/spec/cross-owner-txn.md` §5a carries the sizing.

**극한의 성능 최적화** is the second half of the direction and not a
feature list. The CIP programme (`CIP/`, OPT-001..OPT-006) is the vehicle
and its discipline is this version's — `build-release`, interleaved A/B,
per-statement server CPU, every result under `bench/v2.8.0/` named by
`git describe --tags`.

## AE-7 — What one range per relation leaves standing

Range ownership's *machinery* (R0-R4, the range directory, insert
spreading, `Expeditor`'s `kRangeSizeOff`) stays in the tree, shipping
off, untouched and unextended — and the AE-5.2 guards stay with it,
because a mechanism that is reachable is a mechanism that must be
correct. Deleting it outright is a larger step, and **the operator's
call, not CLA's**. Working under the smaller step wastes nothing if the
larger one is taken later. What does *not* fall under this paragraph is
everything owner-granular: that is not "left standing", it is worked on.

## AE-8 — The standing question this version opens `[ANSWERED 2026-09-01 — AF]`

**The answer arrived the same day and has its own document**, as this
section required: `ratification-af-namespace.md`. A **namespace** — a
logical grouping, no physical meaning — selects a relation's owner core
at `CREATE`, fixed by the namespace's first relation, and the operator's
best practice is to put highly-wired relations in one namespace. The
parallelism a range split would have bought *inside* one relation is
bought *between* groups of relations, with the grouping declared by the
person who knows it. 2PC is untouched: a namespace is a placement
declaration, never a boundary.

What follows is the question as AE first stated it, kept because AF's
argument is a reply to it and reads as one.

---


> range split을 통해 못얻는 병렬성을 다른 관점에서 해결하고자 함.

The parallelism a split relation would have bought — many writers on one
relation — is given up **without a replacement named**, and the operator
asks for it to be found from another angle rather than written off. AE
records the question as *open and active*: the candidates already in this
tree (the decomposed commit's waited-sync reduction, the statement-local
inner build, the Cabin's banked acceleration, the CIP hot-path work, the
physical optimizer's relayout) are optimizations of a single-writer core
rather than a second writer, so none of them is the answer yet. **No work
order may assume this question is settled**, and any order that proposes
an answer says so in its own title.

## AE-9 — Correction record

The first draft of this document (committed `1481fad`) rendered
"병렬성을 포기한다" as *"v2.8.0 gives up parallelism"* full stop, and put
three owner-granular things on the removal list that do not belong there:
the `fk_intent` slice, `RestructureForExecutingCore`, and by implication
SA-R6/R7. The operator's correction draws the seam at **range split vs
2PC-crossing transaction**, which AE-2 now states first because every
other section depends on it. Recorded rather than silently edited: the
misreading is the kind a later reader would otherwise re-derive from the
same two sentences.

## AE-10 — Where this lands

Nothing here changes code. On acceptance: `CLAUDE.md`'s milestone rows
for range-granular ownership and the three auxiliaries gain AE's
withdrawal sentence (and its **limit** — cross-core execution's row is
*not* withdrawn); `docs/inflight/known-gaps.md` re-points its SA/SB
entries here; `workplan-auxiliaries-under-split.md` is marked withdrawn
in its range-shaped parts under AE-4e's rule rather than deleted; and the
work orders of AE-6 are filed under `instructions/v2.8.0/`, one subject
at a time.
