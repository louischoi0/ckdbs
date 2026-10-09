// **BA's census instrument** (BA-S2, BA-R0,
// `instructions/v3.0.0/workorder-ba-parallelism.md`): each counter `SHOW
// META`'s `contention` block prints is moved by a cell that forces the wait
// it counts, and an uncontended path moves nothing.
//
// The instrument is process-wide (`base/contention.hpp`), so every cell
// reads a delta across what it drives.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/contention.hpp"
#include "kds/base/current_core.hpp"
#include "kds/base/latch.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/server/session.hpp"
#include "kds/server/superblock.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/wal/durability.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

std::uint64_t WaitsOf(LatchKind kind) { return Contention::Read().waits_of(kind); }
std::uint64_t Of(Tally t) { return Contention::Read().of(t); }

// Holds `latch` on this thread while `contender` runs on another, for 200 ms
// after the contender's thread starts - ample for it to reach the futex -
// then releases it and joins.
void HoldWhile(Latch& latch, const std::function<void()>& contender) {
    latch.lock();
    std::atomic<bool> started{false};
    std::thread other([&] {
        started.store(true, std::memory_order_release);
        contender();
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(200ms);
    latch.unlock();
    other.join();
}

TEST(ContentionCountersTest, AContendedLatchCountsOneWaitUnderItsKindAndItsLength) {
    for (std::size_t k = 0; k < kLatchKindCount; ++k) {
        const auto kind = static_cast<LatchKind>(k);
        Latch latch(kind);
        const Contention::Snapshot before = Contention::Read();
        HoldWhile(latch, [&] {
            latch.lock();
            latch.unlock();
        });
        const Contention::Snapshot after = Contention::Read();
        EXPECT_EQ(after.waits_of(kind) - before.waits_of(kind), 1u) << kLatchKindNames[k];
        // The holder slept 200 ms after the contender started; the wait is
        // most of that, and a clock that read nothing would be 0.
        EXPECT_GE(after.wait_ns_of(kind) - before.wait_ns_of(kind), 10'000'000u)
            << kLatchKindNames[k];
    }
}

TEST(ContentionCountersTest, AnUncontendedLatchCountsNothing) {
    Latch latch(LatchKind::kFrameTable);
    const std::uint64_t before = WaitsOf(LatchKind::kFrameTable);
    for (int i = 0; i < 1000; ++i) {
        latch.lock();
        latch.unlock();
    }
    // Another cell in this process may contend a frame table meanwhile; this
    // one is single-threaded, so none of its acquisitions waited. The suite
    // runs cells in one process serially.
    EXPECT_EQ(WaitsOf(LatchKind::kFrameTable), before);
}

TEST(ContentionCountersTest, TheLongestRunIsTheMaximumOverEveryCore) {
    const std::uint64_t before = Contention::Read().of(Longest::kCheckpointNs);
    const std::uint64_t big = before + 7'000'000'000ull;
    {
        const CurrentCoreGuard as(5);
        Contention::Max(Longest::kCheckpointNs, big);
        Contention::Max(Longest::kCheckpointNs, big - 1);  // a shorter run keeps the longest
    }
    EXPECT_EQ(Contention::Read().of(Longest::kCheckpointNs), big);
}

// The rig's instance, with one relation, for the cells that need a store or
// both reactors.
std::unique_ptr<TwoCoreRig> OpenRig(TwoCoreRig::Options options = {}) {
    auto opened = TwoCoreRig::Open(options);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    return opened.ok() ? std::move(opened.value()) : nullptr;
}

bool StartsWith(const std::string& reply, std::string_view prefix) {
    return reply.rfind(prefix, 0) == 0;
}

TEST(ContentionCountersTest, TheFrameTableAndTheFreeMapCountUnderTheirOwnKinds) {
    std::unique_ptr<TwoCoreRig> rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    storage::DevicePageStore& store = rig->store();
    // Page 7, the catalog's `sys.tables` root: allocated on every volume.
    constexpr PageId kPage = 7;

    ASSERT_NE(store.StructureLatchForTest(), nullptr) << "the rig arms the store's latches";
    const std::uint64_t frames = WaitsOf(LatchKind::kFrameTable);
    HoldWhile(*store.StructureLatchForTest(), [&] {
        const CurrentCoreGuard as(1);
        auto page = store.GetForRead(kPage);
        EXPECT_TRUE(page.ok()) << page.status().message();
    });
    EXPECT_GE(WaitsOf(LatchKind::kFrameTable) - frames, 1u);

    ASSERT_NE(store.MapLatchForTest(), nullptr);
    const std::uint64_t map = WaitsOf(LatchKind::kFreeMap);
    HoldWhile(*store.MapLatchForTest(), [&] {
        const CurrentCoreGuard as(1);
        EXPECT_TRUE(store.IsAllocated(kPage));
    });
    EXPECT_GE(WaitsOf(LatchKind::kFreeMap) - map, 1u);
}

TEST(ContentionCountersTest, APageLatchWaitCountsItsSpinTurns) {
    std::unique_ptr<TwoCoreRig> rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    storage::DevicePageStore& store = rig->store();
    constexpr PageId kPage = 7;

    const std::uint64_t waits = Of(Tally::kPageLatchWaits);
    const std::uint64_t turns = Of(Tally::kPageLatchSpinTurns);
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {
        const CurrentCoreGuard as(0);
        auto page = store.Get(kPage);
        ASSERT_TRUE(page.ok()) << page.status().message();
        held.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    while (!held.load(std::memory_order_acquire)) std::this_thread::yield();
    std::thread reader([&] {
        const CurrentCoreGuard as(1);
        auto page = store.GetForRead(kPage);
        EXPECT_TRUE(page.ok()) << page.status().message();
    });
    std::this_thread::sleep_for(50ms);
    release.store(true, std::memory_order_release);
    holder.join();
    reader.join();
    EXPECT_GE(Of(Tally::kPageLatchWaits) - waits, 1u);
    // At least the 64 spun turns before the first yield.
    EXPECT_GE(Of(Tally::kPageLatchSpinTurns) - turns, 64u);
}

TEST(ContentionCountersTest, ACarveCountsAndKeepsItsLengthAndWaitsOnTheSuperblockLatch) {
    SuperBlock superblock = SuperBlock::CreateFresh(/*now_unix_seconds=*/1);
    Latch latch(LatchKind::kSuperblock);
    txn::TrxIdSequence sequence(superblock, [] {
        std::this_thread::sleep_for(2ms);  // a persist that takes a while
        return Status::OK();
    });
    sequence.SetLatch(&latch);

    const Contention::Snapshot before = Contention::Read();
    HoldWhile(latch, [&] {
        auto range = sequence.Carve(txn::kTrxIdBlockSize);
        EXPECT_TRUE(range.ok()) << range.status().message();
    });
    const Contention::Snapshot after = Contention::Read();
    EXPECT_EQ(after.of(Tally::kCarves) - before.of(Tally::kCarves), 1u);
    EXPECT_GE(after.waits_of(LatchKind::kSuperblock) - before.waits_of(LatchKind::kSuperblock), 1u);
    // The carve waited for the latch and slept in its persist.
    EXPECT_GE(after.of(Longest::kCarveNs), 2'000'000u);
}

TEST(ContentionCountersTest, ACheckpointRunCountsAndKeepsItsLength) {
    TwoCoreRig::Options options;
    options.fold_anchor = true;
    std::unique_ptr<TwoCoreRig> rig = OpenRig(options);
    ASSERT_NE(rig, nullptr);
    const std::uint64_t runs = Of(Tally::kCheckpointRuns);
    ASSERT_TRUE(rig->core(1).Checkpoint().ok());
    EXPECT_EQ(Of(Tally::kCheckpointRuns) - runs, 1u);
    EXPECT_GT(Contention::Read().of(Longest::kCheckpointNs), 0u);
}

// Both reactors write one relation at once - the census's monotonic-insert
// shape on two cores - and the latches every insert takes are contended:
// the frame table and the stream. Core 0 commits `relaxed`, core 1 `group`
// (its drain and the writer). Repeated rounds, bounded, until each count
// has moved: contention is a race, and a round that happens to interleave
// none of one latch is not a failure.
struct Script {
    Session* session = nullptr;
    std::vector<std::string> lines;
    std::function<bool()> go_pred;
    std::atomic<bool> go{false};
    std::atomic<std::size_t> done{0};
    std::vector<DispatchOutcome> outs;
    // Retryable refusals seen and retried (a stale descent, P9): each one is
    // a count at one of the seven refusal sites.
    std::atomic<std::uint64_t> refusals{0};
};

sched::Coro RunScript(CommandDispatcher& d, Script& s) {
    s.go_pred = [&s] { return s.go.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&s.go_pred};
    s.outs.resize(s.lines.size());
    for (std::size_t i = 0; i < s.lines.size(); ++i) {
        for (int attempt = 0; attempt < 50; ++attempt) {
            co_await d.DispatchAsync(s.lines[i], s.session, &s.outs[i]);
            if (s.outs[i].response.find("retryable=1") == std::string::npos) break;
            s.refusals.fetch_add(1, std::memory_order_relaxed);
        }
        s.done.store(i + 1, std::memory_order_release);
    }
    co_return Status::OK();
}

std::uint64_t RefusalTallies(const Contention::Snapshot& c) {
    std::uint64_t n = 0;
    for (std::size_t t = static_cast<std::size_t>(Tally::kRefusalBtreeDescend);
         t <= static_cast<std::size_t>(Tally::kRefusalIndexSecure); ++t) {
        n += c.tallies[t];
    }
    return n;
}

TEST(ContentionCountersTest, TwoCoresInsertingIntoOneRelationContendWhatAnInsertTakes) {
    // The window and the lock partitions are left to the next cell: ten
    // rounds of this shape on this host contended neither, which is a
    // census fact rather than a counter's fault.
    const LatchKind kinds[] = {LatchKind::kFrameTable, LatchKind::kWalStream};
    // Both rig cores are attached to the stream, so every sync here is the
    // writer's; the inline arm is core 0's alone under `Expeditor`, and the
    // cell after this one drives it.
    const Tally tallies[] = {Tally::kSyncsWriter, Tally::kDrainPassesPending};
    const Contention::Snapshot before = Contention::Read();
    const auto moved = [&] {
        const Contention::Snapshot now = Contention::Read();
        for (LatchKind k : kinds) {
            if (now.waits_of(k) == before.waits_of(k)) return false;
        }
        for (Tally t : tallies) {
            if (now.of(t) == before.of(t)) return false;
        }
        return true;
    };

    std::uint64_t refusals_seen = 0;
    constexpr int kRounds = 10;
    // Few enough rows that a loaded host's device syncs fit the bound below
    // (`known-gaps.md`, Testing: the IdAllocation cell's timeout).
    constexpr int kRows = 200;
    for (int round = 0; round < kRounds && !moved(); ++round) {
        std::unique_ptr<TwoCoreRig> rig = OpenRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CommandDispatcher& d1 = rig->core(1).dispatcher();
        ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE t (id int64, v int64)").response,
                               "CREATED"));
        Session relaxed;
        relaxed.set_durability(wal::DurabilityClass::kRelaxed);
        Session group;
        group.set_durability(wal::DurabilityClass::kGroup);
        Script s0{&relaxed};
        Script s1{&group};
        for (int i = 0; i < kRows; ++i) {
            s0.lines.push_back("INSERT INTO t VALUES (" + std::to_string(i) + ")");
            s1.lines.push_back("INSERT INTO t VALUES (" + std::to_string(i) + ")");
        }
        rig->core(0).scheduler().Submit(
            sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d0, s0)));
        rig->core(1).scheduler().Submit(
            sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d1, s1)));
        struct StopAtExit {
            TwoCoreRig& rig;
            ~StopAtExit() { rig.Stop(); }
        } stop_at_exit{*rig};
        rig->Start();
        s0.go.store(true, std::memory_order_release);
        s1.go.store(true, std::memory_order_release);
        ASSERT_TRUE(Within(60000ms, [&] {
            return s0.done.load() == s0.lines.size() && s1.done.load() == s1.lines.size();
        })) << "the writes never finished: core 0 at " << s0.done.load() << ", core 1 at "
            << s1.done.load() << " of " << kRows;
        for (const Script* s : {&s0, &s1}) {
            for (const DispatchOutcome& out : s->outs) {
                ASSERT_FALSE(StartsWith(out.response, "ERR")) << out.response;
            }
            refusals_seen += s->refusals.load();
        }
    }
    const Contention::Snapshot after = Contention::Read();
    // A retryable refusal a writer saw is a count at one of the seven sites -
    // at least as many, since another cell in this process may add more.
    EXPECT_GE(RefusalTallies(after) - RefusalTallies(before), refusals_seen);
    for (LatchKind k : kinds) {
        EXPECT_GT(after.waits_of(k), before.waits_of(k))
            << kLatchKindNames[static_cast<std::size_t>(k)];
    }
    for (Tally t : tallies) {
        EXPECT_GT(after.of(t), before.of(t)) << kTallyNames[static_cast<std::size_t>(t)];
    }
}

TEST(ContentionCountersTest, TheWindowAndTheLockTableCountUnderTheirOwnKinds) {
    std::unique_ptr<TwoCoreRig> rig = OpenRig();
    ASSERT_NE(rig, nullptr);

    const std::uint64_t window = WaitsOf(LatchKind::kWindow);
    HoldWhile(rig->visibility().WindowLatchForTest(), [&] {
        (void)rig->visibility().window_size();
    });
    EXPECT_EQ(WaitsOf(LatchKind::kWindow) - window, 1u);

    txn::LockTable& locks = rig->locks();
    const txn::LockKey key = txn::LockKey::Relation(/*rel=*/4242);
    ASSERT_NE(locks.LatchOf(key), nullptr) << "a two-core table has partition latches";
    const std::uint64_t partition = WaitsOf(LatchKind::kLockPartition);
    txn::LockHoldings holdings;
    HoldWhile(*const_cast<Latch*>(locks.LatchOf(key)), [&] {
        auto granted = locks.TryAcquire(/*txn=*/7, key, txn::LockMode::kIntentionShared, holdings);
        EXPECT_TRUE(granted.ok() && granted.value());
    });
    EXPECT_EQ(WaitsOf(LatchKind::kLockPartition) - partition, 1u);
    locks.Release(/*txn=*/7, holdings);

    ASSERT_NE(locks.WaitForLatchForTest(), nullptr);
    const std::uint64_t wait_for = WaitsOf(LatchKind::kLockWaitFor);
    HoldWhile(*const_cast<Latch*>(locks.WaitForLatchForTest()), [&] {
        (void)locks.WaitEdgeCount();
    });
    EXPECT_EQ(WaitsOf(LatchKind::kLockWaitFor) - wait_for, 1u);
}

TEST(ContentionCountersTest, AnOwningManagersSyncCountsAsInline) {
    auto device = wal::MemoryLogDevice::Create(wal::kDefaultSegmentSize);
    ASSERT_TRUE(device.ok()) << device.status().message();
    sched::ManualClock clock;
    auto wal = wal::WalManager::Open(device.value().get(), clock, /*core_id=*/0);
    ASSERT_TRUE(wal.ok()) << wal.status().message();
    ASSERT_FALSE(wal.value()->attached());
    const std::uint64_t inline_syncs = Of(Tally::kSyncsInline);
    const std::uint64_t writer_syncs = Of(Tally::kSyncsWriter);
    ASSERT_TRUE(wal.value()->SyncAll().ok());
    EXPECT_EQ(Of(Tally::kSyncsInline) - inline_syncs, 1u);
    EXPECT_EQ(Of(Tally::kSyncsWriter), writer_syncs);
}

TEST(ContentionCountersTest, ShowMetaPrintsTheBlockForEveryKindAndTally) {
    std::unique_ptr<TwoCoreRig> rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    const std::string meta = rig->core(1).dispatcher().Dispatch("SHOW META").response;
    for (const char* name : kLatchKindNames) {
        EXPECT_NE(meta.find(std::string(" contention_") + name + "_waits="), std::string::npos)
            << name;
        EXPECT_NE(meta.find(std::string(" contention_") + name + "_wait_us="), std::string::npos)
            << name;
    }
    for (const char* name : kTallyNames) {
        EXPECT_NE(meta.find(std::string(" contention_") + name + "="), std::string::npos) << name;
    }
    for (const char* name : kLongestNames) {
        EXPECT_NE(meta.find(std::string(" contention_") + name + "="), std::string::npos) << name;
    }
}

TEST(ReactorCpusTest, ParsesAListAndRefusesWhatIsNotOne) {
    auto two = ParseReactorCpus("0, 2");
    ASSERT_TRUE(two.ok()) << two.status().message();
    EXPECT_EQ(two.value(), (std::vector<std::uint32_t>{0, 2}));
    for (std::string_view bad : {"", "0,", "a", "0,,2", "-1", "4294967296"}) {
        auto parsed = ParseReactorCpus(bad);
        EXPECT_FALSE(parsed.ok()) << "'" << bad << "'";
        if (!parsed.ok()) EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
    }
}

TEST(ReactorCpusTest, NamesOneDistinctCpuPerCoreThisMachineHas) {
    EXPECT_TRUE(CheckReactorCpus({}, 4, 8).ok()) << "no map keeps the default";
    EXPECT_TRUE(CheckReactorCpus({0, 2}, 2, 8).ok());
    EXPECT_TRUE(CheckReactorCpus({0, 9}, 2, 0).ok()) << "an unknown CPU count checks no bound";
    EXPECT_FALSE(CheckReactorCpus({0}, 2, 8).ok()) << "one CPU short";
    EXPECT_FALSE(CheckReactorCpus({0, 2, 4}, 2, 8).ok()) << "one CPU over";
    EXPECT_FALSE(CheckReactorCpus({2, 2}, 2, 8).ok()) << "a CPU twice";
    EXPECT_FALSE(CheckReactorCpus({0, 8}, 2, 8).ok()) << "a CPU the machine lacks";
}

}  // namespace
}  // namespace kds::server
