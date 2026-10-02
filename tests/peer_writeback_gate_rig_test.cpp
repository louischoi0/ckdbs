// BA-S1: a peer's writeback waits on the WAL writer, not on core 0's inline
// sync. Defect C of `instructions/v3.0.0/workorder-ba-parallelism.md` §1.7,
// ruled by its BA-R1; the bug entry is
// `docs/inflight/bugs/a-peers-writeback-runs-core-0s-wal-sync-on-the-peers-thread.md`.
//
// **The shared store has one WAL gate, core 0's owning manager** - here
// `TwoCoreRig::wal()`, which no peer re-gates. A writeback on core 1 of a page
// whose record is not yet durable therefore runs the owner's `Sync()` on core
// 1's thread: the owning arm's `fdatasync` and its plain fields, which
// `manager.hpp` says no other thread touches. The fix keeps a gate per core,
// picked by `CurrentCore()`, and a peer's is its attached manager: a flush
// through the stream latch and a wait on the writer, the route its commits
// already take.
//
// **The red is the route, read off counters only one arm moves.** The owning
// arm bumps the owner's `syncs` and `flushes` and is the one caller of
// `WalStream::Sync`, so the stream's own durable watermark moves only there;
// the attached arm moves the writer's syncs and watermark and core 1's
// `flushes`. The race itself is not shown and cannot be: both rig runtimes
// are attached, so no reactor here writes the owner's fields for a peer to
// race, and what the cells prove is that no peer reaches them.
//
// **One cell per peer path**, the order's four and the shutdown checkpoint,
// each also asserting its page clean, so a writeback that never ran cannot
// pass for one that waited. The last cell is green on both trees: core 0's
// own writeback still syncs inline, `WalManager::Sync()`'s recorded decision.
//
// **Core 1 is `CurrentCore() == 1`**, the one thing the fix keys on. Most
// cells declare it on the rig's thread with a `CurrentCoreGuard`, as
// `CoreRuntime::Open`, `Checkpoint()` and `Dispatch` do in production, so one
// run kills a mutant; one runs on core 1's reactor to show the guard stands
// for it. `stats()` is plain fields, read on the driving thread or after a
// join; baselines follow `Open`, whose completion checkpoint on core 1 has
// already moved the writer; and `wal_drain_interval_ns` stays at the rig's 0,
// so no idle burn carves behind a cell.

#include "two_core_rig.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/superblock_checkpoint_anchor.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/wal/record.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

std::unique_ptr<TwoCoreRig> OpenRig() {
    auto rig = TwoCoreRig::Open();
    EXPECT_TRUE(rig.ok()) << rig.status().message();
    return rig.ok() ? std::move(rig.value()) : nullptr;
}

// A page and the LSN of the record that describes it.
struct Stamped {
    PageId page = kInvalidPageId;
    wal::Lsn lsn = wal::kNoLsn;
};

// **A dirty page whose record is not yet durable, made as `core`** - what
// every writeback below is asked to write. The record goes in through that
// core's own manager, so a peer's lands in the shared ring as its statements'
// do, and the page is stamped under its handle, the order every logged
// mutation keeps.
::testing::AssertionResult StampUndurable(TwoCoreRig& rig, std::uint32_t core, Stamped& out) {
    const CurrentCoreGuard as_core(core);
    auto created = rig.store().CreateNew();
    if (!created.ok()) return ::testing::AssertionFailure() << created.status().message();
    out.page = created.value().first;
    storage::FormatPage(created.value().second.bytes(), PageType::kHeap);
    auto lsn = rig.core(core).wal().Append(
        wal::RecordSpec{wal::RecordType::kHeapInsert, /*txn_id=*/1, out.page, 0});
    if (!lsn.ok()) return ::testing::AssertionFailure() << lsn.status().message();
    out.lsn = lsn.value();
    if (Status s = rig.store().StampPageLsn(out.page, out.lsn); !s.ok()) {
        return ::testing::AssertionFailure() << s.message();
    }
    // Were it durable already, every gate below would be a no-op on both
    // trees and nothing would be under test.
    if (rig.wal().IsDurable(out.lsn)) {
        return ::testing::AssertionFailure()
               << "lsn " << out.lsn << " was durable before any writeback asked";
    }
    return ::testing::AssertionSuccess();
}

// **What the two arms of `WalManager::Sync()` move, in one read.** The
// owner's `syncs` and `flushes` and the stream's own watermark are the owning
// arm's; the writer's syncs and watermark and core 1's `flushes` are the
// attached arm's (`syncs` is always 0 on an attached manager, so it is not
// read).
struct Gauges {
    std::uint64_t owner_syncs = 0;
    std::uint64_t owner_flushes = 0;
    wal::Lsn stream_durable = wal::kNoLsn;
    std::uint64_t writer_syncs = 0;
    wal::Lsn writer_durable = wal::kNoLsn;
    std::uint64_t peer_flushes = 0;

    static Gauges Read(TwoCoreRig& rig) {
        Gauges g;
        g.owner_syncs = rig.wal().stats().syncs;
        g.owner_flushes = rig.wal().stats().flushes;
        g.stream_durable = rig.wal().stream()->durable_lsn();
        g.writer_syncs = rig.wal().writer_syncs();
        g.writer_durable = rig.wal().writer()->durable_lsn();
        g.peer_flushes = rig.core(1).wal().stats().flushes;
        return g;
    }
};

// The red half of every peer cell: today each of these moves, on the peer's
// thread.
void ExpectTheOwnerDidNotSync(TwoCoreRig& rig, const Gauges& before) {
    const Gauges now = Gauges::Read(rig);
    EXPECT_EQ(now.owner_syncs, before.owner_syncs)
        << "core 0's manager synced inline for a peer's writeback";
    EXPECT_EQ(now.owner_flushes, before.owner_flushes)
        << "core 0's manager flushed for a peer's writeback";
    EXPECT_EQ(now.stream_durable, before.stream_durable)
        << "the stream's own watermark moved, and only the owning arm moves it";
}

// The green half, where nothing else asks the writer: a cell that completes a
// checkpoint cannot use it, because `Complete()` asks the writer on both trees.
void ExpectTheWriterSyncedPast(TwoCoreRig& rig, const Gauges& before, wal::Lsn lsn) {
    const Gauges now = Gauges::Read(rig);
    EXPECT_GT(now.writer_syncs, before.writer_syncs) << "the writer was never asked";
    EXPECT_GT(now.writer_durable, lsn) << "the writer's watermark is not past the record";
    EXPECT_GT(now.peer_flushes, before.peer_flushes)
        << "core 1's own manager never flushed, so its gate was not the one asked";
}

// **The page went out, after its record.** Clean is what makes the rest mean
// anything: a writeback that never ran moves none of the counters, and would
// pass `ExpectTheOwnerDidNotSync` for nothing.
void ExpectWrittenBack(TwoCoreRig& rig, const Stamped& p) {
    EXPECT_TRUE(rig.wal().IsDurable(p.lsn)) << "the page went out ahead of its record";
    const std::vector<PageId> dirty = rig.store().DirtyPageIds();
    const bool still_dirty = std::find(dirty.begin(), dirty.end(), p.page) != dirty.end();
    EXPECT_FALSE(still_dirty) << "page " << p.page << " is still dirty: no writeback ran";
}

TEST(PeerWritebackGateRigTest, APeersWritebackWaitsOnTheWriterNotCoreZerosInlineSync) {
    // The defect at its smallest, on one thread, so one run decides: core 1
    // writes back one page whose record is not durable. Today the store's
    // one gate is the owner and its inline arm runs here; fixed, core 1's
    // gate flushes through the latch and waits on the writer.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    Stamped p;
    ASSERT_TRUE(StampUndurable(*rig, /*core=*/1, p));
    const Gauges before = Gauges::Read(*rig);

    const CurrentCoreGuard as_core_1(1);
    const PageId ids[] = {p.page};
    const Status flushed = rig->store().FlushPages(ids);
    ASSERT_TRUE(flushed.ok()) << flushed.message();

    ExpectTheOwnerDidNotSync(*rig, before);
    ExpectTheWriterSyncedPast(*rig, before, p.lsn);
    ExpectWrittenBack(*rig, p);
}

// The writeback, as a task on whichever reactor polls it. `ran_as` and
// `status` are written there and read after `Stop()`, whose join orders them;
// only `done` is read while the reactor runs.
struct ReactorFlush {
    PageId page = kInvalidPageId;
    std::uint32_t ran_as = ~std::uint32_t{0};
    Status status;
    std::atomic<bool> done{false};
};

sched::Coro FlushOnThisReactor(storage::DevicePageStore& store, ReactorFlush& f) {
    f.ran_as = CurrentCore();
    const PageId ids[] = {f.page};
    f.status = store.FlushPages(ids);
    f.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

TEST(PeerWritebackGateRigTest, APeersWritebackOnItsOwnReactorWaitsOnTheWriter) {
    // The cell above on core 1's real thread, whose identity is the
    // reactor's own (`Scheduler::RunOnce`) rather than a guard's. Nothing
    // else on either reactor syncs: an attached manager's drain with no
    // commit staged neither flushes nor asks the writer.
    //
    // Before the rig, so it outlives the reactor that writes it on every
    // exit.
    ReactorFlush flush;
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    Stamped p;
    ASSERT_TRUE(StampUndurable(*rig, /*core=*/1, p));
    const Gauges before = Gauges::Read(*rig);

    flush.page = p.page;
    rig->core(1).scheduler().Submit(sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                                         FlushOnThisReactor(rig->store(), flush)));
    rig->Start();
    const bool ran =
        KickUntil(*rig, 1, [&] { return flush.done.load(std::memory_order_acquire); });
    rig->Stop();
    ASSERT_TRUE(ran) << "core 1's reactor never ran the writeback";
    ASSERT_TRUE(flush.status.ok()) << flush.status.message();
    EXPECT_EQ(flush.ran_as, 1u) << "the writeback did not run as its reactor's core";

    ExpectTheOwnerDidNotSync(*rig, before);
    ExpectTheWriterSyncedPast(*rig, before, p.lsn);
    ExpectWrittenBack(*rig, p);
}

TEST(PeerWritebackGateRigTest, APeersCheckpointFlushWaitsOnTheWriter) {
    // The order's first path: the checkpoint's `Step` writes its dirty
    // table back through `FlushPages`. `Checkpoint()` declares core 1 itself
    // and needs no reactor. The writer is no evidence here - `Complete()`
    // makes CHECKPOINT_END durable through core 1's manager on both trees -
    // so the owner's stillness is, with the page clean, which also says the
    // checkpoint gate was entered rather than skipped.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    Stamped p;
    ASSERT_TRUE(StampUndurable(*rig, /*core=*/1, p));
    const Gauges before = Gauges::Read(*rig);

    const Status checkpointed = rig->core(1).Checkpoint();
    ASSERT_TRUE(checkpointed.ok()) << checkpointed.message();

    ExpectTheOwnerDidNotSync(*rig, before);
    ExpectWrittenBack(*rig, p);
}

TEST(PeerWritebackGateRigTest, APeersAnchorPublishWaitsOnTheWriter) {
    // The order's second path: the publish's store sync. The rig's peer
    // publishes into an in-memory anchor, which syncs nothing, so the cell
    // builds production's - over the instance's superblock and its latch, as
    // `Expeditor::Open` does - and publishes as core 1.
    //
    // **The record never lands**: one core of two has published into this
    // anchor, so the fold's warm-up rewrites the mount anchor unchanged, and
    // the numbers are `superblock_checkpoint_anchor_test.cpp`'s. What the
    // cell needs is the sync after it.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    const CoreRuntime::Config& config = rig->core(1).config();
    SuperBlockCheckpointAnchor anchor(*config.trx_id_ceiling.superblock, rig->store());
    anchor.SetLatch(config.trx_id_ceiling.latch);
    Stamped p;
    ASSERT_TRUE(StampUndurable(*rig, /*core=*/1, p));
    const Gauges before = Gauges::Read(*rig);

    const CurrentCoreGuard as_core_1(1);
    const Status published = anchor.Publish({/*core_id=*/1, 950, 850, 1050, 0});
    ASSERT_TRUE(published.ok()) << published.message();
    EXPECT_EQ(anchor.publishes(), 1u);

    ExpectTheOwnerDidNotSync(*rig, before);
    ExpectTheWriterSyncedPast(*rig, before, p.lsn);
    ExpectWrittenBack(*rig, p);
}

TEST(PeerWritebackGateRigTest, APeersTrxIdCarveWaitsOnTheWriter) {
    // The order's third path: a carve persists page 0 through the store's
    // `Sync()` (the rig's `PersistSuperBlock`, `Expeditor::
    // PersistTrxIdCeiling`'s shape). `BurnWindow` - the idle burn's call -
    // carves unconditionally, where a `BEGIN` carves only on a spent window.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    Stamped p;
    ASSERT_TRUE(StampUndurable(*rig, /*core=*/1, p));
    const Gauges before = Gauges::Read(*rig);

    const CurrentCoreGuard as_core_1(1);
    txn::TrxIdSequence& trx_ids = rig->core(1).trx_ids();
    const std::uint64_t ceiling_before = trx_ids.ceiling();
    const Status burned = trx_ids.BurnWindow();
    ASSERT_TRUE(burned.ok()) << burned.message();
    EXPECT_GT(trx_ids.ceiling(), ceiling_before) << "no window was carved";

    ExpectTheOwnerDidNotSync(*rig, before);
    ExpectTheWriterSyncedPast(*rig, before, p.lsn);
    ExpectWrittenBack(*rig, p);
}

TEST(PeerWritebackGateRigTest, AClientSyncOnAPeerWaitsOnTheWriter) {
    // The order's fourth path, which one thread cannot reach: `HandleSync`
    // makes every appended record durable through core 1's manager before it
    // syncs the store, so its gate has nothing left to sync unless another
    // core's stamp lands between the two. The rig's thread is that core. It
    // holds P exclusive before the SYNC starts, waits out SYNC's log sync,
    // then appends, stamps P and lets it go; core 1's copy takes P shared,
    // so the stamp reaches one of its two gates - the batch's or the run's -
    // on core 1's thread. Core 0 stamps and core 1 writes, so a gate picked
    // by the page's stamp rather than by the caller fails here too.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    // Core 0 throughout: a hold of core 1's own would be re-entered by its
    // copy rather than waited for (`PageLatch` is re-entrant for its owner).
    const CurrentCoreGuard as_core_0(0);

    PageId page = kInvalidPageId;
    {
        auto created = rig->store().CreateNew();
        ASSERT_TRUE(created.ok()) << created.status().message();
        page = created.value().first;
        storage::FormatPage(created.value().second.bytes(), PageType::kHeap);
    }
    {
        // Bytes in the ring, so SYNC's log sync asks the writer - which is
        // what the barrier below waits for.
        const CurrentCoreGuard as_core_1(1);
        auto appended = rig->core(1).wal().Append(
            wal::RecordSpec{wal::RecordType::kHeapInsert, /*txn_id=*/1, page, 0});
        ASSERT_TRUE(appended.ok()) << appended.status().message();
    }
    const Gauges before = Gauges::Read(*rig);

    // **Released and joined on every exit.** An `ASSERT` that left the hold
    // in place would leave core 1 waiting on it and the join waiting on core
    // 1 - how AV-S2's cell hung.
    struct HeldSync {
        std::string reply;  // core 1's, read after the join
        std::atomic<bool> replied{false};
        storage::PageRef hold;  // core 0's, exclusive
        std::thread sync;
        ~HeldSync() {
            hold.Release();
            if (sync.joinable()) sync.join();
        }
    } held;
    {
        auto got = rig->store().Get(page);
        ASSERT_TRUE(got.ok()) << got.status().message();
        held.hold = std::move(got.value());
    }
    // `Dispatch` declares core 1 itself, as it does for every statement.
    held.sync = std::thread([&rig, &held] {
        held.reply = rig->core(1).dispatcher().Dispatch("SYNC").response;
        held.replied.store(true, std::memory_order_release);
    });
    ASSERT_TRUE(Within(2000ms, [&] { return rig->wal().writer_syncs() > before.writer_syncs; }))
        << "core 1's SYNC never synced the log";

    auto l2 = rig->core(0).wal().Append(
        wal::RecordSpec{wal::RecordType::kHeapInsert, /*txn_id=*/1, page, 0});
    ASSERT_TRUE(l2.ok()) << l2.status().message();
    // Asked before the stamp: from the stamp on, core 1's batch gate may
    // read it and sync without waiting for the hold.
    ASSERT_FALSE(rig->wal().IsDurable(l2.value())) << "durable before any gate asked";
    const Stamped p{page, l2.value()};
    ASSERT_TRUE(rig->store().StampPageLsn(p.page, p.lsn).ok());
    held.hold.Release();

    ASSERT_TRUE(Within(2000ms, [&] { return held.replied.load(std::memory_order_acquire); }))
        << "core 1's SYNC never finished";
    held.sync.join();

    EXPECT_EQ(held.reply, "OK synced");
    ExpectTheOwnerDidNotSync(*rig, before);
    // SYNC's log sync is one; the gate's is the second, and today it is the
    // owner's instead.
    EXPECT_GE(rig->wal().writer_syncs(), before.writer_syncs + 2)
        << "the gate's sync was not the writer's";
    ExpectWrittenBack(*rig, p);
}

TEST(PeerWritebackGateRigTest, APeersShutdownCheckpointWaitsOnTheWriter) {
    // A fifth peer path the bug entry does not list: the shutdown checkpoint
    // syncs the store before it checkpoints, as core 1, from the thread that
    // joined the reactors - here before they ever start. As in the
    // checkpoint cell the writer is no evidence: the checkpoint after the
    // flush completes through it on both trees.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    Stamped p;
    ASSERT_TRUE(StampUndurable(*rig, /*core=*/1, p));
    const Gauges before = Gauges::Read(*rig);

    const Status shut = rig->core(1).ShutdownCheckpoint();
    ASSERT_TRUE(shut.ok()) << shut.message();

    ExpectTheOwnerDidNotSync(*rig, before);
    ExpectWrittenBack(*rig, p);
}

TEST(PeerWritebackGateRigTest, CoreZerosWritebackStillSyncsInlineOnTheOwner) {
    // Green on both trees, and the fix has to keep it so: core 0's waited-on
    // syncs stay inline, the decision `WalManager::Sync()` records. A fix
    // that gave core 0 a gate of its own - in the rig, its runtime's
    // attached manager - would send this through the writer.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    Stamped p;
    ASSERT_TRUE(StampUndurable(*rig, /*core=*/0, p));
    const Gauges before = Gauges::Read(*rig);

    const CurrentCoreGuard as_core_0(0);
    const PageId ids[] = {p.page};
    const Status flushed = rig->store().FlushPages(ids);
    ASSERT_TRUE(flushed.ok()) << flushed.message();

    const Gauges after = Gauges::Read(*rig);
    EXPECT_EQ(after.owner_syncs, before.owner_syncs + 1)
        << "core 0's writeback did not sync inline";
    EXPECT_EQ(after.owner_flushes, before.owner_flushes + 1)
        << "the owner's sync did not flush the staged record";
    EXPECT_GT(after.stream_durable, p.lsn) << "the stream's watermark is not past the record";
    EXPECT_EQ(after.writer_syncs, before.writer_syncs)
        << "core 0's writeback handed its sync to the writer";
    EXPECT_EQ(after.peer_flushes, before.peer_flushes)
        << "core 1's manager flushed for core 0's writeback";
    ExpectWrittenBack(*rig, p);
}

}  // namespace
}  // namespace kds::server
