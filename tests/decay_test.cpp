#include "kds/stats/decay.hpp"

#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include "alloc_counter.hpp"
#include "kds/sched/clock.hpp"

// The lazy-decay score (docs/spec/physical-optimizer.md R1, workplan PX02).
//
// The precision contract these tests pin: exact at whole half-lives,
// bucketed between them. The exact points are asserted as equalities; the
// buckets are asserted against the public LUT, because the LUT *is* the
// contract there, not an implementation detail.

namespace kds::stats {
namespace {

constexpr sched::MonoTimeNs kHalfLife = 600'000'000'000ULL;  // the [PROPOSED] 600 s

// Seed a state with `points` touches at the clock's current instant.
DecayState Seed(std::uint32_t points, const sched::Clock* clock) {
    DecayState s;
    for (std::uint32_t i = 0; i < points; ++i) Touch(s, clock, kHalfLife);
    return s;
}

TEST(DecayTest, AbsentClockDegradesToARawCounter) {
    DecayState s;
    for (int i = 0; i < 5; ++i) Touch(s, nullptr, kHalfLife);
    EXPECT_EQ(ValueAt(s, nullptr, kHalfLife), 5 * kDecayScoreScale);
}

TEST(DecayTest, OneHalfLifeHalvesExactly) {
    sched::ManualClock clock;
    DecayState s = Seed(8, &clock);
    clock.Advance(kHalfLife);
    EXPECT_EQ(ValueAt(s, &clock, kHalfLife), 4 * kDecayScoreScale);
}

TEST(DecayTest, TwoHalfLivesQuarterExactly) {
    sched::ManualClock clock;
    DecayState s = Seed(8, &clock);
    clock.Advance(2 * kHalfLife);
    EXPECT_EQ(ValueAt(s, &clock, kHalfLife), 2 * kDecayScoreScale);
}

TEST(DecayTest, AccumulateAddsWholePointsAfterDecaying) {
    sched::ManualClock clock;
    DecayState s;
    Accumulate(s, &clock, kHalfLife, 10);
    EXPECT_EQ(ValueAt(s, &clock, kHalfLife), 10 * kDecayScoreScale);

    // Decay-then-add: one half-life halves the 10, then 6 more land whole.
    clock.Advance(kHalfLife);
    Accumulate(s, &clock, kHalfLife, 6);
    EXPECT_EQ(ValueAt(s, &clock, kHalfLife), 11 * kDecayScoreScale);

    // The N-point form saturates exactly as the one-point form does.
    Accumulate(s, &clock, kHalfLife, UINT32_MAX);
    EXPECT_EQ(s.scaled, UINT32_MAX);
}

TEST(DecayTest, TouchThenReadIsReadPlusOnePoint) {
    sched::ManualClock clock(12345);
    DecayState s = Seed(3, &clock);
    clock.Advance(kHalfLife / 3);

    const std::uint32_t before = ValueAt(s, &clock, kHalfLife);
    Touch(s, &clock, kHalfLife);
    EXPECT_EQ(ValueAt(s, &clock, kHalfLife), before + kDecayScoreScale);
}

TEST(DecayTest, TouchAfterAHalfLifeStoresTheDecayedScorePlusOne) {
    sched::ManualClock clock;
    DecayState s = Seed(8, &clock);

    clock.Advance(kHalfLife);
    Touch(s, &clock, kHalfLife);  // stores 4 + 1 = 5 points, stamped now
    EXPECT_EQ(ValueAt(s, &clock, kHalfLife), 5 * kDecayScoreScale);

    // The next half-life decays from the *touch*, not from the seed.
    clock.Advance(kHalfLife);
    EXPECT_EQ(ValueAt(s, &clock, kHalfLife), 5 * kDecayScoreScale / 2);
}

TEST(DecayTest, FractionalBucketsFollowThePublicLut) {
    sched::ManualClock clock;
    DecayState s = Seed(10, &clock);  // 2560 scaled

    // Half a half-life is bucket 8 of 16: value = scaled * lut[8] >> 8.
    clock.Advance(kHalfLife / 2);
    const std::uint32_t expected =
        static_cast<std::uint32_t>((std::uint64_t{10 * kDecayScoreScale} *
                                    kDecayFractionQ8[kDecayFractionBuckets / 2]) >>
                                   8);
    EXPECT_EQ(ValueAt(s, &clock, kHalfLife), expected);
}

TEST(DecayTest, ValueNeverIncreasesAsTimeAdvances) {
    sched::ManualClock clock;
    DecayState s = Seed(1000, &clock);

    // Odd-sized steps so the walk crosses bucket and halving boundaries at
    // unaligned points — the monotonicity must not depend on alignment.
    std::uint32_t last = ValueAt(s, &clock, kHalfLife);
    for (int i = 0; i < 200; ++i) {
        clock.Advance(kHalfLife / 7 + 13);
        const std::uint32_t now = ValueAt(s, &clock, kHalfLife);
        ASSERT_LE(now, last) << "step " << i;
        last = now;
    }
}

TEST(DecayTest, ThirtyTwoHalfLivesIsZeroAndThirtyOneIsNot) {
    sched::ManualClock clock;
    DecayState s;
    s.scaled = UINT32_MAX;
    s.last_bump = clock.Now();

    clock.Advance(31 * kHalfLife);
    EXPECT_GT(ValueAt(s, &clock, kHalfLife), 0u);

    clock.Advance(kHalfLife);
    EXPECT_EQ(ValueAt(s, &clock, kHalfLife), 0u);

    // Far past the width guard: still zero, never undefined.
    clock.Advance(1000 * kHalfLife);
    EXPECT_EQ(ValueAt(s, &clock, kHalfLife), 0u);
}

TEST(DecayTest, ABackwardsClockNeverGrowsAScore) {
    sched::ManualClock clock(10 * kHalfLife);
    DecayState s = Seed(8, &clock);

    const std::uint32_t at_stamp = ValueAt(s, &clock, kHalfLife);
    clock.SetNow(5 * kHalfLife);  // before the last touch
    EXPECT_EQ(ValueAt(s, &clock, kHalfLife), at_stamp);

    // A touch under a backwards clock adds its point without decaying and
    // keeps the later stamp, so restoring the clock decays from the
    // original touch, not from the earlier reading.
    Touch(s, &clock, kHalfLife);
    clock.SetNow(11 * kHalfLife);
    EXPECT_EQ(ValueAt(s, &clock, kHalfLife), (at_stamp + kDecayScoreScale) / 2);
}

TEST(DecayTest, SaturatesAtTheCeilingInsteadOfWrapping) {
    DecayState s;
    s.scaled = UINT32_MAX - 100;  // less than one point of headroom
    Touch(s, nullptr, kHalfLife);
    EXPECT_EQ(s.scaled, UINT32_MAX);
    Touch(s, nullptr, kHalfLife);
    EXPECT_EQ(s.scaled, UINT32_MAX);
}

TEST(DecayTest, ZeroHalfLifeMeansNoDecayDefensively) {
    // The config layer refuses 0; the function must still not divide by it.
    DecayState s{4 * kDecayScoreScale, 0};
    EXPECT_EQ(DecayedScaledAt(s, 1'000'000, 0), 4 * kDecayScoreScale);
}

// ---- The log-domain read (2026-08-10) ------------------------------------

TEST(DecayTest, TheLinearReadUnderflowsAndTheLogReadDoesNot) {
    // The defect, stated as a test: Q24.8 gives a score about 16
    // half-lives of silence before it reads zero, after which no
    // comparison can order two cold things. The log read never bottoms
    // out, because decay there is a subtraction.
    sched::ManualClock clock(1);
    DecayState small;
    DecayState large;
    Accumulate(small, &clock, kHalfLife, 4);
    Accumulate(large, &clock, kHalfLife, 4096);  // 1024x the evidence

    // Live, both readable and correctly ordered.
    EXPECT_GT(ValueAt(large, &clock, kHalfLife), ValueAt(small, &clock, kHalfLife));

    clock.Advance(30 * kHalfLife);
    // Linear: both gone, and indistinguishable - the flattening that cost
    // the cabin optimizer its retirement ordering.
    EXPECT_EQ(ValueAt(small, &clock, kHalfLife), 0u);
    EXPECT_EQ(ValueAt(large, &clock, kHalfLife), 0u);

    // Log: still ordered, and by the right margin. 4096/4 is 1024, i.e.
    // exactly 10 half-lives of head start, which is what the difference
    // must come to (within the LUT's 0.78%).
    const Log2Q16 small_log = Log2ValueAt(small, &clock, kHalfLife);
    const Log2Q16 large_log = Log2ValueAt(large, &clock, kHalfLife);
    EXPECT_GT(large_log, small_log);
    const Log2Q16 gap = large_log - small_log;
    EXPECT_NEAR(static_cast<double>(gap) / 65536.0, 10.0, 0.05);

    // And the ordering holds after a silence no operator will ever wait.
    clock.Advance(10'000 * kHalfLife);
    EXPECT_GT(Log2ValueAt(large, &clock, kHalfLife), Log2ValueAt(small, &clock, kHalfLife));
}

TEST(DecayTest, TheLogReadTracksTheLinearOneWhileBothHaveResolution) {
    // The two reads are projections of one state, so where the linear one
    // still works they must agree - otherwise the consumer that consults
    // both would see a discontinuity at the crossover.
    sched::ManualClock clock(1);
    DecayState s;
    Accumulate(s, &clock, kHalfLife, 1000);

    for (int i = 0; i <= 12; ++i) {
        const std::uint32_t linear = ValueAt(s, &clock, kHalfLife);
        if (linear == 0) break;
        const double from_linear = std::log2(static_cast<double>(linear));
        const double from_log = static_cast<double>(Log2ValueAt(s, &clock, kHalfLife)) / 65536.0;
        EXPECT_NEAR(from_log, from_linear, 0.05) << "half-life " << i;
        clock.Advance(kHalfLife);
    }
}

TEST(DecayTest, AnUntouchedScoreIsColderThanEveryDecayedOne) {
    // Negative infinity has to behave like one: a state nobody ever
    // touched must sort below a state that decayed for a century, or the
    // "coldest entry" scan would pick a live one.
    sched::ManualClock clock(1);
    DecayState never;
    DecayState ancient;
    Accumulate(ancient, &clock, kHalfLife, 1);
    clock.Advance(100'000 * kHalfLife);

    EXPECT_EQ(Log2ValueAt(never, &clock, kHalfLife), kLog2NegInf);
    EXPECT_GT(Log2ValueAt(ancient, &clock, kHalfLife), kLog2NegInf);
}

TEST(DecayTest, TouchAndReadAllocateNothing) {
    sched::ManualClock clock;
    DecayState s;

    test_support::CountAllocations counter;
    for (int i = 0; i < 1000; ++i) {
        Touch(s, &clock, kHalfLife);
        clock.Advance(kHalfLife / 11);
        (void)ValueAt(s, &clock, kHalfLife);
    }
    EXPECT_EQ(counter.count(), 0u);
}

}  // namespace
}  // namespace kds::stats
