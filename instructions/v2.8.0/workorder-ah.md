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
| AH-T1 | **Built 2026-09-01**, no probe wired — the local path restructured only. `exec::FkParentVerdicts` (child-side, statement-scoped) holds one verdict per distinct (parent relation, pk); `CommandDispatcher::ResolveForeignKeyParents` is the extraction pass and `CheckForeignKeyOnWrite` starts no descent. All three callers converted. **The colocated A/B measurement (H-AH1's latency half) is not run** — see below. Full suite 3139/3139 green |
| AH-T2 | **First slice built 2026-09-01** — owner resolution, per-owner grouping (`FkParentVerdicts::Defer`), and the fail-closed refusal at all three sites. **The wire is not built**: no payloads, no sender, no park/resume. AH-R7 is answered by survey and needed no code. Full suite 3140/3140 green |
| AH-T3..T6 | not started |

### AH-T2, first slice — what it built and what it corrected

**Built.** The extraction pass asks the parent's `owner_core` **before**
descending. A parent this core owns resolves as at AH-T1; a foreign one is
deferred into its owner's group — `FkParentVerdicts::Defer`, deduplicated
per (relation, pk) and grouped per **owner**, which is the object one
`kFkProbeRequest` will carry. Nothing sends it, so all three sites then
call `RefuseUnsentForeignKeyProbes`, which refuses `NotImplemented` naming
the owners the statement would have had to ask.

The refusal is the point of the slice. `CheckParentPresent` descends
`parent.desc_page_id` with **no ownership question anywhere in it**
(survey §3.1), so on a foreign parent it faults a page this core may not
fault, or answers from one. Refusing is the only correct third option, and
§1's rule — a constraint that silently does not run is not a degraded mode
— is what rules out the other two.

**Corrected, twice, and both corrections matter more than the code.**

1. **This is defence in depth, not a live-bug fix.** A first framing of
   this slice called the local descent on a foreign parent "a live
   hazard". It is not reachable: `CheckForeignKeyColocation` refuses a
   cross-owner declaration, and a dispatcher on a core owning neither
   relation is turned away first by the peer-write refusal. A foreign
   parent arises only where migration separated an already-declared pair —
   AH-R6's "relations split from their parents by history" — which nothing
   builds for user relations yet. The guard is right to have on the safe
   side of a refusal (SB's precedent), and saying it closes something live
   would overstate it.

2. **The cell that "proved" it was catching a different refusal.** A first
   draft ran the INSERT on a `core_id = 1` dispatcher and asserted a
   refusal. It passed — on *"this transaction's writes are bound to core 1
   and relation 'trades' is owned by core 0"*, the pre-existing peer-write
   refusal, which fires before the extraction pass. The cell was green and
   proved nothing about AH-T2. It is replaced by a unit cell on the
   grouping itself (`FkParentVerdicts.ForeignParentsGroupByOwnerAndDeduplicate`),
   at the same level and for the same stated reason the F5 colocation cell
   already uses: there is no way to *create* a cross-core pair, and the
   check exists for when there is.

**AH-R7 is answered by survey, and the work order's premise was wrong.**
The background names three callers as `SortedFillInner`, the UPDATE path
and "the KWP load path". The third is `InsertOneRow`; there is **no
separate load-path caller**. `KwpLoadServer::HandleLoadChunk` builds a
`parser::InsertStmt` and calls `dispatcher_->ExecuteInsert` — the same
function the extraction pass now lives in — so the load path inherits the
hoist, the per-owner grouping and the refusal without a line of its own.
AH-R7's fallback ("if no park-capable seam exists, refuse fail-closed") is
satisfied structurally rather than by a special case, and the
`known-gaps.md` entry it anticipated is not owed.

**What the rest of AH-T2 still owes**, unchanged: the
`FkProbeRequestPayload` / `FkProbeReplyPayload` structs (referenced by
`ring_message.hpp:220-223` and never written), the sender, the peer
handler that grants the intent into `FkIntentTable`, enrolment riding the
round, and the park/resume. The resume's shape is the open design
question: the engine's `pending_*` records all *finish* work, where a
foreign FK must **re-enter the statement** with verdicts in hand — the
candidate being a pending record carrying the statement line plus a member
the extraction pass consults first, so the second pass resolves everything
from held state and sends nothing.

### AH-T1 — what the hoist ran into

**Two shapes could have changed an answer. Both are handled, and each has
a cell that was proved to fail with its guard removed.**

1. **An UPDATE matching zero rows.** Today's check never runs when no row
   qualifies, so `UPDATE trades SET account_id = 99 WHERE qty = 12345`
   answers `UPDATED 0` even though 99 is no parent. Hoisting the *verdict*
   would turn that into an `FK_VIOLATION` no version of this engine has
   reported. So the hoist moves the **descent** and not the failure: the
   parent is resolved once at the fork, and the verdict is **applied per
   matched row**. Proved by applying it at resolve time instead — the cell
   then reports `ERR FK_VIOLATION … row id=99` where it must say
   `UPDATED 0`.

   This is the sharper reading of AH-R1 than the ruling's own words: *"the
   statement then runs synchronously against the intents it holds"* —
   holding an intent is not the same as acting on it, and a statement that
   touches nothing must use none of them.

2. **A self-referencing foreign key.** Row 2's parent can be row 1, written
   by the same statement, so a verdict taken before any row work says "no
   such parent" where the per-row check says pass.
   `ResolveForeignKeyParents` skips `fk.rel_oid == child.oid`, and the
   carve-out **costs AH nothing by construction**: one relation is one
   `owner_core`, so a self-referencing foreign key can never be foreign and
   its descent never needs to cross. AH-R1's rule — nothing in an open
   `WriteScope` starts a ring round trip — is intact.

   **And it is unreachable today**, which the survey did not say and the
   cell now pins: `REFERENCES nodes` inside `CREATE TABLE nodes` cannot
   resolve, the parent not existing yet, and nothing else declares a
   foreign key. The cell asserts the refusal, so the day self-reference
   becomes declarable it fails and points at a carve-out already waiting
   rather than at a wrong answer nobody was looking for.

**What moved that a user can see.** `SHOW ACCESS` now reports **one**
`Lookup` on the parent where a twenty-row insert against one parent
reported twenty (`uses=20` → `uses=1`, proved by disabling the dedup). The
counter moved because the work did — this is AH-R2's deduplication
arriving one task earlier than the crossing that motivated it, and it is
the answer to `fk_check.hpp`'s recorded regret that "batch-inserting N
children of one parent pays N descents where §2 predicts one". **§2's
probe-memo prediction is met, by a different mechanism than §2 named.**

**A refusal's shape is unchanged**: resolution is hoisted over every row,
the verdict is applied per row, so a bulk insert whose third row names a
missing parent still says `(row 3)`.

**A miss in the held set is `NotImplemented`, not `Corruption`** — nothing
on disk disagrees with anything; a statement shape reached the write path
that the extraction pass does not enumerate, which is the two-code rule's
"nobody built this yet". Unreachable today under F1.

**What AH-T1 owes and did not do:** H-AH1's *latency* half. The verdict
byte-identity is proved by the suite; the "zero within noise on the
colocated path" claim is **not measured** — `build-release`, interleaved
A/B, per AH's measurement discipline. It is owed before AH-T6 and is
stated here as unrun rather than assumed, the more so because the
mechanism above predicts the colocated path got *faster* on a batch, not
merely no slower.

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
