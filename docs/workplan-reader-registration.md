# Workplan — Reader Registration and the First Purge Consumer

Status: **complete 2026-08-19** (RR1-RR5). Owning specs: `docs/txn.md`
§4.1 (the registration mechanism), `docs/spec-ddl-transactional.md` §5d
(the catalog delete-mark purge, the first consumer). The specs own the
design now; this file is the history and the decision record.

One decision moved during the build, recorded where the next reader
trips on it: D2's "no mintable view produces a zero bound" was wrong on
a **peer core**, whose id sequence has never issued an id and whose
views therefore carry a zero high-water mark — semantically "sees no
real-id version at all". `RegisterReader` stores such a view at bound 1
(below every real id, conservative and exact) rather than refusing the
statement, which is what the first full-suite run caught.

## Why

`docs/txn.md` §4.1 states the current design: *"Readers are not registered
anywhere."* That is what makes every form of reclamation unbuildable — a
purge must prove no live read view still needs a version before retiring
it, and an unregistered reader cannot be proved absent. Two independent
gaps in `docs/known-gaps.md` now name this same prerequisite by name:

1. the MVCC **undo purge** (`docs/txn.md` §9, "Purge needs the reader
   registration §4.1 deliberately omits");
2. the **catalog delete-mark purge within a mount**
   (`docs/spec-ddl-transactional.md` §5c: DT10 finalizes marks *at mount
   only*, "because afterwards a mark may belong to a transaction that is
   still open" — and, it must be added, to a committed drop an old live
   view still cannot see; `marks` is unbounded between restarts).

This workplan builds the registration substrate and the **second** of
those as its first consumer. The undo purge is deliberately **not** built:
its retention policy and `SnapshotTooOld` surfacing are still `[OPEN]` in
`docs/txn.md` §9, and the rule (`docs/txn.md` §8, repeated by
`feat-index.md` §13) is to build no partial version of a guarantee. What
this workplan delivers is the prerequisite both consumers share, proved by
the smaller one.

## The readers, enumerated

Every path that mints a read view today, and whether it must register:

| Reader | Where minted | Lifetime | Registers? |
|---|---|---|---|
| Explicit transaction | `TransactionManager::Begin` / `StartStatement` | until Commit/Abort | **already tracked** — `live_` holds it; the horizon walks `live_` |
| Autocommit statement snapshot | `CommandDispatcher::SnapshotFor` → `txn::AutocommitSnapshot` | the statement, **including parks** (a session-side statement awaiting remote batches) | **yes** — via the lease `AutocommitSnapshot` now returns |
| Cross-core pipeline stage | `RemoteStepService` (serve / producer / consumer), `txn::AutocommitSnapshot(txns_)` | the stage, held across every credit/boundary park | **yes** — same lease, on the stage's own core's manager |
| FK / assertion-build check view | `MintReadView(kNoTrxId)` at check time | one synchronous span | **no** — latest-state semantics; `CheckVisibility` never walks the undo chain and never reads a superseded version |
| DDL-open name-resolution view | `CommandDispatcher::ViewFor` | one synchronous resolution | **no** — synchronous on the core's one thread; the purge runs only at a dispatch seam, so no such view can be live when it runs |
| Cabin optimizer check view | `CabinOptimizerExecutor::MintCheckView` | one synchronous tick step | **no** — same latest-state argument |

The rule the table encodes, and the one the spec amendment must state:
**a view that can be used to read a superseded version — walk an undo
chain, or filter a catalog read — and that can be held across a park,
must be registered.** Latest-state check views are exempt because no
purge can change a latest-state answer; synchronous views are exempt
because the core is single-threaded and every purge runs at a dispatch
seam, never inside a resolution. The second exemption is an invariant to
re-check whenever a new suspension point is added to the executor
(P4d-3's page-boundary awaits are the named watch item).

## Decisions

**D1 — registration is per-core, and so is the horizon.** Each
`TransactionManager` owns its registry; `ReadHorizon()` answers for its
core only. Sound because every reader reads its own core's data: CC4 says
no view crosses a core, CC3 refuses cross-core writes, and a shipped
stage mints from *its* core's manager. Catalog rows are written only by
core-0 transactions, and a peer's unfiltered catalog read already counts
a settled mark as gone — so core 0's horizon is the only one the catalog
purge needs. **Named revisit condition**: a cross-core writer, or any
reader holding a view over another core's versions, must extend this to
a cross-core horizon — it joins the same list as the cross-core commit
oracle (`docs/workplan-ddl-transactional.md`'s DT9 scope note).

**D2 — a lease, not a flag.** `RegisterReader(view)` returns a move-only
RAII `ReaderLease`; destruction unregisters. Slots are a fixed array
(`kMaxRegisteredReaders = 256`) with an O(1) free-list, value 0 = free —
no allocation on the statement front door, per `read_view.hpp`'s POD
rule. Exhaustion is `OutOfSpace`, the same surfaced-bound policy as
`kMaxTrackedLiveTxns`. The manager must outlive every lease, which the
ownership order already guarantees (core runtime owns the manager;
dispatcher and stages die first).

**D3 — registration is structural, not disciplinary.**
`txn::AutocommitSnapshot` changes its return type to a move-only
`LeasedSnapshot { Snapshot snap; ReaderLease lease; }`, so every holder
of an autocommit snapshot registers by construction and a future call
site cannot forget. A null manager returns an empty lease with the
everything-view, exactly as before.

**D4 — the horizon formula.** A view's bound is
`min(up_to_trx_id, in_flight[0])` (`in_flight` is sorted; empty means
`up_to` alone), further bounded by `own_trx_id` when the view is owned.
`ReadHorizon()` = the minimum of that bound over every **active**
transaction in `live_` (also bounded by the transaction's own id) and
every occupied lease slot; `UINT64_MAX` when there are none. The claim it
supports: **a version superseded by a committed transaction with
`id < ReadHorizon()` is invisible to every live view and every future
view** — every live view has `up_to > id` and its smallest in-flight
above `id`, so `Visible(id)` holds; a future view's `up_to` only grows,
and a committed id is in no future in-flight set. An id `>=` the horizon
proves nothing and its version must stay.

**D5 — the first consumer: `Catalog::PurgeSettledDeleteMarks()`.** The
same sweep as `FinalizeDeleteMarksAtMount` (shared helper), retiring only
marks whose deleter is `< txn_->ReadHorizon()`. `deleter < horizon`
already implies the deleter is not in flight (an active transaction
bounds the horizon at or below its own id), and rollback clears its own
marks synchronously, so every horizon-passed mark is a committed drop no
one can see. Trigger: `CommandDispatcher::EndDdlScope`, both endings,
right after `InvalidateAfterCompensation()` — DDL resolution is the only
event that creates or settles marks (autocommit DDL writes at
`kBootstrapXid` and retires directly, leaving no mark), it is rare, and
the core is between resolutions there so the synchronous-view exemption
holds. **No version bump**: every retired row was already invisible to
every reader — unfiltered reads settle the mark by the same comparison,
filtered readers this old are exactly what the horizon proves absent — so
no cached answer changes, and bumping would broadcast `kCatalogInvalidate`
and stale bound statements for nothing. A mark whose deleter has not yet
cleared the horizon simply survives to the next DDL resolution or the
next mount (DT10) — accumulation is now bounded by reader lifetimes, not
by the mount.

**D6 — observability.** The dispatcher accumulates
`catalog_marks_purged` since mount and `SHOW META` prints it beside
`catalog_marks_finalized`. No new statement, no new counter subsystem.

## Tasks

- **RR1** — this document. ☑
- **RR2** — `ReaderLease`, `RegisterReader`, `ReadHorizon` on
  `TransactionManager`; unit tests in `txn_manager_test.cpp` (lease RAII
  and move, slot exhaustion is `OutOfSpace` and release reopens it,
  horizon over live transactions / leases / both / neither, the
  in-flight-bounded case). ☑
- **RR3** — `LeasedSnapshot` through `AutocommitSnapshot`; thread the
  type through `CommandDispatcher::SnapshotFor` and the three
  `RemoteStepService` stage sites. Behaviour-neutral; the suite proves
  it. ☑ (with the zero-bound peer finding above)
- **RR4** — `Catalog::PurgeSettledDeleteMarks()` + the `EndDdlScope`
  trigger + the `SHOW META` line. Tests in `txn_session_test.cpp`: a
  committed transactional `DROP INDEX`'s marks purge at its own
  resolution; a registered older lease holds them and its release frees
  them; an open older transaction holds them; a rolled-back drop leaves
  nothing to purge; DT10's mount test amended to lease a reader, which
  is the state a mount realistically inherits. ☑
- **RR5** — doc amendments: `docs/txn.md` §4.1 (registration exists;
  §9's undo-retention entry loses its "needs registration" clause but
  stays open), `read_view.hpp`'s header note,
  `docs/spec-ddl-transactional.md` §5d, `docs/known-gaps.md` (the
  reclamation preamble and the `marks` bound), `CLAUDE.md` milestone
  rows. ☑

## Not in scope, by decision

- The **undo purge** and everything behind it (retention policy,
  `SnapshotTooOld`, tuple/var-heap/index/Cabin reclamation) — §9-open.
- Any background or periodic purge cadence — the only trigger is DDL
  resolution; a cadence is a `maintenance`-group decision that belongs
  with the undo purge.
- Cross-core horizon aggregation — D1's revisit condition, empty today.
