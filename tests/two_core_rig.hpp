#pragma once

// AV-S1: the deterministic two-core rig
// (`instructions/v3.0.0/workorder-av-two-core-rig.md`).
//
// Two `CoreRuntime`s over **one** `DevicePageStore`, **one** WAL stream,
// **one** `InstanceVisibility` and **one** `WakerTable` - the arrangement
// `Expeditor::Open`/`Start` wires at `cores = 2`, and since AM-S2 step 3 the
// only arrangement a mountable volume has (AV-R2). Both reactors run on
// their own threads through `CoreRuntime::Run()`, production's own body,
// and the one cross-core primitive between them is `WakerTable::Kick`,
// reached through a `SimWakerTable` that logs every kick and holds it until
// the cell advances the tick (AV-R1's mark: the seam is the table).
//
// **What is deterministic, and what is not** (AV-R3): the delay each kick
// draws is a pure function of `(seed, dst, tick)`, so what a kick costs in
// ticks reproduces from the seed; which kicks happen, and at which tick, is
// the two threads' interleaving, which nothing reproduces - the peer's own
// anchor send is a kick the cell never asked for. A cell that needs a state
// reached first reaches it by a barrier - an atomic the task flips, or the
// scheduler's cross-thread-safe `idle_blocks()` - and reads the kick log
// per destination.
//
// **Core 0 is a `CoreRuntime` here and an `Expeditor` in production.** The
// AV-S0 read (the order's §8 table) lists what that leaves out; what it
// leaves in is everything `CoreRuntime::AttachTransport` wires for every
// core - shipping, 2PC, the foreign-key probe, the remote step server - so a
// statement crossing the rig crosses production's code. The two things a
// core-0 runtime lacked and this rig needed are landed in the runtime
// rather than worked around here: it persists page 0 when it carves an id
// block, and its reactor takes a `SchedulerConfig`.
//
// **Nothing here depends on machinery AW-S1b deleted** (AV-R4): no lease,
// no fault grant, no claim by stamp. Two runtimes over a shared store need
// none of it.
//
// ---- Threading -------------------------------------------------------------
//
// The rig's own thread builds everything, then `Start()` hands each runtime
// to its worker and touches it no more, but for the two things
// `core_runtime.hpp` names as safe from another thread - `Stop()` and a
// kick - which is how `Stop()` here ends both reactors. A cell that wants a
// task parked on a reactor submits it **before** `Start()`; after it, a
// task reaches a reactor only the way production's do, over the ring.

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <unistd.h>

#include "kds/base/status.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/send_retry.hpp"
#include "kds/sched/sim_waker_table.hpp"
#include "kds/server/core_runtime.hpp"
#include "kds/server/row_id_lease_service.hpp"
#include "kds/server/superblock.hpp"
#include "kds/server/trx_id_lease_service.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/txn/instance_visibility.hpp"
#include "kds/wal/file_log_device.hpp"
#include "kds/wal/manager.hpp"

namespace kds::server {

class TwoCoreRig {
public:
    struct Options {
        // The kick's delay and seed (`sim_waker_table.hpp`). Zero delay is
        // the production path with a log line in front of it.
        sched::SimWakerConfig wake;
        // Both reactors' idle block. Long by default, so a reactor that
        // times out instead of waking fails a cell rather than passing it
        // slowly; a cell about block expiry sets it short.
        int max_idle_block_ms = 5000;
        // The peer's `system` tick (lease refills, access-stat flushes).
        // Off by default so an idle reactor's block is the whole block;
        // a cell that wants production's refill path over the ring turns
        // it on.
        sched::MonoTimeNs wal_drain_interval_ns = 0;
        // Where core 0's DDL places a relation. Rotate at two cores puts
        // every relation on core 1, which is what a cross-owner cell wants.
        catalog::PlacementPolicy placement = catalog::PlacementPolicy::kRotate;
    };

    static StatusOr<std::unique_ptr<TwoCoreRig>> Open() { return Open(Options{}); }
    static StatusOr<std::unique_ptr<TwoCoreRig>> Open(Options options) {
        auto rig = std::unique_ptr<TwoCoreRig>(new TwoCoreRig(options));
        if (Status s = rig->Build(); !s.ok()) return s;
        return rig;
    }

    TwoCoreRig(const TwoCoreRig&) = delete;
    TwoCoreRig& operator=(const TwoCoreRig&) = delete;

    ~TwoCoreRig() {
        Stop();
        // The runtimes first, each as its own core (`~CoreRuntime` takes the
        // guard), and the peer before core 0 for the reason `Expeditor`
        // drops peers ahead of its own stack: a peer borrows the stream,
        // the pool and the visibility that core 0's side of this rig owns.
        cores_[1].reset();
        cores_[0].reset();
        // The log closes before its directory goes - the writer thread joins
        // in `~WalManager` - and the rest follows in reverse declaration
        // order: the sim table, the real table, the transport, the
        // visibility, the store, the device.
        wal_.reset();
        log_device_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    CoreRuntime& core(std::uint32_t id) noexcept { return *cores_[id]; }
    storage::DevicePageStore& store() noexcept { return *store_; }
    sched::SimWakerTable& wake() noexcept { return *sim_; }
    // The real table under the sim: what was actually written, and the
    // registry a teardown kicks through.
    sched::WakerTable& wakers() noexcept { return *wakers_; }
    sched::RealRingTransport& transport() noexcept { return *transport_; }

    // Funds the peer with a row-id block for `oid` (`row_id_lease.hpp`), the
    // way core 0's grant handler would over the ring on the peer's tick -
    // which this rig leaves off by default. Rig thread, before `Start()`.
    Status FundPeerRelation(catalog::Oid oid, std::uint64_t count = 16) {
        auto first = core(0).catalog().AllocateRowIdRange(oid, count);
        if (!first.ok()) return first.status();
        core(1).row_id_leases().Grant(oid, first.value(), count);
        return Status::OK();
    }

    // Both reactors on their own threads, production's `Run()`.
    void Start() {
        if (started_) return;
        started_ = true;
        for (std::uint32_t id = 0; id < 2; ++id) {
            threads_[id] = std::thread([core = cores_[id].get()] { core->Run(); });
        }
    }

    // Flag, then kick - through the **real** table, so a teardown never
    // waits on the cell to advance the sim's tick - then join. `Expeditor::
    // BroadcastShutdown`'s shape, and the same two calls `core_runtime.hpp`
    // admits from another thread.
    void Stop() {
        if (!started_) return;
        for (std::uint32_t id = 0; id < 2; ++id) {
            cores_[id]->scheduler().Stop();
            wakers_->Kick(id);
        }
        for (auto& thread : threads_) {
            if (thread.joinable()) thread.join();
        }
        started_ = false;
    }

private:
    explicit TwoCoreRig(Options options) : options_(options) {}

    Status Build() {
        static std::atomic<int> counter{0};
        dir_ = std::filesystem::temp_directory_path() /
               ("kds_two_core_rig_" + std::to_string(::getpid()) + "_" +
                std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(dir_);

        // ---- The volume: `Expeditor::Open`'s stack, over memory --------
        auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/64);
        if (!device.ok()) return device.status();
        device_ = std::move(device.value());
        auto store = storage::DevicePageStore::Open(*device_, kFirstUserPageId);
        if (!store.ok()) return store.status();
        store_ = std::move(store.value());
        // EV3's floor, `Expeditor::Open`'s one install site.
        store_->SetResidentLimit(kFirstUserPageId);

        auto boot = bootstrap::BootstrapDatabase(*store_, /*now_unix_seconds=*/1000,
                                                 storage::kDefaultInlineCellWidth,
                                                 /*cores=*/2, /*log=*/nullptr);
        if (!boot.ok()) return boot.status();
        boot_.emplace(std::move(boot.value()));
        if (Status s = store_->Sync(); !s.ok()) return s;

        // The instance's one stream, its writer, and the gate (AR0 M0).
        auto log_device = wal::FileLogDevice::Open(dir_.string(), /*core_id=*/0);
        if (!log_device.ok()) return log_device.status();
        log_device_ = std::move(log_device.value());
        wal::WalManagerConfig wal_config;
        wal_config.shared_stream = true;
        auto wal = wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/0, wal_config);
        if (!wal.ok()) return wal.status();
        wal_ = std::move(wal.value());
        wal_->StartWriter();
        store_->SetWalGate(wal_.get());
        // The page latch and the pinner count, from the core count (AM-S1,
        // AM-S2): the store is shared, so this is the one arming site.
        store_->SetLatchArmed(true, /*concurrent_pinners=*/2);
        // The instance's visibility state (AN-R1), one for both cores.
        visibility_.emplace();

        // ---- The fan-out: `Expeditor::Start`'s, minus the listeners -----
        auto transport = sched::RealRingTransport::Create(/*core_count=*/2, sched::kCoreRingSlots,
                                                          sched::kCoreRingPayloadBytes);
        if (!transport.ok()) return transport.status();
        transport_.emplace(std::move(transport.value()));
        wakers_.emplace(/*core_count=*/2);
        sim_.emplace(*wakers_, options_.wake);
        // The transport kicks through the sim, so a send's wake is logged
        // and held like any other (AU-S1b: one registry, and this is it).
        transport_->AttachWakers(&*sim_);

        for (std::uint32_t id = 0; id < 2; ++id) {
            CoreRuntime::Config config;
            config.core_id = id;
            config.superblock = &boot_->superblock;
            config.core_count = boot_->superblock.core_count();
            config.inline_cell_width = boot_->superblock.inline_cell_width();
            config.anchor = boot_->superblock.wal_anchor(0);
            config.shared_store = store_.get();
            config.shared_stream = wal_->stream();
            config.shared_writer = wal_->writer();
            config.visibility = &*visibility_;
            config.wal_drain_interval_ns = options_.wal_drain_interval_ns;
            config.scheduler.max_idle_block_ms = options_.max_idle_block_ms;
            auto core = CoreRuntime::Open(config, *device_, clock_, /*log=*/nullptr);
            if (!core.ok()) return core.status();
            cores_[id] = std::move(core.value());
            // Every core's services, production's own wiring; then the
            // registry, through the sim, so this reactor can be kicked and
            // the kick is logged.
            if (Status s = cores_[id]->AttachTransport(*transport_); !s.ok()) return s;
            if (Status s = cores_[id]->scheduler().AttachWakerTable(&*sim_, id); !s.ok()) {
                return s;
            }
        }

        // ---- Core 0's half that `Expeditor` wires and a runtime does not ----
        CoreRuntime& core0 = *cores_[0];
        // The peer's completion checkpoint publishes its anchor here
        // (`remote_checkpoint_anchor.hpp`); a rig never remounts, so it is
        // acknowledged by dropping it rather than by writing page 0.
        if (Status s = core0.scheduler().RegisterMessageHandler(
                sched::RingMessageKind::kAnchorWrite,
                [](const sched::MessageHeader&, std::span<const std::byte>) {});
            !s.ok()) {
            return s;
        }
        // The two lease services' grant sides, over core 0's own sequence
        // and catalog - production's handlers, so a peer whose tick asks is
        // answered the way an instance answers it.
        if (Status s = RegisterTrxIdGrantHandler(core0.scheduler(), *transport_,
                                                 core0.trx_ids());
            !s.ok()) {
            return s;
        }
        if (Status s = RegisterRowIdGrantHandler(
                core0.scheduler(), *transport_, core0.catalog(), /*log=*/nullptr,
                store_.get(), &core0.wal(), &core0.dispatcher().assertions(), core0.cabins(),
                &core0.dispatcher().cabin_split_discards());
            !s.ok()) {
            return s;
        }
        // Placement, then the DDL choke point: flush the unlogged catalog
        // pages, then tell the peer to re-read them - `Expeditor::
        // BroadcastCatalogInvalidation`'s order, on core 0's own reactor,
        // which is the thread every DDL runs on.
        core0.catalog().SetPlacementPolicy(options_.placement);
        core0.catalog().SetInvalidationHook([this, &core0] {
            if (!store_->FlushPages(catalog::kEveryCatalogPage).ok()) return;
            sched::MessageHeader header{};
            header.src_core = 0;
            header.dst_core = 1;
            header.session_core = 0;
            header.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kCatalogInvalidate);
            header.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
            core0.scheduler().Submit(sched::MakeSendRetryTask(*transport_, header, {}));
        });
        // The peer's first transaction-id block, carved from the one
        // sequence - which persists page 0 through core 0's runtime - so
        // the peer can write before its tick has asked for anything.
        auto block = core0.trx_ids().Carve(kTrxIdLeasePerGrant);
        if (!block.ok()) return block.status();
        cores_[1]->trx_id_lease().Grant(block.value().first, block.value().count);
        return Status::OK();
    }

    Options options_;
    std::filesystem::path dir_;
    sched::SystemClock clock_;
    std::unique_ptr<storage::MemoryPageDevice> device_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::unique_ptr<wal::FileLogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;
    std::optional<txn::InstanceVisibility> visibility_;
    std::optional<sched::RealRingTransport> transport_;
    std::optional<sched::WakerTable> wakers_;
    std::optional<sched::SimWakerTable> sim_;
    std::array<std::thread, 2> threads_;
    // Last, so they die first: every runtime borrows everything above.
    std::array<std::unique_ptr<CoreRuntime>, 2> cores_;
    bool started_ = false;
};

}  // namespace kds::server
