# Ratification AF — the namespace picks the owner core, and that is AE-8's answer

Recorded 2026-09-01 by CLA on `worktree-v2.8.0-ratification-ae` at
`975950b` (`v2.7.0-77-g975950b`), from the operator's direction of the
same day. **Operator input, not a CLA proposal.**
`ratification-ae.md` AE-8 left the version's parallelism question open and
required that "any order that proposes an answer says so in its own
title". This is that order.

## AF-1 — The direction, verbatim

> So I would like to use namespace for selecting fist created owner core
> number, it is just logical thing, 2PC still make it enables to cross
> core transaction. as best practices createing relations higly wired
> under same namespace is recommended

## AF-2 — What it says, in this engine's terms

**A namespace is a logical grouping that decides placement.** A relation
created in a namespace is owned by the core that owns that namespace,
which is the core that created the namespace's **first** relation. It
carries no physical meaning: no partitioning, no separate storage, no new
page kind.

**It changes nothing about what a transaction may touch.** Two relations
in two namespaces are two relations on two cores, and a transaction over
both crosses and commits under 2PC exactly as it does today. A namespace
is not a boundary; it is a *placement declaration*.

**The best practice is the point, not a footnote.** Relations that are
highly wired to one another — joined, foreign-keyed, read together —
belong in one namespace, so that the wiring is core-local. Relations that
have nothing to do with each other belong in different namespaces, so
that their work runs on different cores at the same time.

That is the whole mechanism, and it is why it answers AE-8: **the
parallelism a range split would have bought inside one relation is bought
instead between groups of relations, with the grouping declared by the
person who knows it.**

## AF-3 — What the tree already has (source-read at `975950b`)

The survey's finding is that AF is mostly *wiring what exists*, and that
matters for how much of it is a format question. Nothing below is
proposed; all of it is in the tree today.

| fact | site |
|---|---|
| `SysTableRow` **already persists `namespace_oid`**, 8 bytes at offset 8, with an `offsetof` static_assert pinning it | `include/kds/catalog/rows.hpp:60`, `:137`, `:155` |
| So does `SysObjectRow` | `rows.hpp:34`, `:40`, `:51` |
| A namespace **is** an object — `kTypeNamespace = 17`, and "a namespace's own namespace is itself" | `include/kds/catalog/well_known.hpp:19`, `src/catalog/catalog.cpp:474-494` |
| Two namespaces exist: `kNamespaceSys = 0`, `kNamespacePublic = 1`, registered at bootstrap | `well_known.hpp:20-21`, `catalog.cpp:493-494` |
| `Catalog::CreateTable` **already takes a `namespace_oid` as its first parameter** | `include/kds/catalog/catalog.hpp:446` |
| …and every caller passes `kNamespacePublic` hard-coded | `src/server/command_dispatcher.cpp:2150`, `:4073` |
| Placement is already a policy with a seam: `PlacementPolicy { kCreatingCore, kRotate }` and `AssignOwnerCore(policy, creating_core, core_count, relation_seq)` | `include/kds/catalog/core_placement.hpp:100-112` |
| `sys.tables`'s diagnostic view already returns `namespace_oid` as a column | `src/exec/catalog_view.cpp:53`, `:68` |

**The consequence worth stating first: AF needs no format bump.** The
field it wants is on disk, asserted at its offset, and written on every
relation created since the format existed. A file written yesterday
mounts under AF and means exactly what it meant — the same sentence the
key-order byte earned in 2026-08-25.

## AF-4 — Why DA2 does not contradict this, and in fact argues for it

`kRotate` exists and **lost**. DA2 (2026-08-31,
`instructions/v2.7.0/ratification-da.md`) ratified `kCreatingCore` as the
shipped default on a measurement, not a preference:
`bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md` §6 —
rotation's crossover is a step at the first core to take a second
session, and **past it rotation is negative at seven writer cores
(0.51×)**.

Read the reason rather than the number: rotation scatters relations
**blindly**. It has no idea which two relations are joined every second
and which two never meet, so it splits the first pair as readily as the
second, and every join it splits pays a crossing. It was not too much
parallelism; it was parallelism spent in the wrong place.

**AF is rotation with the grouping supplied.** Same seam, same function,
same `owner_core` field — the difference is that the person who knows
which relations are wired together says so, once, at `CREATE`. That is
information the engine cannot derive and never had. So AF does not
reverse DA2; it supplies the input whose absence produced DA2's 0.51×,
and `kCreatingCore` remains the right default for anyone who declares
nothing.

## AF-5 — CLA's proposals, offered for the operator's word

Under AE this version's orders take the SA convention: a decision CLA
must make is written down as a proposal with its reasoning, not assumed.

**AF-P1 — The namespace's core is its first relation's core, and it is
derived, not stored.** `AssignOwnerCore` gains a `kNamespace` policy: at
`CREATE TABLE`, look up the relations already in this `namespace_oid` and
take the `owner_core` of the lowest-oid one; if the namespace has none,
the creating core owns it and this relation is what fixes it. **Reasoning:
it is the operator's sentence executed literally ("first created owner
core number"), it needs no new field on any row, and it cannot disagree
with itself — the answer is a function of rows that already exist rather
than a cached number that could drift from them. The cost is a catalog
scan on the DDL path only, which is where the engine already scans for a
name collision.**

**AF-P2 — A namespace is a `SysObjectRow` of `kTypeNamespace`, and users
may create one.** The two well-known namespaces stay exactly as they are;
a user namespace is an ordinary object row with an oid from
`kUserOidStart`. **Reasoning: the row type, the type oid and the
self-referential `namespace_oid` convention all exist and are used by the
bootstrap; adding a second way to spell a namespace would be the "no
second name for one concept" rule broken on its first use.**

**AF-P3 — `namespace_oid != kNamespacePublic` is the idiom for "a system
relation", and a third namespace breaks it in two different directions.**
Four sites use it. They are **not** one change, and the difference is the
whole of why this task lands first:

- **Three are refusals that would become wrong.** `ALTER … RENAME`
  (`src/catalog/catalog.cpp:1635`), `DROP TABLE` (`:1717`) and the
  dispatcher's ALTER guard (`src/server/command_dispatcher.cpp:2824`) all
  answer *"'x' is a system relation and cannot be renamed/dropped/
  altered"*. Create a relation in namespace `orders` and it becomes
  **undroppable and unrenamable**, with a message that is false about
  what it is. These must become `== kNamespaceSys` (or an explicit
  system-oid test) **before any user namespace can exist**.
- **One is a gate that is deliberately the other way, and stays.**
  `src/server/range_alloc.cpp:89-97` chose `!= kNamespacePublic` on
  purpose, and says so: *"the two agree only while `sys` and `public` are
  the only namespaces, and the difference between them is which way a
  third one fails. **A gate must fail closed.**"* Under a third namespace
  it over-declines — a user relation in `orders` is refused a split — and
  that is the correct failure for a gate, doubly so under AE-3.2, which
  splits nothing. **Do not "fix" this one for symmetry**; changing it
  would convert a deliberate fail-closed into a fail-open, which is the
  trade AE-5.1 forbids.

**Reasoning: the shared spelling hides that these four sites want
opposite behaviour under a third namespace, and a blanket
search-and-replace would either leave three false refusals or open a
gate. Splitting them is the prerequisite, not a cleanup, and it is
buildable today — before the syntax question is answered, because it
needs no new syntax to be right.**

**AF-P4 — Placement is decided once, at `CREATE`, and never rebalanced.**
`SysTableRow`'s own comment already promises this ("Assigned once, at
CREATE… never rebalanced, which is what makes it cacheable on
`TableAccess`"). AF does not weaken it: moving a relation between cores
because its namespace moved would make ownership a fact that changes
without DDL, which `catalog_cache.hpp`'s rule forbids caching. **A
namespace's core is therefore immutable once its first relation exists**,
and a `DROP` of every relation in a namespace does not free the namespace
to move — stated now, because "the namespace is empty again" is exactly
the case someone will reach for later.

**AF-P5 — The FK colocation refusal becomes satisfiable, and stays.**
`CheckForeignKeyColocation` (`src/catalog/foreign_key.cpp:31`) refuses a
parent and child on different cores. Today a user has **no way** to
comply except by luck of creation order; under AF, "put them in the same
namespace" is the compliance instruction, and the refusal's message
should say so. **Reasoning: AE-6 makes the cross-core FK this version's
work, and AF answers a large part of it by placement rather than by
protocol — a constraint that never needs to cross is cheaper than one
that crosses correctly. It does not close the protocol question, because
two relations *already* on two cores still cannot be linked; it makes the
refusal actionable instead of arbitrary.**

## AF-6 — The syntax `[DECIDED 2026-09-01 — operator: shape (a)]`

> **`CREATE NAMESPACE <name>;` plus qualified names `ns.table`.** An
> unqualified name still means `public`, so every statement written
> before AF means exactly what it meant.

The operator took shape (a) below. What that binds:

- `CREATE NAMESPACE` is a new keyword, and the qualified `ns.table` form
  is a new name shape — **both fingerprint-visible**, so AF-T3 answers
  `kFingerprintVersion` explicitly under `fingerprint.hpp`'s bump rule
  with the golden corpus re-pinned, rather than discovering the question
  when a stored waystone stops matching.
- A qualified name is resolvable **everywhere a relation is named**, not
  only at `CREATE TABLE`: `FROM`, `JOIN`, `INSERT INTO`, `UPDATE`,
  `DELETE FROM`, `REFERENCES`, `CREATE INDEX ON`, `ALTER TABLE`, `DROP
  TABLE`, and every `SHOW` that takes a relation.
- **Implicit creation is refused** — `CREATE TABLE orders.customer` with
  no `orders` namespace is an error naming the byte, not a namespace
  springing into existence. That is the whole reason (a) was preferred
  over (b): a typo must not be indistinguishable from an intent.

The three shapes as they were put, kept because the argument for (a) is
an argument against the other two:

CLA does not pick this, and the reason is a rule rather than caution:
**nothing new is reserved lightly** — keywords hash as identifiers and
`kFingerprintVersion` moves only under `fingerprint.hpp`'s bump rule,
with the golden corpus pinning it. A new keyword and a new name form are
both fingerprint-visible.

Three shapes, with what each costs:

- **(a) `CREATE NAMESPACE <name>;` plus qualified names `ns.table`.**
  Explicit, standard-looking, and the namespace exists before anything is
  placed in it — so "the first relation fixes the core" is the only rule
  needed. Costs: one keyword, and a qualified-name form in every place a
  relation is named (the parser's identifier path, not just `CREATE`).
- **(b) `CREATE TABLE ns.name (...)`, the namespace created implicitly on
  first use.** No `CREATE NAMESPACE`, one grammar change instead of two.
  Costs: a typo silently creates a namespace, and the engine cannot tell
  the two apart — which is the failure mode implicit creation always has.
- **(c) `CREATE TABLE name (...) WITH (namespace = 'ns')`.** Rides V11's
  unbuilt `WITH (...)` table options, which the per-relation spreading
  flag also wants, so two features would land on one grammar addition.
  Costs: V11 is unbuilt, so this is gated on building it; and a
  placement declaration reads oddly as a table *option* when it is really
  part of the name.

CLA recommended (a) on the grounds that placement is a naming fact and
should be visible in the name a query writes, and that implicit creation
(b) makes a typo indistinguishable from an intent. **The operator took
(a)**, which is what the box at the head of this section records.

## AF-7 — Tasks, in dependency order

**AF-T0 — the system-relation predicate, split from the fail-closed
gate.** The three DDL refusals get a predicate that means what it says;
`range_alloc`'s gate keeps its deliberate `!= kNamespacePublic` and gains
a sentence saying it is now knowingly over-declining rather than agreeing
with the other three (AF-P3). Cells: a user relation outside `public` is
renamable, droppable and alterable; the same relation is still declined a
split. **This lands before any user namespace can exist**, and it is
buildable today, before the syntax question is settled. Gate: none.

**AF-T1 — the namespace as a catalog object.** `SysObjectRow` of
`kTypeNamespace` with a user oid, created inside the DDL transaction like
any other catalog object, name-unique, `DROP` refused while any relation
is in it (RESTRICT, the rule `DROP TABLE` already uses for fkeys and
assertions). Gate: AF-T0.

**AF-T2 — `PlacementPolicy::kNamespace`.** AF-P1's derivation, wired
through `AssignOwnerCore`'s existing seam; `kCreatingCore` stays the
default for a relation created in `public`. Gate: AF-T1.

**AF-T3 — the syntax.** Whichever of AF-6's shapes the operator names,
plus resolution of a qualified name everywhere a relation is named, plus
the `kFingerprintVersion` question answered explicitly rather than
noticed later. Gate: AF-6's decision.

**AF-T4 — the FK message.** `CheckForeignKeyColocation`'s refusal names
the namespace remedy (AF-P5). Gate: AF-T3.

**AF-T5 — measurement.** The cell that matters is the one DA2's 0.51×
came from, re-run with the grouping supplied: a workload of *k*
independent relation groups, one namespace each, against the same
workload in one namespace and against `kCreatingCore`. **The hypothesis
to falsify: namespace placement recovers rotation's parallelism without
rotation's crossing cost, because no join in the workload crosses.** If
it does not, AF is a convenience and not an answer to AE-8, and the
results file says so. `bench/v2.8.0/`, `git describe --tags`,
interleaved arms. Gate: AF-T3.

## AF-8 — Row status (CLA, appended as rows land)

| row | status |
|---|---|
| AF-T0 | **Built and reviewed 2026-09-01** on `worktree-v2.8.0-ratification-ae`. `catalog::IsSystemNamespace` (`include/kds/catalog/well_known.hpp`) carries the identity question; the three DDL refusals call it; `range_alloc.cpp`'s gate keeps `!= kNamespacePublic`. **Each of the four guards was reverted on its own and the cell that names it failed** — the dispatcher's (the `RENAME COLUMN` line, the form where it is the sole door), `RenameTable`'s (the `RENAME TO` line, which the column line then proves was not the dispatcher's), `DropTable`'s, and the gate's (converted for symmetry, and the split opened at page 131). `CatalogTest.ACatalogsOwnRelationIsStillRefusedARenameAndADrop` holds the converse. Full suite 3131/3131 green; overhead not measured (v2 suspension) |
| AF-T1..T5 | not started |

**What the review established and AF inherits.** `critics-developer`,
2026-09-01, against `e79e1bc`:

- **No fifth site.** Two *other* idioms for "is this a catalog relation"
  exist and both stay correct under a third namespace: the oid-range test
  (`mount_recovery.cpp:130`, `relayout_planner.cpp:87`/`:207`,
  `sim/integrity.cpp:162` — a user-namespace relation's oid still comes
  from `GenerateUserOid()`, above `kUserOidStart`), and the anchor-page
  test (`catalog.cpp:2455`, `:3337` — structural, namespace-independent).
  Nothing else reads `namespace_oid` in a decision; `SHOW TABLES`,
  `ListTables` and `catalog_view.cpp` filter on `type_oid` alone, which
  is what makes a user-namespace relation listed and resolvable today.
- **The gate's own decline message was still telling the lie AF-T0 took
  out of the other three** — it logged "a catalog relation's pages and
  chain head are core 0's by construction" for a user-namespace relation
  too. Fixed; the `why` now names both populations and asserts neither of
  the other.
- **The fail-closed argument was overstated and is now stated truthfully.**
  There *is* an exact spelling of the gate's scope — `rel_oid <
  kUserOidStart`, the idiom three other files use — which would also
  delete the `GetSysTableRow` scan the gate makes for one field. It is not
  taken because it is strictly *narrower*, so adopting it would **loosen**
  a gate, which AE-5.1 forbids. The reason to keep the coarse test is the
  AE programme, not the absence of a precise one, and the comment says so
  rather than leaving the next reader to re-derive a dilemma that does not
  exist.
- **`RenameColumn` has no namespace guard of its own** (`catalog.cpp:1655`),
  so the dispatcher is the *sole* door for that form where `RENAME TO` has
  two. Recorded rather than changed: AF-T0 narrows the one door in the
  safe direction, and the asymmetry is AF-T3's to resolve if a namespace
  ever scopes names.
- **`TableAccess::namespace_oid` is write-only today** — set at
  `catalog.cpp:2005`, read by no engine code. Kept, because AF-T2 is its
  first reader; said at the field so that is a decision rather than a
  discovery.

**What AF-T0 found and AF-T3 inherits:** `Catalog::FindTableOidByName`
(`include/kds/catalog/catalog.hpp:468`) takes a name and **no namespace**,
and its own comment calls itself "the gate" — SQL reaches a relation by
name and never by oid. So today a relation in namespace `orders` is
reachable by the bare name `orders_line`, which is what let AF-T0's cells
run with no syntax at all. Under AF-T3 that is a question rather than a
convenience: either names stay instance-global and a namespace is
placement-only, or names become namespace-scoped and this function grows
a namespace argument along with every caller. **The first is smaller and
is what AF-2 describes** — a namespace decides *where a relation lives*,
not *what it is called* — but it means two relations in two namespaces
still cannot share a name, which is the thing people usually expect a
namespace to buy. Named here so AF-T3 decides it deliberately.

## AF-9 — What this does not claim

It does not partition anything, and it introduces no second unit of
ownership: the owning unit stays the **relation**, and a namespace only
decides what that relation's `owner_core` is set to at `CREATE`. It does
not make a namespace a transaction boundary, a recovery unit, a security
scope, or a name-collision domain beyond what AF-T1's uniqueness rule
says. It does not rebalance (AF-P4). It does not reopen the range split
— AE-3.2 stands, and AF is the *reason* it can stand: the parallelism is
found between relations instead of inside one.
