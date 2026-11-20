#include "kds/stats/counter_table.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/wait_state.hpp"

// `stats::CounterTable` (DL's W1, `docs/rules/rules.md` §3a): a core's counts
// by id with no latch - a slot claimed once and never moved, a bound that
// drops and counts, and a reader that takes nothing.

namespace kds::stats {
namespace {

struct Pair {
    sched::RelaxedU64 a;
    sched::RelaxedU64 b;
};

TEST(CounterTableTest, AnIdKeepsOneSlotForTheTablesLife) {
    CounterTable<Pair, 8> table;
    Pair* first = table.CountsOf(41);
    ASSERT_NE(first, nullptr);
    first->a.add(3);
    for (std::uint64_t id = 1; id <= 7; ++id) ASSERT_NE(table.CountsOf(id), nullptr);
    EXPECT_EQ(table.CountsOf(41), first);
    EXPECT_EQ(first->a.get(), 3u);
    EXPECT_EQ(table.dropped(), 0u);
}

TEST(CounterTableTest, ANewIdPastTheBoundIsDroppedAndCountedAndAKnownOneStillCounts) {
    CounterTable<Pair, 4> table;
    for (std::uint64_t id = 1; id <= 4; ++id) ASSERT_NE(table.CountsOf(id), nullptr);
    EXPECT_EQ(table.CountsOf(5), nullptr);
    EXPECT_EQ(table.CountsOf(6), nullptr);
    EXPECT_EQ(table.dropped(), 2u);
    ASSERT_NE(table.CountsOf(3), nullptr);
    EXPECT_EQ(table.dropped(), 2u);
}

TEST(CounterTableTest, AReaderSeesEveryClaimedIdOnceAndAnEmptyTableNothing) {
    CounterTable<Pair, 64> table;
    std::size_t seen = 0;
    table.ForEach([&](std::uint64_t, const Pair&) { ++seen; });
    EXPECT_EQ(seen, 0u);

    for (std::uint64_t id = 1; id <= 64; ++id) table.CountsOf(id * 1000)->b.add(id);
    std::map<std::uint64_t, std::uint64_t> rows;
    table.ForEach([&](std::uint64_t id, const Pair& p) { rows[id] += p.b.get(); });
    ASSERT_EQ(rows.size(), 64u);
    for (std::uint64_t id = 1; id <= 64; ++id) EXPECT_EQ(rows[id * 1000], id);
}

TEST(CounterTableTest, AResetZeroesTheTableWhereItStands) {
    CounterTable<Pair, 8> table;
    Pair* kept = table.CountsOf(9);
    kept->a.add(5);
    EXPECT_EQ(table.CountsOf(100), table.CountsOf(100));
    for (std::uint64_t id = 1; id <= 6; ++id) table.CountsOf(id);
    EXPECT_EQ(table.CountsOf(77), nullptr);
    table.ResetForTest();
    EXPECT_EQ(table.dropped(), 0u);
    EXPECT_EQ(kept->a.get(), 0u);
    std::size_t seen = 0;
    table.ForEach([&](std::uint64_t, const Pair&) { ++seen; });
    EXPECT_EQ(seen, 0u);
    ASSERT_NE(table.CountsOf(77), nullptr);
}

// The reader takes nothing and the writer waits for nothing: a writer adding
// through claimed slots and claiming new ones while a reader walks the table
// without pause. Under `tsan` this is the cell that shows a walk beside a
// claim is no data race; everywhere it shows no count of a single writer is
// lost.
TEST(CounterTableTest, AWriterAndAReaderRunSideBySideAndNoCountIsLost) {
    constexpr std::uint64_t kIds = 512;
    constexpr std::uint64_t kRounds = 200;
    CounterTable<Pair, 1024> table;
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> walks{0};
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            std::uint64_t rows = 0;
            table.ForEach([&](std::uint64_t id, const Pair& p) {
                ++rows;
                EXPECT_NE(id, 0u);
                EXPECT_LE(p.a.get(), kRounds);
            });
            EXPECT_LE(rows, kIds);
            walks.fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (std::uint64_t round = 0; round < kRounds; ++round) {
        for (std::uint64_t id = 1; id <= kIds; ++id) {
            Pair* counts = table.CountsOf(id);
            ASSERT_NE(counts, nullptr);
            counts->a.add(1);
        }
    }
    done.store(true, std::memory_order_release);
    reader.join();
    std::uint64_t total = 0;
    table.ForEach([&](std::uint64_t, const Pair& p) { total += p.a.get(); });
    EXPECT_EQ(total, kIds * kRounds);
    EXPECT_EQ(table.dropped(), 0u);
}

// Two threads recording as one part - the premise broken - lose at most
// counts: each id still has one slot, and the table stays within its bound.
TEST(CounterTableTest, TwoThreadsRecordingAsOnePartClaimEachIdOnce) {
    constexpr std::uint64_t kIds = 256;
    CounterTable<Pair, 256> table;
    std::vector<std::thread> writers;
    for (int t = 0; t < 2; ++t) {
        writers.emplace_back([&] {
            for (int round = 0; round < 50; ++round) {
                for (std::uint64_t id = 1; id <= kIds; ++id) {
                    if (Pair* counts = table.CountsOf(id)) counts->a.add(1);
                }
            }
        });
    }
    for (std::thread& w : writers) w.join();
    std::map<std::uint64_t, int> slots;
    table.ForEach([&](std::uint64_t id, const Pair&) { ++slots[id]; });
    EXPECT_EQ(slots.size(), kIds);
    for (const auto& [id, n] : slots) EXPECT_EQ(n, 1) << id;
}

}  // namespace
}  // namespace kds::stats
