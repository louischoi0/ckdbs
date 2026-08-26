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
#include "kds/exec/row_codec.hpp"
#include "kds/exec/index_ddl.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/index_build_service.hpp"
#include "kds/server/relation_grant_service.hpp"
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

        extents_.emplace(*core0_store_, kFirstUserPageId);
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

    // PW1c-6b-3's rig (defined below, after the schema helpers it needs):
    // a peer owning one populated relation, core 0 as a dispatcher over a
    // real ring, and the client the statement parks on.
    void OpenForeignIndexRig(struct ForeignIndexRig& rig, const char* table);

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
    // The property asserted is the end of that path, not the send: core 0's
    // superblock carries core 1's anchor. `SuperBlockCheckpointAnchor` is
    // the receiving half here exactly as it is in `Expeditor::Serve`.
    auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 64);
    ASSERT_TRUE(transport.ok()) << transport.status().message();

    sched::NullIoBackend io0;
    sched::Scheduler core0(clock_, io0);
    core0.AttachTransport(&transport.value(), 0);

    SuperBlockCheckpointAnchor receiver(core0_->superblock, *core0_store_);
    RegisterAnchorReceiver(core0, receiver);

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

TEST_F(CoreRuntimeTest, AMountAfterAPeersCleanStopDoesNotRereadTheRunsWholeLog) {
    // PW3b. Core 0 checkpoints at three points - the completion checkpoint
    // at mount, the cadence, and the way out - and PW3 gave a peer the first
    // two. Without the third a graceful restart replayed every peer's stream
    // from its last cadence tick (docs/known-gaps.md; the core-0 property is
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
        core0.AttachTransport(&transport.value(), 0);
        SuperBlockCheckpointAnchor receiver(core0_->superblock, *core0_store_);
        RegisterAnchorReceiver(core0, receiver);

        CoreRuntime::Config first_run = ConfigFor(1);
        first_run.next_trx_id = core0_->superblock.next_trx_id();
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
            EXPECT_EQ(receiver.publishes(), 1u);
        }
        const WalAnchorFields stop_anchor = core0_->superblock.wal_anchor(1);
        peer.value().reset();

        // The restart, with the anchor Expeditor copies out of the superblock.
        CoreRuntime::Config again = ConfigFor(1);
        again.anchor = stop_anchor;
        again.next_trx_id = core0_->superblock.next_trx_id();
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
    }
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

TEST_F(CoreRuntimeTest, APeersOwnPagesSurviveARestartByTheirStamp) {
    // PW1c-7 (workplan-peer-writer.md §8): a peer that wrote a relation
    // across several pages, then restarted, holds nothing in memory - a
    // fresh extent lease that covers none of its old pages, no fault grant,
    // no write grant. What it does hold is durable: every page it wrote
    // carries its stream stamp (PL §9 rule 4), the creation pages since the
    // acquisition restamp (rule 6). The store claims from the stamp on the
    // fault, so the relation reads whole and takes writes again with no
    // grant re-delivered at all. Two restarts: one after the pages were
    // flushed (the device path), one with the pages living only in the
    // log (redo's replay leaves them resident without rights, stamped as it
    // applied them - the resident-frame path).
    for (const bool flush_before_restart : {true, false}) {
        SCOPED_TRACE(flush_before_restart ? "pages flushed before the restart"
                                          : "pages living only in the log");
        catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth,
                                  /*core_count=*/2);
        catalog2.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
        const std::string name = flush_before_restart ? "survives_flushed" : "survives_logged";
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
        first_run.next_trx_id = core0_->superblock.next_trx_id();
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
        if (flush_before_restart) ASSERT_TRUE(peer.value()->store().Sync().ok());
        peer.value().reset();

        // The restart: a new lease, nothing granted, the ceiling core 0
        // would copy in.
        CoreRuntime::Config again = ConfigFor(1);
        again.next_trx_id = core0_->superblock.next_trx_id();
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
        // log - the second iteration's mount replays this stream too, which
        // is the never-written-page case the store now reads as NotFound.
        reopened.value().reset();
    }
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
    core0.AttachTransport(&transport.value(), 0);
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
    config.next_trx_id = core0_->superblock.next_trx_id();
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

TEST_F(CoreRuntimeTest, APeerListenerServesItsOwnRelationRefusesForeignAndRoutesStop) {
    // FINDING 5 of the PW5 review: nothing proved a peer listener serves
    // anything. This is the whole loop over a real socket - a rotated
    // relation is served on the peer that owns it, a core-0 relation is
    // refused with the affinity answer, and STOP does not stop this
    // reactor: it routes a kShutdown to the system core
    // (tcp_server.hpp's stop contract, CoreRuntime::ListenAndAttach).
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
    ASSERT_TRUE(peer.value()->ListenAndAttach(kPort).ok());

    std::thread worker([&] { peer.value()->Run(); });

    int fd = ConnectLoopback(kPort);
    ASSERT_GE(fd, 0);

    // Served: the relation this core owns (empty is fine; not an ERR).
    const std::string own = RoundTrip(fd, "SELECT * FROM rotated");
    EXPECT_EQ(own.rfind("ERR", 0), std::string::npos) << own;
    // Refused: a relation core 0 owns, with the affinity answer - the
    // peer has no session-side pipeline, so nothing ships from here.
    const std::string foreign = RoundTrip(fd, "SELECT * FROM local0");
    EXPECT_EQ(foreign.rfind("ERR", 0), 0u) << foreign;
    EXPECT_NE(foreign.find("owned by core 0"), std::string::npos) << foreign;
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

    // STOP: replied to, and routed - the kShutdown lands on core 0's ring
    // rather than stopping this reactor.
    (void)RoundTrip(fd, "STOP");
    ::close(fd);

    sched::MessageHeader header{};
    std::vector<std::byte> payload;
    bool routed = false;
    for (int spins = 0; spins < 5000 && !routed; ++spins) {
        while (transport.value().TryReceive(/*dst_core=*/0, header, payload)) {
            if (header.kind == static_cast<std::uint16_t>(sched::RingMessageKind::kShutdown)) {
                EXPECT_EQ(header.src_core, 1u);
                routed = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(routed) << "STOP on a peer never reached the system core";

    // What Serve's tail would then do: stop the peer over its ring.
    sched::MessageHeader stop{};
    stop.src_core = 0;
    stop.dst_core = 1;
    stop.session_core = 0;
    stop.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kShutdown);
    stop.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
    ASSERT_TRUE(transport.value().TrySend(stop, {}).ok());
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
    ASSERT_TRUE(peer.value()->ListenAndAttach(kPort).ok());

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
    std::optional<CommandDispatcher> dispatcher;
    std::unique_ptr<CoreRuntime> peer;
    catalog::Oid oid = 0;
    catalog::SysTableRow row{};

    sched::RealRingTransport& ring() { return transport->value(); }

    // One turn of both reactors, the peer first: a message core 0 sent
    // last turn is handled before core 0 polls anything parked on it.
    void Pump(int rounds = 1) {
        for (int i = 0; i < rounds; ++i) {
            peer->scheduler().RunOnce();
            core0->RunOnce();
        }
    }
    // Core 0's statement as the coroutine its reactor would poll. Never
    // polled here: the tests poll it between turns, so they see it parked.
    std::unique_ptr<sched::CoroTask> Start(const char* sql, DispatchOutcome& out,
                                           Session* session = nullptr) {
        return sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                   dispatcher->DispatchAsync(sql, session, &out));
    }
    // Polls `statement` between turns until it finishes; false if it does
    // not within `max_rounds`.
    bool Drive(sched::CoroTask& statement, int max_rounds = 256) {
        for (int i = 0; i < max_rounds; ++i) {
            if (statement.Poll() == sched::PollResult::kDone) return true;
            Pump();
        }
        return false;
    }
};

void CoreRuntimeTest::OpenForeignIndexRig(ForeignIndexRig& rig, const char* table) {
    auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 256);
    ASSERT_TRUE(transport.ok()) << transport.status().message();
    rig.transport.emplace(std::move(transport));
    rig.core0.emplace(rig.clock, rig.io0);
    rig.core0->AttachTransport(&rig.ring(), 0);
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
    config.next_trx_id = core0_->superblock.next_trx_id();
    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    rig.peer = std::move(peer.value());
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
    rig.dispatcher.emplace(core0_->superblock, *rig.catalog2, *core0_store_, /*log=*/nullptr,
                           &rig.clock, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                           exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                           /*access_statistics=*/false, /*cabins=*/nullptr, &*rig.txns,
                           txn::IsolationLevel::kReadCommitted, /*core_id=*/0);
    rig.client.emplace(*rig.core0, rig.ring(), rig.clock);
    ASSERT_TRUE(rig.client->RegisterReplyReceiver().ok());
    rig.dispatcher->SetIndexBuilds(&*rig.client);

    // Rows the owner wrote and core 0 never saw - what the build must find.
    const std::string ins =
        rig.peer->dispatcher()
            .Dispatch("INSERT INTO " + std::string(table) + " VALUES (10), (20), (30)")
            .response;
    ASSERT_NE(ins.rfind("ERR", 0), 0u) << ins;
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
    core0.AttachTransport(&transport.value(), 0);
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
    config.next_trx_id = core0_->superblock.next_trx_id();
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
    EXPECT_EQ(replies[1].status_code, static_cast<std::uint32_t>(StatusCode::kUnsupported))
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
