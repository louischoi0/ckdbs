# Transactional DDL

`CREATE TABLE` and `CREATE INDEX` are atomic, isolated, consistent and
durable; `DROP INDEX` is atomic and isolated on core 0 — on a relation
another core owns it is refused inside a transaction (§5e); `DROP TABLE`
is atomic only — §5 says what each gets and §5a why they differ. §5e and
§5f are the two-core cases: a `CREATE INDEX` or `CREATE ASSERTION` whose
relation another core owns is built by that owner and published by core 0.

## 0. The decision this reverses

`docs/spec/txn.md` §7 records the decision this file reversed ("DDL is
neither logged nor transactional"); this file is the contract.

## 1. What "transactional" means here, precisely

Four properties are usually bundled under the word. They are separable
and have very different costs in this engine:

| | Property | Meaning for DDL | Status |
|---|---|---|---|
| A | **Atomicity** | `ROLLBACK` undoes a `CREATE TABLE`; the relation does not exist afterwards | built |
| B | **Isolation** | Another connection cannot see a relation whose creating transaction has not committed | built |
| C | **Consistency of the pair** | A statement that creates a relation *and* inserts into it either leaves both or neither | built |
| D | **Durability** | A committed `CREATE TABLE` survives a crash | built, §7 |

## 2. The mechanism, and why it is smaller than it looks

Three facts about the code make this tractable:

1. **Catalog rows are MVCC tuples.** They live in heap pages and carry
   invariant 12's header — `trx_id:48 | undo_ptr | data_len | flags` —
   exactly like user rows. Nothing about the *format* changes.
2. **Bootstrap rows are stamped `kBootstrapXid`** (= 1 =
   `kAlwaysVisibleTrxId`) by one function, `InsertRow()` in
   `src/catalog/catalog.cpp`, which is the whole reason they are visible
   to every read view. User DDL stamps the real transaction id.
3. **Catalog reads filter by the reader's view where resolution is at
   stake** (§5a's classes). `ScanAll` and its single-row sibling skip a
   dead slot because `PageView::ReadTuple` reports `NotFound`; a view,
   when one is passed, hides rows whose writer is in flight.

So **isolation** is: stamp the real transaction id, and filter catalog
reads by the reader's view. While the creating transaction is live its id
sits in every other view's in-flight set, so its rows are invisible to
them and visible to itself. That is the whole of property B.

**Atomicity is not visibility.** `txn::ReadView::Visible` answers *"below
the high-water mark and not in-flight"* → **visible**; it has no notion of
"aborted", so once the aborting transaction is out of the live set a view
minted afterwards reads its id as committed. The engine hides aborted work
by **compensation**: `TransactionManager::Abort` walks the transaction's
trail in reverse and physically undoes each mutation, and for an insert
that is `PageView::RetireSlot`. So DDL does what every other write does —
**registers its catalog row writes on the transaction's trail**
(`NoteDdlRows`, `NoteCatalogRowChanges`), even when the DDL statement
failed, so `Abort` compensates them. Isolation and atomicity are separate
phases, and only the first is delivered by the read filter.

## 3. Invariants this must not break

- **Invariant 12 is untouched**: the header stays 20 bytes, and no
  `xmax` appears. A DROP delete-marks exactly as a user `DELETE` does.
- **Ids are burned, never reused** (invariant 11 / K1). A rolled-back
  `CREATE TABLE` consumes its oid and its Keystone id permanently. This
  is not a leak to be fixed later; it is the issue-once contract, and
  DDL gets no exemption.
- **A peer may not write the catalog** (crosscore.md P6). DDL runs on
  core 0, and a peer's view of the catalog is "drop the cache and
  re-read".
- **Bootstrap rows keep `kBootstrapXid`.** The well-known types,
  namespaces and the catalog's own descriptors are not transactional and
  must remain visible to every view, including a view minted before any
  transaction existed. Only *user* DDL takes a real id.

## 4. The cache

`CatalogCache` memoizes name→oid and oid→`TableAccess`, per instance and
snapshot-blind. The rule that keeps it correct: **a view is minted only
while some transaction holds uncommitted DDL** (`CommandDispatcher::
ViewFor`, gated by `ddl_txns_`). With none in flight every catalog row is
either a bootstrap row or a committed one, so an unfiltered, cached read
is correct for every reader. While one is open, a statement resolves
names under a view — its own transaction's, or a fresh committed-now view
in autocommit — and a filtered lookup deliberately bypasses the shared
cache, costing a catalog page scan per resolution for exactly as long as
isolation is at stake. Each statement takes its view boundary once
(`EnsureStatementBoundary`), so two resolutions in one statement never
disagree.

`Catalog::catalog_version()` is not a sound freshness guard —
`InvalidateFromPeer()` clears the cache without bumping it, deliberately —
and nothing here leans on it.

## 5. What is in scope

Built, and what each gets:

- **`CREATE TABLE`** — atomic, isolated, durable, rolled back by
  `ROLLBACK`. `BEGIN; CREATE TABLE t ...; INSERT INTO t ...; ROLLBACK;`
  leaves no relation and no rows.
- **`DROP TABLE`** — **atomic only**, and deliberately not isolated;
  §5a is the whole argument. In autocommit it retires its dependent rows;
  inside a transaction it delete-marks them.
- **`CREATE INDEX`** — atomic and isolated, exactly as `CREATE TABLE`.
- **`DROP INDEX`** — atomic and isolated, **core-0-scoped** (§5b): on a
  relation another core owns it is refused inside a transaction (§5e).
- **`CREATE INDEX` on a relation another core owns** — atomic and
  isolated across two cores: the owner builds the tree from its own lease,
  core 0 writes and commits the `sys.indexes` row (§5e).
- **`CREATE ASSERTION` on a relation another core owns** — the owner
  builds, core 0 publishes; admitted inside a transaction (§5f).
- The autocommit path is unchanged in behaviour: a bare `CREATE TABLE`
  commits immediately.

**Not transactional:** `ALTER TABLE`, cabins, assertions and foreign keys.
Their catalog writes are logged and durable (§7) but not undone by
`ROLLBACK`.

### 5a. `DROP TABLE` is atomic, and deliberately **not** isolated

A drop inside a transaction **delete-marks** its dependent rows instead of
retiring them and records the `sys.objects` retype's before-image, both on
the transaction's trail — so `ROLLBACK` clears the marks and rewrites the
tombstone back to a live table, restoring the relation and its rows.
Autocommit retires.

**Other sessions see the drop immediately, before it commits.** The
`sys.objects` retype is an *in-place overwrite*, and a catalog row has no
undo chain (`txn.md` §7) — so the prior image exists only in the aborting
transaction's own trail, and there is nowhere for another reader to
recover it from. A filtered `ScanAll` *skips* a row whose writer it cannot
see, so an outsider's name lookup answers `NotFound` and the relation
vanishes rather than lingering; no rule about delete-marks reaches an
overwrite. Between `DROP TABLE t` and the `COMMIT` or `ROLLBACK` that
resolves it, other sessions see `t` as already gone; if the transaction
rolls back, `t` comes back. Reads in that window are not wrong about the
rows — the data pages are untouched — they are early about the schema.

The general limit under it: **any catalog change that unfiltered readers
act on cannot be isolated**. A delete-mark is isolable only where every
reader of that row filters or applies §5b's rule.

Two smaller facts:

- **The sweep loop's termination.** It runs until nothing matches, which
  a retired slot satisfies by disappearing and a delete-marked one does
  not, so the transactional path skips rows already marked.
- **A committed transactional drop leaves its marked rows on the page**,
  where autocommit's retire reclaims the slot; §5c and §5d retire them
  later. Both read as gone, so the difference is space rather than
  meaning.

### Which reads filter, and which deliberately do not

Every route into "does this relation exist" must answer the same way, or
the one that answers differently is the leak. Three classes, and the
membership is a decision:

- **Filtered — a statement's own resolution.** `SELECT` (through
  `exec::Compile`, its sub-chains, and `CompileWhere`), `INSERT`,
  `UPDATE`, `DELETE`, `DESCRIBE`, `SHOW TABLES`, `SHOW INDEXES`, `ALTER`,
  `DROP TABLE`, and a foreign key's parent lookup. These decide what a
  statement may touch, so they answer under the session's view.
- **Unfiltered by design — "does this name already exist".** The
  duplicate-name check in both `CREATE TABLE` forms. Filtering it would
  hide another transaction's uncommitted relation of the same name, both
  creates would succeed, and two rows would claim one name. Seeing
  everything refuses the second instead — the half that cannot corrupt
  anything. The cost is a refusal that can be spurious (the first
  transaction may roll back) and that names a relation the asker cannot
  see (`EXISTS oid=…`, which is not an `ERR` and does not poison the
  session).
- **Unfiltered by design — diagnostics.** `SHOW ACCESS`, `SHOW BUDGET`,
  `SHOW ASSERTIONS`, `SHOW CABINS`, and the name-rendering helper. These
  answer *"what does this instance hold"*, which is an operator's
  question, not a statement's. Also unfiltered, and unaffected either
  way: `ALTER`'s system-relation guard, which tests an already-resolved
  oid against bootstrap rows that every view sees.

**The line between the classes.** A surface reporting **schema objects**
— which relations or indexes exist — is a resolution route and must
filter. A surface reporting **engine state** — statistics, budgets,
memory-resident structures — is a diagnostic and must not.

### 5b. What an unfiltered read does with an open delete-mark

> **An object exists from the moment its row is written until its
> removal commits.**

That is the whole rule, and it is deliberately **asymmetric**. An
unfiltered read still sees an *inserted* row immediately, whoever wrote
it; it stops seeing a *delete-marked* one only once the deleter is no
longer in flight. The symmetric version — mint a committed-now view for
internal reads, hiding uncommitted inserts too — is a bug in the mirror
direction: a session's own uncommitted `CREATE INDEX` would stop being
maintained by its own `INSERT`s, and would commit an index missing every
row the transaction wrote. Both halves as stated fail toward *"the
object is there"*, and the object is only ever **maintained** by a
writer that would otherwise skip it.

What it costs when a drop is open: index maintenance keeps writing
entries for an index that is about to disappear. If the drop commits,
those entries go with the index; if it rolls back, the index is whole.
The wasted work is bounded by the length of the transaction holding the
drop.

**Where it lives.** One arm of one function — `ScanAll`'s delete-mark
branch in `src/catalog/catalog.cpp`, the only *reader* of a catalog
delete-mark in the tree. `ScanAll` takes
`TransactionManager::OldestActiveTrxId()` once per scan: a deleter below
it is settled by definition (`live_` holds every running transaction on
this core, so an id below the smallest of them is not one of them), and
only a mark whose deleter is at or above it pays the
`TransactionManager::IsInFlight` walk — a walk of the live list rather
than a minted `ReadView`, because the caller wants one bit and a view is a
528-byte array copy. With nothing running the manager is not consulted at
all.

**"No longer in flight" is safe to read as "committed"** for exactly one
reason, and it is an ordering fact rather than a definition:
`TransactionManager::Abort` compensates the entire trail *before* it
clears `active_`. A mark whose deleter has gone inactive is a mark no
rollback is coming for. If that order is ever inverted, this rule breaks
silently.

**The catalog asks only when it has a manager to ask.** `Catalog`
carries a `SetTransactionManager` handle, armed by the
`CommandDispatcher` constructor — the one place a catalog and a manager
are known to belong together. Left null, every unfiltered read answers
as if no mark had a live deleter: bootstrap, recovery and a test over a
bare store have no in-flight transaction to be wrong about.

**A mark left by a transaction from a previous mount** has no deleter to
ask about, and may have one that is not its own (the transaction-id
ceiling is unlogged, `txn/trx_id.hpp`, so a crash can reissue the block);
§5c removes the question by retiring every such mark at mount.

**The claim is core-0-scoped.** `IsInFlight` answers about one core's
`live_` list, and a peer maintains its own relation's index. A core-0
`DROP INDEX` on a peer-owned relation is therefore **refused inside a
transaction** (§5e); no other cross-core DDL may lean on this predicate
without the same guard.

**The cache learns it at both endings.** `EndDdlScope` invalidates the
catalog cache unconditionally when a DDL-holding transaction resolves,
commit or rollback: commit is the moment a delete-mark starts counting, and
a cache filled during an open `DROP INDEX` holds the index deliberately —
holding it past the commit would keep maintaining an index that is gone,
and never tell a peer to re-read.

### 5c. Delete-marks are finalized at mount

Every delete-marked catalog row is retired at mount, on the system core,
after recovery and before the listener binds
(`Catalog::FinalizeDeleteMarksAtMount`).

**Why.** §5b makes a mark's meaning depend on whether its deleter is in
flight. A mark that outlived its mount has no deleter to ask about — and
may have one that is not its own, since a reissued transaction id could
make a finished drop read as open, re-arm the dropped index and answer
probes from a btree missing every row written since. Retiring is the only
available answer: a mark whose transaction committed should be gone, and a
mark whose transaction did not commit cannot be rolled back either — the
trail that would compensate it died with the process. Both already read as
gone to every unfiltered reader; what changes is that they stop being
*ambiguous*. The sweep is also the purge of rows nothing else would ever
reclaim.

**Where, and why only there.** After recovery, so a mark this mount's own
log restored is included; before the transaction stack exists, so no live
transaction can own a mark it retires — which is what makes "retire every
mark" safe here and catastrophic anywhere else. The system core's alone:
a peer may not write a catalog page (P6), and by the time a peer mounts,
core 0 has done it.

**What it costs.** One forward pass over the catalog root chains.
`RetireSlot` sets the dead flag in place and never renumbers slots behind
the walk, so one pass suffices. A crash mid-sweep leaves exactly the
state it started from, which the next mount sweeps again. `SHOW META`
reports `catalog_marks_finalized`; zero is what a clean shutdown produces.

### 5d. Delete-marks purge at DDL resolution, horizon-gated

The in-mount sibling of §5c's sweep: `Catalog::PurgeSettledDeleteMarks()`
retires every delete-marked row whose deleter has cleared the core's
**read horizon** (`TransactionManager::ReadHorizon()`, `txn.md` §4.1),
and `CommandDispatcher::EndDdlScope` runs it at every DDL resolution —
both endings, before the cache invalidation so the flush carries the
retirements too.

**Why the horizon licenses what §5c's mount-only rule forbade.** After
mount a mark may belong to a transaction that is still open — or to a
committed drop an old live view still cannot see, which would resurrect
the row for that reader's filtered reads. The horizon is precisely the
missing proof: a deleter below it is committed (an active transaction
bounds the horizon at or below its own id) and visible to every live and
future view, so the row it marked is gone by every route — filtered reads
see the drop, unfiltered reads settle the mark by the same comparison. A
rollback clears its own marks synchronously, so no aborted transaction's
mark survives to be asked about.

**Why at DDL resolution and nowhere hotter.** DDL resolution is the only
event that creates or settles a mark (autocommit DDL retires directly,
leaving none), it is rare enough that a catalog page sweep costs nothing
worth measuring, and the core is between resolutions there — so no
unregistered synchronous view is live, which is the exemption `txn.md`
§4.1's registration rule leans on. A mark whose deleter has not cleared
the horizon survives to the next resolution or to §5c at the next mount;
there is deliberately **no** background cadence. **System core only**, by
an explicit gate at the call site: the horizon is per-core and blind to
every other core's readers, so a peer's — no transactions, no leases —
answers `UINT64_MAX` and would retire a mark whose deleter is live on
core 0. A failed sweep is a maintenance failure, not the statement's: the
marks it left are exactly as reachable as before, so it is logged and the
reply stands.

**No version bump, deliberately.** Every retired row was already gone to
every reader — that is what the horizon proves — so no cached answer
changes, and a bump would broadcast `kCatalogInvalidate` to peers and
stale this instance's bound statements for nothing. A crash mid-sweep
leaves the state it started from.

**Observability.** `SHOW META` prints `catalog_marks_purged` — this
mount's own retirements — beside the recovery report's
`catalog_marks_finalized`, which counts a previous mount's leftovers.

**What it bounds.** `marks` is bounded; the sweep's cost is not: retired
catalog slots are never reclaimed, so every resolution sweeps every slot
DDL ever occupied. The purge exists to bound `marks` and remove §5b's
ambiguity, never to speed reads — a retired slot's `NotFound` path costs
slightly more than a settled mark's one comparison.

### 5e. A relation another core owns: `CREATE INDEX` built by the owner

`CREATE INDEX` is core 0's statement — the catalog has one writer — but
the tree it builds is *pages*, and a relation another core owns holds its
pages in that core's pool, stamped by that core's stream, with rows core 0
never faulted. Core 0 cannot backfill them. So the **build** runs on the
owner while the **catalog write** stays on core 0, and the statement is
two phases with a park between them (`crosscore.md` CC7's owner-builds
exception says why the pages may not travel the other way instead).

**Atomic.** The owner builds the tree from its own lease under `kNoTxnId`
and replies with the root; core 0 writes the `sys.indexes` row naming that
root and commits. There is exactly one publishing event — core 0's commit
— and until it lands nothing names the tree. A rollback, a refused reply,
or a reply that never comes ends the statement with an error and tells the
owner `done(aborted)`: the tree orphans, exactly as a dropped index's
pages orphan, and no row points at it. A crash between core 0's
commit-record *append* and its durability makes the DDL a recovery loser —
the row is retired and the owner's `kNoTxnId` tree, redone regardless, is
an orphan. Atomic across the crash, because orphaned is not published.

**Isolated.** From the request's arrival until `done`, the owner **refuses
writes to the relation** (a retryable `TXN_CONFLICT`): a row written while
the index is being built would be indexed by nobody, since the owner's
catalog shows no index until core 0's commit invalidates its cache. And
the half-built index is invisible everywhere else — the `sys.indexes` row
is stamped with core 0's transaction and filtered by every reader's view
until it commits, and the owner's cache holds no index until
`done(committed)` (or the catalog-invalidation broadcast) drops it. So no
session reads a partial index and no write slips past unindexed.

**Two gaps a single-core `CREATE INDEX` does not have**, neither a
correctness defect today:

- **A window that expires.** The owner bounds its refusal window by
  `kIndexBuildPendingCeilingNs` (180 s) against a lost `done`; core 0
  bounds its park by `kIndexBuildReplyDeadlineNs` (60 s). The ceiling
  exceeds the deadline, so the owner never releases while core 0 is still
  waiting — but nothing bounds the commit leg, so if the window expires
  before a late commit lands, writes are admitted that the published
  index would miss. Unreachable in practice at these timeouts.
- **`SHOW INDEXES` on core 0** for a peer-owned relation reads the
  build-time root from the `sys.indexes` row — a foreign `InitTableAccess`
  does not read the owner's anchor — which a maintenance split can move, so
  a stale root walks a subtree and prints a plausible-wrong
  `entries=`/`height=`. Diagnostics only: a cross-core *read* downgrades an
  index probe to a scan before it ships (`step_descriptor.cpp`), so query
  answers never depend on core 0's stale root.

**`DROP INDEX` on a peer-owned relation is refused inside a transaction**
(`NotImplemented`, naming the owner), for §5b's reason: the mark's
`BumpVersion` broadcasts before core 0 commits, the owner's §5b predicate
walks its own live list and cannot see core 0's deleter, so it would drop
the index from its view and maintain nothing before COMMIT — and a
ROLLBACK would then restore an index missing every meanwhile-write.
Autocommit keeps only the commit-failure window every DDL has and is
admitted; a `DROP INDEX` on a relation core 0 owns is untouched.

### 5f. The same relation's `CREATE ASSERTION`, and why it is not §5e twice

The shape is §5e's — core 0 checks the declaration and issues the id, the
owner builds, core 0 publishes the `sys.assertions` row, the statement
parks between the two phases — and the *reason* is stronger. An index is
built once from rows core 0 cannot see; a Bound Cabin is **written by
every subsequent write to the relation**, so its pages have to be the
owner's for the assertion's whole life, not only at build time.

**Admitted inside an explicit transaction**, and on the same terms as a
locally-declared one: `InsertAssertion` writes under no transaction, so
the owner enforces before `COMMIT` and a `ROLLBACK` leaves the assertion
in place. One consequence is shared with the local arm and named rather
than hidden: a transaction that has already written the relation meets
the build's in-flight refusal (`assertion_build.cpp`'s `kBusy` →
`TXN_CONFLICT`), and a retry inside that transaction cannot succeed — its
own row stays in flight until it ends — while each attempt burns an
assertion id and orphans the owner's chain. Run the declaration first, or
outside the transaction that writes.

**Atomic**, on the same terms: one publishing event, core 0's row. A refused
reply, a deadline or a failed publish tells the owner `done(aborted)` and the
chain orphans with the entries any meanwhile-write put in it, exactly as a
dropped assertion's pages orphan.

**Isolated differently, and deliberately.** §5e's owner refuses the
relation's writes for the whole build; this one refuses none, because the
owner adopts the directory at the end of its own synchronous build task —
there is no interval between the last scanned row and the adoption in which a
write could be missed. What that leaves is a write admitted after the
adoption and before core 0's publish: counted by a cabin whose row is on its
way, and reserved into an orphan chain if the publish then fails. The cabin
is a stricter-than-snapshot admission structure (`assertion.md` §4.3),
so counting early is the side it already errs on.

**One gap of its own**: a write that passed its admission check and then
parked can reserve after an adoption that happened in between, so a single
row can be reserved unchecked. `CREATE INDEX` has the identical hole against
its window; both need a statement's gate-to-write span to be atomic, which
nothing provides.

## 6. Open decisions — do not assume

The decisions this section listed are unrecorded here.

## 7. Durability

Catalog writes are WAL-logged as the ordinary record types and replayed;
every DDL statement runs under a real transaction (autocommit DDL takes
the implicit one `BeginWrite` opens); a loser's catalog writes carry undo
records the mount rolls back through, appended *inside* the catalog's
write points so redo alone can never resurrect what undo cannot retire. A
committed `CREATE TABLE` whose pages never reached the device comes back
by redo; a committed `CREATE INDEX`'s backfilled tree travels as full page
images before the row that publishes it. The transactionless DDL
statements (ALTER, cabin, assertion) sync their records before the
acknowledgement (`CommandDispatcher::AwaitDdlDurability`), having no
commit record for the durability class to ride on. `SHOW META`:
`ddl_durable=1`, `catalog_recovered=1`.

**One relation is outside this rule, by ratification** (`docs/rules/rules.md`
§5). `sys.access_stats` is written **unlogged**: its whole content is a
statistic, which invariant 8 already prices as performance and never as a
result, so it is neither redone nor undone and a mount that finds it
damaged **discards** it (`Catalog::ResetAccessStatsIfDamaged`). Read the
paragraph above as "every catalog write except that one" — the exception
is sole, and a second relation would have to be shown to meet the same
test rather than to resemble this one.

Two contract consequences, stated because nothing else states them: **a
failed DDL statement inside an explicit transaction poisons the session**,
exactly as a failed DML statement does — the common refusal
(`EXISTS oid=…`) is not an `ERR` and does not poison. And **a refused
autocommit DDL pays a `TXN_BEGIN`/`TXN_ABORT` pair**, as refused
autocommit DML does.
