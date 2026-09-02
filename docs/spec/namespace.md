# Namespaces — logical grouping over one global oid space

Decisions NS1–NS10. Drafted 2026-08-26 against `main` at `bf12ac3`;
**amended 2026-09-02 by the operator's ratification AF**
(`instructions/v2.8.0/ratification-af-namespace.md`) and built at AF-T1,
AF-T2 and AF-T3 on `worktree-workorder-wf-te-t5`.

> **What AF changed in this file, stated at the top because it reverses the
> spec's central exclusion.** This document was written before the operator
> decided what a namespace is *for*. AF decides it: **a namespace selects
> the core that owns the relations created in it** (AF-2), which is the
> thing NS1 listed as a non-goal and the "Open" list at the foot called
> undecided. NS10 below is that rule and it governs.
>
> AF also **declines the naming half** this spec assumed. AF-9 says a
> namespace is not a name-collision domain, so relation names stay
> instance-global: NS5's fallback, NS6's per-namespace uniqueness and NS8's
> session current namespace are **not built and not this engine's rule**.
> Each is marked in place rather than deleted, because the argument for
> each still stands the day someone wants it and the reason it was declined
> is the more useful record.

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
- ~~It does not select a core.~~ **Reversed 2026-09-02 by AF — see NS10.**
  A namespace *does* select a core: it is the placement declaration
  `owner_core` is set from, and the whole reason AF exists. The sentence
  that stood here — "two relations in one namespace may sit on different
  cores" — is now false under the shipped policy and true only under
  `placement = creating` or `rotate`. What survives of the exclusion is
  narrower and still load-bearing: **a namespace is not a unit of
  ownership.** The owning unit stays the relation, and a namespace only
  decides what a relation's `owner_core` is set to at CREATE (AF-9).
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

## NS5 — Resolution `[AMENDED 2026-09-02 by AF-T3]`

**What is built.** A name is either **qualified** (`ns.table`) or
**unqualified** (`table`), and a relation's name is **instance-global**:

- Unqualified: resolves to the one relation of that name, wherever it
  lives. Every statement written before AF therefore means exactly what it
  meant.
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
  **never created by being named** (AF-6's argument for shape (a) over
  shape (b): a typo must not be indistinguishable from an intent).

**Why the two-step fallback below is not built.** It presupposes NS6 and
NS8 — a name that can mean two relations, and session state that decides
which. AF-9 declines the first outright, so the second has nothing to
choose between. The rule is kept because it is the right rule the day
names become namespace-scoped, and because the argument against a search
path is unaffected by any of this:

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

## NS6 — Uniqueness `[AMENDED 2026-09-02 by AF-T3: `(name)` alone]`

**What is built:** a relation's name is unique **instance-wide**, which is
what it has always been — `Catalog::FindTableOidByName` takes a name and no
namespace, and its own comment calls itself "the gate", because SQL reaches
a relation by name and never by oid. Two namespaces cannot hold two
relations called `orders`, and that is the thing people usually expect a
namespace to buy. It is declined deliberately: AF-9 says a namespace is not
a name-collision domain, and making it one means a namespace argument on
`FindTableOidByName` and on every one of its callers, plus NS8's session
state to give an unqualified name a meaning.

The rule below is what it becomes if that is ever built.

`(namespace_oid, name)` is unique among live relations. `(name)` alone is
not. Two namespaces may each hold a `orders`; they have different oids and
are different relations in every respect.

The dropped-relation tombstone participates by oid, not by name: a retyped
`kTypeDroppedTable` row keeps its `namespace_oid` for provenance, and its
name is free for reuse in that namespace — the same rule DROP TABLE already
follows, now read with the namespace in the key.

## NS7 — DDL surface, v1 `[BUILT 2026-09-02 at AF-T1/AF-T3]`

- `CREATE NAMESPACE <name>` — allocates an oid, writes one `sys.objects`
  row with `type_oid = kTypeNamespace`. No pages, no relations. ~~No
  placement decision.~~ **It is now exactly the placement decision** —
  NS10, and the namespace's core is fixed by its first relation, not by
  this statement. Refused if the name is live, and refused for the two
  reserved spellings `sys` and `public` (`well_known.hpp` says why each).
- `DROP NAMESPACE <name>` — permitted **only when empty**. No `CASCADE` in
  v1, stated as a scope decision and not an oversight: a cascade is a
  multi-relation DDL whose relations may be owned by different cores, so it
  is either a multi-core DDL or a loop that can half-succeed, and both are
  questions this spec declines to answer ahead of the DDL-transactional
  work. ~~The refusal names the count of live objects.~~ **It names the
  relation** that blocked it (AF-T1), because the user's next act is to drop
  or move that relation and a count sends them to a catalog query first.
  The row is **retyped to `kTypeDroppedNamespace` and never retired**: it is
  `GenerateUserOid()`'s floor evidence, and — since NS10 — the evidence a
  namespace's rank is derived from, so retiring it could move a namespace's
  core.
- `CREATE TABLE [ns.]name` — ~~unqualified creates in the session's current
  namespace~~ **unqualified creates in `public`**, there being no session
  current namespace (NS8, declined). A named namespace must already exist.
- `SHOW NAMESPACES` — NS9's listing, built at AF-T3. The two bootstrap
  namespaces are listed by their **SQL spellings** (`sys`, `public`) rather
  than by the registry names their rows carry (`namespaceSys`,
  `namespacePublic`), because what a user needs from the command is the
  word they would write in a statement. The two spellings for one thing are
  recorded at `well_known.hpp` rather than converged, which is user-visible
  and nobody has asked for.
- Qualified names are accepted anywhere a relation name is accepted:
  `SELECT` (`FROM` and every `JOIN`, and inside every subquery block),
  `INSERT`, `UPDATE`, `DELETE`, `REFERENCES`, `ALTER TABLE`, `DROP TABLE`,
  `CREATE INDEX`, `CREATE CABIN`, `CREATE ASSERTION`, `DESCRIBE` and
  `SHOW RELAYOUT`. Verified, never ignored (NS5). Two implementation sites
  cover all of them — `exec::CompileBlock`'s relation-binding loop for every
  `SELECT` shape, and one `CommandDispatcher::QualifierRefusal` per other
  statement.

DDL stays system-core-only (unchanged); a namespace is a catalog row and
core 0 writes catalog rows.

## NS8 — Session state `[DECLINED 2026-09-02 by AF-T3 — not built]`

**No session carries a current namespace.** With names instance-global
(NS5, NS6) there is nothing for one to select between: an unqualified name
already reaches exactly one relation, and a "current namespace" that
changed which relation a name reached would be the session-state-dependent
meaning this spec's own NS5 argues against under a different heading.

The rule below is what NS8 becomes if names ever become namespace-scoped,
and its second paragraph is the binding requirement that would land with
it — a shipped statement must carry a resolved name, never a
session-dependent one.

## NS8 (unbuilt) — Session state: one current namespace

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

## NS10 — The namespace selects the owner core `[AF, 2026-09-02]`

The operator's direction, verbatim
(`instructions/v2.8.0/ratification-af-namespace.md` AF-1):

> So I would like to use namespace for selecting fist created owner core
> number, it is just logical thing, 2PC still make it enables to cross
> core transaction. as best practices createing relations higly wired
> under same namespace is recommended

**The rule.** A relation created in a namespace is owned by the core that
owns that namespace, and a namespace's core is fixed by its **first**
relation and never rebalanced (AF-P4). Implemented as
`PlacementPolicy::kNamespace` in `include/kds/catalog/core_placement.hpp`,
which is the **shipped default** since AF-T2.

**How the core is derived, and why it is derived rather than stored.**
`Catalog::DeriveNamespacePlacement` reads three facts off catalog rows and
`AssignOwnerCore` decides from them; no row gains a field, so a file
written before AF mounts unchanged and means what it meant.

1. **`sys` and `public` are never rotated.** The test is
   `namespace_oid >= kUserOidStart` — "did anybody declare this?" — and it
   is deliberately *not* `IsSystemNamespace`, which answers the different
   question of whether a row may be renamed or dropped. This is the clause
   that keeps DA2 true: a relation nobody declared a namespace for is
   placed exactly where `placement = creating` places it, so the shipped
   default's behaviour is unchanged for every instance that writes no
   `CREATE NAMESPACE`.
2. **A namespace with relations answers with its lowest-oid relation's
   `owner_core`** (AF-P1). The answer is a function of rows that exist
   rather than a cached number that could drift from them, and it is what
   makes an existing file's placement survive a changed core count.
3. **A namespace with none rotates on its declaration order** — how many
   `kTypeNamespace`/`kTypeDroppedNamespace` rows on `sys.objects` precede
   it. Dropped rows are counted precisely because they are never retired,
   which makes the rank immutable and is how AF-P4 survives someone
   emptying a namespace and refilling it.

**Clause 3 is a correction to AF-P1 as first written, and the operator
ratified it on 2026-09-02** (AF-P1a). AF-P1 said an unfixed namespace
takes "the creating core". DDL runs on the system core and only there —
`Catalog::CreateTable` passes `kSystemCore` — so that rule places *every*
namespace on core 0 and the policy does nothing at all, which contradicts
AF-4's own statement of what AF is ("rotation with the grouping
supplied"). The rank supplies the missing half. The alternative the
correction offered — a core the operator *states* at `CREATE NAMESPACE` —
was **not** taken, and is kept in AF-P1a as what a re-design would start
from.

**What this does not claim.** It introduces no second unit of ownership:
the owning unit stays the relation (NS1 as amended, AF-9). It does not make
a namespace a transaction boundary — two namespaces are two cores and a
transaction over both crosses and commits under 2PC exactly as before
(NS4). It does not rebalance. And it does not reopen the range split: AE-3.2
stands, and AF is the reason it can — the parallelism a split would have
found inside one relation is found between groups of relations instead,
with the grouping declared by the person who knows it.

**The best practice is the point, not a footnote** (AF-2). Relations that
are joined, foreign-keyed or read together belong in one namespace, so the
wiring is core-local; relations that have nothing to do with each other
belong in different ones, so their work runs at the same time. A foreign
key across namespaces is admitted and priced (`foreign-keys.md` §2a); one
inside a namespace never crosses at all.

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
- ~~**Namespace-aware placement**~~ — **closed 2026-09-02 by AF**, and not
  in the direction this line expected: the namespace *decides* placement
  rather than hinting it. NS10.
- **Namespace-scoped names** — NS5's fallback, NS6's `(namespace_oid,
  name)` key and NS8's session current namespace, all declined at AF-T3
  and all kept in place above. What building them costs is named at NS6: a
  namespace argument on `Catalog::FindTableOidByName` and on each of its
  callers, plus the session state to give an unqualified name a meaning.
- **A namespace's placement under a changed core count.** NS10 clause 2
  keeps a populated namespace on its relations' core, and clause 3
  re-derives an empty one's from a rank taken modulo the *current* count —
  so an empty namespace can move across a mount that changes `cores`. It is
  the same class of question as `wal.md` §3's recovery under a changed core
  count and is not answered ahead of it.
- **Renaming a namespace.** `ALTER` surface generally; nothing here
  forecloses it, and NS2 means a rename changes no identity.
