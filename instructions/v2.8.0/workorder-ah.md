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
| AH-T2 | **Second slice built 2026-09-01** — the wire: `FkProbeRequestPayload` / `FkProbeReplyPayload`, `FkProbeServer` (resolve, grant, reply), `FkProbeClient` (send, wait, read), both halves on every core, and the intent release wired to the 2PC decide. **The park/resume is not built**, so the fork's refusal still stands and nothing sends yet. Seven cells in `fk_probe_service_test.cpp`. Full suite 3147/3147 green |
| AH-T2 | **Third slice built 2026-09-01** — the park, the resume, and enrolment. The dispatcher now **sends** where it refused. **Not exercisable end to end until AH-T4**, and that is AH-R6's mark taking effect rather than a gap; see below. Full suite 3147/3147 green |
| AH-T3 | **Built 2026-09-01 with AH-T4**, because it is AH-T4's gate for a correctness reason and not an ordering one: lifting F5 without it leaves a parent `DELETE` blind to foreign intents. The parent-side check consults `FkIntentTable` before anything local and answers busy |
| AH-T4 | **Built 2026-09-01.** `CheckForeignKeyColocation` admits. F5 amended, §3a added, `known-gaps.md` opened. Full suite 3149/3149 green |
| AH-T5 | **Blocked 2026-09-01 by a gate AH did not know about** — the crash point is placed and the probe is written, and neither can run. §AH-T5 below. Its own gate (XG3's process-kill half) **is** repaid: `bench/v2.8.0/results-xg3-answer-edge-kill-v2.7.0-86-g6b18f69.md`, 3/3 across three passes |
| AH-T6 | not started |

### AH-T5 — the gate that makes the whole crossing unreachable

**XG3's half is repaid.** The AH-T5 gate the operator set on
2026-09-01 (item 4(b)) is discharged: the three answer-edge kill cells
run and pass, three passes each, in `bench/shipped_answer_edge_kill_probe.py`.
Five harness findings and two engine ones are written up with them.

**Then AH-T5's own cell hit a wall, and the wall is the finding.**
`participant.fk_intent_granted_preprepare` is placed
(`fk_probe_service.cpp`, right after the grant) and
`bench/fk_intent_crash_probe.py` is written. The probe cannot get as far
as arming it, because the setup is refused:

> `ERR NOT_IMPLEMENTED retryable=0 an FK-linked relation cannot take
> writes on core 2: validation reads the linked relation, which this core
> may not fault (workplan-peer-writer.md §4)`

That is `command_dispatcher.cpp:5414`'s **peer-writer funding gate**:
`funded_shape` requires `fkeys_out.empty() && fkeys_in.empty()`, so a
relation with *any* foreign key — either direction — takes no writes on
any core but 0.

**What it means for AH, stated plainly.** AH-T4 converted the
*declaration* refusal; a cross-owner `REFERENCES` is admitted. But a
peer-owned relation on either end of it still cannot be **written**, so
the forward check never runs, the park is never entered, and the probe
never sends. **The crossing AH built is unreachable in a running
instance**, behind a gate AH's own survey did not name.

**And its stated reason is the one AH answered.** The gate's message —
*"validation reads the linked relation, which this core may not fault"* —
is exactly the defect §2a exists to remove: the forward check no longer
reads the linked relation locally, it probes the owner. The gate is a
year-appropriate refusal that AH made obsolete without noticing it
existed.

**This is what the missing end-to-end cell would have caught at AH-T2**,
and it is the cost of that debt landing rather than being paid. The
`known-gaps.md` entry AH-T4 opened is amended to say so.

**Not lifted here, and the reason is not caution but ownership.** The
arm sits beside two siblings whose refusals are still live — the cabined
arm and `CannotEnforce`'s, the latter carrying a *measured* Finding 2
(`bench/v2.2.0/results-shipping-part-a-*`: a shipped write put a second
row in a group under `CHECK COUNT(*) <= 1`). Narrowing one arm of a
funding gate is a change to what a peer core will admit, it belongs to
`workplan-peer-writer.md` §4, and the argument for it — the forward check
funds itself now, the reverse refuses by name (§3a) — should be ratified
rather than assumed by the order that benefits from it.

### AH-T3 + AH-T4 — the conversion, and the two things it made live

**AH-T3 was built with AH-T4 rather than before it**, because AH-T4's
gate on it is a correctness statement: F5 lifted without the intent check
is a parent `DELETE` that cannot see the foreign transaction relying on
the row it removes. The two are one change.

**The intent check comes first, ahead of everything local.** A live
reference intent is evidence no local walk can produce — the child row it
protects is on another core. Meeting one answers **busy**
(`TxnConflict`, retryable, one code wide per AH-R4), because the foreign
transaction has not committed and the answer depends on how it ends. The
self-exclusion `HeldByAnotherThan` offers is **structurally vacuous on
this table** — an intent lands here only from a core whose extraction
pass found this parent foreign, and a statement never defers a parent its
own core owns — and is asked through the same predicate anyway, so the
day a local grant becomes possible the site already asks the right
question.

**The second live thing is the one the survey predicted.**
`workplan-auxiliaries-under-split.md` §3.1 wrote it down and said the day
F5 relaxes both directions are owed: the reverse check's scope guard was
keyed on `!child.ranges.empty()`, so an **unsplit child owned by another
core** was walked at `desc_page_id` with no scope question asked —
correct only because of the refusal AH-T4 has now lifted. The owner
direction is paid: `child.owner_core != core_id` refuses.

**What that costs is an asymmetry, and it is stated rather than hidden**:
the forward direction crosses, and a **parent in a cross-owner foreign
key cannot be deleted**. RESTRICT degrades to refusing the delete, which
is fail-closed and not a wrong answer; the fan-out that would replace it
(one boolean probe per child owner) is not built, and colocating the pair
— a namespace, AF-P5 — avoids it entirely. That trade is what makes
F5-as-advice honest: colocation is not a style preference, it buys back a
capability.

**`CheckForeignKeyColocation` keeps its parameters and its callers.** It
returns OK and says why in its own body, so the signature — and the two
doors that call it — are unchanged the day a reason to refuse returns.

**Owed, and named in `known-gaps.md` rather than left implicit**: the
dispatcher's park still has no end-to-end cell. AH-T2 handed that debt to
this task on the grounds that the statements were not declarable; they
are declarable now, and the debt moves to **AH-T6**, which needs the same
two-core fixture for its measurement and should build it once.

### AH-T2, third slice — the park that resumes by re-entering

**Built.** `SendForeignKeyProbes` replaces the refusal at all three sites:
one `kFkProbeRequest` per foreign owner, the request ids and their groups
carried on a new `DispatchOutcome::pending_fk_probe`, and the statement
returned unrun. `DispatchAsync` parks on **one** predicate over every
owner — k sequential parks would serialise on whichever owner the loop
named first, which is `pending_remote`'s rule (RD7) and its reason —
collects the verdicts into `resumed_fk_verdicts_`, closes every waiter,
and **re-dispatches the line**. The extraction pass consults those
verdicts first, so the second pass resolves everything from held state,
groups nothing foreign, and sends nothing: a plain local statement.

**Parking is not failing, and that took a specific primitive.** The write
scope opened before the parse has to close without committing *and*
without the failure arm that poisons an explicit transaction.
`AbandonWriteForShipping` is exactly that, and it already existed for the
shipped statement — the foreign-key arm sits beside it in `HandleInsert`
and `HandleUpdate` and says the same thing. Without it a parked statement
inside a transaction would have poisoned the session for having waited.

**Enrolment lands with the send**, after it and never before: a
participant recorded for a request that never left would be prepared for
a transaction it holds nothing of — the shipping enrolment's rule,
quoted at the site. Autocommit enrols nobody, which is right and is also
what makes the intent safe there: the decide this core's own commit sends
is what releases it. **The second-slice leak is therefore closed.**

**A deadline is a refusal, not a verdict.** A waiter settled without
having arrived answers `TxnConflict` — retryable — and clears the
verdicts before returning, so no row can be written on the strength of an
answer nobody gave. The synchronous `Dispatch()` closes its waiters and
refuses the same way: with no reactor the park cannot complete.

`transaction_id` travels as 0 on the wire. The intent's holder is
**(coordinator core, session)**, which is what a decide releases by, and
an id minted on this core names nothing on the owner's; the field is
there for a reader of a captured frame, not for the protocol.

**What this slice does not have, stated plainly: an end-to-end cell.**
The wire is proved by `fk_probe_service_test` (seven cells, both halves);
the dispatcher plumbing is proved by compilation and by the suite not
regressing, and by nothing else. It cannot be more than that yet, and the
reason is AH-R6's own mark: `CheckForeignKeyColocation` refuses a
cross-owner declaration until AH-T4, so **no statement this engine admits
can reach the park**. The `kRotate` second-catalog trick that stages a
relation off core 0 (second slice) cannot help, because the FK
*declaration* is what refuses, not the write.

So **AH-T4 inherits AH-T2's acceptance tests**, and this is a real
consequence of ordering the conversion last rather than an oversight:
the one-owner and two-owner fixtures, the probe-reply negative (parent
absent → `kFkViolation` before any row is written) and the in-flight
parent (→ `TXN_CONFLICT`) that AH-T2's task text asks for are all
statements that must first be declarable. They land with the arm that
makes them declarable.

### AH-T2, second slice — the wire, and the cap that had to be derived

**Built.** `fk_probe_service.hpp/cpp`, on `index_build_service`'s shape:
the owner's `OnRequest` bounds the count, refuses any parent that is not
its own, resolves each against a view it mints itself (§4 asks for the
*parent owner's* now, so carrying a view would carry the wrong core's idea
of who is live), **grants an intent per pass and only per pass**, and
replies with verdicts matched **positionally**. The child's `Request`
opens a waiter under a deadline and sends; `Settled` / `Find` / `Close`
are `IndexBuildClient`'s and for its reasons.

**Both halves on every core**, unlike the index build's owner-only server:
a relation is a foreign parent on one statement and a child on the next,
and core 0 is not special here the way it is for DDL.

**The intent's release rides the decide** (AH-R5), wired in `CoreRuntime`'s
`kTxnDecideRequest` handler rather than inside `Txn2pcServer` — which would
take a 2PC dependency on the FK for a reason 2PC has none of. It runs
**after** `OnDecide`, because the decision is what the intent was holding
the parent still for, and it is idempotent both sides, which a resendable
decide requires.

**The cap had to be derived, and finding that out cost a whole red
suite.** `kFkProbeMaxParents` was first written as a flat 64, producing a
1048-byte request against a 1024-byte ring payload
(`sched::kCoreRingPayloadBytes`). The ring refuses an oversized send, so
every cell failed on a reply that never came — the failure of a chosen
constant, seen from the outside. It is now
`(kCoreRingPayloadBytes - head) / 16` = **62**, with a `static_assert` on
the payload as the real statement. A statement naming more distinct
parents on one owner is **refused, not chunked**: chunking is protocol for
a shape nobody has produced, and if AH-T6 measures one past the cap the
refusal converts into it.

**Two fixture facts worth keeping**, because both took a red run to find:

- The owner is ring core **0** and the child ring core 1, not the reverse.
  `AssignOwnerCore` puts every relation on its creator, so core 0 is who
  must answer for the parent; attached the other way the owner replied to
  itself and the ring dropped it.
- The one way this engine can currently produce a relation a given core
  does **not** own is a second `Catalog` over the same store under
  `kRotate` with `core_count = 2`. That is what the stale-owner cell uses,
  and it is the closest thing to a migration this tree can stage.

**What AH-T2 still owes — and it is the whole reason the refusal stays**:
the park/resume. The extraction pass groups foreign parents and
`RefuseUnsentForeignKeyProbes` still turns the statement away; nothing
calls `FkProbeClient::Request` from the dispatcher yet. The open design
question is unchanged and now sharper for having built the rest: every
`pending_*` record in `DispatchOutcome` **finishes** work, where a foreign
FK must **re-enter the statement** with verdicts in hand. The candidate
stands — a pending record carrying the statement line plus a member the
extraction pass consults first, so the second pass resolves everything
from held state and sends nothing — and it is the third slice.

Also owed and not started: **enrolment**. The probe round is specified to
carry the participant's enrolment (AH-R2, RR1's enrol-on-first-contact),
and this slice sends `session_id` and `transaction_id` on the request
without yet enrolling the owner as a 2PC participant. Until that lands, an
intent granted here is released only if the coordinator happens to send
that owner a decide for another reason — which is a **leak**, and it is
why the fork's refusal may not be lifted before the third slice.

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
