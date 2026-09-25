#include "kds/server/core_runtime.hpp"

#include "kds/base/current_core.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>


#include "kds/exec/assertion_catalog.hpp"
#include "kds/exec/catalog_spills.hpp"
#include "kds/exec/step_vm.hpp"
#include "kds/sched/epoll_io_backend.hpp"
#include "kds/server/mount_recovery.hpp"
#include "kds/storage/page_header.hpp"

namespace kds::server {

// The header states the rule; this is all it takes. Dropping the optional
// destroys the ready queues, and with them every unfinished `CoroTask` and
// the coroutine frame it owns - while `txn_manager_`, `catalog_` and
// `store_` are still standing, which is exactly what those frames' locals
// need.
//
// **The listener goes first, and it has to be said here rather than left to
// declaration order** (PW5): this body runs before any member destructor, so
// dropping the scheduler here would leave `~TcpServer`'s `Detach()` - which
// unregisters the listening fd and every client fd - calling into a reactor
// that no longer exists. Detaching first is also exactly what core 0 does
// (`Expeditor::Serve` detaches its listener before its scheduler leaves
// scope), so a peer's teardown is the same sequence as the system core's.
CoreRuntime::~CoreRuntime() {
    // **This runs as this core, wherever it is called from** (AM-S2 step 3).
    // The Expeditor calls this on a peer from *core 0's* thread on the way
    // down, and the work below writes pages: `StampPageLsn` records whose
    // stream the page_lsn beside it belongs to, so without this the peer's
    // pages would go out stamped core 0's - a mislabel `SHOW PAGE` would
    // print, and one the next mount refused (rule 5) until AW-S1b took the
    // stamp's ownership reading. It was the store's own `core_id_` that
    // hid this, and that member is gone because a shared store cannot have
    // one.
    const CurrentCoreGuard as_this_core(core_id());
    listener_.reset();
    // R6-2's rollback of the cross-owner transactions this core was a
    // participant in stood here until AT-S6, which retired the participant:
    // a transaction is one core's, whole, and ends with its session.
    // The dispatcher's view of the reactor is nulled by hand: the header's
    // teardown argument is that a member destructor may reach back, and a
    // stale reactor pointer is exactly what that argument forbids. Nothing
    // below dispatches today, so this is the contract rather than a live
    // fix.
    if (dispatcher_.has_value()) {
        dispatcher_->set_scheduler_view(nullptr);
    }
    // **`scheduler_.reset()` used to stand here and is deliberately gone.**
    // It inverted declaration order to enforce a contract that declaration
    // order already keeps: `scheduler_` is declared above every borrower,
    // so reverse-order destruction drops every one of them first. Dropping
    // it here instead did the opposite of what its own comment argued for.
    // The hand-nulled borrows
    // above stay: they are for members declared *below* the dispatcher,
    // which reverse order takes first, and that order no line here can fix.
}

StatusOr<std::unique_ptr<CoreRuntime>> CoreRuntime::Open(Config config,
                                                         storage::PageDevice& device,
                                                         const sched::Clock& clock, Logger* log) {
    // **Before anything else, because everything below may read it.** The
    // volume's image is required rather than optional: a `CoreRuntime` that
    // does not know what volume it is on answers a legal, silent, wrong
    // zero for every field it was not individually told about, which is the
    // shape three separate defects have now taken (`core_runtime.hpp`'s
    // `Config::superblock`).
    if (config.superblock == nullptr) {
        return Status::InvalidArgument(
            "CoreRuntime: core " + std::to_string(config.core_id) +
            " was given no superblock; a core cannot be opened without the volume's image");
    }
    // And the instance's transaction-id ceiling (AT-S10b), for the same
    // reason: a core that carved from a ceiling of its own would issue ids
    // another core issues too.
    if (config.trx_id_ceiling.superblock == nullptr) {
        return Status::InvalidArgument(
            "CoreRuntime: core " + std::to_string(config.core_id) +
            " was given no transaction-id ceiling; two cores carving from two would issue one "
            "id twice");
    }

    // **This whole pass acts as the core it is opening** (AM-S2 step 3,
    // `base/current_core.hpp`). `Open` runs on the *startup* thread, not on
    // the reactor thread this core will get, and it takes page latches -
    // recovery's redo does, with no second thread alive. Declaring the
    // identity here rather than letting it default to 0 is what makes the
    // page latch's owner field exact instead of harmless-by-accident: today
    // nothing contends with the mount pass, and step 3 is not the place to
    // start relying on that.
    const CurrentCoreGuard as_this_core(config.core_id);

    auto runtime = std::unique_ptr<CoreRuntime>(new CoreRuntime(config, log));
    // The whole decoded image, not a field of it. `anchor` stays on the
    // config: it is a selection over the image - the fold's slot 0 - not a
    // gap in it. `anchors` stood beside it and went at AM-S4(d) with the
    // cross-stream resolver that was its only reader.
    runtime->superblock_ = *config.superblock;

    // Each core gets its own epoll instance. Sharing one would be shared
    // mutable state between cores (workplan guideline 1) and would also
    // defeat the point: a reactor blocks in *its* backend, and one backend
    // means one core can wake for another's event.
    auto backend = sched::EpollIoBackend::Create();
    if (!backend.ok()) return backend.status();
    runtime->io_backend_ = std::make_unique<sched::EpollIoBackend>(std::move(backend.value()));

    runtime->scheduler_.emplace(clock, *runtime->io_backend_, config.scheduler);
    runtime->scheduler_->SetLogger(log);
    // **The reactor learns its core here**, with no registry: `RunOnce`
    // sets the thread's `CurrentCore()` from it every iteration, so a
    // reactor left at the default runs as core 0 - re-entering core 0's
    // exclusive page latches and stamping core 0's stream. `AttachTransport`
    // set it until AT-S10d; the instance's `AttachWakerTable` sets it again
    // with the same id, and a runtime that never gets one keeps this.
    if (Status s = runtime->scheduler_->AttachWakerTable(nullptr, config.core_id); !s.ok()) {
        return s;
    }

    // **The instance's one stream** (AR0 M0, AL-S1c; AM-S4(d)). This core
    // opens no log device at all: it appends through core 0's stream under
    // its latch and asks the writer for every sync, which is what takes
    // `fdatasync` off this reactor (AL-2's case for the whole milestone).
    //
    // The arm that stood beside this one opened
    // `wal-<core_id>-<segment_no>.log` for a peer that owned its own
    // stream. No mountable volume has one, so this core has no `wal_dir`
    // to open and holds no `log_device_`.
    if (config.shared_stream == nullptr || config.shared_writer == nullptr) {
        // A peer with no stream to attach to would silently open one of
        // its own and write records nobody replays.
        return Status::InvalidArgument(
            "core " + std::to_string(config.core_id) +
            ": this database has one WAL stream, but no stream and writer were handed to "
            "this core to attach to");
    }
    auto wal =
        wal::WalManager::Attach(config.shared_stream, config.shared_writer, clock, config.core_id);
    if (!wal.ok()) return wal.status();
    runtime->wal_ = std::move(wal.value());
    runtime->wal_->SetLogger(log);

    // **The instance's one pool, or this core's own** (AM-S2 step 3). A
    // shared store arrives already opened, logged, gated, budgeted and
    // armed by `Expeditor` - core 0 did all of it before any peer existed -
    // so a peer that borrows one applies none of those again. Applying them
    // would not be redundant, it would be wrong: `SetWalGate` would swap the
    // instance's gate for this core's manager, and `SetFrameBudget` would
    // hand one pool a per-core share of itself.
    //
    // The unshared arm is a fixture's: its own store over the device,
    // allocating from the free map above the system range.
    if (config.shared_store != nullptr) {
        runtime->store_ = config.shared_store;
    } else {
        auto store = storage::DevicePageStore::Open(device, kFirstUserPageId);
        if (!store.ok()) return store.status();
        runtime->owned_store_ = std::move(store.value());
        runtime->store_ = runtime->owned_store_.get();
        runtime->store_->SetLogger(log);
        runtime->store_->SetWalGate(runtime->wal_.get());
    }
    // Only when the config carries a share. Zero must not be written:
    // Open() may already hold the debug KDS_TEST_FRAME_BUDGET override,
    // and writing zero here would silently undo it on every peer store -
    // the same rule the core-0 site follows (expeditor.cpp).
    if (runtime->owned_store_ != nullptr && config.buffer_pool_frames != 0) {
        runtime->store_->SetFrameBudget(config.buffer_pool_frames);
    }

    // **This core's recovery** (RV1/RV2, server/mount_recovery.hpp) - which
    // on a peer is nothing: one stream, recovered whole by core 0's mount
    // before this core is built (below).
    //
    // It ran **before the lease was installed**, deliberately, until AW-S1b
    // removed the lease: RC04's repair raises the store's allocation floor,
    // and a leased store refused that raise - correctly, since such a core
    // took its ids from its extent and never consulted the floor - so
    // installing the lease first made every peer with a non-empty stream
    // refuse its own mount.
    //
    // The undo log is built here for a reason of its own: recovery's undo phase
    // writes through it, and the rest of the transaction stack must not exist
    // yet, because `TrxIdSequence` caches the transaction ceiling at
    // construction (txn/trx_id.hpp).
    //
    // **The stamp identity needs no ordering here any more** (AM-S2 step 3).
    // Recovery's undo phase writes compensations through `StampPageLsn`,
    // which records whose stream the page_lsn belongs to; this used to come
    // from the store's own `core_id_`, set by the same call that installed
    // the lease - so a `SetStreamCoreId` call had to be hoisted above the
    // recovery or a peer stamped its own pages as core 0's, which rule 5
    // refused at the next mount until AW-S1b. The identity is the thread's
    // now, and the
    // `CurrentCoreGuard` at the top of this function covers the whole pass.
    // The page latch (AM-S1): armed from the instance's core count, which
    // bootstrap recorded in the superblock and `Expeditor::Open` copied here -
    // after the identity above, so the owner field the word records is this
    // core's. `core_count > 1` alone, which is what the WAL's own arming
    // predicate came down to at AM-S4(d) as well. At one core the word is
    // never touched (device_page_store.hpp, "The page latch").
    // **The pinner count, not just the arm** (AM-S2). `kPinCeiling` bounds
    // *one operation*'''s pin stack, and `live_pins_` is a proxy for it only
    // while one thread reaches this store. That holds today and stops
    // holding at step 3, where N threads each running a 7-deep descent
    // against one store would trip a debug abort on correct traffic. Passing
    // the count here scales the bound with the operations that can be in
    // flight, and only ever loosens a debug assert.
    // Core 0 armed a shared store when it opened it, with the same two
    // numbers; re-arming would be harmless and re-stating it here would
    // still be a second place for the decision to live.
    if (runtime->owned_store_ != nullptr) {
        runtime->store_->SetLatchArmed(config.core_count > 1, config.core_count);
    }
    runtime->undo_log_.emplace(*runtime->store_, &*runtime->wal_);
    // Timed, as core 0's is (`Expeditor::Open` passes its clock): `SHOW META`
    // prints the whole RC09 block only when `timings.timed` says a clock was
    // there, so an untimed recovery here would silently drop the phase
    // numbers *and* the completion checkpoint's, which the end of `Open` times.
    //
    // **Under one stream a peer recovers nothing** (AR0 M0, AL-R5). There
    // is one log, core 0 recovered it whole before this core was built, and
    // a second pass over the same records would not merely be wasted work:
    // this core would redo pages core 0 already redid and undo losers
    // core 0 has already rolled back. The recovery report stays
    // zeroed, which is the truth for a core that recovered nothing, and
    // `SHOW META`'s block reads that way on a peer.
    // **Unconditional since AM-S4(d)**: the per-core arm that stood beside
    // this one called `RecoverCoreAtMount` over this core's own stream, and
    // no mountable volume gives it one.
    //
    // **Timed, though nothing was recovered.** `SHOW META` prints the
    // whole `_us` block only where a clock was supplied
    // (`command_dispatcher.cpp`), and this core still measures the
    // completion checkpoint at the end of `Open` - so leaving this false
    // would hide a number that was taken, and print a peer's recovery
    // block as a subset of core 0's rather than the same block reading
    // zero.
    runtime->recovery_.timings.timed = true;
    // Says why the numbers below it are zero: this core did not scan,
    // rather than scanning and finding nothing.
    runtime->recovery_.ran = false;
    if (log != nullptr && log->enabled(LogLevel::kInfo)) {
        log->Info("recovery", "core " + std::to_string(config.core_id) +
                                  ": one stream, so core 0's mount pass covered this core's "
                                  "records; nothing recovered here");
    }

    // **No completion checkpoint here** (RC08): it runs at the end of `Open`,
    // once the dispatcher whose assertion registry it may snapshot exists,
    // and only on a runtime handed the instance's anchor (AT-S8). Core 0's
    // own runs in Expeditor::Open.
    // **The system range, and nothing else** (AW-S1b). This installed an
    // extent lease beside it while per-core stores existed; the lease is
    // gone, and what survives is the residency floor - the boundary was
    // also core 0's write boundary until AT-S5 retired that arm. A store
    // the Expeditor built already carries it - `SetResidentLimit` is where
    // that store gets it - so this is the arm for a `CoreRuntime` opened
    // directly on a device, which is a test fixture rather than a server.
    if (runtime->owned_store_ != nullptr) {
        runtime->store_->SetResidentLimit(kFirstUserPageId);
    }

    // This core's catalog: every core writes the catalog's pages since
    // AT-S5 and asks the instance's schema word at its task boundaries
    // (AT-S2), so the memo here is this core's cache of shared rows.
    runtime->catalog_.emplace(*runtime->store_, config.inline_cell_width);
    runtime->catalog_->SetLogger(log);
    runtime->catalog_->SetSchemaWord(config.schema_word);  // AT-S2
    runtime->catalog_->SetOidSequence(config.oid_sequence);  // AT-S5b
    runtime->catalog_->SetMarkCounter(config.mark_counter);  // AT-S5b
    // Every core's catalog writes are logged (RV3), on every core since
    // AT-S5.
    runtime->catalog_->SetWal(runtime->wal_.get());

    // The transaction stack. **Every core carves its own transaction-id
    // window from the instance's ceiling since AT-S10b** - the superblock,
    // latch and persist it was handed (`Config::trx_id_ceiling`) - and
    // issues row ids through the catalog's direct path, as core 0 always
    // has. A peer used to draw both from blocks core 0 carved and leased to
    // it over the ring, because page 0 and the catalog pages were core 0's.
    runtime->trx_ids_.emplace(*config.trx_id_ceiling.superblock, config.trx_id_ceiling.persist);
    runtime->trx_ids_->SetLatch(config.trx_id_ceiling.latch);
    // The undo log is already built - recovery wrote its compensations
    // through it above, before this stack existed.
    // The lock table: the instance's when handed one (AO-S5), else this
    // runtime's own at one core - see the member's declaration for the
    // boundary. Settled before the manager so the manager can take it: a
    // decide is where borrows and wait-for edges go back (AO-R6), and a
    // manager without it would leave the dispatcher's own clear as the
    // only cleaner.
    if (config.locks != nullptr) {
        runtime->locks_ = config.locks;
    } else if (config.core_count == 1) {
        auto locks = txn::LockTable::Create(/*core_count=*/1);
        if (!locks.ok()) return locks.status();
        runtime->owned_locks_ = std::move(locks.value());
        runtime->locks_ = runtime->owned_locks_.get();
    }
    runtime->txn_manager_.emplace(*runtime->trx_ids_, *runtime->undo_log_, *runtime->store_,
                                  &*runtime->wal_, config.visibility, config.core_id,
                                  runtime->locks_);
    // The schema word a catalog-writing decide moves before its borrows go
    // (AT-S5e): the same word this core's catalog asks at its boundaries.
    runtime->txn_manager_->SetSchemaWord(config.schema_word);

    // **The instance's Cabin store where one was handed** (AT-S7); this
    // core's own only where nobody did, which is a fixture.
    runtime->cabins_ = config.cabins_store;
    if (config.cabins && runtime->cabins_ == nullptr) {
        runtime->cabin_store_.emplace(config.cabin_limits);
    }
    // **Waystone records here too since AT-S7.** A peer had no recorder
    // at all - `sys.patterns` is a catalog page, read-only on a peer until
    // AT-S5 - and every core writes catalog pages since then. Built
    // before the dispatcher, which borrows it.
    if (config.waystone_recording) {
        runtime->trail_recorder_.emplace(*runtime->catalog_, *runtime->store_, &clock);
    }
    runtime->dispatcher_.emplace(
        runtime->superblock_, *runtime->catalog_, *runtime->store_, log, &clock,
        &*runtime->wal_, config.durability, config.budget,
        runtime->trail_recorder_ ? &*runtime->trail_recorder_ : nullptr, config.waystone_replay,
        // **The instance's switch, on every core** (AT-S7). This was a
        // hard `false` for every core a runtime opens, because
        // `sys.access_stats` sat in the reserved range and a peer could
        // not write it; CR7 gave a peer a batch instead, and the batch is
        // gone with the ring kind that flushed it.
        config.access_statistics,
        runtime->cabins(), &*runtime->txn_manager_,
        config.isolation, config.core_id);
    // Core 0's statement limits, the ones `Expeditor::Open` sets on its own
    // dispatcher (AT-S8; `Config::statement_limits` says what went wrong
    // without them).
    runtime->dispatcher_->set_statement_limits(config.statement_limits);
    runtime->dispatcher_->set_optimizer_surface(config.optimizer);
    // This core's mount, for its `SHOW META` recovery block (RC09's field
    // list, docs/spec/client-manual.md) - `Expeditor::Open`'s wiring, per core
    // since PW3b. `recovery_` is declared above the dispatcher and outlives it.
    runtime->dispatcher_->set_recovery(&runtime->recovery_);
    // The lock-wait fault net, copied from core 0's config (R6-5), so a
    // writer waiting on this core is bounded by the instance's one value.
    runtime->dispatcher_->set_lock_wait_fault_net_ns(config.lock_wait_fault_net_ns);
    // The wait-for graph and the admission it gates, on every core since
    // AO-S4b - the member's declaration says why that is safe now.
    runtime->dispatcher_->set_locks(runtime->locks_);
    // And the instance's assertion registry (AT-S5d), on the same terms: one
    // directory per assertion is what makes a write on this core checked
    // against the rows every other core admitted. Null leaves the
    // dispatcher its own, a fixture's shape.
    runtime->dispatcher_->set_assertions(config.assertions);
    // `SHOW META`'s group-accounting block on this core (sched.md §4). Set
    // on every core, peer or not: the accounting question is about a
    // reactor, and every core runs one. Set on the startup thread, before
    // this runtime's worker exists, so the reactor-local read rule holds.
    // The view is dropped in `~CoreRuntime`, which destroys the scheduler
    // *ahead* of the dispatcher - declaration order alone would not do it.
    runtime->dispatcher_->set_scheduler_view(&*runtime->scheduler_);
    // The catalog's one-writer rule was made enforceable at dispatch by PW4
    // and is history: the argument lived at `PeerDdlRefused`, which AT-S5
    // deleted with the route (`crosscore.md` CC11).
    // `SetCatalogReadOnly(true)` stood here until AT-S5 and CR7's batch
    // arming until AT-S7, both for a peer: its catalog pages had one writer
    // and `sys.access_stats` sat where it could not write.

    // **Assertion enforcement at mount** (RC07). Here rather than beside
    // `RecoverCoreAtMount` above for `Expeditor::Open`'s reason - the
    // registry lives on the dispatcher, which did not exist yet - and still
    // before the listener binds, so no statement is accepted against an
    // unenforcing constraint.
    //
    // **The self-grant this used to need went with CC7's fault rights**
    // (AW-S1b). `sys.assertions` keeps each declaration's text in a
    // var-heap page taken from the general supply rather than the reserved
    // range (`crosscore.md` CC12/CR1), which the system-range arm of the
    // old fault predicate did not cover - so a peer granted itself read
    // rights over exactly the pages its rows named
    // (`exec::CatalogSpillPages`, deleted with the grant) or `ListAssertions`
    // was refused the fetch and no assertion of its own could be revived.
    // One frame table serves every core, so a peer reading such a page
    // finds the frame core 0 wrote and needs no right to it.

    // **The assertion resume is the registry's, not the core's** (AT-S5d).
    // Handed the instance's registry, this core resumes nothing: core 0's
    // mount filled it before any peer was built, for every relation, and a
    // second resume would adopt every directory twice. It resumed the
    // assertions on the relations it owned into its own registry until
    // then, which is why a peer's `recovery_assertions_*` counted.
    //
    // A runtime with a registry of its own - a fixture - resumes into it
    // exactly as core 0 does, from `redo_start_lsn` rather than
    // `checkpoint_lsn`: the scan must begin at or before the first
    // `ASSERT_SNAPSHOT` any core wrote after its own `CHECKPOINT_BEGIN`,
    // and the anchor's fold bounds only `redo_start_lsn` below every one of
    // them. Widening the scan is safe - the pass folds `ASSERT_*` records
    // after whatever base it finds, and `DedupeEntryLinkage` exists for
    // that overlap.
    if (config.assertions == nullptr) {
        runtime->recovery_ = ResumeAssertionsAfterRecovery(
            *runtime->catalog_, *runtime->store_,
            // **The stream's device.** This core opened none, so there is
            // no `log_device_` to ask; the stream answers core 0's, which
            // it is attached to (`core_runtime.hpp`).
            *runtime->wal_->stream()->device(),
            // **The stream's core, which is never this core**: the scanner
            // validates every segment header against the id it is given.
            // Literal 0 since AM-S4(d): there is one stream and it is core
            // 0's.
            /*stream_core=*/0, config.anchor.redo_start_lsn, runtime->dispatcher_->assertions(),
            runtime->recovery_, log);
    }

    // **This core's checkpointer** (PW3), built wherever the runtime is handed
    // the instance's anchor (AT-S8), which it publishes into directly
    // (`Expeditor`'s `SuperBlockCheckpointAnchor`).
    if (config.checkpoint_anchor != nullptr) {
        runtime->checkpoint_target_.emplace(*runtime->store_);
        runtime->checkpointer_.emplace(*runtime->wal_, *runtime->checkpoint_target_,
                                       *runtime->txn_manager_, *config.checkpoint_anchor);
        runtime->checkpointer_->SetLogger(log);
        // AS6a's snapshot source - **only for a registry this runtime owns**
        // (AT-S5d). The instance's registry is snapshotted by core 0's
        // checkpoints alone: two cores snapshotting it would put two runs of
        // the same assertions into the one stream, and nothing orders them -
        // each `CHECKPOINT_BEGIN` is appended outside the registry's latch,
        // so the runs can land back to back, and recovery reads the second
        // run's first record while the first run's base is still open (a
        // duplicate group id, a failed pass, and every asserted relation
        // refusing writes for the mount, `assertion_recover.cpp`). A
        // runtime with its own registry - a fixture - snapshots it.
        const wal::AssertionSnapshotSource* snapshot_source =
            config.assertions == nullptr ? &runtime->dispatcher_->assertions() : nullptr;
        runtime->checkpointer_->SetAssertionSource(snapshot_source);

        // **The completion checkpoint** (RC08): an anchor past everything
        // the mount replayed, so the next crash scans from here. On the
        // startup thread, before any worker exists, so it needs no gate.
        // Through the helper core 0 uses rather than `Checkpoint()`: it
        // carries the empty active table (the fact here), the Info line
        // that is this core's evidence its mount bounded the next crash,
        // and the context that names which checkpoint aborted the mount.
        if (Status s = CheckpointAfterRecovery(
                config.core_id, *runtime->wal_, *runtime->checkpoint_target_,
                *config.checkpoint_anchor, log, &runtime->scheduler_->clock(),
                &runtime->recovery_.checkpoint_ns, snapshot_source);
            !s.ok()) {
            return s;
        }
    }

    if (log != nullptr && log->enabled(LogLevel::kDebug)) {
        log->Debug("core", "core " + std::to_string(config.core_id) +
                               " ready: wal stream, page store, catalog, dispatcher");
    }
    return runtime;
}

void CoreRuntime::Run() {
    // As this core, wherever called from - see `~CoreRuntime` (AM-S2 step 3).
    const CurrentCoreGuard as_this_core(core_id());

    // The two `system`-group cadences Expeditor already ran on the single
    // core, now per core. Both are no-ops on a core with nothing logged -
    // which today is every core but 0 (see the header) - so arming them
    // costs a timer and buys the property that a core which *starts*
    // logging needs no new wiring.
    auto drain = [this]() -> bool {
        // The bool is for the post-task hook below, and through it for the
        // idle policy: **a tick that had a commit staged did work that a
        // parked statement is waiting on**, and a reactor told otherwise
        // would sleep between the staging and the wake-up, putting the
        // drain interval on every commit (Scheduler::SetPostTaskHook).
        // Read before the drain, which is what clears it.
        const bool had_staged_commits = wal_->HasPendingGroupCommits();
        if (Status s = wal_->DrainOnce(); !s.ok() && log_ != nullptr &&
                                          log_->enabled(LogLevel::kError)) {
            log_->Error("wal", "core " + std::to_string(config_.core_id) +
                                   ": drain failed: " + s.message());
        }
        return had_staged_commits;
    };

    // **After every iteration's tasks**, which is what makes group commit a
    // group: a statement stages its commit and parks, every other runnable
    // statement does the same, and this syncs once for all of them. The
    // timer below still exists for the D3 loss-window bound, which is about
    // a core with nothing running rather than about a waiting commit.
    scheduler_->SetPostTaskHook(drain);

    if (config_.wal_drain_interval_ns > 0) {
        scheduler_->SubmitEvery(config_.wal_drain_interval_ns, drain);
    }

    // **The idle burn rides the drain tick** (AN-R13), on every core but 0,
    // whose tick is `Expeditor`'s. It rode with the two id-lease refills
    // until AT-S10b retired them; a burn is a carve here like any other
    // since, so it needs nothing from core 0. (R6-2's lifetime-ceiling sweep rode
    // here too until AT-S6, and what its removal leaves is a gap:
    // `docs/inflight/known-gaps.md`.)
    if (config_.core_id != 0 && config_.wal_drain_interval_ns > 0) {
        scheduler_->SubmitEvery(config_.wal_drain_interval_ns,
                                [this] { (void)txn_manager_->MaybeBurnIdleBlock(); });
    }

    // The `system`-group checkpoint cadence of wal.md §11, per core since
    // PW3 - core 0 has run one since RC08 and a peer ran none, so a peer
    // that wrote left an anchor that never advanced and a stream every
    // later mount replayed whole. A no-op where `checkpointer_` is unset.
    //
    // **Staggered by core** (AT-S8): the run is the instance's
    // (`wal::CheckpointGate`), and every core arms this within milliseconds
    // of the others with one period, so unstaggered ticks meet each period -
    // and whenever a run outlasts the start-up skew, the same core wins each
    // time and the rest skip for good, holding the fold, and so the anchor,
    // at their completion checkpoints. Core `k` of `n` starts `k/n` of a
    // period late; core 0's cadence, `Expeditor`'s, starts at phase 0.
    if (checkpointer_.has_value() && config_.checkpoint_interval_ns > 0) {
        const sched::MonoTimeNs period = config_.checkpoint_interval_ns;
        const sched::MonoTimeNs phase =
            period / std::max<std::uint32_t>(config_.core_count, 1) * config_.core_id;
        scheduler_->SubmitAt(scheduler_->clock().Now() + phase, [this, period] {
            scheduler_->SubmitEvery(period, [this] { (void)Checkpoint(); });
        });
    }

    // Per core, on the thread that will run the statements - the audit's
    // counters are core-local (exec/step_vm.cpp), so installing it once on
    // the startup thread would leave every worker unguarded. The core's
    // own store rides along for the pin half of the rule (P4d-3).
    // **The pin half of the audit is this core's only where the store is**
    // (AM-S2 step 3). `step_vm.cpp`'s `g_audit_store` tests `live_pins() != 0`
    // at a park, on the premise its own comment states - "each core has its
    // own store". On a borrowed pool that count is the instance's sum, so a
    // peer parking while core 0 holds a pin would record a park-with-pin
    // that never happened. The audit is a debug recorder, so a false entry
    // costs a diagnostic rather than a run; a false entry is still worse
    // than a missing one, because it makes the record untrustworthy where a
    // gap is merely narrower. The coroutine half is unaffected and stays.
    exec::InstallSuspendAudit(owned_store_ != nullptr ? store_ : nullptr);

    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("core", "core " + std::to_string(config_.core_id) + " reactor running");
    }
    scheduler_->Run();
    // The audit's store pointer must not outlive the store: this thread
    // may outlive the runtime, and a coroutine polled on it afterwards
    // would hand the (debug-only) audit a freed store.
    exec::UninstallSuspendAudit();
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("core", "core " + std::to_string(config_.core_id) + " reactor stopped");
    }
}

Status CoreRuntime::Checkpoint() {
    // As this core, wherever called from - see `~CoreRuntime` (AM-S2 step 3).
    const CurrentCoreGuard as_this_core(core_id());

    // Nothing to do on a core that has no checkpointer: core 0, whose one
    // lives on `Expeditor`, and any runtime handed no anchor.
    if (!checkpointer_.has_value()) return Status::OK();
    return checkpointer_->RunGated(config_.checkpoint_gate, config_.core_id);
}

Status CoreRuntime::ShutdownCheckpoint() {
    // As this core, wherever called from - see `~CoreRuntime` (AM-S2 step 3).
    const CurrentCoreGuard as_this_core(core_id());

    // Pages first, and unconditionally: the flush is this core's own work
    // and wants no checkpointer, while only the publish below does. The
    // header says why the order matters.
    if (Status s = store_->Sync(); !s.ok()) return s;
    return Checkpoint();
}

Status CoreRuntime::ListenAndAttach(std::uint16_t port, const TcpServer::ClientSetup& setup) {
    // As this core, wherever called from - see `~CoreRuntime` (AM-S2 step 3).
    const CurrentCoreGuard as_this_core(core_id());
    auto listener = TcpServer::Listen(port, /*reuse_port=*/true);
    if (!listener.ok()) return listener.status();
    if (Status s = AttachListener(std::move(listener.value()), setup); !s.ok()) return s;
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("core", "core " + std::to_string(config_.core_id) +
                               " listening on port " + std::to_string(port) +
                               " (SO_REUSEPORT)");
    }
    return Status::OK();
}

Status CoreRuntime::HostHandedConnections(ConnectionHandoff& handoff,
                                          const TcpServer::ClientSetup& setup) {
    // As this core, wherever called from - see `~CoreRuntime` (AM-S2 step 3).
    const CurrentCoreGuard as_this_core(core_id());
    if (Status s = AttachListener(TcpServer::Hosting(), setup); !s.ok()) return s;
    if (Status s = listener_->Host(handoff, config_.core_id); !s.ok()) {
        listener_.reset();
        return s;
    }
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("core", "core " + std::to_string(config_.core_id) +
                               " runs the connections core 0 hands it (no SO_REUSEPORT)");
    }
    return Status::OK();
}

Status CoreRuntime::AttachListener(TcpServer server, const TcpServer::ClientSetup& setup) {
    listener_.emplace(std::move(server));
    listener_->Configure(setup);
    if (Status s = listener_->Attach(*scheduler_, *dispatcher_, log_); !s.ok()) {
        listener_.reset();
        return s;
    }
    // STOP accepted on this core must stop the *instance*, not this
    // reactor (the review's BUG 2: a stopped peer's socket keeps
    // receiving a kernel share of new connections nobody drains while
    // core 0 reports healthy). So it routes,
    // through the instance's hook, straight to core 0's stop flag - atomic
    // since AU-S3 - plus a kick to end the block it is sitting in; Serve's
    // ordinary tail then broadcasts shutdown to every peer, one stop path
    // whichever core the client landed on. It used to route a `kShutdown`
    // message for one reason only: the flag was a plain bool, so core 0's
    // own thread had to be the one to flip it. The hook is installed by the
    // instance, so a fixture that wires a listener with no instance around
    // it simply has none and `TcpServer` falls back to stopping the reactor
    // this listener is attached to (`tcp_server.cpp`'s `scheduler_->Stop()`).
    if (instance_stop_) {
        listener_->set_stop_handler([this] { instance_stop_(); });
    }
    return Status::OK();
}

Status CoreRuntime::Sync() {
    // As this core, wherever called from - see `~CoreRuntime` (AM-S2 step 3).
    const CurrentCoreGuard as_this_core(core_id());
    return wal_->SyncAll();
}

}  // namespace kds::server
