#include "kds/server/core_runtime.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "kds/sched/send_retry.hpp"

#include "kds/exec/step_vm.hpp"
#include "kds/sched/epoll_io_backend.hpp"
#include "kds/server/mount_recovery.hpp"

namespace kds::server {

// The header states the rule; this is all it takes. Dropping the optional
// destroys the ready queues, and with them every unfinished `CoroTask` and
// the coroutine frame it owns - while `txn_manager_`, `catalog_` and
// `store_` are still standing, which is exactly what those frames' locals
// need.
CoreRuntime::~CoreRuntime() { scheduler_.reset(); }

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
    runtime->store_->SetWalGate(runtime->wal_.get());

    // **This core's recovery** (RV1/RV2, server/mount_recovery.hpp): each
    // stream is independent, so a peer recovers its own rather than waiting
    // on core 0 - and no order between the two is introduced, which is what
    // workplan-crosscore.md guideline 3 forbids.
    //
    // It runs **before `SetCoreOwnership`**, deliberately. RC04's repair
    // raises the store's allocation floor, and `DevicePageStore` refuses that
    // raise once a lease is installed - correctly, since a leased core takes
    // its ids from its extent and never consults the floor. Installing the
    // lease first would therefore make every peer with a non-empty stream
    // refuse its own mount.
    //
    // The undo log is built here for the same reason: recovery's undo phase
    // writes through it, and the rest of the transaction stack must not exist
    // yet, because `TrxIdSequence` caches the transaction ceiling at
    // construction (txn/trx_id.hpp).
    runtime->undo_log_.emplace(*runtime->store_, &*runtime->wal_);
    auto recovered =
        RecoverCoreAtMount(config.core_id, config.anchor, *runtime->log_device_,
                           *runtime->store_, *runtime->undo_log_, &*runtime->wal_, log);
    if (!recovered.ok()) return recovered.status();

    // A peer may not raise the durable transaction ceiling - the superblock is
    // page 0 and belongs to the system core (M5), and `superblock_` here is a
    // copy. Refused rather than applied to the copy, which would be a raise
    // nothing persists and a ceiling core 0 never learns of.
    //
    // **The ceiling it compares against is core 0's, and PW1 is when that
    // started mattering.** `superblock_` is default-constructed - zero
    // everywhere, `next_trx_id` included - so this check used to compare a
    // recovered stream against 0. That was harmless only while a peer's
    // stream named no transaction of its own, which the sentence this
    // comment replaced predicted would end with the id leases: the first
    // peer that writes and then remounts would recover ids above 0 and
    // refuse its own mount, on a database that did nothing wrong. So core 0
    // copies its ceiling in at startup, exactly as it copies this core's WAL
    // anchor and for the same reason - the field cannot be read off
    // `superblock_`, and a zero there is legal, silent and wrong.
    if (config.next_trx_id > runtime->superblock_.next_trx_id()) {
        if (Status s = runtime->superblock_.SetNextTrxId(config.next_trx_id); !s.ok()) return s;
    }
    if (recovered.value().next_trx_id > runtime->superblock_.next_trx_id()) {
        return Status::Unsupported(
            "core " + std::to_string(config.core_id) + ": its log names transaction id " +
            std::to_string(recovered.value().next_trx_id - 1) +
            ", above the ceiling the superblock carries; the system core owns page 0 and grants "
            "this core its id blocks (docs/workplan-peer-writer.md PW1)");
    }

    // **No completion checkpoint here** (RC08), and the reason is the same one
    // that makes the refusal above a refusal: publishing an anchor means
    // writing page 0, which belongs to the system core (M5), and the ring this
    // core would send it over (remote_checkpoint_anchor.hpp) is not attached
    // until AttachTransport().
    //
    // **Not here is no longer nowhere** (PW3). What used to make that a
    // permanent state was that a peer could not reserve a transaction id, so
    // its stream held no writes of its own to rescan; PW1 ended it. Only the
    // *placement* survives: the completion checkpoint runs at
    // `AttachTransport()`, where the ring exists, and the cadence in `Run()`.
    // A peer with no transport still publishes nothing and still rescans,
    // which describes a test fixture rather than a server. Core 0's own
    // checkpoint runs in Expeditor::Open.
    runtime->store_->SetCoreOwnership(config.core_id, &runtime->lease_, kFirstUserPageId);

    // The catalog, read-only in practice: DDL is core 0's, and the store
    // above refuses a write to the pages it lives on. What this instance
    // does is *read* and cache, and drop the cache when core 0 says so.
    runtime->catalog_.emplace(*runtime->store_, config.inline_cell_width, config.core_count);
    runtime->catalog_->SetLogger(log);
    // RV3: a peer may not write a catalog page (P6), so this should never
    // fire - but if a write ever slips through, logged beats silent.
    runtime->catalog_->SetWal(runtime->wal_.get());
    // A peer may not write the catalog, so its row ids come from leased
    // blocks (P5's shape, catalog/row_id_lease.hpp): AllocateRowId() draws
    // from this table, and a spent block is retryable exhaustion until the
    // kRowIdLease refill lands. Core 0 keeps the direct path - it owns the
    // page the sequence lives on.
    if (config.core_id != 0) {
        runtime->catalog_->SetRowIdLeases(&runtime->row_id_leases_);
    }

    // The transaction stack. `superblock_` is a copy (see the header): the
    // sequence would write through it, which is why the persist callback
    // below refuses rather than pretending.
    runtime->trx_ids_.emplace(runtime->superblock_, [core_id = config.core_id] {
        // A peer may not write the superblock - it is page 0 and belongs to
        // the system core (M5). Since PW1 a peer does not come here at all:
        // its sequence draws windows from the lease installed below, and
        // this callback is the backstop that says a lease source went
        // missing rather than a gap that has not been filled.
        return Status::Unsupported("core " + std::to_string(core_id) +
                                    " cannot raise the transaction-id ceiling; the superblock "
                                    "belongs to the system core, and this core's transaction-id "
                                    "lease source is not installed");
    });
    // Transaction ids come from a leased block on a peer, exactly as row
    // ids do above (`docs/workplan-peer-writer.md` PW1). Installed here
    // rather than at AttachTransport because a peer without a transport
    // must fail its first write with the lease's retryable exhaustion, not
    // with the superblock refusal above - the refusal names a wiring bug,
    // and a transport-less core is a test fixture rather than one.
    if (config.core_id != 0) {
        runtime->trx_ids_->SetLeaseSource(&runtime->trx_id_lease_);
    }
    // The undo log is already built - recovery wrote its compensations
    // through it above, before this stack existed.
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

    // CC7's handoff (workplan P6b): core 0 flushed a relation's pages and
    // is handing this core fault rights over them. Ordering against
    // kCatalogInvalidate does not matter: a statement racing either answers
    // retryably, exactly as the invalidation window above.
    if (Status s = scheduler_->RegisterMessageHandler(
            sched::RingMessageKind::kRelationFaultGrant,
            [this](const sched::MessageHeader&, std::span<const std::byte> payload) {
                if (payload.size() != sizeof(ExtentGrantPayload)) {
                    if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                        log_->Error("core", "core " + std::to_string(config_.core_id) +
                                                " dropped a malformed relation fault grant (" +
                                                std::to_string(payload.size()) + " bytes)");
                    }
                    return;
                }
                ExtentGrantPayload grant{};
                std::memcpy(&grant, payload.data(), sizeof(grant));
                GrantRelationFault(storage::Extent{grant.first_page_id, grant.page_count});
            });
        !s.ok()) {
        return s;
    }

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
    remote_steps_.emplace(
        *catalog_, *store_, config_.core_id,
        [this](std::uint32_t dst, sched::RingMessageKind kind, std::vector<std::byte> payload) {
            sched::MessageHeader out{};
            out.src_core = config_.core_id;
            out.dst_core = dst;
            out.session_core = dst;
            out.kind = static_cast<std::uint16_t>(kind);
            out.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kForeground);
            scheduler_->Submit(sched::MakeSendRetryTask(*transport_, out, payload));
            return Status::OK();
        },
        log_, kStepBatchTargetBytes,
        [this](std::unique_ptr<sched::Task> task) { scheduler_->Submit(std::move(task)); },
        &*txn_manager_,
        // And this core's configured row-touch ceiling, which the server
        // ignored until P4d-4c's review - a shipped statement was bounded
        // only by whatever a fresh `exec::Budget()` defaulted to.
        config_.budget);
    if (Status s = scheduler_->RegisterMessageHandler(
            sched::RingMessageKind::kStepOpen,
            [this](const sched::MessageHeader& header, std::span<const std::byte> payload) {
                remote_steps_->OnStepOpen(header, payload);
            });
        !s.ok()) {
        return s;
    }
    if (Status s = scheduler_->RegisterMessageHandler(
            sched::RingMessageKind::kStepCredit,
            [this](const sched::MessageHeader&, std::span<const std::byte> payload) {
                remote_steps_->OnStepCredit(payload);
            });
        !s.ok()) {
        return s;
    }
    if (Status s = scheduler_->RegisterMessageHandler(
            sched::RingMessageKind::kStepCancel,
            [this](const sched::MessageHeader&, std::span<const std::byte> payload) {
                remote_steps_->OnStepCancel(payload);
            });
        !s.ok()) {
        return s;
    }
    // The input edges of consuming stages (P4d-4b-2): a peer had no
    // kStepBatch/kStepEof consumer until pipelines grew middles. Safe
    // because these are a *peer's* scheduler and `SessionStepClient`'s
    // same-kind handlers are core 0's - the map holds one handler per
    // kind and assigns, so two claimants on one scheduler would be a
    // silent drop, not a fan-out (remote_step_service.hpp).
    if (Status s = scheduler_->RegisterMessageHandler(
            sched::RingMessageKind::kStepBatch,
            [this](const sched::MessageHeader&, std::span<const std::byte> payload) {
                remote_steps_->OnStepBatch(payload);
            });
        !s.ok()) {
        return s;
    }
    if (Status s = scheduler_->RegisterMessageHandler(
            sched::RingMessageKind::kStepEof,
            [this](const sched::MessageHeader&, std::span<const std::byte> payload) {
                remote_steps_->OnStepEof(payload);
            });
        !s.ok()) {
        return s;
    }

    // The grant side of the page-id lease (workplan P5). Registered here
    // rather than in Run() because a grant can arrive before this core has
    // armed anything.
    transport_ = &transport;
    if (Status s = RegisterExtentGrantReceiver(*scheduler_, refill_, log_); !s.ok()) return s;

    // **This core's checkpointer** (PW3, docs/workplan-peer-writer.md), and
    // it can only be built here: the anchor publishes over the ring, so it
    // cannot exist before the ring does.
    //
    // Peers only. Core 0's checkpointer is `Expeditor`'s and writes page 0
    // directly; a core-0 `CoreRuntime` exists only in tests, and giving it
    // one would have it send its own anchor to itself.
    if (config_.core_id == 0) return Status::OK();

    checkpoint_target_.emplace(*store_);
    checkpoint_anchor_.emplace(transport, *scheduler_, config_.core_id, /*system_core=*/0);
    checkpoint_anchor_->SetLogger(log_);
    checkpointer_.emplace(*wal_, *checkpoint_target_, *txn_manager_, *checkpoint_anchor_);
    checkpointer_->SetLogger(log_);
    // AS6a's snapshot source, wired on the same terms core 0 wires it. A
    // peer's registry is **empty today** - `ResumeAssertionsAfterRecovery`
    // runs only on core 0 - so this writes no group snapshots. Wired anyway
    // rather than omitted, because the omission would be the silent kind: a
    // peer that later enforces would checkpoint without snapshots and the
    // next mount would find no base to fold from, which is the failure RC07
    // exists to prevent.
    checkpointer_->SetAssertionSource(&dispatcher_->assertions());

    // **The completion checkpoint** (RC08), which core 0 runs at the end of
    // its own recovery and a peer could not: it publishes an anchor past
    // everything this core's mount replayed, so the next crash scans from
    // here rather than from wherever core 0 last wrote this slot. Run on the
    // startup thread, before the worker exists - the send it queues is
    // picked up by this core's own reactor once `Run()` starts, and core 0's
    // `kAnchorWrite` handler is registered before any peer attaches.
    //
    // Through the same helper core 0 uses, rather than through `Checkpoint()`
    // below: `CheckpointAfterRecovery` *is* "the completion checkpoint" as a
    // named thing, and it carries three that the cadence path does not - the
    // `NoActiveTransactions` table (empty by fact here, and its signature is
    // what makes handing over a stale one impossible), the Info line that is
    // a peer's only evidence its mount bounded the next crash, and the
    // context on failure that says which checkpoint aborted the mount.
    return CheckpointAfterRecovery(config_.core_id, *wal_, *checkpoint_target_,
                                   *checkpoint_anchor_, log_, /*clock=*/nullptr,
                                   /*elapsed_ns=*/nullptr, &dispatcher_->assertions());
}

void CoreRuntime::GrantRelationFault(storage::Extent extent) {
    store_->GrantFaultPages(extent);
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("core", "core " + std::to_string(config_.core_id) +
                                " granted fault range [" + std::to_string(extent.first) + ", " +
                                std::to_string(extent.end()) + ")");
    }
}

storage::Extent RelationFaultExtentOf(const catalog::SysTableRow& row,
                                      std::uint32_t extent_pages) {
    PageId low = row.desc_page_id;
    PageId high = row.desc_page_id;
    if (row.varheap_page_id != kInvalidPageId) {
        low = std::min(low, row.varheap_page_id);
        high = std::max(high, row.varheap_page_id);
    }
    const PageId first = (low / extent_pages) * extent_pages;
    const PageId end = ((high / extent_pages) + 1) * extent_pages;
    return storage::Extent{first, end - first};
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
    auto drain = [this] {
        if (Status s = wal_->DrainOnce(); !s.ok() && log_ != nullptr &&
                                          log_->enabled(LogLevel::kError)) {
            log_->Error("wal", "core " + std::to_string(config_.core_id) +
                                   ": drain failed: " + s.message());
        }
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

    // The low-water check (extent_lease_service.hpp). It runs on the WAL
    // drain's cadence rather than one of its own: both are cheap `system`
    // work, and a second timer for a check that is one integer comparison
    // would cost more than it measures.
    if (transport_ != nullptr && config_.wal_drain_interval_ns > 0) {
        scheduler_->SubmitEvery(config_.wal_drain_interval_ns, [this] { MaybeRefillLease(); });
        // The transaction-id lease rides the same tick (PW1). A peer that
        // has never held a window reads as low, so the first tick asks and
        // a peer is ready to write before a client arrives - which is the
        // point, since `TrxIdSequence::Next()` cannot await a grant.
        if (config_.core_id != 0) {
            scheduler_->SubmitEvery(config_.wal_drain_interval_ns,
                                    [this] { MaybeRefillTrxIds(); });
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
    exec::InstallSuspendAudit(store_.get());

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

void CoreRuntime::MaybeRefillTrxIds() {
    // Asked for *before* the window is spent, the extent lease's rule and
    // for its reason: `TrxIdSequence::Next()` is called from inside a
    // statement and cannot await, so by the time it reports exhaustion the
    // statement is already lost.
    if (trx_id_refill_in_flight_ || !trx_ids_->low_water()) return;

    trx_id_refill_in_flight_ = true;
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kSystem,
        RequestTrxIdLease(*transport_, trx_id_refill_, config_.core_id, /*system_core=*/0, log_),
        [this](const Status& s) {
            trx_id_refill_in_flight_ = false;
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
    // Nothing to do on a core that has no checkpointer: core 0, whose one
    // lives on `Expeditor`, and any runtime built without a transport, which
    // has nowhere to publish an anchor to.
    if (!checkpointer_.has_value()) return Status::OK();

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

Status CoreRuntime::Sync() { return wal_->SyncAll(); }

}  // namespace kds::server
