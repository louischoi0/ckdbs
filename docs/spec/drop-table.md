# DROP TABLE v1 — catalog-scoped, with the oid tombstone

Decisions DT1-DT6. `DROP TABLE` removes the catalog's knowledge of a
relation and reclaims no pages. It shares `docs/spec/alter.md` AL4's
RESTRICT predicate, and its transactional behaviour is
`docs/spec/ddl-transactional.md` §5a's.

## DT1 — Catalog-scoped: the relation becomes unreachable, its pages orphan

v1 removes the *catalog's* knowledge of the relation and reclaims no
pages. The heap/btree chain, the var-heap chain, index pages and any
Bound Cabin pages stay allocated and unreachable — leaked space, stated
plainly. Returning a page to the free map is `physical-optimizer.md` §6
gate 3 (a reallocated page breaks trail validation — per-relation Keystone
ids collide at a reused slot), and any reuse needs a consumer of the
reader horizon that no user-relation purge is (`txn.md` §4.1). A DROP that
guessed at
either would be the partial recovery `txn.md` §8 forbids, in different
clothes.

## DT2 — The oid tombstone: a dropped oid is never reissued

`Catalog::GenerateUserOid()` recovers its floor from the highest oid in
`sys.objects`/`sys.columns` — the rows *are* the counter. Removing a
dropped relation's rows outright could hand its oid to the next CREATE,
and a reissued oid falsifies "(oid, pk) is forever-unique": stale
advisory structures (trails, access stats) keyed by the dead oid would
validate against the new relation, and with the dead relation's pages
still holding their bytes (DT1), a recorded location could serve a dead
table's row as a live answer. So the `sys.objects` row is **retyped, not
retired**: `type_oid` becomes `kTypeDroppedTable`, the row keeps its oid
and name forever. Name resolution filters on `kTypeTable`, so the name
frees for reuse immediately; the oid floor stands because the max-scan
reads every row. One row of catalog space per dropped relation is the
whole price, and K3 calls a burned oid free.

## DT3 — RESTRICT in, dependents out

Two things block a drop, each refused naming the blocker:
- a **foreign key referencing the relation as parent** (`sys.fkeys` by
  `parent_rel_oid`) — declared-level, so an empty child table still
  blocks: the constraint exists whether or not rows do. Drop the child
  first (v1 has no DROP of a single fk).
- an **assertion on the relation** — `exec::AssertionsOnRelation()`,
  AL4's predicate. Same argument as ALTER's: an enforcing constraint is
  not allowed to die quietly.

Everything the relation *owns* drops with it, in one statement:
`sys.tables`, all `sys.columns` rows (their slots retire and are
reusable, which matters against the columns-ever-created ceiling), its
`sys.indexes` rows, its `sys.cabins` rows (plus the in-memory
`CabinStore::Forget`), and its **child-side** `sys.fkeys` rows.

## DT4 — Advisory structures die, like AL3 said

Patterns and trails whose text or entries name the dead relation stop
resolving and answer nothing — invariant 8's license, the rename
argument verbatim. `sys.access_stats` rows for the dead oid stay as
ghosts (keyed by an oid that can never be reissued, they can never
mis-attribute; `SHOW ACCESS` may list them and honesty costs a row).

## DT5 — One bump; atomic, not isolated

A catalog write like all DDL: WAL-logged and durable
(`ddl-transactional.md` §7), `BumpVersion()` once at the end (a dropped
name is read by resolution itself — no in-place exception), peers
invalidated through the `kCatalogInvalidate` path. `sys.*` relations are
refused, ALTER's AL7 verbatim.

In autocommit the drop **retires** its dependent rows. Inside an explicit
transaction it **delete-marks** them instead and records the
`sys.objects` retype's before-image, both on the transaction's trail, so
`ROLLBACK` restores the relation and its rows. The drop is **atomic but
not isolated** — other sessions see it before it commits;
`docs/spec/ddl-transactional.md` §5a says why.

## DT6 — Grammar

`DROP TABLE <name>`. Nothing is reserved; `DROP` is not a patternable
head, so corpus lines carry `-` hashes. Refusals: unknown name `NotFound`;
a `sys.*` relation refused; the DT3 blockers named with their objects.
