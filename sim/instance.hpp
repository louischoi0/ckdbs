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
#include "kds/server/page_reclaim.hpp"
#include "kds/server/superblock_checkpoint_anchor.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/mount_recovery.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
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
    // 64 KiB segments (BC-S4): small enough that every run rolls the log
    // many times and its checkpoints recycle what lies below their anchor,
    // and still eight times the largest record (a FULL_PAGE_IMAGE).
    std::uint64_t wal_segment_bytes = 64ull * 1024;
    std::uint32_t extent_pages = 64;
    std::uint32_t initial_pages = 64;
    // The buffer pool's maximum (BE-Q2): the harness runs under a real cap,
    // chosen per seed (`loop.cpp`'s `InstanceOptions`), so eviction runs on
    // every seed rather than in a mode no server has.
    std::size_t buffer_pool_frames = 1024;
    wal::DurabilityClass durability = wal::DurabilityClass::kGroup;

    // **A fault injection, and the only reason it exists**: boot without the
    // recovery phase a real mount runs (server/mount_recovery.hpp). It is
    // how the crash contract's assertion is proved to *fire* — with recovery
    // wired in, no seed loses an acknowledged row any more, so the gate
    // could otherwise only be shown to pass, and a gate that cannot fail is
    // not a gate (docs/workplan-wal-recovery.md RC10).
    //
    // Never true in a production shape, and never a "recovery off" mode: an
    // instance booted this way has a loser's writes on its pages, which
    // txn.md §8's gap then reads as committed.
    bool skip_recovery = false;

    // **The advisory features, per instance** (SIM06). Off here and on in
    // the server's defaults, because the harness's interest in them is the
    // invariant, not the feature: toggling any of the three may never
    // change a result (invariant 8 for Waystone; the Cabin and the access
    // statistics carry the same promise in their own specs). The oracle
    // does not know they exist, and the loop's paired run puts two
    // instances that differ only in these three side by side.
    bool waystone = false;           // trail recording *and* replay
    bool cabins = false;             // the value-observed store
    bool access_statistics = true;   // sys.access_stats, the server default
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

    // H2: `Crash()` with the **log** cut inside its unsynced run rather
    // than at the last `Sync()`, keeping its first `keep_bytes`. The page
    // device still takes the whole crash - a torn page is unhealable until
    // the full-page-image cadence exists, so cutting it would only produce
    // a mount refusal that says nothing about recovery's logic
    // (`sim/faults.hpp` draws the same line).
    //
    // What this reaches that `Crash()` cannot: a log whose readable prefix
    // ends part-way through one statement's records. Use
    // `UnsyncedLogBytes()` to choose a cut inside the run.
    void CrashWithLogPrefix(std::uint64_t keep_bytes);

    // How many log bytes are unsynced right now - the range a
    // `CrashWithLogPrefix` cut is meaningful in.
    std::uint64_t UnsyncedLogBytes() const noexcept;

    // Log sync + store sync through the dispatcher's own SYNC — the same
    // path a client's SYNC takes — then a checkpoint, then tear the stack down
    // without a crash. What the devices hold afterwards is the clean-shutdown
    // image.
    //
    // The checkpoint is what `Expeditor::Serve` does on its way out, and for its
    // reason: without one the anchor stays wherever the last tick left it, so the
    // *next* mount re-reads every record this run wrote and redoes none of them.
    Status CleanShutdown();

    // One checkpoint to completion, publishing the superblock anchor. Three
    // callers, the server's three: the tail of a mount (RC08), a clean
    // shutdown, and the cadence tick mid-run (BC-S4), after which the log is
    // recycled below the anchor and the crash that follows is recovered from
    // a log whose head is gone.
    Status RunCheckpoint();

    // Recycling's counts over the instance's life (BC-S4): advances of the
    // durable redo start the recycler was handed, and segments the log
    // device removed. Both survive a reboot; the device does.
    std::uint64_t recycles() const noexcept { return recycles_; }
    std::uint64_t segments_recycled() const noexcept { return log_device_->segments_removed(); }
    // Detaches the recycler's detach-only arm had refused.
    std::uint64_t detach_refusals() const noexcept { return detach_refusals_; }

    // The log stopped after a failed device write (`wal/stream.hpp`'s
    // fail-stop): the instance refuses every write until it is restarted.
    bool log_stopped() const noexcept { return wal_ != nullptr && wal_->stream()->stopped(); }

    // Crashes that landed between a detach and its reclaim, and so brought
    // detached segments back for the next mount to skip.
    std::uint64_t crashes_reviving_segments() const noexcept { return crashes_reviving_segments_; }

    // Brings the engine back up over whatever the devices hold. Legal after
    // Crash() or CleanShutdown(); a defect if the engine is still up.
    Status Reboot();

    bool running() const { return dispatcher_.has_value(); }

    // What the last Boot()'s recovery did. Zeroed on the first boot of fresh
    // devices, which is honest: there was nothing to recover. Exposed because
    // the harness is where "a mount after a clean stop reads almost nothing" is
    // checkable at all.
    const server::MountRecovery& recovery() const noexcept { return recovery_; }

    // One statement through the front door, on this instance's session.
    // Never fails outward — errors come back as "ERR ..." replies, exactly
    // as a client sees them.
    std::string Execute(std::string_view sql);

    // **DROP TABLE page reclamation, as a mount runs it** (BF-R8): the
    // tombstones this boot found pending, freed before the harness reads.
    // Null after a boot that skipped recovery.
    server::PageReclaimer* reclaimer() { return reclaimer_ ? &*reclaimer_ : nullptr; }
    // One bounded step, as core 0's tick runs it (`Expeditor::ReclaimStep`);
    // and every job driven to its end, which the harness owes a reboot
    // before it reads the volume.
    Status ReclaimStep();
    Status SettleReclaim();
    const server::ReclaimCounters& reclaim_counters() const noexcept { return reclaim_counters_; }

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

    // The recycler `RunCheckpoint`'s anchor is handed (BC-R5's inline arm:
    // the harness runs no writer thread). **Three advances in four detach
    // without reclaiming**, so the detached segments are still on the
    // device - their directory sync has not run - and a crash before the
    // next advance brings them back (`MemoryLogDevice::Crash`), which is the
    // window between an unlink and its directory sync.
    void Recycle(wal::Lsn durable_redo_start);

    Options options_{};
    sched::ManualClock clock_;
    std::uint64_t recycles_ = 0;
    std::uint64_t crashes_reviving_segments_ = 0;
    std::uint64_t detach_refusals_ = 0;

    // Device layer — survives crash and reboot.
    std::unique_ptr<storage::MemoryPageDevice> page_device_;
    std::unique_ptr<wal::MemoryLogDevice> log_device_;

    // Engine stack — rebuilt per boot. The recorder and the Cabin store
    // hold references into the catalog and the store, so they are rebuilt
    // with them; a Cabin's entry sets are memory-resident by design
    // (docs/spec/cabin.md), so a reboot forgets them, exactly as a server
    // restart does.
    std::unique_ptr<wal::WalManager> wal_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> trx_ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> txn_;
    std::optional<stats::TrailRecorder> trail_recorder_;
    std::optional<stats::CabinStore> cabin_store_;
    std::optional<server::CommandDispatcher> dispatcher_;
    server::Session session_;
    server::MountRecovery recovery_;
    // **One anchor object per boot** (BF-R12), as `Expeditor` holds one:
    // its durable redo start is the `D` a reclaim is gated on, and a fresh
    // object per checkpoint started it over at 0.
    std::optional<server::SuperBlockCheckpointAnchor> anchor_;
    server::ReclaimCounters reclaim_counters_;
    std::optional<server::PageReclaimer> reclaimer_;
    wal::Lsn reclaim_gate_ = 0;
};

}  // namespace kds::sim
