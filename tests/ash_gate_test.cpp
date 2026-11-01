#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "kds/sched/ash_sampler.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/sched/wait_state.hpp"

// CJ's G2 gate (`instructions/v3.2.0/workorder-cj-wait-events-ash.md`): the
// sampled share against an injected one, determinism and volatility, on a
// logical clock. The overhead row is the milestone-close measurement.

#ifndef KDS_WAIT_INSTRUMENTATION_OFF

namespace kds::sched {
namespace {

using stats::WaitEvent;

constexpr MonoTimeNs kStep = 100'000;          // 0.1 ms per turn
constexpr MonoTimeNs kInterval = 1'000'000;    // one sample per ms
constexpr int kSamples = 12'000;               // >= 10,000 samples (G2)

// A task that spends its life alternating between two waits; the driver
// decides when each ends by raising `*go`.
Coro Alternating(bool* go) {
    for (;;) {
        {
            WaitScope w(WaitEvent::kCommitWalFlush, 1, 0, 0);
            co_await WaitFor{go};
        }
        *go = false;
        {
            WaitScope w(WaitEvent::kIoPageRead, 2, 0, 0);
            co_await WaitFor{go};
        }
        *go = false;
    }
}

struct Outcome {
    std::vector<stats::AshRecord> records;
    std::uint64_t truth_wal_steps = 0;  // turns the task really spent in commit.wal_flush
    std::uint64_t truth_steps = 0;
    std::vector<bool> wal_at_sample;  // the true state at each grid point
};

// A seeded schedule of phase lengths (in turns), run on a logical clock.
Outcome RunScenario(std::uint64_t seed) {
    ResetWaitContext();
    ManualClock clock(0);
    SetWaitClock(&clock);
    NullIoBackend io;
    Scheduler scheduler(clock, io);
    scheduler.ash().set_interval_ns(kInterval);
    bool go = false;
    scheduler.Submit(std::make_unique<CoroTask>(SchedulingGroup::kForeground, Alternating(&go)));
    scheduler.RunOnce();  // arms the grid and parks the task in its first wait

    std::uint64_t x = seed * 6364136223846793005ull + 1442695040888963407ull;
    const auto next = [&] {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        return x >> 33;
    };
    Outcome out;
    bool in_wal = true;
    std::uint64_t left = 10 + next() % 40;  // turns left in the current phase
    const std::uint64_t total = static_cast<std::uint64_t>(kSamples) * (kInterval / kStep);
    for (std::uint64_t t = 0; t < total; ++t) {
        clock.Advance(kStep);
        // The step just elapsed was spent in the state the task parked in last
        // turn; it is booked before this turn's flip, which the task only acts
        // on after this turn's sample (phase 3.9 precedes phase 4).
        ++out.truth_steps;
        out.truth_wal_steps += in_wal ? 1 : 0;
        if ((t + 1) % (kInterval / kStep) == 0) out.wal_at_sample.push_back(in_wal);
        if (left-- == 0) {
            go = true;  // ends the current wait at this turn
            in_wal = !in_wal;
            left = in_wal ? 10 + next() % 40 : 10 + next() % 100;
        }
        scheduler.RunOnce();
    }
    out.records = scheduler.ash().ring()->Snapshot();
    SetWaitClock(nullptr);
    ResetWaitContext();
    return out;
}

TEST(AshGate, TheSampledShareMatchesTheInjectedShareWithinThreePoints) {
    const Outcome o = RunScenario(7);
    // Exactly one record per grid point: a drifting grid, a skipped or a
    // doubled batch is a wrong count, not a share.
    ASSERT_EQ(o.records.size(), static_cast<std::size_t>(kSamples));
    ASSERT_EQ(o.wal_at_sample.size(), o.records.size());
    std::uint64_t wal = 0;
    for (std::size_t i = 0; i < o.records.size(); ++i) {
        const bool is_wal = o.records[i].event_id == static_cast<std::uint16_t>(WaitEvent::kCommitWalFlush);
        wal += is_wal;
        // Turn for turn, not only in sum: a sampler returning a stale state
        // would keep the share and fail here.
        ASSERT_EQ(is_wal, static_cast<bool>(o.wal_at_sample[i])) << "sample " << i;
    }
    const double sampled = 100.0 * static_cast<double>(wal) / static_cast<double>(o.records.size());
    const double injected = 100.0 * static_cast<double>(o.truth_wal_steps) / static_cast<double>(o.truth_steps);
    EXPECT_NEAR(sampled, injected, 3.0) << "sampled " << sampled << "% against injected " << injected << "%";
    EXPECT_GT(injected, 10.0);
    EXPECT_LT(injected, 90.0) << "a share worth telling apart from all-or-nothing";
}

TEST(AshGate, TheSameSeedAndClockGiveByteIdenticalSamples) {
    const Outcome a = RunScenario(11);
    const Outcome b = RunScenario(11);
    ASSERT_EQ(a.records.size(), static_cast<std::size_t>(kSamples)) << "not vacuous";
    ASSERT_EQ(a.records.size(), b.records.size());
    for (std::size_t i = 0; i < a.records.size(); ++i) {
        ASSERT_EQ(stats::EncodeAsh(a.records[i]), stats::EncodeAsh(b.records[i])) << "record " << i;
    }
    // The samples, not only the test's own schedule, follow the seed.
    const Outcome c = RunScenario(12);
    ASSERT_EQ(c.records.size(), a.records.size());
    bool differs = false;
    for (std::size_t i = 0; i < a.records.size() && !differs; ++i) {
        differs = stats::EncodeAsh(a.records[i]) != stats::EncodeAsh(c.records[i]);
    }
    EXPECT_TRUE(differs) << "another seed gives other samples";
}

TEST(AshGate, ARestartedCoreStartsWithAnEmptyRing) {
    // Memory only (WD8, I5): a new scheduler for a core id holds no sample of
    // the one before it, and the dead one's ring is gone from the registry.
    ResetWaitContext();
    ManualClock clock(0);
    SetWaitClock(&clock);
    NullIoBackend io;
    {
        Scheduler first(clock, io);
        first.ash().set_core_id(57);  // adopts the process defaults; pin what this test needs
        first.ash().set_enabled(true);
        first.ash().set_interval_ns(kInterval);
        bool never = false;
        first.Submit(std::make_unique<CoroTask>(SchedulingGroup::kForeground,
                                                [](bool* n) -> Coro {
                                                    WaitScope w(WaitEvent::kIoPageRead, 0, 0, 0);
                                                    co_await WaitFor{n};
                                                    co_return Status::OK();
                                                }(&never)));
        first.RunOnce();
        clock.Advance(kInterval);
        first.RunOnce();
        ASSERT_NE(AshRingOfCore(57), nullptr);
        ASSERT_EQ(AshRingOfCore(57)->Snapshot().size(), 1u);
    }
    EXPECT_EQ(AshRingOfCore(57), nullptr);
    Scheduler second(clock, io);
    second.ash().set_core_id(57);
    ASSERT_NE(AshRingOfCore(57), nullptr);
    EXPECT_TRUE(AshRingOfCore(57)->Snapshot().empty());
    SetWaitClock(nullptr);
    ResetWaitContext();
}

}  // namespace
}  // namespace kds::sched

#endif
