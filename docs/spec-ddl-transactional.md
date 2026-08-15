# Transactional DDL

Status: **specification, nothing built** (2026-08-15). Owning workplan:
`docs/workplan-ddl-transactional.md`.

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

So the change is: **stamp the real transaction id, and filter catalog
reads through the same `txn::Classify` predicate user reads already
use.** Atomicity then falls out for free — an aborted transaction's id
never commits, so no view ever sees its rows, which is precisely how an
aborted `INSERT` already behaves. **No undo record is required for
rollback**, and none is written.

The row an aborted DDL leaves behind is garbage that nothing reclaims.
That is consistent with the engine's existing posture (nothing purges
anywhere — `known-gaps.md`), and it is stated rather than hidden.

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

- `CREATE TABLE`, `DROP TABLE`, `CREATE INDEX`, `DROP INDEX` inside an
  explicit transaction: atomic, isolated, rolled back by `ROLLBACK`.
- The autocommit path is unchanged in behaviour: a bare `CREATE TABLE`
  commits immediately, exactly as today.
- Mixed statements: `BEGIN; CREATE TABLE t ...; INSERT INTO t ...;
  ROLLBACK;` leaves no relation and no rows.

Out of v1, by name: `ALTER TABLE` (its own rename semantics interact with
the cache differently), `CREATE PATTERN` / `CREATE CABIN` / assertions /
foreign keys (each writes its own catalog page and can follow once the
mechanism is proven on the two simplest), and anything about concurrent
DDL from two transactions — see §6.

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
