# CN-2 — Concept Note: Loose Foreign Keys and Tuple Completeness

Status: CONCEPT, not a work order, not a CIP. Nothing here opens a stage.
**No work starts on this note until the operator rules on §8.** The four
choices recorded in §4 are the operator's verbal answers to CLA's
questions; they narrow the concept and do not license building it.
Author: CLA, 2026-09-10, against `df8cc5f`
Origin: operator's verbal concept of 2026-09-10 (three statements, §0),
elaborated by CLA over two exchanges; the operator answered four of
CLA's five questions and chose the flag form over a separate interface
(§4).
Claim tags: `[operator]` for the operator's statements; `[source-read]`
with `path:line` at `df8cc5f`; everything else `[design]`. Nothing is
`[measured]`.
Relation to `docs/spec/foreign-keys.md`: this note builds nothing and
changes no decision F1–F6. Where it names them it names them to place
the concept (§5), and it proposes decisions F7–F13 for the operator to
accept, amend or refuse (§3).

---

## 0. The concept in one sentence

`[operator]` A foreign key may be declared **loose**: inserts and
deletes on either side are never refused for it, and what the engine
offers instead is **tuple completeness** — whether a given tuple's
references all resolve — as a fact the user can ask for with a command,
independent of any application logic.

`[design]` The product claim is not "foreign keys you can ignore". It is:
**a reference the engine records and can audit, without the engine
policing every write for it.** The RESTRICT foreign key the spec ships
today becomes the case where the audit is run at write time and its
failure is refused; the loose one is the same reference with the audit
deferred to when the user asks.

## 1. Why this is coherent on this engine and not elsewhere

`[source-read]` `docs/spec/foreign-keys.md:17-21` (F1): the child fk
column holds the parent's Keystone id, and `docs/rules/keystoneid-invariant.md`
K1/K2 make that id issue-once — a deleted pk is never reissued.

`[design]` That is what makes a *dangling* reference safe to keep. On an
engine that reuses keys, a child row whose parent was deleted may, after
enough churn, point at an unrelated row: the reference silently
mis-attributes. Here it cannot. A dangling reference means exactly one
thing — "the parent is gone, or never was" — and stays meaning that for
the life of the volume. The spec already states the consequence for the
RESTRICT case (F1: "a stored reference can dangle but never
mis-attribute"); a loose key is the decision to *allow* the dangle and
report it, rather than refuse it.

## 2. Definitions

`[design]` Terms this note uses, fixed here so §3 can be short.

- **Loose foreign key.** A `sys.fkeys` row with a flag set that removes
  it from both write-path checks: the forward check on child
  INSERT/UPDATE (`foreign-keys.md` §2) and the reverse check on parent
  DELETE (§3). The row remains a foreign key for every other reader of
  the catalog — placement policy, co-location warnings, `SHOW` output.
- **Tuple completeness.** A tuple is *complete* when, for every foreign
  key declared on its relation (loose or not), the fk column is NULL or
  names a parent that is live at latest state. It is *incomplete* when
  at least one fk column names a parent that is absent. It is *busy*
  when the answer for at least one column depends on an in-flight
  writer. The relation between the three is the relation between
  `CheckVerdict::kLive`, `kAbsent` and `kBusy`
  (`include/kds/txn/visibility.hpp:131-142`, `[source-read]`), lifted
  from one column to the whole tuple.
- **Completeness is a property of a tuple, not of a set.** The word
  *completeness* is already load-bearing in this tree for Cabins and
  the Waystone trail — a *set's* completeness, `docs/spec/cabin.md:334,571`,
  `docs/spec/waystone-concpets.md:35-45` (`[source-read]`). The two are
  unrelated: a set is complete when it holds every member; a tuple is
  complete when every reference it holds resolves. §8 asks whether the
  name should change to keep "one quantity, one name".

## 3. Proposed decisions — F7 to F13

`[design]` Written in the form of `foreign-keys.md`'s F1–F6 so that, if
the operator accepts them, they move into that file with no
translation. **Proposed, not decided.**

- **F7 — LOOSE is a flag on the same foreign key.** Grammar
  `REFERENCES <parent> LOOSE`. Catalog: `sys.fkeys.flags` bit 1,
  `kFkLoose`, beside `kFkNullable` at bit 0
  (`include/kds/catalog/rows.hpp:867,991`, `[source-read]`). One
  catalog row, the same referent (F1), the same placement policy and
  co-location `WARN` (F5). Not an action clause: F2 keeps the grammar
  free of `ON DELETE`, and LOOSE is not a delete-time behaviour but the
  removal of both checks. The alternative — a separate object — was
  considered and refused for three reasons: the FK graph has consumers
  that must read one catalog (F5's placement input); completeness must
  be defined for RESTRICT keys too, so it cannot be a loose-only
  feature; and a RESTRICT↔LOOSE transition on one row is an `ALTER`,
  where two objects would need DROP + CREATE with an unprotected
  interval between.
- **F8 — Completeness is computed, never stored.** No completeness bit in
  the tuple header, no hidden column, no cache. The verdict is produced
  at query time by one parent probe per fk column, through the same
  path the forward check already takes (`exec::CheckParentPresent`,
  `include/kds/exec/fk_check.hpp:99`, `[source-read]`). The stored form
  was refused on cost: every parent DELETE would have to flip the bit
  on every referencing child, and a bit flip is a tuple write — a
  `trx_id` stamp, an undo record, and a `CheckWriteConflict` against
  any transaction updating those children — which hands back as write
  conflicts the freedom LOOSE removed as refusals. A stored bit, if ever
  wanted, is a cache over this and not a replacement for it.
- **F9 — The definition.** As §2: NULL is complete (MATCH SIMPLE,
  `foreign-keys.md:155-156`, `[source-read]`; the same rule the forward
  check applies, and independent of `kFkNullable`); a live parent is
  complete; an absent parent is incomplete; an in-flight parent is busy.
  By K1/K2, incomplete and "points at the wrong row" are never the same
  state.
- **F10 — Latest state, not the statement snapshot.** The probe reads
  under a check view minted at the probe
  (`TransactionManager::MintCheckView`, `include/kds/txn/manager.hpp`,
  `[source-read]`) and `CheckVisibility`, never `Classify` and never the
  undo chain. The answer is "now", is not stable across a snapshot, and
  that is what the operator asked for: an audit of the current state.
  Inside a transaction the view carries `own_trx_id`, so a transaction's
  own uncommitted parent counts as live for its own tuples — the same
  rule `foreign-keys.md` §4 states for the check.
  **Busy is reported, not waited on, and not refused** — this is the
  one place the concept departs from F3 as amended at AO-S3
  (`foreign-keys.md:25-36`, `[source-read]`): the constraint check parks
  the *statement* on the writer's decide and re-runs, because the
  statement cannot complete without the answer. A completeness command
  is a read that can complete with "busy" as an answer, so parking it
  would be a wait nobody asked for. §8 carries the question of whether
  an opt-in wait is wanted.
- **F11 — The surface is a command.** `CHECK COMPLETENESS <table>
  [WHERE <pred>]`, emitting one row per tuple examined as
  `(pk, verdict, dangling columns)` with `verdict ∈ {complete,
  incomplete, busy}`, and by default emitting only the incomplete and
  busy rows. A pseudo-column (`SELECT ..., __complete`) was refused: it
  would put a parent probe inside the step VM, which F4 keeps the
  checks out of. An aggregate (`SHOW`-style counts) is not refused but
  is not v1. Cost is the forward check's: one probe per fk column per
  tuple, and for a cross-owner parent one ring round trip per distinct
  owner (F5, `foreign-keys.md` §2a) — the command is a walk with a
  probe per row and is priced as one.
- **F12 — RESTRICT ↔ LOOSE is an ALTER.** RESTRICT → LOOSE always
  succeeds: it removes checks and invalidates nothing. LOOSE → RESTRICT
  runs the completeness audit over the whole child relation first and
  is refused with `FK_VIOLATION retryable=0` if any tuple is incomplete;
  a busy tuple makes the ALTER itself busy (`TXN_CONFLICT retryable=1`).
  What protects the transition against a concurrent child write that
  lands after the audit and before the flag is a §8 question; the
  assertion cutover (`docs/spec/assertion.md` §8.1a, `[source-read]`)
  is the shape in the tree that answers the same question for its own
  structure.
- **F13 — Deliberately absent.** No stored completeness; no automatic
  parent→child reverse index (the user creates a Cabin on the fk column
  as today, F6, and the command may consult it as the reverse check
  does); no tracking of a later parent INSERT that "heals" a dangling
  child; no child-before-parent insertion — the child cannot know an
  engine-issued id that does not yet exist, and F1 stands; no error of
  any kind for an incomplete tuple outside F12's transition.

## 4. What the operator has already chosen, and what that fixes

`[operator]` Four answers of 2026-09-10, recorded so the proposals above
are read as narrowed by them rather than as CLA's preference:

| CLA's question | operator's answer | fixes |
|---|---|---|
| stored bit or computed at query | computed | F8 |
| NULL fk column | complete | F9 |
| statement snapshot or latest state | latest state | F10 |
| command, pseudo-column, or aggregate | command | F11 |
| flag on the same FK or a separate interface | flag | F7 |

`[design]` One question went unanswered and this note assumes the
answer: **child-before-parent insertion is not a goal.** If it is, the
flag form is wrong — the child would need a key it can choose, which is
not F1's referent — and this note's §3 should be replaced rather than
amended. §8 asks.

## 5. Placement against existing structures

`[source-read]` at `df8cc5f`, named to place the concept and not to
change anything:

- `sys.fkeys` (`include/kds/catalog/rows.hpp:844-884`): a `u16 flags`
  with bit 0 taken. Bit 1 is free.
- `exec::CheckParentPresent` (`include/kds/exec/fk_check.hpp:99`): the
  forward probe, returning `FkVerdict::{kOk, kFkViolation, kBusy}`. F8's
  probe is this function; the command would call it per fk column and
  fold the verdicts.
- `CheckVisibility` (`include/kds/txn/visibility.hpp:146-153`): the
  three-way latest-state verdict F9 lifts to the tuple. Pure in the
  three header fields, safe under a page span, walks no undo — which is
  why F10 costs no scratch copy.
- The forward check across owners (`foreign-keys.md` §2a): the probe
  ring round trip a cross-owner parent costs. F11's command inherits it
  per row.
- The reverse check and its Cabin (`foreign-keys.md` §3, F6): not used
  by the command in v1. Named because F12's LOOSE → RESTRICT audit is
  the forward direction over every child, not the reverse direction
  over any parent, so it does not need the Cabin either.
- `ALTER` (`docs/spec/alter.md`): where F12 would live; not read for
  this note beyond confirming the spec exists.

## 6. Positioning

`[design]` Deferred or "not validated" constraints exist in commercial
engines (Oracle `NOVALIDATE`, PostgreSQL `NOT VALID`, MySQL
`foreign_key_checks = 0`). Every one of them is a *loading* device: the
constraint is meant to be validated eventually, and the engine either
refuses new violations while old ones stand or trusts the operator to
turn the checks back on. None of them offers the per-tuple question
"does this row's reference resolve" as a first-class read, because none
of them can promise a dangling reference is not a mis-attributed one.
K1/K2 is what lets this engine make the reference a durable, auditable
fact rather than a temporarily unenforced rule. That is the claim, and
it is the only reason this concept is worth a note rather than a config
knob.

## 7. What this note does not license

- No code. Not the flag, not the grammar, not the command, not the
  ALTER.
- No change to `docs/spec/foreign-keys.md`. F1–F6 stand as written at
  `df8cc5f`; F7–F13 are proposals in this file and nowhere else until
  the operator moves them.
- No catalog format change. `kFkLoose` is a name in this note, not a
  constant in `rows.hpp`.
- No work order. If taken up, the work order cites this note and this
  note records that it was taken up; the work order is then the
  authority.
- No measurement claim. The cost statements in F8 and F11 are
  `[design]` reasoning from the forward check's known shape; the
  command's actual cost per row is unmeasured and the first stage of
  any work order would have to measure it before naming a number.

## 8. Operator decisions this note leaves open

1. **Go / no-go on the concept**, and if go, whether F7–F13 move to
   `foreign-keys.md` as written, amended, or in part. Until this is
   answered, nothing in §3 is a decision.
2. **Child-before-parent insertion**: not a goal (this note's
   assumption), or a goal (this note's §3 is replaced; see §4).
3. **The name.** "Completeness" already names a set property in the
   Cabin and Waystone specs (§2). Keep "tuple completeness" and rely on
   the qualifier, or choose a word with no prior tenant — *resolved*,
   *intact*, *referentially whole* — before the grammar spells it.
4. **Busy handling in the command** (F10): report `busy` as a third
   verdict and return, or offer an opt-in wait that parks on the
   writer's decide the way the constraint check does since AO-S3, or
   both.
5. **The transition guard** (F12): what stops a child write that lands
   between the LOOSE → RESTRICT audit and the flag flip. The assertion
   cutover protocol is the nearest existing shape; whether it is the
   right one, or whether a relation-level `X` borrow for the ALTER's
   duration is simpler now that M2 has closed, is the operator's call.
6. **Whether the command may consult a Cabin** on the fk column to
   answer "absent" faster, or must always probe the parent. v1 as
   proposed always probes.
