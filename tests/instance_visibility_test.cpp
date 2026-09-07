#include "kds/txn/instance_visibility.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"

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
    EXPECT_EQ(vis.LookupCommit(7).commit_lsn, 400u);
    EXPECT_EQ(vis.window_size(), 1u);
    // Uncommitted, aborted and reclaimed are one answer here; the floor is
    // what separates them, never this call.
    EXPECT_EQ(vis.LookupCommit(8).commit_lsn, kNoCommitLsn);
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
    EXPECT_EQ(vis.LookupCommit(8600).commit_lsn, 20u);
    EXPECT_EQ(vis.LookupCommit(8000).commit_lsn, kNoCommitLsn);
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
    //
    // **In the order `TransactionManager::PublishCoreBounds` uses**: the
    // bound that moves down first, the cursor second, with a reclamation
    // between them. That intermediate state is the one a concurrent core
    // can observe, and reclaiming from it must not license a floor above a
    // live id. Publishing the cursor first fails here at 5001.
    vis.PublishOldestUnresolved(kCore1, 5000);
    ASSERT_EQ(vis.Reclaim(), 0u);
    EXPECT_LE(vis.Floor(), 5000u) << "reclaimed mid-publication and passed a live transaction";
    vis.PublishIssueCursor(kCore1, 5001);
    vis.Reclaim();
    // `t < Floor()` is the branch that would have answered "committed".
    // The floor is at 5000, not above it, so it does not.
    EXPECT_LE(vis.Floor(), 5000u);
    EXPECT_EQ(vis.LookupCommit(5000).commit_lsn, kNoCommitLsn);
}

TEST(InstanceVisibilityTest, FloorNeverFallsWhenTheCandidateDoes) {
    InstanceVisibility vis;
    vis.PublishIssueCursor(kCore0, 9000);
    vis.PublishIssueCursor(kCore1, 9000);
    vis.PublishOldestUnresolved(kCore0, kUnboundedBound);
    vis.PublishOldestUnresolved(kCore1, kUnboundedBound);
    vis.PublishCommit(8000, 10);
    vis.Reclaim();
    ASSERT_EQ(vis.Floor(), 9000u);

    // A transaction beginning above the floor lowers the candidate to its
    // own id and no further; the floor is already below it and stays.
    vis.PublishOldestUnresolved(kCore1, 9200);
    EXPECT_EQ(vis.FloorCandidate(), 9000u);
    vis.Reclaim();
    EXPECT_EQ(vis.Floor(), 9000u);

    // The one state that pulls the candidate *below* the floor, and the
    // only reason the raise is a CAS: a third core attaching with a first
    // cursor under it. The floor may not follow it down - an entry the
    // floor has already licensed must stay licensed - and the wiring is
    // what keeps this from happening (a core's sequence opens at the
    // superblock's `next_trx_id`).
    vis.PublishIssueCursor(/*core=*/2, 4000);
    EXPECT_EQ(vis.FloorCandidate(), 4000u);
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
    EXPECT_EQ(vis.LookupCommit(200).commit_lsn, 30u);
    EXPECT_EQ(vis.LookupCommit(300).commit_lsn, 50u);

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

// ---- The publication points (AN-R2) ---------------------------------------
//
// The four cells AN-S1 states of `TransactionManager` rather than of the
// structure: what a core publishes when it attaches, when it begins, when it
// commits and when it aborts. One core, a real WAL over a memory log device,
// because "the LSN `Commit` returned" is the whole content of a window entry
// and the unlogged path has none to record.

class VisibilityWiringTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto log_device = wal::MemoryLogDevice::Create(/*segment_size=*/1 << 20);
        ASSERT_TRUE(log_device.ok()) << log_device.status().message();
        log_device_ = std::move(log_device.value());
        auto wal = wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/0);
        ASSERT_TRUE(wal.ok()) << wal.status().message();
        wal_ = std::move(wal.value());

        superblock_ = server::SuperBlock::CreateFresh(/*now_unix_seconds=*/1000);
        ids_ = std::make_unique<TrxIdSequence>(superblock_);
        undo_ = std::make_unique<UndoLog>(store_, wal_.get());
        // **A second core, as far as visibility can tell one apart**: its
        // own sequence over the same superblock, so the first id it issues
        // carves a block *above* whatever core 0 has carved - the disjoint
        // per-core blocks AN-3 E's H1 and H2 rest on - its own undo log,
        // and the same store, stream and visibility. What it is not is a
        // second reactor: both managers run on this thread, which is
        // enough for cells about what a view answers and not for cells
        // about who runs when (those are the two-core rig's, AV).
        ids1_ = std::make_unique<TrxIdSequence>(superblock_);
        undo1_ = std::make_unique<UndoLog>(store_, wal_.get());
    }

    std::unique_ptr<TransactionManager> Attach() {
        return std::make_unique<TransactionManager>(*ids_, *undo_, store_, wal_.get(), &vis_,
                                                    kCore0);
    }

    std::unique_ptr<TransactionManager> AttachPeer() {
        return std::make_unique<TransactionManager>(*ids1_, *undo1_, store_, wal_.get(), &vis_,
                                                    kCore1);
    }

    // Begin-and-commit one transaction, which on a fresh sequence is what
    // carves that core's first block. Returns the id it spent.
    static std::uint64_t CommitOne(TransactionManager& mgr) {
        auto txn = mgr.Begin(IsolationLevel::kReadCommitted);
        EXPECT_TRUE(txn.ok()) << txn.status().message();
        if (!txn.ok()) return 0;
        const std::uint64_t id = txn.value()->id();
        EXPECT_TRUE(mgr.Commit(*txn.value(), wal::DurabilityClass::kRelaxed).ok());
        mgr.Release(*txn.value());
        return id;
    }

    sched::ManualClock clock_;
    std::unique_ptr<wal::MemoryLogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;
    storage::InMemoryPageStore store_{128};
    server::SuperBlock superblock_;
    InstanceVisibility vis_;
    std::unique_ptr<TrxIdSequence> ids_;
    std::unique_ptr<UndoLog> undo_;
    std::unique_ptr<TrxIdSequence> ids1_;
    std::unique_ptr<UndoLog> undo1_;
};

// ---- AN-S2: the two shapes the per-core view cannot get right -------------
//
// Written **before** the cutover and run against it, because the AN-S2 row
// asks for exactly that: both must fail on the trx-id predicate and pass on
// the commit-LSN one. Neither is a fixture artefact - each core issues from
// its own block, as a real peer does, and that is the whole cause.

// AN-3 E's H1. Core 1 holds the higher block. It commits, and core 0's
// *next* view - minted after that commit - must see the row. Under a bound
// on trx ids it does not: the committed id is above core 0's cursor and so
// reads as "not yet started", until core 0 burns through its own block.
TEST_F(VisibilityWiringTest, ACommitOnAHigherBlockIsVisibleToALowerCoresNextView) {
    auto core0 = Attach();
    auto core1 = AttachPeer();
    // Core 0 carves the low block and spends one id: its cursor is low and
    // most of its block is unspent.
    CommitOne(*core0);
    ASSERT_GT(ids_->remaining(), 0u);
    // Core 1 carves the high block and commits from it.
    const std::uint64_t committed = CommitOne(*core1);
    ASSERT_GT(committed, ids_->peek()) << "the fixture did not put core 1's block above core 0's";

    const ReadView view = core0->MintReadView(kNoTrxId);
    EXPECT_TRUE(view.Visible(committed))
        << "H1: a transaction committed on a core holding a higher id block is invisible to "
           "the next view minted on a lower core";
}

// AN-3 E's H2. Core 1 holds the higher block and mints a view. Core 0 then
// begins a transaction out of its own unspent range, *below* that view's
// cursor and in no in-flight set the view could have taken. A bound on trx
// ids answers "committed" for a transaction that had not started when the
// view was taken - a dirty read - and keeps answering it after the commit,
// which a pinned view must never do.
TEST_F(VisibilityWiringTest, ATransactionBegunAfterTheMintFromALowerBlockStaysInvisible) {
    auto core0 = Attach();
    auto core1 = AttachPeer();
    CommitOne(*core0);
    CommitOne(*core1);
    ASSERT_GT(ids1_->peek(), ids_->peek());

    const ReadView pinned = core1->MintReadView(kNoTrxId);

    auto late = core0->Begin(IsolationLevel::kReadCommitted);
    ASSERT_TRUE(late.ok()) << late.status().message();
    const std::uint64_t late_id = late.value()->id();
    ASSERT_LT(late_id, ids1_->peek()) << "the fixture did not issue from the lower block";
    EXPECT_FALSE(pinned.Visible(late_id))
        << "H2: a transaction begun after the mint, from a lower core's unspent range, reads "
           "as committed to the pinned view";

    ASSERT_TRUE(core0->Commit(*late.value(), wal::DurabilityClass::kRelaxed).ok());
    core0->Release(*late.value());
    EXPECT_FALSE(pinned.Visible(late_id))
        << "the view is pinned: a commit after the mint stays invisible to it";
    // And a view minted now sees it, on either core.
    EXPECT_TRUE(core1->MintReadView(kNoTrxId).Visible(late_id));
    EXPECT_TRUE(core0->MintReadView(kNoTrxId).Visible(late_id));
}

// A view minted on core 1 while core 0's transaction is live does not see
// it - before its commit, and after, because the view is pinned. A READ
// COMMITTED transaction's next statement does, because that is the one
// event that re-mints.
TEST_F(VisibilityWiringTest, AViewMintedBesideAnotherCoresLiveTransactionStaysBlindToIt) {
    auto core0 = Attach();
    auto core1 = AttachPeer();
    CommitOne(*core0);
    CommitOne(*core1);

    auto live = core0->Begin(IsolationLevel::kReadCommitted);
    ASSERT_TRUE(live.ok()) << live.status().message();
    const std::uint64_t live_id = live.value()->id();

    auto reader = core1->Begin(IsolationLevel::kReadCommitted);
    ASSERT_TRUE(reader.ok()) << reader.status().message();
    const ReadView pinned = reader.value()->view();
    EXPECT_FALSE(pinned.Visible(live_id)) << "live on another core";
    EXPECT_FALSE(pinned.in_flight_at_mint)
        << "the Cabin's bit is this core's: core 1 had nothing else live";

    ASSERT_TRUE(core0->Commit(*live.value(), wal::DurabilityClass::kRelaxed).ok());
    core0->Release(*live.value());
    EXPECT_FALSE(pinned.Visible(live_id)) << "committed after the mint; the view is pinned";
    EXPECT_FALSE(reader.value()->view().Visible(live_id))
        << "the transaction's current view is the pinned one until a statement boundary";

    ASSERT_TRUE(core1->StartStatement(*reader.value()).ok());
    EXPECT_TRUE(reader.value()->view().Visible(live_id))
        << "READ COMMITTED: the next statement sees what committed before it";
    ASSERT_TRUE(core1->Commit(*reader.value(), wal::DurabilityClass::kRelaxed).ok());
    core1->Release(*reader.value());
}

// **AN-R9, and the interval AN-Q3 names, at the structure.** Core 0 has
// begun a commit - its LSN is fixed, its entry is not yet in - when core 1
// publishes a commit at a *higher* LSN. The published maximum is now above
// core 0's unpublished commit; a snapshot that took it would answer core
// 0's commit invisible now and visible once its entry lands. The pending
// marker caps the ceiling instead, so no snapshot covers an LSN whose
// entry it cannot see, and every view's answer is stable for its life.
//
// **Mutation**: read the published maximum at the mint instead of the
// capped ceiling, and `early.Visible(11)` flips from false to true when
// core 0 publishes.
TEST(InstanceVisibilityTest, ASnapshotNeverCoversACommitWhoseEntryIsNotYetPublished) {
    InstanceVisibility vis;
    vis.PublishCommit(10, 100);
    ASSERT_EQ(vis.SnapshotCeiling(), 100u);

    // Core 0 is between its append and its publication.
    vis.BeginCommit(kCore0);
    // Core 1 appends after core 0 - a higher LSN - and publishes first.
    vis.PublishCommit(20, 200);
    EXPECT_EQ(vis.CommitCeiling(), 200u) << "the published maximum moved";
    EXPECT_EQ(vis.SnapshotCeiling(), 100u) << "the ceiling a mint takes did not";

    ReadView early;
    early.visibility = &vis;
    early.snapshot_lsn = vis.SnapshotCeiling();
    EXPECT_FALSE(early.Visible(20)) << "committed above the snapshot";
    EXPECT_FALSE(early.Visible(11)) << "not yet published";

    // Core 0's commit lands, at the LSN it was assigned before core 1's.
    vis.PublishCommit(11, 150);
    vis.EndCommit(kCore0);
    EXPECT_EQ(vis.SnapshotCeiling(), 200u) << "the cap lifted with the publication";
    EXPECT_FALSE(early.Visible(11)) << "the early view's answer must not flip";
    EXPECT_FALSE(early.Visible(20));

    ReadView later;
    later.visibility = &vis;
    later.snapshot_lsn = vis.SnapshotCeiling();
    EXPECT_TRUE(later.Visible(11));
    EXPECT_TRUE(later.Visible(20));
    // Monotone: a later mint never reads below an earlier one.
    EXPECT_GE(later.snapshot_lsn, early.snapshot_lsn);
}

// **The review's C2**: a pass is bounded by the pending-commit markers as
// well as by the horizon. Core 0 has begun a commit with the ceiling at
// 40; core 1 publishes at 50; a pass with no reader anywhere must not drop
// core 1's entry, because a mint capped by core 0's marker takes 40 and
// would then find 50 committed by the floor.
//
// **Mutation**: bound the pass by the horizon alone and the entry goes.
TEST(InstanceVisibilityTest, AReclamationPassIsCappedByAPendingCommit) {
    InstanceVisibility vis;
    vis.PublishBounds(kCore0, kUnboundedBound, /*cursor=*/1000);
    vis.PublishCommit(10, 40);
    vis.BeginCommit(kCore0);
    vis.PublishCommit(20, 50);

    EXPECT_EQ(vis.Reclaim(), 1u) << "10 committed at 40 is below the marker and may go";
    EXPECT_EQ(vis.LookupCommit(20).commit_lsn, 50u) << "20 committed at 50 must stay";
    EXPECT_LE(vis.Floor(), 20u);
    ReadView capped;
    capped.visibility = &vis;
    capped.snapshot_lsn = vis.SnapshotCeiling();
    ASSERT_EQ(capped.snapshot_lsn, 40u);
    EXPECT_FALSE(capped.Visible(20));

    // The marker lifts. The capped view is now what holds the entry, and it
    // does so the way any held view does - through its core's slot, which
    // a real mint lowers before it reads the ceiling (the manager's half,
    // `AHeldMintLowersTheSlotBeforeItIsPublished`).
    vis.EndCommit(kCore0);
    vis.PublishSnapshotBound(kCore1, capped.snapshot_lsn);
    EXPECT_EQ(vis.Reclaim(), 0u);
    EXPECT_FALSE(capped.Visible(20)) << "still not: the view was minted at 40";
    vis.PublishSnapshotBound(kCore1, kUnboundedBound);
    EXPECT_EQ(vis.Reclaim(), 1u);
    EXPECT_GT(vis.Floor(), 20u);
}

// **The review's C1**: a mint that has read its ceiling and not yet
// published is a reader a pass cannot see - unless the mint lowered its
// core's slot first. The structure's half: a lowered slot bounds a pass
// exactly as a published one does.
TEST(InstanceVisibilityTest, ALoweredSlotBoundsAPassLikeAPublishedSnapshot) {
    InstanceVisibility vis;
    vis.PublishBounds(kCore0, kUnboundedBound, /*cursor=*/1000);
    vis.PublishCommit(20, 50);
    vis.LowerSnapshotBound(kCore1, 40);
    EXPECT_EQ(vis.HorizonLsn(), 40u);
    EXPECT_EQ(vis.Reclaim(), 0u);
    EXPECT_EQ(vis.LookupCommit(20).commit_lsn, 50u);
    // Lower-only: a later, higher value does not raise it.
    vis.LowerSnapshotBound(kCore1, 60);
    EXPECT_EQ(vis.HorizonLsn(), 40u);
    // A publication does.
    vis.PublishSnapshotBound(kCore1, kUnboundedBound);
    EXPECT_EQ(vis.Reclaim(), 1u);
}

// The manager's half of C1: the slot covers a held view from the moment
// it is minted, before anything registers or holds it, and a check view
// leaves the slot alone because nothing will hold it.
TEST_F(VisibilityWiringTest, AHeldMintLowersTheSlotBeforeItIsPublished) {
    auto mgr = Attach();
    CommitOne(*mgr);
    ASSERT_EQ(vis_.slot(kCore0).min_snapshot_lsn.load(), kUnboundedBound);

    const ReadView held = mgr->MintReadView(kNoTrxId);
    EXPECT_NE(vis_.slot(kCore0).min_snapshot_lsn.load(), kUnboundedBound)
        << "a minted, unregistered view is invisible to a reclamation pass";
    EXPECT_LE(vis_.slot(kCore0).min_snapshot_lsn.load(), held.snapshot_lsn);

    // Registering recomputes the slot exactly, and releasing restores it.
    auto lease = mgr->RegisterReader(held);
    ASSERT_TRUE(lease.ok());
    EXPECT_EQ(vis_.slot(kCore0).min_snapshot_lsn.load(), held.snapshot_lsn);
    lease.value().Release();
    EXPECT_EQ(vis_.slot(kCore0).min_snapshot_lsn.load(), kUnboundedBound);

    const ReadView check = mgr->MintCheckView(kNoTrxId);
    EXPECT_EQ(check.snapshot_lsn, held.snapshot_lsn);
    EXPECT_EQ(vis_.slot(kCore0).min_snapshot_lsn.load(), kUnboundedBound)
        << "a check view is never held and holds nothing back";
}

// **AN-S3, at the managers.** The case `cross-owner-txn.md` §3 stated as
// possible: a cross-owner REPEATABLE READ transaction whose participant
// minted its own snapshot at its own BEGIN saw a commit the coordinator's
// snapshot predates - two instants in one transaction. The participant
// adopts the coordinator's snapshot instead, publishes it into its slot
// before it reads, and from then on both cores hold that instant: when
// the coordinator ends, the participant alone keeps the entry above the
// snapshot from being reclaimed.
//
// The roles are chosen so the floor *can* pass the commit: the coordinator
// on core 1 (the high block), the participant and the commit on core 0
// (the low block), whose cursor is what bounds the floor once nothing
// holds it. **Mutation**: adopt the view without lowering the slot, and
// the pass after the coordinator ends drops the entry, the floor passes
// it, and the adopted view answers it committed.
TEST_F(VisibilityWiringTest, AnAdoptedSnapshotReadsTheCoordinatorsInstantAndHoldsIt) {
    auto core0 = Attach();
    auto core1 = AttachPeer();
    CommitOne(*core0);
    CommitOne(*core1);

    // The coordinator, on core 1, pins its instant.
    auto coordinator = core1->Begin(IsolationLevel::kRepeatableRead);
    ASSERT_TRUE(coordinator.ok()) << coordinator.status().message();
    const std::uint64_t instant = coordinator.value()->view().snapshot_lsn;

    // A commit on core 0 after that instant.
    const std::uint64_t later = CommitOne(*core0);
    ASSERT_FALSE(coordinator.value()->view().Visible(later));

    // The participant's own BEGIN mints after the commit and would see it:
    // the two halves of one transaction disagreeing about `later`.
    auto participant = core0->Begin(IsolationLevel::kRepeatableRead);
    ASSERT_TRUE(participant.ok()) << participant.status().message();
    ASSERT_TRUE(participant.value()->view().Visible(later))
        << "the fixture did not put the participant's mint after the commit";

    // Adoption: the coordinator's instant, published before the first read.
    ASSERT_TRUE(core0->AdoptSnapshot(*participant.value(), instant).ok());
    EXPECT_EQ(participant.value()->view().snapshot_lsn, instant);
    EXPECT_FALSE(participant.value()->view().Visible(later));
    EXPECT_LE(vis_.slot(kCore0).min_snapshot_lsn.load(), instant)
        << "the adopted snapshot is not what core 0's slot publishes";

    // The coordinator ends. The participant is now the only thing holding
    // `later`'s entry above the floor, and it must.
    ASSERT_TRUE(core1->Commit(*coordinator.value(), wal::DurabilityClass::kRelaxed).ok());
    core1->Release(*coordinator.value());
    vis_.Reclaim();
    EXPECT_NE(vis_.LookupCommit(later).commit_lsn, kNoCommitLsn)
        << "a pass reclaimed a commit the adopted snapshot must still not see";
    EXPECT_FALSE(participant.value()->view().Visible(later))
        << "the adopted view's answer flipped: two instants in one transaction";

    // The participant ends, and the entry is free to go.
    ASSERT_TRUE(core0->Commit(*participant.value(), wal::DurabilityClass::kRelaxed).ok());
    core0->Release(*participant.value());
    vis_.Reclaim();
    EXPECT_GT(vis_.Floor(), later);
}

// A snapshot above the ceiling this transaction's own mint read is refused
// and the view is untouched: one commit order makes it impossible for a
// coordinator that minted first.
TEST_F(VisibilityWiringTest, AdoptingASnapshotAboveTheCeilingIsRefused) {
    auto mgr = Attach();
    auto txn = mgr->Begin(IsolationLevel::kRepeatableRead);
    ASSERT_TRUE(txn.ok()) << txn.status().message();
    const std::uint64_t own = txn.value()->view().snapshot_lsn;
    const Status refused = mgr->AdoptSnapshot(*txn.value(), own + 1);
    EXPECT_EQ(refused.code(), StatusCode::kInvalidArgument) << refused.message();
    EXPECT_EQ(txn.value()->view().snapshot_lsn, own);
    // At the ceiling itself is legal: the coordinator minted at the same
    // instant.
    EXPECT_TRUE(mgr->AdoptSnapshot(*txn.value(), own).ok());
}

// The marker is a slot field because at most one commit per core is ever
// between its append and its publication; a failed append lifts it too, or
// every later mint on the instance would stay capped.
TEST_F(VisibilityWiringTest, ACommitLeavesNoPendingMarkerBehindIt) {
    auto mgr = Attach();
    EXPECT_EQ(vis_.slot(kCore0).pending_commit_bound.load(), kUnboundedBound);
    const std::uint64_t id = CommitOne(*mgr);
    EXPECT_EQ(vis_.slot(kCore0).pending_commit_bound.load(), kUnboundedBound);
    EXPECT_EQ(vis_.SnapshotCeiling(), vis_.LookupCommit(id).commit_lsn)
        << "one core, one commit: the ceiling is that commit";
}

// The manager publishes its oldest live snapshot into its slot at every
// point the set of live views changes, so the instance horizon is what
// this core's readers hold and nothing staler.
TEST_F(VisibilityWiringTest, ACorePublishesItsOldestLiveSnapshot) {
    auto mgr = Attach();
    EXPECT_EQ(vis_.slot(kCore0).min_snapshot_lsn.load(), kUnboundedBound);

    auto txn = mgr->Begin(IsolationLevel::kReadCommitted);
    ASSERT_TRUE(txn.ok()) << txn.status().message();
    EXPECT_EQ(vis_.slot(kCore0).min_snapshot_lsn.load(), txn.value()->view().snapshot_lsn);
    EXPECT_EQ(mgr->ReadHorizon(), txn.value()->view().snapshot_lsn);

    // A leased reader below the transaction's snapshot lowers it further.
    const ReadView older = txn.value()->view();
    ASSERT_TRUE(mgr->Commit(*txn.value(), wal::DurabilityClass::kRelaxed).ok());
    mgr->Release(*txn.value());
    EXPECT_EQ(vis_.slot(kCore0).min_snapshot_lsn.load(), kUnboundedBound);
    auto lease = mgr->RegisterReader(older);
    ASSERT_TRUE(lease.ok());
    EXPECT_EQ(vis_.slot(kCore0).min_snapshot_lsn.load(), older.snapshot_lsn);
    lease.value().Release();
    EXPECT_EQ(vis_.slot(kCore0).min_snapshot_lsn.load(), kUnboundedBound);
}

// **Core 0's undo purge does not pass a reader on another core.** A reader
// on core 1 holds a snapshot from before core 0's writer committed; that
// commit sits above the instance horizon, so the floor stops at the
// writer's id and the pages holding its undo stay - however much core 0
// grows its log. Releasing the lease lets the next growth recycle them.
//
// Before AN-S2 core 0's horizon walked core 0's readers alone, and this
// reader was invisible to it.
TEST_F(VisibilityWiringTest, CoreZerosUndoPurgeDoesNotPassAReaderOnAnotherCore) {
    auto core0 = Attach();
    auto core1 = AttachPeer();
    CommitOne(*core0);
    CommitOne(*core1);

    // The reader on core 1, registered before the writer runs.
    const ReadView reader_view = core1->MintReadView(kNoTrxId);
    auto lease = core1->RegisterReader(reader_view);
    ASSERT_TRUE(lease.ok());

    const std::vector<std::byte> image(kUndoPageCapacity / 4 - kUndoRecordHeaderSize,
                                       std::byte{0x5A});
    UndoRecordFields fields{};
    fields.type = static_cast<std::uint8_t>(UndoRecordType::kOverwrite);
    fields.prior_trx_id = kAlwaysVisibleTrxId;
    fields.prior_undo_ptr = kNoUndoPtr;
    fields.target_page_id = 300;
    fields.target_slot = 2;

    // The writer on core 0: two pages of undo, then a commit the reader's
    // snapshot predates.
    auto writer = core0->Begin(IsolationLevel::kReadCommitted);
    ASSERT_TRUE(writer.ok()) << writer.status().message();
    const std::uint64_t writer_id = writer.value()->id();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(core0->AppendUndo(*writer.value(), fields, /*pk=*/1, image).ok());
    }
    ASSERT_TRUE(core0->Commit(*writer.value(), wal::DurabilityClass::kRelaxed).ok());
    core0->Release(*writer.value());
    EXPECT_FALSE(reader_view.Visible(writer_id));

    // A second writer grows the log past the first's pages while the
    // reader still holds its snapshot: nothing may recycle.
    auto next = core0->Begin(IsolationLevel::kReadCommitted);
    ASSERT_TRUE(next.ok()) << next.status().message();
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(core0->AppendUndo(*next.value(), fields, /*pk=*/1, image).ok());
    }
    EXPECT_EQ(undo_->PagesRecycled(), 0u)
        << "core 0 recycled undo a reader on core 1 can still reach";
    EXPECT_LE(vis_.Floor(), writer_id) << "the floor passed a commit above the horizon";
    ASSERT_TRUE(core0->Commit(*next.value(), wal::DurabilityClass::kRelaxed).ok());
    core0->Release(*next.value());

    // The reader lets go, and the next growth finds the pages settled.
    lease.value().Release();
    auto after = core0->Begin(IsolationLevel::kReadCommitted);
    ASSERT_TRUE(after.ok()) << after.status().message();
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(core0->AppendUndo(*after.value(), fields, /*pk=*/1, image).ok());
    }
    EXPECT_GE(undo_->PagesRecycled(), 1u) << "with no reader left, nothing held the pages";
    ASSERT_TRUE(core0->Commit(*after.value(), wal::DurabilityClass::kRelaxed).ok());
    core0->Release(*after.value());
}

// AN-R8's mount case. The floor is **not** zero once a core has attached: at
// mount both of its terms sit at the post-recovery high-water, and a floor
// left at zero would answer "not committed" for every transaction this
// volume committed before the restart - with an empty window and nothing
// else to consult.
TEST_F(VisibilityWiringTest, AttachingPublishesTheCursorAndPutsTheFloorAtTheHighWater) {
    ASSERT_EQ(vis_.Floor(), 0u);
    auto mgr = Attach();
    const std::uint64_t high_water = superblock_.next_trx_id();
    EXPECT_EQ(vis_.slot(kCore0).issue_cursor.load(), high_water);
    EXPECT_EQ(vis_.slot(kCore0).oldest_unresolved.load(), kUnboundedBound);
    EXPECT_EQ(vis_.Floor(), high_water);
    EXPECT_EQ(vis_.window_size(), 0u);
}

TEST_F(VisibilityWiringTest, BeginHoldsTheFloorAtOrBelowTheIdItIssued) {
    auto mgr = Attach();
    auto txn = mgr->Begin(IsolationLevel::kReadCommitted);
    ASSERT_TRUE(txn.ok()) << txn.status().message();
    const std::uint64_t id = txn.value()->id();

    EXPECT_EQ(vis_.slot(kCore0).oldest_unresolved.load(), id);
    // Exclusive: the cursor names the id this core would issue *next*.
    EXPECT_EQ(vis_.slot(kCore0).issue_cursor.load(), id + 1);
    // Whatever any core reclaims now, the floor's branch may not claim a
    // transaction that is still running.
    vis_.Reclaim();
    EXPECT_LE(vis_.Floor(), id);
}

TEST_F(VisibilityWiringTest, CommitEntersTheWindowAtTheLsnItReturned) {
    auto mgr = Attach();
    auto txn = mgr->Begin(IsolationLevel::kReadCommitted);
    ASSERT_TRUE(txn.ok()) << txn.status().message();
    const std::uint64_t id = txn.value()->id();

    auto lsn = mgr->Commit(*txn.value(), wal::DurabilityClass::kRelaxed);
    ASSERT_TRUE(lsn.ok()) << lsn.status().message();
    EXPECT_NE(lsn.value(), wal::kNoLsn);
    EXPECT_EQ(vis_.LookupCommit(id).commit_lsn, lsn.value());
    EXPECT_EQ(vis_.window_size(), 1u);
    // And the transaction stops holding the floor down.
    EXPECT_EQ(vis_.slot(kCore0).oldest_unresolved.load(), kUnboundedBound);
    vis_.Reclaim();
    EXPECT_GT(vis_.Floor(), id);
}

TEST_F(VisibilityWiringTest, AbortLeavesNoWindowEntry) {
    auto mgr = Attach();
    auto txn = mgr->Begin(IsolationLevel::kReadCommitted);
    ASSERT_TRUE(txn.ok()) << txn.status().message();
    const std::uint64_t id = txn.value()->id();

    ASSERT_TRUE(mgr->Abort(*txn.value()).ok());
    // A loser is invisible by absence: no entry, and the floor is free to
    // rise past it because its page changes are undone.
    EXPECT_EQ(vis_.LookupCommit(id).commit_lsn, kNoCommitLsn);
    EXPECT_EQ(vis_.window_size(), 0u);
    EXPECT_EQ(vis_.slot(kCore0).oldest_unresolved.load(), kUnboundedBound);
    vis_.Reclaim();
    EXPECT_GT(vis_.Floor(), id);
}

// ---- AN-S1b: burning an idle core's block (AN-R13) ------------------------
//
// The floor is a minimum over cores of each core's issue cursor, and a
// cursor rises only when its core issues an id. So a core that stops running
// transactions freezes the floor for the whole instance and the commit
// window can never drop another entry. Burning the unspent block is the
// operator's marked exit: the core discards ids it reserved and takes a
// fresh block at the current high-water, which is what `trx_id.hpp`'s own
// trade already permits - unique and monotonic, never gapless.

TEST(InstanceVisibilityTest, OneAttachedCorePinsNothing) {
    InstanceVisibility vis;
    vis.PublishIssueCursor(kCore0, 9000);
    vis.PublishOldestUnresolved(kCore0, kUnboundedBound);
    // The floor tracks this core's own cursor, which rises with its own
    // work, so there is nothing to burn for. The shipped `cores = 1` case.
    EXPECT_EQ(vis.attached_cores(), 1u);
    EXPECT_FALSE(vis.PinsFloor(kCore0));
}

TEST(InstanceVisibilityTest, TheLowestCursorIsTheCorePinningTheFloor) {
    InstanceVisibility vis;
    vis.PublishIssueCursor(kCore0, 5000);
    vis.PublishOldestUnresolved(kCore0, kUnboundedBound);
    vis.PublishIssueCursor(kCore1, 9000);
    vis.PublishOldestUnresolved(kCore1, kUnboundedBound);

    EXPECT_EQ(vis.attached_cores(), 2u);
    EXPECT_TRUE(vis.PinsFloor(kCore0));
    EXPECT_FALSE(vis.PinsFloor(kCore1));
    // An unattached core pins nothing and is never asked to burn.
    EXPECT_FALSE(vis.PinsFloor(7));
}

// A *busy* core whose oldest live transaction sits below the idle core's
// cursor is the one holding the floor, and it lets go when that transaction
// ends. Burning the idle core's block would buy nothing, so it is not asked
// to: `PinsFloor` compares against the candidate, not against the cursors.
TEST(InstanceVisibilityTest, ALiveTransactionBelowTheCursorPinsInstead) {
    InstanceVisibility vis;
    vis.PublishIssueCursor(kCore0, 5000);
    vis.PublishOldestUnresolved(kCore0, kUnboundedBound);
    vis.PublishIssueCursor(kCore1, 9000);
    vis.PublishOldestUnresolved(kCore1, 4000);

    EXPECT_EQ(vis.FloorCandidate(), 4000u);
    EXPECT_FALSE(vis.PinsFloor(kCore0));
    EXPECT_FALSE(vis.PinsFloor(kCore1));
}

// The failure AN-R13 names, as a cell: an idle core freezes its cursor, the
// busy core carves every later block above it, and the floor never moves
// again. Without the burn this is the state the instance stays in.
TEST(InstanceVisibilityTest, AnIdleCoreFreezesTheFloorAndTheWindowGrows) {
    InstanceVisibility vis;
    vis.PublishIssueCursor(kCore1, 4096);  // attached, idle, never moves
    vis.PublishOldestUnresolved(kCore1, kUnboundedBound);
    vis.PublishIssueCursor(kCore0, 8192);
    vis.PublishOldestUnresolved(kCore0, kUnboundedBound);

    for (std::uint64_t id = 8192; id < 8192 + 20000; ++id) {
        vis.PublishIssueCursor(kCore0, id + 1);
        vis.PublishCommit(id, id);
    }
    // Pinned at the idle core's cursor, and every one of those commits is
    // above it, so nothing was ever dropped.
    EXPECT_EQ(vis.Floor(), 4096u);
    EXPECT_EQ(vis.window_size(), 20000u);
    EXPECT_TRUE(vis.PinsFloor(kCore1));

    // The burn, as the sequence performs it: the cursor jumps to a block
    // carved at the current high-water, which is above every block already
    // carved - so above the busy core's cursor, not level with it.
    vis.PublishIssueCursor(kCore1, 8192 + 20000 + 4096);
    // The pin moves to the busy core, which is the converging state and not
    // a stuck one: that cursor rises with its own work.
    EXPECT_FALSE(vis.PinsFloor(kCore1));
    EXPECT_TRUE(vis.PinsFloor(kCore0));
    EXPECT_EQ(vis.Reclaim(), 20000u);
    EXPECT_EQ(vis.window_size(), 0u);
    EXPECT_EQ(vis.Floor(), 8192 + 20000u);
}

TEST_F(VisibilityWiringTest, AnIdleCoreBurnsItsBlockAndUnpinsTheFloor) {
    auto mgr = Attach();
    const std::uint64_t high_water = superblock_.next_trx_id();

    // A second core's block, **carved rather than asserted**. That matters:
    // a burn takes its new window from the superblock's high-water, so a
    // peer whose cursor was merely published would leave the high-water
    // where core 0 already is and the burn would hand it back the block it
    // started on. Carving is what a real peer's lease does, and it is what
    // puts a block above core 0's.
    auto peer_block = ids_->Carve(4096);
    ASSERT_TRUE(peer_block.ok()) << peer_block.status().message();
    vis_.PublishIssueCursor(kCore1, peer_block.value().first);
    vis_.PublishOldestUnresolved(kCore1, kUnboundedBound);
    ASSERT_TRUE(vis_.PinsFloor(kCore0));

    // The first tick only records the cursor: "idle" means unmoved
    // *between* two ticks, so nothing can burn on a first observation.
    EXPECT_EQ(mgr->MaybeBurnIdleBlock(), TransactionManager::BurnOutcome::kNotNeeded);

    // Idle and pinning, but the window has not grown, so a burn is not worth
    // a superblock write.
    EXPECT_EQ(mgr->MaybeBurnIdleBlock(), TransactionManager::BurnOutcome::kNotNeeded);
    EXPECT_EQ(vis_.slot(kCore0).issue_cursor.load(), high_water);

    for (std::uint64_t id = 1; id <= 4096; ++id) vis_.PublishCommit(high_water + id, id);
    ASSERT_GE(vis_.window_size(), 4096u);

    // Now it is. Core 0 carves its own block, so the burn is synchronous and
    // it never asks anyone for one.
    EXPECT_EQ(mgr->MaybeBurnIdleBlock(), TransactionManager::BurnOutcome::kBurned);
    EXPECT_GT(vis_.slot(kCore0).issue_cursor.load(), high_water);
    EXPECT_FALSE(vis_.PinsFloor(kCore0));
}

TEST_F(VisibilityWiringTest, ABusyCoreIsNeverAskedToBurn) {
    auto mgr = Attach();
    const std::uint64_t high_water = superblock_.next_trx_id();
    vis_.PublishIssueCursor(kCore1, high_water + 1'000'000);
    vis_.PublishOldestUnresolved(kCore1, kUnboundedBound);
    for (std::uint64_t id = 1; id <= 4096; ++id) vis_.PublishCommit(high_water + id, id);

    EXPECT_EQ(mgr->MaybeBurnIdleBlock(), TransactionManager::BurnOutcome::kNotNeeded);
    // A transaction between two ticks moves the cursor, which is what "busy"
    // is - exactly, and with no counter, because `Next()` has one caller.
    auto txn = mgr->Begin(IsolationLevel::kReadCommitted);
    ASSERT_TRUE(txn.ok()) << txn.status().message();
    EXPECT_EQ(mgr->MaybeBurnIdleBlock(), TransactionManager::BurnOutcome::kNotNeeded);
    EXPECT_EQ(vis_.slot(kCore0).issue_cursor.load(), high_water + 1);
}

// **The safety property, through the manager rather than beside it.** The
// cell above only ever exercises the `!idle` branch: the cursor moved, so
// both answers are `kNotNeeded` for the cheapest possible reason. The case
// that matters is a core holding a **live transaction** whose cursor is
// *still* between two ticks - idle by the burn's own test - where the only
// thing standing between it and a burn is `PinsFloor` reading
// `oldest_unresolved`. Burning there would put this core's cursor above the
// high-water and let the floor pass a writer that has not committed, which
// is the whole hazard AN-R8 exists for, reached through a timer instead of
// through the predicate.
TEST_F(VisibilityWiringTest, ALiveTransactionStopsTheBurnThoughTheCursorIsStill) {
    auto mgr = Attach();

    // **The transaction first, and the peer's block after it.** Order
    // matters here for a reason worth stating: `Begin` is what makes this
    // core carve its own block, so a block carved *before* it sits below
    // the live id, and every commit in it is below the floor's binding
    // term and drains on the next reclamation - leaving the window empty
    // and the cell asserting nothing.
    auto txn = mgr->Begin(IsolationLevel::kReadCommitted);
    ASSERT_TRUE(txn.ok()) << txn.status().message();
    const std::uint64_t live_id = txn.value()->id();

    // Nothing moves this core's cursor from here: no further `Begin`.
    auto peer = ids_->Carve(4096);
    ASSERT_TRUE(peer.ok()) << peer.status().message();
    vis_.PublishIssueCursor(kCore1, peer.value().first + peer.value().count);
    vis_.PublishOldestUnresolved(kCore1, kUnboundedBound);
    for (std::uint64_t i = 0; i < 4096; ++i) vis_.PublishCommit(peer.value().first + i, i + 1);
    ASSERT_GE(vis_.window_size(), 4096u);

    // Idle by the cursor test on the second call, pinning by cursor, window
    // over the threshold - every gate open but the one that matters.
    EXPECT_EQ(mgr->MaybeBurnIdleBlock(), TransactionManager::BurnOutcome::kNotNeeded);
    EXPECT_EQ(mgr->MaybeBurnIdleBlock(), TransactionManager::BurnOutcome::kNotNeeded);
    EXPECT_FALSE(vis_.PinsFloor(kCore0));
    // And the floor never passed the live writer, which is what the refusal
    // was protecting.
    EXPECT_LE(vis_.Floor(), live_id);
}

// **A burn spends itself, and the gate closes on the same call.** The gate
// reads `window_size()`, and the window shrinks only inside `Reclaim()` -
// which runs from `PublishCommit` and from an attach, and from nowhere else.
// A burn that raised the cursor and stopped would therefore leave the gate
// open on exactly the instance it was built for: with the commits stopped
// nothing calls `Reclaim()` again, so every idle core that pins burns on
// every tick for the life of the process - a carve and a superblock `Sync()`
// each time, on an instance doing nothing.
TEST_F(VisibilityWiringTest, ABurnDrainsTheWindowItWasTakenFor) {
    auto mgr = Attach();
    const std::uint64_t high_water = superblock_.next_trx_id();

    // A peer that carved a block, spent it, and stopped - so its cursor sits
    // one block above the high-water core 0 still holds, and every id in the
    // window is one this peer issued.
    auto peer_block = ids_->Carve(4096);
    ASSERT_TRUE(peer_block.ok()) << peer_block.status().message();
    const TrxIdRange peer = peer_block.value();
    vis_.PublishIssueCursor(kCore1, peer.first + peer.count);
    vis_.PublishOldestUnresolved(kCore1, kUnboundedBound);
    for (std::uint64_t i = 0; i < peer.count; ++i) vis_.PublishCommit(peer.first + i, i + 1);

    // Core 0 has issued nothing, so its cursor is the binding term and none
    // of the peer's commits can be dropped.
    ASSERT_TRUE(vis_.PinsFloor(kCore0));
    ASSERT_EQ(vis_.window_size(), peer.count);

    EXPECT_EQ(mgr->MaybeBurnIdleBlock(), TransactionManager::BurnOutcome::kNotNeeded);
    EXPECT_EQ(mgr->MaybeBurnIdleBlock(), TransactionManager::BurnOutcome::kBurned);

    // The burn put core 0's cursor above every id ever issued, and the pass
    // that reads it ran in the same call: the window is gone, not merely
    // droppable.
    EXPECT_EQ(vis_.window_size(), 0u);
    EXPECT_GE(vis_.Floor(), peer.first + peer.count);
    // And so the gate is shut: the next tick is idle and still pinning, and
    // answers "not needed" because there is nothing left to buy.
    EXPECT_EQ(mgr->MaybeBurnIdleBlock(), TransactionManager::BurnOutcome::kNotNeeded);
    EXPECT_EQ(superblock_.next_trx_id(), high_water + 2 * peer.count);
}

// A manager with no instance state is every fixture and every tool: the
// check must cost it nothing and change nothing.
TEST_F(VisibilityWiringTest, ABareManagerNeverBurns) {
    TransactionManager bare(*ids_, *undo_, store_, wal_.get());
    const std::uint64_t before = ids_->peek();
    EXPECT_EQ(bare.MaybeBurnIdleBlock(), TransactionManager::BurnOutcome::kNotNeeded);
    EXPECT_EQ(bare.MaybeBurnIdleBlock(), TransactionManager::BurnOutcome::kNotNeeded);
    EXPECT_EQ(ids_->peek(), before);
}

// ---- AN-R9: the ceiling a snapshot will take -------------------------------

TEST(InstanceVisibilityTest, TheCommitCeilingCoversEveryPublishedCommitAndOnlyThose) {
    // **The ceiling AN-S2's mint will read**, landed ahead of it because it
    // is the additive half: nothing takes it yet, so this cell is the whole
    // of its behaviour.
    //
    // The property is not "the highest LSN assigned" but "the highest
    // **published**". A commit's LSN is fixed under the append latch and
    // its window entry becomes readable later, and AN-Q3 names that
    // interval as the one place this design can be implemented wrongly and
    // pass every test - so what a snapshot may take is bounded by what it
    // could then find.
    InstanceVisibility vis;
    EXPECT_EQ(vis.CommitCeiling(), 0u) << "a fresh instance has published nothing";

    vis.PublishCommit(10, 400);
    EXPECT_EQ(vis.CommitCeiling(), 400u);

    // Every commit at or below the ceiling is findable, which is the pairing
    // the ordering inside `PublishCommit` exists to guarantee.
    const InstanceVisibility::CommitLookup found = vis.LookupCommit(10);
    EXPECT_EQ(found.commit_lsn, 400u);
    EXPECT_LE(found.commit_lsn, vis.CommitCeiling());

    // Monotone, and out-of-order publication does not lower it. A commit
    // LSN comes from one stream under one latch so this should not arise,
    // but the max is taken rather than assumed: "assigned in order" and
    // "published in order" are the two things AN-Q3 says not to conflate.
    vis.PublishCommit(11, 900);
    EXPECT_EQ(vis.CommitCeiling(), 900u);
    vis.PublishCommit(12, 700);
    EXPECT_EQ(vis.CommitCeiling(), 900u) << "a later publication lowered the ceiling";

    // An abort publishes nothing, so it moves neither the window nor the
    // ceiling - the property `Abort leaves no entry` already pins, restated
    // here because a ceiling raised by an abort would let a snapshot cover
    // a transaction that never committed.
    const std::uint64_t before = vis.CommitCeiling();
    EXPECT_EQ(vis.LookupCommit(13).commit_lsn, kNoCommitLsn);
    EXPECT_EQ(vis.CommitCeiling(), before);
}

// ---- AN-R12: the floor and the window are read together -------------------

TEST(InstanceVisibilityTest, AReclaimedWinnerIsNeverAnsweredUncommitted) {
    // **The straddle AN-R12 closes.** `Reclaim()` erases entries below
    // `reachable` and raises the floor to it, both under the window latch.
    // A reader that took the floor *before* a pass and looked the window up
    // *after* it sees no entry and `trx_id >= floor_old`, and concludes not
    // committed for a transaction committed long ago - a lost row, from two
    // reads straddling one pass.
    //
    // **The invariant, and it is exactly what one hold buys**: for a
    // transaction that has committed, an absent window entry is an entry
    // *below the floor*. There is no third state under `LookupCommit`,
    // because the pass cannot run between its two reads.
    //
    // **Mutation** (the one this cell is written against): give
    // `LookupCommit` a `Floor()` read outside the latch and a separately
    // latched window lookup - two holds - and the violation count goes
    // above zero. The reader below runs while the floor is climbing through
    // its ids, which is the only time the window between two such reads
    // contains anything.
    constexpr std::uint64_t kIds = 3000;
    InstanceVisibility vis;

    // Every id has committed, so an absent entry can only mean reclaimed -
    // which is what makes the invariant testable at all. An id that never
    // committed is legitimately absent at any floor.
    for (std::uint64_t id = 1; id <= kIds; ++id) vis.PublishCommit(id, id * 10);
    vis.PublishOldestUnresolved(kCore0, kUnboundedBound);

    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> violations{0};
    // **The liveness pair, and it is what makes the cell non-vacuous.** A
    // reader that ran entirely before the passes sees every entry present;
    // one that ran entirely after sees every entry absent. Seeing *both* is
    // the proof it was reading while the floor climbed, which is the only
    // state where a straddle is possible at all. Counting sweeps instead
    // does not discriminate: the first version of this cell asserted the
    // reader outlived one sweep and failed on a correct implementation,
    // because 120 passes over 3000 ids finish inside a single sweep.
    std::atomic<std::uint64_t> seen_present{0};
    std::atomic<std::uint64_t> seen_reclaimed{0};

    // **A start barrier, because thread construction outran the passes.**
    // Without it the writer's 120 reclaims finished before the reader's
    // first lookup and `seen_present` was 0 - the reader raced nothing and
    // the cell proved nothing. Measured, not reasoned: that is exactly how
    // it failed.
    std::atomic<bool> reader_running{false};

    const auto sweep = [&] {
        for (std::uint64_t id = 1; id <= kIds; ++id) {
            const InstanceVisibility::CommitLookup found = vis.LookupCommit(id);
            if (found.commit_lsn != kNoCommitLsn) {
                seen_present.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            seen_reclaimed.fetch_add(1, std::memory_order_relaxed);
            // The lost row: no entry, and the floor does not cover it.
            if (id >= found.floor) violations.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread reader([&] {
        reader_running.store(true, std::memory_order_release);
        while (!done.load(std::memory_order_acquire)) sweep();
        // **One sweep after the passes, unconditionally.** Without it
        // `seen_reclaimed` depends on where the reader happened to be when
        // `done` was set, which under a loaded `ctest -j8` is a coin toss -
        // and a liveness assertion that flakes is worse than none.
        sweep();
    });

    // **Wait for the reader to have *read*, not merely to have started.**
    // The flag alone proves the thread is alive; under load it can then be
    // descheduled for the whole climb below, leaving `seen_present` at 0
    // and the cell failing on a correct implementation. One observed live
    // entry is the state the straddle needs to exist at all.
    while (!reader_running.load(std::memory_order_acquire)) std::this_thread::yield();
    while (seen_present.load(std::memory_order_relaxed) == 0) std::this_thread::yield();

    // The floor climbs through the ids in steps, so the reader is looking
    // at entries the passes are erasing rather than at a settled window.
    // A yield per step widens the overlap; without one the whole climb fits
    // inside a fraction of a single sweep.
    for (std::uint64_t cursor = 1; cursor <= kIds; cursor += 25) {
        vis.PublishIssueCursor(kCore0, cursor);
        vis.Reclaim();
        std::this_thread::yield();
    }
    vis.PublishIssueCursor(kCore0, kIds + 1);
    vis.Reclaim();
    done.store(true, std::memory_order_release);
    reader.join();

    EXPECT_EQ(vis.window_size(), 0u) << "the passes did not drain the window, so nothing raced";
    EXPECT_EQ(vis.Floor(), kIds + 1);
    EXPECT_GT(seen_present.load(), 0u)
        << "the reader saw no live entry, so it started after every pass had run";
    EXPECT_GT(seen_reclaimed.load(), 0u)
        << "the reader saw no reclaimed entry, so it finished before any pass ran";
    EXPECT_EQ(violations.load(), 0u)
        << "a reclaimed winner was answered uncommitted: the floor and the window were read "
           "on either side of a Reclaim() pass";
}

}  // namespace
}  // namespace kds::txn
