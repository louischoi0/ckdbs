#include "kds/server/extent_lease_service.hpp"

#include <array>
#include <set>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/free_map.hpp"
#include "kds/storage/memory_page_device.hpp"

// Refilling a page-id lease over the ring (workplan-crosscore.md M5/P5).
//
// This is the **first production use of the coroutine decision**: a peer
// sends a request and resumes on the grant, in straight-line code. P2 built
// the lease and left the refill out precisely because it could not be
// written before that decision landed.

namespace kds::server {
namespace {

using MapPage = std::array<std::byte, kPageSize>;

class ExtentLeaseServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        storage::FormatFreeMapPage(std::span<std::byte, kPageSize>(map_));
        allocator_.emplace(std::span<std::byte, kPageSize>(map_), /*first_new_page_id=*/128);

        auto transport = sched::RealRingTransport::Create(/*core_count=*/2, 16, 64);
        ASSERT_TRUE(transport.ok()) << transport.status().message();
        transport_.emplace(std::move(transport.value()));

        core0_.emplace(clock_, io0_);
        core1_.emplace(clock_, io1_);
        ASSERT_TRUE(core0_->AttachTransport(&*transport_, 0).ok());
        ASSERT_TRUE(core1_->AttachTransport(&*transport_, 1).ok());

        ASSERT_TRUE(RegisterExtentGrantHandler(*core0_, *transport_, *allocator_,
                                               /*pages_per_grant=*/64)
                        .ok());
        ASSERT_TRUE(RegisterExtentGrantReceiver(*core1_, refill_).ok());
    }

    // Steps both reactors round-robin, which is sched.md §8's simulation
    // shape and what makes every one of these deterministic.
    void Pump(int iterations = 20) {
        for (int i = 0; i < iterations; ++i) {
            core0_->RunOnce();
            core1_->RunOnce();
        }
    }

    MapPage map_{};
    sched::ManualClock clock_;
    sched::NullIoBackend io0_;
    sched::NullIoBackend io1_;
    std::optional<storage::ExtentAllocator> allocator_;
    std::optional<sched::RealRingTransport> transport_;
    std::optional<sched::Scheduler> core0_;
    std::optional<sched::Scheduler> core1_;
    ExtentRefill refill_;
};

TEST_F(ExtentLeaseServiceTest, APeerAsksAndIsGrantedAnExtent) {
    storage::LeasedIdSource lease;
    ASSERT_TRUE(lease.spent()) << "a peer with no lease should have nothing to give";

    bool finished = false;
    Status result;
    core1_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kSystem,
        RequestExtentRefill(*transport_, lease, refill_, /*core_id=*/1),
        [&](const Status& s) {
            result = s;
            finished = true;
        }));

    Pump();

    ASSERT_TRUE(finished) << "the refill never completed";
    EXPECT_TRUE(result.ok()) << result.message();
    EXPECT_EQ(refill_.stats.requests, 1u);
    EXPECT_EQ(refill_.stats.grants, 1u);

    // The lease can now issue, and the ids it issues are the ones core 0
    // reserved - and marked allocated, so nobody else can find them.
    EXPECT_FALSE(lease.spent());
    auto id = lease.Next();
    ASSERT_TRUE(id.ok()) << id.status().message();
    EXPECT_TRUE(storage::FreeMapIsAllocated(std::span<const std::byte, kPageSize>(map_),
                                            id.value()));
}

TEST_F(ExtentLeaseServiceTest, ARefillExtendsTheLeaseRatherThanReplacingWhatItOwns) {
    // A core keeps faulting pages it allocated under an earlier lease, so a
    // refill must add to what it owns, not swap it.
    storage::LeasedIdSource lease(storage::Extent{500, 2});
    const PageId old_page = lease.Next().value();

    bool finished = false;
    core1_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kSystem,
        RequestExtentRefill(*transport_, lease, refill_, 1),
        [&](const Status&) { finished = true; }));
    Pump();
    ASSERT_TRUE(finished);

    EXPECT_TRUE(lease.Owns(old_page)) << "the refill disowned a page the core still uses";
    EXPECT_TRUE(lease.Owns(refill_.extent.first));
}

TEST_F(ExtentLeaseServiceTest, SuccessiveRefillsNeverHandOutTheSameId) {
    // The property the whole mechanism exists for, now across the ring.
    storage::LeasedIdSource a;
    storage::LeasedIdSource b;
    // Both peers are core 1 here - there are only two cores in the fixture -
    // so they share a receiver; what matters is that the *allocator* never
    // reissues, which is what a second core would rely on.
    for (storage::LeasedIdSource* lease : {&a, &b}) {
        bool finished = false;
        core1_->Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kSystem,
            RequestExtentRefill(*transport_, *lease, refill_, 1),
            [&](const Status&) { finished = true; }));
        Pump();
        ASSERT_TRUE(finished);
    }

    std::set<PageId> seen;
    while (!a.spent()) EXPECT_TRUE(seen.insert(a.Next().value()).second);
    while (!b.spent()) {
        EXPECT_TRUE(seen.insert(b.Next().value()).second) << "an id was granted twice";
    }
}

TEST_F(ExtentLeaseServiceTest, AnExhaustedMapIsReportedRatherThanHungOn) {
    // The failure that would otherwise park a coroutine forever. Core 0
    // replies with a zero-page grant, which the waiter can read.
    MapPage full{};
    storage::FormatFreeMapPage(std::span<std::byte, kPageSize>(full));
    for (std::uint32_t i = 0; i < storage::kFreeMapBitsPerPage; ++i) {
        storage::FreeMapAllocate(std::span<std::byte, kPageSize>(full), i);
    }
    storage::ExtentAllocator none(std::span<std::byte, kPageSize>(full), 128);

    sched::Scheduler core0(clock_, io0_);
    ASSERT_TRUE(core0.AttachTransport(&*transport_, 0).ok());
    ASSERT_TRUE(RegisterExtentGrantHandler(core0, *transport_, none, 64).ok());

    storage::LeasedIdSource lease;
    bool finished = false;
    Status result;
    core1_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kSystem,
        RequestExtentRefill(*transport_, lease, refill_, 1),
        [&](const Status& s) {
            result = s;
            finished = true;
        }));

    for (int i = 0; i < 20 && !finished; ++i) {
        core0.RunOnce();
        core1_->RunOnce();
    }

    ASSERT_TRUE(finished) << "an exhausted map left the requester parked forever";
    EXPECT_EQ(result.code(), StatusCode::kResourceExhausted);
    EXPECT_TRUE(lease.spent()) << "a failed refill must leave the lease untouched";
}

TEST_F(ExtentLeaseServiceTest, AGrantedExtentIsOnTheDeviceBeforeTheGrantLeaves) {
    // PW3b's finding (workplan-peer-writer.md §6, docs/inflight/known-gaps.md): the
    // peer will commit rows into this run, so the run has to be *allocated
    // on the device* before it may - or a crash frees it, and the next
    // mount's allocator hands it out over those rows. The handler therefore
    // calls `ExtentAllocator::Persist()` before replying.
    //
    // Over a real store, because that is the only form where the property
    // exists: the fixture's scripted map has no device behind it. The crash
    // is what makes this "durable" rather than "written" - a map that only
    // reached the page cache would be gone at the reopen.
    auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/64);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto store = storage::DevicePageStore::Open(*device.value(), kFirstUserPageId);
    ASSERT_TRUE(store.ok()) << store.status().message();
    storage::ExtentAllocator over_store(*store.value(), kFirstUserPageId);
    ASSERT_TRUE(store.value()->Sync().ok());  // a clean map, the shape at any refill

    sched::Scheduler core0(clock_, io0_);
    ASSERT_TRUE(core0.AttachTransport(&*transport_, 0).ok());
    ASSERT_TRUE(RegisterExtentGrantHandler(core0, *transport_, over_store, 64).ok());

    storage::LeasedIdSource lease;
    bool finished = false;
    core1_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kSystem,
        RequestExtentRefill(*transport_, lease, refill_, 1),
        [&](const Status&) { finished = true; }));
    for (int i = 0; i < 20 && !finished; ++i) {
        core0.RunOnce();
        core1_->RunOnce();
    }
    ASSERT_TRUE(finished);
    ASSERT_FALSE(lease.spent());
    const PageId granted = refill_.extent.first;

    // Nothing else flushes: the device loses every write that was not
    // synced, which is what separates a durable map from a written one.
    // The store stays alive - `over_store` points at it - and its in-memory
    // map is exactly what must not be believed here.
    device.value()->Crash();

    auto reopened = storage::DevicePageStore::Open(*device.value(), kFirstUserPageId);
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();
    EXPECT_TRUE(reopened.value()->IsAllocated(granted))
        << "the grant left core 0 before its run was durable, so a crash frees a run the "
           "peer may already have written";
    EXPECT_TRUE(reopened.value()->IsAllocated(granted + refill_.extent.count - 1));
}

TEST_F(ExtentLeaseServiceTest, TheRequestingCoreKeepsServingWhileItWaits) {
    // The point of doing this with a coroutine rather than a blocking call:
    // the core that is waiting for its lease is not occupied by the wait.
    storage::LeasedIdSource lease;
    bool finished = false;
    core1_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kSystem,
        RequestExtentRefill(*transport_, lease, refill_, 1),
        [&](const Status&) { finished = true; }));

    int other_work = 0;
    for (int i = 0; i < 4; ++i) {
        core1_->Submit(std::make_unique<sched::FunctionTask>(
            sched::SchedulingGroup::kForeground, [&] {
                ++other_work;
                return sched::PollResult::kDone;
            }));
    }

    // Core 1 only: the grant cannot arrive because core 0 never runs.
    for (int i = 0; i < 10; ++i) core1_->RunOnce();
    EXPECT_FALSE(finished);
    EXPECT_EQ(other_work, 4) << "the waiting refill held the core";

    Pump();
    EXPECT_TRUE(finished);
}

TEST_F(ExtentLeaseServiceTest, LowWaterFiresWhileTheLeaseCanStillAllocate) {
    // Why the refill is a background task: it has to be *asked for* while
    // there are still ids to hand out, because allocation itself cannot
    // await anything (extent_lease.hpp).
    storage::LeasedIdSource lease(storage::Extent{1000, 8});
    while (!lease.low_water()) ASSERT_TRUE(lease.Next().ok());
    EXPECT_GT(lease.remaining(), 0u) << "there is no room left to allocate during the refill";

    bool finished = false;
    core1_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kSystem,
        RequestExtentRefill(*transport_, lease, refill_, 1),
        [&](const Status&) { finished = true; }));

    // Allocation keeps working while the request is in flight.
    ASSERT_TRUE(lease.Next().ok());
    Pump();
    EXPECT_TRUE(finished);
    EXPECT_FALSE(lease.spent());
}

}  // namespace
}  // namespace kds::server
