# DROP NAMESPACE's RESTRICT races a create in the namespace

**Found by reading, not reproduced.** Verified at `1635263` on
`at-s17-one-name-one-row`, by AT-S17's survey of the name-taking writes
(`catalog.md` CT7). Not a name: a relation's *membership*, which is why
AT-S17 does not close it.

## What is wrong

`DROP NAMESPACE` is RESTRICT: `Catalog::DropNamespace` scans `sys.tables`
(page 7) for a relation in the namespace, and only then retypes the
namespace's `sys.objects` row (page 6). `CREATE TABLE ns.t` resolves `ns`
in the dispatcher (`ResolveCreateNamespace`), and `Catalog::CreateTable`
writes `t`'s `sys.objects` row and, after it, its `sys.tables` row.

Nothing holds across either pair. That was one act while every DDL ran on
core 0; since AT-S5 a DDL runs where its session is, so on two cores:

1. core 1 resolves `ns` - live;
2. core 0's RESTRICT scan of `sys.tables` finds no relation in `ns`, and
   the retype drops it;
3. core 1 writes `t`'s rows, naming `ns`.

A relation in a dropped namespace. The transactional flavours are covered
already: an open create's `sys.tables` row is on the page, so the
unfiltered RESTRICT scan refuses the drop; an open drop's retype is on the
page, so an unfiltered namespace resolution refuses the create. What is
open is the latch-level interleaving above.

## Smallest reproduction (not yet written)

`catalog_name_race_test.cpp`'s two-catalog shape: core 0 `DropNamespace`,
core 1 `CreateTable` into it, a rendezvous between core 1's namespace
lookup and its `CreateTable`. Afterwards, a `sys.tables` row whose
`namespace_oid` names a `kTypeDroppedNamespace` row.

## What it costs

`ns.t` no longer resolves (the qualifier names nothing), `t` resolves
unqualified, and `SHOW NAMESPACES` omits the namespace its relation claims.
Every surface that reads the namespace off the relation answers from a
row nothing else can name.

## The fix

Known in shape: CT7's held page reaches it, because `t`'s `sys.objects`
row carries its `namespace_oid`. `CreateTable` re-checks that the
namespace is live under its hold of page 6, and `DropNamespace` holds page
6 across a RESTRICT check that reads `sys.objects` - where the create's
row lands under the same hold - rather than `sys.tables`, which the create
writes after the hold is dropped. Unscheduled; whether it rides AT-S19's
§8 list or its own stage is the operator's.
