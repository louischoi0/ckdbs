#include "sim/instance.hpp"

#include <utility>

#include "kds/server/mount_recovery.hpp"
#include "kds/server/superblock.hpp"
#include "kds/server/superblock_checkpoint_anchor.hpp"
#include "kds/storage/page_store_checkpoint_target.hpp"

namespace kds::sim {

// Fixed and deliberately unrealistic: the harness has no wall clock
// ([OPEN: sim clock] in bench/workplan-teststrategy), and last_mount_time
// is the only consumer. A constant keeps two runs of one seed
// byte-identical.
constexpr std::uint64_t kMountTime = 1000;

StatusOr<std::unique_ptr<SimInstance>> SimInstance::Create(Options options) {
    std::unique_ptr<SimInstance> instance(new SimInstance());
    instance->options_ = options;

    auto page_device = storage::MemoryPageDevice::Create(options.extent_pages,
                                                         options.initial_pages);
    if (!page_device.ok()) return page_device.status();
    instance->page_device_ = std::move(page_device.value());

    auto log_device = wal::MemoryLogDevice::Create(options.wal_segment_bytes);
    if (!log_device.ok()) return log_device.status();
    instance->log_device_ = std::move(log_device.value());

    if (Status s = instance->Boot(); !s.ok()) return s;
    return instance;
}

Status SimInstance::Boot() {
    auto wal = wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/0);
    if (!wal.ok()) return wal.status();
    wal_ = std::move(wal.value());

    auto store = storage::DevicePageStore::Open(*page_device_, server::kFirstUserPageId);
    if (!store.ok()) return store.status();
    store_ = std::move(store.value());
    store_->SetWalGate(wal_.get());

    auto boot = bootstrap::BootstrapDatabase(*store_, kMountTime);
    if (!boot.ok()) return boot.status();
    boot_.emplace(std::move(boot.value()));
    // RV3, exactly where the expeditor arms it: after bootstrap (which is
    // entitled to run unlogged), before recovery and everything else.
    boot_->catalog.SetWal(wal_.get());

    // **Recovery, exactly where the expeditor runs it** (RV1,
    // server/mount_recovery.hpp). The harness is a full instance, and a
    // reboot that skipped the phase a real mount runs would grade an engine
    // this project does not ship: SIM04's crash contract is what recovery is
    // *for*, so the loop must be measuring the recovered image.
    //
    // The undo log comes first because undo writes through it, and the id
    // sequence comes after because it caches the transaction ceiling at
    // construction (txn/trx_id.hpp) - the same ordering Expeditor::Open has.
    // Whether this boot owes the two steps that have to follow the dispatcher
    // (see below). A local: both the set and the read are inside this function.
    const bool run_recovery_tail = !options_.skip_recovery;

    undo_.emplace(*store_, wal_.get());
    if (!options_.skip_recovery) {
        auto recovered = server::RecoverCoreAtMount(/*core_id=*/0, boot_->superblock.wal_anchor(0),
                                                    *log_device_, *store_, *undo_, wal_.get(),
                                                    /*log=*/nullptr);
        if (!recovered.ok()) return recovered.status();
        recovery_ = recovered.value();
        // RV3 D3a, exactly as the expeditor does it: redo mutated catalog
        // pages, so whatever the cache holds predates them.
        boot_->catalog.InvalidateFromPeer();
        if (recovered.value().next_trx_id > boot_->superblock.next_trx_id()) {
            if (Status s = boot_->superblock.SetNextTrxId(recovered.value().next_trx_id); !s.ok()) {
                return s;
            }
            if (Status s = PersistSuperBlock(); !s.ok()) return s;
        }

        // The completion checkpoint (RC08), for the reason the harness runs
        // recovery at all: without it every reboot rescans from the head of the
        // stream, because nothing here runs the periodic checkpointer - so the
        // harness would be measuring a mount cost no server pays and missing
        // the one property RC08 adds.
    }

    // The persist callback, exactly as the expeditor wires it: the harness
    // is a full instance, and the null-persist path is the socket-free
    // tests' — ids reissued across a restart would be *this harness's*
    // fault, not the engine's.
    trx_ids_.emplace(boot_->superblock, [this] { return PersistSuperBlock(); });
    txn_.emplace(*trx_ids_, *undo_, *store_, wal_.get());

    // The advisory features, wired exactly as Expeditor::Open wires them
    // and gated by the same three switches (SIM06).
    if (options_.cabins) cabin_store_.emplace(stats::CabinLimits{});
    if (options_.waystone) trail_recorder_.emplace(boot_->catalog, *store_, &clock_);

    dispatcher_.emplace(boot_->superblock, boot_->catalog, *store_, /*log=*/nullptr,
                        /*clock=*/nullptr, wal_.get(), options_.durability,
                        exec::Budget(),
                        trail_recorder_ ? &*trail_recorder_ : nullptr,
                        /*replay_enabled=*/options_.waystone,
                        options_.access_statistics,
                        cabin_store_ ? &*cabin_store_ : nullptr, &*txn_);
    session_ = server::Session();

    // Assertion enforcement and the completion checkpoint, in the expeditor's
    // order and for its reason (RC07/RC08): the registry lives on the dispatcher
    // that has only just been built, and the checkpoint has to be written *after*
    // it is refilled, because that checkpoint becomes the anchor the next mount
    // folds its directories from.
    if (run_recovery_tail) {
        recovery_ = server::ResumeAssertionsAfterRecovery(
            boot_->catalog, *store_, *log_device_, /*core_id=*/0,
            boot_->superblock.wal_anchor(0).checkpoint_lsn, dispatcher_->assertions(),
            recovery_, /*log=*/nullptr);
        if (Status s = RunCheckpoint(); !s.ok()) return s;
    }
    return Status::OK();
}

Status SimInstance::RunCheckpoint() {
    storage::PageStoreCheckpointTarget target(*store_);
    server::SuperBlockCheckpointAnchor anchor(boot_->superblock, *store_);
    return server::CheckpointAfterRecovery(/*core_id=*/0, *wal_, target, anchor,
                                           /*log=*/nullptr, /*clock=*/nullptr,
                                           /*elapsed_ns=*/nullptr, &dispatcher_->assertions());
}

void SimInstance::TearDown() {
    // Reverse of Boot(). The session first: it may hold a pointer into the
    // transaction manager it sits above.
    session_ = server::Session();
    dispatcher_.reset();
    trail_recorder_.reset();
    cabin_store_.reset();
    txn_.reset();
    undo_.reset();
    trx_ids_.reset();
    boot_.reset();
    store_.reset();
    wal_.reset();
}

void SimInstance::Crash() {
    // The devices lose their unsynced halves *first*: from this moment the
    // image is what a power cut left, and tearing the stack down afterwards
    // cannot write (no component has a flushing destructor — that property
    // is what makes this two-liner a crash rather than a shutdown).
    page_device_->Crash();
    log_device_->Crash();
    TearDown();
}

Status SimInstance::CleanShutdown() {
    // The dispatcher's own SYNC: log first, then the store, the same order
    // and the same code a client's SYNC runs.
    const std::string reply = Execute("SYNC");
    if (reply.rfind("OK", 0) != 0) {
        return Status::IoError("clean shutdown: SYNC answered: " + reply);
    }
    // The anchor, on the way out. `Expeditor::Serve` does this for the reason
    // this harness has to as well: a stop that syncs and publishes nothing
    // leaves the next mount re-reading every record this run wrote.
    if (Status s = RunCheckpoint(); !s.ok()) return s;
    TearDown();
    return Status::OK();
}

Status SimInstance::PersistSuperBlock() {
    // Expeditor::PersistSuperBlock's shape: encode into the resident frame,
    // then sync the store so the raised ceiling is really on the platter.
    auto page = store_->Get(server::kSuperBlockPageId);
    if (!page.ok()) return page.status();
    boot_->superblock.Encode(page.value().bytes());
    return store_->Sync();
}

Status SimInstance::Reboot() {
    if (running()) {
        return Status::InvalidArgument("Reboot() while the engine is up; Crash() or "
                                       "CleanShutdown() first");
    }
    return Boot();
}

std::string SimInstance::Execute(std::string_view sql) {
    return dispatcher_->Dispatch(sql, &session_).response;
}

}  // namespace kds::sim
