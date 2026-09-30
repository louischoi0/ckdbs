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
`docs/spec/cross-owner-txn.md` (retired; the intent's release was there).

Decisions:

- **F1 — FKs reference the parent's Keystone id.** The child fk column
  holds the parent's engine pk (40-bit id in a u64/int cell), never a
  business key. Consequences bought outright by K1/K2: *ON UPDATE
  CASCADE does not exist* (the referenced key is immutable), and a
  stored reference can dangle but never mis-attribute (issue-once).
- **F2 — Actions: RESTRICT / NO ACTION only.** The grammar is
  `REFERENCES <parent>` with no action clause; CASCADE and SET NULL are
  not accepted.
- **F3 — the check fails fast; the statement waits.** A constraint check
  that meets a conflicting *in-flight* writer returns `kBusy` immediately
  — the check itself never blocks, running as it does inside a write
  path with no suspension point. **What the statement does with that
  answer changed at AO-S3**: it parks on the writer's decide and runs
  again, on the same core (AO-S3) or on the parent's (AO-S5(b)), instead
  of handing the wait to the client's retry loop. F3's original ground,
  *"blocking is not expressible on a cooperative single-writer core"*, was
  true of the engine that had no lock family and is not true of this one;
  what survives is the deterministic-error semantic, which is the one
  unique checks use, and the code the client eventually sees. The busy verdict is `FkVerdict::kBusy` → `Status::TxnConflict`,
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
- **F5 — There is no co-location to ask for.** Both checks run on the
  core the statement runs on and read every core's pages (§2a, §3a, since
  AT-S5f), and no relation is owned by a core since AT-S9, so a parent and
  a child in two namespaces cost exactly what they cost in one. What this
  decision said until then - admit a cross-owner pair, price it at one ring
  round trip per distinct parent owner, warn at `CREATE TABLE` and point at
  a namespace as the remedy - went with the owners.
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

CREATE-time validation, in `catalog::CheckForeignKeyDeclaration`
(`include/kds/catalog/foreign_key.hpp`):
both relations exist; the child column is not column 0 and its type can
carry a Keystone id; **the parent is a btree relation** (below); a
duplicate FK on the same (child, column) is rejected; any pair of
relations is admitted (F5). They are free functions rather than `Catalog` methods
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

## 2a. The forward check is hoisted, and nothing crosses (AT-S5f)

**Where the check runs.** On the core the statement runs on, whichever
core owns the parent. `CheckParentPresent` descends `parent.desc_page_id`
with no ownership question in it, and since AM-S2 step 3 that is a page
every core faults through one frame table — so the descent was already
uniform and what made a parent *foreign* was a deferral above it, not the
read below it. AT-S5f removed the deferral.

**The hoist stays, and it is not about crossing.** Every parent pk a
statement needs is still extracted **before any row work** — an INSERT's
from its `VALUES`, an UPDATE's from its `SET` body — and resolved at the
dispatch fork. Two things pay for it, and neither is a ring round trip:

- **Deduplication** (AH-R2). The extracted pks are one entry per distinct
  (parent relation, pk), so a thousand-row insert against one parent costs
  one descent and `SHOW ACCESS` reports one `kLookup`.
- **The wait has somewhere to happen.** The per-row check runs inside an
  open `WriteScope`, which is no place to park; the fork is before any row
  is written, so a statement refused there can be re-run whole.

**Nothing inside an open `WriteScope` waits or crosses**, which was
AH-R1's rule and survives it: the per-row check answers from the resolved
verdicts and local state alone. The self-referencing arm's hold (below) is
asked there and, refused, is *recorded* - the insert's own row borrow is
recorded the same way - and the statement ends to wait outside the scope.

**The parent row is held before it is read, and held to the decide**
(D9(a), AY-S5). For each distinct parent the check takes `IS` on the parent
relation and `S` on the parent row, into the transaction's borrows, and only
then descends - under a check view minted after the grant, so a writer that
decided in between is visible rather than busy. Both are released with the
transaction's other borrows at commit or abort. So a parent `DELETE`, which
takes the row's `X` before its reverse walk, waits for every open child
writer of that row on whichever core either runs (§3a).

- **`S` before the descent, never after** (AY-Q2, `[quiet-wrong]` if
  reversed): after a passing descent a whole `DELETE` of the parent fits
  before the grant, and the check would have passed a row that is gone.
  With the `S` first, a parent row held by a writer is not read at all
  until the writer decides (`FkParentHoldTest`).
- **A refused `S` is the wait.** A writer of the parent row holds its `X`
  (AO-S6a), and a parent `DELETE` with a pk window holds a range over it,
  met in the lock table's verify (AY-S2); the refused ask registers a wake
  on the entry that refused it, and the statement - which has written
  nothing - parks and runs again at the release, from whichever core
  releases. The waiter records `waiter -> holder` in the wait-for graph,
  and a wait that would close a cycle is refused naming deadlock.
- **Counted against `max_locks_per_txn`**, one `S` per distinct parent a
  transaction references, and refused `ResourceExhausted` past it: no
  escalation (AO-R10). Until AY-S5 the check asked the table with the cap
  ignored, holding nothing after the ask.
- **The self-referencing arm** is not hoisted (a parent the same statement
  writes would read "no such parent" at the fork) and checks per row. It
  takes the same pair per row - `IS`, which the write's `IX` already covers,
  then `S`, then the descent - held to the decide, and a busy parent is
  waited for rather than refused, the wait recorded as the insert's own
  row borrow records its (AY-Q3). A parent the statement wrote itself is
  its own `X`, which never refuses its own `S`. **The arm is unreachable**:
  a self-referencing key cannot be declared
  (`ForeignKeyCheckTest.ASelfReferencingForeignKeyCannotBeDeclared`).
- **Without a lock table** - a dispatcher built without one - nothing is
  held, and a busy parent is `NoteBlockingWriter`'s `IsInFlight` poll
  (AO-S3's wait, unchanged). With a table a busy verdict under a granted
  `S` does not arise; if it did, it would refuse retryably.
- **At every isolation level**, because the check view is minted at the
  check and not at `BEGIN` (`txn.md` §5): a commit makes the parent
  visible to the re-run and an abort makes the violation terminal, so
  neither arm is futile.

**It is a local descent, so it reads the transaction's own id** - and
that is a dependency worth naming, because the probe it replaced carried
the coordinator's `(session, transaction)` on the wire precisely so a
parent written by one half of a cross-owner transaction was recognised as
the *asker's own* rather than as a stranger's. AT-S5 deleted the write
ship, so a transaction writes on one core and the two ids are one id. If
a write ever ships again, this check will meet its own sibling's row,
read a foreign holder, and wait on a transaction that is waiting for it -
a cycle the detector cannot see, because both halves hold the same id.
The check would then need the coordinator's identity back.

**The parent set must be enumerable.** F1 makes an fk value a literal or
a bound parameter, so the extraction pass is total. A statement whose
parent set cannot be enumerated at the fork is **refused**, with the
byte, rather than run against a partial set.

The peer-writer funding gate (`CheckWriteAffinity`) does not refuse a
write for carrying a foreign key.

**The load path is in scope.** `CheckForeignKeyOnWrite`'s third caller is
the KWP load path, which takes the same hoist at its own batch boundary.
It never had text to re-run with and never needs any: nothing it does can
park.

### 2c. What the held `S` costs (AY-Q1, AY-Q4)

D9(a) is built as ratified, and its costs are the engine's:

- **Every `UPDATE` of a parent row waits while a child writer of it is
  open**, not only its `DELETE`: the `UPDATE`'s row `X` meets the child's
  `S`. The `S` protects the row's existence and the lock family has one
  unit per row.
- **Two transactions that each write a child of P and then update P
  deadlock**, and the second to ask is refused naming deadlock; before
  AY-S5 the two updates serialised on P's `X`. "Insert a trade, then
  update its account" is that shape
  (`FkCrossCoreRigTest.TwoChildWritersThatThenUpdateTheirParentDeadlock`).
- **Each distinct parent's `S` counts against `max_locks_per_txn`**
  beside the transaction's own row borrows, so a transaction reaches the
  cap sooner - a bulk insert whose every row names a distinct parent at
  about half the rows it reached before - and past it is refused
  `ResourceExhausted` (§2a).
- **A DDL that takes a parent relation's `X`** - `CREATE INDEX`,
  `DROP INDEX`, `CREATE ASSERTION` on it - **waits for
  every open child writer's `IS` on it**, and a steady stream of child
  writers can refuse the DDL `TxnConflict` at the lock family's 1 s fault
  net, as AO-0 item 25 accepts for `DROP TABLE` (AY-Q4).
- **An `UPDATE` that sets an fk column takes the `S` at the hoist**, before
  its walk, so one that matches no row still holds the parent it names
  until its transaction decides.
- **A check that fails gives back the `S` its own ask took** (AZ-S5,
  AZ-Q3). An absent parent answers `FK_VIOLATION`, which poisons the
  transaction: it can write nothing that `S` protects, and holding it would
  refuse every insert of that parent key until the client's `ROLLBACK`.
  PostgreSQL's `FOR KEY SHARE` locks nothing for a missing row either. An
  `S` the transaction held before the statement - a zero-row `UPDATE`'s,
  say - stays, since another statement stands on it; so does the
  relation's `IS`, which the statement's other parent rows stand under.
  The hoist records which rows its asks took (`FkParentVerdicts::Asked`) -
  an ask took the row when it appended the ledger's newest record, an O(1)
  test (AZ-S7 measured a ledger walk before each ask at +11 µs a row with
  16,384 distinct parents); the self-referencing arm tests the same, and is
  unreachable while no self-referencing key can be declared.
  **A statement that parked after its hoist keeps the `S`**: it runs again
  whole, and its first run's `S` is still in the transaction's ledger, so
  the re-run's test reads it as held before. Inside `BEGIN` - a violation
  after a wait on a child row another transaction held - the `S` then
  stays until the rollback, as it did before AZ-S5
  (`known-gaps.md`, Foreign keys).

An existence-only unit that only a `DELETE` would take is not built; it is
a change to AR2's units, and would be put forward as its own item if these
costs are measured to matter.

### 2b. What the crossing was, and what went with it

Between AH (2026-09-01) and AT-S5f the forward check across owners was a
**protocol**. A parent whose `owner_core` was not this core was deferred
into that owner's group and asked over one `kFkProbeRequest` per owner;
the owner answered a verdict and left a row-scoped **reference intent**
behind, which its own `DELETE` consulted and which the child
transaction's **decide** released. AO-S5(b) added the park: the parent's
core held a busy probe until its writer decided rather than answering
busy.

All of it is retired — the two ring kind pairs, `FkIntentTable`,
`FkPendingDeleteTable`, `fk_probe_service`, the probe park and its rounds
loop, the intent-holder list beside `participants_`, and the decide's
`intent_only` byte. **What replaces the intent is not a smaller intent:
it is the reverse check** (§3a), which now sees every child row on every
core, so an uncommitted child is evidence the parent's `DELETE` reads for
itself rather than evidence another core has to have left behind.

**Every cross-core contact still mints the session's shipping identity**,
but there is one contact now: a ship. `ShipStatement` mints it.

## 3a. The reverse check sees every child (AT-S5f)

A parent's `DELETE` asks *"does any child still reference me"*, and
RESTRICT needs that answer to be **authoritative**: a "no children" that
saw only some of the children is a dangling foreign key with the
constraint reporting success, which §1 names as the one degraded mode a
constraint may not have.

**So the check sees all of them.** `CheckNoChildReferences` walks the
child relation whole - its one heap chain from `desc_page_id`, or a btree
child descended whole. Two things stood in the way and both are gone: the
refusal for a child with a range this core did not own, and the fan-out
that replaced it (one `kFkReverseProbeRequest` per child owner, a
collect pass to name the rows, a registration to hold the window open
across the park, and as many rounds as the pk set needed).

**It runs per row inside the walk**, where the local arm always ran.
There is nothing to hoist: a check that asks nobody needs no answer in
hand before the walk starts, and a DELETE on the synchronous path runs
exactly as it does on a served one.

**The read path asks the same question since AT-S9.** It walked only the
ranges its core owned while a fan-in concatenated the rest; ranges have no
owners since AT-S9 and no relation is split since 2026-09-30
(`crosscore.md` CC8), so a read and this check walk the same structure.

Verdicts are the local check's: no visible child → clear; a committed
visible child → `kFkViolation` (terminal); a row with an in-flight
`trx_id` → busy, which the `DELETE` waits out (below), whichever core is
writing it.

**Both intervals of a child write are closed since AY-S5.** A child writer
holds the parent row's `S` from its check to its decide (§2a), and the
parent's `DELETE` takes that row's `X` before it walks, so the `DELETE`
waits for the child writer on whichever core either runs - before the
child's row is written as well as after it. What stood here was the
interval before the write, open across cores from AT-S5f, which removed the
reference intent that had closed it: a `DELETE` on one core walked an honest
empty child between another core's passing check and its row write, and
both statements reported success over a child of a deleted parent.
`FkCrossCoreRigTest.AParentDeletedBetweenAChildsCheckAndItsWriteLeavesNoOrphan`
reproduced it at `64b97e7`.

**A child writer that holds no `S` on the parent is met by the walk
instead** (AY-Q8): a child `DELETE`, an `UPDATE` of other columns, or
one moving the fk column off it. The walk answers busy and carries that writer's id and the row's pk
out; the `DELETE` parks mid-walk on the row, nothing of the parent row
written yet, and runs the check again at the writer's decide - a commit
leaves the parent unreferenced, a rollback puts the reference back and the
re-check refuses. The check's view is minted per checked row, after the
row's `X`: every child writer that set the fk to the parent has decided by
then and is visible to it, where a view taken at the statement's start
would read a child committed since as never inserted. **A row an undecided `UPDATE` moved off the parent holds
another value in its page**, so the key test alone would skip it; the walk
copies such a row out and, after the walk, reads the version before it
through the undo log, answering busy if that version references the
parent. Until AY-S5 the walk skipped it, and the writer's rollback left a
child of a deleted parent with nothing refused.

**The Cabin finds a child and clears a parent** (F6; restored at AY-S6).
A hit is authoritative and returns without walking, and so is an
**exhausted** set - every entry checked, none a live or undecided child -
which answers "no children" and records a `CabinProbe`, no walk. It rests
on three things, each of which once failed:

- **One store for the instance** (AT-S7). While the store was a
  dispatcher's own (until then; AT-R15 took the return away for it) a
  child written on another core never reached this core's set.
- **D9(a)'s `S`** (§2a). A set's count is fixed when the check reads it,
  and a child is placed before its hook appends it; a writer that sets the
  fk to the parent holds the parent row's `S` from before its descent to
  its decide, and the check runs under that row's `X`, so no such writer is
  mid-write while the set is read.
- **The banking gate and the announce** (`cabin.md` §6, §6a), on both
  builds - the serve path's and, since AY-S6, the controller's. A set
  banked while a child insert was open would lack it, the insert's hook
  having found the value unobserved; the gate declines such a bank. A
  child written behind a build's walk is appended into the announced set.
  A writer whose table access predates a `CREATE CABIN` writes without the
  hook, but it is open while it does, which the gate declines, and its
  rows are committed and visible to any later bank.

**A loop that gives the set up is not exhausted**, and the walk answers:
a heap child whose hint fails (no descent heals it, and the value is
un-observed), and a row an undecided writer moved off the parent, whose pk
stays in the set while its page holds the new value (AY-Q8).

## 3. Reverse check — parent DELETE

`exec::CheckNoChildReferences` is called from `DeleteInner`'s per-row
lambda, before the mark. Per incoming FK it is an existence walk over the
child relation:

```
walk child_rel
      residual: child.fk_col == <parent pk being deleted>
      stop:     VisitControl::kStop on first visible match
```

- First visible child → `kFkViolation` (RESTRICT). An undecided child
  row → busy, and the `DELETE` parks on that row's writer and re-checks at
  its decide (§3a, AY-Q8) - the in-place row with a foreign `trx_id` names
  the writer, and the wait is on the row's own entry in the lock table. A
  child writer that *set* the fk column to the parent - an insert, or an
  update of the column - never reaches this: its `S` refused the `DELETE`'s
  `X` first. Every other undecided writer of a referencing row does: a
  child `DELETE`, an `UPDATE` of other columns, one moving the column off.
  A violation costs a prefix; only a pass costs the relation.
- Cost: a full child walk per deleted parent. `CREATE CABIN ON
  child(fk_col)` (F6) pays for both halves of it: the reverse check
  consults an active Cabin on the child's fk column **read-only**, an
  observed value's entry set is resolved and key-re-checked, a live match
  there answers `kFkViolation` or busy without walking, and a set that
  drains answers "no children" without walking (§3a). A heap child with a failed hint, or a row an
  undecided writer moved off the parent, gives the set up and walks.

There is no reverse check for parent UPDATE: K2 makes pk update
Unsupported, so the case is closed by contract, not by code.

## 4. Check visibility — one MVCC mode, not a second implementation

Constraint checks cannot read at the statement snapshot alone: a
parent committed-deleted *after* this snapshot was taken must still
fail the check (latest-state semantics), and an in-flight writer must
be *seen* to fail fast (F3). `txn::CheckVisibility` is a sibling of the
snapshot visibility routine over the same three tuple fields, against a
read view **minted at check time** (`TransactionManager::MintCheckView`,
which registers nothing and holds no horizon) rather than the statement's.
Latest-state semantics means the answer is the version on the page, so the
check steps back through undo in one case only - the reverse check's row an
undecided writer moved off the parent (§3a), whose page holds the writer's
value; its busy verdict is still this table's:

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
its own check by the ordinary rule — on the parent's core too, where the
probe's view carries the requester's own participant there as
`own_trx_id` (§5, AO-S5(b) C1).

Implementation rule: this mode lives beside the snapshot visibility
routine in the same translation unit. A second, FK-private visibility
implementation is the failure mode to refuse in review.

## 5. What is deliberately absent

- **No gap locks.** D8 as ratified closes write skew by named units; the
  foreign key's is the parent row's `S` (§2a), and an absent parent is
  held by the same `S` on its key, which a concurrent insert of that
  parent's `X` meets - for as long as the check stands; a failed one gives
  back the `S` its own ask took (§2c).
- **Both checks wait, on the lock table** (AY-S5): the forward check on
  the parent row's `S`, the reverse check on the undecided child row's
  writer. A waiter records `waiter -> holder` in the wait-for graph and a
  wait that would close a cycle is refused naming deadlock rather than
  netted (AO-S4a, `txn.md` §5). Past the lock family's fault net with the
  holder undecided the answer is a retryable refusal, which is the net's
  shape rather than the ordinary one.
- **A transaction's own pending image answers at once**, which needs no
  special case now that the check reads the row itself: §4's `own_trx_id`
  rule sees the transaction's own uncommitted parent and answers pass or
  violation rather than parking on it. **The reverse check's view is
  minted the same way**, with the deleter's id, so a transaction's own
  child rows answer violation to its own reverse check rather than busy.
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
and `DEFERRABLE` semantics (checks are immediate).
