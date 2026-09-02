# Work order AJ — the reverse fan-out: a parent in a cross-owner foreign key can be deleted

Drafted 2026-09-02 by CLA against `640afdf` (`v2.7.0-102-g640afdf`),
superseding a same-day draft against `8e5a3af`. **Every claim below is
source-read at `640afdf` unless tagged measured.** The series letter
follows AI.

**Why this order exists.** Work order AH is closed and its hand-on list
is now four items (`known-gaps.md` §"Cross-owner foreign keys"): the
surviving-coordinator crash half, `strict` and concurrency, the
participant-coordinated release's cost (measured, closed as a finding),
and **the reverse fan-out** — the crossing's *one standing asymmetry*.
`fk_check.cpp:190` refuses `NotImplemented` when
`child.owner_core != options.core_id`: RESTRICT needs an authoritative
"no children" and the parent's core cannot see a child it does not own.
Fail-closed, not a wrong answer — and exactly the shape AE-2 names as this
version's work. AE-6's FK subject is not finished while it stands. AF-P5
(a namespace colocating parent and child) *avoids* the refusal; a foreign
key across namespaces meets it on the first DELETE.

**What the last four commits changed about this design.** The draft
against `8e5a3af` proposed a parent-side pending-delete set and left its
release to "the decide". `4d520de`..`640afdf` sharpened what a decide is:

- A core that answered a probe is an **intent holder, not a
  participant**, told so per target (`TxnDecideRequestPayload::intent_only`,
  `Session::IntentOnlyTargets()`, `session.hpp:353`); a holder that was
  sent an ordinary decide took the *anomaly* arm on every success path
  (C1, `4d520de`).
- **A participant that holds an intent coordinates its own release**, and
  that costs it **720×** its acknowledgement leg (4.4 µs → 3.1 ms, measured
  at `results-ah-t6-participant-release-cost-v2.7.0-101-ged47cfc.md`): it
  stops being a participant that acks cheaply and becomes a coordinator
  with an unconditional decision-durability wait. Invisible under the
  shipped class, +53.8% under `relaxed`.
- The autocommit decide is sent **before** the coordinator's own commit
  record is durable (§2b's last paragraph).

The consequence for AJ is a simplification the earlier draft did not
have: **the reverse fan-out enrols nobody.** The child's owner answers a
read and forgets it; the only state the reverse leaves is on the
*deleting* core, which is already the coordinator (or the shipped
participant that coordinates its own release), so its release is a local
clear on a decide that is already being sent. No new holder, no new
release leg, no new 720×.

**What is reused.** The dispatch fork's park (`HandleDelete`,
`command_dispatcher.cpp:9981`, has `HandleInsert`'s `pending_shipped`
branch), the probe wire (`kFkProbeRequest`/`Reply`,
`ring_message.hpp:234`; `FkProbeServer`/`FkProbeClient`,
`fk_probe_service.hpp:130`/`:186`), the local reverse check a child's
owner already runs for its own parents (`exec::CheckNoChildReferences`,
Cabin-first per F6), the decide sites (autocommit `:502`, explicit
`:9573`/`:9682`, refusal-release `:4020`), and AI-T3's leg instrument. AJ
adds one message direction, one coordinator-local table, and the fork
hoist for DELETE.

**Standing-rule note.** This order converts a refusal into a result.
AJ-R1..AJ-R4 are `[DECISION]`; nothing is built until they are ruled on.
AJ-R5..R7 are CLA proposals accepted by default.

## Background

### Why the reverse is not the forward with the arrows turned

The forward could be hoisted (AH-R1) because its parent set is
**enumerable before any row work** (AH-R3, resting on F1). A DELETE's set
is the rows its `WHERE` matches, and today the reverse check runs **per
qualifying row inside the walk** (`:10156`, from `DeleteInner`'s mark
lambda, `:10014`), where nothing can park (RD5's wall). So the reverse
has a question the forward never had: *which parent pks is this statement
about to delete, and can it know them at the fork?*

- **Bare pk equality** — one pk, known at the fork. `PkEqualityTarget`
  (`:7195`) is UPDATE's point-statement seam; whether `DeleteInner` calls
  it is not confirmed (AJ-T0). This is the target workload's shape (§3:
  parent deletes are rare, and by id when they happen).
- **Anything else** — known only by walking. Either a **collect pass**
  (walk read-only under the statement's snapshot, gather pks, probe, walk
  again to mark) or **keep the refusal** for the shape. AJ-R2 decides.

### The race, and why the forward's intent alone does not close it

The forward leaves a **reference intent** on the parent's owner so a
DELETE between a probe's answer and the child's commit meets busy
(`CheckNoChildrenBeforeDelete`, `:4240`, asks the intent table first).
The reverse needs the mirror closed, and the sequence that dangles is:

1. DELETE of parent `P` (owner p) fans out; child owner c answers **no
   children**.
2. A child INSERT on c referencing `P` probes p; `P` is present and not
   yet marked; p grants a reference intent.
3. The INSERT commits on c; its decide releases the intent on p.
4. The DELETE's per-row check on p finds no intent, holds a stale "no
   children", and marks `P`. Dangling, with RESTRICT reporting success.

**(a) Coordinator-local pending-delete set, registered first.** Before any
reverse probe is sent, p records `(P.oid, pk)` in a **pending-delete set**
on its own core, keyed by the deciding identity — `(p, ship id)` for the
deleting session, which is the key every decide on p already carries.
`FkProbeServer` consults the set before answering a forward probe: a
member answers **in-flight** — the verdict an uncommitted delete-mark
already produces (H-AH1's fourth fixture, `TXN_CONFLICT` retryable). Then
p checks its own intent table for the pks (busy if a child probed *before*
the registration), then fans out, then walks and marks. **The set is
cleared where p's decide is sent** — the autocommit decide at `:502`,
`PrepareAcrossOwners`/`CommitLocal`'s union at `:9573`/`:9682`, the
refusal-release at `:4020` — and, for a shipped DELETE, at the
participant's own `COMMIT`/`ROLLBACK` dispatch
(`shipped_statement_executor.cpp`, which already forks on
`has_intent_holders()`). After registration no new reference intent on `P`
can be granted, so step 2 is told in-flight and retries after the DELETE
decides. **One table, one side, one existing handler consults it, no ring
message releases it.**

**(b) Child-side deletion intent.** The reverse probe leaves "`P` is being
deleted" on c, consulted by c's extraction pass. Symmetric with the
forward — and it makes c a holder, which is exactly the thing `4d520de`
and `640afdf` just showed to cost a decide leg and, when c is itself a
participant, a 720× release. Rejected on that evidence, subject to AJ-R3.

### What the child owner answers, and from what

The reverse probe is answered by the code c already runs for a local
parent: `CheckNoChildReferences` with `options.core_id = c` — the Cabin if
the fk column has one (F6: the verified-empty set is the authoritative "no
children"), else the walk. Verdicts map onto the reply as onto the local
check today: no visible child → **clear**; a committed visible child →
**violation** (`kFkViolation`, terminal); a row with a foreign `trx_id` →
**busy** (F3). The read view is c's own current snapshot when it answers;
§4's one-MVCC rule is untouched. c records nothing and is enrolled in
nothing.

### What stays refused

- A split child (`fk_check.cpp:198`) — withdrawn by AE-4, kept as a
  refusal by AE-5.1.
- DELETE shapes AJ-R2 does not admit — the message names this order.
- A child owner that does not answer within `kFkProbeReplyDeadlineNs` —
  the fork refuses as the forward does (`SendForeignKeyProbes`, `:4040`,
  falls back to the refusal). Never a local-only answer (§1).

## Rulings

**AJ-R1 `[DECISION]` — Build the reverse fan-out in v2.8.0.** CLA's
proposal: **yes, sequenced after AF-T2..T5.** It is AE-6's FK subject's
last open item and AE-2's exact shape; AH built the wire, and the release
question that made the earlier draft heavier has been answered by
`640afdf` in AJ's favour. The counter-argument (AF-P5 avoids it; §3 says
parent deletes are rare) is a sequencing argument. If ruled *no*, this
file stays as the design and the row status says so.

**AJ-R2 `[DECISION]` — Which DELETE shapes cross.** (i) bare pk equality
only; other `WHERE`s keep `fk_check.cpp:190`'s refusal with a message
naming this order. (ii) (i) plus a collect pass for any other `WHERE`, at
a second read-only walk of the parent. CLA's proposal: **(i) now; (ii) as
AJ-T6, built only if a scenario benchmark needs it.** A pk IN-list, the
next shape, is F1's business first.

**AJ-R3 `[DECISION]` — Where the mirror state lives.** (a)
coordinator-local pending-delete set consulted by `FkProbeServer`; (b)
child-side deletion intent. CLA's proposal: **(a).** Quiet-wrong-answer
item — the wrong ordering is step 4 above — so not accepted by default.
The 720× finding is new evidence against (b) that the earlier draft did
not have.

**AJ-R4 `[DECISION]` — The set's lifetime under abort, restart and the
D4 window.** Cleared on commit and abort at the sites named; memory-
resident; empty after a restart. What a restart loses is answered by the
WAL: a covered delete-mark is either committed (row gone; forward probes
answer absent) or rolled back (row present; referenceable). The D4 window
§2b names — the autocommit decide sent before p's own commit record is
durable — is safe here in the direction that matters: if p clears the
set, answers a forward probe **absent** from the not-yet-durable mark, and
then dies, recovery restores `P` and the child that was refused wrote
nothing; no dangling reference in either outcome. CLA's proposal: **accept
as stated, pin it with AJ-T4's three kills.** `[DECISION]` because it is
the same class of statement AH-R5 was.

**AJ-R5 — The child's owner is not enrolled.** Not as a participant (it
wrote nothing), not as an intent holder (it holds nothing). No
`intent_only` target, no release leg, no entry in `Session::intent_holders_`
for the reverse. This is the design's whole advantage over (b) and is
stated as a ruling so a later "symmetry" refactor cannot undo it
silently. *Accepted by default.*

**AJ-R6 — Message shape.** A new pair `kFkReverseProbeRequest`/`Reply`
rather than a direction flag: the forward carries `(parent oid, pks)` and
answers exist/absent/in-flight; the reverse carries `(child oid, fk
column, parent pks)` and answers clear/violation/busy. Bounding and
deadline from the forward. *Accepted by default.*

**AJ-R7 — Contract home.** `foreign-keys.md` §3a rewritten from "a
refusal, not a fan-out" to the fan-out, keeping the refusal paragraph for
AJ-R2's remainder; §3's per-row check stays as the local half;
`known-gaps.md`'s "one standing asymmetry" bullet struck at AJ-T3.
*Accepted by default.*

## Hypotheses

- **H-AJ1** — `DELETE ... WHERE pk = k` on a parent whose child is on
  another core runs fork → register → intent check → fan-out → walk →
  mark → decide, byte-identical to the colocated case for four fixtures:
  no child (row deleted), committed child (`kFkViolation`, no mark),
  in-flight child on c (`TXN_CONFLICT` retryable), live reference intent on
  p (`TXN_CONFLICT` retryable). Autocommit and inside a transaction, and
  shipped (client on a third core).
- **H-AJ2** — The race in Background cannot be reproduced: a child INSERT
  probing `P` after registration meets in-flight; one that probed before
  holds an intent the DELETE meets. Pinned by an interleaving cell at the
  dispatcher, H-AH4's shape mirrored.
- **H-AJ3** — The reverse adds **no decide target and no release leg**:
  `decide_refusals() == 0`, `IntentOnlyTargets()` unchanged by a reverse
  round, and the participant-release leg of a shipped DELETE is the plain
  participant's 4.4 µs class, not the 3.1 ms class. Falsified by any of
  the three.
- **H-AJ4** — Cost is flat in child owners and paid once per statement
  (AH-T6: the slowest owner's reply, not the sum); a Cabin on the fk
  column makes c's answer O(1) against the walk's linear cost.
- **H-AJ5** — `cores = 1` byte-identical: the set is written only on the
  fan-out path, which a single core never enters (G2).

## Tasks, in dependency order

**AJ-T0 — Survey.** (1) Whether `DeleteInner` (`:10014`) has a pk fast
path or always walks. (2) `HandleDelete`'s fork: a `pending_probe` return
sharing `HandleInsert`'s resume (`:6548`). (3) The shipped DELETE: where
`ShippedStatementExecutor` forks and whether a reverse park fits the seam
that already forks on `has_intent_holders()`. (4) `FkProbeServer`'s
handler: where the pending-set consult precedes the existence read. (5)
The reverse handler on c: minting a read view with no session (the
forward's handler is the model). Output: a table in row status. Gate:
AJ-R1.

**AJ-T1 — The pending-delete set and the forward probe's consult.** One
core-local table beside `FkIntentTable`, keyed `(oid, pk)` → deciding
identity; `FkProbeServer` answers in-flight for a member; cleared at the
four decide sites and the shipped participant's own dispatch. Tests: the
consult; clear on commit; clear on abort; clear on the refusal path;
`cores = 1` untouched. Gate: AJ-T0, AJ-R3.

**AJ-T2 — The reverse pair and c's server half.** `kFkReverseProbeRequest`
/`Reply`; c's handler mints a read view and calls `CheckNoChildReferences`
with its own `core_id`, Cabin-first; one verdict per pk plus the first
non-clear message. Gate: AJ-T1, AJ-R6.

**AJ-T3 — The fork hoist for DELETE.** For AJ-R2's shapes: resolve child
owners from `fkeys_in`; register; check the intent table for the pks; one
reverse probe per foreign child owner; park; resume; `DeleteInner` against
held verdicts, the per-row check answering from them for foreign children
and locally for own children. Refusal message for the remainder. Rewrite
§3a per AJ-R7. Tests: H-AJ1's fixtures × three paths, H-AJ2's interleaving,
H-AJ3's three assertions, H-AJ5's byte-identity. Gate: AJ-T2, AJ-R2, AJ-R5.

**AJ-T4 — The crash cells.** Kill p after registration and before the
mark; kill p after the mark and before the decide (the D4 window); kill c
after answering clear and before p decides. After restart: no dangling
reference, no in-doubt residue, the set empty, the WAL's answer matching
what it covered. `bench/fk_intent_crash_probe.py`'s shape. Gate: AJ-T3,
AJ-R4.

**AJ-T5 — Measurement.** Arms: colocated DELETE; cross-owner, one child
owner; two child owners; shipped DELETE (client on a third core); Cabin on
the fk column versus walk at 1k/10k/100k child rows; `group` and `relaxed`,
because `640afdf` showed which class exposes a release leg. AI-T3's leg
instrument extended with the reverse round. `bench/v2.8.0/`,
`git describe --tags`, interleaved, ≥3 runs. Gate: AJ-T3.

**AJ-T6 — Collect pass.** Only if AJ-R2 rules (ii) or a scenario benchmark
demands it. Not sequenced.

## Measurement discipline

`build-release` only; results under `bench/v2.8.0/`; interleaved A/B;
≥3 runs with the spread stated; a loaded host is stated and nothing rests
above p90 on it (the `640afdf` file's precedent); claims tagged measured
(with invocation) or source-read (path:line and commit).

## Sequencing

After AF-T2..T5 (`ratification-af-namespace.md` §AF-7; no new order
needed there). AJ touches `command_dispatcher.cpp`'s DELETE path,
`fk_probe_service.*`, and the four decide sites; AF touches placement and
DDL. Separate worktrees.

---

## AJ-T0 — the survey, run ahead of its gate

**Run 2026-09-02 on `worktree-workorder-aj` at `640afdf`, source-read.**
AJ-T0's stated gate is AJ-R1, and it was run before that ruling
deliberately: every answer below is decision-independent, and three of
them are premises AJ-R2 and AJ-R3 rest on. Nothing was built.

| # | question | answer at `640afdf` | consequence |
|---|---|---|---|
| 1 | Does `DeleteInner` have a pk fast path? | **Yes**, `command_dispatcher.cpp:10244`: `PkEqualityTarget(ta, stmt.where)` then `LocateByPk`, falling through to `VisitRelation` when the locator cannot resolve. `PkEqualityTarget` (`:7289`) is a pure function of the WHERE and the schema — one `kCompareValue` `kEq` conjunct on column 0, non-negative int. | AJ-R2(i) is implementable at the fork with **no new predicate analysis**. The fork calls `PkEqualityTarget` directly. |
| 1a | Does the fork already hold the pk? | **No, not in the shipped configuration.** `WriteTargetCore` (`:7329`) fills `*target_id` only after the `access.ranges.empty()` early return, so with spreading off — every relation until one asks — `target_id` at `:10048` stays unset. | The hoist must call `PkEqualityTarget` itself; it must not read `target_id`. Cheap (no page access), so this costs nothing. |
| 2 | Does `HandleDelete`'s fork have INSERT's park? | **The `pending_shipped` branch, yes** (`:9992`, which the file's own comment labels "HandleInsert's branch, for its reason"). The **fk park** is not there: the `pending_fk_probe` return exists only on INSERT (`:6552`, `:6639`) and UPDATE (`:9090`). The park machinery itself is in `DispatchAsync` (`:366`–`:540`) and is statement-agnostic. | AJ-T3 adds `if (out_probe.pending_fk_probe.has_value()) return out_probe;` to `DeleteInner` and one propagation in `HandleDelete`. The wait, collect, resume and decide blocks are reused unmodified. |
| 2a | Can a DELETE need two parks? | **No.** `DeleteInner` mints `check_view` only for `fkeys_in` (`:10078`); a DELETE runs no forward check, so `fkeys_out` never produces a probe on this path. | `DispatchAsync:482`'s `NotImplemented` guard ("one park per dispatch is all this path has") is not reachable by AJ, and AJ must not make it reachable. |
| 3 | Where does the shipped DELETE fork, and does it fork on `has_intent_holders()`? | **The premise is wrong.** `has_intent_holders` appears in exactly two files — `session.hpp` and `command_dispatcher.cpp` — and **nowhere in `shipped_statement_executor.cpp`**. A shipped statement runs through `dispatcher_.DispatchAsync(state->text, state->session, &state->out)` (`shipped_statement_executor.cpp:195`) on the owner core, in autocommit (`:276` rolls back and refuses one that opens a transaction). | A shipped DELETE **already reaches the park and the release** through `DispatchAsync`'s own autocommit decide block (`:502`) on the owner's session. **AJ-T1's fourth clear site does not exist and must not be invented**; the shipped case is covered by the same three sites as the local one. The prepare-relevance predicate at `shipped_statement_executor.cpp:706` (`wrote_nothing`) is a *different* question and is unaffected. |
| 4 | Where does the pending-set consult go? | Inside `FkProbeServer::OnRequest`'s per-parent loop (`fk_probe_service.cpp:71`–`:122`), **between the owner-core re-check (`:88`) and `exec::CheckParentPresent` (`:96`)** — necessarily before `intents_.Add` (`:110`), which is the grant AJ-R3(a) must prevent. | One consult, one site, ahead of the existence read. Confirms AJ-R3(a)'s "one existing handler consults it". |
| 4a | Is there an in-flight verdict to answer with? | **Yes, and it needs no new enum value.** `exec::FkVerdict` is `{kPass, kViolation, kBusy}` (`fk_check.hpp:65`); `kBusy` is already "another transaction is writing the row the check depends on… reported as `kTxnConflict`". | The reverse's mirror answer is `kBusy`. No wire-format change on the forward pair. |
| 5 | How does c mint a read view with no session? | `txn_->MintReadView(/*writer=*/0)` (`fk_probe_service.cpp:57`), with the reason stated at the site: the check reads the **owner's own latest state**, never a view carried on the wire. | The reverse handler copies it verbatim. §4's one-MVCC rule is untouched, as this order claims. |

### Three findings the survey produced that change the tasks

**S1 — the pending-delete set's key needs a minted `ship_id`, or it
repeats a defect AI already fixed once.** AJ-R3(a) keys the set on
`(p, ship id)`. A purely local autocommit DELETE has **never shipped and
never probed forward**, so its `Session::ship_id()` is still 0 — and
`(p, 0)` is shared by every such session on the core. That is exactly the
collision `SendForeignKeyProbes` mints against (`command_dispatcher.cpp:4059`,
whose comment records that a session probing under id 0 "made every
un-shipped session share the holder key `(core, 0)`, so one decide would
release another session's intents"). **AJ-T1 must mint the identity on the
reverse round by the same rule**, before registering. Without it, one
session's decide clears another's pending-delete entries and the race in
Background reopens.

**S2 — reuse `PendingFkProbe` and the existing waiter map; add kinds, not
a second park.** `FkProbeClient::waiting_` holds `FkProbeOutcome`
(`arrived`, `status`, `verdicts`) keyed by `request_id`
(`fk_probe_service.cpp:206`), and nothing in it is forward-specific. A
second `RegisterMessageHandler` for `kFkReverseProbeReply` writing into the
same map makes `DispatchAsync`'s settle-and-collect block work unchanged;
only the collection branch at `:396`–`:420` needs a reverse arm that fills
a `resumed_fk_reverse_verdicts_` member beside `resumed_fk_verdicts_`.
This keeps AJ-R6's "a new pair rather than a direction flag" while adding
no second park field and no second deadline path.

**S3 — the reverse request's cap is derived, and the derivation differs.**
`kFkProbeMaxParents` is 62, derived as
`(kCoreRingPayloadBytes - kFkProbeRequestHeadBytes) / (2 * sizeof(uint64_t))`
with a `static_assert` and a comment recording that a first draft's chosen
64 produced a 1048-byte payload the ring silently refuses. A reverse entry
is `(child_oid, column_no, parent_pk)`; as three parallel arrays that is 17
bytes, giving `(1024 - 24) / 17 = 58`. **AJ-T2 derives its own cap and
`static_assert`s it — it does not reuse 62.** Under AJ-R2(i) the reverse's
cardinality is (child relations of this parent on that owner) × 1 pk, so
58 is ample and the refusal is unreachable in practice; it is still
fail-closed and names this order, as the forward's does.

## Row status (CLA, appended as rows land)

| row | status |
|---|---|
| AJ-R1..R4 | awaiting the operator's word |
| AJ-T0 | **run 2026-09-02 on `worktree-workorder-aj` at `640afdf`**, ahead of its gate because every answer is decision-independent. Table above; three findings (S1, S2, S3) amend AJ-T1, AJ-T2 and AJ-T3's premises. One premise in this order's own text is **corrected**: `shipped_statement_executor.cpp` does not fork on `has_intent_holders()` and has no clear site to add |
| AJ-T1..T5 | not started |
| AJ-T6 | not sequenced |
