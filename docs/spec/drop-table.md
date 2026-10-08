# DROP TABLE v1 — catalog-scoped, with the oid tombstone

Decisions DT1-DT7. `DROP TABLE` removes the catalog's knowledge of a
relation, and its pages go back to the allocator after the drop's commit,
once no replay and no reader can reach them (DT1). It shares
`docs/spec/alter.md` AL4's RESTRICT predicate, and its transactional
behaviour is `docs/spec/ddl-transactional.md` §5a's.

## DT1 — Catalog-scoped, and its pages reclaimed after the commit

The drop itself writes catalog rows only. Its retype of the `sys.objects`
row also records what the relation owes: the anchor page and the var-heap
root, packed into the tombstone's `pending_roots`
(`catalog/rows.hpp`; superblock 21). A rollback, or a loser undone at
mount, restores the word to 0 with the rest of the row, so nothing is owed
(`instructions/v3.0.0/workorder-bf-drop-table-page-reclaim.md` BF-R1,
BF-R2).

**What is reclaimed** is what those roots reach (BF-R3,
`server/page_reclaim.hpp`):
- the anchor;
- the clustered tree, by descent and by its leaf chain;
- every tree an anchor slot names - a dropped or rolled-back index's
  included, since no slot is ever removed;
- the var-heap chain;
- a pre-SUS-1 heap chain.

Every page is checked for the class its parent names and for its owner -
the relation's oid, or the slot's index oid. A page that fails is neither
freed nor walked through, and is counted `reclaim_skipped`.

**When**, both conditions together:
- **No replay can name a page** (BF-R4): the durable redo start has passed
  the drop's commit. No WAL record is added.
- **No reader can reach a page.**
  - A tombstone a mount found pending is freed on core 0's tick after that
    mount's completion checkpoint (BF-R8). No statement of that run can
    reach it: the name resolves `NotFound`, and every memo and cache is
    built after the mount. At `cores = 1` that is at once; at `cores > 1`
    it waits for every core's first checkpoint.
  - A drop committed during the run is freed once every core's statement
    epoch has passed the drop's commit (BF-R9). That is the end of every
    statement whose memo could hold the relation, and a statement
    publishes its epoch before it reads the schema word.

**The order** frees children before parents: leaves, then each internal
level, with a map sync after each; each chain tail-first, a sync per page;
the anchor last. Only once those frees are durable is the tombstone's word
cleared. A crash before the clear leaves the tombstone pending, and the
next mount drives it again; the walk's owner check makes that re-drive
idempotent (BF-R7). A freed id comes back through the store's free list
(`page.md` §5), and the file never shrinks.

**What stays leaked** is every page no root reaches:
- pages left unlinked by a failed growth or split;
- a failed `CREATE INDEX`'s tree;
- a failed or rolled-back `CREATE TABLE`'s pages;
- a refused catalog report's page;
- a previous run's undo pages (UP4);
- a dropped assertion's Bound Cabin chain;
- Waystone pages.

`docs/inflight/known-gaps.md` names each. A drop never frees Bound Cabin
pages: DT3 refuses it while an assertion exists.

**Gate 3 is answered for a dropped relation only**
(`physical-optimizer.md` §6). A reused page that would validate a stale
location is a miss, through the walk's owner check, the replay gate and
BF-R10's checks on every remembered location. Gate 3 stays shut for a
mover.

## DT2 — The oid tombstone: a dropped oid is never reissued

`Catalog::GenerateUserOid()` recovers its floor from the highest oid in
`sys.objects`/`sys.columns` — the rows *are* the counter. Removing a
dropped relation's rows outright could hand its oid to the next CREATE,
and a reissued oid falsifies "(oid, pk) is forever-unique": stale
advisory structures (trails, access stats) keyed by the dead oid would
validate against the new relation, and with the dead relation's pages
handed out again to another relation (DT1), a recorded location could
serve a dead table's row as a live answer. So the `sys.objects` row is **retyped, not
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

Every production drop **delete-marks** its dependent rows and records the
`sys.objects` retype's before-image, both on its transaction's trail, so
`ROLLBACK` restores the relation and its rows. That includes an autocommit
drop, which runs in the implicit transaction `BeginWrite` opens. The marks
retire once settled, through the §5d purge or at the next mount. Only a
drop at the bootstrap id retires outright, and only tests reach one (BF-Q12
(a): the code governs, and this text was restated to it). The drop is **atomic but
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
and reading on. A fresh resolution answers `NotFound` once the drop's
catalog write is made; the readers that read on are the ones whose memo
predates the drop, and they stay correct because no page of the relation is
freed while one of them runs (DT1's statement epoch) and the oid is never
reissued (DT2). So the guarantee is
"a positioned reader is not overtaken", never "no reader sees the drop
before it commits".

**Since AT-S1 the borrow is taken at the bind, not at the first page**
(`workorder-at-m3-uniformity.md` AT-R1), and it widens what a drop waits
for: a relation that is a join's inner side, a subquery's relation, or the
target of an `INSERT`/`UPDATE`/`DELETE` holds the `IS` for the whole
statement - from the bind to the end - where only a scan's outermost walk
held it before. (A remote stage declared its own from its resolve until
AT-S10 deleted the remote-step protocol.) Under a
continuous stream of such statements a drop reaches the lock family's
fault net and is refused where it used to succeed, which is the sentence
above with a larger population of readers behind it. **The net is 1 s
since AT-S6** - it was 11 s while a holder could sit inside a
coordinator's phase deadline, and no holder waits on another core now -
so that refusal is roughly eleven times easier to reach than when this
paragraph was written, and `lock_wait_fault_net_ms` is what an operator
who wants the old bound sets.

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

- **The wait is bounded by the lock family's fault net**, 1 s since AT-S6
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
