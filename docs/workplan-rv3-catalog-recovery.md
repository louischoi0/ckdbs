# Workplan — RV3: Catalog Recovery (DDL becomes durable)

Status: **ratified 2026-08-19** — D1 undo records, D2 implicit
transaction, D3 post-redo invalidate, each the recommended option; the
declined alternatives stand below as the record of what was refused and
why. Build in progress. Owners once landed: `docs/wal.md` (the write side — its §"Catalog"
line has reserved the design since the WAL existed: *"catalog-page
mutations (DDL, Waystone flag changes) as ordinary page records"*),
`docs/txn.md` §7, `docs/spec-ddl-transactional.md` (durability joins
atomicity/isolation), `docs/workplan-wal-recovery.md` (RV3 closes).

## What the survey established, on `rv3-catalog-recovery` at `bd640bc`

1. **The vocabulary already exists.** Catalog rows are ordinary
   `heap::PageView` tuples on catalog pages (`catalog.cpp`'s `InsertRow`);
   every mutation shape maps to an existing record type — `kHeapInsert`,
   `kHeapOverwrite` (the DT5 in-place retype), `kHeapDeleteMark`,
   `kSlotRetire`, `kPageInit` (`AllocateCatalogPage`), `kVarHeapAppend`
   (pattern/assertion source text). **No new record type, no segment
   format bump.** Redo applies all of them physically already.
2. **The mutation sites funnel.** One `InsertRow`, the DT5 change
   writers, four retire/mark sites, `AllocateCatalogPage`, and the
   var-heap appends — roughly ten sites, all inside `catalog.cpp`, all
   reachable by threading a `WalManager*` into the catalog (`SetWal`,
   beside `SetTransactionManager`).
3. **The checkpoint needs nothing.** Catalog writes go through
   `store.Get()`, which already dirties the frame; adding
   `StampPageLsn` puts them under the existing WAL-before-data gate and
   into the recLSN accounting for free.
4. **Recovery-undo compensates physically.** `RecoveryUndo::Compensate`
   works from the undo record alone — target page/slot, prior header,
   image — with no keystone identity dependence. Catalog rows never
   renumber slots (`RetireSlot` sets a flag in place), so the existing
   machinery can roll back a crash loser's catalog writes **if those
   writes append undo records** — which `AppendUndo` supports without
   stamping the tuple, so `undo_ptr` stays 0 on catalog rows and no
   read path changes.
5. **Mount ordering is lighter than feared.** Bootstrap on an existing
   database reads only the superblock; catalog pages are read lazily
   through the cache, and nothing reads them between catalog
   construction (`expeditor.cpp:614`) and `RecoverCoreAtMount` (`:691`).
   One defensive `cache_.Invalidate()` after redo, plus the stated rule,
   suffices — no reordering.
6. **A bonus in scope**: `sys.tables.next_id` block carves
   (`AllocateRowIdRange`) are catalog overwrites. Logging them closes
   the unlogged-ceiling half of Keystone K1's crash exposure
   (`docs/keystoneid-k0-findings.md`, K-M2a's named prerequisite) at the
   price of one logged overwrite per 4096 ids.

## D1 — The crash-loser mechanism `[OPEN — ratify]`

A crash can interrupt a DDL transaction after some catalog rows are
durable via redo. Who removes them?

**(a) Undo records, via the existing machinery. RECOMMENDED.**
Transactional DDL writes call `AppendUndo` beside their trail entries
(`kInsert` for row inserts, `kOverwrite` with the prior image for the
`sys.objects` retype, `kDeleteMark` for marks). Recovery-undo then rolls
a loser's catalog writes back with **zero new recovery code** — the same
chain walk, the same compensations, the same TXN_ABORT terminator. Live
rollback stays on the trail, unchanged. This is the only option that can
undo the retype overwrite, which has no other before-image after a crash.

**(b) A mount sweep of non-winner rows.** DT10's style: retire any
catalog row whose writer analysis does not name a winner. No undo
records — but it cannot undo an **overwrite** (the retype's prior bytes
exist nowhere), so a crashed `DROP TABLE` would leave `sys.objects`
retyped with its dependent rows restored: a corrupt half-state. Refused
unless (a) proves unworkable.

## D2 — Autocommit DDL's transaction identity `[OPEN — ratify]`

Analysis classifies winners by TXN_COMMIT. Autocommit DDL today writes
at `kBootstrapXid` with no transaction at all — logged but unowned, a
mid-statement crash leaves it half-applied with nothing to roll it back.

**(a) Autocommit DDL joins the implicit transaction. RECOMMENDED.** The
same shape autocommit DML already has (`BeginWrite`): a real trx id, the
trail, undo records, commit at statement end. Uniform loser story; a
half-crashed CREATE TABLE rolls back like any loser. Cost: DDL statements
gain the begin/commit records and the id; `AutocommitDdlIsUnchanged`-class
tests move to asserting the new, stronger contract.

**(b) Keep `kBootstrapXid` autocommit DDL.** Cheaper, but a mid-statement
crash can now durably persist *half* a DDL — strictly worse than today's
"all of it may vanish". Refused unless (a) hits something structural.

## D3 — Mount ordering `[OPEN — ratify]`

**(a) Post-redo cache invalidation plus the stated rule. RECOMMENDED.**
Redo mutates catalog pages under a constructed-but-unread catalog;
`InvalidateFromPeer()`-style cache drop right after recovery, and a rule
in `expeditor.cpp` that nothing may read catalog rows between
construction and recovery. Smallest change; survey fact 5 says it is
sufficient today.

**(b) Reorder: recovery before bootstrap.** Structurally cleaner,
heavier: `RecoverCoreAtMount` currently takes the catalog for the RV3
audit, and fresh-create would need a WAL-less arm. Keep as the fallback
if (a)'s rule proves unenforceable.

Fresh-database bootstrap stays unlogged either way: it runs once, before
any acknowledgement exists to break, and ends in the existing flush.

## What flips when this lands

`SHOW META`: `ddl_durable=1`, `catalog_recovered=1` (the
`relations_missing_pages` census stays as the audit). `wal.md`'s "still
outside the log" paragraph closes. `spec-ddl-transactional.md` §7's
deferral closes. Per-relation grants lose their named gate
(`docs/protocol.md`). K-M2a's prerequisite closes if the D1/D2
recommendations carry the next_id carve.

## Task sketch (firm after ratification)

- **RV3-1** — ratify D1-D3; this file becomes the workplan.
- **RV3-2** — the write side: `Catalog::SetWal`, log helpers, the ~ten
  sites emit records + `StampPageLsn`; `AllocateCatalogPage` logs
  `PAGE_INIT`; the next_id carve logs its overwrite.
- **RV3-3** — the loser side: transactional DDL writes append undo
  records (D1a); autocommit DDL joins the implicit transaction (D2a).
- **RV3-4** — mount: post-redo invalidation (D3a); `ddl_durable=1`;
  RV3 audit update.
- **RV3-5** — tests: crash sim with a DDL-bearing workload (the sim
  workload writes no DDL today — extend it); unit tests per shape
  (create/drop table/index, committed and crash-lost, across a mount);
  the DT-series suites stay green.
- **RV3-6** — critics-developer review; ck-tester (DDL statement cost
  A/B — the WAL appends are new bytes on a rare path; INSERT/UPDATE
  paths must not move); docs.

## Not in scope

Per-relation grants themselves (unblocked, not built); catalog *reads*
under MVCC (unchanged); `DROP TABLE` isolation (§5a's separate gap);
page reclamation of any kind.
