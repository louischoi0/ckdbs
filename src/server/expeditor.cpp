#include "kds/server/expeditor.hpp"

#include <limits>
#include <utility>

#include "kds/sched/epoll_io_backend.hpp"
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
            "wal_drain_interval_us", "log_dir",  "log_file",               "log_level",
            "max_rows_touched",      "inline_cell_width",      "waystone_recording",
            "waystone_replay"};
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
    if (file.Has("waystone_replay")) {
        auto v = file.GetBool("waystone_replay");
        if (!v.ok()) return v.status();
        waystone_replay = v.value();
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
                                     config.inline_cell_width, &*expeditor->logger_);
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

    auto wal = wal::WalManager::Open(expeditor->log_device_.get(), expeditor->clock_,
                                     /*core_id=*/0);
    if (!wal.ok()) return wal.status();
    expeditor->wal_ = std::move(wal.value());
    expeditor->wal_->SetLogger(&*expeditor->logger_);

    // WAL-before-data, enforced by the store rather than asked of its
    // callers (device_page_store.hpp): from here on no dirty page reaches
    // the device ahead of the records describing it.
    expeditor->store_->SetWalGate(expeditor->wal_.get());

    if (expeditor->config_.waystone_recording) {
        expeditor->trail_recorder_.emplace(expeditor->database_->catalog, *expeditor->store_,
                                           &expeditor->clock_);
    }
    expeditor->dispatcher_.emplace(
        expeditor->database_->superblock, expeditor->database_->catalog, *expeditor->store_,
        &*expeditor->logger_, &expeditor->clock_, &*expeditor->wal_,
        expeditor->config_.durability, exec::Budget(expeditor->config_.max_rows_touched),
        expeditor->trail_recorder_ ? &*expeditor->trail_recorder_ : nullptr,
        expeditor->config_.waystone_replay);
    expeditor->logger_->Info("expeditor",
                             std::string("INSERT durability ") +
                                 wal::DurabilityClassName(expeditor->config_.durability));

    expeditor->checkpoint_target_.emplace(*expeditor->store_);
    expeditor->checkpoint_anchor_.emplace(expeditor->database_->superblock, *expeditor->store_);
    expeditor->checkpoint_anchor_->SetLogger(&*expeditor->logger_);
    expeditor->checkpointer_.emplace(*expeditor->wal_, *expeditor->checkpoint_target_,
                                     expeditor->no_txns_, *expeditor->checkpoint_anchor_);
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

Status Expeditor::Serve() {
    auto io_backend = sched::EpollIoBackend::Create();
    if (!io_backend.ok()) return io_backend.status();

    auto listener = TcpServer::Listen(config_.port);
    if (!listener.ok()) return listener.status();

    sched::Scheduler scheduler(clock_, io_backend.value());
    scheduler.SetLogger(&*logger_);
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
    if (config_.wal_drain_interval_ns > 0) {
        scheduler.SubmitEvery(config_.wal_drain_interval_ns, [this] {
            if (Status s = wal_->DrainOnce(); !s.ok()) {
                // Same shape as the checkpoint timer: no caller to return
                // to, so the log is the only place this becomes visible.
                logger_->Error("wal", "drain failed: " + s.message());
            }
        });
    } else {
        logger_->Warn("wal", "drain cadence disabled; relaxed commits stay unsynced "
                             "until checkpoint or shutdown");
    }

    logger_->Info("expeditor", "listening on 127.0.0.1:" + std::to_string(config_.port));
    scheduler.Run();
    logger_->Info("expeditor", "stopping");

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
