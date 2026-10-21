# Transactional DDL

`CREATE TABLE` and `CREATE INDEX` are atomic, isolated, consistent and
durable; `DROP INDEX` is atomic and isolated on every core, inside a
transaction as well (§5e); `DROP TABLE` is atomic only — §5 says what each gets and §5a why they differ. §5e is
how `CREATE INDEX` and `DROP INDEX` fence a relation's writers on every core
(the relation `X`, since AT-S5e), and §5f is `CREATE ASSERTION`'s build,
fenced the same way.

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
reads by the reader's view. While the creating transaction is live the
instance's commit window holds no entry for its id, so its rows are
invisible to every other view and visible to itself (`txn.md` §4.1). That
is the whole of property B.

**Atomicity is not visibility.** `txn::ReadView::Visible` answers
"committed" for any id below the instance's floor; it has no notion of
"aborted" there, so once the floor has passed an aborting transaction's id
a view reads it as committed. The engine hides aborted work
by **compensation**: `TransactionManager::Abort` walks the transaction's
trail in reverse and physically undoes each mutation, and for an insert
that is `PageView::RetireSlot`. So DDL does what every other write does —
**registers its catalog row writes on the transaction's trail**
(`NoteDdlRows`, `NoteCatalogRowChanges`), even when the DDL statement
failed, so `Abort` compensates them. **The row whose own write failed is
not registered, because that write removes it** (AZ-S1). If the undo hook
refused, nothing names the row, so it is taken back whole
(`PageView::UnInsertTuple`) and the page is as it was. If the hook
succeeded and the row's record was refused, the undo record names the
slot, so the slot is retired in place and is never reused. That dead slot
is described by no record; it cannot refuse a later mount because the
refusal fail-stops the log (`docs/spec/wal.md` §6-5), so no later insert on
the page is logged and the page is never written back. Isolation and
atomicity are separate phases, and only the first is delivered by the read
filter.

## 3. Invariants this must not break

- **Invariant 12 is untouched**: the header stays 20 bytes, and no
  `xmax` appears. A DROP delete-marks exactly as a user `DELETE` does.
- **Ids are burned, never reused** (invariant 11 / K1). A rolled-back
  `CREATE TABLE` consumes its oid and its Keystone id permanently. This
  is not a leak to be fixed later; it is the issue-once contract, and
  DDL gets no exemption.
- **Every core writes the catalog**, under the page latch, and a DDL runs
  where its session is (`catalog.md` CT5); every other core's view of the
  catalog is "drop the cache and re-read" at its next boundary
  (`catalog.md` CT2).
- **Bootstrap rows keep `kBootstrapXid`.** The well-known types,
  namespaces and the catalog's own descriptors are not transactional and
  must remain visible to every view, including a view minted before any
  transaction existed. Only *user* DDL takes a real id.

## 4. The cache

`CatalogCache` memoizes name→oid and oid→`TableAccess`, per instance and
snapshot-blind. The rule that keeps it correct: **a view is minted only
while some transaction holds uncommitted DDL** (`CommandDispatcher::
ViewFor`), and "some" is the instance's: `InstanceVisibility::AnyDdlOpen`
counts every core's open DDL transactions. A transaction is counted
before its first catalog write - when its DDL scope opens, whether or not
it goes on to write - and uncounted after it is decided. Each core keeps
its own transactions in `ddl_txns_`, which the count is the sum of.
**A relation name resolves through `ResolveNames`**, which closes the
window between reading the count and reading the catalog: a reader that
read the count as zero resolves unfiltered, then asks whether the
instance's DDL generation (`InstanceVisibility::DdlGeneration`, moved by
every open, after the count) changed while it read, and resolves again
under a view if it did. A DDL moves the generation before its first
catalog write, so a reader that found its row sees the move. A write's
`WHERE` subqueries resolve through it too, and `CREATE INDEX` builds only
over the relation it resolved and borrowed. `DROP INDEX`'s index name
resolves through it, and again under the relation `X` under a view taken
once the scope counted the drop; `C_LOAD_BEGIN`'s bind resolves through
it (`CommandDispatcher::ResolveRelationName`); `CREATE CABIN`, written with
no transaction, resolves under a committed-now view, so its own row never
names a relation that has not committed - the creating session's own
included. **A statement head reads the count before it revalidates the
cache**, and `ViewFor` takes the unfiltered path only if neither that
reading nor its own sees a DDL open: a closing DDL moves the schema word
before it uncounts, so a head that read the count as zero read the word
after every move a closed DDL made. A DDL that opens and closes wholly
inside one statement, after its head, is not covered (`known-gaps.md`).
With none in flight every catalog row is
either a bootstrap row or a committed one, so an unfiltered, cached read
is correct for every reader. While one is open, a statement resolves
names under a view — its own transaction's, or a fresh committed-now view
in autocommit — and a filtered lookup deliberately bypasses the shared
cache, costing a catalog page scan per resolution for exactly as long as
isolation is at stake. Each statement takes its view boundary once
(`EnsureStatementBoundary`), so two resolutions in one statement never
disagree.

`Catalog::catalog_version()` is not a sound freshness guard (`catalog.md`
CT1 says why), and nothing here leans on it.

## 5. What is in scope

Built, and what each gets:

- **`CREATE TABLE`** — atomic, isolated, durable, rolled back by
  `ROLLBACK`. `BEGIN; CREATE TABLE t ...; INSERT INTO t ...; ROLLBACK;`
  leaves no relation and no rows. **One name, one row on every core since
  AT-S17**: the duplicate check and the insert are one hold of
  `sys.objects`' root page, and the check refuses a name another core's
  open drop has freed, answering `TXN_CONFLICT retryable=1` (`catalog.md`
  CT7, which also covers
  `RENAME TO`, `CREATE NAMESPACE`, `RENAME COLUMN`, index and assertion
  names, and a column's cabin). **A `CABIN` column's Cabin is a row of
  the `CREATE TABLE`**: its `sys.cabins` row carries the transaction's id
  and is on its trail, so `ROLLBACK` and the mount's undo of a loser
  retire it with the relation (`cabin.md` §8.1). The `sys.fkeys` rows a
  `REFERENCES` clause writes are not on the trail, and stay (below).
- **`DROP TABLE`** — **atomic only**, and deliberately not isolated;
  §5a is the whole argument. Every production drop delete-marks its
  dependent rows, autocommit's implicit transaction included; a drop at
  the bootstrap id retires them, and only tests reach one (BF-Q12 (a)).
- **`CREATE INDEX`** — atomic and isolated, exactly as `CREATE TABLE`,
  on every core: it takes the relation `X` and builds where its session is
  (§5e).
- **`DROP INDEX`** — atomic and isolated on every core, admitted inside a
  transaction (§5b, §5e); a `CREATE INDEX` of the name it dropped is
  admitted in the same transaction (BY-R8, `catalog.md` CT7).
- **`REINDEX INDEX`** (BY) — a `DROP INDEX` and a `CREATE INDEX` of the
  same name in one transaction, under one relation `X`, so atomic and
  isolated as they are; autocommit only, refused `NotImplemented` inside
  one (`index.md` §12).
- **`CREATE ASSERTION`** — built and published where its session is,
  on any core, since AT-S5d; admitted inside a transaction
  (§5f).
- **`ALTER TABLE ... ADD COLUMN` / `DROP COLUMN`** (BW, `alter.md` AL11) -
  atomic and writer-isolated on every core, §5e's shape: the relation `X`
  before the first catalog write, `ADD` inserting its `sys.columns` row and
  `DROP` overwriting its row as the transaction's (the prior image to the
  undo log), so `ROLLBACK` and mount recovery undo either. Readers are not
  isolated: the row is rewritten in place, so another session's read can
  see the undecided column set, never a wrong value (BW-Q8 (a)).
- The autocommit path is unchanged in behaviour: a bare `CREATE TABLE`
  commits immediately.

**Not transactional:** `ALTER TABLE`'s renames, `CREATE CABIN` and
`DROP CABIN`, assertions and foreign keys.
Their catalog writes are logged and durable (§7) but not undone by
`ROLLBACK`. **The renames still take the relation `X`** (the operator's
ruling of 2026-10-21, `alter.md` AL4a), under §5f's holder - a transaction
of the statement's own that writes nothing and is rolled back when the
statement ends, so the `X` is held across `AwaitDdlDurability` (§7) and a
refused rename poisons nothing. A session whose own transaction already
holds the relation's `X` takes no second holder; one holding a weaker mode
on it is refused `TxnConflict` at once.

### 5a. `DROP TABLE` is atomic, and deliberately **not** isolated

A drop inside a transaction **delete-marks** its dependent rows instead of
retiring them and records the `sys.objects` retype's before-image, both on
the transaction's trail — so `ROLLBACK` clears the marks and rewrites the
tombstone back to a live table, restoring the relation and its rows. An
autocommit drop does the same inside the implicit transaction `BeginWrite`
opens (`drop-table.md` DT5).

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

**One shape is narrowed and the rule is not** (AO-S6e-b): since the drop
takes the relation `X` from the lock family, a reader **already walking**
the relation holds it in `IS` and the drop waits for that statement to end
(`drop-table.md` DT7). A positioned reader therefore finishes against a
live schema instead of meeting its re-`Bind`'s error. Every other reader is
exactly as this section describes it - a read that starts after the grant
takes no borrow, because a refused read borrow leaves the reader reading
on, so it sees the drop before it commits like any other outsider. The
guarantee is "a positioned reader is not overtaken", which is a narrower
statement than isolation and is why this section keeps its title.

The general limit under it: **any catalog change that unfiltered readers
act on cannot be isolated**. A delete-mark is isolable only where every
reader of that row filters or applies §5b's rule.

Two smaller facts:

- **The sweep loop's termination.** It runs until nothing matches, which
  a retired slot satisfies by disappearing and a delete-marked one does
  not, so the transactional path skips rows already marked.
- **A committed drop leaves its marked rows on the page**, autocommit's
  included; §5c and §5d retire them later. Both read as gone, so the
  difference is space rather than
  meaning.

### Which reads filter, and which deliberately do not

Every route into "does this relation exist" must answer the same way, or
the one that answers differently is the leak. Three classes, and the
membership is a decision:

- **Filtered — a statement's own resolution.** `SELECT` (through
  `exec::Compile`, its sub-chains, and `CompileWhere`), `INSERT`,
  `UPDATE`, `DELETE` (the subquery relations of their WHERE resolve under
  the session's view too, through `ResolveNames`), `ALTER`, `DROP TABLE`, a foreign key's parent
  lookup, and **the schema-object views** - `sys.tables`, `sys.objects`,
  `sys.columns` (so `DESCRIBE`), `sys.namespaces`, `sys.indexes` and
  `sys.fkeys`, and with them `SHOW TABLES`, `SHOW NAMESPACES`,
  `SHOW INDEXES` and `SHOW FKEYS`, each one `SELECT` over its view
  (`sys-views.md` §3, BO-Q2). These decide what a statement may touch, or
  report which schema objects exist, so they answer under the session's
  view; a foreign key is listed where its child relation is visible.
  **`CREATE ASSERTION` is filtered under a settled view instead** (§5f,
  `assertion.md` §8.1): it reads settled state, so a relation an open
  transaction created - the session's own too - is not its target, and
  under repeatable read it may target a relation committed after `BEGIN`.
- **Unfiltered by design — "does this name already exist".** The
  duplicate-name check in both `CREATE TABLE` forms. Filtering it would
  hide another transaction's uncommitted relation of the same name, both
  creates would succeed, and two rows would claim one name. Seeing
  everything refuses the second instead — the half that cannot corrupt
  anything. The cost is a refusal that can be spurious (the first
  transaction may roll back) and that names a relation the asker cannot
  see (`EXISTS oid=…`, which is not an `ERR` and does not poison the
  session).
- **Unfiltered by design — diagnostics.** Every engine-state view
  (`sys-views.md` §3) - `sys.access_stats`, `sys.relation_stats` (so
  `SHOW BUDGET`), `sys.patterns`, the statistics views and `sys.meta`
  among them - and the name-rendering helper. **The views that report a
  declaration's enforcement are diagnostics too** - `sys.assertions`,
  `sys.cabins`, `sys.transitions`, whose rows say whether and how a
  declaration is enforced, as `SHOW ASSERTIONS` and `SHOW CABINS` were
  before them (BO-Q16 (a)). These answer *"what does this instance hold"*, which is an operator's
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
`TransactionManager::NoneInFlightBelow()` once per scan — the instance's
floor candidate, below which no transaction is running on any core and
none will begin, so it holds for the whole scan however many transactions
another core begins meanwhile. A deleter below it is settled; only a mark
whose deleter is at or above it asks `TransactionManager::IsInFlight`,
which reads every core's in-flight table (`instance_visibility.hpp`,
AX-S1) — one bit, where a minted `ReadView` would be a view.

**"No longer in flight" is safe to read as "committed"** for exactly one
reason, and it is an ordering fact rather than a definition:
`TransactionManager::Abort` compensates the entire trail *before* it
retires the id from its core's in-flight table (`txn.md` §4.1). A mark
whose deleter no table holds is a mark no rollback is coming for. If that
order is ever inverted, this rule breaks silently.

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

**The predicate is the instance's** (AX-S2,
`instructions/v3.0.0/workorder-ax-inflight-publication.md`, AX-Q2): every
core resolves the index an open `DROP INDEX` marked, as the dropping core
does, and stops resolving it at the commit. A writer needs more: the drop
holds the relation `X` and moves the
schema word before releasing it (§5e), so a writer that resolved the
relation while the drop was open writes nothing until the decide and
re-resolves after it.

**The cache learns it at both endings.** `EndDdlScope` invalidates the
catalog cache unconditionally when a DDL-holding transaction resolves,
commit or rollback: commit is the moment a delete-mark starts counting, and
a cache filled during an open `DROP INDEX` holds the index deliberately —
holding it past the commit would keep maintaining an index that is gone,
and never tell another core to re-read.

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
by the time any other core starts, core 0 has done it.

**What it costs.** One forward pass over the catalog root chains.
`RetireSlot` sets the dead flag in place and never renumbers slots behind
the walk, so one pass suffices. A crash mid-sweep leaves exactly the
state it started from, which the next mount sweeps again. `SHOW META`
reports `catalog_marks_finalized`; zero is what a clean shutdown produces.

### 5d. Delete-marks purge on core 0's tick, horizon-gated

The in-mount sibling of §5c's sweep: `Catalog::PurgeSettledDeleteMarks()`
retires every delete-marked row whose deleter is **resolved for every
reader** — below the instance's floor, or committed at or below every live
and future snapshot (`TransactionManager::ResolvedForEveryReader()`,
`txn.md` §4.1). **Core 0's 50 ms `system` tick runs it**
(`CommandDispatcher::SweepSettledDeleteMarks`, BP-H11, since BP-S4), so a
peer's `DROP` is swept as soon as it settles.

**Why the horizon licenses what §5c's mount-only rule forbade.** After
mount a mark may belong to a transaction that is still open — or to a
committed drop an old live view still cannot see, which would resurrect
the row for that reader's filtered reads. The two-branch test is precisely
the missing proof. A deleter **below the instance's floor** is resolved,
and a loser's marks are compensated away, so a mark still on the page came
from a winner — and an active transaction holds the floor at or below its
own id, so no live deleter is ever below it. A deleter the **window** still
carries at or below every live and future snapshot is committed to every
view that can be minted. Either way the row it marked is gone by every
route — filtered reads see the drop, unfiltered reads settle the mark by
the same comparison. A
rollback clears its own marks synchronously, so no aborted transaction's
mark survives to be asked about.

**Why on the tick, and when it sweeps.** Placement is not what makes it
sound: every view that can read a superseded version is registered, and a
check view needs no lease by proof (`txn.md` §4.1), so the predicate holds
on any core at any moment, for peers' running statements as for core 0's. A
sweep write-fetches every catalog page even when it retires nothing, so the
tick sweeps only while a mark is pending, and only when the pending count
or the settlement has moved since the last sweep
(`TransactionManager::SettlementNow`: the floor, the pending-commit bound,
and the horizon capped at the commit ceiling). Under an open older reader
the horizon stands still, so commits elsewhere do not make it sweep; with
no reader the bound follows the commit ceiling, and a committed deleter
settles on the next tick. A mark whose deleter has not cleared the horizon
survives to a later sweep or to §5c at the next mount. **One core sweeps**,
a **placement and not an authority**: two cores walking the same chains at
once is what it prevents, and core 0 is the one because it is always
present. What makes a single sweeper sound is AN-S2: the predicate is
instance-wide, so any core's sweep judges every mark by the same floor and
horizon.

**Because one core sweeps, the gate it reads is the instance's**
(AT-S5b): a counter per `Catalog` would hold a mark another core's `DROP
TABLE` raised where the sweeping core never reads it. The gate is one
atomic on `Expeditor`, `fetch_add` at the mark and a delta at the
resettle; `catalog.md` CT6 carries the counter's own rule. A failed sweep is a maintenance
failure, not the statement's: the marks it left are exactly as reachable as
before, so it is logged and the reply stands.

**No version bump, deliberately.** Every retired row was already gone to
every reader — that is what the horizon proves — so no cached answer
changes, and a bump would move the schema word for every core and stale
this instance's bound statements for nothing. A crash mid-sweep leaves
the state it started from.

**Observability.** `SHOW META` prints `catalog_marks_purged` — this
mount's own retirements — and `catalog_purge_sweeps`, the sweeps that ran — beside the recovery report's
`catalog_marks_finalized`, which counts a previous mount's leftovers.

**What it bounds.** `marks` is bounded; the sweep's cost is not: retired
catalog slots are never reclaimed, so every resolution sweeps every slot
DDL ever occupied. The purge exists to bound `marks` and remove §5b's
ambiguity, never to speed reads — a retired slot's `NotFound` path costs
slightly more than a settled mark's one comparison.

### 5e. `CREATE INDEX` and `DROP INDEX` take the relation `X`

**Since AT-S5e both statements take the relation `X` before their first
catalog write, on whichever core the session is**, and build or mark there
(`workorder-at-m3-uniformity.md` AT-S5e, D6). The borrow is `DROP TABLE`'s
(§5a, AO-S6e-b): the DDL transaction's, held to its decide, and waited for
on the table's own slot when a writer or a positioned reader holds the
relation.

**What it fences.** Every writer of the relation holds its `IX` from its
first intention until its transaction decides - an `INSERT` asks at
`InsertParsed`, ahead of its assertion admission; an `UPDATE` or `DELETE` at its declared borrow or, where its
predicate declares no key window, at its first qualifying row. So a DDL's
grant is a moment no writer is mid-statement, and a writer that arrives
while the DDL is undecided parks on the slot and re-runs after the decide.
**Two things make the re-run see what the decide did.** The decide moves
the schema word before its borrows are released
(`Transaction::NoteWroteCatalog`), so the re-run's task boundary drops the
memo it resolved while the DDL was open (since AX-S2 made §5b's predicate
the instance's, no cell shows this load-bearing for a rolled-back `DROP
INDEX`; `known-gaps.md`, Testing). And a writer's *first* intention checks that the
word has not moved since its own boundary (`Catalog::MemoIsCurrent`): the
intention comes after the resolution, and a DDL could have taken the
relation, published and released in between; moved, the statement runs
again before it writes anything. A writer on another core waits the same way: the slot is flipped by
the release from whichever core releases, and the kick that follows wakes
the waiter's core.

**Atomic and isolated, on every core.** A `CREATE INDEX` backfills with no
concurrent writer, so the finished index holds every row, and nothing names
it until its commit; a rollback leaves the tree in its anchor slot, as a
committed drop's stays, and each is reclaimed by an index job queued at
the decide, or with its relation if that is dropped first (`index.md` §12, BY;
`drop-table.md` DT1). A `DROP INDEX` inside a transaction is admitted: a writer on
another core resolves the relation with the index still in it (§5b, since
AX-S2), and in any case writes nothing until the drop decides, and the
decide moves the word before it releases, so the writer re-resolves and
maintains the index a rollback restored.

### 5f. `CREATE ASSERTION`, built where its session is

**The statement runs where its session is**: the checks and the id, the
build, the publish and the adoption into the instance's one assertion
registry (`assertion.md` §6.1).

**Admitted inside an explicit transaction**: `InsertAssertion` writes
under no transaction, so the assertion enforces before `COMMIT` and a
`ROLLBACK` leaves it in place. One consequence is named rather than hidden:
a transaction that has already written the relation meets the build's
in-flight refusal (`assertion_build.cpp`'s `kBusy` → `TXN_CONFLICT`), and a
retry inside that transaction cannot succeed - its own row stays in flight
until it ends - while each attempt burns an assertion id and orphans a
chain. Run the declaration first, or outside the transaction that writes.

**Atomic**: one publishing event, the `sys.assertions` row. A refused build
or a failed publish adopts nothing, and the chain the build wrote orphans,
exactly as a dropped assertion's pages do.

**Fenced from every writer, on every core, since AT-S5e** (AT-0 item 13,
the operator's mark). The build takes the relation `X` as §5e's statements
do, so it waits for every open writer of the relation and every writer that
arrives parks until the directory is adopted and then re-runs against it,
so no write lands where the scan has passed uncounted. **Its holder is a transaction of the statement's own**,
not the session's: it writes nothing and is rolled back when the statement
ends, so the relation is released at the statement's end rather than at the
session's `COMMIT`, and a `CREATE ASSERTION` the build itself refuses still
poisons nothing. **The run owns the holder** (`DdlBuildLock`, owned by
`server::AssertionBuildRun` and `server::TransitionBuildRun` from the ask
on): since A1 stage 2 the build yields its core between slices of relation
leaves (`sched.md` §3), and a build handed on keeps the `X` across every
yield until its tail has published and synced, then releases it
(`DdlBuildLock::End`); a run destroyed before its tail releases it as the
statement's end would. **A cancelled `CREATE ASSERTION` or `CREATE
TRANSITION` fails an open explicit transaction** (the operator's ruling of
2026-10-21): the build takes the session's cancel before its first slice
and at each slice boundary, ends `CANCELLED` with nothing published, and
leaves the transaction failed until `ROLLBACK`, as a cancelled statement
does (`protocol.md` §10). This changes the earlier contract that a failed
`CREATE ASSERTION` poisons nothing; the build's own refusals - a violation,
an in-flight row, a taken name - still poison nothing.
The one holder it cannot wait for is the session's own transaction, having
written the relation (or holding it at any mode) - that is refused
`TxnConflict` at once by the holder's own-transaction test, before the
build. **The memo is made current right after the grant**
(`Catalog::Revalidate`), before the check view is minted and the name is
resolved again: a rename, `ADD COLUMN` or `DROP COLUMN` another core
finished between the statement's boundary and an uncontended grant would
otherwise leave the build reading the relation's schema from before it.
`ALTER TABLE`'s renames take the same holder (§5).

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
result, so it is neither redone nor undone and every mount **discards**
it (`Catalog::DiscardAccessStats`), cutting a damaged chain. Read the
paragraph above as "every catalog write except that one" — the exception
is sole, and a second relation would have to be shown to meet the same
test rather than to resemble this one.

Two contract consequences, stated because nothing else states them: **a
failed DDL statement inside an explicit transaction poisons the session**,
exactly as a failed DML statement does — the common refusal
(`EXISTS oid=…`) is not an `ERR` and does not poison. And **a refused
autocommit DDL pays a `TXN_BEGIN`/`TXN_ABORT` pair**, as refused
autocommit DML does.
