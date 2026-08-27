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
  checks. New status code `kConstraintBusy` `[PROPOSED]`, distinct
  from `kFkViolation`, so clients can distinguish "retry" from
  "wrong".
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
