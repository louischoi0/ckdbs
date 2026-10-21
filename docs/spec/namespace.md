# Namespaces — logical grouping over one global oid space

Decisions NS1–NS10. **A namespace is a name-collision domain, and it
declares the affinity of the relations created in it** (NS10) — a
statement about which relations belong together, which nothing reads: no
relation is bound to a core, so a qualifier **selects a name, never
identity and never placement**. A relation's name is unique within its
namespace (NS6); an unqualified name means the session's namespace, set by
`USE` (NS5, NS8). Its identity is still its oid (NS2).

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
- It is **not an execution unit**: a statement runs on the core its
  session is on, whatever namespace its relations are in (NS10).
- It does not scope the WAL, a checkpoint, or a snapshot. The instance has
  one log (`docs/spec/wal.md` §3); checkpoints are per core and one runs at
  a time; nothing about namespaces changes either.
- It does not create a query boundary. See NS4.

The one-line test for anything else proposed for this layer: **if removing
every namespace and renaming objects to be unique would change the answer,
it does not belong here** — and the affinity declaration passes that test
too, because nothing reads it. The name a relation is reached by is the
pair `(namespace, name)`, which a unique rename preserves.

## NS2 — Oids are globally unique; the namespace never enters the identity

Every object oid is unique across the whole instance, for the life of the
instance, regardless of namespace. `Catalog::GenerateUserOid()` remains the
single source and keeps its existing recovery-from-highest-oid behavior;
namespaces draw from the same counter as types and relations, which is what
`well_known.hpp` asserts and what `kAllWellKnownOids`' static_assert
enforces.

Consequences, all of them load-bearing:

- `(oid, tuple key)` stays unique without a namespace qualification - unique at
  any instant, and for life up to a `PURGE` that frees a key, which every
  advisory structure re-checks against the row now there
  (`heap-and-tuple.md` §4.1c) - so every advisory structure keyed by oid -
  Waystone trails, access stats, Cabin bounds - needs no namespace context
  and none of them changes.
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

**Nothing refuses on cores, and a refusal never names a namespace.** A
transaction writing two relations in two namespaces is one core's
transaction like any other, and no statement is sent anywhere by the
namespace its relations are in.

## NS5 — Resolution

A name is either **qualified** (`ns.table`) or **unqualified** (`table`), and
a relation's name is the pair `(namespace, name)` (NS6):

- Qualified: the qualifier selects the namespace and the name is looked up
  in it alone. An unknown namespace is `NotFound` naming it, with its byte.
- Unqualified: means `default.table`, where `default` is the session's
  namespace (NS8) - `public` until a `USE`, and `public` for every caller
  with no session (a KWP load, an engine-internal statement).
- **There is no search.** One namespace is asked; a relation of the name in
  another namespace is not found. The miss is `NotFound`, `no relation named
  '<as written>' (byte N)`, and **names every namespace that does hold the
  name** - `no relation named 't' in namespace 'public'; 't' is in namespace
  'orders' - write orders.t (byte 14)` - because writing the qualifier is
  the user's next act.
- `CREATE TABLE ns.t` creates in `ns`; `CREATE TABLE t` creates in the
  session's namespace. An unknown namespace is refused with its byte and is
  **never created by being named**: a typo must not be indistinguishable
  from an intent.
- **`sys` never resolves for a statement.** Its relations are catalog views,
  read as `SELECT ... FROM sys.<view>` on the view path, which never reaches
  this resolution; every other statement naming `sys.x` is `Unsupported`,
  and an unqualified catalog name (`tables`) is a miss in the session's
  namespace that says `read it as sys.tables`. A catalog relation has no
  column rows a statement could bind, which is why the unqualified name used
  to reach `no columns for this rel_id`.
- Every place a relation is named resolves through
  `Catalog::ResolveRelation` - `exec::CompileBlock`'s binding loop for every
  `SELECT` shape (FROM, every JOIN, every subquery block), each write's and
  DDL's handler, `REFERENCES`, `DESCRIBE`, `sys.columns`' `rel_name` key,
  and the KWP load's relation (`ns.t` or `t`).

**Why a session namespace and not a search path.** A search path (an ordered
list) makes the meaning of a name depend on which namespaces exist and on
their order; one namespace makes it depend on one word the session set, and
a miss can always say which qualifier to write. The cost is the one NS8
names: an unqualified name's meaning is session state.

**Persisted declarations are not re-resolved by name.** An assertion, a
Transition and a foreign key store their relations' oids; a re-read of the
declaration text binds by oid, so neither a `USE` nor a second relation of
the same name elsewhere changes what they govern.

**Advisory structures are untouched** (NS2): a statement's fingerprint is
its text, so `SELECT * FROM t` under two `USE`s is one pattern over two
relations. That shares a `sys.patterns` row - and so one `WAYSTONE ON`
opts the text in under every namespace (`waystone-concpets.md` §4d) - and
costs a miss, never an answer: every trail entry carries its `rel_oid` and replay skips one that is
not the step's (`exec/trail_replay.cpp`).

## NS6 — Uniqueness: `(namespace, name)`

A relation's name is unique **within its namespace** among live relations.
Two namespaces may each hold a relation called `orders`; each is reached by
its own qualifier, or unqualified from a session whose namespace it is.
`Catalog::FindTableOidByName` takes the namespace and the name, and the name
cache is keyed by the pair.

The binding check (`catalog.md` CT7) asks a relation's name in its
namespace alone, under `sys.objects`' root-page hold: `CREATE TABLE` in the
namespace it creates in, `ALTER TABLE ... RENAME TO` in the relation's own.
The dropped-relation tombstone keeps its `namespace_oid`, so an undecided
drop holds its name in that namespace only.

A namespace's own name, an index's, an assertion's, a Transition's and a
Cabin's stay **instance-wide**: only relation names are scoped.

**Every catalog view that names a relation names its namespace beside it**
(a `namespace` column, `child_namespace` / `parent_namespace` in
`sys.fkeys`, `references_namespace` in `sys.columns`), as the SQL spelling:
a name alone no longer identifies a relation to a reader of the catalog.
`manual/sql/sql.md` §6 lists them. A view keyed by a relation name resolves
a bare key in the namespace a `namespace = '<literal>'` conjunct names, else
in the session's.

## NS7 — DDL surface

- `CREATE NAMESPACE <name>` — allocates an oid, writes one `sys.objects`
  row with `type_oid = kTypeNamespace`. No pages, no relations. It is the
  affinity declaration (NS10), and nothing reads it. Refused if the name is
  live, and for the two reserved spellings `sys` and `public`
  (`well_known.hpp` says why each).
- `DROP NAMESPACE <name>` — permitted **only when empty**, asked under the hold a
  create's membership is written under (`catalog.md` CT7, AT-S17b); no `CASCADE`,
  a cascade being a multi-relation DDL nobody has specified. The refusal
  **names the relation** that blocked it, because the user's next act is to
  drop or move that relation. The row is **retyped to
  `kTypeDroppedNamespace` and never retired**: it is `GenerateUserOid()`'s
  floor evidence.
- `CREATE TABLE [ns.]name` — unqualified creates in the session's
  namespace (NS8). A named namespace must already exist.
- `USE <namespace>` — sets the session's namespace (NS8).
- `SHOW NAMESPACES` — NS9's listing. The two bootstrap namespaces are
  listed by their **SQL spellings** (`sys`, `public`) rather than by the
  registry names their rows carry (`namespaceSys`, `namespacePublic`),
  because what a user needs from the command is the word they would write
  in a statement; the two spellings for one thing are recorded at
  `well_known.hpp`.
- Qualified names are accepted anywhere a relation name is accepted:
  `SELECT` (`FROM` and every `JOIN`, and inside every subquery block),
  `INSERT`, `UPDATE`, `DELETE`, `PURGE`, `REFERENCES`, `ALTER TABLE`,
  `DROP TABLE`, `CREATE INDEX`, `CREATE CABIN`, `CREATE ASSERTION`,
  `CREATE TRANSITION` and `DESCRIBE`. Each resolves as NS5
  states.

A namespace DDL runs where its session is, as every DDL does: every core
writes the catalog's pages under the page latch.

## NS8 — Session state: the session's namespace

**Every session carries one namespace**, the one an unqualified relation
name means (NS5) and an unqualified `CREATE TABLE` lands in. It starts as
`public` and is set by `USE <namespace>`:

- The namespace resolves under the session's view, so one this
  transaction created can be used. An unknown one is `NotFound` at its
  byte; anything but one identifier (and one trailing `;`) is
  `InvalidArgument`; `USE sys` is refused, its relations being read only
  qualified (NS5).
- The reply is `USE namespace=<name>`. A refused `USE` leaves the session
  where it was.
- **Session state, not transactional**: admitted inside a transaction, it
  changes what a name written later means and no view; a `ROLLBACK` does not
  undo it. It is routed as `SET` is (`DispatchInner`), is never
  fingerprinted, and needs the readonly role.
- A statement reads the namespace once, when it starts
  (`exec::StatementContext::default_namespace`), so a parked write resumes
  against the namespace it began in.
- A namespace dropped after the `USE` leaves the session in an oid that
  resolves nothing: every unqualified name is a miss and an unqualified
  `CREATE TABLE` is refused by the namespace-live check, until the next
  `USE`. `DROP NAMESPACE` does not ask which sessions use it.
- The session moves whole between cores (`crosscore.md`), so its namespace
  goes with it, and a name means the same thing on every core.

## NS10 — The namespace declares the affinity of its relations

**The rule** (AR0-5 D17 and D18, AR2 E8's verb). A relation created in a
namespace is *declared* to belong with the namespace's other relations, and
that declaration is all a namespace says about execution. **Nothing reads
it today**: D18 keeps affinity as a statistic and an optimizer hint at
weight 0 (AR0 D10) until AS-E, and no statistic is stored —
`sys.tables.owner_core` is a reserved word, written 0 and never read. When
a consumer arrives, the declaration it reads is the namespace a relation
was created in, which is already on the relation's row. A namespace selects
no core: every core reads every page, and the `placement` config key is
refused by name.

**The best practice is a statement of intent, not a performance rule.** Relations that are joined, foreign-keyed or read together belong in
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
index key, no free-map structure. If an implementation
finds itself adding `namespace_oid` to any of those, NS1 has been violated
and the design is wrong.

---

## Open, and deliberately not decided here

Privileges, `DROP NAMESPACE CASCADE`, what an affinity consumer reads (D18,
AS-E), renaming a namespace, and moving a relation between namespaces are
undecided; the decisions are not recorded here. A search path is declined
(NS5).
