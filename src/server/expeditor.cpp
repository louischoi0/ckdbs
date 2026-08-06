#include "kds/server/expeditor.hpp"

#include <pthread.h>
#include <sched.h>

#include <cstring>
#include <limits>
#include <thread>
#include <utility>

#include "kds/exec/step_vm.hpp"
#include "kds/sched/epoll_io_backend.hpp"
#include "kds/sched/send_retry.hpp"
#include "kds/server/remote_checkpoint_anchor.hpp"
#include "kds/storage/file_page_device.hpp"

namespace kds::server {

std::string Expeditor::Config::LogPath() const {
    if (log_file.empty()) return {};                 // logging to a file disabled
    if (log_dir.empty() || log_file.front() == '/') return log_file;
    if (log_dir.back() == '/') return log_dir + log_file;
    return log_dir + "/" + log_file;
}

std::vector<std::string> Expeditor::Config::KnownConfigKeys() {
    return {"data_file",  "port",     "wal_dir",  "checkpoint_interval_ms", "durability",
            "isolation",
            "wal_drain_interval_us", "relaxed_flush_interval_us",
            "log_dir",  "log_file",               "log_level",
            "max_rows_touched",      "inline_cell_width",      "waystone_recording",
            "waystone_replay",
            "access_statistics",       "cabins",   "cabin_max_values",
            "cabin_max_entries_per_value", "cores"};
}

Status Expeditor::Config::ApplyFile(const ConfigFile& file) {
    std::vector<std::string> unknown = file.UnknownKeys(KnownConfigKeys());
    if (!unknown.empty()) {
        std::string msg = file.origin() + ": unknown config key(s):";
        for (const std::string& key : unknown) msg += " '" + key + "'";
        return Status::InvalidArgument(std::move(msg));
    }

    if (file.Has("data_file")) {
        auto v = file.GetString("data_file");
        if (!v.ok()) return v.status();
        data_file = std::move(v.value());
    }
    if (file.Has("port")) {
        auto v = file.GetUint("port");
        if (!v.ok()) return v.status();
        if (v.value() == 0 || v.value() > std::numeric_limits<std::uint16_t>::max()) {
            return Status::InvalidArgument(file.origin() + ": port " + std::to_string(v.value()) +
                                            " is outside 1..65535");
        }
        port = static_cast<std::uint16_t>(v.value());
    }
    if (file.Has("wal_dir")) {
        auto v = file.GetString("wal_dir");
        if (!v.ok()) return v.status();
        wal_dir = std::move(v.value());
    }
    if (file.Has("checkpoint_interval_ms")) {
        auto v = file.GetUint("checkpoint_interval_ms");
        if (!v.ok()) return v.status();
        // Expressed in ms in the file because that is the unit an operator
        // reasons about, and held in ns internally because that is the
        // scheduler's. 0 keeps its documented meaning: no cadence.
        checkpoint_interval_ns = v.value() * 1'000'000ULL;
    }
    if (file.Has("durability")) {
        auto v = file.GetString("durability");
        if (!v.ok()) return v.status();
        auto parsed = wal::ParseDurabilityClass(v.value());
        if (!parsed.ok()) {
            return Status::InvalidArgument(file.origin() + ": " + parsed.status().message());
        }
        durability = parsed.value();
    }
    if (file.Has("waystone_recording")) {
        auto v = file.GetBool("waystone_recording");
        if (!v.ok()) return v.status();
        waystone_recording = v.value();
    }
    if (file.Has("isolation")) {
        auto v = file.GetString("isolation");
        if (!v.ok()) return v.status();
        auto parsed = txn::ParseIsolationLevel(v.value());
        if (!parsed.ok()) return parsed.status();
        isolation = parsed.value();
    }
    if (file.Has("waystone_replay")) {
        auto v = file.GetBool("waystone_replay");
        if (!v.ok()) return v.status();
        waystone_replay = v.value();
    }
    if (file.Has("access_statistics")) {
        auto v = file.GetBool("access_statistics");
        if (!v.ok()) return v.status();
        access_statistics = v.value();
    }
    if (file.Has("cabins")) {
        auto v = file.GetBool("cabins");
        if (!v.ok()) return v.status();
        cabins = v.value();
    }
    if (file.Has("cabin_max_values")) {
        auto v = file.GetUint("cabin_max_values");
        if (!v.ok()) return v.status();
        // No upper range check, and no zero check either: 0 means no value
        // may be observed, which is a coherent way to keep the catalog
        // objects while switching the behaviour off per instance.
        cabin_max_values = static_cast<std::size_t>(v.value());
    }
    if (file.Has("cabin_max_entries_per_value")) {
        auto v = file.GetUint("cabin_max_entries_per_value");
        if (!v.ok()) return v.status();
        cabin_max_entries_per_value = static_cast<std::size_t>(v.value());
    }
    if (file.Has("max_rows_touched")) {
        auto v = file.GetUint("max_rows_touched");
        if (!v.ok()) return v.status();
        // No range check beyond the parse: 0 is a documented value
        // (unlimited) and there is no upper bound worth inventing - a
        // ceiling too high to reach is the same as no ceiling, which the
        // operator has already asked for by setting it.
        max_rows_touched = v.value();
    }
    if (file.Has("wal_drain_interval_us")) {
        auto v = file.GetUint("wal_drain_interval_us");
        if (!v.ok()) return v.status();
        // Microseconds in the file: a drain interval an operator cares
        // about is well under a millisecond, so ms would round it away.
        wal_drain_interval_ns = v.value() * 1'000ULL;
    }
    if (file.Has("relaxed_flush_interval_us")) {
        auto v = file.GetUint("relaxed_flush_interval_us");
        if (!v.ok()) return v.status();
        relaxed_flush_interval_ns = v.value() * 1'000ULL;
    }
    if (file.Has("inline_cell_width")) {
        auto v = file.GetUint("inline_cell_width");
        if (!v.ok()) return v.status();
        // Range-checked through the same function the superblock validates
        // with, so a config file and a data file can never disagree about
        // what a legal width is.
        if (v.value() > std::numeric_limits<std::uint32_t>::max()) {
            return Status::InvalidArgument(file.origin() + ": inline_cell_width " +
                                            std::to_string(v.value()) + " is not a u32");
        }
        auto width = static_cast<std::uint32_t>(v.value());
        if (Status s = storage::CheckInlineCellWidth(width); !s.ok()) {
            return Status::InvalidArgument(file.origin() + ": " + s.message());
        }
        inline_cell_width = width;
    }
    if (file.Has("cores")) {
        auto v = file.GetUint("cores");
        if (!v.ok()) return v.status();
        if (v.value() > std::numeric_limits<std::uint32_t>::max()) {
            return Status::InvalidArgument(file.origin() + ": cores " +
                                            std::to_string(v.value()) + " is not a u32");
        }
        auto count = static_cast<std::uint32_t>(v.value());
        // Range-checked through the same function the superblock validates
        // with, so a config file and a data file can never disagree about
        // what a legal core count is - the arrangement inline_cell_width
        // above already uses.
        if (Status s = CheckCoreCount(count); !s.ok()) {
            return Status::InvalidArgument(file.origin() + ": " + s.message());
        }
        cores = count;
    }
    if (file.Has("log_dir")) {
        auto v = file.GetString("log_dir");
        if (!v.ok()) return v.status();
        log_dir = std::move(v.value());
    }
    if (file.Has("log_file")) {
        auto v = file.GetString("log_file");
        if (!v.ok()) return v.status();
        log_file = std::move(v.value());
    }
    if (file.Has("log_level")) {
        auto v = file.GetString("log_level");
        if (!v.ok()) return v.status();
        auto level = ParseLogLevel(v.value());
        if (!level.ok()) return Status::InvalidArgument(file.origin() + ": " +
                                                        level.status().message());
        log_level = level.value();
    }
    return Status::OK();
}

Expeditor::Expeditor(Config config, std::unique_ptr<storage::PageDevice> device,
                     std::unique_ptr<storage::DevicePageStore> store) noexcept
    : config_(std::move(config)), device_(std::move(device)), store_(std::move(store)) {}

StatusOr<std::unique_ptr<Expeditor>> Expeditor::Open(Config config,
                                                     std::uint64_t now_unix_seconds) {
    if (config.wal_dir.empty()) {
        config.wal_dir = config.data_file + ".wal";
    }

    // Checked here rather than in ApplyFile() because it is the one
    // validation that depends on the machine rather than on the value: a
    // config file is portable and a hardware core count is not, so the same
    // file must be able to fail on one host and pass on another. This is
    // the platform layer (rules.md #4), which is the only place allowed to
    // ask the hardware anything.
    //
    // Reactors are pinned and never block, so N of them on fewer than N
    // cores does not run slower - it runs one reactor's whole workload
    // behind another's, with no preemption to break the tie.
    if (Status s = CheckCoreCount(config.cores); !s.ok()) return s;
    const unsigned hardware_cores = std::thread::hardware_concurrency();
    // 0 means "not detectable" - not "no cores". Skipping the check is the
    // only honest response; refusing would make the server unstartable on a
    // platform that simply declines to answer.
    if (hardware_cores > 0 && config.cores > hardware_cores) {
        return Status::InvalidArgument(
            "cores " + std::to_string(config.cores) + " exceeds the " +
            std::to_string(hardware_cores) +
            " this machine reports; reactors are pinned one per core and never block, so "
            "overcommitting them serializes whole workloads behind each other");
    }

    auto device = storage::FilePageDevice::Open(config.data_file);
    if (!device.ok()) return device.status();

    auto store = storage::DevicePageStore::Open(*device.value(), kFirstUserPageId);
    if (!store.ok()) return store.status();

    // Built here rather than in the initializer list because the members
    // below take references into it, which only become stable once the
    // Expeditor itself is on the heap and pinned.
    auto expeditor = std::unique_ptr<Expeditor>(new Expeditor(
        std::move(config), std::move(device.value()), std::move(store.value())));

    // Opened first, so everything after it can report. A log that cannot be
    // opened *is* fatal: the operator asked for a specific destination, and
    // starting anyway would run a server whose diagnostics silently go
    // nowhere - which is discovered at exactly the wrong moment.
    if (Status s = expeditor->OpenLog(); !s.ok()) return s;
    expeditor->logger_->Info("expeditor", "opening database '" + expeditor->config_.data_file +
                                              "', wal dir '" + expeditor->config_.wal_dir + "'");

    // The store is opened before the log exists (the log's own destination
    // is configuration, and reading it must not depend on a database), so
    // it is handed the logger here rather than at construction. Everything
    // it did during Open() is reported by the "database ready" line below.
    expeditor->store_->SetLogger(&*expeditor->logger_);

    auto database =
        bootstrap::BootstrapDatabase(*expeditor->store_, now_unix_seconds,
                                     expeditor->config_.inline_cell_width,
                                     expeditor->config_.cores, &*expeditor->logger_);
    if (!database.ok()) return database.status();
    expeditor->database_.emplace(std::move(database.value()));
    // The Catalog was moved out of the BootstrapResult; its logger came
    // along, but re-setting it keeps that fact local instead of depending
    // on a copy elsewhere staying a copy.
    expeditor->database_->catalog.SetLogger(&*expeditor->logger_);

    // The WAL stack, before the dispatcher: INSERT logs through it, so the
    // dispatcher cannot be built until it exists.
    auto log_device = wal::FileLogDevice::Open(expeditor->config_.wal_dir, /*core_id=*/0);
    if (!log_device.ok()) return log_device.status();
    expeditor->log_device_ = std::move(log_device.value());

    wal::WalManagerConfig wal_config;
    wal_config.relaxed_flush_interval_ns = expeditor->config_.relaxed_flush_interval_ns;
    auto wal = wal::WalManager::Open(expeditor->log_device_.get(), expeditor->clock_,
                                     /*core_id=*/0, wal_config);
    if (!wal.ok()) return wal.status();
    expeditor->wal_ = std::move(wal.value());
    expeditor->wal_->SetLogger(&*expeditor->logger_);

    // WAL-before-data, enforced by the store rather than asked of its
    // callers (device_page_store.hpp): from here on no dirty page reaches
    // the device ahead of the records describing it.
    expeditor->store_->SetWalGate(expeditor->wal_.get());

    if (expeditor->config_.cabins) {
        expeditor->cabin_store_.emplace(
            stats::CabinLimits{expeditor->config_.cabin_max_values,
                               expeditor->config_.cabin_max_entries_per_value});
    }
    if (expeditor->config_.waystone_recording) {
        expeditor->trail_recorder_.emplace(expeditor->database_->catalog, *expeditor->store_,
                                           &expeditor->clock_);
    }
    // The transaction stack, before the dispatcher that reads through it.
    // The persist callback is what makes a reserved id block durable: the
    // superblock is unlogged, so a block is only safe once the page has
    // been written and synced (txn/trx_id.hpp records the exposure that
    // leaves).
    Expeditor* self = expeditor.get();
    expeditor->trx_ids_.emplace(expeditor->database_->superblock,
                                [self] { return self->PersistSuperBlock(); });
    expeditor->undo_log_.emplace(*expeditor->store_, &*expeditor->wal_);
    expeditor->txn_manager_.emplace(*expeditor->trx_ids_, *expeditor->undo_log_,
                                    *expeditor->store_, &*expeditor->wal_);

    expeditor->dispatcher_.emplace(
        expeditor->database_->superblock, expeditor->database_->catalog, *expeditor->store_,
        &*expeditor->logger_, &expeditor->clock_, &*expeditor->wal_,
        expeditor->config_.durability, exec::Budget(expeditor->config_.max_rows_touched),
        expeditor->trail_recorder_ ? &*expeditor->trail_recorder_ : nullptr,
        expeditor->config_.waystone_replay, expeditor->config_.access_statistics,
        expeditor->cabin_store_ ? &*expeditor->cabin_store_ : nullptr,
        &*expeditor->txn_manager_, expeditor->config_.isolation);
    expeditor->logger_->Info("expeditor",
                             std::string("INSERT durability ") +
                                 wal::DurabilityClassName(expeditor->config_.durability) +
                                 ", isolation " +
                                 txn::IsolationLevelName(expeditor->config_.isolation));

    expeditor->checkpoint_target_.emplace(*expeditor->store_);
    expeditor->checkpoint_anchor_.emplace(expeditor->database_->superblock, *expeditor->store_);
    expeditor->checkpoint_anchor_->SetLogger(&*expeditor->logger_);
    expeditor->checkpointer_.emplace(*expeditor->wal_, *expeditor->checkpoint_target_,
                                     *expeditor->txn_manager_,
                                     *expeditor->checkpoint_anchor_);
    expeditor->checkpointer_->SetLogger(&*expeditor->logger_);

    if (Status s = expeditor->Sync(); !s.ok()) return s;

    expeditor->logger_->Info("expeditor",
                             "database ready: " +
                                 std::to_string(expeditor->store_->allocated_pages()) +
                                 " pages, superblock version " +
                                 std::to_string(expeditor->database_->superblock.version()));
    return expeditor;
}

Status Expeditor::OpenLog() {
    const std::string path = config_.LogPath();
    if (path.empty()) {
        // No file configured. A Logger over a null sink still satisfies
        // every call site, so nothing downstream has to know.
        logger_.emplace(nullptr, wall_clock_, config_.log_level);
        return Status::OK();
    }

    auto sink = FileLogSink::Open(path);
    if (!sink.ok()) return sink.status();
    log_sink_ = std::move(sink.value());
    logger_.emplace(log_sink_.get(), wall_clock_, config_.log_level);
    return Status::OK();
}

Status Expeditor::PersistSuperBlock() {
    auto page = store_->Get(kSuperBlockPageId);
    if (!page.ok()) return page.status();
    database_->superblock.Encode(page.value());
    return Sync();
}

Status Expeditor::Checkpoint() {
    // CheckpointStats counters are cumulative over the process, so this
    // one's contribution is the delta. Logging the running total would
    // read as "this checkpoint flushed 5 pages" on every tick after the
    // first one that did.
    const std::uint64_t flushed_before = checkpointer_->stats().pages_flushed;

    Status s = checkpointer_->RunToCompletion();
    if (!s.ok()) {
        // The one place a checkpoint failure becomes visible. It runs on a
        // timer with no caller to return to, so without this it is a
        // silently widening loss window.
        logger_->Error("checkpoint", "checkpoint failed: " + s.message());
        return s;
    }

    const std::uint64_t flushed = checkpointer_->stats().pages_flushed - flushed_before;
    logger_->Debug("checkpoint", "checkpoint complete: redo_start=" +
                                     std::to_string(checkpointer_->redo_start_lsn()) +
                                     " pages_flushed=" + std::to_string(flushed));
    return Status::OK();
}

namespace {

// Pins `thread` to `core_id`. Best-effort by design: a container or a
// restricted cpuset can refuse, and a reactor that runs unpinned is slower
// rather than wrong - so this reports and continues instead of failing the
// server. The platform layer is allowed the syscall (rules.md #4).
void PinToCore(std::thread& thread, std::uint32_t core_id, Logger* log) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(core_id), &set);
    const int rc = pthread_setaffinity_np(thread.native_handle(), sizeof(set), &set);
    if (rc != 0 && log != nullptr && log->enabled(LogLevel::kWarn)) {
        log->Warn("expeditor", "could not pin core " + std::to_string(core_id) +
                                   " (errno " + std::to_string(rc) +
                                   "); it will run unpinned");
    }
}

}  // namespace

void Expeditor::BroadcastCatalogInvalidation(sched::Scheduler& core0_scheduler) {
    if (!transport_.has_value() || cores_.empty()) return;

    // **Flush before telling anyone.** Catalog writes are unlogged and
    // otherwise reach the device only at checkpoint or SYNC, so a peer told
    // to re-read now would read the state *before* this DDL - and conclude
    // the new relation does not exist, permanently, until something else
    // happened to flush. This is the ordering the whole scheme rests on.
    if (Status s = store_->FlushPages(catalog::kEveryCatalogPage); !s.ok()) {
        // Reported and not propagated: the DDL itself has already succeeded
        // and the caller is BumpVersion(), which returns void. The cost is
        // peers that keep a stale catalog until the next flush - stale, not
        // wrong, because a stale catalog answers "not found" and never a
        // wrong row.
        logger_->Error("catalog", "flushing catalog pages before invalidating peers failed: " +
                                      s.message());
        return;
    }

    for (const auto& core : cores_) {
        sched::MessageHeader header{};
        header.src_core = 0;
        header.dst_core = core->core_id();
        header.session_core = 0;
        header.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kCatalogInvalidate);
        header.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
        core0_scheduler.Submit(sched::MakeSendRetryTask(*transport_, header, {}));
    }
}

void Expeditor::BroadcastShutdown(sched::Scheduler& core0_scheduler) {
    if (!transport_.has_value()) return;

    for (const auto& core : cores_) {
        sched::MessageHeader header{};
        header.src_core = 0;
        header.dst_core = core->core_id();
        header.session_core = 0;
        header.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kShutdown);
        header.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
        core0_scheduler.Submit(sched::MakeSendRetryTask(*transport_, header, {}));
    }

    // Core 0's reactor has already left Run(), so nothing is draining its
    // ready queue - the sends above have to be pumped by hand. Bounded
    // rather than "until empty": a peer whose ring is full and whose reactor
    // has already stopped would otherwise hang the shutdown, and a core that
    // misses its message is joined below anyway once it notices its own
    // stop.
    for (int i = 0; i < 1000 && core0_scheduler.RunOnce(); ++i) {
    }
}

Status Expeditor::Serve() {
    auto io_backend = sched::EpollIoBackend::Create();
    if (!io_backend.ok()) return io_backend.status();

    auto listener = TcpServer::Listen(config_.port);
    if (!listener.ok()) return listener.status();

    sched::Scheduler scheduler(clock_, io_backend.value());
    scheduler.SetLogger(&*logger_);

    // Core-local, and installed before any statement runs: from here on a
    // coroutine that suspends while holding a page span is detected rather
    // than merely forbidden in prose (exec/step_vm.hpp, sched/coro.hpp).
    // Nothing in the executor suspends yet - this is in place *before* the
    // first thing that can.
    exec::InstallSuspendAudit();

    // ---- The fan-out (workplan-crosscore.md P2) -------------------------
    //
    // At `cores = 1` none of this runs: no transport is built, no thread is
    // spawned, and the reactor below is the same single one that has always
    // served. Guideline 2 asks for zero messages and zero allocations on the
    // single-core path, and the cheapest way to mean it is to build nothing.
    std::vector<std::thread> workers;
    if (config_.cores > 1) {
        auto transport = sched::RealRingTransport::Create(
            config_.cores, kCoreRingSlots, kCoreRingPayloadBytes);
        if (!transport.ok()) return transport.status();
        transport_.emplace(std::move(transport.value()));

        scheduler.AttachTransport(&*transport_, /*core_id=*/0);

        // The system core's half of the anchor path (M5): the superblock is
        // page 0 and belongs to core 0, so a peer's completed checkpoint
        // sends its anchor here and this writes it. The write itself goes
        // through the same SuperBlockCheckpointAnchor a local checkpoint
        // uses, so there is exactly one piece of code that knows how an
        // anchor reaches the page.
        if (Status s = scheduler.RegisterMessageHandler(
                sched::RingMessageKind::kAnchorWrite,
                [this](const sched::MessageHeader& header, std::span<const std::byte> payload) {
                    if (payload.size() != sizeof(AnchorWritePayload)) {
                        logger_->Error("checkpoint",
                                       "anchor write from core " +
                                           std::to_string(header.src_core) + " has " +
                                           std::to_string(payload.size()) +
                                           " bytes, not " +
                                           std::to_string(sizeof(AnchorWritePayload)));
                        return;
                    }
                    AnchorWritePayload fields{};
                    std::memcpy(&fields, payload.data(), sizeof(fields));

                    wal::CheckpointAnchorRecord record;
                    record.core_id = fields.core_id;
                    record.checkpoint_lsn = fields.checkpoint_lsn;
                    record.redo_start_lsn = fields.redo_start_lsn;
                    record.durable_lsn = fields.durable_lsn;
                    record.segment_no = fields.segment_no;

                    if (Status s = checkpoint_anchor_->Publish(record); !s.ok()) {
                        // Nowhere to return it: the sender is fire-and-forget
                        // by design, because a lost anchor costs a longer
                        // replay and never an answer (wal.md §8-3).
                        logger_->Error("checkpoint", "publishing core " +
                                                         std::to_string(fields.core_id) +
                                                         "'s anchor failed: " + s.message());
                    }
                });
            !s.ok()) {
            return s;
        }

        // Every peer's page-id lease is carved here, on the startup thread,
        // out of core 0's free map - which is the only writer of it (M5).
        extents_.emplace(store_->free_map_bytes(), kFirstUserPageId);

        for (std::uint32_t core_id = 1; core_id < config_.cores; ++core_id) {
            auto lease = extents_->Reserve(storage::kDefaultExtentPages);
            if (!lease.ok()) return lease.status();

            CoreRuntime::Config core_config;
            core_config.core_id = core_id;
            core_config.wal_dir = config_.wal_dir;
            core_config.checkpoint_interval_ns = config_.checkpoint_interval_ns;
            core_config.wal_drain_interval_ns = config_.wal_drain_interval_ns;
            core_config.inline_cell_width = database_->superblock.inline_cell_width();
            core_config.core_count = database_->superblock.core_count();
            core_config.durability = config_.durability;
            core_config.isolation = config_.isolation;
            core_config.budget = exec::Budget(config_.max_rows_touched);
            core_config.lease = lease.value();

            auto core = CoreRuntime::Open(core_config, *device_, clock_, &*logger_);
            if (!core.ok()) return core.status();
            if (Status s = core.value()->AttachTransport(*transport_); !s.ok()) return s;
            cores_.push_back(std::move(core.value()));
        }

        // The reservations above set free-map bits that only exist in
        // memory until something writes the page. A peer's first allocation
        // must not be an id a restart would think free.
        if (Status s = Sync(); !s.ok()) return s;

        // Core 0's half of the page-id lease service (P5): a peer at its
        // low-water mark asks here, and this carves the next extent. The
        // reservation is synchronous because on this core it is a local
        // call - it is the *asking* that had to wait for coroutines.
        if (Status s = RegisterExtentGrantHandler(scheduler, *transport_, *extents_,
                                                   storage::kDefaultExtentPages, &*logger_);
            !s.ok()) {
            return s;
        }

        // Core 0's DDL choke point, wired to the broadcast. Installed after
        // the peers exist so the loop below always has somebody to tell.
        database_->catalog.SetInvalidationHook([this, &scheduler] {
            BroadcastCatalogInvalidation(scheduler);
        });

        // Spawned only after every core is built, so a failure above leaves
        // no thread to unwind.
        for (auto& core : cores_) {
            workers.emplace_back([&core] { core->Run(); });
            PinToCore(workers.back(), core->core_id(), &*logger_);
        }
        logger_->Info("expeditor", "running " + std::to_string(config_.cores) +
                                       " cores; core 0 serves every statement until the "
                                       "per-core catalog cache exists (workplan P6)");
    }
    if (Status s = listener.value().Attach(scheduler, *dispatcher_, &*logger_); !s.ok()) {
        return s;
    }

    // The `system`-group cadence of wal.md section 11. A failed checkpoint
    // is not fatal and does not disarm the timer: the pages it did not
    // flush stay dirty and the next tick retries them, which is the same
    // recovery the paced Step() path already relies on. Reporting it needs
    // the observability path that does not exist yet - the failure is
    // visible in checkpoint_stats(), where started outruns completed.
    if (config_.checkpoint_interval_ns > 0) {
        scheduler.SubmitEvery(config_.checkpoint_interval_ns, [this] { (void)Checkpoint(); });
        logger_->Info("expeditor", "checkpoint cadence " +
                                       std::to_string(config_.checkpoint_interval_ns / 1'000'000) +
                                       "ms");
    } else {
        logger_->Warn("expeditor",
                      "checkpoint cadence disabled; durability is SYNC and shutdown only");
    }

    // The other `system`-group task of wal.md section 6-2/6-3. It is what
    // bounds a kRelaxed commit's loss window and what resolves a kGroup
    // batch no committer is waiting on; a tick with nothing staged does no
    // I/O, so the interval is chosen for the loss window, not for cost.
    auto drain = [this] {
        if (Status s = wal_->DrainOnce(); !s.ok()) {
            // Same shape as the checkpoint timer: no caller to return
            // to, so the log is the only place this becomes visible.
            logger_->Error("wal", "drain failed: " + s.message());
        }
    };

    // **The group committer.** Once per reactor iteration, after every
    // runnable statement has staged whatever it is going to stage, so one
    // device sync covers all of them. A committing statement parks instead
    // of syncing on its own stack (command_dispatcher.hpp's `pending_lsn`),
    // and this is what it parks *for*: without it the parked statement has
    // nothing to wake it until the timer below fires, and with the timer
    // alone every commit would pay that interval.
    scheduler.SetPostTaskHook(drain);

    if (config_.wal_drain_interval_ns > 0) {
        scheduler.SubmitEvery(config_.wal_drain_interval_ns, drain);
    } else {
        logger_->Warn("wal", "drain cadence disabled; relaxed commits stay unsynced "
                             "until checkpoint or shutdown");
    }

    logger_->Info("expeditor", "listening on 127.0.0.1:" + std::to_string(config_.port));
    scheduler.Run();
    logger_->Info("expeditor", "stopping");

    // Every peer is told to stop, then joined. The message is how a core is
    // stopped at all - `Scheduler::Stop()` writes a plain bool owned by that
    // core's own thread (ring_message.hpp's kShutdown says why).
    BroadcastShutdown(scheduler);
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    // After the join, so nothing is still appending to a stream being
    // synced. Each core's log is its own, so this is N independent syncs
    // and not a barrier.
    for (auto& core : cores_) {
        if (Status s = core->Sync(); !s.ok()) {
            logger_->Error("expeditor", "core " + std::to_string(core->core_id()) +
                                            ": final log sync failed: " + s.message());
        }
    }
    cores_.clear();
    transport_.reset();

    // Torn down before the scheduler leaves scope: both hold fds
    // registered with it.
    listener.value().Detach();

    Status s = Sync();
    if (!s.ok()) {
        logger_->Error("expeditor", "final sync failed: " + s.message());
    } else {
        logger_->Info("expeditor", "stopped cleanly; " +
                                       std::to_string(store_->allocated_pages()) +
                                       " pages persisted");
    }
    return s;
}

}  // namespace kds::server
