# Namespaces — logical grouping over one global oid space

Decisions NS1–NS10. **A namespace is a name, and it declares the affinity
of the relations created in it** (NS10) — a statement about which relations
belong together, which since AT-S9 nothing reads: no relation is owned by a
core, so a qualifier **declares grouping, never identity and never
placement** — relation names stay instance-global (NS5, NS6).

This spec **ratifies and completes** something the engine already carries
rather than introducing it: `kNamespaceSys` (0) and `kNamespacePublic` (1)
are well-known oids, `kTypeNamespace` (17) is a well-known type, and both
`SysObjectRow` and `SysTableRow` store a `namespace_oid`
(`include/kds/catalog/rows.hpp`, with `offsetof` static_asserts).
`include/kds/catalog/well_known.hpp` states the central rule:
*"Namespaces, types and relations share one oid space."* This document is
the surface (DDL, qualified names, resolution) and the normative statement
of what a namespace is **not**.

Naming: this engine calls the layer **namespace**, not schema, because
`schema` is already taken by the other meaning — `catalog/schema.hpp` holds
column definitions and `TableAccess`. Nothing in the codebase should use
one word for both.

---

## NS1 — A namespace is a name and an affinity declaration, and nothing else

A namespace groups objects for naming, and it declares that the relations
created in it belong together (NS10). It is **not** a physical boundary,
**not** an execution boundary, and **not** a unit of recovery or backup.

Stated as exclusions, because each one is a thing other engines bind to this
layer and this engine deliberately does not:

- It does not select a file, extent, or device. Physical placement is the
  free map's and (if multi-file ever lands) the file model's concern, on an
  axis orthogonal to this one.
- It is **not a unit of ownership, and nothing is one** since AT-S9: a
  statement runs on the core its session is on, whatever namespace its
  relations are in. It selected an owner core until then (NS10's history).
- It does not scope the WAL, a checkpoint, or a snapshot. The instance has
  one log (`docs/spec/wal.md` §3); checkpoints are per core and one runs at
  a time; nothing about namespaces changes either.
- It does not create a query boundary. See NS4.

The one-line test for anything else proposed for this layer: **if removing
every namespace and renaming objects to be unique would change the answer,
it does not belong here** — and since AT-S9 the affinity declaration passes
that test too, because nothing reads it.

## NS2 — Oids are globally unique; the namespace never enters the identity

Every object oid is unique across the whole instance, for the life of the
instance, regardless of namespace. `Catalog::GenerateUserOid()` remains the
single source and keeps its existing recovery-from-highest-oid behavior;
namespaces draw from the same counter as types and relations, which is what
`well_known.hpp` asserts and what `kAllWellKnownOids`' static_assert
enforces.

Consequences, all of them load-bearing:

- `(oid, pk)` stays forever-unique without qualification, so every advisory
  structure keyed by oid — Waystone trails, access stats, Cabin bounds —
  needs no namespace context and none of them changes.
- A namespace is itself an object with an oid, registered in `sys.objects`
  with `type_oid = kTypeNamespace`, exactly as the two well-known ones are.
- The DT2 tombstone rule extends unchanged: a dropped namespace's oid is
  never reissued, for the same reason a dropped relation's is not — the
  rows are the counter.
- **`namespace_oid` is a property of a row, never part of a key.** No index,
  no `min_key`, no page header, and no wire message gains a namespace
  field. Nothing on disk outside `sys.objects` / `sys.tables` learns that
  namespaces exist.

## NS3 — Two reserved namespaces, and what may not be done to them

`sys` (oid 0) holds the catalog and every engine-owned relation. `public`
(oid 1) is where an unqualified CREATE lands.

`sys` is reserved: `CREATE TABLE sys.x`, `DROP NAMESPACE sys`, and any DDL
that would add to or remove from it are refused. `public` may be used
freely but may not be dropped — an unqualified `CREATE TABLE` lands in it
(NS7) and must always find it.

## NS4 — Cross-namespace queries are permitted, without qualification of the rule

A single statement may read, join, and write across namespaces. Nothing
about a name grants or withholds reachability.

This is a deliberate divergence from the engine most users will arrive
from. PostgreSQL forbids cross-*database* queries because its catalog is
physically per database and one backend cannot open two; this engine has
one catalog, so the restriction would buy nothing. The two things users
actually want from separation are name-collision avoidance and access
control — refusing queries serves neither while breaking legitimate joins.

**Nothing refuses on cores any more, and a refusal never names a
namespace.** A transaction writing two relations in two namespaces is one
core's transaction like any other (AT-S5, AT-S6), and no statement is sent
anywhere by the namespace its relations are in (AT-S9).

## NS5 — Resolution

A name is either **qualified** (`ns.table`) or **unqualified** (`table`),
and a relation's name is **instance-global**:

- Unqualified: resolves to the one relation of that name, wherever it
  lives.
- Qualified: resolves the same relation and then **verifies** the
  qualifier against the relation's stored `namespace_oid`. A disagreement
  is `NotFound` naming both parts *and the namespace the relation is
  actually in* — the useful answer to a wrong qualifier is the right
  one. `Catalog::CheckRelationQualifier` is the one implementation;
  `parser/ast.hpp`'s namespace-qualifier rule is the one statement of the
  rule.
- `CREATE TABLE ns.t` is the exception and the only place a qualifier
  *decides* rather than asserts: it selects the namespace, and through
  NS10 the relation's declared affinity. An unknown namespace is refused with its byte and is
  **never created by being named**: a typo must not be indistinguishable
  from an intent.

There is no fallback resolution and no search path. A name reaches exactly
one relation (NS6), so there is nothing to search; and a search-path list
(PostgreSQL's model) is declined because ordered-list resolution makes the
meaning of a name depend on session state that is invisible in the
statement, the failure mode `docs/rules/rules.md` guards against elsewhere
by preferring explicit constants to inferred ones. `sys` is never on any
resolution path but its own: catalog relations are addressed as
`sys.objects`, so a user relation named `tables` can neither shadow nor be
shadowed by a system one.

## NS6 — Uniqueness: `(name)` alone

A relation's name is unique **instance-wide** among live relations:
`Catalog::FindTableOidByName` takes a name and no namespace, and it is the
gate, because SQL reaches a relation by name and never by oid. Two
namespaces cannot hold two relations called `orders`. A namespace is
**not a name-collision domain**, deliberately: `(namespace_oid, name)`
scoping would put a namespace argument on `FindTableOidByName` and on
every one of its callers, plus session state (NS8) to give an unqualified
name a meaning.

The dropped-relation tombstone participates by oid, not by name: a retyped
`kTypeDroppedTable` row keeps its `namespace_oid` for provenance, and its
name is free for reuse — the same rule DROP TABLE already follows.

## NS7 — DDL surface

- `CREATE NAMESPACE <name>` — allocates an oid, writes one `sys.objects`
  row with `type_oid = kTypeNamespace`. No pages, no relations. It is the
  affinity declaration (NS10), and nothing reads it. Refused if the name is
  live, and
  refused for the two reserved spellings `sys` and `public`
  (`well_known.hpp` says why each).
- `DROP NAMESPACE <name>` — permitted **only when empty**; no `CASCADE`,
  a cascade being a multi-relation DDL nobody has specified. The refusal
  **names the relation** that blocked it, because the user's next act is to
  drop or move that relation. The row is **retyped to
  `kTypeDroppedNamespace` and never retired**: it is `GenerateUserOid()`'s
  floor evidence. (It was also the evidence a namespace's placement rank was
  derived from until AT-S9 retired placement.)
- `CREATE TABLE [ns.]name` — unqualified creates in `public`, there being
  no session current namespace (NS8). A named namespace must already
  exist.
- `SHOW NAMESPACES` — NS9's listing. The two bootstrap namespaces are
  listed by their **SQL spellings** (`sys`, `public`) rather than by the
  registry names their rows carry (`namespaceSys`, `namespacePublic`),
  because what a user needs from the command is the word they would write
  in a statement; the two spellings for one thing are recorded at
  `well_known.hpp`.
- Qualified names are accepted anywhere a relation name is accepted:
  `SELECT` (`FROM` and every `JOIN`, and inside every subquery block),
  `INSERT`, `UPDATE`, `DELETE`, `REFERENCES`, `ALTER TABLE`, `DROP TABLE`,
  `CREATE INDEX`, `CREATE CABIN`, `CREATE ASSERTION`, `DESCRIBE` and
  `SHOW RELAYOUT`. Verified, never ignored (NS5). Two implementation sites
  cover all of them — `exec::CompileBlock`'s relation-binding loop for every
  `SELECT` shape, and one `CommandDispatcher::QualifierRefusal` per other
  statement.

A namespace DDL runs where its session is, as every DDL has since AT-S5:
every core writes the catalog's pages under the page latch.

## NS8 — Session state: none

**No session carries a current namespace.** With names instance-global
(NS5, NS6) there is nothing for one to select between: an unqualified name
already reaches exactly one relation, and a "current namespace" that
changed which relation a name reached would be the session-state-dependent
meaning NS5 declines. A name therefore means the same thing on every
core.

## NS10 — The namespace declares the affinity of its relations

**The rule, since AT-S9** (AR0-5 D17 and D18, AR2 E8's verb). A relation
created in a namespace is *declared* to belong with the namespace's other
relations, and that declaration is all a namespace says about execution.
**Nothing reads it today**: D18 keeps affinity as a statistic and an
optimizer hint at weight 0 (AR0 D10) until AS-E, and no statistic is
stored — `sys.tables.owner_core`, the column that carried the placement
this rule used to make, was dropped at AT-S9 (its bytes are reserved,
written 0 and ignored on read, so a pre-AT volume mounts unchanged). When a
consumer arrives, the declaration it reads is the namespace a relation was
created in, which is already on the relation's row.

**What it was, and why it went.** Until AT-S9 this read *"the namespace
selects the owner core"*: `PlacementPolicy::kNamespace`, the shipped default,
placed a relation on its namespace's core — fixed by the namespace's first
relation, rotated on declaration order before that — and every write to it
ran there. AT-S5 made a write run where its session is and AT-S6 a read, so
the owner stopped deciding where anything ran; AT-S9 retired the owner, the
policy (`core_placement.hpp`, the `placement` config key, refused by name
now) and the derivation (`DeriveNamespacePlacement`). The write-side cost
co-location was measured to carry (against the `bench/` tree at `1769487`)
went with it, and so did the reason relations had to be grouped to make a
join or a foreign key core-local: every core reads every page.

**The best practice survives as a statement of intent, not a performance
rule.** Relations that are joined, foreign-keyed or read together belong in
one namespace: that is the grouping a future affinity consumer will read,
and it is the grouping a reader of the catalog sees. A foreign key across
namespaces is admitted and costs what one inside a namespace costs
(`foreign-keys.md` §2a, §3a).

## NS9 — What the catalog stores, and what it does not

`sys.objects` gains nothing: it already carries `namespace_oid`. `sys.tables`
already carries it too. A namespace's own row is a `sys.objects` row like
any other object's.

No new catalog relation is introduced. `SHOW NAMESPACES` reads
`sys.objects` filtered by `type_oid`, the same way `SHOW TABLES` filters by
`kTypeTable`.

**Nothing outside the catalog changes.** No page format, no WAL record, no
ring message, no index key, no free-map structure. If an implementation
finds itself adding `namespace_oid` to any of those, NS1 has been violated
and the design is wrong.

---

## Open, and deliberately not decided here

Privileges, `DROP NAMESPACE CASCADE`, a search path, namespace-scoped
names, what an affinity consumer reads (D18, AS-E), and renaming a
namespace are undecided; the decisions are not recorded here.
