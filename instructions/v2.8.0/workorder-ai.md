# Work order AI — the peer-writer funding gate's FK arm, narrowed by ratification

Drafted 2026-09-02 by CLA against `546ddc8` (`v2.7.0-88-g546ddc8`).
**Every claim below is source-read at that commit unless tagged
measured.** The series letter follows AH; AG is skipped for the reason
`workorder-ah.md` gives (AG-M is `aggregate.md`'s).

**Why this order exists.** AH-T5 (`workorder-ah.md` §AH-T5) found that
the crossing AH built is unreachable in a running instance: a relation
with a foreign key in *either* direction takes no writes on any core but
0, because `CheckWriteAffinity`'s peer branch requires
`fkeys_out.empty() && fkeys_in.empty()`
(`src/server/command_dispatcher.cpp:5413`). AH declined to narrow that
arm itself because it sits in a **funding gate** — a change to what a peer
core admits — which `workplan-peer-writer.md` §4 owns, beside two sibling
arms whose refusals are still live. This order is that ratification,
filed as a work order so the operator's word lands as rulings and the
code lands as tasks. It is the gate on AH-T5, AH-T6, and the end-to-end
cross-owner INSERT cell that has been carried from AH-T2 to AH-T4 to
AH-T6 (`docs/inflight/known-gaps.md`, the AH-T4 entry).

**Standing-rule note.** This order narrows a refusal. Under the SB
rationale, an item that converts a refusal into a result needs explicit
operator judgment, not default acceptance — so AI-R1..AI-R4 are marked
`[DECISION]` and nothing below is built until they are ruled on.

## Background

### The gate as it stands

`CheckWriteAffinity`, peer branch (`command_dispatcher.cpp:5340-5450`):
after `IndexBuildPending`, `funded_shape` is

```
fkeys_out.empty() && fkeys_in.empty() && !any_cabin && !enforcer_.CannotEnforce(oid)
```

and the refusal branches, in order, are the FK arm (`:5416`, message
*"an FK-linked relation cannot take writes on core N: validation reads
the linked relation, which this core may not fault
(workplan-peer-writer.md §4)"*), the cabined arm (`:5423`, *"whether a
Bound Cabin's entry pages follow the grant is unverified"*), the
`CannotEnforce` arm (`:5430`, PW1c-6c's, backed by Finding 2), and the
whitelist tail (`:5439`, §8). Every arm is `NotImplemented`, none poisons
the session, and the backstop under all of them is the store's every-build
`MayWrite`.

The arm's history is the pattern this order continues: btree lifted
2026-08-24 (PW2-4), indexed lifted 2026-08-25 (PW1c-6b-4), key-mode
lifted 2026-08-25, assertion narrowed to `CannotEnforce` 2026-08-26
(PW1c-6c). Each lift was ratified in `workplan-peer-writer.md` and each
left a narrower predicate behind. The FK arm is the one that was never
revisited, and its stated reason has since been removed.

### What the arm's reason was, and what removed it

The reason is the message: the forward check *read the linked relation
locally*, and a peer may not fault a relation it does not own. That was
true. `CheckForeignKeyOnWrite` descended the parent at its
`desc_page_id` — correct only because `CheckForeignKeyColocation` forced
parent and child onto one core (`fk_check.cpp:171-179`'s own comment).

AH removed it in both directions:

- **Forward (child write, `fkeys_out`)** — `ResolveForeignKeyParents`
  (`command_dispatcher.cpp:3753`) resolves each parent's owner; a parent
  on another core is `Defer`red (`:3813-3814`) into `FkParentVerdicts`
  and served by one `FkProbeClient::Request` per distinct owner
  (`:3853`) from the dispatch fork's park. The child core faults nothing
  of the parent. A parent on *this* core is walked locally, and its pages
  are this core's own. `docs/spec/foreign-keys.md` §2a.
- **Reverse (parent DELETE, `fkeys_in`)** — `fk_check.cpp:190` refuses
  by name when `child.owner_core != options.core_id`: *"cannot see its
  rows, but a foreign key's reverse check must see every child row"*.
  A child on this core is walked locally on this core's pages. A parent
  INSERT or non-pk UPDATE consults `FkIntentTable` (`:4020`) and reads no
  child at all; pk UPDATE is Unsupported by K2. `foreign-keys.md` §3a.

So with the FK arm lifted, **no path on a peer faults a linked relation
it does not own**: the forward direction probes, the reverse direction
either walks its own pages or refuses by name. The arm is a refusal that
guards against a read that no longer happens.

### What the arm still does today, stated so the lift is honest

1. It is the **only** thing that refuses a *shipped* write to an
   FK-linked peer relation. `CoreRuntimeTest.AShippedWriteToAnFkLinkedPeerRelationIsRefusedByTheOwnersShapeGate`
   (`tests/core_runtime_test.cpp:6465`, group A5) pins it, and the
   property A5 is really testing — *shipping must not route around a
   gate* — is independent of which arm is used to prove it.
2. It makes `fk_intent_test.cpp` and `fk_probe_service_test.cpp` (seven
   cells) the *only* evidence for the wire. No test drives a real
   cross-owner INSERT through fork → park → probe → resume, because none
   can (the known-gaps entry).
3. It hides the funding question for the **KWP load path** (AH-R7's
   load-path refusal) and for the other `fkeys_out`/`fkeys_in` sites
   in the dispatcher (`:6254`, `:6342`, `:6376`, `:6619`, `:8752` on
   `fkeys_out`; `:9787`, `:9863` on `fkeys_in`). Whether any of these
   descends a foreign relation on a peer has not been surveyed, because
   the arm made the question moot. **AI-T0 surveys them.**

### Why the sibling arms are not this order's

- **`CannotEnforce`** carries a measured defect (Finding 2,
  `bench/v2.2.0/results-shipping-part-a-v2.2.0-11-g925f483.md`: a shipped
  write put a second row in a group under `CHECK COUNT(*) <= 1`). Its
  predicate is already the narrow one PW1c-6c derived. Nothing in AH
  touches it. **Stays.**
- **The cabined arm** refuses on an unverified co-location — whether a
  Bound Cabin's entry pages follow the CC7 grant
  (`workplan-peer-writer.md` §4, the last bullet). That is a Cabin
  question and AE-6 lists it under Cabin, not FK. Lifting it here would
  be a second ratification riding on the first. **Stays, unless AI-R2
  says otherwise.**

## Rulings — the operator's word is required

**AI-R1 `[DECISION]` — Does the FK arm narrow at all?** CLA's proposal:
**yes, both halves** (`fkeys_out` and `fkeys_in`), because the refusal's
stated reason is gone in both directions and each direction has its own
fail-closed guard behind the gate (§2a's fork refusal when a probe cannot
be sent; `fk_check.cpp:190` for the reverse). The alternative that keeps
`fkeys_in` refusing is available but buys nothing: a peer-owned parent's
DELETE with a foreign child is *already* refused by name one layer down,
and its INSERT reads no child.

**AI-R2 `[DECISION]` — Scope: the FK arm alone, or the cabined arm with
it?** CLA's proposal: **FK arm alone.** The cabined arm's question is
unverified rather than answered, and this order has no evidence to offer
on it. If the operator wants both lifted in one pass, AI-T0 gains a Cabin
survey and AI-T1 a second predicate; the order is then AE-6's Cabin work
as well as its FK work and should say so in its title.

**AI-R3 `[DECISION]` — What A5's test becomes.** The property A5 proves
(a shipped write does not route around the owner's shape gate) must
survive; the FK shape can no longer be the vehicle. CLA's proposal:
**re-point A5 to the cabined arm** (`CREATE CABIN` on the peer relation,
shipped INSERT refused by the cabined message, byte-identical to the
owner's own line), and add the FK shape's *admitting* twin beside it as
the end-to-end cell (AI-T2). If the operator prefers, A5 can be re-pointed
to `CannotEnforce` instead, but that requires a pre-PW1c-6c fixture
(a core-0-built cabin), which the rig does not have today.

**AI-R4 `[DECISION]` — Does the e2e cross-owner INSERT cell land here or
stay with AH-T6?** CLA's proposal: **here, as AI-T2, and it is this
order's acceptance test.** The cell has been deferred three times; the
last deferral is what hid this gate. An order that narrows a funding gate
without driving a write through what it opened would be the same debt
again.

**AI-R5 — Ratification home.** `workplan-peer-writer.md` §4's FK bullet
is amended to record the lift with AI's argument, and §8's whitelist
comment lists the shapes actually refused (cabined, assertion-covered,
EXPLICIT — no longer FK-linked). The PW1c-6b-4 precedent: the four stale
contract claims a lift outdates are corrected in place, in the same
commit. *CLA proposal, accepted by default.*

**AI-R6 — The message that replaces the arm's, where a refusal still
fires.** The old text (*"validation reads the linked relation"*) is
retired everywhere, including from any site AI-T0 finds still refusing.
A refusal that survives names its own reason (§3a's reverse, or the KWP
load path's), never the retired one. *CLA proposal, accepted by default.*

## Hypotheses

- **H-AI1** — With the FK arm lifted and nothing else changed, a
  cross-owner INSERT (child on a peer, parent on core 0) runs fork →
  park → probe → resume → row written, and its verdict is byte-identical
  to the colocated case for the four fixtures H-AH1 named (present
  parent, absent parent → `kFkViolation` with no row written, retired
  parent, in-flight parent → `TXN_CONFLICT` retryable). Falsified by any
  cell where the write reaches a `MayWrite` refusal or a page fault on a
  foreign relation.
- **H-AI2** — The lift changes nothing at `cores = 1`: the peer branch is
  `catalog_read_only_`-gated, and a single-core instance never enters it.
  Falsified by any byte difference in the single-core suite's output.
- **H-AI3** — No `fkeys_out`/`fkeys_in` site outside `CheckWriteAffinity`
  descends a foreign relation on a peer. Falsified by AI-T0's survey; if
  falsified, the site gets its own named refusal (AI-R6) and the lift
  still proceeds, because that site is not what the arm guarded.
- **H-AI4** — The shipped-write cell stays refused for the cabined shape
  with the owner's message byte-for-byte across the ring, which is the
  A5 property with a different vehicle.

## Tasks, in dependency order

**AI-T0 — the survey of the seven other sites.** For each
`fkeys_out`/`fkeys_in` read in `command_dispatcher.cpp` outside the gate
(`:6254`, `:6342`, `:6376`, `:6619`, `:8752`, `:9787`, `:9863`) and for
the KWP load path's FK seam (AH-R7), state whether the path, on a peer,
faults any page of a relation this core does not own. The answer per
site is `local-only` / `probes` / `refuses by name` / `descends foreign`.
A `descends foreign` finding is H-AI3's falsification and gets a refusal
of its own in AI-T1. Output: a table in this file's row status. Gate:
AI-R1 ruled.

**AI-T1 — the lift.** Drop the FK terms from `funded_shape`, delete the
FK refusal branch, correct the contract claims PW1c-6b-4's precedent
names (the DDL refused-shapes list, the whitelist opener, the
exhaustiveness comment, `SetIndexBuilds`'s header if it lists shapes),
amend `workplan-peer-writer.md` §4 and §8 per AI-R5, and add any refusal
AI-T0 owes. Re-point A5 per AI-R3. Tests: the full suite green; the
`cores = 1` byte-identity check (H-AI2). Gate: AI-T0, AI-R2, AI-R3.

**AI-T2 — the end-to-end cross-owner INSERT cell.** On the
`ForeignIndexRig` (or its successor with `FkProbeServer` armed on both
cores — `core_runtime.cpp` wires the probe halves on every core), child
on the peer, parent on core 0: the four H-AH1 fixtures driven through the
peer's `DispatchAsync`, the park observed mid-probe, the intent seen in
core 0's `FkIntentTable` while parked and gone after the decide, the row
present on the peer afterwards. The shipped variant (client on core 0,
write shipped to the peer owner) beside it. This is the cell
`known-gaps.md`'s AH-T4 entry names as the debt; on landing, that entry
closes and AH-T5's probe (`bench/fk_intent_crash_probe.py`) becomes
runnable. Gate: AI-T1, AI-R4.

**AI-T3 — measurement.** Not a hypothesis of its own: the cell AH-T6
already owes (H-AH1's cross-owner latency, XD-style leg attribution) is
now reachable, and AI hands it back to AH-T6 rather than duplicating it.
What AI measures itself is the **overhead of the lift on the colocated
peer write**: the gate's predicate loses two `empty()` tests, which is
expected to be unmeasurable, and the results file says so with numbers
rather than by assertion. `bench/v2.8.0/`, `git describe --tags`,
interleaved arms, ≥3 runs. Gate: AI-T2.

## Measurement discipline

`build-release` only; results under `bench/v2.8.0/` named by
`git describe --tags`; interleaved A/B; ≥3 runs with the spread stated;
claims tagged measured (with invocation) or source-read (path:line and
commit). A result on a different host is a new baseline, not a
comparison.

## Row status (CLA, appended as rows land)

| row | status |
|---|---|
| AI-R1 | **Ruled and enacted before the order was filed** — see §"The order arrived after its own AI-T1" below |
| AI-R2 | **Ruled and enacted before the order was filed** — FK arm alone; the cabined and `CannotEnforce` arms are untouched |
| AI-R3 | **Ruled 2026-09-02: re-point A5 to the cabined arm**, CLA's proposal. Enacted below |
| AI-R4 | **Ruled 2026-09-02: the end-to-end cell lands here, as AI-T2**, CLA's proposal — it is this order's acceptance test |
| AI-T0 | **Done 2026-09-02.** Eight sites surveyed, H-AI3 **held**: no site outside the gate descends a relation this core does not own. Table below |
| AI-T1 | **Complete 2026-09-02.** The lift itself and three of AI-R5's placements landed 2026-09-01 at `956f00d`; the remainder lands here — the one surviving stale contract claim corrected, H-AI2 answered by source-read, and AI-R3's re-point built. See "AI-T1's remainder" below |
| AI-T2 | **Two slices built 2026-09-02, and they found four defects.** The end-to-end cross-owner INSERT runs in process and is pinned; F2/F3 are fixed on the operator's ruling and F4 was found behind them. H-AI1's remaining fixtures wait on F1/F4. See "AI-T2, first slice" and "AI-T2, second slice" below |
| AI-T3 | not started |

### The order arrived after its own AI-T1

**AI is drafted against `546ddc8`, and the arm it ratifies was already
narrowed at `956f00d` on 2026-09-01**, on the operator's direct
instruction of that day, landing on `origin/main` at `8c0314d`. The
Background's "the gate as it stands" is therefore history rather than
current source, and this section is the reconciliation rather than a
rewrite of the operator's text.

What that means ruling by ruling:

- **AI-R1 is ruled `yes, both halves` and enacted.** `funded_shape` at
  HEAD is `!any_cabin && !enforcer_.CannotEnforce(access.oid)`; both
  fkey terms and the whole FK refusal branch are gone.
- **AI-R2 is ruled `FK arm alone` and enacted.** The cabined arm and
  `CannotEnforce`'s stand, with their messages unchanged.
- **AI-R5 is partly enacted**: `workplan-peer-writer.md` §4's FK bullet
  carries the lift and its argument, `known-gaps.md`'s AH-T4 entry is
  struck, `foreign-keys.md` §2a gains the reachability paragraph. §8's
  whitelist comment and the four stale contract claims are **not** done.
- **AI-R6 is enacted for the gate itself** — the retired message exists
  nowhere in `src/` — and AI-T0 below confirms no other site was using
  it.
- **AI-R3 is not enacted, and what landed is not what AI-R3 proposes.**
  The FK cell was converted to its own converse
  (`AnFkLinkedPeerRelationNoLongerMeetsTheShapeGate`, asserting the FK
  arm no longer answers in either direction) rather than A5's property
  being re-pointed to the cabined arm. The cabined cell exists and is
  green, but it asserts only that the refusal crossed — **not** the
  byte-identity against the owner's own dispatcher, nor the absence of
  `retryable=1`, which is the half of A5 that proved shipping does not
  route around the gate. That half is currently **proved by nothing**.
- **AI-R4 is not enacted, and what landed assumes the opposite**: the
  end-to-end cell was handed to AH-T6 with its reason stated (the
  in-process rig refills no peer lease for a relation it did not open,
  so a completed peer write meets `TXN_CONFLICT retryable=1` there
  forever). AI-R4 asks for it here instead. The fixture limit is real
  either way and is the work, wherever the row sits.

### AI-T0 — the survey, and H-AI3 holds

Line numbers are HEAD's (`8c0314d`); the order's are `546ddc8`'s, a
uniform −26 from these.

| site (HEAD) | path | on a peer, does it fault a foreign relation? |
|---|---|---|
| `:6280` | `HandleInsert`'s extraction pass over every row | **probes** — `ResolveForeignKeyParents` defers a foreign parent and `SendForeignKeyProbes` parks; a local parent is this core's own pages |
| `:6368` | `SortedFillInner`'s extraction pass | **probes** — same two calls, same fork |
| `:6402`/`:6407` | the bulk admission loop's per-row answer | **local-only** — `CheckForeignKeyOnWrite` with `fk_held`; no descent may be added there and the code says so |
| `:6645`/`:6648` | `InsertOneRow`'s per-row forward check | **local-only** — same held-verdict answer |
| `:8778` | UPDATE's `fk_assignments` build | **local-only** — a name comparison over the schema, no page read; the extraction and probe follow at `:8818`/`:8827` |
| `:8938` | UPDATE's per-row apply | **local-only** — held verdicts |
| `:9813` | DELETE's `check_view` mint | **local-only** — mints a read view, reads no child |
| `:9889` | DELETE's reverse check | **refuses by name** — `CheckNoChildrenBeforeDelete` consults `FkIntentTable` first (catalog-only), then `CheckNoChildReferences`, which refuses `NotImplemented` at `fk_check.cpp:190` before any child page is touched |
| KWP load path | `KwpLoadServer::HandleLoadChunk` | **probes** — it builds a `parser::InsertStmt` and calls `ExecuteInsert`, so it inherits `:6280`'s hoist; AH-R7's survey (`workorder-ah.md`) already found there is no separate caller |

**One arm descends a parent locally and is not a falsification**:
`CheckForeignKeyOnWrite`'s self-referencing case (`fk.rel_oid ==
child.oid`) walks the parent directly, which the extraction pass skips
by design. The relation it descends is the one being written, so the
core writing it owns it by construction.

**H-AI3 holds.** No site outside the gate descends a relation this core
does not own, so the lift owes no additional refusal and AI-T1's "add
any refusal AI-T0 owes" clause is satisfied by there being none.

### AI-T1's remainder — what the 2026-09-01 lift left, and what it did not

**Three claims of the four PW1c-6b-4's precedent names were already
true**, which is worth stating because the order predicted four
corrections and one was owed. At `8c0314d` only the **DDL refused-shapes
list** (`command_dispatcher.cpp`, the comment recording what replaced
PW1c's interim DML guard) still named FK-linked among the shapes the
shape gate refuses. The **whitelist opener** (the `CheckWriteAffinity`
block comment) enumerates the shapes it *admits* and never named the FK
one; the **exhaustiveness comment** and the **`SetIndexBuilds` header**
name index and assertion clients and no FK shape at all. Corrected: the
one list, which now names cabined and assertion-covered as refused and
records the FK lift with its date and this order.

Two rows in `workplan-peer-writer.md`'s task tables (PW1c-5's and
PW1c-6b-4's) still read "FK-linked" and are **deliberately left**: they
are build records of what was true when those tasks landed, and editing
them would falsify the history they exist to keep. §4's bullet is where
the current state lives, and it carries the lift.

**H-AI2 holds, by source-read rather than by a second run.** The peer
branch of `CheckWriteAffinity` opens at `command_dispatcher.cpp:5350`
with `if (catalog_read_only_)`, which is false on core 0 and on every
single-core instance, so no single-core statement reaches the predicate
the lift changed. The suite is green either side of it (3149/3149 at
`546ddc8` and at `8c0314d`), which is consistent with the hypothesis but
does not prove it on its own — the source-read is what proves it, and
this row says which is which.

**AI-R3's re-point, built.** A5's property is *a shipped write is
answered by the owner's own gate, byte for byte, with no retryable bit
invented on the way*, and it is independent of which arm answers. The
two assertions that carried it — the byte-identity against
`rig.peer->dispatcher()`'s own reply, and the absence of `retryable=1`
and `TXN_CONFLICT` — moved from the retired FK cell onto
`AShippedWriteToACabinedPeerRelationIsRefusedByTheOwnersShapeGate`,
which until now asserted only that *a* refusal crossed. The group header
records the move so the next reader does not have to reconstruct it.
Both cells green.

### AI-T2, first slice — the crossing runs, and three things behind it do not

**The cell exists and passes**, which is the first time a real cross-owner
`INSERT` has run in this repository:
`CoreRuntimeTest.ACrossOwnerInsertProbesTheParentsOwnerAndWritesTheChildRow`.
Parent on core 0, child on the peer, the statement started on the **peer's**
dispatcher: the first poll parks (the extraction pass runs before any row
work, AH-R1), one pump carries the probe and core 0 answers, `probes()` goes
to 1 and stays there, an intent appears on the parent's owner, the statement
resumes and the row is written and readable through the peer's own
dispatcher.

**Two fixture pieces made it reachable, and neither is an engine change.**
Core 0 in `ForeignIndexRig` is hand-built, so it needed the two probe halves
production wires on every core (`CoreRuntime::AttachTransport`), plus
`SetFkIntents`; and `FundPeerForRelation` applies
`AFundedPeerInsertsIntoItsOwnRelationEndToEnd`'s recipe — fault extent, write
grants over the two creation pages, a row-id block — to a relation created
*after* the rig opened. The "a completed peer write meets `TXN_CONFLICT
retryable=1` there forever" note carried at `956f00d` was therefore a
**fixture gap, not a fixture limit**, and the corrected reading is in
`known-gaps.md`. One ordering matters and is stated in the cell: sync core
0's store **before** funding, because `AdmitWritePages` faults each granted
page for read before restamping it and abandons the whole grant silently
otherwise.

**Two policies place a relation on each side of a two-core rig.** `kRotate`
puts everything on core 1 at two cores (`kSystemCore + 1 + seq % (core_count
- 1)`), so the parent takes `kCreatingCore` for the length of its `CREATE`.

**What the slice found** is in
`docs/inflight/bugs/fk-reference-intent-never-released-on-autocommit.md`,
open: the intent is never released on an autocommit statement and the parent
row becomes permanently un-deletable behind a `retryable=1` code (F1); a
session whose first cross-core contact is a probe never mints a shipping
identity, so a transaction carrying a cross-owner FK write refuses at
`COMMIT` (F2); and every un-shipped session shares the holder key
`(core, 0)`, so one decide would release another session's intents (F3). One
root cause — the probe path uses the shipping identity without being one of
the things that mints it, and the autocommit path assumes a decide its own
enrolment rule prevents.

**H-AI1 is therefore split rather than held or falsified.** The present-parent
fixture holds end to end. The other three (absent parent, retired parent,
in-flight parent) and the intent's release-after-decide are **not built
here**, because two of them cannot be asserted until F1/F2 are answered: an
explicit transaction cannot commit, and an autocommit's intent never ends.
The remaining fixtures land with the fix.

### AI-T2, second slice — two of the four fixed, and the third moved

**F2 and F3 are fixed on the operator's ruling of 2026-09-02**: the
shipping identity is minted at the first cross-core **contact** rather than
at the first ship, which is the rule `ShipStatement`'s own comment already
states applied to the other contact. One line in `SendForeignKeyProbes`,
and the branch that refused *"a cross-owner transaction has participants
but no shipping identity"* — whose comment called its own state impossible
— is no longer reachable from a probe.

**It did not make the transaction commit, because F4 was behind it.** With
an identity the `COMMIT` reaches the prepare leg and the prepare finds
nothing to prepare: enrolment by probe is **coordinator-side only**, and
the participant's context lives in `ShippedStatementExecutor::enrolled_`,
which a shipped statement fills and an intent-only participant never has.
The owner answers *"holds no transaction for core 1's session N"* and the
transaction aborts, retryably, forever.

**F1's ruling inherits it.** The operator ruled that autocommit enrols too,
so the existing decide leg releases the intent; whether that decide is
reachable without a prepare is F4's answer, so F1 is ruled and blocked
rather than built.

**The wall is pinned, not worked around**:
`ACrossOwnerFkProbeMintsTheSessionsShippingIdentityAndStopsAtThePrepare`
asserts that F2 is fixed (an identity exists, and the identity refusal is
gone) and that F4 is where the statement stops. Its three F4 assertions are
to be **deleted with the fix, not around it** — a suite that quietly
stopped asserting them would be a transaction that started committing
without anyone deciding how an intent-only participant prepares. F4's three
candidate shapes are in the bug file, and the choice between them is a
design decision this order does not take.

**The rig gained core 0's participant half of 2PC** for this slice, with
the FK release riding the decide exactly as `CoreRuntime::AttachTransport`
wires it. Until the foreign-key probe nothing enrolled core 0 as a
*participant* — a peer's shipped write makes core 0 the owner, not a
participant of somebody else's transaction — so the rig had the coordinator
half alone.
