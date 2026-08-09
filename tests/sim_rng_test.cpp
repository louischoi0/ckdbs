#include "sim/rng.hpp"

#include <vector>

#include <gtest/gtest.h>

// SIM01 (bench/workplan-teststrategy): the harness's one entropy source.
// Three contracts, each of which the corpus discipline leans on: the same
// seed reproduces the same stream byte for byte, different seeds do not,
// and a fork is a function of (seed, label) alone — consumption elsewhere
// never shifts it.

namespace kds::sim {
namespace {

std::vector<std::uint64_t> Draw(Rng& rng, std::size_t n) {
    std::vector<std::uint64_t> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) out.push_back(rng.Next());
    return out;
}

TEST(SimRng, SameSeedYieldsTheIdenticalStream) {
    Rng a(42), b(42);
    EXPECT_EQ(Draw(a, 256), Draw(b, 256));
}

TEST(SimRng, DifferentSeedsYieldDifferentStreams) {
    Rng a(42), b(43);
    EXPECT_NE(Draw(a, 256), Draw(b, 256));
}

TEST(SimRng, ForkIsAFunctionOfSeedAndLabelAloneNotOfConsumption) {
    Rng fresh(7);
    Rng spent(7);
    // Consuming the parent must not move any child stream.
    for (int i = 0; i < 1000; ++i) spent.Next();

    Rng fork_of_fresh = fresh.Fork("workload");
    Rng fork_of_spent = spent.Fork("workload");
    EXPECT_EQ(Draw(fork_of_fresh, 64), Draw(fork_of_spent, 64));
}

TEST(SimRng, DistinctLabelsYieldIndependentStreams) {
    Rng root(7);
    Rng a = root.Fork("workload");
    Rng b = root.Fork("faults");
    EXPECT_NE(Draw(a, 64), Draw(b, 64));
}

TEST(SimRng, SubForksCompose) {
    Rng root(9);
    // The iteration-scoped forking SIM04's loop uses: every path through
    // the label tree is a distinct, stable stream.
    Rng it3 = root.Fork("iteration/3");
    Rng it4 = root.Fork("iteration/4");
    EXPECT_NE(Draw(it3, 16), Draw(it4, 16));

    Rng a = root.Fork("iteration/3").Fork("workload");
    Rng b = root.Fork("iteration/3").Fork("workload");
    EXPECT_EQ(Draw(a, 64), Draw(b, 64));
}

TEST(SimRng, BelowIsBoundedAndDeterministic) {
    Rng a(11), b(11);
    for (int i = 0; i < 1000; ++i) {
        const std::uint64_t va = a.Below(37);
        EXPECT_LT(va, 37u);
        EXPECT_EQ(va, b.Below(37));
    }
    EXPECT_EQ(a.Below(0), 0u);
}

TEST(SimRng, RangeIsInclusiveBothEnds) {
    Rng rng(13);
    bool saw_lo = false, saw_hi = false;
    for (int i = 0; i < 2000; ++i) {
        const std::int64_t v = rng.Range(-3, 3);
        EXPECT_GE(v, -3);
        EXPECT_LE(v, 3);
        saw_lo |= v == -3;
        saw_hi |= v == 3;
    }
    EXPECT_TRUE(saw_lo);
    EXPECT_TRUE(saw_hi);
}

// The mapping is spelled out in the header so it can never drift with a
// toolchain update; this pins it. If this test ever fails, every committed
// seed's behavior just changed — that is a corpus-invalidating event, not
// a constant to update.
TEST(SimRng, ForkSeedMappingIsPinned) {
    EXPECT_EQ(ForkSeed(0, ""), ForkSeed(0, ""));
    EXPECT_NE(ForkSeed(0, "a"), ForkSeed(0, "b"));
    EXPECT_NE(ForkSeed(0, "a"), ForkSeed(1, "a"));
    // One literal anchor value, computed once at authoring time.
    EXPECT_EQ(ForkSeed(42, "workload"), 0x5b161bcbab49cf85ull);
}

}  // namespace
}  // namespace kds::sim
