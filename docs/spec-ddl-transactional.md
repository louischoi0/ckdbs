# Transactional DDL

Status: **built 2026-08-16, extended 2026-08-18** (DT1-DT7 and DT9;
DT8, durability, deferred by name and never scheduled). `CREATE TABLE`
is atomic, isolated and consistent; `DROP INDEX` is atomic and isolated
**on core 0** since DT9 took §5a's open decision; `DROP TABLE` is atomic
only — §5 says exactly what each gets and §5a why they still differ.
Owning workplan: `docs/workplan-ddl-transactional.md`.

## 0. This reverses a recorded decision, deliberately

`docs/txn.md` §9 lists transactional DDL under *"Explicitly **not** open,
and out of scope"*, and §7 states the consequence: **"DDL is neither
logged nor transactional, and `CREATE TABLE` inside an explicit
transaction is not rolled back by `ROLLBACK`."**

That decision is reversed by direction, 2026-08-15. It is recorded here
rather than quietly contradicted, because a spec that says "out of scope"
while the code does it is worse than either answer alone. `txn.md` §7 and
§9 are amended to point here.

Worth noting the list §9 puts it in has already moved once: recovery was
on it too, and was built (RC01-RC11). The list is a snapshot of
priorities, not a set of impossibilities.

## 1. What "transactional" means here, precisely

Four properties are usually bundled under the word. They are separable,
they have very different costs in this engine, and **this spec commits to
the first three and defers the fourth by name**:

| | Property | Meaning for DDL | This spec |
|---|---|---|---|
| A | **Atomicity** | `ROLLBACK` undoes a `CREATE TABLE`; the relation does not exist afterwards | **v1** |
| B | **Isolation** | Another connection cannot see a relation whose creating transaction has not committed | **v1** |
| C | **Consistency of the pair** | A statement that creates a relation *and* inserts into it either leaves both or neither | **v1** |
| D | **Durability** | A committed `CREATE TABLE` survives a crash | **deferred, §7** |

D is deferred because it is not a DDL problem: catalog writes are
unlogged and the catalog is not recovered (`docs/known-gaps.md`, RV3).
Building A-C does not make D worse and does not depend on it — an
uncommitted DDL is invisible either way, and a committed one is exactly
as durable as every catalog write is today. **A `SHOW META` or manual
must not claim durable DDL until §7 lands**, and the workplan carries
that as an obligation rather than an afterthought.

## 2. The mechanism, and why it is smaller than it looks

Three facts about the code make this tractable:

1. **Catalog rows are already MVCC tuples.** They live in heap pages and
   carry invariant 12's header — `trx_id:48 | undo_ptr | data_len |
   flags` — exactly like user rows. Nothing about the *format* changes.
2. **They are stamped `kBootstrapXid` (= 1 = `kAlwaysVisibleTrxId`)**, by
   one function: `InsertRow()` in `src/catalog/catalog.cpp`, called with
   that constant from every DDL path. That is the whole reason they are
   visible to every read view.
3. **Catalog reads do not apply the visibility predicate.** `ScanAll` and
   its single-row sibling accept any live slot. A dead slot is skipped
   because `PageView::ReadTuple` reports `NotFound`, and that is the only
   filtering there is.

So **isolation** is: stamp the real transaction id, and filter catalog
reads by the reader's view. While the creating transaction is live its id
sits in every other view's in-flight set, so its rows are invisible to
them and visible to itself. That is the whole of property B.

### Atomicity is not free, and an earlier draft of this spec said it was

This section claimed atomicity fell out of visibility — that an aborted
transaction's rows are never seen because its id never commits.
**That is wrong, and the correction is load-bearing.**
`txn::ReadView::Visible` answers *"below the high-water mark and not
in-flight"* → **visible**. It has no notion of "aborted". Once the
aborting transaction is out of the live set, a view minted afterwards
reads its id as committed — the same mechanism `txn.md` §8 already
describes for the crash case.

The engine does not hide aborted work by visibility. It hides it by
**compensation**: `TransactionManager::Abort` walks the transaction's
trail in reverse and physically undoes each mutation, and for an insert
that is `PageView::RetireSlot`.

So DDL must do what every other write does — **register its catalog row
insert on the transaction's trail** (`NoteInsert`), so `Abort`
compensates it. That is not a new mechanism; it is the existing one,
applied to a page the catalog happens to own. What it needs is the
`(page_id, slot)` of the row, which `InsertRow` currently discards.

The consequence for planning: **isolation and atomicity are separate
phases**, and only the first is delivered by the read filter.

## 3. Invariants this must not break

- **Invariant 12 is untouched**: the header stays 20 bytes, and no
  `xmax` appears. A DROP delete-marks exactly as a user `DELETE` does.
- **Ids are burned, never reused** (invariant 11 / K1). A rolled-back
  `CREATE TABLE` consumes its oid and its Keystone id permanently. This
  is not a leak to be fixed later; it is the issue-once contract, and
  DDL gets no exemption.
- **A peer may not write the catalog** (crosscore.md P6). Nothing here
  changes that: DDL runs on core 0, and a peer's view of the catalog is
  still "drop the cache and re-read".
- **Bootstrap rows keep `kBootstrapXid`.** The well-known types,
  namespaces and the catalog's own descriptors are not transactional and
  must remain visible to every view, including a view minted before any
  transaction existed. Only *user* DDL takes a real id.

## 4. The hard part: the cache

`CatalogCache` memoizes name→oid and oid→`TableAccess`. It is
**per-instance and snapshot-blind**, which is exactly wrong once two
transactions on one core can disagree about whether a relation exists.

Three options, and this spec does not pick one — see §6:

- **(a) Bypass.** A session that has performed DDL in its current
  transaction reads the catalog uncached until it commits or aborts.
  Cheapest to reason about; costs a page scan per catalog lookup for
  that one session, and only while its transaction is open.
- **(b) Overlay.** The transaction keeps a small private map of what it
  created and dropped, consulted before the shared cache. Faster, and
  another place for "what exists" to be answered — the two-homes problem
  this codebase keeps finding bugs in.
- **(c) Snapshot-keyed cache.** Correct in general, and far more machinery
  than this feature justifies today.

**Recommendation: (a).** DDL inside a transaction is rare, the penalty is
scoped to the transaction that did it, and it introduces no second
answer to "does this relation exist".

Whatever is chosen, one thing is already known and must be respected:
**`Catalog::catalog_version()` is not a sound freshness guard** —
`InvalidateFromPeer()` clears the cache without bumping it, deliberately
(`docs/known-gaps.md`). Any cache work here inherits that.

## 5. What is in scope for v1

**v1's scope is complete as of 2026-08-16.** This section briefly said
`CREATE INDEX` / `DROP INDEX` were not built — true for a few hours
between the table statements landing and the index pair following. Both
now ship, so the original list stands as written.

Built, and what each actually gets:

- **`CREATE TABLE`** — atomic, isolated, rolled back by `ROLLBACK`. All
  four properties §1 lists except durability.
- **`DROP TABLE`** — **atomic only**, and deliberately not isolated;
  §5a is the whole argument, and it is a property of in-place overwrites
  with no undo chain rather than a gap to fill in later.
- The autocommit path is unchanged in behaviour: a bare `CREATE TABLE`
  commits immediately, exactly as today, and a bare `DROP TABLE` still
  retires its dependent rows rather than delete-marking them.
- Mixed statements: `BEGIN; CREATE TABLE t ...; INSERT INTO t ...;
  ROLLBACK;` leaves no relation and no rows.

- **`CREATE INDEX`** — atomic and isolated, exactly as `CREATE TABLE`.
- **`DROP INDEX`** — **atomic and isolated again as of 2026-08-18
  (DT9), and the round trip is worth reading in §5a.** It shipped as
  "atomic and isolated" on 2026-08-16; that was wrong, so it was refused
  inside a transaction; DT9 fixed the read the claim actually depended on
  and the refusal was withdrawn. The isolation claim is **core-0-scoped**
  — see §5a's last paragraph for what that means and when it stops being
  enough. Outside a transaction it behaves exactly as it always did.

**Not built, and each is now mechanical rather than open.** `ALTER
TABLE`, patterns, cabins, assertions and foreign keys stay
non-transactional. Each writes its own catalog page
and can adopt the mechanism the two table statements proved: stamp the
transaction's id, register what was written on its trail, and let
`Abort` compensate. **Nothing new has to be decided for the ones that
only insert rows.** An index drop is the exception worth checking first
— if it retires rather than delete-marks, it inherits §5a's limit and
DT5's terminating-sweep trap with it.

### 5a. `DROP TABLE` is atomic, and deliberately **not** isolated

Built 2026-08-16 as option (b) of DT5's decision. A drop inside a
transaction **delete-marks** its dependent rows instead of retiring them
and records the `sys.objects` retype's before-image, both on the
transaction's trail — so `ROLLBACK` clears the marks and rewrites the
tombstone back to a live table, restoring the relation and its rows.
Autocommit still retires, exactly as before.

**A claim this section made was wrong, and the correction is the more
useful statement.** It said the limit belongs to the `sys.objects`
*retype*, and offered `DROP INDEX` as the contrasting case that "proves"
it — an index drop delete-marks one row whose payload survives, so a
filtered reader still sees the index.

**That reasoning generalised from one surface without checking the
others.** `SHOW INDEXES` filters; `InitTableAccess` does not. It builds a
relation's index list through `ListIndexes()` with a **null view**, so
index maintenance and planning treat a delete-mark as done the moment it
is written. During an uncommitted `DROP INDEX`, another session's
`INSERT` writes no index entry — and if the drop rolls back, the index
returns *missing that row*, and a probe answers a committed row with
nothing. **A wrong query result, not an early view of the schema.**

So the limit is not about the retype. It is: **any catalog change that
unfiltered readers act on cannot be isolated**, and every internal
catalog read is unfiltered. A delete-mark is only isolable where every
reader of that row filters — which is true of `sys.objects` name lookups
and false of `sys.indexes`.

`DROP INDEX` inside a transaction was therefore **refused** rather than
answered wrongly, until DT9 below made the unfiltered read itself
correct. `DROP TABLE` stays atomic-not-isolated, and DT9 does not change
that — see the retype paragraph, and the correction beneath it.

**Other sessions see the drop immediately, before it commits.** That is
not an oversight, it is what option (b) costs. The `sys.objects` retype
is an *in-place overwrite*, and a catalog row has no undo chain
(`txn.md` §7) — so the prior image exists only in the aborting
transaction's own trail, and there is nowhere for another reader to
recover it from. Isolating a drop needs undo *records* for catalog rows,
which is option (a) and is not built.

The consequence a user meets: between `DROP TABLE t` and the `COMMIT`
or `ROLLBACK` that resolves it, other sessions see `t` as already gone.
If the transaction rolls back, `t` comes back. Reads in that window are
not wrong about the rows — the data pages are untouched — they are early
about the schema.

Two smaller facts worth stating with it:

- **The sweep loop's termination changed meaning.** It runs until
  nothing matches, which a retired slot satisfies by disappearing and a
  delete-marked one does not. The transactional path therefore skips
  rows already marked; without that it re-marks the same row forever.
  This is why the original code's *"retired, not delete-marked"* comment
  was load-bearing twice over, not only for read semantics.
- **A committed transactional drop leaves its marked rows on the page**,
  where autocommit's retire reclaims the slot. Nothing purges either way
  (`known-gaps.md`), and both read as gone, so the difference is space
  rather than meaning.

### Which reads filter, and which deliberately do not

Every route into "does this relation exist" must answer the same way, or
the one that answers differently is the leak. Three classes, and the
membership is a decision:

- **Filtered — a statement's own resolution.** `SELECT` (through
  `exec::Compile`, its sub-chains, and `CompileWhere`), `INSERT`,
  `UPDATE`, `DELETE`, `DESCRIBE`, `SHOW TABLES`, `ALTER`, `DROP TABLE`,
  and a foreign key's parent lookup. These decide what a statement may
  touch, so they answer under the session's view.
- **Unfiltered by design — "does this name already exist".** The
  duplicate-name check in both `CREATE TABLE` forms. Filtering it would
  hide another transaction's uncommitted relation of the same name, both
  creates would succeed, and two rows would claim one name. Seeing
  everything refuses the second instead — the conservative half of §6's
  open decision, and the half that cannot corrupt anything. The cost is
  a refusal that can be spurious (the first transaction may roll back)
  and that names a relation the asker cannot see.
**The line between the two, learned the hard way.** A surface reporting
**schema objects** — which relations or indexes exist — is a resolution
route and must filter. A surface reporting **engine state** — statistics,
budgets, memory-resident structures — is a diagnostic and must not.
`SHOW INDEXES` was first grouped with the diagnostics, which let an
uncommitted `DROP INDEX` be visible to everyone while the rest of the
catalog hid it. Only a test asserting the isolation caught it.

- **Unfiltered by design — diagnostics.** `SHOW ACCESS`, `SHOW BUDGET`,
  `SHOW ASSERTIONS`, `SHOW CABINS`, and the name-rendering helper.
  (`SHOW INDEXES` was moved *out* of this list — see above.) These answer *"what does this instance hold"*, which is an
  operator's question, not a statement's. An operator debugging a stuck
  transaction needs to see the pages and budget it is consuming; hiding
  them would make the tool useless exactly when it is needed. Also
  unfiltered, and unaffected either way: `ALTER`'s system-relation guard,
  which tests an already-resolved oid against bootstrap rows that every
  view sees.

Concurrent DDL from two transactions is §6's, and its conservative half
*is* built: the second create of a name in use is refused. What stays
open there is the message, not the behaviour.

### 5b. DT9 — what an unfiltered read does with an open delete-mark

**Decided and built 2026-08-18**, taking the decision §5a left open.

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
branch in `src/catalog/catalog.cpp`, which is the only *reader* of a
catalog delete-mark in the tree. §5a estimated "about twenty sites";
`ScanAll` has 16 call sites, three of which already pass a view. The
predicate is `txn::TransactionManager::IsInFlight`, a walk of the live
list rather than a minted `ReadView`, because the caller wants one bit
and a view is a 528-byte array copy.

**"No longer in flight" is safe to read as "committed"** for exactly one
reason, and it is an ordering fact rather than a definition:
`TransactionManager::Abort` compensates the entire trail *before* it
clears `active_`. A mark whose deleter has gone inactive is a mark no
rollback is coming for. If that order is ever inverted, this rule breaks
silently — a reader would treat an about-to-be-reversed mark as final.

**The catalog asks only when it has a manager to ask.** `Catalog`
carries a `SetTransactionManager` handle, armed by the
`CommandDispatcher` constructor — the one place a catalog and a manager
are known to belong together, so a new construction site cannot silently
keep the pre-DT9 answer. Left null, every unfiltered read answers
exactly as it did before: bootstrap, recovery and a test over a bare
store have no in-flight transaction to be wrong about.

**A mark left by a transaction from a previous mount is the one case
where this rule can answer wrongly, and it is stated rather than
patched.** An earlier draft of this section claimed such a mark "reads
as final". It does not, necessarily: `txn/trx_id.hpp` says by name that
the id ceiling is unlogged, so a crash between the in-memory raise and
the page reaching the platter **reissues the block on the next boot**.
A committed transactional `DROP INDEX` leaves its deleter's id on a
catalog page, which persists; if the next mount reissues that id, then
while the new holder is open `IsInFlight` answers true, the dropped
index is re-armed by `InitTableAccess`, and probes read a btree missing
every row written since the drop. Silently missing rows — where the
pre-DT9 rule answered correctly.

No cheap guard separates the two: a reissued id is at or above this
mount's floor, exactly like a live one. It belongs beside `txn.md` §8's
accepted post-crash family and RV3 in `docs/known-gaps.md`, and the
answer that would remove it rather than document it is **DT10, proposed
and not built**: retire every delete-marked catalog row at mount, before
the listener binds. A mark from a previous mount is unresolvable anyway
with no catalog recovery, so finalizing it is the only answer that
exists — and the same sweep would purge the marks that otherwise
accumulate forever, one per column and per index of every transactionally
dropped relation, re-scanned on every catalog cache miss. One sweep of
nine pages. Not taken here because it is new behaviour at mount and
needs its own decision.

**The claim is core-0-scoped, and must be written that way.**
`IsInFlight` answers about one core's `live_` list. That is every
writer's core only while CC3 refuses cross-core writes and core 0 alone
listens; the day DML shipping lands, a peer's index maintenance can meet
a core-0 deleter it cannot see, and this rule needs a cross-core commit
oracle before `DROP INDEX` may be called isolated outright. Saying
"isolated" without the scope would repeat exactly the overclaim the rest
of this section exists to correct.

**The cache had to learn the same thing, and this was the step's one
real bug.** `EndDdlScope` invalidated the catalog cache on rollback
only, reasoning that "a commit leaves the rows in place, so what was
cached about them stays true". DT9 retires that reasoning: **commit is
the moment a delete-mark starts counting.** A cache filled during an open
`DROP INDEX` holds the index deliberately — that is the whole point — and
holding it past the commit keeps maintenance writing entries for an index
that is gone, and never tells a peer to re-read. Invalidation is now
unconditional on a DDL-holding transaction resolving, either ending.

**A correction to §5a's own estimate of the payoff.** §5a said this fix
"would let both drops isolate". It does not. `DROP TABLE`'s exposure is
the `sys.objects` **in-place retype**, and a filtered `ScanAll` *skips* a
row whose writer it cannot see — so an outsider's name lookup answers
`NotFound` and the relation vanishes rather than lingering. No rule about
delete-marks reaches an overwrite. Isolating `DROP TABLE` still needs
undo *records* for catalog rows, which is option (a) and is not built.

## 6. Open decisions — do not assume

- **The cache strategy** (§4): (a) bypass, (b) overlay, (c) snapshot-keyed.
  Recommendation (a), not yet ratified.
- **Two transactions doing DDL at once.** There is no lock manager
  (`txn.md` §5 puts lock-based blocking out of scope), and two
  uncommitted `CREATE TABLE`s of the *same name* would both succeed and
  one would lose at commit — or both would exist. Options: refuse the
  second at DDL time by scanning for an uncommitted row with the same
  name (cheap, and a refusal rather than a corruption), or accept
  last-writer-wins. **Refusing is the recommendation**; nothing is built
  until this is ratified.
- **What `SHOW META` and the manual say** before §7 lands. A user who
  reads "transactional DDL" will assume durability. The recommendation
  is that both say "atomic and isolated; not yet crash-durable" until it
  is true.
- **Whether a transaction that did DDL may be shipped** (crosscore).
  Today writes bind to a home core, which already covers it, but the
  interaction should be stated rather than inherited.
- **A cross-core commit oracle for DT9's rule** (§5b). `IsInFlight`
  answers about one core's live list, so `DROP INDEX`'s isolation is
  core-0-scoped. Sound while CC3 refuses cross-core writes; the day DML
  shipping lands, a peer's index maintenance can meet a deleter it
  cannot see. Not a defect today, and not something to quietly inherit.

## 7. Durability, deferred and named

A committed `CREATE TABLE` survives a crash only when catalog writes are
WAL-logged and replayed. That is RV3 in `docs/known-gaps.md` and is not
scheduled here. Until it lands:

- a committed DDL is exactly as durable as any catalog write today —
  which is to say, it depends on the page reaching the device;
- the *transactional* properties A-C hold within a running instance,
  which is what §1 claims and all it claims.

This ordering is deliberate. A-C are useful on their own (a failed
migration script leaves no half-built schema), they are cheap, and they
do not make D harder — the records D needs are about pages, and nothing
in A-C changes what a catalog page looks like beyond the `trx_id` field
that is already in it.
