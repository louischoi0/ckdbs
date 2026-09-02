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
| AF-T1 | **Built 2026-09-01.** `Catalog::CreateNamespace` / `FindNamespaceOidByName` / `ListNamespaces` / `DropNamespace`, a `sys.objects` row of `kTypeNamespace` whose `namespace_oid` is its own oid — `InitWellKnownObjects`' convention, so a user namespace and a well-known one decode alike and `IsSystemNamespace` answers false without being told. **No format change**: the row type and the field both predate this. DROP is RESTRICT and retypes to the new `kTypeDroppedNamespace` (35) rather than retiring — the row is `GenerateUserOid()`'s floor evidence, and the tombstone is a *distinct* oid from `kTypeDroppedTable` so the two sweeps cannot match each other's. Five cells in `catalog_test.cpp`. Full suite 3135/3135 green |
| AF-T2 | **Built 2026-09-02** on `worktree-workorder-wf-te-t5`. `PlacementPolicy::kNamespace` (`include/kds/catalog/core_placement.hpp`), fed by `Catalog::DeriveNamespacePlacement` off the `sys.tables` scan `CreateTable` already makes. **It is the shipped default** (`Expeditor::Config`, `kds.conf.sample`, config key `placement = namespace`), and DA2 is not reversed by that: an undeclared namespace - `public`, every relation until somebody writes `CREATE NAMESPACE` - is `kCreatingCore`'s answer, asserted by a `static_assert` and by `ExpeditorConfigTest`. **AF-P1 needed a correction to work at all**, recorded as AF-P1a below and in `namespace.md` NS10 clause 3. Cells: four in `catalog_test.cpp` (`NamespacePlacementTest`), nine `static_assert`s in `core_runtime_test.cpp`, two in `config_file_test.cpp` |
| AF-T3 | **Built 2026-09-02.** `CREATE NAMESPACE` / `DROP NAMESPACE` / `SHOW NAMESPACES`, and `ns.table` accepted at every relation name in the grammar through one production (`Parser::ParseQualifiedName`) and checked at two sites (`exec::CompileBlock`'s binding loop for every `SELECT` shape; `CommandDispatcher::QualifierRefusal` for the rest). **The naming question AF-8 left open is decided: names stay instance-global**, so a qualifier declares placement and never identity - `namespace.md` NS5/NS6 amended, NS8 declined. **`kFingerprintVersion` stays at 1**, answered explicitly rather than noticed: a keyword hashes as an identifier, `ns.t` is not a new *token* shape (the dot has lexed since the catalog views), and DDL is not patternable at all - `FingerprintTest.TheNamespaceSyntaxNeededNoFingerprintVersionBump` pins the argument and re-pins `sys.tables`' hash by value. Six cells in `command_dispatcher_test.cpp`; two pre-existing cells changed, both because the behaviour they pinned is what AF changed |
| AF-T4 | **Built 2026-09-02**, and not where AF-T4 expected. `CheckForeignKeyColocation` no longer refuses (AH-T4), so a message on it would be a message nobody reads; the advice went to the two moments a user is choosing instead - a `WARN` line on the `CREATE TABLE` that declares a cross-owner foreign key, naming both costs and the remedy, and the last clause of the parent-`DELETE` refusal in `exec/fk_check.cpp`. `foreign-keys.md` F5 carries it. Two cells in `foreign_key_test.cpp` (`ForeignKeyPlacementTest`), the second of which is also the end-to-end proof that two namespaces are two cores through the dispatcher |
| AF-T5 | **Measured 2026-09-02**, `bench/v2.8.0/results-af-t5-namespace-grouping-v2.7.0-99-g775e79d.md`. **AF-T5's own instruction could not be followed and the reason is the finding**: the DA2 cell it names has no join and no foreign key in its workload, so nothing in it ever crosses a core (its own §6a says the 0.51x collapse "is entirely the denominator"), and its driver creates every relation unqualified, so `placement = namespace` would be `creating` under another name. A new probe measures *k* **wired** groups instead. **The hypothesis holds with one qualification**: namespace placement beats single-core placement 1.12x at two groups and 1.38-1.39x at three, and beats blind rotation by 5-10% in four cells of five - but it **costs 19% at one group** (the hop with nothing to overlap it), and at three groups on eight cores it **ties** rotation on throughput (0.99x, overlapping ranges) while keeping a 4.5x better p100. So the advantage over rotation is a contention effect, not a pure crossing cost. **The owed 7-group cells were run the same day** (§3a, at `v2.7.0-105-g154df22`, with `g3-c4` re-run as the bridge across the merge): at **2.33 declared groups per writer core** the grouping is worth **1.954x** against single-core placement and **1.224x** against blind rotation - the largest margin in the sweep, and the case AF exists for. At **1.00** group per writer core it is **0.900x** of rotation. So the sweep's real finding is the *condition*: keeping a wired pair together pays once cores are shared and pays nothing while every group can have one, and **nothing in the engine reports which side an instance is on**. **A write-side cost co-location carries was found, explained, and the explanation retracted the same day** (§3b/§3c): the cost is real and measured (2.35x on a seven-group load, 1.02x under `relaxed`), the batching mechanism first given for it is false - `wal_mean_group_batch` reads 1.000 at one *and at two* committers on a core, so the batch is not what separates the layouts - and the sweep behind that is bigger than AF: **D2 does not batch below four concurrent committers on a core** (1.000/1.000/2.000/~4.000 at 1/2/4/8 sessions), so a core with one or two pays D1's device cost. Still open: anything past 2.33 groups per core, and how that write cost scales |

## AF-P1a `[RATIFIED 2026-09-02 — operator: "AF-P1a 그대로 진행"]`

The operator took the reading below as it stands. What that binds: a
namespace nobody has placed rotates on its **declaration order**, and the
alternative this section offered — a core stated by the operator at
`CREATE NAMESPACE`, through a number or a `WITH (...)` option — is **not**
taken. AF-P1's own words ("the creating core") are superseded for the
unplaced case and stand for every other clause.

## AF-P1a — the correction AF-T2 had to make, stated rather than absorbed

**AF-P1 as written cannot place anything.** It says a namespace with no
relation takes "the creating core". DDL runs on the system core and only
there — `Catalog::CreateTable` passes `kSystemCore` to `AssignOwnerCore`
literally, and `crosscore.md` CC12/CR2 make that a rule rather than an
accident — so *every* namespace's first relation would be created on core
0, every namespace would fix on core 0, and `kNamespace` would be
`kCreatingCore` under a second name.

That contradicts AF-4's own statement of what AF is: *"AF is rotation with
the grouping supplied. Same seam, same function, same `owner_core` field."*
Rotation is the half AF-P1 omitted.

**What AF-T2 built instead**, keeping AF-P1's structure and supplying the
missing half:

1. `sys` and `public` are never rotated (`oid >= kUserOidStart` is the
   test), which is what keeps DA2's behaviour for anyone who declares
   nothing;
2. a namespace **with** relations answers with its lowest-oid relation's
   `owner_core` — AF-P1 unchanged, and the clause that makes an existing
   file's placement survive;
3. a namespace with **none** rotates on its **declaration order** — how
   many namespace rows precede it on `sys.objects`, dropped ones counted
   because they are never retired, which is what makes the rank immutable
   and AF-P4 true through an emptying.

~~**The operator may want the other reading**~~ — **ratified as written,
2026-09-02** (the box at the head of this section). The alternative is
recorded rather than deleted, because it is what a later re-design starts
from: if a namespace's core should ever come from something the operator
states at `CREATE NAMESPACE` — a core number, or a `WITH (...)` option —
clause 3 is the one that changes and clauses 1 and 2 stand.

**Two names decided at AF-T1 that AF-T3 inherits.** `sys` and `public`
are **reserved**: `CREATE NAMESPACE` refuses both, and
`FindNamespaceOidByName` resolves them to `kNamespaceSys` /
`kNamespacePublic`. The grounds are not symmetry —

- **`sys` is already a qualifier**, and has been since the catalog views:
  `exec::kCatalogSchema` is `"sys"`, so `sys.tables` today resolves to a
  *view* rather than to the bootstrap relation of that name
  (`command_dispatcher.cpp`'s schema-qualified arm, which also carries the
  only "unknown schema" refusal in the engine). Admitting a user namespace
  called `sys` would create the ambiguity before AF-T3 had ruled on it.
- **`public` has no spelling at all today** — the qualifier is simply
  omitted. A user namespace of that name would be a *third* thing,
  distinct from the default, and `public.customer` would name it rather
  than the relation a bare `customer` resolves to.

The registry names stay `namespaceSys` / `namespacePublic`, which is what
`SELECT ... FROM sys.objects` shows. Two spellings for one thing, which
this codebase otherwise forbids — recorded at `well_known.hpp` rather than
quietly fixed, because converging them is user-visible and AF-T3 is where
it belongs.

**And the syntax half of AF-6 is already built.** `Parser::ParseRelationRef`
(`src/parser/parser.cpp:1181`) parses `ns.table` into `RelationRef::schema`
and has since the catalog views; `command_dispatcher.cpp:7471-7489` is the
single site that resolves it, and today answers everything but `sys` with
*"unknown schema '<x>'; the only qualified relations are the catalog views
under `sys`"*. **AF-T3 is therefore mostly that one arm plus `CREATE
NAMESPACE`**, not a grammar project — a materially smaller task than AF-6
priced it at.

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
