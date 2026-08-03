# Foreign keys in KDS — implementation guideline (v1)

Status: **GUIDELINE** — decisions F1–F6 proposed as a set; milestones
FK-M1..M6 not started.
Depends on: keystone-id-invariant.md (K1/K2, adopted), step chain +
compiler (exec/), probe memo (step_vm), MVCC in-place + undo model
(heap_page.hpp: tuple header `trx_id` + `undo_ptr`, no xmax),
core-ownership dispatch (D3), stoppable walks (VisitControl, V03).
Interlocks with: cabin.md (§reverse check), unique-constraint semantics
(fail-fast, same family).

Grounding note: this guideline is written against the current tree —
`CommandDispatcher::HandleInsert` / `HandleUpdate` exist; **a DELETE
statement does not exist yet** (delete-marking exists at the heap
layer only), which sequences the milestones below.

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

```
sys.fkeys                       [PROPOSED]
  fk_id            u64
  child_rel_oid    u64
  child_column_no  u16
  parent_rel_oid   u64
  flags            u16          (bit 0: kFkNullable — MATCH SIMPLE)
```

No parent-column field exists: F1 fixes the parent side to the
Keystone id, always. CREATE-time validation: both relations exist;
child column's type can carry a Keystone id; **owning core equality
(F5)**; duplicate FK on the same (child, column) rejected. Fixed-width
row on a catalog page, same machinery as sys.patterns.

Compiler visibility: the catalog cache exposes, per relation, its
outgoing FKs (child side) and incoming FKs (parent side) — both lists
are what the compiler and the delete path consult; neither is consulted
per tuple (per-tuple work reads the compiled chain only).

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

**Semantics.**

- NULL fk value → check skipped (MATCH SIMPLE), gated by `kFkNullable`.
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

Prerequisite reality: **the DELETE statement itself does not exist
yet** (dispatcher has no handler; only heap-level delete-marking
exists). The reverse check is therefore specified here but sequenced
after DELETE lands (FK-M3).

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

**FK-M1 — Catalog + DDL surface.** sys.fkeys row/codec + catalog
cache lists (outgoing/incoming) + `CREATE TABLE ... REFERENCES
parent` parsing + CREATE-time validation incl. colocation (F5).
Acceptance: declarable, introspectable (SHOW), rejectable
(cross-core, type, duplicate) — with tests at all three.

**FK-M2 — Forward check.** Compiler injection of the implicit kProbe
sub-chain on INSERT; check-visibility mode (§4); `kConstraintBusy` /
`kFkViolation` statuses; probe-memo batch behavior verified by test
(N-child insert = 1 descent). Acceptance: violation, busy, NULL-skip,
and batch cases green; ANALYZE (when its per-step stats land) shows
the implicit step.

**FK-M3 — UPDATE-of-fk-column + reverse check.** Extend injection to
HandleUpdate's SET analysis; implement reverse existence walk —
**blocked on DELETE statement support**, which should be scheduled as
its own prerequisite work item. Acceptance: RESTRICT blocks a
referenced parent's delete; stop-on-first-match verified via page-touch
counts.

**FK-M4 — ANALYZE integration.** Tag implicit steps in the plan
printer; per-step stats attribute fk-check cost. Acceptance: operator
can read "this INSERT spends X on FK probes" from ANALYZE output.

**FK-M5 — Cabin nomination.** When cabins land: FK declaration
registers the child fk column as a nomination; reverse check consults
an active cabin's observed set before walking. Acceptance: with a
cabin observed for the parent value, reverse check is O(entry-set)
and walk-free; empty-set RESTRICT pass verified.

**FK-M6 — (deferred) CASCADE / SET NULL.** Requires the budget
interaction design (long cascades vs run-to-completion) and
multi-relation write semantics; out of v1 by F2.

Order: FK-M1 → FK-M2 → (DELETE statement) → FK-M3 → FK-M4; FK-M5
rides the Cabin timeline. FK-M2 is independently shippable and already
delivers the highest-value half (child-side integrity) for
insert-dominated workloads.

## 7. Out of scope

- Composite (multi-column) FKs — single Keystone reference only in v1.
- Referencing non-pk unique columns (needs B2 unique secondary
  indexes first; F1 keeps v1 on engine identity).
- DEFERRABLE semantics — checks are immediate; revisit only with the
  transaction-model work.
- Cross-core FKs — placement policy work (D3 + FK graph) may later
  relax F5; v1 rejects.
