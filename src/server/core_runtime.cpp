#include "kds/server/core_runtime.hpp"

#include "kds/base/current_core.hpp"

#include "kds/catalog/core_placement.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "kds/sched/send_retry.hpp"

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
    // pages would go out stamped core 0's - the lie `page_header.hpp`'s rule
    // 5 refuses at the next mount. It was the store's own `core_id_` that
    // hid this, and that member is gone because a shared store cannot have
    // one.
    const CurrentCoreGuard as_this_core(core_id());
    listener_.reset();
    // R6-2: any cross-owner transaction this core was a participant in ends
    // here, rolled back. The worker has joined, so nothing will decide one
    // now, and a transaction left `active_` outlives the executor holding
    // its session - which is the shape `docs/spec/cross-owner-txn.md` (retired)
    // refuses an autocommit statement for. Recovery would unwind these as
    // losers at the next mount either way; doing it here is what keeps the
    // in-process invariant ("no transaction outlives its executor") true
    // rather than merely repaired later. Before the dispatcher's borrows are
    // withdrawn below, because the rollback goes through it.
    // Before the reactor goes, because this body inverts declaration order
    // (see the header): `scheduler_` is dropped here, ahead of the
    // dispatcher that holds a view on it. Nothing below dispatches today,
    // so this is the contract rather than a live fix - but the header's
    // whole teardown argument is that a member destructor may reach back,
    // and a stale reactor pointer is exactly what that argument forbids.
    if (dispatcher_.has_value()) {
        dispatcher_->set_scheduler_view(nullptr);
    }
    // RR2's fan-in client is declared *above* `dispatcher_`, so reverse
    // destruction already takes the borrower first and this withdrawal is
    // a no-op today. Kept, and kept out of the block above whose members
    // are the other way round: it is what stops the borrow from dangling
    // if that declaration order is ever changed, which is the one way this
    // pairing has been got wrong before.
    if (dispatcher_.has_value()) {
        dispatcher_->SetRemoteReads(nullptr);
    }
    // **`scheduler_.reset()` used to stand here and is deliberately gone.**
    // It inverted declaration order to enforce a contract that declaration
    // order already keeps: `scheduler_` is declared above every borrower,
    // so reverse-order destruction drops all seven of them first. Dropping
    // it here instead did the opposite of what its own comment argued for,
    // and `MakeStepSend` made that reachable rather than theoretical -
    // `remote_steps_` now holds a `sched::Scheduler&` and would have
    // outlived it by the width of this function. The hand-nulled borrows
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
    // `first_new_page_id` on the unshared arm is irrelevant - allocation
    // comes from the lease, never from the free map - but it is passed for
    // the range check the store still does.
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

    // **This core's recovery** (RV1/RV2, server/mount_recovery.hpp): each
    // stream is independent, so a peer recovers its own rather than waiting
    // on core 0 - and no order between the two is introduced, which is what
    // workplan-crosscore.md guideline 3 forbids.
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
    // recovery or a peer stamped its own pages as core 0's, the lie rule 5
    // refuses at the next mount. The identity is the thread's now, and the
    // `CurrentCoreGuard` at the top of this function covers the whole pass.
    // The page latch (AM-S1): armed from the instance's core count, which
    // the superblock pinned at bootstrap and `Expeditor::Open` copied here -
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
    // numbers *and* the completion checkpoint's, which AttachTransport times.
    // The wal dir goes in too (R6-4): a transaction this core prepared and
    // never heard the outcome of is resolved by reading its coordinator's
    // stream, which is another file in that same directory.
    //
    // **Under one stream a peer recovers nothing** (AR0 M0, AL-R5). There
    // is one log, core 0 recovered it whole before this core was built, and
    // a second pass over the same records would not merely be wasted work:
    // this core would redo another core's pages through its own store,
    // outside the extent grants that say which pages are its to write, and
    // undo losers core 0 has already rolled back. The recovery report stays
    // zeroed, which is the truth for a core that recovered nothing, and
    // `SHOW META`'s block reads that way on a peer.
    // **Unconditional since AM-S4(d)**: the per-core arm that stood beside
    // this one called `RecoverCoreAtMount` over this core's own stream, and
    // no mountable volume gives it one.
    //
    // **Timed, though nothing was recovered.** `SHOW META` prints the
    // whole `_us` block only where a clock was supplied
    // (`command_dispatcher.cpp`), and this core still measures the
    // completion checkpoint at `AttachTransport` - so leaving this false
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

    // A peer may not raise the durable transaction ceiling - the superblock is
    // page 0 and belongs to the system core (M5), and `superblock_` here is a
    // copy. Refused rather than applied to the copy, which would be a raise
    // nothing persists and a ceiling core 0 never learns of.
    //
    // **The ceiling it compares against is core 0's, and PW1 is when that
    // started mattering.** This check used to compare a recovered stream
    // against the 0 of a default-constructed copy, which was harmless only
    // while a peer's stream named no transaction of its own - and the id
    // leases ended that: the first peer to write and then remount would
    // recover ids above 0 and refuse its own mount, on a database that did
    // nothing wrong. PW1 fixed it by copying that one field across; the
    // AL-S9 review replaced the copying with the whole image above, because
    // field-by-field had by then been wrong three more times.

    // Reads the report this core's own recovery produced, which is zeroed
    // under one stream because core 0's pass covered these records and
    // raised the ceiling there. So in that topology the check is core 0's
    // to make and this one is vacuously true rather than skipped.
    if (runtime->recovery_.next_trx_id > runtime->superblock_.next_trx_id()) {
        return Status::Unsupported(
            "core " + std::to_string(config.core_id) + ": its log names transaction id " +
            std::to_string(runtime->recovery_.next_trx_id - 1) +
            ", above the ceiling the superblock carries; the system core owns page 0 and grants "
            "this core its id blocks (docs/inflight/in-progress/workplan-peer-writer.md PW1)");
    }

    // **No completion checkpoint here** (RC08): it runs at the end of `Open`,
    // once the dispatcher whose assertion registry it may snapshot exists,
    // and only on a runtime handed the instance's anchor (AT-S8). Core 0's
    // own runs in Expeditor::Open.
    // **The system range, and nothing else** (AW-S1b). This installed an
    // extent lease beside it while per-core stores existed; the lease is
    // gone, and what survives is the boundary below which only core 0 may
    // write (AM-R2, AO-R14). A store the Expeditor built already carries
    // it - `SetResidentLimit` is where that store gets it - so this is the
    // arm for a `CoreRuntime` opened directly on a device, which is a test
    // fixture rather than a server.
    if (runtime->owned_store_ != nullptr) {
        runtime->store_->SetResidentLimit(kFirstUserPageId);
    }

    // The catalog, read-only in practice: DDL is core 0's, and the store
    // above refuses a write to the pages it lives on. What this instance
    // does is *read* and cache, and drop the cache when core 0 says so.
    runtime->catalog_.emplace(*runtime->store_, config.inline_cell_width, config.core_count,
                              config.core_id);
    runtime->catalog_->SetLogger(log);
    runtime->catalog_->SetSchemaWord(config.schema_word);  // AT-S2
    runtime->catalog_->SetOidSequence(config.oid_sequence);  // AT-S5b
    runtime->catalog_->SetMarkCounter(config.mark_counter);  // AT-S5b
    // RV3: a peer may not write a catalog page (P6), so this should never
    // fire - but if a write ever slips through, logged beats silent.
    runtime->catalog_->SetWal(runtime->wal_.get());
    const bool is_peer = config.core_id != catalog::kSystemCore;

    // A peer may not write the catalog, so its row ids come from leased
    // blocks (P5's shape, catalog/row_id_lease.hpp): AllocateRowId() draws
    // from this table, and a spent block is retryable exhaustion until the
    // kRowIdLease refill lands. Core 0 keeps the direct path - it owns the
    // page the sequence lives on.
    if (is_peer) {
        runtime->catalog_->SetRowIdLeases(&runtime->row_id_leases_);
    }

    // The transaction stack. `superblock_` is a copy (see the header): the
    // sequence would write through it, which is why the persist callback
    // below refuses rather than pretending.
    runtime->trx_ids_.emplace(runtime->superblock_, [runtime = runtime.get(), is_peer] {
        // A peer may not write the superblock - it is page 0 and belongs to
        // the system core (M5). Since PW1 a peer does not come here at all:
        // its sequence draws windows from the lease installed below, and
        // this callback is the backstop that says a lease source went
        // missing rather than a gap that has not been filled.
        if (is_peer) {
            return Status::NotImplemented(
                "core " + std::to_string(runtime->core_id()) +
                " cannot raise the transaction-id ceiling; the superblock belongs to the "
                "system core, and this core's transaction-id lease source is not installed");
        }
        // **Core 0 is the system core, wherever it was built** (AV-S1).
        // A core-0 `CoreRuntime` exists only in a rig, and until the rig
        // needed one it refused every carve through the arm above - so a
        // core 0 built this way could not open a single writing transaction,
        // and every two-core fixture in the tree built core 0 by hand
        // instead. The raised ceiling reaches page 0 before the block is
        // handed out, which is the ordering `Carve` says is a correctness
        // statement rather than a preference.
        //
        // **Read-modify-write, not `Expeditor::PersistSuperBlock`'s blanket
        // encode.** The Expeditor writes the one image every writer of page
        // 0 goes through; `superblock_` here is a *copy* taken at `Open`,
        // and encoding it whole would erase any anchor a checkpoint had
        // written to the page since - silently, the symptom being a later
        // mount replaying from the head of the log. Nothing writes page 0
        // beside this on a core-0 runtime today (its `AttachTransport`
        // builds no anchor and a rig drops the peer's), and the shape is
        // what keeps that from being load-bearing. The store's sync alone:
        // page 0 is unlogged, so there is no record to make durable first.
        auto page = runtime->store_->Get(kSuperBlockPageId);
        if (!page.ok()) return page.status();
        auto on_disk = SuperBlock::Decode(page.value().bytes());
        if (!on_disk.ok()) return on_disk.status();
        if (Status s = on_disk.value().SetNextTrxId(runtime->superblock_.next_trx_id());
            !s.ok()) {
            return s;
        }
        on_disk.value().Encode(page.value().bytes());
        return runtime->store_->Sync();
    });
    // Transaction ids come from a leased block on a peer, exactly as row
    // ids do above (`docs/inflight/in-progress/workplan-peer-writer.md` PW1). Installed here
    // rather than at AttachTransport because a peer without a transport
    // must fail its first write with the lease's retryable exhaustion, not
    // with the superblock refusal above - the refusal names a wiring bug,
    // and a transport-less core is a test fixture rather than one.
    if (is_peer) {
        runtime->trx_ids_->SetLeaseSource(&runtime->trx_id_lease_);
    }
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

    // Recording off, deliberately and not as a default - see the header:
    // Waystone is advisory, so a peer returns identical rows without it and
    // loses speed and the optimizer's input. The Cabin store is this core's
    // own since AK-S2 (the header's rule 3): the owner observes, appends and
    // serves, which is the whole of a Cabin's life at one range per relation.
    // **The instance's store where one was handed** (AT-S7); its own only
    // where nobody did, which is a fixture.
    runtime->cabins_ = config.cabins_store;
    if (config.cabins && runtime->cabins_ == nullptr) {
        runtime->cabin_store_.emplace(config.cabin_limits);
    }
    // **Waystone records here too since AT-S7.** A peer had no recorder
    // at all - `sys.patterns` is a catalog page and rule 3 of the header's
    // asymmetry list said why - and every core writes catalog pages since
    // AT-S5. Built before the dispatcher, which borrows it.
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
    // This core's mount, for its `SHOW META` recovery block (RC09's field
    // list, docs/spec/client-manual.md) - `Expeditor::Open`'s wiring, per core
    // since PW3b. `recovery_` is declared above the dispatcher and outlives it.
    runtime->dispatcher_->set_recovery(&runtime->recovery_);
    // D5's in-doubt ceiling, copied from core 0's config (R6-5). Set here
    // and not at `AttachTransport`, because a writer blocked on an in-doubt
    // row is blocked whether or not this core has a transport - the
    // transaction that holds the row is this core's own.
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
    // RD5's size, which is also the arming bit (`server/range_alloc.hpp`:
    // one key, because a range **is** its grant). On every core and not
    // only a peer: core 0 owns most relations but not all of them under a
    // rotating placement, so it too can hold a foreign INSERT that R4/IS1
    // wants to leave a demand behind.
    runtime->dispatcher_->set_range_size_ids(config.range_size_ids);
    // Asymmetry 1 was made enforceable at dispatch by PW4 and is history:
    // the argument lived at `PeerDdlRefused`, which AT-S5 deleted with the
    // route (`crosscore.md` CC11).
    if (is_peer) {
        // `SetCatalogReadOnly(true)` stood here until AT-S5: a peer's
        // dispatcher refused DDL and named keys, took no sorted fill, and
        // shipped or refused every write to a relation it did not own -
        // because the catalog pages had one writer. They have none
        // (`DevicePageStore::MayWrite` names what serialises them).
        // **CR7's batch was armed here and is gone** (AT-S7): a peer
        // recorded nothing until CR7, then folded into a batch and flushed
        // it to core 0 on the tick, because `sys.access_stats` sits in the
        // reserved range and a peer could not write it. It writes the
        // relation itself now, under its root page's latch, so the switch
        // above is the whole of the arming and this block has nothing left
        // to do for statistics.
        // The lease refills' cost, for `SHOW META` on this core
        // (lease_refill_stats.hpp): the trace PW6's four-writer cell asked
        // for.
        runtime->dispatcher_->set_lease_refill_stats(&runtime->trx_id_refill_.stats,
                                                     &runtime->row_id_refill_.stats);
    }

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
    // the instance's anchor (AT-S8). It waited for `AttachTransport` while its
    // anchor was a send over the ring; it publishes into `Expeditor`'s
    // `SuperBlockCheckpointAnchor` now, so it needs nothing the ring gives.
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

Status CoreRuntime::AttachTransport(sched::RingTransport& transport) {
    // As this core, wherever called from - see `~CoreRuntime` (AM-S2 step 3).
    const CurrentCoreGuard as_this_core(core_id());

    if (Status s = scheduler_->AttachTransport(&transport, config_.core_id); !s.ok()) {
        return s;
    }

    // **No shutdown handler since AU-S3.** Stopping this reactor used to be
    // a message, and the reason was narrow and specific: `Scheduler::Stop()`
    // wrote a plain bool, so only this reactor's own thread could safely
    // flip it. The flag is atomic now, so core 0 stops a peer directly and
    // kicks it awake (`Expeditor::BroadcastShutdown`, AR0-6-R1) - the kind
    // is gone from `ring_message.hpp` with it.

    // **CC7's two grant handlers went with the grants** (AW-S1b): core 0
    // used to hand a peer fault rights over a relation's page range and
    // write rights over its exact creation pages, because a peer's own
    // frame table could reach neither without being told. One frame table
    // serves every core now, so both questions the grants answered have no
    // asker. Their kinds were struck at AT-S2b; a stale peer's message on a
    // struck number finds no handler in the scheduler's map and is dropped.

    // The row-id lease's receive side (P5's shape), peers only: core 0
    // owns the sequence pages and never leases from itself - and in
    // production its scheduler carries the *grant handler* on this kind
    // instead (row_id_lease_service.hpp).
    if (config_.core_id != 0) {
        if (Status s = RegisterRowIdGrantReceiver(*scheduler_, row_id_refill_, row_id_leases_,
                                                  log_);
            !s.ok()) {
            return s;
        }
        // And the transaction-id lease's receive side (PW1), on the same
        // terms and for the same reason: core 0 carries the grant handler
        // on this kind instead (trx_id_lease_service.hpp).
        if (Status s = RegisterTrxIdGrantReceiver(*scheduler_, trx_id_refill_, trx_id_lease_,
                                                  log_);
            !s.ok()) {
            return s;
        }
    }

    // The remote step server (workplan P4b, streaming since P4d-4a): this
    // core executes STEP_OPENs against relations it owns and streams the
    // batches back under credit, the producer parking at page boundaries
    // when credit runs dry. The sender submits through the retry task - a
    // full ring yields and retries, never drops (M7) - and the producer
    // itself is a coroutine task on this same reactor.
    //
    // **`transport`, the parameter - never `transport_`.** That member is
    // not assigned until the end of this function, and reading it here
    // was first a null dereference and then, once guarded, a silent
    // default that left the batch ceiling absent, so nothing clamped an
    // oversize batch. `MakeStepSend` takes the parameter and holds it,
    // so the whole class of ordering error is gone rather than commented
    // around.
    //
    // **One seam, two owners** (R4-R/RR2): the client takes a copy of the
    // sender and the server takes the seam, which is `expeditor.cpp`'s
    // pairing on core 0 and is what keeps the sender and the ceiling the
    // server seals against from coming from different transports.
    StepSendSeam step_seam = MakeStepSend(*scheduler_, transport, config_.core_id);
    remote_reads_.emplace(config_.core_id, step_seam.send, log_);
    remote_steps_.emplace(
        *catalog_, *store_, config_.core_id, std::move(step_seam), log_, kStepBatchTargetBytes,
        [this](std::unique_ptr<sched::Task> task) { scheduler_->Submit(std::move(task)); },
        &*txn_manager_,
        // And this core's configured row-touch ceiling, which the server
        // ignored until P4d-4c's review - a shipped statement was bounded
        // only by whatever a fresh `exec::Budget()` defaulted to.
        config_.budget,
        // And this core's Cabin store (AK-S2): a stage on a relation this
        // core owns serves from the same sets the dispatcher does.
        cabins(),
        // And the instance's lock table, so a producer declares the relation
        // it streams (AT-S1).
        locks_);
    // All six kinds, in `remote_step_service.hpp`'s one home - including
    // the kStepBatch/kStepEof fan-out to both endpoints, which is the rule
    // that must not be written twice.
    if (Status s = WireStepEndpoints(*scheduler_, *remote_reads_, *remote_steps_); !s.ok()) {
        return s;
    }
    // **After the receivers, never before**: the dispatcher learning about
    // the client is what lets a statement open a stage, and a reply must
    // not be able to beat its receiver into existence.
    dispatcher_->SetRemoteReads(&*remote_reads_);

    // **Statement shipping and 2PC are not wired, because they no longer
    // exist** (AT-S6). What stood here built this core's shipped-statement
    // executor and its participant seams, registered the ship request
    // handler and the reply receiver, and then both halves of the
    // cross-owner commit - every core a participant and every core a
    // coordinator. A read runs where the session is now, as a write has
    // since AT-S5, so no statement crosses and no transaction has a half
    // to prepare.

    transport_ = &transport;
    return Status::OK();
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

    // **The page-id lease's low-water check went with the lease** (AW-S1b).
    // The other two refills below keep the cadence it set: both are cheap
    // `system` work, and a second timer for a check that is one integer
    // comparison would cost more than it measures.
    if (transport_ != nullptr && config_.wal_drain_interval_ns > 0) {
        // **R6-2's lifetime-ceiling sweep stood here and went with the
        // protocol** (AT-S6): it expired a cross-owner participant context
        // nobody decided. Its registration was left behind with an empty
        // body - a task submitted on every drain tick doing nothing - until
        // this line removed it. What the removal leaves is recorded as a
        // gap: no transaction has a lifetime ceiling any more
        // (`docs/inflight/known-gaps.md`).
        //
        // The transaction-id lease rides the drain tick (PW1). A peer that
        // has never held a window reads as low, so the first tick asks and
        // a peer is ready to write before a client arrives - which is the
        // point, since `TrxIdSequence::Next()` cannot await a grant.
        // The row-id lease (PW1b) and the relation-grant re-delivery
        // (PW1c-7) ride the same tick and for the same reason the extent
        // check is here: cheap `system` work, and a timer each would cost
        // more than it measures - so one timer, one lambda (the PW1c-7
        // review's S4; they were one registration apiece).
        if (config_.core_id != 0) {
            scheduler_->SubmitEvery(config_.wal_drain_interval_ns, [this] {
                // A task boundary (AT-S2): the refill below reads the
                // catalog, and stale here is a range that does not open.
                catalog_->Revalidate();
                // **Before the refill, and that order is the mechanism**
                // (AN-R13): the burn check is what sets `burn_requested_`,
                // and the refill below is what acts on it. A peer that
                // should burn therefore asks on this tick and installs on
                // the next.
                MaybeBurnIdleTrxIdBlock();
                MaybeRefillTrxIds();
                MaybeRefillRowIds();
            });
        }
    }

    // The `system`-group checkpoint cadence of wal.md §11, per core since
    // PW3 - core 0 has run one since RC08 and a peer ran none, so a peer
    // that wrote left an anchor that never advanced and a stream every
    // later mount replayed whole. A no-op where `checkpointer_` is unset.
    if (checkpointer_.has_value() && config_.checkpoint_interval_ns > 0) {
        scheduler_->SubmitEvery(config_.checkpoint_interval_ns, [this] { (void)Checkpoint(); });
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

void CoreRuntime::MaybeRefillRowIds() {
    // The row-id lease's asking half (PW1b). It could not ride the trx-id
    // lease's shape directly: that sequence is per *instance*, so a peer can
    // pre-empt for it from the first tick, while a row-id lease is per
    // *relation* and has no subject until a statement names one. So demand
    // is recorded where it is discovered - `RowIdLeaseTable::Next` inserts
    // the spent entry on a miss - and this tick answers it.
    //
    // The consequence, stated because a client sees it: the **first** INSERT
    // into a relation on a given peer fails retryably, which is exactly what
    // that lease's `TxnConflict` message already promises, and no later one
    // does.
    if (row_id_refill_in_flight_) return;
    const auto neediest = row_id_leases_.NeediestRelation();
    if (!neediest.has_value()) return;

    // RD5, and **this is the one core that may ask it** (workplan §9c):
    // the fifth gate's fact lives in this core's own assertion registry,
    // so core 0 asking on our behalf would answer "eligible" for exactly
    // the relation an assertion should decline. Core 0 re-checks what its
    // catalog can see and may still decline; the ids arrive either way.
    //
    // **Off by default since the 2026-08-31 operator amendment**
    // (`range_size_ids`, range_alloc.hpp), which reverses DA1's arming:
    // spreading is a per-relation option the user decides, so nothing
    // spreads until something asks. RD6 made a range its own chain, which
    // is what had forced the key off before that - a directory row would
    // have described a partition no insert or read honoured. A
    // `CoreRuntime` built by hand takes `kRangeSizeOff` too
    // (core_runtime.hpp).
    const bool ranges_on = config_.range_size_ids != kRangeSizeOff;
    bool open_range = false;
    if (ranges_on && dispatcher_.has_value()) {
        auto access = catalog_->InitTableAccess(*neediest);
        if (access.ok()) {
            const exec::RangeGate gate =
                exec::RangeEligible(*access.value(), dispatcher_->assertions());
            open_range = gate == exec::RangeGate::kNone;
            // C3 (§9e): the counter carries the per-ask volume, and the
            // line rides the *transition* - a permanently gated relation
            // is every indexed one, and a line per refill would pay
            // log.hpp's synchronous write once per lease block forever.
            if (!open_range && dispatcher_->range_split_declines().Record(*neediest, gate)) {
                LogRangeDecline(log_, config_.core_id, *neediest, gate,
                                "the owner core's own registry, which is the authority");
            }
        }
    }
    // The range **is** the lease grant, so one number sizes both
    // (`server/range_alloc.hpp`).
    const std::uint64_t count = ranges_on ? config_.range_size_ids : kRowIdLeasePerGrant;

    row_id_refill_in_flight_ = true;
    row_id_refill_.stats.NoteSubmit(scheduler_->clock().Now(), scheduler_->iterations());
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kSystem,
        RequestRowIdLease(*transport_, row_id_refill_, *neediest, count, config_.core_id,
                          /*system_core=*/0, log_, &*scheduler_, open_range),
        [this](const Status& s) {
            row_id_refill_in_flight_ = false;
            row_id_refill_.stats.Complete(scheduler_->clock().Now(), scheduler_->iterations());
            // CC10 step 4's other half. **The write admission it ran went
            // with the grants** (AW-S1b): core 0 formatted the range's head
            // page and this core had to acquire write rights over it before
            // it could write its own range, which a shared frame table
            // makes unnecessary. What is left is the reason the admission
            // was here rather than on the ordinary grant path - the
            // directory this core must see before its next statement
            // routes.
            if (row_id_refill_.entry_page != kInvalidPageId) {
                // The boundary core 0 just published. Asking here rather
                // than at the next statement's head means the very next
                // statement resolves against the directory rather than
                // the tick after - which is what R4/IS3's routing needs,
                // since until this core sees its own range it keeps
                // shipping the INSERT away - which since AT-S5 it does
                // not; the revalidation stays for the directory's sake.
                //
                // This completion is a task boundary and the next thing it
                // reads is the range core 0 just opened, whose `sys.ranges`
                // row bumped the schema word - so ask now (AT-S2). A drop
                // stood here, and before it a no-op: IS3's end-to-end test
                // found the peer never saw the range it had just been
                // granted.
                catalog_->Revalidate();
                row_id_refill_.entry_page = kInvalidPageId;
            }
            if (!s.ok() && log_ != nullptr && log_->enabled(LogLevel::kError)) {
                // Nothing to return it to - a background task - and the
                // consequence is bounded: INSERTs into that relation keep
                // failing retryably until a later tick succeeds. A relation
                // whose id space is genuinely exhausted answers zero-count
                // forever, and the retry is then the honest report.
                log_->Error("rowid", "core " + std::to_string(config_.core_id) +
                                         ": row-id refill failed: " + s.message());
            }
        }));
}

void CoreRuntime::MaybeBurnIdleTrxIdBlock() {
    if (!txn_manager_.has_value()) return;
    // **Re-derived every tick, never latched.** `MaybeRefillTrxIds` below
    // clears the flag only on the tick it acts on it, and it returns early
    // while a refill is already in flight - so latching would leave the flag
    // set across the grant that answered it, and the next tick would ask for
    // a second block nothing needs. On core 0 that second ask is a carve, a
    // superblock write and a `Sync()`. The answer is a fact about this tick,
    // so it is stored as one.
    burn_requested_ =
        txn_manager_->MaybeBurnIdleBlock() == txn::TransactionManager::BurnOutcome::kNeedsBlock;
}

void CoreRuntime::MaybeRefillTrxIds() {
    // Asked for *before* the window is spent, the extent lease's rule and
    // for its reason: `TrxIdSequence::Next()` is called from inside a
    // statement and cannot await, so by the time it reports exhaustion the
    // statement is already lost.
    //
    // **Or asked for while the window is full**, which is the one case that
    // inverts the rule above (AN-R13, AN-S1b): an *idle* core's window is
    // not low and never will be, and its unspent range is what holds the
    // instance's commit-order floor down. `burn_requested_` is set by the
    // burn check on the same tick, so the grant is asked for on one tick
    // and installed on the next - never burning into an empty hand, which
    // would leave this core's next write failing retryably for no gain.
    if (trx_id_refill_in_flight_ || (!trx_ids_->low_water() && !burn_requested_)) return;
    burn_requested_ = false;

    trx_id_refill_in_flight_ = true;
    trx_id_refill_.stats.NoteSubmit(scheduler_->clock().Now(), scheduler_->iterations());
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kSystem,
        RequestTrxIdLease(*transport_, trx_id_refill_, config_.core_id, /*system_core=*/0, log_,
                          &*scheduler_),
        [this](const Status& s) {
            trx_id_refill_in_flight_ = false;
            trx_id_refill_.stats.Complete(scheduler_->clock().Now(), scheduler_->iterations());
            if (!s.ok() && log_ != nullptr && log_->enabled(LogLevel::kError)) {
                // Nothing to return it to - this is a background task - and
                // the consequence is bounded: writes on this core fail
                // retryably until a later tick succeeds. Reads are
                // untouched either way; a read view issues no id.
                log_->Error("trxid", "core " + std::to_string(config_.core_id) +
                                         ": transaction-id refill failed: " + s.message());
            }
        }));
}

Status CoreRuntime::Checkpoint() {
    // As this core, wherever called from - see `~CoreRuntime` (AM-S2 step 3).
    const CurrentCoreGuard as_this_core(core_id());

    // Nothing to do on a core that has no checkpointer: core 0, whose one
    // lives on `Expeditor`, and any runtime handed no anchor.
    if (!checkpointer_.has_value()) return Status::OK();

    // At most one of the instance's checkpointers runs (AT-S8).
    const wal::CheckpointGate::Hold run(config_.checkpoint_gate);
    if (!run.entered()) {
        if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
            log_->Debug("checkpoint", "core " + std::to_string(config_.core_id) +
                                          ": skipped: another core's checkpoint is running");
        }
        return Status::OK();
    }

    // Cumulative counters, so this checkpoint's contribution is the delta -
    // `Expeditor::Checkpoint`'s reason: logging the running total would read
    // as "this checkpoint flushed 5 pages" on every tick after the first one
    // that did.
    const std::uint64_t flushed_before = checkpointer_->stats().pages_flushed;

    if (Status s = checkpointer_->RunToCompletion(); !s.ok()) {
        // The one place this becomes visible. It runs on a timer with no
        // caller to return to, so without the log it is a silently widening
        // loss window. Not fatal and it does not disarm the cadence: the
        // pages it did not flush stay dirty and the next tick retries them.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("checkpoint", "core " + std::to_string(config_.core_id) +
                                          ": checkpoint failed: " + s.message());
        }
        return s;
    }

    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("checkpoint",
                    "core " + std::to_string(config_.core_id) +
                        ": checkpoint complete: redo_start=" +
                        std::to_string(checkpointer_->redo_start_lsn()) + " pages_flushed=" +
                        std::to_string(checkpointer_->stats().pages_flushed - flushed_before));
    }
    return Status::OK();
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

Status CoreRuntime::ListenAndAttach(std::uint16_t port, Protocol protocol,
                                    wal::DurabilityClass durability,
                                    TcpServer::IdentitySource identity) {
    // As this core, wherever called from - see `~CoreRuntime` (AM-S2 step 3).
    const CurrentCoreGuard as_this_core(core_id());
    auto listener = TcpServer::Listen(port, /*reuse_port=*/true);
    if (!listener.ok()) return listener.status();
    listener_.emplace(std::move(listener.value()));
    listener_->set_protocol(protocol);
    listener_->set_durability(durability);
    if (identity) listener_->set_identity_source(std::move(identity));
    if (Status s = listener_->Attach(*scheduler_, *dispatcher_, log_); !s.ok()) {
        listener_.reset();
        return s;
    }
    // STOP accepted on this core must stop the *instance*, not this
    // reactor (the review's BUG 2: a stopped peer's socket keeps
    // receiving a kernel share of new connections nobody drains, and its
    // ring goes undrained while core 0 reports healthy). So it routes,
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
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("core", "core " + std::to_string(config_.core_id) +
                               " listening on port " + std::to_string(port) +
                               " (SO_REUSEPORT)");
    }
    return Status::OK();
}

Status CoreRuntime::Sync() {
    // As this core, wherever called from - see `~CoreRuntime` (AM-S2 step 3).
    const CurrentCoreGuard as_this_core(core_id());
    return wal_->SyncAll();
}

}  // namespace kds::server
