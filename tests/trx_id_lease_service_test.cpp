#include "kds/server/trx_id_lease_service.hpp"

#include <set>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/server/superblock.hpp"

// Leasing transaction ids over the ring (`docs/inflight/in-progress/workplan-peer-writer.md` PW1).
//
// The gap this closes is not a slow path, it is a closed door:
// `TrxIdSequence` constructs spent, so a peer's very first `Next()` reserved
// a block and its persist callback refused - every peer write died at its
// first id, ahead of any page. `APeerCannotIssueAnIdWithoutALease` is that
// door, kept as a test so the refusal stays a refusal.

namespace kds::server {
namespace {

class TrxIdLeaseServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 64);
        ASSERT_TRUE(transport.ok()) << transport.status().message();
        transport_.emplace(std::move(transport.value()));

        core0_.emplace(clock_, io0_);
        core1_.emplace(clock_, io1_);
        ASSERT_TRUE(core0_->AttachTransport(&*transport_, 0).ok());
        ASSERT_TRUE(core1_->AttachTransport(&*transport_, 1).ok());

        // Core 0's own sequence, and the one the grant handler carves from.
        // Sharing it is the property under test in `TwoConsumers...` below.
        superblock_ = server::SuperBlock::CreateFresh(/*now_unix_seconds=*/1000);
        core0_ids_.emplace(superblock_, [this] {
            ++persists_;
            return Status::OK();
        });

        ASSERT_TRUE(
            RegisterTrxIdGrantHandler(*core0_, *transport_, *core0_ids_, kTrxIdLeasePerGrant,
                                      nullptr)
                .ok());
        ASSERT_TRUE(RegisterTrxIdGrantReceiver(*core1_, refill_, lease_, nullptr).ok());
    }

    // Steps both reactors round-robin, sched.md §8's simulation shape and
    // what makes every one of these deterministic.
    void Pump(int iterations = 20) {
        for (int i = 0; i < iterations; ++i) {
            core0_->RunOnce();
            core1_->RunOnce();
        }
    }

    // Asks for one block from core 1 and runs it to completion. The block
    // size is not a parameter: `RequestTrxIdLease` does not take one, and
    // every caller used the default - a knob that controlled nothing.
    Status Refill() {
        bool finished = false;
        Status result;
        core1_->Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kSystem,
            RequestTrxIdLease(*transport_, refill_, /*core_id=*/1),
            [&](const Status& s) {
                result = s;
                finished = true;
            }));
        Pump();
        EXPECT_TRUE(finished) << "the refill never completed";
        return result;
    }

    sched::ManualClock clock_;
    sched::NullIoBackend io0_;
    sched::NullIoBackend io1_;
    std::optional<sched::RealRingTransport> transport_;
    std::optional<sched::Scheduler> core0_;
    std::optional<sched::Scheduler> core1_;
    server::SuperBlock superblock_{server::SuperBlock::CreateFresh(0)};
    std::optional<txn::TrxIdSequence> core0_ids_;
    std::uint64_t persists_ = 0;
    txn::TrxIdLease lease_;
    TrxIdRefill refill_;
};

TEST_F(TrxIdLeaseServiceTest, APeerCannotIssueAnIdWithoutALease) {
    // The pre-PW1 state, and what a missing lease source must still do: a
    // peer's sequence with the superblock refusal installed and nothing
    // else fails at the *first* id, not at the 4097th.
    server::SuperBlock peer_copy = server::SuperBlock::CreateFresh(1000);
    txn::TrxIdSequence peer(peer_copy,
                            [] { return Status::Unsupported("a peer may not write page 0"); });

    auto id = peer.Next();
    ASSERT_FALSE(id.ok()) << "a peer issued " << id.value() << " with no lease and no page 0";
    EXPECT_EQ(id.status().code(), StatusCode::kUnsupported);
}

TEST_F(TrxIdLeaseServiceTest, ALeasedPeerIssuesIdsAndTheCeilingIsDurableFirst) {
    server::SuperBlock peer_copy = server::SuperBlock::CreateFresh(1000);
    txn::TrxIdSequence peer(peer_copy, [] { return Status::Unsupported("not reachable"); });
    peer.SetLeaseSource(&lease_);

    // Spent, and retryable rather than fatal - the caller's answer is "the
    // grant is on its way", never "the id space is gone".
    auto before = peer.Next();
    ASSERT_FALSE(before.ok());
    EXPECT_EQ(before.status().code(), StatusCode::kTxnConflict);
    EXPECT_TRUE(before.status().retryable());

    const std::uint64_t persists_before = persists_;
    ASSERT_TRUE(Refill().ok());
    EXPECT_EQ(refill_.stats.requests, 1u);
    EXPECT_EQ(refill_.stats.grants, 1u);
    EXPECT_EQ(refill_.count, kTrxIdLeasePerGrant);

    // **Persisted before the reply, not after.** CoreRuntime::Open refuses a
    // mount whose peer stream names an id above the superblock's ceiling, so
    // a grant that outran its own durability would refuse the mount of a
    // database that did nothing wrong.
    EXPECT_GT(persists_, persists_before) << "a block was granted without persisting the ceiling";
    EXPECT_GE(superblock_.next_trx_id(), refill_.first_id + refill_.count);

    auto id = peer.Next();
    ASSERT_TRUE(id.ok()) << id.status().message();
    EXPECT_EQ(id.value(), refill_.first_id);
}

TEST_F(TrxIdLeaseServiceTest, TwoConsumersOfOneCeilingNeverIssueTheSameId) {
    // The property the whole mechanism exists for. Core 0 keeps issuing from
    // its own window while it carves blocks for a peer, and the two windows
    // must not overlap - invariant 12 has no room for a reissued writer id.
    server::SuperBlock peer_copy = server::SuperBlock::CreateFresh(1000);
    txn::TrxIdSequence peer(peer_copy, [] { return Status::Unsupported("not reachable"); });
    peer.SetLeaseSource(&lease_);

    std::set<std::uint64_t> seen;
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 200; ++i) {
            auto id = core0_ids_->Next();
            ASSERT_TRUE(id.ok()) << id.status().message();
            EXPECT_TRUE(seen.insert(id.value()).second)
                << "core 0 reissued transaction id " << id.value();
        }
        ASSERT_TRUE(Refill().ok());
        for (int i = 0; i < 200; ++i) {
            auto id = peer.Next();
            ASSERT_TRUE(id.ok()) << id.status().message();
            EXPECT_TRUE(seen.insert(id.value()).second)
                << "a peer was granted transaction id " << id.value() << " twice";
        }
    }
}

TEST_F(TrxIdLeaseServiceTest, ACoreZeroReserveAfterAPeerGrantDoesNotLowerTheCeiling) {
    // The arithmetic PW1 had to change. Reserving from the sequence's own
    // `next_` computed a ceiling *below* the durable one once a peer's block
    // had raised it, and `SetNextTrxId` refuses to lower - so core 0's next
    // reserve failed outright. Carving from the superblock's high-water is
    // what makes the two consumers compose.
    ASSERT_TRUE(core0_ids_->Next().ok());  // core 0 takes its first window
    ASSERT_TRUE(Refill().ok());            // a peer's block raises the ceiling above it

    while (core0_ids_->remaining() > 0) ASSERT_TRUE(core0_ids_->Next().ok());
    auto after = core0_ids_->Next();
    ASSERT_TRUE(after.ok()) << after.status().message();
    EXPECT_GE(after.value(), refill_.first_id + refill_.count)
        << "core 0's new window overlaps the block it granted the peer";
}

TEST_F(TrxIdLeaseServiceTest, AGrantThatCannotBeCarvedIsReportedRatherThanHungOn) {
    // The failure that would otherwise park a coroutine forever: core 0
    // replies with a zero-count grant, which the waiter can read.
    server::SuperBlock unwritable = server::SuperBlock::CreateFresh(1000);
    txn::TrxIdSequence refusing(unwritable,
                                [] { return Status::IoError("the superblock page is gone"); });
    sched::Scheduler failing_core0(clock_, io0_);
    ASSERT_TRUE(failing_core0.AttachTransport(&*transport_, 0).ok());
    ASSERT_TRUE(
        RegisterTrxIdGrantHandler(failing_core0, *transport_, refusing, kTrxIdLeasePerGrant,
                                  nullptr)
            .ok());

    bool finished = false;
    Status result;
    core1_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kSystem,
        RequestTrxIdLease(*transport_, refill_, /*core_id=*/1),
        [&](const Status& s) {
            result = s;
            finished = true;
        }));
    for (int i = 0; i < 20; ++i) {
        failing_core0.RunOnce();
        core1_->RunOnce();
    }

    ASSERT_TRUE(finished) << "the requester is still parked on a grant that will never come";
    EXPECT_EQ(result.code(), StatusCode::kResourceExhausted);
    EXPECT_EQ(refill_.count, 0u);
    EXPECT_FALSE(lease_.pending());
}

TEST_F(TrxIdLeaseServiceTest, ASecondGrantExtendsAContiguousPendingBlock) {
    ASSERT_TRUE(Refill().ok());
    const std::uint64_t first = refill_.first_id;
    ASSERT_TRUE(Refill().ok());

    // Both blocks are still pending - nothing consumed the first - and core
    // 0 carves contiguously, so the peer holds one run rather than having
    // burned a block it never issued from.
    EXPECT_EQ(lease_.pending_count(), 2 * kTrxIdLeasePerGrant);

    server::SuperBlock peer_copy = server::SuperBlock::CreateFresh(1000);
    txn::TrxIdSequence peer(peer_copy, nullptr);
    peer.SetLeaseSource(&lease_);
    auto id = peer.Next();
    ASSERT_TRUE(id.ok()) << id.status().message();
    EXPECT_EQ(id.value(), first);
}

TEST_F(TrxIdLeaseServiceTest, APendingGrantClearsTheLowWaterMark) {
    // The refill cadence's **stopping** condition, and the one place this
    // lease may not simply copy `LeasedIdSource`. That one installs its
    // extent the moment a grant arrives, so its `low_water()` falls with the
    // grant; this one *parks* the block, and the sequence takes it only once
    // its own window is spent. A `low_water()` reading the window alone
    // would therefore still be true immediately after a grant landed, and
    // `CoreRuntime::MaybeRefillTrxIds` - which asks whenever it is true and
    // nothing is in flight - would ask again on the very next tick, and on
    // every tick after that. At the 1 ms drain interval that is a superblock
    // write and a full `Sync()` per millisecond on core 0's reactor, and
    // 4096 transaction ids burned with each, for a peer that has not issued
    // one.
    server::SuperBlock peer_copy = server::SuperBlock::CreateFresh(1000);
    txn::TrxIdSequence peer(peer_copy, nullptr);
    peer.SetLeaseSource(&lease_);

    EXPECT_TRUE(peer.low_water()) << "a peer that has never held a window must ask";
    ASSERT_TRUE(Refill().ok());
    EXPECT_FALSE(peer.low_water()) << "a peer holding an unconsumed grant asked for another";

    // And once the block is in the window the mark stays down until the
    // window is three-quarters spent, which is the extent lease's threshold.
    ASSERT_TRUE(peer.Next().ok());
    EXPECT_FALSE(peer.low_water());
    while (peer.remaining() > kTrxIdLeasePerGrant / 4) ASSERT_TRUE(peer.Next().ok());
    EXPECT_TRUE(peer.low_water()) << "the mark never came back up";
}

}  // namespace
}  // namespace kds::server
