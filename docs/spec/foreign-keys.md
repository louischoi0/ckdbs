# Foreign keys in KDS — implementation guideline (v1)

Status: **GUIDELINE** — decisions F1–F6 proposed as a set. **FK-M1 through
FK-M5 are built** (2026-08-05); FK-M6 is out of v1 by F2. Foreign keys are
declared *and enforced*.
Depends on: keystone-id-invariant.md (K1/K2, adopted), step chain +
compiler (exec/), probe memo (step_vm), MVCC in-place + undo model
(heap_page.hpp: tuple header `trx_id` + `undo_ptr`, no xmax),
core-ownership dispatch (D3), stoppable walks (VisitControl, V03).
Interlocks with: cabin.md (§reverse check), unique-constraint semantics
(fail-fast, same family).

Grounding note: this guideline was written against an earlier tree, and
**three of its premises have since expired**. They are corrected here
rather than in place, so the original reasoning stays readable:

- **DELETE exists** (`CommandDispatcher::DeleteInner`), as a delete-mark
  with undo and WAL. §3's "specified here but sequenced after DELETE
  lands" no longer defers anything: FK-M3 is unblocked.
- **Transactions and MVCC are built** (`docs/spec/txn.md` T01–T14), so §4's
  check-visibility mode is a small addition beside an existing
  predicate rather than new machinery. See the amendment under §4.
- **Cabin v1 is built** (CB01–CB11), so FK-M5's "when cabins land" is
  now a matter of calling `Catalog::CreateCabin` and reading the
  observed set.

One premise that has **not** expired and blocks F4 as written: INSERT
compiles to no step chain. `exec::Compile()` is SELECT-only, and
UPDATE/DELETE use `CompileWhere` → one `Step` walked by the dispatcher,
not the step VM. There is nowhere to inject an implicit sub-chain on
the path that needs one. See the amendment under §2.

Decisions proposed as v1:

- **F1 — FKs reference the parent's Keystone id.** The child fk column
  holds the parent's engine pk (40-bit id in a u64/int cell), never a
  business key. Consequences bought outright by K1/K2: *ON UPDATE
  CASCADE does not exist* (the referenced key is immutable), and a
  stored reference can dangle but never mis-attribute (issue-once).
- **F2 — v1 actions: RESTRICT / NO ACTION only.** CASCADE and SET
  NULL are deferred: a cascading delete of a large subtree is one
  statement monopolizing a core under run-to-completion, and needs a
  budget-interaction design of its own first.
- **F3 — Fail-fast, no waiting.** A constraint check that meets a
  conflicting *in-flight* writer returns an error immediately (client
  retries). Commercial engines block on the writer's outcome; blocking
  is not expressible on a cooperative single-writer core, and the
  deterministic-error semantic is the same one adopted for unique
  checks. ~~New status code `kConstraintBusy` `[PROPOSED]`, distinct
  from `kFkViolation`, so clients can distinguish "retry" from
  "wrong".~~

  **`kConstraintBusy` is not enacted, and the crossing does not
  reopen it** (AH-R4, 2026-09-01,
  `instructions/v2.8.0/workorder-ah.md`). FK-M2 declined it and the
  decline stands: the busy verdict is `FkVerdict::kBusy` →
  `Status::TxnConflict`, wire-spelled `TXN_CONFLICT`, **one retryable
  code wide**. SA-R7 proposed enacting it on the grounds that a
  cross-core check makes the retry/wrong distinction load-bearing —
  and it *is* load-bearing, but `TxnConflict` already carries it: what
  a client does with a busy is retry, which is exactly what the code
  it already has says. A second retryable code buys the client
  nothing and costs the wire a permanent registry entry
  (`status.hpp`'s standing argument on the width of the `retryable`
  bit). If the *engine* ever wants to tell its own two busies apart,
  that is a protocol-silent addition made then.
- **F4 — Checks compile into the step chain.** No trigger subsystem,
  no SPI-style re-entry: the forward check is an implicit
  **correlated sub-chain** whose single step is a `kProbe` on the
  parent relation, emitted by the step compiler. One evaluator, one
  executor, one stats/ANALYZE surface — FK checks show up as ordinary
  steps.
- **F5 — Colocation prerequisite.** Parent and child must be owned by
  the same core. `CREATE`-time validation rejects a cross-core FK as
  Unsupported (J2-style, no slow path); the FK graph becomes an input
  to placement policy (D3), not a runtime coordination problem.

  **Converted 2026-09-01 by AH-T4** (`instructions/v2.8.0/workorder-ah.md`,
  operator's mark at AH-R6). Colocation is no longer a *prerequisite*;
  `CheckForeignKeyColocation` admits a cross-owner pair. What replaces
  the refusal is §2a's forward check, and what survives of F5 is
  **advice**: colocation is the cheaper shape, and a **namespace** is how
  a user asks for it (`ratification-af-namespace.md` AF-P5) — a constraint
  that never has to cross beats one that crosses correctly. D3 keeps the
  FK graph as a placement input either way.

  **What a cross-owner pair costs, so the admission is not read as
  free**, both fail-closed and neither a wrong answer:

  - every child `INSERT`/`UPDATE` pays **one ring round trip per distinct
    parent owner**, at the dispatch fork, before any row work (§2a);
  - a **`DELETE` of the parent is refused** while any child lives on
    another core (§3a). RESTRICT needs an authoritative "no children" and
    the parent's core cannot see them.

  The refusal it replaced was correct for its time and its argument is
  worth keeping: a declaration admitted before the crossing existed would
  have sent `CheckParentPresent` to `BtreeLookup` a page this core may not
  fault. That is why the conversion was ordered last rather than first.
- **F6 — Reverse check is Cabin's territory.** Parent-delete's "does
  any child reference me" starts as a stoppable walk and is the
  designated beneficiary of a Cabin on the child fk column; an FK
  declaration *nominates* that cabin (cabin.md §7). RESTRICT needs an
  authoritative "no children" — exactly Cabin's verified empty set.

---

## 1. Catalog

**Built (FK-M1).** `SysFkeyRow` in `include/kds/catalog/rows.hpp`, on
the fixed catalog page `kCatalogPageFkeys = 13`:

```
sys.fkeys                       28 bytes, offsets pinned by offsetof
  fk_id            u64          AllocateRowId(kSysFkeysTable)
  child_rel_oid    u64
  parent_rel_oid   u64
  child_column_no  u16          never 0
  flags            u16          (bit 0: kFkNullable — MATCH SIMPLE)
```

Field order is by descending alignment, so the struct's offsets and the
on-disk ones coincide — the discipline every catalog row follows. Two
absences are decisions: **no parent column**, because F1 fixes the
parent side to the Keystone id for every foreign key there can be, and
**no action field**, because F2 leaves v1 with exactly one action and a
field with one legal value is a field that only records that a decision
was deferred. `kFkNullable` gained its writer with NULL storage
(2026-08-20, `docs/spec/null.md`): `Catalog::CreateForeignKey` stamps it
from the child column's declared nullability. A stored 0 keeps its one
reading — "the check runs" — and enforcement never consults the bit (see
§3's semantics note); it records the declaration for display.

Adding the relation cost a **superblock format bump, 10 → 11**, so every
pre-existing data file stops mounting. That is the fourth repeat of the
5 → 6 shape (a new bootstrap relation on a page id an older file does
not have) and the one where mounting anyway would be worst: a
version-10 file would read an empty foreign-key list and, once FK-M2
lands, enforce nothing. A constraint that silently does not run is not
a degraded mode.

CREATE-time validation, in `catalog::CheckForeignKeyDeclaration` and
`CheckForeignKeyColocation` (`include/kds/catalog/foreign_key.hpp`):
both relations exist; the child column is not column 0 and its type can
carry a Keystone id; **the parent is a btree relation** (see below);
**owning core equality (F5)**; duplicate FK on the same (child, column)
rejected. They are free functions rather than `Catalog` methods because
**two doors ask the same questions**: `CREATE TABLE` checks before the
relation is created, so a refusable declaration writes nothing (there is
no DROP TABLE to undo one, and unlike a Cabin a constraint may not
degrade to a warning), and `Catalog::CreateForeignKey` checks again
because it is the door every foreign key comes through — the argument
`CreateCabin` already makes about `NO CABIN`.

**`[AMENDED 2026-08-31 - which refusal code]`** The heap parent and the cross-core
pair answer **`NotImplemented`** (`include/kds/base/status.hpp`): both are listed
as open in `CLAUDE.md`, so both are things a later release could build, and telling
a client "rewrite, forever" about them would be a false statement about this
engine's future.

**A heap parent is refused, `Unsupported`** — a decision taken at FK-M1
and not in the original F-set. F1 puts the reference on the parent's
Keystone id, and a heap relation has no pk index: `LocateByPk` answers
`kScan` for one, so every child INSERT would scan the parent, and the
whole parent when the row is missing — which is the case the check
exists to catch. Refusing keeps a constraint's cost a descent. It is
also the cheapest thing to relax later: allowing heap parents adds a
case, changes no format, and invalidates no stored row.

**Nothing back-checks existing rows**, and nothing has to: a foreign
key can only be declared at `CREATE TABLE`, on an empty relation, since
there is no `ALTER TABLE`. A back-check is what an `ADD CONSTRAINT`
path would need, and it does not exist to need it.

Compiler and write-path visibility: `TableAccess` carries `fkeys_out`
(this relation as the child) and `fkeys_in` (as the parent), both built
from **one** `sys.fkeys` scan when the relation is opened. Neither is
consulted per tuple. Note the direction that forces a global
invalidation rather than an in-place cache update: creating a *child*
stales the **parent's** `fkeys_in`, a relation the DDL statement never
names.

Surface: `<col> <type> REFERENCES <parent>` at `CREATE TABLE`,
unreserved like the `CABIN` suffix beside it and written before it when
both appear. `REFERENCES <parent>(<col>)` is refused with a position —
the only column it could name is the one F1 already picked, and any
other is a reference the engine cannot store. `SHOW FKEYS` lists them;
`DESCRIBE` carries `references=<parent>` on the declaring column.


## 2. Forward check — child INSERT / UPDATE of the fk column

**Where.** The step compiler, when compiling INSERT (and UPDATE whose
SET touches an fk column), appends an implicit correlated sub-chain
per applicable FK:

```
step: kProbe parent_rel  key = <fk value being written>
      residuals: none    semantics: EXISTS
```

This is deliberately the same shape as a user-written correlated
EXISTS — the executor needs no new step kind, `ExecStats` counts it
like any step, and ANALYZE prints it (tagged `implicit-fk`
`[PROPOSED]` in the plan printer so operators can see constraint cost
per statement).

**Amendment (FK-M1, found by reading the code).** The step-chain
injection above cannot be written as specified: INSERT compiles to no
chain at all, and UPDATE/DELETE compile a single `Step` that the
dispatcher walks itself rather than running through the step VM. F4's
*intent* survives and is what FK-M2 should build — one shared check
helper, no trigger subsystem, no second evaluator, no SPI-style
re-entry — but the mechanism (an implicit correlated sub-chain emitted
by the step compiler) is unavailable until write statements compile to
chains. The consequences to accept with it: the probe memo does not
apply, since it lives in the step VM, so the batch-insert win §2 claims
has to come from somewhere else or not at all; and `ExecStats` /
ANALYZE do not see the check for free, which is what FK-M4 has to
supply instead. Converting INSERT to a step chain first is the
alternative, and it is a much larger change that also has to thread the
check-visibility mode through `AcceptTupleAt`.

**Semantics.**

- NULL fk value → check skipped (MATCH SIMPLE). Realized without reading
  `kFkNullable`: the forward check's non-integer bail passes a NULL
  through, and the row codec then stores it (column declared `NULL`) or
  refuses it by name (`NOT NULL`) — so the NOT NULL refusal is the gate.
  On the reverse side a NULL child cell matches no parent pk, so a NULL
  child never blocks its parent's delete.
- Probe finds a version → apply **check visibility** (§4): visible
  committed parent → pass; delete-marked by an in-flight foreign trx →
  `kConstraintBusy` (F3); deleted-committed or not found →
  `kFkViolation`.
- Check runs **before** the heap write of the child row: on failure
  the statement aborts with no undo work. Ordering is free under
  run-to-completion; check-first is simply cheaper.

**What the current machinery gives for free.**

- **Probe memo**: batch-inserting N children of one parent pays one
  descent; N−1 checks are memo hits re-verified on the memoized page.
  This is the single biggest practical win — the common OLTP shape
  (many trades, one account) makes the FK check nearly free after the
  first row.
- **Budget**: fk probes count into `Budget::touched()` like any other
  page touches — no separate accounting.
- Later, the same probe position is exactly where trail replay and
  (C6) location hints already apply. Nothing FK-specific to build.

## 2a. The forward check across owners — the park is at the dispatch fork

`[AMENDED 2026-09-01 — AH-T0. The text is the contract; the code
follows at AH-T1/T2 and this section says "amended", not "built",
until it does.]`

**The problem, in one sentence.** The check runs where nothing can
wait. `CheckForeignKeyOnWrite` is a plain `Status` member called per
row from inside an already-open `WriteScope`, and nothing on that
stack suspends — so a check that has to ask another core cannot ask
from where it stands.

**The answer is to move the asking, not to make the write scope
wait** (AH-R1). Every foreign parent pk a statement needs is extracted
**before any row work** — an INSERT's from its `VALUES`, an UPDATE's
from its `SET` body — and probed at the **dispatch fork**, the one
place a write already knows how to park: `HandleInsert` /
`HandleUpdate` return `pending_remote` and resume there today. The
statement then runs **synchronously** against the intents it now
holds, and the per-row check that remains inside the write scope
answers from held intents and local state only. Nothing inside an open
`WriteScope` ever initiates a ring round trip — RD5's wall is left
untouched and unmet rather than negotiated with.

**One round per distinct owner, never per row** (AH-R2). The extracted
pks are deduplicated and grouped by resolved owner; each foreign owner
gets **one** `kFkProbeRequest` carrying its whole set, and enrolling
that owner as a participant rides the same round (RR1's
enrol-on-first-contact). So a statement's foreign-FK cost is a
function of **how many distinct owners its parents live on**, not of
how many rows it writes — which is the property that makes a
thousand-row insert against one foreign parent cost one round trip.

**The parent set must be enumerable, and today it always is**
(AH-R3). F1 makes an fk value a literal or a bound parameter, so the
extraction pass is total. The rule is written for the day F1 moves: a
statement whose parent set cannot be enumerated at the fork is
**refused**, with the byte, rather than run against a partial set of
intents. Fail-closed; in code an assert-and-refuse arm, not a handled
path.

**What the probe leaves behind is memory-resident, and that is safe
for a stated reason** (AH-R5). The probe leaves a row-scoped
**reference intent** on the parent's owner; a parent-side DELETE
meeting a live foreign intent answers busy (§3, and one code wide per
F3 as amended). Under `cross-owner-txn.md` §1a an intent-only
participant writes no `TXN_PREPARE` record, so its intents die with
the process. The invariant that makes that safe is not an argument
but a testable statement: **a participant that restarts after granting
an intent and before its prepare leg forces the coordinator's
transaction to fail** — the prepare cannot be answered by a process
that has lost the enrolment. AH-T5 proves it by killing the
participant in exactly that window. A window in which the coordinator
can still commit is a defect, not a documented limitation.

**What makes this reachable at all** is a refusal in a different
subsystem, narrowed 2026-09-01 on operator direction:
`CheckWriteAffinity`'s peer-writer funding gate refused every write to an
FK-linked relation on any core but 0, on the grounds that *"validation
reads the linked relation, which this core may not fault"* — the exact
defect this section removes. Its FK arm is struck and the narrowing is
recorded in `workplan-peer-writer.md` §4; the gate's cabined and
unenforceable-assertion arms are untouched and still refuse. Before that,
a relation carrying a foreign key took no peer write, so nothing on this
path ever ran in a live instance.

**The load path is in scope** (AH-R7). `CheckForeignKeyOnWrite`'s
third caller is the KWP load path, which has its own batch boundary
and takes the same hoist there. If no park-capable seam exists in it,
that path **refuses** a cross-owner-FK write with a message naming
this order — never a silently local-only check, which is the one
degraded mode §1 says a constraint may not have.

## 2b. The intent's end — a holder is not a participant

`[AMENDED 2026-09-02 — work order AI, AI-T2. Built, and pinned by
`ACrossOwnerInsertProbesTheParentsOwnerAndWritesTheChildRow` and
`ACrossOwnerFkWriteInATransactionCommitsAndItsDecideEndsTheIntent`.]`

§2a says the intent is released by the transaction's **decide** and by
nothing else. Two things about that sentence were wrong in the first
build, and the end-to-end cell is what found them.

**A core that answered a probe is an intent holder, not a participant.**
The distinction is what the two lists exist to keep:

| | participant | intent holder |
|---|---|---|
| what it holds | rows of this transaction | a reference intent, and nothing else |
| how it got a context | a statement shipped to it | it answered a probe |
| the prepare | votes, and a missing context is an **abort** | is not asked |
| the decide | told | told |

The first build enrolled the owner as a *participant*, and a participant
is asked to prepare. An intent holder has no context to prepare with — the
participant-side context lives in `ShippedStatementExecutor::enrolled_`,
which a shipped statement fills — so the owner answered *"holds no
transaction for core N's session M"* and the transaction aborted,
retryably, forever. **A cross-owner foreign key write inside an explicit
transaction could not commit at all.** The prepare now goes to the
participants and the decide to the union, and a core in both lists is
prepared once and decided once.

**And the separation is what keeps RR0's join bit true.** `HasParticipant`
is what a shipped statement reads to decide whether the owner *already
holds a context* — true means it must join one rather than open a second.
An intent holder holds none, so a probe that enrolled a participant would
have told the next statement shipped to that owner to join a context that
does not exist. Recording the holder on its own list answers `false` there,
which is the truth.

**An autocommit statement decides for itself.** Its transaction begins and
ends inside one statement, so no `COMMIT` will ever run for it. The first
build enrolled nobody there, reasoning that the decide "is the one this
core's commit sends to every participant it has" — and with nobody
enrolled there was no decide, so the intent was never released and the
parent row was **un-deletable for the life of the process**, behind a
`retryable=1` code a client's retry loop cannot get past. The statement
now sends its own decide, after the write scope closes and never inside it
(AH-R1), and waits for the acknowledgement.

**A holder is told it is one, per target.** The decide carries
`TxnDecideRequestPayload::intent_only`, set on the targets that hold an
intent and no rows. Without it a holder's missing context is
indistinguishable from a participant's, and `ShippedStatementExecutor`'s
anomaly arm — the one that reports a lost transaction half — fired on the
*success* path of every cross-owner foreign-key statement, logging an error
and bumping `decide_refusals`. A core in **both** lists is a participant and
takes the ordinary path; the bit is a per-target fact, not a per-decide one.

**A participant coordinates its own release, and this is load-bearing.**
When the write that probed was itself a *shipped* statement, the intent
holder is enrolled on the participant's context session — and the
coordinator's decision is applied by dispatching `COMMIT`/`ROLLBACK` through
that session (`shipped_statement_executor.cpp`), which forks on
`has_intent_holders()` like any other. So the participant sends its own
decide, keyed `(its core, its ship id)`, which is exactly the key the intent
carries — and the coordinator's own key, `(its core, its client session)`,
is a different one, so the two do not collide.

**Pinned since 2026-09-02** by
`CoreRuntimeTest.AParticipantCoordinatesItsOwnIntentReleaseThroughTheDecidedCommit`,
which takes apart what every other cell holds together: the core that probes
is not the core that decides. It asserts that the coordinator records no
holder of its own, that the intent is live while the transaction is, that it
is gone after a `COMMIT` whose decide the *participant* sent, and that the
holder reports no 2PC anomaly — the last of which fails when
`Session::IntentOnlyTargets()` is neutered, which is how the cell is known to
bite rather than merely to pass.

**Measured 2026-09-02**
(`bench/v2.8.0/results-ah-t6-participant-release-cost-v2.7.0-101-ged47cfc.md`),
and the measurement corrected the prediction. That `COMMIT` takes the
cross-owner parked path, so the participant's own acknowledgement leg goes
from **4.4 µs to 3.1 ms — 720×** — and XE1's `kAtAppend` saving is entirely
lost for this shape. But it is **not** the `fdatasync` XE1 removed: under
`relaxed`, which stages no group commit at all, the leg is the same order
(2269 µs). What the participant pays is the cross-owner path's *own*
decision-durability wait, which is unconditional on the class by design,
plus its own decide round trip. **An intent-holding participant stops being
a participant that acks cheaply and becomes a coordinator that must make its
own decision durable**, and a coordinator's durability wait was never XE1's
to remove.

Whether a client feels it depends on what else is waiting: under the shipped
class the 3 ms is invisible (−1.4%, inside noise) because the coordinator is
already device-bound; under `relaxed` 1940 µs of it reaches the client
(+53.8%). It is free today and would stop being free if the coordinator's
own device wait went away.

**The autocommit decide is sent before this core's commit record is
durable**, unlike the explicit-transaction path where the decision is durable
first (D4). That is sound and worth saying: if the coordinator dies in that
window its child row is lost at recovery, so the parent whose intent was just
released has no surviving referent.

**Every cross-core contact mints the session's shipping identity**, not
only a ship. The identity is what an intent's holder key is
`(coordinator core, session id)` built from, so a session that had never
shipped probed under id 0 — which made every un-shipped session on a core
share one holder key, and left the coordinator holding participants it had
no identity to address. `ShipStatement` and `SendForeignKeyProbes` are the
two contacts, and both mint it.

## 3a. The reverse check across owners — a refusal, not a fan-out

`[AMENDED 2026-09-01 — AH-T4.]`

A parent's `DELETE` asks *"does any child still reference me"*, and
RESTRICT needs that answer to be **authoritative**: a "no children" that
saw only some of the children is a dangling foreign key with the
constraint reporting success, which §1 names as the one degraded mode a
constraint may not have.

Once F5 converts, a child can live on another core, and this core cannot
see its rows. So the reverse check **refuses** rather than walking what it
can reach: `NotImplemented`, naming the child's owner. The consequence,
stated plainly, is that **a parent in a cross-owner foreign key cannot be
deleted** — an asymmetry with the forward direction, which crosses fine.

What would replace the refusal is a **fan-out**: one boolean probe per
child owner, each answering from its own Cabin (F6's nomination) or its
own walk, over the existing fan-in shape. It is not built.

**And before the walk, the intents.** A live reference intent on the row
being deleted is evidence no local walk can produce: a transaction on
another core probed this parent, was told it exists, and is writing a
child against it — a row this core cannot see. Meeting one answers
**busy** (`TxnConflict`, retryable, one code wide per F3 as amended),
because the foreign transaction has not committed and the answer depends
on how it ends. The intent is released by that transaction's decide and
by nothing else (§2a).

## 3. Reverse check — parent DELETE

Prerequisite reality **as of FK-M1: DELETE exists** — a delete-mark
with undo and WAL, in `CommandDispatcher::DeleteInner`, which walks the
relation and marks each matching row. The reverse check hooks into that
per-row lambda, before the mark. (This paragraph previously said the
statement did not exist and sequenced FK-M3 behind it; that is no
longer true, and nothing sequences FK-M3 but FK-M2.)

When DELETE compiles for a relation with incoming FKs, emit per
incoming FK an implicit sub-chain on the child relation:

```
step: existence walk over child_rel
      residual: child.fk_col == <parent pk being deleted>
      stop:     VisitControl::kStop on first visible match
```

- First visible child → `kFkViolation` (RESTRICT). In-flight child
  insert encountered → `kConstraintBusy` (F3) — the in-place row with
  a foreign `trx_id` *is* the lock record; no lock manager exists or
  is needed.
- Cost honesty: a full child walk per deleted parent until a Cabin
  covers the fk column. Acceptable for v1 because parent deletes are
  rare in the target workload; the moment it isn't, the fix is
  declared: `CREATE CABIN ON child(fk_col)` — whose **verified empty
  set is the authoritative "no children"** RESTRICT wants, and whose
  observation is naturally driven by exactly the parents that get
  deleted (F6). The FK declaration nominates this cabin; auto-creation
  thresholds belong to the cabin/promotion policy, not here.

There is no reverse check for parent UPDATE: K2 makes pk update
Unsupported, so the case is closed by contract, not by code.

## 4. Check visibility — one MVCC mode, not a second implementation

Constraint checks cannot read at the statement snapshot alone: a
parent committed-deleted *after* this snapshot was taken must still
fail the check (latest-state semantics, as in commercial engines), and
an in-flight writer must be *seen* to fail fast (F3). Define a
**check-visibility mode** on the existing visibility routine — same
code path, a flag, three verdicts:

| tuple state at check | verdict |
|---|---|
| current version committed, live | pass |
| current version delete-marked / absent, committed | `kFkViolation` |
| current version written by another in-flight trx (`trx_id` foreign, unresolved) | `kConstraintBusy` |
| written by **own** trx | judge by own pending image (self-consistency) |

Implementation rule: this mode lives beside the snapshot visibility
routine in the same translation unit and shares its version-walk code.
A second, FK-private visibility implementation is the failure mode to
refuse in review.

**Amendment (FK-M1).** `docs/spec/txn.md` is built, which makes this
concrete and *smaller* than written. There is no version walk to share:
latest-state semantics means the answer is the version on the page, so
the check never steps back through undo — and `parser-v2.md` I15's R1
is satisfied without the two-phase split `Classify` /
`ResolveThroughUndo` needed. The mode is a sibling function over the
same three tuple fields, against a read view **minted at check time**
(`TransactionManager::MintReadView`) rather than the statement's:

| tuple's own version, against a freshly minted view | verdict |
|---|---|
| writer visible, not delete-marked | pass |
| writer visible, delete-marked | `kFkViolation` |
| writer not visible (in flight) | `kConstraintBusy` |

It cannot call `Classify` verbatim, and the case that proves it is an
in-flight *insert* of the parent: `undo_ptr == 0` with an invisible
writer, which `Classify` answers `kNoVersion` and the check must answer
**busy**, not violation. The fourth row of the table above needs no
special case — a fresh view carries `own_trx_id`, so a transaction's
own pending image is visible to its own check by the ordinary rule.

One open question this amendment does not settle: `kConstraintBusy`
would be the **second** retryable status code, and `IsRetryable` maps
directly onto the wire's `retryable` bit, which `docs/spec/protocol.md` §11
calls a compatibility surface. Reusing `kTxnConflict` for the busy case
— it is a first-updater-wins-shaped conflict, and a client already
retries on it — keeps that surface one code wide while leaving
retry-versus-wrong distinguishable, since the violation gets its own
non-retryable code. Decide at FK-M2.

## 5. What is deliberately absent

- No lock manager, no wait queues, no deadlock detector — F3 plus
  in-place `trx_id` makes the uncommitted row itself the conflict
  signal, and run-to-completion removes the check-to-write race that
  gap locks exist to close elsewhere.
- No ON UPDATE actions of any kind (K2).
- No cross-relation write hooks: both checks are *reads* injected into
  the writing statement's own chain; FK never writes to the other
  relation in v1 (that starts with CASCADE, which is deferred).
- No trigger framework: F4 forecloses it on purpose.

## 6. Milestones

**FK-M1 — Catalog + DDL surface. Built (2026-08-05).** sys.fkeys
row/codec + catalog cache lists (outgoing/incoming) + `CREATE TABLE ...
REFERENCES parent` parsing + CREATE-time validation incl. colocation
(F5). Acceptance met at all three: declarable, introspectable (`SHOW
FKEYS`, `DESCRIBE`), rejectable (unknown parent, heap parent, type,
pk column, parent column list, duplicate, cross-core) —
`tests/foreign_key_test.cpp`. Three things landed that the milestone
did not name: the heap-parent refusal (§1), the format bump to 11, and
`catalog::CheckForeignKey*` as free functions so the pre-check and the
door share one definition of a legal declaration.

**FK-M2 — Forward check. Built.** `exec::CheckParentPresent`
(`include/kds/exec/fk_check.hpp`) plus `txn::CheckVisibility`, called
from `InsertInner` **before the row id is allocated** and from
`UpdateInner`'s per-row lambda when the SET list touches an fk column.
Acceptance met: violation, busy, own-transaction parent, rolled-back
parent, committed-deleted parent, NULL-skip (vacuous while no column
can hold one), and an UPDATE matching no row running no check.

**FK-M3 — UPDATE-of-fk + reverse check. Built.**
`exec::CheckNoChildReferences`, called from `DeleteInner`'s per-row
lambda before the mark. Stops at the first live child
(`VisitControl::kStop`), so a violation costs a prefix and only a pass
costs the relation.

**FK-M4 — Statistics. Built, in the form the F4 amendment leaves
possible.** The checks are not steps, so `CommandDispatcher::
RecordFkAccess` records the shape by hand: a `kLookup` on the parent's
pk for the forward check, a `kFilterScan` (or `kCabinProbe` when the
Cabin answered) on the child's fk column for the reverse one. Both show
up in `SHOW ACCESS` beside ordinary query shapes, which is what lets an
operator compare constraint cost against query cost. The plan-printer
tagging the milestone asks for has no plan to print on an INSERT.

**FK-M5 — Cabin. Built, read-only.** The reverse check consults an
active Cabin on the child's fk column: an observed value's entry set is
resolved and key-re-checked, and an exhausted, all-non-matching set is
an authoritative "no children" answered **without walking**. A heap
child with a failed hint abandons the Cabin and walks, exactly as
`ServeFromCabin` does.

**The nomination half is deliberately not built, and F6 is corrected
rather than deferred.** A reverse check would record the pk *being
deleted*, and a pk is deleted once - so the entry teaches a value no
later check can ask about, while `cabin_max_values` is a cap that
refuses to observe once full. Recording would spend a bounded budget on
values dead by construction and could crowd out live ones. The values
that make the hit path fire arrive the ordinary way, from queries
filtering children by parent id - which is the workload that justifies
such a Cabin anyway. So no `REFERENCES` clause auto-creates a Cabin
either: `CREATE CABIN ON child(fk_col)` is the surface, and the
promotion pipeline that would judge it automatically is the `CABIN
AUTO` decision, unchanged.

**F3's `kConstraintBusy` was not added** (decided at FK-M2). A
violation is `kFkViolation`, new and **non-retryable**, spelled `ERR
FK_VIOLATION retryable=0`; an in-flight writer reuses `kTxnConflict`,
`ERR TXN_CONFLICT retryable=1`. Retry-versus-wrong stays
distinguishable, and the wire's `retryable` bit - a compatibility
surface - stays one code wide instead of two.

**FK-M2 — Forward check.** Compiler injection of the implicit kProbe
sub-chain on INSERT; check-visibility mode (§4); `kConstraintBusy` /
`kFkViolation` statuses; probe-memo batch behavior verified by test
(N-child insert = 1 descent). Acceptance: violation, busy, NULL-skip,
and batch cases green; ANALYZE (when its per-step stats land) shows
the implicit step.

**FK-M3 — UPDATE-of-fk-column + reverse check.** Extend injection to
HandleUpdate's SET analysis; implement the reverse existence walk in
`DeleteInner`'s per-row lambda, stopping at the first visible match via
`VisitControl::kStop` (V03 built the stoppable walk this needs).
**No longer blocked**: DELETE landed with the transaction work.
Acceptance: RESTRICT blocks a referenced parent's delete;
stop-on-first-match verified via page-touch counts.

**FK-M4 — ANALYZE integration.** Tag implicit steps in the plan
printer; per-step stats attribute fk-check cost. Acceptance: operator
can read "this INSERT spends X on FK probes" from ANALYZE output.

**FK-M5 — Cabin nomination.** Cabins have landed (CB01–CB11), so this
is now a matter of `Catalog::CreateCabin` at declaration and reading
the observed set at check time. FK declaration
registers the child fk column as a nomination; reverse check consults
an active cabin's observed set before walking. Acceptance: with a
cabin observed for the parent value, reverse check is O(entry-set)
and walk-free; empty-set RESTRICT pass verified.

**FK-M6 — (deferred) CASCADE / SET NULL.** Requires the budget
interaction design (long cascades vs run-to-completion) and
multi-relation write semantics; out of v1 by F2.

Order: FK-M1 → FK-M2 → FK-M3 → FK-M4 → FK-M5, **all done**. The DELETE
prerequisite that used to sit between M2 and M3 is built, and FK-M5 no
longer waits on the Cabin timeline for the same reason. FK-M2 is
independently shippable and already delivers the highest-value half
(child-side integrity) for insert-dominated workloads — and it is the
milestone that makes a declared foreign key mean anything at all.

## 7. Out of scope

- Composite (multi-column) FKs — single Keystone reference only in v1.
- Referencing non-pk unique columns (needs B2 unique secondary
  indexes first; F1 keeps v1 on engine identity).
- DEFERRABLE semantics — checks are immediate; revisit only with the
  transaction-model work.
- Cross-core FKs — placement policy work (D3 + FK graph) may later
  relax F5; v1 rejects.
- FK under a **split** relation (added 2026-08-24): `docs/spec/crosscore.md`
  §6a gates an FK parent or child from splitting until this doc decides
  — RESTRICT validation's validation-to-commit window is sound only
  against local latest-committed state, and a split parent makes it
  cross-core.
