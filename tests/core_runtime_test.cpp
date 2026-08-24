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
#include "kds/exec/row_codec.hpp"
#include "kds/server/remote_step_service.hpp"
#include "kds/server/superblock_checkpoint_anchor.hpp"
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
                                           catalog::ClusteredType::kHeap,
                                           catalog::KeyMode::kAssigned);
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
                                 catalog::ClusteredType::kHeap, catalog::KeyMode::kAssigned)
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
                                           catalog::ClusteredType::kHeap,
                                           catalog::KeyMode::kAssigned);
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
                                    TwoColumnSchema(), catalog::ClusteredType::kHeap,
                                    catalog::KeyMode::kAssigned);
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
                                           catalog::ClusteredType::kHeap,
                                           catalog::KeyMode::kAssigned);
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

TEST_F(CoreRuntimeTest, APeerIssuesLeasedTransactionIdsWithoutWritingTheSuperblock) {
    // The row-id lease's twin, and the door PW1 opened
    // (`docs/workplan-peer-writer.md`): before it, a peer's TrxIdSequence
    // constructed spent and its persist callback refused, so a peer could
    // not begin a *single* transaction - every write died at its first id,
    // ahead of any page. Reads never noticed: a read view mints from
    // `peek()`, which issues nothing.
    // Core 0's ceiling travels in the config, the way its WAL anchor does:
    // a peer's own `SuperBlock` is default-constructed, so without this the
    // mount check downstream compares a recovered stream against 0.
    CoreRuntime::Config config = ConfigFor(1);
    config.next_trx_id = core0_->superblock.next_trx_id();
    ASSERT_GT(config.next_trx_id, 0u) << "a bootstrapped database should carry a ceiling";

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
    EXPECT_GE(block.value().first, config.next_trx_id);

    // And the windows stay disjoint: core 0's next id sits past the block it
    // granted, so a peer's transaction id can never collide with a core-0
    // one - invariant 12's writer identity across cores.
    auto next_on_core0 = core0_ids.Next();
    ASSERT_TRUE(next_on_core0.ok()) << next_on_core0.status().message();
    EXPECT_GE(next_on_core0.value(), block.value().first + block.value().count);
}

TEST_F(CoreRuntimeTest, APeersCheckpointAnchorReachesCoreZerosSuperblock) {
    // PW3. A peer cannot write page 0, so its completed checkpoint sends the
    // anchor and core 0 writes it (remote_checkpoint_anchor.hpp). Before
    // this, a peer had no checkpointer at all: it published nothing, its
    // anchor slot never advanced, and every later mount rescanned its whole
    // stream - free while a peer could not write, and not free since PW1.
    //
    // The property asserted is the end of that path, not the send: core 0's
    // superblock carries core 1's anchor. `SuperBlockCheckpointAnchor` is
    // the receiving half here exactly as it is in `Expeditor::Serve`.
    auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 64);
    ASSERT_TRUE(transport.ok()) << transport.status().message();

    sched::NullIoBackend io0;
    sched::Scheduler core0(clock_, io0);
    core0.AttachTransport(&transport.value(), 0);

    SuperBlockCheckpointAnchor receiver(core0_->superblock, *core0_store_);
    ASSERT_TRUE(core0
                    .RegisterMessageHandler(
                        sched::RingMessageKind::kAnchorWrite,
                        [&](const sched::MessageHeader&, std::span<const std::byte> payload) {
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

    ASSERT_EQ(core0_->superblock.wal_anchor(1).checkpoint_lsn, 0u)
        << "core 1 should have no anchor before it checkpoints";

    CoreRuntime::Config config = ConfigFor(1);
    config.next_trx_id = core0_->superblock.next_trx_id();
    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    // AttachTransport runs the completion checkpoint (RC08's half for a
    // peer) and queues the send on this core's own reactor.
    ASSERT_TRUE(peer.value()->AttachTransport(transport.value()).ok());

    for (int i = 0; i < 20; ++i) {
        peer.value()->scheduler().RunOnce();
        core0.RunOnce();
    }

    EXPECT_GT(core0_->superblock.wal_anchor(1).checkpoint_lsn, 0u)
        << "the peer's completion checkpoint never reached core 0's superblock";
    EXPECT_EQ(receiver.publishes(), 1u);

    // And a second checkpoint advances it rather than republishing the
    // first - the cadence's whole purpose.
    const std::uint64_t first = core0_->superblock.wal_anchor(1).checkpoint_lsn;
    ASSERT_TRUE(peer.value()->Checkpoint().ok());
    for (int i = 0; i < 20; ++i) {
        peer.value()->scheduler().RunOnce();
        core0.RunOnce();
    }
    EXPECT_GT(core0_->superblock.wal_anchor(1).checkpoint_lsn, first);
    EXPECT_EQ(receiver.publishes(), 2u);
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
                                           catalog::ClusteredType::kHeap,
                                           catalog::KeyMode::kAssigned);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 64);
    ASSERT_TRUE(transport.ok()) << transport.status().message();

    sched::NullIoBackend io0;
    sched::Scheduler core0(clock_, io0);
    core0.AttachTransport(&transport.value(), 0);
    ASSERT_TRUE(
        RegisterRowIdGrantHandler(core0, transport.value(), core0_->catalog, nullptr).ok());

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    ASSERT_TRUE(peer.value()->AttachTransport(transport.value()).ok());

    // Nothing has asked yet, so the table knows of no relation at all and the
    // tick has nothing to do.
    EXPECT_FALSE(peer.value()->row_id_leases().NeediestRelation().has_value());
    peer.value()->MaybeRefillRowIds();
    EXPECT_EQ(peer.value()->row_id_refill().requests, 0u)
        << "a peer asked for ids for a relation no statement had named";

    // The first allocation fails retryably **and records the demand** - the
    // half that did not exist before PW1b.
    auto dry = peer.value()->catalog().AllocateRowId(oid.value());
    ASSERT_FALSE(dry.ok());
    EXPECT_EQ(dry.status().code(), StatusCode::kResourceExhausted);
    ASSERT_TRUE(peer.value()->row_id_leases().NeediestRelation().has_value())
        << "the miss did not record which relation needs ids";
    EXPECT_EQ(*peer.value()->row_id_leases().NeediestRelation(), oid.value());

    // The tick answers it, and the retry the message promised now succeeds.
    peer.value()->MaybeRefillRowIds();
    for (int i = 0; i < 20; ++i) {
        peer.value()->scheduler().RunOnce();
        core0.RunOnce();
    }
    EXPECT_EQ(peer.value()->row_id_refill().requests, 1u);
    EXPECT_EQ(peer.value()->row_id_refill().grants, 1u);

    auto wet = peer.value()->catalog().AllocateRowId(oid.value());
    ASSERT_TRUE(wet.ok()) << wet.status().message();

    // And the relation stops being needy, so the tick does not ask again on
    // every cadence - PW1's defect, which had the same shape one lease over.
    EXPECT_FALSE(peer.value()->row_id_leases().NeediestRelation().has_value())
        << "a freshly granted relation still reads as low water";
    peer.value()->MaybeRefillRowIds();
    EXPECT_EQ(peer.value()->row_id_refill().requests, 1u)
        << "the tick asked again for a relation that had just been granted a block";

    // The ids are core 0's to give, and disjoint from what core 0 issues.
    auto on_core0 = core0_->catalog.AllocateRowId(oid.value());
    ASSERT_TRUE(on_core0.ok()) << on_core0.status().message();
    EXPECT_GE(on_core0.value(), wet.value() + kRowIdLeasePerGrant)
        << "core 0's next id sits inside the block it granted the peer";
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
                                           catalog::ClusteredType::kHeap,
                                           catalog::KeyMode::kAssigned);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_GT(oid.value(), 3000u);  // the ungrantable oid below must sort first
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 64);
    ASSERT_TRUE(transport.ok()) << transport.status().message();

    sched::NullIoBackend io0;
    sched::Scheduler core0(clock_, io0);
    core0.AttachTransport(&transport.value(), 0);
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
    EXPECT_EQ(peer.value()->row_id_refill().requests, 1u);
    EXPECT_EQ(peer.value()->row_id_refill().grants, 0u) << "core 0 granted a relation it has no row for";
    ASSERT_TRUE(peer.value()->row_id_leases().NeediestRelation().has_value());
    EXPECT_EQ(*peer.value()->row_id_leases().NeediestRelation(), oid.value())
        << "a relation core 0 refused still counts as demand, so the tick never reaches another";

    // The next tick reaches the real relation, and the one after that asks
    // for nothing at all.
    turn();
    EXPECT_EQ(peer.value()->row_id_refill().requests, 2u);
    EXPECT_EQ(peer.value()->row_id_refill().grants, 1u);
    EXPECT_TRUE(peer.value()->catalog().AllocateRowId(oid.value()).ok());
    turn();
    EXPECT_EQ(peer.value()->row_id_refill().requests, 2u)
        << "the tick asked again for a relation core 0 had already refused";

    // And the refusal is not permanent to a *statement*: a fresh miss is
    // fresh demand, so the retry the message promises is one that is made.
    EXPECT_FALSE(peer.value()->row_id_leases().Next(3000).ok());
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
                                    catalog::ClusteredType::kHeap, catalog::KeyMode::kAssigned);
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
        [&](std::uint32_t, sched::RingMessageKind kind, std::vector<std::byte> payload) {
            switch (kind) {
                case sched::RingMessageKind::kStepBatch: client->OnStepBatch(payload); break;
                case sched::RingMessageKind::kStepEof: client->OnStepEof(payload); break;
                case sched::RingMessageKind::kStepError: client->OnStepError(payload); break;
                default: ADD_FAILURE() << "unexpected server send";
            }
            return Status::OK();
        });
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
                                          catalog::ClusteredType::kHeap,
                                          catalog::KeyMode::kAssigned);
    ASSERT_TRUE(outer_oid.ok()) << outer_oid.status().message();
    auto inner_oid = catalog2.CreateTable(catalog::kNamespacePublic, "tb", make_schema("qty"),
                                          catalog::ClusteredType::kHeap,
                                          catalog::KeyMode::kAssigned);
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
    server.emplace(catalog2, *core0_store_, /*core_id=*/1, deliver, nullptr,
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
                                          catalog::ClusteredType::kHeap,
                                          catalog::KeyMode::kAssigned);
    ASSERT_TRUE(outer_oid.ok()) << outer_oid.status().message();
    auto inner_oid = catalog2.CreateTable(catalog::kNamespacePublic, "tb", make_schema("qty"),
                                          catalog::ClusteredType::kHeap,
                                          catalog::KeyMode::kAssigned);
    ASSERT_TRUE(inner_oid.ok()) << inner_oid.status().message();
    // A third relation whose *non-pk* column overlaps `ta.b_id`, so a join
    // on it matches real rows. Without the overlap the non-pk cases below
    // would compare two empty answers and prove nothing.
    auto tag_oid = catalog2.CreateTable(catalog::kNamespacePublic, "tc", make_schema("tag"),
                                        catalog::ClusteredType::kHeap,
                                        catalog::KeyMode::kAssigned);
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
    server.emplace(catalog2, *core0_store_, /*core_id=*/1, deliver, nullptr,
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
    // (docs/spec-join-inner-build.md): locally the inner step builds a map
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
    // docs/known-gaps.md's closed entry named its own blind spot: "no
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
        "CREATE PATTERN p4 ON pw4 (v)",
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
