#include "kds/server/core_runtime.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/send_retry.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/extent_lease.hpp"
#include "kds/storage/memory_page_device.hpp"

// One core's stack, and the shutdown protocol that stops it
// (docs/workplan-crosscore.md P2).
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

        auto boot = bootstrap::BootstrapDatabase(*core0_store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        core0_.emplace(std::move(boot.value()));

        // What `Expeditor::Open()` does before anything else exists, and it
        // is load-bearing here: a peer builds its view of *which pages
        // exist* by reading the free map off the device at Open(), so a
        // peer that starts before core 0 has flushed sees an empty database
        // and answers NotFound to everything.
        ASSERT_TRUE(core0_store_->Sync().ok());

        extents_.emplace(core0_store_->free_map_bytes(), kFirstUserPageId);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    CoreRuntime::Config ConfigFor(std::uint32_t core_id) {
        CoreRuntime::Config c;
        c.core_id = core_id;
        c.wal_dir = dir_.string();
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

    static inline int counter_ = 0;
    std::filesystem::path dir_;
    sched::SystemClock clock_;
    std::unique_ptr<storage::MemoryPageDevice> device_;
    std::unique_ptr<storage::DevicePageStore> core0_store_;
    std::optional<bootstrap::BootstrapResult> core0_;
    std::optional<storage::ExtentAllocator> extents_;
};

TEST_F(CoreRuntimeTest, EachCoreOpensItsOwnWalStream) {
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

TEST_F(CoreRuntimeTest, AShutdownMessageStopsTheReactorFromItsOwnThread) {
    // The reason shutdown is a message at all: `Scheduler::Stop()` writes a
    // plain bool owned by the reactor's thread, so core 0 may not call it.
    auto transport = sched::RealRingTransport::Create(2, 16, 64);
    ASSERT_TRUE(transport.ok());

    auto core = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(core.ok()) << core.status().message();
    ASSERT_TRUE(core.value()->AttachTransport(transport.value()).ok());

    std::thread worker([&] { core.value()->Run(); });

    sched::MessageHeader header{};
    header.src_core = 0;
    header.dst_core = 1;
    header.session_core = 0;
    header.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kShutdown);
    header.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
    ASSERT_TRUE(transport.value().TrySend(header, {}).ok());

    // If the message is not noticed the test hangs, which is the honest
    // failure for "the reactor never stopped" - a timeout here would only
    // convert a hang into a flake.
    worker.join();
    EXPECT_TRUE(core.value()->scheduler().stopped());
}

TEST_F(CoreRuntimeTest, ShutdownStopsOnlyTheCoreItIsAddressedTo) {
    auto transport = sched::RealRingTransport::Create(3, 16, 64);
    ASSERT_TRUE(transport.ok());

    // The survivor is checked by **liveness, not by reading its flag**.
    //
    // `Scheduler::stopped_` is a plain bool owned by its reactor's thread,
    // so reading it from here while that thread runs is a data race - the
    // very thing kShutdown exists to avoid, and something ThreadSanitizer
    // catches in a test that tries it. A running core is instead shown to be
    // running by making it do something observable.
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

    send(1, sched::RingMessageKind::kShutdown);
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

    send(2, sched::RingMessageKind::kShutdown);
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

    for (auto& core : cores) {
        sched::MessageHeader h{};
        h.src_core = 0;
        h.dst_core = core->core_id();
        h.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kShutdown);
        h.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
        ASSERT_TRUE(transport.value().TrySend(h, {}).ok());
    }

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
// `docs/keystoneid-invariant.md` K-M2's bump-ahead allocator is the same
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
    catalog2.SetRelationPublishHook(
        [&](catalog::Oid oid, std::uint32_t owner, PageId root, PageId varheap) {
            published = {oid, owner, root, varheap, published.calls + 1};
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
    EXPECT_EQ(dry.status().code(), StatusCode::kResourceExhausted);

    table.Grant(1000, 100, 3);
    EXPECT_EQ(table.Next(1000).value(), 100u);
    EXPECT_EQ(table.Next(1000).value(), 101u);
    // Relations do not share blocks: oid 2000's lease is its own.
    EXPECT_FALSE(table.Next(2000).ok());
    EXPECT_EQ(table.Next(1000).value(), 102u);
    EXPECT_EQ(table.Next(1000).status().code(), StatusCode::kResourceExhausted);

    // A contiguous grant extends; a disjoint one replaces and burns.
    table.Grant(1000, 103, 2);
    EXPECT_EQ(table.Next(1000).value(), 103u);
    table.Grant(1000, 500, 2);
    EXPECT_EQ(table.Next(1000).value(), 500u);
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
    EXPECT_EQ(dry.status().code(), StatusCode::kResourceExhausted);

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
              StatusCode::kResourceExhausted);

    // And the blocks stay disjoint: core 0's next single id sits past the
    // granted block, so a peer id can never collide with a core-0 id -
    // K1's issue-once contract across cores.
    auto next_on_core0 = core0_->catalog.AllocateRowId(oid.value());
    ASSERT_TRUE(next_on_core0.ok());
    EXPECT_GE(next_on_core0.value(), first.value() + 16);
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

}  // namespace
}  // namespace kds::server
