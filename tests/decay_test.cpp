#include "kds/stats/decay.hpp"

#include <cstdint>

#include <gtest/gtest.h>

#include "alloc_counter.hpp"
#include "kds/sched/clock.hpp"

// The lazy-decay score (docs/feat-physical-optimizer.md R1, workplan PX02).
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
