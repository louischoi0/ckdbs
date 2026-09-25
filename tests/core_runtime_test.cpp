#include "kds/base/current_core.hpp"
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
#include "kds/server/superblock_checkpoint_anchor.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/task.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/wal/log_scanner.hpp"
#include "kds/wal/payload.hpp"

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
    // **Which volume this fixture bootstraps** (AM-S0). `kSingleStream`, and
    // since AW-S1b there is no other: a `LogTopology()` hook stood here so
    // a derived fixture could ask for `kPerCoreStreams` over a volume that
    // genuinely said so, which was the only honest way to keep the legacy
    // cells. D14 made that volume unmountable and `BootstrapDatabase`
    // unable to write one, so the hook had nothing left to vary.

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
                                                 /*log=*/nullptr);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        core0_.emplace(std::move(boot.value()));
        core0_->catalog.SetSchemaWord(&schema_word_);
        core0_->catalog.SetOidSequence(&oid_sequence_);
        core0_->catalog.SetMarkCounter(&pending_marks_);

        ASSERT_TRUE(core0_store_->Sync().ok());

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
        // Cells that genuinely tested the **legacy** topology — a peer
        // opening its own `wal-<core>-*`, running its own recovery,
        // publishing its own anchor — stood below this fixture until
        // AW-S1b, and the branch they covered was reachable then. It is not
        // now: D14 refuses any image that is not version 17 and nothing can
        // write a per-core-stream one, so the skip that guarded this
        // attachment has no arm to guard against.
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
        // **The instance's read view** (AN-R1, AN-S3): one for the fixture,
        // shared with core 0's manager where a rig builds one, as
        // `Expeditor` shares it with every core. Until AN-S3 each side
        // owned a private one, which no instance can be in - a peer's view
        // could not see a core-0 commit at all and nothing noticed, because
        // no cell read the other core's rows locally. What made it
        // load-bearing is the coordinator's snapshot crossing the ring: a
        // participant that adopted an LSN from another instance's order
        // would read nonsense.
        c.visibility = &visibility_;
        c.schema_word = &schema_word_;
        c.oid_sequence = &oid_sequence_;
        c.mark_counter = &pending_marks_;
        // What `Expeditor` hands a peer on a single-stream volume, and what
        // `CoreRuntime::Open` refuses to proceed without (AM-S0). Null on
        // the per-core arm, where a peer opens its own log.
        if (core0_wal_ != nullptr) {
            c.shared_stream = core0_wal_->stream();
            c.shared_writer = core0_wal_->writer();
        }
        // **And the instance's pool** (AM-S2 step 3, AW-S1b). This fixture
        // used to give every peer a store of its own over the same device,
        // which was the pre-step-3 arrangement: a private copy of the free
        // map, read at that peer's Open and stale from the next thing core
        // 0 allocated. The lease, the fault grants and
        // `AdoptDeviceMapOnMiss` existed to paper over exactly that, and
        // when AW-S1b removed them the fixture was the only place in the
        // tree still in the arrangement they served. `Expeditor` shares the
        // pool on every single-stream volume, which is every volume this
        // build can mount.
        c.shared_store = core0_store_.get();
        // The instance's transaction-id ceiling (AT-S10b): core 0's image,
        // in memory only - a fixture's cells carve and never restart past
        // what the image already carries.
        c.trx_id_ceiling = {&core0_->superblock, &superblock_latch_, nullptr};
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

    // Asserts the peer may write a relation created after the rig opened
    // (AI-T2). It funded one - fault extent, write grants, a row-id block -
    // until each funding piece retired (the grants at AW-S1b, the row-id
    // lease at AT-S10b); what is left is the check that none is needed.
    void FundPeerForRelation(struct ForeignIndexRig& rig, catalog::Oid oid);

    // PW1c-7's restart, shared by two cells because only *half* of it is
    // about the log topology (AM-S0). `flush_before_restart` selects which
    // half: with the pages on the device the claim is read off the platter,
    // which is true whatever the volume's log says; with them only in the
    // log it is redo's replay that leaves them resident and stamped, and a
    // peer runs redo only under per-core streams.
    void PeerPagesSurviveARestart(bool flush_before_restart, const std::string& name);

    // A foreign key pair, funded for the peer: `<base>p` and `<base>c`, the
    // child referencing the parent. Until AT-S9 a placement policy put the
    // parent on core 0 and the child on the peer (AH-T6); no core owns a
    // relation since, and the peer is funded for the child it writes.
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
    // The instance read view every core of this fixture shares (AN-S3;
    // `ConfigFor` says why one and not one per side).
    txn::InstanceVisibility visibility_;
    // **The instance's schema version word** (AT-S2), one for the fixture:
    // core 0's catalog bumps it and every peer's revalidates against it,
    // which is what replaced the broadcast the rigs here used to send.
    std::atomic<std::uint64_t> schema_word_{0};
    std::atomic<catalog::Oid> oid_sequence_{0};  // AT-S5b
    std::atomic<std::uint64_t> pending_marks_{0};  // AT-S5b
    Latch superblock_latch_;  // AT-S10b: the one ceiling every core carves from
};

TEST_F(CoreRuntimeTest, AnotherThreadStopsTheReactorThroughTheAtomicFlag) {
    // **Renamed with the mechanism it tests** (AU-S3). It was
    // `AShutdownMessageStopsTheReactorFromItsOwnThread`, and both halves of
    // that name are now false: there is no message, and the point is that
    // the stop comes from a thread that is *not* the reactor's.
    //
    // `Scheduler::Stop()` wrote a plain bool, so only the reactor's own
    // thread could flip it - which is the whole reason `kShutdown` existed.
    // The flag is atomic now and the message is gone.
    auto core = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(core.ok()) << core.status().message();

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
    // The survivor is checked by **liveness, not by reading its flag**, and
    // the reason changed at AU-S3 without the practice changing. Reading
    // `stopped()` from here is no longer a data race - the flag is atomic -
    // but it was never the assertion worth making: `false` says only that
    // nobody asked this core to stop, where the claim is that it is still
    // serving. A running core is shown to be running by making it do
    // something observable: here a periodic timer on core 2, armed before
    // its worker exists, that counts its own firings. It was a ring message
    // to core 2 until AT-S10d retired the transport.
    std::atomic<int> turns{0};

    std::vector<std::unique_ptr<CoreRuntime>> cores;
    for (std::uint32_t id = 1; id < 3; ++id) {
        auto core = CoreRuntime::Open(ConfigFor(id), *device_, clock_, nullptr);
        ASSERT_TRUE(core.ok()) << core.status().message();
        cores.push_back(std::move(core.value()));
    }
    cores[1]->scheduler().SubmitEvery(/*period_ns=*/1'000'000, [&turns] {
        turns.fetch_add(1, std::memory_order_relaxed);
    });

    std::vector<std::thread> workers;
    for (auto& core : cores) workers.emplace_back([&core] { core->Run(); });

    cores[0]->scheduler().Stop();  // AU-S3: the flag is atomic; no message
    workers[0].join();
    // Safe here and only here: the join is what makes core 1's writes
    // visible to this thread.
    EXPECT_TRUE(cores[0]->scheduler().stopped());

    // Core 2 is still serving - which is a stronger statement than "its flag
    // is false", and one this thread is allowed to make: its timer keeps
    // firing after core 1 is gone - twice, because a reactor stopped by
    // mistake could still fire one coalesced tick on its way out.
    const int after_stop = turns.load(std::memory_order_relaxed);
    for (int i = 0; i < 1000 && turns.load(std::memory_order_relaxed) <= after_stop + 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GT(turns.load(std::memory_order_relaxed), after_stop + 1)
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
    std::vector<std::unique_ptr<CoreRuntime>> cores;
    for (std::uint32_t id = 1; id < kCores; ++id) {
        auto core = CoreRuntime::Open(ConfigFor(id), *device_, clock_, nullptr);
        ASSERT_TRUE(core.ok()) << core.status().message();
        // The reactor is this core with no wake registry attached: its
        // `RunOnce` sets the thread's `CurrentCore()`, so a reactor left at
        // 0 would run every peer as core 0.
        EXPECT_EQ(core.value()->scheduler().core_id(), id);
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

TEST_F(CoreRuntimeTest, APeerStatementSeesADdlThroughItsOwnBoundary) {
    // The boundary the engine actually has: `DispatchAndStage`'s head asks
    // the schema word before a statement resolves anything (AT-S2). What
    // the word guards is a cached fact that *changed* - a new relation is
    // never in anyone's memo, so its first resolve is fresh whatever the
    // word says. So: a peer statement fills `early`'s entry, core 0 renames
    // a column of it, and the peer's next *statement* - not a hand-driven
    // read - describes the new name. `DESCRIBE`, because it is a diagnostic
    // read every core serves through `InitTableAccess`. This is the cell a
    // mutant removing that head's `Revalidate()` kills.
    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "early", TwoColumnSchema(),
                                           catalog::ClusteredType::kBtree);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    Session s;
    const std::string first = peer.value()->dispatcher().Dispatch("DESCRIBE early", &s).response;
    ASSERT_NE(first.rfind("ERR", 0), 0u) << first;
    ASSERT_NE(first.find("v"), std::string::npos) << first;

    ASSERT_TRUE(core0_->catalog.RenameColumn(oid.value(), "v", "renamed").ok());
    const std::string second = peer.value()->dispatcher().Dispatch("DESCRIBE early", &s).response;
    ASSERT_NE(second.rfind("ERR", 0), 0u) << second;
    EXPECT_NE(second.find("renamed"), std::string::npos)
        << "the peer's statement described a column name a DDL had renamed: " << second;
}

TEST_F(CoreRuntimeTest, APeerSeesADdlAtItsNextCachedReadWithNothingSent) {
    // AT-S2's cell, and `APeerSeesADdlThatWasNotFlushedBecauseItReadsTheSameFrame`
    // before it: one frame table serves every core, so the bytes are there
    // with no flush; what stood between a peer and a row was its own
    // *memo*, dropped by a broadcast. The broadcast is gone. Core 0's DDL
    // bumps the instance's schema word and the peer's next cached read
    // compares it - so the cache fills, the DDL happens, and the very next
    // lookup sees the row with nothing sent and nothing called.
    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    // Fill the peer's memo first: a cached `TableAccess` for a relation that
    // exists, and a cached "no such name" is not a thing - so the fact
    // that goes stale is the table list.
    ASSERT_TRUE(core0_->catalog
                    .CreateTable(catalog::kNamespacePublic, "early", TwoColumnSchema(),
                                 catalog::ClusteredType::kHeap)
                    .ok());
    auto listed = peer.value()->catalog().ListTables();
    ASSERT_TRUE(listed.ok()) << listed.status().message();
    const std::size_t before = listed.value().size();

    ASSERT_TRUE(core0_->catalog
                    .CreateTable(catalog::kNamespacePublic, "late", TwoColumnSchema(),
                                 catalog::ClusteredType::kHeap)
                    .ok());

    // The peer's next boundary - what `DispatchAndStage`'s head does for
    // a statement, and what this cell does by hand because it reads the
    // catalog directly. Nothing sent, nothing dropped by anyone else.
    peer.value()->catalog().Revalidate();
    auto found = peer.value()->catalog().FindTableOidByName("late");
    EXPECT_TRUE(found.ok()) << found.status().message();
    auto relisted = peer.value()->catalog().ListTables();
    ASSERT_TRUE(relisted.ok());
    EXPECT_EQ(relisted.value().size(), before + 1)
        << "the peer's cached table list survived a DDL it was never told about";
}

// **The free-map refresh cell went with the refresh** (AW-S1b). It
// pinned `InvalidateCatalog`'s adoption of a bit core 0 had set after a
// peer's own snapshot of the map was taken - the defect that made 58
// shipped `CREATE INDEX`es turn every later write to that relation into
// a permanent `page id not found`
// (`bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md` §8d).
// There is one copy of the map now and core 0 sets the bit in it, so
// the snapshot the refresh reconciled does not exist. The cell that
// still says something about this path is
// `APeerResolvesARelationWhoseCatalogRowsSpilledOntoAnOverflowPage`,
// which walks the grown chain end to end.

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

    // The pages are unlogged, so the flush is what puts them on the
    // device; the peer's memo revalidates by itself (AT-S2).
    ASSERT_TRUE(core0_store_->FlushPages(catalog::kEveryCatalogPage).ok());

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

    // **The boundary and the asker, both explicit since AW-S1b.** The
    // boundary because a shared store is given it by `Expeditor::Open` and
    // this fixture is not the Expeditor; the asker because `MayWrite`
    // answers from `CurrentCore()` now rather than from "does this store
    // carry a lease" - so a question put from the test thread is core 0's
    // question, whichever runtime's store it names. In an instance the
    // asker is always the reactor running the statement.
    peer.value()->store().SetResidentLimit(kFirstUserPageId);
    {
        const CurrentCoreGuard as_the_peer(1);
        // Both admitted since AT-S5: the arm that refused a peer the
        // system range is gone, and the page latch serialises the bytes.
        EXPECT_TRUE(peer.value()->store().MayWrite(catalog::kCatalogPageTables));
        EXPECT_TRUE(peer.value()->store().MayWrite(kSuperBlockPageId));

        // Above the boundary it writes freely: which core may write a user
        // page is the Expeditor's routing decision, not this predicate's
        // (AM-R2, AO-R14).
        auto own = peer.value()->store().CreateNew();
        ASSERT_TRUE(own.ok()) << own.status().message();
        EXPECT_TRUE(peer.value()->store().MayWrite(own.value().first));
    }
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
// A second blocker was that **a peer could not INSERT**, because
// `Catalog::AllocateRowId()` bumps `next_id` on the sys.tables page, which
// a peer could not write; P5 leased it a range of row ids. Every core
// writes the catalog pages since AT-S5 and bumps the mark itself since
// AT-S10b.

// ---- Row ids and transaction ids on a peer (AT-S10b) -------------------

// `RowIdLeaseTableTest.IssuesFromAGrantAndExhaustsRetryably` stood here until AT-S10b: it pinned a peer's per-relation row-id lease; the leases were retired at AT-S10b: a peer carves its transaction-id window from the instance's ceiling and bumps a relation's row-id mark in place, so nothing is granted to it and nothing is refilled.

// `RowIdLeaseTableTest.ADeniedRelationAnswersOnceWithoutTheBitThenAsksAgain` stood here until AT-S10b: it pinned a denied lease's one unretryable answer; the leases were retired at AT-S10b: a peer carves its transaction-id window from the instance's ceiling and bumps a relation's row-id mark in place, so nothing is granted to it and nothing is refilled.

TEST_F(CoreRuntimeTest, APeerIssuesRowIdsFromTheRelationsOwnMarkInIssueOrder) {
    // AT-S10b: a peer's `AllocateRowId` bumps the relation's `sys.tables`
    // mark in place, as core 0's always has, so ids issued by two cores
    // into one relation are **one sequence in issue order** - invariant 11
    // with spreading off. The lease this replaced handed a peer a block
    // ahead of core 0's mark, so core 0's next id sat above the peer's
    // later ones.
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "t", TwoColumnSchema(),
                                           catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok());
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    // Nothing granted, and the first ask answers.
    std::uint64_t previous = 0;
    for (int i = 0; i < 8; ++i) {
        catalog::Catalog& issuer = i % 2 == 0 ? peer.value()->catalog() : core0_->catalog;
        auto id = issuer.AllocateRowId(oid.value());
        ASSERT_TRUE(id.ok()) << "issue " << i << ": " << id.status().message();
        if (i > 0) {
            EXPECT_EQ(id.value(), previous + 1)
                << "issue " << i << " is not the next id after the other core's";
        }
        previous = id.value();
    }
}

TEST_F(CoreRuntimeTest, APeerCarvesItsTransactionIdsFromTheInstancesCeiling) {
    // AT-S10b: a peer's sequence carves its window from the one superblock
    // core 0's does (`Config::trx_id_ceiling`), so a peer begins a
    // transaction with nothing granted to it, and the two windows are
    // disjoint - invariant 12's writer identity across cores. Before it, a
    // peer began nothing until core 0 had carved and leased it a block.
    CoreRuntime::Config config = ConfigFor(1);
    const std::uint64_t ceiling_before = core0_->superblock.next_trx_id();
    ASSERT_GT(ceiling_before, 0u) << "a bootstrapped database should carry a ceiling";
    // Core 0's sequence, built before the peer carves: its window opens at
    // the ceiling as it stands now, so its first id is a carve that must
    // find the peer's raise - a peer carving from a copy would leave core 0
    // issuing the peer's own ids.
    txn::TrxIdSequence core0_ids(core0_->superblock);

    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    const auto began = peer.value()->dispatcher().Dispatch("BEGIN").response;
    EXPECT_NE(began.rfind("ERR", 0), 0u) << "a peer could not begin on its own: " << began;
    (void)peer.value()->dispatcher().Dispatch("ROLLBACK");

    // The peer's carve raised the instance's ceiling, from where it stood.
    const txn::TrxIdSequence& peer_ids = peer.value()->trx_ids();
    EXPECT_GE(peer_ids.peek(), ceiling_before);
    EXPECT_EQ(core0_->superblock.next_trx_id(), peer_ids.ceiling())
        << "the peer carved from a copy rather than from the instance's superblock";

    // And core 0's next window sits past it.
    auto next_on_core0 = core0_ids.Next();
    ASSERT_TRUE(next_on_core0.ok()) << next_on_core0.status().message();
    EXPECT_GE(next_on_core0.value(), peer_ids.ceiling());
}

TEST_F(CoreRuntimeTest, APeersDispatcherRunsUnderTheStatementLimitsItIsHanded) {
    // AT-S8: every core accepts sessions, so a peer's dispatcher must refuse
    // what core 0's refuses under the same config file. It was built with
    // the dispatcher's own defaults and never handed `sort_max_rows` and its
    // siblings, so a peer-accepted session could sort a million rows where
    // the operator had capped it at three.
    CoreRuntime::Config config = ConfigFor(1);
    config.statement_limits.sort_max_rows = 1;
    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    CommandDispatcher& d = peer.value()->dispatcher();
    for (const char* sql : {"CREATE TABLE capped (id int64, v int64)",
                            "INSERT INTO capped VALUES (1, 20)", "INSERT INTO capped VALUES (2, 10)"}) {
        const std::string r = d.Dispatch(sql).response;
        ASSERT_NE(r.rfind("ERR", 0), 0u) << sql << ": " << r;
    }

    const std::string reply = d.Dispatch("SELECT v FROM capped ORDER BY v").response;
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << "the peer sorted past the cap it was handed: " << reply;
    EXPECT_NE(reply.find("sort_max_rows"), std::string::npos) << reply;
}

TEST_F(CoreRuntimeTest, APeerHandedTheOptimizerSurfaceSetsTheInstancesSwitchAndSeesItsController) {
    // AT-S8, on the operator's word: every core accepts sessions, so a peer
    // must reach the one optimizer. Handed nothing, `SET CABIN_OPTIMIZER ON`
    // on a peer answered OK and flipped a flag of its own that nothing read,
    // and `SHOW CABIN_OPTIMIZER` answered "absent". **Mutation**: drop
    // `set_optimizer_surface` from `CoreRuntime::Open` and both halves fail.
    std::atomic<bool> instance_switch{false};
    stats::CabinOptimizer controller;
    Latch view_latch;
    OptimizerSurface surface;
    surface.cabin_optimizer_on = &instance_switch;
    surface.controller = &controller;
    surface.view_latch = &view_latch;

    CoreRuntime::Config config = ConfigFor(1);
    config.optimizer = surface;
    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    CommandDispatcher& d = peer.value()->dispatcher();

    const std::string set = d.Dispatch("SET CABIN_OPTIMIZER ON").response;
    EXPECT_EQ(set.rfind("OK", 0), 0u) << set;
    EXPECT_TRUE(instance_switch.load()) << "the peer's SET moved a flag the controller never reads";

    const std::string show = d.Dispatch("SHOW CABIN_OPTIMIZER").response;
    EXPECT_EQ(show.find("absent"), std::string::npos) << "the peer cannot see the controller: " << show;

    // And the read waits out a tick: the view latch held here as core 0's
    // cadence holds it. **Mutation**: drop the handler's guard and the SHOW
    // answers under the hold.
    std::optional<LatchGuard> tick(std::in_place, &view_latch);
    std::atomic<bool> answered{false};
    std::thread reader([&] {
        (void)d.Dispatch("SHOW CABIN_OPTIMIZER");
        answered.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(answered.load()) << "SHOW CABIN_OPTIMIZER read the controller mid-tick";
    tick.reset();
    reader.join();
}

TEST_F(CoreRuntimeTest, APeersCheckpointAnchorReachesTheInstanceSuperblock) {
    // PW3, and since AT-S8 with no ring in it: a peer handed the instance's
    // `SuperBlockCheckpointAnchor` publishes into it directly, from its own
    // core, where it used to send the anchor to core 0 as `kAnchorWrite`.
    //
    // The property asserted is the end of that path: the peer's anchor is
    // folded into the volume's one anchor. Under one stream there is one
    // anchor, slot 0, holding the minimum over every core - and it does not
    // move until every core has published at least once, or it would advance
    // past records a silent core still needs (AL-S3's warm-up). So the
    // arrival is proved by the fold's own input, and the anchor's movement by
    // completing the fold.
    Latch superblock_latch;
    wal::CheckpointGate gate;
    SuperBlockCheckpointAnchor receiver(core0_->superblock, *core0_store_);
    receiver.SetLatch(&superblock_latch);

    ASSERT_EQ(core0_->superblock.wal_anchor(0).checkpoint_lsn, 0u)
        << "a fresh database has no anchor before anything checkpoints";
    ASSERT_EQ(receiver.folded_cores(), 0u);

    CoreRuntime::Config config = ConfigFor(1);
    config.checkpoint_anchor = &receiver;
    config.checkpoint_gate = &gate;
    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    // `Open` ran the completion checkpoint (RC08's half for a peer), and it
    // published synchronously - no transport attached, no reactor turned.
    EXPECT_EQ(receiver.publishes(), 1u) << "the peer's completion checkpoint never published";
    EXPECT_EQ(receiver.folded_cores(), 1u)
        << "it published but is not in the fold, so it counts for nothing";
    // Held, not advanced: core 0 has not published, so one core's number is
    // not yet a floor for the instance.
    EXPECT_EQ(core0_->superblock.wal_anchor(0).checkpoint_lsn, 0u);
    EXPECT_EQ(core0_->superblock.wal_anchor(1).checkpoint_lsn, 0u);

    // Core 0 speaks, the warm-up ends, and the one anchor moves.
    //
    // **Above the peer, deliberately.** The fold selects the *lowest*
    // `redo_start_lsn` and breaks a tie by ascending core id, so a stand-in
    // sharing the peer's number would win it - and this test would then
    // assert its own synthetic record back to itself.
    const wal::Lsn peer_point = peer.value()->wal().appended_lsn();
    ASSERT_TRUE(receiver
                    .Publish({/*core_id=*/0, peer_point + 1, peer_point + 1, peer_point + 1, 0})
                    .ok());
    EXPECT_EQ(receiver.folded_cores(), 2u);
    const std::uint64_t folded = core0_->superblock.wal_anchor(0).checkpoint_lsn;
    EXPECT_GT(folded, 0u) << "every core has published and the anchor still has not moved";
    EXPECT_NE(folded, peer_point + 1)
        << "slot 0 carries the stand-in, so the peer's anchor never reached the superblock";

    // And it reached page 0, not only the in-memory object: the image
    // decoded off the frame carries the fold.
    {
        auto page = core0_store_->GetForRead(kSuperBlockPageId);
        ASSERT_TRUE(page.ok()) << page.status().message();
        auto on_page = SuperBlock::Decode(page.value().bytes());
        ASSERT_TRUE(on_page.ok()) << on_page.status().message();
        EXPECT_EQ(on_page.value().wal_anchor(0).checkpoint_lsn, folded);
    }

    // A cadence checkpoint advances it rather than republishing the first.
    ASSERT_TRUE(peer.value()->Checkpoint().ok());
    EXPECT_EQ(receiver.publishes(), 3u);
    EXPECT_GE(core0_->superblock.wal_anchor(0).checkpoint_lsn, folded);
    EXPECT_EQ(gate.skipped(), 0u);
}

TEST_F(CoreRuntimeTest, APeersCheckpointSkipsWhileAnotherCoresRuns) {
    // AT-S8's first done-condition: two cores cannot run a checkpoint at
    // once. The run is held here the way another core's checkpoint holds it;
    // the peer's cadence tick must then publish nothing and say so by the
    // gate's count, and the next tick after the release must run.
    wal::InMemoryCheckpointAnchor anchor;
    wal::CheckpointGate gate;
    CoreRuntime::Config config = ConfigFor(1);
    config.checkpoint_anchor = &anchor;
    config.checkpoint_gate = &gate;
    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    ASSERT_EQ(anchor.publishes(), 1u) << "the completion checkpoint is ungated and runs at Open";

    {
        const wal::CheckpointGate::Hold other_core(&gate);
        ASSERT_TRUE(other_core.entered());
        ASSERT_TRUE(peer.value()->Checkpoint().ok()) << "a skip is not a failure";
        EXPECT_EQ(anchor.publishes(), 1u) << "the peer checkpointed while the run was held";
        EXPECT_EQ(gate.skipped(), 1u);
    }

    ASSERT_TRUE(peer.value()->Checkpoint().ok());
    EXPECT_EQ(anchor.publishes(), 2u) << "the release did not let the next tick run";
    EXPECT_EQ(gate.skipped(), 1u);
}

TEST(CheckpointGateTest, TwoThreadsNeverHoldTheRunAtOnce) {
    // The gate's own claim, under real threads: whichever of two contenders
    // enters, the other is refused until it leaves. `inside` would reach 2
    // if both ever held the run.
    wal::CheckpointGate gate;
    std::atomic<int> inside{0};
    std::atomic<int> worst{0};
    std::atomic<std::uint64_t> entered{0};
    const auto contend = [&] {
        for (int i = 0; i < 20000; ++i) {
            const wal::CheckpointGate::Hold run(&gate);
            if (!run.entered()) continue;
            entered.fetch_add(1, std::memory_order_relaxed);
            const int now = inside.fetch_add(1) + 1;
            int seen = worst.load();
            while (now > seen && !worst.compare_exchange_weak(seen, now)) {
            }
            inside.fetch_sub(1);
        }
    };
    std::thread a(contend);
    std::thread b(contend);
    a.join();
    b.join();
    EXPECT_EQ(worst.load(), 1);
    EXPECT_EQ(entered.load() + gate.skipped(), 40000u) << "every attempt either ran or counted";
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
// The declaration has no cabin, so the resume cannot revive it and records
// it unenforceable - which is beside the point: the crash was in *reaching*
// the scan, before any of that was decided. (The peer counted it foreign
// until AT-S5d; a peer with a registry of its own resumes every declaration
// now, and one handed the instance's resumes none - the next cell.)
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
    // It reached the resume and came back, and the declaration it could not
    // revive refuses its relation's writes rather than admitting them.
    EXPECT_EQ(peer.value()->recovery().assertions_unrecovered, 1u);
    EXPECT_TRUE(peer.value()->dispatcher().assertions().CannotEnforce(oid.value()));
}

TEST_F(CoreRuntimeTest, APeerHandedTheInstancesAssertionRegistryResumesNothing) {
    // **AT-S5d**: the registry is the instance's and core 0's mount resumed
    // it before any peer was built, so a peer handed it must not resume a
    // second time - every directory would be adopted twice, and the second
    // adoption would replace the first's live directory with a fresh one
    // folded from the log, dropping whatever core 0 had reserved since.
    // Here the one declaration is unrevivable, so a peer that resumed would
    // leave it in the registry's unenforceable set; one that did not leaves
    // the registry exactly as it was handed.
    auto oid = core0_->catalog.CreateTable(catalog::kNamespacePublic, "asserted_shared",
                                           TwoColumnSchema(), catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(exec::InsertAssertion(core0_->catalog, *core0_store_, /*wal=*/nullptr,
                                      /*id=*/1, oid.value(), "a_bound",
                                      "CHECK COUNT(*) <= 100", kInvalidPageId)
                    .ok());
    ASSERT_TRUE(core0_store_->Sync().ok());

    exec::AssertionEnforcer instance(/*shared=*/true);
    CoreRuntime::Config config = ConfigFor(1);
    config.assertions = &instance;
    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    EXPECT_EQ(peer.value()->recovery().assertions_enforcing, 0u);
    EXPECT_EQ(peer.value()->recovery().assertions_unrecovered, 0u);
    EXPECT_FALSE(instance.CannotEnforce(oid.value()))
        << "the peer resumed into the instance's registry";
    EXPECT_EQ(&peer.value()->dispatcher().assertions(), &instance)
        << "the peer's writes would check a registry of their own";
}

TEST_F(CoreRuntimeTest, APeerHandedTheInstancesAssertionRegistryWritesNoSnapshotOfIt) {
    // **The AT-S5d review's defect 1.** Every core's checkpointer used to
    // snapshot its own registry; once the registry was the instance's, every
    // core snapshotted the same assertions into the one stream. Each
    // `CHECKPOINT_BEGIN` is appended outside the registry's latch, so two
    // cores' runs can land back to back, and recovery then meets the second
    // run's first group while the first run's base is still open - a
    // duplicate group id, a failed pass, and every asserted relation refusing
    // writes for the mount. Only core 0 snapshots the instance's registry
    // now, so a peer handed it writes no `ASSERT_SNAPSHOT` at its completion
    // checkpoint or at any other.
    //
    // **Mutation**, measured: the peer's checkpointer handed the registry
    // anyway - its two checkpoints write two snapshots, killed 1 in 1.
    constexpr std::uint64_t kAssertionId = 77;
    exec::AssertionEnforcer instance(/*shared=*/true);
    exec::LiveAssertion live;
    live.assertion_id = kAssertionId;
    live.target_oid = 4000;
    instance.Adopt(std::move(live));

    wal::InMemoryCheckpointAnchor anchor;
    CoreRuntime::Config config = ConfigFor(1);
    config.assertions = &instance;
    config.checkpoint_anchor = &anchor;  // AT-S8: no anchor, no checkpointer
    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    // The completion checkpoint at `Open`, then a cadence one.
    ASSERT_TRUE(peer.value()->Checkpoint().ok());
    ASSERT_EQ(anchor.publishes(), 2u) << "the cell's premise: the peer checkpointed twice";
    ASSERT_TRUE(peer.value()->Sync().ok());
    ASSERT_TRUE(core0_wal_->Flush().ok());

    int snapshots = 0;
    auto scanned = wal::ScanLog(*core0_log_device_, /*core_id=*/0, /*from_lsn=*/0,
                                [&](const wal::DecodedRecord& record) -> Status {
                                    if (record.type() != wal::RecordType::kAssertSnapshot) {
                                        return Status::OK();
                                    }
                                    auto decoded = wal::DecodeAssertSnapshot(record.payload);
                                    if (!decoded.ok()) return decoded.status();
                                    if (decoded.value().fields.assertion_id == kAssertionId) {
                                        ++snapshots;
                                    }
                                    return Status::OK();
                                });
    ASSERT_TRUE(scanned.ok()) << scanned.status().message();
    EXPECT_EQ(snapshots, 0) << "a peer snapshotted the registry core 0 snapshots";
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

// `APeerAsksForRowIdsItWasNeverGrantedAndTheRetrySucceeds` stood here until AT-S10b: it pinned the row-id refill a miss armed and the tick answered; the leases were retired at AT-S10b: a peer carves its transaction-id window from the instance's ceiling and bumps a relation's row-id mark in place, so nothing is granted to it and nothing is refilled.

// `AForeignInsertLeavesTheDemandThatBecomesThisCoresRange` stood here until AT-S5: it pinned the row-id demand a shipped insert left on this core; nothing ships, and the demand path is AT-S4's.

// `AForeignInsertLeavesNoDemandWhenRangesAreOff` stood here until AT-S5: it pinned the row-id demand a shipped insert left on this core; nothing ships, and the demand path is AT-S4's.

// `AForeignInsertThatNamesItsKeyLeavesNoDemand` stood here until AT-S5: it pinned the row-id demand a shipped insert left on this core; nothing ships, and the demand path is AT-S4's.

// `RowIdLeaseTableTest.AContiguousTopUpKeepsTheWindowAtTheRunInHand` stood here until AT-S10b: it pinned the lease's low-water window; the leases were retired at AT-S10b: a peer carves its transaction-id window from the instance's ceiling and bumps a relation's row-id mark in place, so nothing is granted to it and nothing is refilled.

// `ARelationCoreZeroCannotGrantIsAskedForOnceAndStarvesNoOther` stood here until AT-S10b: it pinned the refill's denial handling; the leases were retired at AT-S10b: a peer carves its transaction-id window from the instance's ceiling and bumps a relation's row-id mark in place, so nothing is granted to it and nothing is refilled.

// ---- P4c: a SELECT against a rotated relation executes remotely ---------

TEST_F(CoreRuntimeTest, ASelectAgainstARotatedRelationIsServedHere) {
    // **The cell that measured the whole cross-core read path end to end**
    // - compile, see `owner_core = 1`, open the step, stream the batches,
    // finish the reply - and what it measures since AT-S6 is that none of
    // that happens: the relation is unsplit, so one walk here answers it
    // and the pipeline is not opened at all. The rows are the same rows,
    // which is the point. Since AT-S9 no relation has an owner and no read
    // opens the pipeline, split or not.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth);
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

    // The session core's runtime. Its store is lease-bound, so schema
    // resolution needs CC7's grant exactly as a real session core would
    // have received at the relation's publish.
    auto runtime = CoreRuntime::Open(ConfigFor(0), *device_, clock_, nullptr);
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();

    auto out = runtime.value()->dispatcher().Dispatch("SELECT * FROM rotated");
    EXPECT_EQ(out.response,
              "id,v\\n1,0\\n2,10\\n3,20\\n4,30");

    // **The ineligible shapes are answered too since AT-S6.** A projection
    // list is still not a shape the pipeline takes (P4c), and what it fell
    // through to was the affinity refusal - which is gone, so it falls
    // through to a local walk instead. The route's eligibility rules are
    // untouched; what changed is what being ineligible costs.
    auto projected = runtime.value()->dispatcher().Dispatch("SELECT v FROM rotated");
    EXPECT_EQ(projected.response.rfind("ERR ", 0), std::string::npos) << projected.response;
    EXPECT_EQ(projected.response, "v\\n0\\n10\\n20\\n30") << projected.response;
}

// ---- RD7: a read of a split relation walks every range (AT-S9) ---------

TEST_F(CoreRuntimeTest, ASelectAgainstASplitRelationIsAnsweredWholeNotShort) {
    // The wrong answer RD7's ownership question decided. Until AT-S9 the
    // walk covered the ranges this core owned and no others, so a read of a
    // relation owned here but not *wholly* here returned one range's rows
    // and reported success - two rows where four exist - and this cell
    // pinned the refusal that stood in for it. No range has an owner since
    // AT-S9 and the walk covers every range (`TableAccess::WalkHeads`), so
    // the cell pins the answer itself: every row, from both ranges.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "half_here", TwoColumnSchema(),
                                    catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    auto upper_head = catalog2.CreateRangeEntryPage(oid.value(), 4096);
    ASSERT_TRUE(upper_head.ok()) << upper_head.status().message();
    ASSERT_TRUE(catalog2.OpenRangeRows(oid.value(), 4096, upper_head.value()).ok());

    auto ranges = catalog2.RangesOf(oid.value());
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
    place(oid.value(), 1, 10, ranges.value()[0].entry_page);
    place(oid.value(), 4096, 30, ranges.value()[1].entry_page);
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto runtime = CoreRuntime::Open(ConfigFor(0), *device_, clock_, nullptr);
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();

    auto out = runtime.value()->dispatcher().Dispatch("SELECT * FROM half_here");
    EXPECT_EQ(out.response, "id,v\\n1,10\\n4096,30") << "a range was left unwalked";
}

// ---- R4-R/RR5: the equivalence case that did not exist until RR1 --------
//
// **A split relation**, which before RR1 no core could read in any shape
// (`bench/v2.6.0/` §6a measured that at 395 rows) and which, since AT-S9,
// every core reads by walking every range.
//
// Byte-identical against the same rows unsplit, **straddling the
// boundary**, which is RB5's discipline: every defect this line has found
// returned a right answer for data that stayed on one side of the cut.
TEST_F(CoreRuntimeTest, ACoreReadsASplitRelationAsItsUnsplitTwin) {
    // What RR1 measured as "a relation this core owns but does not wholly
    // hold", and what is left of it since AT-S9 retired the owner: a split
    // relation read here answers byte for byte what the same rows unsplit
    // answer. The fan-in RR1 routed it through is gone; every range is
    // walked here.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth);
    auto split = catalog2.CreateTable(catalog::kNamespacePublic, "held", TwoColumnSchema(),
                                      catalog::ClusteredType::kHeap);
    ASSERT_TRUE(split.ok()) << split.status().message();
    auto whole = catalog2.CreateTable(catalog::kNamespacePublic, "wholly", TwoColumnSchema(),
                                      catalog::ClusteredType::kHeap);
    ASSERT_TRUE(whole.ok()) << whole.status().message();

    auto head = catalog2.CreateRangeEntryPage(split.value(), 4096);
    ASSERT_TRUE(head.ok()) << head.status().message();
    ASSERT_TRUE(catalog2.OpenRangeRows(split.value(), 4096, head.value()).ok());
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
             {1, 10}, {2, 20}, {4096, 30}, {4097, 40}}) {
        place(split.value(), id, v,
              id < 4096 ? ranges.value()[0].entry_page : ranges.value()[1].entry_page);
        place(whole.value(), id, v, whole_head);
    }
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto runtime = CoreRuntime::Open(ConfigFor(0), *device_, clock_, nullptr);
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();

    const std::string split_reply =
        runtime.value()->dispatcher().Dispatch("SELECT * FROM held").response;
    const std::string whole_reply =
        runtime.value()->dispatcher().Dispatch("SELECT * FROM wholly").response;
    ASSERT_EQ(split_reply.rfind("ERR", 0), std::string::npos)
        << "a split relation was refused: " << split_reply;
    // A star reply's header carries column names and not the relation's, so
    // these compare whole rather than modulo a substitution.
    EXPECT_EQ(split_reply, whole_reply) << "the split changed the answer";
    EXPECT_EQ(split_reply, "id,v\\n1,10\\n2,20\\n4096,30\\n4097,40");

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
// RS5 pinned a peer opening a fan-in of its own, one stage self-directed
// and one remote. The fan-in went with ownership at AT-S9; what stays is
// the equivalence from a core that is not core 0, byte-identical against
// the same rows unsplit and **straddling the boundary**, because every
// defect this line has found returned a right answer for data on one side
// of the cut.
TEST_F(CoreRuntimeTest, APeerReadsASplitRelationAsItsUnsplitTwin) {
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth);
    auto split = catalog2.CreateTable(catalog::kNamespacePublic, "spread", TwoColumnSchema(),
                                      catalog::ClusteredType::kHeap);
    ASSERT_TRUE(split.ok()) << split.status().message();
    auto whole = catalog2.CreateTable(catalog::kNamespacePublic, "twin", TwoColumnSchema(),
                                      catalog::ClusteredType::kHeap);
    ASSERT_TRUE(whole.ok()) << whole.status().message();

    auto head = catalog2.CreateRangeEntryPage(split.value(), 4096);
    ASSERT_TRUE(head.ok()) << head.status().message();
    ASSERT_TRUE(catalog2.OpenRangeRows(split.value(), 4096, head.value()).ok());
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
             {1, 10}, {2, 20}, {4096, 30}, {4097, 40}}) {
        place(split.value(), id, v,
              id < 4096 ? ranges.value()[0].entry_page : ranges.value()[1].entry_page);
        place(whole.value(), id, v, whole_head);
    }
    ASSERT_TRUE(core0_store_->Sync().ok());

    // **The reader is core 2.**
    CoreRuntime::Config peer_config = ConfigFor(2);
    peer_config.core_count = 3;
    auto runtime = CoreRuntime::Open(peer_config, *device_, clock_, nullptr);
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();

    const std::string split_reply =
        runtime.value()->dispatcher().Dispatch("SELECT * FROM spread").response;
    const std::string whole_reply =
        runtime.value()->dispatcher().Dispatch("SELECT * FROM twin").response;
    ASSERT_EQ(split_reply.rfind("ERR", 0), std::string::npos)
        << "a peer could not read a split relation: " << split_reply;
    EXPECT_EQ(split_reply, whole_reply) << "the split changed the answer";
    EXPECT_EQ(split_reply, "id,v\\n1,10\\n2,20\\n4096,30\\n4097,40");

    const std::string straddle =
        runtime.value()->dispatcher().Dispatch("SELECT * FROM spread WHERE v > 15").response;
    EXPECT_EQ(
        straddle,
        runtime.value()->dispatcher().Dispatch("SELECT * FROM twin WHERE v > 15").response);
    EXPECT_EQ(straddle, "id,v\\n2,20\\n4096,30\\n4097,40");
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
// **The oracle is the unsplit twin**: `wholly` is one chain, `held` is two
// ranges, and both are folded here since AT-S9 retired the fan-in that
// folded `held` off the wire. A difference between the two is the defect
// this test exists to catch.
//
// **Straddling the boundary**, per RB5's discipline, and with duplicate
// group keys on both sides of the cut: `GROUP BY` emits in first-seen order
// (AG6), so a walk that visited the ranges in any order but range order
// would answer the same *groups* in a different order - a wrong reply that
// every per-group assertion would still pass.
TEST_F(CoreRuntimeTest, AFoldAndAProjectionOverASpreadRelationAnswerAsTheUnsplitTwinDoes) {
    // `10` appears once on each side of the cut, so a group founded in the
    // low range is folded into again from the high one.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth);
    auto split = catalog2.CreateTable(catalog::kNamespacePublic, "held", TwoColumnSchema(),
                                      catalog::ClusteredType::kHeap);
    ASSERT_TRUE(split.ok()) << split.status().message();
    auto whole = catalog2.CreateTable(catalog::kNamespacePublic, "wholly", TwoColumnSchema(),
                                      catalog::ClusteredType::kHeap);
    ASSERT_TRUE(whole.ok()) << whole.status().message();

    auto head = catalog2.CreateRangeEntryPage(split.value(), 4096);
    ASSERT_TRUE(head.ok()) << head.status().message();
    ASSERT_TRUE(catalog2.OpenRangeRows(split.value(), 4096, head.value()).ok());
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

    auto runtime = CoreRuntime::Open(ConfigFor(0), *device_, clock_, nullptr);
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();

    // Each shape twice: over the split relation and over the unsplit twin.
    // The expected text is pinned as well as compared, so a change that
    // broke *both* the same way would not read as agreement.
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
            << "the split relation refused `" << on_split << "`: " << split_reply;
        EXPECT_EQ(split_reply, whole_reply)
            << "the split changed the answer to `" << on_split << "`";
        EXPECT_EQ(split_reply, c.expected) << "`" << on_split << "`";
    }
}

TEST_F(CoreRuntimeTest, APeerOnASharedPoolTakesNoBudgetOfItsOwn) {
    // **The share is what sharing removes** (EV4), and this cell was
    // `APeerStoreTakesItsConfiguredFrameBudgetShare`. The instance key
    // never reached a peer before 2026-08-24: only core 0's store was
    // budgeted, so every peer pool ran unbounded whatever the operator
    // configured, and the fix passed a per-core share through
    // `CoreRuntime::Config`. AM-S2 step 3 made the whole number core 0's
    // and `Expeditor` passes 0 to every peer, because handing one pool a
    // fraction of itself is what a share would now mean.
    //
    // So the contract is that the field is **ignored on a borrowed pool**
    // (`core_runtime.hpp`), and the cell asks a store whose budget it knows
    // for a number the config tried to change.
    const std::uint32_t before = core0_store_->frame_budget();
    CoreRuntime::Config config = ConfigFor(1);
    config.buffer_pool_frames = before + 8;
    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    EXPECT_EQ(peer.value()->store().frame_budget(), before)
        << "a peer re-budgeted the instance's pool from its own config";
}

TEST_F(CoreRuntimeTest, AFundedPeerInsertsIntoItsOwnRelationEndToEnd) {
    // PW1c-5's e2e: the interim guard is gone, and a peer with every
    // funding piece - fault grant, write grant (rule 6's acquisition
    // restamp inside it), a row-id block, a trx-id block - runs a
    // single-row INSERT into its own heap relation and reads it back.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "owned", TwoColumnSchema(),
                                    catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto row = catalog2.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();

    // **The write grant this cell opened with went with the grants**
    // (AW-S1b): the peer wrote through core 0's frame table, so the pages
    // core 0 formatted for a relation it owns were writable from the moment
    // they existed. **The id half of funding went at AT-S10b**: the peer
    // carves its transaction ids and bumps the relation's mark itself.
    ASSERT_TRUE(peer.value()->store().MayWrite(row.value().desc_page_id));

    const auto ins = peer.value()->dispatcher().Dispatch("INSERT INTO owned VALUES (7)").response;
    EXPECT_NE(ins.rfind("ERR", 0), 0u) << "the funded INSERT must run: " << ins;

    // Multi-row runs too (revised at the 25059bf review's S-1): the sorted
    // fill is merely ineligible on a peer, and the ordinary per-row path
    // allocates through the catalog's mark.
    const auto bulk =
        peer.value()->dispatcher().Dispatch("INSERT INTO owned VALUES (8), (9)").response;
    EXPECT_NE(bulk.rfind("ERR", 0), 0u) << "the per-row path must serve a peer: " << bulk;
    EXPECT_NE(bulk.find("rows=2"), std::string::npos) << bulk;

    // The reply is the CSV shape the neighbouring rotated-SELECT test
    // pins: a header line then one line per row, ",<v>" carrying the
    // inserted value after the issued id.
    const auto sel = peer.value()->dispatcher().Dispatch("SELECT * FROM owned").response;
    EXPECT_NE(sel.find(",7"), std::string::npos) << sel;
    EXPECT_NE(sel.find(",9"), std::string::npos) << sel;
}

// `ASpentLeaseRefusesWithTheWiresRetryableBit` stood here until AT-S10b: it pinned that an unfunded peer's write was refused `TXN_CONFLICT retryable=1` naming its lease; the leases were retired at AT-S10b: a peer carves its transaction-id window from the instance's ceiling and bumps a relation's row-id mark in place, so nothing is granted to it and nothing is refilled. `APeerListenerServesAReadAndAWriteWithNothingGrantedAndRoutesStop` pins the replacement.

// A peer that wrote a relation across several pages, then restarted, reads
// it whole and writes it again.
//
// **What the cell was for, and what it now measures** (AW-S1b). It was
// PW1c-7's: a restarted peer held nothing in memory - a fresh extent lease
// covering none of its old pages, no fault grant, no write grant - and what
// made the relation reachable again was each page's own stream stamp, which
// the store claimed on the fault. All three of those absences were
// properties of a store one core owned. Every core reaches the one frame
// table and the one free map now, so the restart has nothing to reconstruct
// and the cell holds the outcome the reconstruction existed for: the rows
// are all there, and the owner writes again.
void CoreRuntimeTest::PeerPagesSurviveARestart(bool flush_before_restart,
                                               const std::string& name) {
    {
        catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth);
        auto oid = catalog2.CreateTable(catalog::kNamespacePublic, name, TwoColumnSchema(),
                                        catalog::ClusteredType::kHeap);
        ASSERT_TRUE(oid.ok()) << oid.status().message();
        auto row = catalog2.GetSysTableRow(oid.value());
        ASSERT_TRUE(row.ok());
        ASSERT_TRUE(core0_store_->Sync().ok());
        const PageId root = row.value().desc_page_id;

        // The first run: funded the ordinary way, grown past one page.
        CoreRuntime::Config first_run = ConfigFor(1);
        auto peer = CoreRuntime::Open(first_run, *device_, clock_, nullptr);
        ASSERT_TRUE(peer.ok()) << peer.status().message();
        for (int i = 0; i < 600; ++i) {
            const auto ins = peer.value()
                                 ->dispatcher()
                                 .Dispatch("INSERT INTO " + name + " VALUES (" +
                                           std::to_string(i) + ")")
                                 .response;
            ASSERT_NE(ins.rfind("ERR", 0), 0u) << "row " << i << ": " << ins;
        }
        ASSERT_TRUE(peer.value()->Sync().ok()) << "the log is what survives";
        if (flush_before_restart) {
            ASSERT_TRUE(peer.value()->store().Sync().ok());
            // **And core 0's free map, which is the authority on which pages
            // exist.** A restarting core builds that view by reading the map
            // off the device (`core_runtime.cpp`), so without this the
            // restarted peer answers `not found` for pages the first run
            // allocated. On the log path redo's `PAGE_INIT` replay allocates
            // them instead, which is why only this arm needs it; in a real
            // instance the pass that does it is core 0's own mount, before
            // any peer attaches.
            ASSERT_TRUE(core0_store_->Sync().ok());
        }
        peer.value().reset();

        // The restart, with the ceiling core 0 would copy in.
        CoreRuntime::Config again = ConfigFor(1);
        auto reopened = CoreRuntime::Open(again, *device_, clock_, nullptr);
        ASSERT_TRUE(reopened.ok()) << reopened.status().message();

        const auto count =
            reopened.value()->dispatcher().Dispatch("SELECT COUNT(*) FROM " + name).response;
        EXPECT_NE(count.find("600"), std::string::npos) << count;
        EXPECT_TRUE(reopened.value()->store().MayWrite(root))
            << "the owner may not write the root it built";

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

TEST_F(CoreRuntimeTest, APeersOwnPagesSurviveARestart) {
    // **The device path**, which is the half that has nothing to do with
    // the log: the pages were flushed, and the restarted owner reads and
    // writes them again. It was `APeersOwnPagesSurviveARestartByTheirStamp`
    // until AW-S1b, and what changed is the mechanism, not the outcome -
    // the helper above says which. Run under the one topology every volume
    // this build creates.
    PeerPagesSurviveARestart(/*flush_before_restart=*/true, "survives_flushed");
}

// `APeerRefusesACallerSuppliedKeyAndTakesTheSameRowWithout` stood here until AT-S5: it pinned a peer refusing a caller-supplied key, which every core admits since AT-S5 (`ReadBorrowRigTest.ANamedKeyAdmitsOnAPeer` is the positive form).

TEST_F(CoreRuntimeTest, AFundedPeerGrowsItsOwnBtreeWritingNoCatalogPage) {
    // PW2-4's proof: a peer INSERTs into its own btree relation far enough
    // to divide leaves - every split page from its own lease, every root
    // move in its own granted anchor - and the sys.tables row never moves.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "btree_owned", TwoColumnSchema(),
                                    catalog::ClusteredType::kBtree);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto row = catalog2.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    ASSERT_TRUE(peer.value()->store().MayWrite(row.value().anchor_page_id));


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

    catalog::Catalog fresh(*core0_store_, storage::kDefaultInlineCellWidth);
    auto access = fresh.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    EXPECT_EQ(access.value()->desc_page_id, moved_root)
        << "the fill must read the anchor, not the row";
    // The row itself is CREATE-fixed - unchanged by the move.
    auto row_after = fresh.GetSysTableRow(oid.value());
    ASSERT_TRUE(row_after.ok());
    EXPECT_EQ(row_after.value().desc_page_id, row.value().desc_page_id);
}

TEST_F(CoreRuntimeTest, ACreateIndexOnAPeerOwnedRelationBuildsWhereItRuns) {
    // **AT-S5e.** A core-0 runtime opened bare, and a relation placed on
    // core 1 until AT-S9 retired placement. This was refused by name until AT-S5e - the build
    // was the owner's, shipped to it, and a dispatcher with no index-build
    // client had nothing to reach it with. `CREATE INDEX` builds where its
    // session is now, whoever the relation's owner is.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth);
    auto oid = catalog2.CreateTable(catalog::kNamespacePublic, "rotated_ix", TwoColumnSchema(),
                                    catalog::ClusteredType::kBtree);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto core0 = CoreRuntime::Open(ConfigFor(0), *device_, clock_, nullptr);
    ASSERT_TRUE(core0.ok()) << core0.status().message();
    const std::string reply =
        core0.value()
            ->dispatcher()
            .Dispatch("CREATE INDEX rix ON rotated_ix (v)")
            .response;
    EXPECT_EQ(reply.rfind("CREATED INDEX", 0), 0u) << reply;
    EXPECT_EQ(reply.find("built_by_core"), std::string::npos) << reply;
}

// `APeerRefusesEveryDdlVerbByNameAndStillServesReads` stood here until AT-S5: it pinned a peer refusing DDL, which runs where the session is since AT-S5 (`crosscore.md` CC11).

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

// The newline surface these listener cells speak, with no gate and no TLS.
TcpServer::ClientSetup TextSetup() {
    TcpServer::ClientSetup setup;
    setup.protocol = Protocol::kText;
    return setup;
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

TEST_F(CoreRuntimeTest, APeerListenerServesAReadAndAWriteWithNothingGrantedAndRoutesStop) {
    // FINDING 5 of the PW5 review: nothing proved a peer listener serves
    // anything. This is the whole loop over a real socket - a rotated
    // relation is served on the peer that owns it, a core-0 relation is
    // refused with the affinity answer, and STOP does not stop this
    // reactor: it fires the instance's stop hook (tcp_server.hpp's stop
    // contract, CoreRuntime::ListenAndAttach; a `kShutdown` message to the
    // system core until AU-S3).
    constexpr std::uint16_t kPort = 25442;

    // A relation owned by core 1 (the :417 test's arrangement), and one
    // owned by core 0 as the foreign control.
    catalog::Catalog catalog2(*core0_store_, storage::kDefaultInlineCellWidth);
    auto rotated = catalog2.CreateTable(catalog::kNamespacePublic, "rotated",
                                        TwoColumnSchema(), catalog::ClusteredType::kHeap);
    ASSERT_TRUE(rotated.ok()) << rotated.status().message();
    auto local0 = core0_->catalog.CreateTable(catalog::kNamespacePublic, "local0",
                                              TwoColumnSchema(), catalog::ClusteredType::kHeap);
    ASSERT_TRUE(local0.ok()) << local0.status().message();
    ASSERT_TRUE(core0_store_->Sync().ok());

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    auto row = catalog2.GetSysTableRow(rotated.value());
    ASSERT_TRUE(row.ok());
    // AU-S3: what the instance installs on every peer, standing in for
    // `Expeditor`'s `instance_stop_`. Installed **before** the listener, so
    // the stop handler it feeds exists by the time a client can send STOP.
    std::atomic<bool> instance_stopped{false};
    peer.value()->set_instance_stop([&instance_stopped] { instance_stopped.store(true); });
    ASSERT_TRUE(peer.value()->ListenAndAttach(kPort, TextSetup()).ok());

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
    // **A write with nothing granted runs** (AT-S10b). This cell pinned
    // the opposite until then: a listener-served peer held no transaction-
    // id lease and no row-id lease, so the INSERT died at the first funding
    // wall, retryably, naming the lease. A peer carves its own ids now, so
    // a session on any core writes on its first statement.
    const std::string write = RoundTrip(fd, "INSERT INTO rotated VALUES (7)");
    EXPECT_EQ(write.rfind("INSERTED", 0), 0u) << "a peer's first write did not run: " << write;

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

    // The write a recording peer would attempt, refused at the store. The
    // boundary and the asker are both stated for the reason
    // `APeerReadsTheCatalogAndCannotWriteIt` gives (AW-S1b).
    peer.value()->store().SetResidentLimit(kFirstUserPageId);
    {
        const CurrentCoreGuard as_the_peer(1);
        // Writable since AT-S5; what keeps a peer from recording here is
        // the `waystone_recording` default, AT-S7's, not the store.
        EXPECT_TRUE(peer.value()->store().MayWrite(catalog::kCatalogPageAccessStats));
        EXPECT_TRUE(peer.value()->store().MayWrite(catalog::kCatalogPagePatterns));
    }

    // And nothing on core 0's side was written by the peer existing.
    auto shapes = core0_->catalog.ListAccessStats();
    ASSERT_TRUE(shapes.ok()) << shapes.status().message();
    EXPECT_TRUE(shapes.value().empty());
}

TEST_F(CoreRuntimeTest, APeerListenerAsksTheAuthGateItIsHanded) {
    // AT-S8: every core listens, so a peer's listener must gate a session
    // exactly as core 0's does. The pairing of per-core listeners with
    // `auth = scram` was refused while a peer's listener was wired by hand
    // with neither factory - a peer-accepted session would have started
    // authenticated. The gate here refuses every line and closes, so a
    // statement answered by the dispatcher is the failure.
    constexpr std::uint16_t kPort = 25443;
    class RefuseAll final : public AuthGate {
    public:
        Result OnLine(std::string_view) override {
            Result r;
            r.reply = "ERR refused by the test's gate";
            r.close = true;
            return r;
        }
    };
    std::atomic<int> gates{0};

    auto peer = CoreRuntime::Open(ConfigFor(1), *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    std::atomic<bool> instance_stopped{false};
    peer.value()->set_instance_stop([&instance_stopped] { instance_stopped.store(true); });
    TcpServer::ClientSetup setup;
    setup.protocol = Protocol::kText;
    setup.auth = [&gates] {
        gates.fetch_add(1);
        return std::make_unique<RefuseAll>();
    };
    ASSERT_TRUE(peer.value()->ListenAndAttach(kPort, setup).ok());

    std::thread worker([&] { peer.value()->Run(); });
    const int fd = ConnectLoopback(kPort);
    ASSERT_GE(fd, 0);
    const std::string reply = RoundTrip(fd, "SHOW TABLES");
    ::close(fd);
    peer.value()->scheduler().Stop();
    worker.join();

    EXPECT_EQ(gates.load(), 1) << "the peer's listener built no gate for the session it accepted";
    EXPECT_EQ(reply.rfind("ERR refused by the test's gate", 0), 0u)
        << "the peer answered an unauthenticated statement: " << reply;
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
    ASSERT_TRUE(peer.value()->ListenAndAttach(kPort, TextSetup()).ok());

    // A socket without SO_REUSEPORT may not join a REUSEPORT group, so this
    // is the port being genuinely held by the peer.
    EXPECT_FALSE(TcpServer::Listen(kPort).ok());

    peer.value().reset();

    auto after = TcpServer::Listen(kPort);
    EXPECT_TRUE(after.ok()) << after.status().message();
}

// ---- PW1c-6b-3: core 0 and a peer, turned by hand -------------------------
//
// It ran core 0's two phases over a real ring until AT-S10d retired the
// transport; nothing in the rig crossed it by then.

// Everything the tests below share. Core 0 is a scheduler, a dispatcher
// with its transaction stack (so phase 2's DDL scope is a real
// transaction, D2) and the client that parks between the phases; the peer
// is a whole runtime owning one relation with three rows core 0 never
// faulted. `clock` is core 0's alone - the timeout test drives it by hand
// while the peer keeps the system clock, so nothing there expires.
struct ForeignIndexRig {
    explicit ForeignIndexRig(const sched::Clock& core0_clock) : clock(core0_clock) {}

    const sched::Clock& clock;
    sched::NullIoBackend io0;
    std::optional<sched::Scheduler> core0;
    std::optional<catalog::Catalog> catalog2;
    std::optional<txn::TrxIdSequence> ids;
    std::optional<txn::UndoLog> undo;
    std::optional<txn::TransactionManager> txns;
    std::optional<stats::CabinStore> cabins0;
    std::optional<CommandDispatcher> dispatcher;
    std::unique_ptr<CoreRuntime> peer;
    catalog::Oid oid = 0;
    catalog::SysTableRow row{};

    // Core 0's WAL, borrowed from the fixture (AM-S0). Since the peer
    // attaches to core 0's stream rather than opening its own, **core 0 is
    // the only thing that can drain it** - and in this rig core 0 is a bare
    // scheduler rather than a `CoreRuntime`, so the drain has to be run by
    // hand. Null in a rig opened before that wiring existed.
    wal::WalManager* shared_wal = nullptr;

    // One turn of both reactors, the peer first.
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
    bool TurnUntil(Turn turn, Done done, int max_rounds,
                   std::chrono::milliseconds ceiling = kDriveCeiling) {
        const auto deadline = std::chrono::steady_clock::now() + ceiling;
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
    // not within `TurnUntil`'s bound. A statement whose *specified* wait is
    // the probe deadline - five seconds, the ceiling's own length - passes
    // its ceiling explicitly, since the default cannot hold it plus
    // whatever follows the deadline.
    bool Drive(sched::CoroTask& statement, int max_rounds = 256,
               std::chrono::milliseconds ceiling = kDriveCeiling) {
        return TurnUntil([this] { Pump(); },
                         [&statement] { return statement.Poll() == sched::PollResult::kDone; },
                         max_rounds, ceiling);
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
    rig.core0.emplace(rig.clock, rig.io0);

    rig.catalog2.emplace(*core0_store_, storage::kDefaultInlineCellWidth);
    // The same word the peer's catalog reads (AT-S2): this catalog's DDL
    // bumps it, and the owner revalidates off the shared frame at `done`.
    // The flush that stood here carried the row to the *device*, for an
    // owner that re-read from there; one pool serves every core now.
    rig.catalog2->SetSchemaWord(&schema_word_);
    // All three instance words, not the schema one alone: this rig's second
    // catalog writes the same store, so a private oid counter here is the
    // duplicate-oid trap AT-S5b closed everywhere else. Unreachable as the
    // rig stands - one table, created before the wired catalogs issue
    // anything - and wired because the next cell that adds a table would
    // not know it had to.
    rig.catalog2->SetOidSequence(&oid_sequence_);
    rig.catalog2->SetMarkCounter(&pending_marks_);
    auto oid = rig.catalog2->CreateTable(catalog::kNamespacePublic, table, TwoColumnSchema(),
                                         catalog::ClusteredType::kBtree);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    rig.oid = oid.value();
    auto row = rig.catalog2->GetSysTableRow(rig.oid);
    ASSERT_TRUE(row.ok());
    rig.row = row.value();
    ASSERT_TRUE(core0_store_->Sync().ok());

    CoreRuntime::Config config = ConfigFor(1);
    auto peer = CoreRuntime::Open(config, *device_, clock_, nullptr);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    rig.peer = std::move(peer.value());
    // The stream the peer just attached to, so `Pump` can drain it.
    rig.shared_wal = core0_wal_.get();

    // Core 0's manager's sequence, over the superblock the peer carves from
    // (`ConfigFor`), so the two windows are disjoint.
    rig.ids.emplace(core0_->superblock);
    rig.undo.emplace(*core0_store_, /*wal=*/nullptr);
    // Unlogged, over the fixture's shared visibility (`ConfigFor` says why
    // it is shared). **The fixture's one lie, stated**: core 0's commits
    // take the window's own order - one past the highest published - while
    // the peer's take real LSNs from the stream, and the two interleave
    // consistently only while core 0 commits fewer times between two peer
    // commits than a commit record is bytes wide. A production instance
    // logs every core; the rig AV owns is where this stops being a lie.
    rig.txns.emplace(*rig.ids, *rig.undo, *core0_store_, /*wal=*/nullptr, &visibility_,
                     /*core=*/0);
    rig.cabins0.emplace();
    rig.dispatcher.emplace(core0_->superblock, *rig.catalog2, *core0_store_, /*log=*/nullptr,
                           &rig.clock, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                           exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                           /*access_statistics=*/false, &*rig.cabins0, &*rig.txns,
                           txn::IsolationLevel::kReadCommitted, /*core_id=*/0);
    // **The rig wires neither shipping nor 2PC since AT-S6**: core 0's
    // arrival half and its owner half, the coordinator's client and the
    // participant's server with its two ring handlers. None of it exists -
    // a read runs where the session is, as a write has since AT-S5.

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
    ASSERT_EQ(rig.dispatcher
                  ->Dispatch("CREATE TABLE " + base + "p (id int64, v int64) BTREE")
                  .response.substr(0, 3),
              "CRE");
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
    // Sync before funding: `AdmitWritePages` faults each granted page for
    // read before restamping it, and a creation page still only in core 0's
    // cache abandons the whole grant silently.
    ASSERT_TRUE(core0_store_->Sync().ok());
    FundPeerForRelation(rig, child_oid.value());
}

void CoreRuntimeTest::FundPeerForRelation(ForeignIndexRig& rig, catalog::Oid oid) {
    auto row = rig.catalog2->GetSysTableRow(oid);
    ASSERT_TRUE(row.ok()) << row.status().message();
    // **The page half of funding went with the write grants** (AW-S1b):
    // this granted the relation's root, anchor and var-heap head before a
    // peer could write any of them, and a shared frame table makes them
    // writable where they are formatted. The id half went at AT-S10b.
    ASSERT_TRUE(rig.peer->store().MayWrite(row.value().desc_page_id));
}

// ---- CR5 / CB4-CB6: a peer routes DDL to core 0 --------------------------
//
// PW4 refused the whole verb on a peer, because every target of
// `CREATE`/`ALTER`/`DROP` writes state only the system core may write. CR5
// keeps that premise and changes the answer: the peer **ships** the
// statement to core 0 and waits, so the refusal survives for the cases the
// route cannot serve rather than for all of them. `MayShip`'s conditions
// are what decide which of the two a client gets.

// `APeerWithNoShipClientStillRefusesDdlAndPoisons` stood here until AT-S5: it pinned a peer refusing DDL, which runs where the session is since AT-S5 (`crosscore.md` CC11).

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
    rig.peer->store().SetResidentLimit(kFirstUserPageId);
    {
        const CurrentCoreGuard as_the_peer(1);
        EXPECT_TRUE(rig.peer->store().MayWrite(catalog::kCatalogPageTables));  // AT-S5
    }

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

// `ADdlInsideAnExplicitTransactionIsStillRefusedOnAPeer` stood here until AT-S5: it pinned a peer refusing DDL, which runs where the session is since AT-S5 (`crosscore.md` CC11).

// `APeerWriteMeetingAnOpenIndexBuildWindowWaitsInsteadOfBeingRefused` stood here until AT-S5e: it pinned a peer write parking on an open index-build window; `CREATE INDEX` takes the relation `X` and builds where its session is since AT-S5e, and the owner-built ship and its window are struck.

// `ACreateIndexOnAPeerRelationIsBuiltByTheOwnerAndPublishedByCore0` stood here until AT-S5e: it pinned a peer-owned relation's index built by the owner and published by core 0; `CREATE INDEX` takes the relation `X` and builds where its session is since AT-S5e, and the owner-built ship and its window are struck.

TEST_F(CoreRuntimeTest, APeerMaintainsInsertsIntoAnIndexCore0BuiltAndReadsAnswerWhole) {
    // The e2e for the maintained path across cores: core 0 builds the index
    // on a relation core 1 owns (the owner built it until AT-S5e), and a
    // run of INSERTs on core 1 each maintains it through the frame table
    // both share, and every keyed read answers whole - the pre-build rows
    // the backfill covered and the post-build rows maintenance added, and
    // a value with no row answering none without opening the relation.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "maintained_ix");

    DispatchOutcome out;
    auto statement = rig.Start("CREATE INDEX mix ON maintained_ix (v)", out);
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    ASSERT_EQ(out.response.rfind("CREATED INDEX", 0), 0u) << out.response;
    rig.Pump(8);

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

TEST_F(CoreRuntimeTest, ADropIndexOnAPeerRelationIsAdmittedInsideATransactionAndRollsBackWhole) {
    // **AT-S5e.** This was refused inside a transaction (PW1c-6b-4): DT9's
    // "is the deleter in flight" predicate is core-local, so the owner would
    // have stopped maintaining the index before `COMMIT` and a `ROLLBACK`
    // would have restored it missing the owner's meanwhile-writes. The drop
    // takes the relation `X` now, so no writer of the relation runs on any
    // core while it is undecided and the predicate is never asked about
    // one: admitted, and a `ROLLBACK` restores the index whole.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "droptest_ix");

    DispatchOutcome out;
    auto statement = rig.Start("CREATE INDEX dix ON droptest_ix (v)", out);
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    ASSERT_EQ(out.response.rfind("CREATED INDEX", 0), 0u) << out.response;
    rig.Pump(8);

    Session session(txn::IsolationLevel::kReadCommitted);
    ASSERT_NE(rig.dispatcher->Dispatch("BEGIN", &session).response.rfind("ERR", 0), 0u);
    const std::string dropped = rig.dispatcher->Dispatch("DROP INDEX dix", &session).response;
    EXPECT_NE(dropped.find("DROPPED INDEX"), std::string::npos) << dropped;
    EXPECT_FALSE(session.failed());
    ASSERT_NE(rig.dispatcher->Dispatch("ROLLBACK", &session).response.rfind("ERR", 0), 0u);
    EXPECT_TRUE(rig.catalog2->FindIndexByName("dix").ok()) << "the rollback lost the index";

    const std::string again = rig.dispatcher->Dispatch("DROP INDEX dix").response;
    EXPECT_NE(again.find("DROPPED INDEX"), std::string::npos) << again;
}

// `ACreateIndexOnAPeerRelationIsRefusedInsideATransaction` stood here until AT-S5e: it pinned the foreign build's refusal inside a transaction; `CREATE INDEX` takes the relation `X` and builds where its session is since AT-S5e, and the owner-built ship and its window are struck.

// `ACreateIndexOnAPeerRelationTimesOutAndTellsTheOwner` stood here until AT-S5e: it pinned the foreign build's reply deadline; `CREATE INDEX` takes the relation `X` and builds where its session is since AT-S5e, and the owner-built ship and its window are struck.

// `ASecondCreateIndexOnTheRelationIsRefusedByTheOwnerWhileTheFirstBuilds` stood here until AT-S5e: it pinned the owner refusing a second build while its window was open; `CREATE INDEX` takes the relation `X` and builds where its session is since AT-S5e, and the owner-built ship and its window are struck.

// `TheReplyToAnAbandonedRequestOrphansTheTreeAndClosesTheWindow` stood here until AT-S5e: it pinned an abandoned build's reply orphaning the tree and closing the window; `CREATE INDEX` takes the relation `X` and builds where its session is since AT-S5e, and the owner-built ship and its window are struck.

// `ACreateIndexOnAPeerRelationNeedsTheReactorPath` stood here until AT-S5e: it pinned the synchronous path abandoning a foreign build; `CREATE INDEX` takes the relation `X` and builds where its session is since AT-S5e, and the owner-built ship and its window are struck.

// ---- Statement shipping, end to end (SS2/SS3) ---------------------------
//
// The rig above is the whole instance a shipped statement crosses: core 0's
// dispatcher with an arrival-core client, and a peer whose transport attach
// wired the owner's server and executor exactly as production did. What
// these pin is the fork's contract - which statements ship, which keep the
// refusal they always had, and that a shipped one really executes on the
// core that owns the relation rather than being simulated on core 0.

// `AWriteToAPeerOwnedRelationIsShippedAndTheOwnerExecutesIt` stood here until AT-S5: it pinned a write shipped to its relation's owner, and a write runs where the session is now (AT-R5).

// ---- R6-8: the cross-owner transaction, on a live path -----------------------
//
// Every row before this one was reachable only from a test that called the
// seams by hand, because `MayShip` refused inside an explicit transaction and
// so nothing ever enrolled a participant. These are the first tests in which
// a **client statement** makes a transaction cross-owner and the protocol runs
// end to end over a real ring, with the peer's participant half wired at
// its transport attach exactly as production wired it.

// `AWriteInsideATransactionEnrolsItsOwnerAndTheCommitRunsBothPhases` stood here until AT-S5: it pinned the cross-owner transaction, whose traffic went with the route at AT-S5; the protocol itself is AT-S6's to retire.

// `EachCrossOwnerCommitLegIsTimedAndAOneOwnerCommitTimesNothing` stood here until AT-S5: it pinned the cross-owner transaction, whose traffic went with the route at AT-S5; the protocol itself is AT-S6's to retire.

// `AParticipantEnrolledByReadsAlonePreparesWithNoRecord` stood here until AT-S5: it pinned the cross-owner transaction, whose traffic went with the route at AT-S5; the protocol itself is AT-S6's to retire.

// `ARolledBackCrossOwnerTransactionLeavesTheOwnersRowsAlone` stood here until AT-S5: it pinned the cross-owner transaction, whose traffic went with the route at AT-S5; the protocol itself is AT-S6's to retire.

// `AConnectionThatDiesMidCrossOwnerTransactionAbortsItsParticipants` stood here until AT-S5: it pinned the cross-owner transaction, whose traffic went with the route at AT-S5; the protocol itself is AT-S6's to retire.

// `TheCoordinatorsIsolationLevelCrossesToTheParticipant` stood here until AT-S5: it pinned the cross-owner transaction, whose traffic went with the route at AT-S5; the protocol itself is AT-S6's to retire.

// `AWriteInsideATransactionOnACoreWithNoCoordinatorKeepsItsOldRefusal` stood here until AT-S5: it pinned the cross-owner transaction, whose traffic went with the route at AT-S5; the protocol itself is AT-S6's to retire.

// `ARolledBackCrossOwnerTransactionsWritesDoNotCommitWithTheNext` stood here until AT-S5: it pinned the cross-owner transaction, whose traffic went with the route at AT-S5; the protocol itself is AT-S6's to retire.

// `AShippedStatementTheOwnerRefusesPoisonsTheTransactionThatSentIt` stood here until AT-S5: it pinned a write shipped to its relation's owner, and a write runs where the session is now (AT-R5).

TEST_F(CoreRuntimeTest, AReadOfAPeerOwnedRelationIsAnsweredHere) {
    // D1's read half: a relation another core created is read here, a
    // local walk of pages every core faults.
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
        << "a read of a peer-owned relation must answer exactly what its owner would";
}

// **Ten shipped-read cells went at AT-S6**, with the protocol they
// measured, each named with what it pinned:
//   - `ATypedClientsShippedReadComesBackAsRowsOnTheAnswerEdge` and
//     `AShippedReadToATextClientKeepsTheRenderedLine` (XG1): the two reply
//     shapes a shipped read could come back in.
//   - `AReadInsideATransactionShipsAndEnrolsSinceRR1` (RR1): the read half
//     of enrolment. `ShippedReadOwnWrite` is what stands in its place, and
//     it asserts the opposite, because the enrolment RR1 added is what
//     made a transaction unable to read its own write once AT-S5 moved the
//     write, which `ShippedReadOwnWrite` measured and now pins.
//   - `ARepeatableReadCrossOwnerTransactionReadsOnePinnedViewPerParticipant`
//     and `...ReadsOneInstantOnEveryCore` (AN-S3): the coordinator's
//     snapshot riding the wire and each participant adopting it. One core
//     reads one instant by construction now.
//   - `AnOverLongShippedReadIsRefusedAndLeavesItsTransactionOpen`: the
//     968-byte statement cap, which was a property of the wire.
//   - `TheSameShippedIdentityArrivingTwiceRunsOnceAndAnswersFromTheRecord`
//     and `AReplyLostAfterTheOwnerCommittedLeavesOneRowAndTheRetryFindsTheRecord`
//     (SS4): the dedup record, which existed so a lost reply could not
//     become a second execution.
//   - `AStormOfRefusedShippedDmlCostsTheOwnerNoPageAndNoMapGrowth` and
//     `AShippedSessionReadsItsOwnWriteBackWhenThatWriteWasRetried`: the
//     owner's cost under refusal, and read-your-own-write across a retry.
//
// What survives them is `AReadOfAPeerOwnedRelationIsAnsweredHere`: a
// relation another core created is read here. The fan-in cells went at
// AT-S9, when a split relation stopped fanning in and began to be walked
// whole where the session is.


// `UpdateAndDeleteShipToTheOwnerToo` stood here until AT-S5: it pinned a write shipped to its relation's owner, and a write runs where the session is now (AT-R5).

// `TheOwnersRefusalReachesTheClientAsTheOwnerSpelledIt` stood here until AT-S5: it pinned a write shipped to its relation's owner, and a write runs where the session is now (AT-R5).

// `AStatementInsideATransactionShipsAndEnrolsSinceR68` stood here until AT-S5: it pinned the cross-owner transaction, whose traffic went with the route at AT-S5; the protocol itself is AT-S6's to retire.



// `RowsWithNoDescriptionAreRefusedRatherThanDecoded` stood here until
// AT-S6: it pinned that a typed client handed rows with no result
// description was refused rather than given untyped values. Its own setup
// records why it cannot stand - it ran the read **inside a transaction**
// "which is what forces the shipping route", because an autocommit read of
// an unsplit foreign relation takes the remote-step edge and describes
// itself from the relation's schema. There is no shipping route, so the
// undescribed-rows path has no producer left to reach it from.


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
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "refused_read");

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
}






TEST_F(CoreRuntimeTest, AReadFarWiderThanOneRingSlotIsAnsweredWhole) {
    // A read whose reply is far past what one ring slot carried is answered
    // whole. It took the remote-step pipeline until AT-S9 retired the route
    // with ownership; it is a local walk now, and the size floor below is
    // what keeps the cell about a wide answer.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "big_read_auto");

    // Ten statements, not three hundred: a multi-row `INSERT` is one
    // transaction.
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
    // **The reply is far past what one ring slot carried** - the size floor
    // that keeps this cell about a wide answer.
    EXPECT_GT(out.response.size(), std::size_t{1000})
        << "the answer is too small for this shape to prove anything";
}

// `ASynchronousDispatchDoesNotShipBecauseItCannotAwaitTheAnswer` stood here until AT-S5: it pinned a write shipped to its relation's owner, and a write runs where the session is now (AT-R5).

// `AShippedStatementDoesNotShipOnward` stood here until AT-S5: it pinned a write shipped to its relation's owner, and a write runs where the session is now (AT-R5).

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



// `AReconnectingClientTakesAFreshShipIdSoNoStaleSequenceMatchesIt` stood here until AT-S5: it pinned a write shipped to its relation's owner, and a write runs where the session is now (AT-R5).

// ---- A2: the client stops listening -------------------------------------
//
// Two endings where nobody is waiting for the answer: the arrival core's
// deadline, and a connection that went away while its statement was parked.
// What must hold across both is that the row lands exactly once, the waiter
// is reclaimed, and the client can tell the ending apart from a refusal.

// `AShippedStatementsDeadlineIsUnknownOutcomeAndTheOwnerStillAppliesIt` stood here until AT-S5: it pinned a write shipped to its relation's owner, and a write runs where the session is now (AT-R5).

// `ADroppedConnectionsShippedStatementFinishesAndReclaimsItsWaiter` stood here until AT-S5: it pinned a write shipped to its relation's owner, and a write runs where the session is now (AT-R5).

// `AParkedShippedStatementDestroyedUnderItsWaiterLeaksTheWaiter` stood here until AT-S5: it pinned a write shipped to its relation's owner, and a write runs where the session is now (AT-R5).

// `EveryStatementSurvivesARingFilledPastItsCapacity` stood here until
// AT-S6: it filled the ring past its 16 slots with shipped statements and
// pinned that a full ring costs latency and nothing else - no message
// dropped, no answer invented, no allocation. Nothing ships, so nothing
// fills the ring that way; the transport and its own cells retired whole at
// AT-S10d.




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
    // gate, and nothing about the write's outcome: the foreign-key cells
    // own that.
    ASSERT_EQ(rig.dispatcher->Dispatch("CREATE TABLE fkparent (id int64, v int64) BTREE")
                  .response.substr(0, 3),
              "CRE");
    ASSERT_EQ(rig.dispatcher
                  ->Dispatch("CREATE TABLE fkchild (id int64, pid int64 REFERENCES fkparent) "
                             "BTREE")
                  .response.substr(0, 3),
              "CRE");
    ASSERT_TRUE(core0_store_->Sync().ok());

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

// `ACabinedPeerRelationTakesWritesAndItsOwnerServesTheCabin` stood here until AT-S5: it pinned a write shipped to its relation's owner, and a write runs where the session is now (AT-R5).

// `AShippedRefusalCrossesTheRingByteIdenticalAndTerminal` stood here until AT-S5: it pinned a write shipped to its relation's owner, and a write runs where the session is now (AT-R5).

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
// **Two placement policies held one relation on each side until AT-S9.**
// No core owns a relation since; "cross-owner" in the names below is the
// parent written from core 0 and the child from the peer.

TEST_F(CoreRuntimeTest, ACrossOwnerInsertResolvesTheParentAndWritesTheChildRow) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "ai_base");

    ASSERT_EQ(rig.dispatcher->Dispatch("CREATE TABLE aiparent (id int64, v int64) BTREE")
                  .response.substr(0, 3),
              "CRE");
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

    // **Sync before funding, not after.** `AdmitWritePages` faults each
    // granted page for read before it restamps it, so a creation page still
    // only in core 0's cache abandons the whole grant silently and the peer
    // is left owning a relation it may not write.
    ASSERT_TRUE(core0_store_->Sync().ok());
    FundPeerForRelation(rig, child_oid.value());

    // A parent row with a **named** pk, so the child below references a
    // value this test knows rather than one it parses back out of a reply.
    ASSERT_NE(rig.dispatcher->Dispatch("INSERT INTO aiparent VALUES (7, 5)").response.rfind("ERR",
                                                                                            0),
              0u);

    DispatchOutcome out;
    auto statement = rig.StartOnPeer("INSERT INTO aichild VALUES (7)", out);

    // **Nothing crosses and nothing parks.** The extraction pass runs
    // before any row work, as AH-R1 requires, and resolves the parent by
    // descending core 0's pages from core 1 - one frame table since AM-S2
    // step 3 - so the statement never suspends on another core's answer.
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    EXPECT_NE(out.response.rfind("ERR", 0), 0u) << "the cross-owner INSERT: " << out.response;

    // The row is on the peer, readable through the peer's own dispatcher.
    const std::string rows = rig.peer->dispatcher().Dispatch("SELECT pid FROM aichild").response;
    EXPECT_EQ(rows.rfind("ERR", 0), std::string::npos) << rows;
    EXPECT_NE(rows.find("7"), std::string::npos) << "the child row is not there: " << rows;

    // **And the parent is deletable afterwards.** Nothing was left holding
    // it: the reference intent a passing probe used to grant lived on the
    // parent's owner until a decide released it, and a decide that never
    // came pinned the row for the life of the process (F1). There is
    // nothing to leak, and the `DELETE` that used to be refused by the
    // intent - and then by §3a's "cannot see a child on another core" -
    // now walks the peer-owned child and answers RESTRICT on its merits.
    const std::string del =
        rig.dispatcher->Dispatch("DELETE FROM aiparent WHERE id = 7").response;
    EXPECT_NE(del.find("FK_VIOLATION"), std::string::npos) << del;
    EXPECT_EQ(del.find("relied on by a foreign key check"), std::string::npos) << del;

}

// `ACrossOwnerFkWriteInATransactionCommitsAndItsDecideEndsTheIntent` stood
// here until AT-S5f: it pinned that a transaction's decide released the
// reference intent its forward probe had been granted on the parent's
// owner. No probe grants an intent and no decide releases one.

TEST_F(CoreRuntimeTest, ACrossOwnerInsertNamingAnAbsentParentIsRefusedAndWritesNoRow) {
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "ah6_absent");
    OpenCrossOwnerFkPair(rig, "abs");

    // No parent row was ever written, so the descent answers a verdict and
    // not a wait: the child's own core reads the parent's pages and
    // refuses on `kViolation`.
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

}

TEST_F(CoreRuntimeTest, ACrossOwnerParentDeleteOnASynchronousPathRunsRatherThanRefusing) {
    // **This cell has said three things and each was that day's truth.**
    // As `ACrossOwnerParentCannotBeRetiredAtAllSoThatFixtureCannotExist` it
    // recorded §3a's refusal - RESTRICT needs an authoritative "no
    // children" and this core could not see them. AJ-T3 turned that into a
    // fan-out and left this path refusing **retryably**, because `Dispatch`
    // has no reactor to park the fan-out's answers on.
    //
    // **AT-S5f removes the asking, so the path has nothing to park on.**
    // The reverse check walks the peer-owned child from here, and a
    // statement that needs no reply needs no reactor: the DELETE runs on
    // the synchronous path exactly as it does on the served one.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "ah6_retire");
    OpenCrossOwnerFkPair(rig, "ret");
    ASSERT_NE(rig.dispatcher->Dispatch("INSERT INTO retp VALUES (7, 5)").response.rfind("ERR", 0),
              0u);

    const std::string del = rig.dispatcher->Dispatch("DELETE FROM retp WHERE id = 7").response;
    EXPECT_EQ(del, "DELETED 1") << del;

    // And the row is gone, which is the half a refusal could also have
    // claimed: this one deleted rather than declining to.
    const std::string rows = rig.dispatcher->Dispatch("SELECT v FROM retp").response;
    EXPECT_EQ(rows.find("5"), std::string::npos) << rows;

    // The converse on the same path: a parent a peer's child references is
    // refused, and refused by the constraint rather than by the path.
    ASSERT_NE(rig.dispatcher->Dispatch("INSERT INTO retp VALUES (8, 6)").response.rfind("ERR", 0),
              0u);
    DispatchOutcome child;
    auto wrote = rig.StartOnPeer("INSERT INTO retc VALUES (8)", child);
    ASSERT_TRUE(rig.Drive(*wrote)) << child.response;
    ASSERT_NE(child.response.rfind("ERR", 0), 0u) << child.response;
    const std::string kept = rig.dispatcher->Dispatch("DELETE FROM retp WHERE id = 8").response;
    EXPECT_NE(kept.find("FK_VIOLATION"), std::string::npos) << kept;
    EXPECT_EQ(kept.find("needs the reactor path"), std::string::npos) << kept;
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

TEST_F(CoreRuntimeTest, ACrossOwnerParentDeleteByPkChecksItsChildAndDeletes) {
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

    // Unreferenced: the walk over the peer-owned child finds nothing and
    // the row goes.
    DispatchOutcome gone;
    auto del7 = rig.Start("DELETE FROM pkp WHERE id = 7", gone);
    ASSERT_TRUE(rig.Drive(*del7)) << gone.response;
    EXPECT_EQ(gone.response, "DELETED 1") << gone.response;
    // Referenced from a child another core owns: RESTRICT, and there is
    // one spelling because there is one check.
    DispatchOutcome kept;
    auto del8 = rig.Start("DELETE FROM pkp WHERE id = 8", kept);
    ASSERT_TRUE(rig.Drive(*del8)) << kept.response;
    EXPECT_NE(kept.response.find("FK_VIOLATION"), std::string::npos) << kept.response;
    EXPECT_EQ(RowsWith(*rig.peer, "pkc", "8"), 1);
    const std::string rows = rig.dispatcher->Dispatch("SELECT id FROM pkp").response;
    EXPECT_EQ(rows.find("7"), std::string::npos) << rows;
    EXPECT_NE(rows.find("8"), std::string::npos) << rows;
}

TEST_F(CoreRuntimeTest, ACrossOwnerParentDeleteByPredicateChecksEveryRowItMarks) {
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

    // Three rows named by a non-pk predicate: each checked against the
    // peer-owned child as the walk reaches it, and deleted.
    DispatchOutcome three;
    auto del5 = rig.Start("DELETE FROM prp WHERE v = 5", three);
    ASSERT_TRUE(rig.Drive(*del5)) << three.response;
    EXPECT_EQ(three.response, "DELETED 3") << three.response;
    // The referenced one, by the same shape: RESTRICT from the walk.
    DispatchOutcome kept;
    auto del6 = rig.Start("DELETE FROM prp WHERE v = 6", kept);
    ASSERT_TRUE(rig.Drive(*del6)) << kept.response;
    EXPECT_NE(kept.response.find("FK_VIOLATION"), std::string::npos) << kept.response;
    // Nothing matches: nothing checked, nothing deleted, no refusal.
    DispatchOutcome none;
    auto del9 = rig.Start("DELETE FROM prp WHERE v = 99", none);
    ASSERT_TRUE(rig.Drive(*del9)) << none.response;
    EXPECT_EQ(none.response, "DELETED 0") << none.response;
    // The three unreferenced rows are gone and the referenced one stays.
    const std::string rows = rig.dispatcher->Dispatch("SELECT id, v FROM prp").response;
    EXPECT_EQ(CountOccurrences(rows, ",5"), 0) << rows;
    EXPECT_EQ(CountOccurrences(rows, ",6"), 1) << rows;
}

// **Four cells went at AT-S5f**, each the reverse fan-out's own machinery:
//   - `ACollectedSetPastOneMessageTakesAnotherRound` (AK-S3): a pk set
//     larger than one reverse message took a second round. There are no
//     messages and no rounds; the walk checks every row it marks.
//   - `ARowAppearingWhileParkedIsRefusedRetryablyAndDeletedOnRetry` (AK-S3):
//     the collect pass fixed a set and a row that appeared after it was
//     refused retryably. There is no collect pass: a row the walk reaches
//     is checked when it reaches it.
//   - `ACrossOwnerInsertNamingAnInFlightParentAnswersRetryable` (AH-T2):
//     the *refusal* this milestone exists to remove. A child naming an
//     in-flight parent on another core waits for it now, which
//     `FkCrossCoreRigTest` pins on the two-core rig.
//   - `AParentDeleteMeetingALiveForeignIntentAnswersBusyBeforeAnythingElse`
//     (AH-T3): the intent table's own cell, retired with the table. What it
//     protected - a parent deleted while another core's transaction writes a
//     child against it - is the walk's `kBusy` on the uncommitted child row.

TEST_F(CoreRuntimeTest, AStatementSpanningTwoOwnersRunsHere) {
    // **This cell asserted a refusal until AT-S6.** `SoleForeignOwner`
    // refused a chain whose steps did not all belong to one foreign core,
    // and the statement fell through to the affinity refusal - shipping a
    // statement one owner could not answer whole was the failure it
    // prevented. Nothing ships, and a join over two unsplit relations is a
    // walk of pages every core faults, so the shape that had no owner to
    // send it to is the shape that needs none.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "shipped_span");

    // A second relation on core 0, so the join spans core 0 and the peer.
    auto local = rig.catalog2->CreateTable(catalog::kNamespacePublic, "span_local",
                                           TwoColumnSchema(), catalog::ClusteredType::kBtree);
    ASSERT_TRUE(local.ok()) << local.status().message();
    auto row = rig.catalog2->GetSysTableRow(local.value());
    ASSERT_TRUE(row.ok());
    ASSERT_TRUE(core0_store_->Sync().ok());

    DispatchOutcome out;
    auto statement = rig.Start(
        "SELECT span_local.v FROM span_local JOIN shipped_span ON "
        "span_local.v = shipped_span.v",
        out);
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    EXPECT_EQ(out.response.rfind("ERR ", 0), std::string::npos) << out.response;
    EXPECT_EQ(out.response.find("is owned by core"), std::string::npos) << out.response;
}

TEST_F(CoreRuntimeTest, AnalyzeOfAPeerOwnedRelationIsPlannedHere) {
    // The read fork runs on the *stripped* text (`ANALYZE` is a dispatcher
    // prefix, not a parser keyword), so shipping it would have answered a
    // request for a plan with a result set - which is why it was excluded
    // from the ship and then refused by affinity. Neither exists: the plan
    // describes a run this core performs.
    ForeignIndexRig rig(clock_);
    OpenForeignIndexRig(rig, "no_ship_analyze");

    DispatchOutcome out;
    auto statement = rig.Start("ANALYZE SELECT * FROM no_ship_analyze", out);
    ASSERT_TRUE(rig.Drive(*statement)) << out.response;
    EXPECT_EQ(out.response.rfind("ERR ", 0), std::string::npos) << out.response;
    EXPECT_EQ(out.response.find("owned by core"), std::string::npos) << out.response;
}

// `AStatementWhoseSubqueryNamesASecondCoresRelationIsNotShipped` stood here until AT-S5: it pinned a write shipped to its relation's owner, and a write runs where the session is now (AT-R5).

// `AnIndexBuildIsRefusedForAForeignRelationAndReleasedOnAbort` stood here until AT-S5e: it pinned the owner's build-request endings over raw payloads; `CREATE INDEX` takes the relation `X` and builds where its session is since AT-S5e, and the owner-built ship and its window are struck.

}  // namespace
}  // namespace kds::server
