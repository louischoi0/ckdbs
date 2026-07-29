#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "kds/base/status.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/tcp_server.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/page_device.hpp"

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
// and prints, and does nothing else (rules.md #4). Subsystems that do not
// exist yet - the WAL, the thread-per-core worker pool - attach here when
// they land, which is the point of having a single owner.

namespace kds::server {

class Expeditor {
public:
    struct Config {
        std::string data_file;
        std::uint16_t port = 15432;
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

    // Writes everything back to stable storage. Until the WAL lands
    // (docs/wal.md), this and Serve()'s shutdown are what make a mutation
    // survive the process dying - which is also why SYNC is a client
    // command (command_dispatcher.hpp).
    Status Sync() { return store_->Sync(); }

    const SuperBlock& superblock() const noexcept { return database_->superblock; }
    catalog::Catalog& catalog() noexcept { return database_->catalog; }
    storage::DevicePageStore& store() noexcept { return *store_; }
    CommandDispatcher& dispatcher() noexcept { return *dispatcher_; }
    const Config& config() const noexcept { return config_; }

private:
    Expeditor(Config config, std::unique_ptr<storage::PageDevice> device,
              std::unique_ptr<storage::DevicePageStore> store) noexcept;

    Config config_;
    std::unique_ptr<storage::PageDevice> device_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::optional<bootstrap::BootstrapResult> database_;
    std::optional<CommandDispatcher> dispatcher_;
};

}  // namespace kds::server
