#include "kds/server/core_runtime.hpp"

#include "kds/storage/anchor_page.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <cstring>
#include <map>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/assertion_catalog.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/index_ddl.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/assertion_build_service.hpp"
#include "kds/server/index_build_service.hpp"
#include "kds/server/relation_grant_service.hpp"
#include "kds/server/remote_step_service.hpp"
#include "kds/server/superblock_checkpoint_anchor.hpp"
// CB4: the rig arms core 0's *owner* half of statement shipping, which
// nothing needed until a peer's DDL was routed there rather than refused.
#include "kds/server/shipped_statement_executor.hpp"
#include "kds/server/statement_ship_service.hpp"
// R6-8: the rig arms core 0 as a coordinator, so a write inside a
// transaction ships and enrols instead of being refused.
#include "kds/server/txn_2pc_service.hpp"
#include "kds/server/session_step_client.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/send_retry.hpp"
#include "kds/sched/task.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/extent_lease.hpp"
#include "kds/storage/memory_page_device.hpp"

// One core's stack, and the shutdown protocol that stops it
// (docs/inflight/in-progress/workplan-crosscore.md P2).
//
// These are the engine's **first threaded tests**. They are deliberately
// narrow: what is under test is that a reactor comes up on its own thread,
// stops when told to over the ring, and joins - not anything about what it
// computes, because at P2 a non-zero core computes nothing (see
// core_runtime.hpp on why cores above 0 come up idle).

namespace kds::server {
namespace {

class CoreRuntimeTest : public ::testing::Test {
protected:
    // **Which volume this fixture bootstraps** (AM-S0). `kSingleStream` is
    // what this build writes and what every cell below is about;
    // `CoreRuntimePerCoreStreamTest` overrides it to `kPerCoreStreams` and inherits
    // everything else, which is the only honest way to keep a legacy cell:
    // over a volume that genuinely says per-core, not over a single-stream
    // one with its image rewritten on a copy.
    virtual std::uint32_t LogTopology() const { return kSingleStream; }

    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("kds_core_runtime_" + std::to_string(::getpid()) + "_" +
                std::to_string(counter_++));
        std::filesystem::create_directories(dir_);

        // The database core 0 would own: one shared device, bootstrapped
        // through core 0's own store, exactly as Expeditor does it.
        auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/64);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());

        auto store = storage::DevicePageStore::Open(*device_, kFirstUserPageId);
        ASSERT_TRUE(store.ok()) << store.status().message();
        core0_store_ = std::move(store.value());

        // **Two cores, because that is what this fixture models.** Every
        // test here builds core 0 plus a peer, and several place a relation
        // on core 1 through a two-core catalog - but the superblock said
        // one, and nothing checked until the anchor fold began asking which
        // cores a volume has (AR0 M0, AL-S3). A volume that says one core
        // has no core 1 to publish an anchor for, and the refusal is right.
        auto boot = bootstrap::BootstrapDatabase(*core0_store_, 1000,
                                                 storage::kDefaultInlineCellWidth, /*cores=*/2,
                                                 /*log=*/nullptr, LogTopology());
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        core0_.emplace(std::move(boot.value()));

        // What `Expeditor::Open()` does before anything else exists, and it
        // is load-bearing here: a peer builds its view of *which pages
        // exist* by reading the free map off the device at Open(), so a
        // peer that starts before core 0 has flushed sees an empty database
        // and answers NotFound to everything.
        ASSERT_TRUE(core0_store_->Sync().ok());

        extents_.emplace(*core0_store_, kFirstUserPageId);

        // **Core 0's log, and the stream every peer attaches to** (AM-S0).
        //
        // This is what `Expeditor::Open` does and what this fixture used to
        // fake: `BootstrapDatabase` writes a **single-stream** volume, so a
        // peer must be handed a stream, and `CoreRuntime::Open` refuses one
        // that is not. The fixture previously bootstrapped a single-stream
        // volume and then overwrote the topology to per-core on its copy of
        // the image — a combination no instance can be in, and the defect
        // `git show 30e0377:docs/inflight/bugs/core-runtime-fixture-models-per-core-streams.md`
        // recorded (closed and deleted at AM-S0(b), which is this fixture).
        // The override is gone; the stream is real.
        //
        // Cells that genuinely test the **legacy** topology — a peer opening
        // its own `wal-<core>-*`, running its own recovery, publishing its
        // own anchor — live in `CoreRuntimePerCoreStreamTest` below, over a volume
        // actually says `kPerCoreStreams`.
        //
        // **Skipped whole on the per-core arm** (`CoreRuntimePerCoreStreamTest`):
        // is no stream to attach to on a volume that says every core opens
        // its own, and `ConfigFor` hands nothing.
        if (LogTopology() != kSingleStream) {
            config_superblock_ = core0_->superblock;
            return;
        }
        auto log_device = wal::FileLogDevice::Open(dir_.string(), /*core_id=*/0);
        ASSERT_TRUE(log_device.ok()) << log_device.status().message();
        core0_log_device_ = std::move(log_device.value());

        wal::WalManagerConfig wal_config;
        // The latch, armed because this fixture is two cores by
        // construction — the same conjunct `Expeditor` uses, where a
        // one-core instance arms nothing.
        wal_config.shared_stream = true;
        auto wal = wal::WalManager::Open(core0_log_device_.get(), clock_, /*core_id=*/0,
                                         wal_config);
        ASSERT_TRUE(wal.ok()) << wal.status().message();
        core0_wal_ = std::move(wal.value());
        // **And the writer, because `writer()` is null until this runs.**
        // An attached peer never touches the device, so the writer is its
        // only route to durability, and `CoreRuntime::Open` refuses a peer
        // handed a null one. `Expeditor::Open` calls this at the same
        // point, immediately after opening the log.
        core0_wal_->StartWriter();

        config_superblock_ = core0_->superblock;
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    CoreRuntime::Config ConfigFor(std::uint32_t core_id) {
        CoreRuntime::Config c;
        c.core_id = core_id;
        c.wal_dir = dir_.string();
        // Required, and deliberately so: a core cannot be opened without
        // the volume's image (`core_runtime.hpp`). Every cell below then
        // sees the topology, the ceiling and the times the volume really
        // has, instead of the legal zeros a default-constructed copy gave.
        // Re-read core 0's image **now**, not once at SetUp: `Expeditor`
        // reads it when it launches a peer, and by then core 0's
        // transaction-id ceiling has moved. A snapshot taken earlier hands
        // a restarting peer a ceiling below the ids its own log already
        // names, and the mount refuses - correctly.
        config_superblock_ = core0_->superblock;
        c.superblock = &config_superblock_;
        // What `Expeditor` hands a peer on a single-stream volume, and what
        // `CoreRuntime::Open` refuses to proceed without (AM-S0). Null on
        // the per-core arm, where a peer opens its own log.
        if (core0_wal_ != nullptr) {
            c.shared_stream = core0_wal_->stream();
            c.shared_writer = core0_wal_->writer();
        }
        auto lease = extents_->Reserve(storage::kDefaultExtentPages);
        EXPECT_TRUE(lease.ok()) << lease.status().message();
        if (lease.ok()) c.lease = lease.value();
        return c;
    }

    // What Expeditor does before telling peers to re-read: the catalog
    // pages are unlogged, so nothing else puts them on the device.
    void FlushCatalog() {
        ASSERT_TRUE(core0_store_->FlushPages(catalog::kAllCatalogPages).ok());
    }

    // PW1c-6b-3's rig (defined below, after the schema helpers it needs):
    // a peer owning one populated relation, core 0 as a dispatcher over a
    // real ring, and the client the statement parks on.
    void OpenForeignIndexRig(struct ForeignIndexRig& rig, const char* table);

    // Funds the peer for **one more** relation than the rig opened with
    // (AI-T2). `OpenForeignIndexRig` funds exactly its own table, so a
    // relation created afterwards through core 0's dispatcher is
    // peer-*owned* and peer-*unfunded* - and an unfunded write refuses
    // `TXN_CONFLICT retryable=1` forever, which reads like an engine
    // refusal and is a fixture's. This is `AFundedPeerInsertsIntoItsOwn
    // RelationEndToEnd`'s recipe, once: the fault extent, the write grants
    // over the two creation pages, and a row-id block.
    void FundPeerForRelation(struct ForeignIndexRig& rig, catalog::Oid oid);

    // PW1c-7's restart, shared by two cells because only *half* of it is
    // about the log topology (AM-S0). `flush_before_restart` selects which
    // half: with the pages on the device the claim is read off the platter,
    // which is true whatever the volume's log says; with them only in the
    // log it is redo's replay that leaves them resident and stamped, and a
    // peer runs redo only under per-core streams.
    void PeerPagesSurviveARestart(bool flush_before_restart, const std::string& name);

    // A cross-owner foreign key pair, funded: `<base>p` on core 0 and
    // `<base>c` on the peer, the child referencing the parent. The two
    // placement policies are how a two-core rig puts one relation on each
    // side - `kRotate` at two cores places *everything* on core 1, so the
    // parent takes `kCreatingCore` for the length of its CREATE and nothing
    // else (AH-T6).
    void OpenCrossOwnerFkPair(struct ForeignIndexRig& rig, const std::string& base);

    static inline int counter_ = 0;
    std::filesystem::path dir_;
    sched::SystemClock clock_;
    std::unique_ptr<storage::MemoryPageDevice> device_;
    std::unique_ptr<storage::DevicePageStore> core0_store_;
    std::optional<bootstrap::BootstrapResult> core0_;
    // Core 0's log and the manager over it. Declared before
    // `config_superblock_` so they outlive nothing that matters here, and
    // **borrowed by every peer a cell opens** — which is why a cell must
    // let its `CoreRuntime`s go before the fixture does (they are locals;
    // the fixture's members die after `TearDown`).
    std::unique_ptr<wal::FileLogDevice> core0_log_device_;
    std::unique_ptr<wal::WalManager> core0_wal_;
    // The image `ConfigFor` hands over: core 0's, as the volume really is.
    SuperBlock config_superblock_;
    std::optional<storage::ExtentAllocator> extents_;
};

// ---- The legacy arm ------------------------------------------------------
//
// **A volume this build does not create, mounted the way it still mounts.**
// `wal.md` §3 keeps per-core streams as a live branch: a pre-M0 volume says
// `kPerCoreStreams` in its superblock and every core opens its own
// `wal-<core>-*`, runs its own recovery and publishes its own anchor. The
// cells marked `CoreRuntimePerCoreStreamTest` are the ones that are *about*
// that
// branch, and they are here rather than deleted because deleting them would
// leave the branch untested while it is still reachable.
//
// **The name carries the `CoreRuntime` prefix on purpose**: cells are
// registered as `Fixture.Name` (`gtest_discover_tests`), so a fixture named
// anything else would drop this arm out of `ctest -R CoreRuntime` - the very
// command the work order names as the stage's verification, and the arm this
// stage exists to preserve.
//
// The whole difference is `LogTopology()`, and it is the difference that
// matters: `BootstrapDatabase` writes the topology into the image, so this
// arm's peers see per-core streams because the volume says so - not because
// `SetUp` rewrote the image on its own copy, which is the defect AM-S0
// removed (recorded while it stood, at
// `git show 30e0377:docs/inflight/bugs/core-runtime-fixture-models-per-core-streams.md`).
class CoreRuntimePerCoreStreamTest : public CoreRuntimeTest {
protected:
    std::uint32_t LogTopology() const override { return kPerCoreStreams; }
};

TEST_F(CoreRuntimePerCoreStreamTest, EachCoreOpensItsOwnWalStream) {
    // The segment naming (`wal-<core_id>-<segment_no>.log`) predates
    // multicore, which is why N streams share one directory without
    // colliding. Asserted because it is load-bearing and invisible.
    std::vector<std::unique_ptr<CoreRuntime>> cores;
    for (std::uint32_t id = 0; id < 3; ++id) {
        auto core = CoreRuntime::Open(ConfigFor(id), *device_, clock_, nullptr);
        ASSERT_TRUE(core.ok()) << core.status().message();
        EXPECT_EQ(core.value()->core_id(), id);
        EXPECT_EQ(core.value()->wal().core_id(), id);
        cores.push_back(std::move(core.value()));
    }

    int segments = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
        if (entry.path().filename().string().rfind("wal-", 0) == 0) ++segments;
    }
    EXPECT_EQ(segments, 3) << "three cores did not produce three streams";
}

TEST_F(CoreRuntimeTest, AnotherThreadStopsTheReactorThroughTheAtomicFlag) {
    // **Renamed with the mechanism it tests** (AU-S3). It was
    // `AShutdownMessageStopsTheReactorFromItsOwnThread`, and both halves of
    // that name are now false: there is no message, and the point is that
    // the stop comes from a thread that is *not* the reactor's.
    //
    // `Scheduler::Stop()` wrote a plain bool, so only the reactor's own
    // thread could flip it - which is the whole reason `kShutdown` existed.
    // The flag is atomic now and the message is gone.
    auto transport = sched::RealRingTransport::Create(2, 16, 64);
    ASSERT_TRUE(transport.ok());

    auto core = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(core.ok()) << core.status().message();
    ASSERT_TRUE(core.value()->AttachTransport(transport.value()).ok());

    std::thread worker([&] { core.value()->Run(); });

    // **AU-S3: a direct call, not a message.** `Scheduler::stopped_` is
    // atomic now, so another thread may flip it - which is exactly what the
    // comment above this test used to say was impossible, and the reason
    // `kShutdown` existed. No kick here: the default idle block is 10 ms, so
    // the reactor notices within one, and a test that needed it sooner would
    // register a `WakerTable`.
    core.value()->scheduler().Stop();

    // If the message is not noticed the test hangs, which is the honest
    // failure for "the reactor never stopped" - a timeout here would only
    // convert a hang into a flake.
    worker.join();
    EXPECT_TRUE(core.value()->scheduler().stopped());
}

TEST_F(CoreRuntimeTest, ShutdownStopsOnlyTheCoreItIsAddressedTo) {
    auto transport = sched::RealRingTransport::Create(3, 16, 64);
    ASSERT_TRUE(transport.ok());

    // The survivor is checked by **liveness, not by reading its flag**, and
    // the reason changed at AU-S3 without the practice changing. Reading
    // `stopped()` from here is no longer a data race - the flag is atomic -
    // but it was never the assertion worth making: `false` says only that
    // nobody asked this core to stop, where the claim is that it is still
    // serving. A running core is shown to be running by making it do
    // something observable.
    std::atomic<int> served{0};

    std::vector<std::unique_ptr<CoreRuntime>> cores;
    for (std::uint32_t id = 1; id < 3; ++id) {
        auto core = CoreRuntime::Open(ConfigFor(id), *device_, clock_, nullptr);
        ASSERT_TRUE(core.ok()) << core.status().message();
        ASSERT_TRUE(core.value()->AttachTransport(transport.value()).ok());
        ASSERT_TRUE(core.value()
                        ->scheduler()
                        .RegisterMessageHandler(
                            sched::RingMessageKind::kStepEof,
                            [&served](const sched::MessageHeader&, std::span<const std::byte>) {
                                served.fetch_add(1, std::memory_order_relaxed);
                            })
                        .ok());
        cores.push_back(std::move(core.value()));
    }

    std::vector<std::thread> workers;
    for (auto& core : cores) workers.emplace_back([&core] { core->Run(); });

    auto send = [&](std::uint32_t dst, sched::RingMessageKind kind) {
        sched::MessageHeader h{};
        h.src_core = 0;
        h.dst_core = dst;
        h.kind = static_cast<std::uint16_t>(kind);
        h.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
        ASSERT_TRUE(transport.value().TrySend(h, {}).ok());
    };

    cores[0]->scheduler().Stop();  // AU-S3: the flag is atomic; no message
    workers[0].join();
    // Safe here and only here: the join is what makes core 1's writes
    // visible to this thread.
    EXPECT_TRUE(cores[0]->scheduler().stopped());

    // Core 2 is still serving - which is a stronger statement than "its flag
    // is false", and one this thread is allowed to make.
    send(2, sched::RingMessageKind::kStepEof);
    for (int i = 0; i < 1000 && served.load(std::memory_order_relaxed) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(served.load(std::memory_order_relaxed), 1)
        << "one core's stop took another down with it";

    cores[1]->scheduler().Stop();  // AU-S3
    workers[1].join();
    EXPECT_TRUE(cores[1]->scheduler().stopped());
}

TEST_F(CoreRuntimeTest, ManyCoresStartAndJoinCleanly) {
    // The shape Expeditor::Serve() uses, at the counts the workplan's test
    // matrix names. What this is really checking is that nothing in the
    // startup path is accidentally shared - four reactors, four epoll
    // instances, four WAL streams, no synchronization anywhere.
    constexpr std::uint32_t kCores = 4;
    auto transport = sched::RealRingTransport::Create(kCores, 16, 64);
    ASSERT_TRUE(transport.ok());

    std::vector<std::unique_ptr<CoreRuntime>> cores;
    for (std::uint32_t id = 1; id < kCores; ++id) {
        auto core = CoreRuntime::Open(ConfigFor(id), *device_, clock_, nullptr);
        ASSERT_TRUE(core.ok()) << core.status().message();
        ASSERT_TRUE(core.value()->AttachTransport(transport.value()).ok());
        cores.push_back(std::move(core.value()));
    }

    std::vector<std::thread> workers;
    for (auto& core : cores) workers.emplace_back([&core] { core->Run(); });

    for (auto& core : cores) core->scheduler().Stop();  // AU-S3

    for (auto& worker : workers) worker.join();
    for (auto& core : cores) {
        EXPECT_TRUE(core->scheduler().stopped());
        EXPECT_TRUE(core->Sync().ok());
    }
}

// ---- P6: a peer serves a statement ------------------------------------

// Creates a relation through core 0's catalog, exactly as a DDL statement
// would, and puts the catalog pages on the device so a peer can read them.
catalog::Schema TwoColumnSchema() {
    catalog::Schema schema;
    catalog::SysColumnRow id{};
    id.pos = 0;
    catalog::SetName(id.name, "id");
    id.type_val = catalog::kTypeValInt64;
    id.len = 8;
    id.notnull = true;
    catalog::SysColumnRow v{};
    v.pos = 1;
    catalog::SetName(v.name, "v");
    v.type_val = catalog::kTypeValInt64;
    v.len = 8;
    v.notnull = true;
    schema.columns = {id, v};
    return schema;
}

TEST_F(CoreRuntimeTest, APeerResolvesARelationCoreZeroCreated) {
    // The point of the whole phase: a non-zero core can read the catalog,
    // so it can resolve a relation, so it can serve a statement.
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "t", TwoColumnSchema(),
                                           catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    FlushCatalog();

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    auto found = peer.value()->catalog().FindTableOidByName("t");
    ASSERT_TRUE(found.ok()) << found.status().message();
    EXPECT_EQ(found.value(), oid.value());

    // And the whole schema, not just the name - which is what a step
    // compiler needs before it can plan anything.
    auto access = peer.value()->catalog().InitTableAccess(found.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    EXPECT_EQ(access.value()->schema.columns.size(), 2u);
}

TEST_F(CoreRuntimeTest, APeerDoesNotSeeADdlThatWasNotFlushed) {
    // The ordering the scheme rests on. Catalog writes are unlogged, so
    // without core 0's flush the peer reads the device's older bytes - and
    // answers "not found", which is stale rather than wrong.
    //
    // The relation is created **before** the peer opens, so the peer's
    // free-map snapshot already knows its pages; the second relation below
    // is the one that tests the flush. See the blocker note at the bottom
    // of this file for why that ordering matters.
    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    ASSERT_TRUE(core0_->catalog
                    .CreateTable(catalog::kNamespacePublic, "late", TwoColumnSchema(),
                                 catalog::ClusteredType::kHeap)
                    .ok());

    // Not flushed yet: invisible, and a NotFound rather than an error.
    auto before = peer.value()->catalog().FindTableOidByName("late");
    EXPECT_FALSE(before.ok());
    EXPECT_EQ(before.status().code(), StatusCode::kNotFound);

    FlushCatalog();
    peer.value()->InvalidateCatalog();

    // The name resolves off the flushed page. Its *schema* does not yet -
    // InitTableAccess would need the relation's pages, which are not in
    // this peer's lease. That is the blocker below, not a fault in the
    // flush-then-invalidate ordering this test covers.
    auto after = peer.value()->catalog().FindTableOidByName("late");
    EXPECT_TRUE(after.ok()) << after.status().message();
}

TEST_F(CoreRuntimeTest, InvalidatingTheCatalogRefreshesThePeersFreeMap) {
    // **The catalog can grow onto a page the peer has never heard of.**
    //
    // A peer's free-map copy is a snapshot taken at Open(). Until
    // 2026-08-26 the only thing that refreshed it was a relation grant, so
    // a page core 0 allocated with *no grant attached* stayed invisible to
    // the peer forever - and the catalog is exactly that case: `sys.indexes`
    // fills its root and spills onto `kCatalogOverflowFirst`, which core 0
    // allocates from the map it owns. The peer then re-reads the chain,
    // follows `next_page_id` into that page, and `IsAllocated` answers from
    // a snapshot in which it does not exist: `page id not found`, which
    // carries no retryable bit and never clears.
    //
    // Measured before the fix: 58 shipped `CREATE INDEX`es on a peer-owned
    // relation, after which every write to it failed permanently
    // (`bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md` §8d).
    // This is that hazard in one page, without the 58 builds.
    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    // Core 0 allocates a page *after* the peer's snapshot was taken - the
    // catalog's overflow growth, in miniature.
    auto fresh = core0_store_->CreateNew();
    ASSERT_TRUE(fresh.ok()) << fresh.status().message();
    const PageId grown = fresh.value().first;
    EXPECT_TRUE(core0_store_->IsAllocated(grown)) << "core 0 owns the map; it knows at once";

    // The peer does not know it yet, and that is not the bug - the snapshot
    // is allowed to be behind. Reading it answers NotFound, which is what
    // becomes permanent without the refresh below.
    EXPECT_FALSE(peer.value()->store().IsAllocated(grown))
        << "the peer's snapshot predates the allocation";

    // The flush half is the one `BroadcastCatalogInvalidation` already runs
    // before the message leaves: FlushPages writes the dirty maps after the
    // pages they describe, so the bit is on the device before any peer is
    // told to look.
    ASSERT_TRUE(core0_store_->FlushPages(catalog::kEveryCatalogPage).ok());

    peer.value()->InvalidateCatalog();

    // The peer has adopted the bit, so a page core 0 grew the catalog onto
    // is addressable here rather than answering NotFound forever.
    EXPECT_TRUE(peer.value()->store().IsAllocated(grown))
        << "the invalidation must refresh the free map, not only the frames";
}

TEST_F(CoreRuntimeTest, APeerResolvesARelationWhoseCatalogRowsSpilledOntoAnOverflowPage) {
    // **The scenario, not just the mechanism.** The test above pins that an
    // invalidation refreshes the map; this one is the shape that made it
    // matter, and it fails the same way the 58-build reproduction did.
    //
    // A catalog chain grows onto `kCatalogOverflowFirst` when its root
    // fills (`AllocateCatalogPage`). `sys.columns` is the fastest to get
    // there - one row per column of every relation - so a peer that mounted
    // before the spill, and then resolves a relation whose columns live on
    // the new page, walks `next_page_id` into an id its snapshot calls
    // free. Without the refresh that is `page id not found`, permanently.
    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    // Everything from here is created *after* the peer's snapshot.
    const PageId first_overflow = catalog::kCatalogOverflowFirst;
    ASSERT_FALSE(peer.value()->store().IsAllocated(first_overflow))
        << "the spill has not happened yet";

    std::string spilled;
    for (int i = 0; i < 64 && !core0_store_->IsAllocated(first_overflow); ++i) {
        spilled = "spill" + std::to_string(i);
        auto created = core0_->catalog.CreateTable(catalog::kNamespacePublic, spilled,
                                                   TwoColumnSchema(),
                                                   catalog::ClusteredType::kHeap);
        ASSERT_TRUE(created.ok()) << created.status().message();
    }
    ASSERT_TRUE(core0_store_->IsAllocated(first_overflow))
        << "the catalog never spilled; this test needs a chain that grows";

    // Core 0's half of the ordering, exactly as BroadcastCatalogInvalidation
    // runs it: FlushPages writes the dirty maps after the pages.
    ASSERT_TRUE(core0_store_->FlushPages(catalog::kEveryCatalogPage).ok());
    peer.value()->InvalidateCatalog();

    // The last relation created is the one whose rows are furthest along
    // the chain, so resolving it is what walks onto the overflow page.
    auto found = peer.value()->catalog().FindTableOidByName(spilled);
    ASSERT_TRUE(found.ok()) << found.status().message();
    auto access = peer.value()->catalog().InitTableAccess(found.value());
    ASSERT_TRUE(access.ok()) << access.status().message()
                             << " - the peer could not read a catalog page core 0 allocated "
                                "after it mounted";
    EXPECT_EQ(access.value()->schema.columns.size(), 2u);
}

TEST_F(CoreRuntimeTest, APeerReadsTheCatalogAndCannotWriteIt) {
    // The asymmetry that makes a peer's stale view safe: one writer per
    // catalog page, so a peer can be behind but never torn.
    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    EXPECT_TRUE(peer.value()->store().MayFault(catalog::kCatalogPageTables));
    EXPECT_FALSE(peer.value()->store().MayWrite(catalog::kCatalogPageTables));
    EXPECT_TRUE(peer.value()->store().MayFault(kSuperBlockPageId));
    EXPECT_FALSE(peer.value()->store().MayWrite(kSuperBlockPageId));

    // Its own leased pages stay fully its own - the system range is an
    // addition to the lease rule, not a replacement for it.
    auto own = peer.value()->store().CreateNew();
    ASSERT_TRUE(own.ok()) << own.status().message();
    EXPECT_TRUE(peer.value()->store().MayWrite(own.value().first));
}

// ---- CC7: the ownership reconciliation (workplan P6b) -----------------
//
// The blocker P6 stopped at - relation ownership and page ownership were
// different facts nothing reconciled - is decided (crosscore.md CC7,
// operator-ratified 2026-08-10): **page ownership is a function of the
// catalog**, realized at DDL publish by the flush-then-grant handoff. The
// test below is the positive contract that replaced the pinned negative
// (`APeerCannotYetFaultARelationsDataPages`): after the grant, the owner
// faults the relation's pages read-only and its schema resolves.
//
// A second, independent blocker remains: **a peer cannot INSERT**, because
// `Catalog::AllocateRowId()` bumps `next_id` on the sys.tables page, and a
// peer may not write the catalog. That one is P5's shape - a leased range
// of row ids, exactly like the page-id lease - and
// `docs/rules/keystoneid-invariant.md` K-M2's bump-ahead allocator is the same
// mechanism.

TEST_F(CoreRuntimeTest, AGrantedPeerFaultsARelationsDataPagesReadOnly) {
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "t", TwoColumnSchema(),
                                           catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok());
    // The flush half of flush-then-grant: the relation's pages must be on
    // the device before the grant makes them reachable, or the peer faults
    // stale bytes. Sync() covers the catalog pages and the relation's own.
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto row = core0_->catalog.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    // The catalog resolves - that is what P6's catalog half bought.
    EXPECT_TRUE(peer.value()->catalog().FindTableOidByName("t").ok());

    // Before the grant: the old pinned state. Core 0 allocated the root, so
    // it is in no lease of this peer's - the check must still refuse it, or
    // the grant below is not what made the difference.
    EXPECT_FALSE(peer.value()->store().MayFault(row.value().desc_page_id));

    // The grant (what a kRelationFaultGrant message delivers; called
    // directly for the reason InvalidateCatalog is callable directly).
    peer.value()->GrantRelationFault(
        RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));

    // Readable, never writable: CC7 grants fault rights only. The write
    // path arrives with statement dispatch, not with this grant.
    EXPECT_TRUE(peer.value()->store().MayFault(row.value().desc_page_id));
    EXPECT_FALSE(peer.value()->store().MayWrite(row.value().desc_page_id));

    // And the schema resolves now: InitTableAccess reads the relation's
    // root page, which is exactly what the old test pinned as impossible.
    EXPECT_TRUE(peer.value()->catalog().InitTableAccess(oid.value()).ok());
}

TEST_F(CoreRuntimeTest, AWriteGrantedPeerRestampsThePageAndMayWriteIt) {
    // PW1c-4's receive side, and PL §9 rule 6 end to end at the store: the
    // granted page is restamped to the peer's stream - stamp its own,
    // page_lsn re-based into its space - flushed, and only then writable.
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "t", TwoColumnSchema(),
                                           catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok());
    ASSERT_TRUE(core0_store_->Sync().ok());
    auto row = core0_->catalog.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    const PageId root = row.value().desc_page_id;

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    // The fault grant precedes the write grant on the wire; mirror it.
    peer.value()->GrantRelationFault(
        RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));
    ASSERT_FALSE(peer.value()->store().MayWrite(root));

    const PageId pages[] = {root};
    peer.value()->GrantRelationWrite(pages);

    EXPECT_TRUE(peer.value()->store().MayWrite(root));
    auto page = peer.value()->store().GetForRead(root);
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(storage::GetPageStreamStamp(page.value().bytes()),
              storage::StreamStampFor(1))
        << "the acquisition restamp must name the peer's stream";
    // page_lsn names the peer's logged acquisition record: nonzero, in
    // this stream's space, strictly below the append point (the WAL gate
    // refuses a page claiming a record never logged - what forced the
    // acquisition to be a record at all).
    const auto lsn = storage::GetPageLsn(page.value().bytes());
    EXPECT_NE(lsn, 0u);
    EXPECT_LT(lsn, peer.value()->wal().appended_lsn());
}

// ---- P6c: placement -----------------------------------------------------

TEST(CorePlacementTest, RotationSkipsTheSystemCoreAndCreatingStaysPut) {
    using catalog::AssignOwnerCore;
    using catalog::PlacementPolicy;
    // The default policy pins to the creating core whatever the count.
    static_assert(AssignOwnerCore(PlacementPolicy::kCreatingCore, 0, 4, 7) == 0);
    // Rotation walks the non-system cores in relation order...
    static_assert(AssignOwnerCore(PlacementPolicy::kRotate, 0, 4, 0) == 1);
    static_assert(AssignOwnerCore(PlacementPolicy::kRotate, 0, 4, 1) == 2);
    static_assert(AssignOwnerCore(PlacementPolicy::kRotate, 0, 4, 2) == 3);
    static_assert(AssignOwnerCore(PlacementPolicy::kRotate, 0, 4, 3) == 1);
    // ...never lands on core 0...
    static_assert(AssignOwnerCore(PlacementPolicy::kRotate, 0, 2, 5) == 1);
    // ...and degrades to the creating core when there is nowhere to rotate.
    static_assert(AssignOwnerCore(PlacementPolicy::kRotate, 0, 1, 5) == 0);

    // AF-T2's seam, the shipped default. An undeclared namespace - `public`,
    // which is every relation until somebody writes CREATE NAMESPACE - is
    // `kCreatingCore`'s answer whatever the relation count, which is what
    // keeps DA2 true under the new default.
    using catalog::NamespacePlacement;
    static_assert(AssignOwnerCore(PlacementPolicy::kNamespace, 0, 4, 7) == 0);
    static_assert(AssignOwnerCore(PlacementPolicy::kNamespace, 0, 4, 7,
                                  NamespacePlacement{}) == 0);
    // A declared namespace nothing has placed rotates on its **declaration
    // order**, not on the relation count - two relations in one namespace
    // must not land on two cores.
    static_assert(AssignOwnerCore(PlacementPolicy::kNamespace, 0, 4, 99,
                                  NamespacePlacement{catalog::kUnplacedNamespace, 0, true}) == 1);
    static_assert(AssignOwnerCore(PlacementPolicy::kNamespace, 0, 4, 99,
                                  NamespacePlacement{catalog::kUnplacedNamespace, 1, true}) == 2);
    static_assert(AssignOwnerCore(PlacementPolicy::kNamespace, 0, 4, 99,
                                  NamespacePlacement{catalog::kUnplacedNamespace, 3, true}) == 1);
    // A namespace its first relation already fixed answers that, and the
    // rank is not consulted - AF-P4's "never rebalanced".
    static_assert(AssignOwnerCore(PlacementPolicy::kNamespace, 0, 4, 99,
                                  NamespacePlacement{3, 0, true}) == 3);
    // Including back onto core 0, which is a legal answer and the reason
    // absence needs a sentinel rather than a zero.
    static_assert(AssignOwnerCore(PlacementPolicy::kNamespace, 0, 4, 99,
                                  NamespacePlacement{0, 2, true}) == 0);
    // One core has nowhere to rotate, exactly as `rotate` degrades.
    static_assert(AssignOwnerCore(PlacementPolicy::kNamespace, 0, 1, 0,
                                  NamespacePlacement{catalog::kUnplacedNamespace, 5, true}) == 0);
    SUCCEED();
}

TEST_F(CoreRuntimeTest, ARotatedRelationIsPlacedOnAPeerAndPublished) {
    // The catalog half of P6c end to end: rotation chooses a peer, the
    // publish hook fires with the facts the send needs, and the grant it
    // implies lets that peer fault the relation - the same grant P6b's
    // test drives, now produced by the placement path rather than by hand.
    //
    // A two-core catalog over the same store, because the fixture's was
    // bootstrapped at core_count = 1 and rotation correctly degrades to
    // the creating core there - which the placement unit test pins.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/2);
    catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);

    struct Published {
        catalog::Oid oid = 0;
        std::uint32_t owner = 0;
        PageId root = kInvalidPageId;
        PageId varheap = kInvalidPageId;
        int calls = 0;
    } published;
    // The evict runs *inside* the hook - CreateTable is still on the
    // stack - which is what pins the 25059bf review's C-1: the root's
    // creation PageRef must have dropped by publish time, or the
    // production hook's EvictClean of departed pages fails on every peer
    // CREATE TABLE. Flush first; eviction refuses dirty frames, and the
    // production hook flushes before it too.
    Status evict_at_publish = Status::OK();
    catalog2.SetRelationPublishHook(
        [&](catalog::Oid oid, std::uint32_t owner, PageId root, PageId varheap, PageId anchor) {
            published = {oid, owner, root, varheap, published.calls + 1};
            EXPECT_NE(anchor, kInvalidPageId) << "a user relation always gets an anchor (PW2-1)";
            const PageId departed[] = {root};
            evict_at_publish = core0_store_->FlushPages(departed);
            if (evict_at_publish.ok()) {
                evict_at_publish = core0_store_->EvictClean(departed);
            }
        });

    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "rotated",
                                    TwoColumnSchema(), catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    // The catalog recorded the rotated owner, and the hook saw the same
    // facts the row carries.
    auto row = catalog2.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().owner_core, 1u);
    EXPECT_EQ(published.calls, 1);
    EXPECT_EQ(published.oid, oid.value());
    EXPECT_EQ(published.owner, 1u);
    EXPECT_EQ(published.root, row.value().desc_page_id);

    EXPECT_TRUE(evict_at_publish.ok())
        << "the root must be unpinned when the publish hook fires: "
        << evict_at_publish.message();

    // The grant the hook's installer would send reaches the peer, and the
    // relation resolves there - CC7's whole point, driven by placement.
    ASSERT_TRUE(core0_store_->Sync().ok());
    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    peer.value()->GrantRelationFault(
        RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));
    EXPECT_TRUE(peer.value()->store().MayFault(row.value().desc_page_id));
    EXPECT_TRUE(peer.value()->catalog().InitTableAccess(oid.value()).ok());
}

// ---- Row-id leases (P5's shape) ----------------------------------------

TEST(RowIdLeaseTableTest, IssuesFromAGrantAndExhaustsRetryably) {
    catalog::RowIdLeaseTable table;

    // No grant yet: exhaustion, and the code a retry loop keys on.
    auto dry = table.Next(1000);
    ASSERT_FALSE(dry.ok());
    EXPECT_EQ(dry.status().code(), StatusCode::kTxnConflict);

    table.Grant(1000, 100, 3);
    EXPECT_EQ(table.Next(1000).value(), 100u);
    EXPECT_EQ(table.Next(1000).value(), 101u);
    // Relations do not share blocks: oid 2000's lease is its own.
    EXPECT_FALSE(table.Next(2000).ok());
    EXPECT_EQ(table.Next(1000).value(), 102u);
    EXPECT_EQ(table.Next(1000).status().code(), StatusCode::kTxnConflict);

    // A contiguous grant extends; a disjoint one replaces and burns.
    table.Grant(1000, 103, 2);
    EXPECT_EQ(table.Next(1000).value(), 103u);
    table.Grant(1000, 500, 2);
    EXPECT_EQ(table.Next(1000).value(), 500u);
}

TEST(RowIdLeaseTableTest, ADeniedRelationAnswersOnceWithoutTheBitThenAsksAgain) {
    // The review of the retryable-bit change: a "none" from core 0 is
    // permanent, so the statement that meets it must not be told to retry
    // with the bit - it would spin to its own deadline. One answer without
    // the bit, and the entry is re-armed so the next statement asks again.
    catalog::RowIdLeaseTable table;
    auto miss = table.Next(1000);
    ASSERT_FALSE(miss.ok());
    EXPECT_TRUE(miss.status().retryable());
    ASSERT_EQ(*table.NeediestRelation(), 1000u);

    table.Deny(1000);
    EXPECT_FALSE(table.NeediestRelation().has_value()) << "a denied relation is not demand";
    auto denied = table.Next(1000);
    ASSERT_FALSE(denied.ok());
    EXPECT_EQ(denied.status().code(), StatusCode::kResourceExhausted);
    EXPECT_FALSE(denied.status().retryable());
    ASSERT_EQ(*table.NeediestRelation(), 1000u) << "the denied answer must re-arm the demand";

    // Re-armed means the next miss is fresh demand again, with the bit.
    EXPECT_TRUE(table.Next(1000).status().retryable());
}

TEST_F(CoreRuntimeTest, APeerIssuesLeasedRowIdsWithoutWritingTheCatalog) {
    // The whole point of the lease: a peer's AllocateRowId() answers from
    // its granted block and never touches the sys.tables page - which its
    // own store would refuse to write anyway (MayWrite is the guard this
    // path exists to satisfy, not to bypass).
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "t", TwoColumnSchema(),
                                           catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok());
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    // Before any grant: retryable exhaustion, never a catalog write.
    auto dry = peer.value()->catalog().AllocateRowId(oid.value());
    ASSERT_FALSE(dry.ok());
    EXPECT_EQ(dry.status().code(), StatusCode::kTxnConflict);

    // Core 0 carves a block with the bulk-INSERT primitive - the exact
    // call the kRowIdLease handler makes - and the peer's table takes it,
    // the exact application the receiver makes.
    auto first = core0_->catalog.AllocateRowIdRange(oid.value(), 16);
    ASSERT_TRUE(first.ok()) << first.status().message();
    peer.value()->row_id_leases().Grant(oid.value(), first.value(), 16);

    // The peer issues the block, in order, from its own table.
    for (std::uint64_t i = 0; i < 16; ++i) {
        auto id = peer.value()->catalog().AllocateRowId(oid.value());
        ASSERT_TRUE(id.ok()) << id.status().message();
        EXPECT_EQ(id.value(), first.value() + i);
    }
    EXPECT_EQ(peer.value()->catalog().AllocateRowId(oid.value()).status().code(),
              StatusCode::kTxnConflict);

    // And the blocks stay disjoint: core 0's next single id sits past the
    // granted block, so a peer id can never collide with a core-0 id -
    // K1's issue-once contract across cores.
    auto next_on_core0 = core0_->catalog.AllocateRowId(oid.value());
    ASSERT_TRUE(next_on_core0.ok());
    EXPECT_GE(next_on_core0.value(), first.value() + 16);
}

TEST_F(CoreRuntimeTest, APeerIssuesLeasedTransactionIdsWithoutWritingTheSuperblock) {
    // The row-id lease's twin, and the door PW1 opened
    // (`docs/inflight/in-progress/workplan-peer-writer.md`): before it, a peer's TrxIdSequence
    // constructed spent and its persist callback refused, so a peer could
    // not begin a *single* transaction - every write died at its first id,
    // ahead of any page. Reads never noticed: a read view mints from
    // `peek()`, which issues nothing.
    // Core 0's ceiling travels in the config, the way its WAL anchor does -
    // as of the AL-S9 review, inside the whole superblock rather than as a
    // field of its own, because a peer's copy answered a legal zero for
    // every field nobody had thought to carry.
    CoreRuntime::Config config = ConfigFor(1);
    ASSERT_GT(config.superblock->next_trx_id(), 0u)
        << "a bootstrapped database should carry a ceiling";

    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    // A read still works with no lease at all, which is the half that was
    // never broken and must stay unbroken.
    EXPECT_EQ(peer.value()->dispatcher().Dispatch("SHOW TABLES").response.rfind("ERR", 0),
              std::string::npos);

    // Before any grant: retryable exhaustion, and never a write to page 0 -
    // this core's store would refuse that anyway, which is the guard this
    // path exists to satisfy rather than to bypass.
    const auto dry = peer.value()->dispatcher().Dispatch("BEGIN").response;
    EXPECT_EQ(dry.rfind("ERR", 0), 0u) << "a peer began a transaction with no leased ids: " << dry;
    EXPECT_NE(dry.find("lease"), std::string::npos)
        << "the refusal should name the lease, not page 0: " << dry;

    // Core 0 carves a block through the same `Carve()` its own windows come
    // from - the exact call the kTrxIdLease handler makes - and the peer's
    // lease takes it, the exact application the receiver makes.
    txn::TrxIdSequence core0_ids(core0_->superblock);
    auto block = core0_ids.Carve(16);
    ASSERT_TRUE(block.ok()) << block.status().message();
    peer.value()->trx_id_lease().Grant(block.value().first, block.value().count);

    const auto wet = peer.value()->dispatcher().Dispatch("BEGIN").response;
    EXPECT_NE(wet.rfind("ERR", 0), 0u) << "a leased peer still could not begin: " << wet;
    (void)peer.value()->dispatcher().Dispatch("ROLLBACK");

    // The grant sits at or above the ceiling the config carried, so the
    // out-of-order guard in `ReserveBlock` has a real floor to check
    // against rather than the 0 a default-constructed superblock reads.
    EXPECT_GE(block.value().first, config.superblock->next_trx_id());

    // And the windows stay disjoint: core 0's next id sits past the block it
    // granted, so a peer's transaction id can never collide with a core-0
    // one - invariant 12's writer identity across cores.
    auto next_on_core0 = core0_ids.Next();
    ASSERT_TRUE(next_on_core0.ok()) << next_on_core0.status().message();
    EXPECT_GE(next_on_core0.value(), block.value().first + block.value().count);
}

// Core 0's receiving half of a peer's anchor, as `Expeditor::Serve` wires
// it: the ring payload decoded into the superblock anchor's Publish.
void RegisterAnchorReceiver(sched::Scheduler& core0, SuperBlockCheckpointAnchor& receiver) {
    ASSERT_TRUE(core0
                    .RegisterMessageHandler(
                        sched::RingMessageKind::kAnchorWrite,
                        [&receiver](const sched::MessageHeader&,
                                    std::span<const std::byte> payload) {
                            ASSERT_EQ(payload.size(), sizeof(AnchorWritePayload));
                            AnchorWritePayload fields{};
                            std::memcpy(&fields, payload.data(), sizeof(fields));
                            wal::CheckpointAnchorRecord record;
                            record.core_id = fields.core_id;
                            record.checkpoint_lsn = fields.checkpoint_lsn;
                            record.redo_start_lsn = fields.redo_start_lsn;
                            record.durable_lsn = fields.durable_lsn;
                            record.segment_no = fields.segment_no;
                            EXPECT_TRUE(receiver.Publish(record).ok());
                        })
                    .ok());
}

TEST_F(CoreRuntimeTest, APeersCheckpointAnchorReachesCoreZerosSuperblock) {
    // PW3. A peer cannot write page 0, so its completed checkpoint sends the
    // anchor and core 0 writes it (remote_checkpoint_anchor.hpp). Before
    // this, a peer had no checkpointer at all: it published nothing, its
    // anchor slot never advanced, and every later mount rescanned its whole
    // stream - free while a peer could not write, and not free since PW1.
    //
    // The property asserted is the end of that path, not the send: the
    // peer's anchor reaches core 0 and is folded into the volume's one
    // anchor. `SuperBlockCheckpointAnchor` is the receiving half here
    // exactly as it is in `Expeditor::Serve`.
    //
    // **What "reaches core 0" means changed with AR0 M0**, and this test is
    // where it shows. Under per-core streams the peer's number landed in
    // *its own* slot, and the slot moving was the proof. Under one stream
    // there is one anchor, slot 0, holding the minimum over every core - and
    // it does not move until every core has published at least once, or it
    // would advance past records a silent core still needs (AL-S3's
    // warm-up). So the arrival is proved by the fold's own input, and the
    // anchor's movement by completing the fold.
    auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 64);
    ASSERT_TRUE(transport.ok()) << transport.status().message();

    sched::NullIoBackend io0;
    sched::Scheduler core0(clock_, io0);
    ASSERT_TRUE(core0.AttachTransport(&transport.value(), 0).ok());

    SuperBlockCheckpointAnchor receiver(core0_->superblock, *core0_store_);
    RegisterAnchorReceiver(core0, receiver);

    ASSERT_EQ(core0_->superblock.wal_anchor(0).checkpoint_lsn, 0u)
        << "a fresh database has no anchor before anything checkpoints";
    ASSERT_EQ(receiver.folded_cores(), 0u);

    CoreRuntime::Config config = ConfigFor(1);
    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    // AttachTransport runs the completion checkpoint (RC08's half for a
    // peer) and queues the send on this core's own reactor.
    ASSERT_TRUE(peer.value()->AttachTransport(transport.value()).ok());

    for (int i = 0; i < 20; ++i) {
        peer.value()->scheduler().RunOnce();
        core0.RunOnce();
    }

    EXPECT_EQ(receiver.publishes(), 1u)
        << "the peer's completion checkpoint never reached core 0";
    EXPECT_EQ(receiver.folded_cores(), 1u)
        << "it reached core 0 but is not in the fold, so it counts for nothing";
    // Held, not advanced: core 0 has not published, so one core's number is
    // not yet a floor for the instance.
    EXPECT_EQ(core0_->superblock.wal_anchor(0).checkpoint_lsn, 0u);
    // And no peer slot was written, which is the other half of one anchor.
    EXPECT_EQ(core0_->superblock.wal_anchor(1).checkpoint_lsn, 0u);

    // Core 0 speaks, the warm-up ends, and the one anchor moves.
    //
    // **Above the peer, deliberately.** The fold selects the *lowest*
    // `redo_start_lsn` and breaks a tie by ascending core id, so a stand-in
    // sharing the peer's number would win it - and this test would then
    // assert its own synthetic record back to itself while proving nothing
    // about the peer's anchor reaching page 0, which is its whole subject.
    const wal::Lsn peer_point = peer.value()->wal().appended_lsn();
    ASSERT_TRUE(receiver
                    .Publish({/*core_id=*/0, peer_point + 1, peer_point + 1, peer_point + 1, 0})
                    .ok());
    EXPECT_EQ(receiver.folded_cores(), 2u);
    const std::uint64_t folded = core0_->superblock.wal_anchor(0).checkpoint_lsn;
    EXPECT_GT(folded, 0u) << "every core has published and the anchor still has not moved";
    // And it is the *peer's* record that landed, not core 0's stand-in -
    // which is the sentence this test's name makes.
    EXPECT_NE(folded, peer_point + 1)
        << "slot 0 carries the stand-in, so the peer's anchor never reached the superblock";

    // A second checkpoint advances it rather than republishing the first -
    // the cadence's whole purpose - and it is still slot 0 that moves.
    ASSERT_TRUE(peer.value()->Checkpoint().ok());
    for (int i = 0; i < 20; ++i) {
        peer.value()->scheduler().RunOnce();
        core0.RunOnce();
    }
    EXPECT_EQ(receiver.publishes(), 3u);
    EXPECT_GE(core0_->superblock.wal_anchor(0).checkpoint_lsn, folded);
}

TEST_F(CoreRuntimePerCoreStreamTest, AMountAfterAPeersCleanStopDoesNotRereadTheRunsWholeLog) {
    // PW3b. Core 0 checkpoints at three points - the completion checkpoint
    // at mount, the cadence, and the way out - and PW3 gave a peer the first
    // two. Without the third a graceful restart replayed every peer's stream
    // from its last cadence tick (docs/inflight/known-gaps.md; the core-0 property is
    // the sim harness's AMountAfterACleanStopDoesNotRereadTheRunsWholeLog).
    //
    // The shape is Serve's tail after the worker join: Sync(), then
    // ShutdownCheckpoint through core 0's own SuperBlockCheckpointAnchor -
    // direct, with no reactor pumped on either side, which is the point
    // (remote_checkpoint_anchor.hpp's last section). The control iteration
    // stops the old way, so the test shows the gap and not only the bound.
    for (const bool clean_stop : {false, true}) {
        SCOPED_TRACE(clean_stop ? "stopped with the shutdown checkpoint"
                                : "stopped without it - the PW3 gap");
        catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                                  /*core_count=*/2);
        catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
        const std::string name = clean_stop ? "stop_clean" : "stop_gap";
        auto oid = catalog2.CreateTable(catalog::kNamespacePublic, name, TwoColumnSchema(),
                                        catalog::ClusteredType::kHeap);
        ASSERT_TRUE(oid.ok()) << oid.status().message();
        auto row = catalog2.GetSysTableRow(oid.value());
        ASSERT_TRUE(row.ok());
        ASSERT_EQ(row.value().owner_core, 1u);
        ASSERT_TRUE(core0_store_->Sync().ok());

        // Core 0's half, as Serve wires it: the ring and the receiving anchor.
        auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 64);
        ASSERT_TRUE(transport.ok()) << transport.status().message();
        sched::NullIoBackend io0;
        sched::Scheduler core0(clock_, io0);
        ASSERT_TRUE(core0.AttachTransport(&transport.value(), 0).ok());
        SuperBlockCheckpointAnchor receiver(core0_->superblock, *core0_store_);
        RegisterAnchorReceiver(core0, receiver);

        CoreRuntime::Config first_run = ConfigFor(1);
        auto peer = CoreRuntime::Open(first_run, *device_, clock_, nullptr);
        ASSERT_TRUE(peer.ok()) << peer.status().message();
        ASSERT_TRUE(peer.value()->AttachTransport(transport.value()).ok());
        // The completion checkpoint's anchor lands over the ring, as at a
        // real start - so the control's next mount starts from *that*
        // anchor, and what it re-reads is exactly this run.
        for (int i = 0; i < 20; ++i) {
            peer.value()->scheduler().RunOnce();
            core0.RunOnce();
        }
        ASSERT_EQ(receiver.publishes(), 1u);
        // **Slot 1, because this volume says per-core.**
        // `SuperBlockCheckpointAnchor::Publish` folds into slot 0 only under
        // `single_stream()`; here every core keeps its own slot and nothing
        // is a minimum over anything, so the peer's anchor is read where the
        // peer wrote it. A core-0 stand-in has nothing to contribute for the
        // same reason - there is no fold for it to hold open.
        const WalAnchorFields mount_anchor = core0_->superblock.wal_anchor(1);
        ASSERT_GT(mount_anchor.checkpoint_lsn, 0u);

        // Funded the ordinary way, then a run's worth of rows.
        peer.value()->GrantRelationFault(
            RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));
        const PageId pages[] = {row.value().desc_page_id, row.value().anchor_page_id};
        peer.value()->GrantRelationWrite(pages);
        auto first = catalog2.AllocateRowIdRange(oid.value(), 256);
        ASSERT_TRUE(first.ok());
        peer.value()->row_id_leases().Grant(oid.value(), first.value(), 256);
        txn::TrxIdSequence core0_ids(core0_->superblock);
        auto block = core0_ids.Carve(256);
        ASSERT_TRUE(block.ok());
        peer.value()->trx_id_lease().Grant(block.value().first, block.value().count);
        for (int i = 0; i < 200; ++i) {
            const auto ins = peer.value()
                                 ->dispatcher()
                                 .Dispatch("INSERT INTO " + name + " VALUES (" +
                                           std::to_string(i) + ")")
                                 .response;
            ASSERT_NE(ins.rfind("ERR", 0), 0u) << "row " << i << ": " << ins;
        }

        // Serve's tail after the join: the log sync, then - on a clean stop -
        // the shutdown checkpoint, published directly.
        ASSERT_TRUE(peer.value()->Sync().ok());
        if (clean_stop) {
            ASSERT_TRUE(peer.value()->ShutdownCheckpoint(receiver).ok());
            EXPECT_EQ(receiver.publishes(), 2u)
                << "the shutdown anchor must reach page 0 with no reactor running";
            EXPECT_GT(core0_->superblock.wal_anchor(1).checkpoint_lsn,
                      mount_anchor.checkpoint_lsn);
        } else {
            EXPECT_EQ(receiver.publishes(), 1u);  // the mount's alone
        }
        const WalAnchorFields stop_anchor = core0_->superblock.wal_anchor(1);
        peer.value().reset();

        // The restart, with the anchor Expeditor copies out of the superblock.
        CoreRuntime::Config again = ConfigFor(1);
        again.anchor = stop_anchor;
        auto reopened = CoreRuntime::Open(again, *device_, clock_, nullptr);
        ASSERT_TRUE(reopened.ok()) << reopened.status().message();

        const auto count =
            reopened.value()->dispatcher().Dispatch("SELECT COUNT(*) FROM " + name).response;
        EXPECT_NE(count.find("200"), std::string::npos) << count;
        const MountRecovery& mount = reopened.value()->recovery();
        if (clean_stop) {
            // The checkpoint's own two records and nothing to redo - the
            // bound rather than the constant, as the core-0 test asserts it.
            EXPECT_LT(mount.records, 20u)
                << "the mount re-read " << mount.records << " records after a clean stop";
            EXPECT_EQ(mount.redo_applied, 0u);
        } else {
            EXPECT_GT(mount.records, 200u) << "the control re-read only " << mount.records
                                           << " records, so it no longer shows the gap";
        }
        // And the block is where an operator reads it: this core's SHOW META.
        const auto meta = reopened.value()->dispatcher().Dispatch("SHOW META").response;
        EXPECT_NE(meta.find("recovery_records=" + std::to_string(mount.records)),
                  std::string::npos)
            << meta;
        // Whole, not half: the `_us` fields are printed only when the mount
        // supplied a clock (command_dispatcher.cpp), so this is what says a
        // peer's block is core 0's block and not a subset of it - and it is
        // the only route by which `checkpoint_ns`, timed at AttachTransport,
        // is ever read.
        EXPECT_NE(meta.find("recovery_checkpoint_us="), std::string::npos) << meta;
        // A core that ran its own pass does not carry the marker: its
        // absence is what says "these numbers are mine".
        EXPECT_EQ(meta.find("recovery_by="), std::string::npos) << meta;

        // **AR0 M0, AL-R5: the same stop, mounted as one stream.** A third
        // open of the same peer, differing only in what the volume says its
        // log is. Core 0's pass would be the whole instance's there, so this
        // core must recover nothing at all - and the clean-stop arm is what
        // makes that checkable, because the pages are already on the platter
        // and "recovered nothing" and "still serves its rows" are true at
        // once. On the crash arm they would not be, which is exactly why the
        // cutover may not precede this stage.
        if (clean_stop) {
            reopened.value().reset();

            // The instance's log, as `Expeditor::Open` builds it: one device
            // at `wal-0-*`, a stream with its latch armed, and the writer
            // every attached core asks for its syncs.
            auto shared_device = wal::FileLogDevice::Open(dir_.string(), /*core_id=*/0);
            ASSERT_TRUE(shared_device.ok()) << shared_device.status().message();
            wal::WalManagerConfig shared_config;
            shared_config.shared_stream = true;
            auto owner = wal::WalManager::Open(shared_device.value().get(), clock_,
                                               /*core_id=*/0, shared_config);
            ASSERT_TRUE(owner.ok()) << owner.status().message();
            owner.value()->StartWriter();

            CoreRuntime::Config as_one_stream = ConfigFor(1);
            as_one_stream.anchor = stop_anchor;
            // **The one place in this file that still hands a peer an image
            // its volume disagrees with, and it is deliberate.** Everywhere
            // else that was the defect AM-S0 removed; here the disagreement
            // *is* the experiment - the same volume, the same stop, differing
            // only in what the log topology says - and it is sound because
            // under `kSingleStream` this core reads no log at all, so there
            // is no per-core log content for the claim to be wrong about.
            // The assertion below is exactly that: it recovered nothing.
            SuperBlock one_stream_image = core0_->superblock;
            ASSERT_TRUE(one_stream_image.SetLogTopology(kSingleStream).ok());
            as_one_stream.superblock = &one_stream_image;
            as_one_stream.shared_stream = owner.value()->stream();
            as_one_stream.shared_writer = owner.value()->writer();
            auto single = CoreRuntime::Open(as_one_stream, *device_, clock_, nullptr);
            ASSERT_TRUE(single.ok()) << single.status().message();

            // It opened no log of its own: the manager it holds is attached
            // to core 0's, and its writes land in core 0's stream.
            EXPECT_TRUE(single.value()->wal().attached())
                << "the peer opened a second stream on a single-stream volume";
            EXPECT_EQ(single.value()->wal().stream(), owner.value()->stream());

            const MountRecovery& skipped = single.value()->recovery();
            EXPECT_EQ(skipped.records, 0u)
                << "the peer re-read a log core 0's pass already covered";
            EXPECT_EQ(skipped.redo_applied, 0u);
            EXPECT_EQ(skipped.transactions_rolled_back, 0u);

            const auto still =
                single.value()->dispatcher().Dispatch("SELECT COUNT(*) FROM " + name).response;
            EXPECT_NE(still.find("200"), std::string::npos) << still;

            // And the block still prints whole. This core measured a
            // completion checkpoint even though it recovered nothing, and a
            // `_us` field that was taken must not be hidden by the skip.
            const auto one_meta =
                single.value()->dispatcher().Dispatch("SHOW META").response;
            EXPECT_NE(one_meta.find("recovery_records=0"), std::string::npos) << one_meta;
            EXPECT_NE(one_meta.find("recovery_checkpoint_us="), std::string::npos) << one_meta;
            // And it says *why* those zeroes are zero, which is the whole
            // difference between a measurement and an absence of one.
            EXPECT_NE(one_meta.find("recovery_by=core0"), std::string::npos) << one_meta;
        }
    }
}


// A peer that is told the volume has one stream, and handed nothing to
// attach to, must refuse. Opening one of its own would write records into
// a file no mount of this volume ever replays - the failure would surface
// as lost rows after a crash, arbitrarily later.
// **The cell that would have caught the cutover's crash.** Under one stream
// a peer opens no log device of its own, and the assertion resume was still
// handed `*log_device_` - a reference formed from a null `unique_ptr`, then
// a virtual call on it. Nothing found it because the resume short-circuits
// when the catalog lists no assertions, and no test opened a single-stream
// peer that owned one.
//
// The relation is core 0's here, so the peer counts the assertion as
// foreign and adopts nothing - which is the point: the crash was in
// *reaching* the scan, before any of that was decided.
TEST_F(CoreRuntimeTest, UnderOneStreamAPeerWithAnAssertionInTheCatalogStillMounts) {
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "asserted",
                                           TwoColumnSchema(), catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(exec::InsertAssertion(core0_->catalog, *core0_store_, /*wal=*/nullptr,
                                      /*id=*/1, oid.value(), "a_bound",
                                      "CHECK COUNT(*) <= 100", kInvalidPageId)
                    .ok());
    ASSERT_TRUE(core0_store_->Sync().ok());

    // The instance's log is the fixture's since AM-S0, and `ConfigFor` hands
    // it: a second `WalManager` over the same `wal-0-0.log`, with a second
    // writer thread, was two owning managers on one file for no gain.
    CoreRuntime::Config config = ConfigFor(1);

    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    // It reached the resume and came back: the assertion is core 0's, so
    // this core counted it foreign rather than adopting it.
    EXPECT_GE(peer.value()->recovery().assertions_foreign, 1u);
}

TEST_F(CoreRuntimeTest, APeersOwnSuperblockAnswersForTheVolumeAndNotWithZeros) {
    // The AL-S9 review's finding, and then the review of the fix. A peer's
    // `superblock_` was a *default-constructed copy*, and zero is a legal
    // value of most of its fields - so a peer reported
    // `wal_topology=per-core` on a single-stream volume, and printed
    // `version=0` and two epoch timestamps beside it, under
    // `peer_listeners = on`. Wrong rather than absent, and directly against
    // `crosscore.md` CC11's "every core reads with the same authority".
    //
    // Three fields had already been carried across one at a time
    // (`next_trx_id`, the WAL anchor, `log_topology`) and the next three
    // were live. The fix is the whole image, so this cell asserts the
    // *class* is closed rather than the third instance.
    // The fixture's own log and image, for `AForeignAssertion`'s reason
    // just above: `ConfigFor` hands what `Expeditor` hands.
    CoreRuntime::Config config = ConfigFor(1);

    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    // Asserted through `SHOW META` rather than through the fields, because
    // the report is where every wrong answer reached a client.
    const std::string meta = peer.value()->dispatcher().Dispatch("SHOW META").response;
    EXPECT_NE(meta.find("wal_topology=single"), std::string::npos) << meta;
    EXPECT_EQ(meta.find("wal_anchor_count="), std::string::npos)
        << "printed only under per-core, and this volume is not: " << meta;
    // The three the review found live. `version=0` is not a version this
    // build ever wrote, and a zero `create_time` renders as the epoch - the
    // reading an operator would act on.
    EXPECT_EQ(meta.find("version=0 "), std::string::npos) << meta;
    EXPECT_EQ(meta.find("create_time=0 "), std::string::npos) << meta;
    EXPECT_EQ(meta.find("last_mount_time=0 "), std::string::npos) << meta;
    EXPECT_NE(meta.find("version=" + std::to_string(kSuperBlockVersion)), std::string::npos)
        << "a peer must answer the volume's version, not its copy's: " << meta;
}

TEST_F(CoreRuntimeTest, ACoreCannotBeOpenedWithoutTheVolumesImage) {
    // The guard that closes the class rather than the instance: three
    // separate defects came from a field nobody carried across, and nothing
    // listed which fields a peer was entitled to answer. Now there is no
    // such list, because there is no such choice.
    CoreRuntime::Config config = ConfigFor(1);
    config.superblock = nullptr;

    auto opened = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(opened.status().message().find("superblock"), std::string::npos)
        << opened.status().message();
}

TEST_F(CoreRuntimeTest, APeerWithNoStreamToAttachToRefusesRatherThanOpeningOne) {
    CoreRuntime::Config config = ConfigFor(1);
    // **Cleared on purpose, and this is now the only cell that does.**
    // Since AM-S0 `ConfigFor` hands the real stream, because the volume is
    // single-stream and `Expeditor` would; the wiring bug this cell exists
    // for is a peer handed *nothing*, so it has to take the stream back
    // out. Nothing has to rewrite the image either - the volume already
    // says one stream.
    config.shared_stream = nullptr;
    config.shared_writer = nullptr;

    auto opened = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(opened.status().message().find("one WAL stream"), std::string::npos)
        << opened.status().message();
}

TEST_F(CoreRuntimeTest, APeerAsksForRowIdsItWasNeverGrantedAndTheRetrySucceeds) {
    // PW1b. `RequestRowIdLease` had no callers, so a peer's lease table was
    // never granted anything and `AllocateRowId` answered ResourceExhausted
    // forever - the retry its own message promises could not succeed.
    //
    // The trigger could not copy PW1's: that lease is per *instance*, so a
    // peer pre-empts for it from the first tick, while a row-id lease is per
    // *relation* and has no subject until a statement names one. So the miss
    // records the demand and the refill tick answers it.
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "t", TwoColumnSchema(),
                                           catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 64);
    ASSERT_TRUE(transport.ok()) << transport.status().message();

    sched::NullIoBackend io0;
    sched::Scheduler core0(clock_, io0);
    ASSERT_TRUE(core0.AttachTransport(&transport.value(), 0).ok());
    ASSERT_TRUE(
        RegisterRowIdGrantHandler(core0, transport.value(), core0_->catalog, nullptr).ok());

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    ASSERT_TRUE(peer.value()->AttachTransport(transport.value()).ok());

    // Nothing has asked yet, so the table knows of no relation at all and the
    // tick has nothing to do.
    EXPECT_FALSE(peer.value()->row_id_leases().NeediestRelation().has_value());
    peer.value()->MaybeRefillRowIds();
    EXPECT_EQ(peer.value()->row_id_refill().stats.requests, 0u)
        << "a peer asked for ids for a relation no statement had named";

    // The first allocation fails retryably **and records the demand** - the
    // half that did not exist before PW1b.
    auto dry = peer.value()->catalog().AllocateRowId(oid.value());
    ASSERT_FALSE(dry.ok());
    EXPECT_EQ(dry.status().code(), StatusCode::kTxnConflict);
    ASSERT_TRUE(peer.value()->row_id_leases().NeediestRelation().has_value())
        << "the miss did not record which relation needs ids";
    EXPECT_EQ(*peer.value()->row_id_leases().NeediestRelation(), oid.value());

    // The tick answers it, and the retry the message promised now succeeds.
    peer.value()->MaybeRefillRowIds();
    for (int i = 0; i < 20; ++i) {
        peer.value()->scheduler().RunOnce();
        core0.RunOnce();
    }
    EXPECT_EQ(peer.value()->row_id_refill().stats.requests, 1u);
    EXPECT_EQ(peer.value()->row_id_refill().stats.grants, 1u);
    // The refill's three legs are stamped (lease_refill_stats.hpp): the
    // request's submit, the grant's arrival on this reactor, the parked
    // coroutine's completion. Real clock, so nonzero and ordered is the
    // pin; the in-flight stamps are cleared by the completion.
    {
        const auto& st = peer.value()->row_id_refill().stats;
        EXPECT_GT(st.wait_total_max_ns, 0u);
        EXPECT_GE(st.wait_total_max_ns, st.wait_to_grant_max_ns);
        EXPECT_GE(st.wait_total_max_ns, st.resume_lag_max_ns);
        EXPECT_FALSE(st.in_flight) << "the completion clears the in-flight request";
    }
    // And a peer's SHOW META prints them; core 0's never does.
    const auto meta = peer.value()->dispatcher().Dispatch("SHOW META").response;
    EXPECT_NE(meta.find("rowid_refill_requests=1 rowid_refill_grants=1"), std::string::npos)
        << meta;
    EXPECT_NE(meta.find("rowid_refill_wait_max_us="), std::string::npos) << meta;

    auto wet = peer.value()->catalog().AllocateRowId(oid.value());
    ASSERT_TRUE(wet.ok()) << wet.status().message();

    // And the relation stops being needy, so the tick does not ask again on
    // every cadence - PW1's defect, which had the same shape one lease over.
    EXPECT_FALSE(peer.value()->row_id_leases().NeediestRelation().has_value())
        << "a freshly granted relation still reads as low water";
    peer.value()->MaybeRefillRowIds();
    EXPECT_EQ(peer.value()->row_id_refill().stats.requests, 1u)
        << "the tick asked again for a relation that had just been granted a block";

    // The ids are core 0's to give, and disjoint from what core 0 issues.
    auto on_core0 = core0_->catalog.AllocateRowId(oid.value());
    ASSERT_TRUE(on_core0.ok()) << on_core0.status().message();
    EXPECT_GE(on_core0.value(), wet.value() + kRowIdLeasePerGrant)
        << "core 0's next id sits inside the block it granted the peer";
}

// RD5's ring half, which nothing else exercises: the owner core asks on
// its own tick, core 0 opens the range and hands its head page over, and
// the owner ends up able to write it (work order
// `instructions/v2.5.0/range-directory.md` RB2; `crosscore.md` CC10).
//
// This is HD5's demonstration as well - `RangeEligible` gets its first
// caller on a real relation here, and the gate that fires is the one the
// counter records.
TEST_F(CoreRuntimeTest, APeersLeaseBlockBecomesARangeItCanWrite) {
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "spread",
                                           TwoColumnSchema(), catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 64);
    ASSERT_TRUE(transport.ok()) << transport.status().message();

    sched::NullIoBackend io0;
    sched::Scheduler core0(clock_, io0);
    ASSERT_TRUE(core0.AttachTransport(&transport.value(), 0).ok());
    // The store and the enforcer are what turn the range half on at core
    // 0; without them the handler grants ids and opens nothing, which is
    // the arm every other test in this file takes.
    exec::AssertionEnforcer core0_enforcer;
    ASSERT_TRUE(RegisterRowIdGrantHandler(core0, transport.value(), core0_->catalog, nullptr,
                                          core0_store_.get(), nullptr, &core0_enforcer)
                    .ok());

    // The owner core, with ranges armed. One key sizes the grant and the
    // range, so the boundary below is this number.
    CoreRuntime::Config cfg = ConfigFor(1);
    cfg.range_size_ids = 4096;
    auto peer = CoreRuntime::Open(cfg, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    ASSERT_TRUE(peer.value()->AttachTransport(transport.value()).ok());

    // A statement names the relation, which is what records the demand.
    auto dry = peer.value()->catalog().AllocateRowId(oid.value());
    ASSERT_FALSE(dry.ok());
    ASSERT_EQ(*peer.value()->row_id_leases().NeediestRelation(), oid.value());

    peer.value()->MaybeRefillRowIds();
    for (int i = 0; i < 40; ++i) {
        peer.value()->scheduler().RunOnce();
        core0.RunOnce();
    }
    ASSERT_EQ(peer.value()->row_id_refill().stats.grants, 1u);

    // The directory core 0 wrote: the opening row plus the boundary at the
    // granted block's first id, owned by the core that asked.
    auto ranges = core0_->catalog.RangesOf(oid.value());
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    ASSERT_EQ(ranges.value().size(), 2u) << "the lease block did not become a range";
    EXPECT_EQ(ranges.value()[0].lo, 0u);
    EXPECT_EQ(ranges.value()[1].lo, peer.value()->row_id_refill().first_id);
    EXPECT_EQ(ranges.value()[1].owner_core, 1u);

    // **And the owner can write the head**, which is the half that was
    // wrong the first time this row was built: core 0 formatted the page in
    // its own frame and never flushed it, so the admission faulted an
    // unwritten id and the range had a head no core could touch.
    const PageId head = ranges.value()[1].entry_page;
    ASSERT_NE(head, kInvalidPageId);
    EXPECT_TRUE(peer.value()->store().MayWrite(head))
        << "the owner holds no write rights over its own range's head page";
    auto faulted = peer.value()->store().GetForRead(head);
    ASSERT_TRUE(faulted.ok()) << faulted.status().message();
    heap::PageView view(faulted.value().bytes());
    // Invariant 3 made structural: nothing below the boundary can land in
    // this page even by mistake.
    EXPECT_EQ(view.min_key(), ranges.value()[1].lo);

    // ---- R4/IS5: the second block opens no second boundary --------------
    //
    // The same core asking again lands in the range it already owns - ids
    // only ascend and that range runs to the end of the space - so a
    // boundary there would cut this core's own chain in two for nothing,
    // and spend a fan-in stage per lease block forever.
    for (std::uint64_t i = 0; i < cfg.range_size_ids; ++i) {
        ASSERT_TRUE(peer.value()->catalog().AllocateRowId(oid.value()).ok());
    }
    ASSERT_TRUE(peer.value()->row_id_leases().NeediestRelation().has_value())
        << "a spent block did not read as demand";
    peer.value()->MaybeRefillRowIds();
    for (int i = 0; i < 40; ++i) {
        peer.value()->scheduler().RunOnce();
        core0.RunOnce();
    }
    ASSERT_EQ(peer.value()->row_id_refill().stats.grants, 2u) << "the second block never arrived";

    auto after = core0_->catalog.RangesOf(oid.value());
    ASSERT_TRUE(after.ok()) << after.status().message();
    EXPECT_EQ(after.value().size(), 2u)
        << "a second block by the same core opened a second boundary";
}

// ---- R4/IS1: the pump ---------------------------------------------------
//
// The test above reaches around the dispatcher and calls `AllocateRowId`
// directly, which is what let RD5 land with no producer: through a
// statement, a peer never asks for a foreign relation's ids at all,
// because the write is shipped or refused first. This is that same
// arrangement driven the way a client drives it - and the assertion is on
// the *demand*, because demand is the whole of what IS1 adds.
TEST_F(CoreRuntimeTest, AForeignInsertLeavesTheDemandThatBecomesThisCoresRange) {
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "spread",
                                           TwoColumnSchema(), catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());

    // No transport, so nothing ships: the statement takes the affinity
    // refusal, which is the arm IS1 must leave byte-identical.
    CoreRuntime::Config cfg = ConfigFor(1);
    cfg.range_size_ids = 4096;
    auto peer = CoreRuntime::Open(cfg, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    ASSERT_FALSE(peer.value()->row_id_leases().NeediestRelation().has_value())
        << "the fixture started with demand already recorded";

    // The transaction-id block, without which `BeginWrite` refuses before
    // the statement is ever parsed - the refusal would look identical from
    // the wire and prove nothing about ownership.
    txn::TrxIdSequence core0_ids(core0_->superblock);
    auto block = core0_ids.Carve(16);
    ASSERT_TRUE(block.ok());
    peer.value()->trx_id_lease().Grant(block.value().first, block.value().count);

    const auto out = peer.value()->dispatcher().Dispatch("INSERT INTO spread VALUES (7)");
    EXPECT_EQ(out.response.rfind("ERR TXN_CONFLICT retryable=1 ", 0), 0u)
        << "IS1 changed the refusal a foreign write already answered: " << out.response;
    EXPECT_NE(out.response.find("core 1"), std::string::npos)
        << "the refusal was not the cross-core one: " << out.response;

    // And the half that is new: the relation is now needy on **this** core,
    // so the drain tick asks core 0, and core 0 opens the range owned by
    // whoever asked. Nothing was written to reach this state.
    ASSERT_TRUE(peer.value()->row_id_leases().NeediestRelation().has_value())
        << "a foreign INSERT left no demand behind it, so this relation stays single-writer";
    EXPECT_EQ(*peer.value()->row_id_leases().NeediestRelation(), oid.value());
}

// The off-switch, which is one key with the size (`range_alloc.hpp`): with
// no range to open, a lease block for a statement that runs on another
// core is ids burnt for nothing, so the demand is not recorded either.
TEST_F(CoreRuntimeTest, AForeignInsertLeavesNoDemandWhenRangesAreOff) {
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "unspread",
                                           TwoColumnSchema(), catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());

    CoreRuntime::Config cfg = ConfigFor(1);  // range_size_ids defaults to kRangeSizeOff
    auto peer = CoreRuntime::Open(cfg, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    txn::TrxIdSequence core0_ids(core0_->superblock);
    auto block = core0_ids.Carve(16);
    ASSERT_TRUE(block.ok());
    peer.value()->trx_id_lease().Grant(block.value().first, block.value().count);

    const auto out = peer.value()->dispatcher().Dispatch("INSERT INTO unspread VALUES (7)");
    EXPECT_EQ(out.response.rfind("ERR TXN_CONFLICT retryable=1 ", 0), 0u) << out.response;
    EXPECT_FALSE(peer.value()->row_id_leases().NeediestRelation().has_value())
        << "an unarmed instance leased a block for a statement it does not run";
}

// A row that names its own pk draws from no lease at all - it writes the
// relation's high-water mark, which is core 0's page - so leaving a demand
// for it would lease a block nothing issues from.
TEST_F(CoreRuntimeTest, AForeignInsertThatNamesItsKeyLeavesNoDemand) {
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "named", TwoColumnSchema(),
                                           catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());

    CoreRuntime::Config cfg = ConfigFor(1);
    cfg.range_size_ids = 4096;
    auto peer = CoreRuntime::Open(cfg, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    txn::TrxIdSequence core0_ids(core0_->superblock);
    auto block = core0_ids.Carve(16);
    ASSERT_TRUE(block.ok());
    peer.value()->trx_id_lease().Grant(block.value().first, block.value().count);

    const auto out = peer.value()->dispatcher().Dispatch("INSERT INTO named VALUES (1, 7)");
    EXPECT_EQ(out.response.rfind("ERR TXN_CONFLICT retryable=1 ", 0), 0u) << out.response;
    EXPECT_FALSE(peer.value()->row_id_leases().NeediestRelation().has_value());
}

// ---- R4/IS2 + IS3: the spreading itself ---------------------------------
//
// §8 test 12's core, at two cores: the same statement that was refused
// runs **here** once this core has a range, its rows land in that range's
// own chain, and nothing was shipped to do it. The whole route is
// exercised through `Dispatch`, because the class R4 closes is a statement
// going to the wrong core rather than a function answering wrongly.
TEST_F(CoreRuntimeTest, APeerInsertsIntoItsOwnRangeInsteadOfShippingToTheOwner) {
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "spread",
                                           TwoColumnSchema(), catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 64);
    ASSERT_TRUE(transport.ok()) << transport.status().message();
    sched::NullIoBackend io0;
    sched::Scheduler core0(clock_, io0);
    ASSERT_TRUE(core0.AttachTransport(&transport.value(), 0).ok());
    exec::AssertionEnforcer core0_enforcer;
    ASSERT_TRUE(RegisterRowIdGrantHandler(core0, transport.value(), core0_->catalog, nullptr,
                                          core0_store_.get(), nullptr, &core0_enforcer)
                    .ok());

    CoreRuntime::Config cfg = ConfigFor(1);
    cfg.range_size_ids = 4096;
    auto peer = CoreRuntime::Open(cfg, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    ASSERT_TRUE(peer.value()->AttachTransport(transport.value()).ok());
    txn::TrxIdSequence core0_ids(core0_->superblock);
    auto block = core0_ids.Carve(64);
    ASSERT_TRUE(block.ok());
    peer.value()->trx_id_lease().Grant(block.value().first, block.value().count);

    // The relation is core 0's, so this is refused - and leaves the demand.
    const auto before =
        peer.value()->dispatcher().Dispatch("INSERT INTO spread VALUES (7)").response;
    ASSERT_EQ(before.rfind("ERR ", 0), 0u) << before;

    peer.value()->MaybeRefillRowIds();
    for (int i = 0; i < 40; ++i) {
        peer.value()->scheduler().RunOnce();
        core0.RunOnce();
    }
    // What `Catalog::BumpVersion`'s hook does in a running instance and
    // nothing does in a fixture: flush the rows core 0 just wrote, and
    // broadcast the invalidation the peer answers by **evicting** its own
    // frames of those pages. `InvalidateFromPeer` alone is not enough and
    // that is not a fixture artefact - it drops the catalog cache but
    // leaves the peer's resident catalog page, so the re-read finds the
    // same stale bytes (`CoreRuntime::InvalidateCatalog` is the half that
    // evicts).
    FlushCatalog();
    peer.value()->InvalidateCatalog();

    auto ranges = core0_->catalog.RangesOf(oid.value());
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    ASSERT_EQ(ranges.value().size(), 2u) << "the pump did not turn the demand into a range";
    ASSERT_EQ(ranges.value()[1].owner_core, 1u);
    const std::uint64_t boundary = ranges.value()[1].lo;

    // **The same statement, and now it runs here.** The id is the range's
    // first, which is what "the block is the range" means from outside.
    const auto after =
        peer.value()->dispatcher().Dispatch("INSERT INTO spread VALUES (7)").response;
    ASSERT_EQ(after.rfind("INSERTED", 0), 0u) << after;
    EXPECT_NE(after.find("id=" + std::to_string(boundary)), std::string::npos) << after;

    // In the range's **own** chain, not the relation's - the page it named
    // is the entry page core 0 handed over, or one this core grew from it.
    const PageId head = ranges.value()[1].entry_page;
    auto walked = peer.value()->store().GetForRead(head);
    ASSERT_TRUE(walked.ok()) << walked.status().message();
    heap::PageView view(walked.value().bytes());
    EXPECT_EQ(view.min_key(), boundary) << "invariant 3 does not hold on this range's head";
    EXPECT_GT(view.slot_count(), 0u) << "the row did not land in this core's own range";

    // And the ids stay disjoint from core 0's, which is K1 across cores:
    // core 0's next issue sits above the block it granted.
    auto on_core0 = core0_->catalog.AllocateRowId(oid.value());
    ASSERT_TRUE(on_core0.ok()) << on_core0.status().message();
    EXPECT_GE(on_core0.value(), boundary + cfg.range_size_ids)
        << "core 0 issued an id inside the block it leased to core 1";
}

// ---- R4/IS6: `crosscore.md` §8 test 12, at k = 2 writers ----------------
//
// *"k cores inserting concurrently each land in their own range's tail;
// ids ascend per range; ids stay globally unique (K1's issue-once contract
// across cores); invariant 3 holds per range."* All four, over two peers
// and the relation's own owner, through `Dispatch`.
TEST_F(CoreRuntimeTest, TwoPeersEachInsertIntoTheirOwnRangesTail) {
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "fanout",
                                           TwoColumnSchema(), catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto transport = sched::RealRingTransport::Create(/*core_count=*/3, 16, 64);
    ASSERT_TRUE(transport.ok()) << transport.status().message();
    sched::NullIoBackend io0;
    sched::Scheduler core0(clock_, io0);
    ASSERT_TRUE(core0.AttachTransport(&transport.value(), 0).ok());
    exec::AssertionEnforcer core0_enforcer;
    ASSERT_TRUE(RegisterRowIdGrantHandler(core0, transport.value(), core0_->catalog, nullptr,
                                          core0_store_.get(), nullptr, &core0_enforcer)
                    .ok());
    txn::TrxIdSequence core0_ids(core0_->superblock);

    std::vector<std::unique_ptr<CoreRuntime>> peers;
    for (std::uint32_t id : {1u, 2u}) {
        CoreRuntime::Config cfg = ConfigFor(id);
        cfg.range_size_ids = 4096;
        auto peer = CoreRuntime::Open(cfg, *device_, clock_, nullptr);
        ASSERT_TRUE(peer.ok()) << peer.status().message();
        ASSERT_TRUE(peer.value()->AttachTransport(transport.value()).ok());
        auto block = core0_ids.Carve(64);
        ASSERT_TRUE(block.ok());
        peer.value()->trx_id_lease().Grant(block.value().first, block.value().count);
        peers.push_back(std::move(peer.value()));
    }

    // Round 1: both are refused and both leave a demand. **Interleaved on
    // purpose** - the two blocks are carved by one core in the order the
    // requests arrive, so the ranges alternate owners, which is the
    // arrangement §6b calls interleaved and RD7 opens one stage per.
    for (auto& peer : peers) {
        const auto out = peer->dispatcher().Dispatch("INSERT INTO fanout VALUES (1)").response;
        ASSERT_EQ(out.rfind("ERR ", 0), 0u) << out;
        peer->MaybeRefillRowIds();
    }
    for (int i = 0; i < 80; ++i) {
        for (auto& peer : peers) peer->scheduler().RunOnce();
        core0.RunOnce();
    }
    FlushCatalog();
    for (auto& peer : peers) peer->InvalidateCatalog();

    auto ranges = core0_->catalog.RangesOf(oid.value());
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    // The lo = 0 anchor CC9 requires, plus one range per writer core.
    ASSERT_EQ(ranges.value().size(), 3u) << "two writers did not produce two ranges";
    EXPECT_EQ(ranges.value()[0].lo, 0u);
    EXPECT_EQ(ranges.value()[0].owner_core, 0u);
    EXPECT_NE(ranges.value()[1].owner_core, ranges.value()[2].owner_core)
        << "both blocks were granted to one core";

    // Round 2: each runs **locally**, and the reply names an id inside that
    // core's own range.
    std::vector<std::uint64_t> issued;
    for (std::size_t k = 0; k < peers.size(); ++k) {
        for (int rep = 0; rep < 3; ++rep) {
            const auto out =
                peers[k]->dispatcher().Dispatch("INSERT INTO fanout VALUES (1)").response;
            ASSERT_EQ(out.rfind("INSERTED", 0), 0u)
                << "core " << (k + 1) << " did not insert into its own range: " << out;
            // " id=" with the space: the reply opens with `oid=`, which
            // contains `id=` and would be parsed instead.
            const std::size_t at = out.find(" id=");
            ASSERT_NE(at, std::string::npos) << out;
            issued.push_back(std::strtoull(out.c_str() + at + 4, nullptr, 10));
        }
    }

    // **Globally unique** (K1 across cores): the blocks are carved from one
    // `sys.tables.next_id`, so no two cores can name the same id.
    std::vector<std::uint64_t> sorted = issued;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end())
        << "two cores issued the same id";

    // **Ascending per range, and in each range's own chain** - which is
    // also invariant 3 per range, since the head's `min_key` is `lo`. The
    // row count is asserted and not inferred: without it the ascent runs
    // over whichever rows happen to be present, so writes landing in the
    // wrong range would leave every remaining check trivially true.
    auto each_range_ascends = [&](std::uint16_t expected_rows, const char* when) {
        for (std::size_t r = 1; r < ranges.value().size(); ++r) {
            const catalog::SysRangeRow& row = ranges.value()[r];
            auto& owner = peers[row.owner_core - 1];
            auto page = owner->store().GetForRead(row.entry_page);
            ASSERT_TRUE(page.ok()) << page.status().message();
            heap::PageView view(page.value().bytes());
            EXPECT_EQ(view.min_key(), row.lo);
            EXPECT_EQ(view.slot_count(), expected_rows)
                << "range at lo " << row.lo << " owned by core " << row.owner_core
                << " did not take its owner's rows " << when;
            std::uint64_t previous = 0;
            for (std::uint16_t slot = 0; slot < view.slot_count(); ++slot) {
                auto tuple = view.ReadTuple(slot);
                ASSERT_TRUE(tuple.ok()) << tuple.status().message();
                auto id = KeystoneIdOfPayload(tuple.value().payload);
                ASSERT_TRUE(id.ok());
                EXPECT_GE(id.value(), row.lo) << "invariant 3 broken " << when;
                EXPECT_GT(id.value(), previous) << "ids did not ascend inside one range " << when;
                previous = id.value();
            }
        }
    };
    // Round 2's three, and nothing from round 1, which was refused before
    // it wrote.
    each_range_ascends(3, "in round 2");

    // ---- RB5's second review item, answered where it is reachable ------
    //
    // **Round 3 interleaves the two writers**, which round 2 did not: it
    // ran each core's three inserts together, so the issue sequence
    // happened to ascend and nothing said whether it had to. It does not,
    // and that is R4's amendment to invariant 11 (§4.1a) as an assertion
    // rather than a sentence - each core issues from its **own leased
    // block**, carved above the mark once and never consulted again, so
    // alternating writers descend every time the turn passes back down.
    //
    // This is the falsifier `range_chain_test.cpp`'s
    // `AnOutOfOrderInsertIsRefusedSoOneCoreCannotBreakTheByteIdentity`
    // cannot construct: on one core the below-the-mark refusal forbids
    // exactly this sequence, and two leased blocks are what get past it.
    std::vector<std::uint64_t> interleaved;
    for (int rep = 0; rep < 3; ++rep) {
        for (auto& peer : peers) {
            const auto out = peer->dispatcher().Dispatch("INSERT INTO fanout VALUES (2)").response;
            ASSERT_EQ(out.rfind("INSERTED", 0), 0u) << out;
            const std::size_t at = out.find(" id=");
            ASSERT_NE(at, std::string::npos) << out;
            interleaved.push_back(std::strtoull(out.c_str() + at + 4, nullptr, 10));
        }
    }
    EXPECT_FALSE(std::is_sorted(interleaved.begin(), interleaved.end()))
        << "two interleaved writers issued an ascending sequence, so this host produced no "
           "second block and the test proves nothing";
    std::vector<std::uint64_t> unique = interleaved;
    std::sort(unique.begin(), unique.end());
    EXPECT_EQ(std::adjacent_find(unique.begin(), unique.end()), unique.end())
        << "interleaving cost uniqueness, which it must not";

    // **The ascent moved down a level, it was not given up**: round 2's
    // three rows plus round 3's three, still ascending inside each range.
    each_range_ascends(6, "after round 3's interleaving");
}

// The other arm, and the one every existing relation takes: a gated
// relation is declined, the ids arrive anyway, and the decline is readable
// from outside the process (C3, workplan §9e).
TEST_F(CoreRuntimeTest, AGatedRelationIsDeclinedAndTheDeclineIsCounted) {
    // A btree relation - D1's decline, the gate that actually fires today.
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "tree", TwoColumnSchema(),
                                           catalog::ClusteredType::kBtree);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 64);
    ASSERT_TRUE(transport.ok()) << transport.status().message();

    sched::NullIoBackend io0;
    sched::Scheduler core0(clock_, io0);
    ASSERT_TRUE(core0.AttachTransport(&transport.value(), 0).ok());
    exec::AssertionEnforcer core0_enforcer;
    ASSERT_TRUE(RegisterRowIdGrantHandler(core0, transport.value(), core0_->catalog, nullptr,
                                          core0_store_.get(), nullptr, &core0_enforcer)
                    .ok());

    CoreRuntime::Config cfg = ConfigFor(1);
    cfg.range_size_ids = 4096;
    auto peer = CoreRuntime::Open(cfg, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    ASSERT_TRUE(peer.value()->AttachTransport(transport.value()).ok());

    ASSERT_FALSE(peer.value()->catalog().AllocateRowId(oid.value()).ok());
    peer.value()->MaybeRefillRowIds();
    for (int i = 0; i < 40; ++i) {
        peer.value()->scheduler().RunOnce();
        core0.RunOnce();
    }

    // The ids arrived: a decline must not cost the statements waiting on
    // them, which is what makes it a refusal rather than a failure.
    EXPECT_EQ(peer.value()->row_id_refill().stats.grants, 1u);
    EXPECT_TRUE(peer.value()->catalog().AllocateRowId(oid.value()).ok());

    // And nothing was written: the relation is still the one range CC8
    // says it starts as.
    auto ranges = core0_->catalog.RangesOf(oid.value());
    ASSERT_TRUE(ranges.ok());
    EXPECT_TRUE(ranges.value().empty()) << "a gated relation got a directory";

    // C3's surface, from outside the process. Absent-rather-than-zeroed is
    // why the assertion is on the presence of the field, not on a `=0`.
    const auto meta = peer.value()->dispatcher().Dispatch("SHOW META").response;
    EXPECT_NE(meta.find("range_split_declines=1"), std::string::npos) << meta;
    EXPECT_NE(meta.find("range_split_decline_detail=" + std::to_string(oid.value()) + ":btree-clustered=1"),
              std::string::npos)
        << meta;
}

TEST(RowIdLeaseTableTest, AContiguousTopUpKeepsTheWindowAtTheRunInHand) {
    // PW1b review. `window` is what `low_water()` takes its quarter of, so
    // it must be the run in hand and not the sum of every run ever granted.
    // Accumulating it raised the mark by count/4 per refill, which asked for
    // the next run after only 3/4 of this one had been issued - a standing
    // 25% burn of the relation's 40-bit space, and a mark that drifts up
    // without bound.
    catalog::RowIdLeaseTable table;
    table.Grant(4000, 100, 4096);
    for (int refill = 0; refill < 8; ++refill) {
        while (!table.NeediestRelation().has_value()) {
            ASSERT_TRUE(table.Next(4000).ok());
        }
        // Topped up contiguously, exactly as core 0's sequential carve does.
        table.Grant(4000, 100 + 4096 * (refill + 1), 4096);
        ASSERT_FALSE(table.NeediestRelation().has_value())
            << "a freshly topped-up relation reads as low water";
    }
    // Eight refills in, the mark is still a fraction of *one* run rather
    // than of their sum: ask now and the relation is nearly spent, which is
    // what makes it issue almost every id it is granted. Accumulating the
    // window puts 8,192 ids behind this mark instead of 1,365.
    while (!table.NeediestRelation().has_value()) {
        ASSERT_TRUE(table.Next(4000).ok());
    }
    EXPECT_LT(table.remaining(4000), 4096u)
        << "the low-water mark drifted up with every refill, so a run is asked for again "
           "with more than a whole run still in hand";
}

TEST_F(CoreRuntimeTest, ARelationCoreZeroCannotGrantIsAskedForOnceAndStarvesNoOther) {
    // PW1b review. A carve fails for reasons that are permanent - the
    // relation has no sys.tables row, it names its own ids, or its 40-bit
    // space is gone - and core 0 answers those with a zero-count grant. The
    // entry stays spent, so it reads as low water forever: without the
    // denial the drain tick asks again every cadence, and because one
    // request is in flight per core and the neediest is the lowest low-water
    // oid, no *other* relation on that core is ever asked for again.
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "t", TwoColumnSchema(),
                                           catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_GT(oid.value(), 3000u);  // the ungrantable oid below must sort first
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 64);
    ASSERT_TRUE(transport.ok()) << transport.status().message();

    sched::NullIoBackend io0;
    sched::Scheduler core0(clock_, io0);
    ASSERT_TRUE(core0.AttachTransport(&transport.value(), 0).ok());
    ASSERT_TRUE(
        RegisterRowIdGrantHandler(core0, transport.value(), core0_->catalog, nullptr).ok());

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    ASSERT_TRUE(peer.value()->AttachTransport(transport.value()).ok());

    // Demand for a relation core 0 has no sys.tables row for - what a
    // dropped relation looks like to the lease table - and demand for a
    // real one, which sorts after it.
    EXPECT_FALSE(peer.value()->row_id_leases().Next(3000).ok());
    EXPECT_FALSE(peer.value()->catalog().AllocateRowId(oid.value()).ok());
    ASSERT_EQ(*peer.value()->row_id_leases().NeediestRelation(), 3000u);

    auto turn = [&] {
        peer.value()->MaybeRefillRowIds();
        for (int i = 0; i < 20; ++i) {
            peer.value()->scheduler().RunOnce();
            core0.RunOnce();
        }
    };

    turn();
    EXPECT_EQ(peer.value()->row_id_refill().stats.requests, 1u);
    EXPECT_EQ(peer.value()->row_id_refill().stats.grants, 0u) << "core 0 granted a relation it has no row for";
    ASSERT_TRUE(peer.value()->row_id_leases().NeediestRelation().has_value());
    EXPECT_EQ(*peer.value()->row_id_leases().NeediestRelation(), oid.value())
        << "a relation core 0 refused still counts as demand, so the tick never reaches another";

    // The next tick reaches the real relation, and the one after that asks
    // for nothing at all.
    turn();
    EXPECT_EQ(peer.value()->row_id_refill().stats.requests, 2u);
    EXPECT_EQ(peer.value()->row_id_refill().stats.grants, 1u);
    EXPECT_TRUE(peer.value()->catalog().AllocateRowId(oid.value()).ok());
    turn();
    EXPECT_EQ(peer.value()->row_id_refill().stats.requests, 2u)
        << "the tick asked again for a relation core 0 had already refused";

    // And the refusal is not permanent to a *statement*: a fresh miss is
    // fresh demand, so the retry the message promises is one that is made -
    // but the statement that meets the denial is answered without the
    // wire's bit, since the "none" was for a permanent cause.
    auto after_deny = peer.value()->row_id_leases().Next(3000);
    ASSERT_FALSE(after_deny.ok());
    EXPECT_EQ(after_deny.status().code(), StatusCode::kResourceExhausted);
    EXPECT_FALSE(after_deny.status().retryable());
    EXPECT_EQ(*peer.value()->row_id_leases().NeediestRelation(), 3000u);
}

// ---- P4c: a SELECT against a rotated relation executes remotely ---------

TEST_F(CoreRuntimeTest, ASelectAgainstARotatedRelationIsServedRemotely) {
    // The whole cross-core read path end to end, loopback transport: the
    // dispatcher compiles, sees owner_core=1, ships the step; the "remote"
    // server executes it and streams batches; the session finishes the
    // reply. Everything but the rings.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/2);
    catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "rotated", TwoColumnSchema(),
                                    catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto access = catalog2.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    for (int i = 0; i < 4; ++i) {
        auto id = catalog2.AllocateRowId(oid.value());
        ASSERT_TRUE(id.ok());
        parser::AstValue v;
        v.type = parser::ValueType::kInt;
        v.int_val = i * 10;
        v.raw_int_text = std::to_string(i * 10);
        auto payload = exec::EncodeRow(access.value()->schema, access.value()->layout,
                                       id.value(), {v});
        ASSERT_TRUE(payload.ok());
        auto placed = heap::ChainInsert(*core0_store_, access.value()->desc_page_id,
                                        id.value(), payload.value(), 1, access.value()->oid);
        ASSERT_TRUE(placed.ok());
    }
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto row = catalog2.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    ASSERT_EQ(row.value().owner_core, 1u);

    // The session core's runtime. Its store is lease-bound, so schema
    // resolution needs CC7's grant exactly as a real session core would
    // have received at the relation's publish.
    auto runtime = CoreRuntime::Open(ConfigFor(0), *device_, clock_, nullptr);
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();
    runtime.value()->GrantRelationFault(
        RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));

    // The loopback pair: the "owner" executes over the fixture's
    // unrestricted store; sends cross-deliver in process.
    std::optional<RemoteStepServer> server;
    std::optional<SessionStepClient> client;
    server.emplace(
        catalog2, *core0_store_, /*core_id=*/1,
        StepSendSeam{[&](std::uint32_t, sched::RingMessageKind kind,
                         std::vector<std::byte> payload) {
            switch (kind) {
                case sched::RingMessageKind::kStepBatch: client->OnStepBatch(payload); break;
                case sched::RingMessageKind::kStepEof: client->OnStepEof(payload); break;
                case sched::RingMessageKind::kStepError: client->OnStepError(payload); break;
                default: ADD_FAILURE() << "unexpected server send";
            }
            return Status::OK();
        }});
    client.emplace(
        /*core_id=*/0,
        [&](std::uint32_t, sched::RingMessageKind kind, std::vector<std::byte> payload) {
            sched::MessageHeader h{};
            h.src_core = 0;
            h.dst_core = 1;
            switch (kind) {
                case sched::RingMessageKind::kStepOpen: server->OnStepOpen(h, payload); break;
                case sched::RingMessageKind::kStepCredit: server->OnStepCredit(payload); break;
                case sched::RingMessageKind::kStepCancel: server->OnStepCancel(payload); break;
                default: ADD_FAILURE() << "unexpected client send";
            }
            return Status::OK();
        });
    runtime.value()->dispatcher().SetRemoteReads(&*client);

    auto out = runtime.value()->dispatcher().Dispatch("SELECT * FROM rotated");
    EXPECT_EQ(out.response,
              "id,v\\n1,0\\n2,10\\n3,20\\n4,30");
    EXPECT_EQ(client->open_reads(), 0u);

    // The ineligible shapes keep the refusal: a projection list is not
    // shipped in P4c and answers the affinity refusal, never wrong rows.
    auto refused = runtime.value()->dispatcher().Dispatch("SELECT v FROM rotated");
    EXPECT_EQ(refused.response.rfind("ERR ", 0), 0u);
}

// ---- RD7: a read of a split relation fans in over its owners ------------

TEST_F(CoreRuntimeTest, ASelectAgainstARelationSplitAcrossTwoCoresFansInInRangeOrder) {
    // RB4's substance, and the first statement in this engine to consume
    // more than one producer. The directory is written by hand, and since
    // R4 that is a **fixture choice rather than a necessity**: insert
    // spreading now produces a second owner through the ordinary route
    // (`TwoPeersEachInsertIntoTheirOwnRangesTail` drives it), and this
    // test keeps the hand-written split because what it is about is the
    // fan-in's ordering, which wants a boundary at a known id and rows
    // placed on both sides of it without a lease block's arithmetic in
    // the way.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/3);
    catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "split", TwoColumnSchema(),
                                    catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    // The cut: [0, 4096) stays with the relation's owner, [4096, end)
    // goes to another core. **The other core is read off the relation, not
    // written down**: rotation counts the relations that already exist, so
    // a bootstrap relation added or removed moves which core `CreateTable`
    // picks - which is exactly what withdrawing `sys.pattern_defs` did on
    // 2026-08-31, and what a hard-coded 2 was silently depending on.
    auto owner = catalog2.InitTableAccess(oid.value());
    ASSERT_TRUE(owner.ok()) << owner.status().message();
    const std::uint32_t other_core = owner.value()->owner_core == 1 ? 2 : 1;

    auto upper_head = catalog2.CreateRangeEntryPage(oid.value(), 4096);
    ASSERT_TRUE(upper_head.ok()) << upper_head.status().message();
    ASSERT_TRUE(catalog2.OpenRangeRows(oid.value(), 4096, other_core, upper_head.value()).ok());

    auto ranges = catalog2.RangesOf(oid.value());
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    ASSERT_EQ(ranges.value().size(), 2u);
    ASSERT_NE(ranges.value()[0].owner_core, ranges.value()[1].owner_core)
        << "the fixture did not produce a two-owner relation";

    // Two rows in each range's own chain, placed directly: what RB4 is
    // being tested on is the read, and RB3 already owns the write half.
    auto access = catalog2.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    auto place = [&](std::uint64_t id, std::int64_t v, PageId head) {
        parser::AstValue value;
        value.type = parser::ValueType::kInt;
        value.int_val = v;
        value.raw_int_text = std::to_string(v);
        auto payload = exec::EncodeRow(access.value()->schema, access.value()->layout, id,
                                       {value});
        ASSERT_TRUE(payload.ok());
        ASSERT_TRUE(heap::ChainInsert(*core0_store_, head, id, payload.value(), 1,
                                      access.value()->oid)
                        .ok());
    };
    place(1, 10, ranges.value()[0].entry_page);
    place(2, 20, ranges.value()[0].entry_page);
    place(4096, 30, ranges.value()[1].entry_page);
    place(4097, 40, ranges.value()[1].entry_page);
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto row = catalog2.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    auto runtime = CoreRuntime::Open(ConfigFor(0), *device_, clock_, nullptr);
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();
    runtime.value()->GrantRelationFault(
        RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));

    // One server per owner core, and the client routes by the core the
    // stage names - which is what a fan-in is, and what the single-server
    // loopback above could not express.
    std::optional<SessionStepClient> client;
    auto make_seam = [&] {
        return StepSendSeam{[&](std::uint32_t, sched::RingMessageKind kind,
                                std::vector<std::byte> payload) {
            switch (kind) {
                case sched::RingMessageKind::kStepBatch: client->OnStepBatch(payload); break;
                case sched::RingMessageKind::kStepEof: client->OnStepEof(payload); break;
                case sched::RingMessageKind::kStepError: client->OnStepError(payload); break;
                default: ADD_FAILURE() << "unexpected server send";
            }
            return Status::OK();
        }};
    };
    //
    // **One catalog per core, which is what production has**: a stage
    // walks the ranges *it* owns (`TableAccess::WalkHeadsFor`), and the
    // executor learns which core it is on from the catalog it was built
    // with. Sharing one catalog across two servers would make both stages
    // believe they were core 0 and answer nothing - the fixture would be
    // lying about the one fact the fan-in turns on.
    catalog::Catalog catalog_lo(*core0_store_, storage::kDefaultInlineCellWidth,
                                /*core_count=*/3, ranges.value()[0].owner_core);
    catalog::Catalog catalog_hi(*core0_store_, storage::kDefaultInlineCellWidth,
                                /*core_count=*/3, ranges.value()[1].owner_core);
    std::optional<RemoteStepServer> owner_lo;
    std::optional<RemoteStepServer> owner_hi;
    owner_lo.emplace(catalog_lo, *core0_store_, ranges.value()[0].owner_core, make_seam());
    owner_hi.emplace(catalog_hi, *core0_store_, ranges.value()[1].owner_core, make_seam());
    client.emplace(
        /*core_id=*/0,
        [&](std::uint32_t dst, sched::RingMessageKind kind, std::vector<std::byte> payload) {
            RemoteStepServer& to =
                dst == ranges.value()[0].owner_core ? *owner_lo : *owner_hi;
            sched::MessageHeader h{};
            h.src_core = 0;
            h.dst_core = dst;
            switch (kind) {
                case sched::RingMessageKind::kStepOpen: to.OnStepOpen(h, payload); break;
                case sched::RingMessageKind::kStepCredit: to.OnStepCredit(payload); break;
                case sched::RingMessageKind::kStepCancel: to.OnStepCancel(payload); break;
                default: ADD_FAILURE() << "unexpected client send";
            }
            return Status::OK();
        });
    runtime.value()->dispatcher().SetRemoteReads(&*client);

    // **Range order, and every range.** A fan-in that dropped a sibling
    // would answer two rows and report success - §3's silent discard,
    // which the plural `InputEdge` exists to end - and one that
    // concatenated out of order would disagree with the local walk over
    // the same rows.
    auto out = runtime.value()->dispatcher().Dispatch("SELECT * FROM split");
    EXPECT_EQ(out.response, "id,v\\n1,10\\n2,20\\n4096,30\\n4097,40");
    EXPECT_EQ(client->open_reads(), 0u) << "a fan-in left a stage open";
}

TEST_F(CoreRuntimeTest, ASelectAgainstARelationWithARangeOnAnotherCoreIsRefusedNotAnsweredShort) {
    // The other half of RD7's ownership question, and the one that decides
    // a *wrong answer* rather than a route. `sys.tables.owner_core` names
    // one core; a range of the same relation may name another. The walk
    // covers the ranges this core owns and no others
    // (`TableAccess::WalkHeadsFor`), so a read that runs locally over a
    // relation owned here but not *wholly* here returns the rows of one
    // range and reports success - two rows where four exist, with nothing
    // logged. The fan-in cannot take this statement, because its gate is
    // "somebody else owns the relation" and here nobody else does.
    //
    // A refusal is therefore the only honest ending, and this test is what
    // says so: an answer of any kind here is the defect.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/3);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "half_here", TwoColumnSchema(),
                                    catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    auto upper_head = catalog2.CreateRangeEntryPage(oid.value(), 4096);
    ASSERT_TRUE(upper_head.ok()) << upper_head.status().message();
    ASSERT_TRUE(catalog2.OpenRangeRows(oid.value(), 4096, /*owner_core=*/2, upper_head.value())
                    .ok());

    auto ranges = catalog2.RangesOf(oid.value());
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    ASSERT_EQ(ranges.value().size(), 2u);
    // The default placement is the creating core, so the relation is core
    // 0's - which is exactly what keeps the fan-in from firing.
    ASSERT_EQ(ranges.value()[0].owner_core, 0u);
    ASSERT_EQ(ranges.value()[1].owner_core, 2u);

    auto access = catalog2.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    auto place = [&](std::uint64_t id, std::int64_t v, PageId head) {
        parser::AstValue value;
        value.type = parser::ValueType::kInt;
        value.int_val = v;
        value.raw_int_text = std::to_string(v);
        auto payload = exec::EncodeRow(access.value()->schema, access.value()->layout, id,
                                       {value});
        ASSERT_TRUE(payload.ok());
        ASSERT_TRUE(heap::ChainInsert(*core0_store_, head, id, payload.value(), 1,
                                      access.value()->oid)
                        .ok());
    };
    place(1, 10, ranges.value()[0].entry_page);
    place(4096, 30, ranges.value()[1].entry_page);
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto row = catalog2.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    auto runtime = CoreRuntime::Open(ConfigFor(0), *device_, clock_, nullptr);
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();
    runtime.value()->GrantRelationFault(
        RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));

    auto out = runtime.value()->dispatcher().Dispatch("SELECT * FROM half_here");
    EXPECT_EQ(out.response.rfind("ERR ", 0), 0u) << out.response;
    EXPECT_NE(out.response.find("answer short"), std::string::npos) << out.response;
}

TEST_F(CoreRuntimeTest, AFanInOverInterleavedOwnershipStillAnswersInRangeOrder) {
    // **The case grouping by owner core gets wrong**, and the reason RB4's
    // stages are per contiguous *run*: ownership A,B,A emits A's two runs
    // adjacent if the fan-in groups by core, so the answer comes back
    // `1,2, 8192,8193, 4096,4097` where the same rows unsplit on one core
    // read `1,2, 4096,4097, 8192,8193`. §8 test 9 asks for byte-identical,
    // and interleaving is not a corner case - it is what R4's
    // id-block-aligned spreading produces (`crosscore.md` §6b).
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/3);
    catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "woven", TwoColumnSchema(),
                                    catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    // [0,4096) -> the relation's owner, [4096,8192) -> another core,
    // [8192, end) -> back to the owner. Two runs on one core. Both cores
    // are read off the relation rather than written down, for the reason
    // the two-owner test above states: rotation counts existing relations,
    // so a bootstrap relation removed moves which core CreateTable picks.
    auto owner = catalog2.InitTableAccess(oid.value());
    ASSERT_TRUE(owner.ok()) << owner.status().message();
    const std::uint32_t own_core = owner.value()->owner_core;
    const std::uint32_t other_core = own_core == 1 ? 2 : 1;
    for (auto [lo, owner] : std::vector<std::pair<std::uint64_t, std::uint32_t>>{
             {4096, other_core}, {8192, own_core}}) {
        auto head = catalog2.CreateRangeEntryPage(oid.value(), lo);
        ASSERT_TRUE(head.ok()) << head.status().message();
        ASSERT_TRUE(catalog2.OpenRangeRows(oid.value(), lo, owner, head.value()).ok());
    }
    auto ranges = catalog2.RangesOf(oid.value());
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    ASSERT_EQ(ranges.value().size(), 3u);
    ASSERT_EQ(ranges.value()[0].owner_core, ranges.value()[2].owner_core)
        << "the fixture did not interleave ownership";
    ASSERT_NE(ranges.value()[0].owner_core, ranges.value()[1].owner_core);

    auto access = catalog2.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    auto place = [&](std::uint64_t id, std::int64_t v, PageId head) {
        parser::AstValue value;
        value.type = parser::ValueType::kInt;
        value.int_val = v;
        value.raw_int_text = std::to_string(v);
        auto payload = exec::EncodeRow(access.value()->schema, access.value()->layout, id,
                                       {value});
        ASSERT_TRUE(payload.ok());
        ASSERT_TRUE(heap::ChainInsert(*core0_store_, head, id, payload.value(), 1,
                                      access.value()->oid)
                        .ok());
    };
    place(1, 10, ranges.value()[0].entry_page);
    place(4096, 20, ranges.value()[1].entry_page);
    place(8192, 30, ranges.value()[2].entry_page);
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto row = catalog2.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    auto runtime = CoreRuntime::Open(ConfigFor(0), *device_, clock_, nullptr);
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();
    runtime.value()->GrantRelationFault(
        RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));

    std::optional<SessionStepClient> client;
    auto make_seam = [&] {
        return StepSendSeam{[&](std::uint32_t, sched::RingMessageKind kind,
                                std::vector<std::byte> payload) {
            switch (kind) {
                case sched::RingMessageKind::kStepBatch: client->OnStepBatch(payload); break;
                case sched::RingMessageKind::kStepEof: client->OnStepEof(payload); break;
                case sched::RingMessageKind::kStepError: client->OnStepError(payload); break;
                default: ADD_FAILURE() << "unexpected server send";
            }
            return Status::OK();
        }};
    };
    const std::uint32_t core_a = ranges.value()[0].owner_core;
    const std::uint32_t core_b = ranges.value()[1].owner_core;
    catalog::Catalog catalog_a(*core0_store_, storage::kDefaultInlineCellWidth, 3, core_a);
    catalog::Catalog catalog_b(*core0_store_, storage::kDefaultInlineCellWidth, 3, core_b);
    std::optional<RemoteStepServer> server_a;
    std::optional<RemoteStepServer> server_b;
    server_a.emplace(catalog_a, *core0_store_, core_a, make_seam());
    server_b.emplace(catalog_b, *core0_store_, core_b, make_seam());
    client.emplace(
        /*core_id=*/0,
        [&](std::uint32_t dst, sched::RingMessageKind kind, std::vector<std::byte> payload) {
            RemoteStepServer& to = dst == core_a ? *server_a : *server_b;
            sched::MessageHeader h{};
            h.src_core = 0;
            h.dst_core = dst;
            switch (kind) {
                case sched::RingMessageKind::kStepOpen: to.OnStepOpen(h, payload); break;
                case sched::RingMessageKind::kStepCredit: to.OnStepCredit(payload); break;
                case sched::RingMessageKind::kStepCancel: to.OnStepCancel(payload); break;
                default: ADD_FAILURE() << "unexpected client send";
            }
            return Status::OK();
        });
    runtime.value()->dispatcher().SetRemoteReads(&*client);

    // Three stages, not two: core A's two runs are two stages, each
    // walking its own span. Grouping by core would give two stages and
    // this order wrong; a stage without its span would give core A's rows
    // twice.
    auto out = runtime.value()->dispatcher().Dispatch("SELECT * FROM woven");
    EXPECT_EQ(out.response, "id,v\\n1,10\\n4096,20\\n8192,30");
    EXPECT_EQ(client->open_reads(), 0u) << "a fan-in left a stage open";
}

TEST_F(CoreRuntimeTest, AFanInWiderThanTheCeilingIsRefusedRatherThanAnsweredShort) {
    // **The ceiling as a refusal, which is the half that had no test.** The
    // wire's own guard cannot be it: the STEP_OPEN upstream count is one
    // byte, so since DA3 raised `kMaxFanInUpstreams` to 255 the byte cannot
    // spell a count above the ceiling and the decoder's check is
    // unreachable. The ceiling that binds is the *dispatcher's stage
    // count*, which is not a wire quantity at all - each stage is its own
    // pipeline carrying zero upstreams - and refusing there is what keeps a
    // relation too wide to fan in from being read as the stages that did
    // fit.
    //
    // One range past the ceiling, alternating owners so every range is its
    // own run (`crosscore.md` section 6b's interleave, which is what
    // spreading produces).
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/3);
    catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "toowide", TwoColumnSchema(),
                                    catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    auto owner = catalog2.InitTableAccess(oid.value());
    ASSERT_TRUE(owner.ok()) << owner.status().message();
    const std::uint32_t own_core = owner.value()->owner_core;
    const std::uint32_t other_core = own_core == 1 ? 2 : 1;

    // The lo = 0 range exists implicitly and is the owner's, so opening
    // `kMaxFanInUpstreams` more with strictly alternating owners leaves
    // `kMaxFanInUpstreams + 1` runs - one past the ceiling, and the
    // smallest relation that is.
    for (std::size_t i = 1; i <= kMaxFanInUpstreams; ++i) {
        const std::uint64_t lo = static_cast<std::uint64_t>(i) * 4096;
        const std::uint32_t core = (i % 2 == 1) ? other_core : own_core;
        auto head = catalog2.CreateRangeEntryPage(oid.value(), lo);
        ASSERT_TRUE(head.ok()) << head.status().message();
        ASSERT_TRUE(catalog2.OpenRangeRows(oid.value(), lo, core, head.value()).ok());
    }
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto ranges = catalog2.RangesOf(oid.value());
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    ASSERT_EQ(ranges.value().size(), kMaxFanInUpstreams + 1);

    auto row = catalog2.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    auto runtime = CoreRuntime::Open(ConfigFor(0), *device_, clock_, nullptr);
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();
    runtime.value()->GrantRelationFault(
        RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));

    // A client must exist for the route to be considered at all; nothing is
    // ever sent through it, because the refusal is decided before the first
    // stage opens - which is the property under test.
    SessionStepClient client(
        /*core_id=*/0, [](std::uint32_t, sched::RingMessageKind, std::vector<std::byte>) {
            ADD_FAILURE() << "a refused fan-in opened a stage";
            return Status::OK();
        });
    runtime.value()->dispatcher().SetRemoteReads(&client);

    auto out = runtime.value()->dispatcher().Dispatch("SELECT * FROM toowide");
    EXPECT_EQ(out.response.rfind("ERR ", 0), 0u) << out.response;
    EXPECT_NE(out.response.find("above the fan-in ceiling of"), std::string::npos)
        << out.response;
    EXPECT_NE(out.response.find(std::to_string(kMaxFanInUpstreams)), std::string::npos)
        << out.response;
    EXPECT_EQ(client.open_reads(), 0u) << "a refused fan-in left a stage open";
}

// ---- R4-R/RR5: the equivalence case that did not exist until RR1 --------
//
// **A relation the reading core *owns* but does not wholly hold**, which
// is what `placement = creating` produces the moment R4's spreading opens
// a peer's range - and which, before RR1, no core could read in any shape
// (`bench/v2.6.0/` §6a measured that at 395 rows). The route's predicate
// asked *"is this relation someone else's"*; it now asks *"can a local
// walk serve this"*, and this is the case where the two answers differ.
//
// Byte-identical against the same rows unsplit, **straddling the
// boundary**, which is RB5's discipline: every defect this line has found
// returned a right answer for data that stayed on one side of the cut.
TEST_F(CoreRuntimeTest, ACoreReadsARelationItOwnsButDoesNotWhollyHold) {
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/3);
    // **`creating`, not `rotate`** - the arrangement spreading produces and
    // the one the old predicate could not serve: core 0 owns both
    // relations, and one of them has a range elsewhere.
    auto split = catalog2.CreateTable(catalog::kNamespacePublic, "held", TwoColumnSchema(),
                                      catalog::ClusteredType::kHeap);
    ASSERT_TRUE(split.ok()) << split.status().message();
    auto whole = catalog2.CreateTable(catalog::kNamespacePublic, "wholly", TwoColumnSchema(),
                                      catalog::ClusteredType::kHeap);
    ASSERT_TRUE(whole.ok()) << whole.status().message();
    ASSERT_EQ(catalog2.InitTableAccess(split.value()).value()->owner_core, 0u)
        << "the fixture did not place both relations on core 0";

    // One cut, and the range above it is **core 2's** - so core 0 owns the
    // relation and holds only the range below.
    auto head = catalog2.CreateRangeEntryPage(split.value(), 4096);
    ASSERT_TRUE(head.ok()) << head.status().message();
    ASSERT_TRUE(catalog2.OpenRangeRows(split.value(), 4096, /*owner_core=*/2, head.value()).ok());
    auto ranges = catalog2.RangesOf(split.value());
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    ASSERT_EQ(ranges.value().size(), 2u);
    ASSERT_EQ(ranges.value()[0].owner_core, 0u);
    ASSERT_EQ(ranges.value()[1].owner_core, 2u) << "the fixture did not put a range elsewhere";

    auto place = [&](catalog::Oid oid, std::uint64_t id, std::int64_t v, PageId chain) {
        auto access = catalog2.InitTableAccess(oid);
        ASSERT_TRUE(access.ok());
        parser::AstValue value;
        value.type = parser::ValueType::kInt;
        value.int_val = v;
        value.raw_int_text = std::to_string(v);
        auto payload =
            exec::EncodeRow(access.value()->schema, access.value()->layout, id, {value});
        ASSERT_TRUE(payload.ok());
        ASSERT_TRUE(heap::ChainInsert(*core0_store_, chain, id, payload.value(), 1, oid).ok());
    };
    // **Rows on both sides**, and the same rows in the same order into the
    // unsplit twin, so the two replies differ in the split and nothing else.
    const PageId whole_head = catalog2.GetSysTableRow(whole.value()).value().desc_page_id;
    for (auto [id, v] : std::vector<std::pair<std::uint64_t, std::int64_t>>{
             {1, 10}, {2, 20}, {4096, 30}, {4097, 40}}) {
        place(split.value(), id, v,
              id < 4096 ? ranges.value()[0].entry_page : ranges.value()[1].entry_page);
        place(whole.value(), id, v, whole_head);
    }
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto runtime = CoreRuntime::Open(ConfigFor(0), *device_, clock_, nullptr);
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();
    for (catalog::Oid oid : {split.value(), whole.value()}) {
        runtime.value()->GrantRelationFault(RelationFaultExtentOf(
            catalog2.GetSysTableRow(oid).value(), storage::kDefaultExtentPages));
    }

    std::optional<SessionStepClient> client;
    std::optional<RemoteStepServer> server_0;
    std::optional<RemoteStepServer> server_2;
    auto make_seam = [&] {
        return StepSendSeam{[&](std::uint32_t, sched::RingMessageKind kind,
                                std::vector<std::byte> payload) {
            switch (kind) {
                case sched::RingMessageKind::kStepBatch: client->OnStepBatch(payload); break;
                case sched::RingMessageKind::kStepEof: client->OnStepEof(payload); break;
                case sched::RingMessageKind::kStepError: client->OnStepError(payload); break;
                default: ADD_FAILURE() << "unexpected server send";
            }
            return Status::OK();
        }};
    };
    catalog::Catalog catalog_0(*core0_store_, storage::kDefaultInlineCellWidth, 3, 0);
    catalog::Catalog catalog_2(*core0_store_, storage::kDefaultInlineCellWidth, 3, 2);
    server_0.emplace(catalog_0, *core0_store_, 0, make_seam());
    server_2.emplace(catalog_2, *core0_store_, 2, make_seam());
    client.emplace(
        /*core_id=*/0,
        [&](std::uint32_t dst, sched::RingMessageKind kind, std::vector<std::byte> payload) {
            // **A stage directed at core 0 itself is served here**, which is
            // the self-directed stage RR0 answered: the same protocol, a
            // self-send, and no second path
            // (`workplan-insert-spreading.md` §10a).
            RemoteStepServer& to = dst == 0 ? *server_0 : *server_2;
            sched::MessageHeader h{};
            h.src_core = 0;
            h.dst_core = dst;
            switch (kind) {
                case sched::RingMessageKind::kStepOpen: to.OnStepOpen(h, payload); break;
                case sched::RingMessageKind::kStepCredit: to.OnStepCredit(payload); break;
                case sched::RingMessageKind::kStepCancel: to.OnStepCancel(payload); break;
                default: ADD_FAILURE() << "unexpected client send";
            }
            return Status::OK();
        });
    runtime.value()->dispatcher().SetRemoteReads(&*client);

    const std::string split_reply =
        runtime.value()->dispatcher().Dispatch("SELECT * FROM held").response;
    const std::string whole_reply =
        runtime.value()->dispatcher().Dispatch("SELECT * FROM wholly").response;
    ASSERT_EQ(split_reply.rfind("ERR", 0), std::string::npos)
        << "a relation this core owns but does not wholly hold was refused: " << split_reply;
    // A star reply's header carries column names and not the relation's, so
    // these compare whole rather than modulo a substitution.
    EXPECT_EQ(split_reply, whole_reply) << "the split changed the answer";
    EXPECT_EQ(split_reply, "id,v\\n1,10\\n2,20\\n4096,30\\n4097,40");
    EXPECT_EQ(client->open_reads(), 0u) << "a fan-in left a stage open";

    // **The straddle, and the half that would pass on one side alone.** A
    // predicate matching rows in both ranges is what a walk stopping at the
    // boundary answers short.
    const std::string straddle =
        runtime.value()->dispatcher().Dispatch("SELECT * FROM held WHERE v > 15").response;
    EXPECT_EQ(
        straddle,
        runtime.value()->dispatcher().Dispatch("SELECT * FROM wholly WHERE v > 15").response);
    EXPECT_EQ(straddle, "id,v\\n2,20\\n4096,30\\n4097,40");
}

// ---- RS5: the same equivalence, from a **non-zero** core ----------------
//
// RR5's case above reads from core 0, which is where the fan-in client has
// always lived. RR2 put a client on every core, and **that is the case no
// unit test covered**: a peer session opening a fan-in of its own, one
// stage of which is *self-directed* - core 2 asking core 2 - and one of
// which is remote.
//
// The distinction is not cosmetic. Before RR2 a peer had no client at all,
// so this read fell through to statement shipping and was answered (or
// lost) by core 0; the route exercised here did not exist. RB5's
// discipline applies to it unchanged: byte-identical against the same rows
// unsplit, **straddling the boundary**, because every defect this line has
// found returned a right answer for data on one side of the cut.
//
// Vacuity matrix (`workplan-insert-spreading.md` §11): reverting
// `ServableBy` to `owner_core == core_id_` alone, re-pinning the route to
// `owner_core != core_id_`, and dropping the peer's client each fail this
// test - which is what makes it a gate rather than a second spelling of
// RR5's.
TEST_F(CoreRuntimeTest, APeerReadsASpreadRelationThroughItsOwnFanIn) {
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/3);
    // `creating` again: core 0 owns both relations. The reader below is
    // core **2**, which owns neither - and holds one range of one of them.
    auto split = catalog2.CreateTable(catalog::kNamespacePublic, "spread", TwoColumnSchema(),
                                      catalog::ClusteredType::kHeap);
    ASSERT_TRUE(split.ok()) << split.status().message();
    auto whole = catalog2.CreateTable(catalog::kNamespacePublic, "twin", TwoColumnSchema(),
                                      catalog::ClusteredType::kHeap);
    ASSERT_TRUE(whole.ok()) << whole.status().message();
    ASSERT_EQ(catalog2.InitTableAccess(split.value()).value()->owner_core, 0u);

    auto head = catalog2.CreateRangeEntryPage(split.value(), 4096);
    ASSERT_TRUE(head.ok()) << head.status().message();
    ASSERT_TRUE(catalog2.OpenRangeRows(split.value(), 4096, /*owner_core=*/2, head.value()).ok());
    auto ranges = catalog2.RangesOf(split.value());
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    ASSERT_EQ(ranges.value().size(), 2u);
    ASSERT_EQ(ranges.value()[0].owner_core, 0u) << "the low range must be the owner's";
    ASSERT_EQ(ranges.value()[1].owner_core, 2u) << "the high range must be the reader's own";

    auto place = [&](catalog::Oid oid, std::uint64_t id, std::int64_t v, PageId chain) {
        auto access = catalog2.InitTableAccess(oid);
        ASSERT_TRUE(access.ok());
        parser::AstValue value;
        value.type = parser::ValueType::kInt;
        value.int_val = v;
        value.raw_int_text = std::to_string(v);
        auto payload =
            exec::EncodeRow(access.value()->schema, access.value()->layout, id, {value});
        ASSERT_TRUE(payload.ok());
        ASSERT_TRUE(heap::ChainInsert(*core0_store_, chain, id, payload.value(), 1, oid).ok());
    };
    const PageId whole_head = catalog2.GetSysTableRow(whole.value()).value().desc_page_id;
    for (auto [id, v] : std::vector<std::pair<std::uint64_t, std::int64_t>>{
             {1, 10}, {2, 20}, {4096, 30}, {4097, 40}}) {
        place(split.value(), id, v,
              id < 4096 ? ranges.value()[0].entry_page : ranges.value()[1].entry_page);
        place(whole.value(), id, v, whole_head);
    }
    ASSERT_TRUE(core0_store_->Sync().ok());

    // **The reader is core 2.** Everything below is RR5's rig with that one
    // substitution, which is the whole of what RS5 adds.
    CoreRuntime::Config peer_config = ConfigFor(2);
    peer_config.core_count = 3;
    auto runtime = CoreRuntime::Open(peer_config, *device_, clock_, nullptr);
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();
    for (catalog::Oid oid : {split.value(), whole.value()}) {
        runtime.value()->GrantRelationFault(RelationFaultExtentOf(
            catalog2.GetSysTableRow(oid).value(), storage::kDefaultExtentPages));
    }

    std::optional<SessionStepClient> client;
    std::optional<RemoteStepServer> server_0;
    std::optional<RemoteStepServer> server_2;
    auto make_seam = [&] {
        return StepSendSeam{[&](std::uint32_t, sched::RingMessageKind kind,
                                std::vector<std::byte> payload) {
            switch (kind) {
                case sched::RingMessageKind::kStepBatch: client->OnStepBatch(payload); break;
                case sched::RingMessageKind::kStepEof: client->OnStepEof(payload); break;
                case sched::RingMessageKind::kStepError: client->OnStepError(payload); break;
                default: ADD_FAILURE() << "unexpected server send";
            }
            return Status::OK();
        }};
    };
    catalog::Catalog catalog_0(*core0_store_, storage::kDefaultInlineCellWidth, 3, 0);
    catalog::Catalog catalog_2(*core0_store_, storage::kDefaultInlineCellWidth, 3, 2);
    server_0.emplace(catalog_0, *core0_store_, 0, make_seam());
    server_2.emplace(catalog_2, *core0_store_, 2, make_seam());
    // Counted, because "the peer opened a fan-in" and "the peer walked the
    // whole thing locally" produce the same rows and only one of them is
    // this test's subject.
    int opens_to_self = 0;
    int opens_to_owner = 0;
    client.emplace(
        /*core_id=*/2,
        [&](std::uint32_t dst, sched::RingMessageKind kind, std::vector<std::byte> payload) {
            RemoteStepServer& to = dst == 0 ? *server_0 : *server_2;
            sched::MessageHeader h{};
            h.src_core = 2;
            h.dst_core = dst;
            switch (kind) {
                case sched::RingMessageKind::kStepOpen:
                    (dst == 2 ? opens_to_self : opens_to_owner)++;
                    to.OnStepOpen(h, payload);
                    break;
                case sched::RingMessageKind::kStepCredit: to.OnStepCredit(payload); break;
                case sched::RingMessageKind::kStepCancel: to.OnStepCancel(payload); break;
                default: ADD_FAILURE() << "unexpected client send";
            }
            return Status::OK();
        });
    runtime.value()->dispatcher().SetRemoteReads(&*client);

    const std::string split_reply =
        runtime.value()->dispatcher().Dispatch("SELECT * FROM spread").response;
    const std::string whole_reply =
        runtime.value()->dispatcher().Dispatch("SELECT * FROM twin").response;
    ASSERT_EQ(split_reply.rfind("ERR", 0), std::string::npos)
        << "a peer could not read a relation one of whose ranges is its own: " << split_reply;
    EXPECT_EQ(split_reply, whole_reply) << "the split changed the answer";
    EXPECT_EQ(split_reply, "id,v\\n1,10\\n2,20\\n4096,30\\n4097,40");
    EXPECT_EQ(client->open_reads(), 0u) << "a fan-in left a stage open";
    // One stage to the owner for the split relation's low range, one to
    // itself for its own - and one to the owner for the unsplit twin, which
    // a peer also reaches only through the pipeline. The self-directed one
    // is on a **non-zero** core, which is the mechanism RS5 gates.
    EXPECT_EQ(opens_to_owner, 2) << "the owner's range and the whole twin are both remote stages";
    EXPECT_EQ(opens_to_self, 1) << "the peer's own range was not walked by a self-directed stage";

    const std::string straddle =
        runtime.value()->dispatcher().Dispatch("SELECT * FROM spread WHERE v > 15").response;
    EXPECT_EQ(
        straddle,
        runtime.value()->dispatcher().Dispatch("SELECT * FROM twin WHERE v > 15").response);
    EXPECT_EQ(straddle, "id,v\\n2,20\\n4096,30\\n4097,40");
}

// **And the wiring, which the loopback rig above cannot gate.** That test
// hands the dispatcher a client it built itself, so it would pass on a
// tree where `CoreRuntime` never constructs one - which is exactly the
// state every peer was in before RR2, and the state that made a spread
// relation unreadable from a peer in any shape.
//
// So this one asserts the thing the rig assumes: a peer's **own**
// dispatcher, on a peer's **own** transport, *plans* a fan-in instead of
// refusing.
//
// **The evidence is which refusal comes back**, because the synchronous
// `Dispatch` cannot finish a fan-in either way - it has no reactor to run
// the other side on, so it closes every stage it opened and answers
// `TxnConflict("remote read needs the reactor path")`. That refusal is
// reached only *after* the route has resolved the ranges and opened the
// stages, so it is proof the plan was made. Without RR2's
// `remote_reads_.emplace(...)` and the `SetRemoteReads` that follows it,
// the route is skipped entirely and the answer is `CheckReadAffinity`'s
// `NotImplemented` - `CrossCoreReadNotImplemented`'s "relation 'spread2' is owned
// by core 0 and this statement is running on core 1", which is what every
// peer answered before RR2. **Not** the "cannot fan in over them" arm: that
// one is reached only when `owner_core == core_id_`, and this fixture reads
// from a core that owns nothing, so asserting its absence would assert
// nothing. Completing the read is the test above's subject; reaching the
// route is this one's.
TEST_F(CoreRuntimeTest, APeersOwnDispatcherPlansAFanInRatherThanRefusing) {
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/2);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "spread2", TwoColumnSchema(),
                                    catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_EQ(catalog2.InitTableAccess(oid.value()).value()->owner_core, 0u);
    auto head = catalog2.CreateRangeEntryPage(oid.value(), 4096);
    ASSERT_TRUE(head.ok()) << head.status().message();
    ASSERT_TRUE(catalog2.OpenRangeRows(oid.value(), 4096, /*owner_core=*/1, head.value()).ok());
    ASSERT_TRUE(core0_store_->Sync().ok());

    // `max_payload` wide enough for a STEP_OPEN: the seam refuses an
    // oversize message rather than truncating it, and that refusal would
    // fall through to the very affinity error this test distinguishes
    // itself from - a pass and a fail arriving as the same reply.
    auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 8192);
    ASSERT_TRUE(transport.ok()) << transport.status().message();

    CoreRuntime::Config cfg = ConfigFor(1);
    cfg.core_count = 2;
    auto peer = CoreRuntime::Open(cfg, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    ASSERT_TRUE(peer.value()->AttachTransport(transport.value()).ok());

    const DispatchOutcome out = peer.value()->dispatcher().Dispatch("SELECT * FROM spread2");
    EXPECT_EQ(out.response.find("is owned by core 0"), std::string::npos)
        << "the peer took CheckReadAffinity's refusal, so its dispatcher has no fan-in "
           "client: " << out.response;
    EXPECT_NE(out.response.find("remote read needs the reactor path"), std::string::npos)
        << "the peer did not open a fan-in at all: " << out.response;
}

// ---- R4-A/AG3: the fold and the projection over a spread relation -------
//
// RR1 and RR2 made a spread relation *readable*; what they made readable
// was one shape, `SELECT *` with an optional WHERE. Every aggregate and
// every projection stayed refused by `HandleSelect`'s shape gate, which is
// what stopped scenario 2's booking transaction dead: its capacity read is
// `SELECT SUM(cbm) ... WHERE operation_id = ?`, in the hot path and not in
// a verification pass (`workplan-insert-spreading.md` §12a).
//
// The widening is on the session side alone - a stage ships the same whole
// rows, filtered by the same residual - so **the oracle is the local walk**:
// `wholly` is this core's entirely and is folded by `RunAggregated` off a
// `ChainFrame` the executor filled, while `held` is folded by
// `FinishRemoteReads` off a `ChainFrame` filled from the wire. A difference
// between the two is the defect this test exists to catch.
//
// **Straddling the boundary**, per RB5's discipline, and with duplicate
// group keys on both sides of the cut: `GROUP BY` emits in first-seen order
// (AG6), so a fan-in that concatenated its stages in any order but range
// order would answer the same *groups* in a different order - a wrong reply
// that every per-group assertion would still pass.
TEST_F(CoreRuntimeTest, AFoldAndAProjectionOverASpreadRelationAnswerAsTheUnsplitTwinDoes) {
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/3);
    auto split = catalog2.CreateTable(catalog::kNamespacePublic, "held", TwoColumnSchema(),
                                      catalog::ClusteredType::kHeap);
    ASSERT_TRUE(split.ok()) << split.status().message();
    auto whole = catalog2.CreateTable(catalog::kNamespacePublic, "wholly", TwoColumnSchema(),
                                      catalog::ClusteredType::kHeap);
    ASSERT_TRUE(whole.ok()) << whole.status().message();
    ASSERT_EQ(catalog2.InitTableAccess(split.value()).value()->owner_core, 0u);

    auto head = catalog2.CreateRangeEntryPage(split.value(), 4096);
    ASSERT_TRUE(head.ok()) << head.status().message();
    ASSERT_TRUE(catalog2.OpenRangeRows(split.value(), 4096, /*owner_core=*/2, head.value()).ok());
    auto ranges = catalog2.RangesOf(split.value());
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    ASSERT_EQ(ranges.value().size(), 2u);

    auto place = [&](catalog::Oid oid, std::uint64_t id, std::int64_t v, PageId chain) {
        auto access = catalog2.InitTableAccess(oid);
        ASSERT_TRUE(access.ok());
        parser::AstValue value;
        value.type = parser::ValueType::kInt;
        value.int_val = v;
        value.raw_int_text = std::to_string(v);
        auto payload =
            exec::EncodeRow(access.value()->schema, access.value()->layout, id, {value});
        ASSERT_TRUE(payload.ok());
        ASSERT_TRUE(heap::ChainInsert(*core0_store_, chain, id, payload.value(), 1, oid).ok());
    };
    // `10` appears once on each side of the cut, so a group founded in the
    // low range is folded into again from the high one.
    const PageId whole_head = catalog2.GetSysTableRow(whole.value()).value().desc_page_id;
    for (auto [id, v] : std::vector<std::pair<std::uint64_t, std::int64_t>>{
             {1, 10}, {2, 20}, {4096, 10}, {4097, 40}}) {
        place(split.value(), id, v,
              id < 4096 ? ranges.value()[0].entry_page : ranges.value()[1].entry_page);
        place(whole.value(), id, v, whole_head);
    }
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto runtime = CoreRuntime::Open(ConfigFor(0), *device_, clock_, nullptr);
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();
    for (catalog::Oid oid : {split.value(), whole.value()}) {
        runtime.value()->GrantRelationFault(RelationFaultExtentOf(
            catalog2.GetSysTableRow(oid).value(), storage::kDefaultExtentPages));
    }

    std::optional<SessionStepClient> client;
    std::optional<RemoteStepServer> server_0;
    std::optional<RemoteStepServer> server_2;
    auto make_seam = [&] {
        return StepSendSeam{[&](std::uint32_t, sched::RingMessageKind kind,
                                std::vector<std::byte> payload) {
            switch (kind) {
                case sched::RingMessageKind::kStepBatch: client->OnStepBatch(payload); break;
                case sched::RingMessageKind::kStepEof: client->OnStepEof(payload); break;
                case sched::RingMessageKind::kStepError: client->OnStepError(payload); break;
                default: ADD_FAILURE() << "unexpected server send";
            }
            return Status::OK();
        }};
    };
    catalog::Catalog catalog_0(*core0_store_, storage::kDefaultInlineCellWidth, 3, 0);
    catalog::Catalog catalog_2(*core0_store_, storage::kDefaultInlineCellWidth, 3, 2);
    server_0.emplace(catalog_0, *core0_store_, 0, make_seam());
    server_2.emplace(catalog_2, *core0_store_, 2, make_seam());
    client.emplace(
        /*core_id=*/0,
        [&](std::uint32_t dst, sched::RingMessageKind kind, std::vector<std::byte> payload) {
            RemoteStepServer& to = dst == 0 ? *server_0 : *server_2;
            sched::MessageHeader h{};
            h.src_core = 0;
            h.dst_core = dst;
            switch (kind) {
                case sched::RingMessageKind::kStepOpen: to.OnStepOpen(h, payload); break;
                case sched::RingMessageKind::kStepCredit: to.OnStepCredit(payload); break;
                case sched::RingMessageKind::kStepCancel: to.OnStepCancel(payload); break;
                default: ADD_FAILURE() << "unexpected client send";
            }
            return Status::OK();
        });
    runtime.value()->dispatcher().SetRemoteReads(&*client);

    // Each shape twice: over the split relation through the fan-in, and
    // over the unsplit twin through the local walk. The expected text is
    // pinned as well as compared, so a change that broke *both* paths the
    // same way would not read as agreement.
    struct Case {
        const char* shape;
        const char* expected;
    };
    for (const Case& c : std::vector<Case>{
             {"SELECT v FROM %s", "v\\n10\\n20\\n10\\n40"},
             {"SELECT v FROM %s WHERE v > 15", "v\\n20\\n40"},
             {"SELECT COUNT(*) FROM %s", "count(*)\\n4"},
             {"SELECT SUM(v) FROM %s", "sum(v)\\n80"},
             {"SELECT SUM(v) FROM %s WHERE v > 15", "sum(v)\\n60"},
             {"SELECT v, COUNT(*) FROM %s GROUP BY v", "v,count(*)\\n10,2\\n20,1\\n40,1"},
             {"SELECT COUNT(*), MIN(v), MAX(v) FROM %s",
              "count(*),min(v),max(v)\\n4,10,40"}}) {
        const std::string shape(c.shape);
        const std::size_t at = shape.find("%s");
        ASSERT_NE(at, std::string::npos);
        const std::string on_split = shape.substr(0, at) + "held" + shape.substr(at + 2);
        const std::string on_whole = shape.substr(0, at) + "wholly" + shape.substr(at + 2);

        const std::string split_reply =
            runtime.value()->dispatcher().Dispatch(on_split).response;
        const std::string whole_reply =
            runtime.value()->dispatcher().Dispatch(on_whole).response;
        ASSERT_NE(split_reply.rfind("ERR", 0), 0u)
            << "the fan-in refused `" << on_split << "`: " << split_reply;
        EXPECT_EQ(split_reply, whole_reply)
            << "the split changed the answer to `" << on_split << "`";
        EXPECT_EQ(split_reply, c.expected) << "`" << on_split << "`";
        EXPECT_EQ(client->open_reads(), 0u) << "a fan-in left a stage open";
    }
}

// ---- AG3's routing rule: one owner keeps the ship path -------------------
//
// A widened shape takes the fan-in **only when no single core can answer
// the statement whole**. One stage means one owner holds every range, and
// such a statement ships as text to that owner, which folds it there and
// sends back the fold's one row instead of every row it read; pulling the
// rows here to fold them would be the same answer over the whole
// relation's worth of wire.
//
// Read from a **peer**, because that is where the two cases sit side by
// side: `spread` has a range of its own and a range of the owner's - two
// stages, one of them self-directed - while `twin` is the owner's
// entirely. The rig has no statement-ship service wired, so `twin`'s
// widened read ends at the affinity refusal, which is exactly the evidence
// wanted: the route declined it, and the count of opens says so.
TEST_F(CoreRuntimeTest, AWidenedShapeOverASingleOwnerRelationIsNotFannedIn) {
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/3);
    auto split = catalog2.CreateTable(catalog::kNamespacePublic, "spread", TwoColumnSchema(),
                                      catalog::ClusteredType::kHeap);
    ASSERT_TRUE(split.ok()) << split.status().message();
    auto whole = catalog2.CreateTable(catalog::kNamespacePublic, "twin", TwoColumnSchema(),
                                      catalog::ClusteredType::kHeap);
    ASSERT_TRUE(whole.ok()) << whole.status().message();

    auto head = catalog2.CreateRangeEntryPage(split.value(), 4096);
    ASSERT_TRUE(head.ok()) << head.status().message();
    ASSERT_TRUE(catalog2.OpenRangeRows(split.value(), 4096, /*owner_core=*/2, head.value()).ok());
    auto ranges = catalog2.RangesOf(split.value());
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    ASSERT_EQ(ranges.value().size(), 2u);

    auto place = [&](catalog::Oid oid, std::uint64_t id, std::int64_t v, PageId chain) {
        auto access = catalog2.InitTableAccess(oid);
        ASSERT_TRUE(access.ok());
        parser::AstValue value;
        value.type = parser::ValueType::kInt;
        value.int_val = v;
        value.raw_int_text = std::to_string(v);
        auto payload =
            exec::EncodeRow(access.value()->schema, access.value()->layout, id, {value});
        ASSERT_TRUE(payload.ok());
        ASSERT_TRUE(heap::ChainInsert(*core0_store_, chain, id, payload.value(), 1, oid).ok());
    };
    const PageId whole_head = catalog2.GetSysTableRow(whole.value()).value().desc_page_id;
    for (auto [id, v] : std::vector<std::pair<std::uint64_t, std::int64_t>>{
             {1, 10}, {2, 20}, {4096, 10}, {4097, 40}}) {
        place(split.value(), id, v,
              id < 4096 ? ranges.value()[0].entry_page : ranges.value()[1].entry_page);
        place(whole.value(), id, v, whole_head);
    }
    ASSERT_TRUE(core0_store_->Sync().ok());

    CoreRuntime::Config peer_config = ConfigFor(2);
    peer_config.core_count = 3;
    auto runtime = CoreRuntime::Open(peer_config, *device_, clock_, nullptr);
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();
    for (catalog::Oid oid : {split.value(), whole.value()}) {
        runtime.value()->GrantRelationFault(RelationFaultExtentOf(
            catalog2.GetSysTableRow(oid).value(), storage::kDefaultExtentPages));
    }

    std::optional<SessionStepClient> client;
    std::optional<RemoteStepServer> server_0;
    std::optional<RemoteStepServer> server_2;
    auto make_seam = [&] {
        return StepSendSeam{[&](std::uint32_t, sched::RingMessageKind kind,
                                std::vector<std::byte> payload) {
            switch (kind) {
                case sched::RingMessageKind::kStepBatch: client->OnStepBatch(payload); break;
                case sched::RingMessageKind::kStepEof: client->OnStepEof(payload); break;
                case sched::RingMessageKind::kStepError: client->OnStepError(payload); break;
                default: ADD_FAILURE() << "unexpected server send";
            }
            return Status::OK();
        }};
    };
    catalog::Catalog catalog_0(*core0_store_, storage::kDefaultInlineCellWidth, 3, 0);
    catalog::Catalog catalog_2(*core0_store_, storage::kDefaultInlineCellWidth, 3, 2);
    server_0.emplace(catalog_0, *core0_store_, 0, make_seam());
    server_2.emplace(catalog_2, *core0_store_, 2, make_seam());
    int opens_to_self = 0;
    int opens_to_owner = 0;
    client.emplace(
        /*core_id=*/2,
        [&](std::uint32_t dst, sched::RingMessageKind kind, std::vector<std::byte> payload) {
            RemoteStepServer& to = dst == 0 ? *server_0 : *server_2;
            sched::MessageHeader h{};
            h.src_core = 2;
            h.dst_core = dst;
            switch (kind) {
                case sched::RingMessageKind::kStepOpen:
                    (dst == 2 ? opens_to_self : opens_to_owner)++;
                    to.OnStepOpen(h, payload);
                    break;
                case sched::RingMessageKind::kStepCredit: to.OnStepCredit(payload); break;
                case sched::RingMessageKind::kStepCancel: to.OnStepCancel(payload); break;
                default: ADD_FAILURE() << "unexpected client send";
            }
            return Status::OK();
        });
    runtime.value()->dispatcher().SetRemoteReads(&*client);

    // Two owners: the fan-in answers it, one stage to each, the peer's own
    // being the self-directed one.
    const std::string folded =
        runtime.value()->dispatcher().Dispatch("SELECT SUM(v) FROM spread").response;
    EXPECT_EQ(folded, "sum(v)\\n80") << folded;
    EXPECT_EQ(opens_to_owner, 1);
    EXPECT_EQ(opens_to_self, 1) << "the peer's own range was not walked by a self-directed stage";
    EXPECT_EQ(client->open_reads(), 0u) << "a fan-in left a stage open";

    // One owner: no stage is opened at all. The refusal that comes back is
    // the affinity one, because this rig ships nothing - what is asserted
    // is that the route declined, not what the ship path would have said.
    const std::string not_fanned =
        runtime.value()->dispatcher().Dispatch("SELECT SUM(v) FROM twin").response;
    EXPECT_EQ(opens_to_owner, 1) << "a single-owner fold was fanned in: " << not_fanned;
    EXPECT_EQ(opens_to_self, 1);
    EXPECT_EQ(not_fanned.rfind("ERR", 0), 0u) << not_fanned;

    // And the star read over that same single-owner relation still fans in,
    // untouched: P4c's routing is not what this rule narrows.
    const std::string star = runtime.value()->dispatcher().Dispatch("SELECT * FROM twin").response;
    EXPECT_EQ(star, "id,v\\n1,10\\n2,20\\n4096,10\\n4097,40") << star;
    EXPECT_EQ(opens_to_owner, 2) << "the star read stopped taking the fan-in";
}

// ---- P4d-4b-3: a two-step join executes as a cross-core pipeline ---------

TEST_F(CoreRuntimeTest, ATwoStepJoinAgainstRotatedRelationsIsServedAsAPipeline) {
    // The engine's first multi-step cross-core statement, end to end in
    // loopback: the dispatcher compiles a scan-feeding-probe join, plans
    // the edge, ships the chained open; the "remote" core opens the
    // consuming stage, forwards the enclosed leaf open to itself
    // (self-sends are the same protocol), streams the join under credit;
    // the session's typed decode renders the projected reply.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/2);
    catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);

    auto make_schema = [&](const char* second) {
        catalog::Schema schema;
        catalog::SysColumnRow id{};
        id.pos = 0;
        catalog::SetName(id.name, "id");
        id.type_val = catalog::kTypeValInt64;
        id.len = 8;
        id.notnull = true;
        catalog::SysColumnRow other{};
        other.pos = 1;
        catalog::SetName(other.name, second);
        other.type_val = catalog::kTypeValInt64;
        other.len = 8;
        other.notnull = true;
        schema.columns = {id, other};
        return schema;
    };
    auto outer_oid = catalog2.CreateTable(catalog::kNamespacePublic, "ta", make_schema("b_id"),
                                          catalog::ClusteredType::kHeap);
    ASSERT_TRUE(outer_oid.ok()) << outer_oid.status().message();
    auto inner_oid = catalog2.CreateTable(catalog::kNamespacePublic, "tb", make_schema("qty"),
                                          catalog::ClusteredType::kHeap);
    ASSERT_TRUE(inner_oid.ok()) << inner_oid.status().message();

    auto insert = [&](catalog::Oid oid, std::int64_t second) {
        auto access = catalog2.InitTableAccess(oid);
        ASSERT_TRUE(access.ok());
        auto id = catalog2.AllocateRowId(oid);
        ASSERT_TRUE(id.ok());
        parser::AstValue v;
        v.type = parser::ValueType::kInt;
        v.int_val = second;
        v.raw_int_text = std::to_string(second);
        auto payload = exec::EncodeRow(access.value()->schema, access.value()->layout,
                                       id.value(), {v});
        ASSERT_TRUE(payload.ok());
        auto placed = heap::ChainInsert(*core0_store_, access.value()->desc_page_id,
                                        id.value(), payload.value(), 1, access.value()->oid);
        ASSERT_TRUE(placed.ok()) << placed.status().message();
    };
    // ta: (1, b_id=2) (2, b_id=1) (3, b_id=9 -> miss) (4, b_id=3);
    // tb: (1, 100) (2, 200) (3, 300).
    insert(outer_oid.value(), 2);
    insert(outer_oid.value(), 1);
    insert(outer_oid.value(), 9);
    insert(outer_oid.value(), 3);
    insert(inner_oid.value(), 100);
    insert(inner_oid.value(), 200);
    insert(inner_oid.value(), 300);
    ASSERT_TRUE(core0_store_->Sync().ok());

    // Rotation at core_count=2 places every relation on core 1: both
    // stages of the pipeline live on one peer, which is exactly the
    // stage-to-stage self-send shape.
    auto outer_row = catalog2.GetSysTableRow(outer_oid.value());
    auto inner_row = catalog2.GetSysTableRow(inner_oid.value());
    ASSERT_TRUE(outer_row.ok());
    ASSERT_TRUE(inner_row.ok());
    ASSERT_EQ(outer_row.value().owner_core, 1u);
    ASSERT_EQ(inner_row.value().owner_core, 1u);

    auto runtime = CoreRuntime::Open(ConfigFor(0), *device_, clock_, nullptr);
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();
    runtime.value()->GrantRelationFault(
        RelationFaultExtentOf(outer_row.value(), storage::kDefaultExtentPages));
    runtime.value()->GrantRelationFault(
        RelationFaultExtentOf(inner_row.value(), storage::kDefaultExtentPages));

    // The loopback pair, streaming this time: a consuming stage needs a
    // reactor, so the server's tasks land in `tasks` and Pump() is one
    // reactor pass. Sends route by destination core - core 1's traffic
    // (the chained forward, the leaf's batches to its consumer, credits
    // and cancels between the stages) re-enters the server itself.
    std::optional<RemoteStepServer> server;
    std::optional<SessionStepClient> client;
    std::vector<std::unique_ptr<sched::Task>> tasks;
    auto pump = [&] {
        for (auto& task : tasks) {
            if (task != nullptr && task->Poll() == sched::PollResult::kDone) task.reset();
        }
        std::erase(tasks, nullptr);
    };
    auto deliver = [&](std::uint32_t dst, sched::RingMessageKind kind,
                       std::vector<std::byte> payload) {
        if (dst == 1) {
            sched::MessageHeader h{};
            h.src_core = 1;
            h.dst_core = 1;
            switch (kind) {
                case sched::RingMessageKind::kStepOpen: server->OnStepOpen(h, payload); break;
                case sched::RingMessageKind::kStepCredit: server->OnStepCredit(payload); break;
                case sched::RingMessageKind::kStepBatch: server->OnStepBatch(payload); break;
                case sched::RingMessageKind::kStepEof: server->OnStepEof(payload); break;
                case sched::RingMessageKind::kStepCancel: server->OnStepCancel(payload); break;
                default: ADD_FAILURE() << "unexpected kind to core 1";
            }
            return Status::OK();
        }
        switch (kind) {
            case sched::RingMessageKind::kStepBatch: client->OnStepBatch(payload); break;
            case sched::RingMessageKind::kStepEof: client->OnStepEof(payload); break;
            case sched::RingMessageKind::kStepError: client->OnStepError(payload); break;
            default: ADD_FAILURE() << "unexpected kind to core 0";
        }
        return Status::OK();
    };
    server.emplace(catalog2, *core0_store_, /*core_id=*/1, StepSendSeam{deliver}, nullptr,
                   /*batch_target_bytes=*/1,
                   [&](std::unique_ptr<sched::Task> task) { tasks.push_back(std::move(task)); });
    client.emplace(/*core_id=*/0, deliver);
    runtime.value()->dispatcher().SetRemoteReads(&*client);

    // The statement path itself parks on the read, so it runs as the
    // coroutine the reactor would poll, interleaved with the server's
    // producer and consumer tasks.
    DispatchOutcome out;
    auto statement = sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        runtime.value()->dispatcher().DispatchAsync(
            "SELECT a.id, b.qty FROM ta AS a JOIN tb AS b ON b.id = a.b_id", nullptr, &out));
    int rounds = 0;
    while (statement->Poll() != sched::PollResult::kDone) {
        pump();
        ASSERT_LT(++rounds, 64) << "the pipeline did not converge";
    }

    // The joined rows, typed-decoded and rendered by the session: outer
    // walk order, the miss dropped, headings the chain's own - the
    // qualified spelling a local join answers with.
    EXPECT_EQ(out.response, "a.id,b.qty\\n1,200\\n2,100\\n4,300");
    EXPECT_EQ(client->open_reads(), 0u);
    EXPECT_EQ(server->open_pipelines(), 0u);
    EXPECT_TRUE(tasks.empty());
}

// ---- P4e: the pipeline's reply is the local reply, byte for byte --------

TEST_F(CoreRuntimeTest, EveryShippableShapeAnswersExactlyWhatLocalExecutionAnswers) {
    // The equivalence pass (workplan P4e). **One dataset, two
    // dispatchers differing only in `core_id`**: the relations are owned
    // by core 1, so a dispatcher that calls itself core 1 runs every
    // statement locally, and one that calls itself core 0 ships it. Both
    // read the same pages through the same catalog, so any difference
    // between the two replies is the pipeline's doing and nothing else.
    // That is a stronger claim than an expected-string test, which can
    // only be as right as the string somebody typed.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/2);
    catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);

    auto make_schema = [&](const char* second) {
        catalog::Schema schema;
        catalog::SysColumnRow id{};
        id.pos = 0;
        catalog::SetName(id.name, "id");
        id.type_val = catalog::kTypeValInt64;
        id.len = 8;
        id.notnull = true;
        catalog::SysColumnRow other{};
        other.pos = 1;
        catalog::SetName(other.name, second);
        other.type_val = catalog::kTypeValInt64;
        other.len = 8;
        other.notnull = true;
        schema.columns = {id, other};
        return schema;
    };
    auto outer_oid = catalog2.CreateTable(catalog::kNamespacePublic, "ta", make_schema("b_id"),
                                          catalog::ClusteredType::kHeap);
    ASSERT_TRUE(outer_oid.ok()) << outer_oid.status().message();
    auto inner_oid = catalog2.CreateTable(catalog::kNamespacePublic, "tb", make_schema("qty"),
                                          catalog::ClusteredType::kHeap);
    ASSERT_TRUE(inner_oid.ok()) << inner_oid.status().message();
    // A third relation whose *non-pk* column overlaps `ta.b_id`, so a join
    // on it matches real rows. Without the overlap the non-pk cases below
    // would compare two empty answers and prove nothing.
    auto tag_oid = catalog2.CreateTable(catalog::kNamespacePublic, "tc", make_schema("tag"),
                                        catalog::ClusteredType::kHeap);
    ASSERT_TRUE(tag_oid.ok()) << tag_oid.status().message();

    auto insert = [&](catalog::Oid oid, std::int64_t second) {
        auto access = catalog2.InitTableAccess(oid);
        ASSERT_TRUE(access.ok());
        auto id = catalog2.AllocateRowId(oid);
        ASSERT_TRUE(id.ok());
        parser::AstValue v;
        v.type = parser::ValueType::kInt;
        v.int_val = second;
        v.raw_int_text = std::to_string(second);
        auto payload = exec::EncodeRow(access.value()->schema, access.value()->layout,
                                       id.value(), {v});
        ASSERT_TRUE(payload.ok());
        auto placed = heap::ChainInsert(*core0_store_, access.value()->desc_page_id,
                                        id.value(), payload.value(), 1, access.value()->oid);
        ASSERT_TRUE(placed.ok()) << placed.status().message();
    };
    // Deliberately includes a key that matches nothing (b_id=9) and a
    // duplicated key (two outer rows probing tb 1), so the comparison
    // covers a miss and a fan-in rather than only clean one-to-one rows.
    for (std::int64_t b_id : {2, 1, 9, 3, 1}) insert(outer_oid.value(), b_id);
    for (std::int64_t qty : {100, 200, 300}) insert(inner_oid.value(), qty);
    // tc.tag: 2 matches two outer rows, 1 matches two, 5 matches none -
    // so the non-pk join covers fan-out on both sides and a dead value.
    for (std::int64_t tag : {2, 1, 5, 2}) insert(tag_oid.value(), tag);
    ASSERT_TRUE(core0_store_->Sync().ok());

    ASSERT_EQ(catalog2.GetSysTableRow(outer_oid.value()).value().owner_core, 1u);
    ASSERT_EQ(catalog2.GetSysTableRow(inner_oid.value()).value().owner_core, 1u);
    ASSERT_EQ(catalog2.GetSysTableRow(tag_oid.value()).value().owner_core, 1u);

    // The local side: core 1 owns both relations, so this dispatcher's
    // affinity check passes and nothing is shipped.
    CommandDispatcher local(core0_->superblock, catalog2, *core0_store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr,
                            wal::DurabilityClass::kGroup, exec::Budget(),
                            /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, /*txn=*/nullptr,
                            txn::IsolationLevel::kReadCommitted, /*core_id=*/1);

    // The pipeline side: core 0 owns nothing here, so every eligible
    // shape ships to the loopback server standing in for core 1.
    CommandDispatcher session(core0_->superblock, catalog2, *core0_store_, /*log=*/nullptr,
                              /*clock=*/nullptr, /*wal=*/nullptr,
                              wal::DurabilityClass::kGroup, exec::Budget(),
                              /*recorder=*/nullptr, /*replay_enabled=*/false,
                              /*access_statistics=*/false, /*cabins=*/nullptr, /*txn=*/nullptr,
                              txn::IsolationLevel::kReadCommitted, /*core_id=*/0);

    std::optional<RemoteStepServer> server;
    std::optional<SessionStepClient> client;
    std::vector<std::unique_ptr<sched::Task>> tasks;
    // Counts the stages actually opened on the far core. Without it this
    // test could degrade into comparing two local runs and still pass -
    // the one way an equivalence test lies.
    int stages_opened = 0;
    auto deliver = [&](std::uint32_t dst, sched::RingMessageKind kind,
                       std::vector<std::byte> payload) {
        if (dst == 1) {
            sched::MessageHeader h{};
            h.src_core = 1;
            h.dst_core = 1;
            switch (kind) {
                case sched::RingMessageKind::kStepOpen:
                    ++stages_opened;
                    server->OnStepOpen(h, payload);
                    break;
                case sched::RingMessageKind::kStepCredit: server->OnStepCredit(payload); break;
                case sched::RingMessageKind::kStepBatch: server->OnStepBatch(payload); break;
                case sched::RingMessageKind::kStepEof: server->OnStepEof(payload); break;
                case sched::RingMessageKind::kStepCancel: server->OnStepCancel(payload); break;
                default: ADD_FAILURE() << "unexpected kind to core 1";
            }
            return Status::OK();
        }
        switch (kind) {
            case sched::RingMessageKind::kStepBatch: client->OnStepBatch(payload); break;
            case sched::RingMessageKind::kStepEof: client->OnStepEof(payload); break;
            case sched::RingMessageKind::kStepError: client->OnStepError(payload); break;
            default: ADD_FAILURE() << "unexpected kind to core 0";
        }
        return Status::OK();
    };
    server.emplace(catalog2, *core0_store_, /*core_id=*/1, StepSendSeam{deliver}, nullptr,
                   /*batch_target_bytes=*/1,
                   [&](std::unique_ptr<sched::Task> task) { tasks.push_back(std::move(task)); });
    client.emplace(/*core_id=*/0, deliver);
    session.SetRemoteReads(&*client);

    // Runs one statement through the pipeline, pumping the reactor the
    // stages park on. A tiny batch target (1 row) means every shape
    // crosses the credit gate several times, so the comparison exercises
    // the parked path rather than a single flush.
    auto shipped = [&](const std::string& sql) {
        DispatchOutcome out;
        const int opened_before = stages_opened;
        auto statement = sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground,
            session.DispatchAsync(sql, nullptr, &out));
        int rounds = 0;
        while (statement->Poll() != sched::PollResult::kDone) {
            for (auto& task : tasks) {
                if (task != nullptr && task->Poll() == sched::PollResult::kDone) task.reset();
            }
            std::erase(tasks, nullptr);
            EXPECT_LT(++rounds, 256) << "the pipeline did not converge: " << sql;
            if (rounds >= 256) break;
        }
        EXPECT_GT(stages_opened, opened_before)
            << "nothing was shipped, so this compared two local runs: " << sql;
        EXPECT_EQ(client->open_reads(), 0u) << sql;
        EXPECT_EQ(server->open_pipelines(), 0u) << sql;
        EXPECT_TRUE(tasks.empty()) << sql;
        return out.response;
    };

    for (const std::string& sql : {
             // The P4c shape: a single-step star read.
             std::string("SELECT * FROM ta"),
             std::string("SELECT * FROM tb"),
             // The 4b-3 shape: scan feeding probe, projected.
             std::string("SELECT a.id, b.qty FROM ta AS a JOIN tb AS b ON b.id = a.b_id"),
             // Projection order reversed, and the inner column alone -
             // the output spec is what carries this, so it is exactly
             // what a wrong spec would scramble.
             std::string("SELECT b.qty, a.id FROM ta AS a JOIN tb AS b ON b.id = a.b_id"),
             std::string("SELECT b.qty FROM ta AS a JOIN tb AS b ON b.id = a.b_id"),
             // A residual on the leaf (outer relation) ...
             std::string("SELECT a.id, b.qty FROM ta AS a JOIN tb AS b ON b.id = a.b_id "
                         "WHERE a.b_id > 1"),
             // ... and one on the consuming stage (inner relation).
             std::string("SELECT a.id, b.qty FROM ta AS a JOIN tb AS b ON b.id = a.b_id "
                         "WHERE b.qty > 150"),
             // Both at once, and an empty answer.
             std::string("SELECT a.id, b.qty FROM ta AS a JOIN tb AS b ON b.id = a.b_id "
                         "WHERE a.b_id > 1 AND b.qty > 150"),
             std::string("SELECT a.id, b.qty FROM ta AS a JOIN tb AS b ON b.id = a.b_id "
                         "WHERE b.qty > 100000"),
             // A join on a **non-pk** column: no descent is possible, so
             // the inner step stays a walk filtered by the join residual -
             // the shape P4d-4c's gated inner walk exists to bound, and
             // the shape refused outright until it did.
             std::string("SELECT a.id, c.id FROM ta AS a JOIN tc AS c ON c.tag = a.b_id"),
             std::string("SELECT c.id, a.b_id FROM ta AS a JOIN tc AS c ON c.tag = a.b_id "
                         "WHERE a.b_id > 1"),
             std::string("SELECT a.id, c.tag FROM ta AS a JOIN tc AS c ON c.tag = a.b_id "
                         "WHERE c.tag > 1"),
         }) {
        const std::string local_reply = local.Dispatch(sql).response;
        ASSERT_EQ(local_reply.rfind("ERR ", 0), std::string::npos)
            << "the local side refused, so the comparison would prove nothing: " << sql
            << " -> " << local_reply;
        EXPECT_EQ(shipped(sql), local_reply) << sql;
    }

    // And the non-pk join is not vacuous: `tc.tag` {2,1,5,2} against
    // `ta.b_id` {2,1,9,3,1} matches four pairs - one outer row hitting two
    // inner rows, two outer rows hitting the same inner row, and two outer
    // rows hitting none. Spelled out because "the two sides agree" is only
    // worth having if they agreed about something.
    EXPECT_EQ(local.Dispatch("SELECT a.id, c.id FROM ta AS a JOIN tc AS c ON c.tag = a.b_id")
                  .response,
              "a.id,c.id\\n1,1\\n1,4\\n2,2\\n5,2");

    // ---- And the local side of that shape really does build (JB7) -------
    //
    // The three `tc.tag` statements above are the walked join, which is
    // the shape the statement-local inner build serves
    // (docs/spec/join-inner-build.md): locally the inner step builds a map
    // on its first outer row and probes it thereafter, while the shipped
    // side gets `ShippedForm`'s walk with the annotation cleared. So the
    // equivalence those rows assert is **build against shipped walk**, not
    // walk against walk - and it is worth exactly as much as that claim is
    // true, which is why it is checked here rather than assumed. Same
    // argument as `stages_opened` above: an equivalence test that quietly
    // stopped comparing two different things would still pass.
    {
        const std::string plan =
            local.Dispatch("ANALYZE SELECT a.id, c.id FROM ta AS a JOIN tc AS c "
                           "ON c.tag = a.b_id")
                .response;
        EXPECT_NE(plan.find("build on=col1"), std::string::npos) << plan;
        EXPECT_NE(plan.find("inner_built=1"), std::string::npos) << plan;
    }

    // ---- The structure-served shapes ship as their walk -----------------
    //
    // docs/inflight/known-gaps.md's closed entry named its own blind spot: "no
    // cross-core test declares an index, which is why no suite catches
    // it." This block is that test. An index or Cabin probe cannot cross
    // the descriptor; before the ship-time downgrade every shape below
    // fell out of the remote path and answered the affinity ERR - so
    // declaring an index on a peer relation's join column stopped the
    // join answering. Now each ships as the walk it would fall back to,
    // and the reply must equal the local one byte for byte, through the
    // same shipped() guard that proves something actually crossed.
    //
    // `td` is created through the dispatcher rather than the catalog
    // helper because an index needs a BTREE relation (IX3) and the
    // dispatcher's insert path is what maintains it.
    ASSERT_EQ(local.Dispatch("CREATE TABLE td (id int64, tag int64) BTREE")
                  .response.substr(0, 7),
              "CREATED");
    for (std::int64_t tag : {2, 1, 5, 2}) {
        ASSERT_EQ(local.Dispatch("INSERT INTO td VALUES (" + std::to_string(tag) + ")")
                      .response.substr(0, 8),
                  "INSERTED");
    }
    ASSERT_EQ(local.Dispatch("CREATE INDEX td_tag ON td (tag)").response.substr(0, 7),
              "CREATED");
    // Cabins on both sides of the join, so the downgrade is exercised at
    // the leaf (outer) as well as the consuming stage (inner).
    ASSERT_EQ(local.Dispatch("CREATE CABIN ON tc(tag)").response.substr(0, 7), "CREATED");
    ASSERT_EQ(local.Dispatch("CREATE CABIN ON ta(b_id)").response.substr(0, 7), "CREATED");

    for (const std::string& sql : {
             // The single-step seam: a literal IndexProbe, an IndexRange,
             // and a CabinProbe, each a star read of a peer relation.
             std::string("SELECT * FROM td WHERE tag = 2"),
             std::string("SELECT * FROM td WHERE tag BETWEEN 1 AND 2"),
             std::string("SELECT * FROM tc WHERE tag = 2"),
             // The consuming stage: IX17's correlated probe (no literal
             // anywhere), the literal probe propagation derives, and a
             // cabined inner.
             std::string("SELECT a.id, d.id FROM ta AS a JOIN td AS d ON d.tag = a.b_id"),
             std::string("SELECT a.id, d.id FROM ta AS a JOIN td AS d ON d.tag = a.b_id "
                         "WHERE d.tag = 2"),
             std::string("SELECT a.id, c.id FROM ta AS a JOIN tc AS c ON c.tag = a.b_id "
                         "WHERE c.tag = 2"),
             // The leaf: the outer relation's cabined column, single-step
             // and inside a join.
             std::string("SELECT * FROM ta WHERE b_id = 1"),
             std::string("SELECT a.id, c.id FROM ta AS a JOIN tc AS c ON c.tag = a.b_id "
                         "WHERE a.b_id = 1"),
         }) {
        const std::string local_reply = local.Dispatch(sql).response;
        ASSERT_EQ(local_reply.rfind("ERR ", 0), std::string::npos)
            << "the local side refused, so the comparison would prove nothing: " << sql
            << " -> " << local_reply;
        EXPECT_EQ(shipped(sql), local_reply) << sql;
    }

    // Not vacuous either: the indexed join matches the same four pairs
    // the tc join does, through the index this time.
    EXPECT_EQ(local.Dispatch("SELECT a.id, d.id FROM ta AS a JOIN td AS d ON d.tag = a.b_id")
                  .response,
              "a.id,d.id\\n1,1\\n1,4\\n2,2\\n5,2");
}

TEST_F(CoreRuntimeTest, APeerStoreTakesItsConfiguredFrameBudgetShare) {
    // The instance key never reached a peer before 2026-08-24: only core
    // 0's store was budgeted (expeditor.cpp), so on a multicore instance
    // every peer pool ran unbounded whatever the operator configured. The
    // share arrives through CoreRuntime::Config now. Asserted against the
    // configured value rather than "0 by default", because the debug
    // KDS_TEST_FRAME_BUDGET override may legitimately budget every store
    // in this suite (MG05) - a default-0 assertion would fail exactly in
    // the pressure runs that matter most.
    CoreRuntime::Config config = ConfigFor(1);
    config.buffer_pool_frames = 8;
    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    EXPECT_EQ(peer.value()->store().frame_budget(), 8u);
}

TEST_F(CoreRuntimeTest, AFundedPeerInsertsIntoItsOwnRelationEndToEnd) {
    // PW1c-5's e2e: the interim guard is gone, and a peer with every
    // funding piece - fault grant, write grant (rule 6's acquisition
    // restamp inside it), a row-id block, a trx-id block - runs a
    // single-row INSERT into its own heap relation and reads it back.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/2);
    catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "owned", TwoColumnSchema(),
                                    catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto row = catalog2.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    ASSERT_EQ(row.value().owner_core, 1u);
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    peer.value()->GrantRelationFault(
        RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));
    // The production grant set (root, var-heap root, anchor); PW1c-7's
    // rights probe asks for all of it before admitting a write.
    const PageId pages[] = {row.value().desc_page_id, row.value().anchor_page_id};
    peer.value()->GrantRelationWrite(pages);
    ASSERT_TRUE(peer.value()->store().MayWrite(row.value().desc_page_id));

    auto first = catalog2.AllocateRowIdRange(oid.value(), 16);
    ASSERT_TRUE(first.ok());
    peer.value()->row_id_leases().Grant(oid.value(), first.value(), 16);
    txn::TrxIdSequence core0_ids(core0_->superblock);
    auto block = core0_ids.Carve(16);
    ASSERT_TRUE(block.ok());
    peer.value()->trx_id_lease().Grant(block.value().first, block.value().count);

    const auto ins = peer.value()->dispatcher().Dispatch("INSERT INTO owned VALUES (7)").response;
    EXPECT_NE(ins.rfind("ERR", 0), 0u) << "the funded INSERT must run: " << ins;

    // Multi-row runs too (revised at the 25059bf review's S-1): the sorted
    // fill is merely ineligible on a peer, and the ordinary per-row path
    // allocates through the lease.
    const auto bulk =
        peer.value()->dispatcher().Dispatch("INSERT INTO owned VALUES (8), (9)").response;
    EXPECT_NE(bulk.rfind("ERR", 0), 0u) << "the per-row path must serve a peer: " << bulk;
    EXPECT_NE(bulk.find("rows=2"), std::string::npos) << bulk;

    // The reply is the CSV shape the neighbouring rotated-SELECT test
    // pins: a header line then one line per row, ",<v>" carrying the
    // inserted value after the leased id.
    const auto sel = peer.value()->dispatcher().Dispatch("SELECT * FROM owned").response;
    EXPECT_NE(sel.find(",7"), std::string::npos) << sel;
    EXPECT_NE(sel.find(",9"), std::string::npos) << sel;

    // The 25059bf review's idempotence pin: a repeat write grant appends
    // no second acquisition record.
    const auto before = peer.value()->wal().appended_lsn();
    peer.value()->GrantRelationWrite(pages);
    EXPECT_EQ(peer.value()->wal().appended_lsn(), before)
        << "a page already writable must take no second acquisition";
}

TEST_F(CoreRuntimeTest, ASpentLeaseRefusesWithTheWiresRetryableBit) {
    // PW6's finding (2), closed: a peer whose lease is spent used to answer
    // a bare `ERR` (ResourceExhausted is not IsRetryable), so a client
    // retrying on the bit did not retry it and lost the row. The refusal
    // is TxnConflict now and the dispatcher renders it through ErrorReply,
    // so the wire carries `retryable=1` - the token a retry loop reads.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/2);
    catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "owned", TwoColumnSchema(),
                                    catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto row = catalog2.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    peer.value()->GrantRelationFault(
        RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));
    const PageId pages[] = {row.value().desc_page_id, row.value().anchor_page_id};
    peer.value()->GrantRelationWrite(pages);

    // No transaction-id block: BeginWrite refuses first, before the row.
    const std::string kToken = "ERR TXN_CONFLICT retryable=1 ";
    const auto no_trx = peer.value()->dispatcher().Dispatch("INSERT INTO owned VALUES (7)").response;
    EXPECT_EQ(no_trx.substr(0, kToken.size()), kToken) << no_trx;
    EXPECT_NE(no_trx.find("transaction-id lease"), std::string::npos) << no_trx;

    // With transaction ids but no row-id block: the row's allocation refuses.
    txn::TrxIdSequence core0_ids(core0_->superblock);
    auto block = core0_ids.Carve(16);
    ASSERT_TRUE(block.ok());
    peer.value()->trx_id_lease().Grant(block.value().first, block.value().count);
    const auto no_rows = peer.value()->dispatcher().Dispatch("INSERT INTO owned VALUES (7)").response;
    EXPECT_EQ(no_rows.substr(0, kToken.size()), kToken) << no_rows;
    EXPECT_NE(no_rows.find("row-id lease"), std::string::npos) << no_rows;

    // Both funded: the same statement runs. The refusals above were the
    // lease's, never the relation's.
    auto first = catalog2.AllocateRowIdRange(oid.value(), 16);
    ASSERT_TRUE(first.ok());
    peer.value()->row_id_leases().Grant(oid.value(), first.value(), 16);
    const auto ins = peer.value()->dispatcher().Dispatch("INSERT INTO owned VALUES (7)").response;
    EXPECT_NE(ins.rfind("ERR", 0), 0u) << ins;
}

// PW1c-7 (workplan-peer-writer.md §8): a peer that wrote a relation across
// several pages, then restarted, holds nothing in memory - a fresh extent
// lease that covers none of its old pages, no fault grant, no write grant.
// What it does hold is durable: every page it wrote carries its stream stamp
// (PL §9 rule 4), the creation pages since the acquisition restamp (rule 6).
// The store claims from the stamp on the fault, so the relation reads whole
// and takes writes again with no grant re-delivered at all.
void CoreRuntimeTest::PeerPagesSurviveARestart(bool flush_before_restart,
                                               const std::string& name) {
    {
        catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                                  /*core_count=*/2);
        catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
        auto oid = catalog2.CreateTable(catalog::kNamespacePublic, name, TwoColumnSchema(),
                                        catalog::ClusteredType::kHeap);
        ASSERT_TRUE(oid.ok()) << oid.status().message();
        auto row = catalog2.GetSysTableRow(oid.value());
        ASSERT_TRUE(row.ok());
        ASSERT_EQ(row.value().owner_core, 1u);
        ASSERT_TRUE(core0_store_->Sync().ok());
        const PageId root = row.value().desc_page_id;

        txn::TrxIdSequence core0_ids(core0_->superblock);
        auto fund = [&](CoreRuntime& peer) {
            auto first = catalog2.AllocateRowIdRange(oid.value(), 1024);
            ASSERT_TRUE(first.ok());
            peer.row_id_leases().Grant(oid.value(), first.value(), 1024);
            auto block = core0_ids.Carve(1024);
            ASSERT_TRUE(block.ok());
            peer.trx_id_lease().Grant(block.value().first, block.value().count);
        };

        // The first run: funded the ordinary way, grown past one page.
        CoreRuntime::Config first_run = ConfigFor(1);
        // The ceiling core 0 copies in at every start (Expeditor's loop):
        // the second iteration's stream already names the first's ids.
        SCOPED_TRACE("first run's lease starts at page " + std::to_string(first_run.lease.first));
        auto peer = CoreRuntime::Open(first_run, *device_, clock_, nullptr);
        ASSERT_TRUE(peer.ok()) << peer.status().message();
        peer.value()->GrantRelationFault(
            RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));
        const PageId pages[] = {root, row.value().anchor_page_id};
        peer.value()->GrantRelationWrite(pages);
        fund(*peer.value());
        for (int i = 0; i < 600; ++i) {
            const auto ins = peer.value()
                                 ->dispatcher()
                                 .Dispatch("INSERT INTO " + name + " VALUES (" +
                                           std::to_string(i) + ")")
                                 .response;
            ASSERT_NE(ins.rfind("ERR", 0), 0u) << "row " << i << ": " << ins;
        }
        EXPECT_EQ(peer.value()->store().stamp_claims(), 0u)
            << "a funded first run claims nothing";
        ASSERT_TRUE(peer.value()->Sync().ok()) << "the log is what survives";
        if (flush_before_restart) {
            ASSERT_TRUE(peer.value()->store().Sync().ok());
            // **And core 0's free map, which is the authority on which pages
            // exist.** A restarting peer builds that view by reading the map
            // off the device (`core_runtime.cpp`), and the extent this run
            // was leased was carved out of core 0's copy *after* the last
            // sync - so without this the restarted peer answers `not found`
            // for its own pages before the stamp claim is ever reached. On
            // the log path redo's `PAGE_INIT` replay allocates them instead,
            // which is why only this arm needs it; in a real instance the
            // pass that does it is core 0's own mount, before any peer
            // attaches.
            ASSERT_TRUE(core0_store_->Sync().ok());
        }
        peer.value().reset();

        // The restart: a new lease, nothing granted, the ceiling core 0
        // would copy in.
        CoreRuntime::Config again = ConfigFor(1);
        auto reopened = CoreRuntime::Open(again, *device_, clock_, nullptr);
        ASSERT_TRUE(reopened.ok()) << reopened.status().message();
        EXPECT_FALSE(reopened.value()->store().MayWrite(root))
            << "nothing in memory says the root is this core's yet";

        const auto count =
            reopened.value()->dispatcher().Dispatch("SELECT COUNT(*) FROM " + name).response;
        EXPECT_NE(count.find("600"), std::string::npos) << count;
        EXPECT_GT(reopened.value()->store().stamp_claims(), 1u)
            << "the root and its growth pages must each have been claimed";
        EXPECT_TRUE(reopened.value()->store().MayWrite(root))
            << "the read's claim is the write's right";

        fund(*reopened.value());
        const auto ins =
            reopened.value()->dispatcher().Dispatch("INSERT INTO " + name + " VALUES (600)").response;
        EXPECT_NE(ins.rfind("ERR", 0), 0u) << "the restarted owner must write again: " << ins;
        const auto after =
            reopened.value()->dispatcher().Dispatch("SELECT COUNT(*) FROM " + name).response;
        EXPECT_NE(after.find("601"), std::string::npos) << after;
        // Destroyed with its last page unflushed and its PAGE_INIT in the
        // log - the never-written-page case the store reads as NotFound.
        reopened.value().reset();
    }
}

TEST_F(CoreRuntimeTest, APeersOwnPagesSurviveARestartByTheirStamp) {
    // **The device path**, which is the half that has nothing to do with
    // the log: the pages were flushed, and the restarted owner claims each
    // one from the stamp it reads off the platter
    // (`DevicePageStore::TryClaimByStamp`). True under either topology, and
    // run here under the one every volume this build creates.
    PeerPagesSurviveARestart(/*flush_before_restart=*/true, "survives_flushed");
}

TEST_F(CoreRuntimePerCoreStreamTest, APeersOwnPagesSurviveARestartByRedosStamp) {
    // **The resident-frame path**, and it is per-core by construction: the
    // pages live only in the log, so what leaves them resident and stamped
    // is the peer's *own* redo - and under one stream a peer runs no
    // recovery at all (core 0's pass is the instance's).
    PeerPagesSurviveARestart(/*flush_before_restart=*/false, "survives_logged");
}

TEST_F(CoreRuntimeTest, AnUnacquiredRelationIsAskedForAndTheRegrantLands) {
    // PW1c-7's other half: the stamp claims only what this stream wrote,
    // and a relation whose creation pages this peer never acquired - the
    // grant crashed, was lost to the ring, or preceded a restart - has an
    // owner and no writer. The dispatcher's rights probe refuses by name
    // and records the demand; the tick asks core 0; core 0 re-runs the
    // publish; the grants land through PW1c-4's receivers; the retry
    // writes. Over a real ring, with core 0's handler wired to the same
    // publish sequence Expeditor installs.
    auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 64);
    ASSERT_TRUE(transport.ok()) << transport.status().message();
    sched::NullIoBackend io0;
    sched::Scheduler core0(clock_, io0);
    ASSERT_TRUE(core0.AttachTransport(&transport.value(), 0).ok());
    // The peer's completion checkpoint publishes an anchor here; not what
    // this test is about, so it is accepted and dropped.
    ASSERT_TRUE(core0
                    .RegisterMessageHandler(sched::RingMessageKind::kAnchorWrite,
                                            [](const sched::MessageHeader&,
                                               std::span<const std::byte>) {})
                    .ok());

    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/2);
    catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    // No publish hook on the catalog: this CREATE TABLE's grants are the
    // ones that got lost.
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "unacquired", TwoColumnSchema(),
                                    catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto row = catalog2.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    ASSERT_EQ(row.value().owner_core, 1u);
    ASSERT_TRUE(core0_store_->Sync().ok());
    const PageId root = row.value().desc_page_id;

    int publishes = 0;
    const auto send = [&](sched::RingMessageKind kind, const auto& pod) {
        std::byte payload[sizeof(pod)];
        std::memcpy(payload, &pod, sizeof(pod));
        sched::MessageHeader header{};
        header.src_core = 0;
        header.dst_core = 1;
        header.session_core = 0;
        header.kind = static_cast<std::uint16_t>(kind);
        header.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
        core0.Submit(sched::MakeSendRetryTask(transport.value(), header, payload));
    };
    // Expeditor's publish, on the fixture's store: flush, durable handoff
    // records (an unlogged store answers kNoLsn), both grants.
    const catalog::Catalog::RelationPublishHook publish =
        [&](catalog::Oid, std::uint32_t owner, PageId r, PageId varheap, PageId anchor) {
            catalog::SysTableRow facts{};
            facts.desc_page_id = r;
            facts.varheap_page_id = varheap;
            facts.anchor_page_id = anchor;
            const storage::Extent range =
                RelationFaultExtentOf(facts, storage::kDefaultExtentPages);
            std::vector<PageId> pages;
            for (PageId id = range.first; id < range.end(); ++id) pages.push_back(id);
            ASSERT_TRUE(core0_store_->FlushPages(pages).ok());
            const PageId formatted[] = {r, varheap, anchor};
            auto grant = PrepareRelationHandoff(nullptr, owner, formatted);
            ASSERT_TRUE(grant.ok()) << grant.status().message();
            send(sched::RingMessageKind::kRelationFaultGrant,
                 ExtentGrantPayload{range.first, range.count});
            send(sched::RingMessageKind::kRelationWriteGrant, grant.value());
            ++publishes;
        };
    ASSERT_TRUE(RegisterRelationGrantHandler(core0, catalog2, publish, nullptr).ok());

    CoreRuntime::Config config = ConfigFor(1);
    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    ASSERT_TRUE(peer.value()->AttachTransport(transport.value()).ok());
    auto first = catalog2.AllocateRowIdRange(oid.value(), 16);
    ASSERT_TRUE(first.ok());
    peer.value()->row_id_leases().Grant(oid.value(), first.value(), 16);
    txn::TrxIdSequence core0_ids(core0_->superblock);
    auto block = core0_ids.Carve(16);
    ASSERT_TRUE(block.ok());
    peer.value()->trx_id_lease().Grant(block.value().first, block.value().count);

    // Refused by name, retryably, with the demand recorded - and no
    // request has left yet.
    const auto refused =
        peer.value()->dispatcher().Dispatch("INSERT INTO unacquired VALUES (1)").response;
    EXPECT_EQ(refused.rfind("ERR", 0), 0u) << refused;
    EXPECT_NE(refused.find("PW1c-7"), std::string::npos) << refused;
    EXPECT_NE(refused.find("TXN_CONFLICT"), std::string::npos)
        << "a re-delivery makes the retry succeed, so the refusal is retryable: " << refused;
    EXPECT_FALSE(peer.value()->relation_grant_demand().empty());
    EXPECT_EQ(publishes, 0);

    peer.value()->MaybeRequestRelationGrants();
    // A statement refused while the request is out records its demand and
    // the tick sends nothing more (the review's C4 latch): one publish on
    // core 0 per request, however hard a client retries.
    const auto refused_again =
        peer.value()->dispatcher().Dispatch("INSERT INTO unacquired VALUES (1)").response;
    EXPECT_EQ(refused_again.rfind("ERR", 0), 0u) << refused_again;
    peer.value()->MaybeRequestRelationGrants();
    for (int i = 0; i < 40; ++i) {
        peer.value()->scheduler().RunOnce();
        core0.RunOnce();
    }
    EXPECT_EQ(publishes, 1) << "core 0 must have run the publish exactly once";
    EXPECT_TRUE(peer.value()->store().MayWrite(root)) << "the re-delivered grant did not land";

    // The grant's admission released the latch, so the demand that waited
    // goes out on the next tick - and core 0's repeat is harmless: no
    // second acquisition, the same rights.
    EXPECT_FALSE(peer.value()->relation_grant_demand().empty());
    peer.value()->MaybeRequestRelationGrants();
    for (int i = 0; i < 40; ++i) {
        peer.value()->scheduler().RunOnce();
        core0.RunOnce();
    }
    EXPECT_EQ(publishes, 2);
    EXPECT_TRUE(peer.value()->relation_grant_demand().empty());

    const auto retried =
        peer.value()->dispatcher().Dispatch("INSERT INTO unacquired VALUES (1)").response;
    EXPECT_NE(retried.rfind("ERR", 0), 0u) << "the retry must write: " << retried;
    const auto sel = peer.value()->dispatcher().Dispatch("SELECT * FROM unacquired").response;
    EXPECT_NE(sel.find(",1"), std::string::npos) << sel;

    // Core 0 re-delivers only to the catalog's owner: a request for a
    // relation this peer does not own is dropped, never granted.
    auto other = core0_->catalog.CreateTable(catalog::kNamespacePublic, "core0s",
                                             TwoColumnSchema(), catalog::ClusteredType::kHeap);
    ASSERT_TRUE(other.ok());
    RequestRelationGrant(peer.value()->scheduler(), transport.value(), other.value(), 1);
    for (int i = 0; i < 40; ++i) {
        peer.value()->scheduler().RunOnce();
        core0.RunOnce();
    }
    EXPECT_EQ(publishes, 2) << "a foreign relation's request must be dropped";
}

TEST_F(CoreRuntimeTest, AWriteGrantAloneCarriesItsOwnFaultRights) {
    // The 95b45e8 review's C2, pinned: two send-retry tasks can reorder on
    // a full ring, so the write grant must survive arriving before the
    // extent fault grant.
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "solo", TwoColumnSchema(),
                                           catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok());
    ASSERT_TRUE(core0_store_->Sync().ok());
    auto row = core0_->catalog.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    const PageId root = row.value().desc_page_id;

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    const PageId pages[] = {root};
    peer.value()->GrantRelationWrite(pages);  // no fault grant first
    EXPECT_TRUE(peer.value()->store().MayWrite(root));
    EXPECT_TRUE(peer.value()->store().MayFault(root));
}

TEST_F(CoreRuntimeTest, PrepareRelationHandoffRefusesPastCapacityAndSkipsInvalid) {
    // The send half, testable at last (the 95b45e8 review's S1 was the
    // extraction; the 25059bf review's gap 1 is this test). A null WAL is
    // the unlogged store: kNoLsn throughout, nothing to sync.
    const PageId two[] = {130, kInvalidPageId, 131};
    auto grant = PrepareRelationHandoff(nullptr, 1, two);
    ASSERT_TRUE(grant.ok()) << grant.status().message();
    EXPECT_EQ(grant.value().count, 2u);
    EXPECT_EQ(grant.value().page_ids[0], 130u);
    EXPECT_EQ(grant.value().page_ids[1], 131u);

    PageId many[RelationWriteGrantPayload::kMaxPages + 1];
    for (std::uint32_t i = 0; i < RelationWriteGrantPayload::kMaxPages + 1; ++i) {
        many[i] = 200 + i;
    }
    auto refused = PrepareRelationHandoff(nullptr, 1, many);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kUnsupported)
        << refused.status().message();
}

TEST_F(CoreRuntimeTest, APeerRefusesACallerSuppliedKeyAndTakesTheSameRowWithout) {
    // The shape gate's key-mode arm lifted with the mode (heap-and-tuple.md
    // §4.1) and the refusal moved into the row: admitting a supplied id
    // writes the relation's sys.tables row, which a peer may never write,
    // while the *same* relation takes a row that omits its key through this
    // core's own id lease. Per row, so both halves are asserted here - the
    // gate's old form refused the relation and would have failed the second.
    // No session poison either way.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/2);
    catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "explicit_owned",
                                    TwoColumnSchema(), catalog::ClusteredType::kBtree);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    auto row = catalog2.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    peer.value()->GrantRelationFault(
        RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));
    // **Funded the whole way**, which the old form of this test did not have
    // to be: its per-relation refusal fired above the write grant and the id
    // lease, so a peer with neither still produced the expected message. The
    // refusal is per row now, and the row that *omits* its key has to reach
    // the storage and succeed - so everything a funded peer write needs is
    // granted here, and what the test then isolates is the one thing left.
    const PageId pages[] = {row.value().desc_page_id, row.value().anchor_page_id};
    peer.value()->GrantRelationWrite(pages);
    auto first = catalog2.AllocateRowIdRange(oid.value(), 16);
    ASSERT_TRUE(first.ok());
    peer.value()->row_id_leases().Grant(oid.value(), first.value(), 16);
    txn::TrxIdSequence core0_ids(core0_->superblock);
    auto block = core0_ids.Carve(16);
    ASSERT_TRUE(block.ok());
    peer.value()->trx_id_lease().Grant(block.value().first, block.value().count);

    const auto reply =
        peer.value()
            ->dispatcher()
            .Dispatch("INSERT INTO explicit_owned VALUES (5, 1)")
            .response;
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
    EXPECT_NE(reply.find("caller-supplied primary key"), std::string::npos) << reply;

    // The same relation, the same core, the key omitted: admitted. This is
    // the half the old per-relation gate could not express.
    const auto omitted =
        peer.value()->dispatcher().Dispatch("INSERT INTO explicit_owned VALUES (1)").response;
    EXPECT_EQ(omitted.substr(0, 8), "INSERTED") << omitted;

    // Not poisoned: the next statement answers normally.
    EXPECT_NE(peer.value()->dispatcher().Dispatch("SHOW TABLES").response.rfind("ERR", 0), 0u);
}

TEST_F(CoreRuntimeTest, AFundedPeerGrowsItsOwnBtreeWritingNoCatalogPage) {
    // PW2-4's proof: a peer INSERTs into its own btree relation far enough
    // to divide leaves - every split page from its own lease, every root
    // move in its own granted anchor - and the sys.tables row never moves.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/2);
    catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "btree_owned", TwoColumnSchema(),
                                    catalog::ClusteredType::kBtree);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto row = catalog2.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    ASSERT_EQ(row.value().owner_core, 1u);
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    peer.value()->GrantRelationFault(
        RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));
    const PageId pages[] = {row.value().desc_page_id, row.value().anchor_page_id};
    peer.value()->GrantRelationWrite(pages);
    ASSERT_TRUE(peer.value()->store().MayWrite(row.value().anchor_page_id));

    auto first = catalog2.AllocateRowIdRange(oid.value(), 1024);
    ASSERT_TRUE(first.ok());
    peer.value()->row_id_leases().Grant(oid.value(), first.value(), 1024);
    txn::TrxIdSequence core0_ids(core0_->superblock);
    auto block = core0_ids.Carve(1024);
    ASSERT_TRUE(block.ok());
    peer.value()->trx_id_lease().Grant(block.value().first, block.value().count);

    for (int i = 0; i < 600; ++i) {
        const auto ins =
            peer.value()
                ->dispatcher()
                .Dispatch("INSERT INTO btree_owned VALUES (" + std::to_string(i) + ")")
                .response;
        ASSERT_NE(ins.rfind("ERR", 0), 0u) << "row " << i << ": " << ins;
    }

    // The row never moved; the anchor's root is live and the relation
    // answers whole.
    auto row_after = catalog2.GetSysTableRow(oid.value());
    ASSERT_TRUE(row_after.ok());
    EXPECT_EQ(row_after.value().desc_page_id, row.value().desc_page_id)
        << "the peer must not have written the catalog row";
    const auto count =
        peer.value()->dispatcher().Dispatch("SELECT COUNT(*) FROM btree_owned").response;
    EXPECT_NE(count.find("600"), std::string::npos) << count;
}

TEST_F(CoreRuntimeTest, ACreatedRelationsAnchorIsWiredWholeThroughTheCatalog) {
    // The 3f07eda review's S6: the four facts PW2-2 will stand on, pinned
    // against the real store - the row names a page whose type is kAnchor,
    // that page's clustered root is the relation's, and the fact
    // round-trips through TableAccess. (The ANCHOR_UPDATE's emission is
    // pinned by the redo replay test and exercised by the sim's crash
    // sweep, which remounts created relations.)
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "anchored",
                                           TwoColumnSchema(), catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto row = core0_->catalog.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    ASSERT_NE(row.value().anchor_page_id, kInvalidPageId);
    ASSERT_NE(row.value().anchor_page_id, row.value().desc_page_id);

    auto page = core0_store_->GetForRead(row.value().anchor_page_id);
    ASSERT_TRUE(page.ok()) << page.status().message();
    EXPECT_EQ(storage::RawPageType(page.value().bytes()),
              static_cast<std::uint8_t>(PageType::kAnchor));
    EXPECT_EQ(storage::GetOwnerOid(page.value().bytes()), oid.value());
    EXPECT_EQ(storage::AnchorClusteredRoot(page.value().bytes()),
              row.value().desc_page_id);

    auto access = core0_->catalog.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    EXPECT_EQ(access.value()->anchor_page_id, row.value().anchor_page_id);
}

TEST_F(CoreRuntimeTest, TheAnchorNotTheRowIsTheClusteredRootsTruth) {
    // PW2-2: a fresh fill resolves desc_page_id through the anchor, so a
    // root move that writes only the anchor (PW2-3's contract) is seen by
    // the next fill while the CREATE-fixed row stays put. Simulated by
    // moving the anchor's slot by hand and filling through a fresh
    // catalog over the same store - the cache-miss path.
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "moved", TwoColumnSchema(),
                                           catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok());
    auto row = core0_->catalog.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    const PageId moved_root = row.value().desc_page_id + 7;  // any distinct id
    // Through the real mover (PW2-3): the anchor slot moves, the row does
    // not - the retirement's whole contract in one call.
    ASSERT_TRUE(core0_->catalog
                    .UpdateRelationDescPage(oid.value(), moved_root,
                                            row.value().anchor_page_id)
                    .ok());

    catalog::Catalog fresh(*core0_store_, storage::kDefaultInlineCellWidth,
                           /*core_count=*/1);
    auto access = fresh.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    EXPECT_EQ(access.value()->desc_page_id, moved_root)
        << "the fill must read the anchor, not the row";
    // The row itself is CREATE-fixed - unchanged by the move.
    auto row_after = fresh.GetSysTableRow(oid.value());
    ASSERT_TRUE(row_after.ok());
    EXPECT_EQ(row_after.value().desc_page_id, row.value().desc_page_id);
}

TEST_F(CoreRuntimeTest, CreateIndexOnAPeerOwnedRelationIsRefusedByName) {
    // A core-0 runtime opened bare - no Expeditor, so no index-build
    // client (only the Expeditor calls SetIndexBuilds). The foreign arm
    // has nothing to reach the owner with and refuses by name and byte
    // rather than build a tree in the wrong core's pages (PW1c-6b-4).
    // With the client wired the same statement is the two-phase build,
    // covered by the ForeignIndexRig tests below.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/2);
    catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "rotated_ix", TwoColumnSchema(),
                                    catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto core0 = CoreRuntime::Open(ConfigFor(0), *device_, clock_, nullptr);
    ASSERT_TRUE(core0.ok()) << core0.status().message();
    const std::string reply =
        core0.value()
            ->dispatcher()
            .Dispatch("CREATE INDEX rix ON rotated_ix (value)")
            .response;
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
    EXPECT_NE(reply.find("PW1c-6b"), std::string::npos) << reply;
    EXPECT_NE(reply.find("no index-build client"), std::string::npos) << reply;
    EXPECT_NE(reply.find("at byte"), std::string::npos) << reply;
}

TEST_F(CoreRuntimeTest, APeerOpenedBeforeTheDdlCanStillTakeTheWriteGrant) {
    // The 95b45e8 review's C1, pinned in the production ordering: the
    // peer starts first, the DDL lands later, and the grant must still
    // take - the peer's free-map snapshot predates the relation, and the
    // grant receivers refresh it from the device (which core 0 flushed
    // before any grant left).
    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);  // peer first
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "late", TwoColumnSchema(),
                                           catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok());
    ASSERT_TRUE(core0_store_->Sync().ok());
    auto row = core0_->catalog.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    const PageId root = row.value().desc_page_id;

    peer.value()->GrantRelationFault(
        RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));
    EXPECT_TRUE(peer.value()->store().GetForRead(root).ok())
        << "the fault grant must refresh the free-map snapshot";
    const PageId pages[] = {root};
    peer.value()->GrantRelationWrite(pages);
    EXPECT_TRUE(peer.value()->store().MayWrite(root));
}

TEST_F(CoreRuntimeTest, APeerRefusesEveryDdlVerbByNameAndStillServesReads) {
    // PW4 (workplan-peer-writer.md): the refusal must exist *before* PW5
    // gives peers listeners, and it must name where DDL runs - the
    // alternative was running until MayWrite failed with a page id. The
    // §5d purge gate's soundness argument cites this guard.
    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    const std::string_view ddl[] = {
        "CREATE TABLE pw4 (id int64, v int64)",
        "CREATE INDEX pw4_v ON pw4 (v)",
        "CREATE ASSERTION a4 ON pw4 GROUP BY (v) CHECK COUNT(*) <= 1",
        "CREATE CABIN c4 ON pw4 (v)",
        "ALTER TABLE pw4 RENAME TO pw4b",
        "DROP TABLE pw4",
        "DROP INDEX pw4_v",
    };
    for (std::string_view stmt : ddl) {
        const std::string reply = peer.value()->dispatcher().Dispatch(stmt).response;
        EXPECT_EQ(reply.rfind("ERR", 0), 0u) << stmt << " -> " << reply;
        EXPECT_NE(reply.find("takes no DDL"), std::string::npos) << stmt << " -> " << reply;
        EXPECT_NE(reply.find("core 1"), std::string::npos) << stmt << " -> " << reply;
        EXPECT_NE(reply.find("core 0"), std::string::npos) << stmt << " -> " << reply;
    }

    // The guard keys on the router's own token, so spelling coverage is by
    // construction - pinned once so a tokenizer change cannot silently
    // narrow it - and the ANALYZE prefix routes to the SELECT path, never
    // to a DDL handler, so it must answer as the parser and not this guard.
    EXPECT_NE(peer.value()
                  ->dispatcher()
                  .Dispatch("  create table sp (id int64)")
                  .response.find("takes no DDL"),
              std::string::npos);
    EXPECT_EQ(peer.value()
                  ->dispatcher()
                  .Dispatch("ANALYZE CREATE TABLE sp (id int64)")
                  .response.find("takes no DDL"),
              std::string::npos);

    // The control, twice over: reads are untouched on the peer, and the
    // guard is *core*-scoped, not a new refusal of DDL itself. A core-0
    // CoreRuntime in this fixture cannot run DDL end to end (its
    // TrxIdSequence has no superblock persist rights here - the whole
    // suite's ordinary DDL tests prove the positive), so the control pins
    // exactly the guard's marker: core 0's reply, whatever else it says,
    // never says "takes no DDL".
    EXPECT_EQ(peer.value()->dispatcher().Dispatch("SHOW TABLES").response.rfind("ERR", 0),
              std::string::npos);
    auto core0 = CoreRuntime::Open(ConfigFor(0), *device_, clock_, nullptr);
    ASSERT_TRUE(core0.ok()) << core0.status().message();
    const std::string on_core0 =
        core0.value()->dispatcher().Dispatch("CREATE TABLE pw4 (id int64, v int64)").response;
    EXPECT_EQ(on_core0.find("takes no DDL"), std::string::npos) << on_core0;
}

namespace {

// Minimal blocking client for the peer-listener test: connect, send one
// line, read one newline-terminated reply under a poll() deadline.
int ConnectLoopback(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string RoundTrip(int fd, std::string_view line) {
    std::string out(line);
    out.push_back('\n');
    if (::send(fd, out.data(), out.size(), 0) != static_cast<ssize_t>(out.size())) return "";
    std::string reply;
    char buf[4096];
    for (int spins = 0; spins < 5000; ++spins) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 1) <= 0) continue;
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        reply.append(buf, static_cast<std::size_t>(n));
        if (reply.find('\n') != std::string::npos) break;
    }
    return reply;
}

}  // namespace

TEST_F(CoreRuntimeTest, APeerListenerServesItsOwnRelationRefusesAnUnfundedWriteAndRoutesStop) {
    // FINDING 5 of the PW5 review: nothing proved a peer listener serves
    // anything. This is the whole loop over a real socket - a rotated
    // relation is served on the peer that owns it, a core-0 relation is
    // refused with the affinity answer, and STOP does not stop this
    // reactor: it fires the instance's stop hook (tcp_server.hpp's stop
    // contract, CoreRuntime::ListenAndAttach; a `kShutdown` message to the
    // system core until AU-S3).
    constexpr std::uint16_t kPort = 25442;

    auto transport = sched::RealRingTransport::Create(2, 16, 256);
    ASSERT_TRUE(transport.ok());

    // A relation owned by core 1 (the :417 test's arrangement), and one
    // owned by core 0 as the foreign control.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/2);
    catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    auto rotated = catalog2.CreateTable(catalog::kNamespacePublic, "rotated",
                                        TwoColumnSchema(), catalog::ClusteredType::kHeap);
    ASSERT_TRUE(rotated.ok()) << rotated.status().message();
    auto local0 = core0_->catalog.CreateTable(catalog::kNamespacePublic, "local0",
                                              TwoColumnSchema(), catalog::ClusteredType::kHeap);
    ASSERT_TRUE(local0.ok()) << local0.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    ASSERT_TRUE(peer.value()->AttachTransport(transport.value()).ok());
    auto row = catalog2.GetSysTableRow(rotated.value());
    ASSERT_TRUE(row.ok());
    ASSERT_EQ(row.value().owner_core, 1u);
    peer.value()->GrantRelationFault(
        RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));
    // AU-S3: what the instance installs on every peer, standing in for
    // `Expeditor`'s `instance_stop_`. Installed **before** the listener, so
    // the stop handler it feeds exists by the time a client can send STOP.
    std::atomic<bool> instance_stopped{false};
    peer.value()->set_instance_stop([&instance_stopped] { instance_stopped.store(true); });
    ASSERT_TRUE(peer.value()->ListenAndAttach(kPort, Protocol::kText).ok());

    std::thread worker([&] { peer.value()->Run(); });

    int fd = ConnectLoopback(kPort);
    ASSERT_GE(fd, 0);

    // Served: the relation this core owns (empty is fine; not an ERR).
    const std::string own = RoundTrip(fd, "SELECT * FROM rotated");
    EXPECT_EQ(own.rfind("ERR", 0), std::string::npos) << own;
    // **The foreign read is no longer asserted here, and that is SS2.** A
    // relation core 0 owns used to answer "owned by core 0" from this
    // listener; it now ships to core 0 and is answered there, which is the
    // whole of what this version changes. It cannot be asserted in *this*
    // fixture: no core-0 reactor runs in it, so the shipped statement would
    // wait out the ten-second deadline and answer `UNKNOWN_OUTCOME` -
    // truthful, and a ten-second test. It is pinned instead where core 0
    // answers, on the rig that has one
    // (`AReadOfAPeerOwnedRelationShipsAndAnswersWithTheOwnersRows`, and the
    // write half beside it).
    //
    // The `BEGIN`-then-read form is not the substitute it looks like: a
    // listener-served peer holds no transaction-id lease either, so `BEGIN`
    // itself refuses and the read that follows is an autocommit read again.
    // Refused: an *unfunded* write to the relation this core owns. The
    // PW1c-5 replacement for the interim guard: this listener-served peer
    // holds no write grant and no row-id lease, so the INSERT dies at the
    // first funding wall - retryably, from the lease - and in release the
    // store's now-always-on MayWrite is what stands behind it
    // (workplan-peer-writer.md §8, the PW1c-4r row).
    const std::string write = RoundTrip(fd, "INSERT INTO rotated VALUES (7)");
    EXPECT_EQ(write.rfind("ERR", 0), 0u) << write;
    EXPECT_NE(write.find("lease"), std::string::npos)
        << "the refusal should name the funding wall, not a page or a read: " << write;

    // STOP: replied to, and **routed to the instance rather than to this
    // reactor**. That is the contract - a stopped peer would still take its
    // kernel share of new connections while core 0 reported healthy - and it
    // is now observed directly. Until AU-S3 this asserted on a `kShutdown`
    // appearing in core 0's ring, which tested the transport rather than the
    // routing; the hook is what the instance actually installs.
    (void)RoundTrip(fd, "STOP");
    ::close(fd);

    for (int spins = 0; spins < 5000 && !instance_stopped.load(); ++spins) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(instance_stopped.load()) << "STOP on a peer never reached the instance";

    // What Serve's tail would then do: stop the peer. A direct call since
    // AU-S3, the flag being atomic.
    peer.value()->scheduler().Stop();
    worker.join();
}

TEST_F(CoreRuntimeTest, APeerIsWiredWithRecordingOff) {
    // P6's deliberate cost, pinned so it stays a decision rather than
    // becoming a surprise: sys.patterns and sys.access_stats are catalog
    // pages written on the statement path, and a peer may not write them.
    // Both features are advisory, so a peer will return the same rows
    // without them once it can serve at all.
    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    // The write a recording peer would attempt, refused at the store.
    EXPECT_FALSE(peer.value()->store().MayWrite(catalog::kCatalogPageAccessStats));
    EXPECT_FALSE(peer.value()->store().MayWrite(catalog::kCatalogPagePatterns));

    // And nothing on core 0's side was written by the peer existing.
    auto shapes = core0_->catalog.ListAccessStats();
    ASSERT_TRUE(shapes.ok()) << shapes.status().message();
    EXPECT_TRUE(shapes.value().empty());
}

TEST_F(CoreRuntimeTest, APeerListenerIsTornDownBeforeTheReactorItRegisteredWith) {
    // PW5's teardown, and the one thing declaration order does not decide:
    // `~CoreRuntime`'s *body* drops the scheduler before any member
    // destructor runs, so a listener left to the members' reverse order
    // would run `~TcpServer` - whose `Detach()` unregisters the listening fd
    // and every client fd - against a reactor that had already been
    // destroyed. Dropping `listener_` first is what makes that impossible,
    // and this is the observable half: while the peer holds the port a plain
    // (non-SO_REUSEPORT) bind is refused, and once the runtime is gone the
    // same bind succeeds - which is only true if the teardown really ran and
    // closed the socket.
    constexpr std::uint16_t kPort = 25441;
    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    ASSERT_TRUE(peer.value()->ListenAndAttach(kPort, Protocol::kText).ok());

    // A socket without SO_REUSEPORT may not join a REUSEPORT group, so this
    // is the port being genuinely held by the peer.
    EXPECT_FALSE(TcpServer::Listen(kPort).ok());

    peer.value().reset();

    auto after = TcpServer::Listen(kPort);
    EXPECT_TRUE(after.ok()) << after.status().message();
}

// ---- PW1c-6b-3: core 0's two phases over a real ring ----------------------

// Everything the tests below share. Core 0 is a scheduler, a dispatcher
// with its transaction stack (so phase 2's DDL scope is a real
// transaction, D2) and the client that parks between the phases; the peer
// is a whole runtime owning one relation with three rows core 0 never
// faulted. `clock` is core 0's alone - the timeout test drives it by hand
// while the peer keeps the system clock, so nothing there expires.
struct ForeignIndexRig {
    explicit ForeignIndexRig(const sched::Clock& core0_clock) : clock(core0_clock) {}

    const sched::Clock& clock;
    std::optional<StatusOr<sched::RealRingTransport>> transport;
    sched::NullIoBackend io0;
    std::optional<sched::Scheduler> core0;
    std::optional<catalog::Catalog> catalog2;
    std::optional<txn::TrxIdSequence> ids;
    std::optional<txn::UndoLog> undo;
    std::optional<txn::TransactionManager> txns;
    // `client` before `dispatcher`, deliberately: members die in reverse
    // declaration order, and `SetIndexBuilds` requires the client to
    // outlive the dispatcher that holds a pointer to it. It is built from
    // `core0`, so it is declared after that and dies before it - which is
    // the contract's other half, and the reason nothing pumps `core0`
    // after this rig starts unwinding.
    std::optional<IndexBuildClient> client;
    // Core 0's assertion-build client (PW1c-6c), declared with `client` and
    // for its reason.
    std::optional<AssertionBuildClient> assertion_client;
    // Core 0's statement-shipping client (SS2), declared with `client` and
    // for its reason: the dispatcher holds a pointer to it, so it must
    // outlive the dispatcher.
    std::optional<StatementShipClient> ship;
    // Core 0's coordinator half of the cross-owner commit (R6-3/R6-8),
    // declared with `ship` and for its reason - the dispatcher holds a
    // pointer to it and must die first.
    std::optional<Txn2pcClient> txn2pc;
    // **Core 0's two halves of the foreign-key probe** (AI-T2). Production
    // wires both on every core, because a relation is a foreign parent on
    // one statement and a child on the next - `CoreRuntime::AttachTransport`
    // for a peer and `Expeditor::Serve` for core 0. **The second of those
    // was missing until 2026-09-02** and this hand-wiring is what concealed
    // it: every cell whose parent is core-0-owned passed against a rig that
    // had what production did not. The table is declared ahead of the
    // server that fills it and ahead of the dispatcher that reads it, which
    // is `core_runtime.hpp`'s own order and for its reason.
    FkIntentTable fk_intents;
    // AJ-T1's mirror, in the rig for `fk_intents`' reason: the dispatcher
    // reads it and the probe server consults it, so a rig without one would
    // pass cells that production cannot.
    FkPendingDeleteTable fk_pending_deletes;
    std::optional<FkProbeClient> fk_client;
    std::optional<FkProbeServer> fk_server;
    // Core 0's Cabin store, so the rig's core 0 is built the way
    // `Expeditor` builds it (AK-S2's cells read `SHOW CABINS` from it).
    // Declared ahead of the dispatcher that borrows it.
    std::optional<stats::CabinStore> cabins0;
    std::optional<CommandDispatcher> dispatcher;
    // **Core 0's owner half of statement shipping** (SS1's wiring rule:
    // every core answers requests and every core receives replies). The
    // rig had only the arrival half, because until CR5 nothing shipped
    // *to* core 0 - a peer's DDL was refused at its own dispatch. Declared
    // after `dispatcher` so both die before it: the executor holds a
    // reference to it, and the server holds the executor's seam.
    std::optional<ShippedStatementExecutor> executor;
    std::optional<StatementShipServer> ship_server;
    // **Core 0's participant half of the cross-owner commit** (AI-T2). The
    // rig had the coordinator half only, because until the foreign-key
    // probe nothing enrolled core 0 as a *participant* - a peer's write
    // shipped to core 0 makes core 0 the owner, not a participant of
    // somebody else's transaction. A cross-owner FK write reverses the
    // roles: the peer coordinates and core 0 holds the intent, so core 0
    // has to answer prepare and decide. Declared after `executor`, whose
    // seams it holds.
    std::optional<Txn2pcServer> txn2pc_server;
    std::unique_ptr<CoreRuntime> peer;
    catalog::Oid oid = 0;
    catalog::SysTableRow row{};

    sched::RealRingTransport& ring() { return transport->value(); }

    // Core 0's WAL, borrowed from the fixture (AM-S0). Since the peer
    // attaches to core 0's stream rather than opening its own, **core 0 is
    // the only thing that can drain it** - and in this rig core 0 is a bare
    // scheduler rather than a `CoreRuntime`, so the drain has to be run by
    // hand. Null in a rig opened before that wiring existed.
    wal::WalManager* shared_wal = nullptr;

    // One turn of both reactors, the peer first: a message core 0 sent
    // last turn is handled before core 0 polls anything parked on it.
    //
    // **And the group committer after them**, which is what
    // `Expeditor::Serve` installs as the scheduler's post-task hook. It is
    // not bookkeeping: a committing statement *parks* instead of syncing on
    // its own stack (`command_dispatcher.hpp`'s `pending_lsn`), and the
    // drain is what it parks *for*. Without it a peer's commit never wakes
    // and `Drive` spends its whole round budget polling a task that cannot
    // finish - which is exactly what every rig cell did the moment the peer
    // stopped owning its own stream.
    // The hook is installed in `CoreRuntime::Run()`, and this rig never
    // calls it - it drives `scheduler().RunOnce()` directly - so the peer's
    // drain is run here for it. Both managers: the peer's attached one is
    // what wakes its parked commit, and core 0's owning one is what
    // actually reaches the device, and in this rig core 0 has no runtime to
    // do it.
    void Pump(int rounds = 1) {
        for (int i = 0; i < rounds; ++i) {
            peer->scheduler().RunOnce();
            (void)peer->wal().DrainOnce();
            core0->RunOnce();
            if (shared_wal != nullptr) (void)shared_wal->DrainOnce();
        }
    }
    // Core 0's statement as the coroutine its reactor would poll. Never
    // polled here: the tests poll it between turns, so they see it parked.
    std::unique_ptr<sched::CoroTask> Start(const char* sql, DispatchOutcome& out,
                                           Session* session = nullptr) {
        return sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                   dispatcher->DispatchAsync(sql, session, &out));
    }
    // The **peer's** statement as its reactor would poll it (AI-T2). The
    // rig's other cells drive core 0 because that is where a shipped
    // statement starts; a cross-owner INSERT starts on the owner, and the
    // owner is the peer.
    std::unique_ptr<sched::CoroTask> StartOnPeer(const char* sql, DispatchOutcome& out,
                                                 Session* session = nullptr) {
        return sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                   peer->dispatcher().DispatchAsync(sql, session, &out));
    }
    // **Turns until `done` answers true, bounded by a deadline** - and
    // `max_rounds` is only the floor under it (AM-S0).
    //
    // A round count was a proxy for progress while every wait a statement
    // could take was resolved by something this loop itself did: a ring
    // message, a build reply, an inline `Sync()`. Under one stream that
    // stopped being true. A committing statement parks on `IsDurable`, and
    // the only thing that moves that watermark on an attached manager is
    // core 0's **writer thread** taking an `fdatasync` -
    // `WalManager::RequestSyncNow` asks and does not wait, deliberately,
    // because a drain that blocked would hold the reactor for another
    // thread's I/O. 256 tight rounds run out in well under the fsync they
    // are waiting for, so every shipped write parked forever the moment the
    // peer stopped owning its stream: `executed=0 running=1 waiting=1`, an
    // engine doing exactly what it should against a rig measuring turns
    // where it now has to measure time.
    //
    // Past the floor the loop sleeps rather than spins, so the writer gets
    // a CPU instead of racing this thread for one.
    template <typename Turn, typename Done>
    bool TurnUntil(Turn turn, Done done, int max_rounds) {
        const auto deadline = std::chrono::steady_clock::now() + kDriveCeiling;
        for (int i = 0;; ++i) {
            if (done()) return true;
            turn();
            if (i < max_rounds) continue;
            // `done()` once more, not `false`: the `turn()` just above may be
            // the one that finished the work, and reporting a hang for it
            // would fail a cell that succeeded.
            if (std::chrono::steady_clock::now() >= deadline) return done();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    // `TurnUntil` over `Pump()`, which is what all but one caller wants.
    template <typename Done>
    bool PumpUntil(Done done, int max_rounds = 256) {
        return TurnUntil([this] { Pump(); }, done, max_rounds);
    }
    // Polls `statement` between turns until it finishes; false if it does
    // not within `TurnUntil`'s bound.
    bool Drive(sched::CoroTask& statement, int max_rounds = 256) {
        return TurnUntil([this] { Pump(); },
                         [&statement] { return statement.Poll() == sched::PollResult::kDone; },
                         max_rounds);
    }
    // Long enough that a slow device's sync is never mistaken for a hang,
    // short enough that a real hang fails the cell rather than the suite's
    // timeout. `Drive`'s callers all assert the result; the `PumpUntil`
    // sites discard it and assert the condition itself on the next line, so
    // this bound is only ever reached on a defect either way - but it is
    // reached once per call, and a cell with several of them pays for each.
    static constexpr std::chrono::milliseconds kDriveCeiling{5000};
};

void CoreRuntimeTest::OpenForeignIndexRig(ForeignIndexRig& rig, const char* table) {
    // The full payload, not 256: a shipped statement's request and reply
    // each fill exactly one slot (statement_ship_service.hpp's
    // static_asserts), so a narrower ring cannot carry one.
    auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16,
                                                      sched::kCoreRingPayloadBytes);
    ASSERT_TRUE(transport.ok()) << transport.status().message();
    rig.transport.emplace(std::move(transport));
    rig.core0.emplace(rig.clock, rig.io0);
    ASSERT_TRUE(rig.core0->AttachTransport(&rig.ring(), 0).ok());
    ASSERT_TRUE(rig.core0
                    ->RegisterMessageHandler(sched::RingMessageKind::kAnchorWrite,
                                             [](const sched::MessageHeader&,
                                                std::span<const std::byte>) {})
                    .ok());

    rig.catalog2.emplace(*core0_store_, storage::kDefaultInlineCellWidth, /*core_count=*/2);
    rig.catalog2->SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    // What Expeditor's invalidation hook does before any peer is told
    // (index_build_service.hpp's ordering): the catalog pages are unlogged
    // here, so nothing else puts the row on the device the owner re-reads
    // at `done`.
    rig.catalog2->SetInvalidationHook([this] { FlushCatalog(); });
    auto oid = rig.catalog2->CreateTable(catalog::kNamespacePublic, table, TwoColumnSchema(),
                                         catalog::ClusteredType::kBtree);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    rig.oid = oid.value();
    auto row = rig.catalog2->GetSysTableRow(rig.oid);
    ASSERT_TRUE(row.ok());
    ASSERT_EQ(row.value().owner_core, 1u);
    rig.row = row.value();
    ASSERT_TRUE(core0_store_->Sync().ok());

    CoreRuntime::Config config = ConfigFor(1);
    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    rig.peer = std::move(peer.value());
    // The stream the peer just attached to, so `Pump` can drain it.
    rig.shared_wal = core0_wal_.get();
    ASSERT_TRUE(rig.peer->AttachTransport(rig.ring()).ok());
    rig.peer->GrantRelationFault(RelationFaultExtentOf(rig.row, storage::kDefaultExtentPages));
    const PageId pages[] = {rig.row.desc_page_id, rig.row.anchor_page_id};
    rig.peer->GrantRelationWrite(pages);
    auto first = rig.catalog2->AllocateRowIdRange(rig.oid, 16);
    ASSERT_TRUE(first.ok());
    rig.peer->row_id_leases().Grant(rig.oid, first.value(), 16);

    // One transaction-id sequence: the peer's lease is carved from it, and
    // core 0's manager draws from it.
    rig.ids.emplace(core0_->superblock);
    auto block = rig.ids->Carve(16);
    ASSERT_TRUE(block.ok());
    rig.peer->trx_id_lease().Grant(block.value().first, block.value().count);
    rig.undo.emplace(*core0_store_, /*wal=*/nullptr);
    rig.txns.emplace(*rig.ids, *rig.undo, *core0_store_, /*wal=*/nullptr);
    rig.cabins0.emplace();
    rig.dispatcher.emplace(core0_->superblock, *rig.catalog2, *core0_store_, /*log=*/nullptr,
                           &rig.clock, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                           exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                           /*access_statistics=*/false, &*rig.cabins0, &*rig.txns,
                           txn::IsolationLevel::kReadCommitted, /*core_id=*/0);
    rig.client.emplace(*rig.core0, rig.ring(), rig.clock);
    ASSERT_TRUE(rig.client->RegisterReplyReceiver().ok());
    rig.dispatcher->SetIndexBuilds(&*rig.client);

    // Core 0's half of a peer-owned relation's CREATE ASSERTION (PW1c-6c).
    // The owner's half is the peer's own, wired by `AttachTransport` above.
    rig.assertion_client.emplace(*rig.core0, rig.ring(), rig.clock);
    ASSERT_TRUE(rig.assertion_client->RegisterReplyReceiver().ok());
    rig.dispatcher->SetAssertionBuilds(&*rig.assertion_client);

    // Core 0's arrival-core half of statement shipping (SS2). The owner's
    // half is the peer's own, wired by `AttachTransport` above - which is
    // the production wiring, not a fixture's.
    rig.ship.emplace(/*core_id=*/0, *rig.core0, rig.ring(), rig.clock);
    ASSERT_TRUE(rig.ship->RegisterReplyReceiver().ok());
    rig.dispatcher->SetStatementShip(&*rig.ship);

    // **And core 0's coordinator half** (R6-8): without it `MayEnrolShip`
    // refuses and a write inside a transaction keeps the refusal it had,
    // which is the single-core instance's behaviour rather than this rig's.
    // The peer's participant half was wired by `AttachTransport` above,
    // production's own wiring.
    rig.txn2pc.emplace(/*core_id=*/0, *rig.core0, rig.ring(), rig.clock);
    ASSERT_TRUE(rig.txn2pc->RegisterReplyReceivers().ok());
    rig.dispatcher->SetTxn2pc(&*rig.txn2pc);

    // **Core 0's foreign-key probe halves** (AI-T2, fk_probe_service.hpp).
    // The client first and the receiver before the pointer, which is the
    // ordering `core_runtime.cpp` states: a reply cannot arrive before
    // there is somewhere to deliver it.
    rig.fk_client.emplace(/*core_id=*/0, *rig.core0, rig.ring(), rig.clock);
    ASSERT_TRUE(rig.fk_client->RegisterReplyReceiver().ok());
    rig.dispatcher->SetFkProbes(&*rig.fk_client);
    rig.fk_server.emplace(*rig.catalog2, *core0_store_, /*core_id=*/0, rig.fk_intents,
                          rig.fk_pending_deletes, *rig.core0, rig.ring(), &*rig.txns,
                          /*log=*/nullptr);
    ASSERT_TRUE(rig.core0
                    ->RegisterMessageHandler(
                        sched::RingMessageKind::kFkProbeRequest,
                        [&rig](const sched::MessageHeader& header,
                               std::span<const std::byte> payload) {
                            rig.fk_server->OnRequest(header, payload);
                        })
                    .ok());
    // The parent side's own half (AH-T3): core 0's DELETE consults what a
    // foreign transaction is relying on before it may proceed.
    rig.dispatcher->SetFkIntents(&rig.fk_intents);
    rig.dispatcher->SetFkPendingDeletes(&rig.fk_pending_deletes);

    // **Core 0's owner half of statement shipping** (CB4). Production wires
    // both halves on every core (`CoreRuntime::AttachTransport`); this rig
    // built core 0 by hand and so has to do the same, or a DDL a peer ships
    // under CR5 reaches a core that never registered the handler and times
    // out instead of running.
    rig.executor.emplace(/*core_id=*/0, *rig.dispatcher, *rig.core0, rig.clock,
                         /*log=*/nullptr, /*wal=*/nullptr);
    rig.ship_server.emplace(/*core_id=*/0, *rig.core0, rig.ring(), rig.executor->Seam(),
                            /*log=*/nullptr);
    ASSERT_TRUE(rig.core0
                    ->RegisterMessageHandler(
                        sched::RingMessageKind::kShippedStatementRequest,
                        [&rig](const sched::MessageHeader& header,
                               std::span<const std::byte> payload) {
                            rig.ship_server->OnRequest(header, payload);
                        })
                    .ok());

    // **Core 0's participant half of 2PC** (AI-T2), production's own wiring
    // (`CoreRuntime::AttachTransport`) done by hand, **including the FK
    // release that rides the decide** - which is where the intent ends and
    // the only thing that ends it (AH-R5).
    rig.txn2pc_server.emplace(/*core_id=*/0, *rig.core0, rig.ring(),
                              rig.executor->PrepareSeam(), rig.executor->DecideSeam(),
                              rig.executor->ResolveSeam(), /*log=*/nullptr);
    ASSERT_TRUE(rig.txn2pc_server->RegisterResolveReplyReceiver().ok());
    rig.executor->SetTxn2pcServer(&*rig.txn2pc_server);
    ASSERT_TRUE(rig.core0
                    ->RegisterMessageHandler(
                        sched::RingMessageKind::kTxnPrepareRequest,
                        [&rig](const sched::MessageHeader& header,
                               std::span<const std::byte> payload) {
                            rig.txn2pc_server->OnPrepare(header, payload);
                        })
                    .ok());
    ASSERT_TRUE(rig.core0
                    ->RegisterMessageHandler(
                        sched::RingMessageKind::kTxnDecideRequest,
                        [&rig](const sched::MessageHeader& header,
                               std::span<const std::byte> payload) {
                            rig.txn2pc_server->OnDecide(header, payload);
                            TxnDecideRequestPayload decide{};
                            if (payload.size() != sizeof(decide)) return;
                            std::memcpy(&decide, payload.data(), sizeof(decide));
                            rig.fk_server->ReleaseIntents(header.src_core, decide.session_id);
                        })
                    .ok());

    // **The owner's group-commit drain**, which `CoreRuntime::Run()`
    // installs and this rig has to install itself, because it pumps
    // `RunOnce()` rather than running the reactor. Without it a statement
    // that stages a commit on the owner parks on `IsDurable` forever - and
    // that is precisely what a shipped write does, since joining the
    // owner's group commit is the whole point of shipping (D3). The
    // index-build tests never needed it: their write happens on core 0.
    rig.peer->scheduler().SetPostTaskHook([&rig] {
        const bool staged = rig.peer->wal().HasPendingGroupCommits();
        (void)rig.peer->wal().DrainOnce();
        return staged;
    });

    // Rows the owner wrote and core 0 never saw - what the build must find.
    const std::string ins =
        rig.peer->dispatcher()
            .Dispatch("INSERT INTO " + std::string(table) + " VALUES (10), (20), (30)")
            .response;
    ASSERT_NE(ins.rfind("ERR", 0), 0u) << ins;
}

void CoreRuntimeTest::OpenCrossOwnerFkPair(ForeignIndexRig& rig, const std::string& base) {
    rig.catalog2->SetPlacementPolicy(catalog::PlacementPolicy::kCreatingCore);
    ASSERT_EQ(rig.dispatcher
                  ->Dispatch("CREATE TABLE " + base + "p (id int64, v int64) BTREE")
                  .response.substr(0, 3),
              "CRE");
    rig.catalog2->SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    ASSERT_EQ(rig.dispatcher
                  ->Dispatch("CREATE TABLE " + base + "c (id int64, pid int64 REFERENCES " +
                             base + "p) BTREE")
                  .response.substr(0, 3),
              "CRE");
    auto parent_oid = rig.catalog2->FindTableOidByName(base + "p");
    ASSERT_TRUE(parent_oid.ok()) << parent_oid.status().message();
    auto child_oid = rig.catalog2->FindTableOidByName(base + "c");
    ASSERT_TRUE(child_oid.ok()) << child_oid.status().message();
    auto parent_row = rig.catalog2->GetSysTableRow(parent_oid.value());
    ASSERT_TRUE(parent_row.ok());
    ASSERT_EQ(parent_row.value().owner_core, 0u) << "the parent must be core 0's to cross";
    // Sync before funding: `AdmitWritePages` faults each granted page for
    // read before restamping it, and a creation page still only in core 0's
    // cache abandons the whole grant silently.
    ASSERT_TRUE(core0_store_->Sync().ok());
    rig.peer->InvalidateCatalog();
    FundPeerForRelation(rig, child_oid.value());
}

void CoreRuntimeTest::FundPeerForRelation(ForeignIndexRig& rig, catalog::Oid oid) {
    auto row = rig.catalog2->GetSysTableRow(oid);
    ASSERT_TRUE(row.ok()) << row.status().message();
    ASSERT_EQ(row.value().owner_core, 1u) << "only a peer-owned relation needs funding here";
    rig.peer->GrantRelationFault(RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));
    // The var-heap head too, where the schema has one (a `varchar` column):
    // the rights probe asks for every page the relation's first write may
    // touch, and a relation with no spill has `kInvalidPageId` there.
    std::vector<PageId> pages = {row.value().desc_page_id, row.value().anchor_page_id};
    if (row.value().varheap_page_id != kInvalidPageId) {
        pages.push_back(row.value().varheap_page_id);
    }
    rig.peer->GrantRelationWrite(pages);
    ASSERT_TRUE(rig.peer->store().MayWrite(row.value().desc_page_id));
    auto first = rig.catalog2->AllocateRowIdRange(oid, 16);
    ASSERT_TRUE(first.ok()) << first.status().message();
    rig.peer->row_id_leases().Grant(oid, first.value(), 16);
}

// ---- CR5 / CB4-CB6: a peer routes DDL to core 0 --------------------------
//
// PW4 refused the whole verb on a peer, because every target of
// `CREATE`/`ALTER`/`DROP` writes state only the system core may write. CR5
// keeps that premise and changes the answer: the peer **ships** the
// statement to core 0 and waits, so the refusal survives for the cases the
// route cannot serve rather than for all of them. `MayShip`'s conditions
// are what decide which of the two a client gets.

TEST_F(CoreRuntimeTest, APeerWithNoShipClientStillRefusesDdlAndPoisons) {
    // No transport, so no ship client and no parkable path: `MayShip` is
    // false on all three counts and the statement takes PW4's refusal
    // unchanged. This is the arm that keeps a single-core instance and
    // every fixture byte-identical.
    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    const auto out = peer.value()->dispatcher().Dispatch("CREATE TABLE t (id int64, v int64)");
    EXPECT_EQ(out.response.rfind("ERR", 0), 0u) << out.response;
    EXPECT_NE(out.response.find("core 1"), std::string::npos)
        << "the refusal was not the peer-DDL one: " << out.response;
}

TEST_F(CoreRuntimeTest, APeersDdlRunsOnCoreZeroAndItsOwnNextStatementSeesIt) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "cb4_base");

    // The peer's own statement, driven as its reactor would drive it: it
    // parks on the ship, and core 0 runs the DDL under its own catalog.
    DispatchOutcome out;
    auto statement = sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        rig.peer->dispatcher().DispatchAsync("CREATE TABLE cb4_new (id int64, v int64)", nullptr,
                                             &out));
    ASSERT_TRUE(rig.Drive(*statement)) << "the shipped DDL never finished";
    EXPECT_NE(out.response.rfind("ERR", 0), 0u) << out.response;

    // It ran where the catalog is writable, which is the whole point: core
    // 0's own catalog has the relation, and the peer wrote no page of it.
    auto oid = rig.catalog2->FindTableOidByName("cb4_new");
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    EXPECT_FALSE(rig.peer->store().MayWrite(catalog::kCatalogPageTables));

    // **CB6: the peer sees its own DDL.** Core 0's invalidation broadcast is
    // a submitted task and nothing orders it against the reply to this ship,
    // so without CB6 the session that typed the statement could be told by
    // its own core that the relation does not exist. The peer drops its
    // catalog cache when the answer arrives, and core 0 flushed the pages
    // before answering, so the next statement on this core resolves.
    const std::string described = rig.peer->dispatcher().Dispatch("DESCRIBE cb4_new").response;
    EXPECT_NE(described.rfind("ERR", 0), 0u)
        << "a peer could not resolve the DDL it had just been told succeeded: " << described;
}

TEST_F(CoreRuntimeTest, ADdlInsideAnExplicitTransactionIsStillRefusedOnAPeer) {
    // CB5: shipping inside a transaction would enrol core 0 as a 2PC
    // participant, and `known-gaps.md` still records ALTER, cabin, pattern,
    // assertion and FK as non-transactional. A participant with nothing to
    // prepare is a worse failure than a refusal the client can see, so the
    // transactional case keeps PW4's answer and poisons.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "cb5_base");

    Session session;
    ASSERT_NE(rig.peer->dispatcher().Dispatch("BEGIN", &session).response.rfind("ERR", 0), 0u);
    const auto out =
        rig.peer->dispatcher().Dispatch("CREATE TABLE cb5_new (id int64)", &session);
    EXPECT_EQ(out.response.rfind("ERR", 0), 0u) << out.response;
    EXPECT_TRUE(session.failed()) << "the refusal did not poison the transaction";
}

TEST_F(CoreRuntimeTest, ACreateIndexOnAPeerRelationIsBuiltByTheOwnerAndPublishedByCore0) {
    // PW1c-6b-3's happy path through HandleIndex itself: core 0's statement
    // parks on the owner's build, the owner refuses the relation's writes
    // by name meanwhile, and the row core 0 then commits names the owner's
    // root - which the owner's own probes answer through.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "rotated_ix");

    DispatchOutcome out;
    auto statement = rig.Start("CREATE INDEX rix ON rotated_ix (v)", out);
    // Driven a turn at a time so the window can be observed: it opens on
    // the owner the turn the request lands and holds until core 0's `done`.
    bool window_seen = false;
    int rounds = 0;
    while (statement->Poll() != sched::PollResult::kDone) {
        rig.Pump();
        if (!window_seen && rig.peer->pending_index_builds().Covers(rig.oid)) {
            window_seen = true;
            EXPECT_EQ(rig.client->waiting(), 1u);
            const std::string refused =
                rig.peer->dispatcher().Dispatch("INSERT INTO rotated_ix VALUES (40)").response;
            EXPECT_NE(refused.find("TXN_CONFLICT"), std::string::npos) << refused;
            EXPECT_NE(refused.find("PW1c-6b"), std::string::npos) << refused;
            // And the owner's SHOW META shows the window while it is open.
            const std::string meta = rig.peer->dispatcher().Dispatch("SHOW META").response;
            EXPECT_NE(meta.find(" index_build_windows=1 "), std::string::npos) << meta;
        }
        ASSERT_LT(++rounds, 256) << "the statement did not finish: " << out.response;
    }
    EXPECT_TRUE(window_seen) << "the owner never opened a window";
    ASSERT_EQ(out.response.rfind("CREATED INDEX", 0), 0u) << out.response;
    EXPECT_NE(out.response.find("built_by_core=1"), std::string::npos) << out.response;
    EXPECT_EQ(rig.client->waiting(), 0u);
    EXPECT_EQ(rig.peer->index_builds()->builds(), 1u);
    rig.Pump(8);  // `done(committed)` is in flight
    EXPECT_TRUE(rig.peer->pending_index_builds().empty());
    {
        const std::string meta = rig.peer->dispatcher().Dispatch("SHOW META").response;
        EXPECT_NE(meta.find(" index_build_windows=0 "), std::string::npos) << meta;
    }

    // The row names the owner's root, and the owner's anchor slot agrees.
    auto published = rig.catalog2->FindIndexByName("rix");
    ASSERT_TRUE(published.ok()) << published.status().message();
    ASSERT_NE(published.value().root_page_id, kInvalidPageId);
    {
        auto anchor = rig.peer->store().GetForRead(rig.row.anchor_page_id);
        ASSERT_TRUE(anchor.ok()) << anchor.status().message();
        auto slot =
            storage::AnchorIndexRoot(anchor.value().bytes(), published.value().index_oid);
        ASSERT_TRUE(slot.ok()) << slot.status().message();
        EXPECT_EQ(slot.value(), published.value().root_page_id);
    }

    // `done(committed)` dropped the owner's cache: the index is in its view
    // with the root it built, and a keyed read answers *through it* - the
    // plan says so, not just the rows.
    auto access = rig.peer->catalog().InitTableAccess(rig.oid);
    ASSERT_TRUE(access.ok()) << access.status().message();
    ASSERT_EQ(access.value()->indexes.size(), 1u);
    EXPECT_EQ(access.value()->indexes[0].root_page_id, published.value().root_page_id);
    const std::string plan =
        rig.peer->dispatcher().Dispatch("ANALYZE SELECT * FROM rotated_ix WHERE v = 20").response;
    EXPECT_NE(plan.find("IndexProbe"), std::string::npos) << plan;
    const std::string sel =
        rig.peer->dispatcher().Dispatch("SELECT * FROM rotated_ix WHERE v = 20").response;
    EXPECT_NE(sel.find(",20"), std::string::npos) << sel;
    EXPECT_EQ(sel.find(",10"), std::string::npos) << sel;

    // The window is closed, and the shape gate's indexed arm is lifted
    // (PW1c-6b-4): the INSERT the window refused is now admitted, the
    // owner maintains the index it built, and a keyed read finds the new
    // row through it.
    const std::string after =
        rig.peer->dispatcher().Dispatch("INSERT INTO rotated_ix VALUES (40)").response;
    EXPECT_NE(after.rfind("ERR", 0), 0u) << "the gate did not lift: " << after;
    // The read-through that proves the INSERT was maintained is
    // AnOwnerMaintainsInsertsIntoAPeerBuiltIndexAndReadsAnswerWhole's, over
    // seven values; here the one new fact is that the write is admitted.
}

TEST_F(CoreRuntimeTest, AnOwnerMaintainsInsertsIntoAPeerBuiltIndexAndReadsAnswerWhole) {
    // The PW1c-6b-4 e2e for the maintained path: after the owner builds
    // and core 0 publishes, a run of INSERTs on the owner each maintains
    // the index (a local write - owner-stamped leaves, the granted anchor
    // on a split), and every keyed read answers whole - the pre-build rows
    // the backfill covered and the post-build rows maintenance added, and
    // a value with no row answering none without opening the relation.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "maintained_ix");

    DispatchOutcome out;
    auto statement = rig.Start("CREATE INDEX mix ON maintained_ix (v)", out);
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    ASSERT_EQ(out.response.rfind("CREATED INDEX", 0), 0u) << out.response;
    rig.Pump(8);  // `done(committed)` lands, the owner's cache drops
    EXPECT_TRUE(rig.peer->pending_index_builds().empty());

    // Entries before: the backfill covered the three rows the rig wrote.
    const std::string before =
        rig.peer->dispatcher().Dispatch("SHOW INDEXES").response;
    EXPECT_NE(before.find("entries=3"), std::string::npos) << before;

    // A run of INSERTs, each admitted and maintained.
    for (int v : {40, 50, 60, 70}) {
        const std::string ins =
            rig.peer->dispatcher()
                .Dispatch("INSERT INTO maintained_ix VALUES (" + std::to_string(v) + ")")
                .response;
        ASSERT_NE(ins.rfind("ERR", 0), 0u) << ins;
    }
    const std::string after =
        rig.peer->dispatcher().Dispatch("SHOW INDEXES").response;
    EXPECT_NE(after.find("entries=7"), std::string::npos)
        << "the four INSERTs were not all maintained: " << after;

    // Every value present reads whole through the index; an absent one
    // answers none.
    for (int v : {10, 20, 30, 40, 50, 60, 70}) {
        const std::string sel =
            rig.peer->dispatcher()
                .Dispatch("SELECT * FROM maintained_ix WHERE v = " + std::to_string(v))
                .response;
        EXPECT_NE(sel.find("," + std::to_string(v)), std::string::npos) << sel;
    }
    const std::string plan =
        rig.peer->dispatcher().Dispatch("ANALYZE SELECT * FROM maintained_ix WHERE v = 60")
            .response;
    EXPECT_NE(plan.find("IndexProbe"), std::string::npos) << plan;
    const std::string absent =
        rig.peer->dispatcher().Dispatch("SELECT * FROM maintained_ix WHERE v = 999").response;
    EXPECT_NE(absent.rfind("ERR", 0), 0u) << absent;
    EXPECT_EQ(absent.find(",999"), std::string::npos) << absent;
}

TEST_F(CoreRuntimeTest, DropIndexOnAPeerRelationIsRefusedInsideATransactionAndAdmittedInAutocommit) {
    // The gate lift's cross-core DROP hole (PW1c-6b-4, the review's
    // finding): inside a transaction the owner would drop the index from
    // its view - DT9's in-flight predicate is core-local and cannot see
    // core 0's deleter - and maintain nothing before a ROLLBACK restores
    // it, so it is refused by name; autocommit keeps only the general DDL
    // commit-failure window and is admitted.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "droptest_ix");

    DispatchOutcome out;
    auto statement = rig.Start("CREATE INDEX dix ON droptest_ix (v)", out);
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    ASSERT_EQ(out.response.rfind("CREATED INDEX", 0), 0u) << out.response;
    rig.Pump(8);

    // Inside a transaction: refused by name and byte, the transaction
    // untouched, and the index still present.
    Session session(txn::IsolationLevel::kReadCommitted);
    ASSERT_NE(rig.dispatcher->Dispatch("BEGIN", &session).response.rfind("ERR", 0), 0u);
    const std::string refused =
        rig.dispatcher->Dispatch("DROP INDEX dix", &session).response;
    EXPECT_EQ(refused.rfind("ERR", 0), 0u) << refused;
    EXPECT_NE(refused.find("PW1c-6b-4"), std::string::npos) << refused;
    EXPECT_NE(refused.find("at byte"), std::string::npos) << refused;
    EXPECT_FALSE(session.failed()) << "refused before anything was written";
    ASSERT_NE(rig.dispatcher->Dispatch("ROLLBACK", &session).response.rfind("ERR", 0), 0u);
    EXPECT_TRUE(rig.catalog2->FindIndexByName("dix").ok()) << "the index was dropped anyway";

    // Autocommit: admitted - the row is delete-marked and committed in one
    // statement, so the reachable window closes with it.
    const std::string dropped = rig.dispatcher->Dispatch("DROP INDEX dix").response;
    EXPECT_NE(dropped.rfind("ERR", 0), 0u) << dropped;
    EXPECT_NE(dropped.find("DROPPED INDEX"), std::string::npos) << dropped;
}

TEST_F(CoreRuntimeTest, ACreateIndexOnAPeerRelationIsRefusedInsideATransaction) {
    // §7c: inside an explicit transaction the owner's refusal window would
    // last until the client's COMMIT, so the statement is refused by name
    // before anything is sent - no request, no oid, no window, and the
    // transaction is not poisoned.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "rotated_ix_txn");

    Session session(txn::IsolationLevel::kReadCommitted);
    const std::string begun = rig.dispatcher->Dispatch("BEGIN", &session).response;
    ASSERT_NE(begun.rfind("ERR", 0), 0u) << begun;
    // The refusal sits before PrepareIndexDef, which is what keeps the
    // index oid from being issued: the sequence must not move.
    auto before = rig.catalog2->GetSysTableRow(catalog::kSysIndexesTable);
    ASSERT_TRUE(before.ok()) << before.status().message();
    DispatchOutcome out;
    auto statement = rig.Start("CREATE INDEX rix ON rotated_ix_txn (v)", out, &session);
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    auto after = rig.catalog2->GetSysTableRow(catalog::kSysIndexesTable);
    ASSERT_TRUE(after.ok()) << after.status().message();
    EXPECT_EQ(after.value().next_id, before.value().next_id) << "an oid was burned";
    EXPECT_EQ(out.response.rfind("ERR", 0), 0u) << out.response;
    EXPECT_NE(out.response.find("PW1c-6b-3"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find("COMMIT"), std::string::npos) << out.response;
    EXPECT_EQ(rig.client->waiting(), 0u);
    EXPECT_FALSE(session.failed()) << "refused before anything was written";
    rig.Pump(8);
    EXPECT_EQ(rig.peer->index_builds()->builds(), 0u);
    EXPECT_TRUE(rig.peer->pending_index_builds().empty());
    EXPECT_FALSE(rig.catalog2->FindIndexByName("rix").ok());
    const std::string ended = rig.dispatcher->Dispatch("ROLLBACK", &session).response;
    EXPECT_NE(ended.rfind("ERR", 0), 0u) << ended;
}

TEST_F(CoreRuntimeTest, ACreateIndexOnAPeerRelationTimesOutAndTellsTheOwner) {
    // Core 0 under a manual clock and the owner never run: the statement
    // parks until the deadline, ends retryably with no row, and the
    // `done(aborted)` it sends reaches the owner behind the late request -
    // so the tree the owner then builds is orphaned and its window closes
    // at once, not at the ceiling.
    sched::ManualClock core0_clock;
    ForeignIndexRig rig(core0_clock);
    OpenForeignIndexRig(rig, "rotated_ix_late");

    DispatchOutcome out;
    auto statement = rig.Start("CREATE INDEX rix ON rotated_ix_late (v)", out);
    for (int i = 0; i < 8; ++i) {
        EXPECT_NE(statement->Poll(), sched::PollResult::kDone) << out.response;
        rig.core0->RunOnce();  // core 0 alone: the request leaves, nothing answers
    }
    EXPECT_EQ(rig.client->waiting(), 1u);
    core0_clock.Advance(kIndexBuildReplyDeadlineNs);
    ASSERT_EQ(statement->Poll(), sched::PollResult::kDone);
    EXPECT_EQ(out.response.rfind("ERR", 0), 0u) << out.response;
    EXPECT_NE(out.response.find("TXN_CONFLICT"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find("did not reply"), std::string::npos) << out.response;
    EXPECT_EQ(rig.client->waiting(), 0u);
    EXPECT_FALSE(rig.catalog2->FindIndexByName("rix").ok());

    // Now the owner runs. The request, first in the ring, opens a window
    // and builds; the `done(aborted)` behind it closes the window; the
    // reply finds no waiter on core 0 and is answered with a second
    // `done(aborted)`, which the owner ignores.
    rig.Pump(40);
    EXPECT_EQ(rig.peer->index_builds()->builds(), 1u);
    EXPECT_TRUE(rig.peer->pending_index_builds().empty());
    auto access = rig.peer->catalog().InitTableAccess(rig.oid);
    ASSERT_TRUE(access.ok()) << access.status().message();
    EXPECT_TRUE(access.value()->indexes.empty()) << "an abandoned build publishes nothing";
    // The window closed, so the owner writes again - the relation has no
    // index in its view, and the orphaned tree's slot is nobody's.
    const std::string after =
        rig.peer->dispatcher().Dispatch("INSERT INTO rotated_ix_late VALUES (40)").response;
    EXPECT_NE(after.rfind("ERR", 0), 0u) << after;
}

TEST_F(CoreRuntimeTest, ASecondCreateIndexOnTheRelationIsRefusedByTheOwnerWhileTheFirstBuilds) {
    // Two statements parked at once on core 0's dispatcher. The owner
    // refuses the second by name (one window per relation), the second
    // ends retryably with its own `done(aborted)` - which names an index
    // the owner has no window for and ignores - and the first is
    // untouched by it.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "rotated_ix_two");

    DispatchOutcome first_out;
    DispatchOutcome second_out;
    auto first = rig.Start("CREATE INDEX rix ON rotated_ix_two (v)", first_out);
    EXPECT_NE(first->Poll(), sched::PollResult::kDone) << first_out.response;
    auto second = rig.Start("CREATE INDEX rix_second ON rotated_ix_two (v)", second_out);
    EXPECT_NE(second->Poll(), sched::PollResult::kDone) << second_out.response;
    EXPECT_EQ(rig.client->waiting(), 2u);

    bool first_done = false;
    bool second_done = false;
    int rounds = 0;
    while (!first_done || !second_done) {
        if (!first_done) first_done = first->Poll() == sched::PollResult::kDone;
        if (!second_done) second_done = second->Poll() == sched::PollResult::kDone;
        rig.Pump();
        ASSERT_LT(++rounds, 256) << first_out.response << " / " << second_out.response;
    }
    EXPECT_EQ(second_out.response.rfind("ERR", 0), 0u) << second_out.response;
    EXPECT_NE(second_out.response.find("TXN_CONFLICT"), std::string::npos)
        << second_out.response;
    EXPECT_NE(second_out.response.find("already has an index build pending"), std::string::npos)
        << second_out.response;
    EXPECT_NE(second_out.response.find("refused the build"), std::string::npos)
        << second_out.response;
    ASSERT_EQ(first_out.response.rfind("CREATED INDEX", 0), 0u) << first_out.response;
    rig.Pump(8);
    EXPECT_EQ(rig.client->waiting(), 0u);
    EXPECT_EQ(rig.peer->index_builds()->builds(), 1u);
    EXPECT_TRUE(rig.peer->pending_index_builds().empty());
    EXPECT_TRUE(rig.catalog2->FindIndexByName("rix").ok());
    EXPECT_FALSE(rig.catalog2->FindIndexByName("rix_second").ok());
    auto access = rig.peer->catalog().InitTableAccess(rig.oid);
    ASSERT_TRUE(access.ok()) << access.status().message();
    EXPECT_EQ(access.value()->indexes.size(), 1u);
}

TEST_F(CoreRuntimeTest, TheReplyToAnAbandonedRequestOrphansTheTreeAndClosesTheWindow) {
    // The receiver's no-waiter branch, which the deadline and sync-path
    // tests cannot reach - their `done(aborted)` is ahead of the reply in
    // the ring and closes the window first. Here core 0 drops its waiter
    // without a word, the owner builds and replies, and the reply itself
    // is what closes the window: now, not at the 180 s ceiling.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "rotated_ix_dropped");

    parser::Parser parser("CREATE INDEX rix ON rotated_ix_dropped (v)");
    auto parsed = parser.Parse();
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    auto def = exec::PrepareIndexDef(*rig.catalog2, std::get<parser::IndexStmt>(parsed.value()),
                                     nullptr, catalog::Catalog::AnchorSeed::kByOwner);
    ASSERT_TRUE(def.ok()) << def.status().message();
    ASSERT_TRUE(rig.client->Request(/*owner_core=*/1, /*request_id=*/77, def.value()).ok());
    rig.client->Close(77);
    EXPECT_EQ(rig.client->waiting(), 0u);

    bool window_seen = false;
    for (int i = 0; i < 40; ++i) {
        rig.Pump();
        window_seen = window_seen || rig.peer->pending_index_builds().Covers(rig.oid);
    }
    EXPECT_TRUE(window_seen) << "the owner never opened a window";
    EXPECT_EQ(rig.peer->index_builds()->builds(), 1u);
    EXPECT_TRUE(rig.peer->pending_index_builds().empty())
        << "the reply's done(aborted) did not close the window";
    auto access = rig.peer->catalog().InitTableAccess(rig.oid);
    ASSERT_TRUE(access.ok()) << access.status().message();
    EXPECT_TRUE(access.value()->indexes.empty());
}

TEST_F(CoreRuntimeTest, ACreateIndexOnAPeerRelationNeedsTheReactorPath) {
    // The synchronous Dispatch() has nothing to receive the owner's reply
    // on, so it abandons the build at once - the remote read's stance -
    // and tells the owner, whose window would otherwise wait the ceiling
    // out on a tree nobody publishes.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "rotated_ix_sync");

    const std::string reply =
        rig.dispatcher->Dispatch("CREATE INDEX rix ON rotated_ix_sync (v)").response;
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
    EXPECT_NE(reply.find("TXN_CONFLICT"), std::string::npos) << reply;
    EXPECT_NE(reply.find("reactor path"), std::string::npos) << reply;
    EXPECT_EQ(rig.client->waiting(), 0u);
    EXPECT_FALSE(rig.catalog2->FindIndexByName("rix").ok());

    // The request was queued before the refusal; the owner builds it and
    // is told to orphan it.
    rig.Pump(40);
    EXPECT_EQ(rig.peer->index_builds()->builds(), 1u);
    EXPECT_TRUE(rig.peer->pending_index_builds().empty());
    auto access = rig.peer->catalog().InitTableAccess(rig.oid);
    ASSERT_TRUE(access.ok()) << access.status().message();
    EXPECT_TRUE(access.value()->indexes.empty());
}

// ---- Statement shipping, end to end (SS2/SS3) ---------------------------
//
// The rig above is the whole instance a shipped statement crosses: core 0's
// dispatcher with an arrival-core client, and a peer whose `AttachTransport`
// wired the owner's server and executor exactly as production does. What
// these pin is the fork's contract - which statements ship, which keep the
// refusal they always had, and that a shipped one really executes on the
// core that owns the relation rather than being simulated on core 0.

TEST_F(CoreRuntimeTest, AWriteToAPeerOwnedRelationIsShippedAndTheOwnerExecutesIt) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_write");

    DispatchOutcome out;
    auto statement = rig.Start("INSERT INTO shipped_write VALUES (40)", out);
    ASSERT_TRUE(rig.Drive(*statement))
        << "response='" << out.response << "'"
        << " executed=" << rig.peer->shipped_statements()->executed()
        << " running=" << rig.peer->shipped_statements()->running()
        << " waiting=" << rig.ship->waiting();
    EXPECT_EQ(out.response.rfind("INSERTED", 0), 0u) << out.response;

    // It ran **on the owner**: the owner's executor counted it, and the row
    // is in the owner's relation - which core 0 cannot even fault.
    ASSERT_NE(rig.peer->shipped_statements(), nullptr);
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 1u);
    EXPECT_EQ(rig.peer->shipped_statements()->running(), 0u);
    const std::string rows =
        rig.peer->dispatcher().Dispatch("SELECT * FROM shipped_write").response;
    EXPECT_NE(rows.find(",40"), std::string::npos) << rows;

    // And the waiter is closed, so nothing leaks per statement.
    EXPECT_EQ(rig.ship->waiting(), 0u);
    EXPECT_EQ(rig.ship->late_executed_replies(), 0u);
    EXPECT_EQ(rig.ship->identity_mismatches(), 0u);

    // D7's two halves, each on the core it describes (SS4). The arrival
    // core reports what it sent and what came back; the owner reports what
    // it ran for other cores. Neither number exists on the other core,
    // which is what makes a reading of a multi-core instance one reading
    // per core rather than a sum of ambiguous fields.
    const std::string meta = rig.dispatcher->Dispatch("SHOW META").response;
    EXPECT_NE(meta.find(" shipped_statements=1 "), std::string::npos) << meta;
    EXPECT_NE(meta.find(" shipped_replies=1 "), std::string::npos) << meta;
    EXPECT_NE(meta.find(" shipped_refusals=0 "), std::string::npos) << meta;
    EXPECT_NE(meta.find(" shipped_waiting=0 "), std::string::npos) << meta;
    EXPECT_NE(meta.find(" shipped_wait_us_max="), std::string::npos) << meta;
    EXPECT_EQ(meta.find(" shipped_executed="), std::string::npos)
        << "core 0 ran nothing for anyone: " << meta;

    const std::string owner_meta = rig.peer->dispatcher().Dispatch("SHOW META").response;
    EXPECT_NE(owner_meta.find(" shipped_executed=1 "), std::string::npos) << owner_meta;
    EXPECT_NE(owner_meta.find(" shipped_running=0 "), std::string::npos) << owner_meta;
    EXPECT_NE(owner_meta.find(" shipped_deduped=0 "), std::string::npos) << owner_meta;
    EXPECT_NE(owner_meta.find(" shipped_early_evictions=0 "), std::string::npos) << owner_meta;
    // The owner shipped nothing itself, so its arrival-core half reads zero
    // rather than being absent - it *has* a client, it just did not use it.
    EXPECT_NE(owner_meta.find(" shipped_statements=0 "), std::string::npos) << owner_meta;

    // **And the refusal counter's two eras** (D7): core 0's write was
    // shipped, not refused, so the field that used to count it stays flat.
    // A statement shipping converts is a statement that counter no longer
    // sees, which is what makes its remaining population the 2PC evidence.
    EXPECT_NE(meta.find(" cross_core_write_refusals=0 "), std::string::npos) << meta;
}

// ---- R6-8: the cross-owner transaction, on a live path -----------------------
//
// Every row before this one was reachable only from a test that called the
// seams by hand, because `MayShip` refused inside an explicit transaction and
// so nothing ever enrolled a participant. These are the first tests in which
// a **client statement** makes a transaction cross-owner and the protocol runs
// end to end over a real ring, with the peer's participant half wired by
// `AttachTransport` exactly as production wires it.

TEST_F(CoreRuntimeTest, AWriteInsideATransactionEnrolsItsOwnerAndTheCommitRunsBothPhases) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "cross_owner");

    Session session;
    ASSERT_EQ(rig.dispatcher->Dispatch("BEGIN", &session).response.rfind("BEGIN", 0), 0u);
    ASSERT_FALSE(session.has_participants()) << "a transaction is one-owner until it is not";

    // The write the engine refused before this row. It ships, and shipping
    // inside a transaction is what enrols the owner as a participant.
    DispatchOutcome out;
    auto write = rig.Start("INSERT INTO cross_owner VALUES (41)", out, &session);
    ASSERT_TRUE(rig.Drive(*write)) << out.response;
    EXPECT_EQ(out.response.rfind("INSERTED", 0), 0u) << out.response;
    ASSERT_TRUE(session.has_participants());
    ASSERT_EQ(session.participants().size(), 1u);
    EXPECT_EQ(session.participants()[0], 1u);
    // The transaction is still open and unpoisoned: the statement went to
    // another owner and this core's half is untouched.
    EXPECT_TRUE(session.in_explicit_txn());
    EXPECT_FALSE(session.failed());

    // The owner holds it open rather than committing per statement (R6-2),
    // so nothing outside the transaction can see the row yet.
    ASSERT_NE(rig.peer->shipped_statements(), nullptr);
    EXPECT_EQ(rig.peer->shipped_statements()->enrolled(), 1u);
    const std::string before =
        rig.peer->dispatcher().Dispatch("SELECT * FROM cross_owner").response;
    EXPECT_EQ(before.find(",41"), std::string::npos) << before;

    // **The COMMIT, which is D4's two phases** - and no local statement in
    // this transaction wrote anything, so what proves it committed is the
    // owner's rows.
    DispatchOutcome commit_out;
    auto commit = rig.Start("COMMIT", commit_out, &session);
    ASSERT_TRUE(rig.Drive(*commit)) << commit_out.response;
    EXPECT_EQ(commit_out.response.rfind("COMMIT trx_id=", 0), 0u) << commit_out.response;

    EXPECT_EQ(rig.txn2pc->prepare_messages(), 1u);
    EXPECT_EQ(rig.txn2pc->decide_messages(), 1u);
    EXPECT_EQ(rig.txn2pc->phase_timeouts(), 0u);
    EXPECT_EQ(rig.txn2pc->prepare_refusals(), 0u);
    EXPECT_EQ(rig.txn2pc->decide_refusals(), 0u);
    EXPECT_EQ(rig.txn2pc->waiting(), 0u) << "both phases closed behind them";
    EXPECT_EQ(rig.peer->shipped_statements()->prepared(), 1u);
    EXPECT_EQ(rig.peer->shipped_statements()->decides_committed(), 1u);
    EXPECT_EQ(rig.peer->shipped_statements()->in_doubt(), 0u);
    EXPECT_EQ(rig.peer->shipped_statements()->enrolled(), 0u);

    const std::string rows =
        rig.peer->dispatcher().Dispatch("SELECT * FROM cross_owner").response;
    EXPECT_NE(rows.find(",41"), std::string::npos) << rows;
    // And the session is clean for the next transaction: the participant
    // list belongs to the transaction that enrolled it.
    EXPECT_FALSE(session.in_explicit_txn());
    EXPECT_FALSE(session.has_participants());
}

TEST_F(CoreRuntimeTest, EachCrossOwnerCommitLegIsTimedAndAOneOwnerCommitTimesNothing) {
    // **XF4's instrument, and its cost guard in one test.** Three results
    // files billed for per-leg times (`results-crosscore-2pc` §8,
    // `results-xd-commit-decomposition` §8, `results-xe-ack-at-append` §6),
    // and the guard the work order set on paying that bill is that a
    // one-owner commit must not read the clock even once more than before.
    // That guard is structural - every `Note` lives inside the cross-owner
    // path - and this is what makes it visible: the same dispatcher, two
    // commits, and only the second records anything.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "commit_legs");

    ASSERT_NE(rig.peer->shipped_statements(), nullptr);
    EXPECT_FALSE(rig.dispatcher->xowner_commit_stats().observed())
        << "a dispatcher that has committed nothing reported a leg";
    EXPECT_FALSE(rig.peer->shipped_statements()->commit_phase().observed());

    // **The one-owner commit.** No statement, so no participant: this is
    // the shape the guard is about, and it must leave every leg untouched.
    Session local;
    ASSERT_EQ(rig.dispatcher->Dispatch("BEGIN", &local).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(rig.dispatcher->Dispatch("COMMIT", &local).response.rfind("COMMIT", 0), 0u);
    EXPECT_FALSE(rig.dispatcher->xowner_commit_stats().observed())
        << "a one-owner commit recorded a cross-owner leg";
    EXPECT_EQ(rig.dispatcher->xowner_commit_stats().whole.count, 0u);

    // And `SHOW META` is silent about all of it - the absent-rather-than-
    // zeroed rule, which is what keeps a single-owner instance from
    // reading as "the protocol ran and cost nothing".
    const std::string quiet = rig.dispatcher->Dispatch("SHOW META").response;
    EXPECT_EQ(quiet.find("xowner_commit_n="), std::string::npos) << quiet;
    EXPECT_EQ(quiet.find("xowner_part_ack_n="), std::string::npos) << quiet;

    // **The cross-owner commit**, the same shape the test above proves
    // runs both phases.
    Session session;
    ASSERT_EQ(rig.dispatcher->Dispatch("BEGIN", &session).response.rfind("BEGIN", 0), 0u);
    DispatchOutcome out;
    auto write = rig.Start("INSERT INTO commit_legs VALUES (43)", out, &session);
    ASSERT_TRUE(rig.Drive(*write)) << out.response;
    ASSERT_TRUE(session.has_participants());

    DispatchOutcome commit_out;
    auto commit = rig.Start("COMMIT", commit_out, &session);
    ASSERT_TRUE(rig.Drive(*commit)) << commit_out.response;
    ASSERT_EQ(commit_out.response.rfind("COMMIT trx_id=", 0), 0u) << commit_out.response;

    // All four coordinator legs, walked once each. Counts rather than
    // durations: this rig runs on a `SystemClock` and a span here is
    // microseconds of real time, which is not a thing to assert on.
    const CoordinatorCommitStats& c = rig.dispatcher->xowner_commit_stats();
    EXPECT_EQ(c.prepare.count, 1u);
    // Walked by every cross-owner transaction that decided to commit,
    // whatever the instance's logging: **this rig's core-0 dispatcher has
    // no WAL manager at all** (`ForeignIndexRig` wires a catalog, an undo
    // log and a transaction manager and no `wal::Manager`), so the leg has
    // nothing to make durable and is very nearly instant here. That it is
    // still counted is the point - a short `decision.count` must mean an
    // abort and nothing else.
    EXPECT_EQ(c.decision.count, 1u);
    EXPECT_EQ(c.decide.count, 1u);
    EXPECT_EQ(c.whole.count, 1u);

    // The participant's prepare and its ack. The third leg - its own
    // record's durability - is a park that outlives the coordinator's
    // answer by construction, so it is pumped for rather than expected to
    // have happened by now.
    const ParticipantCommitStats& p = rig.peer->shipped_statements()->commit_phase();
    EXPECT_EQ(p.prepare_durable.count, 1u);
    EXPECT_EQ(p.decide_ack.count, 1u);
    rig.PumpUntil([&p] { return p.decide_durable.count != 0; }, 512);
    EXPECT_EQ(p.decide_durable.count, 1u)
        << "the participant's own terminal record never became durable";

    // And now `SHOW META` carries both blocks - on the core that walked
    // them, which for the participant's half is the peer.
    const std::string meta = rig.dispatcher->Dispatch("SHOW META").response;
    EXPECT_NE(meta.find("xowner_commit_n=1"), std::string::npos) << meta;
    EXPECT_NE(meta.find("xowner_prepare_n=1"), std::string::npos) << meta;
    const std::string peer_meta = rig.peer->dispatcher().Dispatch("SHOW META").response;
    EXPECT_NE(peer_meta.find("xowner_part_ack_n=1"), std::string::npos) << peer_meta;
    EXPECT_NE(peer_meta.find("xowner_part_prepare_n=1"), std::string::npos) << peer_meta;
    // The peer coordinated nothing, so its coordinator block stays absent -
    // the two halves are independent, which is the reason they are two
    // conditions in `SHOW META` and not one.
    EXPECT_EQ(peer_meta.find("xowner_commit_n="), std::string::npos) << peer_meta;
}

TEST_F(CoreRuntimeTest, AParticipantEnrolledByReadsAlonePreparesWithNoRecord) {
    // **SA-T0.** The largest measured cost the cross-owner line leaves
    // (XD6): a participant enrolled by a read pays a durable `TXN_PREPARE`
    // - an append and a device sync the coordinator is parked on - for a
    // half of the transaction that has nothing in this stream. It has no
    // rows here, no redo and an empty undo chain, so the record's replay is
    // a no-op and removing it removes nothing recovery uses.
    //
    // The two arms are in one test on purpose: what makes the read-only
    // count meaningful is that the writing enrolment does **not** take the
    // same path, and a test that only ever reads cannot show that.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "readonly_part");

    ASSERT_NE(rig.peer->shipped_statements(), nullptr);
    // A row to read, written by the owner itself so nothing about it is
    // this test's subject.
    ASSERT_EQ(rig.peer->dispatcher()
                  .Dispatch("INSERT INTO readonly_part VALUES (91)")
                  .response.rfind("INSERTED", 0),
              0u);
    ASSERT_EQ(rig.peer->shipped_statements()->read_only_prepares(), 0u);

    // ---- Arm 1: a transaction whose only foreign statement is a read ----
    Session reader;
    ASSERT_EQ(rig.dispatcher->Dispatch("BEGIN", &reader).response.rfind("BEGIN", 0), 0u);
    DispatchOutcome got;
    auto read = rig.Start("SELECT * FROM readonly_part", got, &reader);
    ASSERT_TRUE(rig.Drive(*read)) << got.response;
    EXPECT_NE(got.response.find(",91"), std::string::npos) << got.response;
    // RR1: a read inside a transaction ships and **enrols**, which is what
    // puts a read-only participant into the protocol at all.
    ASSERT_EQ(reader.participants().size(), 1u);

    DispatchOutcome committed;
    auto commit = rig.Start("COMMIT", committed, &reader);
    ASSERT_TRUE(rig.Drive(*commit)) << committed.response;
    EXPECT_EQ(committed.response.rfind("COMMIT trx_id=", 0), 0u) << committed.response;

    EXPECT_EQ(rig.peer->shipped_statements()->read_only_prepares(), 1u)
        << "a participant that wrote nothing still logged a prepare record";
    EXPECT_EQ(rig.peer->shipped_statements()->prepared(), 1u)
        << "the promise is still made - only the record is gone";
    EXPECT_EQ(rig.peer->shipped_statements()->decides_committed(), 1u);
    EXPECT_EQ(rig.peer->shipped_statements()->in_doubt(), 0u);
    EXPECT_EQ(rig.peer->shipped_statements()->enrolled(), 0u);

    // ---- Arm 2: the same shape with a write in it takes the other path --
    Session writer;
    ASSERT_EQ(rig.dispatcher->Dispatch("BEGIN", &writer).response.rfind("BEGIN", 0), 0u);
    DispatchOutcome wrote;
    auto write = rig.Start("INSERT INTO readonly_part VALUES (92)", wrote, &writer);
    ASSERT_TRUE(rig.Drive(*write)) << wrote.response;
    ASSERT_EQ(wrote.response.rfind("INSERTED", 0), 0u) << wrote.response;

    DispatchOutcome committed2;
    auto commit2 = rig.Start("COMMIT", committed2, &writer);
    ASSERT_TRUE(rig.Drive(*commit2)) << committed2.response;
    ASSERT_EQ(committed2.response.rfind("COMMIT trx_id=", 0), 0u) << committed2.response;

    EXPECT_EQ(rig.peer->shipped_statements()->read_only_prepares(), 1u)
        << "a participant that wrote took the record-free path";
    EXPECT_EQ(rig.peer->shipped_statements()->prepared(), 2u);

    // Both halves are real: the read saw the owner's row and the write
    // landed. The optimisation is about the record, never the outcome.
    const std::string rows =
        rig.peer->dispatcher().Dispatch("SELECT * FROM readonly_part").response;
    EXPECT_NE(rows.find(",91"), std::string::npos) << rows;
    EXPECT_NE(rows.find(",92"), std::string::npos) << rows;
}

TEST_F(CoreRuntimeTest, ARolledBackCrossOwnerTransactionLeavesTheOwnersRowsAlone) {
    // The other end of the same path: a client's ROLLBACK unwinds the
    // *participant's* half too, and does not merely leave it invisible.
    //
    // **The distinction is this test's whole subject**, because for one
    // commit it was the weaker of the two. The R6-8 review found the
    // rollback leg missing - `HandleCommit` forked on `has_participants()`
    // and `HandleRollback` did not - so the row was unseen only because
    // the participant's transaction had not committed, and the real
    // unwinding waited five minutes for `kShippedTxnIdleCeilingNs`. D4 says
    // a coordinator tells its participants either way; `AbortAndForget` is
    // that leg, and the assertions below are what separate "told" from
    // "not yet swept".
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "cross_owner_rb");

    Session session;
    ASSERT_EQ(rig.dispatcher->Dispatch("BEGIN", &session).response.rfind("BEGIN", 0), 0u);
    DispatchOutcome out;
    auto write = rig.Start("INSERT INTO cross_owner_rb VALUES (42)", out, &session);
    ASSERT_TRUE(rig.Drive(*write)) << out.response;
    ASSERT_TRUE(session.has_participants());

    DispatchOutcome rb_out;
    auto rollback = rig.Start("ROLLBACK", rb_out, &session);
    ASSERT_TRUE(rig.Drive(*rollback)) << rb_out.response;
    EXPECT_EQ(rb_out.response.rfind("ROLLBACK", 0), 0u) << rb_out.response;

    // **Nothing was prepared**: a rollback is not a two-phase commit, and
    // asking a participant to prepare a transaction that is about to be
    // aborted would cost a sync for nothing.
    EXPECT_EQ(rig.txn2pc->prepare_messages(), 0u);
    for (int i = 0; i < 32; ++i) rig.Pump();
    const std::string rows =
        rig.peer->dispatcher().Dispatch("SELECT * FROM cross_owner_rb").response;
    EXPECT_EQ(rows.find(",42"), std::string::npos) << rows;
    EXPECT_FALSE(session.has_participants());

    // **The owner's half is gone, not merely uncommitted**: the abort
    // reached it, it rolled back and released - no enrolment, no live
    // transaction pinning that core's read horizon, no enrolment slot held
    // for five minutes.
    EXPECT_EQ(rig.peer->shipped_statements()->enrolled(), 0u)
        << "the ROLLBACK never reached the participant";
    EXPECT_EQ(rig.peer->shipped_statements()->decides_aborted(), 1u);
    EXPECT_EQ(rig.peer->shipped_statements()->in_doubt(), 0u);
    // Sent with nobody waiting, which is what the counter separates: a
    // participant's acknowledgement finding no waiter is expected here and
    // must not read as a deadline problem.
    EXPECT_EQ(rig.txn2pc->aborts_forgotten(), 1u);
    EXPECT_EQ(rig.txn2pc->waiting(), 0u);
}

TEST_F(CoreRuntimeTest, AConnectionThatDiesMidCrossOwnerTransactionAbortsItsParticipants) {
    // The caller the abort leg is really for: `TcpServer::CloseClient`
    // rolls a dropped connection's transaction back through the
    // **synchronous** `Dispatch()`, which cannot park - so a decide leg
    // that waited for acknowledgements could not serve it at all, and a
    // dropped connection would leave a participant holding rows until its
    // idle ceiling. Driven here as that path drives it.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "cross_owner_drop");

    Session session;
    ASSERT_EQ(rig.dispatcher->Dispatch("BEGIN", &session).response.rfind("BEGIN", 0), 0u);
    DispatchOutcome out;
    auto write = rig.Start("INSERT INTO cross_owner_drop VALUES (45)", out, &session);
    ASSERT_TRUE(rig.Drive(*write)) << out.response;
    ASSERT_TRUE(session.has_participants());
    ASSERT_EQ(rig.peer->shipped_statements()->enrolled(), 1u);

    // What the close path runs, verbatim: no coroutine, no reactor of its
    // own, nothing to await.
    const std::string rb = rig.dispatcher->Dispatch("ROLLBACK", &session).response;
    EXPECT_EQ(rb.rfind("ROLLBACK", 0), 0u) << rb;
    for (int i = 0; i < 32; ++i) rig.Pump();

    EXPECT_EQ(rig.peer->shipped_statements()->enrolled(), 0u)
        << "a dropped connection left its participant holding the transaction";
    EXPECT_EQ(rig.peer->shipped_statements()->decides_aborted(), 1u);
    const std::string rows =
        rig.peer->dispatcher().Dispatch("SELECT * FROM cross_owner_drop").response;
    EXPECT_EQ(rows.find(",45"), std::string::npos) << rows;
}

TEST_F(CoreRuntimeTest, TheCoordinatorsIsolationLevelCrossesToTheParticipant) {
    // §2's obligation on D3, which R6-3 was named for and landed without:
    // the participant opens its transaction with its *own* dispatcher's
    // default, so a client that asked for REPEATABLE READ got a READ
    // COMMITTED participant and was never told. Under D3 ratified the level
    // selects a branch, so the promise and the participant have to agree.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "cross_owner_iso");

    Session session;
    ASSERT_EQ(rig.dispatcher->Dispatch("BEGIN ISOLATION LEVEL REPEATABLE READ", &session)
                  .response.rfind("BEGIN", 0),
              0u);
    DispatchOutcome out;
    auto write = rig.Start("INSERT INTO cross_owner_iso VALUES (43)", out, &session);
    ASSERT_TRUE(rig.Drive(*write)) << out.response;
    EXPECT_EQ(out.response.rfind("INSERTED", 0), 0u) << out.response;

    // The participant's transaction runs at the level the client chose, not
    // at the peer's configured default - which this rig leaves at READ
    // COMMITTED, so the two are distinguishable.
    ASSERT_NE(rig.peer->shipped_statements(), nullptr);
    ASSERT_EQ(rig.peer->shipped_statements()->enrolled(), 1u);
    EXPECT_EQ(rig.peer->dispatcher().default_isolation(), txn::IsolationLevel::kReadCommitted)
        << "the fixture no longer distinguishes the two levels";
    const auto level = rig.peer->shipped_statements()->enrolled_isolation(
        /*coordinator=*/0, session.ship_id());
    ASSERT_TRUE(level.has_value()) << "the peer holds no context for this session";
    EXPECT_EQ(*level, txn::IsolationLevel::kRepeatableRead);

    DispatchOutcome rb_out;
    auto rollback = rig.Start("ROLLBACK", rb_out, &session);
    ASSERT_TRUE(rig.Drive(*rollback)) << rb_out.response;
}

TEST_F(CoreRuntimeTest, AWriteInsideATransactionOnACoreWithNoCoordinatorKeepsItsOldRefusal) {
    // HP4, at the one site R6-8 changes: with no coordinator armed - every
    // single-core instance, every fixture - `MayEnrolShip` is false and the
    // statement falls through to the affinity check it always reached, with
    // the refusal spelled exactly as it was and counted where it was.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "no_coordinator");
    rig.dispatcher->SetTxn2pc(nullptr);

    Session session;
    ASSERT_EQ(rig.dispatcher->Dispatch("BEGIN", &session).response.rfind("BEGIN", 0), 0u);
    DispatchOutcome out;
    auto write = rig.Start("INSERT INTO no_coordinator VALUES (44)", out, &session);
    ASSERT_TRUE(rig.Drive(*write)) << out.response;

    EXPECT_EQ(out.response.rfind("ERR ", 0), 0u) << out.response;
    EXPECT_TRUE(StatusFromErrorReply(out.response).retryable()) << out.response;
    EXPECT_EQ(rig.txn2pc->prepare_messages(), 0u);
    EXPECT_FALSE(session.has_participants());
    // And it is still counted where `crosscore.md` §6 says it is - the
    // refusal counter's population is what R6-8 narrows, not what it
    // renames.
    const std::string meta = rig.dispatcher->Dispatch("SHOW META").response;
    EXPECT_NE(meta.find(" cross_core_write_refusals=1 "), std::string::npos) << meta;

    // Restored so the rig unwinds the way every other test leaves it.
    rig.dispatcher->SetTxn2pc(&*rig.txn2pc);
}

TEST_F(CoreRuntimeTest, ARolledBackCrossOwnerTransactionsWritesDoNotCommitWithTheNext) {
    // **The R6-8 review's regression test, and it caught a wrong answer.**
    // A participant's transaction context is keyed on
    // `(coordinator core, session_id)` and nothing else - the statement leg
    // carries no transaction id - and a `ROLLBACK` tells a participant
    // nothing, since a transaction that never reached prepare has no decide
    // leg. So the context outlived the transaction that opened it, and with
    // the shipping id stable for the connection's life the *next*
    // transaction's first shipped statement joined it. Its `COMMIT` then
    // committed both halves: writes the client had rolled back became
    // durable on the owner.
    //
    // The fix is one line in `Session::Finish()` - a transaction that
    // enrolled anyone drops the shipping id, so the next one addresses a
    // fresh context. What it does *not* fix, and what this test therefore
    // does not assert, is the abandoned context itself: it is still there,
    // holding its rows uncommitted until `kShippedTxnIdleCeilingNs`.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "cross_owner_reuse");

    Session session;
    ASSERT_EQ(rig.dispatcher->Dispatch("BEGIN", &session).response.rfind("BEGIN", 0), 0u);
    DispatchOutcome first_out;
    auto first = rig.Start("INSERT INTO cross_owner_reuse VALUES (91)", first_out, &session);
    ASSERT_TRUE(rig.Drive(*first)) << first_out.response;
    ASSERT_EQ(first_out.response.rfind("INSERTED", 0), 0u) << first_out.response;
    const std::uint64_t rolled_back_ship_id = session.ship_id();

    DispatchOutcome rb_out;
    auto rollback = rig.Start("ROLLBACK", rb_out, &session);
    ASSERT_TRUE(rig.Drive(*rollback)) << rb_out.response;
    for (int i = 0; i < 32; ++i) rig.Pump();

    // The second transaction, on the same connection.
    ASSERT_EQ(rig.dispatcher->Dispatch("BEGIN", &session).response.rfind("BEGIN", 0), 0u);
    DispatchOutcome second_out;
    auto second = rig.Start("INSERT INTO cross_owner_reuse VALUES (92)", second_out, &session);
    ASSERT_TRUE(rig.Drive(*second)) << second_out.response;
    ASSERT_EQ(second_out.response.rfind("INSERTED", 0), 0u) << second_out.response;
    EXPECT_NE(session.ship_id(), rolled_back_ship_id)
        << "the second transaction addressed the first's participant context";

    DispatchOutcome commit_out;
    auto commit = rig.Start("COMMIT", commit_out, &session);
    ASSERT_TRUE(rig.Drive(*commit)) << commit_out.response;
    EXPECT_EQ(commit_out.response.rfind("COMMIT trx_id=", 0), 0u) << commit_out.response;
    for (int i = 0; i < 32; ++i) rig.Pump();

    const std::string rows =
        rig.peer->dispatcher().Dispatch("SELECT * FROM cross_owner_reuse").response;
    EXPECT_EQ(rows.find(",91"), std::string::npos)
        << "a rolled-back cross-owner write committed with the next transaction: " << rows;
    EXPECT_NE(rows.find(",92"), std::string::npos) << rows;
}

TEST_F(CoreRuntimeTest, AShippedStatementTheOwnerRefusesPoisonsTheTransactionThatSentIt) {
    // **The other half of the same review finding**: failure atomicity is
    // per transaction (`txn.md` §6), so a statement that fails inside
    // `BEGIN` puts the session in `failed-txn` and the client must
    // `ROLLBACK`. The local path takes that poison in `EndWrite`, which an
    // enrolled ship skips - it has nothing to end - so before the fix an
    // owner's refusal left the transaction committable, and the rows an
    // owner-side statement wrote before its own failure would have
    // committed under a client that had been told `ERR`.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "cross_owner_refuse");

    Session session;
    ASSERT_EQ(rig.dispatcher->Dispatch("BEGIN", &session).response.rfind("BEGIN", 0), 0u);
    DispatchOutcome out;
    // A caller-supplied pk, which a peer refuses per row
    // (`workplan-peer-writer.md` §7a): the coordinator parses it, finds the
    // relation foreign and ships it, and the *owner* is what refuses - so
    // the refusal arrives after the statement left, which is the shape
    // this test needs and the one only an owner-side failure has.
    auto write = rig.Start("INSERT INTO cross_owner_refuse VALUES (1, 99)", out, &session);
    ASSERT_TRUE(rig.Drive(*write)) << out.response;
    ASSERT_NE(out.response.find("a caller-supplied primary key cannot be written on core 1"),
              std::string::npos)
        << "the fixture no longer refuses this statement on the owner: " << out.response;
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 1u) << "it did not reach the owner";

    EXPECT_TRUE(session.failed()) << "an owner's refusal left the transaction committable";
    const std::string refused = rig.dispatcher->Dispatch("COMMIT", &session).response;
    EXPECT_NE(refused.find("current transaction is aborted"), std::string::npos) << refused;

    DispatchOutcome rb_out;
    auto rollback = rig.Start("ROLLBACK", rb_out, &session);
    ASSERT_TRUE(rig.Drive(*rollback)) << rb_out.response;
}

TEST_F(CoreRuntimeTest, AReadOfAPeerOwnedRelationShipsAndAnswersWithTheOwnersRows) {
    // D1's read half. This rig installs no pipeline (`SetRemoteReads` is
    // never called), which is the peer's own situation as the pretasks
    // measured it: a plain statement cannot reach P4 from dispatch, so the
    // read either ships or is refused.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_read");

    DispatchOutcome out;
    auto statement = rig.Start("SELECT * FROM shipped_read", out);
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    EXPECT_NE(out.response.rfind("ERR", 0), 0u) << out.response;
    // The three rows `OpenForeignIndexRig` wrote on the owner, which core 0
    // has never seen.
    EXPECT_NE(out.response.find(",10"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find(",20"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find(",30"), std::string::npos) << out.response;
    EXPECT_EQ(out.response,
              rig.peer->dispatcher().Dispatch("SELECT * FROM shipped_read").response)
        << "a shipped read must answer exactly what the owner answers";
}

// ---- The step pipeline at the ring slot it actually sends through -------

TEST_F(CoreRuntimeTest, AStepBatchWiderThanTheRingSlotStillDeliversEveryRow) {
    // P4e's equivalence rig proves the pipeline's reply is the local reply
    // byte for byte - but it hands each message to the far side by calling
    // its handler directly, so no batch it builds is ever measured against
    // a ring slot. Production sends the same batch through
    // `sched::kCoreRingPayloadBytes` (1,024) while the server seals at
    // `kStepBatchTargetBytes` (32 KiB) or, unconditionally, at the end of
    // the walk. This runs the P4c single-step shape over a **real** ring at
    // both production sizes and compares the row count the session sees
    // against the owner's own answer, at growing widths.
    //
    // A two-column int64 row is 24 encoded bytes (a 4-byte length plus 8
    // per column), the STEP_BATCH header is 24 and the row-batch header 2
    // (wire/row_codec.cpp), so 24 + 2 + 24n crosses 1,024 at n = 42 - the
    // measured first loss exactly.
    std::optional<SessionStepClient> client;  // declared first: outlives rig.dispatcher
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "wide_batch");

    // The owner's rows. `OpenForeignIndexRig` leaves three (10, 20, 30) and
    // a 16-id lease, so both are extended here.
    auto more = rig.catalog2->AllocateRowIdRange(rig.oid, 512);
    ASSERT_TRUE(more.ok()) << more.status().message();
    rig.peer->row_id_leases().Grant(rig.oid, more.value(), 512);
    constexpr int kRows = 120;
    for (int base = 0; base < kRows; base += 10) {
        std::string ins = "INSERT INTO wide_batch VALUES ";
        for (int i = 0; i < 10; ++i) {
            if (i > 0) ins += ", ";
            ins += "(" + std::to_string(1000 + base + i + 1) + ")";
        }
        const std::string reply = rig.peer->dispatcher().Dispatch(ins).response;
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << reply;
    }

    // Core 0's session-side pipeline client, wired exactly as
    // `Expeditor::Serve` wires it: the send is a `SendRetryTask` on the
    // real transport, and the three consumer kinds are registered on core
    // 0's reactor.
    client.emplace(/*core_id=*/0,
                   [&rig](std::uint32_t dst, sched::RingMessageKind kind,
                          std::vector<std::byte> payload) {
                       sched::MessageHeader out{};
                       out.src_core = 0;
                       out.dst_core = dst;
                       out.session_core = 0;
                       out.kind = static_cast<std::uint16_t>(kind);
                       out.sched_group =
                           static_cast<std::uint16_t>(sched::SchedulingGroup::kForeground);
                       rig.core0->Submit(
                           sched::MakeSendRetryTask(rig.ring(), out, payload));
                       return Status::OK();
                   });
    ASSERT_TRUE(rig.core0
                    ->RegisterMessageHandler(sched::RingMessageKind::kStepBatch,
                                             [&client](const sched::MessageHeader&,
                                                       std::span<const std::byte> payload) {
                                                 client->OnStepBatch(payload);
                                             })
                    .ok());
    ASSERT_TRUE(rig.core0
                    ->RegisterMessageHandler(sched::RingMessageKind::kStepEof,
                                             [&client](const sched::MessageHeader&,
                                                       std::span<const std::byte> payload) {
                                                 client->OnStepEof(payload);
                                             })
                    .ok());
    ASSERT_TRUE(rig.core0
                    ->RegisterMessageHandler(sched::RingMessageKind::kStepError,
                                             [&client](const sched::MessageHeader&,
                                                       std::span<const std::byte> payload) {
                                                 client->OnStepError(payload);
                                             })
                    .ok());
    rig.dispatcher->SetRemoteReads(&*client);

    // The text protocol separates rows with a literal backslash-n (the P4e
    // expectations above are spelled that way), so the row count is the
    // number of separators: one heading line, then one per row.
    auto rows_in = [](const std::string& reply) {
        std::size_t rows = 0;
        for (std::size_t at = reply.find("\\n"); at != std::string::npos;
             at = reply.find("\\n", at + 2)) {
            ++rows;
        }
        return rows;
    };

    // `SELECT *` with a residual stays the star shape the single-step
    // pipeline takes (command_dispatcher.cpp's eligibility test), so the
    // predicate is the width dial: `v <= n` is exactly n rows.
    for (int n : {1, 8, 20, 30, 38, 40, 41, 42, 43, 44, 48, 64, 100, 120}) {
        const std::string sql =
            "SELECT * FROM wide_batch WHERE v > 1000 AND v <= " + std::to_string(1000 + n);
        const std::string local = rig.peer->dispatcher().Dispatch(sql).response;
        ASSERT_NE(local.rfind("ERR", 0), 0u) << local;
        ASSERT_EQ(rows_in(local), static_cast<std::size_t>(n)) << local;

        DispatchOutcome out;
        auto statement = rig.Start(sql.c_str(), out);
        ASSERT_TRUE(rig.Drive(*statement, 1024)) << "n=" << n << ": " << out.response;
        // Nothing shipped as text: this shape takes the pipeline, which
        // sits above the shipping fork. Without this the test could pass
        // by measuring the other path.
        ASSERT_EQ(rig.peer->shipped_statements()->executed(), 0u) << "n=" << n;
        EXPECT_EQ(rows_in(out.response), static_cast<std::size_t>(n))
            << "n=" << n << " reply: " << out.response;
        EXPECT_EQ(out.response, local) << "n=" << n;
    }
}

TEST_F(CoreRuntimeTest, UpdateAndDeleteShipToTheOwnerToo) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_dml");

    DispatchOutcome updated;
    auto update = rig.Start("UPDATE shipped_dml SET v = 99 WHERE v = 20", updated);
    ASSERT_TRUE(rig.Drive(*update)) << updated.response;
    EXPECT_NE(updated.response.rfind("ERR", 0), 0u) << updated.response;

    DispatchOutcome deleted;
    auto del = rig.Start("DELETE FROM shipped_dml WHERE v = 10", deleted);
    ASSERT_TRUE(rig.Drive(*del)) << deleted.response;
    EXPECT_NE(deleted.response.rfind("ERR", 0), 0u) << deleted.response;

    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 2u);
    const std::string rows =
        rig.peer->dispatcher().Dispatch("SELECT * FROM shipped_dml").response;
    EXPECT_NE(rows.find(",99"), std::string::npos) << rows;
    EXPECT_EQ(rows.find(",10"), std::string::npos) << rows;
    EXPECT_EQ(rows.find(",20"), std::string::npos) << rows;
}

TEST_F(CoreRuntimeTest, TheOwnersRefusalReachesTheClientAsTheOwnerSpelledIt) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_refusal");

    DispatchOutcome out;
    auto statement = rig.Start("INSERT INTO shipped_refusal VALUES ('not an int')", out);
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    ASSERT_EQ(out.response.rfind("ERR ", 0), 0u) << out.response;
    // Byte for byte the line the owner would have written itself: the code
    // crossed and `ErrorReply` rendered it again on this side.
    EXPECT_EQ(out.response,
              rig.peer->dispatcher()
                  .Dispatch("INSERT INTO shipped_refusal VALUES ('not an int')")
                  .response);
    // A refusal is not an execution, so nothing was written on the owner.
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 1u);
}

TEST_F(CoreRuntimeTest, AStatementInsideATransactionShipsAndEnrolsSinceR68) {
    // **This is the one refusal class R6-8 converts, asserted at the site
    // that used to assert its opposite.** Until 2026-08-28 this test read
    // `AStatementInsideATransactionIsNotShippedAndKeepsItsRefusal` and
    // expected `TXN_CONFLICT`, on the ground SS2 gave - *"D1: nothing
    // crosses transaction state, so an explicit transaction keeps the CC3
    // refusal it has always had"*. D4 is what supersedes that: the
    // transaction state now crosses as one wire bit, the owner holds a
    // transaction open for it (R6-2), and the coordinator's COMMIT runs
    // two phases over it.
    //
    // Kept as a renamed test rather than deleted, because HP4's falsifier
    // is "any existing refusal test needs its expected text or status
    // edited" - and the honest report is that exactly one did, this one,
    // for the class the row exists to convert. Every other refusal test in
    // this file passes unedited, which is what HP4 actually claims.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_txn");

    Session session;
    DispatchOutcome begun;
    auto begin = rig.Start("BEGIN", begun, &session);
    ASSERT_TRUE(rig.Drive(*begin)) << begun.response;

    DispatchOutcome out;
    auto statement = rig.Start("INSERT INTO shipped_txn VALUES (40)", out, &session);
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    EXPECT_EQ(out.response.rfind("INSERTED", 0), 0u) << out.response;
    EXPECT_EQ(out.response.find("TXN_CONFLICT"), std::string::npos) << out.response;
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 1u);
    EXPECT_TRUE(session.has_participants());
    // The waiter closed behind the statement, as it does for an autocommit
    // ship: what R6-8 changes is which shapes reach the wire, not how one
    // is carried.
    EXPECT_EQ(rig.ship->waiting(), 0u);

    DispatchOutcome rb;
    auto rollback = rig.Start("ROLLBACK", rb, &session);
    ASSERT_TRUE(rig.Drive(*rollback)) << rb.response;
}

// **The pipeline, wired exactly as `Expeditor::Serve` wires it**
// (`src/server/expeditor.cpp`) - the send is a `SendRetryTask` on the real
// transport and the three consumer kinds are registered on core 0's
// reactor.
//
// It exists because `ForeignIndexRig` deliberately installs no pipeline,
// and for a **read** that omission is not neutral: the two remote-read fast
// paths in `HandleSelect` sit *above* the shipping fork and take
// `SELECT * FROM <peer relation>` whole, so a rig without them measures a
// route production does not take. Every RR1 test below wires it, which is
// what makes "the read ships" a statement about the server rather than
// about the fixture.
// `with_description = false` is XG3's fault injection: the arrival core
// never receives a `kShippedRowDesc`, which is what an owner dying
// between its rows and its description looks like from here.
void WireRemoteReads(ForeignIndexRig& rig, std::optional<SessionStepClient>& client,
                     bool with_description = true) {
    client.emplace(/*core_id=*/0,
                   [&rig](std::uint32_t dst, sched::RingMessageKind kind,
                          std::vector<std::byte> payload) {
                       sched::MessageHeader out{};
                       out.src_core = 0;
                       out.dst_core = dst;
                       out.session_core = 0;
                       out.kind = static_cast<std::uint16_t>(kind);
                       out.sched_group =
                           static_cast<std::uint16_t>(sched::SchedulingGroup::kForeground);
                       rig.core0->Submit(sched::MakeSendRetryTask(rig.ring(), out, payload));
                       return Status::OK();
                   });
    ASSERT_TRUE(rig.core0
                    ->RegisterMessageHandler(sched::RingMessageKind::kStepBatch,
                                             [&client](const sched::MessageHeader&,
                                                       std::span<const std::byte> payload) {
                                                 client->OnStepBatch(payload);
                                             })
                    .ok());
    ASSERT_TRUE(rig.core0
                    ->RegisterMessageHandler(sched::RingMessageKind::kStepEof,
                                             [&client](const sched::MessageHeader&,
                                                       std::span<const std::byte> payload) {
                                                 client->OnStepEof(payload);
                                             })
                    .ok());
    ASSERT_TRUE(rig.core0
                    ->RegisterMessageHandler(sched::RingMessageKind::kStepError,
                                             [&client](const sched::MessageHeader&,
                                                       std::span<const std::byte> payload) {
                                                 client->OnStepError(payload);
                                             })
                    .ok());
    // XG1: a shipped read's answer edge carries its description on its own
    // kind, and this rig wires the endpoints by hand rather than through
    // `WireStepEndpoints`. Missing it would leave a typed read's rows
    // arriving with nothing to type them - which is exactly what
    // `ForwardAnswerEdge` refuses, so the omission would have shown up as a
    // refusal rather than a wrong answer.
    if (with_description) {
        ASSERT_TRUE(rig.core0
                        ->RegisterMessageHandler(sched::RingMessageKind::kShippedRowDesc,
                                                 [&client](const sched::MessageHeader&,
                                                           std::span<const std::byte> payload) {
                                                     client->OnShippedRowDesc(payload);
                                                 })
                        .ok());
    }
    rig.dispatcher->SetRemoteReads(&*client);
}

TEST_F(CoreRuntimeTest, ATypedClientsShippedReadComesBackAsRowsOnTheAnswerEdge) {
    // **XG1 end to end** (`docs/spec/crosscore.md` §4a). Until this row a
    // shipped read was refused outright to any session carrying a result
    // sink, which under KWP/1 is every session - so a typed client could
    // not read foreign data inside a transaction at all.
    //
    // **The sink is what makes this test the feature's test.** Without one
    // the statement takes the text arm and proves nothing about the edge;
    // every other shipping test in this file is on that arm, and all of
    // them stayed green while this path did not exist.
    std::optional<SessionStepClient> reads;  // declared first: outlives rig.dispatcher
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "typed_ship");
    WireRemoteReads(rig, reads);

    ASSERT_EQ(rig.peer->dispatcher()
                  .Dispatch("INSERT INTO typed_ship VALUES (61)")
                  .response.rfind("INSERTED", 0),
              0u);
    ASSERT_EQ(rig.peer->dispatcher()
                  .Dispatch("INSERT INTO typed_ship VALUES (62)")
                  .response.rfind("INSERTED", 0),
              0u);

    // A typed client: a session carrying the wire sink, which is what
    // `ShipStatement` reads to decide the answer's form.
    WireResultSink sink;
    Session session;
    session.set_result_sink(&sink);

    DispatchOutcome begun;
    auto begin = rig.Start("BEGIN", begun, &session);
    ASSERT_TRUE(rig.Drive(*begin)) << begun.response;

    DispatchOutcome out;
    auto read = rig.Start("SELECT * FROM typed_ship", out, &session);
    ASSERT_TRUE(rig.Drive(*read)) << out.response;
    EXPECT_NE(out.response.rfind("ERR", 0), 0u)
        << "a typed client's shipped read was still refused: " << out.response;

    // The description arrived and typed the rows - it is the owner's,
    // because only the owner compiled the statement.
    EXPECT_TRUE(sink.described()) << "no description reached the sink";
    EXPECT_FALSE(sink.fields().empty());

    // **As many rows as the owner's own local read answers**, which is the
    // property the whole cross-core suite is built on: a statement answered
    // over an edge and the same statement answered locally are one answer.
    // Counted from the owner's rendered reply - one `"\n"` section per row -
    // rather than from a literal, because the rig seeds rows of its own and
    // a hard-coded count would be asserting the fixture.
    const std::string local =
        rig.peer->dispatcher().Dispatch("SELECT * FROM typed_ship").response;
    std::uint64_t local_rows = 0;
    for (std::size_t at = local.find("\\n"); at != std::string::npos;
         at = local.find("\\n", at + 2)) {
        ++local_rows;
    }
    ASSERT_GE(local_rows, 2u) << "the fixture answered nothing to compare against: " << local;
    EXPECT_EQ(sink.row_count(), local_rows)
        << "the edge delivered a different number of rows than the owner's own read";

    // And the receiver is gone: an edge left registered holds its batches
    // for the session's life.
    EXPECT_EQ(reads->open_reads(), 0u);

    DispatchOutcome rb;
    auto rollback = rig.Start("ROLLBACK", rb, &session);
    ASSERT_TRUE(rig.Drive(*rollback)) << rb.response;
}

TEST_F(CoreRuntimeTest, RowsWithNoDescriptionAreRefusedRatherThanDecoded) {
    // **XG3's description cell, end to end.** An owner that died between
    // its rows and its description leaves the arrival core holding bytes
    // it cannot type. Decoding them against a guessed shape would produce
    // plausible field widths - invariant 13's forbidden reading one layer
    // up - so the forward refuses, and it refuses **before** anything
    // reaches the sink.
    std::optional<SessionStepClient> reads;
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "no_desc");
    WireRemoteReads(rig, reads, /*with_description=*/false);

    ASSERT_EQ(rig.peer->dispatcher()
                  .Dispatch("INSERT INTO no_desc VALUES (81)")
                  .response.rfind("INSERTED", 0),
              0u);

    WireResultSink sink;
    Session session;
    session.set_result_sink(&sink);

    // **Inside a transaction, which is what forces the shipping route.** An
    // autocommit star read of an unsplit foreign relation is served by the
    // remote-step edge instead and would describe itself from the
    // relation's schema - passing this test while proving nothing about the
    // path under it. That is exactly the trap the first draft of this cell
    // fell into.
    DispatchOutcome begun;
    auto begin = rig.Start("BEGIN", begun, &session);
    ASSERT_TRUE(rig.Drive(*begin)) << begun.response;

    DispatchOutcome out;
    auto read = rig.Start("SELECT * FROM no_desc", out, &session);
    ASSERT_TRUE(rig.Drive(*read)) << out.response;
    EXPECT_EQ(out.response.rfind("ERR", 0), 0u)
        << "rows with no description were delivered anyway: " << out.response;
    EXPECT_EQ(sink.row_count(), 0u) << "a row reached the sink untyped";
    EXPECT_FALSE(sink.described());
    EXPECT_EQ(reads->open_reads(), 0u) << "the receiver outlived its statement";

    DispatchOutcome rb;
    auto rollback = rig.Start("ROLLBACK", rb, &session);
    ASSERT_TRUE(rig.Drive(*rollback)) << rb.response;
}

TEST_F(CoreRuntimeTest, ARefusedReadDescribesNothingToATypedClient) {
    // **A failed read must reach a typed client as a failure and nothing
    // else** - not as an empty result set, which is a different answer.
    //
    // **What this covers and what it does not**, stated because the
    // difference matters: this refusal is taken on *this* core, at compile,
    // before anything ships - so it proves the sink is left untouched and
    // no receiver is left behind, and it does **not** exercise an owner
    // failing part way through a result it has already begun sending. That
    // case needs a process kill at `shipped.answer_batch_sent:1`, which is
    // why that crash point exists; it is not reachable from this rig.
    std::optional<SessionStepClient> reads;
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "refused_read");
    WireRemoteReads(rig, reads);

    WireResultSink sink;
    Session session;
    session.set_result_sink(&sink);

    DispatchOutcome out;
    auto read = rig.Start("SELECT no_such_column FROM refused_read", out, &session);
    ASSERT_TRUE(rig.Drive(*read)) << out.response;
    EXPECT_EQ(out.response.rfind("ERR", 0), 0u)
        << "the owner's refusal did not reach the client: " << out.response;
    EXPECT_EQ(sink.row_count(), 0u);
    EXPECT_FALSE(sink.described())
        << "a refused statement described a result set it does not have";
    EXPECT_EQ(reads->open_reads(), 0u)
        << "the answer edge is closed on every exit, refusals included";
}

TEST_F(CoreRuntimeTest, AShippedReadToATextClientKeepsTheRenderedLine) {
    // The other arm, asserted beside it: a session with **no** sink ships
    // `form = 0`, the owner installs no sink, and the answer is the
    // rendered reply line byte for byte. That arm is what the newline
    // byte-identity suite rests on, and a change that quietly typed it
    // would pass every test in this file except this one.
    std::optional<SessionStepClient> reads;
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "text_ship");
    WireRemoteReads(rig, reads);

    ASSERT_EQ(rig.peer->dispatcher()
                  .Dispatch("INSERT INTO text_ship VALUES (71)")
                  .response.rfind("INSERTED", 0),
              0u);

    Session session;  // no sink
    DispatchOutcome begun;
    auto begin = rig.Start("BEGIN", begun, &session);
    ASSERT_TRUE(rig.Drive(*begin)) << begun.response;

    DispatchOutcome out;
    auto read = rig.Start("SELECT * FROM text_ship", out, &session);
    ASSERT_TRUE(rig.Drive(*read)) << out.response;
    EXPECT_NE(out.response.find(",71"), std::string::npos)
        << "the text arm stopped answering in a rendered line: " << out.response;
    EXPECT_EQ(reads->open_reads(), 0u) << "the text arm must register no edge at all";

    DispatchOutcome rb;
    auto rollback = rig.Start("ROLLBACK", rb, &session);
    ASSERT_TRUE(rig.Drive(*rollback)) << rb.response;
}

TEST_F(CoreRuntimeTest, AReadInsideATransactionShipsAndEnrolsSinceRR1) {
    // **RR1: the read half of enrolment**, and the mirror of
    // `AStatementInsideATransactionShipsAndEnrolsSinceR68` above. Until
    // 2026-08-28 the read site tested `MayShip` alone, which refuses inside
    // an explicit transaction - so this statement fell through to
    // `CheckReadAffinity` and was refused, while the identical read outside
    // a transaction shipped. RP8's B5 is what found it: a realistic
    // transaction reads before it writes, so the refusal made the whole
    // two-phase path unreachable from any workload in this tree.
    std::optional<SessionStepClient> reads;  // declared first: outlives rig.dispatcher
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_txn_read");
    WireRemoteReads(rig, reads);

    Session session;
    DispatchOutcome begun;
    auto begin = rig.Start("BEGIN", begun, &session);
    ASSERT_TRUE(rig.Drive(*begin)) << begun.response;

    DispatchOutcome out;
    auto read = rig.Start("SELECT * FROM shipped_txn_read", out, &session);
    ASSERT_TRUE(rig.Drive(*read)) << out.response;
    EXPECT_NE(out.response.rfind("ERR", 0), 0u) << out.response;
    EXPECT_NE(out.response.find(",10"), std::string::npos) << out.response;
    // **And it shipped rather than pipelined**, which is the whole claim:
    // this rig has the pipeline wired, and `SELECT *` on a single foreign
    // relation is exactly the shape the single-step pipeline takes. Inside
    // a session that can enrol it is diverted, because the pipeline answers
    // from a view outside this transaction.
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 1u) << "it did not reach the owner";

    // **A read enrols**, which is the part that costs something: the owner
    // holds a transaction open for it and the coordinator's COMMIT will run
    // two phases over a participant that only read. That is the price of
    // the read seeing this transaction's own uncommitted writes on that
    // core (the test below), and it is stated rather than optimised - a
    // read-only participant reply is a protocol change and out of this
    // order's scope.
    EXPECT_TRUE(session.has_participants());
    EXPECT_EQ(rig.peer->shipped_statements()->enrolled(), 1u);

    DispatchOutcome rb;
    auto rollback = rig.Start("ROLLBACK", rb, &session);
    ASSERT_TRUE(rig.Drive(*rollback)) << rb.response;
}

TEST_F(CoreRuntimeTest, ACrossOwnerTransactionReadsBackItsOwnUncommittedWriteOnThePeer) {
    // **Why the read has to ship rather than be answered from a snapshot**
    // (RR1). A transaction that wrote a row on a peer and then reads that
    // relation must see its own uncommitted write; only the peer's own
    // transaction can show it, so the read joins that transaction. Nothing
    // else does: the row is not on this core, and no view this core holds
    // can make a peer's uncommitted row visible.
    std::optional<SessionStepClient> reads;  // declared first: outlives rig.dispatcher
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "read_own_write");
    WireRemoteReads(rig, reads);

    Session session;
    DispatchOutcome begun;
    auto begin = rig.Start("BEGIN", begun, &session);
    ASSERT_TRUE(rig.Drive(*begin)) << begun.response;

    DispatchOutcome wrote;
    auto write = rig.Start("INSERT INTO read_own_write VALUES (77)", wrote, &session);
    ASSERT_TRUE(rig.Drive(*write)) << wrote.response;
    ASSERT_EQ(wrote.response.rfind("INSERTED", 0), 0u) << wrote.response;

    DispatchOutcome out;
    auto read = rig.Start("SELECT * FROM read_own_write", out, &session);
    ASSERT_TRUE(rig.Drive(*read)) << out.response;
    EXPECT_NE(out.response.find(",77"), std::string::npos)
        << "a cross-owner transaction did not see its own write on the peer: " << out.response;

    // And nobody else does, because it is not committed: the owner's own
    // autocommit read runs under a fresh view of the owner's history.
    const std::string outside =
        rig.peer->dispatcher().Dispatch("SELECT * FROM read_own_write").response;
    EXPECT_EQ(outside.find(",77"), std::string::npos)
        << "an uncommitted cross-owner write was visible outside its transaction: " << outside;

    // One participant and one context for both statements - the second
    // joined the first's transaction rather than opening a second one.
    EXPECT_EQ(session.participants().size(), 1u);
    EXPECT_EQ(rig.peer->shipped_statements()->enrolments(), 1u);

    DispatchOutcome rb;
    auto rollback = rig.Start("ROLLBACK", rb, &session);
    ASSERT_TRUE(rig.Drive(*rollback)) << rb.response;
}

TEST_F(CoreRuntimeTest, AStatementThatCanOnlyJoinIsRefusedWhenTheParticipantsContextIsGone) {
    // **RR0's correctness half, and the answer to CR2.** A participant's
    // context is keyed on `(coordinator core, session_id)` and nothing
    // else, and two things end one while its coordinator's transaction is
    // still open: the idle ceiling (`kShippedTxnIdleCeilingNs`) and this
    // core stopping. Before the `join` bit the next statement of that
    // transaction found no context and opened a **fresh** one; prepare and
    // commit then made the second half durable, the first half was gone,
    // and the client was told the whole transaction committed.
    //
    // `RollbackAllEnrolled` is the ceiling's effect without the ceiling's
    // clock - this rig runs on a `SystemClock`, and what is under test is
    // what happens *after* a context disappears, not what makes it
    // disappear.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "join_gone");

    Session session;
    DispatchOutcome begun;
    auto begin = rig.Start("BEGIN", begun, &session);
    ASSERT_TRUE(rig.Drive(*begin)) << begun.response;

    DispatchOutcome first;
    auto write = rig.Start("INSERT INTO join_gone VALUES (81)", first, &session);
    ASSERT_TRUE(rig.Drive(*write)) << first.response;
    ASSERT_EQ(first.response.rfind("INSERTED", 0), 0u) << first.response;
    ASSERT_EQ(rig.peer->shipped_statements()->enrolments(), 1u);

    rig.peer->shipped_statements()->RollbackAllEnrolled();
    ASSERT_EQ(rig.peer->shipped_statements()->enrolled(), 0u);

    DispatchOutcome second;
    auto again = rig.Start("INSERT INTO join_gone VALUES (82)", second, &session);
    ASSERT_TRUE(rig.Drive(*again)) << second.response;
    EXPECT_EQ(second.response.rfind("ERR", 0), 0u)
        << "a statement opened a second transaction for one cross-owner transaction: "
        << second.response;
    EXPECT_NE(second.response.find("TXN_CONFLICT"), std::string::npos) << second.response;
    EXPECT_EQ(rig.peer->shipped_statements()->join_refusals(), 1u);
    // The refusal is the whole point: no second context was opened.
    EXPECT_EQ(rig.peer->shipped_statements()->enrolments(), 1u);
    EXPECT_TRUE(session.failed())
        << "the transaction stayed committable after half of it was rolled back";

    DispatchOutcome rb;
    auto rollback = rig.Start("ROLLBACK", rb, &session);
    ASSERT_TRUE(rig.Drive(*rollback)) << rb.response;
}

TEST_F(CoreRuntimeTest, ARepeatableReadCrossOwnerTransactionCarriesOneWatermarkPerParticipant) {
    // **RR0 / D3, both halves of the ratified `[OPEN]` in one test.**
    // REPEATABLE READ carries a watermark per participant - the
    // `up_to_trx_id` that participant's own transaction pinned, in that
    // core's own id space - and it does not move across the transaction's
    // statements, which is the consistent-per-core snapshot D3 promises.
    // READ COMMITTED carries none at all, which is the `[OPEN]` ratified
    // "yes, RC skips the watermark entirely" and is why the default level
    // pays nothing for any of this.
    std::optional<SessionStepClient> reads;  // declared first: outlives rig.dispatcher
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "watermark_rr");
    WireRemoteReads(rig, reads);

    Session rr;
    DispatchOutcome begun;
    auto begin = rig.Start("BEGIN ISOLATION LEVEL REPEATABLE READ", begun, &rr);
    ASSERT_TRUE(rig.Drive(*begin)) << begun.response;

    DispatchOutcome first;
    auto read = rig.Start("SELECT * FROM watermark_rr", first, &rr);
    ASSERT_TRUE(rig.Drive(*read)) << first.response;
    ASSERT_NE(first.response.rfind("ERR", 0), 0u) << first.response;
    const std::uint64_t watermark = rr.ParticipantWatermark(1);
    EXPECT_NE(watermark, 0u) << "an RR participant reported no watermark";

    // **What "consistent per core" means, made concrete rather than
    // asserted**: the participant commits a row of its own between the two
    // reads, and the second read must not show it. Without the pinned view
    // the two reads differ by exactly this row, which is what makes the
    // equality below a statement about isolation and not about a relation
    // nobody touched.
    ASSERT_EQ(rig.peer->dispatcher()
                  .Dispatch("INSERT INTO watermark_rr VALUES (444)")
                  .response.rfind("INSERTED", 0),
              0u);

    DispatchOutcome second;
    auto again = rig.Start("SELECT * FROM watermark_rr", second, &rr);
    ASSERT_TRUE(rig.Drive(*again)) << second.response;
    EXPECT_NE(second.response.rfind("ERR", 0), 0u) << second.response;
    EXPECT_EQ(rr.ParticipantWatermark(1), watermark)
        << "the participant's snapshot moved under a REPEATABLE READ transaction";
    EXPECT_EQ(second.response, first.response)
        << "two reads of one relation in one RR transaction disagreed";
    EXPECT_EQ(second.response.find(",444"), std::string::npos)
        << "a REPEATABLE READ transaction saw a commit that happened after it began: "
        << second.response;

    DispatchOutcome rb;
    auto rollback = rig.Start("ROLLBACK", rb, &rr);
    ASSERT_TRUE(rig.Drive(*rollback)) << rb.response;

    Session rc;
    DispatchOutcome rc_begun;
    auto rc_begin = rig.Start("BEGIN", rc_begun, &rc);
    ASSERT_TRUE(rig.Drive(*rc_begin)) << rc_begun.response;
    DispatchOutcome rc_out;
    auto rc_read = rig.Start("SELECT * FROM watermark_rr", rc_out, &rc);
    ASSERT_TRUE(rig.Drive(*rc_read)) << rc_out.response;
    ASSERT_NE(rc_out.response.rfind("ERR", 0), 0u) << rc_out.response;
    EXPECT_EQ(rc.ParticipantWatermark(1), 0u)
        << "READ COMMITTED carried a watermark it is ratified to skip";
    // And it is not merely unwatermarked - it reads the latest committed
    // state, which is the level's own contract and the reason a watermark
    // for it would name a view already gone.
    EXPECT_NE(rc_out.response.find(",444"), std::string::npos) << rc_out.response;

    DispatchOutcome rc_rb;
    auto rc_rollback = rig.Start("ROLLBACK", rc_rb, &rc);
    ASSERT_TRUE(rig.Drive(*rc_rollback)) << rc_rb.response;
}

TEST_F(CoreRuntimeTest, AnOverLongShippedReadIsRefusedAndLeavesItsTransactionOpen) {
    // **The price RR1's gate has, pinned rather than described.** A read
    // inside a cross-owner transaction ships, and a shipped answer must fit
    // one ring slot - so past `kShippedStatementReplyTextMax` the client is
    // refused. Two things about that refusal matter, and this test is for
    // them rather than for the cap:
    //
    // 1. it is a **refusal**, never a truncated answer wearing an OK;
    // 2. it **does not end the transaction**. A read has no effect, so
    //    failure atomicity has nothing to protect here, and a local
    //    `SELECT` that fails leaves its transaction open - the two have to
    //    agree, and until RR1 they agreed only because reads never shipped.
    std::optional<SessionStepClient> reads;  // declared first: outlives rig.dispatcher
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "big_read");
    WireRemoteReads(rig, reads);

    // Enough rows that the reply cannot fit. `OpenForeignIndexRig` leaves
    // three and a 16-id lease, so both are extended.
    auto more = rig.catalog2->AllocateRowIdRange(rig.oid, 512);
    ASSERT_TRUE(more.ok()) << more.status().message();
    rig.peer->row_id_leases().Grant(rig.oid, more.value(), 512);
    // Ten statements, not three hundred: the rig grants the peer sixteen
    // transaction ids, and each `INSERT` is one.
    for (int base = 0; base < 300; base += 30) {
        std::string ins = "INSERT INTO big_read VALUES ";
        for (int i = 0; i < 30; ++i) {
            if (i > 0) ins += ", ";
            ins += "(" + std::to_string(100000 + base + i) + ")";
        }
        const std::string r = rig.peer->dispatcher().Dispatch(ins).response;
        ASSERT_NE(r.rfind("ERR", 0), 0u) << r;
    }

    Session session;
    DispatchOutcome begun;
    auto begin = rig.Start("BEGIN", begun, &session);
    ASSERT_TRUE(rig.Drive(*begin)) << begun.response;

    DispatchOutcome out;
    auto read = rig.Start("SELECT * FROM big_read", out, &session);
    ASSERT_TRUE(rig.Drive(*read)) << out.response;
    EXPECT_EQ(out.response.rfind("ERR", 0), 0u)
        << "an answer too long to carry was carried anyway: " << out.response;
    EXPECT_NE(out.response.find("UNKNOWN_OUTCOME"), std::string::npos) << out.response;
    EXPECT_EQ(out.response.find("read the data back"), std::string::npos)
        << "a read was told to read the data back: " << out.response;

    // Still open, and still usable: the write below is the proof, because a
    // poisoned session refuses everything but ROLLBACK.
    ASSERT_FALSE(session.failed()) << "a failed read ended its transaction";
    DispatchOutcome wrote;
    auto write = rig.Start("INSERT INTO big_read VALUES (99999)", wrote, &session);
    ASSERT_TRUE(rig.Drive(*write)) << wrote.response;
    EXPECT_EQ(wrote.response.rfind("INSERTED", 0), 0u) << wrote.response;

    DispatchOutcome rb;
    auto rollback = rig.Start("ROLLBACK", rb, &session);
    ASSERT_TRUE(rig.Drive(*rollback)) << rb.response;
}

TEST_F(CoreRuntimeTest, TheSameReadOutsideATransactionTakesThePipelineAndHasNoCap) {
    // The other half of the sentence above, and what keeps the gate from
    // reading as a general regression: the pipeline is untouched for every
    // session that cannot enrol, which is every autocommit one. Same
    // relation, same rows, same statement - answered whole.
    std::optional<SessionStepClient> reads;  // declared first: outlives rig.dispatcher
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "big_read_auto");
    WireRemoteReads(rig, reads);

    auto more = rig.catalog2->AllocateRowIdRange(rig.oid, 512);
    ASSERT_TRUE(more.ok()) << more.status().message();
    rig.peer->row_id_leases().Grant(rig.oid, more.value(), 512);
    // Ten statements, not three hundred: the rig grants the peer sixteen
    // transaction ids, and each `INSERT` is one.
    for (int base = 0; base < 300; base += 30) {
        std::string ins = "INSERT INTO big_read_auto VALUES ";
        for (int i = 0; i < 30; ++i) {
            if (i > 0) ins += ", ";
            ins += "(" + std::to_string(100000 + base + i) + ")";
        }
        const std::string r = rig.peer->dispatcher().Dispatch(ins).response;
        ASSERT_NE(r.rfind("ERR", 0), 0u) << r;
    }

    DispatchOutcome out;
    auto read = rig.Start("SELECT * FROM big_read_auto", out);
    ASSERT_TRUE(rig.Drive(*read)) << out.response;
    ASSERT_NE(out.response.rfind("ERR", 0), 0u) << out.response;
    EXPECT_GT(out.response.size(), kShippedStatementReplyTextMax)
        << "the answer fit one slot, so this shape proves nothing about the cap";
    // The pipeline ran it, not the ship path.
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 0u);
}

TEST_F(CoreRuntimeTest, ASynchronousDispatchDoesNotShipBecauseItCannotAwaitTheAnswer) {
    // The rule that keeps `UnknownOutcome` rare: a path that cannot park
    // must not send, because a statement the owner may commit would have
    // nowhere to deliver its answer. `Dispatch()` is that path, and it
    // refuses exactly as it did before shipping existed.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_sync");

    const std::string out =
        rig.dispatcher->Dispatch("INSERT INTO shipped_sync VALUES (40)").response;
    EXPECT_NE(out.find("TXN_CONFLICT"), std::string::npos) << out;
    EXPECT_EQ(rig.ship->waiting(), 0u);
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 0u);
}

TEST_F(CoreRuntimeTest, AShippedStatementDoesNotShipOnward) {
    // The hop limit (session.hpp). Driven artificially, because the fork
    // cannot produce it: two cores would have to disagree about an owner.
    // The peer is asked to run a statement against a relation **core 0**
    // owns - so its own fork would ship it back, and without the limit the
    // two cores would pass it between them, each hop a fresh identity the
    // dedup record cannot recognise.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_hop");

    // Placed on core 0, which rotation cannot do - it skips the system core
    // (core_placement.hpp) - so the policy is switched for this one
    // relation and switched back.
    rig.catalog2->SetPlacementPolicy(catalog::PlacementPolicy::kCreatingCore);
    auto core0_oid =
        rig.catalog2->CreateTable(catalog::kNamespacePublic, "core0_owned", TwoColumnSchema(),
                                  catalog::ClusteredType::kBtree);
    rig.catalog2->SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    ASSERT_TRUE(core0_oid.ok()) << core0_oid.status().message();
    auto row = rig.catalog2->GetSysTableRow(core0_oid.value());
    ASSERT_TRUE(row.ok());
    ASSERT_EQ(row.value().owner_core, 0u);
    ASSERT_TRUE(core0_store_->Sync().ok());
    rig.peer->InvalidateCatalog();

    ASSERT_NE(rig.peer->statement_ship(), nullptr);
    ASSERT_TRUE(rig.peer->statement_ship()
                    ->Ship(/*owner_core=*/1, /*request_id=*/7, /*session_id=*/1,
                           /*sequence=*/1, core0_oid.value(), Role::kAdmin,
                           "INSERT INTO core0_owned VALUES (1)")
                    .ok());
    rig.Pump(32);

    // The peer ran it, found the relation foreign, and **refused** rather
    // than shipping it on: its client has nothing else parked.
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 1u);
    EXPECT_EQ(rig.peer->statement_ship()->waiting(), 1u)
        << "only the artificial request above should be parked";
    const ShippedStatementOutcome* answer = rig.peer->statement_ship()->Find(7);
    ASSERT_NE(answer, nullptr);
    ASSERT_TRUE(answer->arrived) << "the peer must have answered rather than shipped onward";
    EXPECT_FALSE(answer->status.ok());
    EXPECT_NE(answer->status.message().find("core 0"), std::string::npos)
        << answer->status.message();
}

// ---- A1: outcome integrity under adversarial delivery -------------------
//
// The post-SS5 verification order's first item, driven over the real ring
// rather than at the seam: engine-issued pks make a blind retry a second
// row, so the dedup record is attacked here with the two deliveries a
// routing layer will produce - the same identity arriving twice, and a
// reply lost after the owner has committed. Each case's verdict is the row
// count, not the returned status.

// How many rows of `SELECT *` carry `needle`. The row count is what A1
// asks for: a status can be right while the relation holds two rows.
static int CountOccurrences(const std::string& haystack, const std::string& needle) {
    int found = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + 1)) {
        ++found;
    }
    return found;
}

static int RowsWith(CoreRuntime& owner, const std::string& table, const std::string& needle) {
    return CountOccurrences(owner.dispatcher().Dispatch("SELECT * FROM " + table).response,
                            needle);
}

TEST_F(CoreRuntimeTest, TheSameShippedIdentityArrivingTwiceRunsOnceAndAnswersFromTheRecord) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_dup");

    // Shipped through the client directly: the dispatcher mints a fresh
    // sequence per statement, so it cannot produce the duplicate this
    // exists for - which is exactly why the record is the guard for a
    // retry path rather than for anything running today.
    const char* kSql = "INSERT INTO shipped_dup VALUES (41)";
    ASSERT_TRUE(rig.ship
                    ->Ship(/*owner_core=*/1, /*request_id=*/100, /*session_id=*/5,
                           /*sequence=*/1, rig.oid, Role::kReadWrite, kSql)
                    .ok());
    rig.PumpUntil([&rig] { return rig.ship->Settled(100); });
    const ShippedStatementOutcome* first = rig.ship->Find(100);
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(first->arrived) << "the first statement never answered";
    ASSERT_TRUE(first->status.ok()) << first->status.message();
    const std::string first_text = first->text;
    rig.ship->Close(100);

    // The same (session, sequence) again, on a new request id - which is
    // what a retry after a lost reply looks like from the owner's side.
    ASSERT_TRUE(rig.ship
                    ->Ship(/*owner_core=*/1, /*request_id=*/101, /*session_id=*/5,
                           /*sequence=*/1, rig.oid, Role::kReadWrite, kSql)
                    .ok());
    rig.PumpUntil([&rig] { return rig.ship->Settled(101); });
    const ShippedStatementOutcome* again = rig.ship->Find(101);
    ASSERT_NE(again, nullptr);
    ASSERT_TRUE(again->arrived);
    EXPECT_TRUE(again->status.ok()) << again->status.message();
    EXPECT_EQ(again->text, first_text) << "the recorded outcome, not a fresh one";

    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 1u);
    EXPECT_EQ(rig.peer->shipped_statements()->deduped(), 1u);
    // The verdict A1 asks for.
    EXPECT_EQ(RowsWith(*rig.peer, "shipped_dup", ",41"), 1) << "the duplicate inserted a second row";
    rig.ship->Close(101);
}

TEST_F(CoreRuntimeTest, AReplyLostAfterTheOwnerCommittedLeavesOneRowAndTheRetryFindsTheRecord) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_lost");

    const char* kSql = "INSERT INTO shipped_lost VALUES (42)";
    ASSERT_TRUE(rig.ship
                    ->Ship(/*owner_core=*/1, /*request_id=*/200, /*session_id=*/6,
                           /*sequence=*/1, rig.oid, Role::kReadWrite, kSql)
                    .ok());
    // Core 0 turns only far enough to put the request in the ring - `Ship`
    // submits a send task rather than sending inline - and then stops.
    for (int i = 0; i < 8; ++i) rig.core0->RunOnce();
    // **From here only the owner is pumped**, so the statement runs and
    // commits there while core 0 has drained nothing. This is the window
    // the whole scheme is written for: the effect stands, and the answer
    // has not landed.
    // **Only the owner turns here**, so this is `TurnUntil` over the peer's
    // reactor alone rather than `PumpUntil` - core 0 drains nothing, which
    // is the window the cell exists for.
    rig.TurnUntil([&rig] { rig.peer->scheduler().RunOnce(); },
                  [&rig] { return rig.peer->shipped_statements()->executed() != 0; }, 256);
    ASSERT_EQ(rig.peer->shipped_statements()->executed(), 1u) << "the owner never ran it";
    ASSERT_EQ(RowsWith(*rig.peer, "shipped_lost", ",42"), 1);

    // The reply path dies: the waiter is closed before core 0 ever handles
    // the answer, which is what a client whose statement was answered
    // `UnknownOutcome` on its deadline leaves behind.
    rig.ship->Close(200);
    rig.Pump(8);
    EXPECT_EQ(rig.ship->late_executed_replies(), 1u)
        << "the lost reply must be counted as a result nobody received";
    EXPECT_EQ(rig.ship->identity_mismatches(), 0u);

    // The retry. It must find the record - not re-run against an
    // engine-issued pk.
    ASSERT_TRUE(rig.ship
                    ->Ship(/*owner_core=*/1, /*request_id=*/201, /*session_id=*/6,
                           /*sequence=*/1, rig.oid, Role::kReadWrite, kSql)
                    .ok());
    rig.PumpUntil([&rig] { return rig.ship->Settled(201); });
    const ShippedStatementOutcome* retry = rig.ship->Find(201);
    ASSERT_NE(retry, nullptr);
    ASSERT_TRUE(retry->arrived);
    EXPECT_TRUE(retry->status.ok()) << retry->status.message();
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 1u);
    EXPECT_EQ(rig.peer->shipped_statements()->deduped(), 1u);
    EXPECT_EQ(RowsWith(*rig.peer, "shipped_lost", ",42"), 1) << "the retry inserted a second row";
    rig.ship->Close(201);
}

TEST_F(CoreRuntimeTest, AReconnectingClientTakesAFreshShipIdSoNoStaleSequenceMatchesIt) {
    // A1's quietest case: if an arrival core could hand a new connection a
    // session id a previous connection used, a stale sequence would match
    // the new session's record and one client's statement would be answered
    // with another's outcome. `next_ship_session_id_` is per core and
    // monotonic from 1 (command_dispatcher.hpp), and a `Session` mints from
    // it once - so ids are not reused within a mount, and a mount is the
    // longest an owner's record lives. Pinned rather than inherited.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_reconnect");

    Session first;
    DispatchOutcome out1;
    auto s1 = rig.Start("INSERT INTO shipped_reconnect VALUES (43)", out1, &first);
    ASSERT_TRUE(rig.Drive(*s1)) << out1.response;
    ASSERT_EQ(out1.response.rfind("INSERTED", 0), 0u) << out1.response;
    const std::uint64_t first_id = first.ship_id();
    EXPECT_NE(first_id, 0u);

    // The reconnect: a new `Session`, as a new connection gets.
    Session second;
    DispatchOutcome out2;
    auto s2 = rig.Start("INSERT INTO shipped_reconnect VALUES (44)", out2, &second);
    ASSERT_TRUE(rig.Drive(*s2)) << out2.response;
    ASSERT_EQ(out2.response.rfind("INSERTED", 0), 0u) << out2.response;
    EXPECT_NE(second.ship_id(), first_id) << "a reconnecting client reused a ship id";

    // Both statements ran: the second's sequence 1 did not match the
    // first's record, which is what a reused id would have produced.
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 2u);
    EXPECT_EQ(rig.peer->shipped_statements()->deduped(), 0u);
    EXPECT_EQ(rig.peer->shipped_statements()->unanswerable(), 0u);
    EXPECT_EQ(RowsWith(*rig.peer, "shipped_reconnect", ",43"), 1);
    EXPECT_EQ(RowsWith(*rig.peer, "shipped_reconnect", ",44"), 1);
}

// ---- A2: the client stops listening -------------------------------------
//
// Two endings where nobody is waiting for the answer: the arrival core's
// deadline, and a connection that went away while its statement was parked.
// What must hold across both is that the row lands exactly once, the waiter
// is reclaimed, and the client can tell the ending apart from a refusal.

TEST_F(CoreRuntimeTest, AShippedStatementsDeadlineIsUnknownOutcomeAndTheOwnerStillAppliesIt) {
    // Core 0 under a manual clock and the owner never pumped: the statement
    // parks until the deadline. What it is answered with is the one code no
    // retry loop follows - because the owner may yet run it, which the
    // second half of this test then makes it do.
    sched::ManualClock core0_clock;
    ForeignIndexRig rig(core0_clock);
    OpenForeignIndexRig(rig, "shipped_deadline");

    DispatchOutcome out;
    auto statement = rig.Start("INSERT INTO shipped_deadline VALUES (45)", out);
    for (int i = 0; i < 8; ++i) {
        EXPECT_NE(statement->Poll(), sched::PollResult::kDone) << out.response;
        rig.core0->RunOnce();  // the request leaves; nothing answers it
    }
    EXPECT_EQ(rig.ship->waiting(), 1u);

    core0_clock.Advance(kShippedStatementDeadlineNs);
    ASSERT_EQ(statement->Poll(), sched::PollResult::kDone);
    // **Distinguishable from a refusal, and non-retryable**: a client that
    // retried this would insert a second row against an engine-issued pk.
    EXPECT_EQ(out.response.rfind("ERR UNKNOWN_OUTCOME retryable=0 ", 0), 0u) << out.response;
    EXPECT_NE(out.response.find("read the data back"), std::string::npos) << out.response;
    // The slot is reclaimed at the deadline, not held for the answer.
    EXPECT_EQ(rig.ship->waiting(), 0u) << "the deadline leaked a waiter";

    // Now the owner runs. **It applies the statement** - there is no
    // cancellation in this engine, and a request already in the ring cannot
    // be recalled - so the effect stands while the client was told the
    // outcome is unknown. That is the documented contract, not a defect,
    // and this is the assertion that keeps it documented.
    // Pumped until the answer nobody wanted comes back, not for a fixed
    // count: the owner's commit is durable only when core 0's writer thread
    // has synced for it (`TurnUntil`). **Then turns anyway**, because every
    // assertion below is an *exactly-once* claim and a wait that stops at
    // the first reply has given a second one no chance to appear.
    rig.PumpUntil([&rig] { return rig.ship->late_executed_replies() != 0; }, 64);
    rig.Pump(8);
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 1u);
    EXPECT_EQ(RowsWith(*rig.peer, "shipped_deadline", ",45"), 1)
        << "the statement must apply exactly once, whatever the client was told";
    // And the answer nobody received is counted as exactly that.
    EXPECT_EQ(rig.ship->late_executed_replies(), 1u);
    EXPECT_EQ(rig.ship->late_refused_replies(), 0u);
    EXPECT_EQ(rig.ship->identity_mismatches(), 0u);
}

TEST_F(CoreRuntimeTest, ADroppedConnectionsShippedStatementFinishesAndReclaimsItsWaiter) {
    // A connection that goes away mid-statement does **not** destroy the
    // statement: `TcpServer::CloseClient` defers the whole teardown while
    // `conn.in_flight` and lets `OnStatementComplete` do it
    // (src/server/tcp_server.cpp, the `conn.closing` arm). So the parked
    // coroutine runs to completion with nobody to answer, and the waiter is
    // closed on the ordinary path.
    //
    // The consequence worth naming: for a shipped statement that deferral
    // is bounded by the ten-second ship deadline rather than by the
    // row-touch budget `CloseClient`'s comment cites, so a dropped
    // connection can hold its slot that long.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_dropped");

    DispatchOutcome out;
    auto statement = rig.Start("INSERT INTO shipped_dropped VALUES (46)", out);
    for (int i = 0; i < 4; ++i) {
        ASSERT_NE(statement->Poll(), sched::PollResult::kDone) << out.response;
        rig.core0->RunOnce();
    }
    ASSERT_EQ(rig.ship->waiting(), 1u);

    // The client is gone from here on: nothing reads `out`. The statement
    // is still driven, which is precisely what the server does.
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    EXPECT_EQ(out.response.rfind("INSERTED", 0), 0u) << out.response;
    EXPECT_EQ(rig.ship->waiting(), 0u) << "a dropped connection leaked a waiter";
    EXPECT_EQ(RowsWith(*rig.peer, "shipped_dropped", ",46"), 1);
}

TEST_F(CoreRuntimeTest, AParkedShippedStatementDestroyedUnderItsWaiterLeaksTheWaiter) {
    // **The invariant behind the test above, pinned from the other side.**
    // Nothing reclaims a waiter if the statement parked on it is destroyed
    // rather than completed: `Close` is called only from
    // `FinishShippedStatement`, which a destroyed coroutine never reaches.
    // Unreachable today because `CloseClient` defers teardown while a
    // statement is in flight - and this is what would break the moment a
    // cancellation path is added, so it is asserted rather than assumed.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_destroyed");

    DispatchOutcome out;
    auto statement = rig.Start("INSERT INTO shipped_destroyed VALUES (47)", out);
    for (int i = 0; i < 4; ++i) {
        ASSERT_NE(statement->Poll(), sched::PollResult::kDone) << out.response;
        rig.core0->RunOnce();
    }
    ASSERT_EQ(rig.ship->waiting(), 1u);

    statement.reset();  // the park destroyed, as cancellation would destroy it
    // The owner still runs it, and its commit waits on the writer thread -
    // so this waits for the run rather than counting turns (`TurnUntil`).
    // **On core 0's event, not the peer's**: what is asserted below is
    // core 0's waiter count, which nothing moves until the reply lands
    // there - and `Pump(8)` after it is the turns core 0 would need to
    // reclaim the waiter if a future cancellation path ever did. Waiting on
    // the peer's `executed()` alone would prove only that core 0 had not
    // reclaimed it within a single turn.
    rig.PumpUntil([&rig] { return rig.ship->late_executed_replies() != 0; }, 64);
    rig.Pump(8);
    EXPECT_EQ(rig.ship->waiting(), 1u)
        << "a destroyed park now reclaims its waiter - update this invariant, and "
           "docs/inflight/known-gaps.md's entry for it";
    // The owner ran it regardless, so the row is there and the answer went
    // to a waiter nobody will ever read.
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 1u);
    EXPECT_EQ(RowsWith(*rig.peer, "shipped_destroyed", ",47"), 1);
}

// ---- A3: the ring at capacity --------------------------------------------
//
// Shipped DDL was rare; shipped DML is the high-volume path, so the rings
// fill in ordinary operation rather than in a corner case. What must hold is
// that a full ring costs latency and nothing else: no message dropped (the
// worst outcome available), no answer invented, and no allocation.

TEST_F(CoreRuntimeTest, EveryStatementSurvivesARingFilledPastItsCapacity) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_saturate");

    // The rig's ring is 16 slots (`OpenForeignIndexRig`), so 40 statements
    // shipped before the owner runs once must overflow it by 24. Reads,
    // because they need no row-id lease and this is about the wire.
    constexpr int kStatements = 40;
    const std::uint64_t polls_before = rig.core0->polls_total(sched::SchedulingGroup::kSystem);
    for (int i = 0; i < kStatements; ++i) {
        ASSERT_TRUE(rig.ship
                        ->Ship(/*owner_core=*/1, /*request_id=*/static_cast<std::uint64_t>(300 + i),
                               /*session_id=*/static_cast<std::uint64_t>(i), /*sequence=*/1,
                               rig.oid, Role::kReadOnly, "SELECT * FROM shipped_saturate")
                        .ok())
            << i;
    }
    EXPECT_EQ(rig.ship->shipped(), static_cast<std::uint64_t>(kStatements));

    // Core 0 alone: the ring fills, and the sends past capacity are
    // refused by `TrySend` and **retried**, never dropped and never
    // reported upward (`sched/send_retry.hpp`).
    for (int i = 0; i < 32; ++i) rig.core0->RunOnce();
    const std::uint64_t polls_after = rig.core0->polls_total(sched::SchedulingGroup::kSystem);
    EXPECT_GT(polls_after - polls_before, static_cast<std::uint64_t>(kStatements))
        << "no send was re-polled, so the ring never filled and this test proves nothing";

    // Now both. Every statement answers - which is the B2 class: a dropped
    // message would leave its waiter to the deadline instead.
    for (int round = 0; round < 4096; ++round) {
        bool all = true;
        for (int i = 0; i < kStatements; ++i) {
            if (!rig.ship->Settled(static_cast<std::uint64_t>(300 + i))) all = false;
        }
        if (all) break;
        rig.Pump();
    }
    int arrived = 0;
    for (int i = 0; i < kStatements; ++i) {
        const ShippedStatementOutcome* out = rig.ship->Find(static_cast<std::uint64_t>(300 + i));
        ASSERT_NE(out, nullptr) << i;
        EXPECT_TRUE(out->arrived) << "statement " << i << " was never answered";
        EXPECT_TRUE(out->status.ok()) << i << ": " << out->status.message();
        if (out->arrived) ++arrived;
        rig.ship->Close(static_cast<std::uint64_t>(300 + i));
    }
    EXPECT_EQ(arrived, kStatements);
    EXPECT_EQ(rig.peer->shipped_statements()->executed(),
              static_cast<std::uint64_t>(kStatements));
    // Nothing was answered from a record, nothing was unanswerable, and no
    // reply went to the wrong waiter: distinct sessions, so a full ring
    // must not have reordered one statement's answer onto another.
    EXPECT_EQ(rig.peer->shipped_statements()->deduped(), 0u);
    EXPECT_EQ(rig.peer->shipped_statements()->unanswerable(), 0u);
    EXPECT_EQ(rig.ship->identity_mismatches(), 0u);
    EXPECT_EQ(rig.ship->late_executed_replies(), 0u);
    EXPECT_EQ(rig.ship->late_refused_replies(), 0u);
}

TEST_F(CoreRuntimeTest, AStormOfRefusedShippedDmlCostsTheOwnerNoPageAndNoMapGrowth) {
    // G2's storm, adapted to DML as A3 asks. G2 was a *conforming* retry
    // loop that destroyed an instance in 30 seconds because each refusal
    // allocated pages nothing frees. The shipped DML refusal path is the
    // same shape at a far higher rate, so its cost is audited by the two
    // numbers that grew there: the owner's page count and its allocation
    // map's residency.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_storm");

    auto owner_meta = [&] { return rig.peer->dispatcher().Dispatch("SHOW META").response; };
    auto field = [](const std::string& meta, const std::string& key) -> std::string {
        const std::size_t at = meta.find(" " + key + "=");
        if (at == std::string::npos) return "<missing>";
        const std::size_t from = at + key.size() + 2;
        return meta.substr(from, meta.find(' ', from) - from);
    };
    const std::string before = owner_meta();
    const std::string map_before = field(before, "map_pages_resident");
    const std::string regions_before = field(before, "map_regions");
    ASSERT_NE(map_before, "<missing>") << before;

    // Three refusal shapes a shipped DML client can actually produce,
    // stormed: a rank that forbids the write, a relation that does not
    // exist, and a statement the parser rejects. Every one crosses the
    // ring and is refused on the owner.
    std::uint64_t request_id = 400;
    for (int i = 0; i < 60; ++i) {
        const char* const shapes[] = {
            "INSERT INTO shipped_storm VALUES (1)",   // refused: readonly rank
            "UPDATE nosuch SET v = 1 WHERE v = 1",    // refused: no such relation
            "DELETE FROM shipped_storm WHERE",        // refused: parse
        };
        const Role roles[] = {Role::kReadOnly, Role::kReadWrite, Role::kReadWrite};
        for (int s = 0; s < 3; ++s) {
            ASSERT_TRUE(rig.ship
                            ->Ship(/*owner_core=*/1, request_id,
                                   /*session_id=*/static_cast<std::uint64_t>(1000 + s),
                                   /*sequence=*/static_cast<std::uint64_t>(i + 1), rig.oid,
                                   roles[s], shapes[s])
                            .ok());
            for (int round = 0; round < 256 && !rig.ship->Settled(request_id); ++round) {
                rig.Pump();
            }
            const ShippedStatementOutcome* out = rig.ship->Find(request_id);
            ASSERT_NE(out, nullptr);
            ASSERT_TRUE(out->arrived) << "shape " << s << " was never answered";
            EXPECT_FALSE(out->status.ok()) << "shape " << s << " was not refused";
            rig.ship->Close(request_id);
            ++request_id;
        }
    }

    const std::string after = owner_meta();
    EXPECT_EQ(field(after, "map_pages_resident"), map_before) << "the refusal storm grew the map";
    EXPECT_EQ(field(after, "map_regions"), regions_before);
    EXPECT_EQ(rig.ship->refusals(), 180u);
    EXPECT_EQ(rig.ship->identity_mismatches(), 0u);
    EXPECT_EQ(rig.peer->shipped_statements()->early_evictions(), 0u);
}

// ---- A4: per-session ordering across the ship boundary --------------------
//
// A session that issues S1 then S2 to the same owner must have them execute
// in that order. The ring is per-edge FIFO, but retry paths are historically
// where that is lost, so it is verified rather than inherited - and the
// visible failure is the one asserted here: a write followed by a read that
// does not see it.

TEST_F(CoreRuntimeTest, AShippedSessionReadsItsOwnWriteBack) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_ryow");

    // One session, both statements shipped to the same owner.
    Session session;
    DispatchOutcome wrote;
    auto insert = rig.Start("INSERT INTO shipped_ryow VALUES (48)", wrote, &session);
    ASSERT_TRUE(rig.Drive(*insert)) << wrote.response;
    ASSERT_EQ(wrote.response.rfind("INSERTED", 0), 0u) << wrote.response;

    DispatchOutcome read;
    auto select = rig.Start("SELECT * FROM shipped_ryow", read, &session);
    ASSERT_TRUE(rig.Drive(*select)) << read.response;
    EXPECT_NE(read.response.find(",48"), std::string::npos)
        << "a session did not see its own shipped write: " << read.response;
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 2u);
}

TEST_F(CoreRuntimeTest, AShippedSessionReadsItsOwnWriteBackWhenThatWriteWasRetried) {
    // The same ordering with a retry in front of it - which is the case the
    // ring's FIFO does not by itself cover, because the retry is a second
    // request for the same identity and its answer comes from the record
    // rather than from an execution.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_ryow_retry");

    auto settle = [&](std::uint64_t id) {
        rig.PumpUntil([&rig, id] { return rig.ship->Settled(id); });
        const ShippedStatementOutcome* out = rig.ship->Find(id);
        return out != nullptr && out->arrived ? out->status : Status::IoError("never answered");
    };

    const char* kInsert = "INSERT INTO shipped_ryow_retry VALUES (49)";
    ASSERT_TRUE(rig.ship
                    ->Ship(1, /*request_id=*/500, /*session_id=*/7, /*sequence=*/1, rig.oid,
                           Role::kReadWrite, kInsert)
                    .ok());
    ASSERT_TRUE(settle(500).ok());
    rig.ship->Close(500);

    // The retry of S1, answered from the record.
    ASSERT_TRUE(rig.ship
                    ->Ship(1, /*request_id=*/501, /*session_id=*/7, /*sequence=*/1, rig.oid,
                           Role::kReadWrite, kInsert)
                    .ok());
    ASSERT_TRUE(settle(501).ok());
    rig.ship->Close(501);
    ASSERT_EQ(rig.peer->shipped_statements()->deduped(), 1u);

    // S2, the read. It must see S1's row, and exactly one of it.
    ASSERT_TRUE(rig.ship
                    ->Ship(1, /*request_id=*/502, /*session_id=*/7, /*sequence=*/2, rig.oid,
                           Role::kReadOnly, "SELECT * FROM shipped_ryow_retry")
                    .ok());
    ASSERT_TRUE(settle(502).ok());
    const ShippedStatementOutcome* read = rig.ship->Find(502);
    ASSERT_NE(read, nullptr);
    EXPECT_NE(read->text.find(",49"), std::string::npos)
        << "the read did not see the write that preceded it: " << read->text;
    EXPECT_EQ(RowsWith(*rig.peer, "shipped_ryow_retry", ",49"), 1)
        << "the retried write inserted a second row";
    rig.ship->Close(502);
}

// ---- A5: the shape gates survive the fork ---------------------------------
//
// Shipping must not become a path that routes around a gate. The fork (SS2)
// sits after the relation is resolved and before `CheckWriteAffinity`, and
// it *returns* - so everything above it has already run, and everything
// below it is what the owner runs instead, through its own ordinary
// dispatcher. The gates that could therefore be lost are the owner's, and
// the peer-side shape gate (`workplan-peer-writer.md` §4: assertion-covered
// and unenforceable - FK-linked lifted 2026-09-01 by work order AI, cabined
// lifted 2026-09-02 by AK-S2) is the one a shipped write newly reaches -
// core 0 could write those relations itself, and a peer cannot.
//
// **The vehicle moved twice, and the property did not** (AI-R3). A5's real
// claim is *a shipped write is answered by the owner's own gate, byte for
// byte, with no retryable bit invented on the way* - which is independent
// of which arm answers. The FK shape proved it until 2026-09-01, the
// cabined shape until 2026-09-02, and both admit now; the caller-supplied
// pk refusal carries it (`AShippedRefusalCrossesTheRingByteIdenticalAndTerminal`),
// and the two cells below prove the converse for each lifted arm.

TEST_F(CoreRuntimeTest, AnFkLinkedPeerRelationNoLongerMeetsTheShapeGate) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_gate");

    // Both relations rotate onto the peer, so the child is a peer-owned
    // FK-linked relation. **Until 2026-09-01 that shape was refused
    // outright** - `funded_shape` required both fkey lists empty, on the
    // grounds that "validation reads the linked relation, which this core
    // may not fault". AH-T4 removed the reason: the forward check probes a
    // foreign parent instead of reading it (§2a), and the reverse refuses
    // by name on a foreign child (§3a). The arm lifted, and this cell is
    // its converse.
    //
    // **What this cell proves and what it deliberately does not.** It
    // asserts the peer's own dispatcher no longer answers the FK shape
    // gate. It does *not* drive the write to completion: this rig does not
    // refill a peer's transaction-id or row-id leases for a relation it
    // did not open itself, so a completed peer write meets
    // `TXN_CONFLICT retryable=1` forever here - a fixture limit, not an
    // engine one. The end-to-end write is AH-T6's, which builds the
    // two-process fixture for its measurement anyway.
    ASSERT_EQ(rig.dispatcher->Dispatch("CREATE TABLE fkparent (id int64, v int64) BTREE")
                  .response.substr(0, 3),
              "CRE");
    ASSERT_EQ(rig.dispatcher
                  ->Dispatch("CREATE TABLE fkchild (id int64, pid int64 REFERENCES fkparent) "
                             "BTREE")
                  .response.substr(0, 3),
              "CRE");
    ASSERT_TRUE(core0_store_->Sync().ok());
    rig.peer->InvalidateCatalog();

    // The owner's own answer, which is what the shape gate is a property
    // of. Whatever refuses now, it is not the FK arm.
    const std::string owner_says =
        rig.peer->dispatcher().Dispatch("INSERT INTO fkchild VALUES (1)").response;
    EXPECT_EQ(owner_says.find("FK-linked relation cannot take writes"), std::string::npos)
        << "the peer-writer FK arm still refuses: " << owner_says;
    // And a parent write, the other direction of the same arm: a relation
    // with `fkeys_in` was refused by the identical test.
    const std::string parent_says =
        rig.peer->dispatcher().Dispatch("INSERT INTO fkparent VALUES (1, 5)").response;
    EXPECT_EQ(parent_says.find("FK-linked relation cannot take writes"), std::string::npos)
        << "the arm still refuses a relation that is only a parent: " << parent_says;
}

TEST_F(CoreRuntimeTest, ACabinedPeerRelationTakesWritesAndItsOwnerServesTheCabin) {
    // AK-S2 (`instructions/v2.8.0/workorder-ak.md`): the funding gate's
    // Cabin arm is gone, because its ground - a peer had no Cabin store, so
    // a write there could append to no set - is gone: every core holds a
    // store. What this cell pins is the whole of a Cabin's life on the
    // core that owns the relation: the shipped write is admitted, the
    // owner observes a probed value, serves the next probe from its set,
    // appends the write that follows, and serves that too.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_gate2");

    ASSERT_EQ(rig.dispatcher
                  ->Dispatch("CREATE TABLE cabined (id int64, sym varchar CABIN) BTREE")
                  .response.substr(0, 3),
              "CRE");
    ASSERT_TRUE(core0_store_->Sync().ok());
    rig.peer->InvalidateCatalog();
    auto oid = rig.catalog2->FindTableOidByName("cabined");
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    FundPeerForRelation(rig, oid.value());
    ASSERT_NE(rig.peer->cabins(), nullptr) << "the peer was built with no Cabin store";

    DispatchOutcome first_write;
    auto to_cabined = rig.Start("INSERT INTO cabined VALUES ('zq')", first_write);
    ASSERT_TRUE(rig.Drive(*to_cabined)) << first_write.response;
    ASSERT_EQ(first_write.response.rfind("ERR", 0), std::string::npos) << first_write.response;
    EXPECT_EQ(RowsWith(*rig.peer, "cabined", "zq"), 1) << first_write.response;

    // A declared Cabin records at n=1: the first probe walks and observes,
    // the second is served from the owner's set. Asserted on the owner's
    // store, which is the only place work-not-done leaves a trace.
    const stats::CabinStore& owner_store = *rig.peer->cabins();
    for (int probe = 0; probe < 2; ++probe) {
        DispatchOutcome read;
        auto select = rig.Start("SELECT * FROM cabined WHERE sym = 'zq'", read);
        ASSERT_TRUE(rig.Drive(*select)) << read.response;
        ASSERT_EQ(read.response.rfind("ERR", 0), std::string::npos) << read.response;
        EXPECT_NE(read.response.find("zq"), std::string::npos) << read.response;
    }
    EXPECT_EQ(owner_store.stats().recordings, 1u) << "the owner never observed the value";
    EXPECT_EQ(owner_store.stats().hits, 1u) << "the second probe was not served from the set";

    // The write after observation is the case the arm was guarding: it
    // must append to the owner's set, or the set is a subset and the next
    // served probe a wrong answer. Two rows served, both carried.
    DispatchOutcome second_write;
    auto again = rig.Start("INSERT INTO cabined VALUES ('zq')", second_write);
    ASSERT_TRUE(rig.Drive(*again)) << second_write.response;
    ASSERT_EQ(second_write.response.rfind("ERR", 0), std::string::npos) << second_write.response;
    EXPECT_EQ(owner_store.stats().appends, 1u) << "the owner's write did not append to its set";
    DispatchOutcome served;
    auto third = rig.Start("SELECT * FROM cabined WHERE sym = 'zq'", served);
    ASSERT_TRUE(rig.Drive(*third)) << served.response;
    EXPECT_EQ(owner_store.stats().hits, 2u);
    EXPECT_EQ(CountOccurrences(served.response, "zq"), 2) << served.response;
    // The surface a client on the owner's own listener reads: ANALYZE's
    // counters are the owner's store's. (ANALYZE does not ship - the typed
    // read path serves a whole-row read and nothing else - so it is asked
    // on the owner, which is where a client of that relation would be.)
    const std::string analyzed =
        rig.peer->dispatcher().Dispatch("ANALYZE SELECT * FROM cabined WHERE sym = 'zq'").response;
    EXPECT_NE(analyzed.find("cabin_hits=1"), std::string::npos) << analyzed;

    // `SHOW CABINS` tells the truth on both cores (the review's C1): the
    // owner prints its store's counts, and core 0 - which holds no set for
    // a relation it does not own - prints the dash arm naming the owner
    // rather than zeros that would read as "never probed".
    const std::string owner_says = rig.peer->dispatcher().Dispatch("SHOW CABINS").response;
    EXPECT_NE(owner_says.find("rel=cabined"), std::string::npos) << owner_says;
    EXPECT_NE(owner_says.find("hits=" + std::to_string(owner_store.stats().hits)),
              std::string::npos)
        << owner_says;
    const std::string core0_says = rig.dispatcher->Dispatch("SHOW CABINS").response;
    EXPECT_NE(core0_says.find("held by core 1"), std::string::npos) << core0_says;
    EXPECT_EQ(core0_says.find("hits=0"), std::string::npos) << core0_says;
}

TEST_F(CoreRuntimeTest, AShippedRefusalCrossesTheRingByteIdenticalAndTerminal) {
    // **A5's property** (AI-R3), which lived on the cabined shape until
    // AK-S2 admitted it: a refusal the owner answers reaches the client
    // byte for byte - the code is a spelled token, so equality proves the
    // *code* survived the ring (`Status::FromWire` is where it would be
    // lost, degrading to a bare IoError) - and it is terminal, not the
    // `TXN_CONFLICT retryable=1` core 0's affinity check answered before
    // shipping. The vehicle now is a caller-supplied primary key, which a
    // peer refuses because admitting one writes the relation's catalog row
    // (workplan-peer-writer.md §7a) - outside AK's scope, so it stays.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_gate2");

    DispatchOutcome out;
    auto keyed = rig.Start("INSERT INTO shipped_gate2 VALUES (900, 5)", out);
    ASSERT_TRUE(rig.Drive(*keyed)) << out.response;
    ASSERT_EQ(out.response.rfind("ERR ", 0), 0u) << out.response;
    EXPECT_NE(out.response.find("NOT_IMPLEMENTED"), std::string::npos) << out.response;
    EXPECT_EQ(out.response,
              rig.peer->dispatcher().Dispatch("INSERT INTO shipped_gate2 VALUES (900, 5)").response);
    EXPECT_EQ(out.response.find("retryable=1"), std::string::npos) << out.response;
    EXPECT_EQ(out.response.find("TXN_CONFLICT"), std::string::npos) << out.response;
}

// ---- AI-T2: the cross-owner INSERT, driven end to end -------------------
//
// The cell `known-gaps.md` has been owed since AH-T2 and that AH-T4 and
// AH-T6 each deferred (`instructions/v2.8.0/workorder-ai.md`, AI-R4). Every
// piece of the crossing has had its own unit cell - the wire's seven, the
// intent's, the reverse check's two - and none of them drives a statement
// through fork -> park -> probe -> resume -> row. That is what hid the
// funding gate AH-T5 found: each piece was right and the path was
// unreachable.
//
// **Two policies make a two-core rig hold one relation on each side.**
// `kRotate` at two cores places *everything* on core 1 (the rotation is
// `kSystemCore + 1 + seq % (core_count - 1)`, which is core 1 for every
// seq), so a parent on core 0 needs `kCreatingCore` for the length of its
// CREATE and nothing else.

TEST_F(CoreRuntimeTest, ACrossOwnerInsertProbesTheParentsOwnerAndWritesTheChildRow) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "ai_base");

    rig.catalog2->SetPlacementPolicy(catalog::PlacementPolicy::kCreatingCore);
    ASSERT_EQ(rig.dispatcher->Dispatch("CREATE TABLE aiparent (id int64, v int64) BTREE")
                  .response.substr(0, 3),
              "CRE");
    rig.catalog2->SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    ASSERT_EQ(rig.dispatcher
                  ->Dispatch("CREATE TABLE aichild (id int64, pid int64 REFERENCES aiparent) "
                             "BTREE")
                  .response.substr(0, 3),
              "CRE");

    auto parent_oid = rig.catalog2->FindTableOidByName("aiparent");
    ASSERT_TRUE(parent_oid.ok()) << parent_oid.status().message();
    auto child_oid = rig.catalog2->FindTableOidByName("aichild");
    ASSERT_TRUE(child_oid.ok()) << child_oid.status().message();
    auto parent_row = rig.catalog2->GetSysTableRow(parent_oid.value());
    ASSERT_TRUE(parent_row.ok());
    ASSERT_EQ(parent_row.value().owner_core, 0u) << "the parent must be core 0's for this to cross";

    // **Sync before funding, not after.** `AdmitWritePages` faults each
    // granted page for read before it restamps it, so a creation page still
    // only in core 0's cache abandons the whole grant silently and the peer
    // is left owning a relation it may not write.
    ASSERT_TRUE(core0_store_->Sync().ok());
    rig.peer->InvalidateCatalog();
    FundPeerForRelation(rig, child_oid.value());

    // A parent row with a **named** pk, so the child below references a
    // value this test knows rather than one it parses back out of a reply.
    ASSERT_NE(rig.dispatcher->Dispatch("INSERT INTO aiparent VALUES (7, 5)").response.rfind("ERR",
                                                                                            0),
              0u);

    // **Absent rather than zeroed**, before anything crosses: an instance
    // that has never probed carries no foreign-key block at all, so the
    // five counters below are a statement that something happened rather
    // than a row of zeroes on every `SHOW META` ever printed.
    EXPECT_EQ(rig.peer->dispatcher().Dispatch("SHOW META").response.find("fk_probes_sent="),
              std::string::npos);

    DispatchOutcome out;
    auto statement = rig.StartOnPeer("INSERT INTO aichild VALUES (7)", out);

    // **Parked.** The first poll runs the extraction pass, finds the parent
    // foreign, sends one probe and returns pending - before any row work,
    // which is AH-R1's whole point.
    ASSERT_EQ(statement->Poll(), sched::PollResult::kSuspended) << out.response;
    EXPECT_EQ(rig.fk_server->probes(), 0u)
        << "the request cannot have been handled before the ring was pumped";

    // **The probe crosses, the owner answers, the intent is left behind.**
    // One pump is the peer's send and core 0's handling of it.
    rig.Pump();
    EXPECT_EQ(rig.fk_server->probes(), 1u) << "core 0 never saw the probe";
    EXPECT_EQ(rig.fk_intents.live_rows(), 1u)
        << "a passing probe must leave a reference intent on the parent's owner";

    // **Resumed, and the row is written.** One probe round for one
    // statement, whatever its row count (AH-R2).
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    EXPECT_NE(out.response.rfind("ERR", 0), 0u) << "the cross-owner INSERT: " << out.response;
    EXPECT_EQ(rig.fk_server->probes(), 1u) << "one round per distinct owner, not per row";

    // The row is on the peer, readable through the peer's own dispatcher.
    const std::string rows = rig.peer->dispatcher().Dispatch("SELECT pid FROM aichild").response;
    EXPECT_EQ(rows.rfind("ERR", 0), std::string::npos) << rows;
    EXPECT_NE(rows.find("7"), std::string::npos) << "the child row is not there: " << rows;

    // **And the intent ended with the statement** (F1). An autocommit
    // statement is its own transaction, so no `COMMIT` will ever run for it
    // and the decide that releases its intents is the one it sends itself.
    // Until 2026-09-02 it sent none: `live_rows` stayed at 1 for the life
    // of the process and the parent row answered `TXN_CONFLICT
    // retryable=1` to every `DELETE` - a retry loop that could not succeed.
    rig.Pump(8);
    EXPECT_EQ(rig.fk_intents.live_rows(), 0u)
        << "the autocommit statement left its reference intent behind";
    // The client-visible half of the same fact. The parent's `DELETE` is
    // still refused - RESTRICT cannot see a child on another core, which is
    // §3a's own refusal - but it is no longer refused *by the intent*, and
    // the two are different sentences with different codes.
    const std::string del =
        rig.dispatcher->Dispatch("DELETE FROM aiparent WHERE id = 7").response;
    EXPECT_EQ(del.find("relied on by a foreign key check"), std::string::npos) << del;

    // **Counted, not inferred** (AI-T3): the crossing is a fact on both
    // cores' `SHOW META`, which is what lets a measurement tell a crossing
    // from a colocated statement that never left. One round for one
    // statement (AH-R2), one intent granted and the same one released.
    const std::string peer_meta = rig.peer->dispatcher().Dispatch("SHOW META").response;
    EXPECT_NE(peer_meta.find("fk_probes_sent=1"), std::string::npos) << peer_meta;
    // **And the two rounds are timed apart** (AH-T6). One probe round and
    // one release decide, both walked by the core that owns the child -
    // which is the split `results-ai-t3-fk-crossing-cost` priced together
    // and named as owed.
    EXPECT_NE(peer_meta.find("fk_probe_round_n=1"), std::string::npos) << peer_meta;
    EXPECT_NE(peer_meta.find("fk_release_decide_n=1"), std::string::npos) << peer_meta;

    // **And the healthy path does not read as a lost transaction half.**
    // A decide reaching a core with no context is `ShippedStatementExecutor`'s
    // anomaly - it logs an Error and bumps `decide_refusals`, the counter
    // that means a participant is missing - and an intent holder has no
    // context by construction, so before the wire carried `intent_only`
    // *every* cross-owner foreign-key statement tripped it on its success
    // path. The tests were green throughout, because they asserted the
    // intent was released and never looked at what the release cost.
    EXPECT_EQ(rig.executor->decide_refusals(), 0u)
        << "the parent's owner reported a 2PC anomaly on a healthy crossing";

    const std::string core0_meta = rig.dispatcher->Dispatch("SHOW META").response;
    EXPECT_NE(core0_meta.find("fk_intents_granted=1"), std::string::npos) << core0_meta;
    EXPECT_NE(core0_meta.find("fk_intents_released=1"), std::string::npos) << core0_meta;
    EXPECT_NE(core0_meta.find("fk_intents_live=0"), std::string::npos) << core0_meta;
}

TEST_F(CoreRuntimeTest, ACrossOwnerFkWriteInATransactionCommitsAndItsDecideEndsTheIntent) {
    // The same crossing inside an explicit transaction, which is where the
    // intent's **end** is observable: the probe records the parent's owner
    // as an intent holder, the COMMIT's decide reaches it, and the decide
    // is the only thing that releases what the probe granted (AH-R5).
    //
    // **Three defects sat between this statement and its commit**, all
    // found by AI-T2 and all fixed 2026-09-02 on the operator's rulings:
    //
    //   F2 the shipping identity was minted only by a ship, so a session
    //      whose first cross-core contact was a probe had none, and its
    //      COMMIT refused "a cross-owner transaction has participants but
    //      no shipping identity" - a branch whose comment called its own
    //      state impossible, because it was written before anything but a
    //      ship could enrol.
    //   F3 the same zero made every un-shipped session share the intent
    //      holder key `(core, 0)`, so one decide would release another
    //      session's intents.
    //   F4 the probe enrolled a *participant*, and a participant is asked
    //      to prepare. An intent holder has no rows and no context, so the
    //      owner answered "holds no transaction for core 1's session N"
    //      and the transaction aborted, retryably, forever. An intent
    //      holder is not a participant: it takes the decide and not the
    //      prepare.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "aitxn_base");

    rig.catalog2->SetPlacementPolicy(catalog::PlacementPolicy::kCreatingCore);
    ASSERT_EQ(rig.dispatcher->Dispatch("CREATE TABLE txnparent (id int64, v int64) BTREE")
                  .response.substr(0, 3),
              "CRE");
    rig.catalog2->SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    ASSERT_EQ(rig.dispatcher
                  ->Dispatch("CREATE TABLE txnchild (id int64, pid int64 REFERENCES txnparent) "
                             "BTREE")
                  .response.substr(0, 3),
              "CRE");
    auto child_oid = rig.catalog2->FindTableOidByName("txnchild");
    ASSERT_TRUE(child_oid.ok()) << child_oid.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());
    rig.peer->InvalidateCatalog();
    FundPeerForRelation(rig, child_oid.value());
    ASSERT_NE(rig.dispatcher->Dispatch("INSERT INTO txnparent VALUES (7, 5)").response.rfind("ERR",
                                                                                             0),
              0u);

    Session sess;
    DispatchOutcome begun;
    auto begin = rig.StartOnPeer("BEGIN", begun, &sess);
    ASSERT_TRUE(rig.Drive(*begin)) << begun.response;
    ASSERT_EQ(begun.response.rfind("ERR", 0), std::string::npos) << begun.response;

    DispatchOutcome wrote;
    auto insert = rig.StartOnPeer("INSERT INTO txnchild VALUES (7)", wrote, &sess);
    ASSERT_TRUE(rig.Drive(*insert)) << wrote.response;
    ASSERT_NE(wrote.response.rfind("ERR", 0), 0u) << "the cross-owner write: " << wrote.response;
    EXPECT_EQ(rig.fk_intents.live_rows(), 1u) << "the probe left no intent to release";

    // F2's assertion: the identity exists because the probe minted it, and
    // a holder with no identity cannot be told anything.
    EXPECT_NE(sess.ship_id(), 0u);
    // F4's: the owner is on the decide's list and not on the prepare's.
    EXPECT_FALSE(sess.has_participants()) << "an intent holder must not be prepared";
    EXPECT_TRUE(sess.has_intent_holders());

    DispatchOutcome committed;
    auto commit = rig.StartOnPeer("COMMIT", committed, &sess);
    ASSERT_TRUE(rig.Drive(*commit, 512)) << committed.response;
    EXPECT_NE(committed.response.rfind("ERR", 0), 0u)
        << "the cross-owner commit: " << committed.response;

    // **And the intent is gone**, because the decide reached the owner that
    // granted it.
    rig.Pump(8);
    EXPECT_EQ(rig.fk_intents.live_rows(), 0u)
        << "the decide did not release the reference intent";

    // The parent is deletable again, which is the client-visible statement
    // of the intent's end and the thing the intent was holding still.
    const std::string del =
        rig.dispatcher->Dispatch("DELETE FROM txnparent WHERE id = 7").response;
    EXPECT_EQ(del.find("relied on by a foreign key check"), std::string::npos) << del;

    // **And the healthy path does not read as a lost transaction half.**
    // A decide reaching a core with no context is `ShippedStatementExecutor`'s
    // anomaly - it logs an Error and bumps `decide_refusals`, the counter
    // that means a participant is missing - and an intent holder has no
    // context by construction, so before the wire carried `intent_only`
    // *every* cross-owner foreign-key statement tripped it on its success
    // path. The tests were green throughout, because they asserted the
    // intent was released and never looked at what the release cost.
    EXPECT_EQ(rig.executor->decide_refusals(), 0u)
        << "the parent's owner reported a 2PC anomaly on a healthy crossing";
}

TEST_F(CoreRuntimeTest, AParticipantCoordinatesItsOwnIntentReleaseThroughTheDecidedCommit) {
    // **The path `foreign-keys.md` §2b names as load-bearing and untested.**
    // Every other cell here has the core that probes also be the core that
    // decides. This one takes them apart: the write that probes is itself a
    // **shipped** statement, so the intent holder is enrolled on the
    // *participant's* context session, and the participant sends its own
    // decide when the coordinator's decision is applied to it.
    //
    // The chain, and each link is asserted below:
    //
    //   core 0   BEGIN, INSERT INTO <child> - ships to the peer, which owns it
    //   peer     executes it, finds the parent foreign, probes core 0,
    //            records core 0 as an intent holder on *its* context session
    //   core 0   COMMIT - prepares the peer (it holds rows), then decides it
    //   peer     applies that decision by dispatching COMMIT on the context
    //            session, which forks on `has_intent_holders()` and sends
    //            the peer's **own** decide to core 0
    //   core 0   releases the intent it granted
    //
    // So core 0 is the outer transaction's coordinator *and* the inner
    // decide's participant, and the two are keyed differently - the intent
    // by `(peer, the context session's ship id)`, the outer transaction by
    // `(core 0, the client session's)`. Nothing but this cell shows that
    // they do not collide.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "selfrelease");
    OpenCrossOwnerFkPair(rig, "sr");
    ASSERT_NE(rig.dispatcher->Dispatch("INSERT INTO srp VALUES (7, 5)").response.rfind("ERR", 0),
              0u);

    Session client;
    DispatchOutcome begun;
    auto begin = rig.Start("BEGIN", begun, &client);
    ASSERT_TRUE(rig.Drive(*begin)) << begun.response;

    DispatchOutcome wrote;
    auto write = rig.Start("INSERT INTO src VALUES (7)", wrote, &client);
    ASSERT_TRUE(rig.Drive(*write)) << wrote.response;
    ASSERT_EQ(wrote.response.rfind("INSERTED", 0), 0u) << wrote.response;

    // **The holder is the peer's, not this session's.** Core 0's client
    // session enrolled the peer as a *participant* - it holds rows - and
    // recorded no intent holder at all, because core 0 never probed
    // anybody. The intent on core 0 was granted to the peer's context
    // session, and it is live because the transaction is.
    EXPECT_EQ(client.participants().size(), 1u);
    EXPECT_FALSE(client.has_intent_holders())
        << "the coordinator recorded a holder it never asked for";
    EXPECT_EQ(rig.fk_intents.live_rows(), 1u)
        << "the shipped write did not leave a reference intent on the parent's owner";

    DispatchOutcome committed;
    auto commit = rig.Start("COMMIT", committed, &client);
    ASSERT_TRUE(rig.Drive(*commit, 512)) << committed.response;
    EXPECT_NE(committed.response.rfind("ERR", 0), 0u)
        << "the cross-owner commit: " << committed.response;

    // **And the participant's own decide ended it.** Core 0 sent one decide
    // - to the peer, for the outer transaction - and that decide carries no
    // intent of core 0's to release. What released this one is the decide
    // the *peer* sent back, from the COMMIT its decision dispatched.
    rig.Pump(16);
    EXPECT_EQ(rig.fk_intents.live_rows(), 0u)
        << "nobody released the intent the shipped write left behind";

    // Core 0 is the holder here, so its executor is the one that would cry
    // anomaly if `intent_only` were not carried through this longer chain.
    EXPECT_EQ(rig.executor->decide_refusals(), 0u)
        << "the intent holder reported a 2PC anomaly on a healthy crossing";

    // The row is on its owner, and the parent is no longer pinned by an
    // intent - §3a's own refusal is what answers now.
    const std::string rows = rig.peer->dispatcher().Dispatch("SELECT pid FROM src").response;
    EXPECT_NE(rows.find("7"), std::string::npos) << rows;
    const std::string del = rig.dispatcher->Dispatch("DELETE FROM srp WHERE id = 7").response;
    EXPECT_EQ(del.find("relied on by a foreign key check"), std::string::npos) << del;
}

// ---- AH-T6: the verdicts the crossing carries, and the race it opens ----
//
// AI-T2 drove the *present-parent* fixture end to end and left the other
// three of H-AH1's named, because two of them could not be asserted until
// F1/F4 were fixed. They are here, and one of them turned out not to exist
// in this shape at all - which is a finding rather than a gap, and is said
// so in the cell that would have held it.

TEST_F(CoreRuntimeTest, ACrossOwnerInsertNamingAnAbsentParentIsRefusedAndWritesNoRow) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "ah6_absent");
    OpenCrossOwnerFkPair(rig, "abs");

    // No parent row was ever written, so the owner's answer is a verdict
    // and not a wait: `kViolation` travels back over the ring and the
    // child's core refuses on it.
    DispatchOutcome out;
    auto statement = rig.StartOnPeer("INSERT INTO absc VALUES (4242)", out);
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    EXPECT_EQ(out.response.rfind("ERR ", 0), 0u) << out.response;
    EXPECT_NE(out.response.find("FK_VIOLATION"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find("which does not exist"), std::string::npos) << out.response;
    // **Terminal, not retryable.** A violation re-run violates again, and a
    // client that retried it would spin - which is the distinction F3 draws
    // between this and the busy answer two cells below.
    EXPECT_EQ(out.response.find("retryable=1"), std::string::npos) << out.response;

    // And nothing was written. The check runs before the id is allocated
    // (§2), so a refused row costs no Keystone id either - what this cell
    // can see from outside is that the relation is empty.
    const std::string rows = rig.peer->dispatcher().Dispatch("SELECT pid FROM absc").response;
    EXPECT_EQ(rows.find("4242"), std::string::npos) << rows;

    // **The intent is the other half of "nothing happened".** A violation
    // promises nothing - there is no row to hold still - so the owner
    // grants no intent, and a parent that was never vouched for is not
    // pinned by a statement that failed.
    const std::string meta = rig.dispatcher->Dispatch("SHOW META").response;
    EXPECT_EQ(meta.find("fk_intents_granted=1"), std::string::npos) << meta;
}

TEST_F(CoreRuntimeTest, ACrossOwnerParentDeleteOnASynchronousPathIsRetryableNotTerminal) {
    // **This cell used to say the fixture could not exist.** Its former
    // name - `ACrossOwnerParentCannotBeRetiredAtAllSoThatFixtureCannotExist`
    // - recorded §3a's old refusal: a parent `DELETE` was refused outright
    // when the child lived on another core, because RESTRICT needs an
    // authoritative "no children" and this core cannot see them. **AJ-T3
    // converts that refusal into a result**, so the assertion it carried is
    // now false by design and the cell says the new thing instead.
    //
    // What is left here is the *synchronous* path, which is a different
    // refusal with a different meaning. `Dispatch` has no reactor, so the
    // fan-out cannot park on its answers - and the answer is therefore
    // **retryable**: the statement did not run, and a served connection
    // will run it. The old refusal was `NOT_IMPLEMENTED` and terminal,
    // which is the one thing a client must no longer be told, because
    // retrying now works.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "ah6_retire");
    OpenCrossOwnerFkPair(rig, "ret");
    ASSERT_NE(rig.dispatcher->Dispatch("INSERT INTO retp VALUES (7, 5)").response.rfind("ERR", 0),
              0u);

    const std::string del = rig.dispatcher->Dispatch("DELETE FROM retp WHERE id = 7").response;
    EXPECT_EQ(del.rfind("ERR ", 0), 0u) << del;
    EXPECT_NE(del.find("retryable=1"), std::string::npos) << del;
    EXPECT_NE(del.find("reverse probe needs the reactor path"), std::string::npos) << del;
    // The wording names the side that is elsewhere, and for a DELETE that
    // is the children - the parent is this core's.
    EXPECT_EQ(del.find("parent is owned by another core"), std::string::npos) << del;
    EXPECT_EQ(del.find("NOT_IMPLEMENTED"), std::string::npos)
        << "the terminal refusal survived AJ-T3: " << del;

    // The row is still there, which is what "fail-closed" means here: the
    // refusal did not half-delete anything.
    const std::string rows = rig.dispatcher->Dispatch("SELECT v FROM retp").response;
    EXPECT_NE(rows.find("5"), std::string::npos) << rows;

    // **And nothing was left registered.** The fork registered the row
    // before it sent, and this path has no resume whose `EndWrite` would
    // clear it - so `DispatchAndStage`'s refusal arm clears it instead
    // (AJ-T1's B3). Left behind, it would answer busy to every forward
    // probe for this row until the process ended.
    EXPECT_EQ(rig.fk_pending_deletes.live_rows(), 0u)
        << "a refused fan-out left its pending-delete registration behind";
}

// ---- AK-S3: the reverse fan-out, driven, and the collect pass -------------
//
// AJ-T3 built the chain - fork, register, intent check, fan-out, park,
// resume, per-row check from held verdicts - and landed with no cell that
// drives it end to end. The first cell here is that owed cell for the shape
// AJ-R2 admitted (a bare pk equality); the rest are AK-S3's: any other
// WHERE collects its pks by a read-only pass and fans out over them, a set
// too large for one message takes another round, and a row that appears
// while the statement is parked is caught by the round that follows.

TEST_F(CoreRuntimeTest, ACrossOwnerParentDeleteByPkFansOutAndDeletes) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "ak3_pk");
    OpenCrossOwnerFkPair(rig, "pk");
    ASSERT_NE(rig.dispatcher->Dispatch("INSERT INTO pkp VALUES (7, 5), (8, 5)").response.rfind("ERR", 0),
              0u);
    // A child of 8 on the peer, none of 7.
    DispatchOutcome child;
    auto wrote = rig.StartOnPeer("INSERT INTO pkc VALUES (8)", child);
    ASSERT_TRUE(rig.Drive(*wrote)) << child.response;
    ASSERT_NE(child.response.rfind("ERR", 0), 0u) << child.response;

    // Unreferenced: the fan-out answers clear and the row goes.
    DispatchOutcome gone;
    auto del7 = rig.Start("DELETE FROM pkp WHERE id = 7", gone);
    ASSERT_TRUE(rig.Drive(*del7)) << gone.response;
    EXPECT_EQ(gone.response, "DELETED 1") << gone.response;
    // Referenced from the peer: RESTRICT, answered by the child's owner and
    // spelled as the local check spells it.
    DispatchOutcome kept;
    auto del8 = rig.Start("DELETE FROM pkp WHERE id = 8", kept);
    ASSERT_TRUE(rig.Drive(*del8)) << kept.response;
    EXPECT_NE(kept.response.find("FK_VIOLATION"), std::string::npos) << kept.response;
    EXPECT_EQ(RowsWith(*rig.peer, "pkc", "8"), 1);
    const std::string rows = rig.dispatcher->Dispatch("SELECT id FROM pkp").response;
    EXPECT_EQ(rows.find("7"), std::string::npos) << rows;
    EXPECT_NE(rows.find("8"), std::string::npos) << rows;
    // Nothing left registered by either statement.
    EXPECT_EQ(rig.fk_pending_deletes.live_rows(), 0u);
}

TEST_F(CoreRuntimeTest, ACrossOwnerParentDeleteByPredicateCollectsThenFansOut) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "ak3_pred");
    OpenCrossOwnerFkPair(rig, "pr");
    ASSERT_NE(rig.dispatcher
                  ->Dispatch("INSERT INTO prp VALUES (1, 5), (2, 5), (3, 5), (4, 6)")
                  .response.rfind("ERR", 0),
              0u);
    DispatchOutcome child;
    auto wrote = rig.StartOnPeer("INSERT INTO prc VALUES (4)", child);
    ASSERT_TRUE(rig.Drive(*wrote)) << child.response;
    ASSERT_NE(child.response.rfind("ERR", 0), 0u) << child.response;

    // Three rows named by a non-pk predicate: collected, asked about on the
    // peer in one round, deleted.
    DispatchOutcome three;
    auto del5 = rig.Start("DELETE FROM prp WHERE v = 5", three);
    ASSERT_TRUE(rig.Drive(*del5)) << three.response;
    EXPECT_EQ(three.response, "DELETED 3") << three.response;
    // The referenced one, by the same shape: RESTRICT from the fan-out.
    DispatchOutcome kept;
    auto del6 = rig.Start("DELETE FROM prp WHERE v = 6", kept);
    ASSERT_TRUE(rig.Drive(*del6)) << kept.response;
    EXPECT_NE(kept.response.find("FK_VIOLATION"), std::string::npos) << kept.response;
    // Nothing collected: nothing asked, nothing deleted, no refusal.
    DispatchOutcome none;
    auto del9 = rig.Start("DELETE FROM prp WHERE v = 99", none);
    ASSERT_TRUE(rig.Drive(*del9)) << none.response;
    EXPECT_EQ(none.response, "DELETED 0") << none.response;
    EXPECT_EQ(rig.fk_pending_deletes.live_rows(), 0u);
    // The three unreferenced rows are gone and the referenced one stays.
    const std::string rows = rig.dispatcher->Dispatch("SELECT id, v FROM prp").response;
    EXPECT_EQ(CountOccurrences(rows, ",5"), 0) << rows;
    EXPECT_EQ(CountOccurrences(rows, ",6"), 1) << rows;
}

TEST_F(CoreRuntimeTest, ACollectedSetPastOneMessageTakesAnotherRound) {
    // More rows than one reverse message carries per owner: the first round
    // asks about `kFkReverseProbeMaxEntries`, the resume finds those held,
    // asks about the rest, and the walk deletes them all.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "ak3_rounds");
    OpenCrossOwnerFkPair(rig, "rd");
    const int rows = static_cast<int>(kFkReverseProbeMaxEntries) + 5;
    std::string values;
    for (int i = 1; i <= rows; ++i) {
        values += (i == 1 ? "(" : ", (") + std::to_string(i) + ", 5)";
    }
    ASSERT_NE(rig.dispatcher->Dispatch("INSERT INTO rdp VALUES " + values).response.rfind("ERR", 0),
              0u);

    DispatchOutcome out;
    auto del = rig.Start("DELETE FROM rdp WHERE v = 5", out);
    ASSERT_TRUE(rig.Drive(*del, 1024)) << out.response;
    EXPECT_EQ(out.response, "DELETED " + std::to_string(rows)) << out.response;
    EXPECT_EQ(rig.fk_pending_deletes.live_rows(), 0u);
}

TEST_F(CoreRuntimeTest, ARowAppearingWhileParkedIsRefusedRetryablyAndDeletedOnRetry) {
    // The collect pass fixed the rows under one snapshot; a row committed
    // while the fan-out was out is not among them and no owner was asked
    // about it. The walk meets it with no answer and refuses retryably
    // rather than marking it - never a row its children's owner was not
    // asked about - and the retry's pass sees it. **And the registration
    // survives the park** (the review's C1): before AK-S3's follow-up a
    // parked DELETE fell through to `EndWrite` and released its rows before
    // the owner had answered, which is the window AJ-R3(a) exists to close.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "ak3_race");
    OpenCrossOwnerFkPair(rig, "rc");
    ASSERT_NE(rig.dispatcher->Dispatch("INSERT INTO rcp VALUES (1, 5)").response.rfind("ERR", 0),
              0u);

    DispatchOutcome out;
    auto del = rig.Start("DELETE FROM rcp WHERE v = 5", out);
    ASSERT_NE(del->Poll(), sched::PollResult::kDone) << "the fan-out did not park";
    EXPECT_EQ(rig.fk_pending_deletes.live_rows(), 1u) << "the park released the registration";
    // Committed on core 0 while the DELETE is parked on the peer's answer.
    ASSERT_NE(rig.dispatcher->Dispatch("INSERT INTO rcp VALUES (2, 5)").response.rfind("ERR", 0),
              0u);
    ASSERT_TRUE(rig.Drive(*del)) << out.response;
    EXPECT_EQ(out.response.rfind("ERR ", 0), 0u) << out.response;
    EXPECT_NE(out.response.find("TXN_CONFLICT"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find("retryable=1"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find("became visible after"), std::string::npos) << out.response;
    // Nothing half-deleted: the refusal took the statement back whole.
    EXPECT_EQ(CountOccurrences(rig.dispatcher->Dispatch("SELECT id, v FROM rcp").response, ",5"),
              2);
    EXPECT_EQ(rig.fk_pending_deletes.live_rows(), 0u);

    DispatchOutcome again;
    auto retry = rig.Start("DELETE FROM rcp WHERE v = 5", again);
    ASSERT_TRUE(rig.Drive(*retry)) << again.response;
    EXPECT_EQ(again.response, "DELETED 2") << again.response;
    EXPECT_EQ(rig.fk_pending_deletes.live_rows(), 0u);
}

TEST_F(CoreRuntimeTest, ACrossOwnerInsertNamingAnInFlightParentAnswersRetryable) {
    // H-AH1's fourth fixture. The parent row exists but belongs to a
    // transaction that has not ended, so the owner's check - which reads
    // **latest state**, minted on the owner rather than carried on the wire
    // (§4) - can say neither pass nor violation. F3's answer is busy, and
    // busy is `TXN_CONFLICT` with the retryable bit set, because the right
    // answer depends on how the other transaction ends.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "ah6_inflight");
    OpenCrossOwnerFkPair(rig, "infl");

    // Core 0's own transaction, left open across the child's statement.
    Session writer;
    DispatchOutcome begun;
    auto begin = rig.Start("BEGIN", begun, &writer);
    ASSERT_TRUE(rig.Drive(*begin)) << begun.response;
    DispatchOutcome wrote;
    auto insert = rig.Start("INSERT INTO inflp VALUES (9, 5)", wrote, &writer);
    ASSERT_TRUE(rig.Drive(*insert)) << wrote.response;
    ASSERT_NE(wrote.response.rfind("ERR", 0), 0u) << wrote.response;

    DispatchOutcome out;
    auto child = rig.StartOnPeer("INSERT INTO inflc VALUES (9)", out);
    ASSERT_TRUE(rig.Drive(*child)) << out.response;
    EXPECT_EQ(out.response.rfind("ERR ", 0), 0u) << out.response;
    EXPECT_NE(out.response.find("TXN_CONFLICT"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find("retryable=1"), std::string::npos) << out.response;

    // **And the busy answer granted nothing**, which is what makes it safe
    // to retry: an intent taken on a row the check could not vouch for
    // would pin a parent for a statement that never ran.
    const std::string meta = rig.dispatcher->Dispatch("SHOW META").response;
    EXPECT_EQ(meta.find("fk_intents_granted=1"), std::string::npos) << meta;

    DispatchOutcome rolled;
    auto rollback = rig.Start("ROLLBACK", rolled, &writer);
    ASSERT_TRUE(rig.Drive(*rollback)) << rolled.response;
}

TEST_F(CoreRuntimeTest, AParentDeleteMeetingALiveForeignIntentAnswersBusyBeforeAnythingElse) {
    // **H-AH4, at the dispatcher rather than in miniature.** The unit cell
    // `AGrantedIntentIsWhatAParentDeleteWouldHaveToMeet` puts an intent in
    // the table by hand; this one has a real statement put it there, parked
    // mid-flight, and asks what a parent `DELETE` does while it is held.
    //
    // The ordering is the assertion. `CheckNoChildrenBeforeDelete` consults
    // `FkIntentTable` **before** the per-child loop, so the busy answer wins
    // over §3a's "cannot see its rows" - and it must, because they say
    // different things: the intent refusal is *retryable* (a transaction is
    // in flight and the answer depends on how it ends) where §3a's is
    // terminal (nothing built the fan-out). A client that got the terminal
    // one here would stop retrying a statement that was about to succeed.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "ah6_race");
    OpenCrossOwnerFkPair(rig, "race");
    ASSERT_NE(rig.dispatcher->Dispatch("INSERT INTO racep VALUES (7, 5)").response.rfind("ERR", 0),
              0u);

    DispatchOutcome out;
    auto statement = rig.StartOnPeer("INSERT INTO racec VALUES (7)", out);
    ASSERT_EQ(statement->Poll(), sched::PollResult::kSuspended) << out.response;
    rig.Pump();  // the probe crosses and is answered: the intent is live
    ASSERT_EQ(rig.fk_intents.live_rows(), 1u) << "no intent to race";

    const std::string busy =
        rig.dispatcher->Dispatch("DELETE FROM racep WHERE id = 7").response;
    EXPECT_EQ(busy.rfind("ERR ", 0), 0u) << busy;
    EXPECT_NE(busy.find("relied on by a foreign key check running on another core"),
              std::string::npos)
        << busy;
    EXPECT_NE(busy.find("retryable=1"), std::string::npos) << busy;
    // Not the reverse fan-out's refusal, which sits one step further on.
    EXPECT_EQ(busy.find("reverse probe needs the reactor path"), std::string::npos) << busy;

    // The child's statement finishes and its decide releases the intent -
    // and only then does the *other* refusal become the one a client sees.
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    ASSERT_NE(out.response.rfind("ERR", 0), 0u) << out.response;
    rig.Pump(8);
    EXPECT_EQ(rig.fk_intents.live_rows(), 0u);
    const std::string after =
        rig.dispatcher->Dispatch("DELETE FROM racep WHERE id = 7").response;
    // **Which refusal it hands over to changed at AJ-T3 and the ordering it
    // proves did not.** §3a's "cannot see its rows" is gone - the fan-out
    // replaced it - so what stands one step further on is the synchronous
    // path's inability to *park* on the fan-out's answers. Both are still
    // `TXN_CONFLICT`, so the point of the cell is now sharper rather than
    // weaker: the two retryable refusals must still be told apart, because
    // one clears when a transaction ends and the other when the client
    // moves to a served connection.
    EXPECT_NE(after.find("reverse probe needs the reactor path"), std::string::npos)
        << "the intent refusal did not hand over to the fan-out's: " << after;
    EXPECT_EQ(after.find("relied on by a foreign key check running on another core"),
              std::string::npos)
        << "the released intent was still being reported: " << after;
}


// **The finding this closes** (A5 of the post-SS5 verification order,
// `bench/v2.2.0/results-shipping-part-a-v2.2.0-11-g925f483.md` Finding 2):
// a shipped write to an assertion-covered peer-owned relation was *admitted
// and unenforced*, putting a second row in a group under
// `CHECK COUNT(*) <= 1`. It was disabled and failing until PW1c-6c.
//
// The fix is ownership, not a refusal: the owner builds the Bound Cabin
// from its own lease, adopts it into its own registry and enforces it, so
// the write is refused **by the assertion** rather than by a shape gate,
// and every legal write to that relation still lands. This test asserts
// all three - the build happened on core 1, the violating row was refused,
// and a non-violating row is admitted - because a version of this that
// refused everything would pass a test that only checked the refusal.
TEST_F(CoreRuntimeTest, ACreateAssertionOnAPeerRelationIsBuiltAndEnforcedByTheOwner) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_gate3");

    // Core 0's statement parks on the owner's build, exactly as the foreign
    // CREATE INDEX does. The rows it constrains (10, 20, 30) are the
    // owner's own - core 0 never faulted them - which is the reason the
    // scan has to run there.
    DispatchOutcome made;
    auto create = rig.Start(
        "CREATE ASSERTION cap ON shipped_gate3 GROUP BY (v) CHECK COUNT(*) <= 1", made);
    ASSERT_TRUE(rig.Drive(*create)) << made.response;
    ASSERT_EQ(made.response.rfind("ERR", 0), std::string::npos) << made.response;
    EXPECT_NE(made.response.find("built_by_core=1"), std::string::npos) << made.response;
    EXPECT_NE(made.response.find("rows=3"), std::string::npos) << made.response;
    EXPECT_NE(made.response.find("groups=3"), std::string::npos) << made.response;
    EXPECT_NE(made.response.find("enforcing=1"), std::string::npos) << made.response;
    EXPECT_EQ(rig.peer->assertion_builds()->builds(), 1u);
    // The directory lives on the core that will append to it, and nowhere
    // else: core 0 published the row and holds nothing.
    EXPECT_TRUE(rig.peer->dispatcher().assertions().AnyOn(rig.oid));
    EXPECT_TRUE(rig.dispatcher->assertions().empty())
        << "core 0 adopted a directory it can neither maintain nor write";

    // The write the finding measured: a second row in group 10, shipped to
    // the owner. Refused now - and by the assertion, whose message names
    // it, not by the shape gate.
    DispatchOutcome violating;
    auto second = rig.Start("INSERT INTO shipped_gate3 VALUES (10)", violating);
    ASSERT_TRUE(rig.Drive(*second)) << violating.response;
    EXPECT_EQ(violating.response.rfind("ERR ", 0), 0u)
        << "a shipped write to an assertion-covered relation was admitted: "
        << violating.response;
    EXPECT_NE(violating.response.find("ASSERTION_VIOLATION"), std::string::npos)
        << violating.response;
    EXPECT_EQ(RowsWith(*rig.peer, "shipped_gate3", ",10"), 1)
        << "the assertion was not enforced on the shipped path";

    // And the other half, which is what makes this a fix rather than a
    // wider refusal: a row in a group of its own still lands.
    DispatchOutcome legal;
    auto fourth = rig.Start("INSERT INTO shipped_gate3 VALUES (40)", legal);
    ASSERT_TRUE(rig.Drive(*fourth)) << legal.response;
    EXPECT_EQ(legal.response.rfind("ERR ", 0), std::string::npos) << legal.response;
    EXPECT_EQ(RowsWith(*rig.peer, "shipped_gate3", ",40"), 1) << legal.response;
}

TEST_F(CoreRuntimeTest, AnOwnerBuiltAssertionIsEnforcingAgainAfterTheOwnersRestart) {
    // The other half of building on the owner: the cabin, its `ASSERT_BUILD`
    // records and AS6a's base are all in **the owner's** stream, so it is
    // the owner's own mount that folds them back - which is why PW3's peer
    // checkpoint had to be carrying assertion snapshots before this was
    // sound, and why `ResumeAssertionsAfterRecovery` now runs on every core.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "assert_restart");

    DispatchOutcome made;
    auto create = rig.Start(
        "CREATE ASSERTION cap ON assert_restart GROUP BY (v) CHECK COUNT(*) <= 1", made);
    ASSERT_TRUE(rig.Drive(*create)) << made.response;
    ASSERT_EQ(made.response.rfind("ERR", 0), std::string::npos) << made.response;
    // The row is core 0's and unlogged in this rig, so it reaches the device
    // only by a flush - which is what the owner's remount will read.
    FlushCatalog();
    ASSERT_TRUE(core0_store_->Sync().ok());
    // **And the owner's own pages, which under one stream nothing else puts
    // there** (AM-S0). The revive below walks the cabin chain and claims
    // each page from its stream stamp, and a stamp is read off the
    // *device* - so the pages the owner built have to have reached it.
    // Under per-core streams the restarted peer replayed its own log and
    // re-faulted them on the way past; under one stream a peer runs no
    // recovery of its own (core 0's pass is the instance's), so what stands
    // in for that replay is the flush a clean stop performs - `Serve`'s
    // tail, which is what this reset models.
    ASSERT_TRUE(rig.peer->store().Sync().ok());

    rig.peer.reset();
    CoreRuntime::Config config = ConfigFor(1);
    auto again = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(again.ok()) << again.status().message();

    // The two facts the resume rests on, pinned separately from its result
    // because each was a real obstacle. **One**: a peer can read the whole
    // declaration - the heap row from the low catalog range and the spilled
    // text from the var-heap extent the mount grants itself, without which
    // this scan is refused `may not fault page N`. **Two**: the cabin the
    // owner built in the *previous* run is writable in this one, and only
    // *after* the revive has walked the chain, because that walk is what
    // lets the store claim its own stamp (PW1c-7). Asking before it would
    // answer no, and the resume would refuse a relation it owns.
    {
        auto defs = exec::ListAssertions(again.value()->catalog(), again.value()->store());
        ASSERT_TRUE(defs.ok()) << defs.status().message();
        ASSERT_EQ(defs.value().size(), 1u);
        auto revived = exec::ReviveAssertion(again.value()->catalog(), again.value()->store(),
                                             defs.value().front());
        ASSERT_TRUE(revived.ok()) << "revive: " << revived.status().message();
        EXPECT_TRUE(again.value()->store().MayWrite(revived.value().chain.root()))
            << "the owner may not append to the cabin it built, root page "
            << revived.value().chain.root();
    }
    EXPECT_EQ(again.value()->recovery().assertions_enforcing, 1u);
    EXPECT_EQ(again.value()->recovery().assertions_unrecovered, 0u);
    EXPECT_TRUE(again.value()->dispatcher().assertions().AnyOn(rig.oid))
        << "the owner mounted without the directory its own writes maintain";

    // **With the recovered aggregate, not a zeroed directory**: group 10
    // holds one row and the bound is one, so a second is refused. A shell
    // with empty groups would admit it, and would report `enforcing=1`
    // while enforcing nothing.
    std::vector<parser::AstValue> row(1);
    row[0].type = parser::ValueType::kInt;
    row[0].int_val = 10;
    EXPECT_EQ(again.value()->dispatcher().assertions().AdmitInsert(rig.oid, row).code(),
              StatusCode::kAssertionViolation);
    row[0].int_val = 40;
    EXPECT_TRUE(again.value()->dispatcher().assertions().AdmitInsert(rig.oid, row).ok());
}

TEST_F(CoreRuntimeTest, AnAssertionTheOwnerCannotEnforceRefusesTheRelationsWritesByName) {
    // The file written **before** PW1c-6c: core 0 built a Bound Cabin for a
    // peer-owned relation, so its pages are core 0's and the owner may not
    // append to them. The mount records that through `NoteUnenforceable`
    // (mount_recovery.cpp); this drives the same seam directly, because
    // producing such a file needs an engine that no longer exists.
    //
    // The gate's whole point: **refused, not admitted**. An unenforceable
    // constraint costs the relation its writes on that core, which is the
    // fail-closed side and the one the finding's engine got wrong.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "legacy_assert");

    rig.peer->dispatcher().assertions().NoteUnenforceable(rig.oid, /*assertion_id=*/7);

    DispatchOutcome out;
    auto insert = rig.Start("INSERT INTO legacy_assert VALUES (40)", out);
    ASSERT_TRUE(rig.Drive(*insert)) << out.response;
    ASSERT_EQ(out.response.rfind("ERR ", 0), 0u) << out.response;
    EXPECT_NE(out.response.find("assertion this core cannot enforce"), std::string::npos)
        << out.response;
    EXPECT_EQ(RowsWith(*rig.peer, "legacy_assert", ",40"), 0) << out.response;

    // **And the repair, without a remount.** Dropping such an assertion is
    // what an operator does before re-creating it so the owner builds the
    // cabin; the drop reaches this registry as an eviction (the DROP arm's
    // `done(aborted)`), and the relation must take writes again at once. A
    // record that outlived its assertion would refuse them forever.
    rig.peer->dispatcher().assertions().Evict(/*assertion_id=*/7);
    DispatchOutcome after;
    auto retried = rig.Start("INSERT INTO legacy_assert VALUES (40)", after);
    ASSERT_TRUE(rig.Drive(*retried)) << after.response;
    EXPECT_EQ(after.response.rfind("ERR ", 0), std::string::npos) << after.response;
    EXPECT_EQ(RowsWith(*rig.peer, "legacy_assert", ",40"), 1) << after.response;
}

TEST_F(CoreRuntimeTest, ADropOfAPeerOwnedAssertionEvictsTheOwnersDirectory) {
    // The other side of ownership: the directory is the owner's, so a DROP
    // on core 0 that only retired the `sys.assertions` row would leave the
    // owner refusing writes for a constraint that no longer exists. The
    // drop arm sends the owner `done(aborted)`, which is "forget this id".
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "assert_dropped");

    DispatchOutcome made;
    auto create = rig.Start(
        "CREATE ASSERTION cap ON assert_dropped GROUP BY (v) CHECK COUNT(*) <= 1", made);
    ASSERT_TRUE(rig.Drive(*create)) << made.response;
    ASSERT_EQ(made.response.rfind("ERR", 0), std::string::npos) << made.response;
    ASSERT_TRUE(rig.peer->dispatcher().assertions().AnyOn(rig.oid));

    // Enforcing, so the second row in group 10 is refused.
    DispatchOutcome refused;
    auto blocked = rig.Start("INSERT INTO assert_dropped VALUES (10)", refused);
    ASSERT_TRUE(rig.Drive(*blocked)) << refused.response;
    ASSERT_EQ(refused.response.rfind("ERR ", 0), 0u) << refused.response;

    // The drop needs no park - the row is core 0's - so the synchronous
    // path carries it, and the message rides the ring behind it.
    const std::string dropped = rig.dispatcher->Dispatch("DROP ASSERTION cap").response;
    ASSERT_EQ(dropped.rfind("ERR", 0), std::string::npos) << dropped;
    rig.Pump(8);
    EXPECT_FALSE(rig.peer->dispatcher().assertions().AnyOn(rig.oid))
        << "the owner is still enforcing an assertion that was dropped";

    DispatchOutcome admitted;
    auto second = rig.Start("INSERT INTO assert_dropped VALUES (10)", admitted);
    ASSERT_TRUE(rig.Drive(*second)) << admitted.response;
    EXPECT_EQ(admitted.response.rfind("ERR ", 0), std::string::npos) << admitted.response;
    EXPECT_EQ(RowsWith(*rig.peer, "assert_dropped", ",10"), 2) << admitted.response;
}

TEST_F(CoreRuntimeTest, ARefusedForeignAssertionBuildLeavesTheOwnerEnforcingNothing) {
    // The owner's refusal, and what core 0 does with it: the data already
    // carries two rows in group 10, so `COUNT(*) <= 1` cannot be declared -
    // and the refusal has to come from the owner, because core 0 cannot see
    // either row. It reaches the client with its own code, no row is
    // published, and the owner adopted nothing, so the relation keeps
    // taking writes.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "assert_refused");

    // The second row in group 10, written by the owner before any
    // assertion exists.
    ASSERT_EQ(rig.peer->dispatcher()
                  .Dispatch("INSERT INTO assert_refused VALUES (10)")
                  .response.rfind("ERR", 0),
              std::string::npos);

    DispatchOutcome made;
    auto create = rig.Start(
        "CREATE ASSERTION toosmall ON assert_refused GROUP BY (v) CHECK COUNT(*) <= 1", made);
    ASSERT_TRUE(rig.Drive(*create)) << made.response;
    ASSERT_EQ(made.response.rfind("ERR ", 0), 0u) << made.response;
    EXPECT_NE(made.response.find("ASSERTION_VIOLATION"), std::string::npos) << made.response;
    EXPECT_FALSE(rig.peer->dispatcher().assertions().AnyOn(rig.oid))
        << "a refused build left a directory enforcing on the owner";

    // Nothing published: `SHOW ASSERTIONS` on core 0 knows of none.
    const std::string shown = rig.dispatcher->Dispatch("SHOW ASSERTIONS").response;
    EXPECT_NE(shown.find("assertions=0"), std::string::npos) << shown;

    // And the relation is writable, which a leftover window would have
    // denied - this protocol opens none.
    DispatchOutcome out;
    auto insert = rig.Start("INSERT INTO assert_refused VALUES (40)", out);
    ASSERT_TRUE(rig.Drive(*insert)) << out.response;
    EXPECT_EQ(out.response.rfind("ERR ", 0), std::string::npos) << out.response;
}

TEST_F(CoreRuntimeTest, AForeignAssertionBuildAbandonedByCore0IsEvictedOnTheOwner) {
    // `done(aborted)`: the owner built and adopted, and core 0 then failed
    // to publish. Reached here through the synchronous `Dispatch()`, which
    // is the path that cannot park - it abandons the statement and tells
    // the owner, so the reply arrives at a client with no waiter and the
    // no-waiter branch says `done(aborted)` itself.
    //
    // What must be true afterwards is that the owner enforces **nothing**:
    // a directory kept for a constraint no `sys.assertions` row names would
    // refuse writes forever for a statement that answered ERR.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "assert_abandon");

    const std::string refused =
        rig.dispatcher
            ->Dispatch("CREATE ASSERTION gone ON assert_abandon GROUP BY (v) CHECK COUNT(*) <= 1")
            .response;
    ASSERT_EQ(refused.rfind("ERR ", 0), 0u) << refused;
    EXPECT_NE(refused.find("needs the reactor path"), std::string::npos) << refused;

    // Both directions drained: the request reaches the owner, it builds and
    // adopts, its reply finds no waiter, and the `done(aborted)` that
    // follows evicts what it adopted.
    rig.Pump(16);
    EXPECT_EQ(rig.peer->assertion_builds()->builds(), 1u)
        << "the owner never saw the request this test is about";
    EXPECT_FALSE(rig.peer->dispatcher().assertions().AnyOn(rig.oid))
        << "the owner is enforcing a constraint core 0 abandoned";

    // The relation still takes writes, in the group the abandoned
    // assertion would have capped.
    DispatchOutcome out;
    auto insert = rig.Start("INSERT INTO assert_abandon VALUES (10)", out);
    ASSERT_TRUE(rig.Drive(*insert)) << out.response;
    EXPECT_EQ(out.response.rfind("ERR ", 0), std::string::npos) << out.response;
    EXPECT_EQ(RowsWith(*rig.peer, "assert_abandon", ",10"), 2) << out.response;
}

TEST_F(CoreRuntimeTest, AForeignAssertionBuildsInsideAnExplicitTransactionLikeTheLocalArm) {
    // The local arm's contract on a peer-owned relation (AK-S1): the
    // statement parks inside the transaction, the owner enforces before
    // COMMIT - against the transaction's own enrolled write too - and a
    // ROLLBACK undoes the transaction's rows and leaves the assertion where
    // a locally-declared one would be: assertions are non-transactional
    // DDL (`ddl-transactional.md` §5f).
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "assert_in_txn");
    // A core-0-owned relation, so the transaction has a row of its own for
    // the ROLLBACK to undo - which is what separates "the assertion
    // survived" from "nothing was undone". Not the target relation: a
    // build meeting the transaction's own in-flight row refuses (§5f).
    rig.catalog2->SetPlacementPolicy(catalog::PlacementPolicy::kCreatingCore);
    ASSERT_EQ(rig.dispatcher->Dispatch("CREATE TABLE c0_rows (id int64, v int64) BTREE")
                  .response.substr(0, 3),
              "CRE");
    rig.catalog2->SetPlacementPolicy(catalog::PlacementPolicy::kRotate);

    Session session;
    ASSERT_EQ(rig.dispatcher->Dispatch("BEGIN", &session).response.rfind("ERR", 0),
              std::string::npos);
    ASSERT_EQ(rig.dispatcher->Dispatch("INSERT INTO c0_rows VALUES (77)", &session)
                  .response.rfind("ERR", 0),
              std::string::npos);
    DispatchOutcome made;
    auto create = rig.Start(
        "CREATE ASSERTION intxn ON assert_in_txn GROUP BY (v) CHECK COUNT(*) <= 1", made,
        &session);
    ASSERT_TRUE(rig.Drive(*create)) << made.response;
    ASSERT_EQ(made.response.rfind("ERR", 0), std::string::npos) << made.response;
    EXPECT_NE(made.response.find("built_by_core=1"), std::string::npos) << made.response;
    EXPECT_EQ(rig.peer->assertion_builds()->builds(), 1u) << "the owner was not asked to build";
    EXPECT_TRUE(session.in_explicit_txn()) << "the park ended the client's transaction";
    EXPECT_TRUE(rig.peer->dispatcher().assertions().AnyOn(rig.oid))
        << "the owner is not enforcing before COMMIT";

    // Enforced before COMMIT, on both routes to the owner: an autocommit
    // write shipped whole, and this transaction's own write, which ships
    // and enrols the owner (R6-8) - the path the lifted refusal kept
    // unreachable.
    DispatchOutcome shipped;
    auto second = rig.Start("INSERT INTO assert_in_txn VALUES (10)", shipped);
    ASSERT_TRUE(rig.Drive(*second)) << shipped.response;
    EXPECT_NE(shipped.response.find("ASSERTION_VIOLATION"), std::string::npos)
        << shipped.response;
    DispatchOutcome enrolled;
    auto third = rig.Start("INSERT INTO assert_in_txn VALUES (10)", enrolled, &session);
    ASSERT_TRUE(rig.Drive(*third)) << enrolled.response;
    EXPECT_NE(enrolled.response.find("ASSERTION_VIOLATION"), std::string::npos)
        << enrolled.response;

    // ROLLBACK: the transaction's row goes, the assertion stays.
    DispatchOutcome rolled;
    auto rollback = rig.Start("ROLLBACK", rolled, &session);
    ASSERT_TRUE(rig.Drive(*rollback)) << rolled.response;
    EXPECT_FALSE(session.in_explicit_txn());
    EXPECT_EQ(rig.dispatcher->Dispatch("SELECT * FROM c0_rows").response.find(",77"),
              std::string::npos)
        << "the ROLLBACK left the transaction's own row";
    EXPECT_TRUE(rig.peer->dispatcher().assertions().AnyOn(rig.oid))
        << "the ROLLBACK took a non-transactional DDL with it";
    EXPECT_NE(rig.dispatcher->Dispatch("SHOW ASSERTIONS").response.find("name=intxn"),
              std::string::npos);
    DispatchOutcome again;
    auto fourth = rig.Start("INSERT INTO assert_in_txn VALUES (20)", again);
    ASSERT_TRUE(rig.Drive(*fourth)) << again.response;
    EXPECT_NE(again.response.find("ASSERTION_VIOLATION"), std::string::npos) << again.response;
}
TEST_F(CoreRuntimeTest, AStatementSpanningTwoOwnersIsNotShippedAndKeepsItsRefusal) {
    // R6's multi-owner statement: `SoleForeignOwner` refuses a chain whose
    // steps do not all belong to one foreign core, so the statement falls
    // through to the affinity refusal it always had. Shipping a statement
    // one owner cannot answer whole is the failure this prevents.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_span");

    // A second relation on core 0, so the join spans core 0 and the peer.
    rig.catalog2->SetPlacementPolicy(catalog::PlacementPolicy::kCreatingCore);
    auto local = rig.catalog2->CreateTable(catalog::kNamespacePublic, "span_local",
                                           TwoColumnSchema(), catalog::ClusteredType::kBtree);
    rig.catalog2->SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    ASSERT_TRUE(local.ok()) << local.status().message();
    auto row = rig.catalog2->GetSysTableRow(local.value());
    ASSERT_TRUE(row.ok());
    ASSERT_EQ(row.value().owner_core, 0u);
    ASSERT_TRUE(core0_store_->Sync().ok());

    DispatchOutcome out;
    auto statement = rig.Start(
        "SELECT span_local.v FROM span_local JOIN shipped_span ON "
        "span_local.v = shipped_span.v",
        out);
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    ASSERT_EQ(out.response.rfind("ERR ", 0), 0u) << out.response;
    // **The affinity refusal specifically**, not any error: a statement that
    // failed to compile would also answer `ERR` and would prove nothing
    // about the fork.
    EXPECT_NE(out.response.find("is owned by core"), std::string::npos) << out.response;
    // Nothing crossed: the owner ran nothing, and no waiter was opened.
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 0u) << out.response;
    EXPECT_EQ(rig.ship->shipped(), 0u) << out.response;
    EXPECT_EQ(rig.ship->waiting(), 0u);
}

TEST_F(CoreRuntimeTest, AnalyzeOfAPeerOwnedRelationIsNotShipped) {
    // The read fork runs on the *stripped* text (`ANALYZE` is a dispatcher
    // prefix, not a parser keyword), so shipping it would send a bare
    // `SELECT` and answer a request for a plan with a result set. Refused
    // exactly as it was before shipping existed.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "no_ship_analyze");

    DispatchOutcome out;
    auto statement = rig.Start("ANALYZE SELECT * FROM no_ship_analyze", out);
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    EXPECT_EQ(out.response.rfind("ERR ", 0), 0u) << out.response;
    EXPECT_NE(out.response.find("owned by core 1"), std::string::npos) << out.response;
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 0u);
    EXPECT_EQ(rig.ship->waiting(), 0u);
}

TEST_F(CoreRuntimeTest, AStatementWhoseSubqueryNamesASecondCoresRelationIsNotShipped) {
    // **A sub-chain reads real pages.** The compiler leaves a correlated
    // sub-chain - and every value-bearing uncorrelated one, `IN` included -
    // on the *step* that carries its outer column, not in `hoisted`, so a
    // walk over `hoisted` and `steps` alone does not see it. Shipping such
    // a chain sends it to the outer relation's owner, which then faults the
    // other core's pages: refused in a Debug build by the shared-nothing
    // check, and in a **release** build performed, judging visibility
    // against the wrong core's transaction manager. It answered an empty
    // result set where the row matched.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "sub_ship");

    // A relation core 0 owns. Rotation skips the system core, so the policy
    // is switched for this one relation and switched back.
    rig.catalog2->SetPlacementPolicy(catalog::PlacementPolicy::kCreatingCore);
    auto core0_oid =
        rig.catalog2->CreateTable(catalog::kNamespacePublic, "sub_core0", TwoColumnSchema(),
                                  catalog::ClusteredType::kBtree);
    rig.catalog2->SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    ASSERT_TRUE(core0_oid.ok()) << core0_oid.status().message();
    auto row = rig.catalog2->GetSysTableRow(core0_oid.value());
    ASSERT_TRUE(row.ok());
    ASSERT_EQ(row.value().owner_core, 0u);
    ASSERT_TRUE(core0_store_->Sync().ok());
    rig.peer->InvalidateCatalog();

    ASSERT_EQ(rig.dispatcher->Dispatch("INSERT INTO sub_core0 VALUES (10)").response.rfind("ERR",
                                                                                          0),
              std::string::npos);
    ASSERT_TRUE(core0_store_->Sync().ok());
    rig.peer->InvalidateCatalog();

    // The read: outer on the peer, sub-chain on core 0.
    DispatchOutcome out;
    auto statement =
        rig.Start("SELECT * FROM sub_ship WHERE v IN (SELECT v FROM sub_core0)", out);
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    EXPECT_EQ(out.response.rfind("ERR ", 0), 0u) << out.response;
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 0u) << out.response;

    // The write half: UPDATE and DELETE never compile a chain, so their
    // fork resolves the target relation's owner and nothing else. A
    // subquery predicate keeps the affinity refusal.
    DispatchOutcome upd;
    auto update =
        rig.Start("UPDATE sub_ship SET v = 7 WHERE v IN (SELECT v FROM sub_core0)", upd);
    ASSERT_TRUE(rig.Drive(*update)) << upd.response;
    EXPECT_EQ(upd.response.rfind("ERR ", 0), 0u) << upd.response;
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 0u) << upd.response;

    DispatchOutcome del;
    auto remove = rig.Start("DELETE FROM sub_ship WHERE v IN (SELECT v FROM sub_core0)", del);
    ASSERT_TRUE(rig.Drive(*remove)) << del.response;
    EXPECT_EQ(del.response.rfind("ERR ", 0), 0u) << del.response;
    EXPECT_EQ(rig.peer->shipped_statements()->executed(), 0u) << del.response;

    // And the local-outer form, which shipping never reaches: this is
    // `CheckReadAffinity` alone, which used to pass it and read the peer's
    // pages from core 0.
    const std::string local_outer =
        rig.dispatcher->Dispatch("SELECT * FROM sub_core0 WHERE v IN (SELECT v FROM sub_ship)")
            .response;
    EXPECT_EQ(local_outer.rfind("ERR ", 0), 0u) << local_outer;
    EXPECT_NE(local_outer.find("owned by core 1"), std::string::npos) << local_outer;
    EXPECT_EQ(rig.ship->waiting(), 0u);
}

TEST_F(CoreRuntimeTest, AnIndexBuildIsRefusedForAForeignRelationAndReleasedOnAbort) {
    // The owner's endings over raw payloads, with core 0 a bare scheduler
    // capturing replies - no client, since these are requests a client
    // never sends: a relation this peer does not own, counts past the
    // caps (refused on the wire by the owner, and before the wire by
    // core 0's encode), a build whose statement aborts (window closed, row
    // never written, tree orphaned, slot kept), a `done` for nothing, and
    // a window nobody closes.
    auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 256);
    ASSERT_TRUE(transport.ok()) << transport.status().message();
    sched::NullIoBackend io0;
    sched::Scheduler core0(clock_, io0);
    ASSERT_TRUE(core0.AttachTransport(&transport.value(), 0).ok());
    ASSERT_TRUE(core0
                    .RegisterMessageHandler(sched::RingMessageKind::kAnchorWrite,
                                            [](const sched::MessageHeader&,
                                               std::span<const std::byte>) {})
                    .ok());
    std::map<std::uint64_t, IndexBuildReplyPayload> replies;
    ASSERT_TRUE(core0
                    .RegisterMessageHandler(
                        sched::RingMessageKind::kIndexBuildReply,
                        [&replies](const sched::MessageHeader& header,
                                   std::span<const std::byte> payload) {
                            IndexBuildReplyPayload reply{};
                            ASSERT_EQ(payload.size(), sizeof(reply));
                            std::memcpy(&reply, payload.data(), sizeof(reply));
                            replies[header.request_id] = reply;
                        })
                    .ok());

    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                              /*core_count=*/2);
    catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "rotated_ix2", TwoColumnSchema(),
                                    catalog::ClusteredType::kBtree);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto row = catalog2.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    ASSERT_EQ(row.value().owner_core, 1u);
    auto other = core0_->catalog.CreateTable(catalog::kNamespacePublic, "core0s_ix",
                                             TwoColumnSchema(), catalog::ClusteredType::kBtree);
    ASSERT_TRUE(other.ok()) << other.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());

    CoreRuntime::Config config = ConfigFor(1);
    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    ASSERT_TRUE(peer.value()->AttachTransport(transport.value()).ok());
    peer.value()->GrantRelationFault(
        RelationFaultExtentOf(row.value(), storage::kDefaultExtentPages));
    const PageId pages[] = {row.value().desc_page_id, row.value().anchor_page_id};
    peer.value()->GrantRelationWrite(pages);

    const auto pump = [&] {
        for (int i = 0; i < 40; ++i) {
            peer.value()->scheduler().RunOnce();
            core0.RunOnce();
        }
    };
    const auto send_request = [&](std::uint64_t request_id,
                                  const IndexBuildRequestPayload& request) {
        sched::SubmitSendPod(core0, transport.value(), 0, 1, /*session_core=*/0, request_id,
                             sched::RingMessageKind::kIndexBuildRequest, request);
    };
    const auto send_done = [&](std::uint64_t index_oid, bool committed) {
        IndexBuildDonePayload done{};
        done.index_oid = index_oid;
        done.committed = committed ? 1 : 0;
        sched::SubmitSendPod(core0, transport.value(), 0, 1, /*session_core=*/0,
                             /*request_id=*/0, sched::RingMessageKind::kIndexBuildDone, done);
    };
    const auto prepare = [&](const char* sql) {
        parser::Parser parser(sql);
        auto parsed = parser.Parse();
        EXPECT_TRUE(parsed.ok()) << parsed.status().message();
        return exec::PrepareIndexDef(catalog2, std::get<parser::IndexStmt>(parsed.value()),
                                     nullptr, catalog::Catalog::AnchorSeed::kByOwner);
    };

    // A relation core 0 owns: refused, no window, no build.
    IndexBuildRequestPayload foreign{};
    foreign.table_oid = other.value();
    foreign.index_oid = 990;
    foreign.key_width = 8;
    foreign.entry_width = 16;
    foreign.nkeys = 1;
    foreign.key_cols[0] = 1;
    std::memcpy(foreign.name, "fx", 2);
    send_request(1, foreign);
    pump();
    ASSERT_TRUE(replies.count(1));
    EXPECT_EQ(replies[1].status_code, static_cast<std::uint32_t>(StatusCode::kNotImplemented))
        << replies[1].message;
    EXPECT_NE(std::string(replies[1].message).find("owned by core 0"), std::string::npos)
        << replies[1].message;
    EXPECT_TRUE(peer.value()->pending_index_builds().empty());
    EXPECT_EQ(peer.value()->index_builds()->builds(), 0u);

    // Counts past the caps: refused before either array is read.
    IndexBuildRequestPayload wide = foreign;
    wide.table_oid = oid.value();
    wide.nkeys = 5;
    send_request(2, wide);
    pump();
    ASSERT_TRUE(replies.count(2));
    EXPECT_EQ(replies[2].status_code, static_cast<std::uint32_t>(StatusCode::kInvalidArgument))
        << replies[2].message;
    EXPECT_TRUE(peer.value()->pending_index_builds().empty());

    // A good build whose statement then aborts.
    auto def = prepare("CREATE INDEX rix2 ON rotated_ix2 (v)");
    ASSERT_TRUE(def.ok()) << def.status().message();
    {
        // And the same over-cap shape refused by core 0's encode, never
        // truncated into a request the owner would check as something else.
        catalog::Catalog::IndexDef too_wide = def.value();
        too_wide.key_cols.assign(catalog::kMaxIndexKeyColumns + 1, 1);
        EXPECT_FALSE(IndexBuildRequestOf(too_wide).ok());
    }
    auto request = IndexBuildRequestOf(def.value());
    ASSERT_TRUE(request.ok()) << request.status().message();
    send_request(3, request.value());
    pump();
    ASSERT_TRUE(replies.count(3));
    ASSERT_EQ(replies[3].status_code, 0u) << replies[3].message;
    EXPECT_TRUE(peer.value()->pending_index_builds().Covers(oid.value()));
    send_done(def.value().index_oid, /*committed=*/false);
    pump();
    EXPECT_TRUE(peer.value()->pending_index_builds().empty());
    auto access = peer.value()->catalog().InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    EXPECT_TRUE(access.value()->indexes.empty()) << "an aborted build publishes nothing";
    {
        // The slot stays - PW2-3's named debt, one more occupant.
        auto anchor = peer.value()->store().GetForRead(row.value().anchor_page_id);
        ASSERT_TRUE(anchor.ok());
        EXPECT_TRUE(
            storage::AnchorIndexRoot(anchor.value().bytes(), def.value().index_oid).ok());
    }

    // A `done` naming no open window is ignored.
    send_done(12345, /*committed=*/true);
    pump();
    EXPECT_TRUE(peer.value()->pending_index_builds().empty());

    // A window nobody closes releases at the ceiling, not before.
    auto def2 = prepare("CREATE INDEX rix3 ON rotated_ix2 (v)");
    ASSERT_TRUE(def2.ok()) << def2.status().message();
    auto request2 = IndexBuildRequestOf(def2.value());
    ASSERT_TRUE(request2.ok()) << request2.status().message();
    send_request(4, request2.value());
    pump();
    ASSERT_TRUE(replies.count(4));
    ASSERT_EQ(replies[4].status_code, 0u) << replies[4].message;
    peer.value()->index_builds()->Expire(clock_.Now());
    EXPECT_TRUE(peer.value()->pending_index_builds().Covers(oid.value()))
        << "younger than the ceiling, the window stays";
    peer.value()->index_builds()->Expire(clock_.Now() + kIndexBuildPendingCeilingNs);
    EXPECT_TRUE(peer.value()->pending_index_builds().empty());
}

}  // namespace
}  // namespace kds::server
