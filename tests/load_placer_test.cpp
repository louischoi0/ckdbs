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

}  // namespace
}  // namespace kds::server
