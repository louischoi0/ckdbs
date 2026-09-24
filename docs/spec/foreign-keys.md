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
verdicts and local state alone.

**A parent held by an undecided writer is waited for, on whichever core
holds it.** The row's own borrow is what the wait is taken on: its writer
holds the tuple `X` (AO-S6a), the lock table is the instance's (AO-S5(a)),
and a refused `TryAcquire` leaves a wake registration whose slot the
holder's release flips from whichever core releases. The statement then
runs again whole.

- **A refused ask acquires nothing.** No queue position, nothing in the
  transaction's holdings, only the registration — so the check takes no
  fence over the parent and holds nothing after it. That is why D9(a)'s
  `S` fence is still a separate decision and not a side effect of this
  wait.
- **A granted ask means the holder decided** between the header read and
  the ask; the borrow is released at once and the statement takes the busy
  verdict's retryable refusal, which is what a decided holder has always
  produced.
- **Without a lock table** — a dispatcher built without one — the holder
  can only be this core's, and `NoteBlockingWriter`'s per-core predicate
  is the honest wait (AO-S3's, unchanged).
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

**So the check sees all of them.** `CheckNoChildReferences` walks every
chain the child relation has — `TableAccess::WalkHeads`, one entry per
range, or `desc_page_id` unsplit — and a btree child is
descended whole. Two things stood in the way and both are gone: the
refusal for a child with a range this core did not own, and the fan-out
that replaced it (one `kFkReverseProbeRequest` per child owner, a
collect pass to name the rows, a registration to hold the window open
across the park, and as many rounds as the pk set needed).

**It runs per row inside the walk**, where the local arm always ran.
There is nothing to hoist: a check that asks nobody needs no answer in
hand before the walk starts, and a DELETE on the synchronous path runs
exactly as it does on a served one.

**The read path asks the same question since AT-S9.** It walked only the
ranges its core owned while a fan-in concatenated the rest, under a second
method (`WalkHeadsFor`); ranges have no owners and the fan-in is retired,
so a read and this check both call `WalkHeads`.

Verdicts are the local check's: no visible child → clear; a committed
visible child → `kFkViolation` (terminal); a row with an in-flight
`trx_id` → busy (`TxnConflict`, retryable, F3), whichever core is writing
it. §4's one-MVCC rule is untouched.

**A parent being written is protected by the walk**: the child row is
written before its transaction decides, the walk reads it, and an
uncommitted row answers busy. That covers the interval `[the child's
write, its decide]`.

**The interval before it is open across cores, and the intent used to
close it** (AT-S5f's own finding, from its review). The forward check
takes no borrow on a parent it passes, so between the check and the
child's write there is nothing holding the parent still:

```
core 0: DELETE p WHERE id=7 — takes the row's X, walks the child, finds
        nothing, marks, commits
core 1: INSERT INTO c VALUES (7) — passed its check a moment earlier on
        a live header, now writes the row and commits
```

and the result is a committed child referencing a deleted parent, both
statements reporting success — §1's one forbidden answer. **On one core
this cannot happen**: a statement runs to completion between the fork and
the write, so the `DELETE` has no place to interleave. Across cores two
reactors run at once and it can.

What closed it was the reference intent: the probe granted one on a pass,
and the parent's owner answered its own `DELETE` busy while one was live
(`[check, decide]`, wider than the window needs). Nothing replaces it
here, and nothing in this section should be read as claiming otherwise.
**The replacement is D9(a)'s `S` fence** — the parent's `DELETE` already
takes that row's `X` before it walks, so an `S` held from the check to
the child's decide is refused by it and the window closes exactly. D9(a)
is the following letter's (AT-0 item 6), and this window is its first
named consequence: `docs/inflight/known-gaps.md` carries it until then.

**The Cabin may find a child and may not clear one** (AT-R15, D4).
`stats::CabinStore` is a dispatcher's own and a write files its entry
into the *writing* core's store, which since AT-S5 is where the session
is rather than where the relation's owner is — so a set observed on this
core can be complete for what this core wrote and blind to what another
core wrote. A hit is therefore still authoritative and still returns
without walking; an exhausted, all-non-matching set is **not** an
authoritative "no children" and falls through to the walk. The store
becomes the instance's at AT-S7, which is what restores the fast path
rather than removing it.

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
  a foreign `trx_id` *is* the lock record this check reads. **Not because
  no lock manager exists** — there has been one since M2 — but because a row being
  written already carries its writer, and a check that asked the table
  would ask it about a row the header has already answered for. A violation costs a prefix; only a pass costs the relation.
- Cost: a full child walk per deleted parent. `CREATE CABIN ON
  child(fk_col)` (F6) pays for the **violation** half of it: the reverse
  check consults an active Cabin on the child's fk column **read-only**,
  an observed value's entry set is resolved and key-re-checked, and a live
  match there answers `kFkViolation` or busy without walking. A set that
  drains does **not** answer "no children" — §3a says why — so the pass
  case still costs the relation until AT-S7 makes the store the
  instance's. A heap child with a failed hint abandons the Cabin and
  walks, exactly as `ServeFromCabin` does.

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
check never steps back through undo:

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

- The lock family is **not consulted for the reverse check's own answer**
  (a `DELETE` meeting a child row being written is still answered `busy`;
  D9(a)'s `S` fence is the following letter's, and AO-R14 says so). The
  engine has wait queues and a deadlock detector since M2 — this bullet
  is about which answer this check takes, not about what exists — F3 plus
  in-place `trx_id` makes the uncommitted row itself the conflict signal,
  and run-to-completion removes the check-to-write race that gap locks
  exist to close elsewhere. That is true of a child on any core since
  AT-S5f: the walk reads the row's header wherever the writer is.
- **The forward check waits, and the wait is one mechanism** (AT-S5f). A
  parent being written is waited for on whichever core holds it, on the
  parent row's own entry in the instance's lock table; the waiter records
  `waiter -> holder` in the wait-for graph and a wait that would close a
  cycle is refused naming deadlock rather than netted (AO-S4a, `txn.md`
  §5). Past the lock family's fault net with the holder undecided the
  answer is the busy verdict's refusal, which is the net's shape rather
  than the ordinary one.
- **A transaction's own pending image answers at once**, which needs no
  special case now that the check reads the row itself: §4's `own_trx_id`
  rule sees the transaction's own uncommitted parent and answers pass or
  violation rather than parking on it. Forward only — the reverse check's
  view stays writerless, so a transaction's own child rows answer busy to
  its own reverse check as any writer's do.
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
