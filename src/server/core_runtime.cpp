#include "kds/server/core_runtime.hpp"

#include <string>
#include <utility>

#include "kds/exec/step_vm.hpp"
#include "kds/sched/epoll_io_backend.hpp"

namespace kds::server {

StatusOr<std::unique_ptr<CoreRuntime>> CoreRuntime::Open(Config config,
                                                         storage::PageDevice& device,
                                                         const sched::Clock& clock, Logger* log) {
    auto runtime = std::unique_ptr<CoreRuntime>(new CoreRuntime(config, log));

    // Each core gets its own epoll instance. Sharing one would be shared
    // mutable state between cores (workplan guideline 1) and would also
    // defeat the point: a reactor blocks in *its* backend, and one backend
    // means one core can wake for another's event.
    auto backend = sched::EpollIoBackend::Create();
    if (!backend.ok()) return backend.status();
    runtime->io_backend_ = std::make_unique<sched::EpollIoBackend>(std::move(backend.value()));

    runtime->scheduler_.emplace(clock, *runtime->io_backend_);
    runtime->scheduler_->SetLogger(log);

    // `wal-<core_id>-<segment_no>.log` in a shared directory - the naming
    // predates multicore (file_log_device.hpp) and is why N streams need no
    // per-core directory.
    auto log_device = wal::FileLogDevice::Open(config.wal_dir, config.core_id);
    if (!log_device.ok()) return log_device.status();
    runtime->log_device_ = std::move(log_device.value());

    auto wal = wal::WalManager::Open(runtime->log_device_.get(), clock, config.core_id);
    if (!wal.ok()) return wal.status();
    runtime->wal_ = std::move(wal.value());
    runtime->wal_->SetLogger(log);

    // This core's own page store over the shared device. `first_new_page_id`
    // is irrelevant here - allocation comes from the lease, never from the
    // free map - but it is passed for the range check the store still does.
    auto store = storage::DevicePageStore::Open(device, kFirstUserPageId);
    if (!store.ok()) return store.status();
    runtime->store_ = std::move(store.value());
    runtime->store_->SetLogger(log);
    runtime->store_->SetCoreOwnership(config.core_id, &runtime->lease_, kFirstUserPageId);
    runtime->store_->SetWalGate(runtime->wal_.get());

    // The catalog, read-only in practice: DDL is core 0's, and the store
    // above refuses a write to the pages it lives on. What this instance
    // does is *read* and cache, and drop the cache when core 0 says so.
    runtime->catalog_.emplace(*runtime->store_, config.inline_cell_width, config.core_count);
    runtime->catalog_->SetLogger(log);

    // The transaction stack. `superblock_` is a copy (see the header): the
    // sequence would write through it, which is why the persist callback
    // below refuses rather than pretending.
    runtime->trx_ids_.emplace(runtime->superblock_, [core_id = config.core_id] {
        // A peer may not write the superblock - it is page 0 and belongs to
        // the system core (M5). Reaching here means a peer tried to reserve
        // a transaction-id block, which needs the lease service P5 owns.
        // Refusing names the gap; silently succeeding would hand out ids
        // whose ceiling never became durable.
        return Status::Unsupported("core " + std::to_string(core_id) +
                                    " cannot raise the transaction-id ceiling; the superblock "
                                    "belongs to the system core and per-core id leases are "
                                    "workplan P5");
    });
    runtime->undo_log_.emplace(*runtime->store_, &*runtime->wal_);
    runtime->txn_manager_.emplace(*runtime->trx_ids_, *runtime->undo_log_, *runtime->store_,
                                  &*runtime->wal_);

    // Recording off, deliberately and not as a default - see the header.
    // Both features are advisory, so a peer returns identical rows without
    // them; what it loses is speed and the optimizer's input.
    runtime->dispatcher_.emplace(
        runtime->superblock_, *runtime->catalog_, *runtime->store_, log, &clock,
        &*runtime->wal_, config.durability, config.budget,
        /*recorder=*/nullptr, /*replay_enabled=*/false, /*access_statistics=*/false,
        /*cabins=*/nullptr, &*runtime->txn_manager_, config.isolation, config.core_id);

    if (log != nullptr && log->enabled(LogLevel::kDebug)) {
        log->Debug("core", "core " + std::to_string(config.core_id) +
                               " ready: wal stream, page store, catalog, dispatcher");
    }
    return runtime;
}

Status CoreRuntime::AttachTransport(sched::RingTransport& transport) {
    scheduler_->AttachTransport(&transport, config_.core_id);

    // Stops *this* reactor, from this reactor's own thread - which is the
    // whole reason shutdown is a message (ring_message.hpp's kShutdown).
    if (Status s = scheduler_->RegisterMessageHandler(
            sched::RingMessageKind::kShutdown,
            [this](const sched::MessageHeader& header, std::span<const std::byte>) {
                if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
                    log_->Debug("core", "core " + std::to_string(config_.core_id) +
                                            " stopping on request from core " +
                                            std::to_string(header.src_core));
                }
                scheduler_->Stop();
            });
        !s.ok()) {
        return s;
    }

    // Core 0 did a DDL. Drop every cached fact so the next statement
    // re-reads the catalog pages off the device, which core 0 flushed
    // before sending this (workplan P6).
    //
    // The window between the DDL and this arriving is real and is *not* an
    // error to close: a statement in it resolves against the old catalog and
    // answers "table not found", which crosscore.md §5 already specifies as
    // retryable. Nothing here waits for anything.
    if (Status s = scheduler_->RegisterMessageHandler(
            sched::RingMessageKind::kCatalogInvalidate,
            [this](const sched::MessageHeader&, std::span<const std::byte>) {
                InvalidateCatalog();
            });
        !s.ok()) {
        return s;
    }

    // The grant side of the page-id lease (workplan P5). Registered here
    // rather than in Run() because a grant can arrive before this core has
    // armed anything.
    transport_ = &transport;
    return RegisterExtentGrantReceiver(*scheduler_, refill_, log_);
}

void CoreRuntime::InvalidateCatalog() {
    // **Both halves, and the order matters little but the pairing does.**
    // Dropping the catalog's derived facts without dropping the page frames
    // they were derived from is a no-op: the next scan reads the same stale
    // bytes back and reaches the same conclusion. The frames are the
    // authority, the cache is the memo.
    if (Status s = store_->EvictClean(catalog::kEveryCatalogPage); !s.ok()) {
        // A dirty catalog frame on a peer means something wrote a page it
        // may not write, which the store's own check should already have
        // refused. Reported rather than propagated - there is no caller,
        // this runs in a message handler - and the cache is left alone,
        // because a half-invalidated view is worse than a stale one.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("core", "core " + std::to_string(config_.core_id) +
                                    ": catalog invalidation failed: " + s.message());
        }
        return;
    }
    catalog_->InvalidateFromPeer();
}

void CoreRuntime::Run() {
    // The two `system`-group cadences Expeditor already ran on the single
    // core, now per core. Both are no-ops on a core with nothing logged -
    // which today is every core but 0 (see the header) - so arming them
    // costs a timer and buys the property that a core which *starts*
    // logging needs no new wiring.
    if (config_.wal_drain_interval_ns > 0) {
        scheduler_->SubmitEvery(config_.wal_drain_interval_ns, [this] {
            if (Status s = wal_->DrainOnce(); !s.ok() && log_ != nullptr &&
                                              log_->enabled(LogLevel::kError)) {
                log_->Error("wal", "core " + std::to_string(config_.core_id) +
                                       ": drain failed: " + s.message());
            }
        });
    }

    // The low-water check (extent_lease_service.hpp). It runs on the WAL
    // drain's cadence rather than one of its own: both are cheap `system`
    // work, and a second timer for a check that is one integer comparison
    // would cost more than it measures.
    if (transport_ != nullptr && config_.wal_drain_interval_ns > 0) {
        scheduler_->SubmitEvery(config_.wal_drain_interval_ns, [this] { MaybeRefillLease(); });
    }

    // Per core, on the thread that will run the statements - the audit's
    // counters are core-local (exec/step_vm.cpp), so installing it once on
    // the startup thread would leave every worker unguarded.
    exec::InstallSuspendAudit();

    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("core", "core " + std::to_string(config_.core_id) + " reactor running");
    }
    scheduler_->Run();
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("core", "core " + std::to_string(config_.core_id) + " reactor stopped");
    }
}

void CoreRuntime::MaybeRefillLease() {
    // Asked for *before* the lease is spent, because allocation itself
    // cannot await anything (extent_lease.hpp) - by the time Next() fails
    // it is already too late for this statement.
    if (refill_in_flight_ || !lease_.low_water()) return;

    refill_in_flight_ = true;
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kSystem,
        RequestExtentRefill(*transport_, lease_, refill_, config_.core_id, /*system_core=*/0,
                            log_),
        [this](const Status& s) {
            refill_in_flight_ = false;
            if (!s.ok() && log_ != nullptr && log_->enabled(LogLevel::kError)) {
                // Nothing to return it to - this is a background task - and
                // the consequence is bounded: allocation on this core fails
                // retryably until a later tick succeeds.
                log_->Error("extent", "core " + std::to_string(config_.core_id) +
                                          ": lease refill failed: " + s.message());
            }
        }));
}

Status CoreRuntime::Sync() { return wal_->SyncAll(); }

}  // namespace kds::server
