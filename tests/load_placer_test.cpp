#include "kds/server/load_placer.hpp"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "kds/sched/core_load.hpp"
#include "kds/server/session.hpp"

// **BL-S3: the balancer's decision** (BL-R4, `load_placer.hpp`). `Decide` is
// the seam a cell drives with fixed words; the last cell drives `Place`
// itself over `sched::CoreLoad`'s slots, for the bookkeeping `Decide` is
// handed rather than keeps. Each restraint has the cell that kills its
// removal.

namespace kds::server {
namespace {

using View = LoadPlacer::CoreView;
constexpr std::uint64_t kNow = 100'000'000'000;  // 100 s into the clock
constexpr std::uint64_t kNever = 0;              // a session that never moved
constexpr std::uint32_t kM = LoadPlacer::kMarginPpm;

// Every word fresh, as of `kNow`.
View At(std::uint32_t busy, std::uint32_t active) { return {busy, active, kNow}; }

// **The mutation**: `>=` for `>` and the exact-margin gap moves.
TEST(LoadPlacerTest, AGapPastTheMarginMovesAndOneAtItStays) {
    const std::array<View, 3> wide = {At(kM + 300'001, 8), At(300'000, 8), At(600'000, 8)};
    EXPECT_EQ(LoadPlacer::Decide(wide, 0, kNow, kNever, false), 1u) << "core 1 is the least busy";
    const std::array<View, 3> at = {At(kM + 300'000, 8), At(300'000, 8), At(600'000, 8)};
    EXPECT_EQ(LoadPlacer::Decide(at, 0, kNow, kNever, false), 0u) << "a gap of exactly the margin";
    const std::array<View, 2> calm = {At(100'000, 8), At(900'000, 8)};
    EXPECT_EQ(LoadPlacer::Decide(calm, 0, kNow, kNever, false), 0u) << "toward a busier core";
}

// **A move must shrink the gap**: one session of share `s` turns a gap `g`
// into `|g - 2s|`, smaller exactly when `s < g`. A lone session is its
// core's whole busy fraction and stays; two on one core and none on the
// other balance, as do three against one.
//
// **The mutations**: drop the share test and the lone session moves;
// require `2 * share < gap` and neither split balances.
TEST(LoadPlacerTest, AMoveIsTakenOnlyWhereItShrinksTheGap) {
    const std::array<View, 2> lone = {At(600'000, 1), At(10'000, 0)};
    EXPECT_EQ(LoadPlacer::Decide(lone, 0, kNow, kNever, false), 0u) << "the lone session moved";
    const std::array<View, 2> two_to_none = {At(900'000, 2), At(0, 0)};
    EXPECT_EQ(LoadPlacer::Decide(two_to_none, 0, kNow, kNever, false), 1u);
    const std::array<View, 2> three_to_one = {At(900'000, 3), At(300'000, 1)};
    EXPECT_EQ(LoadPlacer::Decide(three_to_one, 0, kNow, kNever, false), 1u);
}

// **A core inside a long hold publishes nothing**, and its last word would
// draw sessions into the hold.
//
// **The mutation**: drop the staleness test and the stalled core is chosen.
TEST(LoadPlacerTest, ACoreWhoseWordIsStaleIsNotChosen) {
    const std::uint64_t edge = kNow - LoadPlacer::kStaleNs;
    const std::array<View, 3> fresh = {At(900'000, 8), {0, 0, edge}, At(400'000, 4)};
    EXPECT_EQ(LoadPlacer::Decide(fresh, 0, kNow, kNever, false), 1u) << "a word exactly kStaleNs old";
    const std::array<View, 3> stale = {At(900'000, 8), {0, 0, edge - 1}, At(400'000, 4)};
    EXPECT_EQ(LoadPlacer::Decide(stale, 0, kNow, kNever, false), 2u)
        << "a core held past kStaleNs drew the session";
}

// **A session that moved is not reconsidered within its dwell.**
//
// **The mutation**: ignore `moved_at_ns` and the session moves back at once.
TEST(LoadPlacerTest, ASessionInsideItsDwellStays) {
    const std::array<View, 2> busy = {At(900'000, 8), At(0, 0)};
    EXPECT_EQ(LoadPlacer::Decide(busy, 0, kNow, kNow - LoadPlacer::kDwellNs + 1, false), 0u);
    EXPECT_EQ(LoadPlacer::Decide(busy, 0, kNow, kNow - LoadPlacer::kDwellNs, false), 1u);
    // A move time later than this core's window end - another core's
    // window ran ahead - is inside the dwell, not a wrap past it.
    EXPECT_EQ(LoadPlacer::Decide(busy, 0, kNow, kNow + 1, false), 0u);
}

// **`Place` keeps the guard and the dwell `Decide` is handed**: one session
// leaves a busy core per window, and the one that left carries its move
// time. Over the process-wide slots, restored after.
//
// **The mutations**: drop `moved_out_in`'s store and the second session
// leaves in the same window; drop `set_moved_at_ns` and the first is
// eligible again at once.
TEST(LoadPlacerTest, PlaceLetsOneSessionLeaveACorePerWindowAndStampsIt) {
    sched::CoreLoad::SetCores(2);
    sched::CoreLoad::Reset(0);
    sched::CoreLoad::Reset(1);
    sched::CoreLoad::Slot& busy = sched::CoreLoad::Of(0);
    sched::CoreLoad::Slot& idle = sched::CoreLoad::Of(1);
    busy.busy_ppm.store(900'000);
    busy.active_sessions.store(4);
    busy.window_end_ns.store(kNow);
    idle.window_end_ns.store(kNow);

    LoadPlacer placer;
    Session first;
    Session second;
    EXPECT_EQ(placer.Place(0, first), 1u);
    EXPECT_EQ(first.moved_at_ns(), kNow) << "the move left no time behind";
    EXPECT_EQ(placer.Place(0, second), 0u) << "two sessions left one core in one window";
    EXPECT_EQ(sched::CoreLoad::Of(1).busy_ppm.load(), 0u);

    // The next window: the first is inside its dwell, and the second may go.
    busy.window_end_ns.store(kNow + sched::CoreLoad::kWindowNs);
    idle.window_end_ns.store(kNow + sched::CoreLoad::kWindowNs);
    EXPECT_EQ(placer.Place(0, first), 0u) << "a session that just moved moved again";
    EXPECT_EQ(placer.Place(0, second), 1u);

    sched::CoreLoad::Reset(0);
    sched::CoreLoad::Reset(1);
    sched::CoreLoad::SetCores(0);
}

// ---- BL-S4: the partition (BL-R2, BL-R4) ------------------------------------

using Side = LoadPlacer::Side;

// `max(1, cores / 4)` batch cores, core 0 first and then the
// highest-numbered (ruling 2 (a)); none at one core.
TEST(LoadPlacerTest, CoreZeroIsTheFirstBatchCoreAndTheRestAreTheHighest) {
    EXPECT_FALSE(LoadPlacer::IsBatchCore(0, 1)) << "a partition at one core";
    EXPECT_TRUE(LoadPlacer::IsBatchCore(0, 2));
    EXPECT_FALSE(LoadPlacer::IsBatchCore(1, 2));
    for (std::uint32_t k = 0; k < 4; ++k) EXPECT_EQ(LoadPlacer::IsBatchCore(k, 4), k == 0) << k;
    for (std::uint32_t k = 0; k < 8; ++k) {
        EXPECT_EQ(LoadPlacer::IsBatchCore(k, 8), k == 0 || k == 7) << k;
    }
    for (std::uint32_t k = 0; k < 16; ++k) {
        EXPECT_EQ(LoadPlacer::IsBatchCore(k, 16), k == 0 || k >= 13) << k;
    }
}

// **A session outside its side moves at its head, whatever the load says**:
// no margin, no dwell, no per-window guard - the first boundary after it
// declares a level, or after the kernel's hash put it on the other side.
//
// **The mutation**: drop the partition filter (`inside`) and neither
// session moves - the dwell and the per-window guard hold it.
TEST(LoadPlacerTest, ASessionOutsideItsSideMovesWithNoMarginAndNoDwell) {
    const std::array<View, 4> even = {At(500'000, 2), At(500'000, 2), At(500'000, 2),
                                      At(500'000, 2)};
    // An interactive session on core 0, the batch core: to the least busy of
    // 1..3, inside its dwell and after another session left this window.
    std::array<View, 4> skew = even;
    skew[2].busy_ppm = 100'000;
    EXPECT_EQ(LoadPlacer::Decide(skew, 0, kNow, kNow, true, Side::kInteractive), 2u);
    EXPECT_EQ(LoadPlacer::Decide(even, 0, kNow, kNow, true, Side::kInteractive), 1u)
        << "an interactive session stayed on the batch core";
    // A batch session on an interactive core: to core 0, the only batch core.
    for (std::uint32_t k = 1; k < 4; ++k) {
        EXPECT_EQ(LoadPlacer::Decide(even, k, kNow, kNow, true, Side::kBatch), 0u) << k;
    }
}

// **A load move never leaves the side**, and inside it the margin and the
// dwell still hold.
//
// **The mutation**: drop `inside` from the load loop and the interactive
// session on core 1 moves to core 0, the idle batch core.
TEST(LoadPlacerTest, InsideItsSideALoadMoveKeepsTheMarginAndTheDwell) {
    const std::array<View, 4> idle_batch = {At(0, 0), At(900'000, 4), At(800'000, 4),
                                            At(800'000, 4)};
    EXPECT_EQ(LoadPlacer::Decide(idle_batch, 1, kNow, kNever, false, Side::kInteractive), 1u)
        << "a load move left the interactive side";
    EXPECT_EQ(LoadPlacer::Decide(idle_batch, 1, kNow, kNever, false, Side::kAny), 0u)
        << "with no partition the idle core is every session's";
    const std::array<View, 4> wide = {At(0, 0), At(kM + 300'001, 4), At(300'000, 4),
                                      At(900'000, 4)};
    EXPECT_EQ(LoadPlacer::Decide(wide, 1, kNow, kNever, false, Side::kInteractive), 2u);
    EXPECT_EQ(LoadPlacer::Decide(wide, 1, kNow, kNow, false, Side::kInteractive), 1u)
        << "a load move inside the side ignored the dwell";
    const std::array<View, 4> at = {At(0, 0), At(kM + 300'000, 4), At(300'000, 4),
                                    At(900'000, 4)};
    EXPECT_EQ(LoadPlacer::Decide(at, 1, kNow, kNever, false, Side::kInteractive), 1u)
        << "a gap of exactly the margin moved";
}

// A side whose every word is stale still takes the session: the partition is
// never given up for want of a fresh word, and a fresh core is preferred.
TEST(LoadPlacerTest, APartitionMovePrefersAFreshCoreAndTakesAStaleOne) {
    const std::uint64_t stale = kNow - LoadPlacer::kStaleNs - 1;
    std::array<View, 4> words = {At(500'000, 2), View{0, 0, stale}, View{0, 0, stale},
                                 At(700'000, 2)};
    EXPECT_EQ(LoadPlacer::Decide(words, 0, kNow, kNever, false, Side::kInteractive), 3u)
        << "a stale core was chosen over a fresh one";
    words[3].window_end_ns = stale;
    EXPECT_EQ(LoadPlacer::Decide(words, 0, kNow, kNever, false, Side::kInteractive), 1u)
        << "not the side's least-busy core when every word on it is stale";
}

// **`Place` reads the partition from the instance's batch count and the
// session's level**, and counts a partition move apart. Over the
// process-wide slots, restored after.
//
// **The mutations**: read the partition as always active and the `normal`
// session leaves core 0 with no batch session live; drop the count's
// increment in `PrioritySeat::set` and the `normal` session stays on core 0
// with one live.
TEST(LoadPlacerTest, PlaceActivatesThePartitionOnlyWhileABatchSessionLives) {
    sched::CoreLoad::SetCores(4);
    for (std::uint32_t k = 0; k < 4; ++k) {
        sched::CoreLoad::Reset(k);
        sched::CoreLoad::Of(k).busy_ppm.store(500'000);
        sched::CoreLoad::Of(k).active_sessions.store(2);
        sched::CoreLoad::Of(k).window_end_ns.store(kNow);
    }
    const std::uint32_t before = sched::CoreLoad::BatchSessions().load();
    ASSERT_EQ(before, 0u) << "a batch-level session outlived its test";
    LoadPlacer placer;
    Session plain;
    EXPECT_EQ(placer.Place(0, plain), 0u) << "the partition was active with no batch session";
    {
        Session analyst;
        analyst.set_priority(Priority::kAnalytic);
        EXPECT_EQ(sched::CoreLoad::BatchSessions().load(), before + 1);
        EXPECT_EQ(placer.Place(2, analyst), 0u) << "the batch session was not moved to core 0";
        EXPECT_EQ(sched::CoreLoad::Of(2).partition_moves_out.load(), 1u);
        // The next window, so the per-window guard is not what keeps it.
        sched::CoreLoad::Of(0).window_end_ns.store(kNow + sched::CoreLoad::kWindowNs);
        EXPECT_NE(placer.Place(0, plain), 0u) << "a normal session stayed on the batch core";
        EXPECT_EQ(sched::CoreLoad::Of(0).partition_moves_out.load(), 1u);
        Session moved(std::move(analyst));
        EXPECT_EQ(moved.priority(), Priority::kAnalytic);
        EXPECT_EQ(analyst.priority(), Priority::kNormal);  // NOLINT(bugprone-use-after-move)
        EXPECT_EQ(sched::CoreLoad::BatchSessions().load(), before + 1) << "a move counted twice";
    }
    EXPECT_EQ(sched::CoreLoad::BatchSessions().load(), before)
        << "an ended batch session still counts";
    Session switcher;
    switcher.set_priority(Priority::kBackground);
    switcher.set_priority(Priority::kAnalytic);
    EXPECT_EQ(sched::CoreLoad::BatchSessions().load(), before + 1) << "batch to batch counted";
    switcher.set_priority(Priority::kTransactional);
    EXPECT_EQ(sched::CoreLoad::BatchSessions().load(), before);

    // **After the lapse a former batch core is every session's again**: a
    // `normal` session on a busy core moves by load to an idle core 0.
    for (std::uint32_t k = 0; k < 4; ++k) {
        sched::CoreLoad::Of(k).window_end_ns.store(kNow + 4 * sched::CoreLoad::kWindowNs);
    }
    sched::CoreLoad::Of(0).busy_ppm.store(0);
    sched::CoreLoad::Of(1).busy_ppm.store(900'000);
    sched::CoreLoad::Of(1).active_sessions.store(4);
    Session spreader;
    EXPECT_EQ(placer.Place(1, spreader), 0u) << "the lapsed partition still kept core 0 from load";

    for (std::uint32_t k = 0; k < 4; ++k) sched::CoreLoad::Reset(k);
    sched::CoreLoad::SetCores(0);
}

}  // namespace
}  // namespace kds::server
