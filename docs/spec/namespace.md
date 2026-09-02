# Namespaces — logical grouping over one global oid space

Decisions NS1–NS10. **A namespace selects the core that owns the relations
created in it** (NS10), and that is the whole of what it adds to a name: a
qualifier **declares placement, never identity** — relation names stay
instance-global (NS5, NS6).

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

## NS1 — A namespace is a name and a placement declaration, and nothing else

A namespace groups objects for naming, and it selects the owner core of
the relations created in it (NS10). It is **not**
a physical boundary, **not** an execution boundary, and **not** a unit of
recovery or backup.

Stated as exclusions, because each one is a thing other engines bind to this
layer and this engine deliberately does not:

- It does not select a file, extent, or device. Physical placement is the
  free map's and (if multi-file ever lands) the file model's concern, on an
  axis orthogonal to this one.
- It is **not a unit of ownership**. The owning unit stays the relation; a
  namespace only decides what a relation's `owner_core` is set to at
  CREATE (NS10). Two relations in one namespace sit on one core under the
  shipped policy and may not under `placement = creating` or `rotate`.
- It does not scope a WAL stream, a checkpoint, or a snapshot. Streams are
  per core, and nothing about namespaces changes that.
- It does not create a query boundary. See NS4.

The one-line test for anything else proposed for this layer: **if removing
every namespace and renaming objects to be unique would change the answer,
it does not belong here** — placement (NS10) is the one declared exception.

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

**What still refuses is about cores, not names.** A transaction writing
two relations owned by different cores is refused with its existing CC3
spelling and its retryable bit; that refusal must continue to name the
*relation and its owner*, never the namespace, because a message that
blamed the namespace would be false — the same two relations in one
namespace refuse identically, and in two namespaces on one core they do
not refuse at all. Autocommit single-relation statements are shipped
(SS1–SS5) regardless of namespace.

## NS5 — Resolution

A name is either **qualified** (`ns.table`) or **unqualified** (`table`),
and a relation's name is **instance-global**:

- Unqualified: resolves to the one relation of that name, wherever it
  lives.
- Qualified: resolves the same relation and then **verifies** the
  qualifier against the relation's stored `namespace_oid`. A disagreement
  is `NotFound` naming both parts *and the namespace the relation is
  actually in* — the useful answer to a wrong placement assertion is the
  placement. `Catalog::CheckRelationQualifier` is the one implementation;
  `parser/ast.hpp`'s namespace-qualifier rule is the one statement of the
  rule.
- `CREATE TABLE ns.t` is the exception and the only place a qualifier
  *decides* rather than asserts: it selects the namespace, and through
  NS10 the core. An unknown namespace is refused with its byte and is
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
  placement declaration (NS10), but the namespace's core is fixed by its
  first relation, not by this statement. Refused if the name is live, and
  refused for the two reserved spellings `sys` and `public`
  (`well_known.hpp` says why each).
- `DROP NAMESPACE <name>` — permitted **only when empty**; no `CASCADE`,
  because a cascade is a multi-relation DDL whose relations may be owned
  by different cores. The refusal **names the relation** that blocked it,
  because the user's next act is to drop or move that relation. The row
  is **retyped to `kTypeDroppedNamespace` and never retired**: it is
  `GenerateUserOid()`'s floor evidence and the evidence a namespace's rank
  is derived from (NS10), so retiring it could move a namespace's core.
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

DDL stays system-core-only; a namespace is a catalog row and core 0 writes
catalog rows.

## NS8 — Session state: none

**No session carries a current namespace.** With names instance-global
(NS5, NS6) there is nothing for one to select between: an unqualified name
already reaches exactly one relation, and a "current namespace" that
changed which relation a name reached would be the session-state-dependent
meaning NS5 declines. A shipped statement therefore always carries a name
that means the same thing on every core.

## NS10 — The namespace selects the owner core

**The rule.** A relation created in a namespace is owned by the core that
owns that namespace, and a namespace's core is fixed by its **first**
relation and never rebalanced. Implemented as
`PlacementPolicy::kNamespace` in `include/kds/catalog/core_placement.hpp`,
the **shipped default**.

**How the core is derived, and why it is derived rather than stored.**
`Catalog::DeriveNamespacePlacement` reads three facts off catalog rows and
`AssignOwnerCore` decides from them; no row gains a field, so a file
written before the policy existed mounts unchanged and means what it meant.

1. **`sys` and `public` are never rotated.** The test is
   `namespace_oid >= kUserOidStart` — "did anybody declare this?" — and it
   is deliberately *not* `IsSystemNamespace`, which answers the different
   question of whether a row may be renamed or dropped. A relation nobody
   declared a namespace for is placed exactly where `placement = creating`
   places it, so an instance that writes no `CREATE NAMESPACE` sees no
   change.
2. **A namespace with relations answers with its lowest-oid relation's
   `owner_core`.** The answer is a function of rows that exist rather than
   a cached number that could drift from them, and it is what makes an
   existing file's placement survive a changed core count.
3. **A namespace with none rotates on its declaration order** — how many
   `kTypeNamespace`/`kTypeDroppedNamespace` rows on `sys.objects` precede
   it, modulo the current core count. Dropped rows are counted precisely
   because they are never retired, which makes the rank immutable and is
   how the rule survives someone emptying a namespace and refilling it.
   Because the rank is taken modulo the *current* count, an **empty**
   namespace's core can move across a mount that changes `cores`; a
   populated one's cannot (clause 2). A core the operator *states* at
   `CREATE NAMESPACE` is not the mechanism.

**What this does not claim.** It introduces no second unit of ownership:
the owning unit stays the relation (NS1). It does not make a namespace a
transaction boundary — two namespaces are two cores and a transaction over
both crosses and commits under 2PC exactly as before (NS4). It does not
rebalance. And it does not reopen the range split: the parallelism a split
would have found inside one relation is found between groups of relations
instead, with the grouping declared by the person who knows it.

**The write side.** Co-locating groups on one core has a measured
write-side throughput cost (measured against the `bench/` tree at
`1769487`) whose mechanism is not established; group-commit batching is
ruled out. It does not touch the read side the grouping exists to
accelerate, and it is never a correctness cost. `SHOW META`'s
`wal_mean_group_batch` is the core-local observable: `1.000` means every
commit on this core paid its own device sync, and reading a *peer's* needs
a session there (`peer_listeners = on`).

**The best practice is the point, not a footnote.** Relations that are
joined, foreign-keyed or read together belong in one namespace, so the
wiring is core-local; relations that have nothing to do with each other
belong in different ones, so their work runs at the same time. A foreign
key across namespaces is admitted and priced (`foreign-keys.md` §2a); one
inside a namespace never crosses at all.

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
names, an empty namespace's core across a changed `cores`, and renaming a
namespace are undecided; the decisions are not recorded here.
