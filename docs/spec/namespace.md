# Namespaces — logical grouping over one global oid space

Decisions NS1–NS9. Drafted 2026-08-26 against `main` at `bf12ac3`.

This spec **ratifies and completes** something the engine already carries
rather than introducing it: `kNamespaceSys` (0) and `kNamespacePublic` (1)
are well-known oids, `kTypeNamespace` (17) is a well-known type, and both
`SysObjectRow` and `SysTableRow` already store a `namespace_oid`
(`include/kds/catalog/rows.hpp:40`, `:137`, with `offsetof` static_asserts).
`include/kds/catalog/well_known.hpp:206` already states the central rule:
*"Namespaces, types and relations share one oid space."* What is missing is
the surface (DDL, qualified names, resolution) and the normative statement
of what a namespace is **not**.

Naming: this engine calls the layer **namespace**, not schema, because
`schema` is already taken by the other meaning — `catalog/schema.hpp` holds
column definitions and `TableAccess`. Nothing in the codebase should use
one word for both.

---

## NS1 — A namespace is a name, and nothing else

A namespace groups objects for naming and (later) for privilege. It is
**not** a physical boundary, **not** an execution boundary, and **not** a
unit of recovery, backup, or placement.

Stated as exclusions, because each one is a thing other engines bind to this
layer and this engine deliberately does not:

- It does not select a file, extent, or device. Physical placement is the
  free map's and (if multi-file ever lands) the file model's concern, on an
  axis orthogonal to this one.
- It does not select a core. `owner_core` is the execution axis and is
  decided by placement policy (`docs/spec/crosscore.md` §9), independently
  of the namespace a relation is named in. Two relations in one namespace
  may sit on different cores; two relations on one core may sit in
  different namespaces.
- It does not scope a WAL stream, a checkpoint, or a snapshot. Streams are
  per core (guideline 3), and nothing about namespaces changes that.
- It does not create a query boundary. See NS4.

The one-line test for anything proposed for this layer later: **if removing
every namespace and renaming objects to be unique would change the answer,
it does not belong here.**

## NS2 — Oids are globally unique; the namespace never enters the identity

Every object oid is unique across the whole instance, for the life of the
instance, regardless of namespace. `Catalog::GenerateUserOid()` remains the
single source and keeps its existing recovery-from-highest-oid behavior;
namespaces draw from the same counter as types and relations, which is what
`well_known.hpp:206` already asserts and what `kAllWellKnownOids`'
static_assert already enforces.

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
(oid 1) is where an unqualified CREATE lands by default.

`sys` is reserved: `CREATE TABLE sys.x`, `DROP NAMESPACE sys`, and any DDL
that would add to or remove from it are refused. `public` may be used
freely but may not be dropped — resolution (NS5) names it as a fallback and
must always find it.

## NS4 — Cross-namespace queries are permitted, without qualification of the rule

A single statement may read, join, and write across namespaces. Nothing
about a name grants or withholds reachability.

This is a deliberate divergence from the engine most users will arrive
from. PostgreSQL forbids cross-*database* queries because its catalog is
physically per database and one backend cannot open two; this engine has
one catalog, so the restriction would buy nothing. The two things users
actually want from separation are name-collision avoidance (NS1 gives it)
and access control (privileges will give it) — refusing queries serves
neither while breaking legitimate joins.

**What still refuses is unchanged and is about cores, not names.** A
transaction writing two relations owned by different cores is refused with
its existing CC3 spelling and its retryable bit; that refusal must continue
to name the *relation and its owner*, never the namespace, because a
message that blamed the namespace would be false — the same two relations
in one namespace refuse identically, and in two namespaces on one core they
do not refuse at all. Autocommit single-relation statements are shipped
(SS1–SS5) regardless of namespace.

## NS5 — Resolution: qualified, else current, else `public`

A name is either **qualified** (`ns.table`) or **unqualified** (`table`).

- Qualified: resolved in exactly that namespace. No fallback. A miss is
  `NotFound` naming both parts.
- Unqualified: resolved in the session's current namespace, then in
  `public`. Two steps, in that order, and no more.

`sys` is **not** on the fallback path: catalog relations are addressed as
`sys.objects` and always have been, and adding a third silent step would
let a user relation named `tables` shadow or be shadowed by a system one
depending on creation order.

A search-path list (PostgreSQL's model) is deliberately not adopted:
ordered-list resolution makes the meaning of a name depend on session state
that is invisible in the statement, which is exactly the failure mode
`docs/rules/rules.md` guards against elsewhere by preferring explicit
constants to inferred ones. Two fixed steps are predictable and need no
session-state audit. If a search path is ever wanted, it is a strict
superset of this rule and can be added without changing what already-valid
statements mean.

## NS6 — Uniqueness is per namespace for names, global for oids

`(namespace_oid, name)` is unique among live relations. `(name)` alone is
not. Two namespaces may each hold a `orders`; they have different oids and
are different relations in every respect.

The dropped-relation tombstone participates by oid, not by name: a retyped
`kTypeDroppedTable` row keeps its `namespace_oid` for provenance, and its
name is free for reuse in that namespace — the same rule DROP TABLE already
follows, now read with the namespace in the key.

## NS7 — DDL surface, v1

- `CREATE NAMESPACE <name>` — allocates an oid, writes one `sys.objects`
  row with `type_oid = kTypeNamespace`. No pages, no relations, no
  placement decision. Refused if the name is live.
- `DROP NAMESPACE <name>` — permitted **only when empty**. No `CASCADE` in
  v1, stated as a scope decision and not an oversight: a cascade is a
  multi-relation DDL whose relations may be owned by different cores, so it
  is either a multi-core DDL or a loop that can half-succeed, and both are
  questions this spec declines to answer ahead of the DDL-transactional
  work. The refusal names the count of live objects.
- `CREATE TABLE [ns.]name` — unqualified creates in the session's current
  namespace (NS5's first step only; creation does not fall back to
  `public`, because falling back would create the object somewhere the
  user did not name).
- Qualified names are accepted anywhere a relation name is accepted:
  `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `DROP TABLE`, `CREATE INDEX`,
  and every `SHOW` that names a relation.

DDL stays system-core-only (unchanged); a namespace is a catalog row and
core 0 writes catalog rows.

## NS8 — Session state: one current namespace

A session carries exactly one current namespace, defaulting to `public`,
settable by a session statement. It affects **resolution only** (NS5) and
nothing else — not placement, not privileges once they exist beyond
resolution, not visibility.

Because a shipped statement executes on another core (SS3), the current
namespace must be resolved **before** the ship, on the arrival core, so the
owner receives a fully-qualified relation and never a session-dependent
name. This is the only interaction between namespaces and statement
shipping, and it is a binding requirement: shipping a bare name would make
the answer depend on which core's session state was consulted.

## NS9 — What the catalog stores, and what it does not

`sys.objects` gains nothing: it already carries `namespace_oid`
(`rows.hpp:40`). `sys.tables` already carries it too (`rows.hpp:137`). A
namespace's own row is a `sys.objects` row like any other object's.

No new catalog relation is introduced. `SHOW NAMESPACES` reads
`sys.objects` filtered by `type_oid`, the same way `SHOW TABLES` filters by
`kTypeTable` today.

**Nothing outside the catalog changes.** No page format, no WAL record, no
ring message, no index key, no free-map structure. If an implementation
finds itself adding `namespace_oid` to any of those, NS1 has been violated
and the design is wrong.

---

## Open, and deliberately not decided here

- **Privileges.** The namespace is the natural grant unit and this spec
  reserves it for that, but grants, roles, and their catalog rows are a
  separate feature with their own spec.
- **`DROP NAMESPACE CASCADE`** — NS7, gated on the DDL-transactional work
  and on whether multi-core DDL exists.
- **A search path** — NS5, a strict superset if ever wanted.
- **Namespace-aware placement** — whether a namespace could ever *hint*
  `owner_core`. NS1 forbids it from *deciding* placement; whether it may
  advise one is a placement-policy question (`crosscore.md` §9) and is not
  answered by making the namespace a physical thing.
- **Renaming a namespace.** `ALTER` surface generally; nothing here
  forecloses it, and NS2 means a rename changes no identity.
