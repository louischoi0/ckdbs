#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/config_file.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/superblock_checkpoint_anchor.hpp"
#include "kds/server/tcp_server.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/page_device.hpp"
#include "kds/storage/page_store_checkpoint_target.hpp"
#include "kds/wal/checkpointer.hpp"
#include "kds/wal/file_log_device.hpp"
#include "kds/wal/manager.hpp"

// The Expeditor: the KDS super process. One instance owns every subsystem
// of a running database - the page device, the page store above it, the
// superblock and catalog produced by bootstrap, the command dispatcher,
// and the listener - and owns their lifetime and start/stop order:
//
//   device -> store -> bootstrap (superblock + catalog) -> dispatcher
//   -> listener,  torn down in reverse, with a final Sync().
//
// Everything above holds references down the stack (Catalog into the
// store, CommandDispatcher into all three), so an Expeditor is pinned:
// non-copyable, non-movable, handed out as a unique_ptr by Open(). Moving
// one would leave those references pointing at a corpse.
//
// main() is the platform layer around this: it reads argv and the clock
// and prints, and does nothing else (rules.md #4).
//
// ---- The reactor, and why durability stopped depending on STOP ----------
//
// Serve() runs a `sched::Scheduler` (sched.md section 2), not a loop of its
// own. TcpServer attaches its sockets to it, and a `system`-group timer
// fires the checkpointer every `checkpoint_interval_ns`. Before this, the
// process sat blocked in accept()/read() for its whole life, so the only
// moments anything reached the platter were an explicit SYNC and the final
// Sync() after STOP - a server nobody typed SYNC into held every mutation
// in memory indefinitely.
//
// The checkpoint is what closes that: it flushes the store's dirty pages
// on a cadence (wal.md section 11-2) through PageStoreCheckpointTarget.
// Note what it is *not* yet - the WAL gate is not on this store's flush
// path, and nothing logs page mutations, so the checkpoint's redo start is
// not yet a recovery guarantee (see page_store_checkpoint_target.hpp). It
// bounds the loss window; it does not survive a crash mid-write.

namespace kds::server {

class Expeditor {
public:
    struct Config {
        std::string data_file = "kds.db";
        std::uint16_t port = 15432;

        // Directory the per-core WAL segment files live in. Defaults to
        // `<data_file>.wal` when left empty.
        std::string wal_dir;

        // How often the `system`-group checkpoint task runs. Checkpoint
        // cadence is the RTO knob and an [OPEN] decision (wal.md sections
        // 11 and 13), so this is a parameter with a default, never a
        // constant anything may depend on. 0 disables the timer entirely,
        // which puts durability back on SYNC and STOP alone.
        sched::MonoTimeNs checkpoint_interval_ns = 5'000'000'000;  // 5 s

        // Diagnostic log (base/log.hpp). `log_dir` empty means "next to
        // wherever the process runs"; the two are joined into one path, so
        // an absolute `log_file` is honoured on its own.
        std::string log_dir;
        std::string log_file = "kdb.log";
        LogLevel log_level = LogLevel::kInfo;

        // Resolved destination: log_dir joined with log_file. Empty only
        // when log_file is empty, which means "do not log to a file".
        std::string LogPath() const;

        // Every key a config file may set (config_file.hpp). Kept next to
        // the fields it fills so adding a field and forgetting the key is
        // one edit away from being noticed.
        static std::vector<std::string> KnownConfigKeys();

        // Overlays `file` onto this config. Keys absent from the file leave
        // the current value alone, which is what makes the precedence chain
        // (defaults, then file, then command line) a matter of call order.
        //
        // Fails on an unknown key, an unparseable value, or a value out of
        // range - a config file that is quietly half-applied is worse than
        // one that refuses to start.
        Status ApplyFile(const ConfigFile& file);
    };

    // Opens (creating if absent) the data file, brings a database up on it,
    // and persists the result before returning, so a fresh database that
    // dies before its first client cannot come back looking fresh again.
    // Does not bind the port - Serve() does that.
    static StatusOr<std::unique_ptr<Expeditor>> Open(Config config,
                                                     std::uint64_t now_unix_seconds);

    Expeditor(const Expeditor&) = delete;
    Expeditor& operator=(const Expeditor&) = delete;

    // Binds the port and serves clients until a STOP command, then syncs.
    Status Serve();

    // Writes everything back to stable storage. Still what SYNC and the
    // shutdown path call; no longer the *only* thing that persists, now
    // that the checkpoint timer runs (see the file comment).
    Status Sync() { return store_->Sync(); }

    // Runs one checkpoint to completion: snapshot the dirty table, flush
    // it, log CHECKPOINT_END, publish the superblock anchor. This is what
    // the interval timer calls, exposed so a test can drive the cadence
    // deterministically instead of waiting on wall time.
    Status Checkpoint();

    const SuperBlock& superblock() const noexcept { return database_->superblock; }
    catalog::Catalog& catalog() noexcept { return database_->catalog; }
    storage::DevicePageStore& store() noexcept { return *store_; }
    CommandDispatcher& dispatcher() noexcept { return *dispatcher_; }
    const Config& config() const noexcept { return config_; }
    const wal::CheckpointStats& checkpoint_stats() const noexcept {
        return checkpointer_->stats();
    }
    wal::WalManager& wal() noexcept { return *wal_; }

    // The server's log. Always non-null: with no file configured it is a
    // Logger over a null sink, so call sites never test for one.
    Logger& log() noexcept { return *logger_; }

private:
    Expeditor(Config config, std::unique_ptr<storage::PageDevice> device,
              std::unique_ptr<storage::DevicePageStore> store) noexcept;

    Status OpenLog();

    Config config_;
    sched::SystemClock clock_;
    SystemWallClock wall_clock_;

    // Declared before everything it might report on, and destroyed after
    // it - so a subsystem's teardown can still log.
    std::unique_ptr<FileLogSink> log_sink_;
    std::optional<Logger> logger_;

    // Declared in construction order, and torn down in reverse: the
    // checkpointer holds references into the WAL manager, the target, and
    // the anchor; the anchor holds one into the superblock and the store.
    std::unique_ptr<storage::PageDevice> device_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::optional<bootstrap::BootstrapResult> database_;
    std::optional<CommandDispatcher> dispatcher_;

    std::unique_ptr<wal::FileLogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;
    std::optional<storage::PageStoreCheckpointTarget> checkpoint_target_;
    std::optional<SuperBlockCheckpointAnchor> checkpoint_anchor_;
    wal::NoActiveTransactions no_txns_;
    std::optional<wal::Checkpointer> checkpointer_;
};

}  // namespace kds::server
