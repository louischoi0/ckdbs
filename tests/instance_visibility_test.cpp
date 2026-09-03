#include "kds/txn/instance_visibility.hpp"

#include <cstdint>

#include <gtest/gtest.h>

// AN-S1 (`instructions/v3.0.0/workorder-an-read-view.md`): the instance
// visibility state publishes commit order and a floor, and **nothing reads
// it yet** - the predicate is still the per-core `ReadView`. So these cells
// are about the structure's own contract, and the multi-core ones are
// written against two slots directly rather than against an assembled
// two-core instance, which `docs/inflight/known-gaps.md` records as untested
// ground and AN-S2 is where it becomes reachable.
//
// The two shapes worth naming, because they are the reason the floor has a
// second bound at all (AN-R8, AN-3 E's H2): ids are leased in disjoint
// per-core blocks, so a core can hold an unspent range *below* another
// core's committed ids, and a floor raised on resolution alone would answer
// "committed" for a transaction that has not started.

namespace kds::txn {
namespace {

constexpr std::uint32_t kCore0 = 0;
constexpr std::uint32_t kCore1 = 1;

TEST(InstanceVisibilityTest, FreshInstanceConstrainsNothing) {
    InstanceVisibility vis;
    EXPECT_EQ(vis.Floor(), 0u);
    EXPECT_EQ(vis.window_size(), 0u);
    // No core has attached, so nothing is known about what may still be
    // issued and the floor may not move on that absence.
    EXPECT_EQ(vis.FloorCandidate(), kUnboundedBound);
    EXPECT_EQ(vis.HorizonLsn(), kUnboundedBound);
    EXPECT_EQ(vis.Reclaim(), 0u);
    EXPECT_EQ(vis.Floor(), 0u);
}

TEST(InstanceVisibilityTest, WindowRecordsCommitLsnAndMissesEverythingElse) {
    InstanceVisibility vis;
    vis.PublishCommit(7, 400);
    EXPECT_EQ(vis.CommitLsnOf(7), 400u);
    EXPECT_EQ(vis.window_size(), 1u);
    // Uncommitted, aborted and reclaimed are one answer here; the floor is
    // what separates them, never this call.
    EXPECT_EQ(vis.CommitLsnOf(8), kNoCommitLsn);
}

TEST(InstanceVisibilityTest, IssueCursorNeverMovesBackwards) {
    InstanceVisibility vis;
    vis.PublishIssueCursor(kCore0, 9000);
    EXPECT_EQ(vis.slot(kCore0).issue_cursor.load(), 9000u);
    // A late attach or a stale caller. Taking it would lower the bound that
    // has already licensed a floor, which is the one thing it exists to
    // stop.
    vis.PublishIssueCursor(kCore0, 5000);
    EXPECT_EQ(vis.slot(kCore0).issue_cursor.load(), 9000u);
    vis.PublishIssueCursor(kCore0, 9500);
    EXPECT_EQ(vis.slot(kCore0).issue_cursor.load(), 9500u);
}

TEST(InstanceVisibilityTest, FloorRisesToTheCursorWhenNothingIsLive) {
    InstanceVisibility vis;
    vis.PublishIssueCursor(kCore0, 9000);
    vis.PublishOldestUnresolved(kCore0, kUnboundedBound);
    vis.PublishCommit(100, 10);
    vis.PublishCommit(200, 20);

    EXPECT_EQ(vis.FloorCandidate(), 9000u);
    EXPECT_EQ(vis.Reclaim(), 2u);
    EXPECT_EQ(vis.Floor(), 9000u);
    EXPECT_EQ(vis.window_size(), 0u);
}

TEST(InstanceVisibilityTest, ALiveTransactionHoldsTheFloorAtItsOwnId) {
    InstanceVisibility vis;
    vis.PublishIssueCursor(kCore0, 9000);
    vis.PublishOldestUnresolved(kCore0, 8500);
    vis.PublishCommit(8000, 10);
    vis.PublishCommit(8600, 20);

    EXPECT_EQ(vis.FloorCandidate(), 8500u);
    // Only what is below the live transaction goes.
    EXPECT_EQ(vis.Reclaim(), 1u);
    EXPECT_EQ(vis.Floor(), 8500u);
    EXPECT_EQ(vis.CommitLsnOf(8600), 20u);
    EXPECT_EQ(vis.CommitLsnOf(8000), kNoCommitLsn);
}

// AN-R8, and AN-3 E's H2 as a cell. Core 1 holds a block *below* core 0's
// and has not spent it. A floor defined on resolution alone would rise past
// that unspent range and answer "committed" for the transaction core 1 is
// about to begin.
TEST(InstanceVisibilityTest, FloorStopsAtALowerCoresUnspentRange) {
    InstanceVisibility vis;
    // Core 0 holds [8192, 12288) and has issued up to 9000; nothing live.
    vis.PublishIssueCursor(kCore0, 9000);
    vis.PublishOldestUnresolved(kCore0, kUnboundedBound);
    // Core 1 holds [4096, 8192) and has issued up to 5000; nothing live
    // either - every transaction *it has issued* is resolved.
    vis.PublishIssueCursor(kCore1, 5000);
    vis.PublishOldestUnresolved(kCore1, kUnboundedBound);

    // Resolution alone would say 9000. The cursor bound says 5000.
    EXPECT_EQ(vis.FloorCandidate(), 5000u);
    vis.Reclaim();
    EXPECT_EQ(vis.Floor(), 5000u);

    // And now the transaction that would have been wrongly visible: core 1
    // issues 5000 out of its own unspent range, which is *below* core 0's
    // cursor and above the floor - so the floor's branch never claims it.
    vis.PublishIssueCursor(kCore1, 5001);
    vis.PublishOldestUnresolved(kCore1, 5000);
    // `t < Floor()` is the branch that would have answered "committed".
    // The floor is at 5000, not above it, so it does not.
    EXPECT_LE(vis.Floor(), 5000u);
    EXPECT_EQ(vis.CommitLsnOf(5000), kNoCommitLsn);
}

TEST(InstanceVisibilityTest, FloorNeverFallsWhenALowerIdGoesLive) {
    InstanceVisibility vis;
    vis.PublishIssueCursor(kCore0, 9000);
    vis.PublishIssueCursor(kCore1, 9000);
    vis.PublishOldestUnresolved(kCore0, kUnboundedBound);
    vis.PublishOldestUnresolved(kCore1, kUnboundedBound);
    vis.Reclaim();
    ASSERT_EQ(vis.Floor(), 9000u);

    // A core beginning a transaction lowers the candidate. The floor is
    // monotone and stays - which is sound because it only reached 9000 with
    // every cursor at or above 9000, and cursors do not fall, so nothing
    // below 9000 can be issued.
    vis.PublishOldestUnresolved(kCore1, 9200);
    EXPECT_EQ(vis.FloorCandidate(), 9000u);
    vis.Reclaim();
    EXPECT_EQ(vis.Floor(), 9000u);
}

// AN-R1's reclamation rule: an entry whose commit is above the oldest live
// snapshot pins the floor at its own id, because a snapshot below that
// commit must still be told the writer had not committed.
TEST(InstanceVisibilityTest, AReaderHoldsTheFloorBelowItsSnapshot) {
    InstanceVisibility vis;
    vis.PublishIssueCursor(kCore0, 9000);
    vis.PublishOldestUnresolved(kCore0, kUnboundedBound);
    vis.PublishCommit(100, 10);
    vis.PublishCommit(200, 30);
    vis.PublishCommit(300, 50);

    // A reader on core 1 holding a snapshot at LSN 20.
    vis.PublishSnapshotBound(kCore1, 20);
    EXPECT_EQ(vis.HorizonLsn(), 20u);

    // 100 committed at 10 and may go; 200 committed at 30 pins the floor at
    // 200, so 300 stays with it.
    EXPECT_EQ(vis.Reclaim(), 1u);
    EXPECT_EQ(vis.Floor(), 200u);
    EXPECT_EQ(vis.CommitLsnOf(200), 30u);
    EXPECT_EQ(vis.CommitLsnOf(300), 50u);

    // The reader goes away and the rest follows.
    vis.PublishSnapshotBound(kCore1, kUnboundedBound);
    EXPECT_EQ(vis.Reclaim(), 2u);
    EXPECT_EQ(vis.Floor(), 9000u);
}

TEST(InstanceVisibilityTest, HorizonIsTheMinimumOverCores) {
    InstanceVisibility vis;
    EXPECT_EQ(vis.HorizonLsn(), kUnboundedBound);
    vis.PublishSnapshotBound(kCore0, 900);
    EXPECT_EQ(vis.HorizonLsn(), 900u);
    vis.PublishSnapshotBound(kCore1, 400);
    EXPECT_EQ(vis.HorizonLsn(), 400u);
    vis.PublishSnapshotBound(kCore1, kUnboundedBound);
    EXPECT_EQ(vis.HorizonLsn(), 900u);
}

// The window is not a leak: a commit stream with nothing holding the floor
// reclaims as it goes, and the amortised threshold is what keeps the pass
// off every commit.
TEST(InstanceVisibilityTest, WindowStaysBoundedUnderASteadyCommitStream) {
    InstanceVisibility vis;
    vis.PublishOldestUnresolved(kCore0, kUnboundedBound);
    for (std::uint64_t id = 1; id <= 20000; ++id) {
        vis.PublishIssueCursor(kCore0, id + 1);
        vis.PublishCommit(id, id);
    }
    EXPECT_LT(vis.window_size(), 4096u);
    EXPECT_GT(vis.Floor(), 0u);
}

}  // namespace
}  // namespace kds::txn
