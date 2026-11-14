#include "kds/base/cycle_guard.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

// `CycleGuard` is the bound of every walk whose chain may be legally longer
// than any budget (the audit's G2, G16, G19): it must never refuse an
// acyclic walk however long, and must refuse every cycle, soon.

namespace kds {
namespace {

// Walks the sequence `start, f(start), f(f(start)), ...` - a tail of
// `tail` distinct ids into a cycle of `cycle` distinct ids, or no cycle at
// all when `cycle` is 0 - and returns the number of hops before the guard
// answered, or `limit` if it never did.
std::uint64_t HopsToDetect(std::uint64_t tail, std::uint64_t cycle, std::uint64_t limit) {
    const auto next = [&](std::uint64_t id) {
        if (cycle == 0 || id + 1 < tail + cycle) return id + 1;
        return tail;  // the last id of the cycle links back to its first
    };
    std::uint64_t at = 0;
    CycleGuard<std::uint64_t> guard(at);
    for (std::uint64_t hop = 1; hop <= limit; ++hop) {
        at = next(at);
        if (guard.Revisits(at)) return hop;
    }
    return limit;
}

TEST(CycleGuardTest, AnAcyclicWalkIsNeverRefusedHoweverLong) {
    // Past 2^21 hops - twice the budget the walks it replaces refused at.
    constexpr std::uint64_t kHops = (std::uint64_t{1} << 21) + 12345;
    EXPECT_EQ(HopsToDetect(kHops + 1, 0, kHops), kHops);
}

TEST(CycleGuardTest, EveryCycleIsFoundWithinThreeTimesItsLengthPlusItsTail) {
    for (const std::uint64_t cycle : {1u, 2u, 3u, 7u, 64u, 1000u, 65537u}) {
        for (const std::uint64_t tail : {0u, 1u, 5u, 4096u}) {
            SCOPED_TRACE(testing::Message() << "tail " << tail << " cycle " << cycle);
            const std::uint64_t bound = 3 * (tail + cycle) + 2;
            const std::uint64_t hops = HopsToDetect(tail, cycle, bound + 1);
            EXPECT_LE(hops, bound) << "the cycle was not found in time";
            EXPECT_GE(hops, tail + cycle) << "a revisit was claimed before one happened";
        }
    }
}

TEST(CycleGuardTest, ASelfLinkIsFoundOnTheFirstHop) {
    CycleGuard<std::uint32_t> guard(7);
    EXPECT_TRUE(guard.Revisits(7));
}

}  // namespace
}  // namespace kds
