// DZ-S1 (`instructions/v3.2.0/workorder-dz-checkpoint-reactor-residue.md`):
// a checkpoint run's dirty table is collected in slices it yields between.
// These cells pin that the sliced collection is the whole walk's table, and
// that a run task really gives its reactor back inside its walk:
//
//   - the sort and merge (an LSD radix sort, then one entry a page at its
//     lowest non-zero recLSN) answer what `MergeDirtyTable` answers, over
//     entries that span both radix digits and repeat pages;
//   - over a store, armed or not, the slices list exactly what
//     `DirtyPagesWithRecLsn` lists;
//   - a run task over the store polls a foreground task between two slices
//     of its start's walk, and still publishes.
//
// DZ-R1's crash cell, the transaction begun inside the walk, is in
// `dirty_table_chunked_walk_test.cpp` beside DO-R1's.

#include "kds/storage/dirty_table_collector.hpp"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/page_store_checkpoint_target.hpp"
#include "kds/wal/checkpointer.hpp"
#include "kds/wal/memory_log_device.hpp"

namespace kds::storage {
namespace {

using Pairs = std::vector<std::pair<PageId, wal::Lsn>>;

Pairs AsPairs(const std::vector<wal::CheckpointDirtyPage>& table) {
    Pairs out;
    for (const auto& e : table) out.emplace_back(e.page_id, e.rec_lsn);
    return out;
}

TEST(DirtyTableCollectorTest, TheSlicedSortAndMergeAnswerWhatMergeDirtyTableAnswers) {
    std::mt19937_64 rng(7);
    // Page ids across both 16-bit digits, a fifth of them repeated, a third
    // of the recLSNs 0: every case the merge has.
    std::uniform_int_distribution<PageId> page(0, 0xFFFFFFF0u);
    std::uniform_int_distribution<wal::Lsn> lsn(1, 1'000'000);
    std::vector<wal::CheckpointDirtyPage> entries;
    for (int i = 0; i < 200'000; ++i) {
        const PageId id = (i % 5 == 4 && !entries.empty())
                              ? entries[rng() % entries.size()].page_id
                              : page(rng);
        entries.push_back({id, rng() % 3 == 0 ? wal::kNoLsn : lsn(rng)});
    }
    Pairs want = AsPairs(entries);
    DevicePageStore::MergeDirtyTable(want);

    DirtyTableCollector c = DirtyTableCollector::ForEntriesForTest(std::move(entries));
    int calls = 1;
    while (!c.Advance()) ++calls;
    EXPECT_EQ(AsPairs(c.table()), want);
    // Four passes of 200,000 entries at 64 Ki a slice.
    EXPECT_GE(calls, 12) << "the sort was not sliced";
    EXPECT_TRUE(c.Advance()) << "a whole collection answered not whole";
}

TEST(DirtyTableCollectorTest, AnEmptyTableIsWholeInOneCall) {
    DirtyTableCollector c = DirtyTableCollector::ForEntriesForTest({});
    EXPECT_TRUE(c.Advance());
    EXPECT_TRUE(c.table().empty());
}

class DirtyTableCollectorStoreTest : public ::testing::TestWithParam<bool> {
protected:
    void Fill(std::size_t pages) {
        device_ = std::move(MemoryPageDevice::Create(/*extent_pages=*/64, 0).value());
        auto store = DevicePageStore::Open(*device_, FrameCapacity{4096}, /*first=*/16);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
        store_->SetLatchArmed(GetParam());
        for (std::size_t i = 0; i < pages; ++i) {
            auto created = store_->CreateNew();
            ASSERT_TRUE(created.ok()) << created.status().message();
            FormatPage(created.value().second.bytes(), PageType::kHeap);
        }
    }

    std::unique_ptr<MemoryPageDevice> device_;
    std::unique_ptr<DevicePageStore> store_;
};

TEST_P(DirtyTableCollectorStoreTest, TheSlicesListWhatTheWholeWalkLists) {
    Fill(3000);
    // Half the pages clean, and some recLSNs real, so the walk skips and keeps.
    ASSERT_TRUE(store_->Flush().ok());
    for (PageId id = 16; id < 16 + 3000; id += 2) {
        ASSERT_TRUE(store_->StampPageLsn(id, 100 + id).ok());
    }
    DirtyTableCollector c(*store_, /*chunks_per_slice=*/1);
    int calls = 1;
    while (!c.Advance()) ++calls;
    EXPECT_GE(calls, 3) << "3,000 frames span three chunks, one a slice";
    EXPECT_EQ(AsPairs(c.table()), store_->DirtyPagesWithRecLsn());
    EXPECT_EQ(c.table().size(), 1500u);
}

INSTANTIATE_TEST_SUITE_P(ArmedAndNot, DirtyTableCollectorStoreTest, ::testing::Bool(),
                         [](const auto& info) { return info.param ? "Armed" : "Unarmed"; });

TEST(DirtyTableCollectorTaskTest, ARunTaskPollsAForegroundTaskInsideItsWalkAndStillPublishes) {
    const CurrentCoreGuard as_core_0{0};
    auto device = MemoryPageDevice::Create(/*extent_pages=*/64, 0);
    ASSERT_TRUE(device.ok());
    auto opened = DevicePageStore::Open(*device.value(), FrameCapacity{4096}, /*first=*/16);
    ASSERT_TRUE(opened.ok());
    DevicePageStore& store = *opened.value();
    for (int i = 0; i < 3000; ++i) {
        auto created = store.CreateNew();
        ASSERT_TRUE(created.ok());
        FormatPage(created.value().second.bytes(), PageType::kHeap);
    }
    sched::ManualClock clock{1'000};
    sched::NullIoBackend io;
    sched::Scheduler reactor{clock, io};
    auto log = wal::MemoryLogDevice::Create(1024 * 1024);
    ASSERT_TRUE(log.ok());
    auto wal = wal::WalManager::Open(log.value().get(), clock, 0);
    ASSERT_TRUE(wal.ok());
    PageStoreCheckpointTarget target(store);
    target.SetChunksPerSliceForTest(1);
    wal::NoActiveTransactions txns;
    wal::InMemoryCheckpointAnchor anchor;
    wal::Checkpointer c(*wal.value(), target, txns, anchor, wal::CheckpointerConfig{.pages_per_step = 64});

    std::uint64_t polls = 0;
    bool stop = false;
    auto foreground = [](std::uint64_t* n, bool* halt) -> sched::Coro {
        while (!*halt) {
            ++*n;
            co_await sched::Yield{};
        }
        co_return Status::OK();
    };
    reactor.Submit(sched::MakeCoroTask(sched::SchedulingGroup::kForeground, foreground(&polls, &stop)));
    // The foreground's count at each chunk of the run's first walk - the
    // start's, before any begin record.
    std::vector<std::uint64_t> at_chunk;
    store.SetAfterDirtyTableChunkForTest([&](std::size_t) {
        if (!c.in_progress() && c.stats().started == 0) at_chunk.push_back(polls);
    });
    c.SubmitRun(reactor, nullptr, 0);
    for (int turns = 0; c.run_in_flight() && turns < 100'000; ++turns) reactor.RunOnce();
    stop = true;
    reactor.RunOnce();
    store.SetAfterDirtyTableChunkForTest(nullptr);

    ASSERT_EQ(at_chunk.size(), 3u) << "the start's walk did not visit three chunks";
    EXPECT_GT(at_chunk[1], at_chunk[0]) << "no foreground poll between two slices of the walk";
    EXPECT_GT(at_chunk[2], at_chunk[1]) << "no foreground poll between two slices of the walk";
    EXPECT_EQ(anchor.publishes(), 1u);
    EXPECT_TRUE(store.DirtyPageIds().empty()) << "the run left a page of its table dirty";
}

}  // namespace
}  // namespace kds::storage
