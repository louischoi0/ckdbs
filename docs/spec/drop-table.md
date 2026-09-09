# DROP TABLE v1 — catalog-scoped, with the oid tombstone

Decisions DT1-DT7. `DROP TABLE` removes the catalog's knowledge of a
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
name is read by resolution itself — no in-place exception), every other
core re-reading at its next boundary through the schema version word
(`catalog.md` CT2). `sys.*` relations are refused, ALTER's AL7 verbatim.

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

## DT7 — It waits for a statement already using the relation

Since AO-S6e-b the drop takes the relation in `X` from the lock family
(`txn.md` §5, AR2 §3's DDL row) before its first catalog write, and a
**positioned reader holds the same entry in `IS`** — the read borrow a
walk declares. So a drop that arrives while a scan of the relation is
running waits for that scan to finish and then runs whole, instead of
retiring the relation under it.

**What that buys, exactly, and it is one sentence**: a reader that was
already walking finishes its statement against a live schema, rather than
meeting the clean error the post-park re-`Bind` gives it
(`exec/step_vm.cpp`) when a DDL invalidates the catalog beneath it.

**What it does not buy, and DT5 is unchanged**: the drop is still not
isolated. A read that *starts* after the drop has taken the relation takes
no borrow at all — a refused read borrow leaves the reader holding nothing
and reading on, because a reader needs no borrow to be correct (DT1: the
pages stay allocated and the oid is never reissued). So the guarantee is
"a positioned reader is not overtaken", never "no reader sees the drop
before it commits".

**Since AT-S1 the borrow is taken at the bind, not at the first page**
(`workorder-at-m3-uniformity.md` AT-R1), and it widens what a drop waits
for: a relation that is a join's inner side, a subquery's relation, the
target of an `INSERT`/`UPDATE`/`DELETE`, or the relation a remote step is
streaming on another core, holds the `IS` for the whole statement - from
the bind, or on a remote stage from its own resolve, to the end - where only
a scan's outermost walk held it before. Under a
continuous stream of such statements a drop reaches the lock family's 11 s
net and is refused where it used to succeed, which is the sentence above
with a larger population of readers behind it.

**And it is not only readers.** A relation `X` is refused by the `IX`
every writer of the relation holds, so a drop also waits for a transaction
that has written a row of it - until that transaction decides, since a
write's borrow is the transaction's and not the statement's. Before this
the drop retired the relation under such a writer. Two things follow: the
wait can be long where a client holds a transaction open, and a drop
**inside a transaction that holds rows** can be half of a deadlock cycle -
which is why the wait registers an edge in the wait-for graph there and
refuses the statement naming deadlock rather than waiting for a holder that
is waiting for it (`txn.md` §5). An autocommit drop holds nothing while it
waits and registers none.

Four consequences worth stating rather than discovering:

- **The wait is bounded by the lock family's fault net**, 11 s
  (`txn.md` §5), and reaching it refuses the drop `TxnConflict`, retryable.
  A `DROP TABLE` under a continuous stream of readers can therefore be
  refused where it used to succeed: readers do not queue behind it, so a
  second reader may take the relation between the release the drop was
  woken for and its next ask.
- **The refusal names a reader, not a transaction.** A read borrow is a
  statement's and holds under an identity that is not a transaction id, so
  the message says "held by a positioned reader".
- **A drop reached over the synchronous path is refused rather than
  waited**, since there is no reactor to park on — the same division every
  other wait in the dispatcher draws.
- **Inside a transaction the failed drop does not poison it while the wait
  is still possible**: the statement wrote nothing, so it is re-run after
  the wait, and only the refusal that ends the wait poisons - the rule
  `txn.md` §5 already states for a write refused by an undecided holder.
