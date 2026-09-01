# Work order AH — the foreign key crosses the owner: the forward check parks at the dispatch fork

Drafted 2026-09-01 by CLA against `main` at `e5861b0`
(`v2.7.0-79-ge5861b0`). Files under `instructions/v2.8.0/` (AE-6's FK
subject, first-class per AE-2's seam: this is owner-granular work with
no split in it). The series letter skips AG, which
`docs/spec/aggregate.md` already owns (AG-M). Operator direction of
2026-09-01 ratified the survey's option (a) — the hoist — and declined
to reverse FK-M2; both are folded in below as rulings rather than open
questions. Source of record:
`docs/inflight/in-progress/workplan-auxiliaries-under-split.md` §4
(the survey this order answers), `src/server/command_dispatcher.cpp`
(`CheckForeignKeyOnWrite` and its three callers; the dispatch fork's
`pending_remote`), `src/catalog/foreign_key.cpp:31`
(`CheckForeignKeyColocation`), commit `ff500ee` (SA-T4 first slice:
`kFkProbeRequest`/`kFkProbeReply` and the reference intent, landed
unreachable), commit `925fc9d` (the reverse check),
`docs/spec/foreign-keys.md` (F1..F6, FK-M1..M5),
`docs/spec/cross-owner-txn.md` §1a (SA-T0's no-record prepare),
`instructions/v2.8.0/ratification-af-namespace.md` AF-P5.

## Rulings — AH-R1..AH-R7

**AH-R1 — The park point is the dispatch fork, and the write scope
never waits.** Ratified from the survey's option (a): every foreign
parent pk a statement needs is extracted **before any row work** — an
INSERT's from its `VALUES`, an UPDATE's from its `SET` body — probed
and enrolled at the fork where `HandleInsert`/`HandleUpdate` already
know how to return `pending_remote` and resume. The statement then
runs **synchronously** against the intents it holds. Nothing inside an
open `WriteScope` ever initiates a ring round trip; the per-row check
that remains there answers from held intents and local state only.
RD5's wall stands untouched and unmet.

**AH-R2 — One round per distinct owner, not per pk.** The extracted
parent pks are deduplicated and grouped by resolved owner; each
foreign owner receives **one** `kFkProbeRequest` carrying its set, and
enrolment of that owner as a participant rides the same round (RR1's
enrol-on-first-contact shape). A statement's foreign-FK cost is
therefore a function of *how many distinct owners its parents live
on*, never of its row count — H-AH2 exists to hold this to the
counters.

**AH-R3 — An unknowable parent set refuses, fail-closed, and today it
cannot happen.** F1 makes an fk value a literal or a bound parameter,
so the extraction pass is total. The rule is still stated for the day
F1 moves: a statement whose parent set cannot be enumerated at the
fork is refused with a message naming the byte, never run with a
partial intent set. In code this is an assert-and-refuse arm, not a
handled path.

**AH-R4 — No new status code.** The busy verdict stays
`FkVerdict::kBusy` → `Status::TxnConflict`, wire-spelled
`TXN_CONFLICT`, one retryable code wide. SA-R7's `kConstraintBusy` is
**not** enacted: FK-M2's decline stands, per the operator's direction
and `status.hpp`'s standing argument on the width of the wire's
`retryable` bit. If internal distinguishability is ever wanted it is a
later, protocol-silent addition.

**AH-R5 — Intents are memory-resident by design, and the safety is an
invariant with a crash cell, not an argument.** Under SA-T0 an
intent-only participant writes no `TXN_PREPARE` record; its intents
die with it. The invariant that makes this safe: **a participant that
restarts after granting an intent and before its prepare leg forces
the coordinator's transaction to fail** — the prepare cannot be
answered by a process that has lost the enrolment. AH-T5 states the
mechanism against `prepared_resolver.hpp` and proves it by killing the
participant in that window, per the process-kill discipline. A window
in which the coordinator can still commit is a defect of this order,
full stop.

**AH-R6 — The colocation refusal's disposition is the operator's mark
at AH-T0, because two records point different ways.** `ff500ee`'s
message calls dropping `CheckForeignKeyColocation` "SA-T6's arm";
AF-P5, recorded later the same day from operator input, says the
refusal "becomes satisfiable, **and stays**", with the namespace as
the compliance instruction — while also saying it "does not close the
protocol question" for relations already on two cores. AH is that
protocol. The reconciliation this order proposes, for ratification:
the refusal stands until AH's final task, then **converts from
constraint to recommendation** — a cross-owner `REFERENCES` is
admitted, and the message (AF-T4's) keeps naming colocation-by-
namespace as the cheaper shape. If the operator marks "stays" as
permanent instead, AH still lands whole: the forward check's crossing
is exercised by relations split from their parents by history, and
the declaration arm simply never opens.

**AH-R7 — The KWP load path is in scope, and its fallback is a named
refusal.** `CheckForeignKeyOnWrite`'s third caller is the load path,
which has its own batch boundary. The hoist applies there at that
boundary; if AH-T2's survey finds no park-capable seam in it, the
load path **refuses** a cross-owner-FK write fail-closed with a
message naming this order, and the refusal is recorded in
`known-gaps.md` — never a silent local-only check.

## Background

Restated once, source-read:

1. **The check runs where nothing can wait.** `CheckForeignKeyOnWrite`
   is a plain `Status` member; its callers (`SortedFillInner`, the
   UPDATE path, the KWP load path) are plain members; the check fires
   per row inside an already-open `WriteScope`. Nothing on that stack
   suspends (survey §4.1). The engine's one cross-core park for a
   write is the dispatch fork's `pending_remote`.
2. **The wire and the intent already exist, unreachable.** `ff500ee`
   landed `kFkProbeRequest`/`kFkProbeReply` and the reference intent
   "ahead of their sender"; `925fc9d` landed the reverse check
   ("answer for the whole child, or refuse"). AH is the sender.
3. **What is not in the way** (survey §4.4): the forward check is not
   a sub-chain, so `step_vm`'s no-await rule does not bind it; writes
   already cross owners and RR1 already enrols on contact.

## Hypotheses

- **H-AH1 (the hoist changes no colocated answer, for free).** On a
  colocated parent, arm A (today's per-row check) and arm B (the
  hoisted extraction + per-row answer-from-held-state) return
  byte-identical verdicts on every fixture cell, and B's latency
  increment on the colocated path is zero within noise — measured,
  not asserted, because the extraction pass runs before knowing the
  parents are local.
- **H-AH2 (cost counts owners, not rows).** A statement writing N ∈
  {1, 10, 100} child rows against one foreign parent owner shows a
  flat cross-owner increment across N; two distinct foreign owners
  show approximately twice the increment at every N. The counters
  (probe requests sent, participants enrolled) are reported beside
  latency so the shape is visible, not inferred.
- **H-AH3 (the restart window cannot commit).** Kill the intent-
  holding participant after grant, before prepare: the coordinator's
  transaction fails, every time, and the answer is inside the
  existing outcome taxonomy (error / `UnknownOutcome` / clean close —
  no new class).
- **H-AH4 (the race answers retryable, never wrong).** A parent
  `DELETE` racing a foreign child's intent grant resolves to
  `TXN_CONFLICT` on exactly one side or a clean serialization of
  both — never a committed child row whose parent committed its
  delete first.

## Tasks

**AH-T0 — the rulings land as text.** Amend `foreign-keys.md` (the
forward check's crossing; AH-R3's rule beside F1; AH-R4's non-reversal
recorded in F3's paragraph in FK-M2's own words), `crosscore.md`'s FK
paragraph, and `workplan-auxiliaries-under-split.md` (SA-T4/SA-T6 rows
retired into AH with a pointer). Record AH-R1..R7 as ratified or
amended; AH-R6 carries the operator's mark. Gate: none.

**AH-T1 — the hoist, colocated first.** The extraction pass at the
fork (parent pks from `VALUES` / `SET`), the held-state answer inside
the write scope, and the three callers converted — **with no foreign
probe yet wired**, so the change is a pure restructuring of the local
path. Tests: verdict byte-identity per H-AH1's fixture (INSERT,
UPDATE, the retired-parent and in-flight-parent cells); the colocated
A/B measurement. This lands alone because it must be provably free
before it is allowed to be useful. Gate: AH-T0.

**AH-T2 — the crossing.** Owner resolution and per-owner grouping;
the `kFkProbeRequest` sender; enrolment riding the probe round;
resumption into the synchronous statement against held intents; the
KWP load path's seam surveyed and either hoisted or refused per
AH-R7. Tests: one-owner and two-owner fixtures; the probe-reply
negative (parent absent → `kFkViolation` before any row is written);
the in-flight parent (→ `TXN_CONFLICT`, retryable). Gate: AH-T1.

**AH-T3 — the reverse meets the foreign intent.** The parent-side
`DELETE`/`UPDATE` path answers a foreign child's held intent exactly
as it answers a local one — `FkVerdict::kBusy` through the existing
spelling. The race cell of H-AH4 lives here. Gate: AH-T2.

**AH-T4 — the declaration arm, per AH-R6's mark.** If the mark is
"converts": `CheckForeignKeyColocation` admits, and the refusal text
becomes AF-T4's recommendation message (coordinated with AF, which
owns that message's namespace wording). If the mark is "stays": this
task writes the known-gaps entry saying the protocol exists and the
declaration does not use it, and why. Gate: AH-T3, and AF-T4 for the
message wording only.

**AH-T5 — the crash half.** The restart-window cell of H-AH3 (kill
after grant, before prepare), the post-prepare cell (existing matrix
ground re-run with an intent-only participant), and H-AH4's race
cell. **Gate: the owed XG3 process-kill half fires first** (operator
decision of 2026-09-01, item 4(b): the shipped-read matrix debt is
repaid before new crash surface is added to the 2PC path), then
AH-T2.

**AH-T6 — measurement and closure.** The H-AH1/H-AH2 matrix under
`bench/v2.8.0/`, XD-style leg attribution on the cross-owner cell so
the probe round's cost sits beside the commit legs it joins;
`known-gaps.md` and the workplan rows closed; a verdict section
naming which hypotheses held, refused, or split. Gate: AH-T5.

## Measurement discipline

`build-release` only; `git describe --tags` naming; ≥3 runs with
min/max/stddev; interleaved arms for every A/B; probe/enrolment
counters reported beside every latency; results under
`bench/v2.8.0/`; no retroactive stamping; claims tagged **measured**
(with invocation) or **source-read** (with `path:line` and commit).

---

## Row status (CLA, appended as rows land)

| row | status |
|---|---|
| AH-T0 | **Done 2026-09-01.** AH-R1..R7 recorded; AH-R6 carries the operator's mark below. Amendments landed: `foreign-keys.md` gains **§2a** (the crossing, the fork park, the per-owner round, AH-R3's fail-closed rule, AH-R5's invariant, AH-R7's load-path refusal), F3 carries AH-R4's non-reversal, F5 carries the conversion; `crosscore.md`'s FK bullet splits along AE-2's seam; `workplan-auxiliaries-under-split.md`'s SA-T4/SA-T6 rows retired into AH |
| AH-T1..T6 | not started |

### AH-R6 — the operator's mark, 2026-09-01

> **Converts at AH-T4.**

The refusal stands unchanged through AH-T1..T3 and becomes a
recommendation at AH-T4: a cross-owner `REFERENCES` is admitted, and
the message names colocation-by-namespace as the cheaper shape
(AF-T4 owns that wording).

Two things the mark settles that are worth stating separately, because
each was a live reading of the record before it:

- **`ff500ee`'s "SA-T6's arm" and AF-P5's "and stays" are reconciled by
  *time*, not by one of them being wrong.** The refusal stays while
  there is no protocol behind it, and converts once there is. AF-P5's
  own sentence — the refusal "does not close the protocol question" —
  is what leaves the door open; AH is the protocol that walks through
  it.
- **The third option was declined and the reason is a defect, not a
  preference.** Converting at AH-T1 would admit a declaration whose
  forward check is still `CheckParentPresent`'s local `BtreeLookup` on
  `parent.desc_page_id`, with no ownership question anywhere in it — a
  page this core may not fault. That is the survey's §3.1 finding, and
  it is why the conversion is gated on the crossing rather than
  shipped ahead of it.

### AH-T0 — what the amendment pass found

Two things worth carrying into AH-T1, neither of which changes a
ruling:

1. **F4 is already known-unbuildable, and §2a does not revive it.**
   FK-M1's amendment records that the implicit correlated sub-chain
   "cannot be written as specified" — INSERT compiles to no chain at
   all. §2a's hoist is therefore not a *replacement* for the sub-chain
   shape but the first mechanism that fits what the write path
   actually is, and it inherits FK-M1's two consequences unchanged:
   no probe memo (it lives in the step VM), and `ExecStats`/ANALYZE do
   not see the check for free. **AH-T6's counters are the second
   consequence's answer** and should be read as continuing FK-M4's
   debt rather than opening a new one.
2. **§2's "probe memo makes batch insert nearly free" is already
   false** and is now false in a second way. It was false because
   FK-M1 removed the memo; under AH-R2 the batch shape is cheap for a
   *different* reason — one round per owner, not one memo hit per row.
   The old sentence is left standing in §2 where FK-M1's amendment
   already qualifies it, rather than edited twice; §2a states the new
   mechanism in its own words.
