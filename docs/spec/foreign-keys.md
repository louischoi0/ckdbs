# Foreign keys in KDS — implementation guideline (v1)

Status: **built and enforcing.** Foreign keys are declared *and enforced*;
decisions F1–F6 govern.
Depends on: `docs/rules/keystoneid-invariant.md` (K1/K2), the dispatcher's
write paths (`InsertInner`, `UpdateInner`, `DeleteInner`), the MVCC
in-place + undo model (`heap_page.hpp`: tuple header `trx_id` +
`undo_ptr`, no xmax), core-ownership dispatch, stoppable walks
(`VisitControl`).
Interlocks with: `docs/spec/cabin.md` (the reverse check),
unique-constraint semantics (fail-fast, same family),
`docs/spec/cross-owner-txn.md` (the intent's release),
`docs/spec/namespace.md` NS10 (co-location).

Decisions:

- **F1 — FKs reference the parent's Keystone id.** The child fk column
  holds the parent's engine pk (40-bit id in a u64/int cell), never a
  business key. Consequences bought outright by K1/K2: *ON UPDATE
  CASCADE does not exist* (the referenced key is immutable), and a
  stored reference can dangle but never mis-attribute (issue-once).
- **F2 — Actions: RESTRICT / NO ACTION only.** The grammar is
  `REFERENCES <parent>` with no action clause; CASCADE and SET NULL are
  not accepted.
- **F3 — Fail-fast, no waiting.** A constraint check that meets a
  conflicting *in-flight* writer returns an error immediately (client
  retries). Blocking is not expressible on a cooperative single-writer
  core, and the deterministic-error semantic is the one unique checks
  use. The busy verdict is `FkVerdict::kBusy` → `Status::TxnConflict`,
  wire-spelled `ERR TXN_CONFLICT retryable=1` — **one retryable code
  wide**, no separate busy code. A violation is `kFkViolation`,
  `ERR FK_VIOLATION retryable=0`. Retry-versus-wrong stays
  distinguishable; the wire's `retryable` bit stays one code wide.
- **F4 — One shared check, no trigger subsystem.** No trigger framework,
  no second evaluator, no SPI-style re-entry. INSERT compiles to no step
  chain and UPDATE/DELETE walk one `Step` from the dispatcher, so the
  checks are not steps: `exec::CheckParentPresent` and
  `exec::CheckNoChildReferences` (`include/kds/exec/fk_check.hpp`) are
  called from the dispatcher's write paths (§2, §3). Two consequences:
  the step VM's probe memo does not apply, and the checks reach
  statistics by hand (§2, Statistics).
- **F5 — Co-location is advice, not a prerequisite.**
  `CheckForeignKeyColocation` admits a cross-owner pair. The forward
  check crosses at the dispatch fork (§2a) and the reverse check fans out
  (§3a). What a cross-owner pair costs, so the admission is not read as
  free: every child `INSERT`/`UPDATE` pays **one ring round trip per
  distinct parent owner** before any row work, and a parent `DELETE`
  pays a read-only collecting pass plus one probe round per child owner.
  A **namespace** is how a user asks for co-location: a pair created in
  one namespace is on one core (`namespace.md` NS10) and never crosses.
  `CREATE TABLE` emits a `WARN` line when the declared parent's owner
  differs from the child's, naming the cost and the remedy (create the
  two in one namespace); it is silent otherwise, which is what makes the
  warning mean something when it appears. The FK graph stays an input to
  placement policy.
- **F6 — Reverse check is Cabin's territory.** Parent-delete's "does any
  child reference me" is a stoppable walk that consults an active Cabin
  on the child fk column first: its verified empty set is the
  authoritative "no children" RESTRICT wants (§3). Nothing auto-creates
  that Cabin from a `REFERENCES` clause — `CREATE CABIN ON child(fk_col)`
  is the surface — and the reverse check records nothing into it: a pk
  is deleted once, so recording it would spend `cabin_max_values` on
  values no later check can ask about.

---

## 1. Catalog

`SysFkeyRow` in `include/kds/catalog/rows.hpp`, on the fixed catalog page
`kCatalogPageFkeys = 13`:

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
**no action field**, because F2 leaves exactly one action and a field
with one legal value records nothing. `Catalog::CreateForeignKey` stamps
`kFkNullable` from the child column's declared nullability; enforcement
never consults the bit (§2's NULL rule is realized by the row codec) — it
records the declaration for display.

`sys.fkeys` is a bootstrap relation: a data file without it is refused at
mount rather than read as an empty foreign-key list, because a constraint
that silently does not run is not a degraded mode.

CREATE-time validation, in `catalog::CheckForeignKeyDeclaration` and
`CheckForeignKeyColocation` (`include/kds/catalog/foreign_key.hpp`):
both relations exist; the child column is not column 0 and its type can
carry a Keystone id; **the parent is a btree relation** (below); a
duplicate FK on the same (child, column) is rejected; a cross-owner pair
is admitted (F5). They are free functions rather than `Catalog` methods
because **two doors ask the same questions**: `CREATE TABLE` checks before
the relation is created, so a refusable declaration writes nothing
(unlike a Cabin, a constraint may not degrade to a warning), and
`Catalog::CreateForeignKey` checks again because it is the door every
foreign key comes through — the argument `CreateCabin` already makes
about `NO CABIN`.

**A heap parent is refused, `NotImplemented`** (`include/kds/base/status.hpp`:
a thing a later release could build, not one the architecture cannot
admit). F1 puts the reference on the parent's Keystone id, and a heap
relation has no pk index: `LocateByPk` answers `kScan` for one, so every
child INSERT would scan the parent — the whole parent when the row is
missing, which is the case the check exists to catch. Refusing keeps a
constraint's cost a descent.

**Nothing back-checks existing rows**: a foreign key is declared only at
`CREATE TABLE`, on an empty relation; there is no `ADD CONSTRAINT`.

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

**Where.** `exec::CheckParentPresent` plus `txn::CheckVisibility` (§4),
called from `InsertInner` **before the row id is allocated** and from
`UpdateInner`'s per-row lambda when the SET list touches an fk column.
The check has the shape of a correlated EXISTS probe on the parent:

```
probe parent_rel  key = <fk value being written>
      residuals: none    semantics: EXISTS
```

**Semantics.**

- NULL fk value → check skipped (MATCH SIMPLE). Realized without reading
  `kFkNullable`: the forward check's non-integer bail passes a NULL
  through, and the row codec then stores it (column declared `NULL`) or
  refuses it by name (`NOT NULL`) — so the NOT NULL refusal is the gate.
  On the reverse side a NULL child cell matches no parent pk, so a NULL
  child never blocks its parent's delete.
- Probe finds a version → apply **check visibility** (§4): visible
  committed parent → pass; written or delete-marked by an in-flight
  foreign trx → busy (`TxnConflict`, F3); deleted-committed or not found →
  `kFkViolation`.
- Check runs **before** the heap write of the child row: on failure
  the statement aborts with no undo work. Ordering is free under
  run-to-completion; check-first is simply cheaper.

**Budget.** fk probes count into `Budget::touched()` like any other page
touches — no separate accounting.

**Statistics.** The checks are not steps, so `CommandDispatcher::
RecordFkAccess` records the shape by hand: a `kLookup` on the parent's
pk for the forward check, a `kFilterScan` (or `kCabinProbe` when the
Cabin answered) on the child's fk column for the reverse one. Both show
up in `SHOW ACCESS` beside ordinary query shapes, which is what lets an
operator compare constraint cost against query cost. An INSERT has no
plan, so ANALYZE carries no tag for the check.

## 2a. The forward check across owners — the park is at the dispatch fork

**The problem, in one sentence.** The check runs where nothing can
wait. `CheckForeignKeyOnWrite` is a plain `Status` member called per
row from inside an already-open `WriteScope`, and nothing on that
stack suspends — so a check that has to ask another core cannot ask
from where it stands.

**The answer is to move the asking, not to make the write scope
wait.** Every foreign parent pk a statement needs is extracted
**before any row work** — an INSERT's from its `VALUES`, an UPDATE's
from its `SET` body — and probed at the **dispatch fork**, the one
place a write already knows how to park: `HandleInsert` /
`HandleUpdate` return `pending_remote` and resume there. The
statement then runs **synchronously** against the intents it now
holds, and the per-row check that remains inside the write scope
answers from held intents and local state only. **Nothing inside an
open `WriteScope` ever initiates a ring round trip.**

**One round per distinct owner, never per row.** The extracted pks are
deduplicated and grouped by resolved owner; each foreign owner gets
**one** `kFkProbeRequest` carrying its whole set, and enrolling that
owner rides the same round (enrol-on-first-contact). A statement's
foreign-FK cost is a function of **how many distinct owners its parents
live on**, not of how many rows it writes — a thousand-row insert
against one foreign parent costs one round trip.

**The parent set must be enumerable.** F1 makes an fk value a literal or
a bound parameter, so the extraction pass is total. A statement whose
parent set cannot be enumerated at the fork is **refused**, with the
byte, rather than run against a partial set of intents. Fail-closed; in
code an assert-and-refuse arm, not a handled path.

**What the probe leaves behind is memory-resident, and that is safe
for a stated reason.** The probe leaves a row-scoped **reference
intent** on the parent's owner; a parent-side DELETE meeting a live
foreign intent answers busy (§3a, one code wide per F3). Under
`cross-owner-txn.md` §1a an intent-only participant writes no
`TXN_PREPARE` record, so its intents die with the process. The
invariant that makes that safe is a testable statement: **a participant
that restarts after granting an intent and before its prepare leg
forces the coordinator's transaction to fail** — the prepare cannot be
answered by a process that has lost the enrolment. A window in which
the coordinator can still commit is a defect, not a documented
limitation.

The peer-writer funding gate (`CheckWriteAffinity`) does not refuse a
write for carrying a foreign key; the cross-owner check is what
validates it.

**The load path is in scope.** `CheckForeignKeyOnWrite`'s third caller
is the KWP load path, which has its own batch boundary and takes the
same hoist there. If no park-capable seam exists in it, that path
**refuses** a cross-owner-FK write with a message naming this rule —
never a silently local-only check, which is the one degraded mode §1
says a constraint may not have.

## 2b. The intent's end — a holder is not a participant

§2a's intent is released by the transaction's **decide** and by nothing
else.

**A core that answered a probe is an intent holder, not a participant.**
The distinction is what the two lists exist to keep:

| | participant | intent holder |
|---|---|---|
| what it holds | rows of this transaction | a reference intent, and nothing else |
| how it got a context | a statement shipped to it | it answered a probe |
| the prepare | votes, and a missing context is an **abort** | is not asked |
| the decide | told | told |

An intent holder has no context to prepare with — the participant-side
context lives in `ShippedStatementExecutor::enrolled_`, which a shipped
statement fills — so the prepare goes to the participants and the decide
to the union, and a core in both lists is prepared once and decided
once.

**The separation is what keeps the join bit true.** `HasParticipant` is
what a shipped statement reads to decide whether the owner *already
holds a context* — true means it must join one rather than open a
second. An intent holder holds none, so it is recorded on its own list
and answers `false` there.

**An autocommit statement decides for itself.** Its transaction begins
and ends inside one statement, so no `COMMIT` will ever run for it. The
statement sends its own decide, after the write scope closes and never
inside it (§2a), and waits for the acknowledgement. **The autocommit
decide is sent before this core's commit record is durable**, unlike the
explicit-transaction path where the decision is durable first. That is
sound: if the coordinator dies in that window its child row is lost at
recovery, so the parent whose intent was just released has no surviving
referent.

**A holder is told it is one, per target.** The decide carries
`TxnDecideRequestPayload::intent_only`, set on the targets that hold an
intent and no rows; without it a holder's missing context is
indistinguishable from a participant's lost transaction half. A core in
**both** lists is a participant and takes the ordinary path; the bit is
a per-target fact, not a per-decide one.

**A participant coordinates its own release.** When the write that
probed was itself a *shipped* statement, the intent holder is enrolled
on the participant's context session — and the coordinator's decision is
applied by dispatching `COMMIT`/`ROLLBACK` through that session
(`shipped_statement_executor.cpp`), which forks on
`has_intent_holders()` like any other. So the participant sends its own
decide, keyed `(its core, its ship id)`, which is exactly the key the
intent carries — and the coordinator's own key, `(its core, its client
session)`, is a different one, so the two do not collide.

**Every cross-core contact mints the session's shipping identity**, not
only a ship. The identity is what an intent's holder key
`(coordinator core, session id)` is built from; a session that never
minted one would probe under id 0 and share a holder key with every
other un-shipped session on the core. `ShipStatement` and
`SendForeignKeyProbes` are the two contacts, and both mint it.

## 3a. The reverse check across owners — the fan-out

A parent's `DELETE` asks *"does any child still reference me"*, and
RESTRICT needs that answer to be **authoritative**: a "no children" that
saw only some of the children is a dangling foreign key with the
constraint reporting success, which §1 names as the one degraded mode a
constraint may not have. A child can live on another core, and this core
cannot see its rows, so the reverse check **fans out** at the dispatch
fork, before the walk — the one place a write can still park (§2a's
rule, applied to the other direction):

1. **The rows.** A bare `WHERE pk = k` names the one row. Any other
   `WHERE` is the walk's answer, and the walk cannot park — so the pks
   are **collected** first by a read-only pass under the statement's own
   snapshot, applying exactly the walk's stage-1 match and marking
   nothing. That is one extra walk of the parent per round, paid only on
   this shape and only when a child lives on another core.
2. **The registration**, before anything is asked: every pk goes into
   the coordinator-local pending-delete set, so from here on a forward
   probe for one of these rows answers in-flight and no *new* reference
   intent can be granted while the fan-out is out. Then this core's own
   intent table is asked, and a pk a foreign check is already relying on
   answers busy (`TxnConflict`, retryable).
3. **The fan-out**: one reverse probe per distinct child owner, carrying
   up to `kFkReverseProbeMaxEntries` (child, pk) questions, answered by
   the code that owner already runs for a local parent —
   `CheckNoChildReferences` with its own `core_id`, its Cabin (F6) first
   — under its own current snapshot. The child's owner records nothing
   and is enrolled in nothing.
4. **The resume** re-enters the statement with the answers held and
   **the rows fixed** — a re-entry takes the pks its first round
   collected rather than collecting again, so every round consumes one
   known set. The per-row check answers a foreign child from the held
   verdict and never falls through to a local walk; a collected pk with
   no answer — a row that became visible after the pass — is refused
   retryably (`TxnConflict`), not marked, and the retry's pass sees it.
   A resume whose set outgrows one message per owner groups only the
   unanswered pks and parks again: **one dispatch takes as many rounds
   as the set needs**, `ceil(rows × children / kFkReverseProbeMaxEntries)`
   per owner, and needs no bound because every round retires at least
   one question from a finite set. The registration made at the fork
   survives every park — a parked DELETE ends its scope without ending
   the statement — which is what keeps a "no children" answered in round
   one still true when the row is marked after round three.

Verdicts map onto the reply as onto the local check: no visible child →
clear; a committed visible child → `kFkViolation` (terminal); a row with
a foreign `trx_id` → busy (F3). §4's one-MVCC rule is untouched.

What is refused: a DELETE on the synchronous path, which has no
reactor to park on (retryable, never a local-only answer); a split child
(`docs/spec/crosscore.md` §6a); and a child owner that does not answer
within `kFkProbeReplyDeadlineNs` (retryable).

**And before the walk, the intents.** A live reference intent on the row
being deleted is evidence no local walk can produce: a transaction on
another core probed this parent, was told it exists, and is writing a
child against it — a row this core cannot see. Meeting one answers
**busy** (`TxnConflict`, retryable, one code wide per F3), because the
foreign transaction has not committed and the answer depends on how it
ends. The intent is released by that transaction's decide and by nothing
else (§2a, §2b).

## 3. Reverse check — parent DELETE

`exec::CheckNoChildReferences` is called from `DeleteInner`'s per-row
lambda, before the mark. Per incoming FK it is an existence walk over the
child relation:

```
walk child_rel
      residual: child.fk_col == <parent pk being deleted>
      stop:     VisitControl::kStop on first visible match
```

- First visible child → `kFkViolation` (RESTRICT). In-flight child
  insert encountered → busy (`TxnConflict`, F3) — the in-place row with
  a foreign `trx_id` *is* the lock record; no lock manager exists or
  is needed. A violation costs a prefix; only a pass costs the relation.
- Cost: a full child walk per deleted parent until a Cabin covers the
  fk column. The fix is declared: `CREATE CABIN ON child(fk_col)` — whose
  **verified empty set is the authoritative "no children"** RESTRICT
  wants (F6). The reverse check consults an active Cabin on the child's
  fk column **read-only**: an observed value's entry set is resolved and
  key-re-checked, and an exhausted, all-non-matching set is an
  authoritative "no children" answered **without walking**. A heap
  child with a failed hint abandons the Cabin and walks, exactly as
  `ServeFromCabin` does.

There is no reverse check for parent UPDATE: K2 makes pk update
Unsupported, so the case is closed by contract, not by code.

## 4. Check visibility — one MVCC mode, not a second implementation

Constraint checks cannot read at the statement snapshot alone: a
parent committed-deleted *after* this snapshot was taken must still
fail the check (latest-state semantics), and an in-flight writer must
be *seen* to fail fast (F3). `txn::CheckVisibility` is a sibling of the
snapshot visibility routine over the same three tuple fields, against a
read view **minted at check time** (`TransactionManager::MintReadView`)
rather than the statement's. Latest-state semantics means the answer is
the version on the page, so the check never steps back through undo:

| tuple's own version, against a freshly minted view | verdict |
|---|---|
| writer visible, not delete-marked | pass |
| writer visible, delete-marked | `kFkViolation` |
| writer not visible (in flight) | busy (`TxnConflict`) |

It cannot call `Classify` verbatim, and the case that proves it is an
in-flight *insert* of the parent: `undo_ptr == 0` with an invisible
writer, which `Classify` answers `kNoVersion` and the check must answer
**busy**, not violation. A transaction's own pending image needs no
special case — a fresh view carries `own_trx_id`, so it is visible to
its own check by the ordinary rule.

Implementation rule: this mode lives beside the snapshot visibility
routine in the same translation unit. A second, FK-private visibility
implementation is the failure mode to refuse in review.

## 5. What is deliberately absent

- No lock manager, no wait queues, no deadlock detector — F3 plus
  in-place `trx_id` makes the uncommitted row itself the conflict
  signal, and run-to-completion removes the check-to-write race that
  gap locks exist to close elsewhere.
- No ON UPDATE actions of any kind (K2).
- No cross-relation write hooks: both checks are *reads* injected into
  the writing statement's own path; FK never writes to the other
  relation.
- No trigger framework: F4 forecloses it on purpose.

## 6. Milestones

Every v1 milestone is built; the contract is §1–§5. Nothing further is
recorded here.

## 7. Out of scope

Not in this engine, all refused at the statement: composite
(multi-column) foreign keys; a reference to any column but the parent's
Keystone id (`REFERENCES p(col)` is refused with a position, F1);
`DEFERRABLE` semantics (checks are immediate); and a foreign key on a
relation of two or more ranges — an FK parent or child does not split,
and FK is refused on a split relation (`docs/spec/crosscore.md` §6a).
