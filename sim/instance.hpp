#pragma once

// sim/instance.hpp — one simulated database instance (bench/workplan-
// teststrategy SIM01/SIM04).
//
// Owns the whole engine stack over crashable in-memory devices:
//
//     MemoryPageDevice + MemoryLogDevice        (survive a crash)
//     WalManager / DevicePageStore / Bootstrap  (rebuilt per boot)
//     TrxIdSequence / UndoLog / TransactionManager
//     CommandDispatcher + one Session
//
// The devices outlive the engine: Crash() drops everything either device
// holds that was never synced — the semantics memory_page_device_test.cpp
// and memory_log_device_test.cpp pin — and Reboot() brings a fresh engine
// stack up over the surviving image, taking bootstrap's *existing* path.
// Statements go through CommandDispatcher::Dispatch, the same front door
// every client uses, so the parser, compiler, step VM and write paths are
// all inside the tested surface.
//
// Deliberately not here: no randomness (the workload owns that), no
// verdicts (the loop owns those). This file only makes "an instance you
// can kill" a value.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "kds/base/status.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"

namespace kds::sim {

// At namespace scope rather than nested: a nested aggregate's default member
// initializers are not usable in the enclosing class's own default arguments
// (the complete-class-context rule), and Create() wants `= {}`.
struct SimInstanceOptions {
    // 1 MiB segments: big enough that a short run never rolls over, small
    // enough that one that does is still exercised.
    std::uint64_t wal_segment_bytes = 1ull << 20;
    std::uint32_t extent_pages = 64;
    std::uint32_t initial_pages = 64;
    wal::DurabilityClass durability = wal::DurabilityClass::kGroup;
};

class SimInstance {
public:
    using Options = SimInstanceOptions;

    // Fresh devices, fresh database (bootstrap's fresh path).
    static StatusOr<std::unique_ptr<SimInstance>> Create(Options options = {});

    // Power-cut: both devices revert to their last-synced image and the
    // engine stack is torn down. No destructor flushes anything on the way
    // out (verified: every component's destructor is defaulted), so what
    // survives is exactly what a real crash would leave.
    void Crash();

    // Log sync + store sync through the dispatcher's own SYNC — the same
    // path a client's SYNC takes — then tear the stack down without a
    // crash. What the devices hold afterwards is the clean-shutdown image.
    Status CleanShutdown();

    // Brings the engine back up over whatever the devices hold. Legal after
    // Crash() or CleanShutdown(); a defect if the engine is still up.
    Status Reboot();

    bool running() const { return dispatcher_.has_value(); }

    // One statement through the front door, on this instance's session.
    // Never fails outward — errors come back as "ERR ..." replies, exactly
    // as a client sees them.
    std::string Execute(std::string_view sql);

    server::CommandDispatcher& dispatcher() { return *dispatcher_; }
    server::Session& session() { return session_; }
    storage::DevicePageStore& store() { return *store_; }
    catalog::Catalog& catalog() { return boot_->catalog; }
    server::SuperBlock& superblock() { return boot_->superblock; }
    storage::MemoryPageDevice& page_device() { return *page_device_; }
    wal::MemoryLogDevice& log_device() { return *log_device_; }

private:
    SimInstance() = default;

    // Engine bring-up over the current device contents; shared by Create()
    // and Reboot().
    Status Boot();

    // The TrxIdSequence persist callback (Expeditor::PersistSuperBlock's
    // shape): encode the superblock into page 0 and sync the store.
    Status PersistSuperBlock();

    // Reverse construction order, no I/O.
    void TearDown();

    Options options_{};
    sched::ManualClock clock_;

    // Device layer — survives crash and reboot.
    std::unique_ptr<storage::MemoryPageDevice> page_device_;
    std::unique_ptr<wal::MemoryLogDevice> log_device_;

    // Engine stack — rebuilt per boot.
    std::unique_ptr<wal::WalManager> wal_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> trx_ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> txn_;
    std::optional<server::CommandDispatcher> dispatcher_;
    server::Session session_;
};

}  // namespace kds::sim
