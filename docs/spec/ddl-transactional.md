# Transactional DDL

Status: **built 2026-08-16, extended 2026-08-18** (DT1-DT7 and DT9;
DT8, durability, deferred by name and never scheduled). `CREATE TABLE`
is atomic, isolated and consistent; `DROP INDEX` is atomic and isolated
**on core 0** since DT9 took §5a's open decision; `DROP TABLE` is atomic
only — §5 says exactly what each gets and §5a why they still differ.
**§5e** adds the two-core case (PW1c-6b, 2026-08-25): a `CREATE INDEX`
whose relation another core owns is built by that owner and stays atomic
and isolated across both, and `DROP INDEX` on such a relation is refused
inside a transaction. **§5f** does the same for `CREATE ASSERTION`
(PW1c-6c, 2026-08-26) on a stronger premise — a Bound Cabin is written by
every later write to the relation, not only at build time — and with no
refusal window. Owning workplan: `docs/inflight/in-progress/workplan-ddl-transactional.md`
(this spec) and `docs/inflight/in-progress/workplan-peer-writer.md` §7c/§7d
(the two-core cases).

## 0. This reverses a recorded decision, deliberately

`docs/spec/txn.md` §9 lists transactional DDL under *"Explicitly **not** open,
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
unlogged and the catalog is not recovered (`docs/inflight/known-gaps.md`, RV3).
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
(`docs/inflight/known-gaps.md`). Any cache work here inherits that.

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
- **`CREATE INDEX` on a relation another core owns** — atomic and
  isolated across two cores: the owner builds the tree in its own stream,
  core 0 writes and commits the `sys.indexes` row. §5e is the whole
  account (PW1c-6b).
- **`DROP INDEX` on a relation another core owns** — refused inside a
  transaction (§5e), for the reason §5b's core-0 scope names: the owner's
  index maintenance cannot see core 0's deleter in flight. Autocommit is
  admitted, and a `DROP INDEX` on a relation core 0 owns is untouched.

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
mount's floor, exactly like a live one. **Closed by §5c**, which removes
the question rather than documenting it.

**The claim is core-0-scoped, and must be written that way.**
`IsInFlight` answers about one core's `live_` list. That is every
writer's core only while CC3 refuses cross-core writes and core 0 alone
listens; the day DML shipping lands, a peer's index maintenance can meet
a core-0 deleter it cannot see, and this rule needs a cross-core commit
oracle before `DROP INDEX` may be called isolated outright. Saying
"isolated" without the scope would repeat exactly the overclaim the rest
of this section exists to correct. **That day arrived for indexes on
2026-08-25** (PW1c-6b-4, `docs/inflight/in-progress/workplan-peer-writer.md`): a peer now
maintains its own relation's index, so a core-0 `DROP INDEX` on a
peer-owned relation is the exact meeting this paragraph warned of. It is
handled by **refusing that `DROP INDEX` inside a transaction** (§5e), not
by widening the predicate — a cross-core commit oracle is still what
would let it be admitted.

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

### 5c. DT10 — delete-marks are finalized at mount

**Decided and built 2026-08-18.** Every delete-marked catalog row is
retired at mount, on the system core, after recovery and before the
listener binds.

**Why it has to exist at all.** DT9 made a mark's meaning depend on
whether its deleter is in flight. A mark that outlived its mount has no
deleter to ask about — and worse, may have one that is not its own: the
transaction-id ceiling is unlogged (`txn/trx_id.hpp`), so a crash
reissues the block, and a live transaction wearing a committed dropper's
id makes a finished drop read as open. The dropped index is re-armed and
answers probes from a btree missing every row written since. §5b names
that exposure; this section is its answer.

**Retiring is the only available answer, not the conservative one.** A
mark whose transaction committed should be gone. A mark whose
transaction did not commit cannot be rolled back either — the trail that
would compensate it died with the process, and the catalog is not
recovered (RV3), so nothing can reconstruct the intent. Both already read
as gone to every unfiltered reader before DT9. What changes is that they
stop being *ambiguous*.

**A second effect that would justify it alone.** Nothing else ever purges
these rows. A transactional `DROP TABLE` leaves one mark per column, per
index and per foreign key, forever, re-read on every catalog cache miss.
The sweep is also the purge.

**Where, and why only there.** After recovery, so a mark this mount's own
log restored is included; before the transaction stack exists, so no live
transaction can own a mark it retires — which is what makes "retire every
mark" safe here and catastrophic anywhere else. The system core's alone:
a peer may not write a catalog page (P6), and by the time a peer mounts,
core 0 has done it.

**What it bounds, and what it does not.** Measured at `04ae010`
(`bench/results-ddl-catalog-read-ab.md`), DT9's cost on a cold catalog
resolution fits `marks × (0.4 ns + 0.45 ns × live)` — the `IsInFlight`
loop and nothing else. `live` is capped at 64 by `kMaxTrackedLiveTxns`;
**`marks` is capped only by this sweep, which runs once per mount.** So
accumulation is bounded across restarts and *unbounded within one
long-lived process*.

**Half of that product is now gone, and the other half cannot be.** The
`live` factor left the per-mark term on 2026-08-18: `ScanAll` takes
`TransactionManager::OldestActiveTrxId()` once per scan, and a deleter
below it is settled by definition — `live_` holds every running
transaction on this core, so an id below the smallest of them is not one
of them. The common mark, left by a drop that committed long ago, costs
one comparison; with nothing running the manager is not consulted at all.
Only a mark whose deleter is at or above the oldest active transaction
still pays the walk, and there is at most one such drop per open
transaction.

~~**`marks` itself needs a purge, and a purge cannot be built.**~~ This
paragraph was true when written (2026-08-18) and the prerequisite it
named fell the next day: a purge must not retire a mark some reader's
view still needs, consulting `live_` was not enough (a cross-core stage
holds its `AutocommitSnapshot` **across its parks**,
`remote_step_service.hpp` — a reader nowhere in `live_`), and that
missing record is exactly the reader registration
`docs/workplan-reader-registration.md` built. §5d below is the purge.
A mark now waits at most for its last older reader plus the next DDL
resolution, rather than for the mount.

**What it costs.** One forward pass over the catalog root chains.
`RetireSlot` sets the dead flag in place and never renumbers slots behind
the walk, so one pass suffices. Unlogged, like every catalog write — a
crash mid-sweep leaves exactly the state it started from, which the next
mount sweeps again. `SHOW META` reports `catalog_marks_finalized`; zero
is what a clean shutdown produces.

### 5d. Delete-marks purge at DDL resolution, horizon-gated

**Built 2026-08-19** (`docs/workplan-reader-registration.md` RR4). The
in-mount sibling of §5c's sweep: `Catalog::PurgeSettledDeleteMarks()`
retires every delete-marked row whose deleter has cleared the core's
**read horizon** (`TransactionManager::ReadHorizon()`, `txn.md` §4.1),
and `CommandDispatcher::EndDdlScope` runs it at every DDL resolution —
both endings, right after the cache invalidation.

**Why the horizon licenses what §5c's mount-only rule forbade.** §5c may
retire only at mount because afterwards a mark may belong to a
transaction that is still open — or to a committed drop an old live view
still cannot see, which would resurrect the row for that reader's
filtered reads. The horizon is precisely the missing proof: a deleter
below it is committed (an active transaction bounds the horizon at or
below its own id) and visible to every live and future view, so the row
it marked is gone by every route — filtered reads see the drop,
unfiltered reads settle the mark by the same comparison. A rollback
clears its own marks synchronously, so no aborted transaction's mark
survives to be asked about.

**Why at DDL resolution and nowhere hotter.** DDL resolution is the only
event that creates or settles a mark (autocommit DDL writes at
`kBootstrapXid` and retires directly, leaving none), it is rare enough
that a catalog page sweep costs nothing worth measuring, and the core is
between resolutions there — so no unregistered synchronous view is live,
which is the exemption `txn.md` §4.1's registration rule leans on. A
mark whose deleter has not cleared the horizon survives to the next
resolution or to §5c at the next mount; there is deliberately **no**
background cadence — that is a `maintenance`-group decision that belongs
with the undo purge (`txn.md` §9). **System core only**, by an explicit
gate at the call site: the horizon is per-core and blind to every other
core's readers, so a peer's — no transactions, no leases — answers
`UINT64_MAX` and would retire a mark whose deleter is live on core 0.
Unreachable while peers take no DDL, but the gate is what makes the
argument local instead of global.

**No version bump, deliberately.** Every retired row was already gone to
every reader — that is what the horizon proves — so no cached answer
changes, and a bump would broadcast `kCatalogInvalidate` to peers and
stale this instance's bound statements for nothing. Unlogged like every
catalog write (RV3): a crash mid-sweep leaves the state it started from.

**Observability.** `SHOW META` prints `catalog_marks_purged` — this
mount's own retirements, live dispatcher state — beside the recovery
report's `catalog_marks_finalized`, which counts a previous mount's
leftovers. The pair reads as one statement: what resolution reclaimed,
and what a reader carried across a shutdown for the mount to take.

**Measured 2026-08-19** (Release A/B `a10890e` vs `d84fdc3`,
`docs/workplan-reader-registration.md` has the tables). The sweep costs
~10 µs per DDL resolution on a young catalog, on resolutions that are
~900 µs fsync-dominated; a non-DDL ROLLBACK moved +0.3 µs, so the
`held_ddl` gate confines it. Two facts the design must carry honestly:

1. **The cost model is O(catalog slots ever occupied), not O(marks
   retired)** — retired slots are never reclaimed, so every resolution
   sweeps all DDL history: ≈39 ns per dead catalog row per resolution.
   Fine while DDL is rare; a DDL-heavy lifetime accumulates quadratic
   total sweep work, which is the catalog-reclamation gap
   `known-gaps.md` tracks, now with a number.
2. **The purge does not speed cold catalog reads — it slightly slows
   them** (+5.6–7.3 µs p50 at 200–10k rows): a retired slot's
   `NotFound` path costs ~10 ns more per slot than the settled mark it
   replaced costs as one comparison. The payoff is bounding `marks` and
   deleting DT9's ambiguity, never read speed; do not quote this
   section as a cold-read optimization.

### 5e. A relation another core owns: `CREATE INDEX` built by the owner

**Built 2026-08-25 (PW1c-6b, `docs/inflight/in-progress/workplan-peer-writer.md` §7c).**
`CREATE INDEX` is core 0's statement — the catalog has one writer — but
the tree it builds is *pages*, and a relation another core owns holds its
pages in that core's pool, stamped by that core's stream, with rows core 0
never faulted. Core 0 cannot backfill them. So the **build** moves to the
owner while the **catalog write** stays on core 0, and the statement is
two phases with a park between them (`crosscore.md` CC7's owner-builds
exception says why the pages may not travel the other way instead).

**Atomic.** The owner builds the tree in its own stream under `kNoTxnId`
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
until it commits (the ordinary DT1-DT7 filtering, not DT9's delete-mark
rule), and the owner's cache holds no index until `done(committed)` (or
the catalog-invalidation broadcast) drops it. So no session reads a partial
index and no write slips past unindexed.

**Two gaps a single-core `CREATE INDEX` does not have**, both owed forward
and neither a correctness defect today:

- **A window that expires.** The owner bounds its refusal window by
  `kIndexBuildPendingCeilingNs` (180 s) against a lost `done`; core 0
  bounds its park by `kIndexBuildReplyDeadlineNs` (60 s). The ceiling
  exceeds the deadline, so the owner never releases while core 0 is still
  waiting — but it does not exceed *deadline + a bound on the commit*, and
  no such bound exists. If the window expires before a late commit lands,
  writes are admitted that the published index would miss. Pre-existing in
  shape (the pre-lift gate keyed on the owner's own, equally stale, catalog
  view) and unreachable in practice at these timeouts; the real close is a
  bound on the commit leg, or the cross-core commit oracle §6 owes.
- **`SHOW INDEXES` on core 0** for a peer-owned relation reads the
  build-time root from the `sys.indexes` row — a foreign `InitTableAccess`
  does not read the owner's anchor — which a maintenance split can move, so
  a stale root walks a subtree and prints a plausible-wrong
  `entries=`/`height=`. Diagnostics only: a cross-core *read* downgrades an
  index probe to a scan before it ships (`step_descriptor.cpp`), so query
  answers never depend on core 0's stale root.

**`DROP INDEX` on a peer-owned relation** is the mirror hole, and the gate
lift (PW1c-6b-4 — a peer now maintains its own index) is what makes it
reachable. It is **refused inside a transaction**, for §5b's reason: the
mark's `BumpVersion` broadcasts before core 0 commits, the owner's DT9
predicate walks its own live list and cannot see core 0's deleter, so it
would drop the index from its view and maintain nothing before COMMIT — and
a ROLLBACK would then restore an index missing every meanwhile-write.
Autocommit keeps only the commit-failure window every DDL has and is
admitted; a `DROP INDEX` on a relation core 0 owns is untouched (DT9
isolates it).

### 5f. The same relation's `CREATE ASSERTION`, and why it is not §5e twice

**Built 2026-08-26 (PW1c-6c, `docs/inflight/in-progress/workplan-peer-writer.md`
§7d.)** The shape is §5e's — core 0 checks the declaration and issues the
id, the owner builds, core 0 publishes the `sys.assertions` row, the
statement parks between the two phases and is refused inside an explicit
transaction — and the *reason* is stronger. An index is built once from rows
core 0 cannot see; a Bound Cabin is **written by every subsequent write to
the relation**, so its pages have to be the owner's for the assertion's whole
life, not only at build time. That is why nothing about this can be fixed by
telling core 0's build to try harder.

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
nothing provides today.

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
- **A cross-core commit oracle for DT9's rule** (§5b, §5e). `IsInFlight`
  answers about one core's live list, so `DROP INDEX`'s isolation is
  core-0-scoped. **The meeting this warned of arrived for indexes**
  (PW1c-6b-4: a peer maintains its own index), and the conservative close
  is in place — `DROP INDEX` on a peer-owned relation is refused inside a
  transaction (§5e). The oracle is what would let it be admitted instead;
  until then the refusal stands, and no other cross-core DDL may lean on
  the predicate without the same guard.

## 7. Durability, ~~deferred and named~~ — **landed 2026-08-19**

RV3 built what this section deferred
(`docs/workplan-rv3-catalog-recovery.md`): catalog writes are WAL-logged
as the ordinary record types and replayed; every DDL statement runs
under a real transaction (D2 — autocommit DDL takes the implicit one
`BeginWrite` opens); a loser's catalog writes carry undo records the
mount rolls back through, appended *inside* the catalog's write points
so redo alone can never resurrect what undo cannot retire. A committed
`CREATE TABLE` whose pages never reached the device comes back by redo;
a committed `CREATE INDEX`'s backfilled tree travels as full page images
before the row that publishes it. `SHOW META`: `ddl_durable=1`,
`catalog_recovered=1`.

Two contract changes D2 carries, stated because nothing else states
them: **a failed DDL statement inside an explicit transaction now
poisons the session**, exactly as a failed DML statement does — before
RV3 the DDL handlers never reached `EndWrite`, so a syntax error left
the session usable; §6's per-transaction failure atomicity is why the
DML rule is the right one to inherit, and the common refusal
(`EXISTS oid=…`) is not an `ERR` and does not poison. And **a refused
autocommit DDL pays a `TXN_BEGIN`/`TXN_ABORT` pair**, as refused
autocommit DML always has.

The paragraphs below stand as the design record
of why A-C were built first:

This ordering is deliberate. A-C are useful on their own (a failed
migration script leaves no half-built schema), they are cheap, and they
do not make D harder — the records D needs are about pages, and nothing
in A-C changes what a catalog page looks like beyond the `trx_id` field
that is already in it.
